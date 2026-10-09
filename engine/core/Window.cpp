#include "core/Window.h"
#include "core/Logger.h"
#include "core/Input.h"

#include <stdexcept>

// stb_image 的实现放在 assets/Texture.cpp（STB_IMAGE_IMPLEMENTATION 只能有
// 一份），这里只拿声明用。
#include <stb_image.h>

namespace core {

Window::Window(const std::string& title, int width, int height, bool resizable)
    : m_title(title), m_width(width), m_height(height) {
    if (glfwInit() != GLFW_TRUE) {
        VK_LOG_ERROR("glfwInit failed!");
        throw std::runtime_error("Failed to init GLFW");
    }
    // 不让 GLFW 自动创建 OpenGL 上下文
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, resizable ? GLFW_TRUE : GLFW_FALSE);

    m_window = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (!m_window) {
        glfwTerminate();
        VK_LOG_ERROR("glfwCreateWindow failed!");
        throw std::runtime_error("Failed to create GLFW window");
    }
    glfwSetWindowUserPointer(m_window, this);

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
}

bool Window::shouldClose() const {
    return m_shouldClose || glfwWindowShouldClose(m_window) == GLFW_TRUE;
}

// ---------------------------------------------------------------- 窗口图标
//
// glfwSetWindowIcon 在 Win32 后端里就是给窗口发 WM_SETICON（ICON_SMALL +
// ICON_BIG）—— 于是标题栏左上角、Alt+Tab、任务栏三处同时生效。
// 单张 512×512 就够：系统会自己降采样出 16/32/48 那几档。
void Window::setIconFromFile(const std::string& pngPath) {
    if (!m_window || pngPath.empty()) return;

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
