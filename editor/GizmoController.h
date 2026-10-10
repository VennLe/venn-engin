#pragma once
// ============================================================
// editor/GizmoController —— 视口里的移动 / 旋转 / 缩放手柄
//
// 为什么不用 ImGuizmo：third_party 里没有，而它自带一份需要长期维护的
// 源码 + 它的坐标系约定（右手 Z-up、深度朝 -Z）与本引擎的相机/裁剪约定
// 并不完全一致，接进来反而要写一层适配。这里用手柄自绘 + 射线数学，
// 一共两个文件，行为完全可控。
//
// ------------------------------------------------------------
// 能抓的东西（和 Blender / UE 一样的三种粒度）
// ------------------------------------------------------------
//   · **单轴**  X / Y / Z 三根轴：
//       移动 = 沿轴平移；旋转 = 绕轴转；缩放 = 单轴拉伸
//   · **双轴（平面）**  YZ / XZ / XY 三块方片：
//       移动 = 在这个平面里自由移动；缩放 = 这两个轴一起缩放
//       方片画在两根轴夹角处，颜色 = **平面法线**的颜色
//       （法线是 X 的那块是红的 —— 与 UE / Blender 的配色习惯一致）
//   · **整体**  原点上的中心手柄：
//       移动 = 在**视平面**里自由移动（跟手，前后不动）
//       缩放 = 三个轴**等比**缩放
//       旋转 = 外圈自由旋转（arcball，绕屏幕上的任意方向转）
//
// ------------------------------------------------------------
// 交互模型
// ------------------------------------------------------------
//   · 手柄画在 ImGui 的 draw list 上（屏幕空间），世界→屏幕走
//     PickingSystem::worldToScreen。
//   · 手柄的世界尺寸随相机距离自适应，屏幕长度基本恒定（约 90px）。
//   · 单轴移动 / 缩放：把鼠标射线与"手柄轴所在直线"求最近点，用参数差
//     得到沿轴位移 —— 这样从任何角度看都不会跳。
//   · 平面移动 / 缩放：把射线与"过物体、以该平面法线为法线"的平面求交，
//     交点相对起点的位移投影到平面内两个基向量上。
//   · 旋转（单轴）：把射线与"过物体、垂直于旋转轴"的平面求交，交点相对
//     轴心的方位角之差就是旋转量。
//   · 旋转（自由 / arcball）：把鼠标相对手柄中心的屏幕偏移映射到虚拟球面
//     上，起始点 → 当前点的旋转就是这次的旋转量。
//   · 旋转/自由旋转都始终在**拖拽开始时的姿态**上叠加，不做增量累乘，
//     避免长拖拽的浮点漂移。
//   · 拖拽中每帧都在改 Transform，但只在**松手那一刻**入一次撤销栈
//     （命令合并，见 Command.h 的设计说明）。
//
// ------------------------------------------------------------
// ⚠ 拖拽的参考线必须**在按下鼠标那一刻冻结**
// ------------------------------------------------------------
// 平移是"物体绝对位置 = 起始位置 + 轴向增量"，而增量是拿鼠标射线与
// 轴线的最近点参数作差算出来的。如果这条轴线每帧都取**物体当前**的
// 位置（m_origin 就是这么更新的），那么物体自己走过的位移会在下一帧
// 又被算进参数里 —— 正反馈。实测：鼠标不动也会每帧再走一遍刚才的
// 位移，手感就是"越拖越快、直接飞出屏幕"。
//   （曾经还有一个符号错误：最近点参数被整体取反 → 方向完全反过来。
//     两个 bug 叠在一起就是用户报的"方向是反的 + 速度过快"。）
// 所以 beginDrag 里把 origin / 轴向 / 平面基向量 / 手柄世界长度全部存
// 一份，applyDrag 只认这份快照；m_origin 每帧更新只是为了让手柄跟着
// 物体画。
//
// ------------------------------------------------------------
// 吸附（三套独立，参考 UE5 视口工具条）
// ------------------------------------------------------------
//   · 平移：位移增量按 snapMoveStep（米）量化（平面移动按两个方向分别量化）
//   · 旋转：角度增量按 snapRotateStep（度）量化
//   · 缩放：缩放**倍率**按 snapScaleStep 量化（倍率而不是绝对值 ——
//     否则拖拽第一下就会从 0.37 之类的位置跳到 0.4，看起来像抽了一下）
//   开关与步长都由 EditorContext 持有，三种变换各自独立。
// ============================================================

#include "Command.h"        // TransformSnapshot
#include "EditorContext.h"  // GizmoMode / GizmoSpace
#include "PickingSystem.h"

#include <glm/glm.hpp>

#include <string>

namespace scene {
class Scene;
class Camera;
}
struct ImDrawList;

namespace editor {

// 手柄上一次能抓住的"东西"。
//
// 平面枚举名写的是**该平面里不包含的那个轴**（也就是这个平面的法线）：
// PlaneYZ 的法线是 X、PlaneXZ 的法线是 Y、PlaneXY 的法线是 Z。
// 之所以这么定，是因为它的颜色按法线取 —— 法线 X 的那块是红的，
// 于是"红方块 = 法线朝 X 的面板"，和 UE / Blender 一致，不用记额外规则。
enum class Handle : int {
    None = -1,
    AxisX = 0,
    AxisY = 1,
    AxisZ = 2,
    PlaneYZ = 3,  // 法线 X（红）
    PlaneXZ = 4,  // 法线 Y（绿）
    PlaneXY = 5,  // 法线 Z（蓝）
    Screen = 6,   // 中心手柄：视平面移动 / 等比缩放 / 自由旋转
};

class GizmoController {
public:
    // 在视口里更新 + 绘制手柄。
    //   hovered : 鼠标是否落在视口图像上
    //   返回    : 本帧手柄吃掉了鼠标（调用方不要再做点选/相机操作）
    bool update(EditorContext& ctx, const scene::Camera& camera,
                const glm::vec2& viewportPos, const glm::vec2& viewportSize,
                bool hovered, ImDrawList* draw);

    bool dragging() const { return m_dragging; }

    // 正在拖拽的手柄（Handle::None = 没有）
    Handle hotHandle() const { return m_dragHandles; }
    const char* handleName() const;
    const char* modeName() const;

    // 手柄当前作用在哪一组 TRS 上（"entity" / "collider"）。
    // 供状态栏与自动化日志断言用。
    const char* targetName() const {
        return m_onCollider ? "collider" : "entity";
    }
    bool editingCollider() const { return m_onCollider; }

    // 拖拽过程中的实时读数（"Move X: +0.30 m" / "Move XY: +0.30 / +0.10 m" /
    // "Rotate Z: +45.0deg" / "Scale all: x1.10"，吸附打开时末尾带 " [snap]"）。
    // 擦除后为空串。
    const std::string& readout() const { return m_readout; }

private:
    // 旋转环用折线近似，段数越高越圆（绘制与命中测试共用同一组点）
    static constexpr int kRingSegments = 48;

    // 命中测试：返回抓到的手柄（Handle::None = 没抓到）
    Handle hitTest(const glm::vec2& mouse) const;

    void drawGizmo(EditorContext& ctx, ImDrawList* draw, Handle highlight) const;

    void beginDrag(EditorContext& ctx, scene::Scene& scene, ecs::Entity e,
                   const Ray& ray, const glm::vec2& mouse);
    void applyDrag(EditorContext& ctx, scene::Scene& scene, const Ray& ray,
                   const glm::vec2& mouse);
    void endDrag(EditorContext& ctx, scene::Scene& scene);

    // 平面手柄（法线 = 轴 index）在平面内用的两个基向量：index 之外的另两轴
    void planeBasisForHandles(int normalIdx, glm::vec3& u, glm::vec3& v) const;

    // 手柄这次要读写哪一组 TRS。
    //
    // 实体的 TransformComponent 与实体的 CollisionComponent 里，
    // position / rotation / scale 三个字段的名字、类型、欧拉角顺序**完全
    // 一致**，所以平移 / 旋转 / 缩放那套数学一行都不用改 —— 只需要在
    // 读写的那一刻换一组指针。这里就是那个"换指针"的地方。
    //
    // 返回空指针组表示"该目标不存在"（比如选中项没有碰撞体）。
    struct TrsPointers {
        glm::vec3* pos = nullptr;
        glm::vec3* rot = nullptr;
        glm::vec3* scale = nullptr;
        bool valid() const { return pos && rot && scale; }
    };
    static TrsPointers trsOf(scene::Scene& scene, ecs::Entity e,
                             bool onCollider);

    // 手柄的世界矩阵：实体世界矩阵（作用在实体上）
    // 或 实体世界矩阵 × 碰撞体局部矩阵（作用在碰撞体上）
    glm::mat4 targetWorld(scene::Scene& scene, ecs::Entity e) const;

    // 投影缓存：每帧算一次，绘制与命中测试共用
    glm::vec2 m_originScreen{0.0f};
    glm::mat4 m_viewProj{1.0f};
    glm::vec2 m_viewportPos{0.0f};
    glm::vec2 m_viewportSize{1.0f};
    glm::vec3 m_axisDir[3]{glm::vec3(1, 0, 0), glm::vec3(0, 1, 0),
                           glm::vec3(0, 0, 1)};
    // 物体**自己的**三根局部轴在世界里的方向（= 世界矩阵的三列，归一化）。
    // 和 m_axisDir 的区别：m_axisDir 是"手柄画出来/被点中的"那套轴，随
    // GizmoSpace 在"世界轴"和"局部轴"之间切换；这一份**恒定是局部轴**，
    // 专门给缩放的换算用 —— TransformComponent::scale 是局部的，而手柄
    // 拖的是世界里的方向，两者对旋转过的物体并不重合。详见 applyDrag。
    glm::vec3 m_localAxisWorld[3]{glm::vec3(1, 0, 0), glm::vec3(0, 1, 0),
                                  glm::vec3(0, 0, 1)};
    glm::vec3 m_camRight{1.0f, 0.0f, 0.0f};
    glm::vec3 m_camUp{0.0f, 1.0f, 0.0f};
    glm::vec3 m_viewNormal{0.0f, 0.0f, 1.0f};  // 相机视线方向
    float m_worldLen = 1.0f;
    float m_pxPerUnit = 1.0f;

    // 平面手柄的三个方片（屏幕空间的四个角；绘制与命中都用它）
    glm::vec2 m_planePts[3][4];
    bool m_planeValid[3] = {false, false, false};

    // 旋转环的投影折线（3 根轴 + 1 根视轴）
    glm::vec2 m_ringPoints[3][kRingSegments];
    bool m_ringValid[3] = {false, false, false};
    glm::vec2 m_viewRing[kRingSegments];
    bool m_viewRingValid = false;

    Handle m_hover = Handle::None;   // 本帧鼠标悬停的手柄（绘制高亮用）
    Handle m_dragHandles = Handle::None;  // 正在拖的那个（拖拽中冻结）
    bool m_dragging = false;
    bool m_changed = false;
    bool m_worldSpace = true;

    ecs::Entity m_entity{};
    GizmoMode m_mode = GizmoMode::Translate;
    // 这次（拖拽期间冻结）作用在碰撞体上？
    bool m_onCollider = false;
    TransformSnapshot m_before;
    glm::vec3 m_startPos{0.0f};
    glm::vec3 m_startRot{0.0f};
    glm::vec3 m_startScale{1.0f};
    glm::vec3 m_origin{0.0f};       // 每帧跟随物体（只用于绘制/命中）
    // ---- 拖拽开始时的冻结快照 ----
    glm::vec3 m_dragOrigin{0.0f};   // 轴心
    glm::vec3 m_dragAxis{1.0f, 0.0f, 0.0f};   // 单轴：轴向；平面/屏幕：平面法线
    glm::vec3 m_dragU{1.0f, 0.0f, 0.0f};      // 平面内基向量 1
    glm::vec3 m_dragV{0.0f, 1.0f, 0.0f};      // 平面内基向量 2
    glm::vec3 m_dragViewNormal{0.0f, 0.0f, 1.0f};
    // 拖拽开始时的局部轴（缩放换算用；和上面几个一样必须冻结）
    glm::vec3 m_dragLocalAxes[3]{glm::vec3(1, 0, 0), glm::vec3(0, 1, 0),
                                 glm::vec3(0, 0, 1)};
    float m_dragWorldLen = 1.0f;
    float m_startParam = 0.0f;      // 单轴：最近点参数
    glm::vec3 m_startHit{0.0f};     // 平面 / 屏幕：按下时的交点
    float m_startAngle = 0.0f;      // 单轴旋转：按下时的方位角
    glm::vec2 m_startMouse{0.0f};
    std::string m_readout;
    // 上锁实体的"不给手柄"提示只打一次日志（否则每帧一行刷屏）；
    // 离开锁定状态时复位，下次再选中它还会提示。
    bool m_lockedNotified = false;
};

} // namespace editor
