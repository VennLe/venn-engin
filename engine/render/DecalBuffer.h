#pragma once
// ============================================================
// render/DecalBuffer —— 延迟贴花的三张 DBuffer（R4-4）
//
// 位置：**深度预通道之后、前向 Pass 之前**。
//
//   深度预通道 ─→ [贴花 pass：写 DBuffer] ─→ 分簇 compute ─→ 前向(采样 DBuffer)
//
// ---- 一张贴花是怎么"贴"上去的 ----
//   1. 画盒体几何（渲染器自建的单位立方体 × 贴花世界矩阵）；
//   2. 片元**不采盒体自己的表面**，而是按 gl_FragCoord 从深度图
//      texelFetch 重建该像素的场景表面世界坐标（与 SSAO/SSR 同一套
//      重建，见 assets/shaders/space.glsl）；
//   3. 该点乘 worldToLocal 投到贴花局部空间：
//        · |localPos.xyz| 任一超过半轴 → 场景表面不在盒内 → discard
//          （这一条同时替代了"深度测试"：盒体在表面后面的像素，重建出的
//            点必然落在盒外）
//        · 否则按局部 Z 方向做平面投影取 uv，采样贴花贴图；
//   4. 结果写进三张 DBuffer（反照率 / 世界法线 / 粗糙度-金属度），
//      第四通道统一是 **coverage**。
//
// ---- 为什么 pass 里没有深度附件 ----
// 引擎的场景深度是 **MSAA** 的（跟 HdrTarget 走），而 DBuffer 是单采样；
// 两者采样数不同，没法共用同一个 RenderPass 的深度附件。所以这个 pass
// 干脆不要深度附件：遮挡关系完全由第 3 步的盒内测试决定，盒体自己
// 也不吃深度测试（管线 depthTest/depthWrite 全关）。
//
// 盒体画**正面**（cull back）。对凸体来说正面的屏幕投影恰好等于整个
// 盒体的屏幕投影，所以"表面点在盒内"的像素一定被覆盖到；反过来
// （相机在盒体内部 → 全部面都是背面）会一个片元都不产生 —— 这是已知
// 边界情形，做个贴花体积大到把相机包进去的程度才会遇到。
//
// ---- 为什么是三张图而不是一张 ----
// 前向 Pass 要在**着色之前**把贴花混进材质：反照率要参与 F0、法线要参与
// N·L、粗糙度/金属度要参与 BRDF。分开存就能各自用合适的精度与混合规则
// （UE 的 DBuffer 同款做法）。
//
// ---- 混合语义 ----
// 三张图用**同一套**混合规则（PipelineDesc 只支持一套）：
//     rgb   = src·src.a + dst·(1 - src.a)
//     alpha = src·1     + dst·(1 - src.a)
// 也就是"颜色按 alpha 混、覆盖度按 over 累加"。于是：
//   · 清屏后 alpha = 0 → 前向 Pass 直接跳过整段（这就是"关 = 基线
//     逐像素不变"与"开着但场景里没有贴花 = 基线"两条断言的实现依据）；
//   · 多张贴花叠在一起时覆盖度会累积到 1，不会出现"两张半透明贴花
//     叠起来反而更淡"的怪事。
//
// 依赖的着色器 / 变体：
//   assets/shaders/decal.vert            所有情况共用
//   assets/shaders/decal.frag            VENN_MSAA_DEPTH=0（深度单采样）
//   assets/shaders/decal.msaa.frag       VENN_MSAA_DEPTH=1（MSAA=4）
// 运行期按 HdrTarget::samples() 选哪份 —— 与 SSAOPass 完全一致。
//
// 环境变量：MYVK_DECALS=1 开（默认**关**：关着时一个 pass 都不录，
//           前向的 binding 20/21/22 指向 1×1 全透明图）。
// ============================================================

#include "rhi/VulkanCommon.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace rhi {
class Device;
class CommandPool;
class Pipeline;
class DescriptorAllocator;
class Buffer;
}

namespace assets {
class Mesh;
class Texture;
}

namespace render {

// 单条贴花的 GPU 记录（std430，共 192 字节）。
//
// 直接 memcpy 进 SSBO，被 decal.frag 按 push 里的下标读取。
// 为什么走 SSBO 而不是 push constant：world + worldToLocal 两个 mat4
// 就已经 128 字节，而 push 的硬件下限正好是 128 —— 再想塞
// tint/opacity/尺寸/粗糙度就溢出了。立 SSBO 之后 push 只剩 4 字节下标。
//
// worldToLocal 由 **CPU** 求逆（glm::inverse）：片元里每个像素都求一次
// 4×4 逆矩阵是纯浪费，而且贴花通常是 TRS 组合，CPU 侧求逆还更稳定。
struct DecalRecord {
    glm::mat4 world;         //   0  贴花局部（单位立方体）→ 世界
    glm::mat4 worldToLocal;  //  64  世界 → 贴花**米空间**（不含 size 缩放）
    // 128  rgb = tint（乘在 albedo 上）, a = opacity（coverage 上限）
    glm::vec4 tintOpacity;
    // 144  xyz = 盒体尺寸（米，沿贴花局部轴）, w = 边缘软淡出比例
    glm::vec4 sizeEdge;
    // 160  x = roughness, y = metallic, z = normalScale, w = 保留
    glm::vec4 params;
    // 176  x = 距离淡出起点（米）, y = 距离淡出终点（米）, zw = 保留
    glm::vec4 fade;
};
static_assert(sizeof(DecalRecord) == 192, "DecalRecord 布局必须与 decal.frag 一致");

// 一条待渲染的贴花（CPU 侧描述，由 Renderer 每帧从 ECS 收集）。
struct DecalDrawItem {
    glm::mat4 world{1.0f};       // 已含 size 缩放（单位立方体 → 盒体）
    // 世界 → 贴花**米空间**（= inverse(不含 size 缩放的 TRS)）。
    // ⚠ 不能用 inverse(world) —— 那个带 size 缩放，反投影出来的是
    //   单位立方体坐标（[-0.5,0.5]³），而片元里"盒内测试"和"uv 投影"
    //   两处都按**米**写（|local| vs size/2、local.xy/size.xy）。
    //   两套空间混用的后果是盒体判定比真实体积大 6 倍 / uv 只采到
    //   贴图正中间的一小块。由 CPU 求逆（见 DecalBuffer.h 顶部说明）。
    glm::mat4 worldToLocal{1.0f};
    glm::vec3 size{1.0f, 1.0f, 1.0f};
    float opacity = 1.0f;
    glm::vec3 tint{1.0f};
    float roughness = 0.7f;
    float metallic = 0.0f;
    float normalScale = 1.0f;
    float edgeFade = 0.15f;
    float fadeStart = 0.0f;
    float fadeEnd = 0.0f;
    // 贴图传 **view/sampler**（不是 Texture*）：DecalBuffer 不该为了拿
    // 两张图去依赖 assets 模块的完整定义，也方便调用方替换缺省图。
    VkImageView albedoView = VK_NULL_HANDLE;
    VkSampler albedoSampler = VK_NULL_HANDLE;
    VkImageView normalView = VK_NULL_HANDLE;
    VkSampler normalSampler = VK_NULL_HANDLE;
};

class DecalBuffer {
public:
    // 一帧最多几张贴花。超出的直接丢弃并打日志 —— DBuffer 是单层，
    // 再多也只是把同一批像素反复覆盖，没有收益。
    static constexpr uint32_t kMaxDecals = 64;

    // 三张 DBuffer 的格式。都是 UNORM8：
    //   · 反照率 / 法线 / 粗糙度-金属度 都是**线性**量（法线是编码后的
    //     方向），做 sRGB 解码会把它们整体扳偏 —— 与"数据贴图必须
    //     srgb=false"是同一条道理。
    static constexpr VkFormat kAlbedoFormat = VK_FORMAT_R8G8B8A8_UNORM;
    static constexpr VkFormat kNormalFormat = VK_FORMAT_R8G8B8A8_UNORM;
    static constexpr VkFormat kRoughMetalFormat = VK_FORMAT_R8G8B8A8_UNORM;

    DecalBuffer(rhi::Device& device, rhi::CommandPool& commandPool,
                uint32_t framesInFlight);
    ~DecalBuffer();

    DecalBuffer(const DecalBuffer&) = delete;
    DecalBuffer& operator=(const DecalBuffer&) = delete;

    // 尺寸 / MSAA 变化时整组重建（三张图 + framebuffer + 管线）。
    void recreate(VkExtent2D extent, VkSampleCountFlagBits samples,
                  VkFormat depthFormat);

    // 每帧的场景深度可采样视图（HdrTarget::depthSampledView）。
    // HdrTarget 重建后视图换了 → 必须重新喂。
    void setDepthViews(const std::vector<VkImageView>& views);

    // 缺省贴图：albedo 为空时用白（alpha=255 → 纯色块），
    // normal 为空时用平坦法线（不扰动场景法线）。
    void setDefaultTextures(VkImageView albedoView, VkSampler albedoSampler,
                            VkImageView normalView, VkSampler normalSampler);

    // 上传本帧的贴花列表：写本帧的 record SSBO + 逐贴花的纹理描述符集。
    // frameUbo = 该帧的全局 UBO（binding 0 就是它，用于取 invViewProj /
    // camPos / screenSize）。**必须在录制命令缓冲之前调用**。
    void upload(VkBuffer frameUbo, uint32_t frameIndex,
                const std::vector<DecalDrawItem>& items);

    // 录制贴花 pass（前提：深度预通道已结束）。
    // 没上传任何贴花时也要调 —— 它负责把三张 DBuffer 清成 "coverage=0"。
    void render(VkCommandBuffer cmd, uint32_t frameIndex);

    // 本帧实际上传的贴花数（<= kMaxDecals）
    uint32_t count() const { return m_count; }
    uint32_t uploadedTotal() const { return m_uploadedTotal; }

    // which: 0 = 反照率, 1 = 世界法线, 2 = 粗糙度-金属度
    VkImageView view(uint32_t frameIndex, int which) const;
    // 前向 Pass 采样 DBuffer 用的线性 + CLAMP 采样器
    VkSampler sampler() const { return m_sampler; }

    bool ready() const { return m_ready; }

private:
    struct DecalFrame {
        VkImage albedoImage = VK_NULL_HANDLE;
        VkDeviceMemory albedoMemory = VK_NULL_HANDLE;
        VkImageView albedoView = VK_NULL_HANDLE;

        VkImage normalImage = VK_NULL_HANDLE;
        VkDeviceMemory normalMemory = VK_NULL_HANDLE;
        VkImageView normalView = VK_NULL_HANDLE;

        VkImage rmImage = VK_NULL_HANDLE;
        VkDeviceMemory rmMemory = VK_NULL_HANDLE;
        VkImageView rmView = VK_NULL_HANDLE;

        VkFramebuffer fb = VK_NULL_HANDLE;
    };

    void createRenderPass();
    void createPipelines();
    void destroyFrames();
    void createFrames();

    rhi::Device& m_device;
    rhi::CommandPool& m_commandPool;
    uint32_t m_framesInFlight = 2;

    VkExtent2D m_extent{0, 0};
    bool m_msaa = true;      // 深度图是否多采样（选哪份 .spv）
    bool m_ready = false;

    // 3 个颜色附件的 RenderPass：UNDEFINED 进（LOAD_OP_CLEAR 清成
    // "无贴花"）、SHADER_READ_ONLY 出（前向 Pass 要采样它）。
    VkRenderPass m_renderPass = VK_NULL_HANDLE;

    std::unique_ptr<rhi::Pipeline> m_pipeline;
    // 盒体几何：单位立方体（[-0.5,0.5]³），渲染器自建 —— 它不是场景资产。
    std::unique_ptr<assets::Mesh> m_cubeMesh;

    // set 0 = {FrameUBO, 深度图, 贴花记录 SSBO}
    VkDescriptorSetLayout m_frameSetLayout = VK_NULL_HANDLE;
    // set 1 = {albedo, normal}（逐贴花，按帧槽各一套）
    VkDescriptorSetLayout m_texSetLayout = VK_NULL_HANDLE;

    VkSampler m_sampler = VK_NULL_HANDLE;       // 采样 DBuffer（后处理用，线性）
    VkSampler m_depthSampler = VK_NULL_HANDLE;  // 采样深度（NEAREST，禁插值）
    VkSampler m_texSampler = VK_NULL_HANDLE;    // 采样贴花贴图（线性 + REPEAT）

    std::unique_ptr<rhi::DescriptorAllocator> m_descriptors;
    std::vector<VkDescriptorSet> m_frameSets;             // 每帧一个
    std::vector<std::vector<VkDescriptorSet>> m_texSets;  // [帧][贴花序号]

    // 每帧的贴花记录缓冲（HOST_VISIBLE 常驻映射）
    std::vector<std::unique_ptr<rhi::Buffer>> m_records;
    std::vector<void*> m_recordsMapped;

    std::vector<DecalFrame> m_frames;

    VkImageView m_defaultAlbedoView = VK_NULL_HANDLE;
    VkSampler m_defaultAlbedoSampler = VK_NULL_HANDLE;
    VkImageView m_defaultNormalView = VK_NULL_HANDLE;
    VkSampler m_defaultNormalSampler = VK_NULL_HANDLE;

    uint32_t m_count = 0;          // 本帧上传的贴花数
    uint32_t m_uploadedTotal = 0;  // 累计被丢弃前的请求数（诊断用）
};

} // namespace render
