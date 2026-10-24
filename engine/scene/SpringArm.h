#pragma once
// ============================================================
// scene/SpringArm —— 第三人称"弹簧臂"（UE SpringArmComponent 的思路）
//
// 相机不是"焊在角色身上的一根棍"，而是**从枢轴往后探一根可伸缩的臂**：
// 撞到墙就缩短，离开墙再伸回去。这一条解决第三人称最常见的两个毛病：
// 镜头穿墙、贴墙时角色被自己的镜头挡住。
//
// ------------------------------------------------------------
// 为什么从 runtime/ 搬到 engine/scene/
// ------------------------------------------------------------
// 这套逻辑现在**两个**调用方都要：
//   · runtime/PlayerController（Game.exe，物理世界射线）
//   · editor/EditorApp::runPlayers（编辑器 Play，场景 BVH 射线）
// 以前它只活在 runtime/ 里，编辑器那条链根本没有碰撞 —— 于是"编辑器里
// 贴墙相机钻进墙里"（用户第六轮第 3 条报障）。搬进 engine 之后两边共用
// 同一份实现，手感不会各走一套。
//
// ------------------------------------------------------------
// 与"一条射线"的区别（这是 UE 的做法，逐条对应）
// ------------------------------------------------------------
//   ① **扫掠体不是射线**：UE 用 SphereSweep，命中判据是"球心走到哪里会碰到"。
//      单条射线只对"正对着的平面墙"正确：斜墙、墙角、柱子边缘，射线会从
//      缝里穿过去，而相机的近裁剪面早就切进墙里了 —— 正是"贴墙时视野变成
//      墙内部"的根因。这里用"1 条中心射线 + 绕轴 8 条偏移射线"近似球扫掠：
//      每条偏移射线的起点就在球面上（横向偏移 = 半径），所以它的命中距离
//      就是球心能走到的距离，取最小值即可。
//   ② **最小臂长是"舒适值"，不是"硬下限"**：UE 的 MinTargetArmLength 只是
//      构图偏好，**不允许因为到了下限就让镜头留在墙里**。所以这里允许臂长
//      缩到 kHardMin（5 cm），宁可暂时贴到角色后脑勺，也不给"看墙内部"。
//   ③ **贴地单独管**：地形高度场不在刚体射线里，调用方把地形并进 trace
//      回调（见 runtime/PlayerController 的 makeTerrainAwareTrace）。
// ============================================================

#include <glm/glm.hpp>

#include <functional>

namespace scene {

// 一次"从 origin 沿 dir（单位向量）最多走 maxDist"的查询。
// 返回最近命中距离；无命中返回 >= maxDist 的数。
//
// 用 std::function 而不是抽象基类：两个调用方各自捕获自己的世界
// （PhysicsWorld* / Scene*），没必要为"多一种世界"引入一层继承。
// 每帧 9 次间接调用，可以忽略。
using SpringArmCast =
    std::function<float(const glm::vec3& origin, const glm::vec3& dir,
                        float maxDist)>;

struct SpringArmResult {
    glm::vec3 position{0.0f};   // 相机应该在哪
    float distance = 0.0f;      // 实际臂长
    bool blocked = false;       // 是否因为障碍缩短了
    glm::vec3 hitNormal{0.0f};  // 命中面法线（未命中为零向量）
};

// pivot：枢轴（角色头肩之间）；dir：**从枢轴指向相机**的单位向量；
// desiredDistance：期望臂长；minDistance：舒适下限（可以被突破）；
// radius：相机半径（通常 0.2~0.3 m，球扫掠的半径）。
//
// `cast` 为空 = 不做碰撞（等价于旧行为的关碰撞分支）。
SpringArmResult resolveSpringArm(const SpringArmCast& cast,
                                 const glm::vec3& pivot,
                                 const glm::vec3& dir, float desiredDistance,
                                 float minDistance, float radius);

// 臂长硬下限：再近就没法画了（相机会进到角色头里）。
inline constexpr float kSpringArmHardMin = 0.05f;

// ------------------------------------------------------------
// 相机不许钻进地面（球扫掠管不到的那一半）
// ------------------------------------------------------------
// 球扫掠解决的是"撞到墙"，但**俯视/仰视到极限时臂会甩到地表以下** —— 地面
// 在那里不是一堵墙，是一片"你本来就该在它上面"的平面，所以它只能单独管：
// 把相机沿竖直方向顶到 minZ 之上，再按新位置**反解出仍然看着枢轴的
// (pitch, distance)**。否则会出现两种坏画面：镜头在地底（视野全是地形背面），
// 或者镜头被顶高但朝向没变（突然开始看天，角色跑出画面）。
//
// ⚠ 这个函数存在的第二个理由是**防抄错符号**。运行时和编辑器是两条链，
//   相机 pitch 的符号约定还相反，手写两遍必然漂 —— 第一版运行时那一份就
//   把 `(pos.z - pivot.z)` 写成了 `(pivot.z - pos.z)`，于是钳制发生时相机
//   被镜像到枢轴下方（正是它想躲开的地底），下一帧再钳制 → 抖动。
//   自检 F 组直接断言"用反解出的 pitch/distance 重建位置 == 钳制后的位置"。
//
// pivot：枢轴；minZ：相机 z 的下界（地表 + clearance）；
// position：相机位置（**入参**，会被抬到 minZ）；
// distance / pitch：出参，按钳制后的位置反解（约定同 scene::Camera：
// pitch 正 = 相机在枢轴上方、往下看）。
// 返回 true = 确实发生了钳制（调用方可知 pitch 不是原始的）。
bool clampCameraAboveGround(const glm::vec3& pivot, float minZ,
                            glm::vec3* position, float* distance, float* pitch);

// 无窗口自检：不挡 / 正墙 / 半墙（只有偏移射线打得到）/ 极窄 / 地面钳制
// 的符号。由 `Game.exe --selftest springarm` 调用，输出
// `SPRINGARM-TEST pass|FAIL <名字> — <细节>` 与 `SPRINGARM-TOTAL pass=n fail=m`。
int runSpringArmSelfTest();

} // namespace scene
