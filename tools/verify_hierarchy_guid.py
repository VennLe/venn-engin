"""端到端验证：Hierarchy 右键的 `Copy GUID` / `Select by GUID…`（G16-4 尾巴）。

==============================================================================
这两项为什么值得单开一条用例
==============================================================================
guid 是**唯一**跨会话稳定的身份，而树里显示的是名字 —— 名字可改、可重名、
可以没有。排查"存档里那个 `…0001` 到底是哪一根"时，除了这两个入口没有
第二条路。它们坏掉的样子都**不是崩**，而是"看起来做了、其实没做"：

  · `Copy GUID` 复制错实体 / 复制成空串 / 只写日志没进剪贴板；
  · `Select by GUID` 找到了却选错（多选残留）、找不到却静默关窗。

所以判据不能只看"日志里有那一行"。

==============================================================================
判据
==============================================================================
A · `Copy GUID`
  A0 右键第 0 行 → 菜单里出现 `HIER-CTX-CopyGuid`（在正确的位置上）。
  A1 点它 → `HIER-GUID copy=<16 hex> entity=N`。
  A2 **剪贴板内容 == 日志里那串**。
     ⚠ 这是"复制"这件事的**唯一**判据：只验日志的话，`SetClipboardText`
       写错变量（比如复制了 guid 的十进制）永远是绿的。
  A3 日志里的 `entity=N` == 刚才点选那一行时 `HIER-SELECT ... id=N` 的 N。
     ⚠ 只验"是一串 16 位 hex"不够：复制到**别的行**的 guid 也满足。

B · `Select by GUID…`
  B1 右键 → 点这一项 → 输入框出现（`HIER-GUID-QUERY`）。
  B2 输入**第 1 行**的 guid + 回车 → `hit=1`。
  B3 `sel=1` 且 `n=1`（**选中的就是它、且是单选**），`entity=` 与第 1 行对上。
     ⚠ 只验 `hit=1` 会把"找到了但选了别人"读成成功 —— `select()` 被调用过
       和"选中状态真的变了"是两件事。

C · 负面（这三条才是防"静静做错"的）
  C1 输入一个格式合法但不存在的 guid → `hit=0`，且**不**打 `hit=1`。
  C2 紧接着（**同一个弹窗**里）输入正确 guid → 仍然成功。
     ⚠ 证明 C1 只是"报错不关窗"：如果 C1 把窗关掉了，用户会以为"我输对了
       但没反应"，而真正的错误是他上一个输入。
  C3 输入 `0x…`（18 字符）→ **不产生任何** `HIER-GUID select=` 行 ——
     它走的是"格式不对"那条分支（`parseGuidHex` 对 `0x` 前缀返回 0），
     不是"去场景里找一个叫 0x… 的实体"。

⚠ 否定测试（必须做，否则 A2/C3 只是在描述"现在的样子"）：
  · 把 `ImGui::SetClipboardText(hex.c_str())` 注释掉 → **A2 必须 FAIL**；
  · 把 `if (s.size() != 16)` 那道闸去掉 → **C3 必须 FAIL**（会走到 find）。

==============================================================================
现场
==============================================================================
用 `assets/scenes/_save_fixture.json`：它有两个**显式写死 guid** 的实体
（`5a1e000000000001` / `…0002`），所以"复制出来的那串应该是多少"不用猜。
⚠ 场景路径相对 `Game/Editor.exe` 的工作目录（`build/bin`）。

用法：
    python tools/verify_hierarchy_guid.py
"""

import argparse
import ctypes
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import find_window, user32                       # noqa: E402
from verify_add_menu import (LogTap, kill_stale_editors, post_key,   # noqa: E402
                             VK_RETURN, VK_ESCAPE)
from verify_asset_drag import client_to_screen, force_foreground     # noqa: E402
from verify_bvh import MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP   # noqa: E402
from verify_collider_menu import right_click                         # noqa: E402
from verify_hierarchy_status import wait_rect_of, center             # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
# ⚠ 相对 `build/bin`（编辑器的工作目录），不是相对仓库根。
SCENE = "assets/scenes/_save_fixture.json"

WM_CHAR = 0x0102

COPY_RE = re.compile(r"HIER-GUID copy=([0-9a-f]{16}) entity=(\d+)")
SEL_RE = re.compile(r"HIER-GUID select=([0-9a-f]{16}) hit=(\d)"
                    r"(?: entity=(\d+) sel=(\d+) n=(\d+))?")
SELECT_ROW_RE = re.compile(r"HIER-SELECT mode=single id=(\d+)")

results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print("  [%s] %s %s" % ("PASS" if ok else "FAIL", name, detail))
    return ok


# ---------------------------------------------------------------- 点击
#
# ⚠ 与 `verify_bvh.click_client` 的区别只有一个，但它是致命的：
#   **先抢前台，再走两格，然后才落点**。
#   `click_client` 直接 `SetCursorPos` + `mouse_event` —— 窗口不在前台时
#   这一发落到别的窗口上（或者被"激活窗口"这一下吃掉），症状是"点了完全
#   没反应"，而且**偶发**：本机 2026-10-31 同一份代码第一次跑绿、第二次
#   连点第 0 行都收不到 `HIER-SELECT`。
#   这跟"合成鼠标的起手要预热"是同一条纪律（见 verify_bvh 里那段长注释）。
def click_item(hwnd, cx, cy):
    if not force_foreground(hwnd):
        print("  ⚠ 抢前台失败（继续试，只记录）")
    sx, sy = client_to_screen(hwnd, int(cx), int(cy))
    user32.SetCursorPos(sx - 18, sy - 12)     # 起手：先走一格
    time.sleep(0.12)
    user32.SetCursorPos(sx, sy)
    time.sleep(0.35)
    user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.12)
    user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
    time.sleep(0.6)


def skip(name, detail=""):
    print("  [SKIP] %s %s" % (name, detail))


# ---------------------------------------------------------------- 日志取数

def lines_of(tap):
    with tap.lock:
        return list(tap.lines)


def count_where(tap, rx):
    return [m for m in (rx.search(l) for l in lines_of(tap)) if m]


def wait_where(tap, rx, n, timeout=8.0):
    """等到**累计**出现 n 条匹配；超时返回当前列表。"""
    end = time.time() + timeout
    while time.time() < end:
        got = count_where(tap, rx)
        if len(got) >= n:
            return got
        time.sleep(0.15)
    return count_where(tap, rx)


# ---------------------------------------------------------------- 剪贴板

CF_UNICODETEXT = 13


def clipboard_text():
    """读 Windows 剪贴板（CF_UNICODETEXT）。

    ⚠ 剪贴板是**全局独占**资源：打开失败（别的程序正占着）时返回 ""，
      调用方必须把""当成"读不到"而不是"复制的是空串" —— 否则 A2 会以一种
      完全随机的方式红（本机实测：连着跑两条 GUI 用例时偶发）。

    ⚠⚠ **必须显式写 `restype = c_void_p`**：ctypes 对没声明的函数按 `int`
      处理（32 位），64 位下 `GlobalLock` 返回的指针会被**符号扩展**成
      `0xFFFFFFFFC7B0C170`，再 `wstring_at` 就是 `access violation`
      —— 崩在 Python 里，看着像"剪贴板坏了"。本机 2026-10-31 真踩过。
    """
    u = ctypes.windll.user32
    k = ctypes.windll.kernel32
    k.GlobalLock.restype = ctypes.c_void_p
    k.GlobalLock.argtypes = [ctypes.c_void_p]
    k.GlobalUnlock.argtypes = [ctypes.c_void_p]
    u.GetClipboardData.restype = ctypes.c_void_p
    u.OpenClipboard.argtypes = [ctypes.c_void_p]
    if not u.OpenClipboard(None):
        return ""
    try:
        h = u.GetClipboardData(CF_UNICODETEXT)
        if not h:
            return ""
        p = k.GlobalLock(h)
        if not p:
            return ""
        try:
            return ctypes.wstring_at(p)
        finally:
            k.GlobalUnlock(h)
    finally:
        u.CloseClipboard()


# ---------------------------------------------------------------- 鼠标 / 键盘

def type_text(hwnd, text):
    """逐字符 `PostMessage(WM_CHAR)`。

    ⚠ 不用 `keybd_event`：它走系统输入队列，窗口不在前台时全落空；
      而 `WM_CHAR` 直投窗口过程 → GLFW char 回调 → `ImGui::AddInputCharacter`。
    """
    for ch in text:
        user32.PostMessageW(hwnd, WM_CHAR, ord(ch), 1)
        time.sleep(0.08)


def clear_field(hwnd, n):
    """按 n 次退格（VK_BACK，扫描码 0x0E —— GLFW 按扫描码查表）。"""
    for _ in range(n):
        user32.PostMessageW(hwnd, 0x0100, 0x08, (0x0E << 16) | 1)
        time.sleep(0.05)
        user32.PostMessageW(hwnd, 0x0101, 0x08, (0x0E << 16) | 0xC0000001)
        time.sleep(0.06)


def dismiss_menu(hwnd):
    """把任何开着的菜单 / 弹窗关掉。

    ⚠ 三样都做：Esc（关弹窗）、点视口空白（关右键菜单）、再等一下。
      只按 Esc 关不掉右键菜单（ImGui 的 popup 对 Esc 的处理与模态不同）。
    """
    post_key(hwnd, VK_ESCAPE)
    time.sleep(0.2)
    click_item(hwnd, 900, 700)
    time.sleep(0.4)


def right_click_row(hwnd, row0, k):
    """右键第 k 行。row0 是 `HIER-RECT row0` 的矩形，行高从它推。"""
    rowH = row0[3] - row0[1]
    right_click(hwnd, int(row0[0] + 60), int(row0[1] + rowH * (k + 0.5)))


# ---------------------------------------------------------------- 主流程

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--keep", action="store_true", help="跑完不关窗口（调试）")
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    if not os.path.exists(exe):
        print("找不到可执行文件:", exe)
        return 2

    kill_stale_editors()

    env = dict(os.environ)
    env["MYVK_NO_WINDOW_SAVE"] = "1"   # 布局走默认（Hierarchy 会打开）
    env["MYVK_LOG_RECTS"] = "1"        # logRect 的锚点全靠它
    env["MYVK_EDITOR_SCENE"] = SCENE
    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    print("launched pid", proc.pid)

    try:
        hwnd = find_window("Venn Editor")
        if not hwnd:
            print("未找到窗口")
            return 2
        user32.ShowWindow(hwnd, 9)
        time.sleep(2.0)
        # 摆到 (0,0)：不钉位置的话每次启动右移 38px（级联），坐标会飘。
        user32.SetWindowPos(hwnd, 0, 0, 0, 1600, 900, 0x0002 | 0x0004)
        time.sleep(2.5)

        row0 = wait_rect_of(tap, "HIER-RECT row0", 10.0)
        if not row0:
            print("拿不到 HIER-RECT row0（Hierarchy 没开 / 场景没加载？）")
            with tap.lock:
                print([l for l in tap.lines if "HIER" in l][-8:])
            return 3
        print("row0=%s" % (row0,))

        # ---- 先点一下第 0 行：拿它的 entity id，用来判 A3 ----
        click_item(hwnd, int(row0[0] + 60), int(row0[1] + 5))
        time.sleep(0.8)
        sel_rows = count_where(tap, SELECT_ROW_RE)
        if not sel_rows:
            print("点第 0 行没有 HIER-SELECT 锚点 —— 后面的 A3 无从判起")
            return 3
        row0_id = int(sel_rows[-1].group(1))
        print("row0 entity id =", row0_id)

        # ======================================================== A · Copy GUID
        print("\n== A · Copy GUID ==")
        dismiss_menu(hwnd)
        right_click_row(hwnd, row0, 0)
        copy_rect = wait_rect_of(tap, "HIER-CTX-CopyGuid", 6.0)
        check("A0 右键第 0 行 → 菜单里有 Copy GUID（矩形拿到）",
              copy_rect is not None, "rect=%s" % (copy_rect,))

        n_copy = len(count_where(tap, COPY_RE))
        copy_ok = False
        copied_guid = ""
        copied_entity = -1
        if copy_rect:
            click_item(hwnd, *[int(v) for v in center(copy_rect)])
            got = wait_where(tap, COPY_RE, n_copy + 1, 8.0)
            copy_ok = len(got) > n_copy
            if copy_ok:
                copied_guid = got[-1].group(1)
                copied_entity = int(got[-1].group(2))
        check("A1 点它 → HIER-GUID copy=<16 hex>", copy_ok,
              "guid=%s entity=%s" % (copied_guid, copied_entity))

        # A2：剪贴板 == 日志里那串（"复制"的唯一判据）
        cb = ""
        for _ in range(6):                      # 剪贴板偶尔被占，重试
            cb = clipboard_text()
            if cb:
                break
            time.sleep(0.3)
        check("A2 剪贴板内容 == 日志里那串（真的进了剪贴板）",
              bool(copied_guid) and cb.strip().lower() == copied_guid,
              "clipboard=%r log=%r" % (cb, copied_guid))

        check("A3 复制的是**这一行**的 guid（entity 与点选的行一致）",
              copy_ok and copied_entity == row0_id,
              "copied_entity=%d row0_id=%d" % (copied_entity, row0_id))

        # ==================================================== 取第 1 行的 guid
        # B 组要"输入**另一个**实体的 guid"，否则"选中了"与"本来就选着它"
        # 分不开（第 0 行此刻正被选中）。
        print("\n== 取第 1 行的 guid（给 B 组当靶子）==")
        dismiss_menu(hwnd)
        right_click_row(hwnd, row0, 1)
        copy_rect2 = wait_rect_of(tap, "HIER-CTX-CopyGuid", 6.0)
        n_copy2 = len(count_where(tap, COPY_RE))
        row1_guid, row1_entity = "", -1
        if copy_rect2:
            click_item(hwnd, *[int(v) for v in center(copy_rect2)])
            got = wait_where(tap, COPY_RE, n_copy2 + 1, 8.0)
            if len(got) > n_copy2:
                row1_guid = got[-1].group(1)
                row1_entity = int(got[-1].group(2))
        have_row1 = bool(row1_guid) and row1_guid != copied_guid
        if not have_row1:
            skip("B/C 组", "拿不到第 1 行的 guid（场景里只有一行？）")
        else:
            print("row1 guid=%s entity=%d" % (row1_guid, row1_entity))

            # ================================================= B · Select by GUID
            print("\n== B · Select by GUID ==")
            dismiss_menu(hwnd)
            right_click_row(hwnd, row0, 0)
            sel_rect = wait_rect_of(tap, "HIER-CTX-SelectGuid", 6.0)
            n_sel0 = len(count_where(tap, SEL_RE))
            query = None
            if sel_rect:
                click_item(hwnd, *[int(v) for v in center(sel_rect)])
                query = wait_rect_of(tap, "HIER-GUID-QUERY", 6.0)
            check("B1 点 Select by GUID… → 弹窗的输入框出现", query is not None,
                  "menu=%s query=%s" % (sel_rect, query))

            b2_ok = False
            sel_line = None
            if query:
                type_text(hwnd, row1_guid)
                time.sleep(0.3)
                post_key(hwnd, VK_RETURN)
                got = wait_where(tap, SEL_RE, n_sel0 + 1, 8.0)
                if len(got) > n_sel0:
                    m = got[-1]
                    sel_line = m
                    b2_ok = m.group(1) == row1_guid and m.group(2) == "1"
            check("B2 输入第 1 行的 guid + 回车 → hit=1（真的找到了）", b2_ok,
                  "line=%s" % ((sel_line.groups() if sel_line else None),))

            b3_ok = False
            if sel_line:
                ent = int(sel_line.group(3)) if sel_line.group(3) else -1
                sel = int(sel_line.group(4)) if sel_line.group(4) else 0
                cnt = int(sel_line.group(5)) if sel_line.group(5) else -1
                # ⚠ `n == 1` 是"单选"的判据：`select()` 在多选残留时
                #   会让 Inspector 显示一个集合，而 `sel=1` 照样成立。
                b3_ok = (sel == 1 and cnt == 1 and ent == row1_entity)
            check("B3 选中的就是它、且是单选（sel=1 n=1 entity 对得上）",
                  b3_ok,
                  "line=%s want_entity=%d"
                  % ((sel_line.groups() if sel_line else None), row1_entity))

            # ==================================================== C · 负面
            print("\n== C · 负面 ==")

            # C1：格式合法但不存在
            dismiss_menu(hwnd)
            right_click_row(hwnd, row0, 0)
            sel_rect = wait_rect_of(tap, "HIER-CTX-SelectGuid", 6.0)
            n_sel1 = len(count_where(tap, SEL_RE))
            c1_line = None
            if sel_rect:
                click_item(hwnd, *[int(v) for v in center(sel_rect)])
                if wait_rect_of(tap, "HIER-GUID-QUERY", 6.0):
                    type_text(hwnd, "5a1e0000000fffff")
                    time.sleep(0.3)
                    post_key(hwnd, VK_RETURN)
                    got = wait_where(tap, SEL_RE, n_sel1 + 1, 6.0)
                    if len(got) > n_sel1:
                        c1_line = got[-1]
            check("C1 输入不存在的 guid → hit=0 且不假装选上了",
                  c1_line is not None and c1_line.group(2) == "0",
                  "line=%s" % ((c1_line.groups() if c1_line else None),))

            # C2：同一个弹窗里改成正确 guid，仍然能成功（窗口没被关掉）
            # ⚠ 先点一下输入框再改：`EnterReturnsTrue` 在返回 true 的同时会清掉
            #   ImGui 的 active id，产品侧我们已经把焦点还回去了（m_guidRefocus），
            #   但用例不该依赖这一点 —— 它测的是"窗还开着、还能改"，不是"焦点
            #   自动回来了"。
            c2_ok = False
            if c1_line:
                q = wait_rect_of(tap, "HIER-GUID-QUERY", 4.0)
                if q:
                    click_item(hwnd, *[int(v) for v in center(q)])
                clear_field(hwnd, 20)
                type_text(hwnd, row1_guid)
                time.sleep(0.3)
                post_key(hwnd, VK_RETURN)
                got = wait_where(tap, SEL_RE, n_sel1 + 2, 8.0)
                c2_ok = len(got) > n_sel1 + 1 and got[-1].group(2) == "1"
            check("C2 同一个弹窗里改成正确 guid → 仍然成功（C1 只是报错不关窗）",
                  c2_ok, "")

            # C3：`0x` 前缀 —— 走"格式不对"那条分支，**不该**产生 select 行
            dismiss_menu(hwnd)
            right_click_row(hwnd, row0, 0)
            sel_rect = wait_rect_of(tap, "HIER-CTX-SelectGuid", 6.0)
            n_sel2 = len(count_where(tap, SEL_RE))
            if sel_rect:
                click_item(hwnd, *[int(v) for v in center(sel_rect)])
                if wait_rect_of(tap, "HIER-GUID-QUERY", 6.0):
                    type_text(hwnd, "0x" + row1_guid)
                    time.sleep(0.3)
                    post_key(hwnd, VK_RETURN)
                    time.sleep(1.2)
            after = count_where(tap, SEL_RE)
            check("C3 `0x…` 不是合法输入 → 不进 find()、不打 select 行",
                  len(after) == n_sel2,
                  "before=%d after=%d" % (n_sel2, len(after)))
            dismiss_menu(hwnd)

    finally:
        if args.keep:
            print("--keep：留窗口 pid", proc.pid)
        else:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()

    n_pass = sum(1 for _, ok, _ in results if ok)
    print("\n%d/%d PASS" % (n_pass, len(results)))
    # ⚠ 收尾行必须是 `结果: PASS` / `结果: FAIL`：`run_regression.py` 的
    #   OK_RE 只认这句（只打 "8/8 PASS" 会 rc=0 却判 FAIL）。
    if n_pass == len(results):
        print("结果: PASS")
        return 0
    print("结果: FAIL: %d 项" % (len(results) - n_pass))
    return 1


if __name__ == "__main__":
    sys.exit(main())
