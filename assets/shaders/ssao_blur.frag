#version 450
// ============================================================
// ssao_blur.frag —— SSAO 去噪（深度感知 3×3）
//
// SSAO 的采样核带逐像素随机旋转（见 ssao.frag），输出天然是
// 高频噪声图。直接乘进环境光会把噪声带进着色 —— 这里做一遍
// 3×3 的**深度感知**模糊：按视图深度差给邻域权重，深度差大
// （跨越轮廓）的邻居权重趋 0，避免把 AO 糊到物体轮廓外形成
// 光晕（halo）—— 那是无深度感知的朴素模糊最显眼的毛病。
// ============================================================

#include "space.glsl"

layout(set = 0, binding = 0) uniform sampler2D ssaoTex;
// 深度图在 set 1（与 SSAOPass::buildDescriptorSets 的
// m_blurRawSets / m_blurDepthSets 两个独立 set 一一对应；
// createPipelines 给 blur 管线推了两个 setLayout）。
#if VENN_MSAA_DEPTH
layout(set = 1, binding = 0) uniform sampler2DMS blurDepthTex;
#else
layout(set = 1, binding = 0) uniform sampler2D blurDepthTex;
#endif

layout(push_constant) uniform PC {
    // x=1/宽 y=1/高 z=深度衰减(1/米) w=未用
    vec4 params;
    // x=zNear y=zFar（深度 → 视深度线性化用）
    vec4 camera;
} pc;

layout(location = 0) in vec2 vUv;
layout(location = 0) out float outAO;

void main() {
    const float aoC = texture(ssaoTex, vUv).r;
    const float dC = vennSceneDepth(blurDepthTex, vUv);
    const float zC = vennViewDepthFromNdc(dC, pc.camera.x, pc.camera.y);

    const vec2 texel = vec2(pc.params.x, pc.params.y);
    const float falloff = max(pc.params.z, 1e-4);

    float sum = aoC * 2.0;   // 中心权重 2
    float wsum = 2.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            if (x == 0 && y == 0) continue;
            const vec2 uv = vUv + vec2(float(x), float(y)) * texel;
            const float ao = texture(ssaoTex, uv).r;
            const float d = vennSceneDepth(blurDepthTex, uv);
            const float z = vennViewDepthFromNdc(d, pc.camera.x, pc.camera.y);
            // 深度差大 → 大概率跨了轮廓 → 权重归零
            const float w = exp(-abs(z - zC) * falloff);
            sum += ao * w;
            wsum += w;
        }
    }
    outAO = clamp(sum / wsum, 0.0, 1.0);
}
