#pragma once
// ============================================================
// ui/KeyCodes —— 键盘键码的三种表示与它们之间的换算
//
// 一个"按键"在本仓库里有三张面孔：
//
//   GLFW 键码（int）   —— `core::Input` / `InputMap` / 存盘（场景里的
//                         `animKeys.entries[].key`）用的是它。稳定、跨平台、
//                         不依赖任何 UI 库。
//   ImGuiKey（枚举）   —— ImGui 自己的键枚举，面板上"捕获按键"只能从这里拿
//                         （`ImGui::IsKeyPressed`）。
//   显示名（字符串）   —— 面板上显示 "F" / "Num1" / "Space"。
//
// 为什么要单独一个文件：这三者本来分散在三处 —— 捕获在角色面板、判定在
// 动画状态机、显示在 tooltip。每处各写一份映射，早晚有一处漏掉（"绑了
// 但显示为空"或"面板上叫 F、实际绑的是别的码"）。现在只有这一张表。
//
// ⚠ 认不出来的键**不许猜**：`glfwFromImGui` 返回 0（= 无效），调用方据此
//   忽略这次捕获；`label` 退化成 "Key<码>"。猜一个"差不多"的码会绑到
//   玩家没按的键上，而那种 bug 看起来像"按键没反应"。
//
// ⚠ 为什么编辑器侧判定必须走 ImGui 而不是 `core::Input::keyPressed`：
//   后者被 `g_uiKeyboard` 门控（`setUiCapturing`，编辑器每帧按
//   `io.WantCaptureKeyboard` 设）。Play 的时候只要 ImGui 有窗口拿着键盘
//   焦点，`keyPressed` 就恒为 false —— 实测症状是"按了绑定的键没反应，
//   而日志里连一行都没有"。`ImGui::IsKeyPressed` 是原始键状态，不受这个
//   门控影响。
// ============================================================

#include <imgui.h>

namespace ui_keys {

// GLFW 键码 → 显示名（认不出退化成 "Key<码>"，不会返回空串）。
const char* label(int glfwCode);

// ImGuiKey → GLFW 键码。0 = 这张表不认识这个键（调用方应当忽略）。
int glfwFromImGui(int imguiKey);

// 编辑器侧判定：本帧这个键是否**刚按下**（边沿）。
// 走 ImGui 的原始键状态，不受 `io.WantCaptureKeyboard` 影响。
bool pressed(int glfwCode);

// 编辑器侧判定：这个键是否处于按下状态。
bool down(int glfwCode);

// 捕获：本帧刚按下的第一个"可绑定键"，返回 ImGuiKey（`ImGuiKey_None` = 没有）。
// 给面板上的"按一个键"用 —— 表在 .cpp 里，调用方不需要知道有哪些键。
int captureImGuiKey();

} // namespace ui_keys
