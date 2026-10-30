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
    // 界面语言。**空串 = auto = 跟随系统**（2026-10-29）：
    // 以前写死 "en"，中文 Windows 上的玩家一开游戏就是英文。
    // 三种写法都算 auto：空串、`"auto"`、`"system"`（构建器给人看的写法）。
    // ⚠ 真正解析成下标在 `RuntimeApp::onInit`，这里只存原值 ——
    //   配置解析不该去问 Win32"这台机器装了什么语言"。
    std::string language = "";
    std::string quality = "high";

    // ---- UI 文档（#312）----
    // `assets/ui/docs/*.ui.json`：控件树 + 锚点 + 九宫格 + 数据绑定。
    // **空 = 不画任何文档**（HUD 仍然走上 U8 的硬编码那一套，画面与 #311
    // 之后逐位相同）。这是"新功能默认关"的守卫：不是靠 if 记得写，而是
    // 靠"没有文档就没有调用"这条结构。
    std::string uiDoc;

    // ---- 玩家外观（P1：把第三方角色模型绑到第三人称玩家上）----
    //
    // 出厂（空 model）= 现在这个胶囊占位体，行为与引入本块之前**逐位相同**。
    //
    // 为什么放在 game.json 而不是场景里：玩家实体是**运行时产物**
    // （见 RuntimeApp.cpp 的 spawnPlayerFromGameStart —— 出生点只标位置，
    // 存盘里没有玩家）。既然实体不落盘，它的外观就没有地方可写；而
    // "这个游戏的主角长什么样"本来就是**出厂设置**，不是关卡设置。
    //
    // 模型路径按资产根解析（与场景里 `kind: "fbx"` 的 path 同一套规则）。
    // .vmesh 烤制版存在时优先用它（loadFBX 内部决定），没有就走原始 FBX。
    struct PlayerVisual {
        std::string model;          // 空 = 胶囊占位体
        std::string clip;           // 空 = 模型的第一段 clip
        // 按模型高度反推胶囊 / 眼高（与编辑器 CharacterWindow 的
        // generateSkeletonBinding 同一套解剖比例）。关掉 = 用下面的死值。
        //
        // ⚠ 这是"1:1 真实尺度"的保证点：胶囊**跟着模型走**，不是让模型
        //   去迁就 1.8 m 的默认胶囊 —— 否则换个 2.05 m 的角色，模型会跟
        //   地面的接触点错位（脚陷进去或悬空）。
        bool fitBody = true;
        // fitBody=false 时用的死值（米）。与 CharacterController 的默认一致。
        float radius = 0.35f;
        float halfHeight = 0.55f;

        // 四段基础动画的 clip 名（P3：程序化生成的那四段）。
        // 空 = 该状态不切 clip（保持 defaultClip 一直播）。
        // ⚠ 名字对不上时**不报错也不沉默**：切不过去会打一行
        //   `GAME-ANIM-MISSING`，因为"模型在动但动的是走路的季节"是最难
        //   查的一类问题（姿势有、状态不对）。
        std::string idleClip = "Idle";
        std::string walkClip = "Walk";
        std::string runClip = "Run";
        std::string jumpClip = "Jump";

        // 动画状态机控制器（`assets/animators/*.anim.json`）。
        // **空 = 走上面那四个 clip 名的硬编码四态**（Locomotion，P4 的行为）。
        // 非空 = 状态机接管，上面四个 clip 名**不再生效** —— 因为"哪段动画
        // 归哪个状态"已经写在控制器的 states 里了。两者同时生效的话，
        // 每帧互相覆盖，症状是"角色在两个动作之间高频抖动"。
        //
        // 参数名约定（引擎侧写死，见 RuntimeApp::tickPlayerAnimation）：
        //   Speed(float) / Grounded(bool) / Jump(trigger)
        std::string controller;
        // 走 / 跑分界（m/s）：低于 walkSpeed 算 idle，高于 runSpeed 算 run。
        // ⚠ runSpeed **默认 0 = 自动**（= 控制器 maxSpeed × 1.15）。只有
        //   明确要做"小跑"这类中间档时才需要写死一个数。
        float walkSpeed = 0.15f;
        float runSpeed = 0.0f;
        // 切 clip 时是否把播放头归零。true = 每步都从头迈（步伐清晰），
        // false = 相位连续（不会每步"抖一下"，但停下再走会接在中间）。
        bool restartOnSwitch = true;
        // 静止多久重放一次 idle（秒）。0 = 关。
        //
        // 用户要求（2026-10-22）："停在原地如果不动长达 8 秒以上就触发
        // idle 动画，此后如果一直没移动，则每隔 8 秒继续触发"。
        // 语义见 `animation/Locomotion.h` —— 是**重放**（time 归零），
        // 不是"等 8 秒才开始播"（那样人会在上一个姿势里僵 8 秒）。
        float idleTriggerSec = 8.0f;

        // 模型的"正面"补偿角（度，绕 Z）。见 `scene::ModelInstanceOptions::
        // yawOffsetDeg` 的完整说明。
        //
        // ⚠ 2026-10-24 起这只是**兜底值**：装配时先问骨架
        //   （`assets::modelForwardYawDeg`，由"脚踝→脚趾"反解），
        //   问不出来才用这里声明的数。原因见 assets/ModelOrientation.h
        //   —— 同一个 90 对 FBX 资产是对的、对 VRM 资产正好差 180°。
        //
        // 默认 **90**：Mixamo / FBX 来源（Y-up，正面朝 +Z）经 ufbx 的
        // (x,-z,y) 映射后正面落在 **-Y**，而实体的 yaw 语义是
        // "yaw=0 → 正面朝 +X"，所以差一个 +90°。
        // 不补偿的症状是**横着平移**（人朝 -Y、往 +X 走），而不是"稍微歪"。
        // 换成已经烤成 +X 正面的资产时，把它写 0。
        float modelYaw = 90.0f;

        // ---- A4 步幅匹配（2026-10-30）----
        // 按"移动速度 ÷ 这段 clip 的自然步速"缩放播放速率，让脚步与位移
        // 对上（不滑步）。自然步速默认**从 clip 里量**，所以换 Walk 资产
        // 不用改配置；下面两个 `*StrideSpeed` 是"我知道这段是多少"的覆盖。
        //
        // ⚠ 关掉 = 退回 1× 播放（P4 的行为）。留着这个位是因为它就是
        //   "滑 / 不滑"的对照组。
        bool strideMatch = true;
        // 速率夹范围：0.6~1.7 大致是"还看得出是走路"的区间。
        // 夹住说明这段动画表达不了那么快的速度（日志会喊 STRIDE-CLAMPED）。
        float strideMinRate = 0.6f;
        float strideMaxRate = 1.7f;
        // 覆盖量出来的自然步速（m/s）。0 = 自动量。
        float walkStrideSpeed = 0.0f;
        float runStrideSpeed = 0.0f;

        // ---- A4b 相位速率曲线（2026-10-30）----
        // 一圈之内按相位分配播放速度（接触锁定）。与上面的 `strideMatch`
        // 是**两层**：那个决定平均速率，这个决定一圈内怎么分配。
        //
        // ⚠ 默认**开**，必须与编辑器的 `EditorContext::m_strideCurve`
        //   **一致** —— 否则会出现"编辑器里脚钉得住、出包后滑"，
        //   而那是最难查的一类（两边的日志都正常）。
        //   关掉它的唯一理由是 A/B 对照（播放头轨迹会变）。
        bool strideCurve = true;
    };
    PlayerVisual player;

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
