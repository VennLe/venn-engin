#!/usr/bin/env python3
# ============================================================
# verify_morph_gpu —— §15 GPU 形变验收（像素级）
#
# ---------------------------- 为什么非要像素 ----------------------------
# `verify_morph.py` 验到"数据是对的"为止：MORPH 区块 v2 的 channel 表、
# delta 与顶点的双向自洽、offset_weights 预乘、运行期 MorphRig 合并。
# 那些全绿**也不能**证明顶点着色器真的读了 delta —— 中间还隔着三层：
#
#   ① Renderer 有没有为本帧分配槽位、把权重写进 SSBO（binding 26/27）
#   ② 槽位有没有被**正确送到**顶点手里（MDI 的 `gl_InstanceIndex` 是
#      紧凑化之后的下标，索引不到全局槽空间 —— 这一层最容易静默错，
#      见 Renderer::buildMdiFrameData 里 MdiInstance.aux.y 的说明）
#   ③ 形变有没有发生在**蒙皮之前**（顺序反了，角色一转身表情就往侧边飞）
#
# 这三层任何一层错了，日志全绿、数据全对、画面全错。
#
# ---------------------------- 怎么跑 ----------------------------
# 全程走**真实入口**，不手写角色场景：
#
#   MYVK_EDITOR_IMPORT=models/characters/alicia.fbx
#       → `EditorScene::instantiateModel`（唯一装配入口：20 个子网格实体
#         + SkinnedMeshComponent + MorphComponent）
#   MYVK_EDITOR_SCENE=scenes/_m15_cam.json
#       → 只提供一个正面特写的相机（`applyCamera` 直接设值）。
#         ⚠ 不能用 `MYVK_EDITOR_CAM`：它走 `setTarget(eye + forward*d)`，
#           在飞行模式下位置会被再推 `distance` 米，实测落到十几米外，
#           角色成了几十像素的一团 —— 那样的"像素差异"没有判别力。
#   MYVK_EDITOR_CONSOLE_CMD="morph.set target_0 1.0"
#       → 与 Inspector 滑条**同一条路**（都调 `assets::setMorphWeight`）。
#
# 四次启动，只有 console 命令不同：
#
#   base  不设权重                （全部 0）
#   zero  morph.set target_0 0.0  （显式设 0）
#   on    morph.set target_0 1.0
#   bad   morph.set <不存在的名> 1.0
#
# ---------------------------- 判据 ----------------------------
#   G0  四次都截到图；G0c 视口里**真有内容**（不是空场景）。
#       没有 G0c 的话，空场景会让 G1 平凡通过、G2 无意义地红 —— 上一版
#       脚本就栽在这儿（手写 skinned 块不建子网格实体，视口一片空白，
#       而 G1 显示"逐像素相同"，看起来还很健康）。
#   G1  base 与 zero 逐像素相同 → 形变是 `+= delta * w`，w=0 必须是
#       **恒等**，不是"接近恒等"
#   G2  zero 与 on 有可见差异 → 着色器真的在动顶点
#   G3  差异是**局部**的（包围盒 ≤ 视口 55%）→ 排除"整体位移/缩放/
#       槽位串了导致整个模型乱飞"
#   G4  on 打出 `MORPH-SET ok=1 channel=target_0`；bad 打出 ok=0
#       → 名字驱动真的是"按名字查表、查不到就失败"，不是静默成功
#   G5  bad 与 base 逐像素相同 → 命令失败时画面不能被改
#   G6  全程无崩溃
#
# 用法：
#     python tools/verify_morph_gpu.py
# ============================================================

import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_client, find_window, user32          # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors                 # noqa: E402
from verify_asset_drag import force_foreground                         # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
EXE = os.path.join(BIN, "Editor.exe")
SCENE_DIR = os.path.join(BIN, "assets", "scenes")
OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify", "morph_gpu")

SIZE = "1400x900"
MODEL = "models/characters/alicia.fbx"
CAM_SCENE = "_m15_cam.json"

# 位移最大的那条滑条（`verify_morph.py` 的 G 组统计：total|d| = 12.2 m，
# 覆盖 2495 个顶点）。选它是因为"像素要变"需要幅度够大，而不是因为它
# 语义上像某个表情 —— alicia 的通道名本来就是 target_N（见 MorphRig.cpp）。
CHANNEL = "target_0"

# 正面特写：角色站在原点、身高 1.652 m，脸在 z≈1.5。
# yaw = -90° 把相机放在 -Y 侧朝 +Y 看（实测这一侧是正面）。
CAM = {"target": [0.0, 0.0, 1.38], "yaw": -1.5708, "pitch": 0.05,
       "distance": 1.55, "fov": 50.0}

RE_MORPH_SET_OK = re.compile(r"MORPH-SET ok=1 entity=(\d+) channel=(\S+) "
                             r"w=([\d.-]+)")
RE_MORPH_SET_FAIL = re.compile(r"MORPH-SET ok=0 reason=(\S+)")
RE_MODELINST = re.compile(r"MODEL-INSTANCE .*meshes=(\d+) skinned=(\d+) "
                          r".*morph=(\d+)")

_n = 0
_bad = 0


def log(msg):
    print(msg)


def check(name, ok, detail=""):
    global _n, _bad
    _n += 1
    if not ok:
        _bad += 1
    print("CHECK %s %s%s" % (name, "OK" if ok else "FAIL",
                             (" (" + detail + ")") if detail else ""))


def write_cam_scene():
    """只放相机 + 一块地面。

    地面不是装饰：只有 skinned 实体的场景会让 BVH 建树越界
    （docs/anime_gap_audit.md 的 9.7 记过），有静态几何即可绕开。
    """
    os.makedirs(SCENE_DIR, exist_ok=True)
    scene = {
        "version": 1,
        "generator": "verify_morph_gpu.py",
        "camera": CAM,
        "light": {"direction": [0.42, 0.72, -0.55], "color": [1.0, 0.96, 0.9],
                  "intensity": 3.2, "castsShadow": True,
                  "ambientScale": 1.0},
        "physics": {"enabled": False},
        "entities": [{
            "name": "Ground",
            "transform": {"position": [0.0, 0.0, 0.0],
                          "rotation": [0.0, 0.0, 0.0],
                          "scale": [1.0, 1.0, 1.0]},
            "parent": -1, "visible": True, "castShadow": False, "locked": True,
            "mesh": {"kind": "builtin", "shape": "plane", "size": 40.0,
                     "name": "m15_ground"},
            "material": {"kind": "procedural", "name": "m15_ground_mat",
                         "baseColor": [0.42, 0.44, 0.47, 1.0],
                         "metallic": 0.0, "roughness": 0.9,
                         "normalScale": 0.0, "ao": 1.0,
                         "emissive": [0.0, 0.0, 0.0],
                         "doubleSided": False, "alphaBlend": False},
        }],
    }
    with open(os.path.join(SCENE_DIR, CAM_SCENE), "w", encoding="utf-8") as f:
        json.dump(scene, f, ensure_ascii=False, indent=1)
    return "scenes/" + CAM_SCENE


def snap(tap):
    """`verify_add_menu.LogTap` 只暴露带锁的 `lines`。

    直接读会与 pump 线程竞争 —— 那会让"逐像素相同"这类判据偶发假红。
    """
    with tap.lock:
        return list(tap.lines)


def grab(hwnd):
    try:
        return capture_client(hwnd)[0].convert("RGB")
    except Exception as exc:                              # noqa: BLE001
        log("  截图失败: %s" % exc)
        return None


def close(proc, hwnd):
    if hwnd:
        user32.PostMessageW(hwnd, 0x0010, 0, 0)
    time.sleep(0.6)
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=8)
        except subprocess.TimeoutExpired:
            proc.kill()


def run_case(tag, scene, console_cmd):
    env = {k: v for k, v in os.environ.items() if not k.startswith("MYVK_")}
    env["MYVK_EDITOR_SCENE"] = scene
    env["MYVK_EDITOR_IMPORT"] = MODEL
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    # HUD 关掉：叠加层里的 FPS / 帧时间每次都不同，会在背景上留下几阶
    # 噪声，"逐像素相同"立刻变成概率判据。
    env["MYVK_EDITOR_NO_HUD"] = "1"
    env["MYVK_LOG_RECTS"] = "1"
    # MORPH-FRAME 诊断行：G4d 靠它数"一次驱动命中了几个材质桶"。
    env["MYVK_MORPH_DBG"] = "1"
    if console_cmd:
        env["MYVK_EDITOR_CONSOLE_CMD"] = console_cmd
        # 默认第 40 帧执行 —— 那时布局、资产、第一帧渲染都已完成。
        # 再往后推一点，给 import 留足时间。
        env["MYVK_EDITOR_CONSOLE_CMD_FRAME"] = "70"

    proc = subprocess.Popen([EXE], cwd=BIN, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)

    hwnd = find_window("Venn Editor", timeout=60.0)
    if not hwnd:
        close(proc, None)
        return None, [], None

    user32.ShowWindow(hwnd, 9)
    time.sleep(1.2)
    tw, th = (int(v) for v in SIZE.split("x"))
    user32.SetWindowPos(hwnd, 0, 0, 0, tw, th, 0x0002 | 0x0004)
    time.sleep(3.0)
    force_foreground(hwnd)

    # 等模型真的进来（import 是异步的：解析 → 加载/烤 .vmesh → 建实体）。
    deadline = time.time() + 30.0
    while time.time() < deadline:
        if any("MODEL-INSTANCE" in ln for ln in snap(tap)):
            break
        time.sleep(0.2)
    # 命令在第 70 帧执行；TAA 还要若干帧收敛。留足时间再截。
    time.sleep(6.0)

    img = grab(hwnd)
    if img is not None:
        os.makedirs(OUT_DIR, exist_ok=True)
        img.save(os.path.join(OUT_DIR, "%s.png" % tag))
    vp = tap.last_rect("VP-RECT")
    lines = snap(tap)
    close(proc, hwnd)
    return img, lines, vp


def diff_stats(a, b, rect, thresh=24):
    """返回 (差异像素占比, 差异包围盒占比, 非背景像素占比)。

    第三个值是"画面里真有东西"的判据：以视口左上角 8×8 的平均色当背景
    （清屏色，角色永远不在那儿），与它差 > 30 的像素算"有内容"。
    """
    if a is None or b is None:
        return None, None, None
    w = min(a.width, b.width, int(rect[2]))
    h = min(a.height, b.height, int(rect[3]))
    xs, ys = max(0, int(rect[0])), max(0, int(rect[1]))
    pa, pb = a.load(), b.load()

    r = g = bl = 0.0
    cnt = 0
    for y in range(ys, min(ys + 8, h)):
        for x in range(xs, min(xs + 8, w)):
            c = pa[x, y]
            r += c[0]
            g += c[1]
            bl += c[2]
            cnt += 1
    bg = (r / max(cnt, 1), g / max(cnt, 1), bl / max(cnt, 1))

    n = 0
    content = 0
    x0, y0, x1, y1 = w, h, -1, -1
    for y in range(ys, h):
        for x in range(xs, w):
            ca, cb = pa[x, y], pb[x, y]
            if (abs(ca[0] - cb[0]) + abs(ca[1] - cb[1]) +
                    abs(ca[2] - cb[2])) > thresh:
                n += 1
                if x < x0:
                    x0 = x
                if x > x1:
                    x1 = x
                if y < y0:
                    y0 = y
                if y > y1:
                    y1 = y
            if (abs(ca[0] - bg[0]) + abs(ca[1] - bg[1]) +
                    abs(ca[2] - bg[2])) > 30:
                content += 1
    total = max(1, (w - xs) * (h - ys))
    if x1 < 0:
        return 0.0, 0.0, content / float(total)
    box = (x1 - x0 + 1) * (y1 - y0 + 1)
    return n / float(total), box / float(total), content / float(total)


def main():
    if not os.path.isfile(EXE):
        log("找不到 %s" % EXE)
        return 2
    kill_stale_editors()
    scene = write_cam_scene()

    cases = [
        ("base", None),
        ("zero", "morph.set %s 0.0" % CHANNEL),
        ("on", "morph.set %s 1.0" % CHANNEL),
        ("bad", "morph.set __no_such_channel__ 1.0"),
    ]
    imgs, logs, rects = {}, {}, {}
    for tag, cmd in cases:
        log("--- %s: %s ---" % (tag, cmd or "(不执行命令)"))
        img, lines, vp = run_case(tag, scene, cmd)
        imgs[tag], logs[tag], rects[tag] = img, lines, vp
        check("G0 场景 %s 截到图" % tag, img is not None, "vp=%s" % (vp,))

    vps = [r for r in rects.values() if r]
    cmp_rect = None
    if len(vps) == len(cases):
        cmp_rect = (max(r[0] for r in vps), max(r[1] for r in vps),
                    min(r[2] for r in vps), min(r[3] for r in vps))
        if cmp_rect[2] <= cmp_rect[0] or cmp_rect[3] <= cmp_rect[1]:
            cmp_rect = None
    check("G0b 拿到主视口矩形（只在视口内比像素）", cmp_rect is not None,
          str(cmp_rect))
    if cmp_rect is None:
        print("\n结果: FAIL（没有视口矩形，后面所有像素判据无从谈起）")
        return 1

    # ---- G0c：视口里真的有角色 ----
    _r, _b, content = diff_stats(imgs.get("base"), imgs.get("base"), cmp_rect)
    check("G0c 视口里有内容（不是空场景）",
          content is not None and content >= 0.05,
          "非背景像素 %.1f%%（阈值 5%%）" % ((content or 0) * 100.0))

    # ---- G4：命令本身 ----
    ok1 = RE_MORPH_SET_OK.search("\n".join(logs.get("on") or []))
    check("G4a on 打出 MORPH-SET ok=1 且 channel 对得上",
          ok1 is not None and ok1.group(2) == CHANNEL,
          ok1.group(0) if ok1 else "没有 ok=1 行")
    fail = RE_MORPH_SET_FAIL.search("\n".join(logs.get("bad") or []))
    check("G4b bad 打出 MORPH-SET ok=0（名字查不到必须失败，不能静默）",
          fail is not None and "unknown-channel" in fail.group(0),
          fail.group(0) if fail else "没有 ok=0 行")
    mi = RE_MODELINST.search("\n".join(logs.get("on") or []))
    check("G4c 走的是 instantiateModel（20 个子网格 + 蒙皮 + 49 根滑条）",
          mi is not None and mi.group(1) == "20" and mi.group(2) == "1"
          and mi.group(3) == "49",
          mi.group(0) if mi else "没有 MODEL-INSTANCE 行")

    # ---- G4d：一个滑条必须驱动**多个材质桶** ----
    #
    # 这一条针对的是一个真实发生过的 bug：`MorphRig::evaluate` 把"同一个
    # targetWeight 上挂的多个桶"误当成 in-between 的关键帧链，于是
    # `w <= points.front().weight`（key 的 targetWeight 普遍是 1.0，恒成立）
    # 只让**第一个桶**拿到 1.0，其余为 0 —— 一个滑条只动一块脸。
    # 日志、数据、`verify_morph.py` 的 34 项全绿，只有像素能看出来。
    # `MORPH-FRAME` 里每个槽位末尾的 `w0=` 把这件事摊开了：
    # `target_0` 在 alicia 上跨 3 个桶，推满之后至少要有 2 个槽 w0=1.00
    # （第 4 个桶里没有这条 channel，正确地保持 0）。
    frames = [ln for ln in (logs.get("on") or []) if "MORPH-FRAME" in ln]
    hit = 0
    if frames:
        hit = frames[-1].count("w0=1.00")
    check("G4d 一个滑条驱动了多个子网格桶（同名 channel 跨桶合并真的生效）",
          hit >= 2, "最后一帧有 %d 个槽位的首权重为 1.00（要求 >= 2）" % hit)

    # ---- G1：权重 0 = 恒等 ----
    r1, _b1, _c1 = diff_stats(imgs.get("base"), imgs.get("zero"), cmp_rect)
    check("G1 权重为 0 时逐像素等于不设权重（形变在 w=0 时是恒等）",
          r1 is not None and r1 == 0.0,
          "差异 %.4f%%（要求 0）" % (0.0 if r1 is None else r1 * 100.0))

    # ---- G2 / G3：真的在动，且只动局部 ----
    r2, b2, _c2 = diff_stats(imgs.get("zero"), imgs.get("on"), cmp_rect)
    check("G2 推满一根滑条画面必须变（GPU 真的读了 delta）",
          r2 is not None and r2 >= 0.0002,
          "差异 %.3f%%（阈值 0.02%%）" % (0.0 if r2 is None else r2 * 100.0))
    check("G3 变化是局部的（不是整体位移/缩放/槽位串了）",
          b2 is not None and b2 <= 0.55,
          "差异包围盒占视口 %.1f%%（阈值 55%%）" % ((b2 or 0) * 100.0))

    # ---- G5：失败的命令不能改画面 ----
    r3, _b3, _c3 = diff_stats(imgs.get("base"), imgs.get("bad"), cmp_rect)
    check("G5 命令失败时画面不变", r3 is not None and r3 == 0.0,
          "差异 %.4f%%（要求 0）" % (0.0 if r3 is None else r3 * 100.0))

    # ---- G6：无崩溃 ----
    crashed = [t for t, _c in cases
               if any(re.search(r"\bAssertion failed\b|terminate called|"
                                r"Access violation", ln)
                      for ln in (logs.get(t) or []))]
    check("G6 四次运行都没有崩溃", not crashed, "崩在 %s" % ",".join(crashed))

    print("\n结果: %s（%d 项）" % ("PASS" if _bad == 0 else "FAIL: %d 项" % _bad, _n))
    print("截图: %s" % OUT_DIR)
    return 0 if _bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
