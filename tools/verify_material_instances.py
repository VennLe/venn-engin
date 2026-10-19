"""端到端验证：材质身份 / 材质实例（W0 + W1，见 docs/world_scale_roadmap.md）。

断言的四件事（对应路线图的 W0-1 / W0-2 / W0-3 / W1）：

  A 组（无窗口、无设备）
    A1 `Game.exe --selftest material` 全绿，且条数 >= 15
       —— 实例的"跟随 / 不跟随"、位序冻结、只存覆盖项，都是纯数据断言。

  B 组（无窗口，但要 GPU：真的加载模型 / 建材质）
    B1 **W0-1** 同名材质不再静默合并
       探针 `mat_conflict_probe.json` 的两个 Box 共用材质名 `shared_mat`，
       参数不同 → dump 出 **两行不同的 roughness**（0.20 / 0.80），
       且日志恰好一条 `ASSET-MAT-COLLIDE`。
       ⚠ 修复前这里是两行都是 0.20，而且**一个 WARN 都没有** ——
         这就是"Ctrl+D 复制实体然后材质串味"的根因。
    B2 **W0-2** 材质身份是 `模型路径#glTF材质名`，不是数组下标
       `mat_model_probe.json` 加载 anime_girl.glb → 7 条 `ASSET-MAT-ID`，
       7 个 id 与 glTF 里的 7 个材质名（Skin/Hair/Cloth/EyeWhite/Iris/Shoe/Lash）
       一一对应，且**没有任何一个 id 含 `#mat<数字>`**（那是下标的形状）。
    B3 **W0-3** 导入模型的材质覆盖能存能读
       同一个探针给了 `overrides: {roughness: 0.05, baseColor: [1,0,0,1]}` →
       dump 出来 `kind:"model"` + 同样的两个覆盖值。
       ⚠ 修复前 dump 出来是 `material: null`（整个材质块消失，物体走默认白）。
    B4 **W0-3 的守卫**：材质 id 找不到时**不能**返回 null
       故意写一个不存在的 id → 日志有 `ASSET-MAT-MISS`，实体**仍在**
       （`skipped=0`），材质块是一个 `#missing` 占位而不是 null。
       ⚠ 返回 null 会让实体掉出渲染列表 —— 物体凭空消失，一个日志都没有。
    B5 **W1-4** 实例的序列化只写 `parent` + 被覆盖的那几个字段
       场景往返后 child 仍是 `kind:"instance"`，`overrides` 里**只有**
       roughness（不含 metallic/baseColor 等未覆盖字段）。
       ⚠ 存全量会让"母材质改了而实例没跟着变"，因为读回来每一个字段都
         "值 ≠ 母材质" → 全被当成覆盖（路线图 D2）。

  C 组（编辑器，需要窗口）
    C1 Tool/Material 页签上 `INS-MAT-INSTANCE` 矩形存在（"Material (asset)" 那一行）
    C2 `INS-MAT-MAKEUNIQUE` 按钮矩形存在
    C3 真的点它 → 日志 `MATERIAL-INSTANCE action=make-unique`
       + 之后 `INS-MAT-INSTANCE` 那一行变成 "Instance of ..."（可再次读到矩形）

用法：
    python tools/verify_material_instances.py
    python tools/verify_material_instances.py --no-ui    # 只跑 A/B 组
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import find_window, user32                       # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors               # noqa: E402
from verify_asset_drag import client_to_screen, force_foreground     # noqa: E402
from safe_fs import safe_remove

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
GAME = os.path.join(BIN, "Game.exe")
EDITOR = os.path.join(BIN, "Editor.exe")
SCENES = os.path.join(BIN, "assets", "scenes")
OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify")

MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004

MAT_ID_RE = re.compile(r"ASSET-MAT-ID model=(\S+) key=(\S+) id=(\S+)")
MAT_COLLIDE_RE = re.compile(r"ASSET-MAT-COLLIDE name=(\S+) renamed=(\S+)")
MAT_MISS_RE = re.compile(r"ASSET-MAT-MISS id=(\S*) name=(\S*) reason=(\S+)")
MAT_INST_RE = re.compile(
    r"MATERIAL-INSTANCE action=make-unique entity=(\d+) parent=(\S+) name=(\S+)")

GLTF_MAT_NAMES = ["Skin", "Hair", "Cloth", "EyeWhite", "Iris", "Shoe", "Lash"]

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


def run(exe, args, timeout=120):
    env = dict(os.environ)
    env.pop("MYVK_ASSET_DIR", None)
    # 别让开发机上残留的编辑器环境变量影响运行时用例
    for k in list(env):
        if k.startswith("MYVK_EDITOR_") or k.startswith("MYVK_RUNTIME_"):
            env.pop(k, None)
    try:
        p = subprocess.run([exe] + list(args), cwd=BIN, env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, encoding="utf-8", errors="replace",
                           timeout=timeout)
    except subprocess.TimeoutExpired:
        return -1, []
    return p.returncode, p.stdout.splitlines()


def dump_scene(scene_rel, out_name, extra_env=None):
    """跑一小段然后 --dump-scene，返回 (lines, dump_dict|None)。"""
    out_abs = os.path.join(SCENES, out_name)
    if os.path.exists(out_abs):
        safe_remove(out_abs)
    args = ["--scene", scene_rel, "--dump-scene", out_abs, "--frames", "6"]
    rc, lines = run(GAME, args)
    data = None
    if os.path.exists(out_abs):
        try:
            with open(out_abs, encoding="utf-8") as f:
                data = json.load(f)
        except Exception:
            data = None
        safe_remove(out_abs)
    return rc, lines, data


def ents_by_name(dump):
    out = {}
    for e in (dump or {}).get("entities", []):
        out[e.get("name", "")] = e
    return out


# --------------------------------------------------------------- B 组场景

INSTANCE_SCENE = {
    "version": 1,
    "generator": "verify_material_instances: W1-4 instance round-trip",
    "entities": [
        {
            "name": "Parent",
            "transform": {"position": [-2.0, 0.0, 0.5], "rotation": [0, 0, 0],
                          "scale": [1, 1, 1]},
            "parent": -1, "visible": True, "castShadow": True,
            "mesh": {"kind": "builtin", "shape": "cube", "size": 1.0,
                     "name": "cube"},
            "primitive": {"kind": "cube", "size": 1.0},
            "material": {
                "kind": "procedural", "name": "mat_inst_parent",
                "baseColor": [0.5, 0.5, 0.5, 1.0], "metallic": 0.1,
                "roughness": 0.5, "normalScale": 1.0, "ao": 1.0,
                "emissive": [0.0, 0.0, 0.0], "doubleSided": False,
            },
        },
        {
            "name": "Child",
            "transform": {"position": [2.0, 0.0, 0.5], "rotation": [0, 0, 0],
                          "scale": [1, 1, 1]},
            "parent": -1, "visible": True, "castShadow": True,
            "mesh": {"kind": "builtin", "shape": "cube", "size": 1.0,
                     "name": "cube"},
            "primitive": {"kind": "cube", "size": 1.0},
            "material": {
                "kind": "instance", "name": "mat_inst_child",
                "parent": "mat_inst_parent",
                "overrides": {"roughness": 0.9},
            },
        },
    ],
}

MISS_SCENE = {
    "version": 1,
    "generator": "verify_material_instances: W0-3 placeholder guard",
    "entities": [
        {
            "name": "Ghost",
            "transform": {"position": [0.0, 0.0, 0.5], "rotation": [0, 0, 0],
                          "scale": [1, 1, 1]},
            "parent": -1, "visible": True, "castShadow": True,
            "mesh": {"kind": "builtin", "shape": "cube", "size": 1.0,
                     "name": "cube"},
            "primitive": {"kind": "cube", "size": 1.0},
            "material": {
                "kind": "model", "name": "models/nope.glb/mat0",
                "modelKey": "models/nope.glb", "id": "models/nope.glb#Nope",
                "overrides": {"roughness": 0.31},
            },
        },
    ],
}


def write_temp_scenes():
    paths = []
    for name, data in (("_vmi_instance.json", INSTANCE_SCENE),
                       ("_vmi_miss.json", MISS_SCENE)):
        p = os.path.join(SCENES, name)
        with open(p, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2, ensure_ascii=False)
        paths.append(p)
    return paths


# ------------------------------------------------------------------ 组 A

def group_a():
    print("\n== A 组：无窗口自检 ==")
    rc, lines = run(GAME, ["--selftest", "material"])
    total = [l for l in lines if "MAT-TOTAL" in l]
    ok = bool(total) and "fail=0" in total[-1]
    m = re.search(r"pass=(\d+)", total[-1]) if total else None
    n = int(m.group(1)) if m else 0
    check("A1 --selftest material 全绿（pass>=15）", rc == 0 and ok and n >= 15,
          total[-1] if total else "no MAT-TOTAL")
    # 反向：不能是"一条断言都没跑"也算绿
    check("A2 自检真的跑了至少 15 条断言", n >= 15, "pass=%d" % n)


# ------------------------------------------------------------------ 组 B

def group_b():
    print("\n== B 组：场景往返（无窗口，需要 GPU）==")

    # --- B1 同名材质冲突 ---
    rc, lines, data = dump_scene("scenes/runtime_test/mat_conflict_probe.json",
                                 "_vmi_out.json")
    ents = ents_by_name(data)
    a = (ents.get("A") or {}).get("material") or {}
    b = (ents.get("B") or {}).get("material") or {}
    ra, rb = a.get("roughness"), b.get("roughness")
    collide = [l for l in lines if MAT_COLLIDE_RE.search(l)]
    check("B1 同名材质不再静默合并（两行 roughness 不同）",
          ra is not None and rb is not None and abs(ra - 0.2) < 1e-3
          and abs(rb - 0.8) < 1e-3,
          "A=%s B=%s" % (ra, rb))
    check("B2 冲突时恰好一条 ASSET-MAT-COLLIDE", len(collide) == 1,
          collide[0].split("] ")[-1] if collide else "none")

    # --- B3 材质稳定身份 ---
    rc, lines, _ = dump_scene("scenes/runtime_test/mat_model_probe.json",
                              "_vmi_out2.json")
    ids = []
    for l in lines:
        m = MAT_ID_RE.search(l)
        if m and m.group(1) == "models/anime_girl.glb":
            ids.append(m.group(3))
    want = {"models/anime_girl.glb#" + n for n in GLTF_MAT_NAMES}
    check("B3 7 个材质 id 与 glTF 材质名一一对应", set(ids) == want,
          "got=%d want=%d" % (len(set(ids)), len(want)))
    check("B4 材质 id 里没有数组下标的形状（#mat<数字>）",
          not any(re.search(r"#mat\d+$", i) for i in ids),
          ",".join(sorted(ids))[:90])

    # --- B5 导入模型的材质覆盖能存能读 ---
    ents2 = ents_by_name(dump_scene("scenes/runtime_test/mat_model_probe.json",
                                    "_vmi_out3.json")[2])
    g = (ents2.get("Girl") or {}).get("material") or {}
    ov = g.get("overrides") or {}
    check("B5 模型材质的覆盖能存能读（kind=model + 两个覆盖字段）",
          g.get("kind") == "model"
          and g.get("id") == "models/anime_girl.glb#Skin"
          and abs(ov.get("roughness", -1) - 0.05) < 1e-4
          and (ov.get("baseColor") or [0])[0] == 1.0,
          json.dumps(g, ensure_ascii=False)[:110])

    # --- B6 找不到材质时的占位守卫 ---
    rc, lines, data = dump_scene("scenes/_vmi_miss.json", "_vmi_out4.json")
    miss = [l for l in lines if MAT_MISS_RE.search(l)]
    ents3 = ents_by_name(data)
    ghost = ents3.get("Ghost") or {}
    gmat = ghost.get("material")
    check("B7 id 找不到时打 ASSET-MAT-MISS", len(miss) >= 1,
          miss[0].split("] ")[-1] if miss else "none")
    check("B8 id 找不到时物体**不消失**（仍有材质块，不是 null）",
          isinstance(gmat, dict) and gmat.get("name", "").endswith("#missing"),
          json.dumps(gmat, ensure_ascii=False)[:90])
    check("B9 占位材质上的覆盖也能往返",
          isinstance(gmat, dict)
          and abs(((gmat.get("overrides") or {}).get("roughness", -1)) - 0.31)
          < 1e-4,
          json.dumps(gmat.get("overrides"), ensure_ascii=False)
          if isinstance(gmat, dict) else "n/a")

    # --- B10 实例的序列化只写覆盖项 ---
    rc, lines, data = dump_scene("scenes/_vmi_instance.json", "_vmi_out5.json")
    ents4 = ents_by_name(data)
    child = (ents4.get("Child") or {}).get("material") or {}
    ovc = child.get("overrides") or {}
    check("B10 实例往返后仍是 kind=instance + parent 可解析",
          child.get("kind") == "instance"
          and child.get("parent") == "mat_inst_parent",
          json.dumps(child, ensure_ascii=False)[:110])
    check("B11 实例只写被覆盖的字段（不含 metallic/baseColor）",
          set(ovc.keys()) == {"roughness"}
          and abs(ovc.get("roughness", 0) - 0.9) < 1e-4,
          "overrides=" + json.dumps(ovc, ensure_ascii=False))


# ------------------------------------------------------------------ 组 C

def group_c():
    print("\n== C 组：编辑器（需要窗口）==")
    kill_stale_editors()
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_EDITOR_SCENE"] = "scenes/sandbox.json"
    env["MYVK_EDITOR_SELECT"] = "BoxA"
    env["MYVK_EDITOR_INSP_TAB"] = "material"
    env["MYVK_EDITOR_NO_HUD"] = "1"
    proc = subprocess.Popen([EDITOR], cwd=BIN, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            stdin=subprocess.DEVNULL,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    try:
        hwnd = find_window("Venn Editor", 15.0)
        if not hwnd:
            check("C0 编辑器窗口出现", False, "no window")
            return
        user32.ShowWindow(hwnd, 9)
        time.sleep(2.0)
        user32.SetWindowPos(hwnd, 0, 0, 0, 1600, 900, 0x0002 | 0x0004)
        time.sleep(2.0)
        force_foreground(hwnd)
        time.sleep(1.0)

        inst = tap.last_rect("INS-MAT-INSTANCE")
        mk = tap.last_rect("INS-MAT-MAKEUNIQUE")
        check("C1 INS-MAT-INSTANCE 存在（材质身份那一行）", rect_ok(inst),
              str(inst))
        check("C2 INS-MAT-MAKEUNIQUE 按钮存在", rect_ok(mk), str(mk))
        if not (rect_ok(inst) and rect_ok(mk)):
            return

        # 点 Make Unique
        before = grab(hwnd)
        cx = (mk[0] + mk[2]) // 2
        cy = (mk[1] + mk[3]) // 2
        sx, sy = client_to_screen(hwnd, cx, cy)
        user32.SetCursorPos(sx, sy)
        time.sleep(0.4)
        user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
        time.sleep(0.10)
        user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
        time.sleep(1.2)

        made = [l for l in list(tap.lines) if MAT_INST_RE.search(l)]
        check("C3 点 Make Unique → MATERIAL-INSTANCE action=make-unique",
              len(made) >= 1, made[-1].split("] ")[-1] if made else "none")
        # 之后那一行文字应该从 "Material (asset)" 变成 "Instance of ..."
        # ⚠ 用**显著变化像素占比**而不是整图平均差：那一行只有 18px 高，
        #   平均差会被周围空白稀释到看不出（MEMORY 里的像素断言纪律）。
        after = grab(hwnd)
        ratio = sig_ratio(before, after, inst)
        check("C4 点完之后材质行文字真的变了（Instance of ...）",
              ratio > 0.05, "sig=%.3f%%" % (ratio * 100.0))
    finally:
        try:
            user32.PostMessageW(hwnd, 0x0010, 0, 0)   # WM_CLOSE
        except Exception:
            pass
        time.sleep(0.8)
        try:
            proc.terminate()
        except Exception:
            pass
        try:
            proc.wait(timeout=8)
        except Exception:
            proc.kill()
        kill_stale_editors()


# ------------------------------------------------------------------ 组 D
#
# W2-1：材质资产（`.vmat`）。母材质住在独立文件里 → 多个物体/多个关卡共享它，
# "改一次 .vmat 全都变"。三件事必须一起成立，缺一条都算没做完：
#   ① 引用同一个 .vmat 的两个物体**只解析一次**（日志恰好一条 ASSET-VMAT）；
#   ② dump 出来是 `kind:"vmat"` + path + 共享 id（不是把参数烤进场景）；
#   ③ 改 .vmat 的参数，**两个物体都变**（这条才能证明参数真的被应用了 ——
#      否则"id 对了但颜色没生效"也能在前两条里全绿）。

SCENE_VMAT = "scenes/runtime_test/vmat_ref_probe.json"
VMAT_REL = os.path.join("assets", "materials", "crate.vmat")
VMAT_RE = re.compile(r"ASSET-VMAT path=(\S+) id=(\S+) name=(\S+)")
VMAT_TITLE = "VennVMatTest"


def capture_runtime(title, scene_rel, settle=3.2):
    """起一个**带窗口**的运行时，摆到固定位置，抓一张图后关掉。

    ⚠ 显式 SetWindowPos 到 (0,0)+固定尺寸：Win32 的级联摆放会让两次启动的
      客户区差几十像素，两次截图直接对不齐（harness 两类抖动的第二类）。

    ⚠⚠ stdout 必须**落文件**，不能用 PIPE。不读的 PIPE 会被子进程写满
      （Windows 匿名管道默认只有几 KB，运行时光启动就写 5.4 KB）→ 子进程
      卡在 write 上，连窗口都没建出来，`find_window` 空转 20 秒返回 0。
      表现为"抓图全是 None、差异全是 0.000%"，看不出跟窗口有关。
    """
    env = dict(os.environ)
    for k in list(env):
        if k.startswith("MYVK_"):
            env.pop(k, None)
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    log_path = os.path.join(OUT_DIR, "_vmat_runtime.log")
    logf = open(log_path, "w+", encoding="utf-8")
    proc = subprocess.Popen([GAME, "--title", title, "--scene", scene_rel],
                            cwd=BIN, env=env, stdout=logf,
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
        time.sleep(settle)
        img = grab(hwnd)
    finally:
        try:
            user32.PostMessageW(hwnd, 0x0010, 0, 0)   # WM_CLOSE
        except Exception:
            pass
        time.sleep(0.5)
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


def set_vmat_basecolor(path, rgb):
    with open(path, encoding="utf-8") as f:
        doc = json.load(f)
    doc["baseColor"] = [rgb[0], rgb[1], rgb[2], 1.0]
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, indent=2)


def diff_halves(a, b, thresh=8):
    """整图按竖直中线切两半，各自算"显著变化像素占比"。

    两个立方体摆在 x=±2，屏幕中线大致是世界原点 → 左半动 = 左边那个变了，
    右半动 = 右边那个变了。**不许只看整图**：只改一个也能让整图有差异。
    """
    if a is None or b is None:
        return 0.0, 0.0
    W, H = a.size
    return (sig_ratio(a, b, (0, 0, W // 2, H), thresh),
            sig_ratio(a, b, (W // 2, 0, W, H), thresh))


def group_d():
    print("\n== D 组：材质资产 .vmat（W2-1，需要窗口才能验第 ③ 条）==")

    # ---- D1/D2：解析一次 + 写回形状 ----
    rc, lines, dump = dump_scene(SCENE_VMAT, "_vmi_vmat_out.json")
    vmat_lines = [l for l in lines if "ASSET-VMAT" in l]
    check("D1 两个物体引用同一个 .vmat → 恰好解析一次（1 条 ASSET-VMAT）",
          len(vmat_lines) == 1,
          "%d 条: %s" % (len(vmat_lines),
                        vmat_lines[0].split("] ")[-1] if vmat_lines else "none"))

    ents = ents_by_name(dump)
    a, b = ents.get("CrateA"), ents.get("CrateB")
    ok_shape = True
    detail = []
    for nm, e in (("CrateA", a), ("CrateB", b)):
        m = (e or {}).get("material") or {}
        good = (m.get("kind") == "vmat"
                and m.get("path") == "materials/crate.vmat"
                and m.get("id") == "materials/crate")
        ok_shape = ok_shape and good
        detail.append("%s=%s" % (nm, json.dumps(m, ensure_ascii=False)))
    check("D2 dump 里两个都是 kind=vmat + path + 同一个 id",
          ok_shape and a is not None and b is not None, "; ".join(detail))

    # ---- D3/D4：改 .vmat，两个都得变 ----
    vmat_abs = os.path.join(BIN, VMAT_REL)
    if not os.path.exists(vmat_abs):
        check("D3 改 .vmat 后两个物体都变", False, "找不到 " + vmat_abs)
        return
    with open(vmat_abs, "rb") as f:
        original = f.read()
    try:
        # 基线：**同一个** .vmat 连开两次。两次启动如果本身就有像素差，
        # 后面"改颜色"的差异就不能直接拿来当证据（自校准）。
        base_a = capture_runtime(VMAT_TITLE, SCENE_VMAT)
        base_b = capture_runtime(VMAT_TITLE, SCENE_VMAT)
        bl, br = diff_halves(base_a, base_b)
        baseline = max(bl, br)

        set_vmat_basecolor(vmat_abs, (0.02, 0.02, 0.85))   # 棕 → 蓝
        tinted = capture_runtime(VMAT_TITLE, SCENE_VMAT)
        tl, tr = diff_halves(base_b, tinted)

        # 判据：两侧都显著变化，且都比基线大一个量级。
        # 只要求"有一侧变了"是假断言 —— 只有一个物体跟着材质走也能通过。
        ok = (tl > max(0.005, baseline * 10.0)
              and tr > max(0.005, baseline * 10.0))
        check("D3 改 .vmat 的 baseColor → 左半与右半**都**变",
              ok, "left=%.3f%% right=%.3f%% 基线=%.3f%%"
                  % (tl * 100.0, tr * 100.0, baseline * 100.0))

        # 两侧变化量级相近才说明"两个物体同等程度地跟着材质走"；
        # 差距过大更像是只有一边响应（另一边可能压根用的是别的材质）。
        lo, hi = min(tl, tr), max(tl, tr)
        check("D4 两侧变化量级相近（两边都真的在跟）",
              hi > 0 and lo / hi > 0.2,
              "left=%.3f%% right=%.3f%% 比值=%.2f"
              % (tl * 100.0, tr * 100.0, (lo / hi) if hi else 0.0))
    finally:
        with open(vmat_abs, "wb") as f:
            f.write(original)


def rect_ok(r):
    return r is not None and r[2] - r[0] > 2 and r[3] - r[1] > 2


def grab(hwnd):
    from capture_window import capture_client
    img, ok = capture_client(hwnd)   # ⚠ 返回的是 (Image, ok) 元组
    return img if ok else None


def sig_ratio(a, b, box, thresh=8):
    """box 内"变化显著的像素占比"（0..1）。

    ⚠ 不用整图/区域平均差：一行 18px 高的文字变化被周围空白一稀释，
      平均差只有零点几/255，阈值根本选不出来。占比就没有这个问题。
    """
    if a is None or b is None or box is None:
        return 0.0
    try:
        from PIL import ImageChops
    except ImportError:
        return 0.0
    x0, y0, x1, y1 = [int(v) for v in box]
    W, H = a.size
    x0, y0 = max(x0, 0), max(y0, 0)
    x1, y1 = min(x1, W), min(y1, H)
    if x1 - x0 < 2 or y1 - y0 < 2:
        return 0.0
    ca = a.crop((x0, y0, x1, y1)).convert("RGB")
    cb = b.crop((x0, y0, x1, y1)).convert("RGB")
    diff = ImageChops.difference(ca, cb).convert("L")
    hist = diff.histogram()
    changed = sum(hist[thresh + 1:])
    total = max((x1 - x0) * (y1 - y0), 1)
    return changed / float(total)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-ui", action="store_true",
                    help="跳过 C/D 组（不开窗口）")
    args = ap.parse_args()

    os.makedirs(OUT_DIR, exist_ok=True)
    for p in (GAME, EDITOR):
        if not os.path.exists(p):
            print("找不到可执行文件，请先构建：", p)
            return 1

    temps = write_temp_scenes()
    try:
        group_a()
        group_b()
        if not args.no_ui:
            group_c()
            group_d()
    finally:
        for p in temps:
            try:
                safe_remove(p)
            except OSError:
                pass
        kill_stale_editors()

    print("\n结果: %s  (pass=%d fail=%d)"
          % ("PASS" if g_fail == 0 else "FAIL", g_pass, g_fail))
    return 0 if g_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
