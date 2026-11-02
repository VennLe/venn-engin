#pragma once
// ============================================================
// nav/NavBake —— 从物理世界烘一张导航格子（C8-2）
//
// ------------------------------------------------------------
// 一格是怎么判定的（四步，顺序有讲究）
// ------------------------------------------------------------
//   ① 从格子中心的正上方（`max.z`）打一条向下的射线。
//      没有命中 → 这一格**没有地面**（坑、悬空），记 NaN → Blocked。
//   ② 命中面的法线 `n.z < cos(maxSlope)` → 太陡，站不住 → Blocked。
//      判据与 `CharacterController` 完全一致（都是"法线的 z 分量"），
//      否则会出现"导航说能走、角色站不住"的经典缝隙。
//   ③ 代理体积检查：在命中点上方 `max(radius+0.05, height/2)` 处做一个
//      半径 `radius` 的球形重叠。有任何刚体 → 贴着墙/塞在桌子里 → Blocked。
//      这是**近似**（球形而不是胶囊扫掠），代价是"离墙小于 0.35 m 的格子
//      会被判死"，正好是想要的保守方向。
//   ④ 都过了才记为 Walk，高度 = 命中点 z。
//
// ⚠ 为什么要 `overlapSphere` 而不是"再打四条水平射线看有没有墙"：
//   四条射线只能发现**正对着**的墙，斜着贴着的墙角会漏；而且四条射线
//   就是四倍的成本。球形重叠用的是已有的 broadphase，一次查询。
//   ⚠ 但它会把**地面上放的小道具**也当成障碍（一个 20 cm 的花盆让整格
//     不可走）。第一版接受这个：宁可"绕着走"也不要"穿过去"。
//
// ⚠ 场景里**没有物理世界**时（`PhysicsSettings::enabled = false`）这里
//   会烘出一张空格子网。调用方必须判 `stats.walk == 0` 并给出提示 ——
//   否则症状是"烘焙成功但所有代理一动不动"，日志里一行错都没有。
// ============================================================

#include "nav/NavGrid.h"

#include <string>

namespace physics {
class PhysicsWorld;
}

namespace nav {

struct NavBakeStats {
    int cols = 0;
    int rows = 0;
    int cells = 0;
    int walk = 0;
    int noGround = 0;    // 射线没命中
    int tooSteep = 0;    // 坡度超限
    int tooTight = 0;    // 代理体积被挡
    int rays = 0;        // 实际打了多少条射线（= cells，留作判据）
    double ms = 0.0;     // 耗时（毫秒，steady_clock 实测）
    // 是否可用：walk == 0 时调用方要打一条明确的 WARN。
    bool usable() const { return walk > 0; }
};

// `world` 为 nullptr / 空世界 → 返回 stats（walk=0），`out` 被清成一张
// 尺寸正确但全 Blocked 的网（**不是**保持旧网 —— 留着旧网会让"换场景后
// 还在用上一关的导航"这种 bug 静默存在）。
NavBakeStats bakeFromPhysics(const NavGridDesc& desc, const physics::PhysicsWorld* world,
                             NavGrid& out);

// 一行的可读摘要（面板状态栏 + 命令回显共用，避免两处各写一份格式）。
std::string bakeSummary(const NavBakeStats& s);

} // namespace nav
