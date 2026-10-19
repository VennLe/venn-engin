// ============================================================
// scene/ModelInstance.cpp —— Model → 场景实体（唯一装配路径）
// ============================================================

#include "scene/ModelInstance.h"

#include "core/Logger.h"

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

        if (auto* t = w.get<ecs::TransformComponent>(e))
            applySubTransform(*t, sub.transform);

        scene.setParent(e, root);
        out.entities.push_back(e);
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

    // ---- 自检锚点（每次导入一行，自动化断言就靠它）----
    if (!opt.sourcePath.empty()) {
        VK_LOG_INFO("MODEL-INSTANCE path=%s root=%u meshes=%d skinned=%d "
                    "joints=%d parent=%d",
                    opt.sourcePath.c_str(),
                    static_cast<unsigned>(root.id),
                    static_cast<int>(out.entities.size()),
                    out.skinned ? 1 : 0, out.joints,
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
