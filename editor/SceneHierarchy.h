#pragma once
// ============================================================
// editor/SceneHierarchy —— 场景树
//
// 层级来自 ecs::HierarchyComponent（parent + children，都存 EntityId）。
// 序列化时父子关系写成**数组下标**（见 SceneSerializer），所以
// "改父子关系"是一次结构性编辑 —— 走 EditorContext::structuralEdit
// 的整场景快照撤销，不需要为层级单独写回滚逻辑。
//
// 一条铁律：**不在遍历 ECS 的过程中增删实体**。
// World 的 each() 直接遍历组件的 dense 数组，中途增删会打乱它。
// 所以本面板把所有破坏性操作（新建 / 删除 / 复制 / 改父子）记成
// "pending"，等树画完再统一执行。
// ============================================================

#include "EditorContext.h"
#include "EditorScene.h"  // PrimitiveKind
#include "PickingSystem.h"  // E13：拖放导入模型后按包围盒贴地

#include <string>
#include <utility>
#include <vector>

namespace editor {

class SceneHierarchy {
public:
    explicit SceneHierarchy(EditorContext& ctx) : m_ctx(ctx) {}

    void draw();

    // ---- 快捷键入口（由 EditorApp 转发，见 .cpp 里的说明）----
    void beginRenameSelection();   // F2
    void duplicateSelection();     // Ctrl+D

private:
    void drawNode(ecs::Entity e, int depth);
    void drawNodeContextMenu(ecs::Entity e);
    // E12 顶部面包屑：选中实体的祖先链，可点击跳转
    void drawBreadcrumb();

    // ---- E12 过滤升级：type: 前缀语法 + 保留祖先链 ----
    // nodeMatches = 本节点是否命中（文本或 type: 语法）；
    // subtreeMatches = 本节点或任一后代命中（祖先因此保持可见）。
    bool nodeMatches(ecs::Entity e, const std::string& name) const;
    bool subtreeMatches(ecs::Entity e) const;

    // 画完之后统一执行（避免在遍历中动 World）
    void flushPending();

    EditorContext& m_ctx;

    char m_filter[64] = {0};

    // 重命名
    ecs::Entity m_renameTarget{};
    char m_renameBuf[128] = {0};
    bool m_renameFocus = false;

    // ---- pending：树画完再执行 ----
    ecs::Entity m_pendingDelete{};
    ecs::Entity m_pendingDuplicate{};
    ecs::Entity m_pendingFocus{};
    bool m_pendingCreate = false;
    PrimitiveKind m_pendingCreateKind = PrimitiveKind::Cube;
    // 新建到哪一级（无效 = 根下）；m_createAsFolder = 建成名为 Folder 的空节点
    ecs::Entity m_createParent{};
    bool m_createAsFolder = false;
    bool m_hasReparent = false;
    ecs::Entity m_reparentChild{};
    ecs::Entity m_reparentParent{};
    bool m_hasUnparent = false;
    ecs::Entity m_unparentTarget{};
    // 文件夹标记（Mark / Unmark as Folder）
    bool m_hasGroupToggle = false;
    ecs::Entity m_groupTarget{};
    bool m_groupValue = false;
    bool m_hasRename = false;
    ecs::Entity m_renameCommit{};
    std::string m_renameValue;

    // ---- E12 状态列的 pending（👁 可见 / 🔒 锁定，树画完再执行）----
    struct VisToggle {
        ecs::Entity e;
        bool value;
    };
    std::vector<VisToggle> m_pendingVisToggles;
    std::vector<std::pair<ecs::Entity, bool>> m_pendingLockToggles;

    // ---- E13 拖放导入：把 Content 里的模型拖到树上 → 生成实体 ----
    // 记成 pending：拖放回调发生在遍历 ECS 的过程中，不能当场创建实体。
    std::string m_pendingImportRel;
    PickingSystem m_picking;
};

} // namespace editor
