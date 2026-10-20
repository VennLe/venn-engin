"""结构验证：G01（CMake 三目标拆分）+ G02（脚本引擎搬到 engine/script/）。

这一例**不起窗口、不开 Vulkan、不跑物理**——它只看"文件与链接关系"，
所以是毫秒级的。为什么值得单独一条用例：

  拆分这类结构性改动最容易"看起来做完了，其实没做完"：
    · `Editor.exe` 还能跑，因为引擎和编辑器**还在同一个 target 里**；
    · `Game.exe` 也能跑，因为它是**复制**了一份编辑器源码编译出来的；
    · `engine/` 里偷偷 include 了 `editor/`（单向依赖被破坏），
      在编辑器里编译得过，做运行时才发现链接不上。
  这三种病都不会让任何一条现有的 GUI 用例变红，而它们正好是
  `docs/ship_roadmap.md` §13 硬约束 1/2 要守的东西。

断言分组：

  A. **两个可执行文件并存**（G01 的验收条款原话）
     A1/A2 `build/bin/Editor.exe`、`build/bin/Game.exe` 都存在且非空

  B. **引擎库是唯一共享体**（读 `build/build.ninja` 的链接规则）
     B1 `VennEngine.lib` 的全部输入都在 `engine/` 下 —— 零 `editor/`、零 `runtime/`
     B2 `Editor.exe` 的输入里没有 `runtime/` 的 TU，且有 `editor/main.cpp.obj`
     B3 `Game.exe` 的输入里没有 `editor/` 的 TU，且有 `runtime/main.cpp.obj`
     B4 两个 exe **都**链接 `VennEngine.lib`（真共享，不是各编一份）
     B5 `VennRuntime` 的 TU 数 ≥ 5（主干：GameLoop/GameState/GameStateStack/
        RuntimeApp/RuntimeConfig/RuntimeLog/main 至少这些）

  C. **G02：脚本引擎已在引擎层**
     C1 `engine/script/ScriptEngine.{h,cpp}` 存在
     C2 `editor/script/` 目录已消失（`rmdir` 过）
     C3 头文件里是 `namespace vks`（不是 `editor`）—— 引擎层不能有编辑器命名空间
     C4 全仓再无 `editor/script/` 与 `editor::ScriptEngine` 这类旧引用
     C5 重命名历史可见：`git log --follow` 或暂存区的 `R` 记录

  D. **运行时能自报家门**（最便宜的一次进程启动）
     D1 `Game.exe --help` 退出码 0 且列出 `--scene` / `--selftest`

⚠ B 组读的是 `build/build.ninja`：它要求"已经 configure + build 过一次"。
  没构建过就报 SKIP（而不是 FAIL）并提示先跑 `make build` —— 这一例的
  价值全在"跟**本次**构建产物对账"，拿一份陈旧的 ninja 去断言只会误导。

用法：
    python tools/verify_runtime_build.py
"""

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN_DIR = os.path.join(ROOT, "build", "bin")
EDITOR_EXE = os.path.join(BIN_DIR, "Editor.exe")
GAME_EXE = os.path.join(BIN_DIR, "Game.exe")
NINJA = os.path.join(ROOT, "build", "build.ninja")

# 一个 exe 小不到这个尺寸就说明是空壳（真实产物 10MB+）
MIN_EXE_BYTES = 1_000_000
# VennRuntime 的主干 TU（G01 拆分时新建的 7 个文件里至少要有这些）
RUNTIME_TUS = [
    "runtime/main.cpp", "runtime/RuntimeApp.cpp", "runtime/RuntimeConfig.cpp",
    "runtime/RuntimeLog.cpp", "runtime/GameLoop.cpp", "runtime/GameState.cpp",
    "runtime/GameStateStack.cpp",
]


def read_ninja_targets(path):
    """从 build.ninja 里抽出"链接规则 → 输入文件列表"。

    只收 `build <target>: <RULE> <inputs> || <order-only>` 这一种形状。
    输入里混着 `.lib` / `.obj` / `.res`，统一把反斜杠换成 `/` 好做前缀判断。
    """
    out = {}
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if not line.startswith("build "):
                continue
            m = re.match(r"build (.+?): (\S+) (.*)$", line.rstrip("\r\n"))
            if not m:
                continue
            target, rule, rest = m.group(1), m.group(2), m.group(3)
            main = rest.split("||", 1)[0]
            out.setdefault(rule, []).append(
                (target, [p.replace("\\", "/") for p in main.split()]))
    return out


def find_target(targets, rule_pat):
    """按规则名正则找唯一一个目标；找不到/撞车都给个明确的错。"""
    hits = []
    for rule, entries in targets.items():
        if re.search(rule_pat, rule):
            hits.extend(entries)
    return hits


def git(*args):
    try:
        p = subprocess.run(["git", "-c", "core.quotepath=false"] + list(args),
                           cwd=ROOT, stdout=subprocess.PIPE,
                           stderr=subprocess.DEVNULL, timeout=30)
        if p.returncode != 0:
            return None
        return p.stdout.decode("utf-8", errors="replace")
    except (OSError, subprocess.SubprocessError):
        return None


def main():
    fails = []
    skips = []

    def check(name, ok, detail=""):
        print("  [%s] %s%s" % ("OK  " if ok else "FAIL", name,
                              ("  <- " + detail) if detail else ""))
        if not ok:
            fails.append(name)

    def skip(name, why):
        print("  [SKIP] %s  <- %s" % (name, why))
        skips.append(name)

    print("=" * 68)
    print("运行时骨架 · 结构验证（G01 三目标拆分 / G02 脚本引擎搬家）")
    print("  仓库: %s" % ROOT)
    print("=" * 68)

    # ---------------------------------------------------------- A. 双 exe
    print("\n--- A. 两个可执行文件并存（G01）---")
    for aid, label, path in (("A1", "Editor.exe", EDITOR_EXE),
                             ("A2", "Game.exe", GAME_EXE)):
        size = os.path.getsize(path) if os.path.isfile(path) else 0
        check("%s %s 存在且非空" % (aid, label), size >= MIN_EXE_BYTES,
              "%d 字节（要求 ≥ %d）" % (size, MIN_EXE_BYTES))

    # ---------------------------------------------------------- B. 链接关系
    print("\n--- B. 引擎库是唯一共享体（G01）---")
    if not os.path.isfile(NINJA):
        for n in ["B1", "B2", "B3", "B4", "B5"]:
            skip(n, "没有 build/build.ninja —— 先跑 make build")
    else:
        tg = read_ninja_targets(NINJA)
        engine = find_target(tg, r"STATIC_LIBRARY.*VennEngine")
        editor = find_target(tg, r"EXECUTABLE.*VennEditor")
        runtime = find_target(tg, r"EXECUTABLE.*VennRuntime")

        # ---- B1 ----
        if not engine:
            check("B1 VennEngine 静态库目标存在", False, "ninja 里没找到")
        else:
            srcs = [p for _, ins in engine for p in ins
                    if ".dir/" in p and p.endswith(".obj")]
            # ⚠ 大小写：ninja 里是 `CMakeFiles\VennEngine.dir\engine\...`，
            #   规范化后判断前缀必须**忽略大小写**（第一版就栽在这）。
            bad = [p for p in srcs
                   if not p.lower().startswith(
                       "cmakefiles/vennengine.dir/engine/")]
            leak = [p for p in srcs if "editor/" in p or "runtime/" in p]
            check("B1a VennEngine 的 TU 全在 engine/ 下（≥40 个）",
                  len(srcs) >= 40 and not bad,
                  "越界: %s" % bad[:5] if bad else "%d 个 TU" % len(srcs))
            check("B1b VennEngine 里零 editor/ 零 runtime/ 源文件", not leak,
                  "泄漏: %s" % leak[:5])

        # ---- B2 / B3 ----
        for bid, tag, hits, foreign, own_main in (
                ("B2", "Editor.exe", editor, "runtime/", "editor/main.cpp.obj"),
                ("B3", "Game.exe", runtime, "editor/", "runtime/main.cpp.obj")):
            if not hits:
                check("%s %s 链接规则存在" % (bid, tag), False,
                      "ninja 里没找到")
                continue
            _target, ins = hits[0]
            tus = [p for p in ins if ".dir/" in p and p.endswith(".obj")]
            # `editor/` / `runtime/` 只认 `.dir/<层>/` 那一段，别误伤
            # `CMakeFiles/VennEditor.dir/editor/...` 这种"目标名里带 editor"的
            bad = [p for p in tus if re.search(r"\.dir/" + foreign, p)]
            check("%sa %s 不编译来自 %s 的 TU" % (bid, tag, foreign), not bad,
                  "混入: %s" % bad[:5] if bad else "%d 个 TU" % len(tus))
            check("%sb %s 含 %s" % (bid, tag, own_main),
                  any(p.endswith(own_main) for p in tus),
                  "实际: %s" % [p.split("/")[-1] for p in tus][:3])

        # ---- B4 ----
        # ⚠ 库文件名随工具链变：MSVC → `VennEngine.lib`，MinGW → `libVennEngine.a`。
        #   只认 MSVC 的名字会让 MinGW 构建永远红（本机就是 MinGW）。
        ENGINE_LIB_NAMES = ("VennEngine.lib", "libVennEngine.a")
        for label, hits in (("Editor.exe", editor), ("Game.exe", runtime)):
            if not hits:
                continue
            _, ins = hits[0]
            linked = [n for n in ENGINE_LIB_NAMES if n in ins]
            check("B4 %s 链接引擎库" % label, bool(linked),
                  "输入里没有引擎库（找过 %s）" % (ENGINE_LIB_NAMES,)
                  if not linked else "")

        # ---- B5 ----
        if runtime:
            _, ins = runtime[0]
            # `CMakeFiles/VennRuntime.dir/runtime/main.cpp.obj` → `runtime/main.cpp`
            tus = {p.split(".dir/", 1)[-1][:-len(".obj")]
                   for p in ins if ".dir/" in p and p.endswith(".obj")}
            missing = [t for t in RUNTIME_TUS if t not in tus]
            check("B5 Game.exe 含 G01 的全部主干 TU（%d 个）" % len(RUNTIME_TUS),
                  not missing, "缺: %s" % missing if missing else
                  "%d 个 TU" % len(tus))

    # ---------------------------------------------------------- C. G02
    print("\n--- C. 脚本引擎已在引擎层（G02）---")
    for fn in ("ScriptEngine.h", "ScriptEngine.cpp"):
        p = os.path.join(ROOT, "engine", "script", fn)
        check("C1 engine/script/%s 存在" % fn, os.path.isfile(p))

    old_dir = os.path.join(ROOT, "editor", "script")
    check("C2 editor/script/ 已消失", not os.path.isdir(old_dir),
          "目录还在" if os.path.isdir(old_dir) else "")

    header = os.path.join(ROOT, "engine", "script", "ScriptEngine.h")
    if os.path.isfile(header):
        with open(header, "r", encoding="utf-8", errors="replace") as fh:
            src = fh.read()
        check("C3 头文件用 namespace vks（不是 editor）",
              re.search(r"^\s*namespace\s+vks\b", src, re.M) is not None
              and re.search(r"^\s*namespace\s+editor\b", src, re.M) is None)
        check("C4a 头文件不再自称 editor 脚本", "editor/script" not in src)

    # 全仓扫一遍旧引用（只扫源码，跳过 build/ 与文档）
    stale = []
    for dirpath, dirnames, filenames in os.walk(ROOT):
        dirnames[:] = [d for d in dirnames
                       if d not in ("build", ".git", "_venn_verify", "docs",
                                    ".workbuddy", "__pycache__")]
        for fn in filenames:
            if not fn.endswith((".cpp", ".h", ".hpp", ".inl")):
                continue
            full = os.path.join(dirpath, fn)
            try:
                with open(full, "r", encoding="utf-8", errors="replace") as fh:
                    txt = fh.read()
            except OSError:
                continue
            if "editor/script" in txt or "editor::ScriptEngine" in txt:
                stale.append(os.path.relpath(full, ROOT).replace("\\", "/"))
    check("C4b 源码里零 `editor/script` / `editor::ScriptEngine` 残留",
          not stale, "残留: %s" % stale[:5])

    # ---- C5：重命名历史 ----
    # 两种合法状态都算过：
    #   ① 已经提交 → `git log --follow` 能追到 editor/script/ 那条 R 记录
    #   ② 还只在暂存区 → `git status` 里已经是 R（rm 掉了不是删+加）
    # 都不成立才是 FAIL（那就是真拿"复制一份再删"糊弄过去的，历史断了）。
    log = git("log", "--follow", "--name-status", "--format=",
              "--", "engine/script/ScriptEngine.cpp")
    hist_ok = bool(log) and re.search(r"^R\d*\s+editor/script/ScriptEngine\.",
                                      log or "", re.M) is not None
    status = git("status", "--porcelain", "-M")
    stage_ok = bool(status) and re.search(
        r"^R[ M]?\s+editor/script/ScriptEngine\.\S+\s+->\s+engine/script/",
        status or "", re.M) is not None
    if log is None and status is None:
        skip("C5 重命名历史", "不是 git 仓库 / 没有 git")
    else:
        check("C5 git 记录的是 rename（不是删除+新增）", hist_ok or stage_ok,
              "已提交且可 follow" if hist_ok else
              ("暂存区已记为 R（提交后 git log --follow 可见）" if stage_ok
               else "git log/status 里都没有 rename 记录"))

    # ---------------------------------------------------------- D. 自报家门
    print("\n--- D. 运行时能自报家门 ---")
    if not os.path.isfile(GAME_EXE):
        check("D1 Game.exe --help", False, "exe 不存在")
    else:
        env = {k: v for k, v in os.environ.items()
               if not k.startswith("MYVK_")}
        try:
            p = subprocess.run([GAME_EXE, "--help"], cwd=BIN_DIR, env=env,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               timeout=60)
            txt = p.stdout.decode("utf-8", errors="replace")
            ok = (p.returncode == 0 and "--scene" in txt and "--selftest" in txt
                  and "Venn" in txt)
            check("D1 Game.exe --help 退出码 0 且列出运行参数", ok,
                  "rc=%d len=%d" % (p.returncode, len(txt)))
        except (OSError, subprocess.SubprocessError) as e:
            check("D1 Game.exe --help", False, str(e))

    # ---------------------------------------------------------- 结论
    print("\n" + "=" * 68)
    if fails:
        print("结果: FAIL: %d 项未通过 -> %s" % (len(fails), fails))
        return 1
    if skips:
        print("结果: PASS  （%d 项跳过：%s）" % (len(skips), ", ".join(skips)))
    else:
        print("结果: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
