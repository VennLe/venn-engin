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
    // Alpha-to-coverage（R4-1）：把片元 alpha 转成逐采样覆盖掩码，让
    // alpha 裁切/透贴的边缘在 MSAA 下得到硬件抗锯齿。**只能在 samples > 1
    // 时开**（Vulkan 硬性规定，1x 下开会让 vkCreateGraphicsPipelines 报错）。
    bool alphaToCoverage = false;
    // 多边形模式：FILL（默认）/ LINE（线框，需 fillModeNonSolid 特性）。
    // Wireframe 视图模式（R0-2）用它 —— LINE 模式下片元照样跑 pbr.frag
    // （输出固定色），深度测试关闭以透视全部边线。
    VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;
    VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;
    // 正面绕序：几何数据按"从外侧看 CCW"生成（见 assets/Mesh.cpp addFace），
    // 配合 Camera::projMatrix 的 Y 翻转，Vulkan 中正面即 COUNTER_CLOCKWISE
    VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    // ---- 共用管线布局（可选）----
    // 非空时 Pipeline 直接用它，且**不拥有、不销毁**（调用方负责生命周期）。
    // 用途：着色器变体族（render/VariantPipelines）。变体之间只有片元逻辑
    // 与固定状态不同，描述符集与 push constant 范围完全一致 —— 共用一份
    // 布局既省对象，也让"取布局"这件事不再需要先建出一个管线来。
    VkPipelineLayout sharedLayout = VK_NULL_HANDLE;
};

// 按 desc 里的 setLayouts / pushConstant 建一个管线布局。
// 单独暴露出来是为了让变体族能在**还没建任何管线**的时候就拿到布局
// （vkCmdBindDescriptorSets 在第一次绘制之前就要用它）。
VkPipelineLayout createPipelineLayout(Device& device, const PipelineDesc& desc);

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
    // false = m_layout 是 desc.sharedLayout 借来的，析构时不销毁。
    bool m_ownsLayout = true;
};

} // namespace rhi
