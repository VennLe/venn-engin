#include "ScriptRuntime.h"

#include "core/Logger.h"

#include <algorithm>
#include "ecs/Components.h"
#include "scene/Scene.h"

#include <utility>

namespace vks {

namespace {

// 热重载轮询间隔。
// ⚠ 为什么在 tick 里节流而不是每步都调：`ScriptAsset::poll()` 本身
//   "时间戳没变就 return false"很便宜，但 `last_write_time` 是**系统调用**，
//   而 tick 是每个固定步一次（60 Hz × 每步几次）—— 一百个脚本就是每秒
//   上万次 stat。0.5 s 与 `ScriptAsset` 时代的观感一致（改完半秒内生效）。
constexpr double kReloadInterval = 0.5;

// 状态变化才打一行（S5）。
//
// ⚠⚠ 这个函数必须被**四条**路径共用：① 实例表同步后（新建 / 换脚本）、
//    ② 熔断跳过（`inst.failed` 帧）、③ 本帧执行失败置熔断、④ 正常跑完。
//    原来只在 ④ 里内联着打，于是 `state=Failed` **一行都不会出现** ——
//    两条失败路径都在 `continue` 之前把 `failed` 置位，日志块根本走不到。
//    F5 第一次跑就是这么红的（面板上红字明明画出来了，日志里查无此行）。
void noteState(const ScriptRuntime& rt, ScriptInstance& inst, ecs::World& w) {
    const char* ns = rt.stateOf(inst);
    if (inst.stateLogged == ns) return;
    inst.stateLogged = ns;
    const std::string* nm = w.name(inst.entity);
    VK_LOG_INFO("SCR-STATE entity=%u name=%s state=%s vars=%zu", inst.entity.id,
                nm ? nm->c_str() : "?", ns, inst.vars.size());
}

} // namespace

// ---------------------------------------------------------------- 生命周期

void ScriptRuntime::reset() {
    m_instances.clear();
    m_index.clear();
    m_scene = nullptr;
    m_stats = Stats{};
    // 接触事件跟着场景走：换场景/停止时不能把上一局的碰撞带过来
    m_contacts.clear();
    m_contactCount = 0;
    m_reloadAccum = 0.0;
    m_syncedOnce = false;
}

bool ScriptRuntime::resetInstance(ecs::Entity e) {
    ScriptInstance* inst = find(e);
    if (!inst) return false;
    // 保留 path / enabled / timeScale（那是场景数据），只清**运行状态**。
    inst->vars.clear();
    inst->localTime = 0.0;
    inst->simFrames = 0;
    inst->failed = false;
    inst->started = false;
    inst->runCount = 0;
    inst->stepsLastFrame = 0;
    inst->lastError.clear();
    inst->progVersion = 0;
    inst->prints.clear();
    // ★ S4：协程也必须清 —— 否则 Reset 之后实例**还停在旧的 wait 里**
    //   （wakeAt 是 100 秒后），界面显示"重置成功"但脚本一动不动。
    //   换一个空协程就等于"回到从未执行过的状态"。
    inst->coro = m_engine.makeCoroutine();
    inst->waitingNow = false;
    // ⚠ stateLogged 故意**不清**：Reset 后状态若仍是 Running，就没必要再打
    //   一行；若从 Failed 变回 Running，变化本身会让 next tick 打印出来。
    return true;
}

ScriptInstance* ScriptRuntime::find(ecs::Entity e) {
    auto it = m_index.find(e.id);
    if (it == m_index.end()) return nullptr;
    ScriptInstance* inst = m_instances[it->second].get();
    // generation 对不上 = 句柄被复用（老实体删了，新实体拿了同一个 id）。
    // ⚠ 必须在这里挡住：否则"删掉 Boss 再建个小怪"会让小怪继承 Boss 的
    //   血量 —— 这类 bug 从日志上完全看不出来。
    if (inst->entity.generation != e.generation) return nullptr;
    return inst;
}

const ScriptInstance* ScriptRuntime::find(ecs::Entity e) const {
    return const_cast<ScriptRuntime*>(this)->find(e);
}

void ScriptRuntime::reportError(ScriptInstance& inst, const std::string& msg,
                                ScriptInstance::ErrorKind kind) {
    inst.errorKind = kind;
    // ★ P4 的另一半：内容没变就不再报。原实现每帧打一条一模一样的 WARN，
    //   日志被刷爆之后真正的错误反而看不见。
    if (inst.lastError == msg) return;
    inst.lastError = msg;
    if (m_errorSink) {
        m_errorSink(inst, msg);
    } else {
        VK_LOG_WARN("VKS-ERROR entity=%u path=%s %s", inst.entity.id,
                    inst.path.c_str(), msg.c_str());
    }
}

// ---------------------------------------------------------------- 实例表同步

void ScriptRuntime::syncInstances(scene::Scene& sc) {
    // 场景身份变了（编辑态 ↔ 运行态副本 / 换关卡）→ 整体重建。
    // ⚠ 不重建的话，两个场景里 id 相同的实体会互相串状态。
    if (&sc != m_scene) {
        m_instances.clear();
        m_index.clear();
        m_scene = &sc;
    }

    std::vector<char> seen(m_instances.size(), 0);
    ecs::World& w = sc.world();

    // 遍历顺序 = ECS dense 顺序（S-D11 的确定性要求：脚本执行顺序必须
    // 可复现，否则回归脚本逐行比日志的策略直接失效）。
    w.each<ecs::ScriptComponent>(
        [&](ecs::Entity e, ecs::ScriptComponent& scomp) {
            auto it = m_index.find(e.id);
            if (it != m_index.end()) {
                ScriptInstance& inst = *m_instances[it->second];
                if (inst.entity.generation != e.generation) {
                    // 句柄被复用 → 当成一个**新**实体，从头来
                    inst.entity = e;
                    inst.vars.clear();
                    inst.localTime = 0.0;
                    inst.simFrames = 0;
                    inst.failed = false;
                    inst.started = false;
                    inst.runCount = 0;
                    inst.lastError.clear();
                    inst.progVersion = 0;
                }
                seen[it->second] = 1;

                // 换脚本 = 换逻辑，运行状态必须清零（S0 语义）。
                // ⚠ 与"热重载"不同：热重载是**同一个** path 换了 AST，
                //   那边保状态（S-D7），这边换的是另一个文件。
                if (inst.path != scomp.path) {
                    inst.path = scomp.path;
                    inst.vars.clear();
                    inst.localTime = 0.0;
                    inst.simFrames = 0;
                    inst.failed = false;
                    inst.started = false;
                    inst.runCount = 0;
                    inst.lastError.clear();
                    inst.progVersion = 0;
                }
                inst.enabled = scomp.enabled;
                inst.timeScale = scomp.timeScale;
                return;
            }

            auto inst = std::make_unique<ScriptInstance>();
            inst->entity = e;
            inst->path = scomp.path;
            inst->enabled = scomp.enabled;
            inst->timeScale = scomp.timeScale;
            m_index[e.id] = m_instances.size();
            m_instances.push_back(std::move(inst));
            seen.push_back(1);
        });

    // 回收消失的实体。用"整体压缩 + 重建索引"而不是逐个 erase：
    // 后者要么 O(n²)，要么得手工修所有受影响的下标——那种代码改一次错一次。
    bool anyGone = false;
    for (std::size_t i = 0; i < m_instances.size(); ++i) {
        if (!seen[i]) { anyGone = true; break; }
    }
    if (anyGone) {
        std::vector<std::unique_ptr<ScriptInstance>> keep;
        keep.reserve(m_instances.size());
        for (std::size_t i = 0; i < m_instances.size(); ++i) {
            if (seen[i]) keep.push_back(std::move(m_instances[i]));
        }
        m_instances = std::move(keep);
        m_index.clear();
        for (std::size_t i = 0; i < m_instances.size(); ++i) {
            m_index[m_instances[i]->entity.id] = i;
        }
    }

    // ★ S5：同步完把"新出现的 / 状态变了的"实例各报一行。
    //   ⚠ 编辑态**也必须**报 —— 那时 tick 一次都不跑（S-D10：编辑态不许执行
    //     脚本），如果只在 tick 里打，Instances 面板在日志里就是一片空白，
    //     F2 的"Idle → Running"只能靠 Play 之后那条 Running 猜（假绿）。
    for (auto& up : m_instances) noteState(*this, *up, w);
}

// 实体名（找不到返回空）—— 只给接触事件的 Value 缓存名字用
std::string nameOfIn(scene::Scene& sc, ecs::Entity e) {
    const std::string* n = sc.world().name(e);
    return n ? *n : std::string();
}

// ---------------------------------------------------------------- 状态（S5）

const char* ScriptRuntime::stateOf(const ScriptInstance& inst) const {
    if (inst.failed) return "Failed";
    // ⚠ Waiting 必须在 Running 之前判 —— 挂起的实例"跑过"，但这一刻它在等。
    if (inst.waitingNow) return "Waiting";
    if (!inst.started) return "Idle";
    return "Running";
}

std::vector<std::pair<std::string, std::string>>
ScriptRuntime::sortedVars(const ScriptInstance& inst) const {
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(inst.vars.size());
    for (const auto& kv : inst.vars) out.emplace_back(kv.first, kv.second.toStr());
    // 排序不只是"好看"：自动化断言读的是控制台里那几行，哈希序每次进程都不同。
    std::sort(out.begin(), out.end(),
              [](const std::pair<std::string, std::string>& a,
                 const std::pair<std::string, std::string>& b) {
                  return a.first < b.first;
              });
    return out;
}

// ---------------------------------------------------------------- tick

void ScriptRuntime::tick(scene::Scene& sc, double simTime, float dt,
                         uint64_t renderFrame) {
    syncInstances(sc);

    // ---- 热重载（路径级，与实例表无关）----
    m_reloadAccum += static_cast<double>(dt);
    if (!m_syncedOnce || m_reloadAccum >= kReloadInterval) {
        m_reloadAccum = 0.0;
        m_engine.pollHotReload();
    }
    m_syncedOnce = true;

    Stats st;
    st.total = static_cast<uint32_t>(m_instances.size());

    for (auto& up : m_instances) {
        ScriptInstance& inst = *up;
        if (!inst.enabled || inst.path.empty()) {
            ++st.skipped;
            continue;
        }

        ScriptAsset* asset = m_engine.get(inst.path);
        ScriptProgram* prog = asset ? asset->program() : nullptr;

        // ---- 热重载成功 → 解除熔断（S-D8 的三个出口之一）----
        // ⚠ 用**版本号**而不是指针（见 ScriptAsset::version() 的注释）：
        //   `clear()` 释放旧 Program 之后，allocator 完全可能把新对象放在
        //   同一个地址 → 指针相等 → 误判"没变" → 熔断永不解除。
        //   版本号只在成功编译后才前进，"改了但没编译过"不会解熔断。
        const uint64_t ver = asset ? asset->version() : 0;
        if (ver != inst.progVersion) {
            if (inst.progVersion != 0) {
                // ★ S4 / S-D7 第 3 条：先把"挂在哪条语句上"翻译到新 AST。
                //   ⚠ 此刻旧 Program 已经析构、栈里的 `list` 指针全悬垂，
                //   但 reattach 只读帧里的**数值**（ownerId / ip / kind），
                //   不 deref 旧指针 —— 这是整套设计能成立的前提。
                //   prog 为空（新版本编译失败）时 kept=true：什么都别动，
                //   等它编译好了再做一次对账。
                const bool kept = !prog || prog->reattach(*inst.coro);
                // 对账成功也留一行（F3）：`vars` 是"变量没被清"的数字证据，
                // 而"变量没清"正是 S-D7 第 1 条要保的东西。
                if (kept && prog) {
                    VK_LOG_INFO("VKS-RELOAD-KEEP entity=%u path=%s vars=%zu",
                                inst.entity.id, inst.path.c_str(),
                                inst.vars.size());
                }
                if (!kept) {
                    // 结构变了（增删语句 / 改了钩子）→ 整体重启。
                    // ⚠ 变量清空是"重启"的一部分：不清的话，新脚本里新加的
                    //   `let` 会因为"已有值"被永久跳过。
                    VK_LOG_INFO("VKS-RELOAD-RESTART entity=%u path=%s",
                                inst.entity.id, inst.path.c_str());
                    // 换一个全新的空协程就是"清空"—— 不必看见它的布局
                    inst.coro = m_engine.makeCoroutine();
                    inst.vars.clear();
                    inst.started = false;
                    inst.localTime = 0.0;
                    ++inst.restartCount;
                }
                inst.failed = false;
                // ⚠ S-D7 第 2 条写在这里是**刻意**的：`on start` **不重跑**。
                //   `started` 保持原值不动 —— 热重载最常见的用法是"调一个
                //   数字"，此时把出生点重置成"重载时所在的位置"就是错的。
                inst.lastError.clear();
                inst.errorKind = ScriptInstance::ErrorKind::None;
            }
            inst.progVersion = ver;
            // 编译失败时 prog 是 nullptr —— 下面会走"跑不了"分支报错。
        }

        // ---- 熔断：后续帧一律不执行（P4）----
        if (inst.failed) {
            ++st.failed;
            noteState(*this, inst, sc.world());
            continue;
        }

        // ---- 编译没过 / 文件不存在 ----
        if (!prog || !prog->valid()) {
            const std::string msg =
                asset ? asset->lastError().toString()
                      : std::string("script asset unavailable");
            reportError(inst, msg, ScriptInstance::ErrorKind::Compile);
            ++st.failed;
            continue;
        }

        // ---- 时间（P3）----
        const double ts = static_cast<double>(inst.timeScale);
        const double stepDt = static_cast<double>(dt) * ts;
        double time = 0.0;
        if (simTime == kSelfTimed) {
            inst.localTime += stepDt;
            time = inst.localTime;
        } else {
            // 预览态：跟随外部时间轴（编辑器动画要能拖、能回放）
            time = simTime * ts;
            inst.localTime = time;
        }
        ++inst.simFrames;  // P7：固定步号，严格 +1

        // 协程是懒创建的：没写过 wait 的实例也有一份（空栈，几字节），
        // 但这样 tick 里就没有分支判断"要不要建"了。
        if (!inst.coro) inst.coro = m_engine.makeCoroutine();

        // ---- 宿主（唯一接缝）----
        ScriptHost host;
        host.scene = &sc;
        host.entity = inst.entity;
        host.setHealth = m_services.setHealth;
        host.damage = m_services.damage;
        host.popup = m_services.popup;
        host.playSound = m_services.playSound;
        host.stopAllSounds = m_services.stopAllSounds;
        host.setBusVolume = m_services.setBusVolume;
        host.act = m_services.act;   // S2：玩家 / 物理 / 输入，整块拷

        ScriptRunContext ctx;
        ctx.vars = &inst.vars;
        ctx.prints = &inst.prints;
        ctx.time = time;
        ctx.dt = stepDt;
        ctx.frame = renderFrame;
        ctx.simFrame = inst.simFrames;

        // ---- 本帧要喂给这个实例的东西（S3）----
        ScriptFrameInput in;
        in.runStart = !inst.started;
        // 接触事件：本实体是 a 就把 b 当"对方"，反之亦然。
        // ⚠ 双方都要收到 —— 否则"撞人的一方有反应、被撞的一方没有"，
        //   而这种不对称在双人场景里才会暴露。
        for (const ScriptContact& c : m_contacts) {
            ecs::Entity other;
            bool mine = false;
            if (c.a.id == inst.entity.id && c.a.generation == inst.entity.generation) {
                other = c.b;
                mine = true;
            } else if (c.b.id == inst.entity.id &&
                       c.b.generation == inst.entity.generation) {
                other = c.a;
                mine = true;
            }
            if (!mine) continue;

            ScriptFrameInput::Contact ce;
            ce.other = Value::makeEntity(other, other.valid() ? nameOfIn(sc, other)
                                                              : std::string());
            switch (c.kind) {
                case ScriptContact::Kind::Enter:
                    ce.kind = c.trigger ? HookKind::Trigger : HookKind::Collide;
                    break;
                case ScriptContact::Kind::Stay:
                    // 触发体只管"进去那一下"，Stay 不再派发（否则站在门口
                    // 每步都开一次门）；硬碰撞的 Stay 要派发（持续伤害）。
                    if (c.trigger) continue;
                    ce.kind = HookKind::Collide;
                    break;
                case ScriptContact::Kind::Exit:
                    ce.kind = HookKind::Exit;
                    break;
            }
            in.contacts.push_back(std::move(ce));
        }

        ScriptError err;
        const bool ok = prog->runFrame(host, ctx, *inst.coro, in, err);
        inst.stepsLastFrame = ctx.steps;
        st.steps += ctx.steps;

        // `on start` 跑过就置位。⚠ 失败时也置位：不然熔断解除后它会重跑
        // start，而"重跑 start"= 把出生点/血量重置 —— S-D7 明确禁止。
        inst.started = true;

        if (!ok) {
            inst.failed = true;  // ★ 熔断
            reportError(inst, err.toString(), ScriptInstance::ErrorKind::Runtime);
            ++st.failed;
            // ⚠ Failed 这一行必须在这里打：下一个 `continue` 一走，
            //   循环末尾那段日志就永远到不了。
            noteState(*this, inst, sc.world());
            continue;
        }

        if (!inst.lastError.empty()) {
            inst.lastError.clear();
            inst.errorKind = ScriptInstance::ErrorKind::None;
        }
        ++inst.runCount;
        // ★ S4：`wait` 还没到点的实例只是"看表"了一圈，不算执行。
        //   E4 的 O(活跃) 全靠这个判据 —— 1000 个实例在等时，每帧
        //   只做一次时间比较，不碰任何语句。
        if (ctx.executed) {
            ++st.executed;
            inst.waitingNow = false;
        } else {
            ++st.skipped;
            inst.waitingNow = true;   // 本帧没动 = 挂在 wait 里
        }

        // 状态变化才打一行（S5）：`Idle → Running → Waiting` 这一串是
        // 自动化唯一能"文本双证"的东西（另一证是面板像素）。
        noteState(*this, inst, sc.world());

        if (m_printSink) {
            for (const std::string& line : inst.prints) m_printSink(inst, line);
        }
    }

    m_stats = st;

    // 本步的事件派发完了。⚠ 放在**末尾**而不是开头：开头清会让"先 tick
    // 再投递"的宿主顺序静默丢事件，而那种丢法是逐帧的、看不出规律。
    m_contacts.clear();

    // ④ 统计行（只在 trace 下）—— 默认关，因为它每帧一行。
    if (m_trace) {
        VK_LOG_INFO("GAME-SCRIPT stats total=%u exec=%u failed=%u skipped=%u "
                    "steps=%llu contacts=%zu",
                    st.total, st.executed, st.failed, st.skipped,
                    static_cast<unsigned long long>(st.steps), m_contactCount);
    }
    m_contactCount = 0;
}

} // namespace vks
