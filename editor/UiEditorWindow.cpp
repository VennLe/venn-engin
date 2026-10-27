// ============================================================
// editor/UiEditorWindow.cpp
//
// 结构：工具栏（路径 / Reload / Save / New）→ 左栏控件树 → 中间设计画布
//      → 右栏属性 → 底部校验栏。所有**改动数据**的入口都是 `act*` 那一族，
//      界面按钮与注入脚本共用它们（同剧本 / 时间轴编辑器）。
//
// ⚠ 日志锚点前缀 `UIED-`（UI EDitor），与运行时的 `UIDOC-*`、基础层的
//   `UI2-*` 分开：三套东西同时出现在一份日志里时，"搜某前缀全绿"才有意义。
// ============================================================

#include "UiEditorWindow.h"

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
#include <functional>
#include <utility>

namespace editor {

namespace {

const ImVec4 kOkCol(0.45f, 0.85f, 0.52f, 1.0f);
const ImVec4 kBadCol(1.00f, 0.45f, 0.35f, 1.0f);
const ImVec4 kWarnCol(1.00f, 0.75f, 0.30f, 1.0f);

// 画布留白：设计稿贴着面板边缘的话，右锚/下锚的控件看起来像被裁掉了。
constexpr float kCanvasPad = 10.0f;
// 拖动吸附（参考像素）：整数像素是作者的真实需求（"离左边 24"），
// 连续小数只会让人对不齐。
constexpr float kSnap = 1.0f;
// 锚点标记的命中半径（屏幕像素）
constexpr float kAnchorHit = 9.0f;

// `#RRGGBBAA`（与 `UiDocument` 的落盘格式同一条），不合法返回 false。
bool parseHexColor(const std::string& s, uint32_t& out) {
    if ((s.size() != 7 && s.size() != 9) || s[0] != '#') return false;
    int v[4] = {255, 255, 255, 255};
    const int n = static_cast<int>((s.size() - 1) / 2);
    for (int i = 0; i < n; ++i) {
        int hi = 0, lo = 0;
        for (int k = 0; k < 2; ++k) {
            const char ch = s[static_cast<std::size_t>(1 + i * 2 + k)];
            int d = -1;
            if (ch >= '0' && ch <= '9') d = ch - '0';
            else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
            if (d < 0) return false;
            if (k == 0) hi = d; else lo = d;
        }
        v[i] = hi * 16 + lo;
    }
    out = ui2::rgba(v[0], v[1], v[2], v[3]);
    return true;
}

void toF4(uint32_t c, float out[4]) {
    out[0] = static_cast<float>(c & 0xFFu) / 255.0f;
    out[1] = static_cast<float>((c >> 8) & 0xFFu) / 255.0f;
    out[2] = static_cast<float>((c >> 16) & 0xFFu) / 255.0f;
    out[3] = static_cast<float>((c >> 24) & 0xFFu) / 255.0f;
}

uint32_t fromF4(const float c[4]) {
    const auto q = [](float v) {
        const float x = v * 255.0f + 0.5f;
        return static_cast<int>(x < 0.0f ? 0.0f : (x > 255.0f ? 255.0f : x));
    };
    return ui2::rgba(q(c[0]), q(c[1]), q(c[2]), q(c[3]));
}

// ImGui 的 IM_COL32 与 `ui2::rgba` 的**字节布局相同**（R 在最低字节），
// 所以这里可以直接搬，不需要逐通道翻译。
ImU32 imCol(uint32_t c, int alphaOverride = -1) {
    if (alphaOverride < 0) return static_cast<ImU32>(c);
    const uint32_t a = static_cast<uint32_t>(alphaOverride & 0xFF);
    return static_cast<ImU32>((c & 0x00FFFFFFu) | (a << 24));
}

const char* typeLabel(ui2::UiWidgetType t) {
    switch (t) {
        case ui2::UiWidgetType::Panel: return "panel";
        case ui2::UiWidgetType::Nine: return "nine";
        case ui2::UiWidgetType::Image: return "image";
        case ui2::UiWidgetType::Text: return "text";
        case ui2::UiWidgetType::Button: return "button";
        case ui2::UiWidgetType::Progress: return "progress";
        default: return "?";
    }
}

// 每种控件的"两个颜色槽叫什么"—— 属性面板上直接写用途，作者不用记
// "color2 对 nine 是填充还是边框"。
const char* slotALabel(ui2::UiWidgetType t) {
    switch (t) {
        case ui2::UiWidgetType::Nine: return "Border";
        case ui2::UiWidgetType::Image: return "Tint";
        case ui2::UiWidgetType::Text: return "Text";
        case ui2::UiWidgetType::Button: return "Fill";
        case ui2::UiWidgetType::Progress: return "Fill";
        default: return "Fill";
    }
}

const char* slotBLabel(ui2::UiWidgetType t) {
    switch (t) {
        case ui2::UiWidgetType::Nine: return "Inner";
        case ui2::UiWidgetType::Button: return "Label";
        case ui2::UiWidgetType::Progress: return "Track";
        default: return "Second";
    }
}

} // namespace

UiEditorWindow::UiEditorWindow(EditorContext& ctx) : m_ctx(ctx) {}

// ============================================================
// 打开 / 重读
// ============================================================

bool UiEditorWindow::open(const std::string& relPath) {
    m_open = true;
    m_path = relPath;
    m_sel.clear();
    m_dirty = false;
    m_uiError.clear();
    m_error.clear();
    m_bufKey.clear();
    m_injected = false;
    m_loaded = true;
    m_dragMode = 0;
    m_dragId.clear();
    std::snprintf(m_pathBuf, sizeof(m_pathBuf), "%s", m_path.c_str());

    if (relPath.empty()) {
        // 空手开窗（View 菜单 / 托盘点开）：没有文档，只剩 "New"。
        m_doc = ui2::UiDocument{};
        m_readOnly = false;
        VK_LOG_INFO("UIED-OPEN path=(none) ok=0 widgets=0 readOnly=0");
        revalidate();
        return false;
    }

    ui2::UiDocument d;
    const bool ok = ui2::loadUiDocument(relPath, d);
    m_doc = std::move(d);
    // ② 读盘失败 → 只读（见 .h）
    m_readOnly = !ok;
    if (!ok) m_error = m_doc.error;

    VK_LOG_INFO("UIED-OPEN path=%s ok=%d widgets=%d readOnly=%d", m_path.c_str(),
                ok ? 1 : 0, ui2::countWidgets(m_doc.widgets), m_readOnly ? 1 : 0);
    if (!ok) VK_LOG_WARN("UIED-OPEN-ERR path=%s err=%s", m_path.c_str(),
                         m_error.c_str());

    // 默认选中第一个控件：空手打开一个文档却"什么都没选"时，右栏整片空白，
    // 作者第一反应是"没打开成功"。
    if (!m_doc.widgets.empty()) m_sel = m_doc.widgets.front().id;
    revalidate();
    return ok;
}

void UiEditorWindow::close() {
    m_open = false;
    m_dragMode = 0;
    m_dragId.clear();
}

void UiEditorWindow::reload() {
    if (m_path.empty()) {
        revalidate();
        return;
    }
    ui2::UiDocument d;
    const bool ok = ui2::loadUiDocument(m_path, d);
    m_doc = std::move(d);
    m_readOnly = !ok;
    m_error = ok ? std::string() : m_doc.error;
    m_dirty = false;
    m_dragMode = 0;
    m_bufKey.clear();
    if (!m_doc.widgets.empty()) {
        // 选中项尽量保留（重读之后还是同一个控件），丢了就退到第一个。
        if (ui2::findWidget(m_doc, m_sel) == nullptr)
            m_sel = m_doc.widgets.front().id;
    } else {
        m_sel.clear();
    }
    VK_LOG_INFO("UIED-RELOAD path=%s ok=%d widgets=%d readOnly=%d",
                m_path.c_str(), ok ? 1 : 0, ui2::countWidgets(m_doc.widgets),
                m_readOnly ? 1 : 0);
    revalidate();
}

// ============================================================
// 校验：**序列化后重新解析**（见 .h ①）
// ============================================================

void UiEditorWindow::revalidate() {
    const std::string text = ui2::toJson(m_doc);
    ui2::UiDocument tmp;
    const bool parsed = ui2::parseUiDocument(text, m_path, tmp);
    // ⚠ 只读（读盘失败）时**不许**宣称"检查通过"：此时 `m_doc` 是解析到一半
    //   留下的残片，把它序列化再解析当然能过 —— 而那条"通过"会让作者以为
    //   文件只是"有点小问题"。真正的错误在 `m_error` 里。
    const bool ok = parsed && !m_readOnly;
    m_checkValid = ok;
    m_checkError = ok ? std::string()
                      : (m_readOnly
                             ? (m_error.empty() ? std::string("the file did not load")
                                                : m_error)
                             : tmp.error);
    m_checkWarnings = tmp.warnings;
    // ⚠ `m_doc.valid` 只用于"几何能不能算"（`placeDocumentAt` 会检查它）。
    //   新建的文档还没落过盘，但它是合法的 —— 所以这里跟着解析结论走。
    m_doc.valid = parsed;

    char key[512];
    std::snprintf(key, sizeof(key), "%d|%d|%s|%d", ok ? 1 : 0,
                  ui2::countWidgets(m_doc.widgets), m_checkError.c_str(),
                  static_cast<int>(m_checkWarnings.size()));
    if (m_checkLogKey != key) {
        m_checkLogKey = key;
        VK_LOG_INFO("UIED-CHECK ok=%d widgets=%d warns=%d err=%s", ok ? 1 : 0,
                    ui2::countWidgets(m_doc.widgets),
                    static_cast<int>(m_checkWarnings.size()),
                    m_checkError.empty() ? "-" : m_checkError.c_str());
    }
}

void UiEditorWindow::markDirty() {
    m_dirty = true;
}

bool UiEditorWindow::reject(const char* op, const std::string& reason) {
    m_uiError = reason;
    VK_LOG_WARN("UIED-REJECT op=%s reason=%s", op, reason.c_str());
    return false;
}

// ============================================================
// 选中项
// ============================================================

ui2::UiWidget* UiEditorWindow::sel() {
    if (m_sel.empty()) return nullptr;
    return ui2::findWidget(m_doc, m_sel);
}

const ui2::UiWidget* UiEditorWindow::sel() const {
    if (m_sel.empty()) return nullptr;
    return ui2::findWidget(m_doc, m_sel);
}

std::string UiEditorWindow::parentPathOf(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    return (slash == std::string::npos) ? std::string() : path.substr(0, slash);
}

std::string UiEditorWindow::leafOf(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

// ============================================================
// 树 / 画布几何
// ============================================================

std::vector<ui2::UiDocPlaced> UiEditorWindow::designRects() const {
    ui2::UiRect canvas;
    canvas.w = static_cast<float>(m_doc.refW);
    canvas.h = static_cast<float>(m_doc.refH);
    // ⚠ 绑定表给空表：设计视图上 `{health}` 保持原样（见 .h ⑤）。
    //   真实数值是运行时的责任，编辑器里"预览假数据"会把作者骗得更惨。
    return ui2::placeDocumentAt(m_doc, ui2::UiBindings{}, canvas, 1.0f);
}

ui2::UiRect UiEditorWindow::parentRectOf(const std::string& path) const {
    ui2::UiRect canvas;
    canvas.w = static_cast<float>(m_doc.refW);
    canvas.h = static_cast<float>(m_doc.refH);
    const std::string pp = parentPathOf(path);
    if (pp.empty()) return canvas;
    const std::string leaf = leafOf(pp);
    for (const auto& r : designRects()) {
        if (r.id == leaf) return r.rect;
    }
    return canvas;
}

UiEditorWindow::Pt UiEditorWindow::anchorMarkOf(const std::string& path) const {
    Pt p;
    const ui2::UiWidget* w = ui2::findWidget(m_doc, path);
    if (!w) return p;
    const std::string leaf = leafOf(path);
    ui2::UiRect r;
    r.w = -1.0f;
    for (const auto& pl : designRects()) {
        if (pl.id == leaf) {
            r = pl.rect;
            break;
        }
    }
    if (r.w < 0.0f) return p;

    const int a = static_cast<int>(w->anchor);
    const bool left = (a % 3) == 0;
    const bool midX = (a % 3) == 1;
    const bool top = a < 3;
    const bool midY = a >= 3 && a < 6;
    p.x = left ? r.x : (midX ? r.cx() : r.right());
    p.y = top ? r.y : (midY ? r.cy() : r.bottom());
    return p;
}

void UiEditorWindow::syncBuffers() {
    const ui2::UiWidget* w = sel();
    const std::string key = w ? m_sel : std::string("<none>");
    if (key == m_bufKey) return;
    m_bufKey = key;
    if (!w) {
        m_idBuf[0] = m_textBuf[0] = m_bindBuf[0] = m_bindMaxBuf[0] = '\0';
        return;
    }
    std::snprintf(m_idBuf, sizeof(m_idBuf), "%s", w->id.c_str());
    std::snprintf(m_textBuf, sizeof(m_textBuf), "%s", w->text.c_str());
    std::snprintf(m_bindBuf, sizeof(m_bindBuf), "%s", w->bind.c_str());
    std::snprintf(m_bindMaxBuf, sizeof(m_bindMaxBuf), "%s", w->bindMax.c_str());
}

// ============================================================
// 动作（UI 与注入脚本共用）
// ============================================================

bool UiEditorWindow::idUsed(const std::string& id,
                            const std::string& exceptPath) const {
    bool used = false;
    std::function<void(const std::vector<ui2::UiWidget>&, const std::string&)> walk =
        [&](const std::vector<ui2::UiWidget>& ws, const std::string& prefix) {
            for (const auto& w : ws) {
                if (used) return;
                const std::string path = prefix.empty() ? w.id : prefix + "/" + w.id;
                if (w.id == id && path != exceptPath) {
                    used = true;
                    return;
                }
                if (!w.children.empty()) walk(w.children, path);
            }
        };
    walk(m_doc.widgets, std::string());
    return used;
}

std::string UiEditorWindow::uniqueId(const std::string& base) const {
    // 全表查重（跨层级）：id 是身份，不是"同级唯一"。
    if (!idUsed(base, std::string())) return base;
    for (int n = 2; n < 1000; ++n) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s%d", base.c_str(), n);
        if (!idUsed(buf, std::string())) return buf;
    }
    return base;
}

bool UiEditorWindow::actSelect(const std::string& path) {
    if (path.empty()) {
        m_sel.clear();
        m_bufKey.clear();
        VK_LOG_INFO("UIED-SEL path=(none)");
        return true;
    }
    if (ui2::findWidget(m_doc, path) == nullptr) return false;
    m_sel = path;
    m_bufKey.clear();
    m_uiError.clear();
    VK_LOG_INFO("UIED-SEL path=%s", m_sel.c_str());
    return true;
}

bool UiEditorWindow::actNew() {
    if (m_readOnly) return reject("new", "the file did not load (read-only)");
    ui2::UiDocument d;
    // 名字取文件名去掉 `.ui.json`（`std::filesystem` 会把 `.ui` 留在 stem 里）。
    std::string stem = "hud";
    if (!m_path.empty()) {
        const std::size_t slash = m_path.find_last_of("/\\");
        stem = (slash == std::string::npos) ? m_path : m_path.substr(slash + 1);
        const std::size_t dot = stem.rfind(".ui.json");
        if (dot != std::string::npos) stem = stem.substr(0, dot);
    }
    d.name = stem;
    d.refW = 1920;
    d.refH = 1080;
    d.valid = true;

    // 骨架：一块底部血条面板 + 两个子控件。这不是"随便给点东西"——
    // 它把三种最容易搞错的关系一次性摆出来：**面板带子控件**、**进度条绑
    // 血量**、**文本带 {key} 绑定**。作者照着改比从空白开始快得多。
    ui2::UiWidget hud;
    hud.id = "hud";
    hud.type = ui2::UiWidgetType::Nine;
    hud.anchor = ui2::UiAnchor::BottomLeft;
    hud.x = 24.0f;
    hud.y = 24.0f;
    hud.w = 340.0f;
    hud.h = 74.0f;
    hud.color = ui2::rgbA(0x0E1116, 230);
    hud.color2 = ui2::rgbA(0x1A1F27, 242);
    hud.radius = 8.0f;
    hud.inset = 2.0f;

    ui2::UiWidget bar;
    bar.id = "hpbar";
    bar.type = ui2::UiWidgetType::Progress;
    bar.x = 10.0f;
    bar.y = 10.0f;
    bar.w = 320.0f;
    bar.h = 18.0f;
    bar.color = ui2::rgbA(0xD93A3A, 255);
    bar.color2 = ui2::rgbA(0x000000, 128);
    bar.radius = 9.0f;
    bar.bind = "health";
    bar.bindMax = "maxHealth";
    hud.children.push_back(bar);

    ui2::UiWidget num;
    num.id = "hpnum";
    num.type = ui2::UiWidgetType::Text;
    num.x = 10.0f;
    num.y = 34.0f;
    num.w = 320.0f;
    num.h = 26.0f;
    num.text = "HP {health} / {maxHealth}";
    num.size = 18.0f;
    num.align = 2;
    hud.children.push_back(num);

    d.widgets.push_back(hud);
    m_doc = std::move(d);
    m_sel = "hud";
    m_readOnly = false;
    m_bufKey.clear();
    markDirty();
    revalidate();
    VK_LOG_INFO("UIED-NEW widgets=%d ref=%dx%d", ui2::countWidgets(m_doc.widgets),
                m_doc.refW, m_doc.refH);
    return true;
}

bool UiEditorWindow::actAdd(ui2::UiWidgetType type) {
    if (m_readOnly) return reject("add", "the file did not load (read-only)");
    ui2::UiWidget w;
    w.type = type;
    w.id = uniqueId(typeLabel(type));
    w.w = 240.0f;
    w.h = 64.0f;
    switch (type) {
        case ui2::UiWidgetType::Text:
            w.text = "Text";
            w.h = 32.0f;
            w.size = 20.0f;
            break;
        case ui2::UiWidgetType::Button:
            w.text = "Button";
            w.color = ui2::rgbA(0x2A2E36, 242);
            w.color2 = ui2::rgbA(0xFFFFFF, 255);
            w.radius = 8.0f;
            break;
        case ui2::UiWidgetType::Progress:
            w.h = 18.0f;
            w.color = ui2::rgbA(0x7F5AF0, 255);
            w.color2 = ui2::rgbA(0x000000, 128);
            w.radius = 9.0f;
            w.value = 1.0f;
            break;
        case ui2::UiWidgetType::Nine:
            w.color = ui2::rgbA(0x101319, 255);
            w.color2 = ui2::rgbA(0x1B2028, 255);
            w.radius = 8.0f;
            w.inset = 2.0f;
            break;
        case ui2::UiWidgetType::Image:
            w.w = 96.0f;
            w.h = 96.0f;
            break;
        default:
            w.color = ui2::rgbA(0x101319, 230);
            break;
    }

    // 插到选中项**之后**（同级）。选中项在别人家孩子里时，新控件就是那个
    // 父节点的新兄弟 —— 而不是"跑到顶层去"，那会让作者以为自己点错了面板。
    const std::string parent = parentPathOf(m_sel);
    int index = -1;
    if (!m_sel.empty()) {
        std::vector<ui2::UiWidget>* list = &m_doc.widgets;
        if (!parent.empty()) {
            ui2::UiWidget* p = ui2::findWidget(m_doc, parent);
            if (p) list = &p->children;
        }
        for (std::size_t i = 0; i < list->size(); ++i) {
            if ((*list)[i].id == leafOf(m_sel)) {
                index = static_cast<int>(i) + 1;
                break;
            }
        }
    }
    if (!ui2::attachWidget(m_doc, parent, w, index))
        return reject("add", "cannot attach to that parent");

    m_sel = parent.empty() ? w.id : parent + "/" + w.id;
    m_bufKey.clear();
    markDirty();
    revalidate();
    VK_LOG_INFO("UIED-ADD type=%s id=%s parent=%s total=%d",
                typeLabel(type), w.id.c_str(),
                parent.empty() ? "(root)" : parent.c_str(),
                ui2::countWidgets(m_doc.widgets));
    return true;
}

bool UiEditorWindow::actDuplicate() {
    if (m_readOnly) return reject("dup", "the file did not load (read-only)");
    const ui2::UiWidget* w = sel();
    if (!w) return reject("dup", "nothing is selected");
    ui2::UiWidget copy = *w;                 // 连子树一起复制
    copy.id = uniqueId(w->id);
    copy.x += 16.0f;
    copy.y += 16.0f;
    // 子控件的 id 也必须换（跨层唯一），路径跟着新父走。
    std::vector<ui2::UiWidget*> stack;
    for (auto& c : copy.children) stack.push_back(&c);
    while (!stack.empty()) {
        ui2::UiWidget* c = stack.back();
        stack.pop_back();
        c->id = uniqueId(c->id);
        for (auto& g : c->children) stack.push_back(&g);
    }

    // ⚠ 先把 id 抄出来：`attachWidget` 会往 vector 里插元素，`sel()` 那个
    //   指针可能立刻悬垂（这正是"日志里打出来的 id 是乱码"的来源）。
    const std::string srcId = w->id;
    const std::string parent = parentPathOf(m_sel);
    int index = -1;
    std::vector<ui2::UiWidget>* list = &m_doc.widgets;
    if (!parent.empty()) {
        ui2::UiWidget* p = ui2::findWidget(m_doc, parent);
        if (p) list = &p->children;
    }
    for (std::size_t i = 0; i < list->size(); ++i) {
        if ((*list)[i].id == leafOf(m_sel)) {
            index = static_cast<int>(i) + 1;
            break;
        }
    }
    if (!ui2::attachWidget(m_doc, parent, copy, index))
        return reject("dup", "cannot attach the copy");
    m_sel = parent.empty() ? copy.id : parent + "/" + copy.id;
    m_bufKey.clear();
    markDirty();
    revalidate();
    VK_LOG_INFO("UIED-DUP from=%s to=%s total=%d", srcId.c_str(),
                copy.id.c_str(), ui2::countWidgets(m_doc.widgets));
    return true;
}

bool UiEditorWindow::actDelete() {
    if (m_readOnly) return reject("del", "the file did not load (read-only)");
    if (m_sel.empty()) return reject("del", "nothing is selected");
    const std::string parent = parentPathOf(m_sel);
    const int before = ui2::countWidgets(m_doc.widgets);
    ui2::UiWidget removed;
    if (!ui2::detachWidget(m_doc, m_sel, &removed))
        return reject("del", "cannot detach " + m_sel);
    m_sel = parent;
    m_bufKey.clear();
    markDirty();
    revalidate();
    VK_LOG_INFO("UIED-DEL id=%s removed=%d total=%d", removed.id.c_str(),
                before - ui2::countWidgets(m_doc.widgets),
                ui2::countWidgets(m_doc.widgets));
    return true;
}

bool UiEditorWindow::actMove(int dir) {
    if (m_readOnly) return reject("move", "the file did not load (read-only)");
    if (m_sel.empty()) return reject("move", "nothing is selected");
    const std::string parent = parentPathOf(m_sel);
    std::vector<ui2::UiWidget>* list = &m_doc.widgets;
    if (!parent.empty()) {
        ui2::UiWidget* p = ui2::findWidget(m_doc, parent);
        if (!p) return reject("move", "parent not found");
        list = &p->children;
    }
    int from = -1;
    for (std::size_t i = 0; i < list->size(); ++i) {
        if ((*list)[i].id == leafOf(m_sel)) {
            from = static_cast<int>(i);
            break;
        }
    }
    if (from < 0) return reject("move", "selected widget is not in this list");
    const int to = from + dir;
    if (to < 0 || to >= static_cast<int>(list->size())) return false;
    std::swap((*list)[static_cast<std::size_t>(from)],
              (*list)[static_cast<std::size_t>(to)]);
    markDirty();
    revalidate();
    VK_LOG_INFO("UIED-MOVE id=%s from=%d to=%d", m_sel.c_str(), from, to);
    return true;
}

bool UiEditorWindow::actSetId(const std::string& id) {
    if (m_readOnly) return reject("setid", "the file did not load (read-only)");
    ui2::UiWidget* w = sel();
    if (!w) return reject("setid", "nothing is selected");
    if (id == w->id) return true;
    if (id.empty()) return reject("setid", "the id cannot be empty");
    for (char ch : id) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' ||
                        ch == '.';
        if (!ok) return reject("setid", "id may only use letters, digits, '_', "
                                        "'-', '.' (no spaces, no '/')");
    }
    if (idUsed(id, m_sel))
        return reject("setid", "id '" + id + "' is already used");

    const std::string parent = parentPathOf(m_sel);
    w->id = id;
    m_sel = parent.empty() ? id : parent + "/" + id;
    m_bufKey.clear();
    markDirty();
    revalidate();
    VK_LOG_INFO("UIED-SETID path=%s", m_sel.c_str());
    return true;
}

bool UiEditorWindow::actSetPos(float x, float y) {
    if (m_readOnly) return reject("setpos", "the file did not load (read-only)");
    ui2::UiWidget* w = sel();
    if (!w) return reject("setpos", "nothing is selected");
    w->x = std::round(x / kSnap) * kSnap;
    w->y = std::round(y / kSnap) * kSnap;
    markDirty();
    revalidate();
    return true;
}

bool UiEditorWindow::actSetSize(float w, float h) {
    if (m_readOnly) return reject("setsize", "the file did not load (read-only)");
    ui2::UiWidget* it = sel();
    if (!it) return reject("setsize", "nothing is selected");
    // 0 尺寸的控件画不出来、也点不中 —— 那是"控件消失了"，不是"很小的控件"。
    if (!(w > 0.0f) || !(h > 0.0f))
        return reject("setsize", "width and height must be greater than 0");
    it->w = std::round(w / kSnap) * kSnap;
    it->h = std::round(h / kSnap) * kSnap;
    markDirty();
    revalidate();
    return true;
}

bool UiEditorWindow::actSetAnchorKeepRect(ui2::UiAnchor a) {
    if (m_readOnly) return reject("anchor", "the file did not load (read-only)");
    ui2::UiWidget* w = sel();
    if (!w) return reject("anchor", "nothing is selected");
    if (w->anchor == a) return true;

    // 当前矩形（参考像素）与父矩形
    const std::string leaf = leafOf(m_sel);
    ui2::UiRect r;
    r.w = -1.0f;
    for (const auto& pl : designRects()) {
        if (pl.id == leaf) {
            r = pl.rect;
            break;
        }
    }
    if (r.w < 0.0f) return reject("anchor", "the widget has no rect");

    const ui2::UiRect parent = parentRectOf(m_sel);
    const ui2::UiAnchor old = w->anchor;
    w->anchor = a;

    // 由"支点贴锚点"反解 x/y（与 `placeRectIn` 是同一套公式的逆运算）：
    //   左锚 x = r.x - parent.x               右锚 x = parent.right - r.right
    //   中锚 x = r.cx - parent.cx
    const int ai = static_cast<int>(a);
    const bool left = (ai % 3) == 0;
    const bool midX = (ai % 3) == 1;
    const bool top = ai < 3;
    const bool midY = ai >= 3 && ai < 6;
    if (left)       w->x = r.x - parent.x;
    else if (midX)  w->x = r.cx() - parent.cx();
    else            w->x = parent.right() - r.right();
    if (top)        w->y = r.y - parent.y;
    else if (midY)  w->y = r.cy() - parent.cy();
    else            w->y = parent.bottom() - r.bottom();

    m_bufKey.clear();
    markDirty();
    revalidate();
    VK_LOG_INFO("UIED-ANCHOR id=%s from=%s to=%s x=%.1f y=%.1f keep=1",
                w->id.c_str(), ui2::anchorName(old), ui2::anchorName(a),
                static_cast<double>(w->x), static_cast<double>(w->y));
    return true;
}

bool UiEditorWindow::actSetColor(uint32_t c) {
    if (m_readOnly) return reject("color", "the file did not load (read-only)");
    ui2::UiWidget* w = sel();
    if (!w) return reject("color", "nothing is selected");
    w->color = c;
    markDirty();
    revalidate();
    VK_LOG_INFO("UIED-COLOR id=%s color=#%08X", w->id.c_str(), c);
    return true;
}

bool UiEditorWindow::actSetColor2(uint32_t c) {
    if (m_readOnly) return reject("color2", "the file did not load (read-only)");
    ui2::UiWidget* w = sel();
    if (!w) return reject("color2", "nothing is selected");
    w->color2 = c;
    markDirty();
    revalidate();
    return true;
}

bool UiEditorWindow::actSetRadius(float r) {
    if (m_readOnly) return reject("radius", "the file did not load (read-only)");
    ui2::UiWidget* w = sel();
    if (!w) return reject("radius", "nothing is selected");
    w->radius = r < 0.0f ? 0.0f : r;
    markDirty();
    revalidate();
    return true;
}

bool UiEditorWindow::actSetInset(float v) {
    if (m_readOnly) return reject("inset", "the file did not load (read-only)");
    ui2::UiWidget* w = sel();
    if (!w) return reject("inset", "nothing is selected");
    w->inset = v < 0.0f ? 0.0f : v;
    markDirty();
    revalidate();
    return true;
}

bool UiEditorWindow::actSetText(const std::string& t) {
    if (m_readOnly) return reject("text", "the file did not load (read-only)");
    ui2::UiWidget* w = sel();
    if (!w) return reject("text", "nothing is selected");
    w->text = t;
    markDirty();
    revalidate();
    return true;
}

bool UiEditorWindow::actSetFontSize(float s) {
    if (m_readOnly) return reject("font", "the file did not load (read-only)");
    ui2::UiWidget* w = sel();
    if (!w) return reject("font", "nothing is selected");
    if (!(s > 0.0f)) return reject("font", "font size must be > 0");
    w->size = s;
    markDirty();
    revalidate();
    return true;
}

bool UiEditorWindow::actSetAlign(int a) {
    if (m_readOnly) return reject("align", "the file did not load (read-only)");
    ui2::UiWidget* w = sel();
    if (!w) return reject("align", "nothing is selected");
    w->align = (a < 0 || a > 2) ? 0 : a;
    markDirty();
    revalidate();
    return true;
}

bool UiEditorWindow::actSetBind(const std::string& key) {
    if (m_readOnly) return reject("bind", "the file did not load (read-only)");
    ui2::UiWidget* w = sel();
    if (!w) return reject("bind", "nothing is selected");
    w->bind = key;
    markDirty();
    revalidate();
    VK_LOG_INFO("UIED-BIND id=%s key=%s", w->id.c_str(),
                key.empty() ? "(none)" : key.c_str());
    return true;
}

bool UiEditorWindow::actSetBindMax(const std::string& key) {
    if (m_readOnly) return reject("bindmax", "the file did not load (read-only)");
    ui2::UiWidget* w = sel();
    if (!w) return reject("bindmax", "nothing is selected");
    w->bindMax = key;
    markDirty();
    revalidate();
    return true;
}

bool UiEditorWindow::actSetValue(float v) {
    if (m_readOnly) return reject("value", "the file did not load (read-only)");
    ui2::UiWidget* w = sel();
    if (!w) return reject("value", "nothing is selected");
    w->value = v;
    markDirty();
    revalidate();
    return true;
}

bool UiEditorWindow::actSetVisible(bool v) {
    if (m_readOnly) return reject("visible", "the file did not load (read-only)");
    ui2::UiWidget* w = sel();
    if (!w) return reject("visible", "nothing is selected");
    w->visible = v;
    markDirty();
    revalidate();
    return true;
}

bool UiEditorWindow::actDragTo(float x, float y) {
    if (m_readOnly) return false;
    ui2::UiWidget* w = sel();
    if (!w) return false;
    const float nx = std::round(x / kSnap) * kSnap;
    const float ny = std::round(y / kSnap) * kSnap;
    if (std::fabs(nx - w->x) < 0.01f && std::fabs(ny - w->y) < 0.01f) return false;
    w->x = nx;
    w->y = ny;
    markDirty();
    revalidate();
    VK_LOG_INFO("UIED-DRAG id=%s x=%.1f y=%.1f", w->id.c_str(),
                static_cast<double>(w->x), static_cast<double>(w->y));
    return true;
}

// ============================================================
// 存盘
// ============================================================

bool UiEditorWindow::save(bool force) {
    if (m_readOnly) {
        VK_LOG_WARN("UIED-SAVE-BLOCKED path=%s reason=read-only",
                    m_path.c_str());
        return reject("save", "the file did not load — saving is disabled");
    }
    if (m_path.empty()) {
        VK_LOG_WARN("UIED-SAVE-BLOCKED path=(none) reason=no-path");
        return reject("save", "set an asset path first");
    }
    if (!m_checkValid && !force) {
        VK_LOG_WARN("UIED-SAVE-BLOCKED path=%s reason=invalid err=%s",
                    m_path.c_str(), m_checkError.c_str());
        return reject("save", "the document does not load — see the check bar");
    }
    std::string err;
    if (!ui2::saveUiDocument(m_path, m_doc, err))
        return reject("save", err);
    m_dirty = false;
    m_error.clear();
    m_uiError.clear();
    return true;
}

// ============================================================
// 绘制
// ============================================================

void UiEditorWindow::draw(bool& open) {
    if (!m_open) return;

    ImGuiWindowFlags wf = 0;
    const bool docked = applyDockHint(wf);
    if (!docked) {
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowSizeConstraints(ImVec2(820.0f, 480.0f),
                                            ImVec2(FLT_MAX, FLT_MAX));
        // ⚠ 夹到屏幕内：1180 是"三栏都舒服"的宽度，但 1092 宽的窗口上
        //   右栏会被裁掉一半（作者看到的是"颜色名字只剩 Bord"）。
        //   位置也要跟着夹 —— 只夹尺寸不夹位置的话，窗口右边缘照样出界。
        const float w = std::min(1180.0f, io.DisplaySize.x * 0.94f);
        const float h = std::min(660.0f, io.DisplaySize.y * 0.88f);
        ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_FirstUseEver);
        const float px = std::max(4.0f, std::min(io.DisplaySize.x * 0.10f,
                                                 io.DisplaySize.x - w - 8.0f));
        const float py = std::max(4.0f, std::min(io.DisplaySize.y * 0.08f,
                                                 io.DisplaySize.y - h - 8.0f));
        ImGui::SetNextWindowPos(ImVec2(px, py), ImGuiCond_FirstUseEver);
    }

    // `###panel_uied` 是稳定 ID（标题随路径变化时窗口位置/大小记忆不能丢）。
    std::string title = TR("UI editor");
    if (!m_path.empty()) {
        title += " — ";
        title += m_doc.name.empty() ? m_path : m_doc.name;
        if (m_dirty) title += " *";
    }
    title += "###panel_uied";

    if (!ImGui::Begin(title.c_str(), docked ? nullptr : &open, wf)) {
        ImGui::End();
        return;
    }
    logRect("UIED-WIN", ImGui::GetWindowPos(),
            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                   ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    // 注入脚本排在最前（同另两个编辑器）：脚本改出来的状态同一帧就能被画出来。
    runInjectedScript();

    drawToolbar(open);
    ImGui::Separator();

    const float avail = ImGui::GetContentRegionAvail().y;
    // 底部校验栏固定高度，剩下的三分：树 / 画布 / 属性。
    const float bodyH = std::max(180.0f, avail - 78.0f);
    drawTreePanel(bodyH);   // 内部用 BeginChild 指定宽度
    ImGui::SameLine();
    drawCanvas(bodyH);
    ImGui::SameLine();
    drawInspector(bodyH);
    drawValidation();

    ImGui::End();

    if (!open) close();
}

void UiEditorWindow::drawToolbar(bool& open) {
    (void)open;

    ImGui::TextDisabled("%s", TR("Asset path"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(360.0f);
    if (ImGui::InputTextWithHint("##uied_path", "ui/docs/hud_demo.ui.json",
                                 m_pathBuf, sizeof(m_pathBuf),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
        m_path = m_pathBuf;
        reload();
    }
    logRect("UIED-PATH", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

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
    logRect("UIED-SAVE-BTN", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::EndDisabled();

    if (!m_checkValid && !m_readOnly && !m_path.empty()) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.28f, 0.18f, 1.0f));
        if (ImGui::Button(TR("Save anyway"))) save(true);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", TR("Write even though the document does not "
                                       "load. Use it for work in progress."));
    }

    ImGui::SameLine();
    if (ImGui::Button(TR("New"))) actNew();
    logRect("UIED-NEW-BTN", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    ImGui::SameLine();
    ImGui::TextDisabled(TR("Canvas %dx%d"), m_doc.refW, m_doc.refH);

    if (m_readOnly) {
        ImGui::TextColored(kWarnCol, "%s",
                           TR("Read-only — the file did not load, saving is disabled."));
        if (!m_error.empty()) ImGui::TextDisabled("  %s", m_error.c_str());
    }
    if (!m_uiError.empty()) ImGui::TextColored(kBadCol, "%s", m_uiError.c_str());
}

void UiEditorWindow::drawTreePanel(float height) {
    ImGui::BeginChild("##uied_tree", ImVec2(230.0f, height),
                      ImGuiChildFlags_Borders);
    logRect("UIED-TREE", ImGui::GetWindowPos(),
            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                   ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    ImGui::Text(TR("Widgets (%d)"), ui2::countWidgets(m_doc.widgets));
    logRect("UIED-TREE-HDR", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    // 两个按钮一行，摆两行 —— 一行六个在 230px 里必然被裁掉一半。
    const struct {
        const char* label;
        ui2::UiWidgetType type;
        const char* anchor;
    } kAdd[] = {
        {"+ Panel", ui2::UiWidgetType::Panel, "UIED-ADD-PANEL"},
        {"+ Text", ui2::UiWidgetType::Text, "UIED-ADD-TEXT"},
        {"+ Button", ui2::UiWidgetType::Button, "UIED-ADD-BUTTON"},
        {"+ Progress", ui2::UiWidgetType::Progress, "UIED-ADD-PROGRESS"},
        {"+ Nine", ui2::UiWidgetType::Nine, "UIED-ADD-NINE"},
        {"+ Image", ui2::UiWidgetType::Image, "UIED-ADD-IMAGE"},
    };
    for (int i = 0; i < 6; ++i) {
        if (i % 2) ImGui::SameLine();
        if (ImGui::Button(TR(kAdd[i].label))) actAdd(kAdd[i].type);
        logRect(kAdd[i].anchor, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    }

    // ⚠ 四个按钮**分两行**：230px 的栏宽装不下 Duplicate+Delete+Up+Down
    //   （实测 Down 的右边缘到 451，超出子窗口的 400），而"按钮被裁掉一半"
    //   是最难看的一种布局问题 —— 时间轴编辑器也踩过同一次。
    ImGui::BeginDisabled(m_sel.empty() || m_readOnly);
    if (ImGui::Button(TR("Duplicate"))) actDuplicate();
    logRect("UIED-DUP-BTN", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::Button(TR("Delete"))) actDelete();
    logRect("UIED-DEL-BTN", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::Button(TR("Up"))) actMove(-1);
    ImGui::SameLine();
    if (ImGui::Button(TR("Down"))) actMove(1);
    ImGui::EndDisabled();
    logRect("UIED-TREE-ACT", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    ImGui::Separator();
    for (auto& w : m_doc.widgets) drawTreeNode(w.id, w);

    ImGui::EndChild();
}

void UiEditorWindow::drawTreeNode(const std::string& path, ui2::UiWidget& w) {
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                               ImGuiTreeNodeFlags_SpanAvailWidth |
                               ImGuiTreeNodeFlags_DefaultOpen;
    if (path == m_sel) flags |= ImGuiTreeNodeFlags_Selected;
    if (w.children.empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }
    // 稳定 ID 走 `###`（路径唯一），显示部分是"类型 + id"。
    const std::string label = std::string(typeLabel(w.type)) + "  " + w.id +
                              "###uiednode_" + path;
    if (!w.visible) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
    const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
    if (!w.visible) ImGui::PopStyleColor();
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) actSelect(path);

    if (open && !w.children.empty()) {
        for (auto& c : w.children) drawTreeNode(path + "/" + c.id, c);
        ImGui::TreePop();
    }
}

void UiEditorWindow::drawCanvas(float height) {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    // 右栏固定 330，两次 SameLine 各占一个 ItemSpacing。
    const float sp = ImGui::GetStyle().ItemSpacing.x;
    const float w = std::max(240.0f, avail.x - 330.0f - sp * 2.0f);
    ImGui::BeginChild("##uied_canvas", ImVec2(w, height), ImGuiChildFlags_Borders);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 area(w - 8.0f, height - 8.0f);

    // 16:9（或文档自己的参考比）等比塞进去。**不拉伸** —— 作者摆的是
    // 真实比例，拉伸的画布会让"看起来居中"的控件在运行时偏掉。
    const float refAspect = (m_doc.refH > 0)
                                ? static_cast<float>(m_doc.refW) /
                                      static_cast<float>(m_doc.refH)
                                : 16.0f / 9.0f;
    float cw = area.x - kCanvasPad * 2.0f;
    float ch = cw / refAspect;
    if (ch > area.y - kCanvasPad * 2.0f) {
        ch = area.y - kCanvasPad * 2.0f;
        cw = ch * refAspect;
    }
    if (cw < 16.0f || ch < 16.0f) {
        ImGui::TextDisabled("%s", TR("Not enough room for the design canvas."));
        ImGui::EndChild();
        m_canvas = CanvasGeom{};
        return;
    }

    m_canvas.x0 = origin.x + kCanvasPad;
    m_canvas.y0 = origin.y + kCanvasPad;
    m_canvas.x1 = m_canvas.x0 + cw;
    m_canvas.y1 = m_canvas.y0 + ch;
    m_canvas.scale = cw / static_cast<float>(m_doc.refW);

    // 画布底 + 边框 + 三分线（对齐参考线，不是装饰：二次元 UI 的排版
    // 大多落在三分/中心线上）。
    // ⚠ 底色用**中灰**而不是近黑：二次元 UI 的面板底多半是深色（#0E1116 那
    //   一类），近黑画布上它们整块看不见 —— 而"控件没画上"和"画了但和背景
    //   一个颜色"在截图里长得一模一样。
    dl->AddRectFilled(ImVec2(m_canvas.x0, m_canvas.y0),
                      ImVec2(m_canvas.x1, m_canvas.y1), IM_COL32(70, 76, 88, 255));
    dl->AddRect(ImVec2(m_canvas.x0, m_canvas.y0),
                ImVec2(m_canvas.x1, m_canvas.y1), IM_COL32(70, 76, 88, 255));
    for (int i = 1; i < 3; ++i) {
        const float f = static_cast<float>(i) / 3.0f;
        dl->AddLine(ImVec2(m_canvas.x0 + cw * f, m_canvas.y0),
                    ImVec2(m_canvas.x0 + cw * f, m_canvas.y1),
                    IM_COL32(50, 54, 62, 255));
        dl->AddLine(ImVec2(m_canvas.x0, m_canvas.y0 + ch * f),
                    ImVec2(m_canvas.x1, m_canvas.y0 + ch * f),
                    IM_COL32(50, 54, 62, 255));
    }

    // ---- 交互区（覆盖整个画布）----
    ImGui::SetCursorScreenPos(ImVec2(m_canvas.x0, m_canvas.y0));
    ImGui::InvisibleButton("##uied_canvas_hit", ImVec2(cw, ch),
                           ImGuiButtonFlags_MouseButtonLeft);
    logRect("UIED-CANVAS", ImVec2(m_canvas.x0, m_canvas.y0),
            ImVec2(m_canvas.x1, m_canvas.y1));

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const auto toRef = [&](const ImVec2& p) {
        return Pt{(p.x - m_canvas.x0) / m_canvas.scale,
                  (p.y - m_canvas.y0) / m_canvas.scale};
    };

    const std::vector<ui2::UiDocPlaced> rects = designRects();

    if (ImGui::IsItemActivated()) {
        // ① 先看锚点标记（它常常压在控件边缘上，命中优先级要高于控件本身）
        m_dragMode = 0;
        m_dragId.clear();
        if (!m_sel.empty()) {
            const Pt a = anchorMarkOf(m_sel);
            const ImVec2 ap(m_canvas.x0 + a.x * m_canvas.scale,
                            m_canvas.y0 + a.y * m_canvas.scale);
            const float dx = mouse.x - ap.x;
            const float dy = mouse.y - ap.y;
            if (std::sqrt(dx * dx + dy * dy) <= kAnchorHit) {
                m_dragMode = 2;
                m_dragId = m_sel;
            }
        }
        // ② 否则从头到尾找最上层的控件（**倒序** = 后画的在上面）
        if (m_dragMode == 0) {
            const Pt r = toRef(mouse);
            for (auto it = rects.rbegin(); it != rects.rend(); ++it) {
                if (!it->rect.contains(r.x, r.y)) continue;
                // 命中的 id 要变成**路径**：同名不可能（id 唯一），所以从
                // 选中路径反推父路径即可 —— 但如果命中的是别人的子控件，
                // 得先把它找出来（走一次全树查路径）。
                std::string path;
                std::vector<std::pair<std::string, const ui2::UiWidget*>> stack;
                for (const auto& top : m_doc.widgets) stack.push_back({top.id, &top});
                while (!stack.empty()) {
                    auto cur = stack.back();
                    stack.pop_back();
                    if (cur.second->id == it->id) {
                        path = cur.first;
                        break;
                    }
                    for (const auto& c : cur.second->children)
                        stack.push_back({cur.first + "/" + c.id, &c});
                }
                if (!path.empty()) {
                    actSelect(path);
                    m_dragMode = 1;
                    m_dragId = path;
                }
                break;
            }
        }
        if (m_dragMode == 0) actSelect(std::string());
    }

    if (ImGui::IsItemActive() && m_dragMode == 1) {
        // 拖动：改的是**参考像素**的 x/y（与属性面板的 X/Y 同一口径）。
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        if (ui2::UiWidget* it = sel()) {
            actDragTo(it->x + d.x / m_canvas.scale, it->y + d.y / m_canvas.scale);
        }
    } else if (ImGui::IsItemActive() && m_dragMode == 2) {
        // 拖锚点：找**控件自身矩形**上最近的九个角点之一，换锚点并保持矩形。
        const ui2::UiWidget* w = ui2::findWidget(m_doc, m_dragId);
        if (w) {
            const std::string leaf = leafOf(m_dragId);
            ui2::UiRect r;
            r.w = -1.0f;
            for (const auto& pl : rects) {
                if (pl.id == leaf) {
                    r = pl.rect;
                    break;
                }
            }
            if (r.w >= 0.0f) {
                const Pt m = toRef(mouse);
                int best = static_cast<int>(w->anchor);
                float bestD = 1e30f;
                for (int i = 0; i < 9; ++i) {
                    const bool left = (i % 3) == 0;
                    const bool midX = (i % 3) == 1;
                    const bool top = i < 3;
                    const bool midY = i >= 3 && i < 6;
                    const float px = left ? r.x : (midX ? r.cx() : r.right());
                    const float py = top ? r.y : (midY ? r.cy() : r.bottom());
                    const float dx = m.x - px;
                    const float dy = m.y - py;
                    const float dd = dx * dx + dy * dy;
                    if (dd < bestD) {
                        bestD = dd;
                        best = i;
                    }
                }
                if (best != static_cast<int>(w->anchor))
                    actSetAnchorKeepRect(static_cast<ui2::UiAnchor>(best));
            }
        }
    }
    if (ImGui::IsItemDeactivated() && m_dragMode != 0) {
        VK_LOG_INFO("UIED-DRAG-END id=%s mode=%d", m_dragId.c_str(), m_dragMode);
        m_dragMode = 0;
        m_dragId.clear();
    }

    // ---- 画控件（`designRects` 是 DFS 顺序，与树的遍历一致）----
    std::size_t cursor = 0;
    std::function<void(const std::vector<ui2::UiWidget>&, const std::string&)> paint =
        [&](const std::vector<ui2::UiWidget>& ws, const std::string& prefix) {
            for (const auto& w : ws) {
                const std::string path = prefix.empty() ? w.id : prefix + "/" + w.id;
                if (!w.visible) continue;
                if (cursor >= rects.size()) return;
                const ui2::UiRect r = rects[cursor].rect;
                const ui2::UiRect fill = rects[cursor].fill;
                ++cursor;

                const ImVec2 a(m_canvas.x0 + r.x * m_canvas.scale,
                               m_canvas.y0 + r.y * m_canvas.scale);
                const ImVec2 b(m_canvas.x0 + r.right() * m_canvas.scale,
                               m_canvas.y0 + r.bottom() * m_canvas.scale);
                const bool selected = (path == m_sel);
                const ImU32 fillCol = imCol(w.color, selected ? 235 : 180);

                dl->AddRectFilled(a, b, fillCol,
                                  w.radius * m_canvas.scale);
                if (w.type == ui2::UiWidgetType::Progress && fill.w > 0.5f) {
                    const ImVec2 fa(m_canvas.x0 + fill.x * m_canvas.scale,
                                    m_canvas.y0 + fill.y * m_canvas.scale);
                    const ImVec2 fb(m_canvas.x0 + (fill.x + fill.w) * m_canvas.scale,
                                    m_canvas.y0 + (fill.y + fill.h) * m_canvas.scale);
                    dl->AddRectFilled(fa, fb, imCol(w.color, 255),
                                      w.radius * m_canvas.scale);
                }
                dl->AddRect(a, b,
                            selected ? IM_COL32(255, 214, 92, 255)
                                     : IM_COL32(120, 128, 142, 180));

                // 文字：设计视图里用 ImGui 的字（与运行时字体不同，这没
                // 关系 —— 这里看的是"位置与对齐"，字形的最终效果在 Play 里看）。
                if (!w.text.empty() &&
                    (w.type == ui2::UiWidgetType::Text ||
                     w.type == ui2::UiWidgetType::Button)) {
                    const float fs = std::max(9.0f, w.size * m_canvas.scale);
                    const float base = std::max(1.0f, ImGui::GetFontSize());
                    const float k = fs / base;
                    const ImVec2 ts = ImGui::CalcTextSize(w.text.c_str());
                    const float tw = ts.x * k;
                    const float th = ts.y * k;
                    float tx = a.x + 2.0f;
                    if (w.align == 1) tx = a.x + (b.x - a.x - tw) * 0.5f;
                    else if (w.align == 2) tx = b.x - tw - 2.0f;
                    const float ty = a.y + (b.y - a.y) * 0.5f - th * 0.5f;
                    dl->AddText(ImGui::GetFont(), fs, ImVec2(tx, ty),
                                imCol(w.color2, 235), w.text.c_str());
                }

                if (selected) {
                    char tag[160];
                    std::snprintf(tag, sizeof(tag), "%s  %s  %.0f,%.0f %.0fx%.0f",
                                  typeLabel(w.type), w.id.c_str(),
                                  static_cast<double>(w.x),
                                  static_cast<double>(w.y),
                                  static_cast<double>(w.w),
                                  static_cast<double>(w.h));
                    dl->AddText(ImVec2(a.x + 2.0f, a.y - 15.0f),
                                IM_COL32(255, 214, 92, 255), tag);
                }

                if (!w.children.empty()) paint(w.children, path);
            }
        };
    paint(m_doc.widgets, std::string());

    // ---- 选中项的锚点标记 ----
    if (!m_sel.empty()) {
        const Pt a = anchorMarkOf(m_sel);
        const ImVec2 p(m_canvas.x0 + a.x * m_canvas.scale,
                       m_canvas.y0 + a.y * m_canvas.scale);
        dl->AddCircle(p, kAnchorHit * 0.7f, IM_COL32(255, 214, 92, 255), 16, 1.5f);
        dl->AddLine(ImVec2(p.x - 6.0f, p.y), ImVec2(p.x + 6.0f, p.y),
                    IM_COL32(255, 214, 92, 255));
        dl->AddLine(ImVec2(p.x, p.y - 6.0f), ImVec2(p.x, p.y + 6.0f),
                    IM_COL32(255, 214, 92, 255));
    }

    if (m_doc.widgets.empty()) {
        dl->AddText(ImVec2(m_canvas.x0 + 12.0f, m_canvas.y0 + 12.0f),
                    IM_COL32(150, 156, 168, 255),
                    TR("Nothing here yet — press New or + Panel."));
    }
    ImGui::EndChild();
}

void UiEditorWindow::drawInspector(float height) {
    ImGui::BeginChild("##uied_insp", ImVec2(330.0f, height),
                      ImGuiChildFlags_Borders);
    logRect("UIED-INSPECTOR", ImGui::GetWindowPos(),
            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                   ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));
    syncBuffers();

    ui2::UiWidget* w = sel();
    if (!w) {
        ImGui::TextDisabled("%s", TR("Select a widget on the left."));
        ImGui::EndChild();
        return;
    }

    ImGui::Text(TR("Type: %s"), typeLabel(w->type));

    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::InputText(TR("id"), m_idBuf, sizeof(m_idBuf),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        actSetId(m_idBuf);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", TR("Letters, digits, '_', '-', '.'. No spaces "
                                   "or '/' — the id goes into log anchors."));
    logRect("UIED-ID", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    // ---- 锚点九宫格 ----
    ImGui::TextDisabled("%s", TR("Anchor"));
    ImGui::BeginGroup();
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            const int a = row * 3 + col;
            if (col) ImGui::SameLine();
            const bool on = static_cast<int>(w->anchor) == a;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button,
                                          ImVec4(0.95f, 0.78f, 0.25f, 1.0f));
            // ⚠ 稳定 ID 用 `##` 拼在**字符串尾**，不要塞进 snprintf 的格式串
            //   （`###`/`##` 是 ImGui 的稳定 ID，i18n 的 E 组会把它当成
            //   "没包 TR 的格式串"报出来 —— 时间轴编辑器踩过同一次）。
            const std::string lbl = std::string(on ? "*" : ".") + "##uied_a" +
                                    std::to_string(a);
            if (ImGui::Button(lbl.c_str(), ImVec2(26.0f, 22.0f)))
                actSetAnchorKeepRect(static_cast<ui2::UiAnchor>(a));
            if (a == 4) logRect("UIED-ANCHOR-GRID", ImGui::GetItemRectMin(),
                                ImGui::GetItemRectMax());
            if (on) ImGui::PopStyleColor();
        }
    }
    ImGui::EndGroup();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", ui2::anchorName(w->anchor));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", TR("Picking an anchor keeps the widget exactly "
                                   "where it is — only the offsets are recomputed."));

    // ---- 矩形 ----
    float pos[2] = {w->x, w->y};
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::DragFloat2(TR("X / Y"), pos, 1.0f, -4000.0f, 4000.0f, "%.0f")) {
        actSetPos(pos[0], pos[1]);
    }
    logRect("UIED-POS", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    float size[2] = {w->w, w->h};
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::DragFloat2(TR("W / H"), size, 1.0f, 1.0f, 8000.0f, "%.0f")) {
        actSetSize(size[0], size[1]);
    }
    logRect("UIED-SIZE", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    // ---- 颜色（槽位名字随类型变，见 slotALabel）----
    // ⚠ `NoInputs`：带四个数值框的 ColorEdit4 在 330 宽的栏里会把标签挤成
    //   "Bord"（实测）。色块 + 标签，点开取色器里再填十六进制 —— 那才是
    //   作者真正会用的路径（设计稿上是 #RRGGBB）。
    float ca[4];
    toF4(w->color, ca);
    if (ImGui::ColorEdit4(slotALabel(w->type), ca,
                          ImGuiColorEditFlags_AlphaBar |
                              ImGuiColorEditFlags_AlphaPreviewHalf |
                              ImGuiColorEditFlags_NoInputs)) {
        actSetColor(fromF4(ca));
    }
    logRect("UIED-COLOR", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    float cb[4];
    toF4(w->color2, cb);
    if (ImGui::ColorEdit4(slotBLabel(w->type), cb,
                          ImGuiColorEditFlags_AlphaBar |
                              ImGuiColorEditFlags_AlphaPreviewHalf |
                              ImGuiColorEditFlags_NoInputs)) {
        actSetColor2(fromF4(cb));
    }

    ImGui::SetNextItemWidth(120.0f);
    float rad = w->radius;
    if (ImGui::DragFloat(TR("Radius"), &rad, 0.5f, 0.0f, 400.0f, "%.0f"))
        actSetRadius(rad);
    ImGui::SetNextItemWidth(120.0f);
    float inset = w->inset;
    if (ImGui::DragFloat(TR("Inset"), &inset, 0.5f, 0.0f, 200.0f, "%.0f"))
        actSetInset(inset);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", TR("Border width (nine) or inner padding "
                                   "(progress)."));

    // ---- 文本 ----
    if (w->type == ui2::UiWidgetType::Text ||
        w->type == ui2::UiWidgetType::Button) {
        if (ImGui::InputText(TR("Text"), m_textBuf, sizeof(m_textBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            actSetText(m_textBuf);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", TR("Use {name} to bind a runtime value, "
                                       "e.g. HP {health}/{maxHealth}."));
        ImGui::SetNextItemWidth(120.0f);
        float fs = w->size;
        if (ImGui::DragFloat(TR("Size"), &fs, 0.5f, 4.0f, 200.0f, "%.0f"))
            actSetFontSize(fs);
        const char* aligns[] = {"left", "center", "right"};
        int al = w->align;
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::Combo(TR("Align"), &al, aligns, 3)) actSetAlign(al);
    }

    // ---- 绑定 ----
    if (w->type == ui2::UiWidgetType::Progress) {
        ImGui::Separator();
        ImGui::TextDisabled("%s", TR("Data binding"));
        if (ImGui::InputText(TR("Value key"), m_bindBuf, sizeof(m_bindBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            actSetBind(m_bindBuf);
        }
        if (ImGui::InputText(TR("Max key"), m_bindMaxBuf, sizeof(m_bindMaxBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            actSetBindMax(m_bindMaxBuf);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", TR("Empty = 1.0. Ratio = value / max, "
                                       "clamped to 0..1."));
        ImGui::SetNextItemWidth(120.0f);
        float v = w->value;
        if (ImGui::DragFloat(TR("Value"), &v, 0.01f, 0.0f, 1000.0f, "%.2f"))
            actSetValue(v);
    }

    ImGui::Separator();
    bool vis = w->visible;
    if (ImGui::Checkbox(TR("Visible"), &vis)) actSetVisible(vis);

    ImGui::EndChild();
}

void UiEditorWindow::drawValidation() {
    ImGui::Separator();
    logRect("UIED-VALID", ImGui::GetCursorScreenPos(),
            ImVec2(ImGui::GetCursorScreenPos().x + 200.0f,
                   ImGui::GetCursorScreenPos().y + 16.0f));
    if (m_doc.widgets.empty() && m_path.empty()) {
        ImGui::TextDisabled("%s", TR("No document open — set a path and press "
                                     "Reload, or press New."));
        return;
    }
    if (m_checkValid) {
        ImGui::TextColored(kOkCol, TR("Load check: ok — %d widgets, %d warnings"),
                           ui2::countWidgets(m_doc.widgets),
                           static_cast<int>(m_checkWarnings.size()));
    } else {
        ImGui::TextColored(kBadCol, TR("Load check: %s"), m_checkError.c_str());
    }
    for (const auto& w : m_checkWarnings)
        ImGui::TextColored(kWarnCol, "  %s", w.c_str());
}

// ============================================================
// 自动化钩子
// ============================================================

void UiEditorWindow::runInjectedScript() {
    if (m_injected || !m_loaded) return;
    const char* raw = std::getenv("MYVK_EDITOR_UIED_SCRIPT");
    if (!raw || !raw[0]) return;
    m_injected = true;

    int applied = 0;
    int failed = 0;
    const std::string all(raw);

    std::size_t pos = 0;
    while (pos <= all.size()) {
        const std::size_t semi = all.find(';', pos);
        std::string tok = all.substr(
            pos, semi == std::string::npos ? std::string::npos : semi - pos);
        pos = (semi == std::string::npos) ? all.size() + 1 : semi + 1;
        const std::size_t b = tok.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) continue;
        tok = tok.substr(b);
        while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t'))
            tok.pop_back();
        if (tok.empty()) continue;

        const std::size_t colon = tok.find(':');
        const std::string name = tok.substr(0, colon);
        const std::string arg =
            (colon == std::string::npos) ? std::string() : tok.substr(colon + 1);

        bool ok = false;
        if (name == "sel") {
            ok = actSelect(arg);
        } else if (name == "add") {
            ui2::UiWidgetType t = ui2::UiWidgetType::Panel;
            ok = ui2::typeFromName(arg, t) && actAdd(t);
        } else if (name == "del") ok = actDelete();
        else if (name == "dup") ok = actDuplicate();
        else if (name == "move") ok = actMove(std::atoi(arg.c_str()) < 0 ? -1 : 1);
        else if (name == "setid") ok = actSetId(arg);
        else if (name == "setpos") {
            float x = 0, y = 0;
            if (std::sscanf(arg.c_str(), "%f,%f", &x, &y) == 2)
                ok = actSetPos(x, y);
        } else if (name == "setsize") {
            float w = 0, h = 0;
            if (std::sscanf(arg.c_str(), "%f,%f", &w, &h) == 2)
                ok = actSetSize(w, h);
        } else if (name == "setanchor") {
            ui2::UiAnchor a = ui2::UiAnchor::TopLeft;
            ok = ui2::anchorFromName(arg, a) && actSetAnchorKeepRect(a);
        } else if (name == "setcolor" || name == "setcolor2") {
            uint32_t c = 0;
            if (parseHexColor(arg, c))
                ok = (name == "setcolor") ? actSetColor(c) : actSetColor2(c);
        } else if (name == "setradius") ok = actSetRadius(static_cast<float>(std::atof(arg.c_str())));
        else if (name == "setinset") ok = actSetInset(static_cast<float>(std::atof(arg.c_str())));
        else if (name == "settext") ok = actSetText(arg);
        else if (name == "setfont") ok = actSetFontSize(static_cast<float>(std::atof(arg.c_str())));
        else if (name == "setalign") {
            const int a = (arg == "center") ? 1 : (arg == "right" ? 2 : 0);
            ok = actSetAlign(a);
        } else if (name == "setbind") ok = actSetBind(arg);
        else if (name == "setbindmax") ok = actSetBindMax(arg);
        else if (name == "setvalue") ok = actSetValue(static_cast<float>(std::atof(arg.c_str())));
        else if (name == "visible") ok = actSetVisible(arg != "0");
        else if (name == "new") ok = actNew();
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
            const std::string text = ui2::toJson(m_doc);
            if (const char* dp = std::getenv("MYVK_EDITOR_UIED_DUMP")) {
                if (FILE* f = std::fopen(dp, "wb")) {
                    std::fwrite(text.data(), 1, text.size(), f);
                    std::fclose(f);
                    VK_LOG_INFO("UIED-DUMP path=%s widgets=%d bytes=%d ok=1", dp,
                                ui2::countWidgets(m_doc.widgets),
                                static_cast<int>(text.size()));
                } else {
                    VK_LOG_WARN("UIED-DUMP path=%s ok=0 (cannot open)", dp);
                }
            } else {
                VK_LOG_WARN("UIED-DUMP ok=0 (MYVK_EDITOR_UIED_DUMP unset)");
            }
            ok = true;
        } else {
            VK_LOG_WARN("UIED-SCRIPT-BAD %s", tok.c_str());
        }

        if (ok) ++applied;
        else {
            ++failed;
            VK_LOG_WARN("UIED-SCRIPT-FAIL %s", tok.c_str());
        }
    }

    m_bufKey.clear();
    syncBuffers();
    VK_LOG_INFO("UIED-SCRIPT applied=%d failed=%d", applied, failed);
}

} // namespace editor
