#pragma once
// ============================================================
// render/VolumetricFog —— Froxel 体积雾（R3-2）
//
// 位置：深度预通道之后、分簇 compute 之后、前向 Pass 之前。
// 输入：HdrTarget 的**单采样深度**（resolve 后的 depth，或 MSAA 深度）。
//       本模块只需要"每个 froxel 里最近表面的视图深度"来决定该 froxel
//       是否被几何体占据（不透明遮挡），进而把积分截断在表面之前。
// 输出：一张 3D 纹理（RGBA16F），轴 = (x, y, z)：
//         x/y = 屏幕瓦片索引（froxelX × froxelY，覆盖整屏）
//         z   = 视图空间深度上的指数分层（froxelZ 层，近密远疏）
//       每 texel 存 ( inscatter.rgb , transmittance )：
//         rgb = 从相机到该 froxel 入口处累积的**内散射**（已经过各段
//               消光衰减）
//         a   = 到该 froxel 入口处累积的**透射率**（≤ 1）
//       前向的 pbr.frag 用像素自己的 froxel 坐标采样，得到该像素处的
//       散射/透射，做 out = lit * T + inscatter。
//
// ---- 为什么是 froxel 而不是屏幕空间径向模糊 ----
//   原来的 fg_godrays.frag 只有一个方向（太阳）的屏幕空间径向模糊，
//   本质上假设"介质均匀 + 光源只有太阳"。Froxel 把世界空间切成
//   一个 3D 网格，逐格沿视线积分内散射，因此：
//     · 每盏灯的贡献按**世界坐标**算 —— 聚光灯的锥形有真实的局部形状；
//     · 复用分簇的灯数据 —— 每个 froxel 的视锥小块正好等于一个簇，
//       直接读 clusters[]/lightIndices[] 就知道哪些灯影响这一格；
//     · 介质密度可以是空间的函数（本实现用高度 + 局部雾团）。
//
// ---- 两趟 compute（各自 dispatch 1 次）----
//   ① fog_inject.comp   —— 每个 froxel：算体积属性（密度、吸收），
//                          并对"本 froxel 视锥小块"内的灯求一次
//                          内散射/单位长度的估计，写进 scratch 3D 图。
//                          复用分簇数据：froxel 的 xy 瓦片与簇的 xy
//                          瓦片一般不同尺寸，这里**独立**做球的
//                          视锥小块剔除（保守），不依赖簇网格对齐。
//   ② fog_integrate.comp —— 每个 (x, y) 列：沿 z 从近到远做一遍
//                          前向积分（inscatter += segScatter * T;
//                          T *= exp(-ext * segLen)），把结果写回 3D 图。
//
// 为什么拆两趟而不在一趟里做完：积分本质是**沿 z 的顺序扫描**，
// 每个 (x,y) 列只能有一个线程跑；而属性计算对每个 froxel 独立，
// 能铺满全部线程。拆开让两者各自用最合适的并行度。
//
// ---- 开/关语义 ----
//   默认关。关闭时：不录任何 compute，binding 16 绑一张 1×1×1 的
//   "透明" 3D 图（rgba = (0,0,0,1) → T=1, inscatter=0），pbr.frag
//   的雾合成退化成 out = lit，与基线逐位一致（与 SSAO 的缺省白图
//   同一套路）。
// ============================================================

#include "rhi/VulkanCommon.h"

#include <glm/glm.hpp>

#include <memory>
#include <vector>

namespace rhi {
class Device;
class CommandPool;
class ComputePipeline;
class DescriptorAllocator;
}

namespace render {

struct ScreenSpaceSettings;

class VolumetricFog {
public:
    // 与 fog_inject.comp / fog_integrate.comp 的 push_constant 严格对应
    // （32 字节）。所有参数要么是 froxel 网格尺寸，要么是每帧可能变的
    // 标量；矩阵/相机从 set 0 的 FrameUBO 读。
    struct PushConstants {
        glm::uvec4 grid;      // x=froxelX y=froxelY z=froxelZ w=flags(bit0=inject 用)
        glm::vec4 params;     // x=密度 y=散射反照率 z=高度衰减 w=雾团强度
        glm::vec4 params2;    // x=切片最近深度 y=zNear z=zFar w=相位 g
    };

    VolumetricFog(rhi::Device& device, rhi::CommandPool& cmdPool,
                  VkDescriptorSetLayout globalSetLayout,
                  uint32_t framesInFlight);
    ~VolumetricFog();

    VolumetricFog(const VolumetricFog&) = delete;
    VolumetricFog& operator=(const VolumetricFog&) = delete;

    // 尺寸变化时重建 3D 纹理与 2 个描述符集。froxel 网格尺寸由
    // 设置里的比例决定（见 ScreenSpaceSettings::vfog*），这里只关心
    // 屏幕尺寸（froxelX/Y 与屏幕宽高成比例）。
    // samples = HdrTarget 的采样数（决定深度图是 sampler2DMS 还是
    // sampler2D → 选哪份 .spv）。
    void recreate(VkExtent2D screenExtent, VkSampleCountFlagBits samples);

    // 每帧的深度可采样视图（HdrTarget::depthSampledView）。
    void setDepthViews(const std::vector<VkImageView>& views);

    // 每帧相机参数：切片最近深度（= 指数分层的起点，见 cluster_build.comp
    // 对"为什么不是 zNear"的说明）、zNear、zFar。
    void setCameraSlices(float sliceNear, float zNear, float zFar) {
        m_sliceNear = sliceNear;
        m_zNear = zNear;
        m_zFar = zFar;
    }

    // 建/重建两条 compute 管线（给热重载复用）。
    void createPipelines(bool msaaDepth);

    // 录制两趟 compute。前提：深度预通道已结束（深度处于可读布局）。
    void render(VkCommandBuffer cmd, uint32_t frameIndex,
                const ScreenSpaceSettings& s, VkDescriptorSet globalSet);

    // 前向 pbr.frag 采样用的 3D 图句柄（binding 16）
    VkImageView view(uint32_t frameIndex) const;
    VkSampler sampler() const { return m_sampler; }
    VkImageLayout imageLayout() const {
        return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    // 缺省"透明" 3D 图（1×1×1）。关闭时 Renderer 绑它。
    VkImageView neutralView() const { return m_neutralView; }
    VkSampler neutralSampler() const { return m_neutralSampler; }

    bool ready() const { return m_ready; }

    // 当前实际生效的 froxel 网格（诊断用）
    glm::uvec3 gridSize() const { return m_grid; }

private:
    struct FrameData {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    void createNeutralImage();
    void destroyFrames();
    void createFrames();

    rhi::Device& m_device;
    rhi::CommandPool& m_cmdPool;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_localSetLayout = VK_NULL_HANDLE;  // 本模块私有 set 1

    uint32_t m_framesInFlight = 1;
    VkExtent2D m_screenExtent{0, 0};
    glm::uvec3 m_grid{0, 0, 0};
    bool m_ready = false;
    bool m_msaaDepth = false;

    // 相机切片参数（每帧由 Renderer 喂）
    float m_sliceNear = 0.1f;
    float m_zNear = 0.1f;
    float m_zFar = 1000.0f;

    std::unique_ptr<rhi::ComputePipeline> m_injectPipeline;
    std::unique_ptr<rhi::ComputePipeline> m_integratePipeline;

    VkSampler m_sampler = VK_NULL_HANDLE;       // 线性采样 3D 图
    VkSampler m_depthSampler = VK_NULL_HANDLE;  // 最近邻取深度
    VkSampler m_neutralSampler = VK_NULL_HANDLE;
    VkImageView m_neutralView = VK_NULL_HANDLE;
    VkImage m_neutralImage = VK_NULL_HANDLE;
    VkDeviceMemory m_neutralMemory = VK_NULL_HANDLE;
    std::unique_ptr<rhi::DescriptorAllocator> m_descriptors;

    // 每个 frame-in-flight 一套：注入用的 set（写 scratch 3D 图）
    // 与积分用的 set（读写同一张图）。两张图交替会引入"读写同一
    // image"的屏障复杂度，这里用**一图两视图**：注入写 image，
    // 积分读 image 写回 image —— 靠两次全局屏障分隔（见 .cpp）。
    std::vector<FrameData> m_frames;
    std::vector<VkDescriptorSet> m_injectSets;     // 深度图 + 3D 图(写)
    std::vector<VkDescriptorSet> m_integrateSets;  // 3D 图(读) + 3D 图(写)

    // 3D 图的两个视图：一个用于 compute 的 storage 读写（image3D），
    // 一个用于前向的 sampled（image3D）—— 同一 image，不同用途。
    std::vector<VkImageView> m_storageViews;  // 与 m_frames 等长
};

} // namespace render
