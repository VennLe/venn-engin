#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""行走循环生成器（GaitCycle）的验收。

背景：用户给了一份 Rigify 人形「24fps · 24 帧行走循环」的逐帧 K 帧表。
那张表是**手工摆一遍**的工作流，搬到引擎里要解决手工流程天然做不到的
三件事（换角色就作废 / 脚滑看不出来 / 表是死的）。生成器把这三件事变成
参数 + 可量的毫米数，这个脚本验的就是"真的落地了"：

  A  生成成功，且**逐帧烘焙**真的是一帧一个键（时间严格是 k/24）
  B  承重期脚掌不滑（侧向 / 竖直 / 直线偏差都是毫米级）
  C  无缝循环（闭合 + 半周期镜像）
  D  **原地走**：髋的平移只有上下与左右，前向不动（treadmill）
  E  UI 接上了线（锚点）+ 零 Vulkan 错误
  F  步幅匹配：**模型里那段 Walk** 的自然步速量得出来（A4 的接线前提）
  G  **run 段**：走与跑的三条普适关系（更快 / 承重更少 / 腾空更多）
     —— 用相对关系而不是教科书阈值，理由见 G 组开头的注释

------------------------------------------------------------
为什么既有引擎里的自检、又要这个脚本
------------------------------------------------------------
`MYVK_ANIM_TEST` 那组用的是**手搭的**人形骨架（数值可以手算），跑的是
纯函数；这个脚本用的是**真实的 fbx 资产**、走的是面板那条路
（`generateWalk()` → `m_edit` → 存盘），最后用 Python **直接读盘上的
JSON** 当独立 oracle。两边都过，才说明"换个骨架也能用"和"界面点得到"
同时成立 —— 只测其中一边，另一半坏了都不会被发现。

------------------------------------------------------------
判据为什么不比截图
------------------------------------------------------------
脚滑 3 mm 与滑 30 mm 在 1080p 上是同一只脚，而后者毁掉整段走路。
同理"原地走"和"每帧往前挪 2 cm"在单帧画面上完全一样。
这些只能断数值。

用法：
    python tools/verify_gait_cycle.py
结尾一定是 `结果: PASS` / `结果: FAIL: ...`（ASCII 冒号）。
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verify_add_menu import LogTap, kill_stale_editors          # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
EXE = os.path.join(BIN, "Editor.exe")
OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify", "gait")
MODEL = "assets/models/characters/animate_girl.fbx"

# 落在 build/bin 下（build/ 被 gitignore）。用**相对名**：路径里一旦出现
# 盘符冒号，脚本命令那条 `save:<path>` 就要靠"从第 2 段起拼回去"来解析。
CLIP_BAKED = "_verif_gait.anim.json"
CLIP_BLOCK = "_verif_gait_block.anim.json"

FPS = 24.0
FRAMES = 48
BOB = 0.030          # GaitParams 的默认半振幅
SWAY = 0.030

CHECK_RE = re.compile(
    r"GAIT-CHECK slide=([-\d.]+)mm vert=([-\d.]+)mm line=([-\d.]+)mm "
    r"mirror=([-\d.]+)mm rest=([-\d.]+)mm loop=([-\d.]+)mm bob=([-\d.]+) "
    r"sway=([-\d.]+) fwd=([-\d.]+)mm lift=([-\d.]+) leg=([-\d.]+) "
    r"arm=([-\d.]+) armdown=([-\d.]+)deg reach=([-\d.]+) speed=([-\d.]+)")
GAIT_RE = re.compile(r"CHAR-GAIT op=generate ok=(\d+) tracks=(\d+) keys=(\d+)")
# 步幅匹配（A4）。注意锚点前面还有 CHAR-STRIDE-MATCH / -RANGE / -BTN 三条
# （面板控件的矩形日志），所以这里必须带 `clip=` 才不会被它们误命中。
# 字段顺序与 `CharacterWindow::measureStride` 的格式串逐字对应 —— 改一处
# 必须改两处（错位时 group 不越界，只会静默读错字段，那比崩了更难查）。
STRIDE_RE = re.compile(
    r"CHAR-STRIDE clip=(\S+) ok=(\d+) nominal=([-\d.]+) cycle=([-\d.]+)s "
    r"duty=([-\d.]+) air=([-\d.]+) samples=(\d+) contact=(\d+) "
    r"missing=(\d+) travel=([-\d.]+)/([-\d.]+) low=([-\d.]+)/([-\d.]+) "
    r"rise=([-\d.]+) toe=(\d+) abs=(\d+) span=([-\d.]+)/([-\d.]+) "
    r"err='(.*)'")


def wait_line(tap, needle, timeout):
    end = time.time() + timeout
    while time.time() < end:
        with tap.lock:
            for l in tap.lines:
                if needle in l:
                    return True
        time.sleep(0.15)
    return False


def run_editor(script, wait_s=120.0):
    """起一次编辑器跑一段注入脚本，返回日志行。

    退出闸门是 `CHAR-POSE-SCRIPT applied=`：`runPoseScript()` 在脚本跑完
    之后**一定**打这一行。
    """
    env = dict(os.environ)
    env["MYVK_CHARACTER_WINDOW"] = "1"
    env["MYVK_CHARACTER_MODEL"] = MODEL
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_EDITOR_POSE_SCRIPT"] = script

    proc = subprocess.Popen([EXE], cwd=BIN, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    if not wait_line(tap, "CHAR-POSE-SCRIPT applied=", wait_s):
        print("[WARN] 等不到 CHAR-POSE-SCRIPT（%ds）" % int(wait_s))
    # 再给一帧：UI 锚点是在脚本**之后**的绘制里打的
    time.sleep(1.2)
    try:
        proc.terminate()
        proc.wait(timeout=8)
    except subprocess.TimeoutExpired:
        proc.kill()
        try:
            proc.wait(timeout=8)
        except subprocess.TimeoutExpired:
            pass
    tap.t.join(timeout=8)
    with tap.lock:
        lines = list(tap.lines)
    return [l.replace("\x00", "") for l in lines]


def main() -> int:
    fails = []

    def check(name, ok, detail=""):
        print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name,
                               ("  — " + detail) if detail else ""))
        if not ok:
            fails.append(name + (("  — " + detail) if detail else ""))

    print("=" * 72)
    print("行走循环生成器验收（真实 fbx 资产 + 面板同一条通路）")
    print("=" * 72)

    if not os.path.isfile(EXE):
        print("结果: FAIL: 找不到 %s（先 ./make.cmd build）" % EXE)
        return 1
    kill_stale_editors()

    for f in (CLIP_BAKED, CLIP_BLOCK):
        try:
            os.remove(os.path.join(BIN, f))
        except OSError:
            pass

    # 一次进程跑两遍：先逐帧烘焙，再 blocking。第二遍会覆盖 m_edit，
    # 所以每遍之后立刻落盘 —— 两份文件是**两次不同参数**的产物。
    #
    # `stride:` 三条是 A4 的（步幅匹配）：
    #   · `stride:Walk` / `stride:Run` —— 量**模型里**那两段（Play 会播的
    #     就是它们）。`gait:` 生成的是编辑片段，与它们**是两个对象**，
    #     面板与运行时用的是模型里那份，所以必须单独量。
    #     两段都量是为了 G 组：走/跑的关系（更快、承重更少、腾空更多）
    #     是**唯一**不依赖教科书数值的判据 —— 单看一段的话，"duty=0.28"
    #     到底是走是跑说不清。
    #   · `stride:NoSuchClip`  —— **否定测试**：不存在的 clip 必须 ok=0。
    #     没有它的话，"ok=1"也可能只是日志里写死的一个 1。
    script = ("mode:on;gait:%f:%d:0;save:%s;"
              "stride:Walk;stride:Run;stride:NoSuchClip;stridecurve:1;"
              "gait:%f:%d:1;save:%s" % (0.6, FRAMES, CLIP_BAKED, 0.6, FRAMES,
                                        CLIP_BLOCK))
    log = run_editor(script)
    text = "\n".join(log)

    # ============================================================ A
    print("\n== A · 生成与烘焙 ==")
    gs = [GAIT_RE.search(l) for l in log]
    gs = [m for m in gs if m]
    check("A1 生成器跑了两次（逐帧 + blocking）", len(gs) == 2,
          "n=%d" % len(gs))
    if gs:
        ok_ = gs[0].group(1)
        tracks = int(gs[0].group(2))
        keys = int(gs[0].group(3))
        check("A2 逐帧烘焙 ok=1", ok_ == "1", "ok=%s" % ok_)
        # 18 = 17 根骨的旋转 + 髋的平移。少一根骨就掉一条轨。
        check("A3 轨道数 18", tracks == 18, "tracks=%d" % tracks)
        check("A4 键数 882（18×49）", keys == 18 * (FRAMES + 1),
              "keys=%d" % keys)

    path = os.path.join(BIN, CLIP_BAKED)
    bpath = os.path.join(BIN, CLIP_BLOCK)
    clip = None
    if os.path.isfile(path):
        with open(path, "r", encoding="utf-8") as fh:
            clip = json.load(fh)
    check("A5 逐帧产物落盘了", clip is not None, path)
    if clip is None:
        print("\n结果: FAIL: 没有产物，后面的断言无从谈起")
        return 1

    tracks = clip.get("tracks", [])
    check("A6 盘上 18 条轨道", len(tracks) == 18, "tracks=%d" % len(tracks))
    check("A7 时长 2.000 s", abs(clip.get("duration", 0.0) - 2.0) < 1e-6,
          "duration=%s" % clip.get("duration"))
    check("A8 loop=true（无缝循环的前提）", clip.get("loop") is True)

    # 逐帧烘焙的**定义**：每条轨道都有 49 个键、时间严格是 k/24。
    # 这一条比"keys=882"更狠：882 也可以由"每帧都有但时间错位"凑出来。
    bad_time = 0
    for t in tracks:
        ts = t.get("times", [])
        if len(ts) != FRAMES + 1:
            bad_time += 1
            continue
        for k, v in enumerate(ts):
            if abs(v - k / FPS) > 1e-5:
                bad_time += 1
                break
    check("A9 每条轨 49 个键、时间 = k/24", bad_time == 0,
          "bad=%d" % bad_time)

    # ============================================================ B
    print("\n== B · 承重期脚掌不滑 ==")
    ms = [m for m in (CHECK_RE.search(l) for l in log) if m]
    check("B0 找到 GAIT-CHECK", bool(ms), "n=%d" % len(ms))
    if ms:
        v = [float(x) for x in ms[0].groups()]
        (slide, vert, line, mirror, rest, loop) = v[0:6]
        (bob, sway, fwd, lift, leg, arm, armdown) = v[6:13]
        (reach, speed) = v[13:15]
        # 表 §6 的硬规则：承重期脚掌 X/Z 完全不动。阈值 0.5 mm 是"肉眼
        # 绝对看不出、但插值没管约束会立刻超标一个数量级"的位置。
        check("B1 侧向漂移 < 0.5 mm", slide < 0.5, "slide=%.3fmm" % slide)
        check("B2 竖直漂移 < 0.5 mm", vert < 0.5, "vert=%.3fmm" % vert)
        check("B3 前向偏离直线 < 0.5 mm", line < 0.5, "line=%.3fmm" % line)
        # 表 §9：重心上下 ±0.02~0.04、左右 ±0.03 → 峰峰翻倍。
        check("B4 重心上下峰峰 ≈ 0.06 m", abs(bob - 2 * BOB) < 0.004,
              "bob=%.4f" % bob)
        check("B5 重心左右峰峰 ≈ 0.06 m", abs(sway - 2 * SWAY) < 0.004,
              "sway=%.4f" % sway)
        check("B6 摆动相抬脚 ≈ 0.10 m", abs(lift - 0.10) < 0.01,
              "lift=%.4f" % lift)
        # 反相要**两条符号一起看**：触地帧同侧脚在前、同侧手在后。
        # 只看其中一条，"同手同脚"也可能让那一条为正。
        check("B7a 触地帧左脚在前", leg > 0.3, "leg=%.4f" % leg)
        check("B7b 触地帧左臂在后（同手同脚检测）", arm > 0.15,
              "arm=%.4f" % arm)
        # >1 = IK 够不着 → 腿拉直、脚离地 → 看起来就是在滑。
        check("B8 腿够得着（reach < 1）", 0.0 < reach < 1.0,
              "reach=%.4f" % reach)
        # 这只资产静止姿势自带 0.4~0.6 m 的左右不对称，镜子判据必须先
        # 扣掉它（见 GaitCycle.h 的 restMirrorMm）。把基线打出来，
        # 免得以后有人看见 mirror≈0 而 rest 很大时以为哪边算错了。
        print("        （资产自带左右不对称 rest=%.1fmm，已在镜像判据里扣掉）"
              % rest)

    # ============================================================ C
    print("\n== C · 无缝循环 ==")
    if ms:
        check("C1 闭合帧与首帧一致（< 0.05 mm）", loop < 0.05,
              "loop=%.4fmm" % loop)
        # ⚠⚠ 镜像这一项在**这只资产上没有判据资格**，所以只记录不判红。
        #
        #   "帧 25 = 帧 1 的左右镜像"是**逐点**的说法，它只在导入姿势本身
        #   左右对称时才可能成立。这只资产的静止姿势自带 rest=580 mm 的
        #   不对称（一只脚在前、两条手臂朝向差很多），生成器再怎么摆也
        #   不可能把它抹平 —— 写成 `mirror < 0.05` 就是一条**永远红**的
        #   断言，而红着不管比没有还糟（以后真出问题没人看）。
        #
        #   真正的判据在 `MYVK_ANIM_TEST` 的 `gait_mirror_mm`：那里用的是
        #   手搭的**对称**人形骨架，同一个量实测 0.000007 mm。两边合起来
        #   才成立：对称骨架上镜像精确成立（夹具证），不对称的骨架上
        #   误差由资产自带的不对称解释（这里记录）。
        print("  [记录] C2 半周期镜像 mirror=%.2fmm（资产自带不对称 "
              "rest=%.2fmm，无判据资格）" % (mirror, rest))

    # ============================================================ D
    print("\n== D · 原地走（treadmill）==")
    # 独立 oracle：直接读盘上的**髋平移轨道**，不看引擎报了什么。
    # ⚠ 不能用"某个轴不动"当判据：bob/sway 是沿骨架自己的 up/left 加的，
    #   而这条资产的骨架朝向和世界轴不重合（三个轴的峰峰分别是
    #   0.055 / 0.043 / 0.061，一个都不为 0）。**与朝向无关**的判据是
    #   "首尾相同"（没有净位移），而"前向零位移"由引擎侧按 g.fwd 投影量
    #   （`GAIT-CHECK` 的 fwd=）。
    hips = [t for t in tracks
            if "hips" in str(t.get("joint", "")).lower()
            and t.get("path") == "translation"]
    check("D0 找到髋平移轨道", len(hips) == 1, "n=%d" % len(hips))
    if hips:
        vals = hips[0].get("values", [])
        if vals:
            first, last = vals[0], vals[-1]
            drift = max(abs(a - b) for a, b in zip(first[:3], last[:3]))
            check("D1 髋没有净位移（首尾相同，< 0.1 mm）", drift < 1e-4,
                  "drift=%.6f" % drift)
        else:
            check("D1 髋没有净位移（首尾相同，< 0.1 mm）", False, "空轨道")
    if ms:
        check("D2 引擎侧：髋前向零位移（< 0.5 mm）",
              float(ms[0].group(9)) < 0.5, "fwd=%.3fmm" % float(ms[0].group(9)))

    # ============================================================ E
    print("\n== E · blocking 对照与 UI 接线 ==")
    bclip = None
    if os.path.isfile(bpath):
        with open(bpath, "r", encoding="utf-8") as fh:
            bclip = json.load(fh)
    check("E1 blocking 产物落盘了", bclip is not None, bpath)
    if bclip is not None:
        bt = bclip.get("tracks", [])
        nk = [len(t.get("times", [])) for t in bt]
        check("E2 blocking 每条轨只有 9 个键",
              bool(nk) and max(nk) == 9, "max=%s" % (max(nk) if nk else None))
    # **对照组**：只打关键姿态时承重期会滑。不成立就说明 B 组那三个毫米数
    # 是"盲"的（任何输入都报 0，等于没测）。
    bs = [m for m in (CHECK_RE.search(l) for l in log) if m]
    if len(bs) >= 2:
        bvert = float(bs[1].group(2))
        check("E3 对照组：blocking 确实滑（> 2 mm）", bvert > 2.0,
              "vert=%.2fmm" % bvert)

    check("E4 生成按钮有锚点（CHAR-GAIT-BTN）",
          any("CHAR-GAIT-BTN" in l for l in log))
    check("E5 参数控件有锚点（STEP / FRAMES / BLOCKING）",
          all(any(tag in l for l in log)
              for tag in ("CHAR-GAIT-STEP", "CHAR-GAIT-FRAMES",
                          "CHAR-GAIT-BLOCKING")))
    # ⚠ `[ERROR] Texture load failed` 是**这只资产自带的**：它的材质指向
    #   作者机器上的绝对路径（E:/dev/UE-project/...）。它跟步态无关，
    #   而且 `verify_pose_mode` 用同一个 fbx 也会撞上。这里只拦**与本次
    #   改动有关**的坏信号：Vulkan 校验、崩溃、脚本命令没解析。
    bad = [l for l in log
           if "Validation Error" in l or "VUID-" in l or "SEGV" in l
           or "CHAR-POSE-SCRIPT-BAD" in l
           or ("[ERROR]" in l and "Texture load failed" not in l)
           or "CHAR-GAIT op=generate ok=0" in l]
    # ⚠ `stride:NoSuchClip` 是 F6 那条**故意的**否定测试：脚本命令返回
    #   false 时 `runPoseScript()` 本来就会打一行 CHAR-POSE-SCRIPT-BAD。
    #   把它排掉，剩下的才算真失败（否则"测了否定案例"和"解析真的坏了"
    #   在日志里长得一模一样）。
    bad = [l for l in bad if "CHAR-POSE-SCRIPT-BAD tok='stride:" not in l]
    check("E6 零 Vulkan 错误 / 零崩溃 / 零脚本解析失败 / 生成无失败", not bad,
          "%s" % bad[:3])

    # ============================================================ F
    print("\n== F · 步幅匹配（A4，真实资产）==")
    # 这一段验的是"接线的前提"：`Locomotion` 每帧算播放速率用的就是这个
    # 量。量不出来 → 它退回 1×（不报错），所以"能不能量"必须单独钉住，
    # 否则 A4 在真实资产上等于没接。
    ss = [m for m in (STRIDE_RE.search(l) for l in log) if m]
    check("F0 步幅命令跑了三次（Walk + Run + 不存在的 clip）", len(ss) == 3,
          "n=%d" % len(ss))
    if ss:
        byClip = {m.group(1): m for m in ss}
        w = byClip.get("Walk")
        check("F1 模型里的 Walk 量得出自然步速", w is not None and w.group(2) == "1",
              "clip=%s" % list(byClip))
        if w is not None:
            nom = float(w.group(3))
            duty = float(w.group(5))
            n_s = int(w.group(7))
            n_c = int(w.group(8))
            miss = int(w.group(9))
            # 人形走路的地面速度就在 0.3~3.0 m/s 之间。落在外面说明量错了
            # （比如把"脚相对髋"的竖直量当成了前向 —— 那是 bob，不是步幅）。
            check("F2 自然步速在合理区间（0.3~3.0 m/s）", 0.3 < nom < 3.0,
                  "nominal=%.3f m/s" % nom)
            # ⚠ 这里**不**断言"duty 像教科书走路（0.5~0.62）"：真实资产的
            #   动画未必是教科书步态 —— `animate_girl` 的 Walk 实测 0.28
            #   （大部分时间脚在空中，是一种轻快的走法）。硬套教科书值只会
            #   让判据跟着某一个资产走。这里只要求"有承重期、且不占满整圈"，
            #   走/跑的区别交给 G 组用**相对关系**判。
            check("F3 承重期存在且不占满整圈（0.08~0.92）",
                  0.08 < duty < 0.92, "duty=%.2f" % duty)
            check("F4 采样与承重样本都够多", n_s >= 16 and n_c >= 6,
                  "samples=%d contact=%d" % (n_s, n_c))
            check("F5 角色表没缺腿（缺失 ≤ 3 个非必要角色）", miss <= 3,
                  "missing=%d" % miss)
            # 脚底是否用上了脚尖骨（`min(踝, 脚尖)`）。false = 这具骨架没有
            # 脚尖骨，承重期会被低估 —— 那是资产的限制，但**必须能看出来**，
            # 否则"duty 偏小"会被当成算法错。
            check("F5b 脚底用上了脚尖骨（soleFromToe）", w.group(15) == "1",
                  "toe=%s" % w.group(15))
        # **否定测试**：不存在的 clip 必须明确失败。它绿了才说明 F1 那个
        # ok=1 是量出来的，不是日志里写死的一个 1。
        nc = byClip.get("NoSuchClip")
        check("F6 不存在的 clip → ok=0（否定测试）",
              nc is not None and nc.group(2) == "0", "err=%s"
              % (nc.group(19) if nc else "N/A"))
    check("F7 步幅匹配的 UI 有锚点（MATCH / RANGE / BTN）",
          all(any(tag in l for l in log)
              for tag in ("CHAR-STRIDE-MATCH", "CHAR-STRIDE-RANGE",
                          "CHAR-STRIDE-BTN")))
    # A4b 相位速率曲线的开关也必须点得到（"界面点不到 = 没交付"）。
    # 脚本命令与面板复选框走同一个 setter，所以这一条同时验证了两条路
    # 都没有各写一份状态。
    check("F8 相位速率曲线能开关（锚点 + 脚本命令同一条 setter）",
          any("CHAR-STRIDE-CURVE" in l for l in log), "")

    # ============================================================ G
    print("\n== G · run 段的量法（走 / 跑的相对关系）==")
    #
    # 为什么要用**相对关系**而不是绝对阈值
    # ------------------------------------------------------------
    # 教科书的数字是"走路 duty 0.6、跑步 0.35"。但真实资产的动画未必
    # 符合：`animate_girl` 的 Walk 大部分时间脚在空中（duty 0.28），
    # `alicia` 的 Walk 也只有 0.43。拿教科书值当判据，只会让判据跟着
    # 某一个资产走 —— 换一只资产就红，而算法其实没错。
    #
    # 真正与资产无关的，是**走与跑之间**的三条普适关系：
    #   ① 跑步更快        nominal(Run) > nominal(Walk)
    #   ② 跑步承重更少    duty(Run)    < duty(Walk)
    #   ③ 跑步腾空更多    air(Run)     > air(Walk)
    # 这三条在任何正常制作的步态上都成立，而且**任意一条单独都不够**：
    # 只比速度的话，"同一段动画按 2 倍速播"也能满足；只比 duty 的话，
    # 一段跑步动画被剪短也会满足。三条一起才是"量法真的分得开走跑"。
    if ss:
        w = byClip.get("Walk")
        rn = byClip.get("Run")
        check("G0 Run 段量得出来（ok=1）",
              rn is not None and rn.group(2) == "1",
              "err=%s" % (rn.group(19) if rn else "N/A"))
        if w is not None and rn is not None and rn.group(2) == "1":
            nom_w, nom_r = float(w.group(3)), float(rn.group(3))
            duty_w, duty_r = float(w.group(5)), float(rn.group(5))
            air_w, air_r = float(w.group(6)), float(rn.group(6))
            check("G1 跑步更快（nominal(Run) > nominal(Walk)）",
                  nom_r > nom_w * 1.2,
                  "walk=%.2f run=%.2f" % (nom_w, nom_r))
            check("G2 跑步承重更少（duty(Run) < duty(Walk)）",
                  duty_r < duty_w,
                  "walk=%.2f run=%.2f" % (duty_w, duty_r))
            check("G3 跑步腾空更多（air(Run) > air(Walk)）",
                  air_r > air_w,
                  "walk=%.2f run=%.2f" % (air_w, air_r))
            # 交叉校验（两条都是）：脚相对身体的摆幅折算成速度，与"承重期
            # 脚速"应当同量级。差得远 = 承重样本被污染（中位数被离群速度
            # 带跑），而那种错在 duty / air 上都看不出来。
            # 容差给 2.5 倍很宽：摆幅法对跑步天然偏小（脚在空中划圈），
            # 这里只拦"差一个数量级"那种错。
            for tag, m in (("Walk", w), ("Run", rn)):
                nom = float(m.group(3))
                span_v = float(m.group(18))
                check("G4 交叉校验 %s：摆幅法 / 承重期法 同量级（0.4~2.5）"
                      % tag,
                      nom > 0 and 0.4 < span_v / nom < 2.5,
                      "span=%.2f nominal=%.2f 比=%.2f"
                      % (span_v, nom, span_v / nom))

    # ============================================================
    # 收尾：把产物挪到 _venn_verify 下留档（build/ 每次重建会被清掉）
    try:
        os.makedirs(OUT_DIR, exist_ok=True)
        for f in (CLIP_BAKED, CLIP_BLOCK):
            src = os.path.join(BIN, f)
            if os.path.isfile(src):
                dst = os.path.join(OUT_DIR, f)
                if os.path.isfile(dst):
                    os.remove(dst)
                os.replace(src, dst)
    except OSError as e:
        print("[WARN] 留档失败：%s" % e)

    print()
    print("=" * 72)
    if fails:
        print("结果: FAIL: %d 项" % len(fails))
        for f in fails:
            print("  - %s" % f)
        print("=" * 72)
        return 6
    print("结果: PASS")
    print("=" * 72)
    return 0


if __name__ == "__main__":
    sys.exit(main())
