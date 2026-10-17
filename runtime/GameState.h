#pragma once
// ============================================================
// runtime/GameState —— 游戏阶段状态（Loading / MainMenu / Playing / Paused…）
//
// ------------------------------------------------------------
// 为什么是**栈**而不是一个 enum
// ------------------------------------------------------------
// 因为现实里的阶段是叠加的：
//   · 在 Playing 之上压一个 Paused —— 下层**停止更新**（物理停住），
//     但**继续渲染**（玩家还要看得见被暂停的那个画面）；
//   · 在 Playing 之上压一个 GameOver —— 同样停更新、但画面还在；
//   · MainMenu 是**不透明**的：它压着的时候下面什么都不用画。
// 用一个 enum 表达"停更新但不停渲染"就只能到处写 if，而且每加一个
// 叠加层就再多一层 if。栈把这件事变成两个布尔量。
//
// ------------------------------------------------------------
// 两个布尔量的语义（只有两条，背下来就够）
// ------------------------------------------------------------
//   blocksUpdate = true → 压在我**下面**的状态不再 update
//   blocksRender = true → 压在我**下面**的状态不再 render（我是全屏不透明）
//
// 于是 Paused 就是 `{ blocksUpdate = true, blocksRender = false }`。
//
// ------------------------------------------------------------
// 为什么状态拿的是 GameServices 而不是 RuntimeApp&
// ------------------------------------------------------------
// 状态栈要能在**没有窗口、没有 Vulkan**的情况下被自检驱动
// （`Game.exe --selftest=states`，见 runtime/main.cpp）——
// 而 RuntimeApp 一构造就要创建 GLFW 窗口与 VkDevice。
// 把状态能碰到的东西收窄成几个 std::function，自检就能只搭一个空壳。
// ============================================================

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace runtime {

// 状态能碰到的**服务面**。宿主负责往里面塞实现。
struct GameServices {
    // Playing 态的仿真（M1 = 脚本；M2 起加玩家 / 物理 / 碰撞）
    std::function<void(float)> simulate;
    // 相机 / 动画插值 / UI —— 每帧都跑，不归状态管
    std::function<void(float)> updateVariable;
    // 画世界（只有 Playing 那一层调，被 blocksRender 截断）
    std::function<void()> renderWorld;

    float timeScale = 1.0f;
    bool quitRequested = false;
    std::uint64_t frame = 0;
};

struct GameState {
    virtual ~GameState() = default;
    virtual const char* name() const = 0;
    virtual void onEnter(GameServices&) {}
    virtual void onExit(GameServices&) {}
    virtual void onUpdate(GameServices&, float /*dt*/) {}
    virtual void onRender(GameServices&) {}

    bool blocksUpdate = false;
    bool blocksRender = false;
};

enum class StateId : int {
    Boot = 0,
    Loading,
    MainMenu,
    Playing,
    Paused,
    GameOver,
    Count
};

const char* stateIdName(StateId id);

// ---------------------------------------------------------------- 内置状态
//
// M1 阶段它们只做两件事：**计数 + 转发**。
//   · 计数是给自检用的（`--selftest=states` 靠它证明"Paused 时下层不 update
//     但仍在 render"）；
//   · 转发是把 simulate / renderWorld 接到宿主。
// 真正的 UI（主菜单按钮、暂停菜单）是 G15 的事。
class BuiltinState : public GameState {
public:
    explicit BuiltinState(StateId id);

    const char* name() const override { return stateIdName(m_id); }
    StateId id() const { return m_id; }

    void onEnter(GameServices& svc) override;
    void onExit(GameServices& svc) override;
    void onUpdate(GameServices& svc, float dt) override;
    void onRender(GameServices& svc) override;

    // 自检计数（public：--selftest=states 直接读）
    unsigned updates = 0;
    unsigned renders = 0;

private:
    StateId m_id;
};

std::unique_ptr<GameState> makeBuiltinState(StateId id);

} // namespace runtime
