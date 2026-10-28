#pragma once
// ============================================================
// editor/LayoutSelfTest —— 分栏树"槽位 → 面板"映射的自检
//                          （`Editor.exe` + `MYVK_LAYOUT_TEST=1`）
//
// 为什么要有它：
//   改造前，"哪个面板在哪个位置"是 `PanelDesc::layoutSlot` 这张**常量表**
//   决定的 —— 错了编译器会替我们发现（少一个 case 就画不出来）。
//   E29-1 之后它变成**运行时可写**的数据，错误形态也跟着变成静默的：
//     · 搬家把面板搬丢（源摘掉了、目标没进去）
//     · 同一面板同时出现在两处（写矩形后者覆盖前者，绘制却画两遍
//       —— 视觉上"面板被劈成两块"）
//     · 搬走后旧槽位留着上一帧矩形（LayoutRects 逐帧复用）
//   这三类都不是"看一眼截图"能发现的（要么根本不显示，要么看起来对），
//   所以用纯数值钉死。
//
// 与 `--selftest theme` 同一套纪律：**不建窗口、不碰 ImGui 上下文**。
// 走 **env**（`MYVK_LAYOUT_TEST=1`）而不是 argv —— `Editor.exe` 丢弃命令行
// （见 editor/main.cpp 文件头的 P1 说明）。
// ============================================================

namespace editor {

// 返回失败条数（0 = 全绿）。日志锚点 `LAYOUT-TEST` / `LAYOUT-TOTAL`。
int runLayoutSelfTest();

} // namespace editor
