#pragma once
// ============================================================
// render/RenderTypes.h —— 渲染层共享数据结构
//
// 单独成文件的原因：Renderer 与 ShadowPass 都要用到这些结构，
// 若定义在 Renderer.h 里就会形成循环 include。
//
// 改这里的任何一个字段，都必须同步修改对应的着色器声明：
//   FrameUBO            -> pbr.vert / pbr.frag / sky.vert / cluster_build.comp
//                          / shadow.vert（后者只声明到 lightSpace[4] 前缀）
//   PushConstants       -> pbr.vert / pbr.frag / grid.vert
//   ShadowPushConstants -> shadow.vert
//   LightShadowRecord   -> shadow.vert / pbr.frag（set 0 / binding 11 的 SSBO）
// ============================================================

#include "rhi/VulkanCommon.h"

namespace render {

// 级联阴影（CSM）的级联数上限。
// 4 是"够用"与"每帧要画 4 遍投影体"之间的折中：再往上单级覆盖范围
// 改善得很慢，而阴影 Pass 的提交量是线性增长的。
static constexpr uint32_t kShadowCascadeMax = 4;

// ============================================================
// 局部光源阴影（点光 / 聚光灯）的层预算
//
// 所有需要投影的局部光源**共用一张 2D array 深度图**（见 LocalShadowPass）：
//   · 聚光灯占 **1 层** —— 沿锥轴的一次透视投影就够了；
//   · 点光占 **6 层** —— 立方体 6 个面各一次 90° 透视投影。
// 层号就是"这张图里的层下标"，由 Renderer 每帧分配（光源按到相机的
// 距离由近到远排，预算用完就轮不到，宁可不投影也不要抢掉近处的）。
//
// 12 层 = 2 盏点光 + 0 盏聚光，或 12 盏聚光，或任意组合。
// 1024² × 12 层 × 4 字节 × 2 帧 ≈ 100 MB —— 所以分辨率与环境变量
// （MYVK_LOCAL_SHADOW_RES）都可调，默认值偏保守。
// ============================================================
static constexpr uint32_t kMaxLocalShadowLayers = 12;
static constexpr uint32_t kPointLightFaces = 6;
static constexpr uint32_t kSpotLightFaces = 1;

// 单盏局部光源的阴影矩阵记录（std430，共 80 字节）。
// 直接 memcpy 进 SSBO，被 2 个着色器读：
//   · shadow.vert —— 渲染本层深度图时的世界 → 光源裁剪空间变换
//   · pbr.frag    —— 采样时把片元世界坐标投回同一空间求参考深度
// **只有一份数据源**，所以不可能出现"渲染与采样用了不同矩阵"
// （这条约定与 CSM 的 FrameUBO.lightSpace[4] 完全一致）。
struct LightShadowRecord {
    glm::mat4 matrix;  //  0  世界 → 光源裁剪空间（请用 *_RH_ZO，z ∈ [0,1]）
    // x = 该层投影的 near（米）
    // y = 该层投影的 far（米，通常就是灯的 range）
    // z = 该层所属光源在灯表里的下标（-1 = 该层未使用），只用于调试/dump
    // w = 该层是否有效（0/1）
    //
    // xy 不是装饰：透视投影的 NDC 深度是**双曲**分布 —— 同样大小的 NDC
    // 偏移，在远平面附近对应好几米、在近处只有几毫米。所以深度偏移参数
    // 必须以**世界单位**给出，片元里再用 xy 把它换算回 NDC
    // （见 pbr.frag::ndcBiasFromWorld）。少了 xy 就只能拍一个拍不准的
    // 常数 —— 我们踩过：0.0016 的 NDC 常数把聚光灯的阴影整个推没了。
    glm::vec4 params;  // 64
};
static_assert(sizeof(LightShadowRecord) == 80, "LightShadowRecord 布局变了");

// 每帧全局常量缓冲（std140 布局，共 720 字节）
//
// 前半段是阴影 Pass 就在用的老字段；着色器只声明自己需要的前缀即可 ——
// GLSL 的 uniform block 允许比绑定的 buffer 短。
struct FrameUBO {
    glm::mat4 view;        // offset   0
    glm::mat4 proj;        // offset  64
    glm::vec4 lightDir;    // offset 128  xyz=传播方向（指向被照物），w=强度
    glm::vec4 lightColor;  // offset 144  rgb=颜色
    glm::vec4 camPos;      // offset 160  xyz=相机世界坐标

    // ---- 级联阴影（CSM）----
    // 每个级联一个光源空间矩阵（世界 → 光源裁剪空间）。**同一个数组同时
    // 供两处使用**：ShadowPass 渲染各级深度图时用它，pbr.frag 采样时用
    // 它反查——只有一份数据源，不可能出现"渲染与采样用了不同矩阵"。
    //
    // offset 176，共 256 字节（mat4 的 std140 步长就是 64，紧凑排列）
    glm::mat4 lightSpace[kShadowCascadeMax];

    // ---- 分簇前向渲染（clustered forward）----
    // x = zNear, y = zFar
    // z = 分簇切片的最近深度（**不是** zNear）
    // w = 深度切片数（与 clusterDims.z 冗余，方便着色器少读一个分量）
    //
    // 为什么 z ≠ zNear：本引擎的投影由 glm::perspective 生成，且**没有**
    // 定义 GLM_FORCE_DEPTH_ZERO_TO_ONE，所以 NDC z 的映射范围是 [-1, 1]。
    // 而 Vulkan 只在 clip z ∈ [0,1] 内保留几何 —— 于是真正"可见"的最近
    // 深度是 z_ndc=0 对应的那个值：
    //     d_min = 2 * zNear * zFar / (zNear + zFar)
    // 深度切片必须从 d_min 起算，否则靠近相机的几层切片全落在裁剪区外，
    // 白白浪费掉（而且 AABB 也会算错）。
    glm::vec4 cameraParams;  // offset 432

    // x = gridX, y = gridY, z = depthSlices, w = tileSize（像素）
    glm::vec4 clusterDims;   // offset 448

    // x = 宽度, y = 高度, z = 1/宽度, w = 1/高度
    glm::vec4 screenSize;    // offset 464

    // x = 光源数量, y = 环境光强度倍率, z = 是否启用分簇剔除(0/1),
    // w = 每簇槽位数上限（须与 ClusteredLighting::maxLightsPerCluster 相同，
    //     片元着色器靠它做防御性截断）
    glm::vec4 lightParams;   // offset 480

    // NDC(齐次 w=1) → 世界 的反投影矩阵，= inverse(proj * view)
    // 分簇 compute 用它把每个簇的 8 个角点还原到世界空间求 AABB
    glm::mat4 invViewProj;   // offset 496

    // ---- IBL（见 render/Environment.h）----
    // x = IBL 开关（0/1）。关掉时 pbr.frag 回退到旧的程序化天空梯度，
    //     用于"开关两次画面必须不同、但都要正确"的对照实验。
    // y = 预过滤立方体的最大 mip（roughness × 它 = textureLod 的层级）
    // z, w = 保留
    glm::vec4 envParams;     // offset 560

    // ---- 级联阴影参数 ----
    // x = 实际级联数（1..kShadowCascadeMax）
    // y = 最大阴影距离（世界单位）。超出它的像素不再有阴影 ——
    //     这就是"阴影距离分级"的硬边界，配合 z 做平滑淡出。
    // z = 淡出起点占最大距离的比例（0..1）；0.85 表示最后 15% 线性淡出
    // w = 阴影总开关（1 开 / 0 关）
    glm::vec4 shadowParams;  // offset 576

    // x..w = 第 0..3 级级联覆盖的**远端世界距离**（沿相机视线方向的线性
    // 深度）。级联数不足 4 时多余分量重复最后一级的值，着色器不必特判。
    glm::vec4 cascadeSplits; // offset 592

    // x = 常数深度偏移, y = 斜率深度偏移（都作用在归一化深度上）
    // z = PCF 核半径（纹素）, w = 远级联的 PCF 半径增长系数
    glm::vec4 shadowBias;    // offset 608

    // ---- 局部光源阴影参数 ----
    // x = 常数深度偏移（**米**，不是 NDC —— 透视投影下 NDC 偏移没有
    //     固定的世界含义，见 LightShadowRecord::params）
    // y = 斜率深度偏移（米，乘以 (1 - N·L)）
    // z = PCF 核半径（纹素）
    // w = 局部阴影总开关（1 开 / 0 关）
    //
    // 为什么与方向光分开：正交投影（级联）的深度是**线性**的，按 NDC 给
    // 偏移就够了；透视投影（点/聚光）的深度是双曲的，两者不可能共用一套
    // 单位，也不可能共用一组数值。
    glm::vec4 localShadowParams;  // offset 624

    // ---- 高度雾（屏幕空间效果第一弹，2026-10-12）----
    // x = 基础密度（水平方向）；y = 高度衰减系数（1/米，Z-up：高度 =
    //     世界 z，越高越稀）；z = 雾总开关（1 开 / 0 关）；w = 保留
    // 雾色不在这里 —— pbr.frag 直接采样天空立方体（binding 10），
    // 雾到地平线的颜色过渡与天空盒自动一致，用户改天顶色也不用同步。
    glm::vec4 fogParams;          // offset 640

    // ---- 调试视图模式（R0-2，见 Renderer::ViewMode）----
    // x = 模式（0=Lit 原路径、1=Unlit、2=Normal、3=Roughness、4=Metallic、
    //     5=Wireframe、6=Overdraw、7=LightComplexity）
    // y/z/w = 保留
    // 走运行时 uniform 而不是关键字变体：视图模式是**全局单值**且
    // 随时切换 —— 为它编 8x 份 .spv 会让缓存命中率雪崩（UE 同款取舍）。
    glm::vec4 viewModeParams;     // offset 656

    // ---- 阴影硬化三件套（R3-1）----
    // x = 级联间混合带宽（占本级覆盖跨度的比例，0 = 关闭）。
    //     接收点落在"本级远端最后 band 比例"的距离内时，与**下一级**按
    //     深度线性混合 —— 级联交界处两级分辨率突变导致的亮线/硬边就消失。
    //     必须为 0 时整个分支跳过（关闭 = 基线逐像素不变）。
    // y = 接触硬化强度（0 = 关闭）。按接收点在级联内的相对视深度缩放
    //     PCF 核半径：越靠近本级近端（接触区）半径越收，阴影越锐利、
    //     越不漏光；越远则越软。0 时半径不额外缩放。
    // z, w = 保留
    glm::vec4 shadowFilter;       // offset 672

    // ---- Froxel 体积雾（R3-2，见 render/VolumetricFog.h）----
    // x = froxel 网格 X 维（屏幕横向上 froxel 数）
    // y = froxel 网格 Y 维
    // z = froxel 网格 Z 维（深度分层数）
    // w = 总开关（1 开 / 0 关）。关着时 pbr.frag 整块短路 → 基线逐位不变。
    // 片元用 (gl_FragCoord.xy → 瓦片号, 视图深度 → 层号) 定位自己所在的
    // froxel，采样 binding 16 的 3D 图拿 (inscatter.rgb, transmittance)。
    glm::vec4 vfogParams;         // offset 688

    // ---- 预积分皮肤散射（R4-2）----
    // x = 曲率缩放：把"屏幕空间法线导数"映射到 SSS LUT 的曲率轴(0..1)。
    //     0 时曲率恒 0 → LUT 退化成 Lambert，正好是"关掉 SSS"的对照组。
    //     只被 VENN_SHADING_SKIN 编译单元读取，Default 材质完全不受影响。
    // y/z/w = 保留
    glm::vec4 sssParams;          // offset 704

    // ---- 视差遮挡贴图（POM，R4-3）----
    // x = 淡出起点（米）：相机到片元的距离超过它就线性削弱视差 offset；
    // y = 淡出终点（米）：超过它 offset 恒 0 —— **远处退回普通法线贴图**，
    //     既省掉整段 ray-march 的采样，也避开远处纹素被缩小时必然出现的
    //     抖动（层间 UV 步长已经小于一个纹素，继续步进只是在放大走样）。
    // z = 层数上限（步进的最大层数；越大越精确也越贵，典型 16~64）
    // w = 全局倍率兼开关：乘在材质的 heightScale 上。= 0 → offset 恒 0
    //     （整段短路）→ 与没开 POM 逐像素相同，是 A/B 对照的开关。
    //     MYVK_POM 可覆盖，默认 1.0。
    // 只被 VENN_PARALLAX 编译单元读取。
    glm::vec4 pomParams;          // offset 720

    // ---- 延迟贴花（DBuffer，R4-4）----
    // x = 总开关（1 开 / 0 关）。0 时 pbr.frag 整段跳过贴花采样 ——
    //     此时 set0/binding 20..22 绑的是全透明贴图（coverage 恒 0），
    //     两者效果相同，但跳过省掉三次纹理采样。这就是"关 = 基线
    //     逐像素不变"的实现依据。
    // y/z/w = 保留（留给将来的逐贴花强度 / 屏幕空间模糊半径）。
    //
    // ⚠ 用它而不是新开一个着色器关键字位：贴花的开关是**全局**的、
    //   可以随时切（与 vfogParams.w / viewModeParams 同一条取舍），
    //   而关键字位是材质属性 —— 混进去会让变体数平白翻倍。
    glm::vec4 decalParams;        // offset 736

    // ---- 反射探针（R5-1，见 render/ReflectionProbe.h）----
    // x = 总开关（1 开 / 0 关）。0 时 pbr.frag 里整段探针采样短路 ——
    //     此时 binding 23 绑的是 1×1 缺省 cube array，采了也是黑的，
    //     但跳过省掉一次 cube array 采样与一圈探针循环。这就是
    //     "关 = 基线逐像素不变"的实现依据。
    // y = 本帧有效探针数（上限 = ReflectionProbeSystem 的 maxProbes）
    // z = 预过滤 cube array 的最大 mip（roughness × 它 = textureLod 层级）
    // w = 全局强度倍率（探针各自的强度乘在它之上）
    glm::vec4 probeParams;        // offset 752
};
// 总 768 字节。std140 下 mat4 步长 16、vec4 对齐 16，与这里的自然布局一致。
// ⚠ 追加字段只能放**末尾**：sky.vert / grid.vert / shadow.vert /
//   cluster_build.comp 只声明前缀，尾部追加不影响它们的偏移。
//
// 关于 R6-1 粒子：它**不需要**在这里加字段 —— 全局倍率（尺寸 / 亮度 /
// 软化距离）走粒子自己的 push 常量，发射器参数走 SSBO，存活数由间接
// 命令的 instanceCount 承载（CPU 根本不需要知道）。
static_assert(sizeof(FrameUBO) == 768, "FrameUBO 布局变了，必须同步所有声明它的着色器");

// 阴影 Pass 专用的推送常量（std430 布局，共 96 字节）。
//
// **级联阴影与局部光源阴影共用这一个结构**：两者的差别只是
// `which.x` 指向哪张表、以及读哪张深度图。省掉第二套推常量是因为
// shadow.vert 也要共用 —— 一个着色器只能有一个 push_constant 块。
//
// 为什么不复用 PushConstants：那个已经**恰好用满** Vulkan 保证的 128 字节
// 下限（见下），塞不下级联/层索引。而 ShadowPass 拥有自己的管线布局、并且在
// **自己的 render pass 内自行绑定 set 0**（不像前向 Pass 那样"绑一次 set 0
// 然后切一堆管线"），所以它的 push 范围不必与主前向管线逐字段相同 ——
// 绑定时用的布局和绘制时用的布局是同一个对象，不可能不兼容。
struct ShadowPushConstants {
    glm::mat4 model;   //  0  与主路径一致
    glm::vec4 skin;    // 64  x=jointBase（y/z/w 未用）
    // x = 索引（级联号，或局部阴影的**层号**）
    // y = 0 → 从 FrameUBO.lightSpace[x] 取矩阵（CSM）
    //     1 → 从 lightShadows[x].matrix 取矩阵（点光/聚光灯）
    glm::vec4 which;   // 80
};
static_assert(sizeof(ShadowPushConstants) == 96, "ShadowPushConstants 布局变了");

// 每 draw call 的推送常量（std430 布局，共 128 字节）
// Vulkan 规范保证的 push constant 下限正好是 128 字节 —— 恰好用满。
// （从 112 扩上来的原因：蒙皮需要 jointBase 定位骨骼矩阵 SSBO，
//   见 pbr.vert 的 inJoints/inWeights 与 set0/binding6。）
struct PushConstants {
    glm::mat4 model;           //   0
    glm::vec4 baseColorFactor; //  64
    glm::vec4 pbr;             //  80  x=metallic y=roughness z=normalScale w=ao
    glm::vec4 emissive;        //  96  rgb=自发光
    glm::vec4 skin;            // 112  x=jointBase（SSBO 里首个关节矩阵的下标；
                               //       y=关节总数；z=1 表示该网格蒙皮，
                               //       0 = 静态网格，着色器直接走原路径）
                               //      w = alpha 裁切阈值（R4-1，只给 Mask 材质用；
                               //          其它材质推 0.5 的默认值，但着色器只有在
                               //          VENN_ALPHA_MASK 编译单元里才读它）
};

// GPU instancing 的每实例数据（set 0 binding 13，std430）。
// 合批 = 同 (关键字掩码, 材质, 网格) 的多个实体共享一次
// vkCmdDrawIndexedInstanced —— 管线与纹理 set 每批付一次，只有
// model 矩阵逐实例不同，放进这里让顶点着色器按 gl_InstanceIndex 取。
// std430 的 mat4 数组 stride = 64（4 个 align-16 的列向量），与
// glm::mat4 的内存布局逐字节一致，可以直接 memcpy。
struct InstanceData {
    glm::mat4 model;
};
static_assert(sizeof(InstanceData) == 64, "InstanceData 必须是一个裸 mat4");

// ============================================================
// GPU 形变的逐实例记录（§15，set 0 binding 26，std430）
//
//   x = deltaBase   本实例的 delta 流在 MorphDeltas 里的起点
//   y = weightBase  本实例的 target 权重数组在 MorphWeights 里的起点
//   z = targetCount 权重个数（= 本子网格的 morph target 数）
//   w = 保留（0）
//
// 为什么这三项必须**逐实例**而不是逐材质/逐网格：
//   · deltaBase 逐实例，是因为 delta 表**每帧**按用到的网格重新拼
//     （见 Renderer::buildMorphFrameData 的"按 Mesh* 去重"），
//     同一网格在不同帧可能落在不同位置；
//   · weightBase 逐实例，是因为同一个网格的两个实例可以有不同表情。
//
// 与 `InstanceData` 分开成三块 buffer（slots / deltas / weights），
// 而不是塞进 64 字节的实例记录：N 个实例 × 88 个 float 的权重表塞进去
// 会让每个实例的记录从 64 字节涨到 400+ 字节，静态网格也跟着付这份钱。
// ============================================================
struct MorphInstance {
    glm::uvec4 aux{0, 0, 0, 0};
};
static_assert(sizeof(MorphInstance) == 16, "MorphInstance 必须是 16 字节");

// 地平面栅格的推送常量（与 assets/shaders/grid.{vert,frag} 一一对应）。
// 数据只有 48 字节 —— 它不需要额外描述符（相机矩阵从 FrameUBO 读），
// 所以单独开一份小的，而不是复用上面那 112 字节的 PushConstants。
// ⚠ 注意：这 48 字节只是"实际推多少"。栅格**管线布局**仍然声明与主前向
// 管线相同的 112 字节 range（见 Renderer::createPipelines 里的注释），
// 否则它的 layout 会与 set 0 的绑定不兼容，把整个 Pass 的描述符状态打断。
struct GridPushConstants {
    // x = 半边长(米) y = 最小格(米) z = 主格(米) w = 平面高度 y
    glm::vec4 params;
    // rgb = 线色, a = 总不透明度
    glm::vec4 color;
    // x = 开始淡出距离 y = 完全消失距离 z = 线宽(px) w = 最小格强度
    glm::vec4 fade;
};

static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;
// 每个物体在 set 1..N 各绑一张纹理（一个 set = 一个 binding=0 的 sampler）。
// 之所以一张纹理一个 set，是因为描述符集属于 **Texture 对象本身**（见
// Texture::descriptorSet()）：把多张图塞进同一个 set 就变成"组合"才拥有
// 描述符，缓存与生命周期都要重做。多绑几个 set 的代价远小于那笔复杂度。
//   1 albedo  2 normal  3 orm  4 roughness  5 metallic  6 emissive
//   7 height（R4-3 POM 用；只有挂了高度图的材质才真的采样它）
//
// ⚠ 第 7 个既超出"Vulkan 只保证 4 个 set"的规范下限，也超出过去 6 个的
//   用量 —— 所以这是本引擎对 `maxBoundDescriptorSets` 的实际要求（≥ 8）。
//   RTX 3060 报 32；AMD/Intel/Adreno/Mali 普遍报 8，正好够。真遇到更小的
//   设备，前向变体管线会创建失败（不是静默画错）。
static constexpr uint32_t kTextureSetCount = 7;

} // namespace render
