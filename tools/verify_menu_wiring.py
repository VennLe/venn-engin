"""端到端验证：主菜单栏的"接线"（P2 那五条）。

为什么要有这个脚本：这五项原来是**灰的**或者只弹一句"请用快捷键"，
而它们背后的功能早就做完了（多选 / 复制 / 画质面板 / 原生导入对话框）。
灰项不会报错、也不会崩 —— 它只会让人以为引擎没有这个能力，所以只能靠
"点了之后**有没有真的发生**"来断言。

覆盖：
  A. GameObject ▸ Cube            真的建出实体（用后面的 SELECT all n=1 反证）
  B. Edit ▸ Select All            日志 `SELECT all n=1`（Ground 上锁，不参与）
  C. Edit ▸ Duplicate             日志 `HIER-DUP n=1`
  D. Edit ▸ Select All            日志 `SELECT all n=2`
  E. Edit ▸ Invert Selection      日志 `SELECT invert n=0`（反选后一个都不剩）
  F. Edit ▸ Project Settings...   画质面板出现（QUAL-RECT）
  G. File ▸ Save As               **写到 MYVK_EDITOR_SAVEAS_PATH 指定的文件**，
                                  且 assets/scenes/scene.json 一个字节没动
                                  （这就是原来那个 bug：写死 scene.json）
  H. File ▸ Import ▸ Import assets...  日志 `CB-IMPORT request source=menu`

⚠ 断言全部基于**日志 + 文件系统**，不猜像素坐标 —— 菜单项没有 ImGui ID
  可查，只能按 logRect 的矩形点，而"点到了"的判据是"产生了副作用"。

用法：
    python tools/verify_menu_wiring.py
"""

import argparse
import os
import re
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import find_window, user32  # noqa: E402
from verify_asset_drag import force_foreground  # noqa: E402
from verify_nav_gizmo import MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")

RECT_RE = re.compile(
    r"([A-Za-z0-9_-]+(?:#\d+)?)"
    r"=\((-?[\d.]+),(-?[\d.]+)\)-\((-?[\d.]+),(-?[\d.]+)\)")

WM_KEYDOWN = 0x0100
WM_KEYUP = 0x0101
VK_ESCAPE = 0x1B
SC_ESCAPE = 0x01


class LogTap:
    def __init__(self):
        self.lines = []
        self.lock = threading.Lock()

    def start(self, proc):
        threading.Thread(target=self._pump, args=(proc,), daemon=True).start()

    def _pump(self, proc):
        for raw in proc.stdout:
            with self.lock:
                self.lines.append(raw.strip())

    def last_rect(self, tag):
        rx = re.compile(r"\b" + re.escape(tag) + r"(?:#\d+)?=\(")
        with self.lock:
            found = None
            for line in self.lines:
                if rx.search(line):
                    m = RECT_RE.search(line)
                    if m:
                        found = tuple(float(m.group(i)) for i in range(2, 6))
            return found

    def has_line(self, needle):
        with self.lock:
            return any(needle in l for l in self.lines)

    def count(self, needle):
        with self.lock:
            return sum(1 for l in self.lines if needle in l)

    def find(self, needle):
        """返回**最后一条**含 needle 的行（断言里要报具体数字时用）。"""
        with self.lock:
            hits = [l for l in self.lines if needle in l]
            return hits[-1] if hits else None


def click(hwnd, cx, cy):
    from verify_asset_drag import client_to_screen
    sx, sy = client_to_screen(hwnd, cx, cy)
    user32.SetCursorPos(sx, sy)
    time.sleep(0.25)
    user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.12)
    user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
    time.sleep(0.5)


def esc(hwnd):
    user32.PostMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, (SC_ESCAPE << 16) | 1)
    time.sleep(0.04)
    user32.PostMessageW(hwnd, WM_KEYUP, VK_ESCAPE,
                        (SC_ESCAPE << 16) | 0xC0000001)
    time.sleep(0.3)


def center(rect):
    return ((rect[0] + rect[2]) * 0.5, (rect[1] + rect[3]) * 0.5)


def wait_rect(tap, tag, timeout=4.0):
    end = time.time() + timeout
    while time.time() < end:
        r = tap.last_rect(tag)
        if r:
            return r
        time.sleep(0.15)
    return None


def open_menu(tap, hwnd, tag):
    """点开一级菜单并等它的锚点出现。菜单已经开着时再点一下会收起 ——
    所以先 esc 把状态清干净（上一次操作后菜单通常已经自动关了）。"""
    r = wait_rect(tap, tag, 3.0)
    if not r:
        print("  ✗ 锚点 %s 一直没出现（菜单根本没画出来？）" % tag)
        return False
    click(hwnd, *center(r))
    time.sleep(0.4)
    return True


def click_item(tap, hwnd, tag, timeout=4.0):
    r = wait_rect(tap, tag, timeout)
    if not r:
        print("  ✗ 菜单项锚点 %s 没出现" % tag)
        return False
    click(hwnd, *center(r))
    time.sleep(0.6)
    return True


def do_menu(tap, hwnd, menu_tag, item_tag, expect=None, timeout=4.0):
    """点开一级菜单 → 点其中一项，然后**用日志确认它真的发生了**。

    ⚠ 必须带 `expect` 并且重试：菜单的展开是**一帧的事**，鼠标点下去的那
      一刻菜单可能还没画出来（第一下被当成"打开菜单"吃掉），于是第二下
      点在了同一位置 —— 表现为"这一步偶尔没反应"。纯坐标点击没有"我是否
      点到了"的反馈，只能靠**副作用**判：日志里出现期待的那一行才算数。
      （作者实测：同一份代码连跑两轮，E 组一次 PASS 一次 FAIL。）

    ⚠ 成功路径上**绝不能**按 Esc：编辑器里 Esc = 取消选择（菜单项
      `Select None` 的快捷键就写着 Esc）。每步之后顺手 Esc 一下会把上一步
      刚选好的集合清空 —— 下一步的 Duplicate 直接变灰，症状是"复制没反应"。
      菜单点中之后 ImGui 自己会关，不需要额外动作。
    """
    # ⚠ 用**计数**而不是 has_line：Select All 会被点好几次，has_line 从第二次
    #   起永远为真，重试就形同虚设（"没点上也说点上了"）。
    before = tap.count(expect) if expect else 0
    for attempt in range(3):
        ok = open_menu(tap, hwnd, menu_tag) and click_item(tap, hwnd, item_tag,
                                                          timeout)
        if not ok:
            esc(hwnd)
            continue
        if expect is None:
            return True
        end = time.time() + 1.5
        while time.time() < end:
            if tap.count(expect) > before:
                return True
            time.sleep(0.1)
        print("  retry: 点了 %s 但日志里没多出一条 %r（第 %d 次）"
              % (item_tag, expect, attempt + 1))
        esc(hwnd)
    return False


def kill_stale_editors():
    subprocess.run(["taskkill", "/F", "/IM", "Editor.exe"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    end = time.time() + 6.0
    while time.time() < end:
        if not find_window("Venn Editor", 0.35):
            break
        time.sleep(0.2)
    time.sleep(0.6)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--out-dir", default=os.path.join(ROOT, "..", "_venn_verify"))
    ap.add_argument("--size", default="1600x900")
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    out = os.path.abspath(args.out_dir)
    os.makedirs(out, exist_ok=True)
    if not os.path.exists(exe):
        print("找不到可执行文件，请先构建：", exe)
        return 1

    bindir = os.path.dirname(exe)
    scenes_dir = os.path.join(bindir, "assets", "scenes")
    os.makedirs(scenes_dir, exist_ok=True)

    # 目标路径走环境变量（脚本点不了系统对话框）。
    save_as_abs = os.path.join(scenes_dir, "_menu_saveas.json")
    if os.path.exists(save_as_abs):
        os.remove(save_as_abs)

    # 诱饵：旧实现把 Save As 写死成 `assets/scenes/scene.json`，只要这个
    # 文件存在就会被冲掉（这正是"另存为丢数据"的现场）。造一份已知内容
    # 的诱饵放在这儿，事后逐字节比对。
    #
    # ⚠ 没有 SAVEAS 修复时它的实际症状是二选一：
    #   · scene.json 存在     → 被静默覆盖（最坏）
    #   · scene.json 不存在   → resolveAssetPath 落到
    #                          `build/bin/assets/assets/scenes/scene.json`，
    #                          目录不存在 → saveScene 直接失败（不建目录），
    #                          通知栏一句 "Save failed"，用户以为存上了。
    decoy = os.path.join(scenes_dir, "scene.json")
    decoy_bytes = b'{"version": 1, "marker": "menu-wiring-decoy", "entities": []}\n'
    with open(decoy, "wb") as f:
        f.write(decoy_bytes)

    def scan_scenes():
        """场景目录（含拼错的 assets/assets）里所有 json 的指纹。"""
        out = {}
        for root in (scenes_dir, os.path.join(bindir, "assets", "assets")):
            if not os.path.isdir(root):
                continue
            for name in os.listdir(root):
                if not name.endswith(".json"):
                    continue
                p = os.path.abspath(os.path.join(root, name))
                try:
                    st = os.stat(p)
                except OSError:
                    continue
                out[p] = (st.st_size, st.st_mtime_ns)
        return out

    kill_stale_editors()

    child_env = dict(os.environ)
    child_env["MYVK_LOG_RECTS"] = "1"
    child_env["MYVK_NO_WINDOW_SAVE"] = "1"
    # 两个对话框都不开窗（脚本点不了系统对话框）
    child_env["MYVK_EDITOR_CB_NO_DIALOG"] = "1"
    child_env["MYVK_EDITOR_SAVEAS_PATH"] = save_as_abs

    proc = subprocess.Popen([exe], cwd=bindir, env=child_env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    print("launched pid", proc.pid)

    fails = []

    def check(name, ok, detail=""):
        print("  [%s] %s  %s" % ("PASS" if ok else "FAIL", name, detail))
        if not ok:
            fails.append(name)

    try:
        hwnd = find_window("Venn Editor")
        if not hwnd:
            print("未找到窗口")
            return 2
        user32.ShowWindow(hwnd, 9)
        time.sleep(2.0)
        tw, th = (int(v) for v in args.size.lower().split("x"))
        user32.SetWindowPos(hwnd, 0, 0, 0, tw, th, 0x0002 | 0x0004)
        time.sleep(2.5)
        force_foreground(hwnd)
        time.sleep(1.0)

        # ---------------------------------------------------------- A
        print("\n[A] GameObject ▸ Cube")
        do_menu(tap, hwnd, "MENU-GAMEOBJECT", "MENU-GO-0")   # Cube = 枚举值 0

        # ---------------------------------------------------------- B
        print("\n[B] Edit ▸ Select All → SELECT all n=1")
        do_menu(tap, hwnd, "MENU-EDIT", "MENU-EDIT-SELECTALL",
                expect="SELECT all n=")
        line = tap.find("SELECT all n=")
        n_all = None
        if line:
            m = re.search(r"n=(\d+)", line)
            n_all = int(m.group(1)) if m else None
        check("B1 点了 Select All 真的进到 EditorContext::selectAll",
              line is not None, line or "(没有 SELECT all 日志 = 菜单项还是灰的)")
        check("B2 场景里只有 1 个可选实体（Cube；Ground 上锁不参与）",
              n_all == 1, "n=%s" % n_all)

        # ---------------------------------------------------------- C
        print("\n[C] Edit ▸ Duplicate → HIER-DUP n=1")
        do_menu(tap, hwnd, "MENU-EDIT", "MENU-EDIT-DUP", expect="HIER-DUP")
        line = tap.find("HIER-DUP")
        check("C1 点了 Duplicate 真的复制了一份（不是弹提示）",
              line is not None, line or "(没有 HIER-DUP 日志)")

        # ---------------------------------------------------------- D
        print("\n[D] Edit ▸ Select All → SELECT all n=2")
        do_menu(tap, hwnd, "MENU-EDIT", "MENU-EDIT-SELECTALL",
                expect="SELECT all n=")
        line = tap.find("SELECT all n=")
        n2 = None
        if line:
            m = re.search(r"n=(\d+)", line)
            n2 = int(m.group(1)) if m else None
        check("D1 复制之后全选是 2 个", n2 == 2, "n=%s" % n2)

        # ---------------------------------------------------------- E
        print("\n[E] Edit ▸ Invert Selection → SELECT invert n=0")
        do_menu(tap, hwnd, "MENU-EDIT", "MENU-EDIT-INVERT",
                expect="SELECT invert n=")
        line = tap.find("SELECT invert n=")
        ninv = None
        if line:
            m = re.search(r"n=(\d+)", line)
            ninv = int(m.group(1)) if m else None
        check("E1 点了 Invert 真的进了 invertSelection", line is not None,
              line or "(没有 SELECT invert 日志)")
        # 全选 2 个 → 反选应该是 0 个。这一条同时证明"反选不是把全选重跑一遍"。
        check("E2 反选语义正确（全选后反选 = 空集）", ninv == 0, "n=%s" % ninv)

        # ---------------------------------------------------------- F
        print("\n[F] Edit ▸ Project Settings... → 画质面板出现")
        do_menu(tap, hwnd, "MENU-EDIT", "MENU-EDIT-PROJSET")
        qual = wait_rect(tap, "QUAL-RECT", 4.0)
        check("F1 Project Settings 打开的是画质面板（不再是『随 E26 落地』的提示）",
              qual is not None, str(qual))

        # ---------------------------------------------------------- G
        print("\n[G] File ▸ Save As → 写到指定路径，别的场景一个都不许动")
        before_scan = scan_scenes()
        do_menu(tap, hwnd, "MENU-FILE", "MENU-SAVEAS", expect="SAVEAS abs=")
        time.sleep(1.0)
        exists = os.path.exists(save_as_abs)
        size = os.path.getsize(save_as_abs) if exists else -1
        check("G1 目标文件真的被写出来了（走的是对话框选中的路径）",
              exists and size > 100, "%s size=%d" % (save_as_abs, size))
        line = tap.find("SAVEAS abs=")
        check("G2 日志里记下了实际落盘路径", line is not None, line or "")
        # 诱饵：原来这一项写死 `assets/scenes/scene.json`，只要这个文件存在
        # 就会被冲掉。上面专门造了一份已知内容的 scene.json 放在这儿。
        after_decoy = None
        if os.path.exists(decoy):
            with open(decoy, "rb") as f:
                after_decoy = f.read()
        check("G3 既有 scene.json 的内容一个字节没变（原来会被 Save As 覆盖）",
              after_decoy == decoy_bytes,
              "before=%dB after=%s" % (len(decoy_bytes),
                                       "None" if after_decoy is None
                                       else "%dB" % len(after_decoy)))
        # 兜底：除了目标文件，场景目录里不该有任何别的 json 被新建/改动。
        # 这条守的是"写到别的目录去了"（`assets/assets/...` 那种拼接错位）
        after_scan = scan_scenes()
        changed = sorted(set(after_scan) ^ set(before_scan)) + \
            sorted(k for k in set(after_scan) & set(before_scan)
                   if after_scan[k] != before_scan[k])
        strays = [p for p in changed if os.path.abspath(p) != save_as_abs]
        check("G4 没有顺手写到别的文件（目标之外零改动）",
              not strays, "多出来的：%s" % strays)

        # ---------------------------------------------------------- H
        print("\n[H] File ▸ Import ▸ Import assets... → 转到 Content 的导入通道")
        opened = open_menu(tap, hwnd, "MENU-FILE")
        if opened:
            sub_r = wait_rect(tap, "MENU-FILE-IMPORT", 3.0)
            if sub_r:
                # 悬停即可展开子菜单
                from verify_asset_drag import move_to
                move_to(hwnd, *center(sub_r))
                time.sleep(0.5)
                click_item(tap, hwnd, "MENU-IMPORT-ASSETS", 3.0)
        esc(hwnd)
        line = tap.find("CB-IMPORT request source=menu")
        check("H1 Import 菜单真的把请求交给了 ContentBrowser", line is not None,
              line or "(没有 source=menu 的请求日志)")
        check("H2 走的还是同一条导入通道（对话框被抑制，说明流程到了）",
              tap.has_line("CB-IMPORT dialog=suppressed"), "")

        # ---------------------------------------------------------- 收尾
        print("\n[I] 零 Vulkan 验证层报错")
        # 只认"真的报了"：前面几个断言已经把关键路径跑过一遍，这里做兜底
        has_err = any("VUID-" in l or "[ERROR]" in l for l in _snapshot(tap))
        check("I1 全程没有 VUID / ERROR", not has_err, "")

    finally:
        try:
            proc.terminate()
        except Exception:
            pass
        time.sleep(0.5)
        subprocess.run(["taskkill", "/F", "/IM", "Editor.exe"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    print("\n" + "=" * 60)
    if fails:
        print("结果: FAIL: " + "; ".join(fails))
        return 1
    print("结果: PASS")
    return 0


def _snapshot(tap):
    with tap.lock:
        return list(tap.lines)


if __name__ == "__main__":
    sys.exit(main())
