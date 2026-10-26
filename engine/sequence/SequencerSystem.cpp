// ============================================================
// sequence/SequencerSystem.cpp
// ============================================================

#include "sequence/SequencerSystem.h"

#include "core/Logger.h"
#include "ecs/Components.h"
#include "scene/CameraSystem.h"
#include "scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sequence {

namespace {

// 一帧最多推进多少秒。与 `AnimatorSystem` 的 `m_wallDt` 同一个理由：
// 窗口拖动 / 断点恢复 / 首帧会有几百毫秒到几秒的间隔，直接乘进播放头
// 会让一段 5 秒的过场"一下跳过去"。**这不是精度问题，是可用性问题**。
constexpr float kMaxStep = 0.10f;

bool effectiveLoop(const ecs::SequencerComponent& sc) {
    // 见 Components.h 里"或关系"的说明。
    return sc.loop || sc.asset.loop;
}

// 把 `t` 折进 `[0, dur)`，返回跨了几圈。`dur <= 0` 时原样返回 0 圈。
int wrapInto(float& t, float dur) {
    if (dur <= kSeqTimeEps) return 0;
    int wraps = 0;
    // guard 防死循环：正常游戏里一圈都跨不过去，跨多圈只可能来自
    // "speed 写到 1000" 或 "dt 巨大"，两种情况都不值得为它做正确性。
    while (t >= dur && wraps < 64) {
        t -= dur;
        ++wraps;
    }
    return wraps;
}

} // namespace

SequencerSystem& activeSequencerSystem() {
    // 函数内静态：避免静态初始化顺序问题（与 `activeCameraSystem` 一致）。
    static SequencerSystem s;
    return s;
}

void applyPoseToSceneCamera(scene::Scene& scene, const scene::CameraPose& p) {
    scene::Camera& cam = scene.camera();
    // 与 `CameraSystem::apply` 用同一组 setter —— 两处写法不一致的话，
    // "编辑器里看到的运镜"与"Game.exe 里看到的"会差一点（最典型是
    // 忘了 setUseGlobalInput(false)，编辑态下相机会同时被 WASD 推着走）。
    cam.setFlyMode(true);
    cam.setUseGlobalInput(false);
    cam.setFov(p.fov);
    cam.setYaw(p.yaw);
    cam.setPitch(p.pitch);
    cam.setPosition(p.position);
}

// ---------------------------------------------------------------- 选活跃

ecs::Entity SequencerSystem::selectActive(scene::Scene& scene,
                                          int* runningCount) {
    ecs::Entity best{};
    int bestPri = std::numeric_limits<int>::min();
    int running = 0;
    bool anyInvalidAsset = false;

    scene.world().each<ecs::SequencerComponent>(
        [&](ecs::Entity e, ecs::SequencerComponent& sc) {
            if (!sc.enabled || !sc.playing) return;
            if (sc.sequencePath.empty()) return;

            // 懒加载：只在第一次用到时读盘。放在这里而不是"场景加载时"
            // 是因为路径可能刚在 Inspector 里被改过（改完应当立刻生效）。
            if (!sc.assetLoaded) {
                sc.assetLoaded = true;
                if (!loadSequence(sc.sequencePath, sc.asset)) {
                    anyInvalidAsset = true;
                    if (!sc.loadFailedLogged) {
                        sc.loadFailedLogged = true;
                        VK_LOG_WARN("SEQ-LOAD-FAIL path=%s err=%s",
                                    sc.sequencePath.c_str(),
                                    sc.asset.error.c_str());
                    }
                    return;
                }
            }
            if (!sc.asset.valid) { anyInvalidAsset = true; return; }

            const bool loop = effectiveLoop(sc);
            if (sc.finished && !loop) return;   // 播完了就不再占位

            ++running;
            if (sc.priority > bestPri) {
                bestPri = sc.priority;
                best = e;
            }
        });

    if (runningCount) *runningCount = running;
    (void)anyInvalidAsset;
    return best;
}

// ---------------------------------------------------------------- 每帧

void SequencerSystem::tick(scene::Scene& scene, float dt) {
    if (!m_pose) m_pose = std::make_unique<scene::CameraPose>();
    // 本帧派发队列**必须**在开头清空：消费方约定"同一帧读完就走"，
    // 攒着会让音效晚一帧、让 UI 显示上一帧的事件。
    m_fired.clear();
    m_lastRunning = 0;
    m_lastPlayhead = 0.0f;
    m_lastName.clear();
    m_hasPose = false;      // 没在播的帧绝不能残留上一帧的姿态

    int running = 0;
    const ecs::Entity active = selectActive(scene, &running);
    m_lastRunning = running;
    m_active = active;

    if (!active.valid()) return;

    auto* sc = scene.world().get<ecs::SequencerComponent>(active);
    if (!sc || !sc->asset.valid) return;

    const SequenceAsset& seq = sc->asset;
    const float dur = seq.duration;
    m_lastName = seq.name;

    // ---- 编辑态（dt <= 0）：只应用当前播放头的姿态 ----
    // 不推进、不发事件、不动 `finished`（见 SequencerSystem.h 的那一段）。
    if (dt <= 0.0f) {
        m_hasPose = false;
        SeqCameraKey ck;
        if (sampleCamera(seq, sc->playhead, ck)) {
            m_pose->position = ck.position;
            m_pose->yaw = ck.yaw;
            m_pose->pitch = ck.pitch;
            m_pose->fov = ck.fov;
            m_hasPose = true;
        }
        m_lastPlayhead = sc->playhead;
        return;
    }

    // ---- 首帧 ----
    // `prev` 取 `playhead - eps` 而不是 `playhead`：这样"起播那一刻正好有个
    // 事件"（最常见的是 t=0 的开场音效 / 字幕）会被发出来。
    // ⚠ 起点是 `startTime` 时，它之前的旧事件**不会**被补发 —— 这是对的：
    //   从第 5 秒开始播不该把 0~5 秒的音效一次性糊出来。
    if (!sc->started) {
        sc->started = true;
        sc->playhead = std::max(0.0f, sc->startTime);
        if (dur > kSeqTimeEps) sc->playhead = std::min(sc->playhead, dur);
    }

    const float step = std::min(dt, kMaxStep) * std::max(sc->speed, 0.0f);
    const float prev = sc->playhead;
    float next = prev + step;

    bool wrapped = false;
    int wraps = 0;
    if (dur > kSeqTimeEps && next >= dur) {
        if (effectiveLoop(*sc)) {
            next = prev + step;      // 保留原始 next 再折（wrapInto 就地改）
            wraps = wrapInto(next, dur);
            wrapped = true;
        } else {
            next = dur;
            sc->finished = true;
        }
    }

    // ---- 事件：半开 `[from, to)` ----
    // 跨圈要扫两段：`[prev, dur)` 和 `[0, next)`。**不能**简单地用
    // `[prev, next)` —— 那在回绕时 from > to，一条都不发（症状是"循环
    // 运镜里每圈少一个音效"，而且只在跨越那一刻少，极难定位）。
    std::vector<int> ids;
    if (!wrapped) {
        collectEvents(seq, prev, next, &ids);
    } else {
        collectEvents(seq, prev, dur, &ids);
        collectEvents(seq, 0.0f, next, &ids);
        if (wraps > 1) {
            // 一帧跨多圈只发首尾两段（中间那几圈不发）。真要精确就得按圈
            // 拆开扫，但那种 speed 下画面本来也没法看，所以只报不修。
            VK_LOG_WARN("SEQ-MULTIWRAP seq=%s wraps=%d step=%.3fs dur=%.3fs "
                        "(only first and last lap dispatch events)",
                        seq.name.c_str(), wraps, static_cast<double>(step),
                        static_cast<double>(dur));
        }
    }

    for (const int id : ids) {
        if (id < 0 || id >= static_cast<int>(seq.events.size())) continue;
        fire(scene, active, seq.events[static_cast<std::size_t>(id)]);
    }

    // ---- 相机意图（不应用，交调用方）----
    m_hasPose = false;
    {
        SeqCameraKey ck;
        if (sampleCamera(seq, next, ck)) {
            m_pose->position = ck.position;
            m_pose->yaw = ck.yaw;
            m_pose->pitch = ck.pitch;
            m_pose->fov = ck.fov;
            m_hasPose = true;
        }
    }

    sc->playhead = next;
    m_lastPlayhead = next;

    if (m_trace) {
        VK_LOG_INFO("SEQ-TICK seq=%s t=%.3f/%.3f keys=%d events=%d running=%d "
                    "fired=%d wrapped=%d finished=%d",
                    seq.name.c_str(), static_cast<double>(next),
                    static_cast<double>(dur),
                    static_cast<int>(seq.cameraKeys.size()),
                    static_cast<int>(seq.events.size()), running,
                    static_cast<int>(ids.size()), wrapped ? 1 : 0,
                    sc->finished ? 1 : 0);
    }
}

// ---------------------------------------------------------------- 派发

void SequencerSystem::fire(scene::Scene& /*scene*/, ecs::Entity /*e*/,
                           const SeqEvent& ev) {
    FiredEvent f;
    f.kind = ev.kind;
    f.payload = ev.payload;
    f.t = ev.t;
    m_fired.push_back(f);

    // ⚠ 现在这一层**只记录 + 打日志**，「让事件真的驱动东西」是 C6
    // （脚本导演 API）的活。诚实的说法是：这一版能证明"到点了"，还不能
    // 让"到点了"去开灯。写在这里而不是散在调用方，是为了将来接 C6 时
    // 只有这一个地方要改。
    VK_LOG_INFO("SEQ-EVENT seq=%s kind=%s t=%.4f payload=[%s]",
                m_lastName.c_str(), ev.kind.c_str(),
                static_cast<double>(ev.t), ev.payload.c_str());
}

// ---------------------------------------------------------------- 重置

void SequencerSystem::resetRuntime(scene::Scene& scene) {
    scene.world().each<ecs::SequencerComponent>(
        [](ecs::Entity, ecs::SequencerComponent& sc) {
            sc.playhead = 0.0f;
            sc.started = false;
            sc.finished = false;
            // asset 缓存保留：路径没变就没必要重读盘。
        });
    m_active = ecs::Entity{};
    m_lastRunning = 0;
    m_lastPlayhead = 0.0f;
    m_lastName.clear();
    m_fired.clear();
}

} // namespace sequence
