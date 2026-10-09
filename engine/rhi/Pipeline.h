#pragma once
// ============================================================
// rhi/Pipeline —— 图形管线封装（shader + 顶点布局 + 管线布局）
// 视口/剪刀为动态状态，窗口 resize 无需重建管线
// ============================================================

#include "rhi/VulkanCommon.h"
#include <vector>

namespace rhi {

class Device;

struct PipelineDesc {
    std::vector<char> vertSpv;          // 顶点着色器字节码
    std::vector<char> fragSpv;          // 片元着色器字节码
    std::vector<VkVertexInputBindingDescription> vertexBindings;
    std::vector<VkVertexInputAttributeDescription> vertexAttributes;
    std::vector<VkDescriptorSetLayout> setLayouts;
    VkPushConstantRange pushConstant{}; // size==0 表示不用 push constant
    VkRenderPass renderPass = VK_NULL_HANDLE;
    uint32_t subpass = 0;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    bool depthTest = true;
    bool depthWrite = true;
    // 光栅化采样数。必须与所在 RenderPass 的颜色/深度附件采样数一致，
    // 否则 vkCreateGraphicsPipelines 直接报错（这条最容易被忽略）
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    // 颜色附件数量；设为 0 表示 depth-only 管线（阴影 Pass / 深度预通道）
    uint32_t colorAttachmentCount = 1;

    // ---- 透明混合 ----
    // 打开后走标准 alpha 混合：src*srcAlpha + dst*(1-srcAlpha)。
    // 透明物体必须 depthWrite=false，否则会把后面的物体挡掉。
    bool blendEnable = false;
    VkBlendFactor srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    VkBlendFactor dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    VkBlendOp colorBlendOp = VK_BLEND_OP_ADD;
    VkBlendFactor srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    VkBlendFactor dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    VkBlendOp alphaBlendOp = VK_BLEND_OP_ADD;
    // 深度偏移：渲染阴影图时把深度轻微推开，抵消 shadow acne（自阴影条纹）
    bool depthBiasEnable = false;
    float depthBiasConstant = 0.0f;
    float depthBiasSlope = 0.0f;
    VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;
    // 正面绕序：几何数据按"从外侧看 CCW"生成（见 assets/Mesh.cpp addFace），
    // 配合 Camera::projMatrix 的 Y 翻转，Vulkan 中正面即 COUNTER_CLOCKWISE
    VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
};

class Pipeline {
public:
    Pipeline(Device& device, const PipelineDesc& desc);
    ~Pipeline();

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    VkPipeline get() const { return m_pipeline; }
    VkPipelineLayout layout() const { return m_layout; }

private:
    static VkShaderModule createShaderModule(Device& device,
                                             const std::vector<char>& code);

    Device& m_device;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
};

} // namespace rhi
