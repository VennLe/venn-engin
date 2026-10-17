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
