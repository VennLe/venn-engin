"""端到端验证：GPU instancing / 合批（Renderer::buildOpaqueBatches）。

背景：此前每个 submesh 一次 vkCmdDrawIndexed —— 同一个网格摆 8 份就付
8 次完整的"绑管线 + 推材质 + 绑 6 个纹理 set + 绑 VB/IB"。合批后，
不透明列表按 (关键字掩码, 材质, 网格) 稳定排序、相邻同键项聚合成一批：
状态切换每批付一次，vkCmdDrawIndexedInstanced(count) 展开，每实例的
model 矩阵放在 set 0 binding 13 的 SSBO 里（pbr.inst.vert 用
gl_InstanceIndex 读，firstInstance 即全局槽基址）。

断言四组：

  1. **像素等价（核心断言）**：MYVK_INSTANCING 关 vs 开，视口内必须
     **逐像素相同**。依据：合批只改变提交分组，不改变任何着色输入 ——
     实例 SSBO 里的 mat4 与原来 push 的 model 数值逐位相同，VS 的乘法
     顺序一致，深度预通道与前向也换用同一份 instancing 版顶点着色器。
     只要差 1 个像素就说明有东西变了。
  2. **DRAW-BATCH 日志**：场景 = 地面 + 4 红 cube（共享材质）+ 4 蓝 cube
     （共享材质）+ 1 独立球体 → 不透明 items=10、批 = ground/红/蓝/球
     各 1 = **4**、saved=6。instancing 关时 batches == items。
  3. **CULL-STATS 一致**：剔除口径不受合批影响，两轮 renderables 相同。
  4. 零 VUID / 零 Fatal。

⚠ 透明列表刻意不合批（排序语义优先），本场景也不放透明物 —— 那是
  基线行为的对照面。
⚠ 像素比对只统计视口矩形内（ImGui 面板排版抖动，见 verify_local_shadow）。

用法：
    python tools/verify_instancing.py
"""

import argparse
import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verify_add_menu import kill_stale_editors                       # noqa: E402
from verify_local_shadow import diff_stats, vp_box                   # noqa: E402
from verify_shader_variants import run_once                          # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "..", "_venn_verify")

CAM = "0,-34,15,90,16"

BATCH_RE = re.compile(r"DRAW-BATCH items=(\d+) batches=(\d+) saved=(\d+) "
                      r"transparent=(\d+) instancing=(\d)")
CULL_RE = re.compile(r"CULL-STATS renderables=(\d+) culled=(\d+) "
                     r"drawnOpaque=(\d+) drawnTransparent=(\d+)")


# ============================================================
# 场景：地面 + 4 红 cube + 4 蓝 cube（各自**共享**同一份材质 ——
# 材质名相同 AssetManager 才会给同一个 Material 实例，这是合批的
# 前提之一）+ 1 个独立材质的球体。
# 期望批划分：ground / 红 / 蓝 / 球 各 1 批 = 4 批，items = 10。
# ============================================================
def mat(name, base):
    return {"kind": "standard", "name": name,
            "baseColor": list(base) + [1.0],
            "metallic": 0.0, "roughness": 0.85, "normalScale": 0.0,
            "ao": 1.0, "emissive": [0, 0, 0],
            "doubleSided": False, "alphaBlend": False}


def entity(name, pos, shape, size, material, mesh_name=None):
    # mesh_name：AssetManager 的资源键。**同批实体必须共享同一个键** ——
    # 键不同就是不同的 Mesh 对象，(掩码,材质,网格) 聚合永远不会命中。
    # 不传时用形状名（同形状同尺寸的实体天然共享一份网格资源）。
    return {"name": name,
            "transform": {"position": list(pos), "rotation": [0, 0, 0],
                          "scale": [1, 1, 1]},
            "visible": True, "castShadow": True, "parent": -1,
            "mesh": {"kind": "builtin", "shape": shape, "size": size,
                     "name": mesh_name or shape},
            "material": material}


def build_scene(path):
    ents = [entity("Ground", (0, 0, 0), "plane", 80.0,
                   mat("ins_ground", (0.70, 0.70, 0.73)))]
    red = mat("ins_red", (0.85, 0.30, 0.22))
    blue = mat("ins_blue", (0.25, 0.50, 0.85))
    # 两排各 4 个：同 mesh 资源（ins_cube）、同材质 → 各聚成一批
    for i in range(4):
        ents.append(entity("RedCube%d" % i, (-12.0 + 8.0 * i, 3.0, 6.0),
                           "cube", 4.0, red, mesh_name="ins_cube"))
        ents.append(entity("BlueCube%d" % i, (-12.0 + 8.0 * i, 3.0, -6.0),
                           "cube", 4.0, blue, mesh_name="ins_cube"))
    ents.append(entity("Ball", (0.0, 7.5, 0.0), "sphere", 3.0,
                       mat("ins_ball", (0.90, 0.80, 0.35))))
    doc = {"version": 1, "generator": "verify_instancing.py",
           "light": {"direction": [-0.35, -0.5, -0.79],
                     "color": [1.0, 0.97, 0.92], "intensity": 0.35,
                     "castsShadow": True, "ambientScale": 0.25},
           "entities": ents}
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)


def last_batch(lines):
    m = None
    for line in lines:
        mm = BATCH_RE.search(line)
        if mm:
            m = mm
    if not m:
        return None
    return {"items": int(m.group(1)), "batches": int(m.group(2)),
            "saved": int(m.group(3)), "instancing": int(m.group(5))}


def last_cull(lines):
    m = None
    for line in lines:
        mm = CULL_RE.search(line)
        if mm:
            m = mm
    return m.groups() if m else None


# ============================================================
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
    scene = os.path.join(OUT_DIR, "instancing.json")
    build_scene(scene)

    def run(instancing, tag):
        env = {
            "MYVK_INSTANCING": "1" if instancing else "0",
            "MYVK_DRAW_BATCH": "1",
            "MYVK_LIGHT_GIZMOS": "0",
            "MYVK_EDITOR_NO_HUD": "1",
            "MYVK_EDITOR_SCENE": scene,
            "MYVK_EDITOR_CAM": CAM,
            "MYVK_LOG_RECTS": "1",
            "MYVK_NO_WINDOW_SAVE": "1",
            "MYVK_ENV_DBG": "0",
            "MYVK_SHADER_VARIANTS": "1",
            "MYVK_FRUSTUM_DBG": "1",
        }
        lines, vp, img, errs = run_once(
            exe, scene, env, os.path.join(OUT_DIR, "instancing_%s.png" % tag),
            cam=CAM)
        return lines, vp, img, errs

    # ---- 两轮：基线（逐项提交）vs 合批 ----
    lines0, vp0, img0, errs0 = run(False, "off")
    lines1, vp1, img1, errs1 = run(True, "on")

    # ---- 组 1：像素等价 ----
    check("两轮都拿到截图", img0 is not None and img1 is not None)
    if img0 is not None and img1 is not None and vp0 is not None:
        vp = vp_box(vp0)
        n, mx, mean, box = diff_stats(img0, img1, vp)
        print("     关 vs 开：差异像素=%d 最大差=%d bbox=%s" % (n, mx, box))
        check("像素等价（合批不改画面，一个像素都不能差）", n == 0,
              "n=%d max=%d" % (n, mx))
    else:
        check("像素等价（合批不改画面，一个像素都不能差）", False, "截图缺失")

    # ---- 组 2：DRAW-BATCH ----
    b = last_batch(lines1)
    check("DRAW-BATCH 日志存在", b is not None)
    if b:
        print("     DRAW-BATCH items=%d batches=%d saved=%d instancing=%d"
              % (b["items"], b["batches"], b["saved"], b["instancing"]))
        check("items = 10（地面 + 8 cube + 球）", b["items"] == 10, b)
        check("batches = 4（ground/红/蓝/球各一批）", b["batches"] == 4, b)
        check("saved = 6（10 - 4）", b["saved"] == 6, b)
        check("instancing 标志 = 1", b["instancing"] == 1, b)
    b0 = last_batch(lines0)
    if b0:
        check("关闭时 batches == items（基线不走合批）",
              b0["batches"] == b0["items"] and b0["instancing"] == 0, b0)

    # ---- 组 3：CULL-STATS 口径一致 ----
    c1 = last_cull(lines1)
    check("CULL-STATS 存在", c1 is not None)
    if c1:
        check("renderables = 10（剔除前总量稳定）", int(c1[0]) == 10, c1)

    # ---- 组 4：验证层干净 ----
    check("两轮零 VUID / 零 Fatal", not (errs0 or errs1),
          (errs0 + errs1)[:2])

    # 汇总行：run_regression.py 靠它（结果: PASS/FAIL）判定 ok，
    # 光有退出码不够 —— 每个用例的最后一行必须是这条。
    print("\n结果: %s" % ("PASS" if not fails else "FAIL: " + ", ".join(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
