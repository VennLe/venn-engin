#pragma once
// ============================================================
// animation/AnimationSelfTest —— 无窗口的骨骼动画自检
//
// 为什么要有它：这条链在过去**一个自动化用例都没有**，根因是采样埋在
// `Renderer::updateSkinning` 里 —— 任何动画逻辑都必须开 GUI + 有 GPU 才能
// 验证。A0-5 把采样剥到 `engine/animation/` 之后，这项自检才能存在。
//
// 运行方式：`MYVK_ANIM_TEST=1 ./Editor.exe` —— **不初始化 Vulkan、不开窗口**，
// `editor/main.cpp` 在 `app.run()` 之前就 return（与 `MYVK_PHYS_TEST` 同一条路）。
//
// ⚠ 它不加载任何资产（`assets::Mesh` 是 GPU 资源），骨架与 clip 是代码里
//   手工搭的 —— 这反而是优点：数值期望可以手算。
//
// 输出格式（供 `tools/verify_anim_runtime.py` 解析）：
//   ANIM-TEST <名字> actual=<值> expected=<值> tol=<值> result=PASS|FAIL
//   ANIM-TOTAL pass=<n> fail=<m>
// ============================================================

namespace animation {

// 跑完全部自检，返回失败项数（0 = 全过）。
int runAnimationSelfTest();

} // namespace animation
