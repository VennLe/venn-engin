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

    bool valid() const { return !subMeshes.empty(); }
    bool skinned() const { return skeleton && skeleton->jointCount() > 0; }
};

} // namespace assets
