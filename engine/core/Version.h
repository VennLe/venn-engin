#pragma once
// ============================================================
// core/Version —— 引擎版本号（唯一事实源）
//
// ⚠ 2026-10-19（启动器 M1）：原本是 `editor/EditorApp.cpp` 里的一个
//    `constexpr`（局部于那个 TU）。启动器也要在标题栏显示版本，而
//    `launcher/` 与 `editor/` 是**平行的两个应用层**，互相不许依赖 ——
//    两边各写一份 "0.2.0" 就是又一次"两份数据漂移"（改版本时必漏一处，
//    漏了也看不出来）。
//
// 与 `CMakeLists.txt` 的 `project(Venn VERSION x.y.z)` 对齐 ——
// CMake 那边没有把版本编进代码，所以这里是**唯一**的事实源；
// 改版本时两处一起改。
// ============================================================

namespace core {

constexpr const char* kVennVersion = "0.2.0";

} // namespace core
