#pragma once
// ============================================================
// runtime/GameFlags —— 剧情旗标（"我记得你救过我"那一层）
//
// ------------------------------------------------------------
// 为什么需要这张表
// ------------------------------------------------------------
// 二次元叙事游戏的骨架是**分支**：
//   第 1 章见了谁 → 第 3 章那句台词不一样 → 第 5 章走哪条线
// 而 `.vks` 的 `vars` 是**每实例私有**的：敌人 A 的脚本与 NPC B 的脚本
// 各有一份 `vars`，A 扣了血 B 看不见。分支需要的恰恰是"全世界共用的
// 一小撮状态" —— 这张表就是它。
//
// 三件东西的边界很清楚：
//   实例变量 vars   → 某个脚本自己的、跨帧记的东西（局部量）
//   GameFlags       → 整局游戏共享的、要进存档的东西（全局量）
//   组件            → 场景里的、要跟着实体走的东西（世界量）
//
// ------------------------------------------------------------
// 为什么是 double 而不是 bool
// ------------------------------------------------------------
// 一个玩家对某个角色的好感度是 0~100，不是 0/1。而"布尔型旗标"用
// `0 / 1` 表达就够了 —— 于是整张表只有一种值类型，比较、累加、
// 存档序列化都只有一条路径。`if (getFlag("met_alice"))` 天然成立
// （非 0 = 真），这正是脚本作者想要的写法。
//
// ⚠ **未设过 = 0**，不是"不存在"：`getFlag("从未出现过")` 返回 0 是
//   正确语义（第 1 章没遇见的人，好感度就是 0）。要区分"设成 0"与
//   "从没设过"用 `has()` —— 存档迁移会需要它，玩法逻辑几乎不需要。
//
// ------------------------------------------------------------
// 为什么住 runtime 而不是 engine
// ------------------------------------------------------------
// 它是**一局游戏**的概念，不是引擎的概念：引擎重启一局游戏就是新建
// 一张空表。engine/script 只认 `setFlag` / `getFlag` 两个 std::function
// （与 HUD / 音频同一条分层理由），具体存在哪由宿主决定 —— 于是将来
// 换成一个带脏标记、带自动存档的实现时，engine 一行都不用改。
// ============================================================

#include <cstddef>
#include <map>
#include <string>

namespace runtime {

class GameFlags {
public:
    // 写。同名覆盖（不累加 —— 累加请写成 `setFlag("x", getFlag("x")+1)`，
    // 让"加多少"留在脚本里可见）。
    void set(const std::string& name, double v) { m_v[name] = v; }

    // 读。未设过 = 0（见文件头）。
    double get(const std::string& name) const {
        auto it = m_v.find(name);
        return it == m_v.end() ? 0.0 : it->second;
    }

    // "这个旗标被设过吗"（与"值为 0"区分）
    bool has(const std::string& name) const {
        return m_v.find(name) != m_v.end();
    }

    void erase(const std::string& name) { m_v.erase(name); }

    // 新开一局 / 换场景**不**清 —— 换场景之后旗标当然要留着（那正是
    // 分支的用法）。只有"新游戏"才清，调用点必须显式写 `clear()`。
    void clear() { m_v.clear(); }

    std::size_t size() const { return m_v.size(); }
    bool empty() const { return m_v.empty(); }

    // 只读快照（存档 / Stats 面板 / 断言用）。
    const std::map<std::string, double>& entries() const { return m_v; }

private:
    std::map<std::string, double> m_v;
};

} // namespace runtime
