#pragma once
// ============================================================
// fx/EffectService —— 特效调度（事件型特效的池 + 抢占 + 回收）
//
// ------------------------------------------------------------
// 它解决的具体问题
// ------------------------------------------------------------
//   一次命中要出 2~3 层粒子 + 音效 + 震屏 + 闪白，一场战斗同一帧可能
//   命中 6 次。如果每次都 `spawn` 一个实体再 `destroy`：
//     · 命中一多就是几百次实体增删（World 是 swap-and-pop，池顺序乱跳）
//     · **踩 `render_resource_lifetime` 铁律** —— 一条 CB 在 recording
//       状态时引用过的对象不能销毁，否则 `invalid state` 刷屏 + submit -4
//     · 本机是 Iris Xe，实例化开销一上来直接掉帧
//   所以：**宿主实体永不销毁，只切换参数**。
//
// ------------------------------------------------------------
// ⚠⚠ 关键约束：宿主必须**永远 enabled**
// ------------------------------------------------------------
//   `Renderer::collectParticleEmitters` 是按下标顺序把发射器塞进表的
//   （`if (scratch.size() >= maxEmitters) return;` 然后依次 push）。
//   也就是说：**发射器下标 = 本帧被收集的顺序**，不是实体身份。
//
//   如果空闲宿主用 `enabled=false` 表示，那么"第 2 个空闲、第 3 个在用"
//   时，第 3 个的下标会从 2 前移到 1 → 它申请的粒子槽位跟着前移 →
//   上一批粒子还在飞、参数却换成了别人的。表现是"火花突然变成烟雾"。
//
//   所以空闲的表示是 **`rate = 0` 且所有 burst 的 count = 0**：
//     · 不喷新粒子
//     · 已有的粒子继续飞完（自然消亡，不会定格在空中）
//     · 收集顺序永远稳定，下标永远不跳
//   这也顺带解决了"提前回收会让粒子定格"——它们本来就在继续模拟。
//
// ------------------------------------------------------------
// 抢占规则（必须确定）
// ------------------------------------------------------------
//   槽不够 → **新的被丢弃**，`dropped++`。不按优先级挤掉正在播的。
//   理由：挤掉正在播的特效会看到"火花突然消失"，比"这次没出火花"更明显；
//   而"丢新的"规则简单，同一帧的多次请求得到的结果可复现（截图判据不 flaky）。
//   `priority` 字段保留给后续版本（那时要做的是"同帧超额时按优先级排"）。
// ============================================================

#include "ecs/Entity.h"
#include "fx/EffectAsset.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace scene {
class Scene;
}
namespace assets {
class AssetManager;
}

namespace fx {

// 一次播放请求
struct EffectRequest {
    std::string asset;                  // 相对资产根的路径（"fx/slash_a.veffect"）
    glm::vec3 position{0.0f};           // 世界坐标（宿主的位置）
    glm::vec3 forward{0.0f, 0.0f, 1.0f};// 朝向（粒子的锥/盒按它摆）
    float scale = 1.0f;                 // 整体缩放（乘在宿主 transform 上）
    float speed = 1.0f;                 // 播放速率（暴击慢放用 < 1）
    int priority = 0;                   // 保留：超额时排序用
};

// 表现层的四个出口。fx 层**不**直接依赖 audio / CameraSystem / EditorApp：
//   · 分层干净（fx 只碰场景与资产）
//   · 自检可以塞空实现（不需要声卡、不需要相机）
//   · 编辑器与 Runtime 各自接自己的实现（两边都有音频与相机）
struct EffectSinks {
    // trauma 0..1，radius 是世界米（远处衰减）
    std::function<void(float trauma, const glm::vec3& at, float radius)> shake;
    std::function<void(const std::string& clip, float volume, float pitch,
                       const glm::vec3& at)> sound;
    // strength 0..1，seconds 是衰减时长
    std::function<void(float strength, const glm::vec3& color, float seconds)> flash;
    // 顿帧秒数（宿主用**真实时间**递减）
    std::function<void(float seconds)> hitstop;
};

// 一个槽（= 一个粒子宿主）的状态。
//
// 抽成独立的小类（而不是直接操作场景实体）是为了**能脱离场景跑用例**：
// 抢占 / 回收 / 确定性这三件事全是纯逻辑，出错形态是"少回收一个"或
// "同一帧两次运行结果不同"，靠在编辑器里看画面永远发现不了。
struct EffectSlotState {
    bool busy = false;
    std::string asset;
    float remaining = 0.0f;
    uint32_t id = 0;      // 同一次 play 的多个层共享一个 id
    int priority = 0;
    int layer = 0;        // 属于这个特效的第几层（诊断用）
};

class EffectPool {
public:
    void resize(int n);
    int size() const { return static_cast<int>(m_slots.size()); }
    int active() const;

    // 一次占 `n` 个槽（= 一个特效的层数）。槽不够 → 返回 false 并记 dropped。
    // ⚠ 分配顺序是**下标从小到大**，所以同一串请求的结果可复现。
    bool acquire(int n, const std::string& asset, float duration, int priority,
                 uint32_t& outId, std::vector<int>& outSlots);
    void release(const std::vector<int>& slots);
    // 全部置空，但**保留 id 计数**（否则 stopAll 之后新特效会拿到旧 id，
    // 脚本里还攥着的旧 id 一调 stop 就误杀了刚播的那个）。
    void clearActive();
    // 递减 remaining，到期的槽收集到 freed（**按槽号升序**）。
    void update(float dt, std::vector<int>& freed);
    const EffectSlotState& slot(int i) const { return m_slots[i]; }

    uint64_t dropped() const { return m_dropped; }
    uint64_t played() const { return m_played; }

private:
    std::vector<EffectSlotState> m_slots;
    uint64_t m_dropped = 0;
    uint64_t m_played = 0;
    uint32_t m_nextId = 1;
};

class EffectService {
public:
    EffectService() = default;
    ~EffectService();

    EffectService(const EffectService&) = delete;
    EffectService& operator=(const EffectService&) = delete;

    // 绑定场景与资产库。**不建任何实体**（懒建，见 ensurePool）。
    void attach(scene::Scene* scene, assets::AssetManager* assets);
    void detach();

    void setSinks(const EffectSinks& sinks) { m_sinks = sinks; }
    const EffectSinks& sinks() const { return m_sinks; }
    // 池大小 = 同时能播的特效层数上限。会被夹到 [1, 64]。
    void setBudget(int hosts);
    int budget() const { return m_pool.size(); }

    bool attached() const { return m_scene != nullptr; }
    bool ready() const;   // 已绑定 + 池已建

    // 播一个特效。返回实例 id（同一次 play 的多层共享）；0 = 失败或被丢。
    uint32_t play(const EffectRequest& r);

    // 每帧推进。`realDt` 必须是**真实帧间隔**（不是仿真 dt）——
    // 回收与"粒子余韵"是墙上时间，与 timeScale 无关。
    void update(float realDt);

    void stop(uint32_t id);
    void stopAll();

    std::size_t activeCount() const;
    uint64_t played() const { return m_pool.played(); }
    uint64_t dropped() const { return m_pool.dropped(); }

    // 一行状态（日志 / 面板 / 自动化判据）。**不翻译**。
    std::string stateLine() const;

    // 已缓存的资产数量（诊断：证明同一份 .veffect 只解析一次）
    std::size_t cachedAssets() const { return m_cache.size(); }

    // 取一份资产（带缓存）。失败返回 nullptr，`err` 写原因。
    const EffectAsset* asset(const std::string& relPath, std::string* err);

    // 池与调度的自检（F1~F8），不需要场景 / 设备。返回失败条数。
    int runSelfTest();

private:
    void ensurePool();
    // 把 asset 的第 layer 层参数写进宿主实体；busy=false 时静音它。
    void applyLayer(int host, const EffectAsset& a, int layer, bool active);
    void silenceHost(int host);

    scene::Scene* m_scene = nullptr;
    assets::AssetManager* m_assets = nullptr;
    EffectSinks m_sinks;
    EffectPool m_pool;
    int m_wantedBudget = 8;

    std::vector<ecs::Entity> m_hosts;      // 与 m_pool 的槽一一对应
    std::unordered_map<std::string, EffectAsset> m_cache;
    std::unordered_map<std::string, float> m_cacheScale;  // 预留
    std::vector<int> m_freedScratch;
    std::vector<int> m_slotScratch;
};

} // namespace fx
