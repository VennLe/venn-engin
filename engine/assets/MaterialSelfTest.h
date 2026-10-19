#pragma once
// ============================================================
// assets/MaterialSelfTest —— 材质实例语义的无窗口自检（W1-5）
//
// 为什么需要它：`makeMaterialPBR` 的同名冲突、实例的"跟随/不跟随"、
// 位掩码的字段序，这三件事全都有"看起来对、其实错"的失败模式，而它们
// 都藏在**编辑器里点一下**才能发现的位置。把它们抽成纯数据断言以后，
// `Game.exe --selftest material` 在**没有显卡、没有窗口、没有场景**的
// 机器上也能跑，而且一秒钟出结果。
//
// ⚠ 这里刻意**不构造 AssetManager**：它的构造函数要 `TextureContext`
//   （真设备）。所以测的是 Material 上的纯静态函数，而不是管理器方法 ——
//   `AssetManager::materialChanged` 只是"遍历 + 调这个函数"。
// ============================================================

namespace assets {

// 返回 0 = 全过，非 0 = 失败条数（与其它 selftest 同一个口径）
int runMaterialSelfTest();

} // namespace assets
