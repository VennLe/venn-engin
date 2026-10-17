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
//
// ------------------------------------------------------------
// 2026-10-17 晚修：栅格"虚化 / 一片一片看不见" —— 共面深度冲突
// ------------------------------------------------------------
// 症状随视角变化：地板范围内时有时无，远看像"糊掉"，近看是整块缺线。
// 根因不在本项目任何一行格线代码里，而在**深度**：栅格与地板都在 z = 0，
// 只在顶点里减了 1e-6 NDC 当偏移，而那个四边形横跨 ±2000 m，光栅化插值
// 误差比 1e-6 还大（详见 kDepthBias 的注释）。
// 修法：片元里用像素自己的世界位置重算 gl_FragDepth 再减 1e-5。
// ============================================================

layout(location = 0) in vec3 vWorld;

layout(location = 0) out vec4 outColor;

// 与 grid.vert **逐字段一致**（同一个 set 0 / binding 0，两个阶段都要用它
// 重算片元深度）；见下面 kDepthBias 的说明。
layout(set = 0, binding = 0) uniform FrameUBO {
    mat4 view;
    mat4 proj;
    vec4 lightDir;
    vec4 lightColor;
    vec4 camPos;
    mat4 lightSpace;
    vec4 cameraParams;
    vec4 clusterDims;
    vec4 screenSize;
    vec4 lightParams;
    mat4 invViewProj;
} ubo;

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

// 一格要占多少像素才允许"完整"出现；不到 kMinCellPx 就整族收掉。
//
// ⚠⚠ 2026-10-17（俯视抖动修复）：这两个数原来写的是 0.9 / 2.0，那是
//   按"能不能解析出一条线"定的。但实测证明**能解析 ≠ 不闪**：
//
//     俯视 120 m 时 1 m 小格只占 ~4.3 px（视口 840×590、FOV 60°），
//     线宽 1.4 px、间距 4.3 px —— 这是一张**周期 4 像素**的高频图案。
//     把相机平移 0.3 m（≈1.3 px），整幅画面的 45% 像素都变了
//     （实测 diff: mean=24.5、n>18=221194），视觉上就是"地面在抖"。
//
//   根子是"点采样 + 1 px 宽的覆盖率过渡"在周期只有几个像素时必然走样：
//   像素落在线上就是 1、落在缝里就是 0，而那 0/1 的相位随相机平移
//   以格子为周期来回跳。TAA 帮不上忙（抖动相位被平均后会变成灰噪点），
//   因为这不是"采样点没对齐"，是**图案本身比奈奎斯特频率还高**。
//
//   修法（与 UE5 视口栅格同一套）：把"一格占多少像素"当成 mip 级别，
//   密度不够就**整族淡出**，只保留更粗的那一级。阈值取 5 → 12 px：
//     · 12 px 以上完整显示（此时 1.4 px 的线是清晰的）；
//     · 5 px 以下完全不画（4.3 px 的高频图案正是要收掉的）；
//     · 中间线性过渡，过渡带里图案已经够粗，不会再出现摩尔纹。
//   代价是中距离的小格消失得早一点 —— 但那时它本来也只是一层噪点。
const float kMinCellPx = 5.0;
const float kFullCellPx = 12.0;

// ------------------------------------------------------------
// 共面深度偏移（2026-10-17 晚，"虚化 / 一片一片看不见"的修复）
// ------------------------------------------------------------
// 栅格是**贴在地板上的一层叠加**（两者都在 z = 0），必然要吃深度测试
// 才能被墙、箱子正确遮挡。而两者深度完全相同 → 谁先谁后由浮点误差决定。
//
// 原来只在**顶点着色器**里把 clip.z 减 1e-6（NDC 上恒定），实测不够：
// 四边形横跨 ±2000 m，光栅化器要在 4 km 跨度上插值 clip.z，插值误差
// 与 1e-6 同量级 → 栅格在地板范围内**时有时无**（远看是"虚化"，
// 近看是"缺一片"）。把偏移放大到 1e-3 能盖住，但等于把栅格抬高几米，
// 旁边的物体会穿模。
//
// 现在的做法：**不用插值来的深度，用当前像素自己的世界位置重算**。
//   · vWorld 是在 4 km 上做的透视校正插值，数值上仍是"这一个像素对应的
//     地面点"，误差只有毫米级（相对 2e-7 左右）；
//   · 在片元里重新做一次 proj*view，就得到这个点的**精确** NDC 深度；
//   · 再减一个恒定的 1e-5，余量已经是误差的 ~100 倍。
// 代价：写了 gl_FragDepth 就关掉了这个管线的 early-Z。栅格本来就排在
// 所有不透明几何之后、只看深度不写深度，这点开销可以忽略。
//
// 为什么 1e-5 不会"穿模"：它是 **NDC** 上的偏移，换算成世界距离是
// 4 cm@20 m、4 m@200 m —— 那是深度上的"更靠前一点"，视觉上看不出位移，
// 只有贴着地面 4 cm 以内的物体边缘可能被盖上一条线，不会有可见影响。
const float kDepthBias = 1e-5;

// 一整族格线的覆盖率，把"亚像素怎么办"包在里面：
//   · 线宽 = min(期望像素线宽, kMaxLineDuty × 一格像素数) → 格子越小线越细；
//   · 一格不足 kMinCellPx 时整族淡出（见上面的长注释）。
float gridCoverage(vec2 p, float step, vec2 fw, float baseWpx) {
    const float cellPx = cellPixels(step, fw);
    const float lod = smoothstep(kMinCellPx, kFullCellPx, cellPx);
    if (lod <= 0.0) return 0.0;      // 整族在屏幕上已经太密 → 连 fwidth 都不必算
    const float w = min(baseWpx, kMaxLineDuty * cellPx);
    return coverage(lineDistPx(p, step, fw), w) * lod;
}

void main() {
    const vec2 p = vWorld.xy;
    const vec2 fw = fwidth(p);              // 1 像素对应多少米
    const float wpx = pc.fade.z;

    // ---- 四种线的覆盖率 ----
    const float minor = pc.params.y;
    const float major = pc.params.z;
    // 第三级：主格的 10 倍（默认 10 m → 100 m）。加它是因为 LOD 淡出
    // 是"逐级收掉"的：相机升到 400 m 以上时 10 m 格也掉到 kMinCellPx 以下，
    // 若没有更粗的一级，整块地面会变成**一片空白**（没有任何参考线）。
    // UE5 的视口栅格在每个高度档都至少留一层能看清的格线，这里对上它。
    const float superStep = major * 10.0;

    const float aMinor =
        gridCoverage(p, minor, fw, wpx) * pc.fade.w;   // 最小格再乘一层强度
    const float aMajor = gridCoverage(p, major, fw, wpx * 1.4);
    const float aSuper = gridCoverage(p, superStep, fw, wpx * 1.9) * 0.9;

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
    float m = max(aMinor, max(aMajor, aSuper));
    if (axX > m) { m = axX; col = vec3(0.86, 0.26, 0.24); }
    if (axY > m) { m = axY; col = vec3(0.46, 0.82, 0.33); }

    const float alpha = m * fade * pc.color.a;
    if (alpha < 0.004) discard;

    // 片元级精确深度（见 kDepthBias 的长注释）。放在所有 discard 之后，
    // 保证"活下来"的每个片元都写了一次。
    const vec4 clip = ubo.proj * ubo.view * vec4(vWorld, 1.0);
    gl_FragDepth = clamp(clip.z / clip.w - kDepthBias, 0.0, 1.0);

    outColor = vec4(col, alpha);
}
