#include "ui/ImGuiManager.h"
#include "rhi/Instance.h"
#include "rhi/Device.h"

#include "assets/AssetPath.h"
#include "core/Logger.h"

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <cstdio>

namespace ui {

namespace {
// 默认字号（加载 TTF 时用的基础像素尺寸）。比 ImGui 内置位图字体(13px)
// 大、且是矢量字体，缩放后更清晰。
constexpr float kBaseFontSize = 18.0f;

// ---------------------------------------------------------------- 视觉风格
// 对齐 UE5 编辑器的观感：
//   · 间距比 ImGui 默认舒展一档（默认 FramePadding 是 4,3、ItemSpacing 8,4）
//     —— 这是"面板看起来拥挤"最直接的原因：控件彼此贴太近、文字顶到边框。
//   · 深灰阶 + 单一蓝色强调色；面板用方角（4px 圆角），不用发光/描边。
//   · 主菜单栏与工具栏同色，视觉上连成一条"顶栏"。
void applyVennStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    ImVec4* c = s.Colors;

    // ---- 尺寸 ----
    s.WindowPadding = ImVec2(10.0f, 8.0f);
    s.FramePadding = ImVec2(9.0f, 5.0f);       // 默认 3 → 控件更"厚"，不挤
    s.ItemSpacing = ImVec2(9.0f, 7.0f);        // 默认 8,4 → 行间透气
    s.ItemInnerSpacing = ImVec2(7.0f, 5.0f);
    s.CellPadding = ImVec2(7.0f, 5.0f);
    s.IndentSpacing = 20.0f;
    s.ScrollbarSize = 13.0f;
    s.GrabMinSize = 11.0f;

    s.WindowBorderSize = 1.0f;
    s.ChildBorderSize = 1.0f;
    s.PopupBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f;
    s.TabBorderSize = 0.0f;

    s.WindowRounding = 4.0f;
    s.ChildRounding = 4.0f;
    s.FrameRounding = 4.0f;
    s.PopupRounding = 4.0f;
    s.ScrollbarRounding = 6.0f;
    s.GrabRounding = 4.0f;
    s.TabRounding = 4.0f;

    s.WindowTitleAlign = ImVec2(0.02f, 0.5f);
    s.WindowMenuButtonPosition = ImGuiDir_None;   // 面板标题左边不留折叠三角
    s.SeparatorTextBorderSize = 1.0f;
    s.SeparatorTextPadding = ImVec2(14.0f, 4.0f);

    // ---- 配色（UE5 深色一套的近似值）----
    const ImVec4 accent(0.16f, 0.53f, 0.94f, 1.00f);

    c[ImGuiCol_Text] = ImVec4(0.86f, 0.87f, 0.89f, 1.00f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.48f, 0.50f, 0.53f, 1.00f);

    c[ImGuiCol_WindowBg] = ImVec4(0.106f, 0.110f, 0.118f, 1.00f);
    c[ImGuiCol_ChildBg] = ImVec4(0.129f, 0.133f, 0.141f, 1.00f);
    c[ImGuiCol_PopupBg] = ImVec4(0.118f, 0.122f, 0.129f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0.235f, 0.245f, 0.263f, 0.85f);
    c[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    c[ImGuiCol_FrameBg] = ImVec4(0.176f, 0.184f, 0.196f, 1.00f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.227f, 0.239f, 0.259f, 1.00f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.263f, 0.280f, 0.310f, 1.00f);

    c[ImGuiCol_TitleBg] = ImVec4(0.113f, 0.117f, 0.125f, 1.00f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.149f, 0.157f, 0.169f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.113f, 0.117f, 0.125f, 1.00f);

    c[ImGuiCol_MenuBarBg] = ImVec4(0.113f, 0.117f, 0.125f, 1.00f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.086f, 0.090f, 0.098f, 1.00f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.286f, 0.298f, 0.318f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.353f, 0.365f, 0.388f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.420f, 0.435f, 0.459f, 1.00f);

    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = ImVec4(0.29f, 0.42f, 0.63f, 1.00f);
    c[ImGuiCol_SliderGrabActive] = accent;

    c[ImGuiCol_Button] = ImVec4(0.200f, 0.211f, 0.231f, 1.00f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.247f, 0.310f, 0.427f, 1.00f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.216f, 0.400f, 0.706f, 1.00f);

    c[ImGuiCol_Header] = ImVec4(0.188f, 0.267f, 0.400f, 1.00f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.231f, 0.365f, 0.573f, 1.00f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.243f, 0.451f, 0.780f, 1.00f);

    c[ImGuiCol_Separator] = ImVec4(0.235f, 0.245f, 0.263f, 1.00f);
    c[ImGuiCol_SeparatorHovered] = ImVec4(0.310f, 0.400f, 0.560f, 1.00f);
    c[ImGuiCol_SeparatorActive] = accent;

    c[ImGuiCol_ResizeGrip] = ImVec4(0.235f, 0.245f, 0.263f, 0.60f);
    c[ImGuiCol_ResizeGripHovered] = ImVec4(0.310f, 0.400f, 0.560f, 0.80f);
    c[ImGuiCol_ResizeGripActive] = accent;

    c[ImGuiCol_Tab] = ImVec4(0.145f, 0.153f, 0.165f, 1.00f);
    c[ImGuiCol_TabHovered] = ImVec4(0.231f, 0.365f, 0.573f, 1.00f);
    c[ImGuiCol_TabSelected] = ImVec4(0.188f, 0.267f, 0.400f, 1.00f);

    c[ImGuiCol_PlotLines] = ImVec4(0.60f, 0.66f, 0.74f, 1.00f);
    c[ImGuiCol_PlotHistogram] = accent;

    c[ImGuiCol_TextSelectedBg] = ImVec4(0.16f, 0.53f, 0.94f, 0.35f);
    c[ImGuiCol_NavHighlight] = accent;
    c[ImGuiCol_DragDropTarget] = ImVec4(1.00f, 0.78f, 0.20f, 0.90f);
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.145f, 0.153f, 0.165f, 1.00f);
    c[ImGuiCol_TableBorderStrong] = ImVec4(0.235f, 0.245f, 0.263f, 1.00f);
    c[ImGuiCol_TableBorderLight] = ImVec4(0.180f, 0.188f, 0.200f, 1.00f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1.00f, 1.00f, 1.00f, 0.02f);
}
} // namespace

void ImGuiManager::init(GLFWwindow* window, rhi::Instance& instance,
                        rhi::Device& device, VkRenderPass renderPass,
                        uint32_t minImageCount, uint32_t imageCount) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    applyVennStyle();

    // ---- 加载矢量字体（DroidSans，随 imgui 一起拷贝到 assets/fonts）----
    // 用矢量 TTF 替代内置位图字体：基础字号更大，且运行时 FontGlobalScale
    // 缩放仍保持可读。找不到字体就退回内置字体（不致命）。
    const std::string fontPath =
        assets::resolveAssetPath("fonts/DroidSans.ttf");
    FILE* fp = std::fopen(fontPath.c_str(), "rb");
    if (fp) {
        std::fclose(fp);
        ImFont* font =
            io.Fonts->AddFontFromFileTTF(fontPath.c_str(), kBaseFontSize);
        if (font) {
            io.FontDefault = font;
            VK_LOG_INFO("ImGui font loaded: %s (%.0fpx)",
                        fontPath.c_str(), kBaseFontSize);
        } else {
            VK_LOG_WARN("ImGui font load failed: %s (fallback to builtin)",
                        fontPath.c_str());
        }
    } else {
        VK_LOG_WARN("ImGui font not found: %s (fallback to builtin)",
                    fontPath.c_str());
    }

    ImGui_ImplGlfw_InitForVulkan(window, true);

    ImGui_ImplVulkan_InitInfo info = {};
    info.Instance = instance.get();
    info.PhysicalDevice = device.physical();
    info.Device = device.get();
    info.QueueFamily = device.graphicsFamily();
    info.Queue = device.graphicsQueue();
    info.DescriptorPoolSize = 64;          // 后端自建内部池
    info.MinImageCount = minImageCount;
    info.ImageCount = imageCount;
    info.ApiVersion = VK_API_VERSION_1_0;
    info.MinAllocationSize = 1024 * 1024;
    info.PipelineInfoMain.RenderPass = renderPass;
    info.PipelineInfoMain.Subpass = 0;
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

    if (!ImGui_ImplVulkan_Init(&info)) {
        throw std::runtime_error("ImGui_ImplVulkan_Init failed");
    }
}

void ImGuiManager::beginFrame() {
    // 应用全局字体缩放（运行时即时生效，无需重建字体图集）。
    // 放在 NewFrame 之前，本帧所有 UI 文字都按新缩放渲染。
    ImGui::GetIO().FontGlobalScale = m_fontScale;
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void ImGuiManager::setFontScale(float scale) {
    m_fontScale = scale;
}

void ImGuiManager::render(VkCommandBuffer cmd) {
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
}

void ImGuiManager::shutdown() {
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void ImGuiManager::setMinImageCount(uint32_t minImageCount) {
    ImGui_ImplVulkan_SetMinImageCount(minImageCount);
}

bool ImGuiManager::wantCaptureMouse() const {
    return ImGui::GetCurrentContext() != nullptr &&
           ImGui::GetIO().WantCaptureMouse;
}

bool ImGuiManager::wantCaptureKeyboard() const {
    return ImGui::GetCurrentContext() != nullptr &&
           ImGui::GetIO().WantCaptureKeyboard;
}

} // namespace ui
