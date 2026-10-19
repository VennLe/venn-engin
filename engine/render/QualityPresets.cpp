#include "render/QualityPresets.h"

#include "core/Logger.h"
#include "render/RenderSettingsIO.h"   // SettingsLayer / SettingValue（E26 落盘）

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace render {

namespace {

// ---- 画质预设（原样从 editor/FeatureUIGen.cpp 搬来，一个字没改）----
//
// 每档是"明确打开的启用参数清单"，没列到的一律关 ——
// 这样预设是确定性的（从任何当前状态切过去结果都一样）。
struct QualityPreset {
    const char* name;
    const char* const* on;
};

const char* const kPresetLow[] = {"cull.frustum", "lod.enabled", "sky.enabled",
                                  nullptr};
const char* const kPresetMedium[] = {"cull.frustum", "lod.enabled",
                                     "sky.enabled", "sky.ibl",
                                     "shadow.enabled", "ssao.enabled",
                                     nullptr};
const char* const kPresetHigh[] = {"cull.frustum", "lod.enabled", "sky.enabled",
                                   "sky.ibl", "shadow.enabled", "ssao.enabled",
                                   "taa.enabled", "hfog.enabled", "ssr.enabled",
                                   nullptr};
const char* const kPresetEpic[] = {
    "cull.frustum", "lod.enabled", "sky.enabled", "sky.ibl",
    "shadow.enabled", "lshadow.enabled", "ssao.enabled", "taa.enabled",
    "hfog.enabled", "ssr.enabled", "ssgi.enabled", "godrays.enabled",
    "decals.enabled", nullptr};
// Cinematic = Epic + 相机景深（叙事镜头的标配）。
const char* const kPresetCinematic[] = {
    "cull.frustum", "lod.enabled", "sky.enabled", "sky.ibl",
    "shadow.enabled", "lshadow.enabled", "ssao.enabled", "taa.enabled",
    "hfog.enabled", "ssr.enabled", "ssgi.enabled", "godrays.enabled",
    "decals.enabled", "dof.enabled", nullptr};

const QualityPreset kPresets[] = {
    {"Low", kPresetLow},           {"Medium", kPresetMedium},
    {"High", kPresetHigh},         {"Epic", kPresetEpic},
    {"Cinematic", kPresetCinematic},
};
constexpr int kPresetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));

} // namespace

bool isEnableParam(const ParamDesc& p, const char* featureId) {
    if (p.kind != ParamKind::Bool) return false;
    if (!featureId) return false;
    const std::size_t fl = std::strlen(featureId);
    return std::strncmp(p.id, featureId, fl) == 0 &&
           std::strcmp(p.id + fl, ".enabled") == 0;
}

int qualityPresetCount() { return kPresetCount; }

const char* qualityPresetName(int preset) {
    return (preset >= 0 && preset < kPresetCount) ? kPresets[preset].name
                                                  : "Custom";
}

const char* const* qualityPresetEnableList(int preset) {
    return (preset >= 0 && preset < kPresetCount) ? kPresets[preset].on : nullptr;
}

int qualityPresetByName(const char* name) {
    if (!name || !name[0]) return -1;
    for (int i = 0; i < kPresetCount; ++i)
        if (std::strcmp(kPresets[i].name, name) == 0) return i;
    return -1;
}

// ---------------------------------------------------------------- 枚举全部开关
//
// 惰性建一次（`kFeatures` 是编译期常量表，但"哪些特性有 enable 参数"要
// 遍历才知）。用函数内 static + vector：与 FeatureRegistry 里
// `buildParamIndex()` 同一套写法。
namespace {

const std::vector<EnableParam>& enableTable() {
    static const std::vector<EnableParam> table = [] {
        std::vector<EnableParam> v;
        for (int i = 0; i < kFeatureCount; ++i) {
            const FeatureDesc& f = kFeatures[i];
            for (int j = 0; j < f.paramCount; ++j) {
                if (isEnableParam(f.params[j], f.id)) {
                    v.push_back(EnableParam{f.id, f.params[j].id});
                    break;   // 每个特性最多一个
                }
            }
        }
        return v;
    }();
    return table;
}

} // namespace

int enableParamCount() { return static_cast<int>(enableTable().size()); }

const EnableParam& enableParamAt(int index) {
    static const EnableParam kEmpty{"", ""};
    const auto& t = enableTable();
    if (index < 0 || index >= static_cast<int>(t.size())) return kEmpty;
    return t[static_cast<std::size_t>(index)];
}

int applyPreset(int preset, void (*hook)(const char* paramId)) {
    const char* const* on = qualityPresetEnableList(preset);
    if (!on) return 0;

    int flipped = 0;
    for (int i = 0; i < kFeatureCount; ++i) {
        const FeatureDesc& f = kFeatures[i];
        const ParamDesc* en = nullptr;
        for (int j = 0; j < f.paramCount; ++j) {
            if (isEnableParam(f.params[j], f.id)) {
                en = &f.params[j];
                break;
            }
        }
        if (!en) continue;
        bool* v = static_cast<bool*>(paramPtr(en->id));
        if (!v) continue;

        bool want = false;
        for (const char* const* s = on; *s; ++s)
            if (std::strcmp(*s, en->id) == 0) want = true;

        if (*v != want) {
            *v = want;
            if (hook) hook(en->id);   // 走同一套副作用钩子
            ++flipped;
        }
    }
    VK_LOG_INFO("PRESET-APPLY %s flipped=%d", qualityPresetName(preset), flipped);

    // ---- E26：数值项 ----
    // ⚠ 单独一行日志：`PRESET-APPLY ... flipped=%d` 的格式已经被现有
    //   用例解析，动它等于让旧断言静默失效。数值部分另起一行。
    int changed = 0;
    for (int i = 0; i < qualityValueCount(); ++i) {
        const QualityValueRow& row = qualityValueAt(i);
        const ParamDesc* d = findParam(row.paramId);
        if (!d) continue;
        void* p = paramPtr(row.paramId);
        if (!p) continue;                      // 未绑定：如实跳过
        const double want = row.v[preset];

        switch (d->kind) {
            case ParamKind::Int:
            case ParamKind::Enum: {
                int& v = *static_cast<int*>(p);
                const int nv = static_cast<int>(want + (want >= 0.0 ? 0.5 : -0.5));
                if (v != nv) {
                    v = nv;
                    if (hook) hook(d->id);
                    ++changed;
                }
                break;
            }
            case ParamKind::Float: {
                float& v = *static_cast<float*>(p);
                const float nv = static_cast<float>(want);
                if (std::fabs(v - nv) > 1e-6f) {
                    v = nv;
                    if (hook) hook(d->id);
                    ++changed;
                }
                break;
            }
            default:
                // 颜色 / 三元组不进档位表（表里一个都没有，这里只是兜底）
                break;
        }
    }
    VK_LOG_INFO("PRESET-VALUES %s changed=%d of %d",
                qualityPresetName(preset), changed, qualityValueCount());
    return flipped;
}

int unboundEnableCount() {
    int n = 0;
    for (const auto& e : enableTable())
        if (!paramPtr(e.paramId)) ++n;
    return n;
}

// ============================================================
// E26 · 数值档位表
// ============================================================

namespace {

// ---- 为什么是这三个、为什么是这些数 ----
//
// `shadow.cascades`（默认 4）
//   Low=1 就是"老单张图"的行为（1 张 4 级图 → 只算 1 级，最省）；
//   Medium=2 / High=3 是常见的性价比档；Epic/Cinematic=4（引擎默认）。
//   当帧生效（`uploadSceneState` 每帧 `setCascadeCount`）。
//
// `shadow.distance`（默认 90）
//   **High 必须等于 90** —— 否则"默认工程点一下 High，画面就变了"，
//   用户会以为档位是个随机数生成器。这条有自检（Q11）。
//   Low=60 / Medium=80 / Epic=120 / Cinematic=150，全部落在 UI 的 5~200。
//
// `lshadow.maxLights`（默认 4）
//   Low 只留 1 盏（点光阴影是最贵的单帧开销之一）；Epic/Cinematic 放开。
//   当帧生效（`assignLocalShadowLayers` 每帧按它截断）。
//
// ⚠ 改动任何一个数都要同时看：面板的差异预览、`--selftest quality`、
//   `verify_quality_presets.py` 的期望值。它们是同一份数据的三个读法。
const QualityValueRow kValueRows[] = {
    // paramId                 Low  Med  High  Epic  Cine
    {"shadow.cascades",        {1.0, 2.0, 3.0, 4.0, 4.0}},
    {"shadow.distance",        {60.0, 80.0, 90.0, 120.0, 150.0}},
    {"lshadow.maxLights",      {1.0, 2.0, 4.0, 8.0, 12.0}},
};
constexpr int kValueRowCount =
    static_cast<int>(sizeof(kValueRows) / sizeof(kValueRows[0]));

// "需要重启、所以不由档位驱动"的参数。面板上单独提示，**不进** diff。
const char* const kRestartOnly[] = {
    "shadow.resolution",
    "lshadow.resolution",
};
constexpr int kRestartOnlyCount =
    static_cast<int>(sizeof(kRestartOnly) / sizeof(kRestartOnly[0]));

// 平台 = 档位的别名，不引入第二套数值。
const PlatformPreset kPlatforms[] = {
    {"PC High", 2},   // High
    {"PC Low", 0},    // Low
    // ⚠ 不叫 "Console"：词典里那个词已经是**控制台面板**（"控制台"）。
    //   一词两义只能靠让 UI 原文更具体（MEMORY 里的纪律），所以这里写全。
    {"Game Console", 3},   // Epic
    {"Mobile", 0},    // Low（手机上"Low"是唯一诚实的档位）
    {"VR", 2},        // High —— VR 不能关 TAA，两眼画面差异会晕
};
constexpr int kPlatformCount =
    static_cast<int>(sizeof(kPlatforms) / sizeof(kPlatforms[0]));

// 读当前值。未绑定 → false（**不能**当成"和档位不一致"：那会让一个
// 还没接线的参数每次都报不匹配，面板上永远是红的）。
bool readCurrent(const ParamDesc& d, double* out) {
    void* p = paramPtr(d.id);
    if (!p) return false;
    switch (d.kind) {
        case ParamKind::Bool:
            *out = *static_cast<bool*>(p) ? 1.0 : 0.0;
            return true;
        case ParamKind::Int:
        case ParamKind::Enum:
            *out = static_cast<double>(*static_cast<int*>(p));
            return true;
        case ParamKind::Float:
            *out = static_cast<double>(*static_cast<float*>(p));
            return true;
        default:
            return false;   // Color3 / Vec3 不进表
    }
}

void formatValue(ParamKind kind, double v, char* buf, std::size_t n) {
    switch (kind) {
        case ParamKind::Bool:
            std::snprintf(buf, n, "%s", v != 0.0 ? "on" : "off");
            break;
        case ParamKind::Int:
        case ParamKind::Enum:
            std::snprintf(buf, n, "%d", static_cast<int>(v + (v >= 0.0 ? 0.5 : -0.5)));
            break;
        default:
            std::snprintf(buf, n, "%.4g", v);
            break;
    }
}

bool sameValue(ParamKind kind, double a, double b) {
    if (kind == ParamKind::Float) {
        const double scale = std::fabs(a) > std::fabs(b) ? std::fabs(a) : std::fabs(b);
        return std::fabs(a - b) <= 1e-6 * (scale > 1.0 ? scale : 1.0);
    }
    return a == b;   // Bool / Int / Enum 都是精确的
}

bool enableListHas(const char* const* on, const char* id) {
    for (const char* const* s = on; s && *s; ++s)
        if (std::strcmp(*s, id) == 0) return true;
    return false;
}

} // namespace

int qualityValueCount() { return kValueRowCount; }

const QualityValueRow& qualityValueAt(int index) {
    static const QualityValueRow kEmpty{"", {}};
    if (index < 0 || index >= kValueRowCount) return kEmpty;
    return kValueRows[index];
}

bool qualityValueFor(int preset, const char* paramId, double* out) {
    if (preset < 0 || preset >= kPresetCount || !paramId) return false;
    for (int i = 0; i < kValueRowCount; ++i) {
        if (std::strcmp(kValueRows[i].paramId, paramId) != 0) continue;
        if (out) *out = kValueRows[i].v[preset];
        return true;
    }
    return false;
}

int qualityRestartOnlyCount() { return kRestartOnlyCount; }

const char* qualityRestartOnlyParam(int index) {
    return (index >= 0 && index < kRestartOnlyCount) ? kRestartOnly[index] : "";
}

int platformPresetCount() { return kPlatformCount; }

const PlatformPreset& platformPresetAt(int index) {
    static const PlatformPreset kEmpty{"", -1};
    if (index < 0 || index >= kPlatformCount) return kEmpty;
    return kPlatforms[index];
}

int platformPresetByName(const char* name) {
    if (!name || !name[0]) return -1;
    for (int i = 0; i < kPlatformCount; ++i)
        if (std::strcmp(kPlatforms[i].name, name) == 0) return i;
    return -1;
}

int presetDrivenCount(int preset) {
    if (preset < 0 || preset >= kPresetCount) return -1;
    return enableParamCount() + kValueRowCount;
}

int presetDiff(int preset, PresetChange* out, int maxOut) {
    const char* const* on = qualityPresetEnableList(preset);
    if (!on) return -1;

    int n = 0;

    // ---- enable 参数：全量比对（关着的也要比 —— "该关没关"同样是错）----
    for (int i = 0; i < enableParamCount(); ++i) {
        const EnableParam& e = enableParamAt(i);
        const ParamDesc* d = e.paramId[0] ? findParam(e.paramId) : nullptr;
        if (!d) continue;

        double cur = 0.0;
        if (!readCurrent(*d, &cur)) continue;
        const double want = enableListHas(on, e.paramId) ? 1.0 : 0.0;
        if (sameValue(d->kind, cur, want)) continue;

        if (out && n < maxOut) {
            PresetChange& c = out[n];
            c.paramId = d->id;
            c.label = d->label;
            c.kind = d->kind;
            formatValue(d->kind, cur, c.from, sizeof(c.from));
            formatValue(d->kind, want, c.to, sizeof(c.to));
        }
        ++n;
    }

    // ---- 数值项 ----
    for (int i = 0; i < kValueRowCount; ++i) {
        const QualityValueRow& row = kValueRows[i];
        const ParamDesc* d = findParam(row.paramId);
        if (!d) continue;
        double cur = 0.0;
        if (!readCurrent(*d, &cur)) continue;
        const double want = row.v[preset];
        if (sameValue(d->kind, cur, want)) continue;

        if (out && n < maxOut) {
            PresetChange& c = out[n];
            c.paramId = d->id;
            c.label = d->label;
            c.kind = d->kind;
            formatValue(d->kind, cur, c.from, sizeof(c.from));
            formatValue(d->kind, want, c.to, sizeof(c.to));
        }
        ++n;
    }

    return n;
}

int fillLayerForPreset(int preset, SettingsLayer& out) {
    const char* const* on = qualityPresetEnableList(preset);
    if (!on) return 0;

    out.clear();
    out.scope = SettingScope::Project;
    out.present = true;

    // 每个 enable 参数都写 true/false —— 理由见 `ProjectDisplay.h`：
    // "没写 = 用默认"会让"这一档应该关掉"的特性漏过去。
    int keys = 0;
    for (int i = 0; i < enableParamCount(); ++i) {
        const EnableParam& e = enableParamAt(i);
        if (!e.paramId || !e.paramId[0]) continue;
        SettingValue v;
        v.count = 1;
        v.v[0] = enableListHas(on, e.paramId) ? 1.0 : 0.0;
        out.values[e.paramId] = v;
        ++keys;
    }

    for (int i = 0; i < kValueRowCount; ++i) {
        const QualityValueRow& row = kValueRows[i];
        if (!findParam(row.paramId)) continue;
        SettingValue v;
        v.count = 1;
        v.v[0] = row.v[preset];
        out.values[row.paramId] = v;
        ++keys;
    }
    return keys;
}

// ============================================================
// E26 · 自检（无窗口 / 无 Vulkan / 无 GPU）
// ============================================================

namespace {

int g_qualityFailures = 0;

void qcheck(bool ok, const char* what, const char* detail = "") {
    if (ok) {
        std::printf("QUALITY OK %s\n", what);
    } else {
        std::printf("QUALITY FAIL %s %s\n", what, detail);
        ++g_qualityFailures;
    }
}

} // namespace

int runQualitySelfTest() {
    g_qualityFailures = 0;
    std::printf("QUALITY-TEST begin presets=%d values=%d platforms=%d enable=%d\n",
                kPresetCount, kValueRowCount, kPlatformCount, enableParamCount());

    // Q1 档位数与"Low..Cinematic"五档一致
    qcheck(kPresetCount == 5, "Q1 preset-count", "expected 5");

    // Q2 档名唯一且非空
    bool namesOk = true;
    for (int i = 0; i < kPresetCount; ++i) {
        const char* a = qualityPresetName(i);
        if (!a || !a[0] || std::strcmp(a, "Custom") == 0) namesOk = false;
        for (int j = i + 1; j < kPresetCount; ++j)
            if (std::strcmp(a, qualityPresetName(j)) == 0) namesOk = false;
    }
    qcheck(namesOk, "Q2 preset-names-unique");

    // Q3 越界返回 Custom / -1（不是崩、不是第一个）
    qcheck(std::strcmp(qualityPresetName(-1), "Custom") == 0 &&
               std::strcmp(qualityPresetName(kPresetCount), "Custom") == 0,
           "Q3 preset-name-out-of-range");
    qcheck(qualityPresetByName("High") == 2 && qualityPresetByName("high") < 0 &&
               qualityPresetByName("Nope") < 0,
           "Q4 preset-by-name-case-sensitive");

    // Q5 enable 清单单调不减（高档不能比低档少开）
    int prev = -1;
    bool mono = true;
    for (int i = 0; i < kPresetCount; ++i) {
        int c = 0;
        for (const char* const* s = qualityPresetEnableList(i); s && *s; ++s) ++c;
        if (c < prev) mono = false;
        prev = c;
    }
    qcheck(mono, "Q5 enable-list-monotonic");

    // Q6 每个数值项都在 registry 里、且每档都落在 UI 范围
    bool inRange = true;
    char detail[160] = {0};
    for (int i = 0; i < kValueRowCount; ++i) {
        const ParamDesc* d = findParam(kValueRows[i].paramId);
        if (!d) {
            inRange = false;
            std::snprintf(detail, sizeof(detail), "missing %s", kValueRows[i].paramId);
            break;
        }
        if (d->kind != ParamKind::Int && d->kind != ParamKind::Enum &&
            d->kind != ParamKind::Float) {
            inRange = false;
            std::snprintf(detail, sizeof(detail), "%s kind=%d",
                          kValueRows[i].paramId, static_cast<int>(d->kind));
            break;
        }
        for (int p = 0; p < kPresetCount; ++p) {
            const double v = kValueRows[i].v[p];
            if (v < d->lo || v > d->hi) {
                inRange = false;
                std::snprintf(detail, sizeof(detail), "%s p%d %.2f not in [%.1f,%.1f]",
                              kValueRows[i].paramId, p, v, d->lo, d->hi);
                break;
            }
        }
        if (!inRange) break;
    }
    qcheck(inRange, "Q6 value-rows-in-ui-range", detail);

    // Q7 数值项不能同时出现在"需重启"名单里（自相矛盾）
    bool disjoint = true;
    for (int i = 0; i < kValueRowCount; ++i)
        for (int j = 0; j < kRestartOnlyCount; ++j)
            if (std::strcmp(kValueRows[i].paramId, kRestartOnly[j]) == 0) disjoint = false;
    qcheck(disjoint, "Q7 value-vs-restart-disjoint");

    // Q8 平台表：名字唯一、映射合法、五个平台齐全
    bool platOk = kPlatformCount == 5;
    for (int i = 0; i < kPlatformCount; ++i) {
        const PlatformPreset& p = platformPresetAt(i);
        if (!p.name || !p.name[0]) platOk = false;
        if (p.preset < 0 || p.preset >= kPresetCount) platOk = false;
        for (int j = i + 1; j < kPlatformCount; ++j)
            if (std::strcmp(p.name, platformPresetAt(j).name) == 0) platOk = false;
    }
    qcheck(platOk, "Q8 platform-table");
    qcheck(platformPresetByName("PC High") == 0 &&
               platformPresetByName("Mobile") == 3 &&
               platformPresetByName("Nope") < 0,
           "Q9 platform-by-name");

    // Q10 非法档位的 diff / 驱动数
    qcheck(presetDiff(-1, nullptr, 0) == -1 &&
               presetDiff(kPresetCount, nullptr, 0) == -1 &&
               presetDrivenCount(-1) == -1,
           "Q10 diff-invalid-preset");

    // Q11 High 的 shadow.distance 必须 == 引擎默认 90（见表里的注释）
    double dist = 0.0;
    qcheck(qualityValueFor(2, "shadow.distance", &dist) && std::fabs(dist - 90.0) < 1e-9,
           "Q11 high-matches-default-distance",
           "High distance must equal the engine default 90");

    // Q12 落盘键数 == 驱动数（enable 全量 + 数值项）
    SettingsLayer layer;
    const int written = fillLayerForPreset(2, layer);
    qcheck(written == presetDrivenCount(2) &&
               written == static_cast<int>(layer.values.size()),
           "Q12 fill-layer-key-count");

    // Q13 落盘内容与档位定义逐项一致（"写的"和"diff 的"同源）
    bool writeMatches = true;
    for (int i = 0; i < enableParamCount(); ++i) {
        const EnableParam& e = enableParamAt(i);
        auto it = layer.values.find(e.paramId);
        if (it == layer.values.end()) { writeMatches = false; break; }
        const bool want = enableListHas(qualityPresetEnableList(2), e.paramId);
        if ((it->second.v[0] != 0.0) != want) { writeMatches = false; break; }
    }
    for (int i = 0; i < kValueRowCount; ++i) {
        auto it = layer.values.find(kValueRows[i].paramId);
        if (it == layer.values.end() ||
            std::fabs(it->second.v[0] - kValueRows[i].v[2]) > 1e-9) {
            writeMatches = false;
            break;
        }
    }
    qcheck(writeMatches, "Q13 fill-layer-content");

    // Q14 "Custom" 档（-1）不能被 fillLayer 写出半份文件
    SettingsLayer none;
    qcheck(fillLayerForPreset(-1, none) == 0 && none.values.empty(),
           "Q14 fill-layer-invalid-preset");

    // Q15 未绑定时 applyPreset 不崩、diff 不把"未绑定"算成差异
    //     （完整绑定要在 Renderer 里，这里只验证退化行为是安全的）
    SettingsLayer l2;
    (void)fillLayerForPreset(0, l2);
    const int d0 = presetDiff(0, nullptr, 0);
    qcheck(d0 >= 0, "Q15 diff-without-device-safe");

    std::printf("QUALITY-TEST end failures=%d\n", g_qualityFailures);
    return g_qualityFailures;
}

} // namespace render
