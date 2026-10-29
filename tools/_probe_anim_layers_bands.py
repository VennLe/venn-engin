#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""一次性探针：给 `verify_anim_layers` 重新量机位与分带。

换资产（旧 `anime_girl.fbx` → FAB 的 `animate_girl_rigged.glb`）之后，
`UPPER_END` / `LOWER_START` 这两个"只有头/肩"、"只有膝下/脚"的横线就作废了
—— 它们是对着**旧机位 + 旧身材**量的（见那个文件里的注释）。机位一换，
两条线落在身体哪个部位全变，判据会退化成"两边都算它一半"。

这个探针做三件事，输出直接抄回 `verify_anim_layers.py`：

  ① 角色在视口里占多少 —— 用"与背景色不同的像素"求包围盒，换算成
     相对视口高度的 0..1，看头到哪、脚到哪、左右留白够不够；
  ② 打一层 Allow=upleg（腿换相位）→ 逐行变化占比；
  ③ 打一层 Allow=spine2（上半身换 Idle）→ 逐行变化占比。
     ②③ 的剖面里"只有一类骨"的连续区间就是新的 `UPPER_END` / `LOWER_START`。

用法：
    python tools/_probe_anim_layers_bands.py "3,-3,1.5,45,8.7"
    python tools/_probe_anim_layers_bands.py            # 用文件里的 CAM
"""

import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import verify_anim_layers as V                                       # noqa: E402
from verify_local_shadow import crop_pair                            # noqa: E402

# 背景判定：天空色是纯色渐变，用"每通道偏离中位色 > 6"的数量判"不是背景"。
BG_THR = 6
# 逐行剖面的桶数
BUCKETS = 50


def _vp_of(img, vp):
    """把整窗截图裁到视口，返回 RGB 数组。"""
    pa, _, x0, y0 = crop_pair(img, img, vp)
    if pa is None:
        raise SystemExit("[FAIL] 裁不出视口")
    return pa, x0, y0


def char_bbox(px):
    """角色像素的包围盒（相对视口高度/宽度的 0..1）。"""
    h, w, _ = px.shape
    # 背景色取四角的中位色（角色永远在中间）
    corners = np.concatenate([
        px[0:6, 0:6].reshape(-1, 3), px[0:6, w - 6:].reshape(-1, 3),
        px[h - 6:, 0:6].reshape(-1, 3), px[h - 6:, w - 6:].reshape(-1, 3)])
    bg = np.median(corners, axis=0)
    fg = (np.abs(px.astype(np.float32) - bg).max(axis=2) > BG_THR)
    rows = np.where(fg.any(axis=1))[0]
    cols = np.where(fg.any(axis=0))[0]
    if len(rows) == 0:
        return None
    # 只取中间那一坨：把地面/别的 UI 排除掉（按行连通性取最长的连续段）
    runs = np.split(rows, np.where(np.diff(rows) > 3)[0] + 1)
    big = max(runs, key=len)
    y0, y1 = int(big[0]), int(big[-1])
    sub = fg[y0:y1 + 1]
    cols = np.where(sub.any(axis=0))[0]
    x0, x1 = int(cols[0]), int(cols[-1])
    return (y0 / h, (y1 + 1) / h, x0 / w, (x1 + 1) / w, fg.mean())


def profile(mask, buckets=BUCKETS):
    h = mask.shape[0]
    out = []
    for i in range(buckets):
        a = int(h * i / buckets)
        b = int(h * (i + 1) / buckets)
        out.append(float(mask[a:b].mean()) * 100.0)
    return out


def bar(vals):
    mx = max(vals) if vals else 0.0
    for i, v in enumerate(vals):
        n = int(round(v / mx * 40)) if mx > 0 else 0
        print("   %5.2f-%.2f  %6.2f%%  %s"
              % (i / len(vals), (i + 1) / len(vals), v, "#" * n))


def main():
    cam = sys.argv[1] if len(sys.argv) > 1 else V.CAM
    exe = V.DEFAULT_EXE
    print("[info] 机位 = %s" % cam)
    print("[info] 模板 = %s" % V.TPL)
    V.ensure_template(exe)

    def shoot(tag, layers=None, clip=V.BASE_CLIP, t=V.BASE_TIME):
        scene = V.build_variant(tag, layers, clip, t)
        shot = os.path.join(V.OUT_DIR, "probe_%s.png" % tag)
        lines, vp, img, errs = V.run_shot(exe, scene, dict(V.ENV), shot,
                                         cam=cam, wait=40.0)
        if img is None:
            raise SystemExit("[FAIL] %s 没截图 errs=%s" % (tag, errs[:3]))
        return img, vp

    base, vp = shoot("al_base")
    px, x0, y0 = _vp_of(base, vp)
    print("[info] 视口 %dx%d @ (%d,%d)  裁后 %dx%d"
          % (vp[2] if len(vp) > 2 else -1, vp[3] if len(vp) > 3 else -1,
             x0, y0, px.shape[1], px.shape[0]))
    from PIL import Image
    Image.fromarray(px.astype(np.uint8)).save(
        os.path.join(V.OUT_DIR, "probe_al_vp.png"))
    bb = char_bbox(px)
    if bb is None:
        raise SystemExit("[FAIL] 视口里找不到角色")
    print("[bbox] top=%.3f bot=%.3f left=%.3f right=%.3f  fg=%.2f%%"
          % (bb[0], bb[1], bb[2], bb[3], bb[4] * 100.0))
    print("       身高占视口 %.1f%%（%.0f px）"
          % ((bb[1] - bb[0]) * 100.0, (bb[1] - bb[0]) * px.shape[0]))

    legs, _ = shoot("al_legs", [V.make_layer(V.LAYER_CLIP, 1.0, 1, ["upleg"])])
    m_legs = V.diff_mask(base, legs, vp)
    arms, _ = shoot("al_arms", [V.make_layer("Idle", 1.0, 1, ["spine2"])])
    m_arms = V.diff_mask(base, arms, vp)

    # 身体在视口里的真实上下沿：并集的两端。判"有没有被切掉"看这个。
    both = m_legs | m_arms
    rr = np.where(both.any(axis=1))[0]
    cc = np.where(both.any(axis=0))[0]
    if len(rr):
        print("[body] 变化像素覆盖 y=%.3f..%.3f  x=%.3f..%.3f（相对视口）"
              % (rr[0] / both.shape[0], (rr[-1] + 1) / both.shape[0],
                 cc[0] / both.shape[1], (cc[-1] + 1) / both.shape[1]))
    print("\n[剖面] Allow=upleg（腿）逐行变化占比，行号是相对视口高度：")
    bar(profile(m_legs))
    print("\n[剖面] Allow=spine2（上半身）逐行变化占比：")
    bar(profile(m_arms))

    # 直接给出"互不污染"的分带建议：找最长的连续行区间，使 legs 在那里动、
    # arms 在那里不动（下半），以及反过来（上半）。
    h = m_legs.shape[0]
    rl = m_legs.mean(axis=1)
    ra = m_arms.mean(axis=1)

    def band(pred):
        best, cur, start = (0, 0), None, 0
        for i, ok in enumerate(pred):
            if ok and cur is None:
                cur = i
            elif not ok and cur is not None:
                if i - cur > best[1] - best[0]:
                    best = (cur, i)
                cur = None
        if cur is not None and h - cur > best[1] - best[0]:
            best = (cur, h)
        return best[0] / h, best[1] / h

    lo = band([v > 0.01 for v in rl])          # 腿在动的区间
    up = band([v > 0.01 for v in ra])          # 上半身在动的区间
    print("\n[建议] 腿动区间   = %.3f .. %.3f" % lo)
    print("[建议] 上半动区间 = %.3f .. %.3f" % up)
    print("[建议] 于是 UPPER_END ≈ %.2f（上半动区间的下界）" % up[1])
    print("[建议]      LOWER_START ≈ %.2f（腿动区间的上界）" % lo[0])


if __name__ == "__main__":
    main()
