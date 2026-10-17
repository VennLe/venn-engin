#include "SpringArm.h"

#include "physics/PhysicsWorld.h"

#include <algorithm>
#include <cmath>

namespace runtime {

SpringArmResult resolveSpringArm(const physics::PhysicsWorld* world,
                                 const glm::vec3& pivot,
                                 const glm::vec3& dir, float desiredDistance,
                                 float minDistance, float radius,
                                 physics::BodyId ignore) {
    SpringArmResult out;
    out.distance = std::max(desiredDistance, 0.0f);

    if (!world || out.distance < 1.0e-4f) {
        out.position = pivot + dir * out.distance;
        return out;
    }

    const float len = glm::length(dir);
    const glm::vec3 d = (len > 1.0e-6f) ? (dir / len) : glm::vec3(0.0f, -1.0f, 0.0f);

    // 多探一个相机半径：见头文件里"给相机本身留体积"的说明。
    const physics::RayHit hit = world->raycast(pivot, d, out.distance + radius,
                                               ignore);
    if (hit.hit && hit.distance < out.distance + radius) {
        const float wanted = hit.distance - radius;
        // 夹到 [minDistance, desired]：比 minDistance 还近时宁可"穿一点"，
        // 也不要让相机贴到角色脸上（那看着像贴图错误）。
        out.distance = std::clamp(wanted, std::min(minDistance, out.distance),
                                  out.distance);
        out.blocked = out.distance < desiredDistance - 1.0e-4f;
    }

    out.position = pivot + d * out.distance;
    return out;
}

} // namespace runtime
