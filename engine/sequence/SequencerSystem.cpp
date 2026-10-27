// ============================================================
// sequence/SequencerSystem.cpp
// ============================================================

#include "sequence/SequencerSystem.h"

#include "core/Logger.h"
#include "dialogue/DialogueSystem.h"
#include "ecs/Components.h"
#include "scene/CameraSystem.h"
#include "scene/Scene.h"

#include <algorithm>
#include <cctype>
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

// `kind` 是否是"该说话了"。**不区分大小写**：资产是手写的，
// `Line` / `line` / `Dialogue` 都会出现；要求大小写一致只是给人添麻烦。
// `Dialogue` 当别名收下，因为 B2 的 `AnimEvent` 那边就有人这么写。
bool kindIsLine(const std::string& k) {
    std::string s;
    s.reserve(k.size());
    for (const char c : k)
        s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return s == "line" || s == "dialogue";
}

} // namespace

SequencerSystem::SequencerSystem() = default;
// 定义在 .cpp：`m_cue` 的 unique_ptr 需要一个完整的 `ecs::SequencerComponent`
// 才析构得了。写在头里（隐式析构）会让 SequenceSelfTest.cpp 那些栈对象
// 在各自的 TU 里实例化析构 → "incomplete type"。
SequencerSystem::~SequencerSystem() = default;

SequencerSystem& activeSequencerSystem() {
    // 函数内静态：避免静态初始化顺序问题（与 `activeCameraSystem` 一致）。
    static SequencerSystem s;
    return s;
}

// ---------------------------------------------------------------- 点播

bool SequencerSystem::cue(scene::Scene& scene, const std::string& assetPath) {
    (void)scene;
    if (assetPath.empty()) return false;

    if (!m_cue) m_cue = std::make_unique<ecs::SequencerComponent>();
    // 整份重置：上一段可能 `loop = true`，不重置会让新的一段也循环。
    *m_cue = ecs::SequencerComponent{};
    m_cue->sequencePath = assetPath;
    // 资产在**点播时**就加载：这里失败要能立刻返回 false 让脚本那条
    // 语句报错，而不是"打一行 WARN、然后过场永远不出现"。
    m_cue->assetLoaded = true;
    if (!loadSequence(assetPath, m_cue->asset)) {
        m_cue->playing = false;
        VK_LOG_WARN("SEQ-CUE-FAIL path=%s err=%s", assetPath.c_str(),
                    m_cue->asset.error.c_str());
        return false;
    }

    m_cue->enabled = true;
    m_cue->playing = true;
    m_cue->loop = false;             // 点播默认单次；要循环写资产里的 loop
    m_cue->startTime = 0.0f;
    m_cue->speed = 1.0f;
    m_cue->priority = std::numeric_limits<int>::max();
    m_cue->started = false;
    m_cue->finished = false;
    m_cue->playhead = 0.0f;

    VK_LOG_INFO("SEQ-CUE path=%s dur=%.3f keys=%d events=%d",
                assetPath.c_str(),
                static_cast<double>(m_cue->asset.duration),
                static_cast<int>(m_cue->asset.cameraKeys.size()),
                static_cast<int>(m_cue->asset.events.size()));
    return true;
}

bool SequencerSystem::cuePlaying() const {
    return m_cue && m_cue->playing;
}

const std::string& SequencerSystem::cuePath() const {
    static const std::string kEmpty;
    return m_cue ? m_cue->sequencePath : kEmpty;
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
    ecs::Entity active{};
    ecs::SequencerComponent* sc = nullptr;

    // ---- 谁在播：点播 > 场景组件 ----
    // 点播没有宿主实体（`m_active` 因此是无效的），但事件派发要一个实体
    // 当参数 —— `fire()` 那边只在 `kind=Line` 时用它当对话的 host，
    // 无效句柄是容忍的（对话本来就不需要宿主也能播）。
    if (m_cue && m_cue->playing) {
        sc = m_cue.get();
        running = 1;
    } else {
        active = selectActive(scene, &running);
        if (active.valid())
            sc = scene.world().get<ecs::SequencerComponent>(active);
    }

    m_lastRunning = running;
    m_active = active;

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

    // ---- 点播播完 → 让位 ----
    // 点播是"插播"，不是"抢占"：播完要把位置还给场景里作者原本配好的
    // 时间轴。不这么做的话 `m_cue->playing` 一直是 true，场景组件永远
    // 选不上（`tick` 开头那条 `if (m_cue && m_cue->playing)` 会一直短路）。
    // ⚠ 只在 `finished` 时停：循环点播走的是 `wrapInto` 分支，永远到不了
    //   这里，所以"循环播到一半被掐掉"不会发生。
    if (m_cue && sc == m_cue.get() && sc->finished) {
        sc->playing = false;
        VK_LOG_INFO("SEQ-CUE-END path=%s dur=%.3f",
                    sc->sequencePath.c_str(),
                    static_cast<double>(dur));
    }
}

// ---------------------------------------------------------------- 派发

void SequencerSystem::fire(scene::Scene& scene, ecs::Entity e,
                           const SeqEvent& ev) {
    FiredEvent f;
    f.kind = ev.kind;
    f.payload = ev.payload;
    f.t = ev.t;
    m_fired.push_back(f);

    // ⚠ 这一层**只做两件事**：记录（给 UI / 脚本读）+ 把 `kind=Line` 转成
    //   对话。其余 kind（音效 / 旗标 / 镜头特效）仍然只记录 —— 让它们真的
    //   驱动东西是 C6（脚本导演 API）的活。写在这个函数里而不是散在
    //   调用方，是为了将来接 C6 时只有这一个地方要改。
    VK_LOG_INFO("SEQ-EVENT seq=%s kind=%s t=%.4f payload=[%s]",
                m_lastName.c_str(), ev.kind.c_str(),
                static_cast<double>(ev.t), ev.payload.c_str());

    // ---- kind=Line → 起一段对话 ----
    // `payload` 就是台词 id（例：`ch01_open_01`），由 `resolveDialoguePath`
    // 解析成 `dialogues/<id>.dlg.json`。
    // ⚠ 宿主传的是**时间轴所在的那个实体**：过场通常把 `DialogueComponent`
    //   和时间轴挂在同一个"导演轨"上，语速就从那份组件借。
    if (kindIsLine(ev.kind)) {
        dialogue::activeDialogueSystem().cue(scene, e, ev.payload);
    }
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
    // 点播状态也要清：切场景后不该还记着"上一局那段的播放头"。
    // ⚠ 保留 `m_cue` 对象本身（省一次分配），只把播放状态归零 ——
    //   与下面组件"asset 缓存保留"同一条理由。
    if (m_cue) {
        m_cue->playing = false;
        m_cue->started = false;
        m_cue->finished = false;
        m_cue->playhead = 0.0f;
    }
}

} // namespace sequence
