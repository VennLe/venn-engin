#include "ViewportPanel.h"

#include "Localization.h"
#include "DebugRects.h"    // MYVK_LOG_RECTS：把视口矩形打给自动化脚本
#include "EditorDragDrop.h"
#include "FeatureUIGen.h"   // emitParamChange：改 grid.* 后要标脏 + 记撤销
#include "EditorFocus.h"
#include "EditorScene.h"  // instantiateModel
#include "WaterEditor.h"
#include "LandscapeEditor.h"   // 编辑模式：地形笔刷 / 高度查询
#include "FoliageEditor.h"     // 编辑模式：植被撒点
#include "ToolkitEditors.h"  // 编辑模式：顶点色绘制 / 刷子体放置

#include "render/Renderer.h"
#include "render/FeatureRegistry.h"   // E14：视口设置直接写 grid.* 参数

#include "ui/ImGuiTheme.h"

#include "ecs/Components.h"
#include "physics/CollisionWorld.h"
#include "scene/Camera.h"
#include "scene/Scene.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace editor {

namespace {

// 面板最小渲染尺寸：再小就退化成 1×1 的图，重建开销反而不划算
constexpr float kMinViewport = 64.0f;

// ---- 视口叠加片（模式条 / 统计 HUD / 播放 HUD）的文字色 ----
// 这些贴片的底**故意是深色且与主题无关** —— 它们压在任意 3D 画面上（可能
// 是纯白天空），只有"深底 + 浅字"才在每套主题下都保证对比度；UE / Unity /
// Blender 的视口叠加层都是这么做的。
// 于是**浅色主题**下必须反过来把文字强制成浅色：主题的 ImGuiCol_Text 在
// 浅色主题里是深色，"深底 + 深字"直接看不见。
// ⚠ 深色主题下**一个都不推** → 与改动前逐像素一致（默认主题基线不动）。
constexpr ImVec4 kOverlayText = ImVec4(0.88f, 0.91f, 0.95f, 1.0f);
constexpr ImVec4 kOverlayTextDim = ImVec4(0.64f, 0.68f, 0.74f, 1.0f);

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
//
// 2026-10-22（第十轮）：0.1 → 0.25。用户反馈"右键 + 滚轮调视口移动速度
// 那个可以灵敏度略高一些"—— 原来每格只加 0.1，从 1.0 调到 3.0 要滚 20 格，
// 手感像"滚了没反应"。放大到 0.25 之后 8 格就到 3.0。
// 上限仍是 `kMaxSensitivity`（20），所以调不飞。
constexpr float kSensitivityStep = 0.25f;
// Sensitivity 数值框的宽度。这个框是 InputFloat **不带 step 按钮**的，
// 所以要能放下 "20.00"（上限）再加一点余量 —— 带按钮的版本会被两个
// 方块挤掉一半宽度，数字直接显示不全（这是本次要修的问题）。
constexpr float kSensitivityFieldW = 88.0f;

// 工具下拉弹层的**目标宽度**。搜索框、行宽都以它为基准 —— 见 drawToolMenu
// 里"不能从 GetWindowWidth 反推"那段说明。
constexpr float kToolMenuW = 268.0f;

// ---- 下拉按钮的"最近一次矩形"中转站 ----
//
// vpIconButton / vpValueButton / vpTextButton 是匿名命名空间里的自由函数，
// 拿不到 ViewportPanel 的成员，而 openDropdown 又必须在**按钮刚返回**时
// 知道它的矩形。不能读 GetItemRect*：这几个函数内部在 hover 时会 SetTooltip
// （StartTooltip 会开一个新窗口），LastItemData 就不一定还是那颗按钮了。
// 所以让按钮自己把矩形记到这儿 —— 一行、确定、没有隐式依赖。
ImVec2 g_lastBtnMin(0.0f, 0.0f);
ImVec2 g_lastBtnMax(0.0f, 0.0f);
void noteButtonRect(const ImVec2& p, const ImVec2& q) {
    g_lastBtnMin = p;
    g_lastBtnMax = q;
}

// 给**刚刚提交的那个控件**打一条矩形日志。
// logRect 的 tag 参数签名要求 (tag, a, b)，这里只是省掉重复的
// GetItemRectMin/Max 两行 —— 弹层里要打十几次，写全太吵。
void tagItem(const char* tag) {
    logRect(tag, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

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
//
// 2026-10-17 晚：数值从"三个预设表"扩成工具条上四个下拉的列表。
// 四个下拉各管一件事，而且**都绑定到真实存储**：
//   move  → EditorContext::snapMoveStep（米）
//   grid  → render 的 grid.minorStep（米，粗格自动 ×10）
//   rotate→ EditorContext::snapRotateStep（度）
//   scale → EditorContext::snapScaleStep（倍率）
const float kSnapMovePresets[] = {0.01f, 0.1f, 0.5f, 1.0f, 5.0f, 10.0f, 100.0f};
const float kSnapGridPresets[] = {0.1f, 0.25f, 0.5f, 1.0f, 5.0f, 10.0f};
const float kSnapRotPresets[] = {1.0f, 5.0f, 10.0f, 15.0f, 30.0f, 45.0f, 90.0f};
const float kSnapScalePresets[] = {0.01f, 0.05f, 0.1f, 0.25f, 0.5f, 1.0f};

// 工具条上的一格图标（矢量绘制，不依赖图标字体）。
enum VpIcon {
    kVpIconSelect = 0,
    kVpIconMove,
    kVpIconRotate,
    kVpIconScale,
    kVpIconGlobe,
    kVpIconReset,
    kVpIconSnapMove,
    kVpIconSnapGrid,
    kVpIconSnapRot,
    kVpIconSnapScale,
    kVpIconEye,
    kVpIconGear,
    kVpIconGrid,
    kVpIconCaret,
    kVpIconAxis,
    kVpIconPlus,   // "+"：工具条末尾那颗 add_object（2026-10-21 从顶栏搬来）
};

// 把图标画进 [p, p+s] 这个方块里。坐标系是"单位正方形缩放到 s"，
// 所以同一份代码在 16px 和 24px 下都成立。
void paintVpIcon(ImDrawList* dl, ImVec2 p, float s, int kind, ImU32 c) {
    const auto X = [&](float u) { return p.x + u * s; };
    const auto Y = [&](float v) { return p.y + v * s; };
    const ImVec2 ctr(X(0.5f), Y(0.5f));
    const float th = std::max(1.2f, s * 0.075f);   // 线宽跟着尺寸走

    switch (kind) {
        case kVpIconSelect: {
            // 鼠标指针：主三角 + 下面的尾巴（两段都是凸的，可以分别填充）
            const ImVec2 a(X(0.30f), Y(0.13f));
            const ImVec2 b(X(0.30f), Y(0.80f));
            const ImVec2 cc(X(0.53f), Y(0.60f));
            dl->AddTriangleFilled(a, b, cc, c);
            const ImVec2 t0(X(0.47f), Y(0.62f));
            const ImVec2 t1(X(0.58f), Y(0.88f));
            const ImVec2 t2(X(0.69f), Y(0.82f));
            const ImVec2 t3(X(0.58f), Y(0.57f));
            dl->AddQuadFilled(t0, t1, t2, t3, c);
            break;
        }
        case kVpIconMove: {
            // 四向箭头
            dl->AddLine(ImVec2(X(0.5f), Y(0.18f)), ImVec2(X(0.5f), Y(0.82f)), c, th);
            dl->AddLine(ImVec2(X(0.18f), Y(0.5f)), ImVec2(X(0.82f), Y(0.5f)), c, th);
            dl->AddTriangleFilled(ImVec2(X(0.5f), Y(0.08f)), ImVec2(X(0.40f), Y(0.24f)),
                                  ImVec2(X(0.60f), Y(0.24f)), c);
            dl->AddTriangleFilled(ImVec2(X(0.5f), Y(0.92f)), ImVec2(X(0.40f), Y(0.76f)),
                                  ImVec2(X(0.60f), Y(0.76f)), c);
            dl->AddTriangleFilled(ImVec2(X(0.08f), Y(0.5f)), ImVec2(X(0.24f), Y(0.40f)),
                                  ImVec2(X(0.24f), Y(0.60f)), c);
            dl->AddTriangleFilled(ImVec2(X(0.92f), Y(0.5f)), ImVec2(X(0.76f), Y(0.40f)),
                                  ImVec2(X(0.76f), Y(0.60f)), c);
            break;
        }
        case kVpIconRotate:
        case kVpIconSnapRot: {
            // 圆弧 + 箭头（顺时针）
            dl->PathClear();
            dl->PathArcTo(ctr, s * 0.33f, 0.55f, 5.10f, 24);
            dl->PathStroke(c, 0, th);
            const ImVec2 tip(X(0.72f), Y(0.20f));
            dl->AddTriangleFilled(tip, ImVec2(X(0.52f), Y(0.18f)),
                                  ImVec2(X(0.66f), Y(0.34f)), c);
            break;
        }
        case kVpIconScale:
        case kVpIconSnapScale: {
            // 外框 + 一道斜向箭头 = 缩放
            dl->AddRect(ImVec2(X(0.16f), Y(0.30f)), ImVec2(X(0.70f), Y(0.84f)), c,
                        0.0f, 0, th);
            dl->AddLine(ImVec2(X(0.34f), Y(0.66f)), ImVec2(X(0.78f), Y(0.22f)), c, th);
            dl->AddTriangleFilled(ImVec2(X(0.86f), Y(0.14f)),
                                  ImVec2(X(0.62f), Y(0.20f)),
                                  ImVec2(X(0.80f), Y(0.38f)), c);
            break;
        }
        case kVpIconGlobe: {
            // 球 + 一条经线（竖直椭圆）+ 一条纬线（水平椭圆）= 坐标系
            dl->AddCircle(ctr, s * 0.34f, c, 20, th);
            dl->PathClear();
            dl->PathEllipticalArcTo(ctr, ImVec2(s * 0.15f, s * 0.34f), 0.0f, 0.0f,
                                    6.2832f, 20);
            dl->PathStroke(c, 0, th * 0.8f);
            dl->PathClear();
            dl->PathEllipticalArcTo(ctr, ImVec2(s * 0.34f, s * 0.12f), 0.0f, 0.0f,
                                    6.2832f, 20);
            dl->PathStroke(c, 0, th * 0.8f);
            break;
        }
        case kVpIconReset: {
            // 逆时针回转箭头 + 中心点 = "恢复"
            dl->PathClear();
            dl->PathArcTo(ctr, s * 0.30f, 2.60f, 6.0f, 20);
            dl->PathStroke(c, 0, th);
            dl->AddTriangleFilled(ImVec2(X(0.22f), Y(0.46f)),
                                  ImVec2(X(0.44f), Y(0.36f)),
                                  ImVec2(X(0.24f), Y(0.66f)), c);
            dl->AddCircleFilled(ctr, s * 0.06f, c, 8);
            break;
        }
        case kVpIconSnapMove: {
            // 两个对齐点 + 中间的连线 = 吸附
            dl->AddLine(ImVec2(X(0.28f), Y(0.72f)), ImVec2(X(0.72f), Y(0.28f)), c, th);
            dl->AddCircleFilled(ImVec2(X(0.24f), Y(0.76f)), s * 0.12f, c, 10);
            dl->AddCircle(ImVec2(X(0.76f), Y(0.24f)), s * 0.12f, c, 10, th);
            break;
        }
        case kVpIconGrid:
        case kVpIconSnapGrid: {
            dl->AddRect(ImVec2(X(0.16f), Y(0.16f)), ImVec2(X(0.84f), Y(0.84f)), c,
                        0.0f, 0, th);
            dl->AddLine(ImVec2(X(0.39f), Y(0.16f)), ImVec2(X(0.39f), Y(0.84f)), c, th);
            dl->AddLine(ImVec2(X(0.61f), Y(0.16f)), ImVec2(X(0.61f), Y(0.84f)), c, th);
            dl->AddLine(ImVec2(X(0.16f), Y(0.39f)), ImVec2(X(0.84f), Y(0.39f)), c, th);
            dl->AddLine(ImVec2(X(0.16f), Y(0.61f)), ImVec2(X(0.84f), Y(0.61f)), c, th);
            break;
        }
        case kVpIconEye: {
            // 眼睛：两条三次贝塞尔拼成杏仁形 + 瞳孔
            dl->AddCircleFilled(ctr, s * 0.13f, c, 10);
            dl->PathClear();
            dl->PathLineTo(ImVec2(X(0.10f), Y(0.5f)));
            dl->PathBezierCubicCurveTo(ImVec2(X(0.30f), Y(0.18f)),
                                       ImVec2(X(0.70f), Y(0.18f)),
                                       ImVec2(X(0.90f), Y(0.5f)), 16);
            dl->PathBezierCubicCurveTo(ImVec2(X(0.70f), Y(0.82f)),
                                       ImVec2(X(0.30f), Y(0.82f)),
                                       ImVec2(X(0.10f), Y(0.5f)), 16);
            dl->PathStroke(c, 0, th);
            break;
        }
        case kVpIconGear: {
            dl->AddCircle(ctr, s * 0.20f, c, 14, th);
            for (int i = 0; i < 8; ++i) {
                const float a = 6.2832f * static_cast<float>(i) / 8.0f;
                dl->AddLine(ImVec2(ctr.x + std::cos(a) * s * 0.24f,
                                   ctr.y + std::sin(a) * s * 0.24f),
                            ImVec2(ctr.x + std::cos(a) * s * 0.38f,
                                   ctr.y + std::sin(a) * s * 0.38f),
                            c, th);
            }
            break;
        }
        case kVpIconPlus: {
            // 加号：两条等长线 + 圆角端点（直角的十字在小尺寸下像网格线）
            dl->AddLine(ImVec2(X(0.24f), Y(0.5f)), ImVec2(X(0.76f), Y(0.5f)),
                        c, th * 1.15f);
            dl->AddLine(ImVec2(X(0.5f), Y(0.24f)), ImVec2(X(0.5f), Y(0.76f)),
                        c, th * 1.15f);
            break;
        }
        case kVpIconCaret: {
            dl->AddTriangleFilled(ImVec2(X(0.26f), Y(0.40f)),
                                  ImVec2(X(0.74f), Y(0.40f)),
                                  ImVec2(X(0.50f), Y(0.66f)), c);
            break;
        }
        case kVpIconAxis:
        default: {
            // 三轴小图标（X 红 / Y 绿 / Z 蓝由调用方决定单色版本）
            dl->AddLine(ImVec2(X(0.22f), Y(0.78f)), ImVec2(X(0.78f), Y(0.78f)), c, th);
            dl->AddLine(ImVec2(X(0.22f), Y(0.78f)), ImVec2(X(0.22f), Y(0.22f)), c, th);
            dl->AddLine(ImVec2(X(0.22f), Y(0.78f)), ImVec2(X(0.60f), Y(0.40f)), c, th);
            break;
        }
    }
}

// 工具条上的一格方形图标按钮。active = 当前选中（工具 / 视图模式）。
// 背景用 token 的强调色，浅色主题下不会变成一块深色孤岛。
bool vpIconButton(const char* id, int icon, bool active, const char* tip,
                  float sz = 24.0f, const char* rectTag = nullptr) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();

    ImGui::PushID(id);
    const bool clicked = ImGui::InvisibleButton("##hit", ImVec2(sz, sz));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    ImGui::PopID();

    const ImVec2 q(p.x + sz, p.y + sz);
    if (active) {
        dl->AddRectFilled(p, q, ImGui::GetColorU32(ui::tokens().accentSoft), 3.0f);
        dl->AddRect(p, q, ImGui::GetColorU32(ui::tokens().accent), 3.0f, 0, 1.2f);
    } else if (held) {
        dl->AddRectFilled(p, q, ImGui::GetColorU32(ui::tokens().accentSoft), 3.0f);
    } else if (hovered) {
        dl->AddRectFilled(p, q, IM_COL32(255, 255, 255, 26), 3.0f);
    }

    ImVec4 tc = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    if (!active) tc.w *= 0.84f;   // 未选中降一档，但不用 TextDisabled（太暗）
    paintVpIcon(dl, ImVec2(p.x + (sz - sz * 0.66f) * 0.5f,
                           p.y + (sz - sz * 0.66f) * 0.5f),
                sz * 0.66f, icon, ImGui::GetColorU32(tc));

    noteButtonRect(p, q);
    if (rectTag) logRect(rectTag, p, q);
    if (hovered && tip) ImGui::SetTooltip("%s", tip);
    return clicked;
}

// "图标 + 数值 + ▾" 的下拉按钮（吸附四件套 / 相机速度都是这个样子）。
// 返回 true 时调用方负责 OpenPopup。

// 先算宽度再画：右侧那一组要"先知道总宽才能右对齐"，所以宽度的算法
// 必须和绘制共用同一份（各写一遍迟早会漂）。
float vpValueButtonWidth(const char* value) {
    return 4.0f + 16.0f + 4.0f + ImGui::CalcTextSize(value).x + 4.0f + 10.0f + 4.0f;
}
float vpTextButtonWidth(const char* label) {
    return 8.0f + ImGui::CalcTextSize(label).x + 4.0f + 11.0f + 6.0f;
}

bool vpValueButton(const char* id, int icon, const char* value, const char* tip,
                   const char* rectTag) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = 24.0f;
    const ImVec2 txt = ImGui::CalcTextSize(value);
    const float w = vpValueButtonWidth(value);

    ImGui::PushID(id);
    const bool clicked = ImGui::InvisibleButton("##hit", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    ImGui::PopID();

    const ImVec2 q(p.x + w, p.y + h);
    if (held) {
        dl->AddRectFilled(p, q, ImGui::GetColorU32(ui::tokens().accentSoft), 3.0f);
    } else if (hovered) {
        dl->AddRectFilled(p, q, IM_COL32(255, 255, 255, 26), 3.0f);
    }

    ImVec4 tc = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    if (!hovered) tc.w *= 0.88f;
    const ImU32 col = ImGui::GetColorU32(tc);
    paintVpIcon(dl, ImVec2(p.x + 4.0f, p.y + (h - 16.0f) * 0.5f), 16.0f, icon, col);
    dl->AddText(ImVec2(p.x + 24.0f, p.y + (h - txt.y) * 0.5f), col, value);
    paintVpIcon(dl, ImVec2(p.x + w - 15.0f, p.y + (h - 12.0f) * 0.5f), 12.0f,
                kVpIconCaret,
                ImGui::GetColorU32(ImGuiCol_TextDisabled));

    noteButtonRect(p, q);
    if (rectTag) logRect(rectTag, p, q);
    if (hovered && tip) ImGui::SetTooltip("%s", tip);
    return clicked;
}

// "文字 + ▾" 的下拉按钮（透视 / 光照模式）。
bool vpTextButton(const char* id, const char* label, const char* tip,
                  const char* rectTag, float h = 24.0f) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 txt = ImGui::CalcTextSize(label);
    const float w = vpTextButtonWidth(label);

    ImGui::PushID(id);
    const bool clicked = ImGui::InvisibleButton("##hit", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    ImGui::PopID();

    const ImVec2 q(p.x + w, p.y + h);
    if (held) {
        dl->AddRectFilled(p, q, ImGui::GetColorU32(ui::tokens().accentSoft), 3.0f);
    } else if (hovered) {
        dl->AddRectFilled(p, q, IM_COL32(255, 255, 255, 26), 3.0f);
    }

    ImVec4 tc = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    if (!hovered) tc.w *= 0.88f;
    const ImU32 col = ImGui::GetColorU32(tc);
    dl->AddText(ImVec2(p.x + 8.0f, p.y + (h - txt.y) * 0.5f), col, label);
    paintVpIcon(dl, ImVec2(p.x + w - 17.0f, p.y + (h - 12.0f) * 0.5f), 12.0f,
                kVpIconCaret, ImGui::GetColorU32(ImGuiCol_TextDisabled));

    noteButtonRect(p, q);
    if (rectTag) logRect(rectTag, p, q);
    if (hovered && tip) ImGui::SetTooltip("%s", tip);
    return clicked;
}

// 分屏布局在按钮上显示的短标签 —— 就是用户给的那四个字样：1 / 2V / 2H / 4。
//
// 为什么按钮上不用 viewportLayoutName()（single / two_vertical ...）：那是
// **落盘用的 id**，二十多个字符根本塞不进工具条；而且用户认的就是这四个。
// 完整说明落在下拉里（每行都有），按钮上只需要"我现在是几屏"。
const char* viewportLayoutShortName(ViewportLayout l) {
    switch (l) {
        case ViewportLayout::Single: return "1";
        case ViewportLayout::TwoVertical: return "2V";
        case ViewportLayout::TwoHorizontal: return "2H";
        case ViewportLayout::Quad: return "4";
    }
    return "1";
}

// 工具条的分组竖线（比 TextUnformatted("|") 更像 UE 的分组分隔）
void vpBarSeparator() {    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = 24.0f;
    ImGui::Dummy(ImVec2(9.0f, h));
    const float x = p.x + 4.5f;
    const ImU32 col = ImGui::GetColorU32(ImGuiCol_Separator);
    dl->AddLine(ImVec2(x, p.y + 3.0f), ImVec2(x, p.y + h - 3.0f), col, 1.0f);
}

} // namespace

ViewportPanel::ViewportPanel(EditorContext& ctx, render::Renderer& renderer)
    : m_ctx(ctx), m_renderer(renderer) {}

// 打开下拉并把"弹层该出现在哪"记下来。
//
// 必须在按钮**刚提交完**的那一行调用（读的是 GetItemRect*）。ImGui 自己的
// OpenPopup 会把弹层摆在鼠标位置 —— 落在工具条那一行上，既挡按钮又不
// 像 UE；这里统一改成"从按钮左下角往下展开"。
void ViewportPanel::openDropdown(const char* popupId) {
    m_popupX = g_lastBtnMin.x;
    m_popupY = g_lastBtnMax.y + 1.0f;
    ImGui::OpenPopup(popupId);
}

// 每个 drawXxxMenu 的第一行（BeginPopup 之前）调用。
// ImGuiCond_Appearing：只在弹层出现的那一帧定位，之后用户/滚动条都能接管。
void ViewportPanel::placeDropdown() {
    if (m_popupY <= 0.0f) return;
    ImGui::SetNextWindowPos(ImVec2(m_popupX, m_popupY), ImGuiCond_Appearing);
}

// ---------------------------------------------------------------- 视口工具条
//
// 2026-10-17 晚重做。用户给了 6 张 UE5 视口工具条的截图，要求"按这些图片
// 的 ui 结构和功能重新设计"。落地成**一条紧凑的图标行 + 九个下拉**：
//
//   ┌─────────────────────────────────────────────────────────────────────┐
//   │ [▼工具] │ ↖ ✥ ⟳ ⤢ │ 🌐▾ ↺ │ ✛0▾ ▦5▾ ⟳10°▾ ⤢0.25▾ │  透视▾ 0.51▾ 光照▾ 👁 ⚙ ▦ │
//   └─────────────────────────────────────────────────────────────────────┘
//     ①工具下拉  ②四个工具  ③坐标/复位  ④吸附四件套           ⑤视口看法的右侧组
//
// 为什么把原来的两行（工具 + 吸附）并成一行：原来第二行是"三个勾选框 +
// 三个数字框 + Presets"，一共要 900+ px，窄视口里必被裁掉。改成"图标 +
// 数值 + 下拉"之后每组只占 ~70px，四个吸附加左组全部塞进 520px ——
// 这正是 UE5 的做法（值不是常驻的滑条，而是点开才有的列表）。
//
// 三处刻意的取舍（都是"不许骗人"这条纪律的延伸）：
//   · **正交投影不给**：scene::Camera 只有透视投影，摆一个点了没反应的
//     "正交"单选比没有更糟。菜单里它以禁用态出现，提示写明"引擎暂不支持"。
//   · **视图方向写的是本引擎真实的快捷键**（Num1/3/7 + Ctrl 反向），
//     不是截图里的 Alt+G/J/K/H —— 照抄别人的按钮却挂着另一套键位，
//     是"文档骗人"。
//   · **吸附的四个值各自绑定真实存储**：move/rotate/scale 写 EditorContext，
//     grid 写 render 的 grid.minorStep（粗格自动 ×10），不是四个摆设。
void ViewportPanel::drawToolbar() {
    EditorContext& c = m_ctx;
    const GizmoMode mode = c.gizmoMode();
    const bool spaceLocal = (c.gizmoSpace() == GizmoSpace::Local);

    // 切换工具时打一行日志：自动化据此断言"点这个按钮真的换了模式"。
    // ⚠ 这里**不**打日志（只落状态）。日志挪到 drawToolbar 末尾按帧差分，
    //   因为换模式的入口有三处：这里的四个工具按钮、快捷键 Q/W/E/R、
    //   以及顶栏那颗"选择模式"下拉。只在按钮里打日志的话，后两条路径
    //   一行痕迹都没有 —— 自动化没法证明它们改的是同一份状态。
    const auto setMode = [&](GizmoMode m) {
        if (c.gizmoMode() == m) return;
        c.setGizmoMode(m);
    };
    const auto same = [&]() { ImGui::SameLine(0.0f, 2.0f); };
    // 左组（工具 / 吸附 / Add）画完时，光标停在哪（窗口局部 x）。右组靠它
    // 算"还剩多少地方"—— 见 ④b 里的说明。
    float leftGroupEndX = 0.0f;

    // ---------------- ① 工具下拉（当前工具图标 + ▾）----------------
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = 24.0f, w = 42.0f;
        const ImVec2 q(p.x + w, p.y + h);
        // 先记账再点：openDropdown 靠 g_lastBtnMin/Max 定位，不能读到上一颗按钮
        noteButtonRect(p, q);
        if (ImGui::InvisibleButton("##vp_tool_dd", ImVec2(w, h)))
            openDropdown("##vp_tool_menu");
        const bool hovered = ImGui::IsItemHovered();
        if (hovered) {
            dl->AddRectFilled(p, q, IM_COL32(255, 255, 255, 30), 3.0f);
        }
        const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
        const int curIcon = mode == GizmoMode::Select      ? kVpIconSelect
                            : mode == GizmoMode::Translate ? kVpIconMove
                            : mode == GizmoMode::Rotate    ? kVpIconRotate
                                                           : kVpIconScale;
        paintVpIcon(dl, ImVec2(p.x + 5.0f, p.y + 4.0f), 16.0f, curIcon, col);
        paintVpIcon(dl, ImVec2(p.x + 26.0f, p.y + 7.0f), 11.0f, kVpIconCaret,
                    ImGui::GetColorU32(ImGuiCol_TextDisabled));
        logRect("VP-TOOLBTN", p, q);
        if (hovered) {
            ImGui::SetTooltip(TR("Transform tool  (Q / W / E / R)\n"
                              "Opens the tool list with a search box."));
        }
    }

    // ---------------- ② 四个工具（单选）----------------
    same();
    if (vpIconButton("vpt_sel", kVpIconSelect, mode == GizmoMode::Select,
                     TR("Select  (Q)\nPick objects. No transform handle is shown."),
                     24.0f, "VP-TOOL select"))
        setMode(GizmoMode::Select);
    same();
    if (vpIconButton("vpt_move", kVpIconMove, mode == GizmoMode::Translate,
                     TR("Move  (W)"), 24.0f, "VP-TOOL move"))
        setMode(GizmoMode::Translate);
    same();
    if (vpIconButton("vpt_rot", kVpIconRotate, mode == GizmoMode::Rotate,
                     TR("Rotate  (E)"), 24.0f, "VP-TOOL rotate"))
        setMode(GizmoMode::Rotate);
    same();
    if (vpIconButton("vpt_scale", kVpIconScale, mode == GizmoMode::Scale,
                     TR("Scale  (R)"), 24.0f, "VP-TOOL scale"))
        setMode(GizmoMode::Scale);

    // ---------------- ③ 坐标系 + 复位 ----------------
    same();
    vpBarSeparator();
    same();
    if (vpIconButton("vpt_space", kVpIconGlobe, spaceLocal,
                     TR("Transform space  (X toggles)\nWorld = gizmo axes follow the world grid\n"
                        "Local = they follow the object's rotation"),
                     24.0f, "VP-SPACE"))
        openDropdown("##vp_space_menu");
    same();
    {
        ImGui::BeginDisabled(!c.hasSelection());
        if (vpIconButton("vpt_reset", kVpIconReset, false,
                         TR("Reset the selected object's rotation and scale\n"
                            "(position is kept). Undoable with Ctrl+Z."),
                         24.0f, "VP-XFORM-RESET")) {
            const ecs::Entity e = c.selection();
            bool did = false;
            c.structuralEdit("Reset Transform", [&]() {
                if (auto* t = c.editorScene().world().get<ecs::TransformComponent>(e)) {
                    t->rotation = glm::vec3(0.0f);
                    t->scale = glm::vec3(1.0f);
                    did = true;
                }
            });
            if (did) {
                c.dirty() = true;
                c.setStatus(TR("Reset rotation + scale"));
                VK_LOG_INFO("VP-XFORM-RESET entity=%u", e.id);
            }
        }
        ImGui::EndDisabled();
    }

    // ---------------- ④ 吸附四件套 ----------------
    same();
    vpBarSeparator();
    same();
    {
        char buf[32];
        // 平移吸附：0 = 关（UE 的 0 也是这个意思）
        const bool on = c.snapMove();
        std::snprintf(buf, sizeof(buf), on ? TR("%g m") : "0", static_cast<double>(
                          on ? c.snapMoveStep() : 0.0f));
        if (vpValueButton("vpt_snap_move", kVpIconSnapMove, buf,
                          TR("Move snapping step\nClick for the preset list."),
                          "VP-SNAP move"))
            openDropdown("##vp_snap_move");
    }
    same();
    {
        char buf[32];
        float g = 1.0f;
        if (void* p = render::paramPtr("grid.minorStep")) {
            g = *static_cast<float*>(p);
        }
        std::snprintf(buf, sizeof(buf), TR("%g m"), static_cast<double>(g));
        if (vpValueButton("vpt_snap_grid", kVpIconSnapGrid, buf,
                          TR("Reference grid cell size"), "VP-SNAP grid"))
            openDropdown("##vp_snap_grid");
    }
    same();
    {
        char buf[32];
        const bool on = c.snapRotate();
        std::snprintf(buf, sizeof(buf), on ? "%.0f°" : "0°",
                      static_cast<double>(on ? c.snapRotateStep() : 0.0f));
        if (vpValueButton("vpt_snap_rot", kVpIconSnapRot, buf,
                          TR("Rotation snapping step"), "VP-SNAP rotate"))
            openDropdown("##vp_snap_rot");
    }
    same();
    {
        char buf[32];
        const bool on = c.snapScale();
        std::snprintf(buf, sizeof(buf), on ? "%g" : "0",
                      static_cast<double>(on ? c.snapScaleStep() : 0.0f));
        if (vpValueButton("vpt_snap_scale", kVpIconSnapScale, buf,
                          TR("Scale snapping step"), "VP-SNAP scale"))
            openDropdown("##vp_snap_scale");
    }

    // ---------------- ④b "+ Add"（左组末尾）----------------
    //
    // 2026-10-21 用户要求把它从顶栏挪到"视口上侧工具条左侧的末尾"：
    //   「还有旁边的那个 Add 的列表可以把它的位置调整到视口上侧工具条
    //     左侧的末尾。」
    //
    // 放在左组最后、右对齐组之前 —— 再往右是"透视 / 布局 / 照明"那批
    // **只读视图设置**，语义上分得开。
    //
    // ⚠ 这里**不复制一份菜单**：视口里已经有一套（Shift+A 那个
    //   `drawAddMenu(vpPos, vpSize, hovered)`，按鼠标射线落地、带 #序号
    //   锚点）。按钮只是**再开一次同一个弹层**（`##vp_add`），落点由
    //   drawToolbar 侧算好塞进 m_addSpot。两套菜单必然漂移，一套最省事。
    same();
    vpBarSeparator();
    same();
    {
        // 图标 + "Add" 手绘：ImGui::Button 的标签居中，而这里要和左边
        // 那排图标按钮的视觉节奏一致（左图标右文字）。
        const float h = 24.0f;
        const char* txt = TR("Add");
        const float w = ImGui::CalcTextSize(txt).x + 26.0f;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec2 q(p.x + w, p.y + h);
        const bool hit = ImGui::InvisibleButton("##vp_add_btn", ImVec2(w, h));
        const bool hov = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (hov) dl->AddRectFilled(p, q, IM_COL32(255, 255, 255, 30), 3.0f);
        const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
        paintVpIcon(dl, ImVec2(p.x + 6.0f, p.y + 5.0f), 14.0f, kVpIconPlus, col);
        dl->AddText(ImVec2(p.x + 24.0f,
                           (p.y + q.y) * 0.5f - ImGui::GetTextLineHeight() * 0.5f),
                    col, txt);
        logRect("TB-ADD", p, q);   // 锚点名字沿用历史名（脚本按它点）
        if (hov) {
            // ⚠ 与词典里的那条**逐字符**一致（换行在 "the\n" 之后）：
            //   改写成一个字面量会让 key 对不上，i18n 检查立刻红。
            ImGui::SetTooltip(TR("Add an object to the scene (also: Shift+A in the\n"
                              "viewport, which drops it where the mouse points)."));
        }
        // ⚠ 左组末端 = **这颗按钮的右边缘**（换算成窗口局部坐标），不是
        //   `GetCursorPosX()`：ImGui 每画完一个 item 就把光标 x 收回到行首
        //   （≈ 内边距），读到的永远是十几像素 —— 第一版就是这么写的，于是
        //   "还剩多少地方"算成了整个窗口宽，右组照画在固定位置、和左边这排
        //   重叠（截图里 "+ Add" 压在 "Perspective" 上就是这么来的）。
        leftGroupEndX = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
        if (hit) {
            // 落点：鼠标此刻在工具条上，没有视口射线可用 —— 用"相机朝向前方
            // 5 m 的地面"做基准，再按第几次打开做环形微偏移（连点两次不会
            // 叠在一起，看着像"没加上"）。
            scene::Camera& cam = m_ctx.activeScene().camera();
            glm::vec3 fwd = cam.target() - cam.position();
            if (glm::length(fwd) < 1.0e-4f) fwd = glm::vec3(0.0f, -1.0f, 0.0f);
            m_addSpot = cam.position() + glm::normalize(fwd) * 5.0f;
            m_addSpot.z = 0.0f;
            if (m_addMenuSeq > 0) {
                const float a = 2.39996f * static_cast<float>(m_addMenuSeq);
                m_addSpot.x += std::cos(a) * 2.5f;
                m_addSpot.y += std::sin(a) * 2.5f;
            }
            ++m_addMenuSeq;
            VK_LOG_INFO("VP-ADD-OPEN seq=%d at=(%.1f,%.1f) via=toolbar",
                        m_addMenuSeq, static_cast<double>(m_addSpot.x),
                        static_cast<double>(m_addSpot.y));
            ImGui::OpenPopup("##vp_add");
        }
    }

    // ---------------- ⑤ 右侧组：视口怎么看 ----------------
    //
    // **按预算贪心保留**：从左组末端到窗口右边缘就那么多地方，七颗按钮
    // 全摆上去在这个视口宽度下根本放不下（实测需要 919px，可用 789px）。
    // 原来的写法是"算出来放不下就整组不画"，但那个判断读的是
    // `GetCursorPosX()`，而 ④b 的 Add 弹层会把光标 x 打回 0 —— 判断于是
    // 永远成立，右组照画在固定位置，和左边那排**直接重叠**（截图里
    // "+ Add" 压在 "Perspective" 上就是这么来的）。
    //
    // 现在改成：按重要性从高到低往预算里塞，塞不下的那颗就不画，并在
    // 日志里列出被丢的是谁。宁可少两颗，也不重叠/不被裁一半。
    {
        char speedBuf[32];
        std::snprintf(speedBuf, sizeof(speedBuf), "%.2f",
                      static_cast<double>(c.navSensitivity()));
        const char* lightName = TR(render::Renderer::viewModeName(m_renderer.viewMode()));
        const char* persp = TR("Perspective");
        const char* layoutName = viewportLayoutShortName(c.viewportLayout());

        // 顺序 = 重要性：投影 > 布局 > 照明 > 显示 > 设置 > 网格 > 相机速度
        // （速度还能右键 + 滚轮调，所以排最后）。
        struct VpBarItem { int id; float w; };
        const VpBarItem items[] = {
            {0, vpTextButtonWidth(persp)},      // 投影
            {1, vpTextButtonWidth(layoutName)}, // 布局
            {2, vpTextButtonWidth(lightName)},  // 照明
            {3, 24.0f},                         // 显示开关
            {4, 24.0f},                         // 视口设置
            {5, 24.0f},                         // 网格
            {6, vpValueButtonWidth(speedBuf)},  // 相机速度
        };
        constexpr int kItemCount = 7;

        const float budget =
            ImGui::GetContentRegionMax().x - leftGroupEndX - 8.0f;
        bool take[kItemCount] = {false};
        float need = 0.0f;
        int taken = 0;
        for (int i = 0; i < kItemCount; ++i) {
            const float w = items[i].w + (taken > 0 ? 2.0f : 0.0f);
            if (need + w > budget) break;   // 后面的更不重要，直接停
            take[items[i].id] = true;
            need += w;
            ++taken;
        }

        if (taken > 0) {
            const float targetX = ImGui::GetContentRegionMax().x - need;
            ImGui::SameLine(targetX);
            logRect("VP-BAR-RIGHT", ImGui::GetCursorScreenPos(),
                    ImVec2(ImGui::GetCursorScreenPos().x + need,
                           ImGui::GetCursorScreenPos().y + 24.0f));
            // 自动化：这一帧实际画出了哪几颗（taken）与被丢掉的是谁（budget 不够）
            if (std::getenv("MYVK_LOG_RECTS")) {
                static int lastTaken = -1;
                if (taken != lastTaken) {
                    lastTaken = taken;
                    // 只报数字：taken<7 就说明有条目因为放不下被丢了
                    // （丢的永远是数组尾部那几颗，顺序在 items[] 里写死）
                    VK_LOG_INFO("VP-BAR-FIT taken=%d/%d need=%.0f budget=%.0f",
                                taken, kItemCount,
                                static_cast<double>(need),
                                static_cast<double>(budget));
                }
            }

            if (take[0]) {
                if (vpTextButton("vpt_view", persp,
                                 TR("Projection, view directions and camera"),
                                 "VP-VIEW-BTN"))
                    openDropdown("##vp_view_menu");
                same();
            }
            // 分屏布局（1 / 2V / 2H / 4）。**紧挨着"透视"**是用户明确要求的：
            // "把这些参数也一样放到和 Perspective 这些属性一起并排放置，
            //  点击后也是展开下拉勾选框列表去设置"。
            if (take[1]) {
                if (vpTextButton("vpt_layout", layoutName,
                                 TR("Viewport split layout\n"
                                    "1 = single   2V = side by side\n"
                                    "2H = stacked   4 = quad"),
                                 "VP-LAYOUT-BTN"))
                    openDropdown("##vp_layout_menu");
                same();
            }
            if (take[2]) {
                if (vpTextButton("vpt_light", lightName,
                                 TR("Lighting mode (how the scene is drawn)"),
                                 "VP-LIGHT"))
                    openDropdown("##vp_light_menu");
                same();
            }
            if (take[3]) {
                if (vpIconButton("vpt_show", kVpIconEye, false,
                                 TR("Show flags"), 24.0f, "VP-SHOW"))
                    openDropdown("##vp_show_menu");
                same();
            }
            if (take[4]) {
                if (vpIconButton("vpt_settings", kVpIconGear, false,
                                 TR("Viewport settings (grid / background)"),
                                 24.0f, "VP-SET"))
                    openDropdown("##vp_settings_menu");
                same();
            }
            if (take[5]) {
                if (vpIconButton("vpt_grid", kVpIconGrid, c.showGridRef(),
                                 TR("Reference grid on / off"), 24.0f, "VP-GRID"))
                    c.showGridRef() = !c.showGridRef();
                same();
            }
            if (take[6]) {
                if (vpValueButton("vpt_speed", kVpIconMove, speedBuf,
                                  TR("Camera speed\nHold RMB + wheel to change it "
                                     "without opening this menu."),
                                  "VP-SPEED"))
                    openDropdown("##vp_speed_menu");
            }
        }
    }

    // ---------------- 工具模式变化留痕（单一出口）----------------
    // 放在这一帧所有入口**之后**：不论模式是点工具按钮、按 Q/W/E/R，还是从
    // 顶栏那颗"选择模式"下拉改的，只要和上一帧不同就打一行。这样
    //   · 自动化能证明"三条入口改的是同一份状态"（同一行日志）
    //   · 不会因为同一帧多次赋值而刷出多行
    if (static_cast<int>(mode) != m_lastToolMode) {
        m_lastToolMode = static_cast<int>(mode);
        VK_LOG_INFO("VP-TOOLMODE %s",
                    mode == GizmoMode::Select      ? "select"
                    : mode == GizmoMode::Translate ? "move"
                    : mode == GizmoMode::Rotate    ? "rotate"
                                                   : "scale");
    }

    // ---------------- 下拉内容（每帧无条件提交）----------------
    // ⚠ 这些 BeginPopup 必须**每帧都执行**：ImGui 在没提交的那一帧会把
    //   弹层直接关掉（和 ContentBrowser 右键菜单同一个坑）。所以它们
    //   不在任何 if 里。
    drawToolMenu();
    drawSpaceMenu();
    drawSnapMenu(0);
    drawSnapMenu(1);
    drawSnapMenu(2);
    drawSnapMenu(3);
    drawViewMenu();
    drawLayoutMenu();
    drawSpeedMenu();
    drawLightMenu();
    drawShowMenu();
    drawVpSettingsMenu();
}

// ---------------------------------------------------------------- 下拉内容
//
// 每个函数只画 popup 内部，由 drawToolbar 每帧无条件调用。

// 搜索框的过滤规则（大小写不敏感的子串）。单独提出来是因为**两处**要用：
// vpToolRow 用它决定这一行画不画，drawToolMenu 用它算"剩几行"打日志。
// 两处各写一遍，迟早在大小写/空串上分叉。
static bool vpToolMatch(const char* label, const char* filter) {
    if (!filter || !filter[0]) return true;
    std::string hay(label), needle(filter);
    auto lower = [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); };
    std::transform(hay.begin(), hay.end(), hay.begin(), lower);
    std::transform(needle.begin(), needle.end(), needle.begin(), lower);
    return hay.find(needle) != std::string::npos;
}

// 一行工具项：左侧圆点（选中实心）+ 图标 + 名字 + 右对齐的快捷键。
// 这是截图里"变换工具"那一段的排版 —— 圆点而不是对勾，因为它是**单选**。
static bool vpToolRow(const char* id, int icon, const char* label, const char* key,
                      bool active, const char* filter, const char* rectTag) {
    if (!vpToolMatch(label, filter)) return false;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h = 26.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;

    ImGui::PushID(id);
    const bool clicked = ImGui::Selectable("##row", false,
                                           ImGuiSelectableFlags_None,
                                           ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();

    const float cy = p.y + h * 0.5f;
    // 单选圆点
    if (active) {
        dl->AddCircleFilled(ImVec2(p.x + 10.0f, cy), 4.2f,
                            ImGui::GetColorU32(ui::tokens().accent), 12);
    } else {
        dl->AddCircle(ImVec2(p.x + 10.0f, cy), 4.2f,
                      ImGui::GetColorU32(ImGuiCol_TextDisabled), 12, 1.2f);
    }
    // 图标 + 名字
    ImVec4 tc = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    if (!active && !hovered) tc.w *= 0.86f;
    const ImU32 col = ImGui::GetColorU32(tc);
    paintVpIcon(dl, ImVec2(p.x + 22.0f, cy - 8.0f), 16.0f, icon, col);
    dl->AddText(ImVec2(p.x + 46.0f, cy - ImGui::GetTextLineHeight() * 0.5f), col,
                label);
    // 快捷键（右对齐）
    const ImVec2 kw = ImGui::CalcTextSize(key);
    dl->AddText(ImVec2(p.x + w - kw.x - 10.0f, cy - kw.y * 0.5f),
                ImGui::GetColorU32(ImGuiCol_TextDisabled), key);

    if (rectTag) logRect(rectTag, p, ImVec2(p.x + w, p.y + h));
    return clicked;
}

void ViewportPanel::drawToolMenu() {
    placeDropdown();
    if (!ImGui::BeginPopup("##vp_tool_menu")) return;

    logRect("VP-TOOLMENU-WIN", ImGui::GetWindowPos(),
            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                   ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    // 搜索框宽度**固定**，不要用 GetWindowWidth() 推。
    //
    // 踩过的坑：`SetNextItemWidth(max(230, GetWindowWidth()-16))` 在弹层
    // 第一帧拿到的是父窗口宽度（≈1570），于是搜索框被撑到 1554px、把
    // 整个弹层拉成一条横贯屏幕的带子。弹层是 auto-resize 的 —— 内容的
    // 宽度**决定**窗口宽度，从窗口宽度反推内容宽度是个死循环。
    ImGui::SetNextItemWidth(kToolMenuW - 16.0f);
    ImGui::InputTextWithHint("##vp_tool_search", TR("Start typing to search"),
                             m_toolSearch, sizeof(m_toolSearch));
    logRect("VP-TOOLSEARCH", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    ImGui::SeparatorText(TR("Transform tools"));

    struct Row { GizmoMode m; int icon; const char* label; const char* key;
                 const char* id; };
    const Row rows[4] = {
        {GizmoMode::Select, kVpIconSelect, "Select", "Q", "select"},
        {GizmoMode::Translate, kVpIconMove, "Move", "W", "move"},
        {GizmoMode::Rotate, kVpIconRotate, "Rotate", "E", "rotate"},
        {GizmoMode::Scale, kVpIconScale, "Scale", "R", "scale"},
    };
    const GizmoMode cur = m_ctx.gizmoMode();
    int shown = 0;
    for (const Row& r : rows) {
        char tag[48];
        std::snprintf(tag, sizeof(tag), "VP-TOOLITEM %s", r.label);
        if (vpToolRow(r.label, r.icon, TR(r.label), r.key, cur == r.m,
                      m_toolSearch, tag)) {
            m_ctx.setGizmoMode(r.m);
            // 日志用**小写 id**，和工具条上那四个按钮（setMode）保持同一套
            // 词表 —— 同一个动作打两种写法，自动化脚本就得猜是哪一种。
            VK_LOG_INFO("VP-TOOLMODE %s", r.id);
            ImGui::CloseCurrentPopup();
        }
        if (vpToolMatch(r.label, m_toolSearch)) ++shown;
    }
    // 搜索词变化时打一行"过滤结果剩几行"。搜索是"按行不再提交"来过滤的，
    // 没有可断言的痕迹就没法证明它真的在过滤（而非键没进去）。
    if (std::strcmp(m_toolSearch, m_toolSearchLogged) != 0) {
        std::snprintf(m_toolSearchLogged, sizeof(m_toolSearchLogged), "%s",
                      m_toolSearch);
        VK_LOG_INFO("VP-TOOLSEARCH q=%s rows=%d", m_toolSearch, shown);
    }

    ImGui::Separator();
    // 焦点 / 全屏这种"动作"也放这里 —— 截图里的下拉在工具列表下面还有
    // 若干动作项。只列真的接了线的两个。
    ImGui::BeginDisabled(!m_ctx.hasSelection());
    if (ImGui::MenuItem(TR("Focus selection"), "F")) focusSelection();
    ImGui::EndDisabled();

    ImGui::EndPopup();
}

void ViewportPanel::drawSpaceMenu() {
    placeDropdown();
    if (!ImGui::BeginPopup("##vp_space_menu")) return;
    const GizmoSpace sp = m_ctx.gizmoSpace();
    bool world = (sp == GizmoSpace::World);
    if (ImGui::RadioButton(TR("World"), world)) {
        m_ctx.setGizmoSpace(GizmoSpace::World);
        VK_LOG_INFO("VP-SPACE world");
        // 单选 = 选完就结束。不关的话弹层会一直挂着，下一次点击会被它吃掉
        // （表现为"点了别的按钮没反应"）。
        ImGui::CloseCurrentPopup();
    }
    logRect("VP-SPACEITEM world", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::RadioButton(TR("Local"), !world)) {
        m_ctx.setGizmoSpace(GizmoSpace::Local);
        VK_LOG_INFO("VP-SPACE local");
        ImGui::CloseCurrentPopup();
    }
    logRect("VP-SPACEITEM local", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::Separator();
    ImGui::TextDisabled(TR("X toggles"));
    ImGui::EndPopup();
}

// 一个"对齐数值"下拉：标题 + 单选列表（截图里那一列圆点 + 数字）。
void ViewportPanel::drawSnapMenu(int which) {
    const char* popup = which == 0   ? "##vp_snap_move"
                        : which == 1 ? "##vp_snap_grid"
                        : which == 2 ? "##vp_snap_rot"
                                     : "##vp_snap_scale";
    placeDropdown();
    if (!ImGui::BeginPopup(popup)) return;

    EditorContext& c = m_ctx;
    const char* id = which == 0   ? "move"
                     : which == 1 ? "grid"
                     : which == 2 ? "rotate"
                                  : "scale";
    const float* values = which == 0   ? kSnapMovePresets
                          : which == 1 ? kSnapGridPresets
                          : which == 2 ? kSnapRotPresets
                                       : kSnapScalePresets;
    const int count = which == 0   ? static_cast<int>(sizeof(kSnapMovePresets) / sizeof(float))
                      : which == 1 ? static_cast<int>(sizeof(kSnapGridPresets) / sizeof(float))
                      : which == 2 ? static_cast<int>(sizeof(kSnapRotPresets) / sizeof(float))
                                   : static_cast<int>(sizeof(kSnapScalePresets) / sizeof(float));

    ImGui::SeparatorText(TR("Align size"));
    logRect("VP-SNAPMENU-WIN", ImGui::GetWindowPos(),
            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                   ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    // 当前值（grid 读渲染参数，其余读 EditorContext）
    float cur = 0.0f;
    bool on = true;
    if (which == 0) {
        on = c.snapMove();
        cur = c.snapMoveStep();
    } else if (which == 1) {
        if (void* p = render::paramPtr("grid.minorStep")) {
            cur = *static_cast<float*>(p);
        }
    } else if (which == 2) {
        on = c.snapRotate();
        cur = c.snapRotateStep();
    } else {
        on = c.snapScale();
        cur = c.snapScaleStep();
    }

    // 第 0 项：Off（只有三个变换吸附有"关"，栅格尺寸没有"关"这种说法）
    if (which != 1) {
        if (ImGui::RadioButton(TR("Off"), !on)) {
            if (which == 0) c.snapMoveRef() = false;
            else if (which == 2) c.snapRotateRef() = false;
            else c.snapScaleRef() = false;
            VK_LOG_INFO("VP-SNAP %s on=0", id);
            ImGui::CloseCurrentPopup();
        }
    }

    for (int i = 0; i < count; ++i) {
        char label[32];
        const float v = values[i];
        if (which == 1) {
            std::snprintf(label, sizeof(label), TR("%g m"), static_cast<double>(v));
        } else if (which == 2) {
            std::snprintf(label, sizeof(label), "%.0f°", static_cast<double>(v));
        } else {
            std::snprintf(label, sizeof(label), "%g", static_cast<double>(v));
        }
        const bool selected = on && std::fabs(cur - v) < 1e-4f;
        if (ImGui::RadioButton(label, selected)) {
            if (which == 0) {
                c.snapMoveStepRef() = v;
                c.snapMoveRef() = true;
            } else if (which == 1) {
                if (void* gp = render::paramPtr("grid.minorStep")) {
                    *static_cast<float*>(gp) = v;
                }
                if (void* gp = render::paramPtr("grid.majorStep")) {
                    *static_cast<float*>(gp) = v * 10.0f;
                }
                // 走统一变更钩子：标脏（下一帧落盘）+ 推一条可撤销的 ParamEdit
                editor::emitParamChange("grid.minorStep");
                editor::emitParamChange("grid.majorStep");
            } else if (which == 2) {
                c.snapRotateStepRef() = v;
                c.snapRotateRef() = true;
            } else {
                c.snapScaleStepRef() = v;
                c.snapScaleRef() = true;
            }
            VK_LOG_INFO("VP-SNAP %s=%.4f on=1", id, static_cast<double>(v));
            ImGui::CloseCurrentPopup();
        }
        char tag[48];
        std::snprintf(tag, sizeof(tag), "VP-SNAPITEM %s-%d", id, i);
        logRect(tag, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    }

    ImGui::EndPopup();
}

void ViewportPanel::drawViewMenu() {
    placeDropdown();
    if (!ImGui::BeginPopup("##vp_view_menu")) return;

    scene::Camera& cam = m_ctx.activeScene().camera();

    ImGui::SeparatorText(TR("Projection"));
    // 正交：引擎还没有（Camera 只有透视）。**以禁用态出现**并说明原因 ——
    // 藏起来会让人以为"少了东西"，做成能点却没反应则是骗人。
    ImGui::RadioButton(TR("Perspective"), true);
    logRect("VP-VIEWITEM persp", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::BeginDisabled(true);
    ImGui::RadioButton(TR("Orthographic"), false);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Not implemented: scene::Camera only does a\n"
                          "perspective projection in this engine."));
    }

    ImGui::SeparatorText(TR("View directions"));
    struct Dir { int dir; const char* label; const char* key; };
    const Dir dirs[6] = {
        {kViewPosZ, "Top", "Num7"},    {kViewNegZ, "Bottom", "Ctrl+Num7"},
        {kViewPosX, "Right", "Num3"},  {kViewNegX, "Left", "Ctrl+Num3"},
        {kViewNegY, "Front", "Num1"},  {kViewPosY, "Back", "Ctrl+Num1"},
    };
    for (const Dir& d : dirs) {
        if (ImGui::MenuItem(TR(d.label), d.key)) {
            snapViewTo(cam, d.dir);
            m_ctx.setStatus(std::string("View: ") + viewDirName(d.dir));
            VK_LOG_INFO("VP-VIEWDIR %s", d.label);
        }
        // 逐项打矩形：MenuItem 的位置随翻译长度/主题行高变化，脚本按
        // "第几行"估算点击必错（实测：V2 顶视图一直点不中）。
        char tag[48];
        std::snprintf(tag, sizeof(tag), "VP-VIEWDIRITEM %s", d.label);
        logRect(tag, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    }

    ImGui::SeparatorText(TR("Camera"));
    float fov = cam.fov();
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderFloat(TR("Field of view"), &fov, 20.0f, 120.0f, "%.0f°")) {
        cam.setFov(fov);
        VK_LOG_INFO("VP-VIEWERP fov=%.1f", static_cast<double>(cam.fov()));
    }
    float nearZ = cam.nearPlane();
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::DragFloat(TR("Near plane"), &nearZ, 0.01f, 0.001f, 10.0f, "%.3f")) {
        cam.setClipPlanes(nearZ, cam.farPlane());
        VK_LOG_INFO("VP-VIEWERP near=%.3f", static_cast<double>(cam.nearPlane()));
    }
    ImGui::TextDisabled(TR("Far plane  %.0f m"), static_cast<double>(cam.farPlane()));

    ImGui::Separator();
    ImGui::BeginDisabled(!m_ctx.hasSelection());
    if (ImGui::MenuItem(TR("Focus selection"), "F")) focusSelection();
    ImGui::EndDisabled();

    ImGui::EndPopup();
}

// 分屏布局下拉（1 / 2V / 2H / 4）。
//
// 用户原话："编辑器右上方的 1，2V，2H，4，这些代表的是……分辨率，无论是不是
// 请把这个参数也一样放到和 Perspective 这些属性一起并排放置，点击后也是
// 展开下拉勾选框列表去设置。"
//
// ⚠ 它其实**不是分辨率**，是视口分屏布局（1 屏 / 左右 2 屏 / 上下 2 屏 /
//   四宫格）。所以下拉里每一项都写明"哪种分屏"，按钮上仍用用户认得的
//   1/2V/2H/4 —— 既不误解也不改口。真正的渲染分辨率在右边的统计 HUD 里。
void ViewportPanel::drawLayoutMenu() {
    placeDropdown();
    if (!ImGui::BeginPopup("##vp_layout_menu")) return;

    ImGui::SeparatorText(TR("Viewport layout"));
    struct Item { ViewportLayout l; const char* label; const char* hint; };
    const Item items[4] = {
        {ViewportLayout::Single, "1", "Single viewport"},
        {ViewportLayout::TwoVertical, "2V", "Two side by side"},
        {ViewportLayout::TwoHorizontal, "2H", "Two stacked"},
        {ViewportLayout::Quad, "4", "Quad (four views)"},
    };
    const ViewportLayout cur = m_ctx.viewportLayout();
    for (const Item& it : items) {
        char label[96];
        std::snprintf(label, sizeof(label), "%s\t%s", it.label, TR(it.hint));
        // 勾选式（MenuItem 的 selected 就是那个对勾）—— 单选，不是多选
        if (ImGui::MenuItem(label, nullptr, cur == it.l)) {
            if (cur != it.l) {
                // 走共用实现：写 EditorContext + 设子视口数 + VIEWPORT-LAYOUT
                // 日志（与切工作区 / 读存档那条路径完全一致）。
                applyViewportLayout(m_ctx, m_renderer, it.l);
            }
        }
        char tag[48];
        std::snprintf(tag, sizeof(tag), "VP-LAYOUTITEM %s", it.label);
        logRect(tag, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    }

    ImGui::Separator();
    ImGui::TextDisabled(TR("Split views share one camera for now."));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Each split renders the same camera; per-view cameras\n"
                          "are not implemented yet. Sub-views do get their own\n"
                          "offscreen target."));
    }
    ImGui::EndPopup();
}

void ViewportPanel::drawSpeedMenu() {    placeDropdown();
    if (!ImGui::BeginPopup("##vp_speed_menu")) return;

    ImGui::SeparatorText(TR("Camera speed"));
    float s = m_ctx.navSensitivity();
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::SliderFloat("##vp_speed", &s, 0.1f, 20.0f, "%.2f")) {
        m_ctx.navSensitivityRef() = s;
        VK_LOG_INFO("VP-SPEED %.2f", static_cast<double>(s));
    }
    logRect("VP-SPEEDITEM", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::TextDisabled(TR("Hold RMB and use the wheel to change it\n"
                           "on the fly, without opening this menu."));

    // 基于距离的相机速度：离注视点越远飞得越快（UE 的 "Camera Speed
    // scaled by distance"）。默认关 —— 打开后同样一段 WASD 会走得更远，
    // 这是**行为改变**，必须是用户显式选的。
    bool scaled = m_ctx.navSpeedScalesWithDistance();
    if (ImGui::Checkbox(TR("Scale with distance"), &scaled)) {
        m_ctx.navSpeedScaleRef() = scaled;
        VK_LOG_INFO("VP-SPEED distance=%.0f", scaled ? 1.0 : 0.0);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("The further the camera is from its pivot, the faster\n"
                          "it flies - so the same drag feels the same at any zoom."));
    }

    ImGui::EndPopup();
}

void ViewportPanel::drawLightMenu() {
    placeDropdown();
    if (!ImGui::BeginPopup("##vp_light_menu")) return;
    const int n = render::Renderer::kViewModeCount;
    const int cur = static_cast<int>(m_renderer.viewMode());
    ImGui::SeparatorText(TR("Lighting mode"));
    for (int i = 0; i < n; ++i) {
        const auto vm = static_cast<render::Renderer::ViewMode>(i);
        if (ImGui::RadioButton(TR(render::Renderer::viewModeName(vm)), i == cur)) {
            m_renderer.setViewMode(vm);
            VK_LOG_INFO("VP-LIGHT %s", render::Renderer::viewModeName(vm));
            // 单选，选完关掉（否则弹层挂着吃掉下一次点击 —— 见 drawSpaceMenu）
            ImGui::CloseCurrentPopup();
        }
        char tag[40];
        std::snprintf(tag, sizeof(tag), "VP-LIGHTITEM %d", i);
        logRect(tag, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    }
    ImGui::Separator();
    ImGui::TextDisabled(TR("Shift+1..%d does the same"), n);
    ImGui::EndPopup();
}

void ViewportPanel::drawShowMenu() {
    placeDropdown();
    if (!ImGui::BeginPopup("##vp_show_menu")) return;
    EditorContext::ViewportOverlay& ov = m_ctx.vpOverlayRef();

    ImGui::SeparatorText(TR("Show flags"));

    // 每个勾选框都打矩形（VP-SHOWITEM <flag>）：脚本点的是**真实的框**，
    // 不是"第几行 + 固定像素"。
    bool grid = m_ctx.showGridRef();
    if (ImGui::Checkbox(TR("Reference grid"), &grid)) {
        m_ctx.showGridRef() = grid;
        VK_LOG_INFO("VP-SHOW grid=%d", grid ? 1 : 0);
    }
    tagItem("VP-SHOWITEM grid");
    bool giz = m_showGizmo;
    if (ImGui::Checkbox(TR("Transform gizmo"), &giz)) {
        m_showGizmo = giz;
        VK_LOG_INFO("VP-SHOW gizmo=%d", giz ? 1 : 0);
    }
    tagItem("VP-SHOWITEM gizmo");
    bool hud = m_showOverlay;
    if (ImGui::Checkbox(TR("Viewport HUD"), &hud)) {
        m_showOverlay = hud;
        VK_LOG_INFO("VP-SHOW hud=%d", hud ? 1 : 0);
    }
    tagItem("VP-SHOWITEM hud");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Master switch for the corner overlays below."));
    }
    bool sky = m_renderer.skyEnabled();
    if (ImGui::Checkbox(TR("Sky"), &sky)) {
        m_renderer.skyEnabled() = sky;
        VK_LOG_INFO("VP-SHOW sky=%d", sky ? 1 : 0);
    }
    tagItem("VP-SHOWITEM sky");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Off = the scene clears to the flat background colour."));
    }

    ImGui::SeparatorText(TR("Overlay panels"));
    ImGui::BeginDisabled(!m_showOverlay);
    ImGui::Checkbox(TR("Stats HUD"), &ov.stats);
    ImGui::Checkbox(TR("Navigation gizmo"), &ov.navGizmo);
    ImGui::EndDisabled();

    ImGui::Separator();
    if (ImGui::MenuItem(TR("Show all"))) {
        m_showGizmo = true;
        m_showOverlay = true;
        m_ctx.showGridRef() = true;
        ov.stats = ov.navGizmo = true;
        VK_LOG_INFO("VP-SHOW all=1");
    }

    ImGui::EndPopup();
}

void ViewportPanel::drawVpSettingsMenu() {
    placeDropdown();
    if (!ImGui::BeginPopup("##vp_settings_menu")) return;

    ImGui::SeparatorText(TR("Reference grid"));
    struct G { const char* id; const char* label; float lo, hi; float speed; };
    const G gs[4] = {
        {"grid.minorStep", "Minor step (m)", 0.1f, 10.0f, 0.01f},
        {"grid.majorStep", "Major step (m)", 1.0f, 100.0f, 0.1f},
        {"grid.extent", "Extent (m)", 5.0f, 500.0f, 1.0f},
        {"grid.alpha", "Opacity", 0.0f, 1.0f, 0.005f},
    };
    for (const G& g : gs) {
        void* p = render::paramPtr(g.id);
        if (!p) continue;
        float v = *static_cast<float*>(p);
        ImGui::PushID(g.id);
        ImGui::SetNextItemWidth(150.0f);
        if (ImGui::DragFloat(TR(g.label), &v, g.speed, g.lo, g.hi, "%.3f")) {
            *static_cast<float*>(p) = v;
            editor::emitParamChange(g.id);
            VK_LOG_INFO("VP-SET %s=%.3f", g.id, static_cast<double>(v));
        }
        char tag[48];
        std::snprintf(tag, sizeof(tag), "VP-SETITEM %s", g.id);
        tagItem(tag);
        ImGui::PopID();
    }

    ImGui::SeparatorText(TR("Background"));
    const bool sky = m_renderer.skyEnabled();
    if (ImGui::RadioButton(TR("Sky / environment"), sky)) {
        m_renderer.skyEnabled() = true;
        VK_LOG_INFO("VP-SET sky=1");
    }
    if (ImGui::RadioButton(TR("Solid color"), !sky)) {
        m_renderer.skyEnabled() = false;
        VK_LOG_INFO("VP-SET sky=0");
    }

    ImGui::Separator();
    if (ImGui::MenuItem(TR("Reset grid settings"))) {
        const char* ids[4] = {"grid.minorStep", "grid.majorStep", "grid.extent",
                              "grid.alpha"};
        for (const char* id : ids) {
            render::resetParam(id);
            editor::emitParamChange(id);
        }
        VK_LOG_INFO("VP-SET reset=1");
    }

    ImGui::EndPopup();
}

void ViewportPanel::focusSelection() const {
    if (!m_ctx.hasSelection()) return;
    focusEntity(m_ctx.selection());
}

void ViewportPanel::focusEntity(ecs::Entity e) const {
    // 与 Hierarchy 双击共用同一份实现（editor/EditorFocus.h）
    editor::focusCameraOnEntity(m_ctx.editorScene(), e);
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
        m_ctx.setStatus(TR("Drop to floor: click-select an object in the 3D viewport first"));
        VK_LOG_INFO("drop-to-floor: skipped ('%s' was not picked in the "
                    "viewport)", nm);
        return;
    }

    scene::Scene& sc = m_ctx.editorScene();
    auto* t = sc.world().get<ecs::TransformComponent>(e);
    if (!t) return;

    // 地面高度：有地形就用地形，没地形才是栅格面 z=0。
    // ⚠ 之前一律按 z=0 —— 那在"空场景 + 地形"的新工程里等于把物体埋进
    //   土里（用户报的"按 0 落不到地面"就是这个）。heightAtWorld 在没有
    //   地形时返回 0，所以老场景行为不变。
    const float groundZ = m_landscapeEditor
                              ? m_landscapeEditor->heightAtWorld(t->position.x,
                                                                 t->position.y)
                              : 0.0f;

    Aabb box = m_picking.worldAabb(sc, e);
    if (!box.valid) {
        // 没有网格的实体（GameStart / 水体 / 灯）：
        //   · GameStart 是"脚底在原点"的标记 → 直接把原点落到地面；
        //   · 其它（灯 / 相机）仍然跳过，落地对它们没意义。
        if (!sc.world().has<ecs::GameStartComponent>(e)) {
            VK_LOG_INFO("drop-to-floor: skipped ('%s' has no mesh bounds)", nm);
            return;
        }
        box.valid = true;
        box.min = t->position;
        box.max = t->position + glm::vec3(0.0f, 0.0f, 1.75f);
    }

    const float dz = groundZ - box.min.z;   // 世界空间里要把"底部"抬多高
    if (std::fabs(dz) < 1e-4f) {
        m_ctx.setStatus(TR("Already on the ground"));
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
    // "基于距离的相机速度"（默认关 → 系数恒 1，行为与改动前一致）
    speed *= m_ctx.navDistanceSpeedFactor(glm::length(cam.target() - cam.position()));
    if (io.KeyShift) speed *= 3.0f;
    cam.moveLocal(glm::vec3(r * speed, u * speed, f * speed));
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

    ImGui::SeparatorText(TR("Add"));

    // ⚠ logRect 必须在 BeginMenu 之外无条件调用：BeginMenu 只有在子菜单
    //   展开时才返回 true，写在 if 里会导致"悬停前永远拿不到父项矩形"，
    //   自动化脚本就死锁在"等矩形 → 才知道悬停哪"上（实测踩过）。
    //   BeginMenu 无论返回值如何都会提交 item，GetItemRectMin/Max 始终有效。
    const bool meshOpen = ImGui::BeginMenu(TR("Mesh"));
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
    const bool lightOpen = ImGui::BeginMenu(TR("Light"));
    menuRect("VP-ADD-Light", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (lightOpen) {
        // Sunlight：配置场景级太阳（有阴影、全局唯一），选中太阳实体
        {
            const bool clicked = ImGui::MenuItem(TR("Sunlight"));
            menuRect("VP-ADD-Sunlight", ImGui::GetItemRectMin(),
                     ImGui::GetItemRectMax());
            if (clicked) {
                addEntity("Sunlight", [this]() {
                    return createSunlight(m_ctx.editorScene());
                });
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("The scene's key light - simulates the sun.\n"
                              "Casts shadows, lights the whole world."));
        }
        // 实体级方向光：补光，可摆多盏，无阴影
        {
            const bool clicked = ImGui::MenuItem(TR("Directional Light"));
            menuRect("VP-ADD-DirectionalLight", ImGui::GetItemRectMin(),
                     ImGui::GetItemRectMax());
            if (clicked) {
                addEntity("Directional Light", [this]() {
                    return createDirectionalLight(m_ctx.editorScene(), m_addSpot);
                });
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("A secondary directional fill light\n"
                              "(no shadows, add as many as you like)."));
        }
        {
            const bool clicked = ImGui::MenuItem(TR("Spot Light"));
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
            const bool clicked = ImGui::MenuItem(TR("Point Light"));
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

    // Player：两种相机视角的玩家（WASD + 鼠标，运行态驱动）
    const bool playerOpen = ImGui::BeginMenu(TR("Player"));
    menuRect("VP-ADD-Player", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (playerOpen) {
        const bool fp = ImGui::MenuItem(TR("First-Person Player"));
        menuRect("VP-ADD-FirstPerson", ImGui::GetItemRectMin(),
                 ImGui::GetItemRectMax());
        if (fp) {
            ecs::Entity created{};
            addEntity("First-Person Player", [&]() {
                created = createPlayer(m_ctx.editorScene(), m_ctx.assets(),
                                       ecs::PlayerMode::FirstPerson, m_addSpot);
                return created;
            });
            // 创建后打开角色详情窗口（Persona 风格）
            if (created.valid()) m_ctx.requestCharacterWindow(created);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                TR("The camera IS the player.\n"
                "Run (Play) then: WASD to move, mouse to look around."));

        const bool tp = ImGui::MenuItem(TR("Third-Person Player"));
        menuRect("VP-ADD-ThirdPerson", ImGui::GetItemRectMin(),
                 ImGui::GetItemRectMax());
        if (tp) {
            ecs::Entity created{};
            addEntity("Third-Person Player", [&]() {
                created = createPlayer(m_ctx.editorScene(), m_ctx.assets(),
                                       ecs::PlayerMode::ThirdPerson, m_addSpot);
                return created;
            });
            // 创建后打开角色详情窗口（Persona 风格）
            if (created.valid()) m_ctx.requestCharacterWindow(created);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                TR("A little cartoon character with an orbit camera.\n"
                "Run (Play) then: WASD moves the character (it turns with "
                "you), mouse orbits the camera."));

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
        m_ctx.notify(TR("No mesh to build a collider from"));
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
        m_ctx.notify(TR("Collider build failed (empty mesh?)"));
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
    m_ctx.notify(TR("Removed collision body"));
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
        if (clicked) {
            if (m_ctxPressOnSelected) {
                ++m_ctxMenuSeq;
                m_ctxMenuPos = mouse;
                if (std::getenv("MYVK_LOG_RECTS")) {
                    VK_LOG_INFO("VP-OBJ-OPEN seq=%d at=(%.0f,%.0f)", m_ctxMenuSeq,
                                static_cast<double>(mouse.x),
                                static_cast<double>(mouse.y));
                }
                ImGui::OpenPopup("##vp_object");
            } else {
                // 空白处 → "Viewport Display"（浮层显隐 / 统计 HUD 逐项）。
                // 判定与物体菜单共用一套"按下 → 松开位移 < 6px"的逻辑，
                // 所以右键拖拽转视角永远不会误弹菜单。
                m_ctxMenuPos = mouse;
                ImGui::OpenPopup("##vp_display");
            }
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

    ImGui::SeparatorText(nm ? nm->c_str() : TRL("Object"));

    // 父项矩形必须**无条件**打点（BeginMenu 只在子菜单展开时才返回 true，
    // 写在 if 里脚本会永远等不到它）—— 见 drawAddMenu 里的同一条注释。
    const bool addOpen = ImGui::BeginMenu(TR("Add Collision Body"));
    menuRect("VP-OBJ-Add", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (addOpen) {
        const bool c1 = ImGui::MenuItem(TR("Convex Hull (QuickHull)"));
        menuRect("VP-OBJ-AddConvex", ImGui::GetItemRectMin(),
                 ImGui::GetItemRectMax());
        if (c1) addCollisionBody(e, ecs::ColliderShape::ConvexHull);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                TR("Fits a convex hull around the mesh (QuickHull).\n"
                "Tightest fit - the industry-standard collision shape."));
        }

        const bool c2 = ImGui::MenuItem(TR("Capsule"));
        menuRect("VP-OBJ-AddCapsule", ImGui::GetItemRectMin(),
                 ImGui::GetItemRectMax());
        if (c2) addCollisionBody(e, ecs::ColliderShape::Capsule);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                TR("A capsule wrapping the object's bounds.\n"
                "Cheap and smooth - good for characters."));
        }
        ImGui::EndMenu();
    }

    const bool hasCollider = (cc != nullptr);
    if (ImGui::MenuItem(TR("Remove Collision Body"), nullptr, false, hasCollider)) {
        removeCollisionBody(e);
    }
    menuRect("VP-OBJ-Remove", ImGui::GetItemRectMin(),
             ImGui::GetItemRectMax());

    ImGui::Separator();

    bool editingColl = m_ctx.colliderEdit();
    if (ImGui::MenuItem(TR("Edit Collider with W/E/R"), nullptr, &editingColl,
                        hasCollider)) {
        m_ctx.setColliderEdit(editingColl);
        VK_LOG_INFO("collision: gizmo target -> %s",
                    editingColl ? "collider" : "entity");
    }
    menuRect("VP-OBJ-EditCollider", ImGui::GetItemRectMin(),
             ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered() && !hasCollider) {
        ImGui::SetTooltip(TR("Add a collision body first"));
    }

    if (hasCollider) {
        bool solid = cc->solid;
        if (ImGui::MenuItem(TR("Solid (blocks other colliders)"), nullptr, &solid)) {
            m_ctx.structuralEdit("Toggle Collider Solid", [&]() {
                if (auto* p = w.get<ecs::CollisionComponent>(e))
                    p->solid = solid;
            });
        }
        menuRect("VP-OBJ-Solid", ImGui::GetItemRectMin(),
                 ImGui::GetItemRectMax());

        ImGui::SeparatorText(
            cc->shape == ecs::ColliderShape::ConvexHull ? TRL("convex hull")
                                                        : "capsule");
    }

    ImGui::EndPopup();
}

// ---------------------------------------------------------------- 主流程

// ============================================================
// GameStart 出生点标记：Q 版小人（编辑器叠加层，2026-10-21 第八轮）
// ============================================================
// 为什么画在叠加层、而不是给实体挂一个网格：
//   · 出生点**不该出现在游戏画面里**（UE5 的 PlayerStart 同理只在编辑器可见）；
//   · 场景里就不会多出一个"占位小人实体"，Hierarchy 保持干净（用户要求
//     "场景里不摆放任何物体"）。
// 比例由"投影 1.75 m 高得到的像素高度"决定 —— 离得远自然变小，和真实物体
// 一样有透视感，而不是一个恒定大小的图标。
namespace {

void gsEllipse(ImDrawList* dl, float cx, float cy, float rx, float ry,
               ImU32 col) {
    ImVec2 pts[28];
    for (int i = 0; i < 28; ++i) {
        const float a = 6.2831853f * static_cast<float>(i) / 28.0f;
        pts[i] = ImVec2(cx + std::cos(a) * rx, cy + std::sin(a) * ry);
    }
    dl->AddConvexPolyFilled(pts, 28, col);
}

void drawGameStartMarker(ImDrawList* dl, const glm::mat4& vp,
                         const glm::vec2& vpPos, const glm::vec2& vpSize,
                         const glm::vec3& feet, float yawRad) {
    glm::vec2 f2(0.0f), t2(0.0f);
    if (!PickingSystem::worldToScreen(vp, vpPos, vpSize, feet, f2)) return;
    if (!PickingSystem::worldToScreen(vp, vpPos, vpSize,
                                      feet + glm::vec3(0.0f, 0.0f, 1.75f),
                                      t2))
        return;

    float H = std::fabs(f2.y - t2.y);
    if (H < 16.0f) H = 16.0f;      // 远处保底：别缩成一个点
    if (H > 240.0f) H = 240.0f;
    const float cx = f2.x;
    const float bottom = f2.y;

    const ImU32 bodyCol = IM_COL32(86, 152, 255, 235);
    const ImU32 skinCol = IM_COL32(255, 219, 186, 245);
    const ImU32 lineCol = IM_COL32(46, 48, 66, 235);
    const ImU32 eyeCol = IM_COL32(40, 40, 58, 255);

    // 地面投影：先把小人"钉"在地上，不然读不出它站在哪
    gsEllipse(dl, cx, bottom, H * 0.22f, H * 0.065f, IM_COL32(0, 0, 0, 90));

    // 脚
    dl->AddRectFilled(ImVec2(cx - H * 0.115f, bottom - H * 0.075f),
                      ImVec2(cx - H * 0.015f, bottom), lineCol, H * 0.02f);
    dl->AddRectFilled(ImVec2(cx + H * 0.015f, bottom - H * 0.075f),
                      ImVec2(cx + H * 0.115f, bottom), lineCol, H * 0.02f);

    // 身体：短粗的圆角块（Q 版的关键是"头大身子小"）
    const float bodyTop = bottom - H * 0.46f;
    dl->AddRectFilled(ImVec2(cx - H * 0.155f, bodyTop),
                      ImVec2(cx + H * 0.155f, bottom - H * 0.055f), bodyCol,
                      H * 0.075f);
    dl->AddRect(ImVec2(cx - H * 0.155f, bodyTop),
                ImVec2(cx + H * 0.155f, bottom - H * 0.055f), lineCol,
                H * 0.075f, 0, 1.6f);

    // 头：比身体还宽，压在肩膀上方一点点
    const float headR = H * 0.235f;
    const float headCy = bodyTop - headR * 0.82f;
    dl->AddCircleFilled(ImVec2(cx, headCy), headR, skinCol, 32);
    dl->AddCircle(ImVec2(cx, headCy), headR, lineCol, 32, 1.8f);

    // 眼睛 + 嘴（"简单粗糙"就够，不做五官细节）
    const float eyeDx = headR * 0.36f;
    const float eyeR = std::max(headR * 0.15f, 1.0f);
    dl->AddCircleFilled(ImVec2(cx - eyeDx, headCy - headR * 0.08f), eyeR,
                        eyeCol, 12);
    dl->AddCircleFilled(ImVec2(cx + eyeDx, headCy - headR * 0.08f), eyeR,
                        eyeCol, 12);
    dl->PathArcTo(ImVec2(cx, headCy + headR * 0.10f), headR * 0.42f, 0.35f,
                  3.1415926f - 0.35f, 16);
    dl->PathStroke(lineCol, 0, std::max(headR * 0.09f, 1.0f));

    // 朝向：脚下一支小箭头（把 yaw 方向投影到屏幕，所以会跟着相机变）
    glm::vec2 a2(0.0f), b2(0.0f);
    const glm::vec3 dir(std::cos(yawRad), std::sin(yawRad), 0.0f);
    if (PickingSystem::worldToScreen(vp, vpPos, vpSize, feet, a2) &&
        PickingSystem::worldToScreen(vp, vpPos, vpSize, feet + dir * 1.4f,
                                     b2)) {
        glm::vec2 d(b2.x - a2.x, b2.y - a2.y);
        const float len = glm::length(d);
        if (len > 1.0f) {
            d /= len;
            const glm::vec2 n(-d.y, d.x);
            const glm::vec2 p(b2.x, b2.y);
            dl->AddTriangleFilled(
                ImVec2(p.x + d.x * H * 0.10f, p.y + d.y * H * 0.10f),
                ImVec2(p.x - d.x * H * 0.05f + n.x * H * 0.06f,
                       p.y - d.y * H * 0.05f + n.y * H * 0.06f),
                ImVec2(p.x - d.x * H * 0.05f - n.x * H * 0.06f,
                       p.y - d.y * H * 0.05f - n.y * H * 0.06f),
                IM_COL32(255, 214, 92, 230));
        }
    }

    // 自动化锚点（整只小人的屏幕矩形）+ 名字标签
    const ImVec2 tl(cx - H * 0.3f, headCy - headR - H * 0.16f);
    const ImVec2 br(cx + H * 0.3f, bottom + H * 0.10f);
    logRect("GAMESTART-MARK", tl, br);
    dl->AddText(ImVec2(cx - ImGui::CalcTextSize("GameStart").x * 0.5f,
                       br.y + 2.0f),
                IM_COL32(180, 220, 255, 210), "GameStart");
}

} // namespace

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
    // 2026-10-21：视口上方现在有一条**标签条**（ViewportTabs），它的第一页
    // 就叫 "Viewport" —— 再留一条窗口标题栏就是同一个名字写两遍，还白吃
    // 30px。所以标题栏一律不要（全屏时本来也不要）。
    // ⚠ 代价：标题栏那个 × 没了。关视口改走 View ▸ Viewport / Alt+4；
    //   标签条上 Viewport 这一页是**不可关**的（关掉它整块区域就空了）。
    flags |= ImGuiWindowFlags_NoTitleBar;
    if (gameFullscreen) {
        flags |= ImGuiWindowFlags_NoBringToFrontOnFocus;
    }

    char title[64];
    // 用 "###viewport" 固定窗口 ID：否则 [PLAYING] / [PAUSED] 前缀变化会被
    // ImGui 当成另一个窗口，位置与尺寸的记忆全部丢失
    const char* state = m_ctx.isPlaying()
                            ? TR("  [PLAYING]")
                            : (m_ctx.isPaused() ? TR("  [PAUSED]") : "");
    std::snprintf(title, sizeof(title), TR("Viewport%s###viewport"), state);
    if (!ImGui::Begin(title, nullptr, flags)) {
        ImGui::End();
        if (gameFullscreen) ImGui::PopStyleVar();
        return;
    }

    // 全屏时工具栏不画（否则会盖在游戏画面上），退出全屏有 HUD 上的按钮和 F 键
    if (!gameFullscreen) {
        drawToolbar();
        ImGui::Separator();
    }

    // ============================================================
    // 分屏布局（2026-10-17 第四轮）
    // ------------------------------------------------------------
    // 用户以为工具条右上那四个 1/2V/2H/4 是"渲染分辨率"，其实是**视口分屏**。
    // 挪到"透视"右边的同时把这件功能真正接通 —— 一个点了看不见任何变化的
    // 下拉，和没做没有区别（本项目的一条硬纪律：开关必须真的接到渲染器）。
    //
    // 落地方式：
    //   · **主渲染目标只渲染"一格"**（尺寸 = 单格尺寸），四屏不会摊薄分辨率；
    //   · 其余格用渲染器已有的子视口目标（Renderer::setSubviewSize /
    //     setSubviewCamera / subviewTextureId），每格各一个相机：
    //       格 0 = 用户的透视相机（**唯一可交互的一格**）
    //       格 1..3 = 前 / 顶 / 右三个正交方向（同一位置、同一 FOV）
    //   · 格与格之间留 2px 缝，缝里露出深底色当分隔线。
    //
    // ⚠ 单屏（默认）时这几个表达式全都退化成恒等（cols=rows=1）——
    //   既有视口的交互、拾取、浮层行为一个像素都不变。
    // ============================================================
    const ViewportLayout vlayout = m_ctx.viewportLayout();
    int gCols = 1, gRows = 1;
    switch (vlayout) {
        case ViewportLayout::Single: break;
        case ViewportLayout::TwoVertical: gCols = 2; break;
        case ViewportLayout::TwoHorizontal: gRows = 2; break;
        case ViewportLayout::Quad: gCols = 2; gRows = 2; break;
    }
    constexpr float kPaneGap = 2.0f;
    const int paneCount = gCols * gRows;

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    // 整块区域的尺寸（= 各格 + 缝），保证格宽乘出来正好铺满
    const float fullW = std::max(avail.x, kMinViewport * gCols);
    const float fullH = std::max(avail.y, kMinViewport * gRows);
    const float paneW = (fullW - kPaneGap * (gCols - 1)) / gCols;
    const float paneH = (fullH - kPaneGap * (gRows - 1)) / gRows;

    // 主渲染目标 / 子视口目标都用**单格**尺寸
    const std::uint32_t wantW = static_cast<std::uint32_t>(paneW);
    const std::uint32_t wantH = static_cast<std::uint32_t>(paneH);

    if (wantW != m_requestedW || wantH != m_requestedH) {
        m_requestedW = wantW;
        m_requestedH = wantH;
        m_resizePending = true;
        m_renderer.setViewportSize(wantW, wantH);
    }
    for (int i = 0; i < paneCount - 1; ++i) {
        m_renderer.setSubviewSize(static_cast<size_t>(i), wantW, wantH);
    }
    // 子视口相机：与主相机**同一位置**，朝向换成三个正交方向。
    // 为什么不是"四格同一个相机"：那样四格画面一模一样，看不出分屏有什么用；
    // 为什么要换子格而不是主格：主格是用户正在飞的相机，不能被动。
    if (paneCount > 1) {
        const scene::Camera& cam0 = m_ctx.activeScene().camera();
        const int dirs[3] = {kViewNegY, kViewPosZ, kViewPosX};  // 前 / 顶 / 右
        for (int i = 0; i < paneCount - 1; ++i) {
            scene::Camera c = cam0;
            snapViewTo(c, dirs[i % 3]);
            m_renderer.setSubviewCamera(static_cast<size_t>(i), c);
        }
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const std::uint64_t texId = m_renderer.viewportTextureId();
    // 占位/图像的**控件矩形**永远是整块区域（拾取落点、脚本锚点都以它为准）
    const ImVec2 imageSize(fullW, fullH);

    if (!m_resizePending && texId != 0) {
        // 用 Dummy 占位、用 draw list 画各格：
        // 这样"一个控件覆盖整块区域"的交互模型不变（hover 一直有效），
        // 而内部可以任意切分。
        ImGui::Dummy(imageSize);
        const ImVec2 origin = ImGui::GetItemRectMin();
        dl->AddRectFilled(origin,
                          ImVec2(origin.x + fullW, origin.y + fullH),
                          IM_COL32(10, 11, 14, 255));
        for (int r = 0; r < gRows; ++r) {
            for (int c = 0; c < gCols; ++c) {
                const int idx = r * gCols + c;
                const ImVec2 a(origin.x + c * (paneW + kPaneGap),
                               origin.y + r * (paneH + kPaneGap));
                const ImVec2 b(a.x + paneW, a.y + paneH);
                const std::uint64_t t =
                    idx == 0 ? texId
                             : m_renderer.subviewTextureId(
                                   static_cast<size_t>(idx - 1));
                if (t != 0) {
                    dl->AddImage(static_cast<ImTextureID>(t), a, b);
                } else {
                    // 子视口目标还在建（apply 要等一次 waitIdle）→ 先留空
                    dl->AddRectFilled(a, b, IM_COL32(24, 26, 30, 255));
                }
            }
        }
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
    // 分屏时**只有左上那一格接受键鼠交互**（它是用户的透视视图，其余格是
    // 前/顶/右的只读预览）。所以交互矩形取"第一格"的尺寸，而且鼠标必须真的
    // 落在第一格里 —— 否则在别的格子上也能拖手柄 / 点选，手感会非常错乱。
    // （单屏时 paneW/paneH 就等于整块尺寸，这一整段退化成恒等。）
    const ImVec2 pane0Min = imgMin;
    const ImVec2 pane0Size(paneW, paneH);
    const ImVec2 mousePos = ImGui::GetIO().MousePos;
    const bool inPane0 = mousePos.x >= pane0Min.x &&
                         mousePos.x < pane0Min.x + pane0Size.x &&
                         mousePos.y >= pane0Min.y &&
                         mousePos.y < pane0Min.y + pane0Size.y;
    const bool hovered = ImGui::IsItemHovered() && inPane0;
    // 上报给 EditorContext：运行态 Player 的鼠标视角控制按它门控。
    // 注意这里写的是**矩形**而不是 hover —— 见 setViewportRect 的说明
    // （编辑器的 nav 键盘模式会把 IsItemHovered 一直压成 false）。
    m_ctx.setViewportRect(glm::vec2(pane0Min.x, pane0Min.y),
                          glm::vec2(pane0Size.x, pane0Size.y));

    // 自动化用（MYVK_LOG_RECTS=1）：视口图像的屏幕矩形。
    // 拖拽落点 / 手柄命中都在这块矩形里，脚本靠它算坐标。
    // ⚠ 这里报的是**整块区域**（不是第一格）：脚本按它算中心点，单屏时两者
    //   完全一致；分屏时"整块"才是这块控件的真实范围。
    logRect("VP-RECT", imgMin,
            ImVec2(imgMin.x + imgSz.x, imgMin.y + imgSz.y));


    const glm::vec2 vpPos(pane0Min.x, pane0Min.y);
    const glm::vec2 vpSize(pane0Size.x, pane0Size.y);

    // 1) 相机导航：**只在编辑态**。
    //    Play 期间相机是运行态场景自己的东西 —— 用户明确要的就是"运行前的
    //    视口按 UE 那套来，运行后归游戏管"。下面 1.5) 是"归游戏管"的入口。
    if (m_ctx.isEditing()) handleNavigation(hovered);

    // 1.5) Play 态：**左键点视口 = 接管操作**（P8，2026-10-23）
    //
    // "发现"必须在这里：只有面板知道鼠标是不是真的落在视口里。执行在
    // EditorApp::updatePlayMouse（它拿得到 Window，能改光标模式）。
    //
    // 只在**未捕获**时收这个点击：捕获态下光标已经隐藏且锁在窗口中心，
    // ImGui 的鼠标被关掉了（NoMouse），这里的 hovered 恒为 false —— 两边
    // 的判断是自洽的，不会出现"捕获中又点了一次视口"。
    if (!m_ctx.isEditing() && !m_ctx.playMouseCaptured() && hovered &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        m_ctx.requestPlayMouseCapture();
        // 正式锚点：证明"点击这一下"确实被视口收到了（而不是被别的面板吃掉）。
        // 它和 EditorApp 的 `EDIT-MOUSE captured=1` 成对出现，中间任何一环
        // 断掉都能从这两条日志里直接读出来。
        VK_LOG_INFO("PLAY-CLICK viewport=1 (requesting capture)");
    }

    // 2) 碰撞框线框（**先画**，让手柄压在它上面；X-ray，不参与深度测试）
    if (m_ctx.isEditing()) m_colliders.draw(m_ctx, vpPos, vpSize, dl, hovered);

    // 3) gizmo（先跑：它可能在本次点击里"吃掉"鼠标）。只在编辑态有效
    scene::Camera& cam = m_ctx.activeScene().camera();

    // 2.5) 编辑模式的笔刷（2026-10-20）
    //
    // 地形 / 植被模式下**左键归工具**：不画手柄、不做点选。这三件事抢同一
    // 个键（左键），必须在这里一次分派清楚 —— 分开写条件最容易出现
    // "刷地形的时候顺手把下面的物体拖走了"。
    //
    // ⚠ 右键/中键的相机导航在上面已经处理过，不受影响：刷地形时仍然可以
    //   一边按住右键转视角、一边左键刷。
    const EditMode editMode = m_ctx.editMode();
    // ⚠ 判据是"这个模式**自己**要吃左键"，**不是** "editMode != Select"。
    //
    // 上一版写的是 `editMode != EditMode::Select`，于是九项全部接进来之后，
    // Modeling / Fracture / Animation / PCG 四个模式也把左键霸占了 —— 它们在
    // 视口里根本不需要笔刷，结果是"进了建模模式就点不中任何物体，也拖不动
    // 手柄，只能先切回 Select 选好再切回来"。UE5 里这四个模式是正常可选的
    // （选中项就是操作对象），所以只有真正刷东西的四个模式才归"工具"。
    //
    // Modeling 也在这里：它的左键是"点选顶点/边/面"，同样必须独占 —— 否则
    // 一按左键就先被 gizmo 或实体点选吃掉，"在建模模式里选不中任何一个面"。
    const bool brushMode = editMode == EditMode::Landscape ||
                           editMode == EditMode::Foliage ||
                           editMode == EditMode::MeshPaint ||
                           editMode == EditMode::BrushEdit ||
                           editMode == EditMode::Modeling;
    // ⚠ Water 模式**不**整段独占输入（2026-10-21 第八轮，用户要求）：
    //   "水体不是用鼠标拖动大小的，而是 W 拖 X/Y/Z 位置、R 缩放尺寸"。
    //   之前把 Water 塞进 brushMode，等于把变换手柄整个关掉 —— 水体只能靠
    //   鼠标拖出尺寸，正是用户不想用的那套。
    //   现在按**有没有选中**分工：
    //     · 选中了水体 → 手柄接管（W 平移 / E 旋转 / R 缩放）；
    //     · 没选中     → 左键拖拽才创建新水体（见 WaterEditor::viewportInput）。
    const bool waterToolActive =
        editMode == EditMode::Water && !m_ctx.hasSelection();
    const bool modeToolActive =
        m_ctx.isEditing() && (brushMode || waterToolActive);
    bool modeConsumed = false;
    if (modeToolActive) {
        if (editMode == EditMode::Landscape && m_landscapeEditor) {
            modeConsumed =
                m_landscapeEditor->viewportInput(vpPos, vpSize, hovered, dl);
        } else if (editMode == EditMode::Foliage && m_foliageEditor) {
            modeConsumed =
                m_foliageEditor->viewportInput(vpPos, vpSize, hovered, dl);
        } else if (editMode == EditMode::MeshPaint && m_meshPaintEditor) {
            modeConsumed =
                m_meshPaintEditor->viewportInput(vpPos, vpSize, hovered, dl);
        } else if (editMode == EditMode::BrushEdit && m_brushEditor) {
            modeConsumed =
                m_brushEditor->viewportInput(vpPos, vpSize, hovered, dl);
        } else if (editMode == EditMode::Modeling && m_modelingEditor) {
            modeConsumed =
                m_modelingEditor->viewportInput(vpPos, vpSize, hovered, dl);
        } else if (editMode == EditMode::Water && m_waterEditor) {
            modeConsumed =
                m_waterEditor->viewportInput(vpPos, vpSize, hovered, dl);
        }
    }

    // ---- 水体：body → Transform（手柄要站在水面上）----
    // 水体没有网格，变换手柄只认 TransformComponent；不先把 body 的位姿
    // 镜像进去，手柄会站在原点、拖了水面也不动。见 WaterEditor.h 的注释。
    if (m_ctx.isEditing() && m_ctx.hasSelection() && m_waterEditor) {
        m_waterEditor->syncTransformFromBody(m_ctx.selection());
    }

    // ---- GameStart 出生点标记（只在编辑态画；游戏里不出现）----
    if (m_ctx.isEditing() && dl) {
        const glm::mat4 gvp = PickingSystem::viewProj(cam, vpSize);
        ecs::World& gw = m_ctx.activeScene().world();
        gw.each<ecs::GameStartComponent>(
            [&](ecs::Entity ge, ecs::GameStartComponent& g) {
                if (!g.enabled) return;
                const auto* gtf = gw.get<ecs::TransformComponent>(ge);
                if (!gtf) return;
                drawGameStartMarker(dl, gvp, vpPos, vpSize, gtf->position,
                                    gtf->rotation.z + g.yaw);
            });
    }

    bool gizmoConsumed = false;
    if (!modeToolActive && m_showGizmo && m_ctx.isEditing()) {
        gizmoConsumed = m_gizmo.update(m_ctx, cam, vpPos, vpSize, hovered, dl);
    }

    // ---- 水体：Transform → body（移动/旋转/缩放写回去）----
    // 必须紧跟 gizmo.update：下一帧的 sync 会用 body 覆盖 Transform，
    // 所以"读回"与"写出"是成对的，缺一半就会互相打架。
    if (m_ctx.isEditing() && m_ctx.hasSelection() && m_waterEditor) {
        m_waterEditor->applyTransformToBody(m_ctx.selection());
    }

    // 3) 点选（编辑态、点左键、不在导航手势里、手柄没吃掉）
    //    左键专属于点选与拖手柄：导航手势只有右键（转头）和中键（平移）。
    const ImGuiIO& io = ImGui::GetIO();
    const bool navGesture = ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
                            ImGui::IsMouseDown(ImGuiMouseButton_Middle);
    if (m_ctx.isEditing() && !modeToolActive && !modeConsumed && hovered &&
        !gizmoConsumed && !navGesture && !m_gizmo.dragging() &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
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
                m_ctx.setStatus(TR("Editing collision body (W/E/R)"));
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
                m_ctx.setStatus(TR("Selected ") +
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

    // 4) 叠加信息：E14 起全部走四角浮层（drawStatsHud 等，见下面第 6 步）。
    //    原来这里的 drawOverlay（左上角统计文字）已经并进统计 HUD ——
    //    左上角让给了视图模式条。

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
                m_ctx.notify(std::string(TR("Imported ")) + rel + " (" +
                             std::to_string(imported.count()) + TR(" mesh)"));
                m_ctx.dirty() = true;
            } else {
                m_ctx.notify(TR("Failed to import ") + rel);
            }
        }
        ImGui::EndDragDropTarget();
    }

    // 6) E14 四角浮层。全部放在所有交互之后、ImGui::End() 之前 —— 它们是
    //    **子窗口**（见上面 E14 浮层那一段的说明）。
    //      编辑态 → 左上模式条 / 右上统计 HUD / 左下相机信息 / 右下导航球，
    //               每块各自一个开关（右键空白处的 Viewport Display 菜单）
    //      运行态 → 极简播放浮层（全屏按钮 + 状态），不含任何导航控件
    //
    //    Viewport Display 菜单必须在编辑态**每帧无条件提交**（不提交的帧
    //    ImGui 会直接把 popup 关掉），所以它的调用点在这里、不在 if 里。
    if (m_ctx.isEditing()) {
        drawDisplayMenu(vpPos, vpSize, hovered);
        // 自动化（MYVK_LOG_RECTS=1）：浮层开关状态**只在变化时**打一行。
        // 脚本不能靠"矩形日志还在不在打"判断显隐 —— `logRect` 对同一个矩形
        // 只打一条（按 tag + 4px 去抖），不是逐帧刷的。有了这一行，
        // "点一下菜单 → 状态真翻了" 和 "读盘后生效了" 都是可断言的。
        if (std::getenv("MYVK_LOG_RECTS")) {
            const EditorContext::ViewportOverlay& ov = m_ctx.vpOverlay();
            const int sig = (m_showOverlay ? 1 : 0) | (ov.stats ? 2 : 0) |
                            (ov.navGizmo ? 4 : 0);
            static int lastSig = -1;
            if (sig != lastSig) {
                lastSig = sig;
                VK_LOG_INFO("VP-OVERLAY master=%d stats=%d navGizmo=%d",
                            m_showOverlay ? 1 : 0, ov.stats ? 1 : 0,
                            ov.navGizmo ? 1 : 0);
            }
        }
        if (m_showOverlay) {
            const EditorContext::ViewportOverlay& ov = m_ctx.vpOverlay();
            if (ov.stats) drawStatsHud(vpPos, vpSize);
            // 左下角曾经是相机浮层；已并入 Inspector → World → Camera（见
            // EditorContext::ViewportOverlay 的说明），这里不再画任何东西。
            if (ov.navGizmo) drawNavGizmo(vpPos, vpSize);
        }
    } else {
        drawPlayHud(vpPos, vpSize);
    }

    ImGui::End();
    if (gameFullscreen) ImGui::PopStyleVar();
}

// ---------------------------------------------------------------- E14 浮层
//
// 四角分工：
//   左上 = （空）            右上 = 统计 HUD
//   左下 = 相机信息 + 书签    右下 = 导航 gizmo
//
// 左上原来是"视图模式条"（Lit/Unl/Nrm/... 八个按钮 + 当前模式名）。2026-10-17
// 用户指出：同一组视图模式在右上角的 Lighting 下拉里已经有了，左上这一条纯属
// 重复占地方，删掉。视图模式切换只剩两个入口：Lighting 下拉（ViewportPanel::
// drawLightMenu）与 Shift+1..8 快捷键（EditorApp::handleNavigation）。
//
// 为什么必须是**子窗口**而不是独立顶层窗口：视口窗口本身是焦点窗口，ImGui
// 每帧都会把焦点窗口 BringWindowToDisplayFront，于是独立窗口会被推到列表
// 前面、**被视口盖住**（第一版就是这么翻车的）。子窗口的绘制永远在父窗口
// 之后，天然压在 3D 画面之上。
//
// 位置全部跟着 Image 的矩形（vpPos / vpSize）走，全屏时 Image 就是整屏。
//
// 显隐分两层：`m_showOverlay`（工具栏 "HUD" 勾选框，也是 MYVK_EDITOR_NO_HUD
// 的落点）是**总闸**；总闸之上，四块各自还有一个开关存在 EditorContext 里，
// 右键视口空白处的 "Viewport Display" 菜单切换、经 Settings 落盘。
// 总闸关掉时四块全灭 —— 这就是 NO_HUD 逐像素回归能成立的前提。

// ---- 统计 HUD（右上）----
//
// 每一行都由 EditorContext::StatsHudItems 里的一个开关控制：调性能时全开，
// 盯画面时只留 FPS。宽度按**当前启用的行**里最长的一条算，关掉几行浮层就
// 真的变窄，而不是留一片空白。
void ViewportPanel::drawStatsHud(const glm::vec2& vpPos,
                                 const glm::vec2& vpSize) {
    const ImGuiStyle& st0 = ImGui::GetStyle();

    // ---- 折叠态：只留一颗小胶囊，点一下就展开 ----
    //
    // 折叠状态是**会话内**的（不落盘，也不进偏好表）：它回答的是"这一屏要
    // 不要看数字"，属于临时动作；而"这块 HUD 存不存在"属于偏好
    // （vp.overlay.stats）。两者分开，才不会出现"我折了一次，下次开编辑器
    // 它自己变成关的"。
    if (m_statsCollapsed) {
        const float cw = ImGui::CalcTextSize("Stats").x +
                         st0.FramePadding.x * 2.0f + 8.0f;
        const float chh = ImGui::GetTextLineHeightWithSpacing();
        ImGui::SetCursorScreenPos(
            ImVec2(vpPos.x + vpSize.x - cw - 10.0f, vpPos.y + 10.0f));
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImVec4(0.10f, 0.12f, 0.15f, 0.80f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.80f, 0.87f, 0.95f));
        const bool expand =
            ImGui::Button(TRL("Stats##vp_stats_expand"), ImVec2(cw, chh));
        ImGui::PopStyleColor(2);
        logRect("VP-RECT stats", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());
        if (expand) {
            m_statsCollapsed = false;
            if (std::getenv("MYVK_LOG_RECTS"))
                VK_LOG_INFO("VP-STATS-COLLAPSE 0");
        }
        return;
    }

    const EditorContext::StatsHudItems& it = m_ctx.statsHud();
    const scene::Scene& sc = m_ctx.activeScene();
    const render::Renderer::RenderStats& rs = m_renderer.renderStats();
    const ImGuiIO& io = ImGui::GetIO();

    std::vector<std::string> lines;
    char b[192];
    if (it.fps) {
        std::snprintf(b, sizeof(b), TR("%.0f FPS"),
                      static_cast<double>(io.Framerate));
        lines.emplace_back(b);
    }
    if (it.frameMs) {
        std::snprintf(b, sizeof(b), TR("%.2f ms"),
                      io.Framerate > 0.0f ? 1000.0 / io.Framerate : 0.0);
        lines.emplace_back(b);
    }
    if (it.drawCalls) {
        std::snprintf(b, sizeof(b), TR("draw calls  %u"), rs.drawCalls);
        lines.emplace_back(b);
    }
    if (it.triangles) {
        std::snprintf(b, sizeof(b), TR("triangles   %u"), rs.triangles);
        lines.emplace_back(b);
    }
    if (it.vram) {
        // 引擎没有自己的分配器（无 VMA），拿不到"已用"，这里报的是
        // **设备本地堆容量** —— 调资源预算时看的是同一个数。
        const double gb = static_cast<double>(
                              m_renderer.device().deviceLocalHeapBytes()) /
                          (1024.0 * 1024.0 * 1024.0);
        std::snprintf(b, sizeof(b), TR("VRAM        %.1f GB"), gb);
        lines.emplace_back(b);
    }
    if (it.resolution) {
        std::snprintf(b, sizeof(b), TR("viewport    %ux%u"), m_requestedW,
                      m_requestedH);
        lines.emplace_back(b);
    }
    if (it.viewMode) {
        std::snprintf(b, sizeof(b), TR("view mode   %s"),
                      render::Renderer::viewModeName(m_renderer.viewMode()));
        lines.emplace_back(b);
    }
    if (it.entities) {
        const scene::Scene::LightStats ls = sc.lightStats();
        std::snprintf(b, sizeof(b), TR("entities %zu   lights %zu"),
                      sc.objectCount(), ls.pointLights + ls.spotLights);
        lines.emplace_back(b);
    }
    if (it.selection) {
        const std::string* nm = m_ctx.hasSelection()
                                    ? sc.world().name(m_ctx.selection())
                                    : nullptr;
        std::snprintf(b, sizeof(b), TR("selected: %s"), nm ? nm->c_str() : "(none)");
        lines.emplace_back(b);
    }
    if (it.gizmo) {
        if (!m_ctx.isEditing()) {
            // 视口渲染的就是**运行态副本**本身 —— 游戏画面与编辑画面共用
            // 同一条离屏渲染链，只是相机换成了自由飞行视角。
            std::snprintf(b, sizeof(b), TR("%s  t=%.2f 秒%s"),
                          m_ctx.isPaused() ? TR("PAUSED") : TR("PLAYING"),
                          static_cast<double>(m_ctx.playTime()),
                          m_ctx.gameFullscreen() ? TR("  [fullscreen]") : "");
            lines.emplace_back(b);
        } else {
            std::snprintf(b, sizeof(b), TR("EDIT   gizmo: %s / %s"),
                          m_ctx.gizmoMode() == GizmoMode::Translate ? TR("move")
                          : m_ctx.gizmoMode() == GizmoMode::Rotate ? TR("rotate")
                                                                  : TR("scale"),
                          m_ctx.gizmoSpace() == GizmoSpace::World ? TR("world")
                                                                 : TR("local"));
            lines.emplace_back(b);
            // 拖动手柄时鼠标会挡住画面，这里报"沿哪个轴走了多少"
            // （吸附打开时末尾带 [snap]）是最直观的反馈。
            if (m_gizmo.dragging() && !m_gizmo.readout().empty()) {
                lines.push_back(m_gizmo.readout());
            }
        }
    }

    const ImGuiStyle& st = ImGui::GetStyle();
    float w = ImGui::CalcTextSize("(all stats hidden)").x;
    for (const std::string& s : lines) {
        w = std::max(w, ImGui::CalcTextSize(s.c_str()).x);
    }
    const float cw = w + st.WindowPadding.x * 2.0f + 6.0f;
    // 行距要用 GetTextLineHeightWithSpacing：TextUnformatted 每行前进的是
    // "行高 + ItemSpacing.y"。按 GetTextLineHeight 算会少几像素，10 行时
    // 刚好溢出 → 子窗口冒出滚动条，最后两行直接被滚出可视区（踩过）。
    const float lineH = ImGui::GetTextLineHeightWithSpacing();
    // +1 行给标题（"Stats  −"，点它折叠）
    const float rows = std::max(1.0f, static_cast<float>(lines.size())) + 1.0f;
    const float ch = lineH * rows + st.WindowPadding.y * 2.0f + 2.0f;

    const float statsX = vpPos.x + vpSize.x - cw - 10.0f;
    // 左上角的模式条删掉之后，这里不再需要"往下躲"的避让计算 —— 统计 HUD
    // 永远是右上角第一个浮层。
    const float statsY = vpPos.y + 10.0f;
    ImGui::SetCursorScreenPos(ImVec2(statsX, statsY));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.07f, 0.08f, 0.10f, 0.78f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.89f, 0.94f, 0.92f));
    // 浅色主题再补一个 TextDisabled（深底上的深灰字看不见）。深色主题维持
    // 原来的 2 个 push → 与改动前逐像素一致。
    const int stPush = ui::tokens().isLight ? 3 : 2;
    if (stPush == 3)
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, kOverlayTextDim);
    ImGui::BeginChild("##vp_stats", ImVec2(cw, ch), 0,
                      ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor(stPush);

    logRect("VP-RECT stats", ImGui::GetWindowPos(),
            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                   ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    // 标题行 = 折叠按钮。整块 HUD 的"隐藏"与"收起"是两件事：隐藏归偏好
    // （右键菜单），收起归这颗按钮。
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.13f, 0.15f, 0.19f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImVec4(0.20f, 0.24f, 0.30f, 1.0f));
    if (ImGui::SmallButton(TRL("Stats  -##vp_stats_collapse"))) {
        m_statsCollapsed = true;
        if (std::getenv("MYVK_LOG_RECTS")) VK_LOG_INFO("VP-STATS-COLLAPSE 1");
    }
    ImGui::PopStyleColor(2);

    if (lines.empty()) {
        ImGui::TextDisabled(TR("(all stats hidden)"));
    } else {
        for (const std::string& s : lines) ImGui::TextUnformatted(s.c_str());
    }

    // 编辑态才提示"右键空白处可以改这里" —— Play 态没有这块菜单
    ImGui::EndChild();
}

// ---- 相机信息 + 书签（左下）----
//
// 这里是原来的"右上角导航 HUD"搬家后的样子：灵敏度、栅格开关原样保留，
// 另外补上真正需要在摆机位时盯着看的四个数（位置 / 注视点 / FOV / 距离），
// 以及一排相机书签。
// ---- "Viewport Display" 菜单（右键视口空白处）----
//
// 只负责**画**这个 popup：按下 / 松开的判定在 drawObjectMenu 里（那里已经
// 记好了"右键按在哪儿、是不是按在选中物体上"，两半共用一套逻辑就不会出现
// "拖拽转视角顺手弹出菜单"）。本函数必须每帧无条件调用 —— 不提交的帧
// ImGui 会直接把它关掉。
void ViewportPanel::drawDisplayMenu(const glm::vec2& vpPos,
                                    const glm::vec2& vpSize, bool hovered) {
    (void)vpPos;
    (void)vpSize;
    (void)hovered;
    if (!m_ctx.isEditing()) return;

    ImGui::SetNextWindowPos(ImVec2(m_ctxMenuPos.x, m_ctxMenuPos.y),
                            ImGuiCond_Appearing);
    if (!ImGui::BeginPopup("##vp_display")) return;

    logRect("VP-DISPLAY-WIN", ImGui::GetWindowPos(),
            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                   ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    EditorContext::ViewportOverlay& ov = m_ctx.vpOverlayRef();
    EditorContext::StatsHudItems& st = m_ctx.statsHudRef();

    // 自动化（MYVK_LOG_RECTS=1）：每个开关的真实矩形都打出来 —— 脚本按
    // 日志矩形点击，不猜字体行高（和 VP-ADD-* 那套同一套路）。
    const auto itemRect = [](const char* tag) {
        if (!std::getenv("MYVK_LOG_RECTS")) return;
        logRect(tag, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    };

    ImGui::SeparatorText(TR("Overlay panels"));
    ImGui::Checkbox(TR("Stats HUD (top-right)"), &ov.stats);
    itemRect("VP-DISPLAY-ITEM stats");
    ImGui::Checkbox(TR("Navigation gizmo (bottom-right)"), &ov.navGizmo);
    itemRect("VP-DISPLAY-ITEM navGizmo");

    ImGui::SeparatorText(TR("Stats HUD lines"));
    ImGui::BeginDisabled(!ov.stats);
    ImGui::Checkbox(TR("FPS"), &st.fps);
    ImGui::SameLine();
    ImGui::Checkbox(TR("Frame time"), &st.frameMs);
    ImGui::Checkbox(TR("Draw calls"), &st.drawCalls);
    ImGui::SameLine();
    ImGui::Checkbox(TR("Triangles"), &st.triangles);
    ImGui::Checkbox(TR("VRAM"), &st.vram);
    ImGui::SameLine();
    ImGui::Checkbox(TR("Resolution"), &st.resolution);
    ImGui::Checkbox(TR("View mode"), &st.viewMode);
    ImGui::SameLine();
    ImGui::Checkbox(TR("Entities"), &st.entities);
    ImGui::Checkbox(TR("Selection"), &st.selection);
    ImGui::SameLine();
    ImGui::Checkbox(TR("Gizmo state"), &st.gizmo);
    ImGui::EndDisabled();

    // ---- 视口设置 ----
    // 原来这组控件在左下角相机浮层的 "View..." 弹出菜单里。相机浮层并入
    // Inspector 之后，这里才是它们的自然归宿（都是"视口怎么看"，不是
    // "相机在哪"）。
    ImGui::SeparatorText(TR("Grid"));
    auto setGrid = [](float minor, float major) {
        if (void* p = render::paramPtr("grid.minorStep")) {
            *static_cast<float*>(p) = minor;
        }
        if (void* p = render::paramPtr("grid.majorStep")) {
            *static_cast<float*>(p) = major;
        }
    };
    float curMinor = 1.0f;
    if (void* p = render::paramPtr("grid.minorStep")) {
        curMinor = *static_cast<float*>(p);
    }
    const float steps[3] = {1.0f, 0.5f, 0.1f};
    for (int i = 0; i < 3; ++i) {
        ImGui::PushID(5000 + i);
        char lb[24];
        std::snprintf(lb, sizeof(lb), TR("%g m"), static_cast<double>(steps[i]));
        if (ImGui::RadioButton(lb, std::fabs(curMinor - steps[i]) < 1e-4f)) {
            setGrid(steps[i], steps[i] * 10.0f);
        }
        ImGui::PopID();
        if (i < 2) ImGui::SameLine();
    }
    itemRect("VP-DISPLAY-ITEM gridstep");
    ImGui::Checkbox(TR("Reference grid"), &m_ctx.showGridRef());
    itemRect("VP-DISPLAY-ITEM grid");

    ImGui::SeparatorText(TR("Background"));
    {
        const bool sky = m_renderer.skyEnabled();
        if (ImGui::RadioButton(TR("Sky / environment"), sky)) {
            m_renderer.skyEnabled() = true;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton(TR("Solid color"), !sky)) m_renderer.skyEnabled() = false;
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                TR("Solid = the sky pass is skipped and the scene clears to the\n"
                "flat background colour (useful for silhouette checks)."));
        }
    }

    ImGui::Separator();
    if (ImGui::MenuItem(TR("Show all overlays"))) {
        ov.stats = ov.navGizmo = true;
        st = EditorContext::StatsHudItems{};
    }
    itemRect("VP-DISPLAY-ITEM all");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Turn both corner panels and every stats line "
                          "back on."));
    }

    ImGui::EndPopup();
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
    // E14 起贴**右下角**：左下角让给相机信息块（位置 / FOV / 书签）。
    // Blender 是左下、UE 是右下，两种都常见 —— 这里按 E14 的四角分工走。
    const glm::vec2 c(vpPos.x + vpSize.x - kGizmoMargin - half,
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
        ImGui::SetTooltip(TR("%s\nClick: snap to this view\nDrag: orbit view"),
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
    // Shift 层被 E14 的浮层快捷键占了（Shift+数字 = 视图模式，
    // Ctrl/Alt+Shift+数字 = 相机书签），所以这里直接让路。
    if (io.KeyShift) return;
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

// ---------------------------------------------------------------- E14 快捷键
//
// 数字键已经被两套既有快捷键占满（1/3/7(+Ctrl) 视图方向、Alt+数字 面板显隐），
// 所以 E14 的三件事全挂在 **Shift 层**：
//   Shift+1..8          切视图模式（等价于点左上角的模式条）
//   Ctrl+Shift+1..9     存相机书签到该槽
//   Alt+Shift+1..9      跳回该槽
// 相机书签不带位置：跳转只恢复 yaw / pitch（**保留当前注视点**）——
// 这样"看一眼侧面再回来"不会把镜头甩到别的地方去，比连位置一起还原好用。
void ViewportPanel::handleOverlayShortcuts() {
    if (!m_ctx.isEditing()) return;

    const ImGuiIO& io = ImGui::GetIO();
    if (!io.KeyShift) return;

    static const ImGuiKey kDigits[9] = {
        ImGuiKey_1, ImGuiKey_2, ImGuiKey_3, ImGuiKey_4, ImGuiKey_5,
        ImGuiKey_6, ImGuiKey_7, ImGuiKey_8, ImGuiKey_9};

    if (io.KeyCtrl) {
        // ---- 存书签 ----
        scene::Camera& cam = m_ctx.activeScene().camera();
        for (int i = 0; i < EditorContext::kBookmarkCount; ++i) {
            if (!ImGui::IsKeyPressed(kDigits[i], false)) continue;
            EditorContext::CameraBookmark& b = m_ctx.bookmarksRef()[i];
            b.set = true;
            b.pos = cam.position();
            b.yaw = cam.yaw();
            b.pitch = cam.pitch();
            m_ctx.setStatus(TR("Saved camera bookmark ") + std::to_string(i + 1));
            VK_LOG_INFO("VP-BOOK save slot=%d pos=%.3f,%.3f,%.3f yaw=%.4f "
                        "pitch=%.4f",
                        i + 1, static_cast<double>(b.pos.x),
                        static_cast<double>(b.pos.y),
                        static_cast<double>(b.pos.z),
                        static_cast<double>(b.yaw),
                        static_cast<double>(b.pitch));
        }
        return;
    }

    if (io.KeyAlt) {
        // ---- 跳书签 ----
        scene::Camera& cam = m_ctx.activeScene().camera();
        for (int i = 0; i < EditorContext::kBookmarkCount; ++i) {
            if (!ImGui::IsKeyPressed(kDigits[i], false)) continue;
            const EditorContext::CameraBookmark& b = m_ctx.bookmarksRef()[i];
            if (!b.set) {
                VK_LOG_INFO("VP-BOOK recall slot=%d (empty)", i + 1);
                continue;
            }
            const glm::vec3 keep = cam.target();
            cam.setYaw(b.yaw);
            cam.setPitch(b.pitch);
            cam.setTarget(keep);
            m_ctx.setStatus(TR("Camera bookmark ") + std::to_string(i + 1));
            VK_LOG_INFO("VP-BOOK recall slot=%d yaw=%.4f pitch=%.4f", i + 1,
                        static_cast<double>(b.yaw),
                        static_cast<double>(b.pitch));
        }
        return;
    }

    // ---- 切视图模式 ----
    const int n = render::Renderer::kViewModeCount;
    for (int i = 0; i < n && i < 9; ++i) {
        if (!ImGui::IsKeyPressed(kDigits[i], false)) continue;
        m_renderer.setViewMode(static_cast<render::Renderer::ViewMode>(i));
        VK_LOG_INFO("VP-MODEBAR key=%d mode=%s", i + 1,
                    render::Renderer::viewModeName(m_renderer.viewMode()));
    }
}

// 运行态极简浮层：只留"退出全屏"的入口 + 播放状态。
// **刻意不含任何导航控件** —— Play 期间相机归游戏管，编辑器不该提供会抢
// 输入的 UI；全屏时工具栏不画，所以这里必须留一个图形化的退出口（F / Esc 也行）。
void ViewportPanel::drawPlayHud(const glm::vec2& vpPos, const glm::vec2& vpSize) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float lineH = ImGui::GetTextLineHeightWithSpacing();
    const float frameH = ImGui::GetFrameHeightWithSpacing();

    // ⚠ 度量必须与绘制**同源**：写死英文串去量，一旦界面语言是中文，
    //   算出来的宽度就偏窄 → EndChild 里的文字被裁掉。这里用一个
    //   0 秒的探针串走同一套格式化，宽度自然跟着译文变。
    //   （踩过的坑：`PLAYING` 只是 `%s` 的实参，本身没被 TR 包 → 中文界面
    //   下这一行还是英文，而且是"看得见但翻不到"的那一类。）
    char clockProbe[64];
    std::snprintf(clockProbe, sizeof(clockProbe), TR("%s   t = %.2fs"),
                  TR("PLAYING"), 0.0);
    // 第二行提示（P8）：未接管时的引导 / 接管后的"怎么退出"。
    // 两句话都拿同一个探针量宽度，宽度跟着译文走（中文比英文短，用英文量
    // 一定偏宽，但反过来漏量就会被裁）。取长者。
    const char* hint = m_ctx.playMouseCaptured()
                           ? TRL("In control - Esc shows the mouse")
                           : TRL("Click the viewport to take control");
    float textW = ImGui::CalcTextSize(TRL("Restore view  (F)")).x;
    textW = std::max(textW, ImGui::CalcTextSize(clockProbe).x);
    textW = std::max(textW, ImGui::CalcTextSize(hint).x);
    const float cw = textW + st.WindowPadding.x * 2.0f + 8.0f;
    const float ch = frameH + lineH * 2.0f + st.WindowPadding.y * 2.0f;

    ImGui::SetCursorScreenPos(
        ImVec2(vpPos.x + vpSize.x - cw - 10.0f, vpPos.y + 10.0f));

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.07f, 0.08f, 0.10f, 0.80f));
    // 浅色主题：深底配浅字（否则那颗 Fullscreen 按钮的标签看不见）。
    // 深色主题不推 → 逐像素不变。
    const int phPush = ui::tokens().isLight ? 1 : 0;
    if (phPush) ImGui::PushStyleColor(ImGuiCol_Text, kOverlayText);
    ImGui::BeginChild("##play_hud", ImVec2(cw, ch), 0);

    if (ImGui::Button(m_ctx.gameFullscreen() ? TRL("Restore view  (F)")
                                             : TRL("Fullscreen  (F)"),
                      ImVec2(-1.0f, 0.0f))) {
        m_ctx.toggleGameFullscreen();
    }

    ImGui::TextColored(m_ctx.isPlaying() ? ImVec4(0.55f, 0.9f, 0.58f, 1.0f)
                                         : ImVec4(0.95f, 0.8f, 0.4f, 1.0f),
                       TR("%s   t = %.2fs"),
                       m_ctx.isPlaying() ? TR("PLAYING") : TR("PAUSED"),
                       static_cast<double>(m_ctx.playTime()));

    ImGui::TextDisabled("%s", hint);

    ImGui::EndChild();
    ImGui::PopStyleColor(1 + phPush);
}

} // namespace editor
