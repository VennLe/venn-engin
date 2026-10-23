#pragma once
// ============================================================
// render/MdiDraw —— Multi-draw indirect + GPU 紧凑化（R2-2）
//
// 解决的问题：合批（R1 之前的 MYVK_INSTANCING）已经把"一物一 draw"
// 压成"一批一 draw"，但 CPU 仍要**按批**发命令 —— 场景里材质/网格一多，
// 提交量还是 O(批数)。本模块把提交量压到 **O(同管线 + 同材质的组数)**：
//
//   深度预通道（本帧全部候选，旧路径，不变）
//        │
//        ├─ Hi-Z 金字塔（复用 R2-1 的 hiz_build / hiz_downsample）
//        ├─ mdi_compact.comp  每个候选：遮挡测试 → 可见则
//        │        atomicAdd(间接命令.instanceCount, 1) 抢槽位
//        │        + 把结构化实例记录写进紧凑化实例表
//        └─ vkCmdDrawIndexedIndirect  **每组一次**（组 = 同管线 + 同材质）
//
// 与 R2-1 的差别：R2-1 把可见性**回读到 CPU**、延迟两帧应用；R2-2 让
// GPU 自己紧凑化并写 draw 参数，CPU 只发"组数量级"的命令，且因为
// 紧凑化排在本帧前向之前，**零帧延迟**（没有相机运动时的 pop）。
//
// ---- 材质为什么还能走 push 常量 ----
// 一次 vkCmdDrawIndexedIndirect 覆盖的是一组**同管线 + 同材质**的批：
// set 1..6 的贴图与 push 里的材质参数在组内恒定，所以不需要把材质搬进
// 缓冲（那是 R2-3 bindless 的活儿）。每实例只有 world matrix + aux
// （蒙皮 jointBase）不同。
//
// ---- 默认关 ----
//   关闭时本模块一个 compute 都不录、一个间接命令都不发 →
//   与引入前逐像素相同（回归硬要求）。
// ============================================================

#include "rhi/VulkanCommon.h"
#include "render/RenderTypes.h"
#include "render/ShaderKeywords.h"   // KeywordMask / kKeywordNone

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace rhi {
class Device;
class CommandPool;
class ComputePipeline;
class Buffer;
class DescriptorAllocator;
} // namespace rhi

namespace assets {
struct Material;
class Mesh;
} // namespace assets

namespace render {

// 每个实例的结构化记录（std430，80 字节）。
// 必须与 assets/shaders/pbr.vert 的 MdiInstance、mdi_compact.comp 的
// MdiInstance 逐字段一致（mat4 在 std430 下的 stride 就是 64）。
struct MdiInstance {
    glm::mat4 model{1.0f};  // 0
    glm::ivec4 aux{0};      // 64  x = jointBase（<0 表示静态网格）
};
static_assert(sizeof(MdiInstance) == 80, "MdiInstance 必须与着色器布局一致");

// 一条间接绘制命令 —— 与 VkDrawIndexedIndirectCommand 逐字段一致（20 B）。
// 单独定义一份是为了让 CPU 侧能直接填静态字段、并以裸 uint 视图交给
// mdi_compact.comp 做 atomicAdd（见着色器顶部注释）。
struct MdiDrawCommand {
    uint32_t indexCount = 0;     //  0
    uint32_t instanceCount = 0;  //  4  ← GPU 用 atomicAdd 累加
    uint32_t firstIndex = 0;     //  8
    int32_t vertexOffset = 0;    // 12
    uint32_t firstInstance = 0;  // 16
};
static_assert(sizeof(MdiDrawCommand) == 20,
              "必须与 VkDrawIndexedIndirectCommand 逐字节一致");

// 一次 vkCmdDrawIndexedIndirect = 一组"同管线 + 同材质 + 同网格"的批。
// firstCommand / commandCount 给出该组在间接缓冲里的连续区段。
//
// ⚠ 为什么把**网格**也算进组键：vkCmdDrawIndexedIndirect 一次只带一套
// 已绑定的顶点/索引缓冲（VkDrawIndexedIndirectCommand 里的 firstIndex /
// vertexOffset 只能在该缓冲内寻址），而本引擎每个 Mesh 各有自己的 VB/IB。
// 组内混网格 → 后一条命令会拿前一条的缓冲去解释自己的 firstIndex，
// 画出顶点数据错位的几何。批表本来就按 (掩码, 材质, 网格) 排过序，
// 所以"同材质+同网格"的批天然连续，加这一项不产生额外分组。
struct MdiSubmitGroup {
    KeywordMask keywords = kKeywordNone;
    const assets::Material* material = nullptr;
    const assets::Mesh* mesh = nullptr;
    uint32_t firstCommand = 0;
    uint32_t commandCount = 0;
};

struct MdiStats {
    uint32_t candidates = 0;     // 本帧候选物体数（= 间接命令覆盖的实例上界）
    uint32_t batches = 0;        // 本帧的批数（= 间接命令条数）
    uint32_t submissions = 0;    // 本帧发的 vkCmdDrawIndexedIndirect 次数
    uint32_t instancesDrawn = 0; // 回读：上一轮真正画出的实例数
    uint32_t culled = 0;         // 回读：候选 - 画出
};

class MdiDraw {
public:
    // 与 HiZOcclusion 同为 8192：两边索引必须一一对应。
    static constexpr uint32_t kMaxObjects = 8192;
    static constexpr uint32_t kMaxBatches = 4096;

    MdiDraw(rhi::Device& device, rhi::CommandPool& cmdPool,
            uint32_t framesInFlight);
    ~MdiDraw();

    MdiDraw(const MdiDraw&) = delete;
    MdiDraw& operator=(const MdiDraw&) = delete;

    // Hi-Z 金字塔视图 + 候选 AABB 缓冲由 HiZOcclusion 拥有，重建时
    // （分辨率 / HdrTarget 变化）要重新喂一遍。
    void setInputs(const std::vector<VkImageView>& hizViews,
                   const std::vector<VkBuffer>& aabbBuffers);

    // 一切就绪（有 Hi-Z 视图、AABB 缓冲、缓冲容量够）才返回 true。
    bool ready() const { return m_ready; }

    // 每帧（buildDrawLists 尾部）：上传 catalog / objBatch / 命令静态字段，
    // 并把 instanceCount 清零。返回 false = 超出容量（本帧退回旧路径）。
    bool beginFrame(uint32_t frameIndex,
                    const std::vector<MdiInstance>& catalog,
                    const std::vector<uint32_t>& objBatch,
                    const std::vector<MdiDrawCommand>& commands,
                    const std::vector<MdiSubmitGroup>& groups);

    // in-flight 栅栏之后：回读上一轮写进间接缓冲的 instanceCount 合计。
    void readBack(uint32_t frameIndex);
    const MdiStats& stats() const { return m_stats; }

    // 录制紧凑化（遮挡测试 + 紧凑化 + 写 draw 参数）。
    // 前提：深度预通道已结束、Hi-Z 金字塔已建好（本帧同一命令缓冲内）。
    void compact(VkCommandBuffer cmd, uint32_t frameIndex,
                 const glm::mat4& viewProj, float depthBias, float maxLod,
                 VkExtent2D extent, uint32_t objectCount);

    // 前向 pass 用
    VkBuffer indirectBuffer(uint32_t frameIndex) const;
    const std::vector<MdiSubmitGroup>& groups() const { return m_groups; }
    uint32_t objectCount() const { return m_objectCount; }

    // 紧凑化实例表（GPU 写、顶点着色器经由 set 0 binding 15 读）。
    // 缓冲在构造时按 kMaxObjects 一次分配到底，所以它**永不换对象** ——
    // binding 15 只要在 init / recreate 时写一次就行（否则每次扩容都要
    // 回头重写 set 0，那是这条路径最容易出错的地方）。
    VkBuffer instancesBuffer(uint32_t frameIndex) const;
    VkDeviceSize instancesSize() const {
        return static_cast<VkDeviceSize>(kMaxObjects) * sizeof(MdiInstance);
    }

private:
    struct FrameData {
        std::unique_ptr<rhi::Buffer> catalog;   // CPU 写（源目录）
        std::unique_ptr<rhi::Buffer> objBatch;  // CPU 写（物体 → 批号）
        std::unique_ptr<rhi::Buffer> instances; // GPU 写（紧凑化实例表）
        std::unique_ptr<rhi::Buffer> indirect;  // CPU 静态 + GPU 写 instanceCount
        void* catalogMapped = nullptr;
        void* objBatchMapped = nullptr;
        void* indirectMapped = nullptr;
        VkDescriptorSet set = VK_NULL_HANDLE;
        uint32_t commandCount = 0;
        uint32_t lastObjectCount = 0;
    };

    void createPipeline();
    void createBuffers();
    void writeDescriptors();
    void destroyDescriptors();
    void destroyFrames();

    rhi::Device& m_device;
    rhi::CommandPool& m_cmdPool;
    uint32_t m_framesInFlight = 2;
    bool m_ready = false;

    std::unique_ptr<rhi::ComputePipeline> m_pipeline;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    std::unique_ptr<rhi::DescriptorAllocator> m_descriptors;
    std::vector<FrameData> m_frames;

    // Hi-Z 视图 / AABB 缓冲（HiZOcclusion 拥有，这里只记引用）
    std::vector<VkImageView> m_hizViews;
    std::vector<VkBuffer> m_aabbBuffers;

    std::vector<MdiSubmitGroup> m_groups;
    uint32_t m_objectCount = 0;
    MdiStats m_stats;
};

} // namespace render
