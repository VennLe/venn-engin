#include "project/GameFile.h"

#include "core/Logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

namespace project {

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

// ---- 夹取区间（面板与自检共用同一张表；各写一份必然漂移）----
constexpr int kMinDim = 320;
constexpr int kMaxDim = 7680;
constexpr int kMinFps = 0;      // 0 = 不限帧（`GameLoop::applyFrameLimit`）
constexpr int kMaxFps = 1000;
constexpr float kMinScale = 0.01f;
constexpr float kMaxScale = 10.0f;
constexpr int kMinVoices = 4;
constexpr int kMaxVoices = 256;

float clampF(float v, float lo, float hi) {
    if (!(v == v)) return lo;   // NaN：`!(v==v)` 而不是 `v!=v`（-Wfloat-equal 纪律）
    return v < lo ? lo : (v > hi ? hi : v);
}

// 面板用的语言三档。**顺序即落盘语义**：第一档是 auto（落盘成空串）。
const char* const kLanguages[] = {"auto", "en", "zh"};

void note(std::string* notes, const char* field, const std::string& from,
          const std::string& to) {
    if (!notes) return;
    if (!notes->empty()) *notes += ", ";
    *notes += std::string(field) + " " + from + "→" + to;
}

} // namespace

std::string normalizeLanguage(const std::string& in, bool* isAuto) {
    // 三种写法都算 auto（与 RuntimeConfig 那条注释同源）：空串 / "auto" /
    // "system"。这里统一**落盘成空串**，因为运行时的权威写法就是空串。
    if (in.empty() || in == "auto" || in == "system") {
        if (isAuto) *isAuto = true;
        return std::string();
    }
    if (isAuto) *isAuto = false;
    return in;
}

const char* const* languageChoices(int* count) {
    if (count) *count = static_cast<int>(sizeof(kLanguages) / sizeof(kLanguages[0]));
    return kLanguages;
}

int clampGameSettings(GameSettings& s, std::string* notes) {
    int n = 0;

    auto ci = [&](const char* field, int& v, int lo, int hi) {
        const int from = v;
        v = v < lo ? lo : (v > hi ? hi : v);
        if (v != from) {
            ++n;
            note(notes, field, std::to_string(from), std::to_string(v));
            VK_LOG_INFO("PROJ-GAME-CLAMP field=%s from=%d to=%d", field, from, v);
        }
    };
    auto cf = [&](const char* field, float& v, float lo, float hi) {
        const float from = v;
        v = clampF(v, lo, hi);
        if (v != from) {
            ++n;
            char a[32];
            char b[32];
            std::snprintf(a, sizeof(a), "%.4g", static_cast<double>(from));
            std::snprintf(b, sizeof(b), "%.4g", static_cast<double>(v));
            note(notes, field, a, b);
            VK_LOG_INFO("PROJ-GAME-CLAMP field=%s from=%s to=%s", field, a, b);
        }
    };

    ci("width", s.width, kMinDim, kMaxDim);
    ci("height", s.height, kMinDim, kMaxDim);
    ci("maxFps", s.maxFps, kMinFps, kMaxFps);
    ci("unfocusedFps", s.unfocusedFps, kMinFps, kMaxFps);
    cf("timeScale", s.timeScale, kMinScale, kMaxScale);

    cf("audio.master", s.audio.master, 0.0f, 1.0f);
    cf("audio.music", s.audio.music, 0.0f, 1.0f);
    cf("audio.sfx", s.audio.sfx, 0.0f, 1.0f);
    cf("audio.voice", s.audio.voice, 0.0f, 1.0f);
    cf("audio.duckDepth", s.audio.duckDepth, 0.0f, 1.0f);
    ci("audio.maxVoices", s.audio.maxVoices, kMinVoices, kMaxVoices);

    // 标题与场景**不夹取**：空标题是合法的（运行时用 "Venn"），空场景
    // 也是合法的（空世界）。夹了反而会把"故意留空"变成"不能留空"。
    s.language = normalizeLanguage(s.language);
    return n;
}

GameFileIo loadGameFile(const std::string& path, GameSettings& out) {
    GameFileIo io;
    io.path = path;

    std::ifstream f(path);
    if (!f) {
        // 不存在 = **正常状态**（新建工程还没写过）：交出内置默认，
        // 并明确告诉调用方"这是默认值不是盘上的值"。
        io.ok = true;
        io.created = true;
        clampGameSettings(out, nullptr);
        VK_LOG_INFO("PROJ-GAME-LOAD path=%s ok=1 created=1 title=%s w=%d h=%d",
                    path.c_str(), out.title.c_str(), out.width, out.height);
        return io;
    }

    json j;
    try {
        f >> j;
    } catch (const std::exception& e) {
        io.ok = false;
        io.error = std::string("not valid JSON: ") + e.what();
        VK_LOG_ERROR("PROJ-GAME-LOAD path=%s ok=0 err=%s", path.c_str(),
                     io.error.c_str());
        return io;
    }
    if (!j.is_object()) {
        io.ok = false;
        io.error = "top level is not an object";
        VK_LOG_ERROR("PROJ-GAME-LOAD path=%s ok=0 err=%s", path.c_str(),
                     io.error.c_str());
        return io;
    }

    // ⚠ 逐字段读，且**类型不符就跳过**（不抛、不猜）：与
    //   `runtime::applyGameJson` 完全同一条规则。差别只在日志 ——
    //   这里静默跳过是有意的，面板会把"没读到"显示成默认值。
    auto gStr = [&](const char* k, std::string& dst) {
        if (j.contains(k) && j[k].is_string()) dst = j[k].get<std::string>();
    };
    auto gInt = [&](const char* k, int& dst) {
        if (j.contains(k) && j[k].is_number_integer()) dst = j[k].get<int>();
    };
    auto gBool = [&](const char* k, bool& dst) {
        if (j.contains(k) && j[k].is_boolean()) dst = j[k].get<bool>();
    };
    auto gNum = [&](const char* k, float& dst) {
        if (j.contains(k) && j[k].is_number()) dst = j[k].get<float>();
    };

    gStr("scene", out.scene);
    gStr("title", out.title);
    gInt("width", out.width);
    gInt("height", out.height);
    gBool("fullscreen", out.fullscreen);
    gInt("maxFps", out.maxFps);
    gInt("unfocusedFps", out.unfocusedFps);
    gNum("timeScale", out.timeScale);
    gStr("language", out.language);
    gStr("quality", out.quality);
    gStr("uiDoc", out.uiDoc);
    gStr("startState", out.startState);

    if (j.contains("audio") && j["audio"].is_object()) {
        const json& a = j["audio"];
        auto aBool = [&](const char* k, bool& dst) {
            if (a.contains(k) && a[k].is_boolean()) dst = a[k].get<bool>();
        };
        auto aNum = [&](const char* k, float& dst) {
            if (a.contains(k) && a[k].is_number()) dst = a[k].get<float>();
        };
        aBool("enabled", out.audio.enabled);
        if (a.contains("device") && a["device"].is_string())
            out.audio.device = a["device"].get<std::string>();
        aNum("master", out.audio.master);
        aNum("music", out.audio.music);
        aNum("sfx", out.audio.sfx);
        aNum("voice", out.audio.voice);
        aBool("mute", out.audio.mute);
        aBool("muteOnFocusLost", out.audio.muteOnFocusLost);
        aNum("duckDepth", out.audio.duckDepth);
        if (a.contains("maxVoices") && a["maxVoices"].is_number_integer())
            out.audio.maxVoices = a["maxVoices"].get<int>();
    }

    clampGameSettings(out, nullptr);
    io.ok = true;
    VK_LOG_INFO("PROJ-GAME-LOAD path=%s ok=1 created=0 title=%s w=%d h=%d",
                path.c_str(), out.title.c_str(), out.width, out.height);
    return io;
}

GameFileIo saveGameFile(const std::string& path, const GameSettings& in) {
    GameFileIo io;
    io.path = path;

    // ---- ① 先读现有文档（为了保留未知键）----
    json j = json::object();
    {
        std::ifstream f(path);
        if (f) {
            try {
                f >> j;
            } catch (const std::exception& e) {
                // ⚠ 解析失败时**拒绝写**，不"从空对象重写"。
                //   重写会静默丢掉 `player` / `_comment` / 废弃别名 ——
                //   用户改个标题就把角色模型配置弄没了，而界面上一切正常。
                io.ok = false;
                io.error = std::string("existing file is not valid JSON, refusing "
                                       "to overwrite: ") + e.what();
                VK_LOG_ERROR("PROJ-GAME-SAVE path=%s ok=0 err=%s",
                             path.c_str(), io.error.c_str());
                return io;
            }
            if (!j.is_object()) j = json::object();
        }
    }

    // ---- ② 覆盖我们管的键（不碰其余）----
    GameSettings s = in;
    clampGameSettings(s, nullptr);

    j["scene"] = s.scene;
    j["title"] = s.title;
    j["width"] = s.width;
    j["height"] = s.height;
    j["fullscreen"] = s.fullscreen;
    j["maxFps"] = s.maxFps;
    j["unfocusedFps"] = s.unfocusedFps;
    j["timeScale"] = s.timeScale;
    // ⚠ 空串也要**写出来**（不是省略）：`"language": ""` 与"没有这个键"
    //   在运行时是同义的（都是 auto），但省略会让"我把语言改回 auto 了"
    //   在磁盘上无法与"从没配过"区分，排查时看着像没保存。
    j["language"] = s.language;
    j["quality"] = s.quality;
    if (s.uiDoc.empty()) {
        // uiDoc 不同：**空就删键**。它是 #312 才加的字段，老工程里没有它，
        // 每存一次就多一行空串会让 diff 变吵。语义上两者等价（不画）。
        j.erase("uiDoc");
    } else {
        j["uiDoc"] = s.uiDoc;
    }
    j["startState"] = s.startState;

    json& a = j["audio"];
    if (!a.is_object()) a = json::object();
    a["enabled"] = s.audio.enabled;
    a["device"] = s.audio.device;
    a["master"] = s.audio.master;
    a["music"] = s.audio.music;
    a["sfx"] = s.audio.sfx;
    a["voice"] = s.audio.voice;
    a["mute"] = s.audio.mute;
    a["muteOnFocusLost"] = s.audio.muteOnFocusLost;
    a["duckDepth"] = s.audio.duckDepth;
    a["maxVoices"] = s.audio.maxVoices;

    const std::string text = j.dump(2) + "\n";
    io.bytes = static_cast<int>(text.size());

    // ---- ③ 原子写：临时文件 → rename ----
    // `std::filesystem::rename` 在 POSIX 上是原子的；MSVC 的实现走
    // `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING)`，同卷内也是原子的。
    // ⚠ 不用 `ofstream` 直接覆盖目标：截断之后如果写失败（磁盘满 / 权限），
    //   留下的是一份**半截配置文件**，下一次启动读到它会退回出厂默认。
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            io.ok = false;
            io.error = "cannot open " + tmp + " for writing";
            return io;
        }
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        f.flush();
        if (!f) {
            io.ok = false;
            io.error = "write failed: " + tmp;
            std::error_code ec;
            fs::remove(tmp, ec);
            return io;
        }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        // 跨卷 / 权限 / 目标被占用：退化成"删了再改名"，并把失败说清楚。
        fs::remove(path, ec);
        ec.clear();
        fs::rename(tmp, path, ec);
    }
    if (ec) {
        io.ok = false;
        io.error = "rename failed: " + ec.message();
        std::error_code e2;
        fs::remove(tmp, e2);
        VK_LOG_ERROR("PROJ-GAME-SAVE path=%s ok=0 err=%s", path.c_str(),
                     io.error.c_str());
        return io;
    }

    io.ok = true;
    VK_LOG_INFO("PROJ-GAME-SAVE path=%s ok=1 bytes=%d keys=%zu", path.c_str(),
                io.bytes, j.size());
    return io;
}

// ---------------------------------------------------------------- 自检

int runGameFileSelfTest() {
    int pass = 0;
    int fail = 0;

    auto check = [&](bool ok, const char* name, const std::string& detail) {
        if (ok) {
            ++pass;
            std::printf("PROJ-TEST PASS %s\n", name);
        } else {
            ++fail;
            std::printf("PROJ-TEST FAIL %s (%s)\n", name, detail.c_str());
        }
    };
    auto readAll = [](const std::string& p) {
        std::ifstream f(p, std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    };
    auto writeAll = [](const std::string& p, const std::string& t) {
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        f << t;
    };

    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec) / "venn_gamefile_test";
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);

    const std::string pa = (base / "a.json").generic_string();
    const std::string pb = (base / "b.json").generic_string();

    // ---- G1 文件不存在 → 默认值，且**不算失败** ----
    {
        GameSettings s;
        const GameFileIo io = loadGameFile(pa, s);
        check(io.ok && io.created, "G1 load-missing-uses-defaults",
              io.error.empty() ? "created flag not set" : io.error);
        check(s.title == "Venn Game" && s.width == 1280,
              "G1b missing-file-defaults", s.title);
    }

    // ---- G2 存→读 往返（含空串 language）----
    {
        GameSettings w;
        w.title = "Round Trip";
        w.width = 1600;
        w.height = 900;
        w.fullscreen = true;
        w.maxFps = 144;
        w.timeScale = 0.5f;
        w.language = "";            // auto
        w.uiDoc = "ui/docs/hud_demo.ui.json";
        w.audio.master = 0.25f;
        w.audio.maxVoices = 32;
        const GameFileIo si = saveGameFile(pa, w);
        check(si.ok, "G2 save-ok", si.error);

        GameSettings r;
        const GameFileIo li = loadGameFile(pa, r);
        check(li.ok && !li.created, "G2b reload-ok", li.error);
        const bool same = r.title == w.title && r.width == w.width &&
                          r.height == w.height && r.fullscreen == w.fullscreen &&
                          r.maxFps == w.maxFps &&
                          r.timeScale == w.timeScale &&
                          r.uiDoc == w.uiDoc &&
                          r.audio.master == w.audio.master &&
                          r.audio.maxVoices == w.audio.maxVoices;
        check(same, "G2c roundtrip-fields",
              r.title + " " + std::to_string(r.width) + "x" +
                  std::to_string(r.height));

        // 空串 language 必须**写出来**（不是被省略）
        const std::string txt = readAll(pa);
        check(txt.find("\"language\": \"\"") != std::string::npos,
              "G2d empty-language-written", txt.substr(0, 40));
        check(!txt.empty() && txt.back() == '\n', "G2e trailing-newline", "");
    }

    // ---- G3 未知键必须保留（这条最贵）----
    {
        writeAll(pb, R"({
  "_comment": "hand written",
  "player": { "model": "characters/vexa.fbx", "fitBody": false },
  "title": "Old"
})");
        GameSettings s;
        GameFileIo li = loadGameFile(pb, s);
        check(li.ok, "G3 load-with-unknown-keys", li.error);
        s.title = "New";
        const GameFileIo si = saveGameFile(pb, s);
        check(si.ok, "G3b save-merge-ok", si.error);

        const std::string txt = readAll(pb);
        check(txt.find("\"_comment\"") != std::string::npos &&
                  txt.find("hand written") != std::string::npos,
              "G3c comment-preserved", txt.substr(0, 60));
        check(txt.find("\"player\"") != std::string::npos &&
                  txt.find("vexa.fbx") != std::string::npos,
              "G3d player-block-preserved", txt.substr(0, 60));
        check(txt.find("\"title\": \"New\"") != std::string::npos,
              "G3e our-key-overwritten", "");
    }

    // ---- G4 坏 JSON：读失败、写**拒绝**（不静默清掉别人的键）----
    {
        const std::string pd = (base / "bad.json").generic_string();
        writeAll(pd, "{ this is not json ");
        GameSettings s;
        const GameFileIo li = loadGameFile(pd, s);
        check(!li.ok && !li.error.empty(), "G4 load-bad-json-fails", "accepted");

        const GameFileIo si = saveGameFile(pd, s);
        check(!si.ok, "G4b save-bad-json-refused", "overwrote a broken file");
        // 原内容一个字都不许变
        check(readAll(pd) == "{ this is not json ", "G4c bad-file-untouched",
              "content changed");
    }

    // ---- G5 夹取：每一条都成对（越界的被夹 / 合法的**不动**）----
    {
        GameSettings s;
        s.width = 100;          // < 320
        s.height = 99999;       // > 7680
        s.maxFps = -7;          // < 0
        s.unfocusedFps = 5000;  // > 1000
        s.timeScale = 0.0f;     // < 0.01
        s.audio.master = 2.5f;  // > 1
        s.audio.duckDepth = -1.0f;
        s.audio.maxVoices = 9999;
        std::string notes;
        const int n = clampGameSettings(s, &notes);
        check(n == 8, "G5 clamp-count", std::to_string(n));
        check(s.width == 320 && s.height == 7680, "G5b clamp-dim",
              std::to_string(s.width) + "x" + std::to_string(s.height));
        check(s.maxFps == 0 && s.unfocusedFps == 1000, "G5c clamp-fps",
              std::to_string(s.maxFps) + "/" + std::to_string(s.unfocusedFps));
        check(s.timeScale > 0.0099f && s.timeScale < 0.0101f, "G5d clamp-scale",
              std::to_string(s.timeScale));
        check(s.audio.master == 1.0f && s.audio.duckDepth == 0.0f,
              "G5e clamp-volume", std::to_string(s.audio.master));
        check(s.audio.maxVoices == 256, "G5f clamp-voices",
              std::to_string(s.audio.maxVoices));

        // 合法值**一个都不许动**（夹取函数的否定测试）
        GameSettings ok;
        ok.width = 1920;
        ok.height = 1080;
        ok.maxFps = 0;
        ok.timeScale = 1.0f;
        ok.audio.master = 0.0f;
        ok.audio.duckDepth = 1.0f;
        ok.audio.maxVoices = 4;
        check(clampGameSettings(ok, nullptr) == 0, "G5g clamp-leaves-legal-alone",
              "legal values were modified");
    }

    // ---- G6 语言三档 ----
    {
        bool auto1 = false;
        check(normalizeLanguage("", &auto1).empty() && auto1,
              "G6 lang-empty-is-auto", "");
        check(normalizeLanguage("auto", &auto1).empty() && auto1,
              "G6b lang-auto-normalized", "");
        check(normalizeLanguage("system", &auto1).empty() && auto1,
              "G6c lang-system-normalized", "");
        check(normalizeLanguage("zh", &auto1) == "zh" && !auto1,
              "G6d lang-zh-kept", "");
        int n = 0;
        const char* const* tab = languageChoices(&n);
        check(n == 3 && std::string(tab[0]) == "auto", "G6e three-choices",
              std::to_string(n));
    }

    // ---- G7 读进来的越界值会被夹（而不是原样交给面板）----
    {
        const std::string pe = (base / "wide.json").generic_string();
        writeAll(pe, R"({ "width": 40, "timeScale": 99.0 })");
        GameSettings s;
        const GameFileIo li = loadGameFile(pe, s);
        check(li.ok && s.width == 320, "G7 load-clamps-width",
              std::to_string(s.width));
        check(s.timeScale > 9.99f && s.timeScale < 10.01f, "G7b load-clamps-scale",
              std::to_string(s.timeScale));
    }

    fs::remove_all(base, ec);
    std::printf("PROJ-GAME-TEST SUMMARY pass=%d fail=%d\n", pass, fail);
    return fail;
}

} // namespace project
