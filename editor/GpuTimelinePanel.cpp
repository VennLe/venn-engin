#include "GpuTimelinePanel.h"

#include "Localization.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ConsolePanel.h"
#include "core/Logger.h"
#include "imgui.h"
#include "render/GpuTimer.h"

namespace editor {

namespace {

// 段配色 —— 与 Stats 面板（E15）**同一套**。两处不一致的话，用户在两个
// 面板之间来回看会以为是不同的东西。
const ImVec4 kSegCol[GpuTimelinePanel::kSections] = {
    {0.90f, 0.47f, 0.27f, 1.0f},  // shadows
    {0.86f, 0.31f, 0.63f, 1.0f},  // local_sh
    {0.47f, 0.71f, 1.00f, 1.0f},  // depth
    {0.35f, 0.86f, 0.67f, 1.0f},  // decals
    {0.94f, 0.78f, 0.31f, 1.0f},  // ssao
    {0.63f, 0.47f, 1.00f, 1.0f},  // cluster
    {0.31f, 0.78f, 0.47f, 1.0f},  // forward
    {1.00f, 0.55f, 0.15f, 1.0f},  // particles (R6-1)
    {0.27f, 0.59f, 0.90f, 1.0f},  // framegraph
    {0.78f, 0.78f, 0.35f, 1.0f},  // offscreen
    {0.59f, 0.59f, 0.63f, 1.0f},  // post
};

// 段与段之间那段"没被任何时戳覆盖"的时间（resetFrame → 首个 begin、
// 段之间、末个 end → 帧尾）。画成灰色，紧贴在时间轴右端。
const ImVec4 kGapCol{0.32f, 0.32f, 0.35f, 1.0f};

ImU32 u32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }

// 亮度提升（选中/悬停用）—— 与 ImGui 的 Hovered 语义一致：不换色相，
// 只提亮，保证"这还是同一个段"。
ImVec4 brighten(const ImVec4& c, float f) {
    return ImVec4(std::min(c.x * f, 1.0f), std::min(c.y * f, 1.0f),
                  std::min(c.z * f, 1.0f), c.w);
}

}  // namespace

// ---------------------------------------------------------------- 采样

void GpuTimelinePanel::sample(const render::GpuTimer* gt) {
    if (!gt || !gt->valid()) return;
    const float total = gt->lastTotalMs();
    // < 0 = 首帧还没读回；超 500ms = GpuTimer 认为读数不可信（它会自己
    // 丢弃）。两种都不该进历史 —— 一条假帧会把平均和 P95 一起带偏。
    if (!(total > 0.0f) || total > 500.0f) return;

    TimelineSample s;
    s.total = total;
    for (int i = 0; i < kSections; ++i) {
        const float ms = gt->lastMs(static_cast<uint32_t>(i));
        s.ms[i] = (ms > 0.0f) ? ms : 0.0f;
        s.segSum += s.ms[i];
    }
    // seg_sum 理论上 ≤ total（段是帧的子区间）。若因读数抖动反了，夹一下
    // —— 甘特图要靠这个不变量决定 gap 的宽度。
    if (s.segSum > s.total) s.segSum = s.total;

    m_hist[m_head] = s;
    m_head = (m_head + 1) % kMaxHistory;
    if (m_count < kMaxHistory) ++m_count;
}

const TimelineSample* GpuTimelinePanel::historyAt(int i) const {
    if (i < 0 || i >= m_count) return nullptr;
    const int idx =
        ((m_head - m_count + i) % kMaxHistory + kMaxHistory) % kMaxHistory;
    return &m_hist[idx];
}

// ---------------------------------------------------------------- 统计

float GpuTimelinePanel::lastTotal() const {
    const TimelineSample* s = historyAt(m_count - 1);
    return s ? s->total : -1.0f;
}

float GpuTimelinePanel::lastSegSum() const {
    const TimelineSample* s = historyAt(m_count - 1);
    return s ? s->segSum : -1.0f;
}

float GpuTimelinePanel::lastGap() const {
    const TimelineSample* s = historyAt(m_count - 1);
    return s ? (s->total - s->segSum) : -1.0f;
}

float GpuTimelinePanel::averageTotal() const {
    if (m_count <= 0) return -1.0f;
    float sum = 0.0f;
    for (int i = 0; i < m_count; ++i) sum += m_hist[i].total;
    return sum / static_cast<float>(m_count);
}

float GpuTimelinePanel::maxTotal() const {
    if (m_count <= 0) return -1.0f;
    float mx = 0.0f;
    for (int i = 0; i < m_count; ++i) mx = std::max(mx, m_hist[i].total);
    return mx;
}

int GpuTimelinePanel::hottestSection() const {
    const TimelineSample* s = historyAt(m_count - 1);
    if (!s) return -1;
    int best = -1;
    float bv = 0.0f;
    for (int i = 0; i < kSections; ++i) {
        if (s->ms[i] > bv) { bv = s->ms[i]; best = i; }
    }
    return best;
}

const char* GpuTimelinePanel::selectedName() const {
    if (m_selected < 0 || m_selected >= kSections) return "-";
    return render::GpuTimer::sectionName(static_cast<uint32_t>(m_selected));
}

// ---------------------------------------------------------------- 选中 / 联动

void GpuTimelinePanel::selectSection(int s, bool click) {
    if (s < 0 || s >= kSections) return;
    m_selected = s;
    if (!click) return;
    const char* name = render::GpuTimer::sectionName(static_cast<uint32_t>(s));
    m_filterReq = name;  // 静态字符串（sectionName 返回字面量），不用拷贝
    const float ms = (historyCount() > 0) ? historyAt(m_count - 1)->ms[s] : 0.0f;
    VK_LOG_INFO("GPU-TIMELINE-CLICK seg=%s index=%d ms=%.3f", name, s,
                static_cast<double>(ms));
}

const char* GpuTimelinePanel::consumeFilterRequest() {
    const char* r = m_filterReq;
    m_filterReq = nullptr;
    return r;
}

void applyTimelineFilter(editor::ConsolePanel* console, const char* text) {
    if (!console || !text) return;
    console->setSearchFilter(text);
}

void GpuTimelinePanel::logSnapshot() const {
    if (m_count <= 0) return;
    const TimelineSample* s = historyAt(m_count - 1);
    const int hot = hottestSection();
    const char* hotName =
        (hot >= 0) ? render::GpuTimer::sectionName(static_cast<uint32_t>(hot))
                   : "-";
    const float hotMs = (hot >= 0) ? s->ms[hot] : 0.0f;
    const float gap = s->total - s->segSum;
    // 一条日志给全脚本需要的东西：帧数（历史长度）、last（= GPU-TIMING 的
    // frame 值）、seg_sum（甘特图可画部分）、gap（差值）、avg/max（抖动）、
    // hot（最贵段）、sel（当前选中）。key=value，与 GPU-TIMING 同风格。
    VK_LOG_INFO(
        "GPU-TIMELINE frames=%d last=%.3f seg_sum=%.3f gap=%.3f avg=%.3f "
        "max=%.3f hot=%s:%.3f sel=%s",
        m_count, static_cast<double>(s->total), static_cast<double>(s->segSum),
        static_cast<double>(gap), static_cast<double>(averageTotal()),
        static_cast<double>(maxTotal()), hotName,
        static_cast<double>(hotMs), selectedName());
}

// ---------------------------------------------------------------- 自动化钩子

void GpuTimelinePanel::driveHooks(int editorFrame) {
    if (m_clickHookDone >= 0) return;
    const char* want = std::getenv("MYVK_EDITOR_GPUTIMELINE_CLICK");
    if (!want || !want[0]) return;
    // 等有数据了再点（否则点了个还没有段的空图）
    if (editorFrame < 50 || m_count <= 0) return;
    for (int i = 0; i < kSections; ++i) {
        if (std::strcmp(render::GpuTimer::sectionName(
                            static_cast<uint32_t>(i)), want) == 0) {
            m_clickHookDone = i;
            selectSection(i, true);
            return;
        }
    }
    // 未知段名：记一次拒绝，别每帧重试
    m_clickHookDone = -2;
    VK_LOG_WARN("MYVK_EDITOR_GPUTIMELINE_CLICK: 未知段名 '%s'", want);
}

// ---------------------------------------------------------------- 绘制

void GpuTimelinePanel::draw(bool* open, ConsolePanel* console) {
    ImGui::SetNextWindowPos(ImVec2(40.0f, 320.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(680.0f, 420.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(420.0f, 240.0f),
                                        ImVec2(FLT_MAX, FLT_MAX));

    if (!ImGui::Begin(trId("GPU Timeline", "panel_gputimeline"), open)) {
        ImGui::End();
        return;
    }

    if (m_count <= 0) {
        ImGui::TextDisabled(TR("Waiting for GPU timestamp data... (R0-1 query pool not read back yet)"));
        ImGui::End();
        return;
    }

    const TimelineSample* cur = historyAt(m_count - 1);
    const float avg = averageTotal();
    const float mx = maxTotal();
    const float gap = cur->total - cur->segSum;

    // ---- 概览一行 ----
    ImGui::Text(TR("GPU frame: %.2f ms"), static_cast<double>(cur->total));
    ImGui::SameLine();
    ImGui::TextDisabled(TR("| avg %.2f | max %.2f | %d frames"),
                        static_cast<double>(avg), static_cast<double>(mx),
                        m_count);
    if (gap > 0.0005f) {
        ImGui::SameLine();
        ImGui::TextDisabled(TR("| gap %.3f ms"), static_cast<double>(gap));
    }

    const ImGuiIO& io = ImGui::GetIO();
    const float barW = std::max(ImGui::GetContentRegionAvail().x, 100.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // ================= 本帧甘特图 =================
    ImGui::SeparatorText(TR("Frame timeline (hover to read, click to filter)"));
    {
        const float barH = 26.0f;
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##gantt", ImVec2(barW, barH));
        const bool hov = ImGui::IsItemHovered();
        const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);

        // 反查鼠标落在哪一段（比例空间 → 累计比较）。落在 gap 区 → -1。
        int hoverSeg = -1;
        if (hov && cur->total > 0.0f) {
            const float rel =
                (io.MousePos.x - p0.x) / std::max(barW, 1.0f);
            float acc = 0.0f;
            for (int s = 0; s < kSections; ++s) {
                acc += cur->ms[s] / cur->total;
                if (rel <= acc) { hoverSeg = s; break; }
            }
        }

        float acc = 0.0f;
        for (int s = 0; s < kSections; ++s) {
            const float frac =
                (cur->total > 0.0f) ? cur->ms[s] / cur->total : 0.0f;
            if (barW * frac < 0.5f) { acc += frac; continue; }
            const float x0 = p0.x + barW * acc;
            const float x1 = p0.x + barW * (acc + frac);
            ImVec4 c = kSegCol[s];
            if (s == hoverSeg) c = brighten(c, 1.35f);
            if (s == m_selected) c = brighten(c, 1.2f);
            dl->AddRectFilled(ImVec2(x0, p0.y), ImVec2(x1, p0.y + barH), u32(c));
            if (s == m_selected)
                dl->AddRect(ImVec2(x0, p0.y), ImVec2(x1, p0.y + barH),
                            IM_COL32(255, 255, 255, 220), 0.0f, 0, 2.0f);
            acc += frac;
        }
        // gap：右端余下那条（没被任何时戳覆盖的时间）
        if (acc < 1.0f) {
            dl->AddRectFilled(ImVec2(p0.x + barW * acc, p0.y),
                              ImVec2(p0.x + barW, p0.y + barH), u32(kGapCol));
        }
        dl->AddRect(p0, ImVec2(p0.x + barW, p0.y + barH),
                    IM_COL32(0, 0, 0, 140));

        if (hov && hoverSeg >= 0) {
            ImGui::SetTooltip(
                TR("%s\n%.3f ms  (%.1f%%)\navg over %d frames: %.3f ms"),
                render::GpuTimer::sectionName(
                    static_cast<uint32_t>(hoverSeg)),
                static_cast<double>(cur->ms[hoverSeg]),
                static_cast<double>(100.0f * cur->ms[hoverSeg] /
                                    std::max(cur->total, 1e-6f)),
                m_count, [&] {
                    float a = 0.0f;
                    for (int i = 0; i < m_count; ++i)
                        a += historyAt(i)->ms[hoverSeg];
                    return static_cast<double>(a / std::max(m_count, 1));
                }());
        } else if (hov) {
            ImGui::SetTooltip(TR("gap (frame time not covered by segments or timestamps)\n%.3f ms"),
                              static_cast<double>(gap));
        }
        if (clicked) {
            selectSection(hoverSeg, true);  // hoverSeg == -1 时安全忽略
            if (hoverSeg >= 0 && console) {
                console->setSearchFilter(
                    render::GpuTimer::sectionName(
                        static_cast<uint32_t>(hoverSeg)));
            }
        }
    }

    // ================= 最近 60 帧堆叠 =================
    ImGui::SeparatorText(TR("History (oldest top -> newest bottom)"));
    {
        const float rowH = 4.0f;
        const float rowsH = rowH * static_cast<float>(m_count);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##hist", ImVec2(barW, rowsH));
        const bool hov = ImGui::IsItemHovered();
        // 鼠标行号 → 历史下标（自上而下 = 旧到新）
        int hoverRow = -1;
        if (hov) {
            const int r = static_cast<int>((io.MousePos.y - p0.y) / rowH);
            if (r >= 0 && r < m_count) hoverRow = r;
        }
        for (int i = 0; i < m_count; ++i) {
            const TimelineSample* s = historyAt(i);
            const float y0 = p0.y + rowH * static_cast<float>(i);
            const float y1 = y0 + rowH - 1.0f;
            float a = 0.0f;
            for (int k = 0; k < kSections; ++k) {
                const float frac =
                    (s->total > 0.0f) ? s->ms[k] / s->total : 0.0f;
                if (barW * frac < 0.4f) { a += frac; continue; }
                ImVec4 c = kSegCol[k];
                // 选中段在历史里复用同一"标记"色 —— 于是"这段的曲线"
                // 在纵向一眼可读；非选中段压暗，弱化干扰。
                if (m_selected >= 0 && k != m_selected) c = brighten(c, 0.55f);
                dl->AddRectFilled(ImVec2(p0.x + barW * a, y0),
                                  ImVec2(p0.x + barW * (a + frac), y1), u32(c));
                a += frac;
            }
            if (a < 1.0f)
                dl->AddRectFilled(ImVec2(p0.x + barW * a, y0),
                                  ImVec2(p0.x + barW, y1), u32(kGapCol));
        }
        // 悬停某一行 → 该帧的总耗时（纵向读数的落点）
        if (hoverRow >= 0) {
            const TimelineSample* s = historyAt(hoverRow);
            dl->AddRect(ImVec2(p0.x, p0.y + rowH * hoverRow),
                        ImVec2(p0.x + barW, p0.y + rowH * (hoverRow + 1)),
                        IM_COL32(255, 255, 255, 200), 0.0f, 0, 1.0f);
            ImGui::SetTooltip(TR("frame -%d\n%.3f ms%s"), m_count - 1 - hoverRow,
                              static_cast<double>(s->total),
                              (s == cur) ? "\n(newest)" : "");
        }
    }

    // ================= 段明细 =================
    ImGui::SeparatorText(TR("Sections"));
    if (ImGui::BeginTable("##segs", 4,
                          ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("section", ImGuiTableColumnFlags_WidthFixed,
                                130.0f);
        ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("share", ImGuiTableColumnFlags_WidthFixed,
                                70.0f);
        ImGui::TableSetupColumn(TRL("avg (history)"),
                                ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (int s = 0; s < kSections; ++s) {
            const float ms = cur->ms[s];
            float sum = 0.0f;
            for (int i = 0; i < m_count; ++i) sum += historyAt(i)->ms[s];
            const float a = sum / static_cast<float>(std::max(m_count, 1));
            const bool active = (ms > 0.0005f);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::ColorButton("##c", active ? kSegCol[s]
                                             : ImVec4(0.28f, 0.28f, 0.30f, 1.0f),
                               ImGuiColorEditFlags_NoTooltip,
                               ImVec2(10.0f, 10.0f));
            ImGui::SameLine();
            // 段名可点 = 与点甘特图同一个动作（两处入口一条路径）
            const char* sname =
                render::GpuTimer::sectionName(static_cast<uint32_t>(s));
            char label[64];
            std::snprintf(label, sizeof(label), "%s##seg%d", sname, s);
            if (s == m_selected) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
            }
            const bool hit = ImGui::SmallButton(label);
            if (s == m_selected) ImGui::PopStyleColor();
            if (hit) {
                selectSection(s, true);
                if (console) console->setSearchFilter(sname);
            }
            ImGui::TableSetColumnIndex(1);
            if (active) ImGui::Text(TR("%.3f"), static_cast<double>(ms));
            else ImGui::TextDisabled(TR("0"));
            ImGui::TableSetColumnIndex(2);
            if (active && cur->total > 0.0f)
                ImGui::Text(TR("%.1f%%"),
                            static_cast<double>(100.0f * ms / cur->total));
            else ImGui::TextDisabled(TR("-"));
            ImGui::TableSetColumnIndex(3);
            ImGui::TextDisabled(TR("%.3f"), static_cast<double>(a));
        }
        ImGui::EndTable();
    }

    // ================= 底部动作 =================
    if (ImGui::Button(TR("Clear history"))) {
        m_count = 0;
        m_head = 0;
        m_selected = -1;
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Reset selection"))) m_selected = -1;
    ImGui::SameLine();
    ImGui::TextDisabled(TR("| Click a section to filter Console by its name"));

    // 待联动的过滤词：宿主（EditorApp）在 Console 就绪时取走。
    // 这里不直接调 console->setSearchFilter —— 保持"面板之间不互相引用"，
    // 也让"点了但 Console 还没建"这种顺序问题自然消解。
    if (const char* req = consumeFilterRequest()) {
        if (console) console->setSearchFilter(req);
    }

    ImGui::End();
}

}  // namespace editor
