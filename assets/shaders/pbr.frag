#version 450
// bindless 路径（-DVENN_BINDLESS=1，见下面 set 1 的说明）要用**运行时
// 大小数组 + 动态下标**访问采样器数组。glslang 对"非常量下标访问不透明
// 类型数组"一律要求先声明这个扩展，即便下标来自 push 常量（动态 uniform，
// 严格说是合法且不需要 nonuniformEXT 的）。
// 必须放在任何非预处理记号之前 —— 只有 #version 能排在它前面。
#if VENN_BINDLESS
#extension GL_EXT_nonuniform_qualifier : require
#endif
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
// pbr.k<掩码>.frag.spv（掩码 = 位序拼出来的 0..31）。
// 下面是每个宏的默认值 —— 直接 glslc 编（不带任何 -D）时得到"全关"，
// 也就是 k0，行为与本文件引入关键字之前一致，便于对照。
//
//   VENN_DOUBLE_SIDED  双面材质：关背面剔除（管线状态，见
//                      VariantPipelines::applyKeywordState）+ 按
//                      gl_FrontFacing 翻转法线（下面"法线"那段）
//   VENN_ALPHA_BLEND   半透明：alpha 参与输出（管线那边同时不写深度）
//   VENN_NORMAL_MAP    材质挂了法线贴图：采样 + cotangent frame
//   VENN_ALPHA_MASK    alpha 裁切：alpha < push.skin.w 时丢弃片元，
//                      否则按不透明输出（管线侧写深度、不混合）。
//                      MSAA 下借用 alpha-to-coverage 得到边缘抗锯齿。
//                      R4-1；与 ALPHA_BLEND **互斥**（掩码由枚举保证）。
//   VENN_SHADING_SKIN  预积分皮肤散射（R4-2）：漫反射不再用 max(N·L,0)，
//                      而是按 (N·L, 曲率) 查一张预烘焙的散射 LUT —— 光按
//                      扩散核绕过明暗交界线，红通道散射最远 → 耳缘/鼻梁
//                      泛红透光。默认（未置位）代码整段编掉 → 逐像素不变。
//
// ⚠ 位序与 CMakeLists.txt 的 SHADER_KEYWORDS 列表、以及
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
#ifndef VENN_ALPHA_MASK
#define VENN_ALPHA_MASK 0
#endif
#ifndef VENN_SHADING_SKIN
#define VENN_SHADING_SKIN 0
#endif
// VENN_BINDLESS 是**全局开关**（Renderer 按它选 set 布局与 .spv），
// 不是材质关键字位 —— 所以不参与掩码，只多编一族
// pbr.bindless.k<掩码>.frag.spv（见 CMakeLists.txt）。
#ifndef VENN_BINDLESS
#define VENN_BINDLESS 0
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

    // ---- 调试视图模式（R0-2）----
    // x = 模式（0=Lit 原路径，见 Renderer::ViewMode）。y/z/w 保留。
    // 运行时 uniform 分支是刻意的：全局单值 + 随时切换，不值得为它
    // 翻倍 .spv 变体（UE 同款取舍）。
    vec4 viewModeParams;

    // ---- 阴影硬化三件套（R3-1）----
    // x = 级联间混合带宽（占本级覆盖跨度的比例，0 = 关闭）
    // y = 接触硬化强度（0 = 关闭）
    // z / w = 保留
    vec4 shadowFilter;

    // ---- Froxel 体积雾（R3-2）----
    // xyz = froxel 网格尺寸；w = 总开关
    vec4 vfogParams;

    // ---- 预积分皮肤散射（R4-2）----
    // x = 曲率缩放（把"屏幕空间法线导数"映射到 LUT 的 0..1 曲率轴；
    //     MYVK_SSS=0 时为 0 → 曲率恒 0 → LUT 退化成 Lambert，用作对照组）
    // y/z/w = 保留
    vec4 sssParams;
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

// ---- R3-3：逐灯修饰记录（下标与 lights[] 严格对齐）----
// area: x=半宽(米) y=半高(米) z=是否面光源 w=保留
// tex : x=IES层号(-1=无) y=IES开关 z=cookie层号(-1=无) w=cookie开关
struct LightExt {
    vec4 area;
    vec4 tex;
};
layout(std430, set = 0, binding = 17) readonly buffer LightExtBuffer {
    LightExt lightExt[];
};

// 光源修饰纹理库（cookie + IES 共用一张数组）。第 0 层恒为全白：
// 任何"没配贴图"的灯都退化成乘 1。
layout(set = 0, binding = 18) uniform sampler2DArray lightTex;

// ---- 预积分皮肤散射 LUT（R4-2）----
// 256×256 二维表：u = N·L（-1..1 映射到 0..1），v = 曲率（0..1）。
// 表里存的是"已经按扩散核积分过"的漫反射响应（RGB 逐通道，红散射最远）。
// 只在 VENN_SHADING_SKIN 编译单元里被引用；不置位时这个 binding 在
// layout 里仍然存在（关时绑一张退化的缺省表），只是着色器不去读它。
#if VENN_SHADING_SKIN
layout(set = 0, binding = 19) uniform sampler2D sssLut;
#endif

#if VENN_BINDLESS
// ============================================================
// Bindless 路径（R2-3，-DVENN_BINDLESS=1 编出 pbr.bindless.k<N>.frag.spv）
//
// 六张贴图槽不再是六个 set，而是**同一个运行时大小数组**的元素。
// 下标来自材质"页表"（set 1 binding 1），页号由 CPU 逐 draw push 在
// push.emissive.w 里（见 render/Bindless.h 与 RenderTypes.h）。
//
// 下面这六条 #define 是整段改造的全部 —— 函数体里那 6 处
// `texture(albedoMap, vUV)` 一个字都不用改，宏展开后就是
//   texture(textures[gMat.tex0.x], vUV)
// 之所以用宏而不是改调用点：让**非 bindless 的那份 .spv 与这份逐字
// 同源**，两边不可能因为"手抄时改错一次采样"而分叉。
//
// gMat 是文件级变量，在 main() 开头填一次（材质在一个 draw 内恒定，
// 不值得每个采样点都查一次页表）。
// ============================================================
struct GpuMaterialPage {
    uvec4 tex0;  // x=albedo y=normal z=ORM w=roughness
    uvec4 tex1;  // x=metallic y=emissive
};
layout(set = 1, binding = 0) uniform sampler2D textures[];
layout(std430, set = 1, binding = 1) readonly buffer MaterialPages {
    GpuMaterialPage pages[];
};

GpuMaterialPage gMat;

#define albedoMap    textures[gMat.tex0.x]
#define normalMap    textures[gMat.tex0.y]
#define ormMap       textures[gMat.tex0.z]
#define roughnessMap textures[gMat.tex0.w]
#define metallicMap  textures[gMat.tex1.x]
#define emissiveMap  textures[gMat.tex1.y]
#else
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
#endif

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

// ---- Froxel 体积雾 3D 结果图（R3-2，见 render/VolumetricFog.h）----
// 每 texel = (inscatter.rgb, transmittance)："进入该 froxel 时"从相机
// 累积的散射与透射率。关闭时绑 1×1×1 的透明图（rgb=0, a=1）→ 合成
// 退化成 out = lit，与基线逐位一致（与 ssaoTex 的缺省白图同理）。
layout(set = 0, binding = 16) uniform sampler3D vfogTex;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 baseColorFactor;
    vec4 pbr;
    // rgb = 自发光。**w 在 bindless 路径里是材质页号**（R2-3）：
    // 非 bindless 时 CPU 一律推 1.0，本文件的非 bindless 编译单元从不读
    // 这个 w —— 所以"借一个空闲分量"不会改变任何既有像素。
    // push 常量被硬件卡在 128 字节，而 pbr.frag 只声明到偏移 112，
    // 最后 16 字节（也就是这里的 w）是现成的空位；另开一个字段会直接
    // 越过 maxPushConstantsSize。见 render/Bindless.h 的说明。
    vec4 emissive;
    // 蒙皮定位（见 pbr.vert）。**w = alpha 裁切阈值**（R4-1）：
    // 只有 VENN_ALPHA_MASK 编译单元才读它，其它材质推什么值都无所谓。
    // 借 skin.w 是因为 push 已经被硬件卡死在 128 字节，这是最后一个空位。
    vec4 skin;
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
//
// radiusScale 是 R3-1 接触硬化引入的额外乘子（1.0 = 不缩放，即旧行为）。
float pcfCascade(vec3 proj, int cascade, float bias, float radiusScale) {
    ivec3 size = textureSize(shadowMap, 0);
    vec2 texel = 1.0 / vec2(size.xy);
    float radius = ubo.shadowBias.z * (1.0 + ubo.shadowBias.w * float(cascade))
                   * radiusScale;

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

// 采样**单一级联**的可见度（不含距离淡出）。落在本级正交盒之外 → 1.0。
// radiusScale 透传给 PCF（接触硬化用）。
//
// 之所以把"选级联 + 距离淡出"留在 shadowFactor 里、把单级采样拆出来：
// R3-1 的混合带需要**同时**采样相邻两级再按 depth 混合，只有单级版本
// 才能被复用两次而不重复那段盒外判断。
float sampleCascade(vec3 worldPos, int cascade, float NdotL,
                    float radiusScale) {
    vec4 lsp = ubo.lightSpace[cascade] * vec4(worldPos, 1.0);
    vec3 proj = lsp.xyz / lsp.w;
    // 只有 xy 需要从 NDC [-1,1] 映射到纹理坐标 [0,1]。
    // ⚠ z **不能**再 *0.5+0.5：光源投影是 Zero-to-One 约定（近=0 远=1，
    // 见 ShadowPass::updateCascades），深度已经是 Vulkan 的 [0,1]。
    // 再压一次会把参考深度变成实际深度的一半，整片覆盖区都会被误判成阴影。
    proj.xy = proj.xy * 0.5 + 0.5;

    // 落在本级的正交盒之外 → 视为无遮挡。**不能在这里淡出**：级联盒边缘
    // 在屏幕上是任意形状的，做淡出会留下一圈假的暗带。真正的距离淡出
    // 由 shadowFactor 负责（它只依赖视深度，与级联盒无关）。
    if (proj.z > 1.0 || proj.z < 0.0 || proj.x < 0.0 || proj.x > 1.0 ||
        proj.y < 0.0 || proj.y > 1.0) {
        return 1.0;
    }

    // 斜率相关深度偏移：表面越趋近平行于光线，自阴影条纹越严重
    float bias = max(ubo.shadowBias.y * (1.0 - NdotL), ubo.shadowBias.x);
    return pcfCascade(proj, cascade, bias, radiusScale);
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
    const int n = int(ubo.shadowParams.x);

    // ---- 接触硬化（R3-1 之②）----
    // 用接收点在**本级覆盖跨度内的相对位置**当作"离接触区多远"的代理：
    //   近端（t→0，物体与地面接触的地方）→ radiusScale 收到 1（最锐利，
    //     不会因 PCF 模糊而在地面接缝处漏出亮边）；
    //   远端（t→1）→ radiusScale 放大到 1 + strength·k，阴影自然变软。
    // hardenK 取 4 是经验值：配合默认 PCF 半径 1.0 纹素，最多到 5 纹素，
    // 恰好是 3×3 核在纹素被放大后仍不至于跨过整个核的程度。
    // strength = 0 时 radiusScale 恒为 1 → 逐位回到旧路径。
    float radiusScale = 1.0;
    if (ubo.shadowFilter.y > 0.001) {
        float splitEnd = cascade < 3 ? ubo.cascadeSplits[cascade]
                                     : maxDist;
        float t = clamp(viewDepth / max(splitEnd, 1e-3), 0.0, 1.0);
        radiusScale = mix(1.0, 1.0 + ubo.shadowFilter.y * 4.0, t);
    }

    float sh = sampleCascade(worldPos, cascade, NdotL, radiusScale);

    // ---- 级联间混合带（R3-1 之①）----
    // 级联交界处"本级的最后几个纹素"与"下一级的头几个纹素"分辨率不同，
    // 直接切换会在屏幕上留下一条亮线（本级分辨率更细 → 交界处阴影突然
    // 变锐/变糊）。做法：接收点落在"本级远端最后 band 比例"内时，同时
    // 采样下一级并按深度线性混合 —— 交界处两级各占一半，亮线被抹平。
    //
    // band 用**本级覆盖跨度**的比例表示（不是世界距离常量）：近级的
    // 跨度只有几米、远级有几十米，写死米数会让近级几乎全被混合带吃掉。
    float band = ubo.shadowFilter.x;
    if (band > 0.001 && cascade + 1 < n) {
        float splitEnd = ubo.cascadeSplits[cascade];
        float splitPrev = cascade > 0 ? ubo.cascadeSplits[cascade - 1] : 0.0;
        float span = max(splitEnd - splitPrev, 1e-3);
        float bandLo = splitEnd - span * band;
        if (viewDepth > bandLo) {
            float t = clamp((viewDepth - bandLo) / (span * band), 0.0, 1.0);
            float shNext = sampleCascade(worldPos, cascade + 1, NdotL,
                                         radiusScale);
            sh = mix(sh, shNext, t);
        }
    }

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

// ============================================================
// 预积分皮肤散射辅助（R4-2）
//
// ⚠ 必须定义在 shadeLocalLight / shadeLocalLightMod **之前** —— 那两个
//   函数体里在 VENN_SHADING_SKIN 分支下会调用 skinDiffuse。
//
// 曲率与光源无关，所以 main() 只算一次塞进 gSkinCurvature，之后方向光与
// 局部光的着色函数直接读它 —— 免去顺着调用链改函数签名（那两个函数被
// 两处调用，改动面会扩散到本来与皮肤无关的代码）。
// ============================================================
#if VENN_SHADING_SKIN
float gSkinCurvature = 0.0;

// 屏幕空间法线导数 → LUT 的曲率轴（0..1）。
// **用几何法线**（插值后的顶点法线）而不是扰动后的 N：法线贴图的高频
// 细节会让 fwidth 爆表，整张脸变成噪点。
// ubo.sssParams.x 是尺度旋钮（MYVK_SSS 可覆盖），0 时曲率恒 0 → LUT 退化
// 成 Lambert，正好当"关掉 SSS"的对照组。
float skinCurvatureFrom(vec3 geomN) {
    return clamp(length(fwidth(geomN)) * ubo.sssParams.x, 0.0, 1.0);
}

// 漫反射乘子。Default 是 vec3(NdotL)；Skin 换成查表结果 —— 表里已经
// 含"散射核积分"与"逐通道泛红"（R 散射最远）。返回值恒 ≥ 0，所以在
// 明暗交界线外侧也会给出一点光（背阴面渗光）。
//
// 传**未截断**的 N·L：截断会把 LUT 左半轴（背面）整段压成 0，泛红
// 透光正是靠那一段。u = N·L*0.5+0.5 把 [-1,1] 映射到 [0,1]。
vec3 skinDiffuse(vec3 N, vec3 L) {
    float ndotl = dot(N, L);
    float u = ndotl * 0.5 + 0.5;
    // ⚠ 坐标必须落到**纹素中心**（(i+0.5)/N）而不是 0..1 直给。
    // 纹理采样器是 REPEAT 地址模式，而这两条轴的端点恰好会取到 u=1
    // （正对光）与 v=0（平面：fwidth(几何法线)=0 → 曲率恒 0）—— 边界上
    // 双线性会跟"绕到另一侧"的纹素各取一半：曲率 0 的行会掺进曲率 1 的
    // 红边（实测 v=0 读出 0.18，而表里其实是 0.098），皮肤上所有平坦
    // 区域都会无端泛红。(curv*255+0.5)/256 正好是第 curv*255 号纹素的
    // 中心，与 make_sss_lut.py 的 linspace 布点逐格对齐、永不跨界。
    const float kLutSize = 256.0;
    vec2 uv = (vec2(u, gSkinCurvature) * (kLutSize - 1.0) + 0.5) / kLutSize;
    // **必须 textureLod(...,0)**：隐式 LOD 会按导数选 mip，而这张表两条轴
    // 都是连续量，一混相邻 mip 整张响应就糊了。
    return textureLod(sssLut, uv, 0.0).rgb;
}
#endif

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
#if VENN_SHADING_SKIN
    // 局部光也吃曲率响应（交界线更柔、带一点泛红）。**不做背阴面渗光**：
    // 上面已按 NdotL<=0 提前退出，这里 N·L 必为正 —— 改动面最小，也不
    // 碰 GGX 分母（镜面仍用 NdotL）。
    return (kD * albedo / PI * skinDiffuse(N, Ld) + specular * NdotL) * radiance;
#else
    return (kD * albedo / PI + specular) * radiance * NdotL;
#endif
}

// ============================================================
// R3-3：带光源修饰的局部光照求值
//
//   ① IES：按"灯→片元"方向的角度采样柱面展开的光域网剖面（角度→强度）
//   ② light function：把片元方向投影到灯锥截面，采样投影贴图（cookie）
//   ③ 区域光：矩形发射体按"球光代表点"近似 —— 漫反射取最近点（软化
//      接触处过曝），高光沿反射光取球面最近点（把高光摊开变柔）
//
// 只有三个开关里至少一个为真时才走到这里；否则走原始 shadeLocalLight，
// 保证"修饰全关 = 基线逐位一致"。三个开关都是每灯常量，所以关了的时候
// 这个分支在 entire warp 上一致，没有发散代价。
// ============================================================
vec3 shadeLocalLightMod(Light L, LightExt E, vec3 N, vec3 V, vec3 worldPos,
                        vec3 albedo, vec3 F0, float roughness,
                        float metallic) {
    vec3 toLight = L.positionRange.xyz - worldPos;
    float dist = length(toLight);
    float range = L.positionRange.w;
    if (dist >= range) return vec3(0.0);

    vec3 Lc = toLight / max(dist, 1e-5);
    vec3 Ld = Lc;   // 漫反射方向
    vec3 Ls = Lc;   // 高光方向（区域光时才与 Ld 不同）
    float attD = distanceAttenuation(dist, range);
    float attS = attD;
    float mod = 1.0;  // IES × cookie 的乘性调制

    // 灯的局部正交基（由传播方向定义；与 CPU 侧的投影基同源约定）
    const vec3 D = normalize(L.directionCosInner.xyz);
    const vec3 upv = abs(D.z) > 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0);
    const vec3 T1 = normalize(cross(upv, D));
    const vec3 T2 = cross(D, T1);

    if (L.cosOuterType.y > 0.5) {  // 射灯：先算锥形衰减
        const float cosAngle = dot(-Lc, D);
        const float s = spotAttenuation(cosAngle, L.directionCosInner.w,
                                        L.cosOuterType.x);
        attD *= s * s;
        attS *= s * s;

        // ---- ② light function（投影贴图 / cookie）----
        if (E.tex.w > 0.5) {
            const vec3 rel = worldPos - L.positionRange.xyz;
            const float z = dot(rel, D);
            const float tanOuter =
                tan(acos(clamp(L.cosOuterType.x, -1.0, 1.0)));
            const vec2 uvc = vec2(dot(rel, T1), dot(rel, T2)) /
                                 max(z * tanOuter, 1e-5) * 0.5 + 0.5;
            const bool inside = (z > 0.0) &&
                                all(greaterThanEqual(uvc, vec2(0.0))) &&
                                all(lessThanEqual(uvc, vec2(1.0)));
            mod *= inside ? texture(lightTex, vec3(uvc, E.tex.z)).r : 0.0;
        }
    }

    // ---- ① IES 光域网（点光 / 射灯通用）----
    if (E.tex.y > 0.5) {
        const vec3 dir = -Lc;  // 灯 → 片元
        const float theta = acos(clamp(dot(dir, D), -1.0, 1.0));  // 0..π
        const float phi = atan(dot(dir, T2), dot(dir, T1)) / (2.0 * PI) + 0.5;
        mod *= texture(lightTex, vec3(phi, theta / PI, E.tex.x)).r;
    }

    // ---- ③ 区域光（矩形 → 球光代表点）----
    if (E.area.z > 0.5) {
        const float r = sqrt(max(E.area.x * E.area.y, 0.0));  // 等面积球半径
        if (r > 1e-4) {
            // 漫反射：用"离片元最近的球面"作代表点（软化接触过曝）
            attD = distanceAttenuation(max(dist - r, 1e-4), range);
            // 高光：UE4 式球光代表点 —— 反射光线上离球面最近的点
            const vec3 Rr = reflect(-V, N);
            const vec3 centerToRay = dot(toLight, Rr) * Rr - toLight;
            const float c2r = length(centerToRay);
            const vec3 closest =
                toLight + centerToRay * clamp(r / max(c2r, 1e-4), 0.0, 1.0);
            const float dS = max(length(closest), 1e-4);
            Ls = closest / dS;
            attS = distanceAttenuation(dS, range);
        }
    }

    const float NdotLd = max(dot(N, Ld), 0.0);
    const float NdotLs = max(dot(N, Ls), 0.0);
    if (NdotLd <= 0.0 && NdotLs <= 0.0) return vec3(0.0);

    const float shadow = localShadowFactor(L, worldPos, max(NdotLd, 1e-4));
    if (shadow <= 0.0) return vec3(0.0);

    const float NdotV = max(dot(N, V), 1e-4);
    const vec3 H = normalize(V + Ls);
    const float NDF = distributionGGX(N, H, roughness);
    const float G = geometrySmith(N, V, Ls, roughness);
    const vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
    const vec3 specular = (NDF * G * F) / max(4.0 * NdotV * NdotLs, 1e-4);
    const vec3 kS = F;
    const vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);

    const vec3 radiance =
        L.colorIntensity.rgb * L.colorIntensity.a * mod * shadow;
#if VENN_SHADING_SKIN
    // 面积光/IES/cookie 路径：漫反射同样换成曲率响应（含背阴面渗光，
    // 因为这里的提前退出是 "NdotLd<=0 且 NdotLs<=0"，单侧背阴仍会走到）。
    const vec3 diffuse = kD * albedo / PI * attD * skinDiffuse(N, Ld);
#else
    const vec3 diffuse = kD * albedo / PI * attD * NdotLd;
#endif
    const vec3 spec = specular * attS * NdotLs;
    return (diffuse + spec) * radiance;
}

void main() {
#if VENN_BINDLESS
    // 材质页查一次：页号在 push.emissive.w 里（+0.5 是为了把 float 稳妥地
    // 截回整数 —— 页号 ≤ 4096，float 能精确表示，不存在舍入问题）。
    gMat = pages[uint(push.emissive.w + 0.5)];
#endif
    // ---------------- 材质采样 ----------------
    vec4 albedoSample = texture(albedoMap, vUV);
    vec3 albedo = albedoSample.rgb * push.baseColorFactor.rgb;
#if VENN_ALPHA_BLEND
    // 只有半透明材质才让 albedo 的 alpha 参与输出 —— 管线侧同时开了混合、
    // 关了深度写（见 VariantPipelines::applyKeywordState），两边是一体的。
    float alpha = albedoSample.a * push.baseColorFactor.a;
#elif VENN_ALPHA_MASK
    // alpha 裁切（R4-1）：alpha 低于阈值直接丢弃片元 —— 既不写颜色也不写
    // 深度，于是镂空处从背面能透光。
    //
    // 与 **alpha-to-coverage** 共存（管线在 MSAA 下开了 a2c，见
    // applyKeywordState）：把 alpha 预映射成
    //     alpha' = clamp((alpha - cutoff) * 2 + 0.5)
    // 让"alpha == cutoff"正好落在 a2c 硬编码的 0.5 阈值上，于是 cutoff
    // 滑条在 MSAA 下也生效（a2c 阈值本身 Vulkan 不允许配）。
    //
    // ⚠ 两种采样数**共用这一段**，不引入 VENN_MSAA 宏（否则变体数翻倍）：
    //   · samples > 1：alpha' 写进 outColor.a，硬件按覆盖掩码裁决；
    //     "discard 掉 alpha'<=0 的片元"与 a2c 的结果恰好相同（alpha'<=0
    //     ⟺ 覆盖掩码全 0），所以这次 discard 不改变画面，只省一个片元。
    //   · samples == 1：a2c 是空操作，outColor.a 被忽略 —— 这次 discard
    //     就是唯一的裁决手段，两者行为一致。
    float rawAlpha = albedoSample.a * push.baseColorFactor.a;
    // **抖动模式**（ditherAlpha，R4-1）：push.skin.w 的**符号**是开关
    // （负 → 抖动，绝对值仍是 cutoff）。借符号位是因为 push 已满 128 字节。
    // 抖动 = 用一个屏幕空间有序抖动阈值代替固定 cutoff —— 一个片元要么
    // 完全不透明、要么完全透明，只是"谁被留下"随位置变化，配合 TAA 多帧
    // 累积就得到柔和的半透明边缘（且**照样写深度**，不像 Blend 会排序崩）。
    // ⚠ 抖动模式刻意**不要**软边（alpha' 恒为 keep 的 0/1），否则 a2c 的
    //   软边会与时间抖动叠加成两重模糊。
    float cutoff = abs(push.skin.w);
    float keep, alpha;
    if (push.skin.w < 0.0) {
        // 4×4 Bayer 有序抖动（16 级）。有序抖动比哈希噪声在 MSAA 下更稳定
        // （格子与覆盖掩码对齐），也比 8×8 少一半分支代价。
        const float kBayer[16] = float[16](
            0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0,
            3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
        ivec2 p = ivec2(mod(gl_FragCoord.xy, 4.0));
        float threshold = (kBayer[p.y * 4 + p.x] + 0.5) / 16.0;
        keep = step(threshold, rawAlpha);
        alpha = keep;
    } else {
        keep = step(cutoff, rawAlpha);
        alpha = clamp((rawAlpha - cutoff) * 2.0 + 0.5, 0.0, 1.0);
    }
    if (keep < 0.5) discard;
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
#if VENN_SHADING_SKIN
    // 曲率趁**法线贴图扰动之前**取：这一刻的 N 就是几何法线。用扰动后的
    // 法线算 fwidth 会让贴图的高频细节整片爆掉（满脸噪点）。
    gSkinCurvature = skinCurvatureFrom(N);
#endif

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
#if VENN_SHADING_SKIN
    // R4-2：漫反射乘子换成预积分散射响应（明暗交界线外也渗光 + 泛红），
    // 镜面项仍用真实 NdotL —— 高光不该被次表面散射糊掉。
    vec3 direct = (kD * albedo / PI * skinDiffuse(N, Ld) + specular * NdotL) *
                  radiance * shadow;
#else
    vec3 direct = (kD * albedo / PI + specular) * radiance * NdotL * shadow;
#endif

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
        const LightExt ex = lightExt[li];
        // 三个修饰开关全关 → 走**原样**的 shadeLocalLight，与基线逐位一致。
        if (ex.tex.y > 0.5 || ex.tex.w > 0.5 || ex.area.z > 0.5) {
            localLights += shadeLocalLightMod(lights[li], ex, N, V, vWorldPos,
                                              albedo, F0, roughness, metallic);
        } else {
            localLights += shadeLocalLight(lights[li], N, V, vWorldPos, albedo,
                                           F0, roughness, metallic);
        }
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

    // ---------------- Froxel 体积雾（R3-2）----------------
    // 与上面的**高度雾**是独立路径（两条可以同时开，各自叠一次）。
    // 采样：像素所在的 froxel = (屏幕瓦片, 视图深度层)，直接取该格存好的
    // (inscatter, T)，做 out = color * T + inscatter。
    //
    // 视图深度 → 层号：与 fog_inject.comp 的指数分层公式完全一致
    // （d(k) = sliceNear ·(zf/sliceNear)^(k/slices)），反解 k。
    // 关（w<0.5）时整块跳过 —— 基线逐位不变。
    if (ubo.vfogParams.w > 0.5) {
        const float zf = ubo.cameraParams.y;
        const float sliceNear = ubo.cameraParams.z;
        const float slices = ubo.vfogParams.z;

        // 视图空间深度：从世界距离近似（沿视线），足够定位层号。
        const float viewDist = max(distance(ubo.camPos.xyz, vWorldPos),
                                   sliceNear * 1.0001);
        // d(k) = sliceNear · ratio^(k/slices) → k = slices · log(d/sliceNear)/log(ratio)
        const float logRatio = log(zf / sliceNear);
        float kf = slices * log(viewDist / sliceNear) / max(logRatio, 1e-4);
        // 归一化到 [0,1] 的纹理 z 坐标（3D 图是 CLAMP，越界自动夹边）
        const float tz = (kf + 0.5) / slices;

        const vec2 tile = gl_FragCoord.xy / ubo.vfogParams.xy;
        vec4 fog = texture(vfogTex, vec3(tile, tz));
        color = color * fog.a + fog.rgb;
    }

    // ---------------- 调试视图模式（R0-2）----------------
    // Lit（0）是原路径，下面一个字都不执行 —— 基线逐像素不变。
    // 其余模式覆盖输出：
    //   1 Unlit          纯反照率（无光照/阴影 —— 排查材质用）
    //   2 Normal         世界空间法线 * 0.5 + 0.5（蓝紫为主，法线朝向错一眼可见）
    //   3 Roughness      粗糙度灰度图
    //   4 Metallic       金属度灰度图
    //   5 Wireframe      固定绿（管线侧 polygonMode=LINE，全边线透视）
    //   6 Overdraw       固定低强度紫红（管线侧 additive 混合，重叠累加）
    //   7 LightComplexity 簇内灯数热度（蓝=0，绿=1..4，加红=>4）
    const int vmode = int(ubo.viewModeParams.x + 0.5);
    if (vmode != 0) {
        vec3 debug = color;
        if (vmode == 1) {
            debug = albedo;
        } else if (vmode == 2) {
            debug = N * 0.5 + 0.5;
        } else if (vmode == 3) {
            debug = vec3(roughness);
        } else if (vmode == 4) {
            debug = vec3(metallic);
        } else if (vmode == 5) {
            debug = vec3(0.20, 1.00, 0.45);
        } else if (vmode == 6) {
            // 品红（R>B）：additive 累加，重叠越多越亮；选 R 主导是为了
            // 与"天空的蓝"在直方图上区分开。
            debug = vec3(0.040, 0.010, 0.024);
        } else if (vmode == 7) {
            const float n = float(lightCount);
            // 0 灯 = 深蓝底；1..4 灯 = 绿随数增强；>4 再叠红
            debug = vec3(min(max(n - 4.0, 0.0), 8.0) * 0.12,
                         min(n, 4.0) * 0.22,
                         0.06);
        }
        outColor = vec4(debug, 1.0);
        return;
    }

    // alpha 参与混合：透明材质走 baseline blend 管线
    outColor = vec4(color, alpha);
}
