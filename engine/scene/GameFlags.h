#pragma once
// ============================================================
// engine/scene/GameFlags —— 剧情旗标（"我记得你救过我"那一层）
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
// 为什么住 engine/scene 而不是 runtime
// ------------------------------------------------------------
// 它是**一局游戏**的概念，不是引擎的概念：引擎重启一局游戏就是新建
// 一张空表。engine/script 只认 `setFlag` / `getFlag` 两个 std::function
// —— 具体存在哪由宿主决定。
//
// ⚠ **2026-11-02 从 `runtime/GameFlags.h` 搬到这里**。原因不是"想公开"：
//   那份 class 只有一个宿主在用（Game.exe），而**编辑器 Play 态也是宿主**
//   —— 它此前根本没接 `svc.act.setFlag`，于是 Play 里 `setFlag("x",1)`
//   直接报 "not available in this host"，剧本里的分支在编辑器里等于
//   不存在。而 `editor/` 分层上**不许** include `runtime/`（见
//   CMakeLists 的依赖图）。所以类必须落在两边都能看见的 `engine/` 里。
//   `runtime/GameFlags.h` 现在只是一行 `using` 别名。
//
// ------------------------------------------------------------
// 为什么每个键记一个 "writer"
// ------------------------------------------------------------
// 调试一张几十项的旗标表时，"这个值是谁写的"是最常问的问题，而它
// **只能**在写入那一刻知道（事后推断不出来）。所以 `set()` 多收一个
// 来源串：`"scripts/door.vks"` / `"ui:ui/menu.ui.json"` / `"console"`。
//
// ⚠ writer **不参与**存档：它是调试信息，写进存档只会让文件变大、
//   并在"读档后全部显示成上一局的某个脚本"上骗人。
// ============================================================

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace scene {

// 一项旗标：值 + 来源（见文件头"为什么每个键记一个 writer"）。
struct FlagEntry {
    double value = 0.0;
    std::string writer;
};

// 一行旗标的扁平视图（面板的表格 / 控制台的 `flags.list` 共用）。
struct FlagRow {
    std::string name;
    double value = 0.0;
    std::string writer;
};

// 值的"类型"是**看出来的**，表里没有类型位 —— 与配表（`project::DataTable`
// 没有类型行）同一条理由：说"类型是 int、值是 abc"时到底信谁说不清。
// 这里可能的取值只有三种，按值反推不会歧义：
//   0 / 1   → "bool"（布尔旗标就是 0/1，见文件头）
//   整数值  → "int"
//   其余    → "real"
// ⚠ 它只用于**显示**。任何逻辑判断都该用数值本身，不要按这个串分支。
inline const char* flagKindName(double v) {
    if (v == 0.0 || v == 1.0) return "bool";
    if (v == static_cast<double>(static_cast<long long>(v))) return "int";
    return "real";
}

class GameFlags {
public:
    // ---- 快照（"重置到某章开头"）----
    //
    // 为什么是快照而不是"删掉某些键"：旗标之间**没有**"哪几个属于第 2 章"
    // 的关系，靠名字前缀分组是猜的。快照是唯一不需要作者额外标注、
    // 而且一定能回到原样的做法。
    struct Snapshot {
        std::map<std::string, FlagEntry> v;
        bool valid = false;
    };

    // 写。同名覆盖（不累加 —— 累加请写成 `setFlag("x", getFlag("x")+1)`，
    // 让"加多少"留在脚本里可见）。
    // `writer` 见文件头：只在写入这一刻知道，所以必须由调用方给。
    void set(const std::string& name, double v, const std::string& writer = {}) {
        FlagEntry& e = m_v[name];
        e.value = v;
        e.writer = writer;
    }

    // 读。未设过 = 0（见文件头）。
    double get(const std::string& name) const {
        auto it = m_v.find(name);
        return it == m_v.end() ? 0.0 : it->second.value;
    }

    // "这个旗标被设过吗"（与"值为 0"区分）
    bool has(const std::string& name) const {
        return m_v.find(name) != m_v.end();
    }

    // 谁最后一次写的（未设过 → 空串）
    std::string writerOf(const std::string& name) const {
        auto it = m_v.find(name);
        return it == m_v.end() ? std::string() : it->second.writer;
    }

    void erase(const std::string& name) { m_v.erase(name); }

    // 新开一局 / 换场景**不**清 —— 换场景之后旗标当然要留着（那正是
    // 分支的用法）。只有"新游戏"才清，调用点必须显式写 `clear()`。
    void clear() { m_v.clear(); }

    std::size_t size() const { return m_v.size(); }
    bool empty() const { return m_v.empty(); }

    // 只读快照（存档 / 断言用）。`std::map` 本来就是有序的 → 遍历顺序
    // 按名字字典序，自动化断言不会随机红。
    const std::map<std::string, FlagEntry>& entries() const { return m_v; }

    // 三列扁平视图（面板的表格 / 控制台的 `flags.list` 都用它）。
    std::vector<FlagRow> rows() const {
        std::vector<FlagRow> out;
        out.reserve(m_v.size());
        for (const auto& kv : m_v) {
            FlagRow r;
            r.name = kv.first;
            r.value = kv.second.value;
            r.writer = kv.second.writer;
            out.push_back(std::move(r));
        }
        return out;
    }

    Snapshot snap() const {
        Snapshot s;
        s.v = m_v;
        s.valid = true;
        return s;
    }

    // 整表替换。返回恢复了多少项（无效快照 → 0，不动表）。
    // ⚠ 快照之后**新增**的键会消失 —— 这正是"回到某章开头"要的语义
    //   （那一章之后才出现的旗标不该还在）。
    std::size_t restore(const Snapshot& s) {
        if (!s.valid) return 0;
        m_v = s.v;
        return m_v.size();
    }

private:
    std::map<std::string, FlagEntry> m_v;
};

// 自检（G1~G10）。返回失败条数 —— 与 `project::runAllProjectSelfTests()`
// 同一个口径（0 = 全过）。
int runGameFlagsSelfTest();

} // namespace scene
