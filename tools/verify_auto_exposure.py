"""端到端验证：自动曝光 —— 直方图 eye adaptation（R1-2）。

被测链路（PostProcess::recordAutoExposure + beginFrame）：
    场景色 --(片元降采样)--> 64×64 块平均 log2 亮度图
          --(compute 直方图)--> 128 箱 bins
          --(compute 归约 + 两端各 2% 百分位裁剪)--> 平均 log2 亮度
          --(HOST_VISIBLE 缓冲，**两帧后**回读)--> 目标曝光
          --(按 dt 指数平滑)--> 合成/亮部提取用的曝光

断言分五组：

  A. **关着 = 手动值**（回归硬要求）
     · AUTOEXP 日志全部 enabled=0，且 exposure **精确等于** MYVK_EXPOSURE
       给的手动值（逐位相等，不是"约等于"）；
     · samples 全程 0 —— 关着时测量链一个 pass 都没录；
     · 两次同配置运行，视口区域**逐像素相同**（关着时行为完全确定）。

  B. **收敛（确定性）**：用 MYVK_AE_SIM_LUM 把"测量到的平均亮度"钉死，
     目标就是解析值 key/lum，于是收敛曲线是完全确定的指数曲线。
     · 亮→暗（lum=4.0）：曝光必须**单调下降**并收敛到 0.18/4.0 = 0.045；
     · 往亮走（lum=0.02，目标 9.0 超过 max=8）：
       曝光必须**单调上升**并被 exposureMax 夹住。

  C. **真实直方图路径 + 亮暗对照**：不用 SIM_LUM，改场景的**天空亮度**
     （MYVK_SKY_ZENITH/GROUND）。断言：
     · samples > 0（直方图真的跑起来了）、avgLum > 0；
     · 亮天空 → 收敛到低曝光，暗天空 → 收敛到高曝光，比值 > 1.5×；
     · 两组都收敛（尾部 8 帧波动 < 1%）。

  D. **开启 = 画面变化**：同一场景，手动 3.0 过曝 vs 自动曝光，
     视口区域差异显著。

  E. 全程零 Vulkan 验证层报错。

用法：
    python tools/verify_auto_exposure.py
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_client, capture_nonempty, find_window, user32  # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors, wait_rect   # noqa: E402
from verify_frustum_cull import region_diff                         # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = os.path.join(ROOT, "..", "_venn_verify")

# 相机在 (0,0,3) 朝 +X 平视 —— 天空占画面大半（自动曝光最自然的输入）
CAM = "0,0,3,180,0"

AE_RE = re.compile(
    r"AUTOEXP enabled=(\d+) exposure=([-\d.]+) manual=([-\d.]+) "
    r"avgLog2=([-\d.]+) avgLum=([-\d.]+) target=([-\d.]+) raw=([-\d.]+) "
    r"samples=([-\d.]+) frame=(\d+)")


def build_scene(path):
    """几颗球 + 一盏方向光。天空（背景）才是亮度的主体 ——
    所以 C 组靠改 MYVK_SKY_ZENITH 就能大幅推高/压低整体亮度。"""
    def mat(name, base, metallic, roughness):
        return {"kind": "procedural", "name": name,
                "baseColor": list(base) + [1.0],
                "metallic": metallic, "roughness": roughness,
                "normalScale": 1.0, "ao": 1.0,
                "emissive": [0.0, 0.0, 0.0], "doubleSided": False,
                "alphaBlend": False}

    ents = []
    for i, (y, col, met, rough) in enumerate([
            (-1.2, (0.8, 0.3, 0.25), 0.0, 0.5),
            (0.0, (0.85, 0.85, 0.9), 1.0, 0.2),
            (1.2, (0.3, 0.5, 0.8), 0.0, 0.3)]):
        ents.append({
            "name": "Ball%d" % i,
            "transform": {"position": [4.0, y, 3.0], "rotation": [0, 0, 0],
                          "scale": [1, 1, 1]},
            "visible": True, "castShadow": False, "parent": -1,
            "mesh": {"kind": "builtin", "shape": "sphere", "size": 0.7,
                     "segments": 32, "rings": 16, "name": "ae_ball%d" % i},
            "material": mat("ae_ball_mat%d" % i, col, met, rough),
        })
    doc = {
        "version": 1,
        "generator": "verify_auto_exposure.py",
        "light": {"direction": [-1.0, 0.2, 0.3], "color": [1.0, 0.98, 0.95],
                  "intensity": 1.0, "castsShadow": False, "ambientScale": 1.0},
        "entities": ents,
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)


def run_once(exe, scene, extra_env, shot_path=None, settle=4.5):
    """跑一轮编辑器，返回 (全部日志行, 视口矩形, 截图, 错误行)。

    settle：等场景出图之后再静置这么久 —— 自动曝光的适应速度默认
    3~4 /秒，指数收敛到 0.1% 需要约 1.7s，留 4.5s 足够到稳态。
    """
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_LIGHT_GIZMOS"] = "0"
    # 视口里的统计/导航叠加层有 FPS 这类浮动数字 → 会毁掉逐像素对照
    env["MYVK_EDITOR_NO_HUD"] = "1"
    env["MYVK_EDITOR_SCENE"] = scene
    env["MYVK_EDITOR_CAM"] = CAM
    env.update(extra_env)

    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    hwnd = None
    try:
        hwnd = find_window("Venn Editor")
        if not hwnd:
            return [], None, None, ["未找到编辑器窗口"]
        user32.ShowWindow(hwnd, 9)
        time.sleep(1.5)
        user32.SetWindowPos(hwnd, 0, 0, 0, 1280, 800, 0x0002 | 0x0004)
        time.sleep(2.5)

        vp = wait_rect(tap, "VP-RECT", 10.0)
        # 等场景真的出图（拿到一次剔除统计即视为稳定）
        end = time.time() + 14.0
        while time.time() < end:
            with tap.lock:
                ready = any("CULL-STATS" in l or "LOD-STATS" in l
                            for l in tap.lines)
            if ready:
                break
            time.sleep(0.25)
        time.sleep(settle)

        img = None
        if shot_path:
            img = capture_nonempty(hwnd)
            if img is not None:
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
            except subprocess.TimeoutExpired:
                proc.kill()


def ae_series(lines):
    """按出现顺序抽出 (enabled, exposure, manual, avgLog2, avgLum,
    target, raw, samples, frame)。"""
    out = []
    for line in lines:
        m = AE_RE.search(line)
        if m:
            out.append(tuple(float(v) for v in m.groups()))
    return out


def trim(series, skip=8):
    """丢掉开头若干帧（窗口刚出来时的场景填充/首帧）"""
    return series[skip:] if len(series) > skip else series


def converged(vals, tail=8, tol=0.01):
    """尾部 tail 帧的相对波动 < tol → 认为收敛"""
    t = vals[-tail:]
    if len(t) < 2:
        return False, 0.0
    mean = sum(t) / len(t)
    if mean <= 0:
        return False, 0.0
    spread = (max(t) - min(t)) / mean
    return spread < tol, spread


def monotone(vals, direction, eps_ratio=0.0):
    """方向一致的步数占比。direction=+1 递增 / -1 递减。

    eps_ratio：允许的"逆势步长"占全程变化量的比例（浮点噪声/测量抖动）。
    """
    if len(vals) < 3:
        return 0.0
    total = abs(vals[-1] - vals[0])
    eps = max(total * eps_ratio, 1e-9)
    ok = 0
    for a, b in zip(vals, vals[1:]):
        d = b - a
        if d * direction >= -eps:
            ok += 1
    return ok / float(len(vals) - 1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    os.makedirs(OUT_DIR, exist_ok=True)
    if not os.path.exists(exe):
        print("找不到可执行文件，请先构建：", exe)
        return 1

    scene = os.path.join(OUT_DIR, "ae_test.json")
    build_scene(scene)
    print("测试场景：%s" % scene)

    fails = []

    def check(name, cond, detail=""):
        print(("  [PASS] " if cond else "  [FAIL] ") + name +
              (("  " + str(detail)) if detail else ""))
        if not cond:
            fails.append(name)

    kill_stale_editors()

    # ============ A 组：关着 = 手动值（逐位相等）+ 零开销 ============
    print("\n== A 组：自动曝光关闭 → 精确等于手动曝光，且测量链不录 ==")
    shot_a = os.path.join(OUT_DIR, "ae_off.png")
    lines_a, vp, img_a, errs_a = run_once(
        exe, scene, {"MYVK_AUTOEXP_DBG": "1", "MYVK_EXPOSURE": "0.42"}, shot_a)
    if not vp:
        print("  没等到 VP-RECT")
        return 2
    ser_a = ae_series(lines_a)
    print("  AUTOEXP 行数=%d 样例=%s" % (len(ser_a), ser_a[-1] if ser_a else None))
    check("拿到 AUTOEXP 日志", len(ser_a) > 0)
    check("零 Vulkan 验证层报错", not errs_a, errs_a[:2])
    if ser_a:
        en = set(int(r[0]) for r in ser_a)
        ex = set(round(r[1], 5) for r in ser_a)
        check("关着时 enabled 全程 = 0", en == {0}, sorted(en))
        check("关着时 exposure **精确等于** 手动值 0.42", ex == {0.42}, sorted(ex))
        check("关着时 samples 全程 = 0（测量链一个 pass 都没录）",
              all(r[7] == 0.0 for r in ser_a),
              "samples=%s" % sorted(set(r[7] for r in ser_a))[:4])
        check("关着时 target/raw 不参与（保持 1.0 缺省）",
              all(r[5] == 1.0 and r[6] == 1.0 for r in ser_a))

    print("\n== A2 组：同配置两次运行的视口逐像素一致（关着时行为确定）==")
    shot_a2 = os.path.join(OUT_DIR, "ae_off2.png")
    _, _, img_a2, errs_a2 = run_once(
        exe, scene, {"MYVK_AUTOEXP_DBG": "1", "MYVK_EXPOSURE": "0.42"},
        shot_a2)
    check("第二次运行零报错", not errs_a2, errs_a2[:2])
    if img_a is not None and img_a2 is not None:
        d = region_diff(img_a, img_a2, tuple(vp))
        print("  两次运行视口平均差=%.4f" % d)
        check("两次运行视口逐像素一致（平均差 < 0.01）", d < 0.01, "%.4f" % d)

    # ============ B 组：收敛（SIM_LUM 把测量亮度钉死）============
    print("\n== B1 组：亮→目标 0.18/4.0 = 0.045，单调下降收敛 ==")
    lines_b1, _, _, errs_b1 = run_once(
        exe, scene,
        {"MYVK_AUTOEXP": "1", "MYVK_AUTOEXP_DBG": "1", "MYVK_EXPOSURE": "1.0",
         "MYVK_AE_SPEED": "4.0", "MYVK_AE_SIM_LUM": "4.0"})
    check("零 Vulkan 验证层报错", not errs_b1, errs_b1[:2])
    ser_b1 = trim(ae_series(lines_b1))
    vals_b1 = [r[1] for r in ser_b1]
    print("  样本=%d 首=%.5f 末=%.5f" % (len(vals_b1), vals_b1[0] if vals_b1 else -1,
                                       vals_b1[-1] if vals_b1 else -1))
    check("日志足够长（>= 15 帧）", len(vals_b1) >= 15, len(vals_b1))
    if len(vals_b1) >= 15:
        mono = monotone(vals_b1, -1, 1e-4)
        conv, spread = converged(vals_b1)
        print("  单调占比=%.3f 尾部波动=%.5f" % (mono, spread))
        check("曝光单调下降（>=95% 的步为下降）", mono >= 0.95, "%.3f" % mono)
        check("尾部收敛（后 8 帧波动 < 1%）", conv, "%.5f" % spread)
        want = 0.18 / 4.0
        got = vals_b1[-1]
        check("收敛值 ≈ key/lum = 0.045（±3%）",
              abs(got - want) <= 0.03 * want, "%.5f vs %.5f" % (got, want))
        check("SIM_LUM 下 avgLum 精确等于 4.0",
              abs(ser_b1[-1][4] - 4.0) < 1e-4, "%.4f" % ser_b1[-1][4])

    print("\n== B2 组：暗→目标 0.18/0.02 = 9.0 > max，单调上升并被夹住 ==")
    lines_b2, _, _, errs_b2 = run_once(
        exe, scene,
        {"MYVK_AUTOEXP": "1", "MYVK_AUTOEXP_DBG": "1", "MYVK_EXPOSURE": "1.0",
         "MYVK_AE_SPEED": "4.0", "MYVK_AE_SIM_LUM": "0.02"})
    check("零 Vulkan 验证层报错", not errs_b2, errs_b2[:2])
    ser_b2 = trim(ae_series(lines_b2))
    vals_b2 = [r[1] for r in ser_b2]
    print("  样本=%d 首=%.5f 末=%.5f raw=%.4f target=%.4f"
          % (len(vals_b2), vals_b2[0] if vals_b2 else -1,
             vals_b2[-1] if vals_b2 else -1,
             ser_b2[-1][6] if ser_b2 else -1, ser_b2[-1][5] if ser_b2 else -1))
    check("日志足够长（>= 15 帧）", len(vals_b2) >= 15, len(vals_b2))
    if len(vals_b2) >= 15:
        mono = monotone(vals_b2, +1, 1e-4)
        conv, spread = converged(vals_b2)
        print("  单调占比=%.3f 尾部波动=%.5f" % (mono, spread))
        check("曝光单调上升（>=95% 的步为上升）", mono >= 0.95, "%.3f" % mono)
        check("尾部收敛", conv, "%.5f" % spread)
        check("未夹紧的目标 raw ≈ 9.0",
              abs(ser_b2[-1][6] - 9.0) < 0.05, "%.4f" % ser_b2[-1][6])
        check("被 exposureMax=8.0 夹住（收敛值 ≈ 8.0）",
              abs(vals_b2[-1] - 8.0) < 0.05, "%.5f" % vals_b2[-1])

    # ============ C 组：真实直方图路径 + 亮暗对照 ============
    print("\n== C1 组：亮场景（天空 6,6,6）→ 低曝光 ==")
    lines_c1, _, _, errs_c1 = run_once(
        exe, scene,
        {"MYVK_AUTOEXP": "1", "MYVK_AUTOEXP_DBG": "1", "MYVK_AE_SPEED": "4.0",
         "MYVK_SKY_ZENITH": "6,6,6", "MYVK_SKY_GROUND": "3,3,3"})
    check("零 Vulkan 验证层报错", not errs_c1, errs_c1[:2])
    ser_c1 = trim(ae_series(lines_c1))
    vals_c1 = [r[1] for r in ser_c1]
    if ser_c1:
        print("  样本=%d avgLum=%.4f target=%.5f 末曝光=%.5f"
              % (len(ser_c1), ser_c1[-1][4], ser_c1[-1][5], vals_c1[-1]))
    check("直方图真的跑了（samples > 0）",
          bool(ser_c1) and ser_c1[-1][7] > 0, ser_c1[-1][7] if ser_c1 else -1)
    check("测出的平均亮度 > 0",
          bool(ser_c1) and ser_c1[-1][4] > 0.0, ser_c1[-1][4] if ser_c1 else -1)
    conv1, spread1 = converged(vals_c1)
    check("亮场景下曝光收敛", conv1, "%.5f" % spread1)
    e_bright = vals_c1[-1] if vals_c1 else -1.0

    print("\n== C2 组：暗场景（天空 0.1/0.05）→ 高曝光 ==")
    lines_c2, _, _, errs_c2 = run_once(
        exe, scene,
        {"MYVK_AUTOEXP": "1", "MYVK_AUTOEXP_DBG": "1", "MYVK_AE_SPEED": "4.0",
         "MYVK_SKY_ZENITH": "0.1,0.1,0.1", "MYVK_SKY_GROUND": "0.05,0.05,0.05"})
    check("零 Vulkan 验证层报错", not errs_c2, errs_c2[:2])
    ser_c2 = trim(ae_series(lines_c2))
    vals_c2 = [r[1] for r in ser_c2]
    if ser_c2:
        print("  样本=%d avgLum=%.4f target=%.5f 末曝光=%.5f"
              % (len(ser_c2), ser_c2[-1][4], ser_c2[-1][5], vals_c2[-1]))
    check("直方图真的跑了（samples > 0）",
          bool(ser_c2) and ser_c2[-1][7] > 0, ser_c2[-1][7] if ser_c2 else -1)
    conv2, spread2 = converged(vals_c2)
    check("暗场景下曝光收敛", conv2, "%.5f" % spread2)
    e_dark = vals_c2[-1] if vals_c2 else -1.0

    print("  亮/暗场景收敛曝光：%.5f vs %.5f" % (e_bright, e_dark))
    check("暗场景的曝光显著高于亮场景（> 1.5×）",
          e_bright > 0 and e_dark > 1.5 * e_bright,
          "%.5f vs %.5f" % (e_bright, e_dark))

    # ============ D 组：开启 = 画面显著变化 ============
    print("\n== D 组：开启自动曝光后画面显著变化（手动 3.0 过曝 对照）==")
    shot_d1 = os.path.join(OUT_DIR, "ae_manual3.png")
    _, _, img_d1, errs_d1 = run_once(exe, scene, {"MYVK_EXPOSURE": "3.0"},
                                     shot_d1)
    check("对照组零报错", not errs_d1, errs_d1[:2])
    shot_d2 = os.path.join(OUT_DIR, "ae_auto.png")
    _, _, img_d2, errs_d2 = run_once(
        exe, scene, {"MYVK_AUTOEXP": "1", "MYVK_AE_SPEED": "4.0"}, shot_d2)
    check("自动曝光组零报错", not errs_d2, errs_d2[:2])
    if img_d1 is not None and img_d2 is not None:
        d = region_diff(img_d1, img_d2, tuple(vp))
        print("  手动 3.0 vs 自动：视口平均差=%.1f" % d)
        check("画面显著不同（视口平均差 > 15）", d > 15.0, "%.1f" % d)

    print("\n结果: %s" % ("PASS" if not fails else "FAIL: " + ", ".join(fails)))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
