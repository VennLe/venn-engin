#pragma once
// ============================================================
// animation/BoneAttachment —— 骨骼挂点（2026-10-25 第八刀）
//
// 让一个实体"钉"在某根骨骼上：武器挂右手、帽子挂头、特效挂胸口、
// 相机挂肩膀。每帧把骨骼的世界矩阵换算成那个实体的 TransformComponent。
//
// ------------------------------------------------------------
// 为什么不用"把子实体挂到骨骼节点下"（也就是真正的父子层级）
// ------------------------------------------------------------
// 骨骼**不是场景实体** —— 它们在 `assets::Skeleton` 里是一棵纯数据的树，
// 由 `poseToJoints` 每帧算成矩阵。想让它变成场景实体有两条路：
//   · 给每根骨建一个实体 → 一个角色 60~90 个实体，层级树爆炸，
//     撤销/序列化/Inspector 全部要跟着改；
//   · 让 TransformComponent 能表达"父是一个 joint" → 层级系统要知道骨架，
//     而层级系统现在完全不认识渲染层。
// 挂点是**一个组件 + 一次矩阵写法**就能解决的事，不值得为它改层级系统。
//
// ------------------------------------------------------------
// ⚠ 三个必须说清楚的约定
// ------------------------------------------------------------
// ① **宿主按名字找，不做下标缓存。**
//    实体 id 是运行时序号（会随删除/撤销变化），把它缓存进组件等于给自己
//    埋一个"删除后 id 复用"的坑。名字在场景 JSON 里是稳定的，代价是每帧
//    一次线性查找 —— 挂点通常个位数，可以忽略。
//
// ② **挂点实体不能是宿主的子孙。**
//    本组件每帧直接写它的 TransformComponent（局部），如果它同时在宿主的
//    层级下面，就会"骨骼变换 × 父链变换"叠加两次 —— 症状是挂点绕着宿主
//    飞。层级父存在时这里会先除掉父世界矩阵（见 .cpp），但那只对
//    "挂在别的物体下"有意义，不解决"挂在宿主下"。
//
// ③ **写入点在 `AnimatorSystem::tick` 末尾，一处。**
//    和 `SkinnedMeshComponent::time` 的镜像同一个道理：多一个写入点，
//    症状就是"挂点在两个位置之间跳"（编辑器与运行时各写一次）。
// ============================================================

#include "assets/Skeleton.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace scene {
class Scene;
}

namespace animation {

// 挂点的**引擎世界**矩阵（Z-up，可以直接当 TransformComponent 的来源）：
//
//     hostWorld · subTransform · jointMatrices[j] · invBind(j)⁻¹
//               · translate(offset) · rotateXYZ(eulerDeg)
//
// 中间那三段的来历（**这是本文件唯一需要懂的一行数学**）：
//
//     jointMatrices[j] = invMeshNode · world(j) · invBind(j)     ← 蒙皮用
//     subTransform     = YupToZup · meshNodeWorld
//     invMeshNode      = meshNodeWorld⁻¹
//     ⇒ subTransform · jointMat · invBind⁻¹
//         = YupToZup · meshNodeWorld · meshNodeWorld⁻¹ · world(j)
//         = YupToZup · world(j)                                  ← 模型局部
//
// ⚠ `jointMatrices` 本身**不是**骨骼的世界变换。直接拿它当骨骼矩阵用的
//   症状是"位置对、朝向错"，而且只在那根骨的静止朝向不是单位矩阵时看得
//   出来（invBind 里带着静止姿势的旋转）。
// ⚠ `subTransform` 必须取自一个**蒙皮**子网格：它决定 Y-up→Z-up 那一转。
//   传静态网格的 transform 会把挂点转到侧躺。
glm::mat4 attachmentMatrix(const assets::Skeleton& skel,
                           const std::vector<glm::mat4>& jointMatrices,
                           int joint, const glm::mat4& hostWorld,
                           const glm::mat4& subTransform,
                           const glm::vec3& offset, const glm::vec3& eulerDeg);

// 每帧跑一遍所有 `BoneAttachmentComponent`。由 `AnimatorSystem::tick` 在
// 关节矩阵全部刷新之后调用。
//
// 为什么把统计**回传**而不是在这里直接打日志：日志要按"数量变化"限流，
// 而限流状态必须跟着 `AnimatorSystem` 的生命周期（一个函数内 static 会让
// 两个场景共用一个计数，测试里立刻串味）。
struct AttachmentStats {
    int placed = 0;          // 本帧真的摆好的
    int unresolved = 0;      // 宿主 / 骨名对不上的
    std::string firstProblem; // 第一条问题的原因（`no-host:` / `no-bone:` …）
};
AttachmentStats updateBoneAttachments(scene::Scene& scene);

} // namespace animation
