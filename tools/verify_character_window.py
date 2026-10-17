"""端到端验证：角色详情窗口（UE5 Persona 风格 MVP）。

用户需求（原文要点）：
    "当我创建角色时（包括第一人称和第三人称），点击后，打开一个角色详情
     窗口 …… 上面有一个角色模型预览视图，和正常的3d视图中的操作是差不多的
     …… 左侧属性栏中添加绑定模型，骨骼，还有动画，而且都可以预览和编辑。"

断言：
  1. Shift+A ▸ Player ▸ Third-Person Player 创建成功后，自动打开角色窗口
     （日志 `CharacterWindow: opened`）；
  2. 角色窗口打开后，独立预览离屏链被创建（日志 `Preview render size ->`）；
  3. 编辑器持续运行无崩溃 / 无 Vulkan 验证层报错；
  4. 截图保存角色窗口（供人工核对预览视口 + 左侧属性栏布局）。

用法：
    python tools/verify_character_window.py
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_client, find_window, user32        # noqa: E402
from verify_add_menu import (LogTap, click, kill_stale_editors,
                             open_menu_and_click, wait_rect)   # noqa: E402
from verify_asset_drag import client_to_screen, force_foreground      # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify")


def center(rect):
    return (int((rect[0] + rect[2]) * 0.5), int((rect[1] + rect[3]) * 0.5))


# 鼠标左键按下 / 抬起（drag 用）
_MOUSEEVENTF_LEFTDOWN = 0x0002
_MOUSEEVENTF_LEFTUP = 0x0004


def drag_client(hwnd, x0, y0, x1, y1, steps=10, hold=0.05):
    """在**客户区坐标**上按住左键从 (x0,y0) 拖到 (x1,y1)。

    用真鼠标事件（SetCursorPos + mouse_event）而不是直投 WM_MOUSEMOVE：
    ImGui 的窗口 resize 抓的是光标位置，只发消息不给光标移动会被当成
    "没动"。坐标一律走 client_to_screen 换算，避免标题栏高度差。
    """
    sx, sy = client_to_screen(hwnd, int(x0), int(y0))
    user32.SetCursorPos(sx, sy)
    time.sleep(0.25)
    user32.mouse_event(_MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.15)
    for i in range(1, steps + 1):
        px = x0 + (x1 - x0) * i / steps
        py = y0 + (y1 - y0) * i / steps
        cx, cy = client_to_screen(hwnd, int(px), int(py))
        user32.SetCursorPos(cx, cy)
        time.sleep(hold)
    user32.mouse_event(_MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
    time.sleep(0.35)


def wait_line(tap, needle, timeout=8.0):
    end = time.time() + timeout
    while time.time() < end:
        if tap.has_line(needle):
            return True
        time.sleep(0.15)
    return False


def wait_clip_rect(tap, timeout=5.0):
    """等待第一个 CHAR-CLIP-<名字>=(...) 矩形日志，返回 (x0,y0,x1,y1)。"""
    rx = re.compile(r"CHAR-CLIP-[^=]+=\(([-\d.]+),([-\d.]+)\)-"
                    r"\(([-\d.]+),([-\d.]+)\)")
    end = time.time() + timeout
    while time.time() < end:
        with tap.lock:
            for line in reversed(tap.lines):
                m = rx.search(line)
                if m:
                    return tuple(float(m.group(i)) for i in range(1, 5))
        time.sleep(0.15)
    return None


def wait_valid_rect(tap, tag, timeout=5.0):
    """等待一个**非退化**的矩形（未激活的 tab 会打 (0,0)-(0,0)）。"""
    end = time.time() + timeout
    while time.time() < end:
        r = tap.last_rect(tag)
        if r and (r[2] - r[0]) > 2.0 and (r[3] - r[1]) > 2.0:
            return r
        time.sleep(0.15)
    return None


def grep_lines(tap, pat):
    rx = re.compile(pat)
    with tap.lock:
        return [l for l in tap.lines if rx.search(l)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--size", default="1600x900")
    ap.add_argument("--model", default="assets/models/anime_girl.fbx",
                    help="自动化绑定的模型（MYVK_CHARACTER_MODEL）")
    ap.add_argument("--no-model", action="store_true",
                    help="不自动绑定模型（只测窗口打开）")
    args = ap.parse_args()

    os.makedirs(OUT_DIR, exist_ok=True)
    fails = []

    def check(name, ok, detail=""):
        print("%s  %s%s" % ("PASS" if ok else "FAIL", name,
                            ("  (%s)" % detail) if detail else ""))
        if not ok:
            fails.append(name)

    kill_stale_editors()
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    if not args.no_model and args.model:
        env["MYVK_CHARACTER_MODEL"] = args.model

    proc = subprocess.Popen([args.exe], cwd=os.path.dirname(args.exe), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)

    hwnd = None
    for _ in range(40):
        hwnd = find_window("Venn Editor")
        if hwnd:
            break
        time.sleep(0.25)
    if not hwnd:
        check("编辑器启动", False)
        proc.kill()
        return 2

    user32.ShowWindow(hwnd, 9)
    time.sleep(1.5)
    tw, th = (int(v) for v in args.size.lower().split("x"))
    user32.SetWindowPos(hwnd, 0, 0, 0, tw, th, 0x0002 | 0x0004)
    time.sleep(2.5)
    force_foreground(hwnd)
    time.sleep(0.8)

    try:
        # ---- 1) 拿到视口矩形 ----
        vp = wait_rect(tap, "VP-RECT", 8.0)
        if not vp:
            check("拿到 VP-RECT", False)
            return 3
        mx, my = center(vp)
        sx, sy = client_to_screen(hwnd, mx, my)
        user32.SetCursorPos(sx, sy)
        time.sleep(0.5)

        # ---- 2) 创建第三人称角色（应自动打开角色窗口）----
        ok = open_menu_and_click(tap, hwnd, mx, my, "VP-ADD-Player",
                                 "VP-ADD-ThirdPerson",
                                 "Third-Person Player")
        check("Shift+A ▸ Player ▸ Third-Person Player 创建成功", ok)
        if not ok:
            return 4

        # ---- 3) 角色窗口自动打开 ----
        opened = wait_line(tap, "CharacterWindow: opened")
        check("创建角色后自动打开角色详情窗口", opened,
              grep_lines(tap, "CharacterWindow")[-1] if grep_lines(
                  tap, "CharacterWindow") else "没等到")

        # ---- 4) 模型自动绑定（自动化钩子）----
        if not args.no_model:
            bound = wait_line(tap, "auto-bound model", 8.0)
            check("模型自动绑定（MYVK_CHARACTER_MODEL）", bound,
                  grep_lines(tap, "auto-bound")[-1] if grep_lines(
                      tap, "auto-bound") else "没等到")

        # ---- 5) 预览离屏链被创建 + 预览场景重建 ----
        preview_ok = wait_line(tap, "Preview render size", 10.0)
        check("独立预览离屏链创建（第二套渲染目标）", preview_ok,
              grep_lines(tap, "Preview render size")[-1] if grep_lines(
                  tap, "Preview render size") else "没等到")
        if not args.no_model:
            rebuilt = wait_line(tap, "preview scene rebuilt", 6.0)
            check("预览场景随模型重建", rebuilt,
                  grep_lines(tap, "preview scene rebuilt")[-1] if grep_lines(
                      tap, "preview scene rebuilt") else "没等到")

        # ---- 6) 诊断输出：模型包围盒 + submesh（预览取景排查用）----
        for l in grep_lines(tap, r"model bounds|sub\[\d+\]"):
            print("  LOG:", l)

        # ---- 7) 动画播放：切到 Animation 标签，点第一个 clip ----
        time.sleep(0.4)
        tab_anim = wait_valid_rect(tap, "CHAR-TAB-Animation", 5.0)
        check("拿到 Animation 标签矩形", tab_anim is not None)
        if tab_anim:
            ax, ay = center(tab_anim)
            # click() 接收**客户区**坐标并自行换算屏幕坐标
            click(hwnd, ax, ay)
            time.sleep(0.3)
            # 第一次点击只把焦点给 ImGui 窗口，第二次才真正切标签
            click(hwnd, ax, ay)
            time.sleep(0.4)
            clip = wait_clip_rect(tap, 5.0)
            check("拿到动画条目矩形（CHAR-CLIP）", clip is not None)
            if clip:
                cx2, cy2 = center(clip)
                click(hwnd, cx2, cy2)   # 窗口已聚焦，一次点击即可选中
                played = wait_line(tap, "play clip", 4.0)
                check("点击动画条目触发播放", played,
                      grep_lines(tap, "play clip")[-1] if grep_lines(
                          tap, "play clip") else "没等到")

        # ---- 8) 默认尺寸够大（2026-10-17 用户报"窗口太小"）----
        #
        # ⚠ 只看截图是靠不住的：第 9 步的拖动会把窗口改小，所以**截图必须
        #   拍在拖动之前**，否则"窗口太小"其实是测试自己拖出来的假象。
        #   真正的判据是 ImGui 自己报的 CHAR-WIN 矩形。
        #
        # 目标值（代码里 defW/defH，客户区 1578×844 时）：
        #   0.78 * 1578 = 1231，0.82 * 844 = 692，下限 980×620。
        # 这里只断言"明显大于旧的 760×560"，不去咬具体数值 —— 脚本会在
        # 不同显示器上跑，咬死 1231 就是给自己埋一个换机器才红的坑。
        win_rect = wait_valid_rect(tap, "CHAR-WIN", 5.0)
        check("拿到角色窗口矩形（CHAR-WIN）", win_rect is not None)
        if win_rect:
            ww = win_rect[2] - win_rect[0]
            wh = win_rect[3] - win_rect[1]
            check("角色窗口默认尺寸够大（>旧版 760×560）",
                  ww > 800 and wh > 560,
                  "%.0fx%.0f" % (ww, wh))

        # ---- 9) 截图（拖动之前，人工核对预览视口 + 三栏布局）----
        time.sleep(1.2)
        img = capture_client(hwnd)[0]
        png = os.path.join(OUT_DIR, "character_window.png")
        img.save(png)
        check("角色窗口截图已保存", os.path.exists(png), png)

        # ---- 10) 拖住窗口右下角改大小（2026-10-17 用户报的崩溃）----
        #
        # 症状：用鼠标拖角色窗口的边框改大小 → 程序直接退出。
        # 根因：预览离屏链在**命令录制过程中**重建，RemoveTexture 把旧的
        # VkDescriptorSet 释放掉，而本帧的 ImGui 绘制数据早就把那个句柄记进
        # draw list 了 → vkCmdBindDescriptorSets 绑一个已释放的 set
        # （VUID-vkCmdBindDescriptorSets-pDescriptorSets）。
        # 修法见 Renderer::applyPendingResizes：重建推迟到 UI 构建之前。
        #
        # 断言分三层：① 拖完进程还活着；② 日志里没有那条 VUID；
        #   ③ 尺寸下限（SetNextWindowSizeConstraints 880×560）真的兜住了 ——
        #   这一条顺带证明"用户把窗口拖到很小也不会把预览视口挤没"。
        # ⚠ 只断言①不够 —— 关掉验证层时同样的 UAF 可能"侥幸不崩"。
        resized = False
        if win_rect:
            # 每次拖动都从**当前**右下角出发：用同一个旧坐标连拖三次，
            # 光标会先跳回原处再走，ImGui 的 resize 抓取会中途丢失。
            for (fx, fy) in [(-380, -260), (60, 40), (-120, -60), (-300, -200)]:
                time.sleep(0.25)   # 等上一轮的尺寸落进日志（logRect 有去抖）
                cur = wait_valid_rect(tap, "CHAR-WIN", 1.5) or win_rect
                x0 = cur[2] - 3
                y0 = cur[3] - 3
                drag_client(hwnd, x0, y0, x0 + fx, y0 + fy)
                if proc.poll() is not None:
                    break
            resized = proc.poll() is None
            check("拖动窗口边框改大小后进程仍存活", resized,
                  "崩溃时预览链正在重建" if not resized else "ok")

            time.sleep(0.5)
            final_rect = wait_valid_rect(tap, "CHAR-WIN", 2.0)
            if resized and final_rect:
                fw = final_rect[2] - final_rect[0]
                fh = final_rect[3] - final_rect[1]
                check("拖到很小也被下限兜住（>=880×560）",
                      fw >= 878 and fh >= 558, "%.0fx%.0f" % (fw, fh))
            time.sleep(0.6)
            img = capture_client(hwnd)[0]
            png2 = os.path.join(OUT_DIR, "character_window_resized.png")
            img.save(png2)

        # ---- 11) 无验证层报错 ----
        errors = grep_lines(tap, r"(ERROR|VALIDATION|VUID)")
        check("无 Vulkan 验证层报错", len(errors) == 0,
              "; ".join(errors[:3]) if errors else "clean")

        # ---- 12) 再跑几秒确认稳定 ----
        time.sleep(2.0)
        alive = proc.poll() is None
        check("编辑器持续运行无崩溃", alive)

    finally:
        if hwnd:
            user32.PostMessageW(hwnd, 0x0010, 0, 0)
        time.sleep(0.6)
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                proc.kill()

    return report(fails)


def report(fails):
    print()
    if fails:
        print("RESULT: FAIL")
        for f in fails:
            print("  -", f)
        return 6
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    rc = main()
    sys.exit(rc if rc is not None else 0)
