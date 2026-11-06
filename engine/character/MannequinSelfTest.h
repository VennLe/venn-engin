#pragma once
// ============================================================
// character/MannequinSelfTest —— UE5 Mannequin 骨架与重定向的自检
//
// 入口：`MYVK_MANNEQUIN_TEST=1 Editor.exe`（Vulkan 之前返回，纯 CPU）
// 输出：`MQ-TEST <name> actual=.. expected=.. tol=.. detail=.. result=PASS|FAIL`
// 汇总：`MQ-TOTAL pass=N fail=M`
// ============================================================

namespace character {

// 返回失败条数（0 = 全过）。
int runMannequinSelfTest();

} // namespace character
