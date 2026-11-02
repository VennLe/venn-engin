// engine/scene/LevelStreamer.cpp —— 关卡流送运行时（G19）
//
// 三条最容易写错的地方，都在下面用 ⚠⚠ 标出来了：
//   ① 加载/卸载必须用**两个**盒（滞回）；
//   ② 常驻关卡要**主动加载**，不只是"不卸载"；
//   ③ 卸载只销毁**本次加载的实体**，绝不按包围盒反查。

#include "scene/LevelStreamer.h"

#include "assets/AssetManager.h"
#include "assets/AssetPath.h"
#include "core/Logger.h"
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"

#include <algorithm>

namespace scene {

// ---------------------------------------------------------------- 清单

void LevelStreamer::setManifest(LevelManifest m) {
    m_manifest = std::move(m);
    m_slots.clear();
    m_slots.reserve(m_manifest.levels.size());
    for (const LevelEntry& e : m_manifest.levels) {
        Slot s;
        s.entry = e;
        m_slots.push_back(std::move(s));
    }
    recomputeStats();
    VK_LOG_INFO("LEVELS manifest name=%s levels=%zu",
                m_manifest.name.c_str(), m_slots.size());
}

void LevelStreamer::rebuildSlots(Scene& sc, bool keepLoaded) {
    // 旧 slot 里"清单里已经没有"的关卡：实体必须销毁，否则它们变成
    // 谁也删不掉的孤儿（面板上也不再出现，想手动卸都点不到）。
    std::vector<Slot> next;
    next.reserve(m_manifest.levels.size());
    for (const LevelEntry& e : m_manifest.levels) {
        Slot s;
        s.entry = e;
        if (const Slot* old = findSlot(e.name)) {
            if (keepLoaded && old->loaded && old->entry.path == e.path) {
                // 同一条路径 → 里面装的东西没变，直接接管
                s.loaded = true;
                s.entities = old->entities;
                s.loads = old->loads;
                s.unloads = old->unloads;
            } else if (old->loaded) {
                // 路径变了 / 不保留 → 先卸掉旧的
                for (const ecs::Entity& ent : old->entities)
                    if (sc.world().alive(ent)) sc.world().destroy(ent);
                VK_LOG_INFO("LEVELS drop name=%s entities=%zu path=%s",
                            old->entry.name.c_str(), old->entities.size(),
                            old->entry.path.c_str());
            }
        }
        next.push_back(std::move(s));
    }
    m_slots = std::move(next);
    recomputeStats();
}

// ---------------------------------------------------------------- 手动

LevelStreamer::Slot* LevelStreamer::findSlot(const std::string& name) {
    for (Slot& s : m_slots)
        if (s.entry.name == name) return &s;
    return nullptr;
}

const LevelStreamer::Slot* LevelStreamer::findSlot(const std::string& name) const {
    for (const Slot& s : m_slots)
        if (s.entry.name == name) return &s;
    return nullptr;
}

bool LevelStreamer::hasLevel(const std::string& name) const {
    return findSlot(name) != nullptr;
}

bool LevelStreamer::isLoaded(const std::string& name) const {
    const Slot* s = findSlot(name);
    return s && s->loaded;
}

bool LevelStreamer::loadLevel(Scene& sc, assets::AssetManager& assets,
                              const std::string& name, std::string* err) {
    Slot* s = findSlot(name);
    if (!s) {
        if (err) *err = "no level named '" + name + "'";
        return false;
    }
    // 已经加载 → 什么都不做，**不算一次加载**。重复加载会把同一批实体
    // 再建一遍（两个一模一样的屋子叠在一起），而且没人会觉得不对劲。
    if (s->loaded) return true;

    const std::string full = assets::resolveAssetPath(s->entry.path);
    std::vector<ecs::Entity> created;
    const SceneIoResult r = loadSceneAdditive(sc, assets, full, &created);
    if (!r.ok) {
        s->error = r.error;
        ++m_stats.failed;
        VK_LOG_WARN("LEVELS load-failed name=%s path=%s err=%s",
                    name.c_str(), s->entry.path.c_str(), r.error.c_str());
        recomputeStats();
        return false;
    }

    s->loaded = true;
    s->error.clear();
    s->entities = std::move(created);
    ++s->loads;
    VK_LOG_INFO("LEVELS load name=%s path=%s entities=%zu total=%d",
                name.c_str(), s->entry.path.c_str(), s->entities.size(),
                m_stats.loaded + 1);
    recomputeStats();
    return true;
}

bool LevelStreamer::unloadLevel(Scene& sc, const std::string& name,
                                std::string* err) {
    Slot* s = findSlot(name);
    if (!s) {
        if (err) *err = "no level named '" + name + "'";
        return false;
    }
    if (!s->loaded) return true;

    // ⚠⚠ 只销毁**本次加载建出来的**实体。按名字前缀 / 按包围盒反查都会
    //   误删玩家的东西（玩家可以把一个箱子拖进那个盒子里 —— 它不属于本关）。
    //   `alive()` 先判：编辑器里删过的实体，句柄已经过期，
    //   直接 destroy 一个过期句柄在 swap-and-pop 的池子里是未定义行为。
    std::size_t n = 0;
    for (const ecs::Entity& e : s->entities) {
        if (!sc.world().alive(e)) continue;
        sc.world().destroy(e);
        ++n;
    }
    s->entities.clear();
    s->loaded = false;
    ++s->unloads;
    VK_LOG_INFO("LEVELS unload name=%s destroyed=%zu", name.c_str(), n);
    recomputeStats();
    return true;
}

int LevelStreamer::unloadAll(Scene& sc) {
    int n = 0;
    for (Slot& s : m_slots) {
        if (!s.loaded) continue;
        unloadLevel(sc, s.entry.name, nullptr);
        ++n;
    }
    return n;
}

// ---------------------------------------------------------------- 每帧

LevelStreamer::Stats LevelStreamer::update(Scene& sc, assets::AssetManager& assets,
                                           const glm::vec3& viewer) {
    m_viewer = viewer;
    m_stats.loadRequests = 0;
    m_stats.unloadRequests = 0;

    // ⚠ 两趟，不是一趟：先算"该加载"（可能改变 loaded 状态），再算
    //   "该卸载"。合成一趟时，一张刚被加载的关卡会在同一趟里又被判定
    //   "要不要卸" —— 而它的 `wantsUnload` 依赖的是**加载后的**状态，
    //   顺序一乱就变成"加载完立刻卸"（每帧反复解析场景文件）。
    for (Slot& s : m_slots) {
        if (!s.entry.enabled) continue;

        // ---- ① 常驻：主动加载（不只是"不卸载"）----
        // 第一次进 Play 没人会手动 load，靠这一段把它带起来。
        if (s.entry.alwaysLoaded) {
            if (!s.loaded) {
                if (loadLevel(sc, assets, s.entry.name, nullptr))
                    ++m_stats.loadRequests;
            }
            continue;
        }

        // ---- ② 按需：viewer 进盒就加载 ----
        if (!s.loaded && levelWantsLoad(s.entry, viewer)) {
            if (loadLevel(sc, assets, s.entry.name, nullptr))
                ++m_stats.loadRequests;
        }
    }

    // ---- ③ 卸载（用放大后的盒）----
    for (Slot& s : m_slots) {
        if (!s.loaded) continue;
        if (!s.entry.enabled) continue;       // 关掉的关卡保持现状
        if (s.entry.alwaysLoaded) continue;   // 常驻永不自动卸
        if (levelWantsUnload(s.entry, viewer, unloadScale)) {
            unloadLevel(sc, s.entry.name, nullptr);
            ++m_stats.unloadRequests;
        }
    }

    recomputeStats();
    return m_stats;
}

// ---------------------------------------------------------------- 查询

void LevelStreamer::recomputeStats() {
    Stats st;
    st.failed = m_stats.failed;   // 失败计数不清零（它是累计诊断）
    for (const Slot& s : m_slots) {
        if (s.entry.enabled) ++st.levels;
        if (s.entry.alwaysLoaded) ++st.alwaysLoaded;
        if (s.loaded) {
            ++st.loaded;
            st.entities += static_cast<int>(s.entities.size());
        }
    }
    st.loadRequests = m_stats.loadRequests;
    st.unloadRequests = m_stats.unloadRequests;
    m_stats = st;
}

std::vector<std::string> LevelStreamer::loadedNames() const {
    std::vector<std::string> out;
    for (const Slot& s : m_slots)
        if (s.loaded) out.push_back(s.entry.name);
    return out;
}

std::size_t LevelStreamer::loadedEntityCount() const {
    std::size_t n = 0;
    for (const Slot& s : m_slots)
        if (s.loaded) n += s.entities.size();
    return n;
}

} // namespace scene
