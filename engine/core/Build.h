#pragma once
// ============================================================
// core/Build —— 构建风味（dev / shipping）的**唯一事实源**
//
// ------------------------------------------------------------
// 为什么要单独一个头
// ------------------------------------------------------------
// "这一份二进制是不是出货版"会决定七八处行为：要不要开校验层、要不要
// 认 `--selftest` / `--package` 这类开发命令、标题栏要不要挂 git 号……
// 如果每处各写一份判定（`#ifdef NDEBUG`、`#ifdef _DEBUG`、读某个 env），
// 那么它们在**同一份二进制里**必然出现不一致，而且不一致的方向是
// "以为关掉了、其实还开着" —— 出货版带着校验层和断言框，是最坏的一类。
//
// 所以规则只有一条：**任何地方不许自己猜风味，一律看这里。**
//
// ------------------------------------------------------------
// 两种味
// ------------------------------------------------------------
//   dev（默认，QA 版也是它）
//     · 校验层可用（`MYVK_VALIDATION=0/1` 能开关）
//     · `--selftest` / `--package` 等开发命令存在
//     · `MYVK_FIXED_DT` / `MYVK_PHYS_DUMP` / `MYVK_RUNTIME_TRACE_STATES` 生效
//     · 标题栏与原行为**逐字节一致**（既有回归用例都靠它找窗口）
//
//   shipping（`-DVENN_SHIPPING=ON`）
//     · 校验层**强制关**（不装层的玩家机器上开它只会多一次失败查询）
//     · 开发命令在**编译期**就不存在（字符串都不进二进制 —— 可被
//       `tools/verify_shipping.py` 按字节比对证明）
//     · 标题栏追加 `[<git>]`（现场排查时这是唯一线索）
//
// ------------------------------------------------------------
// 为什么 git 号是编译期常量
// ------------------------------------------------------------
// 由 CMake 在 configure 时 `git describe --tags --always --dirty` 取得，
// 以 `-DVENN_GIT_DESCRIBE="..."` 的形式编进二进制。
//
// ⚠ **它是 configure 时刻的快照**，不是链接时刻。只 `build` 不 `configure`
//   的话，git 号会停在最后一次 configure 上。重新 configure 一次即可
//   （`make configure`）。这里不搞"每次构建重跑 git"是因为那会让
//   **所有 .cpp 每次都过期**（一个宏变了等于全量重编），代价远大于收益。
// ============================================================

#include "core/Version.h"

#include <cstdio>
#include <string>

#ifndef VENN_SHIPPING
// 未定义时按 dev 处理（手工调 cl.exe 编单个文件时的兜底）。
#define VENN_SHIPPING 0
#endif

#ifndef VENN_GIT_DESCRIBE
#define VENN_GIT_DESCRIBE "unknown"
#endif

namespace core {

// true = 出货构建。**编译期常量**：`if (kShipping)` 里的分支在
// shipping 下被编译器整段消掉，这也正是"开发字符串不进二进制"的前提。
constexpr bool kShipping = (VENN_SHIPPING != 0);

constexpr const char* kBuildFlavor = kShipping ? "shipping" : "dev";
constexpr const char* kGitDescribe = VENN_GIT_DESCRIBE;

// 开发钩子（自检 / 出包子命令 / 固定步长 / 物理转储…）是否**存在**。
// 注意语义是"这段代码在不在二进制里"，不是"现在开没开"：
// 出货版里它恒为 false，且相关的命令行分支根本不存在。
constexpr bool devHooksEnabled() { return !kShipping; }

// 一行式构建标识：`venn 0.2.0 shipping git=v0.2.0-3-g1a2b3c4-dirty`
// 日志 / `--version` / 标题栏共用同一份格式 —— 三处各拼一次就一定会漂。
inline const char* buildStamp() {
    static const std::string s = [] {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "venn %s %s git=%s", kVennVersion,
                      kBuildFlavor, kGitDescribe);
        return std::string(buf);
    }();
    return s.c_str();
}

} // namespace core
