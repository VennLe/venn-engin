#include "audio/AudioSystem.h"

#include "audio/NullBackend.h"
#if defined(_WIN32)
#include "audio/WasapiBackend.h"
#endif

#include "core/Logger.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace audio {

namespace {

// 一个 block 的耗时 / block 时长（AUDIO-STATS 的 `cpu=`）。
// ⚠ 用 steady_clock（MSVC 上就是 QPC，读一次 ~20 ns，不进内核）而不是
//   `GetProcessTimes` —— 后者噪声大、不稳定，且要切内核。
using Clock = std::chrono::steady_clock;

inline double secondsSince(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

constexpr float kStreamSeconds = 3.0f;   // 3 s 环形缓冲（§3.8）
constexpr int kStreamId = 1;             // 流式在 Mixer 里固定占 1 号

} // namespace

// ------------------------------------------------------------
// 音频线程回调（**唯一**在实时线程上跑的我们的代码）
// ------------------------------------------------------------
struct AudioSystemCallback {
    static void mix(void* user, float* out, std::uint32_t frames,
                    std::uint32_t channels) {
        auto* sys = static_cast<AudioSystem*>(user);
        const auto t0 = Clock::now();
        if (sys->m_mixReady) {
            sys->m_mixer.mixBlock(out, frames);
        } else {
            std::memset(out, 0, sizeof(float) * frames * channels);
        }
        const double secs = secondsSince(t0);
        if (secs > 0.0) {
            const double blockSecs =
                static_cast<double>(frames) /
                static_cast<double>(sys->m_deviceFormat.sampleRate
                                        ? sys->m_deviceFormat.sampleRate
                                        : 48000);
            if (blockSecs > 0.0) {
                // 指数平滑：单块抖动大，观感上要一个稳定的数字
                const float inst = static_cast<float>(secs / blockSecs);
                const float prev = sys->m_cpuRatio.load(std::memory_order_relaxed);
                sys->m_cpuRatio.store(prev * 0.9f + inst * 0.1f,
                                      std::memory_order_relaxed);
            }
        }
    }
};

AudioSystem::AudioSystem() = default;
AudioSystem::~AudioSystem() { shutdown(); }

const char* AudioSystem::backendName() const {
    return m_backend ? m_backend->name() : "none";
}

void AudioSystem::init(const AudioSettings& settings) {
    m_settings = settings;
    m_master = settings.master;
    m_busGain[static_cast<int>(BusId::Master)] = 1.0f;   // Master 走 m_master
    m_busGain[static_cast<int>(BusId::Music)] = settings.music;
    m_busGain[static_cast<int>(BusId::SFX)] = settings.sfx;
    m_busGain[static_cast<int>(BusId::Voice)] = settings.voice;
    m_muted = settings.mute;

    if (!settings.enabled) {
        // ⚠ 关掉音频时**什么也不开**：产品默认开，但自动化（--selftest /
        //   --frames / 回归脚本）会注入 MYVK_RUNTIME_AUDIO=0，既有基线
        //   才逐位不变（§3.2）。
        m_state = State::Closed;
        VK_LOG_INFO("AUDIO-INIT enabled=0 backend=none device=- sr=0 ch=0 fallback=0");
        return;
    }

    openDevice();
}

void AudioSystem::openDevice() {
    m_state = State::Closed;
    m_backend.reset();

#if defined(_WIN32)
    if (m_settings.forceNull) {
        m_backend = std::make_unique<NullBackend>();
    } else {
        m_backend = std::make_unique<WasapiBackend>();
    }
#else
    m_backend = std::make_unique<NullBackend>();
#endif

    // ⚠ `__none__` 是"显式指定没有设备"的哨兵：**不回落**，直接失败。
    //   验收里用它打"无声卡也不崩"的那条分支（G14-3）。而"填了一个假名字"
    //   是另一回事 —— 那种要回落到默认设备并打 `fallback=1`（R9）。
    if (m_settings.device == "__none__") {
        m_backend.reset();
        m_state = State::Failed;
        if (!m_devfailLogged) {
            VK_LOG_WARN("AUDIO-DEVFAIL backend=%s reason=device-not-found fallback=0",
                        m_settings.forceNull ? "null" : "wasapi");
            m_devfailLogged = true;
        }
        return;
    }

    OpenParams p;
    p.deviceName = m_settings.device;
    p.preferSampleRate = 48000;
    p.preferChannels = 2;
    p.blockFrames = 512;

    std::string err;
    if (!m_backend->open(p, &AudioSystemCallback::mix, this, err)) {
        m_backend.reset();
        m_state = State::Failed;
        if (!m_devfailLogged) {
            VK_LOG_WARN("AUDIO-DEVFAIL backend=%s reason=%s fallback=0",
                        m_settings.forceNull ? "null" : "wasapi", err.c_str());
            m_devfailLogged = true;
        }
        return;
    }

    m_deviceFormat = m_backend->format();
    m_deviceName = m_backend->deviceName();

    // ⚠ 先拿到真实格式再 init Mixer（§4.2）：shared mode 的设备几乎总是
    //   48 kHz float32，和我们"想要的" 44.1 kHz 不一样。重采样归 Mixer。
    m_mixer.init(m_deviceFormat.sampleRate,
                 m_deviceFormat.channels > 2 ? 2 : m_deviceFormat.channels);
    m_mixReady = true;

    m_mixer.setMasterGain(m_master);
    m_mixer.setBusGain(static_cast<int>(BusId::Music),
                       m_busGain[static_cast<int>(BusId::Music)]);
    m_mixer.setBusGain(static_cast<int>(BusId::SFX),
                       m_busGain[static_cast<int>(BusId::SFX)]);
    m_mixer.setBusGain(static_cast<int>(BusId::Voice),
                       m_busGain[static_cast<int>(BusId::Voice)]);
    m_mixer.setMuted(m_muted);
    m_mixer.setDuckDepth(m_settings.duckDepth);

    const std::string& dn = m_deviceName;
    auto esc = dn;
    for (char& c : esc) {
        // ⚠ detail= 里不能有空格（脚本按 \S* 解析）→ 设备名里的空格换下划线
        if (c == ' ' || c == '\t') c = '_';
    }

#if defined(_WIN32)
    const bool fallback =
        (m_backend->name()[0] == 'w')
            ? static_cast<WasapiBackend*>(m_backend.get())->fellBack()
            : false;
#else
    const bool fallback = false;
#endif

    if (!m_backend->start()) {
        m_state = State::Failed;
        if (!m_devfailLogged) {
            VK_LOG_WARN("AUDIO-DEVFAIL backend=%s reason=start-failed fallback=%d",
                        m_backend->name(), fallback ? 1 : 0);
            m_devfailLogged = true;
        }
        return;
    }

    m_state = State::Running;
    m_devfailLogged = false;
    VK_LOG_INFO("AUDIO-INIT enabled=1 backend=%s device=%s sr=%u ch=%u fallback=%d",
                m_backend->name(), esc.c_str(), m_deviceFormat.sampleRate,
                m_deviceFormat.channels, fallback ? 1 : 0);
    if (fallback) {
        // R9：配了名字但没匹配上 → 必须留痕，否则用户以为设置生效了
        VK_LOG_WARN("AUDIO-DEVWARN reason=device-not-found want='%s' -> default",
                    m_settings.device.c_str());
    }
}

void AudioSystem::closeDevice() {
    if (m_backend) {
        m_backend->stop();
        m_backend->close();
        m_backend.reset();
    }
    m_mixReady = false;
    m_state = State::Closed;
}

void AudioSystem::shutdown() {
    if (m_state == State::Closed && !m_backend) return;
    m_mixer.stopAll(0.0f);
    closeDevice();
    m_stream.active = false;
    m_stream.ring.clear();
    m_stream.clip.reset();
    m_clips.clear();
    m_sourceSlots.clear();
    m_cache.clear();
}

void AudioSystem::setListener(const float pos[3], const float forward[3],
                              const float up[3]) {
    if (m_state != State::Running) return;
    m_mixer.setListener(pos, forward, up);
}

// ------------------------------------------------------------
// 素材
// ------------------------------------------------------------
int AudioSystem::ensureClip(const std::string& path) {
    for (std::size_t i = 0; i < m_clips.size(); ++i) {
        if (m_clips[i].path == path) return static_cast<int>(i);
    }
    if (static_cast<int>(m_clips.size()) >= kMaxClips) {
        VK_LOG_WARN("AUDIO-CLIP-FAIL path=%s reason=too-many-clips", path.c_str());
        return -1;
    }
    std::string err;
    auto clip = m_cache.get(path, AudioClip::Mode::InMemory, err);
    if (!clip) return -1;
    ClipEntry e;
    e.path = path;
    e.clip = clip;
    m_clips.push_back(e);
    const int id = static_cast<int>(m_clips.size() - 1);
    m_mixer.setClip(static_cast<std::uint32_t>(id),
                    ClipView{clip->data(), clip->format().frames,
                             clip->format().channels, clip->format().sampleRate});
    return id;
}

// ------------------------------------------------------------
// 播放
// ------------------------------------------------------------
std::uint64_t AudioSystem::startVoice(const PlayDesc& desc,
                                      const std::string& path, bool streaming) {
    if (m_state != State::Running) return 0;
    std::uint32_t slot = 0, gen = 0;
    if (!m_mixer.play(desc, &slot, &gen)) return 0;
    VK_LOG_INFO("AUDIO-PLAY id=%u,%u clip=%s bus=%d loop=%d vol=%.2f stream=%d",
                slot, gen, path.c_str(), static_cast<int>(desc.bus),
                desc.loop ? 1 : 0, static_cast<double>(desc.volume),
                streaming ? 1 : 0);
    return (static_cast<std::uint64_t>(gen) << 32) | slot;
}

bool AudioSystem::playOneShot(const std::string& clipPath, BusId bus,
                              const float* pos, float volume, float pitch) {
    if (m_state != State::Running) {
        // 设备不可用时**不报错也不刷屏**：这是正常工况（无声卡机器）
        return false;
    }
    const int clipId = ensureClip(clipPath);
    if (clipId < 0) return false;

    PlayDesc d;
    d.clipId = static_cast<std::uint32_t>(clipId);
    d.bus = static_cast<std::uint32_t>(bus);
    d.volume = volume;
    d.pitch = pitch;
    d.spatial = (pos != nullptr);
    if (pos) {
        d.pos[0] = pos[0];
        d.pos[1] = pos[1];
        d.pos[2] = pos[2];
        d.minDist = 1.0f;
        d.maxDist = 50.0f;
        d.rolloff = 1.0f;
    }
    return startVoice(d, clipPath, false) != 0;
}

// ------------------------------------------------------------
// 流式音乐
// ------------------------------------------------------------
void AudioSystem::fillStream(Stream& s) {
    if (!s.clip || !s.active) return;
    const std::uint64_t readAbs = s.absRead.load(std::memory_order_relaxed);
    const std::uint64_t written = s.absWritten.load(std::memory_order_relaxed);
    const std::uint64_t target =
        readAbs + static_cast<std::uint64_t>(kStreamSeconds * s.sampleRate);
    if (written >= target) return;

    // ⚠ 绝不在音频回调里读文件；这里在主线程，每帧把环形缓冲填到 3 s。
    //   60 fps 下每帧只需填 16 ms 的量 → 余量 180 倍（主线程卡 2 s 也不断）。
    std::vector<float> tmp(static_cast<std::size_t>(4096) * s.channels);
    std::uint64_t w = written;
    while (w < target) {
        std::uint32_t want = 4096;
        const std::uint64_t room = target - w;
        if (room < want) want = static_cast<std::uint32_t>(room);
        const std::uint32_t got = s.clip->readStreaming(tmp.data(), want);
        if (got == 0) {
            if (!s.loop) {
                s.totalFrames = w;   // 到文件尾：告诉 Mixer 长度
                break;
            }
            s.clip->rewindStreaming();
            s.decodeCursor = 0;
            continue;
        }
        for (std::uint32_t i = 0; i < got; ++i) {
            const std::uint64_t abs = w + i;
            const std::uint64_t idx = abs % s.ringFrames;
            for (std::uint32_t c = 0; c < s.channels; ++c) {
                s.ring[static_cast<std::size_t>(idx) * s.channels + c] =
                    tmp[static_cast<std::size_t>(i) * s.channels + c];
            }
        }
        w += got;
        s.decodeCursor += got;
    }
    s.absWritten.store(w, std::memory_order_relaxed);
}

bool AudioSystem::playMusic(const std::string& clipPath, float volume) {
    if (m_state != State::Running) return false;

    // 同一时刻只允许一路流（§3.8：流式是音乐专用）。第二条直接替换。
    stopMusic();

    // ⚠ err 必须是个**具名**变量：`get` 的第三个参数是非 const 引用，
    //   传临时 `std::string()` 在 MSVC 上直接编译不过（C2664）。
    std::string err;
    auto clip = m_cache.get(clipPath, AudioClip::Mode::Streaming, err);
    if (!clip) return false;

    m_stream.clip = clip;
    m_stream.channels = clip->format().channels ? clip->format().channels : 1;
    m_stream.sampleRate = clip->format().sampleRate ? clip->format().sampleRate
                                                    : 48000;
    m_stream.totalFrames = clip->format().frames;
    m_stream.ringFrames =
        static_cast<std::uint32_t>(kStreamSeconds * m_stream.sampleRate);
    if (m_stream.ringFrames < 1024) m_stream.ringFrames = 1024;
    m_stream.ring.assign(
        static_cast<std::size_t>(m_stream.ringFrames) * m_stream.channels, 0.0f);
    m_stream.absWritten.store(0);
    m_stream.absRead.store(0);
    m_stream.decodeCursor = 0;
    m_stream.loop = true;
    m_stream.active = true;

    if (!clip->rewindStreaming()) {
        m_stream.active = false;
        return false;
    }

    StreamView sv;
    sv.ring = m_stream.ring.data();
    sv.ringFrames = m_stream.ringFrames;
    sv.channels = m_stream.channels;
    sv.sampleRate = m_stream.sampleRate;
    sv.totalFrames = m_stream.totalFrames;
    sv.absWritten = &m_stream.absWritten;
    sv.absRead = &m_stream.absRead;
    m_mixer.setStream(kStreamId, sv);

    // 先填 3 s，再起播（否则第一块必然 underrun）
    fillStream(m_stream);

    PlayDesc d;
    d.streamId = kStreamId;
    d.bus = static_cast<std::uint32_t>(BusId::Music);
    d.volume = volume;
    m_stream.volume = volume;
    d.loop = true;
    std::uint32_t slot = 0, gen = 0;
    if (!m_mixer.play(d, &slot, &gen)) {
        m_stream.active = false;
        return false;
    }
    m_stream.slot = slot;
    m_stream.gen = gen;
    VK_LOG_INFO("AUDIO-PLAY id=%u,%u clip=%s bus=%d loop=1 vol=%.2f stream=1",
                slot, gen, clipPath.c_str(), static_cast<int>(BusId::Music),
                static_cast<double>(volume));
    return true;
}

void AudioSystem::stopMusic() {
    if (!m_stream.active) return;
    if (m_state == State::Running)
        m_mixer.stop(m_stream.slot, m_stream.gen, 5.0f);
    m_stream.active = false;
    // 流式 clip 不留在缓存里（§G14-6 第 4 条）：解码器 + 环形缓冲一起释放
    m_stream.clip.reset();
    m_stream.ring.clear();
    m_stream.ring.shrink_to_fit();
    m_stream.ringFrames = 0;
}

bool AudioSystem::seekMusic(double seconds) {
    if (!m_stream.active || !m_stream.clip) return false;
    if (seconds < 0.0) seconds = 0.0;

    const std::uint64_t target =
        static_cast<std::uint64_t>(seconds * m_stream.sampleRate);

    // ① 旧声部淡出 5 ms（§G14-6 第 3 条：seek 必须淡出，否则是"咔"）。
    //    不去动 `m_stream.active` —— 流是**连续的**，只是位置换了。
    if (m_state == State::Running)
        m_mixer.stop(m_stream.slot, m_stream.gen, 5.0f);

    // ② 清环形缓冲 + 重置计数器。
    //    ⚠ 顺序不能反：`fillStream` 用 `absRead` 算"要填到哪"，
    //      先重置再填 → 目标就是 3 s；先填再重置 → 填了一堆立刻被丢掉，
    //      而 `absWritten` 回 0 之后 `sampleStream` 会认为"还没写进来"，
    //      结果是**静音**（而且一轮内看不出来，因为下一帧又填满了）。
    std::fill(m_stream.ring.begin(), m_stream.ring.end(), 0.0f);
    m_stream.absWritten.store(0, std::memory_order_relaxed);
    m_stream.absRead.store(0, std::memory_order_relaxed);
    m_stream.decodeCursor = 0;

    if (!m_stream.clip->seekStreaming(target)) {
        // 定位失败（超出文件尾）：把流停掉，别留一个"假装在播"的空壳
        stopMusic();
        VK_LOG_WARN("AUDIO-STREAM-SEEK failed want=%.2f",
                    static_cast<double>(seconds));
        return false;
    }

    // ③ 立刻填满 3 s，再起新声部
    fillStream(m_stream);

    PlayDesc d;
    d.streamId = kStreamId;
    d.bus = static_cast<std::uint32_t>(BusId::Music);
    d.volume = m_stream.volume;
    d.loop = true;
    std::uint32_t slot = 0, gen = 0;
    if (!m_mixer.play(d, &slot, &gen)) {
        stopMusic();
        return false;
    }
    m_stream.slot = slot;
    m_stream.gen = gen;

    VK_LOG_INFO("AUDIO-STREAM-SEEK pos=%.2f id=%u,%u ring=%u",
                static_cast<double>(seconds), slot, gen, m_stream.ringFrames);
    return true;
}

double AudioSystem::musicPositionSeconds() const {
    if (!m_stream.sampleRate) return 0.0;
    return static_cast<double>(m_stream.absRead.load(std::memory_order_relaxed)) /
           static_cast<double>(m_stream.sampleRate);
}

void AudioSystem::stopAll() {
    if (m_state == State::Running) m_mixer.stopAll(5.0f);
    m_sourceSlots.clear();
    m_stream.active = false;
}

// ------------------------------------------------------------
// 声源同步
// ------------------------------------------------------------
void AudioSystem::applySources(const SourceUpdate* srcs, std::size_t count) {
    if (m_state != State::Running) return;

    // 1) 停止"已经不在 / 不再播"的
    for (std::size_t i = 0; i < m_sourceSlots.size();) {
        SourceSlot& ss = m_sourceSlots[i];
        const SourceUpdate* live = nullptr;
        for (std::size_t k = 0; k < count; ++k) {
            if (srcs[k].id == ss.id) { live = &srcs[k]; break; }
        }
        const bool gone = !live || !live->playing || live->clip != ss.clip;
        if (gone) {
            m_mixer.stop(ss.slot, ss.gen, 20.0f);
            m_sourceSlots.erase(m_sourceSlots.begin() +
                                static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }

    // 2) 起新的 / 更新已有的
    for (std::size_t k = 0; k < count; ++k) {
        const SourceUpdate& u = srcs[k];
        if (!u.playing || u.clip.empty()) continue;

        SourceSlot* found = nullptr;
        for (auto& ss : m_sourceSlots) {
            if (ss.id == u.id) { found = &ss; break; }
        }

        if (!found) {
            const int clipId = ensureClip(u.clip);
            if (clipId < 0) continue;
            PlayDesc d;
            d.clipId = static_cast<std::uint32_t>(clipId);
            d.bus = static_cast<std::uint32_t>(u.bus);
            d.volume = u.volume;
            d.pitch = u.pitch;
            d.loop = u.loop;
            d.spatial = u.spatial;
            d.minDist = u.minDist;
            d.maxDist = u.maxDist;
            d.rolloff = u.rolloff;
            d.pos[0] = u.pos[0];
            d.pos[1] = u.pos[1];
            d.pos[2] = u.pos[2];
            std::uint32_t slot = 0, gen = 0;
            if (!m_mixer.play(d, &slot, &gen)) continue;
            m_sourceSlots.push_back(SourceSlot{u.id, u.clip, slot, gen});
            VK_LOG_INFO("AUDIO-PLAY id=%u,%u clip=%s bus=%d loop=%d vol=%.2f stream=0",
                        slot, gen, u.clip.c_str(), u.bus, u.loop ? 1 : 0,
                        static_cast<double>(u.volume));
        } else {
            m_mixer.updateVoice(found->slot, found->gen, u.pos, u.volume);
        }
    }
}

// ------------------------------------------------------------
// 每帧
// ------------------------------------------------------------
void AudioSystem::logStats() {
    const MixStats st = m_mixer.stats();
    VK_LOG_INFO("AUDIO-STATS voices=%d rms=%.4f peak=%.4f cpu=%.2f "
                "fill=%u sr=%u ch=%u muted=%d mem=%.0f msgs=%llu",
                st.voices, static_cast<double>(st.rms),
                static_cast<double>(st.peak),
                static_cast<double>(m_cpuRatio.load()) * 100.0,
                m_stream.ringFrames, m_deviceFormat.sampleRate,
                m_deviceFormat.channels, m_muted ? 1 : 0,
                static_cast<double>(cacheResidentBytes() +
                                    streamResidentBytes()),
                static_cast<unsigned long long>(m_statsLines));
    ++m_statsLines;
}

std::uint64_t AudioSystem::streamResidentBytes() const {
    return static_cast<std::uint64_t>(m_stream.ring.size()) * sizeof(float);
}

void AudioSystem::update(float dt) {
    if (m_state == State::Failed) {
        // 设备拔了 / 被独占：如果开了 RETRY，就每 2 s 试一次
        if (m_settings.retryOnFailure) {
            m_retryTimer += dt;
            if (m_retryTimer >= 2.0f) {
                m_retryTimer = 0.0f;
                openDevice();
            }
        }
        return;
    }
    if (m_state != State::Running) return;

    // 设备在运行中被拔掉
    if (m_backend && m_backend->deviceLost()) {
        closeDevice();
        m_state = State::Failed;
        if (!m_devfailLogged) {
            VK_LOG_WARN("AUDIO-DEVFAIL backend=%s reason=device-lost fallback=0",
                        m_settings.forceNull ? "null" : "wasapi");
            m_devfailLogged = true;
        }
        return;
    }

    // ducking 事件（只在跨阈值时打，§6）
    float duckGain = 1.0f;
    if (m_mixer.consumeDuckEvent(&duckGain)) {
        const float db = 20.0f * std::log10(std::max(duckGain, 1e-4f));
        VK_LOG_INFO("AUDIO-DUCK music=%.2fdb reason=voice-active",
                    static_cast<double>(db));
    }

    // 流式预读
    if (m_stream.active) fillStream(m_stream);

    // AUDIO-STATS 每 60 帧（R15：每帧打会把日志淹没，且 tail -1 读到中间态）
    m_frameCounter++;
    if (m_frameCounter % 60 == 0) {
        logStats();
        // `AUDIO-STREAM` 是流式的**专用观测点**（§G14-6 验收）：
        //   underrun=0        → 缓冲够深，主线程卡一下也不断音
        //   cur / loops       → 真的在往前走、真的在绕圈
        //   resident / ring   → 常驻内存与"文件多长"无关
        if (m_stream.active) {
            const MixStats st = m_mixer.stats();
            const std::uint64_t written =
                m_stream.absWritten.load(std::memory_order_relaxed);
            const std::uint64_t loops =
                m_stream.totalFrames ? written / m_stream.totalFrames : 0;
            VK_LOG_INFO("AUDIO-STREAM active=%d cur=%.2f loops=%llu "
                        "ring=%u resident=%llu underrun=%llu fill=%llu",
                        m_stream.active ? 1 : 0, musicPositionSeconds(),
                        static_cast<unsigned long long>(loops),
                        m_stream.ringFrames,
                        static_cast<unsigned long long>(streamResidentBytes()),
                        static_cast<unsigned long long>(st.underruns),
                        static_cast<unsigned long long>(written));
        }
    }
}

// ------------------------------------------------------------
// 音量
// ------------------------------------------------------------
void AudioSystem::setBusGain(BusId bus, float gain) {
    const int b = static_cast<int>(bus);
    if (b < 0 || b >= kBusCount) return;
    m_busGain[b] = gain;
    if (m_state != State::Running) return;
    if (bus == BusId::Master) m_mixer.setMasterGain(gain);
    else m_mixer.setBusGain(b, gain);
}

void AudioSystem::setMasterGain(float gain) {
    m_master = gain;
    if (m_state == State::Running) m_mixer.setMasterGain(gain);
}

void AudioSystem::setMuted(bool muted) {
    m_muted = muted;
    m_settings.mute = muted;
    if (m_state == State::Running) m_mixer.setMuted(muted);
}

void AudioSystem::setDuckDepth(float depth) {
    m_settings.duckDepth = depth;
    if (m_state == State::Running) m_mixer.setDuckDepth(depth);
}

} // namespace audio
