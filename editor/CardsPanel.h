#pragma once
// ============================================================
// editor/CardsPanel —— 牌堆操作台（Debug ▸ Cards，P3-3）
//
// 与 Inspector 的 Gameplay 页签（第 ⑤ 段）分工：
//   · 页签：**看**这个实体的牌堆现状（哪个区有什么牌），改一两手；
//   · 这个面板：**走完整流程**（挂牌组 → 造牌 → 洗 → 抽 → 打 → 结算 →
//     弃 → 收拾 → 存档往返），是判据的入口。
//
// 为什么单独一页而不是塞进 Gameplay 面板：卡牌流程有六个动词、五个区，
// 塞进去会把回合队列那页的半屏占掉，而"接下来谁先动"必须随时可见。
//
// ⚠ 面板**不持有场景**，只拿 `GameplayRuntime*`。所有动词都从公开方法走 ——
//   按钮与 `cards.*` 命令必须同一条路，否则就是"能点不能脚本化"，
//   而那种功能在判据里根本验不到。
// ============================================================

#include "ecs/Entity.h"

#include <string>

namespace scene {
class GameplayRuntime;
}

namespace editor {

class CardsPanel {
public:
    void attach(scene::GameplayRuntime* rt) { m_rt = rt; }
    void draw(ecs::Entity selected);

    // ---- 命令入口（与按钮同一条路）----
    // `cards=<draw,hand,discard,exhaust,limbo>` 一行；没有组件时 `state=none`。
    std::string stateLine(ecs::Entity e) const;
    // 每个区逐张列出（多行，`CARDS-PILE` 一条）。
    std::string detail(ecs::Entity e) const;
    std::string ensure(ecs::Entity e);
    // 载入**示例**定义表（`presetCardDefs()`，冒烟工具）。
    std::string loadPresetDefs(ecs::Entity e);
    // 从配表（`assets/data/cards.csv`）载入定义并灌进场景里所有牌堆（P3-1）。
    // ⚠ 与上面那条的区别：这是**内容**，出包之后游戏靠的就是它。
    std::string loadTableDefs(ecs::Entity e);
    // 打出并**结算效果**（打到自己身上：没有阵营概念之前，这是唯一能看见
    // 伤害数字/状态变化的用法，也是判据的入口）。
    std::string cast(ecs::Entity e, int idx);
    std::string addCards(ecs::Entity e, const std::string& defId, int n);
    // 造一副固定的测试卡组并洗牌：5 打击 / 4 防御 / 1 重击 / 1 黏液。
    // 固定内容 + 固定顺序 = 判据能算准"抽 5 张里必然有什么"。
    std::string buildTestDeck(ecs::Entity e);
    std::string drawCards(ecs::Entity e, int n);
    std::string play(ecs::Entity e, int idx);
    std::string resolveLast(ecs::Entity e);
    std::string discardHand(ecs::Entity e);
    std::string shuffle(ecs::Entity e);
    // 收拾 Limbo（"结算到一半"的牌落地）。
    std::string sweep(ecs::Entity e);
    // 文本行往返：`ok=1 same=1` 说明牌堆能被完整存回来。
    std::string roundTrip(ecs::Entity e);
    // 清空所有区（保留定义表与组件）。组件**删不掉**（World 没有移除组件的
    // API），所以"清空"就是这里能做到的最彻底的复位。
    std::string reset(ecs::Entity e);

private:
    scene::GameplayRuntime* m_rt = nullptr;
    std::string m_status;
    int m_drawCount = 5;
    int m_addCount = 1;
    char m_addDef[32] = "strike";
};

} // namespace editor
