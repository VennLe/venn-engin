// ============================================================
// assetio/AutoRig.cpp —— 一键自动绑骨（静态人形网格 → 骨架 + 权重）
//
// 实现思路见 AutoRig.h 的文件头。这里只补三条**踩过才知道**的实现约定：
//
//   1. 薄层质心的 z 是**名义高度**，不是质心算出来的 z。
//      一条 1 cm 厚的层，其质心 z 就在层中心 —— 但那是"这层在哪里"，
//      不是"关节在哪里"。关节 z 用解剖比例给，横向位置才用质心。
//   2. 躯干薄层必须**掐掉手臂**（|lat| 上限），否则肩部那一层的质心
//      会被两条手臂拽到身体外侧，脊柱整根歪掉。
//   3. 距离反比的权重**必须分区闸门**。T-pose 下左手指尖与右腿的球面
//      距离只有 ~0.4 m，不闸门的话抬左手会把右腿带走 —— 这是所有
//      "距离权重"实现的第一号翻车点。
// ============================================================

#include "assetio/AutoRig.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace assetio {

namespace {

// 点到线段的最短距离
float distPointSegment(const glm::vec3& p, const glm::vec3& a,
                       const glm::vec3& b) {
    const glm::vec3 ab = b - a;
    const float len2 = glm::dot(ab, ab);
    if (len2 < 1.0e-12f) return glm::length(p - a);
    float t = glm::dot(p - a, ab) / len2;
    t = std::clamp(t, 0.0f, 1.0f);
    return glm::length(p - (a + ab * t));
}

// 把子网格变换烘进顶点，transform 置单位阵。
// 法线用**逆转置**：非均匀缩放（fbx 里很常见，cm→m 的 100 倍就属于此类）
// 下直接用 mat3(T) 会把法线拉歪，光照立刻不对。
void bakeTransforms(SourceModel& model) {
    for (SourceSubMesh& sm : model.subMeshes) {
        const glm::mat4& m = sm.transform;
        const bool identity =
            m[0][0] == 1.0f && m[1][1] == 1.0f && m[2][2] == 1.0f &&
            m[3][0] == 0.0f && m[3][1] == 0.0f && m[3][2] == 0.0f &&
            m[0][1] == 0.0f && m[0][2] == 0.0f && m[1][0] == 0.0f &&
            m[1][2] == 0.0f && m[2][0] == 0.0f && m[2][1] == 0.0f;
        if (identity) continue;

        const glm::mat3 nrm(glm::inverseTranspose(glm::mat3(m)));
        for (Vertex& v : sm.vertices) {
            const glm::vec3 p = glm::vec3(
                m * glm::vec4(glm::vec3(v.pos[0], v.pos[1], v.pos[2]), 1.0f));
            v.pos[0] = p.x;
            v.pos[1] = p.y;
            v.pos[2] = p.z;
            glm::vec3 n = nrm * glm::vec3(v.normal[0], v.normal[1], v.normal[2]);
            const float l = glm::length(n);
            n = (l > 1.0e-6f) ? n / l : glm::vec3(0.0f, 0.0f, 1.0f);
            v.normal[0] = n.x;
            v.normal[1] = n.y;
            v.normal[2] = n.z;
        }
        sm.transform = glm::mat4(1.0f);
    }
}

struct SliceStats {
    int n = 0;
    glm::vec3 centroid{0.0f};
};

// 关节定义表：名字 + 父下标。顺序即 joints 顺序（父恒在子之前，
// 于是 topoOrder 就是 0..n-1，不需要再排）。
struct JointDef {
    const char* name;
    int parent;
};

const JointDef kDefs[] = {
    {"Hips", -1},          {"Spine", 0},          {"Spine1", 1},
    {"Spine2", 2},         {"Neck", 3},           {"Head", 4},
    {"HeadTop_End", 5},    {"LeftShoulder", 3},   {"LeftArm", 7},
    {"LeftForeArm", 8},    {"LeftHand", 9},       {"RightShoulder", 3},
    {"RightArm", 11},      {"RightForeArm", 12},  {"RightHand", 13},
    {"LeftUpLeg", 0},      {"LeftLeg", 15},       {"LeftFoot", 16},
    {"LeftToeBase", 17},   {"RightUpLeg", 0},     {"RightLeg", 19},
    {"RightFoot", 20},     {"RightToeBase", 21},
};
constexpr int kJointCount = static_cast<int>(sizeof(kDefs) / sizeof(kDefs[0]));

} // namespace

bool autoRigHumanoid(SourceModel& model, const AutoRigOptions& opt,
                     AutoRigReport* report, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };

    if (model.subMeshes.empty()) return fail("no_mesh");
    if (!model.joints.empty() && !opt.overwriteExisting)
        return fail("already_rigged (pass overwriteExisting to force)");
    if (opt.maxInfluences < 1 || opt.maxInfluences > 4)
        return fail("maxInfluences 必须在 1..4 之间（渲染管线固定 4 槽）");

    if (opt.bakeTransforms) bakeTransforms(model);

    // ---- 收集世界坐标（烘过之后就是顶点自己的坐标）----
    std::vector<glm::vec3> P;
    P.reserve(model.vertexCount());
    for (const SourceSubMesh& sm : model.subMeshes)
        for (const Vertex& v : sm.vertices)
            P.push_back(glm::vec3(v.pos[0], v.pos[1], v.pos[2]));
    if (P.size() < 200) return fail("too_few_vertices");

    // ---- ① 量测 ----
    glm::vec3 lo(1.0e30f), hi(-1.0e30f);
    for (const glm::vec3& p : P) {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    const glm::vec3 ext = hi - lo;
    const float H = ext.z;
    if (H < 0.05f) return fail("degenerate_height");

    const int armAxis = (ext.x >= ext.y) ? 0 : 1;
    const int fwdAxis = 1 - armAxis;
    const glm::vec3 mid = (lo + hi) * 0.5f;

    float halfSpan = 0.0f;
    for (const glm::vec3& p : P)
        halfSpan = std::max(halfSpan, std::fabs(p[armAxis] - mid[armAxis]));
    if (halfSpan < 0.02f * H) halfSpan = 0.02f * H;

    // 朝向：脚趾一定比身体质心更靠前。比"猜 +Y"可靠得多。
    const float footTop = lo.z + 0.08f * H;
    glm::vec3 cAll(0.0f), cFeet(0.0f);
    int nFeet = 0;
    for (const glm::vec3& p : P) {
        cAll += p;
        if (p.z <= footTop) {
            cFeet += p;
            ++nFeet;
        }
    }
    cAll /= static_cast<float>(P.size());
    if (nFeet > 0) cFeet /= static_cast<float>(nFeet);
    float fwdSign = (cFeet[fwdAxis] - cAll[fwdAxis]) >= 0.0f ? 1.0f : -1.0f;
    if (std::fabs(cFeet[fwdAxis] - cAll[fwdAxis]) < 0.004f * H)
        fwdSign = 1.0f;   // 量不出来就取 +（并在报告里留原始量测）

    glm::vec3 up(0.0f, 0.0f, 1.0f);
    glm::vec3 fwd(0.0f);
    fwd[fwdAxis] = fwdSign;
    // 解剖学左：left = up × forward（右手系）。不靠 ±X 的约定。
    const glm::vec3 left = glm::normalize(glm::cross(up, fwd));

    auto latOf = [&](const glm::vec3& p) { return glm::dot(p - mid, left); };
    auto fwdOf = [&](const glm::vec3& p) { return glm::dot(p - mid, fwd); };

    // ---- ② 薄层量测 ----
    // side: 0 = 两侧都要；+1 = 只要 left 侧；-1 = 只要 right 侧
    // maxAbsLat > 0 时掐掉更外侧的顶点（躯干层不能把手臂算进来）
    auto sliceBand = [&](float zLo, float zHi, int side,
                         float maxAbsLat) -> SliceStats {
        SliceStats s;
        glm::vec3 acc(0.0f);
        for (const glm::vec3& p : P) {
            if (p.z < zLo || p.z > zHi) continue;
            const float l = latOf(p);
            if (side > 0 && l < 0.0f) continue;
            if (side < 0 && l > 0.0f) continue;
            if (maxAbsLat > 0.0f && std::fabs(l) > maxAbsLat) continue;
            acc += p;
            ++s.n;
        }
        if (s.n) s.centroid = acc / static_cast<float>(s.n);
        return s;
    };

    // 手臂横向带（T-pose 与 A-pose 都成立：取 |lat| 落在某区间的顶点）
    auto armBand = [&](float latLo, float latHi, int side) -> SliceStats {
        SliceStats s;
        glm::vec3 acc(0.0f);
        for (const glm::vec3& p : P) {
            if (p.z < lo.z + 0.45f * H) continue;   // 只在上半身找
            const float l = latOf(p) * static_cast<float>(side);
            if (l < latLo || l > latHi) continue;
            acc += p;
            ++s.n;
        }
        if (s.n) s.centroid = acc / static_cast<float>(s.n);
        return s;
    };

    const float torsoMaxLat = std::max(0.35f * halfSpan, 0.15f * H);
    const float gateTol = 0.02f * H;
    // 薄层半厚：0.02H ≈ 3.6 cm（1.8 m 的人）。窄了低模取不到 8 个顶点，
    // 宽了关节会被上下两段一起糊住。薄层质心的 **z 不可用**（它就在层
    // 中心），只有横向/前后分量有意义。
    const float bandHalf = 0.02f * H;

    // 躯干关节：横向/前后取薄层质心，z 用名义高度
    auto torso = [&](float zf) {
        const float z = lo.z + zf * H;
        const SliceStats s = sliceBand(z - bandHalf, z + bandHalf, 0,
                                       torsoMaxLat);
        glm::vec3 p = mid;
        p.z = z;
        if (s.n >= 8) {
            p += left * latOf(s.centroid);
            p += fwd * fwdOf(s.centroid);
        }
        return p;
    };

    // 单侧关节（腿）：横向跟着该侧的质心，但**保留符号**
    auto sided = [&](float zf, int side, float latFallback) {
        const float z = lo.z + zf * H;
        const SliceStats s = sliceBand(z - bandHalf, z + bandHalf, side,
                                       torsoMaxLat);
        glm::vec3 p = mid;
        p.z = z;
        if (s.n >= 8) {
            p += left * latOf(s.centroid);
            p += fwd * fwdOf(s.centroid);
        } else {
            p += left * (latFallback * static_cast<float>(side));
        }
        return p;
    };

    // ---- 解剖比例表（分数 = 身高 H 的占比）----
    //
    // 这组数字不是拍脑袋：人体测量学的标准比例，并与 Mixamo 原厂骨架
    // （samba_dancing.fbx 的 52 根骨）逐点核对过 —— 差距都在 1~2 cm。
    // 每个值后面的括号是"原厂值 / H"，改动时对着它比。
    //
    // ⚠ 这一层只给**纵向**与"够不着网格时"的兜底；只要薄层里有足够
    //   顶点，横向位置一律以**网格实测**为准（这正是自动绑骨比"套模板"
    //   强的地方）。
    std::array<glm::vec3, kJointCount> jp{};

    // 躯干链
    jp[0] = torso(0.552f);   // Hips         (0.552)
    jp[1] = torso(0.607f);   // Spine        (0.607)
    jp[2] = torso(0.672f);   // Spine1       (0.672)
    jp[3] = torso(0.746f);   // Spine2       (0.746)
    jp[4] = torso(0.829f);   // Neck         (0.829)
    jp[5] = torso(0.886f);   // Head         (0.886)
    // HeadTop_End：头顶，用头部薄层的横向位置 + 包围盒顶
    {
        jp[6] = torso(0.960f);
        jp[6].z = hi.z;
    }

    // 手臂：先量肘与腕（|lat| 的分位带），再按解剖常数放肩
    //
    // ⚠ 分位带的系数是**对着原厂骨架校准过的**：halfSpan 是"中轴到指尖"，
    //   而肘在 0.474·halfSpan、腕在 0.758·halfSpan（手指占了最后 1/4）。
    //   早期版本按 0.55~0.72 取肘，量出来是"手肘长在手腕上"—— 因为手臂
    //   在 T-pose 下横向展开，|lat| 的分位直接就是骨长比例。
    const float shoulderZ = lo.z + 0.795f * H;   // 原厂 0.795
    for (int side = 1; side >= -1; side -= 2) {
        const SliceStats elbowS = armBand(0.42f * halfSpan, 0.52f * halfSpan, side);
        const SliceStats wristS = armBand(0.70f * halfSpan, 0.80f * halfSpan, side);
        glm::vec3 elbow = mid, wrist = mid;
        elbow.z = wrist.z = shoulderZ;
        if (elbowS.n >= 8) {
            elbow = elbowS.centroid;
        } else {
            elbow += left * (0.47f * halfSpan * side);
            elbow.z = shoulderZ;
        }
        if (wristS.n >= 8) {
            wrist = wristS.centroid;
        } else {
            wrist += left * (0.76f * halfSpan * side);
            wrist.z = shoulderZ;
        }

        // 肩（三角肌球心）：横向取身高的解剖常数 0.104H（原厂 0.1876/1.805），
        // 但不超过网格（袖珍模型上 0.104H 可能已经越过指尖）。
        float shoulderLat = 0.104f * H;
        shoulderLat = std::min(shoulderLat, 0.45f * halfSpan);
        glm::vec3 armP = mid;
        armP += left * (shoulderLat * static_cast<float>(side));
        armP.z = shoulderZ;
        armP += fwd * fwdOf(elbow);

        glm::vec3 clavP = mid;
        clavP += left * (shoulderLat * 0.35f * static_cast<float>(side));
        clavP.z = shoulderZ;
        clavP += fwd * fwdOf(jp[4]);

        if (side > 0) {
            jp[7] = clavP;   // LeftShoulder
            jp[8] = armP;    // LeftArm
            jp[9] = elbow;   // LeftForeArm
            jp[10] = wrist;  // LeftHand
        } else {
            jp[11] = clavP;
            jp[12] = armP;
            jp[13] = elbow;
            jp[14] = wrist;
        }
    }

    // 腿：用 0.45H 处的左右分侧质心定大腿横向位置
    {
        const SliceStats legL = sliceBand(lo.z + 0.42f * H, lo.z + 0.48f * H,
                                          1, torsoMaxLat);
        const SliceStats legR = sliceBand(lo.z + 0.42f * H, lo.z + 0.48f * H,
                                          -1, torsoMaxLat);
        float hipLatL = 0.05f * H, hipLatR = 0.05f * H;
        // 股骨头位置：解剖常数 0.0505H（原厂 0.0912/1.805）比网格质心更准
        // —— 大腿在 0.45H 处的外形质心比股骨头**偏内** 1.5~2 cm（大腿是
        // 锥形，内侧更粗）。所以以常数为准，只把测出来的值当上下界。
        if (legL.n >= 8) {
            const float meas = std::fabs(latOf(legL.centroid));
            hipLatL = glm::clamp(0.0505f * H, meas * 0.75f, meas * 1.25f);
        }
        if (legR.n >= 8) {
            const float meas = std::fabs(latOf(legR.centroid));
            hipLatR = glm::clamp(0.0505f * H, meas * 0.75f, meas * 1.25f);
        }

        const int upLeg[2] = {15, 19};
        const int knee[2] = {16, 20};
        const int foot[2] = {17, 21};
        const int toe[2] = {18, 22};
        const int sideOf[2] = {1, -1};
        const float hipLat[2] = {hipLatL, hipLatR};

        for (int k = 0; k < 2; ++k) {
            const int s = sideOf[k];
            // 大腿根：z 固定在髋高，横向用测出来的
            glm::vec3 hip = mid;
            hip += left * (hipLat[k] * static_cast<float>(s));
            hip.z = lo.z + 0.515f * H;    // 原厂 UpLeg 在 0.515H
            hip += fwd * fwdOf(jp[0]);
            jp[upLeg[k]] = hip;

            jp[knee[k]] = sided(0.290f, s, hipLat[k]);          // 原厂 0.290
            // 踝：横向/前后在**小腿下段**（0.09H）量，z 落在解剖高 0.057H。
            // 直接在 0.057H 量会被脚跟拽偏 —— 脚踝那一层的质心在脚的
            // 前后中点，而踝关节明显偏前（实测偏后 3.3 cm）。
            glm::vec3 ankle = sided(0.090f, s, hipLat[k] * 0.95f);
            ankle.z = lo.z + 0.057f * H;
            jp[foot[k]] = ankle;

            // 脚趾：从踝沿朝向前伸 0.07H（原厂：趾根比踝前移 0.126 m / 1.805）。
            // z 取近地面 —— ToeBase 是"蹬地点"，必须落在脚掌平面上，
            // 抬到踝高会导致走路时脚尖插不进地面、看起来像踩高跷。
            jp[toe[k]] = ankle + fwd * (0.07f * H);
            jp[toe[k]].z = lo.z + 0.006f * H;
        }
    }

    // ---- ③ 写骨架 ----
    // restLocal 只有平移：父位置 → 本位置。旋转全为单位阵。
    model.joints.clear();
    model.joints.resize(static_cast<std::size_t>(kJointCount));
    model.topoOrder.clear();
    for (int i = 0; i < kJointCount; ++i) {
        SourceJoint& j = model.joints[static_cast<std::size_t>(i)];
        j.name = kDefs[i].name;
        j.parent = kDefs[i].parent;
        const glm::vec3 parentPos =
            (kDefs[i].parent >= 0) ? jp[static_cast<std::size_t>(kDefs[i].parent)]
                                   : glm::vec3(0.0f);
        j.restLocal = glm::translate(glm::mat4(1.0f), jp[i] - parentPos);
        j.invBind = glm::translate(glm::mat4(1.0f), -jp[i]);
        j.externalChain = glm::mat4(1.0f);
        model.topoOrder.push_back(i);
    }
    model.invMeshNode = glm::mat4(1.0f);

    // ---- ④ 权重 ----
    // 骨段 = 父关节 → 本关节。Hips 没有父，给它一根很小的竖直"桩"，
    // 否则骨盆那一圈顶点会被三条向外的骨段平分，抬腿时屁股跟着走。
    struct Bone {
        int joint = 0;
        glm::vec3 a{0.0f}, b{0.0f};
        bool leftOnly = false;
        bool rightOnly = false;
    };
    std::vector<Bone> bones;
    bones.reserve(kJointCount);
    {
        Bone hips;
        hips.joint = 0;
        hips.a = jp[0] - up * (0.02f * H);
        hips.b = jp[0] + up * (0.03f * H);
        bones.push_back(hips);
    }
    for (int i = 1; i < kJointCount; ++i) {
        Bone b;
        b.joint = i;
        b.a = jp[static_cast<std::size_t>(kDefs[i].parent)];
        b.b = jp[static_cast<std::size_t>(i)];
        const std::string nm = kDefs[i].name;
        if (nm.rfind("Left", 0) == 0) b.leftOnly = true;
        if (nm.rfind("Right", 0) == 0) b.rightOnly = true;
        bones.push_back(b);
    }

    const int maxInf = opt.maxInfluences;
    const float falloff = std::max(opt.weightFalloff, 1.0f);

    int riggedCount = 0;
    float maxErr = 0.0f;
    float maxInfSeen = 0.0f;

    for (SourceSubMesh& sm : model.subMeshes) {
        for (Vertex& v : sm.vertices) {
            const glm::vec3 p(v.pos[0], v.pos[1], v.pos[2]);
            const float lat = latOf(p);

            float wBest[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            int jBest[4] = {0, 0, 0, 0};
            int used = 0;

            for (const Bone& b : bones) {
                if (b.leftOnly && lat < -gateTol) continue;
                if (b.rightOnly && lat > gateTol) continue;
                const float d = distPointSegment(p, b.a, b.b);
                const float w = 1.0f / std::pow(d + 1.0e-4f, falloff);
                if (!(w > 0.0f) || !std::isfinite(w)) continue;

                // 插入到有序的前 maxInf 个（数组小，插入排序最省事）
                int pos = used;
                while (pos > 0 && wBest[pos - 1] < w) {
                    if (pos < maxInf) {
                        wBest[pos] = wBest[pos - 1];
                        jBest[pos] = jBest[pos - 1];
                    }
                    --pos;
                }
                if (pos < maxInf) {
                    wBest[pos] = w;
                    jBest[pos] = b.joint;
                    if (used < maxInf) ++used;
                }
            }

            float sum = 0.0f;
            for (int i = 0; i < used; ++i) sum += wBest[i];
            if (sum <= 0.0f) {
                // 兜底：全被闸门挡掉（拓扑怪到没有一条骨段靠近它）。
                // 给它 Hips 全权重，至少不会在渲染时"消失到原点"。
                v.joints[0] = 0;
                v.joints[1] = v.joints[2] = v.joints[3] = 0;
                v.weights[0] = 1.0f;
                v.weights[1] = v.weights[2] = v.weights[3] = 0.0f;
                continue;
            }

            float acc = 0.0f;
            for (int i = 0; i < 4; ++i) {
                if (i < used) {
                    const float nw = wBest[i] / sum;
                    v.joints[i] = static_cast<std::uint32_t>(jBest[i]);
                    v.weights[i] = nw;
                    acc += nw;
                } else {
                    v.joints[i] = 0;
                    v.weights[i] = 0.0f;
                }
            }
            maxErr = std::max(maxErr, std::fabs(acc - 1.0f));
            maxInfSeen = std::max(maxInfSeen, static_cast<float>(used));
            ++riggedCount;
        }
    }

    // 包围盒不变（顶点没动），但保险起见重算一次 —— 烘变换之后它的
    // 语义就是"顶点 AABB"，与运行时/CLI 的口径一致。
    {
        glm::vec3 rlo(1.0e30f), rhi(-1.0e30f);
        for (const SourceSubMesh& sm : model.subMeshes)
            for (const Vertex& v : sm.vertices) {
                const glm::vec3 p(v.pos[0], v.pos[1], v.pos[2]);
                rlo = glm::min(rlo, p);
                rhi = glm::max(rhi, p);
            }
        if (rlo.x <= rhi.x) {
            model.boundsMin = rlo;
            model.boundsMax = rhi;
            model.radius = std::max(glm::length(rhi - rlo) * 0.5f, 0.01f);
        }
    }

    if (report) {
        report->height = H;
        report->armSpan = ext[static_cast<std::size_t>(armAxis)];
        report->armAxis = armAxis;
        report->fwdAxis = fwdAxis;
        report->fwdSign = fwdSign;
        report->halfSpan = halfSpan;
        report->jointCount = kJointCount;
        report->vertices = static_cast<int>(P.size());
        report->verticesRigged = riggedCount;
        report->maxWeightError = maxErr;
        report->maxInfluences = maxInfSeen;
        report->jointNames.clear();
        report->jointPositions.clear();
        for (int i = 0; i < kJointCount; ++i) {
            report->jointNames.emplace_back(kDefs[i].name);
            report->jointPositions.push_back(jp[static_cast<std::size_t>(i)]);
        }
    }
    return true;
}

} // namespace assetio
