#pragma once
// ============================================================
// physics/PhysicsShape —— 物理形状（刚体用什么几何参与仿真）
//
// ------------------------------------------------------------
// 为什么这里既有 Shape::Kind 又有 ConvexShape::Kind
// ------------------------------------------------------------
// 凸体（球 / 盒 / 胶囊 / 凸包）全部委托给 **ConvexShape** —— 因为 GJK/EPA
// 只认"支撑函数"，而那套已经写好了（见 ConvexCollision.h），再抄一份必然
// 会漂。所以这里不重复实现任何凸体数学。
//
// 但**平面（Plane）不是凸体**：它是无限半空间，没有 AABB、没有体积、
// 支撑点也不存在（沿 -n 方向最远点是负无穷）。把它塞进 ConvexShape 会让
// supportLocal 返回一个无意义的有限点，从而静默污染 GJK 的结果。
// 所以平面在本文件里走**解析路径**（见 Narrowphase 的 plane-vs-X），
// 并由 ShapeKind 单独标出来。
//
// ------------------------------------------------------------
// 三个约定（和 PhysicsMath.h 一致，别搞混）
// ------------------------------------------------------------
//   · 世界是 **Z-up**：胶囊轴 = 局部 +Z，平面默认法线 (0,0,1)。
//   · 平面方程 dot(n, x) = offset，n 是**单位**局部法线；offset > 0 表示
//     平面沿 +n 偏移。地面 = n=(0,0,1)、offset=0。
//   · 半边 / 半高：Box 的 halfExtents、Capsule 的 halfHeight 都是**一半**。
// ============================================================

#include "physics/ConvexShape.h"
#include "physics/PhysicsMath.h"

#include <glm/glm.hpp>

#include <vector>

namespace physics {

enum class ShapeKind : int {
    Sphere = 0,
    Box = 1,
    Capsule = 2,
    ConvexHull = 3,
    Plane = 4,     // 无限半空间（静态地面 / 墙面）
};

struct Shape {
    ShapeKind kind = ShapeKind::Sphere;

    // ---- 凸体路径（Sphere / Box / Capsule / ConvexHull）----
    // 形状尺寸都在这里（radius / halfExtended / hull 顶点）。
    ConvexShape convex;

    // ---- 平面路径（Plane）----
    glm::vec3 planeNormal{0.0f, 0.0f, 1.0f};  // 局部单位法线
    float planeOffset = 0.0f;                 // dot(n, x) = offset

    // ------------------------------------------------ 工厂
    static Shape sphere(float radius);
    static Shape box(const glm::vec3& halfExtents);
    static Shape capsule(float radius, float halfHeight);
    static Shape hull(std::vector<glm::vec3> points);
    static Shape plane(const glm::vec3& normal, float offset = 0.0f);

    bool isPlane() const { return kind == ShapeKind::Plane; }
    bool valid() const;

    // ------------------------------------------------ 几何查询
    // 局部 AABB。**平面**返回一个"极小"的退化盒（而不是无穷大）——
    // 宽相不靠它做平面的剔除（平面走专门的一对多路径，见 Broadphase）。
    void localAabb(glm::vec3& mn, glm::vec3& mx) const;

    // 沿 dir 最远的**局部**点。平面没有支撑点 → 返回 n*offset 处的点
    // （调用方必须自己保证：只有非平面才会走到这里）。
    glm::vec3 supportLocal(const glm::vec3& dir) const;

    // 体积（m³）。平面返回 0。用于"给定密度算质量"。
    float volume() const;

    // ------------------------------------------------ 质量属性
    // 给定**密度**算质量与惯量张量（局部、绕质心）。
    MassProperties massFromDensity(float density) const;
    // 给定**总质量**算惯量张量（局部、绕质心）。
    MassProperties massFromMass(float mass) const;

    // ------------------------------------------------ 世界 AABB
    // 把局部 AABB 的 8 个角变换过去取包围盒。平面 → 返回 false
    // （无穷大，无法表示；调用方必须单独处理平面）。
    bool worldAabb(const glm::mat4& xform, glm::vec3& mn, glm::vec3& mx) const;

    // 转成 GJK/EPA 认的"形状 + 世界变换"。⚠ 返回的结构持有指向
    // `this->convex` 的指针，所以**只能在本对象存活期内使用**。
    TransformedShape transformed(const glm::mat4& xform) const {
        TransformedShape ts;
        ts.shape = &convex;
        ts.xform = xform;
        return ts;
    }
};

// ---------------------------------------------------------------- 材质
//
// 摩擦 / 弹性。**不做**成刚体字段而是一个可复用的小结构，是因为
// "木头表面"这种材质会被很多物体共用，改一处应该全都变。
struct PhysicsMaterial {
    // 静止摩擦（物体已经贴合时抵抗起滑的力）与动摩擦（滑起来之后的阻力）。
    // 分开是必要的：只用动摩擦的话，斜坡上的箱子会缓慢地"自己往下滑"
    //（因为静摩擦不足时求解器每帧都在放一点位移）。
    float staticFriction = 0.6f;
    float dynamicFriction = 0.4f;
    // 弹性恢复系数：0 = 完全非弹性（落地不弹），1 = 完全弹性。
    float restitution = 0.0f;
    // 密度（kg/m³）。默认 1000 ≈ 接近水的密度，"1 米的箱子 = 1 吨"太大，
    // 所以默认取 500 —— 一个 1 m 的立方体约 250 kg，手感正常。
    float density = 500.0f;
};

// 两个材质接触时的**组合**规则（Bullet/PhysX 的常见默认）：
//   · 摩擦：几何平均（sqrt(a*b)）—— 一边"冰面"就能让整体很滑
//   · 弹性：取**大**者 —— 只要有一个物体是弹的，反弹就该发生
//     （取小者会让"弹力球掉在橡胶垫上完全不弹"，反直觉）
inline float combineFriction(float a, float b) {
    return std::sqrt(std::max(0.0f, a) * std::max(0.0f, b));
}
inline float combineRestitution(float a, float b) {
    return std::max(a, b);
}

} // namespace physics
