#include "AudioMixerPanel.h"

#include "Localization.h"
#include "PanelDock.h"        // applyDockHint
#include "PanelRegistry.h"    // panelOpenRef
#include "ui/DebugRects.h"    // ui::logRect

#include "core/Logger.h"

#include <imgui.h>

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>

namespace editor {

const char* audioKnobName(AudioKnob k) {
    switch (k) {
        case AudioKnob::Master:  return "master";
        case AudioKnob::Music:   return "music";
        case AudioKnob::Sfx:     return "sfx";
        case AudioKnob::Voice:   return "voice";
        case AudioKnob::Duck:    return "duck";
        case AudioKnob::Mute:    return "mute";
        case AudioKnob::StopAll: return "stopall";
        default:                 return "none";
    }
}

namespace {

void tagItem(const char* tag) {
    ui::logRect(tag, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

// 控件 label = "显示名##锚点"（与 PostVolumePanel 同一条纪律：
// 显示名随语言变，锚点不能变）。
std::string idLabel(const char* shown, const char* tag) {
    return std::string(shown) + "##" + tag;
}

// 总线下标 → 枚举。刻意写死映射而不是 `static_cast`：两张表一旦顺序
// 不同，推子会**互相串台**（拖 Music 压低的是 SFX），而画面完全正常。
AudioKnob knobOfBus(int bus) {
    switch (bus) {
        case 0: return AudioKnob::Master;
        case 1: return AudioKnob::Music;
        case 2: return AudioKnob::Sfx;
        case 3: return AudioKnob::Voice;
        default: return AudioKnob::None;
    }
}

// RMS 电平表。
//
// ⚠ 显示用 **sqrt** 压缩而不是线性：语音 / 音效的 RMS 实际落在
//   0.005~0.15 这个区间，线性画出来几乎永远是空条 —— 于是"有声音但条不动"
//   会被误读成"电平没进来"。开方之后 0.01 占 10%、0.09 占 30%，肉眼可用。
//   判据脚本看的是数值（`AM-VOICE-LEVEL` 的 `%.4f`），不是这条弧长。
void meter(float level, const char* tag, float width = 120.0f) {
    const float t = std::sqrt(level < 0.0f ? 0.0f : (level > 1.0f ? 1.0f : level));
    ImGui::ProgressBar(t, ImVec2(width, 0.0f), "");
    tagItem(tag);
}

} // namespace

void AudioMixerPanel::draw(AudioMixerIO& io) {
    bool& open = panelOpenRef("audio");
    if (!open) return;

    ImGuiWindowFlags wf = 0;
    const bool docked = applyDockHint(wf);
    if (!docked) {
        const ImGuiIO& gio = ImGui::GetIO();
        ImGui::SetNextWindowSizeConstraints(ImVec2(440.0f, 380.0f),
                                            ImVec2(FLT_MAX, FLT_MAX));
        // 默认 560 高：推子 4 行 + 静音/duck/停止 3 行 + 总线 3 行 + 语音 3 行
        // + 声源列表 + 汇总。窄了就得滚，而"一眼看全"正是这一屏的理由。
        ImGui::SetNextWindowSize(ImVec2(540.0f, 560.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(gio.DisplaySize.x * 0.62f,
                                       gio.DisplaySize.y * 0.08f),
                                ImGuiCond_FirstUseEver);
    }

    if (!ImGui::Begin(trId("Audio mixer", "panel_audio"),
                      docked ? nullptr : &open, wf)) {
        ImGui::End();
        return;
    }
    ui::logRect("AM-RECT", ImGui::GetWindowPos(),
                ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                       ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    const AudioMixerState& st = io.st;

    // 总线显示名（走 TR，所以必须是字面量 —— 校验器只认字面量）。
    const char* kBusTitle[audio::kBusCount] = {
        TR("Master"), TR("Music"), TR("SFX"), TR("Voice")};
    const char* kFaderTag[audio::kBusCount] = {
        "AM-FADER-MASTER", "AM-FADER-MUSIC", "AM-FADER-SFX", "AM-FADER-VOICE"};

    // ---------------------------------------------------------- 设备
    if (!st.running) {
        // 说清楚"这不是坏了"。无设备时下面每一条 API 都是空操作，
        // 数字照样动 —— 不说的话第一反应是"混音台不生效"。
        ImGui::TextWrapped("%s",
                           TR("No audio device. The numbers below still move, "
                              "but you will not hear anything."));
    } else {
        ImGui::Text("%s %s", TR("Device:"), st.device.c_str());
        ImGui::TextDisabled(TR("%s  %d Hz  %d ch"),
                            st.backend.c_str(), st.sampleRate, st.channels);
    }
    tagItem("AM-STATUS");

    // ---------------------------------------------------------- 推子
    ImGui::SeparatorText(TR("Faders"));
    for (int b = 0; b < audio::kBusCount; ++b) {
        const AudioKnob knob = knobOfBus(b);
        float v = st.gain[b];
        ImGui::PushID(b);
        ImGui::SetNextItemWidth(210.0f);
        if (ImGui::SliderFloat(idLabel(kBusTitle[b], kFaderTag[b]).c_str(), &v,
                               0.0f, 1.0f, "%.2f"))
            io.push(knob, v);
        tagItem(kFaderTag[b]);
        // 松手时才打一行。**不能每帧打**：拖一次推子会刷几十行，
        // 把真正有用的 AUDIO-DEVFAIL / AUDIO-DUCK 顶出可见区。
        if (ImGui::IsItemDeactivatedAfterEdit())
            VK_LOG_INFO("AUDIO-MIX knob=%s value=%.3f", audioKnobName(knob),
                        static_cast<double>(v));

        ImGui::SameLine();
        if (b == 0) {
            ImGui::TextDisabled(TR("voices %d / %d"), st.voices, st.maxVoices);
        } else if (b == 1) {
            // Music 这一行顺带把 **duck 之后的实际增益**打出来：
            // "推子在 0.70、实际只有 0.18"是 ducking 在起作用，而不是 bug。
            if (st.musicPlaying)
                ImGui::TextDisabled(TR("playing %.1f s"), st.musicPos);
            else if (st.duckDepth > 0.0f)
                ImGui::TextDisabled(TR("ducked to %.2f"), st.musicGain);
        } else {
            ImGui::TextDisabled(TR("%d voices"), st.busVoices[b]);
        }
        ImGui::PopID();
    }

    // ---------------------------------------------------------- 静音 / duck
    // 静音
    bool mute = st.mute;
    if (ImGui::Checkbox(idLabel(TR("Mute all"), "am-mute").c_str(), &mute)) {
        io.push(AudioKnob::Mute, mute ? 1.0f : 0.0f);
        VK_LOG_INFO("AUDIO-MIX knob=mute value=%d", mute ? 1 : 0);
    }
    tagItem("AM-MUTE");

    ImGui::SameLine();
    // 「失焦静音」**只读**：`RuntimeApp` 每帧写的是
    // `cfg.audio.mute || !focused`，编辑器没有这一条 —— 因为编辑器的
    // 主用法是"听着 BGM 切到浏览器查资料"，自动静音会让人以为音频断了。
    // 做只读展示而不是不显示：作者得知道发布后会这样。
    {
        bool mfl = st.muteOnFocusLost;
        ImGui::BeginDisabled(true);
        ImGui::Checkbox(idLabel(TR("Mute when unfocused"), "am-mutefocus").c_str(),
                        &mfl);
        ImGui::EndDisabled();
        tagItem("AM-MUTEFOCUS");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("%s",
                              TR("Runtime only (game.json). The editor never "
                                 "mutes itself on focus loss."));
    }

    // duck 深度
    float duck = st.duckDepth;
    ImGui::SetNextItemWidth(210.0f);
    if (ImGui::SliderFloat(idLabel(TR("Ducking depth"), "am-duck").c_str(), &duck,
                           0.0f, 1.0f, "%.2f"))
        io.push(AudioKnob::Duck, duck);
    tagItem("AM-DUCK");
    if (ImGui::IsItemDeactivatedAfterEdit())
        VK_LOG_INFO("AUDIO-MIX knob=duck value=%.3f",
                    static_cast<double>(duck));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("%s",
                          TR("How far Music is pushed down while Voice is "
                             "playing. 0 = off."));
    ImGui::SameLine();
    ImGui::TextDisabled("%s", TR("0 = off"));

    if (ImGui::Button(TR("Stop all sounds"))) {
        io.push(AudioKnob::StopAll, 0.0f);
        VK_LOG_INFO("AUDIO-MIX knob=stopall value=0");
    }
    tagItem("AM-STOPALL");

    // ---------------------------------------------------------- 总线实时
    // 只列**可以挂声部**的三条（1..3）。Master 是汇总级，`busVoices[0]`
    // 永远是 0 —— 列出来只会让人以为"master 上没有声音"。
    ImGui::SeparatorText(TR("Buses (live)"));
    {
        const char* kTag[audio::kBusCount] = {"", "AM-BUS-MUSIC", "AM-BUS-SFX",
                                              "AM-BUS-VOICE"};
        for (int b = 1; b < audio::kBusCount; ++b) {
            ImGui::PushID(100 + b);
            ImGui::Text("%s", kBusTitle[b]);
            ImGui::SameLine(78.0f);
            ImGui::TextDisabled(TR("%d voices"), st.busVoices[b]);
            ImGui::SameLine(158.0f);
            meter(st.busLevel[b], kTag[b]);
            ImGui::SameLine();
            ImGui::TextDisabled("%.4f", static_cast<double>(st.busLevel[b]));
            ImGui::PopID();
        }
    }

    // ---------------------------------------------------------- 语音 / 口型
    // 这一段是这一屏**最不能省**的：B6 口型的唯一输入就是 Voice 总线的
    // RMS，而它以前没有任何界面。于是"嘴不张"的三种原因（没在播 / 播了
    // 但电平 0 / 驱动没接上）在日志里长得一模一样。
    ImGui::SeparatorText(TR("Voice (drives the mouth)"));
    ImGui::Text("%s %.4f", TR("voice level"), st.voiceLevel);
    tagItem("AM-VOICE-LEVEL");
    ImGui::TextDisabled(TR("playing %d   serial %llu"),
                        st.voicePlaying ? 1 : 0,
                        static_cast<unsigned long long>(st.voiceSerial));
    tagItem("AM-VOICE-PLAYING");
    ImGui::TextWrapped("%s",
                       TR("The mouth reads this level. If it stays 0.0000 while "
                          "a line is playing, the mouth will not open."));

    // ---------------------------------------------------------- 声源
    ImGui::SeparatorText(TR("Scene audio sources"));
    if (!st.playing) {
        // 编辑态**不播场景音频**（只有 Play 态才 sync AudioSourceComponent）。
        // 这条说明省掉的正是最常见的一个困惑："我给实体挂了 AudioSource，
        // 编辑器里怎么没声音"。
        ImGui::TextDisabled("%s",
                            TR("Not in Play. Scene audio sources only run while "
                               "playing."));
        tagItem("AM-NOSRC");
    } else if (st.sources.empty()) {
        ImGui::TextDisabled("%s", TR("No AudioSource in this scene."));
        tagItem("AM-NOSRC");
    } else {
        for (int i = 0; i < static_cast<int>(st.sources.size()); ++i) {
            const AudioMixerSource& s = st.sources[static_cast<size_t>(i)];
            const int bus = (s.bus >= 1 && s.bus < audio::kBusCount) ? s.bus : 2;
            ImGui::Text("%s", s.clip.c_str());
            ImGui::SameLine(230.0f);
            ImGui::TextDisabled(TR("%s  vol %.2f  %s"), kBusTitle[bus],
                                static_cast<double>(s.volume),
                                s.spatial ? TR("spatial") : TR("2D"));
            const std::string tag = "AM-SRC-" + std::to_string(i);
            tagItem(tag.c_str());
        }
    }

    // ---------------------------------------------------------- 汇总
    ImGui::Separator();
    ImGui::TextDisabled(TR("rms %.4f   peak %.4f   underruns %llu   stolen %llu"),
                        static_cast<double>(st.rms),
                        static_cast<double>(st.peak),
                        static_cast<unsigned long long>(st.underruns),
                        static_cast<unsigned long long>(st.stolen));
    ImGui::TextDisabled(TR("clip cache %.0f kB   stream %.0f kB"),
                        static_cast<double>(st.cacheBytes) / 1024.0,
                        static_cast<double>(st.streamBytes) / 1024.0);
    tagItem("AM-LIVE");

    ImGui::End();
}

} // namespace editor
