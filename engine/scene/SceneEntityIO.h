#pragma once
// ============================================================
// scene/SceneEntityIO —— **单实体** ↔ JSON
//
// 存在的唯一理由：Prefab 的资产格式与场景文件**同构**（平铺 entities +
// parent 下标），而"哪些组件、哪些字段"这套知识在 `SceneSerializer.cpp`
// 里已经写了 600 多行。让 Prefab 再写一遍 = 以后每加一个组件都要改两处，
// 而漏改**不会报错**，只会让 Prefab 默默丢字段（存一次就永久丢）。
//
// 所以把那段代码抽成 `entityToJsonImpl` / `entityFromJsonImpl`（在
// SceneSerializer.cpp 的匿名 namespace 里），再由这两个壳递出来。
// 场景序列化器自己也改走这两个函数 —— **只有一份实现**。
//
// 为什么单独一个头文件而不是塞进 SceneSerializer.h：
//   `SceneSerializer.h` 被十几个 TU include，往里加 `<nlohmann/json.hpp>`
//   会把 30 MB 的模板塞进每一次编译。只有 Prefab 需要 JSON 类型，就只在
//   这个头里 include。
// ============================================================

#include "ecs/Entity.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace ecs {
class World;
}
namespace assets {
class AssetManager;
}
namespace scene {
class Scene;
}

namespace scene {

// 一个实体的"外壳"：transform / visible / castShadow / locked。
//
// 这三样在场景加载里是**第一遍**（要先有变换才能算父子、算包围盒），而
// `entityFromJson` 只处理"第二遍"的组件。Prefab 实例化与同步同样需要先落
// 外壳再落组件，所以这段单独成函数 —— 否则就会有第二份 transform 解析，
// 而漏掉 `scale` 默认值这类差别**在画面上是静默的**。
//
// `e` 必须已经由 `Scene::createObject` 建好（它自带 Name/Transform/Visibility）。
void entityApplyShell(Scene& scene, const nlohmann::json& je, ecs::Entity e);

// ---- G16-5：把 `entityFromJson` 劈成"纯 CPU"与"要资产"两半 ----
//
// 存在的理由见 `scene/SceneDocument.h`：`sceneFromJson` 原来把「建实体」
// 和「上传网格到 GPU」写在同一个函数体里，导致 G17 无处落脚、G19 拆不了线程、
// `--selftest` 覆盖不到读档。
//
// 两个函数**合起来**与 `entityFromJson` 等价（`entityFromJson` 就是先调前者
// 再调后者 —— 只有那一处串联，所以 Prefab 与场景加载不会走出两条路）。

// 只做「父子关系 + 文件夹标记」。**纯 CPU**：不解析任何资产。
//
// 调用前提：`created` 已经是"文件下标 → 实体"的完整表。父子必须等所有
// 实体建完再做第二遍 —— `parentGuid` 要按 guid 反查，`parent` 下标可能
// 指向文件里靠后的实体。
void entityApplyHierarchy(Scene& scene, const nlohmann::json& je, ecs::Entity e,
                          const std::vector<ecs::Entity>& created);

// 只做「组件值与资产」。**会**解析 mesh / material / 骨骼 / 贴图（= 可能上传
// GPU），是整条读档链上唯一必须留在主线程的一段。
//
// 返回 true = 这个实体"有内容"（与 `entityFromJson` 的返回值同一含义）。
bool entityApplyComponents(Scene& scene, assets::AssetManager& assets,
                           const nlohmann::json& je, ecs::Entity e);

// 一个实体 → JSON（组件字段的唯一一份实现）。
//
// `indexOf` 是"实体 id → 文件内下标"，由调用方给：
//   * 存整场景时是"全场景 id → 下标"；
//   * 存 Prefab 子树时是"子树成员 id → 子树内下标"，根节点因此自然得到
//     `parent = -1`（父子引用是下标，不是 id —— 见 docs/persistence_roadmap.md H7）。
//
// 返回 false = 这个实体"没有内容"（既没网格，也没有任何非网格组件）。
// `out` 此时无意义，调用方自己决定算不算一次 skipped。
//
// ⚠ 实体**必须**有 `TransformComponent`（场景写端只遍历带变换的实体，
//   场景级的太阳没有变换、从不进 JSON）。
// `includeEmpty`（G17-1 存档用）：默认 `false` = 场景文件语义，"没内容的空壳"
// 返回 false 让调用方算 skipped。存档必须传 `true` —— 只挂 Transform 的实体
// （玩家 / 出生点 / 纯分组节点）在场景里没什么好存，但存档漏了它 = 读档后
// 那个物体直接消失（或者被随机补发一个新 guid）。
bool entityToJson(const ecs::World& w, ecs::Entity e,
                  const std::unordered_map<ecs::EntityId, int>& indexOf,
                  nlohmann::json& out, bool includeEmpty = false);

// JSON → 一个已建好的实体（与 entityToJson 成对）。
//
// 调用前提（顺序是硬依赖）：
//   ① `e` 已经 `createObject` 出来，且 transform / visible / locked 已写过；
//   ② `created` 是"文件下标 → 实体"，`parent` 靠它解析（越界/失效就跳过）；
//   ③ 组件块顺序不能换（mesh → material → skinned → morph → animator → 表演层），
//      `modelFromSource` 命中 AssetManager 缓存靠的就是这个顺序。
//
// 返回 true = 这个实体"有内容"。
bool entityFromJson(Scene& scene, assets::AssetManager& assets,
                    const nlohmann::json& je, ecs::Entity e,
                    const std::vector<ecs::Entity>& created);

} // namespace scene
