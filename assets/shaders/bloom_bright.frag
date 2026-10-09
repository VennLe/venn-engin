#version 450
// ============================================================
// bloom_bright.frag —— Bloom 亮部提取（Bright Pass）
//
// 输入：HDR 颜色（线性辐射度）
// 输出：超过阈值的亮部（**保持线性、未曝光**），供后续模糊
//
// 用"软阈值"（soft knee）而不是硬截断：
//   亮度在 [threshold-knee, threshold+knee] 区间内平滑过渡，
//   硬截断会让 bloom 在阈值边界出现一圈生硬的轮廓。
// ============================================================

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D srcTex;

// x = 阈值, y = 过渡带宽(knee), z = 曝光（仅用于判定）, w = 未用
layout(push_constant) uniform PC {
    vec4 params;
} pc;

void main() {
    vec3 c = texture(srcTex, vUv).rgb;

    // 用"曝光后的亮度"做阈值判定，与最终 tonemap 的观感一致
    float lum = max(c.r, max(c.g, c.b)) * pc.params.z;

    float thr  = pc.params.x;
    float knee = max(pc.params.y, 1e-4);

    // 软阈值曲线：0 → thr-knee 完全剔除；thr+knee 以上全额保留
    float soft = clamp(lum - thr + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 1e-5);

    // contrib 是"锐利部分"与"软过渡部分"的较大者，再归一化回原亮度
    float contrib = max(soft, lum - thr) / max(lum, 1e-5);

    outColor = vec4(c * contrib, 1.0);   // 输出未曝光的线性亮部
}
