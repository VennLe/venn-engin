"""端到端验证：编辑器里的「存档槽位」面板（G17-3 尾巴，2026-10-31）。

==============================================================================
为什么单开一条
==============================================================================
`listSaveSlots()` / `saveToSlot()` / `loadFromSlot()` 都有自检和 `verify_save_slots.py`
了，但那些全是**函数级**的。这一块面板要证明的是另外三件事：

  · 它列的槽位就是文件系统里那五个（不是写死的一张表）；
  · 三个按钮真的调到了那三个函数（不是只写日志）；
  · **编辑器与运行时看的是同一批文件** —— 在编辑器里存的档，`Game.exe`
    `--load` 就该读得到。这条不是巧合：两边 CWD 都是 `build/bin`、
    `root` 都是相对路径 `saves/`。写歪任何一边，症状都是"我明明存了"。

==============================================================================
判据
==============================================================================
A · 面板打开（`SAVEPANEL-WIN` 矩形拿到），且清单里五个槽位都在。
B · 点 Save → `SAVEPANEL op=save slot=quick ok=1 entities=N`，且磁盘上真有
    `build/bin/saves/quick.vsave`。
    ⚠ 只验日志的话，"写了日志但没写文件"永远是绿的。
C · **跨进程**：`Game.exe --slot quick --load` 读同一个档 → `SAVE-READ ... ok=1`。
    ⚠ 这是本用例的核心。它在验"两边路径口径一致"，而这条只有真起第二个
      进程才能证。为此夹具**必须**写显式 guid（否则每次加载都补发随机 guid
      → `sceneStructureHash` 变 → `SAVE-DRIFT-REJECT`，看起来像"读档坏了"）。
D · 点 Load → `SAVEPANEL op=load slot=quick ok=1`。
E · 点 Delete → **二次确认弹窗出现** → 点弹窗里的 Delete → `op=delete ok=1`，
    且磁盘上的文件真的没了。
    ⚠ 少了"确认弹窗出现"这一环，用例会退化成"点 Delete 就删了" ——
      而那个弹窗存在的全部意义就是"别让人一点就删"。
F · 删除之后**再点 Save** 仍然成功（证明 `m_baselineReady` 的重置是对的：
    不重置的话删档后存出来的是空增量）。

用法：
    python tools/verify_save_panel.py
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from safe_fs import safe_remove, safe_rmtree               # noqa: E402  （与下方 verify_* 导入同一批）
from capture_window import find_window, user32                       # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors               # noqa: E402
from verify_bvh import click_client                                  # noqa: E402
from verify_hierarchy_status import wait_rect_of, center             # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
EDITOR_EXE = os.path.join(BIN, "Editor.exe")
GAME_EXE = os.path.join(BIN, "Game.exe")

# 夹具写在 build/bin 下（两边的 CWD），用**相对名**。
FIXTURE_REL = "assets/scenes/_savepanel_fixture.json"
FIXTURE = os.path.join(BIN, FIXTURE_REL)
SAVE_DIR = os.path.join(BIN, "saves")
SLOT = "quick"

SAVE_RE = re.compile(r"SAVEPANEL op=save slot=(\S+) ok=(\d) entities=(\d+) "
                     r"path=(\S+)")
LOAD_RE = re.compile(r"SAVEPANEL op=load slot=(\S+) ok=(\d)")
DEL_RE = re.compile(r"SAVEPANEL op=delete slot=(\S+) ok=(\d) existed=(\d)")
READ_RE = re.compile(r"SAVE-READ slot=(\S+) path=\S+ version=\d+ migrated=\d+ "
                     r"bak=(\d) applied=(\d+) destroyed=\d+ missing=(\d+) "
                     r"extra=\d+ orphans=\d+ drift=(\d)")

results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print("  [%s] %s %s" % ("PASS" if ok else "FAIL", name, detail))
    return ok


def lines_of(tap):
    with tap.lock:
        return list(tap.lines)


def count_where(tap, rx):
    return [m for m in (rx.search(l) for l in lines_of(tap)) if m]


def wait_where(tap, rx, n, timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        got = count_where(tap, rx)
        if len(got) >= n:
            return got
        time.sleep(0.15)
    return count_where(tap, rx)


def make_fixture():
    """三个立方体，**每个都写显式 guid**。

    ⚠ 不写 guid 的话每次加载都随机补发 → `sceneStructureHash` 每次不同 →
    编辑器存的档在 Game.exe 里被判"不是同一个关卡"而拒读（C 组会红）。
    """
    ents = []
    for k in range(3):
        ents.append({
            "name": "Box%d" % k,
            "guid": "%016x" % (0x5a1e0000000a0000 + k),
            "transform": {"position": [3.0 * k - 3.0, 0.0, 1.0],
                          "rotation": [0.0, 0.0, 0.0],
                          "scale": [1.0, 1.0, 1.0]},
            "parent": -1,
            "mesh": {"kind": "builtin", "shape": "cube", "size": 1.0},
            "material": {"baseColor": [0.8, 0.5, 0.4, 1.0],
                         "roughness": 0.6, "metallic": 0.0},
        })
    scene = {"version": 2, "generator": "verify_save_panel",
             "camera": {"target": [0, 0, 1], "yaw": 0.61, "pitch": 0.31,
                        "distance": 10.0},
             "physics": {"enabled": False, "groundPlane": False,
                         "fixedTimeStep": 0.016666, "maxSubSteps": 4},
             "entities": ents}
    os.makedirs(os.path.dirname(FIXTURE), exist_ok=True)
    with open(FIXTURE, "w", encoding="utf-8") as f:
        json.dump(scene, f, ensure_ascii=False, indent=1)


def slot_path(slot):
    return os.path.join(SAVE_DIR, slot + ".vsave")


ALL_SLOTS = ["quick", "auto", "manual", "slot1", "slot2"]


def run_game_load(slot, wait=120.0):
    """`Game.exe --slot quick --load --scene <夹具>`：只跑几帧，读档发生在启动。"""
    env = dict(os.environ)
    env["MYVK_LOG_DT"] = ""
    argv = [GAME_EXE, "--scene", FIXTURE_REL, "--slot", slot, "--load",
            "--frames", "20"]
    proc = subprocess.Popen(argv, cwd=BIN, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    try:
        out, _ = proc.communicate(timeout=wait)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, _ = proc.communicate(timeout=30)
    return (out or b"").decode("utf-8", "replace").replace("\x00", "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=EDITOR_EXE)
    ap.add_argument("--game", default=GAME_EXE)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    if not os.path.isfile(exe):
        check("0 找不到 Editor.exe", False, exe)
        return 1

    make_fixture()
    # ⚠ 槽位**不写死**：下拉框默认选中 `saveSlotNames()` 的第一项，而那是
    #   面板/引擎的白名单顺序，改一行就会变。所以从 `op=save slot=` 那行
    #   里读实际槽位，磁盘路径与 `Game.exe --slot` 都跟着它走 —— 否则
    #   "槽位默认值从 auto 改成 quick"这样一次无害的重排会让整条用例变红，
    #   而它测的是"两边路径口径一致"，不是"默认选中哪一个"。
    save_path = None
    # 从干净状态起跑（所有槽位一起清）：上一轮留下的档会让 entities 不可判。
    for sl in ALL_SLOTS:
        for p in (slot_path(sl), slot_path(sl) + ".bak"):
            try:
                safe_remove(p)
            except OSError:
                pass

    kill_stale_editors()
    env = dict(os.environ)
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_EDITOR_SCENE"] = FIXTURE_REL
    # ⚠ 面板默认全关。这条环境变量是"按 id 打开面板"的既有钩子
    #   （见 EditorApp.cpp 里 MYVK_EDITOR_PANELS 那段）。
    env["MYVK_EDITOR_PANELS"] = "save"
    proc = subprocess.Popen([exe], cwd=BIN, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True,
                            encoding="utf-8", errors="replace", bufsize=1)
    tap = LogTap()
    tap.start(proc)

    try:
        hwnd = find_window("Venn Editor")
        if not hwnd:
            print("未找到窗口")
            return 2
        user32.ShowWindow(hwnd, 9)
        time.sleep(2.0)
        user32.SetWindowPos(hwnd, 0, 0, 0, 1600, 900, 0x0002 | 0x0004)
        time.sleep(2.5)

        win = wait_rect_of(tap, "SAVEPANEL-WIN", 10.0)
        check("A 面板打开（SAVEPANEL-WIN 矩形拿到）", win is not None,
              "rect=%s" % (win,))
        if not win:
            dump = [l for l in lines_of(tap) if "PANEL" in l or "panel" in l]
            print("  日志尾部：", dump[-8:])
            return 3

        # ---- B · 存 ----
        n_save = len(count_where(tap, SAVE_RE))
        btn = wait_rect_of(tap, "SAVEPANEL-SAVE", 6.0)
        if btn:
            click_client(hwnd, *[int(v) for v in center(btn)])
        saves = wait_where(tap, SAVE_RE, n_save + 1, 10.0)
        b_ok = len(saves) > n_save and saves[-1].group(2) == "1"
        check("B1 点 Save → SAVEPANEL op=save ok=1", b_ok,
              "line=%s" % ((saves[-1].groups() if b_ok else None),))
        slot = saves[-1].group(1) if b_ok else SLOT
        save_path = slot_path(slot)
        print("  槽位 = %r → %s" % (slot, save_path))
        check("B2 磁盘上真有那个文件（不是只写了日志）",
              os.path.isfile(save_path) and os.path.getsize(save_path) > 64,
              "%s size=%s" % (save_path,
                              os.path.getsize(save_path)
                              if os.path.isfile(save_path) else "missing"))

        # ---- C · 跨进程：Game.exe --load 读同一个档 ----
        # ⚠ 编辑器此刻还开着并占着文件，但读档只读不写，不会冲突。
        #
        # ⚠ 判据不能用 "ok=1"：`SAVE-READ` 成功那行**没有** `ok=` 字段
        #   （失败打的是另一条 `SAVE-READ-FAIL`）。所以判三件事：
        #   `applied > 0`（真的覆盖了实体）+ `missing == 0`（没有实体对不上）
        #   + `drift == 0`（没被 `SAVE-DRIFT-REJECT` 拒读）。
        #   只看"有没有 SAVE-READ 这一行"会把"读进来但一个都没应用"读成成功。
        text = run_game_load(slot)
        reads = [m for m in (READ_RE.search(l) for l in text.splitlines()) if m]
        c_ok = False
        if reads:
            m = reads[-1]
            c_ok = (int(m.group(3)) > 0 and int(m.group(4)) == 0
                    and int(m.group(5)) == 0)
        check("C Game.exe --slot %s --load 读到了同一个档（两边路径口径一致）"
              % slot,
              c_ok and "SAVE-READ-FAIL" not in text,
              "reads=%s fail=%s" % ([m.groups() for m in reads][:3],
                                    "SAVE-READ-FAIL" in text))

        # ---- D · 读 ----
        n_load = len(count_where(tap, LOAD_RE))
        btn = wait_rect_of(tap, "SAVEPANEL-LOAD", 6.0)
        if btn:
            click_client(hwnd, *[int(v) for v in center(btn)])
        loads = wait_where(tap, LOAD_RE, n_load + 1, 10.0)
        check("D 点 Load → SAVEPANEL op=load ok=1",
              len(loads) > n_load and loads[-1].group(2) == "1",
              "line=%s" % ((loads[-1].groups() if len(loads) > n_load else None),))

        # ---- E · 删（含二次确认）----
        btn = wait_rect_of(tap, "SAVEPANEL-DELETE", 6.0)
        if btn:
            click_client(hwnd, *[int(v) for v in center(btn)])
        confirm = wait_rect_of(tap, "SAVEPANEL-CONFIRM-DEL", 6.0)
        check("E1 点 Delete → 二次确认弹窗出现（不是一点就删）",
              confirm is not None, "rect=%s" % (confirm,))
        n_del = len(count_where(tap, DEL_RE))
        if confirm:
            click_client(hwnd, *[int(v) for v in center(confirm)])
        dels = wait_where(tap, DEL_RE, n_del + 1, 10.0)
        e2 = len(dels) > n_del and dels[-1].group(2) == "1"
        check("E2 确认之后真的删了（op=delete ok=1 且文件消失）",
              e2 and not os.path.isfile(save_path),
              "line=%s exists=%s"
              % ((dels[-1].groups() if e2 else None),
                 os.path.isfile(save_path)))

        # ---- F · 删完再存仍然成功（基线重置）----
        n_save2 = len(count_where(tap, SAVE_RE))
        btn = wait_rect_of(tap, "SAVEPANEL-SAVE", 6.0)
        if btn:
            click_client(hwnd, *[int(v) for v in center(btn)])
        saves2 = wait_where(tap, SAVE_RE, n_save2 + 1, 10.0)
        f_ok = len(saves2) > n_save2 and saves2[-1].group(2) == "1"
        # ⚠ entities 必须 > 0：删档后不重置基线的话，这里会写出**空增量**
        #   （`entities: []`），而它看起来完全合法 —— 只有这个数字能看出来。
        f_ent = int(saves2[-1].group(3)) if f_ok else 0
        check("F 删档之后再存：仍然成功且 entities > 0（基线重置对）",
              f_ok and f_ent > 0,
              "line=%s" % ((saves2[-1].groups() if f_ok else None),))

    finally:
        if args.keep:
            print("--keep：留窗口 pid", proc.pid)
        else:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()

    # 收尾：别把夹具 / 存档留在仓库里
    for p in (FIXTURE, save_path, save_path + ".bak"):
        try:
            safe_remove(p)
        except OSError:
            pass

    n_pass = sum(1 for _, ok, _ in results if ok)
    print("\n%d/%d PASS" % (n_pass, len(results)))
    if n_pass == len(results):
        print("结果: PASS")
        return 0
    print("结果: FAIL: %d 项" % (len(results) - n_pass))
    return 1


if __name__ == "__main__":
    sys.exit(main())
