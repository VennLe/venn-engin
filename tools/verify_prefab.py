"""端到端验证：G18 预制体（`.vprefab.json` / 实例化 / 覆盖 / Apply / Revert /
改母版 → 所有实例更新）。对应任务 #315。

背景（需求原文）：
    "Prefab（全仓 prefab 0 命中）：.prefab.json 资产（一个子树）/ 实例化
     （duplicate 递归子树，现在只复制 8 类组件）/ 覆盖（实例改过的字段）+
     Apply / Revert / 改母版 → 所有实例更新"

五组判据（A 数值 + B~E 端到端），分工是**刻意的**：

  A 组（无窗口）`Game.exe --selftest prefab` 36 条 `PREFAB-TEST`：
    解析（含 5 条负向：重复 localId / 父下标越界 / 环 / localId 带 `/` /
    空表）、实例化（3 份 9 个实体 id 互不相同、localId 稳定、父子对、
    名字去重）、传播、覆盖（3/5/3 三个数互不相等）、Apply、Revert、
    场景往返、从子树导出。数值语义全在这里。

  B 组（GUI，编辑器）**从零建一份 prefab 并摆两份**：证明"编辑器里的入口"
    （`prefab.create` / `prefab.instantiate` 走的是与层级右键菜单**同一个**
    EditorContext 方法，不是测试专用旁路）真的把文件写出来了，而且存盘之后
    实例标记还在（`prefab` 块写进场景 JSON）——少了它，存一次盘实例就脱钩。

  C 组（GUI）**改母版 → 所有实例更新，但覆盖字段不跟随**。这是整个功能
    最容易"看起来做完其实没做"的一条：判据是同一份场景里三个数**互不相等**
    （跟随的 2 / 被覆盖的 3 / 保留的落点 5），写成"全等于 2"或"全等于 3"
    都会被抓。同一次启动里接着跑 Revert（`cleared=2` + 再同步一次）。

  D 组（GUI）**Apply**：把实例的覆盖写回资产（`wrote=2 cleared=2`），并且
    之后**别的实例也跟着变**（写进 prefab 文件的值才是权威值）。

  E 组（GUI）**真鼠标拖 gizmo**：覆盖是"记账"不是"与母版比差异"，而真实
    交互里只有 gizmo 松手那一刻会记账（`GizmoController::endDrag`）。判据
    是"只动了一个分量 → 只记 1 条"——三个全记的话，之后改母版的旋转对
    这棵树永远不再生效，而作者从没碰过旋转。

用法：
    python tools/verify_prefab.py
"""

import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import launcher_kit as kit                                        # noqa: E402
from capture_window import find_window, user32                    # noqa: E402
from verify_asset_drag import force_foreground, move_to           # noqa: E402
from safe_fs import safe_remove                                   # noqa: E402

MOUSE_LDOWN = 0x0002      # MOUSEEVENTF_LEFTDOWN
MOUSE_LUP = 0x0004        # MOUSEEVENTF_LEFTUP

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
EDITOR = os.path.join(BIN, "Editor.exe")
GAME = os.path.join(BIN, "Game.exe")
OUT = os.path.join(os.path.dirname(ROOT), "_venn_verify", "g18")

# 资产根：`assets::assetRoots()` 的第 2 条 = exe 所在目录 /assets
# （CMake 会把 assets 拷到 build/bin），第 1 条 MYVK_ASSET_DIR 我们不设。
ASSET_DIR = os.path.join(BIN, "assets")
PREFAB_DIR = os.path.join(ASSET_DIR, "prefabs")

REL_A = "prefabs/vtest_a.vprefab.json"      # B 组：编辑器自己建的
REL_C = "prefabs/vtest_c.vprefab.json"      # C/D/E 组：脚本手写的夹具

FAILS = []


def check(name, cond, detail=""):
    kit.log("CHECK %s %s%s" % (name, "OK" if cond else "FAIL",
                               ("  " + str(detail)) if detail != "" else ""))
    if not cond:
        FAILS.append(name)


def wait_count(tap, needle, n, timeout=25.0):
    """等日志里 needle 出现次数 >= n。

    ⚠ 不能用 `has_line`：那对"前面已经出现过"永远为真。计数必须在
      动作**之前**取（这个项目踩过）。
    """
    end = time.time() + timeout
    while time.time() < end:
        if tap.count_line(needle) >= n:
            return True
        time.sleep(0.1)
    return False


def last_match(tap, rx):
    found = None
    with tap.lock:
        for line in tap.lines:
            m = rx.search(line)
            if m:
                found = m
    return found


# ------------------------------------------------------------------ 夹具

def cube(name, pos, scale=(1.0, 1.0, 1.0), parent=-1, prefab=None):
    e = {
        "name": name,
        "transform": {"position": list(pos),
                      "rotation": [0.0, 0.0, 0.0],
                      "scale": list(scale)},
        "parent": parent,
        "visible": True,
        "mesh": {"kind": "builtin", "shape": "cube", "size": 1.0,
                 "name": "prefab_test_" + name.replace(" ", "_")},
        "material": {"kind": "procedural", "name": "prefab_test_mat",
                     "baseColor": [0.85, 0.35, 0.30, 1.0],
                     "metallic": 0.0, "roughness": 0.55,
                     "normalScale": 0.0, "ao": 1.0,
                     "emissive": [0, 0, 0],
                     "doubleSided": False, "alphaBlend": False},
    }
    if prefab:
        e["prefab"] = prefab
    return e


def write_json(path, obj):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(obj, f, indent=2, ensure_ascii=False)
    return path


def write_prefab(path, scale):
    """手写一份 prefab：两个节点（根 Crate + 子 Lid）。

    ⚠ 位置刻意不是原点（(0,0,0) 与"没设"分不开）；缩放由 `scale` 参数
      决定，C 组靠"改这里再 reload"来验传播。
    """
    nodes = [
        {"localId": "Crate", "parent": -1, "name": "Crate",
         "transform": {"position": [0.0, 0.0, 0.0],
                       "rotation": [0.0, 0.0, 0.0],
                       "scale": [scale, scale, scale]},
         "mesh": {"kind": "builtin", "shape": "cube", "size": 1.0,
                  "name": "prefab_fixture_crate"}},
        {"localId": "Lid", "parent": 0, "name": "Lid",
         "transform": {"position": [0.0, 0.0, 1.0],
                       "rotation": [0.0, 0.0, 0.0],
                       "scale": [scale, scale, scale]},
         "mesh": {"kind": "builtin", "shape": "cube", "size": 1.0,
                  "name": "prefab_fixture_lid"}},
    ]
    return write_json(path, {
        "kind": "venn.prefab",
        "version": 1,
        "name": "vtest_c",
        "//": "verify_prefab.py 的夹具（脚本生成，勿手工改）",
        "entities": nodes,
    })


def scene_b(path):
    """B 组起点：一棵真实的子树（根 + 一个子节点），还没有 prefab。"""
    return write_json(path, {
        "version": 1,
        "generator": "verify_prefab",
        "camera": {"target": [0, 0, 0.5], "yaw": 1.5707963, "pitch": 0.0,
                   "distance": 9.0},
        "entities": [
            cube("Crate", (0.0, 0.0, 0.5)),
            cube("Lid", (0.0, 0.0, 1.2), parent=0),
        ],
    })


def scene_two_instances(path, rel_prefab, with_overrides=True):
    """C/D/E 组起点：**两份**实例，其中实例 1 身上有两条覆盖。

      · Crate   —— 实例 1 的根：位置被摆到 x=5（覆盖）
      · Lid     —— 实例 1 的子：缩放被改成 3（覆盖）
      · Crate 2 —— 实例 2 的根：什么都没覆盖
      · Lid 2   —— 实例 2 的子：什么都没覆盖

    于是"改母版 scale=2 再同步"之后，三处 scale 应该是 2 / 3 / 2 —— 
    三个数**互不相等**，写成"全等于 2"（覆盖失效）或"全等于 3"（传播
    失效）都会被抓。

    `with_overrides=False` → 实例 1 也干净（E 组：拖 gizmo 要能**新记**一条，
    身上已经有同一条路径的话 `recordOverride` 会去重、什么都不打）。
    """
    link = {"path": rel_prefab}
    ov = {}
    if with_overrides:
        ov = {"root": True,
              "overrides": ["Crate/transform.position", "Lid/transform.scale"]}
    else:
        ov = {"root": True}
    return write_json(path, {
        "version": 1,
        "generator": "verify_prefab",
        "camera": {"target": [0, 0, 0.5], "yaw": 1.5707963, "pitch": 0.0,
                   "distance": 14.0},
        "entities": [
            cube("Crate", (5.0, 0.0, 0.0), prefab={
                "localId": "Crate", "instance": 1, **ov, **link}),
            cube("Lid", (0.0, 0.0, 1.0),
                 scale=(3.0, 3.0, 3.0) if with_overrides else (1.0, 1.0, 1.0),
                 parent=0, prefab={"localId": "Lid", "instance": 1, **link}),
            cube("Crate 2", (0.0, 0.0, 0.0), prefab={
                "localId": "Crate", "instance": 2, "root": True, **link}),
            cube("Lid 2", (0.0, 0.0, 1.0), parent=2,
                 prefab={"localId": "Lid", "instance": 2, **link}),
        ],
    })


def load_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def find_entity(scene, name):
    for e in scene.get("entities", []):
        if e.get("name") == name:
            return e
    return None


def xform(e, key, axis=0):
    """读一个实体的 transform.<key>[axis]；拿不到返回 None（断言里会显形）。"""
    if not e:
        return None
    t = e.get("transform") or {}
    v = t.get(key)
    if not isinstance(v, list) or len(v) < 3:
        return None
    return float(v[axis])


# ------------------------------------------------------------------ A 组

def run_selftest():
    kit.log("---- A 组：--selftest prefab ----")
    proc = subprocess.run([GAME, "--selftest", "prefab"], cwd=BIN,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, encoding="utf-8", errors="replace",
                          timeout=180)
    out = proc.stdout or ""
    ok_lines = out.count("result=PASS")
    fail_lines = out.count("result=FAIL")
    check("A1 selftest 退出码 0", proc.returncode == 0, "rc=%d" % proc.returncode)
    check("A2 36 条全过", ok_lines == 36 and fail_lines == 0,
          "ok=%d fail=%d" % (ok_lines, fail_lines))
    m = re.search(r"PREFAB-TOTAL pass=(\d+) fail=(\d+)", out)
    check("A3 汇总行 fail=0", m is not None and m.group(2) == "0",
          m.group(0) if m else "没有 PREFAB-TOTAL 行")
    # 负向用例真的存在（否则"解析器什么都能解析"也会全绿）
    check("A4 5 条负向解析用例都在", out.count("prefab_reject_") >= 5,
          "reject=%d" % out.count("prefab_reject_"))
    if fail_lines:
        for line in out.splitlines():
            if "result=FAIL" in line:
                kit.log("  " + line.strip())


# ------------------------------------------------------------------ 起编辑器

def launch(scene_path, console_cmd, select="", gizmo=""):
    env = kit.clean_env({
        "MYVK_EDITOR_SCENE": scene_path,
        # E 组要靠 `GZ-ORIGIN` / `GZ-AXIS-X` 定位手柄；logRect 只有在这个
        # 变量为真时才打（否则一句矩形日志都看不到）。
        "MYVK_LOG_RECTS": "1",
        "MYVK_NO_WINDOW_SAVE": "1",     # 自动化判定为真 → layout 不落盘
    })
    if console_cmd:
        env["MYVK_EDITOR_CONSOLE_CMD"] = console_cmd
    if select:
        env["MYVK_EDITOR_SELECT"] = select
    if gizmo:
        env["MYVK_EDITOR_GIZMO"] = gizmo
    proc = subprocess.Popen([EDITOR], cwd=BIN, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = kit.LogTap()
    tap.start(proc)
    hwnd = find_window(kit.EDITOR_TITLE, 45.0)
    return proc, tap, hwnd


# ------------------------------------------------------------------ B 组
#
# 命令串走的是 `MYVK_EDITOR_CONSOLE_CMD`（第 40 帧执行），`;` 分隔多条。
# 每一步都对应层级右键菜单里的同一项 —— 所以这里绿了，
# "Create Prefab from Selection" 那条也绿了（同一份 EditorContext 实现）。

def run_create_and_instantiate(work):
    kit.log("---- B 组：建 prefab + 摆两份 + 存盘往返 ----")
    scene_path = scene_b(os.path.join(work, "scene_b.json"))
    # 上一轮的残留会让 prefabDefaultPath 自动改名（_2），断言就找不到文件
    for stale in (os.path.join(PREFAB_DIR, "vtest_a.vprefab.json"),
                  os.path.join(PREFAB_DIR, "vtest_a_2.vprefab.json"),
                  os.path.join(PREFAB_DIR, "vtest_a_3.vprefab.json")):
        if os.path.exists(stale):
            safe_remove(stale)

    proc, tap, hwnd = launch(
        scene_path,
        "prefab.create vtest_a;prefab.info;"
        "prefab.instantiate %s 3 0 0;prefab.info;scene.save" % REL_A,
        select="Crate")
    if not hwnd:
        FAILS.append("B0 编辑器窗口没出现")
        return None
    try:
        # 最后一条命令（scene.save）不自己打锚点，用第二次 PREFAB-INFO 当
        # "命令串跑完了"的信号，再额外等一小会儿让写盘落地。
        ok = wait_count(tap, "PREFAB-INFO ok=1", 2, 60.0)
        check("B1 命令串跑完（两条 PREFAB-INFO）", ok,
              "info=%d" % tap.count_line("PREFAB-INFO ok=1"))
        time.sleep(1.2)

        m = last_match(tap, re.compile(
            r"PREFAB-EXTRACT ok=1 nodes=(\d+) name=(\S+)"))
        check("B2 从选中子树导出 2 个节点", m is not None and m.group(1) == "2",
              m.group(0) if m else "没有 PREFAB-EXTRACT")

        m = last_match(tap, re.compile(
            r"PREFAB-SAVE path=(\S+) full=(\S+) nodes=(\d+) bytes=(\d+)"))
        full = m.group(2) if m else None
        check("B3 prefab 文件真的写出来了",
              bool(m) and m.group(1) == REL_A and m.group(3) == "2"
              and full and os.path.exists(full),
              m.group(0) if m else "没有 PREFAB-SAVE")

        m = last_match(tap, re.compile(
            r"PREFAB-BIND ok=1 path=(\S+) instance=(\d+) members=(\d+)"))
        check("B4 就地绑定成实例 1（不是先删后拖）",
              m is not None and m.group(2) == "1" and m.group(3) == "2",
              m.group(0) if m else "没有 PREFAB-BIND")

        infos = []
        with tap.lock:
            for line in tap.lines:
                mm = re.search(r"PREFAB-INFO ok=1 path=(\S+) instance=(\d+) "
                               r"root=(\d) localId=(\S+) overrides=(\d)", line)
                if mm:
                    infos.append(mm.groups())
        check("B5 建完之后是实例 1、0 条覆盖",
              len(infos) >= 1 and infos[0][1] == "1" and infos[0][4] == "0",
              str(infos[:1]))
        m = last_match(tap, re.compile(
            r"PREFAB-INSTANCE path=(\S+) instance=(\d+) root=(\d+) "
            r"members=(\d+) at=([-\d.]+),([-\d.]+),([-\d.]+)"))
        check("B6 摆进场景成实例 2（2 个成员、落点 3,0,0）",
              m is not None and m.group(2) == "2" and m.group(4) == "2"
              and abs(float(m.group(5)) - 3.0) < 1e-3,
              m.group(0) if m else "没有 PREFAB-INSTANCE")
        # 落点 ≠ 母版位置 → 必须记一条覆盖（否则改母版时实例会自己跳回去）
        check("B7 落点与母版不同 → 记了 1 条覆盖",
              len(infos) >= 2 and infos[1][1] == "2" and infos[1][4] == "1",
              str(infos[1:2]))

        # ---- 文件内容 ----
        if full and os.path.exists(full):
            pj = load_json(full)
            ents = pj.get("entities") or []
            check("B8 prefab 文件 kind/version 正确",
                  pj.get("kind") == "venn.prefab" and pj.get("version") == 1,
                  "kind=%s" % pj.get("kind"))
            check("B9 prefab 文件 2 个节点、localId 稳定",
                  len(ents) == 2
                  and [e.get("localId") for e in ents] == ["Crate", "Lid"],
                  str([e.get("localId") for e in ents]))
            check("B10 prefab 文件 parent 下标 = -1/0",
                  [e.get("parent") for e in ents] == [-1, 0],
                  str([e.get("parent") for e in ents]))

        # ---- 存盘往返：实例标记必须活下来 ----
        sc = load_json(scene_path)
        ents = sc.get("entities") or []
        links = [e for e in ents if isinstance(e.get("prefab"), dict)]
        check("B11 存盘后 4 个实体、4 个都带 prefab 标记",
              len(ents) == 4 and len(links) == 4,
              "entities=%d links=%d" % (len(ents), len(links)))
        roots = [e for e in links if e["prefab"].get("root")]
        check("B12 两份实例（instance 1 / 2）都在",
              sorted(int(e["prefab"].get("instance", 0)) for e in roots)
              == [1, 2],
              str([e["prefab"].get("instance") for e in roots]))
        check("B13 每个成员都带 path + localId",
              all(e["prefab"].get("path") == REL_A
                  and e["prefab"].get("localId") in ("Crate", "Lid")
                  for e in links),
              str([(e["prefab"].get("path"), e["prefab"].get("localId"))
                   for e in links][:2]))
        inst2 = [e for e in roots if int(e["prefab"].get("instance", 0)) == 2]
        check("B14 实例 2 的落点覆盖写进了场景文件",
              inst2 and inst2[0]["prefab"].get("overrides")
              == ["Crate/transform.position"],
              str(inst2[0]["prefab"].get("overrides")) if inst2 else "no inst2")
        inst1 = [e for e in roots if int(e["prefab"].get("instance", 0)) == 1]
        check("B15 实例 1 没有覆盖（0 条就不写这个字段）",
              inst1 and "overrides" not in inst1[0]["prefab"],
              str(inst1[0]["prefab"].keys()) if inst1 else "no inst1")
        return full
    finally:
        kit.shutdown(proc, hwnd)


# ------------------------------------------------------------------ C 组
#
# 母版改 scale 1 → 2，重新读盘 + 同步。判据是**三个互不相等的数**：
#   实例 1 根 scale = 2（跟随）· 实例 1 子 scale = 3（被覆盖）
#   实例 2 根 scale = 2（跟随）· 实例 1 根位置 x = 5（被覆盖，仍保留）

def run_source_edit_and_revert(work, prefab_dir):
    kit.log("---- C 组：改母版 → 所有实例更新（覆盖字段不跟随）+ Revert ----")
    prefab_path = write_prefab(
        os.path.join(prefab_dir, "vtest_c.vprefab.json"), scale=2.0)
    scene_path = scene_two_instances(os.path.join(work, "scene_c.json"), REL_C)

    proc, tap, hwnd = launch(
        scene_path,
        "prefab.reload %s;prefab.info;scene.save;prefab.revert" % REL_C,
        select="Crate")
    if not hwnd:
        FAILS.append("C0 编辑器窗口没出现")
        return
    try:
        ok = wait_count(tap, "PREFAB-REVERT", 1, 60.0)
        check("C1 命令串跑完（PREFAB-REVERT 出现）", ok)
        time.sleep(1.0)

        m = last_match(tap, re.compile(
            r"PREFAB-SYNC path=(\S+) instances=(\d+) members=(\d+) "
            r"applied=(\d+) failed=(\d+)"))
        check("C2 同步覆盖 2 份实例 / 4 个成员 / 0 失败",
              m is not None and m.group(2) == "2" and m.group(3) == "4"
              and m.group(5) == "0",
              m.group(0) if m else "没有 PREFAB-SYNC")
        check("C3 4 个成员全都下发过（applied=4）",
              m is not None and m.group(4) == "4", m.group(4) if m else "-")

        sc = load_json(scene_path)
        s1 = xform(find_entity(sc, "Crate"), "scale")
        s1lid = xform(find_entity(sc, "Lid"), "scale")
        s2 = xform(find_entity(sc, "Crate 2"), "scale")
        s2lid = xform(find_entity(sc, "Lid 2"), "scale")
        check("C4 实例 1 的根跟随母版（1 → 2）", s1 is not None
              and abs(s1 - 2.0) < 1e-3, "scale=%.3f" % (s1 or -1))
        check("C5 实例 1 的子**不跟随**（被覆盖，仍是 3）", s1lid is not None
              and abs(s1lid - 3.0) < 1e-3, "scale=%.3f" % (s1lid or -1))
        check("C6 实例 2 的根跟随母版（1 → 2）", s2 is not None
              and abs(s2 - 2.0) < 1e-3, "scale=%.3f" % (s2 or -1))
        check("C7 实例 2 的子也跟随（1 → 2）", s2lid is not None
              and abs(s2lid - 2.0) < 1e-3, "scale=%.3f" % (s2lid or -1))
        check("C8 三处缩放互不相等（2/3/2 —— 写成全等会漏判）",
              s1 is not None and s1lid is not None and s2lid is not None
              and abs(s1 - s1lid) > 0.5 and abs(s1 - s2lid) < 1e-3,
              "%.2f / %.2f / %.2f" % (s1 or -1, s1lid or -1, s2lid or -1))
        px = xform(find_entity(sc, "Crate"), "position")
        check("C9 被覆盖的落点仍然保留（x=5，没被母版的 0 冲掉）",
              px is not None and abs(px - 5.0) < 1e-3,
              "x=%.3f" % (px if px is not None else -1))
        p2 = xform(find_entity(sc, "Crate 2"), "position")
        check("C10 未覆盖的落点跟随母版（x=0）",
              p2 is not None and abs(p2) < 1e-3,
              "x=%.3f" % (p2 if p2 is not None else -1))

        m = last_match(tap, re.compile(r"PREFAB-REVERT instance=(\d+) cleared=(\d+)"))
        check("C11 Revert 清掉 2 条覆盖（实例 1）",
              m is not None and m.group(1) == "1" and m.group(2) == "2",
              m.group(0) if m else "没有 PREFAB-REVERT")
        check("C12 Revert 之后重新同步了一次（PREFAB-SYNC 有 2 次）",
              tap.count_line("PREFAB-SYNC path=") >= 2,
              "sync=%d" % tap.count_line("PREFAB-SYNC path="))
    finally:
        kit.shutdown(proc, hwnd)


# ------------------------------------------------------------------ D/E 组
#
# Apply：把实例 1 的两条覆盖（position=5 / lid.scale=3）写回 prefab 文件，
# 然后**所有实例**都该跟着变成 5 / 3 —— 这是"Apply 到底生效没有"的判据。

def run_apply(work, prefab_dir):
    kit.log("---- D/E 组：Apply 覆盖写回母版 + 其余实例跟随 ----")
    prefab_path = write_prefab(
        os.path.join(prefab_dir, "vtest_c.vprefab.json"), scale=1.0)
    scene_path = scene_two_instances(os.path.join(work, "scene_e.json"), REL_C)

    proc, tap, hwnd = launch(scene_path, "prefab.apply;scene.save",
                             select="Crate")
    if not hwnd:
        FAILS.append("D0 编辑器窗口没出现")
        return
    try:
        ok = wait_count(tap, "HIER-PREFAB apply=1", 1, 60.0)
        check("D1 命令串跑完（HIER-PREFAB apply=1）", ok)
        time.sleep(1.5)

        m = last_match(tap, re.compile(
            r"PREFAB-APPLY path=(\S+) instance=(\d+) wrote=(\d+) cleared=(\d+)"))
        check("D2 Apply 写回 2 条路径并清掉 2 条覆盖",
              m is not None and m.group(4) == "2" and m.group(3) == "2",
              m.group(0) if m else "没有 PREFAB-APPLY")
        check("D3 没有 PREFAB-APPLY-MISS（覆盖路径都落在了真实字段上）",
              tap.count_line("PREFAB-APPLY-MISS") == 0,
              "miss=%d" % tap.count_line("PREFAB-APPLY-MISS"))

        pj = load_json(prefab_path)
        nodes = {n.get("localId"): n for n in pj.get("entities", [])}
        check("D4 母版文件的 Crate.position.x 变成 5",
              nodes.get("Crate") and abs(float(
                  nodes["Crate"]["transform"]["position"][0]) - 5.0) < 1e-3,
              str(nodes.get("Crate", {}).get("transform", {}).get("position")))
        check("D5 母版文件的 Lid.scale.x 变成 3",
              nodes.get("Lid") and abs(float(
                  nodes["Lid"]["transform"]["scale"][0]) - 3.0) < 1e-3,
              str(nodes.get("Lid", {}).get("transform", {}).get("scale")))

        # 场景里所有实例都跟着变（Apply 之后重新读盘 + 同步）
        sc = load_json(scene_path)
        s1 = xform(find_entity(sc, "Lid"), "scale")
        s2 = xform(find_entity(sc, "Lid 2"), "scale")
        check("D6 Apply 之后**别的实例**也变成 3",
              s1 is not None and s2 is not None
              and abs(s1 - 3.0) < 1e-3 and abs(s2 - 3.0) < 1e-3,
              "%.2f / %.2f" % (s1 or -1, s2 or -1))
        l1 = xform(find_entity(sc, "Crate"), "position")
        l2 = xform(find_entity(sc, "Crate 2"), "position")
        check("D7 位置也一起传播（两份实例 x 都 = 5）",
              l1 is not None and l2 is not None
              and abs(l1 - 5.0) < 1e-3 and abs(l2 - 5.0) < 1e-3,
              "%.2f / %.2f" % (l1 or -1, l2 or -1))
        roots = [e for e in sc.get("entities", [])
                 if isinstance(e.get("prefab"), dict)
                 and e["prefab"].get("root")]
        check("D8 覆盖已清空（场景文件里不再有 overrides）",
              roots and all("overrides" not in r["prefab"] for r in roots),
              str([r["prefab"].get("overrides") for r in roots]))
    finally:
        kit.shutdown(proc, hwnd)


# ------------------------------------------------------------------ E 组
#
# 覆盖是**记账**，不是"与母版比默认值"（见 Prefab.h 决定 ③）。真实交互里
# 只有一处会记账：gizmo 松手那一刻（`GizmoController::endDrag` 三个
# `recordOverride`）。所以这条必须用**真鼠标拖**—— 否则那段 glue 永远
# 没人验，而它恰恰是最容易写错的一处（记错分量、或三个分量全记）。

def drag(hwnd, src, dx, dy, steps=14):
    move_to(hwnd, int(src[0]), int(src[1]))
    time.sleep(0.35)
    user32.mouse_event(MOUSE_LDOWN, 0, 0, 0, 0)
    time.sleep(0.20)
    for k in range(1, steps + 1):
        move_to(hwnd, int(src[0] + dx * k / steps), int(src[1] + dy * k / steps))
        time.sleep(0.035)
    time.sleep(0.15)
    user32.mouse_event(MOUSE_LUP, 0, 0, 0, 0)
    time.sleep(0.7)


GIZMO_END_RE = re.compile(
    r"gizmo end: handle=(\S+) '([^']*)' pos=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\)")
OVR_RE = re.compile(r"PREFAB-OVERRIDE path=(\S+) instance=(\d+) localId=(\S+) total=(\d+)")


def run_gizmo_override(work, prefab_dir):
    kit.log("---- E 组：拖 gizmo → 只记被改的那一个分量 ----")
    write_prefab(os.path.join(prefab_dir, "vtest_c.vprefab.json"), scale=1.0)
    scene_path = scene_two_instances(os.path.join(work, "scene_g.json"), REL_C,
                                     with_overrides=False)

    proc, tap, hwnd = launch(scene_path, "", select="Crate", gizmo="move")
    if not hwnd:
        FAILS.append("E0 编辑器窗口没出现")
        return
    try:
        force_foreground(hwnd)
        time.sleep(1.0)
        origin = tap.wait_rect("GZ-ORIGIN", 25.0)
        tip = tap.wait_rect("GZ-AXIS-X", 25.0)
        check("E1 手柄上屏（GZ-ORIGIN + GZ-AXIS-X）",
              origin is not None and tip is not None,
              "origin=%s tip=%s" % (origin, tip))
        if not origin or not tip:
            return

        ox = (origin[0] + origin[2]) * 0.5
        oy = (origin[1] + origin[3]) * 0.5
        tx = (tip[0] + tip[2]) * 0.5
        ty = (tip[1] + tip[3]) * 0.5
        ux, uy = tx - ox, ty - oy
        ln = max((ux * ux + uy * uy) ** 0.5, 1e-3)
        ux, uy = ux / ln, uy / ln
        # ⚠ 起点不能是手柄中心：中心那个"自由移动"方块的命中半径有 10px，
        #   从原点按下去抓到的是它（_probe_gizmo_scale 踩过）。从 3/4 处下手。
        sx, sy = ox + ux * ln * 0.75, oy + uy * ln * 0.75
        reach = ln * 1.7
        n0 = tap.count_line("gizmo end:")
        drag(hwnd, (sx, sy), ux * reach, uy * reach)
        check("E2 拖拽真的抓住了 X 轴手柄（gizmo end 出现）",
              wait_count(tap, "gizmo end:", n0 + 1, 10.0),
              "end=%d" % tap.count_line("gizmo end:"))

        m = last_match(tap, GIZMO_END_RE)
        moved = False
        if m:
            px = float(m.group(3))
            moved = abs(px - 5.0) > 0.01
            check("E3 位置真的变了（x 离开 5.0）", moved, "x=%.3f" % px)
        else:
            check("E3 位置真的变了（x 离开 5.0）", False, "没有 gizmo end 行")

        ok = wait_count(tap, "PREFAB-OVERRIDE", 1, 10.0)
        check("E4 松手时记了一条覆盖（PREFAB-OVERRIDE）", ok,
              "override=%d" % tap.count_line("PREFAB-OVERRIDE"))
        m = last_match(tap, OVR_RE)
        check("E5 记的是 Crate/transform.position（不是别的分量）",
              m is not None and m.group(1) == "Crate/transform.position"
              and m.group(2) == "1",
              m.group(0) if m else "没有 PREFAB-OVERRIDE")
        # 只动了一个分量 → **只该有一条**。三个全记的话，之后改母版的旋转
        # 对这棵树永远不再生效（而作者从没碰过旋转）。
        check("E6 只记了 1 条（没顺手把 rotation/scale 也钉死）",
              tap.count_line("PREFAB-OVERRIDE") == 1,
              "override=%d" % tap.count_line("PREFAB-OVERRIDE"))
    finally:
        kit.shutdown(proc, hwnd)


# ------------------------------------------------------------------ main

def main():
    work = kit.fresh_dir(OUT)
    kit.log("work dir = %s" % work)

    if not os.path.exists(EDITOR) or not os.path.exists(GAME):
        kit.log("找不到 build/bin 下的 exe —— 先 ./msvc-build.cmd build")
        return kit.finish(["exe 不存在"])

    run_selftest()

    full_a = run_create_and_instantiate(work)
    # C/D/E 三个夹具文件写在同一目录（B 组从日志里读出来的真实资产根）
    prefab_dir = os.path.dirname(full_a) if full_a else PREFAB_DIR
    os.makedirs(prefab_dir, exist_ok=True)

    run_source_edit_and_revert(work, prefab_dir)
    run_apply(work, prefab_dir)
    run_gizmo_override(work, prefab_dir)

    # 清理：只删本脚本自己造的 prefab 资产（单文件删除，不碰目录）
    for rel in ("vtest_a.vprefab.json", "vtest_c.vprefab.json"):
        p = os.path.join(prefab_dir, rel)
        if os.path.exists(p):
            try:
                safe_remove(p)
            except OSError:
                kit.log("WARN 清理失败（不影响结论）: %s" % p)

    return kit.finish(FAILS)


if __name__ == "__main__":
    sys.exit(main())
