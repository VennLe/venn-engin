#pragma once
// ============================================================
// animation/Curve —— F-Curve（函数曲线）核心
//
// 这是**动画系统里"值怎么随时间变"的唯一表述**。在做它之前，全仓有**三份**
// 互不相干的插值实现：
//   · `assets::sampleTrack`（`Skeleton.cpp`）—— STEP + 线性 + slerp，没有缓动；
//   · `AnimationEditor::easeKey`（`ToolkitEditors.cpp`）—— 硬编码 smoothstep；
//   · `sequence::sampleCamera`（`SequenceAsset.cpp`）—— Linear / Smooth / Step。
// 三份都能算"中间那帧"，但"缓动"这件事被写了三遍、能力各不相同，而且
// **一个都说不出"这条曲线第 3 个关键帧的手柄朝哪"** —— 没有手柄就没有
// 图编辑器，没有图编辑器就永远只能拨三个预设。所以先把曲线抽出来。
//
// ------------------------------------------------------------
// 三个必须分清的概念（Blender 里也是三个独立字段）
// ------------------------------------------------------------
//   ① **Interp**（插值）：两个关键帧**之间**怎么连 —— 常量 / 线性 / 贝塞尔。
//   ② **Ease**（缓动）：时间重映射 `u → f(u)`，只对**非贝塞尔**有意义
//      （贝塞尔的形状已经由手柄决定了，再叠一层缓动等于改两次）。
//   ③ **Handle**（手柄）：贝塞尔段的两根切线，决定"进段/出段"的速度。
//
// ⚠⚠ 为什么 `Ease` 在贝塞尔上会被忽略：写代码时最容易"顺手都乘上"，
//   结果是设了手柄还要再被 smoothstep 压一次，曲线永远到不了手柄指的
//   位置 —— 作者会觉得"手柄失灵"。`evaluateCurve` 里这两条是互斥分支。
//
// ------------------------------------------------------------
// 手柄为什么存**相对偏移**而不是绝对坐标
// ------------------------------------------------------------
// Blender 存绝对 (frame, value)。这里存 `{dt, dv}` 相对本帧，两个好处：
//   · 关键帧时间被拖走时，手柄自动跟着走（不用事后修补）；
//   · "自动手柄"的算法天然是"斜率 × 段长/3"，本来就是相对量。
// 代价是"对齐手柄（Aligned）"要在求值时现算方向 —— 可忽略。
//
// ⚠ **x 分量必须保号**：`outHandle.x >= 0`、`inHandle.x <= 0`。贝塞尔求值
//   要在 x 上解 u（三次方程），x 一旦非单调就无解或解出多个，表现是
//   "曲线在某一帧突然跳到别处"。`resolve` 会把它钳回去，而不是相信输入。
// ============================================================

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace animation {

// 两帧之间怎么连。
enum class Interp : int {
    Constant = 0,   // 保持前一帧，到下一帧瞬跳（开关、表情切换、硬切镜头）
    Linear = 1,     // 匀速
    Bezier = 2,     // 三次贝塞尔，形状由手柄决定（默认）
};

// 时间重映射的**方向**（Blender 的 easing 下拉：Ease In / Ease Out / ...）
enum class EaseMode : int {
    None = 0,   // 不重映射（u 原样用）
    In = 1,     // 起步慢
    Out = 2,    // 收尾慢
    InOut = 3,  // 两头慢（= 过去那个 smoothstep 的意图）
};

// 时间重映射的**形状**。这是 Blender 那张 "Sine / Quad / Cubic / Bounce /
// Elastic / Back" 表的对应物；`Linear` + `EaseMode::InOut` 恰好等于老代码
// 里的 `easeKey()`（smoothstep）—— 迁移就是靠这一条对齐的。
enum class EaseShape : int {
    Linear = 0,
    Sine = 1,
    Quad = 2,
    Cubic = 3,
    Back = 4,     // 回拉过冲（"预备动作"）
    Bounce = 5,
    Elastic = 6,  // 果冻感
    // Hermite smoothstep。**`InOut` 特判成它本身**，逐位等于 R4 时期
    // `sequence::smoothstep01()` —— 见 `easeFn()` 里那段"迁移锚"。
    Smooth = 7,
};

// 手柄类型（Blender 按 V 键切的那几个）。
enum class HandleType : int {
    Auto = 0,        // 按左右邻居的**中心差分**算斜率，可能过冲
    AutoClamped = 1, // 同上，但局部极值处压平 —— 不会过冲（默认）
    Vector = 2,      // 手柄指向相邻关键帧 → 该段退化成直线
    Aligned = 3,     // 左右手柄共线但长度可不同
    Free = 4,        // 完全自由，写多少用多少
};

// 关键帧范围之外怎么走。
enum class Extrapolate : int {
    Hold = 0,         // 保持端点值（默认，也是 `sampleTrack` 的历史语义）
    Linear = 1,       // 沿端点切线继续
    Cycle = 2,        // 重复整段
    CycleOffset = 3,  // 重复整段，且每圈累加 (末端值 − 首端值)
    CycleFlipped = 4, // 重复整段，奇数圈镜像（"呼吸"常用）
};

// 一个关键帧。
struct CurveKey {
    float time = 0.0f;
    float value = 0.0f;

    Interp interp = Interp::Bezier;      // 从**这一帧**到下一帧的连法
    EaseMode ease = EaseMode::None;      // 仅 interp != Bezier 时生效
    EaseShape shape = EaseShape::Linear;

    HandleType handle = HandleType::AutoClamped;
    glm::vec2 outHandle{1.0f / 3.0f, 0.0f};    // 相对本帧，(dt >= 0, dv)
    glm::vec2 inHandle{-1.0f / 3.0f, 0.0f};    // 相对本帧，(dt <= 0, dv)
};

// 一条曲线。`keys` 必须按 `time` 升序（`sortCurve` 负责）。
struct Curve {
    std::vector<CurveKey> keys;
    Extrapolate before = Extrapolate::Hold;
    Extrapolate after = Extrapolate::Hold;

    // 循环周期（秒）。<= 0 → 用 `keys.back().time - keys.front().time` 推算。
    // 显式给值是为了"曲线只覆盖前 0.5 s，但我想按 2 s 一圈循环"这种写法。
    float cyclePeriod = 0.0f;
};

// ---- 名字互转（序列化与 UI 共用的单一出口）----
const char* interpName(Interp v);
const char* easeModeName(EaseMode v);
const char* easeShapeName(EaseShape v);
const char* handleTypeName(HandleType v);
const char* extrapolateName(Extrapolate v);

bool parseInterp(const std::string& s, Interp& out);
bool parseEaseMode(const std::string& s, EaseMode& out);
bool parseEaseShape(const std::string& s, EaseShape& out);
bool parseHandleType(const std::string& s, HandleType& out);
bool parseExtrapolate(const std::string& s, Extrapolate& out);

// ---- 标量工具 ----

// 时间重映射：`u ∈ [0,1] → v`。`EaseMode::None` 或 `EaseShape::Linear` 时
// 恒等于 u（**逐位**，不是"差不多"：所有既有资产的观感都挂在这一条上）。
float easeFn(EaseShape shape, EaseMode mode, float u);

// 归一化贝塞尔缓动：从 (0,0) 到 (1,1)，两根手柄是**相对**偏移。
//   `outH` 相对起点（x >= 0），`inH` 相对终点（x <= 0）。
//   默认 `{(1/3,0), (-1/3,0)}` = 标准 ease-in-out。
// 这是过场相机"这一段怎么走"用的那个 w；也是将来图编辑器里
// "把一个 0..1 的段单独放大看"用的同一份求根。
float bezierWeight(float u, glm::vec2 outH, glm::vec2 inH);

// 段内归一化求值：把"从 (0,0) 到 (1,1) 的一段"当成一条曲线来算。
// 存在的理由：`sampleCamera` 每帧要算一次 w，不能为了两帧就分配一个
// `std::vector<CurveKey>`；而它的语义又必须和 `evaluateCurve` **完全一致**
// （否则"过场相机"和"骨骼轨道"会在同一套缓动上走出两条曲线）。
// 所以两者共用同一份 `evalSegment()`，这里只是不给它分配容器。
float evaluateSegmentEase(Interp interp, EaseMode ease, EaseShape shape,
                          glm::vec2 outH, glm::vec2 inH, float u);

// 三次贝塞尔在参数 u 处的值（一维）。给图编辑器画线用。
float bezier1D(float p0, float p1, float p2, float p3, float u);

// 解 `x(u) = x`（x 单调时唯一）。Newton 8 次 + 二分兜底。
// `x1/x2` 是 P1.x / P2.x（P0.x = 0、P3.x = 1）。返回 u ∈ [0,1]。
float bezierSolveU(float x1, float x2, float x);

// ---- 曲线操作 ----

void sortCurve(Curve& c);

// 把每个关键帧的手柄按它自己的 `HandleType` 重算一遍。
// `Free` / `Aligned` 不动（作者写的就是答案）。
// 求值时不会改 `Curve`（const），所以内部有自己的解析函数；
// 这个入口是给"图编辑器要画手柄"和"存盘前想把自动值固化下来"用的。
void autoHandles(Curve& c);

// 解析后的手柄（相对偏移）。`Auto/AutoClamped/Vector/Aligned` 现算，
// `Free` 直接返回存的值。`index` 越界返回零。
glm::vec2 resolveOutHandle(const Curve& c, int index);
glm::vec2 resolveInHandle(const Curve& c, int index);

// t 落在哪一段（返回左端下标）。t < 首帧 → -1；t >= 末帧 → keys.size()-1。
int segmentIndexAt(const Curve& c, float t);

// 求值。空曲线 → 0；单帧 → 该帧值；越界 → 按 `before/after` 外推。
float evaluateCurve(const Curve& c, float t);

} // namespace animation
