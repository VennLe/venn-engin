#version 450
// ============================================================
// fg_godrays.frag —— 屏幕空间体积光（径向 God rays）
//
// 原理：太阳在屏幕上是一个点（方向光投影到远处）。从像素向
// 太阳屏幕坐标连一条线，沿途采样**深度图**：采样点落在天空
// （深度 = 1）说明这段光路没有被几何挡住 → 累计亮度。遮挡越
// 密、衰减越大，画面上就是从遮挡体缝隙里射出来的光柱。
//
// 这是后处理近似（不是光线步进的真体积散射）：光的"体积感"
// 来自屏幕空间径向累积，相机对着太阳转开时立刻消失 —— 所以
// CPU 侧在太阳出屏/背对相机时把强度压到 0，保证"开/关"两条
// 路径逐像素一致（验证脚本靠这个做严格断言）。
//
// 深度读的是**前向 Pass 写完的 MSAA 深度图**（finalLayout 已转
// DEPTH_READ_ONLY）。场景色是前向 resolve 出来的 HDR 图。
// ============================================================

#include "space.glsl"

layout(set = 0, binding = 0) uniform sampler2D sceneColor;
// 深度图：MSAA 构建时是 sampler2DMS，单采样时是 sampler2D
#if VENN_MSAA_DEPTH
layout(set = 1, binding = 0) uniform sampler2DMS godraysDepthTex;
#else
layout(set = 1, binding = 0) uniform sampler2D godraysDepthTex;
#endif

layout(push_constant) uniform PC {
    vec4 sun;         // xy=太阳屏幕 uv  z=太阳是否在屏(0/1)  w=未用
    // x=强度 y=每步衰减(0.9~0.99) z=步进密度 w=未用
    vec4 params;
    vec4 sunColor;    // rgb=光色×强度
} pc;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

const int kSteps = 48;

float occlusion(vec2 uv) {
    const float d = vennSceneDepth(godraysDepthTex, uv);
    // 深度 = 1 → 天空 → 光路无遮挡
    return d >= 1.0 ? 1.0 : 0.0;
}

void main() {
    vec3 col = texture(sceneColor, vUv).rgb;

    if (pc.sun.z > 0.5) {
        const vec2 delta = (pc.sun.xy - vUv) / float(kSteps) * pc.params.z;
        float illum = 0.0;
        float w = 1.0;
        vec2 uv = vUv;
        for (int i = 0; i < kSteps; ++i) {
            uv += delta;
            // 走出屏幕就停：太阳那侧的累积已经足够代表光柱强度
            if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
                break;
            }
            illum += occlusion(uv) * w;
            w *= pc.params.y;
        }
        // 从像素指向太阳的方向越正对，累计越密 —— 归一化一次
        col += pc.sunColor.rgb * illum *
               (pc.params.x / float(kSteps));
    }

    outColor = vec4(col, 1.0);
}
