#pragma once
// ============================================================
// assetio/AutoRig —— 静态人形网格 → 骨架 + 蒙皮权重（**一键自动绑骨**）
//
// 目标（对标 Mixamo 的 Auto-Rigger）：给一个**没有骨骼**的人形网格，
// 自动生成一副可用的骨架与权重，让"下载一个 FBX 就能当角色用"这件事
// 不再需要 Blender + 手刷权重。
//
// ------------------------------------------------------------
// 这是"够用且可解释"的那一档，不是论文级的骨架提取
// ------------------------------------------------------------
// 学术界（Pinocchio / 网格收缩 + 内接球）能处理任意姿势与拓扑，代价是
// 重重叠叠的优化步骤 —— 出了问题没法解释，也没法在回归里断言。这里走的
// 是**量测 + 解剖比例表**这条线：
//
//   1) 量：世界 AABB 给身高 H、臂展；水平两轴里更长的那个是**手臂轴**
//      （T/A-pose 都成立）；脚部的质心相对身体质心的偏移方向是**朝向**
//      —— 脚趾一定朝前，这一条比"猜 +Y"可靠。
//   2) 定位：先按人体解剖比例表给每个关节一个初值，再用**沿高度切薄层
//      的顶点质心**把它拉到网格实际的位置上。于是 2.05 m 的壮汉和
//      1.55 m 的少女得到的是各自的骨架，而不是同一副被整体缩放的骨架。
//   3) 蒙皮：顶点到**骨段**（父关节→本关节的线段）的距离反比，
//      4 次幂衰减 + 左右分区闸门 + 取前 4 个归一化。
//      闸门是必须的：没有它，垂在手边的左手会抢走右腿的权重（T-pose 下
//      两手的球面距离很小），表现为"抬手时大腿跟着动"。
//   4) 出骨：`restLocal` **只有平移**（旋转全为单位阵），
//      `invBind = 反世界矩阵`。纯平移层级是刻意的 —— 程序化动画
//      （ProcAnim）只需要"绕某轴转多少度"，不必反解欧拉角，也就不会
//      出现"旋转顺序和导出器不一致导致姿势歪掉"这类经典事故。
//
// ------------------------------------------------------------
// 空间约定（这是最容易搞错的一步）
// ------------------------------------------------------------
// 引擎世界 = 米、右手系、**Z 轴朝上**。自动绑骨前会把每个子网格的
// `transform` **烘进顶点**（并把 transform 置为单位阵），于是：
//   顶点坐标 == 模型世界坐标 == 骨架所在的空间
// 这样 `jointMat = invMeshNode · world(joint) · invBind` 里
// `invMeshNode = I`，静止时 jointMat = I，与 FBX/glTF 路径同一条公式。
// 不烘的话，带非单位变换的子网格会"骨骼一套空间、顶点另一套空间"，
// 症状是姿势对但整体偏移一个平移量，极难查。
//
// ------------------------------------------------------------
// 命名
// ------------------------------------------------------------
// 关节名沿用 Mixamo 的习惯（Hips / Spine / LeftArm / LeftUpLeg …），
// 这样"换成厂商骨架"时按名字就能对位比较（验证脚本正是这么做的）。
// **左右按解剖学定**：`left = normalize(cross(up, forward))`，
// 不靠 +X 或 -X 的约定 —— 不同导出器朝 +Y / -Y 的都有。
// ============================================================

#include "assetio/SourceModel.h"

#include <string>
#include <vector>

namespace assetio {

struct AutoRigOptions {
    // 先把子网格变换烘进顶点（见文件头的"空间约定"）。默认开。
    // 关掉只在"调用方自己已经烘过"时有意义。
    bool bakeTransforms = true;

    // 每个顶点最多几根骨影响（渲染管线固定 4 槽，改这个要一起改 shader）
    int maxInfluences = 4;

    // 距离反比的指数。4 足够"近的骨头压倒性占优"，2 会糊成一片。
    float weightFalloff = 4.0f;

    // 已有骨架时是否覆盖。默认 false（保护厂商绑定）。
    bool overwriteExisting = false;
};

struct AutoRigReport {
    // ---- 量测 ----
    float height = 0.0f;      // H：AABB 的 Z 向高度（米）
    float armSpan = 0.0f;     // 臂展（手臂轴上的跨度）
    int armAxis = 0;          // 0 = X，1 = Y
    int fwdAxis = 1;
    float fwdSign = 1.0f;     // 朝向 = fwdSign × 正 fwdAxis
    float halfSpan = 0.0f;    // 单侧臂长（中轴到指尖）

    // ---- 结果 ----
    int jointCount = 0;
    int vertices = 0;
    int verticesRigged = 0;   // 真正拿到 ≥1 根骨权重的顶点数
    float maxWeightError = 0.0f;  // 归一化残差的最大值（应 < 1e-4）
    float maxInfluences = 0.0f;   // 实际出现的最大影响数（≤ 4）

    // 关节位置（按 joints 顺序），给"和厂商骨架对比"用
    std::vector<std::string> jointNames;
    std::vector<glm::vec3> jointPositions;
};

// 对 model **就地**生成骨架与权重。
//   · 已有骨架且 `overwriteExisting=false` → 返回 false，error 说明原因；
//   · 找不到可用的人形量测（高度过小 / 顶点太少）→ 返回 false。
// 不抛异常：失败靠返回值 + error，与 importFbx 同一风格。
bool autoRigHumanoid(SourceModel& model, const AutoRigOptions& opt,
                     AutoRigReport* report, std::string* error);

} // namespace assetio
