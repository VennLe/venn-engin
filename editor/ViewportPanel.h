#pragma once
// ============================================================
// editor/ViewportPanel —— 把 3D 场景渲染到 ImGui 子窗口里
//
// 这是"渲染目标不再是窗口全屏"的落地点：
//
//   Renderer::setViewportSize(w, h)
//        → 场景写进一张 w×h 的离屏图（模式 B，见 PostProcess.h）
//   Renderer::viewportTextureId()
//        → 那张离屏图对应的 ImGui 纹理（VkDescriptorSet 的整型化）
//   ImGui::Image(texId, avail)
//        → 贴进本面板
//
// 面板同时承担视口内的全部交互：
//   · 相机导航（**仅编辑态**）—— 完全按 UE 编辑器的习惯：
//       按住右键拖动 = 自由旋转（水平 + 俯仰）
//       按住中键拖动 = 平移
//       单独滚轮     = 沿视线推 / 拉相机（缩放）
//       右键 + 滚轮  = 调导航灵敏度
//       按住右键时 WASD = 前后左右飞行，Q / E = 下降 / 上升，Shift = 加速
//       （飞行只在按住右键时生效，这样 W / E / R 才能继续当手柄快捷键用）
//   · 点选物体（射线拾取，仅编辑态；左键专用于点选与拖手势，不参与导航）
//   · gizmo 拖拽（平移 / 旋转 / 缩放，仅编辑态）
//   · 信息叠加层（帧率、实体数、选中项、当前手柄模式、拖拽实时读数）
//
// 工具栏分两行：第一行是手柄模式 / 坐标系 / 显示开关，第二行是**吸附**
// （drawSnapRow）—— 平移 / 旋转 / 缩放各一套开关 + 步长，照 UE5 的模型。
// 塞一行会被挤出右边缘裁掉，所以必须分开。
//
// **Play 态视口不抢任何键鼠输入**：相机由运行态场景自己的逻辑驱动，
// 编辑器只负责"把它渲染出来"和"全屏切换"。Play 态右上角只留一块极小的
// 浮层（全屏 / 恢复按钮 + 播放状态），因为全屏时工具栏是不画的。
//
// 编辑态右上角则浮一块导航 HUD（drawNavHud）：灵敏度输入框 + 地平面栅格
// 的显示开关 + 操作提示。它必须是个**子窗口**（在 ImGui::End() 之前用
// BeginChild 画）：ImGui 每帧会把焦点窗口提到最前，独立顶层窗口会被
// 视口的 3D 图盖住。
//
// ------------------------------------------------------------
// 一个必须小心的时序问题
// ------------------------------------------------------------
// ImGui 的 UI 在 drawFrame 之前构建，而离屏链的重建发生在 drawFrame 内部。
// 如果本帧的尺寸变了，本帧的绘制数据会引用**即将被销毁**的纹理描述符集。
// 所以尺寸变化的这一帧只画占位块，等下一帧再用新纹理画 —— 代价是拖拽
// 面板期间视口是空白的，换来的是永远不会出现 use-after-free。
// ============================================================

#include "ColliderView.h"
#include "EditorContext.h"
#include "GizmoController.h"
#include "PickingSystem.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>

struct ImDrawList;

namespace render {
class Renderer;
}

namespace editor {

class ViewportPanel {
public:
    ViewportPanel(EditorContext& ctx, render::Renderer& renderer);

    void draw();

    // 手柄是否正在拖拽（Toolbar 之类的状态显示用）
    bool gizmoDragging() const { return m_gizmo.dragging(); }

    // 视图方向快捷键（Blender 小键盘那一套：1/3/7 + Ctrl 反向，2/4/6/8 步进）。
    // 由 EditorApp::handleShortcuts 每帧调一次；内部自带"仅编辑态"判断。
    void handleViewShortcuts();

    // 数字键 0：把选中的物体**竖直**落地（x / y 不动，只让世界包围盒底部
    // 贴到 z = 0 的地面）。只对"在 3D 视口里左键点中"的选中生效
    // （见 EditorContext::selectionFromViewport）。
    void dropSelectionToGround();

private:
    void drawToolbar();
    // 工具栏第二行：平移 / 旋转 / 缩放各自的吸附开关 + 步长 + UE 预设
    void drawSnapRow();
    void drawOverlay(ImDrawList* dl, const glm::vec2& vpPos,
                     const glm::vec2& vpSize) const;
    // 编辑态导航（UE 视口手感：飞行相机 + 滚轮推拉 / 右键滚轮调灵敏度）
    void handleNavigation(bool hovered) const;
    // 编辑态右上角浮层：灵敏度 + 栅格开关 + 操作提示
    void drawNavHud(const glm::vec2& vpPos, const glm::vec2& vpSize);
    // 编辑态左下角导航球：拖拽转视角 / 点轴端小球吸附到正视图
    void drawNavGizmo(const glm::vec2& vpPos, const glm::vec2& vpSize);
    // 运行态右上角浮层：全屏 / 恢复按钮 + 播放状态（不提供任何导航）
    void drawPlayHud(const glm::vec2& vpPos, const glm::vec2& vpSize);
    // Shift+A 添加菜单（Blender 的 Add 菜单）：鼠标处弹出 Mesh / Light
    // 两个子菜单。菜单里加出来的物体落在 m_addSpot（打开菜单那一刻
    // 鼠标射线与地面的交点）—— 弹出后就固定住，不跟着菜单里的鼠标走。
    void drawAddMenu(const glm::vec2& vpPos, const glm::vec2& vpSize,
                     bool hovered);
    // 执行"往场景里加一个物体"并选中新实体（Inspector 随之显示它的参数）
    void addEntity(const char* what, const std::function<ecs::Entity()>& create);
    void focusSelection() const;

    // 选中物体上的**右键菜单**：添加 / 移除碰撞体、切换"手柄编辑碰撞体"。
    //
    // 和右键导航（按住右键转头）共存的判定：按下与松开之间鼠标几乎没动
    // （< 6px）才算"点击"。UE 的视口就是这么分这两件事的 —— 动一下是
    // 转头，没动就是上下文菜单。
    void drawObjectMenu(const glm::vec2& vpPos, const glm::vec2& vpSize,
                        bool hovered);

    // 给实体装碰撞体 / 拆掉（都走 structuralEdit，可撤销）
    void addCollisionBody(ecs::Entity e, ecs::ColliderShape shape);
    void removeCollisionBody(ecs::Entity e);

    EditorContext& m_ctx;
    render::Renderer& m_renderer;
    PickingSystem m_picking;
    GizmoController m_gizmo;
    ColliderView m_colliders;

    std::uint32_t m_requestedW = 0;
    std::uint32_t m_requestedH = 0;
    bool m_resizePending = true;  // 尺寸刚变 → 本帧只画占位

    bool m_showOverlay = true;
    bool m_showGizmo = true;

    // ---- 导航球（Navigation Gizmo）的拖拽状态 ----
    // m_navPressDir：按下时命中的轴端（ViewDir 枚举，-1 = 没点在小球上）。
    // 松手时若位移没超过阈值就当成"点击"→ 吸附到那个正视图；否则算"拖拽"。
    bool m_navDragging = false;
    bool m_navMoved = false;
    int m_navPressDir = -1;
    glm::vec2 m_navPressPos{0.0f, 0.0f};

    // ---- Shift+A 添加菜单 ----
    // 打开菜单那一刻鼠标射线与 z=0 地面的交点（新增物体的落点）
    glm::vec3 m_addSpot{0.0f, 0.0f, 0.0f};

    // "第几次打开这个菜单"。菜单项矩形是按 logRect 打的，而 logRect 会**按
    // 矩形去抖** —— 同一个位置第二次打开菜单时矩形没变，就一行都不会再打。
    // 自动化脚本据此定位时会直接拿到上一次的陈旧坐标，于是在子菜单真正展
    // 开之前就点了下去（实测：第一次加 Cube 成功，第二次加 Sphere 必失败，
    // 因为 VP-ADD-Sphere 的坐标是上一次留下的）。把序号拼进 tag 就能让每
    // 一次打开都重新打点，脚本按"最后一个序号"取到的永远是新鲜的。
    int m_addMenuSeq = 0;

    // ---- 选中物体的右键菜单 ----
    glm::vec2 m_ctxPressPos{0.0f, 0.0f};      // 按下右键的位置
    glm::vec2 m_ctxMenuPos{0.0f, 0.0f};       // 弹出位置（按下时的鼠标位置）
    bool m_ctxPressOnSelected = false;        // 按下时鼠标是否落在选中物体上
    int m_ctxMenuSeq = 0;                     // 和 Add 菜单同理：矩形日志按序号去抖
};

} // namespace editor
