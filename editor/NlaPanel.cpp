// ============================================================
// editor/NlaPanel.cpp —— NLA 时间轴面板
//
// 语义与三条 UI 约束全部写在头文件里。这个文件只做三件事：
//   ① 去场景里取目标实体的 `AnimatorComponent`（面板**不持有数据**）；
//   ② 把时间轴画出来 + 让鼠标改 `start/end/播放头`；
//   ③ 一条注入脚本通路（`MYVK_EDITOR_NLA_SCRIPT`），让验收不依赖像素坐标。
//
// 为什么交互全在 drawBody 里手写（不用一排 ImGui 控件）：
//   时间轴是**二维画布** —— 横轴是时间、纵轴是轨。用控件拼出来的版本
//   每一行都得自己算宽度，"拖 strip 的右边缘"这种命中更是没法表达。
//   代价是命中测试要自己写，收益是"能不能自动验"不取决于把某个点的像素
//   坐标算准没有（注入脚本走的是同一族 `runCommand`）。
// ============================================================

#include "NlaPanel.h"

#include "EditorContext.h"
#include "Localization.h"

#include "animation/NlaTimeline.h"
#include "core/Logger.h"
#include "ecs/Components.h"
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace editor {
namespace {

constexpr float kRowH = 26.0f;      // 一条轨的高度
constexpr float kLeftW = 156.0f;    // 左侧轨名列的宽度
constexpr float kRulerH = 20.0f;    // 顶部标尺高度
constexpr float kEdgePx = 6.0f;     // 拖边缘的命中宽度（约束 ②）
constexpr float kMinSpan = 0.02f;   // strip 最短时长（秒）
constexpr float kMinPxPerSec = 8.0f;
constexpr float kMaxPxPerSec = 2000.0f;

// 时间轴上的时刻 → 写进组件的值。
// **量化到 1 ms**：拖动是连续的，不量化的话每次松手都落一个 3.2173 之类的
// 值，摘要每次都不一样 —— 于是"拖出去再拖回来摘要必须复原"这条断言
// 永远做不成，而且存盘 diff 会脏得没法看。
float quantTime(float t) { return std::round(t * 1000.0f) / 1000.0f; }

// 数值框的步长（拖一下改多少秒）。用固定值而不是"按像素算"：
// 面板宽度会变，按像素算的步长会让同一次拖动在不同窗口大小下改出
// 不同的时间，写进用例就是"这条断言只在 1280 宽下成立"。
constexpr float kTimeStep = 0.01f;

std::string entityLabel(EditorContext& ctx, ecs::Entity e) {
    if (!e.valid()) return std::string("(none)");
    auto* nm = ctx.activeScene().world().get<ecs::NameComponent>(e);
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s #%u",
                  nm && !nm->name.empty() ? nm->name.c_str() : "(unnamed)",
                  static_cast<unsigned>(e.id));
    return std::string(buf);
}

} // namespace

NlaPanel::NlaPanel(EditorContext& ctx) : m_ctx(ctx) {}

void NlaPanel::open(ecs::Entity target) {
    if (target.valid()) {
        m_target = target;
        m_followSelection = false;
    } else {
        m_followSelection = true;
        m_target = m_ctx.selection();
    }
    m_open = true;
    m_uiError.clear();
    VK_LOG_INFO("NLA-OPEN target=%s follow=%d",
                entityLabel(m_ctx, m_target).c_str(), m_followSelection ? 1 : 0);
}

void NlaPanel::close() {
    if (!m_open) return;
    m_open = false;
    m_drag = Drag::None;
    VK_LOG_INFO("NLA-CLOSE");
}

void NlaPanel::resolveTarget() {
    if (m_followSelection) {
        const ecs::Entity sel = m_ctx.selection();
        if (sel.valid() && sel != m_target) {
            m_target = sel;
            m_selTrack = -1;
            m_selStrip = -1;
            VK_LOG_INFO("NLA-TARGET src=selection ent=%s",
                        entityLabel(m_ctx, m_target).c_str());
            return;
        }
        // 没有选择 → 场景里第一个挂 AnimatorComponent 的实体。
        //
        // ⚠ 只在**一个都没选中**时才这么做。选中了一个没有动画器的实体时
        //   悄悄换成别的实体，会变成"我改的是 A、动的是 B" —— 那比"面板
        //   空着"糟得多（空着至少会看到上面那行橙字）。
        // 这条路径主要是给自动化用的：脚本里没有"选中"这一步。
        if (!sel.valid() && !m_target.valid()) {
            bool found = false;
            m_ctx.activeScene().world().each<ecs::AnimatorComponent>(
                [&](ecs::Entity e, ecs::AnimatorComponent&) {
                    if (!found) {
                        m_target = e;
                        found = true;
                    }
                });
            if (found) {
                m_selTrack = -1;
                m_selStrip = -1;
                VK_LOG_INFO("NLA-TARGET src=scan ent=%s",
                            entityLabel(m_ctx, m_target).c_str());
            }
        }
        return;
    }
    // 显式目标失效（被删了）→ 回到跟随选择
    if (!m_target.valid()) {
        m_followSelection = true;
        m_target = m_ctx.selection();
        VK_LOG_INFO("NLA-TARGET src=fallback ent=%s",
                    entityLabel(m_ctx, m_target).c_str());
    }
}

ecs::AnimatorComponent* NlaPanel::animator() {
    if (!m_target.valid()) return nullptr;
    return m_ctx.activeScene().world().get<ecs::AnimatorComponent>(m_target);
}

const ecs::AnimatorComponent* NlaPanel::animatorC() const {
    if (!m_target.valid()) return nullptr;
    return const_cast<EditorContext&>(m_ctx)
        .activeScene()
        .world()
        .get<ecs::AnimatorComponent>(m_target);
}

bool NlaPanel::haveSelection() const {
    const ecs::AnimatorComponent* an = animatorC();
    if (!an) return false;
    if (m_selTrack < 0 ||
        m_selTrack >= static_cast<int>(an->nlaTracks.size()))
        return false;
    if (m_selStrip < 0) return false;
    return m_selStrip <
           static_cast<int>(an->nlaTracks[static_cast<size_t>(m_selTrack)]
                                .strips.size());
}

// ------------------------------------------------------------ 画

void NlaPanel::draw(bool& keepOpen) {
    if (!m_open) return;

    ImGui::SetNextWindowSize(ImVec2(820, 460), ImGuiCond_FirstUseEver);
    bool open = true;
    if (!ImGui::Begin(TR("NLA"), &open)) {
        ImGui::End();
        if (!open) {
            close();
            keepOpen = false;
        }
        return;
    }

    resolveTarget();
    drawHeader();
    drawToolbar();
    drawBody();
    drawInspector();
    runInjectedScript();

    ImGui::End();
    if (!open) {
        close();
        keepOpen = false;
    }
}

void NlaPanel::drawHeader() {
    ecs::AnimatorComponent* an = animator();
    ImGui::TextUnformatted(TR("Target"));
    ImGui::SameLine();
    if (m_followSelection) {
        ImGui::TextColored(ImVec4(0.62f, 0.78f, 1.0f, 1.0f), "%s",
                           entityLabel(m_ctx, m_target).c_str());
    } else {
        ImGui::Text("%s", entityLabel(m_ctx, m_target).c_str());
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(TR("Use selection"))) {
        m_followSelection = true;
        m_target = m_ctx.selection();
        m_selTrack = -1;
        m_selStrip = -1;
        VK_LOG_INFO("NLA-TARGET src=button ent=%s",
                    entityLabel(m_ctx, m_target).c_str());
    }
    if (!m_uiError.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "%s",
                           m_uiError.c_str());
    }
    if (!an) {
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.35f, 1.0f), "%s",
                           TR("This entity has no animator — NLA needs one"));
    }
}

void NlaPanel::drawToolbar() {
    ecs::AnimatorComponent* an = animator();
    if (!an) return;

    // ---- 播放控制（直接改组件字段：约束 ③）----
    if (ImGui::SmallButton(an->nlaPlaying ? TR("Pause") : TR("Play"))) {
        an->nlaPlaying = !an->nlaPlaying;
        VK_LOG_INFO("NLA-PLAY playing=%d", an->nlaPlaying ? 1 : 0);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(84);
    float t = an->nlaTime;
    if (ImGui::DragFloat("##nla_time", &t, kTimeStep, -1000.0f, 1000.0f,
                         "t=%.3f")) {
        an->nlaTime = quantTime(t);
        VK_LOG_INFO("NLA-TIME t=%.3f src=drag",
                    static_cast<double>(an->nlaTime));
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70);
    ImGui::DragFloat("##nla_speed", &an->nlaSpeed, 0.01f, -8.0f, 8.0f,
                     "x%.2f");
    ImGui::SameLine();
    ImGui::Checkbox(TR("Loop"), &an->nlaLoop);
    ImGui::SameLine();
    ImGui::Checkbox(TR("Play"), &an->nlaPlaying);

    // ---- 结构编辑 ----
    if (ImGui::SmallButton(TR("Add track"))) addTrack();
    ImGui::SameLine();
    if (ImGui::SmallButton(TR("Add strip"))) addStrip(m_selTrack);
    ImGui::SameLine();
    if (ImGui::SmallButton(TR("Duplicate"))) duplicateSelected();
    ImGui::SameLine();
    if (ImGui::SmallButton(TR("Delete"))) deleteSelected();
    ImGui::SameLine();
    ImGui::Checkbox(TR("Inspector"), &m_inspectorOpen);

    // ---- 轨开关（选中轨）----
    if (m_selTrack >= 0 &&
        m_selTrack < static_cast<int>(an->nlaTracks.size())) {
        animation::NlaTrack& T =
            an->nlaTracks[static_cast<size_t>(m_selTrack)];
        ImGui::SameLine();
        bool muted = T.muted;
        if (ImGui::Checkbox(TR("Mute track"), &muted)) {
            T.muted = muted;
            VK_LOG_INFO("NLA-TRK trk=%d muted=%d", m_selTrack, muted ? 1 : 0);
        }
        ImGui::SameLine();
        bool solo = T.solo;
        if (ImGui::Checkbox(TR("Solo"), &solo)) {
            T.solo = solo;
            VK_LOG_INFO("NLA-TRK trk=%d solo=%d", m_selTrack, solo ? 1 : 0);
        }
    }
}

float NlaPanel::duration() const {
    const ecs::AnimatorComponent* an = animatorC();
    if (!an) return 0.0f;
    return animation::nlaTimelineDuration(an->nlaTracks);
}

void NlaPanel::drawEmptyHint() {
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s",
                       TR("No strips yet — Add track, then Add strip"));
    ImGui::TextColored(
        ImVec4(0.55f, 0.55f, 0.55f, 1.0f), "%s",
        TR("A strip lives on the timeline: drag its body to move, its edges "
           "to scale. Overlap + blend in/out = crossfade."));
}

void NlaPanel::clampTimeScroll() {
    const float dur = std::max(duration(), 1.0f);
    // 允许往左留一点余量（负时刻是合法的：strip 可以放在 0 之前）
    const float lo = -1.0f;
    const float hi = dur + 1.0f;
    m_scrollSec = std::clamp(m_scrollSec, lo, hi);
}

void NlaPanel::drawBody() {
    ecs::AnimatorComponent* an = animator();
    const int nTracks = an ? static_cast<int>(an->nlaTracks.size()) : 0;

    const float bodyH = kRulerH + std::max(nTracks, 1) * kRowH + 6.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float totalW = std::max(260.0f, ImGui::GetContentRegionAvail().x);
    const float x0 = origin.x + kLeftW;
    const float tw = std::max(80.0f, totalW - kLeftW);

    ImGui::InvisibleButton("##nla_canvas", ImVec2(totalW, bodyH));
    const bool itemHovered = ImGui::IsItemHovered();
    const bool itemActive = ImGui::IsItemActive();
    const bool itemClicked = ImGui::IsItemClicked(0);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 mouse = ImGui::GetIO().MousePos;

    // 时间 → 像素（唯一的换算，画与命中都用它，不许各写一份）
    const auto tx = [&](float t) {
        return x0 + (t - m_scrollSec) * m_pxPerSec;
    };
    const auto xt = [&](float x) { return m_scrollSec + (x - x0) / m_pxPerSec; };

    // ---- 背景：左列（轨名）/ 右列（时间轴）分色 ----
    dl->AddRectFilled(ImVec2(origin.x, origin.y),
                      ImVec2(origin.x + kLeftW, origin.y + bodyH),
                      IM_COL32(26, 26, 30, 255));
    dl->AddRectFilled(ImVec2(x0, origin.y), ImVec2(x0 + tw, origin.y + bodyH),
                      IM_COL32(32, 32, 38, 255));
    // 标尺底色
    dl->AddRectFilled(ImVec2(x0, origin.y), ImVec2(x0 + tw, origin.y + kRulerH),
                      IM_COL32(44, 44, 52, 255));

    // ---- 标尺刻度 ----
    {
        const float visible = tw / m_pxPerSec;
        // 挑一个"整数秒"的步长：让刻度线上写着 1.00 / 2.00 而不是 0.37
        float step = 1.0f;
        if (visible > 12.0f) step = 2.0f;
        else if (visible > 30.0f) step = 5.0f;
        else if (visible < 3.0f) step = 0.25f;
        const float first = std::floor(m_scrollSec / step) * step;
        for (float s = first; s <= m_scrollSec + visible + step; s += step) {
            const float px = tx(s);
            if (px < x0 - 2.0f || px > x0 + tw + 2.0f) continue;
            dl->AddLine(ImVec2(px, origin.y + 4.0f),
                        ImVec2(px, origin.y + bodyH), IM_COL32(60, 60, 70, 140));
            char lab[32];
            std::snprintf(lab, sizeof(lab), "%.2f", static_cast<double>(s));
            dl->AddText(ImVec2(px + 3.0f, origin.y + 2.0f),
                        IM_COL32(178, 178, 190, 255), lab);
        }
    }

    // ---- 轨与 strip ----
    struct Hit {
        int track = -1;
        int strip = -1;
        float x0 = 0.0f, x1 = 0.0f, y0 = 0.0f, y1 = 0.0f;
    };
    std::vector<Hit> hits;

    for (int ti = 0; ti < nTracks; ++ti) {
        const float ry = origin.y + kRulerH + ti * kRowH;
        animation::NlaTrack& T = an->nlaTracks[static_cast<size_t>(ti)];

        // 左列：轨名 + 状态标记
        char name[96];
        std::snprintf(name, sizeof(name), "%d %s%s%s", ti, T.name.c_str(),
                      T.muted ? " [M]" : "", T.solo ? " [S]" : "");
        dl->AddText(ImVec2(origin.x + 6.0f, ry + 6.0f),
                    ti == m_selTrack ? IM_COL32(255, 214, 130, 255)
                                     : IM_COL32(200, 200, 210, 255),
                    name);
        if (ti == m_selTrack) {
            dl->AddRectFilled(ImVec2(origin.x, ry),
                              ImVec2(origin.x + kLeftW, ry + kRowH),
                              IM_COL32(255, 214, 130, 26));
        }
        // 行分隔线
        dl->AddLine(ImVec2(origin.x, ry + kRowH),
                    ImVec2(x0 + tw, ry + kRowH), IM_COL32(52, 52, 60, 180));

        // 右侧：strip
        for (int si = 0; si < static_cast<int>(T.strips.size()); ++si) {
            const animation::NlaStrip& s =
                T.strips[static_cast<size_t>(si)];
            const float ax = tx(s.start);
            const float bx = tx(s.end);
            if (bx < x0 || ax > x0 + tw) continue;   // 完全在视野外
            const float y0 = ry + 3.0f;
            const float y1 = ry + kRowH - 3.0f;

            const bool sel = (ti == m_selTrack && si == m_selStrip);
            const bool muted = s.muted || T.muted;
            ImU32 body = s.blend == animation::NlaBlend::Add
                             ? IM_COL32(120, 168, 96, 235)
                             : IM_COL32(96, 132, 190, 235);
            if (muted) body = IM_COL32(92, 92, 100, 235);
            if (sel) body = s.blend == animation::NlaBlend::Add
                                ? IM_COL32(152, 210, 118, 255)
                                : IM_COL32(124, 168, 236, 255);
            dl->AddRectFilled(ImVec2(ax, y0), ImVec2(bx, y1), body, 3.0f);
            if (sel) {
                dl->AddRect(ImVec2(ax, y0), ImVec2(bx, y1),
                            IM_COL32(255, 226, 150, 255), 3.0f, 0, 1.6f);
            }
            // 淡入淡出区域：画成半透明的斜纹块（"这段时间在过渡"）
            if (s.blendIn > 0.0f) {
                dl->AddRectFilled(ImVec2(ax, y0),
                                  ImVec2(tx(s.start + s.blendIn), y1),
                                  IM_COL32(255, 255, 255, 42));
            }
            if (s.blendOut > 0.0f) {
                dl->AddRectFilled(ImVec2(tx(s.end - s.blendOut), y0),
                                  ImVec2(bx, y1), IM_COL32(255, 255, 255, 42));
            }
            // 名字（裁到 strip 内）
            if (bx - ax > 24.0f) {
                const std::string label =
                    s.name + (s.weight < 1.0f
                                  ? " w=" + std::to_string(s.weight).substr(
                                                0, 4)
                                  : "");
                dl->PushClipRect(ImVec2(ax + 2.0f, y0), ImVec2(bx - 2.0f, y1),
                                 true);
                dl->AddText(ImVec2(ax + 4.0f, y0 + 3.0f),
                            IM_COL32(245, 245, 250, 255), label.c_str());
                dl->PopClipRect();
            }
            hits.push_back({ti, si, ax, bx, y0, y1});
        }
    }

    if (nTracks == 0) {
        dl->AddText(ImVec2(x0 + 10.0f, origin.y + kRulerH + 8.0f),
                    IM_COL32(150, 150, 160, 255),
                    TR("No tracks — press Add track"));
    }

    // ---- 播放头（约束 ③：读的是组件上的 `nlaTime`）----
    if (an) {
        const float px = tx(an->nlaTime);
        if (px >= x0 - 1.0f && px <= x0 + tw + 1.0f) {
            dl->AddLine(ImVec2(px, origin.y), ImVec2(px, origin.y + bodyH),
                        IM_COL32(255, 196, 96, 255), 1.6f);
            dl->AddTriangleFilled(ImVec2(px - 4.0f, origin.y),
                                  ImVec2(px + 4.0f, origin.y),
                                  ImVec2(px, origin.y + 7.0f),
                                  IM_COL32(255, 196, 96, 255));
        }
    }

    if (!an) return;

    // ---- 鼠标：按下那一刻定意图（约束 ②）----
    if (itemClicked) {
        const float mt = xt(mouse.x);
        if (mouse.y < origin.y + kRulerH) {
            m_drag = Drag::Scrub;
            m_dragGrabTime = mt;
            an->nlaTime = quantTime(std::max(0.0f, mt));
            VK_LOG_INFO("NLA-TIME t=%.3f src=scrub",
                        static_cast<double>(an->nlaTime));
        } else {
            // 后画的在上面 → 倒着找第一个命中的
            int hit = -1;
            for (int i = static_cast<int>(hits.size()) - 1; i >= 0; --i) {
                const Hit& h = hits[static_cast<size_t>(i)];
                if (mouse.x >= h.x0 && mouse.x <= h.x1 && mouse.y >= h.y0 &&
                    mouse.y <= h.y1) {
                    hit = i;
                    break;
                }
            }
            if (hit >= 0) {
                const Hit& h = hits[static_cast<size_t>(hit)];
                m_selTrack = h.track;
                m_selStrip = h.strip;
                const animation::NlaStrip& s =
                    an->nlaTracks[static_cast<size_t>(h.track)]
                        .strips[static_cast<size_t>(h.strip)];
                m_dragStart0 = s.start;
                m_dragEnd0 = s.end;
                m_dragGrabTime = mt;
                if (mouse.x - h.x0 <= kEdgePx) m_drag = Drag::EdgeL;
                else if (h.x1 - mouse.x <= kEdgePx) m_drag = Drag::EdgeR;
                else m_drag = Drag::Move;
                VK_LOG_INFO("NLA-SEL trk=%d strip=%d clip=[%s] drag=%d",
                            m_selTrack, m_selStrip, s.clip.c_str(),
                            static_cast<int>(m_drag));
            } else {
                // 点空白 = 选轨（行按 y 算）
                const int row = static_cast<int>(
                    std::floor((mouse.y - (origin.y + kRulerH)) / kRowH));
                if (row >= 0 && row < nTracks) {
                    m_selTrack = row;
                    m_selStrip = -1;
                    VK_LOG_INFO("NLA-SEL trk=%d strip=-1", row);
                } else {
                    m_selTrack = -1;
                    m_selStrip = -1;
                }
            }
        }
    }

    // ---- 拖动中 ----
    if (itemActive && m_drag != Drag::None) {
        const float mt = xt(mouse.x);
        switch (m_drag) {
        case Drag::Scrub:
            an->nlaTime = quantTime(std::max(0.0f, mt));
            break;
        case Drag::Move: {
            const float d = mt - m_dragGrabTime;
            moveStrip(m_selTrack, m_selStrip,
                      quantTime(std::max(0.0f, m_dragStart0 + d)),
                      quantTime(std::max(kMinSpan, m_dragEnd0 + d)),
                      "drag");
            break;
        }
        case Drag::EdgeL: {
            const float s =
                std::min(quantTime(mt), m_dragEnd0 - kMinSpan);
            moveStrip(m_selTrack, m_selStrip, std::max(0.0f, s), m_dragEnd0,
                      "resize-l");
            break;
        }
        case Drag::EdgeR: {
            const float e =
                std::max(quantTime(mt), m_dragStart0 + kMinSpan);
            moveStrip(m_selTrack, m_selStrip, m_dragStart0, e, "resize-r");
            break;
        }
        case Drag::None:
            break;
        }
    }

    // ---- 松手：结束拖动（**不改顺序**，约束 ①）----
    if (m_drag != Drag::None && !itemActive && ImGui::IsMouseReleased(0)) {
        if (haveSelection()) {
            const animation::NlaStrip& s =
                an->nlaTracks[static_cast<size_t>(m_selTrack)]
                    .strips[static_cast<size_t>(m_selStrip)];
            VK_LOG_INFO("NLA-DRAG-END trk=%d strip=%d start=%.3f end=%.3f",
                        m_selTrack, m_selStrip,
                        static_cast<double>(s.start),
                        static_cast<double>(s.end));
        }
        m_drag = Drag::None;
    }

    // 滚轮缩放（只在时间轴区域上、且没在拖）
    if (itemHovered && m_drag == Drag::None) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            const float anchor = xt(mouse.x);
            m_pxPerSec = std::clamp(m_pxPerSec * (wheel > 0.0f ? 1.15f : 0.87f),
                                    kMinPxPerSec, kMaxPxPerSec);
            // 缩放时把鼠标下的时刻钉住（否则内容会"跑掉"）
            m_scrollSec = anchor - (mouse.x - x0) / m_pxPerSec;
            clampTimeScroll();
        }
    }

    // 画完再记一条锚点：`vw` / `px` / `dur` 是"画布到底有没有正确定位"的
    // 唯一证据（第一次跑就撞上过"画布宽度 0、strip 全被裁掉"）。
    VK_LOG_INFO("NLA-CANVAS tracks=%d strips=%d px=%.2f scroll=%.3f w=%.1f",
                nTracks, static_cast<int>(hits.size()),
                static_cast<double>(m_pxPerSec),
                static_cast<double>(m_scrollSec),
                static_cast<double>(tw));

    if (nTracks == 0) drawEmptyHint();
}

void NlaPanel::drawInspector() {
    if (!m_inspectorOpen) return;
    ecs::AnimatorComponent* an = animator();
    if (!an) return;
    ImGui::Separator();
    if (!haveSelection()) {
        ImGui::TextColored(ImVec4(0.62f, 0.62f, 0.68f, 1.0f), "%s",
                           TR("Click a strip on the canvas to edit it"));
        return;
    }
    animation::NlaStrip& s = an->nlaTracks[static_cast<size_t>(m_selTrack)]
                                 .strips[static_cast<size_t>(m_selStrip)];

    char clipBuf[128] = {};
    std::snprintf(clipBuf, sizeof(clipBuf), "%s", s.clip.c_str());
    ImGui::SetNextItemWidth(200);
    if (ImGui::InputText(TR("Clip"), clipBuf, sizeof(clipBuf),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        s.clip = clipBuf;
        VK_LOG_INFO("NLA-STRIP trk=%d strip=%d clip=[%s]", m_selTrack,
                    m_selStrip, s.clip.c_str());
    }
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.65f, 1.0f), "%s",
                       TR("(a clip name from the target skeleton)"));

    // ---- 时间 ----
    ImGui::SetNextItemWidth(96);
    if (ImGui::DragFloat(TR("Start"), &s.start, kTimeStep, 0.0f, 600.0f,
                         "%.3f")) {
        s.start = quantTime(std::max(0.0f, s.start));
        if (s.end < s.start + kMinSpan) s.end = s.start + kMinSpan;
        VK_LOG_INFO("NLA-STRIP trk=%d strip=%d start=%.3f end=%.3f",
                    m_selTrack, m_selStrip, static_cast<double>(s.start),
                    static_cast<double>(s.end));
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(96);
    if (ImGui::DragFloat(TR("End"), &s.end, kTimeStep, 0.0f, 600.0f, "%.3f")) {
        s.end = quantTime(std::max(s.start + kMinSpan, s.end));
        VK_LOG_INFO("NLA-STRIP trk=%d strip=%d start=%.3f end=%.3f",
                    m_selTrack, m_selStrip, static_cast<double>(s.start),
                    static_cast<double>(s.end));
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(96);
    if (ImGui::DragFloat(TR("Speed"), &s.speed, 0.01f, -8.0f, 8.0f, "%.2f")) {
        VK_LOG_INFO("NLA-STRIP trk=%d strip=%d speed=%.3f", m_selTrack,
                    m_selStrip, static_cast<double>(s.speed));
    }
    ImGui::SameLine();
    ImGui::Checkbox(TR("Loop"), &s.loop);
    ImGui::SameLine();
    ImGui::Checkbox(TR("Muted"), &s.muted);

    // ---- 混合 ----
    ImGui::SetNextItemWidth(96);
    ImGui::DragFloat(TR("Weight"), &s.weight, 0.01f, 0.0f, 1.0f, "%.3f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(96);
    ImGui::DragFloat(TR("Blend in"), &s.blendIn, kTimeStep, 0.0f, 60.0f,
                     "%.3f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(96);
    ImGui::DragFloat(TR("Blend out"), &s.blendOut, kTimeStep, 0.0f, 60.0f,
                     "%.3f");

    int blend = static_cast<int>(s.blend);
    ImGui::SetNextItemWidth(120);
    if (ImGui::Combo(TR("Blend"), &blend, "Replace\0Add\0")) {
        animation::NlaBlend b = animation::NlaBlend::Replace;
        if (animation::nlaBlendFromInt(blend, &b)) {
            s.blend = b;
            VK_LOG_INFO("NLA-STRIP trk=%d strip=%d blend=%s", m_selTrack,
                        m_selStrip, animation::nlaBlendName(b));
        }
    }

    // ---- 源区间 ----
    ImGui::SetNextItemWidth(96);
    if (ImGui::DragFloat(TR("Clip start"), &s.clipStart, kTimeStep, 0.0f,
                         600.0f, "%.3f")) {
        s.clipStart = quantTime(std::max(0.0f, s.clipStart));
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(96);
    ImGui::DragFloat(TR("Clip end"), &s.clipEnd, kTimeStep, 0.0f, 600.0f,
                     "%.3f");

    // ---- 遮罩 ----
    int maskMode = static_cast<int>(s.mask.mode);
    ImGui::SetNextItemWidth(120);
    if (ImGui::Combo(TR("Mask"), &maskMode, "All\0Allow\0Deny\0")) {
        s.mask.mode = static_cast<animation::BoneMaskMode>(maskMode);
        VK_LOG_INFO("NLA-STRIP trk=%d strip=%d maskmode=%d", m_selTrack,
                    m_selStrip, maskMode);
    }
    ImGui::SameLine();
    char maskBuf[192] = {};
    {
        std::string joined;
        for (size_t i = 0; i < s.mask.roots.size(); ++i) {
            if (i) joined += ',';
            joined += s.mask.roots[i];
        }
        std::snprintf(maskBuf, sizeof(maskBuf), "%s", joined.c_str());
    }
    ImGui::SetNextItemWidth(240);
    if (ImGui::InputText(TR("Mask roots"), maskBuf, sizeof(maskBuf),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        s.mask.roots.clear();
        std::string cur;
        for (const char* p = maskBuf; *p; ++p) {
            if (*p == ',') {
                if (!cur.empty()) s.mask.roots.push_back(cur);
                cur.clear();
            } else if (*p != ' ') {
                cur += *p;
            }
        }
        if (!cur.empty()) s.mask.roots.push_back(cur);
        VK_LOG_INFO("NLA-STRIP trk=%d strip=%d maskroots=%d", m_selTrack,
                    m_selStrip, static_cast<int>(s.mask.roots.size()));
    }

    // ---- 本帧求值的账（把"为什么没生效"摆在脸上）----
    ImGui::Separator();
    const float dur = animation::nlaTimelineDuration(an->nlaTracks);
    // 这两行是**诊断读数**（"播放头停在哪 / 这条时间轴长什么样"），包 TR
    // 是因为 i18n 体检把"带格式符的界面字面量"一律算作要翻译的东西 ——
    // 它确实会被显示，只是没人会为它写一句文案。
    ImGui::Text(TR("t=%.3f  dur=%.3f  strips=%d"),
                static_cast<double>(an->nlaTime), static_cast<double>(dur),
                animation::nlaStripCount(an->nlaTracks));
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.65f, 1.0f), TR("digest=%s"),
                       digestText().c_str());
}

// ------------------------------------------------------------ 编辑动作

void NlaPanel::addTrack() {
    ecs::AnimatorComponent* an = animator();
    if (!an) {
        m_uiError = TR("No animator on the target");
        VK_LOG_WARN("NLA-REJECT addtrack reason=no-animator");
        return;
    }
    animation::NlaTrack T;
    T.name = "Track " + std::to_string(an->nlaTracks.size());
    an->nlaTracks.push_back(std::move(T));
    m_selTrack = static_cast<int>(an->nlaTracks.size()) - 1;
    m_selStrip = -1;
    VK_LOG_INFO("NLA-TRK added=1 n=%d sel=%d",
                static_cast<int>(an->nlaTracks.size()), m_selTrack);
    logState("NLA-STAT");
}

void NlaPanel::addStrip(int track) {
    ecs::AnimatorComponent* an = animator();
    if (!an) {
        m_uiError = TR("No animator on the target");
        VK_LOG_WARN("NLA-REJECT addstrip reason=no-animator");
        return;
    }
    if (an->nlaTracks.empty()) addTrack();   // 空时间轴上"Add strip"= 先建一条轨
    if (track < 0 || track >= static_cast<int>(an->nlaTracks.size()))
        track = static_cast<int>(an->nlaTracks.size()) - 1;

    animation::NlaTrack& T = an->nlaTracks[static_cast<size_t>(track)];
    animation::NlaStrip s;
    s.name = "Strip " + std::to_string(T.strips.size());
    // 默认 1 秒、摆在这一行的末尾（不是 0 —— 0 处已经有一条时两条会完全
    // 重叠，看起来像"我只加了一条"）
    float at = 0.0f;
    for (const animation::NlaStrip& o : T.strips)
        at = std::max(at, o.end);
    s.start = quantTime(at);
    s.end = quantTime(at + 1.0f);
    T.strips.push_back(std::move(s));
    m_selTrack = track;
    m_selStrip = static_cast<int>(T.strips.size()) - 1;
    VK_LOG_INFO("NLA-STRIP added=1 trk=%d strip=%d start=%.3f end=%.3f",
                m_selTrack, m_selStrip,
                static_cast<double>(T.strips.back().start),
                static_cast<double>(T.strips.back().end));
    logState("NLA-STAT");
}

void NlaPanel::deleteSelected() {
    ecs::AnimatorComponent* an = animator();
    if (!an) return;
    if (m_selTrack < 0 ||
        m_selTrack >= static_cast<int>(an->nlaTracks.size()))
        return;
    animation::NlaTrack& T = an->nlaTracks[static_cast<size_t>(m_selTrack)];
    if (m_selStrip < 0) {
        // 没选 strip = 删整条轨
        an->nlaTracks.erase(an->nlaTracks.begin() + m_selTrack);
        m_selTrack = -1;
        m_selStrip = -1;
        VK_LOG_INFO("NLA-TRK deleted=1 n=%d",
                    static_cast<int>(an->nlaTracks.size()));
        logState("NLA-STAT");
        return;
    }
    if (m_selStrip >= static_cast<int>(T.strips.size())) return;
    T.strips.erase(T.strips.begin() + m_selStrip);
    if (m_selStrip >= static_cast<int>(T.strips.size()))
        m_selStrip = static_cast<int>(T.strips.size()) - 1;
    VK_LOG_INFO("NLA-STRIP deleted=1 trk=%d n=%d", m_selTrack,
                static_cast<int>(T.strips.size()));
    logState("NLA-STAT");
}

void NlaPanel::duplicateSelected() {
    ecs::AnimatorComponent* an = animator();
    if (!an || !haveSelection()) return;
    animation::NlaTrack& T = an->nlaTracks[static_cast<size_t>(m_selTrack)];
    animation::NlaStrip copy = T.strips[static_cast<size_t>(m_selStrip)];
    const float len = copy.span();
    // 复制到**紧后面**：错开一个 span，于是两条首尾相接（不重叠）。
    // 想交叉淡入就自己往回拖 —— 不要替用户猜"他想叠多少"。
    copy.name += " copy";
    copy.start = quantTime(copy.end);
    copy.end = quantTime(copy.end + len);
    T.strips.push_back(std::move(copy));
    m_selStrip = static_cast<int>(T.strips.size()) - 1;
    VK_LOG_INFO("NLA-STRIP dup=1 trk=%d strip=%d start=%.3f end=%.3f",
                m_selTrack, m_selStrip,
                static_cast<double>(T.strips.back().start),
                static_cast<double>(T.strips.back().end));
    logState("NLA-STAT");
}

void NlaPanel::moveStrip(int ti, int si, float start, float end,
                         const char* why) {
    ecs::AnimatorComponent* an = animator();
    if (!an) return;
    if (ti < 0 || ti >= static_cast<int>(an->nlaTracks.size())) return;
    animation::NlaTrack& T = an->nlaTracks[static_cast<size_t>(ti)];
    if (si < 0 || si >= static_cast<int>(T.strips.size())) return;
    animation::NlaStrip& s = T.strips[static_cast<size_t>(si)];
    if (end < start + kMinSpan) end = start + kMinSpan;
    if (s.start == start && s.end == end) return;   // 幂等：不打日志
    s.start = start;
    s.end = end;
    // 约束 ①：**不排序**。列表顺序 = 求值顺序，排序会把作者的意图抹掉。
    VK_LOG_INFO("NLA-MOVE trk=%d strip=%d start=%.3f end=%.3f why=%s", ti, si,
                static_cast<double>(s.start), static_cast<double>(s.end), why);
}

// ------------------------------------------------------------ 摘要 / 日志

std::string NlaPanel::digestText() const {
    const ecs::AnimatorComponent* an = animatorC();
    if (!an) return "0";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx",
                  static_cast<unsigned long long>(
                      animation::nlaDigest(an->nlaTracks)));
    return std::string(buf);
}

void NlaPanel::logState(const char* anchor) const {
    const ecs::AnimatorComponent* an = animatorC();
    if (!an) {
        VK_LOG_INFO("%s tracks=0 strips=0 t=0.000", anchor);
        return;
    }
    VK_LOG_INFO("%s tracks=%d strips=%d t=%.3f dur=%.3f sel=%d/%d "
                "playing=%d digest=%s",
                anchor, static_cast<int>(an->nlaTracks.size()),
                animation::nlaStripCount(an->nlaTracks),
                static_cast<double>(an->nlaTime),
                static_cast<double>(animation::nlaTimelineDuration(
                    an->nlaTracks)),
                m_selTrack, m_selStrip, an->nlaPlaying ? 1 : 0,
                digestText().c_str());
}

// ------------------------------------------------------------ 注入脚本

void NlaPanel::runInjectedScript() {
    if (m_injected) return;
    const char* env = std::getenv("MYVK_EDITOR_NLA_SCRIPT");
    if (!env || !*env) return;
    m_injected = true;

    int applied = 0, failed = 0;
    std::string cmd;
    const std::string all(env);
    for (size_t i = 0; i <= all.size(); ++i) {
        if (i == all.size() || all[i] == ';') {
            if (!cmd.empty()) {
                int ok = 0;
                const std::string err = runCommand(cmd, &ok);
                if (ok) ++applied;
                else {
                    ++failed;
                    VK_LOG_WARN("NLA-SCRIPT-FAIL cmd=[%s] reason=%s",
                                cmd.c_str(), err.c_str());
                }
            }
            cmd.clear();
        } else {
            cmd += all[i];
        }
    }
    VK_LOG_INFO("NLA-SCRIPT applied=%d failed=%d", applied, failed);
    logState("NLA-STAT");
}

std::string NlaPanel::runCommand(const std::string& rawCmd, int* applied) {
    if (applied) *applied = 0;
    // "sleep:200" 单独处理（它不改数据，但算一次"成功执行"）
    std::vector<std::string> tok;
    std::string cur;
    for (char c : rawCmd) {
        if (c == ':') {
            tok.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    tok.push_back(cur);
    if (tok.empty() || tok[0].empty()) return "empty";

    const std::string& c = tok[0];
    const auto arg = [&](size_t i) -> std::string {
        return i < tok.size() ? tok[i] : std::string();
    };
    const auto fnum = [&](size_t i) -> float {
        return static_cast<float>(std::atof(arg(i).c_str()));
    };
    const auto inum = [&](size_t i) -> int { return std::atoi(arg(i).c_str()); };

    if (c == "sleep") {
        // 给"磁盘冲突 / 多进程"那类用例留的闸门（与其它面板同一套语义）
        if (applied) *applied = 1;
        return std::string();
    }

    ecs::AnimatorComponent* an = animator();

    if (c == "ent") {
        // 按 id 找实体：脚本要给"不是当前选中"的实体排时间轴时才用得上
        const unsigned want = static_cast<unsigned>(inum(1));
        bool found = false;
        m_ctx.activeScene().world().each<ecs::AnimatorComponent>(
            [&](ecs::Entity e, ecs::AnimatorComponent&) {
                if (!found && e.id == want) {
                    m_target = e;
                    m_followSelection = false;
                    found = true;
                }
            });
        if (!found) return "no-such-entity";
        VK_LOG_INFO("NLA-TARGET src=script ent=%s",
                    entityLabel(m_ctx, m_target).c_str());
        if (applied) *applied = 1;
        return std::string();
    }
    if (c == "sel") {
        // 跟随选择（脚本先 select 再 sel，就能把目标换过来）
        m_followSelection = true;
        m_target = m_ctx.selection();
        VK_LOG_INFO("NLA-TARGET src=script-sel ent=%s",
                    entityLabel(m_ctx, m_target).c_str());
        if (applied) *applied = 1;
        return std::string();
    }
    if (!an) return "no-animator";

    if (c == "addtrack") {
        addTrack();
        if (applied) *applied = 1;
        return std::string();
    }
    if (c == "addstrip") {
        // addstrip:<clip>:<start>:<end>[:<track>]
        const int trk = arg(4).empty() ? m_selTrack : inum(4);
        addStrip(trk);
        if (!haveSelection()) return "no-strip-selected";
        animation::NlaStrip& s =
            an->nlaTracks[static_cast<size_t>(m_selTrack)]
                .strips[static_cast<size_t>(m_selStrip)];
        if (!arg(1).empty()) s.clip = arg(1);
        s.start = quantTime(fnum(2));
        s.end = quantTime(fnum(3));
        if (s.end < s.start + kMinSpan) s.end = s.start + kMinSpan;
        VK_LOG_INFO("NLA-STRIP trk=%d strip=%d clip=[%s] start=%.3f end=%.3f",
                    m_selTrack, m_selStrip, s.clip.c_str(),
                    static_cast<double>(s.start), static_cast<double>(s.end));
        if (applied) *applied = 1;
        return std::string();
    }
    if (c == "select") {
        // select:<track>[:<strip>]
        const int trk = inum(1);
        const int si = arg(2).empty() ? -1 : inum(2);
        if (trk < 0 || trk >= static_cast<int>(an->nlaTracks.size()))
            return "no-such-track";
        m_selTrack = trk;
        m_selStrip = si;
        if (si >= 0 &&
            si >= static_cast<int>(
                      an->nlaTracks[static_cast<size_t>(trk)].strips.size()))
            return "no-such-strip";
        VK_LOG_INFO("NLA-SEL trk=%d strip=%d", m_selTrack, m_selStrip);
        if (applied) *applied = 1;
        return std::string();
    }
    if (c == "clip" || c == "speed" || c == "weight" || c == "blendin" ||
        c == "blendout" || c == "clipstart" || c == "clipend" ||
        c == "loop" || c == "mutedstrip") {
        if (!haveSelection()) return "no-strip-selected";
        animation::NlaStrip& s =
            an->nlaTracks[static_cast<size_t>(m_selTrack)]
                .strips[static_cast<size_t>(m_selStrip)];
        if (c == "clip") s.clip = arg(1);
        else if (c == "speed") s.speed = fnum(1);
        else if (c == "weight") s.weight = std::clamp(fnum(1), 0.0f, 1.0f);
        else if (c == "blendin") s.blendIn = std::max(0.0f, fnum(1));
        else if (c == "blendout") s.blendOut = std::max(0.0f, fnum(1));
        else if (c == "clipstart") s.clipStart = std::max(0.0f, fnum(1));
        else if (c == "clipend") s.clipEnd = std::max(0.0f, fnum(1));
        else if (c == "loop") s.loop = inum(1) != 0;
        else if (c == "mutedstrip") s.muted = inum(1) != 0;
        VK_LOG_INFO("NLA-STRIP trk=%d strip=%d %s ok=1", m_selTrack,
                    m_selStrip, c.c_str());
        if (applied) *applied = 1;
        return std::string();
    }
    if (c == "blend") {
        if (!haveSelection()) return "no-strip-selected";
        animation::NlaBlend b = animation::NlaBlend::Replace;
        if (!animation::nlaBlendFromInt(inum(1), &b)) return "bad-blend";
        an->nlaTracks[static_cast<size_t>(m_selTrack)]
            .strips[static_cast<size_t>(m_selStrip)]
            .blend = b;
        VK_LOG_INFO("NLA-STRIP trk=%d strip=%d blend=%s", m_selTrack,
                    m_selStrip, animation::nlaBlendName(b));
        if (applied) *applied = 1;
        return std::string();
    }
    if (c == "move" || c == "resize") {
        if (!haveSelection()) return "no-strip-selected";
        animation::NlaStrip& s =
            an->nlaTracks[static_cast<size_t>(m_selTrack)]
                .strips[static_cast<size_t>(m_selStrip)];
        const float a = quantTime(std::max(0.0f, fnum(1)));
        const float b = quantTime(std::max(a + kMinSpan, fnum(2)));
        moveStrip(m_selTrack, m_selStrip, a, b, c.c_str());
        if (applied) *applied = 1;
        return std::string();
    }
    if (c == "mask") {
        // mask:<mode 0/1/2>[:<root1,root2>]
        if (!haveSelection()) return "no-strip-selected";
        animation::NlaStrip& s =
            an->nlaTracks[static_cast<size_t>(m_selTrack)]
                .strips[static_cast<size_t>(m_selStrip)];
        const int mode = inum(1);
        if (mode < 0 || mode > 2) return "bad-mask-mode";
        s.mask.mode = static_cast<animation::BoneMaskMode>(mode);
        s.mask.roots.clear();
        std::string cur;
        const std::string roots = arg(2);
        for (size_t i = 0; i <= roots.size(); ++i) {
            if (i == roots.size() || roots[i] == ',') {
                if (!cur.empty()) s.mask.roots.push_back(cur);
                cur.clear();
            } else {
                cur += roots[i];
            }
        }
        VK_LOG_INFO("NLA-STRIP trk=%d strip=%d maskmode=%d maskroots=%d",
                    m_selTrack, m_selStrip, mode,
                    static_cast<int>(s.mask.roots.size()));
        if (applied) *applied = 1;
        return std::string();
    }
    if (c == "del") {
        deleteSelected();
        if (applied) *applied = 1;
        return std::string();
    }
    if (c == "dup" || c == "duplicate") {
        duplicateSelected();
        if (applied) *applied = 1;
        return std::string();
    }
    if (c == "trackmute" || c == "tracksolo") {
        const int trk = inum(1);
        if (trk < 0 || trk >= static_cast<int>(an->nlaTracks.size()))
            return "no-such-track";
        animation::NlaTrack& T = an->nlaTracks[static_cast<size_t>(trk)];
        if (c == "trackmute") T.muted = inum(2) != 0;
        else T.solo = inum(2) != 0;
        // 日志里打**字段名**而不是命令名：`NLA-TRK trk=0 muted=1` 与上面
        // 勾选框那条锚点同形，读日志的人不用先去查"trackmute 是哪一位"。
        VK_LOG_INFO("NLA-TRK trk=%d %s=%d", trk,
                    c == "trackmute" ? "muted" : "solo", inum(2) != 0 ? 1 : 0);
        if (applied) *applied = 1;
        return std::string();
    }
    if (c == "play" || c == "pause") {
        an->nlaPlaying = (c == "play");
        VK_LOG_INFO("NLA-PLAY playing=%d", an->nlaPlaying ? 1 : 0);
        if (applied) *applied = 1;
        return std::string();
    }
    if (c == "time") {
        an->nlaTime = quantTime(fnum(1));
        VK_LOG_INFO("NLA-TIME t=%.3f src=script",
                    static_cast<double>(an->nlaTime));
        if (applied) *applied = 1;
        return std::string();
    }
    if (c == "tspeed") {
        an->nlaSpeed = fnum(1);
        if (applied) *applied = 1;
        return std::string();
    }
    if (c == "tloop") {
        an->nlaLoop = inum(1) != 0;
        if (applied) *applied = 1;
        return std::string();
    }
    if (c == "zoom") {
        m_pxPerSec = std::clamp(fnum(1), kMinPxPerSec, kMaxPxPerSec);
        if (applied) *applied = 1;
        return std::string();
    }
    if (c == "digest") {
        VK_LOG_INFO("NLA-DIGEST digest=%s", digestText().c_str());
        if (applied) *applied = 1;
        return std::string();
    }
    if (c == "stat") {
        logState("NLA-STAT");
        if (applied) *applied = 1;
        return std::string();
    }
    if (c == "save") {
        // 把**场景**存盘（NLA 跟场景一起落盘，见 Components.h 的注释）。
        // 走的是与 File > Save 同一个 `scene::saveScene`，只是没有撤销快照。
        std::string path = arg(1);
        if (path.empty()) path = m_ctx.scenePath();
        const scene::SceneIoResult r =
            scene::saveScene(m_ctx.activeScene(), path);
        VK_LOG_INFO("NLA-SAVE path=%s ok=%d entities=%d err=%s", path.c_str(),
                    r.ok ? 1 : 0, r.entities, r.error.c_str());
        if (applied) *applied = 1;
        return std::string();
    }
    if (c == "reload") {
        // ⚠ 故意**不做**"从磁盘重读场景"：重读 = 整场景重建（撤销栈、
        //   选择集、面板状态全要重新对齐），那是 File > Open 的活，
        //   不是时间轴面板的活。存读往返的第二步由验收脚本**换一个进程
        //   重新打开**来完成 —— 跨进程比同进程更硬（同进程可能命中内存里
        //   的旧对象，看起来"读回来了"其实什么都没读）。
        return "use-a-second-process";
    }

    return "unknown-command";
}

} // namespace editor
