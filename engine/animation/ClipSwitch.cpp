// ============================================================
// animation/ClipSwitch.cpp
// ============================================================

#include "animation/ClipSwitch.h"

namespace animation {

void switchClip(ecs::SkinnedMeshComponent& sk, ecs::AnimatorComponent* an,
                const std::string& clip, bool restart, float blendSec) {
    if (sk.clipName != clip) {
        if (blendSec > 0.0f && !sk.clipName.empty()) {
            sk.fadeFromClip = sk.clipName;
            // ⚠ 旧播放头必须取**当时**的权威值。有 AnimatorComponent 时是
            //   `an->time`（它可能在这帧刚被状态机推进过），取 `sk.time`
            //   会拿到镜像前的那一份 —— 过渡会从差一帧的位置开始。
            sk.fadeFromTime = an ? an->time : sk.time;
            sk.fadeT = 0.0f;
            sk.fadeDur = blendSec;
        } else {
            // 硬切（或第一次选 clip）：清掉残留的过渡，别让它淡进来
            sk.fadeFromClip.clear();
            sk.fadeT = 0.0f;
            sk.fadeDur = 0.0f;
        }
        sk.clipName = clip;
        if (restart) sk.time = 0.0f;
    }
    // ⚠ clip **相同**且 restart 时，这里刻意不动 `sk.time`（只归零
    //   `an->time`）—— 这是从 `Locomotion.cpp` 原样搬过来的行为，逐位一致
    //   优先。看起来两处会短暂不一致，但 `AnimatorSystem` 每帧都会把
    //   `an->time` 镜像回 `sk.time`，所以对外只有一份真相。

    if (an) {
        if (restart) an->time = 0.0f;
        // 我们亲自选了 clip → 别再补 defaultClip。
        // （不加这一句的话，状态机切到 Walk 之后，凡是 `appliedDefaultClip`
        //   还是 false 的实体都会被"补一次 Idle"打回 idle —— 而这只在
        //   状态机的第一帧发生，症状是"角色一直在抖着切 clip"。）
        an->appliedDefaultClip = true;
    }
}

void setClipPlaying(ecs::SkinnedMeshComponent& sk, ecs::AnimatorComponent* an,
                    bool playing) {
    sk.playing = playing;
    if (an) an->playing = playing;
}

void setClipLoop(ecs::SkinnedMeshComponent& sk, ecs::AnimatorComponent* an,
                 bool loop) {
    sk.loop = loop;
    if (an) an->loop = loop;
}

void setClipSpeed(ecs::SkinnedMeshComponent& sk, ecs::AnimatorComponent* an,
                  float speed) {
    // 两份权威字段都要写（理由与 playing / loop 相同，见 Components.h 的
    // "权威关系"）：只写一份则"有 AnimatorComponent 的实体按另一份播"，
    // 症状是步幅匹配对某些角色生效、对另一些不生效。
    //
    // ⚠ 不夹到正数区间以外：0.01 倍速（几乎定格）与 3 倍速都是合法输入，
    //   越界该由调用方用 strideRateFor 的 min/max 决定。
    sk.speed = speed;
    if (an) an->speed = speed;
}

} // namespace animation
