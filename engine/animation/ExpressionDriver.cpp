// ============================================================
// animation/ExpressionDriver.cpp
// ============================================================

#include "animation/ExpressionDriver.h"

#include "assets/Model.h"     // morphRig（只 include MorphRig.h 是不够的：
#include "assets/MorphRig.h" // Model 里的 morphRig 是值成员，需要完整类型）
#include "core/Logger.h"
#include "ecs/World.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace anim {

namespace {

float smoothstep01(float u) {
    if (u <= 0.0f) return 0.0f;
    if (u >= 1.0f) return 1.0f;
    return u * u * (3.0f - 2.0f * u);
}

// 确定性伪随机（LCG）。见文件头第 ③ 条。
float jitter01(std::uint32_t& seed) {
    seed = seed * 1664525u + 1013904223u;
    return static_cast<float>((seed >> 8) & 0xFFFFFFu) / 16777216.0f;
}

// 帧率无关的指数平滑：`tau` 秒走到 63%。
float approach(float cur, float target, float dt, float tau) {
    if (dt <= 0.0f || tau <= 0.0f) return cur;
    const float k = 1.0f - std::exp(-dt / tau);
    return cur + (target - cur) * k;
}

// 本帧要写的通道表（下标 + 值，保序）。用 vector 而不是 map：
// 一个表情通常 3~10 条通道，线性查找比哈希快，而且顺序稳定（便于日志）。
struct WriteList {
    std::vector<std::pair<int, float>> items;

    void set(int channel, float value) {
        for (auto& kv : items) {
            if (kv.first == channel) {
                kv.second = value;
                return;
            }
        }
        items.emplace_back(channel, value);
    }
    bool has(int channel) const {
        for (const auto& kv : items)
            if (kv.first == channel) return true;
        return false;
    }
    float get(int channel) const {
        for (const auto& kv : items)
            if (kv.first == channel) return kv.second;
        return 0.0f;
    }
};

} // namespace

// ---------------------------------------------------------------- 眨眼曲线

float blinkWeight(float phase, float close, float hold, float open) {
    if (phase < 0.0f) return 0.0f;
    const float c = std::max(0.0f, close);
    const float h = std::max(0.0f, hold);
    const float o = std::max(0.0f, open);
    if (c + h + o <= 0.0f) return 0.0f;   // 退化配置 → 不眨，别定格在闭眼

    float t = phase;
    if (c > 0.0f && t < c) return smoothstep01(t / c);
    t -= c;
    if (t < h) return 1.0f;
    t -= h;
    if (o > 0.0f && t < o) return 1.0f - smoothstep01(t / o);
    // 落到这里有两种情况：o == 0（没有睁眼段）或 t 已经超过总时长。
    return 0.0f;
}

// ---------------------------------------------------------------- 主循环

ExpressionStats tickExpressions(ecs::World& w, float dt, float voiceLevel) {
    ExpressionStats st;
    const float level = std::clamp(voiceLevel, 0.0f, 1.0f);
    st.level = level;

    // 自动眨眼的日志默认关（每 4 秒一条 × N 个角色 = 刷屏），
    // 与 `MYVK_ANIMCLIP_LOG` 同一条规矩：一个静态 bool，零开销。
    static const bool kLog = std::getenv("MYVK_EXPR_LOG") != nullptr;

    w.each<ecs::ExpressionComponent>(
        [&](ecs::Entity e, ecs::ExpressionComponent& xc) {
            ecs::MorphComponent* mp = w.get<ecs::MorphComponent>(e);
            if (!mp || !mp->model) return;
            const assets::MorphRig& rig = mp->model->morphRig;
            if (rig.empty()) return;

            // ---- 懒加载预设资产 ----
            // 与 `SequencerSystem` 同一套：路径可能刚在 Inspector 里被改过，
            // 所以放在"每帧要用的时候"而不是"场景加载时"。失败只报一次。
            if (!xc.presetPath.empty() && !xc.setLoaded) {
                xc.setLoaded = true;
                if (!loadExpressionSet(xc.presetPath, xc.set)) {
                    if (!xc.loadFailedLogged) {
                        xc.loadFailedLogged = true;
                        VK_LOG_WARN("EXPR-LOAD-FAIL path=%s err=%s",
                                    xc.presetPath.c_str(),
                                    xc.set.error.c_str());
                    }
                }
            }

            // ---- 眨眼计时 ----
            int blinkedNow = 0;
            if (xc.blinkEnabled) {
                const float total = std::max(0.0f, xc.blinkClose) +
                                    std::max(0.0f, xc.blinkHold) +
                                    std::max(0.0f, xc.blinkOpen);
                if (total <= 0.0f) {
                    xc.blinkPhase = -1.0f;
                } else {
                    // 首帧：按实体 id 抽一个相位，避免全场景同时眨眼。
                    if (xc.blinkTimer < 0.0f) {
                        xc.blinkSeed = (static_cast<std::uint32_t>(e.id) * 2654435761u) ^
                                       0x9E3779B9u;
                        xc.blinkTimer = xc.blinkInterval *
                                        (0.25f + 0.75f * jitter01(xc.blinkSeed));
                    }
                    if (xc.blinkRequest) {
                        xc.blinkRequest = false;
                        xc.blinkPhase = 0.0f;
                        ++blinkedNow;
                    }
                    if (xc.blinkPhase >= 0.0f) {
                        // dt==0 时**不推进**（纪律），但相位本身保留。
                        if (dt > 0.0f) xc.blinkPhase += dt;
                        if (xc.blinkPhase >= total) {
                            xc.blinkPhase = -1.0f;
                            ++xc.blinkCount;
                            // 下一次间隔 = 均值 × [0.85, 1.15]，并夹一个下限
                            // （0 会让它每帧眨一次）。
                            const float j = 0.85f + 0.30f * jitter01(xc.blinkSeed);
                            xc.blinkTimer = std::max(0.05f,
                                                     xc.blinkInterval * j);
                        }
                    } else if (dt > 0.0f) {
                        xc.blinkTimer -= dt;
                        if (xc.blinkTimer <= 0.0f) {
                            xc.blinkPhase = 0.0f;
                            ++blinkedNow;
                        }
                    }
                }
            } else {
                xc.blinkPhase = -1.0f;
            }
            st.blinked += blinkedNow;
            if (kLog && blinkedNow > 0) {
                const std::string* nm = w.name(e);
                VK_LOG_INFO("EXPR-BLINK name=%s count=%d interval=%.2f",
                            nm ? nm->c_str() : "?", xc.blinkCount,
                            static_cast<double>(xc.blinkInterval));
            }

            // ---- 口型包络 ----
            // 只有开了口型才平滑：否则停在一个非零值上会一直张着嘴。
            if (xc.lipSyncEnabled) {
                const float target = std::clamp(level * std::max(0.0f, xc.mouthGain),
                                                0.0f, 1.0f);
                // 攻击快、释放慢（见文件头第 ④ 条）。dt==0 时不动。
                const float tau = (target > xc.level) ? 0.02f : 0.09f;
                xc.level = approach(xc.level, target, dt, tau);
            } else {
                xc.level = 0.0f;
            }

            // ---- 组装本帧的目标权重 ----
            WriteList wl;
            const float k = std::clamp(xc.intensity, 0.0f, 1.0f);
            const anim::ExpressionPreset* pr =
                xc.set.valid ? xc.set.preset(xc.presetName) : nullptr;
            if (pr) {
                for (const anim::ExprWeight& ew : pr->weights) {
                    const int ci = rig.channelIndex(ew.channel);
                    if (ci < 0) {
                        ++st.missing;
                        continue;
                    }
                    wl.set(ci, std::clamp(ew.value * k, 0.0f, 1.0f));
                }
            }

            // 眨眼与口型是**加性**叠加在预设之上（再钳到 1）：
            // 预设里已经写了 0.3 的闭眼，眨眼时要整只眼闭上，不是回到 0.3。
            if (xc.blinkEnabled && !xc.blinkChannel.empty()) {
                const int ci = rig.channelIndex(xc.blinkChannel);
                if (ci >= 0) {
                    const float bw = blinkWeight(xc.blinkPhase, xc.blinkClose,
                                                 xc.blinkHold, xc.blinkOpen);
                    wl.set(ci, std::clamp(wl.get(ci) + bw, 0.0f, 1.0f));
                }
            }
            if (xc.lipSyncEnabled && !xc.mouthChannel.empty()) {
                const int ci = rig.channelIndex(xc.mouthChannel);
                if (ci >= 0)
                    wl.set(ci, std::clamp(wl.get(ci) + xc.level, 0.0f, 1.0f));
            }

            // ---- 上一帧拥有、这一帧不再拥有的通道 → 归零 ----
            // （纪律 ②：不归零的话，从"笑"切到"怒"，嘴角会一直扬着）
            //
            // ⚠⚠ 归零只做**一次**：`ownedChannels` 存的是**本帧活跃**的
            //   通道（下面用 `active` 覆盖），不包含这些补 0 的。否则清空
            //   预设之后每帧都会往那根通道写 0 —— 用户再推滑条就被抹掉，
            //   而那正是本文件最不能犯的错（见纪律 ①）。
            std::vector<std::string> active;
            active.reserve(wl.items.size());
            for (const auto& kv : wl.items)
                active.push_back(rig.channelName(
                    static_cast<std::size_t>(kv.first)));
            for (const std::string& old : xc.ownedChannels) {
                const int ci = rig.channelIndex(old);
                if (ci < 0) continue;
                if (!wl.has(ci)) wl.items.emplace_back(ci, 0.0f);
            }

            // ---- 落到权重表 ----
            if (mp->weights.size() < rig.channelCount())
                mp->weights.resize(rig.channelCount(), 0.0f);
            for (const auto& kv : wl.items) {
                const std::size_t c = static_cast<std::size_t>(kv.first);
                if (c >= mp->weights.size()) continue;
                mp->weights[c] = kv.second;
            }
            xc.ownedChannels = std::move(active);

            ++st.driven;
            st.channels += static_cast<int>(wl.items.size());
        });

    return st;
}

// ---------------------------------------------------------------- 状态对齐

void syncExpressionPlayState(ecs::World& w, bool reseed) {
    w.each<ecs::ExpressionComponent>(
        [&](ecs::Entity e, ecs::ExpressionComponent& xc) {
            xc.blinkPhase = -1.0f;
            xc.blinkCount = 0;
            xc.level = 0.0f;
            // `<0` 让下一帧的 tick 重新抽相位；reseed 时换个种子，
            // 于是"重开一次"不会每次都从同一个时间点眨眼。
            xc.blinkTimer = -1.0f;
            xc.blinkRequest = false;
            if (reseed)
                xc.blinkSeed = (static_cast<std::uint32_t>(e.id) * 2246822519u) ^
                               0x85EBCA6Bu;
            // 不在这里写权重：交给紧接着的 tick（它需要 rig，而这里没有）。
        });
}

// ---------------------------------------------------------------- 切预设

bool setExpressionPreset(ecs::World& w, ecs::Entity e,
                         const std::string& name, std::string* err) {
    if (err) err->clear();
    ecs::ExpressionComponent* xc = w.get<ecs::ExpressionComponent>(e);
    if (!xc) {
        if (err) *err = "no ExpressionComponent";
        return false;
    }
    if (!xc->setLoaded && !xc->presetPath.empty()) {
        xc->setLoaded = true;
        if (!loadExpressionSet(xc->presetPath, xc->set) && err)
            *err = xc->set.error;
    }
    if (!name.empty()) {
        if (!xc->set.valid) {
            if (err) *err = xc->set.error.empty() ? "no preset asset" : xc->set.error;
            return false;
        }
        if (xc->set.find(name) < 0) {
            if (err) *err = "unknown preset '" + name + "'";
            return false;
        }
    }
    xc->presetName = name;
    const std::string* nm = w.name(e);
    VK_LOG_INFO("EXPR-SET entity=%u name=%s preset=%s intensity=%.3f",
                static_cast<unsigned>(e.id), nm ? nm->c_str() : "?",
                name.empty() ? "-" : name.c_str(),
                static_cast<double>(xc->intensity));
    return true;
}

} // namespace anim
