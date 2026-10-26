#pragma once
// ============================================================
// assets/Skeleton —— glTF 蒙皮与动画的运行时表示
//
// 由 MeshLoader 从 tinygltf 解析而来，挂在 assets::Model 上。
// 一个 Skeleton 描述：
//   * joints       —— 骨骼树（父下标 + 静止局部变换 + 逆绑定矩阵）
//   * clips        —— 动画片段（Walk / Idle ...）
//   * invMeshNode  —— mesh 节点世界矩阵的逆（蒙皮公式的空间换算）
//
// sample() 把"动画时刻 + 骨架"变换成一组 **joint 矩阵**：
//
//     jointMat(j) = invMeshNode * world(j) * invBind(j)
//
// 顶点着色器里直接 Σ w·jointMat(j)·pos。注意这里的 world(j) 是
// **glTF 场景空间**的（不含引擎的 Y-up→Z-up 旋转）——那个旋转在
// push.model（= 实体世界 × SubMesh::transform）里，两边相乘时
// meshNodeWorld 与 invMeshNode 恰好约掉，所以蒙皮路径和静态网格
// 路径能共用同一套 draw 代码，只是 PushConstants 里多一个 jointBase。
// ============================================================

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <map>
#include <string>
#include <vector>

namespace assets {

// ---- 动画轨道：一条 glTF channel + 它的 sampler ----
struct AnimTrack {
    enum Path : int { Translation = 0, Rotation = 1, Scale = 2 };

    int joint = -1;             // 目标 joint（Skeleton::joints 下标）
    Path path = Translation;
    std::vector<float> times;   // 采样时刻（秒，单调递增）
    std::vector<glm::vec4> values;
    // T/S 只用 xyz；R 是四元数 (x,y,z,w)。统一 vec4 是为了插值代码一套。
    bool step = false;          // glTF STEP 插值：不插值，取左端点
};

struct AnimationClip {
    std::string name;
    float duration = 0.0f;      // 最后一个采样的时刻
    std::vector<AnimTrack> tracks;

    // 轨道按 joint 预分组没必要 —— sample 时 O(tracks) 扫一遍即可，
    // 63 条通道的角色每帧也只是 63 次二分查找。
};

// ---- 按名字找关节的匹配选项（2026-10-25，第八刀）----
//
// 为什么要有标志位而不是三个函数：调用方要的组合是"精确 vs 包含"×
// "要不要跳过末端辅助骨"，三份实现各漏一个组合的历史就是 §16.1 记的那件事。
enum JointMatchFlags : unsigned {
    kJointMatchExact = 0u,
    // 名字比较不区分大小写。**几乎总是要开**：Mixamo 导出的骨叫
    // `mixamorig:Head`，美术手填的是 `head`。
    kJointMatchCaseInsensitive = 1u << 0,
    // 精确没命中时退化成"包含"。开了才能在没记住 `mixamorig:` 前缀时也能用。
    kJointMatchSubstring = 1u << 1,
    // 跳过 `Head_end` / `*_tip` / `*Nub` 这类**末端辅助骨**。它们的位置在
    // 骨尖上，拿它当"头"会让旋转轴跑偏（DCC 里几乎人手一根）。
    //
    // ⚠ 只作用于**包含匹配**（退化的那一轮）。精确匹配**永远不过滤** ——
    //   否则显式要 `Head_end` 的人会拿到 -1，"我要的就是这根骨"变成不可能。
    //   这条区分是第八刀的自检抓出来的：一开始两轮都过滤，
    //   `findJoint("Head_end")` 直接返回 -1。
    kJointMatchSkipEndTips = 1u << 2,
};
// 默认：不分大小写 + 允许包含 + **跳过末端辅助骨**。
// 带上第三个的理由：在"返回 -1"和"返回一根错误的骨"之间，前者能查、
// 后者只会表现成"这个角色的头转轴是歪的"。
constexpr unsigned kJointMatchDefault = kJointMatchCaseInsensitive |
                                        kJointMatchSubstring |
                                        kJointMatchSkipEndTips;

// ---- 骨骼 ----
struct Skeleton {
    struct Joint {
        std::string name;
        int parent = -1;             // 父 joint 下标（-1 = 骨架根）
        glm::mat4 restLocal{1.0f};   // 静止局部变换（轨道没覆盖的分量用它）
        glm::mat4 invBind{1.0f};     // inverseBindMatrices
        // glTF 场景根 → 本 joint 之外那条祖先链的静止世界矩阵
        //（骨架通常挂在 armature 节点下，这一两层"外链"不在 joints 里）
        glm::mat4 externalChain{1.0f};
    };

    std::vector<Joint> joints;
    std::vector<int> topoOrder;      // 保证 parent 先于 child 的遍历序
    glm::mat4 invMeshNode{1.0f};
    std::map<int, int> nodeMap;      // glTF node 下标 → joint 下标

    // 骨架空间 → 引擎世界空间（Z-up）的变换。**默认恒等**。
    //
    // 为什么需要这个字段（2026-10-26，B3 根运动）：
    //   两条加载路径给出的 `joints` 不在同一个坐标系里 ——
    //     · glTF：`restLocal` / `externalChain` 取的是**文件原始空间**
    //       （Y-up，见 MeshLoader.cpp 的 parseSkeleton），Y-up→Z-up 的旋转
    //       藏在 `invMeshNode` 里，两者相乘时才约掉。所以
    //       `poseToWorld` 的输出是 **Y-up 空间**。
    //     · FBX ：ufbx 用 `target_axes = right_handed_z_up` 在加载时就把
    //       一切转成了引擎的 Z-up（FbxLoader.cpp 文件头），`invMeshNode`
    //       是单位阵。所以 `poseToWorld` 的输出**已经是 Z-up**。
    //   平时这个差别看不出来（渲染链里 `invMeshNode` 与 `push.model`
    //   恰好抵消），但根运动要把位移**写进 `TransformComponent`**
    //   —— 那是引擎世界空间。不知道骨架在哪个空间，写进去的位移就会
    //   "跑着跑着朝天上飞"（Y-up 的 y 当成 Z-up 的 y）。
    //
    // ⚠ 这个量只描述**旋转+缩放**，不含平移（骨架根在哪个原点不影响
    //   方向）。用的时候按矩阵用就行，别当"模型的世界矩阵"。
    glm::mat4 skeletonToEngine{1.0f};

    std::vector<AnimationClip> clips;

    int jointCount() const { return static_cast<int>(joints.size()); }
    int nodeToJoint(int nodeIdx) const {
        const auto it = nodeMap.find(nodeIdx);
        return it == nodeMap.end() ? -1 : it->second;
    }
    const AnimationClip* findClip(const std::string& name) const {
        for (const auto& c : clips)
            if (c.name == name) return &c;
        return nullptr;
    }

    // ---- 按名字找关节（全仓库唯一实现，2026-10-25）----
    //
    // 抽取原因：这套"精确（不分大小写）→ 退化成包含 → 跳过 end/tip"的
    // 逻辑本来有两份 —— `animation/LookAt.cpp` 的 resolveLookAtBone，和
    // `animation/SpringBone.cpp` 里手写的前缀扫描。两份的差别只在
    // "找一根" vs "找一批"，而"找一根"那份的两个退化分支是被真实资产
    // 逼出来的（`mixamorig:Head`），新加的 IK / 挂点如果各自复制一遍，
    // 迟早有一份漏掉某个分支 —— 症状是"某个角色的手挂不上"。
    //
    // 返回 joint 下标；找不到返回 **-1**（不是 0 —— 静默取第 0 根会让
    // "名字写错"表现成"在根骨骼上转"，几乎不可能从画面上反推）。
    int findJoint(const std::string& name,
                  unsigned flags = kJointMatchDefault) const;

    // 批量：所有名字以 `prefix` 开头（不区分大小写）的 joint 下标，
    // **按 joint 下标升序**（不是 topoOrder —— 调用方若要拓扑序自己排）。
    // prefix 为空 → 不填任何东西（不是"填全部"：空前缀匹配一切，
    // 而弹簧骨那里"空前缀"的语义是"用默认前缀表"，不是"整具骨架"）。
    void findJointsWithPrefix(const std::string& prefix,
                              std::vector<int>& out) const;

    // 采样出 jointCount() 个 joint 矩阵（invMeshNode * world * invBind）。
    // clip 传 nullptr → 静止姿势（rest pose）。out 会被 resize 到关节数。
    void sample(const AnimationClip* clip, float time,
                std::vector<glm::mat4>& out) const;
};

// 单条轨道采样：time 处的值（线性/球面插值，STEP 取左端点）
glm::vec4 sampleTrack(const AnimTrack& t, float time);

} // namespace assets