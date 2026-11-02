#!/usr/bin/env python3
# ============================================================
# verify_flags —— 剧情旗标（A4）
#
# ---------------------------- 这一刀在验什么 ----------------------------
# 2026-11-02 之前：
#   · 脚本里 `setFlag` / `getFlag` 从 10 月就可用（`engine/script`），
#     但**编辑器一处入口都没有**（`grep -rn setFlag editor/` → 0）；
#   · 更糟的是编辑器 Play 态**根本没接** `svc.act.setFlag` ——
#     Play 里调用它报的是 "setFlag() is not available in this host"。
#     于是剧本里的分支在编辑器里等于不存在，**只能出包才能试**。
#
# 所以这一刀验的不是"有个面板"，而是**四个断言合起来**才成立的链：
#
#   A. 内核（`--selftest project` 的 FLAG 组，23 条）
#      未设过 = 0 / 同名覆盖不累加 / writer 记最后一次 /
#      快照恢复是**整表替换**（那一章之后才出现的旗标必须消失）。
#   B. 面板 + 命令（编辑态）：改 → 读 → 删 → 清 → 快照 → 恢复
#      ⚠ 命令全部走**面板的方法**，与按钮同一条路（"能点不能存"是这类
#        面板最常见的缺陷，分开写两套的话判据只覆盖其中一条）。
#   C. **Play 里脚本写的旗标真的落地了**（这一条是整个 A4 的核心）
#      夹具脚本 `_flags_probe.vks` 在 `on start` 写两个、在 `on update`
#      每帧自增一个：
#        C1 日志里**没有** "not available in this host"（反向判据）
#        C2 `met_alice` = 1 且 `set=1`
#        C3 `ticks` 已经涨到 >= 20（证明是**每帧**在写，不是一次性的）
#        C4 `writer` = `script:scripts/_flags_probe.vks`（谁写的）
#   D. 新一局清空：编辑态先塞一个脚本不会写的键，进 Play 之后它必须没了
#      ⚠ 这条最容易被漏掉：不清的话"上一局见过的角色"会带进下一局，
#        而**不会报任何错**，只表现成"分支剧情莫名其妙走错"。
#   E. 真鼠标点一下面板按钮（`FLAG-CLEAR`）→ 旗标真的被清掉
#      "界面点不到 = 没交付"。
#
# ⚠ 夹具（一个 `.vks` + 一个场景）写在 `build/bin/assets/` 下，
#   全程 try/finally 删掉 —— 它们**不是**资产，跑完就不该留在仓库里。
#
# 用法：python tools/verify_flags.py [--only ABCDE]
# 结尾一定是 `结果: PASS` / `结果: FAIL: ...`。
# ============================================================

import argparse
import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import find_window, user32                    # noqa: E402
from verify_add_menu import LogTap, click, kill_stale_editors, wait_rect  # noqa: E402
from verify_asset_drag import force_foreground                    # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
EDITOR = os.path.join(BIN, "Editor.exe")
GAME = os.path.join(BIN, "Game.exe")

ASSETS = os.path.join(BIN, "assets")
PROBE_VKS = os.path.join(ASSETS, "scripts", "_flags_probe.vks")
PROBE_SCENE = os.path.join(ASSETS, "scenes", "_flags_probe.json")
SRC_SCENE = os.path.join(ASSETS, "scenes", "levels", "script_demo.json")
PROBE_SCENE_REL = "scenes/_flags_probe.json"
PROBE_VKS_REL = "scripts/_flags_probe.vks"

SIZE = "1400x900"
fails = []


def check(name, ok, detail=""):
    print("%s  %s%s" % ("PASS" if ok else "FAIL", name,
                        ("  (%s)" % detail) if detail else ""))
    if not ok:
        fails.append(name)


# ---------------------------------------------------------------- 环境 / 启动

def base_env(**extra):
    env = {k: v for k, v in os.environ.items() if not k.startswith("MYVK_")}
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_EDITOR_NO_HUD"] = "1"
    env["MYVK_LOG_RECTS"] = "1"
    env.update({k: str(v) for k, v in extra.items()})
    return env


def snap(tap):
    with tap.lock:
        return list(tap.lines)


def close(proc):
    try:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                proc.kill()
    except Exception:                                          # noqa: BLE001
        pass


def run_editor(panel=None, console_cmd=None, wait_tag=None, timeout=75.0,
               wait_for=None, wait_secs=30.0, play=False, scene=None,
               env_extra=None):
    """起编辑器。

    `env_extra`：追加的环境变量（2026-11-02 加，给 verify_fx 用）。
    ⚠ `base_env()` 会**清掉所有 `MYVK_*`**，所以调用方不能靠 os.environ
      传开关 —— 必须走这个参数，否则设了等于没设（而且看起来像开关没生效）。
    """
    extra = {}
    if panel:
        extra["MYVK_EDITOR_PANELS"] = panel
    if console_cmd:
        extra["MYVK_EDITOR_CONSOLE_CMD"] = console_cmd
    if play:
        extra["MYVK_EDITOR_PLAY"] = "1"
    if scene:
        extra["MYVK_EDITOR_SCENE"] = scene
    env = base_env(**extra)
    if env_extra:
        env.update({k: str(v) for k, v in env_extra.items()})
    proc = subprocess.Popen([EDITOR], cwd=BIN, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    hwnd = find_window("Venn Editor", timeout=timeout)
    if not hwnd:
        close(proc)
        return proc, None, tap
    user32.ShowWindow(hwnd, 9)
    time.sleep(0.8)
    tw, th = (int(v) for v in SIZE.split("x"))
    user32.SetWindowPos(hwnd, 0, 0, 0, tw, th, 0x0002 | 0x0004)
    force_foreground(hwnd)
    deadline = time.time() + 45.0
    while time.time() < deadline:
        if wait_tag is None or any(wait_tag in ln for ln in snap(tap)):
            break
        time.sleep(0.2)
    if wait_for:
        deadline = time.time() + wait_secs
        while time.time() < deadline:
            if any(wait_for in ln for ln in snap(tap)):
                break
            time.sleep(0.2)
    time.sleep(0.6)
    return proc, hwnd, tap


def run_game_selftest(name="project", timeout=180.0):
    proc = subprocess.run([GAME, "--selftest", name], cwd=BIN,
                          capture_output=True, text=True, encoding="utf-8",
                          errors="replace", timeout=timeout)
    return (proc.stdout or "") + (proc.stderr or "")


# ---------------------------------------------------------------- 夹具

PROBE_SRC = """\
# _flags_probe.vks —— A4 验收夹具（**不是资产**，跑完由 verify_flags.py 删掉）
#
# 三件事各占一行，行号是判据的一部分：
#   on start  → 一次性的两个旗标
#   on update → 每帧自增的一个旗标（用来证明"是每帧在写"）
let ticks = 0

on start {
    setFlag("met_alice", true)
    setFlag("affection", 42)
}

on update {
    ticks = ticks + 1
    setFlag("ticks", ticks)
}
"""


def write_probe():
    """写夹具：一个脚本 + 一个只有单实体带脚本的场景。

    ⚠ 场景是**从 `script_demo.json` 派生**的，不是手写 JSON：
      实体块里 20 多个字段（mesh / primitive / material / script）只要
      少一个，加载就会**静默**丢掉那个实体（表现成"脚本一行都没跑"）。
      拷一份已知能用的，只改 name 与 script.path，才不会踩这个坑。
    """
    os.makedirs(os.path.dirname(PROBE_VKS), exist_ok=True)
    with open(PROBE_VKS, "w", encoding="utf-8", newline="\n") as f:
        f.write(PROBE_SRC)

    with open(SRC_SCENE, "r", encoding="utf-8") as f:
        demo = json.load(f)
    probe = None
    for e in demo.get("entities", []):
        if isinstance(e, dict) and isinstance(e.get("script"), dict):
            probe = json.loads(json.dumps(e))
            break
    if probe is None:
        raise RuntimeError("script_demo.json 里找不到带 script 的实体")
    probe["name"] = "FlagProbe"
    probe["script"]["path"] = PROBE_VKS_REL
    probe["transform"]["position"] = [0.0, 0.0, 0.0]

    scene = {
        "version": demo.get("version", 1),
        "generator": "verify_flags.py",
        "camera": demo.get("camera", {}),
        "light": demo.get("light", {}),
        "physics": demo.get("physics", {}),
        "entities": [probe],
    }
    with open(PROBE_SCENE, "w", encoding="utf-8", newline="\n") as f:
        json.dump(scene, f, ensure_ascii=False, indent=1)


def remove_probe():
    for p in (PROBE_VKS, PROBE_SCENE):
        try:
            if os.path.isfile(p):
                os.remove(p)
        except Exception:                                      # noqa: BLE001
            pass


# ---------------------------------------------------------------- A

def group_a():
    print("\n[A] 旗标内核（未设过=0 / 覆盖 / writer / 快照恢复）")
    out = run_game_selftest("project")
    m = re.search(r"FLAG-TEST total pass=(\d+) fail=(\d+)", out)
    check("[A] 打出 FLAG-TEST total", m is not None, "")
    if not m:
        return
    p, f = int(m.group(1)), int(m.group(2))
    check("[A] 23 条全过", p >= 23 and f == 0, "pass=%d fail=%d" % (p, f))

    names = re.findall(r"FLAG-TEST (?:PASS|FAIL) (.+)", out)
    need = ["G1 unset-is-zero", "G1b unset-not-present",
            "G1c set-zero-is-present", "G2 overwrite-not-accumulate",
            "G3 writer-last-writer", "G5 rows-sorted",
            "G6 restore-count", "G6c restore-writer",
            "G6d restore-drops-later-keys", "G7 invalid-snapshot-noop",
            "G8c kind-int", "G9 repeated-set"]
    missing = [n for n in need if n not in names]
    check("[A] 关键用例都在", not missing, "缺 %s" % missing)
    total = re.search(r"PROJECT-ALL-TOTAL failures=(\d+)", out)
    check("[A] 工程自检总计 0 失败",
          total is not None and total.group(1) == "0",
          total.group(0) if total else "no total line")


# ---------------------------------------------------------------- B

def group_b():
    print("\n[B] 面板 + 命令（改 → 读 → 删 → 快照 → 恢复）")
    # 一条命令串把整条链跑完：编辑态没有脚本在写，值是确定的。
    cmd = ("flags.set zeta 3; flags.set alpha 1; flags.count; "
           "flags.get alpha; flags.get nobody; "
           "flags.list; flags.snap; flags.set beta 2; flags.count; "
           "flags.restore; flags.list; "
           "flags.erase alpha; flags.count; flags.clear; flags.count")
    # ⚠ `wait_for` 等的**必须是最后一条命令自己的日志行**，不是面板锚点：
    #   锚点在第 3~4 帧就出现了，而命令要等到第 40 帧才执行 —— 等锚点
    #   等于"刚打开面板就截图"，命令一条都还没跑（第一次跑 B 组 8 条全红
    #   就是这个原因）。
    proc, hwnd, tap = run_editor(panel="flags", console_cmd=cmd,
                                 wait_tag="FLAG-RECT",
                                 wait_for="FLAG-COUNT total=0")
    check("[B] 编辑器窗口起来了", bool(hwnd))
    if not hwnd:
        close(proc)
        return
    for tag in ("FLAG-RECT", "FLAG-FILTER", "FLAG-COUNT", "FLAG-SNAP",
                "FLAG-RESTORE", "FLAG-CLEAR", "FLAG-EMPTY"):
        r = wait_rect(tap, tag, timeout=12.0)
        check("[B] 锚点 %s" % tag, r is not None, "%s" % (r,))
    lines = snap(tap)
    close(proc)
    joined = "\n".join(lines)

    # ⚠ 按**顺序**比整串计数（2 → 3 → 1 → 0），而不是"某处出现过某个数"：
    #   后者在任意一步失败时都可能被后面的值碰巧满足，等于没写。
    #   期望序列 = 写两个键 → 快照后加一个 → 恢复后删一个 → 清空。
    counts = [int(m) for m in re.findall(r"FLAG-COUNT total=(\d+)", joined)]
    check("[B] 计数序列 2→3→1→0", counts == [2, 3, 1, 0], counts)
    check("[B] 读到 alpha=1 且 set=1",
          "FLAG-GET name=alpha value=1.0000 set=1" in joined,
          [l for l in lines if "FLAG-GET name=alpha" in l][:2])
    # ⚠ 反向判据：未设过的键 value=0 **但 set=0** —— "设成 0" 与
    #   "从没设过"必须分得开，否则 `if (getFlag("x"))` 的调试全靠猜。
    check("[B] 未设过的键 set=0",
          "FLAG-GET name=nobody value=0.0000 set=0" in joined,
          [l for l in lines if "FLAG-GET name=nobody" in l][:2])
    check("[B] writer 记成 console",
          "name=alpha value=1.0000 kind=bool writer=console" in joined,
          [l for l in lines if "name=alpha" in l][:3])
    check("[B] 快照之后新增键 → 3 个",
          counts[:2] == [2, 3],
          [l for l in lines if "FLAG-COUNT total" in l][:3])
    # 恢复是**整表替换**：beta 是快照之后加的，必须消失
    rows_after = [l for l in lines if "FLAG-ROW" in l]
    check("[B] 恢复后 beta 消失",
          any("name=zeta" in l for l in rows_after) and
          not any("name=beta value=2.0000 kind=int" in l for l in rows_after),
          rows_after[-4:])
    check("[B] erase 后只剩 1 个",
          "FLAG-ERASE name=alpha ok=1" in joined and counts[-2:-1] == [1],
          [l for l in lines if "FLAG-COUNT total" in l][-3:])
    check("[B] clear 后 count=0",
          "FLAG-CLEAR removed=1" in joined and counts[-1:] == [0],
          [l for l in lines if "FLAG-COUNT total" in l][-2:])


# ---------------------------------------------------------------- C

def group_c():
    print("\n[C] Play：脚本 setFlag 真的落到编辑器（A4 的核心）")
    write_probe()
    try:
        # 命令排队：@120 时脚本已经跑了 100+ 帧。
        cmd = ("@120 flags.get met_alice; @120 flags.get ticks; "
               "@120 flags.get never_written; @120 flags.count; "
               "@120 flags.list met_alice")
        proc, hwnd, tap = run_editor(panel="flags", console_cmd=cmd,
                                     wait_tag=None, wait_for="FLAG-LIST-TOTAL",
                                     play=True, scene=PROBE_SCENE_REL,
                                     wait_secs=45.0)
        check("[C] 编辑器窗口起来了", bool(hwnd))
        if not hwnd:
            close(proc)
            return
        lines = snap(tap)
        close(proc)
        joined = "\n".join(lines)

        # ---- C1 反向判据：那个"没接线"的报错不许出现 ----
        #
        # 这条比任何正向断言都值钱：`setFlag` 没接的时候，脚本不会崩，
        # 只会每帧吐一句 "not available in this host"，而剧情分支静默
        # 全走 else。只断言"读到了值"在**别的路径**写进去时也会绿。
        check("[C] 没有 'not available in this host'",
              "not available in this host" not in joined,
              [l for l in lines if "not available" in l][:3])

        check("[C] met_alice 读到 1 且 set=1",
              "FLAG-GET name=met_alice value=1.0000 set=1" in joined,
              [l for l in lines if "FLAG-GET name=met_alice" in l][:2])

        # ---- C3 每帧自增：>= 20 才说明"一直在写" ----
        m = re.search(r"FLAG-GET name=ticks value=(\d+)\.0000 set=1", joined)
        ticks = int(m.group(1)) if m else -1
        check("[C] ticks 每帧自增（>=20）", ticks >= 20, "ticks=%d" % ticks)

        check("[C] 没写过的键仍然是 set=0",
              "FLAG-GET name=never_written value=0.0000 set=0" in joined,
              [l for l in lines if "FLAG-GET name=never_written" in l][:2])

        # ---- C4 谁写的：必须是那个脚本，而不是 console / editor ----
        check("[C] writer = script:%s" % PROBE_VKS_REL,
              ("writer=script:%s" % PROBE_VKS_REL) in joined,
              [l for l in lines if "FLAG-ROW" in l][:3])

        # 三个键：met_alice / affection / ticks
        m = re.search(r"FLAG-COUNT total=(\d+)", joined)
        cnt = int(m.group(1)) if m else -1
        check("[C] 三个旗标都在", cnt == 3, "count=%d" % cnt)
    finally:
        remove_probe()


# ---------------------------------------------------------------- D

def group_d():
    print("\n[D] 新一局清空：上一段 Play 的旗标不许带进下一局")
    # ⚠ 进 Play 只能**点工具栏**：`play.toggle` 是命令面板（CommandRegistry）
    #   的命令，**不在**控制台那张表里 —— 用 `MYVK_EDITOR_CONSOLE_CMD`
    #   发它是"未知命令"，日志一条都不打，而看起来像"清空没生效"。
    #   （第一次跑 D 组就是栽在这里：set 与 get 都在，play 悄无声息地没执行。）
    cmd = ("@40 flags.set keeper 7; @40 flags.get keeper; "
           "@600 flags.get keeper; @600 flags.count")
    proc, hwnd, tap = run_editor(panel="flags", console_cmd=cmd,
                                 wait_tag="FLAG-RECT",
                                 wait_for="FLAG-GET name=keeper",
                                 wait_secs=45.0)
    check("[D] 编辑器窗口起来了", bool(hwnd))
    if not hwnd:
        close(proc)
        return
    try:
        check("[D] 进 Play 前 keeper=7",
              any("FLAG-GET name=keeper value=7.0000 set=1" in l
                  for l in snap(tap)),
              [l for l in snap(tap) if "FLAG-GET name=keeper" in l][:2])

        r = wait_rect(tap, "TB-PLAY-run", timeout=15.0)
        check("[D] 找到工具栏 Play 按钮", r is not None, "%s" % (r,))
        if r:
            force_foreground(hwnd)
            time.sleep(0.25)
            click(hwnd, (r[0] + r[2]) / 2.0, (r[1] + r[3]) / 2.0)
            time.sleep(0.8)
            click(hwnd, (r[0] + r[2]) / 2.0, (r[1] + r[3]) / 2.0)

        # 等"新一局清空"那一行（只有真的进了 Play 才会打）
        deadline = time.time() + 40.0
        while time.time() < deadline:
            if any("EDITOR-FLAG new-session" in l for l in snap(tap)):
                break
            time.sleep(0.2)
        # 再等 @600 那一批命令
        deadline = time.time() + 45.0
        while time.time() < deadline:
            if any("FLAG-COUNT total=" in l for l in snap(tap)):
                break
            time.sleep(0.2)
        time.sleep(0.5)

        lines = snap(tap)
        joined = "\n".join(lines)
        check("[D] 进 Play 时打了 new-session cleared=1",
              "EDITOR-FLAG new-session cleared=1" in joined,
              [l for l in lines if "new-session" in l][:2])
        # ⚠ 断言必须是 set=0（"从没设过"），不是 value=0 就够了：
        #   只比 value 的话，一个"把值清零但键留着"的实现照样绿。
        check("[D] 进 Play 后 keeper 消失（set=0）",
              "FLAG-GET name=keeper value=0.0000 set=0" in joined,
              [l for l in lines if "FLAG-GET name=keeper" in l][-2:])
        check("[D] 新一局旗标表是空的",
              "FLAG-COUNT total=0" in joined,
              [l for l in lines if "FLAG-COUNT total" in l][-2:])
    finally:
        close(proc)
        remove_probe()


# ---------------------------------------------------------------- E

def group_e():
    print("\n[E] 真鼠标点 FLAG-CLEAR → 旗标真的被清掉")
    proc, hwnd, tap = run_editor(panel="flags",
                                 console_cmd="flags.set keeper 7; flags.count",
                                 wait_tag="FLAG-CLEAR",
                                 wait_for="FLAG-COUNT total=1")
    check("[E] 编辑器窗口起来了", bool(hwnd))
    if not hwnd:
        close(proc)
        return
    r = wait_rect(tap, "FLAG-CLEAR", timeout=12.0)
    check("[E] 找到 FLAG-CLEAR 矩形", r is not None, "%s" % (r,))
    if r:
        # 合成鼠标"起手"要预热：上一动作常把光标甩出客户区（老坑）
        cx = (r[0] + r[2]) / 2.0
        cy = (r[1] + r[3]) / 2.0
        force_foreground(hwnd)
        time.sleep(0.25)
        click(hwnd, cx, cy)
        time.sleep(0.9)
        click(hwnd, cx, cy)
        time.sleep(1.0)
    lines = snap(tap)
    close(proc)
    joined = "\n".join(lines)
    check("[E] 点击真的走了面板的 wipe()",
          "EDITOR-FLAG clear removed=1" in joined,
          [l for l in lines if "EDITOR-FLAG clear" in l][:3])


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="ABCDE")
    args = ap.parse_args()
    only = args.only.upper()

    print("=" * 74)
    print("剧情旗标（A4）：内核 + 面板 + 命令 + Play 里脚本真的写得进去")
    print("=" * 74)

    kill_stale_editors()
    try:
        if "A" in only:
            group_a()
        if "B" in only:
            group_b()
        if "C" in only:
            group_c()
        if "D" in only:
            group_d()
        if "E" in only:
            group_e()
    finally:
        remove_probe()

    print("\n" + "=" * 74)
    if fails:
        print("结果: FAIL: %d 项 —— %s" % (len(fails), fails[:6]))
        return 1
    print("结果: PASS（23 条内核 + 面板/命令链 + Play 里脚本写旗标 + "
          "新一局清空 + 真鼠标清空）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
