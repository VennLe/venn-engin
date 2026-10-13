"""端到端验证：Bindless / descriptor indexing（R2-3）。

被测链路（rhi::Device 的 1.2 特性 + render/Bindless + pbr.bindless.frag +
Renderer 接线 + CMake 的 8 份 bindless 变体）：

    场景纹理（14 张）--(BindlessTextures::textureIndex)-->  set 1 binding 0
                                                           `sampler2D textures[]`
    材质（6 个）     --(materialPage)------------------->  set 1 binding 1
                                                           `GpuMaterialPage pages[]`
    逐 draw：push.emissive.w = 页号 --> pbr.bindless.frag 里
             texture(textures[pages[页号].tex0.x], uv)

沿用全套既有约定：**默认关 = 基线逐像素不变**。bindless 只改"贴图从哪张
描述符来"，不改任何一次采样的数学 —— 所以"开 vs 关逐像素一致"是最关键
的一条正确性断言。索引映射只要错一位（albedo 取到 normal），画面立刻爆。

断言分组：

  A. **关着 = 基线**
     · CULL-STATS 全程 bl=0 / blTex=0 / blPages=0；无 BINDLESS-STATS；
     · 两次同配置运行，视口区域逐像素一致。

  B. **Bindless 真的开着**
     · CULL-STATS bl=1；BINDLESS-STATS 出现且 setsBound ≥ 1（set 1 确实绑了）。

  C. **纹理数组与去重**
     · 注册纹理数 ≥ 场景里的 14 张 + 3 张缺省图 = 17；
     · 且远小于"每材质每槽各注册一张"的 39 —— 共享的贴图被**去重**成
       同一个下标（同一指针第二次查表是 O(1) 命中）。

  D. **材质页**
     · 本帧用掉的页数 = 场景材质数（6）—— 惰性分配 + 同指针去重。

  E. **提交量不变**（本任务**不**改 drawcall 数，只改描述符绑定数）
     · 开 vs 关：drawCalls 相同、drawnOpaque 相同。

  F. **画面逐像素不变**（最关键的正确性）
     · 开 vs 关：视口内 >18 的像素数 = 0，平均差 ≤ 0.35；
     · bindless + MDI（间接绘制路径的另一条 push 填充分支）同样逐像素不变。

  G. 全程零 Vulkan 验证层报错。

用法：
    python tools/verify_bindless.py
"""

import argparse
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verify_hiz import run_once                                   # noqa: E402
from verify_add_menu import kill_stale_editors                    # noqa: E402
from verify_local_shadow import diff_stats                        # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = os.path.join(ROOT, "..", "_venn_verify")

CULL_RE = re.compile(
    r"CULL-STATS renderables=(\d+) culled=(\d+) drawnOpaque=(\d+) "
    r"drawnTransparent=(\d+) gizmosCulled=(\d+) drawCalls=(\d+) tris=(\d+) "
    r"enabled=(\d+).*occl=(\d+) occlOn=(\d+) hizMips=(\d+) "
    r"mdi=(\d+) mdiSub=(\d+) mdiBatches=(\d+) mdiInst=(\d+) "
    r"bl=(\d+) blTex=(\d+) blPages=(\d+)")
BL_RE = re.compile(
    r"BINDLESS-STATS textures=(\d+)/(\d+) pages=(\d+)/(\d+) peak=(\d+) "
    r"setsBound=(\d+) meshes=(\d+)")

# CULL-STATS 的分组下标（免得下面到处写魔数）
I_DRAWN_OPAQUE, I_DRAW_CALLS = 2, 5
I_MDI, I_BL, I_BL_TEX, I_BL_PAGES = 11, 15, 16, 17

N_MATERIALS = 6
N_SHARED_TEXTURES = 14          # 场景里真正用到的不同贴图张数
N_DEFAULTS = 3                  # 白 / 平坦法线 / 黑
N_REGISTERED = N_SHARED_TEXTURES + N_DEFAULTS
N_NAIVE = N_MATERIALS * 6 + N_DEFAULTS   # 不去重时的张数（39）


def _rgba(c, a=255):
    return [c[0], c[1], c[2], a]


def checker(name, a, b, cell=16, size=64):
    return {"kind": "checker", "name": name, "colorA": _rgba(a),
            "colorB": _rgba(b), "cell": cell, "size": size}


# ---- 贴图池：故意让相邻材质**共享**一部分贴图 ----
# 共享是刻意的：它让"按钮 3"（去重）有可断言的空间 —— 如果实现按
# "材质 × 槽"各注册一张，注册数会变成 39 而不是 17。
TEX_ALBEDO = [checker("bl_alb%d" % i, (40 + 30 * i, 30 + 20 * i, 200 - 25 * i),
                      (220 - 20 * i, 210, 60 + 25 * i))
              for i in range(1, N_MATERIALS + 1)]
TEX_NORMAL = [checker("bl_nrm%d" % i, (128, 128, 255), (140, 118, 250), 24, 64)
              for i in range(1, 3)]
TEX_ORM = checker("bl_orm1", (200, 200, 200), (185, 175, 190), 24, 64)
TEX_ROUGH = [checker("bl_rgh%d" % i, (230, 230, 230), (140, 140, 140), 20, 64)
             for i in range(1, 3)]
TEX_METAL = checker("bl_mtl1", (255, 255, 255), (90, 90, 90), 24, 64)
TEX_EMIT = [checker("bl_emt%d" % i, (255, 60, 20), (10, 10, 30), 20, 64)
            for i in range(1, 3)]


def build_scene(path):
    """6 个物体 × 6 个材质，每个材质的 6 个贴图槽**全部非空**。

    为什么必须把 6 个槽都填上：只有全填，bindless 的 6 条下标才全部
    参与采样 —— 任何一条映射错位（albedo↔normal、orm↔roughness）都会
    立刻改变画面，被 F 组的逐像素对照抓住。

    摆在相机正前方一小片区域（相机在 (0,0,3) 朝 +X，fovY=45°，
    在 x=3.5 处视野半高 ≈ 1.45）—— 6 个物体必须**全部可见**，
    否则"页数 = 材质数"那条断言会因为视锥剔除而少算。

    全部 castShadow=False：阴影图的绘制列表与主列表共用材质，
    这里只想让主视口这一条路径可断言。
    """
    def mat(name, i):
        return {
            "kind": "procedural", "name": name,
            "baseColor": [1.0, 1.0, 1.0, 1.0],
            # 因子保持中性：颜色全部来自贴图，贴图接错会立刻显形
            "metallic": 1.0, "roughness": 1.0, "normalScale": 1.0, "ao": 1.0,
            "emissive": [0.0, 0.0, 0.0], "doubleSided": False,
            "alphaBlend": False,
            "albedo": TEX_ALBEDO[i],
            "normal": TEX_NORMAL[i % 2],
            "orm": TEX_ORM,
            "roughnessMap": TEX_ROUGH[i % 2],
            "metallicMap": TEX_METAL,
            "emissiveMap": TEX_EMIT[i % 2],
        }

    def ent(name, pos, mesh, material):
        return {
            "name": name,
            "transform": {"position": list(pos), "rotation": [0, 0, 0],
                          "scale": [1.0, 1.0, 1.0]},
            "visible": True, "castShadow": False, "parent": -1,
            "mesh": mesh, "material": material,
        }

    ents = []
    for i in range(N_MATERIALS):
        col = i % 3                 # 屏幕横向（世界 +Z）
        row = i // 3                # 屏幕纵向（世界 +Y）
        pos = (3.5, -0.7 + 1.4 * row, 2.1 + 0.9 * col)
        if row == 0:
            mesh = {"kind": "builtin", "shape": "sphere", "size": 0.7,
                    "segments": 20, "rings": 12, "name": "bl_sph"}
        else:
            mesh = {"kind": "builtin", "shape": "cube", "size": 0.7,
                    "name": "bl_cube"}
        ents.append(ent("Bindless%d" % i, pos, mesh,
                        mat("bl_mat%d" % i, i)))

    doc = {
        "version": 1,
        "generator": "verify_bindless.py",
        "light": {"direction": [-1.0, 0.2, 0.3], "color": [1.0, 0.98, 0.95],
                  "intensity": 1.0, "castsShadow": False, "ambientScale": 1.0},
        "entities": ents,
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)


def cull_rows(lines):
    out = []
    for l in lines:
        m = CULL_RE.search(l)
        if m:
            out.append(tuple(int(v) for v in m.groups()))
    return out


def bl_rows(lines):
    """(textures, maxTextures, pages, pageCapacity, peak, setsBound, meshes)"""
    out = []
    for l in lines:
        m = BL_RE.search(l)
        if m:
            out.append(tuple(int(v) for v in m.groups()))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    os.makedirs(OUT_DIR, exist_ok=True)
    if not os.path.exists(exe):
        print("找不到可执行文件，请先构建：", exe)
        return 1

    scene = os.path.join(OUT_DIR, "bindless_test.json")
    build_scene(scene)
    print("测试场景：%s（%d 材质 × 6 贴图槽，共用 %d 张贴图）"
          % (scene, N_MATERIALS, N_SHARED_TEXTURES))

    fails = []

    def check(name, cond, detail=""):
        print(("  [PASS] " if cond else "  [FAIL] ") + name +
              (("  " + str(detail)) if detail else ""))
        if not cond:
            fails.append(name)

    DBG = {"MYVK_FRUSTUM_DBG": "1", "MYVK_BINDLESS_DBG": "1"}

    kill_stale_editors()

    # ================= A 组：关着 = 基线 =================
    print("\n== A 组：Bindless 关闭 → 旧路径，两次运行逐像素一致 ==")
    shot_off = os.path.join(OUT_DIR, "bindless_off.png")
    lines_a, vp, img_off, errs_a = run_once(exe, scene, dict(DBG), shot_off)
    if not vp:
        print("  没等到 VP-RECT")
        return 2
    cull_a = cull_rows(lines_a)
    print("  CULL-STATS 行数=%d 样例=%s" % (len(cull_a),
                                            cull_a[-1] if cull_a else None))
    check("拿到 CULL-STATS 日志", len(cull_a) > 0)
    check("零 Vulkan 验证层报错", not errs_a, errs_a[:2])
    if cull_a:
        check("关着时 bl 全程 = 0", set(r[I_BL] for r in cull_a) == {0},
              sorted(set(r[I_BL] for r in cull_a)))
        check("关着时 blTex 全程 = 0", set(r[I_BL_TEX] for r in cull_a) == {0},
              sorted(set(r[I_BL_TEX] for r in cull_a)))
        check("关着时没有 BINDLESS-STATS", not bl_rows(lines_a),
              len(bl_rows(lines_a)))

    shot_off2 = os.path.join(OUT_DIR, "bindless_off2.png")
    _, _, img_off2, errs_a2 = run_once(exe, scene, dict(DBG), shot_off2)
    check("第二次运行零报错", not errs_a2, errs_a2[:2])
    if img_off is not None and img_off2 is not None:
        cnt, mx, mean, _ = diff_stats(img_off, img_off2, vp)
        print("  两次运行：>18 像素=%d 最大差=%d 平均差=%.4f" % (cnt, mx, mean))
        check("关着时两次运行视口逐像素一致（平均差 < 0.01）", mean < 0.01,
              "%.4f" % mean)

    if img_off is None:
        print("  基线截图抓不到（窗口前台时序）—— 无法继续像素断言")
        print("\n结果: FAIL: 基线截图")
        return 1

    calls_off = cull_a[-1][I_DRAW_CALLS] if cull_a else -1
    drawn_off = cull_a[-1][I_DRAWN_OPAQUE] if cull_a else -1
    print("  基线：drawnOpaque=%d drawCalls=%d" % (drawn_off, calls_off))
    check("基线的 6 个物体全部可见", drawn_off == N_MATERIALS, drawn_off)

    # ================= B/C/D/E/F 组：打开 bindless =================
    print("\n== B/C/D/E 组：打开 Bindless → 纹理数组 + 材质页 ==")
    shot_on = os.path.join(OUT_DIR, "bindless_on.png")
    lines_b, _, img_on, errs_b = run_once(exe, scene, dict(DBG,
                                                          MYVK_BINDLESS="1"),
                                          shot_on)
    check("零 Vulkan 验证层报错", not errs_b, errs_b[:2])

    cull_b = cull_rows(lines_b)
    bl_b = bl_rows(lines_b)
    print("  CULL-STATS 行数=%d 样例=%s" % (len(cull_b),
                                            cull_b[-1] if cull_b else None))
    print("  BINDLESS-STATS 行数=%d 样例=%s" % (len(bl_b),
                                               bl_b[-1] if bl_b else None))

    check("打开时 CULL-STATS bl 全程 = 1",
          bool(cull_b) and set(r[I_BL] for r in cull_b) == {1},
          sorted(set(r[I_BL] for r in cull_b)) if cull_b else None)
    check("打开时 BINDLESS-STATS 出现", len(bl_b) > 0)

    # C 组：纹理数组 + 去重
    tex_on = max((r[I_BL_TEX] for r in cull_b), default=0)
    print("  注册纹理数 blTex=%d（场景 %d + 缺省 %d = %d；不去重会是 %d）"
          % (tex_on, N_SHARED_TEXTURES, N_DEFAULTS, N_REGISTERED, N_NAIVE))
    check("场景里所有贴图都注册进数组（blTex ≥ %d）" % N_REGISTERED,
          tex_on >= N_REGISTERED, tex_on)
    check("共享贴图被去重（blTex < %d）" % N_NAIVE, tex_on < N_NAIVE, tex_on)

    # D 组：材质页
    pages_on = max((r[I_BL_PAGES] for r in cull_b), default=0)
    print("  材质页数 blPages=%d（场景材质 %d）" % (pages_on, N_MATERIALS))
    check("每帧页数 = 场景材质数（惰性分配 + 同指针去重）",
          pages_on == N_MATERIALS, pages_on)

    if bl_b:
        cap = bl_b[-1][1]
        bound = max(r[5] for r in bl_b)
        peak = max(r[4] for r in bl_b)
        print("  数组容量=%d 本帧峰值页数=%d set1 绑定次数=%d"
              % (cap, peak, bound))
        check("纹理数组容量 ≥ 场景贴图数", cap >= N_REGISTERED, cap)
        check("本帧 set 1 被绑过（setsBound ≥ 1）", bound >= 1, bound)

    # E 组：提交量不变（本任务只改描述符绑定，不改 drawcall 数）
    calls_on = cull_b[-1][I_DRAW_CALLS] if cull_b else -1
    drawn_on = cull_b[-1][I_DRAWN_OPAQUE] if cull_b else -1
    print("  打开时 drawnOpaque=%d drawCalls=%d（基线 %d / %d）"
          % (drawn_on, calls_on, drawn_off, calls_off))
    check("drawnOpaque 与基线相同（几何提交不受影响）", drawn_on == drawn_off,
          "%d / %d" % (drawn_on, drawn_off))
    check("drawCalls 与基线相同（bindless 只减描述符绑定）",
          calls_on == calls_off, "%d / %d" % (calls_on, calls_off))

    # F 组：画面逐像素不变
    if img_on is not None:
        cnt, mx, mean, _ = diff_stats(img_on, img_off, vp)
        print("  开 vs 关：>18 像素=%d 最大差=%d 平均差=%.4f" % (cnt, mx, mean))
        check("Bindless 不改变画面（>18 像素 = 0）", cnt == 0, cnt)
        check("平均差 ≤ 0.35", mean <= 0.35, "%.4f" % mean)
    else:
        check("打开时抓到截图", False, "抓图失败（前台时序 flaky）")

    # ================= F2 组：bindless + MDI（另一条 push 填充路径）=========
    print("\n== F2 组：Bindless + Multi-draw indirect → 间接路径同样逐像素不变 ==")
    shot_mdi = os.path.join(OUT_DIR, "bindless_mdi.png")
    lines_c, _, img_mdi, errs_c = run_once(
        exe, scene, dict(DBG, MYVK_BINDLESS="1", MYVK_MDI="1"), shot_mdi)
    check("零 Vulkan 验证层报错", not errs_c, errs_c[:2])
    cull_c = cull_rows(lines_c)
    if cull_c:
        print("  CULL-STATS 样例=%s" % (cull_c[-1],))
        check("bindless + MDI 同时开着（bl=1 mdi=1）",
              cull_c[-1][I_BL] == 1 and cull_c[-1][I_MDI] == 1,
              "bl=%d mdi=%d" % (cull_c[-1][I_BL], cull_c[-1][I_MDI]))
        check("MDI 路径里材质页照常分配",
              cull_c[-1][I_BL_PAGES] == N_MATERIALS,
              cull_c[-1][I_BL_PAGES])
    if img_mdi is not None:
        cnt, mx, mean, _ = diff_stats(img_mdi, img_off, vp)
        print("  bindless+MDI vs 基线：>18 像素=%d 最大差=%d 平均差=%.4f"
              % (cnt, mx, mean))
        check("bindless + MDI 不改变画面（>18 像素 = 0）", cnt == 0, cnt)
        check("平均差 ≤ 0.35", mean <= 0.35, "%.4f" % mean)
    else:
        check("bindless + MDI 抓到截图", False, "抓图失败（前台时序 flaky）")

    print("\n结果: %s" % ("PASS" if not fails else "FAIL: " + ", ".join(fails)))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
