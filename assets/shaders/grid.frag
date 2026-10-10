#version 450
// ============================================================
// grid.frag —— 地平面参考栅格（解析式，带抗锯齿）
//
// 思路：把世界坐标的 xy 平面当作"格线的符号距离场"（Z-up：地面 = z=0）。
//   fract(p / step + 0.5) - 0.5  → 到最近格线的距离（单位为格子数）
//   乘回 step                    → 世界单位下的距离
//   除以 fwidth(p)               → 像素单位下的距离
// 于是可以按像素做 1px 宽的抗锯齿线：不依赖 MSAA，走到天边也不会闪烁
// （线宽恒定是"解析法"相对"画一堆线段"的最大优势）。
//
// 三种图层（后画的覆盖先画的）：
//   最小格（1 m，暗）→ 主格（10 m，亮）→ 世界坐标轴（X 红 / Y 绿）
//
// ------------------------------------------------------------
// 2026-10-10 修：贴近地面平视时栅格"越远越淡、最后整片消失"
// ------------------------------------------------------------
// 两个原因都在这份着色器里，而且都是"屏幕像素密度"惹的祸：
//
// ① 最小格的亚像素淡出阈值太狠：一格只剩 2.5 px 就把整族线抹掉。
//    "一格占几像素" = 格距 / fwidth，是**随视角**变的 —— 相机贴到离地
//    几十厘米再平视，一格在屏幕上的尺寸掉得极快，于是 1 m 格在十几米
//    开外就整片消失（实测 z = 0.4 m 时 13 m 以外一条都不剩）。
//    → 阈值放到真正的亚像素区（≈1.5 px），并且先把线宽封顶（见 ②），
//      这样它淡出得又晚又平滑，而不是"啪"一下整族没了。
//
// ② 线宽是**像素**单位（1.4 px）却没有上限：格距掉到亚像素时，1.4 px 的
//    线画在 0.3 px 的格子上 → 覆盖率恒为 1 → 整片糊成一条亮带
//    （实测远处**整行**亮度被抬高 +55，肉眼就是"一片发白、看不到格子"）。
//    → 线宽按 kMaxLineDuty 封顶：一条线最多占一格宽度的 22%。格子越小线
//      越细、越淡，亮度自然收敛 —— 既不会糊，也不会糊完突然消失。
//
// 另外把"距离淡出"从**到相机的距离**改成**到栅格中心的距离**：栅格是
// 固定不动的一块地（±extent），它的样子不该跟着相机跑。按相机距离淡出
// 等于让一圈"没有栅格的环"随相机平移，飞行时非常显眼。
// ============================================================

layout(location = 0) in vec3 vWorld;

layout(location = 0) out vec4 outColor;

// x = 半边长 y = 最小格 z = 主格 w = 平面高度
layout(push_constant) uniform GridPC {
    vec4 params;
    vec4 color;  // rgb = 线色, a = 总不透明度
    vec4 fade;   // x = 边界淡出起点 y = 边界淡出终点（均为"距栅格中心"的米数）
                 // z = 线宽(px) w = 最小格强度
} pc;

// 一条线最多占一格的多少宽度（占空比上限）。0.22 是"看得清格子"和
// "远处不发白"之间的折中：再大远处就糊，再小近处的线会偏细。
const float kMaxLineDuty = 0.22;

// 一格在屏幕上占多少像素（取两个方向里更密的那个 —— 斜看时格子被压扁，
// 决定"能不能解析出格子"的是更密的那个方向）
float cellPixels(float step, vec2 fw) {
    return step / max(max(fw.x, fw.y), 1e-8);
}

// 到最近格线的距离（像素）。x / y 两个方向各算一次，取更近的那个 ——
// 这样每条线都只按自己的方向做抗锯齿，斜看时也不会变粗。
float lineDistPx(vec2 p, float step, vec2 fw) {
    const vec2 d = abs(fract(p / step + 0.5) - 0.5) * step;   // 世界单位
    return min(d.x / max(fw.x, 1e-8), d.y / max(fw.y, 1e-8)); // 像素单位
}

// 像素单位的距离 → 覆盖率（线宽 wpx，边缘 1px 过渡）
float coverage(float distPx, float wpx) {
    const float half_ = max(wpx * 0.5, 0.35);
    return 1.0 - smoothstep(half_, half_ + 1.0, distPx);
}

// 一整族格线的覆盖率，把"亚像素怎么办"包在里面：
//   · 线宽 = min(期望像素线宽, kMaxLineDuty × 一格像素数) → 格子越小线越细；
//   · 一格不足 ~1.6 px 时才真正淡出（那时细节已经解析不出来了）。
float gridCoverage(vec2 p, float step, vec2 fw, float baseWpx) {
    const float cellPx = cellPixels(step, fw);
    const float w = min(baseWpx, kMaxLineDuty * cellPx);
    return coverage(lineDistPx(p, step, fw), w) * smoothstep(0.7, 1.6, cellPx);
}

void main() {
    const vec2 p = vWorld.xy;
    const vec2 fw = fwidth(p);              // 1 像素对应多少米
    const float wpx = pc.fade.z;

    // ---- 三种线的覆盖率 ----
    const float minor = pc.params.y;
    const float major = pc.params.z;

    const float aMinor =
        gridCoverage(p, minor, fw, wpx) * pc.fade.w;   // 最小格再乘一层强度
    const float aMajor = gridCoverage(p, major, fw, wpx * 1.4);

    // 世界坐标轴：y = 0 → X 轴（红），x = 0 → Y 轴（绿）。
    // 与视口 gizmo 的红 / 绿保持同一套配色，一眼能对上方位。
    const float axX = coverage(abs(p.y) / max(fw.y, 1e-8), wpx * 1.6);
    const float axY = coverage(abs(p.x) / max(fw.x, 1e-8), wpx * 1.6);

    // ---- 边界淡出（相对栅格中心，**与相机无关**）----
    // 用切比雪夫距离 max(|x|, |y|)：栅格是块正方形（±extent），按它淡出
    // 正好在方块的边上收干净，也不会把四个角提前切掉。
    const float edge = max(abs(p.x), abs(p.y));
    const float fade =
        1.0 - smoothstep(pc.fade.x, max(pc.fade.y, pc.fade.x + 0.001), edge);
    if (fade <= 0.0) discard;

    // 谁最强就用谁的颜色（叠加只会让线发脏，取最大值更干净）
    vec3 col = pc.color.rgb;
    float m = max(aMinor, aMajor);
    if (axX > m) { m = axX; col = vec3(0.86, 0.26, 0.24); }
    if (axY > m) { m = axY; col = vec3(0.46, 0.82, 0.33); }

    const float alpha = m * fade * pc.color.a;
    if (alpha < 0.004) discard;

    outColor = vec4(col, alpha);
}
