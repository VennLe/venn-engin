#version 450
// ============================================================
// particle.frag —— 粒子的圆形羽化 + 软粒子（R6-1）
//
// 顶点传下来的三个 varying：
//   vUv      四边形上的 [0,1]² 参数 —— **不是屏幕 uv**
//   vColor   rgb = 线性 HDR 颜色（已乘自发光）；a = 不透明度（已乘曲线）
//   vViewZ   视图空间正深度（米）。透视校正插值对"视图空间里线性的量"
//            是精确的（平面片元的三维位置就是线性的），所以直接插就行。
//
// ---- 为什么遮挡不靠深度测试，而在片元里显式判 ----
// 这个 pass 的目标是 HDR 的 **resolve 图**（单采样），而深度图只有
// "被采样"这一种用法可用 —— 它不能同时当这个 pass 的深度附件
// （同一 subpass 里既作附件又作纹理采样是非法的）。于是：
//
//   · 硬遮挡：采场景深度，粒子比它更远就丢掉；
//   · 软粒子：粒子离表面越近越透明。
//
// 这两件事其实是**同一个量**，所以写成一条公式：
//
//     sceneZ = 场景在这一点上的视深度（米）
//     fade   = saturate((sceneZ - vViewZ) / softDistance)
//
//   粒子在表面前方 → sceneZ > vViewZ → fade 大（不透明）；
//   贴住表面      → fade → 0；
//   跑到表面后面  → 差值为负 → saturate 成 0 → **自动就是硬遮挡**。
//
// 关掉软化（params.y = 0）时退化成二值判定（只在"在表面后面"时丢），
// 用来做"软粒子关掉会怎样"的因果对照。
//
// ---- 天空 ----
// 深度 = 1（没有表面）时**什么都不做**：粒子在天空前面，全不透明。
// 硬要线性化会得到一个 ≈ zf 的巨大视深度，公式照样给 fade = 1 ——
// 白算一次反投影，直接跳过更省。
//
// ---- 圆形羽化 ----
//   r² = |vUv*2-1|²，r² > 1 的四个角丢掉 → 圆盘
//   alpha = (1 - r²)²  → 边缘一阶导为 0，看不出方形的"切边"
//
// ---- 两份 .spv ----
// 场景深度图在 4x MSAA 下是 sampler2DMS，必须 texelFetch 单样本
//（多采样图不能当 sampler2D 采，见 space.glsl 顶部）。同一份源码按
// VENN_MSAA_DEPTH 编两份，由 ParticleSystem::createPipelines 按
// HdrTarget::samples() 选。⚠ CMakeLists.txt 的
// SHADER_MSAA_DEPTH_SHADERS 里必须列上 particle.frag。
// ============================================================

#include "space.glsl"

// binding 3 = 场景深度（与 ParticleSystem 的 drawSetLayout 一致）
#if VENN_MSAA_DEPTH
layout(set = 0, binding = 3) uniform sampler2DMS fxDepthTex;
#else
layout(set = 0, binding = 3) uniform sampler2D fxDepthTex;
#endif

layout(push_constant) uniform FxPush {
    // x = 软化距离(米) y = 软化开关(0/1) z = 尺寸倍率 w = 亮度倍率
    vec4 params;
    // x = 视口宽 y = 视口高 z = 近平面 w = 远平面
    vec4 screen;
    // x = aliveBase（顶点阶段用）；片元不用 —— 但 push 常量的**布局是两
    // 个阶段共用的**，两边声明必须逐字一致，少一个字段就会读到错的 params。
    ivec4 extra;
} pc;

layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColor;
layout(location = 2) in float vViewZ;
layout(location = 3) in flat int vSprite;

layout(location = 0) out vec4 outColor;


void main() {
    // ---------------------------------------------------------- ① 圆形羽化
    const vec2 d = vUv * 2.0 - 1.0;
    const float r2 = dot(d, d);
    if (r2 > 1.0) discard;  // 四角不属于这个圆盘
    // ---- 贴图形状（ParticleSprite，见 ecs/Components.h）----
    //
    // 这里全是**距离场**，没有一张贴图：vSprite 由发射器决定，顶点阶段
    // flat 传下来。0 号仍然是改动前那条 (1-r²)² —— 默认路径逐像素不变。
    const float r = sqrt(r2);
    const float edge = 1.0 - r2;
    float feather = edge * edge;
    switch (vSprite) {
        case 1:  // Glow：更软的外晕 + 亮芯
            feather = feather * feather * 2.2 + edge * 0.25;
            break;
        case 2: {  // Spark：四角星芒
            const float a = atan(d.y, d.x);
            const float spike = pow(abs(cos(a * 2.0)), 7.0);
            feather = spike * edge * 1.6 + edge * edge * 0.20;
            break;
        }
        case 3: {  // Smoke：带起伏的团（两个正弦当廉价噪声）
            const float n = 0.5 + 0.25 * sin(d.x * 9.0) * sin(d.y * 9.0) +
                            0.25 * sin((d.x + d.y) * 6.0);
            feather = pow(edge, 1.4) * (0.55 + 0.75 * n);
            break;
        }
        case 4:  // Ring：空心环
            feather = smoothstep(0.42, 0.72, r) * (1.0 - smoothstep(0.78, 1.0, r));
            break;
        case 5: {  // Streak：竖长条（雨丝 / 拖尾）
            const float ax = abs(d.x);
            const float ay = abs(d.y);
            feather = pow(1.0 - ax, 2.0) * (1.0 - ay) * (1.0 - ay);
            break;
        }
        case 6: {  // Flake：六角雪花
            const float a = atan(d.y, d.x);
            const float arms = pow(abs(cos(a * 3.0)), 9.0);
            feather = edge * (0.18 + 0.9 * arms);
            break;
        }
        default:
            break;
    }

    // ---------------------------------------------------------- ② 遮挡 + 软化
    // 屏幕 uv 必须用 gl_FragCoord 现算：vUv 是**四边形上的参数**，拿它当
    // 屏幕坐标会变成"每颗粒子都按自己圆心附近的深度判"，画面上一片片的
    // 硬边。gl_FragCoord 在 Vulkan 里已经是"左上原点"，直接除分辨率即可
    // （与 space.glsl 里 uv = ndc*0.5+0.5 的约定一致）。
    float fade = 1.0;
    const vec2 suv = gl_FragCoord.xy / max(pc.screen.xy, vec2(1.0));
    const float rawDepth = vennSceneDepth(fxDepthTex, suv);
    if (rawDepth < 1.0) {
        // ⚠⚠ 这里**不能**用 space.glsl 的 vennViewDepthFromNdc ⚠⚠
        //
        // 深度缓冲里存的是**裁剪空间 ndc z 本身**：本引擎的投影由
        // glm::perspective 生成（没有 GLM_FORCE_DEPTH_ZERO_TO_ONE），
        // ndc z 的映射范围是 [-1, 1]，而 Vulkan 只保留 clip z ∈ [0, 1]
        // 的几何 —— 于是存下来的就是那个 ndc z（负的半边被裁掉）。
        //
        // vennViewDepthFromNdc 内部还要再 `depth*2-1`，等于把这个 ndc
        // 二次映射一次：真实的 9.6 m 会被算成 5.2 m，粒子整体被判成
        // "在场景后面"而全部丢弃（同一个坑，decal.frag 顶部也记了一段）。
        //
        // 这条公式把两个端点都对上：
        //   raw = 0 → 2·zn·zf/(zn+zf)  ← 引擎里"最近的可见深度"
        //                               （见 RenderTypes.h 的 clusterNear）
        //   raw = 1 → zf               ← 远平面
        const float zn = pc.screen.z;
        const float zf = pc.screen.w;
        const float sceneZ =
            2.0 * zn * zf / ((zn + zf) - max(rawDepth, 0.0) * (zf - zn));
        if (pc.params.y > 0.5) {
            fade = clamp((sceneZ - vViewZ) / max(pc.params.x, 1e-3), 0.0, 1.0);
        } else {
            // 关软化 = 只剩硬遮挡：1e-4 的余量让"正好贴在表面上"的
            // 粒子不被自己的深度误差判掉（深度是 32 位浮点写的，
            // 但两者的计算路径不同，逐位相等不能指望）。
            fade = (sceneZ + 1e-4 >= vViewZ) ? 1.0 : 0.0;
        }
    }
    if (fade <= 0.0) discard;

    // ---------------------------------------------------------- ③ 输出
    // 亮度倍率乘在 rgb 上（加色混合下就是"更亮的火"）。
    // alpha = 曲线 × 羽化 × 软化。两条绘制管线的混合式都乘了 srcAlpha，
    // 所以这里**不预乘** —— 保持"rgb = 材质、a = 覆盖率"的通常语义，
    // 以后改混合式时不会连着改两处。
    outColor = vec4(vColor.rgb * pc.params.w, vColor.a * feather * fade);
}
