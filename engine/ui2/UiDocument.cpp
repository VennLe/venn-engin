// ============================================================
// ui2/UiDocument.cpp
//
// 结构：名字表 → 颜色/φ 小工具 → parse → toJson/save → 树工具
//      → 绑定 → placeDocument（纯几何）→ drawDocument（纯绘制）。
//
// ⚠ 日志锚点前缀 `UIDOC-`。与 `UI2-*`（基础层自检）、`HUD-*`（硬编码 HUD）
//   分开：三套东西同时出现在一份日志里时，"搜某个前缀全绿"才有意义。
// ============================================================

#include "ui2/UiDocument.h"

#include "assets/AssetPath.h"
#include "core/Logger.h"
#include "ui2/SdfFont.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <system_error>

namespace ui2 {

namespace {

using ojson = nlohmann::ordered_json;

// 上限。为什么要有：`.ui.json` 是**手也能写**的格式，而控件树的深度/数量
// 一旦失控（比如某次复制粘贴出了递归结构），`placeDocument` 会在渲染线程
// 里栈溢出 —— 那个崩溃现场离"文件写错了"隔了十万八千里。
constexpr int kMaxWidgets = 512;
constexpr int kMaxDepth = 8;

// ---------------------------------------------------------------- 名字表

const char* const kTypeNames[] = {"panel", "nine", "image",
                                  "text",  "button", "progress"};
static_assert(sizeof(kTypeNames) / sizeof(kTypeNames[0]) ==
                  static_cast<std::size_t>(UiWidgetType::Count),
              "typeName 的字符串表必须与 UiWidgetType 一一对应");

const char* const kAnchorNames[] = {
    "top-left",    "top-center",    "top-right",
    "middle-left", "center",        "middle-right",
    "bottom-left", "bottom-center", "bottom-right"};
static_assert(sizeof(kAnchorNames) / sizeof(kAnchorNames[0]) == 9,
              "锚点名必须是完整的九宫格");

const char* const kAlignNames[] = {"left", "center", "right"};

// ---------------------------------------------------------------- 颜色
//
// 格式 `#RRGGBB` 或 `#RRGGBBAA`（大小写都吃，写出永远大写）。
// ⚠ 为什么不用数字：手写文档时 `4294924880` 没人看得懂；十六进制与
//   设计稿（Figma/Photoshop）复制过来的值一致。

int hexVal(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

bool parseColor(const std::string& s, uint32_t& out) {
    if (s.size() != 7 && s.size() != 9) return false;
    if (s[0] != '#') return false;
    // ⚠ 循环上界是**字节对数**，不是字符串长度：写成 `i + 1 < s.size()`
    //   会让 9 字符的 `#RRGGBBAA` 读到 s[15]（越界，Debug 下直接 assert）。
    const int n = static_cast<int>((s.size() - 1) / 2);
    int v[4] = {0, 0, 0, 255};
    for (int i = 0; i < n; ++i) {
        const int hi = hexVal(s[static_cast<std::size_t>(1 + i * 2)]);
        const int lo = hexVal(s[static_cast<std::size_t>(2 + i * 2)]);
        if (hi < 0 || lo < 0) return false;
        v[i] = hi * 16 + lo;
    }
    out = rgba(v[0], v[1], v[2], v[3]);
    return true;
}

std::string colorToHex(uint32_t c) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X",
                  static_cast<int>(c & 0xFFu),
                  static_cast<int>((c >> 8) & 0xFFu),
                  static_cast<int>((c >> 16) & 0xFFu),
                  static_cast<int>((c >> 24) & 0xFFu));
    return buf;
}

// ---------------------------------------------------------------- id 规则
//
// ⚠ id 必须能进日志锚点（`UIDOC-BOUNDS id=hpbar x=..`）。锚点是**空格
//   分隔的 k=v**（见 ui_hud_roadmap §6 的锚点纪律），所以 id 里不许有
//   空格；`/` 是路径分隔符，也不许有。允许 `.` `-` `_` 与字母数字。
bool idValid(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (char ch : id) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' ||
                        ch == '.';
        if (!ok) return false;
    }
    return true;
}

// 解析上下文：错误只记**第一条**（后续都是它的连锁反应，全记下来只会
// 把真正的原因埋掉）。
struct Parser {
    UiDocument& doc;
    std::string source;
    int count = 0;
    bool failed = false;

    bool fail(const std::string& where, const std::string& msg) {
        if (failed) return false;
        doc.error = where.empty() ? msg : (where + ": " + msg);
        if (!source.empty()) doc.error += " (" + source + ")";
        failed = true;
        return false;
    }
    void warn(const std::string& where, const std::string& msg) {
        doc.warnings.push_back(where.empty() ? msg : (where + ": " + msg));
    }

    bool parseWidget(const nlohmann::json& j, const std::string& whereBase,
                     int depth, UiWidget& out);
};

bool Parser::parseWidget(const nlohmann::json& j, const std::string& whereBase,
                         int depth, UiWidget& out) {
    if (depth > kMaxDepth)
        return fail(whereBase, "nesting is deeper than the limit");
    if (!j.is_object()) return fail(whereBase, "widget must be an object");
    if (++count > kMaxWidgets)
        return fail(whereBase, "more widgets than the limit");

    // ---- id ----
    const auto itId = j.find("id");
    if (itId == j.end() || !itId->is_string())
        return fail(whereBase, "missing \"id\"");
    out.id = itId->get<std::string>();
    if (!idValid(out.id))
        return fail(whereBase,
                    "bad \"id\" (letters, digits, '_', '-', '.' only, <= 64)");
    const std::string where = whereBase + " (id=" + out.id + ")";

    // ---- type ----
    const auto itType = j.find("type");
    if (itType == j.end() || !itType->is_string())
        return fail(where, "missing \"type\"");
    if (!typeFromName(itType->get<std::string>(), out.type)) {
        return fail(where, "unknown type \"" + itType->get<std::string>() +
                               "\" (panel/nine/image/text/button/progress)");
    }

    // ---- anchor ----
    const auto itAnchor = j.find("anchor");
    if (itAnchor != j.end()) {
        if (!itAnchor->is_string())
            return fail(where, "\"anchor\" must be a string");
        if (!anchorFromName(itAnchor->get<std::string>(), out.anchor)) {
            return fail(where, "unknown anchor \"" +
                                   itAnchor->get<std::string>() + "\"");
        }
    }

    // ---- rect ----
    const auto itRect = j.find("rect");
    if (itRect == j.end() || !itRect->is_array() || itRect->size() != 4)
        return fail(where, "\"rect\" must be [x, y, w, h]");
    for (int i = 0; i < 4; ++i) {
        if (!(*itRect)[static_cast<std::size_t>(i)].is_number())
            return fail(where, "\"rect\" entries must be numbers");
    }
    out.x = (*itRect)[0].get<float>();
    out.y = (*itRect)[1].get<float>();
    out.w = (*itRect)[2].get<float>();
    out.h = (*itRect)[3].get<float>();
    // ⚠ 负宽高必须拒绝：`roundRect` 收到负宽会画出镜像矩形，屏幕上是
    //   "控件跑到父矩形外面"，看起来像锚点算错。
    if (out.w <= 0.0f || out.h <= 0.0f)
        return fail(where, "\"rect\" width and height must be > 0");

    // ---- 颜色 ----
    const std::pair<const char*, uint32_t*> cols[] = {
        {"color", &out.color}, {"color2", &out.color2}};
    for (const auto& c : cols) {
        const auto it = j.find(c.first);
        if (it == j.end()) continue;
        if (!it->is_string() || !parseColor(it->get<std::string>(), *c.second))
            warn(where, std::string("\"") + c.first +
                            "\" is not #RRGGBB / #RRGGBBAA — using the default");
    }

    // ---- 数值 ----
    auto num = [&](const char* key, float& dst) {
        const auto it = j.find(key);
        if (it != j.end() && it->is_number()) dst = it->get<float>();
    };
    num("radius", out.radius);
    num("inset", out.inset);
    num("size", out.size);
    num("value", out.value);
    if (out.radius < 0.0f) out.radius = 0.0f;
    if (out.inset < 0.0f) out.inset = 0.0f;
    if (out.size <= 0.0f) out.size = 20.0f;

    // ---- 字符串 ----
    auto str = [&](const char* key, std::string& dst) {
        const auto it = j.find(key);
        if (it != j.end() && it->is_string()) dst = it->get<std::string>();
    };
    str("text", out.text);
    str("bind", out.bind);
    str("bindMax", out.bindMax);

    // ---- align ----
    const auto itAlign = j.find("align");
    if (itAlign != j.end()) {
        if (!itAlign->is_string())
            return fail(where, "\"align\" must be a string");
        const std::string a = itAlign->get<std::string>();
        bool hit = false;
        for (int i = 0; i < 3; ++i) {
            if (a == kAlignNames[i]) {
                out.align = i;
                hit = true;
            }
        }
        if (!hit) return fail(where, "align must be left/center/right");
    }

    // ---- visible ----
    const auto itVis = j.find("visible");
    if (itVis != j.end()) {
        if (!itVis->is_boolean()) return fail(where, "\"visible\" must be bool");
        out.visible = itVis->get<bool>();
    }

    // ---- uv（image）----
    const auto itUv = j.find("uv");
    if (itUv != j.end()) {
        if (!itUv->is_array() || itUv->size() != 4)
            return fail(where, "\"uv\" must be [u0, v0, u1, v1]");
        for (int i = 0; i < 4; ++i) {
            if (!(*itUv)[static_cast<std::size_t>(i)].is_number())
                return fail(where, "\"uv\" entries must be numbers");
        }
        out.u0 = (*itUv)[0].get<float>();
        out.v0 = (*itUv)[1].get<float>();
        out.u1 = (*itUv)[2].get<float>();
        out.v1 = (*itUv)[3].get<float>();
    }

    // ---- children ----
    const auto itKids = j.find("children");
    if (itKids != j.end()) {
        if (!itKids->is_array()) return fail(where, "\"children\" must be array");
        int idx = 0;
        for (const auto& k : *itKids) {
            char wb[96];
            std::snprintf(wb, sizeof(wb), "%s.children[%d]", where.c_str(), idx);
            UiWidget child;
            if (!parseWidget(k, wb, depth + 1, child)) return false;
            out.children.push_back(std::move(child));
            ++idx;
        }
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------- 名字表

const char* typeName(UiWidgetType t) {
    const int i = static_cast<int>(t);
    if (i < 0 || i >= static_cast<int>(UiWidgetType::Count)) return "panel";
    return kTypeNames[i];
}

bool typeFromName(const std::string& s, UiWidgetType& out) {
    for (int i = 0; i < static_cast<int>(UiWidgetType::Count); ++i) {
        if (s == kTypeNames[i]) {
            out = static_cast<UiWidgetType>(i);
            return true;
        }
    }
    return false;
}

const char* anchorName(UiAnchor a) {
    const int i = static_cast<int>(a);
    if (i < 0 || i > 8) return "top-left";
    return kAnchorNames[i];
}

bool anchorFromName(const std::string& s, UiAnchor& out) {
    for (int i = 0; i < 9; ++i) {
        if (s == kAnchorNames[i]) {
            out = static_cast<UiAnchor>(i);
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------- 解析

bool parseUiDocument(const std::string& jsonText, const std::string& sourcePath,
                     UiDocument& out) {
    out = UiDocument{};
    Parser p{out, sourcePath};

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(jsonText);
    } catch (const std::exception& e) {
        p.fail("", std::string("invalid JSON: ") + e.what());
        return false;
    }
    if (!j.is_object()) {
        p.fail("", "root must be an object");
        return false;
    }

    const auto itVer = j.find("version");
    if (itVer != j.end()) {
        if (!itVer->is_number_integer() || itVer->get<int>() != 1)
            return p.fail("", "unsupported \"version\" (only 1)");
    }

    if (const auto itName = j.find("name"); itName != j.end()) {
        if (!itName->is_string()) return p.fail("", "\"name\" must be a string");
        out.name = itName->get<std::string>();
    }

    if (const auto itCanvas = j.find("canvas"); itCanvas != j.end()) {
        if (!itCanvas->is_array() || itCanvas->size() != 2 ||
            !(*itCanvas)[0].is_number() || !(*itCanvas)[1].is_number())
            return p.fail("", "\"canvas\" must be [width, height]");
        out.refW = (*itCanvas)[0].get<int>();
        out.refH = (*itCanvas)[1].get<int>();
        if (out.refW < 320 || out.refH < 240)
            return p.fail("", "\"canvas\" must be at least 320x240");
    }

    if (const auto itC = j.find("//"); itC != j.end() && itC->is_string())
        out.comment = itC->get<std::string>();

    const auto itW = j.find("widgets");
    if (itW == j.end() || !itW->is_array())
        return p.fail("", "\"widgets\" must be an array");

    int idx = 0;
    for (const auto& jw : *itW) {
        char wb[96];
        std::snprintf(wb, sizeof(wb), "widgets[%d]", idx);
        UiWidget w;
        if (!p.parseWidget(jw, wb, 0, w)) return false;
        out.widgets.push_back(std::move(w));
        ++idx;
    }

    // 跨层 id 唯一：id 是**身份**（编辑器按 id 选中、`bind` 按 id 找控件），
    // 重复时"改了一个却动了另一个"永远查不到原因。
    std::string dup;
    if (!idsUnique(out.widgets, &dup))
        return p.fail("", "duplicate id \"" + dup + "\" (ids must be unique)");

    if (out.name.empty()) out.name = "ui";
    out.valid = true;
    return true;
}

bool loadUiDocument(const std::string& path, UiDocument& out) {
    const std::string full = assets::resolveAssetPath(path);
    std::ifstream f(full, std::ios::binary);
    if (!f) {
        out = UiDocument{};
        out.error = "cannot read " + full;
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return parseUiDocument(ss.str(), path, out);
}

// ---------------------------------------------------------------- 写回

std::string toJson(const UiDocument& d) {
    // ⚠ 不写 `valid` / `error` / `warnings`：它们是**解析结果**，写进文件
    //   之后下次一定自相矛盾（文件里写着 "valid": true，而它其实加载不了）。
    //   与 `DialogueAsset` 的"不写诊断字段"是同一条。
    ojson doc = ojson::object();
    doc["version"] = 1;
    doc["name"] = d.name;
    doc["canvas"] = ojson::array({d.refW, d.refH});
    if (!d.comment.empty()) doc["//"] = d.comment;

    // 递归写出。只写**非默认**字段 —— 一份 3 个控件的界面不该有 40 行
    // 全是 `"radius": 0`，那会让人以为这些值有意义。
    std::function<ojson(const UiWidget&)> dump = [&](const UiWidget& w) {
        ojson jw = ojson::object();
        jw["id"] = w.id;
        jw["type"] = typeName(w.type);
        if (w.anchor != UiAnchor::TopLeft) jw["anchor"] = anchorName(w.anchor);
        jw["rect"] = ojson::array({w.x, w.y, w.w, w.h});
        if (w.color != rgbA(0xFFFFFF, 255)) jw["color"] = colorToHex(w.color);
        if (w.color2 != rgbA(0x000000, 160))
            jw["color2"] = colorToHex(w.color2);
        if (w.radius != 0.0f) jw["radius"] = w.radius;
        if (w.inset != 0.0f) jw["inset"] = w.inset;
        if (!w.visible) jw["visible"] = false;
        if (!w.text.empty()) jw["text"] = w.text;
        if (w.size != 20.0f) jw["size"] = w.size;
        if (w.align != 0) jw["align"] = kAlignNames[w.align];
        if (!w.bind.empty()) jw["bind"] = w.bind;
        if (!w.bindMax.empty()) jw["bindMax"] = w.bindMax;
        if (w.value != 1.0f) jw["value"] = w.value;
        if (w.u0 != 0.0f || w.v0 != 0.0f || w.u1 != 1.0f || w.v1 != 1.0f)
            jw["uv"] = ojson::array({w.u0, w.v0, w.u1, w.v1});
        if (!w.children.empty()) {
            ojson kids = ojson::array();
            for (const auto& k : w.children) kids.push_back(dump(k));
            jw["children"] = std::move(kids);
        }
        return jw;
    };

    ojson arr = ojson::array();
    for (const auto& w : d.widgets) arr.push_back(dump(w));
    doc["widgets"] = std::move(arr);
    return doc.dump(2, ' ', false) + "\n";
}

bool saveUiDocument(const std::string& path, const UiDocument& d,
                    std::string& err) {
    err.clear();
    if (path.empty()) {
        err = "empty path";
        return false;
    }
    const std::string text = toJson(d);
    const std::string full = assets::resolveAssetPath(path);
    const std::string tmp = full + ".tmp";

    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            err = "cannot open " + tmp + " for writing";
            return false;
        }
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        f.flush();
        if (!f.good()) {
            err = "write failed: " + tmp;
            return false;
        }
    }

    std::error_code ec;
    std::filesystem::rename(tmp, full, ec);
    if (ec) {
        std::error_code ec2;
        std::filesystem::remove(tmp, ec2);
        err = "rename to " + full + " failed: " + ec.message();
        return false;
    }

    VK_LOG_INFO("UIDOC-SAVE path=%s full=%s widgets=%d bytes=%d",
                path.c_str(), full.c_str(), countWidgets(d.widgets),
                static_cast<int>(text.size()));
    return true;
}

// ---------------------------------------------------------------- 树工具

namespace {

// 在 children 列表里按**剩余路径**下钻。`path` 形如 `a/b/c`。
const UiWidget* findInChildren(const std::vector<UiWidget>& list,
                               const std::string& path) {
    if (path.empty()) return nullptr;
    const std::size_t slash = path.find('/');
    const std::string head = path.substr(0, slash);
    for (const auto& w : list) {
        if (w.id != head) continue;
        if (slash == std::string::npos) return &w;
        return findInChildren(w.children, path.substr(slash + 1));
    }
    return nullptr;
}

bool idsUniqueRec(const std::vector<UiWidget>& ws,
                  std::unordered_map<std::string, int>& seen, std::string* dup) {
    for (const auto& w : ws) {
        if (seen[w.id]++ > 0) {
            if (dup) *dup = w.id;
            return false;
        }
        if (!idsUniqueRec(w.children, seen, dup)) return false;
    }
    return true;
}

bool detachRec(std::vector<UiWidget>& list, const std::string& path,
               UiWidget* out) {
    const std::size_t slash = path.find('/');
    const std::string head = path.substr(0, slash);
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (list[i].id != head) continue;
        if (slash != std::string::npos)
            return detachRec(list[i].children, path.substr(slash + 1), out);
        if (out) *out = std::move(list[i]);
        list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
        return true;
    }
    return false;
}

}  // namespace

const UiWidget* findWidget(const UiDocument& d, const std::string& path) {
    return findInChildren(d.widgets, path);
}

UiWidget* findWidget(UiDocument& d, const std::string& path) {
    // 复用 const 版本再转掉 const：路径逻辑只写一份。
    return const_cast<UiWidget*>(findInChildren(d.widgets, path));
}

int countWidgets(const std::vector<UiWidget>& ws) {
    int n = 0;
    for (const auto& w : ws) n += 1 + countWidgets(w.children);
    return n;
}

bool idsUnique(const std::vector<UiWidget>& ws, std::string* dup) {
    std::unordered_map<std::string, int> seen;
    return idsUniqueRec(ws, seen, dup);
}

bool detachWidget(UiDocument& d, const std::string& path, UiWidget* out) {
    if (path.empty()) return false;
    UiWidget discard;
    return detachRec(d.widgets, path, out ? out : &discard);
}

bool attachWidget(UiDocument& d, const std::string& parentPath,
                  const UiWidget& w, int index) {
    std::vector<UiWidget>* list = &d.widgets;
    if (!parentPath.empty()) {
        UiWidget* p = findWidget(d, parentPath);
        if (!p) return false;
        list = &p->children;
    }
    if (index < 0 || index > static_cast<int>(list->size()))
        index = static_cast<int>(list->size());
    list->insert(list->begin() + index, w);
    return true;
}

// ---------------------------------------------------------------- 绑定

float UiBindings::number(const std::string& key, float def) const {
    const auto it = numbers.find(key);
    return (it == numbers.end()) ? def : it->second;
}

std::string UiBindings::text(const std::string& key,
                             const std::string& def) const {
    const auto it = strings.find(key);
    return (it == strings.end()) ? def : it->second;
}

std::string formatBound(const std::string& tpl, const UiBindings& b) {
    if (tpl.empty() || tpl.find('{') == std::string::npos) return tpl;
    std::string out;
    out.reserve(tpl.size() + 32);
    std::size_t i = 0;
    while (i < tpl.size()) {
        if (tpl[i] != '{') {
            out.push_back(tpl[i++]);
            continue;
        }
        const std::size_t close = tpl.find('}', i + 1);
        if (close == std::string::npos) {  // 没闭合 → 原样
            out.append(tpl.begin() + static_cast<std::ptrdiff_t>(i), tpl.end());
            break;
        }
        const std::string key = tpl.substr(i + 1, close - i - 1);
        const auto sn = b.strings.find(key);
        if (sn != b.strings.end()) {
            out += sn->second;
        } else {
            const auto nm = b.numbers.find(key);
            if (nm != b.numbers.end()) {
                char buf[48];
                const float v = nm->second;
                if (std::fabs(v - std::round(v)) < 1e-3f)
                    std::snprintf(buf, sizeof(buf), "%d",
                                  static_cast<int>(std::lround(v)));
                else
                    std::snprintf(buf, sizeof(buf), "%.1f",
                                  static_cast<double>(v));
                out += buf;
            } else {
                // 查不到：**原样留着**（见 .h 的说明）
                out.push_back('{');
                out += key;
                out.push_back('}');
            }
        }
        i = close + 1;
    }
    return out;
}

// ---------------------------------------------------------------- 布局

namespace {

// Progress 的归一化比值（夹到 0..1，除零返回 0）。
float progressRatio(const UiWidget& w, const UiBindings& b) {
    // ⚠ 绑定了键但**这一帧没有这个键** → 按 0 画（空槽），不是"回到满值"。
    //   满值会让"数据没来"看起来像"满血"，而那是最容易骗过验收的一种假象；
    //   空槽加上文本里原样显示的 `{health}`，一眼就知道是数据没接上。
    const float v = w.bind.empty() ? w.value : b.number(w.bind, 0.0f);
    const float maxV = w.bindMax.empty() ? 1.0f : b.number(w.bindMax, 0.0f);
    if (!(maxV > 0.0f)) return 0.0f;
    const float r = v / maxV;
    return r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r);
}

void placeRec(const std::vector<UiWidget>& ws, const UiRect& parent,
              const UiBindings& b, float scale, std::vector<UiDocPlaced>& out) {
    for (const auto& w : ws) {
        if (!w.visible) continue;

        UiPlacement p;
        p.anchor = w.anchor;
        p.x = w.x;
        p.y = w.y;
        p.w = w.w;
        p.h = w.h;

        UiDocPlaced rec;
        rec.id = w.id;
        rec.type = w.type;
        rec.rect = placeRectIn(parent, p, scale);
        rec.fill = rec.rect;
        if (w.type == UiWidgetType::Progress) {
            const float inset = w.inset * scale;
            const float innerW = rec.rect.w - inset * 2.0f;
            rec.fill.x = rec.rect.x + inset;
            rec.fill.y = rec.rect.y + inset;
            rec.fill.w = innerW * progressRatio(w, b);
            rec.fill.h = rec.rect.h - inset * 2.0f;
        }
        // ⚠ **每个可见控件都要出一条记录**，哪怕矩形退化了。绘制侧靠
        //   "记录序号"与树同步推进；这里漏一条，后面所有控件都会画到
        //   邻居的位置上（而这种错看起来像"锚点算错了"，极难定位）。
        out.push_back(rec);

        if (!w.children.empty()) placeRec(w.children, rec.rect, b, scale, out);
    }
}

}  // namespace

std::vector<UiDocPlaced> placeDocumentAt(const UiDocument& d,
                                         const UiBindings& b,
                                         const UiRect& canvas, float scale) {
    std::vector<UiDocPlaced> out;
    if (!d.valid || canvas.w <= 0.0f || canvas.h <= 0.0f || scale <= 0.0f)
        return out;
    placeRec(d.widgets, canvas, b, scale, out);
    return out;
}

std::vector<UiDocPlaced> placeDocument(const UiDocument& d,
                                       const UiBindings& b, uint32_t outW,
                                       uint32_t outH) {
    UiRect screen;
    screen.w = static_cast<float>(outW);
    screen.h = static_cast<float>(outH);
    return placeDocumentAt(d, b, screen, computeScale(outW, outH));
}

// ---------------------------------------------------------------- 绘制

namespace {

void drawText(UiCanvas& c, const SdfFont& font, const UiWidget& w,
              const UiRect& r, const std::string& utf8, uint32_t color,
              float scale, UiDocDrawStats& st) {
    const float size = w.size * scale;
    if (size <= 0.5f || utf8.empty()) return;

    // 行数与总高（单行时按"视觉居中"；多行时从顶部往下排）。
    int lines = 1;
    for (char ch : utf8)
        if (ch == '\n') ++lines;
    const float lineH = size * 1.25f;
    float baseline = 0.0f;
    if (lines == 1) {
        // 0.35×size 是"基线到大字视觉中心"的经验值（与 Hud.cpp 同一口径）。
        baseline = r.y + r.h * 0.5f + size * 0.35f;
    } else {
        baseline = r.y + size + (r.h - lineH * static_cast<float>(lines)) * 0.5f;
    }

    // 水平对齐：按**最宽的一行**量（多行时逐行对齐会歪）。
    std::size_t start = 0;
    float widest = 0.0f;
    for (std::size_t i = 0; i <= utf8.size(); ++i) {
        if (i == utf8.size() || utf8[i] == '\n') {
            const std::string line = utf8.substr(start, i - start);
            widest = std::max(widest, c.measure(font, line.c_str(), size));
            start = i + 1;
        }
    }
    float x = r.x;
    if (w.align == 1) x = r.x + (r.w - widest) * 0.5f;
    else if (w.align == 2) x = r.right() - widest;

    c.text(font, x, baseline, utf8.c_str(), color, size);
    ++st.texts;
}

void drawRec(UiCanvas& c, const std::vector<UiWidget>& ws,
             const std::vector<UiDocPlaced>& placed, std::size_t& cursor,
             const UiBindings& b, const SdfFont* font, float scale,
             UiDocDrawStats& st) {
    // ⚠ `placed` 与树的遍历顺序**必须**一致（都是"父在前、子在后、DFS"）。
    //   两边各自递归会写两遍树，而"少跳过一个 invisible 节点"这种错会让
    //   整棵树的矩形错位一格 —— 症状是"控件全画在别人的位置上"。
    for (const auto& w : ws) {
        if (!w.visible) {
            // 不可见：`placeDocument` 也不为它出记录 → 序号**不推进**，
            // 整棵子树一起跳过（与布局侧完全一致）。
            ++st.skipped;
            continue;
        }
        if (cursor >= placed.size()) return;
        ++st.widgets;
        const UiRect r = placed[cursor].rect;
        const UiRect fill = placed[cursor].fill;
        ++cursor;

        const bool degenerate = (r.w <= 0.5f || r.h <= 0.5f);
        if (degenerate) {
            // 父矩形为空（窗口最小化等）→ 不画，但**序号必须继续推进**，
            // 否则后面所有控件都会错位一格。
            ++st.skipped;
            if (!w.children.empty())
                drawRec(c, w.children, placed, cursor, b, font, scale, st);
            continue;
        }

        switch (w.type) {
            case UiWidgetType::Panel:
                c.roundRect(r.x, r.y, r.w, r.h, w.radius * scale, w.color);
                break;
            case UiWidgetType::Nine: {
                const float rad = w.radius * scale;
                c.roundRect(r.x, r.y, r.w, r.h, rad, w.color);
                const float in = w.inset * scale;
                if (r.w > in * 2.0f && r.h > in * 2.0f) {
                    c.roundRect(r.x + in, r.y + in, r.w - in * 2.0f,
                                r.h - in * 2.0f, std::max(0.0f, rad - in),
                                w.color2);
                }
                break;
            }
            case UiWidgetType::Image:
                c.image(r.x, r.y, r.w, r.h, w.u0, w.v0, w.u1, w.v1, w.color);
                break;
            case UiWidgetType::Text:
                if (font && font->valid())
                    drawText(c, *font, w, r, formatBound(w.text, b), w.color,
                             scale, st);
                break;
            case UiWidgetType::Button: {
                c.roundRect(r.x, r.y, r.w, r.h, w.radius * scale, w.color);
                if (font && font->valid())
                    drawText(c, *font, w, r, formatBound(w.text, b), w.color2,
                             scale, st);
                break;
            }
            case UiWidgetType::Progress: {
                const float rad = std::max(0.0f, w.radius * scale);
                c.roundRect(r.x, r.y, r.w, r.h, rad, w.color2);
                if (fill.w > 0.5f && fill.h > 0.5f)
                    c.roundRect(fill.x, fill.y, fill.w, fill.h, rad, w.color);
                break;
            }
            default:
                break;
        }

        if (!w.children.empty())
            drawRec(c, w.children, placed, cursor, b, font, scale, st);
    }
}

}  // namespace

UiDocDrawStats drawDocument(UiCanvas& c, const UiDocument& d,
                            const UiBindings& b, uint32_t outW, uint32_t outH,
                            const SdfFont* font) {
    UiDocDrawStats st;
    if (!d.valid || outW == 0 || outH == 0) return st;
    const float scale = computeScale(outW, outH);
    if (scale <= 0.0f) return st;

    const std::vector<UiDocPlaced> placed = placeDocument(d, b, outW, outH);
    std::size_t cursor = 0;
    drawRec(c, d.widgets, placed, cursor, b, font, scale, st);
    return st;
}

}  // namespace ui2
