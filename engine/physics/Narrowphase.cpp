#include "physics/Narrowphase.h"

#include "physics/ConvexCollision.h"

#include <algorithm>
#include <cmath>

namespace physics {

namespace {

// ================================================================
//  小工具
// ================================================================

// 点 p 在线段 [a,b] 上的最近点参数 t ∈ [0,1]
float closestParamOnSegment(const glm::vec3& a, const glm::vec3& b,
                           const glm::vec3& p) {
    const glm::vec3 ab = b - a;
    const float len2 = glm::dot(ab, ab);
    if (len2 < kEpsilon) return 0.0f;
    return glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f);
}

// 两条线段的最近点对（Ericson, Real-Time Collision Detection §5.1.9）。
// 盒-盒的**边-边**情况会用到：那种配置下没有可裁剪的面，只有一个点。
void closestPointsBetweenSegments(const glm::vec3& p1, const glm::vec3& q1,
                                  const glm::vec3& p2, const glm::vec3& q2,
                                  glm::vec3& c1, glm::vec3& c2) {
    const glm::vec3 d1 = q1 - p1;
    const glm::vec3 d2 = q2 - p2;
    const glm::vec3 r = p1 - p2;
    const float a = glm::dot(d1, d1);
    const float e = glm::dot(d2, d2);
    const float f = glm::dot(d2, r);
    float s = 0.0f, t = 0.0f;
    if (a <= kEpsilon && e <= kEpsilon) {
        c1 = p1;
        c2 = p2;
        return;
    }
    if (a <= kEpsilon) {
        t = glm::clamp(f / e, 0.0f, 1.0f);
    } else {
        const float cc = glm::dot(d1, r);
        if (e <= kEpsilon) {
            s = glm::clamp(-cc / a, 0.0f, 1.0f);
        } else {
            const float b = glm::dot(d1, d2);
            const float denom = a * e - b * b;
            if (denom > kEpsilon) {
                s = glm::clamp((b * f - cc * e) / denom, 0.0f, 1.0f);
            }
            t = (b * s + f) / e;
            if (t < 0.0f) {
                t = 0.0f;
                s = glm::clamp(-cc / a, 0.0f, 1.0f);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = glm::clamp((b - cc) / a, 0.0f, 1.0f);
            }
        }
    }
    c1 = p1 + d1 * s;
    c2 = p2 + d2 * t;
}

// ================================================================
//  世界空间的形状视图
// ================================================================

struct WorldBox {
    glm::vec3 c{0.0f};
    // 三列是局部 X/Y/Z 轴在世界空间的方向（列主序，与 glm 一致）
    glm::mat3 axes{1.0f};
    glm::vec3 he{0.5f};

    glm::vec3 vertex(int i) const {
        return c + axes[0] * ((i & 1) ? he.x : -he.x) +
               axes[1] * ((i & 2) ? he.y : -he.y) +
               axes[2] * ((i & 4) ? he.z : -he.z);
    }
};

WorldBox makeBox(const RigidBody& b) {
    WorldBox w;
    w.c = b.position;
    w.axes = glm::mat3_cast(b.rotation);
    w.he = b.shape.convex.halfExtents;
    return w;
}

struct WorldCapsule {
    glm::vec3 a{0.0f};   // 圆柱段的下端（-Z 端，世界空间）
    glm::vec3 b{0.0f};
    float r = 0.5f;
};

WorldCapsule makeCapsule(const RigidBody& rb) {
    WorldCapsule w;
    const glm::vec3 axis = rb.rotation * glm::vec3(0.0f, 0.0f, 1.0f);
    const float hh = rb.shape.convex.halfHeight;
    w.r = rb.shape.convex.radius;
    w.a = rb.position - axis * hh;
    w.b = rb.position + axis * hh;
    return w;
}

// 把流形归一化成"小于等于 4 个**最深**的点"。
// 盒-盒裁剪最多能产出 8 个点（4 个侧面 × 前后），全留下会让求解器
// 每帧多做一倍迭代，而 4 个点已经能撑住任何稳定姿态。
void trimToDeepest(ContactManifold& m) {
    if (m.count <= kMaxManifoldPoints) return;
    std::sort(m.points, m.points + m.count,
              [](const ContactPoint& x, const ContactPoint& y) {
                  return x.depth > y.depth;
              });
    m.count = kMaxManifoldPoints;
}

// 追加一个接触点。featureId 允许为 0（= 不做热启动）。
void addPoint(ContactManifold& m, const glm::vec3& pos, float depth,
              std::uint64_t featureId) {
    if (m.count >= kMaxManifoldCapacity) return;
    ContactPoint& p = m.points[m.count++];
    p.position = pos;
    p.depth = depth;
    p.featureId = featureId;
}

// ================================================================
//  各形状对的特例
// ================================================================

// ---- 球-球 ----
void sphereSphere(const RigidBody& A, const RigidBody& B, ContactManifold& m) {
    const glm::vec3 d = B.position - A.position;
    const float rSum = A.shape.convex.radius + B.shape.convex.radius;
    const float dist2 = glm::dot(d, d);
    if (dist2 > rSum * rSum) return;

    const float dist = std::sqrt(std::max(dist2, 0.0f));
    // 同心：法线无定义 → 取一个确定方向（+Z），保证同一输入总是同一输出
    //（要确定性，不能取随机方向）。
    m.normal = (dist > kEpsilon) ? d / dist : glm::vec3(0.0f, 0.0f, 1.0f);
    const float depth = rSum - dist;
    addPoint(m, A.position + m.normal * (A.shape.convex.radius - depth * 0.5f),
             depth, 0);
}

// ---- 球-盒（球是 A，盒是 B）----
//
// flip 只影响**输出法线的朝向**（内部一律按"盒 → 球"算），所以调用方
// 不必关心自己传进来的顺序。contact 位置统一取**盒面上离球心最近的点** ——
// 对外接触（球陷进面里）和内接触（球心在盒内）用同一个定义，少一个分支。
void sphereBox(const RigidBody& SA, const RigidBody& SB, ContactManifold& m,
               bool flip) {
    const WorldBox box = makeBox(SB);
    const glm::vec3 c = SA.position;
    const float r = SA.shape.convex.radius;

    // 把球心换到盒的局部空间，夹到半边长内 → 盒上最近点
    const glm::vec3 rel = c - box.c;
    glm::vec3 local(glm::dot(rel, box.axes[0]), glm::dot(rel, box.axes[1]),
                    glm::dot(rel, box.axes[2]));
    const glm::vec3 clamped = glm::clamp(local, -box.he, box.he);
    glm::vec3 closest = box.c + box.axes[0] * clamped.x +
                        box.axes[1] * clamped.y + box.axes[2] * clamped.z;

    glm::vec3 n;   // 从**盒**指向**球**
    float depth = 0.0f;
    const bool inside = (clamped == local);
    if (!inside) {
        const glm::vec3 d = c - closest;
        const float dist = glm::length(d);
        if (dist > r) return;
        // dist ≈ 0（球心正好落在面上）：法线退化 → 取 +Z，保证确定性。
        n = (dist > kEpsilon) ? d / dist : glm::vec3(0.0f, 0.0f, 1.0f);
        depth = r - dist;
    } else {
        // 球心在盒**内部**：最近点就是球心自己，上面那套算不出方向。
        // 取"最浅的那一面"把球推出去 —— 这就是"球陷进盒子里自动弹出"。
        int bestAxis = 0;
        float bestDist = box.he.x - std::fabs(local.x);
        for (int i = 1; i < 3; ++i) {
            const float dd = box.he[i] - std::fabs(local[i]);
            if (dd < bestDist) {
                bestDist = dd;
                bestAxis = i;
            }
        }
        n = box.axes[bestAxis] * (local[bestAxis] >= 0.0f ? 1.0f : -1.0f);
        depth = bestDist + r;
        closest = box.c + n * box.he[bestAxis];   // 该面在盒面上的投影点
    }

    // 内部约定是"盒 → 球"；A 侧是球，所以不 flip 时法线要指向盒（取反）。
    m.normal = flip ? n : -n;
    addPoint(m, closest, depth, 0);
}

// ---- 盒-盒：SAT + 参考面 / 入射面裁剪 ----
//
// 这是整个窄相里唯一"真正难"的部分，也是箱堆稳定的关键。流程：
//   1. SAT 在 15 根轴上找**最小穿透**的那根（3+3 个面法线 + 9 个边叉积）
//   2. 若赢家是面法线 → 参考面在那一侧，入射面在对面；裁剪出最多 4 个点
//   3. 若赢家是边叉积 → 只有一条棱棱接触，取两条棱的最近点对（1 个点）
void boxBox(const RigidBody& A, const RigidBody& B, ContactManifold& m) {
    const WorldBox a = makeBox(A);
    const WorldBox b = makeBox(B);

    // 旋转矩阵 R[i][j] = dot(a.axes[i], b.axes[j])
    float R[3][3];
    float AbsR[3][3];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            R[i][j] = glm::dot(a.axes[i], b.axes[j]);
            // +eps：两盒近乎平行时 AbsR 会接近 0，加 eps 让"A 的面法线"
            // 不会被误判成"边叉积轴"，从而避免退化法线。
            AbsR[i][j] = std::fabs(R[i][j]) + 1.0e-5f;
        }
    }
    const glm::vec3 tWorld = b.c - a.c;
    const float t[3] = {glm::dot(tWorld, a.axes[0]), glm::dot(tWorld, a.axes[1]),
                        glm::dot(tWorld, a.axes[2])};

    float bestPen = 1.0e30f;
    glm::vec3 bestAxis(0.0f, 0.0f, 1.0f);
    int bestFaceIsA = -1;   // >=0 → 赢家是 A 的第 i 个面；-1 → B；-2 → 边叉积
    int bestFaceIdx = 0;
    int bestEdgeA = 0, bestEdgeB = 0;

    auto consider = [&](float pen, const glm::vec3& axis, int faceIsA,
                        int faceIdx, int ea, int eb) {
        if (pen < bestPen) {
            bestPen = pen;
            bestAxis = axis;
            bestFaceIsA = faceIsA;
            bestFaceIdx = faceIdx;
            bestEdgeA = ea;
            bestEdgeB = eb;
        }
    };

    // ---- A 的 3 个面法线 ----
    for (int i = 0; i < 3; ++i) {
        const float ra = a.he[i];
        const float rb = b.he[0] * AbsR[i][0] + b.he[1] * AbsR[i][1] +
                         b.he[2] * AbsR[i][2];
        const float d = std::fabs(t[i]);
        const float pen = ra + rb - d;
        if (pen < 0.0f) return;   // 分离轴 → 不相交
        consider(pen, a.axes[i], 0, i, -1, -1);
    }
    // ---- B 的 3 个面法线 ----
    for (int j = 0; j < 3; ++j) {
        const float ra = a.he[0] * AbsR[0][j] + a.he[1] * AbsR[1][j] +
                         a.he[2] * AbsR[2][j];
        const float rb = b.he[j];
        const float d = std::fabs(glm::dot(tWorld, b.axes[j]));
        const float pen = ra + rb - d;
        if (pen < 0.0f) return;
        consider(pen, b.axes[j], 1, j, -1, -1);
    }
    // ---- 9 个边叉积 ----
    // 面法线已经很占优势（pen 明显更小）时可以跳过 —— 但为了正确性
    // （斜着架在棱上的盒子）这里还是全算一遍，代价是 9 次 cross。
    //
    // ⚠⚠ 这里的**尺度**极其容易写错，是本模块最难发现的一处 bug：
    //   `ra` / `rb` 用的公式（Ericson 式 4.26）推导时用了"L 是单位向量"
    //   这个前提，等价于它算出来的是 `真实投影半径 × |L|`。所以分离量也
    //   必须用**未归一化**的叉积：`d = |t · (a×b)|`。
    //   若图省事把 L 归一化之后再点乘（`d = |t · L̂|`），就变成"拿真实
    //   距离和缩放了 |L| 倍的距离作比较" —— |L| 越小，误差越大，最终
    //   pen 算成一个大负数，函数在这里直接 return "分离"。
    //   **实测症状**：一叠轴对齐的箱子，只要因为求解器抖动出 ~0.1° 的
    //   相对旋转，箱-箱接触就**全部消失**（宽相明明配对了 540 对、自检
    //   报 0），整叠塌到地面上；而箱-地面走的是解析路径，完全正常 ——
    //   于是看起来像"地面没问题、箱箱碰撞没实现"，其实只差这一次除法。
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            const glm::vec3 crossRaw = glm::cross(a.axes[i], b.axes[j]);
            const float len = glm::length(crossRaw);
            // 两轴近乎平行 → 叉积退化，这根轴不成立。len = sin(夹角)，
            // 阈值不能取太小：len 越小，ra/rb 的数值误差被放大得越厉害。
            if (len < 1.0e-4f) continue;
            // 沿该轴的投影半径（已含 |L| 因子，见上面的说明）
            const float ra = a.he[(i + 1) % 3] * AbsR[(i + 2) % 3][j] +
                             a.he[(i + 2) % 3] * AbsR[(i + 1) % 3][j];
            const float rb = b.he[(j + 1) % 3] * AbsR[i][(j + 2) % 3] +
                             b.he[(j + 2) % 3] * AbsR[i][(j + 1) % 3];
            const float d = std::fabs(glm::dot(tWorld, crossRaw));
            // 除以 len 把结果换回"真实距离"，才能和面法线的 pen 放在一起比。
            const float pen = (ra + rb - d) / len;
            if (pen < 0.0f) return;
            consider(pen, crossRaw / len, -2, 0, i, j);
        }
    }

    // 法线方向统一成 A → B
    if (glm::dot(bestAxis, tWorld) < 0.0f) bestAxis = -bestAxis;

    // ---- 边-边：单点 ----
    //
    // 两条棱的选取：A 的棱方向 = a 的第 (i+1)%3 轴、靠在朝 B 那一侧的
    // ±he[i] 面上；B 的棱同理靠在朝 A 那一侧。取完直接求两条线段的最近点对。
    if (bestFaceIsA == -2) {
        const int i = bestEdgeA, j = bestEdgeB;
        const float sa = (glm::dot(b.c - a.c, a.axes[i]) >= 0.0f) ? 1.0f : -1.0f;
        const float sb = (glm::dot(a.c - b.c, b.axes[j]) >= 0.0f) ? 1.0f : -1.0f;
        const int iu = (i + 1) % 3;
        const int iv = (i + 2) % 3;
        const glm::vec3 p1 = a.c + a.axes[i] * (sa * a.he[i]) -
                             a.axes[iv] * a.he[iv];
        const glm::vec3 q1 = p1 + a.axes[iu] * (2.0f * a.he[iu]);
        const int ju = (j + 1) % 3;
        const int jv = (j + 2) % 3;
        const glm::vec3 p2 = b.c + b.axes[j] * (sb * b.he[j]) -
                             b.axes[jv] * b.he[jv];
        const glm::vec3 q2 = p2 + b.axes[ju] * (2.0f * b.he[ju]);
        glm::vec3 c1, c2;
        closestPointsBetweenSegments(p1, q1, p2, q2, c1, c2);
        m.normal = bestAxis;
        addPoint(m, (c1 + c2) * 0.5f, std::max(bestPen, 0.0f), 0);
        return;
    }

    // ---- 面-面：参考面裁剪 ----
    const bool refIsA = (bestFaceIsA == 0);
    const WorldBox& ref = refIsA ? a : b;
    const WorldBox& inc = refIsA ? b : a;
    const int axis = bestFaceIdx;

    // 参考面的两个切向轴与半宽
    const int u = (axis + 1) % 3;
    const int v = (axis + 2) % 3;
    // ⚠ **参考面的外法线**（从 ref 指向 inc 的那一侧），这是下面所有
    //   几何构造的唯一方向依据：
    //     · 参考面在 A 上 → 外法线 = +bestAxis（bestAxis 已统一成 A → B）
    //     · 参考面在 B 上 → 外法线 = **−bestAxis**（B 朝 A 的那一面）
    //   早先这里有两处各自硬编码的朝向，都是错的：
    //     ① `faceCenter = ref.c + bestAxis * he` —— B 当参考面时面心落到
    //        了背面，裁剪框整个错位；
    //     ② 入射面固定取"外法线与 bestAxis 最反向"的那个面 —— 这在
    //        refIsA 时是对的，但 refIsB 时应当相对 **−bestAxis** 取反向。
    //        否则入射面会选到 A 的**背面**（本该是 A 的正面），
    //        sep 算出来 ≈ −0.5 m，位置修正会把两个箱子猛地推开。
    //   症状：箱-箱接触的 penetration 突然报 0.5 m、箱子互相弹飞。
    const glm::vec3 refOutward = refIsA ? bestAxis : -bestAxis;
    const glm::vec3 faceCenter = ref.c + refOutward * ref.he[axis];

    // 入射面：取 inc 上**外法线与 refOutward 最反向**的那个面。
    // 每根轴有正负两个面，所以遍历 6 个候选，取 dot 最小者。
    int incAxis = 0;
    float incSign = 1.0f;
    float bestDot = 1.0e30f;
    for (int k = 0; k < 3; ++k) {
        const float dp = glm::dot(inc.axes[k], refOutward);
        if (dp < bestDot) { bestDot = dp; incAxis = k; incSign = 1.0f; }
        if (-dp < bestDot) { bestDot = -dp; incAxis = k; incSign = -1.0f; }
    }

    // 入射面的 4 个顶点（局部构造再变换到世界）
    const int iu = (incAxis + 1) % 3;
    const int iv = (incAxis + 2) % 3;
    const glm::vec3 incCenter =
        inc.c + inc.axes[incAxis] * (incSign * inc.he[incAxis]);
    glm::vec3 poly[8];
    poly[0] = incCenter + inc.axes[iu] * inc.he[iu] + inc.axes[iv] * inc.he[iv];
    poly[1] = incCenter - inc.axes[iu] * inc.he[iu] + inc.axes[iv] * inc.he[iv];
    poly[2] = incCenter - inc.axes[iu] * inc.he[iu] - inc.axes[iv] * inc.he[iv];
    poly[3] = incCenter + inc.axes[iu] * inc.he[iu] - inc.axes[iv] * inc.he[iv];

    // 用参考面的 4 个侧平面裁掉多边形的外部（Sutherland-Hodgman）。
    // 侧平面沿 ±ref.axes[u]、±ref.axes[v]，都过 faceCenter，半宽是 ref.he。
    int n = 4;
    glm::vec3 buf[8];
    for (int side = 0; side < 4 && n > 0; ++side) {
        const glm::vec3 nrm = (side == 0)   ? ref.axes[u]
                              : (side == 1) ? -ref.axes[u]
                              : (side == 2) ? ref.axes[v]
                                            : -ref.axes[v];
        const float half = (side < 2) ? ref.he[u] : ref.he[v];
        const float off = glm::dot(nrm, faceCenter) + half;

        int outN = 0;
        for (int i = 0; i < n; ++i) {
            const glm::vec3 cur = poly[i];
            const glm::vec3 nxt = poly[(i + 1) % n];
            const float dc = glm::dot(nrm, cur) - off;
            const float dn = glm::dot(nrm, nxt) - off;
            if (dc <= 0.0f && outN < 8) buf[outN++] = cur;
            if ((dc < 0.0f) != (dn < 0.0f) && outN < 8) {
                const float s = dc / (dc - dn);
                buf[outN++] = cur + (nxt - cur) * s;
            }
        }
        n = std::min(outN, 8);
        for (int i = 0; i < n; ++i) poly[i] = buf[i];
    }

    m.normal = bestAxis;
    // ⚠ 分离量必须**沿参考面的外法线 refOutward 度量**，不能沿 bestAxis：
    //   · refIsA  → refOutward = +bestAxis，穿透时顶点在面**内**侧 → sep < 0
    //   · refIsB  → refOutward = −bestAxis，穿透时顶点在面**外**侧方向，
    //               若仍沿 bestAxis 度量，sep 会变成 **正数**，于是每一个
    //               入射顶点都被当成"在参考面外侧"丢弃 → 该对**完全没有
    //               接触点**。
    //   因为 A 面 / B 面的 pen 常常只差 1e-6（轴对齐堆叠时几乎相等），
    //   赢家会在两帧之间来回翻 —— 症状就是"箱-箱接触时有时无"，宏观上
    //   表现为整叠箱子周期性掉接触、持续下沉、最后塌掉。
    const float planeD = glm::dot(refOutward, faceCenter);
    for (int i = 0; i < n; ++i) {
        // 分离量 > 0 = 在参考面外侧（没接触）。留一点点正容差，让"刚好
        // 贴住"的构型也能生成接触点 —— 否则静置的箱子会掉进"生成接触 /
        // 不生成接触"的每帧抖动里。
        const float sep = glm::dot(refOutward, poly[i]) - planeD;
        if (sep > 1.0e-4f) continue;
        // 接触点放在"入射顶点"与它在参考面上的投影的中点
        const glm::vec3 onRef = poly[i] - refOutward * sep;
        addPoint(m, (poly[i] + onRef) * 0.5f, -sep,
                 // featureId：参考面轴 × 入射面轴 × 顶点序号 —— 只要
                 // "哪个面在碰哪个面"不变，id 就稳定，冲量可以继承。
                 (static_cast<std::uint64_t>(axis + 1) << 24) |
                     (static_cast<std::uint64_t>(incAxis + 1) << 20) |
                     static_cast<std::uint64_t>(i + 1));
    }
    trimToDeepest(m);
}

// ---- 胶囊-球（胶囊 A，球 B）----
void capsuleSphere(const RigidBody& CA, const RigidBody& SB,
                   ContactManifold& m) {
    const WorldCapsule cap = makeCapsule(CA);
    const float t = closestParamOnSegment(cap.a, cap.b, SB.position);
    const glm::vec3 closest = cap.a + (cap.b - cap.a) * t;
    const glm::vec3 d = SB.position - closest;
    const float rSum = cap.r + SB.shape.convex.radius;
    const float dist2 = glm::dot(d, d);
    if (dist2 > rSum * rSum) return;
    const float dist = std::sqrt(std::max(dist2, 0.0f));
    m.normal = (dist > kEpsilon) ? d / dist : glm::vec3(0.0f, 0.0f, 1.0f);
    const float depth = rSum - dist;
    addPoint(m, closest + m.normal * (cap.r - depth * 0.5f), depth, 0);
}

// ---- 胶囊-胶囊 ----
void capsuleCapsule(const RigidBody& A, const RigidBody& B, ContactManifold& m) {
    const WorldCapsule ca = makeCapsule(A);
    const WorldCapsule cb = makeCapsule(B);
    glm::vec3 p, q;
    closestPointsBetweenSegments(ca.a, ca.b, cb.a, cb.b, p, q);
    const glm::vec3 d = q - p;
    const float rSum = ca.r + cb.r;
    const float dist2 = glm::dot(d, d);
    if (dist2 > rSum * rSum) return;
    const float dist = std::sqrt(std::max(dist2, 0.0f));
    m.normal = (dist > kEpsilon) ? d / dist : glm::vec3(0.0f, 0.0f, 1.0f);
    const float depth = rSum - dist;
    addPoint(m, (p + q) * 0.5f, depth, 0);
}

// ---- 胶囊-盒：把胶囊退化成两端球，逐个对盒做球-盒 ----
void capsuleBox(const RigidBody& CA, const RigidBody& SB, ContactManifold& m,
                bool flip) {
    const WorldCapsule cap = makeCapsule(CA);
    RigidBody probe;
    probe.position = glm::vec3(0.0f);
    probe.shape.kind = ShapeKind::Sphere;
    probe.shape.convex.kind = ConvexShape::Kind::Sphere;
    probe.shape.convex.radius = cap.r;

    // 两个端点各当一次球。用**同一套** sphereBox 逻辑，避免两处实现漂移。
    const glm::vec3 ends[2] = {cap.a, cap.b};
    ContactManifold sub;
    int produced = 0;
    for (int i = 0; i < 2; ++i) {
        probe.position = ends[i];
        sub.count = 0;
        sphereBox(probe, SB, sub, flip);
        // 同一个盒面对两端可能都接触 → 只保留较深的一个点，
        // 否则两个点几乎重合，求解器会在它们之间来回分冲量（抖动）。
        for (int k = 0; k < sub.count && produced < kMaxManifoldPoints; ++k) {
            bool dup = false;
            for (int q = 0; q < m.count; ++q) {
                if (glm::distance(m.points[q].position,
                                  sub.points[k].position) < 0.02f) {
                    dup = true;
                    break;
                }
            }
            if (dup) continue;
            m.normal = sub.normal;
            m.points[m.count++] = sub.points[k];
            ++produced;
        }
    }
}

// ---- 平面 vs 任意（平面是 A）----
void planeVsBody(const RigidBody& PA, const RigidBody& B, ContactManifold& m) {
    const glm::vec3 n = PA.rotation * PA.shape.planeNormal;
    const float D = PA.shape.planeOffset + glm::dot(n, PA.position);
    m.normal = n;   // 平面 → 物体 = 平面法线方向

    switch (B.shape.kind) {
        case ShapeKind::Sphere: {
            const float r = B.shape.convex.radius;
            const float dist = glm::dot(n, B.position) - D;
            if (dist > r) return;
            const float depth = r - dist;
            addPoint(m, B.position - n * ((r + dist) * 0.5f), depth, 0);
            return;
        }
        case ShapeKind::Capsule: {
            const WorldCapsule cap = makeCapsule(B);
            const glm::vec3 ends[2] = {cap.a, cap.b};
            for (int i = 0; i < 2; ++i) {
                const float dist = glm::dot(n, ends[i]) - D;
                if (dist > cap.r) continue;
                const float depth = cap.r - dist;
                addPoint(m, ends[i] - n * ((cap.r + dist) * 0.5f), depth, 0);
            }
            return;
        }
        case ShapeKind::Box: {
            // 八个角里"在平面下方"的（最多 4 个，箱子的一个面最多 4 个角）
            const WorldBox box = makeBox(B);
            for (int i = 0; i < 8; ++i) {
                const glm::vec3 v = box.vertex(i);
                const float dist = glm::dot(n, v) - D;
                if (dist > 0.0f) continue;
                addPoint(m, v - n * (dist * 0.5f), -dist,
                         static_cast<std::uint64_t>(i + 1) << 8);
            }
            trimToDeepest(m);
            return;
        }
        case ShapeKind::ConvexHull: {
            const std::vector<glm::vec3>& pts = B.shape.convex.hull;
            const glm::mat3 R = glm::mat3_cast(B.rotation);
            for (const glm::vec3& p : pts) {
                const glm::vec3 v = B.position + R * p;
                const float dist = glm::dot(n, v) - D;
                if (dist > 0.0f) continue;
                addPoint(m, v - n * (dist * 0.5f), -dist, 0);
            }
            trimToDeepest(m);
            return;
        }
        case ShapeKind::Plane:
            return;   // 平面-平面：无解，也无意义
    }
}

// ---- 兜底：GJK/EPA 单点 ----
void genericConvex(const RigidBody& A, const RigidBody& B, ContactManifold& m) {
    const TransformedShape ta = A.shape.transformed(A.worldMatrix());
    const TransformedShape tb = B.shape.transformed(B.worldMatrix());
    const Contact c = collide(ta, tb);
    if (!c.hit || c.depth <= 0.0f) return;
    m.normal = c.normal;
    glm::vec3 pos = c.point;
    // ConvexCollision 在退化情况下可能给出非有限值 → 退回两质心中点。
    if (!std::isfinite(pos.x) || !std::isfinite(pos.y) ||
        !std::isfinite(pos.z)) {
        pos = (A.position + B.position) * 0.5f;
    }
    addPoint(m, pos, c.depth, 0);
}

// 形状排序权重：**平面永远是最小的**（当 A），其余按枚举值。
// 这样"平面-盒"永远走同一条分支，不需要写两遍。
int kindOrder(ShapeKind k) {
    return (k == ShapeKind::Plane) ? -1 : static_cast<int>(k);
}

} // namespace

// ================================================================
//  入口
// ================================================================

bool collideBodies(const RigidBody& A, const RigidBody& B, std::uint32_t indexA,
                   std::uint32_t indexB, ContactManifold& out) {
    out.bodyA = indexA;
    out.bodyB = indexB;
    out.count = 0;
    out.friction = combineFriction(A.material.dynamicFriction,
                                   B.material.dynamicFriction);
    out.restitution = combineRestitution(A.material.restitution,
                                         B.material.restitution);

    // ---- 触发器：只报"有接触"，不产生任何真正的几何求解 ----
    if (A.hasFlag(kBodyIsTrigger) || B.hasFlag(kBodyIsTrigger)) return false;

    const ShapeKind ka = A.shape.kind;
    const ShapeKind kb = B.shape.kind;
    // 内部统一顺序（平面在前，其余按枚举）。
    const bool swap = kindOrder(ka) > kindOrder(kb);
    const RigidBody& X = swap ? B : A;   // "小"的那个
    const RigidBody& Y = swap ? A : B;
    const ShapeKind kx = swap ? kb : ka;
    const ShapeKind ky = swap ? ka : kb;

    ContactManifold tmp;
    tmp.friction = out.friction;
    tmp.restitution = out.restitution;

    switch (kx) {
        case ShapeKind::Plane:
            planeVsBody(X, Y, tmp);
            break;
        case ShapeKind::Sphere:
            if (ky == ShapeKind::Sphere) sphereSphere(X, Y, tmp);
            else if (ky == ShapeKind::Box) sphereBox(X, Y, tmp, false);
            else if (ky == ShapeKind::Capsule) {
                // 球-胶囊：用胶囊-球再翻过来
                capsuleSphere(Y, X, tmp);
                tmp.normal = -tmp.normal;
            } else genericConvex(X, Y, tmp);
            break;
        case ShapeKind::Box:
            if (ky == ShapeKind::Box) boxBox(X, Y, tmp);
            else if (ky == ShapeKind::Capsule) capsuleBox(Y, X, tmp, true);
            else genericConvex(X, Y, tmp);
            break;
        case ShapeKind::Capsule:
            if (ky == ShapeKind::Capsule) capsuleCapsule(X, Y, tmp);
            else genericConvex(X, Y, tmp);
            break;
        case ShapeKind::ConvexHull:
            genericConvex(X, Y, tmp);
            break;
    }

    if (tmp.count == 0) return false;

    // 统一在这里收敛到求解器认的 4 个点 —— 放在**唯一出口**上，就不必
    // 要求每条特例分支都记得自己 trim（漏一个就会让求解器多做一倍迭代）。
    trimToDeepest(tmp);

    // ---- 翻回调用方的 (A, B) 顺序 ----
    // 只有法线需要翻：bodyA / bodyB 一开始就记的是调用方给的下标，
    // 求解器按"bodyA 用 -n、bodyB 用 +n"施加冲量，法线翻了就自洽了。
    out.normal = swap ? -tmp.normal : tmp.normal;
    out.count = tmp.count;
    for (int i = 0; i < tmp.count; ++i) out.points[i] = tmp.points[i];

    // 非有限的法线直接丢弃 —— 放进去会让整帧的速度变成 NaN。
    if (!std::isfinite(out.normal.x) || !std::isfinite(out.normal.y) ||
        !std::isfinite(out.normal.z) || glm::dot(out.normal, out.normal) < 1e-8f) {
        out.count = 0;
        return false;
    }
    return true;
}

// ================================================================
//  查询用的最小穿透深度
// ================================================================

float penetrationDepth(const ShapeQuery& a, const ShapeQuery& b,
                       glm::vec3& outNormal) {
    RigidBody A, B;
    A.shape = *a.shape;
    A.position = a.position;
    A.rotation = a.rotation;
    A.massProps = a.shape->massFromMass(1.0f);
    A.material.dynamicFriction = 0.5f;

    B.shape = *b.shape;
    B.position = b.position;
    B.rotation = b.rotation;
    B.massProps = b.shape->massFromMass(1.0f);
    B.material.dynamicFriction = 0.5f;

    ContactManifold m;
    if (!collideBodies(A, B, 0, 1, m)) return 0.0f;
    outNormal = m.normal;
    float deepest = 0.0f;
    for (int i = 0; i < m.count; ++i) {
        deepest = std::max(deepest, m.points[i].depth);
    }
    return deepest;
}

} // namespace physics
