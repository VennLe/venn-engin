#pragma once
// ============================================================
// Input.h —— 全局输入状态（静态类）
// 由 GLFW 窗口回调驱动，Application 每帧末调用 endFrame()
// 推移按键边沿状态；ImGui 捕获输入时可被外部标记跳过
// ============================================================

#include <cstdint>

struct GLFWwindow;

namespace core {

class Input {
public:
    static void init(GLFWwindow* window);

    // 每帧结束调用：把“本帧按下”推移为“上一帧状态”
    static void endFrame();

    // ---- 键盘 ----
    static bool keyDown(int key);      // 持续按下
    static bool keyPressed(int key);   // 本帧刚按下（边沿）
    static bool keyReleased(int key);  // 本帧刚松开（边沿）

    // ---- 鼠标 ----
    static bool mouseDown(int button);
    static bool mousePressed(int button);
    static void mouseDelta(double* dx, double* dy); // 本帧位移（已被消费置零）
    // 同上，但**不受 `setUiCapturing` 门控**。
    //
    // 用途只有一个：`GLFW_CURSOR_DISABLED` 的"操作态"（编辑器 Play / 运行时
    // 点击视口之后）。那时鼠标已经被游戏接管 —— 屏幕上根本没有光标可点，
    // ImGui 却仍然坚持自己"占着鼠标"（鼠标锁在窗口中心，正好压在某个面板
    // 上），于是 `g_uiMouse = true` 把所有游戏输入一起掐掉。此时要看的就是
    // **原始位移**，而不是"UI 允不允许游戏看鼠标"。
    static void mouseDeltaRaw(double* dx, double* dy);
    static double scrollDelta();                   // 本帧滚轮（消费置零）
    static void cursorPos(double* x, double* y);

    // ---- ImGui 占用时暂停游戏输入 ----
    static void setUiCapturing(bool mouse, bool keyboard);
    static bool uiCapturingMouse();
    static bool uiCapturingKeyboard();

    // ---- 按键状态清零（窗口失焦时） ----
    static void clear();

    // 丢弃**下一次**鼠标位移。
    //
    // 场景：切光标模式（NORMAL ⇄ DISABLED）时 GLFW 会把光标 warp 到窗口
    // 中心，那次位移是**系统产生的**，不是玩家挥手。不吞掉的话，玩家按
    // ESC 暂停/恢复的那一帧视角会猛转一下 —— 而且它只在"恰好有过鼠标
    // 位移"时出现，是最难复现的那类。
    static void discardNextMouseDelta();

private:
    // GLFW 回调（由 Window 绑定）
    friend class Window;
    static void keyCallback(GLFWwindow* w, int key, int scancode, int action, int mods);
    static void mouseButtonCallback(GLFWwindow* w, int button, int action, int mods);
    static void cursorPosCallback(GLFWwindow* w, double xpos, double ypos);
    static void scrollCallback(GLFWwindow* w, double xoffset, double yoffset);
};

} // namespace core
