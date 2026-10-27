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
    // Del / 右键 Delete。**多选时一次全删**（一条撤销记录），这是 #316 之后
    // 与单选最大的行为差别 —— 所以两个入口（快捷键 / 右键菜单）都收敛到
    // 这一个函数，谁也别自己再写一遍删除。
    void deleteSelection();

private:
    void drawNode(ecs::Entity e, int depth);
    void drawNodeContextMenu(ecs::Entity e);

    // ---- E12 过滤升级：type: 前缀语法 + 保留祖先链 ----
    // nodeMatches = 本节点是否命中（文本或 type: 语法）；
    // subtreeMatches = 本节点或任一后代命中（祖先因此保持可见）。
    bool nodeMatches(ecs::Entity e, const std::string& name) const;
    bool subtreeMatches(ecs::Entity e) const;

    // 画完之后统一执行（避免在遍历中动 World）
    void flushPending();

    EditorContext& m_ctx;

    char m_filter[64] = {0};

    // ---- 列头（项目标签 / 类型）----------
    // 类型列左边界与右边界（屏幕坐标）。每帧在 draw() 里算一次，所有行共用
    // —— 行在树 child 之内、列头在 child 之外，两边各算各的必然差 8px 内边距
    // （有滚动条时还差 14px）。见 .cpp 里列头那一段。
    float m_typeColX = 0.0f;
    float m_typeColRight = 0.0f;

    // 重命名
    ecs::Entity m_renameTarget{};
    char m_renameBuf[128] = {0};
    bool m_renameFocus = false;

    // ---- pending：树画完再执行 ----
    ecs::Entity m_pendingDelete{};
    ecs::Entity m_pendingDuplicate{};
    ecs::Entity m_pendingFocus{};
    // 本帧的**可见行顺序**（DFS 结果，受过滤影响）。Shift 范围选择要它：
    // 场景里没有"树的顺序"这种东西，只有 HierarchyComponent::children 的
    // 顺序；而"哪些行真的画出来了"又是本面板自己的事。
    // ⚠ 必须在画行**之前**整棵走完（不能边画边记）：点击落在第 5 行时，
    //   第 6 行以后的顺序还没进表，Shift 会选出半个范围。
    std::vector<ecs::Entity> m_flatOrder;
    void collectFlatOrder(ecs::Entity e);

    // 每行矩形的日志节流（见 .cpp 里的 HIER-ROW）。这两个成员只为自动化
    // 服务：`m_rowLogNames` 记"上次这一行打印时叫什么"，名字没变就不打。
    static constexpr int kMaxLoggedRows = 16;
    int m_rowCursor = 0;
    std::vector<std::string> m_rowLogNames;
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

    // ---- G18 预制体（右键菜单三项）----
    // 与删除/重设父级同样是 pending：菜单项的回调发生在遍历 ECS 的过程里，
    // 而这三项都要建/改实体，只能等树画完之后再做。
    bool m_hasPrefabCreate = false;
    ecs::Entity m_prefabCreateTarget{};
    bool m_hasPrefabApply = false;
    ecs::Entity m_prefabApplyTarget{};
    bool m_hasPrefabRevert = false;
    ecs::Entity m_prefabRevertTarget{};
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
