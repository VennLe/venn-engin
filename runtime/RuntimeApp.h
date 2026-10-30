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
#include "ui2/UiFocus.h"
#include "audio/AudioSystem.h"        // G14-5：整条音频链的门面
#include "physics/ContactEvents.h"
#include "physics/ScenePhysics.h"   // SceneBinding（按值持有，需要完整类型）

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

    // ---- C6 第二批：世界控制 ----
    // 剧情旗标。**不进 ECS**：它是"一局游戏"的状态，不是"一个实体"的状态
    // —— 换场景要留着，读档要恢复，跟实体生命周期完全无关。
    GameFlags m_flags;
    // 已受理、待执行的换场景请求（空 = 没有）。见 requestSceneSwitch 的注释。
    std::string m_pendingScenePath;
    // 本局换过几次场景（GAME-SCENE-SWITCH 的计数，也给将来"回到标题"用）
    int m_sceneSwitches = 0;

    // 骨骼动画（A0-3）：推进 SkinnedMeshComponent::time 并重算关节矩阵。
    // 在 simulate() 里排在物理**之前**（A4 的根运动是物理的输入），
    // 与编辑器 Play 的顺序一致。
    animation::AnimatorSystem m_animator;

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
