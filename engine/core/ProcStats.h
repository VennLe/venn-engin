#pragma once

namespace core {

// 进程级内存读数（Stats 面板的"内存"一节）。
//
// 只有三个数，因为它们各自回答一个不同的问题：
//   · workingSet —— 任务管理器里那一列。涨了要说清"涨在哪"。
//   · peak       —— 峰值。它比当前值有用得多：一次性的巨大分配
//                   （重建交换链 / 载入大场景）在瞬时值上抓不到。
//   · privateB   —— 私有提交。它才是"我们真的占了这么多"，
//                   工作集可以被系统随时回收，掉下去不代表我们释放了。
struct ProcMemory {
    double workingSetMB = 0.0;
    double peakWorkingSetMB = 0.0;
    double privateMB = 0.0;
    bool valid = false;   // 取不到时全 0 + valid=false（面板显示 "n/a"）
};

ProcMemory queryProcessMemory();

} // namespace core
