#include "render/WaterPass.h"

#include "render/OffscreenImage.h"
#include "render/ShaderPath.h"

#include "core/Logger.h"
#include "rhi/DescriptorSet.h"
#include "rhi/Device.h"
#include "rhi/Pipeline.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace render {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// 顶点：局部 xy（米）+ 法线 + part。
//
// `part` 是这块几何的"角色"，不是可有可无的标记：
//   0 = 水面    —— z 在顶点着色器里加波浪位移，法线由波浪梯度重算
//   1 = 侧壁顶  —— 贴在水面高度上，垂直截面
//   2 = 侧壁底  —— 沉到池底以下（z 由 UBO 里的 skirtBottom 决定）
//
// 为什么侧壁的 z 不在 CPU 算好：水位是一个可以实时拖的滑块。把 z 烘进
// 顶点就意味着"每拖一格都要重建顶点缓冲"，而侧壁底只依赖两个标量
// （水面高度 / 池底），放进 UBO 就完全免费了。
struct WVert {
    glm::vec3 pos;
    glm::vec3 nrm;
    float part = 0.0f;
};

struct MeshData {
    std::vector<WVert> verts;
    std::vector<std::uint32_t> idx;
    // 侧壁贡献了多少三角形（水面之外的部分）。验收脚本用它确认"水体真的
    // 有厚度"，而不是靠肉眼看截图。
    std::uint32_t skirtTris = 0;
    // 与网格配套的**外接**半长（用于水面的视锥剔除与尺寸显示）。
    // 圆形 = (r, r)，矩形 = (halfX, halfZ)。
    glm::vec2 halfExtent{0.0f};
};

// 沿一圈边界生成侧壁（顶圈 + 底圈）。`ring` 必须按**逆时针**给，
// 这样 (t.y, -t.x) 正好是外法线。
void appendSkirt(MeshData& m, const std::vector<glm::vec2>& ring) {
    const std::uint32_t n = static_cast<std::uint32_t>(ring.size());
    if (n < 3) return;
    const std::uint32_t base = static_cast<std::uint32_t>(m.verts.size());

    for (int pass = 0; pass < 2; ++pass) {
        const float part = (pass == 0) ? 1.0f : 2.0f;
        for (std::uint32_t i = 0; i < n; ++i) {
            const glm::vec2 prev = ring[(i + n - 1) % n];
            const glm::vec2 next = ring[(i + 1) % n];
            glm::vec2 t = next - prev;
            const float len = glm::length(t);
            t = (len > 1.0e-6f) ? (t / len) : glm::vec2(1.0f, 0.0f);
            const glm::vec2 nrm(t.y, -t.x);
            WVert v;
            v.pos = glm::vec3(ring[i], 0.0f);
            v.nrm = glm::vec3(nrm, 0.0f);
            v.part = part;
            m.verts.push_back(v);
        }
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::uint32_t i1 = (i + 1) % n;
        const std::uint32_t a = base + i;              // 顶圈 i
        const std::uint32_t b = base + i1;             // 顶圈 i+1
        const std::uint32_t c = base + n + i;          // 底圈 i
        const std::uint32_t d = base + n + i1;         // 底圈 i+1
        m.idx.push_back(a);
        m.idx.push_back(c);
        m.idx.push_back(b);
        m.idx.push_back(b);
        m.idx.push_back(c);
        m.idx.push_back(d);
    }
    m.skirtTris += n * 2;
}

// 矩形的一圈边界，逆时针（从左下角出发沿底边向右）。
std::vector<glm::vec2> rectRing(glm::vec2 h) {
    std::vector<glm::vec2> ring;
    const int segX = std::clamp(static_cast<int>(h.x * 2.0f), 4, 128);
    const int segZ = std::clamp(static_cast<int>(h.y * 2.0f), 4, 128);
    for (int x = 0; x < segX; ++x) {
        const float f = static_cast<float>(x) / static_cast<float>(segX);
        ring.push_back(glm::vec2(-h.x + 2.0f * h.x * f, -h.y));
    }
    for (int z = 0; z < segZ; ++z) {
        const float f = static_cast<float>(z) / static_cast<float>(segZ);
        ring.push_back(glm::vec2(h.x, -h.y + 2.0f * h.y * f));
    }
    for (int x = 0; x < segX; ++x) {
        const float f = static_cast<float>(x) / static_cast<float>(segX);
        ring.push_back(glm::vec2(h.x - 2.0f * h.x * f, h.y));
    }
    for (int z = 0; z < segZ; ++z) {
        const float f = static_cast<float>(z) / static_cast<float>(segZ);
        ring.push_back(glm::vec2(-h.x, h.y - 2.0f * h.y * f));
    }
    return ring;
}


// 水面顶点（z 恒 0，波浪在顶点着色器里加）
void pushTop(MeshData& m, glm::vec2 p) {
    WVert v;
    v.pos = glm::vec3(p, 0.0f);
    v.nrm = glm::vec3(0.0f, 0.0f, 1.0f);
    v.part = 0.0f;
    m.verts.push_back(v);
}

// 圆形：径向网格（1 个中心点 + rings×sectors 圈）。
// 为什么不用"方格 + 裁掉圆外的格子"：那样边缘是一圈锯齿台阶，
// 而水面的边缘正是"岸线"，锯齿会非常显眼（用户要的是"细腻")。
MeshData buildCircle(float radius) {
    MeshData m;
    const float r = std::max(radius, 0.25f);
    const int rings = std::clamp(static_cast<int>(r / 1.0f), 6, 64);
    const int sectors = std::clamp(rings * 3, 24, 192);

    m.verts.reserve(1 + static_cast<std::size_t>(rings) * sectors);
    pushTop(m, glm::vec2(0.0f, 0.0f));  // 中心
    for (int ring = 1; ring <= rings; ++ring) {
        const float rad = r * static_cast<float>(ring) /
                          static_cast<float>(rings);
        for (int s = 0; s < sectors; ++s) {
            const float a = 2.0f * kPi * static_cast<float>(s) /
                            static_cast<float>(sectors);
            pushTop(m, glm::vec2(rad * std::cos(a), rad * std::sin(a)));
        }
    }

    // 中心扇
    for (int s = 0; s < sectors; ++s) {
        const std::uint32_t a = 0;
        const std::uint32_t b = 1 + static_cast<std::uint32_t>(s);
        const std::uint32_t c = 1 + static_cast<std::uint32_t>((s + 1) % sectors);
        m.idx.push_back(a);
        m.idx.push_back(b);
        m.idx.push_back(c);
    }
    // 环带
    for (int ring = 1; ring < rings; ++ring) {
        const std::uint32_t base0 =
            1 + static_cast<std::uint32_t>(ring - 1) * sectors;
        const std::uint32_t base1 = 1 + static_cast<std::uint32_t>(ring) * sectors;
        for (int s = 0; s < sectors; ++s) {
            const std::uint32_t s1 = static_cast<std::uint32_t>((s + 1) % sectors);
            const std::uint32_t a = base0 + static_cast<std::uint32_t>(s);
            const std::uint32_t b = base0 + s1;
            const std::uint32_t c = base1 + static_cast<std::uint32_t>(s);
            const std::uint32_t d = base1 + s1;
            m.idx.push_back(a);
            m.idx.push_back(c);
            m.idx.push_back(b);
            m.idx.push_back(b);
            m.idx.push_back(c);
            m.idx.push_back(d);
        }
    }

    // 侧壁（水体截面）：沿最外圈一圈。没有它，水面在边界处就是一张
    // 悬空的纸 —— 这正是用户反馈的"反重力"。逆时针（与角度递增一致），
    // 于是 appendSkirt 算出来的法线朝外。
    std::vector<glm::vec2> ring;
    ring.reserve(static_cast<std::size_t>(sectors));
    for (int s = 0; s < sectors; ++s) {
        const float a = 2.0f * kPi * static_cast<float>(s) /
                        static_cast<float>(sectors);
        ring.push_back(glm::vec2(r * std::cos(a), r * std::sin(a)));
    }
    appendSkirt(m, ring);

    m.halfExtent = glm::vec2(r, r);
    return m;
}

// 矩形：规整方格。长宽独立的河道用它（圆形做不到"一直往一个方向流"）。
MeshData buildRect(glm::vec2 halfExtent) {
    MeshData m;
    const float hx = std::max(halfExtent.x, 0.25f);
    const float hz = std::max(halfExtent.y, 0.25f);
    const int segX = std::clamp(static_cast<int>(hx * 2.0f), 4, 128);
    const int segZ = std::clamp(static_cast<int>(hz * 2.0f), 4, 128);

    m.verts.reserve(static_cast<std::size_t>(segX + 1) * (segZ + 1));
    for (int z = 0; z <= segZ; ++z) {
        const float fz = static_cast<float>(z) / static_cast<float>(segZ);
        const float pz = -hz + 2.0f * hz * fz;
        for (int x = 0; x <= segX; ++x) {
            const float fx = static_cast<float>(x) / static_cast<float>(segX);
            const float px = -hx + 2.0f * hx * fx;
            pushTop(m, glm::vec2(px, pz));
        }
    }
    const std::uint32_t stride = static_cast<std::uint32_t>(segX + 1);
    for (int z = 0; z < segZ; ++z) {
        for (int x = 0; x < segX; ++x) {
            const std::uint32_t a =
                static_cast<std::uint32_t>(z) * stride + static_cast<std::uint32_t>(x);
            const std::uint32_t b = a + 1;
            const std::uint32_t c = a + stride;
            const std::uint32_t d = c + 1;
            m.idx.push_back(a);
            m.idx.push_back(c);
            m.idx.push_back(b);
            m.idx.push_back(b);
            m.idx.push_back(c);
            m.idx.push_back(d);
        }
    }
    // 侧壁：矩形的一圈边界（逆时针）。河道/海水箱从侧面看才有"水体"的
    // 厚度，而不是一张贴在地面上的色块。
    appendSkirt(m, rectRing(glm::vec2(hx, hz)));

    m.halfExtent = glm::vec2(hx, hz);
    return m;
}

MeshData buildMesh(const water::WaterBody& b) {
    if (b.shape == water::Shape::Circle) return buildCircle(b.halfExtent.x);
    return buildRect(b.halfExtent);
}

VkImageMemoryBarrier imageBarrier(VkImage image, VkImageLayout from,
                                  VkImageLayout to, VkAccessFlags srcAccess,
                                  VkAccessFlags dstAccess) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    return b;
}

} // namespace

WaterPass::~WaterPass() { shutdown(); }

bool WaterPass::init(const InitInfo& info) {
    if (!info.device || info.globalSetLayout == VK_NULL_HANDLE) return false;
    m_device = info.device;
    m_cmdPool = info.cmdPool;
    m_globalSetLayout = info.globalSetLayout;
    m_colorFormat = info.colorFormat;
    m_framesInFlight = std::max(info.framesInFlight, 1u);

    createRenderPass();

    // ---- set 1：0 = 这块水体的参数 UBO，1 = 场景色，2 = 场景深度 ----
    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT |
                             VK_SHADER_STAGE_FRAGMENT_BIT;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    m_setLayout = rhi::makeSetLayout(
        *m_device, std::vector<VkDescriptorSetLayoutBinding>(
                       bindings, bindings + 3));

    // 场景色：线性过滤（折射偏移是亚像素级的，最近邻会看到色块）
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(m_device->get(), &si, nullptr, &m_colorSampler));

    // 深度：NEAREST（插值会造出"两个面之间"的假深度，污染遮挡判断）
    si.magFilter = VK_FILTER_NEAREST;
    si.minFilter = VK_FILTER_NEAREST;
    VK_CHECK(vkCreateSampler(m_device->get(), &si, nullptr, &m_depthSampler));

    createPipeline();
    VK_LOG_INFO("WATER-INIT colorFormat=%d frames=%u maxBodies=%u",
                static_cast<int>(m_colorFormat), m_framesInFlight,
                kMaxBodies);
    return m_pipeline != nullptr;
}

void WaterPass::shutdown() {
    if (!m_device) return;
    m_device->waitIdle();
    destroyFrameTargets();
    m_bodies.clear();
    m_pipeline.reset();
    m_descriptors.reset();

    VkDevice dev = m_device->get();
    if (m_depthSampler != VK_NULL_HANDLE) {
        vkDestroySampler(dev, m_depthSampler, nullptr);
        m_depthSampler = VK_NULL_HANDLE;
    }
    if (m_colorSampler != VK_NULL_HANDLE) {
        vkDestroySampler(dev, m_colorSampler, nullptr);
        m_colorSampler = VK_NULL_HANDLE;
    }
    if (m_setLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, m_setLayout, nullptr);
        m_setLayout = VK_NULL_HANDLE;
    }
    if (m_renderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(dev, m_renderPass, nullptr);
        m_renderPass = VK_NULL_HANDLE;
    }
    m_device = nullptr;
}

void WaterPass::createRenderPass() {
    // 单颜色附件、单采样、**LOAD**（水面之外保持前向 Pass 的结果）。
    // 进出布局都是 SHADER_READ_ONLY_OPTIMAL：进来时是"前向 resolve 完、
    // 后处理还没读"的状态，出去时后处理（Bloom / 色调映射）紧跟着要读它。
    VkAttachmentDescription color{};
    color.format = m_colorFormat;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    color.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = 1;
    ci.pAttachments = &color;
    ci.subpassCount = 1;
    ci.pSubpasses = &subpass;
    ci.dependencyCount = 2;
    ci.pDependencies = deps;
    VK_CHECK(vkCreateRenderPass(m_device->get(), &ci, nullptr, &m_renderPass));
}

void WaterPass::createPipeline() {
    // 多采样深度 → sampler2DMS 版片元；单采样 → 普通版。
    // 两份 .spv 都由 CMake 的 SHADER_MSAA_DEPTH_SHADERS 编出。
    const bool msaaDepth = m_samples != VK_SAMPLE_COUNT_1_BIT;
    const char* fragName =
        msaaDepth ? "water.msaa.frag.spv" : "water.frag.spv";

    rhi::PipelineDesc desc;
    desc.vertSpv = vkutil::readFile(resolveShaderPath("water.vert.spv"));
    desc.fragSpv = vkutil::readFile(resolveShaderPath(fragName));
    if (desc.vertSpv.empty() || desc.fragSpv.empty()) {
        VK_LOG_ERROR("WATER-SHADER-MISSING vert=%zu frag=%zu (%s)",
                     desc.vertSpv.size(), desc.fragSpv.size(), fragName);
        return;
    }

    // 顶点：局部 xy + 法线 + part（水面 / 侧壁顶 / 侧壁底）。
    // 法线只有侧壁用得上（水面法线完全由波浪梯度重算），但放在同一个
    // 顶点里比开第二套管线便宜得多 —— 侧壁只有几百个顶点。
    VkVertexInputBindingDescription vb{};
    vb.binding = 0;
    vb.stride = sizeof(WVert);
    vb.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    desc.vertexBindings.push_back(vb);

    VkVertexInputAttributeDescription va{};
    va.location = 0;
    va.binding = 0;
    va.format = VK_FORMAT_R32G32B32_SFLOAT;
    va.offset = offsetof(WVert, pos);
    desc.vertexAttributes.push_back(va);

    va.location = 1;
    va.format = VK_FORMAT_R32G32B32_SFLOAT;
    va.offset = offsetof(WVert, nrm);
    desc.vertexAttributes.push_back(va);

    va.location = 2;
    va.format = VK_FORMAT_R32_SFLOAT;
    va.offset = offsetof(WVert, part);
    desc.vertexAttributes.push_back(va);

    // set 0 = 主前向的全局 set（同一份布局 → 中途换管线不打断 set 0）
    desc.setLayouts.push_back(m_globalSetLayout);
    desc.setLayouts.push_back(m_setLayout);

    // 与地形同样的取舍：push range 声明 64 字节 VERTEX（只放 model）
    desc.pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    desc.pushConstant.offset = 0;
    desc.pushConstant.size = sizeof(glm::mat4);

    desc.renderPass = m_renderPass;
    desc.colorAttachmentCount = 1;
    desc.samples = VK_SAMPLE_COUNT_1_BIT;  // 本通道没有多采样附件
    desc.depthTest = false;                // 手写深度测试（见文件头）
    desc.depthWrite = false;
    // 水面可能从下方看（水下视角），两面都要画
    desc.cullMode = VK_CULL_MODE_NONE;
    // 片元输出 alpha 恒 1，但混合开着让"岸边 alpha 渐变"以后能直接用
    desc.blendEnable = true;
    desc.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    desc.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;

    m_pipeline = std::make_unique<rhi::Pipeline>(*m_device, desc);
}

void WaterPass::destroyFrameTargets() {
    if (!m_device) return;
    VkDevice dev = m_device->get();
    for (auto& f : m_frames) {
        if (f.fb != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(dev, f.fb, nullptr);
            f.fb = VK_NULL_HANDLE;
        }
        destroyImage2D(*m_device, f.colorCopyImage, f.colorCopyMemory,
                       f.colorCopyView);
        f.resolveView = VK_NULL_HANDLE;
        f.depthView = VK_NULL_HANDLE;
        f.colorCopyValid = false;
    }
    m_frames.clear();
}

void WaterPass::setTargets(VkExtent2D extent, VkSampleCountFlagBits samples,
                           const std::vector<VkImageView>& colorViews,
                           const std::vector<VkImageView>& depthViews) {
    if (!m_device || extent.width == 0 || extent.height == 0) return;
    const bool sizeChanged = (extent.width != m_extent.width ||
                             extent.height != m_extent.height);
    const bool sampleChanged = (samples != m_samples);
    if (!sizeChanged && !sampleChanged && !m_frames.empty() &&
        colorViews.size() == m_frames.size()) {
        // 尺寸与采样数都没变，但**借来的视图可能换了**：HdrTarget 在自己的
        // 分辨率/采样数没变的情况下也可能整组重建（换 DRS 档位、换预览目标、
        // 切 MSAA 之外的重建路径），那时旧 resolve/depth 视图已经销毁。
        //
        // 这里必须逐句柄比对，而不是"反正重写一遍描述符"。重写本身不算错，
        // 但 framebuffer 是从 resolveView 建的 —— 只刷新字段不重建 fb，
        // 下一帧 vkCmdBeginRenderPass 就会拿着已销毁的附件视图开工
        //（2026-10-21 实测：1600x900 → 789 那条路径上报的就是这个）。
        bool viewChanged = false;
        for (std::size_t i = 0; i < m_frames.size(); ++i) {
            const VkImageView rc =
                (i < colorViews.size()) ? colorViews[i] : VK_NULL_HANDLE;
            const VkImageView rd =
                (i < depthViews.size()) ? depthViews[i] : VK_NULL_HANDLE;
            if (rc != m_frames[i].resolveView || rd != m_frames[i].depthView)
                viewChanged = true;
            m_frames[i].resolveView = rc;
            m_frames[i].depthView = rd;
        }
        if (!viewChanged) return;   // 真没变：什么都不用做（每帧都会调进来）

        // framebuffer 跟着 resolveView 走：先拆后建
        VkDevice dev = m_device->get();
        for (auto& f : m_frames) {
            if (f.fb != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(dev, f.fb, nullptr);
                f.fb = VK_NULL_HANDLE;
            }
            if (f.resolveView == VK_NULL_HANDLE) continue;
            VkFramebufferCreateInfo fbci{};
            fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbci.renderPass = m_renderPass;
            fbci.attachmentCount = 1;
            fbci.pAttachments = &f.resolveView;
            fbci.width = m_extent.width;
            fbci.height = m_extent.height;
            fbci.layers = 1;
            VK_CHECK(vkCreateFramebuffer(dev, &fbci, nullptr, &f.fb));
        }
        // 深度视图换了 → 描述符里的 binding 2 要重写（binding 1 是自有的
        // 拷贝图，没变）
        if (m_descriptors) {
            for (auto& b : m_bodies) {
                for (std::size_t i = 0; i < b->set.size() && i < m_frames.size();
                     ++i) {
                    if (m_frames[i].depthView == VK_NULL_HANDLE) continue;
                    m_descriptors->writeTexture(
                        b->set[i], 2, m_depthSampler, m_frames[i].depthView,
                        VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
                }
            }
        }
        return;
    }

    m_device->waitIdle();
    const bool rebuildPipeline = sampleChanged || !m_pipeline;

    // ⚠ 这里必须是 destroyFrameTargets() 而不是 m_frames.clear()：
    // 后者会把旧 framebuffer 与颜色拷贝图直接丢掉（泄漏，且旧 fb 还挂在
    // 已被 HdrTarget 销毁的 resolveView 上）。
    destroyFrameTargets();
    m_descriptors.reset();
    m_extent = extent;
    m_samples = samples;

    const uint32_t frames =
        static_cast<uint32_t>(std::max<std::size_t>(colorViews.size(), 1));
    m_frames.resize(frames);
    for (uint32_t i = 0; i < frames; ++i) {
        FrameTarget& f = m_frames[i];
        f.resolveView = (i < colorViews.size()) ? colorViews[i] : VK_NULL_HANDLE;
        f.depthView = (i < depthViews.size()) ? depthViews[i] : VK_NULL_HANDLE;

        // 场景色拷贝：只要 TRANSFER_DST（每帧被拷进来）+ SAMPLED（片元采样）
        createImage2D(*m_device, m_extent.width, m_extent.height, m_colorFormat,
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                          VK_IMAGE_USAGE_SAMPLED_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT, f.colorCopyImage,
                      f.colorCopyMemory, f.colorCopyView);

        if (f.resolveView != VK_NULL_HANDLE) {
            VkFramebufferCreateInfo fbci{};
            fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbci.renderPass = m_renderPass;
            fbci.attachmentCount = 1;
            fbci.pAttachments = &f.resolveView;
            fbci.width = m_extent.width;
            fbci.height = m_extent.height;
            fbci.layers = 1;
            VK_CHECK(vkCreateFramebuffer(m_device->get(), &fbci, nullptr,
                                         &f.fb));
        }
    }

    // 描述符池：每块水体每帧一个 set（binding 0 是它自己的 UBO）
    const uint32_t maxSets = kMaxBodies * frames + 8;
    m_descriptors = std::make_unique<rhi::DescriptorAllocator>(
        *m_device, maxSets, maxSets, maxSets);

    // 已有的水体要重绑（set 会被重建）
    createBodyResources();

    if (rebuildPipeline) {
        m_pipeline.reset();
        createPipeline();
    }
    VK_LOG_INFO("WATER-TARGETS %ux%u frames=%u samples=%d",
                m_extent.width, m_extent.height, frames,
                static_cast<int>(m_samples));
}

void WaterPass::createBodyResources() {
    if (!m_descriptors || m_frames.empty()) return;
    const std::size_t frames = m_frames.size();
    for (auto& b : m_bodies) {
        b->ubo.clear();
        b->set.clear();
        b->ubo.reserve(frames);
        b->set.reserve(frames);
        for (std::size_t i = 0; i < frames; ++i) {
            b->ubo.push_back(std::make_unique<rhi::Buffer>(
                *m_device, sizeof(WaterUboGpu), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
            VkDescriptorSet set = m_descriptors->allocate(m_setLayout);
            m_descriptors->writeBuffer(set, 0, b->ubo.back()->get(),
                                       sizeof(WaterUboGpu));
            // 场景色：折射的来源（每帧一张）
            m_descriptors->writeTexture(set, 1, m_colorSampler,
                                        m_frames[i].colorCopyView);
            // 场景深度：多采样时必须是 DEPTH_STENCIL_READ_ONLY
            if (m_frames[i].depthView != VK_NULL_HANDLE) {
                m_descriptors->writeTexture(
                    set, 2, m_depthSampler, m_frames[i].depthView,
                    VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
            }
            b->set.push_back(set);
        }
    }
}

bool WaterPass::ensureGeometry(Body& b, const water::WaterBody& src) {
    const bool same = b.hasGeometry && b.shape == src.shape &&
                      b.halfExtent == src.halfExtent;
    if (same) return true;

    MeshData mesh = buildMesh(src);
    if (mesh.verts.empty() || mesh.idx.empty()) return false;

    b.vbo = rhi::Buffer::createDeviceLocal(
        *m_device, *m_cmdPool, mesh.verts.data(),
        mesh.verts.size() * sizeof(WVert), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    b.ibo = rhi::Buffer::createDeviceLocal(
        *m_device, *m_cmdPool, mesh.idx.data(),
        mesh.idx.size() * sizeof(std::uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    if (!b.vbo || !b.ibo) return false;

    b.indexCount = static_cast<uint32_t>(mesh.idx.size());
    b.shape = src.shape;
    b.halfExtent = src.halfExtent;
    b.hasGeometry = true;
    VK_LOG_INFO("WATER-MESH shape=%s verts=%zu tris=%u skirt=%u "
                "halfExtent=(%.1f,%.1f)",
                src.shape == water::Shape::Circle ? "circle" : "rect",
                mesh.verts.size(), b.indexCount / 3, mesh.skirtTris,
                static_cast<double>(mesh.halfExtent.x),
                static_cast<double>(mesh.halfExtent.y));
    return true;
}

void WaterPass::sync(const std::vector<water::WaterBody>& bodies) {
    if (!m_device || !m_descriptors) return;
    const std::size_t count = std::min<std::size_t>(bodies.size(), kMaxBodies);
    if (bodies.size() > kMaxBodies && !m_loggedLimit) {
        m_loggedLimit = true;
        VK_LOG_WARN("WATER-LIMIT bodies=%zu max=%u (rest ignored)",
                    bodies.size(), kMaxBodies);
    }

    // 数量变化 → 增删 Body（多出来的先销毁；销毁要等 GPU，不在这里做，
    // 交给 waitIdle —— 水体数量变化是"用户新建/删除"级别的低频事件）
    if (m_bodies.size() != count) {
        m_device->waitIdle();
        while (m_bodies.size() > count) m_bodies.pop_back();
        while (m_bodies.size() < count) {
            auto b = std::make_unique<Body>();
            m_bodies.push_back(std::move(b));
        }
        createBodyResources();
    }

    for (std::size_t i = 0; i < count; ++i) {
        Body& b = *m_bodies[i];
        b.src = bodies[i];
        ensureGeometry(b, bodies[i]);
    }
}

void WaterPass::copySceneColor(VkCommandBuffer cmd, uint32_t frameIndex,
                              VkImage resolveImage) {
    if (m_frames.empty() || resolveImage == VK_NULL_HANDLE) return;
    FrameTarget& f = m_frames[frameIndex % m_frames.size()];
    if (f.colorCopyImage == VK_NULL_HANDLE) return;

    // 1) resolve：前向 Pass 结束时是 SHADER_READ_ONLY（finalLayout）→ 拷贝源
    VkImageMemoryBarrier toSrc = imageBarrier(
        resolveImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
        VK_ACCESS_TRANSFER_READ_BIT);

    // 2) 拷贝目标：第一次是 UNDEFINED（内容无所谓，整张覆盖）
    VkImageMemoryBarrier toDst = imageBarrier(
        f.colorCopyImage,
        f.colorCopyValid ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                         : VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        f.colorCopyValid ? VK_ACCESS_SHADER_READ_BIT : 0,
        VK_ACCESS_TRANSFER_WRITE_BIT);

    VkImageMemoryBarrier pre[2] = {toSrc, toDst};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 2, pre);

    VkImageCopy region{};
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource.layerCount = 1;
    region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.dstSubresource.layerCount = 1;
    region.extent = {m_extent.width, m_extent.height, 1};
    vkCmdCopyImage(cmd, resolveImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   f.colorCopyImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &region);

    // 3) resolve 回到 SHADER_READ_ONLY（后处理/探针还要读它），
    //    拷贝图转入 SHADER_READ_ONLY（片元的折射采样要读它）
    VkImageMemoryBarrier post[2] = {
        imageBarrier(resolveImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT),
        imageBarrier(f.colorCopyImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 2, post);
    f.colorCopyValid = true;
}

uint64_t WaterPass::record(VkCommandBuffer cmd, uint32_t frameIndex, float time,
                           float zNear, float zFar, VkDescriptorSet globalSet) {
    m_triangles = 0;
    if (!m_pipeline || m_bodies.empty() || m_frames.empty()) return 0;
    const uint32_t fi = frameIndex % static_cast<uint32_t>(m_frames.size());
    const FrameTarget& f = m_frames[fi];
    if (f.fb == VK_NULL_HANDLE) return 0;

    const float w = static_cast<float>(std::max(m_extent.width, 1u));
    const float h = static_cast<float>(std::max(m_extent.height, 1u));

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      m_pipeline->get());
    // set 0 与主前向是同一份布局 —— 本通道也需要 FrameUBO（相机矩阵、
    // 逆 viewProj、光照方向、屏幕尺寸、近远平面）
    if (globalSet != VK_NULL_HANDLE) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_pipeline->layout(), 0, 1, &globalSet, 0,
                                nullptr);
    }

    for (auto& up : m_bodies) {
        Body& b = *up;
        if (!b.hasGeometry || b.indexCount == 0) continue;
        if (fi >= b.set.size() || fi >= b.ubo.size()) continue;

        const water::WaterBody& s = b.src;
        WaterUboGpu gpu{};
        gpu.shallow = glm::vec4(s.shallowColor, s.opacity);
        gpu.deep = glm::vec4(s.deepColor, s.clarity);
        gpu.p0 = glm::vec4(s.level, s.waveHeight, s.waveLength, s.flowSpeed);
        glm::vec2 fdir = s.flowDir;
        const float flen = glm::length(fdir);
        fdir = (flen > 1e-4f) ? (fdir / flen) : glm::vec2(1.0f, 0.0f);
        gpu.p1 = glm::vec4(fdir, s.ripple, s.foam);
        gpu.p2 = glm::vec4(s.refraction, s.ior, s.roughness, time);
        gpu.p3 = glm::vec4(static_cast<float>(static_cast<int>(s.kind)),
                           static_cast<float>(static_cast<int>(s.shape)),
                           s.halfExtent.x, s.halfExtent.y);
        // 池底 + 侧壁下沿。下沿比池底再沉 0.75 m：地形是有起伏的，沉到
        // 最低点以下才保证**任何**观察角度都被地面遮住。多沉的那段埋在
        // 土里，不花任何代价。
        const float skirtBottom =
            std::min(s.bedLevel - 0.75f, s.level - 0.4f);
        gpu.p4 = glm::vec4(s.bedLevel, skirtBottom, 0.0f, 0.0f);
        gpu.screen = glm::vec4(w, h, 1.0f / w, 1.0f / h);
        gpu.camera = glm::vec4(zNear, zFar, 0.0f, 0.0f);
        b.ubo[fi]->writeData(&gpu, sizeof(gpu));

        // 模型：只含平移 + 绕 z 旋转（缩放烘在网格里）
        glm::mat4 model(1.0f);
        model[3] = glm::vec4(s.center.x, s.center.y, s.level, 1.0f);
        if (std::fabs(s.rotation) > 1e-4f) {
            const float rad = s.rotation * kPi / 180.0f;
            const float c = std::cos(rad);
            const float sn = std::sin(rad);
            glm::mat4 rot(1.0f);
            rot[0] = glm::vec4(c, sn, 0.0f, 0.0f);
            rot[1] = glm::vec4(-sn, c, 0.0f, 0.0f);
            model = model * rot;
        }

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_pipeline->layout(), 1, 1, &b.set[fi], 0,
                                nullptr);
        vkCmdPushConstants(cmd, m_pipeline->layout(), VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(glm::mat4), &model);

        VkBuffer vb = b.vbo->get();
        VkDeviceSize off = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &off);
        vkCmdBindIndexBuffer(cmd, b.ibo->get(), 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, b.indexCount, 1, 0, 0, 0);
        m_triangles += b.indexCount / 3;
    }
    return m_triangles;
}

VkFramebuffer WaterPass::framebuffer(uint32_t frameIndex) const {
    if (m_frames.empty()) return VK_NULL_HANDLE;
    return m_frames[frameIndex % m_frames.size()].fb;
}

} // namespace render
