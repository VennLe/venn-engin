"""端到端验证：**出厂界面语言 = 当前电脑的系统语言**（2026-10-29）。

用户原文：
    "引擎的语言默认是设置为当前电脑系统的语言"

改之前：`i18n` 里 `g_lang` 初值写死 `English`，`RuntimeConfig::language`
默认 `"en"` —— 中文 Windows 上编辑器 / 启动器 / 游戏第一次打开全是英文，
必须自己去偏好设置里点一次。这不是"默认值保守"，是默认值不对。

改之后的三档优先级（高 → 低，整条链同一个答案，见 `engine/i18n/I18n.h` 文件头）：

    1) 命令行 / 环境变量   --language / MYVK_LANG / MYVK_EDITOR_LANG
    2) 落盘的用户选择      editor_prefs.json 的 language / game.json 的 language
    3) **系统界面语言**    ← `language()` 的初值（Windows: GetUserDefaultUILanguage）

怎么判（全部落在日志上，不数像素）：

    A 组（运行时，`Game.exe`）
      A1 独立 oracle：Python 侧的 `GetUserDefaultUILanguage()` 得到的语言，
         必须和引擎 A2 里报的一致 —— **不当裁判自己是原告**：要是两边都写
         成同一个错的映射（比如把 LANG_CHINESE 认成日文），只查一侧看不出来。
      A2 `--language auto`（= 空 = 缺省那档）→ `I18N-KEY lang=<系统> src=config`
      A3 `--language en` 在中文机器上 → `lang=en count=0`（显式覆盖系统）
      A4 `MYVK_LANG` 压过命令行 → `src=env:MYVK_LANG`
    B 组（编辑器，`Editor.exe`，看 `EDITOR-LANG lang=… src=…`）
      B1 自动化跑（MYVK_NO_WINDOW_SAVE）→ `lang=en src=automation`
         （判据要确定性文本：中文 Windows 上的回归不该因为系统语言换词）
      B2 **交互式**跑（不设自动化变量）→ `lang=<系统> src=system`
      B3 存档里写了 `language=1` → `lang=zh src=prefs`（用户选择压过出厂）
      B4 `MYVK_EDITOR_LANG=en` → `src=env`（验收钩子仍然最高）
    C 组（启动器）
      C1 `LAUNCHER-LANG lang=<系统> src=system`（启动器没有偏好存储，纯出厂）

⚠ `--language auto` 是"缺省那档"的**等价代理**：`RuntimeConfig::language`
  默认空串，空 / `auto` / `system` 三个写法在 `RuntimeApp::onInit` 里走
  同一条分支（`configured.empty() || auto || system`）。之所以不造一份
  "没有 language 键"的 game.json：那要连 assets/shaders 一起复制一份
  build/bin，代价远大于它多证明的那一行。

用法：
    python tools/verify_system_lang.py
    python tools/verify_system_lang.py --bin build/bin
"""

import argparse
import ctypes
import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import find_window, user32                        # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors               # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify")
TMP_DIR = os.path.join(OUT_DIR, "_tmp_lang")

I18N_RE = re.compile(r"I18N-KEY\s+lang=(\w+)\s+count=(\d+)\s+src=(\S+)")
EDITOR_LANG_RE = re.compile(r"EDITOR-LANG\s+lang=(\w+)\s+src=(\S+)")
LAUNCHER_LANG_RE = re.compile(r"LAUNCHER-LANG\s+lang=(\w+)\s+src=(\S+)")

# LANG_CHINESE（winnt.h）。取 PRIMARYLANGID 的这 10 位。
LANG_CHINESE = 0x04


def os_ui_language_code():
    """**独立**取一次系统界面语言（不经过引擎任何代码）。

    引擎里那份是 `PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE`；
    这里用同一对常量但**独立实现**，两边对不上就是有一边写错了。
    """
    if sys.platform == "win32":
        u = ctypes.windll.kernel32.GetUserDefaultUILanguage()
        return "zh" if (u & 0x3FF) == LANG_CHINESE else "en"
    for k in ("LC_ALL", "LC_MESSAGES", "LANG"):        # POSIX 惯例
        v = os.environ.get(k, "")
        if v.lower().startswith("zh"):
            return "zh"
        if v:
            return "en"
    return "en"


def run_logged(exe, args, env_extra, marker, timeout=30.0, cwd=None):
    """起进程 → 抓 stdout 到出现 marker（正则）→ 收尾 → 返回 (match, lines)。"""
    env = dict(os.environ)
    env.update(env_extra)
    proc = subprocess.Popen([exe] + args, cwd=cwd or os.path.dirname(exe),
                            env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True,
                            encoding="utf-8", errors="replace", bufsize=1)
    tap = LogTap()
    tap.start(proc)
    found = None
    end = time.time() + timeout
    while time.time() < end:
        with tap.lock:
            for line in tap.lines:
                m = marker.search(line)
                if m:
                    found = m
                    break
        if found:
            break
        if proc.poll() is not None:
            break
        time.sleep(0.1)
    # 收尾：先礼后兵。窗口还在就 WM_CLOSE，让 onShutdown 走完（否则日志尾巴丢，
    # 也拿不到"退出时落盘"的证据 —— B2b 就靠它）。等不到自己退才 terminate。
    hwnd = find_window("Venn Editor")
    if hwnd:
        user32.PostMessageW(hwnd, 0x0010, 0, 0)      # WM_CLOSE
        for _ in range(80):                          # 最多等 8s 干净退出
            if proc.poll() is not None:
                break
            time.sleep(0.1)
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
    with tap.lock:
        lines = list(tap.lines)
    return found, lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=os.path.join(ROOT, "build", "bin"))
    args = ap.parse_args()

    exe_dir = os.path.abspath(args.bin)
    game = os.path.join(exe_dir, "Game.exe")
    editor = os.path.join(exe_dir, "Editor.exe")
    launcher = os.path.join(exe_dir, "Launcher.exe")
    for p in (game, editor, launcher):
        if not os.path.exists(p):
            print("找不到可执行文件:", p)
            return 2

    os.makedirs(TMP_DIR, exist_ok=True)
    kill_stale_editors()

    fails = []

    def check(name, ok, detail=""):
        print("  [%s] %s %s" % ("PASS" if ok else "FAIL", name, detail))
        if not ok:
            fails.append(name)

    sys_code = os_ui_language_code()
    print("  本机系统界面语言（独立 oracle）= %s" % sys_code)

    # ================= A 运行时 =================
    print("-- A 运行时（Game.exe）")
    m, _ = run_logged(game, ["--frames", "2", "--language", "auto"], {},
                      I18N_RE, timeout=60.0)
    if not m:
        check("A2 --language auto → 跟随系统语言", False, "没拿到 I18N-KEY")
    else:
        check("A2 --language auto → 跟随系统语言（值来自系统）",
              m.group(1) == sys_code and m.group(3) == "config",
              "lang=%s count=%s src=%s（期望 lang=%s）"
              % (m.group(1), m.group(2), m.group(3), sys_code))
        # 中文档下 count 必须 > 0（表建起来了）；这条同时是"确实换到中文"的
        # 第二证据 —— 只看 lang=zh 有可能只是字符串对，表其实没构造。
        if sys_code == "zh":
            check("A2b 中文档下词典真建起来了（count > 0）",
                  int(m.group(2)) > 0, "count=%s" % m.group(2))

    m, _ = run_logged(game, ["--frames", "2", "--language", "en"], {},
                      I18N_RE, timeout=60.0)
    if not m:
        check("A3 --language en 显式覆盖系统语言", False, "没拿到 I18N-KEY")
    else:
        check("A3 --language en 显式覆盖系统语言（且英文不建表）",
              m.group(1) == "en" and int(m.group(2)) == 0,
              "lang=%s count=%s" % (m.group(1), m.group(2)))

    m, _ = run_logged(game, ["--frames", "2", "--language", "en"],
                      {"MYVK_LANG": "zh"}, I18N_RE, timeout=60.0)
    if not m:
        check("A4 MYVK_LANG 压过命令行", False, "没拿到 I18N-KEY")
    else:
        check("A4 MYVK_LANG 压过命令行（优先级最高的一档）",
              m.group(1) == "zh" and m.group(3) == "env:MYVK_LANG",
              "lang=%s src=%s" % (m.group(1), m.group(3)))

    # ================= B 编辑器 =================
    print("-- B 编辑器（Editor.exe）")
    prefs = os.path.join(TMP_DIR, "b1_none.json")
    if os.path.exists(prefs):
        os.remove(prefs)
    m, _ = run_logged(editor, [],
                      {"MYVK_NO_WINDOW_SAVE": "1",
                       "MYVK_EDITOR_PREFS_PATH": prefs},
                      EDITOR_LANG_RE, timeout=90.0)
    if not m:
        check("B1 自动化跑默认英文", False, "没拿到 EDITOR-LANG")
    else:
        check("B1 自动化跑（MYVK_NO_WINDOW_SAVE）默认英文 = 判据可复现",
              m.group(1) == "en" and m.group(2) == "automation",
              "lang=%s src=%s" % (m.group(1), m.group(2)))

    # B2 交互式：**不设** MYVK_FRAMES / MYVK_NO_WINDOW_SAVE（这两个就是
    # `editorLayoutAutomationRun()` 的判定）。布局与偏好都指到临时路径，
    # 免得把用户自己的编辑器状态改掉。
    prefs = os.path.join(TMP_DIR, "b2_none.json")
    layout = os.path.join(TMP_DIR, "b2_none.layout.json")
    for p in (prefs, layout):
        if os.path.exists(p):
            os.remove(p)
    m, _ = run_logged(editor, [],
                      {"MYVK_EDITOR_PREFS_PATH": prefs,
                       "MYVK_EDITOR_LAYOUT": layout},
                      EDITOR_LANG_RE, timeout=90.0)
    if not m:
        check("B2 交互式跑跟随系统语言", False, "没拿到 EDITOR-LANG")
    else:
        check("B2 交互式跑（无存档）= 系统语言",
              m.group(1) == sys_code and m.group(2) == "system",
              "lang=%s src=%s（期望 lang=%s）"
              % (m.group(1), m.group(2), sys_code))
        # B2b：「跟随系统」不能被**退出时自动落盘**悄悄钉死。
        # 用户没碰过下拉框，prefs 里就不该出现 language 键 —— 否则他下次
        # 改了系统语言，编辑器还是老语言，而"是谁把它钉住的"完全无从查起。
        keys = {}
        if os.path.exists(prefs):
            try:
                with open(prefs, encoding="utf-8") as f:
                    keys = json.load(f).get("prefs", {})
            except (OSError, ValueError):
                keys = {}
        check("B2b 没碰过下拉框 → prefs 里没有 language 键（不钉死「跟随系统」）",
              "language" not in keys,
              "prefs=%s" % (sorted(keys.keys())[:6] if keys else "（未生成）"))

    # B3 存档里的选择压过出厂（自动化跑本来会得到 en，存档写 1 = 中文）
    prefs = os.path.join(TMP_DIR, "b3_zh.json")
    with open(prefs, "w", encoding="utf-8") as f:
        json.dump({"version": 2, "prefs": {"language": 1.0}}, f, indent=2)
    m, _ = run_logged(editor, [],
                      {"MYVK_NO_WINDOW_SAVE": "1",
                       "MYVK_EDITOR_PREFS_PATH": prefs},
                      EDITOR_LANG_RE, timeout=90.0)
    if not m:
        check("B3 存档里的 language 压过出厂默认", False, "没拿到 EDITOR-LANG")
    else:
        check("B3 存档里的 language 压过出厂默认（用户选择 > 系统语言）",
              m.group(1) == "zh" and m.group(2) == "prefs",
              "lang=%s src=%s" % (m.group(1), m.group(2)))

    # B4 验收钩子仍然最高
    prefs = os.path.join(TMP_DIR, "b4_none.json")
    if os.path.exists(prefs):
        os.remove(prefs)
    m, _ = run_logged(editor, [],
                      {"MYVK_NO_WINDOW_SAVE": "1",
                       "MYVK_EDITOR_LANG": "zh",
                       "MYVK_EDITOR_PREFS_PATH": prefs},
                      EDITOR_LANG_RE, timeout=90.0)
    if not m:
        check("B4 MYVK_EDITOR_LANG 仍然最高", False, "没拿到 EDITOR-LANG")
    else:
        check("B4 MYVK_EDITOR_LANG 仍然压过自动化 / 系统（验收钩子没被改弱）",
              m.group(1) == "zh" and m.group(2) == "env",
              "lang=%s src=%s" % (m.group(1), m.group(2)))

    # ================= C 启动器 =================
    print("-- C 启动器（Launcher.exe）")
    m, _ = run_logged(launcher, [],
                      {"MYVK_FRAMES": "3", "MYVK_NO_WINDOW_SAVE": "1"},
                      LAUNCHER_LANG_RE, timeout=90.0)
    if not m:
        check("C1 启动器出厂语言 = 系统语言", False, "没拿到 LAUNCHER-LANG")
    else:
        check("C1 启动器出厂语言 = 系统语言（它没有偏好存储，纯出厂）",
              m.group(1) == sys_code and m.group(2) == "system",
              "lang=%s src=%s" % (m.group(1), m.group(2)))

    # ================= D 静态：Reset All 的落点 =================
    # 「重置默认」必须回到**启动时会出现的那个值**。写成常数 0 的话，
    # 中文机器上 Reset All → 英文、下次冷启动 → 中文，两处对不上。
    # 这条只能静态查（Reset All 要两次点击 + 落盘才能观测），所以单列一组。
    src = os.path.join(ROOT, "editor", "EditorApp.cpp")
    with open(src, encoding="utf-8") as f:
        text = f.read()
    m_lang_def = re.search(
        r'\{"language",\s*"Language",\s*"Appearance",\s*false,\s*([^,]+),',
        text)
    check("D1 偏好表里 language 的 def 不是常数 0（= 跟随系统 / factoryLanguage）",
          bool(m_lang_def) and "factoryLanguage" in m_lang_def.group(1),
          (m_lang_def.group(1).strip() if m_lang_def else "没找到那条 PrefItem"))

    if fails:
        print("结果: FAIL: " + ", ".join(fails))
        return 1
    print("结果: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
