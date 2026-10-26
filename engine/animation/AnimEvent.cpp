#include "animation/AnimEvent.h"

#include <algorithm>

namespace animation {

namespace {

// 收集 [a, b) 里落着的事件下标，**按时间升序**（同时刻保持表里的原顺序）。
// 只在 b > a 时工作 —— 空区间直接返回，这是"暂停时不发事件"的落点。
void collectSegment(const AnimEventTrack& track, float a, float b,
                    std::vector<int>& out) {
    if (b <= a) return;
    const size_t first = out.size();
    for (size_t i = 0; i < track.events.size(); ++i) {
        const float e = track.events[i].time;
        // ⚠ 半开：左闭右开。见头文件"三条不能想当然的语义"第 1 条。
        if (e >= a && e < b) out.push_back(static_cast<int>(i));
    }
    // 只对**本次新增的尾巴**排序：前一段的结果必须留在前面（跨圈的两段
    // 按时间先后是"上圈末尾"→"新圈开头"，全局排序会把它们倒过来）。
    if (out.size() - first > 1) {
        std::stable_sort(out.begin() + static_cast<std::ptrdiff_t>(first),
                         out.end(),
                         [&](int l, int r) {
                             return track.events[static_cast<size_t>(l)].time <
                                    track.events[static_cast<size_t>(r)].time;
                         });
    }
}

} // namespace

const AnimEventTrack* findAnimEventTrack(
    const std::vector<AnimEventTrack>& tracks, const std::string& clip) {
    if (clip.empty()) return nullptr;
    for (const AnimEventTrack& t : tracks) {
        if (t.clip == clip) return &t;
    }
    return nullptr;
}

void sortAnimEventTrack(AnimEventTrack& track) {
    std::stable_sort(track.events.begin(), track.events.end(),
                     [](const AnimEvent& a, const AnimEvent& b) {
                         return a.time < b.time;
                     });
}

void collectAnimEvents(const AnimEventTrack& track, float prevTime,
                       float newTime, float duration, bool looped,
                       std::vector<int>& out) {
    out.clear();
    if (track.events.empty()) return;

    // 时长 <= 0：单帧 clip，播放头永远停在 0，没有"跨过"这回事。
    //
    // ⚠ 不能退化成"落到 [prev,new) 就发"：`duration == 0` 时调用方传进来的
    //   prev/new 都是 0，那会变成"每帧都发"—— 而这正是本文件存在的理由。
    if (!(duration > 0.0f)) return;

    // 夹到 [0, duration]。调用方可能传进 clamp 之后的边界值（不循环时
    // `time` 被 clamp 在 [0,dur]），这里再做一遍是防止"负速度把播放头
    // 推到负数"时事件表被整段跳过。
    prevTime = std::clamp(prevTime, 0.0f, duration);
    newTime = std::clamp(newTime, 0.0f, duration);

    if (newTime >= prevTime) {
        // 常规：一帧内没跨圈
        collectSegment(track, prevTime, newTime, out);
        return;
    }

    // 播放头倒退了。两种来路，语义不同：
    if (looped) {
        // ① 跨圈：[prev, duration) 是上圈尾巴，[0, new) 是新圈开头。
        //    必须**按这个顺序**收集，不能合并成一个大区间排序 ——
        //    "上圈 0.9 s 的事件"发生在"新圈 0.1 s 的事件"之前。
        collectSegment(track, prevTime, duration, out);
        collectSegment(track, 0.0f, newTime, out);
    } else {
        // ② 非循环却倒退了（脚本直接把 time 改小 / 拖时间轴）：
        //    当作从头开始，只收新圈开头这一截。不收 [prev,duration] ——
        //    那一段**没有播放过**，发出去就是凭空多一次。
        collectSegment(track, 0.0f, newTime, out);
    }
}

} // namespace animation
