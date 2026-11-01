#include "animation/TransformClip.h"

#include "core/Logger.h"
#include "scene/Scene.h"

#include <cmath>
#include <cstdlib>

namespace anim {
namespace {

// 与 `editor/ToolkitEditors.cpp` 的 `easeKey` 逐字相同。
// ⚠ 两份实现是**故意的重复**吗？不是 —— 但也不能简单 include 过来：
//   编辑器那个在匿名命名空间里。真正的约束是"两边公式必须一样"，所以这里
//   把公式和理由一起写下来；改一处必须改两处（`verify_transform_clip.py`
//   的 B 组用同一组关键帧比对编辑器/运行时的采样值，漏改会立刻红）。
float easeKey(float u) {
    u = std::clamp(u, 0.0f, 1.0f);
    return u * u * (3.0f - 2.0f * u);
}

void copyKey(const ecs::TransformKey& k, ecs::TransformComponent& out) {
    out.position = k.position;
    out.rotation = k.rotation;
    out.scale = k.scale;
}

void blendKey(const ecs::TransformKey& a, const ecs::TransformKey& b, float u,
              ecs::TransformComponent& out) {
    const float k = easeKey(u);
    out.position = glm::mix(a.position, b.position, k);
    out.rotation = glm::mix(a.rotation, b.rotation, k);
    out.scale = glm::mix(a.scale, b.scale, k);
}

} // namespace

void sampleTransformClip(const ecs::AnimationClipComponent& c, float t,
                         ecs::TransformComponent& out) {
    // 纪律 ③：空轨道不许动 transform。
    if (c.keys.empty()) return;

    const ecs::TransformKey& first = c.keys.front();
    const ecs::TransformKey& last = c.keys.back();

    if (c.keys.size() == 1 || !(t > first.t)) {
        // `!(t > first.t)` 而不是 `t <= first.t`：t 是 NaN 时前者为真，
        // 后者为假 —— 否则下面的循环一个区间都命中不了，transform 会
        // 保持上一帧的值（"偶尔卡一帧"，几乎不可能复现）。
        copyKey(first, out);
        return;
    }
    if (t >= last.t) {
        copyKey(last, out);
        return;
    }
    for (std::size_t i = 0; i + 1 < c.keys.size(); ++i) {
        const ecs::TransformKey& a = c.keys[i];
        const ecs::TransformKey& b = c.keys[i + 1];
        if (t >= a.t && t <= b.t) {
            const float u = (b.t > a.t) ? ((t - a.t) / (b.t - a.t)) : 0.0f;
            blendKey(a, b, u, out);
            return;
        }
    }
    // 走到这里只可能是 keys 没按 t 排序（手改 JSON）或者中间有空洞。
    // 退到最后一帧而不是"什么都不做"：前者至少形状可预期，
    // 后者会让物体停在某个中间值上，看起来像随机卡住。
    copyKey(last, out);
}

bool advanceTransformClip(ecs::AnimationClipComponent& c, float dt) {
    if (!c.playing) return false;
    // `!(dt > 0)` 同时吃掉 0 和 NaN。dt==0 是合法调用（暂停 / 首帧），
    // 但"没推进"就不能让调用方去重写 transform —— 见头文件纪律 ②。
    if (!(dt > 0.0f)) return false;

    c.time += dt;

    if (c.duration > 1.0e-4f) {
        if (c.time >= c.duration) {
            if (c.loop) {
                // fmod 而不是 `-= duration`：卡顿一帧跨过好几圈时，减法
                // 会把 time 留在 duration 之外，下一帧再减一次才回来。
                c.time = std::fmod(c.time, c.duration);
            } else {
                c.time = c.duration;
                c.playing = false;
                VK_LOG_INFO("ANIMCLIP-END dur=%.2f", static_cast<double>(c.duration));
            }
        }
    } else {
        // duration 无效（0 或负）：当成"单帧"。不这么兜的话 loop 分支会
        // `fmod(x, 0)` → NaN → 之后所有采样都拿 NaN 去比区间，全不命中。
        c.time = 0.0f;
        if (!c.loop) c.playing = false;
    }
    return true;
}

TransformClipStats tickTransformClips(scene::Scene& sc, float dt) {
    TransformClipStats st;
    ecs::World& w = sc.world();
    // 首个推进过的实体 —— 只给 `MYVK_ANIMCLIP_LOG` 那条探针用。
    ecs::Entity first{};
    bool hasFirst = false;

    w.each<ecs::AnimationClipComponent>(
        [&](ecs::Entity e, ecs::AnimationClipComponent& c) {
            if (c.keys.empty()) return;
            const bool wasPlaying = c.playing;
            if (!advanceTransformClip(c, dt)) return;   // 纪律 ②：没推进就不写

            ecs::TransformComponent* tr = w.get<ecs::TransformComponent>(e);
            if (!tr) return;

            sampleTransformClip(c, c.time, *tr);
            ++st.applied;
            if (wasPlaying) ++st.playing;
            if (wasPlaying && !c.playing) ++st.ended;
            if (!hasFirst) {
                first = e;
                hasFirst = true;
            }
        });

    // ---- 探针（默认关）----
    // 为什么要有它：这一层唯一的可观察效果是"物体位置变了"。没有日志的话，
    // 端到端用例只能靠截图比像素 —— 而那东西在虚拟机上极不稳。
    // 每 60 帧一条，`MYVK_ANIMCLIP_LOG=1` 才开（默认零开销：一个静态 bool）。
    // ⚠ 不要在自检里跑这个（自检直接调 `tickTransformClips` 但那边的进程
    //   不设这个 env，所以不会污染 `--selftest` 的输出）。
    static const bool kLog = std::getenv("MYVK_ANIMCLIP_LOG") != nullptr;
    if (kLog && st.applied > 0 && hasFirst) {
        static int s_frame = 0;
        if ((s_frame++ % 60) == 0) {
            const auto* c = w.get<ecs::AnimationClipComponent>(first);
            const auto* tr = w.get<ecs::TransformComponent>(first);
            const std::string* nm = w.name(first);
            if (c && tr) {
                VK_LOG_INFO("ANIMCLIP-TICK name=%s t=%.3f pos=(%.4f,%.4f,%.4f) "
                            "n=%d",
                            nm ? nm->c_str() : "?", static_cast<double>(c->time),
                            static_cast<double>(tr->position.x),
                            static_cast<double>(tr->position.y),
                            static_cast<double>(tr->position.z), st.applied);
            }
        }
    }
    return st;
}

void syncTransformClipPlayState(scene::Scene& sc, bool resetTime) {
    ecs::World& w = sc.world();
    w.each<ecs::AnimationClipComponent>(
        [&](ecs::Entity e, ecs::AnimationClipComponent& c) {
            if (c.keys.empty()) return;
            c.playing = c.autoplay;
            if (resetTime) c.time = 0.0f;

            // 采样一次：进入的那一帧就要对，不能先闪一帧模型自带位姿。
            ecs::TransformComponent* tr = w.get<ecs::TransformComponent>(e);
            if (tr) sampleTransformClip(c, c.time, *tr);
        });
}

} // namespace anim
