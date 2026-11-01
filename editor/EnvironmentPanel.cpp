#include "EnvironmentPanel.h"

#include "Localization.h"
#include "PanelDock.h"        // 视口标签页托管
#include "render/Renderer.h"
#include "render/SkyParams.h"
#include "ui/DebugRects.h"    // ui::logRect

#include "core/Logger.h"

#include <imgui.h>

#include <cfloat>
#include <cmath>

namespace editor {

namespace {

// 参数说明。**不做 SameLine**，改成悬停提示。
//
// 第一版是把说明塞在控件右边（`SameLine + TextDisabled`），面板一进
// 视口标签页就被内容区宽度截断 —— 每行只剩 "(top of the dome, t"。
// 悬停提示不占布局宽度，信息量一点没少，还顺带让面板紧凑了一半。
// ⚠ 必须在**控件之后**立刻调用：IsItemHovered 看的是上一个 item。
void paramHint(const char* hint) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("%s", TR(hint));
    }
}

} // namespace

void EnvironmentPanel::draw(render::SkyParams& sky, render::Renderer& renderer) {
    // ---- 窗口本身（与 Physics / Particles / Quality 同一套托管方式）----
    // 视口标签页里时由 EditorApp 钉进内容区（applyDockHint 返回 true），
    // 此时不能自己设尺寸/位置，也不能给 p_open（那是标题栏的 ×）。
    ImGuiWindowFlags wf = 0;
    const bool docked = applyDockHint(wf);
    if (!docked) {
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowSizeConstraints(ImVec2(360.0f, 420.0f),
                                            ImVec2(FLT_MAX, FLT_MAX));
        // 默认高一点：三个折叠段全开时内容有 700+ px，给窄了就要一直滚，
        // 而"一次看全"正是这块面板存在的理由。
        ImGui::SetNextWindowSize(ImVec2(430.0f, 780.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.04f,
                                       io.DisplaySize.y * 0.08f),
                                ImGuiCond_FirstUseEver);
    }

    bool open = true;
    if (!ImGui::Begin(trId("Sky & Atmosphere", "panel_environment"),
                      docked ? nullptr : &open, wf)) {
        ImGui::End();
        return;
    }
    ui::logRect("SKY-RECT", ImGui::GetWindowPos(),
                ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                       ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    // 环境还没建好（IBL 烘焙失败 / 启动早期）时不该让人对着空面板拖滑条 ——
    // 拖了也不会生效，还会让人以为"引擎坏了"。
    if (!renderer.environmentReady()) {
        ImGui::TextDisabled("%s", TR("Environment not ready (IBL build failed)."));
        ImGui::End();
        return;
    }

    // 每帧从**场景**读真值、改完写回（见头文件"真值是 sky"那段）。
    render::SkyParams p = sky;

    // ============================================================
    // 预设
    // ============================================================
    ImGui::TextUnformatted(TR("Weather presets"));
    ImGui::Spacing();

    // 判"当前是不是这套预设"：只比几个关键量（真正的逐字节比较在
    // Renderer::setSkyParams 里做）。这里的目的只是给按钮加高亮，
    // 比全字段宽容 —— 用户改了一个云色不该让"晴天"按钮看起来失效。
    auto looksLike = [&](const render::SkyParams& a) {
        return std::abs(a.cloudCoverage - p.cloudCoverage) < 1e-3f &&
               std::abs(a.starAmount - p.starAmount) < 1e-3f &&
               std::abs(a.sunIntensity - p.sunIntensity) < 1e-3f &&
               glm::length(a.zenith - p.zenith) < 1e-3f;
    };

    const render::SkyParams presets[4] = {
        render::SkyParams::clearDay(),
        render::SkyParams::overcast(),
        render::SkyParams::night(),
        render::SkyParams::dusk(),
    };
    const char* presetIds[4] = {"day", "overcast", "night", "dusk"};
    const char* presetNames[4] = {TR("Clear day"), TR("Overcast"),
                                  TR("Starry night"), TR("Dusk")};

    // 两行两列：四个按钮横排会超出 430 px 的默认宽度。
    for (int i = 0; i < 4; ++i) {
        ImGui::PushID(presetIds[i]);
        const bool active = looksLike(presets[i]);
        if (active)
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
        const bool pressed = ImGui::Button(presetNames[i], ImVec2(198.0f, 0.0f));
        if (active) ImGui::PopStyleColor();
        if (pressed) {
            p = presets[i];
            m_lastPreset = i;
            VK_LOG_INFO("SKY-PANEL-PRESET name=%s", presetIds[i]);
        }
        ImGui::PopID();
        if (i % 2 == 0) ImGui::SameLine();
    }

    ImGui::Spacing();
    ImGui::Separator();

    // ============================================================
    // ① 天空
    // ============================================================
    if (ImGui::CollapsingHeader(TR("Sky"), &m_openSky,
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent(8.0f);

        ImGui::ColorEdit3(TR("Zenith"), &p.zenith[0],
                          ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
        paramHint("top of the dome, the most saturated part");

        ImGui::ColorEdit3(TR("Horizon"), &p.horizon[0],
                          ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
        paramHint("the pale band anime skies are known for");

        ImGui::ColorEdit3(TR("Ground"), &p.ground[0],
                          ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
        paramHint("what you see looking down past the horizon");

        ImGui::SliderFloat(TR("Exposure"), &p.exposure, 0.2f, 3.0f, "%.2f");
        paramHint("scales the whole sky, clouds and stars included");

        ImGui::Spacing();
        ImGui::TextUnformatted(TR("Sun / Moon"));

        // 太阳用"高度角 + 方位角"而不是三维方向 —— 让人拖一个向量
        // 在这块面板里既没有手柄也没有参考系。
        float az = 0.0f, el = 0.0f;
        render::SkyParams::anglesFromDirection(p.sunDirection, az, el);
        bool sunMoved = false;
        sunMoved |= ImGui::SliderFloat(TR("Azimuth"), &az, 0.0f, 360.0f,
                                       "%.0f deg");
        paramHint("0 deg = +X, counter-clockwise seen from above");
        sunMoved |= ImGui::SliderFloat(TR("Elevation"), &el, -10.0f, 90.0f,
                                       "%.0f deg");
        paramHint("0 deg = horizon, 90 deg = straight up");
        if (sunMoved) {
            p.sunDirection = render::SkyParams::directionFromAngles(az, el);
        }

        ImGui::SliderFloat(TR("Intensity"), &p.sunIntensity, 0.0f, 8.0f, "%.2f");
        paramHint("0 removes the disk entirely");
        ImGui::SliderFloat(TR("Angular size"), &p.sunAngularRadius, 0.002f,
                           0.12f, "%.3f rad");
        paramHint("bigger = a softer, storybook sun");
        ImGui::SliderFloat(TR("Halo"), &p.horizonGlow, 0.0f, 2.0f, "%.2f");
        paramHint("how far the glare bleeds into the sky");

        ImGui::Unindent(8.0f);
    }

    // ============================================================
    // ② 云层
    // ============================================================
    if (ImGui::CollapsingHeader(TR("Clouds"), &m_openCloud,
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent(8.0f);

        ImGui::SliderFloat(TR("Coverage"), &p.cloudCoverage, 0.0f, 1.0f, "%.2f");
        paramHint("0 = clear sky, 1 = overcast");

        ImGui::SliderFloat(TR("Thickness"), &p.cloudThickness, 0.0f, 1.0f,
                           "%.2f");
        paramHint("opacity; low values read as thin cirrus");

        ImGui::ColorEdit3(TR("Cloud colour"), &p.cloudColor[0],
                          ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);

        ImGui::SliderFloat(TR("Scale"), &p.cloudScale, 0.2f, 4.0f, "%.2f");
        paramHint("small = huge banks, large = scattered puffs");

        ImGui::SliderFloat(TR("Edge sharpness"), &p.cloudEdgeSharpness, 0.0f,
                           1.0f, "%.2f");
        paramHint("0 = watercolour, 1 = paper cut-out");

        ImGui::SliderFloat(TR("Shadow"), &p.cloudShadow, 0.0f, 1.0f, "%.2f");
        paramHint("how dark the side facing away from the sun gets");

        ImGui::SliderFloat(TR("Wind"), &p.cloudSpeed, 0.0f, 0.5f, "%.3f");
        paramHint("drift speed; 0 freezes the sky");

        ImGui::Unindent(8.0f);
    }

    // ============================================================
    // ③ 星空
    // ============================================================
    if (ImGui::CollapsingHeader(TR("Stars"), &m_openStars,
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent(8.0f);

        ImGui::ColorEdit3(TR("Star colour"), &p.starColor[0],
                          ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);

        ImGui::SliderFloat(TR("Amount"), &p.starAmount, 0.0f, 1.0f, "%.2f");
        paramHint("star count; multiplies with brightness below");

        ImGui::SliderFloat(TR("Brightness"), &p.starBrightness, 0.0f, 1.0f,
                           "%.2f");
        paramHint("turn this up independently to keep the sky dark");

        ImGui::Unindent(8.0f);
    }

    ImGui::Spacing();
    ImGui::Separator();

    // ---- 状态行 ----
    // "待重烘"是**正常**状态（拖动时会看到它闪），不是错误 —— 所以用灰色
    // 而不是警告色。它存在的意义是回答"我拖了滑条，反射为什么慢半拍"。
    if (renderer.skyRebuildPending()) {
        ImGui::TextDisabled("%s", TR("IBL re-baking..."));
    } else {
        ImGui::TextDisabled("%s", TR("Applied."));
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(TR("Rebuild IBL now"))) {
        // 直接把这次编辑推给渲染器并立刻重烘 —— 正常路径是"下一帧
        // EditorApp 统一推"，这里只是让"改完想马上看到反射"不用等 6 帧。
        renderer.setSkyParams(p);
        renderer.rebuildSkyNow();
        VK_LOG_INFO("SKY-PANEL-APPLY manual");
    }

    // 每帧最后统一写回**场景**。EditorApp::onUpdate 会把场景里这份推给
    // Renderer（那边内部按字节比较，没改动不会触发任何重建）。
    sky = p;

    ImGui::End();
}

} // namespace editor
