#pragma once
// ============================================================
// Window.h —— GLFW 窗口封装
// 负责：窗口创建、事件回调绑定、Vulkan Surface 创建
// ============================================================

// 注意：glfwCreateWindowSurface 的声明受 VK_VERSION_1_0 宏保护，
// 因此必须先包含 vulkan.h，再包含 glfw3.h
#include <vulkan/vulkan.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <string>

namespace core {

class Window {
public:
    Window(const std::string& title, int width, int height, bool resizable = true);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool shouldClose() const;
    void pollEvents() const;
    void waitEvents() const;

    int width() const;
    int height() const;
    // 像素尺寸（framebuffer，可能与窗口尺寸不同——高 DPI 下）
    void framebufferSize(int* w, int* h) const;
    bool iconified() const;

    // ---------------------------------------------------------- 窗口几何
    // 全部是**屏幕坐标**（glfwGetWindowPos / glfwGetWindowSize 的单位）。
    // 注意高 DPI 下它和 framebufferSize 不是一回事：几何用屏幕坐标，
    // Vulkan 用像素尺寸，两者不要混着传。
    void windowPos(int* x, int* y) const;
    void setWindowPos(int x, int y);
    void windowSize(int* w, int* h) const;
    void setWindowSize(int w, int h);
    bool maximized() const;
    void maximize();
    // 主显示器工作区（已排除任务栏）。拿不到显示器时返回 false。
    bool primaryMonitorWorkArea(int* x, int* y, int* w, int* h) const;
    // 供"记住上次关闭时的大小"用：窗口模式下的几何。
    // 全屏时返回的是进入全屏**之前**记住的那份，而不是全屏尺寸。
    void windowedGeometry(int* x, int* y, int* w, int* h, bool* maxed) const;

    // ---------------------------------------------------------- 全屏
    // 真·全屏（glfwSetWindowMonitor 切到主显示器当前视频模式），
    // 不是"把某个面板铺满窗口"那种布局意义上的全屏。
    // 进入时记住窗口模式下的几何（含最大化状态），退出时原样恢复；
    // 期间 framebuffer 变化会走 resize 回调 → Renderer 自动重建交换链。
    void setFullscreen(bool on);
    bool isFullscreen() const { return m_fullscreen; }

    // 标记为需要重建交换链（resize 后由 Renderer 查询并清除）
    bool takeResizeFlag();
    void invalidate() { m_shouldClose = true; }

    GLFWwindow* handle() const { return m_window; }

    // ---------------------------------------------------------- 窗口图标
    // 从 PNG 设置窗口图标（Windows 上同时作用于**标题栏左上角**和
    // **任务栏**按钮）。读不到文件就只打一条 WARN，不影响启动。
    //
    // 注意这不是唯一入口：可执行文件的资源里也内嵌了同一个 .ico
    // （assets/icons/venn.rc），所以在程序还没跑到这里之前、
    // 以及资源管理器里看到的也是正确的图标。
    void setIconFromFile(const std::string& pngPath);

    // Vulkan 表面（由 Renderer 在初始化时创建/销毁）
    VkSurfaceKHR createSurface(VkInstance instance) const;

private:
    static void framebufferResizeCallback(GLFWwindow* w, int width, int height);
    static void windowIconifyCallback(GLFWwindow* w, int iconified);
    static void windowCloseCallback(GLFWwindow* w);

    GLFWwindow* m_window = nullptr;
    std::string m_title;
    int m_width = 0;
    int m_height = 0;
    bool m_resized = false;
    bool m_shouldClose = false;

    // 全屏状态 + 进入全屏前的窗口几何（退出时恢复）
    bool m_fullscreen = false;
    int m_windowedX = 0;
    int m_windowedY = 0;
    int m_windowedW = 0;
    int m_windowedH = 0;
    bool m_windowedMaximized = false;
};

} // namespace core
