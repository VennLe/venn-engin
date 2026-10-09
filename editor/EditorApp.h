#pragma once
// ============================================================
// editor/EditorApp —— 编辑器应用（继承 core::Application）
//
// 这是把整个编辑器接到引擎主循环上的地方，关键只有两个接缝：
//
//   1) activeScene() override
//      Application::run() 里"更新哪个场景、渲染哪个场景"全部改问它。
//      编辑器在这里返回 EditorContext::activeScene() —— 编辑态或运行态，
//      由 Play 状态决定。引擎其余部分一行都不用改。
//
//   2) renderer().setViewportSize(w, h)
//      由 ViewportPanel 每帧按面板大小设置 → 3D 场景不再直接写交换链，
//      而是写进一张离屏图，再由 ImGui::Image 贴进面板。
//
// 编辑器的职责边界：
//   · onUpdate 里**只有在 Play 态**才跑脚本 —— 而且跑在"运行态副本"上。
//     渲染则通过 activeScene() 直接落到同一个视口，不需要第二个窗口。
//   · 视口导航是**编辑器独有的**：编辑态按 UE 的习惯驾驶相机
//     （按住右键转头 + WASD 飞行、中键平移、滚轮推拉，见 ViewportPanel）。
//     Play 态视口不抢任何键鼠输入 —— 那时相机归游戏自己管。
//   · 全屏：Play 态按 F（或点按钮）会把**编辑器窗口本身**切成真全屏
//     （GLFW 显示器全屏），同时让视口独占整屏；退出时恢复原窗口几何。
//     另有 F11 = 只切窗口全屏（面板照旧），编辑态也能用。
//   · 窗口几何：第一次启动主显示器居中占 70%，之后按上次关闭时的存档恢复
//     （见 core/WindowGeometry）。
//   · 所有面板的绘制顺序、布局矩形、快捷键都在这里统一编排
//
// 快捷键有一条容易踩的交叉线：W / E / R 既是手柄模式切换，又是飞行键。
// 规则是"按住右键（= 正在驾驶相机）时飞行优先"，见 handleShortcuts。
//
// 编辑器是 venn 引擎唯一的应用层产物（曾经还有一个 game/SandboxGame
// 独立示例程序，2026-10-09 按用户要求删除）。engine/ 依然不认识 editor/，
// 依赖方向保持单向。
// ============================================================

#include "core/Application.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace editor {

class EditorContext;
class Toolbar;
class ViewportPanel;
class SceneHierarchy;
class InspectorPanel;
class ContentBrowser;
class ScriptEngine;
class SplitLayout;

class EditorApp : public core::Application {
public:
    EditorApp();
    ~EditorApp() override;

protected:
    void onInit() override;
    void onUpdate(float dt) override;
    void onImGui() override;
    // 窗口还活着时保存窗口几何，供下次启动恢复
    void onShutdown() override;

    // 引擎问"现在该更新/渲染哪一份场景" —— 编辑器的核心接缝
    scene::Scene& activeScene() override;

private:
    void computeLayout();
    void handleShortcuts();
    // 让 GLFW 顶层窗口的全屏状态跟上"游戏全屏"逻辑状态。
    // **只在状态发生变化的那一帧动窗口** —— 每帧无条件同步会把 F11
    // （纯窗口全屏）立刻覆盖掉。
    void syncWindowFullscreen();
    // F11：只切窗口全屏（面板照旧），编辑态也能用
    void toggleWindowFullscreen();
    // 把编辑态场景引用到的脚本全部加载 + 编译一遍（只为在面板上暴露错误；
    // 真正执行它们的地方在下面的 runRuntimeScripts）
    void preloadScripts();
    // 每帧在**运行态副本**上执行一遍脚本（只在 Play 态调用）
    void runRuntimeScripts(float dt);
    // 游戏全屏：视口独占整窗，其余面板一律不画。返回是否处于该状态。
    bool drawFullscreenGame();

    void drawStatsPanel();
    void drawScriptPanel();
    void drawEnginePanel();
    void drawStatusBar();
    void drawSettingsPanel();

    std::unique_ptr<EditorContext> m_ctx;
    std::unique_ptr<Toolbar> m_toolbar;
    std::unique_ptr<ViewportPanel> m_viewport;
    std::unique_ptr<SceneHierarchy> m_hierarchy;
    std::unique_ptr<InspectorPanel> m_inspector;
    std::unique_ptr<ContentBrowser> m_content;
    std::unique_ptr<ScriptEngine> m_scripts;
    std::unique_ptr<SplitLayout> m_split;

    // 热重载扫描节流（每 0.5s 扫一次脚本文件时间戳）
    float m_hotReloadTimer = 0.0f;
    // 上一次同步给 ScriptEngine 的脚本路径集合（变了才重建缓存）
    std::vector<std::string> m_scriptPaths;

    // 运行态已经跑了多少帧（第一帧打一条脚本执行汇总日志，供冒烟测试确认链路）
    uint64_t m_runtimeFrames = 0;
    // 脚本运行期错误去重：同一个实体同一条错误只打一次日志
    std::unordered_map<unsigned, std::string> m_scriptErrors;
    // 脚本 print() 的滚动日志
    std::vector<std::string> m_printLog;

    // 已经同步给 GLFW 窗口的全屏状态（边沿检测用，见 syncWindowFullscreen）
    bool m_appliedWindowFullscreen = false;
};

} // namespace editor
