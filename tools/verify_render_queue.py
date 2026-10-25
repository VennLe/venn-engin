"""端到端验证：材质渲染顺序（renderQueue / sortOrder / 覆盖带）。

背景：透明物过去只按 `viewDepthSq` 单键降序排。这对玻璃/烟雾是对的，
对**二次元角色**完全不成立 —— 脸皮、眼白、虹膜、睫毛、眉毛几乎在同一个
球面上，距离差不到 1 mm，谁前谁后由浮点噪声决定；更根本的是"眉毛要穿透
刘海"这条硬规矩，物理距离给出的答案永远相反。

2026-10-25（P0-2/B）把排序键改成

    (renderQueue, sortOrder, -viewDepthSq)

前两个是**语义**，最后一个是**物理**。跨带时距离完全不参与。
renderQueue >= 3000 的材质额外进"覆盖带"：不写深度 + 深度测试恒通过。

本脚本用**两颗球**构造一个物理上不可能、但语义上一目了然的局面：

    近球（红）  在 z=+5，直径 8
    远球（绿）  在 z=-6，直径 8      ← 完全落在近球的投影轮廓内

于是"中心那块是什么颜色"就直接回答了"谁画在上面"：

    第 1 组  远球 renderQueue = 2000（普通）→ 中心必须**全红**（被近球挡住）
    第 2 组  远球 renderQueue = 3100（覆盖）→ 中心必须**出现绿色**（穿透近球）
    第 3 组  远球 3100 / 近球 3000        → 绿色仍然在上（顺序由数值定，
                                            与距离无关：远球画得更晚）
    第 4 组  两颗球都用默认 2000          → 回到"近球在上"（旧行为不变）

第 1 组和第 2 组的唯一差别是 renderQueue，其它字节完全相同 —— 这是
"renderQueue 真的在控制绘制顺序"最干净的证据。

⚠ 判据用"绿色像素占比"而不是平均色比：背光面会把两个通道一起压到接近 0，
  比值反而噪声极大。先按 G - R > 24 二值化再数比例，暗部不会污染结果。
⚠ 物体用 emissive 显色（太阳压到 0.25）：否则近球的背光面也是黑的，
  "被挡住"和"没受光"在像素上无法区分。
⚠ 采样区必须同时落在**两个球内**：远球投影约 235 px、近球约 300 px、
  视口 448x531，所以取 0.25 倍视口的中心块（约 112x133）。

用法：
    python tools/verify_render_queue.py [--exe path]
"""

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verify_shader_variants import (                                    # noqa: E402
    ROOT, OUT_DIR, entity, run_once, write_scene)

DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")

# 机位：俯仰 0、视线水平朝 +Y，相机在 (0,-34,8)。
#   两颗球都放在**视线正中心**上：近球 t=34、远球 t=48。
#   于是远球的投影（约 215 px）完全落在近球轮廓（约 304 px）之内 ——
#   物理上远球一定被挡住，除非排序把它画到后面去。
#
# ⚠ 第一版把远球放在 (0,0,-6)（沿着 +Z 往后挪），是错的：这条视线是
#   下俯 16° 的斜线，沿 Z 后退反而让球**偏离视线**（实测偏 10.7 个世界
#   单位，球径只有 8），结果只露出边缘一条，绿色占比 0.111 —— 既不像
#   PASS 也不像 FAIL，看起来像"覆盖带只生效了一半"。判据没错，几何错了。
#   现在的坐标是沿视线方向取的，不依赖任何屏幕坐标推算。
CAM = "0,-34,8,90,0"
NEAR_POS = (0.0, 0.0, 8.0)
FAR_POS = (0.0, 14.0, 8.0)
BALL_SIZE = 8.0
# 太阳压暗：颜色主要由 emissive 给出，光照只负责让它有一点体积感。
# 太亮会让受光面削顶，绿球被压成白球（G-R 差值消失）。
SUN = (0.25, 0.10)

K_OVERLAY = 1 << 7


def ball_mat(name, rgb, queue, order=0):
    """纯色 + emissive 的球材质。queue/order 就是被测的那两个字段。"""
    return {
        "kind": "procedural", "name": name,
        "baseColor": [rgb[0], rgb[1], rgb[2], 1.0],
        "metallic": 0.0, "roughness": 0.85,
        "normalScale": 1.0, "ao": 1.0,
        # emissive 走加法（pbr.frag: emissive = factor + map），
        # 所以这里给的是"最低亮度"，光照再往上加一点。
        "emissive": [rgb[0] * 0.55, rgb[1] * 0.55, rgb[2] * 0.55],
        "doubleSided": False, "alphaMode": "opaque",
        "shadingModel": "default",
        "renderQueue": queue, "sortOrder": order,
    }


def build_scene(path, near_q, far_q, near_o=0, far_o=0):
    ents = [
        entity("NearRed", NEAR_POS,
               {"shape": "sphere", "size": BALL_SIZE, "name": "rq_near"},
               ball_mat("rq_near_mat", (0.90, 0.10, 0.10), near_q, near_o)),
        entity("FarGreen", FAR_POS,
               {"shape": "sphere", "size": BALL_SIZE, "name": "rq_far"},
               ball_mat("rq_far_mat", (0.10, 0.90, 0.10), far_q, far_o)),
    ]
    return write_scene(path, ents, "verify_render_queue: near=%d far=%d"
                       % (near_q, far_q), sun=SUN)


def ball_bbox(a):
    """红球的像素包围盒（x0,y0,x1,y1）。

    只在第 1 组（plain）调用：那时远球被完全挡住，画面里唯一的红就是近球。
    这是本脚本里**唯一**的坐标来源 —— 采样区由实测轮廓推导，不猜屏幕坐标。
    """
    r = a[:, :, 0].astype(np.int32)
    g = a[:, :, 1].astype(np.int32)
    m = (r > 120) & (g < 110)
    ys, xs = np.nonzero(m)
    if len(xs) == 0:
        return None
    return (int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max()))


def center_patch(a, box, frac=0.40):
    """以**球心**为中心的采样块（不是视口中心）。

    ⚠ 为什么不能用视口中心：实测近球心比视口中心偏上 52 px（球心 y=360、
      视口中心 y=412、视口 470,147-918,678）。用视口中心切 0.25 倍的块，
      下半截会越出球体压到红环上 —— 第一版就是这么得到 0.614 的（既不像
      PASS 也不像 FAIL，看着像"覆盖带只生效一半"，其实是采样越界）。
      球心从第 1 组的红球轮廓量出来，远球与它**同心**（在一条视线上），
      所以这块一定同时落在两个球内。
    ⚠ frac=0.40：近球直径约 304 px → 块 122 px，半对角 86 px；
      远球直径约 215 px（半径 107 px）→ 86 < 107，仍在远球内。
    """
    x0, y0, x1, y1 = box
    cx, cy = (x0 + x1) // 2, (y0 + y1) // 2
    side = int(min(x1 - x0, y1 - y0) * frac)
    h, w = a.shape[0], a.shape[1]
    a0, b0 = max(0, cx - side // 2), max(0, cy - side // 2)
    a1, b1 = min(w, a0 + side), min(h, b0 + side)
    return a[b0:b1, a0:a1]


def green_ratio(patch):
    """绿色像素占比：G - max(R,B) > 24 的像素 / 总像素。

    ⚠ 用 G - max(R,B) 而不是 G - R：天空是蓝灰的（R≈50 G≈60 B≈80），
      如果采样区边缘漏进一点背景，`G - R` 会算出 +10 的"假绿"。
      减 max(R,B) 之后天空给的是负值，绿球给的是 +180，两者分得很开。
    ⚠ 阈值 24 而不是 0：8bit 图上两盏不同颜色的 emissive 之间会有
      几个 LSB 的串扰（TAA 邻域、bloom 的极弱尾部），留一点余量。
    """
    if patch.size == 0:
        return 0.0
    r = patch[:, :, 0].astype(np.int32)
    g = patch[:, :, 1].astype(np.int32)
    b = patch[:, :, 2].astype(np.int32)
    return float((g - np.maximum(r, b) > 24).mean())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    args = ap.parse_args()
    exe = args.exe
    if not os.path.isfile(exe):
        print("找不到编辑器：%s" % exe)
        return 2
    os.makedirs(OUT_DIR, exist_ok=True)

    fails = []
    results = []
    # 近球轮廓：第 1 组量出来之后，后面所有组都用它定位采样区。
    box = [None]

    def check(name, ok, detail):
        results.append((name, ok, detail))
        print("  [%s] %s  %s" % ("PASS" if ok else "FAIL", name, detail))
        if not ok:
            fails.append(name)

    def one(tag, near_q, far_q, near_o=0, far_o=0):
        scene = os.path.join(OUT_DIR, "scene_rq_%s.json" % tag)
        shot = os.path.join(OUT_DIR, "rq_%s.png" % tag)
        build_scene(scene, near_q, far_q, near_o, far_o)
        lines, vp, img, errs = run_once(exe, scene, {}, shot)
        if errs or img is None or vp is None:
            print("  [FAIL] %s 运行失败：%s" % (tag, errs))
            fails.append("run_" + tag)
            return None, lines
        a = np.asarray(img.convert("RGB"))
        if box[0] is None:
            box[0] = ball_bbox(a)
            if box[0] is None:
                print("  [FAIL] 量不到近球轮廓（红球不可见）")
                fails.append("no_bbox")
                return None, lines
            bx = box[0]
            print("       近球轮廓 x %d..%d  y %d..%d（中心 %d,%d，直径 %d）"
                  % (bx[0], bx[2], bx[1], bx[3],
                     (bx[0] + bx[2]) // 2, (bx[1] + bx[3]) // 2,
                     bx[2] - bx[0]))
        patch = center_patch(a, box[0])
        gr = green_ratio(patch)
        mean = patch.reshape(-1, 3).mean(axis=0)
        print("       %-10s 绿色占比 %.3f  中心均值 R%.0f G%.0f B%.0f"
              % (tag, gr, mean[0], mean[1], mean[2]))
        return gr, lines

    print("== 第 1 组：不设 renderQueue 时，远球被近球挡住（物理正确）==")
    gr_a, lines_a = one("plain", 2000, 2000)
    if gr_a is not None:
        check("默认顺序 = 近球在上（远球不可见）", gr_a < 0.10,
              "绿色占比 %.3f" % gr_a)

    print("== 第 2 组：远球 renderQueue=3100（覆盖带）→ 应穿透近球 ==")
    gr_b, lines_b = one("overlay", 2000, 3100)
    if gr_b is not None:
        check("覆盖带的远球画在近球之上", gr_b > 0.55,
              "绿色占比 %.3f" % gr_b)
        check("覆盖带真的改了绘制顺序（第 1 组 vs 第 2 组）",
              gr_a is not None and gr_b > gr_a + 0.4,
              "%.3f → %.3f" % (gr_a or 0.0, gr_b))
        # 变体掩码里必须出现 bit7：覆盖带是关键字位（虽然它只改管线状态）
        has_overlay_bit = any(("k%d" % (128 + i)) in ln or "overlay" in ln
                              for ln in (lines_b or []) for i in range(128))
        check("日志里出现了 overlay 关键字位（bit7）", has_overlay_bit,
              "掩码 128 位可见")

    print("== 第 3 组：顺序由数值定，与距离无关（远球 3100 > 近球 3000）==")
    gr_c, _ = one("semantic", 3000, 3100)
    if gr_c is not None:
        check("远球仍在上（说明排序真的不看距离）", gr_c > 0.55,
              "绿色占比 %.3f" % gr_c)

    print("== 第 4 组：sortOrder 同带内生效（两颗球同 queue=3100）==")
    gr_d, _ = one("order", 3100, 3100, near_o=0, far_o=5)
    if gr_d is not None:
        check("sortOrder 大的画在上面（远球 order=5）", gr_d > 0.55,
              "绿色占比 %.3f" % gr_d)
    gr_e, _ = one("order_rev", 3100, 3100, near_o=5, far_o=0)
    if gr_e is not None:
        check("反过来 sortOrder 后，近球回到上面", gr_e < 0.10,
              "绿色占比 %.3f" % gr_e)

    print()
    bad = [n for n, ok, _ in results if not ok]
    if bad:
        print("结果: FAIL: " + "; ".join(bad))
        return 1
    print("结果: PASS（%d 条判据）" % len(results))
    return 0


if __name__ == "__main__":
    sys.exit(main())
