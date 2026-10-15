"""端到端验证：光源修饰（R3-3）—— 区域光 / IES / light function。

三种修饰都挂在**射灯**上（它们共享灯的局部正交基，只有射灯有方向）。
每灯的修饰记录单独走一条并行 SSBO（set0/binding17），纹理走
sampler2DArray（binding18，cookie 与 IES 剖面共用一张数组：

  * light function（cookie）：把片元方向投影到灯锥截面 → [0,1]² 的 uv
  * IES 光域网：柱面展开，u = 水平角，v = 竖直角（角度 → 强度）

三个总开关默认**关**：`MYVK_COOKIE` / `MYVK_IES` / `MYVK_AREA_LIGHT`。
关的时候逐灯记录全零 → 着色器走原始光照路径，**基线逐位一致**。

测试场景刻意构造成"射灯光池正好在画面里"：
  · 400 单位地面（castShadow=false）+ 几根柱子
  · 太阳压到极暗（强度 0.06、环境 0.02）→ 光池是画面主信号
  · 一盏射灯从高处垂直向下打（pool 落在可见地面区）

断言分五组：

  1. **关闭 = 基线**：LIGHTX-DBG 三个 On 全 0；同一配置跑两次逐像素一致。
  2. **cookie**：开启后有显著差异；差异**局部**在光池内；沿光池扫描线
     的差值呈**周期性振荡**（网格明暗线）—— 证明是"投影贴图"而不是
     一层均匀调暗。
  3. **IES**：开启后差异显著；`ies:narrow` 把光池**收窄** → 亮像素数下降。
  4. **区域光**：开启后差异显著；面积**单调**（越大 → 与关的差异越大）。
  5. **名字回退**：IES 名字拼错时该灯退化成"无修饰"（LIGHTX-DBG ies=0），
     画面回到基线 —— 证明名字解析失败不会污染渲染。
  6. **.ies 文件加载**：`MYVK_IES_FILE=name:path` 把 LM-63 文件解析后追加
     成新纹理层（LIGHTX-DBG nodes 7 → 8），且该层真的被灯用上。

用法：
    python tools/verify_light_modifiers.py
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import (capture_client, capture_nonempty, find_window,
                            user32)                                # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors, wait_rect   # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = os.path.join(ROOT, "..", "_venn_verify")

CAM = "0,0,20,270,26"
GROUND_SIZE = 400.0

SUN_DIR = [-0.30, 0.45, -0.84]
SUN_INTENSITY = 0.06
AMBIENT_SCALE = 0.02

SPOT_POS = (0.0, -30.0, 16.0)
SPOT_DIR = (0.0, 0.0, -1.0)
SPOT_RANGE = 80.0
SPOT_INNER = 0.34
SPOT_OUTER = 0.70
SPOT_INTENSITY = 2500.0

PILLARS = [(-14.0, -20.0), (12.0, -24.0), (-4.0, -34.0)]
PILLAR_H = 8.0
PILLAR_W = 1.4

LIGHTX_RE = re.compile(
    r"LIGHTX-DBG lights=(\d+) ies=(\d+) cookie=(\d+) area=(\d+) "
    r"nodes=(\d+) iesOn=(\d+) cookieOn=(\d+) areaOn=(\d+)")


def material(name, base, roughness=0.5):
    return {
        "kind": "procedural", "name": name,
        "baseColor": list(base) + [1.0],
        "metallic": 0.0, "roughness": roughness,
        "normalScale": 1.0, "ao": 1.0,
        "emissive": [0.0, 0.0, 0.0], "doubleSided": False, "alphaBlend": False,
    }


def build_scene(path, ies="ies:narrow", cookie="cookie:grid",
                area_w=10.0, area_h=10.0):
    ents = [{
        "name": "Ground",
        "transform": {"position": [0, 0, 0], "rotation": [0, 0, 0],
                      "scale": [1, 1, 1]},
        "visible": True, "castShadow": False, "parent": -1,
        "mesh": {"kind": "builtin", "shape": "plane", "size": GROUND_SIZE,
                 "name": "lx_ground"},
        "material": material("lx_ground_mat", (0.70, 0.70, 0.72)),
    }]
    for i, (y, x) in enumerate(PILLARS):
        ents.append({
            "name": "Pillar%d" % i,
            "transform": {"position": [x, y, PILLAR_H * 0.5],
                          "rotation": [0, 0, 0],
                          "scale": [PILLAR_W, PILLAR_W, PILLAR_H]},
            "visible": True, "castShadow": True, "parent": -1,
            "mesh": {"kind": "builtin", "shape": "cube", "size": 1.0,
                     "name": "lx_pillar"},
            "material": material("lx_pillar_mat", (0.80, 0.52, 0.32)),
        })
    ents.append({
        "name": "Spot",
        "transform": {"position": list(SPOT_POS), "rotation": [0, 0, 0],
                      "scale": [1, 1, 1]},
        "visible": True, "castShadow": False, "parent": -1,
        "mesh": {"kind": "builtin", "shape": "cube", "size": 0.5,
                 "name": "lx_spot"},
        "material": material("lx_spot_mat", (0.10, 0.10, 0.10)),
        "spotLight": {
            "color": [1.0, 0.95, 0.85],
            "intensity": SPOT_INTENSITY,
            "range": SPOT_RANGE,
            "direction": list(SPOT_DIR),
            "innerAngle": SPOT_INNER,
            "outerAngle": SPOT_OUTER,
            "enabled": True,
            "castsShadow": False,
            "areaWidth": area_w,
            "areaHeight": area_h,
            "iesProfile": ies,
            "lightFunction": cookie,
        },
    })
    doc = {
        "version": 1,
        "generator": "verify_light_modifiers.py",
        "light": {"direction": SUN_DIR, "color": [1.0, 0.97, 0.92],
                  "intensity": SUN_INTENSITY, "castsShadow": True,
                  "ambientScale": AMBIENT_SCALE},
        "entities": ents,
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)
    return len(ents)


def run_once(exe, scene, extra_env, shot_path):
    """跑一轮编辑器，返回 (日志行, 视口矩形, RGB ndarray, 错误行)。"""
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_LIGHT_GIZMOS"] = "0"
    env["MYVK_EDITOR_NO_HUD"] = "1"
    env["MYVK_GRID"] = "0"
    env["MYVK_LIGHTX_DBG"] = "1"
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
        # 等 LIGHTX-DBG 出现（说明灯已上传、渲染跑了几帧）
        end = time.time() + 14.0
        while time.time() < end:
            with tap.lock:
                ready = any("LIGHTX-DBG" in l for l in tap.lines)
            if ready:
                break
            time.sleep(0.25)
        time.sleep(1.5)

        img = None
        if shot_path:
            # 窗口偶尔还没铺开（client 0×0）→ 直接 save 会抛
            # "cannot write empty image"。capture_nonempty 内部重试 12×0.4s，
            # 外层再包 3 轮兜底（回归连跑时机器负载高，一次 4.8s 可能不够）。
            shot = None
            for _try in range(3):
                shot = capture_nonempty(hwnd)
                if shot is not None:
                    break
                time.sleep(0.8)
            if shot is None:
                print("  [warn] 抓图为空（已重试 3 轮）—— 本步像素断言不可用")
            else:
                shot.save(shot_path)
                img = np.asarray(shot.convert("RGB")).astype("float32")

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


def parse_vp(vp):
    if not vp:
        return None
    vals = [int(float(v)) for v in re.findall(r"-?\d+\.?\d*", str(vp))]
    if len(vals) < 4:
        return None
    if vals[2] < vals[0] or vals[3] < vals[1]:
        return (vals[0], vals[1], vals[0] + vals[2], vals[1] + vals[3])
    return (vals[0], vals[1], vals[2], vals[3])


def luma(a):
    return a[:, :, 0] * 0.299 + a[:, :, 1] * 0.587 + a[:, :, 2] * 0.114


def find_line(lines, needle, rx):
    for line in reversed(lines):
        if needle in line:
            m = rx.search(line)
            if m:
                return m
    return None


def full_mean_diff(a, b):
    la, lb = luma(a), luma(b)
    return float(np.abs(la - lb).mean())


def bright_bbox(a, thr=200.0):
    """亮区（光池）包围盒。"""
    L = luma(a)
    ys, xs = np.where(L > thr)
    if len(xs) == 0:
        return None
    return (int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1)


def bright_count(a, thr=180.0):
    return int((luma(a) > thr).sum())


def count_oscillations(a, b, y, x0, x1, band=2.0):
    """沿扫描线 y、x∈[x0,x1) 统计 (a-b) 亮度差围绕其均值穿越的次数。

    网格型 cookie 会在光池里造成周期性的暗线 → 多次穿越。
    均匀调暗只会造成一次整体偏移 → 穿越 ≈ 0。
    """
    la = luma(a)[y, x0:x1]
    lb = luma(b)[y, x0:x1]
    d = la - lb
    m = d.mean()
    state = 0
    crossings = 0
    for v in d:
        z = v - m
        if z > band:
            if state == -1:
                crossings += 1
            state = 1
        elif z < -band:
            if state == 1:
                crossings += 1
            state = -1
    return crossings


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    args = ap.parse_args()

    exe = args.exe
    if not os.path.exists(exe):
        print("FAIL: 找不到可执行文件 %s" % exe)
        return 2

    os.makedirs(OUT_DIR, exist_ok=True)
    failures = []
    print("=" * 68)
    print("光源修饰验证（R3-3：区域光 / IES / light function）")
    print("=" * 68)

    kill_stale_editors()

    def shot_for(name):
        return os.path.join(OUT_DIR, "lx_%s.png" % name)

    def step(name, extra, scene):
        lines, vp, img, errs = run_once(exe, scene, extra, shot_for(name))
        if errs:
            print("  [%s] ⚠ 验证层/错误输出 %d 行，例：%s"
                  % (name, len(errs), errs[0][:120]))
            failures.append("%s: 验证层报错" % name)
        m = find_line(lines, "LIGHTX-DBG", LIGHTX_RE)
        return lines, vp, img, m

    scene = os.path.join(OUT_DIR, "lx_scene.json")
    build_scene(scene)

    # ---- 组 1：关闭 = 基线 ----
    print("\n[1] 关闭 = 基线（逐像素一致）")
    l1, vp, img_off1, m1 = step("off1", {}, scene)
    l2, _, img_off2, _ = step("off2", {}, scene)
    if not m1:
        print("  ✗ 没拿到 LIGHTX-DBG")
        failures.append("组1: 无诊断输出")
    else:
        ies_on, ck_on, ar_on = int(m1.group(6)), int(m1.group(7)), int(m1.group(8))
        print("  LIGHTX-DBG iesOn=%d cookieOn=%d areaOn=%d (期望全 0)"
              % (ies_on, ck_on, ar_on))
        if ies_on or ck_on or ar_on:
            print("  ✗ 默认竟然开着修饰")
            failures.append("组1: 默认未关闭")
        else:
            print("  ✓ 默认三修饰全关")

    vpb = parse_vp(vp)
    if img_off1 is not None and img_off2 is not None:
        d = full_mean_diff(img_off1, img_off2)
        print("  两次关闭态全屏均值差 = %.6f" % d)
        if d > 0.0001:
            print("  ✗ 关闭态两次不一致（基线不稳）")
            failures.append("组1: 基线不稳定")
        else:
            print("  ✓ 关闭态逐像素一致")

    if img_off1 is None:
        print("FAIL: 没抓到截图")
        return 2

    # 视口裁剪（把编辑器面板切掉）
    if vpb:
        x0, y0, x1, y1 = vpb
    else:
        x0, y0, x1, y1 = 268, 118, 790, 628
    def crop(im):
        return im[y0:y1, x0:x1]

    off = crop(img_off1)
    bb = bright_bbox(off)
    print("  光池包围盒 = %s" % (bb,))
    if bb is None:
        print("  ✗ 画面里找不到光池，测试场景有问题")
        failures.append("场景: 无光池")
        return 2

    # ---- 组 2：cookie（投影贴图）----
    print("\n[2] light function（cookie:grid）")
    _, _, im_ck, mck = step("cookie", {"MYVK_COOKIE": "1"}, scene)
    if not (mck and int(mck.group(3)) == 1):
        print("  ✗ LIGHTX-DBG 未显示 cookie=1")
        failures.append("组2: cookie 未生效")
    ck = crop(im_ck)
    d_ck = full_mean_diff(off, ck)
    print("  开启 vs 关闭全屏均值差 = %.4f" % d_ck)
    if d_ck < 1.0:
        print("  ✗ 差异过小，cookie 没起作用")
        failures.append("组2: cookie 无效果")
    else:
        print("  ✓ cookie 有可测效果")

    # 周期性：沿光池中线扫一行，数差值穿越次数
    cy = (bb[1] + bb[3]) // 2
    cross = count_oscillations(off, ck, cy, bb[0], bb[2])
    print("  光池中线差值振荡穿越次数 = %d（网格应有多次）" % cross)
    if cross >= 4:
        print("  ✓ 差值呈周期性（投影图案，不是均匀调暗）")
    else:
        print("  ✗ 差值没有周期性（疑似只做了整体调暗）")
        failures.append("组2: 无投影图案")

    # 局部性：光池内的差异应远大于画面暗角
    h, w = off.shape[:2]
    corner = (slice(2, int(h * 0.12)), slice(2, int(w * 0.12)))
    dc_pool = float(np.abs(luma(off[bb[1]:bb[3], bb[0]:bb[2]])
                           - luma(ck[bb[1]:bb[3], bb[0]:bb[2]])).mean())
    dc_corner = float(np.abs(luma(off[corner]) - luma(ck[corner])).mean())
    print("  光池内均值差 = %.3f，左上角 = %.3f" % (dc_pool, dc_corner))
    if dc_pool > dc_corner + 1.0:
        print("  ✓ 差异集中在光池内（局部性正确）")
    else:
        print("  ✗ 差异没有局部性")
        failures.append("组2: 无局部性")

    # ---- 组 3：IES ----
    print("\n[3] IES 光域网（ies:narrow 收窄光束）")
    _, _, im_ies, mies = step("ies", {"MYVK_IES": "1"}, scene)
    if not (mies and int(mies.group(2)) == 1):
        print("  ✗ LIGHTX-DBG 未显示 ies=1")
        failures.append("组3: IES 未生效")
    ie = crop(im_ies)
    d_ies = full_mean_diff(off, ie)
    b_off, b_ies = bright_count(off), bright_count(ie)
    print("  开启 vs 关闭全屏均值差 = %.4f" % d_ies)
    print("  亮像素数：关闭 = %d，IES = %d" % (b_off, b_ies))
    if d_ies < 1.0:
        print("  ✗ 差异过小，IES 没起作用")
        failures.append("组3: IES 无效果")
    elif b_ies >= b_off:
        print("  ✗ 窄光束没有收窄亮区")
        failures.append("组3: 亮区未收窄")
    else:
        print("  ✓ IES 收窄了光池（%.0f%% → %.0f%%）"
              % (100.0 * b_off / max(b_off, 1), 100.0 * b_ies / max(b_off, 1)))

    # ---- 组 4：区域光（面积单调）----
    print("\n[4] 区域光（面积越大差异越强）")
    diffs = []
    for s in (4.0, 10.0, 22.0):
        sc = os.path.join(OUT_DIR, "lx_scene_a%d.json" % int(s))
        build_scene(sc, area_w=s, area_h=s)
        tag = "area%d" % int(s)
        _, _, im_a, ma = step(tag, {"MYVK_AREA_LIGHT": "1"}, sc)
        if not (ma and int(ma.group(4)) == 1):
            print("  ✗ LIGHTX-DBG 未显示 area=1 (size=%g)" % s)
            failures.append("组4: 区域光未生效")
            continue
        da = full_mean_diff(off, crop(im_a))
        diffs.append((s, da))
        print("  面积 %.0f×%.0f：与关闭态差异 = %.4f" % (s, s, da))
    if len(diffs) == 3:
        ds = [d for _, d in diffs]
        if ds[0] < ds[1] < ds[2]:
            print("  ✓ 面积单调（%.2f < %.2f < %.2f）" % tuple(ds))
        else:
            print("  ✗ 面积不单调")
            failures.append("组4: 面积不单调")
    if diffs and diffs[0][1] < 1.0:
        print("  ✗ 区域光差异过小")
        failures.append("组4: 区域光无效果")

    # ---- 组 5：未知名字回退 ----
    print("\n[5] 名字回退（IES 名字拼错 → 退化成无修饰）")
    bad = os.path.join(OUT_DIR, "lx_scene_bad.json")
    build_scene(bad, ies="ies:does-not-exist")
    _, _, im_bad, mbad = step("badname", {"MYVK_IES": "1"}, bad)
    if not mbad:
        print("  ✗ 没拿到 LIGHTX-DBG")
        failures.append("组5: 无诊断输出")
    else:
        n_ies = int(mbad.group(2))
        d_bad = full_mean_diff(off, crop(im_bad))
        print("  LIGHTX-DBG ies=%d（期望 0），与基线差异 = %.6f" % (n_ies, d_bad))
        if n_ies != 0 or d_bad > 0.0005:
            print("  ✗ 拼错的名字没被正确回退")
            failures.append("组5: 名字回退失败")
        else:
            print("  ✓ 拼错名字退化成基线")

    # ---- 组 6：.ies 文件加载 ----
    print("\n[6] .ies 文件加载（LM-63 解析 → 追加纹理层）")
    ies_path = os.path.join(OUT_DIR, "lx_test.ies")
    # 一个最小但合法的 LM-63（Type C）。轴心最亮、30° 之外迅速衰减。
    with open(ies_path, "w", encoding="ascii") as f:
        f.write("IESNA:LM-63-2002\n")
        f.write("[TEST] venn-verify\n")
        f.write("[TESTLAB] venn\n")
        f.write("TILT=NONE\n")
        f.write("1 1000 1 4 2 1 1 0 0 0\n")
        f.write("0 30 60 90\n")          # 竖直角（度）
        f.write("0 180\n")               # 水平角（度）
        f.write("1000 500 100 0\n")      # h=0 的 4 个竖直样本
        f.write("1000 500 100 0\n")      # h=180
    sc6 = os.path.join(OUT_DIR, "lx_scene_ies.json")
    build_scene(sc6, ies="myies")
    _, _, im_f, mf = step("iesfile",
                          {"MYVK_IES": "1",
                           "MYVK_IES_FILE": "myies:%s" % ies_path}, sc6)
    if not mf:
        print("  ✗ 没拿到 LIGHTX-DBG")
        failures.append("组6: 无诊断输出")
    else:
        nodes = int(mf.group(5))
        n_ies = int(mf.group(2))
        d_f = full_mean_diff(off, crop(im_f))
        print("  LIGHTX-DBG nodes=%d（期望 8 = 7 内建 + 1 加载）ies=%d，差异 = %.4f"
              % (nodes, n_ies, d_f))
        if nodes != 8:
            print("  ✗ IES 文件没被加载进纹理库")
            failures.append("组6: IES 文件未加载")
        elif n_ies != 1 or d_f < 1.0:
            print("  ✗ 加载的 IES 没被用上")
            failures.append("组6: 加载的 IES 未生效")
        else:
            print("  ✓ .ies 解析并生效")

    # ---- 结论 ----
    print("\n" + "=" * 68)
    if failures:
        print("结果: FAIL（%d 项）" % len(failures))
        for f in failures:
            print("  · " + f)
        return 1
    print("结果: PASS（6 组全部通过）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
