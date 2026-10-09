#include "core/Input.h"
#include "core/Logger.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <array>

namespace core {

namespace {
constexpr int kMaxKeys = 512;
constexpr int kMaxButtons = 16;

std::array<bool, kMaxKeys>   g_keyDown{};
std::array<bool, kMaxKeys>   g_keyPressedEdge{};
std::array<bool, kMaxKeys>   g_keyReleasedEdge{};
std::array<bool, kMaxButtons> g_mouseDown{};
std::array<bool, kMaxButtons> g_mousePressedEdge{};

double g_cursorX = 0.0, g_cursorY = 0.0;
double g_cursorLastX = 0.0, g_cursorLastY = 0.0;
double g_deltaX = 0.0, g_deltaY = 0.0;
double g_scrollDelta = 0.0;
bool   g_uiMouse = false, g_uiKeyboard = false;
bool   g_firstMouse = true;
} // namespace

void Input::init(GLFWwindow* window) {
    glfwGetCursorPos(window, &g_cursorX, &g_cursorY);
    g_cursorLastX = g_cursorX;
    g_cursorLastY = g_cursorY;
    g_firstMouse = true;
}

void Input::endFrame() {
    g_keyPressedEdge.fill(false);
    g_keyReleasedEdge.fill(false);
    g_mousePressedEdge.fill(false);
    g_deltaX = 0.0;
    g_deltaY = 0.0;
    g_scrollDelta = 0.0;
}

bool Input::keyDown(int key) {
    if (key < 0 || key >= kMaxKeys) return false;
    if (g_uiKeyboard) return false;
    return g_keyDown[key];
}

bool Input::keyPressed(int key) {
    if (key < 0 || key >= kMaxKeys) return false;
    if (g_uiKeyboard) return false;
    return g_keyPressedEdge[key];
}

bool Input::keyReleased(int key) {
    if (key < 0 || key >= kMaxKeys) return false;
    return g_keyReleasedEdge[key];
}

bool Input::mouseDown(int button) {
    if (button < 0 || button >= kMaxButtons) return false;
    if (g_uiMouse) return false;
    return g_mouseDown[button];
}

bool Input::mousePressed(int button) {
    if (button < 0 || button >= kMaxButtons) return false;
    if (g_uiMouse) return false;
    return g_mousePressedEdge[button];
}

void Input::mouseDelta(double* dx, double* dy) {
    if (dx) *dx = g_uiMouse ? 0.0 : g_deltaX;
    if (dy) *dy = g_uiMouse ? 0.0 : g_deltaY;
}

double Input::scrollDelta() {
    return g_uiMouse ? 0.0 : g_scrollDelta;
}

void Input::cursorPos(double* x, double* y) {
    if (x) *x = g_cursorX;
    if (y) *y = g_cursorY;
}

void Input::setUiCapturing(bool mouse, bool keyboard) {
    g_uiMouse = mouse;
    g_uiKeyboard = keyboard;
}

bool Input::uiCapturingMouse() { return g_uiMouse; }
bool Input::uiCapturingKeyboard() { return g_uiKeyboard; }

void Input::clear() {
    g_keyDown.fill(false);
    g_mouseDown.fill(false);
}

// ---------------- GLFW 回调 ----------------

void Input::keyCallback(GLFWwindow*, int key, int, int action, int) {
    if (key < 0 || key >= kMaxKeys) return;
    if (action == GLFW_PRESS || action == GLFW_REPEAT) {
        g_keyDown[key] = true;
        if (action == GLFW_PRESS) g_keyPressedEdge[key] = true;
    } else if (action == GLFW_RELEASE) {
        g_keyDown[key] = false;
        g_keyReleasedEdge[key] = true;
    }
}

void Input::mouseButtonCallback(GLFWwindow*, int button, int action, int) {
    if (button < 0 || button >= kMaxButtons) return;
    if (action == GLFW_PRESS) {
        g_mouseDown[button] = true;
        g_mousePressedEdge[button] = true;
    } else if (action == GLFW_RELEASE) {
        g_mouseDown[button] = false;
    }
}

void Input::cursorPosCallback(GLFWwindow*, double xpos, double ypos) {
    if (g_firstMouse) {
        g_cursorLastX = xpos;
        g_cursorLastY = ypos;
        g_firstMouse = false;
    }
    g_deltaX += xpos - g_cursorLastX;
    g_deltaY += ypos - g_cursorLastY;
    g_cursorLastX = xpos;
    g_cursorLastY = ypos;
    g_cursorX = xpos;
    g_cursorY = ypos;
}

void Input::scrollCallback(GLFWwindow*, double, double yoffset) {
    g_scrollDelta += yoffset;
}

} // namespace core
