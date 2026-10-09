#pragma once
// ============================================================
// editor/SplitLayout.h —— 可拖拽分栏布局
//
// 替代原来"每帧算死矩形"的 computeLayout：把窗口主体区域组织成一棵
// **分栏树**（二叉树），内部节点是"水平/垂直切分"，叶子是面板矩形。
//
// 相比固定矩形的优势：
//   · 窗口缩放时，各栏**按比例**自适应（不再是写死的 300/360px）
//   · 分栏交界处有一条可拖动的**分隔条**，拖动即改比例，实时生效
//   · 侧栏可拖到折叠（宽度归零），视口自动吞掉剩余空间 —— 视口优先
//
// 设计约束（与既有代码对齐）：
//   · 不引入 ImGui docking 分支，纯手工算矩形 + 一条 InvisibleButton 拖拽
//   · 产出仍然是 EditorContext::LayoutRects，所有面板代码**零改动**
//   · 比例用 0..1 的 float 存，落在窗口内再用像素换算，天然自适应
// ============================================================

#include <algorithm>

#include "EditorContext.h"  // 复用 Rect / LayoutRects

namespace editor {

// ---------------------------------------------------------------- 分栏树
// 每个节点把一块区域切成 A|B 两半。a/b >= 0 表示子节点下标；
// 否则该侧是叶子，由 panelA/panelB 指明对应哪个面板。
struct SplitNode {
    enum class Dir { Horizontal, Vertical };  // Horizontal = 左右分；Vertical = 上下分

    Dir dir = Dir::Horizontal;
    float ratio = 0.5f;   // A 区占的百分比（0..1）
    float minA = 0.0f;    // A 区最小尺寸（像素，0 = 可压到 0，即可折叠）
    float minB = 0.0f;    // B 区最小尺寸（像素）

    int a = -1;           // 子节点下标（-1 = 叶子，用 panelA）
    int b = -1;
    int panelA = -1;      // 叶子面板（a 为 -1 时有效）
    int panelB = -1;      // 叶子面板（b 为 -1 时有效）

    // 当前是否折叠（双击分隔条切换；折叠时 A 区宽度为 0）
    bool collapsed = false;
    float savedRatio = 0.3f;  // 折叠前的比例，展开时恢复
};

// ---------------------------------------------------------------- 布局器
// 持有分栏树 + 交互状态，每帧由 EditorApp 调用：
//   1. beginFrame(io.DisplaySize) —— 传入当前窗口尺寸
//   2. （内部）拖动检测 —— 处理分隔条拖拽
//   3. output(rects) —— 把叶子矩形写到 LayoutRects
class SplitLayout {
public:
    SplitLayout() { buildDefault(); }

    // 每帧第一件事：记录窗口尺寸；首帧按尺寸把初始像素宽度换算成比例
    void beginFrame(float displayW, float displayH);

    // 计算并写出 LayoutRects。调用前必须已 beginFrame。
    void apply(LayoutRects& out);

    // 处理分隔条拖拽（内部调用；也可由 EditorApp 显式调用）
    void handleSplitters();

    // 工具栏（播放控制条）高度。由 EditorApp 从 EditorContext 同步过来，
    // 设置面板里拖高度滑块时实时生效。
    void setToolbarHeight(float h) { m_toolbarH = h; }

    // 重置为默认分栏（"Reset Layout" 用）
    void reset() { buildDefault(); }

private:
    void buildDefault();

    // 递归：把 node 代表的矩形（区域 [x,y,w,h]）写进对应的面板矩形
    void resolve(int nodeIdx, float x, float y, float w, float h,
                 LayoutRects& out);

    // 把叶子面板矩形写到 LayoutRects 对应字段
    void writePanel(int panel, float x, float y, float w, float h,
                    LayoutRects& out);

    // 递归：为每个内部节点的分栏处画/处理一条可拖分隔条
    void processSplitters(int nodeIdx, float x, float y, float w, float h);

    // 拖动状态
    int m_dragNode = -1;     // 正在拖的分栏节点下标
    float m_dragStart = 0.0f; // 拖动起始的像素位置（该分栏当前分隔线位置）

    // 首帧尚未按窗口尺寸初始化比例
    bool m_initialized = false;
    // 用户手动拖过分隔条 —— 之后不再自动按窗口宽度重算比例
    bool m_userAdjusted = false;
    // 上次算比例时的窗口宽度（用来判断"尺寸变化很大"）
    float m_lastInitW = 0.0f;

    float m_dispW = 0.0f;
    float m_dispH = 0.0f;

    float m_toolbarH = 48.0f;   // 工具栏高度（可被 setToolbarHeight 覆盖）

    std::vector<SplitNode> m_nodes;
    int m_root = -1;
};

} // namespace editor
