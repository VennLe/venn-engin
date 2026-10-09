#include "rhi/Pipeline.h"
#include "rhi/Device.h"

namespace rhi {

Pipeline::Pipeline(Device& device, const PipelineDesc& desc) : m_device(device) {
    VkDevice dev = m_device.get();

    VkShaderModule vert = createShaderModule(device, desc.vertSpv);
    VkShaderModule frag = createShaderModule(device, desc.fragSpv);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount =
        static_cast<uint32_t>(desc.vertexBindings.size());
    vertexInput.pVertexBindingDescriptions = desc.vertexBindings.data();
    vertexInput.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(desc.vertexAttributes.size());
    vertexInput.pVertexAttributeDescriptions = desc.vertexAttributes.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = desc.topology;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    // 动态视口/剪刀
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.depthClampEnable = VK_FALSE;
    raster.rasterizerDiscardEnable = VK_FALSE;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = desc.cullMode;
    raster.frontFace = desc.frontFace;
    raster.depthBiasEnable = desc.depthBiasEnable ? VK_TRUE : VK_FALSE;
    raster.depthBiasConstantFactor = desc.depthBiasConstant;
    raster.depthBiasSlopeFactor = desc.depthBiasSlope;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType =
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = desc.samples;
    multisample.sampleShadingEnable = VK_FALSE;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType =
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = desc.depthTest ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = desc.depthWrite ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    // 不透明默认关闭混合；透明材质打开标准 alpha 混合
    blendAttachment.blendEnable = desc.blendEnable ? VK_TRUE : VK_FALSE;
    blendAttachment.srcColorBlendFactor = desc.srcColorBlendFactor;
    blendAttachment.dstColorBlendFactor = desc.dstColorBlendFactor;
    blendAttachment.colorBlendOp = desc.colorBlendOp;
    blendAttachment.srcAlphaBlendFactor = desc.srcAlphaBlendFactor;
    blendAttachment.dstAlphaBlendFactor = desc.dstAlphaBlendFactor;
    blendAttachment.alphaBlendOp = desc.alphaBlendOp;
    blendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.logicOpEnable = VK_FALSE;
    // colorAttachmentCount == 0 → depth-only 管线，颜色混合状态必须为空
    blend.attachmentCount = desc.colorAttachmentCount;
    blend.pAttachments =
        desc.colorAttachmentCount > 0 ? &blendAttachment : nullptr;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                  VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynStates;

    // 管线布局
    VkPipelineLayoutCreateInfo layoutCI{};
    layoutCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCI.setLayoutCount = static_cast<uint32_t>(desc.setLayouts.size());
    layoutCI.pSetLayouts =
        desc.setLayouts.empty() ? nullptr : desc.setLayouts.data();
    layoutCI.pushConstantRangeCount = desc.pushConstant.size > 0 ? 1 : 0;
    layoutCI.pPushConstantRanges =
        desc.pushConstant.size > 0 ? &desc.pushConstant : nullptr;
    VK_CHECK(vkCreatePipelineLayout(dev, &layoutCI, nullptr, &m_layout));

    VkGraphicsPipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vertexInput;
    ci.pInputAssemblyState = &inputAssembly;
    ci.pViewportState = &viewportState;
    ci.pRasterizationState = &raster;
    ci.pMultisampleState = &multisample;
    ci.pDepthStencilState = &depthStencil;
    ci.pColorBlendState = &blend;
    ci.pDynamicState = &dynamic;
    ci.layout = m_layout;
    ci.renderPass = desc.renderPass;
    ci.subpass = desc.subpass;

    VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr,
                                       &m_pipeline));

    vkDestroyShaderModule(dev, vert, nullptr);
    vkDestroyShaderModule(dev, frag, nullptr);
}

Pipeline::~Pipeline() {
    if (m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device.get(), m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_device.get(), m_layout, nullptr);
        m_layout = VK_NULL_HANDLE;
    }
}

VkShaderModule Pipeline::createShaderModule(Device& device,
                                            const std::vector<char>& code) {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(device.get(), &ci, nullptr, &module));
    return module;
}

} // namespace rhi
