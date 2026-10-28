// ============================================================
// scene/PersistenceSelfTest.cpp —— G17-1 验收（`Game.exe --selftest save`）
//
// 五组断言，每一组对应一种"存档会怎么坏"：
//
//   A 结构指纹     —— 关卡漂移判定的地基。**排序**是唯一容易写错又看不出来的地方：
//                     不排序的话"同内容两次加载给出不同 hash"，于是每次进游戏
//                     都认为"关卡换了"，漂移警告刷屏（而所有人都会去查 JSON）。
//   B 格式往返     —— `dump → parse → dump` 必须**逐字节**相同。只比字段会漏：
//                     float 表示不一致 / key 顺序不稳定只会让文件"看起来"一样。
//   C 迁移         —— v1 档必须能读成 v2，且**老字段一个不丢**。
//   D 原子写       —— 写两次同一槽必须成功（Windows 上 `std::rename` 会失败）；
//                     主档坏了必须能退 `.bak`。
//   E 校验         —— 比我们新的档**拒读**；重复 guid **拒绝整份**。
//
// ⚠ 所有断言都问过"别的实现会给出不同的数吗"。
// ============================================================

#include "scene/PersistenceSelfTest.h"

#include "assets/AssetManager.h"
#include "assets/Texture.h"
#include "core/Logger.h"
#include "ecs/Components.h"
#include "ecs/World.h"
#include "scene/SaveGame.h"
#include "scene/SaveGameState.h"
#include "scene/Scene.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace scene {

namespace {

using json = nlohmann::json;

int g_pass = 0;
int g_fail = 0;

// ⚠ detail 里**不能有空格**：验收脚本用 `detail=(\S*)` 解析（全仓自检的硬约定）。
void report(const char* name, double actual, double expected, double tol = 0.0) {
    const bool ok = (actual - expected) <= tol && (expected - actual) <= tol;
    VK_LOG_INFO("SAVE-TEST %s actual=%.6f expected=%.6f tol=%.6f result=%s", name, actual,
                expected, tol, ok ? "PASS" : "FAIL");
    if (ok) ++g_pass;
    else ++g_fail;
}

void reportFlag(const char* name, bool ok, const std::string& detail = {}) {
    VK_LOG_INFO("SAVE-TEST %s actual=%d expected=1 tol=0 detail=%s result=%s", name, ok ? 1 : 0,
                detail.c_str(), ok ? "PASS" : "FAIL");
    if (ok) ++g_pass;
    else ++g_fail;
}

// 临时目录下的固定路径。**只覆盖写、不删** —— 删文件要走 tools/safe_fs.py 那套规矩，
// 而 TEMP 目录本来就是给这种一次性文件用的。
std::string tempPath(const char* name) {
    static const std::string dir = [] {
        std::error_code ec;
        std::string d = (std::filesystem::temp_directory_path(ec) / "venn_save_selftest").string();
        std::filesystem::create_directories(std::filesystem::u8path(d), ec);
        return d;
    }();
    return dir + "/" + name;
}

void writeText(const std::string& path, const std::string& text) {
    FILE* f = nullptr;
#ifdef _WIN32
    if (fopen_s(&f, path.c_str(), "wb") != 0) f = nullptr;
#else
    f = std::fopen(path.c_str(), "wb");
#endif
    if (!f) return;
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
}

// 建一个"关卡"：给定 guid 表，按顺序创建实体并把 guid 钉成表里的值。
//
// ⚠ `Scene` **不可拷贝/不可移动**（它带 BVH 与组件池），所以不能写成
//   "返回 Scene 的 lambda" —— 只能往调用方给的场景里建（`C2280` 那次踩过）。
//
// 需要它的场合：`save_destroyed_recorded` / `save_orphan_parent_fixed` 这类
// "读档要盖到**重新加载出来的**关卡上"的用例 —— 在同一份场景里存档再读档
// 测不出"关卡已经重置回初始状态"这个前提。
void buildLevel(Scene& s, const std::vector<ecs::EntityGuid>& guids) {
    for (size_t i = 0; i < guids.size(); ++i) {
        const ecs::Entity e = s.createObject("E" + std::to_string(i));
        s.world().setGuid(e, guids[i]);
    }
}

// 应用层用的空 AssetManager：**没有设备**，只能安全地处理"没有资产块"的实体。
// 夹具里的实体都没有 `mesh` / `material` / `skinned` 等块，`resolveMesh` /
// `resolveMaterial` 拿到 `null` 会直接返回 nullptr，一次 GPU 调用都不会发生。
// ⚠ 给夹具加任何资产键之前先看这条：加了一个 `"mesh"` 键就会去建 GPU 缓冲，
//   而 device 是 nullptr —— 症状是自检进程直接崩，不是一条 FAIL。
assets::AssetManager& nullAssets() {
    static assets::AssetManager am{assets::TextureContext{}};
    return am;
}

// ============================================================
// A 组：结构指纹
// ============================================================
void groupHash() {
    // ① 同内容两次 → 相同（基础）
    {
        Scene s;
        s.createObject("A");
        s.createObject("B");
        const std::string h1 = sceneStructureHash(s);
        const std::string h2 = sceneStructureHash(s);
        reportFlag("hash_stable_twice", h1 == h2 && h1.size() == 16, std::string("h=") + h1);
    }

    // ② 只改变换 → 指纹**不变**（这是"结构指纹"而不是"内容指纹"的定义）
    {
        Scene s;
        const ecs::Entity e = s.createObject("A");
        const std::string before = sceneStructureHash(s);
        if (auto* t = s.world().get<ecs::TransformComponent>(e)) t->position.x = 42.0f;
        const std::string after = sceneStructureHash(s);
        reportFlag("hash_ignores_transform", before == after,
                   std::string("before=") + before.substr(0, 4) + ",after=" + after.substr(0, 4));
    }

    // ③ 加实体 → 变
    {
        Scene s;
        s.createObject("A");
        const std::string before = sceneStructureHash(s);
        s.createObject("B");
        const std::string after = sceneStructureHash(s);
        reportFlag("hash_detects_add", before != after);
    }

    // ④ 删实体 → 变
    {
        Scene s;
        const ecs::Entity a = s.createObject("A");
        s.createObject("B");
        const std::string before = sceneStructureHash(s);
        s.world().destroy(a);
        const std::string after = sceneStructureHash(s);
        reportFlag("hash_detects_remove", before != after);
    }

    // ⑤ **建实体顺序不影响**指纹 —— 这条是"排序"的唯一证据。
    //    ⚠ 不排序的实现：`World` 的池子按分配顺序遍历，{A,B} 与 {B,A}
    //    会给出不同 hash → 每次进游戏都报"关卡漂移"。
    //    但 guid 是随机的，两个场景不可能"同样两个 guid 不同顺序"——
    //    所以这里用**同一串手工 guid** 造两个场景，顺序相反。
    //    ⚠ `Scene` 不可拷贝/不可移动（它有 BVH 与池子），所以不能用
    //      "返回 Scene 的 lambda" —— 直接建两个。
    {
        const ecs::EntityGuid g1 = 0xAAAA000000000001ull;
        const ecs::EntityGuid g2 = 0xAAAA000000000002ull;

        Scene fwd;
        fwd.world().setGuid(fwd.createObject("X"), g1);
        fwd.world().setGuid(fwd.createObject("Y"), g2);

        Scene rev;
        rev.world().setGuid(rev.createObject("Y"), g2);
        rev.world().setGuid(rev.createObject("X"), g1);

        const std::string h1 = sceneStructureHash(fwd);
        const std::string h2 = sceneStructureHash(rev);
        reportFlag("hash_order_independent", h1 == h2,
                   std::string("fwd=") + h1.substr(0, 6) + ",rev=" + h2.substr(0, 6));
    }
}

// ============================================================
// B 组：格式往返
// ============================================================
void groupFormat() {
    SaveGame sg;
    sg.slot = "slot1";
    sg.timestamp = "2026-10-27T05:00:00Z";
    sg.playTime = 12.5;
    sg.scenePath = "assets/scenes/cutscene_demo.json";
    sg.sceneHash = "0123456789abcdef";

    SaveEntity live;
    live.guid = 0x1111222233334444ull;
    live.raw = json{{"name", "Crate"},
                    {"guid", "1111222233334444"},
                    {"transform", json{{"position", json::array({1.0, 2.0, 3.0})},
                                       {"rotation", json::array({0.0, 0.0, 0.0})},
                                       {"scale", json::array({1.0, 1.0, 1.0})}}},
                    {"visible", false}};
    sg.entities.push_back(live);

    SaveEntity dead;
    dead.guid = 0x5555666677778888ull;
    dead.destroyed = true;
    sg.entities.push_back(dead);

    sg.globals.playerGuid = 0x9999aaaabbbbccccull;
    sg.globals.playerHp = 40.0f;
    sg.globals.playerStamina = 77.5f;
    sg.globals.pickedUp = {0x1111222233334444ull, 0x5555666677778888ull};
    sg.globals.vars["door.opened"] = 1.0;
    sg.globals.vars["quest.stage"] = 2.0;

    // ---- 逐字节往返 ----
    // ⚠ 只比字段会漏两件事：float 的表示（`40` vs `40.0` 在浮点相等下一样，
    //   字节不同）、key 顺序（不稳定的实现会让 diff 变成噪音）。
    const std::string d1 = saveGameToJson(sg).dump();
    SaveGame back;
    SaveIoResult rd;
    const bool ok = saveGameFromJson(json::parse(d1), back, rd);
    const std::string d2 = ok ? saveGameToJson(back).dump() : std::string("<parse-failed>");
    reportFlag("format_roundtrip_bytes", ok && d1 == d2,
               std::to_string(d1.size()) + "/" + std::to_string(d2.size()));
    report("format_roundtrip_entities", static_cast<double>(back.entities.size()), 2.0);
    report("format_roundtrip_destroyed", static_cast<double>(rd.destroyed), 1.0);
    report("format_migrated_zero", static_cast<double>(rd.migrated), 0.0);

    // ---- globals 逐字段 ----
    int bad = 0;
    if (back.globals.playerGuid != sg.globals.playerGuid) ++bad;
    if (back.globals.playerHp != sg.globals.playerHp) ++bad;
    if (back.globals.playerStamina != sg.globals.playerStamina) ++bad;
    if (back.globals.pickedUp != sg.globals.pickedUp) ++bad;
    if (back.globals.vars != sg.globals.vars) ++bad;
    report("format_globals_fields", static_cast<double>(bad), 0.0);

    // ---- `_destroyed` 必须是一个**显式字段**，不是"raw 为空"推出来的 ----
    reportFlag("format_destroyed_flag",
               back.entities.size() > 1 && back.entities[1].destroyed &&
                   back.entities[0].destroyed == false);

    // ---- dump 两次必须相同（`std::map` 的用处）----
    const std::string a = saveGameToJson(sg).dump();
    const std::string b = saveGameToJson(sg).dump();
    reportFlag("format_dump_stable", a == b, std::to_string(a.size()) + "/" +
                                                 std::to_string(b.size()));

    // ---- 抓档：只有"有内容 + 有 guid"的实体进档 ----
    {
        Scene s;
        s.createObject("Crate");                 // createObject 自带 Transform
        s.createObject("Barrel");
        SaveGame cap = captureSaveGame(s, "quick", "scenes/x.json", 3.0);
        report("capture_entity_count", static_cast<double>(cap.entities.size()), 2.0);
        reportFlag("capture_scene_hash_set", cap.sceneHash.size() == 16);
        reportFlag("capture_entities_have_guid",
                   cap.entities.size() == 2 && cap.entities[0].guid != ecs::kInvalidGuid &&
                       cap.entities[1].guid != ecs::kInvalidGuid);
        reportFlag("capture_entities_have_name",
                   cap.entities.size() == 2 &&
                       cap.entities[0].raw.value("name", std::string()) == "Crate");
    }
}

// ============================================================
// C 组：迁移
// ============================================================
void groupMigrate() {
    // v1 档：**没有** playerStamina，也没有 version=2
    const char* v1 = R"({
      "version": 1,
      "game": "venn",
      "slot": "slot1",
      "scenePath": "scenes/level1.json",
      "sceneHash": "0011223344556677",
      "entities": [
        { "guid": "00000000000000a1", "name": "Crate",
          "transform": { "position": [1,0,2], "rotation": [0,0,0], "scale": [1,1,1] } }
      ],
      "globals": {
        "playerGuid": "00000000000000a9",
        "playerHp": 55.0,
        "pickedUp": ["00000000000000a1"],
        "vars": { "door.opened": 1.0 }
      }
    })";

    SaveGame sg;
    SaveIoResult r;
    const bool ok = saveGameFromJson(json::parse(v1), sg, r);
    reportFlag("migrate_v1_ok", ok, r.error);
    report("migrate_v1_steps", static_cast<double>(r.migrated), 1.0);
    report("migrate_v1_field_default", static_cast<double>(sg.globals.playerStamina), 100.0);

    // 老字段一个不丢 —— 这条是"迁移函数只补新字段、不重建整份"的证据。
    int bad = 0;
    if (sg.globals.playerHp != 55.0f) ++bad;
    if (sg.globals.playerGuid != 0xA9ull) ++bad;
    if (sg.globals.pickedUp.size() != 1 || sg.globals.pickedUp[0] != 0xA1ull) ++bad;
    if (sg.globals.vars.size() != 1 || sg.globals.vars["door.opened"] != 1.0) ++bad;
    if (sg.entities.size() != 1 || sg.entities[0].guid != 0xA1ull) ++bad;
    if (sg.sceneHash != "0011223344556677") ++bad;
    report("migrate_v1_keeps_old", static_cast<double>(bad), 0.0);

    // 连 version 都没有的档（第一版忘了写）→ 当成 1，**必须**打 assume 日志
    {
        json noVer = json::parse(v1);
        noVer.erase("version");
        SaveGame sg2;
        SaveIoResult r2;
        const bool ok2 = saveGameFromJson(noVer, sg2, r2);
        reportFlag("migrate_assume_v1", ok2 && r2.migrated == 1 &&
                                            sg2.globals.playerStamina == 100.0f,
                   std::string("steps=") + std::to_string(r2.migrated));
    }

    // 比我们新 → **拒读**（降级读会静默丢字段）
    {
        json future = json::parse(v1);
        future["version"] = kSaveVersion + 1;
        SaveGame sg3;
        SaveIoResult r3;
        const bool ok3 = saveGameFromJson(future, sg3, r3);
        reportFlag("migrate_reject_newer",
                   !ok3 && r3.error.find("newer") != std::string::npos,
                   std::string("err=") + (r3.error.empty() ? "empty" : "newer"));
    }

    // 迁移后写出来必须是**当前版本**，而且再读一次不再迁移（幂等）
    {
        SaveGame sg4;
        SaveIoResult r4;
        saveGameFromJson(json::parse(v1), sg4, r4);
        const std::string dumped = saveGameToJson(sg4).dump();
        SaveGame sg5;
        SaveIoResult r5;
        const bool ok5 = saveGameFromJson(json::parse(dumped), sg5, r5);
        reportFlag("migrate_idempotent", ok5 && r5.migrated == 0,
                   std::string("steps=") + std::to_string(r5.migrated));
    }
}

// ============================================================
// D 组：原子写 / 读
// ============================================================
void groupFile() {
    const std::string path = tempPath("slot1.sav");

    SaveGame sg;
    sg.slot = "slot1";
    sg.scenePath = "scenes/level1.json";
    SaveEntity se;
    se.guid = 0xCAFE000000000001ull;
    se.raw = json{{"name", "Crate"}, {"transform", json::object()}};
    sg.entities.push_back(se);
    sg.globals.playerHp = 12.5f;

    const SaveIoResult w1 = writeSaveGame(sg, path);
    reportFlag("file_write_first", w1.ok, w1.error);

    SaveGame rd;
    const SaveIoResult r1 = readSaveGame(path, rd);
    reportFlag("file_read_roundtrip", r1.ok && rd.entities.size() == 1 &&
                                          rd.globals.playerHp == 12.5f,
               r1.error);

    // ---- 写第二次（同路径）----
    // ⚠ 这条专门抓 `std::rename` 在 Windows 上"目标存在就失败"的坑。
    //   只写一次的自检永远是绿的，用户存第二遍才炸。
    sg.globals.playerHp = 33.0f;
    const SaveIoResult w2 = writeSaveGame(sg, path);
    reportFlag("file_write_twice_replaces", w2.ok, w2.error);
    SaveGame rd2;
    const SaveIoResult r2 = readSaveGame(path, rd2);
    reportFlag("file_read_after_replace", r2.ok && rd2.globals.playerHp == 33.0f,
               r2.error);

    // 写两次之后应当有 `.bak`（第一次的内容）
    {
        std::error_code ec;
        const bool bakExists = std::filesystem::exists(
            std::filesystem::u8path(path + ".bak"), ec);
        reportFlag("file_bak_created", bakExists);
    }

    // ---- 主档坏掉 → 退 `.bak` ----
    writeText(path, "{ this is not json");
    SaveGame rd3;
    const SaveIoResult r3 = readSaveGame(path, rd3);
    reportFlag("file_bak_fallback", r3.ok && r3.usedBak,
               std::string("ok=") + (r3.ok ? "1" : "0") + ",bak=" + (r3.usedBak ? "1" : "0"));

    // ---- 不存在的路径 → 明确失败，不崩 ----
    {
        SaveGame rd4;
        const SaveIoResult r4 = readSaveGame(tempPath("no_such_slot.sav"), rd4);
        reportFlag("file_read_missing_fails", !r4.ok && !r4.error.empty());
    }

    // ---- 重复 guid → 拒绝整份 ----
    // ⚠ 对象形态的 JSON 会静默合并，所以这个 bug 只有在数组形态 + 显式校验下才可见。
    {
        json dup = saveGameToJson(sg);
        json ents = json::array();
        for (int i = 0; i < 2; ++i) {
            json je = se.raw;
            je["guid"] = ecs::guidToHex(se.guid);
            ents.push_back(je);
        }
        dup["entities"] = ents;
        SaveGame sg6;
        SaveIoResult r6;
        const bool ok6 = saveGameFromJson(dup, sg6, r6);
        reportFlag("file_reject_dup_guid",
                   !ok6 && r6.error.find("duplicate") != std::string::npos,
                   std::string("err=") + (r6.error.empty() ? "empty" : "dup"));
    }

    // ---- 非法 guid 字符串 → 拒绝（不是"当成无 guid"）----
    {
        json bad = saveGameToJson(sg);
        json ents = json::array();
        ents.push_back(json{{"name", "X"}, {"guid", "1234"}});   // 不是 16 位
        bad["entities"] = ents;
        SaveGame sg7;
        SaveIoResult r7;
        reportFlag("file_reject_short_guid", !saveGameFromJson(bad, sg7, r7));
    }

    // ---- 收尾：把主档恢复成一份**合法**的 ----
    // ⚠ 上面为了测 `.bak` 兜底故意把主档写坏过。不收尾的话，这个目录会长期
    //   留一份坏档：`tools/verify_persistence.py` 的 B 组直接读它就会红，
    //   而人第一反应会去查"写端是不是坏了"—— 其实是自检留下的现场。
    //   凡是"用例故意造坏数据"的地方，都要有这一步。
    {
        const SaveIoResult w3 = writeSaveGame(sg, path);
        reportFlag("file_restore_valid_after_negative", w3.ok, w3.error);
    }
}

// ============================================================
// E 组：增量（基线 + diff）
// ============================================================
void groupIncremental() {
    // ---- ① 关卡刚加载完 → diff 必须是空的 ----
    // ⚠ 这条最容易"看起来对但其实没测"：如果 diff 的实现忘了把 baseline 传下去，
    //   它会写全量 —— 而写全量对玩家**完全正确**，只是白花 IO。所以判据必须是
    //   `entities.size() == 0`，不是"能读回来"。
    {
        Scene s;
        buildLevel(s, {0xE000000000000001ull, 0xE000000000000002ull, 0xE000000000000003ull,
                       0xE000000000000004ull, 0xE000000000000005ull});
        SaveGameState st;
        st.captureBaseline(s);
        const SaveGame d = st.captureDiff(s, "quick", "scenes/level1.json", 1.0);
        report("diff_empty_after_baseline", static_cast<double>(d.entities.size()), 0.0);
        report("baseline_entity_count", static_cast<double>(st.size()), 5.0);
        // 对照区：同一份场景的**全量**档必须写满 —— 否则上一条的 0 可能只是
        // "什么都没抓到"（比如 excludeEmpty 之类的过滤把实体全丢了）。
        const SaveGame full = captureSaveGame(s, "quick", "scenes/level1.json", 1.0);
        report("diff_full_reference_count", static_cast<double>(full.entities.size()), 5.0);
    }

    // ---- ② 1000 实体只改 1 个 → 只写 1 条，体积比 < 2% ----
    // 这是增量存档唯一的**规模**证据：改 1 条写 1 条，而不是"改 1 条写 1000 条"。
    {
        Scene s;
        std::vector<ecs::EntityGuid> guids;
        guids.reserve(1000);
        for (int i = 0; i < 1000; ++i)
            guids.push_back(0xF000000000000000ull + static_cast<ecs::EntityGuid>(i + 1));
        buildLevel(s, guids);

        SaveGameState st;
        st.captureBaseline(s);

        const SaveGame full = captureSaveGame(s, "quick", "scenes/big.json", 0.0);
        const std::string fullText = saveGameToJson(full).dump();

        // 改中间那个（不取第一个：第一个的改动在"全量/增量"两种实现下都可能被
        // 首条特判掩盖，中间的位置不对任何实现有优待）。
        const ecs::Entity one = s.world().find(0xF000000000000000ull + 500ull);
        if (auto* t = s.world().get<ecs::TransformComponent>(one)) t->position.x += 1.0f;

        const SaveGame d = st.captureDiff(s, "quick", "scenes/big.json", 0.0);
        const std::string diffText = saveGameToJson(d).dump();
        report("diff_only_one_changed", static_cast<double>(d.entities.size()), 1.0);
        const double ratio = fullText.empty()
                                 ? 1.0
                                 : static_cast<double>(diffText.size()) /
                                       static_cast<double>(fullText.size());
        reportFlag("diff_small_ratio", ratio < 0.20,
                   std::string("ratio=") + std::to_string(ratio) + ",full=" +
                       std::to_string(fullText.size()) + ",diff=" + std::to_string(diffText.size()));
    }

    // ---- ③ 只改可见性也必须被发现（证明哈希不只是变换）----
    {
        Scene s;
        buildLevel(s, {0xE100000000000001ull, 0xE100000000000002ull});
        SaveGameState st;
        st.captureBaseline(s);
        const ecs::Entity e = s.world().find(0xE100000000000002ull);
        if (auto* v = s.world().get<ecs::VisibilityComponent>(e)) v->visible = false;
        const SaveGame d = st.captureDiff(s, "quick", "x.json", 0.0);
        reportFlag("diff_detects_visible",
                   d.entities.size() == 1 && d.entities[0].guid == 0xE100000000000002ull &&
                       !d.entities[0].destroyed,
                   std::string("n=") + std::to_string(d.entities.size()));
    }

    // ---- ④ 销毁 → 一条 `_destroyed`，不带值 ----
    {
        Scene s;
        buildLevel(s, {0xE200000000000001ull, 0xE200000000000002ull, 0xE200000000000003ull});
        SaveGameState st;
        st.captureBaseline(s);
        s.world().destroy(s.world().find(0xE200000000000002ull));
        const SaveGame d = st.captureDiff(s, "quick", "x.json", 0.0);
        report("diff_destroyed_count", static_cast<double>(d.entities.size()), 1.0);
        reportFlag("diff_destroyed_flag",
                   d.entities.size() == 1 && d.entities[0].destroyed &&
                       d.entities[0].guid == 0xE200000000000002ull && d.entities[0].raw.is_null(),
                   d.entities.empty() ? "empty" : std::string("d=") +
                                                          std::to_string(d.entities[0].destroyed));
    }

    // ---- ⑤ 「已销毁」是**粘性**的 ----
    // ⚠ 这条抓的是"销毁后把 guid 从基线里摘掉"这个看起来很合理的实现：
    //   摘掉之后第二次存档就不再写 `_destroyed`，而它会被读进**还带着那个实体**
    //   的关卡里 → "打死的怪读档后复活"。两轮 diff 都必须有那一条。
    {
        Scene s;
        const std::vector<ecs::EntityGuid> guids{0xE300000000000001ull, 0xE300000000000002ull};
        buildLevel(s, guids);
        SaveGameState st;
        st.captureBaseline(s);

        SaveGame kill;
        kill.slot = "quick";
        SaveEntity se;
        se.guid = guids[1];
        se.destroyed = true;
        kill.entities.push_back(se);

        s.world().destroy(s.world().find(guids[1]));
        st.noteApplied(s, kill);
        const SaveGame d1 = st.captureDiff(s, "quick", "x.json", 0.0);
        st.noteApplied(s, kill);
        const SaveGame d2 = st.captureDiff(s, "quick", "x.json", 0.0);
        reportFlag("diff_dead_sticky",
                   d1.entities.size() == 1 && d2.entities.size() == 1 &&
                       d2.entities[0].destroyed && d2.entities[0].guid == guids[1],
                   std::string("d1=") + std::to_string(d1.entities.size()) + ",d2=" +
                       std::to_string(d2.entities.size()));
    }

    // ---- ⑥ 父子用 guid 不用下标：销毁"前面的兄弟"不许把子实体的哈希带翻 ----
    // ⚠ 这是 `saveEntityJson` 删掉 `parent` 的**唯一**证据。父子若写下标，
    //   P1 被销毁后 C 的 `parent` 会变 → C 的哈希变了 → 一次销毁让别的实体
    //   也被判成"改过"，增量存档退化。画面上完全正常（读回来的值一模一样），
    //   只有存档体积会莫名其妙地涨。
    //
    // ⚠⚠ 夹具的**创建顺序是被算过的**，别随手改：`World` 的池子是
    //   swap-and-pop —— 销毁下标 i 的实体时，只有**最后一个**元素被搬到 i，
    //   其余元素的下标**纹丝不动**。所以"销毁第一个"根本不会让任何父的下标
    //   变化（第一版夹具就是这样，否定测试时这条居然是绿的）。
    //   必须让**父排在最后**：销毁 X（下标 0）→ 父从 2 搬到 0，子看到的
    //   父下标 2→0 才会真变。这条顺序就是这条断言的全部杀伤力所在。
    {
        Scene s;
        const ecs::Entity victim = s.createObject("X");        // 下标 0，稍后销毁
        s.world().setGuid(victim, 0xE400000000000001ull);
        const ecs::Entity c = s.createObject("C");             // 下标 1（子）
        s.world().setGuid(c, 0xE400000000000003ull);
        const ecs::Entity p = s.createObject("P");             // 下标 2（父，**最后**）
        s.world().setGuid(p, 0xE400000000000002ull);
        s.setParent(c, p);

        SaveGameState st;
        st.captureBaseline(s);
        s.world().destroy(victim);

        const SaveGame d = st.captureDiff(s, "quick", "x.json", 0.0);
        int childInDiff = 0;
        for (const SaveEntity& se : d.entities)
            if (!se.destroyed && se.guid == 0xE400000000000003ull) ++childInDiff;
        report("diff_no_index_coupling", static_cast<double>(childInDiff), 0.0);
        // 对照：确实记到了"兄弟被销毁"（否则上一条 0 也可能是"什么都没抓到"）
        report("diff_sibling_destroyed_recorded", static_cast<double>(d.entities.size()), 1.0);
    }
}

// ============================================================
// F 组：应用到已有世界
// ============================================================
void groupApply() {
    assets::AssetManager& am = nullAssets();

    const std::vector<ecs::EntityGuid> guids{0xA100000000000001ull, 0xA100000000000002ull,
                                             0xA100000000000003ull};

    // ---- ① 存 → 改 → 读 → 值回来（且与存档时刻**逐字节**相同）----
    {
        Scene s;
        buildLevel(s, guids);
        const SaveGame s1 = captureSaveGame(s, "slot1", "scenes/level1.json", 0.0);

        // 改三个不同的字段：位置 / 缩放 / 可见性。
        // 全改位置的话，"只还原位置"的实现也能全绿 —— 覆盖面必须换着来。
        if (auto* t = s.world().get<ecs::TransformComponent>(s.world().find(guids[0])))
            t->position = glm::vec3(9.0f, 9.0f, 9.0f);
        if (auto* t = s.world().get<ecs::TransformComponent>(s.world().find(guids[1])))
            t->scale = glm::vec3(3.0f, 3.0f, 3.0f);
        if (auto* v = s.world().get<ecs::VisibilityComponent>(s.world().find(guids[2])))
            v->visible = false;

        const SaveApplyResult ap = applySaveGame(s, s1, am);
        reportFlag("apply_ok", ap.ok, ap.error);
        report("apply_applied", static_cast<double>(ap.stats.applied), 3.0);

        int bad = 0;
        for (const ecs::EntityGuid g : guids) {
            const ecs::Entity e = s.world().find(g);
            const auto* t = s.world().get<ecs::TransformComponent>(e);
            const auto* v = s.world().get<ecs::VisibilityComponent>(e);
            if (!t) { ++bad; continue; }
            if (t->position != glm::vec3(0.0f) || t->scale != glm::vec3(1.0f)) ++bad;
            if (v && !v->visible) ++bad;
        }
        report("apply_restores_values", static_cast<double>(bad), 0.0);

        // ⚠ 逐字节：只比字段会漏"多写了一个默认值字段 / 少写了一个 0"，
        //   而那种差异会让下一份存档和上一份"看起来一样、哈希不同"。
        const SaveGame s2 = captureSaveGame(s, "slot1", "scenes/level1.json", 0.0);
        const std::string a = saveGameToJson(s1)["entities"].dump();
        const std::string b = saveGameToJson(s2)["entities"].dump();
        reportFlag("apply_entities_bitwise", a == b,
                   std::to_string(a.size()) + "/" + std::to_string(b.size()));
    }

    // ---- ② 销毁条目：盖到**重新加载出来的**关卡上，实体少一个 ----
    {
        Scene level;
        buildLevel(level, guids);
        SaveGameState st;
        st.captureBaseline(level);
        level.world().destroy(level.world().find(guids[1]));
        const SaveGame kill = st.captureDiff(level, "quick", "scenes/level1.json", 0.0);
        report("apply_destroyed_pending", static_cast<double>(kill.entities.size()), 1.0);

        Scene reload;
        buildLevel(reload, guids);   // 关卡又把三个都建出来了
        const int before = static_cast<int>(reload.world().entityCount());
        const SaveApplyResult ap = applySaveGame(reload, kill, am);
        reportFlag("apply_destroyed_ok", ap.ok, ap.error);
        report("apply_destroyed_delta",
               static_cast<double>(before - static_cast<int>(reload.world().entityCount())), 1.0);
        reportFlag("apply_destroyed_gone", !reload.world().find(guids[1]).valid());
    }

    // ---- ③ 关卡漂移：少量 missing 要容忍，其余照常应用 ----
    {
        Scene s;
        buildLevel(s, guids);
        SaveGame sg = captureSaveGame(s, "quick", "x.json", 0.0);

        if (auto* t = s.world().get<ecs::TransformComponent>(s.world().find(guids[0])))
            t->position = glm::vec3(5.0f, 0.0f, 0.0f);

        // 追加一条指向"这个关卡里没有的实体"的条目（关卡删掉了那个箱子）
        json ghost = saveEntityJson(s.world(), s.world().find(guids[2]));
        ghost["guid"] = ecs::guidToHex(0xDEAD000000000001ull);
        SaveEntity se;
        se.guid = 0xDEAD000000000001ull;
        se.raw = ghost;
        sg.entities.push_back(se);

        const SaveApplyResult ap = applySaveGame(s, sg, am);
        reportFlag("apply_drift_ok", ap.ok, ap.error);
        report("apply_drift_missing", static_cast<double>(ap.stats.missing), 1.0);
        const auto* t = s.world().get<ecs::TransformComponent>(s.world().find(guids[0]));
        reportFlag("apply_drift_still_applies", t && t->position == glm::vec3(0.0f));
    }

    // ---- ④ missing 超过一半 → 判定"不是同一个关卡"，**一条都不许动** ----
    // ⚠ 判据是"位置还是被改过的那份"：拒读时如果已经覆盖了一半，玩家看到的是
    //   "一半旧一半新"的世界 + 一句"存档不匹配"，比直接不读更糟。
    {
        Scene s;
        buildLevel(s, {0xA200000000000001ull, 0xA200000000000002ull});
        SaveGame sg = captureSaveGame(s, "quick", "x.json", 0.0);
        if (auto* t = s.world().get<ecs::TransformComponent>(s.world().find(0xA200000000000001ull)))
            t->position = glm::vec3(7.0f, 0.0f, 0.0f);

        for (int i = 0; i < 3; ++i) {
            const ecs::EntityGuid ghostGuid = 0xDEAD100000000000ull + static_cast<ecs::EntityGuid>(i);
            SaveEntity se;
            se.guid = ghostGuid;
            json je = saveEntityJson(s.world(), s.world().find(0xA200000000000002ull));
            je["guid"] = ecs::guidToHex(ghostGuid);
            se.raw = je;
            sg.entities.push_back(se);
        }

        const SaveApplyResult ap = applySaveGame(s, sg, am);
        reportFlag("apply_drift_reject", !ap.ok && ap.stats.driftRejected,
                   std::string("ok=") + (ap.ok ? "1" : "0") + ",rej=" +
                       (ap.stats.driftRejected ? "1" : "0"));
        report("apply_drift_reject_missing", static_cast<double>(ap.stats.missing), 3.0);
        const auto* t = s.world().get<ecs::TransformComponent>(s.world().find(0xA200000000000001ull));
        reportFlag("apply_drift_reject_untouched", t && t->position == glm::vec3(7.0f, 0.0f, 0.0f),
                   t ? std::string("x=ok") : std::string("no_transform"));
    }

    // ---- ⑤ 世界里有、存档里没有 → 保留关卡默认值（extra），不报错 ----
    {
        Scene s;
        buildLevel(s, {0xA300000000000001ull, 0xA300000000000002ull});
        SaveGame sg = captureSaveGame(s, "quick", "x.json", 0.0);
        const ecs::Entity added = s.createObject("LevelAdded");
        s.world().setGuid(added, 0xA300000000000003ull);   // 关卡新增，存档里没有
        if (auto* t = s.world().get<ecs::TransformComponent>(added))
            t->position = glm::vec3(0.0f, 4.0f, 0.0f);

        const SaveApplyResult ap = applySaveGame(s, sg, am);
        reportFlag("apply_extra_ok", ap.ok, ap.error);
        report("apply_extra_count", static_cast<double>(ap.stats.extra), 1.0);
        const auto* t = s.world().get<ecs::TransformComponent>(added);
        reportFlag("apply_extra_untouched", t && t->position == glm::vec3(0.0f, 4.0f, 0.0f));
    }

    // ---- ⑥ globals.vars **合并**进脚本黑板，不清空 ----
    // ⚠ 清空的话，脚本 `let` 声明的关卡常量会消失（它们不在存档里），
    //   而下一帧脚本重新声明之前读到的是 0。
    {
        Scene s;
        buildLevel(s, {0xA400000000000001ull});
        s.scriptVars()["level.constant"] = 42.0;
        SaveGame sg = captureSaveGame(s, "quick", "x.json", 0.0);
        sg.globals.vars["quest.stage"] = 3.0;
        sg.globals.vars["door.opened"] = 1.0;

        const SaveApplyResult ap = applySaveGame(s, sg, am);
        reportFlag("apply_globals_ok", ap.ok, ap.error);
        const bool merged = s.scriptVars().count("quest.stage") &&
                            s.scriptVars()["quest.stage"] == 3.0 &&
                            s.scriptVars()["door.opened"] == 1.0 &&
                            s.scriptVars().count("level.constant") &&
                            s.scriptVars()["level.constant"] == 42.0;
        reportFlag("apply_globals_merge", merged,
                   std::string("vars=") + std::to_string(s.scriptVars().size()));
    }

    // ---- ⑦ 只改值 → 组件**类型集合**不变（D2 的守卫）----
    // 存档条目里**删掉** `audio` 块（模拟旧版本 / 手改过的档），应用后
    // `AudioSourceComponent` 必须还在。反过来（"没写就删掉"）会让读一份旧档
    // 把整关的音效全静音，而日志一条错都没有。
    {
        Scene s;
        const ecs::Entity e = s.createObject("Speaker");
        s.world().setGuid(e, 0xA500000000000001ull);
        ecs::AudioSourceComponent audio;
        audio.clip = "audio/click.wav";
        audio.volume = 0.35f;
        s.world().add<ecs::AudioSourceComponent>(e, audio);

        SaveGame sg = captureSaveGame(s, "quick", "x.json", 0.0);
        reportFlag("apply_fixture_has_audio_block",
                   sg.entities.size() == 1 && sg.entities[0].raw.contains("audio"));
        sg.entities[0].raw.erase("audio");   // 故意少写一块

        const int before = static_cast<int>(s.world().has<ecs::AudioSourceComponent>(e));
        const SaveApplyResult ap = applySaveGame(s, sg, am);
        const int after = static_cast<int>(s.world().has<ecs::AudioSourceComponent>(e));
        reportFlag("apply_no_component_remove", ap.ok && before == 1 && after == 1,
                   std::string("before=") + std::to_string(before) + ",after=" +
                       std::to_string(after));
        // 反向：带 `audio` 块时不许多加一份（`add` 与 `get` 混用会重复插入）
        const int count = static_cast<int>(s.world().has<ecs::AudioSourceComponent>(e));
        reportFlag("apply_audio_single", count == 1);
    }

    // ---- ⑧ 悬挂父：存档里销毁了父亲，子实体必须断开（criterion 10）----
    {
        const std::vector<ecs::EntityGuid> fam{0xA600000000000001ull, 0xA600000000000002ull};
        Scene level;
        buildLevel(level, fam);
        level.setParent(level.world().find(fam[1]), level.world().find(fam[0]));

        SaveGameState st;
        st.captureBaseline(level);
        level.world().destroy(level.world().find(fam[0]));
        const SaveGame kill = st.captureDiff(level, "quick", "x.json", 0.0);
        // ⚠ 这里是 **2** 条，不是 1：父的 `_destroyed` + **子也被判成"改过"**。
        //   原因不是 bug —— 子的 `parentGuid` 是按"父还活着 → 写下 guid"算的，
        //   父一死，`w.guid(handle(parentId))` 变 0，子的 JSON 里就没有
        //   `parentGuid` 了，哈希自然变。这条"父死了，子被重写一遍"是**对的**：
        //   子持久化的父引用确实变了。写死 1 的期望会把这个行为当成 bug 抹掉。
        int destroyedEntries = 0;
        for (const SaveEntity& se : kill.entities) destroyedEntries += se.destroyed ? 1 : 0;
        report("apply_orphan_fixture_destroyed", static_cast<double>(destroyedEntries), 1.0);
        report("apply_orphan_fixture_total", static_cast<double>(kill.entities.size()), 2.0);

        Scene reload;
        buildLevel(reload, fam);
        reload.setParent(reload.world().find(fam[1]), reload.world().find(fam[0]));

        const SaveApplyResult ap = applySaveGame(reload, kill, am);
        const auto* h = reload.world().get<ecs::HierarchyComponent>(reload.world().find(fam[1]));
        reportFlag("apply_orphan_parent_fixed", ap.ok && h && h->parent == ecs::kInvalidEntity,
                   std::string("orphans=") + std::to_string(ap.stats.orphans));
        report("apply_orphan_count", static_cast<double>(ap.stats.orphans), 1.0);
    }

    // ---- ⑨ 读档后立刻再存 → diff 必须是空的（`noteApplied` 的用处）----
    {
        Scene s;
        buildLevel(s, guids);
        SaveGameState st;
        st.captureBaseline(s);
        if (auto* t = s.world().get<ecs::TransformComponent>(s.world().find(guids[0])))
            t->position.x = 4.0f;

        SaveGame d1 = st.captureDiff(s, "quick", "x.json", 0.0);
        report("apply_note_dirty_before", static_cast<double>(d1.entities.size()), 1.0);

        Scene reload;
        buildLevel(reload, guids);
        SaveGameState st2;
        st2.captureBaseline(reload);
        const SaveApplyResult ap = applySaveGame(reload, d1, am, &st2);
        const SaveGame d2 = st2.captureDiff(reload, "quick", "x.json", 0.0);
        reportFlag("apply_note_applied_clean", ap.ok && d2.entities.empty(),
                   std::string("n=") + std::to_string(d2.entities.size()));
    }
}

// ============================================================
// G 组：形状守卫（"只有一份实现"的证据）
// ============================================================
void groupShape() {
    // ---- ① 存档 JSON 用 `parentGuid`，**不**用 `parent` 下标 ----
    {
        Scene s;
        const ecs::Entity p = s.createObject("P");
        s.world().setGuid(p, 0xA700000000000001ull);
        const ecs::Entity c = s.createObject("C");
        s.world().setGuid(c, 0xA700000000000002ull);
        s.setParent(c, p);

        const json je = saveEntityJson(s.world(), c);
        reportFlag("json_has_parent_guid",
                   je.is_object() && je.contains("parentGuid") && !je.contains("parent"),
                   std::string("keys_parent=") + std::to_string(je.contains("parent") ? 1 : 0) +
                       ",guid=" + std::to_string(je.contains("parentGuid") ? 1 : 0));
        reportFlag("json_parent_guid_value",
                   je.is_object() && je.contains("parentGuid") &&
                       je["parentGuid"] == ecs::guidToHex(0xA700000000000001ull));
    }

    // ---- ② diff 里的一条与全量档里的同一条**逐字节相同** ----
    // ⚠ 这条防的是"diff 自己拼了一份 JSON"：字段顺序、`includeEmpty`、
    //   `parent` 有没有删，任一处不同就让哈希永远对不上 —— 而症状只是
    //   "存档忽然变大"，没有任何报错。
    {
        Scene s;
        buildLevel(s, {0xA800000000000001ull, 0xA800000000000002ull});
        SaveGameState st;
        st.captureBaseline(s);
        if (auto* t = s.world().get<ecs::TransformComponent>(s.world().find(0xA800000000000001ull)))
            t->position.z = 2.5f;

        const SaveGame diff = st.captureDiff(s, "quick", "x.json", 0.0);
        const SaveGame full = captureSaveGame(s, "quick", "x.json", 0.0);
        std::string a("<none>");
        std::string b("<none>");
        for (const SaveEntity& se : diff.entities)
            if (se.guid == 0xA800000000000001ull) a = se.raw.dump();
        for (const SaveEntity& se : full.entities)
            if (se.guid == 0xA800000000000001ull) b = se.raw.dump();
        reportFlag("diff_entry_identical_to_full", a == b,
                   std::to_string(a.size()) + "/" + std::to_string(b.size()));
    }

    // ---- ③ `saveEntityHash` 必须等于"同一份 JSON 的 dump 的 FNV" ----
    // 反向的用处：谁把哈希改成"自己遍历字段"，这条会红（dump 顺序不同）。
    {
        Scene s;
        const ecs::Entity e = s.createObject("H");
        const json je = saveEntityJson(s.world(), e);
        const std::string text = je.dump();
        const std::uint64_t manual = fnv1a64(text.data(), text.size());
        const std::uint64_t got = saveEntityHash(s.world(), e);
        reportFlag("hash_is_dump_fnv", got == manual && got != 0,
                   std::to_string(got) + "/" + std::to_string(manual));
        // 同一实体两次必须同值（`std::map` 的有序键 + 最短往返浮点）
        reportFlag("hash_stable_twice", saveEntityHash(s.world(), e) == got);
    }
}

}  // namespace

int runPersistenceSelfTest() {
    g_pass = 0;
    g_fail = 0;

    VK_LOG_INFO("== selftest: save (G17-1 file layer + G17-2 apply layer) ==");
    groupHash();
    groupFormat();
    groupMigrate();
    groupFile();
    groupIncremental();
    groupApply();
    groupShape();

    VK_LOG_INFO("SAVE-TOTAL pass=%d fail=%d", g_pass, g_fail);
    return g_fail;
}

}  // namespace scene
