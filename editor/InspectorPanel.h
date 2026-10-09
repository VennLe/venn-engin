#pragma once
// ============================================================
// editor/InspectorPanel —— 选中实体的属性编辑
//
// 面板本身只做"显示 + 把改动落到组件上"，撤销记录交给两个辅助函数：
//
//   fieldEdit()    —— 连续型控件（DragFloat / SliderFloat / ColorEdit /
//                     DragFloat3）。ImGui 同一时刻只有一个 active item，
//                     所以"激活时抄旧值、失活时入栈"能把整段拖拽
//                     合并成**一条**撤销记录，不会把历史刷满。
//   discreteEdit() —— 离散型控件（Checkbox / Combo）。一次点击就是一次
//                     操作，直接用点击前后两个值入栈。
//
// 两者都走 RawBytesEditCommand（见 Command.h）：它不关心目标是 float、
// glm::vec3 还是 bool，只按字节数拷贝，所以这里不需要为每种字段写命令类。
//
// 有一个坑必须点名：**材质是共享资产**。AssetManager 按名字缓存 Material，
// 两个实体可以指向同一个 Material*。改材质因子会影响所有使用者，而且
// 它不在场景 JSON 里（只记来源描述）——所以材质编辑只能用 value 命令，
// 不能用整场景快照命令去做撤销。
// ============================================================

#include "EditorContext.h"

#include "ecs/Entity.h"

#include <glm/glm.hpp>

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace assets {
class Material;
}

namespace editor {

class InspectorPanel {
public:
    explicit InspectorPanel(EditorContext& ctx) : m_ctx(ctx) {}

    void draw();

private:
    void drawSceneryFallback();  // 没选中时显示相机 / 主光 / 场景信息
    void drawTransform(ecs::Entity e);
    void drawVisibility(ecs::Entity e);
    void drawMeshAndMaterial(ecs::Entity e);
    void drawLights(ecs::Entity e);
    void drawScript(ecs::Entity e);
    void drawAddRemove(ecs::Entity e);

    // ---- 撤销辅助（定义在头里，模板需要可见）----
    //
    // 只在编辑态记录历史。原因不只是"运行态的编辑反正会被丢弃"：
    // Stop 会 clear() 运行态世界，命令里存的目标指针当场变成悬垂指针，
    // 之后再按撤销就是往已释放内存里写。所以运行时一律不记录。
    template <typename T>
    void fieldEdit(EditorContext& ctx, const char* name, T* target,
                   const T& before) {
        if (!ctx.isEditing()) {
            m_armTarget = nullptr;
            return;
        }
        if (ImGui::IsItemActivated()) {
            m_armTarget = target;
            m_armSize = sizeof(T);
            m_armName = name;
            m_armBefore.assign(
                reinterpret_cast<const std::uint8_t*>(&before),
                reinterpret_cast<const std::uint8_t*>(&before) + sizeof(T));
        }
        if (m_armTarget != target || m_armSize != sizeof(T)) return;
        if (!ImGui::IsItemDeactivatedAfterEdit()) return;

        if (m_armBefore.size() == sizeof(T) &&
            std::memcmp(m_armBefore.data(), target, sizeof(T)) != 0) {
            ctx.commands().pushAlreadyApplied(
                std::make_unique<RawBytesEditCommand>(
                    m_armName, target, m_armBefore.data(), target, sizeof(T)));
            ctx.setStatus(m_armName);
            ctx.dirty() = true;
        }
        m_armTarget = nullptr;
    }

    template <typename T>
    void discreteEdit(EditorContext& ctx, const char* name, T* target,
                      const T& before) {
        if (!ctx.isEditing()) return;
        if (std::memcmp(&before, target, sizeof(T)) == 0) return;
        ctx.commands().pushAlreadyApplied(std::make_unique<RawBytesEditCommand>(
            name, target, &before, target, sizeof(T)));
        ctx.setStatus(name);
        ctx.dirty() = true;
    }

    EditorContext& m_ctx;

    // 正在拖拽的控件（ImGui 同时只有一个 active item，单槽位足够）
    const void* m_armTarget = nullptr;
    std::size_t m_armSize = 0;
    std::string m_armName;
    std::vector<std::uint8_t> m_armBefore;

    // 脚本路径的输入缓冲（跟着选中项走）
    ecs::Entity m_scriptBufFor{};
    char m_scriptBuf[256] = {0};
};

} // namespace editor
