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
// **Play 态视口只在"已接管"之后吃键鼠**（P8，2026-10-23 起）：进 Play 时
// 光标可见、输入冻结；左键点一下视口才隐藏光标并把键鼠交给游戏，Esc 交还。
// 相机由运行态场景自己的逻辑驱动，编辑器只负责"把它渲染出来"和"全屏切换"。
// Play 态右上角只留一块极小的浮层（全屏 / 恢复按钮 + 播放状态 + 接管提示），
// 因为全屏时工具栏是不画的。
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

class LandscapeEditor;
class FoliageEditor;

class ViewportPanel {
public:
    ViewportPanel(EditorContext& ctx, render::Renderer& renderer);

    void draw();

    // 2026-10-20：挂上两个"编辑模式工具"（地形 / 植被）。
    //
    // 为什么由视口转发明键鼠，而不是让面板自己读 ImGui：视口是唯一知道
    // "鼠标在哪一格、这一下点进没点进 3D 画面、右键是不是在转相机"的地方。
    // 工具只认"给我一条射线 + 是不是按下"，其余判定全在这里。
    // 指针可以不挂（headless / 单测），都判空。
    void attachModeTools(LandscapeEditor* landscape, FoliageEditor* foliage,
                         class WaterEditor* water = nullptr) {
        m_landscapeEditor = landscape;
        m_foliageEditor = foliage;
        m_waterEditor = water;
    }
    // 2026-10-20 晚：另外两个模式也要视口交互（顶点色绘制 / 刷子体放置）。
    // 单独一个入口而不是把 attachModeTools 改成四个参数：它已经被
    // EditorApp 调用，加参数会让"没挂工具"的调用点全部要改（headless 测试）。
    void attachToolkitTools(class MeshPaintEditor* paint, class BrushEditor* brush,
                            class ModelingEditor* modeling = nullptr) {
        m_meshPaintEditor = paint;
        m_brushEditor = brush;
        m_modelingEditor = modeling;
    }

    // 手柄是否正在拖拽（Toolbar 之类的状态显示用）
    bool gizmoDragging() const { return m_gizmo.dragging(); }

    // 视图方向快捷键（Blender 小键盘那一套：1/3/7 + Ctrl 反向，2/4/6/8 步进）。
    // 由 EditorApp::handleShortcuts 每帧调一次；内部自带"仅编辑态"判断。
    void handleViewShortcuts();

    // E14：浮层快捷键。Shift+1..N 切视图模式；Ctrl+Shift+数字 存相机书签，
    // Alt+Shift+数字 跳回书签。同样由 EditorApp::handleShortcuts 调用。
    //
    // 为什么全挂在 Shift 层：数字键本身被两套既有快捷键占满了 ——
    // 1/3/7(+Ctrl) 是 Blender 视图方向、Alt+数字 是面板显隐（E04）。
    // Shift 层是当时唯一还空着、又和"第二功能"直觉一致的位置。
    void handleOverlayShortcuts();

    // 数字键 0：把选中的物体**竖直**落地（x / y 不动，只让世界包围盒底部
    // 贴到 z = 0 的地面）。只对"在 3D 视口里左键点中"的选中生效
    // （见 EditorContext::selectionFromViewport）。
    void dropSelectionToGround();

    // 关掉视口内所有 2D 叠加层（左上角统计文字、右上角导航 HUD、左下角
    // 导航球、手柄）。给自动化截图用：这些叠加层里有 FPS / 帧时间这种
    // **每次运行都不同**的数字，会污染"背景像素应当完全一致"这类断言。
    // 只影响 ImGui 覆盖层，不影响 3D 场景本身的渲染。
    void setHudVisible(bool visible) {
        m_showOverlay = visible;
        m_showGizmo = visible;
    }

private:
    // 视口工具条（2026-10-17 晚重做：一条紧凑图标行 + 若干下拉，
    // 结构对齐 UE5 视口工具条 —— 见 ViewportPanel.cpp 顶部那段说明）
    void drawToolbar();
    // 工具条上的下拉内容。每个函数**只画 popup 内部**，由 drawToolbar
    // 每帧无条件提交（不提交的那一帧 ImGui 会把弹层关掉）。
    // 打开下拉 / 给下拉定位。**为什么需要这一对**：ImGui 的 OpenPopup 不带
    // 定位时会把弹层摆在**鼠标位置**（正好压在工具条那一行上），而 UE 的
    // 下拉是从按钮左下角往下展开的。openDropdown 记住刚提交的那个按钮矩形，
    // placeDropdown 在 BeginPopup 之前把它设成弹层左上角。
    void openDropdown(const char* popupId);
    void placeDropdown();
    void drawToolMenu();            // 变换工具（带搜索框 + 快捷键）
    void drawSpaceMenu();           // 世界 / 局部坐标
    void drawSnapMenu(int which);   // 0=move 1=grid 2=rotate 3=scale
    void drawViewMenu();            // 透视 / 视图方向 / 相机
    void drawLayoutMenu();          // 视口分屏布局（1 / 2V / 2H / 4）
    void drawSpeedMenu();           // 相机速度
    void drawLightMenu();           // 光照模式（= 视图模式）
    void drawShowMenu();            // 显示标志
    void drawVpSettingsMenu();      // 栅格 / 背景 / 参数
    // 编辑态导航（UE 视口手感：飞行相机 + 滚轮推拉 / 右键滚轮调灵敏度）
    void handleNavigation(bool hovered) const;
    // E14 · 视口浮层（各自独立显隐，见 EditorContext::ViewportOverlay）
    //   （左下角的相机信息块已并入 Inspector → World → Camera）
    //   左上 = 视图模式条
    //   右上 = 统计 HUD（FPS / 帧时间 / draw calls / 三角形 / VRAM / 分辨率 /
    //          视图模式 / 实体 / 选中，逐行可关）
    void drawStatsHud(const glm::vec2& vpPos, const glm::vec2& vpSize);
    // 右键视口空白处 → "Viewport Display" 菜单：浮层开关 + 统计 HUD 的
    // 逐项开关 + 网格步长/背景（右键点中选中物体时仍然是物体菜单，见
    // drawObjectMenu 的按下/松开判定）。
    void drawDisplayMenu(const glm::vec2& vpPos, const glm::vec2& vpSize,
                         bool hovered);
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
    // 把相机对到任意实体（Hierarchy 双击走这条）。没有网格的实体
    // （GameStart / 水体）用它自己的尺寸估距离，见 .cpp。
    void focusEntity(ecs::Entity e) const;

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

    // ---- 框选（2026-10-27 #316）----
    //
    // 只在"左键按在空白处"时开：按了手柄（gizmo）或是笔刷模式的左键，
    // 都在上面被别的分支吃掉了，走不到这里。
    bool m_boxActive = false;
    bool m_boxAdditive = false;   // 按下时按着 Shift/Ctrl = 叠加到现有选中
    glm::vec2 m_boxStart{0.0f};
    glm::vec2 m_boxCurrent{0.0f};
    // 小于这个边长就当"点了一下空白"（清空选中）而不是拉框。
    // 4px 是手抖的上限：定大了"点空白清选中"会变得很难点中。
    static constexpr float kBoxSelectMinPx = 4.0f;
    // e 的屏幕投影矩形是否与框 [a,b] 有交集（碰到就选，见 .cpp 里的说明）
    bool entityInsideBox(ecs::Entity e, const glm::mat4& viewProj,
                         const glm::vec2& vpPos, const glm::vec2& vpSize,
                         const glm::vec2& a, const glm::vec2& b);

    // 编辑模式工具（**不拥有**；生命周期归 EditorApp）。
    // 见 attachModeTools：视口只负责把射线与键鼠状态发过去。
    LandscapeEditor* m_landscapeEditor = nullptr;
    FoliageEditor* m_foliageEditor = nullptr;
    // 2026-10-21：水体模式（拖出一块水体）
    class WaterEditor* m_waterEditor = nullptr;
    class MeshPaintEditor* m_meshPaintEditor = nullptr;
    class BrushEditor* m_brushEditor = nullptr;
    class ModelingEditor* m_modelingEditor = nullptr;

    std::uint32_t m_requestedW = 0;
    std::uint32_t m_requestedH = 0;
    bool m_resizePending = true;  // 尺寸刚变 → 本帧只画占位

    bool m_showOverlay = true;
    bool m_showGizmo = true;

    // 统计 HUD 的"收起"状态（会话内，不落盘）。收起后只画一颗 "Stats" 胶囊。
    // 与 EditorContext::ViewportOverlay::stats 的区别：
    //   stats  = 这块 HUD 要不要存在（偏好，落盘）
    //   本字段 = 存在的情况下收不收起来（临时）
    bool m_statsCollapsed = false;

    // 工具下拉里的搜索框（"开始输入以搜索"）。会话内有效 —— 这是
    // "找工具"的临时输入，不该跟着布局落盘。
    char m_toolSearch[64] = {0};
    // 上一次**打过日志**的搜索词。只在内容变化时打一行 —— 每帧打会刷爆日志。
    char m_toolSearchLogged[64] = {0};

    // 下拉弹层的锚点（屏幕坐标 = 刚点开的那个按钮的左下角）。
    // 用两个 float 而不是 ImVec2：这个头文件不 include imgui.h。
    float m_popupX = 0.0f;
    float m_popupY = 0.0f;

    // 上一次**打过日志**的工具模式（GizmoMode 的整数值；-1 = 还没打过）。
    // 用在 drawToolbar 末尾做差分：三条入口（工具按钮 / 快捷键 / 顶栏下拉）
    // 只在这一处留日志，见 .cpp 里的说明。
    int m_lastToolMode = -1;

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
