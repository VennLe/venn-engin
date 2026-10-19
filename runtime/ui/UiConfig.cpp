#include "UiConfig.h"

#include "core/Logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace runtime {

namespace fs = std::filesystem;

namespace {

std::string exeDir() {
    char buf[1024] = {};
#if defined(_WIN32)
    GetModuleFileNameA(nullptr, buf, sizeof(buf));
#else
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) buf[n] = '\0';
#endif
    if (!buf[0]) return {};
    return fs::path(buf).parent_path().string();
}

// 夹紧。`lo/hi` 是"合理范围"，不是 UI 上的可选范围 —— 后者会随设计变，
// 写进配置文件校验里就变成"改一次设计要改一次校验"。
float clampF(float v, float lo, float hi, float def) {
    if (!std::isfinite(v)) return def;
    return std::max(lo, std::min(hi, v));
}

void readFloat(const nlohmann::json& j, const char* key, float lo, float hi,
               float* dst) {
    if (!j.contains(key)) return;
    const auto& v = j[key];
    if (!v.is_number()) {
        VK_LOG_WARN("UI-CONFIG key=%s ignored (not a number)", key);
        return;
    }
    *dst = clampF(v.get<float>(), lo, hi, *dst);
}

}  // namespace

std::string defaultUiConfigPath() {
    const std::string dir = exeDir();
    if (!dir.empty()) {
        const fs::path atExe = fs::path(dir) / "config" / "ui.json";
        if (fs::exists(atExe)) return atExe.string();
        // 开发期从仓库根跑：build/bin/config/ui.json 不存在，但仓库根有。
        const fs::path atCwd = fs::path("config") / "ui.json";
        if (fs::exists(atCwd)) return atCwd.string();
        return atExe.string();   // 写路径
    }
    return (fs::path("config") / "ui.json").string();
}

bool loadUiConfig(const std::string& path, UiConfig* out) {
    if (!out) return false;
    std::error_code ec;
    if (!fs::exists(path, ec)) return false;   // ① 不存在 = 静默

    std::ifstream in(path);
    if (!in) {
        VK_LOG_WARN("UI-CONFIG open failed path=%s", path.c_str());
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(ss.str());
    } catch (const std::exception& e) {
        // ② 坏文件只 WARN，整层回落默认
        VK_LOG_WARN("UI-CONFIG parse failed path=%s error=%s", path.c_str(),
                    e.what());
        return false;
    }
    if (!j.is_object()) {
        VK_LOG_WARN("UI-CONFIG root is not an object path=%s", path.c_str());
        return false;
    }

    // 键名与 C++ 字段同名，只是分组一层 "hud"。扁平也接受（手写方便）。
    const nlohmann::json& hud =
        (j.contains("hud") && j["hud"].is_object()) ? j["hud"] : j;

    readFloat(hud, "barWidth", 120.0f, 1200.0f, &out->barWidth);
    readFloat(hud, "barHeight", 6.0f, 72.0f, &out->barHeight);
    readFloat(hud, "barMargin", 0.0f, 240.0f, &out->barMargin);
    readFloat(hud, "crosshairLen", 2.0f, 60.0f, &out->crosshairLen);
    readFloat(hud, "crosshairGap", 0.0f, 60.0f, &out->crosshairGap);
    if (hud.contains("crosshair") && hud["crosshair"].is_boolean())
        out->crosshair = hud["crosshair"].get<bool>();

    VK_LOG_INFO("UI-CONFIG path=%s bar=%.0fx%.0f margin=%.0f crosshair=%d",
                path.c_str(), static_cast<double>(out->barWidth),
                static_cast<double>(out->barHeight),
                static_cast<double>(out->barMargin), out->crosshair ? 1 : 0);
    return true;
}

bool saveUiConfig(const std::string& path, const UiConfig& c) {
    nlohmann::json j;
    j["version"] = 1;
    j["hud"] = {
        {"barWidth", c.barWidth},
        {"barHeight", c.barHeight},
        {"barMargin", c.barMargin},
        {"crosshair", c.crosshair},
        {"crosshairLen", c.crosshairLen},
        {"crosshairGap", c.crosshairGap},
    };
    std::error_code ec;
    const fs::path p(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream out(path);
    if (!out) {
        VK_LOG_WARN("UI-CONFIG write failed path=%s", path.c_str());
        return false;
    }
    out << j.dump(2) << "\n";
    return true;
}

}  // namespace runtime
