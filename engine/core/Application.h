#pragma once
// ============================================================
// core/Application —— 引擎主循环
// 拥有：Window / Renderer / Scene / Time
// 游戏层（game/）继承本类，在 onInit/onUpdate/onImGui 里写玩法
// ============================================================

#include "core/Window.h"
#include "core/Time.h"
#include "render/Renderer.h"
#include "scene/Scene.h"
#include "assets/AssetManager.h"

#include <memory>
#include <string>

namespace core {

class Application {
public:
    explicit Application(const std::string& title = "Venn",
                         int width = 1280, int height = 720);
    virtual ~Application();

    // maxFrames > 0 时跑满 N 帧自动退出（冒烟测试用）
    int run(uint64_t maxFrames = 0);

protected:
    // ---- 游戏层钩子 ----
    virtual void onInit() {}
    virtual void onUpdate(float dt) {}
    virtual void onImGui() {}

    // 主循环退出前调用（窗口还活着）。用来做一次性收尾 ——
    // 编辑器在这里保存窗口几何，供下次启动恢复。
    virtual void onShutdown() {}

    // 主循环实际驱动/渲染的那一份场景。
    // 默认就是 Application 自带的 m_scene（游戏路径不变）；
    // 编辑器（editor/EditorApp）重写它，返回"编辑态或运行态"场景 ——
    // 这就是编辑器能把 3D 渲染接到自己场景上的接缝。
    virtual scene::Scene& activeScene() { return m_scene; }

    // ----------------------------------------------------------
    // 谁负责"每帧提交一帧"
    // ----------------------------------------------------------
    // 默认 false → Application::run 在 onUpdate 之后调 drawFrame。
    // 运行时（runtime/RuntimeApp）重写成 true：它的 GameLoop 把"渲染"
    // 当作一个**可计数的相位**（要证明"Paused 时画面仍在出帧"、
    // "每帧恰好画一次"），所以画由它自己发起。
    //
    // ⚠ 重写成 true 之后**必须**在 onUpdate 里真的画一次，否则这一帧
    //   不会呈现 —— 交换链不推进、ImGui 的帧也不闭环。
    virtual bool ownsFrameRendering() const { return false; }

    // ---- 引擎服务 ----
    Window& window() { return *m_window; }
    render::Renderer& renderer() { return *m_renderer; }
    scene::Scene& scene() { return m_scene; }
    Time& time() { return m_time; }
    assets::AssetManager& assets();

private:
    std::unique_ptr<Window> m_window;
    std::unique_ptr<render::Renderer> m_renderer;
    std::unique_ptr<assets::AssetManager> m_assets;
    scene::Scene m_scene;
    Time m_time;
    bool m_initialized = false;
};

} // namespace core
