#include "PanelRegistry.h"

#include "core/Logger.h"

#include <cstdlib>
#include <cstring>

namespace editor {

namespace {

// 显隐状态。为什么用固定数组而不是 vector：
//   · PanelVisibility 持有它的元素的**引用**（bool&），地址必须一辈子不变
//     —— vector 扩容会把引用全部悬垂。
//   · 上限 24 个面板对编辑器来说绰绰有余，超了有 assert 兜底。
bool g_open[kMaxPanels] = {};

// 状态是否已经按 defaultOpen 初始化过（首次访问时做一次）
bool g_initialized = false;

// 找不到 id 时返回的哑元 —— 让 panelOpenRef 永远能给出有效引用。
// 写它等于"写进了黑洞"，不会影响任何面板。
bool g_dummy = false;

void ensureInit() {
    if (g_initialized) return;
    g_initialized = true;
    resetPanelStateToDefaults();
}

} // namespace

// 表里的面板数夹到状态数组容量（表加了条目但忘了抬 kMaxPanels 时兜底）
static int panelCountImpl() {
    const int n = panelCount();
    return n < kMaxPanels ? n : kMaxPanels;
}

bool* panelOpenSlot(int index) {
    if (index < 0 || index >= panelCountImpl()) return nullptr;
    return &g_open[index];
}

bool* panelOpenSlotById(const char* id) {
    return panelOpenSlot(panelIndexOf(id));
}

bool& panelOpenRef(const char* id) {
    ensureInit();
    if (bool* p = panelOpenSlotById(id)) return *p;
    return g_dummy;
}

int panelIndexOf(const char* id) {
    if (!id) return -1;
    const PanelDesc* t = panelTable();
    const int n = panelCount();
    for (int i = 0; i < n; ++i) {
        if (t[i].id && std::strcmp(t[i].id, id) == 0) return i;
    }
    return -1;
}

bool panelOpen(int index) {
    ensureInit();
    if (bool* p = panelOpenSlot(index)) return *p;
    return false;
}

void setPanelOpen(int index, bool open) {
    ensureInit();
    if (bool* p = panelOpenSlot(index)) *p = open;
}

void togglePanel(int index) {
    ensureInit();
    if (bool* p = panelOpenSlot(index)) *p = !*p;
}

const char* panelId(int index) {
    const PanelDesc* t = panelTable();
    if (index < 0 || index >= panelCount()) return "";
    return t[index].id ? t[index].id : "";
}

const char* panelTitle(int index) {
    const PanelDesc* t = panelTable();
    if (index < 0 || index >= panelCount()) return "";
    return t[index].title ? t[index].title : "";
}

int panelIndexByShortcut(const char* chord) {
    if (!chord || !chord[0]) return -1;
    const PanelDesc* t = panelTable();
    const int n = panelCount();
    for (int i = 0; i < n; ++i) {
        if (t[i].shortcut && t[i].shortcut[0] &&
            std::strcmp(t[i].shortcut, chord) == 0) {
            return i;
        }
    }
    return -1;
}

void snapshotPanelState(std::vector<std::pair<std::string, bool>>& out) {
    ensureInit();
    const PanelDesc* t = panelTable();
    const int n = panelCountImpl();
    out.clear();
    out.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        out.emplace_back(t[i].id ? t[i].id : "", g_open[i]);
    }
}

void applyPanelState(const std::vector<std::pair<std::string, bool>>& in) {
    ensureInit();
    for (const auto& kv : in) {
        const int idx = panelIndexOf(kv.first.c_str());
        // 未知 id 静默忽略：配置文件比二进制新（或面板被删掉）时不该整体失败
        if (idx >= 0 && idx < kMaxPanels) g_open[idx] = kv.second;
    }
}

void resetPanelStateToDefaults() {
    const PanelDesc* t = panelTable();
    const int n = panelCountImpl();
    for (int i = 0; i < n; ++i) g_open[i] = t[i].defaultOpen;
    g_initialized = true;
}

void logPanelStates() {
    if (!std::getenv("MYVK_LOG_RECTS")) return;
    const PanelDesc* t = panelTable();
    const int n = panelCountImpl();
    for (int i = 0; i < n; ++i) {
        VK_LOG_INFO("PANEL-STATE %s=%s", t[i].id ? t[i].id : "?",
                    g_open[i] ? "open" : "closed");
    }
}

} // namespace editor
