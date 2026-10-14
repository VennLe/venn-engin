"""端到端验证：纹理能力全家桶（mipmap / 各向异性 / BC 压缩 / 纹理流）。

背景：四项能力的硬约定与屏幕空间效果一致——新特性默认关，
"关 = 基线逐像素不变"。接线语义（见 assets/Texture.h/.cpp）：

  · mipmap   MYVK_MIPMAP=1：RGBA8 路径 blit 链生成 mip 链 + sampler
             放开 LOD；默认关时 mipLevels=1、maxLod=1（永远采 level0）
  · 各向异性 设备上限已接通（≤8）；MYVK_ANISO=N 覆盖（1=各向同性）
  · BC       .dds 文件走压缩路径（BC1/2/3/4/5/7 直接上传不解码），
             DDS 自带 mip 链逐层上传
  · 纹理流   MYVK_TEXSTREAM=1：DDS 先传 mip0 立即可渲染，其余层由
             AssetManager::tickTextureStreaming 每帧补一层，完成后
             重建 sampler 放开 LOD

断言五组：

  1. **基线确定性**：默认设置跑两次逐像素一致（惯例自检）。
  2. **mipmap**：MYVK_MIPMAP 开/关在远景高频棋盘上差异显著
      （三线性滤波压掉摩尔纹）+ 日志确认 mip 链层数。
  3. **各向异性**：MYVK_ANISO=1（强制各向同性）与默认（设备上限 8）
      在斜视地面上差异显著。
  4. **BC 压缩**：.dds 场景加载日志确认 format=BC1 levels=10，
      渲染零 VUID。
  5. **纹理流**：TEXSTREAM=1 的日志出现 begin → 逐层 → complete，
      且**最终帧与一次性上传（TEXSTREAM=0）逐像素一致**——
      "流"的正确性黄金判据。

每轮零 VUID / 零 Fatal。用法：
    python tools/verify_textures.py
"""

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verify_add_menu import kill_stale_editors                       # noqa: E402
from verify_local_shadow import diff_stats, vp_box                  # noqa: E402
from verify_shader_variants import run_once                         # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "..", "_venn_verify")


def mat_with_tex(name, tex_path):
    """材质：albedo 绑 file 纹理（.dds → 压缩路径，.png → stb 路径）"""
    return {"kind": "standard", "name": name,
            "baseColor": [1.0, 1.0, 1.0, 1.0],
            "metallic": 0.0, "roughness": 0.85, "normalScale": 0.0,
            "ao": 1.0, "emissive": [0, 0, 0],
            "doubleSided": False, "alphaBlend": False,
            "albedo": {"kind": "file", "path": tex_path,
                       "name": name.lower(), "srgb": True}}


def mat_checker(name, cell):
    return {"kind": "standard", "name": name,
            "baseColor": [1.0, 1.0, 1.0, 1.0],
            "metallic": 0.0, "roughness": 0.9, "normalScale": 0.0,
            "ao": 1.0, "emissive": [0, 0, 0],
            "doubleSided": False, "alphaBlend": False,
            "albedo": {"kind": "checker", "name": name.lower() + "_tex",
                       "colorA": [30, 30, 30, 255],
                       "colorB": [230, 230, 230, 255],
                       "cell": cell, "size": 1024}}


def build_scene(path):
    """构图：细棋盘大地面（mipmap/aniso 信号源）+ 贴 DDS 的立方体。"""
    ents = [
        {"name": "Ground",
         "transform": {"position": [0, 0, 0], "rotation": [0, 0, 0],
                       "scale": [1, 1, 1]},
         "visible": True, "castShadow": True, "parent": -1,
         "mesh": {"kind": "builtin", "shape": "plane", "size": 60.0,
                  "name": "ground"},
         "material": mat_checker("tex_ground", 4)},
        {"name": "BcCube",
         "transform": {"position": [0, 3, 1.5], "rotation": [0, 25, 0],
                       "scale": [1, 1, 1]},
         "visible": True, "castShadow": True, "parent": -1,
         "mesh": {"kind": "builtin", "shape": "cube", "size": 3.0,
                  "name": "bccube"},
         "material": mat_with_tex("tex_bc", "textures/bc1_test.dds")},
    ]
    doc = {"version": 1, "generator": "verify_textures.py",
           "light": {"direction": [-0.4, -0.6, -0.6],
                     "color": [1.0, 0.97, 0.92], "intensity": 0.5,
                     "castsShadow": False, "ambientScale": 0.4},
           "entities": ents}
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)


# 斜俯视：地面高频棋盘在远处被压缩 → mipmap/各向异性的差异区
# （机位经实测验证：俯角 28° 的 "0,-20,14,..." 会整屏看到天空）
CAM = "0,-26,9,90,20"

BASE_ENV = {
    "MYVK_LIGHT_GIZMOS": "0",
    "MYVK_EDITOR_NO_HUD": "1",
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe",
                    default=os.path.join(ROOT, "build", "bin", "Editor.exe"))
    args = ap.parse_args()
    exe = os.path.abspath(args.exe)
    os.makedirs(OUT_DIR, exist_ok=True)
    if not os.path.exists(exe):
        print("找不到可执行文件，请先构建：", exe)
        return 1

    fails = []

    def check(name, cond, detail=""):
        print(("  [PASS] " if cond else "  [FAIL] ") + name +
              (("  " + str(detail)) if detail else ""))
        if not cond:
            fails.append(name)

    kill_stale_editors()
    scene = os.path.join(OUT_DIR, "textures.json")
    build_scene(scene)

    def run(tag, on=None, env_extra=None):
        env = dict(os.environ)
        env.update(BASE_ENV)
        env["MYVK_EDITOR_SCENE"] = scene
        env["MYVK_EDITOR_CAM"] = CAM
        # 特性钩子全关 = 基线
        env["MYVK_MIPMAP"] = "0"
        env["MYVK_TEXSTREAM"] = "0"
        if on:
            for k, v in on.items():
                env[k] = str(v)
        if env_extra:
            env.update(env_extra)
        return run_once(exe, scene, env, os.path.join(OUT_DIR, "tex_%s.png"
                                                      % tag), cam=CAM)

    def diff_n(imgA, imgB, vpA, vpB, thr=18):
        """公共视口内的差异像素数 / 最大差。

        ⚠ `thr` 必须显式传：`verify_local_shadow.diff_stats` 内部硬编码了
        `d > 18` 的计数阈值，而本文件的断言有的只要求 `max 差 > 8`——
        同一行里两个阈值不自洽（差在 9..18 之间的画面：`mx` 达标而 `n`
        恒为 0）。凡是要用 `n` 的断言，就让 `thr` 与它的"显著"口径一致。
        """
        if imgA is None or imgB is None or vpA is None or vpB is None:
            return -1, -1
        a, b = vp_box(vpA), vp_box(vpB)
        box = (max(a[0], b[0]), max(a[1], b[1]),
               min(a[2], b[2]), min(a[3], b[3]))
        n, mx, _, _ = diff_stats(imgA, imgB, box, thr=thr)
        return n, mx

    # ---- 组 1：基线确定性（默认跑两次逐像素一致）----
    _, vpA, imgA, eA = run("base1")
    _, vpB, imgB, eB = run("base2")
    n, mx = diff_n(imgA, imgB, vpA, vpB)
    check("基线确定性（两次逐像素一致）", n == 0, "n=%d max=%d" % (n, mx))
    check("基线轮零 VUID / 零 Fatal", not (eA or eB), (eA + eB)[:2])

    # ---- 组 2：mipmap（远景高频棋盘差异显著）----
    _, vpC, imgC, eC = run("mip_base")
    lm, vpD, imgD, eD = run("mip_on", on={"MYVK_MIPMAP": "1"})
    n18, mx = diff_n(imgC, imgD, vpC, vpD)
    # 断言里说的是"差异显著"，口径就该和 `mx > 8` 对齐（用 >8 计数），
    # 而不是复用 `n` 的 >18 —— 否则 mx=17 时会自相矛盾地判 FAIL。
    n8, _ = diff_n(imgC, imgD, vpC, vpD, thr=8)
    print("     mipmap：差异像素(>8)=%d 差异像素(>18)=%d 最大差=%d"
          % (n8, n18, mx))
    check("mipmap 开/关差异显著（远景摩尔纹被压掉）",
          n8 > 800 and mx > 8, "n(>8)=%d max=%d" % (n8, mx))
    mip_lines = [l for l in lm if "Texture mip chain" in l]
    check("日志确认 mip 链生成（levels>1）",
          any("levels=" in l and not l.endswith("levels=1") for l in mip_lines),
          mip_lines[:1])
    check("mipmap 轮零 VUID / 零 Fatal", not (eC or eD), (eC + eD)[:2])

    # ---- 组 3：各向异性（ANISO=1 各向同性 vs 默认设备上限 8）----
    _, vpE, imgE, eE = run("aniso1", on={"MYVK_MIPMAP": "1", "MYVK_ANISO": "1"})
    _, vpF, imgF, eF = run("aniso8", on={"MYVK_MIPMAP": "1", "MYVK_ANISO": "8"})
    n, mx = diff_n(imgE, imgF, vpE, vpF)
    print("     aniso：差异像素=%d 最大差=%d" % (n, mx))
    check("各向异性 1 vs 8 差异显著（斜视面清晰度）",
          n > 500 and mx > 8, "n=%d max=%d" % (n, mx))
    check("aniso 轮零 VUID / 零 Fatal", not (eE or eF), (eE + eF)[:2])

    # ---- 组 4：BC 压缩（DDS 加载 + 采样）----
    lb, vpG, imgG, eG = run("bc")
    bc_lines = [l for l in lb if "Texture(DDS)" in l]
    check("DDS 加载日志确认 BC1 格式与 mip 层数",
          any("format=BC1" in l and "levels=10" in l for l in bc_lines),
          bc_lines[:1] or "Texture(DDS) 未出现")
    check("BC 轮零 VUID / 零 Fatal", not eG, eG[:2])
    check("BC 轮截图非空", imgG is not None)

    # ---- 组 5：纹理流（逐层补传，最终帧与一次性上传一致）----
    lt, vpH, imgH, eH = run("stream", on={"MYVK_TEXSTREAM": "1"})
    n, mx = diff_n(imgG, imgH, vpG, vpH)
    print("     texstream：最终帧与一次性上传差异像素=%d 最大差=%d" % (n, mx))
    check("流式最终帧与一次性上传逐像素一致（流完整性）",
          n == 0, "n=%d max=%d" % (n, mx))
    begin = [l for l in lt if "TEXSTREAM begin" in l]
    done = [l for l in lt if "TEXSTREAM complete" in l]
    lvls = [l for l in lt if "TEXSTREAM level " in l]
    check("流日志：begin → 逐层 → complete",
          bool(begin) and bool(done) and len(lvls) >= 9,
          "begin=%d complete=%d levels=%d" % (len(begin), len(done), len(lvls)))
    check("流轮零 VUID / 零 Fatal", not eH, eH[:2])

    # 汇总行：run_regression.py 靠它判定 ok
    print("\n结果: %s" % ("PASS" if not fails else "FAIL: " + ", ".join(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
