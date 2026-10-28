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
#include <vector>

#include "EditorContext.h"  // 复用 Rect / LayoutRects
#include "PanelRegistry.h"  // LayoutSlot（槽位定义只有一份）

namespace editor {

// ---------------------------------------------------------------- 分栏树
// 每个节点把一块区域切成 A|B 两半。a/b >= 0 表示子节点下标；
// 否则该侧是叶子，由 groupA/groupB（面板组）指明装了哪些面板。
struct SplitNode {
    enum class Dir { Horizontal, Vertical };  // Horizontal = 左右分；Vertical = 上下分

    Dir dir = Dir::Horizontal;
    float ratio = 0.5f;   // A 区占的百分比（0..1）
    float minA = 0.0f;    // A 区最小尺寸（像素，0 = 可压到 0，即可折叠）
    float minB = 0.0f;    // B 区最小尺寸（像素）

    int a = -1;           // 子节点下标（-1 = 叶子，用 groupA）
    int b = -1;
    // 叶子面板组（E29-1）。组里存的是**槽位值**（kSlot*），与 writePanel 一致。
    // 现在每个组只有 1 个成员，行为与改造前逐像素一致；但"一个槽位能装多个
    // 面板"是 E29-3（合并成标签组）的前提 —— 先把数据结构摆正，后面加标签条
    // 就只是画的问题，不再动语义。
    std::vector<int> groupA;
    std::vector<int> groupB;
    int activeA = 0;      // 组内当前显示的下标（E29-3：标签页选中项）
    int activeB = 0;

    // 该侧的活动面板（空组 → kSlotNone）。resolve / syncPanelVisibility 都走它。
    // ⚠ 用函数而不是字段："组 + 活动下标"是组合状态，就地派生才不会出现
    //   "组换了、缓存没跟上"这类不同步。
    int panelA() const {
        return (activeA >= 0 && activeA < static_cast<int>(groupA.size()))
                   ? groupA[static_cast<size_t>(activeA)] : kSlotNone;
    }
    int panelB() const {
        return (activeB >= 0 && activeB < static_cast<int>(groupB.size()))
                   ? groupB[static_cast<size_t>(activeB)] : kSlotNone;
    }

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

// ---------------------------------------------------------------- 角落抓手
// 两条分隔条的**交点**。2026-10-28 用户提的规则（原话）：
//   「当鼠标放在两个面板的角的交界时…拖拽时显示成一个加号，这个时候往左
//     拖拽意味着左边区域面板被合并到左边，反之亦然。而在除了角落的其他
//     交接处位置，则是正常地调整面板区域的显示面积大小。」
//
// 也就是一条线被切成**两种语义完全不同的手势**，落在同一批像素上会互相
// 打架，所以必须先切清楚：
//   · 距交点 `kCornerHit` 以内 → 角落（光标变"加号"，拖 = 合并/换位）
//   · 分隔条剩下的线段        → 调整大小（双箭头光标，拖 = 改比例）
//
// 命中判定排在 processSplitters **之前**：命中角落就把 `m_splitHoverTaken`
// 置上，于是所有节点的 `hovered` 都是 false —— 一条线都不会被角落抢走，
// 也不会出现"按下去一会儿调大小、一会儿合并"的状态竞争。
struct SplitCorner {
    float x = 0.0f, y = 0.0f;   // 交点（屏幕坐标）
    int hNode = -1;             // 横线所属节点（Dir::Vertical）
    int vNode = -1;             // 竖线所属节点（Dir::Horizontal）
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

    // ------------------------------------------------------------
    // 角落（E29-5）
    // ------------------------------------------------------------
    // 本帧的交点列表（纯几何；被浮层盖住的已经剔除）。
    // 每帧由 handleSplitters 重算 —— 布局一变（拖比例 / 合并面板 / 关面板）
    // 交点就跟着走，调用方不该缓存上一帧的下标。
    const std::vector<SplitCorner>& corners() const { return m_corners; }
    // 鼠标当前压在哪个交点上（-1 = 没有）。非 -1 时**分隔条一律不响应悬停**。
    int cornerHot() const { return m_cornerHot; }

    // 工具栏（播放控制条）高度。由 EditorApp 从 EditorContext 同步过来，
    // 设置面板里拖高度滑块时实时生效。
    void setToolbarHeight(float h) { m_toolbarH = h; }

    // ------------------------------------------------------------
    // 左栏（模式栏 + 当前模式的参数面板）的**拖动下限**
    // ------------------------------------------------------------
    // 2026-10-28（用户报的 bug）：这个下限原来是写死的 460，而那正好又是
    // 首帧算出来的宽度（`bodyW * 0.175` 撞上 460 的下限）。于是"往下限方向
    // 拖"一格都拖不动 —— 用户看到的就是"往左拖视口扩不开，左边的 Modes
    // 一动不动"。而视口本来还有 ~300px 的余量可以吃。
    //
    // 修法是把"下限"变成**当前模式声明的东西**：
    //   · Landscape / Foliage / Select 的面板是响应式的（按钮按可用宽度
    //     换列）→ 300 就够；
    //   · Modeling / Fracture 这些是一排硬排的 SameLine 按钮，让到 300 会
    //     被裁掉 → 由它们自己报 460。
    // 上限抬高时（切模式）会把当前宽度**顶上去**，否则切到 Modeling 会
    // 得到一条被裁掉一半的左栏。
    void setLeftMinWidth(float px);
    float leftMinWidth() const;

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
    // 遮挡矩形（浮层 / 弹层 / 浮动面板）
    // ------------------------------------------------------------
    // 分隔条画在 ForegroundDrawList 上 —— 那是**所有窗口之上**的最后一层，
    // 所以任何"浮在分栏之上的东西"（Content 抽屉、`+` 按钮的勾选菜单、
    // 右键菜单、下拉、角色窗口、浮动面板、tooltip）都会被那条线**穿过**。
    //
    // 2026-10-18：从"只支持一个抽屉矩形"改成**一帧一份遮挡列表**。
    // 之前只有抽屉这一处调用，`+` 菜单和所有弹出层都还是漏的（用户报的
    // "菜单列表中间穿插着分割线"）。现在由 EditorApp 每帧把所有浮在布局
    // 之上的窗口矩形都塞进来，分隔条逐段裁掉落在它们里面的部分：
    //   · 画只画可见段
    //   · 命中判定也只用可见段（否则在菜单上拖会误拖分栏）
    // 列表为空 = 行为与没有这个功能时逐像素一致。
    struct OcclRect {
        float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    };
    void clearOccluders() { m_occluders.clear(); }
    void addOccluder(float x0, float y0, float x1, float y1) {
        if (x1 - x0 < 1.0f || y1 - y0 < 1.0f) return;  // 空矩形直接忽略
        m_occluders.push_back({std::min(x0, x1), std::min(y0, y1),
                               std::max(x0, x1), std::max(y0, y1)});
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

    // ------------------------------------------------------------
    // 叶子面板组（E29-1：槽位 → 面板的映射从常量变成运行时可写）
    // ------------------------------------------------------------
    // 为什么先做这一步：E29-2（拖拽换位）/ E29-3（合并成标签组）都要求
    // "某个位置现在装的是哪个面板"能改。改造前它是 PanelDesc::layoutSlot
    // 这张常量表决定的，改了没地方存，也没法落盘。
    //
    // ⚠ 这一层**不改视觉**：默认树装出来的矩形与改造前逐像素一致。
    static constexpr int kSideA = 0;
    static constexpr int kSideB = 1;

    int nodeCount() const { return static_cast<int>(m_nodes.size()); }
    // 某侧是不是叶子（子节点侧不能直接放面板 —— movePanel 会拒绝）
    bool isLeaf(int node, int side) const;
    int groupSize(int node, int side) const;
    int groupPanel(int node, int side, int idx) const;
    int activeIndex(int node, int side) const;
    // 整组覆盖（读盘用）。panels 里的非法槽位在这里**静默丢掉**：
    // 老配置里有、这版删掉的面板不该让整份布局加载失败。
    bool setGroup(int node, int side, const std::vector<int>& panels, int active);
    // **整份恢复**（读盘专用）。groups[node][side] = 该叶子要装的面板。
    // ⚠⚠ 读盘**不能**逐格 setGroup：两个叶子互换面板时会互相把对方摘掉 ——
    //   第一版就是这么错的（(2,A) 拿 inspector 时，(2,B) 的 inspector 被
    //   "跨位置去重"丢掉；接着 (2,B) 拿 hierarchy 又从 A 摘走 → 右栏只剩一格，
    //   而"insp.y < hier.y"这条断言还**假绿**了）。这里一次性替换整棵树，
    //   顺序无关。空数组 = 该格清空（默认值由调用方先填进 groups）。
    void restoreGroups(const std::vector<int> (&groups)[kNodeCount][2]);
    bool activateInGroup(int node, int side, int idx);
    // panel 现在在哪：(node, side, 组内下标)。找不到返回 false。
    bool locatePanel(int panel, int& node, int& side, int& idx) const;
    // 把面板搬到某侧：从原位置摘掉，追加到目标组并激活。
    //   · 目标侧不是叶子 / 面板非法（kSlotContent 恒不进树）→ false
    //   · 已经在目标组里 → 只切换活动项（幂等，不算失败）
    //   · 原位置被摘空 → 那侧 live 变 false，邻居吃掉空间（既有机制）
    bool movePanel(int panel, int node, int side);
    // 交换两侧的**活动**面板（E29-2 落点的核心操作）。任一侧为空 → false。
    bool swapActive(int nodeA, int sideA, int nodeB, int sideB);
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

    // 递归：只收集"线"（不画、不判命中），供交点求交用。
    // ⚠ 与 processSplitters 用**同一套** eff/aW/bx 公式：两处各写一遍的话，
    //   改比例算法时交点会与线本身错开（差几像素就吸不到角）。
    void collectCornerLines(int nodeIdx, float x, float y, float w, float h);
    // 画"加号"抓手 + 命中时光标。画在前景层，压在分隔线之上。
    void drawCornerGrips();

    // 一条分隔线。c = 固定坐标（竖线是 x、横线是 y），[t0,t1] = 跨度。
    struct LineSeg {
        float c = 0.0f;
        float t0 = 0.0f;
        float t1 = 0.0f;
        int node = -1;
    };

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

    // ---- 角落抓手（E29-5）----
    // 每帧重算；`m_cornerHot` 是"鼠标压在哪个交点"，-1 = 没有。
    std::vector<LineSeg> m_vLines;      // 竖线（Dir::Horizontal 的节点）
    std::vector<LineSeg> m_hLines;      // 横线（Dir::Vertical 的节点）
    std::vector<SplitCorner> m_corners;
    int m_cornerHot = -1;

    // 首帧尚未按窗口尺寸初始化比例
    bool m_initialized = false;
    // 上次算比例时的窗口宽度（用来判断"尺寸变化很大"）
    float m_lastInitW = 0.0f;

    float m_dispW = 0.0f;
    float m_dispH = 0.0f;

    float m_toolbarH = 48.0f;   // 工具栏高度（可被 setToolbarHeight 覆盖）
    float m_bottomInset = 0.0f; // 底栏（+ 停靠的 Content 抽屉）占掉的高度

    // 左栏拖动下限（按当前编辑模式由 setLeftMinWidth 写入节点 0 的 minA）
    float m_leftMin = 0.0f;

    // 遮挡矩形（浮层 / 弹层 / 浮动面板）。见 addOccluder。
    // 刻意用一组 float 而不是 ImVec2：这个头文件不 include imgui.h
    // （EditorContext 也不 include），为两个点引一整包头文件不划算。
    std::vector<OcclRect> m_occluders;

    std::vector<SplitNode> m_nodes;
    int m_root = -1;

    // 被最大化的槽位（kSlotNone = 没有）
    int m_maximized = kSlotNone;
};

} // namespace editor
