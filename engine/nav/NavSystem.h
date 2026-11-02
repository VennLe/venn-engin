#pragma once
// ============================================================
// nav/NavSystem —— 代理沿路径移动（C8-3）
//
// ------------------------------------------------------------
// 它做什么（只有三件）
// ------------------------------------------------------------
//   ① 持有一张 `NavGrid`（全局一份；换场景才重烘）
//   ② 给代理求路（`gotoPoint`，带 `repathInterval` 防抖）
//   ③ 每帧把代理往下一个路点推 `speed*dt` 米，到点就换下一个
//
// 它**不**做决策：打谁、什么时候去、去哪，全部由脚本 / 行为树 / 关卡
// 作者设的 `goal` 决定。把决策塞进来的那一刻，这套东西就变成"引擎里的
// 一个 AI"，而引擎应该有 AI 的**组件**，不是 AI。
//
// ------------------------------------------------------------
// 两条移动路（有物理胶囊 / 没有）
// ------------------------------------------------------------
//   · 实体带 `CharacterControllerComponent` → 只写**期望速度**，位移由
//     物理那套做（台阶、斜坡、贴地都是它的事）。
//   · 不带 → 直接写 `TransformComponent.position`（kinematic），高度贴
//     网格。巡逻兵、棋盘单位、飞行道具走这条，够用且确定（不依赖物理
//     世界的开关）。
//
// ⚠ 两条路都**不**做"绕开动态障碍"（别的角色站着不动它也照穿）。
//   第一版非目标，见 docs/navigation.md §7。真需要的话是"局部避障"，
//   与全局寻路是两件事。
// ============================================================

#include "ecs/Entity.h"
#include "nav/NavGrid.h"

#include <cstddef>
#include <vector>

namespace scene {
class Scene;
}

namespace nav {

class NavSystem {
public:
    struct Stats {
        int agents = 0;     // 场景里带 NavAgentComponent 的实体数
        int moving = 0;     // 这一帧真的往前走了一点的
        int arrived = 0;    // 这一帧到达（reached = true）的
        int failed = 0;     // 这一帧求路失败 / 没网格
        int expanded = 0;   // 这一帧累计 A* 展开节点数（性能判据）
    };

    // 换网格（一次性把整张拷进来）。`!g.valid()` 等价于 clearGrid()。
    void setGrid(NavGrid g);
    void clearGrid();
    bool hasGrid() const { return m_has; }
    const NavGrid& grid() const { return m_grid; }
    const Stats& stats() const { return m_stats; }

    // 给代理指定目标并立即求一次路。
    // 返回 false：实体没有代理组件 / 没有网格 / 求路失败（`agent.failed` 置位）。
    bool gotoPoint(scene::Scene& s, ecs::Entity e, const glm::vec3& goal);
    // 停住并清目标（路径、next 一起清）。对没有代理组件的实体是空操作。
    void stop(scene::Scene& s, ecs::Entity e);
    // 清掉所有代理的运行态（换场景 / 重开时调）。
    void resetAgents(scene::Scene& s);

    // 每帧调用。`dt <= 0` 直接返回 0（暂停时不该有人偷偷走动）。
    // 返回**移动过的代理数**。
    int tick(scene::Scene& s, float dt);

private:
    NavGrid m_grid;
    bool m_has = false;
    Stats m_stats;
};

// 角度插值（最短弧）。公开是为了让用例能直接钉住它：
// "从 170° 转到 -170°"走 20° 还是 340°，决定了角色会不会原地横扫一圈。
float approachAngle(float cur, float target, float t);

} // namespace nav
