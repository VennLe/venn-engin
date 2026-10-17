"""端到端验证：G04（GameState 栈）+ G05（Runtime 启动配置与日志）。

两件事看似无关，其实是一根链条：**"运行时能不能在没有编辑器、没有任何
环境变量的机器上，按一份配置文件自己起起来"**。G04 管"起起来之后阶段对不对"，
G05 管"凭什么是这份配置、日志落在哪"。

断言分组：

  A. **状态栈自检**（无窗口）`Game.exe --selftest states`
     A1 退出码 0；A2 GAME-STATE-TOTAL pass=5 fail=0
     A3 迁移序列**逐字**等于 `Boot>Loading>MainMenu>Playing>Paused>Playing>MainMenu`
        （验收条款原话；A3 比的是 GAME-STATE-TEST 行里 `seq=` 与 `expect=` 两段
         都等于这个常量，防止"两边一起改错"）
     A4 Paused 的阻断语义：`playingUpdates=0` `pausedUpdates=30`
        `worldRenders=30` `simulate=0` —— 即"Paused 时物理零漂移，
        但帧还在出"
     A5 栈空时 `empty()` 为真（宿主据此退出，不黑窗卡死）

  B. **真实迁移序列**（`--trace-states`，真起窗口）
     B1 默认流程 `Boot>Loading>MainMenu>Playing` —— ⚠ 必须**经过 MainMenu**，
        这是"清空 env 也能进主菜单"的字面证据
     B2 每次迁移都打出 `GAME-STATE <op> name=.. depth=.. top=..`
     B3 `--start-state mainmenu` 停在 `Boot>Loading>MainMenu`

  C. **G05：配置从哪来**
     C1 冷启动（**清空全部 MYVK_***）按 `assets/game.json` 起：
        `GAME-CONFIG ... source='..game.json'` + `GAME-SCENE ... entities=4`
     C2 环境变量覆盖 game.json
     C3 命令行覆盖环境变量（优先级 CLI > env > json > 默认）
     C4 场景不存在时**不崩**：进程仍退出码 0、`GAME-SCENE load failed` 留痕

  D. **G05：日志落在用户目录**
     D1 本次运行**新增**了一个日志文件，路径在 `%LOCALAPPDATA%/Venn/venn/logs`
        下、且**不在仓库/安装目录**里
     D2 该文件里有 `GAME-INIT` / `GAME-SCENE` / `GAME-LOOP-SUMMARY` / `GAME-EXIT`
        —— 证明文件 sink 真的在工作（不是只有 stdout）
     D3 安装目录里没有多出任何 `.log`
     D4 `--help` **不**创建日志文件（main.cpp 里 `--help` 是先拦掉的）

⚠ 关于"Paused 时物理零漂移"：M1 还没有菜单 UI（G15 才有），命令行无法把
  运行中的进程切到 Paused，所以这一条只能由 A4 的自检驱动（它用
  `GameServices` 的 `simulate` 计数器当观测量：Paused 压住 Playing 时
  simulate 一次都没被调用 = 物理一步都没走，而 render 仍在跑）。
  这是**同一语义**的另一种观测面，不是"换个宽松判据糊过去"。

用法：
    python tools/verify_runtime_states.py
"""

import glob
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN_DIR = os.path.join(ROOT, "build", "bin")
GAME_EXE = os.path.join(BIN_DIR, "Game.exe")

SCENE = "scenes/sandbox.json"
SCENE_ENTITIES = 4            # sandbox.json：Ground + BoxA/B/C
FULL_SEQ = ("Boot>Loading>MainMenu>Playing>Paused>Playing>MainMenu")
TRACE_SEQ = "Boot>Loading>MainMenu>Playing"
MENU_SEQ = "Boot>Loading>MainMenu"

SEQ_RE = re.compile(r"GAME-STATE-SEQ\s+(\S+)")
TEST_SEQ_RE = re.compile(r"GAME-STATE-TEST seq=(\S+) expect=(\S+) result=(\w+)")
PAUSED_RE = re.compile(r"GAME-STATE-TEST paused (.*?) result=(\w+)")
CONFIG_RE = re.compile(r"GAME-CONFIG scene='([^']*)'.*?source='([^']*)'")
SCENE_RE = re.compile(r"GAME-SCENE path=(.+?) entities=(\d+) skipped=(\d+)")
STATE_RE = re.compile(r"GAME-STATE (push|pop|replace) name=(\S+) depth=(\d+) "
                      r"top=(\S+)")


def clean_env(**extra):
    """剥掉全部 `MYVK_*` 再叠 extra —— 测试的输入必须自述。"""
    env = {k: v for k, v in os.environ.items() if not k.startswith("MYVK_")}
    env.update({k: str(v) for k, v in extra.items()})
    return env


def run(args=(), env_extra=None, timeout=240):
    p = subprocess.run([GAME_EXE] + list(args), cwd=BIN_DIR,
                       env=clean_env(**(env_extra or {})),
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       timeout=timeout)
    return p.returncode, p.stdout.decode("utf-8", errors="replace")


def log_dir():
    """与 runtime/RuntimeLog.cpp 的 baseDir() 同一套回退顺序。"""
    for var in ("LOCALAPPDATA", "APPDATA", "TEMP"):
        base = os.environ.get(var)
        if base:
            return os.path.join(base, "Venn", "venn", "logs")
    return os.path.join(".", "Venn", "venn", "logs")


def snapshot_logs(d):
    out = {}
    for p in glob.glob(os.path.join(d, "*.log")):
        try:
            st = os.stat(p)
        except OSError:
            continue
        out[p] = (st.st_size, st.st_mtime_ns)
    return out


def main():
    if not os.path.isfile(GAME_EXE):
        print("找不到可执行文件:", GAME_EXE)
        return 2

    fails = []

    def check(name, ok, detail=""):
        print("  [%s] %s%s" % ("OK  " if ok else "FAIL", name,
                              ("  <- " + detail) if detail else ""))
        if not ok:
            fails.append(name)

    print("=" * 68)
    print("G04 · GameState 栈   /   G05 · 启动配置与日志")
    print("  Game.exe : %s" % GAME_EXE)
    print("=" * 68)

    # ---------------------------------------------------------- A. 自检
    print("\n--- A. 状态栈自检（--selftest states，无窗口）---")
    rc, txt = run(["--selftest", "states"])
    check("A1 退出码 0", rc == 0, "rc=%d" % rc)

    m = re.search(r"GAME-STATE-TOTAL pass=(\d+) fail=(\d+)", txt)
    if not m:
        check("A2 出现 GAME-STATE-TOTAL", False, "没找到")
    else:
        check("A2 GAME-STATE-TOTAL pass=5 fail=0",
              int(m.group(1)) == 5 and int(m.group(2)) == 0, m.group(0))

    mt = TEST_SEQ_RE.search(txt)
    check("A3 迁移序列逐字 == %s" % FULL_SEQ,
          bool(mt) and mt.group(1) == FULL_SEQ and mt.group(2) == FULL_SEQ
          and mt.group(3) == "PASS",
          mt.group(0) if mt else "没找到 GAME-STATE-TEST seq=")

    mp = PAUSED_RE.search(txt)
    want = {"playingUpdates": "0", "playingRenders": "30",
            "pausedUpdates": "30", "worldRenders": "30", "simulate": "0"}
    if not mp:
        check("A4 Paused 阻断语义", False, "没找到 paused 行")
    else:
        kv = dict(re.findall(r"(\w+)=(\d+)", mp.group(1)))
        bad = {k: (kv.get(k), v) for k, v in want.items() if kv.get(k) != v}
        check("A4 Paused：仿真停住（simulate=0）但画面仍在出（30 帧）",
              not bad and mp.group(2) == "PASS",
              "不符: %s" % bad if bad else mp.group(1))

    check("A5 栈空 → empty() 为真（宿主据此退出）",
          "栈空时 empty() 为真" in txt and "pass=" in txt)

    # ---------------------------------------------------------- B. 真实迁移
    print("\n--- B. 真实迁移序列（--trace-states）---")
    rc, txt = run(["--trace-states", "--frames", "5", "--scene", SCENE])
    ms = SEQ_RE.search(txt)
    check("B1 默认流程 %s" % TRACE_SEQ,
          bool(ms) and ms.group(1) == TRACE_SEQ and rc == 0,
          (ms.group(0) if ms else "没找到 GAME-STATE-SEQ") + " rc=%d" % rc)

    steps = STATE_RE.findall(txt)
    ops = [(op, name) for op, name, _d, _t in steps]
    want_ops = [("push", "Boot"), ("replace", "Loading"),
                ("replace", "MainMenu"), ("push", "Playing")]
    check("B2 每次迁移都留痕（GAME-STATE op/name/depth/top）",
          ops[:4] == want_ops, "%s" % ops[:4])

    rc, txt = run(["--trace-states", "--frames", "5", "--scene", SCENE,
                   "--start-state", "mainmenu"])
    ms = SEQ_RE.search(txt)
    check("B3 --start-state mainmenu 停在 %s" % MENU_SEQ,
          bool(ms) and ms.group(1) == MENU_SEQ and rc == 0,
          (ms.group(0) if ms else "没找到 GAME-STATE-SEQ") + " rc=%d" % rc)

    # ---------------------------------------------------------- C. 配置
    print("\n--- C. 配置从哪来（CLI > env > game.json > 默认）---")
    # C1：什么 MYVK_* 都不给 —— 只有 assets/game.json
    rc, txt = run(["--frames", "30"])
    mc = CONFIG_RE.search(txt)
    check("C1a 冷启动仍能找到 game.json 并生效",
          bool(mc) and mc.group(2).replace("\\", "/").endswith(
              "assets/game.json") and mc.group(1) == SCENE,
          mc.group(0) if mc else "没找到 GAME-CONFIG")
    msc = SCENE_RE.search(txt)
    check("C1b 冷启动加载 game.json 指定的场景（entities=%d）"
          % SCENE_ENTITIES,
          bool(msc) and int(msc.group(2)) == SCENE_ENTITIES and rc == 0,
          (msc.group(0) if msc else "没找到 GAME-SCENE") + " rc=%d" % rc)

    # C2：环境变量压过 game.json
    rc, txt = run(["--frames", "30"],
                  env_extra={"MYVK_RUNTIME_SCENE": "scenes/__nope__.json"})
    mc = CONFIG_RE.search(txt)
    check("C2 环境变量覆盖 game.json",
          bool(mc) and mc.group(1) == "scenes/__nope__.json",
          mc.group(1) if mc else "没找到 GAME-CONFIG")

    # C4：场景文件不存在 —— 出货版不该因此崩（窗口照开、日志说清原因）
    check("C4 场景不存在时不崩（rc=0 且留下 load failed）",
          rc == 0 and "GAME-SCENE load failed" in txt, "rc=%d" % rc)

    # C3：命令行压过环境变量
    rc, txt = run(["--frames", "30", "--scene", SCENE],
                  env_extra={"MYVK_RUNTIME_SCENE": "scenes/__nope__.json"})
    msc = SCENE_RE.search(txt)
    check("C3 命令行覆盖环境变量",
          bool(msc) and msc.group(1).replace("\\", "/").endswith(
              "scenes/sandbox.json") and int(msc.group(2)) == SCENE_ENTITIES,
          msc.group(1) if msc else "没找到 GAME-SCENE")

    # ---------------------------------------------------------- D. 日志
    print("\n--- D. 日志落在用户目录 ---")
    lgdir = log_dir()
    before = snapshot_logs(lgdir)
    bin_before = set(glob.glob(os.path.join(BIN_DIR, "*.log")))

    rc, txt = run(["--frames", "30", "--scene", SCENE])
    check("D0 本次运行退出码 0", rc == 0, "rc=%d" % rc)

    after = snapshot_logs(lgdir)
    changed = [p for p in after if p not in before or after[p] != before[p]]
    fresh = max(changed, key=lambda p: after[p][1]) if changed else None

    check("D1 新增了一个日志文件（在 %s）" % lgdir, fresh is not None,
          fresh if fresh else "没有新文件 / 内容没变")
    body = ""
    if fresh:
        inside_repo = os.path.abspath(fresh).lower().startswith(
            os.path.abspath(ROOT).lower() + os.sep)
        check("D1b 日志不在仓库/安装目录里（运行时不写安装目录）",
              not inside_repo, os.path.abspath(fresh))
        try:
            with open(fresh, "r", encoding="utf-8", errors="replace") as fh:
                body = fh.read()
        except OSError as e:
            print("      读取失败: %s" % e)
    # ⚠ 没有日志文件时**不能**静默跳过 D2 —— 那会让"日志根本没写"这件事
    #   看上去像通过（负向断言只能在有依据时才算数）。
    need = ["GAME-INIT", "GAME-SCENE", "GAME-LOOP-SUMMARY", "GAME-EXIT"]
    miss = [n for n in need if n not in body]
    check("D2 日志文件里有 %s" % "/".join(need),
          bool(fresh) and not miss,
          "没有日志文件可查" if not fresh
          else ("缺: %s" % miss if miss else "%d 字节" % len(body)))

    bin_after = set(glob.glob(os.path.join(BIN_DIR, "*.log")))
    check("D3 安装目录里没有多出 .log", bin_after == bin_before,
          str(sorted(bin_after - bin_before)))

    # D4：--help 在开日志之前就被拦掉了
    before = snapshot_logs(lgdir)
    rc, txt = run(["--help"])
    after = snapshot_logs(lgdir)
    check("D4 --help 不创建日志文件",
          rc == 0 and before == after,
          "rc=%d 差异=%s" % (rc, [p for p in after
                                  if p not in before or after[p] != before[p]]))

    # ---------------------------------------------------------- 结论
    print("\n" + "=" * 68)
    if fails:
        print("结果: FAIL: %d 项未通过 -> %s" % (len(fails), fails))
        return 1
    print("结果: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
