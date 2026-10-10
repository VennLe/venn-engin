#include "core/Application.h"
#include "core/Input.h"
#include "core/Logger.h"
#include "assets/AssetManager.h"
#include "assets/AssetPath.h"

#include <cstdlib>

namespace core {

Application::Application(const std::string& title, int width, int height) {
    m_window = std::make_unique<Window>(title, width, height);
    m_renderer = std::make_unique<render::Renderer>(*m_window);
}

Application::~Application() {
    // 逆序释放：资源 → 渲染器 → 窗口
    m_assets.reset();
    m_renderer.reset();
    m_window.reset();
}

assets::AssetManager& Application::assets() {
    if (!m_assets) {
        m_assets = std::make_unique<assets::AssetManager>(
            m_renderer->textureContext());
    }
    return *m_assets;
}

int Application::run(uint64_t maxFrames) {
    VK_LOG_INFO("Application starting...");

    // 窗口图标的**兜底**路径：Window 的构造函数已经试过用可执行文件里
    // 内嵌的多尺寸 .ico 设过图标了（而且是在窗口显示之前，见 Window.cpp）。
    // 只有那一步失败（RC 没编进去）时这里才会真正生效 —— 用单张 PNG 走
    // glfwSetWindowIcon。已经设好的话这个调用会直接跳过并打一行 INFO。
    m_window->setIconFromFile(assets::resolveAssetPath("icons/venn_icon.png"));

    // 引擎初始化（Vulkan 资源）
    m_renderer->init();

    // 游戏初始化（场景搭建等）
    onInit();
    m_initialized = true;

    uint64_t frameCounter = 0;

    // 截图对比用：隐藏全部 UI。任何逐帧变化的东西（FPS 数字）都会让
    // "剔除开 vs 关"的像素对比永远失败，所以必须能把它关掉。
    const bool hideUi = [] {
        const char* s = std::getenv("MYVK_HIDE_UI");
        return s && s[0] == '1';
    }();
    if (hideUi) VK_LOG_INFO("UI hidden (MYVK_HIDE_UI)");

    // ================= 主循环 =================
    while (!m_window->shouldClose()) {
        m_window->pollEvents();
        m_time.update();

        // 最小化：等待事件，跳过渲染
        if (m_window->iconified()) {
            m_window->waitEvents();
            continue;
        }

        // ImGui 新帧 + 输入捕获状态同步
        m_renderer->ui().beginFrame();
        Input::setUiCapturing(m_renderer->ui().wantCaptureMouse(),
                              m_renderer->ui().wantCaptureKeyboard());

        // 场景逻辑更新（相机 + 对象动画）
        activeScene().update(m_time.deltaTime());

        // 游戏逻辑更新
        onUpdate(m_time.deltaTime());

        // 游戏层 ImGui UI
        // MYVK_HIDE_UI=1：完全跳过 UI 绘制 —— 截图对比时任何随帧变化的
        // 文字（FPS 等）都会破坏"逐像素一致"的前提，UI 必须消失。
        if (!hideUi) onImGui();

        // 渲染一帧
        m_renderer->drawFrame(activeScene());

        // 帧末：按键边沿状态推移
        Input::endFrame();

        ++frameCounter;
        if (maxFrames > 0 && frameCounter >= maxFrames) {
            VK_LOG_INFO("Auto-exit after %llu frames (smoke test)",
                        static_cast<unsigned long long>(frameCounter));
            break;
        }
    }

    // 收尾（窗口还活着）：编辑器在这里存窗口几何
    onShutdown();

    // 收尾：等 GPU 空闲再销毁
    m_renderer->device().waitIdle();
    VK_LOG_INFO("Application shutdown. Total frames: %llu",
                static_cast<unsigned long long>(m_time.frameCount()));
    (void)m_initialized;
    return 0;
}

} // namespace core
