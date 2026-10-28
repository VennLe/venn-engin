#include "RuntimeConfig.h"

#include "assets/AssetPath.h"
#include "core/Build.h"
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
    // #312：UI 文档（`assets/ui/docs/*.ui.json`）。空 = 不画。
    getStr("uiDoc", cfg.uiDoc);
    getStr("audioDevice", cfg.audioDevice);
    getStr("startState", cfg.startState);

    // ---- 音频块（G14-5）----
    if (j.contains("audio") && j["audio"].is_object()) {
        const json& a = j["audio"];
        auto aBool = [&](const char* k, bool& dst) {
            if (a.contains(k) && a[k].is_boolean()) dst = a[k].get<bool>();
        };
        auto aNum = [&](const char* k, float& dst) {
            if (a.contains(k) && a[k].is_number()) dst = a[k].get<float>();
        };
        aBool("enabled", cfg.audio.enabled);
        if (a.contains("device") && a["device"].is_string())
            cfg.audio.device = a["device"].get<std::string>();
        aNum("master", cfg.audio.master);
        aNum("music", cfg.audio.music);
        aNum("sfx", cfg.audio.sfx);
        aNum("voice", cfg.audio.voice);
        aBool("mute", cfg.audio.mute);
        aBool("muteOnFocusLost", cfg.audio.muteOnFocusLost);
        aNum("duckDepth", cfg.audio.duckDepth);
        if (a.contains("maxVoices") && a["maxVoices"].is_number_integer())
            cfg.audio.maxVoices = a["maxVoices"].get<int>();
    } else if (j.contains("audio")) {
        VK_LOG_WARN("GAME-CONFIG game.json 的 audio 不是对象，忽略（%s）",
                    path.c_str());
    }

    // ---- 玩家外观（P1）----
    // 一个块都没写 = 胶囊占位体（与引入本块之前逐位相同）。
    if (j.contains("player") && j["player"].is_object()) {
        const json& p = j["player"];
        auto pStr = [&](const char* k, std::string& dst) {
            if (p.contains(k) && p[k].is_string()) dst = p[k].get<std::string>();
        };
        auto pBool = [&](const char* k, bool& dst) {
            if (p.contains(k) && p[k].is_boolean()) dst = p[k].get<bool>();
        };
        auto pNum = [&](const char* k, float& dst) {
            if (p.contains(k) && p[k].is_number()) dst = p[k].get<float>();
        };
        pStr("model", cfg.player.model);
        pStr("clip", cfg.player.clip);
        pBool("fitBody", cfg.player.fitBody);
        pNum("radius", cfg.player.radius);
        pNum("halfHeight", cfg.player.halfHeight);
        pStr("idleClip", cfg.player.idleClip);
        pStr("walkClip", cfg.player.walkClip);
        pStr("runClip", cfg.player.runClip);
        pStr("jumpClip", cfg.player.jumpClip);
        pStr("controller", cfg.player.controller);
        pNum("walkSpeed", cfg.player.walkSpeed);
        pNum("runSpeed", cfg.player.runSpeed);
        pBool("restartOnSwitch", cfg.player.restartOnSwitch);
        pNum("idleTrigger", cfg.player.idleTriggerSec);
        pNum("modelYaw", cfg.player.modelYaw);
    } else if (j.contains("player")) {
        VK_LOG_WARN("GAME-CONFIG game.json 的 player 不是对象，忽略（%s）",
                    path.c_str());
    }

    // ---- 老字段兼容（只读一次，别静默丢掉用户的选择）----
    if (cfg.audioDevice != "default" && cfg.audioDevice != cfg.audio.device) {
        VK_LOG_WARN("GAME-CONFIG-DEPRECATED audioDevice='%s' 已废弃，"
                    "请改用 audio.device（本次仍按 audioDevice 生效）",
                    cfg.audioDevice.c_str());
        cfg.audio.device = cfg.audioDevice;
    }

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
    // #312：UI 文档。自动化钩子（`MYVK_RUNTIME_` 前缀天然进白名单）。
    cfg.uiDoc = envStr("MYVK_RUNTIME_UI_DOC", cfg.uiDoc);

    // ---- 音频（G14-5）----
    // ⚠ 全套带 `MYVK_RUNTIME_AUDIO` 前缀（`check_feature_surface.py` 按前缀
    //   放行，新增一个开关不用去白名单补一行）。
    cfg.audio.enabled =
        envFlag("MYVK_RUNTIME_AUDIO", cfg.audio.enabled);
    cfg.audio.device =
        envStr("MYVK_RUNTIME_AUDIO_DEVICE", cfg.audio.device);
    cfg.audio.master = envFloat("MYVK_RUNTIME_AUDIO_MASTER", cfg.audio.master);
    cfg.audio.music = envFloat("MYVK_RUNTIME_AUDIO_MUSIC", cfg.audio.music);
    cfg.audio.sfx = envFloat("MYVK_RUNTIME_AUDIO_SFX", cfg.audio.sfx);
    cfg.audio.voice = envFloat("MYVK_RUNTIME_AUDIO_VOICE", cfg.audio.voice);
    cfg.audio.mute = envFlag("MYVK_RUNTIME_AUDIO_MUTE", cfg.audio.mute);
    cfg.audio.duckDepth =
        envFloat("MYVK_RUNTIME_AUDIO_DUCK", cfg.audio.duckDepth);
    cfg.audio.maxVoices =
        envInt("MYVK_RUNTIME_AUDIO_MAXVOICES", cfg.audio.maxVoices);
    // `=1` 强制 NullBackend：无声卡机器 / 想验证"链路通但不发声"
    cfg.audio.forceNull = envFlag("MYVK_RUNTIME_AUDIO_NULL", cfg.audio.forceNull);
    // ⚠ `_NULL=1` 隐含"我要音频"：明确点了 NullBackend 却说"没开音频"、
    //   于是什么也不做，是最容易让人以为开关坏了的一种组合。
    //   但在自动化默认关的前提下，没有这一条 `_NULL=1` 单独用是**静默无效**的
    //   （`_NULL` 只管后端，不管 enabled）。显式 `MYVK_RUNTIME_AUDIO=0` 仍然优先。
    if (cfg.audio.forceNull && !std::getenv("MYVK_RUNTIME_AUDIO"))
        cfg.audio.enabled = true;
    cfg.audio.retryOnFailure =
        envFlag("MYVK_RUNTIME_AUDIO_RETRY", cfg.audio.retryOnFailure);

    // ------------------------------------------------------------
    // 开发钩子（G30）：shipping 版**不读**这些环境变量。
    // ------------------------------------------------------------
    // 为什么要 `#if`（编译期）而不是 `if (kShipping)`（运行期）：
    //   ① 变量名本身就是信息 —— 出货二进制里不该出现
    //      `MYVK_RUNTIME_SELFTEST` 这种字符串（判据按字节比对）；
    //   ② 运行期判断还得把"我忽略了"这件事写进日志，而那句话又把名字
    //      带回了二进制 —— 绕一圈等于没做。
    // 所以出货版里这些开关**连名字都不存在**，设了它们跟没设一样，
    // 表现为"游戏照常跑"（而不是报错），这正是玩家该看到的。
#if VENN_SHIPPING
    // 保留 `MYVK_FRAMES`（下方）：它只是"跑满 N 帧退出"，是自动化收尾
    // 用的，不是玩家能拿来作弊的面；去掉它只会让出货版的用例只能靠
    // 超时 kill 收场（日志尾巴丢掉、退出码没意义）。
#else
    cfg.selftest = envStr("MYVK_RUNTIME_SELFTEST", cfg.selftest);
    cfg.dumpScenePath = envStr("MYVK_RUNTIME_DUMP_SCENE", cfg.dumpScenePath);
    cfg.traceStates = envFlag("MYVK_RUNTIME_TRACE_STATES", cfg.traceStates);
#endif

    cfg.inputMapPath = envStr("MYVK_RUNTIME_INPUT_MAP", cfg.inputMapPath);
    cfg.startState = envStr("MYVK_RUNTIME_START_STATE", cfg.startState);
    // 玩家外观：自动化只有一个"换成这个模型"的诉求，所以只开两个口子
    // （模型 / clip）。其余字段是美术与手感参数，属于 game.json 的范畴。
    cfg.player.model = envStr("MYVK_RUNTIME_PLAYER_MODEL", cfg.player.model);
    cfg.player.clip = envStr("MYVK_RUNTIME_PLAYER_CLIP", cfg.player.clip);
    // 状态机控制器：自动化要能"换成这个控制器跑一遍"（与换模型同一个诉求）。
    cfg.player.controller =
        envStr("MYVK_RUNTIME_CONTROLLER", cfg.player.controller);
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
        // G30：`--selftest` / `--dump-scene` 是开发钩子 —— shipping 版里
        // **整条分支都不存在**（连参数名这个字符串都不进二进制）。设了它们
        // = 未识别参数 = 照常启动游戏。判据见 tools/verify_shipping.py。
#if !VENN_SHIPPING
        } else if (matchKey(a, "--selftest", val, inlineSet)) {
            takeValue("--selftest", cfg.selftest);
        } else if (matchKey(a, "--dump-scene", val, inlineSet)) {
            takeValue("--dump-scene", cfg.dumpScenePath);
#endif
        } else if (matchKey(a, "--input-map", val, inlineSet)) {
            takeValue("--input-map", cfg.inputMapPath);
        } else if (matchKey(a, "--language", val, inlineSet)) {
            takeValue("--language", cfg.language);
        } else if (matchKey(a, "--ui-doc", val, inlineSet)) {
            takeValue("--ui-doc", cfg.uiDoc);
        } else if (matchKey(a, "--start-state", val, inlineSet)) {
            takeValue("--start-state", cfg.startState);
        } else if (matchKey(a, "--audio", val, inlineSet)) {
            std::string v;
            takeValue("--audio", v);
            // on/off/1/0 都认（快捷方式里怎么写都有可能）
            if (!v.empty())
                cfg.audio.enabled = !(v == "0" || v == "off" || v == "false");
        } else if (matchKey(a, "--audio-device", val, inlineSet)) {
            takeValue("--audio-device", cfg.audio.device);
        } else if (matchKey(a, "--audio-null", val, inlineSet)) {
            (void)val;
            (void)inlineSet;
            cfg.audio.forceNull = true;
        } else if (a == "--mute") {
            cfg.audio.mute = true;
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

    // ---- 音频：自动化默认关（§3.2）----
    //
    // ⚠ 判断"是不是自动化"只看 `selftest` / `maxFrames` —— 这两个都是
    //   **脚本注入的**，人手玩不会带。env 里显式给了 `MYVK_RUNTIME_AUDIO`
    //   就完全听用户的（包括 `=1`）。
    //
    //   为什么必须有这一条：回归一轮要起几十次 Game.exe，如果每次都去开
    //   共享模式的 WASAPI，既有用例会被"设备偶尔被占用"这种无关噪声搞出
    //   时红时绿，而且日志里凭空多出 AUDIO-INIT 行。
    if (!std::getenv("MYVK_RUNTIME_AUDIO") &&
        !std::getenv("MYVK_RUNTIME_AUDIO_NULL")) {
        const bool automation = !cfg.selftest.empty() || cfg.maxFrames > 0;
        if (automation) cfg.audio.enabled = false;
    }

    // 音量底数夹一下：负数会让相位反相（听起来像"声音从背后出来"）
    auto clamp01 = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
    cfg.audio.master = clamp01(cfg.audio.master);
    cfg.audio.music = clamp01(cfg.audio.music);
    cfg.audio.sfx = clamp01(cfg.audio.sfx);
    cfg.audio.voice = clamp01(cfg.audio.voice);
    cfg.audio.duckDepth = clamp01(cfg.audio.duckDepth);
    if (cfg.audio.maxVoices < 1) cfg.audio.maxVoices = 1;
    if (cfg.audio.maxVoices > 64) cfg.audio.maxVoices = 64;

    // 解析结果必须能被自动化**只读地**核对（G14-5 的验收就靠它）：
    // 一行、无空格、字段固定 —— `verify_audio.py` 拿它比对 env / game.json
    // 有没有真的生效。
    {
        std::string dev = cfg.audio.device;
        for (char& c : dev)
            if (c == ' ' || c == '\t') c = '_';
        VK_LOG_INFO("AUDIO-CONFIG enabled=%d device=%s null=%d master=%.2f "
                    "music=%.2f sfx=%.2f voice=%.2f mute=%d duck=%.2f "
                    "maxvoices=%d retry=%d",
                    cfg.audio.enabled ? 1 : 0, dev.c_str(),
                    cfg.audio.forceNull ? 1 : 0,
                    static_cast<double>(cfg.audio.master),
                    static_cast<double>(cfg.audio.music),
                    static_cast<double>(cfg.audio.sfx),
                    static_cast<double>(cfg.audio.voice),
                    cfg.audio.mute ? 1 : 0,
                    static_cast<double>(cfg.audio.duckDepth),
                    cfg.audio.maxVoices, cfg.audio.retryOnFailure ? 1 : 0);
    }

    VK_LOG_INFO("GAME-CONFIG scene='%s' window=%dx%d fullscreen=%d "
                "maxFps=%d unfocusedFps=%d timeScale=%.2f language=%s "
                "quality=%s startState=%s selftest='%s' verbose=%d "
                "uiDoc='%s' source='%s'",
                cfg.scenePath.c_str(), cfg.width, cfg.height,
                cfg.fullscreen ? 1 : 0, cfg.maxFps, cfg.unfocusedFps,
                static_cast<double>(cfg.timeScale), cfg.language.c_str(),
                cfg.quality.c_str(), cfg.startState.c_str(),
                cfg.selftest.empty() ? "-" : cfg.selftest.c_str(),
                cfg.verbose ? 1 : 0, cfg.uiDoc.c_str(),
                cfg.configPath.empty() ? "(defaults)" : cfg.configPath.c_str());
    return cfg;
}

const char* runtimeHelpText() {
    // ⚠ 拼成一个 std::string 而不是一个大字面量：开发钩子那几行要在
    //   shipping 里**整段消失**（`--help` 的输出也是判据之一）。复制一份
    //   完整文本到 `#else` 里必然与前一份漂，所以只切开发那几行。
    static const std::string text = [] {
        std::string s =
            "Venn Game (VennRuntime)\n"
            "构建: ";
        s += core::buildStamp();
        s += "\n"
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
             "  --ui-doc <path>       UI 文档（.ui.json）；不给 = 不画界面层\n"
             "  --start-state <s>     boot | mainmenu | playing（默认 playing）\n"
             "  --verbose             每 60 帧打一行 GAME-LOOP\n"
             "  --version             打印版本 / 构建风味 / git 号后退出\n";
#if !VENN_SHIPPING
        s +=
            "  --trace-states        每次状态迁移打一行 GAME-STATE\n"
            "  --selftest <name>     无窗口自检后退出: states | determinism | input |\n"
            "                        character | contact | camera | player |\n"
            "                        springarm | hud | audio |\n"
            "                        material | lod | terrain | script | scriptbench |\n"
            "                        project（工程数据模型）| audio-config（只打印音频配置）\n"
            "  --dump-scene <path>   把读进来的场景再写一份（读→写往返自检）\n";
#endif
        s +=
            "  --audio <on|off>      开 / 关声音（= MYVK_RUNTIME_AUDIO）\n"
            "  --audio-device <name> 设备名子串；__none__ = 显式无设备（不回落）\n"
            "  --audio-null          强制 NullBackend（链路通但不发声）\n"
            "  --mute                输出静音（声部照常跑，不是停播）\n"
            "  --input-map <path>    键位绑定表（默认 assets/input_map.json）\n"
            "  --help                显示这段文字\n"
            "\n"
            "配置优先级: 命令行 > 环境变量(MYVK_RUNTIME_*) > assets/game.json > 默认\n"
            "日志落在 %LOCALAPPDATA%/Venn/<game>/logs/，不写安装目录。\n";
        return s;
    }();
    return text.c_str();
}

} // namespace runtime
