#!/usr/bin/env python3
# ============================================================
# verify_data_tables —— 配表（C1）+ 本地化编辑（C2）
#
# ---------------------------- 这一刀在验什么 ----------------------------
# 2026-11-02 之前：
#   · 角色属性 / 技能 / 掉落 / 文本表**只能塞脚本常量**，数值一改就重编；
#   · 译文只有 `engine/i18n/ZhTable.cpp` 一张**编译进去**的静态表，
#     策划改一句翻译 = 改 C++ + 重编译。
#
# 这一刀验的是"**文件**真的成了权威"，不是"有个面板"：
#
#   A. 配表内核（`--selftest project` 里的 TABLE 组，18 条）
#      CSV 的边角：引号里的逗号 / 换行 / 转义引号 / BOM / 短行补齐 /
#      未闭合引号报行号 / 往返一致 / 按 id 找行。
#      ⚠ 引号里的逗号这条最贵：写错的话一句带逗号的台词会被从中间劈开，
#        界面上只表现为"多了一列"，不会报错。
#   B. 本地化内核（`--selftest locale`，18 条）
#      只写覆盖 / 撤销回到内置值 / 新增键持久化。
#      ⚠ `L3c only-overrides-written` 是关键：把整张静态表复制进 CSV
#        会让"静态表以后再改"永远被旧 CSV 掩盖。
#   C. 配表面板 + 命令：改一格 → 存 → **磁盘上的 .csv 真的变了**
#   D. 本地化面板 + 命令：找键 → 改译文 → 存 → **磁盘上的 zh.csv 真的有了**
#      再 reload 一次，改动还在（证明读的是文件而不是内存）。
#
# ⚠ 这一条会改 `build/bin/assets/data/*.csv` 与 `assets/i18n/zh.csv`，
#   全程快照 + finally 还原。
#
# 用法：python tools/verify_data_tables.py [--only ABCD]
# 结尾一定是 `结果: PASS` / `结果: FAIL: ...`。
# ============================================================

import argparse
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import find_window, user32                    # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors, wait_rect  # noqa: E402
from verify_asset_drag import force_foreground                    # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
EDITOR = os.path.join(BIN, "Editor.exe")
GAME = os.path.join(BIN, "Game.exe")
DATA_DIR = os.path.join(BIN, "assets", "data")
CSV_ENEMIES = os.path.join(DATA_DIR, "enemies.csv")
I18N_DIR = os.path.join(BIN, "assets", "i18n")
CSV_LOCALE = os.path.join(I18N_DIR, "zh.csv")
SIZE = "1400x900"

fails = []


def check(name, ok, detail=""):
    print("%s  %s%s" % ("PASS" if ok else "FAIL", name,
                        ("  (%s)" % detail) if detail else ""))
    if not ok:
        fails.append(name)


def snap_file(path):
    """返回 (exists, bytes) 快照。"""
    if not os.path.isfile(path):
        return (False, b"")
    with open(path, "rb") as f:
        return (True, f.read())


def restore_file(path, snap):
    exists, data = snap
    if not exists:
        if os.path.isfile(path):
            os.remove(path)
        return
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def read_text(path):
    """按字节读 + 宽松解码。

    ⚠ 不能直接 `open(encoding="utf-8")`：编辑器写盘的内容来自
    `MYVK_EDITOR_CONSOLE_CMD`，而 Windows 的环境变量走**ANSI 代码页**，
    非 ASCII 字符到进程里就已经不是 UTF-8 了 —— 直接按 UTF-8 解码会抛
    `UnicodeDecodeError`，函数返回 None，看起来像"文件不存在"（这条坑
    在第一次跑 D 组时确实把两条判据一起染红了）。
    """
    try:
        with open(path, "rb") as f:
            return f.read().decode("utf-8", errors="replace")
    except Exception:                                          # noqa: BLE001
        return None


# ---------------------------------------------------------------- 运行

def run_game_selftest(name, timeout=120.0):
    proc = subprocess.run([GAME, "--selftest", name], cwd=BIN,
                          capture_output=True, text=True, encoding="utf-8",
                          errors="replace", timeout=timeout)
    return (proc.stdout or "") + (proc.stderr or "")


def base_env(**extra):
    env = {k: v for k, v in os.environ.items() if not k.startswith("MYVK_")}
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_EDITOR_NO_HUD"] = "1"
    env["MYVK_LOG_RECTS"] = "1"
    env.update({k: str(v) for k, v in extra.items()})
    return env


def snap(tap):
    with tap.lock:
        return list(tap.lines)


def close(proc):
    try:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                proc.kill()
    except Exception:                                          # noqa: BLE001
        pass


def run_editor(panel, console_cmd, wait_tag, cmd_frame=40, timeout=70.0,
               wait_for=None, wait_secs=25.0):
    env = base_env(MYVK_EDITOR_PANELS=panel)
    if console_cmd:
        env["MYVK_EDITOR_CONSOLE_CMD"] = console_cmd
        env["MYVK_EDITOR_CONSOLE_CMD_FRAME"] = str(cmd_frame)
    proc = subprocess.Popen([EDITOR], cwd=BIN, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    hwnd = find_window("Venn Editor", timeout=timeout)
    if not hwnd:
        close(proc)
        return proc, None, tap
    user32.ShowWindow(hwnd, 9)
    time.sleep(0.8)
    tw, th = (int(v) for v in SIZE.split("x"))
    user32.SetWindowPos(hwnd, 0, 0, 0, tw, th, 0x0002 | 0x0004)
    force_foreground(hwnd)
    deadline = time.time() + 40.0
    while time.time() < deadline:
        if wait_tag is None or any(wait_tag in ln for ln in snap(tap)):
            break
        time.sleep(0.2)
    # ⚠ 不能只靠固定 sleep：控制台命令在第 `cmd_frame` 帧才执行，而首帧
    #   建面板/建布局要几秒，机器一忙就跑过了我们等的那一小段（第一次跑
    #   D 组就在这里偶发"计数没打出来"）。等**具体那行日志**出现才稳。
    if wait_for:
        deadline = time.time() + wait_secs
        while time.time() < deadline:
            if any(wait_for in ln for ln in snap(tap)):
                break
            time.sleep(0.2)
    time.sleep(0.6)
    return proc, hwnd, tap


# ---------------------------------------------------------------- A

def group_a():
    print("\n[A] 配表内核（CSV 的边角）")
    out = run_game_selftest("project")
    m = re.search(r"TABLE-TEST SUMMARY pass=(\d+) fail=(\d+)", out)
    check("[A] 打出 TABLE-TEST SUMMARY", m is not None, "")
    if not m:
        return
    p, f = int(m.group(1)), int(m.group(2))
    check("[A] 18 条全过", p >= 18 and f == 0, "pass=%d fail=%d" % (p, f))

    names = re.findall(r"TABLE-TEST (?:PASS|FAIL) (.+)", out)
    need = ["G1 parse-quoted", "G1d comma-in-quotes", "G1e escaped-quote",
            "G1f newline-in-quotes", "G3 bom-stripped",
            "G3b trailing-blank-skipped", "G4 unterminated-quote-detected",
            "G5b roundtrip-identical", "G6 row-by-id", "G6f empty-cell-is-empty"]
    missing = [n for n in need if n not in names]
    check("[A] 关键用例都在", not missing, "缺 %s" % missing)
    total = re.search(r"PROJECT-ALL-TOTAL failures=(\d+)", out)
    check("[A] 工程自检总计 0 失败", total is not None and total.group(1) == "0",
          total.group(0) if total else "no total line")


# ---------------------------------------------------------------- B

def group_b():
    print("\n[B] 本地化内核（只写覆盖 / 撤销 / 新增键）")
    out = run_game_selftest("locale")
    m = re.search(r"LOCALE-TEST SUMMARY pass=(\d+) fail=(\d+)", out)
    check("[B] 打出 LOCALE-TEST SUMMARY", m is not None, "")
    if not m:
        return
    p, f = int(m.group(1)), int(m.group(2))
    check("[B] 18 条全过", p >= 18 and f == 0, "pass=%d fail=%d" % (p, f))
    names = re.findall(r"LOCALE-TEST (?:PASS|FAIL) (.+)", out)
    need = ["L2c tr-applied", "L3c only-overrides-written",
            "L4c tr-back-to-builtin", "L5d one-extra-row",
            "L5e new-key-persisted"]
    missing = [n for n in need if n not in names]
    check("[B] 关键用例都在", not missing, "缺 %s" % missing)


# ---------------------------------------------------------------- C

def group_c():
    print("\n[C] 配表面板：改一格 → 存 → 磁盘真的变了")
    snap_csv = snap_file(CSV_ENEMIES)
    try:
        # ---- 第一次：列 + 选 + 读 ----
        proc, hwnd, tap = run_editor(
            "datatable",
            "table.list; table.select enemies; table.cell 0 0",
            "DT-RECT", wait_for="TABLE-CELL")
        check("[C] 编辑器窗口起来了", bool(hwnd))
        if not hwnd:
            close(proc)
            return
        for tag in ("DT-RECT", "DT-PATH", "DT-SELECT", "DT-RELOAD", "DT-SAVE",
                    "DT-GRID"):
            r = wait_rect(tap, tag, timeout=10.0)
            check("[C] 锚点 %s" % tag, r is not None, "%s" % (r,))
        lines = snap(tap)
        close(proc)

        joined = "\n".join(lines)
        check("[C] 两张示例表都被扫到",
              "TABLE-LIST name=enemies" in joined and
              "TABLE-LIST name=items" in joined,
              [l for l in lines if "TABLE-LIST" in l][:4])
        check("[C] 选表成功", "TABLE-CMD select name=enemies ok=1" in joined,
              [l for l in lines if "TABLE-CMD select" in l])
        check("[C] 读到第 0 行第 0 列 = slime",
              "TABLE-CELL row=0 col=0 value=slime" in joined,
              [l for l in lines if "TABLE-CELL" in l])

        # ---- 第二次：改 + 存 ----
        proc, hwnd, tap = run_editor(
            "datatable",
            "table.select enemies; table.set 0 3 99; table.save",
            "DT-RECT", wait_for="TABLE-CMD save")
        close(proc)
        joined = "\n".join(snap(tap))
        check("[C] 命令改值成功", "TABLE-CMD set row=0 col=3 ok=1" in joined,
              [l for l in snap(tap) if "TABLE-CMD set" in l])
        check("[C] 命令存盘成功", "TABLE-CMD save ok=1" in joined,
              [l for l in snap(tap) if "TABLE-CMD save" in l])

        text = read_text(CSV_ENEMIES)
        check("[C] 磁盘上的 enemies.csv 真的改了", text is not None and
              "slime" in text and ",99," in text,
              (text or "").replace("\r\n", " | ")[:120])
        # ⚠ 其余行一个字都不许动 —— 整表重写（丢注释列 / 丢引号）会在这里红
        check("[C] 其它行原样保留（没有整表重写）",
              text is not None and "Big Boss" in text and "starter" in text,
              (text or "")[:120])
    finally:
        restore_file(CSV_ENEMIES, snap_csv)


# ---------------------------------------------------------------- D

def group_d():
    print("\n[D] 本地化面板：找键 → 改译文 → 存 → 磁盘真的有了")
    os.makedirs(I18N_DIR, exist_ok=True)
    snap_csv = snap_file(CSV_LOCALE)
    try:
        # ---- 第一次：面板锚点 + 计数 + 找键 ----
        proc, hwnd, tap = run_editor(
            "localization", "i18n.count; i18n.find Save", "LOC-RECT",
            wait_for="I18N-FIND-TOTAL")
        check("[D] 编辑器窗口起来了", bool(hwnd))
        if not hwnd:
            close(proc)
            return
        for tag in ("LOC-RECT", "LOC-PATH", "LOC-FILTER", "LOC-ONLYMISS",
                    "LOC-SAVE", "LOC-RELOAD", "LOC-STATUS", "LOC-LIST"):
            r = wait_rect(tap, tag, timeout=10.0)
            check("[D] 锚点 %s" % tag, r is not None, "%s" % (r,))
        lines = snap(tap)
        close(proc)

        joined = "\n".join(lines)
        mcnt = re.search(r"I18N-UI count=(\d+) missing=(\d+) dirty=(\d+)", joined)
        check("[D] 打出 i18n.count 三个计数", mcnt is not None,
              mcnt.group(0) if mcnt else "")
        if mcnt:
            check("[D] 键数与静态表同量级（> 2000）",
                  int(mcnt.group(1)) > 2000, mcnt.group(1))
            check("[D] 初始 dirty=0（没改过）", mcnt.group(3) == "0",
                  mcnt.group(3))

        # 精确命中那一条（exact=1）才是我们要改的键
        exact = [ln for ln in lines if "I18N-FIND" in ln and "exact=1" in ln]
        check("[D] i18n.find 找到精确键 'Save'", bool(exact),
              exact[:2])
        if not exact:
            return
        idx = int(re.search(r"idx=(\d+)", exact[0]).group(1))
        print("    用的键下标 = %d" % idx)

        # ---- 第二次：改 + 存 ----
        proc, hwnd, tap = run_editor(
            "localization",
            # ⚠ 用 **纯 ASCII** 的测试值：面板里手输的中文走 ImGui 的
            #   UTF-8，没有问题；但自动化只能经环境变量传参，而 Windows
            #   的环境变量是 ANSI 代码页 —— 中文在里面就已经乱了。这是
            #   自动化钩子的限制，不是本地化功能的限制，所以判据不该
            #   拿它当被测对象（那会让"中文能不能存"看起来像坏了）。
            "i18n.set %d ZZZ_TEST_VALUE; i18n.row %d; i18n.save" % (idx, idx),
            "LOC-RECT", wait_for="I18N-CMD save")
        close(proc)
        lines2 = snap(tap)
        joined2 = "\n".join(lines2)
        check("[D] 改译文成功（没有 set failed）",
              "set failed" not in joined2,
              [l for l in lines2 if "I18N-UI set" in l][:1])
        check("[D] 存盘 ok=1", "I18N-CMD save ok=1" in joined2,
              [l for l in lines2 if "I18N-CMD save" in l])
        check("[D] row 读回新译文且 dirty=1",
              "dirty=1" in joined2 and "ZZZ_TEST_VALUE" in joined2 and
              "builtin=1" in joined2,
              [l for l in lines2 if "I18N-ROW" in l][:1])

        text = read_text(CSV_LOCALE)
        check("[D] 磁盘上出现 assets/i18n/zh.csv", text is not None,
              "" if text is not None else "文件不存在")
        if text is not None:
            check("[D] 文件里有新译文", "ZZZ_TEST_VALUE" in text, text[:160])
            nrows = len([l for l in text.replace("\r\n", "\n").split("\n")
                         if l.strip()])
            # 表头 + 1 条覆盖（内置的 2900 多条**一条都不许进来**）
            check("[D] 只写覆盖行（表头 + 1 行）", nrows == 2,
                  "数据行数 = %d" % nrows)

        # ---- 第三次：reload 之后改动还在（读的是文件不是内存）----
        proc, hwnd, tap = run_editor("localization", "i18n.count", "LOC-RECT",
                                     wait_for="I18N-UI count")
        close(proc)
        joined3 = "\n".join(snap(tap))
        m3 = re.search(r"I18N-UI count=(\d+) missing=(\d+) dirty=(\d+)", joined3)
        check("[D] 重新打开后 dirty=1（从文件读回来）",
              m3 is not None and m3.group(3) == "1",
              m3.group(0) if m3 else "")
    finally:
        restore_file(CSV_LOCALE, snap_csv)


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="ABCD")
    args = ap.parse_args()
    only = args.only.upper()

    print("=" * 74)
    print("配表（C1）+ 本地化编辑（C2）")
    print("=" * 74)

    kill_stale_editors()
    if "A" in only:
        group_a()
    if "B" in only:
        group_b()
    if "C" in only:
        group_c()
    if "D" in only:
        group_d()

    print("\n" + "=" * 74)
    if fails:
        print("结果: FAIL: %d 项 —— %s" % (len(fails), fails[:6]))
        return 1
    print("结果: PASS（CSV 边角 + 只写覆盖 + 配表改存落盘 + 本地化改存落盘"
          " + reload 读回文件）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
