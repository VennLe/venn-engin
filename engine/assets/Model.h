#pragma once
// ============================================================
// assets/Model —— 模型（由若干 SubMesh 组成）
//
// glTF 的一个 mesh 可能含多个 primitive（各自独立材质），
// 且节点带层级变换。加载后每个 primitive 展开成一个 SubMesh，
// 并把节点的**世界变换**烘进 SubMesh::transform。
//
// 这样上层（editor/EditorScene.cpp 的 instantiateModel）只需为每个 SubMesh
// 建一个实体、把 transform 写进 TransformComponent 即可，不必理解 glTF 的
// 节点树结构。
// ============================================================

#include "assets/MorphRig.h"
#include "assets/Skeleton.h"
#include "rhi/VulkanCommon.h"

#include <memory>
#include <string>
#include <vector>

namespace assets {

class Mesh;
struct Material;

struct SubMesh {
    Mesh* mesh = nullptr;
    Material* material = nullptr;
    glm::mat4 transform{1.0f};  // 所属节点的世界变换（已累乘父链）
    std::string name;
};

struct Model {
    std::string name;
    std::vector<SubMesh> subMeshes;

    // 所有顶点位置的包围球（用于相机取景 / 光源范围估算）
    glm::vec3 center{0.0f};
    float radius = 1.0f;

    // ---- 蒙皮（glTF skin + animations）----
    // 所有 primitive 共享一份骨架；为空 = 静态模型。
    // 渲染时 Renderer::updateSkinning 按 skeletonOwner（本 Model 指针）
    // 采样出关节矩阵写入 SSBO，再按 slot 槽位被各 primitive 的绘制用。
    std::shared_ptr<Skeleton> skeleton;

    // ---- 表情/形变（§15）----
    // 同名滑条已跨子网格合并。`empty()` = 这个模型没有表情。
    // 装载时由 VMeshLoader 建一次，运行期只读（求值走 `evaluate`）。
    MorphRig morphRig;

    bool valid() const { return !subMeshes.empty(); }
    bool skinned() const { return skeleton && skeleton->jointCount() > 0; }

    // 网格指针 → 子网格下标（-1 = 不属于本模型）。
    //
    // 为什么需要它：`MorphRig::evaluate` 的输出是**按子网格**分行的，而
    // 渲染层手里只有 `Mesh*`（实体上挂的是 MeshComponent）。子网格数通常
    // 十几到几十，线性扫描足够；建 map 反而要跟着 subMeshes 的生命周期走。
    int subMeshIndexOf(const Mesh* mesh) const {
        for (std::size_t i = 0; i < subMeshes.size(); ++i)
            if (subMeshes[i].mesh == mesh) return static_cast<int>(i);
        return -1;
    }
};

} // namespace assets
