// ============================================================
// scene/SaveGame.cpp —— 存档文件层（G17-1）
//
// 实现顺序刻意与"存档会怎么坏"对齐：
//   ① 哈希（结构指纹）
//   ② 抓档（场景 → SaveGame）
//   ③ saveGameToJson / saveGameFromJson（含**迁移表**）
//   ④ 原子写（tmp → fsync → 备份 → rename）
//   ⑤ 读（主档 → .bak 兜底）
// ============================================================

#include "scene/SaveGame.h"

#include "core/Logger.h"
#include "ecs/Components.h"
#include "ecs/World.h"
#include "scene/Scene.h"
#include "scene/SceneEntityIO.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <system_error>
#include <unordered_map>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#endif

namespace scene {

namespace {

using json = nlohmann::json;

// ---------------- ① 哈希 ----------------

constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

std::string hex16(std::uint64_t v) {
    static const char kHex[] = "0123456789abcdef";
    char buf[17];
    for (int i = 15; i >= 0; --i) {
        buf[i] = kHex[v & 0xFu];
        v >>= 4;
    }
    buf[16] = '\0';
    return std::string(buf);
}

std::string nowIso8601() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}

bool readWholeFile(const std::string& path, std::string& out) {
    FILE* f = nullptr;
#ifdef _WIN32
    if (fopen_s(&f, path.c_str(), "rb") != 0) f = nullptr;
#else
    f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) return false;
    char buf[65536];
    size_t n = 0;
    out.clear();
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return true;
}

// ---------------- ③ 迁移表 ----------------

// v1 → v2：`globals` 多一个 `playerStamina`。
//
// ⚠ 这一步是**故意存在的**（见 SaveGame.h 里 v2 的注释）：v1 发出去之后立刻
//   在同一批改动里加 v2 字段，让迁移机制被真的走过一遍。
// ⚠ v1 里没有 `globals` 的对象（手写 / 损坏）也要能过 —— 迁移函数不许 throw。
void migrateV1ToV2(json& root) {
    if (!root.contains("globals") || !root["globals"].is_object())
        root["globals"] = json::object();
    if (!root["globals"].contains("playerStamina"))
        root["globals"]["playerStamina"] = 100.0;
}

using MigrateFn = void (*)(json&);

struct Migration {
    int from;      // 把这个版本升到 from+1
    MigrateFn fn;
};

const Migration kMigrations[] = {
    {1, migrateV1ToV2},
};

// 返回迁移步数；失败返回 -1 并填 err。
int migrateUp(json& root, std::string& err) {
    // ⚠ 用 `is_number_integer()` 先判：`root.value("version", 0)` 遇到字符串
    //   `"version": "1"`（手改过的档）会抛 `type_error.302`，而抛出点在
    //   `readSaveGame` 的 try 外面 → 整个读档崩掉而不是"返回一份错误"。
    int v = (root.contains("version") && root["version"].is_number_integer())
                ? root["version"].get<int>()
                : 0;
    if (v <= 0) {
        // 第一版忘了写 version 的档：当成 1。**必须打日志** —— 否则这条
        // "假设"会永远藏在代码里，而它是唯一知道旧档来历的地方。
        v = 1;
        VK_LOG_WARN("SAVE-MIGRATE from=0 to=%d assume=1", kSaveVersion);
        root["version"] = 1;
    } else if (v > kSaveVersion) {
        err = "save version " + std::to_string(v) + " is newer than this build supports (" +
              std::to_string(kSaveVersion) + ")";
        VK_LOG_ERROR("SAVE-VERSION too-new found=%d supported=%d", v, kSaveVersion);
        return -1;
    }

    int steps = 0;
    while (v < kSaveVersion) {
        const Migration* found = nullptr;
        for (const Migration& m : kMigrations)
            if (m.from == v) found = &m;
        if (!found) {
            // 缺一环 = 拒绝。**不能**"尽力而为地跳过"，那会产出一份字段残缺的档，
            // 而残缺在应用阶段表现为"某些物体回到默认值"，没人会想到是迁移的问题。
            err = "no migration from save version " + std::to_string(v);
            VK_LOG_ERROR("SAVE-MIGRATE missing-from=%d", v);
            return -1;
        }
        found->fn(root);
        VK_LOG_INFO("SAVE-MIGRATE from=%d to=%d", v, v + 1);
        ++v;
        ++steps;
    }
    root["version"] = kSaveVersion;
    return steps;
}

}  // namespace

// ---------------- ① 哈希 ----------------

std::uint64_t fnv1a64(const void* data, std::size_t n) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    std::uint64_t h = kFnvOffset;
    for (std::size_t i = 0; i < n; ++i) {
        h ^= static_cast<std::uint64_t>(p[i]);
        h *= kFnvPrime;
    }
    return h;
}

std::string sceneStructureHash(const Scene& scene) {
    const ecs::World& w = scene.world();
    std::vector<ecs::EntityGuid> guids;
    w.each<ecs::TransformComponent>([&](ecs::Entity e, const ecs::TransformComponent&) {
        const ecs::EntityGuid g = w.guid(e);
        if (g != ecs::kInvalidGuid) guids.push_back(g);
    });
    // ⚠ 必须排序：`World` 里池子的遍历顺序依赖实体分配历史，同内容两次加载
    //   可能给出不同顺序 → 不排序的哈希会"无缘无故地变"。
    std::sort(guids.begin(), guids.end());

    // ⚠ 必须顺序喂字节，不能对每个 guid 单独算一次 `fnv1a64` 再"混合" ——
    //   那样每次都会从 offset 重新开始，等于只在算最后一个 guid 的哈希
    //   （"加实体哈希不变 / 删实体哈希也不变"就是这么来的，而且很难看出来）。
    std::uint64_t h = kFnvOffset;
    for (ecs::EntityGuid g : guids) {
        for (int i = 0; i < 8; ++i) {
            h ^= static_cast<std::uint64_t>((g >> (i * 8)) & 0xFFu);
            h *= kFnvPrime;
        }
    }
    return hex16(h);
}

// ---------------- ② 抓档 ----------------

namespace {
// 一条实体 JSON 的哈希。0 是"没有内容"的哨兵 —— 真实哈希恰好为 0 的概率约
// 2^-64，但仍要挡一下：挡住它的代价是一次比较，不挡的代价是那个实体永远不进档。
//
// ⚠ 必须哈希 `dump()` 而不是自己遍历字段：`nlohmann` 的 object 用有序 map、
//   float 用最短往返表示，两条合起来保证"同值 → 同字节"。自己遍历会漏掉
//   键顺序与浮点格式，哈希就"总有变化"，增量存档退化成全量而没有人报错。
std::uint64_t hashEntityJson(const json& je) {
    if (!je.is_object()) return 0;
    const std::string text = je.dump();
    const std::uint64_t h = fnv1a64(text.data(), text.size());
    return h == 0 ? 1 : h;
}
}  // namespace

json saveEntityJson(const ecs::World& w, ecs::Entity e) {
    // ⚠ 故意给一张**空下标表**：`entityToJson` 于是把 `parent` 写成 -1，
    //   转而走"父不在本次导出集合里"那条分支写 `parentGuid` —— 存档要的正是它。
    //   把整世界的下标表传进来会让增量存档写出**指错人**的下标（见 SaveGame.h）。
    static const std::unordered_map<ecs::EntityId, int> kEmptyIndex;

    json je;
    if (!entityToJson(w, e, kEmptyIndex, je, /*includeEmpty=*/true)) return json();
    // `parent` 现在一定是 -1（上面那张空表）。直接删掉，别留一个会误导人的
    // `"parent": -1` —— 读它的人会以为"这个实体没有父"，而它可能有一个真父。
    je.erase("parent");
    return je;
}

std::uint64_t saveEntityHash(const ecs::World& w, ecs::Entity e) {
    return hashEntityJson(saveEntityJson(w, e));
}

SaveGame captureSaveGame(const Scene& scene, const std::string& slot,
                         const std::string& scenePath, double playTime,
                         const std::unordered_map<ecs::EntityGuid, std::uint64_t>* baseline) {
    const ecs::World& w = scene.world();

    SaveGame sg;
    sg.version = kSaveVersion;
    sg.slot = slot;
    sg.timestamp = nowIso8601();
    sg.playTime = playTime;
    sg.scenePath = scenePath;
    sg.sceneHash = sceneStructureHash(scene);

    // 先按"世界顺序"收集一遍存活实体：增量循环里要判"基线里哪些已经没了"，
    // 而 `World` 没有"按 guid 遍历"的接口。
    std::vector<ecs::Entity> alive;
    w.each<ecs::TransformComponent>(
        [&](ecs::Entity e, const ecs::TransformComponent&) { alive.push_back(e); });

    sg.entities.reserve(alive.size());
    for (const ecs::Entity e : alive) {
        const ecs::EntityGuid g = w.guid(e);
        if (g == ecs::kInvalidGuid) continue;  // 没身份 → 存档里无法定位

        // ⚠ **必须** includeEmpty=true（在 saveEntityJson 里）：存档要全量。
        //   默认值（false）是场景文件语义"不写空壳"—— 用默认值的话玩家 /
        //   出生点 / 纯分组节点全都不进档，而读档后它们会被当成"关卡里多出来
        //   的实体"保留默认变换（或直接消失），画面上看起来像"读档没生效"。
        json je = saveEntityJson(w, e);
        if (!je.is_object()) continue;

        if (baseline) {
            const auto it = baseline->find(g);
            // 基线里有、且内容一模一样 → 不写。这就是全部"增量"。
            // ⚠ 用**已经序列化好的** `je` 算哈希，不要再调一次 `saveEntityHash`
            //   （那会把同一份 JSON 生成两遍 —— 1000 实体就是白扔一半时间）。
            if (it != baseline->end() && it->second == hashEntityJson(je)) continue;
        }

        SaveEntity se;
        se.guid = g;
        se.raw = std::move(je);
        sg.entities.push_back(std::move(se));
    }

    // ---- 基线里有、世界里已经没有的实体 → `_destroyed` ----
    // ⚠ 用 `find(guid)` 判而不是"上面的 alive 列表里没出现"：后者需要一张
    //   集合，而且会漏掉"guid 因为重复被重发"这种边缘情况。
    // ⚠ 已死条目**不写值**，只写 guid + `_destroyed`（`raw` 也置成 null，
    //   与 `saveGameFromJson` 读出来的形态一致 —— 两边不一致的话，
    //   "存 → 读 → 存"就会在 `"raw": {}` 和 `"raw": null` 之间来回跳）。
    if (baseline) {
        for (const auto& kv : *baseline) {
            if (w.find(kv.first).valid()) continue;
            SaveEntity se;
            se.guid = kv.first;
            se.destroyed = true;
            se.raw = json(nullptr);
            sg.entities.push_back(std::move(se));
        }
    }
    return sg;
}

// ---------------- ③ 序列化 ----------------

json saveGameToJson(const SaveGame& sg) {
    json root;
    root["version"] = sg.version;
    root["game"] = sg.game;
    root["slot"] = sg.slot;
    root["timestamp"] = sg.timestamp;
    root["playTime"] = sg.playTime;
    root["scenePath"] = sg.scenePath;
    root["sceneHash"] = sg.sceneHash;

    json ents = json::array();
    for (const SaveEntity& se : sg.entities) {
        if (se.destroyed) {
            json je;
            je["guid"] = ecs::guidToHex(se.guid);   // ⚠ 必须 16 位补零
            je["_destroyed"] = true;
            ents.push_back(std::move(je));
            continue;
        }
        json je = se.raw.is_object() ? se.raw : json::object();
        // guid 是**存档的键**，由 SaveEntity 说了算（raw 里可能没有 / 可能是旧的）。
        je["guid"] = ecs::guidToHex(se.guid);
        ents.push_back(std::move(je));
    }
    root["entities"] = std::move(ents);

    json g;
    g["playerGuid"] = ecs::guidToHex(sg.globals.playerGuid);   // 0 → 16 个 0，读端认作"无"
    g["playerHp"] = sg.globals.playerHp;
    g["playerStamina"] = sg.globals.playerStamina;
    json picked = json::array();
    for (ecs::EntityGuid p : sg.globals.pickedUp) picked.push_back(ecs::guidToHex(p));
    g["pickedUp"] = std::move(picked);
    json vars = json::object();
    for (const auto& kv : sg.globals.vars) vars[kv.first] = kv.second;
    g["vars"] = std::move(vars);
    root["globals"] = std::move(g);

    return root;
}

bool saveGameFromJson(json root, SaveGame& out, SaveIoResult& r) {
    if (!root.is_object()) {
        r.error = "save root is not an object";
        return false;
    }

    // ---- 迁移（解析后立刻做，早于任何字段读取）----
    std::string err;
    const int steps = migrateUp(root, err);
    if (steps < 0) {
        r.error = err;
        return false;
    }
    r.migrated = steps;

    out = SaveGame{};
    out.version = kSaveVersion;
    out.game = root.value("game", std::string("venn"));
    out.slot = root.value("slot", std::string());
    out.timestamp = root.value("timestamp", std::string());
    out.playTime = root.value("playTime", 0.0);
    out.scenePath = root.value("scenePath", std::string());
    out.sceneHash = root.value("sceneHash", std::string());

    if (root.contains("entities")) {
        if (!root["entities"].is_array()) {
            r.error = "save 'entities' is not an array";
            return false;
        }
        // ⚠ 重复 guid 必须**拒绝整份**，不能"后一条覆盖前一条"：
        //   对象形态的 JSON 会静默合并（见 SaveGame.h 第 1 条），数组形态
        //   存在的意义就是让这里能报出来。
        std::vector<ecs::EntityGuid> seen;
        seen.reserve(root["entities"].size());
        for (const json& je : root["entities"]) {
            if (!je.is_object()) {
                r.error = "save entity entry is not an object";
                return false;
            }
            SaveEntity se;
            const std::string gs = je.value("guid", std::string());
            se.guid = ecs::parseGuidHex(gs);
            if (se.guid == ecs::kInvalidGuid) {
                r.error = "save entity has invalid guid '" + gs + "'";
                return false;
            }
            if (std::find(seen.begin(), seen.end(), se.guid) != seen.end()) {
                r.error = "duplicate guid " + gs + " in save";
                VK_LOG_ERROR("SAVE-DUP-GUID guid=%s", gs.c_str());
                return false;
            }
            seen.push_back(se.guid);

            se.destroyed = je.value("_destroyed", false);
            se.raw = se.destroyed ? json(nullptr) : je;
            if (se.destroyed) ++r.destroyed;
            out.entities.push_back(std::move(se));
        }
    }
    r.entities = static_cast<int>(out.entities.size());

    if (root.contains("globals") && root["globals"].is_object()) {
        const json& g = root["globals"];
        out.globals.playerGuid = ecs::parseGuidHex(g.value("playerGuid", std::string()));
        out.globals.playerHp = g.value("playerHp", out.globals.playerHp);
        out.globals.playerStamina = g.value("playerStamina", out.globals.playerStamina);
        if (g.contains("pickedUp") && g["pickedUp"].is_array()) {
            for (const json& p : g["pickedUp"]) {
                if (!p.is_string()) continue;
                const ecs::EntityGuid pg = ecs::parseGuidHex(p.get<std::string>());
                if (pg != ecs::kInvalidGuid) out.globals.pickedUp.push_back(pg);
            }
        }
        if (g.contains("vars") && g["vars"].is_object()) {
            for (auto it = g["vars"].begin(); it != g["vars"].end(); ++it)
                if (it.value().is_number()) out.globals.vars[it.key()] = it.value().get<double>();
        }
    }

    r.ok = true;
    return true;
}

// ---------------- ④ 原子写 ----------------

namespace {

#ifdef _WIN32
std::wstring toWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}
#endif

bool fileExists(const std::string& p) {
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::u8path(p), ec);
}

// 原子替换。⚠ Windows 上 **不能**用 `std::rename`：目标存在时它按 C 语义**失败**，
// 于是"第二次存同一个槽"会一直报错，而第一次是好的 —— 这种 bug 只在
// 用户存第二遍时出现，自检里跑一次永远看不到。
bool replaceFile(const std::string& from, const std::string& to) {
#ifdef _WIN32
    // ⚠ 用 `MoveFileExW` 而不是 `std::rename`：MSVC 的 `rename` 其实也能覆盖，
    //   但不保证 write-through。`MOVEFILE_WRITE_THROUGH` 让这次替换在返回前
    //   就落到磁盘 —— 存档是"玩家会拔电源"的场景，这一点值一个 API 调用。
    //   （POSIX 上 `rename` 本来就是原子的，走 else 分支。）
    return ::MoveFileExW(toWide(from).c_str(), toWide(to).c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return std::rename(from.c_str(), to.c_str()) == 0;
#endif
}

}  // namespace

SaveIoResult writeSaveGame(const SaveGame& sg, const std::string& path) {
    SaveIoResult r;

    std::string text;
    try {
        text = saveGameToJson(sg).dump();   // 不缩进（见 SaveGame.h 第 3 条）
    } catch (const std::exception& ex) {
        r.error = std::string("json dump failed: ") + ex.what();
        return r;
    }

    const std::string tmp = path + ".tmp";

    // ---- 1. 写 tmp + fsync ----
    // ⚠ `_commit` 不能省：少了它数据还在页缓存里，断电/崩溃后 tmp 可能是空的，
    //   而"空的 tmp"会被下一步 rename **变成正式档**。
    {
        FILE* f = nullptr;
#ifdef _WIN32
        if (fopen_s(&f, tmp.c_str(), "wb") != 0) f = nullptr;
#else
        f = std::fopen(tmp.c_str(), "wb");
#endif
        if (!f) {
            r.error = "cannot open for write: " + tmp;
            return r;
        }
        const size_t wrote = std::fwrite(text.data(), 1, text.size(), f);
        std::fflush(f);
#ifdef _WIN32
        _commit(_fileno(f));
#endif
        std::fclose(f);
        if (wrote != text.size()) {
            r.error = "short write: " + tmp;
            return r;
        }
    }

    // ---- 2. 备份旧档 ----
    // ⚠ 失败**不阻断**：`.bak` 被占用（别的进程开着）不该变成"存不了档"。
    if (fileExists(path)) {
        const std::string bak = path + ".bak";
        std::error_code ec;
        std::filesystem::remove(std::filesystem::u8path(bak), ec);  // 旧 bak 先清掉
        if (!replaceFile(path, bak)) VK_LOG_WARN("SAVE-BAK skipped path=%s", path.c_str());
    }

    // ---- 3. 原子替换 ----
    if (!replaceFile(tmp, path)) {
        r.error = "cannot replace: " + path;
        VK_LOG_ERROR("SAVE-WRITE failed path=%s", path.c_str());
        return r;
    }

    r.ok = true;
    r.entities = static_cast<int>(sg.entities.size());
    for (const SaveEntity& se : sg.entities)
        if (se.destroyed) ++r.destroyed;
    VK_LOG_INFO("SAVE-WRITE slot=%s entities=%d destroyed=%d bytes=%d path=%s",
                sg.slot.c_str(), r.entities, r.destroyed, static_cast<int>(text.size()),
                path.c_str());
    return r;
}

// ---------------- ⑤ 读 ----------------

SaveIoResult readSaveGame(const std::string& path, SaveGame& out) {
    SaveIoResult r;

    std::string text;
    bool usedBak = false;
    if (!readWholeFile(path, text)) {
        // 主档不存在 → 试 .bak（可能是上次写崩在 rename 之前）
        if (!readWholeFile(path + ".bak", text)) {
            r.error = "cannot open: " + path;
            return r;
        }
        usedBak = true;
        VK_LOG_WARN("SAVE-READ fallback=bak path=%s", path.c_str());
    }

    json root;
    bool parsed = true;
    try {
        root = json::parse(text);
    } catch (const std::exception&) {
        parsed = false;
    }

    if (!parsed || !saveGameFromJson(std::move(root), out, r)) {
        // 主档解析/校验失败 → 再试 .bak（只试一次，避免 .bak 也坏时无限循环）
        if (!usedBak) {
            std::string bakText;
            if (readWholeFile(path + ".bak", bakText)) {
                try {
                    json bakRoot = json::parse(bakText);
                    SaveIoResult r2;
                    if (saveGameFromJson(std::move(bakRoot), out, r2)) {
                        r2.usedBak = true;
                        VK_LOG_WARN("SAVE-READ fallback=bak path=%s reason=%s",
                                    path.c_str(), r.error.c_str());
                        return r2;
                    }
                } catch (const std::exception&) {
                }
            }
        }
        if (r.error.empty()) r.error = "save file is not valid JSON: " + path;
        VK_LOG_ERROR("SAVE-READ failed path=%s reason=%s", path.c_str(), r.error.c_str());
        return r;
    }

    r.usedBak = usedBak;
    VK_LOG_INFO("SAVE-READ slot=%s entities=%d destroyed=%d migrated=%d usedBak=%d path=%s",
                out.slot.c_str(), r.entities, r.destroyed, r.migrated, usedBak ? 1 : 0,
                path.c_str());
    return r;
}

}  // namespace scene
