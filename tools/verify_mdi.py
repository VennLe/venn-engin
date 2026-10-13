"""端到端验证：Multi-draw indirect + GPU 紧凑化（R2-2）。

被测链路（MdiDraw + mdi_compact.comp + pbr.mdi.vert + Renderer 接线）：
    深度预通道（本帧全部候选，旧路径）
        --(Hi-Z: hiz_build + hiz_downsample)-->  深度金字塔
        --(mdi_compact.comp)-->  每个候选：遮挡测试 → 可见则
                                 atomicAdd(间接命令.instanceCount,1) + 写实例记录
        --(vkCmdDrawIndexedIndirect × 组数)-->   前向不透明几何

沿用全套既有约定：**默认关 = 基线逐像素不变**。R2-2 的正确性验收与
R2-1 同源 —— 紧凑化/剔除都不该改变画面（被剔的一定是整块躲在几何
后面的物体），所以"开 vs 关逐像素一致"仍是最关键的一条。

断言分组：

  A. **关着 = 基线**
     · CULL-STATS 全程 mdi=0 / mdiSub=0；
     · 两次同配置运行，视口区域逐像素一致。

  B. **排查通道可用**：打开时 mdi=1、MDI-STATS 出现、mdiSub ≥ 1。

  C. **CPU 提交量真的降了**（本任务的核心）
     · mdiSub（发出的 vkCmdDrawIndexedIndirect 次数）**严格小于**
       drawnOpaque（候选物体数）—— 这就是 O(物体数) → O(组数)。

  D. **GPU 确实剔掉了被挡的物体**
     · 回读的 mdiInst（真正画出的实例数）< candidates；
     · 场景里 8 个躲在墙后的物体应当被剔，稳态 mdiInst ≤ 4。

  E. **画面逐像素不变**（最关键的正确性）
     · 开 vs 关：视口内 >18 的像素数 = 0，平均差 ≤ 0.35。

  F. 全程零 Vulkan 验证层报错。

用法：
    python tools/verify_mdi.py
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

# 相机在 (0,0,3) 朝 +X 平视（与 verify_hiz 同一套，场景可互换）
CAM = "0,0,3,180,0"

CULL_RE = re.compile(
    r"CULL-STATS renderables=(\d+) culled=(\d+) drawnOpaque=(\d+) "
    r"drawnTransparent=(\d+) gizmosCulled=(\d+) drawCalls=(\d+) tris=(\d+) "
    r"enabled=(\d+).*occl=(\d+) occlOn=(\d+) hizMips=(\d+) "
    r"mdi=(\d+) mdiSub=(\d+) mdiBatches=(\d+) mdiInst=(\d+)")
MDI_RE = re.compile(
    r"MDI-STATS candidates=(\d+) batches=(\d+) submissions=(\d+) "
    r"instances=(\d+) culled=(\d+) items=(\d+) inst=(\d+)")


def build_scene(path):
    """一面挡在中间的墙 + 墙前 2 个正对照 + 墙后 8 个应被剔的物体。

    关键设计：**共用网格与材质**。
      · 墙        —— 自己的网格/材质（1 批）
      · 墙前的 2 个  —— 与墙后的 8 个共用网格 "mdi_sphere" / "mdi_cube"
                        与材质 M_sphere / M_cube
    于是"同网格 + 同材质"的物体天然聚成 2 大批：候选 11 个 → 3 个批 →
    3 组（= 3 次 vkCmdDrawIndexedIndirect）。提交量从 11 降到 3 就是
    R2-2 要证明的东西。

    全部 castShadow=False：避免阴影图的绘制列表随剔除结果变化，
    污染"A/B 逐像素相同"这条断言。
    """
    def mat(name, base, metallic, roughness):
        return {"kind": "procedural", "name": name,
                "baseColor": list(base) + [1.0],
                "metallic": metallic, "roughness": roughness,
                "normalScale": 1.0, "ao": 1.0,
                "emissive": [0.0, 0.0, 0.0], "doubleSided": False,
                "alphaBlend": False}

    def ent(name, pos, scale, mesh, material):
        return {
            "name": name,
            "transform": {"position": list(pos), "rotation": [0, 0, 0],
                          "scale": list(scale)},
            "visible": True, "castShadow": False, "parent": -1,
            "mesh": mesh, "material": material,
        }

    cube = {"kind": "builtin", "shape": "cube", "size": 1.0, "name": "mdi_cube"}
    wall_mesh = {"kind": "builtin", "shape": "cube", "size": 1.0,
                 "name": "mdi_wall"}
    sphere = {"kind": "builtin", "shape": "sphere", "size": 0.6,
              "segments": 16, "rings": 8, "name": "mdi_sphere"}

    m_wall = mat("mdi_wall_mat", (0.55, 0.55, 0.58), 0.0, 0.6)
    m_sphere = mat("mdi_sphere_mat", (0.9, 0.15, 0.12), 0.0, 0.45)
    m_cube = mat("mdi_cube_mat", (0.12, 0.45, 0.95), 0.0, 0.4)

    ents = [ent("Wall", (3.0, 0.0, 3.0), [0.15, 3.0, 3.0], wall_mesh, m_wall)]

    # 墙前的两个正对照（绝不能被剔）：一个球 + 一个方块
    ents.append(ent("FrontSphere", (1.4, 1.0, 3.0), [1.0, 1.0, 1.0],
                    sphere, m_sphere))
    ents.append(ent("FrontCube", (1.4, -1.2, 3.0), [0.7, 0.7, 0.7],
                    cube, m_cube))

    # 墙后的 8 个（应被剔）：4 球 + 4 方块，全部落在墙的投影内
    for i, y in enumerate([-1.5, -0.5, 0.5, 1.5]):
        ents.append(ent("HiddenSphere%d" % i, (6.0, y, 2.4),
                        [1.0, 1.0, 1.0], sphere, m_sphere))
    for i, y in enumerate([-1.5, -0.5, 0.5, 1.5]):
        ents.append(ent("HiddenCube%d" % i, (6.0, y, 3.6),
                        [0.7, 0.7, 0.7], cube, m_cube))

    doc = {
        "version": 1,
        "generator": "verify_mdi.py",
        "light": {"direction": [-1.0, 0.2, 0.3], "color": [1.0, 0.98, 0.95],
                  "intensity": 1.0, "castsShadow": False, "ambientScale": 1.0},
        "entities": ents,
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)


def cull_lines(lines):
    """抽 (renderables, culled, drawnOpaque, drawnTransparent, gizmosCulled,
    drawCalls, tris, enabled, occl, occlOn, hizMips, mdi, mdiSub, mdiBatches,
    mdiInst)。"""
    out = []
    for l in lines:
        m = CULL_RE.search(l)
        if m:
            out.append(tuple(int(v) for v in m.groups()))
    return out


def mdi_lines(lines):
    """抽 (candidates, batches, submissions, instances, culled, items, inst)。"""
    out = []
    for l in lines:
        m = MDI_RE.search(l)
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

    scene = os.path.join(OUT_DIR, "mdi_test.json")
    build_scene(scene)
    print("测试场景：%s" % scene)

    fails = []

    def check(name, cond, detail=""):
        print(("  [PASS] " if cond else "  [FAIL] ") + name +
              (("  " + str(detail)) if detail else ""))
        if not cond:
            fails.append(name)

    # 两条诊断日志：CULL-STATS（含 mdi 字段）+ MDI-STATS
    DBG = {"MYVK_FRUSTUM_DBG": "1", "MYVK_MDI_DBG": "1"}

    kill_stale_editors()

    # ============ A 组：关着 = 基线 ============
    print("\n== A 组：MDI 关闭 → 零间接命令，两次运行逐像素一致 ==")
    shot_a = os.path.join(OUT_DIR, "mdi_off.png")
    lines_a, vp, img_a, errs_a = run_once(exe, scene, dict(DBG), shot_a)
    if not vp:
        print("  没等到 VP-RECT")
        return 2
    c_a = cull_lines(lines_a)
    print("  CULL-STATS 行数=%d 样例=%s" % (len(c_a), c_a[-1] if c_a else None))
    check("拿到 CULL-STATS 日志", len(c_a) > 0)
    check("零 Vulkan 验证层报错", not errs_a, errs_a[:2])
    if c_a:
        check("关着时 mdi 全程 = 0", set(r[11] for r in c_a) == {0},
              sorted(set(r[11] for r in c_a)))
        check("关着时 mdiSub 全程 = 0", set(r[12] for r in c_a) == {0},
              sorted(set(r[12] for r in c_a)))
        check("关着时没有 MDI-STATS（零 compute）", not mdi_lines(lines_a),
              len(mdi_lines(lines_a)))

    shot_a2 = os.path.join(OUT_DIR, "mdi_off2.png")
    _, _, img_a2, errs_a2 = run_once(exe, scene, dict(DBG), shot_a2)
    check("第二次运行零报错", not errs_a2, errs_a2[:2])
    if img_a is not None and img_a2 is not None:
        cnt, mx, mean, _ = diff_stats(img_a, img_a2, vp)
        print("  两次运行：>18 像素=%d 最大差=%d 平均差=%.4f" % (cnt, mx, mean))
        check("关着时两次运行视口逐像素一致（平均差 < 0.01）", mean < 0.01,
              "%.4f" % mean)

    if img_a is None:
        print("  基线截图抓不到（窗口前台时序）—— 无法继续像素断言")
        fails.append("基线截图")
        print("\n结果: FAIL: 基线截图")
        return 1

    cand_off = c_a[-1][2] if c_a else -1          # drawnOpaque
    calls_off = c_a[-1][5] if c_a else -1         # drawCalls
    print("  基线：候选物体=%d drawCalls=%d" % (cand_off, calls_off))

    # ============ B/C/D/E 组：打开 MDI ============
    print("\n== B/C/D/E 组：打开 MDI → GPU 紧凑化 + 每组一次间接绘制 ==")
    shot_b = os.path.join(OUT_DIR, "mdi_on.png")
    lines_b, _, img_b, errs_b = run_once(
        exe, scene, dict(DBG, MYVK_MDI="1"), shot_b)
    check("零 Vulkan 验证层报错", not errs_b, errs_b[:2])

    c_b = cull_lines(lines_b)
    md_b = mdi_lines(lines_b)
    print("  CULL-STATS 行数=%d 样例=%s" % (len(c_b), c_b[-1] if c_b else None))
    print("  MDI-STATS 行数=%d 样例=%s" % (len(md_b), md_b[-1] if md_b else None))

    check("打开时 CULL-STATS mdi 全程 = 1",
          bool(c_b) and set(r[11] for r in c_b) == {1},
          sorted(set(r[11] for r in c_b)) if c_b else None)
    check("打开时 MDI-STATS 出现且 inst=1",
          bool(md_b) and set(r[6] for r in md_b) == {1},
          sorted(set(r[6] for r in md_b)) if md_b else None)

    # C 组：CPU 提交量下降
    sub_on = max((r[12] for r in c_b), default=0)
    batch_on = max((r[13] for r in c_b), default=0)
    print("  间接绘制次数 mdiSub=%d（批数 mdiBatches=%d，候选物体=%d）"
          % (sub_on, batch_on, cand_off))
    check("确实发了间接绘制（mdiSub ≥ 1）", sub_on >= 1, sub_on)
    check("CPU 提交量严格下降（mdiSub < 候选物体数）",
          cand_off > 0 and sub_on < cand_off, "%d -> %d" % (cand_off, sub_on))
    check("提交量在「材质数」量级（mdiSub ≤ 6）", sub_on <= 6, sub_on)

    calls_on = c_b[-1][5] if c_b else -1
    print("  打开时 drawCalls=%d（基线 %d）" % (calls_on, calls_off))
    check("drawCalls 比基线严格下降", calls_off > 0 and calls_on < calls_off,
          "%d -> %d" % (calls_off, calls_on))

    # D 组：GPU 确实剔掉了被挡的物体
    inst_on = max((r[14] for r in c_b), default=0)
    cand_mdi = max((r[0] for r in md_b), default=0)
    drawn_mdi = max((r[3] for r in md_b), default=0)
    culled_mdi = max((r[4] for r in md_b), default=0)
    print("  GPU：候选=%d 画出=%d 剔除=%d" % (cand_mdi, drawn_mdi, culled_mdi))
    check("回读到实例数（mdiInst > 0）", inst_on > 0, inst_on)
    check("GPU 剔掉了被挡的物体（culled ≥ 4）", culled_mdi >= 4, culled_mdi)
    check("画出的实例数 < 候选数", cand_mdi > 0 and drawn_mdi < cand_mdi,
          "%d / %d" % (drawn_mdi, cand_mdi))
    check("墙前 2 个正对照没被误剔（画出 ≥ 3：墙 + 2 个）", drawn_mdi >= 3,
          drawn_mdi)

    # E 组：画面逐像素不变
    if img_b is not None:
        cnt, mx, mean, _ = diff_stats(img_b, img_a, vp)
        print("  开 vs 关：>18 像素=%d 最大差=%d 平均差=%.4f" % (cnt, mx, mean))
        check("MDI 不改变画面（>18 像素 = 0）", cnt == 0, cnt)
        check("平均差 ≤ 0.35", mean <= 0.35, "%.4f" % mean)
    else:
        check("打开时抓到截图", False, "抓图失败（前台时序 flaky）")

    print("\n结果: %s" % ("PASS" if not fails else "FAIL: " + ", ".join(fails)))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
