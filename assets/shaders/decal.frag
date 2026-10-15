#version 450
// ============================================================
// decal.frag —— 延迟贴花的 DBuffer 写入（R4-4）
//
// 每个片元做四件事：
//   1. 按 gl_FragCoord 从**场景深度图** texelFetch 出该像素的深度，
//      用 invViewProj 反投影重建世界坐标（与 SSAO / SSR 同一套重建）；
//   2. 乘 worldToLocal 投到贴花局部空间。任一轴的 |局部坐标| 超过半轴
//      → 场景表面不在盒内 → discard（这一条同时就是"深度测试"）；
//   3. 按局部 Z 做**平面投影**取 uv，采样贴花 albedo / 法线；
//   4. 写三张 MRT：反照率 / 世界法线 / 粗糙度-金属度，第四通道统一
//      写 **coverage**（= opacity × 边缘淡出 × 距离淡出 × 贴图 alpha）。
//
// 前向 Pass（pbr.frag）按 coverage 把这三张混进材质。coverage 为 0 时
// （清屏值、或贴花之外）前向那段整块跳过 —— 于是"关掉贴花"与"开着但
// 场景里没有贴花"两条都逐像素回到基线。
//
// 两条 .spv：VENN_MSAA_DEPTH=0 → sampler2D；=1 → sampler2DMS。
// 场景深度是 MSAA 的（跟 HdrTarget 走），多采样图像只能用 sampler2DMS +
// texelFetch（见 space.glsl 顶部说明）。运行期按 HdrTarget::samples() 选。
//
// ⚠ 这里**没有**先 discard 再求导数的写法：dFdx/dFdy 在"同一个 quad 里
//   有片元已经 discard"时结果是未定义的（辅助调用被丢弃 → 导数是垃圾）。
//   所以要用的两处导数 —— 重建法线的 dFdx(P)/dFdy(P)、贴图 uv 的
//   dFdx(duv)/dFdy(duv) —— 都放在所有 discard 之前无条件算完；
//   贴图采样因此走 textureGrad（显式给导数），而不是 texture()。
// ============================================================

#include "space.glsl"

// ---- 与 C++ 的 render::DecalRecord 逐字节一致（std430，192 字节）----
struct DecalRecord {
    mat4 world;         //   0
    mat4 worldToLocal;  //  64
    vec4 tintOpacity;   // 128  rgb = tint, a = opacity
    vec4 sizeEdge;      // 144  xyz = 尺寸(米), w = 边缘淡出比例
    vec4 params;        // 160  x = roughness, y = metallic, z = normalScale
    vec4 fade;          // 176  x = 淡出起点(米), y = 淡出终点(米)
};

layout(std430, set = 0, binding = 2) readonly buffer DecalBuffer {
    DecalRecord decals[];
};

// FrameUBO：声明到 invViewProj 为止的完整前缀（偏移必须逐字段对上
// RenderTypes.h —— 中间少一个 vec4，后面全部错位）。
layout(set = 0, binding = 0) uniform FrameUBO {
    mat4 view;                  //   0
    mat4 proj;                  //  64
    vec4 lightDir;              // 128
    vec4 lightColor;            // 144
    vec4 camPos;                // 160
    mat4 lightSpace[4];         // 176
    vec4 cameraParams;          // 432
    vec4 clusterDims;           // 448
    vec4 screenSize;            // 464  x=w y=h z=1/w w=1/h
    vec4 lightParams;           // 480
    mat4 invViewProj;           // 496
} ubo;

// 场景深度：MSAA 构建时是 sampler2DMS，单采样时是 sampler2D。
#if VENN_MSAA_DEPTH
layout(set = 0, binding = 1) uniform sampler2DMS decalDepthTex;
#else
layout(set = 0, binding = 1) uniform sampler2D decalDepthTex;
#endif

layout(set = 1, binding = 0) uniform sampler2D decalAlbedo;
layout(set = 1, binding = 1) uniform sampler2D decalNormal;

layout(push_constant) uniform Push {
    uint decalIndex;
} pc;

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vBoxN;

layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outRoughMetal;

// 深度 → 世界坐标（一次矩阵乘）。
//
// ⚠⚠ 这里**不能**照抄 space.glsl::vennViewPosFromDepth 的 `depth*2-1`！⚠⚠
//   本引擎的投影是 glm::perspective 生成的、**没有** GLM_FORCE_DEPTH_ZERO_TO_ONE
//   → GLM 的 NDC z 范围是 [-1, 1]；而 Vulkan 的深度范围**恒为 [0,1]**
//   （VkViewport::minDepth=0 / maxDepth=1 → z_f = ndc_z）。两者叠起来的后果是：
//     · 只有 ndc_z ∈ [0,1] 的几何被保留（近处另一半被裁掉，这是已知取舍）；
//     · **深度附件里存的就是 ndc_z 本身**，不是 (ndc_z+1)/2。
//   实测佐证：地面在 9..16.5 m，正确约定下深度应是 0.9817..0.9883（= 1.0005
//   - 0.2001/dist），而 (ndc+1)/2 约定下应是 0.9908..0.9942 —— 探针读到的是
//   前者。若这里写 `depth*2-1`，NDC z 会被二次映射成 0.9634..0.9766，
//   重建点被拉近到 ~1.7 倍（地面上的点全部飞到场地上空 2~3 m），
//   贴花的盒内测试随之全数失败 → 一张贴花都看不见。
vec3 worldPosFromDepth(vec2 uv, float depth, mat4 invViewProj) {
    vec4 w = invViewProj * vec4(uv * 2.0 - 1.0, depth, 1.0);
    return w.xyz / w.w;
}

void main() {
    // 视口内 uv —— 直接由片元坐标换算（DBuffer 与场景同分辨率）。
    const vec2 uv = gl_FragCoord.xy * ubo.screenSize.zw;

    const float depth = vennSceneDepth(decalDepthTex, uv);
    // 天空（深度 = 1）：没有表面可贴，直接丢掉（也顺带避开了
    // 远平面上那几个 dFdx 会退化的像素）。
    if (depth >= 1.0) discard;

    const vec3 P = worldPosFromDepth(uv, depth, ubo.invViewProj);

    // ---- 重建的场景表面法线（世界空间）----
    // 用重建点的屏幕导数叉乘。它在轮廓处会退化（混入背景深度），
    // 所以下面判长度、退化了就退回盒体自己的面法线。
    vec3 sceneN = cross(dFdx(P), dFdy(P));
    const float nLen = length(sceneN);
    sceneN = (nLen > 1e-12) ? (sceneN / nLen) : vBoxN;

    const DecalRecord rec = decals[pc.decalIndex];
    const vec3 local = (rec.worldToLocal * vec4(P, 1.0)).xyz;
    const vec3 halfSize = max(rec.sizeEdge.xyz * 0.5, vec3(1e-6));

    // ---- 局部空间法线（决定贴花朝向哪一面）----
    // 用**场景表面法线**当切线空间的 +Z：于是平坦法线图（0,0,1）映射回
    // 世界后正好还原场景法线 —— 贴花在掠射面上不会有"整片都像贴纸"的
    // 光照突变，法线强度=0 时更是严格不改法线。
    vec3 nL = normalize(mat3(rec.worldToLocal) * sceneN);
    // 从背面看过去时，uv 要左右翻一下，否则图案是镜像的。
    const float side = (nL.z >= 0.0) ? 1.0 : -1.0;

    // 切线基：以局部 Z 为参考轴取一条与 nL 正交的切线；nL 与 Z 平行时
    // 叉乘退化，退回局部 X 轴（贴花正对表面时就是这种情况，最常见）。
    vec3 tL = cross(vec3(0.0, 0.0, side), nL);
    if (dot(tL, tL) < 1e-6) tL = vec3(1.0, 0.0, 0.0);
    tL = normalize(tL);
    const vec3 bL = cross(nL, tL);

    // ---- uv：沿局部 Z 的平面投影 ----
    // local 是**米**（worldToLocal 不含 size 缩放），所以除 size.xy 之后
    // 正好铺满整张贴图；盒体对角/短边也各自映射到 0..1。
    const vec2 duv = vec2(local.x * side, local.y) / rec.sizeEdge.xy + 0.5;

    // ⚠ duv 的屏幕导数必须在这里、**在任何 discard 之前**取。
    //   discard 会让同一个 quad 里的隐式导数未定义（辅助调用被丢弃），
    //   而下面那两次贴图采样又恰好落在"盒内判定通过/不通过"的不一致
    //   控制流里。所以导数提前取好、采样改用 textureGrad —— 既避免了
    //   垃圾 LOD（可能采到不存在的 mip），也让掠射角下的贴图能正常
    //   走 mip（否则 mip 0 点采样在地面这种掠射面上会出摩尔纹）。
    const vec2 ddxUv = dFdx(duv);
    const vec2 ddyUv = dFdy(duv);

    // ---- 盒内测试：不在盒里就丢掉 ----
    // 这一步承担了整个 pass 的"遮挡剔除"职责：贴花盒体在场景表面背后
    // 的像素，重建出来的表面点必然落在盒外。
    const vec3 a = abs(local) / halfSize;
    const float m = max(max(a.x, a.y), a.z);
    if (m > 1.0) discard;

    // ---- 边缘软淡出 ----
    // 硬边会在盒体边界上留一条笔直的接缝（尤其是贴花比表面大时）。
    const float ef = clamp(rec.sizeEdge.w, 0.0, 0.95);
    const float edge = 1.0 - smoothstep(1.0 - ef, 1.0, m);

    // ---- 距离淡出（相机到贴花中心）----
    float distFade = 1.0;
    if (rec.fade.y > rec.fade.x) {
        const float dist = length(ubo.camPos.xyz - rec.world[3].xyz);
        distFade = 1.0 - smoothstep(rec.fade.x, rec.fade.y, dist);
    }

    const vec4 albedoTex = textureGrad(decalAlbedo, duv, ddxUv, ddyUv);
    const vec3 nT = textureGrad(decalNormal, duv, ddxUv, ddyUv).xyz * 2.0 - 1.0;

    const float ns = rec.params.z;
    const vec3 nLocal = normalize(nL + tL * (nT.x * ns) + bL * (nT.y * ns));
    // 局部 → 世界：worldToLocal 是刚体变换的逆，逆的转置就是它自己
    // （R⁻¹ᵀ = R）。**不能**用 mat3(rec.world)：那里面还带着 size 缩放
    // （贴花盒往往是 6×6×2 这种非均匀的），拿它转法线会把方向扭歪。
    const vec3 worldN = normalize(transpose(mat3(rec.worldToLocal)) * nLocal);

    // ---- coverage ----
    // 三段淡出乘在一起，再乘贴图自己的 alpha。clamp 到 0..1：
    // 前向 Pass 拿它当混合权重，>1 会让 mix() 外插出脏颜色。
    const float cov = clamp(rec.tintOpacity.a * edge * distFade *
                                albedoTex.a, 0.0, 1.0);

    // 反照率 = 贴图 × tint（**绝对色**，前向里是"替换"而不是"相乘"）。
    outAlbedo = vec4(albedoTex.rgb * rec.tintOpacity.rgb, cov);
    // 世界法线编码到 0..1（DBuffer 是 UNORM8）。
    outNormal = vec4(worldN * 0.5 + 0.5, cov);
    // 粗糙度 / 金属度：贴花把自己的一套覆盖到被贴表面上。
    outRoughMetal = vec4(rec.params.x, rec.params.y, 0.0, cov);
}
