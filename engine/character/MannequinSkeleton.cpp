#include "character/MannequinSkeleton.h"

#include "assets/Model.h"
#include "core/Logger.h"

#include "assetio/SkelFormat.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <numeric>
#include <utility>

namespace character {

namespace {

// ------------------------------------------------------------
// UE5 Mannequin 骨骼表（35 根）
//
// 骨名来自 `SK_Mannequin.uasset` 的可读串（不是凭记忆）。`parent` 是**表内
// 下标**，不是存储下标 —— 存储顺序由重定向决定（见下面"为什么下标不动"）。
//
// 三组"合成骨"：
//   · spine_04 / spine_05 —— 插在 spine_03 与 neck_01 之间的过渡骨。
//     Mixamo 只有 3 节脊柱，UE 有 5 节；放在原骨链的直线上，所以**世界
//     变换不变**，只是多了两个能单独旋转的关节。
//   · neck_02 —— 同理，插在 neck_01 与 head 之间。
//   · ik_* / weapon_* —— UE 的**非形变骨**（bDeform=false）：IK 目标与挂点，
//     不参与蒙皮，位置与 foot_* / hand_* 重合。
// ------------------------------------------------------------
const MannequinBoneDef kBones[] = {
    // name          parent  origin                        from                                to            t      follow deform
    {"root",         -1, MannequinOrigin::AtOrigin,       "",                                 "",           0.0f,  -1, false},
    {"pelvis",        0, MannequinOrigin::Copy,           "Hips,pelvis,Hip",                  "",           0.0f,  -1, true },
    {"spine_01",      1, MannequinOrigin::Copy,           "Spine,spine_01",                   "",           0.0f,  -1, true },
    {"spine_02",      2, MannequinOrigin::Copy,           "Spine1,spine_02",                  "",           0.0f,  -1, true },
    {"spine_03",      3, MannequinOrigin::Copy,           "Spine2,spine_03",                  "",           0.0f,  -1, true },
    {"spine_04",      4, MannequinOrigin::Lerp,           "Spine2,spine_03",                  "Neck,neck_01", 1.0f / 3.0f, -1, true },
    {"spine_05",      5, MannequinOrigin::Lerp,           "Spine2,spine_03",                  "Neck,neck_01", 2.0f / 3.0f, -1, true },
    {"neck_01",       6, MannequinOrigin::Copy,           "Neck,neck_01",                     "",           0.0f,  -1, true },
    {"neck_02",       7, MannequinOrigin::Lerp,           "Neck,neck_01",                     "Head,head",  0.5f,  -1, true },
    {"head",          8, MannequinOrigin::Copy,           "Head,head",                        "",           0.0f,  -1, true },

    {"clavicle_l",    6, MannequinOrigin::Copy,           "LeftShoulder,clavicle_l,shoulder.L", "",         0.0f,  -1, true },
    {"upperarm_l",   10, MannequinOrigin::Copy,           "LeftArm,upperarm_l,upper_arm.L",   "",           0.0f,  -1, true },
    {"lowerarm_l",   11, MannequinOrigin::Copy,           "LeftForeArm,lowerarm_l,forearm.L", "",           0.0f,  -1, true },
    {"hand_l",       12, MannequinOrigin::Copy,           "LeftHand,hand_l,hand.L",           "",           0.0f,  -1, true },
    {"clavicle_r",    6, MannequinOrigin::Copy,           "RightShoulder,clavicle_r,shoulder.R", "",       0.0f,  -1, true },
    {"upperarm_r",   14, MannequinOrigin::Copy,           "RightArm,upperarm_r,upper_arm.R",  "",           0.0f,  -1, true },
    {"lowerarm_r",   15, MannequinOrigin::Copy,           "RightForeArm,lowerarm_r,forearm.R", "",          0.0f,  -1, true },
    {"hand_r",       16, MannequinOrigin::Copy,           "RightHand,hand_r,hand.R",          "",           0.0f,  -1, true },

    {"thigh_l",       1, MannequinOrigin::Copy,           "LeftUpLeg,thigh_l,thigh.L",        "",           0.0f,  -1, true },
    {"calf_l",       18, MannequinOrigin::Copy,           "LeftLeg,calf_l,shin.L",            "",           0.0f,  -1, true },
    {"foot_l",       19, MannequinOrigin::Copy,           "LeftFoot,foot_l",                  "",           0.0f,  -1, true },
    {"ball_l",       20, MannequinOrigin::Copy,           "LeftToeBase,ball_l,toe.L",         "",           0.0f,  -1, true },
    {"thigh_r",       1, MannequinOrigin::Copy,           "RightUpLeg,thigh_r,thigh.R",       "",           0.0f,  -1, true },
    {"calf_r",       22, MannequinOrigin::Copy,           "RightLeg,calf_r,shin.R",           "",           0.0f,  -1, true },
    {"foot_r",       23, MannequinOrigin::Copy,           "RightFoot,foot_r",                 "",           0.0f,  -1, true },
    {"ball_r",       24, MannequinOrigin::Copy,           "RightToeBase,ball_r,toe.R",        "",           0.0f,  -1, true },

    {"ik_foot_root",  0, MannequinOrigin::AtOrigin,       "",                                 "",           0.0f,  -1, false},
    {"ik_foot_l",    26, MannequinOrigin::Follow,         "",                                 "",           0.0f,  20, false},  // = foot_l
    {"ik_foot_r",    26, MannequinOrigin::Follow,         "",                                 "",           0.0f,  24, false},  // = foot_r
    {"ik_hand_root",  0, MannequinOrigin::AtOrigin,       "",                                 "",           0.0f,  -1, false},
    {"ik_hand_gun",  29, MannequinOrigin::Follow,         "",                                 "",           0.0f,  17, false},  // = hand_r
    {"ik_hand_l",    29, MannequinOrigin::Follow,         "",                                 "",           0.0f,  13, false},  // = hand_l
    {"ik_hand_r",    29, MannequinOrigin::Follow,         "",                                 "",           0.0f,  17, false},  // = hand_r
    {"weapon_l",     13, MannequinOrigin::Follow,         "",                                 "",           0.0f,  13, false},  // = hand_l
    {"weapon_r",     17, MannequinOrigin::Follow,         "",                                 "",           0.0f,  17, false},  // = hand_r
};

constexpr int kCount = static_cast<int>(sizeof(kBones) / sizeof(kBones[0]));

// `spec` 是逗号分隔的候选骨名，按顺序试第一个命中的。
//
// ⚠ 用 `findJoint` 而不是精确匹配：它在精确没中时会退化成"包含 + 跳过末端
//   辅助骨"，正好吃掉 `mixamorig:` 前缀与不同美术的随手命名。显式写多个
//   候选是为了**顺序确定**，不让匹配规则替我们掷骰子。
int resolveAny(const assets::Skeleton& s, const char* spec) {
    if (spec == nullptr || *spec == '\0') return -1;
    std::string cur;
    for (const char* p = spec;; ++p) {
        if (*p == ',' || *p == '\0') {
            if (!cur.empty()) {
                const int j = s.findJoint(cur, assets::kJointMatchDefault);
                if (j >= 0) return j;
            }
            cur.clear();
            if (*p == '\0') break;
        } else {
            cur.push_back(*p);
        }
    }
    return -1;
}

// 源骨架的静止世界矩阵。约定与 `assets::Skeleton::sample` 一致：
// 根骨乘 `externalChain`（armature 节点那两三层的静止链）。
void restWorlds(const assets::Skeleton& s, std::vector<glm::mat4>& out) {
    const int n = s.jointCount();
    out.assign(static_cast<size_t>(n), glm::mat4(1.0f));

    std::vector<int> order = s.topoOrder;
    if (static_cast<int>(order.size()) != n) {
        order.resize(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) order[static_cast<size_t>(i)] = i;
    }
    for (int j : order) {
        if (j < 0 || j >= n) continue;
        const assets::Skeleton::Joint& jt = s.joints[static_cast<size_t>(j)];
        const glm::mat4 pw = (jt.parent >= 0 && jt.parent < n)
                                 ? out[static_cast<size_t>(jt.parent)]
                                 : jt.externalChain;
        out[static_cast<size_t>(j)] = pw * jt.restLocal;
    }
}

} // namespace

const MannequinBoneDef* mannequinBones() { return kBones; }

int mannequinBoneCount() { return kCount; }

int mannequinBoneIndex(const std::string& name) {
    for (int i = 0; i < kCount; ++i) {
        if (name == kBones[i].name) return i;
    }
    return -1;
}

bool mannequinBoneDeforms(int index) {
    if (index < 0 || index >= kCount) return false;
    return kBones[index].deform;
}

// ============================================================
// 重定向
//
// ------------------------------------------------------------
// 为什么"下标一个都不动"（这是本函数最关键的设计决定）
// ------------------------------------------------------------
// 顶点里存的是 `Vertex::joints`（`glm::uvec4`）。骨架一旦重排，这些下标就
// 全指错骨 —— 而 `Mesh` 只暴露 `vertices()` 的 const 引用，没有改写入口，
// 改它就要动 GPU 侧缓冲。**所以重排这条路根本不该走。**
//
// 采用的做法：
//   ① 源骨架的**每一根骨在原下标原地保留**（其中 22 根改成 UE 骨名）；
//   ② UE 表里"源里没有对应"的骨（新 spine_04/05、neck_02、ik_*、weapon_*、
//      root）**追加到末尾**；
//   ③ 因此 顶点下标不用动、动画轨道下标也不用动 —— 一个字节的顶点数据都
//      不碰。这正是"重定向不动权重"能成立的**机制**，不是靠自觉。
//
// 代价：源里那些 UE 没有的辅助骨（Mixamo 的 `HeadTop_End`）会留在骨架里，
// 通常 35 + 1 根。这是**故意的** —— 删掉它就得重排下标、动顶点。它不参与
// UE 语义，也不影响任何 UE 骨的层级，代价远小于动顶点。
//
// 层级的变化（`neck_01` 的父从 `Spine2` 变成 `spine_05`）仍然要处理：
// 先把世界矩阵算好，再 `restLocal = inverse(parentWorld) * world`。这样即使
// 父子关系变了，乘出来的世界矩阵还是原来那个 —— 姿势一格都不歪。
// ============================================================
bool retargetToMannequin(const assets::Skeleton& src, assets::Skeleton& out,
                         MannequinRetargetReport& report) {
    report = MannequinRetargetReport{};

    const int n = src.jointCount();
    if (n <= 0) return false;

    std::vector<glm::mat4> sw;
    restWorlds(src, sw);

    // ---- ① 决定每根 UE 表骨落在哪个存储槽 ----
    //
    // -1 = 还没定。Copy 骨取"源里解析到的下标"；非 Copy 骨先看源里是不是
    // **已经有**同名骨（第二遍重定向就是靠这一条保持幂等），否则排队追加。
    std::vector<int> tableToSlot(static_cast<size_t>(kCount), -1);
    std::vector<int> slotToTable(static_cast<size_t>(n), -1);  // 源槽来自表的哪一条
    std::vector<int> appended;                                  // 追加的表下标
    std::vector<char> slotUsed(static_cast<size_t>(n), 0);

    for (int i = 0; i < kCount; ++i) {
        const MannequinBoneDef& def = kBones[i];
        if (def.origin == MannequinOrigin::Copy) {
            int s = resolveAny(src, def.from);
            // 幂等兜底：别名都没中时，看源里是否已经叫这个 UE 名
            if (s < 0) {
                s = src.findJoint(def.name, assets::kJointMatchCaseInsensitive);
            }
            if (s >= 0 && s < n && !slotUsed[static_cast<size_t>(s)]) {
                tableToSlot[static_cast<size_t>(i)] = s;
                slotToTable[static_cast<size_t>(s)] = i;
                slotUsed[static_cast<size_t>(s)] = 1;
                ++report.copied;
            } else {
                // 源里没有 → 退化成一枚举，稍后追加（层级仍然完整）
                ++report.missing;
                report.missingSources.push_back(std::string(def.name) + "<-" +
                                                (def.from ? def.from : "?"));
            }
        } else {
            const int exist = src.findJoint(def.name,
                                            assets::kJointMatchCaseInsensitive);
            if (exist >= 0 && exist < n && !slotUsed[static_cast<size_t>(exist)]) {
                tableToSlot[static_cast<size_t>(i)] = exist;
                slotToTable[static_cast<size_t>(exist)] = i;
                slotUsed[static_cast<size_t>(exist)] = 1;
            } else {
                appended.push_back(i);
            }
        }
    }
    // 缺料的 Copy 骨也走追加（保持骨架完整，只是位置退化到父骨）
    for (int i = 0; i < kCount; ++i) {
        if (tableToSlot[static_cast<size_t>(i)] < 0 &&
            kBones[i].origin == MannequinOrigin::Copy) {
            appended.push_back(i);
        }
    }

    const int total = n + static_cast<int>(appended.size());
    for (int k = 0; k < static_cast<int>(appended.size()); ++k) {
        tableToSlot[static_cast<size_t>(appended[static_cast<size_t>(k)])] = n + k;
        ++report.synthesized;
    }
    for (int i = 0; i < kCount; ++i)
        if (tableToSlot[static_cast<size_t>(i)] >= 0) ++report.ueBones;
    for (int s = 0; s < n; ++s)
        if (slotToTable[static_cast<size_t>(s)] < 0) ++report.keptAux;

    assets::Skeleton dst;
    dst.invMeshNode = src.invMeshNode;
    dst.skeletonToEngine = src.skeletonToEngine;
    dst.joints.assign(static_cast<size_t>(total), assets::Skeleton::Joint{});
    dst.topoOrder.resize(static_cast<size_t>(total));
    for (int i = 0; i < total; ++i) dst.topoOrder[static_cast<size_t>(i)] = i;

    std::vector<glm::mat4> dw(static_cast<size_t>(total), glm::mat4(1.0f));

    // ---- ② 先算"源槽"（下标 0..n-1）----
    //
    // 一个字节都不动：名字照抄（有对应 UE 骨的换成 UE 名），世界矩阵照抄，
    // invBind 照抄。父也暂时照抄，等追加骨算完之后再统一改（见 ③ ）。
    for (int s = 0; s < n; ++s) {
        const assets::Skeleton::Joint& sj = src.joints[static_cast<size_t>(s)];
        assets::Skeleton::Joint& dj = dst.joints[static_cast<size_t>(s)];
        const int t = slotToTable[static_cast<size_t>(s)];
        dj.name = (t >= 0) ? kBones[t].name : sj.name;   // 没对应的保留原名
        dj.parent = sj.parent;
        dj.restLocal = sj.restLocal;
        dj.invBind = sj.invBind;
        dj.externalChain = sj.externalChain;
        dw[static_cast<size_t>(s)] = sw[static_cast<size_t>(s)];
    }

    // ---- ③ 再算追加的 UE 骨（下标 n..total-1）----
    //
    // 顺序**必须**是表的顺序：表是拓扑序，`Follow` 依赖的目标一定已经算过。
    // ⚠ 但目标可能在源槽里（如 weapon_l Follow hand_l 在槽 12），也可能在
    //   更早追加的槽里 —— 两种情况都在 `dw` 里已经就位。
    for (int k = 0; k < static_cast<int>(appended.size()); ++k) {
        const int t = appended[static_cast<size_t>(k)];
        const int slot = n + k;
        const MannequinBoneDef& def = kBones[t];
        assets::Skeleton::Joint& dj = dst.joints[static_cast<size_t>(slot)];
        dj.name = def.name;
        dj.parent = -1;   // 下面填

        glm::mat4 world(1.0f);
        int copyFrom = -1;

        switch (def.origin) {
        case MannequinOrigin::AtOrigin:
            world = glm::mat4(1.0f);
            break;

        case MannequinOrigin::Follow: {
            const int fslot = tableToSlot[static_cast<size_t>(def.follow)];
            world = (fslot >= 0) ? dw[static_cast<size_t>(fslot)] : glm::mat4(1.0f);
            break;
        }

        case MannequinOrigin::Lerp: {
            const int a = resolveAny(src, def.from);
            const int b = resolveAny(src, def.to);
            if (a >= 0 && b >= 0) {
                // 位置插值、旋转沿用**起点骨**：这几根骨都在原骨链的直线上，
                // 局部轴与起点骨一致才不会让下游的 restLocal 拧一下。
                world = sw[static_cast<size_t>(a)];
                world[3] = glm::mix(sw[static_cast<size_t>(a)][3],
                                    sw[static_cast<size_t>(b)][3], def.t);
            } else {
                ++report.missing;
                report.missingSources.push_back(std::string(def.name) + "<-" +
                                                (def.from ? def.from : "?") + ".." +
                                                (def.to ? def.to : "?"));
            }
            break;
        }

        case MannequinOrigin::Copy: {
            // 只可能是"源里缺这根"的退化路径
            const int a = resolveAny(src, def.from);
            if (a >= 0) {
                world = sw[static_cast<size_t>(a)];
                copyFrom = a;
            }
            break;
        }
        }

        (void)copyFrom;
        dw[static_cast<size_t>(slot)] = world;
        dj.invBind = glm::inverse(world);
        dj.externalChain = glm::mat4(1.0f);
        // restLocal 与 parent 在下面统一算（父可能就在源槽里）
        dj.restLocal = glm::mat4(1.0f);
    }

    // ---- ④ 父下标 + restLocal ----
    //
    // 源槽：父照抄（父必须是"没被改名的那条链"上的一根）——但如果它对应的
    //       表骨在 UE 层级里挂在别的父下（`neck_01` 从 `Spine2` 改挂
    //       `spine_05`），就必须按表改。判据是"这个槽有对应表骨"。
    // 追加槽：按表。
    for (int slot = 0; slot < total; ++slot) {
        assets::Skeleton::Joint& dj = dst.joints[static_cast<size_t>(slot)];
        int parent = -1;
        if (slot < n) {
            const int tslot = slotToTable[static_cast<size_t>(slot)];
            if (tslot >= 0 && kBones[tslot].parent >= 0) {
                parent = tableToSlot[static_cast<size_t>(kBones[tslot].parent)];
            } else {
                parent = src.joints[static_cast<size_t>(slot)].parent;
            }
        } else {
            const int def = appended[static_cast<size_t>(slot - n)];
            parent = (kBones[def].parent >= 0)
                         ? tableToSlot[static_cast<size_t>(kBones[def].parent)]
                         : -1;
        }
        if (parent < 0 || parent >= total || parent == slot) parent = -1;
        dj.parent = parent;

        const glm::mat4 pw =
            (parent >= 0) ? dw[static_cast<size_t>(parent)] : glm::mat4(1.0f);
        dj.restLocal = glm::inverse(pw) * dw[static_cast<size_t>(slot)];
    }

    // ---- ⑤ 拓扑序 ----
    //
    // 追加骨在末尾、父却在中间（`neck_01` 在槽 4、父 `spine_05` 在槽 24），
    // 所以存储顺序**不是**拓扑序。`sample()` 走的就是 `topoOrder`，
    // 这里用 Kahn 排一遍。
    {
        std::vector<int> indeg(static_cast<size_t>(total), 0);
        std::vector<std::vector<int>> kids(static_cast<size_t>(total));
        for (int i = 0; i < total; ++i) {
            const int p = dst.joints[static_cast<size_t>(i)].parent;
            if (p >= 0) {
                kids[static_cast<size_t>(p)].push_back(i);
                ++indeg[static_cast<size_t>(i)];
            }
        }
        std::vector<int> queue;
        for (int i = 0; i < total; ++i)
            if (indeg[static_cast<size_t>(i)] == 0)
                queue.push_back(i);
        int head = 0;
        int written = 0;
        while (head < static_cast<int>(queue.size())) {
            const int cur = queue[static_cast<size_t>(head++)];
            dst.topoOrder[static_cast<size_t>(written++)] = cur;
            for (int c : kids[static_cast<size_t>(cur)])
                if (--indeg[static_cast<size_t>(c)] == 0)
                    queue.push_back(c);
        }
        // 有环（不该发生）：剩余按原序补齐，至少不丢骨
        for (int i = 0; i < total && written < total; ++i) {
            bool seen = false;
            for (int k = 0; k < written; ++k)
                if (dst.topoOrder[static_cast<size_t>(k)] == i) { seen = true; break; }
            if (!seen) dst.topoOrder[static_cast<size_t>(written++)] = i;
        }
    }

    // ---- ⑥ 动画：轨道下标一个都不用改 ----
    //
    // 因为源骨都在原下标上。照抄 clip，只把时间与名字带过去 —— 但要把
    // "指向被改名槽"的轨道留着（它们本来就是对的），不丢任何一条。
    dst.clips = src.clips;
    for (const assets::AnimationClip& c : dst.clips)
        report.remappedTracks += static_cast<int>(c.tracks.size());

    // ---- ⑦ 挂点：跟着骨名一起改 ----
    //
    // 挂点挂在**骨名**上（`weapon_r` → `RightHand`），而重定向会把被映射到
    // 的源骨改名成 UE 名（`RightHand` → `hand_r`）。不跟着改的话，重定向后
    // `resolveSockets()` 一律返回 -1，表现是"武器不跟手" —— 静默、无日志。
    //
    // 改法：源槽 s 若被改名（slotToTable[s] >= 0），把 boneName 换成新名字。
    // 没被改名的骨不动（辅助骨名字原样保留）。
    dst.sockets = src.sockets;
    for (assets::Skeleton::Socket& sock : dst.sockets) {
        const int s = src.findJoint(sock.boneName);
        if (s < 0) continue;
        const int t = slotToTable[static_cast<size_t>(s)];
        if (t >= 0) sock.boneName = kBones[t].name;
    }
    // 重解析用的是**新骨架**的骨名索引（上面刚 invalidate 过）。
    dst.resolveSockets();

    dst.boneDigest = 0;   // 骨表换了 → 身份也换了（调用方重算或留 0 = 未知）
    dst.invalidateNameIndex();
    report.bones = dst.jointCount();

    out = std::move(dst);
    return true;
}

bool isMannequinSkeleton(const assets::Skeleton& skel) {
    if (skel.jointCount() <= 0) return false;
    for (int i = 0; i < kCount; ++i) {
        // 精确 + 不分大小写：UE 骨名是写死的，不需要"包含匹配"那些退化
        if (skel.findJoint(kBones[i].name, assets::kJointMatchCaseInsensitive) < 0)
            return false;
    }
    return true;
}

bool upgradeModelSkeletonToMannequin(assets::Model& model,
                                     MannequinRetargetReport* report) {
    if (!model.skeleton || model.skeleton->jointCount() <= 0) return false;
    if (isMannequinSkeleton(*model.skeleton)) return false;

    auto upgraded = std::make_shared<assets::Skeleton>();
    MannequinRetargetReport rep;
    if (!retargetToMannequin(*model.skeleton, *upgraded, rep)) return false;

    // 锚点：装配日志里能直接读到"换了几根骨、追加了几根、源里缺几根"。
    // 没有这行，"为什么骨骼面板里的名字变了"只能靠猜。
    VK_LOG_INFO("MANNEQUIN retarget ok=1 bones=%d ue=%d copy=%d synth=%d "
                "aux=%d missing=%d src=%d",
                rep.bones, rep.ueBones, rep.copied, rep.synthesized,
                rep.keptAux, rep.missing, model.skeleton->jointCount());
    if (!rep.missingSources.empty()) {
        VK_LOG_WARN("MANNEQUIN missing=%d first='%s'",
                    rep.missing, rep.missingSources.front().c_str());
    }

    model.skeleton = std::move(upgraded);
    if (report != nullptr) *report = rep;
    return true;
}

bool upgradeModelSkeletonForPlay(assets::Model& model) {
    // `MYVK_MANNEQUIN=0` 是 A/B 开关：对比"UE 骨名"与"原始骨名"两套装配，
    // 也是回归里区分"这次改动引起"与"本来就有"的手段。
    const char* off = std::getenv("MYVK_MANNEQUIN");
    if (off != nullptr && off[0] == '0') return false;
    return upgradeModelSkeletonToMannequin(model);
}

// ============================================================
// 导出独立骨架资产
// ============================================================
bool writeMannequinSkeletonAsset(const std::string& absPath, std::string* err) {
    const MannequinBoneDef* table = mannequinBones();
    const int n = mannequinBoneCount();
    if (table == nullptr || n <= 0) {
        if (err) *err = "empty mannequin table";
        return false;
    }

    assetio::SourceSkeletonAsset sk;
    sk.name = "SK_Mannequin";
    sk.joints.resize(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        assetio::SourceJoint& jt = sk.joints[static_cast<std::size_t>(i)];
        jt.name = table[i].name;
        jt.parent = table[i].parent;
        // 单位阵 = 参考骨架（见头文件里 `USkeleton` 那段说明）：
        // 骨名 + 父子关系是骨架资产的内容，静止姿势属于 mesh。
        jt.restLocal = glm::mat4(1.0f);
        jt.invBind = glm::mat4(1.0f);
        jt.externalChain = glm::mat4(1.0f);
    }
    // 表本身是拓扑序（parent 下标 < 自己），所以 topoOrder 就是 0..n-1。
    sk.topoOrder.resize(static_cast<std::size_t>(n));
    std::iota(sk.topoOrder.begin(), sk.topoOrder.end(), 0);
    sk.boneDigest = assetio::skeletonBoneDigest(sk.joints);

    // 挂点：`weapon_r`→`hand_r`、`weapon_l`→`hand_l`、`eye`→`head`。
    assetio::applyDefaultSockets(sk.joints, sk.sockets);

    const std::vector<std::uint8_t> bytes = assetio::writeSkelFile(sk);
    if (bytes.empty()) {
        if (err) *err = "writeSkelFile returned empty";
        return false;
    }
    std::ofstream f(absPath, std::ios::binary | std::ios::trunc);
    if (!f) {
        if (err) *err = "cannot open " + absPath;
        return false;
    }
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    if (!f.good()) {
        if (err) *err = "write failed " + absPath;
        return false;
    }
    VK_LOG_INFO("MANNEQUIN-SKEL wrote=%s bones=%d sockets=%zu bytes=%zu "
                "digest=%016llx",
                absPath.c_str(), n, sk.sockets.size(), bytes.size(),
                static_cast<unsigned long long>(sk.boneDigest));
    for (const assetio::SourceSocket& s : sk.sockets) {
        VK_LOG_INFO("MANNEQUIN-SKEL socket \"%s\" -> \"%s\"", s.name.c_str(),
                    s.boneName.c_str());
    }
    return true;
}

} // namespace character
