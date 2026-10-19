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
    // 改标题栏文字。启动器拉起编辑器时用 `MYVK_EDITOR_PROJECT` 传工程根，
    // 编辑器在 onInit 里读到工程名后调这个把名字挂上去。
    void setTitle(const std::string& title);
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

    // ---------------------------------------------------------- 鼠标光标
    //
    // `visible=false` 走 `GLFW_CURSOR_DISABLED`：隐藏 + **锁在窗口内**并
    // 持续上报相对位移 —— 这正是视角控制要的，也正是路线图里说的
    // "clipCursor"。GLFW 没有单独的 clip 模式，DISABLED 就是它的等价物
    // （且跨平台，不用 Win32 ClipCursor + HWND 那一套）。
    //
    // ⚠ 切换模式时 GLFW 会把光标 warp 到中心 → 下一次鼠标位移是系统产生
    //   的巨值。这里顺带调 `Input::discardNextMouseDelta()` 吞掉它，
    //   否则按 ESC 暂停/恢复的那一帧镜头会猛转。
    void setCursorVisible(bool visible);
    bool cursorVisible() const { return m_cursorVisible; }

    // 标记为需要重建交换链（resize 后由 Renderer 查询并清除）
    bool takeResizeFlag();
    void invalidate() { m_shouldClose = true; }

    GLFWwindow* handle() const { return m_window; }

    // ---------------------------------------------------------- 窗口图标
    //
    // 两条路，**优先走 exe 内嵌资源**：
    //
    //   1. 构造函数里（窗口还没显示之前）就调 applyIconFromResource()：
    //      Windows 从可执行文件自己的 RT_GROUP_ICON #1 里按目标尺寸各取
    //      一张 HICON（SM_CXICON 给 ICON_BIG、SM_CXSMICON 给 ICON_SMALL），
    //      同时写 WM_SETICON 和窗口类图标（GCLP_HICON）。这是**唯一**
    //      能让 16×16 标题栏图标清晰的做法 —— 让 Windows 用它自己的
    //      ICO 多尺寸挑选逻辑，而不是把一张 512×512 硬缩下去。
    //
    //   2. setIconFromFile(png)：兜底。只有内嵌资源不可用时才生效
    //      （比如 windres 没找到、RC 没编进去）。此时退回 GLFW 的
    //      glfwSetWindowIcon，把 PNG 当单尺寸图用。
    //
    // 之所以要"窗口创建时就先设好图标"，见 Window.cpp 里构造函数那段的
    // 注释：任务栏按钮是在窗口第一次显示时建的，那之后再改图标只是"补救"。
    void setIconFromFile(const std::string& pngPath);

    // 图标是否已经由可执行文件的内嵌资源设好了
    bool iconFromResource() const { return m_iconFromResource; }

    // Vulkan 表面（由 Renderer 在初始化时创建/销毁）
    VkSurfaceKHR createSurface(VkInstance instance) const;

    // ---------------------------------------------------------- 显示时机
    //
    // 窗口是**隐藏创建**的（见构造函数注释：一是为了任务栏图标，二是为了
    // 消掉"启动闪白"）。什么时候让它露面由 Application 决定：
    //
    //   show()  = 第一帧已经呈现到交换链上了，现在显示 = 用户看到的第一帧
    //             就是渲染结果，不会先看到一块系统白。
    //
    // 幂等：重复调用无害。自动化（MYVK_FRAMES 等）下 Application 会在首帧
    // 后立刻调它，保证脚本能截到图。
    void show();
    bool isShown() const { return m_shown; }

private:
    static void framebufferResizeCallback(GLFWwindow* w, int width, int height);
    static void windowIconifyCallback(GLFWwindow* w, int iconified);
    static void windowCloseCallback(GLFWwindow* w);

    // 用可执行文件内嵌的 .ico 资源设图标。Windows 专用；其它平台直接
    // 返回 false（交给 setIconFromFile 的 PNG 路径）。
    bool applyIconFromResource();

    // 把窗口类的背景刷换成深灰（消除"显示前的白色客户区"）。Windows 专用。
    bool applyDarkBackgroundBrush();

    // 进程级的任务栏身份（AppUserModelID）。必须在**任何窗口创建之前**
    // 调用；Windows 专用。
    void applyAppUserModelId();

    GLFWwindow* m_window = nullptr;
    std::string m_title;
    int m_width = 0;
    int m_height = 0;
    bool m_resized = false;
    bool m_shouldClose = false;
    bool m_iconFromResource = false;
    bool m_cursorVisible = true;
    bool m_shown = false;

    // 从资源里 LoadImageW 出来的 HICON：不带 LR_SHARED 的话由调用方负责
    // 销毁，而窗口活着的全过程都要用它们，所以留到析构再 DestroyIcon。
    // 声明成 void* 是为了不在这个头里拖进 <windows.h>。
    void* m_hiconBig = nullptr;
    void* m_hiconSmall = nullptr;

    // 全屏状态 + 进入全屏前的窗口几何（退出时恢复）
    bool m_fullscreen = false;
    int m_windowedX = 0;
    int m_windowedY = 0;
    int m_windowedW = 0;
    int m_windowedH = 0;
    bool m_windowedMaximized = false;
};

} // namespace core
