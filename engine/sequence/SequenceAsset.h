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

    // 作者写在文件里的 `"//"` 说明（JSON 没有注释语法，约定用这个键）。
    //
    // ⚠⚠ **写回时必须原样带回去**。`opening.seq.json` 里那一段解释
    //     "yaw 是弧度、step 的切点落在下一帧" 的话，是这份格式唯一
    //     触手可及的文档 —— 编辑器存一次盘就把它抹掉的话，作者的注释
    //     会在他第一次点 Save 时无声消失，而那时他正忙着看运镜。
    std::string comment;

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
// 写回（#311 时间轴编辑器用）
// ============================================================
//
// ⚠⚠ **写出来的东西必须能被 `parseSequence` 原样读回**。这是整个写回层
//     唯一的正确性判据，也是自检 `rt_*` 那几条在钉的事：`toJson` 之后
//     `parseSequence` 一遍，`cameraKeys` / `events` / `duration` 必须逐字段相等。
//
// 四样东西刻意**不写**（与 `dialogue::toJson` 同一套理由）：
//   · `valid` / `error` / `warnings` —— 加载期诊断，不是内容。写进去下次读盘
//     会把上次的警告当成"这次也这样"，作者永远甩不掉一条已经修好的警告
//   · `sourcePath` —— 它是**从哪读来的**，不是资产属性。写进文件里，下次
//     换个目录打开就指回老路径
//   · 空的 `payload` —— 整个字段省略。写 `"payload": ""` 与不写是同一件事，
//     但 diff 里会多出一行噪声
//   · `duration <= 0` 的推算意图 —— 见下面 ①
//
// ⚠⚠ ① **`duration` 只在"显式值"和"推算值一致之外"时才有歧义。**
//     `parseSequence` 的口径是：`duration <= 0` → 按最晚的关键帧/事件推算。
//     编辑器把 duration 变成可拖的字段之后，必须**总是显式写出**它 ——
//     否则"我把最后一帧从 8s 拖到 9s"会因为 duration 仍按内容推算而
//     看起来什么都没发生（实际上是 duration 跟着长了，但那不是作者的意图：
//     他是想把这一帧往后挪，让尾巴留白）。写显式值之后这条语义才稳定。
//
// ⚠⚠ ② `ease` 总是写。它是"这两帧之间怎么走"的唯一表述，省略就得去翻
//     代码里的默认值。作者看文件时要能一眼看出哪一段是硬切。
std::string toJson(const SequenceAsset& a);

// 写：临时文件 + rename（写一半断电 = 文件打不开，而存档点就在一个
// 快捷键之外）。失败时 `err` 是人话。
bool saveSequence(const std::string& path, const SequenceAsset& a,
                  std::string& err);

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
