#pragma once
// ============================================================
// editor/TimelineEditorWindow —— 时间轴轨道编辑器（C1 的最后一块）
//
// #307/#309 之前，`sequence` 页签只有 4 个控件（路径 / 时间滑条 /
// Preview / 只读事件树）。**运镜的正文仍然要手写 `.seq.json`** ——
// 想让镜头从 A 飞到 B，作者得自己算 yaw 的弧度、手写 5 个浮点数。
// 对二次元游戏来说，"过场演出"是内容主体之一，这个成本直接压在产能上。
//
//   ┌ Timeline editor — opening *───────────────────────────────────────────┐
//   │ [sequences/opening.seq.json] [Reload] [Save] [New]                    │
//   │ dur [8.00]s  loop[ ]  head [1.234]s [Capture][+Key][+Event][Del]       │
//   ├────────┬──────────────────────────────────────────────────────────────┤
//   │ Camera │ ◆────────◆──────◆────────────────◆                          │
//   │ Events │    ●      ●            ●                                     │
//   ├────────┴──────────────────────────────────────────────────────────────┤
//   │ t [1.234]  pos [5.5][-1.2][1.90]  yaw [11.5°] pitch [-6.9°] fov [45]  │
//   │ ease [smooth ▾]                                                       │
//   │ ── check ──  Load check: OK  |  4 keys, 3 events                      │
//   └───────────────────────────────────────────────────────────────────────┘
//
// ------------------------------------------------------------
// 六条"省了就错"的设计约束
// ------------------------------------------------------------
// ⚠⚠ ① **拖动关键帧时松手才重排，拖动中不排。**
//     `cameraKeys` 必须按 t 升序（见 `SequenceAsset.h` ②），而拖动改变的
//     正是 t。但拖动**过程中**就排序的话，鼠标下面的那一帧会在数组里跳到
//     别的位置，`m_sel` 失准 —— 表现是"拖到一半，选中项换成了别人"。
//     所以拖动中允许暂时乱序（连线交叉，正好直观），松手时做一次
//     "取出-插入"，并把 `m_sel` 跟着挪。
//
// ⚠⚠ ② **UI 用度，资产用弧度。** `yaw` / `pitch` 在 `.seq.json` 里是弧度
//     （与场景 JSON 的 `camera.yaw` 同一口径）。作者不会用弧度思考 ——
//     180° 写成 3.1416 没人算得出来。所以面板显示度，转换只在这一层发生。
//     ⚠ 转换写错了的症状极隐蔽：`yaw=0.2` 显示成 11.5° 看着"挺合理"，
//       要等镜头发疯才会发现。
//
// ⚠⚠ ③ **播放头拖动走 `tick(sc, 0.0f)`，不推进时间。**
//     与 Inspector 的时间滑条同一条纪律（`SequencerSystem.h` 的"编辑态"）：
//     dt=0 只按当前播放头采样姿态，不推进、不发事件。所以拖播放头是
//     "看"而不是"播"。真正的预览是进 Play。
//
// ⚠⚠ ④ **校验不在面板里实现**：每次改动都是"`toJson` → 丢回
//     `parseSequence`"。与剧本编辑器同一条理由 —— 两套规则必然分叉，
//     而分叉的症状是"编辑器说没问题，进 Play 镜头不动"。
//
// ⚠⚠ ⑤ **读盘失败 → 只读。** `parseSequence` 是"边解析边填"，半路出错时
//     `cameraKeys` 里是前几帧的残片。此时按 Save 会把 40 帧的过场截成 3 帧。
//
// ⚠⚠ ⑥ **存盘后刷新场景里引用同一份资产的组件。**
//     不刷的话：窗口里是新运镜、Save 成功、进 Play 还是旧的
//     （`SequencerComponent::assetLoaded` 是"读过一次就别再读"，
//     `SequencerSystem::tick` 老实地用缓存）。与剧本编辑器同构。
//
// ------------------------------------------------------------
// 为什么这一版**不做**"在视口里拖相机的关键帧路径"
// ------------------------------------------------------------
// 那是 UE 的 Matinee / Sequencer 的做法（在 3D 视口里画轨迹线、直接拖点）。
// 它需要一条"视口 gizmo → 时间轴"的反向通道，以及一条世界空间的样条绘制。
// 在**编辑器视口相机是自由相机**（不经过 `CameraSystem`）的前提下，
// 这条通道要穿过 EditorScene / ViewportPanel / Gizmo 三层。
// 先做"在轨道上排版 + 从视口抓当前机位"，就已经覆盖了 90% 的实际操作：
// 摆好机位 → 按 Capture → 时间轴上多一个点。
// ============================================================

#include "sequence/SequenceAsset.h"

#include <string>
#include <vector>

namespace editor {

class EditorContext;

class TimelineEditorWindow {
  public:
    explicit TimelineEditorWindow(EditorContext& ctx);

    // 打开并读一份资产。`relPath` 为空 = 空手开窗（只剩 "New"）。
    // ⚠ 读失败也**开窗**：把错误摆出来，比"点了没反应"有用。
    bool open(const std::string& relPath);
    void draw(bool& open);

    bool isOpen() const { return m_open; }
    void close();
    bool dirty() const { return m_dirty; }

    // 轨道种类。`Count` 只做数组大小，不是合法值。
    enum class Track : int { Camera = 0, Events = 1, Count = 2 };

  private:
    // ---------------------------------------------------------- 数据
    sequence::SequenceAsset m_asset;
    std::string m_path;
    std::string m_error;
    std::string m_uiError;
    bool m_open = false;
    bool m_loaded = false;
    bool m_dirty = false;
    bool m_readOnly = false;
    bool m_injected = false;

    // ---------------------------------------------------------- 校验
    bool m_checkValid = false;
    std::string m_checkError;
    std::vector<std::string> m_checkWarnings;
    std::string m_checkLogKey;
    void revalidate();

    // ---------------------------------------------------------- 选择 / 播放头
    Track m_track = Track::Camera;
    int m_sel = 0;              // 在当前轨道里的下标
    float m_head = 0.0f;        // 播放头（秒）

    // 轨道区的手势状态。⚠ 必须是**成员**而不是函数内 static：
    //   关窗再开（换一份资产）时残留的拖拽状态会让下一次点击"接着上次拖"。
    //   0=无 1=拖相机帧 2=拖事件 3=拖播放头
    int m_dragMode = 0;

    int trackCount(Track t) const;
    void clampSel();
    sequence::SeqCameraKey* selKey();
    sequence::SeqEvent* selEvent();

    // ---------------------------------------------------------- 编辑缓冲
    //
    // ⚠ 与剧本编辑器同一条纪律：不让 `InputText` 直接写资产里的
    //   `std::string`。选中项一变（或删了一帧）指针立刻悬垂，而 ImGui 的
    //   activeId 还记着那个控件。缓冲按"哪个轨道 + 第几项 + 该项的 t 值"
    //   失效重建 —— 只按下标不够（拖过之后 t 变了还是同一项）。
    std::string m_bufKey;
    char m_kindBuf[64] = {};
    char m_payloadBuf[512] = {};
    char m_pathBuf[512] = {};
    void syncBuffers();

    // ---------------------------------------------------------- 动作
    // UI 与注入脚本**共用**这一份（同剧本编辑器）。
    bool actAddKey();                       // 在播放头处插一帧（取当前采样姿态）
    bool actCaptureKey();                   // 把视口相机当前姿态插到播放头
    bool actAddEvent(const std::string& kind);
    bool actDelete();
    bool actSetTime(float t);               // 改选中项的 t（会触发重排）
    bool actSetPos(glm::vec3 p);
    bool actSetYawDeg(float deg);
    bool actSetPitchDeg(float deg);
    bool actSetFov(float fov);
    bool actSetEase(const std::string& name);
    bool actSetEventKind(const std::string& kind);
    bool actSetEventPayload(const std::string& payload);
    bool actSetDuration(float d);
    bool actSetHead(float t);
    bool actSetLoop(bool loop);
    bool actNewSequence();                  // 一帧的骨架，duration=5

    bool save(bool force);
    void reload();
    void refreshSceneComponents();

    // 把当前轨道上第 `from` 项按 t 挪到它该在的位置，返回新下标。
    // ⚠ 只在"改完 t 之后"调用，而且必须把返回值写回 `m_sel`（见 ①）。
    int reorderKey(int from);
    int reorderEvent(int from);

    // 在播放头处采样出来的相机姿态（没有关键帧时取视口相机）
    sequence::SeqCameraKey poseAtHead() const;
    // 视口相机当前姿态（`Capture` 用）
    sequence::SeqCameraKey poseFromViewport() const;

    void markDirty();
    bool reject(const char* op, const std::string& reason);

    // ---------------------------------------------------------- 绘制
    void drawToolbar(bool& open);
    void drawTracks();
    // 轨道区的几何：把"时间 → 像素"这件事只写一次
    struct TrackGeom {
        float x0 = 0.0f;      // t=0 的像素 x
        float x1 = 0.0f;      // t=duration 的像素 x
        float rowY[2] = {};   // 两条轨道的中心 y
        float rowH = 0.0f;
        float dur = 1.0f;
    };
    float timeToX(const TrackGeom& g, float t) const;
    float xToTime(const TrackGeom& g, float x) const;
    void drawInspectorRow();
    void drawValidation();

    // ---------------------------------------------------------- 自动化
    // `MYVK_EDITOR_TLED_SCRIPT="sel:camera:1;settime:2.5;save"`，只跑一次。
    void runInjectedScript();

    EditorContext& m_ctx;
};

} // namespace editor
