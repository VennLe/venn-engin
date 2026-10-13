"""端到端验证：色彩分级 LUT（32³，R1-3）。

被测链路（PostProcess::bakeLut + post.frag）：
    lift / gamma / gain / contrast / saturation
        --(CPU 烘 32³ 格点，16F 打包)--> staging 上传 --> 3D 纹理
        --(合成 pass 一次三线性采样，tonemap 之后、暗角之前)--> 画面

沿用全套既有约定：**默认关 = 基线逐像素不变**。

断言分组：

  A. **关着 = 基线**（回归硬要求）
     · LUT-INFO 全部 enabled=0；
     · 两次同配置运行，视口区域**逐像素一致**（关着时行为完全确定）。

  B. **开着但参数全默认 = 恒等映射**
     · LUT-INFO identity=1（CPU 侧烘出来的格点就是输入本身）；
     · 与 A 的基线对照：视口**几乎逐像素相同**。
       这一条同时验证了：① 三线性采样的坐标映射（×31/32 + 1/64）正确
       —— 坐标映射错了会整体偏色；② 恒等是仿射映射，硬件三线性可精确
       重建（见 post.frag 注释）。

  C. **对比度真的在起作用，且方向正确**
     · contrast=1.8 → 亮度直方图**展宽**（std 上升）；
     · contrast=0.5 → **收窄**（std 下降）。
     方向性断言比"变了没有"强得多 —— 它要求 LUT 里的调色数学是对的，
     而不只是"画面动了"。

  D. **饱和度真的在起作用**：saturation=0 → 视口内 |R-G|、|G-B| 趋近 0
     （整幅图变中性灰）。这一条是"LUT 被真正采样"的铁证：如果 set 2 没
     被采样、或采样到垃圾，不可能得到中性灰。

  E. **混合权重（blend）单调**：同一个 contrast=1.8，blend=0.5 的变化量
     必须严格落在 blend=0（恒等）与 blend=1 之间。

  F. 全程零 Vulkan 验证层报错。

用法：
    python tools/verify_color_lut.py
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

# 相机在 (0,0,3) 朝 +X 平视 —— 画面里同时有彩色球与天空（调色的好输入）
CAM = "0,0,3,180,0"

LUT_RE = re.compile(
    r"LUT-INFO enabled=(\d+) blend=([-\d.]+) identity=(\d+) size=(\d+) "
    r"contrast=([-\d.]+) saturation=([-\d.]+) lift=([-\d.]+) "
    r"gamma=([-\d.]+) gain=([-\d.]+)")


def build_scene(path):
    """天空 + 三颗高饱和彩球：饱和度/对比度的变化都能被看清。"""
    def mat(name, base, metallic, roughness):
        return {"kind": "procedural", "name": name,
                "baseColor": list(base) + [1.0],
                "metallic": metallic, "roughness": roughness,
                "normalScale": 1.0, "ao": 1.0,
                "emissive": [0.0, 0.0, 0.0], "doubleSided": False,
                "alphaBlend": False}

    ents = []
    for i, (y, col, met, rough) in enumerate([
            (-1.2, (0.90, 0.15, 0.10), 0.0, 0.45),   # 红
            (0.0, (0.10, 0.75, 0.20), 0.0, 0.45),    # 绿
            (1.2, (0.12, 0.35, 0.95), 0.0, 0.45)]):  # 蓝
        ents.append({
            "name": "Ball%d" % i,
            "transform": {"position": [4.0, y, 3.0], "rotation": [0, 0, 0],
                          "scale": [1, 1, 1]},
            "visible": True, "castShadow": False, "parent": -1,
            "mesh": {"kind": "builtin", "shape": "sphere", "size": 0.75,
                     "segments": 32, "rings": 16, "name": "cg_ball%d" % i},
            "material": mat("cg_ball_mat%d" % i, col, met, rough),
        })
    doc = {
        "version": 1,
        "generator": "verify_color_lut.py",
        "light": {"direction": [-1.0, 0.2, 0.3], "color": [1.0, 0.98, 0.95],
                  "intensity": 1.0, "castsShadow": False, "ambientScale": 1.0},
        "entities": ents,
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)


def run_once(exe, scene, extra_env, shot_path=None, settle=2.5):
    """跑一轮编辑器，返回 (全部日志行, 视口矩形, 截图, 错误行)。"""
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_LIGHT_GIZMOS"] = "0"
    env["MYVK_EDITOR_NO_HUD"] = "1"
    env["MYVK_SHADER_VARIANTS"] = "1"     # 用它当"前向 Pass 已就绪"的信号
    env["MYVK_EDITOR_SCENE"] = scene
    env["MYVK_EDITOR_CAM"] = CAM
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


def lut_lines(lines):
    """抽 (enabled, blend, identity, size, contrast, saturation, lift,
    gamma, gain)。"""
    out = []
    for l in lines:
        m = LUT_RE.search(l)
        if m:
            out.append(tuple(float(v) for v in m.groups()))
    return out


def vp_stats(img, vp):
    """视口区域内：(平均亮度, 亮度标准差, 平均|R-G|, 平均|G-B|)。

    亮度用 Rec.709 权重（与 LUT 里的饱和度定义一致）。
    """
    import numpy as np
    a = to_np(img).astype(float)
    x0, y0, x1, y1 = vp_box(vp)
    x0, y0 = max(0, x0), max(0, y0)
    x1, y1 = min(a.shape[1], x1), min(a.shape[0], y1)
    a = a[y0:y1, x0:x1]
    if a.size == 0:
        return 0.0, 0.0, 0.0, 0.0
    luma = 0.2126 * a[:, :, 0] + 0.7152 * a[:, :, 1] + 0.0722 * a[:, :, 2]
    rg = float(np.abs(a[:, :, 0] - a[:, :, 1]).mean())
    gb = float(np.abs(a[:, :, 1] - a[:, :, 2]).mean())
    return float(luma.mean()), float(luma.std()), rg, gb


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    os.makedirs(OUT_DIR, exist_ok=True)
    if not os.path.exists(exe):
        print("找不到可执行文件，请先构建：", exe)
        return 1

    scene = os.path.join(OUT_DIR, "cg_test.json")
    build_scene(scene)
    print("测试场景：%s" % scene)

    fails = []

    def check(name, cond, detail=""):
        print(("  [PASS] " if cond else "  [FAIL] ") + name +
              (("  " + str(detail)) if detail else ""))
        if not cond:
            fails.append(name)

    kill_stale_editors()
    DBG = {"MYVK_CG_DBG": "1"}

    # ============ A 组：关着 = 基线 ============
    print("\n== A 组：LUT 关闭 → 走基线路径，且两次运行逐像素一致 ==")
    shot_a = os.path.join(OUT_DIR, "cg_off.png")
    lines_a, vp, img_a, errs_a = run_once(
        exe, scene, dict(DBG, MYVK_EXPOSURE="1.0"), shot_a)
    if not vp:
        print("  没等到 VP-RECT")
        return 2
    l_a = lut_lines(lines_a)
    print("  LUT-INFO 行数=%d 样例=%s" % (len(l_a), l_a[-1] if l_a else None))
    check("拿到 LUT-INFO 日志", len(l_a) > 0)
    check("零 Vulkan 验证层报错", not errs_a, errs_a[:2])
    if l_a:
        check("关着时 enabled 全程 = 0",
              set(int(r[0]) for r in l_a) == {0},
              sorted(set(int(r[0]) for r in l_a)))
        check("LUT 尺寸 = 32³", set(int(r[3]) for r in l_a) == {32},
              sorted(set(int(r[3]) for r in l_a)))

    shot_a2 = os.path.join(OUT_DIR, "cg_off2.png")
    _, _, img_a2, errs_a2 = run_once(
        exe, scene, dict(DBG, MYVK_EXPOSURE="1.0"), shot_a2)
    check("第二次运行零报错", not errs_a2, errs_a2[:2])
    if img_a is not None and img_a2 is not None:
        cnt, mx, mean, _ = diff_stats(img_a, img_a2, vp)
        print("  两次运行：>18 像素=%d 最大差=%d 平均差=%.4f" % (cnt, mx, mean))
        check("关着时两次运行视口逐像素一致（平均差 < 0.01）", mean < 0.01,
              "%.4f" % mean)

    base_mean, base_std, base_rg, base_gb = vp_stats(img_a, vp) \
        if img_a is not None else (0.0, 0.0, 0.0, 0.0)
    print("  基线：亮度均值=%.2f std=%.2f |R-G|=%.2f |G-B|=%.2f"
          % (base_mean, base_std, base_rg, base_gb))
    if img_a is None:
        print("  基线截图抓不到（窗口前台时序）—— 无法继续像素断言")
        fails.append("基线截图")
        print("\n结果: FAIL: 基线截图")
        return 1

    # ============ B 组：开着但恒等 ============
    print("\n== B 组：开启但参数全默认 → 恒等映射，画面几乎不变 ==")
    shot_b = os.path.join(OUT_DIR, "cg_identity.png")
    lines_b, _, img_b, errs_b = run_once(
        exe, scene, dict(DBG, MYVK_EXPOSURE="1.0", MYVK_CG="1"), shot_b)
    check("零 Vulkan 验证层报错", not errs_b, errs_b[:2])
    l_b = lut_lines(lines_b)
    if l_b:
        print("  样例=%s" % (l_b[-1],))
        check("开着时 enabled 全程 = 1",
              set(int(r[0]) for r in l_b) == {1})
        check("CPU 烘出的 LUT 被判为恒等（identity=1）",
              set(int(r[2]) for r in l_b) == {1},
              sorted(set(int(r[2]) for r in l_b)))
    if img_a is not None and img_b is not None:
        cnt, mx, mean, _ = diff_stats(img_b, img_a, vp)
        print("  恒等 LUT vs 基线：>18 像素=%d 最大差=%d 平均差=%.4f"
              % (cnt, mx, mean))
        check("恒等 LUT 与基线无可见差异（>18 像素 = 0）", cnt == 0, cnt)
        check("最大差 ≤ 3（16F 采样精度内）", mx <= 3, mx)
        check("平均差 ≤ 0.3", mean <= 0.3, "%.4f" % mean)

    # ============ C 组：对比度方向正确 ============
    print("\n== C1 组：contrast=1.8 → 亮度直方图展宽（std 上升）==")
    shot_c1 = os.path.join(OUT_DIR, "cg_contrast_hi.png")
    lines_c1, _, img_c1, errs_c1 = run_once(
        exe, scene,
        dict(DBG, MYVK_EXPOSURE="1.0", MYVK_CG="1", MYVK_CG_CONTRAST="1.8"),
        shot_c1)
    check("零 Vulkan 验证层报错", not errs_c1, errs_c1[:2])
    l_c1 = lut_lines(lines_c1)
    check("日志里 contrast=1.8",
          bool(l_c1) and abs(l_c1[-1][4] - 1.8) < 1e-3,
          l_c1[-1][4] if l_c1 else None)
    check("identity=0（参数已偏离恒等）",
          bool(l_c1) and int(l_c1[-1][2]) == 0)
    if img_c1 is not None:
        m1, s1, _, _ = vp_stats(img_c1, vp)
        d = diff_stats(img_c1, img_a, vp)[2]
        print("  contrast=1.8：std=%.2f（基线 %.2f）平均差=%.2f" % (s1, base_std, d))
        check("对比度拉高后亮度 std 上升", s1 > base_std + 0.5,
              "%.2f vs %.2f" % (s1, base_std))
        check("画面显著变化（视口平均差 > 3）", d > 3.0, "%.2f" % d)

    print("\n== C2 组：contrast=0.5 → 亮度直方图收窄（std 下降）==")
    shot_c2 = os.path.join(OUT_DIR, "cg_contrast_lo.png")
    _, _, img_c2, errs_c2 = run_once(
        exe, scene,
        dict(DBG, MYVK_EXPOSURE="1.0", MYVK_CG="1", MYVK_CG_CONTRAST="0.5"),
        shot_c2)
    check("零 Vulkan 验证层报错", not errs_c2, errs_c2[:2])
    if img_c2 is not None:
        m2, s2, _, _ = vp_stats(img_c2, vp)
        print("  contrast=0.5：std=%.2f（基线 %.2f）" % (s2, base_std))
        check("对比度压低后亮度 std 下降", s2 < base_std - 0.5,
              "%.2f vs %.2f" % (s2, base_std))

    # ============ D 组：饱和度归零 → 中性灰 ============
    print("\n== D 组：saturation=0 → 整幅图变中性灰（LUT 真的被采样）==")
    shot_d = os.path.join(OUT_DIR, "cg_gray.png")
    lines_d, _, img_d, errs_d = run_once(
        exe, scene,
        dict(DBG, MYVK_EXPOSURE="1.0", MYVK_CG="1", MYVK_CG_SATURATION="0"),
        shot_d)
    check("零 Vulkan 验证层报错", not errs_d, errs_d[:2])
    l_d = lut_lines(lines_d)
    check("日志里 saturation=0",
          bool(l_d) and abs(l_d[-1][5]) < 1e-3, l_d[-1][5] if l_d else None)
    if img_d is not None:
        _, _, rg, gb = vp_stats(img_d, vp)
        print("  saturation=0：视口 |R-G|=%.3f |G-B|=%.3f（基线 %.2f / %.2f）"
              % (rg, gb, base_rg, base_gb))
        check("视口近乎中性灰（|R-G| < 2）", rg < 2.0, "%.3f" % rg)
        check("视口近乎中性灰（|G-B| < 2）", gb < 2.0, "%.3f" % gb)
        check("去饱和显著改变了画面（基线是彩色的）",
              base_rg > 5.0 and base_gb > 5.0,
              "基线 %.2f/%.2f" % (base_rg, base_gb))

    # ============ E 组：混合权重单调 ============
    print("\n== E 组：blend 权重单调（半开的变化量落在 恒等 与 全开 之间）==")
    shot_e = os.path.join(OUT_DIR, "cg_blend_half.png")
    lines_e, _, img_e, errs_e = run_once(
        exe, scene,
        dict(DBG, MYVK_EXPOSURE="1.0", MYVK_CG="1", MYVK_CG_CONTRAST="1.8",
             MYVK_CG_BLEND="0.5"),
        shot_e)
    check("零 Vulkan 验证层报错", not errs_e, errs_e[:2])
    if img_e is not None and img_c1 is not None:
        d_half = diff_stats(img_e, img_a, vp)[2]
        d_full = diff_stats(img_c1, img_a, vp)[2]
        d_id = diff_stats(img_b, img_a, vp)[2]
        print("  变化量：恒等=%.4f 半开=%.3f 全开=%.3f" % (d_id, d_half, d_full))
        check("半开的变化量 > 恒等（确实应用了一部分）", d_half > d_id + 1.0,
              "%.3f vs %.4f" % (d_half, d_id))
        check("半开的变化量 < 全开（不是照单全收）", d_half < d_full * 0.95,
              "%.3f vs %.3f" % (d_half, d_full))

    print("\n结果: %s" % ("PASS" if not fails else "FAIL: " + ", ".join(fails)))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
