"""端到端验证：编辑器出厂默认值 —— 吸附全开 / 地面栅格 1m 且放大到 UE 规模。

用户原文（2026-10-17）：
    ④ "视口中的物体的移动旋转缩放大小的对应吸附功能默认都要是开启状态的。"
    ⑤ "视口中的地面栅格是标准的 1m*1m 的比例的单元格吗，另外请把地面的面积
       扩大一些，按照 ue5 中的标准规模去弄大些。"

怎么判：这些是"用户默认值"，界面上只有几个勾选框/滑条能看出来，脚本没法
（也不该）去数像素里的对勾。所以在 onInit 末尾打一行结构化状态：

    EDITOR-STATE snap.move=1 snap.rotate=1 snap.scale=1 step.move=0.100
                 step.rotate=10.0 step.scale=0.100 grid.extent=250.0
                 grid.minor=1.000 grid.major=10.000 grid.fade=200.0/250.0
                 theme=0 language=0

（只在 MYVK_LOG_RECTS 下打 —— 本项目统一的自动化观测开关。）

断言：
  A1 三个吸附开关**出厂全开**（用户第 ④ 点的硬要求）
  A2 吸附步长还是 0.1 m / 10 度 / 0.1 倍（别再顺手改了）
  A3 栅格单元格 = 1m × 1m：`grid.minor=1.0`（次格线）且
     `grid.major=10.0`（每 10 格一条主线 → 10m 大方格，UE 的读法）
  A4 栅格面积放大：`grid.extent ≥ 200`（半边长 → ≥400m 见方；改动前 60）
     且 `fadeStart < fadeEnd ≤ extent`
  B1 反例：存档里显式写 `snap.move=0` → 启动后必须是 0
     （证明"出厂默认"和"用户选择"还是分开的两件事，没把默认值写死）
  B2 反例：`language=1` / `theme=4` 能被存档生效（同一行的另外几个字段）

用法：
    python tools/verify_editor_defaults.py
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import find_window, user32                        # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors                # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify")
TMP_DIR = os.path.join(OUT_DIR, "_tmp_prefs")

STATE_RE = re.compile(r"EDITOR-STATE\s+(.*)$")


def parse_state(line):
    out = {}
    for tok in line.split()[1:]:               # 跳过 "EDITOR-STATE"
        if "=" not in tok:
            continue
        k, v = tok.split("=", 1)
        try:
            out[k] = float(v)
        except ValueError:
            out[k] = v
    return out


def launch(exe, prefs_path=None, timeout=8.0):
    env = dict(os.environ)
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_LOG_RECTS"] = "1"
    if prefs_path:
        env["MYVK_EDITOR_PREFS_PATH"] = prefs_path
    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    state = None
    end = time.time() + timeout
    while time.time() < end and state is None:
        with tap.lock:
            for l in tap.lines:
                m = STATE_RE.search(l)
                if m:
                    state = parse_state("EDITOR-STATE " + m.group(1))
                    break
        time.sleep(0.1)
    # 窗口起来后收拾干净
    hwnd = find_window("Venn Editor")
    if hwnd:
        user32.PostMessageW(hwnd, 0x0010, 0, 0)   # WM_CLOSE
        time.sleep(0.5)
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=8)
        except subprocess.TimeoutExpired:
            proc.kill()
    return state


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    if not os.path.exists(exe):
        print("找不到可执行文件:", exe)
        return 2
    os.makedirs(TMP_DIR, exist_ok=True)
    kill_stale_editors()

    fails = []

    def check(name, ok, detail=""):
        print("  [%s] %s %s" % ("PASS" if ok else "FAIL", name, detail))
        if not ok:
            fails.append(name)

    # ---------------- 出厂默认（不设 prefs 路径 = 无存档） ----------------
    st = launch(exe)
    if not st:
        print("拿不到 EDITOR-STATE 日志 —— 起桩失败或没开 MYVK_LOG_RECTS")
        return 3
    print("  出厂:", json.dumps(st, ensure_ascii=False))

    check("A1 三个吸附开关出厂全开", 
          st.get("snap.move") == 1 and st.get("snap.rotate") == 1 and
          st.get("snap.scale") == 1,
          "move=%.0f rotate=%.0f scale=%.0f"
          % (st.get("snap.move", -1), st.get("snap.rotate", -1),
             st.get("snap.scale", -1)))
    check("A2 吸附步长 0.1m / 10° / 0.1×",
          abs(st.get("step.move", 0) - 0.1) < 1e-6 and
          abs(st.get("step.rotate", 0) - 10.0) < 1e-6 and
          abs(st.get("step.scale", 0) - 0.1) < 1e-6,
          "%.3f / %.1f / %.3f" % (st.get("step.move", -1),
                                  st.get("step.rotate", -1),
                                  st.get("step.scale", -1)))
    check("A3 栅格单元格 = 1m × 1m（minor=1, major=10）",
          abs(st.get("grid.minor", 0) - 1.0) < 1e-6 and
          abs(st.get("grid.major", 0) - 10.0) < 1e-6,
          "minor=%.3f major=%.3f" % (st.get("grid.minor", -1),
                                     st.get("grid.major", -1)))
    ext = st.get("grid.extent", 0.0)
    fs = st.get("grid.fadeStart", -1.0)
    fe = st.get("grid.fadeEnd", -1.0)
    check("A4 栅格面积放大到 UE 规模（半边长 ≥ 200m）", ext >= 200.0,
          "extent=%.1f（改动前 60 → 500m 见方）" % ext)
    check("A5 淡出区间合法（0 < fadeStart < fadeEnd ≤ extent）",
          0.0 < fs < fe <= ext + 1e-3,
          "fade %.1f → %.1f，extent %.1f" % (fs, fe, ext))

    # ---------------- 反例：存档还能覆盖默认 ----------------
    p = os.path.join(TMP_DIR, "defaults_override.json")
    with open(p, "w", encoding="utf-8") as f:
        json.dump({"version": 2, "prefs": {"snap.move": 0.0, "snap.rotate": 0.0,
                                           "snap.scale": 0.0, "theme": 4.0,
                                           "language": 1.0,
                                           "grid.extent": 88.0}}, f, indent=2)
    st2 = launch(exe, prefs_path=p)
    if not st2:
        check("B1 存档能覆盖默认", False, "拿不到 EDITOR-STATE")
    else:
        print("  存档覆盖:", json.dumps(st2, ensure_ascii=False))
        check("B1 存档里 snap.move=0 能盖掉出厂默认（默认没被写死）",
              st2.get("snap.move") == 0 and st2.get("snap.rotate") == 0 and
              st2.get("snap.scale") == 0,
              "move=%.0f rotate=%.0f scale=%.0f"
              % (st2.get("snap.move", -1), st2.get("snap.rotate", -1),
                 st2.get("snap.scale", -1)))
        check("B2 同一条状态行里 theme / language 也被存档采纳",
              st2.get("theme") == 4 and st2.get("language") == 1,
              "theme=%.0f language=%.0f" % (st2.get("theme", -1),
                                            st2.get("language", -1)))
        check("B3 存档里的 grid.extent 也生效", 
              abs(st2.get("grid.extent", 0) - 88.0) < 1e-3,
              "extent=%.1f" % st2.get("grid.extent", -1))

    if fails:
        print("结果: FAIL: " + ", ".join(fails))
        return 1
    print("结果: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
