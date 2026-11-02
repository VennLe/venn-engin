#pragma once
// ============================================================
// gameplay/CardTable —— 配表（CSV）→ `CardDef`（P3-1）
//
// ------------------------------------------------------------
// 为什么卡牌定义要走配表
// ------------------------------------------------------------
// `presetCardDefs()` 里的四张牌是**冒烟工具**，不是内容（同 `presetPoison`
// 的取舍）。真做游戏时，费用从 1 改成 2 这种事一天要改二十次，每次重新
// 编译一个 C++ 引擎是不可接受的 —— 所以卡牌定义必须落在 CSV 里。
//
// ------------------------------------------------------------
// 表格式（`assets/data/cards.csv`）
// ------------------------------------------------------------
//   id,#note,name,kind,target,cost,exhaust,unplayable,upgradedCost,type,text,effects
//
// 列名与 `CardDef` 的字段一一对应（`type` 在 `CardDef` 里不存在，见下）。
// `#` 前缀列是注释列，不参与解析（`DataTable` 的约定）。
//
// `effects` 用**紧凑串**而不是嵌套 JSON：CSV 里塞 JSON 要整格加引号、
// 里面再转义引号，策划在 Excel 里改一个数字会变成"改一格爆三格"。
// 格式是「多条效果用 `|` 分隔，每条是 `kind:target:amount:type:count`」：
//
//   damage::6:physical          → 6 点物理伤害，目标用卡自己的
//   status:enemy:2:vulnerable   → 给敌人挂 2 层易伤
//   draw::2                     → 抽 2 张（`target` 留空，位置不能省）
//   energy::1                   → +1 能量
//   damage:enemy:4:fire:3       → 4 点火焰伤害，重复 3 次
//
// ⚠ 位置不能省：`draw:2` 会被读成"目标叫 2"。`draw::2` 里中间那个空字段
//   才是"没有目标"，看着丑但它是**唯一**不会产生歧义的写法（按逗号分隔的
//   键值对更易读，可它让"第 3 条效果的第 2 个字段"在 3 个函数里各写一遍下标）。
//   ⚠ 别写成 `draw:::2`：那是 4 段 = kind/target/amount/type，`2` 会落到
//   **type** 上，amount 是空 → 抽 0 张（钳到 1）→ 症状是"这张牌只抽 1 张"。
//   自检 N3 钉的就是这一条。
//
// ------------------------------------------------------------
// 一条硬纪律：解析失败**不让整个表加载失败**
// ------------------------------------------------------------
// 一格写错的正确后果是"这一行被跳过 + 记一条带行号的 WARN"，而不是
// "整张表空掉"。理由是策划改表的节奏：改坏一格的下一秒他会去游戏里看
// 效果，此时若**所有**卡都变成未知卡，他会以为是"配表没生效"，
// 于是去查目录 / 查打包 / 重开编辑器 —— 而那格写错的提示早就沉在日志里了。
// ============================================================

#include "core/Csv.h"
#include "gameplay/CardZones.h"

#include <string>
#include <vector>

namespace gameplay {

// 一条效果串的解析（`damage::6:physical`）。`out` 被**追加**（不清空），
// 方便把多张牌的效果拼在一张表里测。
// 失败 → false + `err` 里带出错的片段（不抛）。
bool parseCardEffect(const std::string& text, CardEffect& out, std::string* err);
bool parseCardEffects(const std::string& text, std::vector<CardEffect>& out,
                      std::string* err);

// 反向：`CardEffect` 列表 → 紧凑串。**判据与面板用**：
// `parseCardEffects(formatCardEffects(x)) == x` 这条往返能钉住格式本身，
// 而不只是钉住"某张牌读出来是 6"。
std::string formatCardEffects(const std::vector<CardEffect>& fx);

// 一张 CSV 表 → 定义列表。坏行被跳过并记 `CARDTABLE` WARN（带行号）。
struct CardTableResult {
    std::vector<CardDef> defs;
    int skipped = 0;                  // 被跳过的行数
    std::vector<std::string> errors;  // 每行一条（`row 3: ...`）
    bool has(const std::string& id) const;
};
CardTableResult cardsFromCsv(const core::CsvTable& t);

// 默认列名（导出给自检 / 文档 / 探针用，避免"文档写 `kind`、代码写 `type`"）。
// 0..11，其中 11 是表尾哨兵（返回 ""）。真正的列是 0..10 共 11 列。
const char* cardColumnName(int index);
constexpr int kCardColumnCount = 12;   // 11 列 + 哨兵

// 自检 N 组：纯函数，不碰场景。
// 返回失败条数；`passOut` 非空时回填通过条数（`GameplaySelfTest` 要把 N 组
// 计入 GAMEPLAY-TOTAL —— 只计失败的话总数会对不上，而"总数对不上"会让
// 判据里的 `pass=N` 变成一个需要人工核对的数字）。
int runCardTableSelfTest(int* passOut = nullptr);

} // namespace gameplay
