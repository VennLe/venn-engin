"""端到端验证：物体级视锥剔除（+ 世界矩阵缓存 / 统一绘制列表）。

背景：这是"渲染性能"三件套的第一件（视锥剔除 / 遮挡剔除+加速结构 / LOD）。
本脚本只验证视锥剔除这一件，覆盖：

  1. 造一个"绝大多数物体在视锥外"的场景（137 个立方体：4 个在镜头里，
     其余分三组 —— 远在远平面之外 / 在相机背后 / 远在视锥侧面之外）。
     断言 CULL-STATS 里 culled > 0、drawnOpaque == renderables - culled。
  2. 关掉剔除（MYVK_FRUSTUM_CULL=0）再跑一次：断言 culled == 0、
     drawnOpaque == renderables（全量提交）。
  3. **两次的视口截图必须一致** —— 这是"剔除没漏画"的硬证据：
     被剔掉的物体全在屏幕外，不开剔除时它们根本不栅格化，画面理应逐像素相同。
  4. 全程零 Vulkan 验证层报错。

用法：
    python tools/verify_frustum_cull.py
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
from verify_add_menu import LogTap, kill_stale_editors             # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = os.path.join(ROOT, "..", "_venn_verify")

# 相机：站在 (0,0,3)，朝 +X 平视（yaw=180° 时 forward = +X，见 Camera::angleDir）
CAM = "0,0,3,180,0"

# 近处（视锥内）立方体的位置。留出足够的视锥余量，避免"擦边可见"
# 在浮点误差下抖动。
NEAR_CUBES = [(5.0, 0.0, 3.0), (9.0, 1.5, 3.0), (9.0, -1.5, 3.0),
              (14.0, 0.0, 2.5)]


def build_scene(path):
    """写一个 137 个立方体的测试场景，返回物体总数。"""
    ents = []

    def cube(x, y, z, cast=True):
        ents.append({
            "name": "Cube_%d" % len(ents),
            "transform": {"position": [x, y, z], "rotation": [0, 0, 0],
                          "scale": [1, 1, 1]},
            "visible": True,
            "castShadow": cast,
            "parent": -1,
            # 全部共用同一颗网格 / 同一份材质（缓存键相同）—— 137 个实体
            # 只占 1 个 GPU 网格，测的是剔除而不是资源重复上传。
            "mesh": {"kind": "builtin", "shape": "cube", "size": 1.0,
                     "name": "cube_cull"},
            "material": {"kind": "procedural", "name": "mat_cull",
                         "baseColor": [0.70, 0.75, 0.82, 1.0],
                         "metallic": 0.0, "roughness": 0.55,
                         "normalScale": 1.0, "ao": 1.0,
                         "emissive": [0.0, 0.0, 0.0], "doubleSided": False,
                         "alphaBlend": False},
        })

    # 1) 视锥内：4 个
    for (x, y, z) in NEAR_CUBES:
        cube(x, y, z, cast=True)
    # 2) 远在远平面（200m）之外：39 个
    for x in range(250, 4050, 100):
        cube(float(x), 0.0, 3.0, cast=False)
    # 3) 相机背后：40 个
    for x in range(-10, -410, -10):
        cube(float(x), 0.0, 3.0, cast=False)
    # 4) 远在视锥侧面之外：27 + 27 个
    for y in range(300, 3000, 100):
        cube(30.0, float(y), 3.0, cast=False)
    for y in range(-300, -3000, -100):
        cube(30.0, float(y), 3.0, cast=False)

    doc = {
        "version": 1,
        "generator": "verify_frustum_cull.py",
        "light": {"direction": [-0.45, -0.65, -0.62], "color": [1.0, 0.98, 0.95],
                  "intensity": 1.0, "castsShadow": True, "ambientScale": 0.55},
        "entities": ents,
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, ensure_ascii=False, indent=1)
    return len(ents)


STATS_RE = re.compile(r"CULL-STATS\s+(.*)")


def parse_stats(tap):
    """取最后一条 CULL-STATS，解析成 dict。"""
    line = None
    with tap.lock:
        for l in tap.lines:
            if "CULL-STATS" in l:
                line = l
    if not line:
        return None
    m = STATS_RE.search(line)
    if not m:
        return None
    out = {}
    for tok in m.group(1).split():
        if "=" not in tok:
            continue
        k, v = tok.split("=", 1)
        try:
            out[k] = int(v)
        except ValueError:
            out[k] = v
    return out


def run_once(exe, scene, extra_env, shot_path, wait_stats=12.0):
    """跑一轮编辑器：等 CULL-STATS 出现 → 截图 → 收集验证层报错。"""
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_FRUSTUM_DBG"] = "1"
    env["MYVK_BVH_DBG"] = "1"
    env["MYVK_EDITOR_SCENE"] = scene
    env["MYVK_EDITOR_CAM"] = CAM
    # 钉死裁剪面（2026-10-17）。本场景是按"远平面 200 m"设计的：39 个立方体
    # 摆在 x=250..4050，注释原文就是"远在远平面之外"。而编辑器视口默认已经
    # 放开到 5000 m（修"飞高俯视视野越来越窄"那一项时改的，见
    # EditorContext::applyEditorCameraMode）→ 那 39 个全部进入视锥，
    # culled 从 132 掉到 94、drawn 从 4 涨到 42。
    #
    # ⚠ 别改成"把基线从 132 改成 94"：那样这条用例就变成在测"编辑器视口的
    #   远平面是多少"，视口默认再动一次它还会红。剔除算得对不对与远平面无关，
    #   所以要钉的是**输入**，不是**期望**。
    env["MYVK_EDITOR_CLIP"] = "0.1,200"
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
            return None, None, ["未找到编辑器窗口"]
        user32.ShowWindow(hwnd, 9)
        time.sleep(1.5)
        # 固定客户区尺寸，保证两次跑的画面可比
        user32.SetWindowPos(hwnd, 0, 0, 0, 1280, 800, 0x0002 | 0x0004)
        time.sleep(2.5)

        # 等第一批 CULL-STATS（每 30 帧一条）
        end = time.time() + wait_stats
        while time.time() < end and parse_stats(tap) is None:
            time.sleep(0.25)
        stats = parse_stats(tap)

        time.sleep(1.0)
        img, ok = capture_client(hwnd)
        img.save(shot_path)

        with tap.lock:
            errs = [l for l in tap.lines
                    if ("VUID" in l or "VALIDATION" in l or " ERROR" in l
                        or "Error:" in l)]
        return stats, (img, ok), errs
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


def region_diff(img_a, img_b, box):
    """box=(x0,y0,x1,y1) 内的平均绝对像素差（0-255）。"""
    x0, y0, x1, y1 = [int(v) for v in box]
    W, H = img_a.size
    x0, y0 = max(x0, 0), max(y0, 0)
    x1, y1 = min(x1, W), min(y1, H)
    pa, pb = img_a.load(), img_b.load()
    total, n = 0, 0
    for y in range(y0, y1, 2):
        for x in range(x0, x1, 2):
            a, b = pa[x, y], pb[x, y]
            total += abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2])
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

    scene_path = os.path.join(OUT_DIR, "cull_test_scene.json")
    total = build_scene(scene_path)
    print("测试场景：%s（%d 个立方体）" % (scene_path, total))

    fails = []
    def check(name, cond, detail=""):
        print(("  [PASS] " if cond else "  [FAIL] ") + name +
              (("  " + str(detail)) if detail else ""))
        if not cond:
            fails.append(name)

    kill_stale_editors()

    print("\n== 第 1 轮：视锥剔除开启 ==")
    shot_on = os.path.join(OUT_DIR, "frustum_cull_on.png")
    s_on, img_on, errs_on = run_once(exe, scene_path, {}, shot_on)
    print("  stats:", s_on)
    check("拿到 CULL-STATS", s_on is not None)
    check("零 Vulkan 验证层报错", not errs_on, errs_on[:2] if errs_on else "")
    if s_on:
        check("renderables == %d" % total, s_on.get("renderables") == total,
              s_on.get("renderables"))
        check("culled > 0", s_on.get("culled", 0) > 0, s_on.get("culled"))
        check("drawnOpaque == renderables - culled",
              s_on.get("drawnOpaque") ==
              s_on.get("renderables", 0) - s_on.get("culled", 0),
              "drawn=%s renderables=%s culled=%s"
              % (s_on.get("drawnOpaque"), s_on.get("renderables"),
                 s_on.get("culled")))
        check("enabled == 1", s_on.get("enabled") == 1, s_on.get("enabled"))
        check("多数物体被剔除（>= 100）", s_on.get("culled", 0) >= 100,
              s_on.get("culled"))

    print("\n== 第 2 轮：视锥剔除关闭（对照组）==")
    shot_off = os.path.join(OUT_DIR, "frustum_cull_off.png")
    s_off, img_off, errs_off = run_once(exe, scene_path,
                                        {"MYVK_FRUSTUM_CULL": "0"}, shot_off)
    print("  stats:", s_off)
    check("拿到 CULL-STATS", s_off is not None)
    check("零 Vulkan 验证层报错", not errs_off, errs_off[:2] if errs_off else "")
    if s_off:
        check("culled == 0", s_off.get("culled") == 0, s_off.get("culled"))
        check("drawnOpaque == renderables（全量提交）",
              s_off.get("drawnOpaque") == s_off.get("renderables"),
              "drawn=%s renderables=%s" % (s_off.get("drawnOpaque"),
                                           s_off.get("renderables")))
        check("enabled == 0", s_off.get("enabled") == 0, s_off.get("enabled"))
        check("drawCalls 明显变多（剔除确实省了提交）",
              s_off.get("drawCalls", 0) > s_on.get("drawCalls", 0),
              "off=%s on=%s" % (s_off.get("drawCalls"),
                                s_on.get("drawCalls")))

    print("\n== 第 3 轮：画面必须一致（剔除没漏画）==")
    if img_on and img_off:
        a, b = img_on[0], img_off[0]
        if a.size != b.size:
            check("两次截图尺寸一致", False, "%s vs %s" % (a.size, b.size))
        else:
            # 只比 3D 视口区域：状态栏的 FPS 文本逐帧变化，不能算进去
            # 视口大约占客户区左侧 78% × 上方 88%（分栏布局），这里取一个
            # 保守的内缩区域。
            W, H = a.size
            box = (10, 60, int(W * 0.72), int(H * 0.80))
            d = region_diff(a, b, box)
            check("视口区域像素一致（平均差 < 3.0）", d < 3.0, "diff=%.3f" % d)
    else:
        check("两次截图都拿到了", False)

    print("\n结果: %s" % ("PASS" if not fails else "FAIL: " + ", ".join(fails)))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
