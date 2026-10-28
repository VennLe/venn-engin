#!/usr/bin/env python3
"""G30 Shipping 构建剔除 —— 出货版的**成分**对不对。

为什么要单独一组用例：
    "shipping 版"最容易做成的样子是"加了宏，但没人验证它真的少东西"。
    宏写在 CMake 里、`#if` 写在代码里，两边任何一个写错，症状都是
    **游戏照常跑**（因为开发钩子本来就不用），于是这个东西会一直错下去。
    所以判据必须是**可数的**：

      · 出货 exe 的字节里 `--selftest` 出现 **0 次**，而 dev exe 里 > 0
        （后者是对照区 —— 没有它，"扫到 0 次"也可能只是扫描方法失效）；
      · `build-ship/build.ninja` 里根本没有 Editor.exe / Launcher.exe 目标
        （"编了但不用" = 迟早有人拷错）；
      · 收到的包 `build.json` 里 `flavor=shipping`，且 `debug/Game.pdb`
        在包里、exe 旁边没有。

验什么（A~D 共 **39** 条）：
  A  dev 基线（快，不建窗口）：`--version` 报 dev；四个开发字符串都在
  B  `make ship` 出 build-ship/：目标清单里没有编辑器/启动器
  C  出货 exe 自身：`--version` 报 shipping；开发字符串 0 次；
     `--help` 里没有 `--selftest`；给 `--selftest pak` 也不进自检
  D  真打一个包（用 dev 版当工具、指定出货 exe 当内容）：
     `build.json.flavor=shipping`、`debug/Game.pdb` 在包里、顶层没有 pdb；
     干净目录跑 20 帧：rc=0、`validation=off`、0 条校验错误、
     设 `MYVK_RUNTIME_SELFTEST` 也不进自检

⚠ 不验什么：
  * 符号表是否真的被剥离（MSVC 的 exe 本来就不含 pdb 内容，pdb 是单独文件）
  * 崩溃转储（G31 还没做）
  * 跨平台：目前只有 Windows/MSVC 这条路

用法：
    python tools/verify_shipping.py            # 需要时自动 `make ship`
    python tools/verify_shipping.py --no-build # 只跑判定（build-ship 已存在时）
    python tools/verify_shipping.py --rebuild  # 强制重建
"""

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from safe_fs import safe_rmtree                                   # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
DEV_EXE = os.path.join(BIN, "Game.exe")
SHIP_DIR = os.path.join(ROOT, "build-ship")
SHIP_BIN = os.path.join(SHIP_DIR, "bin")
SHIP_EXE = os.path.join(SHIP_BIN, "Game.exe")
TEMPLATE = os.path.join(ROOT, "templates", "third_person")

# 四个"只该出现在开发版里"的字符串。
# ⚠ 选它们的标准是**能被字节搜索命中**且**只在开发路径上**：
#   `--selftest`      命令行分支（RuntimeConfig.cpp）
#   `GAME-STATE-TOTAL` 自检输出（main.cpp）
#   `PACKAGE-OK`      出包子命令（main.cpp）
#   `MYVK_FIXED_DT`   确定性钩子（core/Time.h）
DEV_MARKERS = [b"--selftest", b"GAME-STATE-TOTAL", b"PACKAGE-OK",
               b"MYVK_FIXED_DT"]

results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print("[%s] %s  %s" % ("PASS" if ok else "FAIL", name, detail))
    return bool(ok)


def run(args, cwd=None, env_extra=None, timeout=180):
    env = dict(os.environ)
    env.pop("MYVK_EDITOR_PROJECT", None)
    env.pop("MYVK_RUNTIME_SELFTEST", None)
    env.pop("MYVK_FIXED_DT", None)
    if env_extra:
        env.update(env_extra)
    try:
        p = subprocess.run(args, cwd=cwd, env=env, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=timeout)
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"").decode("utf-8", "replace") if e.stdout else ""
        return 124, out + "\n[timeout]"
    return p.returncode, (p.stdout or b"").decode("utf-8", "replace")


def count_marker(path, needle):
    if not os.path.isfile(path):
        return -1
    with open(path, "rb") as f:
        return f.read().count(needle)


def make_project(dst, name="ShipTest"):
    shutil.copytree(TEMPLATE, dst)
    proj = {
        "format": 1,
        "name": name,
        "template": "third_person",
        "gameType": "third_person",
        "startScene": "scenes/main.json",
        "code": {"mode": "vks", "root": "scripts"},
        "engine": {"name": "venn", "config": "Release"},
    }
    with open(os.path.join(dst, "venn.project.json"), "w", encoding="utf-8") as f:
        json.dump(proj, f, ensure_ascii=False, indent=2)
    return dst


def newest_source_mtime():
    """源码/构建脚本里最新的 mtime —— 用来判断 build-ship 是否过期。"""
    newest = 0.0
    for sub in ("engine", "runtime", "assets/shaders"):
        base = os.path.join(ROOT, sub)
        for root, _, files in os.walk(base):
            for fn in files:
                if fn.endswith((".cpp", ".h", ".hpp", ".vert", ".frag",
                                ".comp", ".glsl")):
                    newest = max(newest, os.path.getmtime(os.path.join(root, fn)))
    for fn in ("CMakeLists.txt", "Makefile"):
        p = os.path.join(ROOT, fn)
        if os.path.isfile(p):
            newest = max(newest, os.path.getmtime(p))
    return newest


def need_build(force):
    if force or not os.path.isfile(SHIP_EXE):
        return True
    return os.path.getmtime(SHIP_EXE) < newest_source_mtime()


# ------------------------------------------------------------------ A dev 基线
def group_dev():
    print("\n== A dev 基线 ==")
    rc, out = run([DEV_EXE, "--version"], timeout=60)
    check("A1 dev 版 --version 能跑（rc=0）", rc == 0, "rc=%d" % rc)
    check("A2 dev 版的构建戳里有 flavor=dev", " dev " in out or "dev" in out,
          out.strip()[:100])
    check("A3 dev 版的戳里有 git 号（不是 unknown）",
          "git=" in out and "git=unknown" not in out, out.strip()[:100])
    # 对照区：dev exe 里这几个串**必须都在**。
    # 没有这一半，"出货版扫到 0" 就可能只是"这个扫描根本没工作"。
    for mk in DEV_MARKERS:
        n = count_marker(DEV_EXE, mk)
        check("A4 dev exe 含 %s（对照区）" % mk.decode(), n > 0, "x%d" % n)
    # 开发钩子真的还在：跑一个纯数值自检
    rc, out = run([DEV_EXE, "--selftest", "pak"], timeout=120)
    check("A5 dev 版 --selftest pak 仍可用（PASS + rc=0）",
          rc == 0 and "PASS" in out, "rc=%d" % rc)


# ------------------------------------------------------------------ B 构建
def group_build(built):
    print("\n== B shipping 构建 ==")
    if not built:
        print("  (build-ship 是最新的，跳过构建)")
    check("B1 build-ship/bin/Game.exe 存在", os.path.isfile(SHIP_EXE), SHIP_EXE)
    ninja = os.path.join(SHIP_DIR, "build.ninja")
    text = ""
    if os.path.isfile(ninja):
        with open(ninja, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
    check("B2 目标清单里没有 Editor.exe", "Editor.exe" not in text,
          "found" if "Editor.exe" in text else "")
    check("B3 目标清单里没有 Launcher.exe", "Launcher.exe" not in text,
          "found" if "Launcher.exe" in text else "")
    check("B4 目标清单里有 Game.exe", "Game.exe" in text, "")
    check("B5 build-ship/bin 里没有 Editor.exe / Launcher.exe",
          not os.path.isfile(os.path.join(SHIP_BIN, "Editor.exe")) and
          not os.path.isfile(os.path.join(SHIP_BIN, "Launcher.exe")), "")
    # AssetC 是**构建期工具**（离线资产编译器），也不该出现在发行构建里。
    check("B5b 目标清单与产物里都没有 AssetC.exe",
          "AssetC.exe" not in text and
          not os.path.isfile(os.path.join(SHIP_BIN, "AssetC.exe")), "")
    # bin 里该只有 Game.exe（+ pdb / assets / shaders / templates 这些运行时资源）
    exes = sorted(f for f in os.listdir(SHIP_BIN) if f.lower().endswith(".exe"))
    check("B5c build-ship/bin 里只有 Game.exe 一个可执行文件",
          exes == ["Game.exe"], str(exes))
    # 符号：RelWithDebInfo + /DEBUG → 单独的 pdb
    check("B6 出货构建带独立的 pdb（Game.pdb）",
          os.path.isfile(os.path.join(SHIP_BIN, "Game.pdb")), "")


# ------------------------------------------------------------------ C 出货 exe
def group_exe():
    print("\n== C 出货 exe 自身 ==")
    rc, out = run([SHIP_EXE, "--version"], timeout=60)
    check("C1 shipping 版 --version 能跑（rc=0）", rc == 0, "rc=%d" % rc)
    check("C2 构建戳里 flavor=shipping", "shipping" in out, out.strip()[:100])
    check("C3 构建戳里 git 号非空", "git=" in out and "git=unknown" not in out,
          out.strip()[:100])
    # 核心判据：四个开发字符串一个都不许剩
    for mk in DEV_MARKERS:
        n = count_marker(SHIP_EXE, mk)
        check("C4 shipping exe 不含 %s（== 0）" % mk.decode(), n == 0,
              "x%d" % n)
    rc, out = run([SHIP_EXE, "--help"], timeout=60)
    check("C5 --help 能输出（rc=0）", rc == 0, "rc=%d" % rc)
    check("C6 --help 里没有 --selftest", "--selftest" not in out, "")
    check("C7 --help 里有 --version（出货版保留的排查入口）",
          "--version" in out, "")
    # 行为证据：给它 --selftest 它也不进自检，而是**照常跑游戏**
    # （带 --frames 5 让它自己退出，不靠超时 kill）
    rc, out = run([SHIP_EXE, "--selftest", "pak", "--frames", "5"], cwd=SHIP_BIN,
                  timeout=180)
    # ⚠ 判据不能是"输出里没有 selftest 这个词"：`GAME-CONFIG` 那行自带
    #   `selftest=-` 字段（配置摘要，与"认不认这个参数"无关）。第一版就是
    #   这么白红了一次。要看的是**有没有真的进自检**。
    check("C8 shipping 版无视 --selftest（没进自检：无 GAME-STATE-TOTAL / 无断言行）",
          "GAME-STATE-TOTAL" not in out and "[PASS]" not in out, "rc=%d" % rc)
    check("C9 shipping 版照常启动了游戏（GAME-BUILD 行）",
          "GAME-BUILD" in out, "")
    check("C10 出货运行时**没有**开校验层（INSTANCE ... validation=off）",
          re.search(r"INSTANCE build=shipping.*validation=off", out) is not None,
          "")
    check("C11 出货运行时 0 条校验错误（Validation Error x0）",
          out.count("Validation Error") == 0,
          "x%d" % out.count("Validation Error"))


# ------------------------------------------------------------------ D 出包
def group_package(base):
    print("\n== D 用出货 exe 打一个包 ==")
    proj = make_project(os.path.join(base, "ShipProj", "ShipGame"))
    dist = os.path.join(base, "dist")
    # 用 **dev 版**当工具、指定 **shipping 版**当内容 —— 这正是构建机上的
    # 真实姿势：出包工具是开发构建，打进包的二进制是出货构建。
    rc, out = run([DEV_EXE, "--package", proj, "--out", dist, "--pak",
                   "--engine", SHIP_EXE], timeout=600)
    check("D1 出包成功（PACKAGE-OK）", rc == 0 and "PACKAGE-OK" in out,
          "rc=%d %s" % (rc, out.strip().splitlines()[-1][:120] if out.strip() else ""))
    man_path = os.path.join(dist, "build.json")
    man = {}
    if os.path.isfile(man_path):
        with open(man_path, "r", encoding="utf-8") as f:
            man = json.load(f)
    check("D2 build.json 记的是 flavor=shipping", man.get("flavor") == "shipping",
          str(man.get("flavor")))
    check("D3 build.json 里有 git 号", bool(man.get("git")), str(man.get("git")))
    check("D4 包里有 debug/Game.pdb", man.get("pdb") == "debug/Game.pdb" and
          os.path.isfile(os.path.join(dist, "debug", "Game.pdb")),
          str(man.get("pdb")))
    top = set(os.listdir(dist))
    check("D5 pdb 不在 dist 顶层（exe 旁边没有符号）",
          not any(f.lower().endswith(".pdb") for f in top), str(sorted(top)))
    check("D6 dist 顶层没有 Editor.exe", "Editor.exe" not in top, "")
    # 包里的 exe 必须**就是**出货那一份（逐字节）
    exe_in_dist = [f for f in top if f.lower().endswith(".exe")]
    same = False
    if exe_in_dist:
        with open(os.path.join(dist, exe_in_dist[0]), "rb") as f1:
            a = f1.read()
        with open(SHIP_EXE, "rb") as f2:
            b = f2.read()
        same = a == b
    check("D7 包里的 exe 与出货 exe 逐字节相同", same, str(exe_in_dist))

    # ---- 干净目录 + 空缓存跑一遍 ----
    fresh = os.path.join(base, "Fresh")
    shutil.copytree(dist, fresh)
    safe_rmtree(os.path.join(base, "ShipProj"))
    cache = os.path.join(base, "cache")
    rc, out = run([os.path.join(fresh, exe_in_dist[0] if exe_in_dist else "x.exe")],
                  cwd=fresh, timeout=240,
                  env_extra={"MYVK_FRAMES": "20", "MYVK_ASSET_CACHE": cache,
                             # 故意设上开发钩子：出货版必须**无视**它
                             "MYVK_RUNTIME_SELFTEST": "states",
                             "MYVK_FIXED_DT": "0.016"})
    tail = "\n".join(out.strip().splitlines()[-3:])[:200]
    check("D8 干净目录 20 帧正常退出（rc=0）", rc == 0, "rc=%d %s" % (rc, tail))
    check("D9 出货包里没有 Editor 的入口（没有『未知自检项』提示）",
          "未知自检项" not in out, "")
    check("D10 设了 MYVK_RUNTIME_SELFTEST 也没进自检（无 GAME-STATE-TOTAL）",
          "GAME-STATE-TOTAL" not in out, "")
    check("D11 0 条校验错误", out.count("Validation Error") == 0,
          "x%d" % out.count("Validation Error"))
    check("D12 真的加载了场景（不是空启动）",
          "SCENE" in out or "scene" in out.lower(), tail.replace("\n", " | ")[:120])


def finish(base=None):
    if base and os.path.isdir(base):
        safe_rmtree(base)
    fails = [n for n, ok, _ in results if not ok]
    if fails:
        print("结果: FAIL: " + ", ".join(fails))
        return 1
    print("结果: PASS")
    return 0


def main():
    if not os.path.isfile(DEV_EXE):
        print("找不到 %s —— 先 `make build`" % DEV_EXE)
        print("结果: FAIL: 缺少 dev 构建")
        return 1
    force = "--rebuild" in sys.argv
    no_build = "--no-build" in sys.argv

    group_dev()

    built = False
    if need_build(force) and not no_build:
        print("\n== 构建 build-ship（make ship；第一次要几分钟）==")
        rc, out = run(["make", "ship"], cwd=ROOT, timeout=1800)
        built = True
        tail = "\n".join(out.strip().splitlines()[-6:])
        check("B0 make ship 成功（rc=0）", rc == 0, "rc=%d %s" % (rc, tail[:200]))
        if rc != 0:
            print(tail)
    group_build(built)

    group_exe()

    base = tempfile.mkdtemp(prefix="venn_ship_")
    try:
        group_package(base)
    finally:
        pass
    return finish(base)


if __name__ == "__main__":
    sys.exit(main())
