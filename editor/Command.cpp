#include "Command.h"

#include "PrimitiveBuilder.h"

#include "assets/AssetManager.h"
#include "core/Logger.h"
#include "ecs/Components.h"
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

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

// ---------------------------------------------------------------- 多选变换

void MultiTransformEditCommand::undo() {
    if (!m_scene) return;
    // 逆序回滚：严格说逐项独立写字段，顺序无关；但倒着走能保证"若将来越来越
    // 多的命令会带派生数据（比如刚体同步），父节点先于子节点恢复不会露馅"。
    for (auto it = m_items.rbegin(); it != m_items.rend(); ++it)
        it->before.apply(*m_scene, it->entity);
    VK_LOG_INFO("MULTI-UNDO '%s' n=%zu", m_name.c_str(), m_items.size());
}

void MultiTransformEditCommand::redo() {
    if (!m_scene) return;
    for (const Item& it : m_items) it.after.apply(*m_scene, it.entity);
    VK_LOG_INFO("MULTI-REDO '%s' n=%zu", m_name.c_str(), m_items.size());
}

// ---------------------------------------------------------------- 碰撞体变换

namespace {

// 把快照写进实体的 CollisionComponent（没有组件就什么都不做 —— 比如
// 撤销到碰撞体已被删除之前/之后的状态）。hullPoints 是派生数据，不动。
void applyColliderSnapshot(scene::Scene& scene, ecs::Entity e,
                           const TransformSnapshot& s) {
    auto* cc = scene.world().get<ecs::CollisionComponent>(e);
    if (!cc) return;
    cc->position = s.position;
    cc->rotation = s.rotation;
    cc->scale = s.scale;
}

} // namespace

void ColliderEditCommand::undo() {
    if (m_scene) applyColliderSnapshot(*m_scene, m_entity, m_before);
}

void ColliderEditCommand::redo() {
    if (m_scene) applyColliderSnapshot(*m_scene, m_entity, m_after);
}

// ---------------------------------------------------------------- 图元生成参数

void PrimitiveEditCommand::apply(const ecs::MeshComponent& snap) {
    if (!m_comp) return;
    // 参数先整体拷回去（指针也是旧的那颗，但下面会再按缓存键确认一次 ——
    // 两道都做是为了即使 AssetManager 换了实例也能自愈）
    *m_comp = snap;
    if (m_assets) rebuildPrimitive(*m_assets, *m_comp);
}

void PrimitiveEditCommand::undo() { apply(m_before); }
void PrimitiveEditCommand::redo() { apply(m_after); }

// ---------------------------------------------------------------- 参数编辑（E19）

ParamEditCommand::ParamEditCommand(std::string label, std::string paramId,
                                   void* target, std::vector<std::uint8_t> before,
                                   std::string beforeStr,
                                   std::vector<std::uint8_t> after,
                                   std::string afterStr)
    : m_label(std::move(label)), m_paramId(std::move(paramId)),
      m_target(target), m_before(std::move(before)),
      m_after(std::move(after)), m_beforeStr(std::move(beforeStr)),
      m_afterStr(std::move(afterStr)) {
    rebuildName();
}

void ParamEditCommand::rebuildName() {
    m_full = m_label + ": " + m_beforeStr + " -> " + m_afterStr;
}

void ParamEditCommand::mergeAfter(std::vector<std::uint8_t> after,
                                  std::string afterStr) {
    m_after = std::move(after);
    m_afterStr = std::move(afterStr);
    rebuildName();
}

void ParamEditCommand::undo() {
    if (!m_target || m_before.empty()) return;
    std::memcpy(m_target, m_before.data(), m_before.size());
}

void ParamEditCommand::redo() {
    if (!m_target || m_after.empty()) return;
    std::memcpy(m_target, m_after.data(), m_after.size());
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

// ---------------------------------------------------------------- PrefabApply

void PrefabApplyCommand::writePrefab(const std::string& fullPath,
                                     const std::string& text) {
    if (fullPath.empty()) return;
    const std::string tmp = fullPath + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            VK_LOG_ERROR("PrefabApplyCommand: cannot write %s", tmp.c_str());
            return;
        }
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        f.flush();
    }
    std::error_code ec;
    std::filesystem::rename(tmp, fullPath, ec);
    if (ec) {
        VK_LOG_ERROR("PrefabApplyCommand: rename failed: %s", ec.message().c_str());
        std::error_code ec2;
        std::filesystem::remove(tmp, ec2);
        return;
    }
    VK_LOG_INFO("PREFAB-UNDO-WRITE path=%s bytes=%d", fullPath.c_str(),
                static_cast<int>(text.size()));
}

void PrefabApplyCommand::undo() {
    writePrefab(m_prefabPath, m_prefabBefore);
    m_sceneCmd.undo();
}

void PrefabApplyCommand::redo() {
    writePrefab(m_prefabPath, m_prefabAfter);
    m_sceneCmd.redo();
}

} // namespace editor
