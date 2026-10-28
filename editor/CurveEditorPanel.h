#pragma once
// ============================================================
// editor/CurveEditorPanel —— 曲线编辑器（Graph Editor，2026-10-28 / R6）
//
// R5 把"值怎么随时间变"收敛成了 `animation::Curve`，但那份能力当时只有
// 一个入口：时间轴编辑器里 4 个拖条（bezier 手柄）。**看不见曲线就调不出
// 手感** —— 作者要的是"这条线在 2.5 秒附近鼓起来一块"，不是
// "outHandle.y = 0.62"。
//
//   ┌ Curve editor — opening *──────────────────────────────────────────┐
//   │ [sequences/opening.seq.json] [Reload] [Save] [Fit] head [1.23] s   │
//   ├───────────┬───────────────────────────────────────────────────────┤
//   │ position.x│ ··············╭────●                                 │
//   │ position.y│        ●─────╯      ╲                                 │
//   │ position.z│   ●                    ●──────●                       │
//   │ yaw  ●    │   ┃（关键帧范围之外是外推段，虚线）                    │
//   │ pitch ★   │ 0s        2.5s       4s        6s        8s           │
//   ├───────────┴───────────────────────────────────────────────────────┤
//   │ sel #2  t [2.500]  value [3.000]  interp [smooth ▾]  handle [free]│
//   │ ── check ── roundtrip OK | 画布 ≡ 运行时（最差 2.4e-07）           │
//   └───────────────────────────────────────────────────────────────────┘
//
// ------------------------------------------------------------
// 六条"省了就错"的约束
// ------------------------------------------------------------
// ⚠⚠ ① **画布不另写求值**。折线是 `evaluateCurve` 按像素列采样出来的，
//     那条 `Curve` 是**从过场相机关键帧投影出来的**（见 `buildCurve`）。
//     投影对不对不靠嘴说：`crossCheck()` 在打开/重读时拿 6 个通道 × 65 个
//     采样点和 `sampleCamera` 逐个比，最差值打进 `CURVED-CHECK xcheck=`。
//     两套求值一旦分叉，作者看到的就是"画布和进 Play 不一样"。
//
// ⚠⚠ ② **手柄是归一化的，一个段只有一组**。`SeqCameraKey::outHandle` 的
//     x 是"段内时间比例"、y 是"进度比例"，position / yaw / pitch / fov
//     四个通道共用它（见 SequenceAsset.h）。所以：
//       · 画布上画的手柄**位置**是按当前通道换算出来的（x·span、y·dv），
//         换个通道看，手柄会跑到别处 —— 那不是 bug，是同一个 w 落在
//         不同纵轴上的样子；
//       · 拖它 = 改这四通道**一起**的节奏，侧栏必须写清楚；
//       · `dv ≈ 0`（这一段该通道值没变）时**换算不出来**：手柄的 y 无法
//         表达。此时禁止拖 y，并在日志里说明（`CURVED-HANDLE-FLAT`）。
//
// ⚠⚠ ③ **拖动中不重排关键帧**。与时间轴编辑器同一条纪律（见
//     TimelineEditorWindow.h 的 ⚠⚠①）：`cameraKeys` 必须按 t 升序，而拖动
//     改变的正是 t。拖动中排序会让鼠标底下那一帧跳到数组别处，`m_sel`
//     当场失准。松手时做一次"取出-插入"，并把选中项跟着挪。
//
// ⚠⚠ ④ **T 切的是 `sequence::Ease` 四态，不是 `animation::Interp` 三态**。
//     `.seq.json` 里存的是 linear / smooth / step / bezier，其中 `smooth`
//     在曲线核心里是 `Interp::Linear + EaseMode::InOut`（逐位等于老的
//     smoothstep，见 Curve.cpp 的迁移锚），`step` 是 `Interp::Constant`。
//     少一态的表现是"按一下 T，smooth 那一段直接变直线"。
//
// ⚠⚠ ⑤ **V 只换算、不存类型**。Blender 记得住"这个键是 Auto"，因为它
//     每个通道是独立的 F-Curve；这里一个段只有一组归一化手柄（②），
//     存"类型"就得为四个通道各存一个类型，而它们会互相打架（yaw 说 Auto、
//     位置说 Free，谁算数？）。所以 V 的语义是**立刻按该规则把数值算出来
//     写回去**：写回之后它就是 Free。与 Blender"Auto 手柄不认你拖的值"
//     正好相反 —— 这里"拖过的手柄不认 Auto"。侧栏如实标 `free`。
//
// ⚠⚠ ⑥ **两个编辑器会同时开同一份资产**（时间轴 + 曲线）。曲线编辑器自己
//     持有一份副本，存盘前比对"磁盘上的文件在我们打开之后有没有被改过"，
//     改过就拒绝（除非 Save anyway）。没有这一条的话，两个窗口各存一次 =
//     后存的把先存的静默覆盖，而两边都显示"保存成功"。
//
// ------------------------------------------------------------
// 为什么第一版只接"过场相机"，不接实体 TRS
// ------------------------------------------------------------
// `engine/ecs/Components.h` 在 `GLOBAL_GLOBS` 里（动它要跑全量回归，见
// docs/animation_authoring_roadmap.md §5）。而这套画布不依赖数据源是相机
// 还是骨骼 —— `buildCurve()` 是唯一要换的地方。先接相机：数据已经在
// `SeqCameraKey` 上、有 73 条 `SEQ-TEST` 兜底、失败也不影响任何运行时特性。
//
// 日志前缀 `CURVED-`（Curve EDitor），与 `TLED-` / `SEQ-` / `ANIM-` 分开。
// ============================================================

#include "animation/Curve.h"
#include "sequence/SequenceAsset.h"

#include <glm/glm.hpp>
// ⚠ 这里 include imgui 只为一件事：`Plot` 存的矩形与 `DragState` 存的按下点
//   是**客户区像素**（与 `logRect` 打出来的是同一套坐标），用 ImVec2 表达最
//   直接。换成 glm::vec2 也行，但那样每次进出都要转一手，而转换点正是
//   "像素 ≈ 值"那类错误最爱藏的地方。
#include <imgui.h>

#include <filesystem>
#include <string>
#include <vector>

namespace editor {

class EditorContext;

class CurveEditorPanel {
  public:
    explicit CurveEditorPanel(EditorContext& ctx);

    // 打开并读一份资产。`relPath` 为空 = 空手开窗（只剩路径框）。
    // ⚠ 读失败也**开窗**：把错误摆出来，比"点了没反应"有用。
    bool open(const std::string& relPath);
    void close();
    bool isOpen() const { return m_open; }
    bool dirty() const { return m_dirty; }
    void draw(bool& open);

    // 画布上一次只看一个通道。名字就是 `.seq.json` 里的字段名（数据，不翻）。
    enum class Channel : int {
        PosX = 0, PosY = 1, PosZ = 2, Yaw = 3, Pitch = 4, Fov = 5, Count = 6
    };
    static const char* channelId(Channel c);
    // 通道值在**画布纵轴**上的单位：yaw/pitch 画成度（资产里是弧度）。
    static bool channelIsDegrees(Channel c);

  private:
    // ---------------------------------------------------------- 数据
    sequence::SequenceAsset m_asset;
    std::string m_path;
    std::string m_error;      // 读盘 / 存盘错误（致命）
    std::string m_uiError;    // 最近一次被拒的操作（人话）
    bool m_open = false;
    bool m_loaded = false;
    bool m_dirty = false;
    bool m_readOnly = false;
    bool m_injected = false;
    char m_pathBuf[512] = {};

    // ⑥ 冲突检测：打开 / 上次存盘那一刻的磁盘时间戳。
    bool m_haveStamp = false;
    std::filesystem::file_time_type m_stampDisk{};
    bool refreshStamp();                       // 读盘上时间戳，失败 → false

    // ---------------------------------------------------------- 通道与投影
    Channel m_channel = Channel::PosX;

    // 当前通道的**投影**：关键帧 → `animation::Curve`（①）。
    // `m_vals` 是通道值（yaw 已展开成连续角度，见 `.cpp` 的 unwrapYaw）。
    animation::Curve m_curve;
    std::vector<float> m_vals;
    std::string m_curveStamp;                  // 投影指纹：变了才重建
    void rebuildCurve();                       // 幂等
    std::string curveStampOf() const;
    animation::Curve buildCurve(const std::vector<float>& vals) const;

    float valueAt(float t) const { return animation::evaluateCurve(m_curve, t); }

    // 把相机关键帧读成通道值（yaw 累加最短弧 → 连续角度，避免 170°→−170°
    // 在画布上绕一大圈）。`ch` 显式传是为了 `crossCheck()` 能遍历全部通道。
    void readChannelValues(std::vector<float>& out, Channel ch) const;
    float keyValue(int i) const;               // 越界 → 0
    bool writeKeyValue(int i, float v);        // 按通道写回（yaw/pitch/fov 带钳制）

    // ---------------------------------------------------------- 选择 / 手势
    std::vector<int> m_sel;                    // 升序、无重复
    int m_primary = -1;                        // 侧栏显示哪一个
    int m_htCycle = 0;                         // V 的循环计数（见 actCycleHandleType）
    bool isSelected(int i) const;

    enum class DragKind : int {
        None = 0, Key = 1, HandleOut = 2, HandleIn = 3, Marquee = 4
    };
    struct DragState {
        DragKind kind = DragKind::None;
        int index = -1;                        // key / handle 所属的**段左端**下标
        glm::vec2 grab{0.0f, 0.0f};            // 按下点与抓手之间的像素偏移
        ImVec2 startMouse{0.0f, 0.0f};
        glm::vec2 accum{0.0f, 0.0f};           // 手柄：累计像素位移
        glm::vec2 handleOrig{0.0f, 0.0f};      // 手柄：按下那一刻的归一化值
        bool moved = false;
        bool timeMoved = false;                // 动过 t → 松手要重排
        // 多选批量：按下那一刻每个被移动关键帧的 (t, v)。
        // 用"原始值 + 同一个增量"而不是"逐个累加"，相对间距才不会漂。
        std::vector<int> moving;
        std::vector<float> origT;
        std::vector<float> origV;
    };
    DragState m_drag;

    // ---------------------------------------------------------- 视图
    float m_t0 = 0.0f, m_t1 = 1.0f;            // 可见时间范围（秒）
    float m_v0 = 0.0f, m_v1 = 1.0f;            // 可见值范围
    float m_head = 0.0f;                       // 画布上的光标（秒）
    std::string m_mapKey;                      // CURVED-MAP 去抖
    std::string m_keyStamp;                    // CURVED-KEY 去抖

    struct Plot {
        ImVec2 rectMin{0.0f, 0.0f};            // InvisibleButton 的矩形
        ImVec2 rectMax{0.0f, 0.0f};
        ImVec2 min{0.0f, 0.0f};                // 绘图区（去掉坐标轴留白）
        ImVec2 max{0.0f, 0.0f};
        float t0 = 0.0f, t1 = 1.0f, v0 = 0.0f, v1 = 1.0f;
    };
    Plot m_plot;                               // 本帧画布几何（脚本要用）

    float pxOfT(const Plot& p, float t) const;
    float pyOfV(const Plot& p, float v) const;
    float tOfPx(const Plot& p, float x) const;
    float vOfPy(const Plot& p, float y) const;
    float pxPerSec(const Plot& p) const;
    float pxPerUnit(const Plot& p) const;

    float durationOf() const;                  // max(duration, 末帧, 0.1)
    void resetView();                          // 时间轴铺满 + 值轴自适应
    void fitValueRange();                      // 只动纵轴（拖动中不跳时间轴）
    void zoomTime(float anchorPx, float factor);

    // ---------------------------------------------------------- 动作
    //
    // ⚠ 鼠标手势与注入脚本**共用**这一族（同时间轴编辑器的纪律）。
    //   所以"能不能自动验"不取决于把某颗按钮的坐标算对没有。
    bool actSelect(int i, bool additive);
    bool actSelectRect(const Plot& p, ImVec2 a, ImVec2 b, bool additive);
    bool actSelectAll();

    // 改一个关键帧的 (t, v)。`reorder=false` 用于拖动中（③）。
    bool actSetKey(int i, float t, float v, bool reorder);
    // 按**像素增量**移动选中的关键帧 —— 鼠标拖动与脚本 `dragkey` 都走这里，
    // 像素→(t,v) 的换算只有一份。
    bool actDragKeysPx(float dx, float dy);
    bool actSetHandle(int i, bool isOut, glm::vec2 norm);
    bool actDragHandlePx(float dx, float dy);  // 用 m_drag.accum 累计

    bool actSetEase(int i, sequence::Ease e);
    bool actCycleEase();                       // T
    bool actSetHandleType(int i, animation::HandleType t);   // V（换算并写回）
    bool actCycleHandleType();                 // V

    bool actSetHead(float t);
    bool actSetChannel(Channel c);

    // 整表按 t 稳定排序，并把 `m_sel` / `m_primary` 跟着换算（③）。
    // ⚠ 用"全局稳定排序"而不是时间轴编辑器那种"取出-插入"：这里拖动可以
    //   是**多选批量**，逐个取出-插入会把相对身份搞乱；而稳定排序只需要
    //   一张 old→new 的映射表，一次性换算完。
    void sortAndRemap();
    void clampSel();
    void beginDrag(const Plot& p, ImVec2 mouse, bool additive);
    void endDrag();                            // 松手：重排 + 落日志

    bool save(bool force);
    void reload();
    void revalidate();
    void refreshSceneComponents();

    // 校验结论（存盘往返 + 画布↔运行时）。`m_checkXCheck` 就是 ① 的那个
    // 最大偏差，侧栏与 `CURVED-CHECK` 都读它。
    bool m_checkValid = false;
    std::string m_checkError;
    int m_checkWarnings = 0;
    float m_checkXCheck = 0.0f;
    std::string m_checkLogKey;

    void markDirty() { m_dirty = true; }
    bool reject(const char* op, const std::string& reason);

    // ---------------------------------------------------------- 绘制
    void drawToolbar();
    void drawBody();                           // 通道列表 | 画布
    void drawChannelList();
    void drawCanvas();
    void drawInspector();
    void drawValidation();

    // ① 的判据：画布投影 vs `sampleCamera`，返回最大偏差（通道单位）。
    struct XCheck {
        float worst = 0.0f;
        const char* where = "";
        int samples = 0;
    };
    XCheck crossCheck() const;

    // ---------------------------------------------------------- 自动化
    // `MYVK_EDITOR_CURVED_SCRIPT="chan:position.x;sel:1;dragkey:1:40:0;save"`
    // `sleep:<ms>` 是给"磁盘冲突"那条用例的**闸门**（见 .cpp）。
    void runInjectedScript();

    EditorContext& m_ctx;
};

} // namespace editor
