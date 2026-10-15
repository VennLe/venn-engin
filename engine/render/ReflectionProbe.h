#pragma once
// ============================================================
// render/ReflectionProbe —— 反射探针（R5-1）
//
// ---- 要解决的问题 ----
//   SSR（fg_ssr.frag）只能在**屏幕里真的有东西**的地方反射。室内场景
//   背对窗口的那面墙、地板、金属球，屏幕里什么都没有 → SSR 全 miss，
//   画面里只剩 IBL 的预过滤天空。这不是 SSR 的实现缺陷，是它的定义。
//
//   反射探针填的就是这块：在场景里放一个盒体，**从盒体中心**把周围
//   环境烘成一张立方体贴图，之后任何落在这个盒体内的表面，其镜面反射
//   都从这张图里取。它不依赖屏幕里有什么，所以背对窗口也有内容。
//
// ---- 三段式管线 ----
//
//   ① 捕获（capture）：从探针位置用 6 个 90° 视锥的相机把**场景本身**
//      渲染进一张立方体贴图。复用主前向管线（renderSceneInto），
//      走的是真正的 PBR + 天空 + 阴影外的全部光照路径 —— 不是"再画一遍
//      假天空"。
//   ② 预过滤（prefilter）：对捕获结果做 GGX 重要性采样，逐 mip 烘一个
//      粗糙度（mip m ↔ α = m/(mips-1)）。**复用 ibl_prefilter.comp**
//      —— 它与 Environment 用的是同一个着色器，输入换成探针捕获图。
//      于是 textureLod(probeMap, R, roughness*maxLod) 就是"粗糙表面反射
//      会变糊"的全部实现，与 IBL 那一路逐位同源。
//   ③ 采样（pbr.frag）：片元落在某个探针盒体内 → 用 **box projection**
//      把反射方向修正到"从盒体内部看这张图"的视角（否则贴在一面墙上的
//      探针会把整个房间的反射压扁成一个方向的天空），再按粗糙度取 mip。
//
// ---- 为什么用 samplerCubeArray 而不是"每探针一个 binding" ----
//   探针数量在运行时可增删，而 set 0 的 binding 是**布局的一部分**
//   （数量变化要重建管线布局 → 管线全重建）。一个 cube array 把
//   "第 i 个探针"变成"第 6i..6i+5 层"，描述符一次写好永不改。
//   这与 bindless 的 textures[] 是同一条思路。
//
// ---- 默认关 = 基线逐像素不变 ----
//   关着时：一次捕获都不录、一个 compute 都不派发，binding 23 绑一张
//   1×1 的缺省 cube array，FrameUBO.probeParams.x = 0 → pbr.frag 里
//   那段整块短路。**资源也是懒建的**（首次启用才分配），所以"从没用过
//   探针"的运行连显存占用都与引入本模块之前相同。
//
// ---- 已知取舍（v1，都有明确升级路径）----
//   · 捕获**不跑局部光源阴影与分簇 compute**：两者用的是全局唯一的一份
//     资源（阴影图 / 簇缓冲），按面各跑一遍会把主视口那份写坏。
//     温度：探针捕获里只有方向光 + IBL + 自发光 + 天空。
//     升级路径：给捕获路径单独一套簇缓冲（或改成 R7-1 的声明式资源）。
//   · 粗糙度混合只在**探针之间**（边界淡出）；SSR 与探针的权重混合
//     是"探针替换 IBL 镜面项、SSR 保持加色"（见 pbr.frag 的说明）——
//     精确混合需要全屏 pass 拿到材质粗糙度，而现在的 SSR v1 就没有。
//   · 捕获是**整帧一次性做 6 面**（不是分帧摊开）：确定性优先，
//     自动化下每次捕获的帧号与内容都可复现。
// ============================================================

#include "rhi/VulkanCommon.h"

#include <memory>
#include <vector>

namespace rhi {
class Device;
class CommandPool;
class ComputePipeline;
class Buffer;
class DescriptorAllocator;
} // namespace rhi

namespace render {

class HdrTarget;

// 一个探针的 GPU 侧记录（std430，与 pbr.frag 里同名的 struct 逐字对齐）。
//
// ⚠ 改这里必须同步改 assets/shaders/pbr.frag 的 Probe —— 有一处
//   static_assert 钉住 C++ 侧的大小，但**字段顺序与语义**只能靠人同步。
struct ProbeGpu {
    glm::mat4 worldToLocal;  // 64B  世界 → 探针局部（只含旋转 + 平移）
    // xyz = 盒体半轴（沿探针局部轴，米）；w = 边界混合距离（米）
    glm::vec4 extents;       // 16B
    // x = 强度倍率  y = 是否启用(0/1)  z = 是否盒投影(0/1)  w = 保留
    glm::vec4 params;        // 16B
    glm::vec4 worldPos;      // 16B  xyz = 探针世界坐标（盒投影用），w=保留
};
// std430 下 64 + 16 + 16 + 16 = 112：mat4 的对齐就是 16，尾部没有填充。
static_assert(sizeof(ProbeGpu) == 112, "ProbeGpu 布局变了，必须同步 pbr.frag");

// 探针系统的设置。前四项接到 FeatureRegistry（Engine 面板 + env + 序列化），
// 后三项是"编译期"的容量/质量，只走 env 与代码默认值。
struct ProbeSettings {
    bool enabled = false;          // 总开关（默认关 = 基线不变）
    float intensity = 1.0f;        // 全局反射强度倍率
    int captureSize = 128;         // 每面捕获分辨率（换它会重建资源）
    // 定期重捕周期（帧）。**0 = 关**（默认）—— 只在"探针数据变了 /
    // 被显式请求 / 第一次"时烘。静态场景里定期重捕的产物与上一份逐位
    // 相同，纯属白烧带宽；动态场景才给它一个非零值。
    int autoCaptureFrames = 0;

    // 容量与质量（不接 UI：它们决定显存与烘一次的时间，属于"装机参数"）
    uint32_t maxProbes = 4;
    uint32_t prefilterSize = 64;
    uint32_t prefilterMips = 5;
    uint32_t prefilterSamples = 256;
};

class ReflectionProbeSystem {
public:
    ReflectionProbeSystem(rhi::Device& device, rhi::CommandPool& cmdPool,
                          const ProbeSettings& settings,
                          uint32_t framesInFlight = 2);
    ~ReflectionProbeSystem();

    ReflectionProbeSystem(const ReflectionProbeSystem&) = delete;
    ReflectionProbeSystem& operator=(const ReflectionProbeSystem&) = delete;

    // ---- 资源生命周期（内部含 waitIdle；只在"首次启用 / 尺寸变了"时调）----
    bool ensureResources();
    void destroyResources();
    bool ready() const { return m_ready; }
    // 换捕获分辨率：下一次 ensureResources 会重建
    void invalidateResources() { m_needRebuild = true; }
    // 更新设置。会改**显存布局**的字段（捕获分辨率 / 容量 / 预过滤质量）
    // 一变就标记重建，下一次 ensureResources 重新分配；enabled / intensity
    // 只影响上传与着色，不触发重建。编辑器面板改参数走这里。
    void setSettings(const ProbeSettings& s) {
        const bool rebuild = s.captureSize != m_settings.captureSize ||
                             s.maxProbes != m_settings.maxProbes ||
                             s.prefilterSize != m_settings.prefilterSize ||
                             s.prefilterMips != m_settings.prefilterMips ||
                             s.prefilterSamples != m_settings.prefilterSamples;
        m_settings = s;
        if (rebuild) m_needRebuild = true;
    }
    // 捕获用的离屏目标的采样数**必须与主视口一致**（render pass 兼容性 ⇒
    // 前向管线可复用）。只在 ensureResources 之前有意义。
    void setCaptureSamples(VkSampleCountFlagBits s) {
        if (s == m_captureSamples) return;
        m_captureSamples = s;
        m_needRebuild = true;
    }

    // ---- 每帧：把探针表写进本帧槽的 SSBO ----
    // probes.size() 是**实际**探针数（上限 maxProbes，超出部分丢弃并 WARN）。
    void upload(const std::vector<ProbeGpu>& probes, uint32_t frameIndex);

    uint32_t probeCount() const { return m_probeCount; }

    // ---- 捕获 ----
    // 6 个面共享的唯一一张离屏目标（正方形，分辨率 = captureSize）。
    HdrTarget* captureTarget() { return m_captureTarget.get(); }
    VkExtent2D captureExtent() const;

    // 探针 p 的捕获开始：把捕获立方体整张转到 TRANSFER_DST（内容丢弃，
    // 6 个面都会被覆盖）。
    void beginCapture(VkCommandBuffer cmd, uint32_t probe);
    // 把刚才渲进 captureTarget() 的那一面拷进捕获立方体的第 face 层。
    // 内部做 resolve 视图的 SHADER_READ_ONLY → TRANSFER_SRC 迁移。
    void copyFace(VkCommandBuffer cmd, uint32_t face);
    // 6 面都拷完：捕获立方体转只读 → 逐 mip 跑 GGX 预过滤写进
    // cube array 的 [6p, 6p+6) 层 → 完成。可选回读自检。
    void finishCapture(VkCommandBuffer cmd, uint32_t probe);

    // ---- 采样资源（Renderer 写进 set 0 的 binding 23）----
    VkSampler sampler() const { return m_sampler; }
    VkImageView cubeArrayView() const {
        return m_ready ? m_prefilter.sampleView : VK_NULL_HANDLE;
    }
    // 关着 / 没就绪时绑的缺省 cube array（1×1，6 层，全黑）
    VkImageView fallbackView() const { return m_fallback.view; }
    VkSampler fallbackSampler() const { return m_fallback.sampler; }
    float maxLod() const {
        return static_cast<float>(m_settings.prefilterMips - 1u);
    }

    // ---- 探针数据 SSBO（set 0 binding 24）----
    VkBuffer dataBuffer(uint32_t frameIndex) const;
    uint32_t dataBufferSize() const { return m_dataByteSize; }

    // ---- 诊断（PROBE-DUMP 日志的数据源）----
    struct Stats {
        uint32_t probeCount = 0;
        uint32_t captures = 0;          // 本进程累计捕获次数
        uint32_t captureSize = 0;
        uint32_t prefilterSize = 0;
        uint32_t mips = 0;
        // 最近一次捕获的 6 面平均亮度（回读算出）。全 0 = 没回读
        // （除非开了 MYVK_PROBE_DBG）。顺序 = Vulkan 面序 +X -X +Y -Y +Z -Z
        float faceLum[6] = {};
        // 每面**正中心纹素**的线性 RGB。比平均亮度更能定性：
        // 侧面中心恰好落在视平线上（天空 / 墙），顶面中心是正上方、
        // 底面中心是正下方。面序映射错了会在这里立刻暴露
        // （平均亮度可能"看着挺合理"，中心色不会）。
        glm::vec3 faceMid[6]{};
        // 每面**非黑纹素计数**。这是三个回读指标里最不容易自欺的一个：
        // 平均亮度会被"大面积暗 + 一小块亮"压扁（亮块占 1/64 时均值
        // 只剩 0.3），中心纹素只有一个采样点、说不清整面。非黑计数
        // 直接回答"这一面到底有没有内容"。
        uint32_t faceNonZero[6] = {};
        bool readbackValid = false;
    };
    const Stats& stats() const { return m_stats; }
    void noteCaptureCounted() { ++m_stats.captures; }

    // 回读自检：把捕获立方体拷进 HOST_VISIBLE 缓冲（在 finishCapture 里录）。
    // 下一帧调 pollReadback() 解析出 6 面平均亮度。
    void pollReadback();

private:
    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView sampleView = VK_NULL_HANDLE;  // CUBE / CUBE_ARRAY
        // 写入视图：每 mip 一个 2D_ARRAY（imageStore 不能指定 mip）
        std::vector<VkImageView> storageViews;
        // 逐 (探针, mip) 的写入视图：只暴露该探针的 6 层
        std::vector<VkImageView> probeStorageViews;
        VkFormat format = VK_FORMAT_UNDEFINED;
        uint32_t size = 0;
        uint32_t mips = 1;
        uint32_t layers = 6;
    };

    void createCube(Image& img, uint32_t size, uint32_t mips, uint32_t layers,
                    VkImageUsageFlags extraUsage);
    void destroyImage(Image& img);

    void createFallback();
    void createPrefilterPipeline();
    void createDescriptors();

    rhi::Device& m_device;
    rhi::CommandPool& m_cmdPool;
    ProbeSettings m_settings;
    uint32_t m_framesInFlight = 2;

    bool m_ready = false;
    bool m_needRebuild = true;
    VkSampleCountFlagBits m_captureSamples = VK_SAMPLE_COUNT_1_BIT;
    uint32_t m_probeCount = 0;
    uint32_t m_dataByteSize = 0;

    // ① 捕获：单张 6 层立方体（所有探针轮流用，捕获完立刻烘）
    Image m_capture;
    // ② 预过滤结果：cube array，层 = 6 × maxProbes
    Image m_prefilter;
    // 离屏目标：6 个面共享（分辨率 = captureSize）
    std::unique_ptr<HdrTarget> m_captureTarget;

    VkSampler m_sampler = VK_NULL_HANDLE;

    // 缺省（未启用时的绑定目标）
    struct Fallback {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
    } m_fallback;

    // 预过滤管线（ibl_prefilter.comp，与 Environment 同源）
    std::unique_ptr<rhi::ComputePipeline> m_prefPipe;
    VkDescriptorSetLayout m_prefSetLayout = VK_NULL_HANDLE;  // 读 cube + 写 2D_ARRAY
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    // [probe][mip] 一个 set（binding1 = 该探针该 mip 的 6 层视图）
    std::vector<VkDescriptorSet> m_prefSets;

    // 探针数据 SSBO（每帧在途一块，HOST_VISIBLE 常驻映射）
    struct DataSlot {
        std::unique_ptr<rhi::Buffer> buffer;
        void* mapped = nullptr;
    };
    std::vector<DataSlot> m_data;

    // 回读（MYVK_PROBE_DBG）：捕获立方体 → 主机缓冲，下一帧解析
    std::unique_ptr<rhi::Buffer> m_readback;
    void* m_readbackMapped = nullptr;
    bool m_readbackPending = false;
    bool m_readbackEnabled = false;

    Stats m_stats;
};

} // namespace render
