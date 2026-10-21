#pragma once
// ============================================================
// editor/WaterEditor —— 水体编辑模式（左栏 "Water"）
//
// ------------------------------------------------------------
// 它编辑的是**场景组件**，没有磁盘资产
// ------------------------------------------------------------
// `ecs::WaterComponent` 里就是一块水体的全部参数（water/WaterBody.h），
// 存盘走场景 JSON。所以与地形最大的差别：**没有 saveAssets 这一步** ——
// 改一个滑块，下一帧渲染器就从组件里读到新值。
//
// 于是"实时"这件事几乎免费：
//     滑块 → 组件 → Renderer::syncWaters → WaterPass::sync
//          → 写 128 字节 UBO → 水面下一帧就变了
// 只有**形状/尺寸**变了才重建网格（顶点缓冲），其余参数都是写 UBO。
//
// ------------------------------------------------------------
// 交互：拖出一个水体
// ------------------------------------------------------------
// 用户原话："类似用鼠标在指定区域去拖拽可生出对应的水体"。
//
// 按下（左键，悬停在地形上）→ 立刻建实体（尺寸 0.5 m，几乎看不见）
// 拖动                      → 尺寸跟着鼠标走**实时**变化（水面已经在画了）
// 松开                      → 尺寸 < 1 m 视为误触，删掉；否则落定并选中
//
// 为什么"按下就建实体"而不是先画一个 ImGui 预览再在松手时建：
//   · 用户要的是"拖拽生水体"，边拖边看到水才是这个交互的意义；
//   · 预览层和真实水面一定会有出入（波纹/折射/泡沫都是着色器算的），
//     画一个假的圆环反而让人以为是两套东西。
//
// 形状：圆形 = 从起点拖出的**半径**；矩形 = 从起点拖到对角点的**半长**。
// 水位默认取按下点地表高度 + kDefaultRise，之后可以用滑块单独调。
// ============================================================

#include "EditorContext.h"
#include "water/WaterBody.h"

#include <glm/glm.hpp>

#include <cstdint>

struct ImDrawList;

namespace editor {

class WaterEditor {
public:
    explicit WaterEditor(EditorContext& ctx) : m_ctx(ctx) {}

    // 面板 UI（EditorApp::panelDrawWater 调）
    void drawPanel();

    // 视口里的一帧输入。返回 true = 本帧"吃掉"了左键（视口不该再点选）。
    // 由 ViewportPanel 在 editMode == Water 时调用。
    bool viewportInput(const glm::vec2& vpPos, const glm::vec2& vpSize,
                       bool hovered, ImDrawList* dl);

    // 进入模式 / 选中变化时重新绑定
    void sync();

    // 当前有没有选中一块水体
    bool hasWater() const;

    // ---- W/E/R 手柄 ↔ WaterBody 的镜像（2026-10-21 第七轮）----
    //
    // 水体**没有网格组件**，所以它不在场景绘制列表里；变换手柄（gizmo）
    // 只认 `TransformComponent` —— 于是"手柄拖了但水面不动"。
    //
    // 做法：把 Transform 当成**手柄载体**，每帧双向同步一次：
    //   · `syncTransformFromBody`：body → Transform（手柄站到水面上）
    //   · `applyTransformToBody`：Transform → body（移动/旋转/缩放写回去）
    // 调用点必须**成对**夹住 gizmo.update —— 上一帧写下去的 body 位置
    // 就是这一帧手柄的起点；只做一半会让手柄与水面互相打架。
    void syncTransformFromBody(ecs::Entity e);
    void applyTransformToBody(ecs::Entity e);

    // 缩放的"上一帧已乘进 halfExtent 的倍率"。
    //
    // ⚠ 必须记这个：gizmo 写的是**绝对**倍率（`m_startScale * factor`，
    //   startScale 被 sync 钉成 1，所以 tf.scale 就是"相对拖拽起点"的
    //   累计倍率）。如果每帧直接 `halfExtent *= tf.scale`，就会把累计量
    //   反复乘进去 —— 实测一次拖拽把半径从 18 干到 2.4e9。
    //   正解：只乘"本帧倍率 / 上帧倍率"这个**比值**。
    glm::vec2 m_scaleApplied{1.0f, 1.0f};
    std::uint32_t m_scaleAppliedId = 0;

private:
    ecs::WaterComponent* selected();          // 可能为空
    const ecs::WaterComponent* selected() const;

    void drawCreateSection();
    void drawShapeSection(ecs::WaterComponent& wc);
    void drawLookSection(ecs::WaterComponent& wc);
    void drawMotionSection(ecs::WaterComponent& wc);
    void drawStatsSection();

    // ---- 预设选择器（按钮行，不是下拉） ----
    //
    // 为什么不用 ImGui::Combo（2026-10-21 改）：这里有两个都是"枚举值一键
    // 切换"的场景（5 种水质 / 2 种形状），Combo 的代价是"点开 → 再点一项"，
    // 而弹出层的项高由主题的 FramePadding 决定 —— 参数面板里最常用的动作
    // 不该依赖这种东西。改成一排按钮之后：
    //   · 用户一下就到（"默认给定几种水体类型"本来就该是可直接点的）；
    //   · 每个按钮有**自己的**锚点（`WATER-KIND sea`），自动化不需要猜像素。
    // `tag` 是锚点前缀，`current` 用来高亮当前项。
    void drawKindButtons(water::WaterBody& b, const char* tag,
                         std::uint32_t ownerId, bool keepLayout);
    void drawShapeButtons(water::WaterBody& b, const char* tag,
                          std::uint32_t ownerId);

    // 建一块新水体（世界 xz 中心 + 水位），返回实体。
    // `fitGround` = 建完立刻采样地形（拖拽路径传 false：尺寸还在变，
    // 松手时再采样 —— 见 GroundProbe 的注释）。
    ecs::Entity createBody(const glm::vec2& center, float level,
                           water::Shape shape, const water::WaterBody& proto,
                           const char* why, bool fitGround);

    // 把当前拖拽的几何写进正在编辑的那块水体
    void applyDrag(const glm::vec2& anchor, const glm::vec2& cur);

    // 采样 footprint 下的地形：刷新 `bedLevel`（池底），并（可选）把水位
    // 贴到"岸线最低点 + kDefaultRise"。改动尺寸、创建、点 Fit 时调。
    // 场景里没有地形时退化成"以当前水位为准的平地"，水体照样贴地。
    void fitToGround(water::WaterBody& b, bool followLevel);

    // 视口里的尺寸提示（拖拽中 / 选中时的轮廓）
    void drawFootprint(ImDrawList* dl, const glm::vec2& vpPos,
                       const glm::vec2& vpSize, const water::WaterBody& b,
                       ImU32 col);

    EditorContext& m_ctx;

    // ---- 拖拽状态 ----
    bool m_dragging = false;
    // 本次拖拽建出来的实体。存**整个句柄**而不是 id：id 会被回收复用，
    // generation 才是"还是不是同一个东西"的判据。
    ecs::Entity m_dragEntity{};
    glm::vec2 m_dragAnchor{0.0f};
    float m_dragLevel = 0.0f;

    // ---- 新建用的默认参数 ----
    // 与"当前选中"分开存：这样用户调好的水质类型不会因为换了块水体就丢
    water::WaterBody m_proto = water::makeDefault();
    int m_seq = 1;                // 命名计数（Water_1 / Water_2 …）
    std::uint32_t m_boundId = 0;  // 当前面板绑定到的实体 id（变化时打一条日志）
    // 下一帧把面板滚回顶部（内容在"新建 / 选中"之间切换时用）
    bool m_scrollTop = false;
};

} // namespace editor
