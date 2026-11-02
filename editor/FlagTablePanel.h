#pragma once
// ============================================================
// editor/FlagTablePanel —— 剧情旗标表（A4）
//
// 补的洞：`setFlag` / `getFlag` 从 10 月就在脚本里可用，而**编辑器里
// 一处入口都没有** —— 写了 20 个旗标之后，没有任何地方能看到"现在有
// 哪些旗标、值是多少、谁写的"。剧情调试只能靠 `print` 大法。
// 更糟的是编辑器 Play 态**根本没接** `svc.act.setFlag`（`verify` 里
// 那条 "not available in this host" 就是它），于是剧本里的分支在编辑器
// 里等于不存在 —— 只能出包才能试。
//
// 这一张表补的就是这两件事：一个**能看**、**能改**、**能回到某章开头**的
// 旗标视图，加上把编辑器 Play 接到同一份表上。
//
// ⚠ 面板**不拥用**数据：`scene::GameFlags` 住在 EditorApp（与运行时同
//   一个类），这里只 attach 一个指针。理由和 `PostVolumePanel` 一样 ——
//   面板关掉不该把游戏状态一起关掉。
// ============================================================

#include "scene/GameFlags.h"

#include <string>
#include <vector>

namespace editor {

class FlagTablePanel {
public:
    // EditorApp 构造时接上自己的旗标表（指针，不拥有）。
    void attach(scene::GameFlags* store) { m_store = store; }

    void draw();

    // ---- 命令入口（控制台 / 自动化）。⚠ 必须与按钮走**同一条路** ——
    // "能点不能存"是这一类面板最常见的缺陷。
    std::size_t count() const;
    double get(const std::string& name) const;
    bool set(const std::string& name, double v, const std::string& writer);
    bool erase(const std::string& name);
    std::size_t wipe();
    bool snapshot();
    std::size_t restore();
    bool hasSnapshot() const { return m_snap.valid; }
    // 过滤后的行（`filter` 按名字子串、不区分大小写）
    std::vector<scene::FlagRow> rows(const std::string& filter) const;
    std::string rowText(const scene::FlagRow& r) const;

private:
    bool visible(const std::string& name) const;

    scene::GameFlags* m_store = nullptr;
    scene::GameFlags::Snapshot m_snap;
    // 过滤框。⚠ 存的是**字节**（UTF-8），旗标名建议用 ASCII —
    // 但中文旗标名也能过滤，`icontains` 逐字节比就够。
    char m_filter[96] = {};
    bool m_onlyNonZero = false;
    std::string m_status;
};

} // namespace editor
