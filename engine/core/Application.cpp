#include "core/Application.h"
#include "core/CpuProfiler.h"
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
        // CPU 分段计时的帧边界（见 core/CpuProfiler.h）。
        // ⚠ 位置：`shouldClose()` 之后、`pollEvents()` 之前。
        //   放在 pollEvents 之后的话，"等 vsync 醒过来"那段会被记进
        //   `kEvents` —— 而那恰恰是"CPU 空闲"最该被单独看见的一段。
        core::CpuProfiler::get().beginFrame();
        VK_CPU_SCOPE(core::CpuProfiler::kFrame);
        // 诊断（MYVK_CPU_STATS=1）：每 30 帧一行，与 GPU-TIMING / CULL-STATS
        // 同频同格式（key=value），验证脚本直接正则截取。
        // ⚠ 这一行必须在 kFrame 的**作用域之外**打不出去，所以放到帧末
        //   （见循环体最后那段）—— 那里读到的 kFrame 是**上一帧**的值。
        {
            VK_CPU_SCOPE(core::CpuProfiler::kEvents);
            m_window->pollEvents();
            m_time.update();
        }

        // 最小化：等待事件，跳过渲染
        if (m_window->iconified()) {
            m_window->waitEvents();
            continue;
        }

        // ImGui 新帧 + 输入捕获状态同步
        {
            // `beginFrame()` 会构建整棵 UI 树的状态机（含 ini 读取），
            // 与下面 onImGui 的"填内容"同属 UI 一段 —— 两处都打 kUi，
            // CpuProfiler 的累加会合并它们（这也是它按累加而不是按
            // 单次赋值实现的原因）。
            VK_CPU_SCOPE(core::CpuProfiler::kUi);
            m_renderer->ui().beginFrame();
        }
        Input::setUiCapturing(m_renderer->ui().wantCaptureMouse(),
                              m_renderer->ui().wantCaptureKeyboard());

        {
            // 场景逻辑更新（相机 + 对象动画）+ LOD 惰性生成，同属"场景"一段。
            VK_CPU_SCOPE(core::CpuProfiler::kScene);
            activeScene().update(m_time.deltaTime());

            // LOD 层级惰性生成：只给"当前场景里实际用到、且还没建链"的网格
            // 建一次。放在这里而不是网格创建时，是为了不给内容浏览器里预览
            // 一下就丢掉的临时网格白造几份 GPU 缓冲；ensureLod 幂等，反复调
            // 的代价只是一次哈希查找。
            scene::Scene& s = activeScene();
            s.forEachRenderable([this](ecs::Entity, ecs::MeshComponent& mc,
                                       ecs::MaterialComponent&) {
                if (mc.mesh) assets().ensureLod(mc.mesh);
            });
        }

        // 游戏逻辑更新
        //
        // ⚠ 分段打点**故意分两条路**：运行时（ownsFrameRendering()）的
        //   `onUpdate` 是**整个 GameLoop::tick**（固定步 + 变量步 + 渲染），
        //   在外面套一层 kLogic 会把渲染那一段也算进"逻辑"，堆叠条于是
        //   重复计数、两段加起来超过 100%。所以那种情况由 GameLoop 的
        //   回调自己打（见 RuntimeApp::onUpdate 里的两次 VK_CPU_SCOPE）。
        if (!ownsFrameRendering()) {
            VK_CPU_SCOPE(core::CpuProfiler::kLogic);
            onUpdate(m_time.deltaTime());
        } else {
            onUpdate(m_time.deltaTime());
        }

        // 游戏层 ImGui UI
        // MYVK_HIDE_UI=1：完全跳过 UI 绘制 —— 截图对比时任何随帧变化的
        // 文字（FPS 等）都会破坏"逐像素一致"的前提，UI 必须消失。
        if (!hideUi) {
            VK_CPU_SCOPE(core::CpuProfiler::kUi);
            onImGui();
        }

        // 纹理流驱动：流式中的 DDS 每帧补一个 mip 层（无流式纹理时
        // 零开销）。放在渲染提交前 —— 补传的层同帧即可被采样。
        if (m_assets) {
            // ⚠ 单独一段而不是并进 kScene：它做的是"每帧补一个 mip 层"的
            //   资产管理活，与场景遍历无关；并进去之后"载入大贴图时掉帧"
            //   会被读成"场景更新变慢了"。
            VK_CPU_SCOPE(core::CpuProfiler::kAssets);
            m_assets->tickTextureStreaming();
        }

        // 渲染一帧
        // ⚠ 默认由这里画；运行时（ownsFrameRendering()==true）自己在
        //   GameLoop 的 render 相位里画（见 Application.h 的说明）。
        //   两种情况都是**每帧恰好一次**。
        if (!ownsFrameRendering()) {
            VK_CPU_SCOPE(core::CpuProfiler::kRender);
            m_renderer->drawFrame(activeScene());
        }

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

        // ---- CPU 分段诊断行（MYVK_CPU_STATS=1，每 30 帧一条）----
        // 位置：帧末。读的是 CpuProfiler 的**历史**（= 上一帧的完整值），
        // 因为本帧的 kFrame 作用域要到循环体末尾才析构 —— 在这里读 m_cur
        // 会漏掉"帧末到读完"那一小段（也包括这一行本身的格式化时间）。
        // 预热 32 帧再打：前几帧历史环是空的，全 0 的行会把"没接上"与
        // "第一帧"混为一谈。
        {
            static const bool cpuStatsDbg = std::getenv("MYVK_CPU_STATS") != nullptr;
            static uint32_t cpuTick = 0;
            if (cpuStatsDbg && frameCounter >= 32 && (cpuTick++ % 30) == 0) {
                const core::CpuProfiler& p = core::CpuProfiler::get();
                VK_LOG_INFO(
                    "CPU-STATS frame=%.2f events=%.2f scene=%.2f logic=%.2f "
                    "ui=%.2f assets=%.2f render=%.2f wait=%.2f sync=%.2f "
                    "record=%.2f submit=%.2f present=%.2f",
                    static_cast<double>(p.lastMs(core::CpuProfiler::kFrame)),
                    static_cast<double>(p.lastMs(core::CpuProfiler::kEvents)),
                    static_cast<double>(p.lastMs(core::CpuProfiler::kScene)),
                    static_cast<double>(p.lastMs(core::CpuProfiler::kLogic)),
                    static_cast<double>(p.lastMs(core::CpuProfiler::kUi)),
                    static_cast<double>(p.lastMs(core::CpuProfiler::kAssets)),
                    static_cast<double>(p.lastMs(core::CpuProfiler::kRender)),
                    static_cast<double>(
                        p.lastMs(core::CpuProfiler::kRenderWait)),
                    static_cast<double>(p.lastMs(core::CpuProfiler::kSceneSync)),
                    static_cast<double>(p.lastMs(core::CpuProfiler::kRecord)),
                    static_cast<double>(p.lastMs(core::CpuProfiler::kSubmit)),
                    static_cast<double>(p.lastMs(core::CpuProfiler::kPresent)));
            }
        }

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
