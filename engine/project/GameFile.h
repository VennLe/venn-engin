#pragma once
// ============================================================
// project/GameFile —— `assets/game.json` 的读写（编辑器侧，2026-11-02）
//
// ------------------------------------------------------------
// 它补的是什么洞
// ------------------------------------------------------------
// `assets/game.json` 是**出厂设置**的权威（标题 / 分辨率 / 全屏 / 帧率上限 /
// 语言 / 场景 / UI 文档 / 音频出厂值）。在 2026-11-02 之前它**只能手改**：
//
//     grep -rn "game.json" editor/   →  只有一句 tooltip 提到它
//
// 而 `Edit ▸ Project Settings...` 打开的是**画质档位**那张面板（E26）。
// 一个策划要改游戏标题，得知道这个文件在哪、键叫什么、值的类型是什么。
//
// ------------------------------------------------------------
// ⚠⚠ 为什么不在内存里做"第二个配置"
// ------------------------------------------------------------
// `ProjectFile.h` 里已经写死过这条纪律：**不存第二份配置**。所以这里
// 刻意做成"**按需读盘 → 改 → 写回同一份文件**"，而不是"编辑器启动时读一份、
// 面板改内存、退出时回写"。落盘即真值，`Reload` 就是重新读文件。
//
// ⚠ 为什么**不**把它做成 `runtime::RuntimeConfig` 的共享结构体
//   （那样"读"就真的只有一份了）：`game.json` 的 `player` 块有 **25 个字段**
//   （步幅匹配 / 相位曲线 / 四段 clip 名 / 模型朝向……），把整块抽成共享 POD
//   要动运行时唯一的配置读路径，收益是 0 —— 面板根本不编辑 `player`。
//   真正防漂移的是**端到端判据**：`tools/verify_project_settings.py` 的 C 组
//   用面板写进文件、再起 `Game.exe` 断言它读到的是同一批值。
//   "两边各写一份解析"这种事只有判据能管住，抽结构体管不住（抽完之后
//   键名仍然可以各写一个字符串）。
//
// ------------------------------------------------------------
// 三条语义
// ------------------------------------------------------------
// ① **未知键必须保留**。`game.json` 里有 `_comment`、`player`、`audioDevice`
//    （废弃别名）、以及以后会有的新块。写回时是**合并**（读现有文档 → 覆盖
//    我们管的键 → 写回），不是整份重写。整份重写的后果是"改了个标题，
//    玩家的模型配置没了"，而那种丢字段不会报错。
//
// ② **`language` 的空串是"跟随系统"**，不是"没填"。写回时也要能写空串 ——
//    所以面板的选项是 `auto / en / zh` 三档，`auto` 落盘成 `""`。
//
// ③ **`quality` 只是标签**。真正的画质值在 `config/render.json`
//    （见 `QualitySettingsPanel.h` 的第 ③ 条）。这里写它只是让启动器/运行时
//    知道"这个工程想用哪一档"；标签与实际值不一致时**不反向纠正**。
//
// ------------------------------------------------------------
// 日志锚点（`tools/verify_project_settings.py` 用）
// ------------------------------------------------------------
//   PROJ-GAME-LOAD   path=… ok=… created=… title=… w=… h=…
//   PROJ-GAME-SAVE   path=… ok=… bytes=… keys=…
//   PROJ-GAME-CLAMP  field=… from=… to=…
// ============================================================

#include <string>

namespace project {

// `game.json` 的 `audio` 块（G14-5）。
//
// ⚠ 这是**出厂值**，不是"现在的音量"。`AudioSystem::busGain()` 才是当前的
//   真值 —— 混音台改的是那个（不落盘）。两者同名的字段在这里只表示
//   "下次启动时初始成多少"，`AudioMixerPanel` 里那条只读提示与此同源。
struct GameAudio {
    bool enabled = true;
    std::string device = "default";
    float master = 1.0f;
    float music = 0.7f;
    float sfx = 1.0f;
    float voice = 1.0f;
    bool mute = false;
    bool muteOnFocusLost = false;
    float duckDepth = 0.75f;
    int maxVoices = 64;
};

// `game.json` 的 `player` 块里**面板编辑的那一部分**（2026-11-03）。
//
// ⚠ 这**不是** player 的全集：真实字段有 25+ 个（步幅匹配 / 相位曲线 /
//   走跑速度 / 模型朝向 / 弹簧骨……）。这里只挑"让一个角色第一次出现在
//   场景里"必需的六项。其余字段靠 `saveGameFile` 的**合并写回**原样保留
//   （见本文件头注释 ①）—— 所以这个结构体小一点是安全的，多一点是危险。
//
// ⚠ `present = false` = "这个工程没配过角色"。此时保存**不会凭空长出**
//   `player` 块：老工程存一次盘就多一段配置，属于"不出错但 diff 很脏"的
//   改动，而且会让"这个工程到底配没配角色"变得无法从磁盘判断。
struct GamePlayer {
    bool present = false;
    std::string model;       // 相对 assets/；空 = 胶囊占位体（引擎默认）
    std::string clip;        // 默认 clip；空 = 模型的第一段
    bool fitBody = true;     // 胶囊按模型高度反推（1:1 真实尺度）
    std::string controller;  // `assets/animators/*.anim.json`；空 = 四态硬编码
    std::string idleClip = "Idle";
    std::string walkClip = "Walk";
    std::string runClip = "Run";
    std::string jumpClip = "Jump";
};

// 面板编辑的那一部分（**不是** game.json 的全集，见头注释）。
struct GameSettings {
    // ---- 通用 ----
    std::string scene = "scenes/sandbox.json";
    std::string title = "Venn Game";
    int width = 1280;
    int height = 720;
    bool fullscreen = false;
    std::string startState = "playing";
    std::string uiDoc;             // 空 = 不画界面层

    // ---- 循环 ----
    int maxFps = 60;
    int unfocusedFps = 15;
    float timeScale = 1.0f;

    // ---- 本地化 ----
    std::string language;          // "" = auto（跟随系统）

    // ---- 画质标签（真值在 config/render.json）----
    std::string quality = "high";

    // ---- 音频出厂值 ----
    GameAudio audio;

    // ---- 玩家外观（P1 / 2026-11-03 起可在面板里改）----
    GamePlayer player;
};

struct GameFileIo {
    bool ok = false;
    std::string path;      // 实际读/写的绝对路径
    std::string error;     // 失败原因（人话，只在不 ok 时有内容）
    bool created = false;  // 文件不存在 → 交出内置默认（**不算失败**）
    int bytes = 0;         // 写出去的大小
};

// 读。文件不存在 → `ok=true, created=true`，`out` 是内置默认。
// 解析失败 / 顶层不是对象 → `ok=false`（**不**静默用默认：那会让面板显示
// 一批"看起来是工程配置、其实是出厂值"的数字，然后一次保存把它们写进盘）。
GameFileIo loadGameFile(const std::string& path, GameSettings& out);

// 写。**合并**（见头注释 ①），原子（临时文件 → rename）。
GameFileIo saveGameFile(const std::string& path, const GameSettings& in);

// 校验并把越界值夹回合法区间，返回被夹的条数（每夹一条打一行
// `PROJ-GAME-CLAMP`）。`notes` 非空时把 "field from→to" 追加进去。
//
// 为什么要夹而不是拒：这些值**不是**用户随手输入的错误，而是"拖太快 /
// 粘贴进一个 0 / 老文件里有个荒唐值"。拒掉会让整个面板卡在保存不了的状态，
// 而作者不知道是哪一格。夹取 + 明说是唯一不会把人堵死的做法。
int clampGameSettings(GameSettings& s, std::string* notes);

// `language` 的规范化：面板写 "auto" / "system" / "" 都算 auto（落盘成 ""）。
// 返回规范后的值，并告诉调用方它是不是 auto。
std::string normalizeLanguage(const std::string& in, bool* isAuto = nullptr);
// 语言下拉的三档（面板与判据共用同一张表，别各写一份）
const char* const* languageChoices(int* count);

// 自检（G 组）。挂在 `runAllProjectSelfTests()` 里。
int runGameFileSelfTest();

} // namespace project
