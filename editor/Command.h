#pragma once
// ============================================================
// editor/Command —— 撤销 / 重做（命令模式）
//
// 设计要点：
//
// 1) 命令栈用"游标"而不是双栈。
//    一个 vector 存全部命令，cursor = 已执行的条数。
//      push  → 截断 cursor 之后的内容（分支被丢弃），执行、追加
//      undo  → cursor-- 再 undo
//      redo  → redo 再 cursor++
//    双栈（undoStack/redoStack）要来回搬对象，游标法只动一个整数。
//
// 2) 两种命令粒度，各司其职：
//    · TransformEditCommand —— 精确、廉价。gizmo 拖拽/数值输入走这条。
//    · SceneSnapshotCommand —— 整场景 JSON 快照。新建/删除/改父子/
//      加载场景这类"结构性"改动走这条：它们会动实体句柄、组件集合、
//      层级表，精确回滚要写一大堆状态，用快照则一行搞定且绝不出错。
//      代价是 O(场景) 的一次序列化 —— 场景是几十~几百个实体、JSON 几十 KB，
//      而这类操作的频率是"人点一下按钮"，完全付得起。
//
// 3) 拖拽合并（coalescing）。
//    gizmo 拖动过程中每帧都在改 transform，但用户心里那只是"一次"操作。
//    做法：鼠标按下时记 before，松开时记 after，只在松开那一刻 push 一条
//    命令 —— 拖动中不入栈，就不会污染历史。
// ============================================================

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "ecs/Entity.h"       // Entity 按值持有，需要完整类型
#include "ecs/Components.h"  // PrimitiveEditCommand 按值持有 MeshComponent

namespace assets {
class AssetManager;
}
namespace scene {
class Scene;
}

namespace editor {

// ---------------------------------------------------------------- 基类

class Command {
public:
    virtual ~Command() = default;

    // 反向执行（把状态恢复到"操作前"）
    virtual void undo() = 0;
    // 正向执行（重做时调用；push 时也会调用一次）
    virtual void redo() = 0;
    // 供 UI 显示，例如 "Move CubeA"
    virtual const char* name() const = 0;
};

// ---------------------------------------------------------------- 命令栈

class CommandStack {
public:
    // 撤销深度上限。超过后从最老的开始丢弃 —— 编辑器里没人会连撤 256 步，
    // 但快照命令各占一份 JSON，无上限会一直涨。
    static constexpr size_t kDefaultLimit = 128;

    explicit CommandStack(size_t limit = kDefaultLimit) : m_limit(limit) {}

    // 执行并入栈（会丢弃当前游标之后的"重做分支"）
    void push(std::unique_ptr<Command> cmd);

    // 操作**已经生效**时用这个入栈 —— 只记录，不重复执行 redo。
    // （gizmo 拖动、结构性编辑都是"先改场景、后补历史"）
    void pushAlreadyApplied(std::unique_ptr<Command> cmd);

    bool undo();
    bool redo();

    bool canUndo() const { return m_cursor > 0; }
    bool canRedo() const { return m_cursor < m_stack.size(); }
    const char* undoName() const;
    const char* redoName() const;

    void clear();

    size_t depth() const { return m_stack.size(); }
    size_t cursor() const { return m_cursor; }
    // 历史（全部命令名，按时间顺序；cursor 之前的是"已执行"）
    std::vector<std::string> historyNames() const;

    // 栈顶（= 最近一条已执行的命令）。空栈 / 游标为 0 时返回 nullptr。
    // E19 的"合并连续编辑"要用它判断"上一条是不是同一个参数的编辑"。
    Command* topCommand() {
        if (m_cursor == 0) return nullptr;
        return m_stack[m_cursor - 1].get();
    }

private:
    std::vector<std::unique_ptr<Command>> m_stack;
    size_t m_cursor = 0;  // 已执行的命令条数
    size_t m_limit;
};

// ---------------------------------------------------------------- 具体命令

// 通用"两个闭包"命令：写小操作时最省事，
// 例如 renameEntity / toggleVisible / 改材质因子。
class LambdaCommand : public Command {
public:
    LambdaCommand(std::string name, std::function<void()> undoFn,
                  std::function<void()> redoFn)
        : m_name(std::move(name)), m_undo(std::move(undoFn)),
          m_redo(std::move(redoFn)) {}

    void undo() override {
        if (m_undo) m_undo();
    }
    void redo() override {
        if (m_redo) m_redo();
    }
    const char* name() const override { return m_name.c_str(); }

private:
    std::string m_name;
    std::function<void()> m_undo;
    std::function<void()> m_redo;
};

// ============================================================
// RawBytesEditCommand —— "就地改一个值"的通用命令
//
// 解决 Inspector 里大量 `DragFloat / ColorEdit / Checkbox` 的撤销问题：
//   · 这些控件的目标类型五花八门（float / glm::vec3 / glm::vec4 / bool /
//     enum），为每种都写一个 Command 类是纯浪费；
//   · 它们都满足"一个平凡可拷贝的 POD + 一个地址"，于是统一成
//     "把 sizeof(T) 字节拷来拷去"。
//
// 用法（配合 ImGui 的激活/失活时序，见 InspectorPanel）：
//   控件前抄一份旧值 → 控件 → 用 == 判断有没有变 → push 一条本命令。
//
// 前提：target 在本命令存活期间（撤销栈深度 128 步内）不会被释放。
// 编辑器里改的都是 ECS 组件字段或 AssetManager 长生命周期的材质，
// 这个前提成立。注意**不要**用它去改"整场景重建后会被搬走"的东西。
// ============================================================
class RawBytesEditCommand : public Command {
public:
    RawBytesEditCommand(std::string name, void* target, const void* before,
                        const void* after, std::size_t size)
        : m_name(std::move(name)), m_target(target), m_size(size) {
        m_before.resize(size);
        m_after.resize(size);
        std::memcpy(m_before.data(), before, size);
        std::memcpy(m_after.data(), after, size);
    }

    void undo() override { std::memcpy(m_target, m_before.data(), m_size); }
    void redo() override { std::memcpy(m_target, m_after.data(), m_size); }
    const char* name() const override { return m_name.c_str(); }

private:
    std::string m_name;
    void* m_target = nullptr;
    std::size_t m_size = 0;
    std::vector<std::uint8_t> m_before;
    std::vector<std::uint8_t> m_after;
};

// ============================================================
// ParamEditCommand —— 注册表参数（渲染设置）的可撤销编辑（E19）
//
// 与 RawBytesEditCommand 的区别在于**能合并**：
//   拖一次滑条 = `IsItemActivated` 记 before、`IsItemDeactivatedAfterEdit`
//   落一条命令 —— 本来就是一次。但"连拖两下"（松手又马上按下去）在用户
//   心里仍是一次调整，本命令支持把第二段的 after 并进同一条记录，
//   于是历史里只留一行 "Set SSR Strength: 0.50 → 0.80"。
//
// 名字预拼好（"Set <参数 label>: <旧值> → <新值>"）：Command::name() 返回
// const char*，若在 name() 里现拼就得持有静态缓冲，多实例会互相覆盖。
// ============================================================
class ParamEditCommand : public Command {
public:
    // label 形如 "Set SSR Strength"；beforeStr/afterStr 已是格式化好的值
    ParamEditCommand(std::string label, std::string paramId, void* target,
                     std::vector<std::uint8_t> before, std::string beforeStr,
                     std::vector<std::uint8_t> after, std::string afterStr);

    void undo() override;
    void redo() override;
    const char* name() const override { return m_full.c_str(); }

    const std::string& paramId() const { return m_paramId; }
    // 合并：只换 after（before 保持不变）——语义是"还是同一次操作"
    void mergeAfter(std::vector<std::uint8_t> after, std::string afterStr);

private:
    void rebuildName();

    std::string m_label;      // "Set SSR Strength"
    std::string m_paramId;    // "ssr.strength"
    void* m_target = nullptr;
    std::vector<std::uint8_t> m_before;
    std::vector<std::uint8_t> m_after;
    std::string m_beforeStr;
    std::string m_afterStr;
    std::string m_full;       // 预拼好的完整名字
};

// 实体的 TRS 快照
struct TransformSnapshot {
    glm::vec3 position{0.0f};
    glm::vec3 rotation{0.0f};
    glm::vec3 scale{1.0f};

    static TransformSnapshot capture(scene::Scene& scene, ecs::Entity e);
    void apply(scene::Scene& scene, ecs::Entity e) const;
};

// 精确的变换编辑（gizmo 拖动、Inspector 里的 DragFloat）
class TransformEditCommand : public Command {
public:
    TransformEditCommand(std::string name, scene::Scene* scene, ecs::Entity e,
                         TransformSnapshot before, TransformSnapshot after)
        : m_name(std::move(name)), m_scene(scene), m_entity(e),
          m_before(before), m_after(after) {}

    void undo() override;
    void redo() override;
    const char* name() const override { return m_name.c_str(); }

    // 供拖拽合并判断：同一个实体 + 同一个名字 → 可以就地更新 after
    ecs::Entity entity() const { return m_entity; }
    const TransformSnapshot& before() const { return m_before; }
    void setAfter(const TransformSnapshot& a) { m_after = a; }

private:
    std::string m_name;
    scene::Scene* m_scene = nullptr;
    ecs::Entity m_entity{};
    TransformSnapshot m_before;
    TransformSnapshot m_after;
};

// ---- 多选时的一次拖拽（2026-10-27 #316）----
//
// 为什么不是"每实体压一条 TransformEditCommand"：那样一次拖拽会在历史里
// 留下 N 条同名记录，Ctrl+Z 要按 N 下才回到拖之前 —— 用户认为自己只做了
// **一个**动作，历史里就该只有一条。
//
// 也不复用 TransformEditCommand 的合并逻辑（同实体 + 同名字 → 就地更新）：
// 多选时"同名字"是常态，合并键要变成实体集合，改它反而会污染单选的路径。
class MultiTransformEditCommand : public Command {
public:
    // 单项 = 一个实体在拖拽前后的 TRS 快照。
    struct Item {
        ecs::Entity entity{};
        TransformSnapshot before;
        TransformSnapshot after;
    };

    MultiTransformEditCommand(std::string name, scene::Scene* scene,
                             std::vector<Item> items)
        : m_name(std::move(name)), m_scene(scene), m_items(std::move(items)) {}

    void undo() override;
    void redo() override;
    const char* name() const override { return m_name.c_str(); }

    const std::vector<Item>& items() const { return m_items; }

private:
    std::string m_name;
    scene::Scene* m_scene = nullptr;
    std::vector<Item> m_items;
};

// 碰撞体的 TRS 编辑（W/E/R 拖手柄、Inspector 里的数值框）。
//
// 为什么不复用 TransformEditCommand：它写的是实体的 TransformComponent，
// 而碰撞体是自己的三个分量（CollisionComponent::position/rotation/scale）。
// 三个分量的类型与语义完全一样，所以复用同一个快照结构体，只是落点不同。
class ColliderEditCommand : public Command {
public:
    ColliderEditCommand(std::string name, scene::Scene* scene, ecs::Entity e,
                        TransformSnapshot before, TransformSnapshot after)
        : m_name(std::move(name)), m_scene(scene), m_entity(e),
          m_before(before), m_after(after) {}

    void undo() override;
    void redo() override;
    const char* name() const override { return m_name.c_str(); }

private:
    std::string m_name;
    scene::Scene* m_scene = nullptr;
    ecs::Entity m_entity{};
    TransformSnapshot m_before;
    TransformSnapshot m_after;
};

// ---------------------------------------------------------------- 图元生成参数
//
// 内置图元的半径 / 分段数这类参数，改一下就要**重建几何**，所以不能用
// RawBytesEditCommand —— 那个只按字节回滚参数，回滚完 mesh 还停在
// 新几何上，参数和几何就对不上了。
//
// 这条命令把两者一起回滚：整体把 MeshComponent 拷回去，再让
// AssetManager 按缓存键重取一遍。因为 AssetManager 从不淘汰资源，
// "回到旧参数"拿回来的就是当初那颗一模一样的 mesh（连指针都相同）。
class PrimitiveEditCommand : public Command {
public:
    PrimitiveEditCommand(std::string name, ecs::MeshComponent* comp,
                         assets::AssetManager* assets,
                         ecs::MeshComponent before, ecs::MeshComponent after)
        : m_name(std::move(name)), m_comp(comp), m_assets(assets),
          m_before(std::move(before)), m_after(std::move(after)) {}

    void undo() override;
    void redo() override;
    const char* name() const override { return m_name.c_str(); }

private:
    void apply(const ecs::MeshComponent& snap);

    std::string m_name;
    ecs::MeshComponent* m_comp = nullptr;
    assets::AssetManager* m_assets = nullptr;
    ecs::MeshComponent m_before;
    ecs::MeshComponent m_after;
};

// ---------------------------------------------------------------- 场景快照
//
// 为什么不做"按名字恢复选择"：撤销删除之后，回滚出来的实体句柄是全新的，
// 命令对象无从知道编辑器当前选中了什么。把这件事故意留给 EditorContext ——
// 它持有名字，在每次 undo/redo 之后按名字重新 find 一次即可（见
// EditorContext::rebuildSelectionByName），命令本身保持纯粹。
class SceneSnapshotCommand : public Command {
public:
    SceneSnapshotCommand(std::string name, scene::Scene* scene,
                         assets::AssetManager* assets, std::string before,
                         std::string after)
        : m_name(std::move(name)), m_scene(scene), m_assets(assets),
          m_before(std::move(before)), m_after(std::move(after)) {}

    void undo() override;
    void redo() override;
    const char* name() const override { return m_name.c_str(); }

private:
    void restore(const std::string& json);

    std::string m_name;
    scene::Scene* m_scene = nullptr;
    assets::AssetManager* m_assets = nullptr;
    std::string m_before;
    std::string m_after;
};

// ============================================================
// 预制体 Apply：**文件 + 场景**一起回滚（G18）
//
// ⚠ 为什么不能用 `structuralEdit`：它只快照**场景**，而 Apply 同时改了
//   磁盘上的 `.vprefab.json`。撤销场景而文件不回滚 → 下一次重新加载就把
//   改动"复活"了，而屏幕上、撤销栈里一切正常 —— 这种"撤销了但没撤销"
//   是本功能最难查的一类 bug（docs/persistence_roadmap.md §6.4 也点了名）。
//
// 场景那半边直接**组合**一个 `SceneSnapshotCommand`，不重写恢复逻辑：
// 两份恢复代码迟早会不一致，而不一致的后果是"撤销后场景半新半旧"。
// ============================================================
class PrefabApplyCommand : public Command {
public:
    PrefabApplyCommand(std::string name, scene::Scene* scene,
                       assets::AssetManager* assets, std::string prefabFullPath,
                       std::string prefabBefore, std::string prefabAfter,
                       std::string sceneBefore, std::string sceneAfter)
        : m_name(std::move(name)),
          m_sceneCmd(m_name, scene, assets, std::move(sceneBefore),
                     std::move(sceneAfter)),
          m_prefabPath(std::move(prefabFullPath)),
          m_prefabBefore(std::move(prefabBefore)),
          m_prefabAfter(std::move(prefabAfter)) {}

    void undo() override;
    void redo() override;
    const char* name() const override { return m_name.c_str(); }

private:
    static void writePrefab(const std::string& fullPath,
                            const std::string& text);

    std::string m_name;
    SceneSnapshotCommand m_sceneCmd;
    std::string m_prefabPath;
    std::string m_prefabBefore;
    std::string m_prefabAfter;
};

} // namespace editor
