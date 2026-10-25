#pragma once
// ============================================================
// render/PostProcess —— Forward 之后的全部 Pass
//
// 编排（全部在同一个命令缓冲内，靠 render pass 的依赖自动同步）：
//   1. Bloom 亮部提取   HDR 全分辨率   → bloomA（半分辨率）
//   2. 高斯模糊 H       bloomA         → bloomB
//   3. 高斯模糊 V       bloomB         → bloomA
//   4. 合成 + Tonemap   HDR + bloomA   → 输出
//
// ---- 两种输出模式（编辑器视口的关键）----
//
//   A) 场景直写交换链（游戏模式，viewportExtent = {0,0}）
//      · 合成 Pass 的 framebuffer 就是交换链图像
//      · ImGui 通过 overlay 回调画在**同一个** Pass 内
//      · 这就是 PostProcess 过去唯一的行为，游戏路径零变化
//
//   B) 场景写离屏图（编辑器模式，viewportExtent != {0,0}）
//      · 合成 Pass 写入一张 viewport 大小的离屏图（format 与交换链一致）
//      · ImGui 拿这张图当纹理，用 ImGui::Image() 画在 Viewport 面板里
//      · 再单独跑一个 Pass：清交换链 + 画 ImGui
//
//   为什么离屏图格式取交换链格式（B8G8R8A8_SRGB）而不是 HDR 格式：
//     ImGui 采样 SRGB 视图时硬件自动做 sRGB→线性，写回 SRGB 交换链时
//     硬件再做线性→sRGB —— 编码两次、解码一次，净结果恰好等价。
//     若换成线性格式，中间那次 sRGB 编码就会丢高光/出现色偏。
//
//   为什么离屏要独立一条合成管线（m_postPipelineOffscreen）：
//     Vulkan 的图形管线绑定在具体的 VkRenderPass 上。离屏 Pass 的
//     finalLayout 是 SHADER_READ_ONLY_OPTIMAL（要被 ImGui 采样），
//     与交换链 Pass 的 PRESENT_SRC_KHR 不同 → 属于两个 RenderPass，
//     必须各建一条管线。多一条全屏三角形管线，代价可以忽略。
//
//   为什么不复用交换链 RenderPass 给离屏用：
//     finalLayout 会被强制成 PRESENT_SRC_KHR，那张图就没法被采样了。
//
// 为什么 Bloom 图像每个 frame-in-flight 各一份：
//   同 ShadowPass/HdrTarget —— 两条命令缓冲可能同时在 GPU 上跑。
//
// 为什么本类自带 DescriptorAllocator：
//   交换链 resize 后 HDR/bloom 的 VkImageView 会变，必须重建描述符集。
//   若复用 Renderer 的池，重置池会连同**纹理**的描述符集一起失效。
//   独立一个池 → resize 时整池丢弃重建，互不影响。
// ============================================================

#include "rhi/VulkanCommon.h"

#include <functional>
#include <memory>
#include <vector>

namespace rhi {
class Device;
class Pipeline;
class ComputePipeline;
class Buffer;
class CommandPool;
class DescriptorAllocator;
}

namespace render {

// 后处理参数（ImGui 实时可调）
struct PostSettings {
    float exposure = 1.0f;        // 曝光（乘在 tonemap 之前）
    float bloomThreshold = 1.2f;  // 亮部阈值（>1 → 只让真正的高光发光）
    float bloomKnee = 0.4f;       // 软过渡带宽
    float bloomStrength = 0.30f;  // bloom 叠加强度（0 = 关闭）
    int   tonemapMode = 0;        // 0=ACES 1=Reinhard 2=Clamp
    bool  fxaa = false;           // FXAA 后处理抗锯齿（MYVK_FXAA=1 默认开）

    // ---- 自动曝光（直方图 eye adaptation，R1-2）----
    // 与 exposure **二选一**：
    //   · autoExposure = false → 用 exposure（手动滑条），与引入本模块
    //     之前逐像素一致（回归基线）；
    //   · autoExposure = true  → 用后台测出的自适应值；exposure 滑条
    //     保留原值（当"起始值"与"关掉后的回退值"，不会被覆盖）。
    //
    //   exposureKey   = 目标中间灰：让"平均亮度 × 曝光"落在这里。
    //                   0.18 是摄影上的中灰（UE 默认量级）。
    //   exposureSpeed = 适应速度（1/秒）：越大收敛越快、越容易"呼吸"。
    //   exposureMin/Max = 曝光夹紧（防止对着纯黑/纯亮场景发散）。
    //   exposureBias  = EV 偏置（+1 亮一档）。后处理体积可覆盖。
    bool  autoExposure = false;
    float exposureKey = 0.18f;
    float exposureSpeed = 3.0f;
    float exposureMin = 0.02f;
    float exposureMax = 8.0f;
    float exposureBias = 0.0f;

    // ---- 色彩分级 LUT（32³，R1-3）----
    // lift / gamma / gain 各三通道 + 对比度 + 饱和度 → 在 CPU 侧烘成
    // 32³ 3D LUT → 合成 pass 里一次三线性采样（tonemap 之后、暗角之前）。
    //
    // 关闭（默认）→ 合成走与引入前**逐像素相同**的路径（回归基线）。
    // 开启且参数全默认 → 烘出恒等 LUT，画面几乎不变（见着色器注释：
    // 恒等是仿射映射，硬件三线性可精确重建）。
    bool  colorGrading = false;
    float cgLutBlend = 1.0f;                              // LUT 混合权重
    float cgLift[3]  = {0.0f, 0.0f, 0.0f};                // 阴影抬升
    float cgGamma[3] = {1.0f, 1.0f, 1.0f};                // 中间调 gamma
    float cgGain[3]  = {1.0f, 1.0f, 1.0f};                // 高光增益
    float cgContrast = 1.0f;                              // 对比度（绕 0.5）
    float cgSaturation = 1.0f;                            // 饱和度（绕亮度）

    // ---- 动态分辨率（DRS，R1-4）----
    // 场景按 renderScale 缩小渲染（HdrTarget / 深度 / 屏幕空间链 / TAA /
    // bloom 全部跟着缩），合成 pass 再升采样回全分辨率的输出。
    // 关闭（默认）→ renderScale = 1.0 → **逐像素回到基线**。
    //
    //   dynamicRes   = 总开关。关着时 renderScale 强制 1.0。
    //   drsAuto      = true 按 GPU 帧耗时自动调；false 用 renderScale 手动值。
    //   renderScale  = 当前生效的缩放比（自动模式下由控制器更新）。
    //   drsTargetMs  = 目标 GPU 帧时间（ms）—— 自动模式的调节目标。
    //   drsMin/MaxScale = 缩放夹紧。
    //   drsSharpness = 升采样的边缘锐度（0 = 纯双线性）。
    bool  dynamicRes = false;
    bool  drsAuto = true;
    float renderScale = 1.0f;
    float drsTargetMs = 16.6f;
    float drsMinScale = 0.5f;
    float drsMaxScale = 1.0f;
    float drsSharpness = 0.6f;

    // ---- 描边（NPR，2026-10-25）----
    // 数据来源是**深度预通道的几何缓冲**（世界法线 + 线性距离），
    // 不是另画一遍外壳。判据两路取 max：
    //   深度边缘 —— 相邻像素的相对距离差（|d0-d1|/min(d0,d1)）
    //   法线边缘 —— 1 - dot(n0,n1)
    // 相对深度差的意义：同一个阈值在 1 米和 50 米处效果一致。用绝对差
    // 的话，"贴着镜头描边粗成一团、远处完全没有"。
    //
    // 关闭（默认）→ 合成走与引入前**逐像素相同**的路径（回归基线）。
    // 这也是"关功能必须回到基线"这条纪律的可断言版本。
    bool  outline = false;
    float outlineWidth = 1.5f;        // 采样步长（渲染分辨率下的像素）
    // 默认 0.05 而不是 0.02：**相对**深度差在掠射角的大平面上并不接近 0
    // （看地面时相邻像素的距离差与距离成正比，相对差是个常数）。
    // 0.02 会把远处地面的一堆像素点判成边缘 → 画面上一层"星点"。
    float outlineDepthThreshold = 0.05f;   // 相对深度差阈值
    float outlineNormalThreshold = 0.35f;  // 1-dot 阈值（≈ 49°）
    float outlineStrength = 1.0f;     // 线的不透明度（0 = 不压暗）
    float outlineColor[3] = {0.06f, 0.05f, 0.08f};  // 线的颜色（显示域）
};

// 由"输出分辨率"与缩放比算出"场景渲染分辨率"。
// 规则：四舍五入、下限 16（避免极小图）、上限不超过输出（不放大渲染）。
// RenderTarget（HdrTarget 尺寸）与 PostProcess（内部 extent）共用同一个
// 函数 —— 两处必须逐位一致，否则 HdrTarget 与 PostProcess 对不上。
VkExtent2D scaledRenderExtent(VkExtent2D out, float scale);

// 相机级"显示域"效果（色差 + 暗角）—— 走合成的第二个推常量 vec4。
// 由 Renderer 每帧从 CameraEffectSettings（含后处理体积覆盖）同步进来。
// 景深 / 运动模糊不在这里（它们在 FrameGraph 链上，需要深度与相机矩阵）。
struct PostCameraFx {
    float chroma = 0.0f;          // 径向色差强度（0 = 关）
    float vignette = 0.25f;       // 暗角强度（引入本模块前就在合成里，默认 0.25）
    float vignettePower = 1.0f;   // 暗角的径向幂（>1 更压向边缘）
};

class PostProcess {
public:
    // Bloom 链跑在 1/2 分辨率上：开销约 1/4，光晕更柔
    static constexpr uint32_t kBloomDownscale = 2;

    PostProcess(rhi::Device& device, VkFormat swapchainFormat,
                VkFormat hdrFormat, uint32_t framesInFlight);
    ~PostProcess();

    PostProcess(const PostProcess&) = delete;
    PostProcess& operator=(const PostProcess&) = delete;

    // 创建/重建。
    //   swapchainViews / swapchainExtent：交换链（UI 与模式 A 的场景输出）
    //   hdrColorViews                   ：HdrTarget 的 resolve 视图（每帧一张）
    //   viewportExtent                  ：{0,0} = 模式 A；否则模式 B 的离屏尺寸
    //   geometryViews                   ：HdrTarget 的几何缓冲视图（法线+线性
    //                                     深度，每帧一张）。**空 = 没有描边输入**
    //                                     —— 合成里描边整段跳过，画面与引入
    //                                     描边之前逐像素相同（回归基线）。
    void recreate(const std::vector<VkImageView>& swapchainViews,
                  VkExtent2D swapchainExtent,
                  const std::vector<VkImageView>& hdrColorViews,
                  VkExtent2D viewportExtent,
                  const std::vector<VkImageView>& geometryViews = {});

    // 是否处于"场景 → 离屏图"模式（编辑器视口）
    bool offscreen() const { return m_splitOutput; }
    // 描边输入是否就绪（几何缓冲的每帧视图都拿到了）。
    // 诊断用：面板上画着"描边已开"但这条是 false 的话，说明几何缓冲没接上
    // —— 那是最难查的一类（开关亮着、画面没线、日志无错）。
    bool outlineReady() const { return !m_geoSets.empty() && m_geoSets[0] != VK_NULL_HANDLE; }
    // 场景渲染分辨率（**缩放后**；模式 A = 交换链尺寸 × renderScale，
    // 模式 B = 视口尺寸 × renderScale）。HdrTarget / 链 / bloom 都用它。
    VkExtent2D sceneExtent() const { return m_sceneExtent; }
    // 全分辨率输出尺寸（交换链 / 视口本身）—— 合成 / FXAA / 离屏输出图用它。
    VkExtent2D outputExtent() const { return m_outExtent; }
    VkExtent2D swapchainExtent() const { return m_swapExtent; }
    // 离屏输出视图（供 ImGui_ImplVulkan_AddTexture 注册成纹理）
    VkImageView sceneOutputView(uint32_t frameIndex) const;
    VkFormat sceneOutputFormat() const { return m_swapFormat; }

    // 录制场景链（bloom + 合成）。overlay 只在模式 A 下被调用。
    //
    // gameUi（U2 起）：**游戏 UI / HUD** 的绘制回调，在**两种模式**下都会被
    // 调用，且一定排在 overlay 之前（游戏 UI 在底层、编辑器面板盖在上面）。
    //
    // ⚠ 为什么必须挂在**这个** Pass 内部，而不是"Post 之后再开一个 pass"：
    //   两个输出 pass 的 finalLayout 是写死的（模式 A = PRESENT_SRC_KHR）。
    //   pass 结束时图已经在 PRESENT_SRC，往同一张交换链图再开一个 pass
    //   要重新 transition，而 PRESENT_SRC 作为 initialLayout 的 pass 限制
    //   很严 —— 验证层会直接报 VUID。所以唯一干净的位置就是这里：
    //   Post 输出 pass 内部、ImGui overlay 之前。
    //
    // ⚠ 模式 B（编辑器视口）也传：那是"在视口里预览 HUD"的能力，
    //   由 `ui.hudInEditor` 控制（默认关 —— 开了会让编辑器截图像素整体漂）。
    //
    // ⚠⚠ 为什么 gameUi 的签名比 overlay **多一个 VkRenderPass**：实现方
    //   （ui2/UiRenderer）的管线和 pass 是**强绑定**的 —— 两个输出 pass 除了
    //   finalLayout，还差一条 subpass dependency，而 dependency **参与**兼容性
    //   判定 → 一份管线两模式通用是错的（`VUID-vkCmdDraw-renderPass-02684`，
    //   实测 2028 条/秒）。所以本类在两个包装里各给一个**确定**的 pass 句柄，
    //   调用方只管拿它去取自己的管线，不用猜当前在哪个模式。
    using GameUiDrawFn = std::function<void(VkCommandBuffer, VkRenderPass)>;
    void renderScene(VkCommandBuffer cmd, uint32_t imageIndex,
                     uint32_t frameIndex,
                     const std::function<void(VkCommandBuffer)>& overlay,
                     const GameUiDrawFn& gameUi = {});

    // 模式 B 的第二步：清交换链 + 画 ImGui。
    void renderUiToSwapchain(VkCommandBuffer cmd, uint32_t imageIndex,
                             const std::function<void(VkCommandBuffer)>& overlay);

    // ImGui 后端的管线绑定在这个 RenderPass 上（模式 A 的合成 Pass 与
    // 模式 B 的 UI Pass 共用同一个 RenderPass 对象 —— 两者附件描述完全
    // 一致：交换链格式、单采样、CLEAR、PRESENT_SRC）。
    VkRenderPass renderPass() const { return m_postRenderPass; }
    VkExtent2D bloomExtent() const { return m_bloomExtent; }

    PostSettings& settings() { return m_settings; }
    const PostSettings& settings() const { return m_settings; }

    // 相机级显示域效果（色差 / 暗角）。Renderer 每帧写 —— 含后处理体积
    // 的覆盖结果。默认 = 只在合成里应用 0.25 的暗角（既有基线）。
    void setCameraFx(const PostCameraFx& fx) { m_cameraFx = fx; }
    const PostCameraFx& cameraFx() const { return m_cameraFx; }

    // ============================================================
    // 自动曝光（R1-2）
    // ============================================================
    // 每帧在 **in-flight 栅栏之后、录制命令之前** 调用：
    //   · 回读"两帧前"写好的直方图结果（那时该帧的栅栏已过，读到的一定
    //     是完整数据 —— 与 ClusteredLighting::beginFrame 同一套时序）；
    //   · 求目标曝光、按 dt 做指数平滑。
    // 录制命令时用的就是这个平滑后的值（recordComposite / 亮部提取）。
    void beginFrame(uint32_t frameIndex, float dt);

    // 回归钩子：跳过回读，把"测量到的平均亮度"直接钉死成给定值
    // （>0 生效）。让收敛曲线与场景内容解耦，断言才是确定性的。
    void setAutoExposureSimLum(float lum) { m_aeSimLum = lum; }

    // 本帧生效的曝光：autoExposure 关 ⇒ **精确等于** settings().exposure
    float effectiveExposure() const {
        return m_settings.autoExposure ? m_aeValue : m_settings.exposure;
    }
    // 诊断读数（日志 / 验证脚本用）
    float autoExposureAvgLog2() const { return m_aeAvgLog2; }
    float autoExposureAvgLum() const;  // exp2(avgLog2)
    float autoExposureSamples() const { return m_aeSamples; }
    float autoExposureTarget() const { return m_aeTarget; }
    float autoExposureTargetRaw() const { return m_aeTargetRaw; }
    float autoExposureValue() const { return m_aeValue; }

    // ============================================================
    // 色彩分级 LUT（32³，R1-3）
    // ============================================================
    // LUT 每轴的纹素数。
    static constexpr uint32_t kLutSize = 32;
    // LUT 资源是否就绪（构造时一次建好，与场景分辨率无关）。
    bool lutReady() const { return m_lutView != VK_NULL_HANDLE; }
    // 诊断：最近一次烘焙出来的 LUT 是否等价于恒等映射（参数全默认时）。
    bool lutIdentity() const { return m_lutIdentity; }
    // 强制下一帧重烘（参数改了但没走 beginFrame 的差异检测时用；面板
    // 改动无需调用 —— beginFrame 会自动比对烘焙快照）。
    void markLutDirty() { m_lutDirty = true; }

    // ============================================================
    // 动态分辨率（DRS，R1-4）
    // ============================================================
    // 当前生效的缩放比（= settings().renderScale）。RenderTarget 建
    // HdrTarget 时读它，必须与 PostProcess 内部的 m_sceneExtent 一致。
    float renderScale() const { return m_settings.renderScale; }

    // 每帧在 **in-flight 栅栏之后** 调用，喂入平滑后的 GPU 帧时间（ms，
    // <=0 表示时戳不可用）。内部按 drsTargetMs 调整 renderScale（指数
    // 趋近 + 死区 + 量化 + 冷却，避免每帧重建资源）。
    // 返回 true 表示"量化后的缩放比变了" → 调用方必须走重建路径
    // （HdrTarget / 屏幕空间链 / 分簇 / PostProcess 全都要重新分配）。
    bool updateDynamicResolution(float gpuFrameMs);
    // 诊断读数（日志 / 验证脚本）
    float drsSmoothedMs() const { return m_drsSmoothedMs; }
    float drsLastFrameMs() const { return m_drsLastMs; }

private:
    struct BloomFrame {
        VkImage aImage = VK_NULL_HANDLE;
        VkDeviceMemory aMemory = VK_NULL_HANDLE;
        VkImageView aView = VK_NULL_HANDLE;
        VkFramebuffer aFb = VK_NULL_HANDLE;

        VkImage bImage = VK_NULL_HANDLE;
        VkDeviceMemory bMemory = VK_NULL_HANDLE;
        VkImageView bView = VK_NULL_HANDLE;
        VkFramebuffer bFb = VK_NULL_HANDLE;
    };

    // 模式 B：每帧一张 viewport 大小的场景输出图
    struct SceneOutputFrame {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFramebuffer fb = VK_NULL_HANDLE;
    };

    // FXAA 中间图：合成（tonemap）的输出、FXAA pass 的输入。
    // 两种模式都常驻 —— 开关 FXAA 只是录制路径分支，不触发资源重建，
    // ImGui 的 checkbox 才能做到实时切。
    struct FxaaFrame {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        // 合成 pass 的目标 framebuffer（绑 m_offscreenRenderPass ——
        // 无论哪种模式，这张图都要被 FXAA 采样，finalLayout 必须是
        // SHADER_READ_ONLY）
        VkFramebuffer fb = VK_NULL_HANDLE;
    };

    void createSetLayoutAndSampler();
    void createRenderPasses();
    void createPipelines();
    void destroyBloom();
    void createBloomFrames();
    void createSwapFramebuffers(const std::vector<VkImageView>& views);
    void destroySwapFramebuffers();
    void createSceneOutputFrames();
    void destroySceneOutputFrames();
    void createFxaaFrames();
    void destroyFxaaFrames();
    void buildDescriptorSets(const std::vector<VkImageView>& hdrColorViews,
                             const std::vector<VkImageView>& geometryViews);
    void destroyDescriptors();

    // ---- 自动曝光的亮度测量链（R1-2）----
    // 资源尺寸**固定**（与场景分辨率无关）→ 只在构造/析构里建销，
    // 不参与 resize / MSAA 变化的 recreate（少一大块状态）。
    //   场景色 --(片元降采样)--> 64×64 块平均 log2 亮度图
    //        --(compute 直方图)--> 128 箱 bins
    //        --(compute 归约+百分位裁剪)--> 平均 log2 亮度（HOST_VISIBLE）
    void createAeResources();
    void destroyAeResources();
    void buildAeDescriptorSets();
    // 录制三步测量链（只在 autoExposure 开时被调用）
    void recordAutoExposure(VkCommandBuffer cmd, uint32_t fi);

    // ---- 色彩分级 LUT（R1-3）----
    // 资源尺寸固定（32³）→ 构造/析构里建销，不参与 resize。
    void createLutResources();
    void destroyLutResources();
    // 把 settings 里的 lift/gamma/gain/contrast/saturation 烘成 32³ 格点，
    // 上传到 m_lutImage。waitIdle 后走一次性命令缓冲（staging 拷贝 + 布局
    // 迁移）。只在参数变化时调用（beginFrame 里比对快照）。
    void bakeLut();
    // 参数快照与当前 settings 不一致 → 重烘
    bool gradingChanged() const;
    void snapshotGrading();
    // beginFrame 里调用：脏了就重烘
    void rebakeLutIfDirty();

    // 亮度图尺寸 / 直方图箱数 / 每纹素分层采样数（log2 轴范围）
    static constexpr uint32_t kLumWidth = 64;
    static constexpr uint32_t kLumHeight = 64;
    static constexpr uint32_t kAeBins = 128;
    static constexpr uint32_t kLumSamplesPerTexel = 4;
    static constexpr float kAeLogLo = -10.0f;  // 2^-10 ≈ 0.001
    static constexpr float kAeLogHi = 10.0f;   // 2^+10 = 1024

    struct LumFrame {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFramebuffer fb = VK_NULL_HANDLE;  // 挂在 m_lumRenderPass 上
    };

    // 亮度图专用 RenderPass：附件与 bloom 图同格式（省一个格式支持性风险），
    // 但同步语义是**为 compute 写的** —— 见 PostProcess.cpp 的注释。
    VkRenderPass m_lumRenderPass = VK_NULL_HANDLE;
    std::unique_ptr<rhi::Pipeline> m_lumPipeline;           // 片元降采样
    std::unique_ptr<rhi::ComputePipeline> m_histPipeline;   // 直方图
    std::unique_ptr<rhi::ComputePipeline> m_avgPipeline;    // 归约
    VkDescriptorSetLayout m_aeSetLayout = VK_NULL_HANDLE;
    std::vector<LumFrame> m_lum;
    std::vector<std::unique_ptr<rhi::Buffer>> m_aeBins;
    std::vector<std::unique_ptr<rhi::Buffer>> m_aeResult;
    std::vector<float*> m_aeResultMapped;
    std::vector<VkDescriptorSet> m_aeSets;

    // CPU 侧的自适应状态
    float m_aeValue = 1.0f;       // 平滑后的当前曝光
    float m_aeTarget = 1.0f;      // 本帧目标（夹紧后）
    float m_aeTargetRaw = 1.0f;   // 本帧目标（夹紧前）
    float m_aeAvgLog2 = 0.0f;
    float m_aeSamples = 0.0f;
    float m_aeSimLum = 0.0f;      // >0 → 跳过回读（回归钩子）
    bool  m_aeWasOn = false;      // 上升沿检测：关→开时以手动值为起点

    // ---- 色彩分级 LUT（R1-3）----
    // 32³ 3D 纹理 + 一个独立描述符集（set 2，单 binding）。
    // 为什么不用 rhi::Image：那是 2D-only 的封装，不支持 VK_IMAGE_TYPE_3D。
    // 为什么格式是 16F：8-bit 会在暗部量化出可见台阶（恒等 LUT 的采样
    // 误差被 sRGB 编码在近黑处放大）；16F 又能被绝大多数设备三线性过滤。
    // 为什么单张（不分帧）：LUT 在帧间恒定，读出是只读 → 全帧槽共用一个集。
    VkImage m_lutImage = VK_NULL_HANDLE;
    VkDeviceMemory m_lutMemory = VK_NULL_HANDLE;
    VkImageView m_lutView = VK_NULL_HANDLE;
    VkFormat m_lutFormat = VK_FORMAT_UNDEFINED;
    VkDescriptorSet m_lutSet = VK_NULL_HANDLE;
    std::unique_ptr<rhi::CommandPool> m_uploadPool;   // 仅烘焙时用（一次）
    bool m_lutDirty = true;       // 构造后第一次 beginFrame 必烘
    bool m_lutIdentity = true;    // 最近一次烘焙是否恒等（诊断）
    // 上一次烘焙用的参数快照（beginFrame 比对，变了才重烘）
    float m_cgLift[3] = {0.0f, 0.0f, 0.0f};
    float m_cgGamma[3] = {1.0f, 1.0f, 1.0f};
    float m_cgGain[3] = {1.0f, 1.0f, 1.0f};
    float m_cgContrast = 1.0f;
    float m_cgSaturation = 1.0f;

    // ---- 动态分辨率（DRS，R1-4）的 CPU 侧状态 ----
    // 平滑后的 GPU 帧时间（指数滑动平均）+ 最近一次原始读数 + 冷却计数。
    // 冷却的意义：缩放比一变就要 waitIdle + 重建一整批图（HDR / 深度 /
    // bloom / 链），太频繁会明显卡顿 —— 因此量化到 1/16 步进 + 至少隔
    // 若干帧才允许再变。
    float m_drsSmoothedMs = 0.0f;
    float m_drsLastMs = 0.0f;
    int   m_drsCooldown = 0;

    // 全屏三角形 Pass 的通用管线构造（无顶点缓冲 / 无深度）
    std::unique_ptr<rhi::Pipeline> makeFullscreenPipeline(
        VkRenderPass renderPass, const char* fragSpv, uint32_t setCount,
        uint32_t pushSize);

    // 在指定 render pass 内画一个全屏三角形到指定 framebuffer
    // （bloom 三连传 m_bloomRenderPass；FXAA 按目标传 m_postRenderPass 或
    // m_offscreenRenderPass —— 与合成 pass 的双管线结构完全对偶）
    void drawFullscreen(VkCommandBuffer cmd, VkRenderPass renderPass,
                        rhi::Pipeline& pipeline, VkFramebuffer fb,
                        VkExtent2D extent, const VkDescriptorSet* sets,
                        uint32_t setCount, const void* push, uint32_t pushSize);

    // 合成 + Tonemap。targetExtent/RP/pipeline 由调用方按模式选择。
    // overlay 非空时在其中绘制（必须落在同一个 RenderPass 实例内）。
    void recordComposite(VkCommandBuffer cmd, VkFramebuffer fb,
                         VkRenderPass rp, rhi::Pipeline& pipeline,
                         VkExtent2D targetExtent, uint32_t frameIndex,
                         const std::function<void(VkCommandBuffer)>* overlay);

    // 全屏 Pass 的通用录制（recordComposite 的形状抽取，FXAA pass 也用它）：
    // Begin(rp) → bind → viewport/scissor → 绑 sets → push → draw →
    // [overlay] → End。overlay 非空时**必须**画在 End 之前 —— 管线绑在
    // 本 RenderPass 上，重开一个 Pass 会把刚画好的内容清掉。
    void recordFullscreenPass(VkCommandBuffer cmd, VkFramebuffer fb,
                              VkRenderPass rp, rhi::Pipeline& pipeline,
                              VkExtent2D targetExtent,
                              const VkDescriptorSet* sets, uint32_t setCount,
                              const void* push, uint32_t pushSize,
                              const std::function<void(VkCommandBuffer)>*
                                  overlay);

    rhi::Device& m_device;
    VkFormat m_swapFormat = VK_FORMAT_UNDEFINED;
    VkFormat m_hdrFormat = VK_FORMAT_UNDEFINED;
    uint32_t m_framesInFlight = 2;

    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    // 几何缓冲（描边）专用的**最近邻**采样器 —— 深度/法线的边缘判据
    // 不能用线性过滤（会把边缘插成中间值 → 双轮廓）。
    VkSampler m_geoNearestSampler = VK_NULL_HANDLE;

    VkRenderPass m_bloomRenderPass = VK_NULL_HANDLE;
    VkRenderPass m_postRenderPass = VK_NULL_HANDLE;       // → 交换链（PRESENT_SRC）
    VkRenderPass m_offscreenRenderPass = VK_NULL_HANDLE;  // → 离屏（SHADER_READ）

    std::unique_ptr<rhi::Pipeline> m_brightPipeline;
    std::unique_ptr<rhi::Pipeline> m_blurPipeline;
    std::unique_ptr<rhi::Pipeline> m_postPipeline;           // 绑 m_postRenderPass
    std::unique_ptr<rhi::Pipeline> m_postOffscreenPipeline;  // 绑离屏 RenderPass
    // FXAA 两条（与合成的双管线对偶）：
    std::unique_ptr<rhi::Pipeline> m_fxaaPipeline;           // → 交换链（模式 A）
    std::unique_ptr<rhi::Pipeline> m_fxaaOffscreenPipeline;  // → 离屏（模式 B）

    std::unique_ptr<rhi::DescriptorAllocator> m_descriptors;

    bool m_splitOutput = false;         // 模式 B
    VkExtent2D m_swapExtent{0, 0};      // 交换链尺寸
    VkExtent2D m_outExtent{0, 0};       // 全分辨率输出（视口 / 交换链）
    VkExtent2D m_sceneExtent{0, 0};     // 场景渲染尺寸 = m_outExtent × renderScale
    VkExtent2D m_bloomExtent{0, 0};     // = m_sceneExtent / kBloomDownscale

    std::vector<BloomFrame> m_bloom;
    std::vector<VkFramebuffer> m_swapFramebuffers;
    std::vector<SceneOutputFrame> m_sceneOutput;
    std::vector<FxaaFrame> m_fxaa;

    // 每帧三个输入集，分别指向 HDR / bloomA / bloomB
    std::vector<VkDescriptorSet> m_hdrSets;
    std::vector<VkDescriptorSet> m_bloomASets;
    std::vector<VkDescriptorSet> m_bloomBSets;
    // 几何缓冲（set 3，描边输入）。空 = 没有输入，合成里描边整段跳过。
    std::vector<VkDescriptorSet> m_geoSets;
    // FXAA 的输入集（指向 fxaa 中间图）
    std::vector<VkDescriptorSet> m_fxaaSets;

    PostSettings m_settings;
    PostCameraFx m_cameraFx;
};

} // namespace render
