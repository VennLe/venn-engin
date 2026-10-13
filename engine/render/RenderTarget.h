#pragma once
// ============================================================
// render/RenderTarget —— 通用离屏渲染目标
//
// 把"一套离屏链"打包成一个自管生命周期的对象：
//
//   HdrTarget（MSAA + HDR + 深度，前向渲染的输出）
//     + PostProcess（Bloom + 合成 + FXAA，输出到离屏图）
//       + 注册给 ImGui 的输出纹理（ImGui::Image 用）
//         + 尺寸/脏标记/绑定的场景与相机
//
// ---- 为什么要有这个类 ----
// 引入它之前，"编辑器视口"与"角色详情预览"是**两套硬编码**的离屏链：
// 各有一对 HdrTarget + PostProcess 成员、各自一组 setSize/apply/
// createTextures/destroyTextures/textureId，代码几乎逐行重复。加第三个
// 离屏视角（小地图 / 反射探针 / 画中画）就得再抄一遍。
//
// 现在两套都是本类的实例（Renderer 持有一个注册表）：
//   · 尺寸管理      setSize / apply / dirty
//   · 纹理注册      refreshTextures / textureId
//   · 多相机绑定    setScene / setCamera
// 都在这里，新增离屏视角 = new 一个实例 + 绑相机，不再动 Renderer 的
// 主链代码。
//
// ---- 两种输出模式（沿用 PostProcess 的既有语义）----
//   · active() == false  → 场景直写交换链（游戏模式）；
//   · active() == true   → 场景写离屏图，ImGui 采它（编辑器模式）。
//
// ---- 主视口的特殊之处 ----
// 主视口的 HdrTarget **深度图**被屏幕空间效果（SSAO / FrameGraph）
// 共享，且分簇光照按它的尺寸分配 —— 这些跨模块依赖仍由 Renderer 编排
// （apply 之后 Renderer 会补 recreateScreenSpace / lighting->recreate）。
// RenderTarget 只负责"这套 HdrTarget + PostProcess + 纹理"本身。
// ============================================================

#include "rhi/VulkanCommon.h"

#include <memory>
#include <vector>

namespace rhi {
class Device;
}

namespace ui {
class ImGuiManager;
}

namespace scene {
class Scene;
class Camera;
}

namespace render {

class HdrTarget;
class PostProcess;

class RenderTarget {
public:
    RenderTarget(rhi::Device& device, VkFormat swapFormat,
                 uint32_t framesInFlight, VkSampleCountFlagBits samples,
                 ui::ImGuiManager* imgui);
    ~RenderTarget();

    RenderTarget(const RenderTarget&) = delete;
    RenderTarget& operator=(const RenderTarget&) = delete;

    // ---- 尺寸 / 模式 ----
    // setSize(w,h)：w 或 h 为 0 → **直写交换链**（游戏模式，PostProcess
    // 走模式 A）；非零 → 离屏（编辑器模式，场景写一张 w×h 的图供 ImGui
    // 采样）。下限 16×16，避免面板被拖到极小时拿到非法 extent。
    void setSize(uint32_t w, uint32_t h);
    uint32_t width() const { return m_width; }
    uint32_t height() const { return m_height; }
    // 是否处于离屏模式（编辑器视口 / 预览）
    bool active() const { return m_offscreen; }
    bool offscreen() const { return m_offscreen; }
    bool dirty() const { return m_dirty; }

    // ImGui 后端在 Renderer 里比目标晚建 → 建好后回填
    void setImGui(ui::ImGuiManager* m) { m_imgui = m; }

    // 在 Renderer::m_targets 里的下标（0 = 主视口）。离屏目标用它索引自己的
    // 相机 UBO 槽（见 FrameResources::OffscreenUbo）—— 每台相机一份 UBO，
    // 否则同一命令缓冲里后录制的场景会覆盖先录制的那台的相机矩阵。
    void setSlotIndex(uint32_t i) { m_slotIndex = i; }
    uint32_t slotIndex() const { return m_slotIndex; }

    VkSampleCountFlagBits sampleCount() const { return m_samples; }
    void setSampleCount(VkSampleCountFlagBits s) {
        if (s == m_samples) return;
        m_samples = s;
        m_dirty = true;
    }

    // ---- 资源访问 ----
    bool ready() const { return m_hdr != nullptr && m_post != nullptr; }
    HdrTarget* hdr() { return m_hdr.get(); }
    const HdrTarget* hdr() const { return m_hdr.get(); }
    PostProcess* post() { return m_post.get(); }
    const PostProcess* post() const { return m_post.get(); }

    VkExtent2D extent() const;

    // 应用尺寸：waitIdle → 懒建/重建 HdrTarget + PostProcess → 注册纹理。
    // postInputViews 为空时用 HdrTarget 的 resolve 视图（基线路径）；
    // 主视口接了 FrameGraph 链时由调用方传链输出视图。
    void apply(const std::vector<VkImageView>& swapViews, VkExtent2D swapExtent,
               const std::vector<VkImageView>& postInputViews);

    // 只重建 PostProcess（尺寸不变，但输入视图换了 —— 例如 FrameGraph
    // 链的激活状态翻转）。含 waitIdle + 纹理重注册。
    void recreatePost(const std::vector<VkImageView>& swapViews,
                      VkExtent2D swapExtent,
                      const std::vector<VkImageView>& postInputViews);

    // 重新注册 ImGui 纹理（离屏输出视图可能已换）。
    // 视图未变时内部提前返回，保证 ImTextureID 稳定。
    void refreshTextures();

    // 释放 ImGui 纹理（在 ImGui 后端 shutdown 之前必须调用）。
    // 析构也会调 —— 但 Renderer 需要"先释放纹理、后销毁 ImGui 后端、
    // 最后才销毁目标"的顺序，所以单独暴露出来。
    void releaseImGuiTextures() { destroyTextures(); }

    // ImGui::Image 用的纹理句柄（当前帧将渲染进去的那张）。
    // 0 = 尚未就绪。
    uint64_t textureId(uint32_t frameIndex) const;

    // ---- 多相机：本目标由哪个场景的哪台相机渲染 ----
    void setScene(scene::Scene* s) { m_scene = s; }
    scene::Scene* scene() const { return m_scene; }
    void setCamera(scene::Camera* c) { m_camera = c; }
    scene::Camera* camera() const { return m_camera; }

    const std::vector<VkImageView>& textureViews() const {
        return m_textureViews;
    }

private:
    void destroyTextures();

    rhi::Device& m_device;
    VkFormat m_swapFormat = VK_FORMAT_UNDEFINED;
    uint32_t m_framesInFlight = 2;
    VkSampleCountFlagBits m_samples = VK_SAMPLE_COUNT_1_BIT;
    ui::ImGuiManager* m_imgui = nullptr;

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_slotIndex = 0;
    bool m_offscreen = false;
    bool m_dirty = false;

    std::unique_ptr<HdrTarget> m_hdr;
    std::unique_ptr<PostProcess> m_post;

    std::vector<VkDescriptorSet> m_textureSets;
    std::vector<VkImageView> m_textureViews;

    scene::Scene* m_scene = nullptr;
    scene::Camera* m_camera = nullptr;
};

} // namespace render
