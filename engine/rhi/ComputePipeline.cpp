#include "rhi/ComputePipeline.h"

#include "rhi/Device.h"

namespace rhi {

ComputePipeline::ComputePipeline(
    Device& device, const std::vector<char>& computeSpv,
    const std::vector<VkDescriptorSetLayout>& setLayouts,
    const VkPushConstantRange* pushConstant)
    : m_device(device) {
    VkDevice dev = m_device.get();

    VkShaderModuleCreateInfo smci{};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = computeSpv.size();
    smci.pCode = reinterpret_cast<const uint32_t*>(computeSpv.data());

    VkShaderModule module = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(dev, &smci, nullptr, &module));

    VkPipelineLayoutCreateInfo layoutCI{};
    layoutCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCI.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    layoutCI.pSetLayouts = setLayouts.empty() ? nullptr : setLayouts.data();
    layoutCI.pushConstantRangeCount = pushConstant ? 1 : 0;
    layoutCI.pPushConstantRanges = pushConstant;
    VK_CHECK(vkCreatePipelineLayout(dev, &layoutCI, nullptr, &m_layout));

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName = "main";

    VkComputePipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage = stage;
    ci.layout = m_layout;

    VK_CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr,
                                      &m_pipeline));

    vkDestroyShaderModule(dev, module, nullptr);
}

ComputePipeline::~ComputePipeline() {
    if (m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device.get(), m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_device.get(), m_layout, nullptr);
        m_layout = VK_NULL_HANDLE;
    }
}

void ComputePipeline::dispatch(VkCommandBuffer cmd, uint32_t groupsX,
                               uint32_t groupsY, uint32_t groupsZ,
                               const VkDescriptorSet* sets, uint32_t setCount,
                               const void* pushData, uint32_t pushSize) const {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    if (setCount > 0) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_layout,
                                0, setCount, sets, 0, nullptr);
    }
    if (pushData && pushSize > 0) {
        vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           pushSize, pushData);
    }
    vkCmdDispatch(cmd, groupsX, groupsY, groupsZ);
}

} // namespace rhi
