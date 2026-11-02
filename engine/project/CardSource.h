#pragma once
// ============================================================
// project/CardSource —— 配表 → `CardDef` 的宿主侧胶水（P3-1）
//
// 为什么单独一个文件：`gameplay/CardTable` 那一层**只认 `core::CsvTable`**
// （它连"文件在哪"都不该知道，否则 `--selftest gameplay` 就得依赖
// 资产目录）。而 `project` 这一层认识 `TableStore` 与 `assets/data/`。
// 这一层做的唯一一件事，就是把两个都"已知是对的"的东西接起来 ——
// 但它接得对不对，决定了**出包之后卡牌还认不认得出来**。
//
// ------------------------------------------------------------
// 三条纪律
// ------------------------------------------------------------
// 1) 表不存在 **不是错误**：新工程还没配卡表是正常状态。记一条 INFO、
//    返回 `tableFound=false`。报 ERROR 会让"还没开始配表"看起来像"配表坏了"。
//
// 2) 表存在但读坏了 **要能看出来**：`errors`/`skipped` 一路上抛，
//    编辑器面板直接显示 —— 不然"这张卡不生效"要靠逐行读日志才能发现。
//
// 3) 读出来的定义**必须灌进场景里所有牌堆**（`GameplayRuntime::setCardDefs`）。
//    只读不灌的话，牌堆里的每张牌都是 `known=0`：打出去什么也不发生，
//    而日志的一行错都没有。
// ============================================================

#include "gameplay/CardZones.h"

#include <string>
#include <vector>

namespace project {

struct CardSourceResult {
    bool tableFound = false;
    int defs = 0;       // 成功解析出的卡牌数
    int skipped = 0;    // 被跳过的行数（坏行 / 重复 id / id 带空格）
    int errors = 0;     // 错误条数（每条都有 `CARDTABLE` WARN 落了日志）
    std::string table;  // 表名（默认 `cards`）
    std::string dir;    // 扫描的目录（`assets/data` 的解析后绝对路径）
};

// 从 `assets/data/<tableName>.csv`（默认 `cards`）读卡牌定义。
// `out` 会被**整体替换**（不清空调用方的旧内容会造成"两次载入叠加"）。
CardSourceResult loadCardDefs(std::vector<gameplay::CardDef>& out,
                              const std::string& tableName = "cards");

} // namespace project
