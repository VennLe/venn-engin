#include "audio/AudioSelfTest.h"

#include "assets/AssetPath.h"
#include "audio/AudioBackend.h"
#include "audio/AudioClip.h"
#include "audio/AudioFormat.h"
#include "audio/AudioSystem.h"
#include "audio/Mixer.h"
#include "audio/NullBackend.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace audio {

namespace {

int g_pass = 0;
int g_fail = 0;

void check(const char* name, bool ok, const std::string& detail = {}) {
    std::printf("  [%s] %-40s %s\n", ok ? "PASS" : "FAIL", name,
                detail.c_str());
    if (ok) ++g_pass;
    else ++g_fail;
}

// 与 HudSelfTest 同款：只按 double 格式化，避免 %d/%f 混传的 UB。
// ⚠ 参数个数必须覆盖格式串（漏传会打出 -9.25e61 这种垃圾 detail）。
std::string fmt(const char* f, double a = 0.0, double b = 0.0,
                double c = 0.0, double d = 0.0) {
    char buf[224];
    std::snprintf(buf, sizeof(buf), f, a, b, c, d);
    return buf;
}

// ---------------------------------------------------------------- 基础

void testApi() {
    std::printf("-- 基础：格式 / 总线名\n");
    AudioFormat f;
    f.sampleRate = 44100;
    f.frames = 44100;
    check("format_duration_1s", std::fabs(f.durationSeconds() - 1.0) < 1e-9,
          fmt("dur=%.6f", f.durationSeconds()));

    check("bus_master_index", busIndexFromName("master") == 0, "");
    check("bus_music_index", busIndexFromName("music") == 1, "");
    check("bus_sfx_index", busIndexFromName("sfx") == 2, "");
    check("bus_voice_index", busIndexFromName("voice") == 3, "");
    // 拼错必须能被发现（不能静默回落）—— §3.11 的教训。
    check("bus_unknown_is_minus1", busIndexFromName("sfxx") == -1, "");
    check("bus_null_is_minus1", busIndexFromName(nullptr) == -1, "");
}

// ---------------------------------------------------------------- 设备

void testNullBackend() {
    std::printf("-- NullBackend：开 / pump / 关\n");
    NullBackend backend;
    OpenParams p;
    p.preferSampleRate = 48000;
    p.preferChannels = 2;
    p.blockFrames = 512;

    std::string err;
    const bool opened = backend.open(p, [](void*, float* out, std::uint32_t n,
                                           std::uint32_t ch) {
        // 填一个已知图案：左 +0.25 / 右 -0.25，方便验证"回调真的被调用"。
        for (std::uint32_t i = 0; i < n; ++i) {
            for (std::uint32_t c = 0; c < ch; ++c)
                out[i * ch + c] = (c == 0) ? 0.25f : -0.25f;
        }
    }, nullptr, err);
    check("null_open", opened && backend.isOpen(), err);

    const float* buf = backend.pump(512);
    check("null_pump_not_null", buf != nullptr, "");
    check("null_pump_pattern",
          buf && buf[0] == 0.25f && buf[1] == -0.25f,
          buf ? fmt("l=%.3f r=%.3f", buf[0], buf[1]) : "no buffer");

    std::vector<DeviceInfo> devs;
    const int n = backend.enumerate(devs);
    check("null_enumerate", n == 1 && devs.size() == 1 &&
                                devs[0].isDefault,
          n == 1 ? devs[0].name : fmt("n=%.0f", static_cast<double>(n)));

    check("null_block_seconds",
          std::fabs(backend.blockDurationSeconds() - 512.0 / 48000.0) < 1e-9,
          fmt("%.6f", backend.blockDurationSeconds()));

    // 设备失效标志必须能被置起来（验收里用它打 AUDIO-DEVFAIL 分支）
    check("null_no_lost_initially", !backend.deviceLost(), "");
    backend.simulateDeviceLost();
    check("null_lost_detectable", backend.deviceLost(), "");

    backend.close();
    check("null_closed", !backend.isOpen(), "");
    check("null_pump_after_close", backend.pump(512) == nullptr, "");
}

// ---------------------------------------------------------------- 解码

double rmsOf(const std::vector<float>& s) {
    if (s.empty()) return 0.0;
    double acc = 0.0;
    for (float v : s) acc += static_cast<double>(v) * v;
    return std::sqrt(acc / static_cast<double>(s.size()));
}

void testDecode() {
    std::printf("-- 解码：WAV 16/24-bit / 元信息 / 坏文件\n");

    // ---- 16-bit 立体声 1 s / 44.1 kHz ----
    AudioClip clip;
    std::string err;
    const bool ok = clip.load("audio/tone_440_stereo.wav",
                              AudioClip::Mode::InMemory, err);
    check("wav_open_16bit", ok, err);
    if (!ok) return;
    check("wav_frames_44100", clip.format().frames == 44100,
          fmt("frames=%.0f", static_cast<double>(clip.format().frames)));
    check("wav_sr_44100", clip.format().sampleRate == 44100,
          fmt("sr=%.0f", static_cast<double>(clip.format().sampleRate)));
    check("wav_ch_2", clip.format().channels == 2,
          fmt("ch=%.0f", static_cast<double>(clip.format().channels)));
    check("wav_samples_size",
          clip.samples().size() == 44100u * 2u,
          fmt("n=%.0f", static_cast<double>(clip.samples().size())));

    // 第一个正弦周期逐点比对：解析波形 vs 解出来的样本。
    // 一个 440 Hz 周期 = 44100/440 ≈ 100.2 帧。
    {
        const double amp = 0.5;
        const int period = static_cast<int>(44100.0 / 440.0);
        double worst = 0.0;
        for (int i = 0; i < period; ++i) {
            const double t = i / 44100.0;
            const double want = amp * std::sin(2.0 * 3.14159265358979323846 *
                                               440.0 * t);
            const double got = clip.samples()[static_cast<std::size_t>(i) * 2];
            worst = std::max(worst, std::fabs(want - got));
        }
        // 16-bit 的量化误差上限是 1/32768 ≈ 3.1e-5，1e-4 是两倍余量。
        check("wav_first_period_max_err", worst < 1e-4,
              fmt("maxerr=%.3e", worst));
    }

    const double rms16 = rmsOf(clip.samples());
    check("wav_rms_nonzero", rms16 > 0.3 && rms16 < 0.4,
          fmt("rms=%.6f", rms16));

    // ---- 24-bit 对照：同一信号，归一化后 RMS 差 < 0.1% ----
    {
        AudioClip c24;
        std::string e24;
        const bool ok24 = c24.load("audio/tone_440_24bit.wav",
                                   AudioClip::Mode::InMemory, e24);
        check("wav_open_24bit", ok24, e24);
        if (ok24) {
            check("wav_24_frames_match", c24.format().frames == 44100,
                  fmt("frames=%.0f", static_cast<double>(c24.format().frames)));
            const double rms24 = rmsOf(c24.samples());
            const double rel = rms16 > 0.0 ? std::fabs(rms24 - rms16) / rms16
                                           : 1.0;
            // ⚠ 这条断言就是"24-bit 按 3 字节打包"的守卫：按 4 字节读会
            //   整体错位，RMS 会跳到完全不同的量级（实测差 > 50%）。
            check("wav_24_vs_16_rms", rel < 0.001,
                  fmt("rms16=%.6f rms24=%.6f rel=%.4f", rms16, rms24, rel));
        }
    }

    // ---- silence：必须**精确**等于 0（不是"约等于"） ----
    {
        AudioClip s;
        std::string es;
        if (s.load("audio/silence.wav", AudioClip::Mode::InMemory, es)) {
            bool allZero = true;
            for (float v : s.samples())
                if (v != 0.0f) { allZero = false; break; }
            check("wav_silence_exact_zero", allZero,
                  fmt("n=%.0f", static_cast<double>(s.samples().size())));
        } else {
            check("wav_silence_exact_zero", false, es);
        }
    }

    // ---- 单声道 / 不同采样率 ----
    {
        AudioClip m;
        std::string em;
        const bool okm = m.load("audio/tone_1k_mono.wav",
                                AudioClip::Mode::InMemory, em);
        check("wav_mono", okm && m.format().channels == 1 &&
                              m.format().frames == 24000 &&
                              m.format().sampleRate == 48000,
              okm ? fmt("frames=%.0f sr=%.0f ch=%.0f",
                        static_cast<double>(m.format().frames),
                        static_cast<double>(m.format().sampleRate),
                        static_cast<double>(m.format().channels))
                  : em);
    }

    // ---- 坏路径 / 坏魔数 / 不认识的扩展名：一条都不能崩 ----
    {
        AudioClip bad;
        std::string e;
        check("wav_missing_fails",
              !bad.load("audio/__nope__.wav", AudioClip::Mode::InMemory, e),
              e);
        check("wav_missing_reason", e == "open-failed", e);
    }
    {
        AudioClip bad;
        std::string e;
        // 这个扩展名故意不认（`.vaudio` 是"资产壳"，不是音频）
        check("unknown_ext_fails",
              !bad.load("audio/click.vaudio", AudioClip::Mode::InMemory, e),
              e);
        check("unknown_ext_reason", e == "unknown-extension", e);
    }

    // ---- Streaming 模式：顺序读 + 尾帧 ----
    {
        AudioClip st;
        std::string es;
        const bool oks = st.load("audio/tone_440_stereo.wav",
                                 AudioClip::Mode::Streaming, es);
        check("stream_open", oks, es);
        if (oks) {
            std::vector<float> buf(static_cast<std::size_t>(kMaxBlockFrames) * 2);
            std::uint64_t total = 0;
            for (;;) {
                const std::uint32_t got = st.readStreaming(buf.data(),
                                                           kMaxBlockFrames);
                if (got == 0) break;
                total += got;
            }
            check("stream_read_all", total == 44100,
                  fmt("frames=%.0f", static_cast<double>(total)));
            check("stream_resident_small",
                  st.residentBytes() < 16384,
                  fmt("bytes=%.0f", static_cast<double>(st.residentBytes())));
        }
    }
}

// ---------------------------------------------------------------- 混音

struct Loaded {
    AudioClip clip;
    bool ok = false;
    ClipView view() const {
        return ClipView{clip.data(), clip.format().frames,
                        clip.format().channels, clip.format().sampleRate};
    }
};

bool loadClip(Loaded& out, const char* path) {
    std::string err;
    out.ok = out.clip.load(path, AudioClip::Mode::InMemory, err);
    return out.ok;
}

// 渲染 seconds 秒到 out（交错立体声）
void renderSeconds(Mixer& m, float seconds, std::vector<float>& out) {
    const std::uint32_t block = 512;
    const std::uint32_t sr = m.outputSampleRate();
    const std::uint32_t total = static_cast<std::uint32_t>(seconds * sr);
    out.assign(static_cast<std::size_t>(total) * 2, 0.0f);
    std::uint32_t done = 0;
    std::vector<float> tmp(static_cast<std::size_t>(block) * 2);
    while (done < total) {
        const std::uint32_t n = std::min(block, total - done);
        m.renderOffline(tmp.data(), n);
        std::memcpy(out.data() + static_cast<std::size_t>(done) * 2, tmp.data(),
                    sizeof(float) * n * 2);
        done += n;
    }
}

double rmsChannel(const std::vector<float>& s, int ch) {
    double acc = 0.0;
    std::size_t n = 0;
    for (std::size_t i = static_cast<std::size_t>(ch); i < s.size(); i += 2) {
        acc += static_cast<double>(s[i]) * s[i];
        ++n;
    }
    return n ? std::sqrt(acc / static_cast<double>(n)) : 0.0;
}

int zeroCrossings(const std::vector<float>& s, int ch) {
    int n = 0;
    bool prevNeg = false;
    bool first = true;
    for (std::size_t i = static_cast<std::size_t>(ch); i < s.size(); i += 2) {
        const bool neg = s[i] < 0.0f;
        if (!first && neg != prevNeg) ++n;
        prevNeg = neg;
        first = false;
    }
    return n;
}

float maxAdjacentDiff(const std::vector<float>& s, int ch) {
    float worst = 0.0f;
    for (std::size_t i = static_cast<std::size_t>(ch) + 2; i < s.size(); i += 2) {
        const float d = std::fabs(s[i] - s[i - 2]);
        if (d > worst) worst = d;
    }
    return worst;
}

// 确定性：同参数两次渲染必须 memcmp 逐位相同（§8.2）
void testDeterminism(const Loaded& tone44, const Loaded& click) {
    std::printf("-- 混音：确定性（两次渲染 memcmp）\n");

    auto build = [&](Mixer& m) {
        m.init(48000, 2);
        m.setClip(1, tone44.view());
        m.setClip(2, click.view());
        const float lpos[3] = {0, 0, 0};
        const float fwd[3] = {0, 0, -1};
        const float up[3] = {0, 1, 0};
        m.setListener(lpos, fwd, up);
        PlayDesc d;
        d.clipId = 1;
        d.bus = 1;
        d.volume = 0.7f;
        d.loop = true;
        m.play(d);
        PlayDesc d2;
        d2.clipId = 2;
        d2.bus = 2;
        d2.spatial = true;
        d2.minDist = 1.0f;
        d2.maxDist = 10.0f;
        d2.pos[0] = 3.0f;   // 右侧
        d2.pos[2] = 2.0f;
        d2.pitch = 1.3f;
        d2.loop = true;
        m.play(d2);
    };

    const std::uint32_t block = 512;
    const int blocks = 200;   // 200 * 512 = 102400 帧 ≈ 2.1 s
    std::vector<float> a(static_cast<std::size_t>(block) * 2);
    std::vector<float> b(a.size());
    std::vector<float> outA, outB;
    outA.reserve(a.size() * blocks);
    outB.reserve(b.size() * blocks);

    {
        Mixer m1;
        build(m1);
        for (int i = 0; i < blocks; ++i) {
            m1.renderOffline(a.data(), block);
            outA.insert(outA.end(), a.begin(), a.end());
        }
    }
    {
        Mixer m2;
        build(m2);
        for (int i = 0; i < blocks; ++i) {
            m2.renderOffline(b.data(), block);
            outB.insert(outB.end(), b.begin(), b.end());
        }
    }

    const bool same = outA.size() == outB.size() &&
                      std::memcmp(outA.data(), outB.data(),
                                  sizeof(float) * outA.size()) == 0;
    check("mixer_deterministic_memcmp", same,
          fmt("frames=%.0f bytes=%.0f", static_cast<double>(outA.size() / 2),
              static_cast<double>(outA.size() * sizeof(float))));
    check("mixer_output_nonzero", rmsChannel(outA, 0) > 0.01,
          fmt("rmsL=%.4f", rmsChannel(outA, 0)));
}

// 过零率 + 音量线性 + 重采样
void testMixerBasics(const Loaded& tone44, const Loaded& tone48) {
    std::printf("-- 混音：过零率 / 音量线性 / 重采样\n");

    // ---- 过零率：1 s 440 Hz @44.1k，不重采样 ----
    {
        Mixer m;
        m.init(44100, 2);
        m.setClip(1, tone44.view());
        PlayDesc d;
        d.clipId = 1;
        d.bus = 2;
        d.volume = 1.0f;
        m.play(d);
        std::vector<float> out;
        renderSeconds(m, 1.0f, out);
        const int zc = zeroCrossings(out, 0);
        const double want = 880.0;
        const double rel = std::fabs(zc - want) / want;
        check("mixer_zero_crossings", rel < 0.01,
              fmt("zc=%.0f want=%.0f rel=%.4f", static_cast<double>(zc), want,
                  rel));
    }

    // ---- 音量线性：0.5 的 RMS 必须是 1.0 的 0.5 倍（1%）----
    {
        double rms[2] = {0.0, 0.0};
        const float vols[2] = {1.0f, 0.5f};
        for (int k = 0; k < 2; ++k) {
            Mixer m;
            m.init(44100, 2);
            m.setClip(1, tone44.view());
            PlayDesc d;
            d.clipId = 1;
            d.bus = 1;
            d.volume = vols[k];
            d.loop = true;
            m.play(d);
            std::vector<float> out;
            renderSeconds(m, 0.5f, out);
            rms[k] = rmsChannel(out, 0);
        }
        const double ratio = rms[0] > 1e-9 ? rms[1] / rms[0] : 0.0;
        check("mixer_volume_linear", std::fabs(ratio - 0.5) < 0.005,
              fmt("rms1=%.6f rms05=%.6f ratio=%.6f", rms[0], rms[1], ratio));
    }

    // ---- 重采样：44.1k clip 在 48k 设备上，48000 帧正好播完 ----
    {
        Mixer m;
        m.init(48000, 2);
        m.setClip(1, tone44.view());
        PlayDesc d;
        d.clipId = 1;
        d.bus = 2;
        m.play(d, nullptr, nullptr);
        std::vector<float> out(static_cast<std::size_t>(kMaxBlockFrames) * 2);
        // 48000 帧 * (44100/48000) = 44100 → 恰好在第 48000 帧之后用完。
        // 容差 1 帧：`pos` 是 double 累加，48000 * (44100.0/48000.0) 可能
        // 落在 44099.99999999999（差一个 ulp 就晚一帧结束），这属于正常。
        auto renderFrames = [&](int n) {
            for (int i = 0; i < n; i += 512) {
                const int k = std::min(512, n - i);
                m.renderOffline(out.data(), static_cast<std::uint32_t>(k));
            }
        };
        renderFrames(47999);
        const bool activeBefore = m.slotBusy(0);
        renderFrames(3);                              // 到 48002
        const bool endedAfter = !m.slotBusy(0);
        check("mixer_resample_exact_length", activeBefore && endedAfter,
              fmt("at47999=%.0f at48002=%.0f",
                  activeBefore ? 1.0 : 0.0, endedAfter ? 1.0 : 0.0));
    }

    // ---- 单声道素材：非空间播放时左右应完全相同 ----
    {
        Loaded mono;
        if (loadClip(mono, "audio/tone_1k_mono.wav")) {
            Mixer m;
            m.init(48000, 2);
            m.setClip(1, mono.view());
            PlayDesc d;
            d.clipId = 1;
            d.bus = 2;
            d.loop = true;
            m.play(d);
            std::vector<float> out;
            renderSeconds(m, 0.2f, out);
            const double l = rmsChannel(out, 0);
            const double r = rmsChannel(out, 1);
            check("mixer_mono_2d_centered",
                  std::fabs(l - r) / std::max(l, 1e-9) < 0.01,
                  fmt("L=%.6f R=%.6f", l, r));
        } else {
            check("mixer_mono_2d_centered", false, "tone_1k_mono 载入失败");
        }
    }
    (void)tone48;
}

// 总线：SFX=0 → SFX 消失、Music 不变（两条都要断言）
void testBuses(const Loaded& tone44) {
    std::printf("-- 混音：总线（SFX=0 而 Music 不变）\n");

    auto render = [&](float sfxGain, double* musicRms, double* sfxRms) {
        Mixer m;
        m.init(48000, 2);
        m.setClip(1, tone44.view());
        m.setClip(2, tone44.view());
        // 音乐：220 Hz 那条思路走不通（素材只有一个频率），改用"两条都放，
        // 但让 SFX 那条音量为 0 的对照"来分离 —— 见下面的减法。
        PlayDesc d;
        d.clipId = 1;
        d.bus = static_cast<std::uint32_t>(BusId::Music);
        d.volume = 0.5f;
        d.loop = true;
        m.play(d);
        m.setBusGain(static_cast<int>(BusId::SFX), sfxGain);
        m.setBusGain(static_cast<int>(BusId::Music), 1.0f);
        std::vector<float> out;
        renderSeconds(m, 0.3f, out);   // 先跑 50 ms 让增益平滑到位
        renderSeconds(m, 0.3f, out);
        *musicRms = rmsChannel(out, 0);
    };

    double musicOnly = 0.0, musicWithSfxMuted = 0.0;
    render(1.0f, &musicOnly, nullptr);            // SFX 增益 1（但没有 SFX 声部）
    render(0.0f, &musicWithSfxMuted, nullptr);    // SFX 增益 0
    check("bus_music_unaffected_by_sfx_mute",
          std::fabs(musicOnly - musicWithSfxMuted) / std::max(musicOnly, 1e-9) <
              0.01,
          fmt("music=%.6f muted=%.6f", musicOnly, musicWithSfxMuted));

    // ---- SFX 声部真的消失 ----
    {
        Mixer m;
        m.init(48000, 2);
        m.setClip(1, tone44.view());
        PlayDesc d;
        d.clipId = 1;
        d.bus = static_cast<std::uint32_t>(BusId::SFX);
        d.volume = 1.0f;
        d.loop = true;
        m.play(d);
        m.setBusGain(static_cast<int>(BusId::SFX), 0.0f);
        std::vector<float> out;
        renderSeconds(m, 0.4f, out);   // 平滑到位
        renderSeconds(m, 0.3f, out);   // 测这一段
        const double r = rmsChannel(out, 0);
        check("bus_sfx_zero_silences", r < 1e-6,
              fmt("rms=%.9f", r));
    }

    // ---- Master=0.5 → RMS 恰好 0.5 倍 ----
    {
        double ratio = 0.0;
        double a = 0.0, b = 0.0;
        for (int k = 0; k < 2; ++k) {
            Mixer m;
            m.init(48000, 2);
            m.setClip(1, tone44.view());
            PlayDesc d;
            d.clipId = 1;
            d.bus = static_cast<std::uint32_t>(BusId::Music);
            d.loop = true;
            m.play(d);
            m.setMasterGain(k == 0 ? 1.0f : 0.5f);
            std::vector<float> out;
            renderSeconds(m, 0.2f, out);
            renderSeconds(m, 0.3f, out);
            (k == 0 ? a : b) = rmsChannel(out, 0);
        }
        ratio = a > 1e-9 ? b / a : 0.0;
        check("bus_master_half", std::fabs(ratio - 0.5) < 0.005,
              fmt("rms1=%.6f rms05=%.6f ratio=%.6f", a, b, ratio));
    }

    // ---- mute：输出全 0，但声部仍在跑 ----
    {
        Mixer m;
        m.init(48000, 2);
        m.setClip(1, tone44.view());
        PlayDesc d;
        d.clipId = 1;
        d.bus = static_cast<std::uint32_t>(BusId::Music);
        d.loop = true;
        m.play(d);
        m.setMuted(true);
        std::vector<float> out;
        renderSeconds(m, 0.2f, out);
        float peak = 0.0f;
        for (float v : out) peak = std::max(peak, std::fabs(v));
        check("master_mute_zero_output", peak == 0.0f,
              fmt("peak=%.9f", static_cast<double>(peak)));
        check("master_mute_keeps_voices", m.stats().voices == 1,
              fmt("voices=%.0f", static_cast<double>(m.stats().voices)));
    }
}

// 声部池：64 + 第 65 个 → 抢最旧，且不爆音
void testVoicePool() {
    std::printf("-- 混音：声部池 64 / 抢最旧 / 削波保护\n");

    // 造一个"直流"素材：相邻样本差恒为 0，于是任何"突然切断"都会
    // 立刻表现为一个大的相邻样本差 —— 这正是我们要抓的"哒"。
    std::vector<float> dc(44100 * 2, 0.5f);

    {
        Mixer m;
        m.init(44100, 2);
        m.setClip(0, ClipView{dc.data(), 44100, 2, 44100});

        PlayDesc d;
        d.clipId = 0;
        d.bus = static_cast<std::uint32_t>(BusId::SFX);
        d.volume = 0.02f;   // 64 * 0.02 = 1.28 → 软削波到 <1，但不会一直饱和
        d.loop = true;
        int started = 0;
        for (int i = 0; i < kMaxVoices; ++i)
            if (m.play(d)) ++started;
        check("pool_64_started", started == kMaxVoices,
              fmt("started=%.0f", static_cast<double>(started)));

        std::vector<float> warm;
        renderSeconds(m, 0.05f, warm);
        check("pool_64_active", m.stats().voices == kMaxVoices,
              fmt("voices=%.0f", static_cast<double>(m.stats().voices)));

        // 第 65 个：应当抢掉最旧的那一个
        const bool ok65 = m.play(d);
        check("pool_65th_accepted", ok65, "");
        std::vector<float> tail;
        renderSeconds(m, 0.06f, tail);   // 覆盖 10 ms 淡出 + pending 接上
        check("pool_still_64", m.stats().voices == kMaxVoices,
              fmt("voices=%.0f", static_cast<double>(m.stats().voices)));
        check("pool_stolen_counted", m.stats().stolen == 1,
              fmt("stolen=%.0f", static_cast<double>(m.stats().stolen)));

        const float diff = maxAdjacentDiff(tail, 0);
        check("pool_steal_no_click", diff < 0.5,
              fmt("maxadj=%.6f", static_cast<double>(diff)));
    }

    // ---- 削波保护：64 路满幅 → |x| <= 1 ----
    {
        std::vector<float> full(44100 * 2, 1.0f);
        Mixer m;
        m.init(44100, 2);
        m.setClip(0, ClipView{full.data(), 44100, 2, 44100});
        PlayDesc d;
        d.clipId = 0;
        d.bus = static_cast<std::uint32_t>(BusId::SFX);
        d.volume = 1.0f;
        d.loop = true;
        for (int i = 0; i < kMaxVoices; ++i) m.play(d);
        std::vector<float> out;
        renderSeconds(m, 0.2f, out);
        float peak = 0.0f;
        for (float v : out) peak = std::max(peak, std::fabs(v));
        check("clip_protection_64x_full", peak <= 1.0f,
              fmt("peak=%.6f", static_cast<double>(peak)));
    }
}

// ---------------------------------------------------------------- 总线自动化

// 音量的 50 ms 线性平滑（R8）。
//
// ⚠ 为什么这条断言必须有：直接把增益从 1.0 改成 0.0，波形上就是一个
//   **半幅阶跃**（DC 素材上是 0.5 的邻差）。听感是"咔"的一声 —— 而这
//   恰恰是"用耳朵测不出来、用断言一眼看穿"的典型。
void testVolumeSmoothing(double* outStep, double* outSettled) {
    std::printf("-- 总线：音量平滑（50 ms 线性，无拉链噪声）\n");

    // DC 素材：相邻样本差恒为 0，于是"我们的增益在跳"会直接变成邻差
    std::vector<float> dc(48000 * 2, 0.5f);

    Mixer m;
    m.init(48000, 2);
    m.setClip(0, ClipView{dc.data(), 48000, 2, 48000});
    PlayDesc d;
    d.clipId = 0;
    d.bus = static_cast<std::uint32_t>(BusId::SFX);
    d.volume = 1.0f;
    d.loop = true;
    m.play(d);

    std::vector<float> warm;
    renderSeconds(m, 0.2f, warm);

    // 1.0 → 0.0：最狠的一步
    m.setBusGain(static_cast<int>(BusId::SFX), 0.0f);
    std::vector<float> ramp;
    renderSeconds(m, 0.05f, ramp);
    const float step = maxAdjacentDiff(ramp, 0);
    // 预期：0.5 / (0.05*48000) = 2.08e-4（每样本）。硬跳是 0.5。
    check("vol_smoothing_no_step", step < 0.01f,
          fmt("maxadj=%.6f", static_cast<double>(step)));

    std::vector<float> after;
    renderSeconds(m, 0.15f, after);
    const double settled = rmsChannel(after, 0);
    check("vol_smoothing_reaches_target", settled < 1e-4,
          fmt("rms=%.9f", settled));

    if (outStep) *outStep = static_cast<double>(step);
    if (outSettled) *outSettled = settled;
}

// ducking（§G14-5 / R7）：Voice 总线有声 → Music 往下压；释放后能回来。
void testDucking(const Loaded& tone) {
    std::printf("-- 总线：ducking（-12 dB / attack 50 ms / release 400 ms）\n");

    auto db = [](float g) {
        return 20.0 * std::log10(std::max(g, 1e-6f));
    };

    std::vector<float> out;

    // ---- ① 有声部 → Music 掉到 ≤ -6 dB；释放后回 ±0.5 dB ----
    {
        Mixer m;
        m.init(48000, 2);
        m.setClip(1, tone.view());
        PlayDesc dm;
        dm.clipId = 1;
        dm.bus = static_cast<std::uint32_t>(BusId::Music);
        dm.volume = 0.4f;
        dm.loop = true;
        m.play(dm);
        m.setBusGain(static_cast<int>(BusId::Music), 1.0f);
        m.setDuckDepth(0.75f);
        renderSeconds(m, 0.2f, out);   // 总线增益平滑到位
        const double before = db(m.stats().musicGain);
        check("duck_quiet_when_no_voice", std::fabs(before) < 0.5,
              fmt("db=%.3f", before));

        PlayDesc dv;
        dv.clipId = 1;
        dv.bus = static_cast<std::uint32_t>(BusId::Voice);
        dv.volume = 0.4f;
        dv.loop = true;
        std::uint32_t slot = 0, gen = 0;
        m.play(dv, &slot, &gen);
        renderSeconds(m, 0.1f, out);   // attack 50 ms → 100 ms 已到位
        const double ducked = db(m.stats().musicGain);
        check("duck_active_at_least_6db", ducked <= -6.0,
              fmt("db=%.3f", ducked));

        // 事件只在跨阈值时置一次
        float g = 0.0f;
        const bool ev1 = m.consumeDuckEvent(&g);
        const bool ev2 = m.consumeDuckEvent(&g);
        check("duck_event_once", ev1 && !ev2,
              fmt("gain=%.4f", static_cast<double>(g)));

        // 释放：停掉 Voice 声部，400 ms 后回到 ±0.5 dB
        m.stop(slot, gen, 10.0f);
        renderSeconds(m, 0.5f, out);
        const double released = db(m.stats().musicGain);
        check("duck_release_back_to_unity", std::fabs(released) < 0.5,
              fmt("db=%.3f", released));
    }

    // ---- ② 深度 0 = 关掉（反向断言：不能"永远 duck"）----
    {
        Mixer m;
        m.init(48000, 2);
        m.setClip(1, tone.view());
        PlayDesc dm;
        dm.clipId = 1;
        dm.bus = static_cast<std::uint32_t>(BusId::Music);
        dm.volume = 0.4f;
        dm.loop = true;
        m.play(dm);
        m.setDuckDepth(0.0f);
        PlayDesc dv;
        dv.clipId = 1;
        dv.bus = static_cast<std::uint32_t>(BusId::Voice);
        dv.volume = 0.4f;
        dv.loop = true;
        m.play(dv);
        renderSeconds(m, 0.3f, out);
        const double g = db(m.stats().musicGain);
        check("duck_disabled_when_depth_zero", std::fabs(g) < 0.5,
              fmt("db=%.3f", g));
    }

    // ---- ③ SFX 总线有声**不该** duck（只有 Voice 才压音乐）----
    {
        Mixer m;
        m.init(48000, 2);
        m.setClip(1, tone.view());
        PlayDesc dm;
        dm.clipId = 1;
        dm.bus = static_cast<std::uint32_t>(BusId::Music);
        dm.volume = 0.4f;
        dm.loop = true;
        m.play(dm);
        PlayDesc ds;
        ds.clipId = 1;
        ds.bus = static_cast<std::uint32_t>(BusId::SFX);
        ds.volume = 0.4f;
        ds.loop = true;
        m.play(ds);
        renderSeconds(m, 0.3f, out);
        const double g = db(m.stats().musicGain);
        check("duck_ignores_sfx_bus", std::fabs(g) < 0.5,
              fmt("db=%.3f", g));
    }
}

// ---------------------------------------------------------------- 3D

// 单路 3D 声源渲染一小段，返回交错立体声。
//
// ⚠ 为什么 3D 断言必须**真的渲染**、不能只测纯函数：`spatialGain` 对了
//   不代表 `renderVoice` 把它乘进去了 —— 少乘一次 gain、pan 写反左右，
//   纯函数测试全都绿，只有输出波形能抓。
void renderOneSpatial(std::vector<float>& out, const Loaded& clip,
                      const float pos[3], bool spatial, float maxDist,
                      float seconds = 0.2f) {
    Mixer m;
    m.init(48000, 2);
    m.setClip(0, clip.view());
    const float lp[3] = {0, 0, 0};
    const float fwd[3] = {0, 0, -1};
    const float up[3] = {0, 1, 0};
    m.setListener(lp, fwd, up);
    PlayDesc d;
    d.clipId = 0;
    d.bus = static_cast<std::uint32_t>(BusId::SFX);
    d.volume = 0.5f;
    d.loop = true;
    d.spatial = spatial;
    d.minDist = 1.0f;
    d.maxDist = maxDist;
    d.rolloff = 1.0f;
    d.pos[0] = pos[0];
    d.pos[1] = pos[1];
    d.pos[2] = pos[2];
    m.play(d);
    renderSeconds(m, seconds, out);
}

void testSpatial() {
    std::printf("-- 3D：距离衰减 / 收尾斜坡 / 等功率 pan\n");

    // ---- 衰减曲线（纯函数）----
    const float kMin = 1.0f, kMax = 50.0f, kRoll = 1.0f;
    check("spatial_gain_at_min", std::fabs(spatialGain(0.5f, kMin, kMax, kRoll) - 1.0f) < 1e-6f,
          fmt("g=%.6f", spatialGain(0.5f, kMin, kMax, kRoll)));
    check("spatial_gain_at_maxdist", spatialGain(kMax, kMin, kMax, kRoll) == 0.0f,
          fmt("g=%.6f", spatialGain(kMax, kMin, kMax, kRoll)));
    check("spatial_gain_beyond_max", spatialGain(kMax * 2.0f, kMin, kMax, kRoll) == 0.0f, "");

    {
        bool mono = true;
        float prev = 2.0f;
        for (float d = kMin; d <= kMax; d += 0.5f) {
            const float g = spatialGain(d, kMin, kMax, kRoll);
            if (g > prev + 1e-7f) mono = false;
            prev = g;
        }
        check("spatial_gain_strictly_decreasing", mono, "");
    }

    // ⚠⚠ 这条是**收尾斜坡**的守卫：没有 fadeOut 时
    //   `g(49.999) = 1/(1+48.999) = 0.0200`，跨过 maxDist 直接跳 0 →
    //   波形上是一个阶跃（听感"啪"）。有 fade 时约 4e-6。
    {
        const float near = spatialGain(kMax - 0.001f, kMin, kMax, kRoll);
        check("spatial_no_step_at_maxdist", near < 1e-4f,
              fmt("g=%.3e", near));
        // ⚠ 别猜绝对阈值：fade 区里 `inv` 本身已经很小，绝对值只能说明
        //   "小"，说明不了"斜坡真的接上了"。直接跟**纯倒数**比比值 ——
        //   d = 47.5 落在 [45, 50] 区间的正中 → 比值应当 ≈ 0.5。
        const float d95 = kMax * 0.95f;
        const float invOnly = kMin / (kMin + kRoll * (d95 - kMin));
        const float withFade = spatialGain(d95, kMin, kMax, kRoll);
        const float ratio = withFade / invOnly;
        check("spatial_fade_engaged_after_90pct",
              std::fabs(ratio - 0.5f) < 0.02f,
              fmt("ratio=%.4f with=%.4f", static_cast<double>(ratio),
                  static_cast<double>(withFade)));
    }

    // 每 0.01 m 扫一遍：不存在 > 0.02 的邻差（硬截断会给出 0.02 的阶跃）
    {
        float worst = 0.0f;
        float prev = spatialGain(0.0f, kMin, kMax, kRoll);
        for (float d = 0.01f; d <= 60.0f; d += 0.01f) {
            const float g = spatialGain(d, kMin, kMax, kRoll);
            worst = std::max(worst, std::fabs(g - prev));
            prev = g;
        }
        check("spatial_sweep_no_jump", worst < 0.02f,
              fmt("maxjump=%.6f", static_cast<double>(worst)));
    }

    // ---- 等功率 pan（纯函数）----
    {
        float l = 0.0f, r = 0.0f;
        equalPowerPan(0.0f, &l, &r);
        check("pan_center_is_half_power",
              std::fabs(l - 0.70710678f) < 1e-4f && std::fabs(r - 0.70710678f) < 1e-4f,
              fmt("l=%.6f r=%.6f", static_cast<double>(l), static_cast<double>(r)));
        // ⚠ 线性 pan 在这里给 l=r=0.5 → 功率 0.5，正中 -3 dB 塌陷。
        check("pan_center_power_unity", std::fabs(l * l + r * r - 1.0f) < 1e-5f,
              fmt("pow=%.6f", static_cast<double>(l * l + r * r)));
    }
    {
        float l = 0.0f, r = 0.0f;
        equalPowerPan(-1.0f, &l, &r);
        check("pan_left_full", std::fabs(l - 1.0f) < 1e-5f && std::fabs(r) < 1e-5f,
              fmt("l=%.6f r=%.6f", static_cast<double>(l), static_cast<double>(r)));
        equalPowerPan(1.0f, &l, &r);
        check("pan_right_full", std::fabs(r - 1.0f) < 1e-5f && std::fabs(l) < 1e-5f,
              fmt("l=%.6f r=%.6f", static_cast<double>(l), static_cast<double>(r)));
        // 越界必须夹住（传 -3 不能变成 cos(负数大角度) 的乱值）
        equalPowerPan(-3.0f, &l, &r);
        check("pan_clamped_below", std::fabs(l - 1.0f) < 1e-5f, "");
        equalPowerPan(3.0f, &l, &r);
        check("pan_clamped_above", std::fabs(r - 1.0f) < 1e-5f, "");
    }
    {
        float worst = 0.0f;
        for (float x = -1.0f; x <= 1.0f; x += 0.01f) {
            float l = 0.0f, r = 0.0f;
            equalPowerPan(x, &l, &r);
            worst = std::max(worst, std::fabs(l * l + r * r - 1.0f));
        }
        check("pan_constant_power_sweep", worst < 1e-5f,
              fmt("worst=%.3e", static_cast<double>(worst)));
    }

    // ---- evaluateSpatial：panX 的几何方向 ----
    {
        const float lp[3] = {0, 0, 0};
        const float right[3] = {1, 0, 0};   // forward=-Z / up=+Y 时的右向量
        float g = 0.0f, px = 0.0f;
        const float left[3] = {-5, 0, 0};
        evaluateSpatial(left, lp, right, kMin, kMax, kRoll, &g, &px);
        check("eval_left_panx_negative", px < -0.5f,
              fmt("panx=%.4f", static_cast<double>(px)));
        const float rightSrc[3] = {5, 0, 0};
        evaluateSpatial(rightSrc, lp, right, kMin, kMax, kRoll, &g, &px);
        check("eval_right_panx_positive", px > 0.5f,
              fmt("panx=%.4f", static_cast<double>(px)));
        const float front[3] = {0, 0, -5};
        evaluateSpatial(front, lp, right, kMin, kMax, kRoll, &g, &px);
        check("eval_front_panx_center", std::fabs(px) < 1e-5f,
              fmt("panx=%.6f", static_cast<double>(px)));
        const float back[3] = {0, 0, 5};
        evaluateSpatial(back, lp, right, kMin, kMax, kRoll, &g, &px);
        check("eval_back_panx_center", std::fabs(px) < 1e-5f,
              fmt("panx=%.6f", static_cast<double>(px)));
    }

    // ---- 端到端：真的渲染出来看左右声道 ----
    Loaded mono;
    if (!loadClip(mono, "audio/tone_1k_mono.wav")) {
        check("spatial_asset_loaded", false, "缺 audio/tone_1k_mono.wav");
        return;
    }

    std::vector<float> buf;

    {
        const float p[3] = {-8.0f, 0.0f, 0.0f};
        renderOneSpatial(buf, mono, p, true, kMax);
        const double l = rmsChannel(buf, 0);
        const double r = rmsChannel(buf, 1);
        check("spatial_left_source_left_louder", l > r * 1.5,
              fmt("l=%.4f r=%.4f", l, r));
    }
    {
        const float p[3] = {0.0f, 0.0f, -8.0f};
        renderOneSpatial(buf, mono, p, true, kMax);
        const double l = rmsChannel(buf, 0);
        const double r = rmsChannel(buf, 1);
        const double denom = std::max(l, r);
        check("spatial_front_balanced",
              denom > 0.001 && std::fabs(l - r) / denom < 0.05,
              fmt("l=%.4f r=%.4f", l, r));
    }
    {
        const float p[3] = {0.0f, 0.0f, 8.0f};
        renderOneSpatial(buf, mono, p, true, kMax);
        const double l = rmsChannel(buf, 0);
        const double r = rmsChannel(buf, 1);
        const double denom = std::max(l, r);
        check("spatial_back_balanced",
              denom > 0.001 && std::fabs(l - r) / denom < 0.05,
              fmt("l=%.4f r=%.4f", l, r));
    }
    {
        const float nearP[3] = {0.0f, 0.0f, -1.0f};
        const float farP[3] = {0.0f, 0.0f, -30.0f};
        renderOneSpatial(buf, mono, nearP, true, kMax);
        const double nearRms = rmsChannel(buf, 0);
        renderOneSpatial(buf, mono, farP, true, kMax);
        const double farRms = rmsChannel(buf, 0);
        check("spatial_near_louder_than_far",
              farRms > 1e-6 && nearRms > farRms * 20.0,
              fmt("near=%.4f far=%.6f", nearRms, farRms));
    }

    // ⚠ 反向断言：`spatial=false` 时距离必须**完全不参与**（否则 2D 音效
    //   会被摄像机距离莫名其妙地调小 —— 这是最容易写漏的一路）。
    {
        const float nearP[3] = {0.0f, 0.0f, -1.0f};
        const float farP[3] = {0.0f, 0.0f, -40.0f};
        renderOneSpatial(buf, mono, nearP, false, kMax);
        const double a = rmsChannel(buf, 0);
        renderOneSpatial(buf, mono, farP, false, kMax);
        const double b = rmsChannel(buf, 0);
        check("spatial_off_distance_independent",
              a > 1e-6 && std::fabs(a - b) / a < 0.01,
              fmt("d1=%.4f d40=%.4f", a, b));
        // 且左右必须对称（2D 音效不该被 pan）
        renderOneSpatial(buf, mono, farP, false, kMax);
        const double l = rmsChannel(buf, 0);
        const double r = rmsChannel(buf, 1);
        check("spatial_off_no_pan", std::fabs(l - r) / std::max(l, r) < 0.01,
              fmt("l=%.4f r=%.4f", l, r));
    }

    // ---- updateVoice：声源移动必须被听见（每帧改位置）----
    //
    // ⚠ 符号约定（写死在这里，别再靠记忆）：listener 在原点、forward=-Z、
    //   up=+Y → 右向量 = forward × up = **+X**。于是
    //   **声源在 -X（左）→ L 更响 → `rmsL - rmsR > 0`**。
    {
        Mixer m;
        m.init(48000, 2);
        m.setClip(0, mono.view());
        const float lp[3] = {0, 0, 0};
        const float fwd[3] = {0, 0, -1};
        const float up[3] = {0, 1, 0};
        m.setListener(lp, fwd, up);
        PlayDesc d;
        d.clipId = 0;
        d.volume = 0.5f;
        d.loop = true;
        d.spatial = true;
        d.minDist = 1.0f;
        d.maxDist = kMax;
        d.pos[0] = -8.0f;
        std::uint32_t slot = 0, gen = 0;
        m.play(d, &slot, &gen);
        renderSeconds(m, 0.1f, buf);
        const double before = rmsChannel(buf, 0) - rmsChannel(buf, 1);

        const float moved[3] = {8.0f, 0.0f, 0.0f};
        m.updateVoice(slot, gen, moved, 0.5f);
        renderSeconds(m, 0.1f, buf);
        const double after = rmsChannel(buf, 0) - rmsChannel(buf, 1);
        check("spatial_move_flips_balance", before > 0.01 && after < -0.01,
              fmt("before=%.4f after=%.4f", before, after));

        // 过期代际的 update 必须被丢弃（槽位复用后的 ABA）。
        // 反向断言：若它被误接受，声源会"跳回左边"，差值变回正。
        const float bogus[3] = {-8.0f, 0.0f, 0.0f};
        m.updateVoice(slot, gen + 99u, bogus, 0.5f);
        renderSeconds(m, 0.1f, buf);
        const double stale = rmsChannel(buf, 0) - rmsChannel(buf, 1);
        check("spatial_stale_update_dropped", stale < -0.01,
              fmt("diff=%.4f", stale));
    }
}

// ---------------------------------------------------------------- 流式

// 流式音乐（§3.8 / G14-6）：
//   环形缓冲的**回绕不在音频线程里** —— `pos` 是绝对帧号，主线程顺序解码
//   并按 `abs % ringFrames` 写入。于是"循环无缝"是结构性的，不是靠特判。
void testStreaming() {
    std::printf("-- 流式：环形缓冲 / 回绕 / underrun / 常驻内存\n");

    // ---- ① Mixer 层：合成环形缓冲，跑满 3 圈 ----
    {
        constexpr std::uint32_t kRing = 4800;   // 100 ms @48k
        constexpr std::uint32_t kCh = 2;
        std::vector<float> ring(static_cast<std::size_t>(kRing) * kCh);
        // 100 Hz 正弦，480 样本一周期 → kRing 恰好 10 个整周期，
        // 回绕处天然连续（这正是"无缝循环素材"的定义）。
        for (std::uint32_t i = 0; i < kRing; ++i) {
            const float v =
                0.8f * std::sin(2.0f * 3.14159265358979f * 100.0f *
                                static_cast<float>(i) / 48000.0f);
            ring[static_cast<std::size_t>(i) * 2 + 0] = v;
            ring[static_cast<std::size_t>(i) * 2 + 1] = v;
        }
        std::atomic<std::uint64_t> written{kRing};
        std::atomic<std::uint64_t> read{0};
        StreamView sv;
        sv.ring = ring.data();
        sv.ringFrames = kRing;
        sv.channels = kCh;
        sv.sampleRate = 48000;
        sv.totalFrames = kRing;
        sv.absWritten = &written;
        sv.absRead = &read;

        Mixer m;
        m.init(48000, 2);
        m.setStream(1, sv);
        PlayDesc d;
        d.streamId = 1;
        d.bus = static_cast<std::uint32_t>(BusId::Music);
        d.loop = true;
        m.play(d);

        // 手动分块渲染：每块之前"把缓冲填满"（就是主线程 fillStream 干的事）。
        // ⚠ 用 `renderSeconds` 一把渲染是不对的 —— 那是 3 秒后才填一次，
        //   中途 `absWritten` 追不上 → 全是 underrun，测不到回绕。
        const std::uint32_t block = 512;
        const std::uint32_t wanted = kRing * 3;   // 3 圈
        std::vector<float> out(static_cast<std::size_t>(wanted) * 2, 0.0f);
        std::vector<float> tmp(static_cast<std::size_t>(block) * 2);
        std::uint32_t done = 0;
        while (done < wanted) {
            written.store(read.load() + kRing, std::memory_order_relaxed);
            const std::uint32_t n = std::min(block, wanted - done);
            m.renderOffline(tmp.data(), n);
            std::memcpy(out.data() + static_cast<std::size_t>(done) * 2,
                        tmp.data(), sizeof(float) * n * 2);
            done += n;
        }

        check("stream_three_loops_playing", m.stats().voices == 1,
              fmt("voices=%.0f", static_cast<double>(m.stats().voices)));
        check("stream_no_underrun", m.stats().underruns == 0,
              fmt("underrun=%.0f", static_cast<double>(m.stats().underruns)));
        // 100 Hz @0.8 的最大每样本斜率 = 0.8*2π*100/48000 = 0.0105
        const float jump = maxAdjacentDiff(out, 0);
        check("stream_wrap_no_jump", jump < 0.05f,
              fmt("maxjump=%.6f", static_cast<double>(jump)));
        // 回绕点两侧也必须连续（把 2 个回绕点单独挑出来看）
        check("stream_wrap_points_continuous",
              std::fabs(out[static_cast<std::size_t>(kRing) * 2] -
                        out[static_cast<std::size_t>(kRing) * 2 - 2]) < 0.05f &&
                  std::fabs(out[static_cast<std::size_t>(kRing) * 4] -
                            out[static_cast<std::size_t>(kRing) * 4 - 2]) < 0.05f,
              "");
    }

    // ---- ② 缓冲没写满（主线程卡住）：输出必须**精确 0**，不能回绕到旧数据 ----
    //
    // ⚠ 这条是"`pos` 不能在音频线程里减回去"的**行为级**守卫：
    //   如果实现成 `pos -= ringFrames`，读到的是环形缓冲里**已经被覆盖**
    //   的槽位 → 输出的是旧声音（非 0），而且听起来像"卡带循环一小段"。
    {
        constexpr std::uint32_t kRing = 4096;
        std::vector<float> ring(static_cast<std::size_t>(kRing) * 2, 0.5f);
        std::atomic<std::uint64_t> written{1000};   // 只有前 1000 帧有效
        std::atomic<std::uint64_t> read{0};
        StreamView sv;
        sv.ring = ring.data();
        sv.ringFrames = kRing;
        sv.channels = 2;
        sv.sampleRate = 48000;
        sv.totalFrames = 1ull << 40;   // 假装文件无限长
        sv.absWritten = &written;
        sv.absRead = &read;

        Mixer m;
        m.init(48000, 2);
        m.setStream(1, sv);
        PlayDesc d;
        d.streamId = 1;
        d.bus = static_cast<std::uint32_t>(BusId::Music);
        d.loop = true;
        m.play(d);
        std::vector<float> out;
        renderSeconds(m, 0.1f, out);   // 4800 帧，够跨过 1000

        float leak = 0.0f;
        for (std::size_t i = static_cast<std::size_t>(1200) * 2; i < out.size(); ++i)
            leak = std::max(leak, std::fabs(out[i]));
        check("stream_underrun_silent_not_wrapped", leak == 0.0f,
              fmt("leak=%.6f", static_cast<double>(leak)));
        check("stream_underrun_counted", m.stats().underruns > 0,
              fmt("underrun=%.0f", static_cast<double>(m.stats().underruns)));
    }

    // ---- ③ 流式 clip **不进** AudioClipCache ----
    {
        AudioClipCache cache;
        std::string e1, e2;
        auto a = cache.get("audio/loop_2s.wav", AudioClip::Mode::Streaming, e1);
        auto b = cache.get("audio/loop_2s.wav", AudioClip::Mode::Streaming, e2);
        check("stream_not_cached",
              a && b && a.get() != b.get() && cache.count() == 0,
              fmt("count=%.0f", static_cast<double>(cache.count())));
    }

    // ---- ④ AudioSystem 端到端：常驻内存只跟"环形缓冲"有关 ----
    {
        AudioSystem sys;
        AudioSettings st;
        st.forceNull = true;
        sys.init(st);
        if (!sys.running()) {
            check("stream_system_running", false, "NullBackend 没起来");
        } else {
            const bool ok = sys.playMusic("audio/loop_2s.wav", 0.5f);
            check("stream_play_music", ok, "");

            // 环形缓冲 = 3 s × sr × ch × 4 B。2 s 的素材也是 3 s 的缓冲
            // （缓冲是**固定**的，不随文件长度变）—— 这正是"4 分钟的歌
            // 只占 1.1 MB"的全部理由。
            const std::uint64_t resident = sys.streamResidentBytes();
            const double ringFrames =
                static_cast<double>(resident) / (2.0 * 4.0);
            check("stream_resident_is_ring_only",
                  resident > 0 && resident < 2ull * 1024 * 1024,
                  fmt("bytes=%.0f frames=%.0f",
                      static_cast<double>(resident), ringFrames));
            check("stream_ring_is_three_seconds",
                  std::fabs(ringFrames / 44100.0 - 3.0) < 0.05,
                  fmt("sec=%.3f", ringFrames / 44100.0));
            // ⚠ 别拿"短文件的 InMemory"跟流式比大小：3 s 的环形缓冲是**固定**
            //   开销（1.06 MB），比 1 s 的音效（353 KB）还大 —— 这是对的行为，
            //   而且正好说明"短音效不该用流式"。有意义的断言是**不随文件变长**：
            //   同一路流跑下去，常驻内存一个字节都不涨。
            const std::uint64_t resident2 = sys.streamResidentBytes();
            for (int i = 0; i < 40; ++i) sys.update(1.0f / 60.0f);
            check("stream_resident_constant",
                  sys.streamResidentBytes() == resident2,
                  fmt("before=%.0f after=%.0f",
                      static_cast<double>(resident2),
                      static_cast<double>(sys.streamResidentBytes())));

            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            const double cur = sys.musicPositionSeconds();
            check("stream_position_advances", cur > 0.0,
                  fmt("cur=%.3f", cur));
            check("stream_seek_ok", sys.seekMusic(0.5), "");
            sys.stopMusic();
            check("stream_stop_releases", sys.streamResidentBytes() == 0,
                  fmt("bytes=%.0f",
                      static_cast<double>(sys.streamResidentBytes())));
            sys.shutdown();
        }
    }

    // ---- ⑤ 长文件对照（10 分钟 wav；fixture 不在就跳过）----
    //
    // ⚠ 这一组是"流式真的在流"的唯一**直接**证据：同一个文件，
    //   流式常驻 ≈ 1 MB，全解码 > 100 MB。没有对照的话，"内存小"
    //   也可能只是"根本没读到"。
    {
        // ⚠ 先用 ifstream 探存在性，别直接 load：load 失败会打一行
        //   `AUDIO-CLIP-FAIL`，而"fixture 不在"是正常状态 —— 每跑一次自检
        //   就刷一条 WARN，日志就没法看了。
        std::ifstream probeFile(
            assets::resolveAssetPath("audio/_long_10min.wav"), std::ios::binary);
        const bool have = probeFile.good();
        probeFile.close();
        if (!have) {
            // 不算失败（仓库里不提交 105 MB 的文件），但必须在日志里留痕 ——
            // `verify_audio.py` 会先生成 fixture，然后把这条当成失败。
            std::printf("  [SKIP] %-40s %s\n", "stream_long_fixture",
                        "缺 audio/_long_10min.wav"
                        "（tools/gen_audio_assets.py --long …）");
        } else {
            AudioClip probe;
            std::string err;
            if (!probe.load("audio/_long_10min.wav",
                            AudioClip::Mode::Streaming, err)) {
                check("stream_long_load", false, err);
                return;
            }
            const std::uint64_t sBytes = probe.residentBytes();
            const double seconds = static_cast<double>(probe.format().frames) /
                                   static_cast<double>(probe.format().sampleRate);
            check("stream_long_seconds", seconds > 590.0,
                  fmt("sec=%.1f", seconds));
            check("stream_long_resident_small", sBytes < 2ull * 1024 * 1024,
                  fmt("bytes=%.0f", static_cast<double>(sBytes)));

            // 对照组：整个解码进内存
            AudioClip full;
            std::string err2;
            const bool okFull = full.load("audio/_long_10min.wav",
                                          AudioClip::Mode::InMemory, err2);
            const std::uint64_t fBytes = full.residentBytes();
            check("inmem_long_resident_large",
                  okFull && fBytes > 100ull * 1024 * 1024,
                  fmt("bytes=%.0f", static_cast<double>(fBytes)));
            check("stream_long_vs_inmem_ratio",
                  okFull && sBytes > 0 && fBytes > sBytes * 50ull,
                  fmt("ratio=%.0f",
                      sBytes ? static_cast<double>(fBytes) /
                                   static_cast<double>(sBytes)
                             : 0.0));
        }
    }
}

// ---------------------------------------------------------------- 设备

void testDevice() {
    std::printf("-- 设备：NullBackend 线程 / AudioSystem 状态机\n");

    // ---- NullBackend 线程模式（只做冒烟：有时钟参与，不参与确定性断言）----
    {
        NullBackend nb;
        OpenParams p;
        p.blockFrames = 256;
        std::string err;
        const bool ok = nb.open(p, [](void*, float* out, std::uint32_t n,
                                      std::uint32_t ch) {
            for (std::uint32_t i = 0; i < n * ch; ++i) out[i] = 0.0f;
        }, nullptr, err);
        check("backend_thread_open", ok, err);
        nb.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        const std::uint64_t n = nb.blocksPumped();
        nb.stop();
        check("backend_thread_pumped", n > 0,
              fmt("blocks=%.0f", static_cast<double>(n)));
    }

    // ---- 设备打不开也不能崩 ----
    {
        AudioSystem sys;
        AudioSettings st;
        st.device = "__none__";
        st.forceNull = false;
        sys.init(st);
        check("device_none_not_running", !sys.running(), "");
        check("device_none_play_is_noop",
              !sys.playOneShot("audio/click.wav", BusId::SFX, nullptr),
              "");
        sys.update(0.016f);   // 不能崩
        sys.shutdown();
        check("device_none_shutdown_ok", !sys.running(), "");
    }

    // ---- 关掉音频：一个 block 都不混 ----
    {
        AudioSystem sys;
        AudioSettings st;
        st.enabled = false;
        sys.init(st);
        check("audio_disabled_not_running", !sys.running(), "");
        check("audio_disabled_no_play",
              !sys.playOneShot("audio/click.wav", BusId::SFX, nullptr), "");
        sys.shutdown();
    }

    // ---- Null 后端跑通整条链路 ----
    {
        AudioSystem sys;
        AudioSettings st;
        st.forceNull = true;
        st.enabled = true;
        st.master = 1.0f;
        st.sfx = 1.0f;
        sys.init(st);
        check("nullsystem_running", sys.running(), sys.backendName());
        const float lp[3] = {0, 0, 0};
        const float fw[3] = {0, 0, -1};
        const float up[3] = {0, 1, 0};
        sys.setListener(lp, fw, up);
        const bool played = sys.playOneShot("audio/tone_440_stereo.wav",
                                            BusId::SFX, nullptr, 1.0f);
        check("nullsystem_play_ok", played, "");
        // 让设备线程真的混几块
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        const MixStats stt = sys.stats();
        check("nullsystem_voices", stt.voices >= 1,
              fmt("voices=%.0f", static_cast<double>(stt.voices)));
        sys.stopAll();
        sys.shutdown();
        check("nullsystem_shutdown", !sys.running(), "");
    }

    // ---- 同一路径加载两次是同一个对象（D4）----
    {
        AudioClipCache cache;
        std::string e1, e2;
        auto a = cache.get("audio/tone_440_stereo.wav",
                           AudioClip::Mode::InMemory, e1);
        auto b = cache.get("audio/tone_440_stereo.wav",
                           AudioClip::Mode::InMemory, e2);
        check("cache_same_pointer", a && b && a.get() == b.get(),
              fmt("count=%.0f", static_cast<double>(cache.count())));
        // 不同 mode 必须是不同对象（否则 42 MB 的曲子会整个进内存）
        auto c = cache.get("audio/tone_440_stereo.wav",
                           AudioClip::Mode::Streaming, e2);
        check("cache_mode_separated", a && c && a.get() != c.get(),
              fmt("count=%.0f", static_cast<double>(cache.count())));
    }
}

} // namespace

int runAudioSelfTest() {
    g_pass = 0;
    g_fail = 0;
    std::printf("=== AUDIO 自检 ===\n");
    testApi();
    testNullBackend();
    testDecode();

    Loaded tone44, tone48, click;
    if (loadClip(tone44, "audio/tone_440_stereo.wav") &&
        loadClip(tone48, "audio/tone_440_48k.wav") &&
        loadClip(click, "audio/click.wav")) {
        testDeterminism(tone44, click);
        testMixerBasics(tone44, tone48);
        testBuses(tone44);
        testVolumeSmoothing(nullptr, nullptr);
        testDucking(tone44);
        testVoicePool();
        testSpatial();
        testStreaming();
    } else {
        check("mixer_assets_loaded", false, "音频素材没准备好（先跑 gen_audio_assets.py）");
    }

    testDevice();

    std::printf("AUDIO-TOTAL pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail;
}

} // namespace audio
