// ============================================================
// editor/TimelineEditorWindow.cpp
//
// 结构：工具栏（路径 / Reload / Save / New / 时长 / 播放头）
//      → 轨道区（标尺 + 相机轨 + 机位轨 + 事件轨，全自绘）
//      → 属性栏（选中那一项的字段）
//      → 校验栏。
//
// 所有**改动数据**的入口都是 `act*` 那一族 —— 界面按钮与注入脚本共用
// 它们（见 .h 的"动作"一节），所以"能不能自动验"不取决于把按钮坐标
// 算对没有。
//
// ⚠ 日志锚点前缀 `TLED-`（Timeline EDitor），与运行时的 `SEQ-` 分开：
//   两套东西同时出现在一份日志里时，"搜 SEQ- 全红"不该把编辑器的正常
//   输出也算进去。
// ============================================================

#include "TimelineEditorWindow.h"

#include "DebugRects.h"
#include "EditorContext.h"
#include "Localization.h"
#include "PanelDock.h"

#include "assets/AssetPath.h"
#include "core/Logger.h"
#include "ecs/Components.h"
#include "scene/CameraSystem.h"   // CameraPose + applyPoseToSceneCamera 的参数类型
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
#include <system_error>

namespace editor {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kRadToDeg = 180.0f / kPi;
constexpr float kDegToRad = kPi / 180.0f;

// 拖动 / 输入时的时间步长。0.01s 是"够细、又不至于永远对不齐" ——
// 时间轴上手动对齐到 10ms 是过场演出的实际精度（一个 60fps 的帧是 16.7ms，
// 再细的点作者按不出来）。
constexpr float kTimeStep = 0.01f;

// 事件 kind 的默认值。做成常量而不是直接写在 `actAddEvent("Sound")` 里：
// 它是**数据**（写进 `.seq.json` 的 `kind` 字段），不是界面文案，不该翻。
constexpr const char* kDefaultEventKind = "Sound";

const ImVec4 kOkCol(0.45f, 0.85f, 0.52f, 1.0f);
const ImVec4 kBadCol(1.00f, 0.45f, 0.35f, 1.0f);
const ImVec4 kWarnCol(1.00f, 0.75f, 0.30f, 1.0f);

std::string fmt1(const char* f, const char* a) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), f, a);
    return buf;
}

// 按**码点**取前缀。`substr(0, n)` 是按字节切 —— 中文会被劈成半个汉字
// （渲染成方块），而事件 `kind` 允许中文（"脚步声"）。
//
// 判据：UTF-8 的续字节是 10xxxxxx。碰到它说明上一个码点还没结束。
std::string utf8Prefix(const std::string& s, int maxCodepoints) {
    if (maxCodepoints <= 0) return {};
    int n = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if ((c & 0xC0u) == 0x80u) continue;   // 续字节：不算新的码点
        if (n == maxCodepoints) return s.substr(0, i);
        ++n;
    }
    return s;
}

// `sequences/opening.seq.json` → `opening`。与 `parseSequence` 的兜底
// 同一口径 —— 名字只是诊断用，但不能在两处推出不同的值。
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

// 两份路径化成"绝对 + 正斜杠 + 小写"再比。作者可能写 `.\sequences\x.seq.json`、
// 可能从 Content 拖过来带 `assets/` 前缀，而组件里存的是 `sequences/x.seq.json`
// —— 三种写法指的是同一份文件。
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

// kind → 颜色。同一个 kind 永远同一个色，作者扫一遍就知道"这三个是脚步声"。
// 用 FNV-1a 取哈希：要求只是"稳定 + 分布够散"，不是密码学。
ImU32 colorForKind(const std::string& kind) {
    unsigned h = 2166136261u;
    for (char c : kind) {
        h ^= static_cast<unsigned>(static_cast<unsigned char>(c));
        h *= 16777619u;
    }
    // 只取色相：75% 饱和度 + 60% 明度，保证在深色背景上都看得清。
    const float hue = static_cast<float>(h % 360u) / 360.0f;
    // HSV → RGB（手写六行，省掉一个 glm 依赖）
    const float s = 0.75f, v = 0.60f;
    const float hh = hue * 6.0f;
    const int i = static_cast<int>(hh) % 6;
    const float f = hh - static_cast<float>(static_cast<int>(hh));
    const float p = v * (1.0f - s);
    const float q = v * (1.0f - s * f);
    const float t = v * (1.0f - s * (1.0f - f));
    float r = v, g = t, b = p;
    switch (i) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
    return IM_COL32(static_cast<int>(r * 255.0f), static_cast<int>(g * 255.0f),
                    static_cast<int>(b * 255.0f), 255);
}

} // namespace

TimelineEditorWindow::TimelineEditorWindow(EditorContext& ctx) : m_ctx(ctx) {}

// ============================================================
// 打开 / 重读
// ============================================================

bool TimelineEditorWindow::open(const std::string& relPath) {
    m_open = true;
    m_path = relPath;
    m_sel = 0;
    m_head = 0.0f;
    m_dirty = false;
    m_uiError.clear();
    m_error.clear();
    m_bufKey.clear();
    m_injected = false;
    m_loaded = true;
    m_track = Track::Camera;
    m_dragMode = 0;   // 关窗再开时残留的拖拽状态会让下一次点击"接着上次拖"
    std::snprintf(m_pathBuf, sizeof(m_pathBuf), "%s", m_path.c_str());

    if (relPath.empty()) {
        // 空手开窗（从 View 菜单点开）：只剩 "New" 与上面那个路径框。
        m_asset = sequence::SequenceAsset{};
        m_readOnly = false;
        VK_LOG_INFO("TLED-OPEN path=(none) ok=0 keys=0 events=0 readOnly=0");
        revalidate();
        return false;
    }

    sequence::SequenceAsset a;
    const bool ok = sequence::loadSequence(relPath, a);
    m_asset = std::move(a);
    // ⑤ 读盘失败 → 只读。`cameraKeys` 可能只是前几帧的残片，允许存盘 =
    //    把 40 帧的过场截成 3 帧。
    m_readOnly = !ok;
    if (!ok) m_error = m_asset.error;

    // ⚠ `shots=%d` 只许**追加在行尾**：`verify_timeline_editor.py` 的
    //   `OPEN_RE` 用 `readOnly=(\d+)` 收尾（没有 `$` 锚），追加安全；
    //   插在中间会把那 5 组既有断言一起弄红。
    VK_LOG_INFO("TLED-OPEN path=%s ok=%d keys=%d events=%d dur=%.3f readOnly=%d "
                "shots=%d",
                relPath.c_str(), ok ? 1 : 0,
                static_cast<int>(m_asset.cameraKeys.size()),
                static_cast<int>(m_asset.events.size()),
                static_cast<double>(m_asset.duration), m_readOnly ? 1 : 0,
                static_cast<int>(m_asset.shots.size()));
    revalidate();
    return ok;
}

void TimelineEditorWindow::close() {
    if (m_open && m_dirty) {
        VK_LOG_WARN("TLED-CLOSE-DIRTY path=%s（有未存的改动）", m_path.c_str());
    }
    m_open = false;
}

void TimelineEditorWindow::reload() {
    if (m_path.empty()) {
        m_asset = sequence::SequenceAsset{};
        m_readOnly = false;
        revalidate();
        return;
    }
    sequence::SequenceAsset a;
    const bool ok = sequence::loadSequence(m_path, a);
    m_asset = std::move(a);
    m_readOnly = !ok;
    m_dirty = false;
    m_bufKey.clear();
    m_error = ok ? std::string() : m_asset.error;
    m_uiError.clear();
    clampSel();
    m_head = std::clamp(m_head, 0.0f, std::max(m_asset.duration, 0.0f));
    VK_LOG_INFO("TLED-RELOAD path=%s ok=%d keys=%d events=%d readOnly=%d",
                m_path.c_str(), ok ? 1 : 0,
                static_cast<int>(m_asset.cameraKeys.size()),
                static_cast<int>(m_asset.events.size()), m_readOnly ? 1 : 0);
    revalidate();
}

// ============================================================
// 校验：序列化 → 丢回运行时那个解析器（④）
// ============================================================

void TimelineEditorWindow::revalidate() {
    sequence::SequenceAsset check;
    const std::string text = sequence::toJson(m_asset);
    const bool parsed = sequence::parseSequence(
        text, m_path.empty() ? std::string("untitled.seq.json") : m_path, check);

    m_checkValid = parsed && check.valid;
    m_checkError = check.error;
    m_checkWarnings = check.warnings;

    // 只在**结论变化**时打日志：每次拖动都打的话，一次拖动就是几十条
    // TLED-CHECK，真正的那条淹没在里面。
    const std::string key =
        std::string(m_checkValid ? "1" : "0") + "|" + m_checkError + "|" +
        std::to_string(m_checkWarnings.size()) + "|" +
        std::to_string(m_asset.cameraKeys.size()) + "|" +
        std::to_string(m_asset.events.size());
    if (key != m_checkLogKey) {
        m_checkLogKey = key;
        VK_LOG_INFO("TLED-CHECK ok=%d keys=%d events=%d dur=%.3f warns=%d err=%s",
                    m_checkValid ? 1 : 0,
                    static_cast<int>(m_asset.cameraKeys.size()),
                    static_cast<int>(m_asset.events.size()),
                    static_cast<double>(m_asset.duration),
                    static_cast<int>(m_checkWarnings.size()),
                    m_checkValid ? "" : m_checkError.c_str());
    }
}

// ============================================================
// 选择
// ============================================================

// 轨道的**显示顺序**（行号 → 轨道）。
//
// ⚠ 用一张表而不是"枚举值即行号"：枚举值要稳定（见 .h 的 ⚠⚠），显示顺序
//   要随时能调。机位轨排在事件轨上面，因为它和相机轨是同一件事的两半
//   （"这台怎么动" / "用哪台"），中间夹一行事件轨读起来是断的。
const TimelineEditorWindow::Track kTrackOrder[] = {
    TimelineEditorWindow::Track::Camera,
    TimelineEditorWindow::Track::Shots,
    TimelineEditorWindow::Track::Events,
};
constexpr int kTrackRows = static_cast<int>(sizeof(kTrackOrder) /
                                            sizeof(kTrackOrder[0]));

int TimelineEditorWindow::rowOf(Track t) {
    for (int i = 0; i < kTrackRows; ++i)
        if (kTrackOrder[i] == t) return i;
    return 0;
}

TimelineEditorWindow::Track TimelineEditorWindow::trackAtRow(int row) {
    if (row < 0 || row >= kTrackRows) return Track::Camera;
    return kTrackOrder[row];
}

int TimelineEditorWindow::trackCount(Track t) const {
    switch (t) {
    case Track::Camera: return static_cast<int>(m_asset.cameraKeys.size());
    case Track::Events: return static_cast<int>(m_asset.events.size());
    case Track::Shots: return static_cast<int>(m_asset.shots.size());
    default: return 0;
    }
}

void TimelineEditorWindow::clampSel() {
    const int n = trackCount(m_track);
    if (n <= 0) { m_sel = 0; return; }
    m_sel = std::clamp(m_sel, 0, n - 1);
}

sequence::SeqCameraKey* TimelineEditorWindow::selKey() {
    if (m_track != Track::Camera) return nullptr;
    if (m_sel < 0 || m_sel >= static_cast<int>(m_asset.cameraKeys.size()))
        return nullptr;
    return &m_asset.cameraKeys[static_cast<std::size_t>(m_sel)];
}

sequence::SeqEvent* TimelineEditorWindow::selEvent() {
    if (m_track != Track::Events) return nullptr;
    if (m_sel < 0 || m_sel >= static_cast<int>(m_asset.events.size())) return nullptr;
    return &m_asset.events[static_cast<std::size_t>(m_sel)];
}

sequence::SeqShot* TimelineEditorWindow::selShot() {
    if (m_track != Track::Shots) return nullptr;
    if (m_sel < 0 || m_sel >= static_cast<int>(m_asset.shots.size())) return nullptr;
    return &m_asset.shots[static_cast<std::size_t>(m_sel)];
}

// ============================================================
// 编辑缓冲（只有事件轨需要：kind / payload 是字符串）
// ============================================================

void TimelineEditorWindow::syncBuffers() {
    // 键 = 轨道 + 下标 + 该项的 t。加 t 是为了让"拖动之后"自动重建
    //（拖完 t 变了，缓冲里的字还是旧的，但显示的是同一项的内容，
    // 重建一次不影响正在输入的东西 —— 拖的时候鼠标在轨道上）。
    //
    // ⚠⚠ 键里**必须带轨道名**（`ev:` / `sh:`）。只按下标的话，从事件轨的
    //   第 2 项切到机位轨的第 2 项，下标相同 → 键相同 → **不重建**，
    //   于是机位栏里显示的是上一条事件的 kind 文本。这是加第三轨时最容易
    //   踩的一脚（原先"只有两条轨"时，两个分支的 key 一个非空一个空，
    //   恰好没暴露）。
    std::string key;
    if (m_track == Track::Events) {
        const sequence::SeqEvent* e = selEvent();
        key = e ? ("ev:" + std::to_string(m_sel) + ":" +
                   std::to_string(static_cast<int>(e->t * 1000.0f)))
                : std::string();
    } else if (m_track == Track::Shots) {
        const sequence::SeqShot* s = selShot();
        key = s ? ("sh:" + std::to_string(m_sel) + ":" +
                   std::to_string(static_cast<int>(s->t * 1000.0f)))
                : std::string();
    }
    if (key == m_bufKey) return;
    m_bufKey = key;

    m_labelBuf[0] = '\0';
    if (const sequence::SeqShot* s = selShot())
        std::snprintf(m_labelBuf, sizeof(m_labelBuf), "%s", s->label.c_str());

    const sequence::SeqEvent* e = selEvent();
    if (!e) {
        m_kindBuf[0] = '\0';
        m_payloadBuf[0] = '\0';
        return;
    }
    std::snprintf(m_kindBuf, sizeof(m_kindBuf), "%s", e->kind.c_str());
    std::snprintf(m_payloadBuf, sizeof(m_payloadBuf), "%s", e->payload.c_str());
}

// ============================================================
// 动作
// ============================================================

void TimelineEditorWindow::markDirty() { m_dirty = true; }

bool TimelineEditorWindow::reject(const char* op, const std::string& reason) {
    m_uiError = std::string(TR("Rejected")) + ": " + reason;
    VK_LOG_WARN("TLED-REJECT op=%s reason=%s", op, reason.c_str());
    return false;
}

// 把第 `from` 项按 t 挪到它该在的位置，返回新下标。
//
// ⚠ 用"取出-插入"而不是整体 `std::sort`：sort 之后只能靠 t 猜"哪个是我"，
//   而同一 t 上允许有多个事件（`parseSequence` 用 stable_sort 保序）。
//   取出-插入天然知道身份，代价是 O(n)，而 n 是个位数到几十。
// ⚠ 相等时插到**后面**（`<=`）：正向拖动更常见，作者的预期是"我拖过它了"。
int TimelineEditorWindow::reorderKey(int from) {
    auto& v = m_asset.cameraKeys;
    if (from < 0 || from >= static_cast<int>(v.size())) return from;
    sequence::SeqCameraKey k = v[static_cast<std::size_t>(from)];
    v.erase(v.begin() + from);
    int to = 0;
    while (to < static_cast<int>(v.size()) &&
           v[static_cast<std::size_t>(to)].t <= k.t + 1.0e-6f)
        ++to;
    v.insert(v.begin() + to, k);
    // 重排是**唯一**会改变"下标 ↔ 身份"对应关系的动作，所以它必须留痕：
    // 拖拽（`TLED-DRAG-END`）和输入框改 t（`TLED-SETTIME`）都会走到这里，
    // 而验收脚本要能同时对两条路径断言"数组真的重排了、且身份没串"。
    VK_LOG_INFO("TLED-REORDER track=camera from=%d to=%d t=%.3f", from, to,
                static_cast<double>(k.t));
    return to;
}

int TimelineEditorWindow::reorderEvent(int from) {
    auto& v = m_asset.events;
    if (from < 0 || from >= static_cast<int>(v.size())) return from;
    sequence::SeqEvent e = v[static_cast<std::size_t>(from)];
    v.erase(v.begin() + from);
    int to = 0;
    while (to < static_cast<int>(v.size()) &&
           v[static_cast<std::size_t>(to)].t <= e.t + 1.0e-6f)
        ++to;
    v.insert(v.begin() + to, e);
    VK_LOG_INFO("TLED-REORDER track=events from=%d to=%d t=%.3f", from, to,
                static_cast<double>(e.t));
    return to;
}

int TimelineEditorWindow::reorderShot(int from) {
    auto& v = m_asset.shots;
    if (from < 0 || from >= static_cast<int>(v.size())) return from;
    sequence::SeqShot s = v[static_cast<std::size_t>(from)];
    v.erase(v.begin() + from);
    int to = 0;
    while (to < static_cast<int>(v.size()) &&
           v[static_cast<std::size_t>(to)].t <= s.t + 1.0e-6f)
        ++to;
    v.insert(v.begin() + to, s);
    VK_LOG_INFO("TLED-REORDER track=shots from=%d to=%d t=%.3f", from, to,
                static_cast<double>(s.t));
    return to;
}

// ============================================================
// 多机位 marker（第三轨，2026-10-30）
// ============================================================
//
// 这一轨的语义与另外两轨**不同构**：相机轨与事件轨都是"某一瞬间发生的一件事"，
// 而 marker 是"**从这时起**切到某台机位"—— 它划的是**区间**，一直有效到
// 下一个 marker（或片尾）。所以：
//   · 拖动它的 t 会改变**两段**的边界，而重排（按 t 升序）在语义上必须保持；
//   · 它不参与 `sampleCamera`，姿态仍来自相机轨 —— marker 只说"用哪台"。

bool TimelineEditorWindow::actAddShot() {
    // 同一时刻已经有一个 marker → 拒绝（与 `actAddKey` 同一条理由：
    // 两个同 t 的 marker 谁生效取决于排序的稳定性，作者看不出来）。
    for (const auto& s : m_asset.shots) {
        if (std::fabs(s.t - m_head) < 1.0e-3f) {
            char msg[160];
            std::snprintf(msg, sizeof(msg),
                          TR("a shot marker already exists at %.3f s"),
                          static_cast<double>(s.t));
            return reject("addshot", msg);
        }
    }
    sequence::SeqShot s;
    s.t = m_head;
    // 默认绑"编辑器里当前选中的那个相机" —— 这是作者的心智模型：
    // 在场景里点中 B 机位 → 时间轴上打个点 → 这一段就是 B。
    s.cameraGuid = selectedCameraGuid();
    m_asset.shots.push_back(s);
    m_track = Track::Shots;
    m_sel = reorderShot(static_cast<int>(m_asset.shots.size()) - 1);
    m_bufKey.clear();
    markDirty();
    VK_LOG_INFO("TLED-ADDSHOT t=%.3f cam=%s shots=%d sel=%d",
                static_cast<double>(m_head),
                s.cameraGuid ? ecs::guidToHex(s.cameraGuid).c_str() : "(keys)",
                static_cast<int>(m_asset.shots.size()), m_sel);
    revalidate();
    return true;
}

// `guid == 0` 是**合法值**（"这一段回落到相机轨"），所以这个函数不能用
// "0 = 没选中"当错误条件。要拒绝的是"非 0 但场景里没有这台相机"。
bool TimelineEditorWindow::actBindShotCam(std::uint64_t guid) {
    sequence::SeqShot* s = selShot();
    if (!s) return reject("setshotcam", TR("no shot marker selected"));
    if (guid != 0) {
        scene::Scene& sc = m_ctx.activeScene();
        const ecs::Entity e = sc.world().find(guid);
        if (!e.valid() || !sc.world().get<ecs::CameraComponent>(e)) {
            // ⚠ 这里必须拒绝。放行的话文件里会留下一个指向"不存在的实体"的
            //   guid，进 Play 后 `CameraSystem` 找不到它 → 导演覆盖被静默
            //   释放（`CAM-CUT-MISS`），作者看到的是"这一段用的还是默认相机"，
            //   而文件里明明写着 B 机位。
            return reject("setshotcam",
                          fmt1(TR("no enabled camera with guid %s in the scene"),
                               ecs::guidToHex(guid).c_str()));
        }
    }
    s->cameraGuid = guid;
    markDirty();
    VK_LOG_INFO("TLED-SETSHOTCAM t=%.3f cam=%s", static_cast<double>(s->t),
                guid ? ecs::guidToHex(guid).c_str() : "(keys)");
    revalidate();
    return true;
}

bool TimelineEditorWindow::actBindShotToSelection() {
    const std::uint64_t g = selectedCameraGuid();
    if (g == 0)
        return reject("bindsel",
                      TR("nothing selected — pick a camera in the scene first"));
    return actBindShotCam(g);
}

bool TimelineEditorWindow::actSetShotTime(float t) {
    sequence::SeqShot* s = selShot();
    if (!s) return reject("setshottime", TR("no shot marker selected"));
    t = std::clamp(t, 0.0f, std::max(m_asset.duration, 0.0f));
    s->t = t;
    m_sel = reorderShot(m_sel);
    markDirty();
    VK_LOG_INFO("TLED-SETSHOTTIME t=%.3f sel=%d", static_cast<double>(t), m_sel);
    revalidate();
    return true;
}

bool TimelineEditorWindow::actSetShotBlend(float seconds) {
    sequence::SeqShot* s = selShot();
    if (!s) return reject("setshotblend", TR("no shot marker selected"));
    // 上限 3 秒：混合是"推拉镜头"的时长，超过三秒就不像是切镜而像是
    // 上一段运镜没停。下限 0 = 硬切（合法且默认）。
    s->blendSeconds = std::clamp(seconds, 0.0f, 3.0f);
    markDirty();
    VK_LOG_INFO("TLED-SETSHOTBLEND t=%.3f blend=%.3f",
                static_cast<double>(s->t), static_cast<double>(s->blendSeconds));
    revalidate();
    return true;
}

bool TimelineEditorWindow::actSetShotLabel(const std::string& label) {
    sequence::SeqShot* s = selShot();
    if (!s) return reject("setshotlabel", TR("no shot marker selected"));
    s->label = label;
    markDirty();
    revalidate();
    return true;
}

std::uint64_t TimelineEditorWindow::selectedCameraGuid() const {
    scene::Scene& sc = m_ctx.activeScene();
    ecs::World& w = sc.world();
    ecs::Entity e = m_ctx.selection();
    // 沿父链向上找。⚠ 必须有层数上限：父子关系是用户编辑出来的数据，
    //   `parent` 指错方向（B 的父是 A、A 的父又是 B）时这个循环会转不出来，
    //   而它跑在**点一下按钮**的路径上 —— 症状是整个编辑器卡死。
    //
    // ⚠ 父链用的是 `HierarchyComponent::parent`（EntityId 槽位），**不是 guid**。
    //   两者不能混：id 在销毁后会被复用，这里只在"同一次加载内"用，正好适用。
    for (int depth = 0; depth < 64 && e.valid(); ++depth) {
        if (const auto* cc = w.get<ecs::CameraComponent>(e); cc && cc->enabled)
            return w.guid(e);
        const auto* hc = w.get<ecs::HierarchyComponent>(e);
        if (!hc || hc->parent == ecs::kInvalidEntity) break;
        ecs::Entity parent;
        parent.id = hc->parent;
        // 用 `alive()` 校验：层数上限挡的是环，挡不住"悬空的 parent id"
        // （父被销毁后子节点的 parent 还指着老槽位 → 可能正好指到一个
        // 复用了这个 id 的**别的**实体，于是我们"向上找到了"一台无关的相机）。
        if (!w.alive(parent)) break;
        e = parent;
    }
    return 0;
}


sequence::SeqCameraKey TimelineEditorWindow::poseAtHead() const {
    sequence::SeqCameraKey out;
    out.t = m_head;
    if (!m_asset.cameraKeys.empty()) {
        // 有关键帧 → 用采样值（"在这个时刻加一帧，画面不变"）
        sequence::sampleCamera(m_asset, m_head, out);
        out.t = m_head;
        return out;
    }
    return poseFromViewport();
}

sequence::SeqCameraKey TimelineEditorWindow::poseFromViewport() const {
    scene::Scene& sc = m_ctx.activeScene();
    const scene::Camera& cam = sc.camera();
    sequence::SeqCameraKey out;
    out.t = m_head;
    out.position = cam.position();
    out.yaw = cam.yaw();
    out.pitch = cam.pitch();
    out.fov = cam.fov();
    out.ease = sequence::Ease::Smooth;
    return out;
}

bool TimelineEditorWindow::actAddKey() {
    // 同一时刻已经有一帧 → 拒绝。两帧同 t 会退化成硬切（`parseSequence`
    // 会报 duplicate 警告），那几乎不会是作者想要的。
    for (const auto& k : m_asset.cameraKeys) {
        if (std::fabs(k.t - m_head) < 1.0e-3f) {
            // ⚠ 这里不能用 `fmt1`（它只吃一个 `%s`）：这个格式串是 `%.3f`，
            //   硬塞空串会打印出一个未定义的数字（或直接崩）。自带缓冲。
            char msg[160];
            std::snprintf(msg, sizeof(msg),
                          TR("a camera key already exists at %.3f s"),
                          static_cast<double>(k.t));
            return reject("addkey", msg);
        }
    }
    m_asset.cameraKeys.push_back(poseAtHead());
    m_track = Track::Camera;
    m_sel = reorderKey(static_cast<int>(m_asset.cameraKeys.size()) - 1);
    markDirty();
    VK_LOG_INFO("TLED-ADDKEY t=%.3f keys=%d sel=%d", static_cast<double>(m_head),
                static_cast<int>(m_asset.cameraKeys.size()), m_sel);
    revalidate();
    return true;
}

bool TimelineEditorWindow::actCaptureKey() {
    sequence::SeqCameraKey snap = poseFromViewport();

    // 这个时刻已经有帧 → **覆盖**它（"我把机位重新摆好了，更新这一帧"
    // 是最常见的操作，比"报错让你先删"顺手得多）。
    for (std::size_t i = 0; i < m_asset.cameraKeys.size(); ++i) {
        if (std::fabs(m_asset.cameraKeys[i].t - m_head) < 1.0e-3f) {
            const sequence::Ease keep = m_asset.cameraKeys[i].ease;
            m_asset.cameraKeys[i] = snap;
            m_asset.cameraKeys[i].t = m_head;
            m_asset.cameraKeys[i].ease = keep;   // 缓动是作者设的，不该被覆盖
            m_track = Track::Camera;
            m_sel = static_cast<int>(i);
            markDirty();
            VK_LOG_INFO("TLED-CAPTURE mode=overwrite t=%.3f sel=%d",
                        static_cast<double>(m_head), m_sel);
            revalidate();
            return true;
        }
    }

    m_asset.cameraKeys.push_back(snap);
    m_track = Track::Camera;
    m_sel = reorderKey(static_cast<int>(m_asset.cameraKeys.size()) - 1);
    markDirty();
    VK_LOG_INFO("TLED-CAPTURE mode=insert t=%.3f keys=%d sel=%d",
                static_cast<double>(m_head),
                static_cast<int>(m_asset.cameraKeys.size()), m_sel);
    revalidate();
    return true;
}

bool TimelineEditorWindow::actAddEvent(const std::string& kind) {
    sequence::SeqEvent e;
    e.t = m_head;
    e.kind = kind.empty() ? std::string(kDefaultEventKind) : kind;
    m_asset.events.push_back(e);
    m_track = Track::Events;
    m_sel = reorderEvent(static_cast<int>(m_asset.events.size()) - 1);
    m_bufKey.clear();
    markDirty();
    VK_LOG_INFO("TLED-ADDEVENT t=%.3f kind=%s events=%d sel=%d",
                static_cast<double>(m_head), e.kind.c_str(),
                static_cast<int>(m_asset.events.size()), m_sel);
    revalidate();
    return true;
}

bool TimelineEditorWindow::actDelete() {
    if (m_track == Track::Camera) {
        if (m_sel < 0 || m_sel >= static_cast<int>(m_asset.cameraKeys.size()))
            return reject("del", TR("no camera key selected"));
        const float t = m_asset.cameraKeys[static_cast<std::size_t>(m_sel)].t;
        m_asset.cameraKeys.erase(m_asset.cameraKeys.begin() + m_sel);
        clampSel();
        markDirty();
        VK_LOG_INFO("TLED-DEL track=camera t=%.3f keys=%d", static_cast<double>(t),
                    static_cast<int>(m_asset.cameraKeys.size()));
    } else if (m_track == Track::Shots) {
        if (m_sel < 0 || m_sel >= static_cast<int>(m_asset.shots.size()))
            return reject("del", TR("no shot marker selected"));
        const float t = m_asset.shots[static_cast<std::size_t>(m_sel)].t;
        m_asset.shots.erase(m_asset.shots.begin() + m_sel);
        clampSel();
        m_bufKey.clear();
        markDirty();
        VK_LOG_INFO("TLED-DEL track=shots t=%.3f shots=%d", static_cast<double>(t),
                    static_cast<int>(m_asset.shots.size()));
    } else {
        if (m_sel < 0 || m_sel >= static_cast<int>(m_asset.events.size()))
            return reject("del", TR("no event selected"));
        const std::string kind =
            m_asset.events[static_cast<std::size_t>(m_sel)].kind;
        m_asset.events.erase(m_asset.events.begin() + m_sel);
        clampSel();
        m_bufKey.clear();
        markDirty();
        VK_LOG_INFO("TLED-DEL track=events kind=%s events=%d", kind.c_str(),
                    static_cast<int>(m_asset.events.size()));
    }
    revalidate();
    return true;
}

bool TimelineEditorWindow::actSetTime(float t) {
    t = std::clamp(t, 0.0f, std::max(m_asset.duration, 0.0f));
    if (m_track == Track::Camera) {
        sequence::SeqCameraKey* k = selKey();
        if (!k) return reject("settime", TR("no camera key selected"));
        k->t = t;
        m_sel = reorderKey(m_sel);      // ① 改完 t 必须重排，且跟着挪选中项
    } else if (m_track == Track::Shots) {
        // marker 的 t 走自己那一条（它有自己的日志名与重排），
        // 但仍要经过 `actSetTime` 这个入口 —— 属性栏只有一个 t 输入框。
        return actSetShotTime(t);
    } else {
        sequence::SeqEvent* e = selEvent();
        if (!e) return reject("settime", TR("no event selected"));
        e->t = t;
        m_sel = reorderEvent(m_sel);
        m_bufKey.clear();
    }
    markDirty();
    VK_LOG_INFO("TLED-SETTIME track=%s t=%.3f sel=%d",
                m_track == Track::Camera ? "camera" : "events",
                static_cast<double>(t), m_sel);
    revalidate();
    return true;
}

bool TimelineEditorWindow::actSetPos(glm::vec3 p) {
    sequence::SeqCameraKey* k = selKey();
    if (!k) return reject("setpos", TR("no camera key selected"));
    k->position = p;
    markDirty();
    revalidate();
    return true;
}

bool TimelineEditorWindow::actSetYawDeg(float deg) {
    sequence::SeqCameraKey* k = selKey();
    if (!k) return reject("setyaw", TR("no camera key selected"));
    // ② UI 用度，资产用弧度。转换只在这一层发生。
    k->yaw = deg * kDegToRad;
    markDirty();
    revalidate();
    return true;
}

bool TimelineEditorWindow::actSetPitchDeg(float deg) {
    sequence::SeqCameraKey* k = selKey();
    if (!k) return reject("setpitch", TR("no camera key selected"));
    // 与 `scene::Camera::kPitchLimit` 同一个上限（89°）：超过之后
    // `glm::lookAt` 的基会退化，画面翻转。
    k->pitch = std::clamp(deg, -89.0f, 89.0f) * kDegToRad;
    markDirty();
    revalidate();
    return true;
}

bool TimelineEditorWindow::actSetFov(float fov) {
    sequence::SeqCameraKey* k = selKey();
    if (!k) return reject("setfov", TR("no camera key selected"));
    k->fov = std::clamp(fov, 10.0f, 120.0f);
    markDirty();
    revalidate();
    return true;
}

bool TimelineEditorWindow::actSetEase(const std::string& name) {
    sequence::SeqCameraKey* k = selKey();
    if (!k) return reject("setease", TR("no camera key selected"));
    sequence::Ease e = sequence::Ease::Smooth;
    if (!sequence::parseEase(name, e)) {
        return reject("setease", fmt1(TR("unknown ease '%s'"), name.c_str()));
    }
    k->ease = e;
    markDirty();
    VK_LOG_INFO("TLED-SETEASE t=%.3f ease=%s", static_cast<double>(k->t),
                name.c_str());
    revalidate();
    return true;
}

bool TimelineEditorWindow::actSetHandle(bool isOut, int axis, float v) {
    sequence::SeqCameraKey* k = selKey();
    if (!k) return reject("sethandle", TR("no camera key selected"));
    glm::vec2& h = isOut ? k->outHandle : k->inHandle;
    if (axis == 0) {
        // ⚠ 符号约束（见 `SeqCameraKey` 的 ⚠⚠）：out.x >= 0、in.x <= 0。
        //   允许拖到反面的话，这段贝塞尔在 x 上不再单调，求根会解出
        //   两个 u —— 表现是"镜头在某一刻往回调了一下"。
        h.x = isOut ? std::clamp(v, 0.0f, 1.0f) : std::clamp(v, -1.0f, 0.0f);
    } else {
        // y 不限符号：负值就是"先往回退再出发"（预备动作），这才是手柄的意义。
        h.y = std::clamp(v, -2.0f, 3.0f);
    }
    markDirty();
    revalidate();
    return true;
}

bool TimelineEditorWindow::actSetEventKind(const std::string& kind) {
    sequence::SeqEvent* e = selEvent();
    if (!e) return reject("setkind", TR("no event selected"));
    if (kind.empty()) return reject("setkind", TR("an event needs a kind"));
    e->kind = kind;
    markDirty();
    revalidate();
    return true;
}

bool TimelineEditorWindow::actSetEventPayload(const std::string& payload) {
    sequence::SeqEvent* e = selEvent();
    if (!e) return reject("setpayload", TR("no event selected"));
    e->payload = payload;
    markDirty();
    revalidate();
    return true;
}

bool TimelineEditorWindow::actSetDuration(float d) {
    // 下限 0.1s：duration=0 的过场在 `parseSequence` 里会走"按内容推算"
    // 分支，而编辑器总是写显式值（见 `SequenceAsset.h` ①）—— 写个 0 进去
    // 等于把语义从"显式"切回"推算"，作者看到的是"我设的 0 变成了 8"。
    m_asset.duration = std::clamp(d, 0.1f, 3600.0f);
    m_head = std::min(m_head, m_asset.duration);
    markDirty();
    VK_LOG_INFO("TLED-SETDUR dur=%.3f", static_cast<double>(m_asset.duration));
    revalidate();
    return true;
}

bool TimelineEditorWindow::actSetHead(float t) {
    m_head = std::clamp(t, 0.0f, std::max(m_asset.duration, 0.0f));
    // ③ 只采样、不推进（dt=0 的语义）。走的是纯函数 `sampleCamera` 而不是
    //    `SequencerSystem::tick` —— 后者的播放头在**组件里**，而窗口手里的
    //    播放头与它无关（从菜单裸开一份资产时，场景里根本没有组件）。
    sequence::SeqCameraKey k;
    if (sequence::sampleCamera(m_asset, m_head, k)) {
        scene::CameraPose p;
        p.position = k.position;
        p.yaw = k.yaw;
        p.pitch = k.pitch;
        p.fov = k.fov;
        sequence::applyPoseToSceneCamera(m_ctx.activeScene(), p);
    }
    return true;
}

bool TimelineEditorWindow::actSetLoop(bool loop) {
    m_asset.loop = loop;
    markDirty();
    revalidate();
    return true;
}

bool TimelineEditorWindow::actNewSequence() {
    m_asset = sequence::SequenceAsset{};
    m_asset.duration = 5.0f;
    // 一帧的骨架。有这一帧，"新建 → 存盘 → 挂组件"就能跑起来看到画面，
    // 而不是"黑屏 + 一行 SEQ-ASSET-BAD: no 'camera' keys"。
    sequence::SeqCameraKey k;
    k.t = 0.0f;
    k.position = poseFromViewport().position;   // 从视口所在位置起步
    k.yaw = poseFromViewport().yaw;
    k.pitch = poseFromViewport().pitch;
    k.fov = poseFromViewport().fov;
    k.ease = sequence::Ease::Smooth;
    m_asset.cameraKeys.push_back(k);
    // 名字从**路径**推（`sequences/opening.seq.json` → `opening`）：
    // ⚠ 不能读 `m_asset.name` —— 上面那一行已经把 m_asset 整个重置了。
    m_asset.name = nameFromPath(m_path);

    m_track = Track::Camera;
    m_sel = 0;
    m_head = 0.0f;
    m_bufKey.clear();
    m_dirty = true;
    m_readOnly = false;      // 新建的资产当然可以存
    m_error.clear();
    m_uiError.clear();
    VK_LOG_INFO("TLED-NEW keys=1 events=0 dur=5.000 path=%s",
                m_path.empty() ? "(none)" : m_path.c_str());
    revalidate();
    return true;
}

// ============================================================
// 存盘
// ============================================================

bool TimelineEditorWindow::save(bool force) {
    if (m_readOnly) {
        m_uiError = TR("Read-only: the file failed to load, saving would "
                       "truncate it. Fix it and press Reload.");
        VK_LOG_WARN("TLED-SAVE-BLOCKED path=%s reason=read-only", m_path.c_str());
        return false;
    }
    if (m_path.empty()) {
        m_uiError = TR("Set an asset path first (e.g. sequences/opening.seq.json).");
        VK_LOG_WARN("TLED-SAVE-BLOCKED path=(none) reason=no-path");
        return false;
    }

    revalidate();
    if (!m_checkValid && !force) {
        m_uiError = std::string(TR("Not saved — the sequence does not load:")) +
                    " " + m_checkError;
        VK_LOG_WARN("TLED-SAVE-BLOCKED path=%s reason=check err=%s",
                    m_path.c_str(), m_checkError.c_str());
        return false;
    }

    std::string err;
    if (!sequence::saveSequence(m_path, m_asset, err)) {
        m_error = err;
        m_uiError = err;
        VK_LOG_ERROR("TLED-SAVE-FAIL path=%s err=%s", m_path.c_str(), err.c_str());
        return false;
    }

    m_dirty = false;
    m_error.clear();
    m_uiError.clear();
    VK_LOG_INFO("TLED-SAVE path=%s keys=%d events=%d force=%d", m_path.c_str(),
                static_cast<int>(m_asset.cameraKeys.size()),
                static_cast<int>(m_asset.events.size()), force ? 1 : 0);
    refreshSceneComponents();
    return true;
}

void TimelineEditorWindow::refreshSceneComponents() {
    const std::string mine = canonicalOf(m_path);
    if (mine.empty()) return;
    scene::Scene& sc = m_ctx.activeScene();
    ecs::World& w = sc.world();

    int reloaded = 0;
    int skipped = 0;
    w.each<ecs::SequencerComponent>([&](ecs::Entity e,
                                        ecs::SequencerComponent& c) {
        if (canonicalOf(c.sequencePath) != mine) return;
        if (c.started && !c.finished && c.playing) {
            // 正在播的那一段不打断：中途换运镜比"这次预览是旧的"更糟 ——
            // 画面会在一帧之内跳到一个完全不同的机位。
            ++skipped;
            VK_LOG_INFO("TLED-SCENE-SKIP entity=%u path=%s (playing)",
                        static_cast<unsigned>(e.id), c.sequencePath.c_str());
            return;
        }
        // ⚠ `assetLoaded = false` 是**唯一**让系统重读的开关
        //   （`SequencerSystem::tick` 里 `if (!c.assetLoaded)` 时才 `loadSequence`）。
        //   不置这个标志的话：窗口里是新运镜、Save 成功、进 Play 还是旧的。
        c.assetLoaded = false;
        c.loadFailedLogged = false;
        c.started = false;
        c.finished = false;
        c.playhead = 0.0f;
        ++reloaded;
    });
    VK_LOG_INFO("TLED-SCENE-RELOAD path=%s reloaded=%d skipped=%d",
                m_path.c_str(), reloaded, skipped);
}

// ============================================================
// 绘制
// ============================================================

void TimelineEditorWindow::draw(bool& open) {
    if (!m_open) return;

    ImGuiWindowFlags wf = 0;
    const bool docked = applyDockHint(wf);
    if (!docked) {
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowSizeConstraints(ImVec2(720.0f, 420.0f),
                                            ImVec2(FLT_MAX, FLT_MAX));
        ImGui::SetNextWindowSize(ImVec2(1060.0f, 560.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.12f,
                                       io.DisplaySize.y * 0.10f),
                                ImGuiCond_FirstUseEver);
    }

    // `###panel_tled` 是稳定 ID（标题随路径变化时窗口位置/大小记忆不能丢）。
    // ⚠ 用 std::string 拼而不是 snprintf：`###` 那段是 ImGui 的稳定 ID，
    //   不是给人看的字，放进格式串会被 verify_i18n_coverage.py 的 D 组抓出来。
    std::string title = TR("Timeline editor");
    if (!m_path.empty()) {
        title += " — ";
        title += m_asset.name.empty() ? m_path : m_asset.name;
        if (m_dirty) title += " *";
    }
    title += "###panel_tled";

    if (!ImGui::Begin(title.c_str(), docked ? nullptr : &open, wf)) {
        ImGui::End();
        return;
    }
    logRect("TLED-WIN", ImGui::GetWindowPos(),
            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                   ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    // 注入脚本排在最前：它要在工具栏之前跑，这样脚本改出来的状态同一帧
    // 就能被画出来（脚本断言与截图看到的是同一个东西）。
    runInjectedScript();

    drawToolbar(open);
    ImGui::Separator();
    drawTracks();
    drawInspectorRow();
    drawValidation();

    ImGui::End();

    if (!open) close();
}

void TimelineEditorWindow::drawToolbar(bool& open) {
    (void)open;

    // ---- 第一行：资产路径与三个按钮 ----
    ImGui::TextDisabled("%s", TR("Asset path"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(360.0f);
    if (ImGui::InputTextWithHint("##tled_path", "sequences/opening.seq.json",
                                 m_pathBuf, sizeof(m_pathBuf),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
        // 改路径**不自动读盘**（半截路径会立刻报"打不开"），回车才读。
        m_path = m_pathBuf;
        reload();
    }
    logRect("TLED-PATH", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

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
    logRect("TLED-SAVE-BTN", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::EndDisabled();

    // 逃生门：校验没过时也能存（WIP 存档是正常需求），但必须是**显式**
    // 的一次点击，而不是 Save 的默认行为。
    if (!m_checkValid && !m_readOnly) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.28f, 0.18f, 1.0f));
        if (ImGui::Button(TR("Save anyway"))) save(true);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", TR("Write even though the sequence does "
                                       "not load. Use it for work in progress."));
    }

    // ⚠ "New sequence" 挪到**第三行**（动作组）：留在这一行的话，
    //   面板宽度 1060 时它会被窗口右边缘切掉一半 —— 实测过，
    //   而"按钮被裁掉一半"是最难看的一种布局问题。
    //
    // ---- 第二行：时长 / 循环 / 播放头 ----
    ImGui::SetNextItemWidth(110.0f);
    float dur = m_asset.duration;
    if (ImGui::DragFloat(TR("Duration"), &dur, 0.01f, 0.1f, 3600.0f, TR("%.2f s"))) {
        actSetDuration(dur);
    }
    logRect("TLED-DUR", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    ImGui::SameLine();
    bool loop = m_asset.loop;
    if (ImGui::Checkbox(TR("Loop"), &loop)) actSetLoop(loop);

    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    float head = m_head;
    if (ImGui::DragFloat(TR("Playhead"), &head, kTimeStep, 0.0f,
                         std::max(m_asset.duration, 0.1f), TR("%.3f s"))) {
        actSetHead(head);
    }
    logRect("TLED-HEAD", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", TR("Scrubbing only samples the camera at this "
                                   "time — it does not advance or fire events."));

    // ---- 第三行：动作 ----
    if (ImGui::Button(TR("Capture"))) actCaptureKey();
    logRect("TLED-CAPTURE", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", TR("Put a camera key at the playhead using the "
                                   "viewport camera's current pose."));

    ImGui::SameLine();
    if (ImGui::Button(TR("+ Key"))) actAddKey();
    logRect("TLED-ADDKEY", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", TR("Insert a camera key at the playhead, keeping "
                                   "the picture unchanged."));

    ImGui::SameLine();
    if (ImGui::Button(TR("+ Event"))) actAddEvent(kDefaultEventKind);
    logRect("TLED-ADDEVENT", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    ImGui::SameLine();
    if (ImGui::Button(TR("+ Shot"))) actAddShot();
    logRect("TLED-ADDSHOT", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", TR("Cut to the selected camera from the playhead "
                                   "until the next marker."));

    ImGui::SameLine();
    if (ImGui::Button(TR("Delete"))) actDelete();
    logRect("TLED-DEL-BTN", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    ImGui::SameLine();
    if (ImGui::Button(TR("New sequence"))) actNewSequence();
    logRect("TLED-NEW-BTN", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    if (m_readOnly) {
        ImGui::TextColored(kWarnCol, "%s",
                           TR("Read-only — the file did not load, saving is disabled."));
        if (!m_error.empty()) ImGui::TextDisabled("  %s", m_error.c_str());
    } else if (!m_path.empty() && !m_checkValid) {
        ImGui::TextColored(kWarnCol, "%s", TR("Unsaved changes cannot be saved until "
                                              "the sequence loads — see the check "
                                              "bar below."));
    }
    if (!m_uiError.empty()) ImGui::TextColored(kBadCol, "%s", m_uiError.c_str());
}

// ---------------------------------------------------------------- 轨道区

float TimelineEditorWindow::timeToX(const TrackGeom& g, float t) const {
    const float span = std::max(g.dur, 1.0e-3f);
    return g.x0 + (t / span) * (g.x1 - g.x0);
}

float TimelineEditorWindow::xToTime(const TrackGeom& g, float x) const {
    const float w = std::max(g.x1 - g.x0, 1.0f);
    return std::clamp((x - g.x0) / w * g.dur, 0.0f, g.dur);
}

void TimelineEditorWindow::drawTracks() {
    const float kLabelW = 88.0f;
    const float kRulerH = 22.0f;
    const float kRowH = 46.0f;
    const float kPadR = 16.0f;
    const float kPadL = 10.0f;

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    // ⚠ 行数跟着 `kTrackOrder[]` 走，不写死 2。写死的话加一条轨的表现是
    //   "最后一条轨被裁在控件外"——它照样画出来了，但点不到（InvisibleButton
    //   的高度就是命中区）。
    const float h = kRulerH + kRowH * static_cast<float>(kTrackRows) + 4.0f;

    ImGui::InvisibleButton("##tled_tracks", ImVec2(avail.x, h));
    logRect("TLED-TRACKS", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    ImDrawList* dl = ImGui::GetWindowDrawList();

    TrackGeom g;
    g.x0 = origin.x + kLabelW + kPadL;
    g.x1 = origin.x + avail.x - kPadR;
    g.dur = std::max(m_asset.duration, 0.1f);
    g.rowH = kRowH;
    for (int r = 0; r < kTrackRows; ++r)
        g.rowY[r] = origin.y + kRulerH + kRowH * (static_cast<float>(r) + 0.5f);

    // 行号 ↔ 轨道的唯一映射（命中测试、绘制、标签三处都从这里取）
    const int rowCam = rowOf(Track::Camera);
    const int rowShot = rowOf(Track::Shots);
    const int rowEvt = rowOf(Track::Events);

    // 背景：每条轨道一块，交替色让行边界看不出来也不至于糊成一片
    const ImU32 colRowA = ImGui::GetColorU32(ImGuiCol_ChildBg, 0.55f);
    const ImU32 colRowB = ImGui::GetColorU32(ImGuiCol_ChildBg, 0.30f);
    for (int r = 0; r < kTrackRows; ++r) {
        const float y0 = origin.y + kRulerH + kRowH * static_cast<float>(r);
        dl->AddRectFilled(ImVec2(origin.x, y0),
                          ImVec2(origin.x + avail.x, y0 + kRowH),
                          (r % 2 == 0) ? colRowA : colRowB);
    }
    dl->AddRectFilled(ImVec2(origin.x, origin.y + kRulerH),
                      ImVec2(origin.x + kLabelW,
                             origin.y + kRulerH + kRowH * static_cast<float>(kTrackRows)),
                      ImGui::GetColorU32(ImGuiCol_MenuBarBg, 0.9f));

    // ---- 标尺 ----
    // 刻度间距自适应：8 秒的过场画 1 秒一格，120 秒的过场就该画 10 秒一格。
    // ⚠ 固定成"每 1 秒"的话长过场的标尺会糊成一片竖线，短过场只有一条。
    float step = 1.0f;
    const float pxPerSec = (g.x1 - g.x0) / g.dur;
    while (pxPerSec * step < 56.0f) step *= 2.0f;
    const ImU32 colGrid = ImGui::GetColorU32(ImGuiCol_Text, 0.10f);
    const ImU32 colTick = ImGui::GetColorU32(ImGuiCol_Text, 0.45f);
    for (float t = 0.0f; t <= g.dur + 1.0e-4f; t += step) {
        const float x = timeToX(g, t);
        dl->AddLine(ImVec2(x, origin.y + kRulerH), ImVec2(x, origin.y + h), colGrid);
        dl->AddLine(ImVec2(x, origin.y + kRulerH - 5.0f), ImVec2(x, origin.y + kRulerH), colTick);
        char lbl[32];
        std::snprintf(lbl, sizeof(lbl), "%.2gs", static_cast<double>(t));
        dl->AddText(ImVec2(x + 3.0f, origin.y + 3.0f), colTick, lbl);
    }

    // ---- 轨道标签 ----
    const ImU32 colTxt = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 colDim = ImGui::GetColorU32(ImGuiCol_Text, 0.55f);
    for (int r = 0; r < kTrackRows; ++r) {
        const Track tr = trackAtRow(r);
        const char* nm = (tr == Track::Camera) ? TR("Camera")
                       : (tr == Track::Shots)  ? TR("Shots")
                                               : TR("Events");
        const float ly = g.rowY[r] - ImGui::GetFontSize() * 0.5f;
        dl->AddText(ImVec2(origin.x + 10.0f, ly), colTxt, nm);
        char n[32];
        std::snprintf(n, sizeof(n), "%d", trackCount(tr));
        // 数量行压到名字下面一行；`*2` 是"名字 + 间距"，与行高 46 相称
        dl->AddText(ImVec2(origin.x + 10.0f, ly + ImGui::GetFontSize() + 1.0f),
                    colDim, n);
    }

    // ---- 相机轨：先画段（相邻两帧之间的连线），再画点 ----
    const ImU32 colSeg = ImGui::GetColorU32(ImGuiCol_Text, 0.30f);
    const ImU32 colKey = IM_COL32(110, 190, 255, 255);
    const ImU32 colKeySel = IM_COL32(255, 210, 90, 255);
    const ImU32 colKeyOut = IM_COL32(255, 90, 70, 255);   // 超出 duration，永远播不到
    const float camY = g.rowY[rowCam];

    for (std::size_t i = 0; i + 1 < m_asset.cameraKeys.size(); ++i) {
        const auto& a = m_asset.cameraKeys[i];
        const auto& b = m_asset.cameraKeys[i + 1];
        const float xa = timeToX(g, std::min(a.t, g.dur));
        const float xb = timeToX(g, std::min(b.t, g.dur));
        if (xb <= xa + 0.5f) continue;
        if (a.ease == sequence::Ease::Step) {
            // 硬切：虚线。一眼能看出"这一段是切过去，不是飞过去"。
            const float dash = 5.0f;
            for (float x = xa; x < xb; x += dash * 2.0f) {
                const float xe = std::min(x + dash, xb);
                dl->AddLine(ImVec2(x, camY), ImVec2(xe, camY), colSeg, 1.5f);
            }
        } else {
            dl->AddLine(ImVec2(xa, camY), ImVec2(xb, camY), colSeg, 1.5f);
        }
    }
    for (std::size_t i = 0; i < m_asset.cameraKeys.size(); ++i) {
        const auto& k = m_asset.cameraKeys[i];
        const float x = timeToX(g, std::min(k.t, g.dur));
        const bool sel = (m_track == Track::Camera &&
                          static_cast<int>(i) == m_sel);
        const ImU32 c = (k.t > g.dur + 1.0e-3f) ? colKeyOut : (sel ? colKeySel : colKey);
        const float r = sel ? 7.0f : 5.5f;
        // 菱形：与事件的圆点在形状上就分得开（不用靠颜色记）
        const ImVec2 p[4] = {ImVec2(x, camY - r), ImVec2(x + r, camY),
                             ImVec2(x, camY + r), ImVec2(x - r, camY)};
        dl->AddConvexPolyFilled(p, 4, c);
        dl->AddPolyline(p, 4, IM_COL32(20, 20, 24, 220), ImDrawFlags_Closed, 1.2f);
    }

    // ---- 机位轨（多机位 marker）----
    //
    // 与另外两轨的**视觉语义不同**：marker 划的是**区间**（"从这时起用这台"），
    // 不是一个瞬间。所以画法也不同 —— 每个 marker 画一条**横条**延伸到下一个
    // marker 的位置（最后一段延伸到片尾），横条上再插一面小旗。
    // 一眼看过去就是"这几段分别由哪台机位在播"，而不是"这里有个点"。
    const float shotY = g.rowY[rowShot];
    const ImU32 colShotOff = IM_COL32(120, 120, 130, 200);   // 回落关键帧轨
    for (std::size_t i = 0; i < m_asset.shots.size(); ++i) {
        const auto& s = m_asset.shots[i];
        const float xa = timeToX(g, std::min(s.t, g.dur));
        const float xb = (i + 1 < m_asset.shots.size())
                             ? timeToX(g, std::min(m_asset.shots[i + 1].t, g.dur))
                             : g.x1;
        const bool sel = (m_track == Track::Shots && static_cast<int>(i) == m_sel);
        // 颜色由绑定机位的 guid 决定（同一台机位永远同一色）→ 扫一眼就知道
        // "这两段是同一台"。回落关键帧轨（guid=0）用中性灰，避免和真机位撞色。
        const ImU32 c = (s.cameraGuid == 0)
                            ? colShotOff
                            : colorForKind(ecs::guidToHex(s.cameraGuid));
        const float bh = sel ? 10.0f : 7.0f;
        if (xb > xa + 1.0f) {
            dl->AddRectFilled(ImVec2(xa, shotY - bh * 0.5f),
                              ImVec2(xb, shotY + bh * 0.5f), c, 2.0f);
            // 混合时长 > 0 的那一段：左端画一小块渐变条，表示"这里是推过来的"。
            // ⚠ 用**像素宽度**而不是秒数换算：0.2 秒在 8 秒的过场上只有几个像素，
            //   按秒画等于看不见。这里只声明意图，不声称它是精确的时长。
            if (s.blendSeconds > 0.0f && xb > xa + 8.0f) {
                const float bw = std::min(18.0f, (xb - xa) * 0.25f);
                dl->AddRectFilledMultiColor(
                    ImVec2(xa, shotY - bh * 0.5f), ImVec2(xa + bw, shotY + bh * 0.5f),
                    IM_COL32(0, 0, 0, 0), c, c, IM_COL32(0, 0, 0, 0));
            }
        }
        // 小旗：竖杆 + 三角（旗朝右），位置 = marker 的 t
        dl->AddLine(ImVec2(xa, shotY - bh * 0.5f), ImVec2(xa, shotY - bh * 0.5f - 9.0f),
                    c, 1.6f);
        const ImVec2 flag[3] = {ImVec2(xa, shotY - bh * 0.5f - 9.0f),
                                ImVec2(xa + 7.0f, shotY - bh * 0.5f - 6.0f),
                                ImVec2(xa, shotY - bh * 0.5f - 3.0f)};
        dl->AddConvexPolyFilled(flag, 3, c);
        if (sel) {
            dl->AddRect(ImVec2(xa - 2.0f, shotY - bh * 0.5f - 11.0f),
                        ImVec2(std::max(xb, xa + 4.0f) + 2.0f, shotY + bh * 0.5f + 2.0f),
                        colKeySel, 3.0f, 0, 1.6f);
        }
        // 标签：作者写的 label，没写就显示机位的十六进制后四位（够区分，
        // 又不至于把轨道糊满）。⚠ 按码点截断，理由同事件轨。
        if (xb > xa + 16.0f) {
            char lbl[48];
            if (!s.label.empty()) {
                std::snprintf(lbl, sizeof(lbl), "%s", utf8Prefix(s.label, 8).c_str());
            } else if (s.cameraGuid != 0) {
                const std::string h = ecs::guidToHex(s.cameraGuid);
                std::snprintf(lbl, sizeof(lbl), "%s",
                              h.size() > 4 ? h.substr(h.size() - 4).c_str() : h.c_str());
            } else {
                std::snprintf(lbl, sizeof(lbl), "%s", TR("keys"));
            }
            dl->AddText(ImVec2(xa + 4.0f, shotY - ImGui::GetFontSize() * 0.5f),
                        IM_COL32(16, 16, 20, 235), lbl);
        }
    }

    // ---- 事件轨 ----
    const float evtY = g.rowY[rowEvt];
    for (std::size_t i = 0; i < m_asset.events.size(); ++i) {
        const auto& e = m_asset.events[i];
        const float x = timeToX(g, std::min(e.t, g.dur));
        const bool sel = (m_track == Track::Events && static_cast<int>(i) == m_sel);
        const ImU32 c = colorForKind(e.kind);
        const float r = sel ? 7.0f : 5.0f;
        dl->AddCircleFilled(ImVec2(x, evtY), r, c);
        if (sel)
            dl->AddCircle(ImVec2(x, evtY), r + 3.0f, colKeySel, 0, 1.6f);
        // 标签：kind 的前几个码点。
        // ⚠ 不能 `substr(0, 9)`：那是按**字节**切，中文 kind 会被劈成半个
        //   汉字（渲染出来是一串方块）。跟着 UTF-8 的续字节边界走。
        char lbl[40];
        std::snprintf(lbl, sizeof(lbl), "%s", utf8Prefix(e.kind, 9).c_str());
        dl->AddText(ImVec2(x + r + 3.0f, evtY - ImGui::GetFontSize() * 0.5f),
                    colDim, lbl);
    }

    // ---- 播放头 ----
    const float hx = timeToX(g, std::min(m_head, g.dur));
    const ImU32 colHead = IM_COL32(255, 120, 100, 235);
    dl->AddLine(ImVec2(hx, origin.y + kRulerH - 8.0f), ImVec2(hx, origin.y + h), colHead, 1.6f);
    const ImVec2 tri[3] = {ImVec2(hx - 5.0f, origin.y + kRulerH - 14.0f),
                           ImVec2(hx + 5.0f, origin.y + kRulerH - 14.0f),
                           ImVec2(hx, origin.y + kRulerH - 5.0f)};
    dl->AddConvexPolyFilled(tri, 3, colHead);

    // ---- 交互 ----
    // ⚠ 四种手势必须在**按下的那一帧**决定（`m_dragMode`），不能在按住期间
    //   每帧重新判断：鼠标一动，命中测试的结果就变了，拖关键帧会中途变成
    //   拖播放头。
    enum DragMode { kNone = 0, kDragKey = 1, kDragEvent = 2, kDragHead = 3,
                    kDragShot = 4 };
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const ImVec2 mp = ImGui::GetIO().MousePos;
        const float pickR = ImGui::GetFontSize() * 0.55f;
        m_dragMode = kNone;

        // ⚠ 行号 → 轨道只走 `trackAtRow()`：命中测试、绘制、标签三处必须用
        //   同一套映射，各写一份的话"点事件轨选中了机位"这种错会在调整
        //   显示顺序时冒出来。
        const int hoverRow = static_cast<int>((mp.y - origin.y - kRulerH) / kRowH);
        const Track hover = (hoverRow >= 0 && hoverRow < kTrackRows)
                                ? trackAtRow(hoverRow)
                                : Track::Camera;
        const bool inRows = hoverRow >= 0 && hoverRow < kTrackRows;

        if (inRows && hover == Track::Camera) {
            int best = -1;
            float bestDx = pickR;
            for (std::size_t i = 0; i < m_asset.cameraKeys.size(); ++i) {
                const float dx = std::fabs(timeToX(g, std::min(m_asset.cameraKeys[i].t, g.dur)) - mp.x);
                if (dx <= bestDx) { bestDx = dx; best = static_cast<int>(i); }
            }
            if (best >= 0) {
                m_track = Track::Camera;
                m_sel = best;
                m_dragMode = kDragKey;
                VK_LOG_INFO("TLED-SEL track=camera i=%d t=%.3f", m_sel,
                            static_cast<double>(m_asset.cameraKeys[static_cast<std::size_t>(m_sel)].t));
            }
        }

        // 机位轨：**整条横条**都是命中区（不是只有小旗附近）。
        // ⚠ 用"区间"命中而不是"最近点"：横条有几秒宽，作者点在段中间
        //   想的就是"我要这段"，要求精确点到旗子上是没道理的。
        if (m_dragMode == kNone && inRows && hover == Track::Shots) {
            int best = -1;
            for (std::size_t i = 0; i < m_asset.shots.size(); ++i) {
                const float xa = timeToX(g, std::min(m_asset.shots[i].t, g.dur));
                const float xb = (i + 1 < m_asset.shots.size())
                                     ? timeToX(g, std::min(m_asset.shots[i + 1].t, g.dur))
                                     : g.x1;
                // 右端留一点余量：最后一段的 xb 就是右边缘，点在线上也该中
                if (mp.x >= xa - pickR && mp.x <= xb + 2.0f) {
                    best = static_cast<int>(i);
                    break;
                }
            }
            if (best >= 0) {
                m_track = Track::Shots;
                m_sel = best;
                m_bufKey.clear();
                m_dragMode = kDragShot;
                VK_LOG_INFO("TLED-SEL track=shots i=%d t=%.3f", m_sel,
                            static_cast<double>(m_asset.shots[static_cast<std::size_t>(m_sel)].t));
            }
        }

        // 事件轨
        if (m_dragMode == kNone && inRows && hover == Track::Events) {
            int best = -1;
            float bestDx = pickR;
            for (std::size_t i = 0; i < m_asset.events.size(); ++i) {
                const float dx = std::fabs(timeToX(g, std::min(m_asset.events[i].t, g.dur)) - mp.x);
                if (dx <= bestDx) { bestDx = dx; best = static_cast<int>(i); }
            }
            if (best >= 0) {
                m_track = Track::Events;
                m_sel = best;
                m_bufKey.clear();
                m_dragMode = kDragEvent;
                VK_LOG_INFO("TLED-SEL track=events i=%d t=%.3f", m_sel,
                            static_cast<double>(m_asset.events[static_cast<std::size_t>(m_sel)].t));
            }
        }
        // 都没命中 → 拖播放头
        if (m_dragMode == kNone) {
            m_dragMode = kDragHead;
            actSetHead(xToTime(g, mp.x));
        }
    }

    if (m_dragMode != kNone && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const float t = xToTime(g, ImGui::GetIO().MousePos.x);
        const float snapped = std::round(t / kTimeStep) * kTimeStep;
        if (m_dragMode == kDragKey && m_track == Track::Camera) {
            sequence::SeqCameraKey* k = selKey();
            if (k) {
                k->t = snapped;
                m_dirty = true;
            }
        } else if (m_dragMode == kDragEvent && m_track == Track::Events) {
            sequence::SeqEvent* e = selEvent();
            if (e) {
                e->t = snapped;
                m_dirty = true;
            }
        } else if (m_dragMode == kDragShot && m_track == Track::Shots) {
            sequence::SeqShot* s = selShot();
            if (s) {
                s->t = snapped;
                m_dirty = true;
            }
        } else {
            actSetHead(snapped);
        }
    }

    if (m_dragMode != kNone && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        // ① 松手才重排。拖动中就排的话，鼠标下面那一帧会在数组里跳到别处，
        //    `m_sel` 失准 —— 症状是"拖到一半，选中项换成了别人"。
        if (m_dragMode == kDragKey) {
            const int before = m_sel;
            m_sel = reorderKey(m_sel);
            VK_LOG_INFO("TLED-DRAG-END track=camera from=%d to=%d t=%.3f", before,
                        m_sel, static_cast<double>(
                                   m_asset.cameraKeys.empty()
                                       ? 0.0f
                                       : m_asset.cameraKeys[static_cast<std::size_t>(m_sel)].t));
            revalidate();
        } else if (m_dragMode == kDragEvent) {
            const int before = m_sel;
            m_sel = reorderEvent(m_sel);
            m_bufKey.clear();
            VK_LOG_INFO("TLED-DRAG-END track=events from=%d to=%d t=%.3f", before,
                        m_sel, static_cast<double>(
                                   m_asset.events.empty()
                                       ? 0.0f
                                       : m_asset.events[static_cast<std::size_t>(m_sel)].t));
            revalidate();
        } else if (m_dragMode == kDragShot) {
            const int before = m_sel;
            m_sel = reorderShot(m_sel);
            m_bufKey.clear();
            VK_LOG_INFO("TLED-DRAG-END track=shots from=%d to=%d t=%.3f", before,
                        m_sel, static_cast<double>(
                                   m_asset.shots.empty()
                                       ? 0.0f
                                       : m_asset.shots[static_cast<std::size_t>(m_sel)].t));
            revalidate();
        }
        m_dragMode = kNone;
    }
}

// ---------------------------------------------------------------- 属性栏

void TimelineEditorWindow::drawInspectorRow() {
    syncBuffers();

    ImGui::PushID("tled_prop");
    // 高度：bezier 段多两行（说明 + 四个手柄），机位 marker 是三条控件行。
    // 用固定值而不是自适应，是因为这是**属性栏** —— 高度随选择连续跳动比
    // "多两行"更难用。
    // ⚠ 宁可按大给：给少了多出来的那一行会被裁在 Child 外，**按钮照样有矩形
    //   却点不到**（本项目的标准坑，见 MEMORY 的 `logRect 不判可见性`）。
    const sequence::SeqCameraKey* selK =
        (m_track == Track::Camera) ? selKey() : nullptr;
    const bool bezierRow = selK && selK->ease == sequence::Ease::Bezier;
    const float propH = bezierRow ? 158.0f
                      : (m_track == Track::Shots ? 148.0f : 116.0f);
    ImGui::BeginChild("##tled_propbox", ImVec2(0.0f, propH),
                      ImGuiChildFlags_Borders);
    logRect("TLED-PROP", ImGui::GetWindowPos(),
            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                   ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    if (m_track == Track::Camera) {
        sequence::SeqCameraKey* k = selKey();
        if (!k) {
            ImGui::TextDisabled("%s", TR("No camera key selected — press + Key "
                                         "to add one at the playhead."));
        } else {
            float t = k->t;
            ImGui::SetNextItemWidth(110.0f);
            if (ImGui::DragFloat(TR("t"), &t, kTimeStep, 0.0f,
                                 std::max(m_asset.duration, 0.1f), TR("%.3f s"))) {
                actSetTime(t);
            }
            logRect("TLED-PROP-T", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

            ImGui::SameLine();
            float pos[3] = {k->position.x, k->position.y, k->position.z};
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::DragFloat3(TR("pos"), pos, 0.01f, -10000.0f, 10000.0f,
                                  "%.3f")) {
                actSetPos(glm::vec3(pos[0], pos[1], pos[2]));
            }
            logRect("TLED-PROP-POS", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

            // ② UI 用度（作者不会用弧度思考），资产存弧度。
            float yawDeg = k->yaw * kRadToDeg;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100.0f);
            if (ImGui::DragFloat(TR("yaw"), &yawDeg, 0.5f, -360.0f, 360.0f, "%.1f°")) {
                actSetYawDeg(yawDeg);
            }
            ImGui::SameLine();
            float pitchDeg = k->pitch * kRadToDeg;
            ImGui::SetNextItemWidth(100.0f);
            if (ImGui::DragFloat(TR("pitch"), &pitchDeg, 0.5f, -89.0f, 89.0f, "%.1f°")) {
                actSetPitchDeg(pitchDeg);
            }

            ImGui::SameLine();
            float fov = k->fov;
            ImGui::SetNextItemWidth(90.0f);
            if (ImGui::DragFloat(TR("fov"), &fov, 0.25f, 10.0f, 120.0f, "%.1f")) {
                actSetFov(fov);
            }

            // ---- ease：这一段"怎么走" ----
            // ⚠ 选项文字用 `easeName()` 的返回值而不是字面量：那四个词是
            //   格式**关键字**（写进 JSON 的 `ease` 字段），翻了就对不上资产。
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.0f);
            if (ImGui::BeginCombo(TR("ease"), sequence::easeName(k->ease))) {
                // 2026-10-28（R5）：多了 bezier。顺序 = 从"最机械"到"最可调"，
                // 和前三个的历史顺序一致（linear / smooth / step 不动）。
                const sequence::Ease all[4] = {sequence::Ease::Linear,
                                               sequence::Ease::Smooth,
                                               sequence::Ease::Step,
                                               sequence::Ease::Bezier};
                for (sequence::Ease e : all) {
                    const bool sel = (k->ease == e);
                    if (ImGui::Selectable(sequence::easeName(e), sel))
                        actSetEase(sequence::easeName(e));
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            logRect("TLED-PROP-EASE", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

            // ---- 贝塞尔手柄（只有 bezier 段才画）----
            // 归一化段内坐标（见 `SeqCameraKey`）：x = 这段时间的百分之几，
            // y = 进度的百分之几。**四个通道共用同一组手柄**，因为这一段
            // 只有一个标量时间重映射 w。
            if (k->ease == sequence::Ease::Bezier) {
                ImGui::TextDisabled("%s", TR("Bezier handles (normalised: x = 0..1 "
                                             "of this segment, y = 0..1 of the value change)"));
                auto handleDrag = [&](const char* label, bool isOut, int axis,
                                      float speed, float lo, float hi) {
                    float v = axis == 0 ? (isOut ? k->outHandle.x : k->inHandle.x)
                                        : (isOut ? k->outHandle.y : k->inHandle.y);
                    ImGui::SetNextItemWidth(96.0f);
                    if (ImGui::DragFloat(label, &v, speed, lo, hi, "%.3f"))
                        actSetHandle(isOut, axis, v);
                    // 日志打在"松手"那一侧：拖一下会触发几十次 DragFloat，
                    // 每次都打的话 `TLED-` 日志会被自己刷满。
                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        sequence::SeqCameraKey* kk = selKey();
                        if (kk) {
                            VK_LOG_INFO(
                                "TLED-BEZIER t=%.3f out=%.3f,%.3f in=%.3f,%.3f",
                                static_cast<double>(kk->t),
                                static_cast<double>(kk->outHandle.x),
                                static_cast<double>(kk->outHandle.y),
                                static_cast<double>(kk->inHandle.x),
                                static_cast<double>(kk->inHandle.y));
                        }
                    }
                };
                handleDrag(TR("out x"), true, 0, 0.005f, 0.0f, 1.0f);
                ImGui::SameLine();
                handleDrag(TR("out y"), true, 1, 0.01f, -2.0f, 3.0f);
                ImGui::SameLine();
                handleDrag(TR("in x"), false, 0, 0.005f, -1.0f, 0.0f);
                ImGui::SameLine();
                handleDrag(TR("in y"), false, 1, 0.01f, -2.0f, 3.0f);
                logRect("TLED-PROP-BEZIER", ImGui::GetItemRectMin(),
                        ImGui::GetItemRectMax());
            }

            ImGui::TextDisabled(
                TR("%d / %d  |  rad yaw=%.4f pitch=%.4f  |  ease applies to the "
                   "segment that starts here"),
                m_sel + 1, static_cast<int>(m_asset.cameraKeys.size()),
                static_cast<double>(k->yaw), static_cast<double>(k->pitch));
        }
    } else if (m_track == Track::Shots) {
        sequence::SeqShot* s = selShot();
        if (!s) {
            ImGui::TextDisabled("%s", TR("No shot marker selected — press + Shot "
                                         "to cut to a camera at the playhead."));
        } else {
            float t = s->t;
            ImGui::SetNextItemWidth(110.0f);
            if (ImGui::DragFloat(TR("t"), &t, kTimeStep, 0.0f,
                                 std::max(m_asset.duration, 0.1f), TR("%.3f s"))) {
                actSetShotTime(t);
            }
            logRect("TLED-PROP-T", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

            ImGui::SameLine();
            float blend = s->blendSeconds;
            ImGui::SetNextItemWidth(120.0f);
            if (ImGui::DragFloat(TR("blend"), &blend, 0.01f, 0.0f, 3.0f,
                                 TR("%.2f s"))) {
                actSetShotBlend(blend);
            }
            logRect("TLED-PROP-BLEND", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", TR("0 = hard cut. Anything above glides "
                                           "from the previous shot's pose."));

            ImGui::SameLine();
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::InputTextWithHint(TR("label"), TR("shot A — close-up"),
                                         m_labelBuf, sizeof(m_labelBuf),
                                         ImGuiInputTextFlags_EnterReturnsTrue)) {
                actSetShotLabel(m_labelBuf);
            }
            logRect("TLED-PROP-LABEL", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

            // ---- 绑定的机位 ----
            // 显示**名字**而不是十六进制：作者记不住 guid。名字查不到时
            // 明确说"scene 里没有这台"——那是文件从别的场景拷过来时的典型状态。
            ImGui::TextDisabled("%s", TR("Camera:"));
            ImGui::SameLine();
            std::string camName;
            if (s->cameraGuid == 0) {
                camName = TR("(falls back to the camera key track)");
            } else {
                scene::Scene& sc = m_ctx.activeScene();
                const ecs::Entity e = sc.world().find(s->cameraGuid);
                const std::string* nm = e.valid() ? sc.world().name(e) : nullptr;
                camName = nm ? *nm : fmt1(TR("missing (%s)"),
                                          ecs::guidToHex(s->cameraGuid).c_str());
            }
            ImGui::Text("%s", camName.c_str());
            logRect("TLED-PROP-CAM", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

            ImGui::SameLine();
            if (ImGui::Button(TR("Bind to selection"))) actBindShotToSelection();
            logRect("TLED-BIND-SEL", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", TR("Point this marker at the camera that "
                                           "is selected in the scene."));

            ImGui::SameLine();
            if (ImGui::Button(TR("Use keys"))) actBindShotCam(0);
            logRect("TLED-BIND-KEYS", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", TR("Release the override: this stretch is "
                                           "driven by the camera key track again."));

            ImGui::TextDisabled(
                TR("%d / %d  |  active until the next marker (or the end)"),
                m_sel + 1, static_cast<int>(m_asset.shots.size()));
        }
    } else {
        sequence::SeqEvent* e = selEvent();
        if (!e) {
            ImGui::TextDisabled("%s", TR("No event selected — press + Event to "
                                         "add one at the playhead."));
        } else {
            float t = e->t;
            ImGui::SetNextItemWidth(110.0f);
            if (ImGui::DragFloat(TR("t"), &t, kTimeStep, 0.0f,
                                 std::max(m_asset.duration, 0.1f), TR("%.3f s"))) {
                actSetTime(t);
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(150.0f);
            if (ImGui::InputText(TR("kind"), m_kindBuf, sizeof(m_kindBuf),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
                actSetEventKind(m_kindBuf);
            }
            logRect("TLED-PROP-KIND", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            ImGui::SameLine();
            ImGui::SetNextItemWidth(360.0f);
            if (ImGui::InputTextWithHint(TR("payload"), TR("se_footstep / l3 / flag_name"),
                                         m_payloadBuf, sizeof(m_payloadBuf),
                                         ImGuiInputTextFlags_EnterReturnsTrue)) {
                actSetEventPayload(m_payloadBuf);
            }
            logRect("TLED-PROP-PAYLOAD", ImGui::GetItemRectMin(),
                    ImGui::GetItemRectMax());
            ImGui::TextDisabled("%s", TR("Payload is free text — the runtime "
                                         "decides what it means (see "
                                         "docs/cutscene_sequencer.md §3)."));
        }
    }

    ImGui::EndChild();
    ImGui::PopID();
}

// ---------------------------------------------------------------- 校验栏

void TimelineEditorWindow::drawValidation() {
    ImGui::PushID("tled_valid");
    ImGui::BeginChild("##tled_validbox", ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_Borders);
    logRect("TLED-VALID", ImGui::GetWindowPos(),
            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                   ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    // ---- 第一段：运行时校验器的结论（权威）----
    if (m_checkValid) {
        ImGui::TextColored(kOkCol, "%s", TR("Load check: OK"));
        ImGui::SameLine();
        ImGui::TextDisabled(TR("%d camera key(s), %d event(s), %d shot(s), %.2f s"),
                            static_cast<int>(m_asset.cameraKeys.size()),
                            static_cast<int>(m_asset.events.size()),
                            static_cast<int>(m_asset.shots.size()),
                            static_cast<double>(m_asset.duration));
    } else {
        ImGui::TextColored(kBadCol, "%s", TR("Load check: FAILED"));
        if (!m_checkError.empty())
            ImGui::TextColored(kBadCol, "  %s", m_checkError.c_str());
    }
    for (const auto& w : m_checkWarnings)
        ImGui::TextColored(kWarnCol, "  %s", w.c_str());

    // ---- 第二段：编辑器自己的建议（**不是**校验规则）----
    // 与上一段分开摆，是因为效力不同：上面那条不解决就存不了盘，下面这些
    // 只是"你可能想看看"。混在一起会让人以为文件坏了。
    std::vector<std::string> advices;
    if (!m_asset.cameraKeys.empty()) {
        const float lastT = m_asset.cameraKeys.back().t;
        if (lastT > m_asset.duration + 1.0e-3f) {
            // ⚠ 整句一个格式串。拼成"前半句译文 + 数字"的话，中文语序不同，
            //   译者只能译半句（剧本编辑器那边踩过一次，见 DialogueEditorWindow）。
            char msg[192];
            std::snprintf(msg, sizeof(msg),
                          TR("the last camera key is at %.3f s — it is past the "
                             "duration and never plays"),
                          static_cast<double>(lastT));
            advices.push_back(msg);
        }
        if (m_asset.cameraKeys.size() == 1)
            advices.push_back(
                TR("only one camera key — the shot is a freeze frame"));
    }
    for (const auto& e : m_asset.events) {
        if (e.t >= m_asset.duration - 1.0e-4f) {
            char msg[192];
            std::snprintf(msg, sizeof(msg),
                          TR("event '%s' never fires (the interval is [from, to))"),
                          e.kind.c_str());
            advices.push_back(msg);
        }
    }
    // 机位 marker 的两条常见失误。**都只是建议**：绑一台不在场景里的相机
    // 在运行时是"静默回落到相机轨"（`CAM-CUT-MISS` 一条 WARN），文件本身
    // 没坏 —— 从别的场景拷过来时很常见，所以不能拦着不让存。
    for (const auto& s : m_asset.shots) {
        if (s.cameraGuid == 0) {
            char msg[192];
            std::snprintf(msg, sizeof(msg),
                          TR("shot at %.2f s has no camera — it falls back to the "
                             "camera key track"), static_cast<double>(s.t));
            advices.push_back(msg);
        } else {
            scene::Scene& sc = m_ctx.activeScene();
            const ecs::Entity e = sc.world().find(s.cameraGuid);
            if (!e.valid() || !sc.world().get<ecs::CameraComponent>(e)) {
                advices.push_back(fmt1(TR("shot camera %s is not in this scene"),
                                       ecs::guidToHex(s.cameraGuid).c_str()));
            }
        }
        if (s.t >= m_asset.duration - 1.0e-4f) {
            char msg[192];
            std::snprintf(msg, sizeof(msg),
                          TR("shot at %.2f s never plays (the interval is "
                             "[from, to))"), static_cast<double>(s.t));
            advices.push_back(msg);
        }
    }
    if (!advices.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("%s", TR("Advice (not a load error):"));
        for (const auto& a : advices) ImGui::TextDisabled("  %s", a.c_str());
    }

    ImGui::EndChild();
    ImGui::PopID();
}

// ============================================================
// 自动化注入
// ============================================================

void TimelineEditorWindow::runInjectedScript() {
    if (m_injected || !m_loaded) return;
    const char* raw = std::getenv("MYVK_EDITOR_TLED_SCRIPT");
    if (!raw || !raw[0]) return;
    m_injected = true;

    int applied = 0;
    int failed = 0;
    const std::string all(raw);

    std::size_t pos = 0;
    while (pos <= all.size()) {
        const std::size_t semi = all.find(';', pos);
        std::string tok = all.substr(pos, semi == std::string::npos
                                              ? std::string::npos
                                              : semi - pos);
        pos = (semi == std::string::npos) ? all.size() + 1 : semi + 1;
        const std::size_t b = tok.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) continue;
        tok = tok.substr(b);
        while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t'))
            tok.pop_back();
        if (tok.empty()) continue;

        // `名字[:参数]`。参数里允许出现 `:`（路径 `C:/…`、`sel:events:2`），
        // 所以只切第一刀。
        const std::size_t colon = tok.find(':');
        const std::string name = tok.substr(0, colon);
        const std::string arg = (colon == std::string::npos)
                                    ? std::string()
                                    : tok.substr(colon + 1);

        bool ok = false;
        if (name == "sel") {
            // `sel:<camera|shots|events>:<i>`
            const std::size_t c2 = arg.find(':');
            const std::string trk = arg.substr(0, c2);
            const int i = std::atoi(c2 == std::string::npos
                                        ? "0"
                                        : arg.substr(c2 + 1).c_str());
            const Track want = (trk == "events") ? Track::Events
                             : (trk == "shots")  ? Track::Shots
                                                 : Track::Camera;
            if (trk == "camera" || trk == "events" || trk == "shots") {
                m_track = want;
                if (i >= 0 && i < trackCount(want)) {
                    m_sel = i;
                    m_bufKey.clear();
                    m_uiError.clear();
                    ok = true;
                    VK_LOG_INFO("TLED-SEL track=%s i=%d", trk.c_str(), m_sel);
                }
            }
        } else if (name == "addkey") ok = actAddKey();
        else if (name == "capture") ok = actCaptureKey();
        else if (name == "addevent") ok = actAddEvent(arg);
        // ---- 多机位 marker ----
        else if (name == "addshot") ok = actAddShot();
        else if (name == "setshotcam") {
            // `setshotcam:<hex guid>` / `setshotcam:0` = 回落到关键帧轨。
            // ⚠ 走 `parseGuidHex` 而不是 `atoll`：它按 16 位十六进制解析，
            //   `atoll` 会把 `00a1b2c3` 读成十进制 10123 …… 看着"绑上了"，
            //   实际绑的是一个不存在的实体。
            ok = actBindShotCam(ecs::parseGuidHex(arg));
        } else if (name == "setshotcamsel") ok = actBindShotToSelection();
        else if (name == "setshotblend") ok = actSetShotBlend(
            static_cast<float>(std::atof(arg.c_str())));
        else if (name == "setshotlabel") ok = actSetShotLabel(arg);
        else if (name == "setshottime") ok = actSetShotTime(
            static_cast<float>(std::atof(arg.c_str())));
        else if (name == "del") ok = actDelete();
        else if (name == "settime") ok = actSetTime(static_cast<float>(std::atof(arg.c_str())));
        else if (name == "setpos") {
            float x = 0, y = 0, z = 0;
            if (std::sscanf(arg.c_str(), "%f,%f,%f", &x, &y, &z) == 3)
                ok = actSetPos(glm::vec3(x, y, z));
        } else if (name == "setyaw") ok = actSetYawDeg(static_cast<float>(std::atof(arg.c_str())));
        else if (name == "setpitch") ok = actSetPitchDeg(static_cast<float>(std::atof(arg.c_str())));
        else if (name == "setfov") ok = actSetFov(static_cast<float>(std::atof(arg.c_str())));
        else if (name == "setease") ok = actSetEase(arg);
        else if (name == "sethandle") {
            // `sethandle:<out|in>:<x|y>:<值>` —— 归一手柄的单位见
            // `SeqCameraKey`（x 是段内时间比例，y 是进度比例）。
            char side[8] = {0}, axisName[8] = {0};
            float v = 0.0f;
            if (std::sscanf(arg.c_str(), "%7[^:]:%7[^:]:%f", side, axisName, &v) == 3) {
                const bool isOut = std::string(side) == "out";
                const int axis = std::string(axisName) == "x" ? 0 : 1;
                ok = actSetHandle(isOut, axis, v);
                sequence::SeqCameraKey* kk = selKey();
                if (kk) {
                    VK_LOG_INFO("TLED-BEZIER t=%.3f out=%.3f,%.3f in=%.3f,%.3f",
                                static_cast<double>(kk->t),
                                static_cast<double>(kk->outHandle.x),
                                static_cast<double>(kk->outHandle.y),
                                static_cast<double>(kk->inHandle.x),
                                static_cast<double>(kk->inHandle.y));
                }
            }
        }
        else if (name == "setkind") ok = actSetEventKind(arg);
        else if (name == "setpayload") ok = actSetEventPayload(arg);
        else if (name == "setdur") ok = actSetDuration(static_cast<float>(std::atof(arg.c_str())));
        else if (name == "head") ok = actSetHead(static_cast<float>(std::atof(arg.c_str())));
        else if (name == "loop") ok = actSetLoop(arg != "0");
        else if (name == "new") ok = actNewSequence();
        else if (name == "save") ok = save(false);
        else if (name == "saveforce") ok = save(true);
        else if (name == "reload") {
            reload();
            ok = !m_readOnly;
        } else if (name == "saveas") {
            const std::string keep = m_path;
            m_path = arg;
            ok = save(true);
            if (!ok) m_path = keep;
        } else if (name == "check") {
            revalidate();
            ok = m_checkValid;
        } else if (name == "dump") {
            const std::string text = sequence::toJson(m_asset);
            if (const char* dp = std::getenv("MYVK_EDITOR_TLED_DUMP")) {
                if (FILE* f = std::fopen(dp, "wb")) {
                    std::fwrite(text.data(), 1, text.size(), f);
                    std::fclose(f);
                    VK_LOG_INFO("TLED-DUMP path=%s keys=%d bytes=%d ok=1", dp,
                                static_cast<int>(m_asset.cameraKeys.size()),
                                static_cast<int>(text.size()));
                } else {
                    VK_LOG_WARN("TLED-DUMP path=%s ok=0 (cannot open)", dp);
                }
            } else {
                VK_LOG_WARN("TLED-DUMP ok=0 (MYVK_EDITOR_TLED_DUMP unset)");
            }
            ok = true;
        } else {
            VK_LOG_WARN("TLED-SCRIPT-BAD %s", tok.c_str());
        }

        if (ok) ++applied;
        else {
            ++failed;
            VK_LOG_WARN("TLED-SCRIPT-FAIL %s", tok.c_str());
        }
    }

    m_bufKey.clear();
    syncBuffers();
    VK_LOG_INFO("TLED-SCRIPT applied=%d failed=%d", applied, failed);
}

} // namespace editor
