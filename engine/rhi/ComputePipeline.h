#pragma once
// ============================================================
// rhi/ComputePipeline —— 计算管线封装（compute shader + 管线布局）
//
// 与 Pipeline（图形管线）分开成两个类的原因：
//   计算管线没有顶点输入 / 光栅化 / 深度 / 混合这一整套状态，
//   硬塞进 PipelineDesc 会得到一堆对计算无意义的字段。
//
// 分簇光照的 cluster_build.comp 是本类目前唯一的使用者。
// ============================================================

#include "rhi/VulkanCommon.h"
#include <vector>

namespace rhi {

class Device;

class ComputePipeline {
public:
    // pushConstant 传 nullptr 表示不需要推送常量
    ComputePipeline(Device& device, const std::vector<char>& computeSpv,
                    const std::vector<VkDescriptorSetLayout>& setLayouts,
                    const VkPushConstantRange* pushConstant = nullptr);
    ~ComputePipeline();

    ComputePipeline(const ComputePipeline&) = delete;
    ComputePipeline& operator=(const ComputePipeline&) = delete;

    VkPipeline get() const { return m_pipeline; }
    VkPipelineLayout layout() const { return m_layout; }

    // 便捷派发：绑定管线 + 描述符集 + （可选）推送常量，再按 local_size 派发。
    // 推送常量必须在 vkCmdDispatch **之前**写入，所以这里一并处理，
    // 不给调用方留"忘了推常量就派发"的机会。
    void dispatch(VkCommandBuffer cmd, uint32_t groupsX, uint32_t groupsY,
                  uint32_t groupsZ, const VkDescriptorSet* sets,
                  uint32_t setCount, const void* pushData = nullptr,
                  uint32_t pushSize = 0) const;

private:
    Device& m_device;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
};

} // namespace rhi
