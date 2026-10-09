#include "ViewportPanel.h"

#include "DebugRects.h"    // MYVK_LOG_RECTS：把视口矩形打给自动化脚本
#include "EditorScene.h"  // instantiateModel

#include "render/Renderer.h"

#include "ecs/Components.h"
#include "scene/Camera.h"
#include "scene/Scene.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace editor {

namespace {

// 面板最小渲染尺寸：再小就退化成 1×1 的图，重建开销反而不划算
constexpr float kMinViewport = 64.0f;

// ---- 视口导航（编辑态）参数 ----
// 鼠标每移动 1 像素转多少弧度（再乘灵敏度）。0.0025 rad ≈ 0.14°/px ——
// 接近 UE 视口按住右键转头的手感。
constexpr float kNavLookPerPixel = 0.0025f;
// 中键平移：每像素移动多少米（再乘灵敏度）。比旋转保守一些，
// 否则轻轻一拖相机就飞出场景了。
constexpr float kNavPanPerPixel = 0.01f;
// 基础飞行速度（米/秒，再乘灵敏度；按住 Shift 再 ×3）
constexpr float kNavMoveSpeed = 5.0f;
// 单独滚轮：每格把相机沿视线推 / 拉多少米（再乘灵敏度）。
// UE 视口里的"缩放"就是让相机前后走，而不是改 FOV —— 更接近"走近看"
// 这种直觉，也让滚轮和 WASD 共用同一套速度感。
constexpr float kNavDollyPerNotch = 0.6f;
// **按住右键**时滚轮每格的灵敏度增减量（"慢慢变大 / 变小"）
constexpr float kSensitivityStep = 0.1f;

// ---- 吸附预设（对齐 UE5 视口工具条的下拉值）----
// UE 的平移预设是 1 / 10 / 100 / 1000 uu，本引擎 1 单位 = 1 米、1 uu = 1 cm，
// 所以换算过来正好是同一组数字 0.01 / 0.1 / 1 / 10（米）。
// 旋转与缩放是无量纲 / 角度，直接照抄 UE 的预设。
const float kSnapMovePresets[] = {0.01f, 0.1f, 1.0f, 10.0f};
const float kSnapRotPresets[] = {5.0f, 10.0f, 15.0f, 30.0f, 45.0f};
const float kSnapScalePresets[] = {0.01f, 0.1f, 1.0f, 10.0f};

struct SnapPresetSet {
    const char* label;
    const float* values;
    int count;
    const char* fmt;
    float* target;
};

// 一个"吸附"控件组：[x] 名称 [数值] —— 勾选框控制开关，数值可直接拖 / 输入。
// 步长范围故意给得宽：平移能到 100 m（搭大场景），也能到 1 mm。
bool snapField(const char* label, bool& enabled, float& value,
               const char* fmt, float vmin, float vmax, float speed,
               const char* tooltip) {
    bool changed = false;
    ImGui::PushID(label);

    if (ImGui::Checkbox("##on", &enabled)) changed = true;
    ImGui::SameLine(0.0f, 4.0f);

    ImGui::BeginDisabled(!enabled);
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("0.0000").x +
                            ImGui::GetStyle().FramePadding.x * 2.0f + 8.0f);
    if (ImGui::DragFloat("##v", &value, speed, vmin, vmax, fmt)) changed = true;
    ImGui::EndDisabled();

    ImGui::SameLine(0.0f, 4.0f);
    ImGui::TextUnformatted(label);

    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);

    ImGui::PopID();
    return changed;
}

} // namespace

ViewportPanel::ViewportPanel(EditorContext& ctx, render::Renderer& renderer)
    : m_ctx(ctx), m_renderer(renderer) {}

// ---------------------------------------------------------------- 工具栏

void ViewportPanel::drawToolbar() {
    // 手柄模式：三个互斥按钮，快捷键 W/E/R
    const GizmoMode mode = m_ctx.gizmoMode();
    const ImVec4 active(0.26f, 0.52f, 0.85f, 1.0f);
    const ImVec4 idle(0.20f, 0.20f, 0.23f, 1.0f);

    ImGui::PushStyleColor(ImGuiCol_Button, mode == GizmoMode::Translate ? active : idle);
    if (ImGui::Button("Move (W)")) m_ctx.setGizmoMode(GizmoMode::Translate);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, mode == GizmoMode::Rotate ? active : idle);
    if (ImGui::Button("Rotate (E)")) m_ctx.setGizmoMode(GizmoMode::Rotate);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, mode == GizmoMode::Scale ? active : idle);
    if (ImGui::Button("Scale (R)")) m_ctx.setGizmoMode(GizmoMode::Scale);
    ImGui::PopStyleColor();

    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();

    // 坐标系
    const GizmoSpace space = m_ctx.gizmoSpace();
    if (ImGui::RadioButton("World", space == GizmoSpace::World))
        m_ctx.setGizmoSpace(GizmoSpace::World);
    ImGui::SameLine();
    if (ImGui::RadioButton("Local", space == GizmoSpace::Local))
        m_ctx.setGizmoSpace(GizmoSpace::Local);

    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();

    ImGui::Checkbox("Gizmo", &m_showGizmo);
    ImGui::SameLine();
    ImGui::Checkbox("HUD", &m_showOverlay);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Viewport overlay (stats / selection) and the\n"
                          "navigation HUD in the top-right corner.");
    }

    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();
    ImGui::BeginDisabled(!m_ctx.hasSelection());
    if (ImGui::Button("Focus (F)")) focusSelection();
    ImGui::EndDisabled();

    // 注：全屏切换按钮**不在这里**。它已经在主工具栏和视口右上角浮层里
    // 各有一个（全屏时工具栏不画，浮层那个是必备的退出口）。视口工具栏本
    // 来就长，再加一个会被挤出右边缘裁掉 —— 上一版就是这么翻车的。

    // 吸附单独占一行：三种变换各有开关 + 步长，塞进上面那行必定溢出。
    drawSnapRow();
}

// ---------------------------------------------------------------- 吸附
//
// UE5 的模型是"每种变换一套吸附开关 + 步长"，这里照做（默认值与预设值
// 见文件头常量）。三个勾选框默认关闭 —— 吸附是"想对齐时开一下"的手段，
// 默认开着会让自由摆放变成一格一格地跳。
//
// 为什么单独起一行：工具栏第一行已经是 Move/Rotate/Scale + World/Local +
// Gizmo/HUD + Focus，再塞三个勾选框 + 三个数字框会被挤出右边缘裁掉
// （这个坑上一版踩过一次）。
void ViewportPanel::drawSnapRow() {
    EditorContext& c = m_ctx;

    ImGui::TextDisabled("Snap");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Grid snapping - one set per transform, same model as UE5's\n"
            "viewport toolbar. Defaults: move 0.1 m / rotate 10 deg /\n"
            "scale 0.1 (= UE's 10 uu / 10 deg / 0.1, with 1 uu = 1 cm).");
    }
    ImGui::SameLine();

    snapField("Move (m)", c.snapMoveRef(), c.snapMoveStepRef(), "%.3f", 0.001f,
              100.0f, 0.005f,
              "Translate: the drag delta is quantised to this many meters.\n"
              "UE default = 10 uu = 0.1 m.");
    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();
    snapField("Rotate (deg)", c.snapRotateRef(), c.snapRotateStepRef(), "%.1f",
              0.5f, 90.0f, 0.1f,
              "Rotate: the drag delta is quantised to this many degrees.\n"
              "UE default = 10 deg.");
    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();
    snapField("Scale (x)", c.snapScaleRef(), c.snapScaleStepRef(), "%.3f",
              0.001f, 10.0f, 0.005f,
              "Scale: the scale FACTOR is quantised (not the absolute value,\n"
              "so the first drag never jumps). UE default = 0.1.");

    // ---- 预设（对齐 UE5 下拉里那一串值）----
    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();
    if (ImGui::Button("Presets")) ImGui::OpenPopup("##snap_presets");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Pick a step from UE5's preset lists.");
    }

    SnapPresetSet sets[3] = {
        {"Move (m)", kSnapMovePresets, 4, "%.2f", &c.snapMoveStepRef()},
        {"Rotate (deg)", kSnapRotPresets, 5, "%.0f", &c.snapRotateStepRef()},
        {"Scale (x)", kSnapScalePresets, 4, "%.2f", &c.snapScaleStepRef()},
    };

    if (ImGui::BeginPopup("##snap_presets")) {
        for (SnapPresetSet& s : sets) {
            ImGui::TextDisabled("%s", s.label);
            for (int i = 0; i < s.count; ++i) {
                char item[48];
                std::snprintf(item, sizeof(item), s.fmt,
                              static_cast<double>(s.values[i]));
                if (i > 0) ImGui::SameLine();
                ImGui::PushID(i);
                if (ImGui::SmallButton(item)) *s.target = s.values[i];
                ImGui::PopID();
            }
            ImGui::Separator();
        }
        ImGui::TextDisabled("Values follow UE5's viewport presets.");
        ImGui::EndPopup();
    }

    // 当前模式的吸附状态（手柄拖拽时的实时读数在左上角叠加层里）
    ImGui::SameLine();
    const char* modeLabel = c.gizmoMode() == GizmoMode::Translate ? "move"
                            : c.gizmoMode() == GizmoMode::Rotate ? "rotate"
                                                                 : "scale";
    ImGui::TextDisabled("current: %s snap %s", modeLabel,
                        c.snapForCurrentMode() ? "ON" : "off");
}

void ViewportPanel::focusSelection() const {
    if (!m_ctx.hasSelection()) return;
    scene::Scene& sc = m_ctx.editorScene();
    const glm::vec3 p = glm::vec3(sc.worldMatrix(m_ctx.selection())[3]);

    // 用网格包围盒估算一个合适的距离，避免"贴脸"或"看不见"
    const Aabb b = m_picking.worldAabb(sc, m_ctx.selection());
    float radius = 1.0f;
    if (b.valid) radius = glm::max(glm::length(b.max - b.min) * 0.5f, 0.35f);

    sc.camera().setTarget(p);
    sc.camera().setDistance(radius / std::tan(glm::radians(sc.camera().fov() * 0.5f)) * 1.8f);
}

// ---------------------------------------------------------------- 编辑态导航
//
// 完全按 UE 编辑器的视口习惯（也正是用户要的那套）：
//
//   按住右键拖动 —— 自由旋转视角（水平 + 俯仰一起动）
//   按住中键拖动 —— 平移（沿相机的右 / 上方向）
//   单独滚轮     —— 沿视线推 / 拉相机（推近 / 拉远）
//   右键 + 滚轮  —— 调导航灵敏度（上滑变大 / 下滑变小）
//   按住右键时：WSAD = 前后左右飞行，Q / E = 下降 / 上升，Shift = 加速
//   左键         —— **不参与导航**：专用于点选物体与拖拽 gizmo 手柄
//
// 为什么移动键要"按住右键才生效"：W / E / R 同时还是手柄模式快捷键
// （Translate / Rotate / Scale，见 EditorApp::handleShortcuts）。UE 就是
// 这么解决的 —— 只有按住右键（真正在"驾驶"相机）时字母键才驱动相机，
// 松开就还给快捷键。否则一按 W 就会既往前飞、又把手柄切成平移模式。
// 滚轮走同一个"右键 = 驾驶中"的判定，于是滚轮有两种互不干扰的含义。
//
// 相机必须处于自由飞行模式（EditorContext::applyEditorCameraMode 保证），
// 否则 look() 会退化成"绕目标点打转"，手感完全不对。
void ViewportPanel::handleNavigation(bool hovered) const {
    if (!hovered || m_gizmo.dragging()) return;

    const ImGuiIO& io = ImGui::GetIO();
    scene::Camera& cam = m_ctx.activeScene().camera();
    const float sens = m_ctx.navSensitivity();

    // 鼠标位移取 ImGui 的 MouseDelta：它在"悬停于视口"时才有意义，
    // 而且不会像 core::Input 那样被 ImGui 的捕获标记清零。
    const glm::vec2 d(io.MouseDelta.x, io.MouseDelta.y);
    const bool lookDown = ImGui::IsMouseDown(ImGuiMouseButton_Right);
    const bool panDown = ImGui::IsMouseDown(ImGuiMouseButton_Middle);

    // ---- 旋转：按住右键（自由旋转，含俯仰）----
    if (lookDown) {
        cam.look(d.x * kNavLookPerPixel * sens, d.y * kNavLookPerPixel * sens);
    }

    // ---- 平移：按住中键（相机本地右 / 上方向）----
    if (panDown && (d.x != 0.0f || d.y != 0.0f)) {
        const float k = kNavPanPerPixel * sens;
        cam.moveLocal(glm::vec3(-d.x * k, d.y * k, 0.0f));
    }

    // ---- 滚轮：两条路，靠"是否按住右键"分开 ----
    //   单独滚轮    → 推 / 拉相机（缩放）
    //   右键 + 滚轮 → 导航灵敏度
    if (io.MouseWheel != 0.0f) {
        if (lookDown) {
            m_ctx.addNavSensitivity(io.MouseWheel * kSensitivityStep);
        } else {
            cam.moveLocal(glm::vec3(0.0f, 0.0f,
                                    io.MouseWheel * kNavDollyPerNotch * sens));
        }
    }

    // ---- 飞行：只在按住右键时（否则字母键要让给手柄快捷键）----
    if (!lookDown) return;
    // 正在 HUD 的输入框里打字时不要抢键（否则 W 会既进文本框又移动相机）
    if (io.WantTextInput) return;

    float f = 0.0f, r = 0.0f, u = 0.0f;
    if (ImGui::IsKeyDown(ImGuiKey_W)) f += 1.0f;
    if (ImGui::IsKeyDown(ImGuiKey_S)) f -= 1.0f;
    if (ImGui::IsKeyDown(ImGuiKey_D)) r += 1.0f;
    if (ImGui::IsKeyDown(ImGuiKey_A)) r -= 1.0f;
    if (ImGui::IsKeyDown(ImGuiKey_E)) u += 1.0f;  // E = 上升
    if (ImGui::IsKeyDown(ImGuiKey_Q)) u -= 1.0f;  // Q = 下降
    if (f == 0.0f && r == 0.0f && u == 0.0f) return;

    float speed = kNavMoveSpeed * sens * io.DeltaTime;
    if (io.KeyShift) speed *= 3.0f;
    cam.moveLocal(glm::vec3(r * speed, u * speed, f * speed));
}

// ---------------------------------------------------------------- 叠加层

void ViewportPanel::drawOverlay(ImDrawList* dl, const glm::vec2& vpPos,
                                const glm::vec2& vpSize) const {
    (void)vpSize;
    const ImU32 col = IM_COL32(216, 228, 240, 230);
    const ImU32 warn = IM_COL32(255, 196, 92, 235);

    const float x = vpPos.x + 10.0f;
    float y = vpPos.y + 8.0f;
    char buf[256];

    const ImGuiIO& io = ImGui::GetIO();
    std::snprintf(buf, sizeof(buf), "%.0f FPS   %.2f ms", io.Framerate,
                  io.Framerate > 0.0f ? 1000.0f / io.Framerate : 0.0f);
    dl->AddText(ImVec2(x, y), col, buf);
    y += 17.0f;

    const scene::Scene& sc = m_ctx.activeScene();
    const scene::Scene::LightStats ls = sc.lightStats();
    std::snprintf(buf, sizeof(buf), "entities %zu   lights %zu   %ux%u",
                  sc.objectCount(), ls.pointLights + ls.spotLights,
                  m_requestedW, m_requestedH);
    dl->AddText(ImVec2(x, y), col, buf);
    y += 17.0f;

    const ecs::World& w = sc.world();
    const std::string* nm =
        m_ctx.hasSelection() ? w.name(m_ctx.selection()) : nullptr;
    std::snprintf(buf, sizeof(buf), "selected: %s",
                  nm ? nm->c_str() : "(none)");
    dl->AddText(ImVec2(x, y),
                nm ? col : IM_COL32(150, 158, 168, 200), buf);
    y += 17.0f;

    if (!m_ctx.isEditing()) {
        // 视口渲染的就是**运行态副本**本身 —— 游戏画面与编辑画面共用同一条
        // 离屏渲染链，只是相机换成了自由飞行视角。
        // 灵敏度与操作提示在右上角的浮层里（见 drawNavHud）。
        const char* state = m_ctx.isPaused() ? "PAUSED" : "PLAYING";
        std::snprintf(buf, sizeof(buf), "%s  t=%.2fs%s", state,
                      static_cast<double>(m_ctx.playTime()),
                      m_ctx.gameFullscreen() ? "  [fullscreen]" : "");
        dl->AddText(ImVec2(x, y), warn, buf);
    } else {
        std::snprintf(buf, sizeof(buf), "EDIT   gizmo: %s / %s",
                      m_ctx.gizmoMode() == GizmoMode::Translate ? "move"
                      : m_ctx.gizmoMode() == GizmoMode::Rotate ? "rotate"
                                                              : "scale",
                      m_ctx.gizmoSpace() == GizmoSpace::World ? "world"
                                                             : "local");
        dl->AddText(ImVec2(x, y), col, buf);
        y += 17.0f;

        // 拖动隐藏着一半的信息：拖的时候手柄会被鼠标挡住，人也未必看得清
        // 屏幕上那点位置变化。这里直接报"沿哪个轴走了多少"（吸附打开时
        // 末尾带 [snap]），是最直观的反馈。
        if (m_gizmo.dragging() && !m_gizmo.readout().empty()) {
            dl->AddText(ImVec2(x, y), IM_COL32(255, 214, 102, 240),
                        m_gizmo.readout().c_str());
        }
    }
}

// ---------------------------------------------------------------- 主流程

void ViewportPanel::draw() {
    EditorContext::PanelVisibility& panels = m_ctx.panels();
    if (!panels.viewport) return;

    // 游戏全屏：视口独占整窗（布局矩形由 EditorApp 填成整屏），连标题栏都不要。
    const bool gameFullscreen = !m_ctx.isEditing() && m_ctx.gameFullscreen();

    const LayoutRects& L = m_ctx.layout();
    // 分栏布局：每帧跟随分栏树，不允许手动拖动窗口
    ImGui::SetNextWindowPos(ImVec2(L.viewport.x, L.viewport.y),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(L.viewport.w, L.viewport.h),
                             ImGuiCond_Always);
    // 全屏时去掉窗口内边距，画面真正贴边
    if (gameFullscreen) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    }

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoScrollWithMouse |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoCollapse;
    if (gameFullscreen) {
        flags |= ImGuiWindowFlags_NoTitleBar |
                 ImGuiWindowFlags_NoBringToFrontOnFocus;
    }

    char title[64];
    // 用 "###viewport" 固定窗口 ID：否则 [PLAYING] / [PAUSED] 前缀变化会被
    // ImGui 当成另一个窗口，位置与尺寸的记忆全部丢失
    const char* state = m_ctx.isPlaying()
                            ? "  [PLAYING]"
                            : (m_ctx.isPaused() ? "  [PAUSED]" : "");
    std::snprintf(title, sizeof(title), "Viewport%s###viewport", state);
    if (!ImGui::Begin(title, &panels.viewport, flags)) {
        ImGui::End();
        if (gameFullscreen) ImGui::PopStyleVar();
        return;
    }

    // 全屏时工具栏不画（否则会盖在游戏画面上），退出全屏有 HUD 上的按钮和 F 键
    if (!gameFullscreen) {
        drawToolbar();
        ImGui::Separator();
    }

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const std::uint32_t wantW = static_cast<std::uint32_t>(
        std::max(avail.x, kMinViewport));
    const std::uint32_t wantH = static_cast<std::uint32_t>(
        std::max(avail.y, kMinViewport));

    if (wantW != m_requestedW || wantH != m_requestedH) {
        m_requestedW = wantW;
        m_requestedH = wantH;
        m_resizePending = true;
        m_renderer.setViewportSize(wantW, wantH);
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const std::uint64_t texId = m_renderer.viewportTextureId();
    const ImVec2 imageSize(static_cast<float>(m_requestedW),
                           static_cast<float>(m_requestedH));

    if (!m_resizePending && texId != 0) {
        ImGui::Image(static_cast<ImTextureID>(texId), imageSize, ImVec2(0, 0),
                     ImVec2(1, 1));
    } else {
        // 尺寸刚变（或首帧还没拿到纹理）：本帧不碰纹理，等下一帧
        ImGui::Dummy(imageSize);
        dl->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                          IM_COL32(16, 18, 22, 255));
        dl->AddText(ImVec2(ImGui::GetItemRectMin().x + 10.0f,
                           ImGui::GetItemRectMin().y + 10.0f),
                    IM_COL32(140, 148, 158, 220),
                    "rebuilding viewport render target...");
        m_resizePending = false;
    }

    const ImVec2 imgMin = ImGui::GetItemRectMin();
    const ImVec2 imgSz = ImGui::GetItemRectSize();
    const bool hovered = ImGui::IsItemHovered();

    // 自动化用（MYVK_LOG_RECTS=1）：视口图像的屏幕矩形。
    // 拖拽落点 / 手柄命中都在这块矩形里，脚本靠它算坐标。
    logRect("VP-RECT", imgMin,
            ImVec2(imgMin.x + imgSz.x, imgMin.y + imgSz.y));

    const glm::vec2 vpPos(imgMin.x, imgMin.y);
    const glm::vec2 vpSize(imgSz.x, imgSz.y);

    // 1) 相机导航：**只在编辑态**。
    //    Play 期间相机是运行态场景自己的东西，编辑器视口一个键鼠事件都不抢
    //    —— 用户明确要的就是"运行前的视口按 UE 那套来，运行后归游戏管"。
    if (m_ctx.isEditing()) handleNavigation(hovered);

    // 2) gizmo（先跑：它可能在本次点击里"吃掉"鼠标）。只在编辑态有效
    scene::Camera& cam = m_ctx.activeScene().camera();
    bool gizmoConsumed = false;
    if (m_showGizmo && m_ctx.isEditing()) {
        gizmoConsumed = m_gizmo.update(m_ctx, cam, vpPos, vpSize, hovered, dl);
    }

    // 3) 点选（编辑态、点左键、不在导航手势里、手柄没吃掉）
    //    左键专属于点选与拖手柄：导航手势只有右键（转头）和中键（平移）。
    const ImGuiIO& io = ImGui::GetIO();
    const bool navGesture = ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
                            ImGui::IsMouseDown(ImGuiMouseButton_Middle);
    if (m_ctx.isEditing() && hovered && !gizmoConsumed && !navGesture &&
        !m_gizmo.dragging() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const glm::vec2 mouse(io.MousePos.x, io.MousePos.y);
        const Ray ray =
            PickingSystem::rayFromViewport(cam, vpPos, vpSize, mouse);
        const PickResult hit = m_picking.pick(m_ctx.activeScene(), ray);
        if (hit.hit) {
            m_ctx.select(hit.entity);
            const std::string* n = m_ctx.activeScene().world().name(hit.entity);
            m_ctx.setStatus("Selected " + (n ? *n : std::string("Object")));
        } else {
            m_ctx.clearSelection();
        }
    }

    // 4) 叠加信息
    if (m_showOverlay) drawOverlay(dl, vpPos, vpSize);

    // 5) 内容浏览器拖进来的**网格** → 就地实例化。
    //    落点 = 鼠标射线与 y = 0 栅格面的交点（拖到哪儿就落在哪儿，不是
    //    视口中心）；实例化之后按世界包围盒把**底部刚好贴到栅格面**上，
    //    水平中心对准落点 —— 这就是"拖进来就稳稳站在地上"。
    if (ImGui::BeginDragDropTarget()) {
        // 拖着一个资产悬在视口上方时，给一圈高亮 + 一句落点说明
        if (const ImGuiPayload* drag = ImGui::GetDragDropPayload()) {
            if (drag->IsDataType("EDITOR_ASSET")) {
                dl->AddRect(imgMin, ImVec2(imgMin.x + imgSz.x, imgMin.y + imgSz.y),
                            IM_COL32(104, 190, 132, 210), 0.0f, 0, 3.0f);
                dl->AddText(ImVec2(imgMin.x + 14.0f, imgMin.y + imgSz.y - 30.0f),
                            IM_COL32(160, 230, 180, 255),
                            "release to place it on the grid (bottom snaps "
                            "to y = 0)");
            }
        }

        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("EDITOR_ASSET")) {
            const char* path = static_cast<const char*>(p->Data);
            const std::string rel = path ? path : std::string();

            const glm::vec2 mouse(io.MousePos.x, io.MousePos.y);
            const Ray ray =
                PickingSystem::rayFromViewport(cam, vpPos, vpSize, mouse);
            glm::vec3 spot(0.0f);
            float t = 0.0f;
            if (PickingSystem::intersectPlane(ray, glm::vec3(0.0f),
                                              glm::vec3(0.0f, 1.0f, 0.0f),
                                              t) &&
                t > 0.0f) {
                spot = ray.origin + ray.dir * t;
                spot.y = 0.0f;
            } else {
                // 相机朝上（射线与地面同向）时兜底：落在视线方向上 5 米处的地面
                glm::vec3 fwd = cam.target() - cam.position();
                if (glm::length(fwd) < 1e-4f) fwd = glm::vec3(0, 0, -1);
                spot = cam.position() + glm::normalize(fwd) * 5.0f;
                spot.y = 0.0f;
            }

            ImportResult imported;
            m_ctx.structuralEdit("Import " + rel, [&]() {
                imported = instantiateModel(m_ctx.editorScene(), m_ctx.assets(),
                                            rel, spot);
                if (imported.ok()) {
                    alignImportToGround(m_ctx.editorScene(), m_picking, imported,
                                        spot);
                }
            });
            if (imported.ok()) {
                m_ctx.select(imported.root);
                m_ctx.notify("Imported " + rel + " (" +
                             std::to_string(imported.count()) + " mesh)");
                m_ctx.dirty() = true;
            } else {
                m_ctx.notify("Failed to import " + rel);
            }
        }
        ImGui::EndDragDropTarget();
    }

    // 视口右上角浮层。两者都放在所有交互之后、End() 之前 —— 它们是
    // **子窗口**，见下面各自的说明。
    //   编辑态 → 导航 HUD（灵敏度 + 操作提示），跟随 "HUD" 勾选框
    //   运行态 → 极简播放浮层（全屏按钮 + 状态），不含任何导航控件
    if (m_ctx.isEditing()) {
        if (m_showOverlay) drawNavHud(vpPos, vpSize);
    } else {
        drawPlayHud(vpPos, vpSize);
    }

    ImGui::End();
    if (gameFullscreen) ImGui::PopStyleVar();
}

// ---------------------------------------------------------------- 右上角浮层
//
// 为什么必须是**子窗口**而不是独立顶层窗口：
// 视口窗口本身是焦点窗口，ImGui 每帧都会把焦点窗口 BringWindowToDisplayFront，
// 于是独立窗口会被推到列表前面、**被视口盖住**（第一版就是这么翻车的）。
// 子窗口的绘制永远在父窗口之后，天然压在 3D 画面之上。
//
// 位置跟着 Image 的矩形走，全屏时 Image 就是整屏，于是它就落在屏幕右上角。

// 编辑态导航 HUD。灵敏度是"移动与旋转共用"的系数：默认 1、必须 > 0。
// 滚轮在视口图像上"按住右键"滑也能调（那条路径在 handleNavigation 里），
// 这里再补一条"鼠标停在 HUD 上时同理" —— 否则用户会觉得滚轮时灵时不灵。
// 顺带放一个**地平面栅格的显示开关**（用户要的"视图右上角的单选框"，
// 默认开；View 菜单里有一个等价入口）。
void ViewportPanel::drawNavHud(const glm::vec2& vpPos, const glm::vec2& vpSize) {
    // 快捷键提示：最宽的一行决定浮层宽度。刻意写得紧凑 —— 视口本身
    // 可能不宽，浮层不能喧宾夺主。
    static const char* kHints[] = {
        "RMB drag look      MMB pan",
        "RMB + WASD fly     Q/E down-up",
        "wheel dolly    RMB+wheel sens",
    };

    // 尺寸跟着当前字号算。字体缩放是用户可调的（Settings 面板），
    // 写死像素的话一放大文字就会被裁掉半截 —— 这个坑第一版就踩过。
    const ImGuiStyle& st = ImGui::GetStyle();
    const float lineH = ImGui::GetTextLineHeightWithSpacing();
    const float frameH = ImGui::GetFrameHeightWithSpacing();

    float textW = ImGui::CalcTextSize("Viewport navigation").x;
    for (const char* h : kHints) {
        textW = std::max(textW, ImGui::CalcTextSize(h).x);
    }
    // 灵敏度那一行是 "输入框(104) + 标签 + 间距"
    textW = std::max(textW, ImGui::CalcTextSize("Sensitivity").x + 118.0f);
    // 栅格那一行是 "勾选框 + 标签 + 副标题"
    textW = std::max(textW, ImGui::CalcTextSize("Ground grid (1 m)").x +
                                 ImGui::CalcTextSize("(y = 0 plane)").x + 44.0f);

    const float cw = textW + st.WindowPadding.x * 2.0f + 8.0f;
    // 5 行文字（标题 + 分隔线 + 3 行提示）+ 3 个控件行（输入框、重置按钮、
    // 栅格勾选框）
    const float ch = lineH * 5.0f + frameH * 3.0f + st.WindowPadding.y * 2.0f +
                     st.ItemSpacing.y * 3.0f;  // 留一点富余，免得出现滚动条

    ImGui::SetCursorScreenPos(
        ImVec2(vpPos.x + vpSize.x - cw - 10.0f, vpPos.y + 10.0f));

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.07f, 0.08f, 0.10f, 0.80f));
    // 注意：1.90+ 的 BeginChild 第三个参数是 child_flags（不再是 border 布尔），
    // 这里传 0 最保险；要不要边框无所谓，背景已经有区分度了。
    ImGui::BeginChild("##nav_hud", ImVec2(cw, ch), 0);

    // 鼠标停在浮层上时：**按住右键**滚轮同样调灵敏度（此时鼠标不在视口
    // 图像上，handleNavigation 那条路径不会触发）。单独滚轮在浮层上不做
    // 任何事 —— 免得想把鼠标滚过输入框时意外把相机推走了。
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsWindowHovered() &&
        ImGui::IsMouseDown(ImGuiMouseButton_Right) && io.MouseWheel != 0.0f) {
        m_ctx.addNavSensitivity(io.MouseWheel * kSensitivityStep);
    }

    ImGui::TextDisabled("Viewport navigation");

    // ---- 灵敏度 ----
    // InputFloat 让用户直接填浮点（点/拖都能改）。约束必须 > 0：
    // 非正数（或输入过程中的乱值）一律忽略，下一帧会回显上一个合法值。
    float sens = m_ctx.navSensitivity();
    ImGui::SetNextItemWidth(104.0f);
    if (ImGui::InputFloat("Sensitivity", &sens, 0.1f, 1.0f, "%.2f")) {
        if (sens > 0.0f) {
            m_ctx.navSensitivityRef() =
                glm::clamp(sens, EditorContext::kMinSensitivity,
                           EditorContext::kMaxSensitivity);
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Must be > 0 (%.2f .. %.2f).\n"
                          "Hold RMB + mouse wheel to nudge it.",
                          static_cast<double>(EditorContext::kMinSensitivity),
                          static_cast<double>(EditorContext::kMaxSensitivity));
    }

    if (ImGui::SmallButton("Reset to 1.00")) m_ctx.navSensitivityRef() = 1.0f;

    // ---- 地平面栅格 ----
    // 用户要的"视图右上角的单选框"。默认开；真正的绘制在 render/Renderer
    // 的 grid pass（最小格 1 m、主格 10 m），这里只管开关。
    // Play 期间栅格不画（视口里是游戏画面），所以这个勾选框只在编辑态出现。
    ImGui::Checkbox("Ground grid (1 m)", &m_ctx.showGridRef());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Reference grid on the y = 0 plane.\n"
            "Minor lines every 1 m, major lines every 10 m.\n"
            "Hidden while playing (the viewport shows the game then).\n"
            "Start with MYVK_GRID=0 to force it off.");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(y = 0 plane)");

    ImGui::Separator();
    for (const char* h : kHints) ImGui::TextDisabled("%s", h);

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// 运行态极简浮层：只留"退出全屏"的入口 + 播放状态。
// **刻意不含任何导航控件** —— Play 期间相机归游戏管，编辑器不该提供会抢
// 输入的 UI；全屏时工具栏不画，所以这里必须留一个图形化的退出口（F / Esc 也行）。
void ViewportPanel::drawPlayHud(const glm::vec2& vpPos, const glm::vec2& vpSize) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float lineH = ImGui::GetTextLineHeightWithSpacing();
    const float frameH = ImGui::GetFrameHeightWithSpacing();

    float textW = ImGui::CalcTextSize("Restore view  (F)").x;
    textW = std::max(textW, ImGui::CalcTextSize("PLAYING   t = 0.00s").x);
    const float cw = textW + st.WindowPadding.x * 2.0f + 8.0f;
    const float ch = frameH + lineH + st.WindowPadding.y * 2.0f;

    ImGui::SetCursorScreenPos(
        ImVec2(vpPos.x + vpSize.x - cw - 10.0f, vpPos.y + 10.0f));

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.07f, 0.08f, 0.10f, 0.80f));
    ImGui::BeginChild("##play_hud", ImVec2(cw, ch), 0);

    if (ImGui::Button(m_ctx.gameFullscreen() ? "Restore view  (F)"
                                             : "Fullscreen  (F)",
                      ImVec2(-1.0f, 0.0f))) {
        m_ctx.toggleGameFullscreen();
    }

    ImGui::TextColored(m_ctx.isPlaying() ? ImVec4(0.55f, 0.9f, 0.58f, 1.0f)
                                         : ImVec4(0.95f, 0.8f, 0.4f, 1.0f),
                       "%s   t = %.2fs",
                       m_ctx.isPlaying() ? "PLAYING" : "PAUSED",
                       static_cast<double>(m_ctx.playTime()));

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

} // namespace editor
