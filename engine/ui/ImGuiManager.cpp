#include "ui/ImGuiManager.h"
#include "ui/ImGuiTheme.h"
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
#include <cstdlib>

namespace ui {

namespace {
// 默认字号（加载 TTF 时用的基础像素尺寸）。比 ImGui 内置位图字体(13px)
// 大、且是矢量字体，缩放后更清晰。
constexpr float kBaseFontSize = 18.0f;

} // namespace

void ImGuiManager::init(GLFWwindow* window, rhi::Instance& instance,
                        rhi::Device& device, VkRenderPass renderPass,
                        uint32_t minImageCount, uint32_t imageCount) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // ---- 自动化跑：imgui.ini 一并不读也不写 ----
    // 编辑器把**浮动面板**（角色窗口 / Preferences / Stats…）的 Pos/Size 交给
    // ImGui 自己持久化，落在 cwd 的 imgui.ini 里。这东西在手动会话里是功能
    // （下次打开还在原处），但对脚本是**毒药**：
    //   手动会话常常是全屏（3840x2054），脚本却把窗口缩到 1600x900 ——
    //   存档里那个在 3840 宽度下完全正常的坐标（实测 [character] 存成
    //   Pos=1559,597 Size=1263,794）一读回来，角色窗口就被顶到客户区右/下
    //   之外：ImGui 照画不误、logRect 照打坐标，可脚本按那段矩形去点击全部
    //   落在窗口外 —— 实测症状正是 verify_character_window 的
    //   "拿到动画条目矩形（CHAR-CLIP）"失败（标签根本切不过去），
    //   而且是**持久化**的：一次踩脏，此后每次都挂。
    // 主面板（Hierarchy/Inspector/viewport/Content…）不受影响：它们的
    // Pos/Size 由 computeLayout() 每帧显式设定，本来就不靠 ini。
    // 这样 MYVK_NO_WINDOW_SAVE 才名副其实（它此前只管 editor_window.ini）——
    // InspectorPanel 里"ini 被 MYVK_NO_WINDOW_SAVE 关掉了"那段注释写的
    // 就是这个约定，只是实现一直缺着。
    if (std::getenv("MYVK_NO_WINDOW_SAVE") || std::getenv("MYVK_FRAMES")) {
        io.IniFilename = nullptr;
        VK_LOG_INFO("ImGui settings persistence disabled (automation run)");
    }

    // 主题（含尺寸度量）—— 具体调色板在 ui/ImGuiTheme.cpp，这里只负责
    // 在启动时应用"当前主题"（默认 VennDark = 改动前那套配色）。
    // 运行中换主题走 ui::applyTheme()，见 Edit ▸ Preferences ▸ Interface。
    applyTheme(currentThemeIndex());

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

    // ---- 合并一个中文字体（编辑器要支持中文界面）----
    //
    // 为什么是"合并"而不是"换成中文字体"：
    //   · 英文字形仍由 DroidSans 提供 —— 这保证了**英文界面逐像素不变**
    //     （回归里那批像素断言全部按英文界面标定）。
    //   · 中文字形从系统字体补进来（MergeMode）。ImGui 的字符查找是
    //     "先注册的字体优先"，所以两边各取所长，不会串味。
    //
    // ⚠ 中文字体不随仓库分发（微软雅黑 19MB），改用**系统字体**：Windows
    //   自带，路径固定。找不到就只打一条 WARN —— 中文界面的汉字会显示成
    //   '?'，但程序照常跑，英文界面完全不受影响。
    {
        static const char* kCjkCandidates[] = {
            "C:/Windows/Fonts/msyh.ttc",    // 微软雅黑（Win7+ 标配）
            "C:/Windows/Fonts/msyhl.ttc",   // 微软雅黑 Light
            "C:/Windows/Fonts/Deng.ttf",    // 等线
            "C:/Windows/Fonts/simhei.ttf",  // 黑体
            "C:/Windows/Fonts/simsun.ttc",  // 宋体
            "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
            "/System/Library/Fonts/PingFang.ttc",
        };
        const char* picked = nullptr;
        for (const char* p : kCjkCandidates) {
            if (FILE* f = std::fopen(p, "rb")) {
                std::fclose(f);
                picked = p;
                break;
            }
        }
        if (picked) {
            ImFontConfig cfg;
            cfg.MergeMode = true;
            // 只取常用简体字（约 2500 个）：全 CJK 区有 2 万个码位，图集会
            // 撑到 8192x8192，启动慢且吃显存 —— 编辑器界面用不到那些生僻字。
            cfg.GlyphRanges = io.Fonts->GetGlyphRangesChineseSimplifiedCommon();
            // .ttc 是多字体集合，取第 0 个面（微软雅黑常规体）
            cfg.FontNo = 0;
            ImFont* cjk = io.Fonts->AddFontFromFileTTF(
                picked, kBaseFontSize, &cfg);
            if (cjk)
                VK_LOG_INFO("ImGui CJK font merged: %s (%.0fpx)", picked,
                            kBaseFontSize);
            else
                VK_LOG_WARN("ImGui CJK font merge failed: %s", picked);
        } else {
            VK_LOG_WARN("No CJK font found - Chinese UI text will not render");
        }
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
