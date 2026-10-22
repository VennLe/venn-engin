"""验收：**打开角色预览不能拆掉主场景的地形**（编辑器闪退修复）。

用户报的现场（原文片段）：

    [ERROR] [validation] vkCmdBeginRenderPass(): was called in VkCommandBuffer
    ... which is now in an invalid state ... - VkBuffer 0x... was destroyed
    - VkSampler 0x... was destroyed ... - VkBuffer 0x... was destroyed
    [VK_CHECK] Vulkan error -4 at engine/rhi/CommandPool.cpp:56
    [ERROR] Fatal: Vulkan call failed
    "在创建第三人称角色时直接程序崩溃退出报错了"

根因（实测复现 + 打点确认）：

  `Renderer::uploadSceneState()` 末尾无条件调 `syncTerrains(scene)` 与
  `syncWaters(scene)`。而它有三个调用方，其中**离屏目标**（角色预览 /
  额外视角 / 反射探针的面）是在 `recordFrame` **录制途中**被调用的
  （`recordOffscreenTarget`），传进去的是**另一个场景**。

  预览场景里没有地形实体 → `items` 为空、与主场景的 `m_terrainRequested`
  对不上 → `same == false` → 走进"全量重建"：`clearInstances()` 把主场景
  地形的 VBO/IBO（实测 2457600 B + 3065856 B）和 8 张层贴图的 sampler
  全销毁 —— 而这些句柄**已经绑在那条正在录制的命令缓冲上**，整条 CB
  当场失效，之后每个命令都报同一句，最终 `vkQueueSubmit` 返回
  -4（VK_ERROR_DEVICE_LOST），`Fatal: Vulkan call failed` 直接退出。

  日志的顺序陷阱：设备丢了之后，地形重试机制（`kTerrainRetryFrames`）
  会再 load 一次地形，于是 8 条 `TERRAIN-LAYER-TEX-FAIL ... (Vulkan call
  failed)` 排在最后 —— 看着像"地形贴图加载失败导致崩溃"，其实是**余波**。
  真正的原因要看**第一条** `invalid state`：里面点了名的对象是地形的
  buffer 与 layer sampler。

判据（每一条都对应"修复前必红"）：

  P0 场景带地形读进来（TERRAIN-INSTANCE 出现）
  P1 Shift+A ▸ Player ▸ Third-Person Player 点击成功
  P2 预览目标真的建出来了（`Preview render size`）—— 否则这条用例是空跑
  P3 0 条 `invalid state`（命令缓冲没有被中途作废）
  P4 0 条 `[VK_CHECK]`（设备没丢）
  P5 地形只建了 1 次（没被预览拆掉重建 / 没走降级重试）
  P6 0 条 `TERRAIN-LAYER-TEX-FAIL`
  P7 点完之后编辑器还活着（4 s 后进程在）

用法：
    python tools/verify_preview_terrain.py
    python tools/verify_preview_terrain.py --keep      # 保留截图
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_client, find_window, user32          # noqa: E402
from safe_fs import safe_remove                                        # noqa: E402
from verify_add_menu import (LogTap as _BaseLogTap, kill_stale_editors,
                             open_menu_and_click, wait_rect)
from verify_asset_drag import client_to_screen, force_foreground

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
EXE = os.path.join(BIN, "Editor.exe")
OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify", "preview_terrain")

SCENE_NAME = "_p247_preview_terrain.json"
SCENE_PATH = os.path.join(BIN, "assets", "scenes", SCENE_NAME)
TERRAIN = "terrain/hills/terrain.vterrain"     # 仓库自带：257x257 / 4 层材质
TITLE_MATCH = "Venn Editor"
SIZE = "1600x1000"

RE_PREVIEW = re.compile(r"Preview render size -> (\d+)x(\d+)")
RE_INSTANCE = re.compile(r"TERRAIN-INSTANCE path=(\S+) chunks=(\d+)")


# ---------------------------------------------------------------- 场景

def make_scene():
    """地形 + 出生点。地形是关键：没有它这条用例抓不到任何东西
    （预览场景和主场景都是空的 → items 与 requested 都是空 → same）。"""
    scene = {
        "version": 1,
        "generator": "verify_preview_terrain.py",
        "camera": {"target": [0.0, 0.0, 1.2], "yaw": 3.141592653589793,
                   "pitch": 0.30, "distance": 12.0, "fov": 55.0},
        "light": {"direction": [0.42, 0.72, -0.55], "color": [1.0, 0.96, 0.9],
                  "intensity": 3.2, "castsShadow": True,
                  "ambientScale": 1.0},
        "physics": {"enabled": True, "groundPlane": True,
                    "fixedTimeStep": 0.0166666675, "maxSubSteps": 4,
                    "allowSleep": True},
        "entities": [
            {
                "name": "Terrain",
                "transform": {"position": [0.0, 0.0, 0.0],
                              "rotation": [0.0, 0.0, 0.0],
                              "scale": [1.0, 1.0, 1.0]},
                "parent": -1, "visible": True, "castShadow": False,
                "locked": True,
                "terrain": {"source": TERRAIN, "visible": True,
                            "heightScale": 1.0, "layers": ["", "", "", ""]},
            },
            {
                "name": "GameStart",
                "transform": {"position": [0.0, 0.0, 0.0],
                              "rotation": [0.0, 0.0, 0.0],
                              "scale": [1.0, 1.0, 1.0]},
                "parent": -1, "visible": True, "castShadow": False,
                "gameStart": {"yaw": 0.0, "enabled": True},
            },
        ],
    }
    os.makedirs(os.path.dirname(SCENE_PATH), exist_ok=True)
    with open(SCENE_PATH, "w", encoding="utf-8") as f:
        json.dump(scene, f, ensure_ascii=False, indent=2)


# ---------------------------------------------------------------- 日志泵

class LogTap(_BaseLogTap):
    """复用 `verify_add_menu.LogTap`（它已经处理了常驻排空 + `last_rect` /
    `has_line` 的 tag 前缀匹配），这里只补两个本用例要用的读取辅助。

    ⚠ 常驻排空不是可选项：管道缓冲（约 64 KB）写满后编辑器会**阻塞在写
    日志上**，表现为"窗口卡死、脚本等到超时"。这条用例一次要出几千行
    矩形日志（`MYVK_LOG_RECTS=1`），比别的用例更容易撞上。
    """

    def snapshot(self):
        with self.lock:
            return list(self.lines)

    def count(self, needle):
        with self.lock:
            return sum(1 for l in self.lines if needle in l)


def clean_env(**extra):
    env = {k: v for k, v in os.environ.items() if not k.startswith("MYVK_")}
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_EDITOR_SCENE"] = SCENE_PATH
    env.update({k: str(v) for k, v in extra.items()})
    return env


def launch(extra_env):
    proc = subprocess.Popen([EXE], cwd=BIN, env=clean_env(**extra_env),
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)

    hwnd = None
    for _ in range(60):
        hwnd = find_window(TITLE_MATCH)
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


def close(proc, hwnd):
    if hwnd:
        user32.PostMessageW(hwnd, 0x0010, 0, 0)   # WM_CLOSE
    time.sleep(0.8)
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=8)
        except subprocess.TimeoutExpired:
            proc.kill()


# ---------------------------------------------------------------- 主流程

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true", help="保留截图与场景文件")
    args = ap.parse_args()

    os.makedirs(OUT_DIR, exist_ok=True)
    results = []

    def check(name, ok, detail=""):
        results.append(bool(ok))
        print("[ %s ] %s  %s" % (" OK " if ok else "FAIL", name, detail))
        return ok

    kill_stale_editors()
    make_scene()

    proc, hwnd, tap = launch({})
    if not hwnd:
        check("P-gen 编辑器起得来", False, "exit=%s" % proc.poll())
        close(proc, None)
        return 1

    try:
        time.sleep(2.0)

        # ---- P0 地形真的建起来了 ----
        inst = [RE_INSTANCE.search(l) for l in tap.snapshot()]
        inst = [m for m in inst if m]
        check("P0 场景带地形读进来（TERRAIN-INSTANCE 出现）", bool(inst),
              "path=%s chunks=%s" % (inst[0].group(1), inst[0].group(2))
              if inst else "没有 TERRAIN-INSTANCE")

        # ---- P1 点菜单 ----
        vp = wait_rect(tap, "VP-RECT", 10.0)
        ok = False
        if vp:
            mx = int((vp[0] + vp[2]) * 0.5)
            my = int((vp[1] + vp[3]) * 0.5)
            user32.SetCursorPos(*client_to_screen(hwnd, mx, my))
            time.sleep(0.4)
            ok = open_menu_and_click(tap, hwnd, mx, my, "VP-ADD-Player",
                                     "VP-ADD-ThirdPerson", "Third-Person Player")
        else:
            print("  拿不到 VP-RECT")
        if not check("P1 Shift+A ▸ Player ▸ Third-Person Player 点击成功", ok):
            # 点不动就没法判定 —— 打印现场便于排查，但**不许当成通过**
            dbg = [l for l in tap.snapshot() if "VP-ADD" in l][-6:]
            print("  VP-ADD 日志:", dbg if dbg else "(none)")
            return 4

        # ---- P2~P7 等预览目标建出来 + 观察若干帧 ----
        end = time.time() + 8.0
        prev = None
        while time.time() < end:
            m = [RE_PREVIEW.search(l) for l in tap.snapshot()]
            m = [x for x in m if x]
            if m:
                prev = m[-1]
                break
            time.sleep(0.2)
        check("P2 预览目标真的建出来了（不是空跑）", prev is not None,
              "%sx%s (offscreen)" % (prev.group(1), prev.group(2))
              if prev else "没有 'Preview render size'")

        time.sleep(4.0)     # 让预览持续渲染一阵（崩溃发生在预览建好之后几帧内）

        lines = tap.snapshot()
        bad_cb = [l for l in lines if "invalid state" in l]
        vkchk = [l for l in lines if "[VK_CHECK]" in l]
        texfail = [l for l in lines if "TERRAIN-LAYER-TEX-FAIL" in l]
        n_inst = len([l for l in lines if "TERRAIN-INSTANCE path=" in l])
        n_load = len([l for l in lines if "TERRAIN-LOAD path=" in l])

        check("P3 命令缓冲没有被中途作废（0 条 invalid state）", not bad_cb,
              "第一条: " + bad_cb[0][:110] if bad_cb else "count=0")
        check("P4 设备没丢（0 条 VK_CHECK）", not vkchk,
              vkchk[0][:110] if vkchk else "count=0")
        check("P5 地形只建了 1 次（没被预览拆掉重建）",
              n_inst == 1 and n_load == 1,
              "TERRAIN-INSTANCE=%d TERRAIN-LOAD=%d" % (n_inst, n_load))
        check("P6 层贴图没有加载失败", not texfail,
              "count=%d" % len(texfail))

        alive = proc.poll() is None
        check("P7 点完之后编辑器还活着", alive, "exit=%s" % proc.poll())

        png = os.path.join(OUT_DIR, "p247_after_click.png")
        try:
            img = capture_client(hwnd)[0]
            img.save(png)
            print("  截图:", png, img.size)
        except Exception as exc:            # noqa: BLE001
            print("  截图失败:", exc)

        with open(os.path.join(OUT_DIR, "p247_full.log"), "w",
                  encoding="utf-8") as f:
            f.write("\n".join(lines))
        print("  完整日志:", os.path.join(OUT_DIR, "p247_full.log"),
              "共 %d 行" % len(lines))
    finally:
        close(proc, hwnd)
        if not args.keep:
            safe_remove(SCENE_PATH)

    npass = sum(1 for r in results if r)
    print("\n结果: %s  (%d 通过 / %d 失败)"
          % ("PASS" if npass == len(results) else "FAIL",
             npass, len(results) - npass))
    return 0 if npass == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
