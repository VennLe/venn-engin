"""端到端验证：方向光阴影硬化三件套（R3-1）。

背景：CSM（4 级级联 @2048）已经能画对阴影，但有三类"观感"问题：

  ① **级联接缝**：级联交界处"本级的最后几个纹素"与"下一级的头几个纹素"
     分辨率不同，直接切换会在屏幕上留下一条亮线/硬边。
  ② **相机移动时阴影游动**：球心连续移动 → 采样网格相对纹素连续滑动 →
     整片阴影边缘亚像素级抖动。
  ③ **接触区漏光**：PCF 核半径是常数（只随级联号增长），物体与地面接触
     的地方被过度模糊，亮边从接缝处渗出来。

三件套的落点：

  · **texel snapping（②）**  `ShadowPass::updateCascades`：球心在光空间
    量化到纹素网格（把差量补回 view[3][0/1]）。开关 `MYVK_SHADOW_SNAP`，
    **默认 1 = 既有行为**（关掉只为做 A/B 对照）。
  · **级联间混合带（①）**  `pbr.frag::shadowFactor`：接收点落在"本级远端
    最后 band 比例"内时，同时采样下一级并按深度线性混合。
    `MYVK_SHADOW_BLEND`（占比，0 = 关闭 = 基线）。
  · **接触硬化（③）**  `pbr.frag::shadowFactor`：按接收点在级联内的
    相对视深度缩放 PCF 核半径 —— 近端收（锐利、不漏光）、远端放（变软）。
    `MYVK_SHADOW_HARDEN`（0 = 关闭 = 基线）。

测试场景刻意构造成**可判定**的：

  · 400 单位地面（castShadow=false）。
  · 一块**斜置的大遮光板**悬在地面上方：它在断口处投下一条**斜的影子
    边缘**，斜着穿过画面 —— 于是这一条边缘会同时跨过三条级联边界
    （≈19.8 / 36.9 / 90m）。这是"能看见级联接缝"与"能测接触硬化"的前提：
    接缝只有在**阴影区域里**才显形（纯受光处两级都返回 1，混合是恒等的）。
  · 相机 (0,0,32)、yaw=270（视线朝 −Y）、pitch=26° → 沿地面看出去，
    视口里从 ~28m 到很远处全是地面，分割点 19.8/36.9 都映射到画面内的
    地面行上。
  · 光源传播方向 [-0.22, +0.55, -0.80]：**朝 +Y 传播**，遮光板的影子
    铺向相机这一侧。

断言分四组：

  1. **关闭 = 基线**（blend=0, harden=0）
     · SHADOW-DBG 里 blend=0.000 harden=0.000 snap=1；
     · 同一配置跑两次，视口**逐像素一致**。

  2. **texel snapping 的作用**（MYVK_SHADOW_SNAP=0/1）
     量化保证了什么：**只要相机移动量小于一个纹素，该级的光空间矩阵就逐位
     不变** —— 矩阵一跳就是整一个纹素。关掉量化后矩阵连续变化 → 均匀的
     亚纹素漂移（不存在"几步不动"）。

     ⚠ **这一组不看截图**。2048² 下一个纹素 ≈ 1cm 世界 ≈ 0.05 屏幕像素，
     量化差异在亚像素以下，而相机一移动，"整幅画面的视差位移"会把它彻底
     淹没 —— 实测 snap 开/关的截图像素差分在小数点后 5 位完全相同
     （0.02037 vs 0.02037），把分辨率降到 256 也一样。**靠像素差分根本测
     不出来**，硬凑只会做出一个恒真的假断言。

     所以断言对象是**画面之外的量**：第 0 级光源空间矩阵的平移列
     （`Renderer` 用 `MYVK_SHADOW_MATRIX_DBG=1` 打 `SHADOW-MATRIX c0 t=(..)`，
     ×1e6 取整）。
     · 相机沿世界 +X 走一串 **0.001 m 的小步**（世界 X 在光轴 X 上投影
       ≈0.93，第 0 级纹素 ≈0.0075 m → 0.001 步 ≈ 1/8 纹素）。
     · 量化开：矩阵平移列**分段常数**（连续几步取同一值 → 一次整格跳）；
       整格跳的幅度 ≈ 一个纹素，且 ≫ 关掉时的单步漂移。
     · 量化关：**每步都变**，是均匀的小漂移。
     断言：① 量化开存在"连续 ≥2 步不动"；② 量化开的整格跳 ≥3× 关掉时的
     单步漂移；③ 量化关的每步都在变（最长常数段 = 1）；④ 量化开的分段
     常数性 > 关掉时。

  3. **级联混合带**（MYVK_SHADOW_BLEND=0.25）
     · SHADOW-DBG 里 blend=0.250；
     · 与关闭态相比有**可测差异**（说明混合带真的改了像素），但差异
       必须**局限在地面**（视口上方 1/4 平均差 ≈ 0）；
     · **接缝处的亮度跳变被削弱**：沿地面取一列像素，统计相邻行的亮度
       差分峰值。混合带打开后，边界附近的**最大跳变应当不高于**关闭态
       （典型的接缝是"一条亮线"，打开后被抹成渐变）。

  4. **接触硬化**（MYVK_SHADOW_HARDEN=1.0）
     · SHADOW-DBG 里 harden=1.000；
     · 与关闭态相比有可测差异，且差异集中在地面；
     · 变化量**随视深度变化**（远端比近端变更多）—— 这证明半径缩放
       确实"随视深度"在动，而不是全局改了个常数。

全程开 MYVK_EDITOR_NO_HUD 关掉视口叠加层（FPS/帧时间每次都不同，
不关掉"逐像素一致"这类断言根本没法成立）。

用法：
    python tools/verify_shadow_hardening.py
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

# 相机 (0,0,32)、yaw=270 → 视线朝 −Y；pitch=40° 俯视 → 地面占画面下半。
CAM = "0,0,32,270,40"
GROUND_SIZE = 400.0
# 光源**传播**方向：朝 +Y（相机这一侧）+ 向下，遮光板的影子铺向相机。
LIGHT_DIR = [-0.22, 0.55, -0.80]
AMBIENT_SCALE = 0.25

CAM_NEAR = 0.1
CAM_FAR = 200.0

# 一根**长条遮光板**悬在地面上方：它在断口处投下一条**又长又宽的影子
# 条带**，条带的**竖直边缘**从近处一直延伸到远处 —— 那条边缘会依次穿过
# 三条级联边界（≈19.8 / 36.9 / 90m）。这是"能看见级联接缝"与"能测接触
# 硬化"的前提：接缝只有在**阴影边缘上**才显形（纯受光处两级都返回 1，
# 混合是恒等的；纯阴影里两级都是 0，也看不出差别）。
SLAB_SIZE = (30.0, 140.0, 2.0)
SLAB_POS = (2.0, -55.0, 40.0)
SLAB_YAW_DEG = 12.0
# 细柱：给接触硬化提供额外的影子边缘。
PILLARS = [(-30.0, -22.0), (-36.0, 10.0), (-54.0, -6.0)]
PILLAR_H = 7.0
PILLAR_W = 1.2

DBG_RE = re.compile(r"SHADOW-DBG cascades=(\d+) dist=([\d.]+) "
                    r"lambda=([\d.]+) splits=([\d.]+)/([\d.]+)/([\d.]+)/([\d.]+) "
                    r"bias=\(([\d.]+),([\d.]+),([\d.]+),([\d.]+)\) enabled=(\d+) "
                    r"blend=([\d.]+) harden=([\d.]+) snap=(\d+)")


def material(name, base, roughness=0.5):
    return {
        "kind": "procedural", "name": name,
        "baseColor": list(base) + [1.0],
        "metallic": 0.0, "roughness": roughness,
        "normalScale": 1.0, "ao": 1.0,
        "emissive": [0.0, 0.0, 0.0], "doubleSided": False, "alphaBlend": False,
    }


def build_scene(path):
    """地面 + 斜置大遮光板（影子边缘斜穿多条级联边界）+ 几根细柱。"""
    ents = [{
        "name": "Ground",
        "transform": {"position": [0, 0, 0], "rotation": [0, 0, 0],
                      "scale": [1, 1, 1]},
        "visible": True,
        # 地面只接收阴影不投影 —— 否则它会变成最大的遮挡体
        "castShadow": False,
        "parent": -1,
        "mesh": {"kind": "builtin", "shape": "plane", "size": GROUND_SIZE,
                 "name": "sh_ground"},
        "material": material("sh_ground_mat", (0.74, 0.74, 0.76)),
    }]
    n = 0
    # 斜置遮光板：绕 Z 轴转 SLAB_YAW_DEG 度。影子边缘因此是地面上的斜线。
    ents.append({
        "name": "Slab",
        "transform": {"position": list(SLAB_POS),
                      "rotation": [0.0, 0.0, SLAB_YAW_DEG],
                      "scale": list(SLAB_SIZE)},
        "visible": True, "castShadow": True, "parent": -1,
        "mesh": {"kind": "builtin", "shape": "cube", "size": 1.0,
                 "name": "sh_slab"},
        "material": material("sh_slab_mat", (0.55, 0.55, 0.60)),
    })
    n += 1
    for i, (y, x) in enumerate(PILLARS):
        ents.append({
            "name": "Pillar%d" % i,
            "transform": {"position": [x, y, PILLAR_H * 0.5],
                          "rotation": [0, 0, 0],
                          "scale": [PILLAR_W, PILLAR_W, PILLAR_H]},
            "visible": True, "castShadow": True, "parent": -1,
            "mesh": {"kind": "builtin", "shape": "cube", "size": 1.0,
                     "name": "sh_pillar"},
            "material": material("sh_pillar_mat", (0.82, 0.55, 0.34)),
        })
        n += 1
    doc = {
        "version": 1,
        "generator": "verify_shadow_hardening.py",
        "light": {"direction": LIGHT_DIR, "color": [1.0, 0.97, 0.92],
                  "intensity": 1.0, "castsShadow": True,
                  "ambientScale": AMBIENT_SCALE},
        "entities": ents,
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)
    return n


def run_once(exe, scene, extra_env, shot_path, cam=None, wait=14.0):
    """跑一轮编辑器，返回 (日志行, 视口矩形, 截图, 错误行)。"""
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_ENV_DBG"] = "0"
    env["MYVK_LIGHT_GIZMOS"] = "0"
    env["MYVK_EDITOR_NO_HUD"] = "1"
    # 关掉视口地平面栅格：栅格是屏幕空间的固定图案，与阴影无关，
    # 只会给"逐像素相同/差异定位"添噪声。
    env["MYVK_GRID"] = "0"
    env["MYVK_SHADOW_DBG"] = "1"
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
                ready = any("SHADOW-DBG" in l for l in tap.lines)
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


def grad_stats(a, b, thresh=20):
    """逐像素差统计：>thresh 的像素数、最大差、均值。"""
    pa, pb = a.load(), b.load()
    W, H = a.size
    n = 0
    mx = 0
    total = 0
    for y in range(H):
        for x in range(W):
            p, q = pa[x, y], pb[x, y]
            d = max(abs(p[0] - q[0]), abs(p[1] - q[1]), abs(p[2] - q[2]))
            total += d
            if d > thresh:
                n += 1
                mx = max(mx, d)
    return n, mx, total / float(W * H)


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


def band_row_gradient(a, x0, x1, ytop, ybot):
    """在 [ytop, ybot) 行内，逐列统计"行到行的亮度跳变"，返回 (均值, 峰值)。

    级联接缝表现为一条**水平的亮度台阶**：相邻两行之间有一步明显的
    亮度差。混合带打开后，台阶被抹成渐变 → 这两个量应当下降。
    逐列做（而不是先按行平均）是因为影子边缘是斜的，按行平均会把
    台阶抹平；逐列则能保留"某列在边界行处的那一步"。
    峰值（而不是均值）才是"亮线"的直接指标：均值会被大量平坦像素稀释。
    """
    px = a.load()
    W, H = a.size
    x0, x1 = max(int(x0), 0), min(int(x1), W)
    ytop, ybot = max(int(ytop), 1), min(int(ybot), H)
    total = 0.0
    n = 0
    peak = 0.0
    for x in range(x0, x1, 2):
        for y in range(ytop, ybot):
            p = px[x, y - 1]
            q = px[x, y]
            lum_a = (p[0] + p[1] + p[2]) / 3.0
            lum_b = (q[0] + q[1] + q[2]) / 3.0
            d = abs(lum_b - lum_a)
            total += d
            n += 1
            if d > peak:
                peak = d
    return total / max(n, 1), peak


def changed_row_span(a, b, thresh=20, min_px=2):
    """变化像素在竖直方向上的分布。

    返回 (行数, 最高行, 最低行)。级联混合带的作用**必须**表现为一条
    **水平带**（每条级联边界一窄条）—— 如果变化散落在整个画面上下，
    那就不是"在边界处混合"，而是全局改了亮度。
    """
    pa, pb = a.load(), b.load()
    W, H = a.size
    rows = []
    for y in range(H):
        cnt = 0
        for x in range(W):
            p, q = pa[x, y], pb[x, y]
            if (abs(p[0] - q[0]) > thresh or abs(p[1] - q[1]) > thresh
                    or abs(p[2] - q[2]) > thresh):
                cnt += 1
                if cnt >= min_px:
                    rows.append(y)
                    break
    if not rows:
        return 0, -1, -1
    return len(rows), rows[0], rows[-1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--quick", action="store_true",
                    help="只跑断言 1/3/4（跳过较慢的 snap 抖动扫描）")
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    os.makedirs(OUT_DIR, exist_ok=True)
    if not os.path.exists(exe):
        print("找不到可执行文件，请先构建：", exe)
        return 1

    scene = os.path.join(OUT_DIR, "shadow_hardening.json")
    n_casters = build_scene(scene)
    print("测试场景：%s（%d 个投影体 + 1 个地面）" % (scene, n_casters))

    fails = []

    def check(name, cond, detail=""):
        print(("  [PASS] " if cond else "  [FAIL] ") + name +
              (("  " + str(detail)) if detail else ""))
        if not cond:
            fails.append(name)

    kill_stale_editors()

    # ============ 第 1 组：关闭 = 基线 ============
    print("\n== 第 1 组：三件套按默认值关闭 → 基线可复现 ==")
    shot_a = os.path.join(OUT_DIR, "sh_base_a.png")
    shot_b = os.path.join(OUT_DIR, "sh_base_b.png")
    lines, vp, img_a, errs = run_once(exe, scene, {}, shot_a)
    if not vp:
        print("  没等到 VP-RECT")
        return 2
    check("零 Vulkan 验证层报错", not errs, errs[:2])

    dbg = find_line(lines, "SHADOW-DBG", DBG_RE)
    print("  SHADOW-DBG:", dbg.group(0) if dbg else None)
    check("拿到含 blend/harden/snap 的 SHADOW-DBG", dbg is not None)
    if not dbg or img_a is None:
        print("\n结果: FAIL（没能建立阴影基线）")
        return 3
    check("blend = 0.000", abs(float(dbg.group(13))) < 1e-3, dbg.group(13))
    check("harden = 0.000", abs(float(dbg.group(14))) < 1e-3, dbg.group(14))
    check("snap = 1（默认开）", dbg.group(15) == "1", dbg.group(15))
    check("cascades = 4 / enabled = 1",
          dbg.group(1) == "4" and dbg.group(12) == "1",
          (dbg.group(1), dbg.group(12)))

    lines_b, _, img_b, errs_b = run_once(exe, scene, {}, shot_b)
    check("第二次运行零报错", not errs_b, errs_b[:2])
    if img_b is not None:
        n, mx, mean = grad_stats(img_a, img_b, 0)
        print("  两次运行差异: 像素=%d 最大差=%d 均值=%.6f" % (n, mx, mean))
        check("关闭态两次运行逐像素一致（最大差 = 0）", mx == 0,
              "max=%d" % mx)
        check("关闭态平均差 = 0.0000", mean < 1e-6, "%.6f" % mean)

    x0, y0, x1, y1 = [float(v) for v in vp]
    vw, vh = x1 - x0, y1 - y0
    upper = (x0, y0, x1, y0 + 0.25 * vh)

    # ============ 第 2 组：texel snapping ============
    if not args.quick:
        print("\n== 第 2 组：texel snapping（光空间矩阵的纹素量化）==")
        # ⚠ 这一组**不看截图**，直接断言"光空间矩阵"这个**画面之外的量**。
        #
        # 为什么不能靠截图：`MYVK_SHADOW_RES=2048` 时一个阴影视纹素 ≈ 1cm
        # 世界尺度 ≈ 0.05 屏幕像素 —— 量化带来的差异**远在亚像素以下**，
        # 而相机一移动，"整幅画面的视差位移"会把这个亚像素差异彻底淹没。
        # 实测：snap 开/关的截图差分在小数点后 5 位完全相同（0.02037 vs
        # 0.02037），把分辨率降到 256 也一样 —— 靠像素差分**根本测不出来**。
        #
        # 量化真正保证的是：**相机在纹素内移动时，该级光源空间矩阵逐位不变**
        # （矩阵一跳就是整一个纹素）。于是断言对象就是矩阵本身：
        #   · snap 开：矩阵平移列**分段常数** —— 一连串小步里出现"连续几步
        #     取同一个值"，然后**一次整格跳**（跳变幅度 ≈ 一个纹素）。
        #   · snap 关：矩阵平移列**每步都变**，且是均匀的亚纹素漂移
        #     （不存在"几步不动"）。
        # Renderer 用 MYVK_SHADOW_MATRIX_DBG=1 打 `SHADOW-MATRIX c0 t=(..)`
        # （平移列 ×1e6 取整，第 0 级）。
        #
        # 步长必须**远小于一个纹素在光轴上的投影**：相机沿世界 +X 走，
        # 世界 X 在光轴 X 上的分量 ≈ 0.93；第 0 级纹素 ≈ 0.0075 m →
        # 步长取 0.001 m（≈ 1/8 纹素）才能在纹素内产生"多步不动"。
        steps = [i * 0.001 for i in range(9)]     # 0.000 .. 0.008

        mtx_re = re.compile(
            r"SHADOW-MATRIX c0 t=\((-?\d+),(-?\d+),(-?\d+)\)")

        def matrix_scan(tag, extra):
            """返回每个 dx 下的第 0 级光空间矩阵平移列（×1e6 整数）。"""
            out = []
            for i, dx in enumerate(steps):
                cam = "%0.4f,0,32,270,40" % dx
                shot = os.path.join(OUT_DIR, "sh_mtx_%s_%d.png" % (tag, i))
                e = dict(extra)
                e["MYVK_SHADOW_MATRIX_DBG"] = "1"
                lines_i, _, _, er = run_once(exe, scene, e, shot, cam=cam)
                if er:
                    check("%s step%d 零报错" % (tag, i), False, er[:2])
                vals = [m.groups() for l in lines_i
                        for m in [mtx_re.search(l)] if m]
                out.append(vals[-1] if vals else None)
            return out

        m_snap = matrix_scan("snap", {})
        m_nosnap = matrix_scan("nosnap", {"MYVK_SHADOW_SNAP": "0"})

        def tx_series(ms):
            return [int(m[0]) for m in ms if m is not None]

        def repeat_runs(xs):
            """把序列压缩成"连续相同值的段长列表"。"""
            if not xs:
                return []
            runs = [1]
            for i in range(1, len(xs)):
                if xs[i] == xs[i - 1]:
                    runs[-1] += 1
                else:
                    runs.append(1)
            return runs

        ts = tx_series(m_snap)
        tn = tx_series(m_nosnap)
        print("  snap   的 t.x 序列:", ts)
        print("  nosnap 的 t.x 序列:", tn)
        if not ts or not tn or m_snap[0] is None or m_nosnap[0] is None:
            check("拿到 SHADOW-MATRIX 日志", False, "snap=%d nosnap=%d"
                  % (len(ts), len(tn)))
        else:
            runs_s = repeat_runs(ts)
            runs_n = repeat_runs(tn)
            print("  snap   段长:", runs_s, "（最长常数段 = %d）" % max(runs_s))
            print("  nosnap 段长:", runs_n, "（最长常数段 = %d）" % max(runs_n))

            # ① snap 开：存在"连续 ≥2 步取同一值"的长段 —— 矩阵在纹素内不动
            check("量化开：矩阵平移列分段常数（存在连续 ≥2 步不动）",
                  max(runs_s) >= 2, "最长段=%d" % max(runs_s))
            # ② snap 开：整格跳的幅度 ≈ 一个纹素（而不是连续小漂移）
            diffs_s = [abs(ts[i] - ts[i - 1]) for i in range(1, len(ts))
                       if ts[i] != ts[i - 1]]
            diffs_n = [abs(tn[i] - tn[i - 1]) for i in range(1, len(tn))
                       if tn[i] != tn[i - 1]]
            jump_s = max(diffs_s) if diffs_s else 0
            step_n = (sum(diffs_n) / len(diffs_n)) if diffs_n else 0.0
            print("  量化开的最大跳变 = %d (1e-6 m) / 量化关的每步漂移均值 = %.1f"
                  % (jump_s, step_n))
            check("量化开的整格跳 ≫ 关掉时的单步漂移（≥3×）",
                  step_n > 0 and jump_s > 3.0 * step_n,
                  "%d vs %.1f" % (jump_s, step_n))
            # ③ snap 关：每一步都在变（不存在连续不动段）
            check("量化关：每步都漂移（最长常数段 = 1）",
                  max(runs_n) == 1, "最长段=%d" % max(runs_n))
            # ④ 直接对比：量化开的"常数段"明显长于关掉时
            check("量化开的分段常数性 > 关掉时",
                  max(runs_s) > max(runs_n),
                  "%d vs %d" % (max(runs_s), max(runs_n)))



    # ============ 第 3 组：级联间混合带 ============
    print("\n== 第 3 组：级联间混合带（MYVK_SHADOW_BLEND=0.15）==")
    shot_bl = os.path.join(OUT_DIR, "sh_blend.png")
    lines_bl, _, img_bl, errs_bl = run_once(
        exe, scene, {"MYVK_SHADOW_BLEND": "0.25"}, shot_bl)
    check("零 Vulkan 验证层报错", not errs_bl, errs_bl[:2])
    dbg_bl = find_line(lines_bl, "SHADOW-DBG", DBG_RE)
    if dbg_bl:
        print("  SHADOW-DBG:", dbg_bl.group(0))
        check("blend = 0.250", abs(float(dbg_bl.group(13)) - 0.25) < 1e-3,
              dbg_bl.group(13))
        check("harden 仍为 0.000（只开了混合带）",
              abs(float(dbg_bl.group(14))) < 1e-3, dbg_bl.group(14))
    else:
        check("拿到 SHADOW-DBG", False)

    if img_bl is not None:
        n, mx, mean = grad_stats(img_a, img_bl, 20)
        print("  混合带开 vs 关: 变化像素=%d 最大差=%d 均值=%.4f"
              % (n, mx, mean))
        check("混合带确实改了像素（变化像素 > 60 且最大差 > 8）",
              n > 60 and mx > 8, "n=%d max=%d" % (n, mx))
        d_upper = region_mean_diff(img_a, img_bl, upper)
        print("  视口上方 1/4 平均差: %.4f" % d_upper)
        check("变化不波及天空/视口上方（平均差 < 0.6）", d_upper < 0.6,
              "%.4f" % d_upper)

        # 变化必须落成**水平带**（每条级联边界一窄条）：混合带只在
        # 边界附近几米内起作用，屏幕上就是几行 —— 若变化铺满整个画面
        # 上下，说明改的不是"边界处混合"而是别的。
        nrows, ytop, ybot = changed_row_span(img_a, img_bl, 20)
        if nrows:
            print("  混合带变化落在 %d 行上（y=%d..%d，视口高 %d，占 %.1f%%）"
                  % (nrows, ytop, ybot, int(vh),
                     100.0 * (ybot - ytop + 1) / vh))
        check("混合带的变化是水平带（高度 < 视口 30%）",
              nrows > 0 and (ybot - ytop + 1) < 0.30 * vh,
              "%d 行" % nrows)

        # 核心断言：接缝是"一条水平亮度台阶"，混合带把台阶抹成渐变。
        # 在变化带附近取若干行（带上下各扩 6 行，避免只量到纯阴影区），
        # 逐列统计相邻行的亮度跳变均值 —— 打开混合带后应当**下降**。
        if nrows:
            gy0 = max(int(y0), ytop - 6)
            gy1 = min(int(y1), ybot + 7)
            m_off, p_off = band_row_gradient(img_a, x0, x1, gy0, gy1)
            m_on, p_on = band_row_gradient(img_bl, x0, x1, gy0, gy1)
            print("  边界带内逐列行跳变: 关(均值=%.3f, 峰值=%.1f) / "
                  "开(均值=%.3f, 峰值=%.1f)"
                  % (m_off, p_off, m_on, p_on))
            # 峰值才是"亮线"的直接指标：均值会被带内大量平坦像素稀释，
            # 期望它明显下降是不合理的；台阶的**最高一步**必须被压下去。
            check("混合带把边界处最亮的那一步台阶压低了（峰值下降 ≥20%）",
                  p_on < p_off * 0.8, "%.1f vs %.1f" % (p_on, p_off))

    # ============ 第 4 组：接触硬化 ============
    print("\n== 第 4 组：接触硬化（MYVK_SHADOW_HARDEN=1.0）==")
    shot_hd = os.path.join(OUT_DIR, "sh_harden.png")
    lines_hd, _, img_hd, errs_hd = run_once(
        exe, scene, {"MYVK_SHADOW_HARDEN": "1.0"}, shot_hd)
    check("零 Vulkan 验证层报错", not errs_hd, errs_hd[:2])
    dbg_hd = find_line(lines_hd, "SHADOW-DBG", DBG_RE)
    if dbg_hd:
        print("  SHADOW-DBG:", dbg_hd.group(0))
        check("harden = 1.000", abs(float(dbg_hd.group(14)) - 1.0) < 1e-3,
              dbg_hd.group(14))
        check("blend 仍为 0.000（只开了接触硬化）",
              abs(float(dbg_hd.group(13))) < 1e-3, dbg_hd.group(13))
    else:
        check("拿到 SHADOW-DBG", False)

    if img_hd is not None:
        n, mx, mean = grad_stats(img_a, img_hd, 20)
        print("  接触硬化开 vs 关: 变化像素=%d 最大差=%d 均值=%.4f"
              % (n, mx, mean))
        check("接触硬化确实改了像素（变化像素 > 60 且最大差 > 8）",
              n > 60 and mx > 8, "n=%d max=%d" % (n, mx))
        d_upper = region_mean_diff(img_a, img_hd, upper)
        print("  视口上方 1/4 平均差: %.4f" % d_upper)
        check("变化不波及天空/视口上方（平均差 < 0.6）", d_upper < 0.6,
              "%.4f" % d_upper)

        # 半径缩放随视深度变化 → 画面上"近处"与"远处"的变化量必然**不同**。
        # 若只是全局改了个常数半径，两条带的变化量会几乎相等。
        near_band = (x0, y0 + 0.55 * vh, x1, y0 + 0.95 * vh)
        far_band = (x0, y0 + 0.25 * vh, x1, y0 + 0.50 * vh)
        d_near = region_mean_diff(img_a, img_hd, near_band)
        d_far = region_mean_diff(img_a, img_hd, far_band)
        print("  接触硬化变化量: 画面下半(近)=%.3f 上中部(远)=%.3f"
              % (d_near, d_far))
        check("两带都发生了变化（证明在动，不是零效果）",
              d_near > 0.05 and d_far > 0.05, "%.3f / %.3f" % (d_near, d_far))
        check("半径缩放随视深度在动：近/远两带变化量差异显著（比值 > 1.15）",
              max(d_near, d_far) > 1.15 * max(min(d_near, d_far), 1e-6),
              "%.3f vs %.3f" % (d_near, d_far))

    print("\n结果: %s" % ("PASS" if not fails else "FAIL: " + ", ".join(fails)))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
