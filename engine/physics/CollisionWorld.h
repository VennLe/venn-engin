#pragma once
// ============================================================
// physics/CollisionWorld —— 场景级的碰撞体与求解
//
// 分两件事：
//
//   A. **生成**：把一个实体的网格变成碰撞体几何
//        · ConvexHull  → QuickHull 求凸包（紧紧包住模型）
//        · Capsule     → 用局部 AABB 拟合一个"刚好套住"的胶囊
//    两者的默认摆放都是"刚好包裹住物体"（见各自实现里的推导），之后可以
//    用 W/E/R 手柄单独调整。
//
//   B. **求解**（只在运行态跑）：
//        · resolveBodyOverlaps —— 所有带碰撞体的实体两两分离
//          （"不能互相穿过"）
//        · resolveCamera       —— 相机当成一个球推出碰撞体
//          （"相机视角也不允许穿过"）
//
// ------------------------------------------------------------
// 位移写回哪里
// ------------------------------------------------------------
// 分离时改的是实体的 TransformComponent::position（物体和它的碰撞体一起
// 动），而不是碰撞体自己的局部 position —— 后者会让"碰撞框慢慢从模型里
// 飘出去"。但 Transform 是**局部量**，而穿透方向是世界空间的，所以要用
// 父节点世界矩阵的逆把世界位移换回局部位移（translateEntityWorld）。
// ============================================================

#include "ecs/Components.h"
#include "ecs/Entity.h"
#include "physics/ConvexShape.h"

#include <glm/glm.hpp>

#include <vector>

namespace assets {
class Mesh;
}
namespace scene {
class Scene;
class Camera;
}

namespace physics {

// ------------------------------------------------------------
// A. 生成碰撞体
// ------------------------------------------------------------
//
// 编辑器只跟这几个"组件级"入口打交道（它拿到的是一整个 CollisionComponent，
// 不需要认识 ConvexShape）。低层的 ConvexShape 版本留在下面，供内部复用。
//
// 三个都返回 false 表示 mesh 是空的 —— 连兜底都做不了。

// 凸包：QuickHull 求凸包 → hullPoints / hullEdges；position 设成 AABB 中心
// （于是"碰撞体局部 TRS 全默认"就等于刚好包住模型）。
bool setupHullCollider(const assets::Mesh& mesh, ecs::CollisionComponent& cc);

// 只重建凸包几何（**不动** position / rotation / scale）。
// 场景读盘后调用：凸包顶点是派生数据，不进 JSON。
bool refreshHullGeometry(const assets::Mesh& mesh, ecs::CollisionComponent& cc);

// 胶囊：轴 = AABB 最长维，半径 = 另外两维的外接圆半径（保证整个包住
// AABB），position = AABB 中心。
bool setupCapsuleCollider(const assets::Mesh& mesh,
                          ecs::CollisionComponent& cc);

// 低层版本：直接给出 ConvexShape（内部复用 / 将来别的用途）
bool buildHullCollider(const assets::Mesh& mesh, ConvexShape& shape,
                       glm::vec3& position, glm::vec3& rotation);

// 按网格 AABB 拟合胶囊。轴取 AABB 最长的那一维（转到碰撞体局部 +Z），
// 半径取另外两维半长的**外接圆**半径 —— 这样胶囊一定包住整个 AABB，
// 这就是"刚好包裹住"。
void buildCapsuleCollider(const assets::Mesh& mesh, ConvexShape& shape,
                          glm::vec3& position, glm::vec3& rotation);

// 网格的局部 AABB（顶点为空时返回 false）。生成碰撞体时用同一份数据，
// 于是"碰撞体 AABB ≈ 网格 AABB"就是"刚好包裹住物体"的可断言形式。
bool meshAabb(const assets::Mesh& mesh, glm::vec3& mn, glm::vec3& mx);

// 碰撞组件在**实体局部空间**里的 AABB（把 cc.localMatrix() 作用到形状的
// 局部 AABB 上）。注意这是"组件自身"的范围，不含实体 Transform —— 要看
// 世界空间范围用 buildCollider()。几何为空（凸包顶点还没生成）→ false。
bool colliderLocalAabb(const ecs::CollisionComponent& cc, glm::vec3& mn,
                       glm::vec3& mx);

// ------------------------------------------------------------
// B. 世界空间碰撞体
// ------------------------------------------------------------

struct Collider {
    ecs::Entity entity{};
    ConvexShape shape;       // 局部空间（已居中）
    glm::mat4 world{1.0f};   // 碰撞体的世界矩阵
    glm::vec3 aabbMin{0.0f};
    glm::vec3 aabbMax{0.0f};
    bool solid = true;
};

// 收集场景里所有有碰撞组件的实体。hullPoints 为空的（刚读盘 / 还没生成）
// 会按网格现场补齐一次。
void collectColliders(scene::Scene& scene, std::vector<Collider>& out);

// 单个实体的碰撞体（没有碰撞组件 / 几何为空 → false）
bool buildCollider(scene::Scene& scene, ecs::Entity e, Collider& out);

// 按网格重新生成碰撞几何（**不动** position/rotation/scale）。
// 场景读盘后调用：凸包顶点是派生数据，不进 JSON，要按 mesh 重建。
void refreshColliderGeometry(scene::Scene& scene, ecs::Entity e);

// ------------------------------------------------------------
// C. 运行态求解
// ------------------------------------------------------------

struct BodyContact {
    ecs::Entity a{};
    ecs::Entity b{};
    float depth = 0.0f;
    glm::vec3 normal{0.0f, 1.0f, 0.0f};  // a → b
};

// 把世界位移换算回实体**局部**位移并加在 position 上。
// 有父节点时要用父节点世界矩阵的逆（父节点带旋转/缩放时，直接加会歪）。
void translateEntityWorld(scene::Scene& scene, ecs::Entity e,
                          const glm::vec3& worldDelta);

// 所有 solid 碰撞体两两分离。iterations 轮迭代，每轮把互相重叠的对各推
// 开一半的穿透深度。返回"实际推开过的对数"（>0 表示这一帧发生过碰撞）。
int resolveBodyOverlaps(scene::Scene& scene, int iterations = 4,
                        std::vector<BodyContact>* contacts = nullptr);

// 相机不穿墙：把相机当成半径 radius 的球，推出所有 solid 碰撞体。
// 返回是否真的推过（调用方据此打日志）。
bool resolveCamera(scene::Scene& scene, scene::Camera& cam, float radius);

} // namespace physics
