#pragma once
// ============================================================
// render/Environment —— 天空盒 + 预计算的 IBL（image based lighting）
//
// ---- 为什么需要它 ----
//   在这之前，pbr.frag 的"环境光"是一个写死的天空梯度函数：
//   漫反射取 envColor(N)，镜面取 envColor(mix(R, N, rough²))。它能
//   让金属有颜色变化（比常量环境色强），但本质上都是假的：
//     · 粗糙度只让反射整体变暗，**不会变模糊**
//     · 完全没有能量守恒（掠射角的菲涅尔、kD/kS 的分配都不对）
//     · 粗糙表面的高光既不聚拢也不铺开，看起来像塑料而不是金属
//   真 IBL 必须把环境光拆成三条独立预计算的通道（Karis 的 split-sum）：
//
//     1) 天空立方体 skyCube —— 环境本体，一切的唯一来源。
//        背景 pass 直接采样它，两条卷积也都以它为输入。RGBA32F。
//     2) 辐照度立方体 irradiance —— 对 skyCube 做**余弦卷积**，
//        给漫反射用。32²、单 mip（辐照度是低通量，方向变化极慢，
//        高分辨率纯属浪费）。片元里 texture(irr, N) 一次采样即可。
//     3) 预过滤立方体 prefilter —— 对 skyCube 做 **GGX 重要性采样**，
//        给镜面用。128²、5 个 mip，mip m 对应 α = m/(mips-1)。
//        片元里按粗糙度取对应 mip，粗糙表面自然变模糊。
//     4) BRDF 查找表 brdfLut —— split-sum 的第二项，只和
//        (N·V, roughness) 有关，与环境无关，所以能烘成一张表。
//        256² RG32F，存 (A, B)。
//
//   最终镜面 = prefilter(R, rough) × (F0 × A + B)
//   漫反射 = albedo × irradiance(N)（irradiance 已含 cos 积分与 1/π 的抵消）
//
// ---- 为什么环境只在 init 生成一次 ----
//   天空由两个颜色参数化，不随场景/相机变化；四张图合计不到 2 MB，
//   生成耗时是毫秒级。每帧重算纯属浪费。
//
// ---- 与 set 0 的关系 ----
//   四张图作为 COMBINED_IMAGE_SAMPLER 写进 set 0 的 binding 7..10。
//   本类**不**持有那些 set（它们属于 Renderer 的每帧资源），只提供
//   采样器与视图。生成过程用本类自己的描述符池与布局 ——
//   Renderer 的池没预留 STORAGE_IMAGE，而且这套资源是自洽的，
//   没必要去挤那个池。
// ============================================================

#include "rhi/VulkanCommon.h"

#include <array>
#include <memory>
#include <vector>

namespace rhi {
class Device;
class CommandPool;
class ComputePipeline;
} // namespace rhi

namespace render {

// 生成参数。改这里任何一个尺寸都需要重新 build()（只在 init 调一次）。
struct EnvSettings {
    uint32_t skySize = 128;
    uint32_t irradianceSize = 32;
    uint32_t prefilterSize = 128;
    uint32_t prefilterMips = 5;
    uint32_t lutSize = 256;

    // 卷积质量
    float irradianceDelta = 0.10f;   // 半球扫掠的角步长（弧度）
    uint32_t prefilterSamples = 512;
    uint32_t brdfSamples = 512;

    // 天空模型（与 common.glsl 的 vennSkyColor 对应）
    glm::vec3 zenith{0.55f, 0.70f, 0.95f};
    glm::vec3 ground{0.10f, 0.09f, 0.09f};
};

class Environment {
public:
    Environment(rhi::Device& device, rhi::CommandPool& cmdPool,
                const EnvSettings& settings);
    ~Environment();

    Environment(const Environment&) = delete;
    Environment& operator=(const Environment&) = delete;

    // 生成四张图（一次性的：建图 → 跑 4 个 compute → 转布局 → 回读自检）
    void build();

    bool valid() const { return m_valid; }

    // ---- 采样资源（Renderer 写进 set 0 的 binding 7..10）----
    VkSampler cubeSampler() const { return m_cubeSampler; }
    VkSampler lutSampler() const { return m_lutSampler; }
    VkImageView skyView() const { return m_sky.sampleView; }
    VkImageView irradianceView() const { return m_irr.sampleView; }
    VkImageView prefilterView() const { return m_pref.sampleView; }
    VkImageView brdfLutView() const { return m_lut.sampleView; }

    uint32_t prefilterMips() const { return m_settings.prefilterMips; }
    // 片元里 textureLod 的上限（= mips - 1）
    float prefilterMaxLod() const {
        return static_cast<float>(m_settings.prefilterMips - 1);
    }

    const EnvSettings& settings() const { return m_settings; }

    // ---- 回读自检（build() 之后填好）----
    //
    // 这不是"调试打印"，是**断言的数据来源**：天空立方体的面平均值
    // 能直接证明面号→方向的映射与 Z-up 约定都对；BRDF 表在
    // (N·V=1, rough=0) 处的理论值是精确的 (1, 0)，可以硬断言。
    struct Probe {
        // 天空立方体各面平均色。下标与 Vulkan 面序一致：
        // 0=+X 1=-X 2=+Y 3=-Y 4=+Z 5=-Z
        glm::vec3 face[6] = {};
        // BRDF 表在两个位置的值
        float lutA_at_ndotv1_rough0 = 0.0f;   // 理论 ≈ 1
        float lutB_at_ndotv1_rough0 = 0.0f;   // 理论 ≈ 0
        float lutA_at_half = 0.0f;            // (0.5, 0.5)，只要落在 [0,1]
        float lutB_at_half = 0.0f;
    };
    const Probe& probe() const { return m_probe; }

    struct Stats {
        uint32_t skySize = 0;
        uint32_t irradianceSize = 0;
        uint32_t prefilterSize = 0;
        uint32_t prefilterMips = 0;
        uint32_t lutSize = 0;
    };
    const Stats& stats() const { return m_stats; }

private:
    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        // 采样视图：立方体资源是 CUBE，LUT 是 2D
        VkImageView sampleView = VK_NULL_HANDLE;
        // 写视图：每个 mip 一个 2D_ARRAY（imageStore 不能指定 mip 层级，
        // 所以预过滤的每一级都要有自己的视图）
        std::vector<VkImageView> storageViews;
        VkFormat format = VK_FORMAT_UNDEFINED;
        uint32_t size = 0;
        uint32_t mips = 1;
        bool cube = false;
    };

    void createCube(Image& img, VkFormat format, uint32_t size, uint32_t mips,
                    bool readback);
    void create2D(Image& img, VkFormat format, uint32_t size, bool readback);
    void destroyImage(Image& img);

    void createDescriptors();
    void createPipelines();
    // 录制"生成 + 转布局 + 回读拷贝"到一次性命令缓冲
    void recordBuild(VkCommandBuffer cmd, VkBuffer skyReadback,
                     VkBuffer lutReadback);
    // 从回读缓冲算出 Probe
    void fillProbe(const void* skyData, const void* lutData);

    rhi::Device& m_device;
    rhi::CommandPool& m_cmdPool;
    EnvSettings m_settings;

    Image m_sky, m_irr, m_pref, m_lut;

    VkSampler m_cubeSampler = VK_NULL_HANDLE;
    VkSampler m_lutSampler = VK_NULL_HANDLE;

    // 生成用的描述符基础设施（自持，不借用 Renderer 的池）
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    // 采样 skyCube（binding0）+ 写目标（binding1）
    VkDescriptorSetLayout m_sourceLayout = VK_NULL_HANDLE;
    // 只写目标（天空生成 / BRDF 表都不需要输入）
    VkDescriptorSetLayout m_sinkLayout = VK_NULL_HANDLE;

    VkDescriptorSet m_skySet = VK_NULL_HANDLE;
    VkDescriptorSet m_irrSet = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_prefSets;  // 每 mip 一个
    VkDescriptorSet m_lutSet = VK_NULL_HANDLE;

    std::unique_ptr<rhi::ComputePipeline> m_skyPipe;
    std::unique_ptr<rhi::ComputePipeline> m_irrPipe;
    std::unique_ptr<rhi::ComputePipeline> m_prefPipe;
    std::unique_ptr<rhi::ComputePipeline> m_lutPipe;

    bool m_valid = false;
    Stats m_stats;
    Probe m_probe;
};

} // namespace render
