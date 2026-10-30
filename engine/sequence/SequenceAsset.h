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

#include <cstdint>
#include <string>
#include <vector>

namespace sequence {

// 缓动。`Step` = 在**这一帧的时间点**直接切过去（硬切镜头），
// 用于"从远景一下切到特写"，这是过场里最常用的一种转场。
//
// 2026-10-28（R5）：新增 `Bezier` —— 真贝塞尔，形状由两根手柄决定。
// 三个老模式**一个字节都没动**（`sampleCamera` 里同一个 `w` 出口，
// 老分支的结果逐位不变，见 SequenceSelfTest 的 `sample_legacy_bitwise`）。
enum class Ease : int {
    Linear = 0,
    Smooth = 1,   // smoothstep（默认）
    Step = 2,     // 保持前一帧，到下一帧瞬间切换
    Bezier = 3,   // 三次贝塞尔，手柄见 SeqCameraKey::outHandle / inHandle
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

    // ---- 贝塞尔手柄（`ease == Bezier` 时生效）----
    //
    // ⚠⚠ 坐标是**归一化段内坐标**，不是绝对 (秒, 弧度)：
    //      x ∈ [0,1] 是"这一段的时间比例"，y 是"这一段的进度比例"。
    //    于是同一组手柄能被 position / yaw / pitch / fov **四个通道共用**
    //    （它们本来就共用同一个 w），而不用为每个通道各存两根手柄 ——
    //    Blender 存的是绝对值，因为它的每个通道是独立的 F-Curve；
    //    这里一个段只有一个标量时间重映射，归一化才是对的抽象。
    //
    // 默认 `{1/3, 0} / {-1/3, 0}` = 标准 ease-in-out（等价于老 Smooth
    // 的"两头慢"，但不是同一条曲线：Bezier 可以过冲，smoothstep 不能）。
    // ⚠ x 的符号有约束：out.x >= 0、in.x <= 0。写反了曲线会倒流
    //   （同一时刻两个值），加载期钳回，见 parseSequence。
    glm::vec2 outHandle{1.0f / 3.0f, 0.0f};    // 相对本帧
    glm::vec2 inHandle{-1.0f / 3.0f, 0.0f};    // 相对**下一帧**
};

struct SeqEvent {
    float t = 0.0f;
    std::string kind;      // "Footstep" / "Sound" / "Dialogue" / "Flag" / 自定义
    std::string payload;   // 自由文本：音效名 / 台词 id / 旗标名
};

// ============================================================
// 多机位 Marker（2026-10-30，运镜体系 §七）
// ============================================================
//
// Blender 的 **Marker Binding**：时间轴上每隔几个 marker 绑一台相机，
// 播放头越过 marker 就换机位。这是"正打 / 反打 / 特写"唯一好用的做法 ——
// 把三段运镜分别放在三台相机上各自 K 帧，比把三段塞进一条 key 轨里
// 再靠 step 断开要清楚得多（也才能给每段配不同的镜头/手持/路径）。
//
// ------------------------------------------------------------
// 三条语义
// ------------------------------------------------------------
// ⚠⚠ ① **区间半开 `[t_i, t_{i+1})`**，与 `AnimEvent` / `SeqEvent` 同一条纪律。
//     闭区间会让"播放头正好压在 marker 上"这段时间每帧都算切换，而
//     混合动画会被反复重置（表现是切过去的镜头永远在抖）。
//
// ⚠⚠ ② **`cameraGuid == 0` 是合法值**，含义是"这一段回到关键帧轨"。
//     它让"两台实体机位 + 中间一段导演机位"能混排；也让作者删掉一台
//     相机之后，那段退化成老行为而不是整份资产打不开。
//
// ⚠⚠ ③ **第一个 marker 之前（`t < shots[0].t`）不算任何 shot**，
//     `shotIndexAt` 返回 −1，播放器保持原行为（用关键帧轨）。
//     不能"取第 0 个 shot 兜住" —— 作者常把第一个 marker 放在 1s（留给
//     黑场/标题），前面那 1 秒必须还是主轨的镜头。
struct SeqShot {
    float t = 0.0f;                 // 从这一时刻起生效（秒）
    // 绑定的相机**实体 guid**（`ecs::guidToHex` 那个 64 位值）。
    // ⚠ 存 guid 不存名字：名字可以改、可以重名；guid 在场景重建后仍唯一
    //   （与 `CameraComponent::lookTargetGuid` 同一个理由）。
    std::uint64_t cameraGuid = 0;   // 0 = 回落关键帧轨
    // 从上一机位混到这一台用多久（秒）。0 = **硬切** —— 电影剪辑里
    // 最常用的是硬切，默认值就该是它。
    // ⚠ 这里只**声明意图**，真正的混合由 `CameraSystem::blendTo` 执行：
    //   本文件是纯数据（不认识 Scene / CameraSystem）。
    float blendSeconds = 0.0f;
    std::string label;              // 面板与日志用（"A 机位 特写"）
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
    // 多机位 marker（已按 t 升序）。**空 = 老行为**：整段只有一条关键帧轨
    // 驱动一台相机。这是"加了功能但不改老资产"的硬约束所在。
    std::vector<SeqShot> shots;

    bool valid = false;
    std::string error;                       // valid=false 时的原因
    std::vector<std::string> warnings;       // 不致命但要让人知道

    bool hasCamera() const { return !cameraKeys.empty(); }
    bool hasShots() const { return !shots.empty(); }
    float lastCameraTime() const;
    float lastEventTime() const;
    float lastShotTime() const;
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

// t 时刻**生效中**的那个机位 marker 的下标（半开 `[t_i, t_{i+1})`）。
//   · 没有 marker       → −1
//   · t 早于第一个 marker → −1（**不是** 0：见 `SeqShot` 的文件头 ③）
//   · t 晚于最后一个     → 最后一个（机位一直保持到片尾，这是剪辑的常识）
int shotIndexAt(const SequenceAsset& seq, float t);

// 收集落在 `[from, to)` 里的事件下标，**按时间升序**写进 `out`。
// from > to（跨圈那一段）请调用方拆成两次调用 —— 这里不自作聪明地
// 处理回绕，否则"到底算发过没有"会变成一笔糊涂账。
// 返回追加的条数。
int collectEvents(const SequenceAsset& seq, float from, float to,
                  std::vector<int>* out);

// 弧度差取最短弧，结果 ∈ (−π, π]。**yaw 插值与"转了多少度"都用它。**
float shortestAngle(float from, float to);

} // namespace sequence
