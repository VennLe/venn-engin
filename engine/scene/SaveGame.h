#pragma once
// ============================================================
// scene/SaveGame —— 存档的**文件层**（G17-1）
//
// 这一层只管四件事：**格式 / 原子写 / 迁移 / 校验**。
// 「算 diff（哪些实体变了）」「应用到已有世界」「悬挂父修正」都是 G17-2，
// 刻意不在这里 —— 混在一起的话，"格式对不对"和"应用对不对"的失败会
// 长得一模一样（都能表现为"读档后箱子回到原位"）。
//
// 三条设计决定（都写下来是为了防止后面偷偷改）：
//
// 1. **`entities` 是数组不是对象**。对象键是 guid 时，重复 guid 会被 JSON
//    解析器**静默合并** —— 一个本该报错的文件看起来完全正常。数组还能保序，
//    diff 稳定。读端会显式断言"guid 唯一"。
//
// 2. **每条是"该实体的完整 JSON"**（与场景文件里的元素**同构**）。应用时
//    直接复用 `entityApplyComponents`，不写第二套字段解析 —— 否则以后加一个
//    组件就要改三处（写端 / 场景读端 / 存档读端），而漏改不报错。
//
// 3. **`dump()` 不缩进**。存档不是给人改的；1000 实体缩进能差 3 倍。场景文件
//    仍然缩进 2 格（人可读是优点，存档不是）。
//
// 原子写（`writeSaveGame`）的顺序**不能换**：
//     tmp → fflush → _commit(fsync) → 备份旧档 → 原子 rename
// ⚠ 先 rename 再 fsync 的话，崩在中间会留下一个"改好名但内容是空的"档 ——
//   而它看起来完全合法，下次启动直接读到一份空存档。
// ============================================================

#include "ecs/Entity.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace ecs {
class World;
}
namespace scene {
class Scene;
}

namespace scene {

// 存档格式版本。
//   v1：首版（`globals` 只有 playerGuid / playerHp / pickedUp / vars）
//   v2：`globals` 多一个 `playerStamina`
//
// ⚠⚠ v2 存在的**唯一理由**是"让迁移机制被真的走过一遍"。同理见下：
//   一张空跑的迁移表比没有迁移表更糟 —— 它给人虚假安全感，等真需要迁移时
//   才发现机制是坏的。所以 v1 落地时**同批**加 v2 字段 + 一份 v1 fixture +
//   一条 `save_migrate_v1_v2`。
constexpr int kSaveVersion = 2;

// 存档里的一个实体条目。
struct SaveEntity {
    ecs::EntityGuid guid = ecs::kInvalidGuid;
    bool destroyed = false;                    // `_destroyed: true`
    nlohmann::json raw = nlohmann::json::object();  // destroyed 时为 null
};

// 与实体无关的一局状态（"这关的进度"，不是"某个物体的字段"）。
struct SaveGlobals {
    ecs::EntityGuid playerGuid = ecs::kInvalidGuid;
    float playerHp = 100.0f;
    float playerStamina = 100.0f;   // v2 新增
    std::vector<ecs::EntityGuid> pickedUp;
    // ⚠ 用 `std::map` 不是 `unordered_map`：JSON 序列化后必须**每次同序**，
    //   否则同一份档两次写出来字节不同，`save_roundtrip_bitwise` 会无缘无故红。
    std::map<std::string, double> vars;
};

struct SaveGame {
    int version = kSaveVersion;
    std::string game = "venn";
    std::string slot;
    std::string timestamp;      // ISO8601 UTC（`2026-10-27T05:30:00Z`）
    double playTime = 0.0;      // 秒
    std::string scenePath;
    // `sceneHash` 是**结构指纹**（guid 集合排序后的 FNV），不是内容指纹：
    // 改关卡（加/删实体）→ 变；改箱子位置 → 不变。用它判断"是不是同一个关卡"。
    std::string sceneHash;      // 16 位小写 hex
    std::vector<SaveEntity> entities;
    SaveGlobals globals;
};

struct SaveIoResult {
    bool ok = false;
    std::string error;
    int entities = 0;
    int destroyed = 0;
    int migrated = 0;      // 迁移跑了几步（0 = 文件本来就是当前版本）
    bool usedBak = false;  // 主档坏了，从 `.bak` 读的
};

// ---- 哈希 ----
std::uint64_t fnv1a64(const void* data, std::size_t n);
// 场景结构指纹（只含 guid 集合，排序后）。空场景 → 16 个 0。
std::string sceneStructureHash(const Scene& scene);

// ---- 一条实体在**存档里**长什么样（G17-2）----
//
// 与场景文件的元素基本同构，**唯一**差别是父子用 `parentGuid` 而不是
// `parent` 下标。三条理由，缺一条都不该改回下标：
//   ① 存档应用**不动层级**（层级归关卡），下标写进去也没人读；
//   ② 下标是"当前活着的全部实体"里的位置 —— 增量存档只写**一部分**实体，
//      下标会指错人；而手改 / 合并过的存档更没得谈；
//   ③ 增量判定靠"这条 JSON 的哈希"，下标会让**删掉一个兄弟**把后面所有
//      实体的哈希全变掉 → 一个箱子被销毁，半个关卡都判成"改过了"。
//
// ⚠ `capture` / `SaveGameState` 的基线 / diff **必须**都走这一个函数。
//   各自拼一份 JSON 的话，哈希立刻对不上（字段顺序、`parent` 有没有，
//   任一处不同就永远"全部改过"），症状是增量存档变成全量而没有任何报错。
nlohmann::json saveEntityJson(const ecs::World& w, ecs::Entity e);
// `saveEntityJson(...).dump()` 的 FNV-1a 64。0 = 这个实体**没有内容**
// （没有 Transform，连 `includeEmpty` 也救不了）。
std::uint64_t saveEntityHash(const ecs::World& w, ecs::Entity e);

// ---- 抓档 ----
// 从场景抓一份档。`baseline == nullptr` → **全量**（关卡刚加载完存的第一份）；
// 给了基线 → 只写"新增 / 改过"的实体 + 基线里已消失的实体（`_destroyed`）。
// 增量语义见 docs/persistence_roadmap.md §5.1（基线哈希，不做脏标记）。
// `scenePath` 只记录，不解析；`playTime` 由调用方给（引擎不持有时钟）。
SaveGame captureSaveGame(const Scene& scene, const std::string& slot,
                         const std::string& scenePath, double playTime,
                         const std::unordered_map<ecs::EntityGuid, std::uint64_t>* baseline =
                             nullptr);

// ---- 序列化 ----
nlohmann::json saveGameToJson(const SaveGame& sg);
// 解析 + **迁移** + 校验。失败时 `r.error` 有明确原因（调用方直接显示给玩家）。
bool saveGameFromJson(nlohmann::json root, SaveGame& out, SaveIoResult& r);

// ---- 文件 ----
// 原子写。失败不破坏已有档（这是它存在的全部意义）。
SaveIoResult writeSaveGame(const SaveGame& sg, const std::string& path);
// 读。主档解析失败 → 退 `.bak`，并置 `usedBak`。
SaveIoResult readSaveGame(const std::string& path, SaveGame& out);

} // namespace scene
