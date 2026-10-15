#pragma once
// ============================================================
// editor/Workspace —— 六套工作区（E05）
//
// 一个"工作区"= 一整套编辑器外观的快照：
//     面板显隐集 + 分栏比例 + 视口布局 + 渲染视图模式
//
// 为什么需要它：3A 引擎的编辑器里，做关卡的人和调材质的人要的东西几乎
// 不相交 —— 前者要大视口 + 层级 + 内容浏览器，后者要 Engine 面板 + 统计
// + 视图模式下拉。让所有人共用一套布局 = 每个人都要手工拖回来。
//
// 与"用户自己拖"的关系（关键设计）：
//   用户拖过分隔条 / 开关过面板之后，那套改动**属于当前工作区**，
//   被标记为 modified（标题栏加 *）。此时：
//     · 切到别的工作区再切回来 → 回到**该工作区的出厂预设**（不是你的改动）
//     · 想留下改动 → Workspace ▸ Save Workspace（写进 config/editor_layout.json）
//   这样"预设"永远是已知良好的基准，不会因为一天的手工拖动而漂成怪物。
//
// ------------------------------------------------------------
// 为什么预设用"显式开/关清单"而不是 bool 数组
// ------------------------------------------------------------
// bool 数组要按下标对齐 kPanels 的顺序 —— 表面类型安全，实际是隐式约定：
// 哪天在表中间插一个面板，所有预设的下标就整体错位，而且**编译期不会报**。
// 用面板 id 字符串列表（nullptr 结尾）则与表的顺序完全无关，插入/删除面板
// 都不会静默改变某套预设的语义。
// 没列在 open 里也没列在 close 里的面板 → 保持它自己的 `defaultOpen`。
// ⚠ 这条成立的前提是 `applyWorkspace()` **先把面板状态复位到所有 defaultOpen**
//   再套用清单。若只做"增量套用"，那些没点名的面板会保持**当前状态**，
//   于是同一套工作区从不同起点切进来长得不一样 —— 与"预设是已知良好的基准"
//   直接矛盾。
// ============================================================

#include <cstdint>
#include <string>

namespace editor {

// 视口布局（E07 用；定义在这里因为它是工作区的一部分）
enum class ViewportLayout : std::uint8_t {
    Single = 0,       // 单视口
    TwoVertical,      // 左右两分
    TwoHorizontal,    // 上下两分
    Quad,             // 四宫格
};

const char* viewportLayoutName(ViewportLayout l);
int viewportLayoutCount();

struct WorkspacePreset {
    const char* id;
    const char* title;
    const char* hint;              // 菜单里的一行说明
    // 分栏比例：{根(左栏占比), 左栏(上下比), 右区(视口占比)}
    // 0.0 = "按窗口尺寸自适应"（沿用 SplitLayout 的首帧逻辑）
    float ratio[3];
    ViewportLayout viewportLayout;
    int viewMode;                  // Renderer::ViewMode 的整数值
    const char* const* openPanels;  // 显式打开（nullptr 结尾）
    const char* const* closePanels; // 显式关闭（nullptr 结尾）
};

extern const WorkspacePreset kWorkspaces[];
extern const int kWorkspaceCount;

int workspaceIndexOf(const char* id);
const char* workspaceId(int index);
const char* workspaceTitle(int index);

// ---------------------------------------------------------------- 运行态
// 只保存"当前是哪一套"。"有没有被改过"由 EditorApp **逐帧比对**当前实际
// 布局与出厂预设得出（见 EditorApp::workspaceModified）——
// 不做 markModified() 手工上报，因为那种做法漏一处就永远不亮星号，
// 而比对是自洽的、加新维度时也不用回来补上报点。
class WorkspaceManager {
public:
    int current() const { return m_current; }
    const char* currentId() const { return workspaceId(m_current); }
    const char* currentTitle() const { return workspaceTitle(m_current); }

    // 切到第 i 套（只记录，不施加 —— 施加由 EditorApp::applyWorkspace 做，
    // 它才拿得到 SplitLayout / Renderer）
    void setCurrent(int i) {
        if (i >= 0 && i < kWorkspaceCount) m_current = i;
    }

private:
    int m_current = 0;  // 0 = level（默认工作区）
};

} // namespace editor
