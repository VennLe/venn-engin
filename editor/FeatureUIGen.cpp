#include "FeatureUIGen.h"

#include "Localization.h"
#include "core/Logger.h"
#include "render/FeatureRegistry.h"
#include "render/GpuTimer.h"
// 2026-10-19（启动器 M4）：画质预设表**下沉**到 engine/render，这里只转发。
// 为什么：启动器要往工程的 `config/render.json` 写"这一档打开了哪些开关"，
// 而 `launcher → engine`（不能反过来依赖 editor 应用层）。表留在两份
// 必然漂移 —— 那会让"启动器选 High、编辑器显示 Epic"这种情况没有任何报错。
#include "render/QualityPresets.h"

#include <imgui.h>

#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>

namespace editor {

namespace {

using render::isEnableParam;

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
    // ⚠ 三件事挤在一行，都不是可选的：
    //   ① TRL 而不是 TR —— 这些 label 有的带 `##fx` 之类的稳定 ID 后缀；
    //   ② PushID(p.id) —— 中文译名可能重名（"Strength" 与 "Intensity" 都
    //      可能译成"强度"），而 ImGui 的控件 ID 就是 label 哈希 → 重名会
    //      撞 ID（症状：拖一个滑条另一个跟着动，或值显示串台）。用参数 id
    //      当 ID 之后，译名怎么改都不会撞；
    //   ③ Push/Pop 必须夹住整个 switch —— 下面 `IsItemActivated` 等
    //      "上一个 item"的查询都发生在同一个 ID 栈层级上。
    ImGui::PushID(p.id);
    const char* label = TRL(labelOverride ? labelOverride : p.label);

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
        ImGui::SetTooltip(TR("%s"), p.tooltip);
    if (changed && g_changeHook) g_changeHook(p.id);
    ImGui::PopID();       // 对应函数开头的 PushID(p.id)
    return changed;
}
// GpuTimer 段名 → 段下标；找不到 -1。
int gpuSectionByName(const char* name) {
    if (!name) return -1;
    for (uint32_t s = 0; s < render::GpuTimer::kSectionCount; ++s)
        if (std::strcmp(render::GpuTimer::sectionName(s), name) == 0)
            return static_cast<int>(s);
    return -1;
}

// ---- 画质预设表（E09 顶栏 Preset ▾）已搬到 engine/render/QualityPresets.cpp ----
// 2026-10-19：表下沉见那里的文件头。这里**一个字都不留** —— 两份表必然
// 漂移，而"启动器写 High / 编辑器显示 Epic"这种漂移两边都不报错。

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
    // ID 隔离已经由 drawControl 内部的 PushID(p.id) 办了，这里不再套一层
    // （两层同 id 不报错，但会让以后"ID 到底哪来的"多一个假线索）。
    return drawControl(p, nullptr);
}

bool drawFeatureBlock(const render::FeatureDesc& f, int tierLevel,
                      const char* filter, const render::GpuTimer* gt) {
    // ---- 过滤：特性名命中 → 整块可见；否则只留 label 命中的参数 ----
    // ⚠ 中文模式下必须**同时匹配英文原文与中文译名**：用户看到的只有中文，
    //   只匹配英文等于"这个搜索框在中文界面里搜不到东西"。
    const bool filtering = filter && filter[0];
    bool nameMatch = false;
    if (filtering) {
        nameMatch = containsCI(f.name, filter) || containsCI(f.id, filter) ||
                    containsCI(TR(f.name), filter);
        bool anyParam = false;
        for (int i = 0; i < f.paramCount; ++i)
            if (containsCI(f.params[i].label, filter) ||
                containsCI(TR(f.params[i].label), filter))
                anyParam = true;
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
        ImGui::TextUnformatted(TR(f.name));
    }
    if (f.roadmap && f.roadmap[0] && ImGui::GetItemRectMax().y > 0.0f) {
        ImGui::SameLine();
        ImGui::TextDisabled(TR("[%s]"), f.roadmap);
    }
    // GPU 成本：注册表给了段名 + 计时器可用时显示（性能意识从面板可见）。
    if (gt && gt->valid()) {
        const int sec = gpuSectionByName(f.gpu);
        if (sec >= 0) {
            const float ms = gt->lastMs(static_cast<uint32_t>(sec));
            if (ms > 0.0f) {
                ImGui::SameLine();
                ImGui::TextDisabled(TR("%.2f ms"), static_cast<double>(ms));
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
        if (ImGui::SmallButton(TR("Reset"))) {
            for (int i = 0; i < f.paramCount; ++i)
                render::resetParam(f.params[i].id);
            VK_LOG_INFO("FTR-RESET %s", f.id);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(TR("Reset all parameters of %s to defaults"),
                              TR(f.name));
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
            if (filtering && !nameMatch && !containsCI(p.label, filter) &&
                !containsCI(TR(p.label), filter))
                continue;
            drawControl(p, nullptr);
        }
        ImGui::Unindent(10.0f);
    }

    ImGui::PopID();
    return true;
}

void applyQualityPreset(int preset) {
    // 表与拨动逻辑都在 engine/render —— 这里只把"改了参数"的通知接上，
    // 保证编辑器侧的 FTR-UI 日志 / 副作用钩子照旧触发。
    render::applyPreset(preset, [](const char* id) {
        if (g_changeHook) g_changeHook(id);
    });
}

const char* qualityPresetName(int preset) {
    return render::qualityPresetName(preset);
}

int qualityPresetCount() {
    return render::qualityPresetCount();
}

}  // namespace editor
