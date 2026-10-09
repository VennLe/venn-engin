#pragma once
// ============================================================
// ui/ImGuiManager —— ImGui + GLFW + Vulkan 后端集成
// 每帧调用顺序：
//   beginFrame() → 游戏层构建 UI → drawFrame() 内部自动 RenderDrawData
// ============================================================

#include <vulkan/vulkan.h>

struct GLFWwindow;

namespace rhi {
class Instance;
class Device;
}

namespace ui {

class ImGuiManager {
public:
    ImGuiManager() = default;
    ~ImGuiManager() = default;

    void init(GLFWwindow* window, rhi::Instance& instance, rhi::Device& device,
              VkRenderPass renderPass, uint32_t minImageCount,
              uint32_t imageCount);

    // 新帧（ImGui_ImplVulkan_NewFrame → GLFW → ImGui::NewFrame）
    void beginFrame();

    // 全局字体缩放（1.0 = 默认字号）。运行时即时生效（FontGlobalScale），
    // 无需重建字体图集。编辑器设置面板里调整字体大小用。
    void setFontScale(float scale);

    float fontScale() const { return m_fontScale; }

    // 结束并录制绘制数据到命令缓冲（在 RenderPass 内调用）
    void render(VkCommandBuffer cmd);

    void shutdown();

    // 交换链重建后同步图像数量（图像数变化时调用）
    void setMinImageCount(uint32_t minImageCount);

    bool wantCaptureMouse() const;
    bool wantCaptureKeyboard() const;

private:
    float m_fontScale = 1.0f;   // 全局字体缩放（setFontScale 写入，beginFrame 应用）
};

} // namespace ui
