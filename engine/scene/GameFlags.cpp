// engine/scene/GameFlags.cpp —— 旗标表的自检（G1~G10）
//
// 为什么自检在这里而不是在面板里：`GameFlags` 是**纯数据 + 纯语义**，
// 它的三条易错点（未设过 = 0 / 同名覆盖不累加 / 恢复是整表替换）都能
// 不启窗口、不开脚本引擎就验证。面板只负责显示，装配错了才是 UI 那条
// `verify_flags.py` 的活。
//
// ⚠ 输出格式与 `project::runDataTableSelfTest()` 一致：`FLAG-TEST PASS|FAIL <名字>`，
//   名字里不带空格、不带中文（自动化按行 grep）。

#include "scene/GameFlags.h"

#include <cstdio>
#include <string>

namespace scene {

int runGameFlagsSelfTest() {
    int pass = 0;
    int fail = 0;
    auto check = [&](bool ok, const char* name, const std::string& detail) {
        if (ok) {
            ++pass;
            std::printf("FLAG-TEST PASS %s\n", name);
        } else {
            ++fail;
            std::printf("FLAG-TEST FAIL %s (%s)\n", name, detail.c_str());
        }
    };

    // ---- G1 未设过 = 0，且 has() 能区分"设成 0" ----
    {
        GameFlags f;
        check(f.get("never") == 0.0, "G1 unset-is-zero",
              std::to_string(f.get("never")));
        check(!f.has("never"), "G1b unset-not-present",
              std::to_string(f.size()));
        f.set("zero", 0.0);
        check(f.has("zero") && f.get("zero") == 0.0, "G1c set-zero-is-present",
              std::to_string(f.size()));
    }

    // ---- G2 同名覆盖不累加 ----
    {
        GameFlags f;
        f.set("affection", 42);
        f.set("affection", 60);
        check(f.get("affection") == 60.0, "G2 overwrite-not-accumulate",
              std::to_string(f.get("affection")));
        check(f.size() == 1, "G2b one-key", std::to_string(f.size()));
    }

    // ---- G3 writer 记录**最后一次**写入者 ----
    //
    // ⚠ 这条容易写成"第一次"，因为 `operator[]` 只在键不存在时插入。
    //   写法是"取引用再赋值"，所以 value 与 writer 都会被覆盖 ——
    //   但只有断言过才敢说它对。
    {
        GameFlags f;
        f.set("met_alice", 1, "scripts/ch01.vks");
        f.set("met_alice", 1, "ui:ui/menu.ui.json");
        check(f.writerOf("met_alice") == "ui:ui/menu.ui.json",
              "G3 writer-last-writer", f.writerOf("met_alice"));
        check(f.writerOf("nobody").empty(), "G3b writer-unset-empty",
              f.writerOf("nobody"));
    }

    // ---- G4 erase / clear ----
    {
        GameFlags f;
        f.set("a", 1);
        f.set("b", 2);
        f.erase("a");
        check(!f.has("a") && f.has("b") && f.size() == 1, "G4 erase-one",
              std::to_string(f.size()));
        f.clear();
        check(f.empty() && f.get("b") == 0.0, "G4b clear-all",
              std::to_string(f.size()));
    }

    // ---- G5 rows() 按名字字典序（自动化断言的前提）----
    {
        GameFlags f;
        f.set("zeta", 3);
        f.set("alpha", 1);
        f.set("mike", 2);
        const auto rows = f.rows();
        const bool ordered = rows.size() == 3 && rows[0].name == "alpha" &&
                             rows[1].name == "mike" && rows[2].name == "zeta";
        check(ordered, "G5 rows-sorted",
              rows.size() == 3 ? rows[0].name + "," + rows[1].name + "," +
                                     rows[2].name
                               : std::to_string(rows.size()));
        check(!rows.empty() && rows[0].value == 1.0, "G5b rows-carry-value",
              std::to_string(rows.empty() ? 0.0 : rows[0].value));
    }

    // ---- G6 快照 / 恢复：值 + writer 都回来，新增键消失 ----
    {
        GameFlags f;
        f.set("ch01_done", 1, "scripts/ch01.vks");
        f.set("affection", 42, "scripts/ch01.vks");
        const GameFlags::Snapshot s = f.snap();
        f.set("affection", 10, "scripts/ch02.vks");
        f.set("ch03_started", 1, "scripts/ch03.vks");
        const std::size_t n = f.restore(s);
        check(n == 2, "G6 restore-count", std::to_string(n));
        check(f.get("affection") == 42.0, "G6b restore-value",
              std::to_string(f.get("affection")));
        check(f.writerOf("affection") == "scripts/ch01.vks",
              "G6c restore-writer", f.writerOf("affection"));
        // ⚠ 这是"重置到某章开头"的核心断言：那一章之后才出现的旗标
        //   必须消失。写成"保留"会让分支剧情带着未来的记忆。
        check(!f.has("ch03_started"), "G6d restore-drops-later-keys",
              std::to_string(f.size()));
    }

    // ---- G7 无效快照不动表 ----
    {
        GameFlags f;
        f.set("keep", 7);
        GameFlags::Snapshot bad;   // valid == false
        const std::size_t n = f.restore(bad);
        check(n == 0 && f.get("keep") == 7.0, "G7 invalid-snapshot-noop",
              std::to_string(n));
    }

    // ---- G8 flagKindName 三种分类 ----
    {
        check(std::string(flagKindName(1.0)) == "bool", "G8 kind-bool-zero",
              flagKindName(1.0));
        check(std::string(flagKindName(0.0)) == "bool", "G8b kind-bool-one",
              flagKindName(0.0));
        check(std::string(flagKindName(42.0)) == "int", "G8c kind-int",
              flagKindName(42.0));
        check(std::string(flagKindName(-7.0)) == "int", "G8d kind-int-neg",
              flagKindName(-7.0));
        check(std::string(flagKindName(2.5)) == "real", "G8e kind-real",
              flagKindName(2.5));
    }

    // ---- G9 覆盖写同一键不改变表大小（脚本每帧写同一旗标是常态）----
    {
        GameFlags f;
        for (int i = 0; i < 100; ++i) f.set("ticks", static_cast<double>(i));
        check(f.size() == 1 && f.get("ticks") == 99.0, "G9 repeated-set",
              std::to_string(f.size()) + "/" + std::to_string(f.get("ticks")));
    }

    // ---- G10 entries() 与 rows() 口径一致 ----
    {
        GameFlags f;
        f.set("b", 2, "x");
        f.set("a", 1, "y");
        std::size_t agree = 0;
        const auto rows = f.rows();
        std::size_t i = 0;
        bool ok = rows.size() == f.entries().size();
        for (const auto& kv : f.entries()) {
            if (i >= rows.size()) { ok = false; break; }
            if (rows[i].name == kv.first && rows[i].value == kv.second.value &&
                rows[i].writer == kv.second.writer)
                ++agree;
            ++i;
        }
        check(ok && agree == 2, "G10 entries-match-rows",
              std::to_string(agree));
    }

    std::printf("FLAG-TEST total pass=%d fail=%d\n", pass, fail);
    return fail;
}

} // namespace scene
