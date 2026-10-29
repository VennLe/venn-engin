#pragma once
// ============================================================
// animation/CurveBake —— 骨骼轨道 ⇄ 曲线（S-D，Graph Editor 接到骨骼上）
//
// ------------------------------------------------------------
// 这一层存在的理由，一句话
// ------------------------------------------------------------
// `animation::Curve`（R5）与曲线视图（R6）都齐了，但视图画的是**过场相机**：
// 数据源写死 `sequence::SequenceAsset`，通道是 `PosX/Y/Z / Yaw / Pitch / Fov`。
// 骨骼那边还是 `assets::AnimTrack` —— 只有 `bool step`（常量 / 线性），
// 没有贝塞尔、没有缓动、没有手柄。于是"动作有生命"这件事一件都做不到：
// 手抬起和放下是两条匀速直线，谁都看得出是机器做的。
//
// 所以这里补的是**最后一环**：把 `EditTrack`（编辑产物，可写）投影成
// `animation::Curve`（可画、可拖、有手柄），改完再写回去。
//
// ------------------------------------------------------------
// 为什么是"单向烘焙"，而不是让 `AnimTrack` 直接变曲线
// ------------------------------------------------------------
// `assets::AnimTrack` 是 glTF/FBX 解析出来的**导入产物**，只读；而且
// `engine/assets/**` 在 GLOBAL_GLOBS 里（动它就是全量回归，见 roadmap §5）。
// 更根本的是语义：导入的采样表**没有手柄**，它是"厂商烘好的结果"，
// 不是"作者的意图"。硬把两者塞进一个结构，重新导入就成了一场数据合并。
//
// 所以方向永远是：
//
//     EditTrack  ──bakeTrackCurve()──▶  Curve      （投影，只读视图用）
//     Curve      ──applyCurveToTrack()──▶ EditTrack （写回，改的是键的
//                                                    时刻/值/插值/手柄）
//
// **没有任何一步就地改 `assets::AnimTrack`。**
//
// ------------------------------------------------------------
// 四条"省了就错"的约定
// ------------------------------------------------------------
// ⚠⚠ ① **没有缓动 = 逐位不变**。`EditTrack::ease` 为空（或全默认）时，
//     `sampleResolvedTrack` 直接转发 `assets::sampleTrack`，一个浮点运算
//     都不多做。这条是"打开曲线编辑器再关掉，动画不许有一丝变化"的依据，
//     由 `curvebake_default_bitwise` / `curvebake_no_ease_bitwise` 钉住。
//
// ⚠⚠ ② **旋转的缓动**分两种，因为两者数学上不是一回事：
//       · Linear + 缓动：`slerp(qa, qb, easeFn(u))` —— 只重映射时间，
//         走的还是那条大圆弧。缓动恒等时**直接转发** `sampleTrack`
//         （同一个 `glm::slerp`，同一份 u），所以逐位相同；
//       · Bezier：四个分量各自解贝塞尔再归一。这是 **nlerp 语义**
//         （同一条弧、另一种速度分配），也是唯一能"手工塑造四元数速度"
//         的路子 —— Blender 的四元数 F-Curve 同样是逐分量插值后归一。
//     ⚠ 别把这两条合并成一条：用 nlerp 实现 Linear，全部老旋转轨的中间帧
//       都会变（`slerp` 与 `nlerp` 在 w=0.5 附近差百分之几，肉眼可见肘部
//       走弧线走扁了）。
//
// ⚠⚠ ③ **插值与手柄属于"键"，不属于"分量"**。一个 `EditTrack` 就是一条
//     glTF channel，四个分量**共用关键帧**（`times` 只有一份）。所以给
//     rotation.x 设贝塞尔，rotation.y/z/w 也跟着贝塞尔。这不是偷懒：
//     分开设会让 w 和 x 用不同的时间重映射，四元数离开单位球再被归一，
//     表现成"转轴在中途晃了一下"。**值**当然各分量独立。
//     而 `AutoClamped` 手柄是按**本分量自己的邻居**现算的（见
//     `resolveOutHandle`），所以默认情况下每个分量拿到的切线是对的；
//     只有手拖过的手柄（Free / Aligned / Vector）是四分量共用的 ——
//     那也有道理：一根骨在某一时刻的"节奏"只有一个。
//
// ⚠⚠ ④ **写回不增删关键帧**。`Curve.keys.size()` 必须等于 `keyCount()`，
//     不等就整条拒绝（`skipped`）。理由同上：给 rotation.x 插一帧，另外
//     三个分量就被迫凭空得到一个"打帧时刻"，那是 R8 的 Dope Sheet 该做
//     的事（它在 `EditClip::setKey` 上一并处理）。曲线编辑器的职责是
//     **改形状**，不是改帧数。
// ============================================================

#include "animation/ClipEdit.h"
#include "animation/Curve.h"

#include "assets/Skeleton.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace animation {

// ------------------------------------------------------------ 分量

// 一条路径有几个标量分量：Translation / Scale = 3，Rotation = 4（四元数）。
int pathComponentCount(assets::AnimTrack::Path p);

// 分量名：T/S → "x"/"y"/"z"；R → "x"/"y"/"z"/"w"（**与 glm::quat 的
// (x,y,z,w) 顺序一致**，也和 `assets::AnimTrack` 存 value 的顺序一致）。
// 越界返回 "?"。
const char* pathComponentName(assets::AnimTrack::Path p, int comp);

// 路径短名："translation" / "rotation" / "scale"（与 `.animclip.json` 同字面）
const char* pathName(assets::AnimTrack::Path p);

// ------------------------------------------------------------ 缓动表解析

// 把 `EditTrack::ease` 展开成**与 times 等长**的一张表，并处理 step 轨道的
// 兜底：`step == true` 且该键没有非默认缓动 → `Interp::Constant`。
//
// 为什么要有这一步：`step` 是**轨道级**属性（导入产物的一部分），
// 缓动表是**键级**的。作者只给第 0 个键设了贝塞尔时，第 1 个键在表里
// 是"缺项"，直接取默认 Linear 会把一条 step 轨道悄悄改成线性的 ——
// 而 step 轨道上"线性"与"常量"在画面上是天壤之别（开关灯 vs 渐亮）。
void resolveEaseTable(const EditTrack& tr, std::vector<KeyEase>& out);

// ------------------------------------------------------------ 烘焙（单向）

// `EditTrack` 的一个标量分量 → `Curve`。
//
// 手柄按"绝对偏移"搬（`KeyEase` 存的就是绝对量），**不做**相机那种
// 段内归一化。`comp` 越界 → 空曲线。
Curve bakeTrackCurve(const EditTrack& tr, int comp);

// 填 `ResolvedTrack` 的 `ease / comps / hasEase / curves`。
//
// ⚠ 要传**源 `EditTrack`**：缓动表住在编辑层，`ResolvedTrack::track` 是
//   `assets::AnimTrack`（没有那个字段）。`rt.track` 必须已经填好 times/
//   values/path/step —— 这里回填其余四项。
// 幂等；`hasEase == false` 时 `curves` 留空（省内存，也省得有人拿它当
// "这条轨道有缓动"的证据）。
void bakeResolvedTrack(const EditTrack& src, ResolvedTrack& rt);

// ------------------------------------------------------------ 采样

// `EditTrack` 在 time 处的值。**没有缓动就转发 `sampleTrack`**（约定 ①）。
// 给自检与工具用；运行时走 `sampleResolvedTrack`（曲线已经烘好）。
glm::vec4 sampleEditTrack(const EditTrack& tr, float time);

// 同上，但用预烘的曲线。
glm::vec4 sampleResolvedTrack(const ResolvedTrack& rt, float time);

// ------------------------------------------------------------ 写回

struct CurveWriteStats {
    int keys = 0;      // 输入曲线的关键帧数
    int written = 0;   // 真正写进轨道的键数
    int changed = 0;   // 其中值或时刻与写回前不同的键数
    int merged = 0;    // 被挪到同一时刻而合并掉的键（>0 就是丢帧）
    int skipped = 0;   // 因 keyCount 不等 / 分量越界被整条拒绝（约定 ④）
    // 写回后再烘一遍，与传入曲线在 33 个采样点上的最大偏差。
    // 正常情况下 **恰好 0.0**（同一份数据、同一个求值器）——
    // 非 0 就说明写回漏了字段（手柄、interp、ease 三者之一）。
    float worstDeviation = 0.0f;
    bool bitIdentical = false;
};

// 把一条曲线写回 `tr` 的第 `comp` 个分量：时刻、值、插值、手柄。
CurveWriteStats applyCurveToTrack(EditTrack& tr, int comp, const Curve& c);

// ------------------------------------------------------------ 通道表（UI）

// 曲线编辑器里"一条能看到曲线"的最小单位：某个轨道的某个分量。
// ⚠ 用 `(track, comp)` 而不是"关节+路径+分量"：轨道在 `EditClip` 里已经
//   是唯一的（`findTrack` 保证），再拿名字查一遍等于给了两次出错机会。
struct BoneChannel {
    int track = -1;   // EditClip::tracks 下标
    int comp = 0;     // 分量下标
};

// 展开顺序 = tracks 顺序 × 分量顺序。**这个顺序就是 UI 列表的顺序**，
// 也是注入脚本里 `chan:N` 的编号 —— 所以它必须稳定：tracks 的顺序是
// "首次打帧的顺序"（见 `EditClip::setKey`），不会因为改值而变。
std::vector<BoneChannel> listBoneChannels(const EditClip& clip);

// 通道显示名：`upperarm.L/rotation.x`。`track` 越界 → "(none)"。
std::string boneChannelId(const EditClip& clip, const BoneChannel& ch);

// ------------------------------------------------------------ 摘要

// 一条曲线的指纹（跨进程比对用：同一个文件在两次运行里必须给出同一个数）。
// 喂进去的是**会进求值的字段**，不含 `cyclePeriod` 之外的展示性数据。
unsigned long long curveDigest(const Curve& c);

} // namespace animation
