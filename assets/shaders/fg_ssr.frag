#version 450
// ============================================================
// fg_ssr.frag —— 屏幕空间反射（SSR，深度步进版）
//
// 原理：从表面点 P 沿反射方向 R 在**视图空间**步进，每一步把
// 点投回屏幕，与深度图里存的表面深度比较：步进点跑到了表面
// **后面**（视深度更大）且没穿深太多 → 命中，取命中处已渲染
// 的场景色作为反射色。
//
// 这是 v1 的折衷（都有明确的改进方向，先立起来）：
//   · 线性步进 24 步，无二分细化 → 反射边缘可能有轻微错位；
//   · 法线用深度导数重建（vennNormalFromViewPos，逐图元平面），
//     曲面上的反射方向会偏"平"；
//   · 不区分材质粗糙度（全屏 pass 拿不到 push 材质参数），
//     用菲涅尔 + 全局强度控制 —— 粗糙表面也会有一层"清漆"式
//     反射。物理上不严格，但视觉上立得住。
//
// 命中不了（走出屏幕 / 步进用尽）就返回原色 —— 反射只在
// "屏幕里真的有东西可以照出来"的地方出现，这正是 SSR 的语义。
// ============================================================

#include "space.glsl"

layout(set = 0, binding = 0) uniform sampler2D sceneColor;
#if VENN_MSAA_DEPTH
layout(set = 1, binding = 0) uniform sampler2DMS ssrDepthTex;
#else
layout(set = 1, binding = 0) uniform sampler2D ssrDepthTex;
#endif

layout(push_constant) uniform PC {
    mat4 invProj;    // 深度 → 视图空间
    vec4 projCoefs;  // 步进点 → 屏幕（见 space.glsl）
    // x=强度 y=厚度容差(米) z=步长(米) w=最大距离(米)
    vec4 params;
    // x=zNear y=zFar —— 深度线性化（vennViewDepthFromNdc）必需，
    // 与 cluster_build.comp 用同一组值，不能拍常数
    vec4 camera;
} pc;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

const int kSteps = 24;

void main() {
    vec3 col = texture(sceneColor, vUv).rgb;

    const float d = vennSceneDepth(ssrDepthTex, vUv);
    if (d >= 1.0) {          // 天空像素：没有表面可反射
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

    const vec3 V = normalize(-P);
    const vec3 R = reflect(-V, N);

    // 反射方向朝相机背后走：反射内容不可能在屏幕里，直接放弃。
    // （镜面朝向相机时 R 指向 -Z 深处；朝天顶时 R.z > 0。）
    if (R.z > 0.05) {
        outColor = vec4(col, 1.0);
        return;
    }

    // 菲涅尔权重（F0 = 0.04 电介质近似）：掠射角反射最强
    const float ndotv = max(dot(N, V), 0.0);
    const float fres = 0.04 + 0.96 * pow(1.0 - ndotv, 5.0);

    const float thickness = max(pc.params.y, 1e-3);
    const float stepLen = max(pc.params.z, 1e-3);
    const float maxDist = max(pc.params.w, stepLen);

    vec3 refl = vec3(0.0);
    float hitMask = 0.0;
    vec2 hitUv = vUv;

    // 起点抬离表面一点，避免第 0 步就和自身深度"命中"
    float t = 0.05;
    for (int i = 0; i < kSteps; ++i) {
        const vec3 q = P + R * t;
        const vec2 uvq = vennProjectViewPos(q, pc.projCoefs);
        if (any(lessThan(uvq, vec2(0.0))) ||
            any(greaterThan(uvq, vec2(1.0)))) {
            break;  // 走出屏幕 → miss
        }

        const float dq = vennSceneDepth(ssrDepthTex, uvq);
        if (dq < 1.0) {
            const float zq = vennViewDepthFromNdc(dq, pc.camera.x,
                                                  pc.camera.y);
            const float qd = -q.z;  // 步进点的视深度（正值）
            if (qd > zq && qd - zq < thickness) {
                refl = texture(sceneColor, uvq).rgb;
                hitMask = 1.0;
                hitUv = uvq;
                break;
            }
        }
        t += stepLen * (1.0 + float(i) * 0.15);  // 步长渐增：近处细远处粗
        if (t > maxDist) break;
    }

    // 命中点贴屏幕边缘时反射内容本就不可靠（采样越界被钳制），
    // 淡出掉，避免边缘一条亮带。
    if (hitMask > 0.0) {
        const vec2 e = min(hitUv, 1.0 - hitUv);
        const float edge = smoothstep(0.0, 0.05, min(e.x, e.y));
        hitMask *= edge;
    }

    col += refl * fres * pc.params.x * hitMask;
    outColor = vec4(col, 1.0);
}
