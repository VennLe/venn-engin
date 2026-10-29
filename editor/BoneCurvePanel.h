#pragma once
// ============================================================
// editor/BoneCurvePanel —— 骨骼曲线编辑器（S-D，2026-10-29）
//
//   ┌ Bone curves — walk *──────────────────────────────────────────────┐
//   │ [assets/.../walk.anim.json] [Reload] [Save] [Fit] cursor [0.42] s │
//   ├────────────────────┬──────────────────────────────────────────────┤
//   │ upperarm.L         │            ╭──●                              │
//   │   rotation.x       │      ●────╯     ╲                            │
//   │   rotation.y  ●    │  ●                ●──────●                   │
//   │   rotation.z       │ 0s      0.4s      0.8s      1.2s             │
//   │   rotation.w       │                                              │
//   ├────────────────────┴──────────────────────────────────────────────┤
//   │ sel #2  t [0.800]  v [-0.120]  interp [bezier ▾]  ease [in-out ▾] │
//   │ shape [sine ▾]  handle [auto_clamped ▾]  out [0.15 / 0.00]        │
//   │ ── check ── roundtrip OK | canvas ≡ runtime (worst 0.0)           │
//   └───────────────────────────────────────────────────────────────────┘
//
// ------------------------------------------------------------
// 为什么不并进 `CurveEditorPanel`（那个画的是过场相机）
// ------------------------------------------------------------
// 两边**看起来**都是"一条曲线 + 几个关键帧"，但手柄的归属根本不同：
//
//   · 相机 `.seq.json`：一个段只有**一组归一化手柄**（x 是段内时间比例、
//     y 是进度比例），position / yaw / pitch / fov 四个通道**共用**。
//     所以点开"position.y"看到的手柄位置和"yaw"里不一样，那是同一个 w
//     落在不同纵轴上的样子（见 CurveEditorPanel.h 的 ⚠⚠②）。
//
//   · 骨骼 `EditTrack`：每个键存**自己的绝对偏移**（`KeyEase::outHandle`
//     是 (dt, dv)，dt 单位秒、dv 单位值）。一个 glTF channel 的四个分量
//     共用**关键帧**，但线性段上各分量各算各的切线。
//
// 把这两种语义塞进一个类、靠 `if (m_source == ...)` 分支隔离，代价是
// **每一次改动都要重新回答"这个手柄到底归谁"** —— 而答错的症状是"拖了
// 手柄，另一条通道跟着变"，画面上完全看不出是哪一步错的。
// 所以宁可分成两个类，各自把自己的语义写死。
//
// 两边共用的只有 `animation::Curve`（求值器）与那几个 UI 习惯（拖动中
// 不重排、画布不另写求值、注入脚本与鼠标手势共用 `act*`）。画布代码
// 是重复的（约 150 行），但那是**两种不同东西**的画布。
//
// ------------------------------------------------------------
// 五条"省了就错"的约束
// ------------------------------------------------------------
// ⚠⚠ ① **画布不另写求值**。折线是 `animation::evaluateCurve` 按像素列采样
//     出来的，而那条 `Curve` 是 `bakeTrackCurve()` 从轨道投影出来的。
//     投影对不对不靠嘴说：`crossCheck()` 拿 65 个采样点把画布值与
//     **运行时采样**（`sampleEditTrack`）逐个比，最差值打进
//     `BONEC-CHECK xcheck=`。
//
// ⚠⚠ ② **【改形状，不改帧数】**。这里不增删关键帧（`applyCurveToTrack`
//     会在 `keys.size() != keyCount()` 时整条拒绝）。给 rotation.x 插一帧，
//     另外三个分量就被迫凭空得到一个"打帧时刻" —— 那是 Dope Sheet 的活
//     （R8 已经在 `EditClip::setKey` 上处理了）。曲线编辑器的职责是
//     **把这一下动作的节奏捏出来**。
//
// ⚠⚠ ③ **拖动中不重排**。`m_curve.keys` 在拖动中可以暂时乱序，松手时
//     `sortAndRemap()` 一次性收拾（同 CurveEditorPanel 的 ③）。拖动中排序
//     会让鼠标底下那一帧跳到数组别处，`m_sel` 当场失准。
//
// ⚠⚠ ④ **V 存类型，不换算数值**。这与相机面板**正好相反**（那边 V 是
//     "立刻按规则把数值算出来写回去，写回之后它就是 Free"），因为骨骼这边
//     每个键**存得下**自己的 `HandleType`。所以按 V 之后：AutoClamped 会
//     一直跟着邻居重算 —— 这才是 Blender 的手感。
//
// ⚠⚠ ⑤ **旋转的 Linear 段运行时走 slerp，画布上没有那条线**。四个分量
//     各自的线性插值再归一（nlerp）与 slerp 同弧不同速，所以在这一段里
//     "画布值"与"运行时值"**本来就该不一样**（正中点相同、其余差几个百分点）。
//     `crossCheck()` 因此**跳过**这些样本并计数（`skipped`），侧栏那句
//     "画布 ≡ 运行时"只在真正走曲线的那部分成立。这不是遮掩 —— 把差值
//     装作 0 才是。见 CurveBake.h 约定 ②。
//
// 日志前缀 `BONEC-`（Bone Curve），与 `CURVED-` / `DOPED-` / `ANIM-` 分开。
// ============================================================

#include "animation/ClipEdit.h"
#include "animation/Curve.h"
#include "animation/CurveBake.h"

#include <glm/glm.hpp>
#include <imgui.h>

#include <filesystem>
#include <string>
#include <vector>

namespace editor {

class EditorContext;

class BoneCurvePanel {
  public:
    explicit BoneCurvePanel(EditorContext& ctx);

    // 打开并读一份 `.anim.json`。`relPath` 为空 = 空手开窗（只剩路径框）。
    // ⚠ 读失败也**开窗**：把错误摆出来比"点了没反应"有用。
    bool open(const std::string& relPath);
    void close();
    bool isOpen() const { return m_open; }
    bool dirty() const { return m_dirty; }
    void draw(bool& open);

    // 自动化用：不经过鼠标，直接跑一遍注入脚本（在 draw 里也会跑）。
    // 命令见 .cpp 的 runInjectedScript()。
    void runInjectedScriptNow() { runInjectedScript(); }

  private:
    // ---------------------------------------------------------- 数据
    animation::EditClip m_clip;
    std::string m_path;
    std::string m_error;     // 读盘 / 存盘错误（致命）
    std::string m_uiError;   // 最近一次被拒的操作（人话）
    bool m_open = false;
    bool m_loaded = false;
    bool m_dirty = false;
    bool m_readOnly = false;
    bool m_injected = false;
    char m_pathBuf[512] = {};

    // ⑥ 冲突检测：打开 / 上次存盘那一刻的磁盘时间戳。
    bool m_haveStamp = false;
    std::filesystem::file_time_type m_stampDisk{};
    bool refreshStamp();

    // ---------------------------------------------------------- 通道
    std::vector<animation::BoneChannel> m_chans;
    int m_chan = 0;
    std::string m_chanStamp;                 // 通道表指纹：变了才重建
    void rebuildChannelList();               // 幂等

    bool haveChannel() const;
    animation::BoneChannel channel() const;
    const animation::EditTrack* trackC() const;
    animation::EditTrack* trackM();

    // 当前通道的**投影**（②）。空通道 → 空曲线。
    animation::Curve m_curve;
    std::string m_curveStamp;                // 投影指纹：变了才重建
    void rebuildCurve();                     // 幂等
    std::string curveStampOf() const;
    float valueAt(float t) const { return animation::evaluateCurve(m_curve, t); }

    float keyValue(int i) const;             // 越界 → 0
    // 改一个关键帧的 (t, v) 到**投影**里（不落盘；由 applyToTrack 统一写回）
    bool setProjectedKey(int i, float t, float v, bool reorder);

    // 投影 → 轨道（②：不增删帧）。每次改完都立刻写回，`m_clip` 永远是最新的
    // —— 这样"存盘"与"视口里看到的样子"不可能分叉。
    bool applyToTrack(const char* why);

    float durationOf() const;

    // ---------------------------------------------------------- 选择 / 手势
    std::vector<int> m_sel;                  // 升序、无重复
    int m_primary = -1;
    int m_htCycle = 0;
    void clampSel();
    bool isSelected(int i) const;

    enum class DragKind : int {
        None = 0, Key = 1, HandleOut = 2, HandleIn = 3, Marquee = 4
    };
    struct DragState {
        DragKind kind = DragKind::None;
        int index = -1;                      // key / handle 所属的关键帧下标
        ImVec2 startMouse{0.0f, 0.0f};
        glm::vec2 accum{0.0f, 0.0f};         // 手柄：累计像素位移
        glm::vec2 handleOrig{0.0f, 0.0f};    // 手柄：按下那一刻的绝对偏移
        bool moved = false;
        bool timeMoved = false;
        std::vector<int> moving;
        std::vector<float> origT;
        std::vector<float> origV;
    };
    DragState m_drag;

    // ---------------------------------------------------------- 视图
    float m_t0 = 0.0f, m_t1 = 1.0f;
    float m_v0 = 0.0f, m_v1 = 1.0f;
    float m_head = 0.0f;
    std::string m_mapKey;
    std::string m_keyLogKey;

    struct Plot {
        ImVec2 rectMin{0.0f, 0.0f};
        ImVec2 rectMax{0.0f, 0.0f};
        ImVec2 min{0.0f, 0.0f};
        ImVec2 max{0.0f, 0.0f};
        float t0 = 0.0f, t1 = 1.0f, v0 = 0.0f, v1 = 1.0f;
    };
    Plot m_plot;

    float pxOfT(const Plot& p, float t) const;
    float pyOfV(const Plot& p, float v) const;
    float tOfPx(const Plot& p, float x) const;
    float vOfPy(const Plot& p, float y) const;
    float pxPerSec(const Plot& p) const;
    float pxPerUnit(const Plot& p) const;
    void resetView();
    void fitValueRange();

    // ---------------------------------------------------------- 动作
    bool actSelect(int i, bool additive);
    bool actSelectAll();
    bool actSelectRect(const Plot& p, ImVec2 a, ImVec2 b, bool additive);
    bool actSetKey(int i, float t, float v, bool reorder);
    bool actDragKeysPx(float dx, float dy);
    bool actSetHandle(int i, bool isOut, glm::vec2 absOffset);
    bool actDragHandlePx(float dx, float dy);
    // T：constant → linear → bezier → constant
    bool actSetInterp(int i, animation::Interp ip);
    bool actCycleInterp();
    bool actSetEase(int i, animation::EaseMode m, animation::EaseShape s);
    bool actSetHandleType(int i, animation::HandleType t);   // ④：存类型
    bool actCycleHandleType();
    bool actSetChannel(int index);
    bool actSetHead(float t);

    void sortAndRemap();
    void beginDrag(const Plot& p, ImVec2 mouse, bool additive);
    void endDrag();

    bool save(bool force);
    void reload();

    // 校验结论（存盘往返 + 画布↔运行时）。`m_checkXCheck` 就是 ① 的那个
    // 最大偏差，`m_checkSkipped` 是 ⑤ 跳过的样本数。
    bool m_checkValid = false;
    std::string m_checkError;
    int m_checkWarnings = 0;
    float m_checkXCheck = 0.0f;
    int m_checkSkipped = 0;
    std::string m_checkLogKey;
    void revalidate();

    struct XCheck {
        float worst = 0.0f;
        const char* where = "";
        int samples = 0;
        int skipped = 0;
    };
    XCheck crossCheck() const;

    bool reject(const char* op, const std::string& reason);
    void markDirty() { m_dirty = true; }
    // "这条轨道的这一段，运行时是不是走 slerp"（约定 ⑤）
    bool segmentUsesSlerp(const animation::EditTrack& tr, float t) const;

    // ---------------------------------------------------------- 绘制
    void drawToolbar();
    void drawBody();
    void drawChannelList();
    void drawCanvas();
    void drawInspector();
    void drawValidation();

    void runInjectedScript();

    EditorContext& m_ctx;
};

} // namespace editor
