#version 450
// ============================================================
// ui2.frag —— 游戏 UI 的片元着色器（U2）
//
// ---- 颜色空间：这里必须做一次 srgbToLinear ----
// 交换链优先选 B8G8R8A8_SRGB（见 rhi/Swapchain.cpp）。sRGB 附件意味着
// **硬件在写入时自动做 linear → sRGB 编码**。所以：
//   设计色 #808080（0.5，是 sRGB 值）→ shader 里先 pow 回线性 0.2158
//   → 硬件再编码回去 → 屏幕上正好 128。
// 如果漏掉这次反编码（直接把 0.5 当线性输出），硬件编码成 0.7354 →
// 屏幕读到 **188**，整块 UI 会"亮一大截"。
// 验收就是按实测值钉的：128 是对的，188 说明漏了这一步。
//
// ---- 混合 ----
// 非预乘：SRC_ALPHA / ONE_MINUS_SRC_ALPHA。sRGB 附件的混合由硬件在
// **线性空间**完成（dst 自动解码、src 按 write 值直接参与），所以这里
// 输出线性色 + alpha 才是正确的；预乘会在 sRGB 下算出脏边。
//
// ---- 图元 ----
//   mode 0 纯色矩形       不做形状处理
//   mode 1 贴图           alpha 取自贴图
//   mode 2 SDF 字形       距离 → smoothstep（宽度随字号缩放）
//   mode 3 SDF 圆角矩形   sdRoundBox，1px 抗锯齿带
// ============================================================

layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec2 vHalf;
layout(location = 3) in float vRadius;
layout(location = 4) in float vMode;
layout(location = 5) in vec2 vLocal;

// 唯一的一个 set / 一个 binding：SDF 图集（R8）或图标图集（RGBA8）。
// ⚠ 图集是**数据不是颜色**，描述符按 UNORM 采样，不能挂 sRGB 视图 ——
//   否则距离值会被当颜色解码，字形边缘全部错位。
layout(set = 0, binding = 0) uniform sampler2D uAtlas;

layout(location = 0) out vec4 oColor;

// 精确的 sRGB → 线性（分段函数，不是 pow 2.2 近似）。
// 用近似会让 0.5 → 0.2176 而不是 0.2158，屏幕上差 1~2 级；验收按 ±2 卡，
// 近似也能过，但既然一行就能写精确的，就没必要留这个含糊。
vec3 srgbToLinear(vec3 c) {
    return mix(c / 12.92,
               pow((c + 0.055) / 1.055, vec3(2.4)),
               step(vec3(0.04045), c));
}

// 圆角矩形有符号距离（负数 = 在内部）。半径 0 时退化成普通矩形。
float sdRoundBox(vec2 p, vec2 b, float r) {
    r = clamp(r, 0.0, min(b.x, b.y));
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

void main() {
    float cov = 1.0;
    vec3 tint = vColor.rgb;

    if (vMode > 2.5) {
        // ---- SDF 圆角矩形 / 纯色几何 ----
        // 1.5px 的过渡带：太窄在非整数缩放下会闪，太宽会"发虚"。
        float d = sdRoundBox(vLocal, vHalf, vRadius);
        cov = 1.0 - smoothstep(-0.75, 0.75, d);
    } else if (vMode > 1.5) {
        // ---- SDF 字形 ----
        // 图集里存的是距离（0..1，0.5 = 轮廓）。平滑宽度 vHalf.x 由 CPU
        // 按 **字号 / 图集字号** 算好传进来 —— 这正是"SDF 任意字号锐利"
        // 的实现点：字号变大时过渡带同步变窄，边缘不会被拉成渐变。
        float d = texture(uAtlas, vUv).r;
        float w = max(vHalf.x, 1e-4);
        cov = smoothstep(0.5 - w, 0.5 + w, d);
    } else if (vMode > 0.5) {
        // ---- 贴图（图标）----
        vec4 t = texture(uAtlas, vUv);
        tint *= t.rgb;
        cov = t.a;
    }

    float a = vColor.a * cov;
    // 全透明片元直接丢弃：省混合带宽，也避免 NaN 参与混合。
    if (a <= 0.002) discard;

    oColor = vec4(srgbToLinear(tint), a);
}
