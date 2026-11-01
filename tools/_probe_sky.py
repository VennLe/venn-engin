"""探针：把四套天空预设各截一张图，用来目视 + 量分带的颜色。

不是验收用例（没有 PASS/FAIL），只是开发期的眼睛。
     python tools/_probe_sky.py
产物：`../_venn_verify/probe_sky_<预设>.png` + 每张的顶部/中部/底部均色。

为什么单独写一个而不是塞进 verify_ibl：verify_ibl 要开 7 次编辑器、
构造成可判定的因果实验，慢且脆。调云的形状时想要的是"改一行、看一张图"。
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_stable, find_window, user32      # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors, wait_rect   # noqa: E402
from verify_ibl import DEFAULT_EXE, OUT_DIR, run_once               # noqa: E402

# 机位：站在原点东南方 5 m、抬到 1.6 m、**仰视** 12°。
# `MYVK_EDITOR_CAM` 的 pitch 是**正 = 俯视**（见 verify_anim_layers 用的
# "3,-3,1.5,45,19.5 略俯视"），所以抬头要写负数 —— 第一版写了 +26，
# 结果整个视口全是栅格地面，天空只剩顶上一条缝。
CAM = "3.5,-3.5,1.6,45,-12"

# 空场景（不设 MYVK_EDITOR_SCENE 就是"白纸"：地面 + 方向光 + 默认机位）。
# 但 run_once 一定要 scene 参数，于是写一个最小的空场景文件。
SCENE = os.path.join(OUT_DIR, "probe_sky_empty.json")


def ensure_scene():
    import json
    if os.path.isfile(SCENE):
        return
    os.makedirs(OUT_DIR, exist_ok=True)
    doc = {
        "version": 1,
        "generator": "probe_sky: 一张白纸，只为了让天空占满视口",
        "camera": {"target": [0.0, 0.0, 0.0], "yaw": 0.6, "pitch": 0.3,
                   "distance": 8.0},
        "entities": [],
    }
    with open(SCENE, "w", encoding="utf-8") as f:
        json.dump(doc, f, ensure_ascii=False, indent=2)


def region_mean(img, box):
    x0, y0, x1, y1 = [int(v) for v in box]
    w, h = img.size
    x0 = max(0, min(x0, w - 1)); x1 = max(x0 + 1, min(x1, w))
    y0 = max(0, min(y0, h - 1)); y1 = max(y0 + 1, min(y1, h))
    crop = img.crop((x0, y0, x1, y1)).convert("RGB")
    px = list(crop.getdata())
    n = max(len(px), 1)
    return (sum(p[0] for p in px) / n, sum(p[1] for p in px) / n,
            sum(p[2] for p in px) / n)


def main():
    ensure_scene()
    kill_stale_editors()
    os.makedirs(OUT_DIR, exist_ok=True)

    cases = [
        ("default", {}),
        ("day", {"MYVK_SKY_PRESET": "day"}),
        ("overcast", {"MYVK_SKY_PRESET": "overcast"}),
        ("night", {"MYVK_SKY_PRESET": "night"}),
        ("dusk", {"MYVK_SKY_PRESET": "dusk"}),
        # 面板截图：确认 Sky & Atmosphere 真的画出来了、控件齐全
        ("panel", {"MYVK_SKY_PRESET": "day",
                   "MYVK_EDITOR_PANELS": "environment"}),
    ]

    for name, extra in cases:
        # run_once 里 `env["MYVK_EDITOR_CAM"] = CAM`（verify_ibl 自己的机位）
        # 在 extra_env 合并**之前**，所以这里塞 CAM 能盖掉它。
        extra = dict(extra)
        extra["MYVK_EDITOR_CAM"] = CAM
        shot = os.path.join(OUT_DIR, "probe_sky_%s.png" % name)
        lines, vp, img, errs = run_once(DEFAULT_EXE, SCENE, extra, shot,
                                        wait=4.0)
        if img is None:
            print("[%s] 抓图失败" % name)
            continue
        w, h = img.size
        # 三条横带：上（天顶方向）/ 中（45° 左右）/ 下（地平线附近）
        top = region_mean(img, (w * 0.30, h * 0.06, w * 0.70, h * 0.16))
        mid = region_mean(img, (w * 0.30, h * 0.34, w * 0.70, h * 0.44))
        low = region_mean(img, (w * 0.30, h * 0.60, w * 0.70, h * 0.70))
        print("[%-8s] 上=(%3.0f,%3.0f,%3.0f) 中=(%3.0f,%3.0f,%3.0f) "
              "下=(%3.0f,%3.0f,%3.0f)  错误=%d"
              % ((name,) + top + mid + low + (len(errs),)))
        for e in errs[:3]:
            print("     !", e[:140])
        time.sleep(0.4)

    print("\n产物：%s/probe_sky_*.png" % OUT_DIR)


if __name__ == "__main__":
    main()
