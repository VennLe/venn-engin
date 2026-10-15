"""端到端验证：天空盒 + 真 IBL（irradiance / prefiltered specular / BRDF LUT）。

背景：在这之前 pbr.frag 的环境光是"程序化天空梯度"的单次方向采样近似 ——
粗糙度只让反射变暗、不会变糊，能量也不守恒。这个脚本验的是替换后的
split-sum IBL 整条链路（见 render/Environment.h）。

测试场景刻意构造成**可判定**的：
  · 相机在 (0,0,3) 朝 +X 平视（yaw=180）；环境是 Z-up，所以地平线
    正好落在画面竖直中央 —— 上半屏是天空、下半屏是地面。
  · 正中一颗半径 1.2 的**金属镜面球**（metallic=1, roughness=0.02），
    距离 4，视场角 45°：它在屏幕上的半径 = asin(1.2/4)/22.5° ≈ 0.38×画面高，
    足以在"球心上方 0.30h"与"下方 0.30h"各取一小块采样区而不会碰到边缘。
  · 太阳放在球的**背面**（NdotL ≤ 0），所以可见面上没有直接光 ——
    球的颜色**只能**来自环境。这一点很关键：它让"球的颜色"变成
    "环境光是否正确"的直接读数。

断言分四组：

  1. **环境资源与回读自检**（ENV-DBG / ENV-READBACK）
     · 四张图的尺寸符合预期。
     · 天空立方体各面平均色的亮度满足 天顶(+Z) > 水平(+Y) > 地面(-Z)
       且严格递减。这一条抓的是 Z-up 引擎里最经典的 bug ——
       天空梯度落到了 dir.y 上（那是水平方向，梯度会变成一条横带）。
     · 天顶面偏蓝（b > r），地面暗。
     · BRDF 表在 (N·V=1, roughness=0) 处的理论值是**精确的 (1, 0)**，
       直接硬断言 A ≈ 1、B ≈ 0。这是整条 split-sum 链路上唯一一个
       "不用跑渲染就能算出标准答案"的点，价值最高。

  2. **天空盒背景确实在画**（MYVK_SKY=0 对照）
     · 关掉天空后，视口四角的像素发生**巨大**变化（从天空色变成清屏色）。
     · 而球体内部区域几乎不变（球的环境光来自烘焙贴图，与背景 pass 无关）。
       这两条一起把"背景 pass"和"物体着色"彻底分开归因。

  3. **IBL 确实在生效**（MYVK_IBL=0 对照）
     · 开关两次，球体区域的像素必须显著不同（近似就是近似）。
     · 四角背景必须**逐像素相同** —— 背景由 sky.frag 画，与 IBL 开关无关。
       如果这里对不上，说明 IBL 开关意外影响到了背景，是回归信号。

  4. **因果实验**（把天顶染成纯红、地面染成纯蓝）
     · 先跑一个**没有球**的场景：画面上方到地平线之间取三条采样带，
       红/蓝比（r/b）必须**单调递减**。相机平视 → 地平线正好在视口竖直
       中央 → y 越往下 dir.z 越小 → 梯度 t² 越小 → 天顶红占比越低。
       这是"天空方向映射用的是 dir.z 而不是 dir.y"最直接的证据。
       （注意不能断言"画面上方就该是红的"——45° FOV 下画面上边缘仰角
        只有 22.5°，那里 t² ≈ 0.48 还不到一半，蓝本来就略占优。）
     · 再关掉天空跑有球场景：球心上方采样区**偏红**、下方**偏蓝**。天空
       关掉之后画面上只剩球和清屏底色，所以红/蓝只可能来自球 —— 这同时
       证明了天空生成、面号映射、GGX 预过滤、BRDF 表整条链路都在真的
       生效。任何一处错（方向反了、mip 选错、LUT 没接上）都会让颜色对不上。

注意：所有轮次都开 MYVK_EDITOR_NO_HUD 关掉视口叠加层。左上角的统计文字
里有 FPS / 帧时间这类**每次运行都不同**的数字，不关掉的话背景区永远会差
几个灰阶，"两次运行必须逐像素相同"的断言根本没法成立。

用法：
    python tools/verify_ibl.py
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_stable, find_window, user32      # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors, wait_rect   # noqa: E402
from verify_frustum_cull import region_diff                         # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = os.path.join(ROOT, "..", "_venn_verify")

# 相机在 (0,0,3) 朝 +X 平视（yaw=180° → forward = +X）
CAM = "0,0,3,180,0"

ENV_RE = re.compile(r"ENV-DBG sky=(\d+) irr=(\d+) pref=(\d+) mips=(\d+) lut=(\d+)")
RB_RE = re.compile(
    r"ENV-READBACK up=\(([-\d.]+),([-\d.]+),([-\d.]+)\) "
    r"side=\(([-\d.]+),([-\d.]+),([-\d.]+)\) "
    r"down=\(([-\d.]+),([-\d.]+),([-\d.]+)\) "
    r"lutA10=([-\d.]+) lutB10=([-\d.]+) lutA55=([-\d.]+) lutB55=([-\d.]+)")


def make_material(name, metallic, roughness, base=(1.0, 1.0, 1.0)):
    return {
        "kind": "procedural", "name": name,
        "baseColor": list(base) + [1.0],
        "metallic": metallic, "roughness": roughness,
        "normalScale": 1.0, "ao": 1.0,
        "emissive": [0.0, 0.0, 0.0], "doubleSided": False, "alphaBlend": False,
    }


def entity(name, pos, mesh, mat):
    return {
        "name": name,
        "transform": {"position": list(pos), "rotation": [0, 0, 0],
                      "scale": [1, 1, 1]},
        "visible": True,
        "castShadow": False,
        "parent": -1,
        "mesh": mesh,
        "material": mat,
    }


def build_scene(path, with_spheres=True):
    """默认：一颗居中的金属镜面球 + 两侧的点缀球。

    with_spheres=False 时只留灯和地面栅格 —— 给"天空梯度方向"那组用：
    竖直中线上没有任何物体遮挡，可以从画面上缘一路采到地平线。
    """
    ents = [
        entity("Metal", (4.0, 0.0, 3.0),
               {"kind": "builtin", "shape": "sphere", "size": 1.2,
                "segments": 48, "rings": 24, "name": "ibl_mirror"},
               make_material("ibl_mirror_mat", 1.0, 0.02)),
        entity("MetalRough", (4.0, -1.9, 3.0),
               {"kind": "builtin", "shape": "sphere", "size": 0.7,
                "segments": 32, "rings": 16, "name": "ibl_rough"},
               make_material("ibl_rough_mat", 1.0, 0.45)),
        entity("Dielectric", (4.0, 1.9, 3.0),
               {"kind": "builtin", "shape": "sphere", "size": 0.7,
                "segments": 32, "rings": 16, "name": "ibl_diel"},
               make_material("ibl_diel_mat", 0.0, 0.25, (0.85, 0.25, 0.2))),
    ]
    if not with_spheres:
        ents = []
    doc = {
        "version": 1,
        "generator": "verify_ibl.py",
        # 太阳放在球的**背面**（传播方向指向 +X 一侧）：
        # 可见面的 NdotL ≤ 0 → 没有直接光 → 球的颜色只来自环境光。
        "light": {"direction": [-1.0, 0.2, 0.3], "color": [1.0, 0.98, 0.95],
                  "intensity": 1.0, "castsShadow": False, "ambientScale": 1.0},
        "entities": ents,
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)
    return len(ents)


def run_once(exe, scene, extra_env, shot_path=None, wait=14.0):
    """跑一轮编辑器，返回 (日志行快照, 视口矩形, 截图, 错误行)。"""
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_ENV_DBG"] = "1"
    # 灯的小球是自发光体，会把画面搅浑；这个测试不关心它
    env["MYVK_LIGHT_GIZMOS"] = "0"
    # 关掉视口里的统计 / 导航叠加层：里面的 FPS、帧时间是浮动的，会让
    # "同一配置两次运行背景逐像素相同"这类断言永远失败
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
        # 等场景真的出图（拿到一次 LOD/剔除统计即视为稳定）
        end = time.time() + wait
        while time.time() < end:
            with tap.lock:
                ready = any("LOD-STATS" in l or "CULL-STATS" in l
                            for l in tap.lines)
            if ready:
                break
            time.sleep(0.25)
        time.sleep(1.5)

        img = None
        if shot_path:
            # capture_stable：重试到拿到"真的画面"（尺寸正确且非整窗单色）
            # —— PrintWindow 偶发返回尺寸正常但整幅纯白的未合成帧
            img = capture_stable(hwnd)
            if img is None:
                print("  WARN: 抓图失败（连续空帧），本轮像素比对不可信")
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
            except subprocess.TimeoutExpired:
                proc.kill()


def find_line(lines, needle, rx):
    for line in reversed(lines):
        if needle in line:
            m = rx.search(line)
            if m:
                return m
    return None


def luminance(c):
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]


def region_mean(img, box):
    """box 内的平均 (r,g,b)，0-255。"""
    x0, y0, x1, y1 = [int(v) for v in box]
    W, H = img.size
    x0, y0 = max(x0, 0), max(y0, 0)
    x1, y1 = min(x1, W), min(y1, H)
    px = img.load()
    sr = sg = sb = 0
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            p = px[x, y]
            sr += p[0]
            sg += p[1]
            sb += p[2]
            n += 1
    n = max(n, 1)
    return (sr / n, sg / n, sb / n)


def boxes(vp):
    """由视口矩形（客户端坐标）推出四个采样区。

    坐标用的是**画面高度 h** 而不是宽高比：球在任何投影下都成一个圆，
    它的屏幕半径在 x/y 方向是同一个像素数（≈0.38h），所以用 h 做尺度
    才能保证采样区一定落在球面内。
    """
    x0, y0, x1, y1 = [float(v) for v in vp]
    w, h = x1 - x0, y1 - y0
    cx, cy = (x0 + x1) / 2.0, (y0 + y1) / 2.0
    return {
        # 球心附近（一定在球面上）
        "center": (cx - 0.13 * h, cy - 0.13 * h, cx + 0.13 * h, cy + 0.13 * h),
        # 球面上方 0.20h~0.30h（圆半径 0.38h，这里离边缘还有余量）
        "upper": (cx - 0.10 * h, cy - 0.30 * h, cx + 0.10 * h, cy - 0.20 * h),
        "lower": (cx - 0.10 * h, cy + 0.20 * h, cx + 0.10 * h, cy + 0.30 * h),
        # 四角：远高于地平线（相机平视），一定是天空
        "tl": (x0 + 0.02 * w, y0 + 0.02 * h, x0 + 0.10 * w, y0 + 0.10 * h),
        "tr": (x1 - 0.10 * w, y0 + 0.02 * h, x1 - 0.02 * w, y0 + 0.10 * h),
        "bl": (x0 + 0.02 * w, y1 - 0.10 * h, x0 + 0.10 * w, y1 - 0.02 * h),
        # 无球场景专用：竖直中线上一串越靠下越贴近地平线的天空采样带。
        # 相机平视 → 地平线正好在视口竖直中央 → y 越大 dir.z 越小。
        # 用中线是因为这里 dir.z 最大，纵向梯度的信噪比最好。
        "sky_hi": (cx - 0.06 * h, y0 + 0.04 * h, cx + 0.06 * h, y0 + 0.14 * h),
        "sky_mid": (cx - 0.06 * h, y0 + 0.28 * h, cx + 0.06 * h, y0 + 0.38 * h),
        "sky_lo": (cx - 0.06 * h, y0 + 0.44 * h, cx + 0.06 * h, y0 + 0.485 * h),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    os.makedirs(OUT_DIR, exist_ok=True)
    if not os.path.exists(exe):
        print("找不到可执行文件，请先构建：", exe)
        return 1

    scene = os.path.join(OUT_DIR, "ibl_test.json")
    build_scene(scene)
    print("测试场景：%s" % scene)

    fails = []

    def check(name, cond, detail=""):
        print(("  [PASS] " if cond else "  [FAIL] ") + name +
              (("  " + str(detail)) if detail else ""))
        if not cond:
            fails.append(name)

    kill_stale_editors()

    # ============ 第 1 组：环境资源 + 回读自检 ============
    print("\n== 第 1 组：环境贴图生成与数值自检 ==")
    shot_a = os.path.join(OUT_DIR, "ibl_default.png")
    lines_a, vp, img_a, errs_a = run_once(exe, scene, {}, shot_a)
    if not vp:
        print("  没等到 VP-RECT")
        return 2

    env = find_line(lines_a, "ENV-DBG", ENV_RE)
    rb = find_line(lines_a, "ENV-READBACK", RB_RE)
    print("  ENV-DBG:", env.group(0) if env else None)
    print("  ENV-READBACK:", rb.group(0) if rb else None)
    check("拿到 ENV-DBG", env is not None)
    check("拿到 ENV-READBACK（天空立方体与 BRDF 表都回读成功）",
          rb is not None)
    check("零 Vulkan 验证层报错", not errs_a, errs_a[:2])
    if not env or not rb:
        print("\n结果: FAIL（环境未能建立）")
        return 3

    check("天空立方体 128²", env.group(1) == "128", env.group(1))
    check("辐照度立方体 32²", env.group(2) == "32", env.group(2))
    check("预过滤立方体 128² × 5 mip",
          env.group(3) == "128" and env.group(4) == "5",
          "%s x %s" % (env.group(3), env.group(4)))
    check("BRDF 表 256²", env.group(5) == "256", env.group(5))

    g = [float(v) for v in rb.groups()]
    up, side, down = g[0:3], g[3:6], g[6:9]
    lum_up, lum_side, lum_down = luminance(up), luminance(side), luminance(down)
    print("  面平均亮度: 天顶=%.3f 水平=%.3f 地面=%.3f" %
          (lum_up, lum_side, lum_down))
    check("天顶比地面亮一个量级（up > 2 × down）",
          lum_up > 2.0 * lum_down, "%.3f vs %.3f" % (lum_up, lum_down))
    # 这一条是 Z-up 的守门人：天空梯度若落到 dir.y（水平方向），
    # +Y 面会变成最亮的、+Z 面反而居中，顺序立刻不成立。
    check("亮度严格递减：天顶(+Z) > 水平(+Y) > 地面(-Z)",
          lum_up > lum_side > lum_down,
          "%.3f > %.3f > %.3f" % (lum_up, lum_side, lum_down))
    check("天顶面偏蓝（b > r）", up[2] > up[0],
          "(%.3f, %.3f, %.3f)" % tuple(up))
    check("地面暗且接近中性", lum_down < 0.20, "%.3f" % lum_down)

    lut_a10, lut_b10, lut_a55, lut_b55 = g[9], g[10], g[11], g[12]
    print("  BRDF 表: (N·V=1, r=0) -> A=%.4f B=%.4f ; (0.5, 0.5) -> A=%.4f B=%.4f"
          % (lut_a10, lut_b10, lut_a55, lut_b55))
    # 理论值：V = N、roughness = 0 时 H 必然等于 N，于是 G_vis = 1、Fc = 0，
    # 积分结果精确地是 A=1、B=0。这是全链路唯一有解析解的检查点。
    check("(N·V=1, roughness=0) 处 A ≈ 1（理论值）", lut_a10 > 0.98,
          "%.4f" % lut_a10)
    check("(N·V=1, roughness=0) 处 B ≈ 0（理论值）", lut_b10 < 0.02,
          "%.4f" % lut_b10)
    check("(0.5, 0.5) 处 A 落在 (0,1)", 0.0 < lut_a55 < 1.0, "%.4f" % lut_a55)
    check("(0.5, 0.5) 处 B 落在 [0,1)", 0.0 <= lut_b55 < 1.0, "%.4f" % lut_b55)

    bx = boxes(vp)
    print("  采样区: %s" % {k: tuple(round(v) for v in b) for k, b in bx.items()})

    # ============ 第 2 组：天空盒背景 ============
    print("\n== 第 2 组：天空盒背景（MYVK_SKY=0 对照）==")
    shot_b = os.path.join(OUT_DIR, "ibl_no_sky.png")
    _, _, img_b, errs_b = run_once(exe, scene, {"MYVK_SKY": "0"}, shot_b)
    check("拿到关天空的截图", img_b is not None)
    check("零 Vulkan 验证层报错", not errs_b, errs_b[:2])
    if img_b is not None:
        d_corner = region_diff(img_a, img_b, bx["tl"])
        d_tr = region_diff(img_a, img_b, bx["tr"])
        d_object = region_diff(img_a, img_b, bx["center"])
        print("  天空开/关: 左上角差=%.1f 右上角差=%.1f 球体区差=%.1f"
              % (d_corner, d_tr, d_object))
        check("关掉天空后背景明显变化（角区平均差 > 20）",
              d_corner > 20.0 and d_tr > 20.0,
              "tl=%.1f tr=%.1f" % (d_corner, d_tr))
        # 球的环境光来自烘焙贴图，与"背景画不画"无关 —— 开关天空
        # 不该改变球的着色。这条把两个 pass 的职责彻底分开。
        check("球体着色不受天空开关影响（球心区差 < 1.0）",
              d_object < 1.0, "%.2f" % d_object)

    # ============ 第 3 组：IBL 开关 ============
    print("\n== 第 3 组：IBL 开关（MYVK_IBL=0 退回近似路径）==")
    shot_c = os.path.join(OUT_DIR, "ibl_approx.png")
    _, _, img_c, errs_c = run_once(exe, scene, {"MYVK_IBL": "0"}, shot_c)
    check("拿到关 IBL 的截图", img_c is not None)
    check("零 Vulkan 验证层报错", not errs_c, errs_c[:2])
    if img_c is not None:
        d_object = region_diff(img_a, img_c, bx["center"])
        d_corner = region_diff(img_a, img_c, bx["tl"])
        print("  IBL 开/关: 球体区差=%.1f 背景角区差=%.2f"
              % (d_object, d_corner))
        check("IBL 真的改变了金属球的着色（球心区差 > 1.5）",
              d_object > 1.5, "%.1f" % d_object)
        # 背景由 sky.frag 画，和 IBL 开关无关 —— 必须逐像素相同。
        # 对不上就说明 IBL 开关越界影响了别的 pass，是回归信号。
        check("IBL 开关不影响背景（角区差 < 1.0）",
              d_corner < 1.0, "%.2f" % d_corner)

    # ============ 第 4 组：因果实验（天顶红 / 地面蓝）============
    print("\n== 第 4 组：天顶=纯红 地面=纯蓝 的因果实验 ==")
    RED_BLUE = {"MYVK_SKY_ZENITH": "1,0,0", "MYVK_SKY_GROUND": "0,0,1"}

    # 4a：**无球**场景 —— 竖直中线上从画面上缘一路采到地平线，红/蓝比
    # 必须单调递减。这是天空方向映射的直接读数。
    sky_scene = os.path.join(OUT_DIR, "ibl_sky_test.json")
    build_scene(sky_scene, with_spheres=False)
    shot_d = os.path.join(OUT_DIR, "ibl_redblue_sky.png")
    _, _, img_d, errs_d = run_once(exe, sky_scene, dict(RED_BLUE), shot_d)
    check("拿到红蓝天空截图（无球场景）", img_d is not None)
    check("零 Vulkan 验证层报错", not errs_d, errs_d[:2])
    if img_d is not None:
        def r_over_b(c):
            return c[0] / max(c[2], 1.0)

        hi = region_mean(img_d, bx["sky_hi"])
        mid = region_mean(img_d, bx["sky_mid"])
        lo = region_mean(img_d, bx["sky_lo"])
        print("  天空采样: 上=(%.0f,%.0f,%.0f) 中=(%.0f,%.0f,%.0f) 下=(%.0f,%.0f,%.0f)"
              % (tuple(hi) + tuple(mid) + tuple(lo)))
        r_hi, r_mid, r_lo = r_over_b(hi), r_over_b(mid), r_over_b(lo)
        print("  r/b: 上=%.3f 中=%.3f 下=%.3f" % (r_hi, r_mid, r_lo))
        # 天顶是红、地面是蓝 → 越靠近天顶红色占比越高。三条必须严格递减。
        check("天空梯度朝下单调变蓝（r/b: 上 > 中 > 下）",
              r_hi > r_mid > r_lo, "%.3f > %.3f > %.3f" % (r_hi, r_mid, r_lo))
        # 不用固定倍数阈值：色调映射把高光段压平了，线性空间里 2.1× 的
        # r/b 差到了画面上只剩 1.24×。改判"逐级落差"—— 这反而是更强的
        # 判据：若梯度错落到 dir.y（水平方向），竖直线上的采样带会拿到
        # **完全相同**的 dir.y，三级落差直接是 0。
        d1, d2 = r_hi - r_mid, r_mid - r_lo
        check("相邻采样带落差明显（每级 r/b 至少降 0.02）",
              d1 > 0.02 and d2 > 0.02, "Δ=%.3f, %.3f" % (d1, d2))

    # 4b：有球场景 + 关天空 —— 此时画面上除了球就是清屏底色，
    # 采样区里的红/蓝只可能来自**球面反射**。
    shot_e = os.path.join(OUT_DIR, "ibl_redblue_sphere.png")
    env_e = dict(RED_BLUE)
    env_e["MYVK_SKY"] = "0"
    _, _, img_e, errs_e = run_once(exe, scene, env_e, shot_e)
    check("拿到红蓝环境下的球体截图", img_e is not None)
    check("零 Vulkan 验证层报错", not errs_e, errs_e[:2])
    if img_e is not None:
        up = region_mean(img_e, bx["upper"])
        lo = region_mean(img_e, bx["lower"])
        print("  球面反射: 上半=(%.0f,%.0f,%.0f) 下半=(%.0f,%.0f,%.0f)"
              % (tuple(up) + tuple(lo)))
        # 球体上半部的反射方向朝上 → 采到天顶色（红）；
        # 下半部朝下 → 采到地面色（蓝）。方向映射错一位就会反过来。
        check("球面：上半反射天顶（红占优 r > 1.5 b）",
              up[0] > 1.5 * up[2], "r=%.0f b=%.0f" % (up[0], up[2]))
        check("球面：下半反射地面（蓝占优 b > 1.5 r）",
              lo[2] > 1.5 * lo[0], "b=%.0f r=%.0f" % (lo[2], lo[0]))
        check("上下半差异显著（上下亮度差 > 20）",
              abs(luminance(up) - luminance(lo)) > 20.0,
              "%.1f vs %.1f" % (luminance(up), luminance(lo)))

    print("\n结果: %s" % ("PASS" if not fails else "FAIL: " + ", ".join(fails)))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
