#pragma once
// ============================================================
// editor/PanelDragDrop.h —— E29-2：面板标题栏拖拽换位
//
// 目标（用户原话）：「各个功能面板应该是灵活可变的，且支持自由拖拽和合并
// 区域的，就和 blender 中的差不多」。E29-1 已经把「槽位 → 面板」的映射
// 变成运行时可写（`SplitLayout::swapActive / locatePanel`），这一层只负责
// **把鼠标动作翻译成那一个调用**。
//
// ------------------------------------------------------------
// 为什么不用 ImGui 的 BeginDragDropSource / Target
// ------------------------------------------------------------
// ImGui 的拖拽系统是**挂在 item 上**的：`BeginDragDropSource()` 要求
// `g.LastItemData` 是一个刚提交的 item。而本工程的面板是**普通顶层窗口**，
// 标题栏由 ImGui 内部绘制（`ImGuiWindow::TitleBarRect()`），它不是一个
// item —— 想在标题栏上开一个 source，只能:
//   · 要么在窗口内容区塞一个覆盖标题栏的 InvisibleButton（会抢标题栏的
//     双击/折叠/× 的命中，且需要把窗口的 ClipRect 掰开）；
//   · 要么在窗口外再铺一层全屏 overlay（会抢整个编辑器的输入）。
// 两条都是"为了用框架的 API 去改框架的布局"，得不偿失。
//
// 这里改成**只读鼠标状态**：
//   · 不提交任何 item，不碰 ImGui 的输入队列 → 标题栏原有交互（焦点、
//     折叠、×、双击）全部照旧；
//   · 自己维护一个"按在标题栏上 → 超过 6px 才算拖"的状态机；
//   · 树内面板都是 `NoMove`，所以 ImGui 那边**不会**同时拖动窗口。
//
// ------------------------------------------------------------
// 坐标语义：槽位矩形 = 面板矩形（这一点是 E29-1 送的）
// ------------------------------------------------------------
// `LayoutRects` 的字段是**按面板槽位**写的（`SplitLayout::writePanel`），
// 不是按"格子"。所以面板被搬到别的格子后，它的矩形自动跟着走 —— 这里的
// 命中测试直接用 `L.hierarchy / L.inspector / ...`，不需要知道树的结构。
// 树结构只在**落下时**用一次（`locatePanel` 找源与目标在树里的位置）。
//
// ------------------------------------------------------------
// E29-3：一格多面板（标签组）
// ------------------------------------------------------------
// E29-2 的落点规则是"拖到哪一格就整格交换"。E29-3 把它拆成两种手势 ——
// 与 Blender 一致：
//   · 松手点落在目标格的**中部**（四周各留 25% 边）→ **并入**那一格，
//     变成一个标签组（`SplitLayout::movePanel`）；
//   · 落在**边缘** → 仍然整格**交换**（`SplitLayout::swapActive`）。
//
// 为什么不是"中部换、边缘并"：Blender 里把面板拖到别区中部就是加标签页，
// 拖到边缘是切分新区域。我们的边缘动作退一步做成"交换"（切分是 E29-4），
// 但**中部 = 加标签**这条必须保留，否则用户的第一直觉就是错的。
//
// 两条硬约束：
//   · **视口不能当标签成员**（`canBeTabMember`）。非活动标签不绘制，视口一旦
//     被并进去，用户看到的就是"3D 视口消失了"，只会以为坏了。
//   · **模式栏不能当标签成员**：它自己就是"当前模式 + 参数"的整块内容，
//     而且它是合成槽位（8 个模式面板共用）。
//     两者仍然可以被**整格交换**（边缘手势），只是不能被合并。
//
// ------------------------------------------------------------
// 可测性
// ------------------------------------------------------------
// `panelDragStep` 的输入是一个纯数据 `DragInput`（谁在按、按在哪），
// 不读 ImGui。所以 `--selftest` 的等价物（`MYVK_LAYOUT_TEST=1`）能用真实
// 的 `SplitLayout` 完整跑一遍状态机，断言"拖完矩形真的换了"。
// ============================================================

#include <vector>

#include "EditorContext.h"  // LayoutRects / Rect
#include "PanelRegistry.h"  // kSlot*（槽位定义只有一份）
#include "SplitLayout.h"

namespace editor {

// ---------------------------------------------------------------- 输入
// 每帧一份。由 EditorApp 从 ImGui 组装（见 PanelDragDrop.cpp 的
// `panelDragInputFromImGui`），自检直接构造。
struct DragInput {
    bool clicked = false;   // 本帧刚按下左键
    bool down = false;      // 左键当前是按着的
    bool esc = false;       // 本帧按了 Esc（取消拖拽）
    float x = 0.0f;         // 鼠标（屏幕坐标）
    float y = 0.0f;
};

// ---------------------------------------------------------------- 落点动作
// 松手会发生什么。None = 什么都不做（落点为空 / 同一格 / 不允许合并）。
enum class DropAction {
    None = 0,
    Swap,    // 整格交换（落点在边缘）
    Merge,   // 并入目标格，成为标签组的一员（落点在中部）
};

// ---------------------------------------------------------------- 状态
// 每帧只有一个拖拽，所以由调用方持有一份（EditorApp 的成员）。
struct PanelDragState {
    // armed = 按在标题栏上了，但还没挪够距离。
    // ⚠ 和 active 分开是**必须**的：点标题栏是一次合法操作（给窗口焦点、
    //   在模式栏上是切模式），不能一按下就进入拖拽态。
    bool armed = false;
    bool active = false;
    int srcSlot = kSlotNone;      // 拖的是哪个面板（槽位值）
    float startX = 0.0f;          // 按下点（判拖动阈值用）
    float startY = 0.0f;
    int targetSlot = kSlotNone;   // 当前落点（kSlotNone = 没有落点）
    // 落点会做什么。**每帧重算**（不是松手时才判断）：高亮要按它换样子，
    // "会把面板并进去"和"会交换"在画面上必须能分清。
    DropAction targetAction = DropAction::None;

    void reset();
};

// 一次落下造成的结果。from/to 都是槽位值，kSlotNone = 本帧没发生。
struct PanelDragOutcome {
    int from = kSlotNone;
    int to = kSlotNone;
    DropAction action = DropAction::None;
    bool applied() const {
        return action != DropAction::None && from != kSlotNone && to != kSlotNone;
    }
};

// ---------------------------------------------------------------- 角落手势
// E29-5（2026-10-28，用户原话）：「当鼠标放在两个面板的角的交界时…拖拽时
// 显示成一个加号，这个时候往左拖拽意味着左边区域面板被合并到左边，反之
// 亦然。而在除了角落的其他交接处位置，则是正常地调整面板区域的显示面积
// 大小。」
//
// 与"拖标题栏"相比，这里**没有源格的选择权**：用户没有抓某个标题栏，他只
// 抓了一个交点。所以源/目标完全由**方向**决定：
//   · 拖向左 → 目标 = 交点左边的格，源 = 交点右边的格（"右边被并到左边"）
//   · 拖向右 → 反过来
//   · 拖向上/向下 → 同理（上下两个格）
// 主轴取 |dx| / |dy| 大的那个：这一下是"横向合并"还是"纵向合并"由用户
// 的手势自己说清楚，不需要他先选中什么。
//
// 交点处常常有**三格**（T 形：左边一格、右边上下两格）。垂直那条轴上
// 有两个候选时，取"格心离鼠标当前位置更近"的那个 —— 拖的时候手偏上就是
// 想动上面那格，这比按面积/固定顺序猜都更符合直觉。
struct CornerDragState {
    bool armed = false;     // 按在角上了，还没挪够距离
    bool active = false;    // 已经进入拖拽
    int srcSlot = kSlotNone;      // 每帧重算（见上：由方向决定）
    int targetSlot = kSlotNone;
    DropAction action = DropAction::None;
    float startX = 0.0f, startY = 0.0f;   // 按下点（判阈值）
    float cornerX = 0.0f, cornerY = 0.0f; // 按下时的交点（拖拽中布局可能变，
                                          // 不能每帧重读 corners()[i]）
    void reset();
};

// 角落主轴的判定阈值：位移不到这个像素数就不动（避免手抖触发合并）。
constexpr float kCornerMinDrag = 4.0f;

// 超过这个位移才算"真的在拖"。6px 与 ImGui 自己的
// `io.MouseDragThreshold` 默认值一致 —— 手感统一，也避免与"点击"打架。
constexpr float kPanelDragThreshold = 6.0f;

// ---------------------------------------------------------------- 纯查询
// 取某个槽位的矩形。槽位非法 / 不在树里 → 全 0。
void slotRectOf(const LayoutRects& L, int slot, float& x, float& y, float& w,
                float& h);

// 点落在哪个面板矩形里（整个矩形）。返回槽位值，没有 → kSlotNone。
// ⚠ 0 尺寸的矩形**不参与**：被最大化顶掉 / 被摘空的面板不该成为落点。
int slotAtPoint(const LayoutRects& L, float px, float py);

// 点落在哪个面板的**标题栏**上（矩形顶部 titleH 像素）。
// 这是"抓手"的唯一入口。
int titleBarAtPoint(const LayoutRects& L, float px, float py, float titleH);

// 这个槽位能不能被**拖起来**。
//   · hierarchy / inspector / viewport → 可以
//   · modes → **不行**：它是合成槽位（8 个模式参数面板共用一块），
//     "把模式栏拖走"没有语义；而且它顶部是 9 行模式单选，按下去先要保证
//     点击切模式不被拖拽吞掉。它仍然可以**作为落点**（把别的面板换进去）。
bool canDragSlot(int slot);

// 这个槽位能不能**作为标签**和别的面板并到同一格（E29-3）。
//   · hierarchy / inspector → 可以
//   · viewport → **不行**：非活动标签不绘制，视口被并进去 = "3D 视口消失"
//   · modes → **不行**：合成槽位，整块就是"当前模式 + 参数"
// 注意与 `canDragSlot` 的区别：**能拖 ≠ 能并**。视口可以拖（整格交换），
// 但不能并；模式栏两者都不行。
bool canBeTabMember(int slot);

// 面板的"老家"格子（与 `SplitLayout::buildDefault` 的四个叶子一一对应）。
// 用来把被并进标签组的面板送回原位 —— 并入之后源格会被收成 0 宽，鼠标
// **再也点不到它**，没有这条路就等于"并了就出不来"。
bool homeCellOf(int slot, int& node, int& side);

// 点是否落在矩形的**中部**（四周各留 25% 边）。中部 = 并成标签，边缘 = 交换。
// ⚠ 边距有下限（8px）也有上限（40%）：格子很窄时纯按 25% 算，中部会缩成
//   几个像素，用户根本对不准；格子很大时 25% 又太宽，边缘手势反而变难。
bool inCenterZone(float x, float y, float w, float h, float px, float py);

// ---------------------------------------------------------------- 标签组
// 一个装了**两个以上**面板的叶子。只有一个面板的格子不画标签条。
struct TabGroupInfo {
    int node = -1;
    int side = -1;
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;   // 整格矩形（含标题行）
    std::vector<int> panels;                         // 按显示顺序
    int active = 0;
};

// 收集当前所有标签组。矩形取"该格活动面板的矩形" —— 这正是 E29-1 留下的
// 语义（`LayoutRects` 按**面板槽位**写，而不是按格子）：活动面板的矩形就是
// 整格矩形，非活动面板是 0。所以这里不需要另一套"格子几何"。
void collectTabGroups(const SplitLayout& split, const LayoutRects& L,
                      std::vector<TabGroupInfo>& out);

// 一枚标签。x/y/w/h 是**可点区域**（含内边距），不是文字矩形。
struct TabPill {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    int panel = kSlotNone;   // 这枚标签对应的槽位
    int index = -1;          // 组内下标（点它 → activateInGroup）
    bool active = false;
};

// 标签条几何（纯函数，不碰 ImGui）。
//   cell    —— 整格矩形
//   titleH  —— 标题行高（标签条就画在这一行里，**不额外占高度** → 面板矩形
//              一个像素都不动，这是"零像素漂移"的关键）
//   widths  —— 每枚标签希望多宽（调用方用 ImGui 量完文字再加内边距）
// 返回标签条高度（0 = 不画：少于 2 枚标签，或格子太窄放不下一枚）。
// ⚠ 上下各内缩 2px：分隔线画在前景层（±1.5px 粗），不内缩会被横线压过顶边。
float tabStripPills(const Rect& cell, float titleH, const std::vector<int>& panels,
                    int active, const std::vector<float>& widths,
                    std::vector<TabPill>& out);

// 点落在哪枚标签上。没有 → -1。
int tabAtPoint(const std::vector<TabPill>& pills, float px, float py);

// ---------------------------------------------------------------- 状态机
// 每帧调一次。返回本帧发生的交换（没有则两个字段都是 kSlotNone）。
//
// `titleH` = 标题栏高度（调用方给，所以自检不依赖 ImGui 的 style）。
PanelDragOutcome panelDragStep(SplitLayout& split, const LayoutRects& L,
                               PanelDragState& st, const DragInput& in,
                               float titleH);

// ---------------------------------------------------------------- 角落手势
// 每帧调一次（在 panelDragStep 之后）。`hotCorner` = 鼠标当前压在的交点，
// nullptr = 没有（此时只清理状态，不会开始新的拖拽）。
//
// 落点动作与标题栏拖拽**完全同一套**语义：
//   · 双方都能当标签成员 → Merge（并成标签组）
//   · 否则              → Swap（整格交换）
// 没这么做的话，同一个"拖面板"的手势在两种入口下会给出两种结果，用户没
// 法建立预期。
PanelDragOutcome cornerDragStep(SplitLayout& split, const LayoutRects& L,
                                const SplitCorner* hotCorner,
                                CornerDragState& st, const DragInput& in);

} // namespace editor
