#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""离线生成音频测试素材（**提交产物**，与 gen_sdf_atlas.py / make_bc_dds.py 同款）。

为什么素材要"离线生成 + 提交"而不是运行时合成：
  · 解码器的验收必须对着**真实文件**跑（RIFF 解析、24-bit 打包、chunk 对齐
    这些 bug 在"内存里造一段 PCM"上根本暴露不出来）；
  · 生成过程必须**可复现**：全部是解析波形（sin / 指数包络），没有一个随机数。
    同一份脚本重新跑一遍，逐字节相同。

产物（`assets/audio/`）：

  tone_440_stereo.wav   1.0 s / 44100 / 立体声 / 16-bit / 440 Hz 满幅 0.5
                        —— 解码主用例：frames=44100 ch=2 sr=44100
  tone_440_48k.wav      1.0 s / 48000 / 立体声 / 16-bit
                        —— 过零率用例（输出 48k，不用重采样）
  tone_440_24bit.wav    与 tone_440_stereo 同一信号，24-bit 打包
                        —— 对照组：与 16-bit 归一化后 RMS 差 < 0.1%
  tone_1k_mono.wav      0.5 s / 48000 / 单声道 / 16-bit
                        —— 单声道 → 立体声输出的 pan 用例
  silence.wav           0.25 s / 44100 / 立体声 / 16-bit / **全 0**
                        —— 断言"样本精确等于 0"而不是"约等于 0"
  click.wav             20 ms / 48000 / 单声道 / 16-bit，1 ms 起音 + 指数衰减
                        —— 一次性音效（抢声部 / 淡出）用例
  loop_2s.wav           2.0 s / 44100 / 立体声 / 16-bit，440+660 Hz（整数周期）
                        —— 无缝循环：绕回点相位恰好为 0

⚠ **10 分钟长文件不提交**（105 MB）：用 `--long` 按需生成到仓库外的
  `../_venn_verify/`（G14-6 的流式验收用）。
"""

import argparse
import math
import os
import struct
import sys

SR44 = 44100
SR48 = 48000


def _f32(x):
    return max(-1.0, min(1.0, x))


def sine(freq, seconds, sr, amp, channels=1):
    """解析正弦。返回按帧交错的 float 列表（长 = frames*channels）。"""
    frames = int(round(seconds * sr))
    out = []
    for i in range(frames):
        v = amp * math.sin(2.0 * math.pi * freq * (i / float(sr)))
        for _ in range(channels):
            out.append(v)
    return out


def silence(seconds, sr, channels=1):
    frames = int(round(seconds * sr))
    return [0.0] * (frames * channels)


def click(seconds, sr, freq=2000.0, attack=0.001, decay=0.006):
    """一个带包络的短音：起音 attack 秒、之后指数衰减。

    指数用 `exp(-t/decay)` 而不是随机噪声 —— 必须可复现。
    """
    frames = int(round(seconds * sr))
    out = []
    for i in range(frames):
        t = i / float(sr)
        a = 1.0 if t >= attack else (t / attack)
        env = a * math.exp(-max(0.0, t - attack) / decay)
        out.append(0.9 * env * math.sin(2.0 * math.pi * freq * t))
    return out


def loop_2s(sr=SR44, seconds=2.0):
    """无缝循环素材：两个频率都取"在 seconds 内整数个周期"的值。

    440*2 = 880 周期、660*2 = 1320 周期 —— 都在边界处相位为 0，
    所以把尾巴接回头**没有样本跳变**（G14-6 的"循环 3 圈无跳变"用它）。
    """
    frames = int(round(seconds * sr))
    out = []
    for i in range(frames):
        t = i / float(sr)
        v = 0.35 * math.sin(2.0 * math.pi * 440.0 * t) + \
            0.20 * math.sin(2.0 * math.pi * 660.0 * t)
        out.append(v)
        out.append(v)
    return out


# ---------------------------------------------------------------- 写文件


def write_wav_16(path, samples, sr, channels):
    """标准 44 字节头的 16-bit PCM。"""
    frames = len(samples) // channels
    data = bytearray()
    for v in samples:
        s = int(round(_f32(v) * 32767.0))
        data += struct.pack("<h", s)
    byte_rate = sr * channels * 2
    block_align = channels * 2
    hdr = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE"
    hdr += b"fmt " + struct.pack("<IHHIIHH", 16, 1, channels, sr,
                                 byte_rate, block_align, 16)
    hdr += b"data" + struct.pack("<I", len(data))
    with open(path, "wb") as f:
        f.write(hdr)
        f.write(data)
    return frames


def write_wav_24(path, samples, sr, channels):
    """24-bit PCM：⚠ 每样本**打包成 3 字节**（不是 4 字节）。

    这是解码器最常见的手写 bug —— 按 4 字节读会整体错位，帧数却还是对的，
    听起来只是"噪音 + 变调"。
    """
    frames = len(samples) // channels
    data = bytearray()
    for v in samples:
        s = int(round(_f32(v) * 8388607.0))
        if s < 0:
            s += 1 << 24
        data += bytes((s & 0xFF, (s >> 8) & 0xFF, (s >> 16) & 0xFF))
    block_align = channels * 3
    byte_rate = sr * block_align
    hdr = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE"
    hdr += b"fmt " + struct.pack("<IHHIIHH", 16, 1, channels, sr,
                                 byte_rate, block_align, 24)
    hdr += b"data" + struct.pack("<I", len(data))
    with open(path, "wb") as f:
        f.write(hdr)
        f.write(data)
    return frames


def emit(out_dir):
    os.makedirs(out_dir, exist_ok=True)
    made = []

    def add(name, frames, sr, ch, extra=""):
        made.append((name, frames, sr, ch, extra))

    sig = sine(440.0, 1.0, SR44, 0.5, 2)
    n = write_wav_16(os.path.join(out_dir, "tone_440_stereo.wav"), sig, SR44, 2)
    add("tone_440_stereo.wav", n, SR44, 2, "440Hz/1s")

    n = write_wav_24(os.path.join(out_dir, "tone_440_24bit.wav"), sig, SR44, 2)
    add("tone_440_24bit.wav", n, SR44, 2, "24-bit 对照")

    sig48 = sine(440.0, 1.0, SR48, 0.5, 2)
    n = write_wav_16(os.path.join(out_dir, "tone_440_48k.wav"), sig48, SR48, 2)
    add("tone_440_48k.wav", n, SR48, 2, "48k")

    mono = sine(1000.0, 0.5, SR48, 0.5, 1)
    n = write_wav_16(os.path.join(out_dir, "tone_1k_mono.wav"), mono, SR48, 1)
    add("tone_1k_mono.wav", n, SR48, 1, "1kHz/0.5s")

    n = write_wav_16(os.path.join(out_dir, "silence.wav"),
                     silence(0.25, SR44, 2), SR44, 2)
    add("silence.wav", n, SR44, 2, "全 0")

    n = write_wav_16(os.path.join(out_dir, "click.wav"),
                     click(0.020, SR48), SR48, 1)
    add("click.wav", n, SR48, 1, "20ms 带包络")

    n = write_wav_16(os.path.join(out_dir, "loop_2s.wav"), loop_2s(), SR44, 2)
    add("loop_2s.wav", n, SR44, 2, "无缝循环")

    total = 0
    print("AUDIO-ASSETS dir=%s" % out_dir)
    for name, frames, sr, ch, extra in made:
        size = os.path.getsize(os.path.join(out_dir, name))
        total += size
        print("  %-22s frames=%-7d sr=%-6d ch=%d %8d B  %s"
              % (name, frames, sr, ch, size, extra))
    print("AUDIO-ASSETS total=%d files=%d" % (total, len(made)))
    return 0


def emit_long(path, seconds):
    """10 分钟长文件（**不提交**）：流式验收用。

    ⚠ 用单调递增的频率让它"听得出有内容"，同时保证可复现（解析函数）。
    """
    sr = SR44
    ch = 2
    frames = int(seconds * sr)
    d = os.path.dirname(os.path.abspath(path))
    if d:
        os.makedirs(d, exist_ok=True)
    block = 1 << 16
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 0) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, ch, sr,
                                      sr * ch * 2, ch * 2, 16))
        f.write(b"data" + struct.pack("<I", 0))
        written = 0
        buf = bytearray()
        for i in range(frames):
            t = i / float(sr)
            v = 0.3 * math.sin(2.0 * math.pi * 220.0 * t)
            s = int(round(v * 32767.0))
            buf += struct.pack("<h", s)
            buf += struct.pack("<h", s)
            written += 1
            if len(buf) >= block:
                f.write(buf)
                buf.clear()
        if buf:
            f.write(buf)
        # 回填尺寸（流式写入时先写 0，最后 seek 回来）
        size = 36 + written * ch * 2
        f.seek(4)
        f.write(struct.pack("<I", size))
        f.seek(40)
        f.write(struct.pack("<I", written * ch * 2))
    print("AUDIO-ASSETS long=%s seconds=%.1f frames=%d bytes=%d"
          % (path, seconds, frames, os.path.getsize(path)))
    return 0


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(root, "assets", "audio"))
    ap.add_argument("--long", default="", help="额外生成一个长 wav 到该路径")
    ap.add_argument("--long-seconds", type=float, default=600.0)
    args = ap.parse_args()
    rc = emit(args.out)
    if args.long:
        rc |= emit_long(args.long, args.long_seconds)
    return rc


if __name__ == "__main__":
    sys.exit(main())
