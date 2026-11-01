#pragma once
// ============================================================
// editor/CharacterStudioPanel —— 角色制作流程壳（2026-11-01）
//
// ------------------------------------------------------------
// 它解决的问题
// ------------------------------------------------------------
// 引擎的动画运行时很厚（`engine/animation/` 下四十多个文件：状态机 /
// 层遮罩 / 事件 / 根运动 / 双骨 IK / 弹簧骨 / LookAt / NLA / 步幅匹配），
// 但**编辑器侧的入口散在七个窗口里**：
//
//   Character(5176 行) · Dope sheet · Bone curves · Curve editor ·
//   NLA · Timeline editor · Cinematography
//
// 每个窗口各管一段，作者得自己记住"我现在走到哪一步、下一步点哪个窗口"。
// 更糟的是 `Animation` 工作区**根本没打开其中任何一个**（它只开
// hierarchy/viewport/inspector/scripts）—— 切过去等于什么都没准备。
//
// 本面板是一个**壳**：一条从上到下的步骤条，每步显示"这一步有没有数据"，
// 右侧显示这一步的现状 + 一个把它送进对应编辑器的按钮。
//
// ------------------------------------------------------------
// 三条明确的设计边界
// ------------------------------------------------------------
// ① **不重新实现任何一步**。已有的七个编辑器一个都没搬进来，也没有
//    复制它们的逻辑 —— 复制出来的第二份必然与第一份漂移。这里只做
//    "看状态 + 跳转 + 唯一缺的那一步（状态机，内嵌）"。
//
// ② **状态是算出来的，不是记下来的**。每一步的"完成度"每帧从场景里
//    现算（`sk->model` 有没有、`nlaTracks` 空不空）。存一份进度会有
//    两个真相源：用户删掉一个组件，步骤条还亮着绿点。
//
// ③ **没有目标实体时说清楚，不假装**。空场景里打开它，右侧显示
//    "选中一个带蒙皮的角色" + 为什么。空状态比"一个全是灰的界面"更能
//    回答"我该先做什么"。
// ============================================================

#include <imgui.h>

#include "ecs/Entity.h"

#include <string>

namespace assets {
struct Skeleton;
}

namespace ecs {
struct AnimatorComponent;
struct SkinnedMeshComponent;
}  // namespace ecs

namespace editor {

class AnimatorGraphPanel;
class EditorContext;

class CharacterStudioPanel {
public:
    // `graph` 由 EditorApp 持有（与 `animgraph` 面板是**同一个对象**）——
    // 内嵌的状态机编辑器必须和独立打开时共享同一份未保存改动，
    // 否则在 Studio 里改一半、关掉去别处打开，看到的是磁盘上的旧版本。
    CharacterStudioPanel(EditorContext& ctx, AnimatorGraphPanel& graph);

    void draw();

    bool isOpen() const { return m_open; }
    void setOpen(bool v) { m_open = v; }

    // 供外部（Inspector 的 "Open in Character Studio"）跳步。
    // 传步骤 id（"import" / "skeleton" / … / "statemachine"）。
    void goTo(const std::string& stepId);

private:
    void drawStepList(float width, float height);
    void drawStepBody();

    // 各步的"有没有数据"探针。返回 false = 这一步还没做。
    bool stepHasData(const char* id) const;

    // 当前步骤对应的独立面板 id（空 = 没有，比如状态机是内嵌的）。
    const char* stepPanelId(const char* id) const;

    void drawImportStep();
    void drawSkeletonStep();
    void drawSkinningStep();
    void drawPoseStep();
    void drawKeysStep();
    void drawCurvesStep();
    void drawNlaStep();
    void drawStateMachineStep();
    void drawLayersStep();
    void drawPreviewStep();

    // 取当前目标的蒙皮 / 动画组件（可能为空）
    struct Target {
        ecs::Entity entity;
        std::string name;
        ecs::SkinnedMeshComponent* sk = nullptr;
        ecs::AnimatorComponent* an = nullptr;
        const assets::Skeleton* skel = nullptr;
    };
    Target resolveTarget() const;

    EditorContext& m_ctx;
    AnimatorGraphPanel& m_graph;
    bool m_open = false;
    int m_step = 0;
    std::string m_status;
    bool m_statusIsError = false;
};

}  // namespace editor
