#pragma once
// ============================================================
// script/ApiTable —— 游戏逻辑 API 的**唯一事实源**
//
// 一句话：**名字、参数个数、签名、文档，只有这一份**。
//
// ------------------------------------------------------------
// 为什么要有这张表（docs/script_framework.md §8）
// ------------------------------------------------------------
// `.vks` 时代加一个内建函数要"同步改三处"（enum / kBuiltins / switch），
// 而 `kBuiltins[]` 是**按位置**对应 enum 的 —— 中间插一条、末尾补一条，
// 从那一位起的所有内建整体错位一格，`getAttr` 会跑到 `setTimeScale` 的
// 分支里执行，而 `static_assert` 只比长度、拦不住。
//
// 表驱动之后：
//   · Lua 绑定的**注册**从表生成      → 名字与实现在结构上不可能错位
//   · API 文档**从表生成**            → 文档不会落后于代码
//   · 参数个数检查从表生成            → 签名一处改，全部生效
//
// ------------------------------------------------------------
// 分层：本文件**不认识 Lua**
// ------------------------------------------------------------
// 这里只有 `const char*` 和整数 —— 不 include `lua.hpp`。
// `LuaBackend.cpp` 负责把 (ns, name) 映射到 `lua_CFunction`；
// 映射缺一条 → `installBindings()` **当场报错并列出名字**，不允许
// "表里有、实际调不到"。这是表驱动真正的收益：**不可能漏**。
// ============================================================

#include <cstdint>

namespace vks {

// 一条 API。`minArgs` / `maxArgs` 数的是**命名空间之后**的实参个数
// （`world.setPos(e, v)` = 2）。
//
// ⚠ `maxArgs == kVarArgs` 表示可变参数（只有 `engine.log`）。
constexpr uint8_t kVarArgs = 255;

struct ApiEntry {
    const char* ns;       // "engine" / "world" / ...
    const char* name;     // 表内字段名，不带命名空间前缀
    uint8_t minArgs;
    uint8_t maxArgs;
    const char* sig;      // "entity, vec3 -> void"
    const char* doc;      // 一句话说明
    // 可选：会咬人的地方。空串 = 无。写在这里而不是注释里，是因为
    // 它要**出现在文档里**（开发者查的就是文档，不是源码）。
    const char* note;
};

// 全部条目：顺序即注册顺序、亦即文档顺序（按命名空间分组）
const ApiEntry* apiEntries();
int apiEntryCount();

} // namespace vks
