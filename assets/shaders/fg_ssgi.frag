#version 450
// ============================================================
// fg_ssgi.frag —— 屏幕空间全局光照（SSGI-lite，加色注入版）
//
// 原理：以表面点为球心，在法线半球里撒样本点；样本点**没有被
// 挡住**（投影回屏幕后，深度图里存的表面比样本点更远）时，把
// 该处的已渲染场景色按余弦权重收进来 —— 也就是"邻居表面朝我
// 这里弹了多少光"。红色物体旁边的地面会染上红色（color
// bleeding），这正是 SSGI 的标志性效果。
//
// 为什么是 lite：真正的 SSGI 要把间接光按 albedo 调制、并区分
// 漫反射/镜面；这里拿不到 albedo（全屏 pass 无材质信息），所以
// 把收到的辐射度直接加进 HDR 颜色 —— 亮度/色彩倾向是对的，
// 能量守恒不严格。每像素 12 个样本 + 逐像素噪声旋转，残余噪声
// 交给人眼对"间接光"的宽容度（不开模糊 pass，代价/收益权衡）。
//
// 与 SSAO 的分工：SSAO 在**前向之前**跑（结果乘进环境光），
// SSGI 在**前向之后**跑（场景色已存在才能采样）。两者读同一张
// MSAA 深度图，互不依赖。
// ============================================================

#include "space.glsl"

layout(set = 0, binding = 0) uniform sampler2D sceneColor;
#if VENN_MSAA_DEPTH
layout(set = 1, binding = 0) uniform sampler2DMS ssgiDepthTex;
#else
layout(set = 1, binding = 0) uniform sampler2D ssgiDepthTex;
#endif

layout(push_constant) uniform PC {
    mat4 invProj;
    vec4 projCoefs;
    // x=半径(米) y=强度 z=表面偏置(米) w=未用
    vec4 params;
} pc;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

const int kSamples = 12;
const float kGoldenAngle = 2.39996323;

void main() {
    vec3 col = texture(sceneColor, vUv).rgb;

    const float d = vennSceneDepth(ssgiDepthTex, vUv);
    if (d >= 1.0) {
        outColor = vec4(col, 1.0);
        return;
    }

    const vec3 P = vennViewPosFromDepth(vUv, d, pc.invProj);
    const vec3 N = vennNormalFromViewPos(P);
    // NaN 与任何比较都为 false —— 一个判断同时挡掉 NaN 与退化法线
    const float nn = dot(N, N);
    if (!(nn > 0.25) || !(nn < 4.0)) {
        outColor = vec4(col, 1.0);
        return;
    }

    vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    const vec3 T = normalize(cross(up, N));
    const vec3 B = cross(N, T);

    const float noise = vennIgNoise(gl_FragCoord.xy);
    const float radius = max(pc.params.x, 1e-3);
    const float surfBias = pc.params.z;

    vec3 gi = vec3(0.0);
    float wsum = 0.0;
    for (int i = 0; i < kSamples; ++i) {
        // 余弦加权半球：cosθ 按平方根分布采样
        const float ang = noise * 6.2831853 + float(i) * kGoldenAngle;
        const float rr = sqrt((float(i) + 0.5) / float(kSamples));
        const vec3 L = normalize(T * cos(ang) + B * sin(ang) +
                                 N * max(rr, 0.15));
        const float ndotl = max(dot(N, L), 0.0);
        if (ndotl <= 0.0) continue;

        // 样本点：抬起一个小的表面偏置，避免与自身深度打架
        const vec3 sp = P + L * radius + N * surfBias;

        const vec2 suv = vennProjectViewPos(sp, pc.projCoefs);
        if (any(lessThan(suv, vec2(0.0))) ||
            any(greaterThan(suv, vec2(1.0)))) {
            continue;
        }

        const float d2 = vennSceneDepth(ssgiDepthTex, suv);
        if (d2 >= 1.0) continue;  // 天空方向不收（环境光已由 IBL 负责）

        const vec3 sq = vennViewPosFromDepth(suv, d2, pc.invProj);
        // 可见 = 存档表面在样本点**后面**（sq.z < sp.z，更远）→
        // 该方向上确实"看得见"那个表面，它弹的光才进得来
        if (sq.z <= sp.z) {
            gi += texture(sceneColor, suv).rgb * ndotl;
            wsum += ndotl;
        }
    }

    if (wsum > 1e-4) {
        col += gi / wsum * pc.params.y;
    }
    outColor = vec4(col, 1.0);
}
