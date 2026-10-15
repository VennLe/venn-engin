#pragma once
// ============================================================
// editor/CommandRegistry —— E18 命令注册表（可发现性的地基）
//
// 解决的问题（E18 之前的现状）：
//   同一个"动作"在**四个地方各写一遍**：
//     · Toolbar::drawMenuBar 的 MenuItem
//     · Toolbar::drawBar 的工具栏按钮
//     · EditorApp::handleShortcuts 的 `if (IsKeyPressed(...))`
//     · EditorApp::registerConsoleCommands 的控制台命令
//   于是"加了一个动作 → 忘了加到菜单"就变成"功能存在但用户找不到"，
//   而且没有任何机制能回答"这个编辑器一共有多少可执行动作、分别叫什么"。
//
// 本注册表把动作登记**一次**，然后派生：
//   · 命令面板（Ctrl+Shift+P）的模糊搜索候选
//   · E23 的快捷键表（shortcut 字段 + 冲突检测 + 可重绑定）
//   · Help ▸ Shortcut Reference 总览
//   · E21 的"这条通知来自哪个命令"归因
//
// ---- 三条设计约束 ----
//
// ① **只做登记与分发，不认识 UI**：CommandDesc 里没有 ImGui 类型，
//    handler 是 `std::function<void()>`。于是引擎侧/无窗口自动化也能注册。
//
// ② **可用性用谓词，不用两套表**：`when` 为空 = 恒可用；返回 false 时
//    面板里显示成灰色并**不可执行**（不是隐藏）—— "看得见但暂时不能用"
//    比"突然消失"更好理解，也是 UE5 命令面板的行为。
//
// ③ **不抢既有热路径**：注册表是"额外的一层索引"，既有菜单/快捷键
//    照旧直接调函数。E18 只把命令面板接到它上面；E23 再把快捷键分发
//    逐步搬过来（那一步才动 handleShortcuts 的主干）。
// ============================================================

#include <functional>
#include <string>
#include <vector>

namespace editor {

// ---------------------------------------------------------------- 命令描述

struct CommandDesc {
    std::string id;        // 稳定标识 "feature.toggle.ssr"（也给 E23 落盘用）
    std::string label;     // "Toggle SSR"（面板里显示 + 模糊搜索的主文本）
    std::string category;  // "Render" / "View" / "Edit" / "File" / "Console"
    std::string shortcut;  // "Ctrl+Shift+P"；空 = 无
    std::string keywords;  // 额外搜索词（空格分隔），如 "ssr reflection"
    // 可用性谓词。空 = 恒可用。返回 false → 面板里灰显、Enter 不执行。
    std::function<bool()> when;
    // 执行体。为空 = 占位（登记了但没有实现，面板里灰显）。
    std::function<void()> run;
};

// ---------------------------------------------------------------- 注册表

class CommandRegistry {
public:
    static CommandRegistry& get();

    // 重复 id 会**覆盖**前者（允许宿主热替换实现，与 ConsolePanel 一致）。
    void add(CommandDesc c);
    void clear();

    const std::vector<CommandDesc>& all() const { return m_cmds; }
    size_t size() const { return m_cmds.size(); }

    // 按 id 找；找不到返回 nullptr。
    const CommandDesc* find(const char* id) const;

    // 可用性：when 为空或返回 true；且 run 非空。
    static bool available(const CommandDesc& c);

    // 执行一条命令：不可用 / 未实现 → 返回 false（不静默）。
    // 成功时记 MRU 并打一条 `CMD-RUN` 日志（自动化断言用）。
    // via 是来源标签（"palette" / "shortcut" / "menu" / "console"）。
    bool run(const char* id, const char* via);

    // ---------------------------------------------------------- 模糊搜索
    // 返回按分数降序排列的候选（指针指向表内元素，勿长期持有）。
    //   query 为空 → MRU 置顶 + 其余按 category/label 排序
    //   query 非空 → 必须是（大小写不敏感的）**子序列**才算命中
    //   maxOut <= 0 → 不限
    std::vector<const CommandDesc*> search(const std::string& query,
                                           int maxOut = 0) const;

    // 无命中时的"最接近"建议（按 label 的 LCS 比例降序）。
    std::vector<const CommandDesc*> closest(const std::string& query,
                                            int maxOut = 5) const;

    // ---------------------------------------------------------- MRU
    static constexpr size_t kMaxRecent = 12;
    const std::vector<std::string>& recent() const { return m_recent; }
    void noteExecuted(const std::string& id);
    void clearRecent() { m_recent.clear(); }

    // 按 chord 精确匹配（"Ctrl+S"）。给 E23 的快捷键表用。
    const CommandDesc* findByShortcut(const char* chord) const;

private:
    std::vector<CommandDesc> m_cmds;
    std::vector<std::string> m_recent;  // 最新在前
};

// ---------------------------------------------------------------- 便捷注册
void registerCommand(CommandDesc c);

// ---------------------------------------------------------------- 模糊匹配

// 子序列打分（大小写不敏感）。needle 不是 hay 的子序列 → 0。
// 词首命中 / 连续命中 / 短 hay 给额外分。
int fuzzyScore(const char* needle, const char* hay);

// 最接近度量（0..1）：最长公共子序列长度 / max(len)。needle 为空 → 0。
float fuzzySimilarity(const char* a, const char* b);

// ---------------------------------------------------------------- 命令面板

// 画命令面板（Ctrl+Shift+P）。`*open` 为 false 时不画，返回 false。
// 面板自己维护查询串 / 选中项 / 输入历史；宿主只持有开合位。
// 返回本帧是否执行了一条命令。
//
// 自动化钩子（editor 侧注入一律 MYVK_EDITOR_ 前缀）：
//   MYVK_EDITOR_CMDPAL=<query>  打开面板并预填查询串（存在即开）
//   MYVK_EDITOR_CMDPAL_EXEC=1   打开后自动执行榜首（只执行一次）
bool drawCommandPalette(CommandRegistry& reg, bool* open);

}  // namespace editor
