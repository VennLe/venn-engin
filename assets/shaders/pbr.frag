#version 450
// ============================================================
// pbr.frag —— Cook-Torrance PBR 片元着色器（分簇前向 / clustered forward）
//
//   BRDF 三件套：
//     D 法线分布      GGX / Trowbridge-Reitz
//     G 几何遮蔽      Smith + Schlick-GGX
//     F 菲涅尔        Schlick 近似
//   直接光 = (kD * albedo/π + specular) * radiance * NdotL
//   环境光 = **真 IBL**（split-sum）：三条预计算通道
//      漫反射 ← irradianceMap（余弦卷积）
//      镜面   ← prefilterMap（GGX 预过滤，按粗糙度取 mip）
//              × brdfLut（(N·V, roughness) → (A, B)）
//    见 render/Environment.h 与 assets/shaders/common.glsl。
//    关掉（ubo.envParams.x = 0）时回退到旧的程序化天空梯度近似 ——
//    保留它是为了做"同一场景两条路径"的对照实验，不是备用方案。
//
//   法线贴图用屏幕空间导数构造 TBN（cotangent frame），
//   因此顶点数据无需切线属性。
//
// ---- 分簇光照（本文件的核心改动）----
//   本像素属于哪个簇：
//     XY = gl_FragCoord.xy / tileSize           → 屏幕瓦片
//     Z  = 线性化(gl_FragCoord.z) 取指数分层     → 深度层
//   然后从 clusters[] 拿到"本簇的灯列表区间"，遍历 lightIndices[] 里的
//   若干盏灯，各自做一次完整的 BRDF 求和。
//   灯列表由 cluster_build.comp 事先算好 —— 片元阶段不做任何"这盏灯
//   离我远不远"的判断，直接处理已经确定相关的灯。
//
//   簇的编址必须与 cluster_build.comp **完全一致**，否则会出现
//   "亮了不该亮的区域"这类极难定位的问题。
// ============================================================

// ------------------------------------------------------------
// Shader 关键字（编译期开关，见 render/ShaderKeywords.h）
//
// 这份 .frag 会被**编译多份**：构建期按关键字组合各编一份
// pbr.k<掩码>.frag.spv（掩码 = 位序拼出来的 0..7）。
// 下面是每个宏的默认值 —— 直接 glslc 编（不带任何 -D）时得到"全关"，
// 也就是 k0，行为与本文件引入关键字之前一致，便于对照。
//
//   VENN_DOUBLE_SIDED  双面材质：关背面剔除（管线状态，见
//                      VariantPipelines::applyKeywordState）+ 按
//                      gl_FrontFacing 翻转法线（下面"法线"那段）
//   VENN_ALPHA_BLEND   半透明：alpha 参与输出（管线那边同时不写深度）
//   VENN_NORMAL_MAP    材质挂了法线贴图：采样 + cotangent frame
//
// ⚠ 位序与 CMakeLists.txt 的 VARIANT_DEFINES_n 手工表、以及
//   ShaderKeywords.cpp 的 kKeywords[] 必须逐字一致。
// ------------------------------------------------------------
#ifndef VENN_DOUBLE_SIDED
#define VENN_DOUBLE_SIDED 0
#endif
#ifndef VENN_ALPHA_BLEND
#define VENN_ALPHA_BLEND 0
#endif
#ifndef VENN_NORMAL_MAP
#define VENN_NORMAL_MAP 0
#endif

#include "common.glsl"

const float PI = 3.14159265359;

// 旧路径（envParams.x == 0）用的天空常数。与 Environment 的默认
// EnvSettings 一致 —— 两条路径用的是同一个天空模型，差别只在
// "预计算卷积"与"单次方向采样近似"。
const vec3 kFallbackZenith = vec3(0.55, 0.70, 0.95);
const vec3 kFallbackGround = vec3(0.10, 0.09, 0.09);

layout(set = 0, binding = 0) uniform FrameUBO {
    mat4 view;
    mat4 proj;
    vec4 lightDir;
    vec4 lightColor;
    vec4 camPos;
    mat4 lightSpace[4]; // 4 级级联的光源空间矩阵，索引 = 级联号
    vec4 cameraParams;  // x=zNear y=zFar z=切片最近深度 w=深度切片数
    vec4 clusterDims;   // x=gridX y=gridY z=depthSlices w=tileSize
    vec4 screenSize;    // x=width y=height z=1/width w=1/height
    vec4 lightParams;   // x=灯数 y=环境光倍率 z=是否启用剔除
                        // w=每簇槽位数上限（与 ClusteredLighting 的
                        //   maxLightsPerCluster 同源）
    mat4 invViewProj;
    vec4 envParams;     // x=IBL 开关 y=预过滤最大 mip
    vec4 shadowParams;  // x=级联数 y=最大阴影距离 z=淡出起点 w=总开关
    vec4 cascadeSplits; // x..w = 各级覆盖的远端世界距离
    vec4 shadowBias;    // x=常数偏移 y=斜率偏移 z=PCF 半径 w=远级联增长
    // ---- 局部光源阴影参数 ----
    // x=常数偏移 y=斜率偏移 z=PCF 半径(纹素) w=总开关
    // 与方向光分开一套：正交（级联）深度是线性的，透射（点光/聚光）
    // 深度在近处极度非线性，同一套 bias 不可能两边都合适。
    vec4 localShadowParams;
    // ---- 高度雾 ----
    // x=基础密度 y=高度衰减系数(1/米) z=雾总开关 w=保留
    // 雾色不从这里来 —— 下面直接采样天空立方体，保证雾到天际线
    // 的颜色与天空盒自动一致。
    vec4 fogParams;
} ubo;

// 级联阴影贴图：一张 **2D array** 的深度图，第 i 层就是第 i 级级联。
// 用 sampler2DArrayShadow：采样时硬件直接做深度比较，texture() 返回
// 0/1（是否被遮挡），着色器里不需要手写 if。
// 第 3 个分量是**层号**（级联号），第 4 个分量才是比较用的参考深度。
layout(set = 0, binding = 1) uniform sampler2DArrayShadow shadowMap;

// 与 scene::LightInstance 严格对应（std430，4×vec4 = 64 字节）
struct Light {
    vec4 positionRange;      // xyz=世界坐标 w=影响半径
    vec4 colorIntensity;     // rgb=颜色  a=强度
    vec4 directionCosInner;  // xyz=传播方向 w=cos(内锥角)
    // x=cos(外锥角)
    // y=类型(0=点 1=射灯 2=方向光)
    // z=**阴影层基准号**（-1 = 不投影）。点光要 6 层，实际层号 =
    //   基准号 + vennCubeFaceIndex(片元方向)；射灯 1 层，直接就是基准号。
    // w=这盏灯是否投射阴影(0/1)。层基准号由 CPU 每帧分配（Renderer::
    //   prepareLocalShadows），着色器只管用，不知道分配策略。
    vec4 cosOuterType;
};

layout(std430, set = 0, binding = 2) readonly buffer LightBuffer {
    Light lights[];
};

layout(std430, set = 0, binding = 3) readonly buffer ClusterBuffer {
    uvec2 clusters[];  // x=灯索引槽位基址 y=本簇灯数
};

layout(std430, set = 0, binding = 4) readonly buffer LightIndexBuffer {
    uint lightIndices[];
};

layout(set = 1, binding = 0) uniform sampler2D albedoMap;
layout(set = 2, binding = 0) uniform sampler2D normalMap;
layout(set = 3, binding = 0) uniform sampler2D ormMap;  // R=AO G=Rough B=Metal

// 单因子贴图（UE5 风格：每个因子旁边可以再单独挂一张）。
// 与 ormMap 是**相乘**关系，未绑定时绑的是内置纯白 → 乘法恒等。
// 只读 R 通道（灰度），按线性采样（粗糙度/金属度是物理量，不是颜色）。
layout(set = 4, binding = 0) uniform sampler2D roughnessMap;
layout(set = 5, binding = 0) uniform sampler2D metallicMap;
// 自发光贴图：与因子**相加**，未绑定时绑内置纯黑 → 加法恒等。
layout(set = 6, binding = 0) uniform sampler2D emissiveMap;

// ---- IBL 三件套（见 render/Environment.h）----
// 都挂在 set 0：Vulkan 只保证至少 4 个 set 可同时绑定，而 set 1..6
// 已经被六张贴图占满，环境资源只能往 set 0 里塞 —— 语义上也正好，
// 它们本来就是"每帧全局"的。
//   · irradianceMap：32² 立方体，texture(irr, N) = 半球余弦积分 / π
//   · prefilterMap ：128² 立方体、5 级 mip，mip ↔ 粗糙度
//   · brdfLut      ：256² 二维表，(N·V, roughness) → (A, B)
layout(set = 0, binding = 7) uniform samplerCube irradianceMap;
layout(set = 0, binding = 8) uniform samplerCube prefilterMap;
layout(set = 0, binding = 9) uniform sampler2D brdfLut;
// 天空立方体（与 sky pass 同一张图）：高度雾用它取雾色 —— 远处物体
// 淡入的终点与背景天空严格一致。只在 fogParams.z > 0.5 时才采样，
// 关着时这条采样不发生，基线逐位不变。
layout(set = 0, binding = 10) uniform samplerCube skyMap;

// ---- 局部光源阴影（点光 / 聚光灯，见 render/LocalShadowPass.h）----
// 逐层的"世界 → 光源裁剪矩阵"。**同一块 buffer 被两处读**：
//   · shadow.vert —— 渲染对应层的深度图时用它变换顶点
//   · 本文件      —— 采样时把片元世界坐标投回同一空间求参考深度
// 只有一份数据源，所以不可能出现"渲染与采样用了不同矩阵"。
struct LightShadow {
    mat4 matrix;  // 世界 → 光源裁剪空间（*_RH_ZO，clip z ∈ [0,1]）
    // x = near（米） y = far（米） z = 所属光源下标（调试用） w = 该层是否有效
    // xy 是必需的：要把"世界单位的深度偏移"换算成 NDC（见
    // ndcBiasFromWorld）。透视投影的 NDC 深度是双曲的，少了它们只能拍
    // 一个拍不准的 NDC 常数，实测会把聚光灯的阴影整个推没。
    vec4 params;
};
layout(std430, set = 0, binding = 11) readonly buffer LightShadows {
    LightShadow records[];
} lightShadow;

// 局部光源阴影图：一张 2D array 深度图（采样器带硬件深度比较）。
//   · 聚光灯占 1 层（沿锥轴一次透视投影）
//   · 点光占 6 层（立方体 6 面，面号 = vennCubeFaceIndex）
// 层号不是固定的，由 CPU 每帧分配 → 从灯表的 cosOuterType.z 读。
layout(set = 0, binding = 12) uniform sampler2DArrayShadow localShadowMap;

// ---- SSAO 结果（见 render/SSAOPass.h 与 assets/shaders/ssao.frag）----
// R8 遮蔽图（1 = 无遮蔽），在深度预通道与前向之间算好。SSAO 关闭时
// 绑的是缺省纯白图 → 采样恒为 1.0，乘上去逐位不变 —— "关闭 = 基线"
// 的对照约定由此成立，不需要着色器里再开一个 if。
layout(set = 0, binding = 14) uniform sampler2D ssaoTex;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 baseColorFactor;
    vec4 pbr;
    vec4 emissive;
} push;

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUV;

layout(location = 0) out vec4 outColor;

// ---------------- BRDF 各项 ----------------

// GGX 法线分布：rough=0 时趋于镜面脉冲，rough=1 时非常平缓
float distributionGGX(vec3 N, vec3 H, float rough) {
    float a = rough * rough;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-6);
}

// Schlick-GGX 几何项（直接光用 k = (r+1)^2 / 8）
float geometrySchlickGGX(float NdotV, float rough) {
    float r = rough + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / max(NdotV * (1.0 - k) + k, 1e-6);
}

float geometrySmith(vec3 N, vec3 V, vec3 L, float rough) {
    return geometrySchlickGGX(max(dot(N, V), 0.0), rough) *
           geometrySchlickGGX(max(dot(N, L), 0.0), rough);
}

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// 旧路径的近似环境色。天空模型本身在 common.glsl 的 vennSkyColor()，
// 与烘焙环境贴图用的是**同一个函数** —— 这条路径的意义是"用单次
// 方向采样代替卷积"，而不是"换一套天空"。
//
// 它为什么不够：方向采样只对"完全光滑的镜面"和对"完全均匀的环境"
// 才正确。粗糙表面应该对波瓣范围做积分，这里却只取一个点；反射方向
// 用 mix(R, N, rough²) 折中一下，粗糙度只让整体变暗，不会真的变糊。
vec3 envColor(vec3 dir) {
    return vennSkyColor(dir, kFallbackZenith, kFallbackGround);
}

// 用位置/UV 的屏幕空间导数构造切线基（无需顶点切线属性）
mat3 cotangentFrame(vec3 N, vec3 p, vec2 uv) {
    vec3 dp1 = dFdx(p);
    vec3 dp2 = dFdy(p);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);

    vec3 dp2perp = cross(dp2, N);
    vec3 dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;

    float m = max(dot(T, T), dot(B, B));
    if (m < 1e-12) return mat3(vec3(1, 0, 0), vec3(0, 1, 0), N);
    float inv = inversesqrt(m);
    return mat3(T * inv, B * inv, N);
}

// ================= 级联阴影（CSM）=================
//
// 三层"距离分级"：
//   ① **级联划分**：近处一级小而密，远处一级大而疏。分割点由 CPU 按
//      log/uniform 混合算好放进 ubo.cascadeSplits。
//   ② **级联选择按视深度**（不是世界距离、也不是 gl_FragCoord.z）：用
//      ubo.view 把世界点变到视图空间取 -z，与 CPU 侧算分割点是同一把尺子。
//   ③ **最大阴影距离 + 淡出**：超出 ubo.shadowParams.y 彻底没有阴影，并在
//      最后一段距离里 smoothstep 淡出，避免阴影在地平线处"一刀切"。
//
// 级联盒的构造见 ShadowPass::updateCascades —— 关键点是每级用**视锥切片的
// 包围球**（与光方向无关 → 相机平移时半径稳定），并把球心在光空间里量化到
// 纹素网格（消除相机移动时的阴影边缘抖动）。

// 按视深度选级联，返回 [0, 级联数)
int selectCascade(float viewDepth) {
    int n = int(ubo.shadowParams.x);
    int last = max(n - 1, 0);
    int c = last;   // 视深度超过所有分割点时落到最后一级
    for (int i = 0; i < 4; ++i) {
        if (i >= n) break;
        if (viewDepth <= ubo.cascadeSplits[i]) {
            c = i;
            break;
        }
    }
    return c;
}

// 3×3 PCF。单点采样会得到锯齿状硬边，对邻域取平均后边缘自然柔和。
// 核半径（纹素）随级联变远而变大：远级联一个纹素覆盖的世界尺寸更大，
// 用同样的纹素半径会显得更硬、也更容易随相机移动而闪烁。
float pcfCascade(vec3 proj, int cascade, float bias) {
    ivec3 size = textureSize(shadowMap, 0);
    vec2 texel = 1.0 / vec2(size.xy);
    float radius = ubo.shadowBias.z * (1.0 + ubo.shadowBias.w * float(cascade));

    float sum = 0.0;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            sum += texture(shadowMap,
                           vec4(proj.xy + vec2(x, y) * texel * radius,
                                float(cascade), proj.z - bias));
        }
    }
    return sum / 9.0;
}

// 返回 [0,1] 的**可见度**：1 = 完全受光，0 = 完全在阴影里。
float shadowFactor(vec3 worldPos, float NdotL) {
    if (ubo.shadowParams.w < 0.5) return 1.0;

    // 视深度：与 CPU 侧算分割点用的是同一把尺子（沿相机视线的线性深度）
    float viewDepth = -(ubo.view * vec4(worldPos, 1.0)).z;

    // ---- 距离分级：超出最大阴影距离 → 完全没有阴影 ----
    float maxDist = ubo.shadowParams.y;
    if (viewDepth > maxDist) return 1.0;
    // 最后一段距离线性淡出：1（完整阴影）→ 0（无阴影）。
    // 两个端点都连续，所以不会出现一圈突兀的边界。
    float fade = 1.0 - smoothstep(maxDist * ubo.shadowParams.z, maxDist,
                                  viewDepth);

    // ---- 选级联 ----
    int cascade = selectCascade(viewDepth);

    vec4 lsp = ubo.lightSpace[cascade] * vec4(worldPos, 1.0);
    vec3 proj = lsp.xyz / lsp.w;
    // 只有 xy 需要从 NDC [-1,1] 映射到纹理坐标 [0,1]。
    // ⚠ z **不能**再 *0.5+0.5：光源投影是 Zero-to-One 约定（近=0 远=1，
    // 见 ShadowPass::updateCascades），深度已经是 Vulkan 的 [0,1]。
    // 再压一次会把参考深度变成实际深度的一半，整片覆盖区都会被误判成阴影。
    proj.xy = proj.xy * 0.5 + 0.5;

    // 落在本级的正交盒之外 → 视为无遮挡。**不能在这里淡出**：级联盒边缘
    // 在屏幕上是任意形状的，做淡出会留下一圈假的暗带。真正的距离淡出
    // 由上面的 fade 负责（它只依赖视深度，与级联盒无关）。
    if (proj.z > 1.0 || proj.z < 0.0 || proj.x < 0.0 || proj.x > 1.0 ||
        proj.y < 0.0 || proj.y > 1.0) {
        return 1.0;
    }

    // 斜率相关深度偏移：表面越趋近平行于光线，自阴影条纹越严重
    float bias = max(ubo.shadowBias.y * (1.0 - NdotL), ubo.shadowBias.x);

    float sh = pcfCascade(proj, cascade, bias);
    return mix(1.0, sh, fade);
}

// ---------------- 局部光源阴影（点光 / 聚光灯）----------------

// 3×3 PCF（局部光源版）。层号由调用方算好一起传进来。
float pcfLocal(vec2 uv, float refZ, float layer, float bias) {
    ivec3 size = textureSize(localShadowMap, 0);
    vec2 texel = 1.0 / vec2(size.xy);
    const float radius = ubo.localShadowParams.z;

    float sum = 0.0;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            sum += texture(localShadowMap,
                           vec4(uv + vec2(x, y) * texel * radius, layer,
                                refZ - bias));
        }
    }
    return sum / 9.0;
}

// 把"世界单位的深度偏移"换算成当前参考深度处的 NDC 偏移。
//
// 透视投影（*_RH_ZO）的 NDC 深度是双曲的：
//     z(d) = (f / (f - n)) · (1 - n / d)
// 对 d 求导得  dz/dd = (f - z·(f - n))² / (n·f·(f - n))。
// 在远平面附近这个值极小（range=32、near=1.6 时约 1.6e-3 每米），
// 意味着 **0.0016 的 NDC 偏移就等于 1 米** —— 上一版就是按 NDC 给的
// 常数偏移，结果把聚光灯的阴影整个推没了。反过来在近平面附近同样的
// NDC 偏移只有几毫米。所以偏移必须以米为单位给，在这里换算。
float ndcBiasFromWorld(float refZ, float nearP, float farP, float worldBias) {
    const float span = farP - nearP;
    if (span <= 1e-6) return 0.0;
    const float denom = farP - refZ * span;
    return worldBias * (denom * denom) /
           max(nearP * farP * span, 1e-9);
}

// 单盏局部光源对本像素的**可见度**：1 = 完全受光，0 = 完全在阴影里。
//
// 层号的推导是这里唯一容易错的地方，两步都从灯表读，不猜：
//   1. 基准层号 = L.cosOuterType.z（CPU 分配的；-1 表示没分到层）
//   2. 点光再按"从灯指向片元的方向"选立方体的面 → 基准号 + 面号
// 面号必须与 common.glsl 的 vennCubeDir 表一致 —— LocalShadowPass 的
// CPU 侧就是用那张表生成 6 个面的视图矩阵的。
float localShadowFactor(Light L, vec3 worldPos, float NdotL) {
    if (ubo.localShadowParams.w < 0.5) return 1.0;  // 总开关
    if (L.cosOuterType.w < 0.5) return 1.0;         // 这盏灯不投影
    const int base = int(L.cosOuterType.z + 0.5);
    if (base < 0) return 1.0;                       // 层预算用完了

    int layer = base;
    if (L.cosOuterType.y < 0.5) {  // 点光：6 个面里选一个
        vec3 d = worldPos - L.positionRange.xyz;
        if (dot(d, d) < 1e-8) return 1.0;
        layer = base + vennCubeFaceIndex(d);
    }

    const vec4 layerParams = lightShadow.records[layer].params;
    vec4 lp = lightShadow.records[layer].matrix * vec4(worldPos, 1.0);
    // 光源背后：透视除法会翻号，投影没有意义 → 视为无遮挡。
    // （正交的级联那边 w 恒为 1，永远不会走到这个分支。）
    if (lp.w <= 1e-5) return 1.0;

    vec3 proj = lp.xyz / lp.w;
    // 只有 xy 要映射到 [0,1]；z 已经是 Zero-to-One 的参考深度。
    proj.xy = proj.xy * 0.5 + 0.5;

    // 落在本层的光锥/光盒之外 → 无遮挡。刻意不做淡出：每一层的边界在
    // 屏幕上是任意形状，淡出会留下一圈假暗带（与级联那边同一条教训）。
    if (proj.z < 0.0 || proj.z > 1.0 || proj.x < 0.0 || proj.x > 1.0 ||
        proj.y < 0.0 || proj.y > 1.0) {
        return 1.0;
    }

    // 斜率相关的深度偏移：表面越趋近平行于光线，自阴影条纹越严重。
    // 参数是**米**，按本层的 near/far 换算成该深度处的 NDC 偏移。
    const float worldBias = max(ubo.localShadowParams.y * (1.0 - NdotL),
                                ubo.localShadowParams.x);
    const float bias =
        ndcBiasFromWorld(proj.z, layerParams.x, layerParams.y, worldBias);
    return pcfLocal(proj.xy, proj.z, float(layer), bias);
}

// ---------------- 分簇：定位本像素所属的簇 ----------------

// 视图空间深度 d ↔ NDC z 的互换（与 cluster_build.comp 用同一组公式）
float ndcToDepth(float z, float zn, float zf) {
    return (2.0 * zn * zf) / ((zn + zf) - z * (zf - zn));
}

uint clusterZIndex(float ndcZ) {
    const float zn = ubo.cameraParams.x;
    const float zf = ubo.cameraParams.y;
    const float clusterNear = ubo.cameraParams.z;
    const float slices = ubo.cameraParams.w;

    float d = ndcToDepth(ndcZ, zn, zf);
    d = clamp(d, clusterNear, zf);

    // 与 compute 侧 d(k) = clusterNear·(zf/clusterNear)^(k/slices) 互逆
    float k = log(d / clusterNear) / log(zf / clusterNear) * slices;
    return uint(clamp(k, 0.0, slices - 1.0));
}

uint clusterIndexOf(vec2 fragCoord, float ndcZ) {
    const uint gridX = uint(ubo.clusterDims.x);
    const uint gridY = uint(ubo.clusterDims.y);
    const uint slices = uint(ubo.clusterDims.z);
    const float tileSize = ubo.clusterDims.w;

    // gl_FragCoord 是像素中心坐标（从 0.5 开始），除以 tileSize 向下取整
    // 得到瓦片号；夹到网格内避免右/下边缘越界
    uvec2 tile = min(uvec2(fragCoord / tileSize), uvec2(gridX - 1u, gridY - 1u));
    uint z = min(clusterZIndex(ndcZ), slices - 1u);

    // 与 compute 侧的 (z*gridY + y)*gridX + x 完全一致
    return (z * gridY + tile.y) * gridX + tile.x;
}

// ---------------- 局部光源求值 ----------------

// 窗口化平方反比衰减：range 处严格归零。
// 朴素的 1/d² 在 range 边界处会留下一个突兀的硬边 —— 乘上一个
// "在 range 处恰好降到 0"的窗口函数才能让灯的影响范围自然消失。
float distanceAttenuation(float dist, float range) {
    float ratio = dist / max(range, 1e-4);
    float r4 = ratio * ratio * ratio * ratio;
    float window = clamp(1.0 - r4, 0.0, 1.0);
    return (window * window) / max(dist * dist, 1e-4);
}

// 锥形衰减：内锥内为 1，外锥外为 0，中间平滑过渡
float spotAttenuation(float cosAngle, float cosInner, float cosOuter) {
    return clamp((cosAngle - cosOuter) / max(cosInner - cosOuter, 1e-4),
                 0.0, 1.0);
}

// 单盏局部光源的完整 BRDF 贡献。
// 类型：0 = 点光，1 = 射灯，2 = 方向光（实体级补光：无衰减、无阴影，
//       L 直接取 -direction；分簇剔除靠它 10⁵ 米的"影响球"保证全覆盖）。
// 注意：灯列表是 compute 侧"保守剔除"的结果，可能包含实际照不到
// 这里的灯（尤其是射灯 —— 只按 range 球剔除），所以这里每一步
// 都要老老实实判断并提前退出。
vec3 shadeLocalLight(Light L, vec3 N, vec3 V, vec3 worldPos, vec3 albedo,
                     vec3 F0, float roughness, float metallic) {
    vec3 Ld;
    float att;

    if (L.cosOuterType.y > 1.5) {
        // ---- 方向光 ----
        Ld = normalize(-L.directionCosInner.xyz);
        att = 1.0;
    } else {
        // ---- 点光 / 射灯 ----
        vec3 toLight = L.positionRange.xyz - worldPos;
        float dist = length(toLight);
        float range = L.positionRange.w;
        if (dist >= range) return vec3(0.0);

        Ld = toLight / max(dist, 1e-5);
        att = distanceAttenuation(dist, range);

        if (L.cosOuterType.y > 0.5) {  // 射灯
            float cosAngle = dot(-Ld, L.directionCosInner.xyz);
            float s = spotAttenuation(cosAngle, L.directionCosInner.w,
                                      L.cosOuterType.x);
            att *= s * s;  // 平方一下让边缘收得更利落
        }
    }

    float NdotL = dot(N, Ld);
    if (NdotL <= 0.0) return vec3(0.0);
    if (att <= 0.0) return vec3(0.0);

    // 阴影只衰减**直接光**（与方向光同一条约定）。方向光（type=2）的
    // cosOuterType.w 恒为 0，这里会直接短路返回 1。
    const float shadow = localShadowFactor(L, worldPos, NdotL);
    if (shadow <= 0.0) return vec3(0.0);

    vec3 H = normalize(V + Ld);
    float NdotV = max(dot(N, V), 1e-4);

    float NDF = distributionGGX(N, H, roughness);
    float G = geometrySmith(N, V, Ld, roughness);
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);

    vec3 specular = (NDF * G * F) / max(4.0 * NdotV * NdotL, 1e-4);
    vec3 kS = F;
    vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);

    // 强度与颜色一起构成辐射亮度；HDR 下可以远大于 1
    vec3 radiance = L.colorIntensity.rgb * L.colorIntensity.a * att * shadow;
    return (kD * albedo / PI + specular) * radiance * NdotL;
}

void main() {
    // ---------------- 材质采样 ----------------
    vec4 albedoSample = texture(albedoMap, vUV);
    vec3 albedo = albedoSample.rgb * push.baseColorFactor.rgb;
#if VENN_ALPHA_BLEND
    // 只有半透明材质才让 albedo 的 alpha 参与输出 —— 管线侧同时开了混合、
    // 关了深度写（见 VariantPipelines::applyKeywordState），两边是一体的。
    float alpha = albedoSample.a * push.baseColorFactor.a;
#else
    // 不透明：直接输出 1。省掉这次乘法是小事，主要是别让 HDR 目标的
    // alpha 通道带着一堆没人看的值一路传到后处理。
    float alpha = 1.0;
#endif

    float metallic = push.pbr.x;
    float roughness = push.pbr.y;
#if VENN_NORMAL_MAP
    float normalScale = push.pbr.z;
#else
    // 没有法线贴图时 normalScale 用不到。留着会是未使用变量，
    // 干脆跟着一起编掉。
    const float normalScale = 0.0;
#endif
    float ao = push.pbr.w;

    vec3 orm = texture(ormMap, vUV).rgb;
    ao *= orm.r;
    roughness *= orm.g;
    metallic *= orm.b;

    // 单因子贴图再乘一层（未绑定时是纯白，等于没乘）。
    // 于是 glTF 那种打包 ORM 与"一张一张单独指定"两种用法可以共存：
    // 想单独换粗糙度，只需要在粗糙度槽里放一张图，不用去动 ORM。
    roughness *= texture(roughnessMap, vUV).r;
    metallic *= texture(metallicMap, vUV).r;

    roughness = clamp(roughness, 0.04, 1.0);
    metallic = clamp(metallic, 0.0, 1.0);

    // ---------------- 法线 ----------------
    vec3 N = normalize(vNormal);

#if VENN_DOUBLE_SIDED
    // 双面材质：背面看到的其实是几何背侧，法线要翻过来才"朝向观察者"。
    // 不翻的话背面 N·L < 0 → 直接光整片归零，看起来像一块纯黑的剪影。
    //
    // ⚠ 必须在采样法线贴图**之前**翻：下面 cotangent frame 是拿 N 当
    //   基准轴建的，先翻再建 TBN，扰动后的法线才与"看得见的那一面"
    //   一致。反过来（先建 TBN 再翻 N）会得到一个既不是 N 也不是 -N 的
    //   斜法线，背面光照会歪。
    if (!gl_FrontFacing) N = -N;
#endif

#if VENN_NORMAL_MAP
    // 有法线贴图才做这一整段。没有法线图的材质（normalMap == nullptr）
    // 在 C++ 侧就不会置这个位（见 keywordsOf），于是这次纹理采样与
    // 下面的 cotangent frame（两次叉乘 + 归一化）**整段被编掉**。
    //
    // 编掉之后画面对不对？对的，而且是逐像素相同：没挂法线图时绑的是
    // 内置平坦法线贴图 (0.5,0.5,1)，*2-1 得 (0,0,1)；xy 再乘 normalScale
    // 仍是 0，于是 TBN * (0,0,1) 恰好等于 TBN 的第三列 = N 本身。
    // 验证脚本 tools/verify_shader_variants.py 把这条当核心断言。
    vec3 nSample = texture(normalMap, vUV).xyz * 2.0 - 1.0;
    nSample.xy *= normalScale;
    if (dot(nSample.xy, nSample.xy) > 1e-8) {
        mat3 TBN = cotangentFrame(N, vWorldPos, vUV);
        N = normalize(TBN * normalize(nSample));
    }
#endif

    vec3 V = normalize(ubo.camPos.xyz - vWorldPos);
    float NdotV = max(dot(N, V), 0.0);

    // 电介质基础反射率 0.04；金属的 F0 取 albedo
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    // ---------------- 方向光（唯一带阴影的灯）----------------
    vec3 Ld = normalize(-ubo.lightDir.xyz);
    vec3 H = normalize(V + Ld);
    float NdotL = max(dot(N, Ld), 0.0);

    float NDF = distributionGGX(N, H, roughness);
    float G = geometrySmith(N, V, Ld, roughness);
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
    vec3 specular = (NDF * G * F) / max(4.0 * NdotV * NdotL, 1e-4);

    vec3 kS = F;
    vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);

    vec3 radiance = ubo.lightColor.rgb * ubo.lightDir.w;

    // 阴影只衰减直接光；环境光不受遮挡影响（简化处理）
    float shadow = shadowFactor(vWorldPos, NdotL);
    vec3 direct = (kD * albedo / PI + specular) * radiance * NdotL * shadow;

    // ---------------- 分簇局部光源 ----------------
    // 只遍历"本簇灯列表"里的灯，数量由场景布置决定（室内小范围灯通常
    // 每簇几盏），与场景总灯数解耦 —— 这正是分簇的意义。
    vec3 localLights = vec3(0.0);
    const uint clusterIndex = clusterIndexOf(gl_FragCoord.xy, gl_FragCoord.z);
    const uvec2 cluster = clusters[clusterIndex];

    // 槽位上限从 UBO 读，与 compute 侧（push constant）同源。
    // clusters[].y 已被 compute clamp 过，这里只是防御性上界 ——
    // 首帧描述符内容未定时不至于读到越界的灯。
    const uint cap = uint(ubo.lightParams.w);
    const uint lightCount = min(cluster.y, cap);

    for (uint i = 0u; i < lightCount; ++i) {
        const uint li = lightIndices[cluster.x + i];
        localLights += shadeLocalLight(lights[li], N, V, vWorldPos, albedo, F0,
                                       roughness, metallic);
    }

    // ---------------- 环境光 ----------------
    // lightParams.y 是环境光倍率：室内场景把它压小，局部光源才有存在感。
    // 两条路径共用它，否则"开关 IBL"会顺带改变整体亮度，对照实验就废了。
    const float ambientScale = ubo.lightParams.y;

    // ---- 屏幕空间 AO ----
    // 只压环境光（含 IBL），不碰直接光 —— 直接光的遮挡归阴影图管，
    // 环境光的遮挡归 SSAO 管，两套系统各司其职。
    const vec2 ssuv = gl_FragCoord.xy * ubo.screenSize.zw;
    const float ssao = texture(ssaoTex, ssuv).r;

    vec3 ambient;
    if (ubo.envParams.x > 0.5) {
        // ---- 真 IBL（split-sum，Karis）----
        // 三项的来龙去脉见 render/Environment.h；这里只讲用法。

        // 漫反射：辐照度图存的已经是 E/π，直接乘 albedo 就是 Lambert 项
        // （(albedo/π)·E == albedo·(E/π)，卷积时那个 π 就是为此乘的）。
        const vec3 irradiance = texture(irradianceMap, N).rgb;

        // 环境光的菲涅尔要先按粗糙度压一下：光滑表面掠射角几乎全反射，
        // 粗糙表面则没那么极端。用 kS 同时把能量分给镜面与漫反射。
        const vec3 kS_env = vennFresnelSchlickRoughness(NdotV, F0, roughness);
        const vec3 kD_env = (vec3(1.0) - kS_env) * (1.0 - metallic);

        // 镜面：按粗糙度取预过滤 mip。这一句就是"粗糙反射会变糊"的
        // 全部实现 —— 上一版这里只做了一次方向采样，所以怎么调都不像。
        const vec3 R = reflect(-V, N);
        const float maxLod = ubo.envParams.y;
        const vec3 prefiltered =
            textureLod(prefilterMap, R, roughness * maxLod).rgb;

        // split-sum 的第二项：把菲涅尔折成 F0·A + B，于是 F0 不必进表。
        const vec2 ab = texture(brdfLut, vec2(max(NdotV, 1e-4), roughness)).rg;

        const vec3 diffuseIbl = kD_env * albedo * irradiance;
        const vec3 specularIbl = prefiltered * (kS_env * ab.x + ab.y);

        ambient = (diffuseIbl + specularIbl) * ao * ambientScale * ssao;
    } else {
        // ---- 旧路径（近似，仅用于对照）----
        // 漫反射：朝 N 方向单次采样
        const vec3 irradiance = envColor(N) * ao * ambientScale;

        // 镜面：沿反射方向取环境色；用 R 与 N 的插值假装"模糊"，
        // 并让能量随粗糙度线性衰减
        const vec3 R = reflect(-V, N);
        const vec3 specDir = normalize(mix(R, N, roughness * roughness));
        const vec3 envSpec = envColor(specDir) * ao * ambientScale;
        const vec3 F_env = fresnelSchlick(NdotV, F0);
        const vec3 ambientSpecular =
            envSpec * F_env * (1.0 - roughness * 0.75);

        ambient = albedo * irradiance + ambientSpecular;
        ambient *= ssao;
    }

    // 自发光 = 因子 + 贴图。贴图槽空着时绑的是内置纯黑，加法恒等；
    // 放一张图进去立刻发光（因子保留 0 也亮），不用两头调。
    vec3 emissive = push.emissive.rgb + texture(emissiveMap, vUV).rgb;

    vec3 color = ambient + direct + localLights + emissive;

    // ---------------- 高度雾（指数雾 × 高度衰减，Z-up）----------------
    // f = 1 - exp(-距离 × 密度 × exp(-高度 × 衰减))：
    //   · 距离越远雾越厚（指数曲线，近处几乎透明 —— 场景主体不受影响）；
    //   · 高度越高雾越稀（地面附近最浓），Z-up 所以高度就是世界 z。
    // 雾色沿视线方向采**天空立方体**（binding 10，就是天空 pass 画的
    // 那张图）—— 远处物体淡入雾色的终点与背景天空颜色严格一致，
    // 地平线不会出现"雾色和天色对不上"的断层。IBL 关闭（走了近似
    // 天空路径）时用同一套 vennSkyColor 公式现算，两条路径共用模型。
    if (ubo.fogParams.z > 0.5) {
        const float dist = distance(ubo.camPos.xyz, vWorldPos);
        const float h = max(vWorldPos.z, 0.0);
        const float fogAmount = dist * ubo.fogParams.x *
                                exp(-ubo.fogParams.y * h);
        const float fogFactor = clamp(1.0 - exp(-fogAmount), 0.0, 1.0);
        const vec3 vdir = normalize(vWorldPos - ubo.camPos.xyz);
        const vec3 fogColor = (ubo.envParams.x > 0.5)
            ? texture(skyMap, vdir).rgb
            : envColor(vdir);
        color = mix(color, fogColor, fogFactor);
    }

    // alpha 参与混合：透明材质走 baseline blend 管线
    outColor = vec4(color, alpha);
}
