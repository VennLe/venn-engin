#include "GameStateStack.h"

#include "core/Logger.h"

#include <utility>

namespace runtime {

// ---------------------------------------------------------------- 观测

void GameStateStack::record(const char* op, const char* affected) {
    const char* topName = this->topName();
    m_topSequence.emplace_back(topName);
    if (m_trace) {
        VK_LOG_INFO("GAME-STATE %s name=%s depth=%zu top=%s", op,
                    affected ? affected : "-", m_stack.size(), topName);
    }
}

std::string GameStateStack::topSequenceString() const {
    std::string out;
    for (std::size_t i = 0; i < m_topSequence.size(); ++i) {
        if (i) out += '>';
        out += m_topSequence[i];
    }
    return out;
}

// ---------------------------------------------------------------- 迁移

void GameStateStack::push(std::unique_ptr<GameState> state, GameServices& svc) {
    if (!state) return;
    const char* nm = state->name();
    state->onEnter(svc);
    m_stack.push_back(std::move(state));
    record("push", nm);
}

void GameStateStack::pop(GameServices& svc) {
    if (m_stack.empty()) return;
    GameState* s = m_stack.back().get();
    const char* nm = s->name();
    s->onExit(svc);
    m_stack.pop_back();
    record("pop", nm);
}

void GameStateStack::replace(std::unique_ptr<GameState> state, GameServices& svc) {
    if (!state) return;
    if (!m_stack.empty()) {
        m_stack.back()->onExit(svc);
        m_stack.pop_back();
    }
    const char* nm = state->name();
    state->onEnter(svc);
    m_stack.push_back(std::move(state));
    record("replace", nm);
}

void GameStateStack::clear(GameServices& svc) {
    while (!m_stack.empty()) {
        m_stack.back()->onExit(svc);
        m_stack.pop_back();
    }
    record("clear", "-");
}

// ---------------------------------------------------------------- 范围

std::size_t GameStateStack::updateFloor() const {
    for (std::size_t i = m_stack.size(); i-- > 0;) {
        if (m_stack[i]->blocksUpdate) return i;
    }
    return 0;
}

std::size_t GameStateStack::renderFloor() const {
    for (std::size_t i = m_stack.size(); i-- > 0;) {
        if (m_stack[i]->blocksRender) return i;
    }
    return 0;
}

// ---------------------------------------------------------------- 驱动

void GameStateStack::update(GameServices& svc, float dt) {
    if (m_stack.empty()) return;
    const std::size_t floor = updateFloor();
    for (std::size_t i = floor; i < m_stack.size(); ++i)
        m_stack[i]->onUpdate(svc, dt);
}

void GameStateStack::render(GameServices& svc) {
    if (m_stack.empty()) return;
    const std::size_t floor = renderFloor();
    for (std::size_t i = floor; i < m_stack.size(); ++i)
        m_stack[i]->onRender(svc);
}

} // namespace runtime
