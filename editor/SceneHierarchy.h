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
    //
    // ⚠ 2026-10-28（#323）：光比名字**不够**。名字没变但位置挪了（窗口首帧
    //   才定尺寸 → 整块面板重排一行；树被滚动；分割条被拖）时不会补打，
    //   于是 `wait_rect` 一直返回**旧坐标**，脚本点下去落到隔壁行。
    //   实测症状是"Ctrl+点 CubeB，日志里却是 name=CubeC"，而且只在连着跑
    //   多个用例时出现（单跑时窗口早就定好尺寸了）。所以再记一份矩形。
    static constexpr int kMaxLoggedRows = 16;
    int m_rowCursor = 0;
    std::vector<std::string> m_rowLogNames;
    std::vector<glm::vec4> m_rowLogRects;   // (x0,y0,x1,y1)，0.5px 容差比
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

    // ---- G16-4 尾巴：按 guid 选（2026-10-31）----
    //
    // 为什么要有：guid 是**唯一**跨会话稳定的身份，而树里显示的是名字 ——
    // 名字可改、可重名、可以没有。排查"存档里那个 `…01` 到底是哪一根"
    // 时，除了右键复制 guid，没有第二条路。
    //
    // ⚠ 这两项都**不能**记成 pending：`Copy GUID` 是只读（不碰 World），
    //   `Select by GUID` 只改选中、不增删实体（`collectFlatOrder` 遍历的是
    //   每帧重算的 DFS 表，不是组件数组）。而"选中"本身要立刻生效，
    //   否则弹窗关掉的下一帧高亮才跳过去，看起来像点错了。
    //
    // ⚠ 弹窗**必须在 `draw()` 里画**，不能在右键 popup 里 `BeginPopupModal`
    //   —— 菜单项的回调发生在 `BeginPopupContextItem` 的栈里，
    //   在那儿开模态窗口会被外层 popup 的 focus 抢掉（点不中按钮）。
    bool m_guidQueryOpen = false;
    // ⚠⚠ 菜单项那一下**不能**当帧就 `OpenPopup`：那一帧正在关右键菜单，
    //   ImGui 会把"这一帧新开的 popup"当成被同一次点击带开的窗口而立刻
    //   关掉 —— 表现就是"菜单消失了、弹窗没出来"，而日志里一个错都不报
    //   （2026-10-31 实测：点 `Select by GUID…` 后画面什么都不变）。所以
    //   按下时只置 pending，下一帧在 `draw()` 里才真正 Open。
    bool m_guidQueryPending = false;
    char m_guidQuery[32] = {0};
    // 回车/点 Select 判定失败之后要把焦点**还给输入框**：`EnterReturnsTrue`
    // 会让 ImGui 在返回 true 的同时清掉 active id，不还焦点的话"发现打错了、
    // 接着改"必须先拿鼠标点一下输入框 —— 一个纯手感的坑，但它会让
    // "同一个弹窗里改成正确 guid"这件事变得做不到（C2 就是这么红的）。
    bool m_guidRefocus = false;
    // 0 = 还没试 / 1 = 格式不对 / 2 = 格式对但场景里没有这个 guid
    int m_guidQueryBad = 0;
    std::string m_guidQueryMsg;   // 弹窗里那句红字（已翻译）
    void drawGuidQuery();

};

} // namespace editor
