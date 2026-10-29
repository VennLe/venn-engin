#pragma once
// ============================================================
// editor/NlaPanel —— NLA 时间轴面板（R9，2026-10-29）
//
// ------------------------------------------------------------
// 它和 `AnimLayer` 面板（Character 窗口里的 Layers 段）的区别
// ------------------------------------------------------------
// 两者看起来都是"多段动画叠在一起"，所以这里必须把话说死（语义差别写在
// `animation/NlaTimeline.h` 的开头，这里只说 UI 上的后果）：
//
//   · Layers 段：**没有时间轴**。一行 = 一段 clip + 一个遮罩 + 一个权重，
//     "现在盖不盖着"。它的 UI 是表单。
//   · 本面板：**就是一条时间轴**。一条 strip 有起止时刻、有包络、有在
//     时间轴上的位置。它的 UI 是画布 —— 因为"第 3.2 秒演什么"这个问题
//     只有横轴是时间的画布回答得了。
//
// 所以：**没有把 Layers 搬进本面板，也没有把 strip 塞进 Layers**。
// 这不是"暂时分开"，是两件不同的事（任务书里点名了硬合并的下场）。
//
// ------------------------------------------------------------
// 为什么编辑目标是"选中实体身上的组件"，而不是一个 `.nla.json` 资产
// ------------------------------------------------------------
// `layers` 与 `eventTracks` 的先例是"跟场景一起落盘"：NLA 也一样
// （见 `Components.h` 里 `nlaTracks` 的注释）。于是本面板**不持有数据**，
// 它每次 `draw` 都去场景里取组件、直接改字段。好处有三个：
//   ① 没有第二条 IO 通路，也就没有"面板里改的和场景里存的不一致"；
//   ② 拖一下 strip，视口里立刻动（走的就是 `AnimatorSystem` 那条正常
//      路径，不需要预览专用的求值器）；
//   ③ 撤销 / 存盘 / 多场景 全部免费继承场景那一套。
//
// 代价是"必须有个目标实体"。默认跟随当前选择；没有 AnimatorComponent 时
// 面板会把话说明白（而不是假装打开了一个空时间轴 —— 那会让人以为
// "我明明拖了 strip 怎么没反应"）。
//
// ------------------------------------------------------------
// 三条 UI 约束（都是被坑出来的）
// ------------------------------------------------------------
// ⚠ ① **拖动中不许重排**。strip 在列表里的顺序 = 求值顺序（语义！），
//    拖动改的是 `start/end`，**绝不能**顺手按 start 排序 —— 那会让"我
//    调过顺序"在松手的一刻被抹掉，而且是在用户看着画布的时候。
//    显示上按 `start` 排是另一回事（那是给眼睛看的，见 `displayOrder()`）。
//
// ⚠ ② **拖边 = 缩放，拖体 = 平移**，命中区按像素算（`kEdgePx`）。
//    两者都改 `start/end`，所以必须先在按下那一刻决定意图，之后整段
//    拖动都按那个意图走 —— 每帧重新判断题，鼠标从"边上"滑到"体内"时
//    会把一次缩放变成一次平移。
//
// ⚠ ③ **播放头归组件管**（`nlaTime`），面板只是读写它。面板自己再存一份
//    就会分叉：进 Play 时组件那一份在跑、面板那一份停着，画上的短线
//    与画面上的动作对不上。
// ============================================================

#include "ecs/Entity.h"

#include <string>
#include <vector>

// 前向声明而不是 include "ecs/Components.h"：这个头只拿它当**指针**用
// （`animator()` 返回引用），而 Components.h 是全仓最重的头之一 ——
// 为一个指针把它拖进每个用到本面板的编译单元不值得。
namespace ecs {
struct AnimatorComponent;
}

namespace editor {

class EditorContext;

class NlaPanel {
  public:
    explicit NlaPanel(EditorContext& ctx);

    // 打开面板。`target` 无效 = 跟随当前选择。
    void open(ecs::Entity target = ecs::Entity{});
    void close();
    bool isOpen() const { return m_open; }
    void draw(bool& keepOpen);

    // 自动化：立刻跑一遍注入脚本（`MYVK_EDITOR_NLA_SCRIPT`）。
    void runInjectedScriptNow() { runInjectedScript(); }

    // 当前目标（自动化断言用）。无效 = 还没解析出来。
    ecs::Entity target() const { return m_target; }

  private:
    // ---- 目标 ----
    EditorContext& m_ctx;
    bool m_open = false;
    ecs::Entity m_target{};
    bool m_followSelection = true;   // 目标从"当前选择"来
    bool m_injected = false;
    std::string m_uiError;           // 最近一次被拒的操作（人话）

    // 解析目标：`m_followSelection` 时取 `ctx.selection()`。
    // 每次 draw 都调（选择随时会变），代价只是一次 world 查找。
    void resolveTarget();
    // 目标实体的动画器。nullptr = 没有目标 / 目标没挂 AnimatorComponent
    ecs::AnimatorComponent* animator();
    const ecs::AnimatorComponent* animatorC() const;

    // ---- 选择 ----
    int m_selTrack = -1;
    int m_selStrip = -1;
    bool haveSelection() const;

    // ---- 拖动状态 ----
    //
    // `m_dragMode` 在鼠标按下的那一帧定下来，之后整段拖动都用它（约束 ②）。
    enum class Drag : int {
        None = 0,
        Move = 1,      // 拖体：整条平移
        EdgeL = 2,     // 拖左边缘：改 start
        EdgeR = 3,     // 拖右边缘：改 end
        Scrub = 4,     // 拖标尺：移播放头
    };
    Drag m_drag = Drag::None;
    float m_dragGrabTime = 0.0f;   // 按下时鼠标在时间轴上的时刻
    float m_dragStart0 = 0.0f;     // 按下时 strip 的 start
    float m_dragEnd0 = 0.0f;       // 按下时 strip 的 end

    // ---- 视图 ----
    float m_pxPerSec = 120.0f;
    float m_scrollSec = 0.0f;      // 时间轴左端对应第几秒
    bool m_inspectorOpen = true;

    // ---- 画 ----
    void drawHeader();
    void drawToolbar();
    void drawBody();          // 左轨名 + 右时间轴
    void drawInspector();
    void drawRuler(float x0, float w, float y);
    void drawTrackRow(int ti, float x0, float w, float y, float rowH);
    void drawPlayhead(float x0, float h);
    void drawEmptyHint();

    // ---- 编辑动作（幂等；被拒时写 `m_uiError` 并打 `NLA-REJECT`）----
    void addTrack();
    void addStrip(int track);
    void deleteSelected();
    void duplicateSelected();
    void moveStrip(int ti, int si, float start, float end, const char* why);
    void clampTimeScroll();
    float duration() const;

    // ---- 注入脚本 ----
    void runInjectedScript();
    // 单条命令。返回空串 = 成功，否则是失败原因（`NLA-SCRIPT` 里逐条打）。
    std::string runCommand(const std::string& cmd, int* applied);

    // ---- 日志 ----
    std::string digestText() const;
    void logState(const char* anchor) const;
};

} // namespace editor
