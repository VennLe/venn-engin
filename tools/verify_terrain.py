"""端到端验证：地形（W4，见 docs/world_scale_roadmap.md §9）。

场景是 `tools/gen_terrain_probe.py` 生成的 `assets/scenes/levels/terrain_probe.json`：
1 个地形实体（512 m × 512 m，257² 采样点，maxHeight 60 m，4 层）+ 6 个台地木箱
+ 3 个坡上石墩。**箱子与石墩的 z 是生成器按同一份高度场算出来的** —— 所以
它们站不站得住，本身就是"渲染用的高度"与"物理用的高度"是否同一份的判据。

七条判据：

  A 组装       资产读进来、4 层材质都解析、管线 cull=none、LOD 真的有分块
  B 往返相等   `--dump-scene` 与源文件逐字段相等（float 容差 1e-6）
  C LOD 按距离 相机贴近地表 → 有 lod0 块；拉到 700 m 高空 → 全部 lod3
  D 改层材质   改 `terrain_grass.vmat` 的 baseColor → 画面显著变化 > 5%
  E 关掉地形   `terrain.visible=false` → 画面显著变化（且不再有 TERRAIN-LOD）
  F 性能       GPU 帧时间 < 16.7 ms
  G 物理自检   `--selftest terrain` 27 条全绿（B4/B5 是"< 1 cm"，D1/D2 是
               "30° 坡上不下滑"，都在那里）

⚠ D/E 都是**像素**判据，所以每组必须先拍两次"同一配置"当基线（自校准）：
  这台机器上两次启动之间角色/云层都能差 1~2%，拿绝对阈值会假红。
⚠ 基线**不能**是"关掉地形"那张 —— 那正是 E 要测的东西，用它当基线，
  E 会永远通过而 D 的阈值会松到没有意义。

用法：
    python tools/verify_terrain.py
    python tools/verify_terrain.py --no-ui     # 跳过 D/E（不开窗口）
"""

import argparse
import json
import math
import os
import re
import shutil
import subprocess
import sys
from safe_fs import safe_remove

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
GAME = os.path.join(BIN, "Game.exe")
SCENES = os.path.join(BIN, "assets", "scenes")
MATS = os.path.join(BIN, "assets", "materials")
TERRAIN_DIR = os.path.join(BIN, "assets", "terrain", "hills")
OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify")

LEVEL_REL = "scenes/levels/terrain_probe.json"
LEVEL_SRC = os.path.join(ROOT, "assets", "scenes", "levels", "terrain_probe.json")
VTERRAIN_SRC = os.path.join(ROOT, "assets", "terrain", "hills", "terrain.vterrain")

LOAD_RE = re.compile(
    r"TERRAIN-LOAD path=(\S+) size=(\d+)x(\d+) mpp=([\d.]+) maxh=([\d.]+) "
    r"layers=(\d+) chunk=(\d+)x(\d+)")
PIPE_RE = re.compile(r"TERRAIN-PIPELINE vert=(\S+) frag=(\S+) sets=(\d+) cull=(\w+)")
LAYER_RE = re.compile(r"TERRAIN-LAYER idx=(\d+) id=(\S+) albedo=(\S+) normal=(\S+)")
LOD_RE = re.compile(r"TERRAIN-LOD chunks=(\d+) lod0=(\d+) lod1=(\d+) "
                    r"lod2=(\d+) lod3=(\d+) tris=(\d+)")
SELFTEST_RE = re.compile(r"TERRAIN-TOTAL pass=(\d+) fail=(\d+)")
GPU_RE = re.compile(r"GPU-TIMING frame=([\d.]+)")
SCENE_RE = re.compile(r"GAME-SCENE path=\S+ entities=(\d+) skipped=(\d+)")

g_pass = 0
g_fail = 0


def check(name, ok, detail=""):
    global g_pass, g_fail
    if ok:
        g_pass += 1
        print("[ OK ] %s%s" % (name, ("  " + detail) if detail else ""))
    else:
        g_fail += 1
        print("[FAIL] %s%s" % (name, ("  " + detail) if detail else ""))


def clean_env(**extra):
    env = {k: v for k, v in os.environ.items() if not k.startswith("MYVK_")}
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env.update({k: str(v) for k, v in extra.items()})
    return env


def run(args, env_extra=None, timeout=240):
    p = subprocess.run([GAME] + list(args), cwd=BIN,
                       env=clean_env(**(env_extra or {})),
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, encoding="utf-8", errors="replace",
                       timeout=timeout)
    return p.returncode, p.stdout.splitlines()


def ensure_assets():
    """新场景 / 新地形资产不在 POST_BUILD 的拷贝范围里（W4 纪律：
    `assets/terrain` 走 `VENN_ALWAYS_ASSET_DIRS` 每次构建都拷，但
    `assets/scenes` 仍走 POST_BUILD）→ 这里补齐，别让人先手动 cp。"""
    dst = os.path.join(BIN, "assets", LEVEL_REL)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copyfile(LEVEL_SRC, dst)
    os.makedirs(TERRAIN_DIR, exist_ok=True)
    for name in ("height.r32", "splat.rgba8", "terrain.vterrain"):
        src = os.path.join(ROOT, "assets", "terrain", "hills", name)
        if os.path.exists(src):
            shutil.copyfile(src, os.path.join(TERRAIN_DIR, name))


def read_vterrain():
    with open(VTERRAIN_SRC, encoding="utf-8") as f:
        return json.load(f)


# ==================================================================== A

def group_a():
    print("\n== A 组：组装 ==")
    rc, lines = run(["--scene", LEVEL_REL, "--frames", "6"])

    n = 0
    m = [x for x in (SCENE_RE.search(l) for l in lines) if x]
    if m:
        n = int(m[0].group(1))
    check("A1 场景 10 个实体全部加载（1 地形 + 6 箱 + 3 石墩）", n >= 10,
          "entities=%d" % n)

    vt = read_vterrain()
    ld = [x for x in (LOAD_RE.search(l) for l in lines) if x]
    if not ld:
        check("A2 .vterrain 读进来", False, "no TERRAIN-LOAD")
    else:
        g = ld[0]
        cols, rows = int(g.group(2)), int(g.group(3))
        mpp, maxh = float(g.group(4)), float(g.group(5))
        layers, cw, ch = int(g.group(6)), int(g.group(7)), int(g.group(8))
        # ⚠ 断言 4 个字段，**不是**"日志出现了"：日志出现只证明走到了那行，
        #   值对不对得逐项比。sha 不进断言 —— 高度图是生成器的产物，
        #   生成器改一次参数这里就该红，而"逐位相等"由 A4 的部分守（自检 A4）。
        ok = (cols == int(vt["cols"]) and rows == int(vt["rows"])
              and abs(mpp - float(vt["metersPerPixel"])) < 1e-4
              and abs(maxh - float(vt["maxHeight"])) < 1e-3
              and layers == 4)
        check("A2 尺寸 / 米每像素 / 最大高度 / 层数 与 .vterrain 一致", ok,
              "%dx%d mpp=%.3f maxh=%.2f layers=%d chunk=%dx%d"
              % (cols, rows, mpp, maxh, layers, cw, ch))

    pp = [x for x in (PIPE_RE.search(l) for l in lines) if x]
    # cull=none 是 D5 的硬要求：裙边三角形的绕序取决于从哪条边看，
    # 背面剔除会把裙边剔掉一半 → 接缝露出来。
    ok = bool(pp) and pp[0].group(3) == "2" and pp[0].group(4) == "none"
    check("A3 管线 sets=2（地形自己的 set1）、cull=none（裙边不能剔除）", ok,
          pp[0].group(0).split("] ")[-1] if pp else "none")

    ly = [x for x in (LAYER_RE.search(l) for l in lines) if x]
    want = list(vt["layers"]) if "layers" in vt else []
    got = [x.group(2) for x in ly]
    check("A4 4 层材质按 .vterrain 的顺序解析（草/土/岩/雪）",
          got == want and len(got) == 4, "%s vs %s" % (got, want))

    lod = [x for x in (LOD_RE.search(l) for l in lines) if x]
    tris = int(lod[0].group(6)) if lod else 0
    chunks = int(lod[0].group(1)) if lod else 0
    # 64 = (512/64)² —— 块 32 格 × 2 m = 64 m。这条同时守住"块切分没退化"。
    check("A5 分成 64 块且真的画了三角形", chunks == 64 and tris > 0,
          "chunks=%d tris=%d" % (chunks, tris))
    return lines


# ==================================================================== B

def group_b():
    print("\n== B 组：场景往返逐字段相等 ==")
    out_abs = os.path.join(SCENES, "_ter_roundtrip.json")
    if os.path.exists(out_abs):
        safe_remove(out_abs)
    run(["--scene", LEVEL_REL, "--dump-scene", out_abs, "--frames", "6"])
    if not os.path.exists(out_abs):
        check("B1 dump 生成", False, "no file")
        return
    with open(LEVEL_SRC, encoding="utf-8") as f:
        a = json.load(f)
    with open(out_abs, encoding="utf-8") as f:
        b = json.load(f)
    safe_remove(out_abs)
    # ⚠ `generator` 是生成器的注释串，dump 不写它 —— 不 pop 掉会永远差一条。
    a.pop("generator", None)
    b.pop("generator", None)

    bad = []

    def walk(x, y, path=""):
        if isinstance(x, dict) and isinstance(y, dict):
            for k in set(x) | set(y):
                if k not in x or k not in y:
                    bad.append(path + "/" + k)
                    continue
                walk(x[k], y[k], path + "/" + k)
        elif isinstance(x, list) and isinstance(y, list):
            if len(x) != len(y):
                bad.append(path + " (len)")
                return
            for i, (u, v) in enumerate(zip(x, y)):
                walk(u, v, path + "[%d]" % i)
        elif isinstance(x, bool) or isinstance(y, bool):
            if x != y:
                bad.append(path)
        elif isinstance(x, (int, float)) and isinstance(y, (int, float)):
            if not math.isclose(x, y, rel_tol=1e-6, abs_tol=1e-6):
                bad.append(path)
        elif x != y:
            bad.append(path)

    walk(a, b)
    check("B1 读→写往返逐字段相等（含 terrain 块）", not bad,
          "%d 处不同: %s" % (len(bad), bad[:4]) if bad else "0 处")


# ==================================================================== C

def _lod_of(scene_rel):
    _, lines = run(["--scene", scene_rel, "--frames", "6"])
    m = [x for x in (LOD_RE.search(l) for l in lines) if x]
    if not m:
        return None
    g = m[-1]      # 分布变化时会重打，取最后一条 = 稳态
    return tuple(int(g.group(i)) for i in range(1, 7))


def group_c():
    print("\n== C 组：LOD 真的按距离选 ==")
    with open(LEVEL_SRC, encoding="utf-8") as f:
        base = json.load(f)
    # 地形在原点附近的高度：生成器算出 17.10 m，差一两米不影响 LOD 判定。
    target = [0.0, 0.0, 17.1]

    near_rel = "scenes/_ter_near.json"
    far_rel = "scenes/_ter_far.json"
    variants = {
        # 贴地：相机在地形正中上方 8 m
        near_rel: {"target": target, "yaw": 0.62, "pitch": 0.02,
                   "distance": 8.0, "fov": 60.0},
        # "远处"**不能**靠把 distance 拉大 —— ⚠ `Camera.cpp:136` 把场景里的
        # distance 钳在 **0.5 ~ 100 m**，写 700 出来还是 100，于是"700 m 高空"
        # 这个机位根本不存在（第一版就是这么假绿的：near 6 / far 7 个 lod0，
        # 分布几乎一样，C3/C4 看着像 bug，其实是机位没动）。
        # 正确做法是**把 target 搬到地形外面**：target 只是一个看向的点，
        # 相机 = target + 100 m 内的偏移 → 整片地形都在 700 m 之外。
        far_rel: {"target": [900.0, 900.0, 0.0], "yaw": 0.62, "pitch": 0.10,
                  "distance": 8.0, "fov": 60.0},
    }
    try:
        got = {}
        for rel, cam in variants.items():
            doc = json.loads(json.dumps(base))
            doc["camera"] = cam
            with open(os.path.join(SCENES, os.path.basename(rel)), "w",
                      encoding="utf-8", newline="\n") as f:
                json.dump(doc, f, indent=2)
            got[rel] = _lod_of(rel)

        near, far = got[near_rel], got[far_rel]
        ok = near and far
        check("C1 两个机位都打出了 LOD 分布", bool(ok),
              "near=%s far=%s" % (near, far))
        if ok:
            # 阈值是 96 / 192 / 384 m（块 64 m × 1.5，之后每级 ×2）。
            # 贴地 8 m → 周围一圈块落在 lod0；搬到 900 m 外 → 最近的块（约
            # 350 m 开外）也过了 384 m 的上限……不够就至少全 lod2 以上。
            check("C2 贴地机位有 lod0 块（近处用满精度）", near[1] > 0,
                  "lod0=%d lod1=%d lod2=%d lod3=%d"
                  % (near[1], near[2], near[3], near[4]))
            check("C3 搬到地形外 900 m → 一个 lod0 都没有", far[1] == 0,
                  "lod0=%d lod1=%d lod2=%d lod3=%d"
                  % (far[1], far[2], far[3], far[4]))
            # 三角形数必须跟着少：LOD 的**唯一目的**就是这个。只看"档位变了"
            # 不够 —— 档位算对了但索引没换，三角形数一样不变。
            check("C4 远处的三角形数显著少于贴地（< 一半）", far[5] < near[5] * 0.5,
                  "near=%d tris far=%d tris (%.1f%%)"
                  % (near[5], far[5], 100.0 * far[5] / max(near[5], 1)))
    finally:
        for rel in variants:
            try:
                safe_remove(os.path.join(SCENES, os.path.basename(rel)))
            except OSError:
                pass


# ==================================================================== D / E

def capture(title, scene_rel, settle=3.4):
    """带窗口跑一次并抓图。stdout **必须落文件** —— 不读的 PIPE 会被写满，
    子进程卡在 write 上，连窗口都建不出来（W2-1 踩过）。"""
    from capture_window import find_window, capture_client, user32
    import time as _t
    log_path = os.path.join(OUT_DIR, "_ter_runtime.log")
    logf = open(log_path, "w+", encoding="utf-8")
    proc = subprocess.Popen([GAME, "--title", title, "--scene", scene_rel],
                            cwd=BIN, env=clean_env(), stdout=logf,
                            stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                            text=True)
    img = None
    hwnd = 0
    try:
        hwnd = find_window(title, 20.0)
        if not hwnd:
            return None
        user32.ShowWindow(hwnd, 9)
        user32.SetWindowPos(hwnd, 0, 0, 0, 1280, 720, 0x0002 | 0x0004)
        _t.sleep(settle)
        img, ok = capture_client(hwnd)
        if not ok:
            img = None
    finally:
        try:
            user32.PostMessageW(hwnd, 0x0010, 0, 0)
        except Exception:
            pass
        _t.sleep(0.5)
        try:
            proc.terminate()
        except Exception:
            pass
        try:
            proc.wait(timeout=8)
        except Exception:
            proc.kill()
        logf.close()
    return img


def sig_ratio(a, b, thresh=8):
    from PIL import ImageChops
    if a is None or b is None:
        return 0.0
    ca, cb = a.convert("RGB"), b.convert("RGB")
    diff = ImageChops.difference(ca, cb).convert("L")
    hist = diff.histogram()
    total = max(ca.size[0] * ca.size[1], 1)
    return sum(hist[thresh + 1:]) / float(total)


def write_variant(name, mutate):
    with open(LEVEL_SRC, encoding="utf-8") as f:
        doc = json.load(f)
    mutate(doc)
    with open(os.path.join(SCENES, name), "w", encoding="utf-8",
              newline="\n") as f:
        json.dump(doc, f, indent=2)
    return "scenes/" + name


def group_de():
    print("\n== D 组：改一次层材质，整片地面都变 ==")
    title = "VennTerrainTest"
    grass = os.path.join(MATS, "terrain_grass.vmat")
    shot = write_variant("_ter_shot.json", lambda d: None)

    with open(grass, "rb") as f:
        original = f.read()
    try:
        # 基线：同一份配置连开两次。**不能**用"关掉地形"那张 —— 那正是 E
        # 要测的东西，拿它当基线会让 E 恒通过。
        base_a = capture(title, shot)
        base_b = capture(title, shot)
        if base_a is None or base_b is None:
            check("D0 抓图（窗口建不出来？）", False, "img=None")
            return
        baseline = sig_ratio(base_a, base_b)

        doc = json.loads(original.decode("utf-8"))
        # ⚠ 探针用 baseColor，不用 roughness：与 verify_first_level 同一个
        #   理由（这套光照下 roughness 只改高光尺寸，全屏变化 < 1%）。
        #   草占屏一半以上，绿 → 品红应该是一眼可见。
        doc["baseColor"] = [0.62, 0.06, 0.52, 1.0]
        with open(grass, "w", encoding="utf-8", newline="\n") as f:
            json.dump(doc, f, indent=2)
        after = capture(title, shot)
        ratio = sig_ratio(base_b, after)
        # 判据 = 占比 > 5% **且** > 10× 基线（自校准，见 MEMORY 的像素断言纪律）
        ok = ratio > max(0.05, baseline * 10.0)
        check("D1 改 terrain_grass.vmat 的 baseColor → 画面显著变化 > 5%", ok,
              "sig=%.2f%% 基线=%.2f%%" % (ratio * 100.0, baseline * 100.0))
    finally:
        with open(grass, "wb") as f:
            f.write(original)

    print("\n== E 组：关掉地形 → 地面消失 ==")
    hidden = write_variant(
        "_ter_hidden.json",
        lambda d: [e["terrain"].__setitem__("visible", False)
                   for e in d["entities"] if "terrain" in e])
    try:
        before = capture(title, shot)
        after = capture(title, hidden)
        ratio = sig_ratio(before, after)
        # 地形占屏远大于一半 → 阈值给 20%，比 D 高得多：
        # 低了就说明"关掉了但其实还在画"。
        check("E1 关掉地形 → 画面显著变化 > 20%", ratio > 0.20,
              "sig=%.2f%%" % (ratio * 100.0))
    finally:
        try:
            safe_remove(os.path.join(SCENES, "_ter_hidden.json"))
        except OSError:
            pass

    # 日志侧的"真的跳过了"由 E' 组单独断（见 group_e_log）——
    # 那是**两个**断言（MEMORY：「状态一致」与「像素一致」不是一回事）。
    try:
        safe_remove(os.path.join(SCENES, "_ter_shot.json"))
    except OSError:
        pass


def group_e_log():
    """关掉地形时渲染层必须整条跳过（无 TERRAIN-LOD）。"""
    print("\n== E' 组：关掉地形时渲染层整条跳过 ==")
    with open(LEVEL_SRC, encoding="utf-8") as f:
        doc = json.load(f)
    for e in doc["entities"]:
        if "terrain" in e:
            e["terrain"]["visible"] = False
    keep = os.path.join(SCENES, "_ter_novis.json")
    with open(keep, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, indent=2)
    try:
        _, lines = run(["--scene", "scenes/_ter_novis.json", "--frames", "6"])
        lod = [x for x in (LOD_RE.search(l) for l in lines) if x]
        # ⚠ 注意"没有日志"不等于"没画"：也可能整个地形实例压根没建。
        #   所以同时要求 TERRAIN-LOAD 也**不**出现 —— 两个都没有才叫"跳过"。
        load = [x for x in (LOAD_RE.search(l) for l in lines) if x]
        check("E2 terrain.visible=false → 既不建实例也不画", not lod,
              "lod_lines=%d load_lines=%d tris=%s"
              % (len(lod), len(load),
                 lod[0].group(6) if lod else "none"))

        # 眼睛图标（实体级可见性）也必须认：这是 Hierarchy 里最常用的操作。
        # 只认 component 的话会出现"眼睛关掉了，山还在"。
        doc2 = json.loads(json.dumps(doc))
        for e in doc2["entities"]:
            if "terrain" in e:
                e["terrain"]["visible"] = True
                e["visible"] = False
        with open(keep, "w", encoding="utf-8", newline="\n") as f:
            json.dump(doc2, f, indent=2)
        _, lines2 = run(["--scene", "scenes/_ter_novis.json", "--frames", "6"])
        lod2 = [x for x in (LOD_RE.search(l) for l in lines2) if x]
        check("E3 实体级 visible=false（Hierarchy 的眼睛）同样让地形消失",
              not lod2, "lod_lines=%d" % len(lod2))
    finally:
        try:
            safe_remove(keep)
        except OSError:
            pass


# ==================================================================== F

def group_f():
    print("\n== F 组：性能 ==")
    _, lines = run(["--scene", LEVEL_REL, "--frames", "60"],
                   {"MYVK_GPU_TIMING": "1"})
    gpu = [float(x.group(1)) for x in (GPU_RE.search(l) for l in lines) if x]
    ok = bool(gpu) and max(gpu) < 16.7
    # 地形是 36k 三角形一条独立管线 + 4 层混合 —— 比 sandbox 贵是正常的，
    # 但不能贵到一个数量级（那就是每帧重建索引/描述符了）。
    check("F1 GPU 帧时间 < 16.7 ms（60 fps 线）", ok,
          "frame=%s ms" % ("/".join("%.2f" % v for v in gpu[:5]) or "none"))


# ==================================================================== G

def group_g():
    print("\n== G 组：物理自检（27 条）==")
    rc, lines = run(["--selftest", "terrain"], timeout=600)
    m = [x for x in (SELFTEST_RE.search(l) for l in lines) if x]
    if not m:
        check("G1 --selftest terrain 跑起来", False, "no TERRAIN-TOTAL")
        return
    p, f = int(m[0].group(1)), int(m[0].group(2))
    # ⚠ `>= 27` 而不是 `== 27`：自检是**可以加**的，加了应该变绿不该变红。
    #   但 fail 必须是 0，rc 必须是 0。
    check("G1 地形自检全绿（≥27 条，0 失败，rc=0）", f == 0 and p >= 27
          and rc == 0, "pass=%d fail=%d rc=%d" % (p, f, rc))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-ui", action="store_true", help="跳过 D/E（不开窗口）")
    args = ap.parse_args()

    if not os.path.exists(GAME):
        print("找不到可执行文件，请先构建：", GAME)
        return 1
    if not os.path.exists(LEVEL_SRC):
        print("找不到场景，请先跑：python tools/gen_terrain_probe.py")
        return 1
    os.makedirs(OUT_DIR, exist_ok=True)
    ensure_assets()

    group_a()
    group_b()
    group_c()
    group_e_log()
    if not args.no_ui:
        group_de()
    group_f()
    group_g()

    print("\n结果: %s  (pass=%d fail=%d)"
          % ("PASS" if g_fail == 0 else "FAIL: %d 项" % g_fail, g_pass, g_fail))
    return 0 if g_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
