#pragma once
// ============================================================
// editor/ColliderView —— 碰撞框在视口里的可视化
//
// ------------------------------------------------------------
// 为什么用 ImGui 的 draw list，而不是走渲染管线画 3D 线
// ------------------------------------------------------------
//   1. 视口里的手柄、导航球、栅格 HUD 已经全是这条路子；再加一条 3D 线
//      管线要动 PipelineLayout / 描述符布局 / 着色器 / 顶点缓冲，代价比
//      收益大得多（而且 setLayouts 数量不一致会直接报 VUID）。
//   2. 编辑器里的碰撞框本来就该"透视可见"（X-ray）：被自己的模型挡住反而
//      没法调。UE 的碰撞体线框默认也是这个行为。
//   代价是没有深度遮挡 —— 这一点和手柄一样，是刻意的取舍。
//
// ------------------------------------------------------------
// 画什么
// ------------------------------------------------------------
//   · ConvexHull → 凸包的**边**（不是面；面是三角形，画出来是一团糊）
//   · Capsule    → 两端圆 + 四条母线 + 两个方向的端部半圆
//
// 顺带把每个碰撞体的屏幕包围矩形 + 一行签名日志打出来（MYVK_LOG_RECTS=1），
// 自动化脚本据此断言"加了碰撞体之后视野里真的多出一个框"。
// ============================================================

#include "EditorContext.h"
#include "PickingSystem.h"

#include <glm/glm.hpp>

#include <vector>

struct ImDrawList;

namespace editor {

class ColliderView {
public:
    // 画场景里所有实体的碰撞框（**只在编辑态**）。
    void draw(EditorContext& ctx, const glm::vec2& vpPos,
              const glm::vec2& vpSize, ImDrawList* dl, bool hovered);

    // 鼠标是否落在"选中实体的碰撞框"那圈线上（屏幕距离 < tolPx）。
    // 用来实现"点碰撞框 → 手柄切换到编辑碰撞体"。
    //
    // 只测选中项：全场景扫一遍线框在这个规模下是纯浪费，而且语义上也
    // 不该让"点远处的碰撞框"改掉手柄的目标。
    bool hitSelectedWireframe(EditorContext& ctx, const glm::vec2& vpPos,
                              const glm::vec2& vpSize, const glm::vec2& mouse,
                              float tolPx = 6.0f);

private:
    // 把一个实体的碰撞框拆成**世界空间**线段（成对端点，存进 m_segments3）
    static void buildSegments(const ecs::CollisionComponent& cc,
                              const glm::mat4& xform,
                              std::vector<glm::vec3>& out);

    // 投影缓存（避免每帧重新分配）
    std::vector<glm::vec3> m_segments3;
    std::vector<glm::vec2> m_screen;
    std::vector<char> m_ok;
};

} // namespace editor
