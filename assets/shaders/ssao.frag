#version 450
// ============================================================
// ssao.frag —— 屏幕空间环境光遮蔽（SSAO）
//
// 在深度预通道之后、前向 Pass 之前跑一次全屏：
//   读 MSAA 深度图 → 重建视图空间位置与（逐图元平面）法线 →
//   在以表面点为球心、法线朝向的半球里撒 16 个样本点 →
//   把样本点投回屏幕，与深度图里存的表面深度比较 →
//   被更近的表面挡住的样本计入遮蔽。
//
// 输出 R8 遮蔽图（1 = 无遮蔽），再经 ssao_blur.frag 去噪后由
// pbr.frag（set 0 binding 14）采出来乘进**环境光**项 —— 只压
// ambient/IBL，不碰直接光，这是 AO 与"把整张图调暗"的区别。
//
// 采样核设计：
//   · 黄金角螺旋分布 + vennIgNoise 逐像素旋转 —— 同一采样数下
//     条带最不明显；残余高频噪声交给后面的深度感知模糊。
//   · 半径按世界单位（米）给定，近处细节处半径覆盖约几个像素，
//     远处自动收窄（投影后），不需要按距离缩放参数。
//   · range check：遮挡体离样本点太远就不算（否则紧贴地面的
//     像素会把"远处的墙"也算进遮蔽，整个画面发灰）。
// ============================================================

#include "space.glsl"

// 深度图：MSAA 构建时是 sampler2DMS，单采样时是 sampler2D ——
// 两份 .spv 由同一份源码按 VENN_MSAA_DEPTH 编出（见 space.glsl）。
#if VENN_MSAA_DEPTH
layout(set = 0, binding = 0) uniform sampler2DMS ssaoDepthTex;
#else
layout(set = 0, binding = 0) uniform sampler2D ssaoDepthTex;
#endif

layout(push_constant) uniform PC {
    mat4 invProj;    // 视图空间重建用
    vec4 projCoefs;  // x=P00 y=P11 z=P22 w=P23 —— 样本点投影回屏幕用
    // x=半径(米) y=强度 z=bias(米) w=未用
    vec4 params;
} pc;

layout(location = 0) in vec2 vUv;
layout(location = 0) out float outAO;

const int kSamples = 16;
const float kGoldenAngle = 2.39996323;

void main() {
    const float d = vennSceneDepth(ssaoDepthTex, vUv);
    // 天空（深度 = 1）：没有表面，谈不上遮蔽
    if (d >= 1.0) {
        outAO = 1.0;
        return;
    }

    const vec3 P = vennViewPosFromDepth(vUv, d, pc.invProj);
    const vec3 N = vennNormalFromViewPos(P);

    // 轮廓处的导数叉乘会退化（长度趋 0 或方向不定）→ 不做遮蔽，
    // 淡出成 1，避免物体边缘出现一圈假黑边。
    // （NaN 与任何比较都为 false —— 一个判断同时挡掉 NaN 与退化法线。）
    const float nn = dot(N, N);
    if (!(nn > 0.25) || !(nn < 4.0)) {
        outAO = 1.0;
        return;
    }

    // 法线的切线基（任取一个不平行于 N 的轴）
    vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    const vec3 T = normalize(cross(up, N));
    const vec3 B = cross(N, T);

    const float noise = vennIgNoise(gl_FragCoord.xy);
    const float radius = max(pc.params.x, 1e-3);
    const float bias = pc.params.z;

    float occlusion = 0.0;
    for (int i = 0; i < kSamples; ++i) {
        // 黄金角螺旋：角度均匀转、半径按 sqrt 均匀铺，逐像素偏转
        const float ang = noise * 6.2831853 + float(i) * kGoldenAngle;
        const float rr = sqrt((float(i) + 0.5) / float(kSamples));
        const vec3 dir = normalize(T * (cos(ang)) + B * (sin(ang)) +
                                   N * 1.2);  // 半球偏置：压向法线一侧
        // 样本点：半球内、离表面越近权重越高（cos 加权的近似）
        const vec3 sp = P + dir * radius * (0.15 + 0.85 * rr);

        const vec2 suv = vennProjectViewPos(sp, pc.projCoefs);
        // 样本点投出屏幕 → 那里没有遮挡体
        if (any(lessThan(suv, vec2(0.0))) || any(greaterThan(suv, vec2(1.0)))) {
            continue;
        }

        const float d2 = vennSceneDepth(ssaoDepthTex, suv);
        if (d2 >= 1.0) continue;  // 投到天空：没有遮挡体

        const vec3 sq = vennViewPosFromDepth(suv, d2, pc.invProj);
        // 视图空间 z 为负、越远越小。遮挡 = 存档表面比样本点**更靠近相机**
        // （sq.z > sp.z）且没有远超采样半径（range check）
        const float dz = sq.z - sp.z;
        if (dz > bias) {
            const float range = smoothstep(0.0, 1.0, radius / max(abs(dz), 1e-4));
            occlusion += range;
        }
    }

    float ao = 1.0 - pc.params.y * occlusion / float(kSamples);
    outAO = clamp(ao, 0.0, 1.0);
}
