// ============================================================
// editor/AnimatorGraphPanel.cpp
// ============================================================

#include "AnimatorGraphPanel.h"

#include "EditorContext.h"
#include "Localization.h"
#include "PanelDock.h"

#include "assets/AssetPath.h"
#include "core/Logger.h"
#include "scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace editor {

namespace {

using animation::AnimController;
using animation::CondOp;
using animation::ControllerState;
using animation::ControllerTransition;
using animation::ParamType;
using animation::TransitionCondition;

// 节点尺寸（画布单位，缩放前）。宽 168 是为了装下 "Walk (Walk)" 这种
// "状态名 + 片段名" 都写得下还不换行的最短宽度。
constexpr float kNodeW = 168.0f;
constexpr float kNodeH = 62.0f;
constexpr float kNodeRounding = 6.0f;

// 命中的像素容差。8 是"看着连上了就点得着"的下限：再小要在 4K 屏上
// 拿放大镜点线，再大两条平行边会互相抢。
constexpr float kEdgeHitPx = 8.0f;

const ImU32 kColNode = IM_COL32(58, 62, 74, 255);
const ImU32 kColNodeSel = IM_COL32(84, 104, 148, 255);
const ImU32 kColNodeDefault = IM_COL32(74, 92, 62, 255);
const ImU32 kColBorder = IM_COL32(120, 128, 145, 255);
const ImU32 kColText = IM_COL32(226, 230, 238, 255);
const ImU32 kColTextDim = IM_COL32(150, 158, 172, 255);
const ImU32 kColEdge = IM_COL32(150, 160, 180, 255);
const ImU32 kColEdgeSel = IM_COL32(240, 190, 90, 255);
const ImU32 kColAny = IM_COL32(200, 130, 220, 255);

// 条件算符的显示名。与 `condOpName()` 分开：那边是**存盘用**的 schema
// 字面量（`">="`），这边是给人看的。两者很容易被合并成一份，
// 然后某天有人把 UI 文案改成 `≥` —— 资产文件里就出现了 `"op": "≥"`，
// 解析器不认识 → 整个控制器不接管。
const char* const kOpLabels[] = {">", ">=", "<", "<=", "==", "!=", "trigger"};
constexpr int kOpCount = 7;

const char* const kTypeLabels[] = {"float", "bool", "trigger"};
constexpr int kTypeCount = 3;

CondOp opFromIndex(int i) {
    i = std::max(0, std::min(kOpCount - 1, i));
    return static_cast<CondOp>(i);
}

int indexOfOp(CondOp op) {
    return std::max(0, std::min(kOpCount - 1, static_cast<int>(op)));
}

ParamType typeFromIndex(int i) {
    i = std::max(0, std::min(kTypeCount - 1, i));
    return static_cast<ParamType>(i);
}

// 状态表里按名字找下标；找不到给 -1（用于"删除后修正引用"）。
int indexOfStateName(const AnimController& c, const std::string& n) {
    return c.findState(n);
}

}  // namespace

// ---------------------------------------------------------------- 打开 / 加载

void AnimatorGraphPanel::open(const std::string& path) {
    m_open = true;
    if (!path.empty() && path != m_path) {
        m_path = path;
        loadFromDisk();
    } else if (m_ctl.states.empty() && !m_path.empty()) {
        loadFromDisk();
    }
    std::snprintf(m_pathBuf, sizeof(m_pathBuf), "%s", m_path.c_str());
}

void AnimatorGraphPanel::close() { m_open = false; }

void AnimatorGraphPanel::loadFromDisk() {
    m_sel = Sel::None;
    m_selState = -1;
    m_selTrans = -1;
    m_dragNode = -1;
    m_pannedOnce = false;

    auto ctl = animation::loadAnimController(m_path);
    m_ctl = *ctl;   // 拷贝：面板里改的是副本，按"保存"才落盘
    m_dirty = false;

    if (!m_ctl.error.empty()) {
        m_status = m_ctl.error;
        m_statusIsError = true;
        VK_LOG_WARN("ANIMGRAPH-LOAD-FAIL path=%s err=%s", m_path.c_str(),
                    m_ctl.error.c_str());
        return;
    }

    autoLayout();

    m_status = "Loaded";
    m_statusIsError = false;
    VK_LOG_INFO("ANIMGRAPH-LOAD path=%s states=%d trans=%d params=%d default=%s",
                m_path.c_str(), static_cast<int>(m_ctl.states.size()),
                static_cast<int>(m_ctl.transitions.size()),
                static_cast<int>(m_ctl.params.size()),
                m_ctl.stateName(m_ctl.defaultState));
}

void AnimatorGraphPanel::saveToDisk() {
    if (m_path.empty()) {
        m_status = "No path set";
        m_statusIsError = true;
        return;
    }

    // 先把"这份东西到底能不能跑"验一遍 —— 只看不改。
    // 用序列化结果去反解析，是最诚实的验证：它同时检查了 serialize 和
    // parse 两侧（只验内存结构的话，序列化里写错一个字段名是抓不到的）。
    const std::string text = animation::serializeAnimController(m_ctl);
    auto back = animation::parseAnimController(text);
    const std::string verifyErr = back->error;

    const std::string err = animation::saveAnimController(m_ctl, m_path);
    if (!err.empty()) {
        m_status = err;
        m_statusIsError = true;
        VK_LOG_ERROR("ANIMGRAPH-SAVE-FAIL path=%s err=%s", m_path.c_str(),
                     err.c_str());
        return;
    }

    m_dirty = false;
    if (verifyErr.empty()) {
        m_status = "Saved";
        m_statusIsError = false;
        VK_LOG_INFO("ANIMGRAPH-SAVE path=%s bytes=%d states=%d trans=%d",
                    m_path.c_str(), static_cast<int>(text.size()),
                    static_cast<int>(m_ctl.states.size()),
                    static_cast<int>(m_ctl.transitions.size()));
    } else {
        // 存了、但跑不起来。这个状态必须显眼 —— 见头文件第 ② 条。
        m_status = "Saved, but the file will not load: " + verifyErr;
        m_statusIsError = true;
        VK_LOG_WARN("ANIMGRAPH-SAVE-INVALID path=%s err=%s", m_path.c_str(),
                    verifyErr.c_str());
    }
}

void AnimatorGraphPanel::newController() {
    m_ctl = AnimController{};
    m_ctl.defaultState = -1;
    m_dirty = true;
    m_sel = Sel::None;
    m_selState = -1;
    m_selTrans = -1;
    m_pannedOnce = false;
    m_status = "New controller (unsaved)";
    m_statusIsError = false;
    VK_LOG_INFO("ANIMGRAPH-NEW");
}

void AnimatorGraphPanel::autoLayout() {
    const size_t n = m_ctl.states.size();
    m_ctl.nodePos.resize(n, {0.0f, 0.0f});

    // 判"需要布点"：只要有一个 (0,0) 就当整份都没布过。
    // 逐个补会得到"几个在原位、几个堆在原点"的图 —— 比全重排更难看懂。
    bool allPlaced = true;
    for (size_t i = 0; i < n; ++i) {
        if (m_ctl.nodePos[i][0] == 0.0f && m_ctl.nodePos[i][1] == 0.0f) {
            allPlaced = false;
            break;
        }
    }
    if (allPlaced && n > 0) return;

    // 环形排布。半径按"节点沿圆周排开不打架"倒推：
    // 每个节点占 kNodeW + 32 的弧长，n 个就是 2πr ≥ n·(kNodeW+32)，
    // 于是 r ≈ n·33。**不能按 90·n 走** —— 那是 4 个状态就画出半径 360
    // 的环、直径 720 单位，一屏根本装不下，表现是"画布上只有几条穿过的
    // 线，节点全在边界外"（2026-11-01 第一版就是这个）。
    const float r = std::max(150.0f, 34.0f * static_cast<float>(n));
    const float cx = 0.0f, cy = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        const float a = static_cast<float>(i) / static_cast<float>(n) *
                        6.2831853f - 1.5707963f;   // 从正上方开始
        m_ctl.nodePos[i] = {cx + std::cos(a) * r, cy + std::sin(a) * r};
    }
}

// ---------------------------------------------------------------- 坐标

ImVec2 AnimatorGraphPanel::toScreen(const ImVec2& canvasPos,
                                    const ImVec2& p) const {
    return ImVec2(canvasPos.x + m_pan.x + p.x * m_zoom,
                  canvasPos.y + m_pan.y + p.y * m_zoom);
}

ImVec2 AnimatorGraphPanel::toCanvas(const ImVec2& canvasPos,
                                    const ImVec2& screen) const {
    return ImVec2((screen.x - canvasPos.x - m_pan.x) / m_zoom,
                  (screen.y - canvasPos.y - m_pan.y) / m_zoom);
}

int AnimatorGraphPanel::hitNode(const ImVec2& canvasPos,
                                const ImVec2& p) const {
    // 逆序遍历 = 后画的（视觉上在上层的）先命中。
    for (int i = static_cast<int>(m_ctl.states.size()) - 1; i >= 0; --i) {
        const auto& np = m_ctl.nodePos[static_cast<size_t>(i)];
        // 节点以 nodePos 为**中心**
        const ImVec2 c(np[0], np[1]);
        const float hw = kNodeW * 0.5f, hh = kNodeH * 0.5f;
        if (p.x >= c.x - hw && p.x <= c.x + hw && p.y >= c.y - hh &&
            p.y <= c.y + hh)
            return i;
    }
    return -1;
}

void AnimatorGraphPanel::transitionCurve(const AnimController& ctl, int ti,
                                         ImVec2* a, ImVec2* c1, ImVec2* c2,
                                         ImVec2* b) const {
    const ControllerTransition& t = ctl.transitions[static_cast<size_t>(ti)];

    // 终点：目标节点中心
    const auto& tp = ctl.nodePos[static_cast<size_t>(t.to)];
    *b = ImVec2(tp[0], tp[1]);

    if (t.from >= 0) {
        const auto& fp = ctl.nodePos[static_cast<size_t>(t.from)];
        *a = ImVec2(fp[0], fp[1]);
    } else {
        // "Any" 边：从一个虚拟源点出发。源点放在整张图的左上外侧 ——
        // 这样所有 Any 边像从一个"入口"射出去，一眼能看出"这些是
        // 任意状态都能进的"。若改成一堆从各自节点出发的线，就与普通
        // 转移完全分不开（而它们语义差别很大：Any 边不需要源状态活跃）。
        float minx = 0.0f, miny = 0.0f;
        for (size_t i = 0; i < ctl.nodePos.size(); ++i) {
            minx = std::min(minx, ctl.nodePos[i][0]);
            miny = std::min(miny, ctl.nodePos[i][1]);
        }
        *a = ImVec2(minx - kNodeW * 0.5f - 30.0f,
                    miny - kNodeH * 0.5f - 30.0f);
    }

    // 控制点：沿"起→终"方向各推 1/3 距离，再加垂直偏移让往复的两条边
    // （A→B 与 B→A）分得开。
    //
    // ⚠ 偏移量必须**按边长收**（`min(30, len·0.12)`），而且方向要跟着
    //   `from < to` 取正负。第一版写死 34 且恒为正，两个相邻节点之间的
    //   往返边会被推成一个小圈 —— 画面上是几个浮在节点中间的椭圆，
    //   完全看不出谁连谁。
    const float dx = b->x - a->x, dy = b->y - a->y;
    const float len = std::sqrt(dx * dx + dy * dy) + 1.0e-3f;
    float off = 0.0f;
    if (t.from >= 0) {
        off = std::min(30.0f, len * 0.12f) * (t.from < t.to ? 1.0f : -1.0f);
    }
    const float px = -dy / len * off, py = dx / len * off;
    *c1 = ImVec2(a->x + dx * 0.32f + px, a->y + dy * 0.32f + py);
    *c2 = ImVec2(a->x + dx * 0.68f + px, a->y + dy * 0.68f + py);
}

int AnimatorGraphPanel::hitTransition(const ImVec2& canvasPos,
                                      const ImVec2& p) const {
    const float tol = kEdgeHitPx / std::max(m_zoom, 0.2f);
    int best = -1;
    float bestD = tol;

    for (int i = 0; i < static_cast<int>(m_ctl.transitions.size()); ++i) {
        ImVec2 a, c1, c2, b;
        transitionCurve(m_ctl, i, &a, &c1, &c2, &b);
        // 采样 24 段足够：最短的边（两个相邻节点）也有 ~100 单位长，
        // 24 段内每段 4 单位，比容差小。
        constexpr int kSteps = 24;
        ImVec2 prev = a;
        for (int s = 1; s <= kSteps; ++s) {
            const float u = static_cast<float>(s) / kSteps;
            const float iu = 1.0f - u;
            const ImVec2 q(
                iu * iu * iu * a.x + 3 * iu * iu * u * c1.x +
                    3 * iu * u * u * c2.x + u * u * u * b.x,
                iu * iu * iu * a.y + 3 * iu * iu * u * c1.y +
                    3 * iu * u * u * c2.y + u * u * u * b.y);
            // 点到线段距离
            const float vx = q.x - prev.x, vy = q.y - prev.y;
            const float wx = p.x - prev.x, wy = p.y - prev.y;
            const float vv = vx * vx + vy * vy;
            float t = vv > 1e-6f ? (wx * vx + wy * vy) / vv : 0.0f;
            t = std::max(0.0f, std::min(1.0f, t));
            const float ddx = wx - vx * t, ddy = wy - vy * t;
            const float d = std::sqrt(ddx * ddx + ddy * ddy);
            if (d < bestD) {
                bestD = d;
                best = i;
            }
            prev = q;
        }
    }
    return best;
}

// ---------------------------------------------------------------- 增删

void AnimatorGraphPanel::addState() {
    ControllerState s;
    s.name = "State" + std::to_string(m_ctl.states.size());
    // 片段名先跟状态名一致 —— 这是 90% 的情况，剩下 10% 再改。
    s.clip = s.name;
    m_ctl.states.push_back(s);

    // 新节点落在当前视图中间偏右下，不跟已有的撞。
    ImVec2 pos(0.0f, 0.0f);
    if (!m_ctl.nodePos.empty()) {
        pos = ImVec2(m_ctl.nodePos.back()[0] + kNodeW * 1.3f,
                     m_ctl.nodePos.back()[1]);
    }
    m_ctl.nodePos.push_back({pos.x, pos.y});

    if (m_ctl.defaultState < 0) m_ctl.defaultState = 0;
    m_sel = Sel::State;
    m_selState = static_cast<int>(m_ctl.states.size()) - 1;
    markDirty();
    VK_LOG_INFO("ANIMGRAPH-ADD-STATE name=%s idx=%d", s.name.c_str(),
                m_selState);
}

void AnimatorGraphPanel::addTransition(int from, int to) {
    if (to < 0 || to >= static_cast<int>(m_ctl.states.size())) return;
    ControllerTransition t;
    t.from = from;
    t.to = to;
    // 默认 0.15s 淡入。硬切（0）看起来像丢帧，默认值不该是那个。
    t.duration = 0.15f;
    m_ctl.transitions.push_back(t);
    m_sel = Sel::Transition;
    m_selTrans = static_cast<int>(m_ctl.transitions.size()) - 1;
    markDirty();
    VK_LOG_INFO("ANIMGRAPH-ADD-TRANS from=%s to=%s idx=%d",
                from < 0 ? "Any" : m_ctl.stateName(from),
                m_ctl.stateName(to), m_selTrans);
}

void AnimatorGraphPanel::removeSelected() {
    if (m_sel == Sel::State && m_selState >= 0) {
        const int gone = m_selState;
        // 先把名字抄下来 —— erase 之后再 stateName(gone) 拿到的是**被删
        // 状态的原位继承者**（或 "<none>"），日志会指着一个仍然存在的状态。
        const std::string goneName = m_ctl.stateName(gone);
        m_ctl.states.erase(m_ctl.states.begin() + gone);
        if (gone < static_cast<int>(m_ctl.nodePos.size()))
            m_ctl.nodePos.erase(m_ctl.nodePos.begin() + gone);

        // 所有引用这个状态的转移一起删 —— 留一条指向不存在状态的边，
        // 文件就再也加载不回来了（validate 会判无效），而用户只是想
        // "删掉一个状态"。这是删状态唯一合理的语义。
        int removed = 0;
        for (int i = static_cast<int>(m_ctl.transitions.size()) - 1; i >= 0;
             --i) {
            const auto& t = m_ctl.transitions[static_cast<size_t>(i)];
            if (t.from == gone || t.to == gone) {
                m_ctl.transitions.erase(m_ctl.transitions.begin() + i);
                ++removed;
            }
        }
        // 剩下的转移下标整体前移（from/to 存的是下标！）
        for (auto& t : m_ctl.transitions) {
            if (t.from > gone) --t.from;
            if (t.to > gone) --t.to;
        }
        if (m_ctl.defaultState == gone) m_ctl.defaultState = 0;
        else if (m_ctl.defaultState > gone) --m_ctl.defaultState;
        if (m_ctl.states.empty()) m_ctl.defaultState = -1;

        m_sel = Sel::None;
        m_selState = -1;
        markDirty();
        VK_LOG_INFO("ANIMGRAPH-DEL-STATE idx=%d name=%s dropsTrans=%d", gone,
                    goneName.c_str(), removed);
        return;
    }
    if (m_sel == Sel::Transition && m_selTrans >= 0) {
        m_ctl.transitions.erase(m_ctl.transitions.begin() + m_selTrans);
        VK_LOG_INFO("ANIMGRAPH-DEL-TRANS idx=%d", m_selTrans);
        m_sel = Sel::None;
        m_selTrans = -1;
        markDirty();
    }
}

// ---------------------------------------------------------------- 绘制

void AnimatorGraphPanel::draw(EditorContext& ctx) {
    if (!m_open) return;

    // ⚠ 必须走 `applyDockHint`（见 editor/PanelDock.h）：本面板的
    //   `layoutSlot` 是 `kSlotNone` = 会被托管成**视口标签页**。不接这个
    //   提示的话，被设成当前标签页时窗口仍按"浮动"画在左上角，
    //   整个编辑器界面被它盖住（2026-11-01 第一次跑就是这个）。
    //
    // ⚠ 标题栏的 × 只能在**非托管**时给（`docked ? nullptr : &m_open`）：
    //   托管时关闭按钮在标签条上，两个 × 会让人以为"关一次不够"。
    const ImGuiIO& io = ImGui::GetIO();
    ImGuiWindowFlags wf = 0;
    const bool docked = applyDockHint(wf);
    if (!docked) {
        // 尺寸给足：左边参数表 + 中间画布 + 右边属性，三栏并排。
        ImGui::SetNextWindowSize(
            ImVec2(glm::clamp(io.DisplaySize.x * 0.72f, 900.0f, 1320.0f),
                   glm::clamp(io.DisplaySize.y * 0.74f, 520.0f, 880.0f)),
            ImGuiCond_FirstUseEver);
    }

    // 稳定 ID 必须与 `trayWindowName("animgraph")` 一致，否则底栏托盘的
    // 最小化 / 置顶按 id 找不到这个窗口。
    if (!ImGui::Begin(trId("State machine", "panel_animgraph"),
                      docked ? nullptr : &m_open, wf)) {
        ImGui::End();
        return;
    }

    drawToolbar();
    drawContent(ctx);
    ImGui::End();
}

void AnimatorGraphPanel::drawContent(EditorContext& ctx) {
    (void)ctx;
    // 三栏：参数 | 画布（自适应）| 属性
    //
    // 参数列的下限 260 不是随手取的：一行里要同时放下
    // `名字(84) + 类型(60) + 默认值(46) + 删除(x)` 加 4 段 ItemSpacing，
    // 200px 会把默认值那一格挤出可视区（第一版就是这样，看起来像
    // "float 参数没有默认值输入框"）。
    const float avail = ImGui::GetContentRegionAvail().x;
    const float paramW = glm::clamp(avail * 0.24f, 264.0f, 340.0f);
    const float inspW = glm::clamp(avail * 0.26f, 260.0f, 380.0f);
    const float canvasW = std::max(160.0f, avail - paramW - inspW - 20.0f);
    const float bodyH = ImGui::GetContentRegionAvail().y - 26.0f;

    ImGui::BeginChild("##params", ImVec2(paramW, bodyH), true);
    drawParameterTable();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##canvasbox", ImVec2(canvasW, bodyH), false);
    drawCanvas(ImGui::GetContentRegionAvail());
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##insp", ImVec2(inspW, bodyH), true);
    drawInspector();
    ImGui::EndChild();

    drawStatusLine();
}

void AnimatorGraphPanel::drawToolbar() {
    ImGui::SetNextItemWidth(340.0f);
    if (ImGui::InputText("##path", m_pathBuf, sizeof(m_pathBuf),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        m_path = m_pathBuf;
        loadFromDisk();
    }
    // Enter 是"载入"，不是"改路径" —— 改完路径按 Enter 就该看到内容。
    ImGui::SameLine();

    if (ImGui::Button(TR("Load"))) {
        m_path = m_pathBuf;
        loadFromDisk();
    }
    ImGui::SameLine();
    // 脏了就标个星：这是唯一提示"你有未保存改动"的地方。
    const char* saveLabel = m_dirty ? TR("Save *") : TR("Save");
    if (ImGui::Button(saveLabel)) saveToDisk();
    ImGui::SameLine();
    if (ImGui::Button(TR("New"))) {
        m_path = m_pathBuf;   // 保留路径：新建后直接存到这儿是常见流程
        newController();
    }

    ImGui::SameLine();
    // 竖分隔线。`SeparatorEx` 在 1.93 里没有导出到公开头（它属于
    // imgui_internal.h），而为一个装饰去 include 内部头不值当 ——
    // 一个竖线字符效果一样。
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (ImGui::Button(TR("+ State"))) addState();
    ImGui::SameLine();
    ImGui::BeginDisabled(m_sel == Sel::None);
    if (ImGui::Button(TR("Delete"))) removeSelected();
    ImGui::EndDisabled();

    ImGui::SameLine();
    // 现在这个文件到底会不会被运行时接管 —— 面板里能直接看到，
    // 不用去翻日志。
    if (m_ctl.valid()) {
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "%s",
                           TR("valid"));
    } else {
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f), "%s",
                           TR("invalid"));
    }
    if (ImGui::IsItemHovered() && !m_ctl.error.empty())
        ImGui::SetTooltip("%s", m_ctl.error.c_str());

    ImGui::TextDisabled(
        "%s", TR("Edits apply when the asset is reloaded (not live)."));
}

void AnimatorGraphPanel::drawParameterTable() {
    ImGui::TextUnformatted(TR("Parameters"));
    ImGui::SameLine();
    if (ImGui::SmallButton(TR("+##param"))) {
        animation::ControllerParam p;
        p.name = "Param" + std::to_string(m_ctl.params.size());
        m_ctl.params.push_back(p);
        markDirty();
        VK_LOG_INFO("ANIMGRAPH-ADD-PARAM name=%s", p.name.c_str());
    }
    ImGui::Separator();

    int removeAt = -1;
    for (int i = 0; i < static_cast<int>(m_ctl.params.size()); ++i) {
        auto& p = m_ctl.params[static_cast<size_t>(i)];
        ImGui::PushID(i);

        char nb[64];
        std::snprintf(nb, sizeof(nb), "%s", p.name.c_str());
        ImGui::SetNextItemWidth(74.0f);
        // 改名要连带改所有引用它的条件 —— 否则条件指向一个不存在的
        // 参数，validate 判无效、整个状态机不接管。
        if (ImGui::InputText("##name", nb, sizeof(nb),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            const std::string old = p.name;
            const std::string now(nb);
            if (!now.empty() && now != old) {
                for (auto& t : m_ctl.transitions)
                    for (auto& c : t.conditions)
                        if (c.param == old) c.param = now;
                p.name = now;
                markDirty();
                VK_LOG_INFO("ANIMGRAPH-RENAME-PARAM %s -> %s", old.c_str(),
                            now.c_str());
            }
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(84.0f);
        int ti = static_cast<int>(p.type);
        if (ImGui::Combo("##type", &ti, kTypeLabels, kTypeCount)) {
            p.type = typeFromIndex(ti);
            // 换成 trigger 时把默认值清掉：trigger 恒 false，
            // 留个 1.0 在那里会让人以为"默认就触发"。
            if (p.type == ParamType::Trigger) p.defaultValue = 0.0f;
            markDirty();
        }

        ImGui::SameLine();
        if (p.type == ParamType::Float) {
            ImGui::SetNextItemWidth(46.0f);
            if (ImGui::DragFloat("##def", &p.defaultValue, 0.01f, -100.0f,
                                 100.0f, "%.2f"))
                markDirty();
        } else if (p.type == ParamType::Bool) {
            bool v = p.defaultValue != 0.0f;
            if (ImGui::Checkbox("##def", &v)) {
                p.defaultValue = v ? 1.0f : 0.0f;
                markDirty();
            }
        } else {
            ImGui::TextDisabled("%s", TR("(event)"));
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("x")) removeAt = i;
        ImGui::PopID();
    }

    if (removeAt >= 0) {
        // 删参数时把引用它的**条件**删掉，但**保留转移**（一条没有条件的
        // 转移是合法的 = 无条件永远成立）。反过来整条删会误伤。
        const std::string gone =
            m_ctl.params[static_cast<size_t>(removeAt)].name;
        m_ctl.params.erase(m_ctl.params.begin() + removeAt);
        for (auto& t : m_ctl.transitions) {
            t.conditions.erase(
                std::remove_if(t.conditions.begin(), t.conditions.end(),
                               [&](const TransitionCondition& c) {
                                   return c.param == gone;
                               }),
                t.conditions.end());
        }
        markDirty();
        VK_LOG_INFO("ANIMGRAPH-DEL-PARAM name=%s", gone.c_str());
    }

    ImGui::Spacing();
    ImGui::Separator();
    // ⚠ 不能用 `TextDisabled("%s", TR("States: %d ..."), n1, n2)`：格式串是
    //   "%s"，多余参数被丢掉、%d 原样印出来（第一版就这样，面板上写着
    //   "States: %d   Transitions: %d"）。先 snprintf 成 buffer 再交给
    //   TextDisabled("%s")，也顺手避开 format-security 警告。
    char cnt[128];
    std::snprintf(cnt, sizeof(cnt), TR("States: %d   Transitions: %d"),
                  static_cast<int>(m_ctl.states.size()),
                  static_cast<int>(m_ctl.transitions.size()));
    ImGui::TextDisabled("%s", cnt);
}

void AnimatorGraphPanel::drawCanvas(ImVec2 size) {
    if (size.x < 32.0f || size.y < 32.0f) return;

    const ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // 背景 + 网格：让"我拖到了哪里"有个参照。20px 一档太重，40 合适。
    dl->AddRectFilled(canvasPos,
                      ImVec2(canvasPos.x + size.x, canvasPos.y + size.y),
                      IM_COL32(28, 30, 36, 255));
    const float step = 40.0f * m_zoom;
    if (step > 6.0f) {
        for (float x = std::fmod(m_pan.x, step); x < size.x; x += step)
            dl->AddLine(ImVec2(canvasPos.x + x, canvasPos.y),
                        ImVec2(canvasPos.x + x, canvasPos.y + size.y),
                        IM_COL32(40, 43, 51, 255));
        for (float y = std::fmod(m_pan.y, step); y < size.y; y += step)
            dl->AddLine(ImVec2(canvasPos.x, canvasPos.y + y),
                        ImVec2(canvasPos.x + size.x, canvasPos.y + y),
                        IM_COL32(40, 43, 51, 255));
    }

    ImGui::InvisibleButton("##canvas", size,
                           ImGuiButtonFlags_MouseButtonLeft |
                               ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();

    // ---- 缩放 / 平移 ----
    if (hovered && ImGui::GetIO().MouseWheel != 0.0f) {
        // 以鼠标位置为锚点缩放（否则缩放时图会往左上跑）。
        const ImVec2 m = ImGui::GetIO().MousePos;
        const ImVec2 before = toCanvas(canvasPos, m);
        m_zoom = glm::clamp(m_zoom * (1.0f + ImGui::GetIO().MouseWheel * 0.12f),
                            0.35f, 2.5f);
        const ImVec2 after = toCanvas(canvasPos, m);
        m_pan.x += (after.x - before.x) * m_zoom;
        m_pan.y += (after.y - before.y) * m_zoom;
    }
    if (ImGui::IsItemActive() &&
        (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
         ImGui::IsMouseDragging(ImGuiMouseButton_Right))) {
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        m_pan.x += d.x;
        m_pan.y += d.y;
    }

    // 首次绘制把视图**适配**到节点的包围盒：先按包围盒算缩放，再居中。
    //
    // 只做"平移 + 假定中心在 (0,0)"是不够的：环形布点确实以原点为心，
    // 但一张手改过 `editor.nodes` 的图可以有任意包围盒，而状态一多，
    // 环的直径也会超过画布 —— 那时无论怎么居中都有节点在边界外。
    if (!m_pannedOnce && !m_ctl.nodePos.empty()) {
        float minx = m_ctl.nodePos[0][0], maxx = minx;
        float miny = m_ctl.nodePos[0][1], maxy = miny;
        for (const auto& p : m_ctl.nodePos) {
            minx = std::min(minx, p[0]);
            maxx = std::max(maxx, p[0]);
            miny = std::min(miny, p[1]);
            maxy = std::max(maxy, p[1]);
        }
        const float spanX = (maxx - minx) + kNodeW + 60.0f;
        const float spanY = (maxy - miny) + kNodeH + 60.0f;
        if (spanX > 1.0f && spanY > 1.0f) {
            const float fit =
                std::min(size.x / spanX, size.y / spanY);
            m_zoom = glm::clamp(fit, 0.35f, 1.0f);
        }
        const float cx = (minx + maxx) * 0.5f, cy = (miny + maxy) * 0.5f;
        m_pan = ImVec2(size.x * 0.5f - cx * m_zoom,
                       size.y * 0.5f - cy * m_zoom);
        m_pannedOnce = true;
    }

    // ---- 命中 / 选择 / 拖动 ----
    const ImVec2 mouseCanvas = toCanvas(canvasPos, ImGui::GetIO().MousePos);

    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        const int n = hitNode(canvasPos, mouseCanvas);
        if (n >= 0) {
            m_sel = Sel::State;
            m_selState = n;
            m_dragNode = n;
            const auto& np = m_ctl.nodePos[static_cast<size_t>(n)];
            m_dragOffset = ImVec2(mouseCanvas.x - np[0],
                                  mouseCanvas.y - np[1]);
        } else {
            const int t = hitTransition(canvasPos, mouseCanvas);
            m_sel = t >= 0 ? Sel::Transition : Sel::None;
            m_selTrans = t;
            m_selState = -1;
            m_dragNode = -1;
        }
    }

    // 拖动节点。用 IsItemActive 而不是 IsMouseDragging 判"还在按着" ——
    // 鼠标拖到画布外时前者仍然成立，后者会中途断开（节点卡在边上）。
    if (m_dragNode >= 0 && active) {
        auto& np = m_ctl.nodePos[static_cast<size_t>(m_dragNode)];
        np[0] = mouseCanvas.x - m_dragOffset.x;
        np[1] = mouseCanvas.y - m_dragOffset.y;
        m_dirty = true;
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) m_dragNode = -1;

    // 右键菜单：从某个状态新建一条边 / 设默认 / 删。
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        const int n = hitNode(canvasPos, mouseCanvas);
        if (n >= 0) {
            m_sel = Sel::State;
            m_selState = n;
            ImGui::OpenPopup("##nodectx");
        } else {
            ImGui::OpenPopup("##canvasctx");
        }
    }
    if (ImGui::BeginPopup("##nodectx")) {
        if (m_selState >= 0) {
            ImGui::TextDisabled("%s", m_ctl.stateName(m_selState));
            ImGui::Separator();
            if (ImGui::MenuItem(TR("Set as default state"))) {
                m_ctl.defaultState = m_selState;
                markDirty();
                VK_LOG_INFO("ANIMGRAPH-SET-DEFAULT name=%s",
                            m_ctl.stateName(m_selState));
            }
            if (ImGui::BeginMenu(TR("Add transition to..."))) {
                for (int i = 0; i < static_cast<int>(m_ctl.states.size());
                     ++i) {
                    if (i == m_selState) continue;
                    if (ImGui::MenuItem(m_ctl.stateName(i)))
                        addTransition(m_selState, i);
                }
                ImGui::Separator();
                if (ImGui::MenuItem(TR("(Any)")))
                    addTransition(-1, m_selState);
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem(TR("Delete state"))) removeSelected();
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##canvasctx")) {
        if (ImGui::MenuItem(TR("Add state"))) addState();
        if (ImGui::MenuItem(TR("Reset view"))) {
            m_pan = ImVec2(size.x * 0.5f, size.y * 0.5f);
            m_zoom = 1.0f;
        }
        ImGui::EndPopup();
    }

    // ---- 画边（先画，节点盖在上面）----
    const ImVec2 clipMin = canvasPos;
    const ImVec2 clipMax(canvasPos.x + size.x, canvasPos.y + size.y);
    dl->PushClipRect(clipMin, clipMax, true);

    for (int i = 0; i < static_cast<int>(m_ctl.transitions.size()); ++i) {
        ImVec2 a, c1, c2, b;
        transitionCurve(m_ctl, i, &a, &c1, &c2, &b);
        const bool sel = (m_sel == Sel::Transition && m_selTrans == i);
        const ImU32 col = m_ctl.transitions[static_cast<size_t>(i)].from < 0
                              ? kColAny
                              : (sel ? kColEdgeSel : kColEdge);

        // 端点收到**节点矩形的边界**上：从中心沿"奔向对方"的方向射出，
        // 求与矩形的交点。
        //
        // ⚠ 不能按固定半宽回收（第一版就是 `中心 + halfW`）：斜着连的时候
        //   回收点会落到矩形角外，而短边上回收掉的长度占比过大 ——
        //   两端一夹，剩下的曲线长度还不够控制点的偏移量，整条边就
        //   退化成一个小圈。
        const ImVec2 sa = toScreen(canvasPos, a);
        const ImVec2 sb = toScreen(canvasPos, b);
        const float hwS = kNodeW * 0.5f * m_zoom;
        const float hhS = kNodeH * 0.5f * m_zoom;
        auto edgePoint = [](ImVec2 c, ImVec2 toward, float hw, float hh) {
            const float ex = toward.x - c.x, ey = toward.y - c.y;
            const float ax = std::abs(ex), ay = std::abs(ey);
            float t = 1.0e9f;
            if (ax > 1.0e-4f) t = std::min(t, hw / ax);
            if (ay > 1.0e-4f) t = std::min(t, hh / ay);
            if (t > 1.0e8f) return c;
            return ImVec2(c.x + ex * t, c.y + ey * t);
        };
        const ImVec2 p0 = m_ctl.transitions[static_cast<size_t>(i)].from < 0
                              ? sa
                              : edgePoint(sa, sb, hwS, hhS);
        const ImVec2 p3 = edgePoint(sb, sa, hwS, hhS);
        const ImVec2 s1 = toScreen(canvasPos, c1);
        const ImVec2 s2 = toScreen(canvasPos, c2);

        dl->AddBezierCubic(p0, s1, s2, p3, col, sel ? 2.6f : 1.7f);

        // 箭头（三角），方向取末端切线
        {
            const float tx = p3.x - s2.x, ty = p3.y - s2.y;
            const float tl = std::sqrt(tx * tx + ty * ty) + 1.0e-3f;
            const float ux = tx / tl, uy = ty / tl;
            const float ar = 7.0f;
            const ImVec2 tip(p3.x + ux * ar, p3.y + uy * ar);
            const ImVec2 q1(p3.x - uy * ar * 0.55f, p3.y + ux * ar * 0.55f);
            const ImVec2 q2(p3.x + uy * ar * 0.55f, p3.y - ux * ar * 0.55f);
            dl->AddTriangleFilled(tip, q1, q2, col);
        }
    }

    // ---- 画节点 ----
    for (int i = 0; i < static_cast<int>(m_ctl.states.size()); ++i) {
        const auto& np = m_ctl.nodePos[static_cast<size_t>(i)];
        const ImVec2 c = toScreen(canvasPos, ImVec2(np[0], np[1]));
        const float hw = kNodeW * 0.5f * m_zoom;
        const float hh = kNodeH * 0.5f * m_zoom;
        const ImVec2 mn(c.x - hw, c.y - hh), mx(c.x + hw, c.y + hh);

        const bool sel = (m_sel == Sel::State && m_selState == i);
        const bool isDefault = (m_ctl.defaultState == i);
        ImU32 fill = sel ? kColNodeSel : (isDefault ? kColNodeDefault : kColNode);

        dl->AddRectFilled(mn, mx, fill, kNodeRounding * m_zoom);
        dl->AddRect(mn, mx, isDefault ? IM_COL32(150, 200, 110, 255) : kColBorder,
                    kNodeRounding * m_zoom, 0, isDefault ? 2.4f : 1.2f);

        // 文本只在缩放到可读时画 —— 0.5 倍以下字会糊成一团墨点，
        // 反而看不到结构。
        if (m_zoom > 0.5f) {
            const std::string& nm = m_ctl.states[static_cast<size_t>(i)].name;
            const std::string& clip =
                m_ctl.states[static_cast<size_t>(i)].clip;
            const ImU32 tcol = kColText;
            const ImVec2 ts(mn.x + 10.0f, mn.y + 8.0f);
            dl->AddText(ts, tcol, nm.c_str());
            const std::string sub = clip.empty() ? std::string("-")
                                                 : ("clip: " + clip);
            dl->AddText(ImVec2(mn.x + 10.0f, mn.y + 30.0f), kColTextDim,
                        sub.c_str());
            if (isDefault)
                dl->AddText(ImVec2(mx.x - 62.0f, mn.y + 8.0f),
                            IM_COL32(170, 220, 130, 255), "start");
        }
    }

    // "Any" 徽章：Any 边的虚拟源点在哪，就画在哪。
    bool hasAny = false;
    for (const auto& t : m_ctl.transitions)
        if (t.from < 0) hasAny = true;
    if (hasAny && !m_ctl.nodePos.empty()) {
        ImVec2 a, c1, c2, b;
        // 找任意一条 Any 边取它的起点（同一个函数算，保证与线对齐）
        for (int i = 0; i < static_cast<int>(m_ctl.transitions.size()); ++i) {
            if (m_ctl.transitions[static_cast<size_t>(i)].from < 0) {
                transitionCurve(m_ctl, i, &a, &c1, &c2, &b);
                break;
            }
        }
        const ImVec2 s = toScreen(canvasPos, a);
        dl->AddCircleFilled(s, 16.0f * m_zoom, kColAny);
        dl->AddText(ImVec2(s.x - 13.0f * m_zoom, s.y - 7.0f * m_zoom),
                    IM_COL32(30, 20, 34, 255), "Any");
    }

    dl->PopClipRect();
}

void AnimatorGraphPanel::drawInspector() {
    if (m_sel == Sel::State && m_selState >= 0 &&
        m_selState < static_cast<int>(m_ctl.states.size())) {
        auto& s = m_ctl.states[static_cast<size_t>(m_selState)];
        ImGui::TextUnformatted(TR("State"));
        ImGui::Separator();

        char nb[96];
        std::snprintf(nb, sizeof(nb), "%s", s.name.c_str());
        if (ImGui::InputText(TR("Name"), nb, sizeof(nb),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            s.name = nb;   // 名字只在状态列表里自引用，转移存的是下标，不用改
            markDirty();
        }
        char cb[128];
        std::snprintf(cb, sizeof(cb), "%s", s.clip.c_str());
        if (ImGui::InputText(TR("Clip"), cb, sizeof(cb),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            s.clip = cb;
            markDirty();
        }
        if (ImGui::DragFloat(TR("Speed"), &s.speed, 0.01f, 0.05f, 4.0f, "%.2f"))
            markDirty();
        if (ImGui::Checkbox(TR("Loop"), &s.loop)) markDirty();

        ImGui::Spacing();
        const bool isDefault = (m_ctl.defaultState == m_selState);
        ImGui::BeginDisabled(isDefault);
        if (ImGui::Button(TR("Set as default"))) {
            m_ctl.defaultState = m_selState;
            markDirty();
            VK_LOG_INFO("ANIMGRAPH-SET-DEFAULT name=%s", s.name.c_str());
        }
        ImGui::EndDisabled();
        if (isDefault) ImGui::TextDisabled("%s", TR("This is the entry state."));

        ImGui::Spacing();
        ImGui::Separator();
        // 这个状态进出的边 —— "为什么它切不过去"多半在这里就能看出来。
        ImGui::TextDisabled("%s", TR("Outgoing"));
        for (int i = 0; i < static_cast<int>(m_ctl.transitions.size()); ++i) {
            const auto& t = m_ctl.transitions[static_cast<size_t>(i)];
            if (t.from != m_selState) continue;
            if (ImGui::Selectable((std::string("-> ") +
                                   m_ctl.stateName(t.to)).c_str())) {
                m_sel = Sel::Transition;
                m_selTrans = i;
            }
        }
        ImGui::TextDisabled("%s", TR("Incoming"));
        for (int i = 0; i < static_cast<int>(m_ctl.transitions.size()); ++i) {
            const auto& t = m_ctl.transitions[static_cast<size_t>(i)];
            if (t.to != m_selState || t.from == m_selState) continue;
            const std::string label =
                (t.from < 0 ? std::string("(Any)") : m_ctl.stateName(t.from)) +
                " ->";
            if (ImGui::Selectable(label.c_str())) {
                m_sel = Sel::Transition;
                m_selTrans = i;
            }
        }
        return;
    }

    if (m_sel == Sel::Transition && m_selTrans >= 0 &&
        m_selTrans < static_cast<int>(m_ctl.transitions.size())) {
        auto& t = m_ctl.transitions[static_cast<size_t>(m_selTrans)];
        ImGui::TextUnformatted(TR("Transition"));
        ImGui::Separator();

        // from / to 用下拉改。to 不允许选自己（validate 会判无效），
        // 所以列表里跳过当前 from —— 让"做不到的事"在 UI 上不存在，
        // 比让人点了之后看红字好。
        ImGui::Text("%s: %s", TR("From"),
                    t.from < 0 ? "(Any)" : m_ctl.stateName(t.from));
        ImGui::Text("%s: %s", TR("To"), m_ctl.stateName(t.to));

        if (ImGui::BeginCombo(TR("Change target"),
                              m_ctl.stateName(t.to))) {
            for (int i = 0; i < static_cast<int>(m_ctl.states.size()); ++i) {
                if (i == t.from) continue;
                if (ImGui::Selectable(m_ctl.stateName(i), i == t.to)) {
                    t.to = i;
                    markDirty();
                }
            }
            ImGui::EndCombo();
        }

        if (ImGui::DragFloat(TR("Duration"), &t.duration, 0.005f, 0.0f, 2.0f,
                             TR("%.3f s")))
            markDirty();
        if (ImGui::Checkbox(TR("Has exit time"), &t.hasExitTime)) markDirty();
        if (t.hasExitTime) {
            if (ImGui::SliderFloat(TR("Exit at"), &t.exitTime, 0.0f, 1.0f,
                                   "%.2f"))
                markDirty();
        }
        if (ImGui::Checkbox(TR("Restart"), &t.restart)) markDirty();
        if (!t.restart)
            ImGui::TextDisabled(
                "%s", TR("Phase continues from the current clip time."));

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextUnformatted(TR("Conditions (all must hold)"));
        ImGui::SameLine();
        if (ImGui::SmallButton(TR("+##cond"))) {
            TransitionCondition c;
            if (!m_ctl.params.empty())
                c.param = m_ctl.params.front().name;
            t.conditions.push_back(c);
            markDirty();
        }

        int dropCond = -1;
        for (int i = 0; i < static_cast<int>(t.conditions.size()); ++i) {
            auto& c = t.conditions[static_cast<size_t>(i)];
            ImGui::PushID(i);

            // 参数下拉：只列已声明的（条件引用未知参数 = 控制器无效）
            const char* curName = c.param.empty() ? "(none)" : c.param.c_str();
            ImGui::SetNextItemWidth(96.0f);
            if (ImGui::BeginCombo("##p", curName)) {
                for (const auto& p : m_ctl.params) {
                    if (ImGui::Selectable(p.name.c_str(), p.name == c.param)) {
                        c.param = p.name;
                        markDirty();
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::SameLine();
            ImGui::SetNextItemWidth(64.0f);
            int oi = indexOfOp(c.op);
            if (ImGui::Combo("##op", &oi, kOpLabels, kOpCount)) {
                c.op = opFromIndex(oi);
                markDirty();
            }

            // trigger 没有值可比
            if (c.op != CondOp::Trigger) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(56.0f);
                if (ImGui::DragFloat("##v", &c.value, 0.01f, -100.0f, 100.0f,
                                     "%.2f"))
                    markDirty();
            } else {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", TR("(fires)"));
            }

            ImGui::SameLine();
            if (ImGui::SmallButton("x")) dropCond = i;
            ImGui::PopID();
        }
        if (dropCond >= 0) {
            t.conditions.erase(t.conditions.begin() + dropCond);
            markDirty();
        }
        if (t.conditions.empty())
            ImGui::TextDisabled("%s",
                                TR("No conditions = always true."));
        return;
    }

    ImGui::TextDisabled("%s", TR("Select a state or a transition."));
    ImGui::Spacing();
    if (!m_ctl.error.empty()) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.45f, 1.0f), "%s",
                           TR("Load / validation error:"));
        ImGui::TextWrapped("%s", m_ctl.error.c_str());
    }
}

void AnimatorGraphPanel::drawStatusLine() {
    ImGui::Separator();
    const ImVec4 col = m_statusIsError ? ImVec4(0.95f, 0.5f, 0.45f, 1.0f)
                                       : ImVec4(0.6f, 0.7f, 0.8f, 1.0f);
    ImGui::TextColored(col, "%s", m_status.c_str());
    if (m_dirty) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", TR("(unsaved changes)"));
    }
}

// ---------------------------------------------------------------- 内嵌

void AnimatorGraphPanel::drawBody(EditorContext& ctx, bool embedded) {
    (void)embedded;
    drawToolbar();
    drawContent(ctx);
}

}  // namespace editor
