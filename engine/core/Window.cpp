#include "core/Window.h"
#include "core/Logger.h"
#include "core/Input.h"

#include <stdexcept>

// stb_image 的实现放在 assets/Texture.cpp（STB_IMAGE_IMPLEMENTATION 只能有
// 一份），这里只拿声明用。
#include <stb_image.h>

#if defined(_WIN32)
// 让 GLFW 暴露 glfwGetWin32Window —— 图标这条路要拿到 HWND 才能直接发
// WM_SETICON / SetClassLongPtr，而 glfwSetWindowIcon 做不到"给 ICON_BIG
// 和 ICON_SMALL 分别指定不同尺寸"。
//
// NOMINMAX / WIN32_LEAN_AND_MEAN 必须放在 windows.h 之前：windows.h 的
// min/max 宏会把后面所有 std::min / std::max 变成语法错误。
// （MinGW 的 libstdc++ 已经预先定义了 NOMINMAX，所以要判一下再加，
//   否则 -Wall 会报 "redefined"。）
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <windows.h>

// shobjidl_core.h 里就有，但那个头会连带拖进一大堆 COM 声明；这里只要
// 一个函数，自己声明更干净（shell32 本来就在 MinGW 的默认链接集里）。
extern "C" __declspec(dllimport) HRESULT __stdcall
SetCurrentProcessExplicitAppUserModelID(PCWSTR appId);
#endif

namespace core {

namespace {

#if defined(_WIN32)
// 任务栏靠 AppUserModelID 给窗口"上户口"。不设的话 Windows 会按 exe 路径
// 猜一个 —— 对**控制台子系统**程序（Editor.exe 就是，见 CMakeLists 里的
// 说明：保留控制台方便看日志）这个猜测会把窗口和控制台主机算成一家，
// 任务栏分组和图标都可能跟着控制台走。显式设一个稳定的 ID 就没这事了。
constexpr wchar_t kAppUserModelId[] = L"venn.engine.editor";

// 可执行文件内嵌图标的资源 ID —— 对应 assets/icons/venn.rc 里的
// "1 ICON venn.ico"。windres 把 7 档尺寸（16/24/32/48/64/128/256）都
// 编进了 RT_GROUP_ICON #1，LoadImageW 会按请求的尺寸自己挑最合适的一档。
constexpr WORD kIconResourceId = 1;
#endif

} // namespace

Window::Window(const std::string& title, int width, int height, bool resizable)
    : m_title(title), m_width(width), m_height(height) {
    applyAppUserModelId();

    if (glfwInit() != GLFW_TRUE) {
        VK_LOG_ERROR("glfwInit failed!");
        throw std::runtime_error("Failed to init GLFW");
    }
    // 不让 GLFW 自动创建 OpenGL 上下文
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, resizable ? GLFW_TRUE : GLFW_FALSE);

    // ------------------------------ 先藏后显，就为了图标 ------------------------------
    // 任务栏按钮是**窗口第一次显示的时候**建的，那一刻的窗口图标会被
    // shell 记下来。原来这里是默认的 GLFW_VISIBLE=true：glfwCreateWindow
    // 内部紧接着就 ShowWindow，此时窗口还没有自己的图标，只有窗口类上的
    // 那个通用图标（GLFW 注册窗口类时挂的 IDI_APPLICATION 兜底）—— 于是
    // 任务栏先按通用图标建了按钮，等 Application::run() 里再调
    // glfwSetWindowIcon 只是"事后补救"，补不补得上看 shell 心情。
    //
    // 改成：隐藏创建 → 设图标 → **等第一帧画完再显示**（见 show() 的注释：
    // 这一步同时解决了"启动闪白一下"）。
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    m_window = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (!m_window) {
        glfwTerminate();
        VK_LOG_ERROR("glfwCreateWindow failed!");
        throw std::runtime_error("Failed to create GLFW window");
    }
    glfwSetWindowUserPointer(m_window, this);

    // 图标必须在 ShowWindow 之前设好（原因见上）
    applyIconFromResource();
    // ⚠ 这里**不再** glfwShowWindow —— 窗口交给 Application::run() 在首帧
    //   呈现之后显示。原因见 Window::show() 的注释（启动闪白）。
    applyDarkBackgroundBrush();

    // 绑定回调
    glfwSetFramebufferSizeCallback(m_window, Window::framebufferResizeCallback);
    glfwSetWindowIconifyCallback(m_window, Window::windowIconifyCallback);
    glfwSetWindowCloseCallback(m_window, Window::windowCloseCallback);
    glfwSetKeyCallback(m_window, Input::keyCallback);
    glfwSetMouseButtonCallback(m_window, Input::mouseButtonCallback);
    glfwSetCursorPosCallback(m_window, Input::cursorPosCallback);
    glfwSetScrollCallback(m_window, Input::scrollCallback);

    Input::init(m_window);
    VK_LOG_INFO("Window created: %s (%d x %d)", title.c_str(), width, height);
}

Window::~Window() {
    if (m_window) {
        glfwDestroyWindow(m_window);
        m_window = nullptr;
    }
    glfwTerminate();

#if defined(_WIN32)
    // 提示：glfwTerminate() 之后窗口已经没了，这里再销毁 HICON 是安全的。
    if (m_hiconSmall) {
        DestroyIcon(static_cast<HICON>(m_hiconSmall));
        m_hiconSmall = nullptr;
    }
    if (m_hiconBig) {
        DestroyIcon(static_cast<HICON>(m_hiconBig));
        m_hiconBig = nullptr;
    }
#endif
}

bool Window::shouldClose() const {
    return m_shouldClose || glfwWindowShouldClose(m_window) == GLFW_TRUE;
}

// ---------------------------------------------------------------- 窗口图标
//
// 这里有两个坑，都是"图标看着不对"的常见来源：
//
// [1] 尺寸。glfwSetWindowIcon 的 Win32 后端会从你给的数组里挑**两张**
//     图（win32_window.c 的 chooseImage），一张按 SM_CXICON、一张按
//     SM_CXSMICON 去匹配。只给一张 512×512 的话两张都选它 —— 于是
//     16×16 的标题栏图标是把 512 硬缩下来的，糊。
//     → 正解是走 exe 内嵌的多尺寸 .ico，按目标尺寸各取一张（下面
//       applyIconFromResource）。
//
// [2] 时机。任务栏按钮在窗口**第一次显示**时创建，那一刻的图标会被 shell
//     记下来。窗口在构造函数里隐藏创建、设好图标再 ShowWindow，就没有
//     "先通用图标、后补救"这段窗口期（见构造函数里的注释）。
//
// 非 Windows 平台（或 RC 没编进去）退回 GLFW 的 PNG 路径，能显示，只是
// 小尺寸不如原生 ICO 清晰。

void Window::applyAppUserModelId() {
#if defined(_WIN32)
    const HRESULT hr =
        SetCurrentProcessExplicitAppUserModelID(kAppUserModelId);
    if (FAILED(hr)) {
        VK_LOG_WARN("SetCurrentProcessExplicitAppUserModelID failed: 0x%08lx",
                    static_cast<unsigned long>(hr));
    } else {
        VK_LOG_INFO("AppUserModelID set (taskbar identity)");
    }
#endif
}

bool Window::applyIconFromResource() {
#if defined(_WIN32)
    if (!m_window) return false;

    HWND hwnd = glfwGetWin32Window(m_window);
    if (!hwnd) {
        VK_LOG_WARN("Window icon: glfwGetWin32Window returned null");
        return false;
    }

    HMODULE mod = GetModuleHandleW(nullptr);
    const int bigW = GetSystemMetrics(SM_CXICON);
    const int bigH = GetSystemMetrics(SM_CYICON);
    const int smW = GetSystemMetrics(SM_CXSMICON);
    const int smH = GetSystemMetrics(SM_CYSMICON);

    // 让 Windows 自己按请求尺寸从 7 档里挑 —— 这就是"标题栏图标清晰"的关键
    HICON big = static_cast<HICON>(LoadImageW(
        mod, MAKEINTRESOURCEW(kIconResourceId), IMAGE_ICON, bigW, bigH,
        LR_DEFAULTCOLOR));
    HICON small = static_cast<HICON>(LoadImageW(
        mod, MAKEINTRESOURCEW(kIconResourceId), IMAGE_ICON, smW, smH,
        LR_DEFAULTCOLOR));

    if (!big && !small) {
        VK_LOG_WARN("Window icon: no RT_GROUP_ICON #%u in the executable "
                    "(windres/RC missing?) - falling back to the PNG path",
                    static_cast<unsigned>(kIconResourceId));
        return false;
    }

    if (big) SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
    if (small) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small));
    // 注意：**不要**再给 ICON_SMALL2(=2) 发 WM_SETICON。MSDN 只定义了
    // WM_SETICON 的 wParam 取 ICON_SMALL/ICON_BIG；传 2 是未定义行为——
    // 在 Win11 上实测它会把 small 句柄写进 ICON_BIG 的 slot，把上面刚设
    // 好的 48 档大图标覆盖成 24 档（Alt+Tab 图标因此变小变糊，2026-10-11
    // 用外部探针定位：重设 ICON_BIG 后 6 秒稳定，证明覆盖来自进程自身）。
    // ICON_SMALL2 只在 **WM_GETICON** 查询时有意义（shell 取 Alt+Tab 图标），
    // 系统会自己从 BIG/SMALL 组合，不需要应用去设置。

    // WM_SETICON 只作用于窗口；Alt+Tab 和一部分 shell 细节走的是**窗口类**
    // 图标（GCLP_HICON/HICONSM）。两处都设上才算真的处处都对。原来 GLFW
    // 只设了窗口图标，类图标一直停在它注册窗口类时的通用兜底图上。
    if (big) SetClassLongPtrW(hwnd, GCLP_HICON, reinterpret_cast<LONG_PTR>(big));
    if (small) SetClassLongPtrW(hwnd, GCLP_HICONSM, reinterpret_cast<LONG_PTR>(small));

    m_hiconBig = big;
    m_hiconSmall = small;
    m_iconFromResource = true;

    VK_LOG_INFO("Window icon from exe resource #%u: big %dx%d, small %dx%d",
                static_cast<unsigned>(kIconResourceId), bigW, bigH, smW, smH);
    return true;
#else
    return false;
#endif
}

bool Window::applyDarkBackgroundBrush() {
#if defined(_WIN32)
    if (!m_window) return false;
    HWND hwnd = glfwGetWin32Window(m_window);
    if (!hwnd) return false;

    // 窗口类默认的背景刷是 COLOR_WINDOW = **纯白**。GLFW 注册窗口类时就是
    // 这么干的。后果：窗口第一次 ShowWindow 时，客户区会先被系统用白刷
    // 擦一遍 —— 在 Vulkan 把第一帧呈现上去之前，屏幕上是"一块白的"。
    // 这就是用户看到的"启动闪白一下"。
    //
    // 两条路一起用（这是第二条）：
    //   1) 主路：窗口根本不显示，等第一帧呈现完再 show（见 Window::show）。
    //   2) 兜底：把窗口类背景刷换成和编辑器底色一致的深灰。这样即使因为
    //      任何原因窗口提前可见（切全屏、DPI 变化触发的重新布局…），看到的
    //      也是一块"编辑器底色"而不是刺眼的白。
    //
    // ⚠ 画刷由**进程**持有并负责释放：类一旦引用它，就不能在窗口还活着时
    //   删掉（删了下次擦背景会用野指针）。这里故意不释放 —— 一个 8 字节的
    //   画刷，进程级别的一次性开销，换取"绝不悬垂"。
    static HBRUSH s_darkBrush =
        CreateSolidBrush(RGB(27, 28, 30));   // ≈ ImGui WindowBg 的 sRGB
    SetClassLongPtrW(hwnd, GCLP_HBRBACKGROUND,
                     reinterpret_cast<LONG_PTR>(s_darkBrush));
    return true;
#else
    return false;
#endif
}

void Window::show() {
    if (!m_window || m_shown) return;
    m_shown = true;
    glfwShowWindow(m_window);
    // 显示之后请求一次窗口拿到焦点：隐藏创建时 GLFW 不会去激活窗口，
    // 不显示出来就直接用会"窗口在但键盘没反应"。
    glfwFocusWindow(m_window);
    VK_LOG_INFO("Window shown (first frame is on screen)");
}

void Window::setIconFromFile(const std::string& pngPath) {
    if (!m_window || pngPath.empty()) return;

    // 内嵌资源已经设过了就别覆盖 —— 那套是按尺寸从多档 ICO 里挑的，
    // 比"一张 512 让 GLFW 缩"好，覆盖回去反而会把手感做坏。
    if (m_iconFromResource) {
        VK_LOG_INFO("Window icon: keeping the exe-embedded multi-size icon "
                    "(png '%s' not needed)", pngPath.c_str());
        return;
    }

    int w = 0, h = 0, channels = 0;
    // 强制 4 通道：GLFW 的 GLFWimage 要求 RGBA
    unsigned char* pixels = stbi_load(pngPath.c_str(), &w, &h, &channels, 4);
    if (!pixels) {
        VK_LOG_WARN("Window icon not loaded: %s (%s)", pngPath.c_str(),
                    stbi_failure_reason());
        return;
    }

    GLFWimage img;
    img.width = w;
    img.height = h;
    img.pixels = pixels;
    glfwSetWindowIcon(m_window, 1, &img);
    stbi_image_free(pixels);

    VK_LOG_INFO("Window icon set from %s (%dx%d)", pngPath.c_str(), w, h);
}

void Window::pollEvents() const { glfwPollEvents(); }
void Window::waitEvents() const { glfwWaitEvents(); }

int Window::width() const {
    int w, h;
    glfwGetWindowSize(m_window, &w, &h);
    return w;
}

int Window::height() const {
    int w, h;
    glfwGetWindowSize(m_window, &w, &h);
    return h;
}

void Window::framebufferSize(int* w, int* h) const {
    glfwGetFramebufferSize(m_window, w, h);
}

bool Window::iconified() const {
    return glfwGetWindowAttrib(m_window, GLFW_ICONIFIED) == GLFW_TRUE;
}

// ---------------------------------------------------------------- 窗口几何

void Window::windowPos(int* x, int* y) const {
    glfwGetWindowPos(m_window, x, y);
}

void Window::setWindowPos(int x, int y) {
    glfwSetWindowPos(m_window, x, y);
}

void Window::windowSize(int* w, int* h) const {
    glfwGetWindowSize(m_window, w, h);
}

void Window::setWindowSize(int w, int h) {
    glfwSetWindowSize(m_window, w, h);
}

bool Window::maximized() const {
    return glfwGetWindowAttrib(m_window, GLFW_MAXIMIZED) == GLFW_TRUE;
}

void Window::maximize() { glfwMaximizeWindow(m_window); }

bool Window::primaryMonitorWorkArea(int* x, int* y, int* w, int* h) const {
    GLFWmonitor* mon = glfwGetPrimaryMonitor();
    if (!mon) return false;
    glfwGetMonitorWorkarea(mon, x, y, w, h);
    return *w > 0 && *h > 0;
}

void Window::windowedGeometry(int* x, int* y, int* w, int* h,
                              bool* maxed) const {
    if (m_fullscreen) {
        // 全屏尺寸不该被当成"下次启动的窗口尺寸"，所以回放进入全屏前记的
        if (x) *x = m_windowedX;
        if (y) *y = m_windowedY;
        if (w) *w = m_windowedW;
        if (h) *h = m_windowedH;
        if (maxed) *maxed = m_windowedMaximized;
        return;
    }
    int cx = 0, cy = 0, cw = 0, ch = 0;
    glfwGetWindowPos(m_window, &cx, &cy);
    glfwGetWindowSize(m_window, &cw, &ch);
    if (x) *x = cx;
    if (y) *y = cy;
    if (w) *w = cw;
    if (h) *h = ch;
    if (maxed) *maxed = maximized();
}

void Window::setFullscreen(bool on) {
    if (on == m_fullscreen) return;

    if (on) {
        GLFWmonitor* mon = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = mon ? glfwGetVideoMode(mon) : nullptr;
        if (!mon || !mode) {
            VK_LOG_WARN("Window: no primary monitor - fullscreen ignored");
            return;
        }
        // 先记住窗口模式下的几何（含最大化），退出全屏时原样恢复
        glfwGetWindowPos(m_window, &m_windowedX, &m_windowedY);
        glfwGetWindowSize(m_window, &m_windowedW, &m_windowedH);
        m_windowedMaximized = maximized();

        // 分辨率和刷新率都跟主显示器当前视频模式走 —— 这样才是"真全屏"，
        // 而不是一张被拉伸的大窗口。
        glfwSetWindowMonitor(m_window, mon, 0, 0, mode->width, mode->height,
                             mode->refreshRate);
        VK_LOG_INFO("Window: FULLSCREEN %d x %d @ %d Hz", mode->width,
                    mode->height, mode->refreshRate);
    } else {
        const int w = m_windowedW > 0 ? m_windowedW : 1280;
        const int h = m_windowedH > 0 ? m_windowedH : 720;
        glfwSetWindowMonitor(m_window, nullptr, m_windowedX, m_windowedY, w, h,
                             0);
        // 进全屏前是最大化的话，退出来也回到最大化
        if (m_windowedMaximized) glfwMaximizeWindow(m_window);
        VK_LOG_INFO("Window: windowed %d x %d at (%d, %d)", w, h, m_windowedX,
                    m_windowedY);
    }

    m_fullscreen = on;
    // framebuffer 回调一般已经置位了；这里再置一次兜底（重建两次无害，
    // 但漏掉的话会拿着旧尺寸的交换链渲染一整帧，画面被拉伸）。
    m_resized = true;
}

bool Window::takeResizeFlag() {
    bool r = m_resized;
    m_resized = false;
    return r;
}

VkSurfaceKHR Window::createSurface(VkInstance instance) const {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkResult err = glfwCreateWindowSurface(instance, m_window, nullptr, &surface);
    if (err != VK_SUCCESS) {
        VK_LOG_ERROR("glfwCreateWindowSurface failed: %d", static_cast<int>(err));
        throw std::runtime_error("Failed to create window surface");
    }
    return surface;
}

void Window::framebufferResizeCallback(GLFWwindow* w, int width, int height) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(w));
    if (self) {
        self->m_width = width;
        self->m_height = height;
        self->m_resized = true;
        VK_LOG_INFO("Framebuffer resized: %d x %d", width, height);
    }
}

void Window::windowIconifyCallback(GLFWwindow* w, int iconified) {
    (void)w;
    (void)iconified;
    // 最小化时主循环自动切换到 glfwWaitEvents 省资源
}

void Window::windowCloseCallback(GLFWwindow* w) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(w));
    if (self) self->m_shouldClose = true;
}

} // namespace core
