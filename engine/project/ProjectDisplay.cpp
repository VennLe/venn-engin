#include "project/ProjectDisplay.h"

#include "core/Logger.h"
#include "render/QualityPresets.h"
#include "render/RenderSettingsIO.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace project {

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

bool writeJsonFile(const fs::path& path, const json& j, std::string* err) {
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    if (ec) {
        if (err) *err = "Cannot create " + path.parent_path().string() + ": " +
                        ec.message();
        return false;
    }
    // 与 ProjectFile::saveProject 同一条：临时文件 + rename。
    const std::string tmp = path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) {
            if (err) *err = "Cannot write " + tmp;
            return false;
        }
        f << j.dump(2) << "\n";
        if (!f.good()) {
            if (err) *err = "Write failed: " + tmp;
            return false;
        }
    }
    fs::rename(fs::path(tmp), path, ec);
    if (ec) {
        fs::remove(path, ec);
        ec.clear();
        fs::rename(fs::path(tmp), path, ec);
    }
    if (ec) {
        fs::remove(fs::path(tmp), ec);
        if (err) *err = "Cannot commit " + path.string() + ": " + ec.message();
        return false;
    }
    return true;
}

} // namespace

std::string gameJsonPath(const std::string& root) {
    return (fs::path(root) / "assets" / "game.json").generic_string();
}

std::string renderJsonPath(const std::string& root) {
    return (fs::path(root) / "config" / "render.json").generic_string();
}

IoResult readDisplay(const std::string& root, DisplayInfo& out, std::string* err) {
    out = DisplayInfo{};
    const std::string path = gameJsonPath(root);

    std::error_code ec;
    if (!fs::is_regular_file(fs::path(path), ec)) {
        // 没有 game.json = 用内置默认。**不是错误**。
        VK_LOG_INFO("DISPLAY-READ no game.json at %s (defaults)", path.c_str());
        return IoResult{true, {}};
    }

    json j;
    try {
        std::ifstream f(path);
        if (!f) {
            if (err) *err = "Cannot open " + path;
            return IoResult{false, err ? *err : "open failed"};
        }
        f >> j;
    } catch (const std::exception& e) {
        if (err) *err = std::string("game.json is not valid JSON: ") + e.what();
        VK_LOG_WARN("DISPLAY-READ bad json: %s", e.what());
        return IoResult{false, err ? *err : "bad json"};
    }
    if (!j.is_object()) {
        if (err) *err = "game.json must contain a JSON object";
        return IoResult{false, err ? *err : "bad shape"};
    }

    if (j.contains("width") && j["width"].is_number_integer())
        out.width = j["width"].get<int>();
    if (j.contains("height") && j["height"].is_number_integer())
        out.height = j["height"].get<int>();
    if (j.contains("fullscreen") && j["fullscreen"].is_boolean())
        out.fullscreen = j["fullscreen"].get<bool>();
    if (j.contains("title") && j["title"].is_string())
        out.title = j["title"].get<std::string>();
    if (j.contains("quality") && j["quality"].is_string())
        out.deprecatedQuality = j["quality"].get<std::string>();

    if (!out.deprecatedQuality.empty()) {
        // ⚠ 这条是 M4 的验收点之一：**不许**"读进来打一行日志然后什么都不做"
        //   还留着当正常配置。要么接线、要么删掉。我们选删掉（画质的权威是
        //   config/render.json），但**必须说清楚**，否则用户会以为它生效了。
        VK_LOG_WARN("GAME-CONFIG-DEPRECATED quality='%s' 已忽略 —— 画质的权威是 "
                    "config/render.json（不要再在 game.json 里写 quality）",
                    out.deprecatedQuality.c_str());
    }

    out.valid = true;
    VK_LOG_INFO("DISPLAY-READ %dx%d fullscreen=%d title='%s'", out.width,
                out.height, out.fullscreen ? 1 : 0, out.title.c_str());
    return IoResult{true, {}};
}

IoResult writeDisplay(const std::string& root, const DisplaySpec& d,
                      std::string* err) {
    const std::string path = gameJsonPath(root);

    // 读进来（缺文件就当成空对象）→ 只改这几个键 → 写回。
    json j = json::object();
    std::error_code ec;
    if (fs::is_regular_file(fs::path(path), ec)) {
        try {
            std::ifstream f(path);
            if (f) f >> j;
        } catch (const std::exception& e) {
            // 旧的坏了：**不要**在这里静默覆盖。用户可能手改坏了想让我们修，
            // 覆盖会把他剩下的东西一起丢掉；直接报错让人看见。
            if (err) *err = std::string("Existing game.json is invalid: ") + e.what();
            VK_LOG_WARN("DISPLAY-WRITE refuse to overwrite invalid game.json: %s",
                        e.what());
            return IoResult{false, err ? *err : "invalid existing json"};
        }
    }
    if (!j.is_object()) j = json::object();

    j["width"] = d.width;
    j["height"] = d.height;
    j["fullscreen"] = d.fullscreen;
    if (!d.title.empty()) j["title"] = d.title;
    // ⚠ 显式删掉废弃键（读到就清）。留着它只会让下一个人再问一遍
    //   "为什么我改 quality 没反应"。
    if (j.contains("quality")) {
        j.erase("quality");
        VK_LOG_INFO("DISPLAY-WRITE removed deprecated key 'quality'");
    }

    if (!writeJsonFile(fs::path(path), j, err)) {
        VK_LOG_WARN("DISPLAY-WRITE failed: %s", err ? err->c_str() : "?");
        return IoResult{false, err ? *err : "write failed"};
    }
    VK_LOG_INFO("DISPLAY-WRITE %s %dx%d fullscreen=%d title='%s'", path.c_str(),
                d.width, d.height, d.fullscreen ? 1 : 0, d.title.c_str());
    return IoResult{true, {}};
}

IoResult writeQuality(const std::string& root, int preset, std::string* err) {
    if (preset < 0 || preset >= render::qualityPresetCount()) {
        if (err) *err = "Invalid quality preset";
        return IoResult{false, err ? *err : "bad preset"};
    }

    // 档位 → 一层覆盖。**这份数据只有一处定义**（`render::fillLayerForPreset`），
    // 编辑器的 Quality Settings 面板落盘走的也是它 —— 否则"启动器选 High"
    // 与"编辑器点 High"会写出两份不同的 render.json（E26 收口时统一的）。
    render::SettingsLayer layer;
    const int keys = render::fillLayerForPreset(preset, layer);
    if (keys <= 0) {
        if (err) *err = "Preset has no data";
        return IoResult{false, err ? *err : "no data"};
    }

    int enabled = 0;
    for (const auto& kv : layer.values) {
        if (kv.first.size() > 8 &&
            kv.first.compare(kv.first.size() - 8, 8, ".enabled") == 0 &&
            kv.second.v[0] != 0.0)
            ++enabled;
    }

    const std::string path = renderJsonPath(root);
    if (!render::saveLayerFile(layer, path)) {
        if (err) *err = "Cannot write " + path;
        VK_LOG_WARN("QUALITY-WRITE failed path=%s", path.c_str());
        return IoResult{false, err ? *err : "write failed"};
    }

    VK_LOG_INFO("QUALITY-WRITE preset=%s path=%s keys=%zu enabled=%d",
                render::qualityPresetName(preset), path.c_str(),
                layer.values.size(), enabled);
    return IoResult{true, {}};
}

std::vector<std::string> describeWrites(const DisplaySpec& d) {
    std::vector<std::string> out;
    char buf[160];

    std::snprintf(buf, sizeof(buf),
                  "assets/game.json <- width=%d height=%d fullscreen=%s", d.width,
                  d.height, d.fullscreen ? "true" : "false");
    out.emplace_back(buf);

    if (d.qualityPreset >= 0 && d.qualityPreset < render::qualityPresetCount()) {
        // 数字与 `writeQuality` 同源（都走 fillLayerForPreset）——
        // 清单上写"22 个开关"而实际写 25 个键，是最容易让人误判的那种不一致。
        render::SettingsLayer layer;
        int keys = render::fillLayerForPreset(d.qualityPreset, layer);
        if (keys < 0) keys = 0;
        int enabled = 0;
        for (const auto& kv : layer.values)
            if (kv.first.size() > 8 &&
                kv.first.compare(kv.first.size() - 8, 8, ".enabled") == 0 &&
                kv.second.v[0] != 0.0)
                ++enabled;
        std::snprintf(buf, sizeof(buf),
                      "config/render.json <- preset=%s (%d keys, %d on)",
                      render::qualityPresetName(d.qualityPreset), keys, enabled);
        out.emplace_back(buf);
    } else {
        out.emplace_back("config/render.json <- (unchanged)");
    }
    return out;
}

// ---------------------------------------------------------------- 自检

int runDisplaySelfTest() {
    int pass = 0, fail = 0;
    auto check = [&](bool ok, const char* name, const std::string& detail) {
        if (ok) {
            ++pass;
            std::printf("DISPLAY-TEST PASS %s\n", name);
        } else {
            ++fail;
            std::printf("DISPLAY-TEST FAIL %s (%s)\n", name, detail.c_str());
        }
    };

    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec) / "venn_display_test";
    fs::remove_all(base, ec);
    fs::create_directories(base / "assets", ec);
    fs::create_directories(base / "config", ec);
    const std::string root = base.generic_string();

    // ---- J1 缺 game.json 也能读（返回默认，不是错误）----
    {
        DisplayInfo info;
        std::string err;
        const IoResult r = readDisplay(root, info, &err);
        check(r.ok && !info.valid && info.width == 1280 && info.height == 720,
              "J1 read-missing-defaults", err);
    }

    // ---- J2 写 → 读往返 ----
    {
        DisplaySpec d;
        d.width = 1920;
        d.height = 1080;
        d.fullscreen = true;
        d.title = "往返工程";
        d.qualityPreset = 2;   // High
        std::string err;
        check(writeDisplay(root, d, &err).ok, "J2 write-display", err);
        check(writeQuality(root, d.qualityPreset, &err).ok, "J3 write-quality", err);

        DisplayInfo info;
        check(readDisplay(root, info, &err).ok, "J4 read-back", err);
        check(info.width == 1920 && info.height == 1080 && info.fullscreen &&
                  info.title == "往返工程",
              "J5 read-back-values",
              std::to_string(info.width) + "x" + std::to_string(info.height));
        check(info.deprecatedQuality.empty(), "J6 no-deprecated-quality",
              info.deprecatedQuality);
    }

    // ---- J7 写 display 时**保留**其它键 ----
    {
        // 先塞一个 maxFps + 一个自定义键
        {
            json j;
            { std::ifstream f(gameJsonPath(root)); f >> j; }
            j["maxFps"] = 144;
            j["customUserKey"] = "keep me";
            std::ofstream o(gameJsonPath(root), std::ios::trunc);
            o << j.dump(2);
        }
        DisplaySpec d;
        d.width = 1280;
        d.height = 720;
        d.fullscreen = false;
        d.title = "保留测试";
        std::string err;
        writeDisplay(root, d, &err);

        json j;
        { std::ifstream f(gameJsonPath(root)); f >> j; }
        check(j.contains("maxFps") && j["maxFps"].get<int>() == 144,
              "J7 keep-maxFps", "lost maxFps");
        check(j.contains("customUserKey") &&
                  j["customUserKey"].get<std::string>() == "keep me",
              "J8 keep-unknown-key", "lost custom key");
    }

    // ---- J9 废弃 quality 键：读到要报告、写的时候要删掉 ----
    {
        json j;
        { std::ifstream f(gameJsonPath(root)); f >> j; }
        j["quality"] = "cinematic";
        { std::ofstream o(gameJsonPath(root), std::ios::trunc); o << j.dump(2); }
        DisplayInfo info;
        std::string err;
        readDisplay(root, info, &err);
        check(info.deprecatedQuality == "cinematic", "J9 detect-deprecated-quality",
              info.deprecatedQuality);

        DisplaySpec d;
        d.width = 1280;
        d.height = 720;
        writeDisplay(root, d, &err);
        json j2;
        { std::ifstream f(gameJsonPath(root)); f >> j2; }
        check(!j2.contains("quality"), "J10 write-drops-deprecated",
              "quality still present");
    }

    // ---- J11 render.json：**全量** enable 键 + E26 的数值档位项 ----
    // ⚠ 期望值是 `enableParamCount() + qualityValueCount()` —— 不是单看
    //   enable 数。E26 之后 `writeQuality` 走 `fillLayerForPreset`，写的键
    //   比 enable 多 3 个（级联数 / 距离 / 局部阴影灯数）。这条断言原来写死
    //   "== enableParamCount()"，改完 E26 当场红（J11 keys=25 want=22）。
    {
        json r;
        {
            std::ifstream f(renderJsonPath(root));
            if (f) f >> r;
        }
        int keys = 0;
        for (auto it = r.begin(); it != r.end(); ++it) {
            const std::string k = it.key();
            if (k.rfind("_", 0) == 0 || k == "version") continue;
            ++keys;
        }
        const int want = render::enableParamCount() + render::qualityValueCount();
        check(keys == want, "J11 render-all-enable-keys",
              "keys=" + std::to_string(keys) + " want=" + std::to_string(want));

        // High 档必须真把 ssao / taa 写成 true、把 dof 写成 false
        auto getBool = [&](const char* k) -> int {
            if (!r.contains(k)) return -1;
            if (!r[k].is_boolean()) return -2;
            return r[k].get<bool>() ? 1 : 0;
        };
        check(getBool("ssao.enabled") == 1, "J12 high-ssao-on",
              std::to_string(getBool("ssao.enabled")));
        check(getBool("taa.enabled") == 1, "J13 high-taa-on",
              std::to_string(getBool("taa.enabled")));
        check(getBool("dof.enabled") == 0, "J14 high-dof-off",
              std::to_string(getBool("dof.enabled")));
        check(getBool("ssgi.enabled") == 0, "J15 high-ssgi-off",
              std::to_string(getBool("ssgi.enabled")));
    }

    // ---- J16 换一档：Cinematic 应该开 dof，Low 应该关 ssao ----
    {
        std::string err;
        writeQuality(root, render::qualityPresetByName("Cinematic"), &err);
        json r;
        { std::ifstream f(renderJsonPath(root)); f >> r; }
        check(r.contains("dof.enabled") && r["dof.enabled"].get<bool>(),
              "J16 cinematic-dof-on", "dof not on");

        writeQuality(root, render::qualityPresetByName("Low"), &err);
        json r2;
        { std::ifstream f(renderJsonPath(root)); f >> r2; }
        check(r2.contains("ssao.enabled") && !r2["ssao.enabled"].get<bool>(),
              "J17 low-ssao-off", "ssao still on");
        check(r2.contains("sky.enabled") && r2["sky.enabled"].get<bool>(),
              "J18 low-sky-on", "sky off (Low should keep sky)");
    }

    // ---- J19 预设名映射（大小写敏感，认不出返回 -1）----
    {
        check(render::qualityPresetByName("High") == 2, "J19 preset-by-name",
              std::to_string(render::qualityPresetByName("High")));
        check(render::qualityPresetByName("high") == -1,
              "J20 preset-by-name-case-sensitive", "accepted lowercase");
        check(render::qualityPresetByName("Nope") == -1,
              "J21 preset-by-name-unknown", "accepted unknown");
    }

    // ---- J22 非法预设被拒 ----
    {
        std::string err;
        check(!writeQuality(root, 99, &err).ok, "J22 reject-bad-preset", "accepted");
        check(!writeQuality(root, -1, &err).ok, "J23 reject-negative-preset",
              "accepted");
    }

    fs::remove_all(base, ec);
    std::printf("DISPLAY-TEST SUMMARY pass=%d fail=%d\n", pass, fail);
    return fail;
}

} // namespace project
