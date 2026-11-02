#pragma once

#include <string>
#include <utility>
#include <vector>

namespace i18n {

// 本地化编辑（C2）的模型层。
//
// ------------------------------------------------------------
// 它解决的是什么
// ------------------------------------------------------------
// 译文原先只有 `engine/i18n/ZhTable.cpp` 一张**编译进去**的静态表 ——
// 策划改一句翻译 = 改 C++ + 重编译。更糟的是：那张表在只读段、只有一份，
// 而"只有一份"这条纪律（见 I18n.h 头注释）是为了防两份漂移，不是为了
// 阻止**外部**覆盖。
//
// 所以这里走 `i18n::addTranslation`（本来就有、且"后写的覆盖"）：
//   · 静态表仍是英文原文的唯一权威；
//   · `assets/i18n/zh.csv` 是**覆盖层**，运行时可改、可存、可热重载；
//   · 覆盖层只在**有差异**时才写一行 —— 否则整张静态表会被复制进 CSV，
//     之后静态表再改就永远被旧 CSV 掩盖（这是"两份配置"最典型的下场）。
//
// ------------------------------------------------------------
// 落盘只写"覆盖"
// ------------------------------------------------------------
// `save()` 写出的是 `m_rows` 里**与内置值不同**的行 + CSV 里新加的键。
// 与 `project::saveGameFile` 的"合并而非重写"是同一条纪律的另一面：
// 那边防的是**丢掉别人的键**，这边防的是**固化出厂值**。
struct LocaleRow {
    std::string en;       // 英文原文（键）
    std::string zh;       // 当前译文（可为空 = 缺译文）
    bool builtin = false; // 是不是静态表里就有的
    bool dirty = false;   // 与内置值不同 / 是新增的 → 需要写进 CSV
};

class LocaleEditor {
public:
    // 读 `assets/i18n/zh.csv`（不存在 = 成功 + 只有内置行）。
    bool load(const std::string& path, std::string* err);
    // 只写覆盖行（见头注释）。
    bool save(std::string* err);

    const std::string& path() const { return m_path; }
    size_t count() const { return m_rows.size(); }
    const LocaleRow& row(size_t i) const { return m_rows[i]; }

    // 改一条。`en` 不存在时新增一行（builtin=false）。
    bool set(const std::string& en, const std::string& zh);
    // 撤销一条覆盖（回到内置值 / 删掉新增行）。
    bool revert(const std::string& en);

    size_t missingCount() const;
    size_t dirtyCount() const;

    // 把覆盖刷进 `i18n` 的运行时表。改完必须调（面板里是改完立刻调）。
    void apply();

    // 内置值（没有内置 = 空）。面板用来判断"改回去了没有"。
    std::string builtinOf(const std::string& en) const;

private:
    std::vector<LocaleRow> m_rows;
    std::string m_path;
};

// 自检（无窗口）：往返 / 只写覆盖 / 撤销 / 缺译文计数。L1~L5。
int runLocaleSelfTest();

} // namespace i18n
