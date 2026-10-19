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

#include "GameLoop.h"
#include "GameState.h"
#include "GameStateStack.h"
#include "PlayerController.h"
#include "RuntimeConfig.h"

#include "core/InputMap.h"
#include "ui/Hud.h"
#include "ui/HudModel.h"
#include "ui/PauseMenu.h"
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
    void buildPhysics();
    void preloadScripts();
    void simulate(float dt);
    int runScripts(float dt);
    void pumpContactEvents();
    void dumpPhysicsBodies(const char* tag);
    // UI tick（U6）：**每帧都跑，在状态栈之外**。采样导航键 → 推进焦点栈
    // → 产出 `m_svc.uiAction`（暂停菜单选了哪一项）。
    void tickUi(float dt);
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

    // 骨骼动画（A0-3）：推进 SkinnedMeshComponent::time 并重算关节矩阵。
    // 在 simulate() 里排在物理**之前**（A4 的根运动是物理的输入），
    // 与编辑器 Play 的顺序一致。
    animation::AnimatorSystem m_animator;

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

    // 从进入 Playing 起算的仿真帧数（确定性对照用；与编辑器的
    // m_physicsFrames 同语义、同起点）
    std::uint64_t m_simFrames = 0;
    // PHYS-BODY 转储间隔（0 = 关）
    int m_physicsDumpEvery = 0;
    bool m_renderedWorldThisFrame = false;
};

} // namespace runtime
