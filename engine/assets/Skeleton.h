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

    // 采样出 jointCount() 个 joint 矩阵（invMeshNode * world * invBind）。
    // clip 传 nullptr → 静止姿势（rest pose）。out 会被 resize 到关节数。
    void sample(const AnimationClip* clip, float time,
                std::vector<glm::mat4>& out) const;
};

// 单条轨道采样：time 处的值（线性/球面插值，STEP 取左端点）
glm::vec4 sampleTrack(const AnimTrack& t, float time);

} // namespace assets