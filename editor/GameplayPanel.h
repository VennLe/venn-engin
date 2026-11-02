#pragma once
// ============================================================
// editor/GameplayPanel —— 玩法面板（Debug ▸ Gameplay，P2）
//
// 与 Inspector 的 Gameplay 页签**分工明确**：
//   · 页签管**一个实体**（属性白值 / 状态 / 伤害预览）；
//   · 这个面板管**整个遭遇**（回合队列 / 推进 / 回合末结算 / 随机种子）。
//
// 为什么回合队列必须单独一页：它是**跨实体**的东西。放在某个实体的页签里
// 就要先选中它才能看到"接下来谁先动"，而"接下来谁先动"恰恰是最该随时
// 可见的一条信息（回合制里所有的策略都建立在它上面）。
//
// ⚠ 面板**不持有**场景，只拿一个 `GameplayRuntime*`（由 EditorApp 创建并
//   跨越 Play 模式存活）。所有动词（advance / tick / setSeed）都从面板的
//   公开方法走 —— 按钮与命令必须同一条路，否则"能点不能脚本化"。
// ============================================================

#include "ecs/Entity.h"

#include <string>

namespace scene {
class GameplayRuntime;
}

namespace editor {

class GameplayPanel {
public:
    void attach(scene::GameplayRuntime* rt) { m_rt = rt; }
    void draw(ecs::Entity selected);

    // ---- 命令入口（与按钮同一条路）----
    std::string stateLine() const;
    std::string orderLine(int n) const;
    std::string advance(float dt);
    std::string endTurn();
    std::string tickRound();
    std::string rebuild();
    bool setSeed(unsigned long long seed, std::string* err);
    // 队列文本的往返（判据读它：`readback=1` 说明顺序完全一致）。
    std::string roundTrip();
    // 给选中实体加一个预设状态（`poison` / `shield` / `vuln`）。
    std::string addPreset(ecs::Entity e, const std::string& which);
    // 打一下（用选中实体当攻击方与目标，验证明细链路）。
    std::string hit(ecs::Entity from, ecs::Entity to, float amount,
                    const std::string& type);

private:
    scene::GameplayRuntime* m_rt = nullptr;
    std::string m_status;
    int m_seedBuf = 20261103;
    float m_advanceDt = 1.0f;
    float m_hitAmount = 50.0f;
    int m_hitType = 0;
};

} // namespace editor
