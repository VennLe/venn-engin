// ============================================================
// editor/ColliderView.cpp
// ============================================================

#include "ColliderView.h"

#include "DebugRects.h"

#include "core/Logger.h"
#include "scene/Scene.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

namespace editor {
namespace {

constexpr int kCapsuleRingSegments = 20;   // 两端圆的边数
constexpr int kCapsuleArcSegments = 10;    // 端部半圆的边数

const ImU32 kColIdle = IM_COL32(96, 208, 196, 165);      // 普通碰撞框：青
const ImU32 kColSelected = IM_COL32(255, 202, 74, 225);  // 选中：金黄
const ImU32 kColEditing = IM_COL32(255, 148, 40, 255);   // 正在编辑：橙

void pushSeg(std::vector<glm::vec3>& out, const glm::vec3& a,
             const glm::vec3& b) {
    out.push_back(a);
    out.push_back(b);
}

} // namespace

void ColliderView::buildSegments(const ecs::CollisionComponent& cc,
                                 const glm::mat4& xform,
                                 std::vector<glm::vec3>& out) {
    out.clear();

    if (cc.shape == ecs::ColliderShape::ConvexHull) {
        const auto& pts = cc.hullPoints;
        for (const glm::uvec2& e : cc.hullEdges) {
            if (e.x >= pts.size() || e.y >= pts.size()) continue;
            out.push_back(glm::vec3(xform * glm::vec4(pts[e.x], 1.0f)));
            out.push_back(glm::vec3(xform * glm::vec4(pts[e.y], 1.0f)));
        }
        return;
    }

    // ---- 胶囊：轴 = 局部 +Z ----
    const float r = cc.capsuleRadius;
    const float h = cc.capsuleHalfHeight;

    auto toWorld = [&](const glm::vec3& p) {
        return glm::vec3(xform * glm::vec4(p, 1.0f));
    };

    // 两端圆
    for (int end = 0; end < 2; ++end) {
        const float z = end == 0 ? h : -h;
        glm::vec3 prev;
        for (int i = 0; i <= kCapsuleRingSegments; ++i) {
            const float a = 2.0f * 3.14159265f * static_cast<float>(i) /
                            static_cast<float>(kCapsuleRingSegments);
            const glm::vec3 p(r * std::cos(a), r * std::sin(a), z);
            if (i > 0) pushSeg(out, toWorld(prev), toWorld(p));
            prev = p;
        }
    }

    // 四条母线（圆的 0/90/180/270 度处）
    for (int k = 0; k < 4; ++k) {
        const float a = 0.5f * 3.14159265f * static_cast<float>(k);
        const glm::vec3 dir(r * std::cos(a), r * std::sin(a), 0.0f);
        pushSeg(out, toWorld(dir + glm::vec3(0, 0, h)),
                toWorld(dir + glm::vec3(0, 0, -h)));
    }

    // 端部半圆：XZ 与 YZ 两个平面各画一道，合起来就是个"球冠"的轮廓
    for (int end = 0; end < 2; ++end) {
        const float sign = end == 0 ? 1.0f : -1.0f;
        for (int plane = 0; plane < 2; ++plane) {
            glm::vec3 prev;
            for (int i = 0; i <= kCapsuleArcSegments; ++i) {
                const float a = 3.14159265f * static_cast<float>(i) /
                                static_cast<float>(kCapsuleArcSegments);
                const float s = std::sin(a) * r;
                const float c = std::cos(a) * r;
                const glm::vec3 p = (plane == 0)
                                        ? glm::vec3(s, 0.0f, sign * (h + c))
                                        : glm::vec3(0.0f, s, sign * (h + c));
                if (i > 0) pushSeg(out, toWorld(prev), toWorld(p));
                prev = p;
            }
        }
    }
}

void ColliderView::draw(EditorContext& ctx, const glm::vec2& vpPos,
                        const glm::vec2& vpSize, ImDrawList* dl, bool hovered) {
    // Play 期间视口里是游戏画面，编辑器叠加层一律不画
    if (!ctx.isEditing() || !dl) return;

    scene::Scene& sc = ctx.activeScene();
    // 用 **const** 的 world 遍历：非 const 的 each() 会为不存在的组件类型
    // 现场建一个空池，等于每帧往场景里塞垃圾。
    const ecs::World& w = sc.world();

    const glm::mat4 viewProj = PickingSystem::viewProj(sc.camera(), vpSize);
    const ecs::Entity sel = ctx.selection();
    const bool editing = ctx.colliderEdit();

    w.each<ecs::CollisionComponent>(
        [&](ecs::Entity e, const ecs::CollisionComponent& cc) {
            const glm::mat4 xform = sc.worldMatrix(e) * cc.localMatrix();
            buildSegments(cc, xform, m_segments3);
            if (m_segments3.empty()) return;

            // 投影（失败的段直接丢 —— 端点在相机背后）
            const std::size_t n = m_segments3.size();
            m_screen.resize(n);
            m_ok.assign(n, 0);
            float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
            int visibleCount = 0;
            for (std::size_t i = 0; i < n; ++i) {
                if (!PickingSystem::worldToScreen(viewProj, vpPos, vpSize,
                                                  m_segments3[i],
                                                  m_screen[i])) {
                    continue;
                }
                m_ok[i] = 1;
                ++visibleCount;
                minX = std::min(minX, m_screen[i].x);
                minY = std::min(minY, m_screen[i].y);
                maxX = std::max(maxX, m_screen[i].x);
                maxY = std::max(maxY, m_screen[i].y);
            }
            if (visibleCount == 0) return;

            const bool isSel = (e == sel);
            const ImU32 col = (isSel && editing) ? kColEditing
                              : isSel            ? kColSelected
                                                 : kColIdle;
            const float thick = (isSel && editing) ? 2.0f : 1.4f;

            for (std::size_t i = 0; i + 1 < n; i += 2) {
                if (!m_ok[i] || !m_ok[i + 1]) continue;
                dl->AddLine(ImVec2(m_screen[i].x, m_screen[i].y),
                            ImVec2(m_screen[i + 1].x, m_screen[i + 1].y), col,
                            thick);
            }

            // 自动化：屏幕包围矩形 + 一行签名（形状 / 顶点数 / 边数）
            if (std::getenv("MYVK_LOG_RECTS")) {
                const std::string* nm = w.name(e);
                const std::string tag =
                    std::string("COLLIDER ") + (nm ? *nm : std::string("?"));
                logRect(tag.c_str(), ImVec2(minX, minY), ImVec2(maxX, maxY));

                static std::string lastSig;
                const std::string sig =
                    (nm ? *nm : std::string("?")) + "|" +
                    (cc.shape == ecs::ColliderShape::ConvexHull ? "convex"
                                                                : "capsule") +
                    "|" + std::to_string(cc.hullPoints.size()) + "|" +
                    std::to_string(cc.hullEdges.size());
                if (sig != lastSig) {
                    lastSig = sig;
                    VK_LOG_INFO("COLLIDER-SIG '%s' shape=%s points=%zu edges=%zu "
                                "solid=%d",
                                nm ? nm->c_str() : "?", 
                                cc.shape == ecs::ColliderShape::ConvexHull
                                    ? "convex"
                                    : "capsule",
                                cc.hullPoints.size(), cc.hullEdges.size(),
                                cc.solid ? 1 : 0);
                }
            }
        });

    (void)hovered;
}

bool ColliderView::hitSelectedWireframe(EditorContext& ctx,
                                        const glm::vec2& vpPos,
                                        const glm::vec2& vpSize,
                                        const glm::vec2& mouse, float tolPx) {
    if (!ctx.isEditing()) return false;
    scene::Scene& sc = ctx.activeScene();
    const ecs::Entity e = ctx.selection();
    if (!e.valid()) return false;
    const auto* cc = sc.world().get<ecs::CollisionComponent>(e);
    if (!cc) return false;

    const glm::mat4 xform = sc.worldMatrix(e) * cc->localMatrix();
    buildSegments(*cc, xform, m_segments3);
    if (m_segments3.empty()) return false;

    const glm::mat4 viewProj = PickingSystem::viewProj(sc.camera(), vpSize);
    const std::size_t n = m_segments3.size();
    m_screen.resize(n);
    m_ok.assign(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        if (PickingSystem::worldToScreen(viewProj, vpPos, vpSize,
                                         m_segments3[i], m_screen[i]))
            m_ok[i] = 1;
    }
    for (std::size_t i = 0; i + 1 < n; i += 2) {
        if (!m_ok[i] || !m_ok[i + 1]) continue;
        if (PickingSystem::screenDistanceToSegment(mouse, m_screen[i],
                                                   m_screen[i + 1]) <= tolPx)
            return true;
    }
    return false;
}

} // namespace editor
