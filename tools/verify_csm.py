"""端到端验证：方向光级联阴影（CSM）+ 阴影距离分级。

背景：在这之前只有一张 2048² 的方向光阴影图，没有级联、没有距离分级。
这个脚本验的是替换后的整条链路（见 render/ShadowPass.h）：

  · 视锥切片按 PSSM practical scheme 切分（log/uniform 插值，λ=0.6）；
  · 每级一个**视锥切片的包围球**（不是投影体的世界 AABB —— 半径与光方向
    无关，光一转阴影边缘不会"呼吸"），球心在光空间做**纹素量化**消闪烁；
  · 每级在自己的裁剪空间做 AABB 剔除（正交投影 w≡1，退化成 6 次区间比较）；
  · 片元里按视深度选级联，超出最大距离彻底无阴影 + smoothstep 淡出。

测试场景刻意构造成**可判定**的：
  · 300 单位地面（castShadow=false）+ 沿 +X 排开的 5 根 6³ 柱子。
  · 柱子全部位于 y=0 / z=3，而相机 (40,60,22) 的视线方向是 −Y、俯角 22°
    —— 也就是说**5 根柱子到相机的视线深度完全相同（≈62.7m）**。
    这一点同时带来一个好处和一个限制：
      + 好处：5 根柱子必然落在同一级里，各级的"剔除后提交量"只由**光空间
        包围盒的大小**决定 —— 远级的盒大、装得下全部 5 根；近级的盒小、
        一根都装不下。于是 `SHADOW-STATS` 的 0/0/2/5 是一个很干净的读数。
      - 限制：4 级和 1 级在这里看到的遮挡体在同一距离上，远级的包围球
        半径几乎一样（65m vs 74m），所以**截图上的差异必然很小**。
        真正的级联收益要靠"近处有遮挡体"才看得出来。因此级联数这一项
        主要靠**数值**断言（分割点 / 级联数 / 每级提交量），截图只做一个
        "确实换了一套阴影图"的弱烟雾检查。

断言分五组：

  1. **数值自检**（SHADOW-DBG）
     · cascades=4、dist=90、λ=0.60、enabled=1。
     · 分割点严格递增、最后一级等于 dist。
     · **分割点必须逐项等于解析公式**：splits[i] = λ·near·(D/near)^(i/N)
       + (1−λ)·(near + (D−near)·i/N)，其中 D = min(相机远平面, 距离上限)。
       这是最容易"看着没糊其实糊了"的地方，有解析解就必须对着算。

  2. **每级剔除**（SHADOW-STATS）
     · considered 必须精确等于 投影体数 × 级联数（每级各遍历一遍全部
       投影体）。不剔除的话提交量就是这个数。
     · 0 < submitted < considered（剔除真的在发生，而且没有过剔）。
     · 各级 submitted 单调不减（级越大 → 盒越大 → 装进去的越多）。
     · 第 0 级（0.1–9.36m 的视锥切片）提交量必须是 0 —— 那一段包围球里
       一根柱子都没有。

  3. **级联数切换 = 退化成旧单张图**（MYVK_SHADOW_CASCADES=1）
     · cascades=1，四个分割点全等于 dist（一级吃满全程）。
     · considered 从 20 掉到 5（少遍历 3 遍）。
     · c0 = 5/5：一级的盒大到能装下全部 5 根。
     · 与 4 级的截图之间**存在**可测差异（弱断言，见上面的说明）。

  4. **阴影距离分级**（MYVK_SHADOW_DIST）
     用三个阈值把"距离分级"量成一条连续信号，全部与 MYVK_SHADOW=0 的
     截图对比：
     · dist=90（默认）：阴影完整，差异像素 > 1000、最大差 ≈ 80。
     · dist=70：柱子处在淡出区间内 → **阴影被削弱但没消失**，
       差异像素显著少于 90、最大差显著小于 80。这一条是关键：它把
       "距离分级"和"全有全无的开关"区分开。
     · dist=25：所有遮挡体都在 25m 之外 → 画面必须与"关阴影"
       **逐像素完全相同**（差异像素 = 0）。这是最强的一条断言。
     · 三个阈值下差异像素数必须单调递减：n(90) > n(70) > n(25)=0。
     · 顺带在 dist=70 上再对一次分割点解析公式（第二组参数下的独立校验）。

  5. **开关对照 + 变化定位**
     · MYVK_SHADOW=0 时阴影带出现显著差异（印证"阴影真的画上去了"）。
     · 差异必须**只**落在地面那条窄带上：视口上方 1/4 区域差必须 ≈ 0，
       差异带的竖直高度必须远小于视口高度。这一条排除了"整体亮度偏移"
       这种伪信号（整体偏移会让画面上方也跟着变）。
     · 全程零 Vulkan 验证层报错。

注意：所有轮次都开 MYVK_EDITOR_NO_HUD 关掉视口叠加层。左上角的统计文字
里有 FPS / 帧时间这类**每次运行都不同**的数字，不关掉的话"逐像素相同"
这类断言根本没法成立。

用法：
    python tools/verify_csm.py
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

# 相机在 (40,60,22)，yaw=270 → 视线朝 −Y；俯角 22°。
# 5 根柱子都在 y=0 → 视线深度全部相同，各级的提交量只由光空间盒的大小决定。
CAM = "40,60,22,270,22"

# 柱子：沿 +X 排开，间隔越来越大（近处密、远处疏）——
# 远级的盒必须"长到"75m 才装得下最后一根，近级的盒连第一根都够不着。
PILLAR_X = [6.0, 16.0, 30.0, 50.0, 75.0]
PILLAR_SIZE = 6.0
GROUND_SIZE = 300.0
# 光：从 (+X, +Y, +Z) 方向来，仰角约 58° —— 影子短而实，落在柱子附近。
LIGHT_DIR = [-0.45, -0.28, -0.85]
AMBIENT_SCALE = 0.30

# 相机默认值（engine/scene/Camera.h）。Viper 之外没有别的来源，
# 解析分割点时必须用同一组数，否则断言会毫无理由地偏掉。
CAM_NEAR = 0.1
CAM_FAR = 200.0

DBG_RE = re.compile(r"SHADOW-DBG cascades=(\d+) dist=([\d.]+) "
                    r"lambda=([\d.]+) splits=([\d.]+)/([\d.]+)/([\d.]+)/([\d.]+) "
                    r"bias=\(([\d.]+),([\d.]+),([\d.]+),([\d.]+)\) enabled=(\d+)")
STATS_RE = re.compile(r"SHADOW-STATS cascades=(\d+) submitted=(\d+) "
                      r"considered=(\d+) c0=(\d+)/(\d+) c1=(\d+)/(\d+) "
                      r"c2=(\d+)/(\d+) c3=(\d+)/(\d+)")


def material(name, base, roughness=0.55):
    return {
        "kind": "procedural", "name": name,
        "baseColor": list(base) + [1.0],
        "metallic": 0.0, "roughness": roughness,
        "normalScale": 1.0, "ao": 1.0,
        "emissive": [0.0, 0.0, 0.0], "doubleSided": False, "alphaBlend": False,
    }


def build_scene(path):
    """地面 + 沿 +X 的 5 根柱子。返回投影体数量。"""
    ents = [{
        "name": "Ground",
        "transform": {"position": [0, 0, 0], "rotation": [0, 0, 0],
                      "scale": [1, 1, 1]},
        "visible": True,
        # 地面只接收阴影，自己不投影 —— 否则它会变成最大的那个遮挡体
        "castShadow": False,
        "parent": -1,
        "mesh": {"kind": "builtin", "shape": "plane", "size": GROUND_SIZE,
                 "name": "csm_ground"},
        "material": material("csm_ground_mat", (0.72, 0.72, 0.74)),
    }]
    for i, x in enumerate(PILLAR_X):
        ents.append({
            "name": "Pillar%d" % i,
            "transform": {"position": [x, 0.0, PILLAR_SIZE * 0.5],
                          "rotation": [0, 0, 0], "scale": [1, 1, 1]},
            "visible": True, "castShadow": True, "parent": -1,
            "mesh": {"kind": "builtin", "shape": "cube", "size": PILLAR_SIZE,
                     "name": "csm_pillar"},
            "material": material("csm_pillar_mat", (0.80, 0.55, 0.35), 0.5),
        })
    doc = {
        "version": 1,
        "generator": "verify_csm.py",
        "light": {"direction": LIGHT_DIR, "color": [1.0, 0.97, 0.92],
                  "intensity": 1.0, "castsShadow": True,
                  "ambientScale": AMBIENT_SCALE},
        "entities": ents,
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)
    return len(PILLAR_X)


def practical_splits(near, far, max_distance, lam, n):
    """ShadowPass::updateCascades 里那套 PSSM practical scheme 的 Python 复刻。

    逐项对着算是有意义的：这个公式的输入（near/far/λ/级数）任何一处接错
    ——比如把 near/far 写成硬编码常量、或者把 upper bound 用成相机远平面
    而不是 min(远平面, 距离上限)——都会让近级精度悄悄掉一个量级，
    而在截图上几乎看不出来。只有公式对公式才能抓到。
    """
    d = min(max(max_distance, 1.0), far if far > 1.0 else 1.0)
    nf = near if near > 1e-4 else 1e-4
    out = []
    for i in range(1, n + 1):
        f = float(i) / float(n)
        log_s = nf * (d / nf) ** f
        uni_s = nf + (d - nf) * f
        out.append(lam * log_s + (1.0 - lam) * uni_s)
    return d, out


def run_once(exe, scene, extra_env, shot_path, wait=14.0):
    """跑一轮编辑器，返回 (日志行, 视口矩形, 截图, 错误行)。"""
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_ENV_DBG"] = "0"
    # 灯的小球是自发光体，会把画面搅浑；这个测试不关心它
    env["MYVK_LIGHT_GIZMOS"] = "0"
    # 关掉视口叠加层：里面的 FPS / 帧时间是浮动的，会让"逐像素相同"失败
    env["MYVK_EDITOR_NO_HUD"] = "1"
    # 打开阴影自检日志（分割点 + 每级提交量）
    env["MYVK_SHADOW_DBG"] = "1"
    env["MYVK_EDITOR_SCENE"] = scene
    env["MYVK_EDITOR_CAM"] = CAM
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
        # 等 SHADOW-STATS 出现（30 帧一条）即视为场景已经稳定出图
        end = time.time() + wait
        while time.time() < end:
            with tap.lock:
                ready = any("SHADOW-STATS" in l for l in tap.lines)
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


def diff_stats(a, b):
    """逐像素最大通道差的统计：>20 的像素数、最大差、均值、包围盒。"""
    pa, pb = a.load(), b.load()
    W, H = a.size
    box = [10 ** 9, 10 ** 9, -1, -1]
    n = 0
    mx = 0
    total = 0
    for y in range(H):
        for x in range(W):
            p, q = pa[x, y], pb[x, y]
            d = max(abs(p[0] - q[0]), abs(p[1] - q[1]), abs(p[2] - q[2]))
            total += d
            if d > 20:
                n += 1
                mx = max(mx, d)
                box[0] = min(box[0], x)
                box[1] = min(box[1], y)
                box[2] = max(box[2], x)
                box[3] = max(box[3], y)
    mean = total / float(W * H)
    return n, mx, mean, (box if n else None)


def region_diff(a, b, box):
    """box 内的平均绝对像素差（0-255）。"""
    x0, y0, x1, y1 = [int(v) for v in box]
    W, H = a.size
    x0, y0 = max(x0, 0), max(y0, 0)
    x1, y1 = min(x1, W), min(y1, H)
    pa, pb = a.load(), b.load()
    total, n = 0, 0
    for y in range(y0, y1, 2):
        for x in range(x0, x1, 2):
            p, q = pa[x, y], pb[x, y]
            total += abs(p[0] - q[0]) + abs(p[1] - q[1]) + abs(p[2] - q[2])
            n += 1
    return total / max(n, 1) / 3.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    os.makedirs(OUT_DIR, exist_ok=True)
    if not os.path.exists(exe):
        print("找不到可执行文件，请先构建：", exe)
        return 1

    scene = os.path.join(OUT_DIR, "csm_test.json")
    n_casters = build_scene(scene)
    print("测试场景：%s（%d 个投影体 + 1 个地面）" % (scene, n_casters))

    fails = []

    def check(name, cond, detail=""):
        print(("  [PASS] " if cond else "  [FAIL] ") + name +
              (("  " + str(detail)) if detail else ""))
        if not cond:
            fails.append(name)

    kill_stale_editors()

    # ---- 基准轮：默认配置 ----
    print("\n== 基准轮：4 级级联 / dist=90 / 阴影开 ==")
    shot_on = os.path.join(OUT_DIR, "csm_on.png")
    lines, vp, img_on, errs = run_once(exe, scene, {}, shot_on)
    if not vp:
        print("  没等到 VP-RECT")
        return 2
    check("零 Vulkan 验证层报错", not errs, errs[:2])

    dbg = find_line(lines, "SHADOW-DBG", DBG_RE)
    stats = find_line(lines, "SHADOW-STATS", STATS_RE)
    print("  SHADOW-DBG  :", dbg.group(0) if dbg else None)
    print("  SHADOW-STATS:", stats.group(0) if stats else None)
    check("拿到 SHADOW-DBG", dbg is not None)
    check("拿到 SHADOW-STATS", stats is not None)
    if not dbg or not stats or img_on is None:
        print("\n结果: FAIL（没能建立阴影基线）")
        return 3

    # ============ 第 1 组：数值自检 ============
    print("\n== 第 1 组：级联分割的数值自检 ==")
    check("cascades = 4", dbg.group(1) == "4", dbg.group(1))
    check("dist = 90.0", abs(float(dbg.group(2)) - 90.0) < 1e-3, dbg.group(2))
    check("lambda = 0.60", abs(float(dbg.group(3)) - 0.60) < 1e-3,
          dbg.group(3))
    check("enabled = 1", dbg.group(12) == "1", dbg.group(12))

    splits = [float(dbg.group(4 + i)) for i in range(4)]
    print("  分割点(实测): %.2f / %.2f / %.2f / %.2f" % tuple(splits))
    check("分割点严格递增",
          splits[0] < splits[1] < splits[2] < splits[3],
          " < ".join("%.2f" % v for v in splits))
    check("最后一级 = dist（远级吃到距离上限）",
          abs(splits[3] - float(dbg.group(2))) < 0.02, "%.2f" % splits[3])
    check("第一级远小于最后一级（对数项在近处起作用）",
          splits[0] < 0.2 * splits[3],
          "%.2f vs %.2f" % (splits[0], splits[3]))

    # 解析公式逐项对比 —— 本组最有价值的一条
    d, expect = practical_splits(CAM_NEAR, CAM_FAR, 90.0, 0.60, 4)
    print("  分割点(解析): %s  (上限 D=%.2f = min(%.1f, %.1f))"
          % (" / ".join("%.2f" % v for v in expect), d, CAM_FAR, 90.0))
    worst = max(abs(a - b) for a, b in zip(splits, expect))
    check("分割点逐项等于解析公式（误差 < 0.02）", worst < 0.02,
          "最大误差 %.4f" % worst)

    # ============ 第 2 组：每级剔除 ============
    print("\n== 第 2 组：每级裁剪空间剔除 ==")
    g = [int(v) for v in stats.groups()]
    n_casc, sub, con = g[0], g[1], g[2]
    per = [(g[3 + 2 * i], g[4 + 2 * i]) for i in range(4)]
    print("  每级 提交/考虑 : " + "  ".join("c%d=%d/%d" % (i, s, c)
                                            for i, (s, c) in enumerate(per)))
    check("cascades = 4", n_casc == 4, n_casc)
    # 每级各遍历一遍全部投影体 —— 不剔除的话提交量恰好是这个数。
    check("considered = 投影体数 × 级联数 = %d" % (n_casters * n_casc),
          con == n_casters * n_casc, con)
    check("总提交量 < 总考虑量（剔除真的在发生）", sub < con,
          "%d vs %d" % (sub, con))
    check("总提交量 > 0（没有过剔）", sub > 0, sub)
    check("不剔除的话会是 %d 次提交，实测 %d（省掉 %.0f%%）"
          % (con, sub, 100.0 * (1.0 - sub / float(con))), sub < con)
    check("每级 submitted <= considered",
          all(s <= c for s, c in per), per)
    check("各级提交量单调不减（级越大 → 光空间盒越大 → 装进去的越多）",
          all(per[i][0] <= per[i + 1][0] for i in range(3)),
          [s for s, _ in per])
    # 0.1–9.36m 的视锥切片包围球里一根柱子都没有（最近的柱子视线深度 ≈62.7m）
    check("第 0 级（0.1–9.36m 切片）提交 0 根", per[0][0] == 0, per[0])

    # ============ 第 3 组：级联数切换 ============
    print("\n== 第 3 组：级联数切换（MYVK_SHADOW_CASCADES=1 → 退化成旧单张图）==")
    shot_c1 = os.path.join(OUT_DIR, "csm_cascades1.png")
    lines_c1, _, img_c1, errs_c1 = run_once(exe, scene,
                                           {"MYVK_SHADOW_CASCADES": "1"},
                                           shot_c1)
    check("零 Vulkan 验证层报错", not errs_c1, errs_c1[:2])
    dbg1 = find_line(lines_c1, "SHADOW-DBG", DBG_RE)
    st1 = find_line(lines_c1, "SHADOW-STATS", STATS_RE)
    print("  SHADOW-DBG  :", dbg1.group(0) if dbg1 else None)
    print("  SHADOW-STATS:", st1.group(0) if st1 else None)
    if dbg1 and st1:
        check("cascades = 1", dbg1.group(1) == "1", dbg1.group(1))
        sp1 = [float(dbg1.group(4 + i)) for i in range(4)]
        check("四个分割点全部 = dist（一级吃满全程）",
              all(abs(v - 90.0) < 0.02 for v in sp1),
              " / ".join("%.2f" % v for v in sp1))
        g1 = [int(v) for v in st1.groups()]
        check("considered 掉到 5（少遍历 3 遍）", g1[2] == n_casters, g1[2])
        check("c0 = 5/5（一级的盒大到装下全部柱子）",
              g1[3] == n_casters and g1[4] == n_casters, (g1[3], g1[4]))
        # 1 级时 c1/c2/c3 从来没被渲染过 —— 统计量保持初值 0
        check("c1/c2/c3 全为 0/0（只渲染了 1 级）",
              g1[5] == 0 and g1[6] == 0 and g1[7] == 0 and g1[8] == 0
              and g1[9] == 0 and g1[10] == 0,
              g1[5:11])
    if img_c1 is not None:
        n, mx, mean, box = diff_stats(img_on, img_c1)
        print("  4 级 vs 1 级: 差异像素=%d 最大差=%d 均值=%.4f bbox=%s"
              % (n, mx, mean, box))
        # 弱断言：本场景所有遮挡体在同一视线深度、都落在同一级里，
        # 远级在两种配置下的包围球半径几乎一样（65m vs 74m）——
        # 差异**必然**很小。这里只确认"确实换了一套阴影图"。
        check("级联数变化确实改变了阴影图（差异像素 >= 5 且最大差 >= 25）",
              n >= 5 and mx >= 25, "n=%d max=%d" % (n, mx))

    # ============ 第 4 组：阴影距离分级 ============
    print("\n== 第 4 组：阴影距离分级（MYVK_SHADOW_DIST）==")
    shot_off = os.path.join(OUT_DIR, "csm_off.png")
    _, _, img_off, errs_off = run_once(exe, scene, {"MYVK_SHADOW": "0"},
                                       shot_off)
    check("拿到关阴影的截图", img_off is not None)
    check("零 Vulkan 验证层报错", not errs_off, errs_off[:2])

    res = {}
    dist_imgs = {}
    for tag, dist in (("near", 70), ("far", 25)):
        shot = os.path.join(OUT_DIR, "csm_dist%d.png" % dist)
        lines_d, _, img_d, errs_d = run_once(
            exe, scene, {"MYVK_SHADOW_DIST": str(dist)}, shot)
        check("dist=%d 零 Vulkan 验证层报错" % dist, not errs_d, errs_d[:2])
        dbg_d = find_line(lines_d, "SHADOW-DBG", DBG_RE)
        if dbg_d is None:
            check("dist=%d 拿到 SHADOW-DBG" % dist, False)
            continue
        spd = [float(dbg_d.group(4 + i)) for i in range(4)]
        print("  dist=%d 实测分割点: %s" % (dist, " / ".join(
            "%.2f" % v for v in spd)))
        d_d, exp_d = practical_splits(CAM_NEAR, CAM_FAR, float(dist), 0.60, 4)
        worst_d = max(abs(a - b) for a, b in zip(spd, exp_d))
        check("dist=%d 的分割点也逐项等于解析公式（误差 < 0.02）" % dist,
              abs(float(dbg_d.group(2)) - dist) < 0.05 and worst_d < 0.02,
              "dist=%.2f 最大误差 %.4f" % (float(dbg_d.group(2)), worst_d))
        check("dist=%d 时最后一级 = %d" % (dist, dist),
              abs(spd[3] - dist) < 0.02, "%.2f" % spd[3])
        if img_d is not None:
            res[dist] = diff_stats(img_off, img_d)
            dist_imgs[dist] = img_d

    if img_on is not None and img_off is not None:
        n90, mx90, mean90, box90 = diff_stats(img_off, img_on)
        print("  dist=90 vs 关阴影: 差异像素=%d 最大差=%d 均值=%.4f bbox=%s"
              % (n90, mx90, mean90, box90))
        check("dist=90 阴影完整（差异像素 > 1000 且最大差 > 40）",
              n90 > 1000 and mx90 > 40, "n=%d max=%d" % (n90, mx90))

        if 70 in res:
            n70, mx70, mean70, box70 = res[70]
            print("  dist=70 vs 关阴影: 差异像素=%d 最大差=%d 均值=%.4f bbox=%s"
                  % (n70, mx70, mean70, box70))
            # 关键一条：70m 落在淡出区间内 → 阴影被**削弱**而不是消失。
            # 这让"距离分级"和"全有全无的开关"彻底区分开。
            check("dist=70 阴影被削弱但仍在（50 < 差异像素 < 1200）",
                  50 < n70 < 1200, n70)
            # ⚠ 强度比较必须**限定在差异带内**。`diff_stats` 的均值是
            # `total/(W*H)`（整幅图），会把全图的零星噪声（上一用例残留、
            # 窗口状态）一起算进来 —— 实测套件里连跑时它会从 0.047 跳到
            # 0.149（而差异像素数与最大差**完全不变**），把这条断言变成
            # flaky。改用 `dist=90` 那条阴影带（box90）内的平均差。
            if dist_imgs.get(70) is not None and box90:
                band90 = region_diff(img_on, img_off, box90)
                band70 = region_diff(dist_imgs[70], img_off, box90)
                print("  差异带 %s 内平均差: dist=90 → %.3f, dist=70 → %.3f"
                      % (box90, band90, band70))
                check("dist=70 的阴影强度明显低于 dist=90"
                      "（差异带内均值 < 0.7 × dist=90）",
                      band70 < 0.7 * band90,
                      "%.4f vs %.4f" % (band70, band90))
            check("dist=70 的阴影变化量小于 dist=90（最大差 < dist=90）",
                  mx70 < mx90, "%d vs %d" % (mx70, mx90))

        if 25 in res:
            n25, mx25, mean25, _ = res[25]
            print("  dist=25 vs 关阴影: 差异像素=%d 最大差=%d 均值=%.4f"
                  % (n25, mx25, mean25))
            # 最强的一条：25m 之外彻底无阴影 → 画面必须与"关阴影"逐像素相同。
            check("dist=25 时画面与关阴影逐像素相同（差异像素 = 0）",
                  n25 == 0, "n=%d" % n25)
            check("dist=25 的平均差 ≈ 0", mean25 < 0.002, "%.6f" % mean25)

        # 单调性：距离上限越小 → 阴影越少 → 与关阴影的差异越小
        if 70 in res and 25 in res:
            check("差异像素随距离上限单调递减：n(90) > n(70) > n(25)=0",
                  n90 > res[70][0] > res[25][0] == 0,
                  "%d > %d > %d" % (n90, res[70][0], res[25][0]))

    # ============ 第 5 组：变化定位 ============
    print("\n== 第 5 组：阴影变化的定位（排除整体亮度偏移）==")
    if img_on is not None and img_off is not None and vp is not None:
        x0, y0, x1, y1 = [float(v) for v in vp]
        w, h = x1 - x0, y1 - y0
        upper = (x0, y0, x1, y0 + 0.25 * h)          # 视口上方 1/4
        d_upper = region_diff(img_on, img_off, upper)
        print("  视口上方 1/4 区域的平均差: %.3f" % d_upper)
        # 上方四分之一是天空/地平线远端，阴影开关不该动它。
        # 整体亮度偏移这类伪信号会让这里也跟着变。
        check("阴影开关不影响视口上方 1/4（平均差 < 0.5）",
              d_upper < 0.5, "%.3f" % d_upper)
        n90, mx90, mean90, box90 = diff_stats(img_off, img_on)
        if box90:
            bh = box90[3] - box90[1] + 1
            bw = box90[2] - box90[0] + 1
            print("  差异带: 高 %d px（视口的 %.1f%%）宽 %d px（视口的 %.1f%%）"
                  % (bh, 100.0 * bh / h, bw, 100.0 * bw / w))
            check("差异集中在地面一条窄带上（高度 < 视口的 8%）",
                  bh < 0.08 * h, "%d px" % bh)
            check("差异带横向铺开（宽度 > 视口的 50%）",
                  bw > 0.5 * w, "%d px" % bw)

    print("\n结果: %s" % ("PASS" if not fails else "FAIL: " + ", ".join(fails)))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
