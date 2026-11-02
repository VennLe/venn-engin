#!/usr/bin/env python3
# ============================================================
# verify_project_settings —— 项目设置面板（`assets/game.json` → UI，B3）
#
# ---------------------------- 这一刀在验什么 ----------------------------
# 2026-11-02 之前，`assets/game.json`（标题 / 分辨率 / 全屏 / 帧率上限 /
# 语言 / 默认场景 / UI 文档 / 音频出厂值）**只能手改**：
#
#     grep -rn "game.json" editor/   →  只有一句 tooltip 提到它
#
# 而 `Edit ▸ Project Settings...` 打开的是**画质档位**那张面板。所以这一刀
# 验的不是 JSON 解析，而是**入口 + 不漂移**：
#
#   A. 读写层自检（无窗口，毫秒级）
#      往返 / **未知键保留** / 坏 JSON 拒绝覆盖 / 夹取 / 语言三档。
#      其中"未知键保留"是全篇最贵的一条：整份重写会把 `player` 块清掉，
#      而那种丢字段**不会报错**。
#   B. 面板真的画出来了（锚点 + 22 个字段全部读回）
#   C. 命令改 → 存 → **reload 之后读回**（值来自磁盘，不是内存）
#   D. **点 Save 按钮真的能存**（"界面点不到 = 没交付"）
#      —— 命令只负责改值，落盘由那一颗按钮完成：证明两条路是同一个落盘点。
#   E. 面板写的值 **Game.exe 真的读到**（端到端）
#      —— 这是"两份配置漂移"的唯一判据。抽共享结构体管不住这个
#         （抽完之后键名仍然可以各写一个字符串）。
#   F. 坏 game.json **不会被面板覆盖**（端到端版的 G4）
#
# ⚠ 这一条会**改 build/bin/assets/game.json**，所以全程快照 + finally 还原。
#   不还原的话，后面任何一条读 game.json 的用例都会看到 "ProjTest"。
#
# 用法：python tools/verify_project_settings.py [--only ABCDEF]
# 结尾一定是 `结果: PASS` / `结果: FAIL: ...`（run_regression 取**最后一条**）。
# ============================================================

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import find_window, user32                    # noqa: E402
from verify_add_menu import LogTap, click, kill_stale_editors, wait_rect  # noqa: E402
from verify_asset_drag import force_foreground                    # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
EDITOR = os.path.join(BIN, "Editor.exe")
GAME = os.path.join(BIN, "Game.exe")
GAME_FILE = os.path.join(BIN, "assets", "game.json")
SCENE_DIR = os.path.join(BIN, "assets", "scenes")
SCENE = "scenes/proj_settings_fixture.json"
SIZE = "1400x900"

fails = []


def check(name, ok, detail=""):
    print("%s  %s%s" % ("PASS" if ok else "FAIL", name,
                        ("  (%s)" % detail) if detail else ""))
    if not ok:
        fails.append(name)


# ---------------------------------------------------------------- 夹具

def write_scene():
    """最小场景：编辑器要有个场景才起得来（本面板不读场景内容）。"""
    os.makedirs(SCENE_DIR, exist_ok=True)
    scene = {
        "version": 2,
        "generator": "verify_project_settings.py",
        "camera": {"target": [0.0, 0.0, 1.0], "yaw": 0.610865,
                   "pitch": 0.314159, "distance": 8.0, "fov": 50.0},
        "light": {"direction": [0.42, 0.72, -0.55], "color": [1.0, 0.96, 0.9],
                  "intensity": 3.2, "castsShadow": True, "ambientScale": 1.0},
        "physics": {"enabled": False},
        "entities": [],
    }
    with open(os.path.join(SCENE_DIR, os.path.basename(SCENE)), "w",
              encoding="utf-8") as f:
        json.dump(scene, f, ensure_ascii=False, indent=1)


def read_game_file():
    try:
        with open(GAME_FILE, encoding="utf-8") as f:
            return json.load(f)
    except Exception:                                          # noqa: BLE001
        return None


def read_game_text():
    try:
        with open(GAME_FILE, encoding="utf-8") as f:
            return f.read()
    except Exception:                                          # noqa: BLE001
        return None


# ---------------------------------------------------------------- 运行

def base_env(**extra):
    env = {k: v for k, v in os.environ.items() if not k.startswith("MYVK_")}
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_EDITOR_NO_HUD"] = "1"
    # ⚠ 面板锚点靠 `ui::logRect`，它只在 MYVK_LOG_RECTS=1 时打日志
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_EDITOR_SCENE"] = SCENE
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


def launch(console_cmd=None, panels="projectsettings", cmd_frame=90,
           wait_tag="PROJ-RECT", timeout=60.0):
    env = base_env(MYVK_EDITOR_PANELS=panels)
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
    time.sleep(1.0)
    tw, th = (int(v) for v in SIZE.split("x"))
    user32.SetWindowPos(hwnd, 0, 0, 0, tw, th, 0x0002 | 0x0004)
    force_foreground(hwnd)

    deadline = time.time() + 45.0
    while time.time() < deadline:
        if wait_tag is None or any(wait_tag in ln for ln in snap(tap)):
            break
        time.sleep(0.2)
    time.sleep(3.0)
    return proc, hwnd, tap


def run_once(console_cmd, **kw):
    proc, _hwnd, tap = launch(console_cmd, **kw)
    lines = snap(tap)
    close(proc)
    return lines


def grep(regex, lines):
    out = []
    for ln in lines:
        m = regex.search(ln)
        if m:
            out.append(m)
    return out


def field_value(lines, key):
    """从 `PROJ-SETTINGS key=<k> value=<v>` 里取最后一个值。"""
    rx = re.compile(r"PROJ-SETTINGS key=%s value=(.*)$" % re.escape(key))
    got = grep(rx, lines)
    return got[-1].group(1).strip() if got else None


# ---------------------------------------------------------------- 正则

SUMMARY_RE = re.compile(r"PROJ-GAME-TEST SUMMARY pass=(\d+) fail=(\d+)")
ALLTOTAL_RE = re.compile(r"PROJECT-ALL-TOTAL failures=(\d+)")
# 用例名里**有空格**（"G3c comment-preserved"）→ 必须吃到行尾，
# 用 `\S+` 只会拿到 "G3c"，然后"关键用例都在"永远红。
TEST_RE = re.compile(r"PROJ-TEST (PASS|FAIL) (.+?)\s*$")
LOAD_RE = re.compile(r"PROJ-GAME-LOAD path=(\S+) ok=(\d+) created=(\d+)")
SAVE_RE = re.compile(r"PROJ-GAME-SAVE path=(\S+) ok=(\d+)")
UISAVE_RE = re.compile(r"PROJ-UI save path=(\S+) bytes=(\d+)")
CMDSET_RE = re.compile(r"PROJ-CMD set key=(\S+) value=(.*)$")
CMDSAVE_RE = re.compile(r"PROJ-CMD save ok=(\d+)")
RUNTIME_CFG_RE = re.compile(
    r"GAME-CONFIG scene='([^']*)' window=(\d+)x(\d+) fullscreen=(\d) "
    r"maxFps=(\d+)")

# 关键自检用例（删掉任意一条 = 这一块被削了）
REQUIRED = [
    "G1 load-missing-uses-defaults",
    "G2b reload-ok", "G2c roundtrip-fields", "G2d empty-language-written",
    "G3 load-with-unknown-keys", "G3c comment-preserved",
    "G3d player-block-preserved", "G3e our-key-overwritten",
    "G4 load-bad-json-fails", "G4b save-bad-json-refused",
    "G4c bad-file-untouched",
    "G5 clamp-count", "G5g clamp-leaves-legal-alone",
    "G6c lang-system-normalized", "G7 load-clamps-width",
]


# ============================================================
# A. 读写层自检（无窗口）
# ============================================================

def case_a():
    print("\n[A] 无窗口自检：Game.exe --selftest project")
    env = base_env()
    p = subprocess.run([GAME, "--selftest", "project"], cwd=BIN, env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, encoding="utf-8", errors="replace",
                       timeout=180)
    lines = (p.stdout or "").splitlines()
    check("[A] 退出码 0", p.returncode == 0, "rc=%d" % p.returncode)

    sm = grep(SUMMARY_RE, lines)
    check("[A] 打出 PROJ-GAME-TEST SUMMARY", bool(sm),
          sm[-1].group(0) if sm else "没等到")
    if sm:
        passed, failed = int(sm[-1].group(1)), int(sm[-1].group(2))
        check("[A] fail=0", failed == 0, "fail=%d" % failed)
        # 下限而不是精确值：新增断言不该让脚本变红
        check("[A] pass >= 29", passed >= 29, "pass=%d" % passed)

    at = grep(ALLTOTAL_RE, lines)
    check("[A] PROJECT-ALL-TOTAL failures=0（没把别的组带红）",
          bool(at) and at[-1].group(1) == "0",
          at[-1].group(0) if at else "没等到")

    seen = set(m.group(2).strip() for m in grep(TEST_RE, lines))
    bad = [m.group(2).strip() for m in grep(TEST_RE, lines)
           if m.group(1) == "FAIL"]
    check("[A] 没有 FAIL 用例", not bad, str(bad))
    missing = [n for n in REQUIRED if n not in seen]
    check("[A] 关键用例都在（缺 %d）" % len(missing), not missing,
          str(missing))


# ============================================================
# B. 面板画出来了 + 22 个字段读回
# ============================================================

def case_b():
    print("\n[B] 面板锚点 + project.settings 读回")
    lines = run_once("project.settings")

    load = grep(LOAD_RE, lines)
    check("[B] 面板打开时读了 game.json（PROJ-GAME-LOAD ok=1）",
          bool(load) and load[-1].group(2) == "1",
          load[-1].group(0) if load else "没等到")

    check("[B] 面板窗口画出来了（PROJ-RECT）",
          any("PROJ-RECT" in ln for ln in lines))
    for tag, why in [("PROJ-PATH", "文件路径"), ("PROJ-DIRTY", "保存状态"),
                     ("PROJ-SAVE", "Save 按钮"), ("PROJ-RELOAD", "Reload 按钮"),
                     ("PROJ-TITLE", "标题"), ("PROJ-SCENE", "默认场景"),
                     ("PROJ-WIDTH", "宽度"), ("PROJ-HEIGHT", "高度"),
                     ("PROJ-DEC-WIDTH", "- 微调"), ("PROJ-INC-WIDTH", "+ 微调"),
                     ("PROJ-MAXFPS", "帧率上限"), ("PROJ-LANG", "语言"),
                     ("PROJ-QUALITY", "画质标签"),
                     ("PROJ-AUDIO-MASTER", "主音量出厂值"),
                     ("PROJ-AUDIO-MAXVOICES", "声部上限")]:
        check("[B] 锚点 %s（%s）" % (tag, why), tag in " ".join(lines))

    keys = ["title", "scene", "uiDoc", "startState", "width", "height",
            "fullscreen", "maxFps", "unfocusedFps", "timeScale", "language",
            "quality", "audio.enabled", "audio.device", "audio.master",
            "audio.music", "audio.sfx", "audio.voice", "audio.mute",
            "audio.muteOnFocusLost", "audio.duckDepth", "audio.maxVoices"]
    got = [k for k in keys if field_value(lines, k) is not None]
    check("[B] project.settings 打全了 22 个字段", len(got) == len(keys),
          "拿到 %d/%d：缺 %s" % (len(got), len(keys),
                                [k for k in keys if k not in got]))
    print("    title=%r width=%s language=%r"
          % (field_value(lines, "title"), field_value(lines, "width"),
             field_value(lines, "language")))


# ============================================================
# C. 命令改 → 存 → reload 后读回（值来自磁盘）
# ============================================================

def case_c():
    print("\n[C] project.set → save → reload → settings（值来自磁盘）")
    # ⚠ 先往文件里塞两个"面板不认识"的键：`player`（25 个字段的大块，运行时
    #   真的会读）和一个故意起的 `zzUnknownKey`。**不前置注入的话，"未知键
    #   被保留"这条断言在本仓的出厂 game.json 上恒真**（它本来就没有 player
    #   块）—— 那是假绿，而且正是"面板把玩家模型配置清空"那个事故的形状。
    before = read_game_file() or {}
    before["player"] = {"model": "characters/keepme.fbx", "fitBody": False}
    before["zzUnknownKey"] = 42
    with open(GAME_FILE, "w", encoding="utf-8") as f:
        json.dump(before, f, ensure_ascii=False, indent=2)
    cmd = ("project.set title ProjSettingsTest;"
           "project.set width 1600;"
           "project.set height 900;"
           "project.set audio.master 0.30;"
           "project.save;"
           "project.reload;"
           "project.settings")
    lines = run_once(cmd)

    sets = grep(CMDSET_RE, lines)
    check("[C] 四条 project.set 都成功", len(sets) >= 4,
          "%d 条：%s" % (len(sets), [m.group(1) for m in sets]))
    saved = grep(CMDSAVE_RE, lines)
    check("[C] project.save ok=1", bool(saved) and saved[-1].group(1) == "1",
          saved[-1].group(0) if saved else "没等到")
    check("[C] PROJ-UI save 真的写了字节",
          bool(grep(UISAVE_RE, lines)),
          grep(UISAVE_RE, lines)[-1].group(0) if grep(UISAVE_RE, lines) else "-")

    # ⚠ 读回必须在 reload **之后**：那时值来自文件，不是内存
    check("[C] reload 后 title 读回 = ProjSettingsTest",
          field_value(lines, "title") == "ProjSettingsTest",
          "got %r" % field_value(lines, "title"))
    check("[C] reload 后 width 读回 = 1600",
          field_value(lines, "width") == "1600", field_value(lines, "width"))
    check("[C] reload 后 audio.master 读回 = 0.3",
          field_value(lines, "audio.master") in ("0.3", "0.30"),
          field_value(lines, "audio.master"))

    # 文件本身
    disk = read_game_file() or {}
    check("[C] 磁盘上 title/width 也变了",
          disk.get("title") == "ProjSettingsTest" and disk.get("width") == 1600,
          "title=%r width=%r" % (disk.get("title"), disk.get("width")))
    check("[C] 数值类型仍是数字（不是被当成字符串写）",
          isinstance(disk.get("width"), int) and
          isinstance(disk.get("audio", {}).get("master"), float),
          "%s / %s" % (type(disk.get("width")).__name__,
                       type(disk.get("audio", {}).get("master")).__name__))
    # 最贵的一条：手写 / 别的工具加的键必须还在（整份重写会静默清掉它们）
    check("[C] `_comment` 保留（未知键不被覆盖）",
          "_comment" in disk, str(list(disk.keys())))
    check("[C] `player` 块原样保留（面板根本不编辑它）",
          disk.get("player", {}).get("model") == "characters/keepme.fbx",
          str(disk.get("player")))
    check("[C] 故意写的陌生键 zzUnknownKey 也保留",
          disk.get("zzUnknownKey") == 42, str(disk.get("zzUnknownKey")))
    print("    磁盘 keys=%s" % list(disk.keys()))


# ============================================================
# D. 点 Save 按钮真的能存
# ============================================================

def case_d():
    print("\n[D] 点 PROJ-SAVE 按钮真的落盘（界面点得到）")
    # 先用命令改值但**不保存**（命令等价于"在面板里改了输入框"），
    # 再由**鼠标点按钮**完成落盘 —— 这样两条路都覆盖到。
    proc, hwnd, tap = launch("project.set title BtnSaved")
    try:
        if not hwnd:
            check("[D] 编辑器窗口起来", False)
            return
        r = wait_rect(tap, "PROJ-SAVE", timeout=10.0)
        check("[D] 拿到 PROJ-SAVE 矩形", r is not None, str(r))
        if not r:
            return
        # 改值之后按钮才在"有未保存改动"状态下可用；先确认脏位已置
        dirty = [ln for ln in snap(tap) if "PROJ-UI set key=title" in ln]
        check("[D] 命令改值后置了脏位", any("dirty=1" in ln for ln in dirty),
              dirty[-1].strip() if dirty else "没等到")

        cx = (r[0] + r[2]) * 0.5
        cy = (r[1] + r[3]) * 0.5
        click(hwnd, cx, cy)
        time.sleep(1.2)

        ui = [m for m in grep(UISAVE_RE, snap(tap))]
        check("[D] 点按钮之后打出 PROJ-UI save", bool(ui),
              ui[-1].group(0) if ui else "没等到（点了没反应）")
        disk = read_game_file() or {}
        check("[D] 磁盘上 title = BtnSaved",
              disk.get("title") == "BtnSaved", repr(disk.get("title")))
        # 存完之后脏位必须落下：面板下一秒要显示"Saved"，否则用户会
        # 一直以为没存上（`PROJ-DIRTY` 那一行是唯一证据）。
        time.sleep(0.6)
        dirties = [ln for ln in snap(tap) if "PROJ-DIRTY=" in ln]
        if dirties:
            ...
    finally:
        close(proc)


# ============================================================
# E. 端到端：面板写的值 Game.exe 真的读到
# ============================================================

def case_e():
    print("\n[E] 端到端：Game.exe 读到面板写进 game.json 的值")
    # 前置：C/D 组已经把 title=BtnSaved、width=1600、height=900 写进盘了。
    disk = read_game_file() or {}
    want_w = disk.get("width")
    want_h = disk.get("height")
    check("[E] 盘上已有面板写的分辨率", want_w == 1600 and want_h == 900,
          "%sx%s" % (want_w, want_h))
    if not (want_w and want_h):
        return

    # ⚠ 必须剥掉 MYVK_RUNTIME_*（否则读到的是 env 覆盖值，不是文件）
    env = base_env()
    p = subprocess.run([GAME, "--frames", "20"], cwd=BIN, env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, encoding="utf-8", errors="replace",
                       timeout=180)
    lines = (p.stdout or "").splitlines()
    cfg = grep(RUNTIME_CFG_RE, lines)
    check("[E] 运行时打出 GAME-CONFIG", bool(cfg),
          cfg[-1].group(0) if cfg else "没等到")
    if not cfg:
        return
    w, h = int(cfg[-1].group(2)), int(cfg[-1].group(3))
    check("[E] Game.exe 的窗口 = 面板写的那一对（%dx%d）" % (want_w, want_h),
          w == want_w and h == want_h, "got=%dx%d" % (w, h))
    # 反面对照：这是"文件里的值"而不是某处默认 1280x720
    check("[E] 不是出厂默认 1280x720（证明真的读了文件）",
          not (w == 1280 and h == 720), "got=%dx%d" % (w, h))


# ============================================================
# F. 坏 game.json 不会被面板覆盖
# ============================================================

def case_f():
    print("\n[F] 坏 game.json：面板拒绝覆盖（不静默清空别人的键）")
    bad = '{ "title": "broken", oops'
    with open(GAME_FILE, "w", encoding="utf-8") as f:
        f.write(bad)
    try:
        lines = run_once("project.save")
        s = grep(SAVE_RE, lines)
        check("[F] PROJ-GAME-SAVE ok=0（拒绝写）",
              bool(s) and s[-1].group(2) == "0",
              s[-1].group(0) if s else "没等到")
        check("[F] 坏文件内容一字未变",
              read_game_text() == bad, (read_game_text() or "")[:40])
    finally:
        # 还原交给 main 的 finally（它持着原始快照）
        pass


# ============================================================

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="ABCDEF",
                    help="只跑指定组，如 --only AB（默认 ABCDEF）")
    args = ap.parse_args()
    only = args.only.upper()

    for exe in (EDITOR, GAME):
        if not os.path.isfile(exe):
            print("结果: FAIL: 找不到 %s（先 ./make.cmd build）" % exe)
            return 4
    if not os.path.isfile(GAME_FILE):
        print("结果: FAIL: 找不到 %s（先 ./make.cmd build）" % GAME_FILE)
        return 4

    kill_stale_editors()
    write_scene()

    # ⚠ 全程快照 + finally 还原：这一条会改 build/bin/assets/game.json，
    #   不还原的话后面任何一条读它的用例都会看到 "ProjTest"。
    backup = GAME_FILE + ".verify_backup"
    shutil.copy2(GAME_FILE, backup)
    try:
        if "A" in only:
            case_a()
        if "B" in only:
            case_b()
        if "C" in only:
            case_c()
        if "D" in only:
            case_d()
        if "E" in only:
            case_e()
        if "F" in only:
            case_f()
    finally:
        kill_stale_editors()
        try:
            shutil.move(backup, GAME_FILE)
        except Exception as e:                                 # noqa: BLE001
            print("!! 还原 game.json 失败：%s（备份在 %s）" % (e, backup))
        try:
            os.remove(os.path.join(SCENE_DIR, os.path.basename(SCENE)))
        except OSError:
            pass

    print()
    if fails:
        print("结果: FAIL:（%d 项）" % len(fails))
        for f in fails:
            print("   -", f)
        return 6
    print("结果: PASS（game.json 读写自检 + 面板锚点 + 命令/按钮落盘 + "
          "端到端读到 + 坏文件不覆盖）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
