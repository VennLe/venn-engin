// ============================================================
// editor/DopeSheetWindow.cpp —— 动画摄影表（Dope Sheet，2026-10-28 / R8）
//
// 结构：工具栏（路径 / Reload / Save / Fit / 播放头）
//      → body（**一整块自绘区**：左通道树 + 右时间网格，共用纵向滚动）
//      → 底栏（选中统计 / 类型 / Mirror / Align / Delete）
//
// ⚠ 日志前缀 `DOPE-`。与 `CURVED-` / `TLED-` / `SEQ-` / `ANIM-` / `POSE-`
//   都分开 —— 摄影表与曲线编辑器可能同时开着同一份资产。
// ============================================================

#include "DopeSheetWindow.h"

#include "DebugRects.h"
#include "EditorContext.h"
#include "Localization.h"
#include "PanelDock.h"

#include "assets/Skeleton.h"
#include "core/Logger.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace editor {

namespace {

// 命中半径（像素）。菱形本身只有 3~5px，手指按不了那么准。
constexpr float kHitPx = 7.0f;
// 时间标尺条的高度（顶部那条，点它拖播放头）
constexpr float kRulerH = 18.0f;

const ImVec4 kBadCol(1.00f, 0.45f, 0.35f, 1.0f);
const ImVec4 kWarnCol(1.00f, 0.75f, 0.30f, 1.0f);

// 顶点颜色族：类型靠**形状**区分（实心/空心/点），颜色只管"选没选中"，
// 这样即使主题换了、色弱用户，也能靠形状读出节奏。
const ImU32 kColKey = IM_COL32(120, 190, 255, 255);
const ImU32 kColExtreme = IM_COL32(255, 160, 90, 255);
const ImU32 kColBreak = IM_COL32(150, 230, 190, 255);
const ImU32 kColInb = IM_COL32(180, 180, 195, 255);
const ImU32 kColSel = IM_COL32(255, 210, 90, 255);
const ImU32 kColHot = IM_COL32(255, 255, 255, 255);

const char* pathShort(assets::AnimTrack::Path p) {
    switch (p) {
        case assets::AnimTrack::Translation: return "loc";
        case assets::AnimTrack::Rotation:    return "rot";
        case assets::AnimTrack::Scale:       return "scale";
    }
    return "?";
}

const char* pathLong(assets::AnimTrack::Path p) {
    switch (p) {
        case assets::AnimTrack::Translation: return "translation";
        case assets::AnimTrack::Rotation:    return "rotation";
        case assets::AnimTrack::Scale:       return "scale";
    }
    return "?";
}

bool timeNear(float a, float b) {
    return std::fabs(a - b) <= animation::kKeyTimeEpsilon;
}

std::string nameFromPath(const std::string& p) {
    return std::filesystem::path(p).filename().string();
}

} // namespace

DopeSheetWindow::DopeSheetWindow(EditorContext& ctx) : m_ctx(ctx) {}

// ---------------------------------------------------------------- 宿主绑定

void DopeSheetWindow::bind(const Host& h) {
    if (h.clip != m_host.clip) {
        // 换了一份资产 → 选择/视图/快照全部作废（下标属于旧的那份）
        m_sel.clear();
        m_haveSnap = false;
        m_grab = false;
        m_drag = DragState{};
        m_error.clear();
        VK_LOG_INFO("DOPE-BIND label='%s' tracks=%d keys=%d",
                    h.label ? h.label : "", static_cast<int>(h.clip->tracks.size()),
                    h.clip->totalKeys());
    }
    m_host = h;
    if (!m_open) {
        m_open = true;
        m_loaded = true;
        fitView();
        VK_LOG_INFO("DOPE-OPEN mode=bound path=(host) ok=1 tracks=%d keys=%d",
                    static_cast<int>(clip().tracks.size()), clip().totalKeys());
    }
}

void DopeSheetWindow::unbind() {
    if (!m_host.clip) return;
    VK_LOG_INFO("DOPE-UNBIND label='%s'", m_host.label ? m_host.label : "");
    m_host = Host{};
    m_sel.clear();
    m_haveSnap = false;
    m_grab = false;
    // 窗口不自动关：从 Pose Mode 退出来之后，作者往往还在看这张表
    // （"刚才那几帧到底在什么位置"）。数据源没了就显示空表 + 提示。
}

animation::EditClip& DopeSheetWindow::clip() {
    return m_host.clip ? *m_host.clip : m_own;
}
const animation::EditClip& DopeSheetWindow::clip() const {
    return m_host.clip ? *m_host.clip : m_own;
}
const assets::Skeleton* DopeSheetWindow::skel() const { return m_host.skel; }

void DopeSheetWindow::markDirty() {
    m_dirty = true;
    syncToHost();
}

void DopeSheetWindow::syncToHost() {
    // 宿主那边有自己的名字解析缓存（m_editResolved），不通知就会"改了帧
    // 但画面不动"—— 而画面不动会被误判成"操作没生效"。
    if (m_host.dirty) *m_host.dirty = true;
}

float DopeSheetWindow::head() const {
    if (m_host.head) return *m_host.head;
    return m_headLocal;
}
void DopeSheetWindow::setHead(float t) {
    const float d = std::max(durationOf(), 1.0e-3f);
    const float v = glm::clamp(t, 0.0f, d);
    if (m_host.head) *m_host.head = v;
    else m_headLocal = v;
}

// ---------------------------------------------------------------- 打开 / 存盘

bool DopeSheetWindow::open(const std::string& relPath) {
    m_open = true;
    m_path = relPath;
    std::snprintf(m_pathBuf, sizeof(m_pathBuf), "%s", relPath.c_str());
    if (relPath.empty()) {
        m_loaded = false;
        m_error.clear();
        VK_LOG_INFO("DOPE-OPEN mode=standalone path=(none) ok=0 tracks=0 keys=0");
        return false;
    }
    reload();
    return m_error.empty();
}

void DopeSheetWindow::close() {
    if (!m_open) return;
    if (m_dirty) {
        VK_LOG_WARN("DOPE-CLOSE-DIRTY path=%s（有未存的改动）", m_path.c_str());
    }
    m_open = false;
    m_loaded = false;
    m_dirty = false;
    m_sel.clear();
    m_grab = false;
    m_haveSnap = false;
    VK_LOG_INFO("DOPE-CLOSE path=%s bound=%d", m_path.c_str(), bound() ? 1 : 0);
}

void DopeSheetWindow::reload() {
    if (bound()) {
        // 绑定期间数据源是宿主，读盘没有意义（而且会把作者手上的改动冲掉）
        m_uiError = TR("Bound to Pose Mode — edit it there, or unbind first.");
        VK_LOG_WARN("DOPE-RELOAD-BOUND");
        return;
    }
    std::string err;
    animation::EditClip c;
    if (!animation::loadEditClip(m_path, c, err)) {
        m_error = err;
        m_uiError.clear();
        VK_LOG_WARN("DOPE-OPEN path=%s ok=0 err=%s", m_path.c_str(), err.c_str());
        return;
    }
    m_own = std::move(c);
    m_loaded = true;
    m_dirty = false;
    m_error.clear();
    m_uiError.clear();
    m_sel.clear();
    m_haveSnap = false;
    m_grab = false;
    m_t0 = 0.0f;
    m_t1 = 1.0f;
    fitView();
    VK_LOG_INFO("DOPE-OPEN path=%s ok=1 tracks=%d keys=%d dur=%.3f",
                m_path.c_str(), static_cast<int>(m_own.tracks.size()),
                m_own.totalKeys(), static_cast<double>(m_own.duration));
}

bool DopeSheetWindow::save() {
    if (bound()) {
        // 见头文件 ⚠⑤：一个文件两个写者 = 后存的静默盖掉先存的。
        m_uiError = TR("Bound to Pose Mode — save from the Character window.");
        VK_LOG_WARN("DOPE-SAVE-BLOCKED reason=bound");
        return false;
    }
    if (m_path.empty()) {
        m_uiError = TR("No path.");
        VK_LOG_WARN("DOPE-SAVE-BLOCKED reason=no-path");
        return false;
    }
    std::string err;
    if (!animation::saveEditClip(m_path, m_own, err)) {
        m_uiError = err;
        VK_LOG_ERROR("DOPE-SAVE-FAIL path=%s err=%s", m_path.c_str(), err.c_str());
        return false;
    }
    m_dirty = false;
    m_uiError.clear();
    VK_LOG_INFO("DOPE-SAVE path=%s tracks=%d keys=%d",
                m_path.c_str(), static_cast<int>(m_own.tracks.size()),
                m_own.totalKeys());
    return true;
}

// ---------------------------------------------------------------- 行

bool DopeSheetWindow::isCollapsed(const std::string& joint) const {
    return std::find(m_collapsed.begin(), m_collapsed.end(), joint) !=
           m_collapsed.end();
}

void DopeSheetWindow::toggleCollapsed(const std::string& joint) {
    const auto it = std::find(m_collapsed.begin(), m_collapsed.end(), joint);
    if (it == m_collapsed.end()) m_collapsed.push_back(joint);
    else m_collapsed.erase(it);
    // 日志打在**动作本体**里，不打在调用点：鼠标点三角与注入脚本的
    // `collapse:` 走的是同一个函数，打两处的话脚本那条会静默（R8 验过）。
    VK_LOG_INFO("DOPE-COLLAPSE joint='%s' collapsed=%d", joint.c_str(),
                isCollapsed(joint) ? 1 : 0);
}

void DopeSheetWindow::rebuildRows() {
    m_rows.clear();
    const animation::EditClip& c = clip();
    const std::vector<std::string> joints = c.keyedJoints();
    if (joints.empty()) return;

    const assets::Skeleton* sk = skel();
    if (!sk || sk->jointCount() <= 0) {
        // 没有骨架（独立打开一份资产）：平铺，骨的先后按**首次打帧顺序**。
        // 这时候谈不上层级 —— 名字对不上任何骨架，硬编造一棵树只会误导。
        for (const auto& jn : joints) {
            Row r;
            r.joint = jn;
            m_rows.push_back(r);
            if (isCollapsed(jn)) continue;
            static const assets::AnimTrack::Path kPaths[3] = {
                assets::AnimTrack::Translation, assets::AnimTrack::Rotation,
                assets::AnimTrack::Scale};
            for (auto p : kPaths) {
                const int ti = c.findTrack(jn, p);
                if (ti < 0) continue;
                Row cr;
                cr.joint = jn;
                cr.path = static_cast<int>(p);
                cr.track = ti;
                cr.depth = 1;
                m_rows.push_back(cr);
            }
        }
        return;
    }

    // 骨架模式：按 `topoOrder`（父先于子）走一遍，缩进 = **有键的**祖先个数。
    // 用"全部祖先"会把没打帧的中间骨也算进缩进，于是同一层的两根骨缩进不同 ——
    // 摄影表上一眼就觉得"这个层级乱了"。
    std::vector<char> keyed(static_cast<std::size_t>(sk->jointCount()), 0);
    for (const auto& jn : joints) {
        const int j = sk->findJoint(jn, assets::kJointMatchCaseInsensitive);
        if (j >= 0) keyed[static_cast<std::size_t>(j)] = 1;
    }

    auto depthOf = [&](int j) {
        int d = 0;
        for (int p = sk->joints[static_cast<std::size_t>(j)].parent; p >= 0;
             p = sk->joints[static_cast<std::size_t>(p)].parent) {
            if (keyed[static_cast<std::size_t>(p)]) ++d;
        }
        return d;
    };
    auto hiddenOf = [&](int j) {
        for (int p = sk->joints[static_cast<std::size_t>(j)].parent; p >= 0;
             p = sk->joints[static_cast<std::size_t>(p)].parent) {
            if (keyed[static_cast<std::size_t>(p)] &&
                isCollapsed(sk->joints[static_cast<std::size_t>(p)].name))
                return true;
        }
        return false;
    };

    std::vector<int> order;
    if (!sk->topoOrder.empty()) {
        order = sk->topoOrder;
    } else {
        for (int j = 0; j < sk->jointCount(); ++j) order.push_back(j);
    }

    static const assets::AnimTrack::Path kPaths[3] = {
        assets::AnimTrack::Translation, assets::AnimTrack::Rotation,
        assets::AnimTrack::Scale};

    for (int j : order) {
        if (j < 0 || j >= sk->jointCount()) continue;
        if (!keyed[static_cast<std::size_t>(j)]) continue;
        if (hiddenOf(j)) continue;
        const std::string& jn = sk->joints[static_cast<std::size_t>(j)].name;
        Row r;
        r.joint = jn;
        r.depth = depthOf(j);
        m_rows.push_back(r);
        if (isCollapsed(jn)) continue;
        for (auto p : kPaths) {
            const int ti = c.findTrack(jn, p);
            if (ti < 0) continue;
            Row cr;
            cr.joint = jn;
            cr.path = static_cast<int>(p);
            cr.track = ti;
            cr.depth = r.depth + 1;
            m_rows.push_back(cr);
        }
    }

    // 骨架里找不到的骨：**追加在末尾并且照样列出**。它们是"这份资产的帧
    // 在这个模型上不生效"的唯一线索（`POSE-RESOLVE-MISS` 打的是日志，
    // 而作者盯的是界面）。静默隐藏 = "我的帧去哪了"。
    for (const auto& jn : joints) {
        if (sk->findJoint(jn, assets::kJointMatchCaseInsensitive) >= 0) continue;
        Row r;
        r.joint = jn;
        m_rows.push_back(r);
        for (auto p : kPaths) {
            const int ti = c.findTrack(jn, p);
            if (ti < 0) continue;
            Row cr;
            cr.joint = jn;
            cr.path = static_cast<int>(p);
            cr.track = ti;
            cr.depth = 1;
            m_rows.push_back(cr);
        }
    }
}

// ---------------------------------------------------------------- 视图

float DopeSheetWindow::durationOf() const {
    float d = clip().duration;
    for (const auto& tr : clip().tracks) {
        if (!tr.times.empty()) d = std::max(d, tr.times.back());
    }
    return std::max(d, 0.1f);
}

void DopeSheetWindow::fitView() {
    const float d = durationOf();
    // 左右各留 4% 余量：末帧贴着画布右沿的话，菱形只画得出一半，
    // "最后一帧到底在不在"就看不出来。
    m_t0 = -d * 0.04f;
    m_t1 = d * 1.04f;
    if (m_t1 - m_t0 < 1.0e-3f) m_t1 = m_t0 + 1.0e-3f;
    m_scrollY = 0.0f;
}

float DopeSheetWindow::pxPerSec(const ImVec2& plotMin, const ImVec2& plotMax) const {
    const float span = std::max(m_t1 - m_t0, 1.0e-6f);
    return std::max(plotMax.x - plotMin.x, 1.0f) / span;
}

float DopeSheetWindow::pxOfT(const ImVec2& plotMin, const ImVec2& plotMax,
                             float t) const {
    (void)plotMax;
    return plotMin.x + (t - m_t0) * pxPerSec(plotMin, plotMax);
}

float DopeSheetWindow::tOfPx(const ImVec2& plotMin, const ImVec2& plotMax,
                             float px) const {
    return m_t0 + (px - plotMin.x) / pxPerSec(plotMin, plotMax);
}

void DopeSheetWindow::zoomTime(float anchorPx, float plotX0, float plotX1,
                               float factor) {
    const float w = std::max(plotX1 - plotX0, 1.0f);
    zoomAtFraction(factor, (anchorPx - plotX0) / w);
}

void DopeSheetWindow::zoomAtFraction(float factor, float frac) {
    if (!(factor > 0.0f) || !std::isfinite(factor)) return;
    const float span = m_t1 - m_t0;
    // 最小 1ms 宽、最大 10 分钟：再窄看不清、再宽 float 的精度撑不住
    const float want = glm::clamp(span * factor, 1.0e-3f, 600.0f);
    const float f = glm::clamp(frac, 0.0f, 1.0f);
    const float anchorT = m_t0 + f * span;
    // 锚点时刻在缩放前后的屏幕位置必须**不变**（见头文件 ⚠⑥）
    m_t0 = anchorT - want * f;
    m_t1 = m_t0 + want;
}

void DopeSheetWindow::panTime(float seconds) {
    if (!std::isfinite(seconds)) return;
    m_t0 += seconds;
    m_t1 += seconds;
}

// ---------------------------------------------------------------- 选择

bool DopeSheetWindow::isSelected(const animation::KeyRef& r) const {
    return std::find(m_sel.begin(), m_sel.end(), r) != m_sel.end();
}

void DopeSheetWindow::setSelection(std::vector<animation::KeyRef> s) {
    std::sort(s.begin(), s.end(), [](const animation::KeyRef& a,
                                     const animation::KeyRef& b) {
        return a.track != b.track ? a.track < b.track : a.key < b.key;
    });
    s.erase(std::unique(s.begin(), s.end()), s.end());
    m_sel = std::move(s);
    float lo = 0.0f, hi = 0.0f;
    if (!m_sel.empty() && animation::selectedTimeRange(clip(), m_sel, lo, hi)) {
        VK_LOG_INFO("DOPE-SEL n=%d t=[%.4f,%.4f]", static_cast<int>(m_sel.size()),
                    static_cast<double>(lo), static_cast<double>(hi));
    } else {
        VK_LOG_INFO("DOPE-SEL n=0");
    }
}

void DopeSheetWindow::refsAt(const Row& row, float time,
                             std::vector<animation::KeyRef>& out) const {
    out.clear();
    const animation::EditClip& c = clip();
    if (row.track >= 0) {
        if (row.track >= static_cast<int>(c.tracks.size())) return;
        const int k =
            c.tracks[static_cast<std::size_t>(row.track)].findKey(time);
        if (k >= 0) out.push_back(animation::KeyRef{row.track, k});
        return;
    }
    // 摘要行：同一时刻可能有 rot/loc/scale 三条轨道各一个帧
    for (std::size_t ti = 0; ti < c.tracks.size(); ++ti) {
        if (c.tracks[ti].joint != row.joint) continue;
        const int k = c.tracks[ti].findKey(time);
        if (k >= 0) out.push_back(animation::KeyRef{static_cast<int>(ti), k});
    }
}

// ---------------------------------------------------------------- 铺点

void DopeSheetWindow::layoutKeys(const ImVec2& plotMin, const ImVec2& plotMax,
                                 float pps, float t0, float scrollY,
                                 float rowH) {
    m_hits.clear();
    const animation::EditClip& c = clip();
    for (int ri = 0; ri < static_cast<int>(m_rows.size()); ++ri) {
        const Row& r = m_rows[ri];
        const float y = plotMin.y + static_cast<float>(ri) * rowH - scrollY;
        // 只铺**看得见**的行：一块屏幕 40 行，没必要为 200 行建点
        if (y + rowH < plotMin.y - 1.0f || y > plotMax.y + 1.0f) continue;
        const float cy = y + rowH * 0.5f;

        if (r.track >= 0) {
            if (r.track >= static_cast<int>(c.tracks.size())) continue;
            const animation::EditTrack& tr =
                c.tracks[static_cast<std::size_t>(r.track)];
            for (int k = 0; k < tr.keyCount(); ++k) {
                Hit h;
                h.row = ri;
                h.t = tr.times[static_cast<std::size_t>(k)];
                h.pos = ImVec2(plotMin.x + (h.t - t0) * pps, cy);
                h.refs.push_back(animation::KeyRef{r.track, k});
                h.type = tr.typeAt(k);
                h.sel = isSelected(h.refs[0]);
                m_hits.push_back(std::move(h));
            }
            continue;
        }

        // 骨摘要行：只在**折叠时**铺（展开时下面三行已经把帧摆出来了，
        // 再叠一层就是同一个帧画两遍，点哪颗都说不清）。
        if (!isCollapsed(r.joint)) continue;
        std::vector<float> times;
        for (std::size_t ti = 0; ti < c.tracks.size(); ++ti) {
            if (c.tracks[ti].joint != r.joint) continue;
            for (float t : c.tracks[ti].times) {
                bool dup = false;
                for (float e : times) {
                    if (timeNear(e, t)) { dup = true; break; }
                }
                if (!dup) times.push_back(t);
            }
        }
        std::sort(times.begin(), times.end());
        for (float t : times) {
            Hit h;
            h.row = ri;
            h.t = t;
            h.pos = ImVec2(plotMin.x + (t - t0) * pps, cy);
            // 摘要帧的代表类型取"最有意义"的那个：极端 > 间断 > 中间 > 普通
            std::vector<animation::KeyRef> refs;
            refsAt(r, t, refs);
            animation::KeyType best = animation::KeyType::Key;
            bool allSel = !refs.empty();
            for (const auto& rr : refs) {
                const auto ty =
                    c.tracks[static_cast<std::size_t>(rr.track)].typeAt(rr.key);
                if (static_cast<int>(ty) > static_cast<int>(best)) best = ty;
                if (!isSelected(rr)) allSel = false;
            }
            h.type = best;
            h.sel = allSel;
            h.refs = std::move(refs);
            m_hits.push_back(std::move(h));
        }
    }
}

// ---------------------------------------------------------------- 动作

bool DopeSheetWindow::actSelectAll() {
    setSelection(animation::allKeys(clip()));
    return !m_sel.empty();
}

bool DopeSheetWindow::actClearSelection() {
    setSelection({});
    return true;
}

bool DopeSheetWindow::actSelectKey(const Hit& h, bool additive) {
    if (!additive) {
        setSelection(h.refs);
        return !h.refs.empty();
    }
    // 加选：已经全选中就反选（点第二下取消）—— 与 ImGui 的多选一致
    std::vector<animation::KeyRef> next;
    bool allIn = !h.refs.empty();
    for (const auto& r : h.refs) {
        if (!isSelected(r)) { allIn = false; break; }
    }
    if (allIn) {
        for (const auto& r : m_sel) {
            bool inHit = false;
            for (const auto& hr : h.refs) {
                if (hr == r) { inHit = true; break; }
            }
            if (!inHit) next.push_back(r);
        }
    } else {
        next = m_sel;
        for (const auto& r : h.refs) {
            if (!isSelected(r)) next.push_back(r);
        }
    }
    setSelection(std::move(next));
    return true;
}

bool DopeSheetWindow::actSelectRect(const ImVec2& plotMin, const ImVec2& plotMax,
                                    ImVec2 a, ImVec2 b, bool additive) {
    const float x0 = std::min(a.x, b.x), x1 = std::max(a.x, b.x);
    const float y0 = std::min(a.y, b.y), y1 = std::max(a.y, b.y);
    std::vector<animation::KeyRef> next = additive ? m_sel : std::vector<animation::KeyRef>{};
    int got = 0;
    for (const Hit& h : m_hits) {
        if (h.pos.x < x0 - 2.0f || h.pos.x > x1 + 2.0f) continue;
        if (h.pos.y < y0 - m_rowH * 0.5f || h.pos.y > y1 + m_rowH * 0.5f) continue;
        for (const auto& r : h.refs) {
            next.push_back(r);
            ++got;
        }
    }
    (void)plotMin;
    (void)plotMax;
    setSelection(std::move(next));
    return got > 0;
}

bool DopeSheetWindow::actSelectAtRow(int row, bool wholeRow) {
    (void)wholeRow;
    std::vector<animation::KeyRef> next;
    for (const Hit& h : m_hits) {
        if (h.row != row) continue;
        for (const auto& r : h.refs) next.push_back(r);
    }
    setSelection(std::move(next));
    return !m_sel.empty();
}

bool DopeSheetWindow::actMoveSelected(float dt) {
    if (m_sel.empty() || dt == 0.0f) return false;
    const animation::BatchStats st =
        animation::moveKeys(clip(), m_sel, dt);
    if (st.moved > 0) {
        markDirty();
        // 位移越过当前时长时把可见窗口跟着撑一下，否则"挪出去就看不见了"
        const float d = durationOf();
        if (d > m_t1) m_t1 = d * 1.04f;
    }
    return st.moved > 0;
}

bool DopeSheetWindow::actMirrorSelected(float pivot) {
    if (m_sel.empty()) return false;
    const animation::BatchStats st =
        animation::mirrorKeys(clip(), m_sel, pivot);
    if (st.moved > 0) markDirty();
    return st.moved > 0;
}

bool DopeSheetWindow::actAlignSelected(float target) {
    if (m_sel.empty() || !(target > 0.0f)) return false;
    const animation::BatchStats st =
        animation::alignCycle(clip(), m_sel, target);
    if (st.moved > 0) {
        markDirty();
        fitView();
    }
    return st.moved > 0;
}

bool DopeSheetWindow::actSetSelectedType(animation::KeyType t) {
    if (m_sel.empty()) return false;
    const int n = animation::setKeyTypes(clip(), m_sel, t);
    if (n > 0) markDirty();
    return n > 0;
}

bool DopeSheetWindow::actDeleteSelected() {
    if (m_sel.empty()) return false;
    const animation::EditClip& c = clip();
    // ⚠ 按**帧时刻**删而不是按帧下标：`removeKey` 每删一个就把后面整体前移，
    //   而且轨道空了会整条 erase（轨道下标也变了）。先在"旧下标→时刻+轨道名"
    //   的快照上收集，再按名字找回来删 —— 这样顺序无关。
    struct Victim {
        std::string joint;
        assets::AnimTrack::Path path;
        float t;
    };
    std::vector<Victim> victims;
    for (const auto& r : m_sel) {
        if (r.track < 0 || r.track >= static_cast<int>(c.tracks.size())) continue;
        const animation::EditTrack& tr =
            c.tracks[static_cast<std::size_t>(r.track)];
        if (r.key < 0 || r.key >= tr.keyCount()) continue;
        victims.push_back(Victim{tr.joint, tr.path, tr.times[static_cast<std::size_t>(r.key)]});
    }
    int n = 0;
    for (const auto& v : victims) {
        const int ti = clip().findTrack(v.joint, v.path);
        if (ti < 0) continue;
        const int k = clip().tracks[static_cast<std::size_t>(ti)].findKey(v.t);
        if (k < 0) continue;
        if (clip().removeKey(v.joint, v.path, k)) ++n;
    }
    if (n > 0) {
        markDirty();
        setSelection({});   // 下标全乱了，清空是唯一安全的写法
        VK_LOG_INFO("DOPE-DEL n=%d", n);
    }
    return n > 0;
}

bool DopeSheetWindow::actSetHeadPx(const ImVec2& plotMin, const ImVec2& plotMax,
                                   float px) {
    const float t = tOfPx(plotMin, plotMax, px);
    setHead(t);
    VK_LOG_INFO("DOPE-HEAD t=%.4f", static_cast<double>(head()));
    return true;
}

bool DopeSheetWindow::actZoom(float factor) {
    // 脚本没有鼠标位置，锚点用**播放头** —— 既确定又符合"缩到某一帧附近"
    // 这个真实诉求，而且同一份脚本在任何窗口尺寸下结果一致。
    const float span = m_t1 - m_t0;
    const float frac = (span > 1.0e-9f) ? (head() - m_t0) / span : 0.5f;
    zoomAtFraction(factor, frac);
    VK_LOG_INFO("DOPE-ZOOM factor=%.3f t0=%.4f t1=%.4f",
                static_cast<double>(factor), static_cast<double>(m_t0),
                static_cast<double>(m_t1));
    return true;
}

bool DopeSheetWindow::actPan(float seconds) {
    panTime(seconds);
    VK_LOG_INFO("DOPE-PAN dt=%.4f t0=%.4f t1=%.4f", static_cast<double>(seconds),
                static_cast<double>(m_t0), static_cast<double>(m_t1));
    return true;
}

void DopeSheetWindow::beginSnapshot() {
    m_snap = clip();
    m_snapSel = m_sel;
    m_haveSnap = true;
    // ⚠ EditClip 是纯值类型（vector + string），拷一份就是完整状态。
    //   这比"记录反向位移"可靠：float 上 `(t+d)−d` 不保证等于 `t`。
}

void DopeSheetWindow::restoreSnapshot() {
    if (!m_haveSnap) return;
    clip() = m_snap;
    m_haveSnap = false;
    markDirty();
    // 下标属于快照之后那份数据，但**快照里存的就是原来的下标**，
    // 所以直接把选择也还原 —— Esc 之后应该还是刚才那几帧被选着。
    m_sel = m_snapSel;
    VK_LOG_INFO("DOPE-CANCEL restored sel=%d", static_cast<int>(m_sel.size()));
}

// ---------------------------------------------------------------- 绘制：外框

void DopeSheetWindow::draw(bool& open) {
    if (!m_open) return;

    ImGuiWindowFlags wf = 0;
    const bool docked = applyDockHint(wf);
    if (!docked) {
        ImGui::SetNextWindowSizeConstraints(ImVec2(640.0f, 360.0f),
                                            ImVec2(FLT_MAX, FLT_MAX));
        ImGui::SetNextWindowSize(ImVec2(980.0f, 520.0f), ImGuiCond_FirstUseEver);
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.12f,
                                       io.DisplaySize.y * 0.16f),
                                ImGuiCond_FirstUseEver);
    }

    // ⚠ 稳定 ID 必须等于 `trayWindowName("dope")` = `###panel_dope`，
    //   否则底栏托盘的最小化 / 置顶按 id 找不到这个窗口（#311 踩过）。
    std::string title = TR("Dope sheet");
    if (bound()) {
        title += " — ";
        title += (m_host.label && m_host.label[0]) ? m_host.label : TR("pose mode");
        title += TR(" (live)");
    } else if (!m_path.empty()) {
        title += " — ";
        const std::string nm = clip().name.empty() ? nameFromPath(m_path)
                                                   : clip().name;
        title += nm;
        if (m_dirty) title += " *";
    }
    title += "###panel_dope";

    if (!ImGui::Begin(title.c_str(), docked ? nullptr : &open, wf)) {
        ImGui::End();
        return;
    }
    logRect("DOPE-WIN", ImGui::GetWindowPos(),
            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                   ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    drawToolbar();
    drawBody();
    // ⚠ 注入脚本排在 body **之后**：`selrect` / `zoom` 需要本帧真实的
    //   像素↔时间映射（`m_plot` 那组量是 drawBody 里算的）。排在前面的话
    //   第一帧拿到的是上一帧的几何，`move` 之后的 `dumpkeys` 的 px 全错。
    runInjectedScript();
    drawFooter();

    ImGui::End();
    if (!open) close();
}

void DopeSheetWindow::drawToolbar() {
    ImGui::TextDisabled("%s", TR("Asset path"));
    ImGui::SameLine();
    ImGui::BeginDisabled(bound());
    ImGui::SetNextItemWidth(320.0f);
    if (ImGui::InputTextWithHint("##dope_path", "anim/walk_edit.anim.json",
                                 m_pathBuf, sizeof(m_pathBuf),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
        m_path = m_pathBuf;
        reload();
    }
    logRect("DOPE-PATH", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::Button(TR("Reload"))) {
        m_path = m_pathBuf;
        reload();
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Save"))) save();
    logRect("DOPE-SAVE", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::EndDisabled();
    if (bound()) {
        ImGui::SameLine();
        ImGui::TextColored(kWarnCol, "%s", TR("bound"));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s",
                              TR("Editing the clip open in Pose Mode. Save it "
                                 "from the Character window — one owner per file."));
        }
    }

    ImGui::SameLine();
    if (ImGui::Button(TR("Fit"))) {
        fitView();
        VK_LOG_INFO("DOPE-FIT t0=%.4f t1=%.4f", static_cast<double>(m_t0),
                    static_cast<double>(m_t1));
    }
    logRect("DOPE-FIT", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    ImGui::SameLine();
    ImGui::TextDisabled("%s", TR("head"));
    ImGui::SameLine();
    float h = head();
    ImGui::SetNextItemWidth(90.0f);
    if (ImGui::DragFloat("##dope_head", &h, 0.001f, 0.0f, durationOf(),
                         TR("%.3fs"))) {
        setHead(h);
    }
    logRect("DOPE-HEAD", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    ImGui::SameLine();
    ImGui::TextDisabled("%s %d / %d", TR("keys"), clip().totalKeys(),
                        static_cast<int>(m_sel.size()));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s",
                          TR("Total keys / selected keys. Wheel scrolls rows, "
                             "Ctrl+wheel zooms time, Shift+wheel pans."));
    }

    if (!m_error.empty()) ImGui::TextColored(kBadCol, "%s", m_error.c_str());
    if (!m_uiError.empty()) ImGui::TextColored(kWarnCol, "%s", m_uiError.c_str());
}

// ---------------------------------------------------------------- 绘制：body

void DopeSheetWindow::drawBody() {
    rebuildRows();

    const float reserve = 2.4f * ImGui::GetFrameHeightWithSpacing();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 size(std::max(avail.x, 320.0f),
                      std::max(avail.y - reserve, 200.0f));
    ImGui::InvisibleButton("##dope_body", size);
    const ImVec2 itemMin = ImGui::GetItemRectMin();
    const ImVec2 itemMax = ImGui::GetItemRectMax();
    logRect("DOPE-CANVAS", itemMin, itemMax);

    // ---- 几何 ----
    m_rowH = std::max(ImGui::GetFontSize() * 1.15f, 16.0f);
    m_treeW = glm::clamp(size.x * 0.22f, 120.0f, 240.0f);
    const ImVec2 plotMin(itemMin.x + m_treeW, itemMin.y + kRulerH);
    const ImVec2 plotMax(itemMax.x - 6.0f, itemMax.y - 4.0f);
    const float viewH = std::max(plotMax.y - plotMin.y, m_rowH);
    const float contentH = static_cast<float>(m_rows.size()) * m_rowH;
    m_scrollY = glm::clamp(m_scrollY, 0.0f, std::max(contentH - viewH, 0.0f));

    const float pps = pxPerSec(plotMin, plotMax);
    m_lastPlotMin = plotMin;
    m_lastPlotMax = plotMax;
    m_lastPps = pps;
    m_lastT0 = m_t0;
    // **先铺点再处理输入**：命中判定读 m_hits（见头文件 ⚠②）
    layoutKeys(plotMin, plotMax, pps, m_t0, m_scrollY, m_rowH);

    // ---- 几何日志（去抖）----
    {
        char k[256];
        std::snprintf(k, sizeof(k),
                      "%.5f|%.5f|%.1f|%.1f|%.1f|%d|%.1f|%.1f|%.1f",
                      static_cast<double>(m_t0), static_cast<double>(m_t1),
                      static_cast<double>(plotMin.x), static_cast<double>(plotMax.x),
                      static_cast<double>(m_rowH), static_cast<int>(m_rows.size()),
                      static_cast<double>(m_scrollY), static_cast<double>(m_treeW),
                      static_cast<double>(m_t1 - m_t0));
        if (m_viewKey != k) {
            m_viewKey = k;
            VK_LOG_INFO("DOPE-VIEW t0=%.5f t1=%.5f px0=%.1f px1=%.1f pps=%.2f "
                        "rowH=%.1f rows=%d scroll=%.1f treeW=%.1f",
                        static_cast<double>(m_t0), static_cast<double>(m_t1),
                        static_cast<double>(plotMin.x), static_cast<double>(plotMax.x),
                        static_cast<double>(pps), static_cast<double>(m_rowH),
                        static_cast<int>(m_rows.size()),
                        static_cast<double>(m_scrollY), static_cast<double>(m_treeW));
        }
        char rk[192];
        std::snprintf(rk, sizeof(rk), "%d|%d|%d", static_cast<int>(m_rows.size()),
                      clip().totalKeys(), static_cast<int>(m_sel.size()));
        if (m_sumKey != rk) {
            m_sumKey = rk;
            VK_LOG_INFO("DOPE-SUMMARY rows=%d keys=%d sel=%d dur=%.4f",
                        static_cast<int>(m_rows.size()), clip().totalKeys(),
                        static_cast<int>(m_sel.size()),
                        static_cast<double>(durationOf()));
        }
    }

    // ---- 输入 ----
    ImGuiIO& io = ImGui::GetIO();
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 mouse = io.MousePos;
    const bool inPlot = hovered && mouse.x >= plotMin.x && mouse.y >= plotMin.y;
    const bool inTree = hovered && mouse.x < plotMin.x && mouse.y >= plotMin.y;
    const bool inRuler = hovered && mouse.y < plotMin.y;
    const bool winFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    // 命中当前鼠标下的菱形
    const int rowUnder =
        inPlot ? static_cast<int>(std::floor((mouse.y - plotMin.y + m_scrollY) / m_rowH))
               : -1;
    const Hit* hitUnder = nullptr;
    if (inPlot) {
        for (const Hit& h : m_hits) {
            if (h.row != rowUnder) continue;
            if (std::fabs(h.pos.x - mouse.x) > kHitPx) continue;
            hitUnder = &h;
            break;
        }
    }

    // 滚轮：默认纵向滚动；Ctrl 缩放时间；Shift 平移时间。
    if (hovered && io.MouseWheel != 0.0f) {
        if (io.KeyCtrl) {
            zoomTime(mouse.x, plotMin.x, plotMax.x,
                     std::exp(-io.MouseWheel * 0.22f));
        } else if (io.KeyShift) {
            panTime(-io.MouseWheel * 0.08f * (m_t1 - m_t0));
        } else {
            m_scrollY -= io.MouseWheel * m_rowH * 3.0f;
        }
    }

    // ---- `G` 抓取模式（不按键跟随鼠标）----
    if (m_grab) {
        const float dt = (mouse.x - m_grabStart.x) / std::max(pps, 1.0e-6f);
        if (dt != m_grabApplied) {
            const float step = dt - m_grabApplied;
            if (actMoveSelected(step)) m_grabApplied = dt;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
            ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            restoreSnapshot();
            m_grab = false;
            m_grabApplied = 0.0f;
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            // 左键落定（Blender 也是这个手感）
            m_grab = false;
            m_haveSnap = false;
            VK_LOG_INFO("DOPE-GRAB-END dt=%.4f", static_cast<double>(m_grabApplied));
        }
    }

    // ---- 左键按下：分四种起点 ----
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !m_grab) {
        if (inRuler) {
            beginSnapshot();
            m_drag.kind = Drag::Head;
            m_drag.start = mouse;
            actSetHeadPx(plotMin, plotMax, mouse.x);
        } else if (inPlot && hitUnder) {
            beginSnapshot();
            actSelectKey(*hitUnder, io.KeyShift || io.KeyCtrl);
            m_drag.kind = Drag::Keys;
            m_drag.start = mouse;
            m_drag.moved = false;
            VK_LOG_INFO("DOPE-DRAG-BEGIN kind=key t=%.4f n=%d",
                        static_cast<double>(hitUnder->t),
                        static_cast<int>(m_sel.size()));
        } else if (inTree) {
            // 树上的点击：三角 = 折叠；否则选这一行的全部帧
            const int ri = static_cast<int>(
                std::floor((mouse.y - plotMin.y + m_scrollY) / m_rowH));
            if (ri >= 0 && ri < static_cast<int>(m_rows.size())) {
                const Row& r = m_rows[ri];
                const float rowY = plotMin.y + static_cast<float>(ri) * m_rowH - m_scrollY;
                const float indent = 6.0f + static_cast<float>(r.depth) * 12.0f;
                const float triCenter = itemMin.x + indent + 4.0f;
                if (r.path < 0 && std::fabs(mouse.x - triCenter) <= 7.0f &&
                    mouse.y >= rowY && mouse.y <= rowY + m_rowH) {
                    toggleCollapsed(r.joint);
                } else {
                    actSelectAtRow(ri, true);
                }
            }
        } else if (inPlot) {
            beginSnapshot();
            m_drag.kind = Drag::Marquee;
            m_drag.start = mouse;
            m_drag.moved = false;
            if (!io.KeyShift) setSelection({});
        } else {
            m_drag.kind = Drag::Pan;
            m_drag.start = mouse;
            m_drag.startScroll = m_scrollY;
            m_drag.startT0 = m_t0;
            m_drag.startSpan = m_t1 - m_t0;
        }
    }

    // 中键哪里都算平移
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
        m_drag.kind = Drag::Pan;
        m_drag.start = mouse;
        m_drag.startScroll = m_scrollY;
        m_drag.startT0 = m_t0;
        m_drag.startSpan = m_t1 - m_t0;
    }

    // ---- 拖动中 ----
    if (m_drag.kind != Drag::None) {
        // Esc 取消任意拖动（要放在"按下"之后判断，否则同一帧的两条路会打架）
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            restoreSnapshot();
            m_drag = DragState{};
        } else if (ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
                   ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
            switch (m_drag.kind) {
                case Drag::Head:
                    actSetHeadPx(plotMin, plotMax, mouse.x);
                    break;
                case Drag::Keys: {
                    const float dt = (mouse.x - m_drag.start.x) / std::max(pps, 1.0e-6f);
                    if (std::fabs(dt) > 1.0e-6f && actMoveSelected(dt)) {
                        m_drag.start.x = mouse.x;
                        m_drag.moved = true;
                    }
                    break;
                }
                case Drag::Marquee:
                    if (std::fabs(mouse.x - m_drag.start.x) > 2.0f ||
                        std::fabs(mouse.y - m_drag.start.y) > 2.0f)
                        m_drag.moved = true;
                    actSelectRect(plotMin, plotMax, m_drag.start, mouse, io.KeyShift);
                    break;
                case Drag::Pan: {
                    // ⚠ 用 startSpan 而不是 `m_t1 - m_t0`：后者在拖动中
                    //   已经被改过了，逐帧用它会把窗口越拖越窄。
                    const float dtt =
                        (m_drag.start.x - mouse.x) / std::max(pps, 1.0e-6f);
                    m_t0 = m_drag.startT0 + dtt;
                    m_t1 = m_t0 + m_drag.startSpan;
                    m_scrollY = m_drag.startScroll - (mouse.y - m_drag.start.y);
                    break;
                }
                default: break;
            }
        } else {
            if ((m_drag.kind == Drag::Keys || m_drag.kind == Drag::Marquee) &&
                !m_drag.moved)
                m_haveSnap = false;   // 只是点了一下，没动过就别留快照
            if (m_drag.kind == Drag::Keys) {
                VK_LOG_INFO("DOPE-DRAG-END kind=key n=%d",
                            static_cast<int>(m_sel.size()));
            } else if (m_drag.kind == Drag::Marquee) {
                VK_LOG_INFO("DOPE-DRAG-END kind=marquee n=%d",
                            static_cast<int>(m_sel.size()));
            }
            m_drag = DragState{};
        }
    }

    // ---- 快捷键（窗口聚焦且不在输入框里）----
    const bool typing = io.WantTextInput;
    if (winFocused && !m_grab && !typing) {
        if (ImGui::IsKeyPressed(ImGuiKey_G, false)) {
            beginSnapshot();
            m_grab = true;
            m_grabStart = mouse;
            m_grabApplied = 0.0f;
            VK_LOG_INFO("DOPE-GRAB-BEGIN n=%d", static_cast<int>(m_sel.size()));
        }
        if (ImGui::IsKeyPressed(ImGuiKey_M, false)) actMirrorSelected(head());
        if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) actAlignSelected(clip().duration);
        if (ImGui::IsKeyPressed(ImGuiKey_A, false) && !io.KeyCtrl) actSelectAll();
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) actDeleteSelected();
        if (ImGui::IsKeyPressed(ImGuiKey_1, false))
            actSetSelectedType(animation::KeyType::Extreme);
        if (ImGui::IsKeyPressed(ImGuiKey_2, false))
            actSetSelectedType(animation::KeyType::Breakdown);
        if (ImGui::IsKeyPressed(ImGuiKey_3, false))
            actSetSelectedType(animation::KeyType::Inbetween);
        if (ImGui::IsKeyPressed(ImGuiKey_0, false))
            actSetSelectedType(animation::KeyType::Key);
    }

    // ---- 画 ----
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 colBg = ImGui::GetColorU32(ImGuiCol_ChildBg, 0.35f);
    const ImU32 colBand = ImGui::GetColorU32(ImGuiCol_Text, 0.04f);
    const ImU32 colGrid = ImGui::GetColorU32(ImGuiCol_Text, 0.09f);
    const ImU32 colDim = ImGui::GetColorU32(ImGuiCol_Text, 0.55f);
    const ImU32 colText = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 colHead = IM_COL32(255, 255, 255, 120);
    const ImU32 colRuler = ImGui::GetColorU32(ImGuiCol_Text, 0.10f);

    dl->AddRectFilled(plotMin, plotMax, colBg);
    // 树区底色单独给一点，让"左栏 / 网格"的分界一眼可见
    dl->AddRectFilled(ImVec2(itemMin.x, plotMin.y),
                      ImVec2(plotMin.x - 1.0f, plotMax.y), colBg);
    // 标尺条
    dl->AddRectFilled(ImVec2(plotMin.x, itemMin.y),
                      ImVec2(plotMax.x, plotMin.y), colRuler);

    // 时间刻度：先挑一个"像素间距 ≥ 56"的步长，再按它画线 + 标签。
    // ⚠ 标签画在**标尺条里**（plotMin.y 之上），不画在网格第一行上 ——
    //   叠在第一行时数字正好压住那一行的菱形，读时间读不出、读帧也读不清。
    //   所以这一段在 PushClipRect **之前**，坐标也用 itemMin.y。
    float tickStep = 0.05f;
    while (pps * tickStep < 56.0f) tickStep *= 2.0f;

    dl->PushClipRect(plotMin, plotMax, true);

    // 行底色（奇偶交替）+ 行分隔
    for (int i = 0; i < static_cast<int>(m_rows.size()); ++i) {
        const float y = plotMin.y + static_cast<float>(i) * m_rowH - m_scrollY;
        if (y + m_rowH < plotMin.y || y > plotMax.y) continue;
        if ((i & 1) == 0)
            dl->AddRectFilled(ImVec2(plotMin.x, y), ImVec2(plotMax.x, y + m_rowH), colBand);
        dl->AddLine(ImVec2(plotMin.x, y), ImVec2(plotMax.x, y), colGrid);
    }

    // 竖直网格线（与标尺同一套步长 —— 线和数字必须对得上）
    {
        const float start = std::ceil(m_t0 / tickStep) * tickStep;
        for (float t = start; t <= m_t1 + 1.0e-6f; t += tickStep) {
            const float x = plotMin.x + (t - m_t0) * pps;
            dl->AddLine(ImVec2(x, plotMin.y), ImVec2(x, plotMax.y), colGrid);
        }
    }

    // 关键帧菱形
    for (const Hit& h : m_hits) {
        const bool hot = (hitUnder == &h);
        drawKeyDiamond(dl, h.pos, h.type, h.sel, hot);
    }

    // 框选矩形
    if (m_drag.kind == Drag::Marquee) {
        dl->AddRectFilled(m_drag.start, mouse, IM_COL32(120, 190, 255, 40));
        dl->AddRect(m_drag.start, mouse, IM_COL32(120, 190, 255, 180));
    }

    // 播放头
    {
        const float x = plotMin.x + (head() - m_t0) * pps;
        if (x >= plotMin.x && x <= plotMax.x) {
            dl->AddLine(ImVec2(x, itemMin.y), ImVec2(x, plotMax.y), colHead, 1.5f);
        }
    }
    dl->PopClipRect();

    // 标尺条上的时间标签 + 三角光标（画在 clip 之外，压不到任何行）
    {
        dl->PushClipRect(ImVec2(plotMin.x, itemMin.y),
                         ImVec2(plotMax.x, plotMin.y), true);
        const float start = std::ceil(m_t0 / tickStep) * tickStep;
        for (float t = start; t <= m_t1 + 1.0e-6f; t += tickStep) {
            const float x = plotMin.x + (t - m_t0) * pps;
            char lbl[32];
            std::snprintf(lbl, sizeof(lbl), "%.2fs", static_cast<double>(t));
            dl->AddText(ImVec2(x + 2.0f, itemMin.y + 2.0f), colDim, lbl);
        }
        const float hx = plotMin.x + (head() - m_t0) * pps;
        if (hx >= plotMin.x && hx <= plotMax.x) {
            // 播放头在标尺上留一个小三角，不然它落到行间就找不着了
            dl->AddTriangleFilled(ImVec2(hx - 4.0f, plotMin.y - 6.0f),
                                  ImVec2(hx + 4.0f, plotMin.y - 6.0f),
                                  ImVec2(hx, plotMin.y - 1.0f), colHead);
        }
        dl->PopClipRect();
    }

    // ---- 左栏通道树（在同一个 draw list 上，clip 到树区）----
    dl->PushClipRect(ImVec2(itemMin.x, plotMin.y), ImVec2(plotMin.x - 1.0f, plotMax.y), true);
    for (int i = 0; i < static_cast<int>(m_rows.size()); ++i) {
        const float y = plotMin.y + static_cast<float>(i) * m_rowH - m_scrollY;
        if (y + m_rowH < plotMin.y || y > plotMax.y) continue;
        const Row& r = m_rows[i];
        const float indent = 6.0f + static_cast<float>(r.depth) * 12.0f;
        float x = itemMin.x + indent;

        if (r.path < 0) {
            // 折叠三角
            const bool col = isCollapsed(r.joint);
            const ImVec2 c(x + 4.0f, y + m_rowH * 0.5f);
            if (col) {
                dl->AddTriangleFilled(ImVec2(c.x - 3.0f, c.y - 4.0f),
                                      ImVec2(c.x + 3.0f, c.y - 4.0f),
                                      ImVec2(c.x, c.y + 4.0f), colDim);
            } else {
                dl->AddTriangleFilled(ImVec2(c.x - 4.0f, c.y - 3.0f),
                                      ImVec2(c.x - 4.0f, c.y + 3.0f),
                                      ImVec2(c.x + 4.0f, c.y), colDim);
            }
            x += 14.0f;
            dl->AddText(ImVec2(x, y + 1.0f), colText, r.joint.c_str());
            // 折叠时右边补一个总帧数（"这一根骨一共几个帧"是折叠态唯一的信息）。
            // 右对齐到树区边界：写死偏移在宽窗口下会飘到列中间，看着像别人的数。
            if (col) {
                int n = 0;
                for (const auto& tr : clip().tracks) {
                    if (tr.joint == r.joint) n += tr.keyCount();
                }
                char b[32];
                std::snprintf(b, sizeof(b), "(%d)", n);
                const float tw = ImGui::CalcTextSize(b).x;
                dl->AddText(ImVec2(plotMin.x - tw - 6.0f, y + 1.0f), colDim, b);
            }
        } else {
            const auto p = static_cast<assets::AnimTrack::Path>(r.path);
            x += 14.0f;
            dl->AddText(ImVec2(x, y + 1.0f), colDim, pathShort(p));
            int n = 0;
            if (r.track >= 0 && r.track < static_cast<int>(clip().tracks.size()))
                n = clip().tracks[static_cast<std::size_t>(r.track)].keyCount();
            char b[32];
            std::snprintf(b, sizeof(b), "%d", n);
            const float tw = ImGui::CalcTextSize(b).x;
            dl->AddText(ImVec2(plotMin.x - tw - 8.0f, y + 1.0f), colDim, b);
        }
    }
    dl->PopClipRect();

    if (m_rows.empty()) {
        dl->AddText(ImVec2(plotMin.x + 12.0f, plotMin.y + 12.0f), colDim,
                    TR("No keys yet — press I on a bone in Pose Mode."));
    }
}

void DopeSheetWindow::drawKeyDiamond(ImDrawList* dl, const ImVec2& p,
                                     animation::KeyType t, bool sel,
                                     bool hot) const {
    // 形状编码"类型"、颜色编码"悬停"、外圈编码"选中" —— 三者互不抢占。
    // 早一版把"选中"也画成换色，结果选中之后类型就看不出来了（摄影表上
    // 最该读的恰恰是类型）。
    const ImU32 col = hot ? kColHot
                          : (t == animation::KeyType::Extreme ? kColExtreme
                             : t == animation::KeyType::Breakdown ? kColBreak
                             : t == animation::KeyType::Inbetween ? kColInb
                                                                  : kColKey);
    switch (t) {
        case animation::KeyType::Extreme: {
            // 实心大菱形 + 外圈：一屏里"转折点"要能第一眼跳出来
            const float r = 6.0f;
            dl->AddQuadFilled(ImVec2(p.x, p.y - r), ImVec2(p.x + r, p.y),
                              ImVec2(p.x, p.y + r), ImVec2(p.x - r, p.y), col);
            dl->AddQuad(ImVec2(p.x, p.y - r - 2.0f), ImVec2(p.x + r + 2.0f, p.y),
                        ImVec2(p.x, p.y + r + 2.0f), ImVec2(p.x - r - 2.0f, p.y),
                        col, 1.0f);
            break;
        }
        case animation::KeyType::Breakdown: {
            // 空心：过渡帧不该抢注意力
            const float r = 5.0f;
            dl->AddQuad(ImVec2(p.x, p.y - r), ImVec2(p.x + r, p.y),
                        ImVec2(p.x, p.y + r), ImVec2(p.x - r, p.y), col, 1.8f);
            break;
        }
        case animation::KeyType::Inbetween: {
            // 小圆点：最弱的一档
            dl->AddCircleFilled(p, 3.0f, col, 10);
            break;
        }
        case animation::KeyType::Key:
        default: {
            const float r = 4.0f;
            dl->AddQuadFilled(ImVec2(p.x, p.y - r), ImVec2(p.x + r, p.y),
                              ImVec2(p.x, p.y + r), ImVec2(p.x - r, p.y), col);
            break;
        }
    }
    if (sel) dl->AddCircle(p, 8.5f, kColSel, 16, 1.3f);
}

// ---------------------------------------------------------------- 绘制：底栏

void DopeSheetWindow::drawFooter() {
    ImGui::Separator();
    const animation::TypeCount tc = animation::countTypes(clip(), m_sel);
    float lo = 0.0f, hi = 0.0f;
    const bool hasRange =
        !m_sel.empty() && animation::selectedTimeRange(clip(), m_sel, lo, hi);

    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s %d", TR("sel"), static_cast<int>(m_sel.size()));
    ImGui::SameLine();
    if (hasRange) {
        ImGui::Text("t=[%.4f, %.4f]", static_cast<double>(lo), static_cast<double>(hi));
        ImGui::SameLine();
        ImGui::TextDisabled("%s %.4f", TR("span"), static_cast<double>(hi - lo));
    } else {
        ImGui::TextDisabled("%s", TR("(nothing selected)"));
    }

    ImGui::SameLine();
    ImGui::TextDisabled("%s %d/%d/%d/%d", TR("types"), tc.key, tc.extreme,
                        tc.breakdown, tc.inbetween);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", TR("key / extreme / breakdown / inbetween. "
                                   "These are markers only — they never change "
                                   "the evaluated pose."));
    }

    // 类型下拉
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130.0f);
    if (ImGui::BeginCombo("##dope_type", TR("key type"))) {
        const animation::KeyType kAll[4] = {
            animation::KeyType::Extreme, animation::KeyType::Breakdown,
            animation::KeyType::Inbetween, animation::KeyType::Key};
        for (auto t : kAll) {
            if (ImGui::Selectable(animation::keyTypeName(t))) {
                actSetSelectedType(t);
            }
        }
        ImGui::EndCombo();
    }
    logRect("DOPE-TYPE", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    ImGui::SameLine();
    ImGui::BeginDisabled(m_sel.empty());
    if (ImGui::Button(TR("Mirror"))) actMirrorSelected(head());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", TR("Reflect the selected keys in TIME around the "
                                   "playhead: t' = 2·pivot − t."));
    logRect("DOPE-MIRROR", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::Button(TR("Align cycle"))) actAlignSelected(clip().duration);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", TR("Scale the selected keys in time so the last "
                                   "one lands exactly on the clip duration. The "
                                   "first selected key stays put (phase anchor)."));
    logRect("DOPE-ALIGN", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::Button(TR("Delete"))) actDeleteSelected();
    logRect("DOPE-DEL", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::TextDisabled("%s", TR("G grab · M mirror · Y align · 1/2/3 type"));
}

// ---------------------------------------------------------------- 注入脚本

void DopeSheetWindow::runInjectedScript() {
    if (m_injected || !m_loaded) return;
    const char* raw = std::getenv("MYVK_EDITOR_DOPE_SCRIPT");
    if (!raw || !raw[0]) return;
    m_injected = true;

    int applied = 0;
    int failed = 0;
    const std::string all(raw);
    std::size_t pos = 0;
    while (pos <= all.size()) {
        const std::size_t semi = all.find(';', pos);
        std::string tok =
            all.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
        pos = (semi == std::string::npos) ? all.size() + 1 : semi + 1;
        const std::size_t b = tok.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) continue;
        tok = tok.substr(b);
        while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t')) tok.pop_back();
        if (tok.empty()) continue;

        const std::size_t colon = tok.find(':');
        const std::string name = tok.substr(0, colon);
        const std::string arg =
            (colon == std::string::npos) ? std::string() : tok.substr(colon + 1);

        bool ok = false;
        if (name == "selall") {
            ok = actSelectAll();
        } else if (name == "selclear") {
            ok = actClearSelection();
        } else if (name == "selrect") {
            // `selrect:<t0>:<t1>` —— 按**时间区间**选，不按像素矩形。
            // 像素矩形要脚本知道窗口多大，而浮动窗口尺寸会被 imgui.ini
            // 记住 → 同一份脚本在不同机器上圈到的帧数不同（CurveEditor 的
            // `boxsel` 用的是比例坐标，同一个理由）。
            float a = 0.0f, b2 = 0.0f;
            if (std::sscanf(arg.c_str(), "%f:%f", &a, &b2) == 2) {
                if (a > b2) std::swap(a, b2);
                std::vector<animation::KeyRef> next;
                for (std::size_t ti = 0; ti < clip().tracks.size(); ++ti) {
                    const auto& tr = clip().tracks[ti];
                    for (int k = 0; k < tr.keyCount(); ++k) {
                        const float t = tr.times[static_cast<std::size_t>(k)];
                        if (t >= a - animation::kKeyTimeEpsilon &&
                            t <= b2 + animation::kKeyTimeEpsilon)
                            next.push_back(animation::KeyRef{
                                static_cast<int>(ti), k});
                    }
                }
                setSelection(std::move(next));
                ok = !m_sel.empty();
            }
        } else if (name == "selrow") {
            // `selrow:<joint>` 选中某根骨的全部帧。
            // ⚠ 按**骨名**而不是行下标：折叠会换掉行号，而且展开时摘要行上
            //   根本没有点（帧摆在下面三行里）—— 一条命令的语义不该随 UI
            //   状态漂移，否则"同一份脚本跑出两种结果"。
            std::vector<animation::KeyRef> next;
            for (std::size_t ti = 0; ti < clip().tracks.size(); ++ti) {
                if (clip().tracks[ti].joint != arg) continue;
                const int n = clip().tracks[ti].keyCount();
                for (int k = 0; k < n; ++k)
                    next.push_back(animation::KeyRef{static_cast<int>(ti), k});
            }
            setSelection(std::move(next));
            ok = !m_sel.empty();
        } else if (name == "move") {
            ok = actMoveSelected(static_cast<float>(std::atof(arg.c_str())));
        } else if (name == "mirror") {
            ok = actMirrorSelected(static_cast<float>(std::atof(arg.c_str())));
        } else if (name == "align") {
            ok = actAlignSelected(static_cast<float>(std::atof(arg.c_str())));
        } else if (name == "type") {
            animation::KeyType t = animation::KeyType::Key;
            if (animation::parseKeyType(arg, t)) ok = actSetSelectedType(t);
        } else if (name == "head") {
            setHead(static_cast<float>(std::atof(arg.c_str())));
            VK_LOG_INFO("DOPE-HEAD t=%.4f", static_cast<double>(head()));
            ok = true;
        } else if (name == "zoom") {
            ok = actZoom(static_cast<float>(std::atof(arg.c_str())));
        } else if (name == "pan") {
            ok = actPan(static_cast<float>(std::atof(arg.c_str())));
        } else if (name == "fit") {
            fitView();
            ok = true;
        } else if (name == "delete") {
            ok = actDeleteSelected();
        } else if (name == "collapse") {
            toggleCollapsed(arg);
            ok = true;
        } else if (name == "dumpkeys") {
            // 逐帧一行（含像素位置）—— 脚本靠它判"缩放/平移之后
            // 每颗菱形的 px 与 px(t) 的推导值一致"。
            // `%a`：与 R7 的 `POSE-DUMP` 同一口径，不用十进制掩盖 1 ulp。
            //
            // ⚠ 先打一行**视图头**：px 是 `m_lastT0` 那一刻的换算率算出来的
            //   （本帧刚画完的那张图），而脚本里排在前面的 `zoom` / `pan` 已经
            //   把 `m_t0` 改了 —— 不带上 t0/pps，验收只能去猜"这一批 px 对应
            //   哪一帧的视图"，猜错的表现是"px 差了几十像素"这种假失败。
            VK_LOG_INFO("DOPE-DUMP n=%d px0=%.1f px1=%.1f t0=%.5f t1=%.5f pps=%.4f",
                        clip().totalKeys(), static_cast<double>(m_lastPlotMin.x),
                        static_cast<double>(m_lastPlotMax.x),
                        static_cast<double>(m_lastT0),
                        static_cast<double>(m_lastT0 +
                                            (m_lastPlotMax.x - m_lastPlotMin.x) /
                                                std::max(m_lastPps, 1.0e-6f)),
                        static_cast<double>(m_lastPps));
            for (std::size_t ti = 0; ti < clip().tracks.size(); ++ti) {
                const auto& tr = clip().tracks[ti];
                for (int k = 0; k < tr.keyCount(); ++k) {
                    const float t = tr.times[static_cast<std::size_t>(k)];
                    bool sel = false;
                    for (const auto& r : m_sel) {
                        if (r.track == static_cast<int>(ti) && r.key == k) {
                            sel = true;
                            break;
                        }
                    }
                    VK_LOG_INFO("DOPE-KEY track=%d joint='%s' path=%s i=%d t=%a "
                                "type=%s sel=%d px=%.3f",
                                static_cast<int>(ti), tr.joint.c_str(),
                                pathLong(tr.path), k, static_cast<double>(t),
                                animation::keyTypeName(tr.typeAt(k)), sel ? 1 : 0,
                                static_cast<double>(m_lastPlotMin.x +
                                                    (t - m_lastT0) * m_lastPps));
                }
            }
            ok = true;
        } else if (name == "save") {
            ok = save();
        } else if (name == "reload") {
            reload();
            ok = m_error.empty();
        } else {
            VK_LOG_WARN("DOPE-SCRIPT-BAD %s", tok.c_str());
        }

        if (ok) ++applied;
        else {
            ++failed;
            VK_LOG_WARN("DOPE-SCRIPT-FAIL %s", tok.c_str());
        }
    }

    VK_LOG_INFO("DOPE-SCRIPT applied=%d failed=%d keys=%d sel=%d", applied, failed,
                clip().totalKeys(), static_cast<int>(m_sel.size()));
}

} // namespace editor
