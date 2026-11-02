#pragma once
// ============================================================
// editor/AudioMixerPanel —— 音频混音台（Audio mixer）
//
// ---- 为什么需要它 ----
// 音频整层（G14）从 10 月就通了：`AudioSystem` + 自研 `Mixer` + WASAPI/Null
// 双后端 + ducking + 3D 空间化 + 流式音乐 + 语音总线，`Game.exe --selftest
// audio` 能在无声卡机器上把它们全断言一遍。
//
// 但 2026-11-02 之前，编辑器里**一个推子都没有**：
//     grep -rn "setMasterGain\|setMuted\|setDuckDepth" editor/  →  0 命中
// 唯一能改的是控制台 `audio.bus <name> <v>`（只覆盖 Music/SFX/Voice），
// 而 Master / 静音 / duck 深度必须改 C++ 再重编译 —— 也就是说"混音"这件事
// 对开发者是**不可达**的。这一屏补的就是这个洞。
//
// 还有一条更贵的缺口：**语音总线是隐形的**。11-01 为 B6 口型新加的
// Voice 总线 RMS 是嘴型的唯一输入，但它没有任何界面 —— 于是"口型为什么不
// 张"永远查不出来（到底是没播、还是播了但电平 0、还是 tick 没接上）。
// 所以这张面板里"Voice (drives the mouth)"那一段是**必答项**，不是附加。
//
// ------------------------------------------------------------
// 三条设计约束
// ------------------------------------------------------------
// ① **真值在 AudioSystem 里**，面板不持副本。面板每帧拿一份快照（`st`），
//    改的时候只往 `edits` 里压一条**意图**（knob + value），由 EditorApp
//    落到 `AudioSystem`。不做本地副本 + "应用"按钮 —— 那一定会分叉，
//    症状是"面板显示 0.70、听起来还是 1.00"。
//
// ② **面板不认识 AudioSystem**（这是 `EditorContext` 的老纪律：面板之间、
//    面板与引擎之间只经一层数据）。所以这里只定义 POD，不 include
//    `audio/AudioSystem.h` 的实现 —— 只有 `audio/AudioFormat.h`（BusId /
//    kBusCount）这种纯 POD 头才允许直接进面板。
//
// ③ **"没设备"必须说清楚**。无 WASAPI / null 后端下所有 API 都是空操作，
//    推子照样能拖、数字照样变，只是**没有声音**。不说的话，第一反应是
//    "混音台坏了"，而不是"我没插声卡"。
//
// ------------------------------------------------------------
// 自动化锚点（`tools/verify_audio_mixer.py` 用）
// ------------------------------------------------------------
//   AM-RECT                      面板矩形
//   AM-STATUS                    设备 / 后端 / 声道那一段
//   AM-FADER-MASTER / -MUSIC / -SFX / -VOICE
//   AM-MUTE / AM-MUTEFOCUS / AM-DUCK / AM-STOPALL
//   AM-BUS-MUSIC / -SFX / -VOICE 每总线的实时行（声部数 + 电平表）
//   AM-VOICE-LEVEL / AM-VOICE-PLAYING
//   AM-SRC-<i>                   第 i 个在播声源；空列表时打 AM-NOSRC
//   AM-LIVE                      底部汇总（rms / peak / 缓存）
// 日志锚点：
//   AUDIO-MIX   knob=<name> value=<v>    面板动作（松手时一条）
//   AUDIO-CMD   knob=<name> value=<v>    控制台命令
//   AUDIO-SOURCES n=<k> / i=<i> clip=... 声源清单（`audio.sources`）
// ============================================================

#include "audio/AudioFormat.h"

#include <cstdint>
#include <string>
#include <vector>

namespace editor {

// 面板能改的旋钮。用枚举而不是"字符串 + 浮点"：命令与面板共用同一条
// 落地路径（`EditorApp::applyAudioKnob`），拼错名字在编译期就挂。
enum class AudioKnob : int {
    None = 0,
    Master,
    Music,
    Sfx,
    Voice,
    Duck,
    Mute,
    StopAll,
};
constexpr int kAudioKnobCount = 7;

// 日志/命令行里的名字。**不翻译**：这是标识符，不是界面文字
// （译文一变，grep 一条都搜不到）。
const char* audioKnobName(AudioKnob k);

// 一路在播声源（来自 `AudioSystem::applySources` 的那份 `SourceUpdate`）。
struct AudioMixerSource {
    std::string clip;
    int bus = static_cast<int>(audio::BusId::SFX);
    float volume = 1.0f;
    bool spatial = false;
    bool loop = false;
    bool playing = true;
};

// 面板要显示的一整套量。**每次 draw 前由 EditorApp 现填**（不留状态）。
struct AudioMixerState {
    bool running = false;
    std::string backend = "none";
    std::string device = "(none)";
    int sampleRate = 0;
    int channels = 0;

    bool playing = false;            // 编辑器在 Play 态吗（决定"声源"段说什么）

    // 增益按**总线下标**存：0 = Master，1 = Music，2 = SFX，3 = Voice
    // （与 `audio::BusId` 逐位一致 —— 别自己另起一套顺序）。
    float gain[audio::kBusCount] = {1.0f, 1.0f, 1.0f, 1.0f};
    bool mute = false;
    float duckDepth = 0.75f;
    // 只读展示：运行时按焦点自动静音。编辑器**不实现**（见面板注释）。
    bool muteOnFocusLost = false;

    int voices = 0;
    int maxVoices = audio::kMaxVoices;
    int busVoices[audio::kBusCount] = {};
    // 每总线 RMS（0..1 量级）。Voice 那条就是口型驱动源。
    float busLevel[audio::kBusCount] = {};
    float rms = 0.0f;
    float peak = 0.0f;
    float musicGain = 1.0f;          // ducking 之后 Music 的实际增益
    unsigned long long underruns = 0;
    unsigned long long stolen = 0;

    bool voicePlaying = false;
    unsigned long long voiceSerial = 0;
    float voiceLevel = 0.0f;

    bool musicPlaying = false;
    double musicPos = 0.0;

    unsigned long long cacheBytes = 0;
    unsigned long long streamBytes = 0;

    std::vector<AudioMixerSource> sources;
};

// 面板 → EditorApp 的一帧编辑意图。
//
// ⚠ 用**小队列**而不是单个"最后一笔"：同一帧里既拖推子又点 Stop 是完全
//   可能的（键盘 + 鼠标，或者自动化连发），单槽会把前一笔**永久**丢掉
//   —— 推子丢一帧不要紧（下一帧还在拖），按钮丢了就是"点了没反应"。
struct AudioKnobEdit {
    AudioKnob knob = AudioKnob::None;
    float value = 0.0f;
};
constexpr int kMaxAudioEdits = 8;

struct AudioMixerIO {
    AudioMixerState st;                          // in：EditorApp 填
    AudioKnobEdit edits[kMaxAudioEdits] = {};    // out：面板填
    int editCount = 0;

    void push(AudioKnob k, float v) {
        if (k == AudioKnob::None) return;
        if (editCount >= kMaxAudioEdits) return;   // 满了丢新的，不覆盖旧的
        edits[editCount].knob = k;
        edits[editCount].value = v;
        ++editCount;
    }
};

class AudioMixerPanel {
public:
    void draw(AudioMixerIO& io);
};

} // namespace editor
