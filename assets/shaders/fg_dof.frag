#version 450
// ============================================================
// fg_dof.frag —— 景深（Depth of Field）
//
// 用深度算 CoC（弥散圆半径），按 CoC 做"黄金角螺旋"圆盘采样。
// 单 Pass 的 scatter-as-gather：
//   · 每个采样点自带 CoC，只有**也足够虚**的样本才参与加权 ——
//     否则清晰的前景会被抹到背景上（经典 halo）；
//   · 若样本比中心更靠近相机且更清晰，再压一层权重（近处清晰物
//     不该吸收远处的模糊）。
//
// 对焦距离附近（radiusPx < 0.75）像素**完全不碰** —— 逐像素不变，
// 这既是"对焦点清晰"的物理预期，也让验证脚本能断言"关/开在对焦区
// 完全一致"。
//
// 深度读前向写好的 MSAA 深度图（space.glsl 双形态），场景色是 HDR 图。
// ============================================================

#include "space.glsl"

layout(set = 0, binding = 0) uniform sampler2D sceneColor;
#if VENN_MSAA_DEPTH
layout(set = 1, binding = 0) uniform sampler2DMS dofDepthTex;
#else
layout(set = 1, binding = 0) uniform sampler2D dofDepthTex;
#endif

layout(push_constant) uniform PC {
    vec4 cam;     // x=zn y=zf z=对焦距离(米) w=光圈
    vec4 params;  // x=最大半径(像素) y=texelX z=texelY w=未用
} pc;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

const int kSamples = 16;
const float kGoldenAngle = 2.39996323;

// 某像素的 CoC 半径（像素）
float cocRadius(vec2 uv) {
    const float z = vennViewDepthFromNdc(vennSceneDepth(dofDepthTex, uv),
                                         pc.cam.x, pc.cam.y);
    const float focus = max(pc.cam.z, 0.05);
    const float coc = clamp(abs(z - focus) / focus * pc.cam.w, 0.0, 1.0);
    return coc * pc.params.x;
}

void main() {
    const float radiusPx = cocRadius(vUv);
    const vec3 center = texture(sceneColor, vUv).rgb;

    // 对焦区：完全不碰（逐像素不变）
    if (radiusPx < 0.75) {
        outColor = vec4(center, 1.0);
        return;
    }

    const float zc = vennViewDepthFromNdc(vennSceneDepth(dofDepthTex, vUv),
                                          pc.cam.x, pc.cam.y);

    vec3 acc = center;
    float wsum = 1.0;
    for (int i = 0; i < kSamples; ++i) {
        const float fi = float(i);
        // 黄金角螺旋：半径按 sqrt 分布（面积均匀），角度按黄金角散开
        const float r = radiusPx * sqrt((fi + 0.5) / float(kSamples));
        const float a = fi * kGoldenAngle;
        const vec2 off = vec2(cos(a), sin(a)) * r;
        const vec2 uv = clamp(vUv + off * pc.params.yz, vec2(0.0), vec2(1.0));

        const float scr = cocRadius(uv);
        // 样本自己要够虚才参与（防 halo）
        float w = smoothstep(0.0, radiusPx, scr);
        // 更近且清晰的样本再压一层
        const float sz = vennViewDepthFromNdc(vennSceneDepth(dofDepthTex, uv),
                                              pc.cam.x, pc.cam.y);
        if (sz < zc) w *= step(radiusPx * 0.5, scr);

        acc += texture(sceneColor, uv).rgb * w;
        wsum += w;
    }

    outColor = vec4(acc / wsum, 1.0);
}
