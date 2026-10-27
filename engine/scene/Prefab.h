#pragma once
// ============================================================
// scene/Prefab —— 预制体资产（G18）
//
// 一份 `.vprefab.json` 描述**一棵子树**，可以摆进场景任意多份。改母版 →
// 所有实例（除被覆盖的字段）跟着变。
//
// ------------------------------------------------------------------
// 三个决定（每个都有"为什么不是别的"）
// ------------------------------------------------------------------
//
// ① **格式与场景文件同构**（平铺 entities + `parent` 下标），只多一个
//    `localId`。这样"哪些组件、哪些字段"不用写第二遍 —— 单实体序列化
//    走 `scene::entityToJson/entityFromJson`（与存场景**同一份**实现）。
//    否则以后每加一个组件都要记得改两处，而漏改不报错、只会默默丢字段。
//
// ② **identity 是 `localId`，不是 guid**。全仓还没有稳定 GUID（G16 未做），
//    而 Prefab 只需要"节点在**文件内**稳定"——`localId` 够了。场景实体 id
//    是运行时序号，与它无关，两者**不要混用**。
//
// ③ **覆盖是显式路径列表**，不是"值与默认值比对"。源一改，比对结果会整体
//    反转：昨天没覆盖的字段今天"自动变成覆盖"，而作者从没动过它。
//    路径语法（刻意只支持两级）：
//        `<localId>/transform`            整个组件块
//        `<localId>/transform.position`   单个字段
//    ⚠ 前缀是 **localId**，不是组件名 —— 一份 prefab 有多个节点，只写
//      `transform.position` 根本不知道是谁的。
//
// ------------------------------------------------------------------
// 明确不做（写下来是为了防止后面偷偷加）
// ------------------------------------------------------------------
//   * 嵌套 prefab（prefab 里放 prefab）：要递归展开 + 深度上限 + 环检测，
//     第一版只做单层。文件里出现 `prefab` 块会被**忽略并警告**。
//   * 删组件传播：母版删掉一个组件，实例身上的不会跟着消失（`entityFromJson`
//     只加/改，不删）。v1 接受，日志里明说。
//   * 数组下标覆盖（`material.textures[2]`）：要写半个 JSON pointer 解析器，
//     需要那个粒度时覆盖整块。
// ============================================================

#include "ecs/Entity.h"

#include <glm/vec3.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace assets {
class AssetManager;
}
namespace scene {
class Scene;
}

namespace scene {

// prefab 文件里的一个节点
struct PrefabNode {
    std::string localId;
    int parentIndex = -1;
    nlohmann::json entity;   // 与场景 `entities[]` 的元素同构
};

struct PrefabAsset {
    std::string path;     // 相对资产根：prefabs/crate.vprefab.json
    std::string name;
    std::string comment;  // "//" —— 作者的说明，往返必须保住
    int version = 1;
    std::vector<PrefabNode> nodes;   // 顺序 = 文件顺序（parent 是下标）

    bool valid = false;
    std::string error;

    int indexOf(const std::string& localId) const;
    const PrefabNode* find(const std::string& localId) const;
};

// 解析 / 读 / 写（写是临时文件 + rename，理由同存档：写到一半断电就打不开了）
bool parsePrefab(const std::string& text, PrefabAsset& out);
bool loadPrefab(const std::string& path, PrefabAsset& out);
std::string prefabToJson(const PrefabAsset& asset);
bool savePrefab(const PrefabAsset& asset, std::string& err);

// 由实体名字推出一个**不撞名**的资产相对路径：
// `Crate 2` → `prefabs/crate.vprefab.json`，撞了就 `_2`、`_3`。
// 编辑器里所有"新建 prefab"的入口都走这里，保证同一个工程不会出现两份
// 只差一个下划线的同名资产。
std::string prefabDefaultPath(const std::string& name);

// 从场景里的一棵子树导出 prefab（编辑器的 Create Prefab from Selection）。
// `root` 必须带 Transform；子树按 `HierarchyComponent::children` DFS 展平，
// 根在最前、`parent = -1`。`localId` 由名字生成并保证唯一。
bool prefabFromSubtree(Scene& scene, ecs::Entity root, PrefabAsset& out,
                       std::string* err);

// 实例编号分配（对外暴露：编辑器"Create Prefab from Selection"要把**已经
// 存在的**那棵子树就地绑成实例 1，而不是先删掉再拖一份进来）。
std::uint32_t prefabNewInstanceId(const Scene& scene);

// 把已存在的子树绑定成 `asset` 的实例：按 DFS 顺序与 `asset.nodes` 一一对应
// （与 `prefabFromSubtree` 同一个展开顺序）。节点数不一致 → false。
bool prefabBindExistingSubtree(Scene& scene, const PrefabAsset& asset,
                               ecs::Entity root, std::uint32_t* outInstance);

struct PrefabInstance {
    ecs::Entity root;
    std::vector<ecs::Entity> members;   // 与 asset.nodes 一一对应
    std::uint32_t id = 0;
};

// 把一份 prefab 摆进场景。`at` 只作用于**根**（子节点保持 prefab 内的相对
// 变换）—— 这样"把 prefab 拖到地上"落点就是它看起来的位置。
bool instantiatePrefab(Scene& scene, assets::AssetManager& assets,
                       const PrefabAsset& asset, const glm::vec3& at,
                       PrefabInstance& out, std::string* err);

// ---------------------------------------------------------------- 覆盖
//
// 路径：`<localId>/<component>[.<field>]`。`recordOverride` 由编辑器在
// "用户真的改了这个字段"的地方调用（gizmo 松手、Inspector 拖完）。
// 它的意义是**记账**，不是"检测变化" —— 见文件头决定 ③。

std::string prefabOverridePath(const std::string& localId,
                               const std::string& component,
                               const std::string& field = std::string());

bool isPrefabMember(const Scene& scene, ecs::Entity e);
ecs::Entity prefabInstanceRoot(Scene& scene, ecs::Entity member);
std::uint32_t prefabInstanceId(Scene& scene, ecs::Entity member);
std::string prefabPathOf(const Scene& scene, ecs::Entity member);

// 把"这个成员的这个字段被改过"记到**实例根**的覆盖表里（去重）。
// 返回 true = 新记了一条。非 prefab 成员返回 false（静默，这是常态）。
bool recordOverride(Scene& scene, ecs::Entity member,
                    const std::string& component,
                    const std::string& field = std::string());

int countOverrides(const Scene& scene, std::uint32_t instance);
int clearInstanceOverrides(Scene& scene, std::uint32_t instance);

struct PrefabSyncResult {
    int instances = 0;   // 命中几个实例
    int members = 0;     // 遍历了几个成员
    int applied = 0;     // 真正下发了几次
    int failed = 0;      // 成员在场景里已不存在 / 找不到对应节点
};

// 改母版 → 所有实例更新。**跳过被覆盖的字段**（这是能"只跟一部分"的全部
// 秘密）。asset 必须是刚读出来的（编辑器在改完文件后要重新 `loadPrefab`）。
PrefabSyncResult syncInstances(Scene& scene, assets::AssetManager& assets,
                               const PrefabAsset& asset);

// Apply：把实例的覆盖值写回 `asset`（**只改内存**，调用方决定何时
// `savePrefab`）。返回写了几条路径；`cleared` 返回清掉了几条覆盖。
int applyInstanceToAsset(Scene& scene, PrefabAsset& asset,
                         std::uint32_t instance, int* cleared);

// 无窗口自检（`Game.exe --selftest prefab`）。返回 0 = 全过。
int runPrefabSelfTest();

} // namespace scene
