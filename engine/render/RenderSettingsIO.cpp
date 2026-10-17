#include "render/RenderSettingsIO.h"

#include "core/Logger.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

namespace render {

namespace {
using json = nlohmann::json;

constexpr int kFormatVersion = 1;
constexpr const char* kDefaultProjectPath = "config/render.json";
constexpr const char* kDefaultPrefsPath = "config/render_prefs.json";

// loadStartupLayers() 只读一次（分阶段绑定靠 applyToRegistry 的取用式标记
// 来补位，不需要重读文件）。reloadFromDisk() 会把它清回去。
bool g_startupLoaded = false;

bool isFinite(double x) { return std::isfinite(x); }

int enumCountOf(const ParamDesc& d) {
    int n = 0;
    while (d.enumNames && d.enumNames[n]) ++n;
    return n;
}

// 把 float 写成"能被精确读回的最短十进制串"。
// 为什么需要它：nlohmann 把 number 存成 double，直接 dump 0.1f 会得到
// 0.10000000149011612。这里先用 7 位有效数字试一次，若转回 float 仍相等
// （绝大多数情况）就用它；否则退回 9 位（float32 的保证位数）。
std::string formatFloat(float f) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.7g", static_cast<double>(f));
    if (static_cast<float>(std::strtod(buf, nullptr)) == f) return buf;
    std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(f));
    return buf;
}

// 值 → 日志/诊断用的短字符串
std::string formatValue(ParamKind kind, const SettingValue& v) {
    char buf[128];
    switch (kind) {
        case ParamKind::Bool:
            return v.v[0] != 0.0 ? "true" : "false";
        case ParamKind::Int:
        case ParamKind::Enum:
            std::snprintf(buf, sizeof(buf), "%d",
                          static_cast<int>(std::llround(v.v[0])));
            return buf;
        case ParamKind::Float:
            if (v.count >= 2) {
                std::snprintf(buf, sizeof(buf), "%.6g,%.6g", v.v[0], v.v[1]);
                return buf;
            }
            std::snprintf(buf, sizeof(buf), "%.6g", v.v[0]);
            return buf;
        case ParamKind::Color3:
        case ParamKind::Vec3:
            std::snprintf(buf, sizeof(buf), "%.4g,%.4g,%.4g", v.v[0], v.v[1],
                          v.v[2]);
            return buf;
    }
    return "?";
}

// json 值 → SettingValue（按 ParamKind 解释）。
bool parseValue(const ParamDesc& d, const json& j, SettingValue& out,
                const char** why) {
    out.count = 1;
    out.v[0] = out.v[1] = out.v[2] = 0.0;

    switch (d.kind) {
        case ParamKind::Bool: {
            if (j.is_boolean()) {
                out.v[0] = j.get<bool>() ? 1.0 : 0.0;
                return true;
            }
            if (j.is_number()) {
                out.v[0] = (j.get<double>() != 0.0) ? 1.0 : 0.0;
                return true;
            }
            if (j.is_string()) {
                const std::string s = j.get<std::string>();
                if (s == "on" || s == "true" || s == "1") { out.v[0] = 1.0; return true; }
                if (s == "off" || s == "false" || s == "0") { out.v[0] = 0.0; return true; }
            }
            *why = "bool 只接受 true/false/0/1/\"on\"/\"off\"";
            return false;
        }

        case ParamKind::Int: {
            if (!j.is_number()) { *why = "int 只接受数字"; return false; }
            out.v[0] = static_cast<double>(std::llround(j.get<double>()));
            return true;
        }

        case ParamKind::Enum: {
            if (j.is_number()) {
                out.v[0] = static_cast<double>(std::llround(j.get<double>()));
                return true;
            }
            if (j.is_string()) {
                // 允许写选项名（"?tonemap": "Reinhard"），比记数字好用得多
                const std::string s = j.get<std::string>();
                const int n = enumCountOf(d);
                for (int i = 0; i < n; ++i) {
                    if (s == d.enumNames[i]) { out.v[0] = i; return true; }
                }
                *why = "枚举里没有这个选项名";
                return false;
            }
            *why = "enum 只接受数字或选项名";
            return false;
        }

        case ParamKind::Float: {
            if (j.is_array()) {
                // FloatPair（pom.fade 那种"一个键写两个字段"）
                if (j.size() < 2 || !j[0].is_number() || !j[1].is_number()) {
                    *why = "float 数组需要两个数字";
                    return false;
                }
                out.count = 2;
                out.v[0] = j[0].get<double>();
                out.v[1] = j[1].get<double>();
                return true;
            }
            if (!j.is_number()) { *why = "float 只接受数字"; return false; }
            out.v[0] = j.get<double>();
            return true;
        }

        case ParamKind::Color3:
        case ParamKind::Vec3: {
            if (!j.is_array() || j.size() < 3) {
                *why = "颜色/三元组需要 [x, y, z]";
                return false;
            }
            for (int i = 0; i < 3; ++i) {
                if (!j[static_cast<size_t>(i)].is_number()) {
                    *why = "颜色分量必须是数字";
                    return false;
                }
                out.v[i] = j[static_cast<size_t>(i)].get<double>();
            }
            out.count = 3;
            return true;
        }
    }
    *why = "未知类型";
    return false;
}

// 与 env 完全同一套校验（见 RenderSettingsIO.h 顶部约束 ④ 的说明）。
// 返回 false = 拒绝这条覆盖（保持默认 / 保持上一层）。
bool sanitize(const ParamDesc& d, SettingValue& v, const char** why) {
    for (int i = 0; i < v.count; ++i) {
        if (!isFinite(v.v[i])) { *why = "非有限值(NaN/Inf)"; return false; }
    }

    switch (d.kind) {
        case ParamKind::Bool:
            v.v[0] = (v.v[0] != 0.0) ? 1.0 : 0.0;
            return true;

        case ParamKind::Enum: {
            const int n = enumCountOf(d);
            const int idx = static_cast<int>(std::llround(v.v[0]));
            if (n > 0 && (idx < 0 || idx >= n)) {
                *why = "枚举下标越界";
                return false;
            }
            v.v[0] = idx;
            return true;
        }

        case ParamKind::Int:
        case ParamKind::Float: {
            const double raw = v.v[0];
            if (raw < static_cast<double>(d.minAccept) ||
                raw > static_cast<double>(d.maxAccept)) {
                *why = "超出可接受范围";
                return false;
            }
            double out = raw;
            if (d.clampLo < d.clampHi) {
                out = out < static_cast<double>(d.clampLo)
                          ? static_cast<double>(d.clampLo)
                          : (out > static_cast<double>(d.clampHi)
                                 ? static_cast<double>(d.clampHi)
                                 : out);
            }
            v.v[0] = out;
            // FloatPair（env 层的规则：起点 >= 0 且终点 > 起点）
            if (d.kind == ParamKind::Float && v.count >= 2) {
                if (d.envMode == EnvMode::FloatPair) {
                    if (!(out >= 0.0) || !(v.v[1] > out)) {
                        *why = "pair 需要 a>=0 且 b>a";
                        return false;
                    }
                }
                if (!isFinite(v.v[1])) { *why = "非有限值(NaN/Inf)"; return false; }
            }
            return true;
        }

        case ParamKind::Color3:
        case ParamKind::Vec3: {
            if (d.clampLo < d.clampHi) {
                for (int i = 0; i < 3; ++i) {
                    v.v[i] = v.v[i] < static_cast<double>(d.clampLo)
                                 ? static_cast<double>(d.clampLo)
                                 : (v.v[i] > static_cast<double>(d.clampHi)
                                        ? static_cast<double>(d.clampHi)
                                        : v.v[i]);
                }
            }
            return true;
        }
    }
    return false;
}

void writeValue(const BindingView& b, const SettingValue& v) {
    switch (b.kind) {
        case ParamKind::Bool:
            *static_cast<bool*>(b.ptr) = (v.v[0] != 0.0);
            break;
        case ParamKind::Int:
        case ParamKind::Enum:
            *static_cast<int*>(b.ptr) = static_cast<int>(std::llround(v.v[0]));
            break;
        case ParamKind::Float:
            *static_cast<float*>(b.ptr) = static_cast<float>(v.v[0]);
            if (b.aux && v.count >= 2) {
                *static_cast<float*>(b.aux) = static_cast<float>(v.v[1]);
            }
            break;
        case ParamKind::Color3:
        case ParamKind::Vec3: {
            float* p = static_cast<float*>(b.ptr);
            p[0] = static_cast<float>(v.v[0]);
            p[1] = static_cast<float>(v.v[1]);
            p[2] = static_cast<float>(v.v[2]);
            break;
        }
    }
}

SettingValue readValue(const BindingView& b) {
    SettingValue v;
    switch (b.kind) {
        case ParamKind::Bool:
            v.v[0] = *static_cast<bool*>(b.ptr) ? 1.0 : 0.0;
            break;
        case ParamKind::Int:
        case ParamKind::Enum:
            v.v[0] = *static_cast<int*>(b.ptr);
            break;
        case ParamKind::Float:
            v.v[0] = *static_cast<float*>(b.ptr);
            if (b.aux) {
                v.count = 2;
                v.v[1] = *static_cast<float*>(b.aux);
            }
            break;
        case ParamKind::Color3:
        case ParamKind::Vec3: {
            const float* p = static_cast<const float*>(
                static_cast<const void*>(b.ptr));
            v.count = 3;
            v.v[0] = p[0];
            v.v[1] = p[1];
            v.v[2] = p[2];
            break;
        }
    }
    return v;
}

SettingValue defaultValueOf(const BindingView& b) {
    SettingValue v;
    switch (b.kind) {
        case ParamKind::Bool:
            v.v[0] = b.dBool ? 1.0 : 0.0;
            break;
        case ParamKind::Int:
        case ParamKind::Enum:
            v.v[0] = b.dInt;
            break;
        case ParamKind::Float:
            v.v[0] = b.dFloat;
            if (b.aux) {
                // aux 的默认值没有单独捕获 —— 绑定那一刻 aux 也是默认值，
                // 但 FeatureRegistry 只记了主字段。保守做法：aux 参数永远
                // 参与导出（宁可多写一个键，也不要把用户改过的值漏掉）。
                v.count = 2;
                v.v[1] = 0.0;
            }
            break;
        case ParamKind::Color3:
        case ParamKind::Vec3:
            v.count = 3;
            v.v[0] = b.dColor[0];
            v.v[1] = b.dColor[1];
            v.v[2] = b.dColor[2];
            break;
    }
    return v;
}

bool sameValue(const SettingValue& a, const SettingValue& b) {
    if (a.count != b.count) return false;
    for (int i = 0; i < a.count; ++i) {
        if (a.v[i] != b.v[i]) return false;
    }
    return true;
}

SettingValue scalarValue(double x) {
    SettingValue v;
    v.v[0] = x;
    return v;
}

} // namespace

// ============================================================
// 作用域名
// ============================================================

const char* settingScopeName(SettingScope s) {
    switch (s) {
        case SettingScope::EditorPrefs: return "prefs";
        case SettingScope::Project:     return "project";
        case SettingScope::Scene:       return "scene";
        case SettingScope::Object:      return "object";
        case SettingScope::Camera:      return "camera";
    }
    return "?";
}

// ============================================================
// 覆盖链
// ============================================================

void SettingsChain::setLayer(const SettingsLayer& layer) {
    const int i = static_cast<int>(layer.scope);
    if (i < 0 || i >= kSettingScopeCount) return;
    m_layers[i] = layer;
    m_layers[i].scope = layer.scope;
}

void SettingsChain::clearLayer(SettingScope s) {
    const int i = static_cast<int>(s);
    if (i < 0 || i >= kSettingScopeCount) return;
    m_layers[i].clear();
    m_layers[i].scope = s;
}

void SettingsChain::clear() {
    for (int i = 0; i < kSettingScopeCount; ++i) {
        m_layers[i].clear();
        m_layers[i].scope = static_cast<SettingScope>(i);
    }
}

const SettingsLayer* SettingsChain::layer(SettingScope s) const {
    const int i = static_cast<int>(s);
    if (i < 0 || i >= kSettingScopeCount) return nullptr;
    return &m_layers[i];
}

int SettingsChain::presentLayerCount() const {
    int n = 0;
    for (int i = 0; i < kSettingScopeCount; ++i) {
        if (m_layers[i].present) ++n;
    }
    return n;
}

int SettingsChain::valueCount() const {
    int n = 0;
    for (int i = 0; i < kSettingScopeCount; ++i) {
        n += static_cast<int>(m_layers[i].values.size());
    }
    return n;
}

bool SettingsChain::resolve(const std::string& paramId, SettingValue& out,
                            SettingScope* from) const {
    // 从最具体（Camera）往最泛（EditorPrefs）回退
    for (int i = kSettingScopeCount - 1; i >= 0; --i) {
        const auto it = m_layers[i].values.find(paramId);
        if (it == m_layers[i].values.end()) continue;
        out = it->second;
        if (from) *from = m_layers[i].scope;
        return true;
    }
    return false;
}

std::string SettingsChain::describe() const {
    std::string s;
    for (int i = 0; i < kSettingScopeCount; ++i) {
        if (!m_layers[i].present) continue;
        if (!s.empty()) s += ",";
        s += settingScopeName(m_layers[i].scope);
        s += "=";
        s += std::to_string(m_layers[i].values.size());
    }
    return s.empty() ? "none" : s;
}

SettingsChain& activeChain() {
    static SettingsChain s;
    return s;
}

// ============================================================
// 路径与守卫
// ============================================================

bool renderSettingsAutomationRun() {
    return std::getenv("MYVK_FRAMES") != nullptr ||
           std::getenv("MYVK_NO_WINDOW_SAVE") != nullptr;
}

std::string projectSettingsPath() {
    if (const char* p = std::getenv("MYVK_RENDER_SETTINGS")) {
        if (p[0] != '\0') return p;
    }
    return kDefaultProjectPath;
}

std::string prefsSettingsPath() {
    if (const char* p = std::getenv("MYVK_RENDER_PREFS")) {
        if (p[0] != '\0') return p;
    }
    return kDefaultPrefsPath;
}

bool projectSettingsPathExplicit() {
    const char* p = std::getenv("MYVK_RENDER_SETTINGS");
    return p != nullptr && p[0] != '\0';
}

bool prefsSettingsPathExplicit() {
    const char* p = std::getenv("MYVK_RENDER_PREFS");
    return p != nullptr && p[0] != '\0';
}

// ============================================================
// 解析
// ============================================================

bool parseLayerText(SettingScope scope, const std::string& source,
                    const std::string& text, SettingsLayer& out) {
    out.clear();
    out.scope = scope;
    out.source = source;

    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        // 验收条款：非法 json 只告警 + 回落默认，绝不崩。
        VK_LOG_WARN("RENDER-SET '%s' 解析失败（%s）→ 忽略这一层，全部走默认",
                    source.c_str(), e.what());
        return false;
    }
    if (!j.is_object()) {
        VK_LOG_WARN("RENDER-SET '%s' 顶层不是 JSON 对象 → 忽略这一层",
                    source.c_str());
        return false;
    }

    out.present = true;
    for (auto it = j.begin(); it != j.end(); ++it) {
        const std::string& key = it.key();
        if (key == "version" || key == "$schema") continue;
        if (!key.empty() && key[0] == '_') continue;   // _comment 之类

        const ParamDesc* d = findParam(key.c_str());
        if (!d) {
            ++out.rejected;
            VK_LOG_WARN("RENDER-SET 未知参数 '%s'（%s）→ 已忽略",
                        key.c_str(), source.c_str());
            continue;
        }
        SettingValue v;
        const char* why = "类型不符";
        if (!parseValue(*d, it.value(), v, &why) || !sanitize(*d, v, &why)) {
            ++out.rejected;
            VK_LOG_WARN("RENDER-SET 参数 '%s' 的值被拒（%s）→ 保持默认",
                        key.c_str(), why);
            continue;
        }
        out.values[key] = v;
    }
    return true;
}

bool loadLayerFile(SettingScope scope, const std::string& path,
                   SettingsLayer& out) {
    out.clear();
    out.scope = scope;
    out.source = path;

    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) return false;   // 正常情况

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        VK_LOG_WARN("RENDER-SET 打不开 '%s' → 忽略这一层", path.c_str());
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    return parseLayerText(scope, path, text, out);
}

bool saveLayerFile(const SettingsLayer& layer, const std::string& path) {
    json j;
    j["version"] = kFormatVersion;
    j["_comment"] =
        "Venn render settings. Keys are <feature>.<param> (see "
        "engine/render/FeatureRegistry.cpp). Absent keys fall back to the next "
        "less specific scope, then to engine defaults.";

    for (const auto& kv : layer.values) {
        const ParamDesc* d = findParam(kv.first.c_str());
        const SettingValue& v = kv.second;
        if (v.count >= 3) {
            json arr = json::array();
            for (int i = 0; i < 3; ++i) {
                arr.push_back(json::parse(formatFloat(static_cast<float>(v.v[i]))));
            }
            j[kv.first] = std::move(arr);
        } else if (v.count == 2) {
            json arr = json::array();
            for (int i = 0; i < 2; ++i) {
                arr.push_back(json::parse(formatFloat(static_cast<float>(v.v[i]))));
            }
            j[kv.first] = std::move(arr);
        } else if (d && d->kind == ParamKind::Bool) {
            j[kv.first] = (v.v[0] != 0.0);
        } else if (d && (d->kind == ParamKind::Int || d->kind == ParamKind::Enum)) {
            j[kv.first] = static_cast<int>(std::llround(v.v[0]));
        } else {
            j[kv.first] = json::parse(formatFloat(static_cast<float>(v.v[0])));
        }
    }

    const std::filesystem::path p(path);
    std::error_code ec;
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path(), ec);
        if (ec) {
            VK_LOG_WARN("RENDER-SET 建目录失败 '%s': %s",
                        p.parent_path().string().c_str(), ec.message().c_str());
            return false;
        }
    }

    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        VK_LOG_WARN("RENDER-SET 打不开写入目标 '%s'", path.c_str());
        return false;
    }
    ofs << j.dump(2) << "\n";
    if (!ofs) {
        VK_LOG_WARN("RENDER-SET 写入失败 '%s'", path.c_str());
        return false;
    }
    VK_LOG_INFO("RENDER-SET saved '%s' (%zu override(s))", path.c_str(),
                layer.values.size());
    return true;
}

// ============================================================
// 抓取 / 应用
// ============================================================

SettingsLayer captureFromRegistry(SettingScope scope) {
    SettingsLayer layer;
    layer.scope = scope;
    layer.source = "(captured)";
    layer.present = true;

    std::vector<BindingView> all;
    collectBindings(all);
    for (const BindingView& b : all) {
        const SettingValue cur = readValue(b);
        bool finite = true;
        for (int i = 0; i < cur.count; ++i) {
            if (!isFinite(cur.v[i])) finite = false;
        }
        if (!finite) continue;      // NaN/Inf 写不进 json，跳过
        if (sameValue(cur, defaultValueOf(b))) continue;   // 只存非默认
        layer.values[b.id] = cur;
    }
    return layer;
}

int loadStartupLayers(bool logSummary) {
    if (g_startupLoaded) return 0;
    g_startupLoaded = true;

    SettingsChain& chain = activeChain();
    const bool autoRun = renderSettingsAutomationRun();

    // ① 跨项目偏好层
    if (!autoRun || prefsSettingsPathExplicit()) {
        SettingsLayer L;
        if (loadLayerFile(SettingScope::EditorPrefs, prefsSettingsPath(), L)) {
            chain.setLayer(L);
        }
    }

    // ② 项目层
    if (!autoRun || projectSettingsPathExplicit()) {
        SettingsLayer L;
        if (loadLayerFile(SettingScope::Project, projectSettingsPath(), L)) {
            chain.setLayer(L);
        }
    }

    const int total = chain.valueCount();
    if (logSummary && total > 0) {
        for (int i = 0; i < kSettingScopeCount; ++i) {
            const SettingsLayer* L = chain.layer(static_cast<SettingScope>(i));
            if (!L || !L->present) continue;
            VK_LOG_INFO("RENDER-SET layer=%s source='%s' values=%zu rejected=%d",
                        settingScopeName(L->scope), L->source.c_str(),
                        L->values.size(), L->rejected);
        }
        VK_LOG_INFO("RENDER-SET chain=%s total=%d", chain.describe().c_str(),
                    total);
    }
    return total;
}

int applyToRegistry(bool force, bool logApplied) {
    std::vector<BindingView> todo;
    if (force) {
        collectBindings(todo);
    } else {
        takeUnappliedBindings(todo);
    }
    if (todo.empty()) return 0;

    SettingsChain& chain = activeChain();
    int applied = 0;
    int logged = 0;
    for (const BindingView& b : todo) {
        SettingValue v;
        SettingScope from = SettingScope::Project;
        if (!chain.resolve(b.id, v, &from)) continue;
        writeValue(b, v);
        ++applied;
        if (logApplied && logged < 32) {
            ++logged;
            VK_LOG_INFO("RENDER-SET applied=%s value=%s from=%s", b.id.c_str(),
                        formatValue(b.kind, v).c_str(),
                        settingScopeName(from));
        }
    }
    if (logApplied && applied > logged) {
        VK_LOG_INFO("RENDER-SET ... %d more override(s) applied",
                    applied - logged);
    }
    return applied;
}

bool reloadFromDisk() {
    g_startupLoaded = false;
    activeChain().clear();
    loadStartupLayers(true);
    // 让所有绑定重新参与一次文件层处理（env 的标记不动 —— env 已经写进
    // 内存了，这里只按用户要求把文件里的值重新盖一遍）。
    resetSettingsApplied();
    applyToRegistry(false, true);
    return true;
}

// ============================================================
// 自检（MYVK_RENDER_SETTINGS_TEST=1）
// ============================================================

int runSettingsSelfTest() {
    int pass = 0;
    int fail = 0;

    auto check = [&pass, &fail](const char* name, bool ok, const char* detail) {
        if (ok) ++pass; else ++fail;
        VK_LOG_INFO("SET-TEST %s result=%s%s%s", name, ok ? "PASS" : "FAIL",
                    (detail && detail[0]) ? " " : "", detail ? detail : "");
    };
    char buf[256];

    VK_LOG_INFO("=== SET-TEST begin ===");

    // ---------------------------------------------------------- A 解析
    {
        SettingsLayer L;
        const char* text = R"({
  "version": 1,
  "_comment": "selftest",
  "shadow.enabled": false,
  "shadow.cascades": 2,
  "ssao.strength": 1.5,
  "post.tonemap": "Reinhard",
  "cg.lift": [0.5, 0.25, 0.125]
})";
        const bool ok = parseLayerText(SettingScope::Project, "selftest", text, L);
        std::snprintf(buf, sizeof(buf), "values=%zu rejected=%d", L.values.size(),
                      L.rejected);
        check("A1_parse_flat_keys", ok && L.present && L.values.size() == 5 &&
                                        L.rejected == 0, buf);
        check("A2_bool", L.values["shadow.enabled"].v[0] == 0.0, "");
        check("A3_int", L.values["shadow.cascades"].v[0] == 2.0, "");
        check("A4_float",
              std::fabs(L.values["ssao.strength"].v[0] - 1.5) < 1e-9, "");
        check("A5_enum_by_name", L.values["post.tonemap"].v[0] == 1.0,
              "Reinhard -> 1");
        check("A6_vec3",
              L.values["cg.lift"].count == 3 &&
                  std::fabs(L.values["cg.lift"].v[2] - 0.125) < 1e-9, "");
    }
    {
        SettingsLayer L;
        const char* text = R"({
  "no.such.param": 1,
  "shadow.cascades": [1, 2],
  "post.tonemap": 9
})";
        parseLayerText(SettingScope::Project, "selftest-bad", text, L);
        std::snprintf(buf, sizeof(buf), "values=%zu rejected=%d",
                      L.values.size(), L.rejected);
        check("A7_reject_unknown_and_bad_types",
              L.values.empty() && L.rejected == 3, buf);
    }
    {
        SettingsLayer L;
        const bool ok = parseLayerText(SettingScope::Project, "selftest-broken",
                                       "{ this is not json", L);
        check("A8_invalid_json_no_crash", !ok && !L.present && L.empty(), "");
    }

    // ---------------------------------------------------------- B 五层优先级
    {
        SettingsChain ch;
        const SettingScope order[kSettingScopeCount] = {
            SettingScope::EditorPrefs, SettingScope::Project,
            SettingScope::Scene, SettingScope::Object, SettingScope::Camera};
        for (int i = 0; i < kSettingScopeCount; ++i) {
            SettingsLayer L;
            L.scope = order[i];
            L.present = true;
            L.source = settingScopeName(order[i]);
            L.values["ssao.strength"] = scalarValue(0.1 * (i + 1));
            ch.setLayer(L);
        }

        bool allOk = true;
        // 从最具体往最泛逐层删，每一轮都必须命中"当前最具体的那个"
        for (int i = kSettingScopeCount - 1; i >= 0; --i) {
            SettingValue v;
            SettingScope from = SettingScope::Project;
            const bool hit = ch.resolve("ssao.strength", v, &from);
            const bool ok = hit && from == order[i] &&
                            std::fabs(v.v[0] - 0.1 * (i + 1)) < 1e-9;
            if (!ok) {
                allOk = false;
                std::snprintf(buf, sizeof(buf), "step=%d expect=%s got=%s", i,
                              settingScopeName(order[i]),
                              hit ? settingScopeName(from) : "none");
                check("B1_specific_wins", false, buf);
            }
            ch.clearLayer(order[i]);
        }
        if (allOk) check("B1_specific_wins", true, "camera>object>scene>project>prefs");

        SettingValue v;
        check("B2_empty_chain_misses", !ch.resolve("ssao.strength", v), "");
    }

    // ---------------------------------------------------------- C 应用到绑定
    // BindingScope：作用域结束自动摘掉本次绑定，避免往全局表里留悬垂指针。
    render::BindingScope bindingScope;
    float ssaoStrength = 0.25f;   // 假装默认值
    int shadowCascades = 4;
    float pomFadeStart = 10.0f;
    float pomFadeEnd = 26.0f;
    render::bindParam("ssao.strength", &ssaoStrength, ParamKind::Float);
    render::bindParam("shadow.cascades", &shadowCascades, ParamKind::Int);
    render::bindParamAux("pom.fade", &pomFadeStart, &pomFadeEnd,
                         ParamKind::Float);
    {
        SettingsChain& chain = activeChain();
        chain.clear();
        SettingsLayer L;
        L.scope = SettingScope::Project;
        L.present = true;
        L.source = "selftest";
        L.values["ssao.strength"] = scalarValue(1.5);
        SettingValue pair;
        pair.count = 2;
        pair.v[0] = 3.0;
        pair.v[1] = 7.0;
        L.values["pom.fade"] = pair;
        chain.setLayer(L);

        resetSettingsApplied();
        const int n = applyToRegistry(false, false);
        std::snprintf(buf, sizeof(buf), "applied=%d", n);
        check("C1_apply_count", n == 2, buf);
        check("C2_float_written", ssaoStrength == 1.5f, "");
        check("C3_int_untouched", shadowCascades == 4, "chain 里没有它");
        check("C4_aux_written",
              pomFadeStart == 3.0f && pomFadeEnd == 7.0f, "");
    }

    // ---------------------------------------------------------- D 抓取只存非默认
    {
        const SettingsLayer cap = captureFromRegistry(SettingScope::Project);
        const bool hasSsao = cap.values.count("ssao.strength") == 1;
        const bool hasCascades = cap.values.count("shadow.cascades") == 1;
        std::snprintf(buf, sizeof(buf), "keys=%zu", cap.values.size());
        check("D1_capture_non_default_only", hasSsao && !hasCascades, buf);
        check("D2_capture_value",
              hasSsao && cap.values.at("ssao.strength").v[0] == 1.5, "");

        // ------------------------------------------------------ E 落盘往返
        std::error_code ec;
        const std::filesystem::path dir =
            std::filesystem::temp_directory_path(ec);
        const std::filesystem::path path =
            (ec ? std::filesystem::path(".") : dir) /
            "venn_render_settings_selftest.json";

        const bool saved = saveLayerFile(cap, path.string());
        check("E1_save_ok", saved, path.string().c_str());

        SettingsLayer back;
        const bool loaded = loadLayerFile(SettingScope::Project, path.string(), back);
        check("E2_roundtrip_present",
              loaded && back.present && back.values.size() == cap.values.size(),
              "");
        bool sameVals = loaded && back.values.size() == cap.values.size();
        if (sameVals) {
            for (const auto& kv : cap.values) {
                const auto it = back.values.find(kv.first);
                if (it == back.values.end() || !sameValue(it->second, kv.second)) {
                    sameVals = false;
                    break;
                }
            }
        }
        check("E3_roundtrip_bitwise", sameVals, "");

        // ------------------------------------------------------ F 坏文件不崩
        {
            std::ofstream bad(path, std::ios::binary | std::ios::trunc);
            bad << "{ \"ssao.strength\": ";
            bad.close();
            SettingsLayer broken;
            const bool okF = loadLayerFile(SettingScope::Project, path.string(), broken);
            check("F1_broken_file_falls_back", !okF && !broken.present, "");
        }

        std::filesystem::remove(path, ec);
    }

    activeChain().clear();

    VK_LOG_INFO("SET-TOTAL pass=%d fail=%d", pass, fail);
    VK_LOG_INFO("=== SET-TEST end ===");
    return fail;
}

} // namespace render
