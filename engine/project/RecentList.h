#pragma once
// ============================================================
// project/RecentList —— 启动器的"最近工程"列表
//
// 落在 `%LOCALAPPDATA%/Venn/launcher/recent.json`（**用户级**，不属于任何
// 工程）—— 与运行时日志同根（`runtime/RuntimeLog.cpp` 的 baseDir 同一套
// 回退链：LOCALAPPDATA → APPDATA → TEMP → "."）。
//
// 为什么是用户级而不是工程级：换台机器打开同一个工程，看见的应该是各自的
// 最近列表（与 `editor_layout.json` 的"最近场景"同一套思路，但那一份是
// **编辑器状态**，这一份是**启动器状态**，两者不共享）。
//
// ------------------------------------------------------------
// 三条硬规则
// ------------------------------------------------------------
// ① **坏文件不崩**。json 语法错 / 顶层不是对象 / items 不是数组 → 当成空
//    列表 + 一条 WARN。启动器是用户双击的第一个窗口，它不该因为一个
//    手改坏的 json 就打不开。
//
// ② **死路径降级，不弹模态**（路线图 P11）。`pruneMissing()` 把目录已经
//    不在的条目剔掉；UI 侧"点开失败"走另一条路（提示 + 移除），不弹窗。
//
// ③ **上限 10 条**，按 `lastOpenedAt` 倒序。超出就丢最旧的 —— 列表是
//    "最近"，不是"全部"；全量是文件系统的事。
// ============================================================

#include "project/ProjectFile.h"

#include <string>
#include <vector>

namespace project {

constexpr int kRecentMaxItems = 10;

struct RecentItem {
    std::string name;
    std::string path;        // 工程根（绝对路径、正斜杠）
    GameType gameType = GameType::ThirdPerson;
    std::string lastOpenedAt;   // ISO8601 UTC
};

// `MYVK_LAUNCHER_RECENT` > `%LOCALAPPDATA%/Venn/launcher/recent.json`
// （前者给自动化用：回归不该污染用户真实列表）
std::string recentListPath();

class RecentList {
public:
    // 读。文件不存在 = 空列表，**不是错误**（返回 true）。
    // 文件坏 = 空列表 + WARN + 返回 false（调用方照常继续）。
    bool load(std::string* err = nullptr);

    // 写。会建父目录。失败只 WARN。
    bool save(std::string* err = nullptr) const;

    // 把一个刚打开/刚创建的工程移到最前（同路径旧条目被替换，不是重复）。
    void touch(const ProjectFile& p);

    // 按路径移除（大小写不敏感 —— Windows 路径）。
    bool remove(const std::string& path);

    // 剔掉"目录已经不存在 / 没有 venn.project.json"的条目。
    // 返回剔除条数。⚠ 只在**用户可见的动作**里调（启动时 + 点开失败），
    // 不要每帧调 —— 那是 O(n) 次 stat。
    int pruneMissing();

    const std::vector<RecentItem>& items() const { return m_items; }
    bool empty() const { return m_items.empty(); }
    std::size_t size() const { return m_items.size(); }

    void clear() { m_items.clear(); }

private:
    std::vector<RecentItem> m_items;
};

// ---------------------------------------------------------------- 自检
//
// 无窗口 / 无 Vulkan（`--selftest project` 的 I 组）。
// 覆盖：往返 / 上限 / 去重 / 大小写 / 死路径剔除 / 坏文件不崩。
int runRecentListSelfTest();

} // namespace project
