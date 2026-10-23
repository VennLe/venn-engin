"""验收：**有地形的场景里加第三人称角色 → 点 Run → 编辑器不崩、人踩在地表上**。

背景（2026-10-23 用户报障）：

    "引擎编辑器3D视口中，我添加了一个第三人称角色，然后直接点击运行游戏，
     然后引擎编辑器直接崩溃退出了"

根因不在角色，而在**地形高度场的所有权**：

  · `ScenePhysics::attachTerrain` 把 `Heightfield::heights` 指向
    `SceneBinding::terrainHeights`（一个 vector<float>）的堆内存；
  · `EditorApp::buildPhysicsWorld` 里 SceneBinding 是**局部变量**，只
    `std::move(binding.world)` 把世界搬走，function 一返回 vector 就析构；
  · 于是 `m_physics->heightfield()->heights` 是悬垂指针。
  · 它在 P6 之前不爆，是因为**编辑器里没有任何代码读 heightfield**；P6 给
    编辑器 Play 加了 `editorGroundHeight()`（读地形做贴地），进 Play 即崩。

证据（WER + 反汇编）：`Editor.exe+0x1ec64e`，正好落在
`physics::Heightfield::raw()` 的 `heights[col*rows+row]` 那条 `movss` 上；
同一份代码在 `sandbox`（无地形）里不崩，在 `water_bowl`（有地形）里必崩。

修法：`PhysicsWorld::setHeightfield` 改成**拷一份**，世界自持地形数据。

判据（每一条都能单独变红）：

  T0 有地形场景里创建第三人称角色 → `PLAYER-DEFAULT bound`
  T1 点工具栏 Run → `Editor: PLAY in viewport`
  T2 **进程活着**（本用例存在的全部理由；旧版在这一步 0xC0000005）
  T3 地形真的进了物理世界 → `PHYS-TERRAIN attach cols=257 rows=257`
  T4 玩家踩在**地表**上且站得住 —— 连续 3 个 `PLAYER-STATE` 的 z 变化
     < 0.02 m，且 z > 0.3 m（water_bowl 地形在 z≈1.7，掉到 0 就是穿地形了）
  T5 全程 0 条 `[VK_CHECK]` / `invalid state`

用法：
    python tools/verify_editor_play_terrain.py
    python tools/verify_editor_play_terrain.py --scene <其它带地形的场景>
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import find_window, user32                       # noqa: E402
from verify_add_menu import (LogTap as _BaseLogTap, click,
                             kill_stale_editors, open_menu_and_click,
                             wait_rect)
from verify_asset_drag import client_to_screen, force_foreground

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
EXE = os.path.join(BIN, "Editor.exe")
ASSETS = os.path.join(BIN, "assets")
OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify", "editor_play_terrain")

# water_bowl：Terrain(257×257, mpp=2.0, maxh=40) + Pond，physics.enabled=true。
# 选它而不是 terrain_probe.json 的原因：后者的 `physics` 段是空的，
# "地形进不进物理世界"会取决于默认值，判据 T3 就不硬了。
DEFAULT_SCENE = os.path.join(ASSETS, "scenes", "water_bowl.json")
SCENE_PATH = os.path.join(ASSETS, "scenes", "_verify_play_terrain.json")
SIZE = "1600x1000"

RE_BOUND = re.compile(r"PLAYER-DEFAULT bound")
RE_PLAY = re.compile(r"Editor: PLAY in viewport")
RE_TERRAIN = re.compile(r"PHYS-TERRAIN attach cols=(\d+) rows=(\d+)")
RE_RUNBTN = re.compile(
    r"TB-PLAY-run=\((-?[\d.]+),(-?[\d.]+)\)-\((-?[\d.]+),(-?[\d.]+)\)")
RE_PSTATE = re.compile(
    r"PLAYER-STATE '.*' mode=third pos=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\)")


class LogTap(_BaseLogTap):
    def snapshot(self):
        with self.lock:
            return list(self.lines)


def clean_env(**extra):
    env = {k: v for k, v in os.environ.items() if not k.startswith("MYVK_")}
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_EDITOR_SCENE"] = SCENE_PATH
    env.update({k: str(v) for k, v in extra.items()})
    return env


def launch():
    proc = subprocess.Popen([EXE], cwd=BIN, env=clean_env(),
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)

    hwnd = None
    for _ in range(60):
        hwnd = find_window("Venn Editor")
        if hwnd:
            break
        if proc.poll() is not None:
            return proc, None, tap
        time.sleep(0.25)
    if not hwnd:
        return proc, None, tap

    user32.ShowWindow(hwnd, 9)
    time.sleep(1.2)
    tw, th = (int(v) for v in SIZE.split("x"))
    user32.SetWindowPos(hwnd, 0, 0, 0, tw, th, 0x0002 | 0x0004)
    time.sleep(2.5)
    force_foreground(hwnd)
    time.sleep(0.8)
    return proc, hwnd, tap


def center(rect):
    return (int((rect[0] + rect[2]) * 0.5), int((rect[1] + rect[3]) * 0.5))


def last_regex(tap, rx, timeout=8.0):
    """等某个正则**最后一次**匹配（矩形日志每帧刷，要的是最新的）。"""
    end = time.time() + timeout
    got = None
    while time.time() < end:
        for line in tap.snapshot():
            m = rx.search(line)
            if m:
                got = m
        if got:
            return got
        time.sleep(0.1)
    return got


def exit_code(proc):
    if proc.poll() is None:
        return None
    c = proc.returncode
    return "%d (0x%08X)" % (c, c & 0xFFFFFFFF)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scene", default=None, help="改用其它带地形的场景")
    ap.add_argument("--keep", action="store_true", help="保留临时场景文件")
    args = ap.parse_args()

    os.makedirs(OUT_DIR, exist_ok=True)
    results = []

    def check(name, ok, detail=""):
        results.append(bool(ok))
        print("[ %s ] %s  %s" % (" OK " if ok else "FAIL", name, detail))
        return ok

    kill_stale_editors()
    src = args.scene or DEFAULT_SCENE
    shutil.copyfile(src, SCENE_PATH)
    print("场景: %s" % os.path.basename(src))

    proc, hwnd, tap = launch()
    if not hwnd:
        check("T0a 编辑器起得来", False, "exit=%s" % exit_code(proc))
        return 1

    try:
        time.sleep(2.5)
        vp = wait_rect(tap, "VP-RECT", 12.0)
        if not vp:
            check("T0a 拿到视口矩形", False)
            return 2
        mx, my = center(vp)
        user32.SetCursorPos(*client_to_screen(hwnd, mx, my))
        time.sleep(0.4)

        # ---- T0 创建第三人称角色 ----
        ok = open_menu_and_click(tap, hwnd, mx, my, "VP-ADD-Player",
                                 "VP-ADD-ThirdPerson", "Third-Person Player")
        if not check("T0a Shift+A ▸ Player ▸ Third-Person Player", ok):
            return 3
        check("T0b 角色装配完成（PLAYER-DEFAULT bound）",
              last_regex(tap, RE_BOUND, 12.0) is not None)
        time.sleep(1.5)

        # ---- T1 点工具栏 Run ----
        btn = last_regex(tap, RE_RUNBTN, 8.0)
        if btn:
            x0, y0, x1, y1 = (float(btn.group(i)) for i in range(1, 5))
            rx, ry = (x0 + x1) * 0.5, (y0 + y1) * 0.5
            user32.SetCursorPos(*client_to_screen(hwnd, int(rx), int(ry)))
            time.sleep(0.25)
            click(hwnd, rx, ry)
            how = "点 Run 按钮 (%.0f,%.0f)" % (rx, ry)
        else:
            # 兜底：Space 在 isEditing() 时走的也是 togglePlayPause → play()
            user32.PostMessageW(hwnd, 0x0100, 0x20, (0x39 << 16) | 1)
            time.sleep(0.05)
            user32.PostMessageW(hwnd, 0x0101, 0x20, (0x39 << 16) | 0xC0000001)
            how = "Space（没拿到 TB-PLAY-run 矩形）"
        check("T1 进入 Play（%s）" % how,
              last_regex(tap, RE_PLAY, 10.0) is not None)

        # ---- 观察 12 秒：这段就是崩溃窗口 ----
        end = time.time() + 12.0
        crashed = False
        while time.time() < end:
            if proc.poll() is not None:
                crashed = True
                break
            time.sleep(0.15)

        # ---- T2 进程活着（核心判据）----
        check("T2 进 Play 后编辑器没崩（旧版 0xC0000005）",
              not crashed, "exit=%s" % exit_code(proc) if crashed else "alive")
        if crashed:
            lines = tap.snapshot()
            print("  ---- 崩溃前最后 25 行 ----")
            for l in lines[-25:]:
                print("  ", l.rstrip())
            return 4

        lines = tap.snapshot()

        # ---- T3 地形进了物理世界 ----
        tm = last_regex(tap, RE_TERRAIN, 2.0)
        check("T3 地形进了物理世界（PHYS-TERRAIN attach）",
              tm is not None and int(tm.group(1)) >= 2 and int(tm.group(2)) >= 2,
              "cols=%s rows=%s" % (tm.group(1), tm.group(2)) if tm
              else "没有 PHYS-TERRAIN attach")

        # ---- T4 人踩在地表上、站得住 ----
        zs = [float(m.group(3)) for m in
              (RE_PSTATE.search(l) for l in lines) if m]
        tail = zs[-4:] if len(zs) >= 4 else zs
        span = (max(tail) - min(tail)) if len(tail) >= 2 else 999.0
        check("T4 玩家踩在地表上且站得住（z 稳定、没往下掉）",
              len(tail) >= 3 and span < 0.02 and min(tail) > 0.3,
              "z=%s span=%.4f" % (["%.3f" % z for z in tail], span))

        # ---- T5 无 Vulkan 错误 ----
        bad = [l for l in lines
               if ("VK_CHECK" in l) or ("invalid state" in l)]
        check("T5 全程无 Vulkan 错误", len(bad) == 0, "%d 条" % len(bad))
    finally:
        if proc.poll() is None:
            user32.PostMessageW(hwnd, 0x0010, 0, 0)
            time.sleep(0.8)
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    proc.kill()
        if not args.keep:
            try:
                os.remove(SCENE_PATH)
            except OSError:
                pass

    passed = sum(1 for r in results if r)
    print("\n%d/%d PASS" % (passed, len(results)))
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
