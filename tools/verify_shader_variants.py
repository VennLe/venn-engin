"""端到端验证：Shader 变体 / 关键字系统（见 render/ShaderKeywords.h）。

背景：着色器是**离线 glslc 预编译**的固定管线。在引入关键字之前，材质
特性只能靠手写 N 条管线来开关（不透明 / 双面 / 透明 / 透明双面四条，
外加三处 if 阶梯），再加一个特性就是 8 条。而且"有无法线图"这件事根本
没得开关 —— pbr.frag 无条件采样法线贴图并跑一遍 cotangent frame，
哪怕材质压根没挂法线图（只是绑了张内置的平坦法线）。

这个脚本验的就是新机制：

  · 材质特性 → 关键字掩码 → 变体 .spv + 管线固定状态，一条规则推导；
  · 掩码是位集合（0..7）：bit0 双面 / bit1 透明混合 / bit2 有法线图；
  · 管线按掩码惰性创建、命中缓存，没用到过的组合不建；
  · 队列分类（不透明 / 透明）也由掩码推导，"半透明"只有一处定义。

断言分五组：

  1. **材质特性 → 变体路由**（SHADER-VARIANTS 日志）
     一个 6 物体的场景，材质分别带 双面 / 法线图 / 透明 / 双面+法线图，
     期望掩码集合恰好是 {0, 1, 2, 4, 5}，fwd_created = 5。
     深度预通道只关心双面那一位（它不输出颜色）→ depth_created = 2。
     同一次运行里连采两次日志，fwd_created 必须**不再增长** —— 这就是
     "缓存生效、没有逐绘制创建管线"的直接证据。

  2. **NORMAL_MAP 编掉的路径 ≡ 绑平坦法线贴图的路径**（核心断言）
     这是"把整段采样与 TBN 编译掉"这个优化**唯一**的正确性依据：
     没挂法线图时绑的是内置平坦法线 (128,128,255, 线性)，*2-1 得
     (0.00392, 0.00392, 1.0)（注意 128/255 ≈ 0.50196，**不是**精确的 0.5），
     再乘 normalScale 仍是它自己，于是 TBN * normalize(nSample) 与 TBN 的
     第三列 = N 只差一个极小的水平扰动。所以两条路径的差异上限是
     **1 个 LSB**（实测 max diff = 1，>18 的像素数为 0，逐像素均值 0.10）。
     做法：场景里**没有任何法线贴图**，跑两次——
       MYVK_SHADER_FORCE_MASK=0（全关，NORMAL_MAP 段被编掉）
       MYVK_SHADER_FORCE_MASK=4（只开 NORMAL_MAP，采样内置平坦法线）
     两张图必须没有任何"可见"差异。不需要任何贴图文件。

  3. **DOUBLE_SIDED 真的改了栅格化状态**
     单独一个只有地面 + 两块朝向相反的竖直单面板（不透明、没法线图）的场景，
     跑两次：FORCE_MASK=0 vs =1。双面那次的背面必须画出来，差异显著。
     两块朝向相反是刻意的 —— 单面板的正面/背面取决于绕序，放两块就
     一定有一块是背对相机的，不必去赌 plane 图元的法线朝哪边。

  4. **NORMAL_MAP 不是死代码**
     场景里挂一张非平坦的棋盘法线图，跑两次 FORCE_MASK=0 vs =4。
     画面必须有显著差异（实测 5808 像素、最大差 30）—— 否则"编掉法线段"
     这个优化就成了"法线贴图根本没生效"，第 2 组也会因为两边都错而假通过。
     第 2 组与第 4 组是一对：一个证明"关掉是对的"（max = 1），
     一个证明"打开是有用的"（max = 30，20 倍余量）。

  5. **ALPHA_BLEND 的路由与混合**
     地面 + 一块半透明挡板（baseColor.a = 0.3）+ 挡板后面一个亮色立方体。
     跑两次 FORCE_MASK=0 vs =2：混合那次必须让后面的立方体透出来，差异显著。
     同时断言材质驱动的那次运行里 drawnTransparent > 0（真的进了透明队列）。

全程零 Vulkan 验证层报错。

⚠ 所有轮次都开 MYVK_EDITOR_NO_HUD（视口叠加层里有 FPS 这类每次都不同的
  数字）与 MYVK_LIGHT_GIZMOS=0（灯小球是自发光体，会搅浑像素比对）。
⚠ 像素比对一律**只统计视口矩形内**（diff_stats 的 vp 参数）：ImGui 面板
  在两次进程启动之间会有 1~2px 的排版抖动（实测最大差 29），不裁掉的话
  会把真信号淹掉。理由详见 verify_local_shadow.py 的 crop_pair。

用法：
    python tools/verify_shader_variants.py
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
from verify_add_menu import LogTap, kill_stale_editors, wait_rect   # noqa: E402
# diff_stats / to_np 复用局部阴影脚本里的那份：它带"裁到视口"的逻辑，
# 那份注释把"为什么要裁"讲清楚了，不适合在第二个脚本里再抄一遍。
from verify_local_shadow import diff_stats, to_np, vp_box           # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = os.path.join(ROOT, "..", "_venn_verify")

# 相机位置 (0,-34,15)、yaw=90（视线朝 +Y）、俯角 16°。
CAM = "0,-34,15,90,16"

# 掩码位（与 render/ShaderKeywords.h 的位序一致）
K_DOUBLE = 1 << 0
K_BLEND = 1 << 1
K_NORMAL = 1 << 2

# scene F 里各物体的 (名字, 位置, 尺寸/旋转, 材质特性) —— 期望掩码
# 写在 SCENE_F_EXPECT 里逐条对照，改场景时必须一起改。
HALF_PI = math.pi * 0.5

VAR_RE = re.compile(r"SHADER-VARIANTS fwd_created=(\d+) depth_created=(\d+) "
                    r"switches=(\d+) forced=(-?\d+) draws=(\S+)")
DRAWS_RE = re.compile(r"k(\d+)=(\d+)")
BUILT_RE = re.compile(r"SHADER-VARIANTS-BUILT (\w+) (.*?)\s*$")
CULL_RE = re.compile(r"CULL-STATS renderables=(\d+) culled=(\d+) "
                     r"drawnOpaque=(\d+) drawnTransparent=(\d+)")


# ============================================================
# 场景构造
# ============================================================
def material(name, base, roughness=0.55, alpha=1.0, double_sided=False,
             alpha_blend=False, checker_normal=None):
    """程序化材质。checker_normal 给一个缓存键名就挂一张非平坦棋盘法线图。"""
    m = {
        "kind": "procedural", "name": name,
        "baseColor": list(base) + [alpha],
        "metallic": 0.0, "roughness": roughness,
        "normalScale": 1.0, "ao": 1.0,
        "emissive": [0.0, 0.0, 0.0],
        "doubleSided": double_sided, "alphaBlend": alpha_blend,
    }
    if checker_normal:
        # 两个格子给两种不同的法线倾斜，扰动幅度大、看上去是明显的凹凸。
        # 只用于"法线图真的在起作用"那一组，不追求物理上正确的法线。
        m["normal"] = {
            "kind": "checker", "name": checker_normal,
            "colorA": [128, 128, 255, 255],
            "colorB": [220, 128, 255, 255],
            "cell": 24, "size": 256,
        }
    return m


def entity(name, pos, mesh, mat, rot=(0, 0, 0)):
    return {
        "name": name,
        "transform": {"position": list(pos), "rotation": list(rot),
                      "scale": [1, 1, 1]},
        "visible": True, "castShadow": True, "parent": -1,
        "mesh": {"kind": "builtin", "shape": mesh["shape"],
                 "size": mesh.get("size", 1.0), "name": mesh["name"]},
        "material": mat,
    }


def ground(size=80.0, mat_name="sv_ground_mat"):
    return entity("Ground", (0, 0, 0),
                  {"shape": "plane", "size": size, "name": "sv_ground"},
                  material(mat_name, (0.70, 0.70, 0.73)))


def write_scene(path, ents, generator, sun=(0.10, 0.10)):
    """写场景。太阳默认压暗 —— 让材质本身的明暗差别成为画面主要信号。"""
    doc = {
        "version": 1,
        "generator": generator,
        "light": {"direction": [-0.35, -0.5, -0.79], "color": [1.0, 0.97, 0.92],
                  "intensity": sun[0], "castsShadow": True,
                  "ambientScale": sun[1]},
        "entities": ents,
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)
    return ents


# scene F：一个场景里把所有 5 种掩码都用上，用来验"材质 → 变体"的路由。
#   (物体名, 期望掩码, 说明)
SCENE_F_EXPECT = [
    ("Ground", 0, "地面：不透明、不双面、没法线图"),
    ("CubePlain", 0, "普通立方体：与地面同掩码"),
    ("WallDoubleA", K_DOUBLE, "双面竖直板（+90°）"),
    ("WallDoubleB", K_DOUBLE, "双面竖直板（-90°）"),
    ("Glass", K_BLEND, "半透明挡板"),
    ("CubeNormal", K_NORMAL, "挂了棋盘法线图"),
    ("CubeBoth", K_DOUBLE | K_NORMAL, "双面 + 法线图"),
]
EXPECT_MASKS = {0, 1, 2, 4, 5}


def build_scene_features(path):
    ents = [ground(80.0, "sv_feat_ground_mat")]
    ents.append(entity("CubePlain", (-9.0, 3.0, 2.0),
                       {"shape": "cube", "size": 4.0, "name": "sv_cube_plain"},
                       material("sv_cube_plain_mat", (0.85, 0.45, 0.30))))
    ents.append(entity("WallDoubleA", (-17.0, 9.0, 5.0),
                       {"shape": "plane", "size": 10.0, "name": "sv_wall_a"},
                       material("sv_wall_a_mat", (0.35, 0.60, 0.85),
                                double_sided=True),
                       rot=(HALF_PI, 0, 0)))
    ents.append(entity("WallDoubleB", (17.0, 9.0, 5.0),
                       {"shape": "plane", "size": 10.0, "name": "sv_wall_b"},
                       material("sv_wall_b_mat", (0.35, 0.85, 0.55),
                                double_sided=True),
                       rot=(-HALF_PI, 0, 0)))
    ents.append(entity("Glass", (0.0, 6.0, 4.0),
                       {"shape": "plane", "size": 10.0, "name": "sv_glass"},
                       material("sv_glass_mat", (0.95, 0.95, 1.0),
                                alpha=0.30, alpha_blend=True),
                       rot=(HALF_PI, 0, 0)))
    ents.append(entity("CubeNormal", (9.0, 3.0, 2.0),
                       {"shape": "cube", "size": 4.0, "name": "sv_cube_normal"},
                       material("sv_cube_normal_mat", (0.85, 0.85, 0.85),
                                checker_normal="sv_normal_checker")))
    ents.append(entity("CubeBoth", (0.0, 16.0, 2.0),
                       {"shape": "cube", "size": 4.0, "name": "sv_cube_both"},
                       material("sv_cube_both_mat", (0.80, 0.80, 0.90),
                                double_sided=True,
                                checker_normal="sv_normal_checker")))
    return write_scene(path, ents, "verify_shader_variants.py", (0.35, 0.25))


def build_scene_planes(path):
    """地面 + 两块朝向相反的竖直单面板。没法线图、不透明 →
    只留 DOUBLE_SIDED 一个变量。同时用于第 2 组（等价）与第 3 组（双面）。"""
    ents = [ground(80.0, "sv_plane_ground_mat")]
    for nm, rot, col in (("Front", HALF_PI, (0.90, 0.50, 0.35)),
                         ("Back", -HALF_PI, (0.35, 0.55, 0.90))):
        ents.append(entity("Panel" + nm, (-14.0 + 28.0 * (nm == "Back"), 8.0, 5.0),
                           {"shape": "plane", "size": 12.0,
                            "name": "sv_panel_" + nm.lower()},
                           material("sv_panel_%s_mat" % nm.lower(), col),
                           rot=(rot, 0, 0)))
    return write_scene(path, ents, "verify_shader_variants.py", (0.25, 0.20))


def build_scene_normal(path):
    """地面 + 一个挂了棋盘法线图的立方体。没有双面、没有透明 →
    只留 NORMAL_MAP 一个变量（第 4 组）。"""
    ents = [ground(80.0, "sv_nm_ground_mat")]
    ents.append(entity("CubeNormal", (0.0, 6.0, 2.5),
                       {"shape": "cube", "size": 5.0, "name": "sv_nm_cube"},
                       material("sv_nm_cube_mat", (0.85, 0.85, 0.85),
                                checker_normal="sv_normal_checker")))
    return write_scene(path, ents, "verify_shader_variants.py", (0.45, 0.25))


def build_scene_blend(path):
    """地面 + 透明挡板 + 挡板后面的亮色立方体（第 5 组）。"""
    ents = [ground(80.0, "sv_bl_ground_mat")]
    ents.append(entity("Behind", (0.0, 9.0, 2.5),
                       {"shape": "cube", "size": 5.0, "name": "sv_bl_cube"},
                       material("sv_bl_cube_mat", (1.0, 0.35, 0.25))))
    ents.append(entity("Glass", (0.0, 2.0, 4.0),
                       {"shape": "plane", "size": 12.0, "name": "sv_bl_glass"},
                       material("sv_bl_glass_mat", (0.85, 0.95, 1.0),
                                alpha=0.30, alpha_blend=True),
                       rot=(HALF_PI, 0, 0)))
    return write_scene(path, ents, "verify_shader_variants.py", (0.35, 0.25))


# ============================================================
# 运行一轮
# ============================================================
def run_once(exe, scene, extra_env, shot_path, cam=CAM, wait=20.0):
    """跑一轮编辑器，返回 (日志行, 视口矩形, 截图, 错误行)。"""
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_ENV_DBG"] = "0"
    env["MYVK_LIGHT_GIZMOS"] = "0"       # 灯小球会搅浑像素比对
    env["MYVK_EDITOR_NO_HUD"] = "1"      # HUD 里的 FPS 每次都不同
    env["MYVK_SHADER_VARIANTS"] = "1"    # 变体自检日志
    env["MYVK_FRUSTUM_DBG"] = "1"        # 要读 CULL-STATS 的 drawnTransparent
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
        # SHADER-VARIANTS 也是 30 帧一条 —— 它出现就说明前向 Pass 已经跑过、
        # 本帧的变体统计完整了。等**两条**（第二条的第 2 个字段才有意义：
        # 第一条时 fwd_created 可能还没到稳态）。
        end = time.time() + wait
        while time.time() < end:
            with tap.lock:
                n = sum(1 for l in tap.lines if "SHADER-VARIANTS " in l)
            if n >= 2:
                break
            time.sleep(0.25)
        time.sleep(1.2)

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


def all_variants(lines):
    """所有 SHADER-VARIANTS 记录，按出现顺序。"""
    out = []
    for line in lines:
        m = VAR_RE.search(line)
        if m:
            out.append({
                "fwd_created": int(m.group(1)),
                "depth_created": int(m.group(2)),
                "switches": int(m.group(3)),
                "forced": int(m.group(4)),
                "draws": {int(k): int(v)
                          for k, v in DRAWS_RE.findall(m.group(5))},
            })
    return out


def built_sets(lines):
    """SHADER-VARIANTS-BUILT → {'fwd': {mask: '名字'}, 'depth': {...}}"""
    out = {"fwd": {}, "depth": {}}
    for line in lines:
        m = BUILT_RE.search(line)
        if not m:
            continue
        which = m.group(1)
        for idx, names in re.findall(r"k(\d+)\[([^\]]*)\]", m.group(2)):
            out.setdefault(which, {})[int(idx)] = names
    return out


def cull_stats(lines):
    m = None
    for line in lines:
        mm = CULL_RE.search(line)
        if mm:
            m = mm
    if not m:
        return None
    return {"renderables": int(m.group(1)), "transparent": int(m.group(4))}


# ============================================================
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
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

    # ---------------- 第 1 组：材质特性 → 变体路由 ----------------
    print("\n== 第 1 组：材质特性 → 变体路由 ==")
    scene_f = os.path.join(OUT_DIR, "shader_variants_features.json")
    build_scene_features(scene_f)
    lines_f, vp_f, img_f, errs_f = run_once(
        exe, scene_f, {}, os.path.join(OUT_DIR, "sv_features.png"))
    check("零 Vulkan 验证层报错", not errs_f, errs_f[:2])
    if not vp_f:
        print("  没等到 VP-RECT")
        return 2

    vs = all_variants(lines_f)
    check("拿到 SHADER-VARIANTS 日志", bool(vs), len(vs))
    if not vs:
        print("\n结果: FAIL（没有变体自检日志）")
        return 3

    first, last = vs[0], vs[-1]
    print("     fwd_created=%d depth_created=%d switches=%d draws=%s"
          % (last["fwd_created"], last["depth_created"], last["switches"],
             ",".join("k%d=%d" % kv for kv in sorted(last["draws"].items()))))

    used = set(last["draws"].keys())
    check("用到的掩码集合 = {0,1,2,4,5}（双面 / 透明 / 法线图各一位）",
          used == EXPECT_MASKS, sorted(used))
    check("前向变体族恰好建了 5 个（每用到的组合一个，不多建）",
          last["fwd_created"] == 5, last["fwd_created"])
    check("深度预通道只按 DOUBLE_SIDED 分（2 个），不为颜色关键字多建",
          last["depth_created"] == 2, last["depth_created"])
    check("没有强制掩码（forced = -1）", last["forced"] == -1, last["forced"])

    # 缓存生效：日志后半段的 fwd_created 必须与第一条**完全一致**。
    # 一直涨说明每次绘制都在建新管线（缓存根本没用上）。
    check("变体缓存生效：跑了很多帧后 fwd_created 不再增长",
          first["fwd_created"] == last["fwd_created"],
          "%d -> %d" % (first["fwd_created"], last["fwd_created"]))

    built = built_sets(lines_f)
    print("     BUILT fwd  : %s"
          % " ".join("k%d[%s]" % kv for kv in sorted(built["fwd"].items())))
    print("     BUILT depth: %s"
          % " ".join("k%d[%s]" % kv for kv in sorted(built["depth"].items())))
    check("BUILT 里列出的是人可读的关键字名",
          built["fwd"].get(5) == "double-sided|normal-map",
          built["fwd"].get(5))
    check("BUILT fwd 的掩码集合与 draws 一致",
          set(built["fwd"].keys()) == used, sorted(built["fwd"].keys()))
    check("BUILT depth 只有 0 / 1 两个（双面那一位）",
          set(built["depth"].keys()) == {0, 1},
          sorted(built["depth"].keys()))

    cs = cull_stats(lines_f)
    check("材质驱动下确实有物体进了透明队列", cs and cs["transparent"] > 0,
          cs["transparent"] if cs else "没有 CULL-STATS")

    # 逐个物体验掩码：draws 里的计数必须与场景期望一致。
    expect_counts = {}
    for _, mask, _ in SCENE_F_EXPECT:
        expect_counts[mask] = expect_counts.get(mask, 0) + 1
    got_counts = {k: v for k, v in last["draws"].items()}
    check("每个掩码的绘制数与场景逐条对上（含两块双面挡板）",
          got_counts == expect_counts,
          "期望 %s，实测 %s" % (sorted(expect_counts.items()),
                                sorted(got_counts.items())))

    # ---------------- 第 2 组 + 第 3 组：竖直单面板场景 ----------------
    print("\n== 第 2 组：NORMAL_MAP 编掉的路径 ≡ 绑平坦法线贴图的路径 ==")
    scene_p = os.path.join(OUT_DIR, "shader_variants_planes.json")
    build_scene_planes(scene_p)
    runs = {}
    for mask, tag in ((0, "off"), (4, "nm"), (1, "ds")):
        lines_p, vp_p, img_p, errs_p = run_once(
            exe, scene_p, {"MYVK_SHADER_FORCE_MASK": str(mask)},
            os.path.join(OUT_DIR, "sv_planes_%s.png" % tag))
        check("FORCE_MASK=%d 零 Vulkan 验证层报错" % mask, not errs_p,
              errs_p[:2])
        vp = all_variants(lines_p)
        check("FORCE_MASK=%d 只建了 1 个前向变体" % mask,
              bool(vp) and vp[-1]["fwd_created"] == 1,
              vp[-1]["fwd_created"] if vp else "无日志")
        check("FORCE_MASK=%d 日志里的 forced 字段回读正确" % mask,
              bool(vp) and vp[-1]["forced"] == mask,
              vp[-1]["forced"] if vp else "无日志")
        runs[mask] = (img_p, vp_p)

    img_off, vp_off = runs[0]
    img_nm, _ = runs[4]
    img_ds, _ = runs[1]

    if img_off is not None and img_nm is not None:
        n, mx, mean, box = diff_stats(img_off, img_nm, vp_off)
        print("     关 NORMAL_MAP vs 开 NORMAL_MAP（绑平坦法线）："
              "差异像素(>18)=%d 最大差=%d 均值=%.4f" % (n, mx, mean))
        # 核心断言：没有像素被"肉眼可见地"改动。
        check("两条路径**没有可见差异**（>18 的像素数为 0）", n == 0, n)
        # 再钉一层：连逐像素的最大差都只有 1 个 LSB。
        # 为什么不是 0：内置平坦法线是 8 位的 (128,128,255)，
        # 128/255*2-1 = 0.00392 而非精确的 0 —— 于是"走 TBN 那一路"与
        # "直接用插值法线那一路"差一个极小的水平倾斜。这是换来的：
        # 不建法线贴图资源、也不用为浮点纹理加一个内部格式分支。
        check("连逐像素最大差都 <= 1 个 LSB（等价，不是'差不多'）", mx <= 1, mx)
        # 均值只用来排除"整体亮度偏移"这种伪信号。逐像素都 <= 1 时，
        # 均值天然很小（但不会是 0，因为差异像素多、每个差 1）。
        check("平均差 < 0.5（排除整体亮度偏移这种伪信号）", mean < 0.5,
              "%.4f" % mean)
        check("差异包围盒为空（没有任何区域被改动）", box is None, box)

    print("\n== 第 3 组：DOUBLE_SIDED 真的改了栅格化状态 ==")
    if img_off is not None and img_ds is not None:
        n, mx, mean, box = diff_stats(img_off, img_ds, vp_off)
        print("     关双面 vs 开双面：差异像素=%d 最大差=%d 均值=%.4f bbox=%s"
              % (n, mx, mean, box))
        check("开双面后背面被画出来了（差异像素 > 2000 且最大差 > 60）",
              n > 2000 and mx > 60, "n=%d max=%d" % (n, mx))
        if box and vp_off:
            x0, y0, x1, y1 = vp_box(vp_off)
            print("     差异带: 宽 %d px（视口的 %.1f%%）"
                  % (box[2] - box[0], 100.0 * (box[2] - box[0]) /
                     max(x1 - x0, 1)))
            check("差异落在视口内（不是面板抖动）",
                  box[0] >= x0 - 2 and box[2] <= x1 + 2 and
                  box[1] >= y0 - 2, box)

    # ---------------- 第 4 组：NORMAL_MAP 不是死代码 ----------------
    print("\n== 第 4 组：NORMAL_MAP 真的在扰动法线（不是死代码）==")
    scene_n = os.path.join(OUT_DIR, "shader_variants_normal.json")
    build_scene_normal(scene_n)
    nm_runs = {}
    for mask in (0, 4):
        lines_n, vp_n, img_n, errs_n = run_once(
            exe, scene_n, {"MYVK_SHADER_FORCE_MASK": str(mask)},
            os.path.join(OUT_DIR, "sv_normal_%d.png" % mask))
        check("法线图场景 FORCE_MASK=%d 零 Vulkan 验证层报错" % mask,
              not errs_n, errs_n[:2])
        nm_runs[mask] = (img_n, vp_n)
    img_n0, vp_n0 = nm_runs[0]
    img_n4, _ = nm_runs[4]
    if img_n0 is not None and img_n4 is not None:
        n, mx, mean, box = diff_stats(img_n0, img_n4, vp_n0)
        print("     关法线图 vs 开法线图（材质**挂了**棋盘法线图）："
              "差异像素=%d 最大差=%d 均值=%.4f bbox=%s" % (n, mx, mean, box))
        # 阈值说明：太阳压到 0.45（避免过曝）后，法线扰动 ±0.7 在漫反射上
        # 只换来约 30/255 的亮度变化 —— 所以这里卡 20 而不是 40。
        # 与第 2 组的 max = 1 相比仍有 20 倍余量，"有信号 / 无信号"分得清。
        check("挂了法线图时开/关会有显著差异（否则法线图是死代码）",
              n > 1500 and mx > 20, "n=%d max=%d" % (n, mx))
        if box and vp_n0:
            x0, y0, x1, y1 = vp_box(vp_n0)
            check("差异落在视口内（不是 ImGui 面板抖动）",
                  box[0] >= x0 - 2 and box[2] <= x1 + 2 and
                  box[1] >= y0 - 2, box)

    # ---------------- 第 5 组：ALPHA_BLEND 的路由与混合 ----------------
    print("\n== 第 5 组：ALPHA_BLEND 的路由与混合 ==")
    scene_b = os.path.join(OUT_DIR, "shader_variants_blend.json")
    build_scene_blend(scene_b)
    bl_runs = {}
    for mask in (0, 2):
        lines_b, vp_b, img_b, errs_b = run_once(
            exe, scene_b, {"MYVK_SHADER_FORCE_MASK": str(mask)},
            os.path.join(OUT_DIR, "sv_blend_%d.png" % mask))
        check("透明场景 FORCE_MASK=%d 零 Vulkan 验证层报错" % mask,
              not errs_b, errs_b[:2])
        bl_runs[mask] = (img_b, vp_b, cull_stats(lines_b))
    img_b0, vp_b0, cs_b0 = bl_runs[0]
    img_b2, vp_b2, _ = bl_runs[2]
    # 透明挡板是**材质驱动**的：把强制掩码设成 0 时它会被判成不透明 →
    # 不该再进透明队列。这一条同时钉住"队列分类由掩码推导"。
    check("FORCE_MASK=0 时没有物体进透明队列（分类确实跟着掩码走）",
          cs_b0 is not None and cs_b0["transparent"] == 0,
          cs_b0["transparent"] if cs_b0 else "没有 CULL-STATS")
    if img_b0 is not None and img_b2 is not None:
        n, mx, mean, box = diff_stats(img_b0, img_b2, vp_b0)
        print("     不透明 vs 半透明：差异像素=%d 最大差=%d 均值=%.4f bbox=%s"
              % (n, mx, mean, box))
        check("半透明挡板让后面的立方体透出来（差异像素 > 1500）",
              n > 1500, n)
        check("差异有实际形貌（最大差 > 40，不是整体微调）", mx > 40, mx)

    print("\n结果: %s" % ("PASS" if not fails else "FAIL: " + ", ".join(fails)))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
