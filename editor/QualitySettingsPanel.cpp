#include "QualitySettingsPanel.h"

#include "FeatureUIGen.h"
#include "PanelDock.h"        // 视口标签页托管（见 PanelDock.h）
#include "Localization.h"
#include "PanelRegistry.h"       // panelOpenRef
#include "ui/DebugRects.h"

#include "core/Logger.h"
#include "render/FeatureRegistry.h"
#include "render/QualityPresets.h"
#include "render/RenderSettingsIO.h"

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace editor {

namespace {

// ---- 读 registry 里的当前值 ----
// 未绑定返回 fallback。**不能**因为未绑定就当成 0 —— 那会让"还没接线
// 的特性"在快照里显示成"被关了"，而实际上它只是没接。
bool readBool(const char* id, bool fallback = false) {
    void* p = render::paramPtr(id);
    return p ? *static_cast<bool*>(p) : fallback;
}

int readInt(const char* id, int fallback = 0) {
    void* p = render::paramPtr(id);
    return p ? *static_cast<int*>(p) : fallback;
}

float readFloat(const char* id, float fallback = 0.0f) {
    void* p = render::paramPtr(id);
    return p ? *static_cast<float*>(p) : fallback;
}

void tagItem(const char* tag) {
    ui::logRect(tag, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

} // namespace

int QualitySettingsPanel::detectCurrentPreset() {
    // 有未绑定的 enable 参数时 diff 会**跳过**它们 → "差 0 项"可能只是
    // "没比到"。这种情况一律报 Custom，不猜。
    if (render::unboundEnableCount() > 0) return -1;
    for (int i = 0; i < render::qualityPresetCount(); ++i)
        if (render::presetDiff(i, nullptr, 0) == 0) return i;
    return -1;
}

int QualitySettingsPanel::checkSelected(bool logResult) {
    // ⚠ 与面板上的"差异预览"是**同一个函数**。两处各写一套比较逻辑，
    //   迟早会一个说"差 0 项"另一个说"差 3 项"，而画面只相信其中一个。
    static constexpr int kMax = 64;
    render::PresetChange changes[kMax];
    const int n = render::presetDiff(m_selected, changes, kMax);
    m_lastMismatch = n < 0 ? 0 : n;

    if (logResult) {
        VK_LOG_INFO("QUALITY-UI check preset=%s mismatches=%d driven=%d unbound=%d",
                    render::qualityPresetName(m_selected), m_lastMismatch,
                    render::presetDrivenCount(m_selected),
                    render::unboundEnableCount());
    }
    return m_lastMismatch;
}

void QualitySettingsPanel::applySelected() {
    editor::applyQualityPreset(m_selected);   // 走 FTR-UI / 副作用钩子
    const int mismatch = checkSelected(true);

    // 一行快照：验收脚本靠它断言"点 Low 之后 SSR/SSAO 真的变成 0"。
    // 只靠 `PRESET-APPLY flipped=14` 证明不了**具体哪个**参数变成了什么。
    VK_LOG_INFO("QUALITY-SNAPSHOT preset=%s ssao=%d ssr=%d taa=%d cascades=%d "
                "dist=%.0f lights=%d",
                render::qualityPresetName(m_selected),
                readBool("ssao.enabled") ? 1 : 0, readBool("ssr.enabled") ? 1 : 0,
                readBool("taa.enabled") ? 1 : 0, readInt("shadow.cascades"),
                static_cast<double>(readFloat("shadow.distance")),
                readInt("lshadow.maxLights"));

    std::snprintf(m_status, sizeof(m_status),
                  TR("Applied %s (mismatches=%d)"),
                  render::qualityPresetName(m_selected), mismatch);
    VK_LOG_INFO("QUALITY-UI apply preset=%s mismatches=%d",
                render::qualityPresetName(m_selected), mismatch);
}

void QualitySettingsPanel::saveSelected() {
    // ⚠ 与 E03 同一条守卫：自动化跑不写默认路径（否则一次 CI 会话会在
    //   仓库里留下一个 json，把所有像素基线悄悄换掉）。显式给了
    //   `MYVK_RENDER_SETTINGS` 才写 —— 那是脚本自己要的。
    if (render::renderSettingsAutomationRun() &&
        !render::projectSettingsPathExplicit()) {
        std::snprintf(m_status, sizeof(m_status), "%s", TR("Save refused (automation run)"));
        VK_LOG_WARN("QUALITY-UI save-refused reason=automation "
                    "(set MYVK_RENDER_SETTINGS to write)");
        return;
    }

    render::SettingsLayer layer;
    const int keys = render::fillLayerForPreset(m_selected, layer);
    const std::string path = render::projectSettingsPath();
    if (keys <= 0 || !render::saveLayerFile(layer, path)) {
        std::snprintf(m_status, sizeof(m_status), "%s", TR("Save failed"));
        VK_LOG_WARN("QUALITY-UI save-failed preset=%s path=%s keys=%d",
                    render::qualityPresetName(m_selected), path.c_str(), keys);
        return;
    }

    m_savedOnce = true;
    std::snprintf(m_status, sizeof(m_status), TR("Saved %d keys"), keys);
    VK_LOG_INFO("QUALITY-UI save preset=%s path=%s keys=%d",
                render::qualityPresetName(m_selected), path.c_str(), keys);
}

void QualitySettingsPanel::draw() {
    bool& open = panelOpenRef("quality");
    if (!open) return;

    // 2026-10-21：视口标签页的一页（见 PanelDock.h）
    ImGuiWindowFlags wf = 0;
    const bool docked = applyDockHint(wf);
    if (!docked) {
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowSizeConstraints(ImVec2(480.0f, 360.0f),
                                            ImVec2(FLT_MAX, FLT_MAX));
        ImGui::SetNextWindowSize(ImVec2(560.0f, 520.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.28f,
                                       io.DisplaySize.y * 0.12f),
                                ImGuiCond_FirstUseEver);
    }

    if (!ImGui::Begin(trId("Quality Settings", "panel_quality"),
                      docked ? nullptr : &open, wf)) {
        ImGui::End();
        return;
    }
    ui::logRect("QUAL-RECT", ImGui::GetWindowPos(),
                ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                       ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    // ---------------------------------------------------------- 首帧：反推当前档位
    // 必须在画单选按钮**之前**：否则重开工程时面板先显示"High"，
    // 而实际值是 Epic —— 用户看到的第一眼就是错的。
    if (!m_initialised) {
        m_initialised = true;
        const int cur = detectCurrentPreset();
        if (cur >= 0) {
            m_selected = cur;
            for (int i = 0; i < render::platformPresetCount(); ++i) {
                if (render::platformPresetAt(i).preset == cur) {
                    m_platform = i;
                    break;
                }
            }
            VK_LOG_INFO("QUALITY-UI detect preset=%s",
                        render::qualityPresetName(cur));
        } else {
            VK_LOG_INFO("QUALITY-UI detect preset=Custom");
        }
        checkSelected(true);
    }

    // ---------------------------------------------------------- 档位单选
    ImGui::SeparatorText(TR("Preset"));
    const int presetCount = render::qualityPresetCount();
    for (int i = 0; i < presetCount; ++i) {
        // 每行 3 个：实测 5 个一行时第 5 个会被窗口右缘裁掉 ——
        // 而**被裁掉的控件 GetItemRect 照样有值**，脚本点下去点到别处
        // （B2 就是这么红的）。
        if (i > 0 && (i % 3) != 0) ImGui::SameLine();
        ImGui::PushID(i);
        // ⚠ 档名后面拼 " quality" 再查词典：`"Cinematic"` 这个词在词典里
        //   已经被相机那一块占成"镜头"（运镜/景深语义）。直接复用会显示成
        //   "镜头" —— 这正是"一词两义靠让 UI 原文更具体"那条纪律的又一例。
        const std::string label =
            std::string(render::qualityPresetName(i)) + " quality";
        if (ImGui::RadioButton(TR(label.c_str()), &m_selected, i)) {
            checkSelected(false);
            VK_LOG_INFO("QUALITY-UI select preset=%s", render::qualityPresetName(i));
        }
        ImGui::PopID();
        const std::string tag = "QUAL-PRESET-" + std::to_string(i);
        tagItem(tag.c_str());
    }

    // ---------------------------------------------------------- 平台预设
    // "平台 = 档位的别名"（不引入第二套数值表）。选平台只是把上面的
    // 单选拨到对应档，用户仍然可以再手动改档 —— 两者不互斥。
    const int platCount = render::platformPresetCount();
    const char* platItems[8];
    for (int i = 0; i < platCount && i < 8; ++i)
        platItems[i] = TR(render::platformPresetAt(i).name);
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::Combo(TR("Platform"), &m_platform, platItems, platCount)) {
        const int mapped = render::platformPresetAt(m_platform).preset;
        if (mapped >= 0 && mapped < presetCount) m_selected = mapped;
        checkSelected(false);
        VK_LOG_INFO("QUALITY-UI platform=%s preset=%s",
                    render::platformPresetAt(m_platform).name,
                    render::qualityPresetName(m_selected));
    }
    tagItem("QUAL-PLATFORM");

    // ---------------------------------------------------------- 动作按钮
    if (ImGui::Button(TR("Apply"))) applySelected();
    tagItem("QUAL-APPLY");

    ImGui::SameLine();
    if (ImGui::Button(TR("Check"))) {
        const int n = checkSelected(true);
        std::snprintf(m_status, sizeof(m_status), TR("Checked: %d mismatch(es)"), n);
    }
    tagItem("QUAL-CHECK");

    ImGui::SameLine();
    if (ImGui::Button(TR("Save to config/render.json"))) saveSelected();
    tagItem("QUAL-SAVE");

    // ---------------------------------------------------------- 差异预览
    ImGui::SeparatorText(TR("Changes from current values"));

    static constexpr int kMax = 64;
    render::PresetChange changes[kMax];
    const int n = render::presetDiff(m_selected, changes, kMax);

    if (n <= 0) {
        ImGui::TextDisabled(TR("No differences — already matches this preset."));
    } else {
        // 只画前 12 条：面板不是表格控件，超过 12 条时刷新按钮证明
        // "还有更多"，而不是让人滚三屏（真正想看全量的用 Check 的日志）。
        const int shown = n < 12 ? n : 12;
        for (int i = 0; i < shown; ++i) {
            const render::PresetChange& c = changes[i];
            ImGui::Text("%s", TR(c.label));
            ImGui::SameLine(250.0f);
            ImGui::TextDisabled("%s  ->  %s", c.from, c.to);
            const std::string tag = "QUAL-DIFF-" + std::to_string(i);
            tagItem(tag.c_str());
        }
        if (n > shown)
            ImGui::TextDisabled(TR("... and %d more (see log)"), n - shown);
    }

    // ---------------------------------------------------------- 状态
    ImGui::Separator();
    ImGui::TextWrapped(TR("Preset drives %d parameters (%d mismatched now)"),
                       render::presetDrivenCount(m_selected), m_lastMismatch);
    tagItem("QUAL-STATUS");

    const int unbound = render::unboundEnableCount();
    if (unbound > 0)
        ImGui::TextDisabled(TR("Unbound parameters: %d (not wired yet)"), unbound);

    // 如实说明"这些不随档位变、要重启"——否则用户会以为切档没生效。
    ImGui::Spacing();
    ImGui::TextDisabled(TR("Not driven by presets (need restart):"));
    for (int i = 0; i < render::qualityRestartOnlyCount(); ++i) {
        const char* id = render::qualityRestartOnlyParam(i);
        const render::ParamDesc* d = render::findParam(id);
        const int v = readInt(id, -1);
        ImGui::BulletText("%s = %d", id, v);
        if (d && d->tooltip && d->tooltip[0] && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", TR(d->tooltip));
    }

    if (m_status[0]) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", m_status);
    }

    ImGui::End();
}

} // namespace editor
