#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/verify_material_import.py —— 地形层材质：从目录刷新导入（2026-10-24）

用户原话（第六轮第 2 条）：
    "在 Terrain 的属性面板中的 Layer materials 那里，那里的地形材质一般是
     指定一个目录里面，将对应的材质信息按照之前 E:\\code\\cpp\\venn-resource
     中的材质信息的形式去自动添加，这个通过一个刷新按钮去找到最新的材质信息"

所以这条用例验的就是"**点一下刷新，目录里的材质自己进库**"：
  扫目录 → 复制贴图 → 生成 `.vmat` → 合并 `library.json` → 重载仓库，
全程不离开编辑器、不跑 Python。

## 判据

  I1  `INS-MATSRC-INIT dir=<源目录>`：面板认出了目录
      （没有设置文件时由 `guessMaterialSourceDir()` 给；用例用
       `MYVK_MATERIAL_SOURCE` 指到临时目录，避免依赖本机 E: 盘）
  I2  点 `INS-MATSRC-REFRESH` 后 `... ok=1 imported=1 added=1`
  I3  运行时资产根里真的落了 `<work>/assets/materials/library/test_moss.vmat`，
      且 albedo / normal / orm 三个槽都指向存在的文件
  I4  三张贴图被复制到 `assets/textures/materials/test_moss/`
  I5  `library.json` 里出现 test_moss 条目（原 2 条 → 3 条，并保住原有条目）
  I6  重载生效：`MATLIB-LOAD ... count=3`（第二次出现，且在点击之后）
  I7  再点一次刷新：`added=0 imported=1` —— **幂等**，不会在清单里堆重复条目
  I8  日志健康：零 `[ERROR]`、零 `[VK_CHECK]`

## 为什么用一个"假材质包"

临时目录里现造一个 `test_moss/test_moss_1k.gltf/`（glTF + textures/，贴图直接
复制仓库里 grass_ground 的 1k jpg）。好处有三个：
  · 不碰真实的 `venn-resource`，用例可重复跑；
  · 导入的是**新**名字，能断言"清单从 2 条变 3 条"，而不是被幂等掩盖；
  · `MYVK_MATERIAL_NO_MIRROR=1` 关掉回写工程源目录，仓库 assets/ 不被污染。

用法：python tools/verify_material_import.py
退出码 0 = 全过。
"""

import json
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import launcher_kit as kit                                            # noqa: E402
from capture_window import find_window                                # noqa: E402
from verify_add_menu import LogTap, click, kill_stale_editors         # noqa: E402
from verify_landscape_ground import focus_window, user32              # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(REPO, "build", "bin", "Editor.exe")
TEMPLATE = os.path.join(kit.TEMPLATES, "third_person")

MAT_NAME = "test_moss"
MAT_TIER = "1k"

# `INS-MATSRC-REFRESH dir=... tier=... ok=1 imported=1 added=1`
RE_IMPORT = re.compile(
    r"INS-MATSRC-REFRESH dir=(.+?) tier=(\S+) ok=(\d) imported=(\d+) added=(\d+)")

RECT_RE = re.compile(r"=\((-?\d+),(-?\d+)\)-\((-?\d+),(-?\d+)\)")

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
    return ok


# ---------------------------------------------------------------- 小工具

def snap(tap):
    with tap.lock:
        return list(tap.lines)


def find_rect(tap, tag, timeout=6.0):
    """按锚点取矩形。锚点行形如 `TAG=(x0,y0)-(x1,y1)`。"""
    deadline = time.time() + timeout
    pat = re.compile(re.escape(tag) + RECT_RE.pattern)
    while time.time() < deadline:
        for line in reversed(snap(tap)):
            m = pat.search(line)
            if m:
                return tuple(float(m.group(i)) for i in range(1, 5))
        time.sleep(0.12)
    return None


def wait_regex(tap, pattern, timeout=12.0, since=0):
    """等到**第 since 条之后**出现匹配行，返回该行的 match 对象。

    为什么要 since：`MATLIB-LOAD` / `INS-MATSRC-REFRESH` 都会出现多次，
    "有匹配"不能证明"这次点击生效了"，必须是新出现的那一条。
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        lines = snap(tap)
        for line in lines[since:]:
            m = pattern.search(line)
            if m:
                return m
        time.sleep(0.12)
    return None


def reveal(tap, hwnd, tag, panel_tag="INS-PANEL", tries=14):
    """把 Inspector 里的目标滚进可见区，返回可点中心。理由同 verify_world_tools。

    锚点报的是 ImGui item 的**未裁剪**矩形；Terrain 段在 All 页签里排在很
    下面，控件经常有一半在面板之外，直接点会打到别的窗口上。
    """
    r = find_rect(tap, tag, 2.0)
    pnl = find_rect(tap, panel_tag, 2.0)
    if not r or not pnl:
        return None
    for _ in range(tries):
        if r[1] >= pnl[1] + 4.0 and r[3] <= pnl[3] - 4.0:
            return ((r[0] + r[2]) * 0.5, (r[1] + r[3]) * 0.5)
        down = r[3] > pnl[3] - 4.0
        cx = (pnl[0] + pnl[2]) * 0.5
        cy = (pnl[1] + pnl[3]) * 0.5
        focus_window(hwnd)
        from verify_asset_drag import client_to_screen
        sx, sy = client_to_screen(hwnd, int(cx), int(cy))
        user32.SetCursorPos(sx, sy)
        time.sleep(0.15)
        user32.mouse_event(0x0800, 0, 0, -120 if down else 120, 0)
        time.sleep(0.45)
        r = find_rect(tap, tag, 1.0) or r
        pnl = find_rect(tap, panel_tag, 1.0) or pnl
    print("      ! %s 滚不进面板 (item=%s panel=%s)"
          % (tag, ["%.0f" % v for v in r], ["%.0f" % v for v in pnl]))
    return None


# ---------------------------------------------------------------- 工程 / 源

def build_project(work):
    root = os.path.join(work, "MatImport")
    os.makedirs(root, exist_ok=True)
    shutil.copytree(TEMPLATE, root, dirs_exist_ok=True)

    # 材质仓库（模板里没有，和 verify_material_library 一样从仓库根补）
    for rel in ("materials/library", "textures/materials"):
        src = os.path.join(REPO, "assets", rel)
        if os.path.isdir(src):
            shutil.copytree(src, os.path.join(root, "assets", rel),
                            dirs_exist_ok=True)
    # 清单本身要带上：否则被测的"合并清单"分支会退化成"新建清单"，
    # 而真实用户打开工程时清单一定在。
    shutil.copyfile(os.path.join(REPO, "assets", "materials", "library.json"),
                    os.path.join(root, "assets", "materials", "library.json"))
    return root


def build_source(src_dir):
    """造一个 `<name>/<name>_1k.gltf/` 形式的材质包（贴图复用 grass_ground 的 1k 档）。"""
    pkg = os.path.join(src_dir, MAT_NAME, "%s_%s.gltf" % (MAT_NAME, MAT_TIER))
    tex = os.path.join(pkg, "textures")
    os.makedirs(tex, exist_ok=True)

    src_tex = os.path.join(REPO, "assets", "textures", "materials", "grass_ground")
    for out, name in (("diff", "grass_ground_diff.jpg"),
                      ("nor", "grass_ground_nor_gl.jpg"),
                      ("arm", "grass_ground_arm.jpg")):
        shutil.copyfile(os.path.join(src_tex, name),
                        os.path.join(tex, out + ".jpg"))

    gltf = {
        "asset": {"version": "2.0", "generator": "verify_material_import.py"},
        "images": [{"uri": "textures/diff.jpg"},
                   {"uri": "textures/nor.jpg"},
                   {"uri": "textures/arm.jpg"}],
        "textures": [{"source": 0}, {"source": 1}, {"source": 2}],
        "materials": [{
            "name": MAT_NAME,
            "doubleSided": True,
            "pbrMetallicRoughness": {
                "baseColorFactor": [1.0, 1.0, 1.0, 1.0],
                # metallicFactor=0 要能被尊重（见 MaterialImport.cpp 的注释）
                "metallicFactor": 0.0,
                "baseColorTexture": {"index": 0},
                "metallicRoughnessTexture": {"index": 2},
            },
            "normalTexture": {"index": 1},
        }],
        "meshes": [],
    }
    with open(os.path.join(pkg, MAT_NAME + "_" + MAT_TIER + ".gltf"), "w",
              encoding="utf-8", newline="\n") as f:
        json.dump(gltf, f, ensure_ascii=False, indent=2)
        f.write("\n")
    return os.path.join(src_dir, MAT_NAME)


# ---------------------------------------------------------------- main

def main():
    if not os.path.isfile(EXE):
        print("找不到可执行文件，请先构建：", EXE)
        return 1
    kill_stale_editors()

    work = kit.fresh_dir(os.path.join(kit.WORK_ROOT, "material_import"))
    root = build_project(work)
    src_dir = os.path.join(work, "mat_src")
    build_source(src_dir)
    kit.log("PROBE project=%s source=%s" % (root, src_dir))

    env = kit.clean_env({
        "MYVK_ASSET_DIR": os.path.join(root, "assets"),
        "MYVK_EDITOR_SCENE": "scenes/main.json",
        "MYVK_EDITOR_SELECT": "Terrain",       # 属性栏才会画 Terrain 段
        "MYVK_EDITOR_INSP_EXPAND": "Terrain",
        "MYVK_LOG_RECTS": "1",
        "MYVK_NO_WINDOW_SAVE": "1",
        "MYVK_VALIDATION": "1",
        # 目录的默认值（没有 library.source.json 时走 guess）
        "MYVK_MATERIAL_SOURCE": src_dir,
        # 别把测试材质回写进仓库的 assets/（否则用例污染工作区）
        "MYVK_MATERIAL_NO_MIRROR": "1",
    })

    proc = subprocess.Popen([EXE], cwd=root, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    hwnd = None
    try:
        hwnd = find_window("Venn Editor", timeout=60)
        if not hwnd:
            check("编辑器启动", False, "找不到窗口")
            return 1
        focus_window(hwnd)
        time.sleep(2.5)

        # ---- I1 面板认出了目录 ----
        m = wait_regex(tap, re.compile(r"INS-MATSRC-INIT dir=(.*?) tier=(\S+)"), 25.0)
        if not check("I1 面板读到材质包目录", m is not None,
                     m.group(0) if m else "没有 INS-MATSRC-INIT"):
            return 2
        got_dir = m.group(1).strip()
        check("I1b 目录命中测试源", os.path.normcase(got_dir) ==
              os.path.normcase(src_dir), got_dir)

        # ---- I2 点刷新 ----
        pos = reveal(tap, hwnd, "INS-MATSRC-REFRESH")
        if not check("I2a Refresh 按钮可点", pos is not None,
                     "%s" % (pos,)):
            return 3
        n_before = len(snap(tap))
        click(hwnd, int(pos[0]), int(pos[1]))
        m = wait_regex(tap, RE_IMPORT, 20.0, since=n_before)
        if not check("I2b 刷新真的导入了", m is not None,
                     m.group(0) if m else "没有新的 INS-MATSRC-REFRESH 行"):
            return 3
        ok, imported, added = m.group(3), int(m.group(4)), int(m.group(5))
        check("I2c 导入结果 ok=1 imported=1 added=1",
              ok == "1" and imported == 1 and added == 1,
              "ok=%s imported=%d added=%d" % (ok, imported, added))

        # ---- I3 / I4 产物 ----
        vmat_path = os.path.join(root, "assets", "materials", "library",
                                 MAT_NAME + ".vmat")
        if not check("I3a .vmat 落盘", os.path.isfile(vmat_path), vmat_path):
            return 4
        with open(vmat_path, encoding="utf-8") as f:
            vmat = json.load(f)
        slots = {k: (vmat.get(k) or {}).get("path", "") for k in
                 ("albedo", "normal", "orm")}
        missing = [k for k, p in slots.items()
                   if not p or not os.path.isfile(
                       os.path.join(root, "assets", p))]
        check("I3b vmat 三个贴图槽都指向真实文件", not missing,
              "缺: %s  槽=%s" % (missing or "无", slots))
        check("I3c 元数据来自 glTF（metallicFactor=0 → metallic 0）",
              abs(float(vmat.get("metallic", -1))) < 1e-6,
              "metallic=%s roughness=%s ao=%s"
              % (vmat.get("metallic"), vmat.get("roughness"), vmat.get("ao")))

        tex_dir = os.path.join(root, "assets", "textures", "materials", MAT_NAME)
        have = sorted(os.listdir(tex_dir)) if os.path.isdir(tex_dir) else []
        check("I4 三张贴图复制到资产里", len(have) == 3, "%s" % have)

        # ---- I5 清单合并 ----
        man_path = os.path.join(root, "assets", "materials", "library.json")
        with open(man_path, encoding="utf-8") as f:
            man = json.load(f)
        names = [e.get("name") for e in man.get("entries", [])]
        check("I5a 清单里多了 test_moss，原有条目还在",
              MAT_NAME in names and "grass_ground" in names, "%s" % names)
        check("I5b 清单条数 = 3", len(names) == 3, "count=%d" % len(names))

        # ---- I6 重载生效 ----
        ms = [mm for mm in
              (re.search(r"MATLIB-LOAD source=(\S+) count=(\d+) default=(\S+)", l)
               for l in snap(tap)) if mm]
        check("I6 重载后仓库变成 3 条",
              bool(ms) and int(ms[-1].group(2)) == 3,
              ms[-1].group(0) if ms else "没有 MATLIB-LOAD")

        # ---- I7 再点一次：幂等 ----
        pos = reveal(tap, hwnd, "INS-MATSRC-REFRESH")
        n_before = len(snap(tap))
        click(hwnd, int(pos[0]), int(pos[1]))
        m2 = wait_regex(tap, RE_IMPORT, 20.0, since=n_before)
        if check("I7a 第二次刷新也执行了", m2 is not None,
                 m2.group(0) if m2 else "没有新的导入日志"):
            check("I7b 幂等：added=0 imported=1",
                  int(m2.group(5)) == 0 and int(m2.group(4)) == 1,
                  "imported=%s added=%s" % (m2.group(4), m2.group(5)))
            with open(man_path, encoding="utf-8") as f:
                names2 = [e.get("name") for e in
                          json.load(f).get("entries", [])]
            check("I7c 条目没被重复插入", names2.count(MAT_NAME) == 1,
                  "%s" % names2)

        # ---- I8 日志健康 ----
        lines = snap(tap)
        errs = [l for l in lines if re.search(r"\[ERROR\]", l)]
        vk = [l for l in lines if "[VK_CHECK]" in l]
        check("I8 零 ERROR / 零 VK_CHECK", not errs and not vk,
              "%s" % (errs + vk)[:2])
        return 0
    finally:
        log_path = os.path.join(work, "material_import.log")
        try:
            with open(log_path, "w", encoding="utf-8") as f:
                f.write("\n".join(snap(tap)) + "\n")
            print("      日志 %s" % log_path)
        except Exception:                                     # noqa: BLE001
            pass
        try:
            if hwnd:
                user32.PostMessageW(hwnd, 0x0010, 0, 0)
        except Exception:                                     # noqa: BLE001
            pass
        time.sleep(0.6)
        if proc.poll() is None:
            proc.terminate()


if __name__ == "__main__":
    rc = main()
    print("\n==== %d passed, %d failed ====" % (g_pass, g_fail))
    sys.exit(0 if g_fail == 0 else 1)
