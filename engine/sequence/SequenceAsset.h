#pragma once
// ============================================================
// sequence/SequenceAsset —— 过场时间轴资产（`.seq.json`）的解析与采样
//
// 这是 C1「过场时间轴」的**数据层**：把一份 JSON 变成"一串相机关键帧 +
// 一串事件"，并且能在任意时刻 `t` 上采样出相机姿态、查出该发哪些事件。
// 播放器（`SequencerSystem`）只负责推进 `t`，不负责决策。
//
// ------------------------------------------------------------
// 为什么这里**不** include `scene/CameraSystem.h`
// ------------------------------------------------------------
// `scene::CameraPose` 是这个数据结构最自然的形态，但那样会成环：
//   ecs/Components.h → sequence/SequenceAsset.h → scene/CameraSystem.h
//                    → ecs/Entity.h → （回到 ecs）
// 所以本文件自己定义 `SeqCameraKey`（字段与 `CameraPose` 一一对应），
// 由 `SequencerSystem` 在接管相机时转成 `CameraPose`。多写四行赋值，
// 换来"ecs 可以安全地 include 本文件"，而不是又一个前向声明地雷
// （见 MEMORY 的 C4099 / LNK2019 那一组坑）。
//
// ------------------------------------------------------------
// 三条语义（都是"看起来能省、省了就错"的）
// ------------------------------------------------------------
// ⚠⚠ ① **yaw 插值必须走短弧**。170° → −170° 应该是 +20°，不是 −340°。
//     直接 `a + (b-a)*u` 会让镜头绕着角色原地转一大圈。值用**弧度**，
//     与场景 JSON 的 `camera.yaw` 同一口径（不是度）。
// ⚠⚠ ② **关键帧必须按 t 排序**。插值依赖"相邻两帧"这个关系；JSON 里
//     写乱顺序，线性插值会在时间段之间反复横跳 —— 而画面只是"有点跳"，
//     很难定位。加载时排序，重复的 t 记一条 `duplicate` 警告。
// ⚠⚠ ③ **事件的区间是半开的 `[from, to)`**。与 B2 的 `AnimEvent` 同一条
//     纪律：闭区间会让"停在事件上"每帧发一次；`(from, to]` 则 `t = 0`
//     的事件永不触发。`t == duration` 的事件**永远不发**（时间轴里没有
//     "超过结尾"这一刻），加载时就把这种事件报出来。
// ============================================================

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace sequence {

// 缓动。`Step` = 在**这一帧的时间点**直接切过去（硬切镜头），
// 用于"从远景一下切到特写"，这是过场里最常用的一种转场。
enum class Ease : int {
    Linear = 0,
    Smooth = 1,   // smoothstep（默认）
    Step = 2,     // 保持前一帧，到下一帧瞬间切换
};

const char* easeName(Ease e);
bool parseEase(const std::string& s, Ease& out);

// 一个相机关键帧。字段与 `scene::CameraPose` 一一对应（见文件头）。
struct SeqCameraKey {
    float t = 0.0f;                 // 秒
    glm::vec3 position{0.0f, 0.0f, 3.0f};
    float yaw = 0.0f;               // 弧度
    float pitch = -0.25f;           // 弧度，正 = 相机在上方俯视
    float fov = 45.0f;              // 度
    Ease ease = Ease::Smooth;       // 从**这一帧**到下一帧的插值方式
};

struct SeqEvent {
    float t = 0.0f;
    std::string kind;      // "Footstep" / "Sound" / "Dialogue" / "Flag" / 自定义
    std::string payload;   // 自由文本：音效名 / 台词 id / 旗标名
};

struct SequenceAsset {
    std::string name;        // 诊断用；缺省取文件名
    std::string sourcePath;  // 解析时给的路径（相对或绝对）

    float duration = 0.0f;   // <=0 → 加载时按最晚的关键帧/事件推算
    bool loop = false;

    std::vector<SeqCameraKey> cameraKeys;   // 已按 t 升序
    std::vector<SeqEvent> events;           // 已按 t 升序

    bool valid = false;
    std::string error;                       // valid=false 时的原因
    std::vector<std::string> warnings;       // 不致命但要让人知道

    bool hasCamera() const { return !cameraKeys.empty(); }
    float lastCameraTime() const;
    float lastEventTime() const;
};

// ---- 解析（纯文本 → 资产；不碰文件系统，自检可以直接喂字符串）----
bool parseSequence(const std::string& text, const std::string& sourcePath,
                   SequenceAsset& out);

// ---- 加载（走 `assets::resolveAssetPath` 的候选链）----
bool loadSequence(const std::string& path, SequenceAsset& out);

// ============================================================
// 采样
// ============================================================

// t 处的相机关键帧姿态。**夹到 `[0, duration]`**，并且：
//   · 没有关键帧 → 返回 false（调用方不要覆盖相机）
//   · t 早于第一帧 → 第一帧（不是"什么都不做"：过场一开始就该是那个机位）
//   · t 晚于最后一帧 → 最后一帧
bool sampleCamera(const SequenceAsset& seq, float t, SeqCameraKey& out);

// t 时刻**生效中**的那一帧的下标（`t` 落在 [keys[i].t, keys[i+1].t) 里）。
// 没有关键帧 → -1；t 早于第一帧 → 0。供 UI 高亮与日志用。
int cameraKeyIndexAt(const SequenceAsset& seq, float t);

// 收集落在 `[from, to)` 里的事件下标，**按时间升序**写进 `out`。
// from > to（跨圈那一段）请调用方拆成两次调用 —— 这里不自作聪明地
// 处理回绕，否则"到底算发过没有"会变成一笔糊涂账。
// 返回追加的条数。
int collectEvents(const SequenceAsset& seq, float from, float to,
                  std::vector<int>* out);

// 弧度差取最短弧，结果 ∈ (−π, π]。**yaw 插值与"转了多少度"都用它。**
float shortestAngle(float from, float to);

} // namespace sequence
