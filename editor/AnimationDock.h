#pragma once
// ============================================================
// editor/AnimationDock —— 编辑器底部的"动画时间轴坞"（2026-11-05）
//
// ------------------------------------------------------------
// 它解决的是什么问题（用户原话）
// ------------------------------------------------------------
// 「对于角色的动画、骨骼、还有运镜的相关面板，在用户操作过程中应该是和
//   场景内容对照的…关于动画的这块，面板只需要一个就够了，能否浓缩到一个
//   操作面板…放在整个编辑器的底部，类似 UE5 和 Blender 那样。」
//
// 改造前：摄影表 / 曲线 / 骨骼曲线 / NLA / 运镜 / 过场 六张面板各自是
// **视口标签页**（kSlotNone → 被 ViewportTabs 托管到 3D 画面里）。切到
// 摄影表的那一刻，视口整块不画 —— 也就是说"对着场景调动画"这件事在
// 布局上就不成立：你只能看着曲线看不见角色。
//
// 改造后：这六张面板搬到编辑器**底部**的一条通栏里（在底栏之上），
// 3D 视口仍然完整可见，上面是场景、下面是时间轴，与 UE5 的 Sequencer /
// Blender 的 Graph Editor 同一套摆法。
//
// ------------------------------------------------------------
// 为什么是"坞"而不是"再写一套时间轴 UI"
// ------------------------------------------------------------
// 这六张面板里已经积累了大量编辑器逻辑（关键帧选择、镜像、对齐、注入
// 脚本、NLA 片段的拖拽、运镜的 nudge-then-bake…）。重写一遍等于把那些
// 判据全部作废。所以这里只做三件事：
//   1. 一条传输条（回到起点 / 上一帧 / 播放暂停 / 下一帧 / 到终点 + 帧号）
//   2. 一条共享的时间标尺（播放头可以把场景时间拖着走）
//   3. 一个标签条，把"当前是哪张面板"钉到底部这块矩形里
// 真正的面板自己一行没改 —— 它们照旧调 `applyDockHint()`，只是这次拿到的
// 矩形在底部而不是视口里。
//
// ------------------------------------------------------------
// 谁控制显隐
// ------------------------------------------------------------
// 坞本身**没有**自己的面板表项：它的显隐 = 「六张面板里有没有一张开着」。
// 这样"打开摄影表"这件事只有一个状态（`panelOpen("dope")`），不会出现
// "坞开着但六张都关着"或者反过来的双真值。菜单 / 底栏托盘 / Alt 快捷键 /
// 布局落盘全部沿用面板表那一套，一个字都不用新加。
// ============================================================

// ⚠ 这里**不** include <imgui.h>：本头文件只声明接口，矩形用四个 float 传。
//   EditorApp.h 是按值持有本类的，把它拖进 imgui.h 会让每个认识 EditorApp
//   的编译单元都多一份 imgui 依赖（那个头文件本来刻意避开了 imgui）。

namespace editor {

class EditorApp;
class EditorContext;
struct PanelDesc;

class AnimationDock {
public:
    // 这一族面板的 id（顺序 = 底部标签条的顺序）。
    // ⚠ 顺序即语义：前三张是"角色动画"，中间两张是"骨架 / 片段"，最后
    //   一张是"镜头"。用户从左边切到右边，正好是"从动作到镜头"的顺序。
    static constexpr int kTabCount = 6;
    static const char* tabId(int i);
    static const char* tabLabelKey(int i);   // TR() 的键

    // 这个面板 id 属不属于本坞（属 → drawPanels 不再自己派发它）
    static bool ownsPanel(const char* id);
    // 这个面板 id 对应第几页（-1 = 不是本坞的）
    static int tabOfPanel(const char* id);
    // 切页：只留一张开着（其余全部关掉）。改的是面板表的 open 位，
    // 所以菜单勾选 / 托盘按钮 / 落盘同一步到位。
    // 三个调用点：坞里点标签 / Inspector 的 "Edit timeline…" /
    //  `MYVK_EDITOR_CURVED` 这类自动化入口 —— 后两者必须能把坞**抢过来**，
    // 否则"请求开的是另一张页，画面却停在原来的页上"。
    static void selectTab(int index);
    // 六张里有没有一张开着（= 坞要不要占位）
    static bool anyOpen();
    // 当前活动页（第一个开着的）；-1 = 一张都没开
    static int activeTab();

    // 画整条坞。`height` 是**引用**：拖动上边缘会就地改它（EditorApp 持有
    // 这个数字，因为 computeLayout 要用它把主体让出来）。
    void draw(EditorApp& app, EditorContext& ctx, float x, float y, float w,
              float& height);

private:
    // 传输条 / 标尺的高度。**跟着字号走，不是常量** ——
    // 早期写死 30/22，在本机（字号 20px）按钮里的文字被窗口裁剪矩形的下沿
    // 齐刷刷切掉下半截（"Dope sheet" 只剩上半），而 1280×800 的开发机看
    // 不出来（那里字号小一档）。见 docs/anim_dock_and_selection_outline_2026-11-05.md
    static float headerH();
    static float rulerH();

    void drawHeader(EditorApp& app, EditorContext& ctx);
    void drawRuler(EditorContext& ctx, float w);

    // 标尺范围（秒）。**不是**片段的真实时长 —— 片长要问 AnimatorComponent
    // + 资产，而那是异步加载的；这里是一个纯粹的"标尺窗口"，用户可以改。
    // 拖播放头 / 点标尺都会直接写 ctx 的动画时钟。
    float m_start = 0.0f;
    float m_end = 5.0f;
    bool m_scrubbing = false;
    // 拖动上边缘改高度时的起始值
    bool m_resizing = false;
    float m_resizeStartH = 0.0f;
    float m_resizeStartY = 0.0f;
};

} // namespace editor
