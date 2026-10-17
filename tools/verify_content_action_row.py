"""端到端验证：Content 右栏顶部的"动作条"（2026-10-17 晚 用户需求 4）。

被验证的用户需求（原话）：

  「Content面板中右侧那一部分的搜索栏的上边加一些东西，分别是几个按钮，
    一个是添加按钮，按钮是图标加文字一起的，添加按钮点击后会在该按钮的
    上侧的位置打开一个子菜单，内容有：材质，模型，场景，动画，过场动画，
    媒体，水，纹理，物理，音频，植被，用户界面，世界地图，还有导入按钮，
    点击导入按钮会打开本地资源管理器选择资源进行导入，还有个保存所有的
    按钮，保存所有按钮的右侧是两个小按钮分别是去上一层目录和下一层目录，
    再右边是一个路径框，里面会显示当前项目文件资源的路径，即相对当前游戏
    项目的根目录的相对路径。」

断言分组（全部读日志锚点 / 文件系统，可机器验证）：

  A. 动作条存在且顺序正确：
     CB-ADD-BTN < CB-SAVEALL < CB-PREV < CB-NEXT < CB-PATH（从左往右），
     五个控件在**同一行**，整行在搜索框（CB-SEARCH）**上方**，
     路径框是这一行最宽的一块。初始 CB-PATH rel=/assets。
  B. Add 菜单：点开后有 13 个资源类型项（CB-ADDITEM <key>）+ import 项，
     全部排在按钮**上侧**（每个 item 的 y1 ≤ 按钮 y0），且竖直一列。
  C. Add ▸ Material：CB-ADD kind=material ext=vmat path=...，
     并且文件**真的落到磁盘上**（脚本检查后删掉，不留垃圾）。
  D. 导入：点击 → CB-IMPORT request dir=... 且（脚本注入
     MYVK_EDITOR_CB_NO_DIALOG）CB-IMPORT dialog=suppressed
     —— 真对话框会阻塞进程，脚本没法点它；这条断言验证的是
     "菜单项确实接到了导入流程"。
  E. 保存所有：点击 → CB-SAVEALL dir=...，接着 EditorApp 执行
     CB-SAVEALL done scene=...（说明请求真的被接走了，不是空按钮）。
  F. 上一层 / 下一层：进 scenes 后点 ‹ → CB-HIST back rel=assets
     且路径框跟着变成 /assets；再点 › → CB-HIST fwd rel=assets/scenes。
  G. 零 VUID / 零 ERROR。

用法：python tools/verify_content_action_row.py
"""

import ctypes
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from capture_window import capture_stable, find_window                # noqa: E402
from verify_add_menu import LogTap, click, force_foreground           # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "..", "_venn_verify")
EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")

BAD_RE = r"VUID-|\[ERROR\]|Validation Error|leaked|SEGV"

# Add ▸ 的 13 种资源类型（顺序 = 菜单顺序 = 用户口述顺序）
KINDS = ["material", "model", "scene", "animation", "cinematic", "media",
         "water", "texture", "physics", "audio", "foliage", "ui", "worldmap"]

fails = []


def check(ok, msg):
    print("    %-68s %s" % (msg, "PASS" if ok else "FAIL"))
    if not ok:
        fails.append(msg)
    return ok


def center(r):
    return (r[0] + r[2]) * 0.5, (r[1] + r[3]) * 0.5


def count(tap, needle):
    with tap.lock:
        return sum(1 for l in tap.lines if needle in l)


def last_path(tap):
    """路径框的**当前**内容：CB-PATH rel=... 只有变化时才打，取最后一条。

    ⚠ 不能像以前那样用 count(tap, "CB-PATH rel=/assets") > 0 判断
      "回到 /assets 了" —— 启动那一刻就打过一条 /assets，之后永远为真，
      于是断言恒 PASS（第一版就是这么错的：‹ 明明没生效，F2 却过了）。
    """
    with tap.lock:
        hits = [l.split("CB-PATH rel=", 1)[1].strip()
                for l in tap.lines if "CB-PATH rel=" in l]
    return hits[-1] if hits else None


# ⚠ 正则必须**以 tag 本身**为锚点，不能写成"一串合法字符 + =(坐标)"。
#   日志行前面还有时间戳（`2026-10-17 19:52:05`），那串字符同样落进
#   字符类里，贪婪匹配会把时间戳和 tag 一起吃掉，group(1) 就永远不等于
#   tag —— 症状是"矩形明明打出来了却取不到"。
def rect_of(tap, tag):
    rx = re.compile(re.escape(tag) + r"(?:#\d+)?=\((-?[\d.]+),(-?[\d.]+)\)"
                                  r"-\((-?[\d.]+),(-?[\d.]+)\)")
    with tap.lock:
        found = None
        for line in tap.lines:
            m = rx.search(line)
            if m:
                found = tuple(float(m.group(i)) for i in range(1, 5))
        return found


def click_until(hwnd, tap, x, y, needle, n_before, tries=3, wait=0.9):
    for _ in range(tries):
        click(hwnd, x, y)
        time.sleep(wait)
        if count(tap, needle) > n_before:
            return True
    return False


def main():
    if not os.path.exists(EXE):
        print("找不到可执行文件，请先构建：", EXE)
        return 1

    from verify_add_menu import kill_stale_editors
    kill_stale_editors()
    os.makedirs(OUT_DIR, exist_ok=True)

    env = dict(os.environ)
    env.update({
        "MYVK_LOG_RECTS": "1",
        "MYVK_NO_WINDOW_SAVE": "1",
        "MYVK_LIGHT_GIZMOS": "0",
        "MYVK_EDITOR_NO_HUD": "1",
        "MYVK_GRID": "0",
        "MYVK_EDITOR_CONTENT_DRAWER": "1",
        # 导入对话框是模态阻塞的 —— 脚本点不了它。给它开关，只走流程。
        "MYVK_EDITOR_CB_NO_DIALOG": "1",
    })

    proc = subprocess.Popen([EXE], cwd=os.path.dirname(EXE), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    user32 = ctypes.windll.user32

    created = None
    try:
        hwnd = find_window("Venn Editor")
        if not hwnd:
            print("未找到编辑器窗口")
            return 2
        user32.ShowWindow(hwnd, 9)
        time.sleep(1.5)
        user32.SetWindowPos(hwnd, 0, 0, 0, 1600, 900, 0x0002 | 0x0004)
        time.sleep(3.0)
        force_foreground(hwnd)
        time.sleep(1.0)

        # ---------------------------------------------------------- A
        print("[A] 动作条：位置与顺序")
        add = rect_of(tap, "CB-ADD-BTN")
        save = rect_of(tap, "CB-SAVEALL")
        prev = rect_of(tap, "CB-PREV")
        nxt = rect_of(tap, "CB-NEXT")
        path = rect_of(tap, "CB-PATH")
        search = tap.last_rect("CB-SEARCH")
        check(all(r is not None for r in (add, save, prev, nxt, path, search)),
              "A1 五个控件 + 搜索框矩形都拿到了")
        if all(r is not None for r in (add, save, prev, nxt, path, search)):
            check(add[0] < save[0] < prev[0] < nxt[0] < path[0],
                  "A2 从左往右：Add < SaveAll < ‹ < › < 路径框 "
                  "(%.0f<%.0f<%.0f<%.0f<%.0f)"
                  % (add[0], save[0], prev[0], nxt[0], path[0]))
            ys = [r[1] for r in (add, save, prev, nxt)]
            check(max(ys) - min(ys) <= 6.0,
                  "A3 四个按钮在同一行（y 差 %.1f px）" % (max(ys) - min(ys)))
            check(path[3] <= search[1] + 1.0,
                  "A4 整条动作行在搜索框上方 (%.0f ≤ %.0f)"
                  % (path[3], search[1]))
            widths = [(path[2] - path[0], "path")]
            widths += [(r[2] - r[0], n) for r, n in
                       ((add, "add"), (save, "save"),
                        (prev, "prev"), (nxt, "next"))]
            widest = max(widths)[1]
            check(widest == "path",
                  "A5 路径框最宽（%.0f px，吃满剩余宽度）" % (path[2] - path[0]))
            check(add[2] - add[0] > 50.0,
                  "A6 Add 是[图标+文字]（宽 %.0f px > 50）"
                  % (add[2] - add[0]))
        check(last_path(tap) == "/assets",
              "A7 初始路径框显示 /assets（相对项目根，当前=%s）"
              % last_path(tap))

        # ---------------------------------------------------------- B
        print("[B] Add 子菜单")
        if add:
            n_menu = count(tap, "CB-ADDITEM")
            click(hwnd, *center(add))
            time.sleep(1.0)
            items = {k: rect_of(tap, "CB-ADDITEM " + k) for k in KINDS}
            check(all(v is not None for v in items.values()),
                  "B1 13 种资源类型项都在菜单里")
            imp = rect_of(tap, "CB-ADDITEM import")
            check(imp is not None, "B2 菜单里有「导入」项")
            if all(v is not None for v in items.values()) and imp:
                below = all(v[3] <= add[1] + 1.0 for v in items.values())
                check(below,
                      "B3 菜单开在按钮的**上侧**（所有项 y1 ≤ %.0f）" % add[1])
                xs = [v[0] for v in items.values()]
                check(max(xs) - min(xs) <= 2.0,
                      "B4 菜单项竖直一列（x 差 %.1f px）" % (max(xs) - min(xs)))
                seq = [items[k][1] for k in KINDS]
                check(seq == sorted(seq) and imp[1] > items["worldmap"][1],
                      "B5 顺序与用户口述一致，导入在最后")
            check(count(tap, "CB-ADDITEM") > n_menu,
                  "B6 打开菜单后 CB-ADDITEM 日志确实新增了")
            # 菜单展开的样子留一张图（人眼验收用）
            img = capture_stable(hwnd)
            if img is not None:
                img.save(os.path.join(OUT_DIR, "cb_add_menu.png"))

        # ---------------------------------------------------------- C
        print("[C] Add ▸ Material 真的落盘")
        if add:
            mat = rect_of(tap, "CB-ADDITEM material")
            if mat:
                n_add = count(tap, "CB-ADD kind=material")
                click(hwnd, *center(mat))
                time.sleep(1.2)
                ok = count(tap, "CB-ADD kind=material") > n_add
                check(ok, "C1 点 Material → CB-ADD kind=material")
                with tap.lock:
                    hit = [l for l in tap.lines if "CB-ADD kind=material" in l]
                if hit:
                    m = re.search(r"path=(.+)$", hit[-1])
                    if m:
                        created = m.group(1).strip()
                check(created is not None and os.path.exists(created),
                      "C2 资产文件真的写到了磁盘（%s）"
                      % (os.path.basename(created) if created else "-"))
            else:
                check(False, "C1/C2 拿不到 material 菜单项矩形")

        # ---------------------------------------------------------- F（先做导航，需要历史）
        print("[F] 上一层 / 下一层")
        node = tap.last_rect("CB-TREE-NODE scenes")
        if node:
            click_until(hwnd, tap, *center(node),
                        "CB-ENTRIES dir=assets/scenes",
                        count(tap, "CB-ENTRIES dir=assets/scenes"))
        check(last_path(tap) == "/assets/scenes",
              "F0 进 scenes 后路径框变成 /assets/scenes（当前=%s）"
              % last_path(tap))
        if prev:
            n_back = count(tap, "CB-HIST back")
            click_until(hwnd, tap, *center(prev), "CB-HIST back", n_back)
            check(count(tap, "CB-HIST back rel=assets") > 0,
                  "F1 点 ‹ → CB-HIST back rel=assets")
            check(last_path(tap) == "/assets",
                  "F2 路径框跟着回到 /assets（当前=%s）" % last_path(tap))
        if nxt:
            n_fwd = count(tap, "CB-HIST fwd")
            click_until(hwnd, tap, *center(nxt), "CB-HIST fwd", n_fwd)
            check(count(tap, "CB-HIST fwd rel=assets/scenes") > 0,
                  "F3 点 › → CB-HIST fwd rel=assets/scenes")
            check(last_path(tap) == "/assets/scenes",
                  "F4 路径框又跟着前进到 /assets/scenes（当前=%s）"
                  % last_path(tap))

        # ---------------------------------------------------------- E
        print("[E] 保存所有")
        if save:
            click(hwnd, *center(save))
            time.sleep(1.2)
            check(count(tap, "CB-SAVEALL dir=") > 0,
                  "E1 点 Save All → CB-SAVEALL dir=...")
            check(count(tap, "CB-SAVEALL done scene=") > 0,
                  "E2 请求被 EditorApp 接走并真的存盘（done scene=...）")

        # ---------------------------------------------------------- D
        print("[D] 导入")
        if add:
            click(hwnd, *center(add))
            time.sleep(0.9)
            imp = rect_of(tap, "CB-ADDITEM import")
            if imp:
                click(hwnd, *center(imp))
                time.sleep(1.0)
            check(count(tap, "CB-IMPORT request") > 0,
                  "D1 点「导入」→ CB-IMPORT request（菜单项接上了导入流程）")
            check(count(tap, "CB-IMPORT dialog=suppressed") > 0,
                  "D2 走到真正开对话框那一步（脚本注入开关后为 suppressed）")

        # ---------------------------------------------------------- 截图
        img = capture_stable(hwnd)
        if img is not None:
            img.save(os.path.join(OUT_DIR, "cb_actions.png"))

        # ---------------------------------------------------------- G
        print("[G] 静默")
        with tap.lock:
            bad = [l for l in tap.lines if re.search(BAD_RE, l)]
        check(not bad, "G1 零 VUID / 零 ERROR")
        if bad:
            for l in bad[:5]:
                print("      !", l)

    finally:
        # 清掉 C 组造出来的资产文件 —— 别在仓库里留垃圾
        if created and os.path.exists(created):
            try:
                os.remove(created)
                print("    (清理测试资产: %s)" % os.path.basename(created))
            except OSError as e:
                print("    (清理失败 %s: %s)" % (created, e))
        try:
            proc.terminate()
            proc.wait(timeout=8)
        except Exception:
            pass

    print()
    if fails:
        print("%d 项 FAIL：" % len(fails))
        for f in fails:
            print("  -", f)
        return 1
    print("全部 PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
