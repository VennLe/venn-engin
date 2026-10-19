#include "ui2/UiRenderer.h"

#include "core/Logger.h"
#include "rhi/Buffer.h"
#include "rhi/CommandPool.h"
#include "rhi/DescriptorSet.h"
#include "rhi/Device.h"
#include "rhi/Image.h"
#include "rhi/Pipeline.h"
#include "render/ShaderPath.h"

#include <stb_image.h>

#include <cstring>
#include <string>
#include <vector>

namespace ui2 {

namespace {

constexpr VkVertexInputBindingDescription kBinding{0, sizeof(Ui2Vertex),
                                                   VK_VERTEX_INPUT_RATE_VERTEX};

// 与 ui2.vert 的 layout(location=N) **逐字段**对应。
// ⚠ 加字段必须同时改 Ui2Vertex 的 static_assert 与这段表。
constexpr VkVertexInputAttributeDescription kAttribs[] = {
    {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Ui2Vertex, x)},
    {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Ui2Vertex, u)},
    // R8G8B8A8_UNORM：CPU 存的是**sRGB 编码值**（0..255），归一化到 0..1，
    // 由片元着色器做 srgbToLinear。
    {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Ui2Vertex, color)},
    {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Ui2Vertex, hx)},
    {4, 0, VK_FORMAT_R32_SFLOAT, offsetof(Ui2Vertex, radius)},
    {5, 0, VK_FORMAT_R32_SFLOAT, offsetof(Ui2Vertex, mode)},
};

}  // namespace

UiRenderer::UiRenderer() = default;

UiRenderer::~UiRenderer() { destroy(); }

bool UiRenderer::init(rhi::Device& device, rhi::CommandPool& cmdPool,
                      VkRenderPass renderPass, uint32_t framesInFlight) {
    m_device = &device;
    m_framesInFlight = framesInFlight > 0 ? framesInFlight : 1;

    if (!createSetLayoutAndPool()) return false;
    if (!createAtlasFallback(cmdPool)) return false;
    if (!pipelineFor(renderPass)) return false;
    if (!createVertexBuffers()) return false;

    // 字体加载放在最后：它只是"能不能画字"，失败了通道照常可用。
    loadFont(cmdPool);

    VK_LOG_INFO("UI2-INIT frames=%u vbo=%uKB/frame atlas=%ux%d",
                static_cast<unsigned>(m_framesInFlight),
                static_cast<unsigned>(m_frameCapacity / 1024u),
                m_font ? static_cast<int>(m_font->atlasW) : 1,
                m_font ? static_cast<int>(m_font->atlasH) : 1);
    return true;
}

bool UiRenderer::loadFont(rhi::CommandPool& cmdPool) {
    const std::string jsonPath = resolveUiAsset("fonts/noto_sc_sdf.json");
    std::string err;
    auto font = std::make_unique<SdfFont>();
    if (!loadSdfFontJson(jsonPath, *font, err)) {
        VK_LOG_WARN("UI2-FONT failed path=%s reason=%s", jsonPath.c_str(),
                    err.c_str());
        return false;
    }

    // PNG 与 JSON 同目录同名（`gen_sdf_atlas.py` 成对写出）。
    std::string pngPath = jsonPath;
    const size_t dot = pngPath.find_last_of('.');
    if (dot != std::string::npos) pngPath = pngPath.substr(0, dot) + ".png";

    int w = 0, h = 0, ch = 0;
    // ⚠ 强制 1 通道：图集是 R8。让 stb 转成 RGBA 再当 R8 用会把 G 通道
    //   当灰度读（这里恰好也等于它，但那是运气不是契约）。
    unsigned char* pix = stbi_load(pngPath.c_str(), &w, &h, &ch, 1);
    if (!pix) {
        VK_LOG_WARN("UI2-FONT failed png=%s reason=%s", pngPath.c_str(),
                    stbi_failure_reason());
        return false;
    }
    if (static_cast<uint32_t>(w) != font->atlasW ||
        static_cast<uint32_t>(h) != font->atlasH) {
        // 图与表不是同一次生成的 → uv 全错。宁可退化成无字形，也别画出
        // "错位的字"（那种错比不画难查十倍）。
        VK_LOG_WARN("UI2-FONT mismatch png=%dx%d json=%ux%u（重跑 "
                    "tools/gen_sdf_atlas.py）", w, h, font->atlasW,
                    font->atlasH);
        stbi_image_free(pix);
        return false;
    }

    const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h;
    m_atlasImage = std::make_unique<rhi::Image>(
        *m_device, static_cast<uint32_t>(w), static_cast<uint32_t>(h),
        VK_FORMAT_R8_UNORM,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_TILING_OPTIMAL, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);

    rhi::Buffer staging(*m_device, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    staging.writeData(pix, bytes);
    stbi_image_free(pix);

    rhi::Image::transitionLayout(*m_device, cmdPool, m_atlasImage->get(),
                                 VK_IMAGE_LAYOUT_UNDEFINED,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    rhi::Image::copyBufferToImage(*m_device, cmdPool, staging.get(),
                                  m_atlasImage->get(),
                                  static_cast<uint32_t>(w),
                                  static_cast<uint32_t>(h));
    rhi::Image::transitionLayout(*m_device, cmdPool, m_atlasImage->get(),
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    setAtlas(m_atlasImage->view());
    // 锚点行：自动化断言"图集真的加载了、字形数对不对"。
    // ⚠ 带上 json= 路径：从 build/bin 跑时可能命中"仓库根兜底"那条候选，
    //   打出来才知道到底读的是哪一份（两份不一致时的经典怪现象）。
    VK_LOG_INFO("UI2-FONT face=%s license=%s glyphs=%u size=%.0f spread=%.1f "
                "atlas=%dx%d ascent=%.0f descent=%.0f top=%u json=%s",
                font->face.c_str(), font->license.c_str(),
                static_cast<unsigned>(font->glyphs.size()), font->size,
                font->spread, w, h, font->ascent, font->descent,
                static_cast<unsigned>(font->find(0x4E00) != nullptr),
                jsonPath.c_str());
    m_font = std::move(font);
    return true;
}

bool UiRenderer::createSetLayoutAndPool() {
    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    m_setLayout = rhi::makeSetLayout(*m_device, {b});
    if (m_setLayout == VK_NULL_HANDLE) return false;

    // 池是**自建**的：不从全局池拿，理由与 PostProcess 一样 ——
    // resize / 换图集要重写描述符，若复用 Renderer 的池，"重置池"会连同
    // 纹理的描述符集一起失效。独立池 → 整池丢弃重建，互不影响。
    m_pool = std::make_unique<rhi::DescriptorAllocator>(
        *m_device, m_framesInFlight, 0, m_framesInFlight);
    m_sets.assign(m_framesInFlight, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < m_framesInFlight; ++i) {
        m_sets[i] = m_pool->allocate(m_setLayout);
        if (m_sets[i] == VK_NULL_HANDLE) return false;
    }

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    // 线性过滤：SDF 的采样就是"插值距离场"，点采样会让字形边缘出现台阶。
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f;
    VK_CHECK(vkCreateSampler(m_device->get(), &si, nullptr, &m_sampler));
    return true;
}

bool UiRenderer::createAtlasFallback(rhi::CommandPool& cmdPool) {
    // 缺省 1×1 **白** RGBA8：
    //   · mode 1（贴图）采到 (1,1,1,1) → 不着色；
    //   · mode 2（SDF 字形）R=1 → 完全覆盖（图集还没就绪时每个字形都是一块实心方块，
    //     一眼就能看出"图集没加载"，比画出空白好排查）。
    const uint32_t pixel = 0xFFFFFFFFu;
    m_white = std::make_unique<rhi::Image>(
        *m_device, 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_TILING_OPTIMAL, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);

    rhi::Buffer staging(*m_device, sizeof(pixel), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    staging.writeData(&pixel, sizeof(pixel));
    rhi::Image::transitionLayout(*m_device, cmdPool, m_white->get(),
                                 VK_IMAGE_LAYOUT_UNDEFINED,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    rhi::Image::copyBufferToImage(*m_device, cmdPool, staging.get(),
                                  m_white->get(), 1, 1);
    rhi::Image::transitionLayout(*m_device, cmdPool, m_white->get(),
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    m_atlasView = m_white->view();
    for (uint32_t i = 0; i < m_framesInFlight; ++i) {
        m_pool->writeTexture(m_sets[i], 0, m_sampler, m_atlasView);
    }
    return true;
}

void UiRenderer::setAtlas(VkImageView view) {
    if (!view || view == m_atlasView || m_sets.empty()) return;
    m_atlasView = view;
    for (uint32_t i = 0; i < m_framesInFlight; ++i) {
        m_pool->writeTexture(m_sets[i], 0, m_sampler, m_atlasView);
    }
    VK_LOG_INFO("UI2-ATLAS swizzled=0 view=%p", reinterpret_cast<void*>(view));
}

// 按 render pass 建管线（懒建 + 缓存）。
//
// ⚠ 为什么不能"一份管线两模式通用"：两个输出 pass 除了 finalLayout，还差
//   一条 subpass dependency（离屏那条要把写转成 FRAGMENT_SHADER 可见），
//   而 **dependencies 参与兼容性判定** → 模式 B 里用建在模式 A 上的管线
//   会被告 `VUID-vkCmdDraw-renderPass-02684`。见 UiRenderer.h 文件头。
rhi::Pipeline* UiRenderer::pipelineFor(VkRenderPass renderPass) {
    for (PassPipeline& p : m_pipelines) {
        if (p.pass == renderPass) return p.pipeline.get();
    }
    if (renderPass == VK_NULL_HANDLE) return nullptr;

    rhi::PipelineDesc d;
    d.vertSpv = vkutil::readFile(render::resolveShaderPath("ui2.vert.spv"));
    d.fragSpv = vkutil::readFile(render::resolveShaderPath("ui2.frag.spv"));
    if (d.vertSpv.empty() || d.fragSpv.empty()) {
        VK_LOG_ERROR("UI2: 找不到 ui2.vert.spv / ui2.frag.spv");
        return nullptr;
    }
    d.vertexBindings = {kBinding};
    d.vertexAttributes.assign(kAttribs, kAttribs + 6);
    d.setLayouts = {m_setLayout};
    d.pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    d.pushConstant.size = sizeof(float) * 2;   // vec2 viewport
    d.renderPass = renderPass;
    d.samples = VK_SAMPLE_COUNT_1_BIT;
    d.colorAttachmentCount = 1;
    // UI 不写深度（输出 pass 根本没有深度附件），也不做背面剔除
    // （quad 的三角形绕序在两个三角形之间是反的也行 —— 因为这里关掉了剔除；
    //   开着的话有一半 quad 会被裁掉，是"UI 少一半"的经典症状）。
    d.depthTest = false;
    d.depthWrite = false;
    d.cullMode = VK_CULL_MODE_NONE;
    d.blendEnable = true;
    d.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    d.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    d.colorBlendOp = VK_BLEND_OP_ADD;
    d.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    d.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;

    PassPipeline entry;
    entry.pass = renderPass;
    entry.pipeline = std::make_unique<rhi::Pipeline>(*m_device, d);
    if (!entry.pipeline) return nullptr;
    m_pipelines.push_back(std::move(entry));
    // 每加一条打印一次：自动化靠这个数确认"两个输出 pass 各建了一条"
    // （一条 = 只在模式 A 用过，模式 B 的路径没被走到）。
    VK_LOG_INFO("UI2-PIPE pass=%p count=%u",
                reinterpret_cast<void*>(renderPass),
                static_cast<unsigned>(m_pipelines.size()));
    return m_pipelines.back().pipeline.get();
}

bool UiRenderer::createVertexBuffers() {
    m_frameCapacity = static_cast<VkDeviceSize>(kMaxVertsPerFrame) *
                      sizeof(Ui2Vertex);
    const VkDeviceSize total = m_frameCapacity * m_framesInFlight;
    m_vbo = std::make_unique<rhi::Buffer>(
        *m_device, total, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    m_mapped = m_vbo->map();
    return m_mapped != nullptr;
}

void UiRenderer::destroy() {
    if (!m_device) return;
    m_device->waitIdle();
    m_pipelines.clear();
    if (m_mapped && m_vbo) {
        m_vbo->unmap();
        m_mapped = nullptr;
    }
    m_vbo.reset();
    m_sets.clear();
    m_pool.reset();
    m_atlasImage.reset();
    m_font.reset();
    m_white.reset();
    if (m_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_device->get(), m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
    }
    if (m_setLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_device->get(), m_setLayout, nullptr);
        m_setLayout = VK_NULL_HANDLE;
    }
    m_device = nullptr;
}

void UiRenderer::record(VkCommandBuffer cmd, uint32_t frameIndex,
                        const UiCanvas& canvas, VkExtent2D outExtent,
                        VkRenderPass renderPass) {
    m_stats = {};
    const std::vector<Ui2Vertex>& verts = canvas.vertices();
    rhi::Pipeline* pipe = pipelineFor(renderPass);
    // 窗口最小化时 extent=0：除零会算出 NaN 的 NDC，整帧消失（还可能触发
    // 验证层报"viewport 尺寸非法"）。直接返回，与 Renderer 的守卫同源。
    if (!pipe || verts.empty() || outExtent.width == 0 ||
        outExtent.height == 0) {
        return;
    }

    uint32_t count = static_cast<uint32_t>(verts.size());
    if (count > kMaxVertsPerFrame) {
        if (!m_overflowWarned) {
            m_overflowWarned = true;
            VK_LOG_WARN("UI2: 顶点溢出 %u > %u（本帧被截断）", count,
                        kMaxVertsPerFrame);
        }
        count = kMaxVertsPerFrame;
    }

    const uint32_t fi = frameIndex % m_framesInFlight;
    const VkDeviceSize offset = m_frameCapacity * fi;
    std::memcpy(static_cast<uint8_t*>(m_mapped) + offset, verts.data(),
                static_cast<size_t>(count) * sizeof(Ui2Vertex));

    VkPipelineLayout layout = pipe->layout();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe->get());

    VkViewport vp{};
    vp.width = static_cast<float>(outExtent.width);
    vp.height = static_cast<float>(outExtent.height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{VkOffset2D{0, 0}, outExtent};
    vkCmdSetScissor(cmd, 0, 1, &sc);

    VkBuffer vb = m_vbo->get();
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);

    const float pc[2] = {static_cast<float>(outExtent.width),
                         static_cast<float>(outExtent.height)};
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), pc);

    VkDescriptorSet set = m_sets[fi];
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1,
                            &set, 0, nullptr);
    vkCmdDraw(cmd, count, 1, 0, 0);

    m_stats.draws = 1;
    m_stats.verts = count;
}

void UiRenderer::recordFrameUi(VkCommandBuffer cmd, uint32_t frameIndex,
                               VkExtent2D outExtent, VkRenderPass renderPass) {
    m_canvas.begin();
    // 字体走"帧上下文"交给画布（buildOverlay 的签名不带它，见 UiCanvas.h）
    setFrameFont(m_font.get());
    buildOverlay(m_canvas, outExtent.width, outExtent.height);
    record(cmd, frameIndex, m_canvas, outExtent, renderPass);

    // 每 60 帧一行。断言的是"合批有效"（单图集 + 无 scissor 变化必须是
    // 1 个 draw），而不是"画了多少" —— 顶点数会随内容变，draw 数不该变。
    if ((m_uiFrames++ % 60u) == 1u) {
        VK_LOG_INFO("UI2-STATS draws=%u verts=%u out=%ux%u",
                    m_stats.draws, m_stats.verts,
                    static_cast<unsigned>(outExtent.width),
                    static_cast<unsigned>(outExtent.height));
    }
}

}  // namespace ui2
