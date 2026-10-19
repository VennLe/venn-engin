#include "audio/Mixer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace audio {

// ============================================================
// 3D 公式（唯一一份，Mixer 与自检共用）
// ============================================================

float spatialGain(float distance, float minDist, float maxDist, float rolloff) {
    if (minDist < 0.0f) minDist = 0.0f;
    if (maxDist <= minDist) maxDist = minDist + 0.001f;
    if (rolloff < 0.0f) rolloff = 0.0f;
    if (distance <= minDist) return 1.0f;
    if (distance >= maxDist) return 0.0f;

    // 倒数衰减：距离越小越接近 1。
    const float inv = minDist / (minDist + rolloff * (distance - minDist));

    // ⚠⚠ 收尾斜坡（R6）：`inv` 在 maxDist 处**并不为 0**（只是很小），
    //   直接"超过 maxDist 就静音"会在跨过边界的那一帧产生阶跃 →
    //   听感是"啪"的一声。最后 10% 线性收到 0。
    const float fadeStart = maxDist * 0.9f;
    float fade = 1.0f;
    if (distance > fadeStart) {
        const float span = maxDist - fadeStart;
        fade = span > 1e-6f ? (maxDist - distance) / span : 0.0f;
        if (fade < 0.0f) fade = 0.0f;
    }
    return inv * fade;
}

void equalPowerPan(float x, float* outL, float* outR) {
    if (x < -1.0f) x = -1.0f;
    if (x > 1.0f) x = 1.0f;
    const float k = (x + 1.0f) * 0.7853981633974483f;   // (x+1)*π/4
    *outL = std::cos(k);
    *outR = std::sin(k);
}

void evaluateSpatial(const float src[3], const float lpos[3],
                     const float lright[3], float minDist, float maxDist,
                     float rolloff, float* outGain, float* outPanX) {
    const float dx = src[0] - lpos[0];
    const float dy = src[1] - lpos[1];
    const float dz = src[2] - lpos[2];
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);

    *outGain = spatialGain(d, minDist, maxDist, rolloff);

    float panX = 0.0f;
    if (d > 1e-4f) {
        const float nx = dx / d, ny = dy / d, nz = dz / d;
        panX = nx * lright[0] + ny * lright[1] + nz * lright[2];
        if (panX < -1.0f) panX = -1.0f;
        if (panX > 1.0f) panX = 1.0f;
    }
    *outPanX = panX;
}

// ============================================================
// Mixer
// ============================================================

namespace {

// 软削波：[-0.8, 0.8] 内**严格线性**，超出后平滑饱和到 ±1。
//
// ⚠ 为什么不是全程 tanh：tanh 在 0.35 处就已经有 ~1.5% 的压缩，
//   "音量 0.5 → RMS 0.5 倍（容差 1%）"这条断言会直接假红。
//   保留线性段既满足"输出 |x| <= 1"，又不破坏音量线性。
inline float softClip(float x) {
    constexpr float kT = 0.8f;
    if (x > kT) return kT + (1.0f - kT) * std::tanh((x - kT) / (1.0f - kT));
    if (x < -kT) return -kT - (1.0f - kT) * std::tanh((-x - kT) / (1.0f - kT));
    return x;
}

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

} // namespace

Mixer::Mixer() {
    // ⚠ 队列必须在**构造**时就分配好，不能等 `init()`：
    //   `stopAll()` / `setMuted()` / `setListener()` 这些在主线程随时可调，
    //   如果设备没打开（`init` 从未跑过），对一个空 vector 取
    //   `m_queue[head % kQueueSize]` 就是越界写 —— 现象是"自检跑到设备
    //   那一组就没输出了"（进程被打穿）。实测踩过一次。
    m_queue.resize(kQueueSize);
}
Mixer::~Mixer() = default;

void Mixer::init(std::uint32_t outSampleRate, std::uint32_t outChannels) {
    m_outSr = outSampleRate ? outSampleRate : 48000;
    m_outCh = outChannels ? outChannels : 2;
    if (m_queue.size() != kQueueSize) m_queue.resize(kQueueSize);
    // ⚠ 一次分配好：`mixBlock` 里绝不再 resize / push_back。
    m_busBuf.assign(static_cast<std::size_t>(kBusCount) * kMaxBlockFrames * 2,
                    0.0f);
    m_dryBuf.assign(static_cast<std::size_t>(kMaxBlockFrames) * 2, 0.0f);
    for (int i = 0; i < kMaxVoices; ++i) {
        m_slotBusy[i].store(false);
        m_slotGen[i].store(0);
        m_allocSeq[i] = 0;
        m_voices[i] = Voice{};
    }
    for (int i = 0; i < kBusCount; ++i) {
        m_busCur[i] = 1.0f;
        m_busTarget[i] = 1.0f;
    }
    m_head.store(0);
    m_tail.store(0);
    m_allocCounter = 0;
    m_muted = false;
    m_duck = 0.0f;
    m_duckEvent = false;
    m_listenerSet = false;
    m_lPos[0] = m_lPos[1] = m_lPos[2] = 0.0f;
    m_lFwd[0] = 0.0f; m_lFwd[1] = 0.0f; m_lFwd[2] = -1.0f;
    m_lUp[0] = 0.0f; m_lUp[1] = 1.0f; m_lUp[2] = 0.0f;
}

void Mixer::setClip(std::uint32_t id, const ClipView& view) {
    if (id >= static_cast<std::uint32_t>(kMaxClips)) return;
    m_clips[id] = view;
}

void Mixer::clearClips() {
    for (int i = 0; i < kMaxClips; ++i) m_clips[i] = ClipView{};
}

void Mixer::setStream(std::uint32_t id, const StreamView& view) {
    if (id >= static_cast<std::uint32_t>(kMaxStreams)) return;
    m_streams[id] = view;
}

bool Mixer::slotBusy(std::uint32_t slot) const {
    if (slot >= static_cast<std::uint32_t>(kMaxVoices)) return false;
    return m_slotBusy[slot].load(std::memory_order_relaxed);
}

std::uint32_t Mixer::slotGeneration(std::uint32_t slot) const {
    if (slot >= static_cast<std::uint32_t>(kMaxVoices)) return 0;
    return m_slotGen[slot].load(std::memory_order_relaxed);
}

bool Mixer::play(const PlayDesc& desc, std::uint32_t* outSlot,
                 std::uint32_t* outGen) {
    if (m_queue.empty()) return false;

    // 先找出一个可用的槽：优先空槽，池满则**抢最旧**（§3.10）。
    int slot = -1;
    for (int i = 0; i < kMaxVoices; ++i) {
        if (!m_slotBusy[i].load(std::memory_order_relaxed)) {
            slot = i;
            break;
        }
    }
    bool stole = false;
    if (slot < 0) {
        std::uint64_t best = ~0ull;
        for (int i = 0; i < kMaxVoices; ++i) {
            if (m_allocSeq[i] < best) {
                best = m_allocSeq[i];
                slot = i;
            }
        }
        stole = true;
        m_stolen.fetch_add(1);
    }
    if (slot < 0) return false;

    // ⚠ 抢断必须先让"被抢的那一声"**淡出 10 ms**，否则是"哒"一声（R16）。
    //   做法：先入队一条针对**旧代际**的 Stop，再入队 Play —— 音频线程
    //   先看到 Stop（voice 进入 fade），再看到 Play（voice 正在 fade →
    //   挂成 pending），fade 结束时自动接上。
    if (stole) {
        const std::uint32_t oldGen = m_slotGen[slot].load(std::memory_order_relaxed);
        stop(static_cast<std::uint32_t>(slot), oldGen, 10.0f);
    }

    const std::uint32_t gen =
        m_slotGen[slot].load(std::memory_order_relaxed) + 1;
    m_slotGen[slot].store(gen, std::memory_order_relaxed);
    m_slotBusy[slot].store(true, std::memory_order_relaxed);
    m_allocSeq[slot] = ++m_allocCounter;

    AudioCommand c;
    c.type = static_cast<std::uint32_t>(CmdType::Play);
    c.slot = static_cast<std::uint32_t>(slot);
    c.gen = gen;
    c.clipId = desc.clipId;
    c.streamId = desc.streamId;
    c.bus = desc.bus;
    c.volume = desc.volume;
    c.pitch = desc.pitch;
    c.minDist = desc.minDist;
    c.maxDist = desc.maxDist;
    c.rolloff = desc.rolloff;
    c.pos[0] = desc.pos[0];
    c.pos[1] = desc.pos[1];
    c.pos[2] = desc.pos[2];
    c.flags = (desc.loop ? 1u : 0u) | (desc.spatial ? 2u : 0u) |
              (desc.streamId ? 4u : 0u);

    const std::uint32_t head = m_head.load(std::memory_order_relaxed);
    const std::uint32_t tail = m_tail.load(std::memory_order_acquire);
    if (head - tail >= kQueueSize) {
        // 队列满：撤销刚才的槽位占用（否则这个槽永远"忙"）。
        m_slotBusy[slot].store(false, std::memory_order_relaxed);
        return false;
    }
    m_queue[head % kQueueSize] = c;
    m_head.store(head + 1, std::memory_order_release);

    if (outSlot) *outSlot = static_cast<std::uint32_t>(slot);
    if (outGen) *outGen = gen;
    return true;
}

void Mixer::stop(std::uint32_t slot, std::uint32_t gen, float fadeMs) {
    AudioCommand c;
    c.type = static_cast<std::uint32_t>(CmdType::Stop);
    c.slot = slot;
    c.gen = gen;
    c.fadeMs = fadeMs;
    const std::uint32_t head = m_head.load(std::memory_order_relaxed);
    const std::uint32_t tail = m_tail.load(std::memory_order_acquire);
    if (head - tail >= kQueueSize) return;
    m_queue[head % kQueueSize] = c;
    m_head.store(head + 1, std::memory_order_release);
}

void Mixer::stopAll(float fadeMs) {
    AudioCommand c;
    c.type = static_cast<std::uint32_t>(CmdType::StopAll);
    c.fadeMs = fadeMs;
    const std::uint32_t head = m_head.load(std::memory_order_relaxed);
    const std::uint32_t tail = m_tail.load(std::memory_order_acquire);
    if (head - tail >= kQueueSize) return;
    m_queue[head % kQueueSize] = c;
    m_head.store(head + 1, std::memory_order_release);
}

void Mixer::setBusGain(int bus, float gain) {
    if (bus < 0 || bus >= kBusCount) return;
    AudioCommand c;
    c.type = static_cast<std::uint32_t>(CmdType::SetBusGain);
    c.bus = static_cast<std::uint32_t>(bus);
    c.value = clampf(gain, 0.0f, 4.0f);
    const std::uint32_t head = m_head.load(std::memory_order_relaxed);
    const std::uint32_t tail = m_tail.load(std::memory_order_acquire);
    if (head - tail >= kQueueSize) return;
    m_queue[head % kQueueSize] = c;
    m_head.store(head + 1, std::memory_order_release);
}

void Mixer::setMasterGain(float gain) { setBusGain(0, gain); }

void Mixer::setMuted(bool muted) {
    AudioCommand c;
    c.type = static_cast<std::uint32_t>(CmdType::SetMuted);
    c.value = muted ? 1.0f : 0.0f;
    const std::uint32_t head = m_head.load(std::memory_order_relaxed);
    const std::uint32_t tail = m_tail.load(std::memory_order_acquire);
    if (head - tail >= kQueueSize) return;
    m_queue[head % kQueueSize] = c;
    m_head.store(head + 1, std::memory_order_release);
}

void Mixer::setDuckDepth(float depth) {
    AudioCommand c;
    c.type = static_cast<std::uint32_t>(CmdType::SetDuckDepth);
    c.value = depth < 0.0f ? 0.0f : (depth > 1.0f ? 1.0f : depth);
    const std::uint32_t head = m_head.load(std::memory_order_relaxed);
    const std::uint32_t tail = m_tail.load(std::memory_order_acquire);
    if (head - tail >= kQueueSize) return;
    m_queue[head % kQueueSize] = c;
    m_head.store(head + 1, std::memory_order_release);
}

void Mixer::setListener(const float pos[3], const float forward[3],
                        const float up[3]) {
    AudioCommand c;
    c.type = static_cast<std::uint32_t>(CmdType::SetListener);
    c.pos[0] = pos[0]; c.pos[1] = pos[1]; c.pos[2] = pos[2];
    c.fwd[0] = forward[0]; c.fwd[1] = forward[1]; c.fwd[2] = forward[2];
    c.up[0] = up[0]; c.up[1] = up[1]; c.up[2] = up[2];
    const std::uint32_t head = m_head.load(std::memory_order_relaxed);
    const std::uint32_t tail = m_tail.load(std::memory_order_acquire);
    if (head - tail >= kQueueSize) return;
    m_queue[head % kQueueSize] = c;
    m_head.store(head + 1, std::memory_order_release);
}

void Mixer::updateVoice(std::uint32_t slot, std::uint32_t gen,
                        const float pos[3], float volume) {
    AudioCommand c;
    c.type = static_cast<std::uint32_t>(CmdType::UpdateVoice);
    c.slot = slot;
    c.gen = gen;
    c.value = volume;
    c.pos[0] = pos[0];
    c.pos[1] = pos[1];
    c.pos[2] = pos[2];
    const std::uint32_t head = m_head.load(std::memory_order_relaxed);
    const std::uint32_t tail = m_tail.load(std::memory_order_acquire);
    if (head - tail >= kQueueSize) return;
    m_queue[head % kQueueSize] = c;
    m_head.store(head + 1, std::memory_order_release);
}

void Mixer::applyCommand(const AudioCommand& c) {
    switch (static_cast<CmdType>(c.type)) {
        case CmdType::Play: {
            if (c.slot >= static_cast<std::uint32_t>(kMaxVoices)) return;
            Voice& v = m_voices[c.slot];
            PlayDesc d;
            d.clipId = c.clipId;
            d.streamId = c.streamId;
            d.bus = c.bus;
            d.volume = c.volume;
            d.pitch = c.pitch;
            d.loop = (c.flags & 1u) != 0;
            d.spatial = (c.flags & 2u) != 0;
            d.minDist = c.minDist;
            d.maxDist = c.maxDist;
            d.rolloff = c.rolloff;
            d.pos[0] = c.pos[0];
            d.pos[1] = c.pos[1];
            d.pos[2] = c.pos[2];
            if (v.active && v.fadeFrames > 0) {
                // 正在淡出：接在后面播（不要立刻覆盖 —— 那就是"哒"一声）
                v.pending = true;
                v.pendingDesc = d;
                v.pendingDesc.streamId = c.streamId;
                return;
            }
            startVoice(static_cast<int>(c.slot), d, c.gen);
            break;
        }
        case CmdType::Stop: {
            if (c.slot >= static_cast<std::uint32_t>(kMaxVoices)) return;
            Voice& v = m_voices[c.slot];
            // ③ 代际不匹配 = 这是一条**过期的**停止命令（槽位已被复用）。
            //    直接丢弃，否则会停掉"刚抢进来的新一声"（ABA）。
            if (!v.active || v.gen != c.gen) return;
            const std::uint32_t frames = static_cast<std::uint32_t>(
                std::max(0.0f, c.fadeMs) * 0.001f * m_outSr);
            if (frames == 0) {
                v.active = false;
                v.fadeFrames = 0;
                m_slotBusy[c.slot].store(false, std::memory_order_relaxed);
            } else {
                v.fadeFrames = frames;
                v.fadeTotal = frames;
            }
            break;
        }
        case CmdType::StopAll: {
            const std::uint32_t frames = static_cast<std::uint32_t>(
                std::max(0.0f, c.fadeMs) * 0.001f * m_outSr);
            for (int i = 0; i < kMaxVoices; ++i) {
                if (!m_voices[i].active) continue;
                if (frames == 0) {
                    m_voices[i].active = false;
                    m_voices[i].fadeFrames = 0;
                    m_slotBusy[i].store(false, std::memory_order_relaxed);
                } else {
                    m_voices[i].fadeFrames = frames;
                    m_voices[i].fadeTotal = frames;
                }
            }
            break;
        }
        case CmdType::SetBusGain:
            if (c.bus < static_cast<std::uint32_t>(kBusCount))
                m_busTarget[c.bus] = c.value;
            break;
        case CmdType::SetMuted:
            m_muted = c.value != 0.0f;
            break;
        case CmdType::SetDuckDepth:
            m_duckDepth = clampf(c.value, 0.0f, 1.0f);
            break;
        case CmdType::UpdateVoice: {
            if (c.slot >= static_cast<std::uint32_t>(kMaxVoices)) return;
            Voice& v = m_voices[c.slot];
            // ② 过期命令（槽位已被复用）→ 丢弃，否则会把"新的一声"搬走
            if (!v.active || v.gen != c.gen) return;
            v.px = c.pos[0];
            v.py = c.pos[1];
            v.pz = c.pos[2];
            v.vol = c.value;
            break;
        }
        case CmdType::SetListener: {
            m_lPos[0] = c.pos[0];
            m_lPos[1] = c.pos[1];
            m_lPos[2] = c.pos[2];
            m_lFwd[0] = c.fwd[0];
            m_lFwd[1] = c.fwd[1];
            m_lFwd[2] = c.fwd[2];
            m_lUp[0] = c.up[0];
            m_lUp[1] = c.up[1];
            m_lUp[2] = c.up[2];
            m_listenerSet = true;
            break;
        }
        default: break;
    }
}

void Mixer::startVoice(int slot, const PlayDesc& d, std::uint32_t gen) {
    Voice& v = m_voices[slot];
    v = Voice{};
    v.active = true;
    v.gen = gen;
    v.bus = static_cast<int>(d.bus);
    if (v.bus < 0 || v.bus >= kBusCount) v.bus = static_cast<int>(BusId::SFX);
    v.vol = d.volume;
    v.loop = d.loop;
    v.spatial = d.spatial;
    v.minDist = d.minDist;
    v.maxDist = d.maxDist;
    v.rolloff = d.rolloff;
    v.px = d.pos[0];
    v.py = d.pos[1];
    v.pz = d.pos[2];
    v.pos = 0.0;
    v.fadeFrames = 0;
    v.fadeTotal = 0;

    if (d.streamId) {
        v.streaming = true;
        v.stream = d.streamId;
        const StreamView& sv = m_streams[d.streamId % kMaxStreams];
        const double src = sv.sampleRate ? sv.sampleRate : m_outSr;
        v.step = (src / static_cast<double>(m_outSr)) * d.pitch;
        if (sv.absRead) sv.absRead->store(0, std::memory_order_relaxed);
    } else {
        v.streaming = false;
        v.clip = d.clipId % kMaxClips;
        const ClipView& cv = m_clips[v.clip];
        const double src = cv.sampleRate ? cv.sampleRate : m_outSr;
        v.step = (src / static_cast<double>(m_outSr)) * d.pitch;
    }
}

float Mixer::sampleStream(const StreamView& s, std::uint64_t frame,
                          int channel) const {
    if (!s.valid()) return 0.0f;
    if (channel >= static_cast<int>(s.channels)) channel = 0;
    const std::uint64_t w = s.absWritten->load(std::memory_order_relaxed);
    if (frame >= w) return 0.0f;                       // 还没写进来（underrun）
    if (w - frame > s.ringFrames) return 0.0f;         // 已经被覆盖（不该发生）
    const std::uint64_t idx = frame % s.ringFrames;
    return s.ring[idx * s.channels + static_cast<std::uint32_t>(channel)];
}

void Mixer::renderVoice(Voice& v, int slot, float* busL, float* busR,
                        std::uint32_t frames) {
    if (!v.active) return;

    // ---- 每个 block 算一次 3D 增益 + pan ----
    float gain = v.vol;
    float panL = 1.0f, panR = 1.0f;
    if (v.spatial) {
        float g = 1.0f, panX = 0.0f;
        // 右向量 = forward × up（右手系；forward=(0,0,-1) 时得 +X）
        const float rx = m_lFwd[1] * m_lUp[2] - m_lFwd[2] * m_lUp[1];
        const float ry = m_lFwd[2] * m_lUp[0] - m_lFwd[0] * m_lUp[2];
        const float rz = m_lFwd[0] * m_lUp[1] - m_lFwd[1] * m_lUp[0];
        float rlen = std::sqrt(rx * rx + ry * ry + rz * rz);
        float right[3] = {1.0f, 0.0f, 0.0f};
        if (rlen > 1e-6f) {
            right[0] = rx / rlen;
            right[1] = ry / rlen;
            right[2] = rz / rlen;
        }
        const float src[3] = {v.px, v.py, v.pz};
        evaluateSpatial(src, m_lPos, right, v.minDist, v.maxDist, v.rolloff,
                        &g, &panX);
        gain *= g;
        equalPowerPan(panX, &panL, &panR);
    }

    const bool fade = v.fadeFrames > 0;
    const float fadeStep = (fade && v.fadeTotal)
                               ? 1.0f / static_cast<float>(v.fadeTotal)
                               : 0.0f;
    float fadeScale = fade
                          ? static_cast<float>(v.fadeFrames) /
                                static_cast<float>(v.fadeTotal ? v.fadeTotal : 1)
                          : 1.0f;

    std::uint64_t framesTotal = 0;
    std::uint32_t channels = 0;
    const float* src = nullptr;
    const StreamView* stream = nullptr;
    if (v.streaming) {
        stream = &m_streams[v.stream % kMaxStreams];
        framesTotal = stream->totalFrames;
        channels = stream->channels;
    } else {
        const ClipView& cv = m_clips[v.clip % kMaxClips];
        src = cv.samples;
        framesTotal = cv.frames;
        channels = cv.channels;
    }

    if (v.streaming) {
        if (!stream->valid()) {
            v.active = false;
            m_slotBusy[slot].store(false, std::memory_order_relaxed);
            return;
        }
    } else if (src == nullptr || framesTotal == 0 || channels == 0) {
        v.active = false;
        m_slotBusy[slot].store(false, std::memory_order_relaxed);
        return;
    }

    if (v.streaming && stream && stream->absWritten) {
        const std::uint64_t w = stream->absWritten->load(std::memory_order_relaxed);
        const std::uint64_t need = static_cast<std::uint64_t>(v.pos) + frames;
        if (w < need) m_underruns.fetch_add(1, std::memory_order_relaxed);
    }

    double pos = v.pos;
    const double step = v.step;
    bool ended = false;

    for (std::uint32_t i = 0; i < frames; ++i) {
        // ---- 位置推进 / 回绕 ----
        if (framesTotal && pos >= static_cast<double>(framesTotal)) {
            if (v.streaming) {
                // ⚠ 流式**不回绕**：`pos` 是**绝对帧号**，回绕由主线程写
                //   环形缓冲时按 `abs % totalFrames` 处理（§3.8）。在音频
                //   线程里减回去会让 pos 回到一个已经被覆盖的槽位。
                if (!v.loop) { ended = true; break; }
            } else if (v.loop) {
                // ⚠ 回绕按**源采样**算（不是重采样之后），否则每次绕回
                //   都有一个 frac 相位跳变 → 听感是轻微的"哒"（§3.9）。
                while (pos >= static_cast<double>(framesTotal))
                    pos -= static_cast<double>(framesTotal);
            } else {
                ended = true;
                break;
            }
        }

        const std::uint64_t i0 = static_cast<std::uint64_t>(pos);
        const float frac = static_cast<float>(pos - static_cast<double>(i0));
        std::uint64_t i1 = i0 + 1;
        if (framesTotal && i1 >= framesTotal) {
            i1 = v.loop ? 0 : framesTotal - 1;
        }

        float sl = 0.0f, sr2 = 0.0f;
        if (v.streaming) {
            sl = sampleStream(*stream, i0, 0);
            const int c1 = channels > 1 ? 1 : 0;
            sl = sl + (sampleStream(*stream, i1, 0) - sl) * frac;
            if (channels > 1) {
                sr2 = sampleStream(*stream, i0, c1);
                sr2 = sr2 + (sampleStream(*stream, i1, c1) - sr2) * frac;
            } else {
                sr2 = sl;
            }
        } else {
            const float* p0 = src + i0 * channels;
            const float* p1 = src + i1 * channels;
            if (channels > 1) {
                sl = p0[0] + (p1[0] - p0[0]) * frac;
                sr2 = p0[1] + (p1[1] - p0[1]) * frac;
            } else {
                sl = p0[0] + (p1[0] - p0[0]) * frac;
                sr2 = sl;
            }
        }

        float g = gain;
        if (fade) {
            g *= fadeScale;
            fadeScale -= fadeStep;
            if (fadeScale < 0.0f) fadeScale = 0.0f;
        }

        busL[i] += sl * g * panL;
        busR[i] += sr2 * g * panR;

        pos += step;
    }

    v.pos = pos;

    if (fade) {
        const std::uint32_t used = frames;
        v.fadeFrames = (v.fadeFrames > used) ? (v.fadeFrames - used) : 0;
        if (v.fadeFrames == 0) {
            v.active = false;
            m_slotBusy[slot].store(false, std::memory_order_relaxed);
            if (v.pending) {
                const PlayDesc d = v.pendingDesc;
                const std::uint32_t gen = v.gen;
                v.pending = false;
                startVoice(slot, d, gen);
            }
            return;
        }
    }

    if (ended) {
        v.active = false;
        m_slotBusy[slot].store(false, std::memory_order_relaxed);
        if (v.pending) {
            const PlayDesc d = v.pendingDesc;
            const std::uint32_t gen = v.gen;
            v.pending = false;
            startVoice(slot, d, gen);
        }
    }
}

void Mixer::mixBlock(float* out, std::uint32_t frames) {
    if (!out) return;
    if (frames > static_cast<std::uint32_t>(kMaxBlockFrames))
        frames = static_cast<std::uint32_t>(kMaxBlockFrames);
    if (frames == 0) return;

    // ---- 1) 消费命令（不做任何分配）----
    std::uint32_t tail = m_tail.load(std::memory_order_relaxed);
    const std::uint32_t head = m_head.load(std::memory_order_acquire);
    while (tail != head) {
        applyCommand(m_queue[tail % kQueueSize]);
        ++tail;
    }
    m_tail.store(tail, std::memory_order_release);

    // ---- 2) 清总线缓冲 ----
    for (int b = 0; b < kBusCount; ++b) {
        float* p = m_busBuf.data() + static_cast<std::size_t>(b) *
                                        kMaxBlockFrames * 2;
        std::memset(p, 0, sizeof(float) * frames * 2);
    }

    // ---- 3) 渲染声部到各自总线 ----
    for (int i = 0; i < kMaxVoices; ++i) {
        Voice& v = m_voices[i];
        if (!v.active) continue;
        int b = v.bus;
        if (b < 0 || b >= kBusCount) b = static_cast<int>(BusId::SFX);
        float* base = m_busBuf.data() + static_cast<std::size_t>(b) *
                                            kMaxBlockFrames * 2;
        // 总线缓冲是交错的 L/R：传两个"步长为 2 的视图"不方便，这里改成
        // 先按 L/R 分离渲染到临时数组，再交错写回。
        // ⚠ 两块区域**都要清**：L 在 [0, frames)、R 在
        //   [kMaxBlockFrames, kMaxBlockFrames+frames)。曾经只清前
        //   `frames*2` 个 float —— 当 frames < kMaxBlockFrames/2 时
        //   R 区根本没被清，而 renderVoice 是 `+=`，于是右声道把**上一块
        //   的残留**加了进来（现象是"单声道素材左右不对称"，实测
        //   L=0.3535 / R=0.2907）。这种量级差异用耳朵根本听不出来。
        std::memset(m_dryBuf.data(), 0, sizeof(float) * frames);
        std::memset(m_dryBuf.data() + kMaxBlockFrames, 0,
                    sizeof(float) * frames);
        renderVoice(v, i, m_dryBuf.data(), m_dryBuf.data() + kMaxBlockFrames,
                    frames);
        for (std::uint32_t k = 0; k < frames; ++k) {
            base[k * 2 + 0] += m_dryBuf[k];
            base[k * 2 + 1] += m_dryBuf[kMaxBlockFrames + k];
        }
    }

    // ---- 4) ducking 包络（§G14-5）----
    // "Voice 总线有活跃声部" → Music 往下压。attack 50 ms / release 400 ms。
    bool voiceActive = false;
    for (int i = 0; i < kMaxVoices; ++i) {
        if (m_voices[i].active &&
            m_voices[i].bus == static_cast<int>(BusId::Voice)) {
            voiceActive = true;
            break;
        }
    }
    {
        const float attack = 1.0f / (0.050f * static_cast<float>(m_outSr));
        const float release = 1.0f / (0.400f * static_cast<float>(m_outSr));
        const float target = voiceActive ? 1.0f : 0.0f;
        const float rate = (target > m_duck) ? attack : release;
        const float stepD = rate * static_cast<float>(frames);
        const float before = m_duck;
        if (m_duck < target) m_duck = std::min(target, m_duck + stepD);
        else m_duck = std::max(target, m_duck - stepD);
        // 只在跨过阈值时记一次事件（别每帧打，R15）
        const bool crossed =
            (before < 0.5f && m_duck >= 0.5f) || (before >= 0.5f && m_duck < 0.5f);
        if (crossed) {
            m_duckEvent = true;
            m_duckEventGain = 1.0f - m_duck * m_duckDepth;
        }
    }
    const float musicScale = 1.0f - m_duck * m_duckDepth;
    m_statMusicGain.store(m_busCur[static_cast<int>(BusId::Music)] * musicScale,
                          std::memory_order_relaxed);

    // ---- 5) 总线求和 + 主增益 + 软削波 ----
    // 音量变化走 **50 ms 线性平滑**（R8：直接改增益会有"拉链噪声"）。
    const float gainStep = 1.0f / (0.050f * static_cast<float>(m_outSr));
    float cur[kBusCount];
    for (int b = 0; b < kBusCount; ++b) cur[b] = m_busCur[b];

    float peak = 0.0f;
    double acc = 0.0;
    const std::uint32_t outCh = m_outCh;

    for (std::uint32_t i = 0; i < frames; ++i) {
        float l = 0.0f, r = 0.0f;
        for (int b = 1; b < kBusCount; ++b) {
            const float t = m_busTarget[b];
            if (cur[b] < t) cur[b] = std::min(t, cur[b] + gainStep);
            else if (cur[b] > t) cur[b] = std::max(t, cur[b] - gainStep);
            float g = cur[b];
            if (b == static_cast<int>(BusId::Music)) g *= musicScale;
            const float* base = m_busBuf.data() +
                                static_cast<std::size_t>(b) * kMaxBlockFrames * 2;
            l += base[i * 2 + 0] * g;
            r += base[i * 2 + 1] * g;
        }
        // 主总线
        const float tm = m_busTarget[0];
        if (cur[0] < tm) cur[0] = std::min(tm, cur[0] + gainStep);
        else if (cur[0] > tm) cur[0] = std::max(tm, cur[0] - gainStep);
        l *= cur[0];
        r *= cur[0];

        // mute 是**输出级**静音：声部照常在跑（恢复到有声能接上，§G14-5）
        if (m_muted) { l = 0.0f; r = 0.0f; }

        l = softClip(l);
        r = softClip(r);

        const float al = std::fabs(l);
        const float ar = std::fabs(r);
        if (al > peak) peak = al;
        if (ar > peak) peak = ar;
        acc += static_cast<double>(l) * l + static_cast<double>(r) * r;

        if (outCh == 1) {
            out[i] = (l + r) * 0.5f;
        } else {
            out[i * outCh + 0] = l;
            out[i * outCh + 1] = r;
            for (std::uint32_t c = 2; c < outCh; ++c) out[i * outCh + c] = 0.0f;
        }
    }
    for (int b = 0; b < kBusCount; ++b) m_busCur[b] = cur[b];

    int voices = 0;
    for (int i = 0; i < kMaxVoices; ++i)
        if (m_voices[i].active) ++voices;

    m_statVoices.store(voices, std::memory_order_relaxed);
    m_statPeak.store(peak, std::memory_order_relaxed);
    m_statRms.store(
        static_cast<float>(std::sqrt(acc / (2.0 * static_cast<double>(frames)))),
        std::memory_order_relaxed);

    // ---- 6) 流式：回报"读到哪了"给主线程做预读 ----
    for (int i = 0; i < kMaxVoices; ++i) {
        const Voice& v = m_voices[i];
        if (!v.active || !v.streaming) continue;
        StreamView& sv = m_streams[v.stream % kMaxStreams];
        if (sv.absRead)
            sv.absRead->store(static_cast<std::uint64_t>(v.pos),
                              std::memory_order_relaxed);
    }
}

void Mixer::renderOffline(float* out, std::uint32_t frames) {
    mixBlock(out, frames);
}

MixStats Mixer::stats() const {
    MixStats s;
    s.voices = m_statVoices.load(std::memory_order_relaxed);
    s.rms = m_statRms.load(std::memory_order_relaxed);
    s.peak = m_statPeak.load(std::memory_order_relaxed);
    s.musicGain = m_statMusicGain.load(std::memory_order_relaxed);
    s.underruns = m_underruns.load(std::memory_order_relaxed);
    s.stolen = m_stolen.load(std::memory_order_relaxed);
    return s;
}

bool Mixer::consumeDuckEvent(float* musicGainOut) {
    if (!m_duckEvent) return false;
    m_duckEvent = false;
    if (musicGainOut) *musicGainOut = m_duckEventGain;
    return true;
}

} // namespace audio
