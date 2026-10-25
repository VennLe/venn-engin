#pragma once
// ============================================================
// render/FrameResources —— 每帧在途资源（MAX_FRAMES_IN_FLIGHT 组）
// 每组包含：命令缓冲 / 栅栏 / 两个信号量 / 全局 UBO
// 由 Renderer 创建与销毁，这里只做聚合
// ============================================================

#include "rhi/VulkanCommon.h"
#include "rhi/Buffer.h"

#include <memory>
#include <vector>

namespace render {

struct FrameResources {
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkFence inFlightFence = VK_NULL_HANDLE;      // CPU-GPU 帧同步
    VkSemaphore imageAvailable = VK_NULL_HANDLE; // 获取图像完成
    // 注意：渲染完成信号量不在这里——它必须"每张交换链图像一个"，
    // 由 Renderer::m_renderFinished 管理，避免与呈现操作复用冲突

    std::unique_ptr<rhi::Buffer> globalUBO;      // HOST_VISIBLE，持久映射
    void* uboMapped = nullptr;
    VkDescriptorSet globalUBOSet = VK_NULL_HANDLE;  // set 0 绑定 UBO

    // 骨骼矩阵 SSBO（蒙皮网格用，set 0 binding 6）。HOST_VISIBLE 持久映射，
    // 每帧采样所有蒙皮模型的关节矩阵写入。容量按需扩展。
    std::unique_ptr<rhi::Buffer> jointMatrices;
    void* jointMapped = nullptr;
    uint32_t jointCapacity = 0;   // 当前能容纳的 mat4 数量

    // 局部光源阴影矩阵 SSBO（set 0 binding 11）。固定 kMaxLocalShadowLayers
    // 条 LightShadowRecord（逐层一个），容量小且恒定，同样常驻映射。
    // 与 jointMatrices 分开而不是接在它后面：两者的**更新时机不同**
    // （灯矩阵在 uploadSceneState 里写，骨骼在 updateSkinning 里写），
    // 挤在同一块 buffer 里会让两次写入互相踩。
    std::unique_ptr<rhi::Buffer> lightShadows;
    void* lightShadowsMapped = nullptr;

    // GPU instancing 的每实例 model 矩阵（set 0 binding 13）。HOST_VISIBLE
    // 持久映射；容量按需扩展（照 jointMatrices 的模式 —— 扩容换 buffer
    // 后必须 writeGlobalDescriptors 重写描述符）。主场景与预览场景的
    // 实例槽在同一块 buffer 里连续排布（游标见 Renderer::m_instanceOffset），
    // 槽序 = 批展开后的绘制序，深度预通道与前向 Pass 共用同一份。
    std::unique_ptr<rhi::Buffer> instanceModels;
    void* instanceMapped = nullptr;
    uint32_t instanceCapacity = 0;  // 当前能容纳的 InstanceData 个数

    // ============================================================
    // GPU 形变（§15）：三块 SSBO ——
    //   binding 25 MorphDeltas   delta 流（32 B/条，与磁盘布局逐字节一致）
    //   binding 26 MorphWeights  每实例一段 float（按 target 下标）
    //   binding 27 MorphSlots    每实例一条 {deltaBase, weightBase, targetCount}
    //
    // 为什么这三块**固定容量、永不重建**（与上面几块"按需扩容"不同）：
    // 扩容换 buffer 之后必须重写描述符，而 morph 数据是**边建绘制列表
    // 边追加**的 —— 一次扩容会发生在"主视口已经录完、预览场景还在追加"
    // 的时刻，那时主视口那些已录制的 draw 会去读一块（空的）新 buffer，
    // 表现为"主视口表情消失、预览正常"。固定容量 + 超限降级
    // （`morphBudgetHit`）不会有这个问题。
    //
    // 容量依据（§15.6 的实测账）：alicia 的 delta 流 566.6 KB、88 个
    // target、单个子网格最多 49 个 target。24 MB 能装 78 万个 delta，
    // 4 MB 权重能装 100 万个 float —— 相当于"场景里同时有 40 只不同的
    // 满表情角色"，超出这个量级就该走 LOD/流送，而不是继续加缓冲。
    // ============================================================
    std::unique_ptr<rhi::Buffer> morphDeltas;   // MorphDelta[]（32 B/条）
    void* morphDeltasMapped = nullptr;
    std::uint32_t morphDeltaCapacity = 0;       // 能容纳的 delta 条数
    std::unique_ptr<rhi::Buffer> morphWeights;  // float[]（每实例一段）
    void* morphWeightsMapped = nullptr;
    std::uint32_t morphWeightCapacity = 0;      // 能容纳的 float 个数
    std::unique_ptr<rhi::Buffer> morphSlots;    // MorphInstance[]（16 B/个）
    void* morphSlotsMapped = nullptr;
    std::uint32_t morphSlotCapacity = 0;        // 能容纳的实例数

    // ============================================================
    // 离屏目标各自的相机 UBO（每个离屏目标一份）
    //
    // FrameUBO 是 HOST_VISIBLE + 常驻映射、在**录制期** memcpy 的：
    // 一个命令缓冲里若先后录制"主视口"与"预览/额外视角"两个场景，
    // 两者需要的相机矩阵不同，而 memcpy 只有最后一次能留在 buffer 里，
    // GPU 执行时**先录制的那个场景也会读到后一次的值** —— 主视口会被
    // 渲染成预览相机的视角。所以离屏目标必须各持一份 UBO + 各自的
    // set 0（其余 14 个 binding 与主链共享同一批每帧资源）。
    //
    // 下标 = 离屏目标序号（Renderer::m_targets 里的序号 - 1），懒建。
    // ============================================================
    struct OffscreenUbo {
        std::unique_ptr<rhi::Buffer> ubo;
        void* mapped = nullptr;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };
    std::vector<OffscreenUbo> offscreenUbos;
};

} // namespace render
