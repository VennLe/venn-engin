#pragma once
// ============================================================
// editor/EditorIcons —— 矢量小图标（全部用 ImDrawList 现画）
//
// 为什么不用字体字形：ImGui 自带字体里没有"文件夹 / 文件类型"这些图标，
// 而往项目里塞一个图标字体既重又要管授权。矢量绘制在任何 DPI / 缩放下
// 都清晰，还不需要额外的资源文件。
//
// 约定：图标画在 [p, p + s] 这个正方形里，颜色写死成常量 ——
// 图标是"内容"，不该跟着面板配色漂移。
// ============================================================

#include <imgui.h>

namespace editor {

// 大号文件夹（内容浏览器里 52px 那种：后板 + 标签 + 前盖三层，
// 做出"打开的文件夹"的层次感）
inline void drawFolderIcon(ImDrawList* dl, ImVec2 p, float s) {
    const ImU32 body = IM_COL32(224, 174, 86, 255);
    const ImU32 bodyDark = IM_COL32(198, 148, 64, 255);
    const ImU32 front = IM_COL32(242, 198, 114, 255);
    const ImU32 edge = IM_COL32(122, 90, 32, 110);
    const float r = s * 0.09f;

    // 后板 + 标签页
    dl->AddRectFilled(ImVec2(p.x + s * 0.05f, p.y + s * 0.08f),
                      ImVec2(p.x + s * 0.48f, p.y + s * 0.34f), bodyDark,
                      r * 0.7f);
    dl->AddRectFilled(ImVec2(p.x + s * 0.05f, p.y + s * 0.20f),
                      ImVec2(p.x + s * 0.95f, p.y + s * 0.92f), body, r);
    // 前盖（更亮）
    dl->AddRectFilled(ImVec2(p.x + s * 0.10f, p.y + s * 0.38f),
                      ImVec2(p.x + s * 0.98f, p.y + s * 0.95f), front, r);
    dl->AddRect(ImVec2(p.x + s * 0.05f, p.y + s * 0.20f),
                ImVec2(p.x + s * 0.95f, p.y + s * 0.92f), edge, r, 0, 1.0f);
}

// 小号文件夹（Hierarchy 树里跟名字并排，尺寸跟着字号走）。
// 这个尺寸下三层会糊成一团，所以简化成"标签 + 一块板"，靠明暗对比区分：
// 板身暗金、前盖亮金、外面一圈深色描边。
inline void drawTreeFolderIcon(ImDrawList* dl, ImVec2 p, float s) {
    const ImU32 back = IM_COL32(190, 142, 58, 255);
    const ImU32 front = IM_COL32(238, 196, 108, 255);
    const ImU32 edge = IM_COL32(96, 68, 22, 200);
    const float r = s * 0.14f;

    // 标签页 + 后板
    dl->AddRectFilled(ImVec2(p.x + s * 0.05f, p.y + s * 0.10f),
                      ImVec2(p.x + s * 0.46f, p.y + s * 0.36f), back, r * 0.6f);
    dl->AddRectFilled(ImVec2(p.x + s * 0.05f, p.y + s * 0.22f),
                      ImVec2(p.x + s * 0.95f, p.y + s * 0.90f), back, r);
    // 前盖
    dl->AddRectFilled(ImVec2(p.x + s * 0.05f, p.y + s * 0.34f),
                      ImVec2(p.x + s * 0.95f, p.y + s * 0.90f), front, r);
    // 描边
    dl->AddRect(ImVec2(p.x + s * 0.05f, p.y + s * 0.22f),
                ImVec2(p.x + s * 0.95f, p.y + s * 0.90f), edge, r, 0, 1.0f);
}

} // namespace editor
