#include "render/RenderTarget.h"

#include "render/HdrTarget.h"
#include "render/PostProcess.h"
#include "ui/ImGuiManager.h"

#include "rhi/Device.h"

#include <backends/imgui_impl_vulkan.h>

namespace render {

RenderTarget::RenderTarget(rhi::Device& device, VkFormat swapFormat,
                           uint32_t framesInFlight,
                           VkSampleCountFlagBits samples,
                           ui::ImGuiManager* imgui)
    : m_device(device), m_swapFormat(swapFormat),
      m_framesInFlight(framesInFlight > 0 ? framesInFlight : 1),
      m_samples(samples), m_imgui(imgui) {}

RenderTarget::~RenderTarget() {
    m_device.waitIdle();
    destroyTextures();
    m_post.reset();
    m_hdr.reset();
}

void RenderTarget::setSize(uint32_t w, uint32_t h) {
    // 面板被折叠 / 拖到极小时给个下限，避免 0 尺寸图像。
    // 0 是"关闭离屏模式"的合法值（回到直写交换链），不夹。
    if (w > 0 && w < 16) w = 16;
    if (h > 0 && h < 16) h = 16;
    const bool off = (w > 0 && h > 0);
    if (w == m_width && h == m_height && off == m_offscreen) return;
    m_width = w;
    m_height = h;
    m_offscreen = off;
    m_dirty = true;
}

VkExtent2D RenderTarget::extent() const {
    if (m_hdr) return m_hdr->extent();
    if (m_width == 0 || m_height == 0) return VkExtent2D{1, 1};
    return VkExtent2D{m_width, m_height};
}

void RenderTarget::apply(const std::vector<VkImageView>& swapViews,
                         VkExtent2D swapExtent,
                         const std::vector<VkImageView>& postInputViews) {
    m_dirty = false;
    m_device.waitIdle();

    if (!m_hdr) {
        m_hdr = std::make_unique<HdrTarget>(m_device, m_framesInFlight,
                                            m_samples);
    }
    if (!m_post) {
        m_post = std::make_unique<PostProcess>(m_device, m_swapFormat,
                                               m_hdr->colorFormat(),
                                               m_framesInFlight);
    }

    // 离屏模式：HDR 按 (w,h)；直写交换链模式：HDR 跟交换链尺寸，
    // 但 PostProcess 的 viewportExtent 传 0（模式 A）。
    // DRS（R1-4）：HDR / 深度按**缩放后**的渲染分辨率分配；而 PostProcess
    // 收到的 viewportExtent 仍是全分辨率的输出尺寸（它内部再乘 scale）。
    // 两处都用 scaledRenderExtent()，保证 HdrTarget 与 PostProcess 的
    // m_sceneExtent 逐位一致。
    const VkExtent2D outExt =
        m_offscreen ? VkExtent2D{m_width, m_height} : swapExtent;
    const VkExtent2D postViewport =
        m_offscreen ? VkExtent2D{m_width, m_height} : VkExtent2D{0, 0};
    const float scale = m_post ? m_post->renderScale() : 1.0f;
    const VkExtent2D hdrExt = scaledRenderExtent(outExt, scale);

    m_hdr->recreate(hdrExt.width > 0 ? hdrExt.width : 1,
                    hdrExt.height > 0 ? hdrExt.height : 1);
    // 设备可能不支持请求的采样数 → 以实际选中的为准
    m_samples = m_hdr->samples();

    std::vector<VkImageView> inputs = postInputViews;
    if (inputs.empty()) {
        for (uint32_t i = 0; i < m_hdr->framesInFlight(); ++i)
            inputs.push_back(m_hdr->colorView(i));
    }
    m_post->recreate(swapViews, swapExtent, inputs, postViewport);
    refreshTextures();
}

void RenderTarget::recreatePost(const std::vector<VkImageView>& swapViews,
                                VkExtent2D swapExtent,
                                const std::vector<VkImageView>& postInputViews) {
    if (!m_post || !m_hdr) return;
    m_device.waitIdle();

    std::vector<VkImageView> inputs = postInputViews;
    if (inputs.empty()) {
        for (uint32_t i = 0; i < m_hdr->framesInFlight(); ++i)
            inputs.push_back(m_hdr->colorView(i));
    }
    const VkExtent2D postViewport =
        m_offscreen ? VkExtent2D{m_width, m_height} : VkExtent2D{0, 0};
    m_post->recreate(swapViews, swapExtent, inputs, postViewport);
    refreshTextures();
}

void RenderTarget::destroyTextures() {
    for (auto ds : m_textureSets) {
        if (ds != VK_NULL_HANDLE) ImGui_ImplVulkan_RemoveTexture(ds);
    }
    m_textureSets.clear();
    m_textureViews.clear();
}

void RenderTarget::refreshTextures() {
    if (!m_post || !m_post->offscreen() || !m_imgui || !m_hdr) {
        destroyTextures();
        return;
    }

    // 先看这次的离屏视图和上次注册的是不是同一批。是 → 什么都不做，
    // ImTextureID 保持不变（交换链 resize 走的正是这条路）。
    std::vector<VkImageView> views;
    const uint32_t n = m_hdr->framesInFlight();
    views.reserve(n);
    for (uint32_t i = 0; i < n; ++i) views.push_back(m_post->sceneOutputView(i));

    if (views == m_textureViews && !views.empty()) return;

    destroyTextures();
    m_textureSets.resize(n, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < n; ++i) {
        if (views[i] == VK_NULL_HANDLE) continue;
        m_textureSets[i] = ImGui_ImplVulkan_AddTexture(
            views[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    m_textureViews = std::move(views);
}

uint64_t RenderTarget::textureId(uint32_t frameIndex) const {
    if (m_textureSets.empty()) return 0;
    const VkDescriptorSet ds = m_textureSets[frameIndex % m_textureSets.size()];
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ds));
}

} // namespace render
