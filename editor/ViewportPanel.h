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

#include "EditorContext.h"
#include "GizmoController.h"
#include "PickingSystem.h"

#include <glm/glm.hpp>

#include <cstdint>

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
    // 运行态右上角浮层：全屏 / 恢复按钮 + 播放状态（不提供任何导航）
    void drawPlayHud(const glm::vec2& vpPos, const glm::vec2& vpSize);
    void focusSelection() const;

    EditorContext& m_ctx;
    render::Renderer& m_renderer;
    PickingSystem m_picking;
    GizmoController m_gizmo;

    std::uint32_t m_requestedW = 0;
    std::uint32_t m_requestedH = 0;
    bool m_resizePending = true;  // 尺寸刚变 → 本帧只画占位

    bool m_showOverlay = true;
    bool m_showGizmo = true;
};

} // namespace editor
