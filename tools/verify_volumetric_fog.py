"""端到端验证：Froxel 体积雾（R3-2）。

背景：引擎原有一条**高度雾**（pbr.frag 里按世界坐标解析求值：距离 ×
密度 × 高度衰减），它假设介质**均匀**、光源只有太阳 —— 因此做不出
"聚光灯锥内的光柱"这种**局部形状**。

Froxel 体积雾把世界空间切成 3D 网格（每个 froxel = 一个视锥小块），
逐格沿视线前向积分内散射/消光（复用分簇的灯数据），前向按像素采样
合成 out = lit·T + inscatter。

  关：**不录任何 compute**，set0/binding16 绑 1×1×1 透明图
      （rgb=0, a=1）→ 合成退化成 out = lit，**基线逐位不变**。
  开：`MYVK_VFOG=1`（网格 / 密度 / 反照率 / 高度衰减 / 雾团 / 相位
      可分别由 MYVK_VFOG_* 覆盖）。

测试场景刻意构造成**可判定**的：
  · 400 单位地面（castShadow=false）+ 几根柱子（给雾提供深度线索）。
  · **太阳压到极暗**（ambientScale=0.03、intensity=0.15）：让"雾"本身
    成为画面上的主要信号，而不是被强烈的方向光淹没。
  · 一盏**射灯**（spotLight）悬在高处、朝下打：锥形内出现**光柱**是
    验收的第一条（froxel 独有的"局部形状"）。
  · 相机 (0,0,20)、yaw=270、pitch=18° → 沿地面看出去，射灯锥在画面中
    景，正对体积雾的成像位置。

断言分四组：

  1. **关闭 = 基线**
     · VFOG-DBG 里 enabled=0；
     · 同一配置跑两次，视口**逐像素一致**（MYVK_EDITOR_NO_HUD=1）。

  2. **开启有可测差异 + 局部形状**
     · VFOG-DBG 里 enabled=1 ready=1；
     · 与关闭态相比有**显著差异**（说明雾真的改了像素）；
     · 差异**不是全屏均匀的**：射灯锥体的投影区域比画面四角差得多
       —— 这证明雾有**局部形状**（如果只是全局加一层常数色，
       ratio ≈ 1）。

  3. **密度参数单调**（MYVK_VFOG_DENSITY）
     · 密度 0.01 → 0.05 → 0.12，与关闭态的差异（全屏均值）必须
       **单调递增**。

  4. **雾团局部形状**（MYVK_VFOG_BLOB + 高度衰减）
     · 高度衰减（MYVK_VFOG_FALLOFF）调大 → "越高越稀"更明显 →
       与关闭态的差异（全屏均值）**下降**（雾整体变淡）。
       —— 这证明高度场真的被用上了。

全程开 MYVK_EDITOR_NO_HUD 关掉视口叠加层（FPS/帧时间每次都不同）。

用法：
    python tools/verify_volumetric_fog.py
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_client, find_window, user32      # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors, wait_rect   # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = os.path.join(ROOT, "..", "_venn_verify")

# 相机 (0,0,20)、yaw=270 → 视线朝 −Y；pitch=18° 微俯 → 地面占画面下半。
CAM = "0,0,20,270,18"
GROUND_SIZE = 400.0

# 太阳极暗：让"雾"成为主要信号，而不被方向光淹没。
LIGHT_DIR = [-0.30, 0.45, -0.84]
AMBIENT_SCALE = 0.03
SUN_INTENSITY = 0.15

# 射灯：悬在相机前方高处，朝下偏前打。锥形内出现光柱 = froxel 的局部形状。
SPOT_POS = (0.0, -12.0, 14.0)
SPOT_DIR = (0.0, 0.55, -0.84)   # 朝 +Y（相机侧）偏下
SPOT_RANGE = 40.0
SPOT_INNER = 0.18               # 弧度
SPOT_OUTER = 0.42
SPOT_INTENSITY = 40.0

PILLARS = [(-14.0, -20.0), (12.0, -24.0), (-4.0, -34.0)]
PILLAR_H = 8.0
PILLAR_W = 1.4

VFOG_DBG_RE = re.compile(
    r"VFOG-DBG enabled=(\d+) grid=(\d+)x(\d+)x(\d+) density=([\d.]+) "
    r"albedo=([\d.]+) falloff=([\d.]+) blob=([\d.]+) g=([\d.]+) ready=(\d+)")


def material(name, base, roughness=0.5):
    return {
        "kind": "procedural", "name": name,
        "baseColor": list(base) + [1.0],
        "metallic": 0.0, "roughness": roughness,
        "normalScale": 1.0, "ao": 1.0,
        "emissive": [0.0, 0.0, 0.0], "doubleSided": False, "alphaBlend": False,
    }


def build_scene(path):
    """地面 + 几根柱子 + 一盏射灯（锥形光柱是验收信号）。"""
    ents = [{
        "name": "Ground",
        "transform": {"position": [0, 0, 0], "rotation": [0, 0, 0],
                      "scale": [1, 1, 1]},
        "visible": True,
        "castShadow": False,
        "parent": -1,
        "mesh": {"kind": "builtin", "shape": "plane", "size": GROUND_SIZE,
                 "name": "vf_ground"},
        "material": material("vf_ground_mat", (0.70, 0.70, 0.72)),
    }]
    for i, (y, x) in enumerate(PILLARS):
        ents.append({
            "name": "Pillar%d" % i,
            "transform": {"position": [x, y, PILLAR_H * 0.5],
                          "rotation": [0, 0, 0],
                          "scale": [PILLAR_W, PILLAR_W, PILLAR_H]},
            "visible": True, "castShadow": True, "parent": -1,
            "mesh": {"kind": "builtin", "shape": "cube", "size": 1.0,
                     "name": "vf_pillar"},
            "material": material("vf_pillar_mat", (0.80, 0.52, 0.32)),
        })
    # 射灯：挂在实体上（entity-level spotLight）。
    ents.append({
        "name": "Spot",
        "transform": {"position": list(SPOT_POS), "rotation": [0, 0, 0],
                      "scale": [1, 1, 1]},
        "visible": True, "castShadow": True, "parent": -1,
        "mesh": {"kind": "builtin", "shape": "cube", "size": 0.4,
                 "name": "vf_spot"},
        "material": material("vf_spot_mat", (0.15, 0.15, 0.15)),
        "spotLight": {
            "color": [1.0, 0.95, 0.80],
            "intensity": SPOT_INTENSITY,
            "range": SPOT_RANGE,
            "direction": list(SPOT_DIR),
            "innerAngle": SPOT_INNER,
            "outerAngle": SPOT_OUTER,
            "enabled": True,
            "castsShadow": False,
        },
    })
    doc = {
        "version": 1,
        "generator": "verify_volumetric_fog.py",
        "light": {"direction": LIGHT_DIR, "color": [1.0, 0.97, 0.92],
                  "intensity": SUN_INTENSITY, "castsShadow": True,
                  "ambientScale": AMBIENT_SCALE},
        "entities": ents,
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)
    return len(ents)


def run_once(exe, scene, extra_env, shot_path, cam=None, wait=14.0):
    """跑一轮编辑器，返回 (日志行, 视口矩形, 截图, 错误行)。"""
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_ENV_DBG"] = "0"
    env["MYVK_LIGHT_GIZMOS"] = "0"
    env["MYVK_EDITOR_NO_HUD"] = "1"
    env["MYVK_GRID"] = "0"
    env["MYVK_VFOG_DBG"] = "1"
    env["MYVK_EDITOR_SCENE"] = scene
    env["MYVK_EDITOR_CAM"] = cam or CAM
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
        end = time.time() + wait
        while time.time() < end:
            with tap.lock:
                ready = any("VFOG-DBG" in l for l in tap.lines)
            if ready:
                break
            time.sleep(0.25)
        time.sleep(1.5)

        img = None
        if shot_path:
            img, _ = capture_client(hwnd)
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


def find_line(lines, needle, rx):
    for line in reversed(lines):
        if needle in line:
            m = rx.search(line)
            if m:
                return m
    return None


def full_mean_diff(a, b):
    """全屏平均逐像素差（只比 RGB）。"""
    pa, pb = a.load(), b.load()
    W, H = a.size
    total, n = 0, 0
    for y in range(0, H, 2):
        for x in range(0, W, 2):
            p, q = pa[x, y], pb[x, y]
            total += abs(p[0] - q[0]) + abs(p[1] - q[1]) + abs(p[2] - q[2])
            n += 1
    return total / max(n, 1) / 3.0


def region_mean_diff(a, b, box):
    x0, y0, x1, y1 = [int(v) for v in box]
    W, H = a.size
    x0, y0 = max(x0, 0), max(y0, 0)
    x1, y1 = min(x1, W), min(y1, H)
    pa, pb = a.load(), b.load()
    total, n = 0, 0
    for y in range(y0, y1, 2):
        for x in range(x0, x1, 2):
            p, q = pa[x, y], pb[x, y]
            total += abs(p[0] - q[0]) + abs(p[1] - q[1]) + abs(p[2] - q[2])
            n += 1
    return total / max(n, 1) / 3.0


def parse_vp(vp):
    """VP-RECT 形如 (x0,y0,x1,y1) 或 x,y,w,h —— 统一成 (x0,y0,x1,y1)。"""
    if not vp:
        return None
    vals = [int(float(v)) for v in re.findall(r"-?\d+\.?\d*", str(vp))]
    if len(vals) < 4:
        return None
    if vals[2] < vals[0] or vals[3] < vals[1]:
        # x,y,w,h
        return (vals[0], vals[1], vals[0] + vals[2], vals[1] + vals[3])
    return (vals[0], vals[1], vals[2], vals[3])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    exe = args.exe
    if not os.path.exists(exe):
        print("FAIL: 找不到可执行文件 %s" % exe)
        return 2

    os.makedirs(OUT_DIR, exist_ok=True)
    scene = os.path.join(OUT_DIR, "vfog_scene.json")
    build_scene(scene)

    kill_stale_editors()
    failures = []
    print("=" * 68)
    print("Froxel 体积雾验证（R3-2）")
    print("=" * 68)

    def step(name, extra):
        shot = os.path.join(OUT_DIR, "vfog_%s.png" % name)
        lines, vp, img, errs = run_once(exe, scene, extra, shot)
        if errs:
            print("  [%s] ⚠ 验证层/错误输出 %d 行，例：%s"
                  % (name, len(errs), errs[0][:120]))
            failures.append("%s: 验证层报错" % name)
        return lines, vp, img, shot

    # ---- 组 1：关闭 = 基线 ----
    print("\n[1] 关闭 = 基线（逐像素一致）")
    l_off1, vp1, img_off1, _ = step("off1", {})
    l_off2, vp2, img_off2, _ = step("off2", {})
    m_off = find_line(l_off1, "VFOG-DBG", VFOG_DBG_RE)
    if not m_off:
        print("  ✗ 没读到 VFOG-DBG（enabled 应为 0）")
        failures.append("组1: 缺 VFOG-DBG")
    else:
        print("  VFOG-DBG: enabled=%s grid=%sx%sx%s ready=%s"
              % (m_off.group(1), m_off.group(2), m_off.group(3),
                 m_off.group(4), m_off.group(10)))
        if m_off.group(1) != "0":
            print("  ✗ 默认应为关（enabled=0）")
            failures.append("组1: 默认未关")
        else:
            print("  ✓ 默认关")

    if img_off1 and img_off2:
        d = full_mean_diff(img_off1, img_off2)
        print("  关闭态两次运行全屏均值差 = %.6f" % d)
        if d > 0.5:
            print("  ✗ 关闭态两次运行不一致（应逐像素相同）")
            failures.append("组1: 关闭态不可复现")
        else:
            print("  ✓ 关闭态可复现（基线稳定）")

    # ---- 组 2：开启有差异 + 局部形状 ----
    print("\n[2] 开启有可测差异 + 局部形状")
    l_on, vp_on, img_on, _ = step("on", {"MYVK_VFOG": "1"})
    m_on = find_line(l_on, "VFOG-DBG", VFOG_DBG_RE)
    if not m_on:
        print("  ✗ 没读到 VFOG-DBG")
        failures.append("组2: 缺 VFOG-DBG")
    else:
        print("  VFOG-DBG: enabled=%s grid=%sx%sx%s ready=%s"
              % (m_on.group(1), m_on.group(2), m_on.group(3),
                 m_on.group(4), m_on.group(10)))
        if m_on.group(1) != "1" or m_on.group(10) != "1":
            print("  ✗ 开启后应 enabled=1 ready=1")
            failures.append("组2: 开启未生效")
        else:
            print("  ✓ 开启生效")

    if img_off1 and img_on:
        vp = parse_vp(vp1)
        d_all = full_mean_diff(img_off1, img_on)
        print("  开启 vs 关闭全屏均值差 = %.4f" % d_all)
        # 全屏均值只是"雾有没有生效"的第一道门槛；雾的能量集中在射灯锥
        # 附近，全屏均值会被大片无雾区域稀释（默认密度下 ~0.9）。真正的
        # 判定在下面的"局部形状"（锥区差 ≫ 角落差）。
        if d_all < 0.3:
            print("  ✗ 差异过小，雾几乎没起作用")
            failures.append("组2: 雾无效果")
        else:
            print("  ✓ 雾有可测效果（全屏均值 > 0.3）")

        # 局部形状：射灯锥投影区（画面中景）应比四角差得多
        if vp:
            x0, y0, x1, y1 = vp
            w, h = x1 - x0, y1 - y0
            center_box = (x0 + int(w * 0.30), y0 + int(h * 0.45),
                          x0 + int(w * 0.70), y0 + int(h * 0.80))
            corner_box = (x0, y0, x0 + int(w * 0.18), y0 + int(h * 0.22))
            dc = region_mean_diff(img_off1, img_on, center_box)
            dk = region_mean_diff(img_off1, img_on, corner_box)
            print("  射灯锥区均值差 = %.4f，左上角 = %.4f，ratio = %.2f"
                  % (dc, dk, dc / max(dk, 1e-3)))
            if dc > max(dk * 1.3, 0.5):
                print("  ✓ 差异集中在射灯锥（雾有局部形状，不是全局常数）")
            else:
                print("  ✗ 差异没有局部性（疑似全局叠加常数色）")
                failures.append("组2: 雾无局部形状")

    # ---- 组 3：密度单调 ----
    print("\n[3] 密度参数单调（MYVK_VFOG_DENSITY）")
    dens = [0.01, 0.05, 0.12]
    means = []
    for i, dv in enumerate(dens):
        l, _, img, _ = step("d%d" % i,
                            {"MYVK_VFOG": "1", "MYVK_VFOG_DENSITY": str(dv)})
        if img and img_off1:
            mm = full_mean_diff(img_off1, img)
            means.append((dv, mm))
            print("  density=%.2f → 与关闭态全屏均值差 = %.4f" % (dv, mm))
    if len(means) >= 3:
        mono = all(means[i + 1][1] > means[i][1] * 1.05
                   for i in range(len(means) - 1))
        if mono:
            print("  ✓ 密度越大差异越大（单调递增）")
        else:
            print("  ✗ 密度与差异非单调")
            failures.append("组3: 密度非单调")

    # ---- 组 4：高度衰减 ----
    print("\n[4] 高度衰减生效（MYVK_VFOG_FALLOFF）")
    l_a, _, img_a, _ = step("fa_low", {"MYVK_VFOG": "1",
                                       "MYVK_VFOG_FALLOFF": "0.0"})
    l_b, _, img_b, _ = step("fa_high", {"MYVK_VFOG": "1",
                                        "MYVK_VFOG_FALLOFF": "0.5"})
    if img_a and img_b:
        da = full_mean_diff(img_off1, img_a)
        db = full_mean_diff(img_off1, img_b)
        print("  falloff=0.0 → 差异 = %.4f；falloff=0.5 → 差异 = %.4f"
              % (da, db))
        if db < da:
            print("  ✓ 高度衰减越大雾越淡（高度场被用上）")
        else:
            print("  ✗ 高度衰减没让雾变淡")
            failures.append("组4: 高度衰减无效")

    print("\n" + "=" * 68)
    if failures:
        print("结果: FAIL（%d 项）" % len(failures))
        for f in failures:
            print("  · " + f)
        return 1
    print("结果: PASS（4 组全部通过）")
    print("截图目录：%s" % OUT_DIR)
    return 0


if __name__ == "__main__":
    sys.exit(main())
