#pragma once
// ============================================================
// scene/LevelStreamer —— 关卡流送的运行时（G19 的第二半）
//
// 职责只有一条：**按 viewer 位置维护"哪些关卡该在内存里"**。
// 它的输出是 `Scene` 里的实体增减，没有别的副作用。
//
// ---- 不变量（改这个文件前先读）----
//
// 1. **常驻关卡永远加载着**。`update()` 会主动把还没加载的常驻关卡加载上
//    （不只是"不卸载"）—— 第一次进 Play 时没人会手动调 `loadLevel`。
// 2. **加载盒 ≠ 卸载盒**。卸载盒是 `halfExtent * unloadScale`（默认 1.25）。
//    同一个盒的话，玩家在边界上左右晃就是"加载→卸载→加载"，每次都重新
//    解析场景文件 —— 表现成"走到某条线上就卡"，而日志里是一串
//    `SCENE-STREAM`，看不出是抖动。
// 3. **没有触发盒的关卡不参与流送**（`levelWantsLoad` 恒 false）。
//    它们要么常驻，要么手动。
// 4. **卸载 = 销毁本次加载出来的那些实体**（`Slot::entities`）。
//    不按"名字前缀"或"包围盒里的所有实体"反推 —— 那会误删玩家的东西。
//
// ---- 为什么 `update` 要接收 `Scene&` 而不是持有它 ----
//
// 编辑器有两份场景（编辑场景 / 运行态副本），流送在 Play 时作用于**运行态
// 副本**。持有 `Scene&` 的成员会在 Stop 之后指向一个被丢弃的世界。
// ============================================================

#include "ecs/Entity.h"
#include "scene/LevelManifest.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace assets {
class AssetManager;
}

namespace scene {

class Scene;

class LevelStreamer {
public:
    // 一张关卡在这一局里的运行状态。
    struct Slot {
        LevelEntry entry;
        bool loaded = false;
        std::string error;                  // 上一次失败的原因（成功后清空）
        std::vector<ecs::Entity> entities;   // 本次加载建出来的实体（卸载的依据）
        std::uint64_t loads = 0;
        std::uint64_t unloads = 0;
    };

    struct Stats {
        int levels = 0;         // 清单里 enabled 的关卡数
        int alwaysLoaded = 0;   // 其中常驻的数量
        int loaded = 0;         // 当前已加载
        int wanted = 0;         // 当前满足"该加载"条件的（含常驻）
        int loadRequests = 0;   // 本次 update 发起的加载
        int unloadRequests = 0; // 本次 update 发起的卸载
        int failed = 0;         // 加载失败的累计（不清零）
        int entities = 0;       // 所有已加载关卡的实体总数
    };

    // ---- 清单 ----
    void setManifest(LevelManifest m);
    const LevelManifest& manifest() const { return m_manifest; }
    LevelManifest& manifest() { return m_manifest; }
    // 重新建立 slot 表。已加载的关卡**保持加载**（清单改了不该把世界清掉），
    // 但消失的那几张会被卸载 —— 否则它们变成"谁也删不掉的实体"。
    // 传 `keepLoaded=false` 时先全部卸载（换世界用）。
    void rebuildSlots(Scene& sc, bool keepLoaded);

    // ---- 手动 ----
    bool loadLevel(Scene& sc, assets::AssetManager& assets,
                   const std::string& name, std::string* err = nullptr);
    bool unloadLevel(Scene& sc, const std::string& name,
                     std::string* err = nullptr);
    int unloadAll(Scene& sc);

    // ---- 每帧 ----
    // viewer 一般是玩家脚底位置（与玩家位置语义一致：脚，不是头）。
    //
    // 这个函数是**幂等**的：同样位置连调两次，第二次不会有任何请求。
    Stats update(Scene& sc, assets::AssetManager& assets,
                 const glm::vec3& viewer);

    // ---- 查询（面板 / 命令 / 判据）----
    const std::vector<Slot>& slots() const { return m_slots; }
    const Stats& stats() const { return m_stats; }
    bool isLoaded(const std::string& name) const;
    bool hasLevel(const std::string& name) const;
    std::vector<std::string> loadedNames() const;
    std::size_t loadedEntityCount() const;
    // 上一次 update 用的 viewer（面板显示"为什么它是这个状态"）
    const glm::vec3& lastViewer() const { return m_viewer; }

    // 卸载滞回的倍数。**必须 > 1**（< 1 会让卸载盒比加载盒小 = 永远卸不掉）。
    float unloadScale = 1.25f;

private:
    Slot* findSlot(const std::string& name);
    const Slot* findSlot(const std::string& name) const;
    void recomputeStats();

    LevelManifest m_manifest;
    std::vector<Slot> m_slots;
    Stats m_stats;
    glm::vec3 m_viewer = glm::vec3(0.0f);
};

} // namespace scene
