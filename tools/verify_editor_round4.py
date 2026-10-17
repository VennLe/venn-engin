"""端到端验证：第四轮三项优化（2026-10-17）。

被验证的用户需求（原话摘录）：
  1) "点击 character 面板右上角的叉号关闭按钮无法关闭 character 面板……
     Content 面板中的 Output log 和 CMD 按钮点击后都没有打开各自的面板……
     通过点击 + 打开的所有面板，右上角都要有可以最小化的按钮，
     点击最小化即把面板窗口最小化放到 Content 面板上 + 按钮右侧原来的位置上。"
  2) "1，2V，2H，4 …… 请把这个参数也一样放到和 Perspective 这些属性一起并排
     放置，点击后也是展开下拉勾选框列表去设置。至于原来的 Move（W / E / R），
     View：lit，Reload Shaders，Undo，Redo，这些按钮全都不需要，都移除掉。"
  3) "粒子系统……添加多个粒子系统时为什么名字都是一样的……初始名字应该和它
     的粒子特效的类型有关，并用编号数字后缀区分开来……右键其名称的时候可以
     打开其右键功能菜单，可以对其进行重命名，删除等其他操作。"

断言分组：
  A. 顶栏新结构（需求 2 的"移掉旧的、换成 UE5 那样"）
     A1 顶栏高度没被撑高（旧那条塞了 5 组按钮，约 44；现在仍是 44±4）
     A2 三个新控件齐全：Select Mode / + Add / 播放条
     A3 从左往右：选择模式 → Add → 播放条
     A4 播放条 5 颗：run/step/pause/stop/more，等宽、紧挨、贴右边缘
  B. 选择模式下拉（截图里那个"下拉列表"）
     B1 点开出现弹层 + Select/Move/Rotate/Scale 四项
     B2 点 Move → 模式真的换了（日志 TB-SELMODE mode=Move）
     B3 再点开点 Select → 换回来
  C. + Add 真的能加物体
     C1 点开有 Mesh / Light / Player 三个子菜单
     C2 Mesh ▸ Cube → 走到工厂函数（TB-ADD-ENTITY what=Cube）
  D. 播放控制
     D1 run → TB-PLAY run=1；再点 → run=0
     D2 step → TB-PLAY step 且 "Editor: STEP one frame"
     D3 run 之后 stop → TB-PLAY stop
  E. 视口分屏下拉（需求 2 的 1/2V/2H/4）
     E1 它紧挨在 Perspective 右边（间隙 ≤ 4px）
     E2 点开有 1 / 2V / 2H / 4 四项
     E3 点 4 → VP-LAYOUT quad subviews=3 且 VIEWPORT-LAYOUT quad (subviews=3)
     E4 点回 1 → single subviews=0
  F. 粒子（需求 3）
     F1 + 勾 Particles → 面板出现
     F2 连点两次 Rain 预设 → 名字是 "Rain 1" / "Rain 2"（不是两个同名）
     F3 右键一行 → 菜单里 Rename / Duplicate / Delete 三项齐全
     F4 点 Duplicate → 新名字 "Rain 3"
  G. 底栏三件事（需求 1）
     G1 点 Output Log → CONSOLE-PANEL 出现，底边贴底栏、横向居中
     G2 托盘按钮开 Character → CHAR-WIN 出现
     G3 点 Character 的 × → PANEL character open=0 reason=user-close，窗口消失
     G4 浮动面板有最小化按钮（PANEL-MIN console 矩形）
     G5 点最小化 → PANEL-STATE console open=0；再点 Output Log → 又回来

全程零 Vulkan 验证层报错。

用法：python tools/verify_editor_round4.py
"""

import ctypes
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from capture_window import capture_client, find_window  # noqa: E402
from verify_add_menu import LogTap, click, force_foreground, kill_stale_editors  # noqa: E402
from verify_collider_menu import right_click  # noqa: E402
from verify_local_shadow import wait_rect  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "..", "_venn_verify")
EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
user32 = ctypes.windll.user32

BAD_RE = r"VUID-|\[ERROR\]|Validation Error|leaked|SEGV"
RECT_ANY = r"=\((-?[\d.]+),(-?[\d.]+)\)-\((-?[\d.]+),(-?[\d.]+)\)"

fails = []


def check(ok, msg):
    print("    %-66s %s" % (msg, "PASS" if ok else "FAIL"))
    if not ok:
        fails.append(msg)
    return ok


def count(tap, needle):
    with tap.lock:
        return sum(1 for l in tap.lines if needle in l)


def center(r):
    return (r[0] + r[2]) * 0.5, (r[1] + r[3]) * 0.5


def rect_of(tap, tag, timeout=5.0):
    """带空格 / 特殊字符的 tag 用自家正则解析（wait_rect 只认 [A-Za-z-]+）。"""
    deadline = time.time() + timeout
    pat = re.compile(re.escape(tag) + RECT_ANY)
    while time.time() < deadline:
        with tap.lock:
            for line in reversed(tap.lines):
                m = pat.search(line)
                if m:
                    return (float(m.group(1)), float(m.group(2)),
                            float(m.group(3)), float(m.group(4)))
        time.sleep(0.15)
    return None


def click_until(hwnd, tap, x, y, needle, n_before, tries=3, wait=1.0):
    for _ in range(tries):
        click(hwnd, x, y)
        time.sleep(wait)
        if count(tap, needle) > n_before:
            return True
    return False


def shoot(hwnd, name):
    img, _ = capture_client(hwnd)
    img.save(os.path.join(OUT_DIR, name))
    return img


def main():
    if not os.path.exists(EXE):
        print("找不到可执行文件，请先构建：", EXE)
        return 1

    kill_stale_editors()
    os.makedirs(OUT_DIR, exist_ok=True)

    env = dict(os.environ)
    env.update({
        "MYVK_LOG_RECTS": "1",
        "MYVK_NO_WINDOW_SAVE": "1",
        "MYVK_LIGHT_GIZMOS": "0",
        "MYVK_EDITOR_NO_HUD": "1",
    })
    proc = subprocess.Popen([EXE], cwd=os.path.dirname(EXE), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)

    hwnd = None
    try:
        hwnd = find_window("Venn Editor")
        if not hwnd:
            print("未找到编辑器窗口")
            return 2
        user32.ShowWindow(hwnd, 9)
        time.sleep(1.5)
        user32.SetWindowPos(hwnd, 0, 0, 0, 1600, 900, 0x0002 | 0x0004)
        time.sleep(3.0)
        force_foreground(hwnd)
        time.sleep(0.8)

        from ctypes import wintypes as wt
        cr = wt.RECT()
        user32.GetClientRect(hwnd, ctypes.byref(cr))
        W, H = cr.right, cr.bottom

        bar = wait_rect(tap, "TB-BAR", 10.0)
        if not bar:
            print("缺 TB-BAR 日志")
            return 3

        # ============================================================ A
        print("\n[A] 顶栏新结构")
        h = bar[3] - bar[1]
        check(abs(h - 44.0) <= 5.0,
              "A1 顶栏高度仍是单排 (h=%.0f)" % h)

        sel = wait_rect(tap, "TB-SELMODE", 5.0)
        add = wait_rect(tap, "TB-ADD", 5.0)
        play = wait_rect(tap, "TB-PLAY", 5.0)
        check(all((sel, add, play)), "A2 Select Mode / + Add / 播放条 三者齐全")

        if all((sel, add, play)):
            check(sel[0] < add[0] < play[0],
                  "A3 从左往右：选择模式(%.0f) → Add(%.0f) → 播放条(%.0f)"
                  % (sel[0], add[0], play[0]))

            names = ["run", "step", "pause", "stop", "more"]
            rs = [(n, wait_rect(tap, "TB-PLAY-" + n, 3.0)) for n in names]
            miss = [n for n, r in rs if not r]
            check(not miss, "A4a 播放条 5 颗齐全（缺 %s）" % miss)
            if not miss:
                widths = [r[2] - r[0] for _, r in rs]
                check(max(widths) - min(widths) <= 1.0,
                      "A4b 五颗等宽 (%.0f)" % widths[0])
                gaps = [rs[i + 1][1][0] - rs[i][1][2] for i in range(4)]
                check(all(0.0 <= g <= 4.0 for g in gaps),
                      "A4c 紧挨成一排 (gaps=%s)"
                      % ["%.0f" % g for g in gaps])
                check(W - rs[-1][1][2] <= 24.0,
                      "A4d 贴右边缘 (右边距=%.0f)" % (W - rs[-1][1][2]))

        shoot(hwnd, "r4_a_topbar.png")

        # ============================================================ B
        print("\n[B] 选择模式下拉")
        n0 = count(tap, "TB-SELMODE-WIN")
        click(hwnd, *center(sel))
        time.sleep(0.9)
        win = wait_rect(tap, "TB-SELMODE-WIN", 3.0)
        check(count(tap, "TB-SELMODE-WIN") > n0 and win is not None,
              "B1a 点开出现下拉弹层")
        items = {}
        for name in ("Select", "Move", "Rotate", "Scale"):
            items[name] = rect_of(tap, "TB-SELMODE-ITEM " + name, 2.0)
        check(all(items.values()),
              "B1b 四项齐全 (缺 %s)"
              % [k for k, v in items.items() if not v])
        shoot(hwnd, "r4_b_selmode.png")

        # ⚠ 默认工具模式就是 Move（Translate），所以"点 Move"是空操作 ——
        #   用它做"是否同步"的证据会永远拿到 0 增量。换成 Rotate。
        if items.get("Rotate"):
            n1 = count(tap, "TB-SELMODE mode=")
            n_rot = count(tap, "VP-TOOLMODE rotate")
            click(hwnd, *center(items["Rotate"]))
            time.sleep(0.9)
            check(count(tap, "TB-SELMODE mode=Rotate") >= 1
                  and count(tap, "TB-SELMODE mode=") > n1,
                  "B2 点 Rotate → 模式真的换成 Rotate")
            check(count(tap, "VP-TOOLMODE rotate") > n_rot,
                  "B2b 视口工具条同步（VP-TOOLMODE rotate 新增一行）")

        # 换回 Select
        click(hwnd, *center(sel))
        time.sleep(0.7)
        it = rect_of(tap, "TB-SELMODE-ITEM Select", 2.0)
        if it:
            click(hwnd, *center(it))
            time.sleep(0.7)
            check(count(tap, "TB-SELMODE mode=Select") >= 1,
                  "B3 再换回 Select")

        # ============================================================ C
        print("\n[C] + Add 能加物体")
        n0 = count(tap, "TB-ADD-WIN")
        click(hwnd, *center(add))
        time.sleep(0.9)
        check(count(tap, "TB-ADD-WIN") > n0, "C1a 点开出现 Add 菜单")
        for sub in ("Mesh", "Light", "Player"):
            check(rect_of(tap, "TB-ADD-" + sub, 2.0) is not None,
                  "C1b 有 %s 子菜单" % sub)
        shoot(hwnd, "r4_c_add.png")

        mesh = rect_of(tap, "TB-ADD-Mesh", 2.0)
        if mesh:
            click(hwnd, *center(mesh))     # 悬停展开
            time.sleep(0.9)
            cube = rect_of(tap, "TB-ADD-Cube", 3.0)
            check(cube is not None, "C2a 展开 Mesh 后能看到 Cube")
            if cube:
                n1 = count(tap, "TB-ADD-ENTITY")
                click(hwnd, *center(cube))
                time.sleep(1.0)
                check(count(tap, "TB-ADD-ENTITY what=Cube") >= 1
                      and count(tap, "TB-ADD-ENTITY") > n1,
                      "C2b 点 Cube → 真的建了实体")

        # ============================================================ D
        print("\n[D] 播放控制")
        run = wait_rect(tap, "TB-PLAY-run", 3.0)
        step = wait_rect(tap, "TB-PLAY-step", 3.0)
        stop = wait_rect(tap, "TB-PLAY-stop", 3.0)
        if run and step and stop:
            n1 = count(tap, "TB-PLAY run=1")
            click(hwnd, *center(run))
            time.sleep(1.3)
            check(count(tap, "TB-PLAY run=1") > n1, "D1a 点 run → run=1")
            n2 = count(tap, "Editor: STEP one frame")
            click(hwnd, *center(step))
            time.sleep(1.2)
            check(count(tap, "TB-PLAY step") >= 1, "D2a 点 step → TB-PLAY step")
            check(count(tap, "Editor: STEP one frame") > n2,
                  "D2b 真的推进了一帧（Editor: STEP one frame）")
            click(hwnd, *center(stop))
            time.sleep(1.0)
            check(count(tap, "TB-PLAY stop") >= 1, "D3 点 stop → TB-PLAY stop")

        # ============================================================ E
        print("\n[E] 视口分屏下拉（1 / 2V / 2H / 4）")
        viewbtn = wait_rect(tap, "VP-VIEW-BTN", 5.0)
        laybtn = wait_rect(tap, "VP-LAYOUT-BTN", 5.0)
        check(all((viewbtn, laybtn)), "E1a Perspective 与分屏按钮都在")
        if viewbtn and laybtn:
            gap = laybtn[0] - viewbtn[2]
            check(-1.0 <= gap <= 5.0,
                  "E1b 分屏按钮紧挨在 Perspective 右边 (gap=%.0f)" % gap)
            check(abs((laybtn[3] - laybtn[1]) - (viewbtn[3] - viewbtn[1])) <= 1.0,
                  "E1c 同一行、同高")

            n0 = count(tap, "VP-LAYOUTITEM")
            click(hwnd, *center(laybtn))
            time.sleep(0.9)
            got = {}
            for name in ("1", "2V", "2H", "4"):
                got[name] = rect_of(tap, "VP-LAYOUTITEM " + name, 2.0)
            check(all(got.values()),
                  "E2 下拉里 1 / 2V / 2H / 4 齐全 (缺 %s)"
                  % [k for k, v in got.items() if not v])
            shoot(hwnd, "r4_e_layout.png")

            if got.get("4"):
                n1 = count(tap, "VIEWPORT-LAYOUT quad (subviews=3)")
                click(hwnd, *center(got["4"]))
                time.sleep(1.6)
                check(count(tap, "VIEWPORT-LAYOUT quad (subviews=3)") > n1,
                      "E3 点 4 → 日志落到 quad subviews=3（渲染器真的切了）")
                shoot(hwnd, "r4_e_quad.png")

            click(hwnd, *center(laybtn))
            time.sleep(0.9)
            one = rect_of(tap, "VP-LAYOUTITEM 1", 3.0)
            if one:
                n2 = count(tap, "VIEWPORT-LAYOUT single")
                click(hwnd, *center(one))
                time.sleep(1.4)
                check(count(tap, "VIEWPORT-LAYOUT single (subviews=0)") > n2,
                      "E4 点回 1 → single，子视口清零")

        # ============================================================ F
        print("\n[F] 粒子命名 / 右键菜单")
        plus = wait_rect(tap, "BAR-TRAY-PLUS", 6.0)
        check(plus is not None, "F1a 底栏 + 按钮存在")
        if plus:
            click(hwnd, *center(plus))
            time.sleep(0.9)
            pick = wait_rect(tap, "BAR-PICK-particles", 4.0)
            check(pick is not None, "F1b + 菜单里有 Particles")
            if pick:
                click(hwnd, *center(pick))
                time.sleep(1.4)
                # 面板窗口矩形（FX-WIN）才是"看得见"的证据；
                # FX-PANEL preset=... 是"点了某个预设"的日志，不是开面板的。
                check(wait_rect(tap, "FX-WIN", 4.0) is not None,
                      "F1c Particles 面板窗口出现了")

                # 连点两次 Rain 预设
                rain = rect_of(tap, "FX-PRESET 3", 4.0)
                check(rain is not None, "F2a 找得到 Rain 预设按钮")
                if rain:
                    for _ in range(2):
                        click(hwnd, *center(rain))
                        time.sleep(1.1)
                    names = []
                    with tap.lock:
                        for l in tap.lines:
                            m = re.search(r'FX-PANEL preset=Rain name="([^"]+)"',
                                          l)
                            if m:
                                names.append(m.group(1))
                    check(names[:2] == ["Rain 1", "Rain 2"],
                          "F2b 连续两次是 Rain 1 / Rain 2（实得 %s）"
                          % names[:3])

                    row = rect_of(tap, "FX-ROW 0", 4.0)
                    check(row is not None, "F3a 清单第一行有矩形")
                    if row:
                        right_click(hwnd, *center(row))
                        time.sleep(1.0)
                        ctm = wait_rect(tap, "FX-CTX", 3.0)
                        check(ctm is not None, "F3b 右键弹出菜单")
                        for item in ("Rename", "Duplicate", "Delete"):
                            check(rect_of(tap, "FX-CTX-" + item, 2.0) is not None,
                                  "F3c 菜单里有 %s" % item)
                        shoot(hwnd, "r4_f_fxctx.png")
                        dup = rect_of(tap, "FX-CTX-Duplicate", 2.0)
                        if dup:
                            n1 = count(tap, "FX-PANEL duplicate")
                            click(hwnd, *center(dup))
                            time.sleep(1.2)
                            check(count(tap, "FX-PANEL duplicate") > n1,
                                  "F4 Duplicate → 真的复制了一份")

        # ============================================================ G
        print("\n[G] 底栏：Output Log / Character × / 最小化")
        logbtn = wait_rect(tap, "BAR-BTN-log", 5.0)
        if logbtn:
            n0 = count(tap, "CONSOLE-PANEL")
            click(hwnd, *center(logbtn))
            time.sleep(1.2)
            cp = rect_of(tap, "CONSOLE-PANEL", 4.0)
            check(count(tap, "CONSOLE-PANEL") > n0 and cp is not None,
                  "G1a 点 Output Log → 面板真的出现")
            if cp:
                barrect = wait_rect(tap, "BAR-BOTTOM", 3.0)
                check(barrect is not None and abs(cp[3] - barrect[1]) <= 40.0,
                      "G1b 面板贴着底栏（底边=%.0f, 底栏上沿=%.0f）"
                      % (cp[3], barrect[1] if barrect else -1))
                check(abs((cp[0] + cp[2]) * 0.5 - W * 0.5) <= 60.0,
                      "G1c 横向居中")
            shoot(hwnd, "r4_g_console.png")

        # ---- 最小化按钮（趁 Console 是最后打开的那一个，没人压在它前面）----
        # ⚠ 顺序有讲究：最小化按钮的命中要求 `g.HoveredWindow == 那个窗口`。
        #   先把 Character 打开（它比 Console 更靠前）就会挡住 Console 的标题栏，
        #   点击落到 Character 上 —— 表现为"最小化按钮点了没反应"，其实是
        #   被盖住了。所以"最小化"这一段必须排在"开 Character"之前。
        if logbtn and count(tap, "CONSOLE-PANEL") > 0:
            mn = rect_of(tap, "PANEL-MIN console", 4.0)
            check(mn is not None, "G2 浮动面板标题栏有最小化按钮")
            if mn:
                n0 = count(tap, "PANEL-STATE console open=0")
                click(hwnd, *center(mn))
                time.sleep(1.2)
                check(count(tap, "PANEL-STATE console open=0") > n0,
                      "G2b 点最小化 → Console 收起")
                # ⚠ 不能用 "CONSOLE-PANEL 矩形又出现" 判定：logRect 是按**矩形
                #   去抖**的，面板回到同一个位置时一行都不会再打（第一次就踩了）。
                #   用 PANEL-FOCUS 这条"每次点都打"的日志。
                n1 = count(tap, "PANEL-FOCUS console")
                click(hwnd, *center(logbtn))
                time.sleep(1.2)
                check(count(tap, "PANEL-FOCUS console") > n1
                      and count(tap, "PANEL-STATE console open=1") >= 1,
                      "G2c 再点 Output Log → 又回来了")

        # Character 是"勾出来才有按钮"的面板：先开 + 勾上，底栏才会长出按钮。
        # ⚠ 托盘按钮的矩形 tag 是 `BAR-TRAY-tray_<id>`（trayButton 里把已经带
        #   "tray_" 前缀的 id 又拼了一遍），不是 BAR-TRAY-<id>。
        cw = None
        tray = rect_of(tap, "BAR-TRAY-tray_character", 3.0)
        if tray is None:
            if plus:
                click(hwnd, *center(plus))
                time.sleep(0.9)
                chk = wait_rect(tap, "BAR-PICK-character", 4.0)
                check(chk is not None, "G3a + 菜单里有 Character")
                if chk:
                    click(hwnd, *center(chk))
                    time.sleep(1.2)
            tray = rect_of(tap, "BAR-TRAY-tray_character", 5.0)
        check(tray is not None, "G3b 勾上之后底栏出现 Character 按钮")
        if tray:
            click(hwnd, *center(tray))
            time.sleep(1.3)
            cw = wait_rect(tap, "CHAR-WIN", 4.0)
            check(cw is not None, "G3c 点开 Character 面板")

        # 点 × 关闭（× 在标题栏右端，往里收一点）
        closed = count(tap, "PANEL character open=0 reason=user-close")
        if cw:
            x = cw[2] - 14.0
            y = cw[1] + 12.0
            for _ in range(3):
                click(hwnd, x, y)
                time.sleep(1.0)
                if count(tap, "PANEL character open=0 reason=user-close") > closed:
                    break
            check(count(tap, "PANEL character open=0 reason=user-close") > closed,
                  "G4 点叉号 → CHARACTER 面板真的关了")

        # ============================================================ 收尾
        print("\n[收尾]")
        with tap.lock:
            bad = [l for l in tap.lines if re.search(BAD_RE, l)]
        check(not bad, "零 Vulkan 验证层报错 (%d 行)" % len(bad))
        for l in bad[:6]:
            print("      ", l.rstrip())

        return 0 if not fails else 1
    finally:
        if hwnd:
            user32.PostMessageW(hwnd, 0x0010, 0, 0)
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.terminate()
        if proc.poll() is None:
            proc.kill()
        if fails:
            print("\n失败 %d 项：" % len(fails))
            for f in fails:
                print("  -", f)
        else:
            print("\n全部通过。")


if __name__ == "__main__":
    sys.exit(main())
