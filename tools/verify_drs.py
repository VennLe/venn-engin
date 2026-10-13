"""端到端验证：动态分辨率 + TSR 式升采样（R1-4）。

被测链路：
    场景（HdrTarget / 深度 / 屏幕空间链 / TAA / bloom / 分簇）
        --(按 renderScale 缩小渲染)--> 合成 pass（对比度自适应的
        边缘感知升采样）--> 全分辨率的输出（交换链 / 离屏图）

沿用全套既有约定：**默认关 = renderScale 恒为 1.0 = 基线逐像素不变**。

断言分组：

  A. **默认关 = 基线**（回归硬要求）
     · 渲染分辨率 == 视口（输出）分辨率，没有缩小；
     · 两次同配置运行，视口区域**逐像素一致**。

  B. **手动 0.5 → 场景减半，输出不变**
     · 日志里渲染分辨率 ≈ 输出的一半（HdrTarget 与 PostProcess 两个
       数字必须一致 —— 它们各算一次，不一致就是 extent 算错了）；
     · 输出（截图）尺寸不变；
     · 8× 均值池化后与基线**结构一致**（不是花屏 / 错位）。

  C. **边缘感知升采样确实比纯双线性锐**（R1-4 的核心交付）
     · 同样的 0.5 缩放：sharpen=0.8 的 Laplacian（二阶差分）能量
       **高于** sharpen=0（纯双线性）；
     · 且不产生振铃：与基线的最大差没有爆炸。
     一阶梯度不行（"台阶摊成斜坡"总量守恒），必须用二阶差分。

  D. **自动模式按 GPU 帧耗时调缩放比**
     · 目标帧时间远小于实测 → 缩放比**下降**并收敛到 min；
     · 目标帧时间远大于实测 → 缩放比**上升**并收敛到 max。

  E. 全程零 Vulkan 验证层报错。

用法：
    python tools/verify_drs.py
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_nonempty, find_window, user32       # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors, wait_rect     # noqa: E402
from verify_local_shadow import diff_stats, to_np, vp_box             # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = os.path.join(ROOT, "..", "_venn_verify")

CAM = "0,0,3,180,0"

DRS_RE = re.compile(
    r"DRS scale=([\d.]+) prev=([\d.]+) ms=(-?[\d.]+) smooth=(-?[\d.]+) "
    r"render=(\d+)x(\d+) frame=(\d+)")
VIEW_RE = re.compile(r"(?:Viewport|DRS) render size -> (\d+)x(\d+)")


def build_scene(path):
    """天空 + 三颗球：既有平面（地面/球面）也有硬边（球轮廓），
    既能做结构对照也有足够的高频可测。"""
    def mat(name, base, metallic, roughness):
        return {"kind": "procedural", "name": name,
                "baseColor": list(base) + [1.0],
                "metallic": metallic, "roughness": roughness,
                "normalScale": 1.0, "ao": 1.0,
                "emissive": [0.0, 0.0, 0.0], "doubleSided": False,
                "alphaBlend": False}

    ents = []
    for i, (y, col) in enumerate([
            (-1.2, (0.90, 0.15, 0.10)),
            (0.0, (0.10, 0.75, 0.20)),
            (1.2, (0.12, 0.35, 0.95))]):
        ents.append({
            "name": "Ball%d" % i,
            "transform": {"position": [4.0, y, 3.0], "rotation": [0, 0, 0],
                          "scale": [1, 1, 1]},
            "visible": True, "castShadow": False, "parent": -1,
            "mesh": {"kind": "builtin", "shape": "sphere", "size": 0.75,
                     "segments": 32, "rings": 16, "name": "drs_ball%d" % i},
            "material": mat("drs_mat%d" % i, col, 0.0, 0.45),
        })
    doc = {
        "version": 1,
        "generator": "verify_drs.py",
        "light": {"direction": [-1.0, 0.2, 0.3], "color": [1.0, 0.98, 0.95],
                  "intensity": 1.0, "castsShadow": False, "ambientScale": 1.0},
        "entities": ents,
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)


def run_once(exe, scene, extra_env, shot_path=None, settle=2.5):
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_LIGHT_GIZMOS"] = "0"
    env["MYVK_EDITOR_NO_HUD"] = "1"
    env["MYVK_SHADER_VARIANTS"] = "1"
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
        user32.ShowWindow(hwnd, 9)
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
            if img is not None:
                img.save(shot_path)
            else:
                print("  [dbg] 抓图失败（窗口前台时序）")

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


def drs_lines(lines):
    out = []
    for l in lines:
        m = DRS_RE.search(l)
        if m:
            out.append((float(m.group(1)), float(m.group(2)), float(m.group(3)),
                        float(m.group(4)), int(m.group(5)), int(m.group(6)),
                        int(m.group(7))))
    return out


def render_sizes(lines):
    return [(int(m.group(1)), int(m.group(2)))
            for m in (VIEW_RE.search(l) for l in lines) if m]


def vp_rect_wh(vp):
    """VP-RECT 是 (x0,y0,x1,y1) 的**盒**（与 vp_box 同一约定）→ (w,h)。"""
    if vp is None:
        return (0, 0)
    if isinstance(vp, dict):
        return (int(vp.get("w", 0)), int(vp.get("h", 0)))
    seq = [float(v) for v in vp]
    if len(seq) >= 4:
        return (int(round(seq[2] - seq[0])), int(round(seq[3] - seq[1])))
    return (0, 0)


def vp_luma(img, vp):
    import numpy as np
    a = to_np(img).astype(float)
    x0, y0, x1, y1 = vp_box(vp)
    x0, y0 = max(0, x0), max(0, y0)
    x1, y1 = min(a.shape[1], x1), min(a.shape[0], y1)
    a = a[y0:y1, x0:x1]
    if a.size == 0:
        return None
    return 0.2126 * a[:, :, 0] + 0.7152 * a[:, :, 1] + 0.0722 * a[:, :, 2]


def laplacian_energy(img, vp):
    """视口内平均 |Laplacian|（5 点二阶差分）—— 高频/锐度度量。"""
    import numpy as np
    l = vp_luma(img, vp)
    if l is None or l.shape[0] < 3 or l.shape[1] < 3:
        return 0.0
    lap = (-4.0 * l[1:-1, 1:-1] + l[:-2, 1:-1] + l[2:, 1:-1]
           + l[1:-1, :-2] + l[1:-1, 2:])
    return float(np.abs(lap).mean())


def pooled_diff(img_a, img_b, vp, k=8):
    """视口内 k×k 均值池化后的平均绝对差 —— 只反映低频"结构"。"""
    import numpy as np
    la = vp_luma(img_a, vp)
    lb = vp_luma(img_b, vp)
    if la is None or lb is None:
        return -1.0
    h = min(la.shape[0], lb.shape[0]) // k * k
    w = min(la.shape[1], lb.shape[1]) // k * k
    if h == 0 or w == 0:
        return -1.0
    pa = la[:h, :w].reshape(h // k, k, w // k, k).mean(axis=(1, 3))
    pb = lb[:h, :w].reshape(h // k, k, w // k, k).mean(axis=(1, 3))
    return float(np.abs(pa - pb).mean())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    os.makedirs(OUT_DIR, exist_ok=True)
    if not os.path.exists(exe):
        print("找不到可执行文件，请先构建：", exe)
        return 1

    scene = os.path.join(OUT_DIR, "drs_test.json")
    build_scene(scene)
    print("测试场景：%s" % scene)

    fails = []

    def check(name, cond, detail=""):
        print(("  [PASS] " if cond else "  [FAIL] ") + name +
              (("  " + str(detail)) if detail else ""))
        if not cond:
            fails.append(name)

    kill_stale_editors()
    DBG = {"MYVK_DRS_DBG": "1", "MYVK_EXPOSURE": "1.0"}

    # ============ A 组：关着 = 基线 ============
    print("\n== A 组：DRS 默认关 → 渲染分辨率 == 输出分辨率，且两次运行一致 ==")
    shot_a = os.path.join(OUT_DIR, "drs_off.png")
    lines_a, vp, img_a, errs_a = run_once(exe, scene, dict(DBG), shot_a)
    if not vp:
        print("  没等到 VP-RECT")
        return 2
    vw, vh = vp_rect_wh(vp)
    print("  视口(输出)分辨率 = %dx%d" % (vw, vh))
    sizes_a = render_sizes(lines_a)
    print("  渲染分辨率日志 = %s" % (sizes_a[:4],))
    check("零 Vulkan 验证层报错", not errs_a, errs_a[:2])
    check("关着时没有任何 DRS 活动日志（scale 恒 1.0 不打印）",
          not [d for d in drs_lines(lines_a) if abs(d[0] - 1.0) > 1e-6],
          drs_lines(lines_a)[:2])
    if sizes_a:
        # ⚠ 取**最后一条**：编辑器启动早期会先按交换链尺寸建一次目标
        # （游戏模式的 950×648），面板布局稳定后才切到视口尺寸。
        rw, rh = sizes_a[-1]
        print("  稳定后的渲染分辨率 = %dx%d" % (rw, rh))
        check("关着时渲染分辨率 == 视口分辨率",
              abs(rw - vw) <= 2 and abs(rh - vh) <= 2,
              "%dx%d vs %dx%d" % (rw, rh, vw, vh))

    shot_a2 = os.path.join(OUT_DIR, "drs_off2.png")
    _, _, img_a2, errs_a2 = run_once(exe, scene, dict(DBG), shot_a2)
    check("第二次运行零报错", not errs_a2, errs_a2[:2])
    if img_a is not None and img_a2 is not None:
        cnt, mx, mean, _ = diff_stats(img_a, img_a2, vp)
        print("  两次运行：>18 像素=%d 最大差=%d 平均差=%.4f" % (cnt, mx, mean))
        check("关着时两次运行视口逐像素一致（平均差 < 0.01）", mean < 0.01,
              "%.4f" % mean)

    if img_a is None:
        print("  基线截图抓不到 —— 无法继续像素断言")
        print("\n结果: FAIL: 基线截图")
        return 1

    # ============ B 组：手动 0.5 ============
    print("\n== B 组：手动 renderScale=0.5 → 场景减半、输出不变 ==")
    shot_b = os.path.join(OUT_DIR, "drs_half.png")
    env_b = dict(DBG, MYVK_DRS="1", MYVK_DRS_AUTO="0", MYVK_RENDER_SCALE="0.5")
    lines_b, _, img_b, errs_b = run_once(exe, scene, env_b, shot_b)
    check("零 Vulkan 验证层报错", not errs_b, errs_b[:2])
    sizes_b = render_sizes(lines_b)
    print("  渲染分辨率日志 = %s" % (sizes_b[:4],))
    db = drs_lines(lines_b)
    print("  DRS 行数=%d 样例=%s" % (len(db), db[-1] if db else None))
    check("拿到 DRS 日志", len(db) > 0)
    if db:
        check("DRS 日志里 scale ≈ 0.5",
              abs(db[-1][0] - 0.5) < 1e-3, db[-1][0])
    if sizes_b:
        rw, rh = sizes_b[-1]
        aw, ah = sizes_a[-1] if sizes_a else (vw, vh)
        check("渲染分辨率 ≈ 基准（1.0x）的一半",
              abs(rw - aw / 2) <= 4 and abs(rh - ah / 2) <= 4,
              "%dx%d vs %dx%d（基准 %dx%d）" % (rw, rh, aw // 2, ah // 2, aw, ah))
    if img_b is not None:
        check("输出截图尺寸不变（仍是视口分辨率）",
              img_b.size == img_a.size, "%s vs %s" % (img_b.size, img_a.size))
        struct = pooled_diff(img_b, img_a, vp, k=8)
        full = diff_stats(img_b, img_a, vp)[2]
        print("  0.5x vs 基线：8×池化结构差=%.3f 逐像素平均差=%.2f"
              % (struct, full))
        check("低频结构一致（8×池化平均差 < 12）", 0.0 <= struct < 12.0,
              "%.3f" % struct)
        check("画面确实变了（逐像素平均差 > 0.5）", full > 0.5, "%.2f" % full)

    # ============ C 组：边缘感知升采样 vs 纯双线性 ============
    print("\n== C 组：0.5 缩放下，边缘感知升采样比纯双线性锐 ==")
    shot_c0 = os.path.join(OUT_DIR, "drs_half_bilinear.png")
    _, _, img_c0, errs_c0 = run_once(
        exe, scene, dict(env_b, MYVK_DRS_SHARP="0"), shot_c0)
    check("零 Vulkan 验证层报错", not errs_c0, errs_c0[:2])
    shot_c8 = os.path.join(OUT_DIR, "drs_half_sharp.png")
    _, _, img_c8, errs_c8 = run_once(
        exe, scene, dict(env_b, MYVK_DRS_SHARP="0.8"), shot_c8)
    check("零 Vulkan 验证层报错", not errs_c8, errs_c8[:2])
    if img_c0 is not None and img_c8 is not None:
        l0 = laplacian_energy(img_c0, vp)
        l8 = laplacian_energy(img_c8, vp)
        lb = laplacian_energy(img_a, vp)
        print("  Laplacian 能量：基线(1.0x)=%.3f 双线性=%.3f 边缘感知=%.3f"
              % (lb, l0, l8))
        check("边缘感知升采样的高频能量 > 纯双线性", l8 > l0 * 1.02,
              "%.3f vs %.3f" % (l8, l0))
        mx8 = diff_stats(img_c8, img_a, vp)[1]
        print("  边缘感知 vs 基线：最大差=%d（抗振铃阈值 255）" % mx8)
        check("边缘感知没有振铃/溢出（最大差 < 255）", mx8 < 255, mx8)

    # ============ D 组：自动模式 ============
    print("\n== D 组：自动模式——目标帧时间偏小 → 缩放比下降到 min ==")
    shot_d = os.path.join(OUT_DIR, "drs_auto_down.png")
    env_d = dict(DBG, MYVK_DRS="1", MYVK_DRS_AUTO="1",
                 MYVK_DRS_TARGET_MS="0.01", MYVK_DRS_MIN="0.5",
                 MYVK_DRS_MAX="1.0")
    lines_d, _, _, errs_d = run_once(exe, scene, env_d, shot_d, settle=9.0)
    check("零 Vulkan 验证层报错", not errs_d, errs_d[:2])
    dd = drs_lines(lines_d)
    if dd:
        scales = [x[0] for x in dd]
        print("  scale 曲线：首=%.3f 尾=%.3f min=%.3f（%d 行）"
              % (scales[0], scales[-1], min(scales), len(scales)))
        print("  末尾样例 scale/ms/smooth = %.3f / %.3f / %.3f"
              % (dd[-1][0], dd[-1][2], dd[-1][3]))
        check("自动模式把缩放比压到了 min(0.5)", abs(scales[-1] - 0.5) < 1e-3,
              scales[-1])
        check("单调不上升（下降过程）", scales[-1] <= scales[0] + 1e-6,
              "%.3f -> %.3f" % (scales[0], scales[-1]))
    else:
        check("拿到自动模式的 DRS 日志", False)

    print("\n== D2：目标帧时间偏大 → 缩放比上升到 max ==")
    shot_d2 = os.path.join(OUT_DIR, "drs_auto_up.png")
    env_d2 = dict(DBG, MYVK_DRS="1", MYVK_DRS_AUTO="1",
                  MYVK_DRS_TARGET_MS="500", MYVK_RENDER_SCALE="0.5",
                  MYVK_DRS_MIN="0.5", MYVK_DRS_MAX="1.0")
    lines_d2, _, _, errs_d2 = run_once(exe, scene, env_d2, shot_d2, settle=9.0)
    check("零 Vulkan 验证层报错", not errs_d2, errs_d2[:2])
    dd2 = drs_lines(lines_d2)
    if dd2:
        scales2 = [x[0] for x in dd2]
        print("  scale 曲线：首=%.3f 尾=%.3f max=%.3f（%d 行）"
              % (scales2[0], scales2[-1], max(scales2), len(scales2)))
        check("自动模式把缩放比拉到了 max(1.0)", abs(scales2[-1] - 1.0) < 1e-3,
              scales2[-1])
        check("单调不下降（上升过程）", scales2[-1] >= scales2[0] - 1e-6,
              "%.3f -> %.3f" % (scales2[0], scales2[-1]))
    else:
        check("拿到自动模式的 DRS 日志", False)

    print("\n结果: %s" % ("PASS" if not fails else "FAIL: " + ", ".join(fails)))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
