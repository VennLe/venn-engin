#pragma once
// ============================================================
// animation/BoneMask —— 骨骼遮罩（B1 动画层的地基）
//
// 为什么单拎一个文件（2026-10-26）：
//   "上半身挥手 + 下半身走路" 这件事的本质是**同一帧有两份姿势，按骨
//   分开取**。它有三个独立的知识点，任何一个写错都只表现为"看着怪"：
//     ① 哪些骨算"上半身" —— 靠**父链子树**，不是靠名字列表写死；
//     ② 混合必须在**局部 TRS** 空间（否则插出缩放/剪切，角色变瘦）；
//     ③ 端点必须**逐位短路**（w=0 与 w=1 都要与来源逐位相同）。
//   写进 AnimatorSystem 里的话，这三条会散在 `tick` 两百行里，第三个用
//   遮罩的人（描边遮罩 A7、蒙皮 LOD、布料分区）必然再抄一遍。
//
// ------------------------------------------------------------
// 为什么 mask 用"根骨 + 子树"而不是"骨的名单"
// ------------------------------------------------------------
//   名单的写法（"spine,spine1,spine2,neck,head,shoulder_l,..."）有两个
//   致命问题：
//     · 资产一换（AutoRig / 换骨架 / Mixamo 改名）名单就整体失效，
//       而且是**静默失效** —— 少列一根骨 = 那只手不跟着动，不报错；
//     · 美术改不了。"上半身"在他们脑子里是一句话，不是 14 个名字。
//   子树语义只要一个根（`spine`），加一根骨时自动被包含。
//
// ------------------------------------------------------------
// 名字匹配：**不分大小写的包含匹配**，且匹配**全部**命中项
// ------------------------------------------------------------
//   不用 `findJoint` 那套"精确优先、命中即返回"：遮罩要的是"所有叫
//   hand 的骨"，不是"第一根叫 hand 的骨"（`hand_l` / `hand_r` 都得进）。
//   用包含而不是前缀：Mixamo 的 `mixamorig:Spine` 用前缀 `spine` 匹配
//   不到，用包含能 —— 而遮罩匹配错一根骨的症状是"半边身体跟着动"，
//   比 LookAt 匹配错更难看出来（它没有"目标"可以对）。
//
// ⚠ 匹配 0 根 = **该 root 无效，不是"全部"**。空遮罩必须等于"这一层
//   什么都不改"，否则配错名字就会变成"整具身体被另一段动画覆盖" ——
//   那是把一个"没生效"变成"全错了"。
// ============================================================

#include "animation/Pose.h"
#include "assets/Skeleton.h"

#include <string>
#include <vector>

namespace animation {

// 三种模式而不是"一个开关 + 取反"：
//   UI 上"包含 / 排除"是人的两种说法，落成 bool 之后 Inspector 与序列化
//   各要写一次 `!`，两边不同步就是"面板上说排除、实际是包含"。
enum class BoneMaskMode : int {
    All = 0,    // 全骨（= 没有遮罩）。默认值，老数据天然落在这里。
    Allow = 1,  // 只有 roots 及其子树
    Deny = 2,   // roots 及其子树**以外**
};

const char* boneMaskModeName(BoneMaskMode m);
// 越界 → false（调用方保持原值），用于读档容错。
bool boneMaskModeFromInt(int v, BoneMaskMode* out);

// 组件里存的就是这个东西（纯配置，可序列化）。
struct BoneMaskSpec {
    BoneMaskMode mode = BoneMaskMode::All;
    std::vector<std::string> roots;   // 每项是一个"根骨名片段"

    bool isAll() const {
        return mode == BoneMaskMode::All || roots.empty();
    }
};

// 解算结果：每根骨一个权重。
//
// 现在权重只有 0 / 1 —— 保留 float 是因为"柔和遮罩"（手指、下巴）迟早
// 要做，而换成 float 会让所有调用点重写一遍。语义上 0 = 完全不吃这一层。
struct BoneMask {
    std::vector<float> weights;
    // 权重 > 0 的骨数（UI 与日志用：0 = 遮罩等于没生效，几乎总是配错名）
    int included = 0;
    // 一个骨都没匹配到的 root 数 + 第一个出问题的名字
    int unresolved = 0;
    std::string firstProblem;

    int count() const { return static_cast<int>(weights.size()); }
    // 全部骨都吃这一层（= 等价于"没有遮罩"，混合可以退化成一个整体权重）
    bool full() const { return count() > 0 && included == count(); }
    // 一根都不吃（配错名字最常见的形态）
    bool empty() const { return included == 0; }
};

// 解算：把 spec 落到具体骨架的每根骨上。
// 骨架为空 / All 模式 → 全部 1.0；匹配到 0 根 → 全部 0.0（见文件头）。
BoneMask buildBoneMask(const assets::Skeleton& skel, const BoneMaskSpec& spec);

// 缓存键。**必须包含骨架身份**（由调用方把骨架地址拼在前面）：
// 同一份 spec 落到不同骨架上解出来的 weights 长度都不同。
std::string boneMaskKey(const BoneMaskSpec& spec);

// out = base，然后按 `mask * weight` 把 layer 混进来（局部 TRS 空间）。
//
// 三条硬约束（都有对应自检）：
//   ① `weight <= 0` 或遮罩为空 → out 与 base **逐位相同**（回归基线靠它）；
//   ② `w >= 1` → 该骨与 layer **逐位相同**（`slerp(q,b,1)` 在浮点下不是
//      恒等，不短路的话"遮罩全开 + 权重 1"的姿势会带 1e-7 的偏差，
//      长跑之后角色会慢慢转歪）；
//   ③ `out` 允许与 `base` 同址（内部先整体拷贝再就地混），但**不能**与
//      `layer` 同址。
void blendPoseMasked(const Pose& base, const Pose& layer, const BoneMask& mask,
                     float weight, Pose& out);

} // namespace animation
