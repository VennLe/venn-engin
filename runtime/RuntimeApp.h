#pragma once
// ============================================================
// runtime/RuntimeApp —— 运行时的应用层（继承 core::Application）
//
// 它把三样东西接在一起：
//   core::Application（窗口 / 交换链 / 启动消闪白 / 事件泵）
//   runtime::GameLoop（固定步长 + 相位顺序）
//   runtime::GameStateStack（阶段栈）
//
// ------------------------------------------------------------
// 为什么让 Application 保留主循环，而不是 GameLoop 自己 while
// ------------------------------------------------------------
// 见 GameLoop.h 的说明：Application 里那些"踩过坑才写对"的东西
// （隐藏创建窗口 + 首帧后才 show、最小化时 waitEvents、resize 触发交换链
// 重建）重写一遍就是在赌自己不会漏。GameLoop 因此只做**相位调度**。
//
// 唯一需要 Application 配合的地方是**渲染归属**：见
// `ownsFrameRendering()` —— 运行时自己画，因为 GameLoop 的 render 相位
// 需要一个真的调用点（否则"每帧都渲染了吗"没法计数，也谈不上"Paused
// 时画面仍在出帧"）。
// ============================================================

#include "core/Application.h"
#include "animation/AnimatorSystem.h"
// 四态 locomotion 状态机：与编辑器 Play **共用**同一份实现
// （见 engine/animation/Locomotion.h —— 以前它只长在本文件里，
//  于是编辑器 Play 只有位移、没有走路动画）。
#include "animation/Locomotion.h"

#include "GameLoop.h"
#include "GameState.h"
#include "GameStateStack.h"
#include "GameFlags.h"          // C6 第二批：剧情旗标（纯数据，无依赖）
#include "PlayerController.h"
#include "RuntimeConfig.h"

#include "core/InputMap.h"
#include "ui/Hud.h"
#include "ui/HudModel.h"
#include "ui/PauseMenu.h"
#include "ui/Subtitle.h"
#include "ui2/UiDocument.h"
#include "ui2/UiDocInput.h"   // 文档层交互（命中 / 派发）
#include "ui2/UiFocus.h"
#include "audio/AudioSystem.h"        // G14-5：整条音频链的门面
#include "fx/EffectService.h"
#include "scene/GameplayRuntime.h"         // P1：特效池（按值持有，需要完整类型）
#include "physics/ContactEvents.h"
#include "physics/ScenePhysics.h"   // SceneBinding（按值持有，需要完整类型）
// C8 导航：`nav::NavSystem m_nav;` 按值成员 → 需要完整类型。
#include "nav/NavSystem.h"
#include "scene/SaveGameState.h"    // 存档基线（G17-3，按值持有）

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace scene {
class Scene;
}

namespace vks {
class ScriptEngine;
class ScriptRuntime;
}

namespace runtime {

class RuntimeApp : public core::Application {
public:
    explicit RuntimeApp(const RuntimeConfig& cfg);
    ~RuntimeApp() override;

    const GameStateStack& stateStack() const { return m_stateStack; }
    const LoopStats& loopStats() const { return m_loop.stats(); }
    const RuntimeConfig& config() const { return m_cfg; }

protected:
    void onInit() override;
    void onUpdate(float dt) override;
    void onShutdown() override;

    // 运行时的渲染在 GameLoop 的 render 相位里发生（见文件头）。
    bool ownsFrameRendering() const override { return true; }

private:
    void loadStartScene();

    // ---- 存档（G17-3）----
    // 关卡**加载/切换完之后**调一次：把当时活着的每个实体的内容哈希记成基线。
    // 不抓的话第一次存档就是全量（不致命），第二次起也永远是全量 ——
    // 因为"基线为空 = 全量"，而增量正是靠它算出来的。
    void captureSaveBaseline(const char* reason);
    // 存到 `--slot` 指定的槽位（F5 与 `--save` 都走这里）。
    bool quickSave(const char* why);
    // 从 `--slot` 读回来（F9 与 `--load` 都走这里）。
    bool quickLoad(const char* why);

    // `openUI(path)` 的落点（C6 第七件）：切"显示哪一份 `.ui.json`"。
    // **空串 = 关掉**；失败时保住当前那块并返回 false。
    bool openUiDocument(const std::string& path);

    // 脚本点播的换场景（C6 第二批）。
    //   ① `requestSceneSwitch` 只**验路径 + 记下请求**，当场返回；
    //   ② `applyPendingSceneSwitch` 在每帧**最开头**执行真正的切换。
    //
    // ⚠ 为什么必须拆成两半：`.vks` 的 `loadScene()` 是在
    //   `ScriptRuntime::tick` 里被调用的，此刻正遍历着旧世界的
    //   `ScriptComponent` 池。这时候销毁世界 = 把正在用的迭代器指向
    //   已释放的内存。拆开之后"销毁世界"永远发生在没有任何人遍历它
    //   的那一刻（下一帧 start-of-frame），语义也从"立即"变成
    //   "下一帧生效" —— 这条**必须**写进 ScriptEngine.h 的注释里，
    //   否则脚本作者会以为下一句就能查新场景的实体（不能）。
    bool requestSceneSwitch(const std::string& relPath);
    void applyPendingSceneSwitch();
    // 按模型路径装配一个实体（`spawn` 的实现）。失败返回无效句柄。
    ecs::Entity spawnModelEntity(const std::string& relPath, glm::vec3 pos);
    void buildPhysics();
    // C8 导航：优先读 `assets/nav/<场景名>.vnav.json`，读不到就现场烘一份
    // （以玩家为中心）。两条路都打一行 `GAME-NAV` 说明走的是哪条。
    void setupNav();
    // 每帧推进代理（排在物理**之前**：它写 Transform / 期望速度）。
    void tickNav(float dt);
    void preloadScripts();
    void simulate(float dt);
    int runScripts(float dt);
    void pumpContactEvents();
    void dumpPhysicsBodies(const char* tag);
    // UI tick（U6）：**每帧都跑，在状态栈之外**。采样导航键 → 推进焦点栈
    // → 产出 `m_svc.uiAction`（暂停菜单选了哪一项）。
    void tickUi(float dt);
    // 玩家四段基础动画（P3）：按"水平速度 + 是否贴地"切
    // idle / walk / run / jump 四段 clip。没有蒙皮模型时什么都不做。
    //
    // 为什么是四段硬状态而不是混合树：这一层要的是"走起来像个角色"，
    // 而 A2 的控制器资产（`assets/animators/*.anim.json`）是给"要精细
    // 过渡"的项目留的。四段足够验证绑骨 + 动画生成这条链真的通了；
    // 想做混合，把这里换成控制器资产即可（clipName 的写入点还是同一个）。
    void tickPlayerAnimation(float dt);
    // 镜头参数 → 渲染效果（2026-10-30 运镜体系）。
    //
    // 相机组件上的景深 / 快门角是**作者填的镜头参数**，而真正执行它们的是
    // `render::CameraEffectSettings`（景深 pass / 运动模糊 pass）。engine 里
    // `scene` 不认识 `render`（单向依赖），所以这一步只能由宿主做 ——
    // 放在这里而不是 Renderer 里，因为只有宿主知道"当前活跃相机是哪台"。
    //
    // ⚠ 语义是**只在配了的时候写**：`dofEnabled == false && shutterAngle == 0`
    //   的相机一个字节都不改 → `MYVK_DOF` / 后期体积的覆盖照旧生效。
    //   相机切换时把**上一次写进去的**那两项复位（否则上一台的景深会粘住）。
    void applyCameraLensEffects();

    // 多机位切镜（2026-10-30；运镜体系 §七）。
    //
    // 把时间轴的 marker 结论翻译成相机系统的**导演覆盖**：
    //   `shotCameraGuid() != 0` → 压过 priority 用那台机位；混合时长交给
    //   `blendCut`（0 = 硬切）
    //   `shotCameraGuid() == 0` → 释放覆盖，回落 priority / 关键帧轨
    //
    // ⚠⚠ 调用位置是硬约束：**`seq.tick()` 之后、`CameraSystem::update()` 之前**。
    //   排在 tick 之前 → 用的是上一帧的 marker（切镜晚一帧，边界处会看到
    //   上一机位的画面）；排在 update 之后 → 这一帧白算。
    //
    // ⚠⚠ 覆盖生效时必须 `clearDesiredPose()`：玩家控制器/时间轴本帧刚喂过
    //   `setDesiredPose`，那份 desired 会让 `update()` 走"用 desired"分支，
    //   于是 marker 选中的机位**根本不动**（画面还是玩家视角），而且日志里
    //   `CAM-CUT` 明明打出来了 —— 这是这条线最容易踩的一个坑。
    void syncDirectorCamera();

    // 音频每帧（G14-5）：listener = 相机；把场景里的 AudioSourceComponent
    // 收集成 SourceUpdate 喂给 AudioSystem。⚠ **只读世界**（§3.6）：
    // 这里一个 `world().get<T>()` 的非 const 引用都不许拿去写。
    void syncAudio(float dt, bool focused);

    // 把宿主能力（HUD / 音频 / print / 错误）接到 ScriptRuntime 上。
    // 只在初始化时调用一次 —— S0 之前这些 std::function 是每帧每实体重建的。
    void installScriptServices();

    RuntimeConfig m_cfg;
    GameLoop m_loop;
    GameStateStack m_stateStack;
    GameServices m_svc;

    // ---- 脚本（S0）----
    // 唯一的 tick 内核：与编辑器共用 `vks::ScriptRuntime`。S0 之前这里和
    // EditorApp 各有一份复制粘贴的循环，能力已经分叉（编辑器那份没接
    // HUD/音频）—— P2 的根治就是把循环搬进 engine。
    std::unique_ptr<vks::ScriptRuntime> m_scriptRuntime;
    vks::ScriptEngine* m_scripts = nullptr;  // = &m_scriptRuntime->engine()
    physics::SceneBinding m_binding;
    // C8 导航：网格 + 代理推进。**没有网格**时 `tick` 是空转（不会崩、
    // 也不会把代理拽到原点）—— 所以"这关没烘导航"只表现为"NPC 站着不动"。
    nav::NavSystem m_nav;

    // ---- C6 第二批：世界控制 ----
    // 剧情旗标。**不进 ECS**：它是"一局游戏"的状态，不是"一个实体"的状态
    // —— 换场景要留着，读档要恢复，跟实体生命周期完全无关。
    GameFlags m_flags;
    // 已受理、待执行的换场景请求（空 = 没有）。见 requestSceneSwitch 的注释。
    std::string m_pendingScenePath;
    // 本局换过几次场景（GAME-SCENE-SWITCH 的计数，也给将来"回到标题"用）
    int m_sceneSwitches = 0;

    // ---- 存档（G17-3）----
    // 基线。一个会话一份，跟着"关卡加载"走 —— 所以放这里而不是 `Scene` 里
    // （塞进场景的话 `scene.save` 会把它写进文件，而它是运行时状态）。
    scene::SaveGameState m_saveState;

    // 骨骼动画（A0-3）：推进 SkinnedMeshComponent::time 并重算关节矩阵。
    // 在 simulate() 里排在物理**之前**（A4 的根运动是物理的输入），
    // 与编辑器 Play 的顺序一致。
    animation::AnimatorSystem m_animator;

    // ---- P1 特效（2026-11-02）----
    //
    // 出包之后**打怪必须有刀光**，所以运行时要自己接一条：动画事件 →
    // `EffectService` → 粒子 / 音效 / 震屏 / 顿帧 / 闪白。
    // ⚠ 与编辑器 Play 共用同一套 engine 侧的三个模块，但**两个宿主各持一份
    //   实例**（`m_fx` 一个进程一个）：编辑器里 Play 用的池不会跟游戏抢槽位。
    // ⚠ 与编辑器 `dispatchAnimEventsThisFrame` 是两份**独立**的消费者：
    //   同一个进程里只有其中一方在跑（编辑器 = Play 态，这里 = 出包运行时），
    //   所以不会"双重曝光"。同一进程里两边同时跑才是灾难。
    fx::EffectService m_fx;
    void setupFxSinks();
    // 每帧：同步场景 / 推进回收 / 递减闪白。传**真实 dt**。
    void tickFx(float realDt);
    // 把本帧动画事件分派成特效/音效/震屏/顿帧。
    void dispatchAnimEventsThisFrame();

    // ---- P3-4：卡牌表现（2026-11-04）----
    //
    // `GameplayRuntime::castCard` 只**记账**（`CardFxEvent` 队列），这里每帧
    // 取走、按 `gameplay::cardFxPreset()` 播粒子 + 弹飘字。
    // ⚠ 排在 `runScripts` 之后（脚本这一帧才 cast），否则飘字慢半拍。
    void dispatchCardFx();

    // ---- P2：玩法状态（2026-11-03）----
    //
    // 与编辑器那边是**独立的一份**：编辑器的 Play 是一局临时游戏，运行时的
    // 这一份才是玩家真的在跑的那局。共用一份的话"编辑器里试了一下"会改掉
    // 运行时的数据（而这条路径在自动化里根本不会同时开）。
    //
    // ⚠ 它**不负责推进回合** —— 什么时候该 `advance` / `tick` 是玩法决定
    //   的（脚本 / 未来的回合系统），运行时只提供能力。
    scene::GameplayRuntime m_gameplay;
    scene::Scene* m_gpScene = nullptr;
    scene::GameplayRuntime& gp();

    // P3-1：配表里的卡牌定义（**内容**，不进场景文件）。
    // `gp()` 在换场景时把它们灌进新场景的所有牌堆 —— 读档之后牌堆里只有
    // `defId`，没有定义表；不灌的话每张牌都 `known=0`（打出去什么都不发生）。
    std::vector<gameplay::CardDef> m_cardDefs;
    bool m_cardDefsLoaded = false;

    // 顿帧（真实秒）。⚠ 三个累计量与编辑器那一份同义：
    //   `total` 抓累加、`elapsed` 抓 scaled dt、`frames` 抓 timeScale=0 死锁。
    float m_hitstopLeft = 0.0f;
    float m_hitstopTotal = 0.0f;
    float m_hitstopElapsed = 0.0f;
    int m_hitstopFrames = 0;
    int m_hitstopReqs = 0;
    // 闪白：走 `ui2::UiCanvas` 的一层满屏矩形（在 HUD/字幕/菜单之**上**，
    // 因为命中反馈该盖住 HUD；但仍在 UI pass 内，不新开渲染 pass）。
    float m_fxFlashStrength = 0.0f;
    float m_fxFlashLeft = 0.0f;
    float m_fxFlashTotal = 0.0f;
    float m_fxFlashElapsed = 0.0f;
    glm::vec3 m_fxFlashColor{1.0f, 1.0f, 1.0f};

    // 镜头效果接线（applyCameraLensEffects）的状态：
    //   `m_lensCam` 记住上一次写效果的那台相机 —— 换相机时先把**自己写过**
    //   的两项（dof / motionBlur）复位，不然上一台的景深会粘在画面上，
    //   而作者只会看到"换了相机景深还在"，完全想不到是这里。
    //   `m_lensSig` 是日志去重签名（每帧打会淹掉真正有用的行）。
    std::uint64_t m_lensCam = 0;
    std::string m_lensSig;

    // 多机位切镜的日志去重签名（只在真的切了那一刻打一条 CAM-CUT-RT）。
    std::string m_cutSig;

    // ---- M2：输入 / 角色 / 相机 / 接触事件 ----
    core::InputMap m_input;
    std::unique_ptr<PlayerController> m_player;
    physics::ContactTracker m_contacts;
    std::vector<physics::ContactEvent> m_events;
    bool m_contactTrace = false;
    int m_playerDumpEvery = 0;

    // ---- M3：UI（U6 焦点 / U8 暂停菜单）----
    //
    // ⚠ 暂停的**权威在状态栈**（topName()=="Paused"），`m_lastUiState`
    //   只是"上一帧是什么"的缓存，用来只在变化时改光标/输入上下文 ——
    //   每帧都 pushContext 会让上下文栈无限增长。
    PauseMenu m_pause;
    // HUD：数据（HudModel）每帧由 simulate 推进，绘制由 Hud 在输出 pass 里做。
    // ⚠ 两者分开是为了"数据能无窗口断言"（见 HudModel.h 的头注释）。
    Hud m_hud;
    HudModel m_hudModel;
    ui2::UiFocusStack m_focus;
    int m_lastUiState = -1;   // -1 未知 / 0 游戏中 / 1 暂停；首次必应用

    // ---- UI 文档（#312）----
    //
    // `m_cfg.uiDoc` 为空 = 这条路**完全不存在**（一个顶点都不画，画面与
    // #311 之后逐位相同）。指定了就把 `.ui.json` 读进来、每帧按绑定值绘制。
    //
    // 为什么文档放 runtime 而不是 engine：读盘/热更/降级都是宿主的事
    // （engine 不认识文件系统策略，也不该认识"这个游戏的 HUD 是哪一份"）。
    // 文档本身的数据结构与绘制都在 `engine/ui2/UiDocument`（纯 CPU、可自检）。
    ui2::UiDocument m_uiDoc;
    ui2::UiBindings m_uiBindings;
    int m_uiDocFrames = 0;    // UIDOC-BOUNDS / UIDOC-STATS 的节流计数
    // 每帧把 HudModel + GameFlags 抄成绑定表（见 .cpp 的注释）
    void buildUiBindings();

    // ---- 文档层的交互（2026-11-02）----
    //
    // 在这之前 `.ui.json` 里的按钮是**纯装饰**：`drawDocument` 会画，
    // 但没有任何命中测试（`grep -rn action engine/ui2/` → 0）。
    // 现在这条链是：UiDocInput 命中 → 产出 action 字符串 →
    // **在这里**执行（引擎只解析，`ui2::parseUiAction` 不碰旗标表）。
    ui2::UiDocInput m_uiDocInput;
    // 上一帧左键是否按着。用来合成"松开沿" —— `core::Input` 只给
    // `mouseDown` / `mousePressed` 两个查询，没有 released，而"按下与松开
    // 必须落在同一个控件上"这条语义非要它不可。
    bool m_uiDocMouseDownPrev = false;
    int m_uiDocInputFrames = 0;
    // `MYVK_RUNTIME_UICLICK="x,y;x2,y2"`（**参考像素**）。解析结果留在
    // 这里，按帧间隔逐发注入 —— 为什么不在 init 里点：那时还没有输出
    // 尺寸，命中测试必然 miss，而 miss 会被误读成"按钮没画上"。
    std::vector<std::pair<float, float>> m_uiDocClicks;
    bool m_uiDocClicksParsed = false;
    int m_uiDocClickNext = 0;
    // 执行一条动作。⚠ 唯一的执行点 —— 命令 / 注入 / 真点击都走这里，
    // 否则"点得动但命令改不了"这类分叉迟早出现。
    void dispatchUiAction(const std::string& text);
    // 每帧的文档交互 tick（在绘制回调里调用，那里才知道输出尺寸）。
    void tickUiDocInput(uint32_t outW, uint32_t outH);
    // 指针（窗口客户区坐标）→ **输出像素**。
    //
    // ⚠ 两处要用同一份换算：`tickUiDocInput`（真鼠标点击）与 `tickUi`
    //   的"点击视口 → 进入操作态"（①b 要先问文档层有没有命中）。
    //   各写一份的后果不是"差几像素"，而是**一条路径点得中、另一条点不中**
    //   —— 而这两条路径的判据都由同一批日志锚点覆盖，红了也分不清是谁。
    bool cursorToOutput(float& px, float& py, uint32_t outW, uint32_t outH);
    // 输出像素 (px,py) 落在某个**可交互且 enabled** 的文档控件上吗。
    // 顺带把 id 写进 `idOut`（命中测试与绘制用同一份 bindings）。
    bool uiDocHitAt(float px, float py, std::string* idOut = nullptr);

    // ---- 字幕框（C2，2026-10-26）----
    // 与 HUD 同一条分工：`dialogue::DialogueSystem` 在 engine（推进状态），
    // 这里每帧把它的状态抄成 `SubtitleModel`（纯数据），由 `SubtitleBox`
    // 画。于是"字幕长什么样"可以脱离对话逻辑单独改。
    SubtitleBox m_subtitle;
    // 选项焦点。**存在这里而不是 `DialogueSystem` 里**：焦点是 UI 的输入
    // 状态（和 `m_focus` 一类），跑不到 ECS 里去，也不该进存档。
    int m_dlgFocus = 0;
    // 焦点归属的那一行台词 id。换行时把焦点复位到 0 —— 不复位的话，
    // 上一题选了第 3 项，下一题一出来光标就停在第 3 项上（选项少的时候
    // 还会越界，得靠下面的 clamp 兜着）。
    std::string m_dlgFocusId;
    // 自动推进（`MYVK_DLG_AUTO=<秒>`）：验收脚本用，走**同一条**
    // `pressAdvance()`/`choose()`，不是另一套"测试模式"分支。
    // 0 = 关（正式运行的默认值）。
    float m_dlgAutoSec = 0.0f;
    float m_dlgAutoTimer = 0.0f;

    // 语音钩子的**假**实现（`MYVK_DLG_VOICE_FAKE=<秒>`，0 = 关）。
    //
    // 为什么需要它：`DLG-PRESS waiting-voice` 这条日志只有在"这句语音还在响"
    // 的时候才会出现，而 CI / 无声卡机器上 `playVoice()` 必然失败 → 对话侧
    // 不设门 → 那条路**永远走不到**。没有这个口子，"钩子接错了"这类 bug
    // （比如装在 `autoStart` 之后、或者压根没接）在自动化里完全看不出来。
    // 假钩子只替换"播没播、播完没"，`pressAdvance` / 打字机 / 翻页全部还是
    // 玩家真正走的那条路。
    float m_dlgVoiceFakeSec = 0.0f;
    std::chrono::steady_clock::time_point m_dlgVoiceFakeStart{};

    // ---- 鼠标捕获 / 操作态（P5，2026-10-22）----
    //
    // 用户要求："游戏启动运行后，必须要鼠标左键在 3d 视口中点击后才能开始
    // 正常和角色开始交互；操作角色时鼠标要隐藏；退出操作可以点 Esc，此时
    // 也显示鼠标。"
    //
    // 所以是**两态**而不是一个"可见/不可见"：
    //   false = 未操作：光标可见、可自由移动（不锁在窗口里），玩家不响应输入
    //   true  = 操作中：GLFW_CURSOR_DISABLED（隐藏 + 锁定 + 无限转视角）
    //
    // ⚠ 只要一个 bool 就够，不要另存"光标可见"：两者是同一件事的两种写法，
    //   存两份必然有一天不一致（表现是"鼠标看不见但角色不动"）。
    bool m_mouseCaptured = false;
    // "未捕获"的提示只打一次（每帧打会把日志淹掉）
    bool m_mouseIdleLogged = false;
    // 请求压 Paused（tickUi 只置意图，迁移在 onUpdate 末尾统一做）
    bool m_pauseRequested = false;
    bool pausedNow() const;

    // ---- 音频（G14-5）----
    // ⚠ 门面是**栈上成员**，不做单例（§4.2：单例会让"两份音频系统"没法共存，
    //   而且退出顺序变得不可控）。设备打不开时所有 API 变空操作。
    audio::AudioSystem m_audio;
    // 每帧复用的声源缓冲（避免每次 applySources 都堆分配）
    std::vector<audio::SourceUpdate> m_audioSrcs;
    // AUDIO-LISTENER 只打一次（每帧打会把日志淹掉，R15）。
    // ⚠ 用成员而不是函数内 `static`：static 会被所有实例共享，
    //   将来一份进程里开两个 App 就只有一个能打。
    bool m_audioListenerLogged = false;

    // 不透明状态在栈顶时（MainMenu）世界不画，但仍要提交一帧
    // （否则 ImGui 的帧不闭环、交换链也不推进）。用一份空场景当背景。
    std::unique_ptr<scene::Scene> m_menuScene;

    // ---- 玩家四段动画（P3 / P4）----
    // 状态机的**内容**已经搬到 `animation/Locomotion`（编辑器 Play 与运行时
    // 共用一份），这里只留这份游标。历史注释保留：`GAME-ANIM` 只在状态
    // 变化时打一行 —— 每帧一行会把别的锚点淹掉。
    animation::LocomotionRuntime m_playerAnim;

    // 从进入 Playing 起算的仿真帧数（确定性对照用；与编辑器的
    // m_physicsFrames 同语义、同起点）
    std::uint64_t m_simFrames = 0;
    // PHYS-BODY 转储间隔（0 = 关）
    int m_physicsDumpEvery = 0;
    bool m_renderedWorldThisFrame = false;
};

} // namespace runtime
