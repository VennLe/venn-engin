#include "RuntimeConfig.h"

#include "assets/AssetPath.h"
#include "core/Logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>

namespace runtime {

namespace {

using json = nlohmann::json;

bool envFlag(const char* name, bool fallback) {
    const char* s = std::getenv(name);
    if (!s) return fallback;
    if (!s[0]) return fallback;
    return !(s[0] == '0' && s[1] == '\0');
}

int envInt(const char* name, int fallback) {
    if (const char* s = std::getenv(name)) {
        try {
            return std::stoi(s);
        } catch (...) {
            VK_LOG_WARN("GAME-CONFIG %s='%s' 不是整数，忽略", name, s);
        }
    }
    return fallback;
}

float envFloat(const char* name, float fallback) {
    if (const char* s = std::getenv(name)) {
        try {
            return std::stof(s);
        } catch (...) {
            VK_LOG_WARN("GAME-CONFIG %s='%s' 不是数字，忽略", name, s);
        }
    }
    return fallback;
}

std::string envStr(const char* name, const std::string& fallback) {
    if (const char* s = std::getenv(name)) return std::string(s);
    return fallback;
}

// game.json 的读取。**只有它存在且能解析**才生效；任何错误都不致命 ——
// 出货版配错一个字段不该让游戏起不来。
void applyGameJson(RuntimeConfig& cfg) {
    std::string path;
    if (const char* override = std::getenv("MYVK_RUNTIME_GAME_JSON")) {
        path = assets::resolveAssetPath(override);
    } else {
        path = assets::resolveAssetPath("game.json");
    }
    std::ifstream f(path);
    if (!f) {
        VK_LOG_INFO("GAME-CONFIG game.json 未找到（%s），用内置默认", path.c_str());
        return;
    }
    json j;
    try {
        f >> j;
    } catch (const std::exception& e) {
        VK_LOG_WARN("GAME-CONFIG game.json 解析失败（%s）：%s", path.c_str(),
                    e.what());
        return;
    }
    if (!j.is_object()) {
        VK_LOG_WARN("GAME-CONFIG game.json 顶层不是对象，忽略（%s）", path.c_str());
        return;
    }
    cfg.configPath = path;

    auto getStr = [&](const char* key, std::string& dst) {
        if (j.contains(key) && j[key].is_string()) dst = j[key].get<std::string>();
    };
    auto getInt = [&](const char* key, int& dst) {
        if (j.contains(key) && j[key].is_number_integer())
            dst = j[key].get<int>();
    };
    auto getBool = [&](const char* key, bool& dst) {
        if (j.contains(key) && j[key].is_boolean()) dst = j[key].get<bool>();
    };

    getStr("scene", cfg.scenePath);
    getStr("title", cfg.title);
    getInt("width", cfg.width);
    getInt("height", cfg.height);
    getBool("fullscreen", cfg.fullscreen);
    getInt("maxFps", cfg.maxFps);
    getInt("unfocusedFps", cfg.unfocusedFps);
    if (j.contains("timeScale") && j["timeScale"].is_number())
        cfg.timeScale = j["timeScale"].get<float>();
    getStr("language", cfg.language);
    getStr("quality", cfg.quality);
    getStr("audioDevice", cfg.audioDevice);
    getStr("startState", cfg.startState);

    VK_LOG_INFO("GAME-CONFIG loaded %s", path.c_str());
}

void applyEnv(RuntimeConfig& cfg) {
    cfg.scenePath = envStr("MYVK_RUNTIME_SCENE", cfg.scenePath);
    cfg.title = envStr("MYVK_RUNTIME_TITLE", cfg.title);
    cfg.width = envInt("MYVK_RUNTIME_WIDTH", cfg.width);
    cfg.height = envInt("MYVK_RUNTIME_HEIGHT", cfg.height);
    cfg.fullscreen = envFlag("MYVK_RUNTIME_FULLSCREEN", cfg.fullscreen);
    cfg.maxFps = envInt("MYVK_RUNTIME_MAXFPS", cfg.maxFps);
    cfg.unfocusedFps = envInt("MYVK_RUNTIME_UNFOCUSED_FPS", cfg.unfocusedFps);
    cfg.timeScale = envFloat("MYVK_RUNTIME_TIMESCALE", cfg.timeScale);
    cfg.language = envStr("MYVK_RUNTIME_LANGUAGE", cfg.language);
    cfg.quality = envStr("MYVK_RUNTIME_QUALITY", cfg.quality);
    cfg.selftest = envStr("MYVK_RUNTIME_SELFTEST", cfg.selftest);
    cfg.dumpScenePath = envStr("MYVK_RUNTIME_DUMP_SCENE", cfg.dumpScenePath);
    cfg.inputMapPath = envStr("MYVK_RUNTIME_INPUT_MAP", cfg.inputMapPath);
    cfg.startState = envStr("MYVK_RUNTIME_START_STATE", cfg.startState);
    cfg.traceStates = envFlag("MYVK_RUNTIME_TRACE_STATES", cfg.traceStates);
    // `MYVK_RUNTIME=1`：运行时 verbose（每 60 帧一行 GAME-LOOP）。
    // 这个**裸**名字是 ship_roadmap 里写定的，所以保留（白名单里已登记）。
    cfg.verbose = envFlag("MYVK_RUNTIME", cfg.verbose);
    if (const char* frames = std::getenv("MYVK_FRAMES")) {
        try {
            cfg.maxFrames = static_cast<std::uint64_t>(std::stoull(frames));
        } catch (...) {
        }
    }
}

// 命令行里参数带值的一律支持 `--key value` 与 `--key=value` 两种写法。
bool matchKey(const std::string& a, const char* key, std::string& inlineValue,
              bool& hasInline) {
    const std::string k(key);
    if (a == k) {
        hasInline = false;
        return true;
    }
    if (a.size() > k.size() + 1 && a.compare(0, k.size() + 1, k + "=") == 0) {
        inlineValue = a.substr(k.size() + 1);
        hasInline = true;
        return true;
    }
    return false;
}

void applyCli(RuntimeConfig& cfg, const std::vector<std::string>& args) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        std::string val;
        bool inlineSet = false;

        auto takeValue = [&](const char* name, std::string& dst) {
            if (inlineSet) {
                dst = val;
            } else if (i + 1 < args.size()) {
                dst = args[++i];
            } else {
                VK_LOG_WARN("GAME-CONFIG %s 缺参数值，忽略", name);
            }
        };

        if (matchKey(a, "--scene", val, inlineSet)) {
            takeValue("--scene", cfg.scenePath);
        } else if (matchKey(a, "--title", val, inlineSet)) {
            takeValue("--title", cfg.title);
        } else if (matchKey(a, "--width", val, inlineSet)) {
            std::string v;
            takeValue("--width", v);
            if (!v.empty()) cfg.width = std::atoi(v.c_str());
        } else if (matchKey(a, "--height", val, inlineSet)) {
            std::string v;
            takeValue("--height", v);
            if (!v.empty()) cfg.height = std::atoi(v.c_str());
        } else if (matchKey(a, "--fps", val, inlineSet)) {
            std::string v;
            takeValue("--fps", v);
            if (!v.empty()) cfg.maxFps = std::atoi(v.c_str());
        } else if (matchKey(a, "--frames", val, inlineSet)) {
            std::string v;
            takeValue("--frames", v);
            if (!v.empty()) cfg.maxFrames = std::strtoull(v.c_str(), nullptr, 10);
        } else if (matchKey(a, "--time-scale", val, inlineSet)) {
            std::string v;
            takeValue("--time-scale", v);
            if (!v.empty()) cfg.timeScale = static_cast<float>(std::atof(v.c_str()));
        } else if (matchKey(a, "--selftest", val, inlineSet)) {
            takeValue("--selftest", cfg.selftest);
        } else if (matchKey(a, "--dump-scene", val, inlineSet)) {
            takeValue("--dump-scene", cfg.dumpScenePath);
        } else if (matchKey(a, "--input-map", val, inlineSet)) {
            takeValue("--input-map", cfg.inputMapPath);
        } else if (matchKey(a, "--language", val, inlineSet)) {
            takeValue("--language", cfg.language);
        } else if (matchKey(a, "--start-state", val, inlineSet)) {
            takeValue("--start-state", cfg.startState);
        } else if (a == "--fullscreen") {
            cfg.fullscreen = true;
        } else if (a == "--windowed") {
            cfg.fullscreen = false;
        } else if (a == "--unlocked") {
            // 关掉帧率上限（交给交换链的呈现模式）
            cfg.maxFps = 0;
        } else if (a == "--verbose") {
            cfg.verbose = true;
        } else if (a == "--trace-states") {
            cfg.traceStates = true;
        }
        // 未知参数不报错：window / 快捷方式里塞进来的东西不该让游戏起不来。
    }
}

} // namespace

RuntimeConfig parseRuntimeConfig(const std::vector<std::string>& args) {
    RuntimeConfig cfg;
    applyGameJson(cfg);
    applyEnv(cfg);
    applyCli(cfg, args);

    // 兜底：窗口尺寸不能是 0 / 负数（否则 GLFW 创建失败，报错很难懂）
    cfg.width = std::max(160, cfg.width);
    cfg.height = std::max(120, cfg.height);
    cfg.timeScale = std::max(0.0f, cfg.timeScale);

    VK_LOG_INFO("GAME-CONFIG scene='%s' window=%dx%d fullscreen=%d "
                "maxFps=%d unfocusedFps=%d timeScale=%.2f language=%s "
                "quality=%s startState=%s selftest='%s' verbose=%d "
                "source='%s'",
                cfg.scenePath.c_str(), cfg.width, cfg.height,
                cfg.fullscreen ? 1 : 0, cfg.maxFps, cfg.unfocusedFps,
                static_cast<double>(cfg.timeScale), cfg.language.c_str(),
                cfg.quality.c_str(), cfg.startState.c_str(),
                cfg.selftest.empty() ? "-" : cfg.selftest.c_str(),
                cfg.verbose ? 1 : 0,
                cfg.configPath.empty() ? "(defaults)" : cfg.configPath.c_str());
    return cfg;
}

const char* runtimeHelpText() {
    return
        "Venn Game (VennRuntime)\n"
        "\n"
        "用法: Game.exe [选项]\n"
        "\n"
        "  --scene <path>        起始场景（相对资产根，或绝对路径）\n"
        "  --width  <n>          窗口宽（默认 1280）\n"
        "  --height <n>          窗口高（默认 720）\n"
        "  --fullscreen | --windowed\n"
        "  --fps <n>             帧率上限，0 = 不限（默认 60）\n"
        "  --unlocked            等价于 --fps 0\n"
        "  --frames <n>          跑满 n 帧后退出（冒烟测试）\n"
        "  --time-scale <f>      时间缩放（0 = 停住，1 = 正常）\n"
        "  --language <en|zh>    界面语言\n"
        "  --start-state <s>     boot | mainmenu | playing（默认 playing）\n"
        "  --verbose             每 60 帧打一行 GAME-LOOP\n"
        "  --trace-states        每次状态迁移打一行 GAME-STATE\n"
        "  --selftest <name>     无窗口自检后退出: states | determinism | input |\n"
        "                        character | contact | camera | player\n"
        "  --dump-scene <path>   把读进来的场景再写一份（读→写往返自检）\n"
        "  --input-map <path>    键位绑定表（默认 assets/input_map.json）\n"
        "  --help                显示这段文字\n"
        "\n"
        "配置优先级: 命令行 > 环境变量(MYVK_RUNTIME_*) > assets/game.json > 默认\n"
        "日志落在 %LOCALAPPDATA%/Venn/<game>/logs/，不写安装目录。\n";
}

} // namespace runtime
