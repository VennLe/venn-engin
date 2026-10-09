#include "Command.h"

#include "assets/AssetManager.h"
#include "core/Logger.h"
#include "ecs/Components.h"
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"

namespace editor {

// ---------------------------------------------------------------- 命令栈

void CommandStack::push(std::unique_ptr<Command> cmd) {
    if (!cmd) return;
    cmd->redo();  // 操作本身在这一刻生效
    pushAlreadyApplied(std::move(cmd));
}

void CommandStack::pushAlreadyApplied(std::unique_ptr<Command> cmd) {
    if (!cmd) return;

    // 丢弃"重做分支"：在历史中间做了新操作，后面的分支就不可能再重放了
    if (m_cursor < m_stack.size()) {
        m_stack.erase(m_stack.begin() + static_cast<std::ptrdiff_t>(m_cursor),
                      m_stack.end());
    }

    m_stack.push_back(std::move(cmd));
    m_cursor = m_stack.size();

    // 超限时从最老的开始丢
    if (m_stack.size() > m_limit) {
        const size_t drop = m_stack.size() - m_limit;
        m_stack.erase(m_stack.begin(),
                      m_stack.begin() + static_cast<std::ptrdiff_t>(drop));
        m_cursor -= drop;
    }
}

bool CommandStack::undo() {
    if (!canUndo()) return false;
    --m_cursor;
    m_stack[m_cursor]->undo();
    return true;
}

bool CommandStack::redo() {
    if (!canRedo()) return false;
    m_stack[m_cursor]->redo();
    ++m_cursor;
    return true;
}

const char* CommandStack::undoName() const {
    if (!canUndo()) return "";
    return m_stack[m_cursor - 1]->name();
}

const char* CommandStack::redoName() const {
    if (!canRedo()) return "";
    return m_stack[m_cursor]->name();
}

void CommandStack::clear() {
    m_stack.clear();
    m_cursor = 0;
}

std::vector<std::string> CommandStack::historyNames() const {
    std::vector<std::string> out;
    out.reserve(m_stack.size());
    for (const auto& c : m_stack) out.emplace_back(c->name());
    return out;
}

// ---------------------------------------------------------------- 变换

TransformSnapshot TransformSnapshot::capture(scene::Scene& scene,
                                             ecs::Entity e) {
    TransformSnapshot s;
    if (const auto* t = scene.world().get<ecs::TransformComponent>(e)) {
        s.position = t->position;
        s.rotation = t->rotation;
        s.scale = t->scale;
    }
    return s;
}

void TransformSnapshot::apply(scene::Scene& scene, ecs::Entity e) const {
    if (auto* t = scene.world().get<ecs::TransformComponent>(e)) {
        t->position = position;
        t->rotation = rotation;
        t->scale = scale;
    }
}

void TransformEditCommand::undo() {
    if (m_scene) m_before.apply(*m_scene, m_entity);
}

void TransformEditCommand::redo() {
    if (m_scene) m_after.apply(*m_scene, m_entity);
}

// ---------------------------------------------------------------- 场景快照

void SceneSnapshotCommand::restore(const std::string& json) {
    if (!m_scene || !m_assets) return;
    const scene::SceneIoResult r =
        scene::sceneFromJson(*m_scene, *m_assets, json);
    if (!r.ok) {
        VK_LOG_ERROR("Command '%s': scene restore failed: %s", m_name.c_str(),
                     r.error.c_str());
    }
}

void SceneSnapshotCommand::undo() { restore(m_before); }
void SceneSnapshotCommand::redo() { restore(m_after); }

} // namespace editor
