#pragma once
// ============================================================
// script/ScriptBackend —— 脚本后端的**语言无关契约**
//
// ------------------------------------------------------------
// 为什么需要这一层（一句话）
// ------------------------------------------------------------
// 现在 `ScriptRuntime` 直接持有 `ScriptProgram`（`.vks` 的 AST），于是
// "换一种脚本语言" = "重写 ScriptRuntime 605 行"。这层接口把
// **「什么时候跑」和「怎么跑」分开**：
//
//   ScriptRuntime（唯一 tick 内核）      后端（可插拔）
//     ├ 实例表 / 时间 / 事件派发           ├ VksBackend（现役，包 ScriptProgram）
//     ├ 预算 / 熔断记账                    └ LuaBackend（新增，包 lua_State）
//     └ 面板要读的状态
//
// 接完之后，新语言 = 新增一个 `IScriptBackend` 实现，**不改 ScriptRuntime**。
//
// ------------------------------------------------------------
// 三条纪律（写在这里为了不重复解释）
// ------------------------------------------------------------
// ① **编译产物按路径共享，运行状态按实体独立**（与 ScriptRuntime 同一条）。
//    `ICompiledScript` 是前者，`IScriptInstance` 是后者 —— 一个脚本被 50 个
//    实体挂上时，只编译一次、只占一份内存。
//
// ② **接口里不出现 `Value`**。`Value` 是 `.vks` 的值模型（在 ScriptEngine.h），
//    Lua 用的是 Lua 栈。接口只要"跑"和"报错"，值怎么表达是后端自己的事。
//
// ③ **`tick` 返回 false = 本实例已熔断**。后端负责**在内部**停下，不要抛异常
//    穿过这层 —— 熔断的语义是"这一个实例不再执行"，不是"整个宿主停摆"。
// ============================================================

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ecs/Entity.h"

namespace vks {

struct ScriptHost;

// 定义在 ScriptEngine.h（`enum class HookKind : uint8_t`）。这里只前向声明，
// 避免把 823 行的 ScriptEngine.h 拖进每一个包含本头文件的翻译单元。
enum class HookKind : uint8_t;

// ---------------------------------------------------------------- 编译产物
//
// 一个脚本文件编译一次，被 N 个实例共享。**热重载时换的是它**（version 变）。
class ICompiledScript {
public:
    virtual ~ICompiledScript() = default;

    // 相对资产根的路径（`scripts/enemy_patrol.lua`）。面板与日志用。
    virtual const std::string& path() const = 0;

    // 编译成功了吗。false 时 `error()` 是原因（带行号），
    // ⚠ 编译失败**不等于**运行失败：热重载失败时旧版本继续跑。
    virtual bool ok() const = 0;
    virtual const std::string& error() const = 0;

    // 这个脚本实现了哪个钩子。没实现的钩子，宿主**一次都不该调**
    // （等价 UE 的 BlueprintImplementableEvent 没实现 → 不触发）。
    virtual bool hasHook(HookKind h) const = 0;

    // 每次成功编译 +1。热重载检测用**版本号**而不是指针 ——
    // 重编译可能落在同一地址，指针相等会漏判（`.vks` 踩过）。
    virtual uint64_t version() const = 0;
};

// ---------------------------------------------------------------- 运行实例
//
// 一个实体挂一个脚本 = 一个实例。字段：身份 / 跨帧状态 / 执行状态，
// 由**后端自己**定义（`.vks` 用 `vars`，Lua 用环境表）。
class IScriptInstance {
public:
    virtual ~IScriptInstance() = default;

    // 首次 tick 之前一次（`.vks` 的 `on start` / Lua 的 `BeginPlay`）。
    // 返回 false = 出错（已写进 lastError），宿主应把它当熔断处理。
    virtual bool beginPlay(ScriptHost& host) = 0;

    // 每个固定步一次（`on update` / `Tick(dt)`）。
    virtual bool tick(ScriptHost& host, float dt) = 0;

    // 接触事件（`on collide` / `OnCollide(o)`）。`other` 是对面那个实体。
    virtual bool fireHook(ScriptHost& host, HookKind h, ecs::Entity other) = 0;

    // 协程唤醒。宿主在 `tick` 之后、事件之前调一次。
    // 返回 true = 这一帧被唤醒并推进了（或本来就没事可做）。
    virtual bool resume(ScriptHost& host, float dt) = 0;

    // 还在 wait 里等吗（面板显示 "Waiting"）。
    virtual bool waiting() const = 0;

    // 熔断了吗。true 之后宿主**不再调 tick/fireHook**，直到 reload 成功或 reset。
    virtual bool failed() const = 0;
    virtual const std::string& lastError() const = 0;

    // 热重载：把编译产物换掉。**跨帧状态必须保留**（`.vks` 靠 Stmt::id 对账，
    // Lua 靠"换 metatable 的 __index"）。返回 false = 结构变太多，本实例已重启。
    virtual bool reload(std::shared_ptr<ICompiledScript> next) = 0;

    // 面板 / 控制台要看的变量。**按名字排序**（面板不能跳来跳去）。
    // 值转成字符串由后端决定（`.vks` 用 `Value::toStr()`，Lua 用 `tostring`）。
    virtual std::vector<std::pair<std::string, std::string>> snapshotVars() const = 0;
};

// ---------------------------------------------------------------- 后端
class IScriptBackend {
public:
    virtual ~IScriptBackend() = default;

    // "vks" / "lua" —— 日志与面板用。
    virtual const char* name() const = 0;

    // 这个后端接不接这个文件（按扩展名）。`.vks` → "vks"，`.lua` → "lua"。
    // ⚠ 判据是**扩展名**，不是内容嗅探：一份写错的脚本不该靠猜决定谁来跑。
    virtual bool accepts(const std::string& path) const = 0;

    // 编译。**不抛**：失败写进 `ok()/error()`。
    virtual std::shared_ptr<ICompiledScript> compile(const std::string& path,
                                                     const std::string& source) = 0;

    // 建实例。`prog->ok()` 必须为真（宿主负责检查）。
    virtual std::unique_ptr<IScriptInstance> createInstance(
        std::shared_ptr<ICompiledScript> prog, ecs::Entity e) = 0;
};

} // namespace vks
