#include "fx/EffectService.h"

#include "assets/AssetPath.h"
#include "core/Logger.h"
#include "ecs/Components.h"
#include "scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace fx {

// ---------------------------------------------------------------- 特效侧随机
//
// ⚠⚠ 这是**独立的一条流**，与将来的玩法随机（P2 的 `Random`）严格分开。
//   理由：音高抖动、火花角度这类"表现随机"如果和"抽牌顺序"共用一条流，
//   那么**多播一个粒子就会改变下一张抽到的牌** —— 症状是"关掉特效之后
//   抽牌顺序变了"，而这种 bug 几乎不可能靠读代码发现。
//   所以这里自己一个 xorshift32，种子固定（同一场战斗的表现可复现）。
namespace {
uint32_t g_vfxState = 0x9E3779B9u;

float nextVfx01() {
    g_vfxState ^= g_vfxState << 13;
    g_vfxState ^= g_vfxState >> 17;
    g_vfxState ^= g_vfxState << 5;
    return static_cast<float>(g_vfxState & 0x00FFFFFFu) /
           static_cast<float>(0x01000000u);
}
} // namespace

// ================================================================ EffectPool

void EffectPool::resize(int n) {
    n = std::clamp(n, 1, 64);
    if (static_cast<int>(m_slots.size()) == n) return;
    m_slots.assign(static_cast<std::size_t>(n), EffectSlotState{});
    m_slots.shrink_to_fit();
}

int EffectPool::active() const {
    int c = 0;
    for (const EffectSlotState& s : m_slots) {
        if (s.busy) ++c;
    }
    return c;
}

bool EffectPool::acquire(int n, const std::string& asset, float duration,
                         int priority, uint32_t& outId,
                         std::vector<int>& outSlots) {
    outSlots.clear();
    outId = 0;
    n = std::max(n, 1);
    if (static_cast<int>(m_slots.size()) < n) {
        ++m_dropped;
        return false;
    }

    // 先数有没有足够空闲 —— 不够就**整笔拒绝**（不留半套）。
    // ⚠ 半套是最坏的：一个特效只出了一半的层，看起来像"参数没生效"。
    int freeCount = 0;
    for (const EffectSlotState& s : m_slots) {
        if (!s.busy) ++freeCount;
    }
    if (freeCount < n) {
        ++m_dropped;
        return false;
    }

    // 按**槽号升序**取：同一串请求得到的结果可复现（截图判据依赖这条）。
    const uint32_t id = m_nextId++;
    for (int i = 0; i < static_cast<int>(m_slots.size()) && static_cast<int>(outSlots.size()) < n;
         ++i) {
        if (m_slots[i].busy) continue;
        EffectSlotState& s = m_slots[i];
        s.busy = true;
        s.asset = asset;
        s.remaining = std::max(duration, 0.01f);
        s.id = id;
        s.priority = priority;
        s.layer = static_cast<int>(outSlots.size());
        outSlots.push_back(i);
    }

    outId = id;
    ++m_played;
    return true;
}

void EffectPool::release(const std::vector<int>& slots) {
    for (int i : slots) {
        if (i < 0 || i >= static_cast<int>(m_slots.size())) continue;
        m_slots[i] = EffectSlotState{};
    }
}

void EffectPool::clearActive() {
    for (EffectSlotState& s : m_slots) s = EffectSlotState{};
}

void EffectPool::update(float dt, std::vector<int>& freed) {
    freed.clear();
    if (!(dt > 0.0f)) return;   // dt <= 0 不推进（暂停 / 顿帧期间不回收）
    for (int i = 0; i < static_cast<int>(m_slots.size()); ++i) {
        EffectSlotState& s = m_slots[i];
        if (!s.busy) continue;
        s.remaining -= dt;
        if (s.remaining <= 0.0f) {
            s = EffectSlotState{};
            freed.push_back(i);   // 天然按槽号升序
        }
    }
}

// ============================================================ EffectService

EffectService::~EffectService() { detach(); }

void EffectService::attach(scene::Scene* scene, assets::AssetManager* assets) {
    if (m_scene == scene && m_assets == assets) return;
    detach();
    m_scene = scene;
    m_assets = assets;
    m_cache.clear();
}

void EffectService::detach() {
    if (m_scene) {
        for (ecs::Entity e : m_hosts) {
            if (m_scene->world().alive(e)) m_scene->world().destroy(e);
        }
    }
    m_hosts.clear();
    m_pool.clearActive();
    m_pool.resize(1);
    m_cache.clear();
    m_scene = nullptr;
    m_assets = nullptr;
}

void EffectService::setBudget(int hosts) {
    const int want = std::clamp(hosts, 1, 64);
    if (want == m_wantedBudget) return;
    m_wantedBudget = want;
    if (!m_hosts.empty()) {
        // 改池大小 = 宿主实体整批重建，正在播的特效会断。
        // ⚠ 这里**不能**调 detach()：那会把 m_scene 也清掉，之后
        //   play() 全部静默失败（"调了预算就再也不出特效"）。
        for (std::size_t i = 0; i < m_hosts.size(); ++i) {
            if (m_scene && m_scene->world().alive(m_hosts[i])) {
                m_scene->world().destroy(m_hosts[i]);
            }
        }
        m_hosts.clear();
        m_pool.clearActive();
        m_pool.resize(1);
    }
}

bool EffectService::ready() const {
    return m_scene != nullptr && !m_hosts.empty() &&
           static_cast<int>(m_hosts.size()) == m_pool.size();
}

void EffectService::ensurePool() {
    if (!m_scene) return;
    const int n = std::clamp(m_wantedBudget, 1, 64);
    if (static_cast<int>(m_hosts.size()) == n) {
        // 场景可能被整体 Load 过（实体全换了）→ 宿主已经失效，重建。
        bool alive = true;
        for (ecs::Entity e : m_hosts) {
            if (!m_scene->world().alive(e)) {
                alive = false;
                break;
            }
        }
        if (alive) return;
        m_hosts.clear();
        m_pool.clearActive();
        m_pool.resize(1);
    }

    m_pool.resize(n);
    m_hosts.resize(static_cast<std::size_t>(n));

    for (int i = 0; i < n; ++i) {
        ecs::Entity e = m_scene->createObject("FX_HOST_" + std::to_string(i));
        m_scene->world().add<ecs::FxHostComponent>(e, ecs::FxHostComponent{i});

        // 起点是一颗"空"发射器：rate 0、无 burst。见 EffectService.h 头注释
        // —— 空闲宿主**不能**用 enabled=false 表示，否则发射器下标会前移。
        ecs::ParticleEmitterComponent pc;
        pc.enabled = true;
        pc.rate = 0.0f;
        pc.rateOverDistance = 0.0f;
        pc.prewarm = false;
        pc.looping = false;
        for (int b = 0; b < ecs::kMaxBursts; ++b) pc.bursts[b].count = 0;

        // 粒子组件的默认 blend 是加色（打击特效最常见），保持默认不动。
        m_scene->world().add<ecs::ParticleEmitterComponent>(e, pc);
        m_hosts[static_cast<std::size_t>(i)] = e;
    }

    VK_LOG_INFO("FX-POOL hosts=%d", n);
}

const EffectAsset* EffectService::asset(const std::string& relPath,
                                        std::string* err) {
    if (relPath.empty()) {
        if (err) *err = "empty asset path";
        return nullptr;
    }
    // 路径归一化：作者在事件表 / 脚本里写 `fx/sword_hit` 就想播
    // `fx/sword_hit.veffect`（文档 G3 的写法就是这个，不带扩展名）。
    //
    // ⚠ 只补**没有扩展名**的：`fx/a.b` 这种（名字里带点）不动 ——
    //   盲目 `substr(0, rfind('.'))` 会把 `fx/v1.2_hit` 截成 `fx/v1`。
    //   判据是"最后一个 `/` 之后再出现点"，没出现才算没扩展名。
    std::string key = relPath;
    {
        const std::size_t slash = key.find_last_of("/\\");
        const std::size_t dot = key.find_last_of('.');
        const bool hasExt = (dot != std::string::npos &&
                             (slash == std::string::npos || dot > slash));
        if (!hasExt) key += ".veffect";
    }
    auto it = m_cache.find(key);
    if (it != m_cache.end()) return it->second.valid ? &it->second : nullptr;

    // 有 AssetManager 就走它的解析（打包后可能是 `pak:` 虚拟路径）；
    // 没有就直接当文件系统路径用（自检路径）。
    std::string abs = key;
    if (m_assets) abs = assets::resolveAssetPath(key);

    std::string loadErr;
    EffectAsset a = loadEffectAssetFile(abs, &loadErr);
    if (!a.valid) {
        if (err) *err = loadErr.empty() ? ("invalid effect asset: " + relPath)
                                        : loadErr;
        // 缓存失败结果：缺一个 .veffect 时不该每次命中都去碰一次磁盘。
        m_cache.emplace(key, EffectAsset{});
        return nullptr;
    }
    if (a.warnings.empty()) {
        // 没有 warning：正常路径，什么都不打（一次命中一行会把日志刷爆）。
    } else {
        for (const std::string& w : a.warnings) {
            VK_LOG_WARN("FX-ASSET %s: %s", relPath.c_str(), w.c_str());
        }
    }

    auto res = m_cache.emplace(key, std::move(a));
    return &res.first->second;
}

void EffectService::silenceHost(int host) {
    if (host < 0 || host >= static_cast<int>(m_hosts.size())) return;
    ecs::Entity e = m_hosts[static_cast<std::size_t>(host)];
    if (!m_scene || !m_scene->world().alive(e)) return;
    auto* pc = m_scene->world().get<ecs::ParticleEmitterComponent>(e);
    if (!pc) return;

    // ⚠ 只关"发新粒子"，**不**碰 `enabled`（理由见头注释）。
    //   已经在飞的粒子会自然活完 —— 这正是"火花不会定格在空中"的原因。
    pc->rate = 0.0f;
    pc->rateOverDistance = 0.0f;
    pc->prewarm = false;
    for (int b = 0; b < ecs::kMaxBursts; ++b) pc->bursts[b].count = 0;
}

void EffectService::applyLayer(int host, const EffectAsset& a, int layer,
                               bool active) {
    if (host < 0 || host >= static_cast<int>(m_hosts.size())) return;
    ecs::Entity e = m_hosts[static_cast<std::size_t>(host)];
    if (!m_scene || !m_scene->world().alive(e)) return;

    auto* pc = m_scene->world().get<ecs::ParticleEmitterComponent>(e);
    if (!pc) return;

    if (layer >= 0 && layer < static_cast<int>(a.particles.size())) {
        // 整份拷过来：字段清单与场景里那份完全一致（同一份解析）。
        *pc = a.particles[static_cast<std::size_t>(layer)];
        pc->enabled = true;   // 见头注释：**永远** true
    } else {
        // 纯打击感特效占的那个"陪跑槽"：保持空发射器。
        *pc = ecs::ParticleEmitterComponent{};
        pc->enabled = true;
        pc->rate = 0.0f;
        pc->rateOverDistance = 0.0f;
        pc->prewarm = false;
        for (int b = 0; b < ecs::kMaxBursts; ++b) pc->bursts[b].count = 0;
    }

    if (!active) silenceHost(host);
}

uint32_t EffectService::play(const EffectRequest& r) {
    if (!m_scene) return 0;
    ensurePool();
    if (m_hosts.empty()) return 0;

    std::string err;
    const EffectAsset* a = asset(r.asset, &err);
    if (!a) {
        // 只报一次（同一个路径），否则连击时日志会被刷爆。
        static std::string lastBad;
        if (lastBad != r.asset) {
            lastBad = r.asset;
            VK_LOG_WARN("FX-PLAY failed: %s (%s)", r.asset.c_str(), err.c_str());
        }
        return 0;
    }

    const int layers = std::max(1, static_cast<int>(a->particles.size()));
    uint32_t id = 0;
    m_slotScratch.clear();
    if (!m_pool.acquire(layers, r.asset, a->effectiveDuration() / std::max(r.speed, 0.01f),
                        r.priority, id, m_slotScratch)) {
        return 0;   // 池满，被丢弃（确定性规则，见头注释）
    }

    // 位置 / 朝向 / 缩放：写进宿主 transform。
    for (int i = 0; i < static_cast<int>(m_slotScratch.size()); ++i) {
        const int slot = m_slotScratch[static_cast<std::size_t>(i)];
        ecs::Entity e = m_hosts[static_cast<std::size_t>(slot)];
        if (!m_scene->world().alive(e)) continue;
        auto* tr = m_scene->world().get<ecs::TransformComponent>(e);
        if (tr) {
            tr->position = r.position;
            // 朝向：把**局部 +Z** 转到 `forward`（锥 / 盒形状都沿局部 +Z 发射）。
            //
            // `TransformComponent::localMatrix()` 的欧拉顺序是"先 Rz、再 Ry、
            // 最后 Rx"（矩阵右乘 Rx*Ry*Rz）。取 rot.z = 0 后就是 Rx(pitch)*Ry(yaw)，
            // 解 Ry(yaw)·(0,0,1) = (sin y, 0, cos y) 再解 Rx(pitch) 得：
            //     yaw   = asin(f.x)
            //     pitch = atan2(-f.y, f.z)
            // ⚠ 不要用 atan2(f.y, f.x) 那一套（那是"绕 Z 的平面角"），
            //   算出来的朝向在俯仰上差 90°，表现是"火花从脚下往外喷"。
            const glm::vec3 f = (glm::length(r.forward) > 1.0e-6f)
                                    ? glm::normalize(r.forward)
                                    : glm::vec3(0.0f, 0.0f, 1.0f);
            const float yaw = std::asin(std::clamp(f.x, -1.0f, 1.0f));
            const float pitch = std::atan2(-f.y, f.z);
            tr->rotation = glm::vec3(pitch, yaw, 0.0f);
            tr->scale = glm::vec3(std::max(r.scale, 0.01f));
        }
        applyLayer(slot, *a, i, true);
    }

    // ---- 表现层四个出口 ----
    if (a->shake > 0.0f && m_sinks.shake) {
        m_sinks.shake(a->shake, r.position, a->shakeRadius);
    }
    if (!a->sound.empty() && m_sinks.sound) {
        const float jitter = 1.0f + (nextVfx01() * 2.0f - 1.0f) * a->pitchVariance;
        m_sinks.sound(a->sound, a->volume, jitter, r.position);
    }
    if (a->flash > 0.0f && a->flashDuration > 0.0f && m_sinks.flash) {
        m_sinks.flash(a->flash, a->flashColor, a->flashDuration);
    }
    if (a->hitstop > 0.0f && m_sinks.hitstop) {
        m_sinks.hitstop(a->hitstop);
    }

    // ⚠ 默认**不打**这一行：一场战斗里每次命中都打会把日志淹掉。
    //   自动化需要"脚本调 playEffect 真的占到了槽"的硬证据，所以给一个
    //   显式开关（与 `MYVK_LOG_FLAGS` / `MYVK_LOG_SCRIPT_PRINT` 同一条规矩）。
    static const bool logPlays = [] {
        const char* e = std::getenv("MYVK_LOG_FX");
        return e && e[0] == '1';
    }();
    if (logPlays) {
        VK_LOG_INFO("FX-PLAY path=%s id=%u active=%d src=service",
                    r.asset.c_str(), id, m_pool.active());
    }

    return id;
}

void EffectService::update(float realDt) {
    if (!m_scene || m_hosts.empty()) return;
    m_freedScratch.clear();
    m_pool.update(realDt, m_freedScratch);
    for (int slot : m_freedScratch) silenceHost(slot);
}

void EffectService::stop(uint32_t id) {
    if (id == 0) return;
    m_slotScratch.clear();
    for (int i = 0; i < m_pool.size(); ++i) {
        if (m_pool.slot(i).busy && m_pool.slot(i).id == id) m_slotScratch.push_back(i);
    }
    for (int slot : m_slotScratch) silenceHost(slot);
    m_pool.release(m_slotScratch);
}

void EffectService::stopAll() {
    for (int i = 0; i < m_pool.size(); ++i) silenceHost(i);
    m_pool.clearActive();
}

std::size_t EffectService::activeCount() const {
    return static_cast<std::size_t>(m_pool.active());
}

std::string EffectService::stateLine() const {
    char buf[256];
    // i18n-keep（D 组）：给脚本读的解析行（`fx.state`），不上屏。
    std::snprintf(buf, sizeof(buf),
                  "fx budget=%d active=%d played=%llu dropped=%llu cached=%zu ready=%d",
                  m_pool.size(), m_pool.active(),
                  static_cast<unsigned long long>(m_pool.played()),
                  static_cast<unsigned long long>(m_pool.dropped()),
                  m_cache.size(), ready() ? 1 : 0);
    return std::string(buf);
}

// ---------------------------------------------------------------- 自检

namespace {

int g_fxSvcFail = 0;
int g_fxSvcPass = 0;

void svcCheck(const char* name, bool ok, const std::string& detail = {}) {
    if (ok) {
        ++g_fxSvcPass;
        std::printf("FX-POOL PASS %s\n", name);
    } else {
        ++g_fxSvcFail;
        std::printf("FX-POOL FAIL %s%s%s\n", name, detail.empty() ? "" : " -- ",
                    detail.c_str());
    }
}

} // namespace

int EffectService::runSelfTest() {
    g_fxSvcFail = 0;
    g_fxSvcPass = 0;

    // ---- F1 初始状态 ----
    {
        EffectPool p;
        p.resize(4);
        svcCheck("F1 resize", p.size() == 4 && p.active() == 0);
    }

    // ---- F2 按槽号升序分配（确定性）----
    {
        EffectPool p;
        p.resize(4);
        uint32_t id = 0;
        std::vector<int> a, b;
        const bool ok1 = p.acquire(1, "x", 1.0f, 0, id, a);
        svcCheck("F2 first-slot", ok1 && a.size() == 1 && a[0] == 0 && id == 1,
                 "slot=" + std::to_string(a.empty() ? -1 : a[0]));
        const bool ok2 = p.acquire(2, "y", 1.0f, 0, id, b);
        svcCheck("F2 next-two-slots", ok2 && b.size() == 2 && b[0] == 1 && b[1] == 2);
    }

    // ---- F3 池满：整笔拒绝 + dropped 递增（不留半套）----
    {
        EffectPool p;
        p.resize(3);
        uint32_t id = 0;
        std::vector<int> s;
        p.acquire(3, "a", 1.0f, 0, id, s);
        const bool ok = p.acquire(2, "b", 1.0f, 0, id, s);
        svcCheck("F3 full-rejects-whole", !ok && s.empty());
        svcCheck("F3 dropped-counted", p.dropped() == 1 && p.played() == 1);
    }

    // ---- F4 到期回收 ----
    {
        EffectPool p;
        p.resize(2);
        uint32_t id = 0;
        std::vector<int> s, freed;
        p.acquire(2, "a", 0.5f, 0, id, s);
        p.update(0.3f, freed);
        svcCheck("F4 not-yet", freed.empty() && p.active() == 2);
        p.update(0.3f, freed);
        svcCheck("F4 expired", freed.size() == 2 && p.active() == 0,
                 "freed=" + std::to_string(freed.size()));
        svcCheck("F4 freed-order", freed.size() == 2 && freed[0] == 0 && freed[1] == 1);
    }

    // ---- F5 dt <= 0 不推进（顿帧/暂停期间不回收）----
    {
        EffectPool p;
        p.resize(2);
        uint32_t id = 0;
        std::vector<int> s, freed;
        p.acquire(1, "a", 0.1f, 0, id, s);
        p.update(0.0f, freed);
        svcCheck("F5 zero-dt-keeps", freed.empty() && p.active() == 1);
    }

    // ---- F6 再分配复用刚释放的槽（不泄漏）----
    {
        EffectPool p;
        p.resize(2);
        uint32_t id = 0;
        std::vector<int> s, freed;
        p.acquire(1, "a", 0.1f, 0, id, s);
        const int first = s.empty() ? -1 : s[0];
        p.update(0.2f, freed);
        p.acquire(1, "b", 0.1f, 0, id, s);
        svcCheck("F6 reuse-slot", !s.empty() && s[0] == first);
    }

    // ---- F7 id 单调递增且从 1 起（0 是"失败"的哨兵）----
    {
        EffectPool p;
        p.resize(4);
        uint32_t id1 = 0, id2 = 0;
        std::vector<int> s;
        p.acquire(1, "a", 1.0f, 0, id1, s);
        p.acquire(1, "b", 1.0f, 0, id2, s);
        svcCheck("F7 id-monotonic", id1 == 1 && id2 == 2 && id1 != 0 && id2 != 0);
    }

    // ---- F8 一个特效的多层共享同一个 id（回收时能整笔找到）----
    {
        EffectPool p;
        p.resize(4);
        uint32_t id = 0;
        std::vector<int> s;
        p.acquire(3, "a", 1.0f, 0, id, s);
        bool same = s.size() == 3;
        for (int slot : s) {
            if (p.slot(slot).id != id) same = false;
        }
        svcCheck("F8 layers-share-id", same && p.active() == 3);
    }

    std::printf("FX-POOL-TOTAL pass=%d fail=%d\n", g_fxSvcPass, g_fxSvcFail);
    return g_fxSvcFail;
}

} // namespace fx
