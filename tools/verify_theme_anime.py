#!/usr/bin/env python3
"""二次元风格改造（2026-10-27）的端到端验收。

用户六条需求的编号沿用对话里的顺序：
  ① 启动器窗口调大         → B1 / B2
  ② 窗口圆角、顶栏不呆板    → A(度量) + B3(顶栏渐变)
  ③ 面板配色错落有致        → A(anime 层次) + C3(换主题后色相真的变)
  ④ 面板自由拖拽/合并       → **本轮未做**（见 docs/anime_ui_restyle.md §5）
  ⑤ 按钮/图标像二次元       → B4(药丸按钮) + A(圆角度量)
  ⑥ 主题多样化              → A(10 套) + C1~C3(色卡可点、点了真换)

验什么：
  A  `Game.exe --selftest theme` 22 条 + **分组存在性**
     （只数总数的话"整组被删掉"也能靠别组凑够）
  B  启动器：客户区尺寸 == 1400x880、窗口底是"深紫"而不是中灰、
     顶栏是**左亮右暗的渐变**（用户第 2 条要的"不呆板"就是它）
  C  编辑器 Preferences：10 枚主题色卡都在（锚点 PREFS-THEMECHIP-0..9），
     点第 6 枚（Matcha Mint）之后 Preferences 窗口内**绿色分量抬起来** ——
     这是"色卡不是画着好看、点了真换"的唯一证据。

不验什么：
  * 字体 / 字形糊不糊 —— 脚本判不了，必须人眼看（项目纪律）。
  * 圆角的"好看程度" —— 只能断言度量值（A 组），观感靠截图。

用法：
    python tools/verify_theme_anime.py
"""

import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_client, find_window, user32        # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors, wait_rect     # noqa: E402
from verify_bvh import click_client_post                             # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
GAME = os.path.join(BIN, "Game.exe")
EDITOR = os.path.join(BIN, "Editor.exe")
LAUNCHER = os.path.join(BIN, "Launcher.exe")
OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify")

# 启动器目标尺寸（engine 侧 kWindowW / kWindowH）。屏幕工作区更小时会被夹，
# 所以断言是"**不超过** 1400x880 且 **明显大于**旧值 1080x720"。
WANT_W, WANT_H = 1400, 880
OLD_W, OLD_H = 1080, 720

results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print("[%s] %s  %s" % ("PASS" if ok else "FAIL", name, detail))
    return bool(ok)


def run_game(args, wait_s=40.0):
    env = dict(os.environ)
    env.pop("MYVK_LOG_DT", None)
    proc = subprocess.Popen([GAME] + args, cwd=BIN, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        out, _ = proc.communicate(timeout=wait_s)
    except subprocess.TimeoutExpired:
        proc.kill()
        try:
            out, _ = proc.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            out = b""
    return (out or b"").decode("utf-8", "replace").replace("\x00", "")


def mean_rgb(img, box=None):
    if box:
        img = img.crop(tuple(int(round(v)) for v in box))
    px = img.convert("RGB").resize((120, 80))
    w, h = px.size
    data = px.load()
    r = g = b = 0.0
    for y in range(h):
        for x in range(w):
            pr, pg, pb = data[x, y]
            r += pr
            g += pg
            b += pb
    n = float(w * h)
    return r / n, g / n, b / n


def log_has(tap, needle):
    with tap.lock:
        return any(needle in line for line in tap.lines)


# ------------------------------------------------------------------ A 自检
def group_selftest():
    out = run_game(["--selftest", "theme"], wait_s=30.0)
    total = re.search(r"THEME-TOTAL pass=(\d+) fail=(\d+)", out)
    fails = re.findall(r"THEME-TEST (\S+).*result=FAIL", out)
    ok = total is not None and total.group(2) == "0" and int(total.group(1)) >= 20
    check("A1 `--selftest theme` 全绿（>=20 条）", ok,
          (total.group(0) if total else "没有 THEME-TOTAL 行") +
          ("；FAIL=%s" % ",".join(fails) if fails else ""))

    # 分组存在性：每组挑一条"只有这组才有"的断言当锚点
    for group, first in (("indices", "venn_window_bg"),
                         ("distinct", "themes_all_distinct"),
                         ("brightness", "lightflag_matches_luma"),
                         ("metrics", "anime_metrics_ok"),
                         ("apply", "clamp_low")):
        check("A2 分组 %s 有断言（%s）" % (group, first),
              ("THEME-TEST %s " % first) in out,
              "锚点%s" % ("" if first in out else "缺"))

    # 10 套主题：这条只能从自检里读，脚本自己数不出来
    m = re.search(r"THEME-TEST theme_count actual=(\d+)", out)
    check("A3 主题共 10 套", m is not None and int(m.group(1)) == 10,
          "actual=%s" % (m.group(1) if m else "?"))


# ------------------------------------------------------------------ B 启动器
def group_launcher():
    env = dict(os.environ)
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_LOG_RECTS"] = "1"
    # ⚠ 必须 text=True：LogTap 里是 `re.search` 字符模式，
    #   喂 bytes 会抛 `cannot use a string pattern on a bytes-like object`。
    proc = subprocess.Popen([LAUNCHER], cwd=BIN, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    try:
        hwnd = find_window("Venn Launcher", timeout=30.0)
        if not hwnd:
            check("B1 启动器窗口出现", False, "25s 内没找到")
            return
        user32.ShowWindow(hwnd, 9)
        time.sleep(2.5)
        img, ok = capture_client(hwnd)
        if not ok:
            time.sleep(2.0)
            img, ok = capture_client(hwnd)
        w, h = img.size
        check("B1 启动器客户区被放大（> 旧值 %dx%d，<= %dx%d）" % (OLD_W, OLD_H, WANT_W, WANT_H),
              w > OLD_W and h > OLD_H and w <= WANT_W and h <= WANT_H,
              "size=%dx%d" % (w, h))
        img.save(os.path.join(OUT_DIR, "anime_launcher_check.png"))

        # B2 窗口底：樱花夜 = 深紫。判据用"紫色调 + 足够暗"两条，
        #   不写死具体 RGB —— 以后微调色号不该让用例红。
        r, g, b = mean_rgb(img, (w * 0.55, h * 0.55, w * 0.95, h * 0.85))
        luma = 0.299 * r + 0.587 * g + 0.114 * b
        check("B2 窗口底是深色且偏紫（B>G, R>G, 亮度<90）",
              b > g + 2 and r > g + 1 and luma < 90,
              "rgb=(%.0f,%.0f,%.0f) luma=%.1f" % (r, g, b, luma))

        # B3 顶栏渐变：同一行上，左端比右端**更亮/更红**（强调色在左）
        hdr = None
        for _ in range(20):
            hdr = tap.last_rect("LAUNCH-RECT-header")
            if hdr:
                break
            time.sleep(0.3)
        if not hdr:
            check("B3 顶栏锚点存在", False, "没有 LAUNCH-RECT-header")
        else:
            y = (hdr[1] + hdr[3]) * 0.5
            lr, lg, lb = mean_rgb(img, (hdr[0] + 6, y - 2, hdr[0] + 26, y + 2))
            rr, rg, rb = mean_rgb(img, (hdr[2] - 26, y - 2, hdr[2] - 6, y + 2))
            check("B3 顶栏是渐变（左端比右端更亮）", lr > rr + 6,
                  "left=%.0f right=%.0f" % (lr, rr))
    finally:
        try:
            proc.terminate()
            proc.wait(timeout=8)
        except Exception:
            proc.kill()


# ------------------------------------------------------------------ C 编辑器
def group_editor():
    env = dict(os.environ)
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_EDITOR_PREFS"] = "1"
    proc = subprocess.Popen([EDITOR], cwd=BIN, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    try:
        hwnd = find_window("Venn Editor", timeout=45.0)
        if not hwnd:
            check("C1 编辑器窗口出现", False, "45s 内没找到")
            return
        user32.ShowWindow(hwnd, 9)
        time.sleep(2.0)
        user32.SetWindowPos(hwnd, 0, 0, 0, 1600, 900, 0x0002 | 0x0004)
        time.sleep(3.0)

        # 色卡只在 Appearance 页绘制，而 Preferences 默认停在 Interface 页
        # → 必须先切页再等锚点，否则会白等 10 x 12s 后全 FAIL
        tab = wait_rect(tap, "PREFS-TAB-Appearance", 12.0)
        if not tab:
            check("C0 找到 Appearance 页签", False, "没有 PREFS-TAB-Appearance")
            return
        click_client_post(hwnd, (tab[0] + tab[2]) * 0.5,
                          (tab[1] + tab[3]) * 0.5)
        time.sleep(1.2)

        # C1 十枚色卡都在（0..9 各自一个锚点）
        missing = []
        for i in range(10):
            if not wait_rect(tap, "PREFS-THEMECHIP-%d" % i, 8.0):
                missing.append(i)
        check("C1 Preferences 里有 10 枚主题色卡", not missing,
              "缺=%s" % (missing or "无"))

        win = wait_rect(tap, "PREFS-WIN", 6.0)
        chip6 = wait_rect(tap, "PREFS-THEMECHIP-6", 8.0)
        if not (win and chip6):
            check("C2 拿到 PREFS-WIN / THEMECHIP-6 锚点", False)
            return

        before, _ = capture_client(hwnd)
        br, bg_, bb = mean_rgb(before, win)
        before_path = os.path.join(OUT_DIR, "anime_theme_before.png")
        before.save(before_path)

        # C2 点第 6 枚（Matcha Mint = 墨绿 + 薄荷）→ Preferences 内绿色分量必须抬起
        click_client_post(hwnd, (chip6[0] + chip6[2]) * 0.5,
                          (chip6[1] + chip6[3]) * 0.5)
        time.sleep(1.5)
        after, _ = capture_client(hwnd)
        ar, ag, ab = mean_rgb(after, win)
        after_path = os.path.join(OUT_DIR, "anime_theme_after.png")
        after.save(after_path)
        check("C2 点『Matcha Mint』色卡后界面真的变绿（G 抬升）",
              (ag - ar) > (bg_ - br) + 3.0,
              "before r=%.0f g=%.0f | after r=%.0f g=%.0f" % (br, bg_, ar, ag))
        has_anchor = log_has(tap, "EDITOR-THEME index=6")
        check("C2b 日志里有 EDITOR-THEME index=6", has_anchor,
              "锚点%s" % ("" if has_anchor else "缺"))
    finally:
        try:
            proc.terminate()
            proc.wait(timeout=8)
        except Exception:
            proc.kill()


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    kill_stale_editors()
    for path in (GAME, EDITOR, LAUNCHER):
        if not os.path.exists(path):
            print("找不到可执行文件:", path)
            return 2

    print("== A 主题自检 ==")
    group_selftest()
    print("== B 启动器 ==")
    group_launcher()
    print("== C 编辑器主题色卡 ==")
    group_editor()

    print("截图 →", OUT_DIR)
    fails = [n for n, ok, _ in results if not ok]
    if fails:
        print("结果: FAIL: " + ", ".join(fails))
        return 1
    print("结果: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
