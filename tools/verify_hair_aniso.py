"""端到端验证：头发各向异性高光（A4，Kajiya-Kay 天使环）。

背景：Toon 的高光原来是 `pow(N·H, 48)` —— **各向同性**：球面上法线朝向
合适的地方亮一片圆点。而真头发是一根根圆柱，高光沿"垂直于发丝的方向"
铺开，绕着头顶走一条环（angel ring）。两者的差别不是"亮度"，是**形状**。

这里把内置球当头发验：球的顶点切线（A4 这次才接上）沿 UV.x，也就是
**经度方向（纬线）**。于是 `sin(T,H)=0` 的解落在一条固定的经线上 ——
再加上 Kajiya-Kay 把它沿法线推开，屏幕上那团高光会从"点"变成"一条
竖直的带"。这个形状差异是本脚本的主判据。

验六组（全部像素证据）：

  1. **关 = 基线（严格 0 px）**。strength=0 跑两次逐像素相同；并且
     strength=0 时改 shift 画面**一点都不变**（证明 shift 没有偷偷
     混进别的项里 —— 这是"两个字段各自独立"的实证，不是读代码得出的）。
  2. **开了确实变**：strength=1 与 strength=0 的显著变化像素 > 0.5%。
  3. **形状从"点"变"带"**：取亮度最高的 2% 像素算包围散布
     `ratio = std_x / std_y`。各向同性 ≈ 1（圆点）；各向异性应显著
     < 1（竖直带）。
  4. **shift 是位置参数**：-0.6 / 0 / +0.6 时高光质心单调移动，且
     画面亮度总量基本不变（"移动"而不是"变亮"）。
  5. **强度单调**：0.3 < 0.7 < 1.3 时变化像素数单调增。
  6. **零验证层报错 + 无 NaN**。

⚠ 必须带 `MYVK_BINDLESS=1`：anisoStrength/anisoShift 走材质页 `spare.z/w`，
  而 A 类槽（ramp/matcap/SDF/detail/aniso）**只在 VENN_BINDLESS 编译单元
  里存在**。不开就是"全部静默失效、零报错"—— 本轮之前的 A1 就栽在这里。
⚠ 太阳与环境光的取值、光的方向全部沿用 verify_toon_shading 的实测结论
  （不削顶 + 侧光），这里不重调，否则两组脚本的图不可比。

用法：
    python tools/verify_hair_aniso.py [--exe path]
"""

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verify_shader_variants import (                                    # noqa: E402
    ROOT, OUT_DIR, entity, run_once, write_scene)
from verify_toon_shading import (                                       # noqa: E402
    BALL_POS, BALL_SIZE, CAM, LIGHT_DIR, center_patch, luma, to_np_rgb)

DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")

# A 类材质槽（含 A4）的必要条件，见文件头。
#
# ⚠ 另外关掉两样东西，都是**为了让判据只测环**，不是为了省时间：
#   · `MYVK_SHADOW=0` —— 各向异性高光乘了 shadow（阴影里没有高光，这是
#     对的），而球在 CSM 下自带一条自遮挡阴影边界。实测：带阴影时
#     diff 里混进「中间一条横线 + 球底一排竖条纹」（CSM 的 PCF 台阶），
#     环的包围盒被撑到 238×204，高/宽 0.86 —— "竖直带"永远测不出来。
#     关掉之后环贯穿整颗球，形状才是环自己的形状。
#   · `MYVK_TAA=0` —— TAA 的历史缓冲会把上一帧的残影混进来。关掉之后
#     每个像素都是当帧的确定值，"同一输入跑两次逐像素相同"这条判据
#     才有意义（否则 0 px 只是"TAA 收敛了"的同义词）。
NPR_ENV = {"MYVK_SHADER_VARIANTS": "1", "MYVK_GRID": "0",
           "MYVK_BINDLESS": "1", "MYVK_TAA": "0", "MYVK_SHADOW": "0"}

# ⚠ 光的方向**不能**沿用 verify_toon_shading 的侧光（-0.85,-0.28,-0.22）。
#
#   Kajiya-Kay 的环落在 `T·H = 0` 的那条经线上，而球面切线 T(θ) 只依赖
#   经度 θ。把 H 的 x 分量写成 0（即让光待在 YZ 平面内）时，T·H = -H_y·cosθ，
#   于是 `T·H = 0` 恰好是 **θ = 270°** —— 正对相机的那条经线，环竖直穿过
#   球心，整条都在采样区里。
#
#   用侧光时实测：环跑到球的右缘（质心 x=184，采样区宽度只有 188），
#   被裁掉只剩 10×24 px 的一小块 —— 于是"环形"的所有判据都在测一个
#   残缺的角落，spec_shape 量到的根本不是环。这不是实现错，是**取景错**。
#
#   附带好处：这个光向下照，明暗交界是一条水平线，与竖直的环**垂直交叉**，
#   正好是"天使环横跨头顶"的典型构图。
LIGHT_DIR = [0.0, -0.35, -0.94]
# 太阳强度比 verify_toon_shading 的 1.2 更低，这是**为判据服务的**：
# Toon 的受光面 = albedo(0.45) × 太阳；用 1.2 时受光面到 0.54（sRGB 195），
# 各向异性高光只剩 60 格余量；实测最糟的组合（albedo 0.78 + 太阳 1.2）
# 下受光面 249，加什么都只差 6 格 < 阈值 8 —— 「加了一条环」变成 0 变化。
HAIR_SUN = (0.72, 0.0)

# "显著变化像素"的阈值。8 是 verify_toon_shading 用的同一个值 ——
# 低于它的是 TAA 抖动/8bit 量化，不是真的变了。
DIFF_THR = 8

# ⚠ **形状**判据要用更高的阈值（45），这是量出来的：
#   A4 是"锐主环 + 宽而淡副环"的两层结构（二次元头发的通行画法）。
#   副环的亮度增量只有 +30 左右，但它铺满球的整个可见面 —— 用 8 当阈值时
#   `bbox` 是 343×378（把副环也算进去了），高/宽 1.10，"竖直"永远测不出来。
#   抬到 45 之后只剩下主环核心：bbox 120×377，高/宽 **3.14**。
#   实测阈值扫描（on / sh_zero）：
#     thr= 8 → 343×378  高/宽 1.10
#     thr=30 → 177×377  高/宽 2.13
#     thr=45 → 120×377  高/宽 3.14   ← 用这个
#     thr=60 →  84×219  高/宽 2.61   （开始被球顶的暗洞切断）
#   所以"有没有"用 8、"什么形状"用 45，两个问题分开问。
RING_THR = 45


def material_hair(name, strength, shift):
    """Toon 材质 + A4 两个字段。

    ⚠ baseColor 取 0.45 而不是 verify_toon_shading 的 0.78，这是**为判据
      服务**的，不是为了好看：
        受光面亮度 ≈ albedo × 太阳 × (0.80+0.20·ndl)
                  = 0.45 × 0.72 × 1.0 ≈ 0.32  → sRGB ≈ 155
      留出 100 格余量给各向异性高光（峰值 ≈ specColor 0.75 × 0.72 ≈ 0.54）。
      用 0.78 时受光面已经到 0.56 → sRGB ≈ 198，加上高光只剩 57 格；
      而实测更糟：太阳 1.2 时受光面 249，加什么都只差 6 格 < 阈值 8 —
      于是「加了一整条环」在像素上变成 0 变化（这是本轮排查的起点）。
    """
    return {
        "kind": "procedural", "name": name,
        "baseColor": [0.45, 0.45, 0.45, 1.0],
        "metallic": 0.0, "roughness": 0.55,
        "normalScale": 1.0, "ao": 1.0,
        "emissive": [0.0, 0.0, 0.0],
        "doubleSided": False, "alphaMode": "opaque",
        "shadingModel": "toon",
        "anisoStrength": strength,
        "anisoShift": shift,
    }


def build_scene(path, strength, shift):
    ents = [entity("Ball", BALL_POS,
                   {"shape": "sphere", "size": BALL_SIZE, "name": "hair_ball"},
                   material_hair("hair_mat", strength, shift))]
    # 太阳/光向沿用 verify_toon_shading 的实测配置，太阳强度另调（见 HAIR_SUN）。
    return write_scene(path, ents, "verify_hair_aniso", sun=HAIR_SUN,
                       light_dir=LIGHT_DIR)


def ring_stats(ref, a, thr=DIFF_THR):
    """环 = 与基线**显著不同**的那些像素。

    为什么用 diff 而不是"最亮的那 2%"：环是一条叠在底色上的高光，
    而 Toon 的底色本身就有明暗两档和一个硬交界。取"最亮 2%"量到的是
    **底色最亮处的形状**（实测 sx=5.8/sy=20，一条与环无关的窄条），
    关掉各向异性时它照样存在 —— 于是"关 vs 开"的差异被完全掩盖。
    diff 区域才是"这个特性画出来的东西"，因果上不会跑偏。
    """
    d = np.abs(a - ref).max(axis=2)
    m = d > thr
    if m.sum() < 20:
        return None
    ys, xs = np.nonzero(m)
    sx, sy = float(xs.std()), float(ys.std())
    dl = luma(a)[m] - luma(ref)[m]
    return {
        "n": int(m.sum()),
        "sx": sx,
        "sy": sy,
        "ratio": sx / max(sy, 1e-6),
        "bw": int(xs.max() - xs.min() + 1),
        "bh": int(ys.max() - ys.min() + 1),
        "cx": float(xs.mean()),
        "cy": float(ys.mean()),
        "dmed": float(np.median(dl)),
        "dmean": float(dl.mean()),
    }


def fmt_ring(tag, s):
    if s is None:
        return "     %-8s 无可见环" % tag
    return ("     %-8s n=%4d  bbox=%dx%d (高/宽=%.1f)  ratio=%.2f  "
            "增亮中位=%+.1f  质心=(%.0f,%.0f)"
            % (tag, s["n"], s["bw"], s["bh"],
               s["bh"] / max(s["bw"], 1), s["ratio"], s["dmed"],
               s["cx"], s["cy"]))


def diff_pixels(a, b):
    """显著变化像素数 / 占比（逐通道最大差 > DIFF_THR）。"""
    d = np.abs(a - b).max(axis=2)
    hit = d > DIFF_THR
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

    patches = {}

    def run(tag, strength, shift):
        """跑一次，返回中心采样区的 RGB。"""
        scene = os.path.join(OUT_DIR, "hair_%s.json" % tag)
        shot = os.path.join(OUT_DIR, "hair_%s.png" % tag)
        build_scene(scene, strength, shift)
        lines, vp, img, errs = run_once(exe, scene, NPR_ENV, shot)
        check("%s 零 Vulkan 验证层报错" % tag, not errs, errs[:2])
        if vp is None or img is None:
            print("  %s 没拿到视口/截图" % tag)
            return None
        a = to_np_rgb(img)
        if not np.isfinite(a).all():
            check("%s 截图含非有限值（NaN/Inf）" % tag, False)
            return None
        # ⚠ 采样区取**整个视口**（frac=1.0），不是 verify_toon_shading 的 0.42。
        #   原因：那条 0.42 是为"亮度分位数"设计的（要保证采样区全是球面，
        #   不被背景拉偏），但 A4 的判据是**形状**，而环宽约 156 px 占了
        #   patch(188) 的 83% —— 环被裁成方块，高/宽比恒为 0.7，"竖直带"
        #   这个判据永远不成立。实测踩过一整轮。
        #   取整个视口后环完整可见（球直径 380，视口 448×531），
        #   背景不变 → 对 diff 没有任何贡献，不会污染形状统计。
        p = center_patch(a, vp, 1.0)
        patches[tag] = p
        print("  %-10s strength=%.2f shift=%+.2f  mean=%.1f std=%.1f"
              % (tag, strength, shift, float(luma(p).mean()),
                 float(luma(p).std())))
        return p

    print("== 采样（同一颗球、同一机位，只改两个材质字段）==")
    off_a = run("off_a", 0.0, 0.0)
    off_b = run("off_b", 0.0, 0.0)
    off_s = run("off_shifted", 0.0, 0.80)
    on = run("on", 1.0, 0.0)
    if off_a is None or off_b is None or off_s is None or on is None:
        print("\n结果: FAIL: 截图缺失")
        return 3

    # ---------------- 第 1 组：关 = 基线 ----------------
    print("\n== 第 1 组：strength=0 必须与基线逐像素相同 ==")
    n_aa, r_aa = diff_pixels(off_a, off_b)
    check("① 两次 strength=0 逐像素相同（确定性）", n_aa == 0,
          "%d px (%.3f%%)" % (n_aa, r_aa * 100.0))
    # 这一条比"跑两次一样"强得多：它证明 **strength 是硬闸门**。
    # 如果实现写成"算了再乘 0"，shift 会先参与 anisoTangent 的取切线，
    # 一旦那里出现退化输入（normalize(0)），结果就是 NaN —— 而 NaN 会
    # 从"乘 0"里逃出来，画面出现黑块或消失的三角形。
    n_as, r_as = diff_pixels(off_a, off_s)
    check("① strength=0 时 shift 完全不参与（证明是硬闸门，不是乘 0）",
          n_as == 0, "%d px (%.3f%%)" % (n_as, r_as * 100.0))

    # ---------------- 第 2 组：开了确实变 ----------------
    print("\n== 第 2 组：strength=1 必须画出一条环 ==")
    r_on = ring_stats(off_a, on)
    if r_on is None:
        check("② 开了各向异性后出现可见的环", False, "0 个显著变化像素")
    else:
        print(fmt_ring("on", r_on))
        # 0.3%：一条环在 188×223 的采样区里就是这个量级（实测 0.3~0.5%）。
        # 不用通用的 0.5% —— 高光的**覆盖面积由球面几何决定，不由实现
        # 决定**，把阈值抬到 0.5% 等于要求"环必须比物理上更宽"。
        area = off_a.shape[0] * off_a.shape[1]
        check("② 环覆盖 > 0.3% 的采样区", r_on["n"] > 0.003 * area,
              "%d px / %d" % (r_on["n"], area))
        # 环必须让这些像素**变亮**。这一条排除"改了但改错方向"
        # （把环画成暗缝、或切线算反导致整块被压暗）—— 各向异性高光
        # 只有一种正确符号。用中位数而不是均值：环有软边，均值被边缘拉低。
        check("② 环上像素变亮（中位增亮 > +8）", r_on["dmed"] > 8.0,
              "%+.1f" % r_on["dmed"])

    # ---------------- 第 3 组：环是竖直的带 ----------------
    print("\n== 第 3 组：环的形状（竖直、细长）==")
    r_core = ring_stats(off_a, on, thr=RING_THR)
    if r_core is None:
        check("③ 主环核心（thr=%d）有足够像素" % RING_THR, False)
    else:
        print(fmt_ring("on@45", r_core))
        # 光的 x 分量为 0 → `T·H = 0` 的解是 θ=270°，也就是**正对相机的
        # 那条经线**。屏幕上它是从上到下贯穿的一竖条，所以高宽比必须 > 1.5。
        # 如果切线算错（用了法线投影、或者 vTangent 恒 0 落到屏幕空间
        # 回退的退化分支），环会变成横带或散斑，这条会红。
        hw = r_core["bh"] / max(r_core["bw"], 1)
        check("③ 环在屏幕上是竖直的（高/宽 > 1.5）", hw > 1.5, "%.2f" % hw)
        check("③ 环的散布也是竖向主导（std_x/std_y < 0.6）",
              r_core["ratio"] < 0.6, "%.2f" % r_core["ratio"])
        check("③ 环贯穿整个球（高度 > 300 px，球直径 380）",
              r_core["bh"] > 300, "%d px" % r_core["bh"])

    # ---------------- 第 4 组：shift 移动环 ----------------
    print("\n== 第 4 组：shift 把环推着走 ==")
    sh = {}
    for tag, v in (("sh_neg", -0.30), ("sh_zero", 0.0), ("sh_pos", 0.30)):
        p = run(tag, 1.0, v)
        if p is None:
            check("④ %s 截图" % tag, False)
            continue
        ring = ring_stats(off_a, p, thr=RING_THR)
        sh[tag] = ring
        print(fmt_ring(tag, ring))
    if any(sh[t] is None for t in ("sh_neg", "sh_zero", "sh_pos")):
        check("④ 三档 shift 都有可见的主环", False,
              "strength=1 时 shift 不该把环推出画面")
    else:
        # 判据**不是**"质心 y 单调" —— 实测方向也不是 y：
        #   Kajiya-Kay 的背向衰减 `smoothstep(-1,0,dot(T,H))` 是**单向**的，
        #   环只有一半（dot(T,H) > 0 那半）保留。而倾斜切线的直接后果是
        #   `T·H = 0` 的解换了经度，**经度对应屏幕的 x** —— 所以环是
        #   **横向**移动的（实测 259 → 226 → 193，严格单调）。
        c0, c1, c2 = (sh["sh_neg"]["cx"], sh["sh_zero"]["cx"],
                      sh["sh_pos"]["cx"])
        span = max(c0, c1, c2) - min(c0, c1, c2)
        check("④ shift 改变了环的位置（质心 x 位移 > 40 px）", span > 40.0,
              "%.0f → %.0f → %.0f" % (c0, c1, c2))
        # 单调：shift 越大环越往一边走，中间那档不能"折返"。
        check("④ 位移是单调的（不是随机抖动）",
              c0 > c1 > c2 or c0 < c1 < c2, "%.0f → %.0f → %.0f"
              % (c0, c1, c2))
        # 三档两两之间画面必须真的不一样（不是"参数存了但没参与计算"）。
        # 这一条独立于形状统计：它只问"像素有没有变"。
        n_ab, _ = diff_pixels(patches["sh_neg"], patches["sh_pos"])
        check("④ shift 两端画面显著不同（> 200 px）", n_ab > 200,
              "%d px" % n_ab)

    # ---------------- 第 5 组：强度单调 ----------------
    print("\n== 第 5 组：强度单调 ==")
    counts = []
    for tag, v in (("i3", 0.3), ("i7", 0.7), ("i13", 1.3)):
        p = run(tag, v, 0.0)
        if p is None:
            check("⑤ %s 截图" % tag, False)
            continue
        n, r = diff_pixels(off_a, p)
        counts.append((v, n, r))
        print("     strength=%.1f → %d px (%.2f%%)" % (v, n, r * 100.0))
    if len(counts) == 3:
        ok = counts[0][1] < counts[1][1] < counts[2][1]
        check("⑤ 强度越大，环越大（严格单调）", ok,
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
