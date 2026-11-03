#pragma once
// ============================================================
// script/LuaBackend —— Lua 5.4 脚本后端
//
// 与 `VksBackend`（`.vks` 自研 VM）并列，选择依据是**脚本文件扩展名**。
// 上层只认 `IScriptBackend`，不知道底下是 Lua 还是树遍历 VM。
//
// ------------------------------------------------------------
// 四条实现决策（都是踩过才这么定的，别随手改）
// ------------------------------------------------------------
// ① **全局一个 `lua_State`**，不是每实体一个。
//      · 内存：一个空 state 约 20~30 KB，千实体就是 20~30 MB；
//      · 更要紧的是**跨实体通信**：A 打 B 是最常见的玩法，两个 VM 之间
//        只能靠序列化。
//    代价是"一个死循环冻住全场"→ 所以有 ②。
//
// ② **`lua_sethook` 指令预算**。每 `kHookCount` 条 VM 指令查一次，超了
//    `luaL_error`。没有它，`while true do end` 会把整个游戏冻住 ——
//    `.vks` 的"3 帧内熔断、只停一个实例"这条纪律就丢了。
//
// ③ **实例 = 一张 env 表 + metatable**。
//      env  = { entity = <句柄> }      ← 实例字段住这里
//      meta = { __index = <模板表> }   ← 方法住模板表
//    热重载只换 `__index` 指向的模板表 → **字段不动、方法立刻更新**。
//    这比"把模板表浅拷贝一份"省内存，也比"重建实例"保得住状态。
//
// ④ **库白名单**。不调 `luaL_openlibs`。只开 base / coroutine / table /
//    string / math / utf8，**不开 io / os / debug / package**。
//    `math.random` / `math.randomseed` 要摘掉（确定性走 `game.random`）。
//    `os.clock` / `os.time` 连库都不存在，脚本拿不到"墙上时间"。
// ============================================================

#include <string>

#include "script/ScriptBackend.h"

struct lua_State;

namespace vks {

class LuaBackend : public IScriptBackend {
public:
    // `stepsPerFrame`：单次 resume 的指令预算（默认 20000，与 `.vks` 同量级）。
    // 环境变量 `MYVK_LUA_BUDGET` 可覆盖（验收要能把它调小来测熔断）。
    explicit LuaBackend(int stepsPerFrame = 20000);
    ~LuaBackend() override;

    LuaBackend(const LuaBackend&) = delete;
    LuaBackend& operator=(const LuaBackend&) = delete;

    const char* name() const override { return "lua"; }
    bool accepts(const std::string& path) const override;

    std::shared_ptr<ICompiledScript> compile(const std::string& path,
                                             const std::string& source) override;
    std::unique_ptr<IScriptInstance> createInstance(std::shared_ptr<ICompiledScript> prog,
                                                    ecs::Entity e) override;

    // ---- 实例与绑定层要用的 ----
    lua_State* state() const { return m_L; }

    // ---- 外部（C++ 游戏模块）函数 ----
    //
    // 把 `game/GameModule` 收集到的函数挂成 Lua 的 `ns.name`。签名与
    // `game/VennAbi.h` 的 `VennFunc` **逐字相同**（参数/返回值都是 double）。
    //
    // ⚠ 这是 M5-c 留给阶段 D 的接口：D 把 Lua 后端接进 `ScriptRuntime` 时，
    //   这里的函数才会真正出现在脚本里。现在接进来只是把链路补齐 ——
    //   提前留空接口而不实现，会让"文档说有、代码没有"。
    //
    // ⚠ 与引擎自带命名空间**撞名时游戏模块优先**并打一行 WARN：覆盖
    //   引擎 API 是危险动作，但不该静默失败。
    void addExternalFunction(const std::string& ns, const std::string& name,
                             int (*fn)(const double*, int, double*, int));

    // 当前这一步的宿主（每 tick 由实例设回 nullptr）。绑定函数靠它拿 scene。
    ScriptHost* host() const { return m_host; }
    void setHost(ScriptHost* h) { m_host = h; }
    int stepsPerFrame() const { return m_stepsPerFrame; }

    // `engine.wait()` 把"要等多久"寄存在这里，实例 resume 时取走。
    // 正数 = 秒；负数 = 帧数（`waitFrames`）。取走后自动清零。
    void setPendingWait(double v) { m_pendingWait = v; }
    double takePendingWait() {
        const double v = m_pendingWait;
        m_pendingWait = 0.0;
        return v;
    }

    // `engine.waitUntil(fn)` 把条件函数丢在 registry 里，ref 寄存在这里，
    // 由实例**每帧求值**（它自己不会在 Lua 侧轮询 —— 那会把每帧的预算
    // 全烧在一次 wait 上）。
    void setPendingUntil(int ref) {
        m_pendingUntil = ref;
        m_hasPendingUntil = true;
    }
    // 返回 -1 = 这次 yield 不是 waitUntil 引起的。
    int takePendingUntil() {
        if (!m_hasPendingUntil) return -1;
        m_hasPendingUntil = false;
        const int r = m_pendingUntil;
        m_pendingUntil = -1;
        return r;
    }

private:
    void installBindings();
    lua_State* m_L = nullptr;
    ScriptHost* m_host = nullptr;
    int m_stepsPerFrame = 20000;
    double m_pendingWait = 0.0;
    int m_pendingUntil = -1;
    bool m_hasPendingUntil = false;
    uint64_t m_version = 0;
};

} // namespace vks
