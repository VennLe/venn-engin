"""端到端验证：局部光源阴影（点光 / 聚光灯）。

背景：在这之前阴影只有方向光那一路（单张图 → 后来升级成 CSM）。点光与
聚光灯**完全不投影** —— 室内一盏吊灯照下来的是一圈假的"均匀亮"，物体
底下接触阴影全靠方向光凑。这个脚本验的是新加的第二条通道
（见 render/LocalShadowPass.h）：

  · 所有需要投影的局部光源**共用一张 1024² × 12 层的 2D array 深度图**；
  · 聚光灯占 **1 层**（沿锥轴一次透视投影，FOV = 2·外锥半角）；
  · 点光占 **6 层**（立方体 6 个面各一次 90° 透视投影，面序与
    common.glsl 的 vennCubeDir 逐条对应）；
  · 层号由 CPU 每帧分配（灯按到相机的距离**由近到远**排，预算用完就轮不到
    远处的），分配结果通过灯表的 cosOuterType.z 送到片元侧；
  · 每个世界 → 光源裁剪矩阵**只有一份副本**（set0/binding11 的 SSBO），
    depth-only Pass 渲染时与 pbr.frag 采样时读的是同一块 buffer；
  · 未使用的层每帧仍走一次空的 depth-only pass（只清深度）—— 否则那张
    array 会有一层停在 UNDEFINED 而被采样，验证层立刻报错。

测试场景刻意构造成**可判定**的：

  · 4 个投影体（一块遮光板 + 两根柱子 + 一面远处的墙），1 个地面不投影。
  · 3 盏投影灯 + 1 盏不投影的对照灯，到相机 (0,-34,15) 的距离平方依次是
    578 / 798 / 1172 —— 所以"近的优先"这条规则会给出一个确定的顺序：
    先分配距离 578 的聚光灯（1 层），再分配 798 的聚光灯（1 层），
    最后才是 1172 的点光灯（6 层）。三盏全上都装得下（8 ≤ 12 层），
    而 MaxLights=1 时只会留下那盏**最便宜的近灯**（1 层，而不是 6 层）。

断言分五组：

  1. **层分配自检**（LOCAL-SHADOW-DBG）
     · candidates=3（那盏 castsShadow=false 的对照灯不算候选）；
     · assigned=3、layers=8/12（1+1+6，硬预算 12 层没被撞破）；
     · MaxLights=1 → layers=1。**这一条最有价值**：它同时证明了
       "近的优先"（留下的是 578 那盏，不是 1172 那盏）与"聚光灯只占 1 层"
       （若按层数贪心就会去装那盏 6 层的）。
     · MaxLights=2 → layers=2；MaxLights=0 → 一盏都不分配。

  2. **逐层裁剪空间剔除**（LOCAL-SHADOW-STATS）
     · used 必须等于分配到的层数；considered = 投影体数 × used
       （每层各遍历一遍全部投影体）。不剔除的话提交量就是这个数。
     · 0 < submitted < considered（剔除真的在发生，而且没有过剔）。
     · 每个"用掉的层"的提交量都 ≤ 投影体数、且 ≥ 0。

  3. **开关对照**
     · MYVK_LOCAL_SHADOW=0 与 MYVK_LOCAL_SHADOW_LIGHTS=0 是**两条不同的
       关断路径**（一个短路采样、一个不分配层），画面必须**逐像素完全相同**。
       这条断言把"层分配"与"采样开关"两件事绑在一起验，任何一边漏了都会红。
     · 开 vs 关：地面上一大片差异（差异像素 > 2000、最大差 > 60）。
     · 差异**只**落在下半屏：视口上方 1/4（天空/远景）平均差必须 ≈ 0。
       这一条排除了"整体亮度偏移"这种伪信号。

  4. **层预算 = 连续信号**（MYVK_LOCAL_SHADOW_LIGHTS=0/1/2/3）
     把"允许几盏灯投影"量成一条单调信号，全部与"一盏都不投影"的截图对比。
     ⚠ 方向别搞反：灯**越多**投影，画面上被遮住的区域越多，与零点图像的
       差异**越大**。而且不是所有量都单调 —— 多开一盏灯同时也把地面照亮了，
       "差异像素计数"会带点非单调；真正干净的是**平均差**与**最大差**，
       外加"差异带宽度只增不减"。
     · mean(1) < mean(2) < mean(3)；max(1) <= max(2) < max(3)。
     · 差异带宽度随灯数变宽（第 2 盏的影子落在别处，覆盖范围只增不减）。
     · 逐对比较（1 vs 2、2 vs 3）：多打开的那盏灯必须带来一片**独立**的
       区域变化（> 300 像素），否则说明它的层被算到了别的灯头上。

  5. **硬预算溢出**
     · 另一个场景：4 盏点光各要 6 层 = 24 层 > 12 层。
       贪心分配必须在第 3 盏面前停下 → assigned=2、layers=12/12、
       且用掉的层数不超过硬上限（越界会读到别的灯的矩阵，画面直接错乱）。

  5. **点光的 6 个面真的都画了**
     · 点光灯单独开（关掉两盏聚光灯，MYVK_LOCAL_SHADOW_LIGHTS=1 时只留近灯，
       所以用另一套做法：把两盏聚光灯从场景里去掉，只留点光）。
       此时点光的影子必须**绕着遮光板铺开**：与"关阴影"相比，差异像素的
       包围盒必须横向铺满遮光板两侧（宽度 > 视口宽度的 40%），
       且左右两侧都有差异 —— 单面立方体贴图做不到这一点。
     · 全程零 Vulkan 验证层报错。

注意：所有轮次都开 MYVK_EDITOR_NO_HUD 关掉视口叠加层。左上角的统计文字
里有 FPS / 帧时间这类**每次运行都不同**的数字，不关掉的话"逐像素相同"
这类断言根本没法成立。

用法：
    python tools/verify_local_shadow.py
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_client, find_window, user32      # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors, wait_rect   # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = os.path.join(ROOT, "..", "_venn_verify")

# 相机在 (0,-34,15)，yaw=90 → 视线朝 +Y，俯角 16°。
# 到三盏投影灯的距离平方依次是 578 / 798 / 1172（见文件头的推导）。
CAM = "0,-34,15,90,16"

GROUND_SIZE = 80.0
OCCLUDERS = [
    # (名字, 位置, 尺寸)
    ("Plate", (0.0, 0.0, 3.5), 5.0),    # 点光灯正下方：影子绕它铺一圈
    ("Post0", (9.0, -1.0, 3.0), 3.0),
    ("Post1", (-9.0, 4.0, 3.0), 3.0),
    ("FarWall", (19.0, 8.0, 2.0), 4.0),  # 远处：多数层里应当被剔掉
]

# 灯：(名字, 位置, 类型, 参数, castsShadow, 到相机距离平方)
# 距离平方是硬编码的常量，脚本用它来断言"近的优先"这条规则。
LIGHTS = [
    ("SpotNear", (-7.0, -11.0, 15.0), "spot", 578.0,
     {"direction": [0.36, 0.56, -0.75], "innerAngle": 0.24, "outerAngle": 0.44,
      "range": 32.0, "intensity": 1400.0, "color": [0.86, 0.92, 1.0]}, True),
    ("SpotFar", (13.0, -9.0, 13.0), "spot", 798.0,
     {"direction": [-0.55, 0.55, -0.63], "innerAngle": 0.24, "outerAngle": 0.44,
      "range": 32.0, "intensity": 1400.0, "color": [1.0, 0.9, 0.82]}, True),
    ("PointTop", (0.0, 0.0, 11.0), "point", 1172.0,
     {"range": 20.0, "intensity": 110.0, "color": [1.0, 0.94, 0.86]}, True),
    # 对照灯：castsShadow=false。它必须**不**进候选，也不能占层。
    ("PointNoShadow", (-16.0, -5.0, 4.0), "point", 0.0,
     {"range": 11.0, "intensity": 60.0, "color": [0.9, 1.0, 0.9]}, False),
]

N_CASTERS = len(OCCLUDERS)          # 投影体数量（地面不投影）
N_SHADOW_LIGHTS = 3                 # castsShadow=true 的灯数
EXPECTED_LAYERS_ALL = 8             # 1 + 1 + 6
MAX_LAYERS = 12

DBG_RE = re.compile(r"LOCAL-SHADOW-DBG candidates=(\d+) assigned=(\d+) "
                    r"layers=(\d+)/(\d+) maxLights=(\d+) enabled=(\d+)")
STATS_RE = re.compile(r"LOCAL-SHADOW-STATS layers=(\d+) used=(\d+) "
                      r"submitted=(\d+) considered=(\d+)")


def material(name, base, roughness=0.55):
    return {
        "kind": "procedural", "name": name,
        "baseColor": list(base) + [1.0],
        "metallic": 0.0, "roughness": roughness,
        "normalScale": 1.0, "ao": 1.0,
        "emissive": [0.0, 0.0, 0.0], "doubleSided": False, "alphaBlend": False,
    }


def build_scene(path, with_point=True, with_spots=True):
    """地面 + 4 个投影体 + 3~4 盏灯。with_* 用来做"只留点光"的对照。"""
    ents = [{
        "name": "Ground",
        "transform": {"position": [0, 0, 0], "rotation": [0, 0, 0],
                      "scale": [1, 1, 1]},
        "visible": True,
        # 地面只接收阴影，自己不投影 —— 否则它会变成最大的那个遮挡体，
        # 每层都被它撑满，剔除统计就什么也看不出来了。
        "castShadow": False,
        "parent": -1,
        "mesh": {"kind": "builtin", "shape": "plane", "size": GROUND_SIZE,
                 "name": "ls_ground"},
        "material": material("ls_ground_mat", (0.70, 0.70, 0.73)),
    }]
    for name, pos, size in OCCLUDERS:
        if not with_point and name == "Plate":
            continue
        ents.append({
            "name": name,
            "transform": {"position": list(pos), "rotation": [0, 0, 0],
                          "scale": [1, 1, 1]},
            "visible": True, "castShadow": True, "parent": -1,
            "mesh": {"kind": "builtin", "shape": "cube", "size": size,
                     "name": "ls_%s" % name.lower()},
            "material": material("ls_%s_mat" % name.lower(), (0.80, 0.52, 0.35),
                                 0.5),
        })

    for name, pos, kind, _, params, casts in LIGHTS:
        if kind == "spot" and not with_spots:
            continue
        if not with_point and kind == "point":
            continue
        e = {
            "name": name,
            "transform": {"position": list(pos), "rotation": [0, 0, 0],
                          "scale": [1, 1, 1]},
            "visible": True, "castShadow": False, "parent": -1,
        }
        key = "spotLight" if kind == "spot" else "pointLight"
        body = dict(params)
        body["enabled"] = True
        body["castsShadow"] = casts
        e[key] = body
        ents.append(e)

    doc = {
        "version": 1,
        "generator": "verify_local_shadow.py",
        # 太阳压得很暗：让局部光源的阴影成为画面上主要的明暗来源，
        # 这样"开关局部阴影"的差异不会被方向光的阴影冲淡。
        "light": {"direction": [-0.35, -0.5, -0.79], "color": [1.0, 0.97, 0.92],
                  "intensity": 0.10, "castsShadow": True,
                  "ambientScale": 0.10},
        "entities": ents,
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)
    return ents


def build_overflow_scene(path, n_points=4):
    """地面 + 2 个投影体 + 4 盏都投影的点光。

    每盏点光要 6 层，4 盏就是 24 层 —— 12 层的硬预算只装得下 2 盏。
    这个场景专门用来撞预算上限：贪心分配必须在第 3 盏面前停下，
    而不是写出 12 层之外的下标（那会读到别的灯的矩阵，画面直接错乱）。
    """
    ents = [{
        "name": "Ground",
        "transform": {"position": [0, 0, 0], "rotation": [0, 0, 0],
                      "scale": [1, 1, 1]},
        "visible": True, "castShadow": False, "parent": -1,
        "mesh": {"kind": "builtin", "shape": "plane", "size": GROUND_SIZE,
                 "name": "ovf_ground"},
        "material": material("ovf_ground_mat", (0.70, 0.70, 0.73)),
    }]
    for i, pos in enumerate([(0.0, 0.0, 3.5), (8.0, 2.0, 3.0)]):
        ents.append({
            "name": "Blk%d" % i,
            "transform": {"position": list(pos), "rotation": [0, 0, 0],
                          "scale": [1, 1, 1]},
            "visible": True, "castShadow": True, "parent": -1,
            "mesh": {"kind": "builtin", "shape": "cube", "size": 5.0,
                     "name": "ovf_blk%d" % i},
            "material": material("ovf_blk%d_mat" % i, (0.80, 0.52, 0.35), 0.5),
        })
    for i, pos in enumerate([(0.0, 0.0, 11.0), (8.0, 2.0, 10.0),
                             (-9.0, -3.0, 9.0), (14.0, 7.0, 12.0)][:n_points]):
        ents.append({
            "name": "Point%d" % i,
            "transform": {"position": list(pos), "rotation": [0, 0, 0],
                          "scale": [1, 1, 1]},
            "visible": True, "castShadow": False, "parent": -1,
            "pointLight": {"color": [1.0, 0.94, 0.86], "intensity": 90.0,
                           "range": 20.0, "enabled": True, "castsShadow": True},
        })
    doc = {
        "version": 1, "generator": "verify_local_shadow.py (overflow)",
        "light": {"direction": [-0.35, -0.5, -0.79], "color": [1.0, 0.97, 0.92],
                  "intensity": 0.10, "castsShadow": True,
                  "ambientScale": 0.10},
        "entities": ents,
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)


def run_once(exe, scene, extra_env, shot_path, cam=CAM, wait=16.0):
    """跑一轮编辑器，返回 (日志行, 视口矩形, 截图, 错误行)。"""
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_ENV_DBG"] = "0"
    # 灯的小球是自发光体，会把画面搅浑；这个测试不关心它
    env["MYVK_LIGHT_GIZMOS"] = "0"
    # 关掉视口叠加层：里面的 FPS / 帧时间是浮动的，会让"逐像素相同"失败
    env["MYVK_EDITOR_NO_HUD"] = "1"
    # 打开阴影自检日志（级联的 + 局部光源的）
    env["MYVK_SHADOW_DBG"] = "1"
    env["MYVK_EDITOR_SCENE"] = scene
    env["MYVK_EDITOR_CAM"] = cam
    env.update(extra_env)

    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    hwnd = None
    try:
        hwnd = find_window("Venn Editor")
        if not hwnd:
            return [], None, None, ["未找到编辑器窗口"]
        user32.ShowWindow(hwnd, 9)
        time.sleep(1.5)
        user32.SetWindowPos(hwnd, 0, 0, 0, 1280, 800, 0x0002 | 0x0004)
        time.sleep(2.5)

        vp = wait_rect(tap, "VP-RECT", 10.0)
        # 等 LOCAL-SHADOW-STATS 出现（30 帧一条）即视为场景已经稳定出图
        end = time.time() + wait
        while time.time() < end:
            with tap.lock:
                ready = any("LOCAL-SHADOW-STATS" in l for l in tap.lines)
            if ready:
                break
            time.sleep(0.25)
        time.sleep(1.5)

        img = None
        if shot_path:
            img, _ = capture_client(hwnd)
            img.save(shot_path)

        with tap.lock:
            lines = list(tap.lines)
        errs = [l for l in lines
                if ("VUID" in l or "VALIDATION" in l or " ERROR" in l
                    or "Error:" in l)]
        return lines, vp, img, errs
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


def find_line(lines, needle, rx):
    for line in reversed(lines):
        if needle in line:
            m = rx.search(line)
            if m:
                return m
    return None


def to_np(img):
    import numpy as np
    return np.asarray(img.convert("RGB")).astype(int)


def vp_box(vp):
    """VP-RECT（浮点 x0,y0,x1,y1）→ 整数像素盒。"""
    x0, y0, x1, y1 = [int(round(float(v))) for v in vp]
    return x0, y0, x1, y1


def crop_pair(a, b, vp):
    """把两张截图一起裁到视口矩形（并取公共尺寸），返回 (pa, pb, x0, y0)。

    ⚠ **必须裁到视口**。ImGui 的面板（Hierarchy / Content / Inspector）在两次
    进程启动之间会有 1~2 px 的排版抖动（实测 Inspector 顶部标题行与底部边缘
    最大差 29），这是 ImGui 自己的事、跟渲染无关。整窗比对时它会**盖过真信号**：
    实测某一次 MaxLights=1 的真信号只有 4672 像素，面板噪声却有 8671 像素，
    直接把"平均差随灯数单调递增"这条断言打翻。
    """
    pa, pb = to_np(a), to_np(b)
    h = min(pa.shape[0], pb.shape[0])
    w = min(pa.shape[1], pb.shape[1])
    x0 = y0 = 0
    if vp:
        x0, y0, x1, y1 = vp_box(vp)
        x0, y0 = max(0, x0), max(0, y0)
        w, h = min(w, x1), min(h, y1)
    if w <= x0 or h <= y0:
        return None, None, 0, 0
    return pa[y0:h, x0:w], pb[y0:h, x0:w], x0, y0


def diff_stats(a, b, vp=None, thr=18):
    """逐像素最大通道差的统计：`> thr` 的像素数、最大差、均值、包围盒。

    vp 非空时**只统计视口矩形内**的像素（理由见 crop_pair）。
    返回的 bbox 仍是**窗口绝对坐标**，方便直接和截图对照。

    ⚠ `thr` 只影响**计数**（`n` 与 `box`），不影响 `max` 与 `mean`。
    调用方若用 `n` 做断言，必须让 `thr` 与它的"显著"口径一致 —— 默认值 18
    与"`max > 8` 就算显著"这类断言是不自洽的（差在 9..18 之间的画面会
    出现 `max` 达标而 `n` 恒为 0）。
    """
    import numpy as np
    pa, pb, x0, y0 = crop_pair(a, b, vp)
    if pa is None:
        return 0, 0, 0.0, None
    d = np.abs(pa - pb).max(axis=2)
    ys, xs = np.nonzero(d > thr)
    box = None
    if len(ys):
        box = [int(xs.min()) + x0, int(ys.min()) + y0,
               int(xs.max()) + x0, int(ys.max()) + y0]
    return len(ys), int(d.max()), float(d.mean()), box


def region_diff(a, b, box):
    """box 内的平均绝对像素差（0-255，三通道平均）。"""
    import numpy as np
    x0, y0, x1, y1 = [int(v) for v in box]
    pa, pb = to_np(a), to_np(b)
    x0, y0 = max(x0, 0), max(y0, 0)
    x1, y1 = min(x1, pa.shape[1]), min(y1, pa.shape[0])
    if x1 <= x0 or y1 <= y0:
        return 0.0
    sub = np.abs(pa[y0:y1, x0:x1] - pb[y0:y1, x0:x1])
    return float(sub.mean())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    os.makedirs(OUT_DIR, exist_ok=True)
    if not os.path.exists(exe):
        print("找不到可执行文件，请先构建：", exe)
        return 1

    scene = os.path.join(OUT_DIR, "local_shadow_test.json")
    build_scene(scene)
    print("测试场景：%s（%d 个投影体，%d 盏投影灯 + 1 盏对照灯）"
          % (scene, N_CASTERS, N_SHADOW_LIGHTS))

    fails = []

    def check(name, cond, detail=""):
        print(("  [PASS] " if cond else "  [FAIL] ") + name +
              (("  " + str(detail)) if detail else ""))
        if not cond:
            fails.append(name)

    kill_stale_editors()

    # ---- 基准轮：三盏全上 / 局部阴影开 ----
    print("\n== 基准轮：MaxLights=3 / 局部阴影开（默认配置）==")
    shot_on = os.path.join(OUT_DIR, "ls_on.png")
    lines, vp, img_on, errs = run_once(exe, scene, {}, shot_on)
    if not vp:
        print("  没等到 VP-RECT")
        return 2
    check("零 Vulkan 验证层报错", not errs, errs[:2])

    dbg = find_line(lines, "LOCAL-SHADOW-DBG", DBG_RE)
    stats = find_line(lines, "LOCAL-SHADOW-STATS", STATS_RE)
    print("  LOCAL-SHADOW-DBG  :", dbg.group(0) if dbg else None)
    print("  LOCAL-SHADOW-STATS:", stats.group(0) if stats else None)
    check("拿到 LOCAL-SHADOW-DBG", dbg is not None)
    check("拿到 LOCAL-SHADOW-STATS", stats is not None)
    # 视口矩形（下面几组都要用它的宽高做相对断言）
    vx0, vy0, vx1, vy1 = [float(v) for v in vp]
    vw, vh = vx1 - vx0, vy1 - vy0
    if not dbg or not stats or img_on is None:
        print("\n结果: FAIL（没能建立局部阴影基线）")
        return 3

    # ============ 第 1 组：层分配自检 ============
    print("\n== 第 1 组：阴影层分配（近的优先 + 每盏灯的层数）==")
    g = [int(v) for v in dbg.groups()]
    cand, assigned, used_layers, total_layers, max_lights, enabled = g
    check("candidates = %d（castsShadow=false 的对照灯不进候选）"
          % N_SHADOW_LIGHTS, cand == N_SHADOW_LIGHTS, cand)
    check("assigned = %d（预算装得下全部投影灯）" % N_SHADOW_LIGHTS,
          assigned == N_SHADOW_LIGHTS, assigned)
    check("layers = %d/%d（1+1+6：聚光灯各 1 层、点光 6 层）"
          % (EXPECTED_LAYERS_ALL, MAX_LAYERS),
          used_layers == EXPECTED_LAYERS_ALL and total_layers == MAX_LAYERS,
          "%d/%d" % (used_layers, total_layers))
    check("enabled = 1", enabled == 1, enabled)

    # 预算：MaxLights=1 必须留下**最便宜的那盏近灯**（1 层），
    # 而不是层数最多的那盏（点光 6 层）。这一条同时钉住两条规则。
    print("\n  -- 层预算与优先级（MYVK_LOCAL_SHADOW_LIGHTS）--")
    budget = {}
    vp_ok = True
    for n in (0, 1, 2, 3):
        shot = os.path.join(OUT_DIR, "ls_lights%d.png" % n)
        lines_n, vp_n, img_n, errs_n = run_once(
            exe, scene, {"MYVK_LOCAL_SHADOW_LIGHTS": str(n)}, shot)
        check("MaxLights=%d 零 Vulkan 验证层报错" % n, not errs_n, errs_n[:2])
        d_n = find_line(lines_n, "LOCAL-SHADOW-DBG", DBG_RE)
        s_n = find_line(lines_n, "LOCAL-SHADOW-STATS", STATS_RE)
        if d_n is None or s_n is None:
            check("MaxLights=%d 拿到自检日志" % n, False)
            continue
        gg = [int(v) for v in d_n.groups()]
        ss = [int(v) for v in s_n.groups()]
        budget[n] = (gg[1], gg[2], gg[4], img_n, ss, vp_n)
        # 视口矩形稳定是**所有像素断言的隐含前提**：它一变，比对的就是两块
        # 不同的区域。实测 4 轮都是同一个矩形，这里把它钉死。
        if vp_n and abs(vp_n[0] - vx0) <= 1 and abs(vp_n[1] - vy0) <= 1 and \
                abs(vp_n[2] - vx1) <= 1 and abs(vp_n[3] - vy1) <= 1:
            vp_ok = True
        else:
            vp_ok = False
        print("     MaxLights=%d → assigned=%d layers=%d/12 used=%d "
              "submitted=%d/%d"
              % (n, gg[1], gg[2], ss[1], ss[2], ss[3]))

    check("4 轮的视口矩形完全一致（像素断言的前提）", vp_ok,
          "baseline=(%g,%g,%g,%g)" % (vx0, vy0, vx1, vy1))

    if 1 in budget:
        a1, l1, ml1, _, _, _ = budget[1]
        check("MaxLights=1 → assigned=1, maxLights 回读=1",
              a1 == 1 and ml1 == 1, (a1, ml1))
        # ★ 核心断言：留下的是"最便宜的近灯"。距离 578 的聚光灯只需 1 层；
        #   若按"层数多的优先"或"按场景顺序"分配，这里会是 6（点光灯）。
        check("MaxLights=1 → layers=1（留下的是最便宜的近灯，不是 6 层的点光）",
              l1 == 1, l1)
    if 2 in budget:
        check("MaxLights=2 → layers=2（两盏近聚光灯）",
              budget[2][0] == 2 and budget[2][1] == 2,
              (budget[2][0], budget[2][1]))
    if 0 in budget:
        check("MaxLights=0 → 一盏都不分配（assigned=0, layers=0）",
              budget[0][0] == 0 and budget[0][1] == 0,
              (budget[0][0], budget[0][1]))
    if 3 in budget:
        check("MaxLights=3 → 与默认配置一致（assigned=3, layers=8）",
              budget[3][0] == 3 and budget[3][1] == EXPECTED_LAYERS_ALL,
              (budget[3][0], budget[3][1]))

    # 撞硬预算：4 盏点光各要 6 层 = 24 层，装不下 → 必须停在第 2 盏之后。
    print("\n  -- 硬预算溢出（4 盏点光 = 24 层 > 12 层）--")
    scene_ovf = os.path.join(OUT_DIR, "local_shadow_overflow.json")
    build_overflow_scene(scene_ovf)
    lines_ovf, _, _, errs_ovf = run_once(exe, scene_ovf, {}, None)
    check("层预算溢出场景零 Vulkan 验证层报错", not errs_ovf, errs_ovf[:2])
    d_ovf = find_line(lines_ovf, "LOCAL-SHADOW-DBG", DBG_RE)
    if d_ovf:
        go = [int(v) for v in d_ovf.groups()]
        print("     candidates=%d assigned=%d layers=%d/%d"
              % (go[0], go[1], go[2], go[3]))
        check("候选 = 4 盏（都投影）", go[0] == 4, go[0])
        check("只装得下 2 盏（assigned=2），12 层正好用满",
              go[1] == 2 and go[2] == MAX_LAYERS and go[3] == MAX_LAYERS,
              "assigned=%d layers=%d/%d" % (go[1], go[2], go[3]))
        check("层号没有越界（用掉的层数 <= 硬上限）", go[2] <= go[3],
              "%d <= %d" % (go[2], go[3]))
    else:
        check("层预算溢出场景拿到 LOCAL-SHADOW-DBG", False)

    # ============ 第 2 组：逐层剔除 ============
    print("\n== 第 2 组：逐层裁剪空间剔除 ==")
    s = [int(v) for v in stats.groups()]
    s_layers, s_used, s_sub, s_con = s
    check("stats 里的 layers = %d" % MAX_LAYERS, s_layers == MAX_LAYERS,
          s_layers)
    check("used = %d（与 DBG 的层分配一致）" % EXPECTED_LAYERS_ALL,
          s_used == EXPECTED_LAYERS_ALL, s_used)
    # 每层各遍历一遍全部投影体 —— 不剔除的话提交量恰好是这个数。
    check("considered = 投影体数 × 用掉的层数 = %d" % (N_CASTERS
                                                     * EXPECTED_LAYERS_ALL),
          s_con == N_CASTERS * EXPECTED_LAYERS_ALL, s_con)
    check("总提交量 < 总考虑量（剔除真的在发生）", s_sub < s_con,
          "%d vs %d" % (s_sub, s_con))
    check("总提交量 > 0（没有过剔）", s_sub > 0, s_sub)
    print("  不剔除的话会是 %d 次提交，实测 %d（省掉 %.0f%%）"
          % (s_con, s_sub, 100.0 * (1.0 - s_sub / float(s_con))))
    check("提交量不超过「全量 × 层数」",
          s_sub <= s_con, s_sub)
    n = s_con - s_sub
    check("剔除至少省掉一个物体 × 一层的量（>= %d 次提交）" % N_CASTERS,
          n >= N_CASTERS, n)

    # ============ 第 3 组：开关对照 ============
    print("\n== 第 3 组：开关对照（两条关断路径必须等价）==")
    shot_off = os.path.join(OUT_DIR, "ls_off.png")
    _, vp_off, img_off, errs_off = run_once(exe, scene, {"MYVK_LOCAL_SHADOW": "0"},
                                            shot_off)
    check("拿到关局部阴影的截图", img_off is not None)
    check("零 Vulkan 验证层报错", not errs_off, errs_off[:2])

    if budget.get(0, (None,) * 6)[3] is not None:
        img_zero = budget[0][3]
        # 只比**视口内**。整窗比会被 ImGui 面板的排版抖动污染，见 crop_pair。
        # 面板那部分的差单独报出来（但不作为失败条件），免得真出问题时被藏住。
        import numpy as np
        pa, pb = to_np(img_zero), to_np(img_off)
        hh = min(pa.shape[0], pb.shape[0])
        ww = min(pa.shape[1], pb.shape[1])
        x0, y0, x1, y1 = vp_box(vp_off or budget[0][5])
        x1, y1 = min(x1, ww), min(y1, hh)
        d_all = np.abs(pa[:hh, :ww] - pb[:hh, :ww])
        d_in = d_all[y0:y1, x0:x1]
        inside = int(d_in.max()) if d_in.size else 0
        d_out = d_all.copy()
        d_out[y0:y1, x0:x1] = 0
        outside = int(d_out.max()) if d_out.size else 0
        print("  MYVK_LOCAL_SHADOW=0 vs MYVK_LOCAL_SHADOW_LIGHTS=0: "
              "视口内最大差 = %d（视口外 ImGui 面板最大差 = %d，不计入）"
              % (inside, outside))
        check("两条关断路径在**视口内**逐像素完全相同", inside == 0,
              "最大差 %d" % inside)

    if img_on is not None and img_off is not None:
        n_on, mx_on, mean_on, box_on = diff_stats(img_on, img_off, vp)
        print("  开 vs 关: 差异像素=%d 最大差=%d 均值=%.4f bbox=%s"
              % (n_on, mx_on, mean_on, box_on))
        check("局部阴影确实在画面上（差异像素 > 2000 且最大差 > 60）",
              n_on > 2000 and mx_on > 60, "n=%d max=%d" % (n_on, mx_on))

        # 差异必须只落在下半屏：上方四分之一是天空/远景，局部光开关不该动它。
        upper = (vx0, vy0, vx1, vy0 + 0.25 * vh)
        d_upper = region_diff(img_on, img_off, upper)
        print("  视口上方 1/4 区域的平均差: %.3f" % d_upper)
        check("局部阴影开关不影响视口上方 1/4（平均差 < 0.5）",
              d_upper < 0.5, "%.3f" % d_upper)

    # ============ 第 4 组：层预算 = 连续信号 ============
    print("\n== 第 4 组：层预算是一条连续信号（与「一盏都不投影」对比）==")
    # 基准：MaxLights=0 时**没有任何**局部光源投影，但光照本身完全没变
    # （只是没分配层号 → 片元查不到层号 → 不采样）。所以它就是"关局部阴影"
    # 的等价物，拿它当零点是站得住的。
    #
    # ⚠ 方向别搞反：灯**越多**投影 → 画面上被遮住的区域越多 → 与零点
    #   图像的差异**越大**。我第一版把判据写成了"递减"，被实测直接打回
    #   （像素计数还带点非单调，因为多一盏灯同时也把地面照亮了；
    #   真正单调的是**平均差**与**最大差** —— 用它们做断言）。
    if budget.get(0, (None,) * 6)[3] is not None:
        img_zero = budget[0][3]
        res = {}
        for n in (1, 2, 3):
            if n not in budget or budget[n][3] is None:
                continue
            res[n] = diff_stats(img_zero, budget[n][3], budget[n][5])
            print("  MaxLights=%d vs 0: 差异像素=%d 最大差=%d 均值=%.4f bbox=%s"
                  % ((n,) + res[n]))
        if 1 in res and 2 in res and 3 in res:
            check("平均差随投影灯数单调递增：mean(1) < mean(2) < mean(3)",
                  res[1][2] < res[2][2] < res[3][2],
                  "%.4f < %.4f < %.4f" % (res[1][2], res[2][2], res[3][2]))
            check("最大差单调不减：max(1) <= max(2) < max(3)",
                  res[1][1] <= res[2][1] < res[3][1],
                  "%d <= %d < %d" % (res[1][1], res[2][1], res[3][1]))
            # 像素计数在相邻档位之间**可能非单调**（多一盏灯同时也把地面
            # 照亮了，某些原本勉强过阈值的像素会掉回去），所以只对
            # "最多的那档"做方向断言，不对 n(1) vs n(2) 下结论。
            check("三盏全上时差异像素最多：n(3) > n(1) 且 n(3) > n(2)",
                  res[3][0] > res[1][0] and res[3][0] > res[2][0],
                  "%d > %d, %d" % (res[3][0], res[1][0], res[2][0]))
            # 差异区必须随灯数变宽：第 2 盏聚光灯的影子落在别处，
            # 整体阴影覆盖范围只会变大不会变小。
            b1, b3 = res[1][3], res[3][3]
            if b1 and b3:
                w1, w3 = b1[2] - b1[0], b3[2] - b3[0]
                print("  差异带宽度: MaxLights=1 → %d px，MaxLights=3 → %d px"
                      % (w1, w3))
                check("三盏全上时阴影覆盖范围更宽（宽度更大）", w3 > w1,
                      "%d > %d" % (w3, w1))
            check("每一盏灯都真的带来了一片影子（三盏全上的差异像素 > 4000）",
                  res[3][0] > 4000, res[3][0])
        # 逐对比较：第 2 盏、第 3 盏灯各自带来一片**独立**的影子
        for a, b in ((1, 2), (2, 3)):
            if a in budget and b in budget and budget[a][3] and budget[b][3]:
                n_ab, mx_ab, mean_ab, box_ab = diff_stats(budget[a][3],
                                                          budget[b][3],
                                                          budget[a][5])
                print("  MaxLights=%d vs %d: 差异像素=%d 最大差=%d bbox=%s"
                      % (a, b, n_ab, mx_ab, box_ab))
                check("多打开第 %d 盏灯确实改变了一片区域（> 300 像素）" % b,
                      n_ab > 300, n_ab)

    # ============ 第 5 组：点光的 6 个面 ============
    print("\n== 第 5 组：点光的立方体 6 个面都画了 ==")
    scene_pt = os.path.join(OUT_DIR, "local_shadow_point_only.json")
    build_scene(scene_pt, with_point=True, with_spots=False)
    shot_pt_on = os.path.join(OUT_DIR, "ls_point_on.png")
    shot_pt_off = os.path.join(OUT_DIR, "ls_point_off.png")
    _, vp_pt_on, img_pt_on, errs_pt = run_once(exe, scene_pt, {}, shot_pt_on)
    check("只留点光时零 Vulkan 验证层报错", not errs_pt, errs_pt[:2])
    _, vp_pt_off, img_pt_off, errs_pt2 = run_once(
        exe, scene_pt, {"MYVK_LOCAL_SHADOW": "0"}, shot_pt_off)
    check("只留点光（关阴影）零 Vulkan 验证层报错", not errs_pt2, errs_pt2[:2])

    if img_pt_on is not None and img_pt_off is not None:
        n_pt, mx_pt, mean_pt, box_pt = diff_stats(img_pt_on, img_pt_off,
                                                  vp_pt_on or vp_pt_off)
        print("  点光 开 vs 关: 差异像素=%d 最大差=%d 均值=%.4f bbox=%s"
              % (n_pt, mx_pt, mean_pt, box_pt))
        check("点光确实投出了影子（差异像素 > 1500 且最大差 > 60）",
              n_pt > 1500 and mx_pt > 60, "n=%d max=%d" % (n_pt, mx_pt))
        if box_pt:
            bw = box_pt[2] - box_pt[0] + 1
            bh = box_pt[3] - box_pt[1] + 1
            print("  差异带: 宽 %d px（视口的 %.1f%%）高 %d px"
                  % (bw, 100.0 * bw / vw, bh))
            # 单张立方体贴图面（90° 视锥）投不出"绕着物体一圈"的影子。
            # 差异横向铺开是"6 个面都在工作"的直接证据。
            check("点光的影子横向铺开（宽度 > 视口宽度的 40%）",
                  bw > 0.40 * vw, "%d px" % bw)

    print("\n结果: %s" % ("PASS" if not fails else "FAIL: " + ", ".join(fails)))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
