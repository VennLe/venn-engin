#pragma once
// ============================================================
// scene/SceneDocument —— 序列化器的**三段时间轴**（G16-5）
//
// 原本 `sceneFromJson` 在同一趟里干了三件事：
//   ① 解析 JSON（纯 CPU）
//   ② 建实体 / 名字 / guid / 组件值（纯 CPU）
//   ③ 把网格 / 材质 / 贴图**上传 GPU**（要设备、要主线程）
//
// 三者交错写在一个函数体里，后果有三个，而且都还没爆但都会爆：
//   * G17 读档要在**已有世界**上改字段，硬走整份加载 = 清空重建；
//   * G19 流送要"后台解析 / 主线程上传"，交错写法连拆线程的缝都没有；
//   * `--selftest` 覆盖不到读档，因为每次都得起设备。
//
// 所以劈成三段，**签名本身就是判据**：
//   ① `parseSceneDocument`（纯 CPU，无 AssetManager、无 Scene）
//   ② `instantiateDocument`（纯 CPU，有 Scene，**没有** AssetManager）
//   ③ `resolveDocumentAssets`（唯一要 AssetManager 的一段）
//
// ⚠ ①② 都**不碰** GPU / AssetManager：把 `AssetManager*` 加进任何一段的
//   参数表，就等于把这条路又焊死了 —— 那正是这个拆分想避免的。
// ⚠ 实现放在 `SceneSerializer.cpp` 里而不是单独 `.cpp`：那 4000 行里的
//   `applyCamera` / `physicsFromJson` / `entityApplyComponentsImpl` 全在
//   匿名 namespace 里，另起一个 TU 就只能复制一份（而复制的那份会漏改）。
// ============================================================

#include "ecs/Entity.h"

#include <nlohmann/json.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace scene {
class Scene;
}
namespace assets {
class AssetManager;
}

namespace scene {

// 场景文件格式版本。
//   v1：没有 `guid` / `parentGuid`（G16 之前的所有文件）
//   v2：实体多了这两个字段（G16-2）
//
// ⚠ 读端**只拒绝比这个更大**的版本，1 照读（缺 guid 就补发）。
//   把 1 判成错会让仓库里所有既有场景与 20 多个用例 fixture 当场打不开。
constexpr int kSceneFormatVersion = 2;

// 文件里**一个实体**的纯数据形态。
//
// ⚠ 这里刻意只放"不依赖任何资产系统"的字段：名字、身份、父子、以及
//   那份**原始 JSON 块**（`raw`）。组件值本身不搬过来 —— 搬过来就要搬
//   每个组件的 `xxxFromJson`，而它们中间有十几个会解析资产（网格 / 材质 /
//   骨骼 / 贴图）。`raw` 让 ③ 段原样复用今天那份**唯一实现**
//   （`entityApplyComponents`），"以后加组件不会漏改第二处"这条才守得住。
struct DocumentEntity {
    std::string     name = "Entity";
    ecs::EntityGuid guid = ecs::kInvalidGuid;
    // 跨文件 / 跨块引用（Prefab 子树根、G19 分期加载的边界实体）用 guid；
    // 同一份文件内部用下标。两条路互斥：写了 parentGuid 就以它为准。
    ecs::EntityGuid parentGuid = ecs::kInvalidGuid;
    int             parentIndex = -1;
    // 原始实体块。③ 段与将来的"存档 overlay"都靠它。
    nlohmann::json  raw = nlohmann::json::object();
};

// 解析结果：一个完整场景文档的纯数据形态。
struct SceneDocument {
    int  formatVersion = 0;
    bool hasEntities = false;   // 文件里到底有没有 `entities` 数组（空场景 ≠ 没有）
    // 相机 / 相机数组 / 方向光 / 物理 / 脚本黑板 / 后处理体积。原样留着，
    // ② 段按 key 应用 —— 与今天 `root.contains("camera")` 的语义一字不差。
    nlohmann::json globals = nlohmann::json::object();
    // 顺序 = 文件顺序。`parentIndex` 依赖它，**不能重排**。
    std::vector<DocumentEntity> entities;
};

struct DocumentStats {
    int entities = 0;      // created.size()
    int guidAssigned = 0;  // 文件里没有 guid → 保留 create() 随机补发的那个
    int guidDup = 0;       // 文件里的 guid 撞了 → 保留随机值，记一条 SCENE-GUID-DUP
    int skipped = 0;       // ③ 段判定"没有内容"的数量
};

// ① 解析 + 校验。**纯 CPU**：不 include rhi / vulkan，可后台线程。
//
// 校验内容：root 是 object、`version` 不比我们新、`entities` 是数组。
// ⚠ 版本校验**必须在这里**（早于任何 `scene.clear()`）：清完再拒绝，
//   调用方会拿到 `ok=false` 却已经丢掉了整个世界。
// 失败返回 false 并填 `err`（调用方原样塞进 `SceneIoResult::error`）。
bool parseSceneDocument(const nlohmann::json& root, SceneDocument& out, std::string& err);

// ② 建世界骨架。**纯 CPU**，参数表里没有 AssetManager。
//
// 做四件事：`scene.clear()` → 相机/光照/物理/黑板/后处理 → 建实体
// （名字 / guid / 外壳 transform-visible-locked）→ 父子关系 + 文件夹标记。
//
// `created` 与 `doc.entities` **一一对应**（解析失败的槽位是 invalid Entity，
// 保留占位 —— 下标是父子关系的坐标系，不能压缩）。
// `guidMap` 让调用方/G17/G19 能按 guid 反查，省一次 `World::find`。
//
// ⚠ 父子必须等**所有**实体建完再做第二遍：`parentGuid` 要按 guid 反查，
//   而 `parent` 下标可能指向文件里靠后的实体。
void instantiateDocument(Scene& scene, const SceneDocument& doc,
                         std::vector<ecs::Entity>& created,
                         std::unordered_map<ecs::EntityGuid, ecs::Entity>& guidMap,
                         DocumentStats& stats);

// ③ 解析资产与其余组件。**唯一要 AssetManager 的一段**（可能要上传 GPU）。
//
// 只碰 `created` 里有效的实体；`skipped` 累加进 `stats`。
void resolveDocumentAssets(Scene& scene, assets::AssetManager& assets,
                           const SceneDocument& doc,
                           const std::vector<ecs::Entity>& created,
                           DocumentStats& stats);

} // namespace scene
