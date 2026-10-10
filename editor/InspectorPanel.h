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
#include "ThumbnailCache.h"

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
class Texture;
} // namespace assets

namespace ecs {
struct MeshComponent;
struct MeshPrimitiveParams;
} // namespace ecs

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
    void drawMeshSection(ecs::Entity e);
    void drawMaterialSection(ecs::Entity e);
    // 碰撞体段落：只在实体**已经有**碰撞组件时出现（添加走视口右键菜单）。
    // 不显示"Add"按钮是有意的 —— 碰撞体的默认形状要按网格算（凸包 / 胶囊），
    // 那需要有 mesh，视口菜单能顺手把 mesh 检查掉。
    void drawCollisionSection(ecs::Entity e);
    void drawLights(ecs::Entity e);
    void drawScript(ecs::Entity e);
    void drawAddRemove(ecs::Entity e);

    // 内置图元的生成参数拖完了：重建几何 + 入撤销栈
    void commitPrimitiveEdit(ecs::Entity e, const ecs::MeshComponent& before);

    // 一个材质纹理槽（"小方块 + 加号"）。挨着对应的因子控件放一行，
    // 从 Content 面板把图片拖进来即可绑定；绑定后方块里放缩略图、右边
    // 显示**文件名**，再点右键可以清空。返回 true 表示本帧改过。
    //   slotId   —— 同时也是日志 tag（MAT-SLOT <slotId>）与 PushID
    //   srgb     —— 颜色贴图传 true（albedo / emissive），数据贴图传 false。
    //               这个参数同时决定缓存键的后缀，见 .cpp 里的 slotCacheKey
    //   maxWidth —— 这一行剩下的水平空间：文件名按它裁剪，避免把窗口撑出去
    bool drawTextureChip(const char* slotId, const char* hintText,
                         assets::Texture** slot, bool srgb, float maxWidth);

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

    // 材质纹理槽的缩略图。和 ContentBrowser 共用同一份实现（同一张 GPU
    // 纹理 + ImGui 句柄的生命周期管理），见 ThumbnailCache.h。
    ThumbnailCache m_thumbs;

    // 正在拖拽的控件（ImGui 同时只有一个 active item，单槽位足够）
    const void* m_armTarget = nullptr;
    std::size_t m_armSize = 0;
    std::string m_armName;
    std::vector<std::uint8_t> m_armBefore;

    // 脚本路径的输入缓冲（跟着选中项走）
    ecs::Entity m_scriptBufFor{};
    char m_scriptBuf[256] = {0};

    // Mesh / Light 面板"这一帧到底画了哪些参数"的签名。
    //
    // 用途有两个：一是**验证**（tools/verify_mesh_params.py 靠这行日志断言
    // "Cube 只有 Size、Sphere 多出 Segments/Rings"这类结构性差异，比去截图
    // 里认控件靠谱得多）；二是排查"为什么这个属性没出现"。
    // 只在签名变化时打一行，不会刷屏 —— 和 logRect 的去抖是同一个思路。
    std::string m_lastMeshParamSig;
    std::string m_lastLightParamSig;
};

} // namespace editor
