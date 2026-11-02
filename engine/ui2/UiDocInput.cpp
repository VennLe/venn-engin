#include "ui2/UiDocInput.h"

#include "core/Logger.h"
#include "ui2/UiLayout.h"

#include <cmath>

namespace ui2 {

namespace {

// 命中测试：矩形包含（输出像素）。
// `UiRect::contains` 已经存在，直接复用 —— 两套包含判定迟早会在
// 边界上分叉（一边 `<` 一边 `<=`），而那种差一格只有自动化能发现。
bool inside(const UiRect& r, float px, float py) {
    if (r.w <= 0.0f || r.h <= 0.0f) return false;   // 退化矩形不命中
    return r.contains(px, py);
}

// 把输出像素的点**逆变换**回控件的本地坐标。
// 行列式接近 0 说明这个变换把面积压成了一条线（scale 极端小），此时
// 逆变换没有意义 —— 直接判"不命中"，而不是除以 0 得到 NaN 再污染比较。
bool toLocal(const Ui2Xform& m, float x, float y, float& ox, float& oy) {
    const float det = m.m00 * m.m11 - m.m01 * m.m10;
    if (std::fabs(det) < 1e-9f) return false;
    const float dx = x - m.tx;
    const float dy = y - m.ty;
    ox = (m.m11 * dx - m.m01 * dy) / det;
    oy = (-m.m10 * dx + m.m00 * dy) / det;
    return true;
}

// 点是否命中这条记录（考虑变换与裁剪）。
bool hits(const UiDocPlaced& p, float px, float py) {
    // 被 Scroll 祖先裁掉的区域**点不到**：滚出可视区的行矩形还在
    // （`logRect` 也不判可见性），点上去照样会中 —— 而玩家眼里那里什么都没有。
    if (p.hasClip && !inside(p.clip, px, py)) return false;
    float lx = px, ly = py;
    if (!toLocal(p.xform, px, py, lx, ly)) return false;
    return inside(p.rect, lx, ly);
}

}  // namespace

std::string hitTestDocument(const std::vector<UiDocPlaced>& placed, float px,
                            float py) {
    // ⚠ **倒序**：`placeDocument` 是 DFS 序（父在前、子在后），后画的盖在
    //   上面，所以最后一条是"最上面"的。正序扫会让被面板盖住的按钮抢走
    //   点击 —— 而玩家看到的是面板。
    for (std::size_t i = placed.size(); i > 0; --i) {
        const UiDocPlaced& p = placed[i - 1];
        if (!p.interactive || !p.enabled) continue;
        if (hits(p, px, py)) return p.id;
    }
    return std::string();
}

std::string findDropTarget(const UiDocument& d,
                           const std::vector<UiDocPlaced>& placed, float px,
                           float py, const std::string& tag) {
    if (tag.empty()) return std::string();
    for (std::size_t i = placed.size(); i > 0; --i) {
        const UiDocPlaced& p = placed[i - 1];
        if (!p.enabled) continue;
        const UiWidget* w = findWidgetById(d, p.id);
        if (!w) continue;
        if (!dropAccepts(w->dropAccept, tag)) continue;
        if (hits(p, px, py)) return p.id;
    }
    return std::string();
}

void UiDocInput::reset() {
    m_st = UiDocInteraction{};
    m_pressId.clear();
    m_dragId.clear();
    m_dragTag.clear();
    m_warnedNoAction.clear();
    m_hasPending = false;
    m_pendingX = 0.0f;
    m_pendingY = 0.0f;
}

void UiDocInput::requestClickRef(float rx, float ry, uint32_t outW,
                                 uint32_t outH) {
    const float scale = computeScale(outW, outH);
    if (scale <= 0.0f) return;
    m_hasPending = true;
    m_pendingX = rx * scale;
    m_pendingY = ry * scale;
}

UiDocInput::Result UiDocInput::update(const UiDocument& d,
                                      const std::vector<UiDocPlaced>& placed,
                                      float px, float py, bool down,
                                      bool pressedEdge, bool releasedEdge) {
    Result res;

    // ---- 注入的点击：一次性，等价于"这一帧按下并松开在同一点" ----
    // ⚠ 走的是与真鼠标**完全相同**的下游：(pressId = hit) → 同一个
    //   activate 分支。另一条"注入专用"的捷径会在"注入能点、鼠标点不动"
    //   这种地方分叉，而判据只覆盖其中一条。
    if (m_hasPending) {
        m_hasPending = false;
        px = m_pendingX;
        py = m_pendingY;
        pressedEdge = true;
        releasedEdge = true;
        down = true;
    }

    const std::string hit = hitTestDocument(placed, px, py);
    m_st.hovered = hit;
    m_st.pointerX = px;
    m_st.pointerY = py;

    if (pressedEdge) {
        m_pressId = hit;
        m_st.pressed = hit;
        // 按下但没命中：**也要打**，否则"点歪了"和"按钮没画"分不清。
        if (hit.empty()) {
            VK_LOG_INFO("UIDOC-CLICK hit=0 x=%.1f y=%.1f",
                        static_cast<double>(px), static_cast<double>(py));
        }
        // ---- 拖拽开始（P3-2）----
        // 按下就进拖拽，不设"移动超过 N 像素才算拖"的阈值：
        //   · 阈值会让"按下 → 立刻甩出去"变成一次点击（卡牌游戏里 = 误打出）；
        //   · 而且阈值需要跨帧记住按下时的坐标，多一份状态就多一处不同步。
        // 代价是"可拖的按钮"点下去也会进拖拽状态 —— 但松手时如果指针还在
        // 它自己身上、且没有落点，会回退成一次点击（见下面 releasedEdge）。
        if (!hit.empty()) {
            const UiWidget* w = findWidgetById(d, hit);
            if (w && !w->dragTag.empty()) {
                m_dragId = hit;
                m_dragTag = w->dragTag;
                m_st.dragging = hit;
                m_st.draggingTag = m_dragTag;
                res.dragBegan = true;
                res.dragId = hit;
                res.dragTag = m_dragTag;
                VK_LOG_INFO("UIDOC-DRAG-BEGIN id=%s tag=%s",
                            hit.c_str(), m_dragTag.c_str());
            }
        }
    }

    // 拖拽中：每帧更新"指针底下的合法落点"（高亮用）。
    if (!m_dragId.empty()) {
        m_st.dropTarget = findDropTarget(d, placed, px, py, m_dragTag);
    }

    // ⚠ 顺序：**先按下、后松开**。注入的点击在同一帧里 press+release
    //   都置位，反过来处理会读到已经清空的 m_pressId → 点了没反应，
    //   而真实鼠标一切正常（这种只在自动化里出现的 bug 最难认）。
    if (releasedEdge) {
        // ---- 拖拽结束（P3-2）----
        if (!m_dragId.empty()) {
            res.dragEnded = true;
            res.dragId = m_dragId;
            res.dragTag = m_dragTag;
            res.dropId = m_st.dropTarget;
            const std::string src = m_dragId;
            const std::string dst = m_st.dropTarget;
            if (!dst.empty()) {
                // 落在**自己身上**不算放下（否则"拖起来又松手"会触发一次
                // 自我转移，卡牌会从手牌掉进手牌）。
                if (dst != src) {
                    VK_LOG_INFO("UIDOC-DRAG-DROP id=%s tag=%s to=%s",
                                src.c_str(), m_dragTag.c_str(), dst.c_str());
                } else {
                    res.dropId.clear();
                    VK_LOG_INFO("UIDOC-DRAG-CANCEL id=%s reason=self",
                                src.c_str());
                }
            } else {
                VK_LOG_INFO("UIDOC-DRAG-CANCEL id=%s reason=no-target",
                            src.c_str());
            }
            m_dragId.clear();
            m_dragTag.clear();
            m_st.dragging.clear();
            m_st.draggingTag.clear();
            m_st.dropTarget.clear();
            // ⚠ **拖过就不再产出点击**：否则"把牌拖到敌人身上"会同时
            //   触发"点了一下这张牌"（卡牌游戏里那通常意味着打出去），
            //   一次拖拽结算两次。这里把 pressId 清掉，下面那条
            //   `hit == m_pressId` 自然不会成立。
            m_pressId.clear();
            m_st.pressed.clear();
            return res;
        }

        // 语义 ②：松开必须落在**按下时的那个控件**上。
        if (!m_pressId.empty() && hit == m_pressId) {
            res.id = m_pressId;
        } else if (!m_pressId.empty()) {
            VK_LOG_INFO("UIDOC-CLICK hit=0 id=%s reason=drag-out",
                        m_pressId.c_str());
        }
        m_pressId.clear();
        m_st.pressed.clear();
    } else if (!down) {
        // 没有松开沿但也不是按下状态（UI 被关掉、或松开的那一帧没被看到）
        // → 把按下的视觉状态清掉，免得残留成一个永远按着的按钮。
        m_pressId.clear();
        m_st.pressed.clear();
        // 拖拽同理：鼠标被抢走（切窗口 / 打开文件对话框）时不能一直
        // 挂着"正在拖"，否则下一次移动会把那张牌甩到随便什么地方。
        if (!m_dragId.empty()) {
            VK_LOG_INFO("UIDOC-DRAG-CANCEL id=%s reason=lost-button",
                        m_dragId.c_str());
            m_dragId.clear();
            m_dragTag.clear();
            m_st.dragging.clear();
            m_st.draggingTag.clear();
            m_st.dropTarget.clear();
        }
    }

    if (!res.id.empty()) {
        const UiWidget* w = findWidgetById(d, res.id);
        res.action = w ? w->action : std::string();
        VK_LOG_INFO("UIDOC-CLICK hit=1 id=%s x=%.1f y=%.1f action=%s",
                    res.id.c_str(), static_cast<double>(px),
                    static_cast<double>(py),
                    res.action.empty() ? "(none)" : res.action.c_str());
        // 点了但没配动作：**只提醒一次**。每帧都提醒会刷满日志，而不提醒
        // 的话"这个按钮没反应"就永远是一个谜。
        if (res.action.empty() && m_warnedNoAction != res.id) {
            m_warnedNoAction = res.id;
            VK_LOG_WARN("UIDOC-NOACTION id=%s —— 这个控件点了什么也不做"
                        "（在 UI 编辑器里给它填一个 action）",
                        res.id.c_str());
        }
    }
    return res;
}

}  // namespace ui2
