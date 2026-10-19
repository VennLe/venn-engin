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

    // ================= 窗口什么时候露面 =================
    //
    // 用户报的现象：编辑器启动时"闪白一下"。根因是窗口一创建就 ShowWindow，
    // 而 Vulkan 的第一帧还没呈现上去 —— 系统拿窗口类的**白刷**把客户区擦了
    // 一遍，于是先看到一块纯白，下一帧才被渲染结果盖掉。
    //
    // 现在窗口是**隐藏创建**的（见 core/Window 构造函数的注释，那里同时还
    // 解决任务栏图标），这里负责在"第一帧已经呈现之后"再显示它。
    //
    // 再叠加 2 秒缓冲（用户明确要求"缓冲两秒再直接打开完整的窗口"）：
    // 这两秒不是干等 —— 主循环照跑，着色器预编译 / 场景构建 / 纹理流式
    // 上传都在这两秒里一帧一帧做完。所以窗口出现时已经是**能用的完整界面**，
    // 而不是边加载边闪的半个界面。
    //
    // ⚠ 自动化跑**不缓冲**：脚本要尽早拿到窗口去送键鼠 / 截图，白等 2 秒
    //   既浪费又可能踩到脚本自己的超时。判定见 automationRun 那一串
    //   （工程里所有 verify 脚本至少会设其中一个）。
    //
    // MYVK_STARTUP_DELAY=1：**强制**保留这两秒，即使上面那串自动化变量已经
    //   设了。为什么需要这个口子：验证"启动不闪白 + 确实是缓冲 2 秒才出现"
    //   必须在**不设** MYVK_NO_WINDOW_SAVE 的情况下跑（那个变量同时关掉了
    //   imgui.ini 存档），可它一设就把这个延迟也跳过了。于是加一个显式开关，
    //   让测试既能拿到"延迟生效"，又能靠 MYVK_NO_WINDOW_SAVE 不写盘。
    const bool startupAutomation =
        std::getenv("MYVK_FRAMES") || std::getenv("MYVK_NO_WINDOW_SAVE") ||
        std::getenv("MYVK_LOG_RECTS") || std::getenv("MYVK_HIDE_UI") ||
        std::getenv("MYVK_EDITOR_NO_HUD");
    const bool forceStartupDelay = [] {
        const char* s = std::getenv("MYVK_STARTUP_DELAY");
        return s && s[0] != '0';
    }();
    // 缓冲时长由子类决定（默认 2 秒；启动器重写成 0，见 Application.h）。
    // ⚠ 局部名不能叫 startupShowDelay —— 那会在初始化式里遮蔽同名成员
    //   函数（C++ 的声明点就在初始化式之前），编译器把 RHS 当成 double。
    const double showDelay = startupShowDelay();
    double elapsedSec = 0.0;
    bool windowShown = false;

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

        // LOD 层级惰性生成：只给"当前场景里实际用到、且还没建链"的网格
        // 建一次。放在这里而不是网格创建时，是为了不给内容浏览器里预览
        // 一下就丢掉的临时网格白造几份 GPU 缓冲；ensureLod 幂等，反复调
        // 的代价只是一次哈希查找。
        {
            scene::Scene& s = activeScene();
            s.forEachRenderable([this](ecs::Entity, ecs::MeshComponent& mc,
                                       ecs::MaterialComponent&) {
                if (mc.mesh) assets().ensureLod(mc.mesh);
            });
        }

        // 游戏逻辑更新
        onUpdate(m_time.deltaTime());

        // 游戏层 ImGui UI
        // MYVK_HIDE_UI=1：完全跳过 UI 绘制 —— 截图对比时任何随帧变化的
        // 文字（FPS 等）都会破坏"逐像素一致"的前提，UI 必须消失。
        if (!hideUi) onImGui();

        // 纹理流驱动：流式中的 DDS 每帧补一个 mip 层（无流式纹理时
        // 零开销）。放在渲染提交前 —— 补传的层同帧即可被采样。
        if (m_assets) m_assets->tickTextureStreaming();

        // 渲染一帧
        // ⚠ 默认由这里画；运行时（ownsFrameRendering()==true）自己在
        //   GameLoop 的 render 相位里画（见 Application.h 的说明）。
        //   两种情况都是**每帧恰好一次**。
        if (!ownsFrameRendering()) m_renderer->drawFrame(activeScene());

        // ---- 首帧已呈现 → 决定要不要让窗口露面（见上面那段注释）----
        // 顺序：要缓冲就**必须**等满 kStartupShowDelay（哪怕在自动化跑里，
        // 只要 MYVK_STARTUP_DELAY 显式要求）；否则自动化跑立刻露面、正常
        // 跑等满两秒。
        if (!windowShown) {
            elapsedSec += m_time.deltaTime();
            const bool buffering = forceStartupDelay || !startupAutomation;
            if (buffering ? (elapsedSec >= showDelay) : true) {
                m_window->show();
                windowShown = true;
            }
        }

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
