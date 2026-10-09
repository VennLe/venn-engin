#version 450
// ============================================================
// post.frag —— 后处理合成（Tonemap）
//
// 输入：HDR 颜色（set 0）+ Bloom 图（set 1）
// 输出：交换链颜色附件（SRGB 格式 → 硬件自动做线性→sRGB 编码）
//
// 因为交换链是 SRGB 格式，这里必须输出**线性**颜色，绝不能
// 手工再做一次 gamma，否则会出现"亮部发灰、暗部发白"的双重校正。
//
// Tonemapping 把 HDR 的无界亮度压回 [0,1]：
//   ACES   —— 电影感，高光滚降柔和（默认）
//   Reinhard —— 简单，对比度低
//   Clamp  —— 直通，用来对照观察 HDR 截断
// ============================================================

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D hdrTex;
layout(set = 1, binding = 0) uniform sampler2D bloomTex;

// x = 曝光, y = bloom 强度, z = tonemap 模式(0/1/2), w = 暗角强度
layout(push_constant) uniform PC {
    vec4 params;
} pc;

// ACES filmic 近似（Narkowicz 2015）—— 便宜且观感接近正式 ACES
vec3 acesFilm(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

vec3 reinhard(vec3 x) { return x / (1.0 + x); }

void main() {
    vec3 hdr   = texture(hdrTex, vUv).rgb;
    vec3 bloom = texture(bloomTex, vUv).rgb;

    // 先叠 bloom，再统一曝光
    vec3 color = hdr + bloom * pc.params.y;
    color *= pc.params.x;

    int mode = int(pc.params.z + 0.5);
    if (mode == 0)      color = acesFilm(color);
    else if (mode == 1) color = reinhard(color);
    else                color = clamp(color, 0.0, 1.0);  // 直通（观察用）

    // 暗角：越靠画面边缘越暗，弱化边缘、聚焦中心
    vec2 d = vUv - 0.5;
    float vig = 1.0 - pc.params.w * dot(d, d) * 2.0;
    color *= clamp(vig, 0.0, 1.0);

    outColor = vec4(color, 1.0);
}
