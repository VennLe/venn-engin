#pragma once
// ============================================================
// engine/audio/Mixer —— 自研混音器（**纯数据，不碰 OS**）
//
// 这是整条音频路线图的地基（docs/audio_roadmap.md §3.1 / G14-2）。
// 它不 open 设备、不认识 WASAPI、不 include 任何系统头 —— 因此
// `Game.exe --selftest audio` 可以在**无声卡、无窗口**的机器上把
// 声部池 / 总线 / 重采样 / 3D 衰减 / pan / ducking 全部断言一遍，
// 并且同参数两次渲染 **memcmp 逐位相同**。
//
// ------------------------------------------------------------
// 五条纪律（破了就会"低负载正常、掉帧时爆音/崩溃"）
// ------------------------------------------------------------
//  1. `mixBlock()` 运行在声卡线程：**不分配、不加锁、不打日志、不碰 World**。
//     所有缓冲在 `init()` 里一次分配好（§3.5）。
//  2. 主线程 → 音频线程只走一个**无锁 SPSC 环形队列**，元素是**定长 POD**
//     （`AudioCommand`）。不用 std::function（会堆分配）。
//  3. 句柄 = `(slot, generation)`。槽位会复用，只有槽号会 ABA ——
//     "停掉的那一声"停到了"刚抢进来的新一声"。代际不匹配的命令直接丢弃。
//  4. 所有"停 / 抢 / 切"都走 **5~10 ms 淡出**（R16），否则是"哒"一声。
//  5. 时基来自**设备 block 的帧数**，不看 `dt` / `timeScale`（§3.6）。
//     `mixBlock` 里没有任何时间查询 —— 这也是确定性的前提。
// ============================================================

#include "audio/AudioFormat.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace audio {

// ------------------------------------------------------------
// 输入视图（POD）。Mixer 只拿裸指针，**不拥有**任何样本。
// ------------------------------------------------------------
struct ClipView {
    const float* samples = nullptr;   // 交错 float32
    std::uint64_t frames = 0;
    std::uint32_t channels = 0;
    std::uint32_t sampleRate = 0;

    bool valid() const {
        return samples != nullptr && frames > 0 && channels > 0 &&
               sampleRate > 0;
    }
};

// 流式来源：一条 3 s 环形缓冲，按**绝对帧号**寻址。
//
// 关键设计（§3.8）：`absWritten` 单调递增，第 a 帧的源样本就是
// "第 (a % totalFrames) 帧" —— 因为主线程是**顺序**解码并写入的。
// 于是循环**不需要**在音频线程里做任何回绕处理，只要主线程在解码器
// 到尾时 rewind 就行（绕回点因此天然无缝）。
struct StreamView {
    const float* ring = nullptr;              // ringFrames * channels
    std::uint32_t ringFrames = 0;
    std::uint32_t channels = 0;
    std::uint32_t sampleRate = 0;
    std::uint64_t totalFrames = 0;
    const std::atomic<std::uint64_t>* absWritten = nullptr;
    // 音频线程每块写一次"我读到哪了"，主线程据此决定预读多少。
    std::atomic<std::uint64_t>* absRead = nullptr;

    bool valid() const {
        return ring && ringFrames > 0 && channels > 0 && sampleRate > 0 &&
               absWritten && absRead;
    }
};

constexpr int kMaxClips = 256;
constexpr int kMaxStreams = 8;

// ------------------------------------------------------------
// 命令（定长 POD，走 SPSC）
// ------------------------------------------------------------
enum class CmdType : std::uint32_t {
    Play = 0,
    Stop,
    StopAll,
    SetBusGain,
    SetMuted,
    SetListener,
    SetMasterGain,
    UpdateVoice,
    SetDuckDepth,
    Count,
};

struct AudioCommand {
    std::uint32_t type = 0;
    std::uint32_t slot = 0;
    std::uint32_t gen = 0;
    std::uint32_t clipId = 0;
    std::uint32_t streamId = 0;
    std::uint32_t bus = 2;
    // bit0 loop / bit1 spatial / bit2 streaming / bit3 fadeIn
    std::uint32_t flags = 0;
    float volume = 1.0f;
    float pitch = 1.0f;
    float minDist = 1.0f;
    float maxDist = 50.0f;
    float rolloff = 1.0f;
    float pos[3] = {0.0f, 0.0f, 0.0f};
    // SetListener 用（显式字段，别拿别的字段复用 —— 那种"省 12 字节"的
    // 技巧第一次改就会错位，而且错得毫无提示）。
    float fwd[3] = {0.0f, 0.0f, -1.0f};
    float up[3] = {0.0f, 1.0f, 0.0f};
    float fadeMs = 10.0f;     // Stop 用
    float value = 0.0f;       // SetBusGain / SetMuted / SetMasterGain
};

// 主线程侧的播放描述（比命令友好）
struct PlayDesc {
    std::uint32_t clipId = 0;
    std::uint32_t streamId = 0;
    std::uint32_t bus = 2;      // 默认 SFX
    float volume = 1.0f;
    float pitch = 1.0f;
    bool loop = false;
    bool spatial = false;
    float minDist = 1.0f;
    float maxDist = 50.0f;
    float rolloff = 1.0f;
    float pos[3] = {0.0f, 0.0f, 0.0f};
};

struct MixStats {
    int voices = 0;         // 活跃声部数
    float rms = 0.0f;       // 最后一块的 RMS（立体声混合）
    float peak = 0.0f;      // 最后一块的峰值
    float musicGain = 1.0f; // ducking 之后 Music 总线实际增益（dB 断言用）
    std::uint64_t underruns = 0;
    std::uint64_t stolen = 0;
};

class Mixer {
public:
    Mixer();
    ~Mixer();

    // 只允许调一次；之后所有缓冲固定（音频线程里不再分配）。
    void init(std::uint32_t outSampleRate, std::uint32_t outChannels);

    std::uint32_t outputSampleRate() const { return m_outSr; }
    std::uint32_t outputChannels() const { return m_outCh; }

    // ---- 主线程：注册素材 ----
    void setClip(std::uint32_t id, const ClipView& view);
    void clearClips();
    void setStream(std::uint32_t id, const StreamView& view);

    // ---- 主线程：发命令 ----
    // 返回 false = 命令队列满（丢弃）。分配槽位与代际在**主线程**做，
    // 音频线程只按命令落到自己的表里（两张表一一对应）。
    bool play(const PlayDesc& desc, std::uint32_t* outSlot = nullptr,
              std::uint32_t* outGen = nullptr);
    void stop(std::uint32_t slot, std::uint32_t gen, float fadeMs = 10.0f);
    void stopAll(float fadeMs = 10.0f);
    void setBusGain(int bus, float gain);
    void setMasterGain(float gain);
    void setMuted(bool muted);
    // ducking 深度（0 = 关掉 ducking，1 = 压到完全静音）。
    // ⚠ 走命令队列而不是直接写成员：`m_duckDepth` 只有音频线程读，
    //   主线程裸写就是一次无同步的竞争（会"看起来生效了但偶尔不生效"）。
    //   默认 0.75 ≈ -12 dB（§G14-5）。
    void setDuckDepth(float depth);
    void setListener(const float pos[3], const float forward[3],
                     const float up[3]);
    // 移动 / 改音量（3D 声源每帧调一次）。代际不匹配直接丢弃。
    void updateVoice(std::uint32_t slot, std::uint32_t gen, const float pos[3],
                     float volume);

    // ---- 音频线程：混一块 ----
    // out 必须是 frames * m_outCh 个 float。**不预清零**，内部会全部写满。
    void mixBlock(float* out, std::uint32_t frames);

    // ---- 自检：离线渲染（同一条代码路径）----
    void renderOffline(float* out, std::uint32_t frames);

    MixStats stats() const;
    // ducking 包络跨过阈值时置位，主线程 drain 后打 AUDIO-DUCK（§6：只在变化时打）
    bool consumeDuckEvent(float* musicGainOut);

    // 主线程查询（用于"抢最旧"的断言）：槽位是否活跃 + 代际
    bool slotBusy(std::uint32_t slot) const;
    std::uint32_t slotGeneration(std::uint32_t slot) const;

    // 当前活跃声部（音频线程写、主线程读的镜像；仅用于日志/断言）
    int activeVoices() const { return m_statVoices.load(std::memory_order_relaxed); }

private:
    struct Voice {
        bool active = false;
        bool pending = false;      // 淡出结束后要接上的新 play
        std::uint32_t gen = 0;
        std::uint32_t clip = 0;
        std::uint32_t stream = 0;
        bool streaming = false;
        int bus = 2;
        double pos = 0.0;
        double step = 1.0;
        float vol = 1.0f;
        bool loop = false;
        bool spatial = false;
        float minDist = 1.0f, maxDist = 50.0f, rolloff = 1.0f;
        float px = 0.0f, py = 0.0f, pz = 0.0f;
        std::uint32_t fadeFrames = 0;
        std::uint32_t fadeTotal = 0;
        PlayDesc pendingDesc;
    };

    void applyCommand(const AudioCommand& c);
    void startVoice(int slot, const PlayDesc& d, std::uint32_t gen);
    void renderVoice(Voice& v, int slot, float* busL, float* busR,
                     std::uint32_t frames);
    float sampleStream(const StreamView& s, std::uint64_t frame,
                       int channel) const;

    std::uint32_t m_outSr = 48000;
    std::uint32_t m_outCh = 2;

    ClipView m_clips[kMaxClips];
    StreamView m_streams[kMaxStreams];

    Voice m_voices[kMaxVoices];

    // 主线程侧镜像：槽位是否被占 + 代际 + 分配序（抢最旧用）
    std::atomic<bool> m_slotBusy[kMaxVoices];
    std::atomic<std::uint32_t> m_slotGen[kMaxVoices];
    std::uint64_t m_allocSeq[kMaxVoices] = {};
    std::uint64_t m_allocCounter = 0;

    // SPSC 队列
    static constexpr std::uint32_t kQueueSize = 512;
    std::vector<AudioCommand> m_queue;
    std::atomic<std::uint32_t> m_head{0};   // 生产者（主线程）
    std::atomic<std::uint32_t> m_tail{0};   // 消费者（音频线程）

    // 总线
    float m_busCur[kBusCount] = {1, 1, 1, 1};
    float m_busTarget[kBusCount] = {1, 1, 1, 1};
    bool m_muted = false;

    // listener
    float m_lPos[3] = {0, 0, 0};
    float m_lFwd[3] = {0, 0, -1};
    float m_lUp[3] = {0, 1, 0};
    bool m_listenerSet = false;

    // ducking（§G14-5）
    float m_duck = 0.0f;       // 0 = 不 duck，1 = 完全 duck
    float m_duckDepth = 0.75f; // -12 dB 左右
    bool m_duckEvent = false;
    float m_duckEventGain = 1.0f;

    // 块缓冲
    std::vector<float> m_busBuf;   // kBusCount * kMaxBlockFrames * 2
    std::vector<float> m_dryBuf;

    std::atomic<int> m_statVoices{0};
    std::atomic<float> m_statRms{0.0f};
    std::atomic<float> m_statPeak{0.0f};
    std::atomic<float> m_statMusicGain{1.0f};
    std::atomic<std::uint64_t> m_underruns{0};
    std::atomic<std::uint64_t> m_stolen{0};
};

// ------------------------------------------------------------
// 3D 空间化（纯函数，供 Mixer 与自检共用 —— 公式只有一份）
// ------------------------------------------------------------
// `gain = minDist/(minDist + rolloff*(d - minDist)) * fadeOut(d)`
//   ⚠⚠ fadeOut 不是可选项：倒数公式在 maxDist 处**并不为 0**，直接截断
//   会产生一个阶跃 → 听感是"啪"的一声（R6）。
float spatialGain(float distance, float minDist, float maxDist, float rolloff);

// 等功率 pan（§3.7）：`L = cos((x+1)π/4)`、`R = sin((x+1)π/4)`
//   ⚠ 不要用线性 `L = (1-x)/2`：正前方会 -3 dB 塌陷，单声道源听起来"凹进去"。
void equalPowerPan(float x, float* outL, float* outR);

// 从"源位置 + listener 基"算 (增益, panX)。
// panX = 源方向在 listener 右向量上的投影，裁剪到 [-1, 1]。
void evaluateSpatial(const float src[3], const float lpos[3],
                     const float lright[3], float minDist, float maxDist,
                     float rolloff, float* outGain, float* outPanX);

} // namespace audio
