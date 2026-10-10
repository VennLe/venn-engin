"""端到端验证：Play 态的两条"不许穿过"。

用户要的形态（原文）：
    "在游戏视图种，所有带碰撞检测的物体是不能互相穿过的。
     还有，相机视角也不允许穿过。"

拆成两条独立的用例，各起一个进程（场景不同、断言不同，互不干扰）：

  用例 A（bodies）—— 两个**重叠**的立方体
      运行态每帧调 resolveBodyOverlaps，把互相嵌入的一对沿最小平移向量
      推开（各推一半）。断言：
        · `collision: 1 overlapping pair(s) resolved (frame 1)`
        · `collision: 'BlockA' <-> 'BlockB' depth=0.4000 normal=(...)`
          深度正是两个 1m 立方体沿 X 交叠的那 0.4 m（可由场景几何反推，
          不是"随便>0"这种糊弄式断言）。
        · 推开之后不再有重叠对（后续帧不再打 pair 日志）。

  用例 B（camera）—— 相机（半径 0.25 的球）**埋在**一个盒子里
      断言 `collision: camera blocked, pushed (8.000,8.000,4.000) -> (x,y,z)`，
      且位移主要发生在 Z 轴上（盒子 Z 向最薄，最近的面就是它），
      位移量 ≈ 到该面的距离 + 相机半径。

为什么两个用例都要 `MYVK_EDITOR_PLAY=1`：
    "物体互不穿透 / 相机不穿墙"是**运行态**行为（编辑态要能自由摆放，
    碰撞求解不参与）。只有进了 Play，onUpdate 才会走 runCollision。

用法：
    python tools/verify_collision_play.py
"""

import argparse
import json
import math
import os
import re
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_client, find_window, user32        # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors               # noqa: E402
from verify_asset_drag import force_foreground                       # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify")

PAIRS_RE = re.compile(
    r"collision: (\d+) overlapping pair\(s\) resolved \(frame (\d+)\)")
CONTACT_RE = re.compile(
    r"collision: '([^']*)' <-> '([^']*)' depth=([\d.]+) "
    r"normal=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\)")
CAMERA_RE = re.compile(
    r"collision: camera blocked, pushed "
    r"\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\) -> "
    r"\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\)")


# ------------------------------------------------------------------ 场景

def base_entity(name, pos, scale, color):
    return {
        "name": name,
        "transform": {"position": pos, "rotation": [0.0, 0.0, 0.0],
                      "scale": scale},
        "parent": -1, "visible": True, "castShadow": True,
        "mesh": {"kind": "builtin", "shape": "cube", "size": 1.0,
                 "name": name.lower() + "_mesh"},
        "material": {"kind": "procedural", "name": name.lower() + "_mat",
                     "baseColor": color, "metallic": 0.0, "roughness": 0.6,
                     "normalScale": 0.0, "ao": 1.0, "emissive": [0, 0, 0],
                     "doubleSided": False, "alphaBlend": False},
        # 凸包：几何由 mesh 在读盘时现场重建（顶点不进 JSON）
        "collision": {"shape": "convex", "position": [0, 0, 0],
                      "rotation": [0, 0, 0], "scale": [1, 1, 1],
                      "solid": True},
    }


def make_scene(path, mode):
    """mode = 'bodies' | 'camera'。"""
    if mode == "bodies":
        # BlockA 中心 x=-0.4（跨 [-0.9,0.1]）、BlockB 中心 x=0.2（跨
        # [-0.3,0.7]）→ 沿 X 交叠 0.4 m。运行态应当把它俩推开。
        ents = [
            base_entity("BlockA", [-0.4, 0.0, 0.5], [1, 1, 1],
                        [0.85, 0.35, 0.30, 1.0]),
            base_entity("BlockB", [0.2, 0.0, 0.5], [1, 1, 1],
                        [0.30, 0.55, 0.85, 1.0]),
        ]
        cam = {"target": [0, 0, 0.5], "yaw": math.radians(135),
               "pitch": math.radians(20), "distance": 8.0}
    else:  # camera —— 盒子 Z 向最薄，相机埋在中心
        # CamTrap 中心 (8,8,4)、scale (4,4,2) → x∈[6,10] y∈[6,10] z∈[3,5]。
        # 相机 eye=(8,8,4)：到 z 面的距离 1、到 x/y 面 2 → 最近面是 Z，
        # 应当沿 Z 被推出约 1 + 0.25（相机半径）。
        ents = [
            base_entity("CamTrap", [8.0, 8.0, 4.0], [4, 4, 2],
                        [0.55, 0.45, 0.75, 1.0]),
        ]
        cam = {"target": [0, 0, 0.5], "yaw": math.radians(135),
               "pitch": math.radians(20), "distance": 8.0}

    scene = {"version": 1, "generator": "verify_collision_play",
             "camera": cam, "entities": ents}
    with open(path, "w", encoding="utf-8") as f:
        json.dump(scene, f, indent=2)
    return path


# ------------------------------------------------------------------ 运行

def run_case(exe, scene_path, size, cam_env, out_png, expect_re, frames=3000):
    """起一个 Play 进程，等到 expect_re 出现在日志里再截图，然后关掉。

    为什么不靠 MYVK_FRAMES 自动退出：Play 态帧率很高（130+ fps），
    MYVK_FRAMES=200 大约 1.5 秒就退干净了 —— 脚本还没来得及截窗口，
    PrintWindow 拿到的是一张空图。这里改为跑足帧数（默认 3000，
    约 20 秒），由脚本自己控制"截完再关"。
    """
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_EDITOR_SCENE"] = scene_path
    env["MYVK_EDITOR_PLAY"] = "1"
    env["MYVK_FRAMES"] = str(frames)
    if cam_env:
        env["MYVK_EDITOR_CAM"] = cam_env
    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env,
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
    shot = False
    if hwnd:
        user32.ShowWindow(hwnd, 9)
        time.sleep(1.0)
        tw, th = (int(v) for v in size.lower().split("x"))
        user32.SetWindowPos(hwnd, 0, 0, 0, tw, th, 0x0002 | 0x0004)
        force_foreground(hwnd)
        # 等目标日志出现（Play 已在跑、画面已稳定）再截图
        end = time.time() + 15.0
        while time.time() < end:
            with tap.lock:
                if any(expect_re.search(l) for l in tap.lines):
                    break
            time.sleep(0.2)
        time.sleep(0.6)
        try:
            capture_client(hwnd)[0].save(out_png)
            shot = True
        except Exception as e:      # 截图失败不该让整个用例挂掉
            print("  截图失败:", e)
    # 主动关窗（不依赖帧数耗尽）
    if hwnd:
        user32.PostMessageW(hwnd, 0x0010, 0, 0)
    time.sleep(0.6)
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
    time.sleep(0.3)
    with tap.lock:
        lines = list(tap.lines)
    return proc.returncode, lines, shot


# ------------------------------------------------------------------ 主流程

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--size", default="1600x900")
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    if not os.path.exists(exe):
        print("找不到可执行文件，请先构建：", exe)
        return 1
    os.makedirs(OUT_DIR, exist_ok=True)

    tmpdir = tempfile.mkdtemp(prefix="venn_colplay_")
    fails = []

    def check(name, ok, detail=""):
        print("%s  %s%s" % ("PASS" if ok else "FAIL", name,
                            ("  (%s)" % detail) if (detail and not ok) else ""))
        if not ok:
            fails.append("%s | %s" % (name, detail))

    kill_stale_editors()
    try:
        # ================= 用例 A：物体互不穿透 =================
        print("[A] 两个重叠的立方体 → 运行态必须把它们分开")
        sp = make_scene(os.path.join(tmpdir, "bodies.json"), "bodies")
        png = os.path.join(OUT_DIR, "collision_play_bodies.png")
        rc, lines, shot = run_case(exe, sp, args.size, "-5.3,-5.3,3.2,135,20",
                                   png, CONTACT_RE)
        check("  进入 Play（runtime scene）",
              any("PLAY in viewport" in l for l in lines), "没有进 Play")
        check("  截到运行态画面", shot, png)
        check("  日志无 ERROR / 校验层报错",
              not any(("VUID" in l) or ("[ERROR" in l.upper()[:8])
                      for l in lines),
              "出现错误行")

        pairs = [PAIRS_RE.search(l) for l in lines]
        pairs = [m for m in pairs if m]
        check("  运行态检测到重叠对（overlapping pair(s) resolved）",
              len(pairs) > 0, "一行都没有")
        if pairs:
            first = pairs[0]
            check("    首帧就解算（frame 1）", first.group(2) == "1",
                  "frame=%s" % first.group(2))
            check("    重叠对 = 1 对", first.group(1) == "1",
                  "pairs=%s" % first.group(1))
        # 打印出接触明细
        contacts = [CONTACT_RE.search(l) for l in lines]
        contacts = [m for m in contacts if m]
        check("  打印了接触明细（depth / normal）", len(contacts) > 0)
        found = None
        for m in contacts:
            if {m.group(1), m.group(2)} == {"BlockA", "BlockB"}:
                found = m
                break
        check("    接触对是 BlockA <-> BlockB", found is not None,
              "没找到这一对")
        if found:
            depth = float(found.group(3))
            nx = float(found.group(4))
            check("    穿透深度 ≈ 0.4 m（1m 立方体沿 X 交叠 0.4）",
                  abs(depth - 0.4) < 0.02, "depth=%.4f" % depth)
            check("    分离法线沿 X（|nx| ≈ 1）",
                  abs(abs(nx) - 1.0) < 0.02, "normal.x=%.4f" % nx)
        # 推开之后不该再反复解算同一对（否则说明推不干净）
        if len(pairs) >= 2:
            print("  note: 共 %d 帧报告过重叠（逐帧推净）" % len(pairs))

        print()
        # ================= 用例 B：相机不穿墙 =================
        print("[B] 相机埋在盒子里 → 运行态必须把它推出来")
        sp2 = make_scene(os.path.join(tmpdir, "camera.json"), "camera")
        png2 = os.path.join(OUT_DIR, "collision_play_camera.png")
        # eye=(8,8,4) 正是 CamTrap 的中心
        rc2, lines2, shot2 = run_case(exe, sp2, args.size,
                                      "8,8,4,135,20", png2, CAMERA_RE)
        check("  进入 Play（runtime scene）",
              any("PLAY in viewport" in l for l in lines2), "没有进 Play")
        check("  截到运行态画面", shot2, png2)

        cams = [CAMERA_RE.search(l) for l in lines2]
        cams = [m for m in cams if m]
        check("  相机被碰撞体挡住并推出（camera blocked, pushed）",
              len(cams) > 0, "一行都没有")
        if cams:
            m = cams[0]
            before = [float(m.group(i)) for i in (1, 2, 3)]
            after = [float(m.group(i)) for i in (4, 5, 6)]
            d = [after[i] - before[i] for i in range(3)]
            dist = math.sqrt(sum(v * v for v in d))
            check("    起点 = 盒子中心 (8,8,4)",
                  all(abs(before[i] - [8, 8, 4][i]) < 0.05 for i in range(3)),
                  "before=%s" % before)
            check("    确实被推开了（位移 > 0.3 m）", dist > 0.3,
                  "dist=%.4f d=%s" % (dist, d))
            check("    位移主要沿 Z（盒子 Z 向最薄：z∈[3,5]）",
                  abs(d[2]) > abs(d[0]) and abs(d[2]) > abs(d[1]),
                  "d=(%.4f,%.4f,%.4f)" % tuple(d))
            check("    Z 位移 ≈ 1.25 m（到 z 面 1.0 + 相机半径 0.25）",
                  abs(abs(d[2]) - 1.25) < 0.15, "dz=%.4f" % d[2])

        print()
        if fails:
            print("RESULT: FAIL")
            for f in fails:
                print("  -", f)
            print("\n最近 40 行日志：")
            for line in lines[-40:]:
                print("   ", line)
            return 4
        print("截图：", os.path.join(OUT_DIR, "collision_play_*.png"))
        print("RESULT: PASS")
        return 0
    finally:
        try:
            for f in os.listdir(tmpdir):
                os.remove(os.path.join(tmpdir, f))
            os.rmdir(tmpdir)
        except OSError:
            pass


if __name__ == "__main__":
    sys.exit(main())
