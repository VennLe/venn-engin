#include "GizmoController.h"

#include "DebugRects.h"
#include "EditorContext.h"
#include "Localization.h"
#include "MultiEditMath.h"

#include "core/Logger.h"
#include "ecs/Components.h"
#include "scene/Camera.h"
#include "scene/Prefab.h"
#include "scene/Scene.h"

#include <imgui.h>

#include <cmath>
#include <cstdio>
#include <string>

namespace editor {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kGizmoPixels = 90.0f;    // 手柄在屏幕上的目标长度
constexpr float kHitRadiusPx = 8.0f;     // 轴命中半径
constexpr float kRingHitPx = 9.0f;       // 旋转环命中半径
constexpr float kCenterHitPx = 10.0f;    // 中心手柄命中半径
// 平面方片在轴上的起止位置（相对手柄长度）
constexpr float kPlaneT0 = 0.30f;
constexpr float kPlaneT1 = 0.62f;
// 拖动平面时射线可能几乎与平面平行 → 交点跑到无穷远，位移会瞬间爆炸。
// 用一个宽松的上限兜底（正常拖拽远远到不了）。
constexpr float kMaxDragUnits = 2000.0f;

int axisOfHandle(Handle h) {
    switch (h) {
        case Handle::AxisX: return 0;
        case Handle::AxisY: return 1;
        case Handle::AxisZ: return 2;
        default: return -1;
    }
}

// 平面手柄 → 法线的轴下标
int planeNormalOfHandle(Handle h) {
    switch (h) {
        case Handle::PlaneYZ: return 0;
        case Handle::PlaneXZ: return 1;
        case Handle::PlaneXY: return 2;
        default: return -1;
    }
}

// 欧拉角（弧度）→ 旋转矩阵。
// 顺序必须与 TransformComponent::localMatrix 完全一致：R = Rx * Ry * Rz
glm::mat3 rotationFromEuler(const glm::vec3& e) {
    const glm::mat4 m = glm::rotate(glm::mat4(1.0f), e.x, glm::vec3(1, 0, 0)) *
                        glm::rotate(glm::mat4(1.0f), e.y, glm::vec3(0, 1, 0)) *
                        glm::rotate(glm::mat4(1.0f), e.z, glm::vec3(0, 0, 1));
    return glm::mat3(m);
}

// 旋转矩阵 → 欧拉角（R = Rx * Ry * Rz 的解析解，含万向锁分支）。
// glm::eulerAngles 用的是 YXZ 约定，与本引擎的 XYZ 不一致，不能直接用。
glm::vec3 eulerFromRotation(const glm::mat3& m) {
    // GLM 是列主序：r_ij（第 i 行第 j 列）= m[j][i]
    const float sy = glm::clamp(m[2][0], -1.0f, 1.0f);
    const float y = std::asin(sy);

    float x = 0.0f;
    float z = 0.0f;
    if (std::fabs(sy) < 0.99999f) {
        x = std::atan2(-m[2][1], m[2][2]);
        z = std::atan2(-m[1][0], m[0][0]);
    } else {
        // 万向锁：y = ±90°，x/z 只有一个自由度，全部归到 x
        x = std::atan2(m[1][2], m[1][1]);
        z = 0.0f;
    }
    return {x, y, z};
}

void planeBasis(const glm::vec3& n, glm::vec3& u, glm::vec3& v) {
    const glm::vec3 nn = glm::normalize(n);
    // 参考轴选"与法线夹角最大的世界轴"（Z-up：先试 +Z，不行换 +X）
    const glm::vec3 ref =
        std::fabs(nn.z) < 0.9f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0);
    u = glm::normalize(glm::cross(ref, nn));
    v = glm::cross(nn, u);
}

// 射线与"过 center、法线 n"的平面的交点，转成平面内的方位角
bool ringAngle(const Ray& r, const glm::vec3& center, const glm::vec3& n,
               float& outAngle) {
    float t = 0.0f;
    if (!PickingSystem::intersectPlane(r, center, n, t)) return false;
    if (t <= 0.0f) return false;  // 交点在身后

    const glm::vec3 hit = r.origin + r.dir * t;
    glm::vec3 u, v;
    planeBasis(n, u, v);
    const glm::vec3 d = hit - center;
    outAngle = std::atan2(glm::dot(d, v), glm::dot(d, u));
    return true;
}

// 射线与平面的交点（过 point、法线 n）
bool planeHit(const Ray& r, const glm::vec3& point, const glm::vec3& n,
              glm::vec3& out) {
    float t = 0.0f;
    if (!PickingSystem::intersectPlane(r, point, n, t)) return false;
    if (t <= 0.0f) return false;
    out = r.origin + r.dir * t;
    return true;
}

// 拖拽方向：世界空间传单位轴；局部空间传实体旋转矩阵的列。
void extractBasisXform(const glm::mat4& wm, bool local, glm::vec3 out[3]) {
    if (!local) {
        out[0] = glm::vec3(1, 0, 0);
        out[1] = glm::vec3(0, 1, 0);
        out[2] = glm::vec3(0, 0, 1);
        return;
    }
    for (int i = 0; i < 3; ++i) {
        glm::vec3 a(wm[i]);
        const float len = glm::length(a);
        out[i] = len > 1e-6f ? a / len : glm::vec3(i == 0, i == 1, i == 2);
    }
}

// 屏幕点是否落在四边形里（四个角可以是任意凸四边形）。
// 用"所有叉积同号"判断 —— 比逐边求交简单，也不会被射线方向搞晕。
bool pointInQuad(const glm::vec2& p, const glm::vec2 q[4]) {
    float sign = 0.0f;
    for (int i = 0; i < 4; ++i) {
        const glm::vec2& a = q[i];
        const glm::vec2& b = q[(i + 1) % 4];
        const float cross = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
        if (std::fabs(cross) < 1e-4f) continue;
        if (sign == 0.0f) {
            sign = cross;
        } else if ((cross > 0.0f) != (sign > 0.0f)) {
            return false;
        }
    }
    return true;
}

// arcball：把鼠标相对手柄中心的屏幕偏移映射到单位虚拟球面上。
// 球外（偏移比半径还长）按标准做法投影到球赤道上的对应点。
glm::vec3 arcballVec(float dx, float dy, float radius) {
    float x = dx / radius;
    float y = -dy / radius;   // 屏幕 y 向下，球面 y 向上
    const float len2 = x * x + y * y;
    float z = 0.0f;
    if (len2 <= 1.0f) {
        z = std::sqrt(1.0f - len2);
    } else {
        const float inv = 1.0f / std::sqrt(len2);
        x *= inv;
        y *= inv;
    }
    return glm::vec3(x, y, z);
}

} // namespace

const char* GizmoController::handleName() const {
    switch (m_dragHandles) {
        case Handle::AxisX: return "X";
        case Handle::AxisY: return "Y";
        case Handle::AxisZ: return "Z";
        case Handle::PlaneYZ: return "YZ";
        case Handle::PlaneXZ: return "XZ";
        case Handle::PlaneXY: return "XY";
        case Handle::Screen: return "screen";
        default: return "-";
    }
}

const char* GizmoController::modeName() const {
    switch (m_mode) {
        case GizmoMode::Translate: return "Move";
        case GizmoMode::Rotate: return "Rotate";
        case GizmoMode::Scale: return "Scale";
        case GizmoMode::Select: return "Select";
    }
    return "?";
}

void GizmoController::planeBasisForHandles(int normalIdx, glm::vec3& u,
                                          glm::vec3& v) const {
    // 平面里不包含法线那一根，所以基向量就是另外两根轴
    static const int kIdx[3][2] = {{1, 2}, {0, 2}, {0, 1}};
    u = m_axisDir[kIdx[normalIdx][0]];
    v = m_axisDir[kIdx[normalIdx][1]];
}

// ---------------------------------------------------------------- 绘制

void GizmoController::drawGizmo(EditorContext& ctx, ImDrawList* draw,
                                Handle highlight) const {
    if (!draw) return;

    const ImU32 colAxis[3] = {
        IM_COL32(228, 78, 78, 255),   // X 红
        IM_COL32(118, 208, 84, 255),  // Y 绿
        IM_COL32(80, 132, 238, 255),  // Z 蓝
    };
    const ImU32 colHot = IM_COL32(252, 218, 62, 255);
    const ImU32 colWhite = IM_COL32(236, 240, 246, 255);

    const ImVec2 o(m_originScreen.x, m_originScreen.y);
    const GizmoMode mode = ctx.gizmoMode();

    // 自动化探针：手柄中心 + 三根轴的端点。
    // 脚本靠这些矩形精确点中"哪一根轴"，而不是在截图里猜坐标 ——
    // 轴的颜色/位置会随相机变，写死坐标必然踩坑（见 DebugRects.h 的说明）。
    logRect("GZ-ORIGIN", ImVec2(o.x - 5.0f, o.y - 5.0f),
            ImVec2(o.x + 5.0f, o.y + 5.0f));
    if (std::getenv("MYVK_LOG_RECTS")) {
        // 相机状态跟手柄一起打（一次）：脚本断言"相机对着哪"时用，
        // 也让"投影跑到屏幕外"这类问题一眼看出是相机被谁动了。
        static const scene::Camera* lastCam = nullptr;
        static glm::vec3 lastPos(0.0f);
        const scene::Camera& dbgCam = ctx.activeScene().camera();
        if (lastCam != &dbgCam ||
            glm::distance(lastPos, dbgCam.position()) > 0.01f) {
            lastCam = &dbgCam;
            lastPos = dbgCam.position();
            VK_LOG_INFO("GZ-CAM pos=(%.2f,%.2f,%.2f) target=(%.2f,%.2f,%.2f) "
                        "yaw=%.3f pitch=%.3f dist=%.2f",
                        static_cast<double>(dbgCam.position().x),
                        static_cast<double>(dbgCam.position().y),
                        static_cast<double>(dbgCam.position().z),
                        static_cast<double>(dbgCam.target().x),
                        static_cast<double>(dbgCam.target().y),
                        static_cast<double>(dbgCam.target().z),
                        static_cast<double>(dbgCam.yaw()),
                        static_cast<double>(dbgCam.pitch()),
                        static_cast<double>(dbgCam.distance()));
        }
    }

    const auto axisColor = [&](int i) {
        return (i == static_cast<int>(highlight)) ? colHot : colAxis[i];
    };
    const auto planeHot = [&](int i) {
        return highlight == static_cast<Handle>(static_cast<int>(Handle::PlaneYZ) + i);
    };
    const bool screenHot = (highlight == Handle::Screen);

    // ---- 中心手柄（整体）：三种模式各画一个形状 ----
    if (mode == GizmoMode::Translate) {
        // 移动：中心一个实心小方块（视平面内自由拖动）
        const float h = 7.0f;
        draw->AddRectFilled(ImVec2(o.x - h, o.y - h), ImVec2(o.x + h, o.y + h),
                            screenHot ? colHot : IM_COL32(226, 230, 236, 235),
                            2.0f);
    } else if (mode == GizmoMode::Scale) {
        // 缩放：中心一个带描边的方块 = 等比缩放（和单轴上的小方块区分开）
        const float h = 9.0f;
        draw->AddRectFilled(ImVec2(o.x - h, o.y - h), ImVec2(o.x + h, o.y + h),
                            screenHot ? IM_COL32(252, 218, 62, 220)
                                      : IM_COL32(226, 230, 236, 200),
                            3.0f);
        draw->AddRect(ImVec2(o.x - h, o.y - h), ImVec2(o.x + h, o.y + h),
                      IM_COL32(40, 44, 50, 220), 3.0f, 0, 1.5f);
    }

    switch (mode) {
        case GizmoMode::Translate:
        case GizmoMode::Scale: {
            // ---- 平面方片（双轴）----
            static const char* kPlaneTag[3] = {"GZ-PLANE-YZ", "GZ-PLANE-XZ",
                                               "GZ-PLANE-XY"};
            for (int i = 0; i < 3; ++i) {
                if (!m_planeValid[i]) continue;
                ImVec2 pts[4];
                for (int k = 0; k < 4; ++k) {
                    pts[k] = ImVec2(m_planePts[i][k].x, m_planePts[i][k].y);
                }
                // 自动化探针：方片中心（凸四边形的重心一定在内部，点它必中）
                {
                    const ImVec2 c((pts[0].x + pts[1].x + pts[2].x + pts[3].x) *
                                       0.25f,
                                   (pts[0].y + pts[1].y + pts[2].y + pts[3].y) *
                                       0.25f);
                    logRect(kPlaneTag[i], ImVec2(c.x - 5.0f, c.y - 5.0f),
                            ImVec2(c.x + 5.0f, c.y + 5.0f));
                }
                const bool hot = planeHot(i);
                // 填充半亮、悬停时更亮；描边用满色 → 即使被 3D 画面盖住也看得清
                const int a = hot ? 190 : 90;
                const ImU32 base = axisColor(i);
                const ImU32 fill = (base & 0x00FFFFFFu) |
                                   (static_cast<ImU32>(a) << 24);
                draw->AddConvexPolyFilled(pts, 4, fill);
                draw->AddPolyline(pts, 4, base, ImDrawFlags_Closed,
                                  hot ? 2.5f : 1.5f);
            }

            // ---- 单轴 ----
            static const char* kAxisTag[3] = {"GZ-AXIS-X", "GZ-AXIS-Y",
                                              "GZ-AXIS-Z"};
            for (int i = 0; i < 3; ++i) {
                glm::vec2 tip;
                if (!PickingSystem::worldToScreen(
                        m_viewProj, m_viewportPos, m_viewportSize,
                        m_origin + m_axisDir[i] * m_worldLen, tip)) {
                    continue;
                }
                // 轴端的可点区域（和绘制出来的小方块/箭头同一位置）
                logRect(kAxisTag[i], ImVec2(tip.x - 7.0f, tip.y - 7.0f),
                        ImVec2(tip.x + 7.0f, tip.y + 7.0f));
                const bool hot = (highlight == static_cast<Handle>(i));
                const ImU32 c = axisColor(i);
                const float th = hot ? 4.0f : 2.5f;
                draw->AddLine(o, ImVec2(tip.x, tip.y), c, th);

                if (mode == GizmoMode::Translate) {
                    // 箭头：沿投影方向的一个小三角
                    const glm::vec2 dir = glm::normalize(tip - m_originScreen);
                    const glm::vec2 n(-dir.y, dir.x);
                    const float hl = 14.0f;
                    const float hw = 6.0f;
                    draw->AddTriangleFilled(
                        ImVec2(tip.x, tip.y),
                        ImVec2(tip.x - dir.x * hl + n.x * hw,
                               tip.y - dir.y * hl + n.y * hw),
                        ImVec2(tip.x - dir.x * hl - n.x * hw,
                               tip.y - dir.y * hl - n.y * hw),
                        c);
                } else {
                    // 缩放：轴端一个小方块
                    const float hs = hot ? 7.0f : 5.0f;
                    draw->AddRectFilled(ImVec2(tip.x - hs, tip.y - hs),
                                        ImVec2(tip.x + hs, tip.y + hs), c);
                }
            }
            break;
        }
        case GizmoMode::Rotate: {
            // ---- 三根轴的旋转环 ----
            // 自动化探针：环上**离中心最远**的那一点。
            // 旋转模式没有 GZ-AXIS-*（轴端被环取代了），脚本就没有可点的
            // 锚点；取"离中心最远"而不是固定第 0 个点，是因为环是投影成
            // 椭圆的 —— 离中心最远处离别的环最远，点它最不容易抓错。
            static const char* kRingTag[3] = {"GZ-RING-X", "GZ-RING-Y",
                                              "GZ-RING-Z"};
            for (int i = 0; i < 3; ++i) {
                if (!m_ringValid[i]) continue;
                const ImU32 c = axisColor(i);
                const float th = (highlight == static_cast<Handle>(i)) ? 4.0f
                                                                      : 2.5f;
                ImVec2 pts[kRingSegments];
                float farD = -1.0f;
                glm::vec2 farPt = m_originScreen;
                for (int k = 0; k < kRingSegments; ++k) {
                    pts[k] = ImVec2(m_ringPoints[i][k].x, m_ringPoints[i][k].y);
                    const float d = glm::length(m_ringPoints[i][k] -
                                                m_originScreen);
                    if (d > farD) {
                        farD = d;
                        farPt = m_ringPoints[i][k];
                    }
                }
                logRect(kRingTag[i], ImVec2(farPt.x - 6.0f, farPt.y - 6.0f),
                        ImVec2(farPt.x + 6.0f, farPt.y + 6.0f));
                // 闭合环：把最后一段单独补上
                draw->AddPolyline(pts, kRingSegments, c, 0, th);
                draw->AddLine(pts[kRingSegments - 1], pts[0], c, th);
            }

            // ---- 外圈：自由旋转（绕视轴 / arcball）----
            if (m_viewRingValid) {
                const ImU32 c = screenHot ? colHot : colWhite;
                const float th = screenHot ? 3.5f : 2.0f;
                ImVec2 pts[kRingSegments];
                for (int k = 0; k < kRingSegments; ++k) {
                    pts[k] = ImVec2(m_viewRing[k].x, m_viewRing[k].y);
                }
                draw->AddPolyline(pts, kRingSegments, c, 0, th);
                draw->AddLine(pts[kRingSegments - 1], pts[0], c, th);
            }
            break;
        }
    }
}

// ---------------------------------------------------------------- 命中

Handle GizmoController::hitTest(const glm::vec2& mouse) const {
    if (m_mode == GizmoMode::Rotate) {
        // 先判三根轴环，再判外圈：正交视图下"视轴环"会和某根轴环完全重合，
        // 这时应该优先给**具体的那根轴**，而不是模棱两可的自由旋转。
        Handle best = Handle::None;
        float bestDist = 1e30f;
        for (int i = 0; i < 3; ++i) {
            if (!m_ringValid[i]) continue;
            for (int k = 0; k < kRingSegments; ++k) {
                const glm::vec2& a = m_ringPoints[i][k];
                const glm::vec2& b = m_ringPoints[i][(k + 1) % kRingSegments];
                const float d =
                    PickingSystem::screenDistanceToSegment(mouse, a, b);
                if (d < kRingHitPx && d < bestDist) {
                    bestDist = d;
                    best = static_cast<Handle>(i);
                }
            }
        }
        if (best != Handle::None) return best;

        if (m_viewRingValid) {
            for (int k = 0; k < kRingSegments; ++k) {
                const glm::vec2& a = m_viewRing[k];
                const glm::vec2& b = m_viewRing[(k + 1) % kRingSegments];
                if (PickingSystem::screenDistanceToSegment(mouse, a, b) <
                    kRingHitPx) {
                    return Handle::Screen;
                }
            }
        }
        return Handle::None;
    }

    // ---- 中心手柄（整体）----
    if (glm::length(mouse - m_originScreen) < kCenterHitPx) return Handle::Screen;

    // ---- 平面方片（双轴）----
    for (int i = 0; i < 3; ++i) {
        if (!m_planeValid[i]) continue;
        if (pointInQuad(mouse, m_planePts[i])) {
            return static_cast<Handle>(static_cast<int>(Handle::PlaneYZ) + i);
        }
    }

    // ---- 单轴 ----
    Handle best = Handle::None;
    float bestDist = 1e30f;
    for (int i = 0; i < 3; ++i) {
        glm::vec2 tip;
        if (!PickingSystem::worldToScreen(
                m_viewProj, m_viewportPos, m_viewportSize,
                m_origin + m_axisDir[i] * m_worldLen, tip)) {
            continue;
        }
        const float d =
            PickingSystem::screenDistanceToSegment(mouse, m_originScreen, tip);
        if (d < kHitRadiusPx && d < bestDist) {
            bestDist = d;
            best = static_cast<Handle>(i);
        }
    }
    return best;
}

// ---------------------------------------------------------------- 拖拽

GizmoController::TrsPointers GizmoController::trsOf(scene::Scene& scene,
                                                    ecs::Entity e,
                                                    bool onCollider) {
    TrsPointers p;
    if (onCollider) {
        auto* cc = scene.world().get<ecs::CollisionComponent>(e);
        if (!cc) return p;
        p.pos = &cc->position;
        p.rot = &cc->rotation;
        p.scale = &cc->scale;
        return p;
    }
    auto* t = scene.world().get<ecs::TransformComponent>(e);
    if (!t) return p;
    p.pos = &t->position;
    p.rot = &t->rotation;
    p.scale = &t->scale;
    return p;
}

glm::mat4 GizmoController::targetWorld(scene::Scene& scene,
                                       ecs::Entity e) const {
    glm::mat4 m = scene.worldMatrix(e);
    if (m_onCollider) {
        if (const auto* cc = scene.world().get<ecs::CollisionComponent>(e))
            m = m * cc->localMatrix();
    }
    return m;
}

void GizmoController::beginDrag(EditorContext& ctx, scene::Scene& scene,
                                ecs::Entity e, const Ray& ray,
                                const glm::vec2& mouse) {
    const TrsPointers tp = trsOf(scene, e, m_onCollider);
    if (!tp.valid()) return;

    m_entity = e;
    m_mode = ctx.gizmoMode();
    m_worldSpace = (ctx.gizmoSpace() == GizmoSpace::World);
    m_before = TransformSnapshot{*tp.pos, *tp.rot, *tp.scale};
    m_startPos = *tp.pos;
    m_startRot = *tp.rot;
    m_startScale = *tp.scale;
    m_startMouse = mouse;
    m_changed = false;
    m_dragging = true;
    m_dragHandles = m_hover;

    // ---- 目标表（多选，2026-10-27 #316）----
    //
    // 三条过滤，每条都对应一个具体的"否则会出怪现象"：
    //   ① 没有 Transform 的跳过 —— 没东西可写。
    //   ② 上锁的跳过（自带地面）—— 它在视口里连点都点不到，批量拖拽把它
    //      一起挪走属于"我没碰它它却动了"。
    //   ③ **祖先也在集合里的跳过** —— 父节点动，子节点跟着动（Hierarchy
    //      已经保证了这一点）。如果子节点也进目标表，它会**动两次**：
    //      一次被父节点带着，一次被自己写进去。这个 bug 的现象是"批量拖
    //      带层级的一堆物体时，子物体飞得比父物体远"，很难联想到是这里。
    m_targets.clear();
    if (!m_onCollider) {
        const std::vector<ecs::Entity>& sel = ctx.selectionSet();
        auto hasSelectedAncestor = [&](ecs::Entity x) {
            const ecs::World& w = scene.world();
            ecs::Entity cur = x;
            for (int depth = 0; depth < 64; ++depth) {
                const auto* h = w.get<ecs::HierarchyComponent>(cur);
                if (!h || h->parent == ecs::kInvalidEntity) return false;
                cur = w.handle(h->parent);
                if (!cur.valid()) return false;
                for (ecs::Entity s : sel)
                    if (s.id == cur.id) return true;
            }
            return false;
        };
        for (ecs::Entity s : sel) {
            if (!scene.world().alive(s)) continue;
            if (!scene.world().has<ecs::TransformComponent>(s)) continue;
            if (scene.world().has<ecs::LockedComponent>(s)) continue;
            if (s.id != e.id && hasSelectedAncestor(s)) continue;

            Target t;
            t.entity = s;
            t.before = TransformSnapshot::capture(scene, s);
            const glm::mat4 wm = scene.worldMatrix(s);
            t.startWorldPos = glm::vec3(wm[3]);
            extractBasisXform(wm, /*local=*/true, t.localAxes);
            if (const auto* h = scene.world().get<ecs::HierarchyComponent>(s)) {
                if (h->parent != ecs::kInvalidEntity) {
                    const ecs::Entity pe = scene.world().handle(h->parent);
                    if (pe.valid()) {
                        t.parentWorld = scene.worldMatrix(pe);
                        t.hasParent = true;
                    }
                }
            }
            m_targets.push_back(t);
        }
        // 主选中项必须排在第一个：手柄的轴、读数、撤销后的选中都以它为准
        // （集合里是 Ctrl 点击顺序，主选中项不一定是第一个）。
        for (std::size_t i = 1; i < m_targets.size(); ++i) {
            if (m_targets[i].entity.id == e.id) {
                std::swap(m_targets[0], m_targets[i]);
                break;
            }
        }
    }
    if (m_targets.empty()) {
        // 集合为空 / 参选的都被过滤掉了 → 退化成"只动主选中项"。
        Target t;
        t.entity = e;
        t.before = m_before;
        const glm::mat4 wm = targetWorld(scene, e);
        t.startWorldPos = glm::vec3(wm[3]);
        extractBasisXform(wm, /*local=*/true, t.localAxes);
        m_targets.push_back(t);
    }

    // ---- 冻结参考线（本次拖拽全程只认这份快照）----
    // 为什么不能直接用每帧更新的 m_origin：物体自己走过的位移会在下一帧
    // 被重新算进"鼠标沿轴的参数"里 → D_new = Δ + D_old，正反馈，越拖越快。
    m_dragOrigin = m_origin;
    m_dragWorldLen = m_worldLen;
    m_dragViewNormal = m_viewNormal;
    for (int i = 0; i < 3; ++i) m_dragLocalAxes[i] = m_localAxisWorld[i];

    const int axis = axisOfHandle(m_dragHandles);
    const int planeN = planeNormalOfHandle(m_dragHandles);

    if (axis >= 0) {
        m_dragAxis = m_axisDir[axis];
    } else if (planeN >= 0) {
        m_dragAxis = m_axisDir[planeN];   // 平面法线
        planeBasisForHandles(planeN, m_dragU, m_dragV);
    } else if (m_dragHandles == Handle::Screen) {
        m_dragAxis = m_viewNormal;
        m_dragU = m_camRight;
        m_dragV = m_camUp;
    }

    if (m_mode == GizmoMode::Rotate) {
        if (m_dragHandles == Handle::Screen) {
            // 自由旋转走 arcball，只需要记住按下的屏幕位置（已存 m_startMouse）
        } else {
            float a = 0.0f;
            m_startAngle =
                ringAngle(ray, m_dragOrigin, m_dragAxis, a) ? a : 0.0f;
        }
    } else if (axis >= 0) {
        float p = 0.0f;
        m_startParam =
            PickingSystem::closestOnAxis(ray, m_dragOrigin, m_dragAxis, p)
                ? p
                : 0.0f;
    } else {
        glm::vec3 hit(0.0f);
        if (planeHit(ray, m_dragOrigin, m_dragAxis, hit)) m_startHit = hit;
    }

    // 自动化探针：把"抓到了哪根手柄、对应世界哪个方向"落成一行日志。
    // 这是排查"拖 Y 却在 Z 方向变形"这类问题的唯一可靠依据 ——
    // 界面上的读数只能证明代码自认为抓到了什么，证明不了它抓得对不对。
    // 后面三个局部轴也一起打：断言"物体沿拖拽方向长大"需要它
    // （缩放作用在局部轴上，判断实际长大方向必须把两者结合起来）。
    // target= 追加在行尾 —— 日志是自动化的"接口"，verify_gizmo_scale.py
    // 的正则锁定了 space= 后紧跟 axis= 的旧次序，插中间会打断它。
    VK_LOG_INFO("gizmo begin: handle=%s mode=%s space=%s "
                "axis=(%.3f,%.3f,%.3f) localX=(%.3f,%.3f,%.3f) "
                "localY=(%.3f,%.3f,%.3f) localZ=(%.3f,%.3f,%.3f) "
                "target=%s",
                handleName(), modeName(), m_worldSpace ? "world" : "local",
                static_cast<double>(m_dragAxis.x),
                static_cast<double>(m_dragAxis.y),
                static_cast<double>(m_dragAxis.z),
                static_cast<double>(m_dragLocalAxes[0].x),
                static_cast<double>(m_dragLocalAxes[0].y),
                static_cast<double>(m_dragLocalAxes[0].z),
                static_cast<double>(m_dragLocalAxes[1].x),
                static_cast<double>(m_dragLocalAxes[1].y),
                static_cast<double>(m_dragLocalAxes[1].z),
                static_cast<double>(m_dragLocalAxes[2].x),
                static_cast<double>(m_dragLocalAxes[2].y),
                static_cast<double>(m_dragLocalAxes[2].z),
                targetName());
}

// ---------------------------------------------------------------- 多选分发
//
// 三个 apply*ToTargets 的分工：**数学在这里，读写在这里，也算"写给谁"**。
// applyDrag 只负责算出"这一次拖了多少"（世界位移 / 统一倍率 / 旋转增量），
// 然后交给这里发到 1~N 个目标上。
//
// ⚠ 单选（含碰撞体编辑）那条分支是**原样搬过来的旧代码**，一个字都没改：
//   多选是新功能，不应该让既有行为产生哪怕 1e-7 的偏移（像素级回归对比
//   就靠这个）。多选走的是下面那个循环。

namespace {

// 世界位移 → 父空间位移：把父链的旋转/缩放抵消掉
glm::vec3 toLocalDelta(const glm::mat4& parentWorld, const glm::vec3& worldDelta) {
    const glm::mat3 pm(parentWorld);
    const float det = glm::determinant(pm);
    if (std::fabs(det) <= 1e-8f) return worldDelta;
    return glm::inverse(pm) * worldDelta;
}

} // namespace

void GizmoController::applyTranslateToTargets(scene::Scene& scene,
                                             const TrsPointers& tp,
                                             const glm::vec3& worldDelta) {
    if (m_targets.size() <= 1) {
        glm::vec3 localDelta = worldDelta;
        if (const auto* h =
                scene.world().get<ecs::HierarchyComponent>(m_entity)) {
            if (h->parent != ecs::kInvalidEntity) {
                const ecs::Entity pe = scene.world().handle(h->parent);
                localDelta = toLocalDelta(scene.worldMatrix(pe), worldDelta);
            }
        }
        *tp.pos = m_startPos + localDelta;
        return;
    }

    for (const Target& t : m_targets) {
        auto* tt = scene.world().get<ecs::TransformComponent>(t.entity);
        if (!tt) continue;
        // 每个目标用自己的父矩阵换算：多选里有的在根下、有的挂在一个
        // 转过 90° 的父节点下，用同一个局部位移会让它们朝不同方向走。
        const glm::vec3 d =
            t.hasParent ? toLocalDelta(t.parentWorld, worldDelta) : worldDelta;
        tt->position = t.before.position + d;
    }
}

// ---- 拖拽方向 → 局部轴的缩放权重 ----
//
// 这里是用户报的"拖 Y 却在 Z 方向拉伸"的根因所在。
//
// 手柄拖的是**世界**里的某一根轴（世界空间下就是世界轴本身），
// 而 TransformComponent::scale 是**局部**的（M = T·R·S）——
// 它作用在物体自己的三根轴上。物体一转，两套轴就不再重合：
//   物体绕 X 转 +90° 后，它的局部 Y 正指着世界的 Z。
//   于是"拖绿轴（世界 Y）"写进 scale.y，物体却沿着世界 Z 长；
//   "拖蓝轴（世界 Z）"写进 scale.z=局部 Z=世界 -Y，又朝着 Y 长。
// 表现就是两根轴"换了位置"。
//
// 正确做法：把拖拽方向投影到物体的三根**局部**轴上，按投影强度
// 分配缩放量。轴对齐时（绝大多数情况、也是旧代码唯一做对的情况）
// 投影恰好是 0/1，公式退化成"只改对得上那一维"，与旧行为一致；
// 物体转过之后则自动落到真正对得上的那一维上。
//
// ⚠ 多选时**每个目标各算一遍权重**（这是多选唯一必须重算的东西）：
//   一批物体朝向不同，同一次"沿世界 X 拖"对各目标意味着不同的局部维。
//   用主选中项的权重套到别人身上，转过 90° 的那个会在**另一个方向**上
//   变胖 —— 而且它变完还是"物体没动、形状变了"，很像拾取错了目标。
void GizmoController::applyScaleToTargets(scene::Scene& scene,
                                          const TrsPointers& tp, float factor,
                                          int axisIdx, int planeN) {
    // 权重：axisIdx >= 0 是单轴，planeN >= 0 是平面（两根腿加一起），
    // 都不是 = 屏幕手柄（等比，与朝向无关）。
    const auto weightsFor = [&](const glm::vec3 axesLocal[3], glm::vec3& w) {
        w = glm::vec3(0.0f);
        if (axisIdx >= 0) {
            for (int i = 0; i < 3; ++i)
                w[i] = std::fabs(glm::dot(m_dragAxis, axesLocal[i]));
        } else if (planeN >= 0) {
            for (int i = 0; i < 3; ++i)
                w[i] = std::fabs(glm::dot(m_dragU, axesLocal[i])) +
                       std::fabs(glm::dot(m_dragV, axesLocal[i]));
        }
    };

    if (m_targets.size() <= 1) {
        glm::vec3 sc = m_startScale;
        if (axisIdx >= 0 || planeN >= 0) {
            glm::vec3 w;
            weightsFor(m_dragLocalAxes, w);
            for (int i = 0; i < 3; ++i)
                sc[i] = m_startScale[i] * (1.0f + (factor - 1.0f) * w[i]);
        } else {
            sc = m_startScale * factor;   // 整体等比：与朝向无关
        }
        *tp.scale = sc;
        return;
    }

    for (const Target& t : m_targets) {
        auto* tt = scene.world().get<ecs::TransformComponent>(t.entity);
        if (!tt) continue;
        if (axisIdx < 0 && planeN < 0) {
            tt->scale = t.before.scale * factor;
            continue;
        }
        glm::vec3 w;
        weightsFor(t.localAxes, w);
        for (int i = 0; i < 3; ++i)
            tt->scale[i] = t.before.scale[i] * (1.0f + (factor - 1.0f) * w[i]);
    }
}

// ---- 旋转 ----
//
// 单选：与引入多选之前逐位一致（起始姿态 * 增量）。
// 多选：**绕支点转** —— 位置和朝向一起转。
//   只转朝向不转位置的后果是"每个物体在各自原地转身"，彼此的相对布局
//   一点没变，用户看到的是"转了，但看起来像没转"（如果物体是球体，那
//   就是完全没变化）。UE / Blender 都是绕支点。
//
// ⚠ 缩放**不**绕支点（与旋转不同）。两件事的不对称是有意的：
//   旋转只有**一个**矩阵 D，所有目标共用它，绕支点的位置换算无歧义；
//   缩放按"每目标自己的局部轴"分配权重，一批朝向不同的物体根本没有
//   共同的缩放基 —— 硬凑一个只会得到"物体一边各自变形、一边彼此漂开"。
//   所以多选缩放是"各自绕自己中心变大变小"，位置不动。这条写进验收。
void GizmoController::applyRotateToTargets(scene::Scene& scene,
                                           const TrsPointers& tp,
                                           const glm::mat3& D, bool worldApply) {
    if (m_targets.size() <= 1) {
        const glm::mat3 R0 = rotationFromEuler(m_startRot);
        *tp.rot = eulerFromRotation(worldApply ? (D * R0) : (R0 * D));
        return;
    }

    const glm::vec3 pivot = m_dragOrigin;   // = 多选时的包围盒中心（见 update）
    for (const Target& t : m_targets) {
        auto* tt = scene.world().get<ecs::TransformComponent>(t.entity);
        if (!tt) continue;
        const glm::mat3 R0 = rotationFromEuler(t.before.rotation);
        tt->rotation = eulerFromRotation(worldApply ? (D * R0) : (R0 * D));

        // 位置绕支点转：世界 → 父空间的换算用**按下时冻结**的父矩阵。
        const glm::vec3 wp = orbitPosition(t.startWorldPos, pivot, D);
        if (t.hasParent) {
            const glm::mat4 inv = glm::inverse(t.parentWorld);
            tt->position = glm::vec3(inv * glm::vec4(wp, 1.0f));
        } else {
            tt->position = wp;
        }
    }
}

void GizmoController::applyDrag(EditorContext& ctx, scene::Scene& scene,
                               const Ray& ray, const glm::vec2& mouse) {
    if (m_dragHandles == Handle::None) return;

    const TrsPointers tp = trsOf(scene, m_entity, m_onCollider);
    if (!tp.valid()) return;

    // 全程用**拖拽开始时冻结**的轴心 / 轴向 / 手柄长度 —— 见文件头的说明。
    const glm::vec3 axis = m_dragAxis;
    const int axisIdx = axisOfHandle(m_dragHandles);
    const int planeN = planeNormalOfHandle(m_dragHandles);
    const bool screenHandle = (m_dragHandles == Handle::Screen);

    switch (m_mode) {
        case GizmoMode::Translate: {
            glm::vec3 worldDelta(0.0f);
            float readoutA = 0.0f;
            float readoutB = 0.0f;

            const float step = ctx.snapMoveStep();
            const bool snap = ctx.snapMove() && step > 1e-4f;
            const auto snapLen = [&](float v) {
                return snap ? std::round(v / step) * step : v;
            };

            if (axisIdx >= 0) {
                float p = 0.0f;
                if (!PickingSystem::closestOnAxis(ray, m_dragOrigin, axis, p))
                    return;
                readoutA = snapLen(p - m_startParam);
                worldDelta = axis * readoutA;
            } else {
                // 平面 / 屏幕：射线与平面求交，交点位移投到平面基向量上
                glm::vec3 hit(0.0f);
                if (!planeHit(ray, m_dragOrigin, axis, hit)) return;
                const glm::vec3 d = hit - m_startHit;
                readoutA = snapLen(glm::dot(d, m_dragU));
                readoutB = snapLen(glm::dot(d, m_dragV));
                if (std::fabs(readoutA) > kMaxDragUnits ||
                    std::fabs(readoutB) > kMaxDragUnits) {
                    return;   // 射线几乎与平面平行，这一帧的数据不可信
                }
                worldDelta = m_dragU * readoutA + m_dragV * readoutB;
            }

            // 世界位移 → 局部位移与"写给谁"都在这里（多选时逐个目标各算一遍）
            applyTranslateToTargets(scene, tp, worldDelta);
            m_changed = true;

            char buf[128];
            if (axisIdx >= 0) {
                std::snprintf(buf, sizeof(buf), TR("Move %s: %+.3f m%s"), TR(handleName()), static_cast<double>(readoutA),
                              snap ? TR("  [snap]") : "");
            } else {
                std::snprintf(buf, sizeof(buf),
                              TR("Move %s: %+.3f / %+.3f m%s"), TR(handleName()),
                              static_cast<double>(readoutA),
                              static_cast<double>(readoutB),
                              snap ? TR("  [snap]") : "");
            }
            m_readout = buf;
            break;
        }

        case GizmoMode::Scale: {
            float factor = 1.0f;

            if (axisIdx >= 0) {
                float p = 0.0f;
                if (!PickingSystem::closestOnAxis(ray, m_dragOrigin, axis, p))
                    return;
                factor = 1.0f + (p - m_startParam) /
                                    glm::max(m_dragWorldLen, 1e-3f);
            } else if (planeN >= 0) {
                glm::vec3 hit(0.0f);
                if (!planeHit(ray, m_dragOrigin, axis, hit)) return;
                const glm::vec3 d = hit - m_startHit;
                const float su = glm::dot(d, m_dragU);
                const float sv = glm::dot(d, m_dragV);
                if (std::fabs(su) > kMaxDragUnits ||
                    std::fabs(sv) > kMaxDragUnits) {
                    return;
                }
                // 两条腿一起走 → 取平均，于是"沿对角线往外拖"就是放大
                factor = 1.0f +
                         (su + sv) * 0.5f / glm::max(m_dragWorldLen, 1e-3f);
            } else {
                // 屏幕（整体）：离手柄中心越远越大，纯屏幕量
                const float d0 =
                    glm::length(m_startMouse - m_originScreen);
                const float d1 = glm::length(mouse - m_originScreen);
                factor = 1.0f + (d1 - d0) / kGizmoPixels;
            }

            // 吸附：量化的是**倍率**（拖动手柄一个"手柄长度"= ×2），
            // 而不是缩放绝对值 —— 后者会让第一下拖拽从 0.37 之类的地方
            // 直接跳到 0.4，看起来像物体抽搐了一下。
            const float sstep = ctx.snapScaleStep();
            const bool snap = ctx.snapScale() && sstep > 1e-4f;
            if (snap) factor = std::round(factor / sstep) * sstep;
            factor = glm::max(factor, 0.01f);

            // 拖拽方向 → 每个目标**自己的**局部轴上的缩放权重，以及"写给谁"。
            // 推导见 applyScaleToTargets 上方的长注释（拖 Y 却在 Z 方向拉伸
            // 那个 bug 的根因就在那里）。
            applyScaleToTargets(scene, tp, factor, axisIdx, planeN);
            m_changed = true;

            // 读数报**倍率**而不是那一维的绝对值 —— 旋转过的物体会把缩放
            // 摊到局部轴上，某个分量的绝对值已经没有"这就是你拖出来的量"
            // 的含义了，倍率才是用户手上真正做出来的动作。
            char buf[128];
            if (axisIdx >= 0) {
                std::snprintf(buf, sizeof(buf), TR("Scale %s: x%.3f%s"), TR(handleName()),
                              static_cast<double>(factor),
                              snap ? TR("  [snap]") : "");
            } else if (planeN >= 0) {
                std::snprintf(buf, sizeof(buf),
                              TR("Scale %s (2 axes): x%.3f%s"), TR(handleName()),
                              static_cast<double>(factor),
                              snap ? TR("  [snap]") : "");
            } else {
                std::snprintf(buf, sizeof(buf), TR("Scale all: x%.3f%s"),
                              static_cast<double>(factor),
                              snap ? TR("  [snap]") : "");
            }
            m_readout = buf;
            break;
        }

        case GizmoMode::Rotate: {
            float angle = 0.0f;
            glm::vec3 rotAxis = axis;

            if (screenHandle) {
                // ---- arcball 自由旋转 ----
                // 把"相对手柄中心的屏幕偏移"映射到单位虚拟球面上，
                // 起始点 → 当前点的旋转量就是这一次的旋转。
                const glm::vec3 p0 = arcballVec(
                    m_startMouse.x - m_originScreen.x,
                    m_startMouse.y - m_originScreen.y, kGizmoPixels);
                const glm::vec3 p1 = arcballVec(
                    mouse.x - m_originScreen.x, mouse.y - m_originScreen.y,
                    kGizmoPixels);

                const glm::vec3 c = glm::cross(p0, p1);
                const float s = glm::length(c);
                if (s < 1e-6f) return;   // 没动 / 正好转了 180° 的退化情况
                angle = std::atan2(s, glm::clamp(glm::dot(p0, p1), -1.0f, 1.0f));

                // 相机系 → 世界系：x = 相机右、y = 相机上、z = 朝向观察者
                const glm::vec3 camAxis = c / s;
                const glm::vec3 w =
                    m_dragU * camAxis.x + m_dragV * camAxis.y +
                    (-m_dragViewNormal) * camAxis.z;
                const float wl = glm::length(w);
                if (wl < 1e-6f) return;
                rotAxis = w / wl;
            } else {
                float a = 0.0f;
                if (!ringAngle(ray, m_dragOrigin, axis, a)) return;
                angle = a - m_startAngle;
                // 归一化到 (-pi, pi]
                while (angle > kPi) angle -= 2.0f * kPi;
                while (angle <= -kPi) angle += 2.0f * kPi;
            }

            const float rstep = ctx.snapRotateStep();  // 度
            const bool snap = ctx.snapRotate() && rstep > 1e-3f;
            if (snap) {
                const float step = glm::radians(rstep);
                angle = std::round(angle / step) * step;
            }

            // 始终在**拖拽起始姿态**上叠加，不做增量累乘。
            // 每个目标用的是**自己**的起始姿态（多选时各自的朝向不同）——
            // 用主选中项的起始姿态去套别的物体，转过 90° 的那几个会被
            // 强行掰到和它一样的朝向。
            const glm::mat3 D = glm::mat3(
                glm::rotate(glm::mat4(1.0f), angle, glm::normalize(rotAxis)));
            // 自由旋转（arcball）的轴是**世界系**算出来的，所以固定走世界空间
            const bool worldApply = m_worldSpace || screenHandle;
            applyRotateToTargets(scene, tp, D, worldApply);
            m_changed = true;

            char buf[128];
            std::snprintf(buf, sizeof(buf), TR("Rotate %s: %+.1f deg%s"),
                          TR(handleName()), static_cast<double>(glm::degrees(angle)),
                          snap ? TR("  [snap]") : "");
            m_readout = buf;
            break;
        }
    }
}

void GizmoController::endDrag(EditorContext& ctx, scene::Scene& scene) {
    // 先把"刚拖的是哪个手柄"拷出来：下面要清 m_dragHandles，而日志/状态栏
    // 还等着用它（handleName() 读的就是它，清完只能打出 "-"）。
    const std::string draggedHandle = handleName();

    m_dragging = false;
    m_dragHandles = Handle::None;
    m_readout.clear();

    if (!m_changed) {
        m_targets.clear();
        return;
    }
    const TrsPointers tp = trsOf(scene, m_entity, m_onCollider);
    if (!tp.valid()) {
        m_targets.clear();
        return;
    }

    const TransformSnapshot after{*tp.pos, *tp.rot, *tp.scale};

    // 值没变（拖了一圈又回到原点）就不进历史
    const bool same =
        glm::all(glm::equal(after.position, m_before.position)) &&
        glm::all(glm::equal(after.rotation, m_before.rotation)) &&
        glm::all(glm::equal(after.scale, m_before.scale));
    if (same && m_targets.size() <= 1) {
        m_targets.clear();
        return;
    }

    // ---- 收集"真的变了的"目标 ----
    //
    // 为什么要按目标过滤而不是"反正都写了"：多选里常有上锁的 / 已经和
    // 别人对齐的物体，它们这一帧的数值没动 —— 把它们塞进撤销记录会让
    // Ctrl+Z 之后的场景与按下之前"看起来一样，但历史里多了几条空命令"。
    //
    // ⚠ 碰撞体那条分支**不能**走这里：它改的是 CollisionComponent，
    //   TransformComponent 一个字都没动，逐目标比对会全部判成"没变"，
    //   于是拖了半天的碰撞体最后一脚被吞掉。
    std::vector<MultiTransformEditCommand::Item> items;
    if (!m_onCollider) {
        items.reserve(m_targets.size());
        for (const Target& t : m_targets) {
            const TransformSnapshot now =
                TransformSnapshot::capture(scene, t.entity);
            const bool moved =
                !glm::all(glm::equal(now.position, t.before.position)) ||
                !glm::all(glm::equal(now.rotation, t.before.rotation)) ||
                !glm::all(glm::equal(now.scale, t.before.scale));
            if (!moved) continue;
            items.push_back({t.entity, t.before, now});
        }
        if (items.empty()) {
            m_targets.clear();
            return;
        }
    }

    const std::string* nm = scene.world().name(m_entity);
    std::string label;
    if (items.size() > 1) {
        // "Move 3 objects" —— 名字用主选中项的话，"3 个物体一起动了"
        // 这件事在状态栏 / 历史里就看不出来（历史里会出现三条同名记录
        // 那样的误解，而这里其实只有一条）。
        label = std::string(modeName()) + " " + std::to_string(items.size()) +
                " objects";
    } else {
        label = std::string(modeName()) + " " +
                (m_onCollider ? "collider of " : "") +
                (nm ? *nm : std::string("Object"));
    }

    // 自动化探针：拖拽结束后的落地值。断言"拖 X 只改 scale.x"这类
    // 不变量就靠它（读数是界面文案，这条是数据）。
    VK_LOG_INFO("gizmo end: handle=%s '%s' pos=(%.3f,%.3f,%.3f) "
                "rot=(%.3f,%.3f,%.3f) scale=(%.3f,%.3f,%.3f) target=%s",
                draggedHandle.c_str(),
                nm ? nm->c_str() : "Object",
                static_cast<double>(after.position.x),
                static_cast<double>(after.position.y),
                static_cast<double>(after.position.z),
                static_cast<double>(after.rotation.x),
                static_cast<double>(after.rotation.y),
                static_cast<double>(after.rotation.z),
                static_cast<double>(after.scale.x),
                static_cast<double>(after.scale.y),
                static_cast<double>(after.scale.z),
                targetName());

    // 碰撞体用另一条命令：它写的是 CollisionComponent 的三个分量，
    // 不是实体的 TransformComponent（见 ColliderEditCommand 的说明）。
    // 碰撞体编辑天然单选（ctx.colliderEdit() 在多选时被强制关掉）。
    if (m_onCollider) {
        ctx.commands().pushAlreadyApplied(std::make_unique<ColliderEditCommand>(
            label, &scene, m_entity, m_before, after));
    } else if (items.size() > 1) {
        ctx.commands().pushAlreadyApplied(
            std::make_unique<MultiTransformEditCommand>(label, &scene,
                                                        std::move(items)));
    } else {
        // 单选（包括"多选了但只有一个真的动了"）：走老命令，保留它与
        // Inspector 数值框之间的合并判定（同一个实体 + 同一名字 → 就地更新）。
        const MultiTransformEditCommand::Item& it = items.front();
        ctx.commands().pushAlreadyApplied(std::make_unique<TransformEditCommand>(
            label, &scene, it.entity, it.before, it.after));
    }
    ctx.setStatus(label);
    // ---- G18：把"用户真的动过的分量"记成 prefab 覆盖 ----
    //
    // 只记**变了**的分量。三个全记的后果很隐蔽：一次平移就把 rotation /
    // scale 一起钉死，之后改母版的旋转对这棵树永远不再生效 —— 而作者
    // 从没碰过旋转。覆盖是**记账**，不是"与母版比差异"（见 Prefab.h 决定 ③）。
    //
    // 碰撞体那条分支不改 TransformComponent，不参与覆盖。
    if (!m_onCollider) {
        for (const Target& t : m_targets) {
            const TransformSnapshot now =
                TransformSnapshot::capture(scene, t.entity);
            if (!glm::all(glm::equal(now.position, t.before.position)))
                scene::recordOverride(scene, t.entity, "transform", "position");
            if (!glm::all(glm::equal(now.rotation, t.before.rotation)))
                scene::recordOverride(scene, t.entity, "transform", "rotation");
            if (!glm::all(glm::equal(now.scale, t.before.scale)))
                scene::recordOverride(scene, t.entity, "transform", "scale");
        }
    }
    if (m_targets.size() > 1) {
        VK_LOG_INFO("MULTI-DRAG n=%zu mode=%s handle=%s", m_targets.size(),
                    modeName(), draggedHandle.c_str());
    }
    m_targets.clear();
    ctx.dirty() = true;
    m_changed = false;
}

// ---------------------------------------------------------------- 主入口

bool GizmoController::update(EditorContext& ctx, const scene::Camera& camera,
                             const glm::vec2& viewportPos,
                             const glm::vec2& viewportSize, bool hovered,
                             ImDrawList* draw) {
    // 读数只在拖拽期间有意义；拖拽一旦结束（含各种提前返回）就清掉，
    // 免得叠加层里留着一行过期的数字。
    if (!m_dragging) m_readout.clear();

    // 只有编辑态允许拖手柄。运行态（含暂停）视口里那份是运行态副本，
    // 在这儿改既不会进历史也不会写回编辑态 —— 与其让人以为"改了没反应"，
    // 不如直接挡住。
    if (!ctx.isEditing()) {
        m_dragging = false;
        m_dragHandles = Handle::None;
        m_hover = Handle::None;
        return false;
    }

    // 选择模式（Q）：这一帧**根本没有手柄** —— 不画、不做命中、不吞左键。
    //
    // 放在几何计算之前返回，是为了不留下任何"上一次的姿态"：m_hover /
    // m_dragging 一并清掉，否则从"移动"切到"选择"的那一帧，指针还停在
    // 旧手柄上会继续显示抓手光标（看起来像手柄没消失）。
    if (ctx.gizmoMode() == GizmoMode::Select) {
        m_dragging = false;
        m_dragHandles = Handle::None;
        m_hover = Handle::None;
        return false;
    }

    m_viewportPos = viewportPos;
    m_viewportSize = viewportSize;
    m_viewProj = PickingSystem::viewProj(camera, viewportSize);

    scene::Scene& sc = ctx.editorScene();
    const ecs::Entity e = ctx.selection();
    auto* t = sc.world().get<ecs::TransformComponent>(e);
    if (!t) {
        m_dragging = false;
        m_dragHandles = Handle::None;
        m_hover = Handle::None;
        return false;
    }

    // 手柄这次作用在哪一组 TRS 上？
    //   · ctx.colliderEdit() 为真 **且** 选中项真的有碰撞体 → 碰撞体
    //   · 否则退回实体（哪怕开关还开着 —— 选中项被删了碰撞体也要能用）
    //
    // 多选时**强制回到实体**：一次拖拽写 N 个碰撞体在语义上没法自洽
    // （碰撞体是"这个物体身上的盒子"，N 个盒子一起挪没有任何含义），
    // 而选择集里只要有一个没碰撞体，那一条路径就要写空指针。
    const bool multiSel = ctx.multiSelected();
    m_onCollider = !multiSel && ctx.colliderEdit() &&
                   sc.world().has<ecs::CollisionComponent>(e);

    // 上锁的实体（自带地面）**不给手柄** —— 它固定不动，画个能拖的手柄
    // 只会误导（拖了没反应）。视口点选本来就会跳过它，这里挡的是"从
    // 层级树里选中"这条路径。
    if (sc.world().has<ecs::LockedComponent>(e)) {
        if (!m_lockedNotified) {
            m_lockedNotified = true;
            const std::string* nm = sc.world().name(e);
            VK_LOG_INFO("gizmo: '%s' is locked -> no transform handle",
                        nm ? nm->c_str() : "?");
        }
        m_dragging = false;
        m_dragHandles = Handle::None;
        m_hover = Handle::None;
        return false;
    }
    m_lockedNotified = false;

    // 手柄的世界矩阵：作用在实体上就是实体世界矩阵；作用在碰撞体上还要
    // 再乘一层碰撞体自己的局部矩阵（于是手柄画在碰撞体的中心、轴也跟它转）。
    const glm::mat4 handleXform = targetWorld(sc, e);
    // 多选时手柄画在**包围盒中心**（各成员世界位置的 AABB 中心），
    // 轴仍然取主选中项的 —— 一批朝向不同的物体没有"共同的朝向"，
    // 取平均出来的朝向既不属于谁，也没法预测转过头会怎样。
    m_origin = multiSel ? ctx.selectionPivot() : glm::vec3(handleXform[3]);

    // 手柄的世界长度：让它在屏幕上恒为 ~90px
    if (!PickingSystem::worldToScreen(m_viewProj, viewportPos, viewportSize,
                                      m_origin, m_originScreen)) {
        m_dragging = false;
        m_dragHandles = Handle::None;
        m_hover = Handle::None;
        return false;
    }

    // 相机基：fwd / right / up（世界空间单位向量）。
    // 屏幕平面（"整体"手柄用的那个平面）的法线就是 fwd。
    glm::vec3 fwd = camera.target() - camera.position();
    if (glm::length(fwd) < 1e-5f) fwd = glm::vec3(0, 0, -1);
    fwd = glm::normalize(fwd);
    glm::vec3 right = glm::cross(fwd, glm::vec3(0, 0, 1));  // Z-up 世界参考
    if (glm::length(right) < 1e-5f) right = glm::vec3(1, 0, 0);
    right = glm::normalize(right);
    const glm::vec3 up = glm::normalize(glm::cross(right, fwd));
    m_viewNormal = fwd;
    m_camRight = right;
    m_camUp = up;

    glm::vec2 probe;
    if (PickingSystem::worldToScreen(m_viewProj, viewportPos, viewportSize,
                                     m_origin + right, probe)) {
        m_pxPerUnit = glm::max(glm::length(probe - m_originScreen), 1e-3f);
    } else {
        m_pxPerUnit = 100.0f;
    }
    m_worldLen = kGizmoPixels / m_pxPerUnit;

    extractBasisXform(handleXform, ctx.gizmoSpace() == GizmoSpace::Local,
                      m_axisDir);
    // 同一份世界矩阵的列，**恒定**当成局部轴取一遍：缩放换算要用它
    // （见 applyDrag 的 Scale 分支）。世界空间下手柄画的是世界轴，
    // 但 scale 永远作用在局部轴上，这两件事必须分开记。
    extractBasisXform(handleXform, /*local=*/true, m_localAxisWorld);

    m_mode = ctx.gizmoMode();
    const float L = m_worldLen;

    // ---- 平面方片的屏幕四角 ----
    for (int i = 0; i < 3; ++i) {
        m_planeValid[i] = false;
        if (m_mode == GizmoMode::Rotate) continue;

        glm::vec3 u, v;
        planeBasisForHandles(i, u, v);
        const float a = L * kPlaneT0;
        const float b = L * kPlaneT1;
        const glm::vec3 corners[4] = {
            m_origin + u * a + v * a, m_origin + u * b + v * a,
            m_origin + u * b + v * b, m_origin + u * a + v * b,
        };
        bool ok = true;
        for (int k = 0; k < 4; ++k) {
            if (!PickingSystem::worldToScreen(m_viewProj, viewportPos,
                                              viewportSize, corners[k],
                                              m_planePts[i][k])) {
                ok = false;
                break;
            }
        }
        m_planeValid[i] = ok;
    }

    // ---- 旋转环的投影点 ----
    m_viewRingValid = false;
    for (int i = 0; i < 3; ++i) {
        m_ringValid[i] = false;
        if (m_mode != GizmoMode::Rotate) continue;

        glm::vec3 u, v;
        planeBasis(m_axisDir[i], u, v);
        bool ok = true;
        for (int k = 0; k < kRingSegments; ++k) {
            const float a = (2.0f * kPi * static_cast<float>(k)) /
                            static_cast<float>(kRingSegments);
            const glm::vec3 p =
                m_origin + (u * std::cos(a) + v * std::sin(a)) * L;
            if (!PickingSystem::worldToScreen(m_viewProj, viewportPos,
                                              viewportSize, p,
                                              m_ringPoints[i][k])) {
                ok = false;
                break;
            }
        }
        m_ringValid[i] = ok;
    }

    if (m_mode == GizmoMode::Rotate) {
        // 外圈（自由旋转）= 视平面上的圆，半径比轴环略大一档，免得完全糊在一起
        glm::vec3 u, v;
        planeBasis(m_viewNormal, u, v);
        const float R = L * 1.22f;
        bool ok = true;
        for (int k = 0; k < kRingSegments; ++k) {
            const float a = (2.0f * kPi * static_cast<float>(k)) /
                            static_cast<float>(kRingSegments);
            const glm::vec3 p = m_origin + (u * std::cos(a) + v * std::sin(a)) * R;
            if (!PickingSystem::worldToScreen(m_viewProj, viewportPos,
                                              viewportSize, p,
                                              m_viewRing[k])) {
                ok = false;
                break;
            }
        }
        m_viewRingValid = ok;
    }

    const glm::vec2 mouse(ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y);

    // 1) 悬停判定（拖拽中锁定手柄）
    if (!m_dragging) m_hover = hovered ? hitTest(mouse) : Handle::None;
    const Handle highlight = m_dragging ? m_dragHandles : m_hover;

    // 2) 绘制（高亮当前手柄）
    drawGizmo(ctx, draw, highlight);

    // 3) 交互
    bool consumed = false;
    if (!m_dragging) {
        if (m_hover != Handle::None) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                const Ray ray = PickingSystem::rayFromViewport(
                    camera, viewportPos, viewportSize, mouse);
                beginDrag(ctx, sc, e, ray, mouse);
                consumed = true;
            }
        }
    } else {
        consumed = true;
        const Ray ray = PickingSystem::rayFromViewport(camera, viewportPos,
                                                      viewportSize, mouse);
        applyDrag(ctx, sc, ray, mouse);
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) ||
            !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            endDrag(ctx, sc);
        }
    }

    return consumed;
}

} // namespace editor
