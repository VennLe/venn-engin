// ============================================================
// editor/DialogueEditorWindow.cpp
//
// 结构：工具栏（路径 / Reload / Save / New）→ 左栏节点列表 → 右栏逐句编辑
//      → 底部校验栏。所有**改动数据**的入口都是 `act*` 那一族，界面按钮
//      与注入脚本共用它们（见 .h 的"动作"一节）。
//
// ⚠ 日志锚点前缀 `DLGED-`（Dialogue EDitor），与运行时的 `DLG-*` 分开：
//   两套东西同时出现在一份日志里时，"搜 DLG- 全红"不该把编辑器的正常
//   输出也算进去。
// ============================================================

#include "DialogueEditorWindow.h"

#include "DebugRects.h"
#include "EditorContext.h"
#include "Localization.h"
#include "PanelDock.h"

#include "assets/AssetPath.h"
#include "core/Logger.h"
#include "ecs/Components.h"
#include "scene/Scene.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace editor {

namespace {

// 文本缓冲上限。8192 字节 ≈ 2700 个汉字，够一段过场用；
// 超过会被 `InputTextMultiline` 静默截断，所以在界面上单独提示一句。
constexpr std::size_t kTextCap = 8192;

// 一个 `%s` 的小格式化器（`std::string` + `vsnprintf`）。
// ImGui 的 Text 可以直接吃格式串，但"先算成字符串再当一行建议存起来"这条
// 路上没有 ImGui 帮忙，所以自带一个。
std::string fmt(const char* f, const char* a) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), f, a);
    return buf;
}

const ImVec4 kOkCol(0.45f, 0.85f, 0.52f, 1.0f);
const ImVec4 kBadCol(1.00f, 0.45f, 0.35f, 1.0f);
const ImVec4 kWarnCol(1.00f, 0.75f, 0.30f, 1.0f);

// 把两份路径都化成"绝对 + 正斜杠"再比。
// ⚠ 不能直接比字符串：作者在工具栏里可能写 `.\dialogues\x.dlg.json`、
//   可能从 Content 拖过来带 `assets/` 前缀，而组件里存的是
//   `dialogues/x.dlg.json` —— 三种写法指的是同一份文件。
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

// 一行台词在列表里长什么样：码点截断（按字节切会把汉字劈成半个 → 一串 ?）
std::string previewOf(const dialogue::DlgNode& n, int maxChars) {
    std::string t = n.text;
    for (std::size_t p = 0; (p = t.find('\n', p)) != std::string::npos; ++p)
        t[p] = ' ';
    if (t.empty() && !n.choices.empty()) t = "<choices>";
    if (t.empty()) t = "<empty>";
    if (dialogue::utf8Count(t) > maxChars) {
        t = dialogue::utf8Prefix(t, maxChars - 1) + "…";
    }
    return t;
}

} // namespace

DialogueEditorWindow::DialogueEditorWindow(EditorContext& ctx) : m_ctx(ctx) {
    m_textBuf.assign(kTextCap, '\0');
}

// ============================================================
// 打开 / 重读
// ============================================================

bool DialogueEditorWindow::open(const std::string& relPath) {
    m_open = true;
    m_path = relPath;
    m_sel = 0;
    m_dirty = false;
    m_uiError.clear();
    m_error.clear();
    m_bufForId.clear();
    m_injected = false;
    m_loaded = true;
    std::snprintf(m_pathBuf, sizeof(m_pathBuf), "%s", m_path.c_str());

    if (relPath.empty()) {
        // 空手开窗（从 View 菜单点开）：只剩"New script"与上面那个路径框。
        m_asset = dialogue::DialogueAsset{};
        m_readOnly = false;
        VK_LOG_INFO("DLGED-OPEN path=(none) ok=0 lines=0 start= readOnly=0");
        revalidate();
        return false;
    }

    dialogue::DialogueAsset a;
    const bool ok = dialogue::loadDialogue(relPath, a);
    m_asset = std::move(a);
    // ③ 读盘失败 → 只读。此时 `m_asset.nodes` 可能只是前几行的残片，
    //    允许存盘 = 把 300 行的剧本截成 119 行。
    m_readOnly = !ok;
    if (!ok) m_error = m_asset.error;

    int withChoices = 0;
    for (const auto& n : m_asset.nodes)
        if (!n.choices.empty()) ++withChoices;
    VK_LOG_INFO("DLGED-OPEN path=%s ok=%d lines=%d start=%s choices=%d readOnly=%d",
                relPath.c_str(), ok ? 1 : 0, nodeCount(),
                m_asset.startId.c_str(), withChoices, m_readOnly ? 1 : 0);
    revalidate();
    return ok;
}

void DialogueEditorWindow::close() {
    if (m_open && m_dirty) {
        // 不弹模态框（模态会挡住"我就想切个面板"的正常操作），只记账。
        VK_LOG_WARN("DLGED-CLOSE-DIRTY path=%s（有未存的改动）", m_path.c_str());
    }
    m_open = false;
}

void DialogueEditorWindow::reload() {
    if (m_path.empty()) {
        m_asset = dialogue::DialogueAsset{};
        m_readOnly = false;
        revalidate();
        return;
    }
    dialogue::DialogueAsset a;
    const bool ok = dialogue::loadDialogue(m_path, a);
    m_asset = std::move(a);
    m_readOnly = !ok;
    m_dirty = false;
    m_bufForId.clear();
    m_sel = std::max(0, std::min(m_sel, nodeCount() - 1));
    m_error = ok ? std::string() : m_asset.error;
    m_uiError.clear();
    VK_LOG_INFO("DLGED-RELOAD path=%s ok=%d lines=%d readOnly=%d", m_path.c_str(),
                ok ? 1 : 0, nodeCount(), m_readOnly ? 1 : 0);
    revalidate();
}

// ============================================================
// 校验：序列化 → 丢回运行时那个解析器
// ============================================================

void DialogueEditorWindow::revalidate() {
    dialogue::DialogueAsset check;
    const std::string text = dialogue::toJson(m_asset);
    const bool parsed = dialogue::parseDialogue(
        text, m_path.empty() ? std::string("untitled.dlg.json") : m_path, check);

    m_checkValid = parsed && check.valid;
    m_checkError = check.error;
    m_checkWarnings = check.warnings;

    // 只在**结论变化**时打日志：每次改动都打的话，敲一行台词就是几十条
    // DLGED-CHECK，真正的那条淹没在里面。
    const std::string key = std::string(m_checkValid ? "1" : "0") + "|" +
                            m_checkError + "|" +
                            std::to_string(m_checkWarnings.size()) + "|" +
                            std::to_string(nodeCount());
    if (key != m_checkLogKey) {
        m_checkLogKey = key;
        VK_LOG_INFO("DLGED-CHECK ok=%d lines=%d start=%s warns=%d err=%s",
                    m_checkValid ? 1 : 0, nodeCount(),
                    m_checkValid ? check.startId.c_str() : "",
                    static_cast<int>(m_checkWarnings.size()),
                    m_checkValid ? "" : m_checkError.c_str());
    }
}

// ============================================================
// 编辑缓冲
// ============================================================

dialogue::DlgNode* DialogueEditorWindow::selNode() {
    if (m_asset.nodes.empty()) return nullptr;
    if (m_sel < 0) m_sel = 0;
    if (m_sel >= nodeCount()) m_sel = nodeCount() - 1;
    return &m_asset.nodes[static_cast<std::size_t>(m_sel)];
}

void DialogueEditorWindow::refillIdBuf() {
    const dialogue::DlgNode* n = m_asset.nodes.empty()
                                     ? nullptr
                                     : &m_asset.nodes[static_cast<std::size_t>(
                                           std::max(0, std::min(m_sel, nodeCount() - 1)))];
    if (!n) {
        m_idBuf[0] = '\0';
        m_speakerBuf[0] = '\0';
        std::fill(m_textBuf.begin(), m_textBuf.end(), '\0');
        return;
    }
    std::snprintf(m_idBuf, sizeof(m_idBuf), "%s", n->id.c_str());
    std::snprintf(m_speakerBuf, sizeof(m_speakerBuf), "%s", n->speaker.c_str());
    std::fill(m_textBuf.begin(), m_textBuf.end(), '\0');
    std::snprintf(m_textBuf.data(), m_textBuf.size(), "%s", n->text.c_str());
}

void DialogueEditorWindow::syncBuffers() {
    const dialogue::DlgNode* n = selNode();
    const std::string want = n ? n->id : std::string();
    // 只按 **id** 判断要不要重填：按下标判断的话，删掉中间一行之后
    // 缓冲会继续显示"新第 3 行"的内容，而它其实是刚上移过来的第 4 行。
    if (n && m_bufForId == want) return;
    m_bufForId = want;
    refillIdBuf();
}

void DialogueEditorWindow::markDirty() { m_dirty = true; }

bool DialogueEditorWindow::reject(const char* op, const std::string& reason) {
    m_uiError = std::string(TR("Rejected")) + ": " + reason;
    VK_LOG_WARN("DLGED-REJECT op=%s reason=%s", op, reason.c_str());
    return false;
}

// ============================================================
// 动作
// ============================================================

void DialogueEditorWindow::rebuildIndex() {
    m_asset.index.clear();
    for (int i = 0; i < nodeCount(); ++i)
        m_asset.index[m_asset.nodes[static_cast<std::size_t>(i)].id] = i;
}

std::string DialogueEditorWindow::uniqueId(const std::string& base) const {
    const std::string b = base.empty() ? std::string("line") : base;
    if (dialogue::findNode(m_asset, b) < 0) return b;
    for (int i = 2; i < 10000; ++i) {
        const std::string c = b + std::to_string(i);
        if (dialogue::findNode(m_asset, c) < 0) return c;
    }
    return b + "_x";
}

int DialogueEditorWindow::countRefs(const std::string& id) const {
    int n = 0;
    for (const auto& node : m_asset.nodes) {
        if (node.nextId == id) ++n;
        for (const auto& c : node.choices)
            if (c.gotoId == id) ++n;
    }
    return n;
}

std::vector<char> DialogueEditorWindow::reachable() const {
    std::vector<char> seen(m_asset.nodes.size(), 0);
    const int s = dialogue::findNode(m_asset, m_asset.startId);
    if (s < 0) return seen;
    std::vector<int> stack{s};
    seen[static_cast<std::size_t>(s)] = 1;
    while (!stack.empty()) {
        const int i = stack.back();
        stack.pop_back();
        const dialogue::DlgNode& n = m_asset.nodes[static_cast<std::size_t>(i)];
        auto push = [&](const std::string& id) {
            const int j = dialogue::findNode(m_asset, id);
            if (j >= 0 && !seen[static_cast<std::size_t>(j)]) {
                seen[static_cast<std::size_t>(j)] = 1;
                stack.push_back(j);
            }
        };
        if (!n.nextId.empty()) push(n.nextId);
        for (const auto& c : n.choices) push(c.gotoId);
    }
    return seen;
}

bool DialogueEditorWindow::actNew(const std::string& wantId) {
    std::string id = wantId;
    if (!id.empty() && dialogue::findNode(m_asset, id) >= 0)
        return reject("new", "id \"" + id + "\" already exists");
    if (id.empty()) id = uniqueId("line");

    const int prevSel = m_sel;
    const int prevCount = nodeCount();

    dialogue::DlgNode n;
    n.id = id;
    m_asset.nodes.push_back(std::move(n));
    m_sel = nodeCount() - 1;

    // 自动接线：**选中最后一句**、且它身上既没有 next 也没有 choices
    // → 把新的一句接上去。写线性对话时这是唯一想做的事，而"加了五句
    // 结果只播第一句"是最常见的第一次踩坑。
    // ⚠ 只在"选中的就是最后一句"时做：否则作者刚点到某个分支末尾加一句，
    //   会被静默改掉那个分支的出口。
    std::string from;
    if (prevSel == prevCount - 1 && prevSel >= 0) {
        dialogue::DlgNode& p = m_asset.nodes[static_cast<std::size_t>(prevSel)];
        if (p.nextId.empty() && p.choices.empty()) {
            p.nextId = id;
            from = p.id;
        }
    }
    if (m_asset.startId.empty()) m_asset.startId = id;   // 第一句自动当起点

    rebuildIndex();
    m_bufForId.clear();
    markDirty();
    VK_LOG_INFO("DLGED-NEW id=%s from=%s total=%d", id.c_str(),
                from.empty() ? "-" : from.c_str(), nodeCount());
    revalidate();
    return true;
}

bool DialogueEditorWindow::actDuplicate() {
    const dialogue::DlgNode* n = selNode();
    if (!n) return reject("dup", "no line selected");
    const std::string fromId = n->id;
    dialogue::DlgNode copy = *n;
    copy.id = uniqueId(n->id + "_copy");
    copy.nextId.clear();   // 复制的副本不该指向原节点后面的那句
    const int at = m_sel + 1;
    m_asset.nodes.insert(m_asset.nodes.begin() + at, std::move(copy));
    m_sel = at;
    rebuildIndex();
    m_bufForId.clear();
    markDirty();
    VK_LOG_INFO("DLGED-DUP from=%s to=%s total=%d", fromId.c_str(),
                m_asset.nodes[static_cast<std::size_t>(at)].id.c_str(), nodeCount());
    revalidate();
    return true;
}

bool DialogueEditorWindow::actDelete() {
    const dialogue::DlgNode* n = selNode();
    if (!n) return reject("del", "no line selected");

    const std::string gone = n->id;
    const int refs = countRefs(gone);
    const bool wasStart = (m_asset.startId == gone);

    // ⚠ 不顺手改写别人的 next：那等于静默改剧本（"我只删了一句，怎么
    //   跳到别的地方去了"）。删完由校验器点名谁还指着它。
    m_asset.nodes.erase(m_asset.nodes.begin() + m_sel);

    if (wasStart) {
        // 起点必须指向存在的节点，否则整份文件加载失败 —— 这一处只能
        // 自动往前挪，但一定记账。
        m_asset.startId = nodeCount() ? m_asset.nodes.front().id : std::string();
        VK_LOG_INFO("DLGED-START-MOVED old=%s new=%s reason=deleted",
                    gone.c_str(), m_asset.startId.c_str());
    }

    if (m_sel >= nodeCount()) m_sel = nodeCount() - 1;
    if (m_sel < 0) m_sel = 0;
    rebuildIndex();
    m_bufForId.clear();
    markDirty();
    VK_LOG_INFO("DLGED-DEL id=%s refs=%d total=%d", gone.c_str(), refs, nodeCount());
    revalidate();
    return true;
}

bool DialogueEditorWindow::actMove(int dir) {
    if (dir == 0) return false;
    const dialogue::DlgNode* n = selNode();
    if (!n) return reject("move", "no line selected");
    const int to = m_sel + (dir > 0 ? 1 : -1);
    if (to < 0 || to >= nodeCount()) return false;
    std::swap(m_asset.nodes[static_cast<std::size_t>(m_sel)],
              m_asset.nodes[static_cast<std::size_t>(to)]);
    m_sel = to;
    rebuildIndex();   // 顺序变了 → 下标变了 → 索引必须重建
    m_bufForId.clear();
    markDirty();
    VK_LOG_INFO("DLGED-MOVE id=%s from=%d to=%d",
                m_asset.nodes[static_cast<std::size_t>(to)].id.c_str(),
                to - (dir > 0 ? 1 : -1), to);
    revalidate();
    return true;
}

bool DialogueEditorWindow::actSetId(const std::string& newId) {
    dialogue::DlgNode* n = selNode();
    if (!n) return reject("setid", "no line selected");
    if (newId.empty()) return reject("setid", "id is empty");
    const std::string old = n->id;
    if (old == newId) return true;
    if (dialogue::findNode(m_asset, newId) >= 0)
        return reject("setid", "id \"" + newId + "\" already exists");

    // ② 连引用一起改：next / goto / start 全表打补丁。
    //   不打补丁的后果不是"报错"，是作者得自己把每一个 next 找回去 ——
    //   校验器只会说"next 'old' is not a line id"，不告诉他在哪几行。
    int patched = 0;
    for (auto& node : m_asset.nodes) {
        if (node.nextId == old) {
            node.nextId = newId;
            ++patched;
        }
        for (auto& c : node.choices) {
            if (c.gotoId == old) {
                c.gotoId = newId;
                ++patched;
            }
        }
    }
    if (m_asset.startId == old) m_asset.startId = newId;

    n->id = newId;
    rebuildIndex();
    m_bufForId = newId;
    markDirty();
    VK_LOG_INFO("DLGED-RENAME old=%s new=%s patched=%d", old.c_str(),
                newId.c_str(), patched);
    revalidate();
    return true;
}

bool DialogueEditorWindow::actSetSpeaker(const std::string& s) {
    dialogue::DlgNode* n = selNode();
    if (!n) return false;
    if (n->speaker == s) return true;
    n->speaker = s;
    markDirty();
    revalidate();
    return true;
}

bool DialogueEditorWindow::actSetText(const std::string& t) {
    dialogue::DlgNode* n = selNode();
    if (!n) return false;
    if (n->text == t) return true;
    n->text = t;
    markDirty();
    revalidate();
    return true;
}

bool DialogueEditorWindow::actSetNext(const std::string& id) {
    dialogue::DlgNode* n = selNode();
    if (!n) return reject("setnext", "no line selected");
    // choices 与 next 不许并存（`parseDialogue` 判错）。这里有选项就直接拒
    // —— 反过来"加选项时把 next 清掉"是可以的，因为 next 只是**一个指针**，
    // 而选项是**一串文本**，清掉就找不回来了。
    if (!n->choices.empty())
        return reject("setnext", "this line has choices; next and choices "
                                "cannot coexist (clear the choices first)");
    if (!id.empty() && dialogue::findNode(m_asset, id) < 0)
        return reject("setnext", "no line with id \"" + id + "\"");
    if (n->nextId == id) return true;
    n->nextId = id;
    markDirty();
    VK_LOG_INFO("DLGED-NEXT id=%s next=%s", n->id.c_str(),
                id.empty() ? "(end)" : id.c_str());
    revalidate();
    return true;
}

bool DialogueEditorWindow::actSetStart() {
    dialogue::DlgNode* n = selNode();
    if (!n) return reject("start", "no line selected");
    if (m_asset.startId == n->id) return true;
    m_asset.startId = n->id;
    markDirty();
    VK_LOG_INFO("DLGED-START id=%s", m_asset.startId.c_str());
    revalidate();
    return true;
}

bool DialogueEditorWindow::actAddChoice(const std::string& gotoId) {
    dialogue::DlgNode* n = selNode();
    if (!n) return reject("addchoice", "no line selected");
    if (m_asset.nodes.empty()) return reject("addchoice", "the script is empty");
    std::string g = gotoId.empty() ? m_asset.nodes.front().id : gotoId;
    if (dialogue::findNode(m_asset, g) < 0)
        return reject("addchoice", "no line with id \"" + g + "\"");

    if (!n->nextId.empty()) {
        // 格式强制：两者不许并存。清掉 next 是**可见**的（界面上立刻
        // 变成 (end)），也记账 —— 与 del 的"不静默改剧本"不矛盾：这里
        // 改的正是作者刚点的那一行。
        VK_LOG_INFO("DLGED-UNLINK id=%s old_next=%s reason=choices",
                    n->id.c_str(), n->nextId.c_str());
        n->nextId.clear();
    }
    dialogue::DlgChoice c;
    c.text = TR("New choice");
    c.gotoId = g;
    n->choices.push_back(std::move(c));
    markDirty();
    VK_LOG_INFO("DLGED-ADDCHOICE id=%s goto=%s n=%d", n->id.c_str(), g.c_str(),
                static_cast<int>(n->choices.size()));
    revalidate();
    return true;
}

bool DialogueEditorWindow::actDelChoice(int i) {
    dialogue::DlgNode* n = selNode();
    if (!n) return false;
    if (i < 0 || i >= static_cast<int>(n->choices.size()))
        return reject("delchoice", "index out of range");
    n->choices.erase(n->choices.begin() + i);
    markDirty();
    VK_LOG_INFO("DLGED-DELCHOICE id=%s i=%d n=%d", n->id.c_str(), i,
                static_cast<int>(n->choices.size()));
    revalidate();
    return true;
}

bool DialogueEditorWindow::actSetChoiceText(int i, const std::string& t) {
    dialogue::DlgNode* n = selNode();
    if (!n || i < 0 || i >= static_cast<int>(n->choices.size())) return false;
    if (n->choices[static_cast<std::size_t>(i)].text == t) return true;
    n->choices[static_cast<std::size_t>(i)].text = t;
    markDirty();
    revalidate();
    return true;
}

bool DialogueEditorWindow::actSetChoiceGoto(int i, const std::string& id) {
    dialogue::DlgNode* n = selNode();
    if (!n || i < 0 || i >= static_cast<int>(n->choices.size())) return false;
    if (dialogue::findNode(m_asset, id) < 0)
        return reject("setchoicegoto", "no line with id \"" + id + "\"");
    if (n->choices[static_cast<std::size_t>(i)].gotoId == id) return true;
    n->choices[static_cast<std::size_t>(i)].gotoId = id;
    markDirty();
    VK_LOG_INFO("DLGED-CHOICEGOTO id=%s i=%d goto=%s", n->id.c_str(), i,
                id.c_str());
    revalidate();
    return true;
}

bool DialogueEditorWindow::actNewScript() {
    m_asset = dialogue::DialogueAsset{};
    // 名字先给个占位：真正的名字由 `parseDialogue` 从**路径**推（存盘后
    // Reload 就会变成文件名）。这里留着 "untitled" 是为了标题栏不空。
    m_asset.name = "untitled";
    m_asset.sourcePath = m_path;

    dialogue::DlgNode a;
    a.id = "l1";
    a.speaker = TR("Narrator");
    a.text = TR("First line. Edit this text, then press Save.");
    a.nextId = "l2";
    dialogue::DlgNode b;
    b.id = "l2";
    b.speaker = TR("Narrator");
    b.text = TR("Second line. Add choices to branch from here.");
    m_asset.nodes.push_back(std::move(a));
    m_asset.nodes.push_back(std::move(b));
    m_asset.startId = "l1";

    rebuildIndex();
    m_sel = 0;
    m_bufForId.clear();
    m_dirty = true;
    m_readOnly = false;    // 新建的资产当然可以存
    m_error.clear();
    m_uiError.clear();
    VK_LOG_INFO("DLGED-NEWSCRIPT lines=2 start=l1 path=%s",
                m_path.empty() ? "(none)" : m_path.c_str());
    revalidate();
    return true;
}

// ============================================================
// 存盘
// ============================================================

bool DialogueEditorWindow::save(bool force) {
    if (m_readOnly) {
        m_uiError = TR("Read-only: the file failed to load, saving would "
                       "truncate it. Fix it and press Reload.");
        VK_LOG_WARN("DLGED-SAVE-BLOCKED path=%s reason=read-only",
                    m_path.c_str());
        return false;
    }
    if (m_path.empty()) {
        m_uiError = TR("Set an asset path first (e.g. dialogues/my_scene.dlg.json).");
        VK_LOG_WARN("DLGED-SAVE-BLOCKED path=(none) reason=no-path");
        return false;
    }

    revalidate();
    if (!m_checkValid && !force) {
        // ④ 默认不许存一份加载不了的剧本：它的症状会出现在运行时
        //（进 Play 黑屏 + 一行 DLG-LOAD-FAIL），而作者以为"我存过了"。
        m_uiError = std::string(TR("Not saved — the script does not load:")) +
                    " " + m_checkError;
        VK_LOG_WARN("DLGED-SAVE-BLOCKED path=%s reason=check err=%s",
                    m_path.c_str(), m_checkError.c_str());
        return false;
    }

    std::string err;
    if (!dialogue::saveDialogue(m_path, m_asset, err)) {
        m_error = err;
        m_uiError = err;
        VK_LOG_ERROR("DLGED-SAVE-FAIL path=%s err=%s", m_path.c_str(),
                     err.c_str());
        return false;
    }

    m_dirty = false;
    m_error.clear();
    m_uiError.clear();
    VK_LOG_INFO("DLGED-SAVE path=%s lines=%d force=%d", m_path.c_str(),
                nodeCount(), force ? 1 : 0);
    refreshSceneComponents();
    return true;
}

void DialogueEditorWindow::refreshSceneComponents() {
    const std::string mine = canonicalOf(m_path);
    if (mine.empty()) return;
    scene::Scene& sc = m_ctx.activeScene();
    ecs::World& w = sc.world();

    int reloaded = 0;
    int skipped = 0;
    w.each<ecs::DialogueComponent>([&](ecs::Entity e, ecs::DialogueComponent& c) {
        if (canonicalOf(c.dialoguePath) != mine) return;
        if (c.player.running()) {
            // 正在播的那一份不打断 —— 下一句就会读到旧文本，但中途把玩家
            // 的台词换掉更糟。下一次起播（`start()` 会重新读？不会，同路径
            // 缓存）—— 所以在 `started=false` 时才会真正重读。
            ++skipped;
            VK_LOG_INFO("DLGED-SCENE-SKIP entity=%u path=%s (playing)",
                        static_cast<unsigned>(e.id), c.dialoguePath.c_str());
            return;
        }
        // ⚠ `DialoguePlayer::load` 同路径**不重读**（它按路径缓存，Inspector
        //   每帧喂一次路径）。必须先把 player 整个复位，缓存才失效。
        c.player = dialogue::DialoguePlayer{};
        c.player.load(c.dialoguePath);
        c.started = false;
        c.loadFailedLogged = false;
        ++reloaded;
    });
    VK_LOG_INFO("DLGED-SCENE-RELOAD path=%s reloaded=%d skipped=%d",
                m_path.c_str(), reloaded, skipped);
}

// ============================================================
// 绘制
// ============================================================

void DialogueEditorWindow::draw(bool& open) {
    if (!m_open) return;

    ImGuiWindowFlags wf = 0;
    const bool docked = applyDockHint(wf);
    if (!docked) {
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowSizeConstraints(ImVec2(760.0f, 430.0f),
                                            ImVec2(FLT_MAX, FLT_MAX));
        ImGui::SetNextWindowSize(ImVec2(1040.0f, 660.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.14f,
                                       io.DisplaySize.y * 0.06f),
                                ImGuiCond_FirstUseEver);
    }

    // 标题里的 `*` = 有未存的改动。`###panel_dialogueed` 是稳定 ID：标题随路径
    // 变化时窗口的位置/大小记忆不能跟着丢。
    // ⚠ 用 std::string 拼而不是 snprintf：`###` 那段是 ImGui 的稳定 ID，
    //   不是给人看的字 —— 放进格式串会被 `verify_i18n_coverage.py` 的 D 组
    //   抓出来（它的判据是"格式串没包 TR 就是没翻译"，而那一段永远不该翻）。
    std::string title = TR("Script editor");
    if (!m_path.empty()) {
        title += " — ";
        title += m_asset.name.empty() ? m_path : m_asset.name;
        if (m_dirty) title += " *";
    }
    title += "###panel_dialogueed";

    if (!ImGui::Begin(title.c_str(), docked ? nullptr : &open, wf)) {
        ImGui::End();
        return;
    }
    logRect("DLGED-WIN", ImGui::GetWindowPos(),
            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                   ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    // 注入脚本排在最前：它要在工具栏之前跑，这样脚本改出来的状态
    // 同一帧就能被画出来（脚本断言与截图看到的是同一个东西）。
    runInjectedScript();

    drawToolbar(open);
    ImGui::Separator();

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float validH = 128.0f;
    const float bodyH = std::max(180.0f, avail.y - validH - 6.0f);
    drawNodeList(bodyH);
    ImGui::SameLine();
    drawNodeEditor(bodyH);
    drawValidation();

    ImGui::End();

    // 点 × 的那一帧 `open` 已经被 ImGui 置 false —— 自己收掉，别等调用方。
    if (!open) close();
}

void DialogueEditorWindow::drawToolbar(bool& open) {
    (void)open;

    ImGui::TextDisabled("%s", TR("Asset path"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(360.0f);
    if (ImGui::InputTextWithHint("##dlged_path", "dialogues/ch01_open_01.dlg.json",
                                 m_pathBuf, sizeof(m_pathBuf),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
        // 改路径**不自动读盘**（半截路径会立刻报"打不开"），回车才读。
        m_path = m_pathBuf;
        reload();
    }
    logRect("DLGED-PATH", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

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
    logRect("DLGED-SAVE-BTN", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::EndDisabled();

    // 逃生门：校验没过时也能存（WIP 存档是正常需求），但必须是**显式**
    // 的一次点击，而不是 Save 的默认行为。
    if (!m_checkValid && !m_readOnly) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.28f, 0.18f, 1.0f));
        if (ImGui::Button(TR("Save anyway"))) save(true);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", TR("Write even though the script does not "
                                       "load. Use it for work in progress."));
    }

    ImGui::SameLine();
    if (ImGui::Button(TR("New script"))) actNewScript();
    logRect("DLGED-NEW-BTN", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    // ---- 状态 ----
    if (m_readOnly) {
        ImGui::TextColored(kBadCol, "%s", TR("Read-only — the file did not load, "
                                             "saving is disabled."));
        if (!m_error.empty()) ImGui::TextDisabled("%s", m_error.c_str());
    } else if (m_dirty) {
        ImGui::TextColored(kWarnCol, "%s", TR("Unsaved changes."));
    }
    if (!m_uiError.empty())
        ImGui::TextColored(kBadCol, "%s", m_uiError.c_str());
}

void DialogueEditorWindow::drawNodeList(float height) {
    ImGui::BeginChild("##dlged_nodes", ImVec2(268.0f, height),
                      ImGuiChildFlags_Borders);
    logRect("DLGED-NODELIST", ImGui::GetWindowPos(),
            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                   ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    ImGui::TextDisabled(TR("%d line(s)"), nodeCount());
    ImGui::SameLine();
    if (ImGui::SmallButton(TR("+ Line"))) actNew();
    ImGui::SameLine();
    if (ImGui::SmallButton(TR("Dup"))) actDuplicate();
    ImGui::SameLine();
    if (ImGui::SmallButton(TR("Del"))) actDelete();
    if (ImGui::SmallButton(TR("Up"))) actMove(-1);
    ImGui::SameLine();
    if (ImGui::SmallButton(TR("Down"))) actMove(1);

    ImGui::Separator();

    // 游离节点（从 start 走不到）只在提示栏里说，不参与校验 —— 它可能是
    // 作者故意留的废稿，而"存不了盘"是对这件事的过度反应。
    const std::vector<char> reach = reachable();

    ImGui::BeginChild("##dlged_scroll", ImVec2(0.0f, 0.0f));
    for (int i = 0; i < nodeCount(); ++i) {
        const dialogue::DlgNode& n = m_asset.nodes[static_cast<std::size_t>(i)];
        // 标记：▶ 起点 / · 走不到 / 空
        const char* mark = (n.id == m_asset.startId)
                               ? ">"
                               : (static_cast<std::size_t>(i) < reach.size() &&
                                          !reach[static_cast<std::size_t>(i)])
                                     ? "."
                                     : " ";
        // 一行的信息量：谁（说话人）+ 哪个（id）+ 说什么（截断的预览）。
        // ⚠ 预览按**码点**截到 10 —— 18 个字在 268px 宽的栏里会折成两行，
        //   整份剧本就变成"一行台词占两行"，扫读性反而更差。
        char label[320];
        std::snprintf(label, sizeof(label), "%s %s [%s] %s", mark,
                      n.id.c_str(),
                      dialogue::utf8Prefix(n.speaker, 5).c_str(),
                      previewOf(n, 10).c_str());
        ImGui::PushID(i);
        if (ImGui::Selectable(label, m_sel == i)) selectNode(i);
        if (m_sel == i)
            logRect("DLGED-ROW", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::EndChild();
}

void DialogueEditorWindow::selectNode(int i) {
    if (i < 0 || i >= nodeCount() || i == m_sel) return;
    m_sel = i;
    m_uiError.clear();
    VK_LOG_INFO("DLGED-SEL i=%d id=%s", i,
                m_asset.nodes[static_cast<std::size_t>(i)].id.c_str());
}

void DialogueEditorWindow::drawNodeEditor(float height) {
    ImGui::BeginChild("##dlged_edit", ImVec2(0.0f, height),
                      ImGuiChildFlags_Borders);
    logRect("DLGED-EDITOR", ImGui::GetWindowPos(),
            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                   ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    dialogue::DlgNode* n = selNode();
    if (!n) {
        ImGui::TextDisabled("%s", TR("No line selected."));
        if (nodeCount() == 0) {
            ImGui::Spacing();
            if (ImGui::Button(TR("New script"))) actNewScript();
            ImGui::SameLine();
            ImGui::TextDisabled("%s", TR("…or type an asset path above and press Reload."));
        }
        ImGui::EndChild();
        return;
    }

    syncBuffers();
    ImGui::PushID(m_sel);

    // ---- id / speaker ----
    ImGui::TextDisabled("%s", TR("id"));
    ImGui::SameLine(64.0f);
    ImGui::SetNextItemWidth(170.0f);
    ImGui::InputText("##dlged_id", m_idBuf, sizeof(m_idBuf));
    logRect("DLGED-ID", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    // ⚠ 改名只在**编辑结束时**做。逐字应用的话，`l1` 改成 `l12` 会经过
    //   `l12`→`l1`(自己) 中间态；而 `l1`→`l` 再 →`l9` 会先和别的 id 撞。
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        if (!actSetId(m_idBuf)) refillIdBuf();   // 被拒 → 显示回原值
    }

    ImGui::SameLine();
    ImGui::TextDisabled("%s", TR("speaker"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::InputText("##dlged_speaker", m_speakerBuf, sizeof(m_speakerBuf)))
        actSetSpeaker(m_speakerBuf);
    logRect("DLGED-SPEAKER", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    ImGui::SameLine();
    if (ImGui::Button(TR("Set as start"))) actSetStart();
    if (n->id == m_asset.startId) {
        ImGui::SameLine();
        ImGui::TextColored(kOkCol, "%s", TR("(start)"));
    }

    // ---- 正文 ----
    ImGui::Separator();
    const float textH = std::max(76.0f, ImGui::GetContentRegionAvail().y * 0.40f);
    if (ImGui::InputTextMultiline("##dlged_text", m_textBuf.data(),
                                  m_textBuf.size(), ImVec2(-1.0f, textH)))
        actSetText(m_textBuf.data());
    logRect("DLGED-TEXT", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::TextDisabled(TR("%d chars  |  %d bytes"),
                        dialogue::utf8Count(n->text),
                        static_cast<int>(n->text.size()));

    // ---- 流程 ----
    ImGui::SeparatorText(TR("Flow"));
    if (!n->choices.empty()) {
        ImGui::TextDisabled("%s", TR("This line ends with choices — \"next\" is "
                                     "not allowed alongside them."));
    } else {
        ImGui::TextDisabled("%s", TR("next"));
        ImGui::SameLine(64.0f);
        ImGui::SetNextItemWidth(190.0f);
        const std::string cur =
            n->nextId.empty() ? std::string(TR("(end)")) : n->nextId;
        if (ImGui::BeginCombo("##dlged_next", cur.c_str())) {
            if (ImGui::Selectable(TR("(end)"), n->nextId.empty())) actSetNext("");
            for (int i = 0; i < nodeCount(); ++i) {
                const std::string id = m_asset.nodes[static_cast<std::size_t>(i)].id;
                if (ImGui::Selectable(id.c_str(), id == n->nextId)) actSetNext(id);
            }
            ImGui::EndCombo();
        }
        logRect("DLGED-NEXT", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        if (!n->nextId.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton(TR("clear"))) actSetNext("");
        }
    }

    // ---- 选项 ----
    ImGui::SeparatorText(TR("Choices"));
    int eraseAt = -1;
    for (int ci = 0; ci < static_cast<int>(n->choices.size()); ++ci) {
        ImGui::PushID(9000 + ci);
        char cbuf[256];
        std::snprintf(cbuf, sizeof(cbuf), "%s",
                      n->choices[static_cast<std::size_t>(ci)].text.c_str());
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::InputText("##ctext", cbuf, sizeof(cbuf)))
            actSetChoiceText(ci, cbuf);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150.0f);
        const std::string& g = n->choices[static_cast<std::size_t>(ci)].gotoId;
        if (ImGui::BeginCombo("##cgoto", g.c_str())) {
            for (int i = 0; i < nodeCount(); ++i) {
                const std::string id = m_asset.nodes[static_cast<std::size_t>(i)].id;
                if (ImGui::Selectable(id.c_str(), id == g)) actSetChoiceGoto(ci, id);
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) eraseAt = ci;
        ImGui::PopID();
    }
    if (eraseAt >= 0) actDelChoice(eraseAt);
    else if (ImGui::SmallButton(TR("+ choice"))) actAddChoice({});

    ImGui::PopID();
    ImGui::EndChild();
}

void DialogueEditorWindow::drawValidation() {
    ImGui::BeginChild("##dlged_valid", ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_Borders);
    logRect("DLGED-VALID", ImGui::GetWindowPos(),
            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                   ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    // ---- 第一段：运行时校验器的结论（权威）----
    if (m_checkValid) {
        ImGui::TextColored(kOkCol, "%s", TR("Load check: OK"));
        ImGui::SameLine();
        ImGui::TextDisabled(TR("%d line(s), start=%s"), nodeCount(),
                            m_asset.startId.c_str());
    } else {
        ImGui::TextColored(kBadCol, "%s", TR("Load check: FAILED"));
        if (!m_checkError.empty()) ImGui::TextColored(kBadCol, "  %s", m_checkError.c_str());
    }
    for (const auto& w : m_checkWarnings)
        ImGui::TextColored(kWarnCol, "  %s", w.c_str());

    // ---- 第二段：编辑器自己的建议（**不是**校验规则）----
    // 与上一段分开摆，是因为它们的效力不同：上面那条不解决就存不了盘，
    // 下面这些只是"你可能想看看"。混在一起会让人以为文件坏了。
    const std::vector<char> reach = reachable();
    std::vector<std::string> advices;
    for (int i = 0; i < nodeCount(); ++i) {
        const dialogue::DlgNode& n = m_asset.nodes[static_cast<std::size_t>(i)];
        if (static_cast<std::size_t>(i) < reach.size() &&
            !reach[static_cast<std::size_t>(i)]) {
            // ⚠ 整句一个格式串，不要拼成"前缀 + id + 后缀"：中文语序与英文
            //   不同，拆开之后译者只能翻半句。
            advices.push_back(fmt(TR("unreachable: %s (nothing points here)"),
                                  n.id.c_str()));
        }
        if (!n.nextId.empty() && n.nextId == n.id)
            advices.push_back(fmt(TR("self loop: %s"), n.id.c_str()));
        if (n.text.size() == 0 && n.choices.empty())
            advices.push_back(fmt(TR("empty line: %s"), n.id.c_str()));
    }
    if (!advices.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("%s", TR("Advice (not a load error):"));
        for (const auto& a : advices)
            ImGui::TextDisabled("  %s", a.c_str());
    }

    ImGui::EndChild();
}

// ============================================================
// 自动化注入
// ============================================================

void DialogueEditorWindow::runInjectedScript() {
    if (m_injected || !m_loaded) return;
    const char* raw = std::getenv("MYVK_EDITOR_DLGED_SCRIPT");
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
        // 去空白
        const std::size_t b = tok.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) continue;
        tok = tok.substr(b);
        while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t')) tok.pop_back();
        if (tok.empty()) continue;

        // `名字[:参数]`。参数里允许出现 `:`（路径 `C:/…`），所以只切第一刀。
        const std::size_t colon = tok.find(':');
        const std::string name = tok.substr(0, colon);
        const std::string arg = (colon == std::string::npos) ? std::string()
                                                            : tok.substr(colon + 1);

        bool ok = false;
        if (name == "sel") {
            const int i = std::atoi(arg.c_str());
            if (i >= 0 && i < nodeCount()) {
                m_sel = i;
                m_uiError.clear();
                ok = true;
                VK_LOG_INFO("DLGED-SEL i=%d id=%s", i,
                            m_asset.nodes[static_cast<std::size_t>(i)].id.c_str());
            }
        } else if (name == "new") ok = actNew(arg);
        else if (name == "dup") ok = actDuplicate();
        else if (name == "del") ok = actDelete();
        else if (name == "up") ok = actMove(-1);
        else if (name == "down") ok = actMove(1);
        else if (name == "setid") ok = actSetId(arg);
        else if (name == "setspeaker") ok = actSetSpeaker(arg);
        else if (name == "settext") ok = actSetText(arg);
        else if (name == "setnext") ok = actSetNext(arg == "-" ? std::string() : arg);
        else if (name == "start") ok = actSetStart();
        else if (name == "addchoice") ok = actAddChoice(arg);
        else if (name == "delchoice") ok = actDelChoice(std::atoi(arg.c_str()));
        else if (name == "newscript") ok = actNewScript();
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
            const std::string text = dialogue::toJson(m_asset);
            if (const char* dp = std::getenv("MYVK_EDITOR_DLGED_DUMP")) {
                if (FILE* f = std::fopen(dp, "wb")) {
                    std::fwrite(text.data(), 1, text.size(), f);
                    std::fclose(f);
                    VK_LOG_INFO("DLGED-DUMP path=%s lines=%d bytes=%d ok=1", dp,
                                nodeCount(), static_cast<int>(text.size()));
                } else {
                    VK_LOG_WARN("DLGED-DUMP path=%s ok=0 (cannot open)", dp);
                }
            } else {
                VK_LOG_WARN("DLGED-DUMP ok=0 (MYVK_EDITOR_DLGED_DUMP unset)");
            }
            ok = true;
        } else {
            VK_LOG_WARN("DLGED-SCRIPT-BAD %s", tok.c_str());
        }

        if (ok) ++applied;
        else {
            ++failed;
            VK_LOG_WARN("DLGED-SCRIPT-FAIL %s", tok.c_str());
        }
    }

    // 脚本改的是资产，缓冲要跟上（否则界面还显示改之前的那句）
    m_bufForId.clear();
    syncBuffers();
    VK_LOG_INFO("DLGED-SCRIPT applied=%d failed=%d", applied, failed);
}

} // namespace editor
