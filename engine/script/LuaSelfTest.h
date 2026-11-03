#pragma once
// ============================================================
// script/LuaSelfTest —— Lua 后端的无窗口自检
//
// 为什么单独一个文件（而不是塞进 ScriptSelfTest.cpp）：
//   `.vks` 的自检用的是 MockHost + 真 Scene；Lua 后端额外要验证
//   **VM 层面的四件事**（白名单库、协程、指令预算、句柄打包），
//   它们和语言语义无关，混在一起会让"改了一条 `.vks` 断言"和
//   "Lua 的 hook 没生效"在同一个红灯里分不开。
//
// 运行：`Game.exe --selftest lua`（不开窗口、不需要 GPU）
// 输出：`LUA-T pass <名字> <细节>` / `LUA-TOTAL pass= fail=`
// ============================================================

namespace vks {

// 返回失败项数（0 = 全过）。
int runLuaSelfTest();

// 把 `ApiTable` 原样打成 markdown，写到 stdout（`Game.exe --selftest api`）。
// 文档的唯一来源是那张表 —— 这个函数只是"表 → 人读的形式"。
void dumpApiMarkdown();

} // namespace vks
