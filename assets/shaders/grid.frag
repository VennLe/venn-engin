#version 450
// ============================================================
// grid.frag —— 地平面参考栅格（解析式，带抗锯齿）
//
// 思路：把世界坐标的 xz 平面当作"格线的符号距离场"。
//   fract(p / step + 0.5) - 0.5  → 到最近格线的距离（单位为格子数）
//   乘回 step                    → 世界单位下的距离
//   除以 fwidth(p)               → 像素单位下的距离
// 于是可以按像素做 1px 宽的抗锯齿线：不依赖 MSAA，走到天边也不会闪烁
// （线宽恒定是"解析法"相对"画一堆线段"的最大优势）。
//
// 三种图层（后画的覆盖先画的）：
//   最小格（1 m，暗）→ 主格（10 m，亮）→ 世界坐标轴（X 红 / Z 蓝）
// 另外两处淡出，避免摩尔纹：
//   · 最小格密到亚像素时（远处）按"一格占几个像素"淡出
//   · 离相机太远整体淡出（fade.xy 给范围）
// ============================================================

layout(location = 0) in vec3 vWorld;
layout(location = 1) in vec3 vCam;

layout(location = 0) out vec4 outColor;

// x = 半边长 y = 最小格 z = 主格 w = 平面高度
layout(push_constant) uniform GridPC {
    vec4 params;
    vec4 color;  // rgb = 线色, a = 总不透明度
    vec4 fade;   // x = 开始淡出距离 y = 完全消失距离 z = 线宽(px) w = 最小格强度
} pc;

// 到最近格线的距离（像素）。x / z 两个方向各算一次，取更近的那个 ——
// 这样每条线都只按自己的方向做抗锯齿，斜看时也不会变粗。
float lineDistPx(vec2 p, float step, vec2 fw) {
    const vec2 d = abs(fract(p / step + 0.5) - 0.5) * step;  // 世界单位
    return min(d.x / max(fw.x, 1e-8), d.y / max(fw.y, 1e-8)); // 像素单位
}

// 像素单位的距离 → 覆盖率（线宽 wpx，边缘 1px 过渡）
float coverage(float distPx, float wpx) {
    const float half_ = max(wpx * 0.5, 0.35);
    return 1.0 - smoothstep(half_, half_ + 1.0, distPx);
}

void main() {
    const vec2 p = vWorld.xz;
    const vec2 fw = fwidth(p);              // 1 像素对应多少米
    const float wpx = pc.fade.z;

    // ---- 三种线的覆盖率 ----
    const float minor = pc.params.y;
    const float major = pc.params.z;

    float aMinor = coverage(lineDistPx(p, minor, fw), wpx);
    float aMajor = coverage(lineDistPx(p, major, fw), wpx * 1.4);

    // 远处最小格会密到亚像素：按"一格占几个像素"淡出（否则一片摩尔纹）
    const float cellPx = minor / max(max(fw.x, fw.y), 1e-8);
    aMinor *= smoothstep(2.5, 8.0, cellPx);

    // 世界坐标轴：x = 0 → Z 轴（蓝），z = 0 → X 轴（红）。
    // 与视口 gizmo 的红/蓝保持同一套配色，一眼能对上方位。
    const float axZ = coverage(abs(p.x) / max(fw.x, 1e-8), wpx * 1.6);
    const float axX = coverage(abs(p.y) / max(fw.y, 1e-8), wpx * 1.6);

    // ---- 距离淡出 ----
    const float dist = length(vWorld - vCam);
    const float fade = 1.0 - smoothstep(pc.fade.x, max(pc.fade.y, pc.fade.x + 0.001), dist);
    if (fade <= 0.0) discard;

    // 谁最强就用谁的颜色（叠加只会让线发脏，取最大值更干净）
    vec3 col = pc.color.rgb;
    float m = max(aMinor * pc.fade.w, aMajor);
    if (axX > m) { m = axX; col = vec3(0.86, 0.26, 0.24); }
    if (axZ > m) { m = axZ; col = vec3(0.28, 0.46, 0.95); }

    const float alpha = m * fade * pc.color.a;
    if (alpha < 0.004) discard;

    outColor = vec4(col, alpha);
}
