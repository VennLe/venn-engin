#version 450
// ============================================================
// fg_taa.frag —— 时域抗锯齿（TAA，R1-1）
//
// 一条链的**头**：前向 resolve 出来的 HDR 是"本帧抖动了子像素"的
// 单张采样，TAA 把这一帧和历史帧混合，等价于对每个像素做了多次
// 亚像素重采样 —— 几何边缘收敛到接近超采样的平滑度。
//
// 三步（全部只用已有设施）：
//
//   1) 重投影：从深度重建**视图空间**点，乘 clipFromView
//      （= prevViewProj × inverse(view)，与运动模糊共用同一条链路）
//      得到上一帧屏幕 uv。相机静止时它精确等于当前 uv → 速度 0。
//   2) 历史钳制：3×3 邻域在 **YCoCg** 空间求均值/方差，把历史色夹到
//      mean ± k·sigma。这是"方差钳制"（Salvi 2015）——比裸 AABB 更紧，
//      能压住鬼影（物体移走后的残影）而保留真实细节。
//   3) 混合：out = mix(history_clamped, current, blend)。
//
// 为什么用 YCoCg 而不是 RGB：亮度（Y）与色度（Co/Cg）解耦后，钳制
// 盒在色度方向收紧、亮度方向放宽，正是抗锯齿想要的（边缘的亮度跳变
// 要平滑，色度跳变要抑制）。
//
// 历史无效的三种情况都直接输出当前帧（等价于"这一帧没有 TAA"），
// 下一帧自然重新开始累积：
//   · push 里的 historyValid = 0（首次开启 / 尺寸变化后）；
//   · 重投影 w <= 0（点在相机背后）；
//   · uvPrev 落在屏幕外（相机切走的区域 —— 顺便天然处理了 disocclusion）。
//
// 为什么重建用**未抖动**的投影系数：clipFromView 两侧都是未抖动矩阵，
// 于是"相机没动"就严格推出"速度 0"，抖动不会自己制造出假运动。
// 抖动只影响本帧的光栅化（proj 的 [2][0]/[2][1]，见 Renderer 的
// taaJitterNdc），重建时的亚像素误差在 0.5px 量级，可忽略。
//
// 与 MSAA 的关系：两者可以共存（TAA 读的颜色是 resolve 后的单采样图，
// 只有深度形态随 MSAA 变，故本文件同样编 .msaa 变体）。
// ============================================================

#include "space.glsl"

layout(set = 0, binding = 0) uniform sampler2D sceneColor;   // 本帧（抖动过）HDR
#if VENN_MSAA_DEPTH
layout(set = 0, binding = 1) uniform sampler2DMS taaDepthTex;
#else
layout(set = 0, binding = 1) uniform sampler2D taaDepthTex;
#endif
layout(set = 0, binding = 2) uniform sampler2D historyColor;  // 上一轮解析结果

layout(push_constant) uniform PC {
    vec4 cam;          // x=P00 y=P11 z=zn w=zf（未抖动投影系数）
    mat4 clipFromView; // prevViewProj（未抖动）× inverse(view)
    vec4 params;       // x=blend(当前帧权重) y=历史有效 z=texelX w=texelY
    vec4 clampP;       // x=方差钳制系数(单位：sigma) y/z/w=保留
} pc;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

// ---- YCoCg（无损整数变换的浮点版）----
vec3 rgb2ycocg(vec3 c) {
    return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b,   // Y
                0.5 * c.r - 0.5 * c.b,                 // Co
                -0.25 * c.r + 0.5 * c.g - 0.25 * c.b); // Cg
}
vec3 ycocg2rgb(vec3 c) {
    return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

void main() {
    const vec3 src = texture(sceneColor, vUv).rgb;

    // ---- 历史无效：直接输出当前帧 ----
    if (pc.params.y < 0.5) {
        outColor = vec4(src, 1.0);
        return;
    }

    // ---- 1) 重投影 ----
    const float z = vennViewDepthFromNdc(vennSceneDepth(taaDepthTex, vUv),
                                         pc.cam.z, pc.cam.w);  // 正距离
    const vec2 ndc = vUv * 2.0 - 1.0;
    const vec3 viewPos = vec3(ndc.x * z / pc.cam.x, ndc.y * z / pc.cam.y, -z);
    const vec4 prev = pc.clipFromView * vec4(viewPos, 1.0);
    if (prev.w <= 1e-5) {                 // 点在相机背后 / 退化
        outColor = vec4(src, 1.0);
        return;
    }
    const vec2 uvPrev = (prev.xy / prev.w) * 0.5 + 0.5;
    if (any(lessThan(uvPrev, vec2(0.0))) || any(greaterThan(uvPrev, vec2(1.0)))) {
        outColor = vec4(src, 1.0);        // 上一帧屏幕外 → 无历史
        return;
    }

    // ---- 2) 3×3 邻域统计（YCoCg）→ 方差钳制盒 ----
    vec3 m1 = vec3(0.0);
    vec3 m2 = vec3(0.0);
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const vec3 c = rgb2ycocg(texture(
                sceneColor,
                vUv + vec2(float(x) * pc.params.z, float(y) * pc.params.w))
                .rgb);
            m1 += c;
            m2 += c * c;
        }
    }
    const vec3 mean = m1 / 9.0;
    const vec3 sigma = sqrt(max(m2 / 9.0 - mean * mean, vec3(0.0)));
    const vec3 lo = mean - sigma * pc.clampP.x;
    const vec3 hi = mean + sigma * pc.clampP.x;

    // ---- 3) 钳制历史 + 时间混合 ----
    const vec3 hist = rgb2ycocg(texture(historyColor, uvPrev).rgb);
    const vec3 clamped = clamp(hist, lo, hi);
    outColor = vec4(ycocg2rgb(mix(clamped, rgb2ycocg(src), pc.params.x)), 1.0);
}
