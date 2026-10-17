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

constexpr int kCapsuleRingSegments = 24;   // 纬线环的边数
constexpr int kCapsuleArcSegments = 6;     // 每条经线在**单个球冠**上的弧段数
constexpr int kCapsuleMeridians = 12;      // 经线条数（每 30° 一条）
constexpr float kPi = 3.14159265358979323846f;

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
    //
    // 尺寸约定（与 ecs::CollisionComponent / 物理侧一致）：
    //   r = 半径；h = **圆柱段**的半高（不含两端半球）
    //   ⇒ 整根胶囊从 z = -(h+r) 到 z = +(h+r)。
    //
    // 2026-10-17 重写：原来的画法只在 XZ / YZ 两个平面画端部半圆，四个
    // 45° 方向上球冠是**空的**，看起来"少了一块"，两端像两片薄薄的镜头
    // 而不是半球。现在的画法和 UE / Unity 的调试胶囊一致：
    //
    //   (1) **经线**：每 30° 一条，从北极沿上半球下来 → 沿圆柱直下 →
    //       沿下半球到南极。一条线贯通整根胶囊，所以球冠与圆柱天然接上。
    //   (2) **纬线环**：圆柱两端 z = ±h 各一圈（半径 r，这就是"赤道"），
    //       两端球冠各再补一圈 45° 环（半径 r/√2）—— 有了它球冠看起来
    //       才是个球面，而不是一顶光秃秃的伞。
    const float r = cc.capsuleRadius;
    const float h = cc.capsuleHalfHeight;

    auto toWorld = [&](const glm::vec3& p) {
        return glm::vec3(xform * glm::vec4(p, 1.0f));
    };

    // ---- (1) 经线 ----
    for (int m = 0; m < kCapsuleMeridians; ++m) {
        const float a = 2.0f * kPi * static_cast<float>(m) /
                        static_cast<float>(kCapsuleMeridians);
        const float ca = std::cos(a);
        const float sa = std::sin(a);

        glm::vec3 prev(0.0f, 0.0f, h + r);            // 北极
        // 上半球：theta 0 → 90°，半径 r*sin、高度 h + r*cos
        for (int i = 1; i <= kCapsuleArcSegments; ++i) {
            const float t = 0.5f * kPi * static_cast<float>(i) /
                            static_cast<float>(kCapsuleArcSegments);
            const glm::vec3 p(r * std::sin(t) * ca, r * std::sin(t) * sa,
                              h + r * std::cos(t));
            pushSeg(out, toWorld(prev), toWorld(p));
            prev = p;
        }
        // 母线（圆柱段）
        const glm::vec3 low(r * ca, r * sa, -h);
        pushSeg(out, toWorld(prev), toWorld(low));
        prev = low;
        // 下半球：theta 0 → 90°，半径 r*cos、高度 -h - r*sin
        for (int i = 1; i <= kCapsuleArcSegments; ++i) {
            const float t = 0.5f * kPi * static_cast<float>(i) /
                            static_cast<float>(kCapsuleArcSegments);
            const glm::vec3 p(r * std::cos(t) * ca, r * std::cos(t) * sa,
                              -h - r * std::sin(t));
            pushSeg(out, toWorld(prev), toWorld(p));
            prev = p;
        }
    }

    // ---- (2) 纬线环 ----
    // 顺序：圆柱下端赤道 → 圆柱上端赤道 → 下端 45° 环 → 上端 45° 环
    {
        const float rc = r * 0.70710678f;    // r/√2：45° 处的半径
        const float zc = r * 0.70710678f;    // 球冠中心到 45° 环的高度差
        const float ringZ[4] = {h, -h, h + zc, -h - zc};
        const float ringR[4] = {r, r, rc, rc};
        for (int k = 0; k < 4; ++k) {
            glm::vec3 prev(ringR[k], 0.0f, ringZ[k]);
            for (int i = 1; i <= kCapsuleRingSegments; ++i) {
                const float a = 2.0f * kPi * static_cast<float>(i) /
                                static_cast<float>(kCapsuleRingSegments);
                const glm::vec3 p(ringR[k] * std::cos(a),
                                  ringR[k] * std::sin(a), ringZ[k]);
                pushSeg(out, toWorld(prev), toWorld(p));
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
            const bool isSel = (e == sel);
            const bool wireHidden =
                !cc.showWireframe && !(isSel && editing);

            // 状态签名在任何情况下都要打（自动化断言"勾选框切到隐藏"
            // 靠它 —— 线框被隐藏时恰恰最需要这行日志）。
            if (std::getenv("MYVK_LOG_RECTS")) {
                const std::string* nm = w.name(e);
                static std::string lastSig;
                const std::string sig =
                    (nm ? *nm : std::string("?")) + "|" +
                    (cc.shape == ecs::ColliderShape::ConvexHull ? "convex"
                                                                : "capsule") +
                    "|" + std::to_string(cc.hullPoints.size()) + "|" +
                    std::to_string(cc.hullEdges.size()) + "|" +
                    (cc.solid ? "1" : "0") + "|" +
                    (cc.showWireframe ? "1" : "0");
                if (sig != lastSig) {
                    lastSig = sig;
                    VK_LOG_INFO("COLLIDER-SIG '%s' shape=%s points=%zu edges=%zu "
                                "solid=%d show=%d",
                                nm ? nm->c_str() : "?",
                                cc.shape == ecs::ColliderShape::ConvexHull
                                    ? "convex"
                                    : "capsule",
                                cc.hullPoints.size(), cc.hullEdges.size(),
                                cc.solid ? 1 : 0, cc.showWireframe ? 1 : 0);
                }
            }

            // 用户关掉线框就不画；但正在编辑（W/E/R 调整）的选中碰撞体
            // 强制显示 —— 否则调整时看不见自己在动什么。
            if (wireHidden) return;

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

            // 自动化：屏幕包围矩形（隐藏时不打 —— 没画东西就没有矩形语义）
            if (std::getenv("MYVK_LOG_RECTS")) {
                const std::string* nm = w.name(e);
                const std::string tag =
                    std::string("COLLIDER ") + (nm ? *nm : std::string("?"));
                logRect(tag.c_str(), ImVec2(minX, minY), ImVec2(maxX, maxY));
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
    // 线框被隐藏（且不在编辑中）就不再响应点击 —— 看不见的东西不该点到
    if (!cc->showWireframe && !ctx.colliderEdit()) return false;

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
