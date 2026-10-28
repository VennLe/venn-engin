// ============================================================
// editor/LayoutSelfTest.cpp —— E29-1 验收（`MYVK_LAYOUT_TEST=1`）
//
// 全部是**纯数值**断言：只调 SplitLayout 的纯计算接口（beginFrame / apply
// / movePanel / swapActive / setGroup），不碰 handleSplitters ——
// 那条路要 ImGui 上下文（拖拽 / 悬停 / tooltip）。
//
// 分组：
//   A 默认树      —— 改造后的默认装出来的东西与改造前逐像素一致
//   B 定位        —— locatePanel 找得准；不在树里的槽位（Content）必须被拒
//   C 搬家        —— 拒绝子树侧 / 自己搬自己幂等 / 跨叶子后源被摘空 / 搬得回来
//   D 交换        —— 活动面板互换、空侧拒绝
//   E 矩形        —— 搬家与交换**真的**改变了矩形（这是唯一能证明"接上了"的
//                    一类断言：只看组里的值变了，可能根本没写进 LayoutRects）
//   F 拖拽        —— E29-2 的状态机：抓手命中 / 阈值 / Esc 取消 / 落下交换。
//                    `panelDragStep` 的输入是纯数据，所以「拖」这件事不用开窗口
//                    也能完整跑（这也是这次没把状态机写进 EditorApp 的原因）。
//   G 标签组      —— E29-3：中部并入 / 边缘交换 / 视口不许并 / 点标签切换 /
//                    双击拆回 / 标签条几何。同样纯数值。
// ============================================================

#include "LayoutSelfTest.h"

#include "SplitLayout.h"
#include "PanelDragDrop.h"

#include "core/Logger.h"

#include <cmath>
#include <string>
#include <vector>

namespace editor {

namespace {

int g_pass = 0;
int g_fail = 0;

// ⚠ detail 里不能有空格（全仓自检的硬约定：脚本用 `detail=(\S*)` 解析）。
void check(const char* name, bool ok, const std::string& detail = {}) {
    VK_LOG_INFO("LAYOUT-TEST %s actual=%d expected=1 tol=0 detail=%s result=%s",
                name, ok ? 1 : 0, detail.c_str(), ok ? "PASS" : "FAIL");
    if (ok) ++g_pass;
    else ++g_fail;
}

void checkInt(const char* name, int actual, int expected) {
    const bool ok = (actual == expected);
    VK_LOG_INFO("LAYOUT-TEST %s actual=%d expected=%d tol=0 result=%s", name,
                actual, expected, ok ? "PASS" : "FAIL");
    if (ok) ++g_pass;
    else ++g_fail;
}

void checkFloat(const char* name, float actual, float expected, float tol) {
    const bool ok = std::fabs(actual - expected) <= tol;
    VK_LOG_INFO("LAYOUT-TEST %s actual=%.3f expected=%.3f tol=%.3f result=%s",
                name, actual, expected, tol, ok ? "PASS" : "FAIL");
    if (ok) ++g_pass;
    else ++g_fail;
}

// 一份"刚开编辑器"的布局：默认树 + 已按 1600x900 算过一遍矩形。
//
// ⚠ 每次都新建，不复用：搬家类断言会改树，共用一个实例时前一条的副作用
//   会让后一条的期望变得难以推理（本项目在 fixture 上踩过一次同类坑）。
struct Fixture {
    SplitLayout split;
    LayoutRects rects;

    Fixture() {
        split.beginFrame(1600.0f, 900.0f);
        split.syncPanelVisibility();
        split.apply(rects);
    }

    void reapply() { split.apply(rects); }
};

constexpr float kEps = 0.5f;

// ============================================================
// A 默认树
// ============================================================
void groupDefault() {
    Fixture f;

    checkInt("default_node_count", f.split.nodeCount(), 3);
    checkInt("default_root_a_modes", f.split.groupPanel(0, SplitLayout::kSideA, 0),
             kSlotModes);
    checkInt("default_mid_a_viewport", f.split.groupPanel(1, SplitLayout::kSideA, 0),
             kSlotViewport);
    check("default_right_stack",
          f.split.groupPanel(2, SplitLayout::kSideA, 0) == kSlotHierarchy &&
              f.split.groupPanel(2, SplitLayout::kSideB, 0) == kSlotInspector);

    // 每个叶子只有一个面板（E29-1 的"不改视觉"承诺）
    checkInt("default_leaf_group_size", f.split.groupSize(2, SplitLayout::kSideA), 1);
    // 子树侧不是叶子 → 组大小必须是 0（否则 movePanel 的拒绝分支失去依据）
    checkInt("default_subtree_side_not_leaf", f.split.groupSize(0, SplitLayout::kSideB), 0);
}

// ============================================================
// B 定位
// ============================================================
void groupLocate() {
    Fixture f;
    int n = -1, s = -1, i = -1;

    const bool found = f.split.locatePanel(kSlotInspector, n, s, i);
    check("locate_inspector", found && n == 2 && s == SplitLayout::kSideB && i == 0);
    check("locate_content_rejected", !f.split.locatePanel(kSlotContent, n, s, i));
    check("locate_unknown_rejected", !f.split.locatePanel(99, n, s, i));

    // 定位失败时输出参数必须是 -1（调用方拿它当下标用，-1 才是"没有"）
    f.split.locatePanel(kSlotContent, n, s, i);
    checkInt("locate_failure_writes_minus_one", n, -1);
}

// ============================================================
// C 搬家
// ============================================================
void groupMove() {
    {
        Fixture f;
        // 目标侧是子树 → 拒绝，且位置一个都不能变
        check("move_to_subtree_rejected",
              !f.split.movePanel(kSlotHierarchy, 1, SplitLayout::kSideB));
        check("move_rejected_kept_position",
              f.split.groupPanel(2, SplitLayout::kSideA, 0) == kSlotHierarchy);
    }
    {
        Fixture f;
        // 搬到自己身上：**返回 true**（只切活动项）。返回 false 会让落点逻辑
        // 弹"不能放在这里"，而拖到自己身上是常见误操作，不是错误。
        check("move_self_idempotent",
              f.split.movePanel(kSlotInspector, 2, SplitLayout::kSideB));
        checkInt("move_self_group_size", f.split.groupSize(2, SplitLayout::kSideB), 1);
    }
    {
        Fixture f;
        // 跨叶子：把 Hierarchy 搬进视口那一格 → 那一组变 2 个（标签组雏形），
        // 活动项指到**刚搬进来的那个**（用户想看的是它）。
        check("move_cross_leaf",
              f.split.movePanel(kSlotHierarchy, 1, SplitLayout::kSideA));
        checkInt("move_cross_leaf_group_size", f.split.groupSize(1, SplitLayout::kSideA), 2);
        checkInt("move_cross_leaf_active", f.split.activeIndex(1, SplitLayout::kSideA), 1);
        checkInt("move_empties_source", f.split.groupSize(2, SplitLayout::kSideA), 0);

        // 搬回来 → 源组恢复成 1，目标组也回到 1
        check("move_back", f.split.movePanel(kSlotHierarchy, 2, SplitLayout::kSideA));
        checkInt("move_back_group_size", f.split.groupSize(2, SplitLayout::kSideA), 1);
        checkInt("move_back_target_size", f.split.groupSize(1, SplitLayout::kSideA), 1);
    }
    {
        Fixture f;
        // setGroup：组内重复 + 跨位置重复都要被丢掉。
        // inspector 已经在 (2,B)，所以这里只剩 hierarchy 一个。
        std::vector<int> want{kSlotHierarchy, kSlotHierarchy, kSlotInspector};
        f.split.setGroup(2, SplitLayout::kSideA, want, 0);
        checkInt("setgroup_dedup", f.split.groupSize(2, SplitLayout::kSideA), 1);
    }
    {
        Fixture f;
        // 非法槽位（Content）进 setGroup 必须被丢掉 → 组空。
        std::vector<int> want{kSlotContent};
        f.split.setGroup(0, SplitLayout::kSideA, want, 0);
        checkInt("setgroup_drops_content", f.split.groupSize(0, SplitLayout::kSideA), 0);
    }
}

// ============================================================
// D 交换
// ============================================================
void groupSwap() {
    {
        Fixture f;
        check("swap_ok", f.split.swapActive(2, SplitLayout::kSideA, 2, SplitLayout::kSideB));
        check("swap_moved_a", f.split.groupPanel(2, SplitLayout::kSideA, 0) == kSlotInspector);
        check("swap_moved_b", f.split.groupPanel(2, SplitLayout::kSideB, 0) == kSlotHierarchy);
        check("swap_back",
              f.split.swapActive(2, SplitLayout::kSideA, 2, SplitLayout::kSideB) &&
                  f.split.groupPanel(2, SplitLayout::kSideA, 0) == kSlotHierarchy);
    }
    {
        Fixture f;
        // 空侧没有可交换的东西 → 拒绝（不是"把对面搬空"）
        f.split.setGroup(2, SplitLayout::kSideB, {}, 0);
        check("swap_empty_side_rejected",
              !f.split.swapActive(2, SplitLayout::kSideA, 2, SplitLayout::kSideB));
    }
}

// ============================================================
// E 矩形（唯一能证明"组变了 → 画面真的变了"的一组）
// ============================================================
void groupRects() {
    {
        Fixture f;
        const Rect& hi = f.rects.hierarchy;
        const Rect& in = f.rects.inspector;
        // 默认：Hierarchy 在 Inspector 正上方，同宽，边贴边
        check("rect_default_stack",
              hi.h > 1.0f && in.h > 1.0f && std::fabs(hi.y + hi.h - in.y) <= kEps &&
                  std::fabs(hi.w - in.w) <= kEps);
        // 模式栏贴最左，视口在它右边
        check("rect_modes_leftmost",
              f.rects.modes.x <= kEps &&
                  f.rects.viewport.x >= f.rects.modes.x + f.rects.modes.w - kEps);
    }
    {
        Fixture f;
        f.split.swapActive(2, SplitLayout::kSideA, 2, SplitLayout::kSideB);
        f.reapply();
        // 交换后 Inspector 跑到上面 —— 这一条才证明"映射接上了 LayoutRects"，
        // 只看 groupPanel() 变了是不够的（可能根本没写进矩形）。
        check("rect_follows_swap",
              f.rects.inspector.y + kEps < f.rects.hierarchy.y);
    }
    {
        Fixture f;
        f.split.movePanel(kSlotHierarchy, 1, SplitLayout::kSideA);
        f.reapply();
        // 搬走之后那个槽位在树里不存在了 → 必须是 0 矩形。
        // ⚠ 不清零的话，LayoutRects 逐帧复用会把上一帧的矩形留给它，于是
        //   同一个面板在旧位置和新位置各画一遍。
        // ⚠ 这里消失的是 **Viewport**：Hierarchy 搬进 (1,A) 后那一格变成
        //   标签组 [viewport, hierarchy] 且活动项是 hierarchy。第一版把断言
        //   写成 inspector.w==0（actual=330）—— **期望错了，实现是对的**
        //   （本仓纪律：FAIL 先怀疑期望）。
        checkFloat("rect_vanished_viewport_zeroed", f.rects.viewport.w, 0.0f, kEps);
        check("rect_merged_group_takes_slot",
              f.rects.hierarchy.w > 1.0f && f.rects.hierarchy.x > 1.0f);
    }
}

// ============================================================
// F 拖拽换位（E29-2）
// ============================================================
// 这里**不模拟鼠标**，只是把「鼠标当时的样子」喂给状态机 —— 这正是把
// DragInput 做成纯数据的价值：整套拖拽逻辑可以在这里一次跑完。
void groupDrag() {
    constexpr float kTitleH = 24.0f;   // 与 ImGui 的默认标题栏高度接近即可

    // ---- 命中面 ----
    {
        Fixture f;
        const Rect& hi = f.rects.hierarchy;
        const Rect& in = f.rects.inspector;

        const float hx = hi.x + hi.w * 0.5f;
        const float hy = hi.y + kTitleH * 0.5f;   // 标题栏正中
        checkInt("drag_hit_title", titleBarAtPoint(f.rects, hx, hy, kTitleH),
                 kSlotHierarchy);
        // 同一个 x，往下挪到标题栏之外 → 不是抓手（面板内容区不能起拖）
        checkInt("drag_hit_body_not_title",
                 titleBarAtPoint(f.rects, hx, hi.y + kTitleH + 40.0f, kTitleH),
                 kSlotNone);
        // 整个矩形的命中（落点用）：面板中部要命中
        checkInt("drag_hit_panel_body",
                 slotAtPoint(f.rects, in.x + in.w * 0.5f, in.y + in.h * 0.5f),
                 kSlotInspector);
        // 矩形外 → 谁都不是（菜单栏区域）
        checkInt("drag_hit_outside", slotAtPoint(f.rects, 5.0f, 5.0f),
                 kSlotNone);

        // 模式栏能被当落点，但**不能被拖起来**（合成槽位，见 canDragSlot）
        const float mx = f.rects.modes.x + 10.0f;
        const float my = f.rects.modes.y + kTitleH * 0.5f;
        checkInt("drag_modes_is_drop_target",
                 titleBarAtPoint(f.rects, mx, my, kTitleH), kSlotModes);
        check("drag_modes_not_a_source", !canDragSlot(kSlotModes));
        check("drag_hierarchy_is_a_source", canDragSlot(kSlotHierarchy));
    }

    // ---- 阈值：点击 ≠ 拖拽 ----
    {
        Fixture f;
        PanelDragState st;
        const Rect& hi = f.rects.hierarchy;
        DragInput in;
        in.clicked = true;
        in.down = true;
        in.x = hi.x + hi.w * 0.5f;
        in.y = hi.y + kTitleH * 0.5f;
        panelDragStep(f.split, f.rects, st, in, kTitleH);
        check("drag_armed_on_click", st.armed && !st.active);
        checkInt("drag_armed_src", st.srcSlot, kSlotHierarchy);

        // 松开、没挪 → 一次普通点击，不能产生交换
        in.clicked = false;
        in.down = false;
        const PanelDragOutcome o = panelDragStep(f.split, f.rects, st, in, kTitleH);
        check("drag_click_is_not_a_drag", !o.applied());
        check("drag_state_cleared_after_click", !st.armed && !st.active);
        // 一次点击不能改动树
        check("drag_click_kept_tree",
              f.split.groupPanel(2, SplitLayout::kSideA, 0) == kSlotHierarchy);
    }

    // ---- 阈值：小于 6px 不激活 ----
    {
        Fixture f;
        PanelDragState st;
        const Rect& hi = f.rects.hierarchy;
        DragInput in;
        in.clicked = true;
        in.down = true;
        in.x = hi.x + hi.w * 0.5f;
        in.y = hi.y + kTitleH * 0.5f;
        panelDragStep(f.split, f.rects, st, in, kTitleH);

        in.clicked = false;
        in.y += 3.0f;   // < kPanelDragThreshold(6)
        panelDragStep(f.split, f.rects, st, in, kTitleH);
        check("drag_below_threshold_not_active", !st.active);

        in.y += 6.0f;   // 累计 9px
        panelDragStep(f.split, f.rects, st, in, kTitleH);
        check("drag_above_threshold_active", st.active);
    }

    // ---- Esc 取消 ----
    {
        Fixture f;
        PanelDragState st;
        st.active = true;
        st.srcSlot = kSlotHierarchy;
        DragInput in;
        in.down = true;
        in.x = 800.0f;
        in.y = 400.0f;
        in.esc = true;
        const PanelDragOutcome o = panelDragStep(f.split, f.rects, st, in, kTitleH);
        check("drag_esc_no_swap", !o.applied());
        check("drag_esc_clears_state", !st.active && !st.armed);
    }

    // ---- 落下交换（端到端：矩形真的要换）----
    // ⚠ E29-3 之后**中部**落点是"并入成标签"，交换要落在**边缘**。
    //   这一条改的是手势，不是实现 —— 交换的机器（swapActive）一位没动。
    {
        Fixture f;
        PanelDragState st;
        const Rect& hi = f.rects.hierarchy;
        const Rect& in = f.rects.inspector;

        DragInput d;
        d.clicked = true;
        d.down = true;
        d.x = hi.x + hi.w * 0.5f;
        d.y = hi.y + kTitleH * 0.5f;
        panelDragStep(f.split, f.rects, st, d, kTitleH);

        // 往 Inspector 的**左边缘**拖：先过阈值，再移到边缘带里
        d.clicked = false;
        d.y += 20.0f;
        panelDragStep(f.split, f.rects, st, d, kTitleH);
        d.x = in.x + in.w * 0.10f;   // 左侧 10% < 25% 边距 → 边缘
        d.y = in.y + in.h * 0.5f;
        panelDragStep(f.split, f.rects, st, d, kTitleH);
        checkInt("drag_target_is_inspector", st.targetSlot, kSlotInspector);
        check("drag_edge_action_is_swap", st.targetAction == DropAction::Swap);

        d.down = false;
        const PanelDragOutcome o = panelDragStep(f.split, f.rects, st, d, kTitleH);
        check("drag_drop_swapped", o.applied() && o.action == DropAction::Swap);
        checkInt("drag_drop_from", o.from, kSlotHierarchy);
        checkInt("drag_drop_to", o.to, kSlotInspector);

        // ⚠ 只看 from/to 证明不了「画面真的换了」—— 必须复算矩形
        f.reapply();
        check("drag_rect_follows_drop",
              f.rects.inspector.y + kEps < f.rects.hierarchy.y);
        check("drag_state_cleared_after_drop",
              !st.armed && !st.active && st.targetSlot == kSlotNone);
    }

    // ---- 原地放下 / 拖到空白处：都不算交换 ----
    {
        Fixture f;
        PanelDragState st;
        const Rect& hi = f.rects.hierarchy;

        DragInput d;
        d.clicked = true;
        d.down = true;
        d.x = hi.x + hi.w * 0.5f;
        d.y = hi.y + kTitleH * 0.5f;
        panelDragStep(f.split, f.rects, st, d, kTitleH);
        d.clicked = false;
        d.y += 20.0f;
        panelDragStep(f.split, f.rects, st, d, kTitleH);

        // 松手时鼠标还在源格 → 目标为空
        d.down = false;
        PanelDragOutcome o = panelDragStep(f.split, f.rects, st, d, kTitleH);
        check("drag_drop_on_self_no_swap", !o.applied());

        // 拖到菜单栏（任何面板之外）→ 同样不交换
        PanelDragState st2;
        DragInput d2;
        d2.clicked = true;
        d2.down = true;
        d2.x = hi.x + hi.w * 0.5f;
        d2.y = hi.y + kTitleH * 0.5f;
        panelDragStep(f.split, f.rects, st2, d2, kTitleH);
        d2.clicked = false;
        d2.x = 800.0f;
        d2.y = 4.0f;    // 菜单栏
        panelDragStep(f.split, f.rects, st2, d2, kTitleH);
        checkInt("drag_target_outside_none", st2.targetSlot, kSlotNone);
        d2.down = false;
        o = panelDragStep(f.split, f.rects, st2, d2, kTitleH);
        check("drag_drop_outside_no_swap", !o.applied());
    }
}

// ============================================================
// G 标签组（E29-3）
// ============================================================
// 这一组测的是 E29-3 **唯一新增的语义**：同一段落点，中部 = 并入目标格，
// 边缘 = 整格交换。以及那条"并进去还能出来"的退路（送回老家）。
void groupTabs() {
    constexpr float kTitleH = 24.0f;

    // ---- 中部 / 边缘判定，以及"能拖 ≠ 能并" ----
    {
        Fixture f;
        const Rect in = f.rects.inspector;
        const float cx = in.x + in.w * 0.5f;
        const float cy = in.y + in.h * 0.5f;
        check("zone_center_is_center", inCenterZone(in.x, in.y, in.w, in.h, cx, cy));
        check("zone_left_edge_not_center",
              !inCenterZone(in.x, in.y, in.w, in.h, in.x + in.w * 0.08f, cy));
        check("zone_bottom_edge_not_center",
              !inCenterZone(in.x, in.y, in.w, in.h, cx, in.y + in.h * 0.95f));
        // 极窄格子：中部不能缩没（边距下限 8px 兜住），否则窄栏永远只能交换
        check("zone_narrow_still_has_center", inCenterZone(0, 0, 20, 400, 10, 200));

        check("tabmember_hierarchy", canBeTabMember(kSlotHierarchy));
        check("tabmember_inspector", canBeTabMember(kSlotInspector));
        check("tabmember_viewport_no", !canBeTabMember(kSlotViewport));
        check("tabmember_modes_no", !canBeTabMember(kSlotModes));
        // 视口**能拖**（整格交换）但**不能并** —— 两条判定必须分开
        check("drag_viewport_yes", canDragSlot(kSlotViewport));
    }

    // ---- 中部落下 = 并入目标格 ----
    {
        Fixture f;
        PanelDragState st;
        const Rect hi = f.rects.hierarchy;
        const Rect in = f.rects.inspector;

        DragInput d;
        d.clicked = true;
        d.down = true;
        d.x = hi.x + hi.w * 0.5f;
        d.y = hi.y + kTitleH * 0.5f;
        panelDragStep(f.split, f.rects, st, d, kTitleH);

        d.clicked = false;
        d.y += 20.0f;                       // 先过 6px 阈值
        panelDragStep(f.split, f.rects, st, d, kTitleH);
        d.x = in.x + in.w * 0.5f;           // 目标格正中
        d.y = in.y + in.h * 0.5f;
        panelDragStep(f.split, f.rects, st, d, kTitleH);
        checkInt("merge_target_is_inspector", st.targetSlot, kSlotInspector);
        check("merge_action_is_merge", st.targetAction == DropAction::Merge);

        d.down = false;
        const PanelDragOutcome o = panelDragStep(f.split, f.rects, st, d, kTitleH);
        check("merge_applied", o.applied() && o.action == DropAction::Merge);
        checkInt("merge_group_size", f.split.groupSize(2, SplitLayout::kSideB), 2);
        // 源格被摘空 → 收成 0（E06-5 的既有机制，"两块并成一块"就是它）
        checkInt("merge_source_cell_emptied", f.split.groupSize(2, SplitLayout::kSideA), 0);
        // 新并进去的成为**活动**标签（Blender 也是这样：并完就看到它）
        const int ai = f.split.activeIndex(2, SplitLayout::kSideB);
        checkInt("merge_new_tab_active",
                 f.split.groupPanel(2, SplitLayout::kSideB, ai), kSlotHierarchy);

        f.reapply();
        // 整格给了活动面板，非活动的必须是 0（apply 先清零）
        checkFloat("merge_inactive_rect_zero", f.rects.inspector.w, 0.0f, kEps);
        check("merge_active_rect_is_whole_cell",
              f.rects.hierarchy.w > 1.0f && f.rects.hierarchy.h > in.h - kEps);

        // ---- 标签组收集 ----
        std::vector<TabGroupInfo> groups;
        collectTabGroups(f.split, f.rects, groups);
        checkInt("tabgroups_count", static_cast<int>(groups.size()), 1);
        if (groups.size() == 1) {
            checkInt("tabgroups_members", static_cast<int>(groups[0].panels.size()), 2);
            check("tabgroups_rect_matches_cell", groups[0].w > 1.0f && groups[0].h > 1.0f);
        }

        // ---- 标签条几何 ----
        std::vector<TabPill> pills;
        std::vector<float> widths{60.0f, 70.0f};
        const Rect cell{f.rects.hierarchy.x, f.rects.hierarchy.y,
                        f.rects.hierarchy.w, f.rects.hierarchy.h};
        const float stripH = tabStripPills(cell, kTitleH, groups[0].panels,
                                          groups[0].active, widths, pills);
        check("tabstrip_height_positive", stripH > 0.0f);
        checkInt("tabstrip_pill_count", static_cast<int>(pills.size()), 2);
        if (pills.size() == 2) {
            // 左留白 6px；第二枚在第一枚右边
            check("tabstrip_first_near_left_edge",
                  pills[0].x > cell.x && pills[0].x < cell.x + 10.0f);
            check("tabstrip_second_right_of_first",
                  pills[1].x >= pills[0].x + pills[0].w);
            checkInt("tabstrip_hit_first",
                     tabAtPoint(pills, pills[0].x + 5.0f, pills[0].y + 5.0f), 0);
            checkInt("tabstrip_hit_second",
                     tabAtPoint(pills, pills[1].x + 5.0f, pills[1].y + 5.0f), 1);
            // 右侧留给关闭 × 的区域不该命中（否则点 × 会切标签）
            checkInt("tabstrip_hit_right_margin",
                     tabAtPoint(pills, cell.x + cell.w - 3.0f, pills[0].y + 5.0f), -1);
        }
        // 只有一个面板 → 不画标签条（这是"默认布局零像素变化"的前提）
        std::vector<TabPill> none;
        const float h1 = tabStripPills(cell, kTitleH, {kSlotInspector}, 0, {60.0f}, none);
        checkFloat("tabstrip_single_panel_none", h1, 0.0f, 0.001f);

        // ---- 点标签切换活动项 ----
        f.split.activateInGroup(2, SplitLayout::kSideB, 0);
        f.reapply();
        check("activate_swaps_visible",
              f.rects.inspector.w > 1.0f && f.rects.hierarchy.w <= kEps);

        // ---- 送回老家：并进去之后必须能出来 ----
        // 源格已经被收成 0 宽，鼠标**点不到它** —— 没有这条路就是单向门。
        int hn = -1, hs = -1;
        const bool hasHome = homeCellOf(kSlotHierarchy, hn, hs);
        check("home_cell_of_hierarchy", hasHome && hn == 2 && hs == SplitLayout::kSideA);
        const bool sent = f.split.movePanel(kSlotHierarchy, hn, hs);
        f.reapply();
        check("detach_restores_two_cells",
              sent && f.split.groupSize(2, SplitLayout::kSideA) == 1 &&
                  f.split.groupSize(2, SplitLayout::kSideB) == 1);
        check("detach_rects_both_visible",
              f.rects.hierarchy.h > 1.0f && f.rects.inspector.h > 1.0f);
    }

    // ---- 中部落到**不能当标签**的格子 → 退回整格交换 ----
    // 不退回的话，中部落下去什么都不发生，用户分不清是"被拒绝"还是"没接住"。
    {
        Fixture f;
        PanelDragState st;
        const Rect hi = f.rects.hierarchy;
        const Rect vp = f.rects.viewport;

        DragInput d;
        d.clicked = true;
        d.down = true;
        d.x = hi.x + hi.w * 0.5f;
        d.y = hi.y + kTitleH * 0.5f;
        panelDragStep(f.split, f.rects, st, d, kTitleH);
        d.clicked = false;
        d.y += 20.0f;
        panelDragStep(f.split, f.rects, st, d, kTitleH);
        d.x = vp.x + vp.w * 0.5f;   // 视口正中
        d.y = vp.y + vp.h * 0.5f;
        panelDragStep(f.split, f.rects, st, d, kTitleH);
        checkInt("merge_into_viewport_target", st.targetSlot, kSlotViewport);
        check("merge_into_viewport_falls_back_to_swap",
              st.targetAction == DropAction::Swap);

        d.down = false;
        const PanelDragOutcome o = panelDragStep(f.split, f.rects, st, d, kTitleH);
        check("viewport_swap_applied",
              o.applied() && o.action == DropAction::Swap);
        // 视口还在，只是换了格子 —— 它没有被并进任何标签组
        f.reapply();
        check("viewport_still_visible", f.rects.viewport.w > 1.0f);
        std::vector<TabGroupInfo> groups;
        collectTabGroups(f.split, f.rects, groups);
        checkInt("viewport_no_tab_group", static_cast<int>(groups.size()), 0);
    }

    // ---- 模式栏同理：能当落点（交换），不能当标签成员 ----
    {
        Fixture f;
        PanelDragState st;
        const Rect in = f.rects.inspector;
        const Rect mo = f.rects.modes;

        DragInput d;
        d.clicked = true;
        d.down = true;
        d.x = in.x + in.w * 0.5f;
        d.y = in.y + kTitleH * 0.5f;
        panelDragStep(f.split, f.rects, st, d, kTitleH);
        d.clicked = false;
        d.y += 20.0f;
        panelDragStep(f.split, f.rects, st, d, kTitleH);
        d.x = mo.x + mo.w * 0.5f;
        d.y = mo.y + mo.h * 0.5f;
        panelDragStep(f.split, f.rects, st, d, kTitleH);
        check("merge_into_modes_falls_back_to_swap",
              st.targetAction == DropAction::Swap);
    }
}

// ============================================================
// H 角落抓手（E29-5）
// ============================================================
// 只测**状态机**（不碰 ImGui）：交点坐标由 rects 反推 —— 竖线在
// hierarchy.x（= 视口右边界），横线在 inspector.y（= hierarchy 下边界）。
// 真实窗口里"角点到底在哪、光标是什么"由 tools/verify_layout_drag.py 的
// F 组验，两边分工：这里守映射（方向 → 源/目标 → 动作），那边守接线。
void groupCorner() {
    auto cornerOf = [](const LayoutRects& L) {
        SplitCorner c;
        c.x = L.hierarchy.x;
        c.y = L.inspector.y;
        return c;
    };

    // ---- 方向 → 源/目标 ----
    {
        Fixture f;
        const SplitCorner c = cornerOf(f.rects);
        CornerDragState st;
        DragInput in;
        in.clicked = true;
        in.down = true;
        in.x = c.x;
        in.y = c.y;
        cornerDragStep(f.split, f.rects, &c, st, in);
        check("corner_armed_on_click", st.armed && !st.active);

        // 往下 60px：目标 = 下面那格（inspector），源 = 上面那格（hierarchy）
        in.clicked = false;
        in.y += 60.0f;
        cornerDragStep(f.split, f.rects, &c, st, in);
        check("corner_drag_active_after_move", st.active);
        checkInt("corner_down_target", st.targetSlot, kSlotInspector);
        checkInt("corner_down_source", st.srcSlot, kSlotHierarchy);
        // 两个都能当标签成员 → Merge
        check("corner_down_is_merge", st.action == DropAction::Merge);

        // 松手：真的并成一组
        in.down = false;
        const PanelDragOutcome o =
            cornerDragStep(f.split, f.rects, &c, st, in);
        check("corner_down_applied", o.applied() &&
                                         o.action == DropAction::Merge);
        checkInt("corner_merge_group_size",
                 f.split.groupSize(2, SplitLayout::kSideB), 2);
        check("corner_state_cleared", !st.armed && !st.active);
    }

    // ---- 横向：目标落在视口上 → 退回交换（视口不能当标签成员）----
    // ⚠ 这条**必须**留在 Swap 上：`movePanel` 会激活被搬进来的面板，
    //   视口一旦变成非活动标签，3D 画面就整块消失（E29-3 的硬约束）。
    {
        Fixture f;
        const SplitCorner c = cornerOf(f.rects);
        CornerDragState st;
        DragInput in;
        in.clicked = true;
        in.down = true;
        in.x = c.x;
        in.y = c.y;
        cornerDragStep(f.split, f.rects, &c, st, in);

        in.clicked = false;
        in.x -= 60.0f;   // 往左
        cornerDragStep(f.split, f.rects, &c, st, in);
        checkInt("corner_left_target", st.targetSlot, kSlotViewport);
        // 源：右边那一格。T 形处上下两格都贴着线，按"格心离鼠标更近"挑 ——
        // 鼠标停在交点的高度上，hierarchy 的格心更近（它比 inspector 矮）。
        checkInt("corner_left_source", st.srcSlot, kSlotHierarchy);
        check("corner_left_falls_back_to_swap", st.action == DropAction::Swap);

        in.down = false;
        const PanelDragOutcome o =
            cornerDragStep(f.split, f.rects, &c, st, in);
        check("corner_swap_applied", o.applied() &&
                                         o.action == DropAction::Swap);
        checkInt("corner_swap_viewport_moved",
                 f.split.groupPanel(2, SplitLayout::kSideA, 0), kSlotViewport);
    }

    // ---- 只点不拖：什么都不许发生 ----
    {
        Fixture f;
        const SplitCorner c = cornerOf(f.rects);
        CornerDragState st;
        DragInput in;
        in.clicked = true;
        in.down = true;
        in.x = c.x;
        in.y = c.y;
        cornerDragStep(f.split, f.rects, &c, st, in);
        in.clicked = false;
        in.down = false;
        const PanelDragOutcome o =
            cornerDragStep(f.split, f.rects, &c, st, in);
        check("corner_click_is_not_a_drag", !o.applied());
        checkInt("corner_click_kept_tree",
                 f.split.groupPanel(2, SplitLayout::kSideA, 0), kSlotHierarchy);
    }

    // ---- 斜着拖：主轴取位移大的那个 ----
    // 这条是"合并还是交换"的分水岭：dx=-30 / dy=+70 时纵向占优 → 上下合并，
    // 而不是"因为往左偏了就按横向去跟视口交换"。手斜一点就换动作的手势，
    // 用起来像 bug。
    {
        Fixture f;
        const SplitCorner c = cornerOf(f.rects);
        CornerDragState st;
        DragInput in;
        in.clicked = true;
        in.down = true;
        in.x = c.x;
        in.y = c.y;
        cornerDragStep(f.split, f.rects, &c, st, in);
        in.clicked = false;
        in.x -= 30.0f;
        in.y += 70.0f;
        cornerDragStep(f.split, f.rects, &c, st, in);
        check("corner_diagonal_uses_dominant_axis",
              st.action == DropAction::Merge && st.targetSlot == kSlotInspector);
    }
}

} // namespace

int runLayoutSelfTest() {
    VK_LOG_INFO("== selftest: layout (E29-1 槽位 / E29-2 拖拽 / E29-3 标签组) ==");

    groupDefault();
    groupLocate();
    groupMove();
    groupSwap();
    groupRects();
    groupDrag();
    groupTabs();
    groupCorner();

    VK_LOG_INFO("LAYOUT-TOTAL pass=%d fail=%d", g_pass, g_fail);
    return g_fail;
}

} // namespace editor
