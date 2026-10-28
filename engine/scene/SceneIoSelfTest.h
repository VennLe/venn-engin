#pragma once
// ============================================================
// scene/SceneIoSelfTest —— G16-5 三段拆分的自检（`Game.exe --selftest sceneio`）
//
// 验收标准（`docs/persistence_roadmap.md` §12 G16-5 那一行）：
//   * `verify_runtime_m2` C 组绿（读→写→再读）
//   * **`--selftest sceneio` 能无设备跑**
//
// 第二条是这个文件存在的理由：三段拆分本身**不是**重构洁癖，它是
// "读档能自检"的前提。所以这里的断言必须能在 `device == nullptr` 下跑完，
// 而且 ①② 两段**根本不接受** AssetManager（签名即判据）。
//
// 返回**失败条数**（0 = 全绿）。
// ============================================================

namespace scene {

int runSceneIoSelfTest();

} // namespace scene
