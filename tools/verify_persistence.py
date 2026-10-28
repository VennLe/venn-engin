#!/usr/bin/env python3
"""G17-1 存档**文件层** + G17-2 存档**应用层**的端到端验收（无窗口、无设备）。

验什么：
  A  `Game.exe --selftest save` 的数值断言
     （结构指纹 / 格式逐字节往返 / v1→v2 迁移 / 原子写 / 校验 /
       增量 diff / 应用到已有世界 / 悬挂父修正 / 形状守卫）
  B  自检**真的落到磁盘**了：`%TEMP%/venn_save_selftest/` 下有 `slot1.sav`、
     内容是合法 JSON、`version == kSaveVersion`、`entities` 是数组且每条带
     16 位 guid。`.bak` 也存在（写第二遍时备份出来的）。
     ⚠ 这一组不是重复 A：A 组可以在"写到一个不存在的目录"时全绿（如果实现
        悄悄把文件写进了别处 / 或者只写进了内存），只有看磁盘才能发现。

不验什么：
  * 槽位目录（`%LOCALAPPDATA%`）/ CLI（`--save/--load/--slot`）—— G17-3
  * 崩在 rename 之前（需要"自我杀进程后父进程断言"，纯 C++ 自检做不到）——
    待补 `--selftest save-crash` + 本脚本起子进程。
"""

import json
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verify_shader_variants import ROOT  # noqa: E402

BIN_DIR = os.path.join(ROOT, "build", "bin")
GAME_EXE = os.path.join(BIN_DIR, "Game.exe")
SAVE_DIR = os.path.join(tempfile.gettempdir(), "venn_save_selftest")
SAVE_PATH = os.path.join(SAVE_DIR, "slot1.sav")
BAK_PATH = SAVE_PATH + ".bak"

# 与 `engine/scene/SaveGame.h` 的 `kSaveVersion` 必须一致。
# ⚠ 升级存档版本时这里要跟着改 —— 断言"文件里写着当前版本"就是本条用例的价值。
EXPECT_VERSION = 2

# `--selftest save` 的断言条数下限。加断言就往上抬，**不要**往下调：
# 这条下限是"整组被删掉"的唯一探测器（总数由别组凑够时它才会红）。
MIN_SELFTEST = 77

results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print("[%s] %s  %s" % ("PASS" if ok else "FAIL", name, detail))
    return bool(ok)


def run_game(args, wait_s=45.0):
    env = dict(os.environ)
    env.pop("MYVK_LOG_DT", None)
    proc = subprocess.Popen([GAME_EXE] + args, cwd=BIN_DIR, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        out, _ = proc.communicate(timeout=wait_s)
    except subprocess.TimeoutExpired:
        proc.kill()
        try:
            out, _ = proc.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            out = b""
    return (out or b"").decode("utf-8", "replace").replace("\x00", "")


def group_selftest():
    out = run_game(["--selftest", "save"], wait_s=60.0)
    total = re.search(r"SAVE-TOTAL pass=(\d+) fail=(\d+)", out)
    fails = re.findall(r"SAVE-TEST (\S+).*result=FAIL", out)
    ok = total is not None and total.group(2) == "0" and int(total.group(1)) >= MIN_SELFTEST
    check("A1 `--selftest save` 全绿（>=%d 条）" % MIN_SELFTEST, ok,
          (total.group(0) if total else "没有 SAVE-TOTAL 行") +
          ("；FAIL=%s" % ",".join(fails) if fails else ""))

    # 分组存在性：只数总数的话"整组被删掉"也能靠别组凑够
    for group_name, first in (("hash", "hash_order_independent"),
                              ("format", "format_roundtrip_bytes"),
                              ("migrate", "migrate_v1_keeps_old"),
                              ("file", "file_bak_fallback"),
                              ("incremental", "diff_no_index_coupling"),
                              ("apply", "apply_orphan_parent_fixed"),
                              ("shape", "diff_entry_identical_to_full")):
        check("A2 分段 %s 有断言（%s）" % (group_name, first),
              ("SAVE-TEST %s " % first) in out, "锚点%s" % ("" if first in out else "缺"))

    # 迁移必须**真的跑过**（日志里出现 SAVE-MIGRATE）── 这是"迁移表不是空跑"的证据
    check("A3 迁移真的执行过（SAVE-MIGRATE 锚点）", "SAVE-MIGRATE" in out,
          "锚点%s" % ("" if "SAVE-MIGRATE" in out else "缺"))

    # 应用层**真的跑过**：`SAVE-APPLY` 与漂移两条锚点。
    # ⚠ 只看数值断言数不够 —— 自检可以"断言全绿"而应用路径一次都没走
    #   （比如 `applySaveGame` 早退）。
    check("A4 应用层真的执行过（SAVE-APPLY 锚点）", "SAVE-APPLY " in out,
          "锚点%s" % ("" if "SAVE-APPLY " in out else "缺"))
    check("A5 悬挂父修正真的触发过（SAVE-ORPHAN 锚点）", "SAVE-ORPHAN " in out,
          "锚点%s" % ("" if "SAVE-ORPHAN " in out else "缺"))
    check("A6 关卡漂移真的判过（SAVE-SCENE-DRIFT 锚点）", "SAVE-SCENE-DRIFT" in out,
          "锚点%s" % ("" if "SAVE-SCENE-DRIFT" in out else "缺"))
    check("A7 漂移拒读真的触发过（SAVE-DRIFT-REJECT 锚点）", "SAVE-DRIFT-REJECT" in out,
          "锚点%s" % ("" if "SAVE-DRIFT-REJECT" in out else "缺"))
    return out


def group_on_disk():
    if not os.path.isfile(SAVE_PATH):
        check("B1 存档文件落盘", False, SAVE_PATH)
        return
    check("B1 存档文件落盘", True, "%d bytes" % os.path.getsize(SAVE_PATH))

    try:
        with open(SAVE_PATH, "r", encoding="utf-8") as f:
            doc = json.load(f)
    except Exception as ex:                                   # noqa: BLE001
        check("B2 存档是合法 JSON", False, repr(ex)[:60])
        return
    check("B2 存档是合法 JSON", True, "keys=%d" % len(doc))

    check("B3 版本号 == %d" % EXPECT_VERSION, doc.get("version") == EXPECT_VERSION,
          "version=%s" % doc.get("version"))
    check("B4 必填字段齐全",
          all(k in doc for k in ("game", "slot", "timestamp", "scenePath",
                                 "sceneHash", "entities", "globals")),
          "缺=%s" % ",".join(k for k in ("game", "slot", "timestamp", "scenePath",
                                          "sceneHash", "entities", "globals")
                             if k not in doc))

    ents = doc.get("entities")
    check("B5 entities 是数组", isinstance(ents, list), type(ents).__name__)
    if isinstance(ents, list) and ents:
        guid_ok = all(isinstance(e.get("guid"), str) and len(e["guid"]) == 16
                      for e in ents if isinstance(e, dict))
        check("B6 每条 guid 都是 16 位 hex", guid_ok, "n=%d" % len(ents))

    # `.bak` 必须存在 —— 写第二遍时旧档被备份出来（`file_bak_created` 的磁盘侧证据）
    check("B7 .bak 已生成", os.path.isfile(BAK_PATH), BAK_PATH)


def main():
    if not os.path.isfile(GAME_EXE):
        print("找不到 %s" % GAME_EXE)
        print("结果: FAIL: 可执行文件不存在（先 ./make.cmd build）")
        return 1

    print("== G17-1 文件层 + G17-2 应用层 ==")
    group_selftest()
    group_on_disk()

    bad = [r for r in results if not r[1]]
    print("-" * 66)
    for name, ok, detail in results:
        print("%s  %s" % ("PASS" if ok else "FAIL", name))
    if bad:
        print("结果: FAIL: %d 项未通过" % len(bad))
        return 1
    print("结果: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
