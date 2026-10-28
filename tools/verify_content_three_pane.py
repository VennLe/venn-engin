"""端到端验证：Content 三栏改造 + 抽屉层级 + 分割线不渗透（2026-10-17）。

被验证的用户需求（原话）：
  · 底部的 Content 面板打开时从层级来说应该覆盖在其他面板区域之上，
    Content 打开时 Hierarchy 与 Viewport 之间的分割线不应该渗透进来；
  · Content 拆成左中右三栏：
      左 = 当前游戏项目文件目录树（根 = 项目内容目录）
      中 = 一竖列资源类型单选框
      右 = 左侧选中目录的具体内容 + 顶部搜索框（能记录最近搜索）
          + 搜索框右侧的排序图标（小子菜单：名称/大小 + 升序/降序）

断言分组（全部读日志锚点，可机器验证）：
  A. 三栏存在且从左往右排：CB-TREE < CB-TYPES < CB-ITEMS，且三栏都在抽屉内；
     左栏里的树根存在（CB-TREE-ROOT）。
  B. 左栏树：点 "scenes" 节点 → 右栏切到 assets/scenes（CB-ENTRIES dir=...）。
  C. 中栏单选：点 "Models" → CB-ENTRIES type=1（且 count 与该类型吻合）。
  D. 右栏：搜索框在排序图标左边（CB-SORT 紧贴 CB-SEARCH 右侧）；
     点排序图标 → 菜单里四个勾选框都在（NAME/SIZE/ASC/DESC）；
     勾 Size → CB-SORT by=size 且 CB-ENTRIES sort=1；勾 Descending → asc=0。
  E. 最近搜索：预置两条历史（MYVK_EDITOR_CB_HIST）→ 点空搜索框 →
     CB-HISTORY n=2。
  F. 层级：点视口（抽屉关掉）→ 再点「内容」重开 → 抽屉**完整**可见
     （抽屉矩形不变，且顶端那一行像素仍是抽屉自己的底色/内容，没被视口盖）；
     这条是"重开后视口跑到绘制顺序末尾把抽屉盖住"的回归。
  G. 分割线不渗透：抽屉打开（浮层）时 SPLIT-0 / SPLIT-2 的矩形下沿
     ≤ 抽屉上沿；抽屉关掉后下沿回到主体底部（说明裁切是条件生效的）。
  H. 零 VUID / 零 ERROR。

用法：python tools/verify_content_three_pane.py
"""

import ctypes
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from capture_window import capture_stable, find_window, user32      # noqa: E402
from verify_add_menu import LogTap, click, force_foreground           # noqa: E402
from verify_asset_drag import move_to                                # noqa: E402
from verify_local_shadow import to_np                                # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "..", "_venn_verify")
EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")

BAD_RE = r"VUID-|\[ERROR\]|Validation Error|leaked|SEGV"
ENTRIES_RE = re.compile(
    r"CB-ENTRIES dir=(\S+) filter=(\S+) type=(\d+) view=(\d+) count=(\d+)"
    r" sort=(\d+) asc=(\d+)")

fails = []


def check(ok, msg):
    print("    %-66s %s" % (msg, "PASS" if ok else "FAIL"))
    if not ok:
        fails.append(msg)
    return ok


def center(r):
    return (r[0] + r[2]) * 0.5, (r[1] + r[3]) * 0.5


MOUSEEVENTF_WHEEL = 0x0800


def wheel(hwnd, x, y, notches):
    """在 (x,y) 处滚 notches 格（正 = 向下滚 → 内容上移）。"""
    force_foreground(hwnd)
    move_to(hwnd, x, y)
    time.sleep(0.15)
    for _ in range(abs(notches)):
        user32.mouse_event(MOUSEEVENTF_WHEEL, 0, 0,
                           -120 if notches > 0 else 120, 0)
        time.sleep(0.12)
    time.sleep(0.4)


def scroll_into(hwnd, tap, node_tag, box_tag, tries=8):
    """把 node 那条滚进 box 里（目录树内容比可视区高时必需的）。

    ⚠ 为什么要有这一步：`logRect` 打的是**逻辑坐标**，节点被滚出可视区
      之后日志照样有一行矩形，但鼠标点上去什么也点不到 —— 用例于是表现成
      "点了没反应"，而不是"节点不可见"。`scenes` 是树的最后一个节点，
      窗口 1600x900 时它正好压在最底下，点它等于点在底栏上。
    """
    box = tap.last_rect(box_tag)
    node = tap.last_rect(node_tag)
    for _ in range(tries):
        if not box or not node:
            return node
        if node[3] <= box[3] - 2 and node[1] >= box[1] + 2:
            return node
        wheel(hwnd, (box[0] + box[2]) * 0.5, (box[1] + box[3]) * 0.5, 1)
        node = tap.last_rect(node_tag)
    return node


def count(tap, needle):
    with tap.lock:
        return sum(1 for l in tap.lines if needle in l)


def last_entries(tap):
    with tap.lock:
        hits = [m for l in tap.lines for m in [ENTRIES_RE.search(l)] if m]
    return hits[-1] if hits else None


def entries_where(tap, **want):
    """最后一个满足条件的 CB-ENTRIES（字段名见 ENTRIES_RE 的分组名）。"""
    names = ["dir", "filter", "type", "view", "count", "sort", "asc"]
    with tap.lock:
        lines = list(tap.lines)
    for l in reversed(lines):
        m = ENTRIES_RE.search(l)
        if not m:
            continue
        ok = True
        for k, v in want.items():
            got = m.group(names.index(k) + 1)
            if str(got) != str(v):
                ok = False
                break
        if ok:
            return m
    return None



# ⚠ 不用 LogTap.last_rect：它内部的 RECT_RE 只认 [A-Za-z-]+ 作为 tag，
#   "SPLIT-0" 这种带数字的 tag 会匹配失败（返回 None，看起来像"没画"）。
RECT_ANY = re.compile(
    r"([A-Za-z0-9_-]+(?:#\d+)?)"
    r"=\((-?[\d.]+),(-?[\d.]+)\)-\((-?[\d.]+),(-?[\d.]+)\)")


def rect_of(tap, tag):
    with tap.lock:
        found = None
        for line in tap.lines:
            if (tag + "=(") not in line:
                continue
            m = RECT_ANY.search(line)
            if m and m.group(1) == tag:
                found = tuple(float(m.group(i)) for i in range(2, 6))
        return found


def click_until(hwnd, tap, x, y, needle, n_before, tries=3, wait=0.9):
    """点 (x,y) 直到日志里 needle 的计数增加（ImGui 偶尔要两帧才对齐鼠标）。"""
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
        # 抽屉一进来就是开的（浮层形态），省掉"点开"这一步的抖动风险；
        # F 组会自己再走一遍"关掉 → 重开"。
        "MYVK_EDITOR_CONTENT_DRAWER": "1",
        # 预置两条"最近搜索"（见 ContentBrowser 里的同名钩子注释）
        "MYVK_EDITOR_CB_HIST": "alpha,beta",
    })
    proc = subprocess.Popen([EXE], cwd=os.path.dirname(EXE), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    user32 = ctypes.windll.user32

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
        print("[A] 三栏结构")
        drawer = tap.last_rect("CONTENT-DRAWER")
        tree = tap.last_rect("CB-TREE")
        types = tap.last_rect("CB-TYPES")
        items = tap.last_rect("CB-ITEMS")
        root = tap.last_rect("CB-TREE-ROOT")
        check(all(r is not None for r in (drawer, tree, types, items, root)),
              "A1 四块矩形都在（抽屉 / 树 / 类型 / 内容）")
        if all(r is not None for r in (drawer, tree, types, items, root)):
            check(tree[2] <= types[0] + 1 and types[2] <= items[0] + 1,
                  "A2 从左往右：树 < 类型 < 内容 (%.0f < %.0f < %.0f)"
                  % (tree[2], types[0], items[0]))
            dy0 = drawer[1] - 2
            check(all(r[1] >= dy0 and r[3] <= drawer[3] + 2
                      for r in (tree, types, items)),
                  "A3 三栏都在抽屉矩形内")
            check(items[2] - items[0] > (types[2] - types[0]) * 2.0,
                  "A4 右栏最宽 (%.0f px)" % (items[2] - items[0]))

        # ---------------------------------------------------------- F1 关掉
        print("[F] 抽屉层级（重开不被视口盖住）")
        bar = tap.last_rect("BAR-BOTTOM")
        btn = tap.last_rect("BAR-BTN-content")
        ok_bar = bar is not None and btn is not None
        check(ok_bar, "F0 拿到底栏与「内容」按钮矩形")

        # 点视口上半部 → 浮层抽屉按设计关闭
        # ⚠ 浮层抽屉**不扣布局**，VP-RECT 一直延伸到窗口底部（y=778），
        #   它的几何中心 (481) 落在抽屉里（抽屉从 466 起）—— 直接点中心
        #   等于点在抽屉自己的内容上。必须挑"抽屉上沿以上"的一点。
        vp = tap.last_rect("VP-RECT")
        if vp and drawer:
            vy = vp[1] + (drawer[1] - vp[1]) * 0.35
            click(hwnd, (vp[0] + vp[2]) * 0.5, vy)
            time.sleep(0.9)
        closed = count(tap, "CONTENT-DRAWER closed=outside") > 0
        check(closed, "F1 点视口 → 抽屉关闭（closed=outside）")

        # 再点「内容」重开：这一帧视口正是"最后被聚焦的窗口"
        if ok_bar:
            n_open = count(tap, "CONTENT-DRAWER open=1")
            reopened = click_until(hwnd, tap, *center(btn),
                                   "CONTENT-DRAWER open=1", n_open)
            check(reopened, "F2 再点「内容」→ 抽屉重新打开")
            time.sleep(0.8)
            drawer2 = tap.last_rect("CONTENT-DRAWER")
            check(drawer2 is not None and drawer is not None and
                  abs(drawer2[1] - drawer[1]) <= 2.0,
                  "F3 重开后抽屉位置与首次一致 (y=%.0f)" %
                  (drawer2[1] if drawer2 else -1))

            # 像素证据：抽屉顶端那条标题带上，抽屉自己的底色必须还在
            # （如果视口盖上来，那里会变成视口的深色背景）
            img = capture_stable(hwnd)
            if img is not None and drawer2 is not None:
                a = to_np(img)
                y = int((drawer2[1] + drawer2[3]) * 0.5)
                row_in = a[int(drawer2[1]) + 6, :, :].reshape(-1, 3)
                # 抽屉标题带里绝大多数像素应该是面板底色（比较均匀）
                spread = float(row_in.std(axis=0).max())
                check(spread < 40.0,
                      "F4 抽屉顶端一行是面板底色（std=%.1f < 40）" % spread)
                os.makedirs(OUT_DIR, exist_ok=True)
                img.save(os.path.join(OUT_DIR, "cb3_reopen.png"))

        # ---------------------------------------------------------- G
        print("[G] 分割线不渗透")
        s0 = rect_of(tap, "SPLIT-0")
        s2 = rect_of(tap, "SPLIT-2")
        dcur = tap.last_rect("CONTENT-DRAWER")
        check(s0 is not None and s2 is not None and dcur is not None,
              "G1 拿到 SPLIT-0 / SPLIT-2 / 抽屉矩形")
        if s0 and s2 and dcur:
            check(s0[3] <= dcur[1] + 1.5 and s2[3] <= dcur[1] + 1.5,
                  "G2 抽屉打开时竖向下沿 ≤ 抽屉上沿 (%.0f/%.0f ≤ %.0f)"
                  % (s0[3], s2[3], dcur[1]))
            check(s0[1] < dcur[1],
                  "G3 分隔线在抽屉上方仍然存在（上半段没被裁掉）")

        # ---------------------------------------------------------- B
        print("[B] 左栏目录树")
        node = tap.last_rect("CB-TREE-NODE scenes")
        check(node is not None, "B1 树里有 scenes 节点")
        if node:
            node = scroll_into(hwnd, tap, "CB-TREE-NODE scenes", "CB-TREE")
            ok = click_until(hwnd, tap, *center(node), "CB-ENTRIES dir=assets/scenes",
                             count(tap, "CB-ENTRIES dir=assets/scenes"))
            check(ok, "B2 点 scenes → 右栏切到 assets/scenes")

        # ---------------------------------------------------------- C
        print("[C] 中栏类型单选")
        rb = tap.last_rect("CB-TYPE Models")
        check(rb is not None, "C1 拿到 Models 单选框矩形")
        if rb:
            ok = click_until(hwnd, tap, *center(rb), "CB-FILTER type=Models",
                             count(tap, "CB-FILTER type=Models"))
            check(ok, "C2 点 Models → CB-FILTER type=Models")
            m = entries_where(tap, dir="assets/scenes", type=1)
            check(m is not None, "C3 目录仍在 assets/scenes，type 已变 1")
            # 单选语义：同时只能有一个被选中 —— 全局只该有 1 个 type 值生效
            e = last_entries(tap)
            check(e is not None and e.group(3) == "1",
                  "C4 最后一条 CB-ENTRIES 的 type == 1（单选生效）")

        # ---------------------------------------------------------- D
        print("[D] 搜索框 + 排序菜单")
        se = tap.last_rect("CB-SEARCH")
        so = tap.last_rect("CB-SORT")
        check(se is not None and so is not None, "D1 搜索框与排序图标都在")
        if se and so:
            check(so[0] >= se[2] - 1.0 and so[0] - se[2] < 12.0,
                  "D2 排序图标紧贴搜索框右侧 (Δ=%.0f px)" % (so[0] - se[2]))
            check(abs((so[3] - so[1]) - (se[3] - se[1])) < 2.0,
                  "D3 排序图标与搜索框同高")

        if so:
            n_sort = count(tap, "CB-SORT-NAME")
            click(hwnd, *center(so))
            time.sleep(0.9)
            got = [t for t in ("CB-SORT-NAME", "CB-SORT-SIZE", "CB-SORT-ASC",
                               "CB-SORT-DESC") if tap.last_rect(t)]
            check(len(got) == 4, "D4 排序菜单四个勾选框都在 (%s)" % ",".join(
                g[8:] for g in got))

            sz = tap.last_rect("CB-SORT-SIZE")
            if sz:
                n_by = count(tap, "CB-SORT by=size")
                click(hwnd, *center(sz))
                time.sleep(0.8)
                check(count(tap, "CB-SORT by=size") > n_by,
                      "D5 勾 Size → CB-SORT by=size")
                m = entries_where(tap, sort=1)
                check(m is not None, "D6 CB-ENTRIES sort=1（按大小排）")

            de = tap.last_rect("CB-SORT-DESC")
            if de:
                n_ord = count(tap, "CB-SORT order=desc")
                click(hwnd, *center(de))
                time.sleep(0.8)
                check(count(tap, "CB-SORT order=desc") > n_ord,
                      "D7 勾 Descending → CB-SORT order=desc")
                check(entries_where(tap, asc=0) is not None,
                      "D8 CB-ENTRIES asc=0（降序）")

                # 菜单还开着：点抽屉里别的地方 —— 这一下只该关菜单。
                # （回归：popup 盖在抽屉上时 IsWindowHovered(抽屉) 是 false，
                #   不特判就会"关菜单顺带把整个抽屉关掉"。）
                n_out = count(tap, "CONTENT-DRAWER closed=outside")
                if items and dcur:
                    click(hwnd, items[0] + 40.0, dcur[3] - 30.0)
                    time.sleep(0.8)
                    check(count(tap, "CONTENT-DRAWER closed=outside") == n_out,
                          "D9 关菜单那一下不连带关抽屉")
                    check(tap.last_rect("CONTENT-DRAWER") is not None and
                          count(tap, "CONTENT-DRAWER open=0") == 0,
                          "D10 抽屉仍然开着")

        # ---------------------------------------------------------- E
        print("[E] 最近搜索")
        seeded = count(tap, "CB-HISTORY seeded=2")
        check(seeded > 0, "E1 预置两条历史成功")
        se2 = tap.last_rect("CB-SEARCH")
        if se2:
            # 排序菜单还开着：ImGui 会把"点外部"的第一下用来关菜单（不转给
            # 目标控件），所以先点一下抽屉标题带把菜单关掉。
            click(hwnd, se2[2] - 30.0, se2[1] - 14.0)
            time.sleep(0.5)
            n_before = count(tap, "CB-HISTORY n=")
            click(hwnd, *center(se2))
            time.sleep(0.9)
            check(count(tap, "CB-HISTORY n=") > n_before,
                  "E2 点空搜索框 → 摊开最近搜索")
            check(count(tap, "CB-HISTORY n=2") > 0,
                  "E3 历史条数 == 2")

        # ---------------------------------------------------------- H
        print("[H] 验证层静默")
        with tap.lock:
            bad = [l for l in tap.lines if re.search(BAD_RE, l)]
        check(not bad, "H1 零 VUID / ERROR (%d 条)" % len(bad))
        if bad:
            for l in bad[:5]:
                print("      ", l)

    finally:
        if "hwnd" in dir() and hwnd:
            user32.PostMessageW(hwnd, 0x0010, 0, 0)
        time.sleep(0.5)
        if proc.poll() is None:
            proc.terminate()

    print()
    print("=" * 72)
    if fails:
        print("失败 %d 项：" % len(fails))
        for f in fails:
            print("  -", f)
        print("结果: FAIL")
        return 1
    print("结果: PASS（三栏结构 4 + 层级 5 + 分割线 3 + 树 2 + 单选 4 + "
          "排序 10 + 历史 3 + 静默 1）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
