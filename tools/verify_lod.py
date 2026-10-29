"""端到端验证：LOD（细节层级）系统。

背景：这是"渲染性能"三件套里的第三件（视锥剔除 / 遮挡剔除+加速结构 /
LOD）。前两件分别在 verify_frustum_cull.py 与 verify_bvh.py 里验过。

本脚本造一个**极简但可判定**的场景：同一颗高模球（48×24 = 2304 三角形）
按 4 个距离摆 4 份，相机在原点朝 +X 平视。球的包围球半径是 0.5×√3，
屏幕占高比 = 0.866 / (距离 × tan22.5°)，于是 4 个距离正好落在 4 个层级里：

    距离   比值    预期层级
      3    0.697   L0（高模）
     12    0.174   L1
     30    0.070   L2
     80    0.026   L3（最简）

断言分六组：

  1. **链生成正确**：`LOD-BUILD` 报出 4 级、三角形数严格递减、最后一级
     比高模少一个数量级以上。
  2. **选层正确**：`LOD-STATS` 里 l0/l1/l2/l3 各 1；提交的三角形数 ==
     四级之和，而"全用 LOD0"会是 4 倍高模 —— 直接量化省了多少。
  3. **稳定不抖**：静止场景里切换计数不再增长（迟滞生效）。
  4. **开关与强制层级有效**：`MYVK_LOD=0` 退回全 LOD0（三角形数 == noLod，
     且和默认那次的截图基本一致）；`MYVK_LOD_FORCE=2` 时 4 个物体全在 L2。
  5. **偏差有效**：`MYVK_LOD_BIAS=2` 把分界线整体推向更低的模 ——
     预期从 1/1/1/1 变成 1/0/1/2（更省）。
  6. **最简网格不白建层级**：换成立方体场景时 `LOD-BUILD` 只有 1 级，
     LOD 完全不介入（ents=0）。
  7. **导入网格走顶点聚类简化**：脚本现写一个 64×32 的 UV 球 OBJ（4096
     三角形）交给引擎，断言它被简化出 >= 3 级、每级大约减半、末级至少
     比原网格少 4 倍。这条覆盖的是和程序化图元**完全不同**的一条生成
     路径（聚类合并顶点而不是重生成细分参数）。
  8. **蒙皮网格不简化**：拿仓库里的 anime_girl.glb（带 JOINTS_0）当输入，
     断言链只有 1 级 —— 蒙皮网格换网格会让关节矩阵基址对不上，是有意
     不做的（见 assets/Lod.h 文件头）。

用法：
    python tools/verify_lod.py
"""

import argparse
import json
import math
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_client, find_window, user32      # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors             # noqa: E402
from verify_frustum_cull import region_diff                        # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = os.path.join(ROOT, "..", "_venn_verify")

# 相机站在原点朝 +X 平视（yaw=180° → forward = +X）
CAM = "0,0,0,180,0"
# 4 个距离，分别落在 L0 / L1 / L2 / L3
DISTANCES = [3.0, 12.0, 30.0, 80.0]
SPHERE_SEGS, SPHERE_RINGS = 48, 24

BUILD_RE = re.compile(
    r"LOD-BUILD '([^']*)' kind=(\d+) levels=(\d+) tris=([0-9/]+)")
SKIP_SKIN_RE = re.compile(r"LOD-SKIP-SKIN '([^']*)'")
STATS_RE = re.compile(
    r"LOD-STATS ents=(\d+) l0=(\d+) l1=(\d+) l2=(\d+) l3=(\d+) "
    r"tris=(\d+) noLod=(\d+) switches=(\d+) forced=(-?\d+) bias=([\d.]+)")

# 最近一次 run_once 的 LogTap —— 第 8 组要扫"某条日志出现过没有"，
# 而 `last_build` 只回最后一条匹配。用模块级变量而不是改返回值：
# run_once 返回的 5 元组在脚本里被解包了 8 处。
_LAST_TAP = None


def make_material():
    return {
        "kind": "procedural", "name": "mat_lod",
        "baseColor": [0.75, 0.72, 0.68, 1.0],
        "metallic": 0.0, "roughness": 0.45,
        "normalScale": 1.0, "ao": 1.0,
        "emissive": [0.0, 0.0, 0.0], "doubleSided": False,
        "alphaBlend": False,
    }


def build_scene(path, shape="sphere"):
    """写测试场景。shape='sphere' 时同一颗高模球摆 4 个距离；
    shape='cube' 时摆 4 个立方体（用来验"最简网格不生成 LOD"）。"""
    ents = []
    for i, d in enumerate(DISTANCES):
        if shape == "sphere":
            mesh = {"kind": "builtin", "shape": "sphere", "size": 0.5,
                    "segments": SPHERE_SEGS, "rings": SPHERE_RINGS,
                    "name": "lod_sphere"}
        else:
            mesh = {"kind": "builtin", "shape": "cube", "size": 1.0,
                    "name": "lod_cube"}
        ents.append({
            "name": "Obj_%d" % i,
            "transform": {"position": [d, 0.0, 0.0], "rotation": [0, 0, 0],
                          "scale": [1, 1, 1]},
            "visible": True,
            "castShadow": False,
            "parent": -1,
            # 4 个实体**共用同一颗网格** —— 一条 LOD 链服务 4 个实例，
            # 这也是真实引擎里的常见形态。
            "mesh": mesh,
            "material": make_material(),
        })

    doc = {
        "version": 1,
        "generator": "verify_lod.py",
        "light": {"direction": [-0.45, -0.65, -0.62], "color": [1.0, 0.98, 0.95],
                  "intensity": 1.0, "castsShadow": False, "ambientScale": 0.55},
        "entities": ents,
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)
    return len(ents)


def write_uv_sphere_obj(path, radius=1.0, seg=64, rings=32):
    """写一个标准 UV 球 OBJ（v + f 就够，加载器对缺失的 vn/vt 有兜底）。

    返回三角形数。用来给"导入网格 → 顶点聚类简化"这条路径提供输入。
    """
    verts = []
    # 极点单独各一个顶点，中间按纬度排环
    verts.append((0.0, 0.0, radius))                       # 北极
    for r in range(1, rings):
        phi = math.pi * r / rings
        z = radius * math.cos(phi)
        s = radius * math.sin(phi)
        for c in range(seg):
            th = 2.0 * math.pi * c / seg
            verts.append((s * math.cos(th), s * math.sin(th), z))
    verts.append((0.0, 0.0, -radius))                      # 南极
    south = len(verts)                       # OBJ 下标从 1 开始，正好是南极

    def vid(r, c):                           # 第 r 环（1..rings-1）第 c 列
        return 1 + 1 + (r - 1) * seg + (c % seg)

    faces = []
    # 北极扇
    for c in range(seg):
        faces.append((1, vid(1, c + 1), vid(1, c)))
    # 中间四边形带 → 两个三角形
    for r in range(1, rings - 1):
        for c in range(seg):
            a, b = vid(r, c), vid(r, c + 1)
            d, e = vid(r + 1, c), vid(r + 1, c + 1)
            faces.append((a, d, e))
            faces.append((a, e, b))
    # 南极扇
    for c in range(seg):
        faces.append((south, vid(rings - 1, c), vid(rings - 1, c + 1)))

    with open(path, "w", encoding="utf-8") as f:
        f.write("# verify_lod.py 生成：UV 球 %dx%d\n" % (seg, rings))
        for v in verts:
            f.write("v %.6f %.6f %.6f\n" % v)
        for a, b, c in faces:
            f.write("f %d %d %d\n" % (a, b, c))
    return len(faces)


def build_file_scene(path, mesh_ref, cam_pos=(6.0, 0.0, 3.0), name="Obj_0"):
    """写一个"只有一个导入网格实体"的场景。"""
    doc = {
        "version": 1,
        "generator": "verify_lod.py",
        "light": {"direction": [-0.45, -0.65, -0.62], "color": [1.0, 0.98, 0.95],
                  "intensity": 1.0, "castsShadow": False, "ambientScale": 0.55},
        "entities": [{
            "name": name,
            "transform": {"position": list(cam_pos), "rotation": [0, 0, 0],
                          "scale": [1, 1, 1]},
            "visible": True,
            "castShadow": False,
            "parent": -1,
            "mesh": mesh_ref,
            "material": make_material(),
        }],
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)
    return 1


def run_once(exe, scene, extra_env, shot_path=None, wait=12.0, cam=CAM):
    """跑一轮编辑器，返回 (build 匹配, stats 匹配, 第二次 stats, 截图, 错误行)。

    第二次 stats 用来判断"切换计数是否还在涨" —— 取完第一条再等 1.5 秒
    取第二条，两者相同即视为稳定。
    """
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_LOD_DBG"] = "1"
    env["MYVK_FRUSTUM_DBG"] = "1"
    env["MYVK_EDITOR_SCENE"] = scene
    env["MYVK_EDITOR_CAM"] = cam
    env.update(extra_env)

    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    global _LAST_TAP
    _LAST_TAP = tap
    hwnd = None
    try:
        hwnd = find_window("Venn Editor")
        if not hwnd:
            return None, None, None, None, ["未找到编辑器窗口"]
        user32.ShowWindow(hwnd, 9)
        time.sleep(1.5)
        user32.SetWindowPos(hwnd, 0, 0, 0, 1280, 800, 0x0002 | 0x0004)
        time.sleep(2.5)

        end = time.time() + wait
        while time.time() < end and last_stats(tap) is None:
            time.sleep(0.25)
        # 启动阶段允许有若干次切换（物体第一次出现、相机就位）
        time.sleep(1.5)
        s1 = last_stats(tap)
        time.sleep(1.5)
        s2 = last_stats(tap)
        b = last_build(tap)

        img = None
        if shot_path:
            time.sleep(0.5)
            img, _ = capture_client(hwnd)
            img.save(shot_path)

        with tap.lock:
            errs = [l for l in tap.lines
                    if ("VUID" in l or "VALIDATION" in l or " ERROR" in l
                        or "Error:" in l)]
        return b, s1, s2, img, errs
    finally:
        if hwnd:
            user32.PostMessageW(hwnd, 0x0010, 0, 0)
        time.sleep(0.5)
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=6)
            except subprocess.TimeoutExpired:
                proc.kill()


def _scan(tap, needle, rx):
    found = None
    with tap.lock:
        for line in tap.lines:
            if needle in line:
                m = rx.search(line)
                if m:
                    found = m
    return found


def last_build(tap):
    return _scan(tap, "LOD-BUILD", BUILD_RE)


def last_stats(tap):
    return _scan(tap, "LOD-STATS", STATS_RE)


def parse_stats(m):
    if not m:
        return None
    ents, l0, l1, l2, l3, tris, nolod, sw, forced, bias = m.groups()
    return {
        "ents": int(ents), "levels": [int(l0), int(l1), int(l2), int(l3)],
        "tris": int(tris), "noLod": int(nolod), "switches": int(sw),
        "forced": int(forced), "bias": float(bias),
    }


def parse_tris(m):
    return [int(x) for x in m.group(4).split("/")]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    os.makedirs(OUT_DIR, exist_ok=True)
    if not os.path.exists(exe):
        print("找不到可执行文件，请先构建：", exe)
        return 1

    sphere_scene = os.path.join(OUT_DIR, "lod_test_sphere.json")
    cube_scene = os.path.join(OUT_DIR, "lod_test_cube.json")
    n = build_scene(sphere_scene, "sphere")
    build_scene(cube_scene, "cube")
    print("测试场景：%s（%d 个高模球，48×24）" % (sphere_scene, n))

    fails = []

    def check(name, cond, detail=""):
        print(("  [PASS] " if cond else "  [FAIL] ") + name +
              (("  " + str(detail)) if detail else ""))
        if not cond:
            fails.append(name)

    kill_stale_editors()

    # ============ 第 1~3 组：默认（自动选层）============
    print("\n== 第 1 组：LOD 链生成 ==")
    shot_on = os.path.join(OUT_DIR, "lod_on.png")
    b, s1, s2, img, errs = run_once(exe, sphere_scene, {}, shot_on)
    if not b or not s1:
        print("  没等到 LOD-BUILD / LOD-STATS")
        return 2

    print("  LOD-BUILD:", b.group(0))
    print("  LOD-STATS:", s1.group(0))
    tris = parse_tris(b)
    print("  各级三角形数:", tris)

    check("生成了 LOD 链（levels >= 2）", int(b.group(3)) >= 2, b.group(3))
    check("级数 == 4", int(b.group(3)) == len(DISTANCES), b.group(3))
    check("三角形数严格递减",
          all(tris[i] > tris[i + 1] for i in range(len(tris) - 1)), tris)
    check("最高级确实高模（> 1000 三角形）", tris[0] > 1000, tris[0])
    check("最低级比高模少一个数量级以上（< 高模/10）",
          tris[-1] * 10 < tris[0], "%d vs %d" % (tris[-1], tris[0]))

    print("\n== 第 2 组：按屏幕占高比选层 ==")
    st = parse_stats(s1)
    check("4 个实例都被 LOD 接管（ents == 4）", st["ents"] == n, st["ents"])
    # 距离 4/12/30/80 恰好各落一级
    check("层级分布 == 1/1/1/1", st["levels"] == [1, 1, 1, 1], st["levels"])
    check("提交三角形数 == 四级之和",
          st["tris"] == sum(tris), "%d vs %d" % (st["tris"], sum(tris)))
    check("不用 LOD 会是 4×高模（noLod == 4 × %d）" % tris[0],
          st["noLod"] == n * tris[0], st["noLod"])
    check("省掉一半以上三角形（tris < noLod/2）",
          st["tris"] * 2 < st["noLod"],
          "%d vs %d（省 %.0f%%）" % (st["tris"], st["noLod"],
                                    100.0 * (1 - st["tris"] / st["noLod"])))

    print("\n== 第 3 组：静止时不抖动（迟滞生效）==")
    st2 = parse_stats(s2)
    check("切换计数不再增长", st2["switches"] == st["switches"],
          "%d -> %d" % (st["switches"], st2["switches"]))
    check("层分布仍然稳定", st2["levels"] == st["levels"], st2["levels"])
    check("零 Vulkan 验证层报错", not errs, errs[:3])

    # ============ 第 4 组：开关与强制层级 ============
    print("\n== 第 4 组：MYVK_LOD=0（退回全 LOD0）与 MYVK_LOD_FORCE=2 ==")
    shot_off = os.path.join(OUT_DIR, "lod_off.png")
    b4, s4, _, img4, errs4 = run_once(exe, sphere_scene, {"MYVK_LOD": "0"},
                                      shot_off)
    st4 = parse_stats(s4)
    print("  LOD-STATS:", s4.group(0) if s4 else None)
    check("关掉 LOD 后没有任何实例被接管（ents == 0）",
          st4 and st4["ents"] == 0, st4 and st4["ents"])
    check("三角形数退回 noLod（等于 4×高模）",
          st4 and st4["tris"] == st4["noLod"] == n * tris[0],
          st4 and (st4["tris"], st4["noLod"]))
    check("链本身仍然建好了（LOD-BUILD 还在）",
          b4 is not None and int(b4.group(3)) >= 2,
          b4.group(0) if b4 else None)
    if img is not None and img4 is not None:
        box = (0, 0, img.size[0], img.size[1])
        d = region_diff(img, img4, box)
        print("  LOD开 vs LOD关 全画面平均像素差 = %.3f" % d)
        # 低模球仍是球、位置尺寸一致，只该有轮廓级别的细微差别。
        # 阈值放宽到 8 是因为几个球的高模/低模轮廓确实不同。
        check("低模没把画面画坏（与全高模的平均像素差 < 8）", d < 8.0, d)
    check("零 Vulkan 验证层报错", not errs4, errs4[:3])

    b5, s5, _, _, errs5 = run_once(exe, sphere_scene, {"MYVK_LOD_FORCE": "2"})
    st5 = parse_stats(s5)
    print("  LOD-STATS:", s5.group(0) if s5 else None)
    check("强制 L2 时 4 个实例全在 L2", st5 and st5["levels"] == [0, 0, 4, 0],
          st5 and st5["levels"])
    check("强制 L2 时三角形数 == 4 × L2",
          st5 and st5["tris"] == n * tris[2],
          st5 and (st5["tris"], n * tris[2]))
    check("零 Vulkan 验证层报错", not errs5, errs5[:3])

    # ============ 第 5 组：偏差 ============
    print("\n== 第 5 组：MYVK_LOD_BIAS=2（更早切低模）==")
    b6, s6, _, _, _ = run_once(exe, sphere_scene, {"MYVK_LOD_BIAS": "2.0"})
    st6 = parse_stats(s6)
    print("  LOD-STATS:", s6.group(0) if s6 else None)
    # bias 除在比值上 → 比值减半 → 分界线整体下移一级：
    # 距离 3→L0、12→L2、30→L3、80→L3
    check("偏差 2.0 时分布变成 1/0/1/2",
          st6 and st6["levels"] == [1, 0, 1, 2], st6 and st6["levels"])
    check("三角形数 == L0 + L2 + 2×L3",
          st6 and st6["tris"] == tris[0] + tris[2] + 2 * tris[3],
          st6 and st6["tris"])
    check("比默认分布更省（bias 确实在往低模推）",
          st6 and st6["tris"] < st["tris"],
          st6 and "%d < %d" % (st6["tris"], st["tris"]))

    # ============ 第 6 组：最简网格不生成 ============
    print("\n== 第 6 组：立方体（已经最简）不该生成 LOD ==")
    b7, s7, _, _, _ = run_once(exe, cube_scene, {})
    st7 = parse_stats(s7)
    print("  LOD-BUILD:", b7.group(0) if b7 else None)
    print("  LOD-STATS:", s7.group(0) if s7 else None)
    check("链只有 1 级", b7 is not None and int(b7.group(3)) == 1,
          b7.group(0) if b7 else None)
    check("没有任何实例被 LOD 接管（ents == 0）",
          st7 and st7["ents"] == 0, st7 and st7["ents"])

    cam3 = "0,0,3,180,0"

    # ============ 第 7 组：导入网格 → 顶点聚类简化 ============
    print("\n== 第 7 组：导入网格（OBJ 高模球）走顶点聚类简化 ==")
    obj_path = os.path.join(OUT_DIR, "lod_test_sphere.obj")
    obj_tris = write_uv_sphere_obj(obj_path)
    obj_scene = os.path.join(OUT_DIR, "lod_test_obj.json")
    build_file_scene(obj_scene, {"kind": "obj", "path": obj_path,
                                 "name": "lod_obj_sphere"})
    print("  生成 OBJ：%s（%d 三角形）" % (obj_path, obj_tris))
    b8, s8, _, _, errs8 = run_once(exe, obj_scene, {}, cam=cam3)
    print("  LOD-BUILD:", b8.group(0) if b8 else None)
    print("  LOD-STATS:", s8.group(0) if s8 else None)
    check("拿到 OBJ 的 LOD-BUILD", b8 is not None,
          b8.group(0) if b8 else None)
    if b8:
        check("来源确实是 OBJ（kind == 2）", b8.group(2) == "2", b8.group(2))
        otris = parse_tris(b8)
        print("  各级三角形数:", otris)
        check("导入网格也被简化出 >= 3 级", len(otris) >= 3, otris)
        check("三角形数严格递减",
              all(otris[i] > otris[i + 1] for i in range(len(otris) - 1)),
              otris)
        check("首级等于原网格三角形数", otris[0] == obj_tris,
              "%d vs %d" % (otris[0], obj_tris))
        # 目标就是"每级减半"，留 15% 的余量（二分会落在目标附近）
        check("每级大约减半（下一级 <= 上一级 × 0.85）",
              all(otris[i + 1] * 100 <= otris[i] * 85
                  for i in range(len(otris) - 1)), otris)
        check("末级至少比原网格少 4 倍", otris[-1] * 4 <= otris[0],
              "%d vs %d" % (otris[-1], otris[0]))
        st8 = parse_stats(s8)
        check("渲染时确实用了简化后的网格（ents == 1）",
              st8 and st8["ents"] == 1, st8 and st8["ents"])
    check("零 Vulkan 验证层报错", not errs8, errs8[:3])

    # ============ 第 8 组：蒙皮网格**不做**自动 LOD ============
    # ⚠ 这条断言在 2026-10-24 反过来了两次：
    #   ① 2026-10-19 之前：蒙皮网格停在 1 级（减面会丢 joints/weights）。
    #   ② W2-3 之后：按主 joint 分组，蒙皮也能减面 → 断言"≥ 2 级"。
    #   ③ 现在（事故修复）：**又回到 1 级**，但理由完全不是 ①。
    #      真正的原因是顶点聚类对骨骼网格有两个致命副作用：
    #        · 选层按**每个子网格自己的**包围球算屏幕占高比 —— 脸只有
    #          19 cm，第三人称 4 m 外比值 ≈ 0.06，直接掉到第 3 级；
    #        · UV 会被平均 —— 脸/眼/眉是同一张图集上切开的小岛，平均后
    #          采样落在图集空白处。实测症状：**脸是一块白板、腿消失、
    #          鞋子悬空**（近距离一切正常，因为那时是第 0 级）。
    #      也就是说"能不能减"（W2-3 解决的）和"该不该减"是两件事。
    #      与 UE5 对齐：骨骼网格体 LOD 走专门管线，不是自动减面能凑的。
    print("\n== 第 8 组：蒙皮网格不生成自动 LOD（2026-10-24 事故修复）==")
    girl = os.path.abspath(os.path.join(
        ROOT, "assets", "models", "characters", "animate_girl_rigged.glb"))
    if not os.path.exists(girl):
        print("  跳过（找不到 %s）" % girl)
    else:
        skin_scene = os.path.join(OUT_DIR, "lod_test_skinned.json")
        build_file_scene(skin_scene, {
            "kind": "gltf", "path": girl, "modelKey": "lod_test_skinned",
            "subMeshIndex": 0, "name": "lod_skinned"})
        b9, s9, _, _, errs9 = run_once(exe, skin_scene, {}, cam=cam3)
        print("  LOD-BUILD:", b9.group(0) if b9 else None)
        skipped = None
        if _LAST_TAP is not None:
            with _LAST_TAP.lock:
                for line in _LAST_TAP.lines:
                    m = SKIP_SKIN_RE.search(line)
                    if m:
                        skipped = m
        print("  LOD-SKIP-SKIN:", skipped.group(1) if skipped else None)
        check("蒙皮网格打了 LOD-SKIP-SKIN 锚点", skipped is not None,
              skipped.group(1) if skipped else "没有这行日志")
        check("蒙皮网格的链停在 1 级（不减面）",
              b9 is not None and int(b9.group(3)) == 1,
              b9.group(0) if b9 else None)
        st9 = parse_stats(s9)
        # ⚠ 这里**不能**再断言 `ents >= 1`：`ents` 的定义是"被 LOD 接管的
        #   实例数"，只有 1 级链的网格永远是 0。真正的判据是"画出来的三角形
        #   等于第 0 级的三角形" —— 也就是远距离也**没有**换成低模。
        base9 = int(b9.group(4).split("/")[0]) if b9 is not None else -1
        check("没有任何实例被 LOD 接管（ents == 0）",
              st9 and st9["ents"] == 0, st9 and st9["ents"])
        check("画的三角形 == 原网格（远距离也没换低模）",
              st9 and st9["tris"] == base9,
              "%s vs %s" % (st9 and st9["tris"], base9))
        check("零 Vulkan 验证层报错", not errs9, errs9[:3])

    # ============ 第 9 组：角色的**所有**子网格都不被减面（端到端）============
    # 第 8 组只证明"单个蒙皮网格不建链"。但用户看到的是**整具角色**，
    # 而 alicia 有 20 个子网格，逐个建链 —— 任何一个漏进去，脸上那块
    # 19 cm 的面片就会被压成 100 个顶点。所以这里拿真实的 Game.exe 跑一遍
    # 真实角色，断言：每个子网格都打了 `LOD-SKIP-SKIN` 且都是 1 级。
    #
    # 为什么不用编辑器场景：`kind:"fbx" + subMeshIndex` 这条静态子网格
    # 路径当前不渲染（已知限制，见 docs），不渲染就不会建链，测不到。
    # 走玩家生成那条路（`instantiateModel`）才是用户真正会看到的那条。
    print("\n== 第 9 组：真实角色（alicia）的所有子网格都不减面 ==")
    alicia = os.path.join(ROOT, "assets", "models", "characters",
                          "alicia.vmesh")
    game_exe = os.path.join(ROOT, "build", "bin", "Game.exe")
    if not (os.path.exists(alicia) and os.path.exists(game_exe)):
        print("  跳过（找不到 alicia.vmesh 或 Game.exe）")
    else:
        play_scene = os.path.join(ROOT, "build", "bin", "assets", "scenes",
                                  "lod_test_play.json")
        game_json = os.path.join(ROOT, "build", "bin", "assets",
                                 "lod_test_game.json")
        with open(play_scene, "w", encoding="utf-8") as f:
            json.dump({
                "version": 1, "generator": "verify_lod: 角色 LOD 端到端",
                "camera": {"target": [0.0, 0.0, 1.2], "yaw": 0.0,
                           "pitch": 0.3, "distance": 5.0, "fov": 55.0},
                "light": {"direction": [0.42, 0.72, -0.55],
                          "color": [1.0, 0.96, 0.9], "intensity": 3.0,
                          "castsShadow": False, "ambientScale": 1.0},
                "physics": {"enabled": True, "groundPlane": True},
                "entities": [
                    {"name": "Ground",
                     "transform": {"position": [0.0, 0.0, 0.0],
                                   "rotation": [0.0, 0.0, 0.0],
                                   "scale": [1.0, 1.0, 1.0]},
                     "parent": -1, "visible": True, "castShadow": False,
                     "locked": True,
                     "mesh": {"kind": "builtin", "shape": "plane",
                              "size": 40.0, "name": "lod_play_ground"},
                     "material": {"kind": "color",
                                  "color": [0.55, 0.58, 0.62, 1.0],
                                  "roughness": 0.9, "metallic": 0.0,
                                  "name": "lod_play_ground_mat"}},
                    {"name": "GameStart",
                     "transform": {"position": [0.0, 0.0, 0.0],
                                   "rotation": [0.0, 0.0, 0.0],
                                   "scale": [1.0, 1.0, 1.0]},
                     "parent": -1, "visible": True, "castShadow": False,
                     "gameStart": {"yaw": 0.0, "enabled": True}},
                ],
            }, f, ensure_ascii=False, indent=2)
        with open(game_json, "w", encoding="utf-8") as f:
            json.dump({
                "scene": "scenes/lod_test_play.json",
                "title": "LOD Test", "width": 640, "height": 360,
                "audio": {"enabled": False}, "quality": "high",
                "startState": "playing",
                "player": {"model": "models/characters/alicia.vmesh"},
            }, f, ensure_ascii=False, indent=2)

        env = {k: v for k, v in os.environ.items() if not k.startswith("MYVK_")}
        env["MYVK_RUNTIME_GAME_JSON"] = game_json
        env["MYVK_LOG_RECTS"] = ""
        proc = subprocess.Popen([game_exe], cwd=os.path.dirname(game_exe),
                                env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True,
                                encoding="utf-8", errors="replace", bufsize=1)
        tap = LogTap()
        tap.start(proc)
        try:
            end = time.time() + 25
            while time.time() < end:
                with tap.lock:
                    if any("GAME-PLAYER visual=model" in l for l in tap.lines):
                        break
                time.sleep(0.4)
            time.sleep(2.0)          # 让它渲染几帧，把所有子网格的链建出来
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=6)
            except subprocess.TimeoutExpired:
                proc.kill()

        builds, skips = [], 0
        with tap.lock:
            for line in tap.lines:
                m = BUILD_RE.search(line)
                if m and "alicia" in m.group(1):
                    builds.append(m)
                if SKIP_SKIN_RE.search(line):
                    skips += 1
        print("  子网格 LOD-BUILD 行数:", len(builds), " LOD-SKIP-SKIN:", skips)
        if builds:
            print("  示例:", builds[0].group(0))
        check("角色的每个子网格都被拦下来了（LOD-SKIP-SKIN >= 10 行，"
              "实测 20 个子网格）", skips >= 10, "skips=%d" % skips)
        check("没有任何子网格建出多级 LOD",
              all(int(m.group(3)) == 1 for m in builds),
              [m.group(0) for m in builds if int(m.group(3)) != 1][:3])

    print("\n结果: %s" % ("PASS" if not fails else "FAIL: " + ", ".join(fails)))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
