#include "GizmoController.h"

#include "EditorContext.h"

#include "ecs/Components.h"
#include "scene/Camera.h"
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
    const glm::vec3 ref =
        std::fabs(nn.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
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
        default: return "Scale";
    }
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
            for (int i = 0; i < 3; ++i) {
                if (!m_planeValid[i]) continue;
                ImVec2 pts[4];
                for (int k = 0; k < 4; ++k) {
                    pts[k] = ImVec2(m_planePts[i][k].x, m_planePts[i][k].y);
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
            for (int i = 0; i < 3; ++i) {
                glm::vec2 tip;
                if (!PickingSystem::worldToScreen(
                        m_viewProj, m_viewportPos, m_viewportSize,
                        m_origin + m_axisDir[i] * m_worldLen, tip)) {
                    continue;
                }
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
            for (int i = 0; i < 3; ++i) {
                if (!m_ringValid[i]) continue;
                const ImU32 c = axisColor(i);
                const float th = (highlight == static_cast<Handle>(i)) ? 4.0f
                                                                      : 2.5f;
                ImVec2 pts[kRingSegments];
                for (int k = 0; k < kRingSegments; ++k) {
                    pts[k] = ImVec2(m_ringPoints[i][k].x, m_ringPoints[i][k].y);
                }
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

void GizmoController::beginDrag(EditorContext& ctx, scene::Scene& scene,
                                ecs::Entity e, const Ray& ray,
                                const glm::vec2& mouse) {
    auto* t = scene.world().get<ecs::TransformComponent>(e);
    if (!t) return;

    m_entity = e;
    m_mode = ctx.gizmoMode();
    m_worldSpace = (ctx.gizmoSpace() == GizmoSpace::World);
    m_before = TransformSnapshot::capture(scene, e);
    m_startPos = t->position;
    m_startRot = t->rotation;
    m_startScale = t->scale;
    m_startMouse = mouse;
    m_changed = false;
    m_dragging = true;
    m_dragHandles = m_hover;

    // ---- 冻结参考线（本次拖拽全程只认这份快照）----
    // 为什么不能直接用每帧更新的 m_origin：物体自己走过的位移会在下一帧
    // 被重新算进"鼠标沿轴的参数"里 → D_new = Δ + D_old，正反馈，越拖越快。
    m_dragOrigin = m_origin;
    m_dragWorldLen = m_worldLen;
    m_dragViewNormal = m_viewNormal;

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
}

void GizmoController::applyDrag(EditorContext& ctx, scene::Scene& scene,
                               const Ray& ray, const glm::vec2& mouse) {
    if (m_dragHandles == Handle::None) return;

    auto* t = scene.world().get<ecs::TransformComponent>(m_entity);
    if (!t) return;

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

            // 世界位移 → 局部位移：把父链的旋转/缩放抵消掉
            glm::vec3 localDelta = worldDelta;
            if (const auto* h =
                    scene.world().get<ecs::HierarchyComponent>(m_entity)) {
                if (h->parent != ecs::kInvalidEntity) {
                    const ecs::Entity pe = scene.world().handle(h->parent);
                    const glm::mat3 pm = glm::mat3(scene.worldMatrix(pe));
                    const float det = glm::determinant(pm);
                    if (std::fabs(det) > 1e-8f) {
                        localDelta = glm::inverse(pm) * worldDelta;
                    }
                }
            }

            t->position = m_startPos + localDelta;
            m_changed = true;

            char buf[128];
            if (axisIdx >= 0) {
                std::snprintf(buf, sizeof(buf), "Move %s: %+.3f m%s",
                              handleName(), static_cast<double>(readoutA),
                              snap ? "  [snap]" : "");
            } else {
                std::snprintf(buf, sizeof(buf),
                              "Move %s: %+.3f / %+.3f m%s", handleName(),
                              static_cast<double>(readoutA),
                              static_cast<double>(readoutB),
                              snap ? "  [snap]" : "");
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

            glm::vec3 sc = m_startScale;
            if (axisIdx >= 0) {
                sc[axisIdx] = m_startScale[axisIdx] * factor;
            } else if (planeN >= 0) {
                for (int i = 0; i < 3; ++i) {
                    if (i != planeN) sc[i] = m_startScale[i] * factor;
                }
            } else {
                sc = m_startScale * factor;   // 等比
            }
            t->scale = sc;
            m_changed = true;

            char buf[128];
            if (axisIdx >= 0) {
                std::snprintf(buf, sizeof(buf), "Scale %s: x%.3f%s", handleName(),
                              static_cast<double>(sc[axisIdx]),
                              snap ? "  [snap]" : "");
            } else if (planeN >= 0) {
                std::snprintf(buf, sizeof(buf),
                              "Scale %s (2 axes): x%.3f%s", handleName(),
                              static_cast<double>(factor),
                              snap ? "  [snap]" : "");
            } else {
                std::snprintf(buf, sizeof(buf), "Scale all: x%.3f%s",
                              static_cast<double>(factor),
                              snap ? "  [snap]" : "");
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

            // 始终在**拖拽起始姿态**上叠加，不做增量累乘
            const glm::mat3 R0 = rotationFromEuler(m_startRot);
            const glm::mat3 D = glm::mat3(
                glm::rotate(glm::mat4(1.0f), angle, glm::normalize(rotAxis)));
            // 自由旋转（arcball）的轴是**世界系**算出来的，所以固定走世界空间
            const bool worldApply = m_worldSpace || screenHandle;
            const glm::mat3 R = worldApply ? (D * R0) : (R0 * D);
            t->rotation = eulerFromRotation(R);
            m_changed = true;

            char buf[128];
            std::snprintf(buf, sizeof(buf), "Rotate %s: %+.1f deg%s",
                          handleName(), static_cast<double>(glm::degrees(angle)),
                          snap ? "  [snap]" : "");
            m_readout = buf;
            break;
        }
    }
}

void GizmoController::endDrag(EditorContext& ctx, scene::Scene& scene) {
    m_dragging = false;
    m_dragHandles = Handle::None;
    m_readout.clear();

    if (!m_changed) return;
    auto* t = scene.world().get<ecs::TransformComponent>(m_entity);
    if (!t) return;

    const TransformSnapshot after = TransformSnapshot::capture(scene, m_entity);

    // 值没变（拖了一圈又回到原点）就不进历史
    const bool same =
        glm::all(glm::equal(after.position, m_before.position)) &&
        glm::all(glm::equal(after.rotation, m_before.rotation)) &&
        glm::all(glm::equal(after.scale, m_before.scale));
    if (same) return;

    const std::string* nm = scene.world().name(m_entity);
    const std::string label =
        std::string(modeName()) + " " + (nm ? *nm : std::string("Object"));

    ctx.commands().pushAlreadyApplied(std::make_unique<TransformEditCommand>(
        label, &scene, m_entity, m_before, after));
    ctx.setStatus(label);
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

    m_origin = glm::vec3(sc.worldMatrix(e)[3]);

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
    glm::vec3 right = glm::cross(fwd, glm::vec3(0, 1, 0));
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

    extractBasisXform(sc.worldMatrix(e),
                      ctx.gizmoSpace() == GizmoSpace::Local, m_axisDir);

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
