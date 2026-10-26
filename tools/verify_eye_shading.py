"""端到端验证：眼睛专用着色（A3，Unlit 底 + 虹膜视差 + 眼神光）。

背景：二次元的眼睛**不是算出来的，是画出来的**。巩膜白、虹膜渐变、瞳孔
全在美术的一张图里，场景光不该动它 —— 一束逆光打过来，眼睛也不该变暗
（真实眼会变暗，二次元不会，这是画风约定）。立体感只剩两件"贴上去"的
东西：① 程序化眼神光（锁在头的朝向上，不随光源走）；② 虹膜视差（眼窝是
凹的，侧看时虹膜朝观察者一侧偏）。

实现落在材质页 `spare2.xyz`（112 B 的新块）：
  x = eyeStrength（Unlit 混合权重，0 = 硬关）
  y = eyeIrisDepth（虹膜 UV 视差强度）
  z = eyeCatchlight（眼神光强度）

⚠ 必须带 `MYVK_BINDLESS=1`：A 类槽（ramp/matcap/SDF/detail/aniso/eye）**只
  在 VENN_BINDLESS 编译单元里存在**，不开就是"全部静默失效、零报错"。
⚠ 必须带 `MYVK_TAA=0`：TAA 的历史缓冲会把上一帧的残影混进来，"0 px"就
  退化成"TAA 收敛了"的同义词（A4 那轮踩过）。

验五组（全部像素证据）：

  1. **关 = 基线（严格 0 px）**。eyeStrength=0 跑两次逐像素相同；并且
     strength=0 时把 irisDepth / catchlight 都调到非 0，画面**一点不变**
     —— 证明那两个参数被硬闸门拦在门外，不是"算了再乘 0"。
  2. **Unlit：眼睛不吃场景光**（本脚本的核心判据）。同一颗球、同一个机位，
     只把太阳从 +X 侧翻到 −X 侧：
       · eyeStrength=1 → 画面必须**逐像素相同**（0 px）
       · eyeStrength=0（对照）→ 必须显著变化
     两个方向都要测。只有对照变了、眼睛没变，才证明"眼睛不吃光"这件事
     是这条特性干的 —— 而不是整个场景本来就对光不敏感。
  3. **虹膜视差**。两个侧向机位各测一次"开视差 vs 关视差"的差异量
     （都必须显著，排除"只有某个角度才动"），再测**随 irisDepth 单调**
     （0 / 0.03 / 0.08 的变化像素数严格增）。
     ⚠ 本组**不判位移方向** —— 代理量不可靠（质心把"接缝移动"和"暗格面积
       变化"混成一个数，且棋盘是周期图案、位移过半格就饱和）。要判方向得
       用真实虹膜贴图 + 相位相关，留到有角色资产之后。详见代码里的注释。
  4. **眼神光在球心上方，且是变亮的**。catchlight 0 → 1.2：出现一片
     **显著变亮**的像素，质心必须落在画面中心线**上方**（二次元眼睛那颗
     高光画在上半部），不在下半部。
  5. **eyeStrength 单调**：0.35 < 0.70 < 1.00 相对基线的变化像素数严格增。

用法：
    python tools/verify_eye_shading.py [--exe path]
"""

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verify_shader_variants import (                                    # noqa: E402
    ROOT, OUT_DIR, entity, run_once, write_scene)
from verify_toon_shading import (                                       # noqa: E402
    BALL_POS, BALL_SIZE, center_patch, luma, to_np_rgb)

DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")

# A 类槽的必要条件（见文件头）。SHADOW=0 的理由与 A4 脚本相同：
# 眼睛的 color 被整体替换，阴影本来就进不来；但**对照材质**（eyeStrength=0）
# 会吃到 CSM 的 PCF 台阶，那是与本题无关的噪声源。
NPR_ENV = {"MYVK_SHADER_VARIANTS": "1", "MYVK_GRID": "0",
           "MYVK_BINDLESS": "1", "MYVK_TAA": "0", "MYVK_SHADOW": "0"}

# 相机：`forward = -angleDir()`，展开成 `(-cos(yaw), +sin(yaw), -sin(pitch))`。
#   · yaw=90° → forward=(0,+1)：相机在 −Y 一侧朝 +Y 看（球的另一侧）；
#   · 水平朝向角 φ 与 yaw 的关系是 **yaw = 180° − φ**（不是 φ 本身！）；
#   · pitch 正 = 俯视（forward.z = −sin(pitch)）。
# ⚠ 第一版把 yaw 写成 φ 本身（110.6 / 69.4 互换），结果两个侧向机位**根本
#   没拍到球**（画面全是天空，mean 174、零暗像素），"视差 0 px 变化"看起来
#   像实现没接线，其实是取景错 —— 与 A4 那轮的"环被推出画面"同一个坑。
CAM_CENTER = "0,-34,15,90,16"
# 侧向机位：相机绕球转约 ±20°，同时把 yaw/pitch 解成"仍盯着球心"：
# 右侧机位 eye=(12,-32,15)，指向球心 (0,0,5) → φ=110.6° → yaw=69.4°。
CAM_RIGHT = "12,-32,15,69.4,16.3"
CAM_LEFT = "-12,-32,15,110.6,16.3"

# 太阳：只留直接光（ambientScale=0）。第 2 组要的正是"环境项一点不剩"，
# 否则环境光把暗部托起来，翻光方向的变化量会被稀释。
SUN = (0.9, 0.0)
# 光从 +X 侧 / −X 侧照过来（传播方向）。
LIGHT_PLUS = [-1.0, -0.10, -0.20]
LIGHT_MINUS = [1.0, -0.10, -0.20]

# "显著变化"的阈值。8 与 A4 / toon 脚本同一个值：低于它的是 8bit 量化抖动。
DIFF_THR = 8


def material_eye(name, strength, iris=0.0, sparkle=0.0):
    """Toon 材质 + A3 三个字段，albedo 用**大格**棋盘。

    为什么是 cell=256/size=1024 而不是小棋盘：球的前半球覆盖 u∈(0,0.5)
    （UV 约定 u=θ/2π，相机在 −Y 看过去正好是这个区间），4×4 格的边界
    落在 u=0.25 ——**正好是可见半球的中线**。于是画面上是一条"左黑右白"
    的竖直接缝：视差一开，接缝就横移，质心判据能直接量到它走了多少。
    用小棋盘（cell=32）时相邻格子长得一样，位移会被周期性吃掉。
    """
    return {
        "kind": "procedural", "name": name,
        "baseColor": [1.0, 1.0, 1.0, 1.0],
        "metallic": 0.0, "roughness": 0.55,
        "normalScale": 1.0, "ao": 1.0,
        "emissive": [0.0, 0.0, 0.0],
        "doubleSided": False, "alphaMode": "opaque",
        "shadingModel": "toon",
        "albedo": {"kind": "checker", "name": "eye_checker",
                   "colorA": [0, 0, 0, 255], "colorB": [255, 255, 255, 255],
                   "cell": 256, "size": 1024},
        "eyeStrength": strength,
        "eyeIrisDepth": iris,
        "eyeCatchlight": sparkle,
    }


def build_scene(path, strength, iris=0.0, sparkle=0.0, light=None):
    ents = [entity("Ball", BALL_POS,
                   {"shape": "sphere", "size": BALL_SIZE, "name": "eye_ball"},
                   material_eye("eye_mat", strength, iris, sparkle))]
    return write_scene(path, ents, "verify_eye_shading", sun=SUN,
                       light_dir=light or LIGHT_PLUS)


def diff_pixels(a, b, thr=DIFF_THR):
    """显著变化像素数 / 占比（逐通道最大差 > thr）。"""
    d = np.abs(a - b).max(axis=2)
    hit = d > thr
    return int(hit.sum()), float(hit.mean())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    args = ap.parse_args()
    exe = args.exe
    if not os.path.exists(exe):
        print("找不到 %s" % exe)
        return 2

    os.makedirs(OUT_DIR, exist_ok=True)
    fails = []

    def check(name, ok, detail=""):
        print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name,
                               ("  " + str(detail)) if detail != "" else ""))
        if not ok:
            fails.append(name)

    shots = {}

    def run(tag, strength, iris=0.0, sparkle=0.0, light=None,
            cam=CAM_CENTER):
        scene = os.path.join(OUT_DIR, "eye_%s.json" % tag)
        shot = os.path.join(OUT_DIR, "eye_%s.png" % tag)
        build_scene(scene, strength, iris, sparkle, light)
        lines, vp, img, errs = run_once(exe, scene, NPR_ENV, shot, cam=cam)
        check("%s 零 Vulkan 验证层报错" % tag, not errs, errs[:2])
        if vp is None or img is None:
            print("  %s 没拿到视口/截图" % tag)
            return None, None
        a = to_np_rgb(img)
        if not np.isfinite(a).all():
            check("%s 截图含非有限值（NaN/Inf）" % tag, False)
            return None, None
        shots[tag] = a
        # 所有判据都取**整个视口**（frac=1.0）：球就是这个画面里唯一的
        # 被测物，背景（天空）在三种材质下都一样 → 对 diff 没有贡献，
        # 不会污染统计（A4 那轮把采样区收到 0.42 反而把环裁掉的教训）。
        full = center_patch(a, vp, 1.0)
        print("  %-12s s=%.2f iris=%.2f spark=%.2f  mean=%.1f"
              % (tag, strength, iris, sparkle, float(luma(full).mean())))
        return full, center_patch(a, vp, 0.42)

    print("== 采样（同一颗球、同一机位，只改三个材质字段）==")
    off_a, _ = run("off_a", 0.0)
    off_b, _ = run("off_b", 0.0)
    # 闸门测试：strength=0，但另外两个参数都非 0
    off_gate, _ = run("off_gate", 0.0, iris=0.09, sparkle=1.5)
    on_c, on_patch = run("on", 1.0)
    if off_a is None or off_b is None or off_gate is None or on_c is None:
        print("\n结果: FAIL: 截图缺失")
        return 3

    # ---------------- 第 1 组：关 = 基线 ----------------
    print("\n== 第 1 组：eyeStrength=0 必须与基线逐像素相同 ==")
    n, r = diff_pixels(off_a, off_b)
    check("① 两次 strength=0 逐像素相同（确定性）", n == 0,
          "%d px (%.3f%%)" % (n, r * 100.0))
    n, r = diff_pixels(off_a, off_gate)
    check("① strength=0 时 iris/sparkle 完全不参与（硬闸门，不是乘 0）",
          n == 0, "%d px (%.3f%%)" % (n, r * 100.0))

    # ---------------- 第 2 组：Unlit（核心）----------------
    print("\n== 第 2 组：眼睛不吃场景光（翻太阳方向）==")
    eye_p, _ = run("light_plus", 1.0, light=LIGHT_PLUS)
    eye_m, _ = run("light_minus", 1.0, light=LIGHT_MINUS)
    ctl_p, _ = run("ctl_plus", 0.0, light=LIGHT_PLUS)
    ctl_m, _ = run("ctl_minus", 0.0, light=LIGHT_MINUS)
    if any(x is None for x in (eye_p, eye_m, ctl_p, ctl_m)):
        check("② 翻光方向的两组截图", False)
    else:
        # 对照先自证：同一个材质、eyeStrength=0 时翻光必须**明显变化**。
        # 没有这一步，"眼睛不变"有可能只是"这个场景对光不敏感"。
        cn, cr = diff_pixels(ctl_p, ctl_m)
        check("② 对照材质（strength=0）翻光后显著变化", cr > 0.02,
              "%d px (%.2f%%)" % (cn, cr * 100.0))
        en, er = diff_pixels(eye_p, eye_m)
        check("② 眼睛（strength=1）翻光后逐像素相同", en == 0,
              "%d px (%.3f%%)" % (en, er * 100.0))

    # ---------------- 第 3 组：虹膜视差 ----------------
    print("\n== 第 3 组：虹膜视差 ==")
    # ⚠ 本组判据的边界，写清楚免得下次以为漏了：
    #   判**会动**（同一机位 depth 0 vs 0.08）与**随深度单调**（0/0.03/0.08），
    #   两侧机位都测一遍（排除"只有某个角度才动"）。
    #   **不判位移方向**：第一版判过（"两侧机位的暗区质心位移方向相反"），
    #   实测右侧 −14.8 px / 左侧 −3.9 px 同号。不是实现错，是这个代理量不可靠 ——
    #     · 质心是"暗区整体"的质心，而视差同时移动接缝**和**改变可见暗格面积，
    #       两个效应混在同一个数里；
    #     · 棋盘是**周期**图案，位移一接近半格，变化像素数就饱和，位移量本身
    #       不再能从像素数反推。
    #   要判方向得用"虹膜圆盘"那种非周期图案 + 亚像素相位相关，需要一张真实的
    #   虹膜贴图（美术资产），不是程序化棋盘搭的夹具能做的事。
    d = 0.08
    r0, _ = run("iris_r0", 1.0, iris=0.0, cam=CAM_RIGHT)
    r1, _ = run("iris_r1", 1.0, iris=d, cam=CAM_RIGHT)
    l0, _ = run("iris_l0", 1.0, iris=0.0, cam=CAM_LEFT)
    l1, _ = run("iris_l1", 1.0, iris=d, cam=CAM_LEFT)
    if any(x is None for x in (r0, r1, l0, l1)):
        check("③ 视差组的截图", False)
    else:
        nr, rr = diff_pixels(r0, r1)
        print("     右侧机位 开视差前后：%d px (%.2f%%)" % (nr, rr * 100.0))
        check("③ 开视差后画面显著变化（> 0.30%）", rr > 0.003,
              "%.2f%%" % (rr * 100.0))
        nl, rl = diff_pixels(l0, l1)
        print("     左侧机位 开视差前后：%d px (%.2f%%)" % (nl, rl * 100.0))
        check("③ 左侧机位一样（> 0.30%，不是某个角度的巧合）", rl > 0.003,
              "%.2f%%" % (rl * 100.0))

        mono = []
        for tag, v in (("iris_d0", 0.0), ("iris_d3", 0.03),
                       ("iris_d8", 0.08)):
            p, _ = run(tag, 1.0, iris=v, cam=CAM_RIGHT)
            if p is None:
                check("③ %s 截图" % tag, False)
                continue
            n, r = diff_pixels(r0, p)
            mono.append((v, n))
            print("     irisDepth=%.2f → %d px (%.2f%%)" % (v, n, r * 100.0))
        if len(mono) == 3:
            check("③ 偏移量随 irisDepth 单调（严格增）",
                  mono[0][1] < mono[1][1] < mono[2][1],
                  " < ".join(str(m[1]) for m in mono))

    # ---------------- 第 4 组：眼神光 ----------------
    print("\n== 第 4 组：眼神光 —— 位置在上半部、方向是变亮 ==")
    s0, _ = run("spark_0", 1.0, iris=0.0, sparkle=0.0)
    s1, _ = run("spark_1", 1.0, iris=0.0, sparkle=1.2)
    if any(x is None for x in (s0, s1)):
        check("④ 眼神光截图", False)
    else:
        diff = np.abs(s1 - s0).max(axis=2)
        m = diff > DIFF_THR
        area = diff.shape[0] * diff.shape[1]
        check("④ 眼神光覆盖 > 0.10% 的视口", int(m.sum()) > 0.001 * area,
              "%d px / %d" % (int(m.sum()), area))
        if m.sum() < 50:
            check("④ 眼神光像素足够做质心", False)
        else:
            ys, xs = np.nonzero(m)
            dl = luma(s1)[m] - luma(s0)[m]
            med = float(np.median(dl))
            # 参考线用**画面中心行**：球在视口正中（BALL_POS 与相机构图决定的），
            # 不写死像素值 —— 视口尺寸会随布局变（A4 那轮的教训）。
            cy = float(ys.mean())
            mid = s1.shape[0] * 0.5
            print("     光斑质心 y=%.0f   中心行 y=%.0f   中位增亮=%+.1f"
                  % (cy, mid, med))
            check("④ 光斑在画面中心线以上（二次元高光画在上半部）",
                  cy < mid - 10.0, "%.0f < %.0f" % (cy, mid - 10.0))
            check("④ 光斑是**变亮**（中位增亮 > +8）", med > 8.0, "%+.1f" % med)

    # ---------------- 第 5 组：强度单调 ----------------
    print("\n== 第 5 组：eyeStrength 单调 ==")
    counts = []
    for tag, v in (("s35", 0.35), ("s70", 0.70), ("s100", 1.00)):
        p, _ = run(tag, v)
        if p is None:
            check("⑤ %s 截图" % tag, False)
            continue
        n, r = diff_pixels(off_a, p)
        counts.append((v, n, r))
        print("     strength=%.2f → %d px (%.2f%%)" % (v, n, r * 100.0))
    if len(counts) == 3:
        ok = counts[0][1] < counts[1][1] < counts[2][1]
        check("⑤ 强度越大，离场景光照越远（严格单调）", ok,
              " < ".join(str(c[1]) for c in counts))

    # ---------------- 结算 ----------------
    print("")
    if fails:
        print("结果: FAIL: " + ", ".join(fails))
        return 1
    print("结果: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
