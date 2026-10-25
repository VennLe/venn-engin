// ============================================================
// animation/BoneAttachment.cpp
//
// 一次 pass：BoneAttachmentComponent → 找宿主 → 找关节 → 写 TransformComponent。
//
// ⚠ 写的是**局部**变换（TransformComponent 的语义）。有层级父时先除掉父
//   世界矩阵，没有时就是世界矩阵。写错这一处的症状是"挂点跟宿主走了两步"
//   （宿主是自己父时），或者"挂在别的物体下的挂点位置偏一个父位移"。
// ============================================================

#include "animation/BoneAttachment.h"

#include "assets/Mesh.h"
#include "assets/Model.h"
#include "core/Logger.h"
#include "ecs/Components.h"
#include "scene/Scene.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>

#include <cmath>

namespace animation {

namespace {

glm::mat4 trsEuler(const glm::vec3& offset, const glm::vec3& eulerDeg) {
    glm::mat4 m = glm::translate(glm::mat4(1.0f), offset);
    // 与 `TransformComponent::localMatrix` 相同的顺序（X → Y → Z）：
    // 否则"Inspector 里把挂点转 90°"与"组件里配 eulerDeg"不是同一件事。
    if (eulerDeg.x != 0.0f)
        m = glm::rotate(m, glm::radians(eulerDeg.x), glm::vec3(1, 0, 0));
    if (eulerDeg.y != 0.0f)
        m = glm::rotate(m, glm::radians(eulerDeg.y), glm::vec3(0, 1, 0));
    if (eulerDeg.z != 0.0f)
        m = glm::rotate(m, glm::radians(eulerDeg.z), glm::vec3(0, 0, 1));
    return m;
}

// mat4 → TransformComponent。用 decompose 而不是逐元素塞：
// 骨骼矩阵里可能带缩放（骨架节点带 scale 是常态），decompose 会把它分开；
// 手写的话"有缩放的骨架"上挂点会歪。
void writeTransform(ecs::TransformComponent& t, const glm::mat4& m) {
    glm::vec3 scale(1.0f), translation(0.0f), skew(0.0f);
    glm::quat orient(1.0f, 0.0f, 0.0f, 0.0f);
    glm::vec4 perspective(0.0f);
    glm::decompose(m, scale, orient, translation, skew, perspective);
    t.position = translation;
    t.rotation = glm::eulerAngles(orient);
    t.scale = scale;
}

// 宿主的"模型→引擎"变换：取第一个有网格的子网格的 `SubMesh::transform`。
//
// 为什么可以随便取一个：蒙皮路径本身要求所有子网格共享同一个
// meshNodeWorld（否则 `invMeshNode` 与 `meshNodeWorld` 约不掉，蒙皮早就
// 已经歪了）。所以这不是新增假设，是沿用现有路径的前提。
glm::mat4 modelToEngine(const assets::Model& model) {
    for (const assets::SubMesh& sm : model.subMeshes)
        if (sm.mesh) return sm.transform;
    return glm::mat4(1.0f);
}

} // namespace

glm::mat4 attachmentMatrix(const assets::Skeleton& skel,
                           const std::vector<glm::mat4>& jointMatrices,
                           int joint, const glm::mat4& hostWorld,
                           const glm::mat4& subTransform,
                           const glm::vec3& offset, const glm::vec3& eulerDeg) {
    if (joint < 0 || joint >= skel.jointCount()) return glm::mat4(1.0f);
    if (joint >= static_cast<int>(jointMatrices.size())) return glm::mat4(1.0f);

    // subTransform · jointMat · invBind⁻¹ = YupToZup · world(joint)（见头文件）
    const glm::mat4 bone =
        subTransform * jointMatrices[static_cast<size_t>(joint)] *
        glm::inverse(skel.joints[static_cast<size_t>(joint)].invBind);
    return hostWorld * bone * trsEuler(offset, eulerDeg);
}

AttachmentStats updateBoneAttachments(scene::Scene& scene) {
    ecs::World& w = scene.world();
    AttachmentStats st;

    w.each<ecs::BoneAttachmentComponent>(
        [&](ecs::Entity e, ecs::BoneAttachmentComponent& a) {
            if (!a.enabled) return;

            // ---- 1) 宿主 ----
            const ecs::Entity host = scene.find(a.hostName);
            if (!host.valid()) {
                if (st.unresolved == 0) st.firstProblem = "no-host:" + a.hostName;
                ++st.unresolved;
                return;
            }
            const auto* sk = w.get<ecs::SkinnedMeshComponent>(host);
            if (!sk || !sk->model || !sk->model->skeleton ||
                sk->jointMatrices.empty()) {
                if (st.unresolved == 0)
                    st.firstProblem = "host-not-skinned:" + a.hostName;
                ++st.unresolved;
                return;
            }
            const assets::Skeleton& skel = *sk->model->skeleton;

            // ---- 2) 关节 ----
            const int joint = skel.findJoint(a.boneName, assets::kJointMatchDefault);
            if (joint < 0) {
                if (st.unresolved == 0) st.firstProblem = "no-bone:" + a.boneName;
                ++st.unresolved;
                return;
            }

            // ---- 3) 矩阵 ----
            const glm::mat4 hostWorld = scene.worldMatrix(host);
            const glm::mat4 world =
                attachmentMatrix(skel, sk->jointMatrices, joint, hostWorld,
                                 modelToEngine(*sk->model), a.offset, a.eulerDeg);

            // ---- 4) 转成局部（有层级父时除掉父世界矩阵）----
            glm::mat4 local = world;
            if (const auto* h = w.get<ecs::HierarchyComponent>(e);
                h && h->parent != ecs::kInvalidEntity) {
                const glm::mat4 parentWorld =
                    scene.worldMatrix(w.handle(h->parent));
                const float det = glm::determinant(parentWorld);
                // 父世界矩阵不可逆（0 缩放）→ 保持原样，不要写出 NaN 变换：
                // 一个 NaN 位置会让整个物体的包围盒变成 NaN，于是它从视口里
                // "消失"，而日志里只有剔除统计变了。
                if (std::abs(det) > 1.0e-12f)
                    local = glm::inverse(parentWorld) * world;
            }

            if (auto* t = w.get<ecs::TransformComponent>(e)) {
                writeTransform(*t, local);
                ++st.placed;
            }
        });

    return st;
}

} // namespace animation
