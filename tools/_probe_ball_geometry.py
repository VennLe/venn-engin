"""一次性诊断：球在不同 size 下到底占多少像素、落在画面哪一块。

动机：verify_toon_shading.py 原来"球在画面正中、直径约 size*11.8 px"的推导
是错的 —— VP-RECT 日志里的矩形和截图像素坐标不是同一套（实测日志给
(470,139)-(1259,834)，而截图只有 1258x744，视口的真实像素范围约
x 466..920 / y 176..670），于是 center_patch 采样到的根本不是球。

这个脚本改用**差分**定位球：同一场景、同一机位，把球挪到相机背后渲一次
当背景，两张图相减就是球的轮廓。这样不依赖任何坐标假设。
"""

import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verify_shader_variants import (                                    # noqa: E402
    ROOT, OUT_DIR, entity, run_once, write_scene)

EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
CAM = "0,-34,15,90,16"
BALL_POS = (0.0, 0.0, 5.0)
# 相机在 y=-34 朝 +Y 看，把球挪到 y=-900（相机背后）就等于"没有球"
HIDDEN_POS = (0.0, -900.0, 5.0)


def mat(name, model="toon"):
    return {"kind": "procedural", "name": name,
            "baseColor": [0.78, 0.78, 0.78, 1.0],
            "metallic": 0.0, "roughness": 0.55,
            "normalScale": 1.0, "ao": 1.0,
            "emissive": [0.0, 0.0, 0.0],
            "doubleSided": False, "alphaMode": "opaque",
            "shadingModel": model}


def scene_with(path, size, pos, tag):
    write_scene(path,
                [entity("Ball", pos,
                        {"shape": "sphere", "size": size, "name": "probe_" + tag},
                        mat("probe_mat_" + tag))],
                "probe: " + tag, sun=(2.6, 0.0))


def rgb(img):
    return np.asarray(img.convert("RGB"), dtype=np.float32)


def shoot(scene, shot, tag):
    lines, vp, img, errs = run_once(EXE, scene,
                                    {"MYVK_SHADER_VARIANTS": "1"}, shot)
    print("  %-10s vp=%s errs=%d img=%s"
          % (tag, vp, len(errs), None if img is None else img.size))
    return vp, (None if img is None else rgb(img))


def main():
    os.makedirs(OUT_DIR, exist_ok=True)

    # 背景基准：球挪到相机背后
    s = os.path.join(OUT_DIR, "probe_bg.json")
    scene_with(s, 1.0, HIDDEN_POS, "bg")
    vp, bg = shoot(s, os.path.join(OUT_DIR, "probe_bg.png"), "bg")
    if bg is None:
        print("背景渲染失败的截图没拿到")
        return 2
    print("  背景整幅亮度 min/max = %.1f / %.1f"
          % (bg.min(), bg.max()))

    for size in (4.0, 6.0, 8.0, 10.0, 12.0):
        s = os.path.join(OUT_DIR, "probe_%g.json" % size)
        scene_with(s, size, BALL_POS, "%g" % size)
        _vp, a = shoot(s, os.path.join(OUT_DIR, "probe_%g.png" % size), "%g" % size)
        if a is None or a.shape != bg.shape:
            print("    size=%g 跳过" % size)
            continue
        d = np.abs(a - bg).max(axis=2)
        m = d > 6.0
        n = int(m.sum())
        if n == 0:
            print("    size=%-4g 差分 mask 为空 —— 球没画出来" % size)
            continue
        ys, xs = np.nonzero(m)
        print("    size=%-4g mask=%6d px  bbox x[%d..%d] y[%d..%d] "
              "(w=%d h=%d)  面积占整幅 %.1f%%"
              % (size, n, xs.min(), xs.max(), ys.min(), ys.max(),
                 xs.max() - xs.min() + 1, ys.max() - ys.min() + 1,
                 100.0 * n / m.size))
        sel = a[m]
        print("           球内 RGB 均值=%s  std=%s  亮度 min/max=%.1f/%.1f"
              % (sel.mean(axis=0).round(1), sel.std(axis=0).round(1),
                 sel.max(), sel.min()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
