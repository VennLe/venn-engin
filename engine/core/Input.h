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
    static double scrollDelta();                   // 本帧滚轮（消费置零）
    static void cursorPos(double* x, double* y);

    // ---- ImGui 占用时暂停游戏输入 ----
    static void setUiCapturing(bool mouse, bool keyboard);
    static bool uiCapturingMouse();
    static bool uiCapturingKeyboard();

    // ---- 按键状态清零（窗口失焦时） ----
    static void clear();

private:
    // GLFW 回调（由 Window 绑定）
    friend class Window;
    static void keyCallback(GLFWwindow* w, int key, int scancode, int action, int mods);
    static void mouseButtonCallback(GLFWwindow* w, int button, int action, int mods);
    static void cursorPosCallback(GLFWwindow* w, double xpos, double ypos);
    static void scrollCallback(GLFWwindow* w, double xoffset, double yoffset);
};

} // namespace core
