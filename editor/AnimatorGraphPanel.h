#pragma once
// ============================================================
// editor/AnimatorGraphPanel —— 动画状态机图编辑器（2026-11-01）
//
// ------------------------------------------------------------
// 它补的是哪一段
// ------------------------------------------------------------
// `animation/AnimController.*` 从 A2b 起就真的在跑状态机了，但**没有编辑器**：
// `AnimatorComponent::controllerPath` 只能在 Inspector 里手填一个路径，
// 填错只有日志里一行 `ANIM-CTL-BAD`。想加一条 `Speed > 1.5 → Run` 的边，
// 得拿文本编辑器去改 `assets/animators/*.anim.json`，再回编辑器里等它重载。
//
// 这就是"角色动画流程不闭环"里**唯一要新写**的那一块 —— 其余步骤
// （骨架 / 蒙皮 / 姿态 / 关键帧 / 曲线 / 摄影表 / NLA）都已经有编辑器，
// 缺的是把它们串起来的壳（`CharacterStudioPanel`）。
//
// ------------------------------------------------------------
// 三个设计决定
// ------------------------------------------------------------
// ① **编辑的是文件，不是组件**。面板持有一份 `AnimController` 副本 +
//    一个路径，保存时整体写回 `.anim.json`。理由：
//      · 状态机是**资产**，可以给多个角色共用；绑在某个实体的组件上
//        就没法"改一份给所有人用"。
//      · 组件里的 `stateIndex / stateTime` 是**运行时状态**，编辑期根本
//        没有意义（没在播）。混在一起会让人以为"我删了个状态，
//        角色为什么还在播那个动作"。
//    运行时下次加载才生效 —— 这一点写在面板顶部（`applies on next load`），
//    不装成"改完立刻生效"。
//
// ② **保存不校验**，但把校验结果当警告打在面板上。
//    校验不过的控制器运行时会整个不接管（`ANIM-CTL-BAD` + 回落单 clip），
//    所以"存了但跑不起来"是真实存在的状态。拒绝写盘反而更糟：用户
//    做了一半想去吃个饭，回来发现全丢了。写盘 + 红字警告，比二选一好。
//
// ③ **转移的顺序就是优先级**（`AnimController.h` 文件头第 3 条），
//    所以列表里上下移动按钮是**语义操作**，不是排版偏好。
//    UI 上标注了这一点 —— 否则没人会去点它。
// ============================================================

#include "animation/AnimController.h"

#include <imgui.h>

#include <string>
#include <vector>

namespace editor {

class EditorContext;

class AnimatorGraphPanel {
public:
    // 打开某个 `.anim.json`（相对资产根或绝对路径都行）。
    // 加载失败时面板仍然打开 —— 里面会红字显示 `ctl.error`，
    // 并给一个"新建一份"的按钮。**不要**加载失败就不开窗：
    // 用户会以为"这个按钮坏了"。
    void open(const std::string& path);
    void close();
    bool isOpen() const { return m_open; }

    // 当前编辑的路径（Inspector 的 "Edit state machine…" 用它决定开谁）。
    const std::string& path() const { return m_path; }

    // 自己 Begin/End 的浮动窗口（面板表项 → `panelDrawAnimGraph`）。
    void draw(EditorContext& ctx);

    // 不 Begin/End 的正文 —— 供 `CharacterStudioPanel` 内嵌。
    // 内嵌时**不画窗口标题栏和路径栏**（那样在壳里会出现第二个输入框），
    // 由调用方负责路径输入。
    void drawBody(EditorContext& ctx, bool embedded);

private:
    // ---- 画布交互状态 ----
    enum class Sel { None, State, Transition };

    void loadFromDisk();
    void saveToDisk();
    void newController();

    // 没有坐标的状态按环形布点。**只在打开时做一次** —— 每帧都排会让
    // 用户拖过的位置在下一帧被抹掉（`m_dirty` 都来不及记）。
    void autoLayout();

    void drawToolbar();
    void drawParameterTable();
    void drawCanvas(ImVec2 size);
    void drawInspector();
    void drawStatusLine();

    // 三栏正文（参数 | 画布 | 属性）+ 状态行。`draw()` 与 `drawBody()`
    // 共用这一段 —— 否则内嵌版迟早与浮动版长得不一样。
    void drawContent(EditorContext& ctx);

    // 画布坐标 → 屏幕坐标
    ImVec2 toScreen(const ImVec2& canvasPos, const ImVec2& p) const;
    ImVec2 toCanvas(const ImVec2& canvasPos, const ImVec2& screen) const;

    // 命中测试。返回 -1 表示没命中。
    int hitNode(const ImVec2& canvasPos, const ImVec2& p) const;
    int hitTransition(const ImVec2& canvasPos, const ImVec2& p) const;

    // 一条转移的起止点（画布坐标），供连线和命中测试共用 ——
    // 两处各算一遍必然出现"看着连上了、点不着"。
    void transitionCurve(const animation::AnimController& ctl, int ti,
                         ImVec2* a, ImVec2* c1, ImVec2* c2, ImVec2* b) const;

    void addState();
    void addTransition(int from, int to);
    void removeSelected();

    void markDirty() { m_dirty = true; }

    bool m_open = false;
    std::string m_path;          // 相对资产根的路径（保存时用原样）
    // 路径输入框的**编辑缓冲**。必须是成员：写成 `static char buf[]` 的话
    // 每帧都会拿 m_path 覆盖回去，用户打一个字就消失（这个坑很经典，
    // 表现是"输入框像被冻住"）。
    char m_pathBuf[512] = {0};
    std::string m_status;        // 上一动作的结果，显示在状态行
    bool m_statusIsError = false;
    bool m_dirty = false;

    animation::AnimController m_ctl;

    Sel m_sel = Sel::None;
    int m_selState = -1;
    int m_selTrans = -1;

    // 画布视图
    ImVec2 m_pan{40.0f, 60.0f};
    float m_zoom = 1.0f;
    int m_dragNode = -1;         // 正在拖的节点
    ImVec2 m_dragOffset{0.0f, 0.0f};
    bool m_pannedOnce = false;   // 首次绘制时把视图居中
};

}  // namespace editor
