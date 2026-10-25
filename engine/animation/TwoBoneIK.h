#pragma once
// ============================================================
// animation/TwoBoneIK —— 两骨解析 IK（L1 修正层，2026-10-25 第八刀）
//
// 目标：给一个**世界坐标目标点**，让 root → mid → end 这条两节链的末端
// 落在目标上（脚踩地面、手扶墙、手握枪柄）。三根骨、解析解、不迭代。
//
// ------------------------------------------------------------
// 为什么是解析解而不是 FABRIK / CCD
// ------------------------------------------------------------
// 两骨链有**闭式解**（余弦定理），一次算完：
//
//     cos θ_root = (L1² + d² − L2²) / (2·L1·d)
//     cos θ_mid  = (L1² + L2² − d²) / (2·L1·L2)
//
// 迭代解的优势是"能推广到 N 骨"，但对两骨来说它带来三个纯负担：
//   · 收敛次数不定 → **不确定**。同一帧跑两次结果可以不同，
//     "同一输入逐像素相同"这条回归断言就废了。
//   · 需要初值 → 姿势变了收敛路径也变 → 快速运动时抖动。
//   · 代价是解析解的 10~30 倍，而收益（推广性）在这个接口上不存在。
// FABRIK 留给将来的多骨链（手指 / 尾巴），不进这一个。
//
// ------------------------------------------------------------
// ⚠ 极向量（pole / bend direction）必须**从当帧动画反推**
// ------------------------------------------------------------
// 两骨 IK 只约束"末端在哪"，还剩**一个自由度**：链绕 root→target 轴转。
// 这个自由度决定膝盖/手肘朝哪边弯。三种给法：
//
//   ① 构建时缓存一个 pole 方向（静止姿势下算的）—— **不能用**。
//      症状：蹲下时膝盖朝前，一站直就朝侧面。因为动画本身在改弯曲平面
//      （走路的腿和劈叉的腿，膝盖朝向的"自然选择"不同），而缓存的常量
//      跟不上。更糟的是它**不会报错**，只是看起来"这个角色的腿有点怪"。
//   ② 每帧从动画里读一个显式的 pole 骨（UE 的 IK Rig 的做法）—— 需要额外
//      绑定，资产改动。
//   ③ **从当帧动画的 mid 骨位置反推**：把 (mid − root) 投影到垂直于
//      root→target 的平面上，归一化，就是当前的弯曲方向。零配置、零状态、
//      跟着动画走。**本文件用这个**（`PoleFromAnimation`）。
//
// ③ 的退化情况必须处理：动画里腿完全伸直时，(mid − root) 与 root→target
// 平行，投影长度为 0 → 方向未定义。此时退到 `poleVector` 配置值；
// 仍然退化就任取一个垂直轴。**不能直接 normalize 零向量** —— 会得到 NaN，
// 然后整条链消失（画面上是"角色腿没了"，日志里一行错都没有）。
//
// ------------------------------------------------------------
// 为什么这个 solver **无状态**
// ------------------------------------------------------------
// 没有成员、没有"上一帧的旋转"、不需要按实体清理。代价是放弃了自带平滑
// （目标点跳变时腿会瞬移），收益是三条硬性质：
//   · `weight = 0` → 提前 return，姿势**逐位不变**（回归基线不会飘）
//   · 同一姿态同一目标 → 结果逐位相同（确定性，可断言）
//   · 实体删除不需要清理状态（没有状态可泄漏）
// 目标点的平滑是**上游**的事（角色控制器 / 动画里的 foot lock 曲线），
// 塞进 IK 里会让"目标点是 X"和"实际到达点"之间多一层看不见的延迟。
// ============================================================

#include "animation/Pose.h"
#include "assets/Skeleton.h"

#include <glm/glm.hpp>

#include <string>

namespace animation {

struct TwoBoneIKSettings {
    bool enabled = true;
    // target 有效才生效。与 LookAt 同一条约定：(0,0,0) 是合法目标点，
    // 不能拿"是不是零向量"当开关。
    bool hasTarget = false;

    // 目标点，**世界坐标，glTF 场景空间**（与 Pose / poseToWorld 同一套）。
    // ⚠ 不是引擎的 Z-up 世界空间：`Pose` 全程在 glTF 场景空间里，
    //   Y-up→Z-up 那一转在 `PushConstants::model` 里（Skeleton.h 头注释）。
    //   传错空间的症状是"脚去够头顶上方的一个点"，而且看起来像参数调错了。
    glm::vec3 target{0.0f};

    // 链的三根骨。名字查找走 `Skeleton::findJoint`（不区分大小写、允许包含）。
    // 留空有默认：
    //   rootBone 空 → 依次找 thigh / upleg / upperleg / upperarm / uparm
    //   midBone  空 → root 的"最长的子骨"（腿 = 小腿，臂 = 前臂）
    //   endBone  空 → mid 的"最长的子骨"
    // 显式给了名字但**不成链**（mid 的父不是 root）→ 整个 solver 拒绝执行，
    // 而不是硬算 —— 不成链时下面"父骨世界旋转"那一步会静默用错坐标系。
    std::string rootBone;
    std::string midBone;
    std::string endBone;

    // 融合权重：0 → 逐位等于没有 IK（这条要能断言）。1 → 完全落在目标上。
    float weight = 1.0f;

    enum PoleMode : int {
        // 从当帧动画的 mid 骨位置反推弯曲平面（默认，见文件头）
        PoleFromAnimation = 0,
        // 用一个显式的世界方向。`poleVector` 会先投影到垂直于 root→target
        // 的平面上 —— 这样"pole 在正上方"这类配置在任意目标方向上都能用。
        PoleFromVector = 1,
    };
    PoleMode poleMode = PoleFromAnimation;
    glm::vec3 poleVector{0.0f, 0.0f, 1.0f};

    // 可达性上限（相对 L1+L2 的比例）。
    // ⚠ 不能取 1.0：正好伸直时 d = L1+L2，余弦定理的自变量会算出
    //   1.0000001，`acos` 返回 NaN，整条链消失。
    float maxReachScale = 0.999f;
};

// 解析出的链（下标；-1 = 没解析出来）
struct TwoBoneIKChain {
    int root = -1;
    int mid = -1;
    int end = -1;
    bool valid() const { return root >= 0 && mid >= 0 && end >= 0; }
};

// 按 settings 的名字解析三根骨。**每帧调**（字符串查找，60 根骨可忽略），
// 不做缓存 —— 缓存要跟骨架生命周期走，而收益是微秒级。
TwoBoneIKChain resolveTwoBoneChain(const assets::Skeleton& skel,
                                   const TwoBoneIKSettings& s);

// 本帧解算的一份账。`ANIM-IK` 日志与自检断言都吃它 ——
// "IK 有没有生效"在截图上分不出 2cm 和 20cm，只能靠数值。
struct TwoBoneIKResult {
    bool applied = false;        // 真的写了 pose
    int root = -1, mid = -1, end = -1;

    float targetDist = 0.0f;     // |target − root|（未钳制）
    float reachDist = 0.0f;      // 钳制之后实际去够的距离
    float errorBefore = 0.0f;    // 解算前 |end − target|
    float errorAfter = 0.0f;     // 解算后 |end − target|（**实测**，二次 FK）
    float bendAngleDeg = 0.0f;   // mid 骨的弯曲角（0 = 完全伸直）

    // 为什么 errorAfter 是实测而不是解析推出的：
    //   weight < 1 时两根骨的局部旋转被 slerp 各插一半，末端**不在**目标上，
    //   解析值会给出一个漂亮的 0 而实际差几厘米。日志里报一个假的 0
    //   比不报更糟。代价是一次额外 FK（63 个 mat4 乘），值得。
};

// 就地修改 pose 的**两根**局部旋转（root / mid）。返回解算账。
// 只读 `pose`，不改 translation / scale：骨骼长度由姿态本身保证。
TwoBoneIKResult solveTwoBoneIK(const assets::Skeleton& skel,
                               const TwoBoneIKSettings& s, Pose& pose);

} // namespace animation
