#pragma once
// ============================================================
// editor/PanelRegistry —— 面板表驱动（E04）
//
// 问题：加一个面板原来要改**四处** ——
//   · EditorContext::PanelVisibility 里加一个 bool
//   · Toolbar::drawMenuBar 的 View 菜单里加一行 MenuItem
//   · EditorApp::onImGui 里加一次 if (panels.xxx) drawXxx()
//   · EditorApp::handleShortcuts 里加一个快捷键分支
// 四处漏一处，表现就是"面板存在但点不开"或"能开但关不掉"。
//
// 现在：**只加一行** —— PanelDesc 表里加一条。四路全部从表派生：
//   · View/Window 菜单（按 group 分子菜单）
//   · 快捷键（PanelApp::handleShortcuts 遍历表匹配 chord）
//   · 绘制派发（EditorApp::drawPanels 遍历表调 draw）
//   · 持久化（state 按 id 存取，落盘 config/editor_layout.json）
//
// ------------------------------------------------------------
// 状态放在哪
// ------------------------------------------------------------
// 表是**常量**（const PanelDesc[]），每帧会被读；显隐状态是**运行时可变**的，
// 单独放在一份按表下标索引的持久 bool 数组里（kMaxPanels 上限）。
// 两者分开的理由：表项里有函数指针，不能整体拷贝；而状态需要"地址稳定"，
// 因为 EditorContext::PanelVisibility 持有的是一堆 bool& 引用
// （见下面 panelOpenRef —— 那样老的调用点 `panels().viewport` 一行都不用改）。
// ============================================================

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace editor {

class EditorApp;

// ---------------------------------------------------------------- 布局槽位
// 主体四个面板在 SplitLayout 分栏树里的位置。定义在这里而不是 SplitLayout.h，
// 是为了让"面板 id ↔ 布局槽位"只有一份映射（PanelDesc::layoutSlot）。
enum LayoutSlot : int {
    kSlotNone = -1,
    kSlotHierarchy = 0,
    kSlotContent = 1,
    kSlotViewport = 2,
    kSlotInspector = 3,
};

// 面板分组 —— 决定 View/Window 菜单里的子菜单层级
enum class PanelGroup : std::uint8_t {
    General = 0,   // 常用主面板（Hierarchy / Inspector / Content / Viewport）
    Debug,         // 调试类（Stats / Scripts / Console）
    Authoring,     // 编辑/调参类（Engine / 未来的资产编辑器 / 时间轴）
};

// ---------------------------------------------------------------- 面板表项
struct PanelDesc {
    const char* id;         // 稳定标识 + 持久化键（"hierarchy"）
    const char* title;      // 窗口标题（"Hierarchy"）
    PanelGroup  group;
    const char* shortcut;   // "Ctrl+1"；"" = 无快捷键
    const char* icon;       // 矢量图标名（EditorIcons 里的函数名，仅文档作用）
    bool defaultOpen;       // 首次启动（无落盘文件时）的显隐
    bool floating;          // 浮动窗口：不走分栏矩形，可以随便拖
    int  layoutSlot;        // kSlot*；kSlotNone = 不在分栏树里
    // 绘制入口。nullptr 有两种含义：
    //   · layoutSlot == kSlotViewport → 视口由 ViewportPanel 自己画
    //   · 其余 → 还没实现，EditorApp 会画一块"待建设"的占位
    void (*draw)(EditorApp&);
};

// 上限：状态数组的容量。加面板时同步看一眼这个数（超了会 assert 出来）。
constexpr int kMaxPanels = 24;

// 表本体定义在 EditorApp.cpp（draw 指针要访问 EditorApp 的私有成员，
// 只能在那里定义）。这里只给只读访问。
const PanelDesc* panelTable();
int panelCount();

// ---------------------------------------------------------------- 状态查询
// 按**表下标**取持久 bool 的地址。越界返回 nullptr。
bool* panelOpenSlot(int index);
// 按 id 取。找不到返回 nullptr。
bool* panelOpenSlotById(const char* id);
// 一定返回一个**有效引用**（找不到时返回一个私有哑元）——
// 供 EditorContext::PanelVisibility 的引用成员初始化，那里不能接 nullptr。
bool& panelOpenRef(const char* id);

// "勾着，但窗口被底栏托盘按钮临时收起来了"（按钮仍在，再点一下回来）。
// 和 panelOpen 分开存，是因为它们是两个概念：panelOpen = 这个面板的按钮
// 在不在（落盘），trayHidden = 窗口现在是不是被藏了（会话内）。
// setPanelOpen(i, true) 会自动清掉它 —— 任何"打开"动作都以可见为准。
bool* panelTrayHiddenSlot(int index);
bool& panelTrayHiddenRef(int index);

int  panelIndexOf(const char* id);
bool panelOpen(int index);
void setPanelOpen(int index, bool open);
void togglePanel(int index);
const char* panelId(int index);     // 越界返回 ""
const char* panelTitle(int index);  // 越界返回 ""

// 快捷键 chord（"Ctrl+1" / "Ctrl+Alt+3"）→ 面板下标；没有匹配返回 -1。
// 只做**字符串精确匹配**，不解析键位 —— 真正的按键判定在
// EditorApp::handleShortcuts（那里才有 ImGuiIO）。这条只是"这个 chord
// 是不是属于某个面板"，用于把按键吞掉、不再往下走其它分支。
int panelIndexByShortcut(const char* chord);

// ---------------------------------------------------------------- 持久化
// 显隐状态的整份导出 / 应用（顺序无关，按 id 匹配）。
// applyPanelState 对**未知 id 静默忽略** —— 老配置文件里删掉的面板不该让
// 整个布局加载失败。
void snapshotPanelState(std::vector<std::pair<std::string, bool>>& out);
void applyPanelState(const std::vector<std::pair<std::string, bool>>& in);
// 恢复成表里的 defaultOpen（"Reset Layout" 与"没有配置文件"两条路都走它）
void resetPanelStateToDefaults();

// ---------------------------------------------------------------- 自动化
// MYVK_LOG_RECTS 下把每个面板的显隐打一行（供脚本断言工作区切换生效）。
void logPanelStates();

} // namespace editor
