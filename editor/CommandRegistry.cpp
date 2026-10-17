#include "CommandRegistry.h"

#include "Localization.h"
#include "core/Logger.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace editor {

// ================================================================
// 模糊匹配
// ================================================================

namespace {

char lowerCh(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

// "词首" = 字符串开头，或前一个字符不是字母数字（"toggle_ssr" 的 s、
// "Toggle SSR" 的 S 都算词首）。
bool isWordStart(const char* hay, int i) {
    if (i == 0) return true;
    return !std::isalnum(static_cast<unsigned char>(hay[i - 1]));
}

}  // namespace

int fuzzyScore(const char* needle, const char* hay) {
    if (!needle || !hay || !needle[0]) return 0;

    int score = 0;
    int hi = 0;        // 在 hay 里的当前位置（单调前进 → 保证是子序列）
    int lastHit = -1;  // 上一次命中的下标（判"连续"）

    for (int ni = 0; needle[ni]; ++ni) {
        const char nc = lowerCh(needle[ni]);
        if (nc == ' ' || nc == '\t') continue;  // 查询里的空白忽略

        bool found = false;
        for (; hay[hi]; ++hi) {
            if (lowerCh(hay[hi]) != nc) continue;
            if (isWordStart(hay, hi)) score += 12;      // 词首最爱
            else if (lastHit == hi - 1) score += 8;     // 连续命中
            else score += 2;                            // 中间随便捞到
            lastHit = hi;
            ++hi;
            found = true;
            break;
        }
        if (!found) return 0;  // 不是子序列 → 不命中
    }

    // 同分时短文本略优先（搜 "ssr" 时 "SSR" 该排在
    // "Screen Space Reflections" 前面）
    const int len = static_cast<int>(std::strlen(hay));
    score += std::max(0, 24 - len / 2);
    return score;
}

float fuzzySimilarity(const char* a, const char* b) {
    if (!a || !b || !a[0] || !b[0]) return 0.0f;
    const size_t n = std::strlen(a);
    const size_t m = std::strlen(b);
    // 最长公共子序列长度（O(n·m)，命令名只有几十字符，够用）
    std::vector<int> prev(m + 1, 0), cur(m + 1, 0);
    for (size_t i = 1; i <= n; ++i) {
        for (size_t j = 1; j <= m; ++j) {
            if (lowerCh(a[i - 1]) == lowerCh(b[j - 1]))
                cur[j] = prev[j - 1] + 1;
            else
                cur[j] = std::max(prev[j], cur[j - 1]);
        }
        prev.swap(cur);
        std::fill(cur.begin(), cur.end(), 0);
    }
    const size_t lcs = static_cast<size_t>(prev[m]);
    return static_cast<float>(lcs) /
           static_cast<float>(std::max(n, m));
}

// ================================================================
// 注册表
// ================================================================

CommandRegistry& CommandRegistry::get() {
    static CommandRegistry inst;
    return inst;
}

void CommandRegistry::add(CommandDesc c) {
    if (c.id.empty()) {
        core::logMessage(core::LogLevel::Warn, "CMD-ADD rejected: empty id");
        return;
    }
    for (auto& existing : m_cmds) {
        if (existing.id == c.id) {
            existing = std::move(c);  // 覆盖（允许宿主热替换实现）
            return;
        }
    }
    m_cmds.push_back(std::move(c));
}

void CommandRegistry::clear() {
    m_cmds.clear();
    m_recent.clear();
}

const CommandDesc* CommandRegistry::find(const char* id) const {
    if (!id || !id[0]) return nullptr;
    for (const auto& c : m_cmds)
        if (c.id == id) return &c;
    return nullptr;
}

bool CommandRegistry::available(const CommandDesc& c) {
    if (!c.run) return false;             // 未实现 → 不可用
    if (c.when && !c.when()) return false;
    return true;
}

bool CommandRegistry::run(const char* id, const char* via) {
    const CommandDesc* c = find(id);
    if (!c) {
        core::logMessage(core::LogLevel::Warn, "CMD-RUN missing id=%s",
                         id ? id : "(null)");
        return false;
    }
    if (!c->run) {
        core::logMessage(core::LogLevel::Warn, "CMD-RUN unimplemented id=%s",
                         c->id.c_str());
        return false;
    }
    if (c->when && !c->when()) {
        core::logMessage(core::LogLevel::Warn, "CMD-RUN unavailable id=%s",
                         c->id.c_str());
        return false;
    }
    c->run();
    noteExecuted(c->id);
    core::logMessage(core::LogLevel::Info, "CMD-RUN id=%s label=\"%s\" via=%s",
                     c->id.c_str(), c->label.c_str(),
                     via ? via : "unknown");
    return true;
}

void CommandRegistry::noteExecuted(const std::string& id) {
    // 去重后插到最前（MRU，最新在前）
    m_recent.erase(std::remove(m_recent.begin(), m_recent.end(), id),
                   m_recent.end());
    m_recent.insert(m_recent.begin(), id);
    if (m_recent.size() > kMaxRecent) m_recent.resize(kMaxRecent);
}

const CommandDesc* CommandRegistry::findByShortcut(const char* chord) const {
    if (!chord || !chord[0]) return nullptr;
    for (const auto& c : m_cmds) {
        if (c.shortcut.size() != std::strlen(chord)) continue;
        bool same = true;
        for (size_t i = 0; chord[i]; ++i) {
            if (lowerCh(c.shortcut[i]) != lowerCh(chord[i])) {
                same = false;
                break;
            }
        }
        if (same) return &c;
    }
    return nullptr;
}

std::vector<const CommandDesc*> CommandRegistry::search(
    const std::string& query, int maxOut) const {
    std::vector<const CommandDesc*> out;

    if (query.empty()) {
        // 空查询：MRU 置顶，其余按 category + label 排（稳定、可预期）
        for (const auto& id : m_recent)
            if (const CommandDesc* c = find(id.c_str())) out.push_back(c);

        std::vector<const CommandDesc*> rest;
        rest.reserve(m_cmds.size());
        for (const auto& c : m_cmds) {
            const bool inMru =
                std::find(m_recent.begin(), m_recent.end(), c.id) !=
                m_recent.end();
            if (!inMru) rest.push_back(&c);
        }
        std::stable_sort(rest.begin(), rest.end(),
                         [](const CommandDesc* a, const CommandDesc* b) {
                             if (a->category != b->category)
                                 return a->category < b->category;
                             return a->label < b->label;
                         });
        out.insert(out.end(), rest.begin(), rest.end());
    } else {
        std::vector<std::pair<int, const CommandDesc*>> hits;
        for (const auto& c : m_cmds) {
            const int s = std::max(
                fuzzyScore(query.c_str(), c.label.c_str()),
                std::max(fuzzyScore(query.c_str(), c.id.c_str()),
                         fuzzyScore(query.c_str(), c.keywords.c_str())));
            if (s > 0) hits.emplace_back(s, &c);
        }
        std::stable_sort(hits.begin(), hits.end(),
                         [](const std::pair<int, const CommandDesc*>& a,
                            const std::pair<int, const CommandDesc*>& b) {
                             if (a.first != b.first) return a.first > b.first;
                             return a.second->label < b.second->label;
                         });
        for (const auto& h : hits) out.push_back(h.second);
    }

    if (maxOut > 0 && static_cast<int>(out.size()) > maxOut)
        out.resize(static_cast<size_t>(maxOut));
    return out;
}

std::vector<const CommandDesc*> CommandRegistry::closest(
    const std::string& query, int maxOut) const {
    std::vector<std::pair<float, const CommandDesc*>> scored;
    for (const auto& c : m_cmds) {
        const float s = std::max(fuzzySimilarity(query.c_str(), c.label.c_str()),
                                 fuzzySimilarity(query.c_str(), c.id.c_str()));
        scored.emplace_back(s, &c);
    }
    std::stable_sort(scored.begin(), scored.end(),
                     [](const std::pair<float, const CommandDesc*>& a,
                        const std::pair<float, const CommandDesc*>& b) {
                         return a.first > b.first;
                     });
    std::vector<const CommandDesc*> out;
    for (const auto& s : scored) {
        if (maxOut > 0 && static_cast<int>(out.size()) >= maxOut) break;
        out.push_back(s.second);
    }
    return out;
}

void registerCommand(CommandDesc c) { CommandRegistry::get().add(std::move(c)); }

// ================================================================
// 命令面板 UI
// ================================================================

namespace {

// 面板自己的 UI 状态（单例 —— 全局只有一个命令面板）
struct PaletteState {
    char query[128] = {};
    int sel = 0;
    bool focusReq = false;
    bool wasOpen = false;
    bool loggedOpen = false;
    // 自动化：签名去重，只在"查询/命中集"变化时打日志
    unsigned sig = 0u;
    bool envExecDone = false;
    double openTime = 0.0;
};
PaletteState g_p;

// FNV-1a，用来给"查询 + 命中 id 列表"算一个签名（只在变化时打日志）
unsigned fnv(const std::string& s) {
    unsigned h = 2166136261u;
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

// 面板里一行命令：左边 label，右边（灰）快捷键 + 分类
void drawRow(const CommandDesc& c, bool selected) {
    const bool ok = CommandRegistry::available(c);
    ImGui::PushID(c.id.c_str());
    if (!ok) ImGui::BeginDisabled();
    if (ImGui::Selectable("##row", selected, ImGuiSelectableFlags_None,
                          ImVec2(0, 0))) {
        if (ok) {
            CommandRegistry::get().run(c.id.c_str(), "palette");
            g_p.sel = -1;  // 让调用方知道"该关了"
        }
    }
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextUnformatted(c.label.c_str());

    const float rightW = ImGui::CalcTextSize(c.shortcut.c_str()).x +
                         ImGui::CalcTextSize(c.category.c_str()).x + 24.0f;
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > rightW) {
        ImGui::SameLine(ImGui::GetCursorPosX() + avail - rightW);
        if (!c.shortcut.empty())
            ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.70f, 1.0f), TR("%s"),
                               c.shortcut.c_str());
        else
            ImGui::TextUnformatted(TR("  "));
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.45f, 0.50f, 0.60f, 1.0f), TR("%s"),
                           c.category.c_str());
    }
    if (!ok) ImGui::EndDisabled();
    ImGui::PopID();
}

}  // namespace

bool drawCommandPalette(CommandRegistry& reg, bool* open) {
    // ---- 自动化：MYVK_EDITOR_CMDPAL=<query> 存在即开面板并预填 ----
    static bool envApplied = false;
    if (const char* q = std::getenv("MYVK_EDITOR_CMDPAL")) {
        if (!envApplied) {
            envApplied = true;
            std::snprintf(g_p.query, sizeof(g_p.query), "%s", q);
            if (open) *open = true;
            g_p.focusReq = true;
        }
    }
    if (!open || !*open) {
        g_p.wasOpen = false;
        g_p.loggedOpen = false;
        g_p.envExecDone = false;
        g_p.sig = 0u;
        return false;
    }

    const ImGuiIO& io = ImGui::GetIO();
    if (!g_p.wasOpen) {
        g_p.wasOpen = true;
        g_p.focusReq = true;
        g_p.sel = 0;
        g_p.openTime = ImGui::GetTime();
    }

    const ImVec2 disp = io.DisplaySize;
    // 固定位置 + 固定尺寸（居中偏上）—— 不用 AlwaysAutoResize：面板的大
    // 小随候选数变化会"跳"，而且固定尺寸对自动化截图更可预期。
    ImGui::SetNextWindowPos(ImVec2(disp.x * 0.5f, disp.y * 0.28f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(560.0f, 400.0f), ImGuiCond_Always);
    if (g_p.focusReq) ImGui::SetNextWindowFocus();

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse;

    bool executed = false;
    if (ImGui::Begin(trId("Command Palette", "cmdpal"), nullptr, flags)) {
        // 窗口真的建起来了才算"打开了"（日志放这里 —— 自动化据此确认
        // 面板确实进了绘制路径，而不是只置了个 bool）
        if (!g_p.loggedOpen) {
            g_p.loggedOpen = true;
            core::logMessage(core::LogLevel::Info, "CMD-PALETTE open");
        }
        ImGui::TextColored(ImVec4(0.55f, 0.60f, 0.70f, 1.0f),
                           TR("Command Palette    (type to search, Enter to run, "
                           "Esc to close)"));
        if (g_p.focusReq) {
            ImGui::SetKeyboardFocusHere();
            g_p.focusReq = false;
        }
        ImGui::SetNextItemWidth(-1.0f);
        const bool entered = ImGui::InputText(
            "##query", g_p.query, sizeof(g_p.query),
            ImGuiInputTextFlags_EnterReturnsTrue);

        // 候选只算一次（↑↓ 与列表共用同一份，避免两次 search 结果不一致）
        std::vector<const CommandDesc*> hits = reg.search(g_p.query, 200);
        const int nHits = static_cast<int>(hits.size());
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && nHits > 0)
            g_p.sel = (g_p.sel + 1) % nHits;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && nHits > 0)
            g_p.sel = (g_p.sel - 1 + nHits) % nHits;
        if (g_p.sel >= nHits) g_p.sel = 0;

        ImGui::Separator();
        if (hits.empty()) {
            const auto near = reg.closest(g_p.query, 5);
            ImGui::TextColored(ImVec4(0.85f, 0.70f, 0.35f, 1.0f),
                               TR("no command matches '%s'%s"), g_p.query,
                               near.empty() ? "" : "   — closest:");
            for (const CommandDesc* c : near) drawRow(*c, false);
        } else {
            const int kMaxShown = 12;
            const int shown = std::min<int>(static_cast<int>(hits.size()),
                                            kMaxShown);
            ImGui::BeginChild("##hits", ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 13.0f), false,
                              ImGuiWindowFlags_NoScrollbar);
            for (int i = 0; i < shown; ++i) drawRow(*hits[i], i == g_p.sel);
            ImGui::EndChild();
            if (static_cast<int>(hits.size()) > kMaxShown)
                ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.60f, 1.0f),
                                   TR("... %d more"),
                                   static_cast<int>(hits.size()) - kMaxShown);
        }

        // row 里点了一下 → 会设 sel = -1 表示"执行过了"
        if (g_p.sel == -1) {
            executed = true;
            *open = false;
            g_p.sel = 0;
        } else if (entered && !hits.empty()) {
            const CommandDesc* c = hits[static_cast<size_t>(g_p.sel)];
            executed = reg.run(c->id.c_str(), "palette");
            *open = false;
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) *open = false;

        // ---- 自动化日志（查询/命中集变化时一条）----
        const unsigned newSig = fnv(std::string(g_p.query) + "|" +
                                    std::to_string(hits.size()));
        if (newSig != g_p.sig) {
            g_p.sig = newSig;
            core::logMessage(core::LogLevel::Info, "CMD-PALETTE query=\"%s\" hits=%d",
                             g_p.query, static_cast<int>(hits.size()));
            const int kLog = std::min<int>(static_cast<int>(hits.size()), 8);
            for (int i = 0; i < kLog; ++i) {
                core::logMessage(core::LogLevel::Info,
                                 "CMD-PALETTE-HIT rank=%d id=%s label=\"%s\" "
                                 "category=%s shortcut=\"%s\"",
                                 i, hits[static_cast<size_t>(i)]->id.c_str(),
                                 hits[static_cast<size_t>(i)]->label.c_str(),
                                 hits[static_cast<size_t>(i)]->category.c_str(),
                                 hits[static_cast<size_t>(i)]->shortcut.c_str());
            }
            if (hits.empty()) {
                const auto near = reg.closest(g_p.query, 3);
                core::logMessage(core::LogLevel::Info,
                                 "CMD-PALETTE nomatch query=\"%s\" best=\"%s\"",
                                 g_p.query,
                                 near.empty() ? "" : near[0]->label.c_str());
            }
        }

        // ---- 自动化：MYVK_EDITOR_CMDPAL_EXEC=1 → 开面板后执行榜首 ----
        if (!g_p.envExecDone && std::getenv("MYVK_EDITOR_CMDPAL_EXEC") &&
            ImGui::GetTime() - g_p.openTime > 0.4) {
            g_p.envExecDone = true;
            std::vector<const CommandDesc*> top = reg.search(g_p.query, 1);
            if (!top.empty()) {
                executed = reg.run(top[0]->id.c_str(), "palette");
                *open = false;
            } else {
                core::logMessage(core::LogLevel::Warn,
                                 "CMD-PALETTE exec: no hit for \"%s\"",
                                 g_p.query);
            }
        }
    }
    ImGui::End();
    return executed;
}

}  // namespace editor
