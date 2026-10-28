#pragma once
// ============================================================
// editor/DopeSheetWindow —— 动画摄影表（Dope Sheet，2026-10-28 / R8）
//
// "只看哪一帧有没有键"的那个视角。曲线编辑器回答"值怎么变"，摄影表回答
// "**节奏**对不对"：一屏几十行、每行一串菱形，哪一下是转折、哪一段是匀速，
// 看形状就知道 —— 曲线视图反而看不出来（曲线叠在一起像一团毛线）。
//
//   ┌ Dope sheet — walk_edit *───────────────────────────────────────────┐
//   │ [anim/walk_edit.anim.json] [Reload] [Save] [Fit]  head [0.42] s     │
//   ├──────────────┬─────────────────────────────────────────────────────┤
//   │ ▼ hips       │ ◆      ◆        ◆                                   │
//   │   ▶ rot      │   ◇   ◆                                             │
//   │   ▼ loc      │ ◆        ◆                                          │
//   │     ◆ ◆ ◆    │                                                     │
//   │ ▶ spine      │ ◆  ◆                                                │
//   ├──────────────┴─────────────────────────────────────────────────────┤
//   │ sel 5  t=[0.10,0.60]  type [extreme ▾]  [Mirror] [Align] [Delete]   │
//   └────────────────────────────────────────────────────────────────────┘
//
// ------------------------------------------------------------
// 六条"省了就错"的约束
// ------------------------------------------------------------
// ⚠⚠ ① **一棵树 + 一张网格，必须是一块画布**。左栏通道树与右栏时间网格
//     共用同一个纵向滚动与行高 —— 做成两个 `BeginChild` 就会各自滚动，
//     滚一下通道名和菱形就错行，而"错行"在摄影表上表现为"点到了隔壁骨"，
//     看起来像选择坏了。所以整块 body 是一个 `InvisibleButton` 自绘区，
//     连折叠三角都是自己算矩形 + 命中判定。
//
// ⚠⚠ ② **改数据只有 `act*` 一族**。鼠标手势、快捷键、注入脚本三路全部
//     汇到那十几个函数上（同 TimelineEditorWindow / CurveEditorPanel 的
//     纪律）。否则"能不能自动验"会取决于某颗按钮的坐标算得准不准。
//
// ⚠⚠ ③ **选中集是 (轨道下标, 帧下标)，任何重排之后都不许自己算**。
//     `moveKeys` / `mirrorKeys` / `alignCycle` 都把 `sel` 当**出入参**改写
//     （见 ClipEdit.h 的注释）。摄影表这边只负责把行上的菱形翻译成 KeyRef。
//
// ⚠⚠ ④ **关键帧类型不进求值**。它在 `EditTrack::types` 里躺着，只影响
//     菱形怎么画。`verify_dope_sheet.py` 的 D 组拿"改类型前后姿势摘要逐位
//     相同"钉这一条 —— 有人把类型塞进插值分支时那条立刻红。
//
// ⚠⚠ ⑤ **可绑定宿主**。Pose Mode 打开时 `CharacterWindow` 每帧把
//     `&m_edit` 绑进来（连同骨架、播放头、脏标记），这时编辑的是**内存里
//     那份**，不是"存盘再读回来"。没绑定的时候才自己读 `.anim.json`。
//     ⚠ 绑定期间不许自己存盘：一个文件两个写者，后存的静默盖掉先存的
//        （CurveEditor 的 ⚠⑥ 是同一类问题的另一面）。界面上 Save 直接变灰。
//
// ⚠⚠ ⑥ **缩放必须绕鼠标锚点**。`m_t0/m_t1` 是可见时间窗；滚轮缩放时锚点
//     处的时刻保持不动。绕中点缩放的症状是"我盯着看的那一帧越缩越跑" ——
//     而横向找帧本来就已经很难了。
//
// 日志前缀 `DOPE-`：与 `CURVED-` / `TLED-` / `SEQ-` / `ANIM-` / `POSE-`
// 都分开。摄影表可能和曲线编辑器同时开着同一份资产。
// ============================================================

#include "animation/ClipEdit.h"

#include <glm/glm.hpp>
#include <imgui.h>

#include <filesystem>
#include <string>
#include <vector>

namespace assets {
struct Skeleton;
} // namespace assets

namespace editor {

class EditorContext;

class DopeSheetWindow {
  public:
    explicit DopeSheetWindow(EditorContext& ctx);

    // ------------------------------------------------------------
    // 宿主绑定（Pose Mode）
    // ------------------------------------------------------------
    // 指针全部由 CharacterWindow 持有；本窗口只读不拥有。解绑要在宿主
    // 销毁之前做 —— `EditorApp` 里两个窗口的销毁次序就是按这条排的。
    struct Host {
        animation::EditClip* clip = nullptr;
        const assets::Skeleton* skel = nullptr;
        float* head = nullptr;      // 播放头（宿主的当前时间）
        bool* dirty = nullptr;      // 编辑数据变了 → 宿主重新解析轨道
        const char* label = "";     // 标题里显示的来源
    };
    void bind(const Host& h);
    void unbind();
    bool bound() const { return m_host.clip != nullptr; }

    // 独立模式：读一份 `.anim.json`。空路径 = 空手开窗（只剩路径框）。
    bool open(const std::string& relPath);
    void close();
    bool isOpen() const { return m_open; }
    bool dirty() const { return m_dirty; }

    void draw(bool& open);

  private:
    // ---------------------------------------------------------- 数据
    animation::EditClip m_own;      // 独立模式下的资产
    Host m_host;
    std::string m_path;
    std::string m_error;
    std::string m_uiError;
    bool m_open = false;
    bool m_loaded = false;
    bool m_dirty = false;
    bool m_injected = false;
    char m_pathBuf[512] = {};
    // 绑定期间"脏"写给宿主，独立期间写给自己。
    void markDirty();
    animation::EditClip& clip();
    const animation::EditClip& clip() const;
    const assets::Skeleton* skel() const;

    float head() const;
    void setHead(float t);

    // ---------------------------------------------------------- 行
    //
    // 一行 = 一根骨（摘要）或者一根骨的一个通道。**每次 draw 重建**：
    // 行集合跟着"哪根骨有关键帧"变，缓存起来就要处理三种失效（打帧 /
    // 删帧 / 换骨架），而重建只有几十次字符串比较。
    struct Row {
        std::string joint;
        int path = -1;      // -1 = 骨摘要行
        int track = -1;     // -1 = 摘要行（该骨所有轨道在这一时刻的并集）
        int depth = 0;
    };
    std::vector<Row> m_rows;
    // 折叠状态按**骨名**记（不按下标）—— 与 ClipEdit 的约定 ① 同一个理由：
    // 下标是导入顺序的产物，重导一次全错位。
    std::vector<std::string> m_collapsed;
    bool isCollapsed(const std::string& joint) const;
    void toggleCollapsed(const std::string& joint);

    // 一帧菱形在屏幕上的位置。`refs` 是它代表的 (轨道,帧) 集合 ——
    // 摘要行的一颗菱形可能对应三根通道的同刻帧。
    struct Hit {
        int row = -1;
        float t = 0.0f;
        ImVec2 pos{0.0f, 0.0f};
        std::vector<animation::KeyRef> refs;
        animation::KeyType type = animation::KeyType::Key;
        bool sel = false;
    };
    std::vector<Hit> m_hits;       // 本帧全部菱形（升序：行 → 时间）

    void rebuildRows();
    // 把当前数据铺成 m_hits。**必须在处理输入之前调用** —— 命中判定读它。
    void layoutKeys(const ImVec2& plotMin, const ImVec2& plotMax, float pxPerSec,
                    float t0, float scrollY, float rowH);

    // ---------------------------------------------------------- 视图
    float m_t0 = 0.0f;
    float m_t1 = 1.0f;
    float m_scrollY = 0.0f;        // 纵向滚动（像素）
    float m_rowH = 18.0f;
    float m_treeW = 190.0f;
    std::string m_viewKey;         // DOPE-VIEW 去抖
    std::string m_rowKey;          // DOPE-ROWS 去抖
    std::string m_sumKey;          // DOPE-SUMMARY 去抖（行数/帧数/选中数）

    float durationOf() const;
    void fitView();
    void zoomTime(float anchorPx, float plotX0, float plotX1, float factor);
    // 绕"播放头在窗口里的相对位置"缩放的公共实现（脚本的 `zoom` 走这条）
    void zoomAtFraction(float factor, float frac);
    void panTime(float seconds);
    float pxOfT(const ImVec2& plotMin, const ImVec2& plotMax, float t) const;
    float tOfPx(const ImVec2& plotMin, const ImVec2& plotMax, float px) const;
    float pxPerSec(const ImVec2& plotMin, const ImVec2& plotMax) const;

    // 本帧画布几何（注入脚本的 `dumpkeys` 要用它算 px；body 里每帧写）
    // ⚠ 三个值必须**成套**取：`m_lastPps` 是 `m_lastT0` 那一刻的换算率。
    //   脚本里的 `zoom` / `pan` 会立刻改 `m_t0`，但本帧的菱形还画在旧换算率
    //   上 —— 只读 `m_t0` 配旧 pps 会让 `dumpkeys` 报一个既不是旧位置也不是
    //   新位置的 px。带上 `m_lastT0` 之后"这一行 px 属于哪一帧的视图"自解释。
    ImVec2 m_lastPlotMin{0.0f, 0.0f};
    ImVec2 m_lastPlotMax{0.0f, 0.0f};
    float m_lastPps = 1.0f;
    float m_lastT0 = 0.0f;
    // 独立模式下的播放头（绑定模式用宿主的那个）
    float m_headLocal = 0.0f;

    // ---------------------------------------------------------- 选择
    std::vector<animation::KeyRef> m_sel;
    bool isSelected(const animation::KeyRef& r) const;
    void setSelection(std::vector<animation::KeyRef> s);
    // 这个时刻有哪些轨道上的帧（摘要行的一颗菱形要能展开成多轨道）
    void refsAt(const Row& row, float time,
                std::vector<animation::KeyRef>& out) const;

    // ---------------------------------------------------------- 手势
    enum class Drag : int { None = 0, Keys = 1, Marquee = 2, Pan = 3, Head = 4 };
    struct DragState {
        Drag kind = Drag::None;
        ImVec2 start{0.0f, 0.0f};
        float startScroll = 0.0f;
        float startT0 = 0.0f;
        float startSpan = 1.0f;
        bool moved = false;
    };
    DragState m_drag;
    // `G` 抓取模式：不按键也跟随鼠标，左键确认 / Esc 取消。
    // 与"按住某颗菱形拖"是两条路，但都汇到 `actMoveSelected(dt)`。
    bool m_grab = false;
    ImVec2 m_grabStart{0.0f, 0.0f};
    float m_grabApplied = 0.0f;    // 已经写进数据的位移（增量累加，见 .cpp）
    std::string m_grabTag;

    // 拖动 / G 开始时的一份整份快照：Esc 取消要**精确**回到原样。
    // 用"再挪回 −dt"反算是错的 —— float 上 `(t+d)−d` 不保证等于 `t`
    // （指数变化时舍入），长拖下来原节奏会一点点漂掉，而且看不出是取消造成的。
    animation::EditClip m_snap;
    bool m_haveSnap = false;
    std::vector<animation::KeyRef> m_snapSel;   // 取消时把选择也一并还原
    void beginSnapshot();
    void restoreSnapshot();

    // ---------------------------------------------------------- 动作
    // 全部返回"有没有真的改到东西"。
    bool actSelectAll();
    bool actClearSelection();
    bool actSelectKey(const Hit& h, bool additive);
    bool actSelectRect(const ImVec2& plotMin, const ImVec2& plotMax,
                       ImVec2 a, ImVec2 b, bool additive);
    bool actSelectAtRow(int row, bool wholeRow);
    bool actMoveSelected(float dt);
    bool actMirrorSelected(float pivot);
    bool actAlignSelected(float target);
    bool actSetSelectedType(animation::KeyType t);
    bool actDeleteSelected();
    bool actSetHeadPx(const ImVec2& plotMin, const ImVec2& plotMax, float px);
    bool actZoom(float factor);      // 绕播放头（脚本用）
    bool actPan(float seconds);

    // ---------------------------------------------------------- 存盘
    bool save();
    void reload();
    void syncToHost();               // 编辑完通知宿主重新解析

    // ---------------------------------------------------------- 绘制
    void drawToolbar();
    void drawBody();
    void drawFooter();
    void drawKeyDiamond(ImDrawList* dl, const ImVec2& p, animation::KeyType t,
                        bool sel, bool hot) const;

    // ---------------------------------------------------------- 自动化
    // `MYVK_EDITOR_DOPE_SCRIPT="selall;move:0.25;save"`
    void runInjectedScript();

    EditorContext& m_ctx;
};

} // namespace editor
