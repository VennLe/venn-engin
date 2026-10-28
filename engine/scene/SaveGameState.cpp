// ============================================================
// scene/SaveGameState.cpp —— 存档应用层（G17-2）
//
// 实现顺序与"读档会怎么坏"对齐：
//   ① 基线（关卡刚加载完的样子）
//   ② diff（只写变了的）
//   ③ 漂移统计（**先**判，拒读时一个实体都不许动）
//   ④ 应用（先销毁再覆盖）
//   ⑤ 悬挂父清扫（无条件）
// ============================================================

#include "scene/SaveGameState.h"

#include "core/Logger.h"
#include "ecs/Components.h"
#include "ecs/World.h"
#include "scene/Scene.h"
#include "scene/SceneEntityIO.h"

#include <algorithm>
#include <unordered_set>
#include <vector>

namespace scene {

namespace {
using json = nlohmann::json;
}  // namespace

// ---------------- ① 基线 ----------------

void SaveGameState::captureBaseline(const Scene& scene) {
    m_baseline.clear();
    const ecs::World& w = scene.world();

    w.each<ecs::TransformComponent>([&](ecs::Entity e, const ecs::TransformComponent&) {
        const ecs::EntityGuid g = w.guid(e);
        if (g == ecs::kInvalidGuid) return;      // 没身份 → 存档里定位不到，别记
        const std::uint64_t h = saveEntityHash(w, e);
        if (h == 0) return;                      // 没有内容 → 存档里也不会有它
        m_baseline[g] = h;
    });

    VK_LOG_INFO("SAVE-BASELINE entities=%d sceneHash=%s",
                static_cast<int>(m_baseline.size()), sceneStructureHash(scene).c_str());
}

// ---------------- ② diff ----------------

SaveGame SaveGameState::captureDiff(const Scene& scene, const std::string& slot,
                                    const std::string& scenePath, double playTime) const {
    // ⚠ 全量与增量**只允许有一个实现**（`captureSaveGame` 多一个 baseline 参数）。
    //   在这里再写一遍"遍历 + 序列化 + 过滤"必然漂移：字段顺序、`includeEmpty`
    //   的取值、`parent` 有没有删，任一处不同就让哈希永远对不上，于是增量退化成
    //   全量 —— 而**没有任何报错**，只有存档文件忽然变大。
    return captureSaveGame(scene, slot, scenePath, playTime, &m_baseline);
}

// ---------------- 基线刷新 ----------------

void SaveGameState::noteApplied(const Scene& scene, const SaveGame& sg) {
    const ecs::World& w = scene.world();
    for (const SaveEntity& se : sg.entities) {
        if (se.destroyed) {
            // 粘性：留在基线里、记成"已死"。摘掉的话下一次存档不会再写
            // `_destroyed`，于是"打死的怪读档后复活"（见 SaveGameState.h 的表）。
            m_baseline[se.guid] = kDeadHash;
            continue;
        }
        const ecs::Entity e = w.find(se.guid);
        // 外面那个世界里没有它（漂移 / 已经被销毁）→ **不记**。
        // ⚠ 别记成 kDeadHash：那会对一个"关卡里从来没有过"的 guid 反复写
        //   `_destroyed`，文件越存越大而且全是噪音。
        if (!e.valid()) continue;
        const std::uint64_t h = saveEntityHash(w, e);
        m_baseline[se.guid] = (h == 0) ? 1 : h;
    }
}

// ---------------- ⑤ 悬挂父（独立函数：不读档也该能跑）----------------

OrphanFix fixOrphanParents(Scene& scene) {
    ecs::World& w = scene.world();
    OrphanFix fix;

    // ⚠ 先收集再改，不在 `each` 回调里动世界：`setParent` 在父无效时不 add
    //   组件（安全），但这条依赖太隐蔽，收集一遍只多一次遍历。
    std::vector<ecs::Entity> badParent;
    std::vector<ecs::Entity> hasStaleChild;
    w.each<ecs::HierarchyComponent>([&](ecs::Entity e, ecs::HierarchyComponent& h) {
        if (h.parent != ecs::kInvalidEntity && !w.alive(w.handle(h.parent)))
            badParent.push_back(e);
        for (const ecs::EntityId c : h.children) {
            if (!w.alive(w.handle(c))) {
                hasStaleChild.push_back(e);
                break;
            }
        }
    });

    for (const ecs::Entity e : badParent) {
        VK_LOG_WARN("SAVE-ORPHAN guid=%s entity=%u action=detach", ecs::guidToHex(w.guid(e)).c_str(),
                    static_cast<unsigned>(e.id));
        // 走 `scene.setParent` 而不是直接写 `h->parent = kInvalidEntity`：
        // 它会顺手把本实体从**旧父的 children 列表**里摘掉。漏了这一步，
        // 旧父（如果还活着）的 children 里会一直留着一个已经不认它的孩子。
        scene.setParent(e, ecs::Entity{});
        ++fix.parents;
    }

    for (const ecs::Entity e : hasStaleChild) {
        auto* h = w.get<ecs::HierarchyComponent>(e);
        if (!h) continue;
        const size_t before = h->children.size();
        h->children.erase(std::remove_if(h->children.begin(), h->children.end(),
                                         [&](ecs::EntityId c) { return !w.alive(w.handle(c)); }),
                          h->children.end());
        const int removed = static_cast<int>(before - h->children.size());
        if (removed <= 0) continue;
        fix.children += removed;
        VK_LOG_WARN("SAVE-STALE-CHILD guid=%s entity=%u removed=%d",
                    ecs::guidToHex(w.guid(e)).c_str(), static_cast<unsigned>(e.id), removed);
    }
    return fix;
}

// ---------------- ③④ 应用 ----------------

SaveApplyResult applySaveGame(Scene& scene, const SaveGame& sg, assets::AssetManager& assets,
                              SaveGameState* state) {
    SaveApplyResult res;
    ecs::World& w = scene.world();
    res.stats.total = static_cast<int>(sg.entities.size());

    // ---- ① 漂移统计 ----
    // ⚠ 必须**先**统计再动手：拒读时如果一个实体已经被覆盖过，玩家看到的是
    //   "一半旧一半新"的世界 + 一句"存档不匹配"，比直接不读还糟。
    std::unordered_set<ecs::EntityGuid> inSave;
    inSave.reserve(sg.entities.size());
    for (const SaveEntity& se : sg.entities) inSave.insert(se.guid);

    w.each<ecs::TransformComponent>([&](ecs::Entity e, const ecs::TransformComponent&) {
        const ecs::EntityGuid g = w.guid(e);
        if (g != ecs::kInvalidGuid && inSave.count(g) == 0) ++res.stats.extra;
    });
    for (const SaveEntity& se : sg.entities)
        if (!w.find(se.guid).valid()) ++res.stats.missing;

    const int total = res.stats.total;
    const int missing = res.stats.missing;
    if (total > 0 && static_cast<double>(missing) / static_cast<double>(total) > kMaxMissingRatio) {
        res.stats.driftRejected = true;
        res.error = "save does not match this level (" + std::to_string(missing) + "/" +
                    std::to_string(total) + " entities missing)";
        VK_LOG_ERROR("SAVE-DRIFT-REJECT want=%s got=%s missing=%d extra=%d total=%d",
                     sg.sceneHash.c_str(), sceneStructureHash(scene).c_str(), missing,
                     res.stats.extra, total);
        return res;
    }
    if (missing > 0 || res.stats.extra > 0) {
        // WARN 不是 ERROR：换了关卡版本还能继续玩，比"存档损坏"体验好得多。
        VK_LOG_WARN("SAVE-SCENE-DRIFT want=%s got=%s missing=%d extra=%d path=%s",
                    sg.sceneHash.c_str(), sceneStructureHash(scene).c_str(), missing,
                    res.stats.extra, sg.scenePath.c_str());
    }

    // ---- ② `_destroyed` 先销毁 ----
    // 反过来（先覆盖再销毁）的话，"既带字段又标销毁"的条目会先把值写进去、
    // 再被删掉 —— 白干一次；如果那个值指向另一个实体，还会短暂出现悬空。
    for (const SaveEntity& se : sg.entities) {
        if (!se.destroyed) continue;
        const ecs::Entity e = w.find(se.guid);
        if (!e.valid()) continue;   // 已经没有 → 幂等，不计数
        w.destroy(e);
        ++res.stats.destroyed;
    }

    // ---- ③ 逐条覆盖 ----
    // ⚠ 复用 `entityApplyShell` + `entityApplyComponents`，**不写第二套字段解析**：
    //   存档条目与场景文件元素同构（唯一差别是父子用 guid），所以解析必须同源。
    //   另写一套的后果是"以后加一个组件要改三处，而漏改不报错"。
    // ⚠ `entities` 里可能带 `parentGuid`，但**这里不调** `entityApplyHierarchy`：
    //   层级是关卡的，读档不改层级（改的话，存档里一条孤立的子实体会把整个
    //   父子结构拆散，而玩家只是读了个档）。
    for (const SaveEntity& se : sg.entities) {
        if (se.destroyed) continue;
        const ecs::Entity e = w.find(se.guid);
        if (!e.valid()) continue;                       // missing，上面已经计过
        if (!se.raw.is_object()) continue;
        const json& je = se.raw;
        entityApplyShell(scene, je, e);
        entityApplyComponents(scene, assets, je, e);
        ++res.stats.applied;
    }

    // ---- ④ 悬挂父清扫（无条件）----
    const OrphanFix fix = fixOrphanParents(scene);
    res.stats.orphans = fix.parents;
    res.stats.staleChildren = fix.children;

    // ---- ⑤ globals ----
    // 只**合并** `vars`，不清空：脚本 `let` 声明的关卡常量不在存档里，
    // 清空会让它们消失（而且下一次脚本 tick 才会重新声明 → 中间那一帧读到 0）。
    for (const auto& kv : sg.globals.vars) scene.scriptVars()[kv.first] = kv.second;

    if (state) state->noteApplied(scene, sg);

    res.ok = true;
    VK_LOG_INFO("SAVE-APPLY applied=%d destroyed=%d missing=%d extra=%d orphans=%d stale=%d "
                "total=%d slot=%s",
                res.stats.applied, res.stats.destroyed, res.stats.missing, res.stats.extra,
                res.stats.orphans, res.stats.staleChildren, res.stats.total, sg.slot.c_str());
    return res;
}

}  // namespace scene
