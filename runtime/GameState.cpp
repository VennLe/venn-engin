#include "GameState.h"

#include "core/Logger.h"

namespace runtime {

const char* stateIdName(StateId id) {
    switch (id) {
        case StateId::Boot:     return "Boot";
        case StateId::Loading:  return "Loading";
        case StateId::MainMenu: return "MainMenu";
        case StateId::Playing:  return "Playing";
        case StateId::Paused:   return "Paused";
        case StateId::GameOver: return "GameOver";
        case StateId::Count:    break;
    }
    return "Unknown";
}

// ---------------------------------------------------------------- 内置状态

BuiltinState::BuiltinState(StateId id) : m_id(id) {
    // 阻断语义见 GameState.h 的两条规矩。
    switch (id) {
        case StateId::Boot:
        case StateId::Loading:
        case StateId::MainMenu:
            // 全屏、且下面本来也没有东西：更新与渲染都截断。
            blocksUpdate = true;
            blocksRender = true;
            break;
        case StateId::Playing:
            // 唯一"让世界跑起来"的状态。
            blocksUpdate = false;
            blocksRender = false;
            break;
        case StateId::Paused:
        case StateId::GameOver:
            // 停更新但**继续渲染** —— 玩家还要看得见被定住的画面。
            blocksUpdate = true;
            blocksRender = false;
            break;
        case StateId::Count:
            break;
    }
}

void BuiltinState::onEnter(GameServices&) {}

void BuiltinState::onExit(GameServices&) {}

void BuiltinState::onUpdate(GameServices& svc, float dt) {
    ++updates;
    // 只有 Playing 推进玩法。Paused/GameOver 在上面时，Playing 根本收不到
    // 这个回调（栈的 update 范围被 blocksUpdate 截断了）。
    if (m_id == StateId::Playing && svc.simulate) svc.simulate(dt);

    // ---- Paused（U6/U8）：玩家在暂停菜单上选了什么就做什么 ----
    //
    // ⚠ 这里**只置意图**，绝不改栈：在 onUpdate 里 pop 自己会让
    //   GameStateStack 正在遍历的 vector 失效（它的第 2 条约定）。
    // ⚠ `uiAction` 由宿主的 UI tick 填。UI tick 在状态栈**之外**每帧都跑，
    //   所以暂停菜单即使压住了 Playing 也照样收得到输入 —— 这正是
    //   "暂停时菜单点不动"那条老 bug 的根治办法。
    if (m_id == StateId::Paused && !svc.uiAction.empty()) {
        if (svc.uiAction == "resume") {
            VK_LOG_INFO("GAME-PAUSE action=resume → pop");
            svc.popRequested = true;
        } else if (svc.uiAction == "quit") {
            // "退出" = 退出进程。回主菜单是另一条路：M1 还没有菜单 UI，
            // 回 MainMenu 会停在不透明的空场景里，玩家点什么都点不到。
            VK_LOG_INFO("GAME-PAUSE action=quit → quitRequested");
            svc.quitRequested = true;
        } else if (svc.uiAction == "settings") {
            // G20 的运行时设置还没做。**明确打出来**，否则"点了没反应"
            // 会看起来像输入坏了（这类最费时间）。
            VK_LOG_INFO("UI2-ACTION id=settings handled=0 reason=not-implemented");
        }
    }
}

void BuiltinState::onRender(GameServices& svc) {
    ++renders;
    // 世界只在 Playing 这一层画一次。Paused 叠在上面时 Playing 仍然在
    // render 范围里（Paused 只截 update），所以画面继续出帧。
    if (m_id == StateId::Playing && svc.renderWorld) svc.renderWorld();
}

std::unique_ptr<GameState> makeBuiltinState(StateId id) {
    return std::make_unique<BuiltinState>(id);
}

} // namespace runtime
