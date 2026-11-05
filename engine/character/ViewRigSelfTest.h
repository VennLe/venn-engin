#pragma once
// ============================================================
// character/ViewRigSelfTest —— 无窗口的第一/第三人称装配自检
//
// 运行方式：`MYVK_VIEWRIG_TEST=1 ./Editor.exe` —— 与 `MYVK_ANIM_TEST`
// 同一条路：**不初始化 Vulkan、不开窗口**，`editor/main.cpp` 在
// `app.run()` 之前就 return。
//
// 为什么需要它：这套东西（眼位 / 阻尼 / 过渡 / 可见性）全部作用在
// **相机那一侧**，而相机的问题在截图里表现成"看起来有点怪"，在日志里
// 一个数都没有。历史上的两个真 bug 都是这一类：
//   · 第一人称相机落在眼前 2 m（数学写反，画面"能用"）；
//   · 运行时眼位在头顶上方 0.56 m（常数按错误的参照系折算）。
// 所以这里把"眼位 = 头骨的世界位置"、"过渡首尾精确"、"帧率无关"直接
// 钉成数值断言。
//
// 输出格式（供 `tools/verify_view_rig.py` 解析）：
//   VIEWRIG-TEST <名字> actual=<值> expected=<值> tol=<值> result=PASS|FAIL
//   VIEWRIG-TOTAL pass=<n> fail=<m>
// ============================================================

namespace character {

// 跑完全部自检，返回失败项数（0 = 全过）。
int runViewRigSelfTest();

} // namespace character
