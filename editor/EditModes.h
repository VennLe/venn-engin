#pragma once
// ============================================================
// editor/EditModes —— 编辑模式（顶栏那颗下拉，UE5 的 "Selection Mode"）
//
// ------------------------------------------------------------
// 它和 GizmoMode 不是一回事
// ------------------------------------------------------------
// 本工程原来那颗顶栏按钮叫 "Select Mode"，下拉里是 Select / Move / Rotate /
// Scale —— 那是**手柄模式**（鼠标左键在视口里干什么）。UE5 那颗同样是
// 下拉的按钮却叫 Selection Mode，里面列的是**编辑模式**：选择 / 地形 /
// 植被 / 网格体绘制 / 建模 / 破裂 / 笔刷编辑 / 动画 / PCG。选哪个，编辑器
// 就换成哪一套编辑工具。
//
// 2026-10-20 用户给了 UE 的截图，要求照那个做。于是这里把它拆成两个概念：
//   · GizmoMode（Q/W/E/R）→ 留在视口工具条，手柄那个小开关本来就是它的家
//   · EditMode（本文件）  → 顶栏下拉，切换的是"整套编辑工具"
//
// 为什么删掉顶栏的 W/E/R 入口不会丢功能：视口工具条那四个图标与 Q/W/E/R
// 快捷键一个都没动，顶栏原来只是**第三个入口**。
//
// ------------------------------------------------------------
// 哪些模式是"真的"、哪些是占位
// ------------------------------------------------------------
// 有真实编辑器的只有三个：Select（常规）/ Landscape（地形）/ Foliage
// （植被）。其余六个在 UE 里是独立的大系统（网格体绘制要顶点色图层、
// 建模要 half-edge、破裂要断裂解算、PCG 要节点图运行时），本工程还没有。
//
// 它们照样出现在下拉里（用户要的就是那 9 项），但 `editModeImplemented()`
// 为 false 的那几行会被画暗并在右边标 "TODO"。点它们不会假装切过去 ——
// 顶栏显示当前模式、面板跟着开，这是整套 UI 的契约，让一个"切过去了但
// 什么都没有"的模式上屏就是在破坏这个契约。
// ============================================================

#include <imgui.h>

#include <cstdint>

namespace editor {

enum class EditMode : int {
    Select = 0,     // 常规选择 / 变换（原有编辑器）
    Landscape,      // 地形：高度笔刷 + 层权重绘制
    Foliage,        // 植被：撒点 / 擦除
    MeshPaint,      // 网格体绘制
    Modeling,       // 建模
    Fracture,       // 破裂
    BrushEdit,      // 笔刷编辑
    Animation,      // 动画
    PCG,            // 程序化内容生成
};

constexpr int kEditModeCount = 9;

// 英文名（TR() 的键，也是日志里的 mode= 值）。下标 = EditMode 的整数值。
const char* editModeName(EditMode m);
// 稳定 id（落盘 / 断言用，小写下划线）
const char* editModeId(EditMode m);
// 一句话说明（下拉项的悬浮提示 + 面板标题下方的解释）
const char* editModeHint(EditMode m);
// 这个模式在本工程里有没有真实编辑器
bool editModeImplemented(EditMode m);

// 把 EditMode 夹到合法范围（落盘文件 / 环境变量里可能是脏数据）
EditMode clampEditMode(int v);

// 14×14 的矢量字形（与顶栏播放条那套一样：手画，不用字体里的图标）。
// p = 左上角，s = 边长，col = 颜色。
void paintEditModeIcon(ImDrawList* dl, ImVec2 p, float s, EditMode m,
                       ImU32 col);

} // namespace editor
