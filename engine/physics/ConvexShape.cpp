#include "physics/ConvexShape.h"

#include <algorithm>
#include <cmath>

namespace physics {

glm::vec3 ConvexShape::supportLocal(const glm::vec3& dir) const {
    switch (kind) {
        case Kind::Hull: {
            if (hull.empty()) return glm::vec3(0.0f);
            std::size_t best = 0;
            float bestDot = glm::dot(hull[0], dir);
            for (std::size_t i = 1; i < hull.size(); ++i) {
                const float dt = glm::dot(hull[i], dir);
                if (dt > bestDot) {
                    bestDot = dt;
                    best = i;
                }
            }
            return hull[best];
        }
        case Kind::Sphere: {
            const float len = glm::length(dir);
            // 零方向：随便给一个球面上的点（GJK 只在极端退化时才会遇到）
            if (len < 1e-8f) return glm::vec3(radius, 0.0f, 0.0f);
            return dir * (radius / len);
        }
        case Kind::Capsule:
        default: {
            const float len = glm::length(dir);
            const glm::vec3 d =
                (len < 1e-8f) ? glm::vec3(1.0f, 0.0f, 0.0f) : dir / len;
            // 圆柱段的两端：沿 +Z 取靠 dir 那一端
            const glm::vec3 seg = (dir.z >= 0.0f)
                                      ? glm::vec3(0.0f, 0.0f, halfHeight)
                                      : glm::vec3(0.0f, 0.0f, -halfHeight);
            return seg + d * radius;
        }
    }
}

void ConvexShape::localAabb(glm::vec3& mn, glm::vec3& mx) const {
    switch (kind) {
        case Kind::Hull: {
            if (hull.empty()) {
                mn = mx = glm::vec3(0.0f);
                return;
            }
            mn = mx = hull[0];
            for (const glm::vec3& p : hull) {
                mn = glm::min(mn, p);
                mx = glm::max(mx, p);
            }
            return;
        }
        case Kind::Sphere:
            mn = glm::vec3(-radius);
            mx = glm::vec3(radius);
            return;
        case Kind::Capsule:
        default:
            mn = glm::vec3(-radius, -radius, -halfHeight - radius);
            mx = glm::vec3(radius, radius, halfHeight + radius);
            return;
    }
}

glm::vec3 TransformedShape::support(const glm::vec3& worldDir) const {
    if (!shape) return glm::vec3(0.0f);
    const glm::mat3 dirXform = glm::transpose(glm::mat3(xform));
    const glm::vec3 local = shape->supportLocal(dirXform * worldDir);
    return glm::vec3(xform * glm::vec4(local, 1.0f));
}

void TransformedShape::worldAabb(glm::vec3& mn, glm::vec3& mx) const {
    if (!shape) {
        mn = mx = glm::vec3(0.0f);
        return;
    }
    glm::vec3 lmn, lmx;
    shape->localAabb(lmn, lmx);
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
}

glm::vec3 TransformedShape::center() const {
    if (!shape) return glm::vec3(0.0f);
    glm::vec3 lmn, lmx;
    shape->localAabb(lmn, lmx);
    return glm::vec3(xform * glm::vec4((lmn + lmx) * 0.5f, 1.0f));
}

} // namespace physics
