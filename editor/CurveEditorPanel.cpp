// ============================================================
// editor/CurveEditorPanel.cpp —— 曲线编辑器（Graph Editor，2026-10-28 / R6）
//
// 结构：工具栏（路径 / Reload / Save / Fit / 通道 / 光标）
//      → 主体（左：通道列表；右：画布，全自绘）
//      → 侧栏（选中那一帧的 t / value / interp / 两根手柄）
//      → 校验栏（存盘往返 + "画布 ≡ 运行时"的数值差）
//
// 所有**改数据**的入口都是 `act*` 那一族 —— 鼠标手势与注入脚本共用它们，
// 所以"能不能自动验"不取决于把某个点算准没有（同 TimelineEditorWindow）。
//
// ⚠ 日志前缀 `CURVED-`：与 `TLED-`（时间轴编辑器）、`SEQ-`（过场运行时）、
//   `ANIM-`（骨骼动画）都分开。四套东西可能同时出现在一份日志里。
// ============================================================

#include "CurveEditorPanel.h"

#include "DebugRects.h"
#include "EditorContext.h"
#include "Localization.h"
#include "PanelDock.h"

#include "assets/AssetPath.h"
#include "core/Logger.h"
#include "ecs/Components.h"
#include "scene/Scene.h"
#include "sequence/SequencerSystem.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <numeric>
#include <system_error>
#include <thread>

namespace editor {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kRadToDeg = 180.0f / kPi;
constexpr float kDegToRad = kPi / 180.0f;

// 命中半径（像素）。7 是"手指按得准"的上限：再大连成一个都选不中，
// 再小就得瞄着点。
constexpr float kKeyHitPx = 7.0f;
constexpr float kHandleHitPx = 6.0f;

const ImVec4 kOkCol(0.45f, 0.85f, 0.52f, 1.0f);
const ImVec4 kBadCol(1.00f, 0.45f, 0.35f, 1.0f);
const ImVec4 kWarnCol(1.00f, 0.75f, 0.30f, 1.0f);

// 与 `sequence::shortestAngle` 同一口径：结果落在 (−π, π]。
float wrapPi(float a) {
    while (a > kPi) a -= 2.0f * kPi;
    while (a <= -kPi) a += 2.0f * kPi;
    return a;
}

// `.seq.json` 的 ease 四态 → 曲线核心的三个字段。
//
// ⚠⚠ 这张表是**唯一的**换算，`sampleCamera` 里那一段是同一张表
//    （SequenceAsset.cpp:460-475）。两处必须一起改 —— 改一处而另一处
//    不动，表现就是"画布和进 Play 不一样"，而画布看着挺合理。
// ⚠ 特别注意 `Smooth`：它是 `Linear + InOut + EaseShape::Smooth`，
//    不是 `Linear + InOut + EaseShape::Linear`。后者会被 `easeFn` 开头的
//    `shape == Linear → 恒等` 短路成**匀速**，也就把 smoothstep 变成了 linear。
void mapEaseToCurve(sequence::Ease e, animation::Interp& ip,
                    animation::EaseMode& em, animation::EaseShape& sh) {
    ip = animation::Interp::Linear;
    em = animation::EaseMode::None;
    sh = animation::EaseShape::Linear;
    switch (e) {
    case sequence::Ease::Linear:
        break;                       // 全默认 = 恒等映射
    case sequence::Ease::Smooth:
        em = animation::EaseMode::InOut;
        sh = animation::EaseShape::Smooth;
        break;
    case sequence::Ease::Step:
        // Step 在 `sampleCamera` 里是提前 return（连 `out.t` 都保持 a.t），
        // 投影成 Constant 的**值**是一致的：整段保持左端值。
        ip = animation::Interp::Constant;
        break;
    case sequence::Ease::Bezier:
        ip = animation::Interp::Bezier;
        break;
    }
}

// `sequences/opening.seq.json` → `opening`（与 parseSequence 的兜底同口径）
std::string nameFromPath(const std::string& p) {
    if (p.empty()) return "untitled";
    std::string s = p;
    const std::size_t slash = s.find_last_of("/\\");
    if (slash != std::string::npos) s = s.substr(slash + 1);
    const std::string suf = ".seq.json";
    if (s.size() > suf.size() &&
        s.compare(s.size() - suf.size(), suf.size(), suf) == 0)
        s = s.substr(0, s.size() - suf.size());
    return s.empty() ? std::string("untitled") : s;
}

// 两份路径化成"绝对 + 正斜杠 + 小写"再比（同 TimelineEditorWindow）。
std::string canonicalOf(const std::string& p) {
    if (p.empty()) return {};
    std::error_code ec;
    const std::string full = assets::resolveAssetPath(p);
    const std::filesystem::path c =
        std::filesystem::weakly_canonical(std::filesystem::path(full), ec);
    std::string s = ec ? full : c.generic_string();
    for (char& ch : s) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return s;
}

const char* easeCycleName(int i) {
    // T 的循环顺序：匀速 → 平滑 → 硬切 → 贝塞尔。从"最常用"排起。
    static const char* kNames[4] = {"linear", "smooth", "step", "bezier"};
    return kNames[((i % 4) + 4) % 4];
}

const char* handleTypeCycleName(int i) {
    using animation::HandleType;
    static const HandleType kAll[5] = {HandleType::Auto, HandleType::AutoClamped,
                                       HandleType::Vector, HandleType::Aligned,
                                       HandleType::Free};
    return animation::handleTypeName(kAll[((i % 5) + 5) % 5]);
}

} // namespace

CurveEditorPanel::CurveEditorPanel(EditorContext& ctx) : m_ctx(ctx) {}

// ============================================================
// 打开 / 重读 / 磁盘时间戳
// ============================================================

bool CurveEditorPanel::refreshStamp() {
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

bool CurveEditorPanel::open(const std::string& relPath) {
    m_open = true;
    m_path = relPath;
    m_sel.clear();
    m_primary = -1;
    m_error.clear();
    m_uiError.clear();
    m_dirty = false;
    m_injected = false;
    m_drag = DragState{};
    m_channel = Channel::PosX;
    m_curveStamp.clear();
    m_mapKey.clear();
    m_keyStamp.clear();
    m_curve = animation::Curve{};
    m_vals.clear();
    m_head = 0.0f;
    m_loaded = true;
    std::snprintf(m_pathBuf, sizeof(m_pathBuf), "%s", m_path.c_str());

    if (relPath.empty()) {
        m_asset = sequence::SequenceAsset{};
        m_readOnly = false;
        m_haveStamp = false;
        VK_LOG_INFO("CURVED-OPEN path=(none) ok=0 keys=0 dur=0.000 readOnly=0");
        revalidate();
        resetView();
        return false;
    }

    sequence::SequenceAsset a;
    const bool ok = sequence::loadSequence(relPath, a);
    m_asset = std::move(a);
    // 读盘失败 → 只读（同 TimelineEditorWindow ⑤）：`cameraKeys` 可能只是
    // 前几帧的残片，允许存盘 = 把 40 帧的过场截成 3 帧。
    m_readOnly = !ok;
    if (!ok) m_error = m_asset.error;
    refreshStamp();

    VK_LOG_INFO("CURVED-OPEN path=%s ok=%d keys=%d dur=%.3f readOnly=%d",
                relPath.c_str(), ok ? 1 : 0,
                static_cast<int>(m_asset.cameraKeys.size()),
                static_cast<double>(m_asset.duration), m_readOnly ? 1 : 0);
    if (!m_asset.cameraKeys.empty()) {
        m_sel.push_back(0);
        m_primary = 0;
    }
    resetView();
    revalidate();
    return ok;
}

void CurveEditorPanel::close() {
    if (m_open && m_dirty) {
        VK_LOG_WARN("CURVED-CLOSE-DIRTY path=%s（有未存的改动）", m_path.c_str());
    }
    m_open = false;
    m_drag = DragState{};
}

void CurveEditorPanel::reload() {
    m_drag = DragState{};
    if (m_path.empty()) {
        m_asset = sequence::SequenceAsset{};
        m_readOnly = false;
        m_dirty = false;
        m_sel.clear();
        m_primary = -1;
        m_curveStamp.clear();
        revalidate();
        resetView();
        return;
    }
    sequence::SequenceAsset a;
    const bool ok = sequence::loadSequence(m_path, a);
    m_asset = std::move(a);
    m_readOnly = !ok;
    m_dirty = false;
    m_uiError.clear();
    m_error = ok ? std::string() : m_asset.error;
    m_curveStamp.clear();
    m_keyStamp.clear();
    m_mapKey.clear();
    m_sel.clear();
    if (!m_asset.cameraKeys.empty()) {
        m_sel.push_back(0);
        m_primary = 0;
    } else {
        m_primary = -1;
    }
    refreshStamp();
    resetView();
    VK_LOG_INFO("CURVED-RELOAD path=%s ok=%d keys=%d readOnly=%d", m_path.c_str(),
                ok ? 1 : 0, static_cast<int>(m_asset.cameraKeys.size()),
                m_readOnly ? 1 : 0);
    revalidate();
}

// ============================================================
// 通道
// ============================================================

const char* CurveEditorPanel::channelId(Channel c) {
    switch (c) {
    case Channel::PosX: return "position.x";
    case Channel::PosY: return "position.y";
    case Channel::PosZ: return "position.z";
    case Channel::Yaw: return "yaw";
    case Channel::Pitch: return "pitch";
    case Channel::Fov: return "fov";
    case Channel::Count: break;
    }
    return "?";
}

bool CurveEditorPanel::channelIsDegrees(Channel c) {
    return c == Channel::Yaw || c == Channel::Pitch;
}

// 通道值（画布纵轴单位：yaw/pitch 是**度**，其余与资产同单位）。
// ⚠ yaw 要**展开**：资产里 170° → −170° 是"转 20°"，而画布上直接连这两个
//   点会画出"往回转 340°"的一条长线 —— 与 `shortestAngle` 的存在理由同一件事。
void CurveEditorPanel::readChannelValues(std::vector<float>& out, Channel ch) const {
    const int n = static_cast<int>(m_asset.cameraKeys.size());
    out.assign(static_cast<std::size_t>(std::max(n, 0)), 0.0f);
    if (n <= 0) return;
    switch (ch) {
    case Channel::PosX:
        for (int i = 0; i < n; ++i) out[i] = m_asset.cameraKeys[i].position.x;
        break;
    case Channel::PosY:
        for (int i = 0; i < n; ++i) out[i] = m_asset.cameraKeys[i].position.y;
        break;
    case Channel::PosZ:
        for (int i = 0; i < n; ++i) out[i] = m_asset.cameraKeys[i].position.z;
        break;
    case Channel::Pitch:
        for (int i = 0; i < n; ++i) out[i] = m_asset.cameraKeys[i].pitch * kRadToDeg;
        break;
    case Channel::Fov:
        for (int i = 0; i < n; ++i) out[i] = m_asset.cameraKeys[i].fov;
        break;
    case Channel::Yaw: {
        out[0] = m_asset.cameraKeys[0].yaw * kRadToDeg;
        for (int i = 1; i < n; ++i) {
            out[i] = out[i - 1] +
                     sequence::shortestAngle(m_asset.cameraKeys[i - 1].yaw,
                                             m_asset.cameraKeys[i].yaw) *
                         kRadToDeg;
        }
        break;
    }
    case Channel::Count:
        break;
    }
}

float CurveEditorPanel::keyValue(int i) const {
    if (i < 0 || i >= static_cast<int>(m_vals.size())) return 0.0f;
    return m_vals[static_cast<std::size_t>(i)];
}

bool CurveEditorPanel::writeKeyValue(int i, float v) {
    if (i < 0 || i >= static_cast<int>(m_asset.cameraKeys.size())) return false;
    sequence::SeqCameraKey& k = m_asset.cameraKeys[static_cast<std::size_t>(i)];
    switch (m_channel) {
    case Channel::PosX: k.position.x = v; break;
    case Channel::PosY: k.position.y = v; break;
    case Channel::PosZ: k.position.z = v; break;
    case Channel::Yaw:
        // ⚠ 写回时**收回** (−π, π]：资产里 yaw 的唯一合法区间就是它
        //   （见 SequenceAsset.h ①）。拖到别的圈上会被折回来，画布上那一帧
        //   会跳半圈 —— 那是 yaw 的周期性，不是拖动坏了。
        k.yaw = wrapPi(v * kDegToRad);
        break;
    case Channel::Pitch:
        // 与 `scene::Camera::kPitchLimit` 同一个上限（89°）：超过之后
        // `glm::lookAt` 的基会退化，画面翻转。
        k.pitch = std::clamp(v, -89.0f, 89.0f) * kDegToRad;
        break;
    case Channel::Fov: k.fov = std::clamp(v, 10.0f, 120.0f); break;
    case Channel::Count: return false;
    }
    m_curveStamp.clear();
    return true;
}

// ---------------------------------------------------------------- 投影

std::string CurveEditorPanel::curveStampOf() const {
    char buf[192];
    std::string s;
    s.reserve(m_asset.cameraKeys.size() * 80 + 32);
    std::snprintf(buf, sizeof(buf), "ch=%d|n=%d|",
                  static_cast<int>(m_channel),
                  static_cast<int>(m_asset.cameraKeys.size()));
    s += buf;
    for (const sequence::SeqCameraKey& k : m_asset.cameraKeys) {
        std::snprintf(buf, sizeof(buf), "%.6f,%.6f,%.6f,%.6f,%d,%.5f,%.5f,%.5f,%.5f;",
                      static_cast<double>(k.t), static_cast<double>(k.position.x),
                      static_cast<double>(k.position.y),
                      static_cast<double>(k.position.z), static_cast<int>(k.ease),
                      static_cast<double>(k.outHandle.x),
                      static_cast<double>(k.outHandle.y),
                      static_cast<double>(k.inHandle.x),
                      static_cast<double>(k.inHandle.y));
        s += buf;
    }
    // yaw/pitch/fov 不进指纹：它们已经被 position 那三行之外的字段覆盖了。
    // 单独补一遍，省得将来换通道时漏掉。
    for (const sequence::SeqCameraKey& k : m_asset.cameraKeys) {
        std::snprintf(buf, sizeof(buf), "%.6f,%.6f,%.6f;", static_cast<double>(k.yaw),
                      static_cast<double>(k.pitch), static_cast<double>(k.fov));
        s += buf;
    }
    return s;
}

void CurveEditorPanel::rebuildCurve() {
    const std::string st = curveStampOf();
    if (st == m_curveStamp) return;
    m_curveStamp = st;
    readChannelValues(m_vals, m_channel);
    m_curve = buildCurve(m_vals);
}

// 关键帧 → `animation::Curve`。**这是画布与运行时之间唯一的换算**。
//
// ⚠⚠ 手柄的归属是这里最容易错的一处：`.seq.json` 里**一个段的两根手柄
//    都记在左端那帧**上（`outHandle` 相对本帧、`inHandle` 相对下一帧，
//    见 SeqCameraKey 的注释与 `sampleCamera` 的调用）。而
//    `animation::Curve` 的语义是"段的两端各管自己那一根" —— 所以把
//    `pk[i].inHandle` 搬到 `c.keys[i+1].inHandle` 上，**不是**抄 `pk[i+1]`。
//    照 `pk[i+1]` 抄的话：任何一段的 in 手柄都会突然变成"下一段那个键
//    的 in 手柄"，画布上看着像手柄跳到了别的段上，而运行时**毫无变化**。
//
// 另：这里对投影**按 t 稳定排序**（不改资产）。拖动中资产数组允许暂时乱序
// （③：松手才重排），但画布必须一直是连得起来的曲线。
animation::Curve CurveEditorPanel::buildCurve(const std::vector<float>& vals) const {
    struct PKey {
        float t = 0.0f;
        float v = 0.0f;
        sequence::Ease ease = sequence::Ease::Smooth;
        glm::vec2 out{0.0f};
        glm::vec2 in{0.0f};
    };
    std::vector<PKey> pk;
    const int n = static_cast<int>(std::min(vals.size(), m_asset.cameraKeys.size()));
    pk.reserve(static_cast<std::size_t>(std::max(n, 0)));
    for (int i = 0; i < n; ++i) {
        const sequence::SeqCameraKey& k = m_asset.cameraKeys[static_cast<std::size_t>(i)];
        PKey p;
        p.t = k.t;
        p.v = vals[static_cast<std::size_t>(i)];
        p.ease = k.ease;
        p.out = k.outHandle;
        p.in = k.inHandle;
        pk.push_back(p);
    }
    std::stable_sort(pk.begin(), pk.end(),
                     [](const PKey& a, const PKey& b) { return a.t < b.t; });

    animation::Curve c;
    c.before = animation::Extrapolate::Hold;
    c.after = animation::Extrapolate::Hold;
    c.keys.resize(pk.size());
    for (std::size_t i = 0; i < pk.size(); ++i) {
        animation::CurveKey& ck = c.keys[i];
        ck.time = pk[i].t;
        ck.value = pk[i].v;
        // ⚠ 必须是 Free：`CurveKey` 默认是 AutoClamped，而自动手柄**不认**
        //   你写的数值（会把 outHandle 覆盖成按邻居算出来的斜率）。R5 的
        //   自检 `curve_bezier_default_is_smoothstep` 就是被这一条咬过的。
        ck.handle = animation::HandleType::Free;
        mapEaseToCurve(pk[i].ease, ck.interp, ck.ease, ck.shape);
        ck.outHandle = glm::vec2(0.0f);
        ck.inHandle = glm::vec2(0.0f);
    }
    for (std::size_t i = 0; i + 1 < pk.size(); ++i) {
        if (pk[i].ease != sequence::Ease::Bezier) continue;
        const float span = std::max(pk[i + 1].t - pk[i].t, 1.0e-6f);
        const float dv = pk[i + 1].v - pk[i].v;
        c.keys[i].outHandle = glm::vec2(pk[i].out.x * span, pk[i].out.y * dv);
        c.keys[i + 1].inHandle = glm::vec2(pk[i].in.x * span, pk[i].in.y * dv);
    }
    return c;
}

// ---------------------------------------------------------------- 画布 ≡ 运行时

CurveEditorPanel::XCheck CurveEditorPanel::crossCheck() const {
    XCheck xc;
    const int n = static_cast<int>(m_asset.cameraKeys.size());
    if (n < 2) return xc;
    const float dur = durationOf();
    for (int ci = 0; ci < static_cast<int>(Channel::Count); ++ci) {
        const Channel ch = static_cast<Channel>(ci);
        std::vector<float> vals;
        readChannelValues(vals, ch);
        const animation::Curve c = buildCurve(vals);
        for (int s = 0; s <= 64; ++s) {
            const float t = dur * static_cast<float>(s) / 64.0f;
            sequence::SeqCameraKey k;
            if (!sequence::sampleCamera(m_asset, t, k)) continue;
            const float ref = animation::evaluateCurve(c, t);
            float run = 0.0f;
            switch (ch) {
            case Channel::PosX: run = k.position.x; break;
            case Channel::PosY: run = k.position.y; break;
            case Channel::PosZ: run = k.position.z; break;
            case Channel::Yaw: run = k.yaw * kRadToDeg; break;
            case Channel::Pitch: run = k.pitch * kRadToDeg; break;
            case Channel::Fov: run = k.fov; break;
            case Channel::Count: break;
            }
            // yaw 两边圈数可能不同（画布是展开的），比"最短弧差"。
            const float d = (ch == Channel::Yaw)
                                ? std::fabs(wrapPi((ref - run) * kDegToRad)) * kRadToDeg
                                : std::fabs(ref - run);
            ++xc.samples;
            if (d > xc.worst) {
                xc.worst = d;
                xc.where = channelId(ch);
            }
        }
    }
    return xc;
}

// ============================================================
// 视图
// ============================================================

float CurveEditorPanel::durationOf() const {
    float d = std::max(m_asset.duration, m_asset.lastCameraTime());
    return std::max(d, 0.1f);
}

float CurveEditorPanel::pxOfT(const Plot& p, float t) const {
    const float span = std::max(p.t1 - p.t0, 1.0e-6f);
    return p.min.x + (t - p.t0) / span * (p.max.x - p.min.x);
}

float CurveEditorPanel::pyOfV(const Plot& p, float v) const {
    const float span = std::max(p.v1 - p.v0, 1.0e-6f);
    // y 轴向下：值越大越靠上
    return p.max.y - (v - p.v0) / span * (p.max.y - p.min.y);
}

float CurveEditorPanel::tOfPx(const Plot& p, float x) const {
    const float w = std::max(p.max.x - p.min.x, 1.0f);
    return p.t0 + (x - p.min.x) / w * (p.t1 - p.t0);
}

float CurveEditorPanel::vOfPy(const Plot& p, float y) const {
    const float h = std::max(p.max.y - p.min.y, 1.0f);
    return p.v0 + (p.max.y - y) / h * (p.v1 - p.v0);
}

float CurveEditorPanel::pxPerSec(const Plot& p) const {
    const float w = std::max(p.max.x - p.min.x, 1.0f);
    return w / std::max(p.t1 - p.t0, 1.0e-6f);
}

float CurveEditorPanel::pxPerUnit(const Plot& p) const {
    const float h = std::max(p.max.y - p.min.y, 1.0f);
    return h / std::max(p.v1 - p.v0, 1.0e-6f);
}

void CurveEditorPanel::resetView() {
    const float dur = durationOf();
    // 两端各留 6%：关键帧范围之外的**外推段**要看得见（画布上那两截虚线）。
    m_t0 = -0.06f * dur;
    m_t1 = 1.06f * dur;
    m_head = std::clamp(m_head, 0.0f, std::max(m_asset.duration, 0.0f));
    rebuildCurve();
    fitValueRange();
}

void CurveEditorPanel::fitValueRange() {
    rebuildCurve();
    float lo = 0.0f, hi = 0.0f;
    bool any = false;
    auto include = [&](float v) {
        if (!any) {
            lo = hi = v;
            any = true;
            return;
        }
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    };

    const int kSamples = 200;
    for (int i = 0; i <= kSamples; ++i) {
        const float t = m_t0 + (m_t1 - m_t0) * static_cast<float>(i) / kSamples;
        include(valueAt(t));
    }
    // 手柄端点：贝塞尔的**控制点**可以落在曲线外面（过冲就是这么来的），
    // 不把它们算进范围的话，手柄点会被画到画布之外 —— 拖不到。
    for (std::size_t i = 0; i + 1 < m_asset.cameraKeys.size(); ++i) {
        const sequence::SeqCameraKey& a = m_asset.cameraKeys[i];
        if (a.ease != sequence::Ease::Bezier) continue;
        const float dv = keyValue(static_cast<int>(i) + 1) - keyValue(static_cast<int>(i));
        include(keyValue(static_cast<int>(i)) + a.outHandle.y * dv);
        include(keyValue(static_cast<int>(i) + 1) + a.inHandle.y * dv);
    }

    if (!any) {
        m_v0 = -1.0f;
        m_v1 = 1.0f;
        return;
    }
    if (hi - lo < 1.0e-4f) {
        const float mid = 0.5f * (lo + hi);
        lo = mid - 1.0f;
        hi = mid + 1.0f;
    }
    const float pad = (hi - lo) * 0.12f;
    m_v0 = lo - pad;
    m_v1 = hi + pad;
}

void CurveEditorPanel::zoomTime(float anchorPx, float factor) {
    const float dur = durationOf();
    const float span = std::max(m_t1 - m_t0, 1.0e-4f);
    const float anchorT = tOfPx(m_plot, anchorPx);
    float ns = std::clamp(span * factor, 0.02f * dur, 8.0f * dur);
    const float k = (anchorT - m_t0) / span;
    m_t0 = anchorT - k * ns;
    m_t1 = m_t0 + ns;
}

// ============================================================
// 选择
// ============================================================

bool CurveEditorPanel::isSelected(int i) const {
    return std::find(m_sel.begin(), m_sel.end(), i) != m_sel.end();
}

void CurveEditorPanel::clampSel() {
    const int n = static_cast<int>(m_asset.cameraKeys.size());
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

bool CurveEditorPanel::actSelect(int i, bool additive) {
    const int n = static_cast<int>(m_asset.cameraKeys.size());
    if (i < 0 || i >= n) return false;
    if (additive) {
        auto it = std::find(m_sel.begin(), m_sel.end(), i);
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
    return true;
}

bool CurveEditorPanel::actSelectAll() {
    const int n = static_cast<int>(m_asset.cameraKeys.size());
    m_sel.clear();
    for (int i = 0; i < n; ++i) m_sel.push_back(i);
    m_primary = n > 0 ? 0 : -1;
    return n > 0;
}

bool CurveEditorPanel::actSelectRect(const Plot& p, ImVec2 a, ImVec2 b, bool additive) {
    rebuildCurve();
    const float x0 = std::min(a.x, b.x), x1 = std::max(a.x, b.x);
    const float y0 = std::min(a.y, b.y), y1 = std::max(a.y, b.y);
    if (!additive) m_sel.clear();
    int added = 0;
    for (int i = 0; i < static_cast<int>(m_asset.cameraKeys.size()); ++i) {
        const float px = pxOfT(p, m_asset.cameraKeys[static_cast<std::size_t>(i)].t);
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

bool CurveEditorPanel::reject(const char* op, const std::string& reason) {
    m_uiError = std::string(TR("Rejected")) + ": " + reason;
    VK_LOG_WARN("CURVED-REJECT op=%s reason=%s", op, reason.c_str());
    return false;
}

void CurveEditorPanel::sortAndRemap() {
    const int n = static_cast<int>(m_asset.cameraKeys.size());
    if (n < 2) return;
    std::vector<int> idx(static_cast<std::size_t>(n));
    std::iota(idx.begin(), idx.end(), 0);
    std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) {
        return m_asset.cameraKeys[static_cast<std::size_t>(a)].t <
               m_asset.cameraKeys[static_cast<std::size_t>(b)].t;
    });
    bool changed = false;
    for (int i = 0; i < n; ++i) {
        if (idx[static_cast<std::size_t>(i)] != i) {
            changed = true;
            break;
        }
    }
    if (!changed) return;

    std::vector<sequence::SeqCameraKey> nk(static_cast<std::size_t>(n));
    std::vector<int> newOfOld(static_cast<std::size_t>(n), 0);
    for (int i = 0; i < n; ++i) {
        const int old = idx[static_cast<std::size_t>(i)];
        nk[static_cast<std::size_t>(i)] = m_asset.cameraKeys[static_cast<std::size_t>(old)];
        newOfOld[static_cast<std::size_t>(old)] = i;
    }
    m_asset.cameraKeys.swap(nk);
    for (int& s : m_sel) s = newOfOld[static_cast<std::size_t>(s)];
    std::sort(m_sel.begin(), m_sel.end());
    if (m_primary >= 0) m_primary = newOfOld[static_cast<std::size_t>(m_primary)];
    m_curveStamp.clear();
    m_keyStamp.clear();
    VK_LOG_INFO("CURVED-REORDER n=%d primary=%d", n, m_primary);
}

bool CurveEditorPanel::actSetKey(int i, float t, float v, bool reorder) {
    rebuildCurve();
    const int n = static_cast<int>(m_asset.cameraKeys.size());
    if (i < 0 || i >= n) return reject("setkey", TR("no camera key selected"));
    t = std::clamp(t, 0.0f, durationOf());
    m_asset.cameraKeys[static_cast<std::size_t>(i)].t = t;
    writeKeyValue(i, v);
    markDirty();
    // 重排会让下标变，`m_primary` 跟着换算 —— 不然拖动到一半选中的就换人了
    if (reorder) {
        m_primary = i;
        sortAndRemap();
    }
    return true;
}

bool CurveEditorPanel::actDragKeysPx(float dx, float dy) {
    if (m_drag.kind != DragKind::Key || m_drag.moving.empty()) return false;
    const float dt = dx / pxPerSec(m_plot);
    const float dv = -dy / pxPerUnit(m_plot);
    bool changed = false;
    for (std::size_t k = 0; k < m_drag.moving.size(); ++k) {
        const int idx = m_drag.moving[k];
        if (idx < 0 || idx >= static_cast<int>(m_asset.cameraKeys.size())) continue;
        const float nt = std::clamp(m_drag.origT[k] + dt, 0.0f, durationOf());
        const float nv = m_drag.origV[k] + dv;
        if (nt != m_asset.cameraKeys[static_cast<std::size_t>(idx)].t ||
            nv != m_drag.origV[k])
            changed = true;
        // ⚠ 拖动中不重排（③）。这里直接写字段，绕开 actSetKey 的 sortAndRemap。
        m_asset.cameraKeys[static_cast<std::size_t>(idx)].t = nt;
        writeKeyValue(idx, nv);
    }
    if (std::fabs(dt) > 1.0e-6f) m_drag.timeMoved = true;
    if (changed) {
        m_drag.moved = true;
        markDirty();
    }
    return changed;
}

bool CurveEditorPanel::actSetHandle(int i, bool isOut, glm::vec2 norm) {
    const int n = static_cast<int>(m_asset.cameraKeys.size());
    if (i < 0 || i >= n) return reject("sethandle", TR("no camera key selected"));
    sequence::SeqCameraKey& k = m_asset.cameraKeys[static_cast<std::size_t>(i)];
    if (isOut) {
        // ⚠ 符号约束（见 SeqCameraKey 的 ⚠⚠）：out.x >= 0、in.x <= 0。
        //   允许拖到反面的话，这段贝塞尔在 x 上不再单调，求根会解出两个 u
        //   —— 表现是"镜头在某一刻往回调了一下"，日志里什么都看不出来。
        k.outHandle.x = std::clamp(norm.x, 0.0f, 1.0f);
        k.outHandle.y = std::clamp(norm.y, -2.0f, 3.0f);
    } else {
        k.inHandle.x = std::clamp(norm.x, -1.0f, 0.0f);
        k.inHandle.y = std::clamp(norm.y, -2.0f, 3.0f);
    }
    m_curveStamp.clear();
    markDirty();
    return true;
}

bool CurveEditorPanel::actDragHandlePx(float dx, float dy) {
    if (m_drag.kind != DragKind::HandleOut && m_drag.kind != DragKind::HandleIn)
        return false;
    const int i = m_drag.index;
    if (i < 0 || i + 1 >= static_cast<int>(m_asset.cameraKeys.size())) return false;
    const bool isOut = (m_drag.kind == DragKind::HandleOut);
    const float span = std::max(m_asset.cameraKeys[static_cast<std::size_t>(i) + 1].t -
                                    m_asset.cameraKeys[static_cast<std::size_t>(i)].t,
                                1.0e-3f);
    const float dv = keyValue(i + 1) - keyValue(i);

    glm::vec2 h = m_drag.handleOrig;
    h.x += (dx / pxPerSec(m_plot)) / span;
    // ⚠ dv ≈ 0 时 y 换算不出来（这一段该通道值没变，进度比例乘 0 永远是 0）。
    //   这时只允许改 x，并在日志里说明一次 —— 换成别的通道就能拖了。
    if (std::fabs(dv) > 1.0e-5f)
        h.y += (-dy / pxPerUnit(m_plot)) / dv;
    else if (std::fabs(dy) > 1.0f)
        VK_LOG_INFO("CURVED-HANDLE-FLAT ch=%s i=%d dv=0（这一段该通道没变化，"
                    "手柄的 y 无法表达）", channelId(m_channel), i);
    const bool ok = actSetHandle(i, isOut, h);
    if (ok) m_drag.moved = true;
    return ok;
}

bool CurveEditorPanel::actSetEase(int i, sequence::Ease e) {
    const int n = static_cast<int>(m_asset.cameraKeys.size());
    if (i < 0 || i >= n) return reject("setease", TR("no camera key selected"));
    m_asset.cameraKeys[static_cast<std::size_t>(i)].ease = e;
    m_curveStamp.clear();
    m_keyStamp.clear();
    markDirty();
    animation::Interp ip = animation::Interp::Linear;
    animation::EaseMode em = animation::EaseMode::None;
    animation::EaseShape sh = animation::EaseShape::Linear;
    mapEaseToCurve(e, ip, em, sh);
    VK_LOG_INFO("CURVED-SETEASE i=%d ease=%s interp=%s easeMode=%s shape=%s",
                i, sequence::easeName(e), animation::interpName(ip),
                animation::easeModeName(em), animation::easeShapeName(sh));
    return true;
}

bool CurveEditorPanel::actCycleEase() {
    if (m_primary < 0) return reject("cycleease", TR("no camera key selected"));
    const sequence::Ease cur =
        m_asset.cameraKeys[static_cast<std::size_t>(m_primary)].ease;
    int idx = 0;
    for (int i = 0; i < 4; ++i) {
        sequence::Ease e = sequence::Ease::Smooth;
        if (sequence::parseEase(easeCycleName(i), e) && e == cur) {
            idx = i;
            break;
        }
    }
    sequence::Ease next = sequence::Ease::Smooth;
    if (!sequence::parseEase(easeCycleName(idx + 1), next))
        return reject("cycleease", TR("cannot map the next ease"));
    return actSetEase(m_primary, next);
}

// V：按类型**换算并写回**（⑤ —— 不存类型，因为一个段只有一组手柄）。
//
// 归一化的 y 怎么来：先把"绝对斜率 s（值/秒）"算出来，再折成
// `y = s · (段长/3) / dv`。三个特例：
//   · `dv ≈ 0` → y 无意义，写 0（该通道这一段是平的）；
//   · `Vector` → 手柄指向另一个端点，归一化正好是 (±1, ±1)；
//   · `Free` → 什么都不做（作者写的数值就是答案）。
bool CurveEditorPanel::actSetHandleType(int i, animation::HandleType type) {
    rebuildCurve();
    const int n = static_cast<int>(m_asset.cameraKeys.size());
    if (i < 0 || i >= n) return reject("setht", TR("no camera key selected"));
    if (type == animation::HandleType::Free) {
        VK_LOG_INFO("CURVED-SETHANDLETYPE i=%d type=free（不换算，保留现有数值）", i);
        return true;
    }
    if (i + 1 >= n) {
        // 最后一帧没有"出段"，Auto/Vector 算不出邻居。
        return reject("setht", TR("the last key has no outgoing segment"));
    }
    const float t0 = m_asset.cameraKeys[static_cast<std::size_t>(i)].t;
    const float t1 = m_asset.cameraKeys[static_cast<std::size_t>(i) + 1].t;
    const float span = std::max(t1 - t0, 1.0e-3f);
    const float v0 = keyValue(i);
    const float v1 = keyValue(i + 1);
    const float dv = v1 - v0;

    glm::vec2 out{1.0f / 3.0f, 0.0f};
    glm::vec2 in{-1.0f / 3.0f, 0.0f};
    if (type == animation::HandleType::Vector) {
        out = glm::vec2(1.0f, 1.0f);
        in = glm::vec2(-1.0f, -1.0f);
    } else if (type == animation::HandleType::Aligned) {
        // 与 out 共线，长度沿用现有的 in（"方向对齐、长度我自己定"）
        const glm::vec2 o = m_asset.cameraKeys[static_cast<std::size_t>(i)].outHandle;
        const float len = glm::length(m_asset.cameraKeys[static_cast<std::size_t>(i)].inHandle);
        in = (glm::length(o) > 1.0e-6f) ? -glm::normalize(o) * std::max(len, 0.05f)
                                        : glm::vec2(-1.0f / 3.0f, 0.0f);
        out = o;
    } else {
        // Auto / AutoClamped：中心差分斜率（端点退化成单侧差分）
        const float tp = (i > 0) ? m_asset.cameraKeys[static_cast<std::size_t>(i) - 1].t : t0;
        const float vp = (i > 0) ? keyValue(i - 1) : v0;
        const float sPrev = (i > 0) ? (v0 - vp) / std::max(t0 - tp, 1.0e-3f) : 0.0f;
        const float sNext = dv / span;
        float s = 0.5f * (sPrev + sNext);
        if (i == 0) s = sNext;
        if (type == animation::HandleType::AutoClamped && sPrev * sNext <= 0.0f)
            s = 0.0f;   // 局部极值 → 压平，不过冲
        if (std::fabs(dv) > 1.0e-5f) {
            out = glm::vec2(1.0f / 3.0f, (s * span / 3.0f) / dv);
            in = glm::vec2(-1.0f / 3.0f, (s * span / 3.0f) / dv);
        } else {
            out = glm::vec2(1.0f / 3.0f, 0.0f);
            in = glm::vec2(-1.0f / 3.0f, 0.0f);
        }
    }

    actSetHandle(i, true, out);
    actSetHandle(i, false, in);
    const sequence::SeqCameraKey& k = m_asset.cameraKeys[static_cast<std::size_t>(i)];
    VK_LOG_INFO("CURVED-SETHANDLETYPE i=%d type=%s ch=%s out=%.3f,%.3f in=%.3f,%.3f",
                i, animation::handleTypeName(type), channelId(m_channel),
                static_cast<double>(k.outHandle.x), static_cast<double>(k.outHandle.y),
                static_cast<double>(k.inHandle.x), static_cast<double>(k.inHandle.y));
    return true;
}

bool CurveEditorPanel::actCycleHandleType() {
    if (m_primary < 0) return reject("cycleht", TR("no camera key selected"));
    // 顺序：auto → auto_clamped → vector → aligned → free → auto。
    // ⚠ 计数器是**成员**不是函数内 static：static 会在关窗再开之后接着
    //   上次的位置走，而"我关掉重开想从第一档开始"是作者的真实预期。
    ++m_htCycle;
    animation::HandleType t = animation::HandleType::Auto;
    if (!animation::parseHandleType(handleTypeCycleName(m_htCycle), t))
        return reject("cycleht", TR("cannot map the next handle type"));
    // Free 是"什么都不做"，循环到它时再往前走一格 —— 否则按 V 会有一档
    // 看起来"按了没反应"。
    if (t == animation::HandleType::Free) {
        ++m_htCycle;
        if (!animation::parseHandleType(handleTypeCycleName(m_htCycle), t))
            return reject("cycleht", TR("cannot map the next handle type"));
    }
    return actSetHandleType(m_primary, t);
}

bool CurveEditorPanel::actSetHead(float t) {
    m_head = std::clamp(t, 0.0f, std::max(m_asset.duration, 0.0f));
    return true;
}

bool CurveEditorPanel::actSetChannel(Channel c) {
    if (c == m_channel) return true;
    m_channel = c;
    m_curveStamp.clear();
    m_keyStamp.clear();
    fitValueRange();
    VK_LOG_INFO("CURVED-CHANNEL name=%s keys=%d", channelId(c),
                static_cast<int>(m_asset.cameraKeys.size()));
    return true;
}

// ============================================================
// 手势
// ============================================================

void CurveEditorPanel::beginDrag(const Plot& p, ImVec2 mouse, bool additive) {
    rebuildCurve();
    m_drag = DragState{};
    const int n = static_cast<int>(m_asset.cameraKeys.size());

    // ① 关键帧点（当前通道）
    int hit = -1;
    float best = kKeyHitPx;
    for (int i = 0; i < n; ++i) {
        const float px = pxOfT(p, m_asset.cameraKeys[static_cast<std::size_t>(i)].t);
        const float py = pyOfV(p, keyValue(i));
        const float d = std::hypot(mouse.x - px, mouse.y - py);
        if (d <= best) {
            best = d;
            hit = i;
        }
    }
    if (hit >= 0) {
        // ⚠ 这里**不能**无条件 `actSelect(hit, false)`：它的语义是"清空再选中"
        //   （见 actSelect 的 else 分支），于是框选三帧之后往其中一帧上一按，
        //   选择立刻坍成 1 个 → `m_drag.moving` 只剩它 → 批量拖永远只能拖一帧。
        //   这个 bug 不会报错、不会崩，只是"多选拖不动"，最容易漏。
        //   规则（与所有曲线编辑器一致）：
        //     · 按住**未选中**的帧 → 选择坍缩到它（非加选）或追加（加选）
        //     · 按住**已选中**的帧 → 保留整组，拖的是这一组
        //     · Shift + 已选中的帧 → 减选（actSelect 的 additive 分支）
        if (additive) {
            actSelect(hit, true);
        } else if (!isSelected(hit)) {
            actSelect(hit, false);
        } else {
            m_primary = hit;        // 只是把"主帧"换过去，选择集合不动
        }
        if (!isSelected(hit)) {
            // Shift 把刚按下的这一帧减掉了：没有任何东西可拖。
            VK_LOG_INFO("CURVED-DRAG-NOTE kind=key i=%d deselected", hit);
            m_drag = DragState{};
            return;
        }
        m_drag.kind = DragKind::Key;
        m_drag.index = hit;
        m_drag.startMouse = mouse;
        const float px = pxOfT(p, m_asset.cameraKeys[static_cast<std::size_t>(hit)].t);
        const float py = pyOfV(p, keyValue(hit));
        m_drag.grab = glm::vec2(mouse.x - px, mouse.y - py);
        // 批量：选中集合整体平移。用"原始值 + 同一个增量"，相对间距不会漂。
        m_drag.moving = m_sel;
        if (m_drag.moving.empty()) m_drag.moving.assign(1, hit);
        for (int idx : m_drag.moving) {
            m_drag.origT.push_back(m_asset.cameraKeys[static_cast<std::size_t>(idx)].t);
            m_drag.origV.push_back(keyValue(idx));
        }
        VK_LOG_INFO("CURVED-DRAG-BEGIN kind=key i=%d n=%d", hit,
                    static_cast<int>(m_drag.moving.size()));
        return;
    }

    // ② 手柄（只对**选中帧的出段**显示，所以也只可能选中这些）
    for (int i : m_sel) {
        if (i + 1 >= n) continue;
        const sequence::SeqCameraKey& a = m_asset.cameraKeys[static_cast<std::size_t>(i)];
        if (a.ease != sequence::Ease::Bezier) continue;
        const float span = std::max(
            m_asset.cameraKeys[static_cast<std::size_t>(i) + 1].t - a.t, 1.0e-3f);
        const float dv = keyValue(i + 1) - keyValue(i);
        // out：锚在本帧；in：锚在下一帧（`.seq.json` 的 inHandle 就是这么存的）
        const float ox = pxOfT(p, a.t + a.outHandle.x * span);
        const float oy = pyOfV(p, keyValue(i) + a.outHandle.y * dv);
        const float ix = pxOfT(p, m_asset.cameraKeys[static_cast<std::size_t>(i) + 1].t +
                                      a.inHandle.x * span);
        const float iy = pyOfV(p, keyValue(i + 1) + a.inHandle.y * dv);
        if (std::hypot(mouse.x - ox, mouse.y - oy) <= kHandleHitPx) {
            m_drag.kind = DragKind::HandleOut;
        } else if (std::hypot(mouse.x - ix, mouse.y - iy) <= kHandleHitPx) {
            m_drag.kind = DragKind::HandleIn;
        } else {
            continue;
        }
        m_drag.index = i;
        m_drag.startMouse = mouse;
        m_drag.handleOrig = a.outHandle;
        if (m_drag.kind == DragKind::HandleIn) m_drag.handleOrig = a.inHandle;
        m_primary = i;
        VK_LOG_INFO("CURVED-DRAG-BEGIN kind=%s i=%d",
                    m_drag.kind == DragKind::HandleOut ? "handleOut" : "handleIn", i);
        return;
    }

    // ③ 框选
    m_drag.kind = DragKind::Marquee;
    m_drag.startMouse = mouse;
    if (!additive) {
        m_sel.clear();
        m_primary = -1;
    }
}

void CurveEditorPanel::endDrag() {
    if (m_drag.kind == DragKind::None) return;
    const DragKind kind = m_drag.kind;
    const int i = m_drag.index;

    if (kind == DragKind::Key) {
        if (m_drag.timeMoved) sortAndRemap();
        if (m_drag.moved) {
            clampSel();
            const int p = m_primary;
            const float t = (p >= 0 && p < static_cast<int>(m_asset.cameraKeys.size()))
                                ? m_asset.cameraKeys[static_cast<std::size_t>(p)].t
                                : 0.0f;
            rebuildCurve();
            VK_LOG_INFO("CURVED-DRAG-END kind=key i=%d t=%.4f v=%.4f n=%d ch=%s", p,
                        static_cast<double>(t), static_cast<double>(keyValue(p)),
                        static_cast<int>(m_drag.moving.size()), channelId(m_channel));
        }
    } else if (kind == DragKind::HandleOut || kind == DragKind::HandleIn) {
        if (m_drag.moved && i >= 0 && i < static_cast<int>(m_asset.cameraKeys.size())) {
            const sequence::SeqCameraKey& k =
                m_asset.cameraKeys[static_cast<std::size_t>(i)];
            const glm::vec2 h = (kind == DragKind::HandleOut) ? k.outHandle : k.inHandle;
            VK_LOG_INFO("CURVED-DRAG-END kind=handle i=%d side=%s x=%.4f y=%.4f ch=%s", i,
                        kind == DragKind::HandleOut ? "out" : "in",
                        static_cast<double>(h.x), static_cast<double>(h.y),
                        channelId(m_channel));
        }
    } else if (kind == DragKind::Marquee) {
        const float dx = ImGui::GetIO().MousePos.x - m_drag.startMouse.x;
        const float dy = ImGui::GetIO().MousePos.y - m_drag.startMouse.y;
        if (std::fabs(dx) < 3.0f && std::fabs(dy) < 3.0f) {
            // 空点一下 = 取消选择（与 Blender 一致）
            if (m_sel.empty()) m_primary = -1;
        }
        VK_LOG_INFO("CURVED-DRAG-END kind=marquee n=%d primary=%d",
                    static_cast<int>(m_sel.size()), m_primary);
    }

    if (m_drag.moved) revalidate();
    m_drag = DragState{};
}

// ============================================================
// 存盘
// ============================================================

void CurveEditorPanel::revalidate() {
    sequence::SequenceAsset chk;
    const std::string text = sequence::toJson(m_asset);
    const bool parsed = sequence::parseSequence(
        text, m_path.empty() ? std::string("untitled.seq.json") : m_path, chk);
    const bool ok = parsed && chk.valid;
    const XCheck xc = crossCheck();

    char key[192];
    std::snprintf(key, sizeof(key), "%d|%d|%d|%.6g|%s", ok ? 1 : 0,
                  static_cast<int>(m_asset.cameraKeys.size()),
                  static_cast<int>(chk.warnings.size()),
                  static_cast<double>(xc.worst), ok ? "" : chk.error.c_str());
    if (m_checkLogKey != key) {
        m_checkLogKey = key;
        VK_LOG_INFO("CURVED-CHECK ok=%d keys=%d dur=%.3f warns=%d xcheck=%.6g "
                    "where=%s err=%s",
                    ok ? 1 : 0, static_cast<int>(m_asset.cameraKeys.size()),
                    static_cast<double>(m_asset.duration),
                    static_cast<int>(chk.warnings.size()),
                    static_cast<double>(xc.worst), xc.where,
                    ok ? "" : chk.error.c_str());
    }
    m_checkValid = ok;
    m_checkError = chk.error;
    m_checkWarnings = static_cast<int>(chk.warnings.size());
    m_checkXCheck = xc.worst;
}

bool CurveEditorPanel::save(bool force) {
    if (m_readOnly) {
        m_uiError = TR("Read-only: the file failed to load, saving would "
                       "truncate it. Fix it and press Reload.");
        VK_LOG_WARN("CURVED-SAVE-BLOCKED path=%s reason=read-only", m_path.c_str());
        return false;
    }
    if (m_path.empty()) {
        m_uiError = TR("Set an asset path first (e.g. sequences/opening.seq.json).");
        VK_LOG_WARN("CURVED-SAVE-BLOCKED path=(none) reason=no-path");
        return false;
    }

    // ⑥ 磁盘冲突：打开之后文件被别人（大概率是时间轴编辑器）改过。
    //    ⚠ 用 `force` 才对 —— 自动化里的 `saveforce` 就是这个逃生门。
    if (!force && m_haveStamp) {
        std::error_code ec;
        const auto now = std::filesystem::last_write_time(
            std::filesystem::path(assets::resolveAssetPath(m_path)), ec);
        if (!ec && now != m_stampDisk) {
            m_uiError = TR("Not saved — the file changed on disk after it was "
                           "opened (another editor saved it). Press Reload to take "
                           "its version, or Save anyway to overwrite it.");
            VK_LOG_WARN("CURVED-SAVE-BLOCKED path=%s reason=stale", m_path.c_str());
            return false;
        }
    }

    revalidate();
    if (!m_checkValid && !force) {
        m_uiError = std::string(TR("Not saved — the sequence does not load:")) +
                    " " + m_checkError;
        VK_LOG_WARN("CURVED-SAVE-BLOCKED path=%s reason=check err=%s",
                    m_path.c_str(), m_checkError.c_str());
        return false;
    }

    std::string err;
    if (!sequence::saveSequence(m_path, m_asset, err)) {
        m_error = err;
        m_uiError = err;
        VK_LOG_ERROR("CURVED-SAVE-FAIL path=%s err=%s", m_path.c_str(), err.c_str());
        return false;
    }

    m_dirty = false;
    m_error.clear();
    m_uiError.clear();
    refreshStamp();   // 自己写的这次不算"别人改过"
    VK_LOG_INFO("CURVED-SAVE path=%s keys=%d force=%d", m_path.c_str(),
                static_cast<int>(m_asset.cameraKeys.size()), force ? 1 : 0);
    refreshSceneComponents();
    return true;
}

// 存盘后刷新场景里引用同一份资产的组件（与 TimelineEditorWindow ⑥ 同构）。
void CurveEditorPanel::refreshSceneComponents() {
    const std::string mine = canonicalOf(m_path);
    if (mine.empty()) return;
    scene::Scene& sc = m_ctx.activeScene();
    ecs::World& w = sc.world();

    int reloaded = 0;
    int skipped = 0;
    w.each<ecs::SequencerComponent>([&](ecs::Entity e, ecs::SequencerComponent& c) {
        if (canonicalOf(c.sequencePath) != mine) return;
        if (c.started && !c.finished && c.playing) {
            // 正在播的那一段不打断：中途换运镜比"这次预览是旧的"更糟
            ++skipped;
            VK_LOG_INFO("CURVED-SCENE-SKIP entity=%u path=%s (playing)",
                        static_cast<unsigned>(e.id), c.sequencePath.c_str());
            return;
        }
        c.assetLoaded = false;
        c.loadFailedLogged = false;
        c.started = false;
        c.finished = false;
        c.playhead = 0.0f;
        ++reloaded;
    });
    VK_LOG_INFO("CURVED-SCENE-RELOAD path=%s reloaded=%d skipped=%d", m_path.c_str(),
                reloaded, skipped);
}

// ============================================================
// 绘制
// ============================================================

void CurveEditorPanel::draw(bool& open) {
    if (!m_open) return;

    ImGuiWindowFlags wf = 0;
    const bool docked = applyDockHint(wf);
    if (!docked) {
        ImGui::SetNextWindowSizeConstraints(ImVec2(720.0f, 420.0f),
                                            ImVec2(FLT_MAX, FLT_MAX));
        ImGui::SetNextWindowSize(ImVec2(1040.0f, 620.0f), ImGuiCond_FirstUseEver);
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.14f,
                                       io.DisplaySize.y * 0.08f),
                                ImGuiCond_FirstUseEver);
    }

    // ⚠ 稳定 ID 必须等于 `trayWindowName("curveed")` = `###panel_curveed`，
    //   否则底栏托盘 / 最小化按钮按 id 找不到这个窗口（#311 的 `###panel_tled`
    //   就和它自己的面板 id `timelineed` 对不上，那套功能对它静默失效）。
    std::string title = TR("Curve editor");
    if (!m_path.empty()) {
        title += " — ";
        title += m_asset.name.empty() ? nameFromPath(m_path) : m_asset.name;
        if (m_dirty) title += " *";
    }
    title += "###panel_curveed";

    if (!ImGui::Begin(title.c_str(), docked ? nullptr : &open, wf)) {
        ImGui::End();
        return;
    }
    logRect("CURVED-WIN", ImGui::GetWindowPos(),
            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                   ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    drawToolbar();
    drawBody();
    // ⚠ 注入脚本排在画布**之后**：`dragkey` 要用本帧真实的像素↔值映射
    //   （`m_plot` 是 drawCanvas 里算出来的）。排在前面的话，第一帧的
    //   映射是空的，拖出来的位移是 0。
    runInjectedScript();
    drawInspector();
    drawValidation();

    ImGui::End();
    if (!open) close();
}

void CurveEditorPanel::drawToolbar() {
    // ---- 第一行：资产路径 + Reload / Save ----
    ImGui::TextDisabled("%s", TR("Asset path"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(340.0f);
    if (ImGui::InputTextWithHint("##curved_path", "sequences/opening.seq.json",
                                 m_pathBuf, sizeof(m_pathBuf),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
        // 改路径**不自动读盘**（半截路径会立刻报"打不开"），回车才读。
        m_path = m_pathBuf;
        reload();
    }
    logRect("CURVED-PATH", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    ImGui::SameLine();
    if (ImGui::Button(TR("Reload"))) {
        m_path = m_pathBuf;
        reload();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", TR("Re-read from disk and drop unsaved changes."));

    ImGui::SameLine();
    ImGui::BeginDisabled(m_readOnly);
    if (ImGui::Button(TR("Save"))) save(false);
    logRect("CURVED-SAVE", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::EndDisabled();

    if (!m_checkValid && !m_readOnly) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.28f, 0.18f, 1.0f));
        if (ImGui::Button(TR("Save anyway"))) save(true);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", TR("Write even though the sequence does "
                                       "not load. Use it for work in progress."));
    }
    if (m_dirty) {
        ImGui::SameLine();
        ImGui::TextColored(kWarnCol, "%s", TR("unsaved"));
    }

    // ---- 第二行：通道 / 光标 / 适配 ----
    ImGui::SetNextItemWidth(150.0f);
    if (ImGui::BeginCombo(TR("Channel"), channelId(m_channel))) {
        for (int i = 0; i < static_cast<int>(Channel::Count); ++i) {
            const Channel c = static_cast<Channel>(i);
            const bool sel = (c == m_channel);
            if (ImGui::Selectable(channelId(c), sel)) actSetChannel(c);
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    logRect("CURVED-CHANNEL", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", TR("The canvas draws one scalar channel at a time. "
                                   "The handles belong to the segment, not to the "
                                   "channel: dragging one retimes all four "
                                   "position/yaw/pitch/fov channels at once."));

    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    float head = m_head;
    if (ImGui::DragFloat(TR("Cursor"), &head, 0.01f, 0.0f,
                         std::max(m_asset.duration, 0.1f), TR("%.3f s"))) {
        actSetHead(head);
    }
    logRect("CURVED-HEAD", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", TR("Scrubbing only samples; it does not play."));

    ImGui::SameLine();
    if (ImGui::Button(TR("Fit"))) {
        resetView();
        VK_LOG_INFO("CURVED-FIT t0=%.3f t1=%.3f v0=%.4f v1=%.4f",
                    static_cast<double>(m_t0), static_cast<double>(m_t1),
                    static_cast<double>(m_v0), static_cast<double>(m_v1));
    }
    logRect("CURVED-FIT", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    if (m_readOnly) {
        ImGui::SameLine();
        ImGui::TextColored(kBadCol, "%s", TR("read-only"));
    }
    if (!m_uiError.empty()) {
        ImGui::TextColored(kBadCol, "%s", m_uiError.c_str());
    }
}

void CurveEditorPanel::drawBody() {
    const float listW = 150.0f;
    // 侧栏 3 行 + 校验 1 行 ≈ 5 个行高。留够，别让画布被挤成一条。
    const float reserve = 5.2f * ImGui::GetFrameHeightWithSpacing();
    const float bodyH = std::max(ImGui::GetContentRegionAvail().y - reserve, 180.0f);

    ImGui::BeginChild("##curved_list", ImVec2(listW, bodyH), ImGuiChildFlags_Borders);
    drawChannelList();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##curved_plot", ImVec2(0.0f, bodyH), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
    drawCanvas();
    ImGui::EndChild();
}

void CurveEditorPanel::drawChannelList() {
    ImGui::TextDisabled("%s", TR("Channels"));
    ImGui::Separator();
    const int n = static_cast<int>(m_asset.cameraKeys.size());
    for (int i = 0; i < static_cast<int>(Channel::Count); ++i) {
        const Channel c = static_cast<Channel>(i);
        ImGui::PushID(i);
        const bool sel = (c == m_channel);
        // 低对比度的一行小字：名字 + 单位 + 这一通道的键数
        if (ImGui::Selectable(channelId(c), sel)) actSetChannel(c);
        if (channelIsDegrees(c)) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", TR("deg"));
        }
        ImGui::PopID();
    }
    ImGui::Separator();
    ImGui::TextDisabled("%s %d", TR("keys"), n);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", TR("Camera keys in this sequence. This panel edits "
                                   "them; a new key still comes from the timeline "
                                   "editor (one owner per operation)."));
}

// ---------------------------------------------------------------- 画布

void CurveEditorPanel::drawCanvas() {
    rebuildCurve();

    ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 size(std::max(avail.x, 240.0f), std::max(avail.y, 160.0f));
    ImGui::InvisibleButton("##curved_canvas", size);
    const ImVec2 itemMin = ImGui::GetItemRectMin();
    const ImVec2 itemMax = ImGui::GetItemRectMax();
    logRect("CURVED-CANVAS", itemMin, itemMax);

    Plot p;
    p.rectMin = itemMin;
    p.rectMax = itemMax;
    // 左边留 46px 给值刻度，下边留 20px 给时间刻度
    p.min = ImVec2(itemMin.x + 46.0f, itemMin.y + 8.0f);
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
                  static_cast<double>(p.min.x), static_cast<double>(p.max.x),
                  static_cast<double>(p.min.y), static_cast<double>(p.max.y));
    if (m_mapKey != mapk) {
        m_mapKey = mapk;
        VK_LOG_INFO("CURVED-MAP t0=%.4f t1=%.4f v0=%.4f v1=%.4f px0=%.1f px1=%.1f "
                    "py0=%.1f py1=%.1f ch=%s",
                    static_cast<double>(p.t0), static_cast<double>(p.t1),
                    static_cast<double>(p.v0), static_cast<double>(p.v1),
                    static_cast<double>(p.min.x), static_cast<double>(p.max.x),
                    static_cast<double>(p.min.y), static_cast<double>(p.max.y),
                    channelId(m_channel));
    }

    // ---- 交互 ----
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemHovered() && io.MouseWheel != 0.0f) {
        zoomTime(io.MousePos.x, std::exp(-io.MouseWheel * 0.15f));
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        beginDrag(p, io.MousePos, io.KeyShift);
    }
    if (m_drag.kind != DragKind::None) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const float dx = io.MousePos.x - m_drag.startMouse.x;
            const float dy = io.MousePos.y - m_drag.startMouse.y;
            switch (m_drag.kind) {
            case DragKind::Key: actDragKeysPx(dx, dy); break;
            case DragKind::HandleOut:
            case DragKind::HandleIn: actDragHandlePx(dx, dy); break;
            case DragKind::Marquee: actSelectRect(p, m_drag.startMouse, io.MousePos, io.KeyShift); break;
            default: break;
            }
        } else {
            endDrag();
        }
    }

    // ---- 画 ----
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 colBg = ImGui::GetColorU32(ImGuiCol_ChildBg, 0.35f);
    const ImU32 colGrid = ImGui::GetColorU32(ImGuiCol_Text, 0.09f);
    const ImU32 colAxis = ImGui::GetColorU32(ImGuiCol_Text, 0.35f);
    const ImU32 colDim = ImGui::GetColorU32(ImGuiCol_Text, 0.55f);
    const ImU32 colCurve = IM_COL32(126, 200, 255, 255);
    const ImU32 colKey = IM_COL32(120, 190, 255, 255);
    const ImU32 colKeySel = IM_COL32(255, 210, 90, 255);
    const ImU32 colOutH = IM_COL32(255, 150, 70, 255);
    const ImU32 colInH = IM_COL32(120, 235, 190, 255);
    const ImU32 colHead = IM_COL32(255, 255, 255, 90);

    dl->AddRectFilled(p.min, p.max, colBg);

    // 网格：横向 4 段、纵向按时间自适应刻度
    for (int i = 1; i < 4; ++i) {
        const float y = p.min.y + (p.max.y - p.min.y) * static_cast<float>(i) / 4.0f;
        dl->AddLine(ImVec2(p.min.x, y), ImVec2(p.max.x, y), colGrid);
    }
    {
        float step = 0.5f;
        const float span = std::max(p.t1 - p.t0, 1.0e-4f);
        while (pxPerSec(p) * step < 60.0f) step *= 2.0f;
        const float start = std::ceil(p.t0 / step) * step;
        for (float t = start; t <= p.t1 + 1.0e-4f; t += step) {
            const float x = pxOfT(p, t);
            dl->AddLine(ImVec2(x, p.min.y), ImVec2(x, p.max.y), colGrid);
            char lbl[32];
            std::snprintf(lbl, sizeof(lbl), "%.2gs", static_cast<double>(t));
            dl->AddText(ImVec2(x + 2.0f, p.max.y + 3.0f), colDim, lbl);
        }
        (void)span;
    }
    // 值刻度（只要上下两个端点，中间靠网格）
    {
        char lbl[32];
        std::snprintf(lbl, sizeof(lbl), "%.3g", static_cast<double>(p.v1));
        dl->AddText(ImVec2(p.min.x + 2.0f, p.min.y), colDim, lbl);
        std::snprintf(lbl, sizeof(lbl), "%.3g", static_cast<double>(p.v0));
        dl->AddText(ImVec2(p.min.x + 2.0f, p.max.y - ImGui::GetFontSize()), colDim, lbl);
    }
    dl->AddRect(p.min, p.max, colAxis);
    // 关键帧范围之外的**外推**（现在是 Hold）用底色标出来，一眼能看出
    // "这两截不是插值出来的，是曲线自己的外推"。
    {
        const float tFirst = m_asset.cameraKeys.empty() ? 0.0f : m_asset.cameraKeys.front().t;
        const float tLast = m_asset.cameraKeys.empty() ? 0.0f : m_asset.cameraKeys.back().t;
        const ImU32 colOut = ImGui::GetColorU32(ImGuiCol_Text, 0.04f);
        if (pxOfT(p, tFirst) > p.min.x)
            dl->AddRectFilled(p.min, ImVec2(pxOfT(p, tFirst), p.max.y), colOut);
        if (pxOfT(p, tLast) < p.max.x)
            dl->AddRectFilled(ImVec2(pxOfT(p, tLast), p.min.y), p.max, colOut);
    }

    // ---- 折线：按像素列采样 `evaluateCurve`（①）----
    if (!m_curve.keys.empty()) {
        const int cols = std::max(static_cast<int>(p.max.x - p.min.x), 2);
        ImVec2 prev(0.0f, 0.0f);
        for (int i = 0; i <= cols; ++i) {
            const float x = p.min.x + static_cast<float>(i);
            const float t = tOfPx(p, x);
            const ImVec2 cur(x, pyOfV(p, valueAt(t)));
            if (i > 0) {
                // 外推段用虚线（读作"这一段不是插值出来的"）
                const bool outside = (t < m_asset.cameraKeys.front().t) ||
                                     (t > m_asset.cameraKeys.back().t);
                if (outside) {
                    const float dash = 4.0f;
                    if (std::fmod(x, dash * 2.0f) < dash)
                        dl->AddLine(prev, cur, colCurve, 1.2f);
                } else {
                    dl->AddLine(prev, cur, colCurve, 1.8f);
                }
            }
            prev = cur;
        }
    }

    // ---- 手柄（只画**选中帧的出段**）----
    for (int i : m_sel) {
        if (i < 0 || i + 1 >= static_cast<int>(m_asset.cameraKeys.size())) continue;
        const sequence::SeqCameraKey& a = m_asset.cameraKeys[static_cast<std::size_t>(i)];
        if (a.ease != sequence::Ease::Bezier) continue;
        const sequence::SeqCameraKey& b = m_asset.cameraKeys[static_cast<std::size_t>(i) + 1];
        const float span = std::max(b.t - a.t, 1.0e-3f);
        const float dv = keyValue(i + 1) - keyValue(i);
        const ImVec2 k0(pxOfT(p, a.t), pyOfV(p, keyValue(i)));
        const ImVec2 k1(pxOfT(p, b.t), pyOfV(p, keyValue(i + 1)));
        const ImVec2 ho(pxOfT(p, a.t + a.outHandle.x * span),
                        pyOfV(p, keyValue(i) + a.outHandle.y * dv));
        const ImVec2 hi(pxOfT(p, b.t + a.inHandle.x * span),
                        pyOfV(p, keyValue(i + 1) + a.inHandle.y * dv));
        dl->AddLine(k0, ho, colOutH, 1.2f);
        dl->AddLine(k1, hi, colInH, 1.2f);
        dl->AddCircleFilled(ho, 4.0f, colOutH);
        dl->AddCircleFilled(hi, 4.0f, colInH);
    }

    // ---- 关键帧点 ----
    std::string ksk;
    for (int i = 0; i < static_cast<int>(m_asset.cameraKeys.size()); ++i) {
        const sequence::SeqCameraKey& k = m_asset.cameraKeys[static_cast<std::size_t>(i)];
        const float x = pxOfT(p, k.t);
        const float y = pyOfV(p, keyValue(i));
        const bool sel = isSelected(i);
        const float r = (i == m_primary) ? 6.5f : (sel ? 5.5f : 4.5f);
        dl->AddCircleFilled(ImVec2(x, y), r, sel ? colKeySel : colKey);
        dl->AddCircle(ImVec2(x, y), r, IM_COL32(20, 20, 24, 220), 0, 1.2f);
        char b[96];
        std::snprintf(b, sizeof(b), "%d:%.4f:%.4f:%d;", i,
                      static_cast<double>(k.t),
                      static_cast<double>(keyValue(i)), sel ? 1 : 0);
        ksk += b;
    }
    // `logRect` 的格式带不了 t/v，所以关键帧点自己打 —— 但同样要**变了才打**，
    // 不然 150 帧就是几百行噪声。
    if (ksk != m_keyStamp) {
        m_keyStamp = ksk;
        for (int i = 0; i < static_cast<int>(m_asset.cameraKeys.size()); ++i) {
            const sequence::SeqCameraKey& k =
                m_asset.cameraKeys[static_cast<std::size_t>(i)];
            VK_LOG_INFO("CURVED-KEY i=%d t=%.4f v=%.4f px=%.1f py=%.1f sel=%d",
                        i, static_cast<double>(k.t), static_cast<double>(keyValue(i)),
                        static_cast<double>(pxOfT(p, k.t)),
                        static_cast<double>(pyOfV(p, keyValue(i))),
                        isSelected(i) ? 1 : 0);
        }
    }

    // ---- 光标 ----
    {
        const float x = pxOfT(p, m_head);
        if (x >= p.min.x - 1.0f && x <= p.max.x + 1.0f) {
            dl->AddLine(ImVec2(x, p.min.y), ImVec2(x, p.max.y), colHead, 1.0f);
            if (!m_curve.keys.empty())
                dl->AddCircleFilled(ImVec2(x, pyOfV(p, valueAt(m_head))), 3.0f,
                                    IM_COL32(255, 255, 255, 200));
        }
    }

    // ---- 框选矩形 ----
    if (m_drag.kind == DragKind::Marquee && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        dl->AddRectFilled(ImVec2(std::min(m_drag.startMouse.x, io.MousePos.x),
                                 std::min(m_drag.startMouse.y, io.MousePos.y)),
                          ImVec2(std::max(m_drag.startMouse.x, io.MousePos.x),
                                 std::max(m_drag.startMouse.y, io.MousePos.y)),
                          ImGui::GetColorU32(ImGuiCol_Text, 0.08f));
        dl->AddRect(ImVec2(std::min(m_drag.startMouse.x, io.MousePos.x),
                           std::min(m_drag.startMouse.y, io.MousePos.y)),
                    ImVec2(std::max(m_drag.startMouse.x, io.MousePos.x),
                           std::max(m_drag.startMouse.y, io.MousePos.y)),
                    ImGui::GetColorU32(ImGuiCol_Text, 0.45f));
    }
}

// ---------------------------------------------------------------- 侧栏

void CurveEditorPanel::drawInspector() {
    ImGui::Separator();
    const int n = static_cast<int>(m_asset.cameraKeys.size());
    if (m_primary < 0 || m_primary >= n) {
        ImGui::TextDisabled("%s", TR("No key selected — click a key on the canvas, "
                                     "or drag a box across several."));
        return;
    }

    ImGui::TextDisabled("%s #%d / %d  (%s)", TR("selected"), m_primary + 1, n,
                        channelId(m_channel));
    // ⚠ 手柄是**段**的属性，不是通道的：侧栏这一行必须说清楚，否则作者会
    //   以为"我改的是 yaw 的手柄"（②）。
    ImGui::SameLine();
    ImGui::TextDisabled("| %s", TR("handles retime all channels"));

    sequence::SeqCameraKey& k = m_asset.cameraKeys[static_cast<std::size_t>(m_primary)];

    // t
    {
        float t = k.t;
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::DragFloat(TR("t"), &t, 0.01f, 0.0f, durationOf(), TR("%.3f s"))) {
            actSetKey(m_primary, t, keyValue(m_primary), true);
        }
        logRect("CURVED-PROP-T", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    }
    ImGui::SameLine();
    {
        float v = keyValue(m_primary);
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::DragFloat(TR("value"), &v, 0.01f, 0.0f, 0.0f, "%.4f")) {
            actSetKey(m_primary, m_asset.cameraKeys[static_cast<std::size_t>(m_primary)].t,
                      v, true);
        }
        logRect("CURVED-PROP-V", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    }

    // interp（T）
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::BeginCombo(TR("interp"), sequence::easeName(k.ease))) {
        static const sequence::Ease kAll[4] = {
            sequence::Ease::Linear, sequence::Ease::Smooth, sequence::Ease::Step,
            sequence::Ease::Bezier};
        for (sequence::Ease e : kAll) {
            const bool sel = (k.ease == e);
            if (ImGui::Selectable(sequence::easeName(e), sel))
                actSetEase(m_primary, e);
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    logRect("CURVED-PROP-EASE", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", TR("linear / smooth / step / bezier. T cycles them. "
                                   "step is the curve core's 'constant', smooth is "
                                   "'linear + ease in-out'."));

    // 手柄（V）
    ImGui::SameLine();
    ImGui::BeginDisabled(k.ease != sequence::Ease::Bezier);
    if (ImGui::Button(TR("V: fit handle"))) actCycleHandleType();
    ImGui::EndDisabled();
    logRect("CURVED-PROP-HT", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s",
                          TR("Recompute both handles of this segment from a rule "
                             "(auto / auto clamped / vector / aligned). The result is "
                             "written as numbers: this data has no handle-type field, "
                             "because the whole segment shares one pair of handles."));

    if (k.ease == sequence::Ease::Bezier) {
        ImGui::SetNextItemWidth(110.0f);
        float ox = k.outHandle.x;
        if (ImGui::DragFloat(TR("out.x"), &ox, 0.002f, 0.0f, 1.0f, "%.3f")) {
            actSetHandle(m_primary, true, glm::vec2(ox, k.outHandle.y));
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        float oy = k.outHandle.y;
        if (ImGui::DragFloat(TR("out.y"), &oy, 0.005f, -2.0f, 3.0f, "%.3f")) {
            actSetHandle(m_primary, true, glm::vec2(k.outHandle.x, oy));
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            const sequence::SeqCameraKey& kk =
                m_asset.cameraKeys[static_cast<std::size_t>(m_primary)];
            VK_LOG_INFO("CURVED-SETHANDLE i=%d side=out x=%.4f y=%.4f", m_primary,
                        static_cast<double>(kk.outHandle.x),
                        static_cast<double>(kk.outHandle.y));
            revalidate();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        float ix = k.inHandle.x;
        if (ImGui::DragFloat(TR("in.x"), &ix, 0.002f, -1.0f, 0.0f, "%.3f")) {
            actSetHandle(m_primary, false, glm::vec2(ix, k.inHandle.y));
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        float iy = k.inHandle.y;
        if (ImGui::DragFloat(TR("in.y"), &iy, 0.005f, -2.0f, 3.0f, "%.3f")) {
            actSetHandle(m_primary, false, glm::vec2(k.inHandle.x, iy));
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            const sequence::SeqCameraKey& kk =
                m_asset.cameraKeys[static_cast<std::size_t>(m_primary)];
            VK_LOG_INFO("CURVED-SETHANDLE i=%d side=in x=%.4f y=%.4f", m_primary,
                        static_cast<double>(kk.inHandle.x),
                        static_cast<double>(kk.inHandle.y));
            revalidate();
        }
    }
}

void CurveEditorPanel::drawValidation() {
    ImGui::Separator();
    if (!m_checkValid) {
        ImGui::TextColored(kBadCol, "%s %s", TR("Load check failed:"),
                           m_checkError.c_str());
    } else {
        ImGui::TextColored(kOkCol, "%s", TR("Round-trip OK"));
    }
    ImGui::SameLine();
    // ① 的可见证据：画布投影与 `sampleCamera` 的最大偏差
    const bool xok = m_checkXCheck < 1.0e-4f;
    ImGui::TextColored(xok ? kOkCol : kBadCol, "| %s %.2e", TR("canvas vs runtime:"),
                       static_cast<double>(m_checkXCheck));
    if (!m_error.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(kBadCol, "| %s", m_error.c_str());
    }
}

// ============================================================
// 注入脚本（`MYVK_EDITOR_CURVED_SCRIPT`）
// ============================================================

void CurveEditorPanel::runInjectedScript() {
    if (m_injected || !m_loaded) return;
    const char* raw = std::getenv("MYVK_EDITOR_CURVED_SCRIPT");
    if (!raw || !raw[0]) return;
    m_injected = true;

    int applied = 0;
    int failed = 0;
    const std::string all(raw);
    std::size_t pos = 0;
    while (pos <= all.size()) {
        const std::size_t semi = all.find(';', pos);
        std::string tok = all.substr(pos, semi == std::string::npos ? std::string::npos
                                                                    : semi - pos);
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
        if (name == "sel") {
            // `sel:1,2,3` / `sel:none`
            if (arg == "none") {
                m_sel.clear();
                m_primary = -1;
                ok = true;
            } else if (arg == "all") {
                ok = actSelectAll();
            } else {
                m_sel.clear();
                m_primary = -1;
                int first = -1;
                std::size_t q = 0;
                while (q <= arg.size()) {
                    const std::size_t comma = arg.find(',', q);
                    const std::string one = arg.substr(
                        q, comma == std::string::npos ? std::string::npos : comma - q);
                    q = (comma == std::string::npos) ? arg.size() + 1 : comma + 1;
                    if (one.empty()) continue;
                    const int i = std::atoi(one.c_str());
                    if (i >= 0 && i < static_cast<int>(m_asset.cameraKeys.size())) {
                        m_sel.push_back(i);
                        if (first < 0) first = i;
                    }
                }
                std::sort(m_sel.begin(), m_sel.end());
                m_sel.erase(std::unique(m_sel.begin(), m_sel.end()), m_sel.end());
                m_primary = first;
                ok = !m_sel.empty();
                if (ok) VK_LOG_INFO("CURVED-SEL n=%d primary=%d",
                                    static_cast<int>(m_sel.size()), m_primary);
            }
        } else if (name == "chan") {
            for (int i = 0; i < static_cast<int>(Channel::Count); ++i) {
                if (arg == channelId(static_cast<Channel>(i))) {
                    ok = actSetChannel(static_cast<Channel>(i));
                    break;
                }
            }
        } else if (name == "dragkey") {
            // `dragkey:<i>:<dxPx>:<dyPx>` —— **走的是鼠标那条路**：
            // 先选中 `i` 并按它的像素位置开始拖，再喂同样的像素增量。
            int i = -1;
            float dx = 0.0f, dy = 0.0f;
            if (std::sscanf(arg.c_str(), "%d:%f:%f", &i, &dx, &dy) == 3) {
                rebuildCurve();
                if (i >= 0 && i < static_cast<int>(m_asset.cameraKeys.size())) {
                    beginDrag(m_plot, ImVec2(pxOfT(m_plot,
                                                   m_asset.cameraKeys[static_cast<std::size_t>(i)].t),
                                             pyOfV(m_plot, keyValue(i))),
                              false);
                    actDragKeysPx(dx, dy);
                    endDrag();
                    ok = true;
                }
            }
        } else if (name == "draghandle") {
            // `draghandle:<i>:<out|in>:<dxPx>:<dyPx>`
            char side[8] = {0};
            float dx = 0.0f, dy = 0.0f;
            int i = -1;
            if (std::sscanf(arg.c_str(), "%d:%7[^:]:%f:%f", &i, side, &dx, &dy) == 4) {
                rebuildCurve();
                if (i >= 0 && i + 1 < static_cast<int>(m_asset.cameraKeys.size())) {
                    const sequence::SeqCameraKey& a =
                        m_asset.cameraKeys[static_cast<std::size_t>(i)];
                    if (a.ease != sequence::Ease::Bezier) {
                        VK_LOG_WARN("CURVED-SCRIPT-NOTE drag handle needs ease=bezier");
                    } else {
                        ok = actSelect(i, false);
                        // 按手柄的真实像素位置起手 —— 与鼠标点它完全同一条路
                        const float span =
                            std::max(m_asset.cameraKeys[static_cast<std::size_t>(i) + 1].t - a.t,
                                     1.0e-3f);
                        const float dv = keyValue(i + 1) - keyValue(i);
                        const bool isOut = std::string(side) == "out";
                        const glm::vec2 h = isOut ? a.outHandle : a.inHandle;
                        const ImVec2 anchor =
                            isOut ? ImVec2(pxOfT(m_plot, a.t), pyOfV(m_plot, keyValue(i)))
                                  : ImVec2(pxOfT(m_plot,
                                                 m_asset.cameraKeys[static_cast<std::size_t>(i) + 1].t),
                                           pyOfV(m_plot, keyValue(i + 1)));
                        m_drag = DragState{};
                        m_drag.kind = isOut ? DragKind::HandleOut : DragKind::HandleIn;
                        m_drag.index = i;
                        m_drag.startMouse = anchor;
                        m_drag.handleOrig = h;
                        m_drag.moved = false;
                        actDragHandlePx(dx, dy);
                        endDrag();
                        ok = true;
                    }
                }
            }
        } else if (name == "boxsel") {
            // `boxsel:<fx0>:<fy0>:<fx1>:<fy1>` —— 坐标是**绘图区的比例**
            // （0..1），不是像素。用像素的话脚本得先知道窗口多大，而浮动
            // 窗口的尺寸会被 imgui.ini 记住 → 同一份脚本在不同机器上圈到
            // 的关键帧数量不一样。比例坐标只依赖"画布是那块矩形"。
            float fx0 = 0.0f, fy0 = 0.0f, fx1 = 1.0f, fy1 = 1.0f;
            if (std::sscanf(arg.c_str(), "%f:%f:%f:%f", &fx0, &fy0, &fx1, &fy1) == 4) {
                const float w = m_plot.max.x - m_plot.min.x;
                const float h = m_plot.max.y - m_plot.min.y;
                const ImVec2 a(m_plot.min.x + fx0 * w, m_plot.min.y + fy0 * h);
                const ImVec2 b(m_plot.min.x + fx1 * w, m_plot.min.y + fy1 * h);
                ok = actSelectRect(m_plot, a, b, false);
                VK_LOG_INFO("CURVED-BOXSEL n=%d primary=%d",
                            static_cast<int>(m_sel.size()), m_primary);
            }
        } else if (name == "setease") {
            sequence::Ease e = sequence::Ease::Smooth;
            if (sequence::parseEase(arg, e) && m_primary >= 0)
                ok = actSetEase(m_primary, e);
        } else if (name == "cycleease") {
            ok = actCycleEase();
        } else if (name == "setht") {
            animation::HandleType t = animation::HandleType::Auto;
            if (animation::parseHandleType(arg, t) && m_primary >= 0)
                ok = actSetHandleType(m_primary, t);
        } else if (name == "cycleht") {
            ok = actCycleHandleType();
        } else if (name == "sethandle") {
            // `sethandle:<out|in>:<x>:<y>`
            char side[8] = {0};
            float hx = 0.f, hy = 0.f;
            if (std::sscanf(arg.c_str(), "%7[^:]:%f:%f", side, &hx, &hy) == 3 &&
                m_primary >= 0) {
                ok = actSetHandle(m_primary, std::string(side) == "out",
                                  glm::vec2(hx, hy));
                if (ok) {
                    const sequence::SeqCameraKey& kk =
                        m_asset.cameraKeys[static_cast<std::size_t>(m_primary)];
                    VK_LOG_INFO("CURVED-SETHANDLE i=%d side=%s x=%.4f y=%.4f", m_primary,
                                std::string(side) == "out" ? "out" : "in",
                                static_cast<double>(std::string(side) == "out"
                                                        ? kk.outHandle.x
                                                        : kk.inHandle.x),
                                static_cast<double>(std::string(side) == "out"
                                                        ? kk.outHandle.y
                                                        : kk.inHandle.y));
                }
            }
        } else if (name == "head") {
            ok = actSetHead(static_cast<float>(std::atof(arg.c_str())));
        } else if (name == "fit") {
            resetView();
            ok = true;
        } else if (name == "sample") {
            // `sample:<t>` —— 打一行**当前通道**在该时刻的采样值。
            // 拖手柄之后靠它判"同一采样点单调变化"。
            const float t = static_cast<float>(std::atof(arg.c_str()));
            rebuildCurve();
            VK_LOG_INFO("CURVED-SAMPLE t=%.4f v=%.6f ch=%s", static_cast<double>(t),
                        static_cast<double>(valueAt(t)), channelId(m_channel));
            ok = true;
        } else if (name == "sleep") {
            // 给"磁盘冲突"那条用例的**闸门**：在脚本中间停一下，让脚本
            // 从外面把文件改掉，接着那条 `save` 就该被拒。
            const int ms = std::atoi(arg.c_str());
            if (ms > 0) {
                VK_LOG_INFO("CURVED-SLEEP ms=%d", ms);
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            }
            ok = true;
        } else if (name == "save") {
            ok = save(false);
        } else if (name == "saveforce") {
            ok = save(true);
        } else if (name == "reload") {
            reload();
            ok = !m_readOnly;
        } else if (name == "check") {
            revalidate();
            ok = m_checkValid;
        } else if (name == "dump") {
            const std::string text = sequence::toJson(m_asset);
            if (const char* dp = std::getenv("MYVK_EDITOR_CURVED_DUMP")) {
                if (FILE* f = std::fopen(dp, "wb")) {
                    std::fwrite(text.data(), 1, text.size(), f);
                    std::fclose(f);
                    VK_LOG_INFO("CURVED-DUMP path=%s keys=%d bytes=%d ok=1", dp,
                                static_cast<int>(m_asset.cameraKeys.size()),
                                static_cast<int>(text.size()));
                } else {
                    VK_LOG_WARN("CURVED-DUMP path=%s ok=0 (cannot open)", dp);
                }
            } else {
                VK_LOG_WARN("CURVED-DUMP ok=0 (MYVK_EDITOR_CURVED_DUMP unset)");
            }
            ok = true;
        } else {
            VK_LOG_WARN("CURVED-SCRIPT-BAD %s", tok.c_str());
        }

        if (ok) {
            ++applied;
        } else {
            ++failed;
            VK_LOG_WARN("CURVED-SCRIPT-FAIL %s", tok.c_str());
        }
    }

    clampSel();
    revalidate();
    VK_LOG_INFO("CURVED-SCRIPT applied=%d failed=%d", applied, failed);
}

} // namespace editor
