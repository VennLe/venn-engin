// ============================================================
// space.glsl —— 屏幕空间效果（SSAO / SSR / SSGI / God rays）共享工具
//
// 这些效果全部建立在一件事上：**从深度图重建视图空间坐标**。
// 本引擎的深度图是 MSAA 的（HdrTarget 的多采样深度附件），而
// 多采样图像不能当普通 sampler2D 采样 —— 要用 sampler2DMS +
// texelFetch。于是每个效果着色器都被编译两份：
//
//   <名字>.<阶段>.spv         VENN_MSAA_DEPTH=0（默认）→ sampler2D
//   <名字>.msaa.<阶段>.spv    VENN_MSAA_DEPTH=1        → sampler2DMS
//
// 两份的其余代码逐字相同（本文件按宏切换深度读取一行），
// 运行期由管线创建方按 HdrTarget::samples() 选 .spv。
// 见 CMakeLists.txt 的 SHADER_MSAA_DEPTH_SHADERS。
//
// NDC 约定（必须与引擎其他地方一致，见 cluster_build.comp / sky.vert）：
//   · glm::perspective 未定义 ZERO_TO_ONE → NDC z ∈ [-1, 1]
//   · proj[1][1] 做过 Vulkan 适配的 Y 翻转 → NDC y=-1 是屏幕上边
//     （= 图像第 0 行 = 纹理 v=0），所以 uv = ndc*0.5+0.5 直接成立。
// ============================================================

#ifndef VENN_SPACE_GLSL
#define VENN_SPACE_GLSL

#ifndef VENN_MSAA_DEPTH
#define VENN_MSAA_DEPTH 0
#endif

// ---- 深度读取（[0,1]，深度附件写入的原始值）----
#if VENN_MSAA_DEPTH
// MSAA：texelFetch 取 0 号样本。取单样本而非 4 样本平均 —— 深度在
// 样本间本来就不连续（边缘处跨深度面），平均会产生不存在的中间深度，
// 对 SSAO/SSR 的可见性判断反而更糟。亚像素级偏差可以接受。
float vennSceneDepth(sampler2DMS tex, vec2 uv) {
    ivec2 sz = textureSize(tex);
    ivec2 p = clamp(ivec2(uv * vec2(sz)), ivec2(0), sz - 1);
    return texelFetch(tex, p, 0).r;
}
#else
float vennSceneDepth(sampler2D tex, vec2 uv) {
    return texture(tex, uv).r;
}
#endif

// ---- 深度 → 视图空间坐标（用 invProj 反投影，一次矩阵乘搞定）----
vec3 vennViewPosFromDepth(vec2 uv, float depth, mat4 invProj) {
    // NDC z = depth*2-1（[-1,1] 约定）
    vec4 v = invProj * vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    return v.xyz / v.w;
}

// ---- 视图空间坐标 → 屏幕 uv（用投影系数解析投影）----
//
// 推常量放不下一整个 proj（invProj 已占 64B），改传四个标量：
//   P00 = proj[0][0]  P11 = proj[1][1]（含 Y 翻转，直接用）
//   P22 = proj[2][2]  P23 = proj[3][2]（glm 列主序：clip.z = P22*z + P23）
// RH 透视的 clip.w = -viewZ。近远端各验算一次：
//   z=-n → clip.z = P22*(-n)+P23 = -n → ndc=-1 ✓
//   z=-f → clip.z = P22*(-f)+P23 = +f → ndc=+1 ✓
vec2 vennProjectViewPos(vec3 p, vec4 pc) {
    const float w = max(-p.z, 1e-6);   // RH：w = -z，z 越负（越远）w 越大
    const vec2 ndc = vec2(pc.x * p.x, pc.y * p.y) / w;
    return ndc * 0.5 + 0.5;
}

// ---- 深度 → 正的视深度（只线性化 z，比重建整点便宜得多）----
// 与 cluster_build.comp 的 ndcToDepth 同一条公式（[-1,1] 约定）。
float vennViewDepthFromNdc(float depth, float zn, float zf) {
    const float z = depth * 2.0 - 1.0;
    return (2.0 * zn * zf) / ((zn + zf) - z * (zf - zn));
}

// ---- 从深度图重建表面法线（屏幕空间导数）----
//
// 全屏三角形上每个像素的 dFdx(viewPos) 在同一图元内是常量
// （平面性），叉乘得到的就是**逐图元的平面法线** —— 对 SSAO 的
// 遮挡判断和 SSR 的反射方向来说，这恰好是想要的东西（几何面法线），
// 还省掉一个法线 G-buffer。轮廓处 dFdx 混入背景深度会产生垃圾
// 法线，调用方用 |N| 的长度或 N·V 淡出即可。
vec3 vennNormalFromViewPos(vec3 p) {
    vec3 n = normalize(cross(dFdx(p), dFdy(p)));
    // 相机看向 -Z：朝向相机的表面法线 z 分量为正
    if (n.z < 0.0) n = -n;
    return n;
}

// ---- 逐像素噪声（Interleaved Gradient Noise）----
// 把采样核按像素旋转，抖掉"固定采样图样"造成的条带。
float vennIgNoise(vec2 fragCoord) {
    return fract(52.9829189 * fract(dot(fragCoord,
        vec2(0.06711056, 0.00583715))));
}

#endif // VENN_SPACE_GLSL
