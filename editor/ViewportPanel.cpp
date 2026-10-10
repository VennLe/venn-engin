#include "ViewportPanel.h"

#include "DebugRects.h"    // MYVK_LOG_RECTS：把视口矩形打给自动化脚本
#include "EditorDragDrop.h"
#include "EditorScene.h"  // instantiateModel

#include "render/Renderer.h"

#include "ecs/Components.h"
#include "physics/CollisionWorld.h"
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
// Sensitivity 数值框的宽度。这个框是 InputFloat **不带 step 按钮**的，
// 所以要能放下 "20.00"（上限）再加一点余量 —— 带按钮的版本会被两个
// 方块挤掉一半宽度，数字直接显示不全（这是本次要修的问题）。
constexpr float kSensitivityFieldW = 88.0f;

// ---- 视图方向（导航球 + 数字键共用）----
// 下标 = 轴 * 2 + (0 正 / 1 负)，顺序必须和 drawNavGizmo 里生成小球的
// 循环顺序一致（X+, X-, Y+, Y-, Z+, Z-），这样"命中第 i 个小球"就等于"第 i 个方向"。
// Z-up 约定（与 Blender 一致）：+Z 是天，-Y 是"正面"（Blender 前视图的相机
// 就在 -Y 侧）。"从 +X 看过去"指相机摆在 +X 一侧、朝原点看。
enum ViewDir {
    kViewPosX = 0,  // 从 +X 看过去 → Right
    kViewNegX = 1,  // 从 -X 看过去 → Left
    kViewPosY = 2,  // 从 +Y 看过去 → Back
    kViewNegY = 3,  // 从 -Y 看过去 → Front
    kViewPosZ = 4,  // 从 +Z 看下→上   → Top
    kViewNegZ = 5,  // 从 -Z 看上→下   → Bottom
};

const char* viewDirName(int dir) {
    switch (dir) {
        case kViewPosX: return "Right  (+X)";
        case kViewNegX: return "Left  (-X)";
        case kViewPosY: return "Back  (+Y)";
        case kViewNegY: return "Front  (-Y)";
        case kViewPosZ: return "Top  (+Z)";
        default:        return "Bottom  (-Z)";
    }
}

// 三根世界轴的 RGB 配色，与 GizmoController 的坐标轴同色：
// 红 = X、绿 = Y、蓝 = Z。现在引擎是 Z 朝上（和 Blender 一样），
// 所以"蓝"是垂直轴，红/绿在水平面里。
const ImU32 kNavAxisCol[3] = {
    IM_COL32(228, 78, 78, 255),   // X 红
    IM_COL32(118, 208, 84, 255),  // Y 绿
    IM_COL32(80, 132, 238, 255),  // Z 蓝
};

// 只换 alpha、保留 RGB —— 用来做"背向观察者的那一端淡一点"的前后层次。
ImU32 withAlpha(ImU32 c, float a) {
    const ImU32 ai =
        static_cast<ImU32>(glm::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
    return (c & ~IM_COL32_A_MASK) | (ai << IM_COL32_A_SHIFT);
}

// 把相机**绕当前注视点**转到指定方向 —— 不是原地转头。
// Blender 的导航球/小键盘切视图就是绕轨道中心转，被观察的东西始终留在画面里；
// 原地转头则会让目标直接飞出视野。
// 做法：先算好新的 yaw/pitch，再把 target 钉回原处 —— setTarget 在飞行模式下
// 会把相机重摆到 "pivot + angleDir * distance"，即绕 pivot 走一段球面。
void snapViewTo(scene::Camera& cam, int dir) {
    constexpr float kHalfPi = 1.5707964f;
    const glm::vec3 pivot = cam.target();
    const float dist = cam.distance();

    switch (dir) {
        case kViewPosX: cam.setYaw(0.0f);        cam.setPitch(0.0f); break;
        case kViewNegX: cam.setYaw(kHalfPi * 2.0f); cam.setPitch(0.0f); break;
        case kViewNegY: cam.setYaw(kHalfPi);     cam.setPitch(0.0f); break;  // Front
        case kViewPosY: cam.setYaw(-kHalfPi);    cam.setPitch(0.0f); break;  // Back
        // 顶 / 底视图 yaw 是退化量：**保留当前 yaw**，画面里的水平朝向才连续
        case kViewPosZ: cam.setPitch(scene::Camera::kPitchLimit); break;
        case kViewNegZ: cam.setPitch(-scene::Camera::kPitchLimit); break;
        default: return;
    }

    cam.setTarget(pivot);   // 钉回注视点 → 绕它转，而不是原地转头
    cam.setDistance(dist);
}

// 自由旋转（拖拽）同样绕注视点，手感和点击吸附保持一致。
void orbitViewAroundPivot(scene::Camera& cam, float dYaw, float dPitch) {
    const glm::vec3 pivot = cam.target();
    cam.orbit(dYaw, dPitch);
    cam.setTarget(pivot);
}

// ---- 导航球（Navigation Gizmo）的几何 ----
constexpr float kGizmoRingR   = 42.0f;  // 圆心 → 轴端小球中心 的距离（像素）
constexpr float kGizmoBallR   = 9.5f;   // 正轴小球半径
constexpr float kGizmoBallNeg = 8.0f;   // 负轴小球半径（略小，和 Blender 一样）
constexpr float kGizmoMargin  = 14.0f;  // 距离视口左下角的留白
constexpr float kGizmoClickSlop = 5.0f; // 位移小于它就当成"点击"（否则算拖拽）

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
        ImGui::SetTooltip("Viewport overlay (stats / selection), the navigation\n"
                          "HUD in the top-right and the navigation gizmo\n"
                          "in the bottom-left corner.");
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

// ---------------------------------------------------------------- 落地（数字键 0）
//
// 把选中的物体**竖直**挪到地面上：x / y 一点都不动，只改 z，让它的世界
// 包围盒底部正好落在 z = 0（栅格面 = 地面顶面）。
//
// 三个刻意的限制：
//   · **只认"在 3D 视口里左键点中"的选中**：层级树 / 内容浏览器里选中的
//     东西不一定在视口里看得见，"落地"对它没有意义，误按 0 不该乱动东西；
//   · 只对**有网格**的实体生效 —— 灯 / 相机没有包围盒，静默忽略；
//   · 已经在贴地状态（|dz| 极小）就直接返回，不往撤销栈里塞空命令。
//
// 位移算的是**世界空间**：如果实体挂在父节点下，父节点的 TRS 会把这
// 段竖直位移旋转 / 缩放掉，所以最后要把它换算回父节点空间的位移
// （用 w = 0 的方向变换，不受父节点平移影响）。
void ViewportPanel::dropSelectionToGround() {
    if (!m_ctx.isEditing()) return;
    if (!m_ctx.hasSelection()) return;

    const ecs::Entity e = m_ctx.selection();
    const std::string* nmp = m_ctx.editorScene().world().name(e);
    const char* nm = nmp ? nmp->c_str() : "?";
    if (!m_ctx.selectionFromViewport()) {
        m_ctx.setStatus("Drop to floor: 先在 3D 视图里左键点选物体");
        VK_LOG_INFO("drop-to-floor: skipped ('%s' was not picked in the "
                    "viewport)", nm);
        return;
    }

    scene::Scene& sc = m_ctx.editorScene();
    auto* t = sc.world().get<ecs::TransformComponent>(e);
    if (!t) return;

    const Aabb box = m_picking.worldAabb(sc, e);
    if (!box.valid) {   // 灯 / 相机 / 没有网格
        VK_LOG_INFO("drop-to-floor: skipped ('%s' has no mesh bounds)", nm);
        return;
    }

    const float dz = -box.min.z;   // 世界空间里要把"底部"抬多高
    if (std::fabs(dz) < 1e-4f) {
        m_ctx.setStatus("Already on the ground");
        VK_LOG_INFO("drop-to-floor: '%s' already on the ground", nm);
        return;
    }

    const glm::vec3 p0 = t->position;
    const TransformSnapshot before = TransformSnapshot::capture(sc, e);

    glm::vec3 deltaWorld(0.0f, 0.0f, dz);
    if (const auto* h = sc.world().get<ecs::HierarchyComponent>(e)) {
        if (h->parent != ecs::kInvalidEntity) {
            const ecs::Entity parent = sc.world().handle(h->parent);
            deltaWorld = glm::vec3(
                glm::inverse(sc.worldMatrix(parent)) * glm::vec4(deltaWorld, 0.0f));
        }
    }
    t->position += deltaWorld;

    const TransformSnapshot after = TransformSnapshot::capture(sc, e);
    const std::string label = "Drop to floor " + std::string(nm);
    m_ctx.commands().pushAlreadyApplied(std::make_unique<TransformEditCommand>(
        label, &sc, e, before, after));
    m_ctx.setStatus(label);
    m_ctx.dirty() = true;
    // 一行断言用日志：自动化脚本靠它核对 dz 与"x / y 一个都没动"。
    VK_LOG_INFO("drop-to-floor: '%s' dz=%.4f pos=(%.3f,%.3f,%.3f)->"
                "(%.3f,%.3f,%.3f) bottom=%.4f",
                nm, dz, p0.x, p0.y, p0.z, t->position.x, t->position.y,
                t->position.z, box.min.z + dz);
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

// ---------------------------------------------------------------- Shift+A 添加菜单
//
// Blender 的 Add 菜单：鼠标停在视口里按 Shift+A，就在鼠标位置弹出
// "Mesh / Light" 两级菜单。菜单必须**每帧无条件提交**（BeginPopup），
// 否则 ImGui 会在没提交的帧直接把它关掉 —— 和 ContentBrowser 右键菜单
// 踩过的是同一个坑。
//
// 落点：打开菜单那一刻记下"鼠标射线 ∩ 地面（z=0）"（m_addSpot），菜单里
// 无论悬停多久、加什么，都落在这一点上 —— 网格底部贴地、灯光抬到半空。

void ViewportPanel::addEntity(const char* what,
                              const std::function<ecs::Entity()>& create) {
    ecs::Entity created{};
    m_ctx.structuralEdit(std::string("Add ") + what, [&]() { created = create(); });
    if (created.valid()) {
        m_ctx.select(created);
        m_ctx.notify(std::string("Added ") + what);
        VK_LOG_INFO("ViewportPanel: added '%s' via Shift+A menu", what);
    }
}

void ViewportPanel::drawAddMenu(const glm::vec2& vpPos, const glm::vec2& vpSize,
                                bool hovered) {
    if (!m_ctx.isEditing()) return;

    const ImGuiIO& io = ImGui::GetIO();

    // 菜单弹出物的矩形一律带上"第几次打开"的序号（VP-ADD-Mesh#3 这种）。
    // logRect 是按矩形去抖的 —— 同一个位置第二次打开菜单时矩形没变，一行
    // 都不会再打，自动化脚本就会拿到上一次的陈旧坐标，在子菜单展开之前
    // 就点下去（实测：加 Cube 成功、紧接着加 Sphere 必失败）。带序号之后
    // 每次打开都是新 tag，脚本取"最后一条"永远是新鲜的。
    const auto menuRect = [this](const char* tag, const ImVec2& a,
                                 const ImVec2& b) {
        const std::string t =
            std::string(tag) + "#" + std::to_string(m_addMenuSeq);
        logRect(t.c_str(), a, b);
    };

    // ---- 打开：视口被悬停 + Shift+A（按住右键驾驶相机时不抢键）----
    if (hovered && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_A, false) &&
        !ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
        ++m_addMenuSeq;
        // 自动化用（MYVK_LOG_RECTS=1）：打一行"菜单已打开 + 第几次"
        if (std::getenv("MYVK_LOG_RECTS")) {
            VK_LOG_INFO("VP-ADD-OPEN seq=%d at=(%.0f,%.0f)", m_addMenuSeq,
                        io.MousePos.x, io.MousePos.y);
        }
        // 落点 = 此刻鼠标射线与地面的交点（朝天时兜底：视线前方 5 米的地面上）
        const glm::vec2 mouse(io.MousePos.x, io.MousePos.y);
        const Ray ray = PickingSystem::rayFromViewport(
            m_ctx.activeScene().camera(), vpPos, vpSize, mouse);
        float t = 0.0f;
        if (PickingSystem::intersectPlane(ray, glm::vec3(0.0f),
                                          glm::vec3(0.0f, 0.0f, 1.0f), t) &&
            t > 0.0f) {
            m_addSpot = ray.origin + ray.dir * t;
            m_addSpot.z = 0.0f;
        } else {
            const scene::Camera& cam = m_ctx.activeScene().camera();
            glm::vec3 fwd = cam.target() - cam.position();
            if (glm::length(fwd) < 1e-4f) fwd = glm::vec3(0, -1, 0);
            m_addSpot = cam.position() + glm::normalize(fwd) * 5.0f;
            m_addSpot.z = 0.0f;
        }
        ImGui::OpenPopup("##vp_add");
    }

    // ---- 绘制：鼠标位置弹出，悬停展开子菜单，移开自动关闭 ----
    ImGui::SetNextWindowPos(io.MousePos, ImGuiCond_Appearing);
    if (!ImGui::BeginPopup("##vp_add")) return;

    // 自动化用（MYVK_LOG_RECTS=1）：弹出窗口与每个菜单项的真实矩形，
    // 脚本按矩形中心精确点击（不猜字体行高）。
    menuRect("VP-ADD-WIN", ImGui::GetWindowPos(),
             ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                    ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    ImGui::SeparatorText("Add");

    // ⚠ logRect 必须在 BeginMenu 之外无条件调用：BeginMenu 只有在子菜单
    //   展开时才返回 true，写在 if 里会导致"悬停前永远拿不到父项矩形"，
    //   自动化脚本就死锁在"等矩形 → 才知道悬停哪"上（实测踩过）。
    //   BeginMenu 无论返回值如何都会提交 item，GetItemRectMin/Max 始终有效。
    const bool meshOpen = ImGui::BeginMenu("Mesh");
    menuRect("VP-ADD-Mesh", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (meshOpen) {
        const std::function<ecs::Entity()> makers[] = {
            [this]() {
                return createPrimitiveOnGround(m_ctx.editorScene(),
                                               m_ctx.assets(),
                                               PrimitiveKind::Cube, m_addSpot);
            },
            [this]() {
                return createPrimitiveOnGround(m_ctx.editorScene(),
                                               m_ctx.assets(),
                                               PrimitiveKind::Sphere, m_addSpot);
            },
            [this]() {
                return createPrimitiveOnGround(m_ctx.editorScene(),
                                               m_ctx.assets(),
                                               PrimitiveKind::Cylinder,
                                               m_addSpot);
            },
            [this]() {
                return createPrimitiveOnGround(m_ctx.editorScene(),
                                               m_ctx.assets(),
                                               PrimitiveKind::Plane, m_addSpot);
            },
        };
        const char* names[] = {"Cube", "Sphere", "Cylinder", "Plane"};
        for (int i = 0; i < 4; ++i) {
            const bool clicked = ImGui::MenuItem(names[i]);
            menuRect((std::string("VP-ADD-") + names[i]).c_str(),
                     ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            if (clicked) {
                addEntity(names[i], makers[i]);
            }
        }
        ImGui::EndMenu();
    }

    // 同上：父项矩形无条件打（子菜单展开与否都要有，脚本靠它定位悬停点）
    const bool lightOpen = ImGui::BeginMenu("Light");
    menuRect("VP-ADD-Light", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (lightOpen) {
        // Sunlight：配置场景级太阳（有阴影、全局唯一），选中太阳实体
        {
            const bool clicked = ImGui::MenuItem("Sunlight");
            menuRect("VP-ADD-Sunlight", ImGui::GetItemRectMin(),
                     ImGui::GetItemRectMax());
            if (clicked) {
                addEntity("Sunlight", [this]() {
                    return createSunlight(m_ctx.editorScene());
                });
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("The scene's key light - simulates the sun.\n"
                              "Casts shadows, lights the whole world.");
        }
        // 实体级方向光：补光，可摆多盏，无阴影
        {
            const bool clicked = ImGui::MenuItem("Directional Light");
            menuRect("VP-ADD-DirectionalLight", ImGui::GetItemRectMin(),
                     ImGui::GetItemRectMax());
            if (clicked) {
                addEntity("Directional Light", [this]() {
                    return createDirectionalLight(m_ctx.editorScene(), m_addSpot);
                });
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("A secondary directional fill light\n"
                              "(no shadows, add as many as you like).");
        }
        {
            const bool clicked = ImGui::MenuItem("Spot Light");
            menuRect("VP-ADD-SpotLight", ImGui::GetItemRectMin(),
                     ImGui::GetItemRectMax());
            if (clicked) {
                addEntity("Spot Light", [this]() {
                    return createPrimitiveOnGround(m_ctx.editorScene(),
                                                   m_ctx.assets(),
                                                   PrimitiveKind::SpotLight,
                                                   m_addSpot);
                });
            }
        }
        {
            const bool clicked = ImGui::MenuItem("Point Light");
            menuRect("VP-ADD-PointLight", ImGui::GetItemRectMin(),
                     ImGui::GetItemRectMax());
            if (clicked) {
                addEntity("Point Light", [this]() {
                    return createPrimitiveOnGround(m_ctx.editorScene(),
                                                   m_ctx.assets(),
                                                   PrimitiveKind::PointLight,
                                                   m_addSpot);
                });
            }
        }
        ImGui::EndMenu();
    }

    ImGui::EndPopup();
}

// ---------------------------------------------------------------- 物体右键菜单
//
// 用户要的形态（原文）：
//   "当选中一个物体时，并且在该物体上按下鼠标右键打开一个菜单，其中有：
//    添加碰撞体功能 …… 有两种碰撞体结构，一个是按照物体几何结构，根据
//    当前业内最优秀的算法找到一个，另一种就是直接给一个胶囊体。"
//
// 两条路：凸包（QuickHull）与胶囊。菜单里都列出来，点了立刻生效并选中，
// Inspector 的 Collision 段落同时出现（参数可以继续改）。

void ViewportPanel::addCollisionBody(ecs::Entity e,
                                     ecs::ColliderShape shape) {
    if (!e.valid()) return;
    auto* mc = m_ctx.editorScene().world().get<ecs::MeshComponent>(e);
    if (!mc || !mc->mesh) {
        m_ctx.notify("No mesh to build a collider from");
        VK_LOG_WARN("addCollisionBody: entity has no mesh");
        return;
    }

    bool ok = false;
    m_ctx.structuralEdit(shape == ecs::ColliderShape::ConvexHull
                             ? "Add Convex Hull Collider"
                             : "Add Capsule Collider",
                         [&]() {
                             ecs::CollisionComponent cc;
                             ok = (shape == ecs::ColliderShape::ConvexHull)
                                      ? physics::setupHullCollider(*mc->mesh, cc)
                                      : physics::setupCapsuleCollider(*mc->mesh,
                                                                      cc);
                             if (!ok) return;
                             if (auto* p = m_ctx.editorScene()
                                               .world()
                                               .get<ecs::CollisionComponent>(e))
                                 *p = cc;
                             else
                                 m_ctx.editorScene().world().add<
                                     ecs::CollisionComponent>(e, cc);
                         });

    if (!ok) {
        m_ctx.notify("Collider build failed (empty mesh?)");
        return;
    }
    // 加完直接进入"编辑碰撞体"状态：手柄立刻作用在刚做出来的框上，
    // 不用再去 Inspector 里勾一下 —— 用户点"加碰撞体"就是想马上调它。
    m_ctx.setColliderEdit(true);
    const std::string* nm = m_ctx.editorScene().world().name(e);
    // 自动化断言靠这一行：形状 + 顶点数（凸包）/ 半径（胶囊）
    const auto* cc =
        m_ctx.editorScene().world().get<ecs::CollisionComponent>(e);
    VK_LOG_INFO("collision: added %s collider to '%s' points=%zu edges=%zu "
                "radius=%.3f halfHeight=%.3f pos=(%.3f,%.3f,%.3f)",
                shape == ecs::ColliderShape::ConvexHull ? "convex" : "capsule",
                nm ? nm->c_str() : "?", cc ? cc->hullPoints.size() : 0,
                cc ? cc->hullEdges.size() : 0,
                cc ? static_cast<double>(cc->capsuleRadius) : 0.0,
                cc ? static_cast<double>(cc->capsuleHalfHeight) : 0.0,
                cc ? static_cast<double>(cc->position.x) : 0.0,
                cc ? static_cast<double>(cc->position.y) : 0.0,
                cc ? static_cast<double>(cc->position.z) : 0.0);

    // "创建时默认刚好包裹住物体"的可断言形式：把**网格**在实体局部空间的
    // AABB 和**碰撞体**在实体局部空间的 AABB 都打出来。
    //   · 凸包：两者应当几乎相等（凸包是网格顶点的子集，AABB 只会更小一点点）
    //   · 胶囊：碰撞体应当完整**盖住**网格 AABB（半径取的是外接圆）
    if (cc) {
        glm::vec3 mmn, mmx, cmn, cmx;
        if (physics::meshAabb(*mc->mesh, mmn, mmx) &&
            physics::colliderLocalAabb(*cc, cmn, cmx)) {
            const glm::vec3 ms = mmx - mmn;
            const glm::vec3 cs = cmx - cmn;
            VK_LOG_INFO("collision: fit '%s' shape=%s mesh=(%.4f,%.4f,%.4f) "
                        "collider=(%.4f,%.4f,%.4f) "
                        "ratio=(%.4f,%.4f,%.4f)",
                        nm ? nm->c_str() : "?",
                        shape == ecs::ColliderShape::ConvexHull ? "convex"
                                                                : "capsule",
                        static_cast<double>(ms.x),
                        static_cast<double>(ms.y), static_cast<double>(ms.z),
                        static_cast<double>(cs.x), static_cast<double>(cs.y),
                        static_cast<double>(cs.z),
                        ms.x > 0.0f ? static_cast<double>(cs.x / ms.x) : 0.0,
                        ms.y > 0.0f ? static_cast<double>(cs.y / ms.y) : 0.0,
                        ms.z > 0.0f ? static_cast<double>(cs.z / ms.z) : 0.0);
        }
    }
    m_ctx.notify(std::string("Added ") +
                 (shape == ecs::ColliderShape::ConvexHull ? "convex hull"
                                                          : "capsule") +
                 " collider");
}

void ViewportPanel::removeCollisionBody(ecs::Entity e) {
    if (!e.valid()) return;
    if (!m_ctx.editorScene().world().has<ecs::CollisionComponent>(e)) return;
    const std::string* nm = m_ctx.editorScene().world().name(e);
    m_ctx.structuralEdit("Remove Collision Body", [&]() {
        m_ctx.editorScene().world().remove<ecs::CollisionComponent>(e);
    });
    m_ctx.setColliderEdit(false);
    VK_LOG_INFO("collision: removed collider from '%s'", nm ? nm->c_str() : "?");
    m_ctx.notify("Removed collision body");
}

void ViewportPanel::drawObjectMenu(const glm::vec2& vpPos,
                                   const glm::vec2& vpSize, bool hovered) {
    if (!m_ctx.isEditing()) return;

    const ImGuiIO& io = ImGui::GetIO();
    const glm::vec2 mouse(io.MousePos.x, io.MousePos.y);

    // 菜单矩形日志带序号（和 Add 菜单同一个理由：logRect 按矩形去抖，
    // 第二次在同一个位置打开菜单就一行都不会再打，脚本会拿到陈旧坐标）。
    const auto menuRect = [this](const char* tag, const ImVec2& a,
                                 const ImVec2& b) {
        const std::string t =
            std::string(tag) + "#" + std::to_string(m_ctxMenuSeq);
        logRect(t.c_str(), a, b);
    };

    // ---- 按下右键：先记下"按在哪儿、是不是按在选中物体上" ----
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        m_ctxPressPos = mouse;
        m_ctxPressOnSelected = false;
        if (m_ctx.hasSelection()) {
            const Ray ray = PickingSystem::rayFromViewport(
                m_ctx.activeScene().camera(), vpPos, vpSize, mouse);
            const PickResult hit = m_picking.pick(m_ctx.activeScene(), ray);
            m_ctxPressOnSelected = hit.hit && hit.entity == m_ctx.selection();
        }
    }

    // ---- 松开右键：**几乎没动**才算点击（动了就是右键拖拽转视角）----
    if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        const bool clicked = glm::length(mouse - m_ctxPressPos) < 6.0f;
        if (m_ctxPressOnSelected && clicked) {
            ++m_ctxMenuSeq;
            m_ctxMenuPos = mouse;
            if (std::getenv("MYVK_LOG_RECTS")) {
                VK_LOG_INFO("VP-OBJ-OPEN seq=%d at=(%.0f,%.0f)", m_ctxMenuSeq,
                            static_cast<double>(mouse.x),
                            static_cast<double>(mouse.y));
            }
            ImGui::OpenPopup("##vp_object");
        }
        m_ctxPressOnSelected = false;
    }

    // 每帧无条件提交（不提交的帧 ImGui 会直接把 popup 关掉）
    ImGui::SetNextWindowPos(ImVec2(m_ctxMenuPos.x, m_ctxMenuPos.y),
                            ImGuiCond_Appearing);
    if (!ImGui::BeginPopup("##vp_object")) return;

    ecs::Entity e = m_ctx.selection();
    if (!e.valid()) {
        ImGui::EndPopup();
        return;
    }
    ecs::World& w = m_ctx.editorScene().world();
    auto* cc = w.get<ecs::CollisionComponent>(e);
    const std::string* nm = w.name(e);

    menuRect("VP-OBJ-WIN", ImGui::GetWindowPos(),
             ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                    ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    ImGui::SeparatorText(nm ? nm->c_str() : "Object");

    // 父项矩形必须**无条件**打点（BeginMenu 只在子菜单展开时才返回 true，
    // 写在 if 里脚本会永远等不到它）—— 见 drawAddMenu 里的同一条注释。
    const bool addOpen = ImGui::BeginMenu("Add Collision Body");
    menuRect("VP-OBJ-Add", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (addOpen) {
        const bool c1 = ImGui::MenuItem("Convex Hull (QuickHull)");
        menuRect("VP-OBJ-AddConvex", ImGui::GetItemRectMin(),
                 ImGui::GetItemRectMax());
        if (c1) addCollisionBody(e, ecs::ColliderShape::ConvexHull);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Fits a convex hull around the mesh (QuickHull).\n"
                "Tightest fit - the industry-standard collision shape.");
        }

        const bool c2 = ImGui::MenuItem("Capsule");
        menuRect("VP-OBJ-AddCapsule", ImGui::GetItemRectMin(),
                 ImGui::GetItemRectMax());
        if (c2) addCollisionBody(e, ecs::ColliderShape::Capsule);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "A capsule wrapping the object's bounds.\n"
                "Cheap and smooth - good for characters.");
        }
        ImGui::EndMenu();
    }

    const bool hasCollider = (cc != nullptr);
    if (ImGui::MenuItem("Remove Collision Body", nullptr, false, hasCollider)) {
        removeCollisionBody(e);
    }
    menuRect("VP-OBJ-Remove", ImGui::GetItemRectMin(),
             ImGui::GetItemRectMax());

    ImGui::Separator();

    bool editingColl = m_ctx.colliderEdit();
    if (ImGui::MenuItem("Edit Collider with W/E/R", nullptr, &editingColl,
                        hasCollider)) {
        m_ctx.setColliderEdit(editingColl);
        VK_LOG_INFO("collision: gizmo target -> %s",
                    editingColl ? "collider" : "entity");
    }
    menuRect("VP-OBJ-EditCollider", ImGui::GetItemRectMin(),
             ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered() && !hasCollider) {
        ImGui::SetTooltip("Add a collision body first");
    }

    if (hasCollider) {
        bool solid = cc->solid;
        if (ImGui::MenuItem("Solid (blocks other colliders)", nullptr, &solid)) {
            m_ctx.structuralEdit("Toggle Collider Solid", [&]() {
                if (auto* p = w.get<ecs::CollisionComponent>(e))
                    p->solid = solid;
            });
        }
        menuRect("VP-OBJ-Solid", ImGui::GetItemRectMin(),
                 ImGui::GetItemRectMax());

        ImGui::SeparatorText(
            cc->shape == ecs::ColliderShape::ConvexHull ? "convex hull"
                                                        : "capsule");
    }

    ImGui::EndPopup();
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

    // 2) 碰撞框线框（**先画**，让手柄压在它上面；X-ray，不参与深度测试）
    if (m_ctx.isEditing()) m_colliders.draw(m_ctx, vpPos, vpSize, dl, hovered);

    // 3) gizmo（先跑：它可能在本次点击里"吃掉"鼠标）。只在编辑态有效
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
        // 先问"是不是点在**选中物体的碰撞框**那圈线上"。
        //
        // 语义：点在框的**轮廓**上 = "我要调这个碰撞框"；点在物体**中间** =
        // 常规点选（手柄回到实体）。这个区分很自然 —— 碰撞框默认贴着模型，
        // 只有轮廓附近才可能命中，所以"点中心回到编辑物体"永远可用。
        if (m_ctx.hasSelection() &&
            m_colliders.hitSelectedWireframe(m_ctx, vpPos, vpSize, mouse)) {
            if (!m_ctx.colliderEdit()) {
                m_ctx.setColliderEdit(true);
                VK_LOG_INFO("pick: collider wireframe hit -> gizmo target = "
                            "collider");
                m_ctx.setStatus("Editing collision body (W/E/R)");
            }
        } else {
            const Ray ray =
                PickingSystem::rayFromViewport(cam, vpPos, vpSize, mouse);
            const PickResult hit = m_picking.pick(m_ctx.activeScene(), ray);
            if (hit.hit) {
                // 记下"这是在 3D 视口里点中的"——数字键 0 的落地只认这种选中
                m_ctx.selectFromViewport(hit.entity);
                const std::string* n =
                    m_ctx.activeScene().world().name(hit.entity);
                m_ctx.setStatus("Selected " +
                                (n ? *n : std::string("Object")));
                // 打一行日志：自动化脚本靠它断言"这一下点中了谁 / 有没有点中"
                // （比如点在地面上应当什么都不选中 —— 地面是 LockedComponent）。
                VK_LOG_INFO("pick: selected '%s' (from viewport)",
                            n ? n->c_str() : "?");
            } else {
                m_ctx.clearSelection();
                VK_LOG_INFO("pick: nothing hit -> selection cleared");
            }
        }
    }

    // 3.5) Shift+A 添加菜单（仅编辑态；每帧无条件提交 popup）
    drawAddMenu(vpPos, vpSize, hovered);

    // 3.6) 选中物体上的右键菜单：添加 / 移除碰撞体、切换手柄目标
    drawObjectMenu(vpPos, vpSize, hovered);

    // 4) 叠加信息
    if (m_showOverlay) drawOverlay(dl, vpPos, vpSize);

    // 5) 内容浏览器拖进来的**网格** → 就地实例化。
    //    落点 = 鼠标射线与 z = 0 栅格面的交点（拖到哪儿就落在哪儿，不是
    //    视口中心）；实例化之后按世界包围盒把**底部刚好贴到栅格面**上，
    //    水平中心对准落点 —— 这就是"拖进来就稳稳站在地上"。
    if (ImGui::BeginDragDropTarget()) {
        // 拖着一个资产悬在视口上方时，给一圈高亮 + 一句落点说明
        if (const ImGuiPayload* drag = ImGui::GetDragDropPayload()) {
            if (drag->IsDataType(drag::kAsset)) {
                dl->AddRect(imgMin, ImVec2(imgMin.x + imgSz.x, imgMin.y + imgSz.y),
                            IM_COL32(104, 190, 132, 210), 0.0f, 0, 3.0f);
                dl->AddText(ImVec2(imgMin.x + 14.0f, imgMin.y + imgSz.y - 30.0f),
                            IM_COL32(160, 230, 180, 255),
                            "release to place it on the grid (bottom snaps "
                            "to z = 0)");
            }
        }

        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(drag::kAsset)) {
            const char* path = static_cast<const char*>(p->Data);
            const std::string rel = path ? path : std::string();

            const glm::vec2 mouse(io.MousePos.x, io.MousePos.y);
            const Ray ray =
                PickingSystem::rayFromViewport(cam, vpPos, vpSize, mouse);
            glm::vec3 spot(0.0f);
            float t = 0.0f;
            if (PickingSystem::intersectPlane(ray, glm::vec3(0.0f),
                                              glm::vec3(0.0f, 0.0f, 1.0f),
                                              t) &&
                t > 0.0f) {
                spot = ray.origin + ray.dir * t;
                spot.z = 0.0f;
            } else {
                // 相机朝上（射线与地面同向）时兜底：落在视线方向上 5 米处的地面
                glm::vec3 fwd = cam.target() - cam.position();
                if (glm::length(fwd) < 1e-4f) fwd = glm::vec3(0, -1, 0);
                spot = cam.position() + glm::normalize(fwd) * 5.0f;
                spot.z = 0.0f;
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
    //   编辑态 → 导航 HUD（灵敏度 + 操作提示）+ 左下角导航球，跟随 "HUD" 勾选框
    //   运行态 → 极简播放浮层（全屏按钮 + 状态），不含任何导航控件
    if (m_ctx.isEditing()) {
        if (m_showOverlay) {
            drawNavHud(vpPos, vpSize);
            drawNavGizmo(vpPos, vpSize);
        }
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
    // 浮层只有三行：标题 + 两个控件行（灵敏度、栅格开关）。
    // 操作提示文字（"RMB drag look" 那三行）和「Reset 1.00」按钮按用户要求
    // 去掉了 —— 视口里只留真正需要拨一下的东西；操作说明在 Help 菜单里。
    //
    // 尺寸跟着当前字号算。字体缩放是用户可调的（Settings 面板），
    // 写死像素的话一放大文字就会被裁掉半截 —— 这个坑第一版就踩过。
    const ImGuiStyle& st = ImGui::GetStyle();
    const float frameH = ImGui::GetFrameHeightWithSpacing();

    float textW = ImGui::CalcTextSize("Viewport navigation").x;
    // 灵敏度那一行 = 数值框 + "Sensitivity" 标签（Reset 按钮已移除）
    textW = std::max(textW, kSensitivityFieldW + st.ItemInnerSpacing.x +
                                 ImGui::CalcTextSize("Sensitivity").x);
    // 栅格那一行是 "勾选框 + 标签 + 副标题"
    textW = std::max(textW, ImGui::CalcTextSize("Ground grid (1 m)").x +
                                 ImGui::CalcTextSize("(z = 0 plane)").x + 44.0f);

    const float cw = textW + st.WindowPadding.x * 2.0f + 8.0f;
    // 1 行文字（标题）+ 2 个控件行（灵敏度、栅格勾选框）
    const float ch = ImGui::GetTextLineHeightWithSpacing() + frameH * 2.0f +
                     st.WindowPadding.y * 2.0f + st.ItemSpacing.y * 2.0f;

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
    // InputFloat 让用户直接填浮点（点/拖都能改）。
    // **step 传 0 就不会画那两个 +/- 方块按钮** —— 它们又占地方又把文本区
    // 挤窄，导致 "20.00" 这种上限值显示不全（用户反馈的正是这个）。
    // 约束必须 > 0：非正数（或输入过程中的乱值）一律忽略，下一帧会回显上一个合法值。
    float sens = m_ctx.navSensitivity();
    ImGui::SetNextItemWidth(kSensitivityFieldW);
    if (ImGui::InputFloat("Sensitivity", &sens, 0.0f, 0.0f, "%.2f")) {
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

    // ---- 地平面栅格 ----
    // 用户要的"视图右上角的单选框"。默认开；真正的绘制在 render/Renderer
    // 的 grid pass（最小格 1 m、主格 10 m），这里只管开关。
    // Play 期间栅格不画（视口里是游戏画面），所以这个勾选框只在编辑态出现。
    ImGui::Checkbox("Ground grid (1 m)", &m_ctx.showGridRef());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Reference grid on the z = 0 plane.\n"
            "Minor lines every 1 m, major lines every 10 m.\n"
            "Hidden while playing (the viewport shows the game then).\n"
            "Start with MYVK_GRID=0 to force it off.");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(z = 0 plane)");

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------- 左下角导航球
//
// Blender 3D 视口左下角那个 Navigation Gizmo 的复刻：
//   · 三根世界轴按**当前相机朝向**投影成一个"米"字，轴端各一个带字母的小球；
//   · 拖拽 = 绕当前注视点自由旋转视角；
//   · 点击小球 = 吸附到那个正视图（点蓝色 +Z 球就是俯视图）；
//   · 悬停高亮 + 名称提示。
//
// 配色按用户要求：红 = X、绿 = Y、蓝 = Z。引擎现在是 Z 朝上的右手系，
// 和 Blender 的轴向习惯一致 —— 蓝球是垂直轴，红/绿在水平面里。
//
// 正轴小球 = 实心轴色 + 白字；负轴小球 = 深色底 + 轴色描边 + 轴色字
// （Blender 区分正负端的方式）。两端在"轴正对镜头"时会投影到同一点，
// 所以按"朝向观察者的深度"从远到近排序绘制，近的自然压在上面；
// 命中判定也优先取更朝前的那个。
//
// 整块方形区域是一个隐形按钮，所以**方形里圆盘之外的四个角**也会吞掉
// 鼠标（不会穿透去点选物体）—— Blender 的控件区也是这个行为。
void ViewportPanel::drawNavGizmo(const glm::vec2& vpPos, const glm::vec2& vpSize) {
    const float half = kGizmoRingR + kGizmoBallR + 5.0f;
    const float box = half * 2.0f;
    // 贴着视口图像左下角
    const glm::vec2 c(vpPos.x + kGizmoMargin + half,
                      vpPos.y + vpSize.y - kGizmoMargin - half);

    ImGui::SetCursorScreenPos(ImVec2(c.x - half, c.y - half));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::BeginChild("##nav_gizmo", ImVec2(box, box), 0,
                      ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##nav_gizmo_hit", ImVec2(box, box));
    const bool active = ImGui::IsItemActive();
    const bool pressed = ImGui::IsItemActivated();
    const bool released = ImGui::IsItemDeactivated();
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 cc(origin.x + half, origin.y + half);

    // ---- 把三根世界轴投影到屏幕上 ----
    // 相机基：r = 右、u = 上（= cross(r, forward)，不是 worldUp）、f = 视线方向。
    // 世界方向 d 的屏幕偏移 = (d·r, -d·u) * 半径（屏幕 y 向下）；
    // 深度 = -d·f，越大越朝向观察者。
    const scene::Camera& cam = m_ctx.activeScene().camera();
    const glm::vec3 f = cam.forwardAxis();
    const glm::vec3 r = cam.rightAxis();
    const glm::vec3 u = glm::cross(r, f);

    struct Ball {
        int dir;      // ViewDir
        int axis;     // 0/1/2
        int sign;     // +1 / -1
        ImVec2 pos;
        float depth;
    };
    static const glm::vec3 kAxes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    Ball balls[6];
    for (int a = 0; a < 3; ++a) {
        for (int s = 0; s < 2; ++s) {
            const int i = a * 2 + s;  // 顺序 = ViewDir 下标
            const float sg = (s == 0) ? 1.0f : -1.0f;
            const glm::vec3 d = kAxes[a] * sg;
            balls[i].dir = i;
            balls[i].axis = a;
            balls[i].sign = (s == 0) ? 1 : -1;
            balls[i].pos = ImVec2(cc.x + glm::dot(d, r) * kGizmoRingR,
                                  cc.y - glm::dot(d, u) * kGizmoRingR);
            balls[i].depth = -glm::dot(d, f);
        }
    }

    // 自动化用（MYVK_LOG_RECTS=1）：三个正轴小球的中心。
    // 脚本靠它点击导航球 —— 之前是在截图里扫"偏蓝的像素"，结果误命中了
    // Content 面板的蓝色像素，点了个空，测试一直假失败（实测踩过）。
    if (std::getenv("MYVK_LOG_RECTS")) {
        static const char* kAxisNames[3] = {"X", "Y", "Z"};
        for (int a = 0; a < 3; ++a) {
            const Ball& b = balls[a * 2];   // s = 0 → 正轴端
            logRect((std::string("NAV-BALL+") + kAxisNames[a]).c_str(),
                    ImVec2(b.pos.x - kGizmoBallR, b.pos.y - kGizmoBallR),
                    ImVec2(b.pos.x + kGizmoBallR, b.pos.y + kGizmoBallR));
        }
    }

    const auto ballR = [](int sign) {
        return sign > 0 ? kGizmoBallR : kGizmoBallNeg;
    };

    // ---- 命中：朝观察者越近越优先 ----
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    int hot = -1;
    if (hovered && !m_navDragging) {
        float bestDepth = -2.0f;
        for (int i = 0; i < 6; ++i) {
            const float rr = ballR(balls[i].sign) + 3.0f;
            const float dx = mouse.x - balls[i].pos.x;
            const float dy = mouse.y - balls[i].pos.y;
            if (dx * dx + dy * dy <= rr * rr && balls[i].depth > bestDepth) {
                bestDepth = balls[i].depth;
                hot = i;
            }
        }
    }

    // ---- 交互：按下记候选，松手时"没怎么动"才算点击 ----
    if (pressed) {
        // 自动化诊断：按下了但 hot == -1 就说明没命中任何小球（点击落空）。
        if (std::getenv("MYVK_LOG_RECTS")) {
            VK_LOG_INFO("NAV-GIZMO press hovered=%d hot=%d mouse=(%.0f,%.0f)",
                        hovered ? 1 : 0, hot, mouse.x, mouse.y);
        }
        m_navDragging = true;
        m_navMoved = false;
        m_navPressDir = hot;
        m_navPressPos = glm::vec2(mouse.x, mouse.y);
    }

    if (m_navDragging && active) {
        const ImVec2 md = ImGui::GetIO().MouseDelta;
        if (md.x != 0.0f || md.y != 0.0f) {
            const glm::vec2 cur(mouse.x, mouse.y);
            if (glm::length(cur - m_navPressPos) > kGizmoClickSlop) {
                m_navMoved = true;
            }
            const float k = kNavLookPerPixel * m_ctx.navSensitivity();
            orbitViewAroundPivot(m_ctx.activeScene().camera(), md.x * k,
                                 md.y * k);
        }
    }

    if (released) {
        if (!m_navMoved && m_navPressDir >= 0) {
            snapViewTo(m_ctx.activeScene().camera(), m_navPressDir);
            m_ctx.setStatus(std::string("View: ") + viewDirName(m_navPressDir));
        }
        m_navDragging = false;
        m_navPressDir = -1;
    }

    // ---- 绘制 ----
    // 底盘：半透明圆盘 + 细描边，保证在亮/暗背景上都读得出来
    dl->AddCircleFilled(cc, half - 2.0f, IM_COL32(14, 16, 21, 130), 56);
    dl->AddCircle(cc, half - 2.0f, IM_COL32(124, 134, 150, 92), 56, 1.0f);

    // 远 → 近
    int order[6] = {0, 1, 2, 3, 4, 5};
    std::sort(order, order + 6,
              [&](int a, int b) { return balls[a].depth < balls[b].depth; });

    for (int k = 0; k < 6; ++k) {
        const Ball& b = balls[order[k]];
        const bool isHot =
            (order[k] == hot) || (m_navDragging && b.dir == m_navPressDir);

        // 背对观察者的一端压暗一点，做出前后层次
        const float a =
            0.55f + 0.45f * glm::clamp((b.depth + 1.0f) * 0.5f, 0.0f, 1.0f);
        const ImU32 col = kNavAxisCol[b.axis];
        const float rad = ballR(b.sign);

        dl->AddLine(cc, b.pos, withAlpha(col, a * 0.85f), 2.0f);
        if (b.sign > 0) {
            dl->AddCircleFilled(b.pos, rad, withAlpha(col, a));
            dl->AddCircle(b.pos, rad, IM_COL32(20, 22, 28, 200), 24, 1.5f);
        } else {
            dl->AddCircleFilled(b.pos, rad, IM_COL32(26, 29, 36, 235));
            dl->AddCircle(b.pos, rad, withAlpha(col, a), 24, 1.7f);
        }
        if (isHot) {
            dl->AddCircle(b.pos, rad + 2.5f, IM_COL32(252, 218, 62, 235), 24,
                          2.0f);
        }

        const char* letter = (b.axis == 0) ? "X" : (b.axis == 1) ? "Y" : "Z";
        const ImVec2 ts = ImGui::CalcTextSize(letter);
        const ImU32 txtCol = (b.sign > 0)
                                 ? IM_COL32(252, 253, 255, 245)
                                 : withAlpha(col, glm::max(a, 0.80f));
        dl->AddText(ImVec2(b.pos.x - ts.x * 0.5f, b.pos.y - ts.y * 0.5f),
                    txtCol, letter);
    }

    // 圆心小点：三根轴才不会看着"悬空"
    dl->AddCircleFilled(cc, 3.0f, IM_COL32(210, 216, 226, 220));

    if (hot >= 0) {
        ImGui::SetTooltip("%s\nClick: snap to this view\nDrag: orbit view",
                          viewDirName(balls[hot].dir));
    }

    // 相机姿态日志（MYVK_LOG_RECTS 通道）：视图吸附 / 数字键切视图后，
    // 自动化脚本靠它断言 yaw/pitch 是否真的到位。变化 > 0.02 rad 才打。
    if (std::getenv("MYVK_LOG_RECTS")) {
        static float lastYaw = 0.0f, lastPitch = 0.0f;
        static bool first = true;
        const float yaw = cam.yaw(), pitch = cam.pitch();
        if (first || std::fabs(yaw - lastYaw) > 0.02f ||
            std::fabs(pitch - lastPitch) > 0.02f) {
            VK_LOG_INFO("NAV-CAM yaw=%.4f pitch=%.4f", yaw, pitch);
            lastYaw = yaw;
            lastPitch = pitch;
            first = false;
        }
    }

    ImGui::EndChild();
}

// ---------------------------------------------------------------- 视图快捷键
//
// Blender 小键盘那一套，映射到本引擎（Z-up，与 Blender 同向）：
//   1 / Ctrl+1 = Front (-Y) / Back  (+Y)
//   3 / Ctrl+3 = Right (+X) / Left  (-X)
//   7 / Ctrl+7 = Top   (+Z) / Bottom(-Z)
//   4 / 6 / 8 / 2 = 视角左转 / 右转 / 上抬 / 下压 15°（可长按连发）
// 主键盘上排数字同样可用 —— 不少笔记本没有独立小键盘。
//
// 两边都只换朝向、**绕当前注视点**转（见 snapViewTo），所以切换后画面里
// 那个被观察的东西仍然居中，不会一下飞出视野。
//
// 未实现的两条（本引擎暂时没有对应概念，按了不响应）：
//   Numpad 5 = 正交/透视切换（相机只有透视）
//   Numpad 0 = 切到场景相机视图（编辑器没有"场景相机"这个实体）
void ViewportPanel::handleViewShortcuts() {
    if (!m_ctx.isEditing()) return;

    const ImGuiIO& io = ImGui::GetIO();
    const bool ctrl = io.KeyCtrl;
    scene::Camera& cam = m_ctx.activeScene().camera();

    // 大小键盘任一即可
    const auto hit = [](ImGuiKey pad, ImGuiKey row) {
        return ImGui::IsKeyPressed(pad, false) ||
               ImGui::IsKeyPressed(row, false);
    };

    int dir = -1;
    if (hit(ImGuiKey_Keypad1, ImGuiKey_1)) {
        dir = ctrl ? kViewPosY : kViewNegY;
    } else if (hit(ImGuiKey_Keypad3, ImGuiKey_3)) {
        dir = ctrl ? kViewNegX : kViewPosX;
    } else if (hit(ImGuiKey_Keypad7, ImGuiKey_7)) {
        dir = ctrl ? kViewNegZ : kViewPosZ;
    }

    if (dir >= 0) {
        snapViewTo(cam, dir);
        m_ctx.setStatus(std::string("View: ") + viewDirName(dir));
        return;
    }

    // 15° 步进（Blender：Numpad 4/6 = 左右转，8/2 = 上下转）。
    // 用 repeat=true 所以按住会连续转；Ctrl 组合留给视图吸附，不参与步进。
    if (ctrl) return;
    constexpr float kStep = 0.2617994f;  // 15°
    float dyaw = 0.0f, dpitch = 0.0f;
    if (ImGui::IsKeyPressed(ImGuiKey_Keypad4, true)) dyaw -= kStep;
    if (ImGui::IsKeyPressed(ImGuiKey_Keypad6, true)) dyaw += kStep;
    if (ImGui::IsKeyPressed(ImGuiKey_Keypad8, true)) dpitch -= kStep;
    if (ImGui::IsKeyPressed(ImGuiKey_Keypad2, true)) dpitch += kStep;
    if (dyaw != 0.0f || dpitch != 0.0f) orbitViewAroundPivot(cam, dyaw, dpitch);
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
