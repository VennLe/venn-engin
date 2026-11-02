#pragma once
// ============================================================
// engine/audio/AudioSystem —— 门面（每 App 一份，栈上成员，**不做单例**）
//
// 分工（docs/audio_roadmap.md §4.2 / §4.3）：
//   主线程  AudioSystem::update(dt)
//             ① drain 音频线程的日志 / duck 事件 → VK_LOG
//             ② 把调用方喂进来的声源状态写进 Mixer 的命令队列
//             ③ listener = 相机
//             ④ 流式 clip 预读（填到 3 s）
//             ⑤ 每 60 帧打 AUDIO-STATS
//   音频线程  Mixer::mixBlock()
//
// ⚠ 三条不可越界
//   1. **音频永不写世界**：这个类只读 `SourceUpdate`，一个写接口都没有。
//      `verify_hud` 的"暂停时位置逐位不变"就是靠这条活着（§3.6）。
//   2. 设备打开失败**不是错误**：进入 `Silent`，所有 API 变空操作，
//      `AUDIO-DEVFAIL` 只打一次（不刷屏），之后不再重试（除非开 RETRY）。
//   3. `engine/audio/**` 不许 include `runtime/` / `editor/`。
// ============================================================

#include "audio/AudioBackend.h"
#include "audio/AudioClipCache.h"
#include "audio/Mixer.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace audio {

// 出厂设置（来自 game.json 的 `audio` 块 + 环境变量；见 G14-5）
struct AudioSettings {
    bool enabled = true;
    std::string device = "default";
    float master = 1.0f;
    float music = 0.7f;
    float sfx = 1.0f;
    float voice = 1.0f;
    bool mute = false;
    bool muteOnFocusLost = false;
    // ducking 深度：Voice 总线有活跃声部时 Music 往下压多少（0 = 关掉，
    // 0.75 ≈ -12 dB）。语义见 Mixer::setDuckDepth。
    float duckDepth = 0.75f;
    int maxVoices = kMaxVoices;
    // 无可用设备时每 2 s 重试一次（拔插耳机）；默认关（避免每帧刷日志）
    bool retryOnFailure = false;
    // 强制用 NullBackend 而不是 WASAPI（自检 / 无声卡机器）
    bool forceNull = false;
};

// 一个声源的"当前状态"（调用方每帧喂进来；AudioSystem 只读）
struct SourceUpdate {
    std::uint64_t id = 0;          // 稳定 id（实体句柄），用来记住"这一声是不是已经在播"
    std::string clip;              // 相对资产根的路径
    int bus = static_cast<int>(BusId::SFX);
    bool loop = false;
    bool spatial = false;
    bool playing = true;           // playOnAwake / 播放中
    float volume = 1.0f;
    float pitch = 1.0f;
    float minDist = 1.0f;
    float maxDist = 50.0f;
    float rolloff = 1.0f;
    float pos[3] = {0.0f, 0.0f, 0.0f};
};

class AudioSystem {
public:
    AudioSystem();
    ~AudioSystem();

    // ---- 生命周期 ----
    // 打 AUDIO-INIT（或 AUDIO-DEVFAIL）。**不抛异常**：没声卡是正常工况。
    void init(const AudioSettings& settings);
    void shutdown();
    bool running() const { return m_state == State::Running; }
    const AudioSettings& settings() const { return m_settings; }

    // ---- 主线程每帧 ----
    void update(float dt);

    // listener（相机）。spatial 声源以此为准。
    void setListener(const float pos[3], const float forward[3],
                     const float up[3]);

    // 声源同步：把场景里**所有** AudioSourceComponent 的当前状态喂进来。
    // ⚠ 只读 —— 这里改不到任何东西（§3.6 的硬约束）。
    void applySources(const SourceUpdate* srcs, std::size_t count);

    // ---- 播放 ----
    // 一次性音效。pos 非空 = 3D。返回 false = 没播（设备不可用 / 素材读不出来）。
    bool playOneShot(const std::string& clipPath, BusId bus = BusId::SFX,
                     const float* pos = nullptr, float volume = 1.0f,
                     float pitch = 1.0f);
    // 流式音乐（3 s 环形缓冲）。同一时刻只有一路（§3.8：流式是音乐专用）。
    bool playMusic(const std::string& clipPath, float volume = 1.0f);
    void stopMusic();
    // 跳到第 seconds 秒（循环音乐的"选段"）。**清空环形缓冲 + 重置 cursor**，
    // 旧声部淡出 5 ms 后由新声部接上（不是"瞬间切"，那样是"咔"）。
    // 返回 false = 当前没有流在播 / seek 失败（超出文件尾）。
    bool seekMusic(double seconds);
    // 当前播放位置（秒）。来自音频线程回报的 `absRead`，只用于日志 / 断言。
    double musicPositionSeconds() const;
    bool musicPlaying() const { return m_stream.active; }
    void stopAll();

    // ---- 语音（D1，2026-10-31）----
    //
    // 缺口原话（`docs/remaining_work_2026-10-27.md` §2.4 D1）：
    //   "`startVoice` 返回句柄，但**无「播完了」查询/回调**"
    //   → "语音播完再进下一句"做不了 → 卡住 C2 的语音版。
    //
    // 为什么不是"给个回调"：这一层的调用方是 `DialogueSystem` / 脚本，
    // 它们都是**每帧轮询**的推进模型（没有事件总线）。回调会把
    // "音频线程何时结束"变成"主线程要重入剧本推进"—— 那是最容易出
    // 重入 bug 的形状。所以给的是**两个可轮询的量**：
    //   `voicePlaying()` 播没播完
    //   `voiceSerial()`  换没换句（宿主据此知道"该等的是新那一句"）
    //
    // 播一句语音（`BusId::Voice`）。返回 false = 设备没开 / 素材读不出来
    // —— 与 `playOneShot` 同语义，**不抛异常**。
    bool playVoice(const std::string& clipPath, float volume = 1.0f);

    // 语音还在播吗。
    // ⚠⚠ 刚 `playVoice()` 之后的约一个 mix block 里，音频线程还没把声部
    //   标成 active —— 直接读 `activeVoicesOnBus` 会得到 0，宿主会在
    //   **第一帧**就判定"播完了"然后立刻跳到下一句（整段语音听不见）。
    //   所以这里带一个宽限期：最近 `kVoiceGraceMs` 内发起过就算在播。
    bool voicePlaying() const;

    // 语音串号：每成功发起一句 +1。宿主记下"我等的第 N 句"，
    // `voiceSerial() != N` 说明已经换了（被打断 / 又有新的一句）。
    // 0 = 这一段会话里从来没播过语音。
    std::uint64_t voiceSerial() const { return m_voiceSerial; }

    // 语音总线当前的 **RMS 电平**（0..1 量级）。B6 口型驱动的唯一输入。
    //
    // 为什么要真电平而不是"在播就张 0.8 的嘴"：嘴型与音量脱钩之后，
    // 一句拖长的元音和一句急促的爆破音会得到**一模一样**的嘴 ——
    // 那比不做口型更出戏（观众能立刻看出是随机动画）。
    //
    // ⚠ 没有语音在播时返回 0（读的就是混音器那一块的能量）；
    //   设备没开（null 后端）时也返回 0 —— 此时口型会停在闭嘴上，
    //   这是对的：没有声音就不该有嘴型。
    float voiceLevel() const;

    // ---- 音量 ----
    void setBusGain(BusId bus, float gain);
    void setMasterGain(float gain);
    void setMuted(bool muted);
    bool muted() const { return m_settings.mute; }
    // 运行期改 ducking 深度（0 = 关），同时也记进 settings()。
    void setDuckDepth(float depth);

    // 当前**实际**增益（0..1）。`BusId::Master` 返回 m_master。
    //
    // ⚠⚠ 为什么不能拿 `settings()` 当读回来的路：`settings().master/music/
    //   sfx/voice` 是 `init()` 时那份**出厂值**，`setBusGain` / `setMasterGain`
    //   只改 `m_master` / `m_busGain[]`，**不回写** settings。
    //   编辑器混音台若读 settings() 画推子位置，拖完下一帧会弹回原位 ——
    //   症状是"推子拖不动"，而不是"数字不对"（2026-11-02 做混音台时补上）。
    float busGain(BusId bus) const {
        const int b = static_cast<int>(bus);
        if (b < 0 || b >= kBusCount) return 0.0f;
        return (bus == BusId::Master) ? m_master : m_busGain[b];
    }

    // ---- 统计 / 自检 ----
    MixStats stats() const { return m_mixer.stats(); }
    std::uint64_t streamResidentBytes() const;
    std::uint64_t cacheResidentBytes() const { return m_cache.residentBytes(); }
    AudioFormat deviceFormat() const { return m_deviceFormat; }
    const std::string& deviceName() const { return m_deviceName; }
    const char* backendName() const;
    // 每 60 帧的 AUDIO-STATS 计数（自检用）
    std::uint64_t statsLines() const { return m_statsLines; }

private:
    enum class State { Closed, Running, Failed };

    void openDevice();
    void closeDevice();
    void logStats();
    int ensureClip(const std::string& path);          // → clipId，失败 -1
    std::uint64_t startVoice(const PlayDesc& desc,
                             const std::string& path, bool streaming);

    struct Stream {
        std::shared_ptr<AudioClip> clip;
        std::vector<float> ring;
        std::atomic<std::uint64_t> absWritten{0};
        std::atomic<std::uint64_t> absRead{0};
        std::uint64_t decodeCursor = 0;
        std::uint32_t ringFrames = 0;
        std::uint32_t channels = 0;
        std::uint32_t sampleRate = 0;
        std::uint64_t totalFrames = 0;
        float volume = 1.0f;   // seek 之后要按原来的音量接上
        bool loop = true;
        bool active = false;
        std::uint32_t slot = 0;
        std::uint32_t gen = 0;
    };

    void fillStream(Stream& s);

    AudioSettings m_settings;
    State m_state = State::Closed;

    std::unique_ptr<AudioBackend> m_backend;
    Mixer m_mixer;
    AudioClipCache m_cache;

    std::string m_deviceName = "(none)";
    AudioFormat m_deviceFormat;
    bool m_mixReady = false;

    // 路径 → clipId（Mixer 的 ClipView 表索引）
    struct ClipEntry {
        std::string path;
        std::shared_ptr<AudioClip> clip;
    };
    std::vector<ClipEntry> m_clips;

    // 一路流（音乐）
    Stream m_stream;

    // 声源 → slot 的映射（applySources 用；voice 结束就清）
    struct SourceSlot {
        std::uint64_t id = 0;
        std::string clip;
        std::uint32_t slot = 0;
        std::uint32_t gen = 0;
    };
    std::vector<SourceSlot> m_sourceSlots;

    float m_master = 1.0f;
    float m_busGain[kBusCount] = {1.0f, 0.7f, 1.0f, 1.0f};
    bool m_muted = false;

    std::uint64_t m_frameCounter = 0;
    std::uint64_t m_statsLines = 0;
    float m_retryTimer = 0.0f;
    bool m_devfailLogged = false;

    // ---- 语音（D1）----
    // ⚠ `m_voiceSerial` 是**主线程独占**的（`playVoice` 里 ++，`voiceSerial()`
    //   里读）。不用 atomic：这一层所有的调用方都在主线程，加 atomic 只会
    //   让"它被别的线程写过"这件事看起来像真的。
    std::uint64_t m_voiceSerial = 0;
    std::chrono::steady_clock::time_point m_voiceStart{};
    static constexpr int kVoiceGraceMs = 120;

    // cpu 占比（音频线程写、主线程读）
    std::atomic<float> m_cpuRatio{0.0f};
    std::atomic<std::uint64_t> m_blockStartQpc{0};
    std::atomic<std::uint64_t> m_lastBlockTicks{0};
    std::atomic<std::uint64_t> m_qpcFreq{0};

    friend struct AudioSystemCallback;
};

} // namespace audio
