#include "render/Frustum.h"

namespace render {

namespace {

// glm 是**列主序**：m[col][row]。
// 因此矩阵的第 r 行 = (m[0][r], m[1][r], m[2][r], m[3][r])。
// Gribb–Hartmann 的推导全部建立在"行"上，这里先把四行取出来。
inline glm::vec4 matrixRow(const glm::mat4& m, int r) {
    return {m[0][r], m[1][r], m[2][r], m[3][r]};
}

}  // namespace

Frustum extractFrustum(const glm::mat4& viewProj) {
    const glm::vec4 r0 = matrixRow(viewProj, 0);  // x 行
    const glm::vec4 r1 = matrixRow(viewProj, 1);  // y 行
    const glm::vec4 r2 = matrixRow(viewProj, 2);  // z 行
    const glm::vec4 r3 = matrixRow(viewProj, 3);  // w 行

    // 裁剪空间约束（NDC z ∈ [-1,1]）：
    //   -w ≤ x ≤ w  →  left:  w + x ≥ 0   right: w - x ≥ 0
    //   -w ≤ y ≤ w  →  bottom: w + y ≥ 0  top:   w - y ≥ 0
    //   -w ≤ z ≤ w  →  near:  w + z ≥ 0   far:   w - z ≥ 0
    const glm::vec4 raw[6] = {
        r3 + r0,  // left
        r3 - r0,  // right
        r3 + r1,  // bottom
        r3 - r1,  // top
        r3 + r2,  // near
        r3 - r2,  // far
    };

    Frustum f;
    for (int i = 0; i < 6; ++i) {
        const glm::vec4 p = raw[i];
        const float len = glm::length(glm::vec3(p));
        if (len > 1e-8f) {
            // 归一化：注意 d 也要跟着除，否则 distance() 的量纲不对
            // （对剔除本身无所谓——只看符号——但归一化后它就是"到平面的
            //  真实距离"，调试与后续按距离分级剔除都用得上）
            f.planes[i].n = glm::vec3(p) / len;
            f.planes[i].d = p.w / len;
        } else {
            // 退化矩阵（例如 proj 未初始化）：给一个永不判出局的平面，
            // 让剔除退化成"什么都不剔"，而不是把整个场景清空。
            f.planes[i].n = glm::vec3(0.0f, 0.0f, 1.0f);
            f.planes[i].d = 1e30f;
        }
    }
    return f;
}

bool Frustum::intersects(const glm::vec3& lo, const glm::vec3& hi) const {
    for (const Plane& p : planes) {
        // "正顶点" = 沿平面法线方向最远的那个角点。
        // 法线指向视锥内部，所以正顶点是整个盒子里最可能"在内部"的点：
        // 连它都在平面外侧，盒子就完全在外侧 → 剔除。
        const glm::vec3 v{
            p.n.x >= 0.0f ? hi.x : lo.x,
            p.n.y >= 0.0f ? hi.y : lo.y,
            p.n.z >= 0.0f ? hi.z : lo.z,
        };
        if (p.distance(v) < 0.0f) return false;
    }
    return true;
}

bool Frustum::intersectsSphere(const glm::vec3& center, float radius) const {
    for (const Plane& p : planes) {
        // 球心到平面的有符号距离 < -r → 整个球都在外侧
        if (p.distance(center) < -radius) return false;
    }
    return true;
}

} // namespace render
