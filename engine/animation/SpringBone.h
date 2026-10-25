#pragma once
// ============================================================
// animation/SpringBone —— 头发 / 裙摆 / 尾巴的质点弹簧（L4 表演层）
//
// 为什么是它先做（2026-10-25）：
//   审计里"表演层"缺五块（驱动 / 合成 / 修正 / 表情 / 次级运动），其中四块
//   要么需要新资产格式（表情）、要么需要新状态机（驱动）、要么收益偏"不出错"
//   （IK）。只有次级运动是**纯 CPU 后处理 + 零资产改动** —— 骨骼名就是全部
//   配置，模型不用重烤、`.vmesh` 不用升版本。
//   而它对"这是不是二次元"的贡献极高：飘动的头发是最容易被人记住的动态特征。
//
// ------------------------------------------------------------
// 它在流水线的哪一格
// ------------------------------------------------------------
//   samplePose → blendPose → [LookAt] → [SpringBone] → poseToJoints
//                                       ^^^^^^^^^^^ 这里
// 必须在 poseToJoints **之前**：弹簧骨改的是局部旋转（`Pose::rotation`），
// 世界矩阵与 invBind 由 poseToJoints 一次算完。放在它之后就晚了。
//
// ------------------------------------------------------------
// 三个只有实际跑起来才会发现的约定
// ------------------------------------------------------------
//
// ① **重力是 -Y，不是 -Z。**
//    `Pose` 与 `poseToJoints` 全部工作在 **glTF 场景空间**（Y-up）；
//    引擎的 Y-up→Z-up 那一转在 `PushConstants::model` 里（`Skeleton.h` 头注释）。
//    写 `-Z` 的后果不是"头发不飘"，是"头发被往侧后方拽"，而且因为阻尼存在
//    看起来很像是参数调错了 —— 会浪费很久在调参上。
//    所以重力是**可配的**（默认 0,-9.8,0），而不是写死的常量。
//
// ② **attach 骨不模拟。**
//    美术的命名通常是 `hair_01`（缝在头上）/ `hair_02` / `hair_03`。
//    只有"父骨也匹配同一前缀"的骨才被物理驱动；链的**第一根**（父骨不匹配，
//    通常是 head）受动画控制、作为固定端。
//    否则 `hair_01` 会把头发的根拽离头皮 —— 表现为"发际线在飘"。
//
// ③ **必须有"回弹到造型"的力。**
//    只做重力 + 阻尼的话，角色站久了头发会**垂直下垂**。二次元角色的头发
//    是塑过型的（翘起、卷曲），塌下来等于毁掉人设。`restStiffness` 把质点
//    往"父骨空间里的静止方向"拉，值小（0.1~0.3）时是"保持造型的轻微摆动"，
//    值 0 时是"纯物理的布条"。
//
// ------------------------------------------------------------
// 为什么不用 Bone Spring 的解析式（正弦抖动）
// ------------------------------------------------------------
// 正弦抖动看不出"惯性"：角色急停时头发应该继续往前甩一下，正弦做不到。
// Verlet 质点 + 距离约束的成本是每骨每帧十几次浮点运算，几十根骨可以忽略，
// 而它能给出真正的滞后与回摆 —— 这才是"飘"的来源。
// ============================================================

#include "animation/Pose.h"
#include "assets/Skeleton.h"

#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace animation {

// 一具骨架的弹簧拓扑。构建一次、缓存起来（拓扑只跟骨骼名有关，
// 与姿态无关）。
struct SpringRig {
    // 参与模拟的 joint 下标，**按 topoOrder 排** —— 父骨一定排在子骨之前。
    // 这个顺序是硬要求：反解旋转时要读"父骨被改之后"的世界旋转。
    std::vector<int> simulated;
    // 与 simulated 对齐：本骨到父骨的**静止**距离（世界空间长度）。
    std::vector<float> restLen;
    // 与 simulated 对齐：父骨空间里的静止方向（单位向量）。
    // 回弹力用它 —— 存父骨空间而不是世界空间，因为父骨每帧都在转。
    std::vector<glm::vec3> restDir;

    int count() const { return static_cast<int>(simulated.size()); }
    bool empty() const { return simulated.empty(); }
};

// 按骨骼名前缀构建拓扑（不区分大小写）。
//   * prefixes 为空 → 用 kDefaultSpringPrefixes
//   * 一个匹配的骨都没有 → 返回空 rig（调用方据此跳过整个阶段）
SpringRig buildSpringRig(const assets::Skeleton& skel,
                         const std::vector<std::string>& prefixes);

// 默认前缀。**裸 `hair` / `skirt`，不带分隔符**（不区分大小写）。
//   为什么不是 `hair_`：真实资产 alicia 的头发骨叫 `hair1_L`，
//   带分隔符的前缀一个都匹配不上（见 SpringBone.cpp 的注释）。
//   为什么没有 `tail`：人体骨架里 tail 常指尾椎，默认匹配会让脊柱软掉。
extern const char* const kDefaultSpringPrefixes[];
extern const int kDefaultSpringPrefixCount;

struct SpringBoneSettings {
    bool enabled = true;

    // 重力（glTF 场景空间，Y-up）。见文件头 ①。
    glm::vec3 gravity{0.0f, -9.8f, 0.0f};
    // 恒定风。加一点（0.2~0.5）就有"空气感"，是廉价的表现力。
    glm::vec3 wind{0.0f};

    // 速度保留率。0.85~0.95 是二次元的手感区间：
    //   0.99 → 发丝像钢条，回摆很久不停
    //   0.70 → 像泡在水里，黏
    float damping = 0.90f;

    // 回弹到静止造型的强度。见文件头 ③。0 = 纯布条，0.3 = 定型发丝。
    float restStiffness = 0.15f;

    // 距离约束的迭代次数。1 次在快速移动时会"拉长"，
    // 3 次对几十根骨的链足够（再多肉眼无差别）。
    int iterations = 3;

    // 单步最大 dt。可变帧率下 dt 突然变大（窗口拖动、断点恢复）会让
    // Verlet 直接炸开 —— 钳住比"检测到异常再重置"体面得多。
    float maxStep = 1.0f / 30.0f;
};

// 每个角色一份的运行时状态（不进存档：物理状态是"这一帧正在发生的事"）。
class SpringBoneState {
public:
    // 丢弃惯性，下一帧从动画姿态重新起步（例如切换模型 / 重新 Play）。
    void reset() {
        m_init = false;
        m_pos.clear();
        m_prev.clear();
    }
    bool initialized() const { return m_init; }

    // 就地修改 `pose`（只写 rotation）。返回本帧模拟的质点数。
    //
    // ⚠ 第一次调用只**采样起步状态**、不修改 pose，返回 0：
    //   这样"加组件的第一帧"与"完全不加组件"逐像素相同，
    //   回归基线不会因为"多跑了一帧物理"而飘。
    int update(const assets::Skeleton& skel, const SpringRig& rig, float dt,
               const SpringBoneSettings& s, Pose& pose);

    // 上一帧解算出的质点世界位置（自检断言用；长度 = rig.count()）。
    const std::vector<glm::vec3>& positions() const { return m_pos; }

private:
    std::vector<glm::vec3> m_pos;
    std::vector<glm::vec3> m_prev;
    bool m_init = false;
};

// 两个单位向量的最短弧旋转。glm 没有现成的，手写一处、两处（弹簧 / LookAt）
// 共用，避免各自实现时把"反向 180°"这个退化情况漏掉。
glm::quat shortestArc(const glm::vec3& from, const glm::vec3& to);

} // namespace animation
