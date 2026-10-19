"""端到端验证：U6（焦点栈 / UI tick 在状态栈之外）+ U8（HUD / 暂停菜单 / 配置）。

四组，前两组起窗口、后两组不重复启动：

  A. **无窗口自检** `Game.exe --selftest hud`
     血量的"显示值追随 / 残影延迟回落 / 夹取"、伤害数字的寿命与对象池、
     血条几何、世界→屏幕的 Y 方向、`config/ui.json` 的往返与夹取。
     全是纯数值 + 文件 IO → 毫秒级、可复现。
     ⚠ 用 `pass >= 20` 当下限而不是精确值（新增断言不该让脚本变红），
       但"断言被删光了仍然 fail=0 → PASS"这个洞必须堵住。

  B. **HUD 真的画上屏了**（`--scene runtime_test/player_scene.json`）
     · `HUD-STATS` 里的 `bar=` 落在**左下角**、宽高比 = 配置的 360:18；
     · 准星：`config/ui.json` 关掉准星后，屏幕中心区域的**显著变化像素占比**
       必须掉下来 —— 这是自校准的否定测试（不猜绝对亮度）；
     · 同一份 `config/ui.json` 把 barWidth 改成 288 → `HUD-STATS bar=` 的
       宽度必须跟着变（配置文件真的被读了）。
     ⚠ 像素比对用"显著变化像素占比"，不用整图平均差：准星只占几十个像素，
       平均差会被背景稀释到噪声以下（MEMORY 里踩过的 0.41/255 假绿）。

  C. **暂停菜单 + 焦点 + 物理冻结**（U6 的验收条款）
     · ESC → `UI2-PAUSE open=1` + 光标可见 + `UI2-FOCUS id=resume`;
     · ↓ ↓ ↓ → settings → quit → 回绕到 resume（`UI2-FOCUS` 序列）；
     · **暂停时按 W**：`GAME-PLAYER-POS` **一条都不新增**（定步不跑），
       而 `UI2-FOCUS` 必须变化（W 走的是 Menu 的 MenuUp）——
       "按 W 不动"是**结构性**的（Gameplay 整组不在栈顶），不是靠 if；
     · ESC → `UI2-PAUSE open=0`，且恢复后的第一个位置与暂停前**逐位相同**。

  D. **脚本扣血 → 血条缩短 + 伤害数字**（`runtime_test/hud_damage_scene.json`）
     夹具脚本在第 60 帧 `damage(30)`：`HUD-STATS` 的 `health` 到 70，
     `fill=` 宽度 = `bar=` 宽度 × 0.7（量几何，不数像素）；
     同一行 `popups>=1`（数字出现），1 秒后 `popups=0`（寿命到了）。

用法：
    python tools/verify_hud.py            # 四组全跑
    python tools/verify_hud.py --only B   # 只跑一组

⚠ 结尾一定是 `结果: PASS` / `结果: FAIL: ...`（回归脚本取**最后一条**）。
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_client, find_window, user32        # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors                # noqa: E402
from verify_asset_drag import client_to_screen                        # noqa: E402
from safe_fs import safe_remove

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Game.exe")
BIN_DIR = os.path.dirname(DEFAULT_EXE)
OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify")

SCENE_PLAYER = "scenes/runtime_test/player_scene.json"
SCENE_DAMAGE = "scenes/runtime_test/hud_damage_scene.json"

# ⚠ 窗口标题必须**唯一**：find_window 是**前缀**匹配，运行时的默认标题
#   "Venn" 会同时命中 "Venn Editor" —— 跑到一半抓到编辑器窗口，截图就全错。
TITLE = "VennHUDTest"

# 暂停菜单的三个按钮：**数组顺序**（Tab 用它），也是 ↓ 的前进方向。
# ⚠ 导航本身是按**几何**算的（UiFocus::nearest），这里之所以能按数组顺序
#   断言，是因为这次是**垂直单列** —— 两者恰好一致。正因为一致才危险：
#   打乱数组顺序的自检放在 engine/ui2/Ui2SelfTest.cpp 的 testFocus 里。
FOCUS_RING = ["resume", "settings", "quit"]

STATS_RE = re.compile(
    r"HUD-STATS popups=(\d+) health=([\d.]+) shown=([\d.]+) trail=([\d.]+) "
    r"bar=([\d.-]+),([\d.-]+),([\d.-]+)x([\d.-]+) "
    r"fill=([\d.-]+),([\d.-]+),([\d.-]+)x([\d.-]+) "
    r"out=(\d+)x(\d+) scale=([\d.]+)")
POS_RE = re.compile(
    r"GAME-PLAYER-POS f=(\d+) pos=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\)")
FOCUS_RE = re.compile(r"UI2-FOCUS id=(\S+)")
PAUSE_RE = re.compile(r"UI2-PAUSE open=(\d) context=(\S+) cursor=(\d)")

VK_ESCAPE, VK_DOWN, VK_W = 0x1B, 0x28, 0x57
# 自己带一张扫描码表：verify_add_menu 的那张里没有 W（MenuUp 用它）。
SC = {VK_ESCAPE: 0x01, VK_DOWN: 0x50, VK_W: 0x11}
# ⚠⚠ 方向键是**扩展键**：lParam 的 bit24（KF_EXTENDED）必须置 1。
#   不置的话 GLFW 的 win32 后端会把扫描码 0x50 翻成 **小键盘 2**，
#   于是"PostMessage 了方向键、引擎只看到 KP_2" —— 焦点一动不动，
#   而这在日志里**没有任何痕迹**（只是"没反应"）。踩过一次。
EXTENDED = {VK_DOWN}

fails = []


def check(name, ok, detail=""):
    print("%s  %s%s" % ("PASS" if ok else "FAIL", name,
                        ("  (%s)" % detail) if detail else ""))
    if not ok:
        fails.append(name)


# ============================================================
# 公共：起进程 / 读锚点
# ============================================================

def _lparam(vk, up):
    lp = 1 | (SC[vk] << 16)
    if vk in EXTENDED:
        lp |= 1 << 24
    if up:
        lp |= (1 << 30) | (1 << 31)
    return lp


def tap_key(hwnd, vk):
    """短按一下。down/up 之间必须隔开 > 一帧（16ms）：同一帧内 down+up
    会被折叠成"没按过"，wasPressed 就永远看不到边沿。"""
    user32.PostMessageW(hwnd, 0x0100, vk, _lparam(vk, False))
    time.sleep(0.06)
    user32.PostMessageW(hwnd, 0x0101, vk, _lparam(vk, True))
    time.sleep(0.35)


def clean_env(**extra):
    """剥掉全部 MYVK_* 再叠 extra —— 测试的输入必须自述。"""
    env = {k: v for k, v in os.environ.items() if not k.startswith("MYVK_")}
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env.update({k: str(v) for k, v in extra.items()})
    return env


def run_once(args, timeout=180):
    """无窗口 / 跑满帧自退的一次性调用。返回 (rc, lines)。"""
    p = subprocess.run([DEFAULT_EXE] + list(args), cwd=BIN_DIR,
                       env=clean_env(), stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True,
                       encoding="utf-8", errors="replace", timeout=timeout)
    return p.returncode, p.stdout.splitlines()


def launch(args, size="1280x720", **env_extra):
    proc = subprocess.Popen([DEFAULT_EXE, "--title", TITLE] + list(args),
                            cwd=BIN_DIR, env=clean_env(**env_extra),
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    hwnd = find_window(TITLE, timeout=20.0)
    if not hwnd:
        proc.kill()
        return proc, None, tap
    user32.ShowWindow(hwnd, 9)
    tw, th = (int(v) for v in size.lower().split("x"))
    # ⚠ 显式摆到 (0,0) + 固定尺寸：Win32 的级联摆放会让两次启动的
    #   客户区差几十像素，截图坐标就全漂了（harness 两类抖动的第二类）。
    user32.SetWindowPos(hwnd, 0, 0, 0, tw, th, 0x0002 | 0x0004)
    time.sleep(2.0)
    return proc, hwnd, tap


def close(proc, hwnd):
    if hwnd:
        user32.PostMessageW(hwnd, 0x0010, 0, 0)   # WM_CLOSE
    time.sleep(0.5)
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=8)
        except subprocess.TimeoutExpired:
            proc.kill()


def lines_of(tap, regex):
    with tap.lock:
        raw = list(tap.lines)
    return [m for m in (regex.search(l) for l in raw) if m]


def wait_for(tap, regex, pred, timeout=15.0):
    """等到某条日志满足 pred(match) —— 返回该 match，超时返回 None。"""
    end = time.time() + timeout
    while time.time() < end:
        hits = [m for m in lines_of(tap, regex) if m and pred(m)]
        if hits:
            return hits[-1]
        time.sleep(0.15)
    return None


def stats_lines(tap):
    return [m for m in lines_of(tap, STATS_RE) if m]


def pos_lines(tap):
    return [m for m in lines_of(tap, POS_RE) if m]


# ============================================================
# A. 无窗口自检
# ============================================================

def case_a():
    print("\n[A] 无窗口自检：Game.exe --selftest hud")
    rc, lines = run_once(["--selftest", "hud"])
    check("[A] selftest hud 退出码 0", rc == 0, "rc=%d" % rc)
    total = None
    for l in lines:
        m = re.search(r"HUD-TOTAL pass=(\d+) fail=(\d+)", l)
        if m:
            total = (int(m.group(1)), int(m.group(2)))
    check("[A] 打出 HUD-TOTAL", total is not None, str(total))
    if not total:
        return
    check("[A] fail=0", total[1] == 0, "fail=%d" % total[1])
    # 下限而不是精确值：新增断言不该让脚本变红，但删光断言必须变红。
    check("[A] 断言条数 >= 20", total[0] >= 20, "pass=%d" % total[0])


# ============================================================
# B + C. 起一次窗口：HUD 上屏 + 暂停菜单
# ============================================================

def crosshair_box(img, out_w, out_h):
    """屏幕中心那一小块（准星所在处），按**截图/输出**比例换算。

    ⚠ 不要用 `VP-RECT` 算坐标：那张离屏图会被 ImGui 等比缩放，
       换算出来的位置整体偏一段。这里只用"中心 + 按比例缩放的半径"。
    """
    if img is None or not out_w or not out_h:
        return (0, 0, 1, 1)
    k = max(img.size[0] / float(out_w), img.size[1] / float(out_h))
    cx = img.size[0] * 0.5
    cy = img.size[1] * 0.5
    r = 26.0 * k          # 准星半长 10 + 空隙 4 逻辑像素，取 26 留余量
    return (int(cx - r), int(cy - r), int(cx + r), int(cy + r))


def diff_ratio(a, b, box):
    """两张图在 box 内**显著变化**的像素占比（阈值 8/255）。"""
    from PIL import ImageChops
    ca = a.convert("RGB").crop(box)
    cb = b.convert("RGB").crop(box)
    d = ImageChops.difference(ca, cb)
    hist = d.convert("L").histogram()
    total = sum(hist) or 1
    changed = sum(hist[9:])         # 差 < 8 视为压缩噪声
    return changed / float(total), d


def write_ui_config(cfg):
    """把 config/ui.json 写到 build/bin/config/（运行时的 exeDir 配置根）。"""
    d = os.path.join(BIN_DIR, "config")
    os.makedirs(d, exist_ok=True)
    p = os.path.join(d, "ui.json")
    with open(p, "w", encoding="utf-8") as f:
        json.dump(cfg, f, indent=2)
    return p


def case_bc():
    print("\n[B] HUD 上屏：血条几何 + 准星 + config/ui.json")
    kill_stale_editors()
    os.makedirs(OUT_DIR, exist_ok=True)

    cfg_path = os.path.join(BIN_DIR, "config", "ui.json")
    shot_base = None
    try:
        # ---- B1：出厂默认 ----
        # ⚠ PLAYER_DUMP 必须开：C 组的"暂停时位置逐位不变"就是量它。
        proc, hwnd, tap = launch(["--scene", SCENE_PLAYER,
                                  "--start-state", "playing"],
                                 MYVK_RUNTIME_PLAYER_DUMP=6)
        if not hwnd:
            check("[B] 运行时窗口起来", False)
            return
        try:
            st = wait_for(tap, STATS_RE, lambda m: int(m.group(13)) > 0)
            check("[B] 打出 HUD-STATS（out>0）", st is not None,
                  st.group(0) if st else "没等到")
            if not st:
                return
            outW, outH = int(st.group(13)), int(st.group(14))
            bx, by, bw, bh = (float(st.group(5)), float(st.group(6)),
                              float(st.group(7)), float(st.group(8)))
            scale = float(st.group(15))
            print("    bar=(%.0f,%.0f) %.0fx%.0f out=%dx%d scale=%.4f"
                  % (bx, by, bw, bh, outW, outH, scale))

            # 血条在**左下角**：左边距 = 下边距 = barMargin×scale
            margin = 24.0 * scale
            check("[B] 血条贴左下角",
                  abs(bx - margin) <= 1.0 and
                  abs((outH - (by + bh)) - margin) <= 1.0,
                  "x=%.1f bottom_gap=%.1f expect=%.1f"
                  % (bx, outH - (by + bh), margin))
            check("[B] 血条宽高比 = 360:18",
                  abs((bw / bh) - (360.0 / 18.0)) < 0.6,
                  "ratio=%.2f expect=20.00" % (bw / bh))

            time.sleep(0.4)
            shot_base = capture_client(hwnd)[0]
            shot_base.save(os.path.join(OUT_DIR, "hud_run.png"))
            check("[B] 截到运行态画面", shot_base is not None)

            # ---- C：暂停菜单（同一个进程，省一次启动）----
            print("\n[C] 暂停菜单 + 焦点 + 物理冻结")

            # 鼠标先挪到左上角：菜单里的按钮在屏幕中央，
            # 悬停会**强行**把焦点拉过去（hoverAt），键盘断言就测不出来了。
            user32.SetCursorPos(*client_to_screen(hwnd, 5, 5))
            time.sleep(0.3)

            # 先在 Playing 下确认 W 真的能推动角色（否证的对照）
            n0 = len(pos_lines(tap))
            user32.PostMessageW(hwnd, 0x0100, VK_W, (0x11 << 16) | 1)
            time.sleep(0.6)
            user32.PostMessageW(hwnd, 0x0101, VK_W, (0x11 << 16) | 0xC0000001)
            time.sleep(0.5)
            ps = pos_lines(tap)
            check("[C] 运行态按 W 能推动角色（注入有效）", len(ps) > n0,
                  "pos 行 %d → %d" % (n0, len(ps)))
            p1 = ps[-1] if ps else None

            if not p1:
                check("[C] 拿到暂停前的位置", False)
                return

            tap_key(hwnd, VK_ESCAPE)
            opened = wait_for(tap, PAUSE_RE, lambda m: m.group(1) == "1")
            check("[C] ESC → UI2-PAUSE open=1", opened is not None,
                  opened.group(0) if opened else "没等到")
            if opened:
                check("[C] 暂停时输入上下文切到 Menu",
                      opened.group(2) == "Menu", opened.group(2))
                check("[C] 暂停时光标可见（菜单要用鼠标）",
                      opened.group(3) == "1", "cursor=" + opened.group(3))

            # 留一张暂停态截图（U8 的交付物之一，肉眼要看得出"菜单 + 唯一高亮项"）
            time.sleep(0.4)
            shot_pause = capture_client(hwnd)[0]
            shot_pause.save(os.path.join(OUT_DIR, "hud_paused.png"))
            same = shot_base is not None and shot_pause.size == shot_base.size
            check("[C] 截到暂停画面", shot_pause is not None and same,
                  "%s vs %s" % (shot_pause.size if shot_pause else "?",
                                shot_base.size if shot_base else "?"))

            f0 = wait_for(tap, FOCUS_RE, lambda m: True, timeout=6.0)
            # ⚠ 起点**不能钉死** resume：`setCursorVisible(true)` 那一刻 GLFW
            #   会把光标还原到它被隐藏前的位置（通常在窗口中央），如果正好
            #   压在某个按钮上，hoverAt 会按设计把焦点拉过去。这**不是** bug
            #   —— 悬停项就该是焦点；能钉的只有"一定有焦点，且在环上"。
            check("[C] 打开菜单即有焦点（在按钮环上）",
                  f0 is not None and f0.group(1) in FOCUS_RING,
                  f0.group(1) if f0 else "没等到")

            # ↓ ↓ ↓ 按**数组环**前进：每一步都必须是环上的下一个（3 步回原点）。
            # ⚠ 基准必须先定：`not seq` 那种写法会在第一轮就接受已经存在的
            #   那条日志，等于什么都没测。
            seq = []
            last = f0.group(1) if f0 else FOCUS_RING[0]
            for _ in range(len(FOCUS_RING)):
                want = FOCUS_RING[(FOCUS_RING.index(last) + 1) % len(FOCUS_RING)]
                tap_key(hwnd, VK_DOWN)
                m = wait_for(tap, FOCUS_RE,
                             lambda mm, _w=want: mm.group(1) == _w,
                             timeout=4.0)
                seq.append((last, want, bool(m)))
                if m:
                    last = m.group(1)
            check("[C] ↓ 按环依次前进 %s" % "→".join(
                      [seq[0][0]] + [x[1] for x in seq]) if seq else "",
                  all(x[2] for x in seq),
                  str(["%s→%s" % (a, b) for a, b, _ in seq]))

            # 焦点回到起点，便于后面用 W（MenuUp，环上倒退一格）观察它变化
            time.sleep(0.2)

            # ---- 核心验收：暂停时按 W ----
            n_before = len(pos_lines(tap))
            fh = lines_of(tap, FOCUS_RE)
            f_before = fh[-1].group(1) if fh else "?"
            user32.PostMessageW(hwnd, 0x0100, VK_W, (0x11 << 16) | 1)
            time.sleep(1.2)
            user32.PostMessageW(hwnd, 0x0101, VK_W, (0x11 << 16) | 0xC0000001)
            time.sleep(0.4)
            n_after = len(pos_lines(tap))
            check("[C] 暂停时按 W 定步不跑（无新的 GAME-PLAYER-POS）",
                  n_after == n_before, "%d → %d" % (n_before, n_after))
            f_after = lines_of(tap, FOCUS_RE)[-1].group(1)
            want_w = FOCUS_RING[(FOCUS_RING.index(f_before) - 1) % len(FOCUS_RING)] \
                if f_before in FOCUS_RING else "?"
            check("[C] 但 W 被菜单吃到了（焦点倒退一格）",
                  f_after == want_w,
                  "%s → %s（期望 %s）" % (f_before, f_after, want_w))

            # ---- 恢复：位置必须逐位不变 ----
            tap_key(hwnd, VK_ESCAPE)
            closed = wait_for(tap, PAUSE_RE, lambda m: m.group(1) == "0")
            check("[C] 再按 ESC → UI2-PAUSE open=0", closed is not None,
                  closed.group(0) if closed else "没等到")
            if closed:
                check("[C] 恢复后上下文回到 Gameplay",
                      closed.group(2) == "Gameplay", closed.group(2))

            m2 = wait_for(tap, POS_RE, lambda m: True, timeout=8.0)
            p2 = pos_lines(tap)[-1] if pos_lines(tap) else None
            if p1 and p2:
                same = (p1.group(2), p1.group(3), p1.group(4)) == \
                       (p2.group(2), p2.group(3), p2.group(4))
                check("[C] 恢复后位置与暂停前逐位相同", same,
                      "before=(%s,%s,%s) after=(%s,%s,%s)"
                      % (p1.group(2), p1.group(3), p1.group(4),
                         p2.group(2), p2.group(3), p2.group(4)))
            else:
                check("[C] 恢复后打出新的 GAME-PLAYER-POS", False)
        finally:
            close(proc, hwnd)

        # ---- B2：config/ui.json 覆盖（关准星 + 改血条宽度）----
        print("\n[B2] config/ui.json 覆盖默认值")
        write_ui_config({"version": 1, "hud": {
            "barWidth": 288, "barHeight": 22, "barMargin": 40,
            "crosshair": False, "crosshairLen": 14, "crosshairGap": 6}})

        proc2, hwnd2, tap2 = launch(["--scene", SCENE_PLAYER,
                                     "--start-state", "playing"])
        if not hwnd2:
            check("[B2] 运行时窗口起来", False)
            return
        try:
            cfglog = wait_for(tap2, re.compile(r"UI-CONFIG path=(\S+) bar=(\d+)x(\d+)"),
                              lambda m: True, timeout=10.0)
            check("[B2] 读到 config/ui.json",
                  cfglog is not None and cfglog.group(2) == "288",
                  cfglog.group(0) if cfglog else "没等到")

            st2 = wait_for(tap2, STATS_RE, lambda m: int(m.group(13)) > 0)
            check("[B2] 打出 HUD-STATS", st2 is not None)
            if not st2:
                return
            outW = int(st2.group(13))
            scale = float(st2.group(15))
            bw = float(st2.group(7))
            bh = float(st2.group(8))
            check("[B2] barWidth=288 生效",
                  abs(bw - 288.0 * scale) <= 1.0,
                  "w=%.1f expect=%.1f" % (bw, 288.0 * scale))
            check("[B2] barHeight=22 生效",
                  abs(bh - 22.0 * scale) <= 1.0,
                  "h=%.1f expect=%.1f" % (bh, 22.0 * scale))

            time.sleep(0.4)
            shot_off = capture_client(hwnd2)[0]
            shot_off.save(os.path.join(OUT_DIR, "hud_nocrosshair.png"))

            # ---- 准星否定测试 ----
            #
            # ⚠ 这里必须带**对照区**，否则会出一个很贵的假绿：
            #   两次启动的窗口只要整体偏了 1~2 像素，整张图逐像素都对不上，
            #   中心区占比直接冲到 100% —— 断言照样"通过"，但它证明的
            #   只是"两张图不一样"，跟准星一点关系都没有。（实测踩到过。）
            #   所以：左上角取一块同尺寸的对照区，它的差异必须接近 0；
            #   中心区的差异必须显著高于对照区（>= 10 倍且 >= 1%）。
            if shot_base is not None and shot_off is not None:
                same_size = shot_base.size == shot_off.size
                check("[B2] 两张截图像素尺寸一致", same_size,
                      "%s vs %s" % (shot_base.size, shot_off.size))
                if same_size:
                    box = crosshair_box(shot_base, outW, int(st2.group(14)))
                    side = box[2] - box[0]
                    ctrl = (20, 20, 20 + side, 20 + side)
                    r_c, _ = diff_ratio(shot_base, shot_off, box)
                    r_k, _ = diff_ratio(shot_base, shot_off, ctrl)
                    print("    中心区 %.3f%%  对照区(20,20)+%d %.3f%%"
                          % (r_c * 100.0, side, r_k * 100.0))
                    check("[B2] 对照区基本不变（同一场景、同一取景）",
                          r_k < 0.02, "%.3f%%" % (r_k * 100.0))
                    check("[B2] 关掉准星后中心区显著变化（>=1% 且 >=10x 对照）",
                          r_c >= 0.01 and r_c >= 10.0 * max(r_k, 1e-6),
                          "%.3f%% vs 对照 %.3f%%" % (r_c * 100.0, r_k * 100.0))
        finally:
            close(proc2, hwnd2)
    finally:
        # ⚠ 必须清掉：这份 config/ui.json 会让**其他**用例也少一条准星
        if os.path.isfile(cfg_path):
            safe_remove(cfg_path)


# ============================================================
# D. 脚本扣血 → 血条缩短 + 伤害数字
# ============================================================

def case_d():
    print("\n[D] 脚本扣血 → 血条缩短 + 伤害数字")
    rc, lines = run_once(["--scene", SCENE_DAMAGE, "--start-state", "playing",
                          "--frames", "260"])
    check("[D] 夹具场景跑完", rc == 0, "rc=%d" % rc)

    stats = []
    for l in lines:
        m = STATS_RE.search(l)
        if m:
            stats.append(m)
    check("[D] 打出 HUD-STATS", len(stats) > 0, "n=%d" % len(stats))
    if not stats:
        return

    # 夹具脚本第 60 帧 damage(30) → health 必须从 100 掉到 70
    hurt = [m for m in stats if abs(float(m.group(2)) - 70.0) < 0.01]
    check("[D] 扣血 30 后 health=70", len(hurt) > 0,
          "health 序列: %s" % ", ".join(m.group(2) for m in stats))

    if hurt:
        h = hurt[0]
        bw = float(h.group(7))
        fw = float(h.group(11))
        check("[D] 血条填充 = 70% 槽宽（量几何）",
              abs(fw - bw * 0.7) <= 1.0,
              "fill=%.1f expect=%.1f" % (fw, bw * 0.7))
        # 伤害数字出现：同一条 HUD-STATS 里 popups>=1
        check("[D] 伤害数字出现（popups>=1）", int(h.group(1)) >= 1,
              "popups=%s" % h.group(1))

    # 1 秒寿命：后面必须有一条 popups=0
    tail = [m for m in stats[-2:]]
    check("[D] 伤害数字 1 秒后自己消失（末段 popups=0）",
          all(int(m.group(1)) == 0 for m in tail),
          ", ".join("popups=" + m.group(1) for m in tail))

    # 残影：掉血瞬间 trail 还在 100 附近（否则"刚掉了多少"看不见）
    if hurt:
        h = hurt[0]
        check("[D] 掉血瞬间残影还停在满血附近",
              float(h.group(4)) > 90.0, "trail=%s" % h.group(4))
        print("    掉血那一行: health=%s shown=%s trail=%s"
              % (h.group(2), h.group(3), h.group(4)))


# ============================================================

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="ABCD",
                    help="只跑指定组，如 --only AB（默认 ABCD）")
    args = ap.parse_args()
    only = args.only.upper()

    os.makedirs(OUT_DIR, exist_ok=True)
    if not os.path.isfile(DEFAULT_EXE):
        print("结果: FAIL: 找不到 %s（先 ./make.cmd build）" % DEFAULT_EXE)
        return 4

    if "A" in only:
        case_a()
    if "B" in only or "C" in only:
        case_bc()
    if "D" in only:
        case_d()

    print()
    if fails:
        print("结果: FAIL:（%d 项）" % len(fails))
        for f in fails:
            print("   -", f)
        return 6
    print("结果: PASS（HUD 自检 + 血条几何 + 准星否定测试 + "
          "暂停焦点导航 + 暂停冻结 + 脚本扣血）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
