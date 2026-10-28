#!/usr/bin/env python3
"""E28-1 出包（Package）验收：一个工程能不能变成"拷到别处就能跑"的目录。

为什么要单独一组用例：
    出包这条路上**没有一个错误会自己叫出来** —— 少一个资产是"玩家那边黑屏"，
    覆盖了别人的 dist 是"以为发了新版、其实发的还是旧包"，打出一个 Debug
    包是"拷到同事机器上双击没反应"。三种都发生在**用户机器上**，我们这边
    Ctrl+F5 一切正常。所以判据全落在"产物里到底有什么"和"什么时候必须
    拒绝"上。

验什么：
  A  自检（`Game.exe --selftest package`）：14 条 P 断言全绿
     —— 正常打包 / 清单对账 / 缺入口必须失败 / 目标非空必须拒绝 /
        Debug 引擎必须带 WARNING
  B  真打一次模板工程（`templates/third_person` 复制出来 + 写工程文件）
     → `PACKAGE-OK`，且 dist 里有 exe / assets / config / build.json
  C  清单与磁盘**逐条对账**（条数 + 每个文件的大小）
  D  **干净目录测试**（全组最要紧的一条）：把 dist 挪到工程外、把工程删掉，
     在 dist 里直接跑 `<Name>.exe`，跑 20 帧后必须正常退出
     → 没有这一条，"能跑"永远只证明"源工程还在的时候能跑"
  E  反例：场景缺失 → 退出码 2 + PACKAGE-FAIL（不许打出"看起来成功"的包）
  F  反例：dist 已存在且非空 → 拒绝，且原内容一个字节都不动

⚠ 不验什么：
  * 引用闭包（只打被引用的资产）：那是 G29 的活。这一版**整棵 assets 拷**，
    所以"漏文件"不会发生，"多带文件"是已知代价（见 ProjectBuild.h 的说明）。
  * .pak 压缩与 shipping 剔除（G29 / G30）。
  * 目标机器**没有** VC 运行时的情况：Debug 包依赖调试版 CRT（不可再分发），
    这一条由 `build.json` 的 warnings 暴露，不在这里跑"另一台机器"。

用法：
    python tools/verify_package.py
"""

import ctypes
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from safe_fs import safe_rmtree                                   # noqa: E402
from capture_window import find_window                            # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors            # noqa: E402
from verify_asset_drag import force_foreground                    # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
GAME = os.path.join(BIN, "Game.exe")
EDITOR = os.path.join(BIN, "Editor.exe")

WM_MOUSEMOVE = 0x0200
WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202
ANY_RE = re.compile(r"\b([A-Z][A-Z0-9_-]*)=\(([-\d.]+),([-\d.]+)\)-\(([-\d.]+),([-\d.]+)\)")
TEMPLATE = os.path.join(ROOT, "templates", "third_person")

results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print("[%s] %s  %s" % ("PASS" if ok else "FAIL", name, detail))
    return bool(ok)


def run(args, cwd=None, env_extra=None, timeout=120):
    env = dict(os.environ)
    env.pop("MYVK_ASSET_DIR", None)      # 别让外部环境把资产根指走
    env.pop("MYVK_EDITOR_PROJECT", None)
    if env_extra:
        env.update(env_extra)
    try:
        p = subprocess.run(args, cwd=cwd, env=env, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=timeout)
    except subprocess.TimeoutExpired as e:
        # ⚠ 卡死是**真实可能的**失败形态：缺字体时 ImGui 会弹断言框，
        #   进程活着、日志停在上一行（本项目在编辑器侧踩过同型）。
        #   超时按 124 返回，让断言给出"卡死"而不是把整组用例拖崩。
        out = (e.stdout or b"").decode("utf-8", "replace") if e.stdout else ""
        return 124, out + "\n[timeout]"
    out = (p.stdout or b"").decode("utf-8", "replace")
    return p.returncode, out


def make_project(dst, name="PackTest"):
    """复制一个模板工程 + 写 venn.project.json（模拟"新建游戏"的产物）。"""
    shutil.copytree(TEMPLATE, dst)
    proj = {
        "format": 1,
        "name": name,
        "template": "third_person",
        "gameType": "third_person",
        "startScene": "scenes/main.json",
        "code": {"mode": "vks", "root": "scripts"},
        "engine": {"name": "venn", "config": "Debug"},
    }
    with open(os.path.join(dst, "venn.project.json"), "w", encoding="utf-8") as f:
        json.dump(proj, f, ensure_ascii=False, indent=2)
    # 故意放两个**没人引用**的孤儿：闭包必须把它们剔掉。
    # ⚠ 不靠"模板里恰好有孤儿"来验 —— 模板随时会变，那时这条会变成假绿。
    with open(os.path.join(dst, "assets", "textures", "orphan_probe.png"), "wb") as f:
        f.write(b"\x89PNG orphan")
    with open(os.path.join(dst, "assets", "scenes", "unused_stage.json"), "w",
              encoding="utf-8") as f:
        f.write('{"entities": []}\n')
    return dst


# ------------------------------------------------------------------ G 编辑器入口
user32 = ctypes.windll.user32


def _lparam(x, y):
    return (int(x) & 0xFFFF) | ((int(y) & 0xFFFF) << 16)


def _c2s(hwnd, x, y):
    class P(ctypes.Structure):
        _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]
    p = P(int(x), int(y))
    user32.ClientToScreen(hwnd, ctypes.byref(p))
    return p.x, p.y


MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004


def click(hwnd, x, y):
    """点一下（客户区坐标）。

    ⚠⚠ 这里必须用 **`mouse_event`（真实输入）**，不能用
      `PostMessage(WM_LBUTTONDOWN)`：菜单条的展开靠 `IsMouseClicked`，而
      ImGui 的 `io.MousePos` 由 GLFW 的**光标回调**更新 —— 那条回调只在窗口
      有焦点时生效。`PostMessage` 绕过了真实输入队列，实测"单独手跑 3/3 成功、
      连着跑在 I 组之后 0/3 成功"（同一套坑在 `verify_add_menu` 里已经用
      `mouse_event` 绕过一次了，这次是照着它改的）。
    """
    user32.SetCursorPos(*_c2s(hwnd, x, y))
    time.sleep(0.3)
    user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.12)
    user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
    time.sleep(0.6)


def anchor(tap, name, timeout=12.0, min_w=60.0):
    """等一个 `NAME=(x0,y0)-(x1,y1)` 锚点。

    `min_w` 过滤掉"菜单刚展开那一帧量出来的 9px 占位矩形" —— 按钮的矩形在
    布局定下来之前会先小后大，按小的那个中心点是点空的（实测踩过）。
    """
    end = time.time() + timeout
    got = None
    while time.time() < end:
        with tap.lock:
            for line in tap.lines:
                for m in ANY_RE.finditer(line):
                    if m.group(1) == name:
                        got = tuple(float(m.group(i)) for i in range(2, 6))
        if got and (got[2] - got[0]) >= min_w:
            return got
        time.sleep(0.15)
    return got


def group_editor_menu(proj_dir):
    """G：编辑器 `File ▸ Package project...` 真的能出包。

    为什么必须验这一条：`--package` 命令行能跑，只证明**内核**对；而用户
    的路径是菜单。菜单项接错（没接上 `packageProject` / 取错工程根 /
    拿 Editor.exe 当游戏 exe）在命令行那边完全看不出来。
    """
    env = dict(os.environ)
    env.update({"MYVK_NO_WINDOW_SAVE": "1", "MYVK_LOG_RECTS": "1",
                "MYVK_EDITOR_NO_HUD": "1", "MYVK_EDITOR_PROJECT": proj_dir})
    env.pop("MYVK_EDITOR_LAYOUT", None)
    env.pop("MYVK_ASSET_DIR", None)
    kill_stale_editors()
    proc = subprocess.Popen([EDITOR], cwd=BIN, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True,
                            encoding="utf-8", errors="replace", bufsize=1)
    tap = LogTap()
    tap.start(proc)
    try:
        hwnd = find_window("Venn Editor", timeout=60)
        if not hwnd:
            check("G0 编辑器窗口出现", False, "没找到 Venn Editor")
            return
        force_foreground(hwnd)
        time.sleep(4.0)

        mf = anchor(tap, "MENU-FILE", 12.0, 30.0)
        check("G1 File 菜单锚点在（顶栏接上了）", mf is not None, str(mf))
        if not mf:
            return
        # ⚠ 点开菜单这一步**要重试**：ImGui 的菜单条靠 `IsMouseClicked`，
        #   而 MousePos 要经过 WM_MOUSEMOVE 才更新；机器忙的时候第一下会
        #   落在"上一帧的陈旧坐标"上（表现为菜单没开、也没有任何报错）。
        #   实测：单独手跑 3/3 成功，连着跑在 I 组之后 1/3 失败 —— 典型相位问题。
        mp = None
        for attempt in range(3):
            force_foreground(hwnd)
            time.sleep(0.25)
            click(hwnd, (mf[0] + mf[2]) * 0.5, (mf[1] + mf[3]) * 0.5)
            # 两个入口都要在（G29-c 把 pak 单列了一条）
            mpp = anchor(tap, "MENU-PACKAGE-PAK", 5.0, 60.0)
            check("G2b File 菜单里也有 Package project as .pak…",
                  mpp is not None, str(mpp))
            mp = anchor(tap, "MENU-PACKAGE", 6.0, 60.0)
            if mp:
                if attempt:
                    print("    （第 %d 次才点开菜单）" % (attempt + 1))
                break
        check("G2 File 菜单里有 Package project…", mp is not None, str(mp))
        if not mp:
            return
        click(hwnd, (mp[0] + mp[2]) * 0.5, (mp[1] + mp[3]) * 0.5)

        # 同步执行：321 个文件 / 38 MB 实测 ~8 秒（阻塞主线程，期间界面冻住）
        end = time.time() + 40.0
        okline = ""
        while time.time() < end:
            with tap.lock:
                for l in tap.lines:
                    if "PACKAGE-OK" in l:
                        okline = l.strip()
            if okline:
                break
            time.sleep(0.5)
        check("G3 点一下真的出了包（PACKAGE-OK）", bool(okline),
              okline[-110:] if okline else "40s 内没有 PACKAGE-OK")
        dist = os.path.join(proj_dir, "dist", os.path.basename(proj_dir))
        check("G4 产物落在 <工程根>/dist/<Name> 下",
              os.path.isfile(os.path.join(dist, os.path.basename(proj_dir) + ".exe")),
              dist)
    finally:
        try:
            proc.terminate()
            proc.wait(timeout=8)
        except Exception:
            proc.kill()
        kill_stale_editors()


def main():
    if not os.path.exists(GAME):
        print("找不到可执行文件:", GAME)
        return 2

    base = tempfile.mkdtemp(prefix="venn_pkg_")
    try:
        # ============================================================
        # A 自检
        # ============================================================
        rc, out = run([GAME, "--selftest", "package"], timeout=180)
        line = ""
        for l in out.splitlines():
            if "PACK-TOTAL" in l:
                line = l.strip()
        fails = [l.strip() for l in out.splitlines() if "[FAIL]" in l]
        ok = rc == 0 and "fail=0" in line
        check("A1 出包自检全绿（PACK-TOTAL fail=0）", ok,
              (line or "没有 PACK-TOTAL 行") + ("；" + fails[0] if fails else ""))
        for anchor in ("P1", "P5", "P6", "P8", "P11", "P13"):
            check("A2 断言 %s 跑过" % anchor, ("PACK-TEST %s " % anchor) in out, "")

        # ============================================================
        # B 真打一次
        # ============================================================
        proj = make_project(os.path.join(base, "Proj", "PackTest"))
        rc, out = run([GAME, "--package", proj], timeout=300)
        okline = ""
        for l in out.splitlines():
            if l.startswith("PACKAGE-OK"):
                okline = l.strip()
        check("B1 打包成功（PACKAGE-OK）", rc == 0 and bool(okline),
              okline or out.strip().splitlines()[-1][:120])
        if not okline:
            return finish()

        dist = os.path.join(proj, "dist", "PackTest")
        need = [("PackTest.exe", "exe"),
                (os.path.join("assets", "game.json"), "assets/game.json"),
                (os.path.join("assets", "scenes", "main.json"), "场景"),
                (os.path.join("config", "render.json"), "config/render.json"),
                ("build.json", "build.json"),
                ("README.txt", "README.txt")]
        for rel, label in need:
            check("B2 产物里有 %s" % label, os.path.isfile(os.path.join(dist, rel)),
                  rel)
        # ---- 引擎侧资产（第一版缺的就是这一撮）----
        shaders_dir = os.path.join(dist, "shaders")
        nsh = len(os.listdir(shaders_dir)) if os.path.isdir(shaders_dir) else 0
        check("B5 包里带了引擎 shaders（>100 个 .spv）", nsh > 100,
              "shaders=%d" % nsh)
        check("B6 包里带了引擎字体（assets/fonts 非空）",
              os.path.isdir(os.path.join(dist, "assets", "fonts")) and
              len(os.listdir(os.path.join(dist, "assets", "fonts"))) > 0, "")
        check("B7 包里带了 UI SDF 图集（assets/ui/fonts）",
              os.path.isdir(os.path.join(dist, "assets", "ui", "fonts")), "")

        # ---- G29-a 引用闭包 ----
        with open(os.path.join(dist, "build.json"), encoding="utf-8") as f:
            man0 = json.load(f)
        check("B8 闭包默认开（build.json closure=true）",
              man0.get("closure") is True, "closure=%s" % man0.get("closure"))
        check("B9 闭包真的剔掉了东西（pruned>=2）", man0.get("pruned", 0) >= 2,
              "pruned=%s index? files=%s" % (man0.get("pruned"), man0.get("totalFiles")))
        check("B10 孤儿贴图没进包（没人引用）",
              not os.path.exists(os.path.join(dist, "assets", "textures", "orphan_probe.png")), "")
        check("B11 孤儿场景没进包",
              not os.path.exists(os.path.join(dist, "assets", "scenes", "unused_stage.json")), "")
        check("B12 模板里只在注释提到过的脚本没进包",
              not os.path.exists(os.path.join(dist, "assets", "scripts", "pickup.vks")), "")
        # 反过来：**被引用**的一个都不能少（缺了就是玩家黑屏）
        for rel in ("assets/scenes/main.json", "assets/scripts/main.vks",
                    "assets/terrain/ground/terrain.vterrain",
                    "assets/terrain/ground/height.r32",
                    "assets/terrain/ground/splat.rgba8",
                    "assets/materials/template_ground.vmat",
                    "assets/textures/template_ground_albedo.png",
                    "assets/textures/template_ground_normal.png"):
            check("B13 被引用的 %s 在包里" % rel.replace("assets/", ""),
                  os.path.isfile(os.path.join(dist, rel.replace("/", os.sep))), "")

        # 编辑器那一份不能被带进去
        check("B3 产物里没有 venn.project.json（运行时不读它）",
              not os.path.exists(os.path.join(dist, "venn.project.json")), "")
        check("B4 产物里没有 Editor.exe",
              not os.path.exists(os.path.join(dist, "Editor.exe")), "")

        # ============================================================
        # C 清单对账
        # ============================================================
        with open(os.path.join(dist, "build.json"), encoding="utf-8") as f:
            man = json.load(f)
        on_disk = []
        for dp, _dn, fn in os.walk(dist):
            for n in fn:
                p = os.path.join(dp, n)
                rel = os.path.relpath(p, dist).replace("\\", "/")
                on_disk.append(rel)
        listed = [e["path"] for e in man["files"]]
        check("C1 清单条数 + 1（build.json 自己）== 磁盘文件数",
              len(listed) + 1 == len(on_disk),
              "listed=%d onDisk=%d" % (len(listed), len(on_disk)))
        check("C2 清单覆盖了每一个磁盘文件",
              set(on_disk) - {"build.json"} == set(listed),
              "差集=%s" % sorted(set(on_disk) - {"build.json"} - set(listed))[:5])
        bad = []
        for e in man["files"]:
            p = os.path.join(dist, e["path"].replace("/", os.sep))
            if not os.path.isfile(p) or os.path.getsize(p) != e["size"]:
                bad.append(e["path"])
        check("C3 每条 size 与磁盘一致", not bad, "不一致=%s" % bad[:3])
        check("C4 清单写明哈希算法", man.get("hashKind") == "fnv1a64",
              str(man.get("hashKind")))
        check("C4b 清单区分了引擎侧资产条数（engineFiles>0）",
              man.get("engineFiles", 0) > 0, "engineFiles=%s" % man.get("engineFiles"))
        # Debug 引擎必须留下警告（本机默认 Debug 构建 —— 这条是"真实现"而不是伪造）
        warns = man.get("warnings") or []
        has_dbg = any("Debug" in w for w in warns)
        check("C5 Debug 引擎 → 清单里带 WARNING（不可再分发的 CRT）", has_dbg,
              "warnings=%d" % len(warns))

        # ============================================================
        # D 干净目录测试
        # ============================================================
        fresh = os.path.join(base, "Fresh")
        shutil.copytree(dist, fresh)
        # 工程本体整个删掉：留下来的如果还能跑，才说明它不依赖源目录
        safe_rmtree(os.path.join(base, "Proj"))
        check("D0 源工程已删除", not os.path.exists(os.path.join(base, "Proj")), "")
        rc, out = run([os.path.join(fresh, "PackTest.exe")], cwd=fresh,
                      env_extra={"MYVK_FRAMES": "20"}, timeout=180)
        tail = "\n".join(out.strip().splitlines()[-3:])[:200]
        check("D1 干净目录里跑 20 帧，正常退出（rc=0）", rc == 0,
              "rc=%d %s" % (rc, tail))
        # ⚠ rc==0 还不够：Vulkan 校验层的"对象没销毁"是 **ERROR 级日志**，
        #   但只要不致命 rc 仍可能是 0。第一版就是缺 shaders → 24 条
        #   VUID-05137（image / device memory / framebuffer 未销毁）。
        nval = out.count("Validation Error")
        check("D1b 没有 Vulkan 校验错误（资源都销毁了）", nval == 0,
              "Validation Error x%d" % nval)
        # 缺引擎资产的第一现场是这几行 WARN
        miss = [l.strip() for l in out.splitlines()
                if ("not found" in l or "failed path=" in l) and "[WARN" in l]
        check("D1c 没有「资源找不到」类警告", not miss,
              "；".join(miss[:2])[:160])
        # 反向证据：它真的加载到了场景，而不是"起来就退"
        check("D2 日志里出现过场景加载（不是空启动）",
              ("SCENE" in out) or ("GAME-SCENE" in out) or ("scene" in out.lower()),
              "日志尾：%s" % tail.replace("\n", " | ")[:120])

        # ============================================================
        # E 反例：场景缺失
        # ============================================================
        proj2 = make_project(os.path.join(base, "Broken", "Broken"), "Broken")
        os.remove(os.path.join(proj2, "assets", "scenes", "main.json"))
        rc, out = run([GAME, "--package", proj2], timeout=120)
        check("E1 场景缺失 → 退出码 2 且 PACKAGE-FAIL", rc == 2 and "PACKAGE-FAIL" in out,
              "rc=%d" % rc)
        check("E2 缺的正是那个场景（报错里点名）",
              "scenes/main.json" in out, "")
        check("E3 失败不产出 dist",
              not os.path.exists(os.path.join(proj2, "dist", "Broken")), "")

        # ============================================================
        # F 反例：目标目录非空
        # ============================================================
        proj3 = make_project(os.path.join(base, "Occupied", "Occ"), "Occ")
        outdir = os.path.join(proj3, "dist", "Occ")
        os.makedirs(outdir)
        keep = os.path.join(outdir, "keep.txt")
        with open(keep, "w", encoding="utf-8") as f:
            f.write("do not delete me\n")
        rc, out = run([GAME, "--package", proj3], timeout=120)
        check("F1 目标目录非空 → 拒绝（rc=2）", rc == 2, "rc=%d" % rc)
        check("F2 原目录内容没被动", os.path.isfile(keep), "keep.txt")
        # ============================================================
        # H 对照：--no-closure（整棵拷）—— 证明"是闭包在起作用"
        # ============================================================
        # 没有这一组，B8~B12 只证明"包里没有孤儿"，证明不了"闭包干的"
        # （也可能是因为别的原因没拷进去，比如漏了整目录还恰好没人发现）。
        proj4 = make_project(os.path.join(base, "Whole", "WholePak"), "WholePak")
        rc, out = run([GAME, "--package", proj4, "--no-closure"], timeout=300)
        dist4 = os.path.join(proj4, "dist", "WholePak")
        check("H1 --no-closure 也能打出包", rc == 0 and os.path.isdir(dist4),
              "rc=%d" % rc)
        check("H2 整棵拷时孤儿贴图**在**包里（闭包才是剔除的那一方）",
              os.path.isfile(os.path.join(dist4, "assets", "textures",
                                          "orphan_probe.png")), "")
        check("H3 整棵拷时孤儿脚本**在**包里",
              os.path.isfile(os.path.join(dist4, "assets", "scripts", "pickup.vks")), "")
        with open(os.path.join(dist4, "build.json"), encoding="utf-8") as f:
            man4 = json.load(f)
        check("H4 build.json 记 closure=false / pruned=0",
              man4.get("closure") is False and man4.get("pruned", -1) == 0,
              "closure=%s pruned=%s" % (man4.get("closure"), man4.get("pruned")))

        # ============================================================
        # I 负例：悬空引用 → 打包失败（G29 验收第 2 条）
        # ============================================================
        proj5 = make_project(os.path.join(base, "Dangle", "Dangle"), "Dangle")
        sc = os.path.join(proj5, "assets", "scenes", "main.json")
        with open(sc, encoding="utf-8") as f:
            scene = f.read()
        # 在场景里加一个指向不存在材质的引用（真实项目里最常见的一种）
        scene = scene.replace('"entities":', '"strayMaterial": "materials/ghost",\n  "entities":', 1)
        with open(sc, "w", encoding="utf-8") as f:
            f.write(scene)
        rc, out = run([GAME, "--package", proj5], timeout=180)
        check("I1 悬空引用 → 退出码 2（不是打个包了事）", rc == 2, "rc=%d" % rc)
        check("I2 报错点名那个引用（materials/ghost）", "materials/ghost" in out, "")
        check("I3 失败不产出 dist",
              not os.path.exists(os.path.join(proj5, "dist", "Dangle")), "")
        # 同样的工程 + --no-closure：**能打出来**（说明是闭包在把关，不是别的原因挂的）
        rc2, _ = run([GAME, "--package", proj5, "--no-closure"], timeout=180)
        check("I4 同一工程 --no-closure 能打出来（闭包才是把关的那一方）", rc2 == 0,
              "rc=%d" % rc2)

        # ============================================================
        # J 组：assets.pak（G29-b）
        # ============================================================
        # 这一组只做一件树模式做不了的事：**证明运行时真的从包里读**。
        # 文件存在、大小变小都不能证明这一点 —— 包可能压根没被挂上，
        # 游戏靠磁盘上残留的 assets/ 目录跑得很开心。
        # 所以判据是「干净的目录 + 空缓存 + 日志里的 ASSET-PAK mounted +
        # 缓存目录里真的落了文件 + 0 条校验错误」四条一起看。
        proj6 = make_project(os.path.join(base, "PakProj", "PakOut"), "PakOut")
        rc, out = run([GAME, "--package", proj6, "--pak"], timeout=300)
        dist6 = os.path.join(proj6, "dist", "PakOut")
        check("J1 --pak 出包成功", rc == 0, "rc=%d %s" % (rc, out.strip()[-120:]))
        # pak 里装两个根：assets/ 与 shaders/（G29-c），所以**两棵树都得消失**
        check("J2 产物有 game.pak、没有 assets/ 与 shaders/ 目录",
              os.path.isfile(os.path.join(dist6, "game.pak")) and
              not os.path.exists(os.path.join(dist6, "assets")) and
              not os.path.exists(os.path.join(dist6, "shaders")), "")
        check("J3 日志报告了 pak 条目数（PACKAGE-PAK）",
              "PACKAGE-PAK" in out, "")
        with open(os.path.join(dist6, "build.json"), encoding="utf-8") as f:
            man6 = json.load(f)
        check("J4 build.json 有 pak 段且 entries>0",
              man6.get("pak", {}).get("entries", 0) > 0,
              str(man6.get("pak")))
        check("J5 清单里不再有 assets/** shaders/** 条目（只记 game.pak）",
              all(not e["path"].startswith(("assets/", "shaders/"))
                  for e in man6["files"]), "")
        check("J6 包里字节小于原始总和（真的压了）",
              man6["pak"]["packedBytes"] < man6["pak"]["rawBytes"],
              "%s -> %s" % (man6["pak"]["rawBytes"], man6["pak"]["packedBytes"]))

        # ---- 拷到别处 + 删源工程 + 空缓存，然后跑 ----
        shipped = os.path.join(base, "PakShipped")
        shutil.copytree(dist6, shipped)
        safe_rmtree(os.path.join(base, "PakProj"))
        cache = os.path.join(base, "PakCache")
        env6 = {"MYVK_ASSET_CACHE": cache}
        env6["MYVK_FRAMES"] = "20"
        rc, out = run([os.path.join(shipped, "PakOut.exe")], cwd=shipped,
                      env_extra=env6, timeout=180)
        check("J7 包里没有任何残留 assets/ shaders/ 目录（拷出去就是纯包）",
              not os.path.exists(os.path.join(shipped, "assets")) and
              not os.path.exists(os.path.join(shipped, "shaders")), "")
        check("J8 冷启动 20 帧正常退出（rc=0）", rc == 0, "rc=%d" % rc)
        check("J9 运行时确实挂上了包（ASSET-PAK mounted）",
              "ASSET-PAK mounted" in out, "")
        nval = out.count("Validation Error")
        check("J10 0 条 Vulkan 校验错误", nval == 0, "x%d" % nval)
        # 场景/贴图都是从包里落的 —— 缓存目录非空就是"按需解包走通了"
        ncache = 0
        for root, _, files in os.walk(cache):
            ncache += len(files)
        check("J11 按需解包真的发生了（缓存里落了文件）", ncache >= 3,
              "%d files" % ncache)
        # ⚠ 判据必须比**我们自己传进去的那个缓存根**，不能比字面量 "assetcache"：
        #   `MYVK_ASSET_CACHE` 一旦覆盖，路径里就没有 "assetcache" 这个词了
        #   （第一版就是这么白红了一次）。
        norm_out = out.replace("\\", "/")
        norm_cache = cache.replace("\\", "/")
        check("J12 加载是从我们自己指定的缓存根读的",
              norm_cache in norm_out, norm_cache)
        # ---- shader 进包（G29-c）----
        # 判据不能只是"包里有 .spv"：真正的风险是**运行时从缓存里读 .spv**，
        # 而缓存里没有的话 Vulkan 管线根本建不出来（rc 会变，日志一堆 VUID）。
        check("J13 包里的条目数覆盖了 shader（>250）",
              man6["pak"]["entries"] > 250, "entries=%d" % man6["pak"]["entries"])
        # ⚠ 判据是 **0**，不是"有"：shader 走 `pak:` 虚拟路径**直读包内存**，
        #   一个 .spv 都不该落盘。落盘版本（G29-c 第一版）实测会写 210 个
        #   / 13 MB —— 这就是"两实现不一样的那个量"。
        nspv = 0
        for root, _, files in os.walk(cache):
            nspv += sum(1 for f in files if f.endswith(".spv"))
        check("J14 shader 直读包内存（缓存里 0 个 .spv）", nspv == 0,
              "%d spv" % nspv)
        # ⚠ 不要断言"顶层正好 5 项"：跑过一次之后 cwd 里会多出 `imgui.ini`
        #   （运行时把 ImGui 布局写在当前目录，与出包无关）。断言"该有的都有、
        #   不该有的没有"，而不是数个数 —— 否则这条会因为**跑过一遍**而红。
        top = set(os.listdir(shipped))
        need = {"FinalPak.exe", "game.pak", "config", "build.json", "README.txt"}
        if not top.issuperset(need):
            need = {"PakOut.exe", "game.pak", "config", "build.json", "README.txt"}
        check("J15 dist 顶层是该有的那几项（exe/pak/config/build.json/README）",
              need.issubset(top) and "assets" not in top and "shaders" not in top,
              str(sorted(top)))

        # ============================================================
        # G 编辑器菜单入口（GUI：一次只能跑一个，放在最后）
        # ============================================================
        uiproj = make_project(os.path.join(base, "UiProj", "UiPak"), "UiPak")
        group_editor_menu(uiproj)
    finally:
        pass

    return finish(base)


def finish(base=None):
    if base and os.path.isdir(base):
        safe_rmtree(base)
    fails = [n for n, ok, _ in results if not ok]
    if fails:
        print("结果: FAIL: " + ", ".join(fails))
        return 1
    print("结果: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
