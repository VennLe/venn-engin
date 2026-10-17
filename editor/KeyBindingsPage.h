#pragma once
// ============================================================
// editor/KeyBindingsPage —— Preferences ▸ Bindings 页（G06 的 UI 面）
//
// ------------------------------------------------------------
// 为什么是"偏好里的一页"而不是新面板
// ------------------------------------------------------------
//   · 键位绑定是**工程级数据**，不是"当前选中对象"的属性 —— 不进 Inspector；
//   · 面板表的 9 个快捷键（Alt+1..9）已经用满，第 10 个面板要么没快捷键、
//     要么改快捷键规则（`kDigitKeys[9]` 与视口方向键的排他关系很脆，
//     见 MEMORY 的"快捷键有排他"），代价远大于收益；
//   · Preferences 已经是"全局设置"的家（E17），第 7 页不动布局、不动面板数、
//     不碰任何既有脚本的锚点。
//
// ------------------------------------------------------------
// 为什么捕获读 GLFW 而不是 core::Input
// ------------------------------------------------------------
// `core::Input::keyPressed` 在 ImGui 认为"键盘归 UI"时**一律返回 false**
// （Application 每帧用 io.WantCaptureKeyboard 灌进去）。而打开 Preferences
// 窗口、且 ImGui 开了键盘导航时，WantCaptureKeyboard 恰好是 true ——
// 用 core::Input 做捕获会得到"按什么都没反应"的死面板。
// 捕获是**编辑器自己的模态操作**（按下 Rebind 之后这一下按键只属于它），
// 所以直接 `glfwGetKey` 自己判边沿，不参与 UI 门控。
//
// ------------------------------------------------------------
// 捕获期间全局快捷键必须闭嘴
// ------------------------------------------------------------
//   `EditorApp::handleShortcuts` 第一行就 `if (ctx.inputCaptureActive()) return;`
//   否则玩家把 Jump 绑到 Space 的那一刻，Space 会先触发"运行/暂停"。
// ============================================================

struct GLFWwindow;

namespace editor {

class EditorContext;

// 画 Preferences 的 Bindings 页（调用方保证当前就是这一页）。
// window 用于捕获按键（见上面的说明）。
void drawKeyBindingsPage(EditorContext& ctx, GLFWwindow* window);

} // namespace editor
