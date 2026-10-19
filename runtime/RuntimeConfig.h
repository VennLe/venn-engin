#pragma once
// ============================================================
// runtime/RuntimeConfig —— Game.exe 的启动配置
//
// ------------------------------------------------------------
// 优先级（高的盖低的）
// ------------------------------------------------------------
//     命令行  >  环境变量(MYVK_RUNTIME_*)  >  assets/game.json  >  内置默认
//
// 为什么是这个顺序：命令行是"这一次运行"的意图（调试 / 快捷方式参数），
// env 是"这一台机器 / 这个脚本"的意图（自动化注入），game.json 是"这个游戏
// 的出厂设置"（会随版本走），默认值只兜底。
//
// ------------------------------------------------------------
// 运行时不读编辑器的配置
// ------------------------------------------------------------
// `config/editor_layout.json` / `editor_prefs.json` 是**编辑器**的界面存档，
// Game.exe 一律不读（读了就会"继承上一台机器上编辑器的窗口布局"，荒谬且
// 难排查）。
//
// ------------------------------------------------------------
// env 名一律带 `MYVK_RUNTIME_` 前缀
// ------------------------------------------------------------
// `tools/check_feature_surface.py` 按前缀放行 `MYVK_EDITOR_*`；运行时的开关
// 走同样的设计（`MYVK_RUNTIME_*`），这样新增一个运行时开关不需要每次去白
// 名单里补一行（项目铁律：裸 env 漏网必须清零）。
// ============================================================

#include "audio/AudioSystem.h"   // AudioSettings（G14-5）

#include <cstdint>
#include <string>
#include <vector>

namespace runtime {

struct RuntimeConfig {
    // ---- 场景 ----
    // 空 = 不加载任何场景（空世界）。相对路径按资产根解析
    // （见 assets/AssetPath.h），绝对路径原样用。
    std::string scenePath;

    // ---- 窗口 ----
    std::string title = "Venn";
    int width = 1280;
    int height = 720;
    bool fullscreen = false;

    // ---- 循环 ----
    int maxFps = 60;
    int unfocusedFps = 15;
    float timeScale = 1.0f;
    // > 0 → 跑满 N 帧自动退出（冒烟测试 / 回归用）
    std::uint64_t maxFrames = 0;

    // ---- 内容 ----
    std::string language = "en";
    std::string quality = "high";

    // ---- 音频（G14-5）----
    // 出厂默认来自 game.json 的 `audio` 块，可被 `MYVK_RUNTIME_AUDIO_*` 覆盖。
    //
    // ⚠ "产品默认开 / 自动化默认关"（docs/audio_roadmap.md §3.2）：
    //   `--selftest` / `--frames`（= 脚本在跑）时**默认不开设备**，
    //   否则每次回归都会去抢一次 WASAPI 的共享设备，还会往日志里塞
    //   `AUDIO-INIT`，把既有基线搅花。要真开就显式给 `MYVK_RUNTIME_AUDIO=1`。
    audio::AudioSettings audio;

    // ⚠ **已废弃**：`game.json` 的老字段，只作为兼容别名读一次
    //   （用户机器上的 game.json 可能已经被改过，直接删会让他的设备选择
    //    静默失效）。读到非默认值会打一行 `GAME-CONFIG-DEPRECATED`，
    //   真正的权威是上面的 `audio.device`。
    std::string audioDevice = "default";

    // ---- 诊断 ----
    // 非空 = 启动时把**读进来**的场景再写一份到这个路径（往返自检用）。
    // 存在的意义：`saveScene`（写路径）与 `loadScene`（读路径）是两套代码，
    // 只测读、不测写，“存档丢了新字段”这种 bug 会一直躲着。回归脚本
    // `verify_runtime_scene.py` 用它做 读→写→再读 的闭环。
    std::string dumpScenePath;
    // 键位绑定表路径（G06）。空 = 用相对资产根的默认 `input_map.json`。
    //
    // 与编辑器**同一份文件**：Preferences ▸ Bindings 写它，这里读它。
    // 文件不存在 = 用 `core::InputMap::makeDefault()` 的出厂绑定（正常状态，
    // 不是错误）。
    std::string inputMapPath;
    // 启动后停在哪个状态：boot | mainmenu | playing（默认 playing）。
    // M1 还没有菜单 UI（G15），所以默认跳过 MainMenu 直接进游戏；
    // `mainmenu` 主要给状态栈的验证用。
    std::string startState = "playing";
    // 非空 = 跑一项无窗口自检然后退出（见 runtime/main.cpp）：
    //   "states"      状态栈语义 + 迁移序列
    //   "determinism" 定步长累加器 + 物理的逐位可复现
    std::string selftest;
    // 每次状态迁移打一行 GAME-STATE
    bool traceStates = false;
    // 每 60 帧打一行 GAME-LOOP（`MYVK_RUNTIME=1` 或 `--verbose`）
    bool verbose = false;

    // ---- 落盘信息（日志用，不参与决策）----
    std::string configPath;   // 实际读到的 game.json（空 = 没读）
};

// 解析：CLI > env > game.json > 默认。
// `args` 是 argv[1..]（不含可执行文件名）。
// 不抛异常：坏掉的 game.json 只打 WARN 并继续用默认值。
RuntimeConfig parseRuntimeConfig(const std::vector<std::string>& args);

// `--help` 的正文
const char* runtimeHelpText();

} // namespace runtime
