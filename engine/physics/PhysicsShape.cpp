#include "physics/PhysicsShape.h"

#include <algorithm>
#include <cmath>

namespace physics {

// ---------------------------------------------------------------- 工厂

Shape Shape::sphere(float radius) {
    Shape s;
    s.kind = ShapeKind::Sphere;
    s.convex.kind = ConvexShape::Kind::Sphere;
    s.convex.radius = std::max(radius, 1.0e-4f);
    return s;
}

Shape Shape::box(const glm::vec3& halfExtents) {
    Shape s;
    s.kind = ShapeKind::Box;
    s.convex.kind = ConvexShape::Kind::Box;
    // 下限：0 厚度的盒子在 SAT 里会让某个轴的分隔距离恒为 0 → 法线选错。
    // 1 mm 是"薄板"的合理下界，不影响正常使用。
    s.convex.halfExtents = glm::max(halfExtents, glm::vec3(1.0e-3f));
    return s;
}

Shape Shape::capsule(float radius, float halfHeight) {
    Shape s;
    s.kind = ShapeKind::Capsule;
    s.convex.kind = ConvexShape::Kind::Capsule;
    s.convex.radius = std::max(radius, 1.0e-4f);
    s.convex.halfHeight = std::max(halfHeight, 0.0f);
    return s;
}

Shape Shape::hull(std::vector<glm::vec3> points) {
    Shape s;
    s.kind = ShapeKind::ConvexHull;
    s.convex.kind = ConvexShape::Kind::Hull;
    s.convex.hull = std::move(points);
    return s;
}

Shape Shape::plane(const glm::vec3& normal, float offset) {
    Shape s;
    s.kind = ShapeKind::Plane;
    const float len = glm::length(normal);
    // 零法线没有意义 → 退回 +Z（世界是 Z-up，"默认地面"就是这个朝向）。
    s.planeNormal = (len > kEpsilon) ? normal / len : glm::vec3(0.0f, 0.0f, 1.0f);
    s.planeOffset = offset;
    return s;
}

bool Shape::valid() const {
    switch (kind) {
        case ShapeKind::Sphere:     return convex.radius > 0.0f;
        case ShapeKind::Box:        return glm::all(glm::greaterThan(
                                               convex.halfExtents, glm::vec3(0.0f)));
        case ShapeKind::Capsule:    return convex.radius > 0.0f;
        case ShapeKind::ConvexHull: return convex.hull.size() >= 4;
        case ShapeKind::Plane:      return true;
    }
    return false;
}

// ---------------------------------------------------------------- 几何查询

void Shape::localAabb(glm::vec3& mn, glm::vec3& mx) const {
    if (isPlane()) {
        // 平面没有 AABB。给一个**退化点**而不是无穷大：无穷大会让
        // "min/max" 运算里出现 NaN（inf - inf），而宽相本来就不应该把
        // 平面喂进来（见 Broadphase：平面走专门的一对多路径）。
        mn = mx = planeNormal * planeOffset;
        return;
    }
    convex.localAabb(mn, mx);
}

glm::vec3 Shape::supportLocal(const glm::vec3& dir) const {
    if (isPlane()) {
        // 理论上不会被调用。返回平面上的一个点，至少不会产生 NaN —— 真要
        // 被调用了，结果是"错但有限"，比"静默产生 inf 污染整条链路"好排查。
        return planeNormal * planeOffset;
    }
    return convex.supportLocal(dir);
}

float Shape::volume() const {
    switch (kind) {
        case ShapeKind::Sphere: {
            const float r = convex.radius;
            return (4.0f / 3.0f) * kPi * r * r * r;
        }
        case ShapeKind::Box: {
            const glm::vec3 e = convex.halfExtents * 2.0f;
            return e.x * e.y * e.z;
        }
        case ShapeKind::Capsule: {
            const float r = convex.radius;
            const float h = convex.halfHeight * 2.0f;
            return kPi * r * r * h + (4.0f / 3.0f) * kPi * r * r * r;
        }
        case ShapeKind::ConvexHull: {
            // 凸包体积 = 以原点为顶点的四面体体积之和（散度定理）。
            // 顶点**已经居中**（见 ConvexShape 的说明），所以这个公式成立。
            // 用面而不是顶点：需要 ConvexShape 的 hullFaces，而物理侧的
            // Shape::hull 只搬了顶点 —— 所以这里退化成 AABB 体积 × 0.6。
            //
            // 为什么可以接受：凸包是编辑器从网格生成的，它的质量只需要
            // "量级正确"（密度 × 体积），0.6 的经验系数对规则网格足够；
            // 真要精确体积，应该在做 QuickHull 时顺手算出来（那是几何侧
            // 的活，不该由物理重复一遍）。
            glm::vec3 mn, mx;
            localAabb(mn, mx);
            const glm::vec3 e = mx - mn;
            return e.x * e.y * e.z * 0.6f;
        }
        case ShapeKind::Plane:
            return 0.0f;
    }
    return 0.0f;
}

// ---------------------------------------------------------------- 质量属性

MassProperties Shape::massFromMass(float mass) const {
    if (mass <= 0.0f) return makeMassProperties(0.0f, glm::mat3(0.0f));
    switch (kind) {
        case ShapeKind::Sphere:
            return makeMassProperties(mass, inertiaSphere(mass, convex.radius));
        case ShapeKind::Box:
            return makeMassProperties(mass,
                                      inertiaBox(mass, convex.halfExtents));
        case ShapeKind::Capsule:
            return makeMassProperties(
                mass, inertiaCapsule(mass, convex.radius, convex.halfHeight));
        case ShapeKind::ConvexHull:
        case ShapeKind::Plane: {
            // 凸包的惯量张量需要逐四面体累加（和体积同一套积分）。
            // 这里退化成"等效长方体"——对"碰撞检测正确、转动略钝"的
            // 手搓刚体够用，且不会因为退化凸包（共面点）算出 NaN。
            glm::vec3 mn, mx;
            localAabb(mn, mx);
            const glm::vec3 he = glm::max((mx - mn) * 0.5f, glm::vec3(1.0e-3f));
            return makeMassProperties(mass, inertiaBox(mass, he));
        }
    }
    return makeMassProperties(mass, inertiaSphere(mass, 0.5f));
}

MassProperties Shape::massFromDensity(float density) const {
    const float m = std::max(density, 0.0f) * volume();
    return massFromMass(m);
}

// ---------------------------------------------------------------- 世界 AABB

bool Shape::worldAabb(const glm::mat4& xform, glm::vec3& mn,
                      glm::vec3& mx) const {
    if (isPlane()) return false;
    glm::vec3 lmn, lmx;
    localAabb(lmn, lmx);
    bool first = true;
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 corner((i & 1) ? lmx.x : lmn.x,
                               (i & 2) ? lmx.y : lmn.y,
                               (i & 4) ? lmx.z : lmn.z);
        const glm::vec3 w = glm::vec3(xform * glm::vec4(corner, 1.0f));
        if (first) {
            mn = mx = w;
            first = false;
        } else {
            mn = glm::min(mn, w);
            mx = glm::max(mx, w);
        }
    }
    return true;
}

} // namespace physics
