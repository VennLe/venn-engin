#pragma once
// ============================================================
// editor/PanelLayout.h —— 参数面板里的"响应式控件网格"
//
// ------------------------------------------------------------
// 为什么需要它（2026-10-28，用户报"Landscape 面板按钮太拥挤"）
// ------------------------------------------------------------
// 左栏参数面板里的按钮过去全是**硬编码像素**：
//
//     ImGui::Button(label, ImVec2(188.0f, 20.0f));   // 2 列 × 4 行，行距 2
//     ImGui::Button(label, ImVec2(96.0f, 26.0f));    // 4 个层，一行排开
//
// 两个后果：
//   1. **挤**。188×20 + 行距 2 是"能塞下就算"的排法，按钮薄得像标签，
//      点起来要靠运气；4 个 96 宽在一行里连间距都只剩 8px。
//   2. **宽度一改就裂**。左栏现在能被拖到 300（见 SplitLayout::setLeftMinWidth），
//      188×2 = 376 + 间距，在 300 宽里右边那个按钮直接被窗口裁掉 ——
//      裁掉的控件"有矩形却点不到"，是最难查的一类坏法。
//
// 所以统一走"按可用宽度算列数"：宽度够就多排一列（少一行、省高度），
// 不够就退成 1~2 列。行高/间距也一起放到调用点用常量写清楚。
//
// ⚠ 网格**只管横向**。竖直方向由调用点 PushStyleVar(ItemSpacing.y) 控制 ——
//   因为同一段 UI 里"工具按钮之间"和"滑条之间"想要的间距不一样。
// ============================================================

#include <algorithm>

#include <imgui.h>

namespace editor {

// 一行能放几列：每列至少 minW，列间 gap。
// 上限 4 列 —— 再宽也不该出现"一排六个小方块"，那是工具栏不是参数面板。
inline int panelGridColumns(float avail, float minW, float gap = 6.0f) {
    if (avail <= 1.0f || minW <= 1.0f) return 1;
    const float step = minW + gap;
    const int n = static_cast<int>((avail + gap) / step);
    return std::clamp(n, 1, 4);
}

// 每列宽度（把剩余像素均分给各列，不留半像素缝）
inline float panelGridWidth(float avail, int cols, float gap = 6.0f) {
    if (cols <= 1) return std::max(avail, 1.0f);
    return (avail - gap * static_cast<float>(cols - 1)) /
           static_cast<float>(cols);
}

// 网格里放一个按钮：idx 不是行首时自动 SameLine（横向间距吃 ItemSpacing.x）。
// 返回 true = 点了。
//
// ⚠ 调用方负责 PushStyleVar(ItemSpacing) —— 见文件头的说明。
inline bool panelGridButton(const char* label, int idx, int cols, float w,
                            float h) {
    if (cols > 1 && (idx % cols) != 0) ImGui::SameLine();
    return ImGui::Button(label, ImVec2(w, h));
}

// 行高 / 间距的**唯一**一份数字。面板之间保持一致，改一处就全变。
constexpr float kBtnRowH = 26.0f;    // 参数面板里的主按钮高度
constexpr float kBtnGap = 6.0f;      // 按钮之间的横纵间距

} // namespace editor
