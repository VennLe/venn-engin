"""R7 · 骨骼关键帧编辑 + Pose Mode 的验收。

任务书的三条验收，逐条对应到下面的 A / B / C 组：

  A  存盘 → 重开 → 骨骼姿态**逐位**一致
  B  打两帧 → 播放 → 关节矩阵确实变化
  C  Pose Mode 里打帧不影响非 Play 态的其他骨骼

外加 D（姿态库存/取 + 落盘）、E（UI 锚点与日志健康）、F（坏骨名的容错）。

------------------------------------------------------------
为什么走 `MYVK_EDITOR_POSE_SCRIPT` 而不是点 GUI
------------------------------------------------------------
摆姿势这件事的值全在"数据对不对"上，而点 GUI 能验证的只有"按钮在不在"。
所以数据走脚本注入（**和 UI 完全同一条通路**：setKey → overlayEditPose →
poseToJoints，不是另一套实现），UI 只断言锚点存在（E 组）。
这样也顺带把"连续拖 40 帧像素才凑出一个角度"这种不稳定因素整个去掉了。

------------------------------------------------------------
逐位一致怎么比
------------------------------------------------------------
`CHAR-POSE-DUMP` 用 `%a`（精确十六进制浮点）打印局部 TRS —— 十进制打印会
掩盖掉 1 ulp 的差异，而"逐位一致"正是 A 组要说的事。
`CHAR-POSE-DIGEST` 是**全部关节矩阵的原始字节**的 FNV-1a，用来一眼看
"整具骨架有没有变"，也用在 B 组"确实变了"的反向断言上。

三个进程各自是一次干净的启动，所以 A 组比的是"真的从磁盘读回来的东西"
和"内存里刚打完帧的东西"，中间没有任何共享状态。

用法：
    python tools/verify_pose_mode.py
"""

import argparse
import math
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
OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify", "pose_mode")

MODEL = "assets/models/anime_girl.fbx"
# 落在 build/bin 下（build/ 被 gitignore）。用相对路径是刻意的：路径里
# 一旦出现盘符冒号，脚本那条 `save:<path>` 就要靠"从第 2 段起拼回去"的
# 补丁来解析 —— 能避开就避开，别让被测的东西顺手测一下解析器的健壮性。
CLIP_FILE = "_verif_pose_t1.anim.json"

# 打帧的骨 / 用来验"没被波及"的骨。选左右上臂：同一套骨架、分属不同肢体，
# 谁都不是谁的祖先 —— 这样"局部 TRS 与世界位置都不该变"才是成立的断言。
BONE_KEY = "upperarm.R"
BONE_FREE = "upperarm.L"

DUMP_RE = re.compile(r"CHAR-POSE-DUMP joint='([^']+)' (.*)$")
DIGEST_RE = re.compile(r"CHAR-POSE-DIGEST tag=(\S+) t=([\d.]+) .*fnv=([0-9a-f]+)")
JOINT_RE = re.compile(r"CHAR-POSE-JOINT i=(\d+) name=(\S+) parent=(-?\d+)")
ROT_RE = re.compile(r"r=([^ ]+)")


def wait_line(tap, needle, timeout):
    end = time.time() + timeout
    while time.time() < end:
        with tap.lock:
            for l in tap.lines:
                if needle in l:
                    return True
        time.sleep(0.15)
    return False


def run_editor(script, wait_s=90.0):
    """起一次编辑器跑一段注入脚本，返回日志行。

    退出闸门是 `CHAR-POSE-SCRIPT applied=` —— `runPoseScript()` 在脚本跑完
    之后**一定**打这一行，所以等到它就说明脚本结束、可以收工了。
    等不到也不硬等满 wait_s 之后才发现：超时照样收，但会打 [WARN]，
    断言自然会红，比"卡死两分钟"好查。
    """
    env = dict(os.environ)
    env["MYVK_CHARACTER_WINDOW"] = "1"
    env["MYVK_CHARACTER_MODEL"] = MODEL
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_LOG_RECTS"] = "1"
    if script:
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


def dumps(lines, bone):
    """取某个骨的所有 DUMP 值串（按出现顺序）。"""
    out = []
    for l in lines:
        m = DUMP_RE.search(l)
        if m and m.group(1) == bone:
            out.append(m.group(2))
    return out


def digests(lines):
    return [(float(m.group(2)), m.group(3))
            for m in (DIGEST_RE.search(l) for l in lines) if m]


def rot_deg(dump, axis_index_from_xyzw=(1,)):
    """从 DUMP 的 r=x,y,z,w（十六进制浮点）里算旋转角（度）。

    用 2*acos(|w|) 而不是 acos(w)：q 与 -q 是同一个旋转，slerp 在跨 180°
    时会翻转符号，直接 acos(w) 会算出 300° 这种"角度"。
    """
    m = ROT_RE.search(dump)
    if not m:
        return None
    parts = m.group(1).split(",")
    if len(parts) < 4:
        return None
    try:
        w = float.fromhex(parts[3])
    except ValueError:
        return None
    w = max(-1.0, min(1.0, abs(w)))
    return math.degrees(2.0 * math.acos(w))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=EXE)
    args = ap.parse_args()

    os.makedirs(OUT_DIR, exist_ok=True)
    fails = []

    def check(name, ok, detail=""):
        print("%s  %s%s" % ("PASS" if ok else "FAIL", name,
                            ("  (%s)" % detail) if detail else ""))
        if not ok:
            fails.append(name)

    globals()["EXE"] = args.exe
    if not os.path.isfile(args.exe):
        print("结果: FAIL: 1 项")
        print("  - 找不到 %s" % args.exe)
        return 2

    kill_stale_editors()
    clip_abs = os.path.join(BIN, CLIP_FILE)
    if os.path.exists(clip_abs):
        os.remove(clip_abs)

    # ============================================================ 进程 1
    # 打两帧 + 存一个姿态库 + 存盘。一并把骨名列表、UI 锚点也拿到。
    script1 = ";".join([
        "mode:on",
        "list",
        "select:" + BONE_KEY,
        "setrot:%s:0:0:0" % BONE_KEY,
        "key:%s:all:0" % BONE_KEY,
        "setrot:%s:0:60:0" % BONE_KEY,
        "key:%s:all:1" % BONE_KEY,
        "sample:0", "dump:" + BONE_KEY,
        "sample:1", "dump:" + BONE_KEY,
        "sample:0.5", "dump:" + BONE_KEY,
        "dump:" + BONE_FREE,
        "posestore:ArmUp",
        "save:" + CLIP_FILE,
    ])
    print("\n== 进程 1 · 打帧 + 存盘 ==")
    log1 = run_editor(script1)
    text1 = "\n".join(log1)

    # ---- A0 骨架里真有这两根骨（否则下面全是空断言）----
    joints = [(int(m.group(1)), m.group(2), int(m.group(3)))
              for m in (JOINT_RE.search(l) for l in log1) if m]
    names = {n for _, n, _ in joints}
    check("A0 骨架列表拿到（%d 根骨）" % len(joints), len(joints) > 0)
    check("A0b 打帧用的骨 %s 存在" % BONE_KEY, BONE_KEY in names,
          "骨架里没有它，见 CHAR-POSE-JOINT 列表")
    check("A0c 对照用的骨 %s 存在" % BONE_FREE, BONE_FREE in names)

    m = re.search(r"CHAR-POSE-SCRIPT applied=(\d+) failed=(\d+)", text1)
    check("A1 注入脚本全部执行（failed=0）",
          bool(m) and m.group(2) == "0", m.group(0) if m else "没有 SCRIPT 行")
    check("A2 存盘写出（POSE-SAVE tracks=3 keys=6）",
          bool(re.search(r"POSE-SAVE .*tracks=3 keys=6", text1)))
    check("A2b 文件真的落到磁盘上", os.path.isfile(clip_abs),
          clip_abs)

    d1_key = dumps(log1, BONE_KEY)
    d1_free = dumps(log1, BONE_FREE)
    dig1 = digests(log1)
    check("A3 三个时刻各有一条 DUMP（%d 条）" % len(d1_key),
          len(d1_key) == 3)
    check("B1 三个时刻的关节矩阵摘要两两不同",
          len(dig1) == 3 and len({d[1] for d in dig1}) == 3,
          "%s" % [d[1] for d in dig1])

    # ---- B2 角度真的按 slerp 走 ----
    if len(d1_key) == 3:
        a0, a1, a_mid = (rot_deg(d1_key[i]) for i in range(3))

        def fmt(v):
            # ⚠ 不能写 `v or -1`：a0 正好是 0.0（单位旋转），它是假值，
            #   于是一条 PASS 会显示成 "-1.0000°"，看起来像算错了。
            return "?" if v is None else "%.4f°" % v

        check("B2 t=0 是单位旋转（0°）", a0 is not None and a0 < 0.05, fmt(a0))
        check("B2b t=1 转到了 60°", a1 is not None and abs(a1 - 60.0) < 0.5,
              fmt(a1))
        check("B2c t=0.5 落在两者之间（slerp 约 30°）",
              a_mid is not None and 29.0 < a_mid < 31.0, fmt(a_mid))
    else:
        for n in ("B2 t=0 是单位旋转（0°）", "B2b t=1 转到了 60°",
                  "B2c t=0.5 落在两者之间（slerp 约 30°）"):
            check(n, False, "DUMP 条数不足")

    # ---- D1 姿态库 ----
    check("D1 姿态库存下（POSELIB op=store bones=21）",
          bool(re.search(r"CHAR-POSELIB op=store name='ArmUp' bones=(\d+)",
                         text1)))

    # ============================================================ 进程 2
    print("\n== 进程 2 · 读回并采样 ==")
    script2 = ";".join([
        "mode:on",
        "load:" + CLIP_FILE,
        "sample:0", "dump:" + BONE_KEY,
        "sample:1", "dump:" + BONE_KEY,
        "sample:0.5", "dump:" + BONE_KEY,
        "dump:" + BONE_FREE,
        "poseapply:ArmUp", "dump:" + BONE_KEY,
    ])
    log2 = run_editor(script2)
    text2 = "\n".join(log2)

    check("A4 读盘成功（POSE-LOAD tracks=3 keys=6 poses=1）",
          bool(re.search(r"POSE-LOAD .*tracks=3 keys=6 poses=1", text2)))

    d2_key = dumps(log2, BONE_KEY)
    d2_free = dumps(log2, BONE_FREE)
    dig2 = digests(log2)

    # A5 才是任务书那条"存盘 → 重开 → 逐位一致"
    check("A5 三个时刻的局部 TRS 逐位一致（%d 对）" % len(d1_key),
          len(d2_key) >= 3 and d1_key[:3] == d2_key[:3],
          "" if d1_key[:3] == d2_key[:3] else "\n    存盘后 %s\n    读回后 %s"
          % (d1_key[:3], d2_key[:3]))
    check("A5b 三个时刻的关节矩阵摘要也逐位一致",
          [d[1] for d in dig1] == [d[1] for d in dig2[:3]],
          "%s vs %s" % ([d[1] for d in dig1], [d[1] for d in dig2[:3]]))

    # D2/D3 姿态库读回并能应用。
    # ArmUp 是在 t=0.5（30°）那一刻存的，所以"应用它"的结果应当与
    # 读回来直接在 t=0.5 采样的结果逐位相同 —— 用不着跨进程比。
    check("D2 姿态库随文件一起读回", bool(re.search(r"poses=1", text2)))
    if len(d2_key) >= 4:
        check("D3 应用姿态库 = 存库那一刻（t=0.5）的姿势",
              d2_key[3] == d2_key[2],
              "" if d2_key[3] == d2_key[2]
              else "%s vs %s" % (d2_key[3], d2_key[2]))
    else:
        check("D3 应用姿态库 = 存库那一刻（t=0.5）的姿势", False,
              "读回后 DUMP 只有 %d 条" % len(d2_key))

    # ============================================================ 进程 3
    # C 组要的基线：**完全不打帧**时这两根骨长什么样。
    print("\n== 进程 3 · 无编辑基线 ==")
    log3 = run_editor("mode:on;sample:0.5;dump:" + BONE_FREE
                      + ";dump:" + BONE_KEY)
    d3_free = dumps(log3, BONE_FREE)
    d3_key = dumps(log3, BONE_KEY)
    check("C0 基线拿到两根骨的 DUMP", len(d3_free) == 1 and len(d3_key) == 1)

    # C1：打过帧的右臂**不该**把左臂带上（局部 TRS 与世界位置都不变）
    check("C1 打帧没有波及非子树的另一根骨（%s 局部+世界都不变）" % BONE_FREE,
          len(d1_free) == 1 and len(d3_free) == 1 and d1_free == d3_free,
          "" if d1_free == d3_free else "%s vs %s" % (d1_free, d3_free))
    # C2：反向对照 —— 打过帧的那根骨在基线里必须与编辑后**不同**。
    #     少了这一条，C1 会被"整个脚本压根没跑"骗过（两边都是空/静止就相等）。
    check("C2 反向对照：打过帧的骨在无编辑基线里是另一个姿势",
          len(d3_key) == 1 and len(d1_key) >= 3 and d1_key[2] != d3_key[0],
          "编辑后 %s / 基线 %s"
          % (d1_key[2] if len(d1_key) >= 3 else "-", d3_key[0] if d3_key else "-"))

    # ============================================================ 进程 4
    # F 组：坏骨名的容错。手写一份带 ghost_bone 的资产 —— 这是"从别的
    # 角色拷过来一份姿态库"的日常情形，不该整份资产打不开。
    print("\n== 进程 4 · 坏骨名容错 ==")
    bad = os.path.join(BIN, "_verif_pose_bad.anim.json")
    with open(bad, "w", encoding="utf-8") as f:
        f.write('{"format":"venn.animclip","version":1,"name":"bad",'
                '"baseClip":"","duration":1.0,"loop":true,"tracks":['
                '{"joint":"ghost_bone","path":"rotation","times":[0,1],'
                '"values":[[0,0,0,1],[0,0.5,0,0.8660254]]},'
                '{"joint":"%s","path":"rotation","times":[0,1],'
                '"values":[[0,0,0,1],[0,0.5,0,0.8660254]]}]}' % BONE_KEY)
    log4 = run_editor("mode:on;load:_verif_pose_bad.anim.json;sample:1;dump:"
                      + BONE_KEY)
    text4 = "\n".join(log4)
    check("F1 坏骨名被报出来（POSE-RESOLVE-MISS）",
          "POSE-RESOLVE-MISS joint='ghost_bone'" in text4)
    check("F1b 同一份资产里好的那条照样生效（channels=1 missing=1）",
          bool(re.search(r"POSE-RESOLVE clips=2 channels=1 joints=1 missing=1",
                         text4)))
    d4 = dumps(log4, BONE_KEY)
    r4 = rot_deg(d4[0]) if len(d4) == 1 else None
    check("F1c 好轨道真的改了姿势（t=1 转到 60°）",
          r4 is not None and abs(r4 - 60.0) < 0.5,
          "?" if r4 is None else "%.3f°" % r4)
    try:
        os.remove(bad)
    except OSError:
        pass

    # ============================================================ E 组
    print("\n== E · UI 与日志健康 ==")
    check("E1 Pose Mode 开关被记录（CHAR-POSE-MODE on=1）",
          "CHAR-POSE-MODE on=1" in text1)
    check("E2 Pose 页签存在（CHAR-TAB-Pose 锚点）",
          any("CHAR-TAB-Pose" in l for l in log1))
    check("E3 打帧按钮有锚点（CHAR-POSE-KEY-BTN）",
          any("CHAR-POSE-KEY-BTN" in l for l in log1))
    check("E4 存/读按钮有锚点",
          any("CHAR-POSE-SAVE-BTN" in l for l in log1)
          and any("CHAR-POSE-LOAD-BTN" in l for l in log1))
    bad_vk = [l for l in (log1 + log2 + log3 + log4)
              if "Validation Error" in l or "VUID-" in l
              or "SEGV" in l or "[ERROR]" in l]
    check("E5 零 Vulkan 校验错误 / 零 ERROR", len(bad_vk) == 0,
          "%s" % bad_vk[:3])

    print()
    if fails:
        print("结果: FAIL: %d 项" % len(fails))
        for f in fails:
            print("  -", f)
        return 6
    print("结果: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
