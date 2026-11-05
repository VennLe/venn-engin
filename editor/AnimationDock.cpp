// ============================================================
// editor/AnimationDock.cpp —— 底部动画坞的"外壳"
//
// 这里只画三样东西（内容面板自己一行没改）：
//   ① 一条标签条：六张动画/骨骼/运镜面板，同一时刻只开一张；
//   ② 一条传输条：回到起点 / 上一帧 / 播暂 / 下一帧 / 到终点 + 帧号；
//   ③ 一条时间标尺：拖播放头 = 直接改 `EditorContext::animTime()`。
// 内容是"把当前那一页钉到坞的内容矩形里"（pushDockHint + 面板自己的
// `applyDockHint`），所以在坞里看到的面板与原来的浮动窗口**逐像素相同**。
//
// ⚠ 为什么标尺的时间窗口（m_start / m_end）是坞自己的状态、不是片长：
//   片长要问 AnimatorComponent + 资产，而资产是异步加载的；坞每帧都要画，
//   不能等资产。用户可改这两个数（拖动范围手柄），标尺只是一个"时间窗口"。
// ============================================================

#include "AnimationDock.h"

#include "DebugRects.h"
#include "EditorApp.h"
#include "EditorContext.h"
#include "Localization.h"
#include "PanelDock.h"
#include "PanelRegistry.h"

#include "core/Logger.h"
#include "ui/ImGuiTheme.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace editor {

namespace {

// tab 顺序即语义：从"动作"到"镜头"。
//   0 dope       摄影表（关键帧一览）
//   1 curveed    曲线编辑器（相机 / 通用通道曲线）
//   2 bonecurv   骨骼曲线（每键绝对偏移）
//   3 nla        片段（NLA strip）
//   4 timelineed 时间轴（实体 TRS 轨道）
//   5 cine       运镜
const char* const kTabIds[AnimationDock::kTabCount] = {
    "dope", "curveed", "bonecurv", "nla", "timelineed", "cine"};

// 帧率只用于"上一帧/下一帧"的步长与帧号显示。不引工程的播放设置 ——
// 那套是给运行时用的，编辑器里"一帧"就是个便于微调的刻度。
constexpr float kDockFps = 30.0f;

int tabPanelIndex(int tab) {
    if (tab < 0 || tab >= AnimationDock::kTabCount) return -1;
    return panelIndexOf(kTabIds[tab]);
}

} // namespace

// ⚠ 这两个高度必须**按字号算**：
//   传输条里的控件是 `ImGui::Button(label, ImVec2(0, h))`，文字在按钮里
//   垂直居中；按钮再比窗口内容区高一点点，文字就会被窗口裁剪矩形切掉
//   下半截（不是"小了一号"，是齐刷刷平切）。本机字号 20px，写死 30 时
//   实测正好切在 568px（窗口底 572 减去 padding）—— 见本轮截图。
//   经验值：文字 + 上下各 7px（按钮内边距 3 + 按钮外留白 4）。
float AnimationDock::headerH() { return ImGui::GetFontSize() + 14.0f; }
// 标尺要放下"一行秒数 + 从底边往上 9px 的刻度线"，所以给得比 header 窄一点。
float AnimationDock::rulerH() { return ImGui::GetFontSize() + 8.0f; }

const char* AnimationDock::tabId(int i) {
    if (i < 0 || i >= kTabCount) return "";
    return kTabIds[i];
}

const char* AnimationDock::tabLabelKey(int i) {
    const int idx = tabPanelIndex(i);
    if (idx < 0) return "";
    return panelTitle(idx);
}

bool AnimationDock::ownsPanel(const char* id) { return tabOfPanel(id) >= 0; }

int AnimationDock::tabOfPanel(const char* id) {
    if (!id || !*id) return -1;
    for (int i = 0; i < kTabCount; ++i) {
        // 逐字符比较而不是 std::string：这个函数每帧对表里 40+ 项各调一次，
        // 建临时 string 没必要（表里的 id 都是编译期字面量）。
        const char* a = kTabIds[i];
        const char* b = id;
        while (*a && *a == *b) { ++a; ++b; }
        if (*a == '\0' && *b == '\0') return i;
    }
    return -1;
}

bool AnimationDock::anyOpen() {
    for (int i = 0; i < kTabCount; ++i) {
        const int idx = tabPanelIndex(i);
        if (idx < 0) continue;
        if (panelOpen(idx) && !panelTrayHiddenRef(idx)) return true;
    }
    return false;
}

int AnimationDock::activeTab() {
    for (int i = 0; i < kTabCount; ++i) {
        const int idx = tabPanelIndex(i);
        if (idx < 0) continue;
        if (panelOpen(idx) && !panelTrayHiddenRef(idx)) return i;
    }
    return -1;
}

void AnimationDock::selectTab(int index) {
    if (index < 0 || index >= kTabCount) return;
    // "只留一张开着"：坞的显隐完全由面板表的 open 位派生（见头文件），
    // 所以切页就是改面板表 —— 菜单勾选 / 底栏托盘 / 落盘同一步到位。
    for (int i = 0; i < kTabCount; ++i) {
        const int idx = tabPanelIndex(i);
        if (idx < 0) continue;
        setPanelOpen(idx, i == index);
    }
}

// ---------------------------------------------------------------- 画

void AnimationDock::draw(EditorApp& app, EditorContext& ctx, float x, float y,
                         float w, float& height) {
    if (w < 80.0f || height < 40.0f) return;

    // 表里可能同时有多张"开着"（菜单里又勾了一张、或 Inspector 的
    // "Edit timeline…" 之后用户又从菜单开了摄影表）。坞只显示一张 ——
    // 这里把状态收敛回去，否则菜单里两个勾、坞里只有一页，看着像坏了。
    // 只在真的多于一时动：常态下这一支一次都不进，setPanelOpen 不会被
    // 每帧调 6 次。
    {
        int openCount = 0;
        for (int i = 0; i < kTabCount; ++i) {
            const int idx = tabPanelIndex(i);
            if (idx >= 0 && panelOpen(idx) && !panelTrayHiddenRef(idx)) {
                ++openCount;
            }
        }
        if (openCount > 1) selectTab(activeTab());
    }

    // ---- 上边缘拖拽改高度 ----
    // 用手算命中而不是 InvisibleButton：这一条纹只有 7px 高，做成控件会
    // 挤掉标题行的光标（Header 里第一行是标签条，位置是绝对定位的）。
    const ImVec2 mp = ImGui::GetIO().MousePos;
    const bool overEdge = mp.x >= x && mp.x <= x + w && mp.y >= y - 3.0f &&
                          mp.y <= y + 4.0f;
    if (overEdge && ImGui::IsMouseClicked(0)) {
        m_resizing = true;
        m_resizeStartH = height;
        m_resizeStartY = mp.y;
    }
    if (m_resizing) {
        if (!ImGui::IsMouseDown(0)) {
            m_resizing = false;
        } else {
            // 往上拖 = 变高。下限 160（再矮连内容面板的一行标题都放不下），
            // 上限由 EditorApp 在算布局时按"主体还剩多少"夹住。
            height = std::max(m_resizeStartH - (mp.y - m_resizeStartY), 160.0f);
        }
    }
    if (overEdge) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);

    const ui::ThemeTokens& tk = ui::tokens();
    const float bodyY = y + headerH() + rulerH();

    // ---- ① 标题行（标签 + 传输条）----
    {
        ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(w, headerH()), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 3.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, tk.barBg);
        const ImGuiWindowFlags wf =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBringToFrontOnFocus;
        if (ImGui::Begin("##anim_dock_header", nullptr, wf)) {
            drawHeader(app, ctx);
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        logRect("ANIMDOCK", ImVec2(x, y), ImVec2(x + w, y + headerH()));
    }

    // ---- ② 时间标尺 ----
    {
        ImGui::SetNextWindowPos(ImVec2(x, y + headerH()), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(w, rulerH()), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, tk.panelBg);
        const ImGuiWindowFlags wf =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBringToFrontOnFocus;
        if (ImGui::Begin("##anim_dock_ruler", nullptr, wf)) {
            drawRuler(ctx, w);
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        logRect("ANIMDOCK-RULER", ImVec2(x, y + headerH()),
                ImVec2(x + w, y + headerH() + rulerH()));
    }

    // ---- ③ 内容：把当前那一页钉进这块矩形 ----
    const int tab = activeTab();
    if (tab >= 0) {
        const float bodyH = height - headerH() - rulerH();
        if (bodyH > 20.0f) {
            pushDockHint(x, bodyY, w, bodyH);
            app.drawAnimationDockContent(tab);
            clearDockHint();
        }
        logRect("ANIMDOCK-BODY", ImVec2(x, bodyY), ImVec2(x + w, bodyY + bodyH));
    }

    // 收起 / 展开只在变化时打一行 —— 自动化据此断言"打开动画面板 → 视口变矮"。
    {
        // 整条坞的矩形（标题 + 标尺 + 内容）。`ANIMDOCK` 只是标题行那一块，
        // 脚本要判"视口有没有和坞重叠"必须拿这一个。
        logRect("ANIMDOCK-FULL", ImVec2(x, y), ImVec2(x + w, y + height));

        static int s_lastTab = -2;
        if (s_lastTab != tab) {
            s_lastTab = tab;
            VK_LOG_INFO("ANIMDOCK open=%d tab=%s height=%.0f bodyY=%.0f",
                        tab >= 0 ? 1 : 0, tab >= 0 ? tabId(tab) : "-", height,
                        bodyY);
        }
    }
}

void AnimationDock::drawHeader(EditorApp& app, EditorContext& ctx) {
    (void)app;
    const ui::ThemeTokens& tk = ui::tokens();

    // ---- 标签（左）：同一时刻只有一个是"亮"的 ----
    const int active = activeTab();
    for (int i = 0; i < kTabCount; ++i) {
        const char* label = TR(tabLabelKey(i));
        if (i) ImGui::SameLine(0.0f, 2.0f);
        const bool on = (i == active);
        if (on) {
            ImGui::PushStyleColor(ImGuiCol_Button, tk.accentSoft);
            ImGui::PushStyleColor(ImGuiCol_Text, tk.accent);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Text, tk.textDim);
        }
        ImGui::PushID(i);
        // 点击当前页 = 收起坞（和 UE5 的 Sequencer 标签同一种闭包：
        // 再点一下收起来，不用绕去菜单）。这里不做 —— 收起入口是底栏托盘，
        // 单击只负责"切到这一页"，否则用户想切页却把它关了会很恼火。
        if (ImGui::Button(label, ImVec2(0.0f, headerH() - 8.0f))) {
            if (!on) selectTab(i);
        }
        ImGui::PopID();
        ImGui::PopStyleColor(2);
        char tag[64];
        std::snprintf(tag, sizeof(tag), "ANIMDOCK-TAB-%s", tabId(i));  // i18n-keep
        // ↑ 纯日志锚点（`verify_anim_dock.py` 按 `ANIMDOCK-TAB-<id>` 找标签矩形），
        //   屏幕上根本看不到 —— 翻成中文等于让脚本失联。
        const ImVec2 mn = ImGui::GetItemRectMin();
        const ImVec2 mx = ImGui::GetItemRectMax();
        logRect(tag, mn, mx);
        if (ImGui::IsItemHovered() && !on) {
            ImGui::SetTooltip("%s", TR(tabLabelKey(i)));
        }
    }

    // ---- 传输条（右）：固定在右端，宽度与标签条无冲突 ----
    // ⚠ 这个宽度**不能写死**：原来是 `5*30 + 92`，字号一变大（本机 20px），
    //   五个按钮 + 帧号读数就顶出窗口右沿 —— 截图里 "0.00s" 被切掉一半。
    //   现在按"按钮真实宽度 + 帧号实测文本宽"算。
    const float btnH = headerH() - 8.0f;
    const float readoutW =
        ImGui::CalcTextSize(TR("Frame %d  %.2fs")).x + 6.0f;
    const float groupW = (26.0f + 2.0f) * 5.0f + 4.0f + 8.0f + readoutW;
    float px = ImGui::GetWindowWidth() - groupW - 10.0f;
    if (px < 8.0f) px = 8.0f;
    ImGui::SameLine(px);
    ImGui::BeginGroup();

    const int frame = static_cast<int>(std::lround(ctx.animTime() * kDockFps));
    const float stepT = 1.0f / kDockFps;

    // 回起点
    if (ImGui::Button("|<", ImVec2(26.0f, btnH))) {
        ctx.setAnimTime(m_start);
        VK_LOG_INFO("ANIMDOCK seek=start t=%.3f", static_cast<double>(m_start));
    }
    ImGui::SameLine(0.0f, 2.0f);
    // 上一帧
    if (ImGui::Button("<", ImVec2(26.0f, btnH))) {
        ctx.setAnimTime(std::max(ctx.animTime() - stepT, m_start));
    }
    ImGui::SameLine(0.0f, 2.0f);
    // 播 / 暂
    {
        const bool playing = ctx.animPlaying();
        ImGui::PushStyleColor(ImGuiCol_Button,
                              playing ? tk.warn : tk.info);
        if (ImGui::Button(playing ? "||" : ">", ImVec2(30.0f, btnH))) {
            if (playing) {
                ctx.animPause();
            } else {
                ctx.animPlay();
            }
            VK_LOG_INFO("ANIMDOCK play=%d", ctx.animPlaying() ? 1 : 0);
        }
        ImGui::PopStyleColor();
    }
    ImGui::SameLine(0.0f, 2.0f);
    // 下一帧
    if (ImGui::Button(">", ImVec2(26.0f, btnH))) {
        ctx.setAnimTime(std::min(ctx.animTime() + stepT, m_end));
    }
    ImGui::SameLine(0.0f, 2.0f);
    // 到终点
    if (ImGui::Button(">|", ImVec2(26.0f, btnH))) {
        ctx.setAnimTime(m_end);
        VK_LOG_INFO("ANIMDOCK seek=end t=%.3f", static_cast<double>(m_end));
    }

    ImGui::SameLine(0.0f, 8.0f);
    // 帧号 + 秒。**"Frame" 这个词必须走 TR()**：早先图省事写成 `"F %d  %.2fs"`，
    // 结果是中文界面下那一格永远挂着英文 F —— 而它右侧的 "s" 是单位缩写，
    // 和 `CharacterWindow` 的 `%.3fs` 一样按惯例不翻（写了词条反而更怪）。
    // ⚠ D 组（裸格式串）判据会数"剥掉 %d/%s 之后还剩几个字母"：`"Frame %d  %.2fs"`
    //   不包 TR 就是 5 个字母，直接红；包上 TR 之后查的是词典，见 ZhTable 同串词条。
    char buf[64];
    std::snprintf(buf, sizeof(buf), TR("Frame %d  %.2fs"), frame,
                  static_cast<double>(ctx.animTime()));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(buf);
    ImGui::EndGroup();
    logRect("ANIMDOCK-TRANSPORT", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
}

void AnimationDock::drawRuler(EditorContext& ctx, float w) {
    if (w < 40.0f) return;
    const ui::ThemeTokens& tk = ui::tokens();
    const float h = rulerH();

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##anim_ruler", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    // 拖动播放头：按下即跳、按住即跟。这是"对着场景调"最需要的那个动作 ——
    // 时间一变，下一帧的脚本 / 动画就按新时间求值。
    if (hovered && ImGui::IsMouseDown(0)) {
        const float u = (ImGui::GetIO().MousePos.x - origin.x) / w;
        const float t = m_start + std::clamp(u, 0.0f, 1.0f) * (m_end - m_start);
        ctx.setAnimTime(t);
        m_scrubbing = true;
    }
    if (!ImGui::IsMouseDown(0)) m_scrubbing = false;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 bg = ImGui::GetColorU32(tk.trackBg);
    dl->AddRectFilled(origin, ImVec2(origin.x + w, origin.y + h), bg);

    // 刻度：每 0.5s 一根短线，每 1s 一根长线 + 秒数
    const float span = std::max(m_end - m_start, 1e-3f);
    const float secW = w / span;
    const ImU32 tick = ImGui::GetColorU32(tk.borderSoft);
    const ImU32 tickMajor = ImGui::GetColorU32(tk.textFaint);
    const int n = static_cast<int>(span / 0.5f) + 1;
    for (int i = 0; i < n; ++i) {
        const float t = m_start + i * 0.5f;
        const float tx = origin.x + (t - m_start) * secW;
        if (tx > origin.x + w - 1.0f) break;
        const bool major = (i % 2) == 0;
        // 刻度从底边往上长，**不能压到秒数**：major 8px / minor 5px，
        // 正好让开上面那一行（`rulerH() = 字号 + 8`）。
        dl->AddLine(ImVec2(tx, origin.y + h - (major ? 8.0f : 5.0f)),
                    ImVec2(tx, origin.y + h - 1.0f),
                    major ? tickMajor : tick, 1.0f);
        if (major) {
            char lb[24];
            std::snprintf(lb, sizeof(lb), "%.0fs", static_cast<double>(t));
            dl->AddText(ImVec2(tx + 3.0f, origin.y + 1.0f),
                        ImGui::GetColorU32(tk.textFaint), lb);
        }
    }

    // 播放头
    const float u = std::clamp((ctx.animTime() - m_start) / span, 0.0f, 1.0f);
    const float hx = origin.x + u * w;
    dl->AddLine(ImVec2(hx, origin.y), ImVec2(hx, origin.y + h),
                ImGui::GetColorU32(tk.accent), 2.0f);
    logRect("ANIMDOCK-PLAYHEAD", ImVec2(hx - 1.0f, origin.y),
            ImVec2(hx + 1.0f, origin.y + h));

    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
}

} // namespace editor
