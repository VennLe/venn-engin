"""端到端验证：二次元素描着色（ShadingModel::Toon / 关键字 bit6）。

背景：引擎原来是纯 PBR（Default = 金属-粗糙度 BRDF，Skin = 预积分次表面散射），
2026-10-25 加入第三档 Toon。Toon 不是"加个参数"，是**换一条漫反射响应**：

    half-lambert → 两档量化 → 阴影乘冷色调

所以它必须是编译期变体（`VENN_SHADING_TOON`，bit6），不能是运行时 uniform ——
否则 Default 材质要多付一次分支，而分支两边是互斥的物理/非物理语汇。

本脚本验三件事（全部是像素证据，不看"应该"看"实际"）：

  1. **变体路由**：场景里写 `shadingModel: "toon"` 的材质，日志必须出现
     `k64[...shading-toon]`（bit6），且与 skin（bit4）**不共存** ——
     两者语义互斥，同时置位的掩码在 CMake 白名单里根本不存在。

  2. **量化真的发生了**：同一颗球、同一机位，Default 与 Toon 各渲一次。
     Toon 的画面必须与 Default 显著不同（否则分支没生效），而且 Toon 的
     **亮度分布更两极**（标准差 ≥ 1.15 倍）：两档量化把连续中间调推向两端。
     ⚠ 别用"总变差更低"当判据 —— 实测是反的（Toon 的硬高光制造了更大
       局部梯度），理由见第 2 组代码里的注释。

  3. **阴影有色且偏冷**：取全画面暗部像素（亮度 10%~35% 分位）算平均 R 与 B/R。
     PBR 的背光面 max(N·L,0)=0 → 只剩环境光，实测 R ≈ 0（死黑）；
     Toon 的半兰伯特让背光面停在 0.5 附近并乘 (0.52,0.56,0.68)，
     于是暗部**有颜色**且偏蓝（B/R 理论 1.31）。
     这是"二次元的阴影有颜色、而不是没有光"最直接的可测证据 ——
     也是它和写实渲染最本质的差别，而且是**质变**而非量变。

⚠ 太阳强度调高、环境光调到 0：让方向光主导，否则 IBL 会把暗部拉灰，
  B/R 差异会被稀释到测不出来。这不是为了"好看"，是为了让判据可分辨。
⚠ 但强度不能高到**削顶**：2.6 时受光面全是 255、暗部全是 0，
  标准差与色比同时被截断，测出来的数好看但没意义。现在取 1.2。
⚠ 球心放在相机视线落点上（见 CAM / BALL_POS / BALL_SIZE 的实测说明），
  画面中心那块就是球 —— 不依赖任何屏幕坐标推算。

用法：
    python tools/verify_toon_shading.py [--exe path]
"""

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verify_shader_variants import (                                    # noqa: E402
    ROOT, OUT_DIR, entity, run_once, write_scene)

DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")

# 相机在 (0,-34,15)，视线朝 +Y、下俯 16°：
#   方向 = (0, cos16°, -sin16°) = (0, 0.961, -0.276)
#   球心放在 (0,0,5) —— 实测球正好落在视口正中（见下面的 size 说明）。
CAM = "0,-34,15,90,16"
BALL_POS = (0.0, 0.0, 5.0)
# ⚠ 球必须**盖住**中心采样区，否则统计到的是天空/网格背景。
#   这个值不是推导出来的，是量出来的（tools/_probe_ball_geometry.py：
#   把球挪到相机背后渲一张当背景，两图相减得到球的真实轮廓）：
#     size=10 → 球直径约 380 px，视口 448x531，球心与视口中心重合；
#     中心 patch（0.42×视口 = 188x223）完全落在球内。
#   踩过的两个坑：
#     · size=5  球只有约 60 px，patch 里 93% 是背景 → "暗部统计"测的是天空，
#               三种着色模型都给出 R=0.0（背景跟着色模型无关），判据全废；
#     · size=20 球直径超过视口高度，相机几乎贴在球面上，球内容溢出画面。
BALL_SIZE = 10.0
# 太阳强度：要**不削顶**。albedo 0.78 × intensity 1.2 ≈ 0.94（受光面留一点余量），
# 暗部 ≈ 0.49 —— 两档都落在 8bit 的有效区间里，量出来的 R/B 才有意义。
# 强度 2.6 时受光面直接削到 255，亮暗两端都被截断，标准差/色比全部失真。
SUN = (1.2, 0.0)
# ⚠ 光必须**侧着照**。默认光（write_scene 的 -0.35,-0.5,-0.79）是从相机
#   背后打过来的 —— 可见半球 100% 受光，暗部在球背面根本看不见。
#   实测症状：整颗球只有受光色，唯一发黑的区域是球**自遮挡阴影**
#   （阴影图 PCF 在粗曲面上产生的锯齿边界），于是"暗部统计"量到的是
#   阴影缺陷而不是卡通暗部，三种着色模型都给出 R=0.0。
#   这里让光从 +X 侧来（传播方向 -X），明暗交界线是一条竖着的大圆，
#   亮暗各占可见半球的一半，判据才有东西可测。
LIGHT_DIR = [-0.85, -0.28, -0.22]

K_SHADING_SKIN = 1 << 4
K_SHADING_TOON = 1 << 6


def material_model(name, model, base=(0.78, 0.78, 0.78)):
    """程序化材质 + 显式着色模型。base 用中性灰：让"阴影偏冷"只来自着色器，
    不被材质本身的有色基色污染（用红球会让 B/R 判据失去意义）。"""
    return {
        "kind": "procedural", "name": name,
        "baseColor": list(base) + [1.0],
        "metallic": 0.0, "roughness": 0.55,
        "normalScale": 1.0, "ao": 1.0,
        "emissive": [0.0, 0.0, 0.0],
        "doubleSided": False, "alphaMode": "opaque",
        "shadingModel": model,
    }


def material_npr(name, ramp=None, char_light=False, model="toon"):
    """带 NPR 扩展字段的材质（A6/A5，2026-10-26）。

    ⚠ 这些字段是**场景 JSON 里直接写**的（`resolveMaterial` 读它们），
      不需要先经过编辑器 —— 这也顺便证明了"手写场景能驱动新特性"。
    """
    m = material_model(name, model)
    if ramp:
        # rampSteps=0 = 按资产自带档位位置采样（.vramp 的默认工作流）
        m["rampPath"] = ramp
        m["rampSteps"] = 0.0
    if char_light:
        m["useCharacterLight"] = True
    return m


def build_scene(path, model):
    """一颗球 + 没有地面实体：像素统计不被地面/网格干扰。"""
    ents = [entity("Ball", BALL_POS,
                   {"shape": "sphere", "size": BALL_SIZE, "name": "toon_ball"},
                   material_model("toon_ball_mat_" + model, model))]
    # 太阳强度 1.2、环境光 0：让明暗交界清晰、暗部不被 IBL 拉灰；
    # 强度刻意选在"不削顶"的区间（见 SUN 的注释）；光侧着照（见 LIGHT_DIR）。
    return write_scene(path, ents, "verify_toon: " + model, sun=SUN,
                       light_dir=LIGHT_DIR)


def to_np_rgb(img):
    return np.asarray(img.convert("RGB"), dtype=np.float32)


def luma(a):
    return a[..., 0] * 0.2126 + a[..., 1] * 0.7152 + a[..., 2] * 0.0722


def center_patch(a, vp, frac=0.42):
    """取视口中心一块（球占据的区域）。frac 是边长占视口的比例。

    ⚠ VP-RECT 是**浮点**的 (x0,y0,x1,y1)，不是 (x,y,w,h)，而且不能直接拿去
    切 numpy 数组（切片索引必须是 int）—— 这里统一按 verify_local_shadow
    的 vp_box 口径转换。
    ⚠ 还要**夹到图片范围内**：矩形偶尔会拿到旧尺寸（比截图还大），
    越界切片会被 numpy 静默截短，采样区就悄悄偏了。
    """
    x0, y0, x1, y1 = [int(round(float(v))) for v in vp]
    x0, y0 = max(0, x0), max(0, y0)
    x1, y1 = min(a.shape[1], x1), min(a.shape[0], y1)
    w, h = x1 - x0, y1 - y0
    cw, ch = int(w * frac), int(h * frac)
    cx = x0 + w // 2 - cw // 2
    cy = y0 + h // 2 - ch // 2
    return a[cy:cy + ch, cx:cx + cw]


def shadow_stats(rgb, lum):
    """暗部像素的平均 R 与 B/R。

    返回 (B/R, 平均R, 像素数)。平均 R 单独返回是因为**"死黑"本身就是信号**：
    PBR 的背光面 max(N·L,0)=0 → 只剩环境光，实测 R 接近 0；
    Toon 的半兰伯特让背光面落在阴影色上 → R 明显 >0 且偏蓝。
    """
    lo, hi = np.percentile(lum, 10), np.percentile(lum, 35)
    mask = (lum >= lo) & (lum <= hi)
    if mask.sum() < 50:
        return None, None, 0
    sel = rgb[mask]
    r = float(sel[:, 0].mean())
    b = float(sel[:, 2].mean())
    ratio = (b / r) if r > 1e-3 else None
    return ratio, r, int(mask.sum())


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

    runs = {}
    for model in ("default", "skin", "toon"):
        scene = os.path.join(OUT_DIR, "toon_%s.json" % model)
        build_scene(scene, model)
        shot = os.path.join(OUT_DIR, "toon_%s.png" % model)
        lines, vp, img, errs = run_once(
            exe, scene,
            # MYVK_GRID=0：地平面栅格是有抗锯齿的细线，任何一度视角变化都会
            # 牵动大片像素，而且它和球一起参与亮度分位数 —— 关掉它，采样区里
            # 就只剩球本身（球的投影也会落在栅格上，见第 3 组的注释）。
            {"MYVK_SHADER_VARIANTS": "1", "MYVK_GRID": "0"},
            shot)
        print("  %-8s vp=%s errs=%d" % (model, vp, len(errs)))
        check("%s 零 Vulkan 验证层报错" % model, not errs, errs[:2])
        if vp is None or img is None:
            print("  %s 没拿到视口/截图" % model)
            return 3
        runs[model] = {"lines": lines, "vp": vp, "rgb": to_np_rgb(img)}
        runs[model]["lum"] = luma(runs[model]["rgb"])

    # ---------------- 第 1 组：变体路由 ----------------
    print("\n== 第 1 组：shadingModel → 变体掩码 ==")
    for model, want_bit, want_name in (("default", 0, "none"),
                                       ("skin", K_SHADING_SKIN, "shading-skin"),
                                       ("toon", K_SHADING_TOON, "shading-toon")):
        built = [ln for ln in runs[model]["lines"]
                 if "SHADER-VARIANTS-BUILT fwd" in ln]
        last = built[-1] if built else ""
        masks = set(int(m) for m in __import__("re").findall(r"k(\d+)\[", last))
        print("     %-8s BUILT masks=%s" % (model, sorted(masks)))
        if want_bit == 0:
            check("default 走 k0（不置任何着色模型位）", 0 in masks, sorted(masks))
        else:
            check("%s 的掩码含 bit%s（k%d）"
                  % (model, want_bit.bit_length() - 1, want_bit),
                  any(m & want_bit for m in masks), sorted(masks))
            check("%s 的 BUILT 行里有可读名 %s" % (model, want_name),
                  want_name in last, last[-80:])
        # 互斥：不该出现同时置 skin+toon 的掩码
        check("%s 没有同时置 skin 与 toon 的非法掩码" % model,
              not any((m & K_SHADING_SKIN) and (m & K_SHADING_TOON)
                      for m in masks), sorted(masks))

    # ---------------- 第 2 组：两档量化 ----------------
    # ⚠ 判据的演进（别照抄网上那套"量化 → 总变差更低"）：
    #   第一版用"总变差（相邻像素亮度差之和）更低"，实测**反了**
    #   （toon 7.19 > default 6.19）。原因是 Toon 还有**硬高光**
    #   （pow48 + smoothstep），那个极窄亮点本身制造了巨大的局部梯度，
    #   盖过了"大片平坦区省下的"那部分。总变差同时被"量化的台阶"和
    #   "高光的尖峰"拉扯，方向不可预测，不适合作判据。
    #
    #   换成**亮度标准差**（分布的两极化程度）：两档量化把连续中间调
    #   推向两端 —— 暗的压到阴影色、亮的留在 albedo，中间几乎没有过渡，
    #   于是直方图比 PBR 的连续渐变**更宽**。实测 94.6 vs 77.4（1.22 倍），
    #   方向稳定、幅度足够。
    print("\n== 第 2 组：两档量化（亮度分布两极化）==")
    vp = runs["default"]["vp"]
    p_def = center_patch(runs["default"]["rgb"], vp)
    p_toon = center_patch(runs["toon"]["rgb"], vp)
    p_skin = center_patch(runs["skin"]["rgb"], vp)
    l_def, l_toon, l_skin = luma(p_def), luma(p_toon), luma(p_skin)
    std_def, std_toon, std_skin = (float(l_def.std()), float(l_toon.std()),
                                   float(l_skin.std()))
    print("     亮度标准差  default=%.1f skin=%.1f toon=%.1f"
          % (std_def, std_skin, std_toon))
    # 采样区是**常量色**说明这一块根本没有几何 —— 采样区错位（VP-RECT 拿到
    # 旧尺寸）或球太小。这种时候后面所有判据都在测背景，必须当场失败，
    # 而不是让"Toon 和 Default 一样"这种假象流到结论里。
    for name, sd in (("default", std_def), ("skin", std_skin), ("toon", std_toon)):
        if sd < 5.0:
            print("  [FAIL] %s 的采样区几乎是单色（std=%.2f）—— 采样区没盖住球，"
                  "后面所有像素判据都不可信" % (name, sd))
            print("         vp=%s patch=%s" % (vp, p_def.shape))
            return 4

    diff_px = int((np.abs(p_def - p_toon).max(axis=2) > 12).sum())
    total_px = p_def.shape[0] * p_def.shape[1]
    print("     default vs toon 差异像素 %d / %d (%.1f%%)"
          % (diff_px, total_px, 100.0 * diff_px / total_px))
    check("Toon 与 Default 画面显著不同（分支真的生效）",
          diff_px > total_px * 0.05, "%d px" % diff_px)
    check("Toon 的亮度分布更两极（std 比 Default 高 15% 以上）",
          std_toon > std_def * 1.15,
          "%.1f vs %.1f  (+%.0f%%)"
          % (std_toon, std_def, 100.0 * (std_toon / std_def - 1.0)))
    check("Skin 的亮度分布与 Default 接近（它只改交界线柔和度，不量化）",
          abs(std_skin - std_def) < std_def * 0.10,
          "%.1f vs %.1f" % (std_skin, std_def))

    # ---------------- 第 3 组：阴影是冷的（而且不再是死黑）----------------
    # 这一组是 Toon 最本质的证据，而且是个**质变**而不是量变：
    #   PBR 的背光面 max(N·L,0)=0 → 只剩环境光，实测暗部 R 接近 0；
    #   Toon 的半兰伯特让背光面停在 0.5 附近并乘上 (0.56,0.60,0.74)，
    #   于是暗部**有颜色**、且偏蓝（B/R ≈ 1.32，混入环境光后实测 1.08）。
    print("\n== 第 3 组：阴影有色且偏冷 ==")
    r_def, avg_def, n_def = shadow_stats(p_def, l_def)
    r_toon, avg_toon, n_toon = shadow_stats(p_toon, l_toon)
    r_skin, avg_skin, n_skin = shadow_stats(p_skin, l_skin)
    print("     暗部  default: R=%.1f B/R=%s | skin: R=%.1f B/R=%s | "
          "toon: R=%.1f B/R=%s"
          % (avg_def or 0, "%.3f" % r_def if r_def else "n/a",
             avg_skin or 0, "%.3f" % r_skin if r_skin else "n/a",
             avg_toon or 0, "%.3f" % r_toon if r_toon else "n/a"))
    check("采样到足够的暗部像素", n_def > 500 and n_toon > 500,
          "%d/%d" % (n_def, n_toon))
    check("Toon 的暗部有红色分量（不是死黑）", (avg_toon or 0) > 8.0,
          "R=%.1f" % (avg_toon or 0))
    check("Toon 的暗部明显比 Default 亮（背光面不再被压到 0）",
          (avg_toon or 0) > (avg_def or 0) * 1.5,
          "R=%.1f vs %.1f" % (avg_toon or 0, avg_def or 0))
    if r_toon:
        check("Toon 的暗部偏蓝（B/R > 1.05，阴影乘了冷色调）",
              r_toon > 1.05, "%.3f" % r_toon)
    # 反向断言：Skin 不碰阴影色，它的暗部仍应是 PBR 的死黑。
    # 这条证明上面的判据测的是**着色模型**，不是"任何非 Default 都提亮"。
    check("Skin 的暗部仍是 PBR 口径（比 Toon 暗得多，说明 Skin≠Toon）",
          (avg_skin or 0) < (avg_toon or 0) * 0.5,
          "R=%.1f vs %.1f" % (avg_skin or 0, avg_toon or 0))

    # ---------------- 第 4 组：色带（A6，2026-10-26）----------------
    # 判据的核心是**颜色方向**，不是"有没有变化"：
    #   没有色带 → 暗部由引擎写死成冷蓝（实测 B/R ≈ 1.08，第 3 组已断言）；
    #   挂了"暖色暗部"的色带 → 暗部必须变成红主导（B/R 明显 < 1）。
    # 这个差异**只可能**来自"色带的三通道被当成颜色用了" —— 上一版只读 R
    # 通道做乘法调制，无论画什么颜色的色带都改不出红暗部。
    print("\n== 第 4 组：色带把暗部颜色交给美术（A6）==")
    ramp_rel = "ramps/toon_warm_3step.vramp"
    ramp_scene = os.path.join(OUT_DIR, "toon_ramp.json")
    write_scene(ramp_scene,
                [entity("Ball", BALL_POS,
                        {"shape": "sphere", "size": BALL_SIZE,
                         "name": "toon_ramp_ball"},
                        material_npr("toon_ramp_mat", ramp=ramp_rel))],
                "verify_toon: ramp", sun=SUN, light_dir=LIGHT_DIR)
    ramp_shot = os.path.join(OUT_DIR, "toon_ramp.png")
    # ⚠⚠ **必须开 bindless**：色带/MatCap/SDF/细节这四个 A 类语义槽与
    #   材质页（set1/binding1 的 96 B 记录）是同一套东西，而 bindless 默认
    #   **关**（`rp.bindless`，MYVK_BINDLESS）。关着时着色器里根本没有
    #   `gMat`，四个槽全部失效 —— 表现是"配了色带但画面一点没变"，
    #   而且不报任何错。这是本轮实测踩到的第一个坑。
    NPR_ENV = {"MYVK_SHADER_VARIANTS": "1", "MYVK_GRID": "0",
               "MYVK_BINDLESS": "1"}
    r_lines, r_vp, r_img, r_errs = run_once(exe, ramp_scene, NPR_ENV,
                                            ramp_shot)
    check("ramp 场景零 Vulkan 验证层报错", not r_errs, r_errs[:2])
    if r_vp is None or r_img is None:
        print("  ramp 场景没拿到视口/截图")
        fails.append("ramp 截图")
        print("\n结果: FAIL: " + ", ".join(fails))
        return 5
    # 资产真的被读到了 —— 否则着色器会静默退回程序化两档（画面看着"没变化"，
    # 而不是报错）。这条日志是"路径解析成功"的唯一证据。
    loaded = [ln for ln in r_lines if "RAMP-LOAD key=" in ln]
    check("色带资产被加载（RAMP-LOAD 日志出现）", bool(loaded),
          loaded[-1][-90:] if loaded else "no RAMP-LOAD line")
    check("日志里的 band 数 = 3",
          bool(loaded) and "bands=3" in loaded[-1],
          loaded[-1][-60:] if loaded else "")

    r_rgb = to_np_rgb(r_img)
    p_ramp = center_patch(r_rgb, r_vp)
    l_ramp = luma(p_ramp)
    ratio_ramp, avg_ramp, n_ramp = shadow_stats(p_ramp, l_ramp)
    print("     暗部  toon: B/R=%s R=%.1f | ramp: B/R=%s R=%.1f"
          % ("%.3f" % r_toon if r_toon else "n/a", avg_toon or 0,
             "%.3f" % ratio_ramp if ratio_ramp else "n/a", avg_ramp or 0))
    check("ramp 场景采到足够暗部像素", (n_ramp or 0) > 500, str(n_ramp))
    if ratio_ramp and r_toon:
        check("挂了暖色色带后暗部转红主导（B/R < 0.85）",
              ratio_ramp < 0.85, "%.3f" % ratio_ramp)
        check("比无色带的冷蓝暗部明显更暖（低 30% 以上）",
              ratio_ramp < r_toon * 0.70,
              "%.3f vs %.3f" % (ratio_ramp, r_toon))
    else:
        check("ramp 暗部有红分量（B/R 可算）", False, "ratio=%s" % ratio_ramp)
        check("ramp 与无色带的色比可比", False, "")
    diff_r = int((np.abs(p_toon - p_ramp).max(axis=2) > 12).sum())
    tot_r = p_ramp.shape[0] * p_ramp.shape[1]
    print("     toon vs ramp 差异像素 %d / %d (%.1f%%)"
          % (diff_r, tot_r, 100.0 * diff_r / tot_r))
    check("色带真的改变了画面（>3% 像素）", diff_r > tot_r * 0.03,
          "%d px" % diff_r)

    # ---------------- 第 5 组：角色光通道（A5，2026-10-26）----------------
    # 三条判据一起构成"通道语义正确"的证据链：
    #   ① 材质**没勾** useCharacterLight 时，开不开角色光逐像素相同
    #      （= "关 = 基线"，它证明这道 gate 在材质侧而不是全局）；
    #   ② 勾了之后，开/关角色光画面显著不同（= 通道真的接进了着色）；
    #   ③ 勾了之后的开/关差异**大于** ①的差异一个量级（防"①只是噪声"）。
    print("\n== 第 5 组：角色光通道（A5）==")

    def cl_run(tag, ramp, char_light, env_extra):
        sc = os.path.join(OUT_DIR, "toon_cl_%s.json" % tag)
        write_scene(sc,
                    [entity("Ball", BALL_POS,
                            {"shape": "sphere", "size": BALL_SIZE,
                             "name": "toon_cl_ball_%s" % tag},
                            material_npr("toon_cl_mat_%s" % tag, ramp=ramp,
                                         char_light=char_light))],
                    "verify_toon: charlight " + tag, sun=SUN,
                    light_dir=LIGHT_DIR)
        shot = os.path.join(OUT_DIR, "toon_cl_%s.png" % tag)
        env = dict(NPR_ENV)
        env.update(env_extra)
        lines, vp, img, errs = run_once(exe, sc, env, shot)
        check("%s 零 Vulkan 验证层报错" % tag, not errs, errs[:2])
        if img is None:
            return None, lines
        return center_patch(to_np_rgb(img), vp), lines

    # ① 材质不勾：角色光开着也必须与 toon 基线逐像素一致
    p_noflag, _ = cl_run("noflag", None, False, {})
    if p_noflag is None:
        fails.append("charlight noflag 截图")
    else:
        d = int((np.abs(p_toon - p_noflag).max(axis=2) > 12).sum())
        tot = p_noflag.shape[0] * p_noflag.shape[1]
        print("     材质未勾选时 charlight=on vs 基线：差异 %d px (%.2f%%)"
              % (d, 100.0 * d / tot))
        check("材质没勾 useCharacterLight 时画面与基线一致（<0.3%）",
              d < tot * 0.003, "%d px" % d)

    # ② 勾了 + 角色光开 / 关
    p_on, lines_on = cl_run("on", None, True, {"MYVK_CHARLIGHT": "1"})
    p_off, _ = cl_run("off", None, True, {"MYVK_CHARLIGHT": "0"})
    if p_on is None or p_off is None:
        fails.append("charlight on/off 截图")
    else:
        d2 = int((np.abs(p_on - p_off).max(axis=2) > 12).sum())
        tot2 = p_on.shape[0] * p_on.shape[1]
        print("     勾选后 charlight on vs off：差异 %d px (%.1f%%)"
              % (d2, 100.0 * d2 / tot2))
        check("勾选后角色光开/关显著不同（>3% 像素）", d2 > tot2 * 0.03,
              "%d px" % d2)
        # ③ 比①的噪声大一个量级
        if p_noflag is not None:
            d1 = int((np.abs(p_toon - p_noflag).max(axis=2) > 12).sum())
            check("开/关差异远大于\"材质没勾\"的噪声（>10 倍）",
                  d2 > max(d1 * 10, tot2 * 0.03), "%d vs %d" % (d2, d1))
        # 角色光材质跳过 CSM：球的自遮挡阴影应当消失 →
        # "最暗一小撮像素"占比下降。这是一条独立于方向变化的判据。
        lo_on = float(np.percentile(luma(p_on), 2))
        lo_off = float(np.percentile(luma(p_off), 2))
        print("     2%% 分位亮度  on=%.1f off=%.1f" % (lo_on, lo_off))
        check("角色光材质不吃 CSM（最暗像素比开 CSM 时更亮）",
              lo_on >= lo_off - 1.0, "%.1f vs %.1f" % (lo_on, lo_off))

    # ---------------- 第 6 组：面部 SDF 阴影（A1，2026-10-26）----------------
    # 判据用**三重对照**，而不是"看起来变暗了"：
    #   ① 白图（处处受光）→ 与基线**逐像素相同**：证明通道没有副作用；
    #   ② 黑图 + strength=1 → 画面显著变化且**分布塌成一条线**：
    #      证明掩码真的把每个片元都钉到了同一档；
    #   ③ 黑图 + strength=0 → 又与基线**逐像素相同**：证明是"强度"这个
    #      旋钮在起作用，而不是"挂了图就有副作用"。
    #
    # ⚠ 这里刻意**不**断言"黑图让画面变暗"。第一次写的就是这条，实测失败：
    #   本场景的可见半球全在受光档，而受光档被 CSM 自遮挡压到 156；
    #   Toon 的"暗档"是**冷色中间调**（199），比被压暗的受光面还亮。
    #   "暗档 ≠ 更暗"是赛璐璐的常识（阴影是有颜色的颜料），用亮度当判据
    #   本身就是错的 —— 换成"偏冷（B > R）"+ "分布塌缩"。
    print("\n== 第 6 组：面部 SDF 阴影（A1）==")

    def sdf_run(tag, tex, strength, ramp=None, frac=0.42):
        sc = os.path.join(OUT_DIR, "toon_sdf_%s.json" % tag)
        mat = material_npr("toon_sdf_mat_%s" % tag, ramp=ramp)
        if tex:
            mat["sdfShadowMap"] = tex
        mat["sdfShadowStrength"] = strength
        mat["sdfFaceShift"] = 0.15
        write_scene(sc,
                    [entity("Ball", BALL_POS,
                            {"shape": "sphere", "size": BALL_SIZE,
                             "name": "toon_sdf_ball_%s" % tag}, mat)],
                    "verify_toon: sdf " + tag, sun=SUN, light_dir=LIGHT_DIR)
        shot = os.path.join(OUT_DIR, "toon_sdf_%s.png" % tag)
        lines, vp, img, errs = run_once(exe, sc, NPR_ENV, shot)
        check("%s 零 Vulkan 验证层报错" % tag, not errs, errs[:2])
        if img is None:
            return None
        return center_patch(to_np_rgb(img), vp, frac=frac)

    TEX_W = {"kind": "solid", "name": "sdf_white", "color": [255, 255, 255, 255]}
    TEX_B = {"kind": "solid", "name": "sdf_black", "color": [0, 0, 0, 255]}
    p_w = sdf_run("white", TEX_W, 1.0)
    p_b = sdf_run("black", TEX_B, 1.0)
    p_b0 = sdf_run("black0", TEX_B, 0.0)
    if p_w is None or p_b is None or p_b0 is None:
        fails.append("sdf 截图")
    else:
        tot = p_b.shape[0] * p_b.shape[1]
        d_w = int((np.abs(p_toon - p_w).max(axis=2) > 12).sum())
        d_0 = int((np.abs(p_toon - p_b0).max(axis=2) > 12).sum())
        d_b = int((np.abs(p_w - p_b).max(axis=2) > 12).sum())
        print("     白图 vs 基线 %d px | 黑图strength=0 vs 基线 %d px | "
              "黑 vs 白 %d px (%.1f%%)"
              % (d_w, d_0, d_b, 100.0 * d_b / tot))
        check("① 白 SDF（处处受光）与基线逐像素一致（<0.3%）",
              d_w < tot * 0.003, "%d px" % d_w)
        check("③ 黑 SDF 但 strength=0 与基线一致（<0.3%）—— 强度是唯一的闸门",
              d_0 < tot * 0.003, "%d px" % d_0)
        check("② 黑 SDF（strength=1）画面显著变化（>20% 像素）",
              d_b > tot * 0.20, "%d px" % d_b)

        l_w, l_b = luma(p_w), luma(p_b)
        s_w, s_b = float(l_w.std()), float(l_b.std())
        print("     亮度标准差 白=%.1f 黑=%.1f（塌成一条线 = 全脸同一档）"
              % (s_w, s_b))
        check("② 黑 SDF 让明暗分布塌缩（std ≤ 白图的 10%）",
              s_b <= max(s_w * 0.10, 2.0), "%.1f vs %.1f" % (s_b, s_w))

        # ④ 方向性判据：换成一张**纯红→纯蓝**的两档探针色带
        #    （assets/ramps/toon_probe_rb.vramp：第 0 档纯红，0.5 起纯蓝）。
        #    黑 SDF 把 u 钉成 0 → 必须回**纯红**；白 SDF → 红蓝各半。
        #
        #    ⚠ 上一版判据是"暖色带下 B/R 更低"，实测判不出来：暖色带的
        #      暗档 (0.70,0.26,0.20) 与受光档白之间在 tonemap + 天空背景
        #      稀释之后只差几个百分点，两个数落到 0.76/0.92，看着"反了"
        #      其实只是分辨率不够。探针色带的红/蓝在通道上**互斥**
        #      （G 恒 0），任何混合都只能落在中间，没有灰度歧义。
        #
        #    ⚠⚠ 这条断言同时是"色带采样坐标"的回归 —— 写它的时候抓到
        #      一个真 bug：rampMap 用的是共享 REPEAT 采样器，su=0 的纹理
        #      坐标是 -0.5 纹素，回绕到 255.5，线性过滤把**首末两档各取
        #      一半**，于是"最暗档"变成了整条色带的平均色 (0.5,0,0.5)。
        #      缺省色带首末两档都接近白，逐像素完全看不出来。
        probe_rel = "ramps/toon_probe_rb.vramp"
        p_rw = sdf_run("rampwhite", TEX_W, 1.0, ramp=probe_rel, frac=0.15)
        p_rb = sdf_run("rampblack", TEX_B, 1.0, ramp=probe_rel, frac=0.15)
        if p_rw is None or p_rb is None:
            fails.append("sdf+ramp 截图")
        else:
            def br(a):
                m = a.reshape(-1, 3).mean(axis=0)
                return float(m[2] / max(m[0], 1.0)), m

            br_w, m_w2 = br(p_rw)
            br_b, m_b2 = br(p_rb)
            print("     探针色带下 白 SDF B/R=%.3f (%.0f,%.0f,%.0f) | "
                  "黑 SDF B/R=%.3f (%.0f,%.0f,%.0f)"
                  % (br_w, m_w2[0], m_w2[1], m_w2[2],
                     br_b, m_b2[0], m_b2[1], m_b2[2]))
            check("④ 黑 SDF 把整片钉到第 0 档 = 纯红（B/R < 0.2）",
                  br_b < 0.20, "%.3f" % br_b)
            check("④ 白 SDF 保持红蓝两档混合（B/R ≥ 黑 SDF 的 3 倍）",
                  br_w >= max(br_b * 3.0, 0.5), "%.3f vs %.3f" % (br_w, br_b))
            # 两张图都必须几乎**没有绿**：G 只可能来自色带 tint 之外
            # （albedo 被 tint 乘掉了、没有局部光、没有自发光）。这一条
            # 是"颜色真的由色带决定"的旁证，比看主色更直接。
            check("④ 探针色带把 G 通道压到 ≈0（颜色由色带决定）",
                  m_w2[1] <= 12 and m_b2[1] <= 12,
                  "G=%.0f/%.0f" % (m_w2[1], m_b2[1]))

    print("\n结果: %s" % ("PASS" if not fails else "FAIL: " + ", ".join(fails)))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
