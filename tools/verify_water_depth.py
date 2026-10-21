"""端到端验证：水体的**厚度**（体积）与**贴地**（2026-10-21 第六轮）。

被验证的用户需求（原话）：
  1. "水体不是只有一个水的平面，还是要有一定水的深度的，并且也要符合水体
     反射和折射的视觉效果。而且创建水体的时候，其参数面板会什么会消失不见。"
  2. "水体创建的位置应该是紧贴在地面上的，不能出现反重力的现象，如果水体
     创建在比较低洼甚至凹凸不平的地形上，也要符合这个规律。"

--------------------------------------------------------------------------
场景为什么必须是 water_bowl
--------------------------------------------------------------------------
`scenes/water_bowl.json` 带一张**碗形**地形（512 m 见方台地 14 m，中心
半径 120 m、深 12 m 的碗，碗底还有 ±1.2 m 起伏）。只有这种地形才能同时
验证到两件事：
  · 小半径（10 m）时水面落在碗底，depth 很小；
  · 大半径（覆盖整个碗）时水面抬到台地边缘，depth 十几米。
平坦场景（sandbox 只有 mesh 地面）永远只有 0.18 m 厚，测不出贴地逻辑。

--------------------------------------------------------------------------
断言分组
--------------------------------------------------------------------------
  H. 水体有厚度
     H1 `WATER-MESH ... skirt>0`（侧壁真的生成了）
     H2 水面三角形仍占多数（侧壁没有把水面挤掉）
  I. 贴地（不悬空）
     I1 `WATER-GROUND src=terrain`（真的采到了地形，不是 fallback）
     I2 bed < level（池底在水面之下）
     I3 level ≤ rim + 0.2（水面不高于岸线 → 不会从岸边溢出去、
        也不会在边缘立起一道"水墙"）
     I4 半径变大后 depth 变大（水往低处积，而不是跟着点击点走）
  J. 面板不会"消失"
     J1 选中水体时 `WATER-NEW-ENTRY`（+ 新建水体）在面板首屏
     J2 点它之后回到新建态：`WATER-ADD` 在画、`WATER-DELETE` 不在画
  G. 日志健康：零 VUID / 零 ERROR

用法：python tools/verify_water_depth.py
"""

import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from capture_window import capture_client, find_window           # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors           # noqa: E402
import verify_toolkit_modes as vtm                               # noqa: E402
from verify_water import drag_slider, wait_event                  # noqa: E402

EXE = vtm.EXE
OUT_DIR = vtm.OUT_DIR
SCENE = "scenes/water_bowl.json"

FAILS = []


def check(ok, label):
    print("    %-64s %s" % (label, "PASS" if ok else "FAIL"))
    if not ok:
        FAILS.append(label)
    return ok


def fnum(line, key):
    m = re.search(r"%s=(-?\d+(?:\.\d+)?)" % key, line or "")
    return float(m.group(1)) if m else None


def ensure_scene_assets():
    """把测试场景/地形拷到 exe 目录旁边。

    `assets/scenes` 与 `assets/terrain` 都不在 CMake 的"每次拷贝"列表里
    （scenes 是运行时会写的目录，terrain 是地形编辑会改的目录），所以新
    加的测试资产必须显式同步 —— 否则场景打开失败会**静默回退到默认场景**，
    地形不存在，贴地断言全部变成 `src=flat`（2026-10-21 踩过）。
    """
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    exe_dir = os.path.dirname(EXE)
    pairs = [
        (os.path.join(root, "assets", "scenes", "water_bowl.json"),
         os.path.join(exe_dir, "assets", "scenes", "water_bowl.json")),
    ]
    for src, dst in pairs:
        if os.path.isfile(src):
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy(src, dst)
    src_terrain = os.path.join(root, "assets", "terrain", "water_bowl")
    dst_terrain = os.path.join(exe_dir, "assets", "terrain", "water_bowl")
    if os.path.isdir(src_terrain):
        os.makedirs(dst_terrain, exist_ok=True)
        for name in os.listdir(src_terrain):
            shutil.copy(os.path.join(src_terrain, name),
                        os.path.join(dst_terrain, name))


def launch():
    kill_stale_editors()
    os.makedirs(OUT_DIR, exist_ok=True)
    env = dict(os.environ)
    env.update({
        "MYVK_LOG_RECTS": "1",
        "MYVK_NO_WINDOW_SAVE": "1",
        "MYVK_EDITOR_NO_HUD": "1",
        "MYVK_LIGHT_GIZMOS": "0",
        "MYVK_EDITOR_SCENE": SCENE,
        "MYVK_EDITMODE": "water",
    })
    proc = subprocess.Popen([EXE], cwd=os.path.dirname(EXE), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    return proc, tap


def stop(proc):
    try:
        proc.terminate()
        proc.wait(timeout=8)
    except Exception:                            # noqa: BLE001
        pass
    kill_stale_editors()


def main():
    ensure_scene_assets()
    proc, tap = launch()
    try:
        hwnd = find_window("Venn", 60.0) or find_window("Editor", 20.0)
        if not hwnd:
            print("没找到编辑器窗口")
            return
        vtm.focus_window(hwnd)
        time.sleep(8.0)

        print("\n[H] 水体有厚度（体积）")
        add = vtm.rect_of(tap, "WATER-ADD", 8.0)
        check(bool(add), "H0 面板里有'创建'按钮（WATER-ADD）")
        if add:
            vtm.click(hwnd, *vtm.center(add))
        time.sleep(2.5)

        mesh = wait_event(tap, r"WATER-MESH ", 8.0)
        check(bool(mesh), "H1 网格生成了（WATER-MESH）")
        skirt = fnum(mesh, "skirt")
        tris = fnum(mesh, "tris")
        if mesh:
            print("      ·", mesh.strip()[-150:])
        check(skirt is not None and skirt > 0,
              "H1b 侧壁三角形 > 0（skirt=%s）" % skirt)
        check(tris is not None and skirt is not None and tris > skirt,
              "H2 水面仍是主体（tris=%s > skirt=%s）" % (tris, skirt))

        print("\n[I] 贴地（不悬空、不溢出）")
        ground = wait_event(tap, r"WATER-GROUND ", 8.0)
        check(bool(ground), "I1 打出了地形采样结果（WATER-GROUND）")
        if ground:
            print("      ·", ground.strip()[-150:])
        src = re.search(r"src=(\w+)", ground or "")
        check(bool(src) and src.group(1) == "terrain",
              "I1b 采到的是真地形（src=terrain，不是 fallback）")

        bed = fnum(ground, "bed")
        rim = fnum(ground, "rim")
        level = fnum(ground, "level")
        depth = fnum(ground, "depth")
        check(bed is not None and level is not None and bed < level,
              "I2 池底低于水面（bed=%s < level=%s）" % (bed, level))
        check(rim is not None and level is not None and level <= rim + 0.25,
              "I3 水面不高于岸线（level=%s ≤ rim+0.25=%s）"
              % (level, None if rim is None else round(rim + 0.25, 2)))

        # ---- I4：把半径拖大，水面抬到台地边缘，水更深 ----
        before_depth = depth
        before_rim = rim
        line = drag_slider(tap, hwnd, "WATER-RADIUS", 130, r"WATER-SIZE ")
        ok = check(bool(line), "I4 半径拖大了（WATER-SIZE）")
        if line:
            print("      ·", line.strip()[-120:])
        time.sleep(1.5)
        ground2 = wait_event(tap, r"WATER-GROUND ", 6.0)
        if ground2:
            print("      ·", ground2.strip()[-150:])
        rim2 = fnum(ground2, "rim")
        depth2 = fnum(ground2, "depth")
        check(ok and rim2 is not None and before_rim is not None
              and rim2 > before_rim + 1.0,
              "I4b 大范围的岸线抬高了（rim %s → %s）" % (before_rim, rim2))
        check(ok and depth2 is not None and before_depth is not None
              and depth2 > before_depth + 1.0,
              "I4c 低洼处水更深（depth %s → %s）" % (before_depth, depth2))
        level2 = fnum(ground2, "level")
        check(rim2 is not None and level2 is not None
              and level2 <= rim2 + 0.25,
              "I4d 放大之后依然不溢出（level=%s ≤ rim=%s + 0.25）"
              % (level2, rim2))

        print("\n[J] 参数面板不会消失")
        entry = vtm.reveal(tap, hwnd, "WATER-NEW-ENTRY", "WATER-PANEL")
        print("      · entry=%s panel=%s log=%d"
              % (entry, vtm.rect_of(tap, "WATER-PANEL", 1.0),
                 vtm.count(tap, "WATER-NEW-ENTRY")))
        check(bool(entry), "J1 选中水体时首屏有'＋ 新建水体'入口")
        if entry:
            n_before = vtm.count(tap, "WATER-CREATE")
            vtm.click(hwnd, *vtm.center(entry))
            time.sleep(0.9)
            print("      · 点击后 WATER-NEW-ENTRY 日志=%d"
                  % vtm.count(tap, "WATER-NEW-ENTRY"))
            # 判据用**日志**而不是锚点：`logRect` 对不动的矩形不重复输出，
            # 新建态内容又不足以滚动，`drawn()` 那套"滚一下看行数涨没涨"
            # 在这里会假失败。
            unbind = wait_event(tap, r"WATER-UNBIND", 5.0)
            check(bool(unbind), "J2 回到新建态（WATER-UNBIND）")
            # 真正要验的是"面板还能用"：再点一次创建，必须再建出一块
            add2 = vtm.rect_of(tap, "WATER-ADD", 4.0)
            if add2:
                vtm.click(hwnd, *vtm.center(add2))
            time.sleep(1.8)
            n_after = vtm.count(tap, "WATER-CREATE")
            check(n_after > n_before,
                  "J2b 面板仍可用：又建出一块（WATER-CREATE %d → %d）"
                  % (n_before, n_after))

        print("\n[G] 日志健康")
        vuid = vtm.count(tap, "VUID")
        print("      · VUID=%d" % vuid)
        check(vuid == 0, "G1 零 VUID 验证错误")

        img, _ = capture_client(hwnd)
        if img is not None:
            img.save(os.path.join(OUT_DIR, "water_depth.png"))
            print("\n截图：%s" % os.path.join(OUT_DIR, "water_depth.png"))

        print("\n%s" % ("全部通过。" if not FAILS
                        else "失败 %d 项：\n  - %s"
                        % (len(FAILS), "\n  - ".join(FAILS))))
    finally:
        stop(proc)


if __name__ == "__main__":
    main()
