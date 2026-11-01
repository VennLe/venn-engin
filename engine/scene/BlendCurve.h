#pragma once
// ============================================================
// scene/BlendCurve —— 切镜混合的**形状**（纯数学，零依赖）
//
// 多机位 marker 的 `blendSeconds` 只说"混多久"，不说"中间怎么走"。
// 这个头补的就是后者：同样是 0.6 秒，`Linear` 是匀速推过去、`EaseOut`
// 是"猛地起来再缓缓贴上去"、`Smoother` 两头都更软 —— 三种在画面上是
// 三个不同的运镜，而在此之前它们全被写死成了 `Smooth`。
//
// ------------------------------------------------------------
// 为什么这个头**一个 include 都没有**
// ------------------------------------------------------------
// `sequence/SequenceAsset.h` 要把它当字段类型（数据层），
// `scene/CameraSystem.h` 要拿它去执行（执行层）。而 SequenceAsset.h 的
// 文件头明确写了"不能 include scene/CameraSystem.h，否则成环"。
// 所以这里只放**纯函数 + 枚举**，既不认识 Camera 也不认识 Scene ——
// 两边谁 include 它都不会把对方的依赖拖进来。
//
// ⚠⚠ 默认值必须是 `Smooth`，且 `Smooth` 必须**逐位等于**改动前那句
//    `t*t*(3-2t)`。老资产没有 `blendCurve` 键，读出来就是 Smooth；只要
//    这两者一致，"加了曲线功能"对既有过场就是零影响。
namespace scene {

enum class BlendCurve : int {
    Linear = 0,     // 匀速。剪辑里"机械但可预期"，快速甩镜常用
    Smooth = 1,     // smoothstep t²(3−2t)。**默认 = 加本功能之前的行为**
    EaseIn = 2,     // t²：起步慢、撞上去快（"最后一下猛地到位"）
    EaseOut = 3,    // 1−(1−t)²：起步快、尾段慢慢贴上去
    Smoother = 4,   // t³(6t²−15t+10)：两端一阶、二阶导都为 0，最软
};

// t ∈ [0,1] → 混合权重。**保证 f(0)=0、f(1)=1**（两端不许有台阶，
// 否则混合开始/结束那一帧会跳一下 —— 这正是"混合看着像抽搐"的成因）。
float blendCurveApply(BlendCurve c, float t);

// 存档用的关键字（小写，写进 JSON 的 `blendCurve` 字段）。
// ⚠ 这是**格式关键字**，不是界面文案：翻成中文就再也读不回来了
//   （与 `sequence::easeName` 同一条纪律）。界面显示也直接用它。
const char* blendCurveName(BlendCurve c);

// 关键字 → 枚举。认不出返回 false（调用方保持默认，并可以记警告）。
bool blendCurveFromName(const char* s, BlendCurve& out);

} // namespace scene
