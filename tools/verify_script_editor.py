"""端到端验证：脚本的「编辑器面」（S5，见 docs/script_roadmap.md §7）。

S0~S4 之后脚本已经能跑、能等、能热重载 —— 但**编辑器里看不见**。这一页是
五件套里的 UI 面：没有它，"wait 挂住了"、"变量没涨"、"熔断不出声"全都只能
翻日志。这个脚本按 §7 的 F1~F6 逐条断。

六组：

  F1 Instances 页签存在，3 个实例的锚点全在
  F2 Idle → Running（**文本 + 像素**双证）
  F3 改一个数字热重载：`VKS-RELOAD-KEEP`，变量数不清零
  F4 点面板上的 Reset：`vars_after=0`
  F5 改成死循环 → `state=Failed` + 红字；点 Reset 后恢复
  F6 控制台 `script.vars Boss` 打印 3 行以上键值

⚠ 三件事让这个用例能跑起来，缺一条都不行（都是实测踩出来的）：
   · `MYVK_EDITOR_PANELS=scripts` —— Scripts 面板默认关闭，Alt+6 靠键盘
     状态，PostMessage 下时灵时不灵；
   · `MYVK_EDITOR_SCRIPT_TAB=instances` —— ImGui 页签位置没法从锚点推，
     只能让它在第一次绘制时就选中；
   · `MYVK_EDITOR_CONSOLE_CMD_FRAME` —— F6 要在 Play **之后**读变量，
     默认的第 40 帧还在编辑态（脚本不跑，变量表是空的）。
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from capture_window import capture_client, find_window, user32      # noqa: E402
from verify_add_menu import LogTap, click, kill_stale_editors        # noqa: E402
from verify_asset_drag import force_foreground, client_to_screen     # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify")

TMP_DIR = tempfile.mkdtemp(prefix="venn_script_ed_")
TMP_SCENE = os.path.join(TMP_DIR, "script_ui.json")

WM_KEYDOWN, WM_KEYUP = 0x0100, 0x0101
VK_SPACE, SC_SPACE = 0x20, 0x39

g_pass = 0
g_fail = 0


def check(name, ok, detail=""):
    global g_pass, g_fail
    if ok:
        g_pass += 1
        print("[ OK ] %s%s" % (name, ("  " + detail) if detail else ""))
    else:
        g_fail += 1
        print("[FAIL] %s%s" % (name, ("  " + detail) if detail else ""))


def sig_ratio(a, b, box=None, thresh=8):
    from PIL import ImageChops
    if a is None or b is None:
        return 0.0
    ca, cb = a.convert("RGB"), b.convert("RGB")
    if box:
        ca, cb = ca.crop(box), cb.crop(box)
    hist = ImageChops.difference(ca, cb).convert("L").histogram()
    total = max(ca.size[0] * ca.size[1], 1)
    return sum(hist[thresh + 1:]) / float(total)


# ---------------------------------------------------------------- 夹具
#
# 三个实体各自挂一份脚本。`Boss` 的变量最多（F6 要 3 行以上），`Watcher`
# 一进去就 wait（验 Waiting），`Runner` 只自增（验 Running）。

BOSS_V1 = """let hp = 100
let stage = 1
let ticks = 0
on update {
    ticks = ticks + 1
}
"""

BOSS_V2 = """let hp = 100
let stage = 1
let ticks = 0
on update {
    ticks = ticks + 2
}
"""

BOSS_FUSE = """let hp = 100
let stage = 1
let ticks = 0
on update {
    ticks = ticks + 1
    while (1) {
        ticks = ticks + 1
    }
}
"""

WATCHER = """let seen = 0
wait(30.0)
seen = 1
"""

RUNNER = """let n = 0
on update {
    n = n + 1
}
"""

SCENE = """{
  "version": 1,
  "generator": "verify_script_editor fixture",
  "camera": {"target": [0.0, 0.0, 1.5], "yaw": 35.0, "pitch": -18.0,
             "distance": 12.0},
  "physics": {"enabled": false, "groundPlane": false,
              "fixedTimeStep": 0.0166666675, "maxSubSteps": 4},
  "entities": [
    {
      "name": "Ground",
      "transform": {"position": [0.0, 0.0, 0.0],
                    "rotation": [0.0, 0.0, 0.0],
                    "scale": [1.0, 1.0, 1.0]},
      "parent": -1, "visible": true, "castShadow": true, "locked": false,
      "mesh": {"kind": "builtin", "shape": "plane", "size": 20.0,
               "name": "script_ui_ground"}
    },
    {
      "name": "Boss",
      "transform": {"position": [-2.5, 0.0, 1.0],
                    "rotation": [0.0, 0.0, 0.0],
                    "scale": [1.0, 1.0, 1.0]},
      "parent": -1, "visible": true, "castShadow": true, "locked": false,
      "mesh": {"kind": "builtin", "shape": "cube", "size": 1.0,
               "name": "script_ui_boss"},
      "script": {"path": "%(boss)s", "enabled": true, "timeScale": 1.0}
    },
    {
      "name": "Watcher",
      "transform": {"position": [0.0, 0.0, 1.0],
                    "rotation": [0.0, 0.0, 0.0],
                    "scale": [1.0, 1.0, 1.0]},
      "parent": -1, "visible": true, "castShadow": true, "locked": false,
      "mesh": {"kind": "builtin", "shape": "cube", "size": 1.0,
               "name": "script_ui_watcher"},
      "script": {"path": "%(watcher)s", "enabled": true, "timeScale": 1.0}
    },
    {
      "name": "Runner",
      "transform": {"position": [2.5, 0.0, 1.0],
                    "rotation": [0.0, 0.0, 0.0],
                    "scale": [1.0, 1.0, 1.0]},
      "parent": -1, "visible": true, "castShadow": true, "locked": false,
      "mesh": {"kind": "builtin", "shape": "cube", "size": 1.0,
               "name": "script_ui_runner"},
      "script": {"path": "%(runner)s", "enabled": true, "timeScale": 1.0}
    }
  ]
}
"""


def write(path, text):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return path


def make_fixture():
    boss = write(os.path.join(TMP_DIR, "boss.vks"), BOSS_V1)
    watcher = write(os.path.join(TMP_DIR, "watcher.vks"), WATCHER)
    runner = write(os.path.join(TMP_DIR, "runner.vks"), RUNNER)
    # JSON 里的反斜杠要转义（Windows 路径）
    esc = lambda p: p.replace("\\", "\\\\")
    write(TMP_SCENE, SCENE % {"boss": esc(boss), "watcher": esc(watcher),
                              "runner": esc(runner)})
    return boss


def launch(exe, extra=None):
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_EDITOR_NO_HUD"] = "1"
    env["MYVK_EDITOR_SCENE"] = TMP_SCENE
    env["MYVK_EDITOR_PANELS"] = "scripts"
    env["MYVK_EDITOR_SCRIPT_TAB"] = "instances"
    if extra:
        env.update(extra)
    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    return proc, tap


def stop(proc):
    try:
        proc.terminate()
        proc.wait(timeout=10)
    except Exception:
        try:
            proc.kill()
        except Exception:
            pass


def wait_rect(tap, tag, timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        r = tap.last_rect(tag)
        if r:
            return r
        time.sleep(0.1)
    return None


def wait_line(tap, needle, timeout=12.0):
    end = time.time() + timeout
    while time.time() < end:
        if tap.has_line(needle):
            return True
        time.sleep(0.1)
    return False


def count_line(tap, needle):
    with tap.lock:
        return sum(1 for l in tap.lines if needle in l)


def wait_count(tap, needle, want, timeout=10.0):
    """等某条日志**累计**到 want 条。

    ⚠ 为什么不能直接 `wait_line`：F4 已经点过一次 Reset，日志里那条
      `SCR-RESET ok=1 source=panel` 早就在了 —— `has_line` 立刻为真，
      F5c 就是"永远通过"的空断言。要的是"**又多了一条**"。
    """
    end = time.time() + timeout
    while time.time() < end:
        if count_line(tap, needle) >= want:
            return True
        time.sleep(0.1)
    return False


def settle(hwnd, tap, w=1600, h=900):
    user32.ShowWindow(hwnd, 9)
    time.sleep(2.0)
    user32.SetWindowPos(hwnd, 0, 0, 0, w, h, 0x0002 | 0x0004)
    time.sleep(2.5)
    force_foreground(hwnd)
    time.sleep(0.8)
    return bool(wait_rect(tap, "VP-RECT", 12.0))


def press_space(hwnd):
    user32.PostMessageW(hwnd, WM_KEYDOWN, VK_SPACE, (SC_SPACE << 16) | 1)
    time.sleep(0.08)
    user32.PostMessageW(hwnd, WM_KEYUP, VK_SPACE,
                        (SC_SPACE << 16) | 1 | (1 << 30) | (1 << 31))


def grab(hwnd):
    return capture_client(hwnd)[0]


def states_in(tap):
    """从日志里取 (entity, state) 序列，按出现顺序。"""
    rx = re.compile(r"SCR-STATE entity=(\d+) name=(\S+) state=(\w+) vars=(\d+)")
    out = []
    for line in list(tap.lines):
        m = rx.search(line)
        if m:
            out.append((m.group(2), m.group(3), int(m.group(4))))
    return out


def last_state(tap, name):
    for n, s, _ in reversed(states_in(tap)):
        if n == name:
            return s
    return None


# ---------------------------------------------------------------- 用例
def run_main(exe):
    """F1~F5：一个会话里跑完（Play / 热重载 / Reset / 熔断都在 Play 态下才有意义）。"""
    kill_stale_editors()
    boss = make_fixture()
    proc, tap = launch(exe)
    try:
        hwnd = find_window("Venn Editor", timeout=30.0)
        if not hwnd:
            check("启动编辑器", False, "找不到窗口")
            return
        if not settle(hwnd, tap):
            check("等待视口就绪", False, "VP-RECT 没出现")
            return
        check("启动编辑器", True, "1600x900")

        # ---- F1 Instances 页签：3 个实例的锚点 ----
        r0 = wait_rect(tap, "SCR-INST-0", 12.0)
        r1 = tap.last_rect("SCR-INST-1")
        r2 = tap.last_rect("SCR-INST-2")
        check("F1 Instances 页签列出 3 个实例", all([r0, r1, r2]),
              "rects=%s" % [bool(r0), bool(r1), bool(r2)])

        rreset = tap.last_rect("SCR-INST-RESET")
        check("F1b Reset 按钮锚点存在（无下标的那个）", bool(rreset),
              "SCR-INST-RESET=%s" % (rreset,))
        rr0 = tap.last_rect("SCR-INST-RESET-0")
        check("F1c 带下标的 Reset 锚点存在", bool(rr0), "%s" % (rr0,))
        # 三行必须在同一个 x 上（都在列表里，而不是散在别处）
        same_col = all(r and abs(r[0] - r0[0]) < 4.0 for r in (r0, r1, r2))
        check("F1d 三行的左边缘对齐（是同一张列表）", same_col,
              "x=%s" % [round(r[0], 1) if r else None for r in (r0, r1, r2)])

        # ---- F2 Idle → Running（文本 + 像素）----
        st_idle = last_state(tap, "Boss")
        check("F2 进 Play 前状态是 Idle", st_idle == "Idle",
              "state=%s" % st_idle)

        # 像素：Idle 是灰、Running 是绿。截的是**文字那一行**（锚点矩形）。
        box = None
        if r0:
            box = (int(r0[0]) - 2, int(r0[1]) - 2, int(r0[2]) + 2,
                   int(r0[3]) + 2)
        shot_idle = grab(hwnd)

        press_space(hwnd)
        got_run = wait_line(tap, "state=Running", 12.0)
        check("F2b 按 Space 进 Play 后状态变 Running（文本）", got_run,
              "SCR-STATE 行数=%d" % len(states_in(tap)))
        time.sleep(1.2)
        shot_run = grab(hwnd)
        diff = sig_ratio(shot_idle, shot_run, box)
        # ⚠ 判据用"显著变化像素占比"而不是整图平均（角色占屏 4% 的教训）。
        #   这里是文字行，灰→绿 + 时间数字跳动，> 5% 是硬要求。
        check("F2c Idle 与 Running 的面板像素显著不同", diff > 0.05,
              "Δ=%.2f%% 区域=%s" % (diff * 100.0, box))

        # Watcher 应该在 Wait
        wait_ok = wait_line(tap, "state=Waiting", 10.0)
        check("F2d wait 中的实例显示 Waiting", wait_ok, "")

        # ---- F3 改一个数字 → 热重载保住变量 ----
        before_vars = None
        for n, s, v in reversed(states_in(tap)):
            if n == "Boss":
                before_vars = v
                break
        write(boss, BOSS_V2)      # ticks + 1  →  ticks + 2（只改字面量）
        kept = wait_line(tap, "VKS-RELOAD-KEEP", 15.0)
        check("F3 改数字后热重载成功对账（VKS-RELOAD-KEEP）", kept, "")
        m = None
        for line in tap.lines:
            if "VKS-RELOAD-KEEP" in line:
                m = re.search(r"vars=(\d+)", line)
        vars_kept = int(m.group(1)) if m else -1
        check("F3b 热重载后变量没被清（面板数字证明）",
              vars_kept > 0 and (before_vars is None or vars_kept == before_vars),
              "before=%s after=%s" % (before_vars, vars_kept))
        check("F3c 没走整体重启分支（没有 RELOAD-RESTART）",
              not any("VKS-RELOAD-RESTART" in l for l in tap.lines), "")

        # ---- F4 点面板 Reset → 变量清零 ----
        r = tap.last_rect("SCR-INST-RESET-0")
        if not r:
            check("F4 找得到 Reset 按钮", False, "")
        else:
            cx = (r[0] + r[2]) * 0.5
            cy = (r[1] + r[3]) * 0.5
            click(hwnd, cx, cy)
            ok = wait_line(tap, "SCR-RESET ok=1 source=panel", 8.0)
            after = None
            for line in tap.lines:
                if "SCR-RESET ok=1 source=panel" in line:
                    mm = re.search(r"vars_after=(\d+)", line)
                    if mm:
                        after = int(mm.group(1))
            check("F4 点 Reset 后面板报了重置", ok, "")
            check("F4b Reset 后变量数归零（vars_after=0）", after == 0,
                  "vars_after=%s" % after)

        # ---- F5 熔断 → Failed + 红字；Reset 后恢复 ----
        # 把 Boss 的脚本换成死循环：**编译能过**，运行期撞步数预算 → Runtime 熔断
        write(boss, BOSS_FUSE)
        fused = wait_line(tap, "state=Failed", 20.0)
        fatal = any("step budget" in l for l in tap.lines)
        check("F5 死循环 → 实例熔断成 Failed", fused and fatal,
              "Failed=%d step-budget=%d" % (int(fused), int(fatal)))
        shot_fail = grab(hwnd)
        rf = tap.last_rect("SCR-INST-0")
        box2 = None
        if rf:
            box2 = (int(rf[0]) - 2, int(rf[1]) - 2, int(rf[2]) + 2,
                    int(rf[3]) + 2)
        # 红字：取样区里"红显著多于绿"的像素占比
        red_share = 0.0
        if shot_fail is not None and box2:
            px = shot_fail.convert("RGB").crop(box2)
            data = list(px.getdata())
            if data:
                red = sum(1 for r_, g_, b_ in data
                          if r_ > 120 and r_ > g_ + 40 and r_ > b_ + 40)
                red_share = red / float(len(data))
        check("F5b Failed 行是红字（像素证据）", red_share > 0.01,
              "red=%.2f%%" % (red_share * 100.0))

        # 点同一个 Reset → 熔断必须解除（这是 S-D8 的第四个出口，
        # 前三个是热重载 / 换脚本 / 场景重载）
        r = tap.last_rect("SCR-INST-RESET-0")
        if r:
            # ⚠ 计数要**在点之前**取：`has_line` 对第 4 组那条 Reset 日志
            #   也是真的，用它做断言等于什么都没验（会永远绿）。
            before_fail = sum(1 for n, s, _ in states_in(tap)
                              if n == "Boss" and s == "Failed")
            before_reset = count_line(tap, "SCR-RESET ok=1 source=panel")
            click(hwnd, (r[0] + r[2]) * 0.5, (r[1] + r[3]) * 0.5)
            recovered = wait_count(tap, "SCR-RESET ok=1 source=panel",
                                   before_reset + 1, 8.0)
            # 脚本还是死循环 → 会立刻再熔断一次；要验的是"解除了又熔断"，
            # 所以看 Failed 行的**出现次数**增加。
            time.sleep(2.5)
            after_fail = sum(1 for n, s, _ in states_in(tap)
                             if n == "Boss" and s == "Failed")
            check("F5c 点 Reset 后熔断被解除并重新执行", recovered,
                  "reset_lines %d→%d" % (before_reset, before_reset + 1))
            check("F5d 解除后重新熔断（Failed 出现次数增加，说明真的又跑了）",
                  after_fail > before_fail,
                  "before=%d after=%d" % (before_fail, after_fail))
    finally:
        stop(proc)


def run_console_vars(exe):
    """F6：控制台 `script.vars Boss` 打印 >= 3 行键值。

    单独一次启动：命令必须在 **Play 之后**执行（编辑态脚本不跑，变量表空）。
    """
    kill_stale_editors()
    make_fixture()
    # 1000 帧 ≈ 16 秒，足够 Python 按 Space 进 Play 并让 ticks 涨起来
    proc, tap = launch(exe, {"MYVK_EDITOR_CONSOLE_CMD": "script.vars Boss",
                             "MYVK_EDITOR_CONSOLE_CMD_FRAME": "1000"})
    try:
        hwnd = find_window("Venn Editor", timeout=30.0)
        if not hwnd or not settle(hwnd, tap):
            check("F6 控制台用例启动", False, "")
            return
        press_space(hwnd)
        ok = wait_line(tap, "SCR-VARS ok=1", 40.0)
        name = None
        count = 0
        kv_lines = 0
        for line in tap.lines:
            if "SCR-VARS ok=1" in line:
                m = re.search(r"name=(\S+) state=(\w+) count=(\d+)", line)
                if m:
                    name = m.group(1)
                    count = int(m.group(3))
            elif "SCR-VARS " in line and "=" in line and "ok=" not in line:
                kv_lines += 1
        check("F6 控制台 script.vars Boss 有输出", ok, "")
        check("F6b 报的是 Boss 这个实例", name == "Boss", "name=%s" % name)
        check("F6c 打印了 3 行以上键值", kv_lines >= 3,
              "kv_lines=%d count=%d" % (kv_lines, count))
    finally:
        stop(proc)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--only", default="", help="main / vars")
    args = ap.parse_args()

    if not os.path.isfile(args.exe):
        print("找不到 %s" % args.exe)
        print("结果: FAIL: exe missing")
        return 1

    os.makedirs(OUT_DIR, exist_ok=True)
    try:
        if args.only in ("", "main"):
            run_main(args.exe)
        if args.only in ("", "vars"):
            run_console_vars(args.exe)
    finally:
        kill_stale_editors()

    print("")
    print("pass=%d fail=%d" % (g_pass, g_fail))
    if g_fail:
        print("结果: FAIL: %d 项" % g_fail)
        return 1
    print("结果: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
