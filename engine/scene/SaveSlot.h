#pragma once
// ============================================================
// scene/SaveSlot —— 槽位名 → 路径 → 一次完整的存 / 读动作（G17-3）
//
// 为什么要有这一层（而不是让调用方自己拼路径）：
//   · **白名单只有一份**。`slot1..3` / `quick` / `auto` 这组名字会同时出现在
//     CLI（`--slot`）、快捷键（F5/F9）、UI 下拉、以及将来的存档管理器里。
//     各写一遍 `if (slot == "quick")` 的话，加一个槽位要改四处，漏一处的症状
//     是"这个槽位在 UI 里能选、在这里报不存在"。
//   · **非法槽位绝不兜底**。`saveSlotPath("", "quik")` 返回**空串**，不是
//     "退回 quick"。静默把自动存档写进玩家手动槽位，比报错糟得多 ——
//     玩家的手动档被一个自己没按过的操作覆盖掉，而且没有任何日志。
//
// ------------------------------------------------------------
// 目录：相对 CWD 的 `saves/`
// ------------------------------------------------------------
// 与 `config/`（`editor_layout.json` / `editor_prefs.json` / `imgui.ini`）
// 同一个规矩：引擎里所有"用户状态"都相对 CWD，不写死盘符、不写 `%APPDATA%`。
// 这样"拷一个 build 目录到别的机器"仍然成立，验收脚本也能用
// `MYVK_RUNTIME_SAVE_DIR` 指到临时目录、互不干扰。
//
// ⚠ 引擎层**只接收 root**，不自己去读环境变量 —— 谁决定目录是调用方的事
//   （与 `SaveGame::writeSaveGame(path)` 不拼目录同一条）。
// ============================================================

#include "ecs/Entity.h"
#include "scene/SaveGame.h"
#include "scene/SaveGameState.h"

#include <string>
#include <vector>

namespace assets {
class AssetManager;
}
namespace scene {
class Scene;
}

namespace scene {

// 默认目录（相对 CWD）。
inline constexpr const char* kSaveDirDefault = "saves";
// 存档扩展名。`.vsave` 而不是 `.json`：**避免被资产扫描器当成资产**收进
// `game.pak`（G29 的引用闭包会把工程里所有 JSON 当候选）。
inline constexpr const char* kSaveExt = ".vsave";

// 槽位名白名单。**大小写不敏感**（"Quick" 与 "quick" 是同一个槽位）——
// 命令行与 UI 下拉两边的大小写习惯不一样，但落到磁盘上只能是同一个文件。
bool isValidSaveSlot(const std::string& slot);
// 规范化：去首尾空白 + 转小写。非法名原样返回（由调用方先判）。
std::string normalizeSaveSlot(const std::string& slot);
// 白名单顺序（UI 下拉与 `listSaveSlots` 都用它，保证顺序唯一）。
const std::vector<std::string>& saveSlotNames();

// slot → `<root>/<slot>.vsave`。`root` 空 → `kSaveDirDefault`。
// 非法槽位 → **空串**。
std::string saveSlotPath(const std::string& root, const std::string& slot);

// 一个槽位现在是什么状态（只读，**不创建目录**）。
struct SaveSlotInfo {
    std::string slot;
    std::string path;
    bool exists = false;
    std::string timestamp;
    std::string scenePath;
    double playTime = 0.0;
    int entities = 0;
};
// 按 `saveSlotNames()` 顺序返回全部槽位。坏档 → `exists=true` 但 `entities=0`
// （"有这个文件但读不动"本身就是要显示出来的信息）。
std::vector<SaveSlotInfo> listSaveSlots(const std::string& root);

struct SaveToSlotResult {
    bool ok = false;
    std::string error;
    std::string path;
    int entities = 0;
    int destroyed = 0;
};

// 抓档（增量，基线来自 `state`）+ 原子写 + 打 `SAVE-WRITE`。
// ⚠ `state` 非 const：抓完**不清基线**（下次存档还要靠它算增量）。
// ⚠⚠ 抓完**也不刷**基线 —— 增量是相对**关卡**（"基线 = 关卡加载完成"），
//   不是相对"上一份档"。改成相对上一份档的话，"存档 A → 什么都没动 →
//   存档 B"会写出一份**空档**（`entities: []`），而它看起来完全合法；
//   把空档读进新关卡 = 什么都没恢复。那是真正的丢档，且没有任何报错。
//   ← 这一条在 `--selftest save` 的 `slot_save_repeat_keeps_entry` 里钉着。
//
// `globals` 里有两样**只有调用方知道**、`captureDiff` 拿不到的东西，这里补上：
//   · `vars` ← `scene.scriptVars()`（脚本的 `let` / `setWorldAttr`）。
//     不补的话"读档后剧情旗标与脚本变量全丢"，而档里 `vars` 永远是 `{}`
//     —— 文件合法、读档成功、剧情从第一句重来。
//   · `playerGuid` ← 调用方给的玩家实体 guid（`kInvalidGuid` = 不知道/没有）。
//     ⚠ 玩家是**运行时生成**的、不进场景文件；不给的话读档后没人知道
//       "这条记录是我的角色"。
SaveToSlotResult saveToSlot(const Scene& scene, SaveGameState& state,
                            const std::string& root, const std::string& slot,
                            const std::string& scenePath, double playTime,
                            ecs::EntityGuid playerGuid = ecs::kInvalidGuid);

struct LoadFromSlotResult {
    bool ok = false;
    std::string error;
    std::string path;
    SaveGame save;
    SaveApplyStats stats;
    int migrated = 0;
    bool usedBak = false;
};

// 读档 + 应用 + 打 `SAVE-READ`。
// ⚠ 失败时**一个实体都不动**（`readSaveGame` 先校验；`applySaveGame` 里
//   漂移拒读也排在所有写入之前）。
LoadFromSlotResult loadFromSlot(Scene& scene, SaveGameState& state,
                                assets::AssetManager& assets,
                                const std::string& root, const std::string& slot);

} // namespace scene
