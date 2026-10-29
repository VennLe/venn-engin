// ============================================================
// editor/BoneCurvePanel.cpp —— 骨骼曲线编辑器（S-D）
//
// 结构：工具栏（路径 / Reload / Save / Fit / 通道 / 光标）
//      → 主体（左：通道列表；右：画布，全自绘）
//      → 侧栏（选中那一帧的 t / value / interp / ease / shape / 手柄）
//      → 校验栏（画布 ≡ 运行时 + 跳过样本数）
//
// 所有**改数据**的入口都是 `act*` 那一族 —— 鼠标手势与注入脚本共用它们。
// 每次改完都立刻 `applyToTrack()` 写回 `m_clip`，所以"存盘"与"视口里
// 看到的样子"不可能分叉：只有一个真值，就是 `m_clip`。
//
// ⚠ 日志前缀 `BONEC-`。
// ============================================================

#include "BoneCurvePanel.h"

#include "DebugRects.h"
#include "EditorContext.h"
#include "Localization.h"
#include "PanelDock.h"

#include "assets/AssetPath.h"
#include "core/Logger.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <system_error>
#include <thread>

namespace editor {

namespace {

constexpr float kKeyHitPx = 7.0f;
constexpr float kHandleHitPx = 6.0f;

const ImVec4 kOkCol(0.45f, 0.85f, 0.52f, 1.0f);
const ImVec4 kBadCol(1.00f, 0.45f, 0.35f, 1.0f);
const ImVec4 kWarnCol(1.00f, 0.75f, 0.30f, 1.0f);

// `assets/characters/walk.anim.json` → `walk`
std::string clipNameFromPath(const std::string& p) {
    if (p.empty()) return "untitled";
    std::string s = p;
    const std::size_t slash = s.find_last_of("/\\");
    if (slash != std::string::npos) s = s.substr(slash + 1);
    for (const char* suf : {".anim.json", ".animclip.json", ".json"}) {
        const std::string sf(suf);
        if (s.size() > sf.size() &&
            s.compare(s.size() - sf.size(), sf.size(), sf) == 0) {
            s = s.substr(0, s.size() - sf.size());
            break;
        }
    }
    return s.empty() ? std::string("untitled") : s;
}

// 画布用的排序副本（约定 ③：拖动中 `m_curve` 允许乱序）。
//
// ⚠ `evaluateCurve` **要求** keys 按时间升序（它用二分找段）。拿乱序的
//   数组去求值不会报错，会画出一条"来回折"的线，而数值上看不出问题。
animation::Curve sortedForDraw(const animation::Curve& c) {
    animation::Curve out = c;
    std::stable_sort(out.keys.begin(), out.keys.end(),
                     [](const animation::CurveKey& a, const animation::CurveKey& b) {
                         return a.time < b.time;
                     });
    return out;
}

} // namespace

BoneCurvePanel::BoneCurvePanel(EditorContext& ctx) : m_ctx(ctx) {}

// ============================================================
// 打开 / 重读 / 磁盘时间戳
// ============================================================

bool BoneCurvePanel::refreshStamp() {
    std::error_code ec;
    const std::string full = assets::resolveAssetPath(m_path);
    const auto t = std::filesystem::last_write_time(std::filesystem::path(full), ec);
    if (ec) {
        m_haveStamp = false;
        return false;
    }
    m_stampDisk = t;
    m_haveStamp = true;
    return true;
}

bool BoneCurvePanel::open(const std::string& relPath) {
    m_open = true;
    m_path = relPath;
    m_sel.clear();
    m_primary = -1;
    m_error.clear();
    m_uiError.clear();
    m_dirty = false;
    m_injected = false;
    m_drag = DragState{};
    m_chan = 0;
    m_chanStamp.clear();
    m_curveStamp.clear();
    m_mapKey.clear();
    m_keyLogKey.clear();
    m_checkLogKey.clear();
    m_curve = animation::Curve{};
    m_chans.clear();
    m_head = 0.0f;
    m_loaded = true;
    std::snprintf(m_pathBuf, sizeof(m_pathBuf), "%s", m_path.c_str());

    if (relPath.empty()) {
        m_clip = animation::EditClip{};
        m_readOnly = false;
        m_haveStamp = false;
        VK_LOG_INFO("BONEC-OPEN path=(none) ok=0 tracks=0 keys=0 dur=0.000 readOnly=0");
        rebuildChannelList();
        rebuildCurve();      // ⚠ 先建投影再 resetView：fitValueRange 读 m_curve
        resetView();
        revalidate();
        return false;
    }

    animation::EditClip c;
    std::string err;
    const bool ok = animation::loadEditClip(assets::resolveAssetPath(relPath), c, err);
    m_clip = std::move(c);
    // 读盘失败 → 只读（同 CurveEditorPanel ⑤）：一份半截的 tracks 允许
    // 存盘 = 把 40 帧的动作截成 3 帧。
    m_readOnly = !ok;
    if (!ok) {
        m_error = err;
        m_clip = animation::EditClip{};
    }
    refreshStamp();
    rebuildChannelList();

    VK_LOG_INFO("BONEC-OPEN path=%s ok=%d tracks=%d keys=%d dur=%.3f chans=%d "
                "readOnly=%d",
                relPath.c_str(), ok ? 1 : 0,
                static_cast<int>(m_clip.tracks.size()), m_clip.totalKeys(),
                static_cast<double>(m_clip.duration),
                static_cast<int>(m_chans.size()), m_readOnly ? 1 : 0);
    if (!m_chans.empty()) {
        m_sel.push_back(0);
        m_primary = 0;
    }
    rebuildCurve();
    resetView();
    revalidate();
    return ok;
}

void BoneCurvePanel::close() {
    if (m_open && m_dirty) {
        VK_LOG_WARN("BONEC-CLOSE-DIRTY path=%s（有未存的改动）", m_path.c_str());
    }
    m_open = false;
    m_drag = DragState{};
}

void BoneCurvePanel::reload() {
    m_drag = DragState{};
    m_dirty = false;
    m_uiError.clear();
    m_curveStamp.clear();
    m_chanStamp.clear();
    m_keyLogKey.clear();
    m_mapKey.clear();
    m_checkLogKey.clear();
    m_sel.clear();
    m_primary = -1;

    if (m_path.empty()) {
        m_clip = animation::EditClip{};
        m_readOnly = false;
        m_error.clear();
        rebuildChannelList();
        rebuildCurve();
        revalidate();
        resetView();
        return;
    }

    animation::EditClip c;
    std::string err;
    const bool ok = animation::loadEditClip(assets::resolveAssetPath(m_path), c, err);
    m_clip = ok ? std::move(c) : animation::EditClip{};
    m_readOnly = !ok;
    m_error = ok ? std::string() : err;
    refreshStamp();
    rebuildChannelList();
    if (!m_chans.empty()) {
        if (m_chan >= static_cast<int>(m_chans.size())) m_chan = 0;
        m_sel.push_back(0);
        m_primary = 0;
    }
    rebuildCurve();
    resetView();
    VK_LOG_INFO("BONEC-RELOAD path=%s ok=%d tracks=%d readOnly=%d", m_path.c_str(),
                ok ? 1 : 0, static_cast<int>(m_clip.tracks.size()),
                m_readOnly ? 1 : 0);
    revalidate();
}

// ============================================================
// 通道与轨道
// ============================================================

void BoneCurvePanel::rebuildChannelList() {
    // 指纹只认"有哪些轨道、各自几个键、什么路径"—— 改值/改缓动不该重建
    // 列表（那会让列表在拖动中抖动，也会把 `m_chan` 的指向搞乱）。
    char buf[96];
    std::string st;
    st.reserve(m_clip.tracks.size() * 64 + 32);
    for (const animation::EditTrack& t : m_clip.tracks) {
        std::snprintf(buf, sizeof(buf), "%s|%d|%d;", t.joint.c_str(),
                      static_cast<int>(t.path), t.keyCount());
        st += buf;
    }
    if (st == m_chanStamp) return;
    m_chanStamp = st;
    m_chans = animation::listBoneChannels(m_clip);
    if (m_chans.empty()) {
        m_chan = 0;
    } else if (m_chan >= static_cast<int>(m_chans.size())) {
        m_chan = static_cast<int>(m_chans.size()) - 1;
    }
    m_curveStamp.clear();
}

bool BoneCurvePanel::haveChannel() const {
    return m_chan >= 0 && m_chan < static_cast<int>(m_chans.size());
}

animation::BoneChannel BoneCurvePanel::channel() const {
    if (!haveChannel()) return animation::BoneChannel{};
    return m_chans[static_cast<std::size_t>(m_chan)];
}

const animation::EditTrack* BoneCurvePanel::trackC() const {
    const animation::BoneChannel ch = channel();
    if (ch.track < 0 || ch.track >= static_cast<int>(m_clip.tracks.size()))
        return nullptr;
    return &m_clip.tracks[static_cast<std::size_t>(ch.track)];
}

animation::EditTrack* BoneCurvePanel::trackM() {
    const animation::BoneChannel ch = channel();
    if (ch.track < 0 || ch.track >= static_cast<int>(m_clip.tracks.size()))
        return nullptr;
    return &m_clip.tracks[static_cast<std::size_t>(ch.track)];
}

// ============================================================
// 投影（轨道 → 曲线）
// ============================================================

std::string BoneCurvePanel::curveStampOf() const {
    const animation::EditTrack* tr = trackC();
    if (!tr) return "none";
    char buf[160];
    std::string s;
    s.reserve(tr->times.size() * 96 + 32);
    const int comp = channel().comp;
    // ⚠ 指纹串刻意**不带标签**（`ch=`/`comp=` 那种）。它只用来比对"变了没有"，
    //   永远不会显示给人看；写成带标签的样子会被 i18n 体检当成界面格式串
    //   要求包 TR() —— 那是误报，但把标签去掉更省事，也更快。
    std::snprintf(buf, sizeof(buf), "%d|%d|%d|%d|", m_chan, comp,
                  tr->step ? 1 : 0, tr->keyCount());
    s += buf;
    for (int i = 0; i < tr->keyCount(); ++i) {
        const animation::KeyEase& e = tr->easeAt(i);
        std::snprintf(buf, sizeof(buf),
                      "%.6f,%.7g,%d,%d,%d,%d,%.5f,%.5f,%.5f,%.5f;",
                      static_cast<double>(tr->times[static_cast<std::size_t>(i)]),
                      static_cast<double>(
                          tr->values[static_cast<std::size_t>(i)]
                                    [static_cast<glm::length_t>(comp)]),
                      static_cast<int>(e.interp), static_cast<int>(e.ease),
                      static_cast<int>(e.shape), static_cast<int>(e.handle),
                      static_cast<double>(e.outHandle.x),
                      static_cast<double>(e.outHandle.y),
                      static_cast<double>(e.inHandle.x),
                      static_cast<double>(e.inHandle.y));
        s += buf;
    }
    return s;
}

void BoneCurvePanel::rebuildCurve() {
    rebuildChannelList();
    const std::string st = curveStampOf();
    if (st == m_curveStamp) return;
    m_curveStamp = st;
    const animation::EditTrack* tr = trackC();
    if (!tr) {
        m_curve = animation::Curve{};
        return;
    }
    m_curve = animation::bakeTrackCurve(*tr, channel().comp);
}

float BoneCurvePanel::keyValue(int i) const {
    if (i < 0 || i >= static_cast<int>(m_curve.keys.size())) return 0.0f;
    return m_curve.keys[static_cast<std::size_t>(i)].value;
}

bool BoneCurvePanel::setProjectedKey(int i, float t, float v, bool reorder) {
    if (i < 0 || i >= static_cast<int>(m_curve.keys.size()))
        return reject("setkey", TR("no key selected"));
    m_curve.keys[static_cast<std::size_t>(i)].time = std::max(t, 0.0f);
    m_curve.keys[static_cast<std::size_t>(i)].value = v;
    if (reorder) {
        m_primary = i;
        // 约定 ③：松手才重排。这里只把**投影**按时间排好并换算选择集。
        sortAndRemap();
    }
    return applyToTrack("setkey");
}

// 投影 → 轨道。约定 ②：不增删帧（数量不等时 `applyCurveToTrack` 整条拒绝）。
bool BoneCurvePanel::applyToTrack(const char* why) {
    animation::EditTrack* tr = trackM();
    if (!tr) return false;
    const int comp = channel().comp;
    const animation::CurveWriteStats st =
        animation::applyCurveToTrack(*tr, comp, m_curve);
    if (st.skipped > 0) {
        // 不把这一条当"失败"：它可能只是"曲线里少了一帧"这种正在编辑的
        // 中间态。但必须让人看见，否则"改了没反应"就是无解的。
        return reject(why, TR("the curve has a different key count than the "
                              "track — curves only reshape keys, they do not "
                              "add or remove them"));
    }
    markDirty();
    // ⚠ 写回后轨道已经按时间排好，而 `m_curve` 可能还是拖动中的乱序。
    //   把指纹对齐到"写回之后的状态"，下一帧就不会立刻重烘（那会把
    //   `m_sel` 的下标含义当场换掉）。真正的重烘发生在 `sortAndRemap()`。
    m_curveStamp = curveStampOf();
    m_chanStamp.clear();      // 键数可能变了（合并），列表要重算
    return true;
}

float BoneCurvePanel::durationOf() const {
    float d = std::max(m_clip.duration, 0.1f);
    for (const animation::EditTrack& t : m_clip.tracks) {
        if (!t.times.empty()) d = std::max(d, t.times.back());
    }
    return std::max(d, 0.1f);
}

// ============================================================
// 视图
// ============================================================

float BoneCurvePanel::pxOfT(const Plot& p, float t) const {
    const float span = std::max(p.t1 - p.t0, 1.0e-6f);
    return p.min.x + (t - p.t0) / span * (p.max.x - p.min.x);
}
float BoneCurvePanel::pyOfV(const Plot& p, float v) const {
    const float span = std::max(p.v1 - p.v0, 1.0e-9f);
    return p.max.y - (v - p.v0) / span * (p.max.y - p.min.y);
}
float BoneCurvePanel::tOfPx(const Plot& p, float x) const {
    const float span = std::max(p.max.x - p.min.x, 1.0f);
    return p.t0 + (x - p.min.x) / span * (p.t1 - p.t0);
}
float BoneCurvePanel::vOfPy(const Plot& p, float y) const {
    const float span = std::max(p.max.y - p.min.y, 1.0f);
    return p.v1 - (y - p.min.y) / span * (p.v1 - p.v0);
}
float BoneCurvePanel::pxPerSec(const Plot& p) const {
    return std::max(p.max.x - p.min.x, 1.0f) / std::max(p.t1 - p.t0, 1.0e-6f);
}
float BoneCurvePanel::pxPerUnit(const Plot& p) const {
    return std::max(p.max.y - p.min.y, 1.0f) / std::max(p.v1 - p.v0, 1.0e-9f);
}

void BoneCurvePanel::resetView() {
    m_t0 = 0.0f;
    m_t1 = durationOf();
    fitValueRange();
    VK_LOG_INFO("BONEC-VIEW t=[%.3f,%.3f] v=[%.4f,%.4f] ch=%s",
                static_cast<double>(m_t0), static_cast<double>(m_t1),
                static_cast<double>(m_v0), static_cast<double>(m_v1),
                haveChannel() ? animation::boneChannelId(m_clip, channel()).c_str()
                              : "(none)");
}

void BoneCurvePanel::fitValueRange() {
    if (m_curve.keys.empty()) {
        m_v0 = -1.0f;
        m_v1 = 1.0f;
        return;
    }
    float lo = m_curve.keys.front().value;
    float hi = lo;
    for (const animation::CurveKey& k : m_curve.keys) {
        lo = std::min(lo, k.value);
        hi = std::max(hi, k.value);
    }
    // 全平的一条线：给一个固定的窗口，否则 span=0 会让 pyOfV 除零
    // （表现是"点全堆在中线上"）。
    if (hi - lo < 1.0e-4f) {
        const float c = 0.5f * (hi + lo);
        lo = c - 0.5f;
        hi = c + 0.5f;
    } else {
        const float pad = 0.12f * (hi - lo);
        lo -= pad;
        hi += pad;
    }
    m_v0 = lo;
    m_v1 = hi;
}

// ============================================================
// 选择
// ============================================================

bool BoneCurvePanel::isSelected(int i) const {
    return std::find(m_sel.begin(), m_sel.end(), i) != m_sel.end();
}

void BoneCurvePanel::clampSel() {
    const int n = static_cast<int>(m_curve.keys.size());
    std::vector<int> keep;
    for (int i : m_sel) {
        if (i >= 0 && i < n) keep.push_back(i);
    }
    std::sort(keep.begin(), keep.end());
    keep.erase(std::unique(keep.begin(), keep.end()), keep.end());
    m_sel.swap(keep);
    if (m_primary >= n) m_primary = n > 0 ? n - 1 : -1;
    if (m_primary < 0 && !m_sel.empty()) m_primary = m_sel.front();
}

bool BoneCurvePanel::actSelect(int i, bool additive) {
    if (i < 0 || i >= static_cast<int>(m_curve.keys.size()))
        return reject("select", TR("no such key"));
    if (additive) {
        const auto it = std::find(m_sel.begin(), m_sel.end(), i);
        if (it != m_sel.end()) {
            m_sel.erase(it);
            if (m_primary == i) m_primary = m_sel.empty() ? -1 : m_sel.back();
            return true;
        }
    } else if (isSelected(i) && m_sel.size() == 1) {
        m_primary = i;
        return true;
    } else {
        m_sel.clear();
    }
    m_sel.push_back(i);
    std::sort(m_sel.begin(), m_sel.end());
    m_primary = i;
    VK_LOG_INFO("BONEC-SEL i=%d n=%d primary=%d ch=%s", i,
                static_cast<int>(m_sel.size()), m_primary,
                animation::boneChannelId(m_clip, channel()).c_str());
    return true;
}

bool BoneCurvePanel::actSelectAll() {
    const int n = static_cast<int>(m_curve.keys.size());
    m_sel.clear();
    for (int i = 0; i < n; ++i) m_sel.push_back(i);
    m_primary = n > 0 ? 0 : -1;
    return n > 0;
}

bool BoneCurvePanel::actSelectRect(const Plot& p, ImVec2 a, ImVec2 b,
                                   bool additive) {
    rebuildCurve();
    const float x0 = std::min(a.x, b.x), x1 = std::max(a.x, b.x);
    const float y0 = std::min(a.y, b.y), y1 = std::max(a.y, b.y);
    if (!additive) m_sel.clear();
    int added = 0;
    for (int i = 0; i < static_cast<int>(m_curve.keys.size()); ++i) {
        const float px = pxOfT(p, m_curve.keys[static_cast<std::size_t>(i)].time);
        const float py = pyOfV(p, keyValue(i));
        if (px < x0 || px > x1 || py < y0 || py > y1) continue;
        if (!isSelected(i)) m_sel.push_back(i);
        ++added;
    }
    std::sort(m_sel.begin(), m_sel.end());
    if (!m_sel.empty() && (m_primary < 0 || !isSelected(m_primary)))
        m_primary = m_sel.front();
    return added > 0;
}

// ============================================================
// 动作：改数据
// ============================================================

bool BoneCurvePanel::reject(const char* op, const std::string& reason) {
    m_uiError = std::string(TR("Rejected")) + ": " + reason;
    VK_LOG_WARN("BONEC-REJECT op=%s reason=%s", op, reason.c_str());
    return false;
}

void BoneCurvePanel::sortAndRemap() {
    const int n = static_cast<int>(m_curve.keys.size());
    if (n < 2) return;
    std::vector<int> idx(static_cast<std::size_t>(n));
    std::iota(idx.begin(), idx.end(), 0);
    std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) {
        return m_curve.keys[static_cast<std::size_t>(a)].time <
               m_curve.keys[static_cast<std::size_t>(b)].time;
    });
    bool changed = false;
    for (int i = 0; i < n; ++i) {
        if (idx[static_cast<std::size_t>(i)] != i) {
            changed = true;
            break;
        }
    }
    if (!changed) return;

    std::vector<animation::CurveKey> nk(static_cast<std::size_t>(n));
    std::vector<int> newOfOld(static_cast<std::size_t>(n), 0);
    for (int i = 0; i < n; ++i) {
        const int old = idx[static_cast<std::size_t>(i)];
        nk[static_cast<std::size_t>(i)] = m_curve.keys[static_cast<std::size_t>(old)];
        newOfOld[static_cast<std::size_t>(old)] = i;
    }
    m_curve.keys.swap(nk);
    for (int& s : m_sel) s = newOfOld[static_cast<std::size_t>(s)];
    std::sort(m_sel.begin(), m_sel.end());
    if (m_primary >= 0) m_primary = newOfOld[static_cast<std::size_t>(m_primary)];
    m_keyLogKey.clear();
    VK_LOG_INFO("BONEC-REORDER n=%d primary=%d ch=%s", n, m_primary,
                animation::boneChannelId(m_clip, channel()).c_str());
}

bool BoneCurvePanel::actSetKey(int i, float t, float v, bool reorder) {
    rebuildCurve();
    if (i < 0 || i >= static_cast<int>(m_curve.keys.size()))
        return reject("setkey", TR("no key selected"));
    t = std::clamp(t, 0.0f, durationOf());
    if (!setProjectedKey(i, t, v, reorder)) return false;
    VK_LOG_INFO("BONEC-SETKEY i=%d t=%.4f v=%.5f ch=%s", i,
                static_cast<double>(m_curve.keys[static_cast<std::size_t>(i)].time),
                static_cast<double>(keyValue(i)),
                animation::boneChannelId(m_clip, channel()).c_str());
    return true;
}

bool BoneCurvePanel::actDragKeysPx(float dx, float dy) {
    if (m_drag.kind != DragKind::Key || m_drag.moving.empty()) return false;
    const float dt = dx / pxPerSec(m_plot);
    const float dv = -dy / pxPerUnit(m_plot);
    bool changed = false;
    for (std::size_t k = 0; k < m_drag.moving.size(); ++k) {
        const int idx = m_drag.moving[k];
        if (idx < 0 || idx >= static_cast<int>(m_curve.keys.size())) continue;
        const float nt = std::clamp(m_drag.origT[k] + dt, 0.0f, durationOf());
        const float nv = m_drag.origV[k] + dv;
        // ⚠ 用"原始值 + 同一个增量"，不是"逐个累加" —— 后者会让相对
        //   间距随每次调用漂移（多选拖动时第一帧看着对、十帧之后全散）。
        animation::CurveKey& ck = m_curve.keys[static_cast<std::size_t>(idx)];
        if (ck.time != nt || ck.value != nv) changed = true;
        ck.time = nt;
        ck.value = nv;
    }
    if (std::fabs(dt) > 1.0e-6f) m_drag.timeMoved = true;
    if (changed) {
        m_drag.moved = true;
        applyToTrack("drag");
    }
    return changed;
}

bool BoneCurvePanel::actSetHandle(int i, bool isOut, glm::vec2 absOffset) {
    if (i < 0 || i >= static_cast<int>(m_curve.keys.size()))
        return reject("sethandle", TR("no key selected"));
    animation::CurveKey& k = m_curve.keys[static_cast<std::size_t>(i)];
    if (isOut) {
        // ⚠ 符号约束（同 Curve.h）：out.x >= 0、in.x <= 0。允许拖到反面
        //   的话这一段在 x 上不再单调，求根会解出两个 u —— 表现是
        //   "曲线在某一刻往回跳了一下"，日志里什么都看不出来。
        k.outHandle.x = std::max(absOffset.x, 0.0f);
        k.outHandle.y = absOffset.y;
    } else {
        k.inHandle.x = std::min(absOffset.x, 0.0f);
        k.inHandle.y = absOffset.y;
    }
    // 拖过的手柄不再"自动"（与 Blender 一致：Auto 不认你拖的值）。
    k.handle = animation::HandleType::Free;
    return applyToTrack("sethandle");
}

bool BoneCurvePanel::actDragHandlePx(float dx, float dy) {
    if (m_drag.kind != DragKind::HandleOut && m_drag.kind != DragKind::HandleIn)
        return false;
    const int i = m_drag.index;
    if (i < 0 || i >= static_cast<int>(m_curve.keys.size())) return false;
    const bool isOut = (m_drag.kind == DragKind::HandleOut);
    glm::vec2 h = m_drag.handleOrig;
    h.x += dx / pxPerSec(m_plot);
    h.y += -dy / pxPerUnit(m_plot);
    const bool ok = actSetHandle(i, isOut, h);
    if (ok) m_drag.moved = true;
    return ok;
}

bool BoneCurvePanel::actSetInterp(int i, animation::Interp ip) {
    if (i < 0 || i >= static_cast<int>(m_curve.keys.size()))
        return reject("setinterp", TR("no key selected"));
    m_curve.keys[static_cast<std::size_t>(i)].interp = ip;
    const bool ok = applyToTrack("setinterp");
    if (ok) {
        VK_LOG_INFO("BONEC-INTERP i=%d interp=%s ch=%s", i,
                    animation::interpName(ip),
                    animation::boneChannelId(m_clip, channel()).c_str());
    }
    return ok;
}

bool BoneCurvePanel::actCycleInterp() {
    if (m_primary < 0) return reject("cycleinterp", TR("no key selected"));
    // 顺序：linear → bezier → constant → linear。
    // 贝塞尔排第二是因为它才是"想捏手感"时要去的那一档；Constant 放最后
    // （它是开关/硬切用的，少有人按着 T 找它）。
    const animation::Interp cur =
        m_curve.keys[static_cast<std::size_t>(m_primary)].interp;
    animation::Interp next = animation::Interp::Linear;
    switch (cur) {
        case animation::Interp::Linear:   next = animation::Interp::Bezier; break;
        case animation::Interp::Bezier:   next = animation::Interp::Constant; break;
        case animation::Interp::Constant: next = animation::Interp::Linear; break;
    }
    return actSetInterp(m_primary, next);
}

bool BoneCurvePanel::actSetEase(int i, animation::EaseMode m,
                                animation::EaseShape s) {
    if (i < 0 || i >= static_cast<int>(m_curve.keys.size()))
        return reject("setease", TR("no key selected"));
    animation::CurveKey& k = m_curve.keys[static_cast<std::size_t>(i)];
    k.ease = m;
    k.shape = s;
    const bool ok = applyToTrack("setease");
    if (ok) {
        VK_LOG_INFO("BONEC-EASE i=%d mode=%s shape=%s ch=%s", i,
                    animation::easeModeName(m), animation::easeShapeName(s),
                    animation::boneChannelId(m_clip, channel()).c_str());
    }
    return ok;
}

// V：**存类型**，不换算数值（约定 ④，与相机面板正好相反）。
// AutoClamped 会一直跟着邻居重算，这才是 Blender 的手感。
bool BoneCurvePanel::actSetHandleType(int i, animation::HandleType t) {
    if (i < 0 || i >= static_cast<int>(m_curve.keys.size()))
        return reject("setht", TR("no key selected"));
    m_curve.keys[static_cast<std::size_t>(i)].handle = t;
    const bool ok = applyToTrack("setht");
    if (ok) {
        VK_LOG_INFO("BONEC-HANDLETYPE i=%d type=%s ch=%s", i,
                    animation::handleTypeName(t),
                    animation::boneChannelId(m_clip, channel()).c_str());
    }
    return ok;
}

bool BoneCurvePanel::actCycleHandleType() {
    if (m_primary < 0) return reject("cycleht", TR("no key selected"));
    // auto → auto_clamped → vector → aligned → free → auto
    // ⚠ 计数器是**成员**：static 会在关窗重开之后接着上次的位置走，
    //   而"我关掉重开想从第一档开始"是作者的真实预期。
    ++m_htCycle;
    static const animation::HandleType kAll[5] = {
        animation::HandleType::Auto, animation::HandleType::AutoClamped,
        animation::HandleType::Vector, animation::HandleType::Aligned,
        animation::HandleType::Free};
    const animation::HandleType t = kAll[((m_htCycle % 5) + 5) % 5];
    return actSetHandleType(m_primary, t);
}

bool BoneCurvePanel::actSetChannel(int index) {
    if (index < 0 || index >= static_cast<int>(m_chans.size()))
        return reject("chan", TR("no such channel"));
    if (index == m_chan) return true;
    m_chan = index;
    m_sel.clear();
    m_primary = -1;
    m_curveStamp.clear();
    m_keyLogKey.clear();
    m_mapKey.clear();
    rebuildCurve();
    if (!m_curve.keys.empty()) {
        m_sel.push_back(0);
        m_primary = 0;
    }
    fitValueRange();
    VK_LOG_INFO("BONEC-CHANNEL i=%d id=%s keys=%d", index,
                animation::boneChannelId(m_clip, channel()).c_str(),
                static_cast<int>(m_curve.keys.size()));
    return true;
}

bool BoneCurvePanel::actSetHead(float t) {
    m_head = std::clamp(t, 0.0f, durationOf());
    // 采样值单独打一行：验收脚本要拿它证明"改了缓动之后同一时刻的值变了"，
    // 而侧栏那行 `v@cursor` 是界面文字，不该被日志正则依赖。
    if (!m_curve.keys.empty()) {
        const animation::Curve draw = sortedForDraw(m_curve);
        VK_LOG_INFO("BONEC-SAMPLE t=%.4f v=%.6f ch=%s",
                    static_cast<double>(m_head),
                    static_cast<double>(animation::evaluateCurve(draw, m_head)),
                    haveChannel() ? animation::boneChannelId(m_clip, channel()).c_str()
                                  : "(none)");
    }
    return true;
}

// ============================================================
// 手势
// ============================================================

void BoneCurvePanel::beginDrag(const Plot& p, ImVec2 mouse, bool additive) {
    rebuildCurve();
    m_drag = DragState{};
    const int n = static_cast<int>(m_curve.keys.size());

    // ---- ① 手柄（先于关键帧：手柄画在上面，命中也要先给它）----
    if (m_primary >= 0 && m_primary < n) {
        const animation::CurveKey& k = m_curve.keys[static_cast<std::size_t>(m_primary)];
        const glm::vec2 outH = animation::resolveOutHandle(m_curve, m_primary);
        const glm::vec2 inH = animation::resolveInHandle(m_curve, m_primary);
        const ImVec2 pOut(pxOfT(p, k.time + outH.x), pyOfV(p, k.value + outH.y));
        const ImVec2 pIn(pxOfT(p, k.time + inH.x), pyOfV(p, k.value + inH.y));
        const auto near = [&](const ImVec2& q) {
            const float dx = mouse.x - q.x, dy = mouse.y - q.y;
            return dx * dx + dy * dy <= kHandleHitPx * kHandleHitPx;
        };
        if (k.interp == animation::Interp::Bezier && near(pOut)) {
            m_drag.kind = DragKind::HandleOut;
            m_drag.index = m_primary;
            m_drag.handleOrig = k.outHandle;
            m_drag.startMouse = mouse;
            return;
        }
        if (k.interp == animation::Interp::Bezier && near(pIn)) {
            m_drag.kind = DragKind::HandleIn;
            m_drag.index = m_primary;
            m_drag.handleOrig = k.inHandle;
            m_drag.startMouse = mouse;
            return;
        }
    }

    // ---- ② 关键帧（命中最近的一个）----
    int hit = -1;
    float best = kKeyHitPx;
    for (int i = 0; i < n; ++i) {
        const ImVec2 q(pxOfT(p, m_curve.keys[static_cast<std::size_t>(i)].time),
                       pyOfV(p, keyValue(i)));
        const float dx = mouse.x - q.x, dy = mouse.y - q.y;
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d <= best) {
            best = d;
            hit = i;
        }
    }
    if (hit >= 0) {
        if (!isSelected(hit)) actSelect(hit, additive);
        else m_primary = hit;
        m_drag.kind = DragKind::Key;
        m_drag.index = hit;
        m_drag.startMouse = mouse;
        // 多选批量拖动：记下每个被移动帧的 (t, v) 原始值
        m_drag.moving = m_sel;
        for (int idx : m_drag.moving) {
            m_drag.origT.push_back(
                m_curve.keys[static_cast<std::size_t>(idx)].time);
            m_drag.origV.push_back(keyValue(idx));
        }
        return;
    }

    // ---- ③ 空白：框选 ----
    m_drag.kind = DragKind::Marquee;
    m_drag.startMouse = mouse;
    if (!additive) m_sel.clear();
}

void BoneCurvePanel::endDrag() {
    if (m_drag.kind == DragKind::None) return;
    const DragKind kind = m_drag.kind;
    const int i = m_drag.index;

    if (kind == DragKind::Key) {
        if (m_drag.timeMoved) {
            // 松手才重排（约定 ③），重排后立刻写回轨道
            sortAndRemap();
            applyToTrack("sort");
        }
        if (m_drag.moved) {
            clampSel();
            rebuildCurve();
            m_keyLogKey.clear();
            VK_LOG_INFO("BONEC-DRAG-END kind=key i=%d t=%.4f v=%.5f n=%d ch=%s",
                        m_primary,
                        static_cast<double>(m_primary >= 0
                                                ? m_curve.keys[static_cast<std::size_t>(
                                                      m_primary)].time
                                                : 0.0f),
                        static_cast<double>(keyValue(m_primary)),
                        static_cast<int>(m_drag.moving.size()),
                        animation::boneChannelId(m_clip, channel()).c_str());
        }
    } else if (kind == DragKind::HandleOut || kind == DragKind::HandleIn) {
        if (m_drag.moved && i >= 0 && i < static_cast<int>(m_curve.keys.size())) {
            const animation::CurveKey& k = m_curve.keys[static_cast<std::size_t>(i)];
            const glm::vec2 h = (kind == DragKind::HandleOut) ? k.outHandle : k.inHandle;
            VK_LOG_INFO("BONEC-DRAG-END kind=handle i=%d side=%s x=%.4f y=%.4f ch=%s",
                        i, kind == DragKind::HandleOut ? "out" : "in",
                        static_cast<double>(h.x), static_cast<double>(h.y),
                        animation::boneChannelId(m_clip, channel()).c_str());
        }
    } else if (kind == DragKind::Marquee) {
        const float dx = ImGui::GetIO().MousePos.x - m_drag.startMouse.x;
        const float dy = ImGui::GetIO().MousePos.y - m_drag.startMouse.y;
        if (std::fabs(dx) >= 3.0f || std::fabs(dy) >= 3.0f) {
            actSelectRect(m_plot, m_drag.startMouse, ImGui::GetIO().MousePos, true);
        }
        VK_LOG_INFO("BONEC-DRAG-END kind=marquee n=%d primary=%d",
                    static_cast<int>(m_sel.size()), m_primary);
    }

    if (m_drag.moved) revalidate();
    m_drag = DragState{};
}

// ============================================================
// 校验 / 存盘
// ============================================================

bool BoneCurvePanel::segmentUsesSlerp(const animation::EditTrack& tr,
                                      float t) const {
    if (tr.path != assets::AnimTrack::Rotation) return false;
    if (tr.keyCount() < 2) return false;
    if (t <= tr.times.front() || t >= tr.times.back()) return false;
    const auto it = std::upper_bound(tr.times.begin(), tr.times.end(), t);
    const int i1 = static_cast<int>(it - tr.times.begin());
    const int i0 = i1 - 1;
    if (i0 < 0 || i1 >= tr.keyCount()) return false;
    const animation::KeyEase& e = tr.easeAt(i0);
    // Linear + 恒等缓动 = 运行时那条路是 slerp（约定 ⑤）。
    return e.interp == animation::Interp::Linear &&
           e.ease == animation::EaseMode::None &&
           e.shape == animation::EaseShape::Linear;
}

BoneCurvePanel::XCheck BoneCurvePanel::crossCheck() const {
    XCheck xc;
    const animation::EditTrack* tr = trackC();
    if (!tr || tr->keyCount() < 2 || m_curve.keys.empty()) return xc;
    const animation::Curve draw = sortedForDraw(m_curve);
    const int comp = channel().comp;
    const float dur = std::max(m_clip.duration, tr->times.back());
    for (int s = 0; s <= 64; ++s) {
        const float t = dur * static_cast<float>(s) / 64.0f;
        if (segmentUsesSlerp(*tr, t)) {
            ++xc.skipped;   // 约定 ⑤：这一段运行时走 slerp，不是同一条线
            continue;
        }
        const float ref = animation::evaluateCurve(draw, t);
        const glm::vec4 run = animation::sampleEditTrack(*tr, t);
        const float d = std::fabs(ref - run[static_cast<glm::length_t>(comp)]);
        ++xc.samples;
        if (d > xc.worst) {
            xc.worst = d;
            xc.where = animation::pathComponentName(tr->path, comp);
        }
    }
    return xc;
}

void BoneCurvePanel::revalidate() {
    // ⚠ 必须先重建投影：`open()` / `reload()` 都会直接调到这里，而
    //   `m_curve` 只在 `drawCanvas` 里被 `rebuildCurve()` 填过。不先建，
    //   第一帧的 `xcheck` 是 `samples=0`（看着像"没有偏差"，其实是
    //   "什么都没比"）—— 这种"绿灯挂在没跑上"的读数比红灯危险。
    rebuildCurve();
    // 存盘往返：把 m_clip 落成文本再读回来，校验结构还能不能打开。
    bool parsed = false;
    std::string err;
    {
        const std::string path =
            (std::filesystem::temp_directory_path() /
             "venn_bonecurve_panel_check.anim.json").string();
        animation::EditClip back;
        if (animation::saveEditClip(path, m_clip, err) &&
            animation::loadEditClip(path, back, err)) {
            parsed = true;
        }
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    const XCheck xc = crossCheck();

    char key[192];
    std::snprintf(key, sizeof(key), "%d|%d|%d|%d|%d|%.6g|%s", parsed ? 1 : 0,
                  m_clip.totalKeys(), static_cast<int>(m_clip.tracks.size()),
                  m_chan, xc.skipped, static_cast<double>(xc.worst),
                  parsed ? "" : err.c_str());
    if (m_checkLogKey != key) {
        m_checkLogKey = key;
        VK_LOG_INFO("BONEC-CHECK ok=%d tracks=%d keys=%d dur=%.3f ch=%s "
                    "xcheck=%.6g samples=%d skipped=%d err=%s",
                    parsed ? 1 : 0, static_cast<int>(m_clip.tracks.size()),
                    m_clip.totalKeys(), static_cast<double>(m_clip.duration),
                    haveChannel() ? animation::boneChannelId(m_clip, channel()).c_str()
                                  : "(none)",
                    static_cast<double>(xc.worst), xc.samples, xc.skipped,
                    parsed ? "" : err.c_str());
    }
    m_checkValid = parsed;
    m_checkError = parsed ? std::string() : err;
    m_checkWarnings = 0;
    m_checkXCheck = xc.worst;
    m_checkSkipped = xc.skipped;
}

bool BoneCurvePanel::save(bool force) {
    if (m_readOnly) {
        m_uiError = TR("Read-only: the file failed to load, saving would "
                       "truncate it. Fix it and press Reload.");
        VK_LOG_WARN("BONEC-SAVE-BLOCKED path=%s reason=read-only", m_path.c_str());
        return false;
    }
    if (m_path.empty()) {
        m_uiError = TR("Set an asset path first (e.g. assets/.../walk.anim.json).");
        VK_LOG_WARN("BONEC-SAVE-BLOCKED path=(none) reason=no-path");
        return false;
    }
    // ⑥ 磁盘冲突：打开之后文件被别人（大概率是 CharacterWindow 的 Pose Mode）
    //    存过。用 `force` 才对 —— 自动化里的 `saveforce` 就是这个逃生门。
    if (!force && m_haveStamp) {
        std::error_code ec;
        const auto now = std::filesystem::last_write_time(
            std::filesystem::path(assets::resolveAssetPath(m_path)), ec);
        if (!ec && now != m_stampDisk) {
            m_uiError = TR("Not saved — the file changed on disk after it was "
                           "opened (another editor saved it). Press Reload to take "
                           "its version, or Save anyway to overwrite it.");
            VK_LOG_WARN("BONEC-SAVE-BLOCKED path=%s reason=stale", m_path.c_str());
            return false;
        }
    }

    std::string err;
    if (!animation::saveEditClip(assets::resolveAssetPath(m_path), m_clip, err)) {
        m_error = err;
        m_uiError = err;
        VK_LOG_ERROR("BONEC-SAVE-FAIL path=%s err=%s", m_path.c_str(), err.c_str());
        return false;
    }
    m_dirty = false;
    m_error.clear();
    m_uiError.clear();
    refreshStamp();
    revalidate();
    VK_LOG_INFO("BONEC-SAVE path=%s tracks=%d keys=%d force=%d", m_path.c_str(),
                static_cast<int>(m_clip.tracks.size()), m_clip.totalKeys(),
                force ? 1 : 0);
    return true;
}

// ============================================================
// 绘制
// ============================================================

void BoneCurvePanel::draw(bool& open) {
    if (!m_open) return;

    ImGuiWindowFlags wf = 0;
    const bool docked = applyDockHint(wf);
    if (!docked) {
        ImGui::SetNextWindowSizeConstraints(ImVec2(720.0f, 420.0f),
                                            ImVec2(FLT_MAX, FLT_MAX));
        ImGui::SetNextWindowSize(ImVec2(1040.0f, 620.0f), ImGuiCond_FirstUseEver);
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.18f,
                                       io.DisplaySize.y * 0.12f),
                                ImGuiCond_FirstUseEver);
    }

    // ⚠ 稳定 ID 必须等于 `trayWindowName("bonecurv")` = `###panel_bonecurv`，
    //   否则底栏托盘 / 最小化按钮按 id 找不到这个窗口。
    std::string title = TR("Bone curves");
    if (!m_path.empty()) {
        title += " — ";
        title += m_clip.name.empty() ? clipNameFromPath(m_path) : m_clip.name;
        if (m_dirty) title += " *";
    }
    title += "###panel_bonecurv";

    if (!ImGui::Begin(title.c_str(), docked ? nullptr : &open, wf)) {
        ImGui::End();
        return;
    }
    logRect("BONEC-WIN", ImGui::GetWindowPos(),
            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                   ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    rebuildChannelList();
    drawToolbar();
    drawBody();
    // ⚠ 注入脚本排在画布**之后**：`dragkey` 要用本帧真实的像素↔值映射。
    runInjectedScript();
    drawInspector();
    drawValidation();

    ImGui::End();
    if (!open) close();
}

void BoneCurvePanel::drawToolbar() {
    ImGui::TextDisabled("%s", TR("Asset path"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(340.0f);
    if (ImGui::InputTextWithHint("##bonec_path", "assets/.../walk.anim.json",
                                 m_pathBuf, sizeof(m_pathBuf),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
        // 改路径**不自动读盘**（半截路径会立刻报"打不开"），回车才读。
        m_path = m_pathBuf;
        reload();
    }
    logRect("BONEC-PATH", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    ImGui::SameLine();
    if (ImGui::Button(TR("Reload"))) {
        m_path = m_pathBuf;
        reload();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(m_readOnly);
    if (ImGui::Button(TR("Save"))) save(false);
    logRect("BONEC-SAVE", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::EndDisabled();

    if (m_dirty) {
        ImGui::SameLine();
        ImGui::TextColored(kWarnCol, "%s", TR("unsaved"));
    }

    // ---- 第二行：通道 / 光标 / 适配 ----
    ImGui::SetNextItemWidth(220.0f);
    const std::string cur =
        haveChannel() ? animation::boneChannelId(m_clip, channel()) : std::string("(none)");
    if (ImGui::BeginCombo(TR("Channel"), cur.c_str())) {
        for (int i = 0; i < static_cast<int>(m_chans.size()); ++i) {
            const bool sel = (i == m_chan);
            const std::string id = animation::boneChannelId(m_clip, m_chans[static_cast<std::size_t>(i)]);
            if (ImGui::Selectable(id.c_str(), sel)) actSetChannel(i);
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    logRect("BONEC-CHANNEL", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", TR("One scalar component of one track. The four "
                                   "quaternion components share the keyframe "
                                   "times and the tangents, but keep their own "
                                   "values."));

    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    float head = m_head;
    if (ImGui::DragFloat(TR("Cursor"), &head, 0.005f, 0.0f, durationOf(),
                         TR("%.3f s"))) {
        actSetHead(head);
    }
    logRect("BONEC-HEAD", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    ImGui::SameLine();
    if (ImGui::Button(TR("Fit"))) {
        resetView();
    }
    logRect("BONEC-FIT", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    if (m_readOnly) {
        ImGui::SameLine();
        ImGui::TextColored(kBadCol, "%s", TR("read-only"));
    }
    if (!m_uiError.empty()) {
        ImGui::TextColored(kBadCol, "%s", m_uiError.c_str());
    }
}

void BoneCurvePanel::drawBody() {
    const float listW = 210.0f;
    const float reserve = 7.0f * ImGui::GetFrameHeightWithSpacing();
    const float bodyH = std::max(ImGui::GetContentRegionAvail().y - reserve, 180.0f);

    ImGui::BeginChild("##bonec_list", ImVec2(listW, bodyH), ImGuiChildFlags_Borders);
    drawChannelList();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##bonec_plot", ImVec2(0.0f, bodyH), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
    drawCanvas();
    ImGui::EndChild();
}

void BoneCurvePanel::drawChannelList() {
    ImGui::TextDisabled("%s", TR("Bones"));
    ImGui::Separator();
    std::string lastJoint;
    for (int i = 0; i < static_cast<int>(m_chans.size()); ++i) {
        const animation::BoneChannel& ch = m_chans[static_cast<std::size_t>(i)];
        if (ch.track < 0 || ch.track >= static_cast<int>(m_clip.tracks.size()))
            continue;
        const animation::EditTrack& tr = m_clip.tracks[static_cast<std::size_t>(ch.track)];
        if (tr.joint != lastJoint) {
            if (!lastJoint.empty()) ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.72f, 0.78f, 0.90f, 1.0f), "%s", tr.joint.c_str());
            lastJoint = tr.joint;
        }
        ImGui::PushID(i);
        char label[128];
        std::snprintf(label, sizeof(label), "   %s.%s",
                      animation::pathName(tr.path),
                      animation::pathComponentName(tr.path, ch.comp));
        const bool sel = (i == m_chan);
        if (ImGui::Selectable(label, sel)) actSetChannel(i);
        if (tr.step) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", TR("step"));
        }
        ImGui::PopID();
    }
    ImGui::Separator();
    ImGui::TextDisabled("%s %d", TR("tracks"), static_cast<int>(m_clip.tracks.size()));
    ImGui::TextDisabled("%s %d", TR("keys"), m_clip.totalKeys());
}

void BoneCurvePanel::drawCanvas() {
    rebuildCurve();

    ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 size(std::max(avail.x, 240.0f), std::max(avail.y, 160.0f));
    ImGui::InvisibleButton("##bonec_canvas", size);
    const ImVec2 itemMin = ImGui::GetItemRectMin();
    const ImVec2 itemMax = ImGui::GetItemRectMax();
    logRect("BONEC-CANVAS", itemMin, itemMax);

    Plot p;
    p.rectMin = itemMin;
    p.rectMax = itemMax;
    p.min = ImVec2(itemMin.x + 56.0f, itemMin.y + 8.0f);
    p.max = ImVec2(itemMax.x - 10.0f, itemMax.y - 20.0f);
    p.t0 = m_t0;
    p.t1 = m_t1;
    p.v0 = m_v0;
    p.v1 = m_v1;
    m_plot = p;

    // 像素↔值的映射：**只在变化时**打一行，脚本拿它把"拖多少像素"
    // 换算成"改多少秒/多少值"。
    char mapk[192];
    std::snprintf(mapk, sizeof(mapk), "%.4f|%.4f|%.4f|%.4f|%.1f|%.1f|%.1f|%.1f",
                  static_cast<double>(p.t0), static_cast<double>(p.t1),
                  static_cast<double>(p.v0), static_cast<double>(p.v1),
                  static_cast<double>(p.min.x), static_cast<double>(p.min.y),
                  static_cast<double>(p.max.x), static_cast<double>(p.max.y));
    if (m_mapKey != mapk) {
        m_mapKey = mapk;
        VK_LOG_INFO("BONEC-MAP t0=%.4f t1=%.4f v0=%.4f v1=%.4f pxmin=%.1f,%.1f "
                    "pxmax=%.1f,%.1f",
                    static_cast<double>(p.t0), static_cast<double>(p.t1),
                    static_cast<double>(p.v0), static_cast<double>(p.v1),
                    static_cast<double>(p.min.x), static_cast<double>(p.min.y),
                    static_cast<double>(p.max.x), static_cast<double>(p.max.y));
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const animation::Curve draw = sortedForDraw(m_curve);

    // ---- 网格：竖线每 0.1 s（够密时抽稀），横线按值域 ---- 
    const float timeSpan = std::max(p.t1 - p.t0, 1.0e-6f);
    float stepT = 0.1f;
    while (stepT * pxPerSec(p) < 42.0f) stepT *= 2.0f;
    for (float t = 0.0f; t <= p.t1 + 1.0e-6f; t += stepT) {
        if (t < p.t0) continue;
        const float x = pxOfT(p, t);
        dl->AddLine(ImVec2(x, p.min.y), ImVec2(x, p.max.y),
                    IM_COL32(255, 255, 255, 18));
        char lbl[32];
        std::snprintf(lbl, sizeof(lbl), "%.2fs", static_cast<double>(t));
        dl->AddText(ImVec2(x + 2.0f, p.max.y + 3.0f), IM_COL32(170, 175, 185, 200), lbl);
    }
    {
        // 值轴刻度：3 条
        for (int i = 0; i <= 2; ++i) {
            const float v = p.v0 + (p.v1 - p.v0) * (0.5f * static_cast<float>(i));
            const float y = pyOfV(p, v);
            dl->AddLine(ImVec2(p.min.x, y), ImVec2(p.max.x, y),
                        IM_COL32(255, 255, 255, 16));
            char lbl[32];
            std::snprintf(lbl, sizeof(lbl), "%.3f", static_cast<double>(v));
            dl->AddText(ImVec2(p.min.x - 52.0f, y - 7.0f),
                        IM_COL32(170, 175, 185, 200), lbl);
        }
    }

    // ---- 曲线：按像素列采样（约定 ①：不另写求值）----
    if (draw.keys.size() >= 2) {
        const int cols = std::max(static_cast<int>(p.max.x - p.min.x), 2);
        ImVec2 prev;
        bool has = false;
        for (int c = 0; c <= cols; ++c) {
            const float x = p.min.x + static_cast<float>(c);
            const float t = tOfPx(p, x);
            const float v = animation::evaluateCurve(draw, t);
            const ImVec2 q(x, pyOfV(p, v));
            if (has) dl->AddLine(prev, q, IM_COL32(120, 200, 255, 235), 1.6f);
            prev = q;
            has = true;
        }
    }

    // ---- 关键帧 ----
    for (int i = 0; i < static_cast<int>(m_curve.keys.size()); ++i) {
        const float x = pxOfT(p, m_curve.keys[static_cast<std::size_t>(i)].time);
        const float y = pyOfV(p, keyValue(i));
        const bool sel = isSelected(i);
        const float r = sel ? 5.0f : 3.5f;
        dl->AddCircleFilled(ImVec2(x, y), r,
                            sel ? IM_COL32(255, 210, 90, 255)
                                : IM_COL32(200, 205, 215, 230));
        // 插值类型在点上标出来（Constant 画方块，一眼能看出硬切）
        if (m_curve.keys[static_cast<std::size_t>(i)].interp ==
            animation::Interp::Constant) {
            dl->AddRect(ImVec2(x - 4.0f, y - 4.0f), ImVec2(x + 4.0f, y + 4.0f),
                        IM_COL32(255, 210, 90, 255), 0.0f, 0, 1.2f);
        }
    }

    // ---- 选中帧的手柄 ----
    if (m_primary >= 0 && m_primary < static_cast<int>(m_curve.keys.size())) {
        const animation::CurveKey& k =
            m_curve.keys[static_cast<std::size_t>(m_primary)];
        const ImVec2 kp(pxOfT(p, k.time), pyOfV(p, k.value));
        if (k.interp == animation::Interp::Bezier) {
            const glm::vec2 oh = animation::resolveOutHandle(m_curve, m_primary);
            const glm::vec2 ih = animation::resolveInHandle(m_curve, m_primary);
            const ImVec2 po(pxOfT(p, k.time + oh.x), pyOfV(p, k.value + oh.y));
            const ImVec2 pi(pxOfT(p, k.time + ih.x), pyOfV(p, k.value + ih.y));
            dl->AddLine(kp, po, IM_COL32(150, 255, 180, 200), 1.2f);
            dl->AddLine(kp, pi, IM_COL32(150, 255, 180, 200), 1.2f);
            dl->AddCircleFilled(po, 4.0f, IM_COL32(150, 255, 180, 255));
            dl->AddCircleFilled(pi, 4.0f, IM_COL32(150, 255, 180, 255));
            logRect("BONEC-HANDLE-OUT", ImVec2(po.x - 4, po.y - 4),
                    ImVec2(po.x + 4, po.y + 4));
            logRect("BONEC-HANDLE-IN", ImVec2(pi.x - 4, pi.y - 4),
                    ImVec2(pi.x + 4, pi.y + 4));
        }
    }

    // ---- 光标 ----
    {
        const float x = pxOfT(p, std::clamp(m_head, p.t0, p.t1));
        dl->AddLine(ImVec2(x, p.min.y), ImVec2(x, p.max.y),
                    IM_COL32(255, 160, 90, 180), 1.0f);
        if (draw.keys.size() >= 2) {
            const float v = animation::evaluateCurve(draw, m_head);
            dl->AddText(ImVec2(x + 3.0f, p.min.y + 2.0f),
                        IM_COL32(255, 190, 130, 220),
                        std::to_string(static_cast<int>(v * 1000.0f) / 1000.0f).c_str());
        }
        logRect("BONEC-CURSOR", ImVec2(x - 1, p.min.y), ImVec2(x + 1, p.max.y));
    }

    // ---- 交互 ----
    const ImGuiIO& io = ImGui::GetIO();
    const bool hovered = ImGui::IsItemHovered();
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        beginDrag(p, io.MousePos, io.KeyShift || io.KeyCtrl);
    if (m_drag.kind != DragKind::None && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const ImVec2 d(io.MousePos.x - m_drag.startMouse.x,
                       io.MousePos.y - m_drag.startMouse.y);
        if (m_drag.kind == DragKind::Key) {
            actDragKeysPx(d.x, d.y);
            m_drag.startMouse = io.MousePos;   // 增量式：本帧用掉就归零
        } else if (m_drag.kind == DragKind::HandleOut ||
                   m_drag.kind == DragKind::HandleIn) {
            actDragHandlePx(d.x, d.y);
            m_drag.startMouse = io.MousePos;
        } else if (m_drag.kind == DragKind::Marquee) {
            dl->AddRect(m_drag.startMouse, io.MousePos, IM_COL32(255, 210, 90, 160));
        }
    }
    if (m_drag.kind != DragKind::None && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        endDrag();
}

void BoneCurvePanel::drawInspector() {
    if (!haveChannel()) return;
    ImGui::Separator();
    const animation::EditTrack* tr = trackC();
    if (!tr) return;

    if (m_primary < 0 || m_primary >= static_cast<int>(m_curve.keys.size())) {
        ImGui::TextDisabled("%s", TR("(no key selected — click a dot on the canvas)"));
        return;
    }
    animation::CurveKey& k = m_curve.keys[static_cast<std::size_t>(m_primary)];

    ImGui::Text("%s #%d", TR("sel"), m_primary);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(96.0f);
    float t = k.time;
    if (ImGui::DragFloat(TR("t"), &t, 0.005f, 0.0f, durationOf(), TR("%.3f s"))) {
        actSetKey(m_primary, t, k.value, true);
    }
    logRect("BONEC-INSP-T", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    ImGui::SameLine();
    ImGui::SetNextItemWidth(96.0f);
    float v = k.value;
    if (ImGui::DragFloat(TR("value"), &v, 0.005f, 0.0f, 0.0f, TR("%.4f"))) {
        actSetKey(m_primary, k.time, v, false);
    }
    logRect("BONEC-INSP-V", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    // ---- interp（T）----
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::BeginCombo(TR("interp"), animation::interpName(k.interp))) {
        const animation::Interp kAll[3] = {animation::Interp::Linear,
                                          animation::Interp::Bezier,
                                          animation::Interp::Constant};
        for (animation::Interp ip : kAll) {
            const bool sel = (ip == k.interp);
            if (ImGui::Selectable(animation::interpName(ip), sel))
                actSetInterp(m_primary, ip);
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    logRect("BONEC-INSP-INTERP", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", TR("T on the canvas cycles linear → bezier → "
                                   "constant. It applies to the segment that "
                                   "starts at this key."));

    // ---- ease（只对非贝塞尔有意义）----
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::BeginCombo(TR("ease"), animation::easeModeName(k.ease))) {
        const animation::EaseMode kAll[4] = {
            animation::EaseMode::None, animation::EaseMode::In,
            animation::EaseMode::Out, animation::EaseMode::InOut};
        for (animation::EaseMode m : kAll) {
            const bool sel = (m == k.ease);
            if (ImGui::Selectable(animation::easeModeName(m), sel))
                actSetEase(m_primary, m, k.shape);
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    logRect("BONEC-INSP-EASE", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", TR("Ignored on bezier segments — the handles "
                                   "already decide the shape. Applying both is "
                                   "how curves end up not reaching their own "
                                   "handles."));

    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::BeginCombo(TR("shape"), animation::easeShapeName(k.shape))) {
        const animation::EaseShape kAll[8] = {
            animation::EaseShape::Linear, animation::EaseShape::Sine,
            animation::EaseShape::Quad,   animation::EaseShape::Cubic,
            animation::EaseShape::Smooth, animation::EaseShape::Back,
            animation::EaseShape::Bounce, animation::EaseShape::Elastic};
        for (animation::EaseShape s : kAll) {
            const bool sel = (s == k.shape);
            if (ImGui::Selectable(animation::easeShapeName(s), sel))
                actSetEase(m_primary, k.ease, s);
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    logRect("BONEC-INSP-SHAPE", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    // ---- handle（V）----
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130.0f);
    if (ImGui::BeginCombo(TR("handle"), animation::handleTypeName(k.handle))) {
        const animation::HandleType kAll[5] = {
            animation::HandleType::Auto, animation::HandleType::AutoClamped,
            animation::HandleType::Vector, animation::HandleType::Aligned,
            animation::HandleType::Free};
        for (animation::HandleType ht : kAll) {
            const bool sel = (ht == k.handle);
            if (ImGui::Selectable(animation::handleTypeName(ht), sel))
                actSetHandleType(m_primary, ht);
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    logRect("BONEC-INSP-HANDLE", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", TR("Unlike the camera curves, the handle type is "
                                   "stored per key — auto_clamped keeps following "
                                   "its neighbours until you drag a handle."));

    // ---- 数值手柄（贝塞尔段才有意义）----
    if (k.interp == animation::Interp::Bezier) {
        ImGui::SetNextItemWidth(110.0f);
        float ox = k.outHandle.x, oy = k.outHandle.y;
        if (ImGui::DragFloat2(TR("out"), &ox, 0.002f)) {
            actSetHandle(m_primary, true, glm::vec2(ox, oy));
        }
        logRect("BONEC-INSP-OUT", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        float ix = k.inHandle.x, iy = k.inHandle.y;
        if (ImGui::DragFloat2(TR("in"), &ix, 0.002f)) {
            actSetHandle(m_primary, false, glm::vec2(ix, iy));
        }
        logRect("BONEC-INSP-IN", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", TR("Offsets in (seconds, value) relative to "
                                       "this key. out.x must stay >= 0 and "
                                       "in.x <= 0, otherwise the segment stops "
                                       "being monotonic in time."));
    }

    // ---- 读数 ----
    ImGui::TextDisabled("%s %.4f  %s %d  %s %s", TR("v@cursor"), valueAt(m_head),
                        TR("keys"), static_cast<int>(m_curve.keys.size()),
                        TR("step"), tr->step ? TR("yes") : TR("no"));
}

void BoneCurvePanel::drawValidation() {
    ImGui::Separator();
    if (m_checkValid) {
        ImGui::TextColored(kOkCol, "%s", TR("roundtrip OK"));
    } else {
        ImGui::TextColored(kBadCol, "%s %s", TR("roundtrip FAILED"),
                           m_checkError.c_str());
    }
    ImGui::SameLine();
    // ① 的核心读数：画布值 vs 运行时采样值
    if (m_checkXCheck == 0.0f) {
        ImGui::TextColored(kOkCol, "%s", TR("canvas ≡ runtime"));
    } else {
        ImGui::TextColored(kWarnCol, "%s %.3g", TR("canvas ≠ runtime"), m_checkXCheck);
    }
    ImGui::SameLine();
    // ⑤：旋转的 Linear 段运行时走 slerp，画布上没有那条线（跳过计数）
    if (m_checkSkipped > 0) {
        ImGui::TextDisabled("%s %d", TR("(slerp segments skipped)"), m_checkSkipped);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", TR("On rotation tracks a linear segment runs "
                                       "through slerp at runtime, while the four "
                                       "component curves interpolate separately "
                                       "and renormalise (nlerp) — same arc, "
                                       "different speed. Those samples are not "
                                       "comparable and are excluded from the "
                                       "number above."));
    }
    logRect("BONEC-CHECK", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

// ============================================================
// 注入脚本
// ============================================================
//
// `MYVK_EDITOR_BONEC_SCRIPT="chan:3;sel:2;setkey:2:0.5:0.25;interp:2:bezier;save"`
//
// 与鼠标手势共用 `act*` 那一族，所以"能不能自动验"不取决于把某个点的
// 像素坐标算准没有。`sleep:<ms>` 是给"磁盘冲突"那条用例的闸门。
void BoneCurvePanel::runInjectedScript() {
    if (m_injected) return;
    const char* env = std::getenv("MYVK_EDITOR_BONEC_SCRIPT");
    if (!env || !*env) return;
    m_injected = true;

    int applied = 0, failed = 0;
    const auto split = [](const std::string& s, char sep) {
        std::vector<std::string> out;
        std::size_t b = 0;
        while (true) {
            const std::size_t p = s.find(sep, b);
            if (p == std::string::npos) {
                out.push_back(s.substr(b));
                break;
            }
            out.push_back(s.substr(b, p - b));
            b = p + 1;
        }
        return out;
    };
    const auto toI = [](const std::string& s, int def) {
        try {
            return std::stoi(s);
        } catch (...) {
            return def;
        }
    };
    const auto toF = [](const std::string& s, float def) {
        try {
            return std::stof(s);
        } catch (...) {
            return def;
        }
    };

    std::string s(env);
    std::size_t pos = 0;
    while (pos <= s.size()) {
        const std::size_t semi = s.find(';', pos);
        std::string cmd = s.substr(pos, semi == std::string::npos
                                            ? std::string::npos
                                            : semi - pos);
        pos = (semi == std::string::npos) ? s.size() + 1 : semi + 1;
        // 去掉首尾空白
        while (!cmd.empty() && (cmd.front() == ' ' || cmd.front() == '\t')) cmd.erase(cmd.begin());
        while (!cmd.empty() && (cmd.back() == ' ' || cmd.back() == '\t')) cmd.pop_back();
        if (cmd.empty()) continue;

        const std::size_t colon = cmd.find(':');
        const std::string verb = (colon == std::string::npos) ? cmd : cmd.substr(0, colon);
        const std::string args = (colon == std::string::npos) ? std::string() : cmd.substr(colon + 1);
        const std::vector<std::string> a = split(args, ':');
        bool ok = false;

        if (verb == "sleep") {
            const int ms = a.empty() ? 0 : toI(a[0], 0);
            if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            ok = true;
        } else if (verb == "path") {
            m_path = args;
            std::snprintf(m_pathBuf, sizeof(m_pathBuf), "%s", m_path.c_str());
            reload();
            ok = true;
        } else if (verb == "reload") {
            reload();
            ok = true;
        } else if (verb == "chan") {
            if (!a.empty() && a[0] == "next") {
                ok = actSetChannel(std::min(m_chan + 1, static_cast<int>(m_chans.size()) - 1));
            } else if (!a.empty() && a[0] == "prev") {
                ok = actSetChannel(std::max(m_chan - 1, 0));
            } else {
                ok = actSetChannel(a.empty() ? 0 : toI(a[0], 0));
            }
        } else if (verb == "sel") {
            m_sel.clear();
            for (const std::string& part : split(args, ','))
                m_sel.push_back(toI(part, -1));
            clampSel();
            m_primary = m_sel.empty() ? -1 : m_sel.front();
            if (!a.empty()) m_primary = toI(a[0], m_primary);
            ok = m_primary >= 0;
        } else if (verb == "selall") {
            ok = actSelectAll();
        } else if (verb == "setkey") {
            if (a.size() >= 3)
                ok = actSetKey(toI(a[0], -1), toF(a[1], 0.0f), toF(a[2], 0.0f), true);
        } else if (verb == "dragkey") {
            if (a.size() >= 3) {
                m_sel = {toI(a[0], -1)};
                m_primary = m_sel[0];
                m_drag = DragState{};
                m_drag.kind = DragKind::Key;
                m_drag.index = m_primary;
                m_drag.moving = {m_primary};
                m_drag.origT = {m_curve.keys.empty()
                                    ? 0.0f
                                    : m_curve.keys[static_cast<std::size_t>(m_primary)].time};
                m_drag.origV = {keyValue(m_primary)};
                ok = actDragKeysPx(toF(a[1], 0.0f), toF(a[2], 0.0f));
                endDrag();
            }
        } else if (verb == "interp") {
            if (a.size() >= 2) {
                const int i = (a[0] == "all") ? -1 : toI(a[0], -1);
                animation::Interp ip = animation::Interp::Linear;
                animation::parseInterp(a[1], ip);
                if (i < 0) {
                    int n = 0;
                    for (int j = 0; j < static_cast<int>(m_curve.keys.size()); ++j)
                        if (actSetInterp(j, ip)) ++n;
                    ok = n > 0;
                } else {
                    ok = actSetInterp(i, ip);
                }
            }
        } else if (verb == "ease") {
            if (a.size() >= 2) {
                animation::EaseMode m = animation::EaseMode::None;
                animation::parseEaseMode(a[1], m);
                const int i = (a[0] == "all") ? -1 : toI(a[0], -1);
                const animation::EaseShape sh =
                    (m_curve.keys.empty() ? animation::EaseShape::Linear
                                          : m_curve.keys[static_cast<std::size_t>(
                                                std::max(i, 0))].shape);
                if (i < 0) {
                    int n = 0;
                    for (int j = 0; j < static_cast<int>(m_curve.keys.size()); ++j)
                        if (actSetEase(j, m, m_curve.keys[static_cast<std::size_t>(j)].shape))
                            ++n;
                    ok = n > 0;
                } else {
                    ok = actSetEase(i, m, sh);
                }
            }
        } else if (verb == "shape") {
            if (a.size() >= 2) {
                animation::EaseShape sh = animation::EaseShape::Linear;
                animation::parseEaseShape(a[1], sh);
                const int i = (a[0] == "all") ? -1 : toI(a[0], -1);
                if (i < 0) {
                    int n = 0;
                    for (int j = 0; j < static_cast<int>(m_curve.keys.size()); ++j)
                        if (actSetEase(j, m_curve.keys[static_cast<std::size_t>(j)].ease, sh))
                            ++n;
                    ok = n > 0;
                } else {
                    ok = actSetEase(i, m_curve.keys[static_cast<std::size_t>(i)].ease, sh);
                }
            }
        } else if (verb == "handle") {
            if (a.size() >= 2) {
                animation::HandleType ht = animation::HandleType::AutoClamped;
                animation::parseHandleType(a[1], ht);
                const int i = (a[0] == "all") ? -1 : toI(a[0], -1);
                if (i < 0) {
                    int n = 0;
                    for (int j = 0; j < static_cast<int>(m_curve.keys.size()); ++j)
                        if (actSetHandleType(j, ht)) ++n;
                    ok = n > 0;
                } else {
                    ok = actSetHandleType(i, ht);
                }
            }
        } else if (verb == "sethandle") {
            if (a.size() >= 4)
                ok = actSetHandle(toI(a[0], -1), a[1] == "out",
                                  glm::vec2(toF(a[2], 0.0f), toF(a[3], 0.0f)));
        } else if (verb == "drag") {
            if (a.size() >= 3) {
                m_drag = DragState{};
                m_drag.kind = (a[0] == "out") ? DragKind::HandleOut
                                              : DragKind::HandleIn;
                m_drag.index = m_primary;
                m_drag.handleOrig =
                    (a[0] == "out")
                        ? m_curve.keys[static_cast<std::size_t>(m_primary)].outHandle
                        : m_curve.keys[static_cast<std::size_t>(m_primary)].inHandle;
                // 像素↔值的映射来自上一帧 `drawCanvas` 填的 `m_plot`（同相机
                // 面板：脚本排在画布之后）。这里不改它。
                ok = actDragHandlePx(toF(a[1], 0.0f), toF(a[2], 0.0f));
                endDrag();
            }
        } else if (verb == "head") {
            ok = actSetHead(a.empty() ? 0.0f : toF(a[0], 0.0f));
        } else if (verb == "fit") {
            resetView();
            ok = true;
        } else if (verb == "digest") {
            const animation::Curve d = sortedForDraw(m_curve);
            VK_LOG_INFO("BONEC-DIGEST ch=%s n=%d d=%llu",
                        animation::boneChannelId(m_clip, channel()).c_str(),
                        static_cast<int>(d.keys.size()),
                        static_cast<unsigned long long>(animation::curveDigest(d)));
            ok = true;
        } else if (verb == "check") {
            revalidate();
            VK_LOG_INFO("BONEC-CHECK-NOW ok=%d xcheck=%.6g skipped=%d",
                        m_checkValid ? 1 : 0,
                        static_cast<double>(m_checkXCheck), m_checkSkipped);
            ok = true;
        } else if (verb == "save") {
            ok = save(false);
        } else if (verb == "saveforce") {
            ok = save(true);
        } else {
            VK_LOG_WARN("BONEC-SCRIPT-UNKNOWN verb=%s", verb.c_str());
        }

        if (ok) ++applied;
        else ++failed;
    }

    VK_LOG_INFO("BONEC-SCRIPT applied=%d failed=%d chan=%d id=%s tracks=%d keys=%d "
                "sel=%d dirty=%d",
                applied, failed, m_chan,
                haveChannel() ? animation::boneChannelId(m_clip, channel()).c_str()
                              : "(none)",
                static_cast<int>(m_clip.tracks.size()), m_clip.totalKeys(),
                static_cast<int>(m_sel.size()), m_dirty ? 1 : 0);
}

} // namespace editor
