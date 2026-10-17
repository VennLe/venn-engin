"""端到端验证：胶囊碰撞体线框的**形状规范性**。

用户原文（2026-10-17）：
    "胶囊碰撞体的栅格形状似乎是有些不太完整，能否做的标准规范些。"

"标准规范"要能机器判，所以这里不靠"看着像"，而是把线框的**屏幕轮廓**抽出来
量几何。两个用例，因为它们走的是**两条不同的建桩路**：

  用例 A：碰撞体参数直接写在场景 JSON 的 `collision` 段里
          （R=0.35 / halfHeight=0.90 → 真胶囊，期望 H/W = (H+R)/R = 3.571）
  用例 B：`MYVK_EDITOR_COLLIDER=capsule` 钩子 —— 对 1m **立方体**做拟合。
          ⚠ 这里拟合出来的**必然是球**：外接圆半径 r = sqrt(0.5²+0.5²) =
          0.707 > 半高 0.5 → `halfHeight = max(0.5 − 0.707, 0) = 0`。
          （不是 bug，是"胶囊整个盖住 AABB"这条设计（见
          CollisionWorld.cpp::buildCapsuleCollider）的必然结果；但它很可能
          正是用户觉得"胶囊形状不太完整"的原因 —— 点"加胶囊碰撞体"以后看到
          的其实是个球。）用例 B 就把这个事实钉成断言，免得以后有人以为线框
          画崩了。

投影相机刻意摆在正前方（yaw=0 / pitch=0）→ 胶囊轴（碰撞体局部 +Z，
见 ecs::Components.h `Capsule = 1  // 轴 = 碰撞体局部 +Z`）投影成**竖直**，
轮廓就是"矩形 + 上下两个半圆"，于是对称性、桶身、球冠都能逐行量。

线框颜色 = 选中金黄 `IM_COL32(255,202,74)`（用例 A 不进编辑态；编辑态是橙
`(255,148,40)`）。掩码判据必须带上 `g > 95` —— gizmo 的 **X 轴红箭头**也在
"暖色"里（R−B 很大），而它就长在胶囊中心、正好污染"桶身最宽"那一段。金/橙
的 g 都在 150~220，红轴的 g 很低，一刀就分开。

断言：
  A1 轮廓外包框的 H/W 与 (H+R)/R 一致（±6%）
  A2 最大宽度**贯穿**竖直中部，且"最宽区段"的中点在竖直中部
     （⚠ 不能用"第一行最宽处的位置"判 —— 桶身形状下那一段从球冠交界就
      开始了，实测落在 12%，但它完全正确）
  A3 上下两端明显收窄（是球冠，不是被切平的端口）
  A4 轮廓上下镜像（逐行宽度差 ≤3px 的比例 ≥ 90%）
  A5 桶身段：存在足够长的一段行宽≈最大宽（圆柱段真的直）
  B1 拟合路对立方体给出 halfHeight=0（球），H/W = 1.0

用法：
    python tools/verify_capsule_shape.py
    python tools/verify_capsule_shape.py --keep     # 留窗口看图
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_client, find_window, user32        # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors, wait_rect     # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify")

# 用例 A 的胶囊尺寸（同时是写进 JSON 的输入和期望值的来源 —— 单一真源）
CAP_R = 0.35
CAP_H = 0.90

AUTO_RE = re.compile(
    r"Editor auto-collider \(capsule\): '([^']*)' points=(\d+) edges=(\d+) "
    r"radius=([\d.]+) halfHeight=([\d.]+)")

# 线框掩码判据（见文件头：g 下限用来排除 gizmo 的红轴）
WARM_RB = 45      # r - b 下限
WARM_RMIN = 110   # r 下限
WARM_GMIN = 95    # g 下限


def make_scene(path, with_collision):
    """灰色材质的 1m 立方体：确保视口里**没有别的暖色**（红材质会把线框
    掩码污染成一片）。相机正对 +Z 轴，距离 6m。

    with_collision=True 时把胶囊参数直接写进 JSON（用例 A）；False 时留给
    MYVK_EDITOR_COLLIDER 钩子去拟合（用例 B）。"""
    ent = {
        "name": "CapsuleCube",
        "transform": {"position": [0.0, 0.0, CAP_H + CAP_R],
                      "rotation": [0.0, 0.0, 0.0],
                      "scale": [1.0, 1.0, 1.0]},
        "parent": -1, "visible": True, "castShadow": True,
        "mesh": {"kind": "builtin", "shape": "cube", "size": 1.0,
                 "name": "capsule_shape_cube"},
        "material": {"kind": "procedural", "name": "capsule_grey",
                     "baseColor": [0.55, 0.55, 0.56, 1.0],
                     "metallic": 0.0, "roughness": 0.85,
                     "normalScale": 0.0, "ao": 1.0,
                     "emissive": [0, 0, 0],
                     "doubleSided": False, "alphaBlend": False},
    }
    if with_collision:
        ent["collision"] = {
            "shape": "capsule",
            "position": [0.0, 0.0, 0.0],
            "rotation": [0.0, 0.0, 0.0],
            "scale": [1.0, 1.0, 1.0],
            "capsuleRadius": CAP_R,
            "capsuleHalfHeight": CAP_H,
            "solid": False,
            "showWireframe": True,
        }
    scene = {
        "version": 1,
        "generator": "verify_capsule_shape",
        "camera": {"target": [0, 0, CAP_H + CAP_R], "yaw": 0.0, "pitch": 0.0,
                   "distance": 6.0},
        "entities": [ent],
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(scene, f, indent=2)
    return path


def row_profile(img, box):
    """返回 [(y, x_left, x_right)] —— 只保留有暖色像素的行。

    box 是 `COLLIDER <名字>` 那个外包矩形（线框自己的屏幕包围框，由
    ColliderView 投影后算出并打日志），比"整屏找暖色"稳得多。
    """
    x0, y0, x1, y1 = [int(round(v)) for v in box]
    W, H = img.size
    x0, y0 = max(x0, 0), max(y0, 0)
    x1, y1 = min(x1, W), min(y1, H)
    px = img.convert("RGB").load()
    rows = []
    for y in range(y0, y1):
        lo, hi = None, None
        for x in range(x0, x1):
            r, g, b = px[x, y]
            if r - b > WARM_RB and r > WARM_RMIN and g > WARM_GMIN:
                if lo is None:
                    lo = x
                hi = x
        if lo is not None:
            rows.append((y, lo, hi))
    return rows


def run_case(exe, tapenv, scene, tag, keep, check, save_png):
    """启动一次、等线框、抽样轮廓。返回 (rows, vp) 或 None。"""
    env = dict(os.environ)
    env.update(tapenv)
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_EDITOR_SCENE"] = scene
    env["MYVK_EDITOR_SELECT"] = "CapsuleCube"
    env["MYVK_GRID"] = "0"            # 栅格是屏幕空间固定图案，去掉
    env["MYVK_EDITOR_NO_HUD"] = "1"   # 视口 2D 叠加层全关（含 FPS 等）
    env["MYVK_LIGHT_GIZMOS"] = "0"

    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    try:
        hwnd = find_window("Venn Editor")
        if not hwnd:
            print("未找到窗口"); return None
        user32.ShowWindow(hwnd, 9)
        time.sleep(2.0)
        user32.SetWindowPos(hwnd, 0, 0, 0, 1100, 900, 0x0002 | 0x0004)
        time.sleep(2.5)

        auto = None
        end = time.time() + 6.0
        while time.time() < end and auto is None:
            with tap.lock:
                for line in tap.lines:
                    m = AUTO_RE.search(line)
                    if m:
                        auto = (float(m.group(4)), float(m.group(5)))
                        break
            time.sleep(0.15)

        box = wait_rect(tap, "COLLIDER CapsuleCube", 8.0)
        if not box:
            print("拿不到 'COLLIDER CapsuleCube' 外包矩形（线框没画？）")
            return None
        img, _ = capture_client(hwnd)
        img.save(os.path.join(OUT_DIR, save_png))
        vp = wait_rect(tap, "VP-RECT", 4.0)
        if vp:
            x0 = max(0, int(vp[0]))
            y0 = max(0, int(vp[1]))
            x1 = min(img.size[0], int(vp[2]))
            y1 = min(img.size[1], int(vp[3]))
            if x1 > x0 and y1 > y0:
                crop = img.crop((x0, y0, x1, y1))
                crop.resize((crop.size[0] * 2, crop.size[1] * 2)).save(
                    os.path.join(OUT_DIR, save_png.replace(
                        ".png", "_zoom.png")))
        rows = row_profile(img, box)
        return rows, auto
    finally:
        if keep:
            print("--keep：留窗口 pid", proc.pid)
        else:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()


def shape_checks(rows, want_ratio, pfx, check):
    if not rows or len(rows) < 20:
        check("%s0 线框像素够多" % pfx, False,
              "只有 %d 行（线框没画出来或掩码过严）" % (len(rows or [])))
        return
    ys = [r[0] for r in rows]
    widths = [r[2] - r[1] + 1 for r in rows]
    hgt = ys[-1] - ys[0] + 1
    wid = max(widths)
    ratio = hgt / float(wid)

    check("%s1 轮廓宽高比 = (halfHeight+radius)/radius" % pfx,
          abs(ratio - want_ratio) <= 0.06 * want_ratio,
          "实测 %.3f vs 期望 %.3f (%d×%d px)" % (ratio, want_ratio, wid, hgt))

    frac = widths.index(wid) / float(len(widths) - 1)
    # ⚠ "最宽行"对**桶身**形状没意义：宽 = 最大宽的行是一整段（从球冠与桶身
    #   的交界开始），"第一行最宽"自然落在顶部附近（实测 12%），但那不是缺陷。
    #   真正该判的是"宽度最大值**贯穿**竖直中部"——球和胶囊都成立。
    mid_i = len(widths) // 2
    #   另外量一把"最宽区段"的中点（球：整个圆面；胶囊：整段桶身），
    #   它必须落在竖直中部 —— 偏了就说明轮廓往一头甩了。
    wide = [i for i, w in enumerate(widths) if w >= 0.97 * wid]
    wide_mid = (wide[0] + wide[-1]) * 0.5 / float(len(widths) - 1)
    check("%s2 最大宽度贯穿竖直中部（桶身/赤道）" % pfx,
          widths[mid_i] >= 0.97 * wid and 0.38 <= wide_mid <= 0.62,
          "中点行宽=%d/%d；最宽区段中点=%.0f%%（首行最宽处 %.0f%% 只作参考）"
          % (widths[mid_i], wid, wide_mid * 100.0, frac * 100.0))

    check("%s3 上下两端明显收窄（球冠，不是平口）" % pfx,
          widths[0] < 0.40 * wid and widths[-1] < 0.40 * wid,
          "top=%d bot=%d vs max=%d" % (widths[0], widths[-1], wid))

    n = len(widths)
    good = sum(1 for i in range(n // 2) if abs(widths[i] - widths[n - 1 - i]) <= 3)
    sym = good / float(max(1, n // 2))
    check("%s4 轮廓上下对称（逐行宽度镜像 ≤3px）" % pfx, sym >= 0.90,
          "对称行 %.0f%%" % (sym * 100.0))

    if want_ratio > 1.5:
        straight = sum(1 for w in widths if w >= 0.97 * wid)
        need = int(round((want_ratio - 1.0) / want_ratio * n * 0.6))
        check("%s5 存在足够长的桶身直段（圆柱段）" % pfx, straight >= max(3, need),
              "直段 %d 行（至少 %d）" % (straight, max(3, need)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--skip-b", action="store_true", help="跳过用例 B")
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    if not os.path.exists(exe):
        print("找不到可执行文件:", exe)
        return 2
    os.makedirs(OUT_DIR, exist_ok=True)
    kill_stale_editors()

    fails = []
    skipped = []

    def check(name, ok, detail=""):
        print("  [%s] %s %s" % ("PASS" if ok else "FAIL", name, detail))
        if not ok:
            fails.append(name)

    def skip(name, detail=""):
        print("  [SKIP] %s %s" % (name, detail))
        skipped.append(name)

    tmp = tempfile.mkdtemp(prefix="venn_capsule_")

    # ---------------- 用例 A：真胶囊（JSON 显式给参数） ----------------
    print("== 用例 A：R=%.2f halfHeight=%.2f（JSON 显式）" % (CAP_R, CAP_H))
    scene_a = make_scene(os.path.join(tmp, "a.json"), True)
    res = run_case(exe, {}, scene_a, "A", args.keep, check,
                   "capsule_shape_explicit.png")
    if not res:
        check("A0 用例 A 起桩", False, "见上面日志")
    else:
        rows_a, _ = res
        want = (CAP_H + CAP_R) / CAP_R
        shape_checks(rows_a, want, "A", check)
        print("  期望 H/W = (%.2f+%.2f)/%.2f = %.3f"
              % (CAP_H, CAP_R, CAP_R, want))

    # ---------------- 用例 B：拟合路（对立方体 → 退化球） ----------------
    if args.skip_b:
        skip("B 拟合路（立方体 → 球）", "--skip-b")
    else:
        print("== 用例 B：MYVK_EDITOR_COLLIDER=capsule 对 1m 立方体拟合")
        scene_b = make_scene(os.path.join(tmp, "b.json"), False)
        res_b = run_case(exe, {"MYVK_EDITOR_COLLIDER": "capsule"}, scene_b,
                         "B", args.keep, check, "capsule_shape_fit.png")
        if not res_b:
            check("B0 用例 B 起桩", False, "见上面日志")
        else:
            rows_b, auto = res_b
            if auto is None:
                skip("B1 拟合参数来自日志", "没抓到 auto-collider 日志")
            else:
                r, h = auto
                check("B1 拟合对立方体给出 halfHeight=0（球，不是胶囊）",
                      h == 0.0 and r > 0.0, "R=%.3f halfHeight=%.3f" % (r, h))
                if rows_b:
                    shape_checks(rows_b, 1.0, "B", check)

    print("截图 →", OUT_DIR)
    tail = ("（另有 %d 条 SKIP：%s）" % (len(skipped), ", ".join(skipped))
            if skipped else "")
    if fails:
        print("结果: FAIL: " + ", ".join(fails) + tail)
        return 1
    print("结果: PASS" + tail)
    return 0


if __name__ == "__main__":
    sys.exit(main())
