#pragma once
// ============================================================
// render/TerrainPass —— 地形渲染（W4-2，按 D5）
//
// ------------------------------------------------------------
// 为什么是**独立管线 + 自己的 set**
// ------------------------------------------------------------
// 前向管线的纹理槽是 7 个（`kTextureSetCount`），已经用满。地形 4 层需要
// 4 albedo + 4 normal + 1 权重 + 1 高度 = 10 张，塞不进去。理论上可以把
// set 加到 9 个（本机 maxBoundDescriptorSets = 32），但低端设备只有 8，
// 而"8"已经是本引擎踩在线的实际要求。所以：
//
//   · 权重打包成**一张 RGBA8**（4 层各 1 通道）
//   · 地形的全部材质面走**一个** set（set 1），一次绑定画完整块地形
//
// ------------------------------------------------------------
// ⚠ 它还是"前向 Pass 内的一条独立管线"，不是独立 Pass
// ------------------------------------------------------------
// 地形和主几何共享同一个 RenderPass / 深度附件 / MSAA 设置，只是管线与
// 描述符布局不同。这样做有三个好处：
//   · 深度预通道写过的深度直接生效（地形被木箱正确遮挡）
//   · 不需要多一份 resolve / 多一次 load-store（省 2 次全屏带宽）
//   · 透明队列仍然排在它之后，混合顺序不变
//
// 但**管线布局**必须与主前向对 set 0 **兼容**：set 0 是整个 Pass 一次性
// 绑定的，中途出现一个不兼容的 vkCmdBindDescriptorSets 会让 Vulkan 认为
// set 0 被"打断"，后面所有 draw 报 VUID-...-08600。所以这里声明
//   · set 0 用**同一份** VkDescriptorSetLayout
//   · push constant range 与主前向**逐字段相同**（offset 0 / 128 / VERTEX|FRAGMENT）
// 虽然地形自己只推 96 字节（TerrainPush），但 range 必须写满 128。
//
// ------------------------------------------------------------
// 裙边与 LOD
// ------------------------------------------------------------
// 每块地形预生成 4 级网格（块内按 1/2/4/8 格减采样），每级都带四条向下
// 拉 2 m 的裙边。任意相邻块 LOD 差都能被裙边兜住，不需要 quad-tree 的
// "过渡三角形"（那是另一种减面逻辑，地形不该再引入一套）。
// ============================================================

#include "assets/Texture.h"
#include "rhi/Buffer.h"
#include "rhi/Image.h"
#include "rhi/VulkanCommon.h"
#include "terrain/Terrain.h"

#include <glm/glm.hpp>

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace rhi {
class CommandPool;
class DescriptorAllocator;
class Device;
class Pipeline;
} // namespace rhi

namespace render {

// 与 assets/shaders/terrain.vert / .frag 的 push_constant **逐字段一致**。
// 96 字节（管线布局里声明的 range 是 128，见文件头）。
struct TerrainPush {
    glm::mat4 model;   //  0
    glm::vec4 params;  // 64  x=maxHeight y=splatRepeat z=layerTiling w=metersPerPixel
    glm::vec4 params2; // 80  x=cols y=rows z=skirtDepth w=层 normal 贴图位掩码
};
static_assert(sizeof(TerrainPush) == 96, "TerrainPush 布局变了，必须同步 terrain.*");

// 与 shader 里的 TerrainLayer 一致（std140，每个 32 字节）
struct TerrainLayerGpu {
    glm::vec4 baseColor;
    glm::vec4 params;  // x=roughness y=metallic z=ao w=normalScale
};
static_assert(sizeof(TerrainLayerGpu) == 32, "TerrainLayerGpu 布局变了");

class TerrainPass {
public:
    struct InitInfo {
        rhi::Device* device = nullptr;
        rhi::CommandPool* cmdPool = nullptr;
        // 主前向的 set 0（**同一份**对象，不是"等价的布局"）
        VkDescriptorSetLayout globalSetLayout = VK_NULL_HANDLE;
        VkRenderPass renderPass = VK_NULL_HANDLE;
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        // 建贴图要用（由 Renderer::textureContext() 提供）
        assets::TextureContext texCtx;
    };

    TerrainPass() = default;
    ~TerrainPass();
    TerrainPass(const TerrainPass&) = delete;
    TerrainPass& operator=(const TerrainPass&) = delete;

    bool init(const InitInfo& info);
    void shutdown();

    // 换一份贴图上下文（Renderer::textureContext()）。
    // 为什么不在 init 里一次性拿到：`m_textureSetLayout` 在 Renderer::init
    // 里是**建完管线之后**才创建的（材质贴图的 set 布局），而地形实例
    // 要到场景加载时才建 —— 那时它已经有效了。所以每次建实例之前刷一次。
    void setTextureContext(const assets::TextureContext& ctx) { m_texCtx = ctx; }

    // 只重建管线（MSAA 采样数或 HDR RenderPass 变了时用）。
    // 实例的缓冲/纹理/描述符集**不动** —— 它们与管线无关，
    // 重建一次要重新上传 3 MB 顶点数据，不值得。
    void recreatePipeline(VkRenderPass renderPass,
                          VkSampleCountFlagBits samples);

    // 已经 init 过且管线建成功（管线是惰性/一次性创建的）
    bool ready() const { return m_pipeline != nullptr; }

    // 从 `.vterrain` 建一个地形实例。失败返回 nullptr（原因进日志）。
    // model 只允许"绕 Z 旋转 + 平移 + 等比缩放"：高度沿本地 +Z 加，
    // 一旦地形被倾斜，渲染出的表面就与物理的高度场对不上了。
    struct Instance;
    // overrides：4 层材质 id 覆盖（nullptr 或空串 = 用 `.vterrain` 里的）。
    // heightScale：乘在 maxHeight 上（渲染与物理**必须**读同一个值）。
    Instance* createTerrain(const std::string& relPath, const glm::mat4& model,
                            float heightScale = 1.0f,
                            const std::array<std::string, 4>* overrides = nullptr);

    uint32_t instanceCount() const {
        return static_cast<uint32_t>(m_instances.size());
    }
    Instance* instanceAt(uint32_t i) {
        return (i < m_instances.size()) ? m_instances[i].get() : nullptr;
    }
    // 全部拆掉（场景里地形实体的集合变了，或资产/层材质被改过）
    void clearInstances();

    // ---- 笔刷专用：原地重传高度图 / 权重图 ----
    //
    // 高度在 terrain.vert 里是采样高度图加出来的，顶点本身是平坦格子 ——
    // 所以刷地形不需要重建任何对象，只要把这张 R32 图重传一遍。
    // 与 clearInstances + createTerrain 的区别：**不销毁任何 VkObject**、
    // 不重传 3 MB 顶点、不重建 sampler / 描述符集，因此可以安全地按
    // 十几赫兹的频率调用；而重建路径必须等 GPU 空闲，高频调用会撞上
    // "destroy while in use" → VK_ERROR_DEVICE_LOST。
    //
    // index 是实例下标（与 instanceAt / Renderer::m_terrainEntities 同序）。
    // splatToo = false 时只刷高度图（纯抬升/平滑不需要碰权重）。
    // holesToo = false 时不动挖洞遮罩（只有 Hole 笔刷才置 true）。
    // 失败返回 false（索引越界 / 资产尺寸变了 → 调用方应退回重建路径）。
    bool refreshSurface(uint32_t index, bool splatToo, bool holesToo = false);

    // 在**前向 Pass 内**录制（调用方负责 RenderPass 的 begin/end 与 set 0）。
    // 返回本帧提交的三角形数（统计用）。
    uint64_t record(VkCommandBuffer cmd, const glm::vec3& camPos);

    // 最近一次 LOD 分布（verify 用例读日志里的 TERRAIN-LOD 行）
    struct LodStats {
        uint32_t chunks[terrain::kLodCount] = {0, 0, 0, 0};
        uint32_t total = 0;
        uint64_t triangles = 0;
    };
    const LodStats& lastLodStats() const { return m_lastLod; }

private:
    void createPipeline();
    bool createInstanceResources(Instance& inst);

    rhi::Device* m_device = nullptr;
    rhi::CommandPool* m_cmdPool = nullptr;
    assets::TextureContext m_texCtx;
    // 建管线时要用的三样东西（init 时存下来，采样数变化后重建也靠它们）
    VkDescriptorSetLayout m_globalSetLayout = VK_NULL_HANDLE;
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkSampleCountFlagBits m_samples = VK_SAMPLE_COUNT_1_BIT;

    // set 1 的布局（地形专用）
    VkDescriptorSetLayout m_terrainSetLayout = VK_NULL_HANDLE;
    std::unique_ptr<rhi::DescriptorAllocator> m_descriptors;
    std::unique_ptr<rhi::Pipeline> m_pipeline;

    // 缺省贴图：albedo = 白，normal = (0.5,0.5,1)
    std::unique_ptr<assets::Texture> m_defaultAlbedo;
    std::unique_ptr<assets::Texture> m_defaultNormal;

    std::vector<std::unique_ptr<Instance>> m_instances;
    LodStats m_lastLod;
    bool m_loggedLod = false;
};

// 一个地形实体在 GPU 上的全部资源
struct TerrainPass::Instance {
    terrain::TerrainAsset asset;
    std::vector<terrain::TerrainChunk> chunks;
    glm::mat4 model{1.0f};

    // 全部块 × 全部 LOD 拼进**一对**大缓冲（每个块-LOD 一段索引区间）。
    // 每块每级一个 VkBuffer 会让 draw 之间的绑定次数翻 64 倍，而它们
    // 本来共享同一个 vertex 布局。
    std::unique_ptr<rhi::Buffer> vbo;
    std::unique_ptr<rhi::Buffer> ibo;
    struct Range {
        uint32_t firstIndex = 0;
        uint32_t indexCount = 0;
    };
    std::vector<Range> ranges;  // [chunkIdx * kLodCount + lod]
    float heightScale = 1.0f;   // 乘在 maxHeight 上（每帧从组件同步）

    std::unique_ptr<rhi::Image> heightImage;
    std::unique_ptr<rhi::Image> splatImage;
    // 挖洞遮罩（R8_UNORM，每采样点 1 字节）。**永远存在** ——
    // 没挖过洞的资产由 loadTerrain 填 255 生成一张全白图，
    // 这样片元着色器里只有一条无条件采样 + 比较，没有"有没有洞图"的分支。
    std::unique_ptr<rhi::Image> holeImage;
    VkSampler heightSampler = VK_NULL_HANDLE;
    VkSampler splatSampler = VK_NULL_HANDLE;
    VkSampler holeSampler = VK_NULL_HANDLE;

    // 层材质：数值参数走 UBO（一次写入），贴图走描述符数组
    struct Layer {
        glm::vec4 baseColor{1.0f};
        glm::vec4 params{0.8f, 0.0f, 1.0f, 1.0f};
        std::unique_ptr<assets::Texture> albedo;
        std::unique_ptr<assets::Texture> normal;
        std::string id;
    };
    std::array<Layer, 4> layers;
    std::unique_ptr<rhi::Buffer> layerUbo;
    VkDescriptorSet texSet = VK_NULL_HANDLE;
    uint32_t normalMask = 0;  // bit i = 第 i 层挂了 normal 贴图

    float chunkSizeMeters = 0.0f;
};

} // namespace render
