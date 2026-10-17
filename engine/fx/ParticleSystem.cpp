#include "fx/ParticleSystem.h"

#include "render/HdrTarget.h"
#include "render/ShaderPath.h"

#include "rhi/Buffer.h"
#include "rhi/CommandPool.h"
#include "rhi/ComputePipeline.h"
#include "rhi/DescriptorSet.h"
#include "rhi/Device.h"
#include "rhi/Pipeline.h"

#include "core/Logger.h"

#include <cstring>
#include <string>

namespace fx {

namespace {

// ⚠ 这里必须是**非索引**的 VkDrawIndirectCommand，而不是
// VkDrawIndexedIndirectCommand —— 后者是 5 × uint32 = 20 字节，前者
// 4 × uint32 = 16 字节。差别不只是大小：
//
//   四边形是顶点着色器用 gl_VertexIndex 现拼的（见 particle.vert），
//   这要求 gl_VertexIndex 恰好是 0..5。**索引绘制下 gl_VertexIndex 是从
//   索引缓冲里读出来的值** —— 而这里根本没有顶点/索引缓冲可绑，于是
//   驱动从无处可读，拿到的是垃圾索引：三角形被切成"月牙"状残片
//   （症状：粒子位置对、数量对、颜色对，但每颗粒子缺一半）。
//   非索引绘制下 gl_VertexIndex = firstVertex + 顶点序号，0..5 正好。
//
//   代价为零：instanceCount 依然由 GPU 在计数 pass 里写，CPU 全程不知道
//   有几颗粒子 —— 间接绘制的两条好处（数据不出显存 / 无需回读）一个不少。
constexpr uint32_t kIndirectCmdStride = 16;
constexpr VkDeviceSize kIndirectBytes =
    kIndirectCmdStride * kBlendModeCount;
// 计数器：[0]=加色存活 [1]=Alpha 存活 [2]=本帧发射数 [3]=保留
constexpr uint32_t kCounterSlots = 4;
// 模拟的工作组局部尺寸（必须与 particle_sim.comp 的 local_size_x 一致）
constexpr uint32_t kSimLocalSize = 64;
// 绘制管线的 push 常量大小：两个 vec4 + 一个 ivec4（与 particle.vert 的
// FxPush 一致）。第三个 vec4 是 aliveBase —— 两条 blend 各自从自己那段
// 紧凑表开始（见 particle.vert 的说明）。
constexpr uint32_t kDrawPushBytes = 48;
// 计数 / 排序管线的 push 常量：两个 vec4（与 particle_count.comp 的
// SortPC 一致）。x = 相机世界坐标 + 排序模式，y = 开关 + 总槽数 + 档数。
constexpr uint32_t kCountPushBytes = 32;
// 粒子绘制管线绑定的 FrameUBO **前缀**大小：view + proj + lightDir +
// lightColor + camPos = 176 字节（shader 只声明到 camPos）。
// ⚠ 描述符的 range 不能超过 buffer 的实际大小（FrameUBO = 768）。
constexpr VkDeviceSize kFrameUboPrefixBytes = 176;

// 计数 / 排序管线的 push 常量结构。与 particle_count.comp 的 SortPC 一致。
struct SortPush {
    // xyz = 相机世界坐标 w = 排序模式（ecs::ParticleSortMode 的整数值）
    glm::vec4 cam;
    // x = 排序开关(0/1) y = 总槽数 z = 档数 w = 保留
    glm::vec4 cfg;
};

} // namespace

ParticleSystem::ParticleSystem(rhi::Device& device, rhi::CommandPool& cmdPool,
                               const ParticleSettings& settings,
                               uint32_t framesInFlight)
    : m_device(device),
      m_cmdPool(cmdPool),
      m_settings(settings),
      m_framesInFlight(framesInFlight > 0 ? framesInFlight : 1) {
    // ---- 深度采样器：NEAREST ----
    // 插值两个不同表面的深度会造出**不存在**的中间深度，软粒子的淡化
    // 距离就跟着乱掉（与 DecalBuffer 的深度采样器同一条理由）。
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_NEAREST;
    si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(m_device.get(), &si, nullptr, &m_depthSampler));

    createRenderPass();

    // ---- set 0（模拟）：全是 SSBO，没有 sampler / UBO ----
    {
        VkDescriptorSetLayoutBinding b[4]{};
        for (int k = 0; k < 4; ++k) {
            b[k].binding = static_cast<uint32_t>(k);
            b[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[k].descriptorCount = 1;
            b[k].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        m_simSetLayout = rhi::makeSetLayout(
            m_device, std::vector<VkDescriptorSetLayoutBinding>(b, b + 4));
    }
    // ---- set 0（计数 / 排序）：计数器 → 间接命令，外加排序要读的东西 ----
    //   0 = 计数器
    //   1 = 间接绘制命令（写）
    //   2 = 发射器表（排序时判 Local 空间的位置要用 world）
    //   3 = 粒子表（排序要读年龄 / 位置 / 寿命）
    //   4 = 模拟填好的紧凑表（读）
    //   5 = 排序后的紧凑表（写）
    {
        VkDescriptorSetLayoutBinding b[6]{};
        for (int k = 0; k < 6; ++k) {
            b[k].binding = static_cast<uint32_t>(k);
            b[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[k].descriptorCount = 1;
            b[k].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        m_countSetLayout = rhi::makeSetLayout(
            m_device, std::vector<VkDescriptorSetLayoutBinding>(b, b + 6));
    }

    // ---- set 0（绘制）----
    //   0 = 粒子 SSBO（顶点阶段读）
    //   1 = alive 紧凑表 SSBO（顶点阶段读）
    //   2 = FrameUBO（顶点阶段要 view / proj / camPos）
    //   3 = 场景深度（片元阶段做软粒子 + 遮挡）
    //   4 = 排序后的紧凑表（Sort Mode != None 时顶点阶段读它）
    //   5 = 发射器表（Simulation Space = Local 时顶点阶段要用 world 变换）
    {
        VkDescriptorSetLayoutBinding b[6]{};
        b[0].binding = 0;
        b[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[0].descriptorCount = 1;
        b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        b[1].binding = 1;
        b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[1].descriptorCount = 1;
        b[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        b[2].binding = 2;
        b[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b[2].descriptorCount = 1;
        b[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        b[3].binding = 3;
        b[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[3].descriptorCount = 1;
        b[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        b[4].binding = 4;
        b[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[4].descriptorCount = 1;
        b[4].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        b[5].binding = 5;
        b[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[5].descriptorCount = 1;
        b[5].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        m_drawSetLayout = rhi::makeSetLayout(
            m_device, std::vector<VkDescriptorSetLayoutBinding>(b, b + 6));
    }

    // 管线在构造函数里先建一次（m_samples 此时是 1x）。真正的采样数由
    // recreate() 给，变了会重建 —— 所以这里的初值只是"有个能用的对象"。
    createPipelines();
}

ParticleSystem::~ParticleSystem() {
    m_device.waitIdle();
    m_drawAdditive.reset();
    m_drawAlpha.reset();
    m_countPipe.reset();
    m_simPipe.reset();
    destroyFrames();
    m_descriptors.reset();

    VkDevice dev = m_device.get();
    if (m_renderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(dev, m_renderPass, nullptr);
        m_renderPass = VK_NULL_HANDLE;
    }
    if (m_simSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, m_simSetLayout, nullptr);
        m_simSetLayout = VK_NULL_HANDLE;
    }
    if (m_countSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, m_countSetLayout, nullptr);
        m_countSetLayout = VK_NULL_HANDLE;
    }
    if (m_drawSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, m_drawSetLayout, nullptr);
        m_drawSetLayout = VK_NULL_HANDLE;
    }
    if (m_depthSampler != VK_NULL_HANDLE) {
        vkDestroySampler(dev, m_depthSampler, nullptr);
        m_depthSampler = VK_NULL_HANDLE;
    }
}

// ============================================================
// 资源
// ============================================================

bool ParticleSystem::ensureResources() {
    if (m_ready && !m_needRebuild) return true;

    m_device.waitIdle();
    destroyFrames();
    m_descriptors.reset();

    createDescriptors();

    m_frames.resize(m_framesInFlight);
    for (uint32_t i = 0; i < m_framesInFlight; ++i) createFrame(i);

    zeroPools();

    m_ready = true;
    m_needRebuild = false;
    m_stats.capacity = capacity();

    VK_LOG_INFO("FX-POOL: allocated %u slots/emitter x %u emitters = %u slots, "
                "%.2f MB (simHz=%u)",
                m_settings.perEmitterCapacity, m_settings.maxEmitters,
                capacity(),
                static_cast<double>(capacity()) *
                    static_cast<double>(sizeof(ParticleGpu)) / 1048576.0,
                m_settings.simHz);
    return true;
}

void ParticleSystem::setSettings(const ParticleSettings& s) {
    // 只有动**显存布局**的字段才需要重建。其余（开关 / 倍率 / 冻结帧）
    // 走上传与着色，改一下立刻生效，不付 waitIdle 的代价。
    const bool rebuild = s.perEmitterCapacity != m_settings.perEmitterCapacity ||
                         s.maxEmitters != m_settings.maxEmitters;
    m_settings = s;
    if (rebuild) m_needRebuild = true;
}

void ParticleSystem::createDescriptors() {
    // 每帧 3 个 set（模拟 / 计数 / 绘制）。
    // 池的容量按类型给：
    //   UBO   —— 只有绘制的 binding 2（FrameUBO），每帧 1 个
    //   采样器 —— 只有绘制的 binding 3（场景深度），每帧 1 个
    //   SSBO  —— **每帧 14 个**（见下）
    //
    // ⚠⚠ SSBO 的数法（2026-10-16 修）：池是按 **set layout 的全部 binding**
    //  计费的，`vkAllocateDescriptorSets` 那一刻就把整个 layout 的每种描述符
    //  从池里扣掉 —— 不是"实际写了几条"。
    //   实际 layout：模拟 4（emitters/particles/alive/counters）
    //              + 计数 6（bindings 0..5，含 aliveSorted）
    //              + 绘制 4（bindings 0/1/4/5）
    //              = **14 个/帧**。
    //   这里原先是 `* 8`，注释还把计数/绘制各算成 2 个（只数了"主要的"那几条）
    //   → 预算 8/帧 < 需求 14/帧 → **第二帧一开始就 `VK_ERROR_OUT_OF_POOL_MEMORY`**
    //   （-1000069000，落在 rhi/DescriptorSet.inl 的 vkAllocateDescriptorSets），
    //   表现是 `MYVK_FX=1` 一开就 `Fatal: Vulkan call failed` —— 空场景也一样，
    //   与场景/UI 无关。留 16 而不是刚好 14：多 2 个余量，以后给某个 set 加
    //   一个 binding 不会再炸。
    m_descriptors = std::make_unique<rhi::DescriptorAllocator>(
        m_device, m_framesInFlight * 3, m_framesInFlight, m_framesInFlight,
        m_framesInFlight * 16, 0);
}

void ParticleSystem::createFrame(uint32_t i) {
    Frame& f = m_frames[i];
    const uint32_t cap = capacity();
    const VkDeviceSize particleBytes =
        static_cast<VkDeviceSize>(cap) * sizeof(ParticleGpu);

    // ---- 发射器表（常驻映射：每帧原地覆盖，不重建对象）----
    f.emitters = std::make_unique<rhi::Buffer>(
        m_device,
        static_cast<VkDeviceSize>(m_settings.maxEmitters) *
            sizeof(ParticleEmitterGpu),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    f.emittersMapped = f.emitters->map();
    std::memset(f.emittersMapped, 0, static_cast<size_t>(f.emitters->size()));

    // ---- 粒子池（DEVICE_LOCAL：只有 GPU 读写，CPU 一个字节都不碰）----
    // TRANSFER_DST 是给 ensureResources 里的 vkCmdFillBuffer 清零用的
    //（见 zeroPools）。STORAGE_BUFFER 本身**不含**传输目的位。
    f.particles = std::make_unique<rhi::Buffer>(
        m_device, particleBytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    // ---- 两张紧凑表 ----
    f.alive = std::make_unique<rhi::Buffer>(
        m_device,
        static_cast<VkDeviceSize>(cap) * sizeof(uint32_t) * kBlendModeCount,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    // ---- 排序后的紧凑表（Sort Mode != None 时由计数那一趟写）----
    // 顶点着色器按 push 常量的开关在这张表和 alive 之间选一张读，
    // 所以**不需要**两张表都在时重录任何东西。
    // TRANSFER_DST 同样是为 zeroPools 的 vkCmdFillBuffer。
    f.aliveSorted = std::make_unique<rhi::Buffer>(
        m_device,
        static_cast<VkDeviceSize>(cap) * sizeof(uint32_t) * kBlendModeCount,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    // ---- 间接绘制命令（GPU 写、GPU 读）----
    f.indirect = std::make_unique<rhi::Buffer>(
        m_device, kIndirectBytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    // ⚠ 必须显式清零：vkAllocateMemory 的内容是**未定义**的，一上来就
    //   "画 40 亿个实例"是能直接把驱动打挂的。
    std::memset(f.indirect->map(), 0, static_cast<size_t>(kIndirectBytes));

    // ---- 计数器（CPU 清零 + 回读，所以 HOST_VISIBLE）----
    f.counters = std::make_unique<rhi::Buffer>(
        m_device, kCounterSlots * sizeof(uint32_t),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    f.countersMapped = f.counters->map();
    std::memset(f.countersMapped, 0, kCounterSlots * sizeof(uint32_t));

    // ---- 描述符 ----
    f.simSet = m_descriptors->allocate(m_simSetLayout);
    m_descriptors->writeBuffer(f.simSet, 0, f.emitters->get(),
                               f.emitters->size(),
                               VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    m_descriptors->writeBuffer(f.simSet, 1, f.particles->get(),
                               f.particles->size(),
                               VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    m_descriptors->writeBuffer(f.simSet, 2, f.alive->get(), f.alive->size(),
                               VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    m_descriptors->writeBuffer(f.simSet, 3, f.counters->get(),
                               f.counters->size(),
                               VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

    f.countSet = m_descriptors->allocate(m_countSetLayout);
    m_descriptors->writeBuffer(f.countSet, 0, f.counters->get(),
                               f.counters->size(),
                               VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    m_descriptors->writeBuffer(f.countSet, 1, f.indirect->get(),
                               f.indirect->size(),
                               VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    m_descriptors->writeBuffer(f.countSet, 2, f.emitters->get(),
                               f.emitters->size(),
                               VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    m_descriptors->writeBuffer(f.countSet, 3, f.particles->get(),
                               f.particles->size(),
                               VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    m_descriptors->writeBuffer(f.countSet, 4, f.alive->get(), f.alive->size(),
                               VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    m_descriptors->writeBuffer(f.countSet, 5, f.aliveSorted->get(),
                               f.aliveSorted->size(),
                               VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

    f.drawSet = m_descriptors->allocate(m_drawSetLayout);
    m_descriptors->writeBuffer(f.drawSet, 0, f.particles->get(),
                               f.particles->size(),
                               VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    m_descriptors->writeBuffer(f.drawSet, 1, f.alive->get(), f.alive->size(),
                               VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    m_descriptors->writeBuffer(f.drawSet, 4, f.aliveSorted->get(),
                               f.aliveSorted->size(),
                               VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    m_descriptors->writeBuffer(f.drawSet, 5, f.emitters->get(),
                               f.emitters->size(),
                               VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    // binding 2（FrameUBO）每帧由 draw() 写；binding 3（深度）在 recreate 里写。
}

void ParticleSystem::destroyFrames() {
    // ⚠ Frame 里的 framebuffer 是**裸句柄**，vector::clear() 不会销毁它
    //   （与 DecalBuffer 踩过的同一个坑）。必须显式销毁。
    VkDevice dev = m_device.get();
    for (Frame& f : m_frames) {
        if (f.fb != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(dev, f.fb, nullptr);
            f.fb = VK_NULL_HANDLE;
        }
        f.simSet = VK_NULL_HANDLE;
        f.countSet = VK_NULL_HANDLE;
        f.drawSet = VK_NULL_HANDLE;
    }
    m_frames.clear();
}

void ParticleSystem::zeroPools() {
    // ---- 为什么必须清零 ----
    // 粒子池是 DEVICE_LOCAL 的，而 vkAllocateMemory 拿到的内容是**未定义**
    // 的。模拟着色器用"velLife.w >= 0"判断存活 —— 初值要是正的，第一帧
    // 就会冒出上万颗位置 / 颜色全是垃圾（甚至 NaN）的粒子。
    // 池里全零恰好等于"死粒子"：velLife.w = 0 且 misc.x（寿命）= 0 →
    // 第一次推进就算出 age = dt >= 0 → 立刻标死。**零正好是合法的死亡态**，
    // 所以不需要别的初始化值。
    //
    // 紧凑表一起清：它只在 instanceCount 范围内被读，理论上不清也没事，
    // 但清掉之后"任何时刻 dump alive[] 都是有意义的"，调试值得这点开销。
    if (m_frames.empty()) return;

    VkCommandBuffer cmd = m_cmdPool.beginOneTime();
    for (Frame& f : m_frames) {
        if (f.particles) {
            vkCmdFillBuffer(cmd, f.particles->get(), 0, f.particles->size(),
                            0u);
        }
        if (f.alive) {
            vkCmdFillBuffer(cmd, f.alive->get(), 0, f.alive->size(), 0u);
        }
        if (f.aliveSorted) {
            vkCmdFillBuffer(cmd, f.aliveSorted->get(), 0,
                            f.aliveSorted->size(), 0u);
        }
    }
    // FILL 是一次**传输写**。要让它对后面的 compute 可见，必须显式声明
    // TRANSFER → COMPUTE 的可用/可见对（endAndSubmit 里的 submit 屏障只
    // 覆盖"命令缓冲之间的顺序"，不覆盖内存可见性）。
    VkMemoryBarrier mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                         0, 1, &mb, 0, nullptr, 0, nullptr);
    m_cmdPool.endAndSubmit(cmd);
}

void ParticleSystem::createRenderPass() {
    // 一个颜色附件：主视口 HDR 的 **resolve 图**（单采样）。
    //
    // initialLayout = SHADER_READ_ONLY：前向 pass 结束时那张图就停在那里
    //   （HdrTarget 的 resolve finalLayout），render pass 自己会把它转到
    //   COLOR_ATTACHMENT；finalLayout 转回 SHADER_READ_ONLY，后面的
    //   FrameGraph 链 / PostProcess 照常采样。
    // loadOp = LOAD：粒子是**叠**在已渲染好的场景上的，绝不能清屏。
    // 没有深度附件：遮挡与软化都在片元里用采到的场景深度判（见 .frag）。
    VkAttachmentDescription color{};
    color.format = render::HdrTarget::kColorFormat;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    color.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference ref{};
    ref.attachment = 0;
    ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    sub.pDepthStencilAttachment = nullptr;

    VkSubpassDependency deps[2]{};
    // 外部 → 子过程：等前向 pass 的颜色写完成（含 WAR：更早的采样读完）
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    // 子过程 → 外部：写完转 SHADER_READ_ONLY 且对后续的采样可见
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
    ci.pSubpasses = &sub;
    ci.dependencyCount = 2;
    ci.pDependencies = deps;
    VK_CHECK(vkCreateRenderPass(m_device.get(), &ci, nullptr, &m_renderPass));
}

void ParticleSystem::createPipelines() {
    // ---- ① 模拟 ----
    {
        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcr.offset = 0;
        pcr.size = sizeof(SimPush);
        m_simPipe = std::make_unique<rhi::ComputePipeline>(
            m_device,
            vkutil::readFile(render::resolveShaderPath("particle_sim.comp.spv")),
            std::vector<VkDescriptorSetLayout>{m_simSetLayout}, &pcr);
    }
    // ---- ② 计数（写间接命令 + 可选的桶排序）----
    //
    // ⚠ push 常量的范围**必须**在这里声明，而且必须和粒子着色器里
    //   `layout(push_constant) uniform SortPC` 那个块逐字节一致。
    //   漏了它有三个后果，且都不是"报个错就算了"：
    //     1) 验证层每帧刷 VUID-VkComputePipelineCreateInfo-layout-07987；
    //     2) vkCmdPushConstants 的内容落到哪儿是**未定义**的 —— 大多数
    //        驱动上恰好还能用，换个驱动就是另一幅画面；
    //     3) 最要命的是 cfg.y（总槽数）取到 0 → `count = min(counters,
    //        0) = 0` → 间接命令的 instanceCount 恒为 0 → **一颗粒子都
    //        不画**，而日志里除了那几行 VUID 什么都没说。
    {
        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcr.offset = 0;
        pcr.size = kCountPushBytes;
        m_countPipe = std::make_unique<rhi::ComputePipeline>(
            m_device,
            vkutil::readFile(
                render::resolveShaderPath("particle_count.comp.spv")),
            std::vector<VkDescriptorSetLayout>{m_countSetLayout}, &pcr);
    }
    // ---- ③ 绘制 ----
    // 片元按 MSAA 选 .spv：深度图是 4 采样时必须用 sampler2DMS
    // （见 space.glsl 的说明），否则 vkCreateGraphicsPipelines 不报错、
    // 但采样结果是未定义的。两份 .spv 由同一份源码按 VENN_MSAA_DEPTH 编出。
    const bool msaa = m_samples != VK_SAMPLE_COUNT_1_BIT;
    const auto fragSpv = vkutil::readFile(render::resolveShaderPath(
        msaa ? "particle.msaa.frag.spv" : "particle.frag.spv"));
    const auto vertSpv =
        vkutil::readFile(render::resolveShaderPath("particle.vert.spv"));

    const auto build = [&](bool additive) {
        rhi::PipelineDesc d;
        d.vertSpv = vertSpv;
        d.fragSpv = fragSpv;
        // **没有顶点缓冲**：四边形由 gl_VertexIndex 现拼（与
        // fullscreen.vert 同一个路子），每实例的粒子数据从 SSBO 取。
        d.setLayouts = {m_drawSetLayout};
        d.pushConstant.stageFlags =
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        d.pushConstant.offset = 0;
        d.pushConstant.size = kDrawPushBytes;
        d.renderPass = m_renderPass;
        d.samples = VK_SAMPLE_COUNT_1_BIT;  // 目标是 resolve 图（单采样）
        d.colorAttachmentCount = 1;
        // 没有深度附件 → 深度测试 / 写入必须都关（否则验证层报
        // "管线要深度附件而 RenderPass 没有"）。
        d.depthTest = false;
        d.depthWrite = false;
        d.cullMode = VK_CULL_MODE_NONE;  // billboard 朝向随相机，别剔
        d.blendEnable = true;
        if (additive) {
            // 加色：src*srcAlpha + dst。**乘 alpha** 而不是纯 ONE ——
            // 不然粒子边缘的羽化就没了（方块的四个角一样亮）。
            d.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            d.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            d.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            d.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        } else {
            d.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            d.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            d.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            d.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        }
        return std::make_unique<rhi::Pipeline>(m_device, d);
    };

    m_drawAdditive = build(true);
    m_drawAlpha = build(false);
}

void ParticleSystem::recreate(VkExtent2D extent, VkSampleCountFlagBits samples,
                              const std::vector<VkImageView>& colorViews,
                              const std::vector<VkImageView>& depthViews) {
    // 调用方负责 waitIdle（与 DecalBuffer / VolumetricFog 同一约定）
    m_extent = extent;
    if (m_extent.width == 0) m_extent.width = 1;
    if (m_extent.height == 0) m_extent.height = 1;

    if (samples != m_samples) {
        m_samples = samples;
        m_drawAdditive.reset();
        m_drawAlpha.reset();
        createPipelines();
    }

    // framebuffer：每帧在途一张，挂的是该帧的 HDR resolve 视图。
    // 视图每次 HdrTarget 重建都会换 → 这里整体重建。
    VkDevice dev = m_device.get();
    for (Frame& f : m_frames) {
        if (f.fb != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(dev, f.fb, nullptr);
            f.fb = VK_NULL_HANDLE;
        }
    }
    for (uint32_t i = 0; i < m_frames.size() && i < colorViews.size(); ++i) {
        if (colorViews[i] == VK_NULL_HANDLE) continue;
        VkFramebufferCreateInfo fb{};
        fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass = m_renderPass;
        fb.attachmentCount = 1;
        fb.pAttachments = &colorViews[i];
        fb.width = m_extent.width;
        fb.height = m_extent.height;
        fb.layers = 1;
        VK_CHECK(vkCreateFramebuffer(dev, &fb, nullptr, &m_frames[i].fb));
    }

    // 深度视图换了 → 重写绘制 set 的 binding 3。
    // 布局必须是 DEPTH_READ_ONLY：前向 pass 结束时深度就停在那里
    //（与 SSAOPass / VolumetricFog 的 setDepthViews 完全一致）。
    if (m_descriptors) {
        for (uint32_t i = 0; i < m_frames.size() && i < depthViews.size(); ++i) {
            if (m_frames[i].drawSet == VK_NULL_HANDLE) continue;
            m_descriptors->writeTexture(m_frames[i].drawSet, 3, m_depthSampler,
                                        depthViews[i],
                                        VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
        }
    }
}

// ============================================================
// 每帧
// ============================================================

void ParticleSystem::upload(const std::vector<ParticleEmitterGpu>& emitters,
                            uint32_t frameIndex, bool resetCounters) {
    if (!m_ready || frameIndex >= m_frames.size()) return;
    Frame& f = m_frames[frameIndex];

    uint32_t n = static_cast<uint32_t>(emitters.size());
    m_dropped = n > m_settings.maxEmitters ? n - m_settings.maxEmitters : 0;
    if (n > m_settings.maxEmitters) n = m_settings.maxEmitters;
    m_emitterCount = n;

    auto* out = static_cast<ParticleEmitterGpu*>(f.emittersMapped);
    for (uint32_t i = 0; i < n; ++i) {
        out[i] = emitters[i];
    }
    // ⚠ 表的尾部要清零：发射器数**变少**时（删掉一个发射器），后面的
    //   槽位还留着上一帧的数据，而 GPU 那边是按 `float(e) < emitterCount`
    //   判有效的 —— 数值上没问题，但 rank/ramp 里会留着旧值，
    //   拷贝出来的场景文件与内存里的状态就不一致了。
    for (uint32_t i = n; i < m_settings.maxEmitters; ++i) {
        out[i] = ParticleEmitterGpu{};
    }

    // 计数器清零（本帧的存活数 / 发射数由 GPU 累加）。
    // 安全性：本帧槽的 fence 已经等过（Renderer 的调用顺序保证），
    // 此刻 GPU 不再访问这块内存。
    //
    // ⚠ 冻结帧（fx.freezeFrame 已到）时**不清**：那一帧不派发模拟，
    //   清掉之后回读永远是 0，FX-DUMP 就看不出"冻住时还剩多少粒子"了
    //   —— 而那正是冻结帧最有用的那一条读数。
    if (resetCounters) {
        std::memset(f.countersMapped, 0, kCounterSlots * sizeof(uint32_t));
    }

    m_stats.emitters = n;
    m_stats.capacity = capacity();
    m_stats.dropped = m_dropped;
}

void ParticleSystem::pollStats(uint32_t frameIndex) {
    if (!m_ready || frameIndex >= m_frames.size()) return;
    const auto* c =
        static_cast<const uint32_t*>(m_frames[frameIndex].countersMapped);
    if (!c) return;
    // 两张紧凑表相加 = 本帧真正画出来的粒子数
    m_stats.alive = c[0] + c[1];
    m_stats.spawned = c[2];
    m_stats.draws = m_stats.alive;
    if (m_stats.alive > m_stats.maxAlive) m_stats.maxAlive = m_stats.alive;
    m_stats.readbackValid = true;

    // 间接命令里的 instanceCount。这是**绘制真的会读的那个值** ——
    // 计数器对而间接命令没写对，画面就是"一颗粒子都没有"，而
    // FX-DUMP 里 alive 却是几百。两者一起打出来，这种不一致一眼可见。
    const auto* ic =
        static_cast<const uint32_t*>(m_frames[frameIndex].indirect->map());
    if (ic) {
        m_stats.instAdditive = ic[kIndirectCmdStride / 4 * 0 + 1];  // cmd[0].instanceCount
        m_stats.instAlpha = ic[kIndirectCmdStride / 4 * 1 + 1];     // cmd[1].instanceCount
    }
}

void ParticleSystem::recordSimulation(VkCommandBuffer cmd, uint32_t frameIndex,
                                      const SimPush& push,
                                      const glm::vec3& camPos, int sortMode) {
    if (!m_ready || frameIndex >= m_frames.size()) return;
    Frame& f = m_frames[frameIndex];
    // draw() 要用它决定顶点着色器读哪张紧凑表（两条 draw 的 push 常量）。
    m_sortMode = sortMode;

    const uint32_t total = capacity();
    const uint32_t groups = (total + kSimLocalSize - 1) / kSimLocalSize;

    // ---- ① 模拟 + 紧凑化 ----
    m_simPipe->dispatch(cmd, groups, 1, 1, &f.simSet, 1, &push, sizeof(push));

    // ---- sim → count：计数器里的存活数必须对第二个 dispatch 可见 ----
    {
        VkMemoryBarrier mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask =
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0,
                             nullptr, 0, nullptr);
    }

    // ---- ② 写间接绘制命令（两条：加色 / Alpha）+ 可选的桶排序 ----
    // 排序需要相机位置（判距离）与总槽数 / 档数 / 最大发射器数，
    // 全部塞在 push 常量里 —— 这一个 1 线程的 dispatch 就把排序做完了
    // （见 particle_count.comp 顶部的推导）。
    {
        SortPush sp{};
        sp.cam = glm::vec4(camPos, static_cast<float>(sortMode));
        sp.cfg = glm::vec4(sortMode != 0 ? 1.0f : 0.0f,
                           static_cast<float>(total),
                           static_cast<float>(kSortBuckets),
                           static_cast<float>(m_settings.maxEmitters));
        m_countPipe->dispatch(cmd, 1, 1, 1, &f.countSet, 1, &sp,
                              sizeof(sp));
    }

    // ---- count → 绘制：间接命令对 DRAW_INDIRECT 可见；粒子的写对
    //      VERTEX_SHADER / FRAGMENT_SHADER 可见（三个目的地一起等）。----
    {
        VkMemoryBarrier mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                           VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                 VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 1, &mb, 0, nullptr, 0, nullptr);
    }

    // ---- 计数器 / 间接命令 → 主机（下一轮同槽回读）----
    // 延迟一帧回读，且**必须**显式声明 HOST 阶段：HOST_VISIBLE 只保证
    // "CPU 能把这块内存映射到"，不保证"GPU 写的值已经落到看得见的地方"。
    // 间接命令也要一起声明 —— FX-DUMP 会回读它的 instanceCount，用它
    // 区分"计数器对但没画出来"和"根本没粒子"。
    {
        VkBufferMemoryBarrier bb[2]{};
        for (int k = 0; k < 2; ++k) {
            bb[k].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            bb[k].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            bb[k].dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            bb[k].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bb[k].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        }
        bb[0].buffer = f.counters->get();
        bb[0].offset = 0;
        bb[0].size = f.counters->size();
        bb[1].buffer = f.indirect->get();
        bb[1].offset = 0;
        bb[1].size = f.indirect->size();
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 2, bb,
                             0, nullptr);
    }

    // 数的是**派发次数**（每帧一次，哪怕一步没推进）—— 见头文件里的说明。
    // "推进了几步"是 Renderer 的 m_particleSimFrame，FX-DUMP 并排打。
    ++m_stats.simDispatches;
}

void ParticleSystem::draw(VkCommandBuffer cmd, uint32_t frameIndex,
                          VkBuffer frameUbo, const glm::vec2& screenSize,
                          float nearZ, float farZ) {
    if (!m_ready || frameIndex >= m_frames.size()) return;
    Frame& f = m_frames[frameIndex];
    if (f.fb == VK_NULL_HANDLE || !m_drawAdditive || !m_drawAlpha) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            VK_LOG_WARN("FX: draw() skipped (fb=%d pipe=%d/%d) -- particles "
                        "will be invisible even though the simulation runs",
                        f.fb != VK_NULL_HANDLE ? 1 : 0,
                        m_drawAdditive ? 1 : 0, m_drawAlpha ? 1 : 0);
        }
        return;
    }

    // binding 2 = 本帧的全局 UBO（与主链共用同一块 buffer，只是换了个
    // 描述符集对象）。每帧重写 —— 相机在动。
    m_descriptors->writeBuffer(f.drawSet, 2, frameUbo, kFrameUboPrefixBytes,
                               VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);

    struct FxPush {
        // x = 软化距离（米）y = 软化开关(0/1) z = 尺寸倍率 w = 亮度倍率
        glm::vec4 params;
        // x = 视口宽 y = 视口高 z = 近平面 w = 远平面
        glm::vec4 screen;
        // x = aliveBase（这一条 draw 用哪张紧凑表：加色 0，Alpha = 总槽数）
        // y = 用排序后的表(1) 还是原子顺序的表(0)
        // z = 发射器数量（顶点着色器把 p.misc.w 夹进合法范围用）
        // w = 保留。
        // **x 必须逐 blend 不同** —— 两张紧凑表在同一个 SSBO 的两段里，
        // 两条间接绘制共用同一个描述符集，只有 push 常量能告诉顶点着色器
        // 该从哪一段取粒子下标。y 则对两条 draw 相同（排序表与原子表
        // 都是同样的两段布局）。
        glm::ivec4 extra{0, 0, 0, 0};
    } pc{};
    pc.params = glm::vec4(m_softBase * m_settings.softness,
                          m_settings.softness > 0.0f ? 1.0f : 0.0f,
                          m_settings.sizeScale, m_settings.intensity);
    pc.screen = glm::vec4(screenSize.x, screenSize.y, nearZ, farZ);
    // 排序表与原子表是两块独立的 buffer，顶点阶段用这个开关选一块读。
    pc.extra.y = (m_sortMode == 0) ? 0 : 1;
    pc.extra.z = static_cast<int>(m_emitterCount);
    static_assert(sizeof(FxPush) == kDrawPushBytes,
                  "FxPush 与 kDrawPushBytes 必须一致（否则 push 范围对不上）");

    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = m_renderPass;
    rp.framebuffer = f.fb;
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = m_extent;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{};
    vp.width = static_cast<float>(m_extent.width);
    vp.height = static_cast<float>(m_extent.height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D sc{};
    sc.extent = m_extent;
    vkCmdSetScissor(cmd, 0, 1, &sc);

    // 两条间接绘制：各画自己那一类。空的那条 instanceCount = 0，
    // 驱动直接跳过（不会真的发起绘制）。
    const rhi::Pipeline* pipes[kBlendModeCount] = {m_drawAdditive.get(),
                                                   m_drawAlpha.get()};
    for (uint32_t m = 0; m < kBlendModeCount; ++m) {
        if (!pipes[m]) continue;
        // 紧凑表分段：[0] 加色在 alive[0..cap)，[1] Alpha 在
        // alive[cap..2cap) —— 与 particle_sim.comp 的 listBase 一致。
        pc.extra.x = static_cast<int>(m * capacity());
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          pipes[m]->get());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipes[m]->layout(), 0, 1, &f.drawSet, 0,
                                nullptr);
        vkCmdPushConstants(cmd, pipes[m]->layout(),
                           VK_SHADER_STAGE_VERTEX_BIT |
                               VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pc), &pc);
        // 顶点/索引缓冲区**一个都不绑**：四边形由 gl_VertexIndex 现拼，
        // 所以必须走非索引的那条路（见 kIndirectCmdStride 的说明）。
        // ⚠ 实例数（间接命令里的 instanceCount）是 GPU 在计数 pass 里
        //   写的 —— CPU 根本不知道本帧有几颗粒子。
        vkCmdDrawIndirect(
            cmd, f.indirect->get(),
            static_cast<VkDeviceSize>(m) * kIndirectCmdStride, 1,
            kIndirectCmdStride);
    }

    vkCmdEndRenderPass(cmd);
}

} // namespace fx
