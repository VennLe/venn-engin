#pragma once
// ============================================================
// engine/script/ScriptRuntime —— 脚本的**实例表** + 唯一 tick 内核（S0）
//
// 一句话：`编译产物按路径共享，运行状态按实体独立`。
//   · `ScriptEngine` 管前者（AST / 热重载）—— 它不知道有几个实体在跑；
//   · 本类管后者（viriable 表 / 本地时间 / 熔断）—— 它不知道 HUD 怎么画。
//
// ------------------------------------------------------------
// 为什么必须存在（docs/script_roadmap.md §0）
// ------------------------------------------------------------
// S0 之前 `Evaluator` 的 `m_vars` 是**每次 run() 新建**的 —— 脚本天生失忆：
//   `hp = hp - 10` 每帧都在对 0 做减法。
// 没有"实例"，实体查询、组件读写、事件、协程**四件事全部是沙上建塔**
// （找到了实体也存不住，下一帧就没了）。所以实例是地基，不是第五项功能。
//
// ------------------------------------------------------------
// 同时清掉的四个隐形问题（§1.3）
// ------------------------------------------------------------
//   P2 两份 tick 循环分叉 → 本类是**唯一**的循环，宿主只填 `ScriptServices`；
//   P3 `time = t * timeScale` 改倍速时倒退 → 实例自己累加 `localTime`；
//   P4 运行期错误每帧重试 → `state = Failed` 熔断，直到热重载/Reset；
//   P7 `frame` 是渲染帧号 → 另给 `simFrame`（固定步号）。
//
// ------------------------------------------------------------
// 时间语义（P3 的完整定义，别改）
// ------------------------------------------------------------
//   `tick(sc, simTime, dt, renderFrame)`：
//     · `simTime == kSelfTimed`（默认）→ **仿真态**：`localTime += dt * ts`。
//       这是 Play / Game.exe 走的路。改 `timeScale` 不会让 `time` 跳变，
//       只改变它**前进的速度**（子弹时间要的正是这个）。
//     · `simTime >= 0` → **预览态**：`time = simTime * ts`。
//       编辑器动画时间轴要能拖、能回放，所以必须跟随外部时间，不能自累加。
//
// ⚠ 铁律：**编辑态不跑脚本**（S-D10）。预览态的 `simTime` 只有编辑器动画
//   预览在用，它跑的是 `editorScene()`；Play 态跑 `runtimeScene()`。
// ============================================================

#include "script/ScriptEngine.h"

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace scene {
class Scene;
}

namespace vks {

// ---------------------------------------------------------------- 宿主能力
//
// 宿主（runtime / editor）启动时填一次。**只在宿主侧有实现**，engine 一律
// 不知道 HUD / 音频 / 输入长什么样 —— 这就是"脚本能碰什么"的类型级封闭。
//
// ⚠ 所有回调都可能是**空的**（nullptr）：自检、无头运行、编辑器编辑态都
//   不填。空回调 = 该动作静默无效，**不是崩溃**（`pressed("Jump")` 恒 false）。
struct ScriptServices {
    // ---- HUD（U8）----
    std::function<void(float)> setHealth;
    std::function<void(float)> damage;
    std::function<void(float, float, float, float)> popup;

    // ---- 音频（G14-8）----
    std::function<void(const std::string&)> playSound;
    std::function<void()> stopAllSounds;
    std::function<void(const std::string&, float)> setBusVolume;

    // ---- 玩家 / 物理 / 输入（S2~S3）----
    // 直接复用 `ScriptHost` 里的那块（一次拷贝，见 ScriptRuntime.cpp 的
    // tick）。玩家是"绑定了 CharacterController 的那个实体"这种玩法级
    // 概念 —— runtime 知道，engine 不知道。
    // ⚠ 输入参数是**动作名**（InputMap 里的 "Jump"），不是键码。
    //   脚本里出现 `pressed(32)` 这种写法在本引擎里是错误的抽象层级。
    ScriptWorldActions act;
};

// ---------------------------------------------------------------- 实例
//
// 一个实体挂一个脚本 = 一个实例。字段分成三组：身份 / 跨帧状态 / 执行状态。
// S3 的钩子、S4 的协程栈都往这里填，**不需要再设计一次**。
struct ScriptInstance {
    // 错误分类：面板上"编译错"和"运行期错"要给不同颜色的提示，
    // 日志上的措辞也不一样（`not runnable` vs `runtime error`）。
    enum class ErrorKind { None, Compile, Runtime };

    // ---- 身份 ----
    ecs::Entity entity{};        // 含 generation，防"实体被删后句柄复用"
    std::string path;            // 用哪个脚本（相对资产根）
    bool enabled = true;
    float timeScale = 1.0f;      // 来自 `ecs::ScriptComponent`

    // ---- 跨帧状态（★ P1 的修复）----
    std::unordered_map<std::string, Value> vars;

    // ---- 时间 ----
    double localTime = 0.0;      // ★ P3：自己累加，不用全局 t * ts
    uint64_t simFrames = 0;      // ★ P7：固定步号（严格 +1）

    // ---- 执行状态 ----
    // Failed = 熔断（P4）：本实例后续帧**不再执行**，直到热重载成功 / Reset。
    bool failed = false;
    bool started = false;        // 给 S3 的 `on start` 用；S0 恒 false
    ErrorKind errorKind = ErrorKind::None;
    std::string lastError;       // 熔断原因 / 编译错误（去重上报靠它）
    uint32_t runCount = 0;       // 成功执行过多少步（面板显示）
    uint64_t stepsLastFrame = 0;
    // S4：因热重载结构变化而"整体重启"过几次（面板 / 自检看它）
    uint32_t restartCount = 0;
    // S5：上一帧是不是"在 wait 里空转"。**镜像在实例上**而不是去问协程 ——
    // 面板 / Inspector / 控制台三个地方都要读它，而 ScriptCoroutine 的
    // 布局不对外（在 ScriptRuntime.cpp 里根本看不见）。
    bool waitingNow = false;
    // S5：上一次打进日志的状态。**只在变化时打** —— 面板要"状态列"，
    // 但每帧一行会把日志冲干净。自动化读 `SCR-STATE` 这一行。
    std::string stateLogged; // 上一步消耗的步数（预算诊断）
    // ★ 热重载检测用**版本号**（`ScriptAsset::version()`），不是指针 ——
    //   `clear()` 后重编译可能落在同一地址，指针相等会漏判。
    uint64_t progVersion = 0;    // 0 = 还没成功编译过
    std::vector<std::string> prints;    // 本步 print 输出（每步清空）
    // ★ S4：执行位置（帧栈 + wait 条件）。**每个实例一份** —— 脚本可以
    //   被多个实体共享（编译产物），但它们绝不能共享"执行到哪了"。
    //   用 shared_ptr 只为让头文件只见到前向声明（ScriptCoroutine 的布局
    //   里有 Stmt 指针，不对外）。
    std::shared_ptr<ScriptCoroutine> coro;
};

// ---------------------------------------------------------------- 运行时

// 一次接触（S3）。由宿主在**固定步之前**投递（`physics::ContactTracker` 的
// 结果翻译过来），tick 时派发给**双方各自的实例**。
//
// ⚠ 为什么不由 engine 直接读物理：`engine/script` 一旦 include
//   `physics/Contact.h` 就得跟着物理的生命周期走（编辑器里物理只在 Play 态
//   存在）。投递是纯数据，没有这层耦合。
struct ScriptContact {
    enum class Kind { Enter, Stay, Exit };
    Kind kind = Kind::Enter;
    ecs::Entity a{};
    ecs::Entity b{};
    bool trigger = false;  // 是触发体积（has TriggerComponent）而不是硬碰撞
};

class ScriptRuntime {
public:
    // 仿真态的时间哨兵（见文件头"时间语义"）
    static constexpr double kSelfTimed = -1.0;

    void setServices(ScriptServices s) { m_services = std::move(s); }
    const ScriptServices& services() const { return m_services; }

    // 清空实例表（进 Play / 换场景 / Stop）。下一步 tick 会按场景重建。
    void reset();

    // 唯一的 tick 内核。
    //   simTime 见文件头；`renderFrame` 是渲染循环帧号（老脚本的 `frame`）。
    void tick(scene::Scene& sc, double simTime, float dt, uint64_t renderFrame);

    // ---- 接触事件（S3）----
    // 宿主每个固定步投递一次，tick 末尾清空 —— 投递了没用上也不会积压。
    void postContact(const ScriptContact& c) {
        m_contacts.push_back(c);
        m_contactCount = m_contacts.size();
    }
    size_t pendingContacts() const { return m_contacts.size(); }

    // 只同步实例表、**不执行**。
    // 用途：① 自检在第一次 tick 之前注入初始变量；② S5 的 Instances 面板
    // 在编辑态枚举实例（那时一个脚本都不许跑，S-D10）。
    void sync(scene::Scene& sc) { syncInstances(sc); }

    // 显式给某个实例一次"从头再来"（面板 Reset 按钮 / 控制台 script.reset）
    bool resetInstance(ecs::Entity e);

    ScriptInstance* find(ecs::Entity e);
    const ScriptInstance* find(ecs::Entity e) const;

    const std::vector<std::unique_ptr<ScriptInstance>>& instances() const {
        return m_instances;
    }
    uint32_t instanceCount() const {
        return static_cast<uint32_t>(m_instances.size());
    }

    // 实例状态串（S5 面板 / Inspector 用）：Idle | Running | Waiting | Failed。
    // ⚠ 定义在 .cpp —— 它要问协程"是不是在等"，而那个类型不对外。
    const char* stateOf(const ScriptInstance& inst) const;

    // 变量表 → 按名字排序的 (键, 值) 列表。面板与 `script.vars` 共用。
    // ⚠ 排序是必须的：控制台键值行若按哈希序，自动化断言会随机红。
    std::vector<std::pair<std::string, std::string>> sortedVars(
        const ScriptInstance& inst) const;

    // 上一步的统计（日志汇总 / Instances 面板 / 基准都读它）
    struct Stats {
        uint32_t total = 0;      // 场景里有几个 ScriptComponent（含 disabled）
        uint32_t executed = 0;   // 本步真的跑了几个
        uint32_t failed = 0;     // 本步因为熔断/编译错被跳过的
        uint32_t skipped = 0;    // disabled / 空路径
        uint64_t steps = 0;      // 本步总步数
    };
    const Stats& lastStats() const { return m_stats; }

    // 面板 / 自检要用资产列表与热重载（路径级共享的那一半）
    ScriptEngine& engine() { return m_engine; }
    const ScriptEngine& engine() const { return m_engine; }

    // `MYVK_VKS_TRACE` 也能开（构造时读一次）。控制台 `script.trace on` 走这里。
    void setTrace(bool on) { m_trace = on; }
    bool trace() const { return m_trace; }

    void setPrintSink(
        std::function<void(const ScriptInstance&, const std::string&)> f) {
        m_printSink = std::move(f);
    }
    // ⚠ 只在错误内容**变化**时调一次（P4：不要每帧刷屏）
    void setErrorSink(
        std::function<void(const ScriptInstance&, const std::string&)> f) {
        m_errorSink = std::move(f);
    }

private:
    void syncInstances(scene::Scene& sc);
    void reportError(ScriptInstance& inst, const std::string& msg,
                     ScriptInstance::ErrorKind kind);

    ScriptEngine m_engine;
    // 本步待派发的接触事件（tick 末尾清空）
    std::vector<ScriptContact> m_contacts;
    // 上一步派发了多少条（只给 trace 统计行用）
    size_t m_contactCount = 0;
    // `MYVK_VKS_TRACE=1` 时每步打一行 `GAME-SCRIPT stats ...`
    bool m_trace = std::getenv("MYVK_VKS_TRACE") != nullptr;
    ScriptServices m_services;
    std::vector<std::unique_ptr<ScriptInstance>> m_instances;
    std::unordered_map<unsigned, std::size_t> m_index;  // entity.id → 下标
    Stats m_stats;

    // 场景变了（编辑态 ↔ 运行态副本 / 换关卡）→ 实例表整体重建。
    // ⚠ 不重建的话，两个场景里 id 相同的实体会**互相串状态** —— 这是
    //   最难查的一类 bug（看起来"脚本状态莫名其妙"）。
    const scene::Scene* m_scene = nullptr;

    double m_reloadAccum = 0.0;  // 热重载轮询节流
    bool m_syncedOnce = false;

    std::function<void(const ScriptInstance&, const std::string&)> m_printSink;
    std::function<void(const ScriptInstance&, const std::string&)> m_errorSink;
};

} // namespace vks
