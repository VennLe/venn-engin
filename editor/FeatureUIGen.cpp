#include "FeatureUIGen.h"

#include "core/Logger.h"
#include "render/FeatureRegistry.h"
#include "render/GpuTimer.h"

#include <imgui.h>

#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>

namespace editor {

namespace {

std::function<void(const char*)> g_changeHook;
std::function<void(const char*, const void*, const void*, std::size_t)>
    g_undoHook;

// E19：一个参数占多少字节（注册表绑定的都是 POD）。
std::size_t paramBytes(render::ParamKind k) {
    switch (k) {
    case render::ParamKind::Bool:   return sizeof(bool);
    case render::ParamKind::Int:
    case render::ParamKind::Enum:   return sizeof(int);
    case render::ParamKind::Float:  return sizeof(float);
    case render::ParamKind::Color3:
    case render::ParamKind::Vec3:   return sizeof(float) * 3;
    }
    return 0;
}

// 编辑中那个控件的"改前值"暂存。同一时刻 ImGui 只可能有一个 item 处于
// 激活态（鼠标/键盘焦点是唯一的），所以一份静态缓冲够用；`g_beforeId`
// 只作交叉校验，避免帧错位时把别的参数的值当成本参数的旧值。
std::uint8_t g_before[16];
std::string g_beforeId;

// 大小写不敏感子串（搜索框用；ASCII 就够 —— 表里全是英文标签）
bool containsCI(const char* hay, const char* needle) {
    if (!hay || !needle || !needle[0]) return true;
    const size_t nl = std::strlen(needle);
    for (const char* p = hay; *p; ++p) {
        size_t i = 0;
        while (i < nl && p[i] &&
               std::tolower(static_cast<unsigned char>(p[i])) ==
                   std::tolower(static_cast<unsigned char>(needle[i])))
            ++i;
        if (i == nl) return true;
    }
    return false;
}

// drawParamControl 的内部实现：允许覆盖显示 label（特性标题行的启用
// 开关要用 FeatureDesc::name 而不是参数自己的 label）。
bool drawControl(const render::ParamDesc& p, const char* labelOverride) {
    void* ptr = render::paramPtr(p.id);
    if (!ptr) return false;   // 未绑定：启动自检会点名，这里静默
    const char* label = labelOverride ? labelOverride : p.label;

    bool changed = false;
    switch (p.kind) {
    case render::ParamKind::Bool:
        changed = ImGui::Checkbox(label, static_cast<bool*>(ptr));
        break;
    case render::ParamKind::Int:
        changed = ImGui::SliderInt(label, static_cast<int*>(ptr),
                                   static_cast<int>(p.lo),
                                   static_cast<int>(p.hi));
        break;
    case render::ParamKind::Enum: {
        int n = 0;
        while (p.enumNames && p.enumNames[n]) ++n;
        if (n <= 0) break;
        int* v = static_cast<int*>(ptr);
        int cur = *v;
        if (cur < 0 || cur >= n) cur = 0;   // 越界值只影响显示，不回写
        if (ImGui::Combo(label, &cur, p.enumNames, n)) {
            *v = cur;
            changed = true;
        }
        break;
    }
    case render::ParamKind::Float:
        // lo == hi 的参数表写法只出现在 Bool/Enum；防御一下别画出
        // 退化滑条。
        if (p.lo < p.hi) {
            changed = ImGui::SliderFloat(label, static_cast<float*>(ptr),
                                         p.lo, p.hi, "%.3f");
        } else {
            changed = ImGui::DragFloat(label, static_cast<float*>(ptr), 0.01f);
        }
        break;
    case render::ParamKind::Color3:
        changed = ImGui::ColorEdit3(label, static_cast<float*>(ptr));
        break;
    case render::ParamKind::Vec3:
        changed = ImGui::SliderFloat3(label, static_cast<float*>(ptr), p.lo,
                                      p.hi, "%.3f");
        break;
    }
    // ---- E19：参数编辑的撤销捕获 ----
    // ⚠ 必须在 `SetTooltip` **之前**：tooltip 会提交一个新的 item，
    //   之后 `IsItemActivated/IsItemDeactivatedAfterEdit` 问的就是 tooltip
    //   而不是控件了。
    const std::size_t bytes = paramBytes(p.kind);
    if (bytes > 0 && bytes <= sizeof(g_before) && g_undoHook) {
        if (ImGui::IsItemActivated()) {
            std::memcpy(g_before, ptr, bytes);
            g_beforeId = p.id;
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && g_beforeId == p.id) {
            // 值真的变了才推（拖了又拖回原位 = 没改过，不该进历史）
            if (std::memcmp(g_before, ptr, bytes) != 0)
                g_undoHook(p.id, g_before, ptr, bytes);
            g_beforeId.clear();
        }
    }

    if (p.tooltip && p.tooltip[0] && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", p.tooltip);
    if (changed && g_changeHook) g_changeHook(p.id);
    return changed;
}
// 这个参数是不是特性的"启用开关"（约定：id == "<featureId>.enabled"）。
bool isEnableParam(const render::ParamDesc& p, const char* featureId) {
    if (p.kind != render::ParamKind::Bool) return false;
    const size_t fl = std::strlen(featureId);
    return std::strncmp(p.id, featureId, fl) == 0 &&
           std::strcmp(p.id + fl, ".enabled") == 0;
}

// GpuTimer 段名 → 段下标；找不到 -1。
int gpuSectionByName(const char* name) {
    if (!name) return -1;
    for (uint32_t s = 0; s < render::GpuTimer::kSectionCount; ++s)
        if (std::strcmp(render::GpuTimer::sectionName(s), name) == 0)
            return static_cast<int>(s);
    return -1;
}

// ---- 画质预设（E09 顶栏 Preset ▾）----
// 只拨"开/关"：质量参数的档位化（半径/级联数/分辨率随档位变）是 E26
// 的数据活。每档是"明确打开的启用参数清单"，没列到的一律关 ——
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

}  // namespace

void setParamChangeHook(std::function<void(const char*)> hook) {
    g_changeHook = std::move(hook);
}

void setParamUndoHook(
    std::function<void(const char*, const void*, const void*, std::size_t)>
        hook) {
    g_undoHook = std::move(hook);
}

void emitParamChange(const char* paramId) {
    if (g_changeHook) g_changeHook(paramId);
}

bool drawParamControl(const render::ParamDesc& p) {
    ImGui::PushID(p.id);
    const bool changed = drawControl(p, nullptr);
    ImGui::PopID();
    return changed;
}

bool drawFeatureBlock(const render::FeatureDesc& f, int tierLevel,
                      const char* filter, const render::GpuTimer* gt) {
    // ---- 过滤：特性名命中 → 整块可见；否则只留 label 命中的参数 ----
    const bool filtering = filter && filter[0];
    bool nameMatch = false;
    if (filtering) {
        nameMatch = containsCI(f.name, filter) || containsCI(f.id, filter);
        bool anyParam = false;
        for (int i = 0; i < f.paramCount; ++i)
            if (containsCI(f.params[i].label, filter)) anyParam = true;
        if (!nameMatch && !anyParam) return false;
    }

    const render::ParamDesc* en = nullptr;
    for (int i = 0; i < f.paramCount; ++i)
        if (isEnableParam(f.params[i], f.id)) {
            en = &f.params[i];
            break;
        }

    ImGui::PushID(f.id);

    // ---- 标题行：[x] 名称  (R3-1) · 0.42 ms      [Reset] ----
    const bool hasAnyParam = f.paramCount > (en ? 1 : 0);
    if (en) {
        drawControl(*en, f.name);
    } else {
        ImGui::TextUnformatted(f.name);
    }
    if (f.roadmap && f.roadmap[0] && ImGui::GetItemRectMax().y > 0.0f) {
        ImGui::SameLine();
        ImGui::TextDisabled("[%s]", f.roadmap);
    }
    // GPU 成本：注册表给了段名 + 计时器可用时显示（性能意识从面板可见）。
    if (gt && gt->valid()) {
        const int sec = gpuSectionByName(f.gpu);
        if (sec >= 0) {
            const float ms = gt->lastMs(static_cast<uint32_t>(sec));
            if (ms > 0.0f) {
                ImGui::SameLine();
                ImGui::TextDisabled("%.2f ms", static_cast<double>(ms));
            }
        }
    }
    if (hasAnyParam) {
        // 右上角复位：把该特性的全部参数恢复到"绑定那一刻"的默认值。
        ImGui::SameLine();
        const float btnW = 46.0f;
        const float avail = ImGui::GetContentRegionAvail().x;
        if (avail > btnW + 8.0f)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - btnW);
        if (ImGui::SmallButton("Reset")) {
            for (int i = 0; i < f.paramCount; ++i)
                render::resetParam(f.params[i].id);
            VK_LOG_INFO("FTR-RESET %s", f.id);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("把 %s 的全部参数恢复默认", f.name);
    }

    // ---- 参数区：启用开关关着 → 收起；档位与搜索过滤 ----
    const bool enabled = !en || !render::paramPtr(en->id) ||
                         *static_cast<bool*>(render::paramPtr(en->id));
    if (enabled) {
        ImGui::Indent(10.0f);
        for (int i = 0; i < f.paramCount; ++i) {
            const render::ParamDesc& p = f.params[i];
            if (en && &p == en) continue;
            if (static_cast<int>(p.tier) > tierLevel) continue;
            if (filtering && !nameMatch && !containsCI(p.label, filter))
                continue;
            drawControl(p, nullptr);
        }
        ImGui::Unindent(10.0f);
    }

    ImGui::PopID();
    return true;
}

void applyQualityPreset(int preset) {
    if (preset < 0 || preset >= qualityPresetCount()) return;
    const char* const* on = kPresets[preset].on;
    int flipped = 0;
    for (int i = 0; i < render::kFeatureCount; ++i) {
        const render::FeatureDesc& f = render::kFeatures[i];
        const render::ParamDesc* en = nullptr;
        for (int j = 0; j < f.paramCount; ++j)
            if (isEnableParam(f.params[j], f.id)) {
                en = &f.params[j];
                break;
            }
        if (!en) continue;
        bool* v = static_cast<bool*>(render::paramPtr(en->id));
        if (!v) continue;
        bool want = false;
        for (const char* const* s = on; *s; ++s)
            if (std::strcmp(*s, en->id) == 0) want = true;
        if (*v != want) {
            *v = want;
            if (g_changeHook) g_changeHook(en->id);   // 走同一套副作用钩子
            ++flipped;
        }
    }
    VK_LOG_INFO("PRESET-APPLY %s flipped=%d", kPresets[preset].name, flipped);
}

const char* qualityPresetName(int preset) {
    return (preset >= 0 && preset < qualityPresetCount())
               ? kPresets[preset].name
               : "Custom";
}

int qualityPresetCount() {
    return static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));
}

}  // namespace editor
