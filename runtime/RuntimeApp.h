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

#include "GameLoop.h"
#include "GameState.h"
#include "GameStateStack.h"
#include "PlayerController.h"
#include "RuntimeConfig.h"

#include "core/InputMap.h"
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

    RuntimeConfig m_cfg;
    GameLoop m_loop;
    GameStateStack m_stateStack;
    GameServices m_svc;

    std::unique_ptr<vks::ScriptEngine> m_scripts;
    physics::SceneBinding m_binding;

    // ---- M2：输入 / 角色 / 相机 / 接触事件 ----
    core::InputMap m_input;
    std::unique_ptr<PlayerController> m_player;
    physics::ContactTracker m_contacts;
    std::vector<physics::ContactEvent> m_events;
    bool m_contactTrace = false;
    int m_playerDumpEvery = 0;

    // 不透明状态在栈顶时（MainMenu）世界不画，但仍要提交一帧
    // （否则 ImGui 的帧不闭环、交换链也不推进）。用一份空场景当背景。
    std::unique_ptr<scene::Scene> m_menuScene;

    // 脚本运行期错误去重（同一实体同一条错误只报一次）
    std::unordered_map<unsigned, std::string> m_scriptErrors;
    // 从进入 Playing 起算的仿真帧数（确定性对照用；与编辑器的
    // m_physicsFrames 同语义、同起点）
    std::uint64_t m_simFrames = 0;
    // PHYS-BODY 转储间隔（0 = 关）
    int m_physicsDumpEvery = 0;
    bool m_renderedWorldThisFrame = false;
};

} // namespace runtime
