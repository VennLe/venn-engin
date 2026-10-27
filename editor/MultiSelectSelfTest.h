#pragma once
// ============================================================
// editor/MultiSelectSelfTest —— 多选（#316）的无窗口自检
//
// `MYVK_MULTISELECT_TEST=1` 时由 editor/main.cpp 在 Vulkan / 窗口之前调用，
// 打印 `MULTI-TEST ...` 行并以 `MULTI-TOTAL pass=N fail=M` 收尾。
//
// 刻意**不测**的两件事（它们只能在真窗口里验，见 tools/verify_multiselect.py）：
//   · Ctrl / Shift 点击、框选矩形、拖 gizmo 的鼠标路径 —— 需要 ImGui 的
//     输入状态与视口矩形；
//   · Inspector 的混合值控件 —— 需要 ImGui 的 draw。
// 这里测的是那三条路径**共同依赖**的东西：选择集的语义、撤销命令、
// 支点计算。它们错了，上面三处再怎么接都不会对。
// ============================================================

namespace editor {

// 返回失败条数（0 = 全过）
int runMultiSelectSelfTest();

} // namespace editor
