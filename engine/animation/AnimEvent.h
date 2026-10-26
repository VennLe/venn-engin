#pragma once
// ============================================================
// animation/AnimEvent —— 动画事件（Notify）
//
// 为什么单独一个文件（2026-10-26，B2）：
//   整套东西里**唯一真正难的地方**是"什么时候发"，而不是"存在哪"。
//   "什么时候发"必须能脱离场景、脱离 ECS、脱离窗口单独跑用例 ——
//   否则它只能靠"进 Play 看一眼日志"来验证，而这件事的失败形态恰好是
//   **多一帧 / 少一次 / 每帧都发**：肉眼在日志里数不出来。
//   所以边界判定抽成 `collectAnimEvents` 一个纯函数，数值用例直接喂
//   (prev, new, duration, looped) 进去。
//
// ------------------------------------------------------------
// 三条**不能想当然**的语义
// ------------------------------------------------------------
// 1) 区间是 **`[prev, new)`**，不是 `(prev, new]`，也不是闭区间。
//    * 闭区间 → 播放头停在事件上（dt=0 / 暂停 / 无符号反向）时**每帧都发**。
//    * `(prev, new]` → `t = 0` 的事件永远发不出来（`0 > 0` 为假），
//      而"开场音效"是最常见的需求。
//    半开之后顺带解决了一个精度问题：**正好落在 `new` 上不会丢**，
//    只是推迟到下一帧（那一帧的 prev 就是它）。闭区间做不到这条 ——
//    步长整除事件时刻时它会重复发，[prev,new] 两端都含就发两次。
//
// 2) **`t = duration` 等价于 `t = 0`**（循环片段）。写在 `duration` 上的
//    事件在 0 半区被排除（半开），永远不会触发 —— 想让它发就写 `0`。
//    这是刻意的：`wrapTime` 之后播放头**永远到不了** `duration`，
//    留一个"理论上存在但永远不发"的时刻比报错更糟。
//
// 3) **换 clip 时 prev 归零**。状态机在 tick 里会改 `clipName` 并把
//    `time` 重置成 0；如果 prev 还留着上一段 clip 的播放头，新 clip 的
//    事件表会被"上一段的位置"当起点，症状是切过去的第一个事件**随机
//    漏发**（漏不漏取决于上一段停在多后面）。
//
// ------------------------------------------------------------
// 为什么事件表挂在组件上而不是 `assets::AnimationClip` 上
// ------------------------------------------------------------
//   clip 是从 FBX/glTF 烤出来的**只读资产**，每次加载重新生成。把编辑好
//   的事件写进去，只在内存里活到下次重载；要持久化就得改资产格式（二进制
//   兼容性 + 重烤即丢）。挂在 `AnimatorComponent` 上则：跟场景一起落盘、
//   与 `layers`（B1）同一套作者工作流、Inspector 一行代码就能编辑。
//   Unity 的 AnimationEvent 也是存在 .anim 里而不是模型里 —— 同一件事：
//   **事件是"这个角色怎么演"，不是"这段曲线长什么样"**。
// ============================================================

#include <cstdint>
#include <string>
#include <vector>

namespace animation {

// 一条事件。`kind` 是**给代码看的**（`"Footstep"` / `"Hit"` / `"Sound"`），
// `payload` 是**给数据看的**（音效名 / 伤害值 / 特效路径）。
//
// ⚠ 两个都是字符串、不搞变体：事件的消费方是脚本 / 音频 / 特效三个系统，
//   把它们要的参数硬编成一个 union 会让"加一种事件"变成改引擎头文件。
//   `payload` 的解析留给消费方（音效只取第一个逗号前、伤害判定 `atoi`）。
struct AnimEvent {
    float time = 0.0f;      // 秒，相对 clip 起点
    std::string kind;
    std::string payload;
};

// 一段 clip 的事件表。按 `clip` **名字**挂（不是下标 —— 下标在重导出模型
// 后会整体错位，与材质那边"身份是数组下标"是同一个坑）。
struct AnimEventTrack {
    std::string clip;
    std::vector<AnimEvent> events;
};

// 找一段 clip 的事件表；没有返回 nullptr。
const AnimEventTrack* findAnimEventTrack(const std::vector<AnimEventTrack>& tracks,
                                         const std::string& clip);

// 按时间升序整理（就地）。作者顺序无所谓，发出去必须按时间。
void sortAnimEventTrack(AnimEventTrack& track);

// ---- 核心：跨过边界判定 ----
//
// prevTime / newTime 都是**已经推进过**的播放头（秒）；
// duration = clip 时长；looped = 这段 clip 是否循环（决定要不要拆两段）。
//
// 输出**事件下标**（相对 track.events），已按时间升序。
// 一次调用内同一事件最多出现一次。
//
// ⚠ `out` 会被清空后重填（调用方不用自己 clear，也就不会漏）。
// ⚠ 不排序 `track` 也能用，但同一时刻多条事件的输出顺序会跟着表里的
//   原始顺序 —— 需要确定性就调 `sortAnimEventTrack`。
void collectAnimEvents(const AnimEventTrack& track, float prevTime,
                       float newTime, float duration, bool looped,
                       std::vector<int>& out);

// 一个"这一次真的发了"的事件。带上它属于谁、哪一段、哪一层。
//
// ⚠ `time` 是**事件自身的时间**，不是播放头时间。这条是"序列不随帧率变"
//   的关键：播放头在不同帧率下落在不同位置，而事件时间永远是作者填的那个
//   数 —— 于是两份日志的 (kind, time) 序列可以逐条比对。若记播放头时间，
//   同一条事件在 60 fps 与 240 fps 下会打印出两个不同的数。
struct FiredAnimEvent {
    uint32_t entity = 0;
    std::string clip;
    std::string layer;    // 空 = 底座 clip；非空 = 附加层（B1）的名字
    float time = 0.0f;    // 事件自身的时间
    std::string kind;
    std::string payload;
};

} // namespace animation
