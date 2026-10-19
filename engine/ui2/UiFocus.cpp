#include "ui2/UiFocus.h"

#include "core/Logger.h"

#include <cmath>

namespace ui2 {

const std::string UiFocusStack::kEmpty;

// ---------------------------------------------------------------- 帧

void UiFocusStack::beginFrame(std::vector<UiFocusItem> items) {
    m_items = std::move(items);
    if (m_items.empty()) {
        m_index = -1;
        return;
    }
    // 焦点项可能这一帧不在了（面板收起 / 按钮禁用）。此时**保持 m_index
    // 不变是错的** —— 它可能越界，也可能指向别的项。做法：把上一次的 id
    // 记住，能对上就重新定位，对不上就清空（让调用方决定要不要 focusFirst）。
    if (m_index >= 0) {
        if (m_index < static_cast<int>(m_items.size()) &&
            m_items[static_cast<std::size_t>(m_index)].enabled) {
            // 下标仍然有效 —— 但项的顺序可能变了，所以按 id 再核一次。
            // 这里不直接信任下标：菜单重建时 push 顺序变化是最常见的
            // "焦点莫名其妙跳到别的按钮"来源。
            const std::string want = m_lastLogged;
            if (!want.empty()) {
                const int i = indexOf(want);
                if (i >= 0 && m_items[static_cast<std::size_t>(i)].enabled)
                    m_index = i;
                else
                    m_index = -1;
            }
        } else {
            m_index = -1;
        }
    }
}

// ---------------------------------------------------------------- 查询

const std::string& UiFocusStack::focusId() const {
    if (m_index < 0 || m_index >= static_cast<int>(m_items.size())) return kEmpty;
    return m_items[static_cast<std::size_t>(m_index)].id;
}

int UiFocusStack::indexOf(const std::string& id) const {
    for (std::size_t i = 0; i < m_items.size(); ++i) {
        if (m_items[i].id == id && m_items[i].enabled)
            return static_cast<int>(i);
    }
    return -1;
}

int UiFocusStack::hitIndex(float px, float py) const {
    for (std::size_t i = 0; i < m_items.size(); ++i) {
        const UiFocusItem& it = m_items[i];
        if (!it.enabled) continue;
        if (it.rect.contains(px, py)) return static_cast<int>(i);
    }
    return -1;
}

// ---------------------------------------------------------------- 焦点

bool UiFocusStack::setFocus(const std::string& id) {
    const int i = indexOf(id);
    m_index = i;
    logFocusIfChanged();
    return i >= 0;
}

void UiFocusStack::clearFocus() {
    m_index = -1;
    logFocusIfChanged();
}

bool UiFocusStack::focusFirst() {
    if (m_items.empty()) return false;
    // "左上优先"：y 最小者胜，y 相同取 x 最小。这不是随意定的 ——
    // 菜单第一项就是几何上最上面那个，读屏/视觉焦点都从那儿开始。
    int best = -1;
    float by = 0.0f, bx = 0.0f;
    for (std::size_t i = 0; i < m_items.size(); ++i) {
        if (!m_items[i].enabled) continue;
        const UiRect& r = m_items[i].rect;
        if (best < 0 || r.cy() < by ||
            (std::fabs(r.cy() - by) < 1.0f && r.cx() < bx)) {
            best = static_cast<int>(i);
            by = r.cy();
            bx = r.cx();
        }
    }
    m_index = best;
    logFocusIfChanged();
    return best >= 0;
}

// ---------------------------------------------------------------- 导航

int UiFocusStack::nearest(int from, int dx, int dy) const {
    const int n = static_cast<int>(m_items.size());
    if (from < 0 || from >= n) return -1;
    const UiRect& cur = m_items[static_cast<std::size_t>(from)].rect;
    const float cx = cur.cx();
    const float cy = cur.cy();

    int best = -1;
    float bestScore = 0.0f;
    for (int i = 0; i < n; ++i) {
        if (i == from) continue;
        const UiFocusItem& it = m_items[static_cast<std::size_t>(i)];
        if (!it.enabled) continue;
        const float ox = it.rect.cx() - cx;
        const float oy = it.rect.cy() - cy;
        // ① 必须在方向的半平面内。1px 容差：同一行的两个按钮中心 y 可能
        //    差不到 1px（布局不取整），严格 `> 0` 会把它们判成"不在下方"。
        const float along = (dx != 0) ? static_cast<float>(dx) * ox
                                      : static_cast<float>(dy) * oy;
        if (along <= 1.0f) continue;
        const float lateral = (dx != 0) ? std::fabs(oy) : std::fabs(ox);
        // ② 侧向偏移罚 2 倍（见头文件"三条定义"）。
        const float score = along + 2.0f * lateral;
        if (best < 0 || score < bestScore) {
            best = i;
            bestScore = score;
        }
    }
    if (best >= 0) return best;

    // ③ 回绕：该方向上没有候选 → 取反方向最远的那个。
    //    （三按钮垂直列里，↓ 到底再按一次回到顶部 —— 这是菜单的通行手感。）
    int wrap = -1;
    float wrapKey = 0.0f;
    for (int i = 0; i < n; ++i) {
        if (i == from) continue;
        const UiFocusItem& it = m_items[static_cast<std::size_t>(i)];
        if (!it.enabled) continue;
        const float ox = it.rect.cx() - cx;
        const float oy = it.rect.cy() - cy;
        const float along = (dx != 0) ? static_cast<float>(dx) * ox
                                      : static_cast<float>(dy) * oy;
        if (wrap < 0 || along < wrapKey) {
            wrap = i;
            wrapKey = along;
        }
    }
    return wrap;
}

bool UiFocusStack::move(int dx, int dy) {
    if (m_items.empty()) return false;
    if (dx == 0 && dy == 0) return false;
    const int from = (m_index >= 0) ? m_index : 0;
    const int to = nearest(from, dx, dy);
    if (to < 0) return false;
    m_index = to;
    logFocusIfChanged();
    return true;
}

bool UiFocusStack::cycle(bool backward) {
    const int n = static_cast<int>(m_items.size());
    if (n == 0) return false;
    int start = (m_index >= 0) ? m_index : (backward ? 0 : -1);
    for (int k = 0; k < n; ++k) {
        start = backward ? (start - 1 + n) % n : (start + 1) % n;
        if (m_items[static_cast<std::size_t>(start)].enabled) {
            m_index = start;
            logFocusIfChanged();
            return true;
        }
    }
    return false;
}

bool UiFocusStack::hoverAt(float px, float py) {
    const int i = hitIndex(px, py);
    if (i < 0) return false;
    if (i == m_index) return true;
    m_index = i;
    logFocusIfChanged();
    return true;
}

// ---------------------------------------------------------------- 路由

bool UiFocusStack::route(const UiNav& nav, std::string* activatedId) {
    if (activatedId) activatedId->clear();
    // 没有可聚焦项 = 这一帧没有菜单 → **一点输入都不吃**（全放行）。
    if (m_items.empty()) return false;

    bool consumed = false;

    // 方向键：还没焦点时先把焦点立起来（键盘"随便按一下就选中"的手感），
    // 有焦点再移动。两者都算消费 —— 否则暂停时按方向键角色会跟着走。
    if (nav.moveX != 0 || nav.moveY != 0) {
        if (m_index < 0) focusFirst();
        else move(nav.moveX, nav.moveY);
        consumed = true;
    }

    if (nav.tab || nav.shiftTab) {
        cycle(nav.shiftTab);
        consumed = true;
    }

    if (nav.activate) {
        if (m_index < 0) focusFirst();
        if (activatedId && m_index >= 0)
            *activatedId = m_items[static_cast<std::size_t>(m_index)].id;
        consumed = true;
    }

    return consumed;
}

// ---------------------------------------------------------------- 锚点

void UiFocusStack::logFocusIfChanged() {
    const std::string& id = focusId();
    if (id == m_lastLogged) return;
    m_lastLogged = id;
    // ⚠ 只在变化时打：每帧都打会让脚本的 `tail -1` 永远读到正确值。
    // ⚠ `id=` 里不能有空格（脚本按 `\S*` 解析）。
    VK_LOG_INFO("UI2-FOCUS id=%s", id.empty() ? "none" : id.c_str());
}

}  // namespace ui2
