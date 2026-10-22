// ============================================================
// ui/KeyCodes.cpp —— 键码换算表（三张面孔，一张表）
// ============================================================

#include "ui/KeyCodes.h"

#include "i18n/I18n.h"

#include <cstdio>

namespace ui_keys {

namespace {

struct Entry {
    int glfw;
    ImGuiKey imgui;
    const char* label;
};

// 只收"给动画 / 玩法绑键"会用到的那一批：字母、数字、F1~F12、小键盘、
// 方向键、几个常用标点。**不收**修饰键（Shift/Ctrl/Alt）与 Esc：它们要么
// 被编辑器快捷键占着，要么是"取消"，绑上去只会造成困惑。
const Entry kTable[] = {
    {65, ImGuiKey_A, "A"}, {66, ImGuiKey_B, "B"}, {67, ImGuiKey_C, "C"},
    {68, ImGuiKey_D, "D"}, {69, ImGuiKey_E, "E"}, {70, ImGuiKey_F, "F"},
    {71, ImGuiKey_G, "G"}, {72, ImGuiKey_H, "H"}, {73, ImGuiKey_I, "I"},
    {74, ImGuiKey_J, "J"}, {75, ImGuiKey_K, "K"}, {76, ImGuiKey_L, "L"},
    {77, ImGuiKey_M, "M"}, {78, ImGuiKey_N, "N"}, {79, ImGuiKey_O, "O"},
    {80, ImGuiKey_P, "P"}, {81, ImGuiKey_Q, "Q"}, {82, ImGuiKey_R, "R"},
    {83, ImGuiKey_S, "S"}, {84, ImGuiKey_T, "T"}, {85, ImGuiKey_U, "U"},
    {86, ImGuiKey_V, "V"}, {87, ImGuiKey_W, "W"}, {88, ImGuiKey_X, "X"},
    {89, ImGuiKey_Y, "Y"}, {90, ImGuiKey_Z, "Z"},
    {48, ImGuiKey_0, "0"}, {49, ImGuiKey_1, "1"}, {50, ImGuiKey_2, "2"},
    {51, ImGuiKey_3, "3"}, {52, ImGuiKey_4, "4"}, {53, ImGuiKey_5, "5"},
    {54, ImGuiKey_6, "6"}, {55, ImGuiKey_7, "7"}, {56, ImGuiKey_8, "8"},
    {57, ImGuiKey_9, "9"},
    {290, ImGuiKey_F1, "F1"},    {291, ImGuiKey_F2, "F2"},
    {292, ImGuiKey_F3, "F3"},    {293, ImGuiKey_F4, "F4"},
    {294, ImGuiKey_F5, "F5"},    {295, ImGuiKey_F6, "F6"},
    {296, ImGuiKey_F7, "F7"},    {297, ImGuiKey_F8, "F8"},
    {298, ImGuiKey_F9, "F9"},    {299, ImGuiKey_F10, "F10"},
    {300, ImGuiKey_F11, "F11"},  {301, ImGuiKey_F12, "F12"},
    {320, ImGuiKey_Keypad0, "Num0"}, {321, ImGuiKey_Keypad1, "Num1"},
    {322, ImGuiKey_Keypad2, "Num2"}, {323, ImGuiKey_Keypad3, "Num3"},
    {324, ImGuiKey_Keypad4, "Num4"}, {325, ImGuiKey_Keypad5, "Num5"},
    {326, ImGuiKey_Keypad6, "Num6"}, {327, ImGuiKey_Keypad7, "Num7"},
    {328, ImGuiKey_Keypad8, "Num8"}, {329, ImGuiKey_Keypad9, "Num9"},
    {32, ImGuiKey_Space, "Space"},   {257, ImGuiKey_Enter, "Enter"},
    {258, ImGuiKey_Tab, "Tab"},      {259, ImGuiKey_Backspace, "Backspace"},
    {261, ImGuiKey_Delete, "Del"},   {262, ImGuiKey_RightArrow, "Right"},
    {263, ImGuiKey_LeftArrow, "Left"}, {264, ImGuiKey_DownArrow, "Down"},
    {265, ImGuiKey_UpArrow, "Up"},   {266, ImGuiKey_PageUp, "PgUp"},
    {267, ImGuiKey_PageDown, "PgDn"}, {268, ImGuiKey_Home, "Home"},
    {269, ImGuiKey_End, "End"},
    {39, ImGuiKey_Apostrophe, "'"},  {44, ImGuiKey_Comma, ","},
    {45, ImGuiKey_Minus, "-"},       {46, ImGuiKey_Period, "."},
    {47, ImGuiKey_Slash, "/"},       {59, ImGuiKey_Semicolon, ";"},
    {61, ImGuiKey_Equal, "="},       {91, ImGuiKey_LeftBracket, "["},
    {92, ImGuiKey_Backslash, "\\"},  {93, ImGuiKey_RightBracket, "]"},
    {96, ImGuiKey_GraveAccent, "`"},
};

constexpr int kCount = static_cast<int>(sizeof(kTable) / sizeof(kTable[0]));

const Entry* findGlfw(int glfwCode) {
    if (glfwCode == 0) return nullptr;
    for (int i = 0; i < kCount; ++i)
        if (kTable[i].glfw == glfwCode) return &kTable[i];
    return nullptr;
}

const Entry* findImGui(int imguiKey) {
    for (int i = 0; i < kCount; ++i)
        if (static_cast<int>(kTable[i].imgui) == imguiKey) return &kTable[i];
    return nullptr;
}

} // namespace

const char* label(int glfwCode) {
    if (glfwCode == 0) return "";
    if (const Entry* e = findGlfw(glfwCode)) return e->label;
    static char buf[16];
    // 兜底标签也要过词典：表里没有的键（将来 GLFW 加了新键码）会显示成
    // "Key%d" —— 中文界面下应该是"按键%d"。
    std::snprintf(buf, sizeof(buf), i18n::tr("Key%d"), glfwCode);
    return buf;
}

int glfwFromImGui(int imguiKey) {
    const Entry* e = findImGui(imguiKey);
    return e ? e->glfw : 0;
}

bool pressed(int glfwCode) {
    const Entry* e = findGlfw(glfwCode);
    if (!e) return false;
    // repeat = false：按住不放只算一次（否则每帧重置播放头，看起来像
    // "卡在第一帧"）。
    return ImGui::IsKeyPressed(e->imgui, false);
}

bool down(int glfwCode) {
    const Entry* e = findGlfw(glfwCode);
    if (!e) return false;
    return ImGui::IsKeyDown(e->imgui);
}

int captureImGuiKey() {
    for (int i = 0; i < kCount; ++i) {
        if (ImGui::IsKeyPressed(kTable[i].imgui, false))
            return static_cast<int>(kTable[i].imgui);
    }
    return static_cast<int>(ImGuiKey_None);
}

} // namespace ui_keys
