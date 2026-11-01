// ============================================================
// scene/SaveSlot.cpp
// ============================================================

#include "scene/SaveSlot.h"

#include "core/Logger.h"
#include "scene/Scene.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

namespace scene {

namespace {

// 白名单。**顺序即 UI 顺序**：手动槽在位（1→3），然后是快捷 / 自动。
// ⚠ 加槽位只改这里；`isValidSaveSlot` / `listSaveSlots` / UI 下拉全都从它派生。
const std::vector<std::string>& names() {
    static const std::vector<std::string> k = {"slot1", "slot2", "slot3",
                                               "quick", "auto"};
    return k;
}

std::string rootOr(const std::string& root) {
    return root.empty() ? std::string(kSaveDirDefault) : root;
}

} // namespace

bool isValidSaveSlot(const std::string& slot) {
    const std::string n = normalizeSaveSlot(slot);
    if (n.empty()) return false;
    const auto& k = names();
    return std::find(k.begin(), k.end(), n) != k.end();
}

std::string normalizeSaveSlot(const std::string& slot) {
    std::size_t b = 0;
    std::size_t e = slot.size();
    while (b < e && std::isspace(static_cast<unsigned char>(slot[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(slot[e - 1]))) --e;
    std::string out;
    out.reserve(e - b);
    for (std::size_t i = b; i < e; ++i)
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(slot[i]))));
    return out;
}

const std::vector<std::string>& saveSlotNames() { return names(); }

std::string saveSlotPath(const std::string& root, const std::string& slot) {
    if (!isValidSaveSlot(slot)) return {};
    return rootOr(root) + "/" + normalizeSaveSlot(slot) + kSaveExt;
}

std::vector<SaveSlotInfo> listSaveSlots(const std::string& root) {
    std::vector<SaveSlotInfo> out;
    for (const std::string& s : names()) {
        SaveSlotInfo info;
        info.slot = s;
        info.path = saveSlotPath(root, s);
        std::error_code ec;
        info.exists = !info.path.empty() &&
                      std::filesystem::exists(std::filesystem::u8path(info.path), ec) &&
                      !ec;
        if (info.exists) {
            // 读一遍拿元信息。**读不动也算存在** —— "有这个文件但坏了"是
            // 玩家在存档界面上必须看到的一行，不能显示成"空槽"（那会让他
            // 以为档没了，然后存一次把好档覆盖掉）。
            SaveGame sg;
            SaveIoResult r = readSaveGame(info.path, sg);
            if (r.ok) {
                info.timestamp = sg.timestamp;
                info.scenePath = sg.scenePath;
                info.playTime = sg.playTime;
                info.entities = static_cast<int>(sg.entities.size());
            }
        }
        out.push_back(std::move(info));
    }
    return out;
}

SaveToSlotResult saveToSlot(const Scene& scene, SaveGameState& state,
                            const std::string& root, const std::string& slot,
                            const std::string& scenePath, double playTime,
                            ecs::EntityGuid playerGuid) {
    SaveToSlotResult r;
    if (!isValidSaveSlot(slot)) {
        r.error = "unknown save slot '" + slot + "'";
        VK_LOG_WARN("SAVE-WRITE-FAIL slot=%s reason=%s", slot.c_str(),
                    r.error.c_str());
        return r;
    }
    r.path = saveSlotPath(root, slot);

    // 目录不存在就建。⚠ 只建**父目录**，不碰已有文件。
    {
        std::error_code ec;
        const std::filesystem::path p = std::filesystem::u8path(r.path);
        if (p.has_parent_path())
            std::filesystem::create_directories(p.parent_path(), ec);
        if (ec) {
            r.error = "cannot create save directory: " + ec.message();
            VK_LOG_WARN("SAVE-WRITE-FAIL slot=%s path=%s reason=%s",
                        slot.c_str(), r.path.c_str(), r.error.c_str());
            return r;
        }
    }

    // 增量抓档：**基线永远来自 `state`**（`state` 空 = 全量，第一次存档就该
    // 是这个行为）。⚠ 不要在这里改成 `captureSaveGame(..., nullptr)` 走全量：
    //   "某一档忽然变大 10 倍" 是玩家能直接感觉到的（磁盘 + 写盘卡顿）。
    SaveGame sg =
        state.captureDiff(scene, normalizeSaveSlot(slot), scenePath, playTime);

    // globals：见头文件那条说明（这两样 `captureDiff` 拿不到）。
    // ⚠ 用 `std::map` 逐条拷（`SaveGlobals::vars` 就是有序的）—— 同一份档
    //   两次写出来的字节必须相同，否则"档没变"也会显得变了。
    sg.globals.vars.clear();
    for (const auto& kv : scene.scriptVars())
        sg.globals.vars[kv.first] = kv.second;
    if (playerGuid != ecs::kInvalidGuid) sg.globals.playerGuid = playerGuid;

    const SaveIoResult w = writeSaveGame(sg, r.path);
    if (!w.ok) {
        r.error = w.error;
        VK_LOG_WARN("SAVE-WRITE-FAIL slot=%s path=%s reason=%s",
                    slot.c_str(), r.path.c_str(), r.error.c_str());
        return r;
    }
    r.ok = true;
    r.entities = w.entities;
    r.destroyed = w.destroyed;
    // 锚点：验收脚本按这一行判"真的写了"。字段顺序也别随手改（有用例在解析）。
    VK_LOG_INFO("SAVE-WRITE slot=%s path=%s entities=%d destroyed=%d "
                "hash=%s playTime=%.1f scene=%s",
                normalizeSaveSlot(slot).c_str(), r.path.c_str(), w.entities,
                w.destroyed, sg.sceneHash.c_str(), sg.playTime,
                scenePath.c_str());
    return r;
}

LoadFromSlotResult loadFromSlot(Scene& scene, SaveGameState& state,
                                assets::AssetManager& assets,
                                const std::string& root,
                                const std::string& slot) {
    LoadFromSlotResult r;
    if (!isValidSaveSlot(slot)) {
        r.error = "unknown save slot '" + slot + "'";
        VK_LOG_WARN("SAVE-READ-FAIL slot=%s reason=%s", slot.c_str(),
                    r.error.c_str());
        return r;
    }
    r.path = saveSlotPath(root, slot);

    const SaveIoResult rd = readSaveGame(r.path, r.save);
    if (!rd.ok) {
        r.error = rd.error;
        VK_LOG_WARN("SAVE-READ-FAIL slot=%s path=%s reason=%s",
                    slot.c_str(), r.path.c_str(), r.error.c_str());
        return r;
    }
    r.migrated = rd.migrated;
    r.usedBak = rd.usedBak;

    // ⚠ 把 `state` 传进去：应用完顺手把基线刷成"刚读完档的样子"。不传的话
    //   下一次存档会把刚读进来的东西原样再写一遍 —— 不致命，但增量白做，
    //   而且档会越存越大。
    const SaveApplyResult ap = applySaveGame(scene, r.save, assets, &state);
    if (!ap.ok) {
        r.error = ap.error;
        VK_LOG_WARN("SAVE-READ-FAIL slot=%s path=%s reason=%s",
                    slot.c_str(), r.path.c_str(), r.error.c_str());
        return r;
    }
    r.ok = true;
    r.stats = ap.stats;
    VK_LOG_INFO("SAVE-READ slot=%s path=%s version=%d migrated=%d bak=%d "
                "applied=%d destroyed=%d missing=%d extra=%d orphans=%d "
                "drift=%d scene=%s",
                normalizeSaveSlot(slot).c_str(), r.path.c_str(),
                r.save.version, r.migrated, r.usedBak ? 1 : 0,
                ap.stats.applied, ap.stats.destroyed, ap.stats.missing,
                ap.stats.extra, ap.stats.orphans,
                ap.stats.driftRejected ? 1 : 0, r.save.scenePath.c_str());
    return r;
}

} // namespace scene
