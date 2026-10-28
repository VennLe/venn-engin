#include "PanelDragDrop.h"

#include <algorithm>
#include <cmath>

namespace editor {

// ============================================================
// 纯查询
// ============================================================

void slotRectOf(const LayoutRects& L, int slot, float& x, float& y, float& w,
                float& h) {
    const Rect* r = nullptr;
    switch (slot) {
        case kSlotHierarchy: r = &L.hierarchy; break;
        case kSlotInspector: r = &L.inspector; break;
        case kSlotViewport:  r = &L.viewport;  break;
        case kSlotModes:     r = &L.modes;     break;
        default: break;
    }
    if (!r) {
        x = y = w = h = 0.0f;
        return;
    }
    x = r->x;
    y = r->y;
    w = r->w;
    h = r->h;
}

namespace {

// 一张"点是否在矩形里"的判定，两处共用一份 —— 两处各写一遍的话，
// 将来改判定（比如加圆角内缩）只改一处，另一处会变成隐蔽的差异。
bool pointInRect(float px, float py, float x, float y, float w, float h) {
    if (w <= 1.0f || h <= 1.0f) return false;   // 0 尺寸 = 不参与（见头文件）
    return px >= x && px < x + w && py >= y && py < y + h;
}

} // namespace

int slotAtPoint(const LayoutRects& L, float px, float py) {
    // 四个槽位按"谁在上面"没有优先级问题：它们互不重叠（分栏树保证）。
    static const int kSlots[4] = {kSlotHierarchy, kSlotInspector, kSlotViewport,
                                  kSlotModes};
    for (int s : kSlots) {
        float x, y, w, h;
        slotRectOf(L, s, x, y, w, h);
        if (pointInRect(px, py, x, y, w, h)) return s;
    }
    return kSlotNone;
}

int titleBarAtPoint(const LayoutRects& L, float px, float py, float titleH) {
    static const int kSlots[4] = {kSlotHierarchy, kSlotInspector, kSlotViewport,
                                  kSlotModes};
    for (int s : kSlots) {
        float x, y, w, h;
        slotRectOf(L, s, x, y, w, h);
        if (!pointInRect(px, py, x, y, w, h)) continue;
        // 顶部 titleH 像素内才算抓手。
        // ⚠ 用 `< y + titleH` 而不是 `<=`：与 ImGui 标题栏的判定一致，
        //   差一像素不影响手感，但让"边界"只有一种写法。
        if (py < y + titleH) return s;
        return kSlotNone;   // 落在面板里但不在标题栏 → 不是抓手
    }
    return kSlotNone;
}

bool canDragSlot(int slot) {
    // modes 的例外见头文件里的说明。
    return slot == kSlotHierarchy || slot == kSlotInspector ||
           slot == kSlotViewport;
}

bool canBeTabMember(int slot) {
    // ⚠ "能拖" 与 "能并" 是两件事，不要合并成一个函数：
    //   · viewport 能拖（整格交换），但**不能并** —— 非活动标签不绘制，
    //     并进去 = 3D 视口从画面上消失；
    //   · modes 两者都不行（合成槽位，整块就是"当前模式 + 参数"）。
    return slot == kSlotHierarchy || slot == kSlotInspector;
}

bool homeCellOf(int slot, int& node, int& side) {
    // 与 `SplitLayout::buildDefault` 的四个叶子逐一对齐（改那边要改这边）。
    // ⚠ 一一对应是这条路能成立的前提：四个槽位恰好四个叶子，所以"送回老家"
    //   永远有地方可放，不会出现"送回去但那一格被占了"。
    switch (slot) {
        case kSlotModes:     node = 0; side = SplitLayout::kSideA; return true;
        case kSlotViewport:  node = 1; side = SplitLayout::kSideA; return true;
        case kSlotHierarchy: node = 2; side = SplitLayout::kSideA; return true;
        case kSlotInspector: node = 2; side = SplitLayout::kSideB; return true;
        default: return false;
    }
}

bool inCenterZone(float x, float y, float w, float h, float px, float py) {
    if (w <= 1.0f || h <= 1.0f) return false;
    if (px < x || px >= x + w || py < y || py >= y + h) return false;

    // 边距：25%，但夹在 [min(8, 40%), 40%] 之间。
    //   · 小格子（窄条）按 25% 算中部只剩几像素，对不准 → 抬到 8px 下限；
    //   · 大格子按 25% 算边缘带宽 250px，交换手势反而要刻意往边上怼 → 由
    //     上限兜住（这里 25% < 40%，只有很小的格子才会碰到这两条边界）。
    auto edge = [](float len) {
        float m = len * 0.25f;
        const float lo = std::min(8.0f, len * 0.40f);
        const float hi = len * 0.40f;
        if (m < lo) m = lo;
        if (m > hi) m = hi;
        return m;
    };
    const float mx = edge(w);
    const float my = edge(h);
    return px >= x + mx && px < x + w - mx && py >= y + my && py < y + h - my;
}

// ============================================================
// 状态机
// ============================================================

void PanelDragState::reset() {
    armed = false;
    active = false;
    srcSlot = kSlotNone;
    startX = startY = 0.0f;
    targetSlot = kSlotNone;
    targetAction = DropAction::None;
}

PanelDragOutcome panelDragStep(SplitLayout& split, const LayoutRects& L,
                               PanelDragState& st, const DragInput& in,
                               float titleH) {
    PanelDragOutcome out;

    // ---- 取消（Esc）----
    // 优先级最高：无论在哪个阶段，Esc 一律把状态清干净。不清的话，用户在
    // 拖动中途按 Esc 再松开，会在松手的位置**照样**完成一次交换。
    if (in.esc && (st.armed || st.active)) {
        st.reset();
        return out;
    }

    // ---- 阶段一：还没进入拖拽 ----
    if (!st.active) {
        // 按下点必须落在某个可拖面板的标题栏上。
        // ⚠ 只在 clicked 那一帧判断，之后鼠标再移进标题栏**不再**arm ——
        //   否则"从视口里划出来经过标题栏"会凭空开始一次拖拽。
        if (!st.armed && in.clicked) {
            const int s = titleBarAtPoint(L, in.x, in.y, titleH);
            if (s != kSlotNone && canDragSlot(s)) {
                int n = -1, side = -1, idx = -1;
                // 面板在树里找不到 → 不 arm（比如它被最大化顶掉了，
                // 或者布局还没初始化）。宁可什么都不做，也不要拖一个
                // 落不下去的源。
                if (split.locatePanel(s, n, side, idx)) {
                    st.armed = true;
                    st.srcSlot = s;
                    st.startX = in.x;
                    st.startY = in.y;
                }
            }
        }

        if (st.armed) {
            if (!in.down) {
                // 按下又松开、且没挪够距离 = 一次普通点击（给窗口焦点 /
                // 双击折叠 / 点 × 都走这条路）。清掉候选，不产生交换。
                st.reset();
            } else {
                const float dx = in.x - st.startX;
                const float dy = in.y - st.startY;
                if (std::sqrt(dx * dx + dy * dy) > kPanelDragThreshold) {
                    st.active = true;
                    st.targetSlot = kSlotNone;
                }
            }
        }
        if (!st.active) return out;   // 本帧没进入拖拽态
    }

    // ---- 阶段二：拖动中 ----
    // 落点 = 鼠标所在的**面板矩形**（整个矩形，不只是标题栏 —— 拖到哪一格
    // 就在哪一格松手，这是 Blender 的手感）。指向自己 = 没有落点。
    int hit = slotAtPoint(L, in.x, in.y);
    if (hit == st.srcSlot) hit = kSlotNone;
    st.targetSlot = hit;

    int sn = -1, ss = -1, si = -1;
    int tn = -1, ts = -1, ti = -1;
    st.targetAction = DropAction::None;
    if (hit != kSlotNone && split.locatePanel(st.srcSlot, sn, ss, si) &&
        split.locatePanel(hit, tn, ts, ti)) {
        // 同一格的两个标签之间拖来拖去不叫拖拽 —— 那是点标签就该做的事。
        if (!(sn == tn && ss == ts)) {
            float x, y, w, h;
            slotRectOf(L, hit, x, y, w, h);
            const bool center = inCenterZone(x, y, w, h, in.x, in.y);
            // 中部 = 并入目标格（E29-3 的新手势）；但只有双方都允许当标签
            // 成员时才成立，否则退回"整格交换"，免得**中部落下去什么都不发生**
            // —— 用户看不出来是"被拒绝"还是"没接住"。
            if (center && canBeTabMember(st.srcSlot) && canBeTabMember(hit)) {
                st.targetAction = DropAction::Merge;
            } else {
                st.targetAction = DropAction::Swap;
            }
        }
    }

    if (!in.down) {
        // 松开：按落点动作执行。
        //   · Merge —— 把面板**追加**到目标格（`movePanel`），源格被摘空后
        //     自动收成 0 宽（E06-5 的既有机制），视觉上就是"两块并成一块"。
        //   · Swap —— 交换两侧的**活动**面板（`swapActive`）。用"交换"而不是
        //     "搬走"，是因为 E29-2 时代它就定型了：交换后格子数恒定，用户
        //     拖来拖去不会越拖越空。
        if (st.targetAction == DropAction::Merge) {
            if (split.movePanel(st.srcSlot, tn, ts)) {
                out.from = st.srcSlot;
                out.to = hit;
                out.action = DropAction::Merge;
            }
        } else if (st.targetAction == DropAction::Swap) {
            if (split.swapActive(sn, ss, tn, ts)) {
                out.from = st.srcSlot;
                out.to = hit;
                out.action = DropAction::Swap;
            }
        }
        st.reset();
    }
    return out;
}


// ============================================================
// 角落手势（E29-5）
// ============================================================

void CornerDragState::reset() {
    armed = false;
    active = false;
    srcSlot = kSlotNone;
    targetSlot = kSlotNone;
    action = DropAction::None;
    startX = startY = 0.0f;
    cornerX = cornerY = 0.0f;
}

namespace {

// 与交点贴着的那些格子里，挑"在 sign 那一侧"的一格。
//   axis = 0 → 左右找；sign = -1 找左边那格（它的右边界压在线上），
//              sign = +1 找右边那格（左边界压在线上）
//   axis = 1 → 上下找；sign = -1 找上面那格（下边界压在线上），+1 找下面那格
//
// 为什么要"贴边"这个条件：同一个方向上可能有好几格（比如右边那格又被上下
// 切成两块），但**只有与这条线相邻**的那一格才是这次合并的对象。用格心
// 距离去挑的话，会挑到隔着一整块的同侧面板。
//
// 三格交于一点（T 形）时同侧会有两个候选 —— 取格心离鼠标（另一轴）更近的
// 那个：拖的时候手偏上就是想动上面那格。
int cellBesideCorner(const LayoutRects& L, float cx, float cy, int axis,
                     int sign, float mx, float my, int exclude) {
    static const int kSlots[4] = {kSlotHierarchy, kSlotInspector, kSlotViewport,
                                  kSlotModes};
    const float eps = 8.0f;   // 分隔线本身只有 ±2，8px 足够吃掉浮点误差
    int best = kSlotNone;
    float bestD = 1.0e9f;
    for (int s : kSlots) {
        if (s == exclude) continue;
        float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
        slotRectOf(L, s, x, y, w, h);
        if (w <= 1.0f || h <= 1.0f) continue;   // 0 尺寸 = 这一格没显示

        // ① 在 sign 那一侧的边必须压在线上
        const float edge = (axis == 0) ? ((sign < 0) ? (x + w) : x)
                                       : ((sign < 0) ? (y + h) : y);
        const float line = (axis == 0) ? cx : cy;
        if (std::fabs(edge - line) > eps) continue;

        // ② 另一轴上的跨度必须覆盖交点（否则它虽然贴着线，却不是"角上那一格"）
        const float lo = (axis == 0) ? y : x;
        const float hi = (axis == 0) ? (y + h) : (x + w);
        const float c2 = (axis == 0) ? cy : cx;
        if (c2 < lo - eps || c2 > hi + eps) continue;

        // ③ T 形时按"格心离鼠标多远"挑
        const float mid = (lo + hi) * 0.5f;
        const float d = std::fabs(mid - ((axis == 0) ? my : mx));
        if (d < bestD) {
            bestD = d;
            best = s;
        }
    }
    return best;
}

} // namespace

PanelDragOutcome cornerDragStep(SplitLayout& split, const LayoutRects& L,
                               const SplitCorner* hotCorner,
                               CornerDragState& st, const DragInput& in) {
    PanelDragOutcome out;

    // Esc 一律清干净（与标题栏拖拽同一套：不清的话松手会照样落一次）
    if (in.esc && (st.armed || st.active)) {
        st.reset();
        return out;
    }

    // ---- 阶段一：起手 ----
    if (!st.active) {
        if (!st.armed && in.clicked && hotCorner) {
            st.armed = true;
            st.startX = in.x;
            st.startY = in.y;
            // 交点**当场记下来**：拖到一半布局可能已经变了（面板被合并后
            // 那一侧收成 0，角的几何就没了），每帧重读会把源判到别处。
            st.cornerX = hotCorner->x;
            st.cornerY = hotCorner->y;
        }
        if (st.armed) {
            if (!in.down) {
                st.reset();   // 按在角上点了一下 = 什么都没做
            } else {
                const float dx = in.x - st.startX;
                const float dy = in.y - st.startY;
                if (std::sqrt(dx * dx + dy * dy) > kPanelDragThreshold) {
                    st.active = true;
                }
            }
        }
        if (!st.active) return out;
    }

    // ---- 阶段二：拖动中，每帧按方向重算源/目标（高亮要跟着手走）----
    const float dx = in.x - st.cornerX;
    const float dy = in.y - st.cornerY;
    st.srcSlot = kSlotNone;
    st.targetSlot = kSlotNone;
    st.action = DropAction::None;

    // 主轴取位移大的那个：横向拖 = 左右合并，纵向拖 = 上下合并。
    const bool horiz = std::fabs(dx) >= std::fabs(dy);
    if (horiz && std::fabs(dx) > kCornerMinDrag) {
        const int sign = (dx < 0.0f) ? -1 : 1;     // 拖向左 = -1
        // 目标 = 拖向的那一侧；源 = 反方向的另一侧（"右边的并进左边"）
        st.targetSlot =
            cellBesideCorner(L, st.cornerX, st.cornerY, 0, sign, in.x, in.y,
                             kSlotNone);
        st.srcSlot = cellBesideCorner(L, st.cornerX, st.cornerY, 0, -sign, in.x,
                                      in.y, st.targetSlot);
    } else if (!horiz && std::fabs(dy) > kCornerMinDrag) {
        const int sign = (dy < 0.0f) ? -1 : 1;     // 拖向上 = -1
        st.targetSlot =
            cellBesideCorner(L, st.cornerX, st.cornerY, 1, sign, in.x, in.y,
                             kSlotNone);
        st.srcSlot = cellBesideCorner(L, st.cornerX, st.cornerY, 1, -sign, in.x,
                                      in.y, st.targetSlot);
    }

    if (st.srcSlot != kSlotNone && st.targetSlot != kSlotNone &&
        st.srcSlot != st.targetSlot) {
        // 与"拖到目标格中部"完全同一条规则：能当标签成员就 Merge，
        // 否则退回 Swap（免得松手什么都不发生，用户分不清是拒绝还是没接住）。
        st.action = (canBeTabMember(st.srcSlot) && canBeTabMember(st.targetSlot))
                        ? DropAction::Merge
                        : DropAction::Swap;
    }

    if (!in.down) {
        if (st.action != DropAction::None) {
            int sn = -1, ss = -1, si = -1;
            int tn = -1, ts = -1, ti = -1;
            if (split.locatePanel(st.srcSlot, sn, ss, si) &&
                split.locatePanel(st.targetSlot, tn, ts, ti) &&
                !(sn == tn && ss == ts)) {
                if (st.action == DropAction::Merge) {
                    if (split.movePanel(st.srcSlot, tn, ts)) {
                        out.from = st.srcSlot;
                        out.to = st.targetSlot;
                        out.action = DropAction::Merge;
                    }
                } else {
                    if (split.swapActive(sn, ss, tn, ts)) {
                        out.from = st.srcSlot;
                        out.to = st.targetSlot;
                        out.action = DropAction::Swap;
                    }
                }
            }
        }
        st.reset();
    }
    return out;
}

// ============================================================
// 标签组（E29-3）
// ============================================================

void collectTabGroups(const SplitLayout& split, const LayoutRects& L,
                      std::vector<TabGroupInfo>& out) {
    out.clear();
    for (int n = 0; n < split.nodeCount(); ++n) {
        for (int side = 0; side < 2; ++side) {
            const int sz = split.groupSize(n, side);
            if (sz < 2) continue;   // 只有一个面板的格子不画标签条

            TabGroupInfo g;
            g.node = n;
            g.side = side;
            g.active = split.activeIndex(n, side);
            g.panels.reserve(static_cast<size_t>(sz));
            for (int i = 0; i < sz; ++i) {
                g.panels.push_back(split.groupPanel(n, side, i));
            }
            // 整格矩形 = **活动面板**的矩形。
            // ⚠ 这就是 E29-1 留下的语义：LayoutRects 按面板槽位写，活动面板
            //   拿到整格、非活动的是 0。所以这里不需要另算一套格子几何 ——
            //   另算一套的话，标签条和它下面的面板迟早会错开。
            const int act = (g.active >= 0 && g.active < sz) ? g.panels[static_cast<size_t>(g.active)]
                                                            : g.panels[0];
            slotRectOf(L, act, g.x, g.y, g.w, g.h);
            // 该格当前没显示（被最大化顶掉 / 被关掉收成 0）→ 不画标签条。
            // 不判这一条会在 0 尺寸矩形上画出一排 20px 高的标签，浮在别处。
            if (g.w <= 1.0f || g.h <= 1.0f) continue;
            out.push_back(std::move(g));
        }
    }
}

float tabStripPills(const Rect& cell, float titleH, const std::vector<int>& panels,
                    int active, const std::vector<float>& widths,
                    std::vector<TabPill>& out) {
    out.clear();
    if (panels.size() < 2) return 0.0f;
    if (titleH < 14.0f) return 0.0f;              // 行高太小：画出来是一排色块
    if (cell.w < 96.0f) return 0.0f;              // 窄到放不下两枚标签

    // ⚠ 上下各内缩 2px：分隔线画在**前景层**（±1.5px 粗），标签条也在前景层，
    //   不内缩的话上面那个格子的横分隔线会压在标签条顶边上（肉眼是一道脏边）。
    const float insetY = 2.0f;
    const float lead = 6.0f;    // 左留白：别贴着竖直分隔线
    const float gap = 4.0f;
    // 右侧给窗口自己的关闭 ×（Hierarchy / Inspector 都带 p_open）留位置。
    const float tail = 26.0f;

    const float h = titleH - insetY * 2.0f;
    if (h < 10.0f) return 0.0f;
    const float limit = cell.x + cell.w - tail;
    float x = cell.x + lead;

    for (size_t i = 0; i < panels.size(); ++i) {
        float w = (i < widths.size()) ? widths[i] : 64.0f;
        if (w < 28.0f) w = 28.0f;                 // 极短名字（"UI"）也要能点
        if (x + w > limit) break;                 // 放不下就**截断**，不换行
        TabPill p;
        p.x = x;
        p.y = cell.y + insetY;
        p.w = w;
        p.h = h;
        p.panel = panels[i];
        p.index = static_cast<int>(i);
        p.active = (static_cast<int>(i) == active);
        out.push_back(p);
        x += w + gap;
    }
    return out.empty() ? 0.0f : h;
}

int tabAtPoint(const std::vector<TabPill>& pills, float px, float py) {
    for (size_t i = 0; i < pills.size(); ++i) {
        if (pointInRect(px, py, pills[i].x, pills[i].y, pills[i].w, pills[i].h)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace editor
