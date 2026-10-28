#pragma once
// ============================================================
// scene/SaveGameState —— 存档的**应用层**（G17-2）
//
// 与 `SaveGame.{h,cpp}`（文件层）的分工，一行说清：
//
//   文件层：格式 / 原子写 / 迁移 / 校验 —— "这个文件本身是不是好的"
//   应用层：基线 / diff / 应用到世界 / 悬挂父修正 —— "把这份档盖到当前世界上"
//
// ⚠ 两层分开**不是为了好看**：混在一起时，"格式对不对"和"应用对不对"的失败
//   长得一模一样（都表现为"读档后箱子回到原位"），排查时无从下手。
//
// ## 增量语义（为什么不做脏标记）
//
// 否掉"每个组件写入点打脏标记"：那要给 40 个组件的所有写入点包一层，漏一个
// 就永远存不上 —— 而且是**静默丢进度**，最坏的一种 bug。
//
// 采用**基线哈希**：关卡加载完成时对每个实体算一次 `saveEntityJson` 的
// FNV-1a 64 存成基线（1000 实体 ≈ 8 KB）。存档时重算，只写不同的。
//
// ## 基线里那三个状态（很容易搞混，所以列全）
//
// | 基线里有 | 世界里活着 | 含义 | diff 行为 |
// |---|---|---|---|
// | 否 | 是 | 运行时新生成（或基线之后才加载） | 写**整条** |
// | 是(h) | 是 | 关卡自带的实体 | `h` 相同 → 跳过；不同 → 写整条 |
// | 是(h) | 否 | 被销毁了 | 写 `_destroyed`（**每次**都写，见下） |
// | 否 | 否 | 关卡里没有 / 运行时生成后又被销毁 | 不写（关卡本来就没有它） |
//
// ⚠⚠ "被销毁"是**粘性**的：销毁后**不能**把 guid 从基线里摘掉。
//   摘掉的话，下一次存档就不会再写 `_destroyed` —— 而它会被读进**还带着那个
//   实体**的关卡里，于是"打死的怪读档后复活"。所以基线里保留 guid，值记成
//   `kDeadHash`。
// ============================================================

#include "ecs/Entity.h"
#include "scene/SaveGame.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace assets {
class AssetManager;
}
namespace ecs {
class World;
}
namespace scene {
class Scene;
}

namespace scene {

// 「这份档不是同一个关卡」的判据：存档里有、世界里没有的实体占比超过这个值。
// ⚠ 写成常量、别散在 `if` 里 —— 它是产品决定（"换了关卡版本还能不能继续玩"），
//   不是一个技术细节，迟早要调。
constexpr double kMaxMissingRatio = 0.5;

struct SaveApplyStats {
    int total = 0;      // 存档里的条目数（含 `_destroyed`）
    int applied = 0;    // 覆盖了几个实体
    int destroyed = 0;  // 销毁了几个
    int missing = 0;    // 存档里有、世界里没有（关卡漂移）
    int extra = 0;      // 世界里有、存档里没有（关卡新增 → 保留关卡默认值）
    int orphans = 0;    // 悬挂父被修正
    int staleChildren = 0;  // 父的 children 列表里指向已死实体
    bool driftRejected = false;
};

struct SaveApplyResult {
    bool ok = false;
    std::string error;
    SaveApplyStats stats;
};

// 把存档盖到场景上。
//
// `assets` **必须**由调用方给：存档里的 `mesh` / `material` / `skinned` 块
// 走的是与场景文件**同一条** `entityApplyComponents`，所以读档会（命中缓存的）
// 重新解析资产。⚠ 自检里传一个空 `TextureContext` 的 AssetManager 就够用 ——
// 夹具的实体没有资产块，`resolveMesh/Material` 拿到 `null` 会直接返回 nullptr。
//
// `state` 给了 → 应用完顺手把基线刷成"刚读完档的样子"。不给 → 调用方要自己
// `captureBaseline()`，否则**下一次存档会把刚读进来的东西再写一遍**（不致命，
// 但增量就白做了）。
//
// 顺序（不能换）：
//   ① 统计漂移（**先**判，拒读时一个实体都不许动）
//   ② `_destroyed` 先销毁
//   ③ 再逐条覆盖（反过来的话，"既销毁又有字段"的条目白写一遍）
//   ④ 悬挂父 / 死子清扫（无论如何都要跑：手改过的存档也能造出悬挂）
//   ⑤ globals（只**合并** `vars`，不清空 —— 脚本 `let` 声明的关卡常量不在存档里）
SaveApplyResult applySaveGame(Scene& scene, const SaveGame& sg, assets::AssetManager& assets,
                              class SaveGameState* state = nullptr);

// 悬挂父修正：`parent` 指向不存活实体 → 清成 `kInvalidEntity`；
// 同时把 `children` 里指向已死实体的 id 摘掉（否则层级树里会留一行点不开的幽灵）。
//
// ⚠ 这一步**无论如何都要跑**，不能只在"这次读档销毁过父"时才跑：手改过 /
//   合并过 / 从别的关卡搬过来的存档同样能造出悬挂父，而悬挂父的症状是
//   `worldMatrix()` 沿父链往上走时读到空组件 —— 表现为"物体突然跑到原点"，
//   没有任何日志会指向存档。
struct OrphanFix {
    int parents = 0;    // 被判为悬挂并清空的父引用
    int children = 0;   // 从 `children` 列表里摘掉的死 id
};
OrphanFix fixOrphanParents(Scene& scene);

// 基线状态。一个会话一份，**不要**塞进 `Scene` 或 `World`：
// 它是"存档子系统"的状态，跟着"关卡加载"走，不是世界的一部分。
class SaveGameState {
public:
    // 基线里 `_destroyed` 的哨兵值。真实哈希为 0 的可能性约 2^-64，
    // 且 `saveEntityHash` 已经把 0 改写成 1，所以这里不会撞。
    static constexpr std::uint64_t kDeadHash = 0;

    // 关卡**加载完成之后**调一次。它会把当时活着的每个实体都记下来。
    void captureBaseline(const Scene& scene);
    void clear() { m_baseline.clear(); }

    std::size_t size() const { return m_baseline.size(); }
    bool empty() const { return m_baseline.empty(); }
    // 基线里有没有这个 guid。⚠ `hashOf` 返回 `kDeadHash` 时要用它区分
    // "记着且已死" 与 "根本没记"。
    bool contains(ecs::EntityGuid g) const { return m_baseline.count(g) != 0; }
    std::uint64_t hashOf(ecs::EntityGuid g) const {
        const auto it = m_baseline.find(g);
        return it == m_baseline.end() ? 0 : it->second;
    }
    const std::unordered_map<ecs::EntityGuid, std::uint64_t>& raw() const { return m_baseline; }

    // 增量抓档。基线为空 → 等价于全量（第一次存档就该是这个行为）。
    SaveGame captureDiff(const Scene& scene, const std::string& slot, const std::string& scenePath,
                         double playTime) const;

    // 应用存档之后刷新基线：**只重算这份档里提到的 guid**，其余不动。
    // 不刷的话"读档 → 立刻再存"会把刚应用的条目原样再写一遍（增量白做）；
    // 只刷提到的那些，是因为没提到的本来就是"与基线一致"的。
    void noteApplied(const Scene& scene, const SaveGame& sg);

private:
    std::unordered_map<ecs::EntityGuid, std::uint64_t> m_baseline;
};

}  // namespace scene
