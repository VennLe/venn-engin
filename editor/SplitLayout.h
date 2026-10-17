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
//
// ------------------------------------------------------------
// E06：从"能用"到"好用"的五件事
// ------------------------------------------------------------
//   1. **分隔线可视化** —— 悬停变亮变粗、拖动时跟一条"比例 xx%"的 tooltip
//   2. **双击折叠/展开** —— 双击哪条线就折哪一侧（原来只能折 A 区）
//   3. **面板最大化** —— Shift+Space 让鼠标所在面板临时占满工作区，再按还原
//   4. **比例预设** —— 右键分隔线弹 50/50、70/30、30/70 快选
//   5. **最小尺寸 / 隐藏折叠** —— 每栏 minW 独立设置；面板被关掉时那一侧
//      **自动收成 0**，邻居吃掉空间（关掉 Hierarchy 后 Content 应该长高，
//      而不是留一块空白）
// ============================================================

#include <algorithm>

#include "EditorContext.h"  // 复用 Rect / LayoutRects
#include "PanelRegistry.h"  // LayoutSlot（槽位定义只有一份）

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
    int panelA = -1;      // 叶子面板（a 为 -1 时有效；LayoutSlot 值）
    int panelB = -1;      // 叶子面板（b 为 -1 时有效）

    // 当前是否折叠（双击分隔条切换；折叠时 A 区宽度为 0）
    bool collapsed = false;
    float savedRatio = 0.3f;  // 折叠前的比例，展开时恢复

    // true = 这条比例是"按窗口尺寸自适应"的（首帧 / 窗口大幅缩放时重算）；
    // false = 用户拖过、或工作区明确指定了数字 → 尊重它，不再自动覆盖。
    // （取代了原来一个全局的 m_userAdjusted 标志：那时拖过根节点就会把
    //   右区节点的自适应一起冻住，是错的。）
    bool autoRatio = true;

    // 当前两侧是否"有面板要显示"（面板被关掉时那侧收成 0，见 E06-5）。
    // 由 beginFrame 每帧写入，resolve/processSplitters 读。
    bool liveA = true;
    bool liveB = true;
};

// ---------------------------------------------------------------- 布局器
// 持有分栏树 + 交互状态，每帧由 EditorApp 调用：
//   1. beginFrame(io.DisplaySize) —— 传入当前窗口尺寸
//   2. （内部）拖动检测 —— 处理分隔条拖拽
//   3. output(rects) —— 把叶子矩形写到 LayoutRects
class SplitLayout {
public:
    SplitLayout() { buildDefault(); }

    // 分栏树节点数：0=根（左右）1=左栏（上下）2=右区（视口|检查器）
    static constexpr int kNodeCount = 3;

    // 每帧第一件事：记录窗口尺寸；首帧按尺寸把初始像素宽度换算成比例
    void beginFrame(float displayW, float displayH);

    // 计算并写出 LayoutRects。调用前必须已 beginFrame。
    void apply(LayoutRects& out);

    // 处理分隔条拖拽（内部调用；也可由 EditorApp 显式调用）
    void handleSplitters();

    // 工具栏（播放控制条）高度。由 EditorApp 从 EditorContext 同步过来，
    // 设置面板里拖高度滑块时实时生效。
    void setToolbarHeight(float h) { m_toolbarH = h; }

    // 主体区底部要**额外**留出的高度（状态栏之上）。
    //
    // 2026-10-17：底栏（Content / 输出日志 / CMD 那一行）从这里扣。
    // 之所以做成"inset"而不是在 EditorApp 里事后压缩各面板矩形：
    // 分隔条的命中检测、最大化判定都复用了同一套 bodyH 计算
    // （见 bodyRect / apply / handleSplitters），事后压缩会让
    // "分隔条画在 A 处、命中在 B 处"。扣在源头只有一处。
    void setBottomInset(float h) { m_bottomInset = h > 0.0f ? h : 0.0f; }

    // 重置为默认分栏（"Reset Layout" 用）
    void reset();

    // ------------------------------------------------------------
    // 遮挡矩形（浮层式 Content 抽屉）
    // ------------------------------------------------------------
    // 抽屉是**浮层**（默认不 dock）：它横跨整宽盖在视口下半部，但它不属于
    // 分栏树 —— 分栏树算出来的 separator 仍然穿过它。所以把这个矩形交给
    // 分栏器，让它把分隔条**切成可见的几段**：
    //   · 画只画可见段（原来整条画在 ForegroundDrawList 上，会直接压在
    //     抽屉里，用户看到的是"Hierarchy | Viewport 那条线渗进 Content"）
    //   · 命中判定也只用可见段（否则在抽屉里拖高度会误拖分栏）
    // off = true 时完全忽略（抽屉关着 → 行为与没有这个功能时逐像素一致）。
    void setOccluder(bool on, float x0, float y0, float x1, float y1) {
        m_occlOn = on;
        m_occlX0 = x0;
        m_occlY0 = y0;
        m_occlX1 = x1;
        m_occlY1 = y1;
    }

    // ---- 面板显隐（E06-5）----
    // 被关掉的面板那一侧收成 0 宽/高，邻居吃掉空间。
    // EditorApp 每帧调一次（面板状态是 PanelRegistry 的全局数组）。
    void syncPanelVisibility();

    // 某个槽位当前是否真的占着地方（最大化时只有那一个为 true）
    bool showsInLayout(int slot) const;

    // ---- 节点参数（工作区切换 / 落盘用）----
    float ratio(int node) const;
    void setRatio(int node, float r);
    bool collapsed(int node) const;
    void setCollapsed(int node, bool c);
    float savedRatio(int node) const;
    void setSavedRatio(int node, float r);
    // 把某个节点交回"按窗口自适应"（工作区预设里 ratio 写 0 的那些节点）
    void setAutoRatio(int node, bool autoRatio);
    bool autoRatio(int node) const;
    // 让下一次 beginFrame 重算全部自动节点（切换工作区 / Reset Layout 用）
    void markNeedsAutoRecalc() { m_initialized = false; }

    // ---- 最大化（E06-3）----
    // slot 传 LayoutSlot 值；-1 = 清除。
    void toggleMaximize(int slot);
    void setMaximized(int slot);
    int maximizedSlot() const { return m_maximized; }

    // ---- 比例预设（E06-4）----
    // aFrac = A 区占的比例（0.5 / 0.7 / 0.3）。同时清掉折叠态。
    void applyPreset(int node, float aFrac);

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

    // 主体区域（菜单栏之下、状态栏之上的那块）
    void bodyRect(float displayW, float displayH, float& x, float& y,
                  float& w, float& h) const;

    // 拖动状态
    int m_dragNode = -1;      // 正在拖的分栏节点下标
    float m_dragStart = 0.0f; // 拖动起始的像素位置
    // 双击判定：ImGui 的 IsMouseDoubleClicked 已经处理了间隔，这里只用来
    // 记住"双击时要折哪一侧"（离鼠标更近的那一侧）
    float m_hoverFrac = 0.0f; // 悬停/拖动时的比例，供 tooltip 显示
    // 本次 handleSplitters 里已有节点吃掉了悬停。两条分隔条叠在同一点时
    // （某节点被折到 0/1，位置正好压在父节点的线上），只让先处理的那个
    // 响应 —— 否则一次双击会同时折一条、展一条。
    bool m_splitHoverTaken = false;

    // 首帧尚未按窗口尺寸初始化比例
    bool m_initialized = false;
    // 上次算比例时的窗口宽度（用来判断"尺寸变化很大"）
    float m_lastInitW = 0.0f;

    float m_dispW = 0.0f;
    float m_dispH = 0.0f;

    float m_toolbarH = 48.0f;   // 工具栏高度（可被 setToolbarHeight 覆盖）
    float m_bottomInset = 0.0f; // 底栏（+ 停靠的 Content 抽屉）占掉的高度

    // 遮挡矩形（浮层抽屉）。见 setOccluder。
    // 刻意用 4 个 float 而不是 ImVec2：这个头文件不 include imgui.h
    // （EditorContext 也不 include），为两个点引一整包头文件不划算。
    bool m_occlOn = false;
    float m_occlX0 = 0.0f;
    float m_occlY0 = 0.0f;
    float m_occlX1 = 0.0f;
    float m_occlY1 = 0.0f;

    std::vector<SplitNode> m_nodes;
    int m_root = -1;

    // 被最大化的槽位（kSlotNone = 没有）
    int m_maximized = kSlotNone;
};

} // namespace editor
