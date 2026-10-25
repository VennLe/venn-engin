#pragma once
// ============================================================
// scene/ModelInstance —— 把一个 assets::Model 装配进场景
//
// 为什么要有这一层（2026-10-18，角色链 A0-1）：
//   在这之前，"Model → 场景实体"这件事有两份各自的实现 ——
//   `editor/EditorScene::instantiateModel`（Content 拖进视口）和
//   `editor/CharacterWindow::instanceModelInto`（角色预览窗口的私有副本）。
//   而**只有后者**会建 `SkinnedMeshComponent`，且只作用于它自己的预览场景。
//   后果：把一个带骨骼的 FBX 拖进主场景，得到的是一堆静态网格摆成 T-pose；
//   Game.exe 走同一条导入路径，所以运行时**永远看不到蒙皮角色**。
//
//   两份实现合并到这里之后，"Model 进场景"只有一条代码路径，
//   蒙皮判定也只剩一处 —— 不会再出现"某一条入口忘了建组件"。
//
// 分层：scene 依赖 assets 是既有事实（`Scene::meshLocalBounds` 就收
// `const assets::Mesh&`），这里不引入新的方向。
// ============================================================

#include "assets/Model.h"
#include "ecs/Components.h"
#include "scene/Scene.h"

#include <string>
#include <vector>

namespace scene {

// 装配参数。默认值 = "放到原点、自建根节点、蒙皮自动播放"，
// 也就是**游戏里想要的样子**；编辑器预览窗口会覆盖其中几项。
struct ModelInstanceOptions {
    // 每个 SubMesh 实体的名字后缀。
    //   EditorScene 用 "_inst"（区分同名网格）
    //   CharacterWindow 用 ""（预览树里干净些）
    std::string subNameSuffix = "_inst";

    // 挂到哪个父节点下。**无效 → 自建根实体**（用 rootName 或 model->name），
    // 并把 position 写进根的 TransformComponent。
    // 有效 → 不建根，直接挂进去，蒙皮组件加在 parent 上。
    ecs::Entity parent;

    // parent 无效时根节点的位置（Z-up，世界空间）。
    glm::vec3 position{0.0f};

    // parent 无效时根节点的名字。空 → 用 model->name，再空 → "model"。
    std::string rootName;

    // **模型的"正面"绕 Z 轴的补偿角（度）**。装配时先把每个 SubMesh 的
    // 局部变换预乘一个绕 Z 的旋转，于是"实体 +X = 模型正面"成为约定。
    //
    // 为什么需要它（2026-10-22，P5 实测）：引擎的实体朝向语义是
    // `rotation.z = atan2(dir.y, dir.x)`，即 yaw=0 时正面朝 **+X**；
    // 而 FBX/glTF 是 Y-up、源模型（Mixamo 等）正面朝 **+Z**，
    // 经 ufbx 的 `right_handed_z_up` 映射 (x,-z,y) 之后落在 **-Y**。
    // 不补偿的后果不是"稍微歪"，而是**横着平移**：人朝 -Y、却往 +X 走，
    // 从镜头看就是侧身滑行 —— 用户原话"没有朝它的视线方向去移动"。
    // 把 -Y 转到 +X 需要 **+90°**。
    //
    // 0 = 不补偿（默认，老场景逐位不变）。非 FBX 来源、或已经把正面烤成
    // +X 的资产，保持 0。
    float yawOffsetDeg = 0.0f;

    // 蒙皮组件建出来后 `playing` 的初值。
    //   游戏 / 编辑器主场景 → true（拖进来就在动）
    //   角色预览窗口     → false（由它自己的时间轴滑条决定）
    bool autoPlay = true;

    // 日志与断言用的资产标识（AssetManager 的 key 或相对路径）。
    // 空 → 不打 `MODEL-INSTANCE` 那行。
    std::string sourcePath;

    // **把 `position` 解释成"地面点"而不是"模型原点"**。
    //
    // 为什么需要这个开关：模型的 pivot 在**几何中心**（见
    // docs/external_model_pipeline.md §1），所以 `position = (0,0,0)` 会让
    // 角色下半身埋进地里、上半身露出地面 —— 拖一个 1.8 m 的人进来，第一眼
    // 看到的是"半个人"。编辑器想要的语义是"脚踩在这块地上"，于是把它打开，
    // 装配完再按世界 AABB 把根节点整体抬 `-min.z`。
    //
    // 默认 false：`CharacterWindow` 的预览场景要的是"绕原点转"，抬头会让
    // 模型在预览里跳一下；运行时的玩家挂载有自己的 lift 计算（还要跟胶囊
    // 对齐），也不需要这个。
    bool dropToGround = false;
};

struct ModelInstanceResult {
    ecs::Entity root;                    // 模型根（parent 有效时 = parent）
    std::vector<ecs::Entity> entities;   // 每个 SubMesh 一个
    bool skinned = false;                // 是否建了 SkinnedMeshComponent
    int joints = 0;                      // 骨架关节数（非蒙皮为 0）
    int morphChannels = 0;               // MorphRig 滑条数（无 morph 为 0）
};

// 装配。model 为空 / 无效时返回空结果（root 无效）。
//
// 行为契约：
//   * 每个 SubMesh 建一个实体，挂 MeshComponent + MaterialComponent，
//     TransformComponent 用 SubMesh::transform 分解出的 TRS
//     （glTF/FBX 的节点世界变换 → 相对模型根的局部变换）。
//   * model->skinned() 时在根实体上挂 SkinnedMeshComponent
//     （已有则覆盖）—— **这是全仓库唯一的创建点**。
//   * 旧场景不受影响：本函数只在"有人拖模型进来"时被调用，
//     加载存档走 SceneSerializer 自己的路径。
ModelInstanceResult instantiateModel(Scene& scene, assets::Model* model,
                                     const ModelInstanceOptions& opt);

} // namespace scene
