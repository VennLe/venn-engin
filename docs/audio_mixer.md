# 音频混音台（Audio mixer，A2）

> 2026-11-02。对应审计文档 `panel_gap_review_2026-11-02.md` 的 A2 条。
> 判据：`tools/verify_audio_mixer.py`（27 条，五组）。
> 面板：`editor/AudioMixerPanel.{h,cpp}`；面板表 id = `audio`。

## 1. 它补的是什么洞

音频**内核**从 G14 起就是完整的 —— 自研 `Mixer`（SPSC 无锁队列 + 定长 POD
命令 + 世代句柄 + 5~10 ms 淡出）、WASAPI / Null 双后端、3D 空间化、等功率
pan、ducking、流式音乐、语音总线，`Game.exe --selftest audio` 能在无声卡
机器上把它们全断言一遍。

但在 2026-11-02 之前：

```
grep -rn "setMasterGain\|setMuted\|setDuckDepth" editor/   →  0 命中
```

也就是说 **Master 音量 / 全局静音 / duck 深度**这三件事对开发者是不可达的
（只能改 C++ 再重编译），而唯一能动的 `audio.bus` 只覆盖 Music/SFX/Voice
三条。这是典型的"能力在、入口零"形状 —— 与 PostVolume 同一个病。

第二条更贵：**语音总线是隐形的**。11-01 为 B6 口型新加的 Voice 总线 RMS 是
嘴型的唯一输入，但它没有任何界面。于是"口型为什么不张"的三种完全不同的
原因（没在播 / 播了但电平 0 / 驱动 tick 没接上）在日志里长得一模一样。

## 2. 怎么用

面板：**菜单 → 音频混音台**（`Authoring` 组，浮动窗口，无快捷键），或
`MYVK_EDITOR_PANELS=audio`。四段：

| 段 | 内容 |
|---|---|
| 设备 | 后端名 / 设备名 / 采样率 / 声道。没设备时明确写"数字照样动，但听不到" |
| 推子 | Master / Music / SFX / Voice 四根 + 全局静音 + duck 深度 + 停止所有声音 |
| 总线（实时） | Music / SFX / Voice 各一行：声部数 + RMS 电平表 + 数值 |
| 语音（口型输入） | `voice level` / `playing` / `serial` + 一句"它恒为 0 会怎样" |
| 场景声源 | Play 态下列出每个 `AudioSourceComponent`（clip / 总线 / 音量 / 空间化）；编辑态明说"不跑" |

控制台命令（与面板**共用** `applyAudioKnob`）：

```
audio.state                                  读回当前真值（判据用）
audio.gain <master|music|sfx|voice|duck> <0..1>
audio.mute <0|1>
audio.bus  <master|music|sfx|voice> <0..1>   老命令，保留
audio.sources                                列出喂给混音器的声源
audio.stats                                  一行 AUDIO-STATS
```

## 3. 语义细节（容易踩的）

### 3.1 `settings()` **不是**当前增益
`AudioSettings` 是 `init()` 时那份**出厂值**。`setBusGain` / `setMasterGain`
只改 `m_master` / `m_busGain[]`，**不回写** settings。

于是本轮加了只读访问器：

```cpp
float busGain(BusId bus) const;   // Master → m_master
```

面板推子必须读它。读 `settings()` 的症状不是"数字不对"，而是**推子拖完弹回
原位** —— 看起来像"拖不动"，于是会去怀疑 ImGui。

### 3.2 `setBusGain(Master)` 曾经与 `setMasterGain` 分叉（已修）
老实现：

```cpp
m_busGain[b] = gain;                      // b == 0 时写的是这里
if (bus == BusId::Master) m_mixer.setMasterGain(gain);
```

`m_master` 没被写 → `masterGain()` 读 `m_master` 得到旧值。
`audio.bus master 0.5` 之后：混音器实际 0.5、读回 1.0，两边各对一半。
现在 `BusId::Master` 直接转发 `setMasterGain`，**单一真值**。
`verify_audio_mixer` 的 D 组就是这条的回归判据。

### 3.3 电平表用 sqrt 压缩
语音 / 音效的 RMS 实际落在 0.005~0.15。线性画出来几乎永远是空条，会被误读
成"电平没进来"。开方之后 0.01 占 10%、0.09 占 30%。**判据看数值不看弧长**。

### 3.4 「失焦静音」只读
`RuntimeApp` 每帧写的是 `cfg.audio.mute || !focused`；编辑器**不实现**这一条
（主用法是"听着 BGM 切到浏览器查资料"）。做只读展示而不是隐藏：作者得知道
发布后会这样。

### 3.5 日志节奏
面板推子在**松手**（`IsItemDeactivatedAfterEdit`）时打一行 `AUDIO-MIX`，
不是每帧。拖一次推子刷几十行会把 `AUDIO-DEVFAIL` / `AUDIO-DUCK` 顶出可见区。
命令打 `AUDIO-CMD`。

## 4. 判据（`tools/verify_audio_mixer.py`，27 条）

| 组 | 验什么 |
|---|---|
| A | 改得动 **且读回来是改后的值**（`audio.state` 四次快照）；认不出的旋钮留痕；编辑态声源 n=0 |
| B | 17 个锚点全在；**没碰控件时没有 AUDIO-MIX**（面板不会每帧推意图） |
| C | **点面板上的 Stop all 真的能改**（`wait_rect` → `click` → `AUDIO-MIX`） |
| D | 老 `audio.bus` 与推子不许分叉（3.2 的回归） |
| E | null 后端（无声卡）面板照画、不报错 |

⚠ A 组为什么必须有读回：只有 `AUDIO-CMD knob=music value=0.30` 的话，
"参数收到了但没落到 AudioSystem"与"真的改了"在日志里**一模一样**。
⚠ C 组不能省：面板与命令共用 `applyAudioKnob`，只测命令测不出"按钮点不到"
—— 而"界面点不到 = 没交付"。

## 5. 已知边界

- **不做总线独奏（solo）**。混音台里 solo 通常要"记住其他总线的原值"，
  与"真值在 AudioSystem"这条冲突（要么加一排 solo 状态，要么面板持副本）。
  单人开发的引擎里，静音三条总线也够用。
- **不改 `maxVoices`**。它是 `init()` 时定下来的（声部池预分配），
  运行期改要重建设备。
- **没有实时频谱 / 波形**。要的是"这一声有没有、多大"，RMS 已经够；
  频谱要另开一条 FFT 缓冲，收益不支持成本。
- **编辑器不自动静音失焦**（见 3.4）。
- **面板不写盘**。音量是会话级状态；要持久化得写 `game.json` 的 `audio`
  块，那是 `Project Settings` 那条的事（审计 D 类）。
