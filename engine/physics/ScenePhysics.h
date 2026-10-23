#pragma once
// ============================================================
// physics/ScenePhysics —— 场景 ⇄ 物理世界 的**唯一**绑定层
//
// ------------------------------------------------------------
// 为什么要有这个文件（2026-10-17，G01/G03）
// ------------------------------------------------------------
// 这段逻辑原来只住在 `editor/EditorApp.cpp::buildPhysicsWorld` 里。
// 那时项目只有一个可执行文件，这是合理的；但 Game.exe 出现之后，
// 运行时**也必须**从同一份场景建物理世界 —— 如果要复制一份，
// 就会出现两份"刚性体参数怎么翻译"的实现。
//
// 两份实现必然漂移，而漂移的表现极其恶劣：**编辑器里调好的手感，
// 出包后不一样**（正是 ship_roadmap §15 风险 #10）。所以这里把
// 三件事提到引擎层，编辑器与运行时**共用同一份代码**：
//
//   buildSceneWorld()         场景 → PhysicsWorld（+ 实体↔刚体映射）
//   syncKinematicFromScene()  实体 → 刚体（步进前，把运动学体的位移变成速度）
//   writeBackToScene()        刚体 → 实体（步进后，只写动态体）
//
// 依赖方向：本文件属于 `physics/`，而项目里 physics **本来就**依赖 scene
// （`CollisionWorld.h` 的操作数就是 `scene::Scene`），所以这里 include
// scene/Scene.h 不会成环。
//
// ------------------------------------------------------------
// 形状的原点与缩放（两条容易踩的规矩）
// ------------------------------------------------------------
//   · 刚性体的世界位姿是 **translate × rotate**（见 RigidBody.h：形状
//     原点即质心，没有第二层变换）。所以实体的缩放只能**烘进形状尺寸**，
//     否则"把物体拉长一倍，碰撞体纹丝不动"。
//   · 显式形状（Inspector 里手填的半径/半长）是**世界空间的米**，刻意
//     **不**乘实体缩放 —— "我填的是 0.5 米"是最好解释的语义。
//     ⚠ 例外：`CollisionComponent` 的 hull/capsule 走 `ccLocal` 的位姿，
//       它携带的缩放会一起烘进形状（与 Auto 路径一致）。
// ============================================================

#include "ecs/Entity.h"
#include "physics/Heightfield.h"
#include "physics/PhysicsWorld.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace scene {
class Scene;
}

namespace physics {

// 建世界的选项
struct SceneBuildOptions {
    // 场景里的 `PhysicsSettings::enabled` 是**权威**。
    // 这个字段只给自动化做 A/B 强制覆盖用（编辑器侧对应 `MYVK_PHYS`）：
    //   -1 = 听场景（正常路径）
    //    0 = 强制关
    //    1 = 强制开
    int enabledOverride = -1;
};

// 建出来的东西：世界 + 映射 + 几条统计（日志 / 面板用）
struct SceneBinding {
    // `enabled` 为假时这里为空 —— 调用方无需再判场景开关。
    std::unique_ptr<PhysicsWorld> world;
    // 实体 id → 刚体 id。**只含**真正建出来的体。
    std::unordered_map<ecs::EntityId, BodyId> entityToBody;
    int bodies = 0;         // 实际加进世界的体数（含地面）
    int rigidEntities = 0;  // 场景里带 RigidBodyComponent 的实体数
    // 触发体数量（G08）：带 TriggerComponent 的实体。它们也算 `bodies`。
    int triggers = 0;
    // 物理材质覆盖（G08）：带 PhysicsMaterialComponent 的体数。
    // 0 = 全部走 RigidBodyComponent 的字段（= G08 之前的行为）。
    int materials = 0;
    bool ground = false;    // 是否加了无限地面
    bool enabled = false;   // 场景开关（或覆盖后）的最终值

    // ---- 地形（第十轮）----
    // 场景里的 `TerrainComponent` 会被读成一份 CPU 高度场交给物理世界，
    // 于是地形是**真的地面**（刚体与角色都停在上面，不会陷进去）。
    //
    // ⚠ 为什么高度单独拷一份 `vector` 而不是留着 `terrain::TerrainAsset`：
    //   那会把 `terrain/` 拖进 `ScenePhysics.h`（本头文件是物理的公共面，
    //   不该为了一个成员引入上层模块的头）。
    //
    // ⚠ 2026-10-23：这两个成员**不再是** `PhysicsWorld::heightfield()`
    //   存活的前提。世界现在会自己拷一份地形数据
    //   （见 `PhysicsWorld::m_terrainOwned`），因为原先"借指针 + 调用方
    //   保证活得比世界长"的约定有个洞：`SceneBinding` 一旦析构，
    //   `terrainHeights` 的堆内存就还回去了，而 `terrainHf->heights`
    //   还指着它 —— 编辑器 `std::move(binding.world)` 之后正是这个状态，
    //   一进 Play 读地形就 ACCESS VIOLATION。
    //
    //   这里保留它们只是为了 `buildSceneWorld` 的返回结构稳定（以及
    //   `terrainHf` 上算好的 origin/mpp/heightScale 那份"最终参数"），
    //   不再承担所有权职责。新代码不要拿 `terrainHf.get()` 当长期指针用。
    std::vector<float> terrainHeights;      // 列主序，原样拷贝
    std::unique_ptr<Heightfield> terrainHf;  // 指向上面那块内存
    int terrains = 0;       // 场景里的地形数（>1 时只用了第一块）
};

// 场景 → 物理世界。
// `scene` 必须是**运行态**的那一份（编辑器在 Play 时用的是 runtimeScene），
// 因为它会被写回变换。
SceneBinding buildSceneWorld(scene::Scene& scene,
                             const SceneBuildOptions& opts = {});

// 运动学体：由**实体**推物理（而不是反过来）。
// 脚本 / 动画把运动学对象挪到哪，它就以那个速度去撞别人 —— 位置差除以 dt
// 就是"这一帧它走多快"。求解器拿这个速度算接触约束，被撞的动态物体才会
// 被真正推开（而不是被硬生生穿过）。
//
// ⚠ 必须在 `PhysicsWorld::step` **之前**调：step 内部会重算 AABB / 收接触。
// ⚠ dt <= 1e-6 时直接返回（除零）。
void syncKinematicFromScene(
    scene::Scene& scene, PhysicsWorld& world,
    const std::unordered_map<ecs::EntityId, BodyId>& map, float dt);

// 刚体 → 实体：只写**动态体**。
//   · 静态体不会动；
//   · 运动学体的位置本来就是源头（syncKinematicFromScene 从实体读进来的），
//     写回去等于原样。
// 写回时反解碰撞体的局部矩阵（`CollisionComponent::localMatrix()`），并把
// 世界矩阵除法回父节点的世界矩阵 —— 因为 `TransformComponent` 是**局部量**。
void writeBackToScene(
    scene::Scene& scene, const PhysicsWorld& world,
    const std::unordered_map<ecs::EntityId, BodyId>& map);

// 按世界缩放把形状"撑大"（见文件头第二条规矩）。
// 公开导出是因为 Inspector 的"按网格拟合"也要用同一套规则。
void scaleShapeForWorld(Shape& s, const glm::vec3& sc);

// 旋转矩阵 → **本站约定的 XYZ 欧拉角**（不是 glm::eulerAngles 的 YXZ）。
//
// ⚠ 为什么不能拿 `glm::eulerAngles` 代替：那是 **YXZ** 约定（跟 Unity 一致，
//   跟本站不一致）。用它的话"翻滚的箱子"写回实体后朝向会错位 —— 而且因为
//   欧拉角只在这一处被消费（渲染与 Inspector 都从它重建矩阵），错位是
//   **静默**的：物理算得完全正确，只是画出来的姿态不对。
glm::vec3 eulerXYZFromMatrix(const glm::mat3& m);

} // namespace physics
