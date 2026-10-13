"""端到端验证：Hi-Z 遮挡剔除（两帧延迟，R2-1）。

被测链路（HiZOcclusion + hiz_build/hiz_downsample/occlusion_test.comp +
Renderer::buildDrawLists）：
    深度图（前向 pass 写完，只读）
        --(hiz_build.comp)-->        Hi-Z mip0（取样本 max）
        --(hiz_downsample.comp ×N)--> Hi-Z mip1..N（2×2 取 max）
        --(occlusion_test.comp)-->    每候选 AABB 的 vis 标志
        --(两帧延迟 + in-flight 栅栏后回读)--> CPU 的 occluded 实体集
        --(buildDrawLists 过滤)-->    被挡物体不进绘制列表

沿用全套既有约定：**默认关 = 基线逐像素不变**。遮挡剔除的验收核心是
"画面逐像素不变、但提交量下降" —— 被剔的一定是整块都躲在几何后面的物体，
所以它本来就不可见。

断言分组：

  A. **关着 = 基线**（回归硬要求）
     · CULL-STATS 全程 occlOn=0 / occl=0；
     · 两次同配置运行，视口区域逐像素一致。

  B. **排查通道可用**：打开时 HIZ-STATE 日志出现且 enabled=1；
     CULL-STATS 里 occlOn=1；金字塔级数 ≥ 6。

  C. **确实剔了东西**（被挡物体）
     · CULL-STATS 的 occl（本帧剔掉的物体数）稳态 ≥ 3；
     · HIZ-STATE 里 GPU 判定 occluded ≥ 3；
     · drawCalls 比关着时**严格下降**（被挡的球不再提交）。

  D. **画面逐像素不变**（最关键的正确性）
     · 开 vs 关：视口内 >18 的像素数 = 0，平均差 ≤ 0.35。
     这一条钉死"只剔了本来就被挡住的东西" —— 一旦误剔可见物体，
     这里立刻爆。

  E. 全程零 Vulkan 验证层报错。

用法：
    python tools/verify_hiz.py
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import (capture_client, capture_nonempty, find_window,  # noqa: E402
                            user32)
from verify_add_menu import LogTap, kill_stale_editors, wait_rect   # noqa: E402
from verify_local_shadow import diff_stats, to_np, vp_box           # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = os.path.join(ROOT, "..", "_venn_verify")

# 相机在 (0,0,3) 朝 +X 平视
CAM = "0,0,3,180,0"

CULL_RE = re.compile(
    r"CULL-STATS renderables=(\d+) culled=(\d+) drawnOpaque=(\d+) "
    r"drawnTransparent=(\d+) gizmosCulled=(\d+) drawCalls=(\d+) tris=(\d+) "
    r"enabled=(\d+).*occl=(\d+) occlOn=(\d+) hizMips=(\d+)")
HIZ_RE = re.compile(
    r"HIZ-STATE enabled=(\d+) tested=(\d+) occluded=(\d+) "
    r"mips=(\d+) size=(\d+)x(\d+) frame=(\d+)")


def build_scene(path):
    """一面正对相机的墙 + 墙后 3 个球（应被剔）+ 墙前 1 个球（不该被剔）。

    相机 (0,0,3) 朝 +X：墙在 x=3 竖着，挡在中间；球 A/B/C 在 x=6 的墙后；
    球 D 在 x=1.5 的墙前 —— D 是"不许误剔"的正对照。
    全部 castShadow=False：避免阴影图随绘制列表变化，污染"A/B 逐像素相同"。
    """
    def mat(name, base, metallic, roughness):
        return {"kind": "procedural", "name": name,
                "baseColor": list(base) + [1.0],
                "metallic": metallic, "roughness": roughness,
                "normalScale": 1.0, "ao": 1.0,
                "emissive": [0.0, 0.0, 0.0], "doubleSided": False,
                "alphaBlend": False}

    def ent(name, pos, scale, mesh, material):
        return {
            "name": name,
            "transform": {"position": list(pos), "rotation": [0, 0, 0],
                          "scale": list(scale)},
            "visible": True, "castShadow": False, "parent": -1,
            "mesh": mesh, "material": material,
        }

    ents = []
    # 墙：薄板，覆盖视野中央一大块
    ents.append(ent(
        "Wall", (3.0, 0.0, 3.0), [0.15, 3.0, 3.0],
        {"kind": "builtin", "shape": "cube", "size": 1.0, "name": "hz_wall"},
        mat("hz_wall_mat", (0.55, 0.55, 0.58), 0.0, 0.6)))
    # 墙后的三个球（应被剔除）
    for i, y in enumerate([-1.2, 0.0, 1.2]):
        ents.append(ent(
            "Hidden%d" % i, (6.0, y, 3.0), [1.0, 1.0, 1.0],
            {"kind": "builtin", "shape": "sphere", "size": 0.6,
             "segments": 24, "rings": 12, "name": "hz_hid%d" % i},
            mat("hz_hid_mat%d" % i, (0.9, 0.15, 0.12), 0.0, 0.45)))
    # 墙前的一个球（正对照：绝不能被剔）
    ents.append(ent(
        "Front", (1.5, 0.0, 3.0), [1.0, 1.0, 1.0],
        {"kind": "builtin", "shape": "sphere", "size": 0.5,
         "segments": 24, "rings": 12, "name": "hz_front"},
        mat("hz_front_mat", (0.12, 0.45, 0.95), 0.0, 0.4)))

    doc = {
        "version": 1,
        "generator": "verify_hiz.py",
        "light": {"direction": [-1.0, 0.2, 0.3], "color": [1.0, 0.98, 0.95],
                  "intensity": 1.0, "castsShadow": False, "ambientScale": 1.0},
        "entities": ents,
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)


def run_once(exe, scene, extra_env, shot_path=None, settle=3.0):
    """跑一轮编辑器，返回 (全部日志行, 视口矩形, 截图, 错误行)。"""
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_LIGHT_GIZMOS"] = "0"
    env["MYVK_EDITOR_NO_HUD"] = "1"
    env["MYVK_SHADER_VARIANTS"] = "1"     # 用它当"前向 Pass 已就绪"的信号
    env["MYVK_EDITOR_SCENE"] = scene
    env["MYVK_EDITOR_CAM"] = CAM
    # 固定曝光，避免自动曝光的收敛曲线在开/关两轮里走不同的路径
    env["MYVK_EXPOSURE"] = "1.0"
    env.update(extra_env)

    proc = subprocess.Popen(
        [exe], cwd=os.path.dirname(exe), env=env, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, encoding="utf-8",
        errors="replace", bufsize=1)
    tap = LogTap()
    tap.start(proc)
    hwnd = None
    try:
        hwnd = find_window("Venn Editor")
        if not hwnd:
            return [], None, None, ["未找到编辑器窗口"]
        user32.ShowWindow(hwnd, 9)          # 9 = SW_RESTORE
        try:
            user32.SetForegroundWindow(hwnd)
            user32.BringWindowToTop(hwnd)
        except Exception:
            pass
        time.sleep(1.5)
        user32.SetWindowPos(hwnd, 0, 0, 0, 1280, 800, 0x0002 | 0x0004)
        time.sleep(2.5)

        vp = wait_rect(tap, "VP-RECT", 10.0)
        end = time.time() + 16.0
        while time.time() < end:
            with tap.lock:
                n = sum(1 for l in tap.lines if "SHADER-VARIANTS " in l)
            if n >= 2:
                break
            time.sleep(0.25)
        time.sleep(settle)

        img = None
        if shot_path:
            img = capture_nonempty(hwnd)
            if img is None:
                import ctypes as _ct
                import ctypes.wintypes as _wt
                r = _wt.RECT()
                user32.GetClientRect(hwnd, _ct.byref(r))
                print("  [dbg] 抓图失败：client=(%d,%d) 进程存活=%s 标题可见=%s"
                      % (r.right, r.bottom, proc.poll() is None,
                         bool(user32.IsWindowVisible(hwnd))))
            else:
                img.save(shot_path)

        with tap.lock:
            lines = list(tap.lines)
        errs = [l for l in lines
                if ("VUID" in l or "VALIDATION" in l or " ERROR" in l
                    or "Error:" in l)]
        return lines, vp, img, errs
    finally:
        if hwnd:
            user32.PostMessageW(hwnd, 0x0010, 0, 0)
        time.sleep(0.5)
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=6)
            except Exception:
                proc.kill()


def cull_lines(lines):
    """抽 (renderables, culled, drawnOpaque, drawnTransparent, gizmosCulled,
    drawCalls, tris, enabled, occl, occlOn, hizMips)。"""
    out = []
    for l in lines:
        m = CULL_RE.search(l)
        if m:
            out.append(tuple(int(v) for v in m.groups()))
    return out


def hiz_lines(lines):
    """抽 (enabled, tested, occluded, mips, w, h, frame)。"""
    out = []
    for l in lines:
        m = HIZ_RE.search(l)
        if m:
            out.append(tuple(int(v) for v in m.groups()))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    os.makedirs(OUT_DIR, exist_ok=True)
    if not os.path.exists(exe):
        print("找不到可执行文件，请先构建：", exe)
        return 1

    scene = os.path.join(OUT_DIR, "hiz_test.json")
    build_scene(scene)
    print("测试场景：%s" % scene)

    fails = []

    def check(name, cond, detail=""):
        print(("  [PASS] " if cond else "  [FAIL] ") + name +
              (("  " + str(detail)) if detail else ""))
        if not cond:
            fails.append(name)

    # 打开两条诊断日志：CULL-STATS（含 occl/occlOn/hizMips）+ HIZ-STATE
    DBG = {"MYVK_FRUSTUM_DBG": "1", "MYVK_HIZ_DBG": "1"}

    kill_stale_editors()

    # ============ A 组：关着 = 基线 ============
    print("\n== A 组：Hi-Z 关闭 → 零 compute、零剔除，两次运行逐像素一致 ==")
    shot_a = os.path.join(OUT_DIR, "hiz_off.png")
    lines_a, vp, img_a, errs_a = run_once(exe, scene, dict(DBG), shot_a)
    if not vp:
        print("  没等到 VP-RECT")
        return 2
    c_a = cull_lines(lines_a)
    print("  CULL-STATS 行数=%d 样例=%s" % (len(c_a), c_a[-1] if c_a else None))
    check("拿到 CULL-STATS 日志", len(c_a) > 0)
    check("零 Vulkan 验证层报错", not errs_a, errs_a[:2])
    if c_a:
        check("关着时 occlOn 全程 = 0", set(r[9] for r in c_a) == {0},
              sorted(set(r[9] for r in c_a)))
        check("关着时 occl 全程 = 0", set(r[8] for r in c_a) == {0},
              sorted(set(r[8] for r in c_a)))
    check("关着时没有 HIZ-STATE 日志（零 compute）", not hiz_lines(lines_a),
          len(hiz_lines(lines_a)))

    shot_a2 = os.path.join(OUT_DIR, "hiz_off2.png")
    _, _, img_a2, errs_a2 = run_once(exe, scene, dict(DBG), shot_a2)
    check("第二次运行零报错", not errs_a2, errs_a2[:2])
    if img_a is not None and img_a2 is not None:
        cnt, mx, mean, _ = diff_stats(img_a, img_a2, vp)
        print("  两次运行：>18 像素=%d 最大差=%d 平均差=%.4f" % (cnt, mx, mean))
        check("关着时两次运行视口逐像素一致（平均差 < 0.01）", mean < 0.01,
              "%.4f" % mean)

    if img_a is None:
        print("  基线截图抓不到（窗口前台时序）—— 无法继续像素断言")
        fails.append("基线截图")
        print("\n结果: FAIL: 基线截图")
        return 1

    draw_off = c_a[-1][5] if c_a else -1
    print("  基线 drawCalls=%d" % draw_off)

    # ============ B/C/D 组：打开 Hi-Z ============
    print("\n== B/C/D 组：打开 Hi-Z → 剔掉被挡的球，画面逐像素不变 ==")
    shot_b = os.path.join(OUT_DIR, "hiz_on.png")
    lines_b, _, img_b, errs_b = run_once(
        exe, scene, dict(DBG, MYVK_HIZ="1"), shot_b)
    check("零 Vulkan 验证层报错", not errs_b, errs_b[:2])

    c_b = cull_lines(lines_b)
    h_b = hiz_lines(lines_b)
    print("  CULL-STATS 行数=%d 样例=%s" % (len(c_b), c_b[-1] if c_b else None))
    print("  HIZ-STATE 行数=%d 样例=%s" % (len(h_b), h_b[-1] if h_b else None))

    check("打开时 CULL-STATS occlOn 全程 = 1", bool(c_b)
          and set(r[9] for r in c_b) == {1},
          sorted(set(r[9] for r in c_b)) if c_b else None)
    check("打开时 HIZ-STATE 出现且 enabled=1", bool(h_b)
          and set(r[0] for r in h_b) == {1},
          sorted(set(r[0] for r in h_b)) if h_b else None)

    # 金字塔级数：1280x800 → maxDim=1280 → 2^10 ≤ 1280 < 2^11 → 11 级
    mips = max((r[10] for r in c_b), default=0)
    print("  Hi-Z 金字塔级数=%d" % mips)
    check("Hi-Z 金字塔级数 ≥ 6", mips >= 6, mips)

    occl_max = max((r[8] for r in c_b), default=0)
    occl_gpu = max((r[2] for r in h_b), default=0)
    print("  本帧剔除数 max=%d / GPU 判定 occluded max=%d" % (occl_max, occl_gpu))
    check("确实剔掉了被挡的球（CULL-STATS occl ≥ 3）", occl_max >= 3, occl_max)
    check("GPU 判定 occluded ≥ 3（HIZ-STATE）", occl_gpu >= 3, occl_gpu)

    draw_on = c_b[-1][5] if c_b else -1
    print("  打开时 drawCalls=%d（基线 %d）" % (draw_on, draw_off))
    check("drawCalls 比基线严格下降（被挡的球不再提交）",
          draw_off > 0 and draw_on < draw_off,
          "%d -> %d" % (draw_off, draw_on))

    if img_b is not None:
        cnt, mx, mean, _ = diff_stats(img_b, img_a, vp)
        print("  开 vs 关：>18 像素=%d 最大差=%d 平均差=%.4f" % (cnt, mx, mean))
        check("遮挡剔除不改变画面（>18 像素 = 0）", cnt == 0, cnt)
        check("平均差 ≤ 0.35", mean <= 0.35, "%.4f" % mean)
    else:
        check("打开时抓到截图", False, "抓图失败（前台时序 flaky）")

    print("\n结果: %s" % ("PASS" if not fails else "FAIL: " + ", ".join(fails)))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
