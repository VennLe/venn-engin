#pragma once
// ============================================================
// runtime/SpringArm —— 第三人称"弹簧臂"（G10）
//
// 相机不是"角色身上焊一根棍"，而是**从枢轴往后探一根可伸缩的臂**：
// 撞到墙就缩短，离开墙再慢慢伸回去。这一条解决了第三人称最常见的
// 两个毛病：镜头穿墙、贴墙时角色被自己的镜头挡住。
//
// 实现刻意只用已有的 `PhysicsWorld::raycast`（不新增物理接口）：
//   从枢轴沿 -forward 打一条射线，长度 = 期望臂长 + 相机半径
//   命中 → 臂长 = max(minDistance, 命中距离 - 相机半径)
// "+ 相机半径"是给相机本身留体积：射线打到墙上时，相机中心还要退开
// 一个半径，否则近裁剪面会切进墙里（看起来像墙被剖开）。
// ============================================================

#include "physics/RigidBody.h"

#include <glm/glm.hpp>

namespace physics {
class PhysicsWorld;
}

namespace runtime {

struct SpringArmResult {
    glm::vec3 position{0.0f};   // 相机应该在哪
    float distance = 0.0f;      // 实际臂长
    bool blocked = false;       // 是否因为障碍缩短了
};

// pivot：枢轴（角色头部）；dir：**从枢轴指向相机**的单位向量；
// desiredDistance：期望臂长；radius：相机半径（通常 0.2~0.3 m）。
SpringArmResult resolveSpringArm(const physics::PhysicsWorld* world,
                                 const glm::vec3& pivot,
                                 const glm::vec3& dir, float desiredDistance,
                                 float minDistance, float radius,
                                 physics::BodyId ignore);

} // namespace runtime
