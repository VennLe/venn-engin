// ============================================================
// scene/ModelInstance.cpp —— Model → 场景实体（唯一装配路径）
// ============================================================

#include "scene/ModelInstance.h"

#include "core/Logger.h"

#include "assets/Mesh.h"   // boundsMin()/boundsMax()（dropToGround 要用）

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <algorithm>
#include <exception>
#include <string>

namespace scene {

namespace {

// SubMesh::transform 是"节点世界变换"（已累乘 glTF/FBX 的父链），
// 而模型根实体在原点、姿态为单位阵 → 它就是**相对根的局部变换**，
// 直接分解成 TRS 塞进 TransformComponent 即可。
void applySubTransform(ecs::TransformComponent& t, const glm::mat4& m) {
    glm::vec3 scale(1.0f), translation(0.0f), skew(0.0f);
    glm::quat orient(1.0f, 0.0f, 0.0f, 0.0f);
    glm::vec4 perspective(0.0f);
    glm::decompose(m, scale, orient, translation, skew, perspective);
    t.position = translation;
    // 引擎是 Z-up，但这里只是把**同一个空间**里的变换换个表示 ——
    // 不做任何轴交换，与改动前的两份实现一致。
    t.rotation = glm::eulerAngles(orient);
    t.scale = scale;
}

} // namespace

ModelInstanceResult instantiateModel(Scene& scene, assets::Model* model,
                                     const ModelInstanceOptions& opt) {
    ModelInstanceResult out;
    if (!model || !model->valid()) return out;

    ecs::World& w = scene.world();

    // ---- 根节点：parent 有效就用它，否则自建 ----
    ecs::Entity root = opt.parent;
    if (!root.valid()) {
        const std::string name =
            !opt.rootName.empty() ? opt.rootName
                                  : (!model->name.empty() ? model->name
                                                          : std::string("model"));
        root = scene.createObject(name);
        if (auto* t = w.get<ecs::TransformComponent>(root))
            t->position = opt.position;
    }
    out.root = root;

    // ---- 每个 SubMesh 一个实体 ----
    out.entities.reserve(model->subMeshes.size());
    for (const auto& sub : model->subMeshes) {
        if (!sub.mesh) continue;

        const std::string base = sub.name.empty() ? std::string("mesh")
                                                  : sub.name;
        ecs::Entity e = scene.createObject(base + opt.subNameSuffix);

        ecs::MeshComponent mc;
        mc.mesh = sub.mesh;
        w.add<ecs::MeshComponent>(e, mc);
        w.add<ecs::MaterialComponent>(e, ecs::MaterialComponent{sub.material});

        if (auto* t = w.get<ecs::TransformComponent>(e)) {
            // 朝向补偿（见 opt.yawOffsetDeg）：**预乘**一个绕 Z 的旋转，
            // 即 local = Rz(offset) * subTransform。
            // ⚠ 不能改成"给 t->rotation.z 加上 offset"：子变换自己可能带
            //   x/y 旋转，欧拉角逐分量相加在那种情况下不等于绕世界 Z 转。
            //   预乘矩阵再 decompose 才是同一个几何。
            glm::mat4 m = sub.transform;
            if (opt.yawOffsetDeg != 0.0f) {
                m = glm::rotate(glm::mat4(1.0f),
                                glm::radians(opt.yawOffsetDeg),
                                glm::vec3(0.0f, 0.0f, 1.0f)) * m;
            }
            applySubTransform(*t, m);
        }

        scene.setParent(e, root);
        out.entities.push_back(e);
    }

    // ---- 落地对齐：把 position 当成"地面点"（见 opt.dropToGround 注释）----
    //
    // 放在这里而不是让调用方各算一遍：`min.z` 必须按**装配之后的实际子网格
    // 变换**求，调用方再算一次就会得到第二套口径（拖放路径的
    // `alignImportToGround` 走的是 picking 的世界 AABB，两者必须一致 ——
    // 它是幂等的，所以这里的抬升到它那里会算出 shift.z=0）。
    if (opt.dropToGround && !opt.parent.valid()) {
        glm::vec3 lo(0.0f), hi(0.0f);
        bool any = false;
        for (const auto& sub : model->subMeshes) {
            if (!sub.mesh) continue;
            for (int c = 0; c < 8; ++c) {
                const glm::vec3 corner(
                    (c & 1) ? sub.mesh->boundsMax().x : sub.mesh->boundsMin().x,
                    (c & 2) ? sub.mesh->boundsMax().y : sub.mesh->boundsMin().y,
                    (c & 4) ? sub.mesh->boundsMax().z : sub.mesh->boundsMin().z);
                const glm::vec3 p = glm::vec3(sub.transform * glm::vec4(corner, 1.0f));
                lo = any ? glm::min(lo, p) : p;
                hi = any ? glm::max(hi, p) : p;
                any = true;
            }
        }
        if (any) {
            if (auto* t = w.get<ecs::TransformComponent>(root)) {
                const float lift = opt.position.z - lo.z;
                t->position.z += lift;
                if (!opt.sourcePath.empty())
                    VK_LOG_INFO("MODEL-GROUND path=%s bounds.z=%.3f..%.3f "
                                "lift=%.3f (bottom -> %.3f)",
                                opt.sourcePath.c_str(),
                                static_cast<double>(lo.z),
                                static_cast<double>(hi.z),
                                static_cast<double>(lift),
                                static_cast<double>(opt.position.z));
            }
        }
    }

    // ---- 蒙皮：挂在**模型根**上（所有 SubMesh 共享一份骨架/动画状态）----
    //
    // 这是全仓库唯一创建 SkinnedMeshComponent 的地方（A0-1 之前它在
    // CharacterWindow 的私有函数里，于是主场景根本建不出来）。
    if (model->skinned()) {
        ecs::SkinnedMeshComponent sk;
        sk.model = model;
        sk.clipName.clear();       // 空 = 静止姿势，由上层改成想要的 clip
        sk.time = 0.0f;
        sk.speed = 1.0f;
        sk.loop = true;
        sk.playing = opt.autoPlay;
        // 自动播放时挑第一个 clip：否则 time 会推进，但 clip 是空的
        // → 采样出 rest pose → 看起来"还是不动"（A0 总验收要求真的在走）。
        // 编辑器预览场景传 autoPlay=false，clip 由它的下拉框决定。
        if (opt.autoPlay && model->skeleton && !model->skeleton->clips.empty())
            sk.clipName = model->skeleton->clips.front().name;

        if (auto* p = w.get<ecs::SkinnedMeshComponent>(root))
            *p = sk;
        else
            w.add<ecs::SkinnedMeshComponent>(root, sk);

        out.skinned = true;
        out.joints = model->skeleton->jointCount();
    }

    // ---- 表情滑条（§15）：挂点与蒙皮**相同**（模型根）----
    //
    // 为什么不挂在每个 SubMesh 实体上：滑条是**模型级**概念。「眨眼」在
    // 皮肤桶里是 target_3、在睫毛桶里是 target_1，同一个滑条驱动多个
    // 子网格的多个 target（见 MorphRig 的跨桶合并）。挂在子网格上会让
    // 一个「眨眼」变成 19 根各管一段的滑条。
    //
    // 与蒙皮**不共用**一个组件：静态网格也可能带 morph（虽然罕见），
    // 而且 MorphComponent 不参与动画姿态求解，混在一起会让"这段代码
    // 该不该在 AnimatorSystem 里跑"变成一个问题。
    if (!model->morphRig.empty()) {
        ecs::MorphComponent mp;
        mp.model = model;
        // 按通道数预置 0：`setMorphWeight` 允许短表，但预置好省得第一帧
        // 求值时还要 resize（求值发生在渲染的绘制列表构建里，不该分配）。
        mp.weights.assign(model->morphRig.channelCount(), 0.0f);
        if (auto* p = w.get<ecs::MorphComponent>(root))
            *p = mp;
        else
            w.add<ecs::MorphComponent>(root, mp);
        out.morphChannels = static_cast<int>(model->morphRig.channelCount());
    }

    // ---- 自检锚点（每次导入一行，自动化断言就靠它）----
    if (!opt.sourcePath.empty()) {
        VK_LOG_INFO("MODEL-INSTANCE path=%s root=%u meshes=%d skinned=%d "
                    "joints=%d morph=%d parent=%d",
                    opt.sourcePath.c_str(),
                    static_cast<unsigned>(root.id),
                    static_cast<int>(out.entities.size()),
                    out.skinned ? 1 : 0, out.joints, out.morphChannels,
                    opt.parent.valid() ? 1 : 0);

        // clip 清单单独一行：调试"模型进来了但不动"时，第一件事就是确认
        // 到底有没有 clip、叫什么名字（clipName 空 → 采样出 rest pose，
        // 看起来和"没反应"一模一样）。只报前 4 个，避免刷屏。
        if (out.skinned && model->skeleton) {
            const auto& clips = model->skeleton->clips;
            std::string names;
            const size_t shown = clips.size() < 4 ? clips.size() : 4;
            for (size_t i = 0; i < shown; ++i) {
                if (i) names += ",";
                names += clips[i].name;
            }
            VK_LOG_INFO("MODEL-CLIPS root=%u n=%d first=%s",
                        static_cast<unsigned>(root.id),
                        static_cast<int>(clips.size()), names.c_str());
        }
    }
    return out;
}

} // namespace scene
