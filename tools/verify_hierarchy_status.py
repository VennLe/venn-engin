"""端到端验证：Hierarchy 行尾的 🔒 锁定 / 👁 显示 图标按钮。

用户原文（2026-10-17）：
    "Hierachy 中对于一些 object（比如 light，mesh 等等），其右侧对应的锁定
     图标按钮点击貌似无法生效。另外，请再加一个图标按钮并列，用于控制对应
     object 的显示或隐藏，其对应的 icon 可以用一个眼睛那样的。"

真因（本轮定位，不是"按钮没接上"）：
    ① 两个图标是**叠在 TreeNodeEx 行矩形上**的 `InvisibleButton`。ImGui 的
       命中测试是"**先提交的先占** HoveredId"——
          imgui.cpp::ItemHoverable():
            if (g.HoveredId != 0 && g.HoveredId != id && !g.HoveredIdAllowOverlap)
                return false;
       节点先提交就把 `g.HoveredId` 占了，后提交的图标在第一道闸就被挡掉 →
       **永远收不到 hover / click**（图标画得出来，点了没反应）。
       修法 = 给节点加 `ImGuiTreeNodeFlags_AllowOverlap`，命中测试变成
       "后提交的（图标）优先"（front-to-back）。
    ② 行尾 👁 那一格原来是"**有 VisibilityComponent 才画**"，而**唯一**没有
       这个组件的实体就是场景级太阳（`Scene::light()` 懒创建时只加了
       Name + DirectionalLight 两个组件，不像 `Scene::createObject` 那样
       无条件带上 Visibility）。用户看到的第一行恰好就是 `DirectionalLight`
       → "灯的可见性开关不见了"。修法 = 每一行都画 👁（缺组件 = 可见），
       点它时由 flushPending 补组件；同时 `Scene::light()` 也补上组件，
       并让 `Renderer` 认 `scene.sunVisible()`（太阳不是可渲染物，强度归零
       即"不发光"）。

判据（都不写死精确色值，只看"暖色像素"—— R−B 明显为正）：
    锁：locked → `IM_COL32(250,200,90)`（R−B=160）；未锁且悬停 → 文字色
        (219,222,227)（R−B=−8）；未锁未悬停 → 灰 alpha90。
    眼：hidden → `IM_COL32(200,90,80)`（R−B=120）；visible → 文字色。
    选中高亮是蓝的 (41,135,240)（R−B=−199）→ 不会误判成"暖色"。

断言（默认第 0 行 = `DirectionalLight`，出厂**未锁且可见**；不依赖初始态，
只看"翻一下再翻回来"）：
  A1 行尾两格都在行矩形内，且 👁 在 🔒 左边（先提交顺序 → 布局顺序）。
  A2-1 点 🔒 → 锁图标暖色状态**翻转**（证明点击送达且生效）。
  A2-2 再点 🔒 → 翻回原状态。
  A3-1 点 👁 → 眼图标暖色状态**翻转**（闭眼 + 斜杠）。
  A3-2 再点 👁 → 翻回原状态。

⚠ 否定测试（必须做，否则这条断言只是在描述"现在的样子"）：
    把 editor/SceneHierarchy.cpp 里 `ImGuiTreeNodeFlags_AllowOverlap` 去掉
    （回到 bug 版本）重新编译 → **A2-1 必须 FAIL**（点了不反应）。

用法：
    python tools/verify_hierarchy_status.py
"""

import argparse
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_client, find_window, user32        # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors                # noqa: E402
from verify_asset_drag import force_foreground                        # noqa: E402
from verify_bvh import click_client, click_client_post                # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify")

# "暖色"判据：R 明显大于 B。锁=橙(250,200,90) / 眼=红(200,90,80)。
# 文字色 (219,222,227) 与面板底 *(27,28,30) 的 R−B 都在 0 附近；
# 选中高亮是蓝的 (41,135,240)，R−B = −199 —— 都不会误判。
HOT_RB = 60
HOT_RMIN = 130

# ⚠ 不能借 `verify_add_menu.last_rect`：它的 RECT_RE 只认 `[A-Za-z-]` 当
#   tag（**不含空格和数字**），所以 `HIER-LOCK row0=(...)` 这种带 `row0`
#   的 tag 会**静默**返回 None（转储里明明有那一行）。这里自己解析。
RECT_ANY_RE = r"=\((-?[\d.]+),(-?[\d.]+)\)-\((-?[\d.]+),(-?[\d.]+)\)"


def rect_of(lines, tag):
    pat = re.compile(re.escape(tag) + RECT_ANY_RE)
    for line in reversed(lines):
        m = pat.search(line)
        if m:
            return tuple(float(m.group(i)) for i in range(1, 5))
    return None


def wait_rect_of(tap, tag, timeout=8.0):
    end = time.time() + timeout
    r = None
    while time.time() < end:
        with tap.lock:
            lines = list(tap.lines)
        r = rect_of(lines, tag)
        if r:
            return r
        time.sleep(0.2)
    return None


def warm_pixels(img, box):
    """box=(x0,y0,x1,y1) 内"暖色"像素个数（橙/红图标线的证据）。"""
    x0, y0, x1, y1 = [int(round(v)) for v in box]
    W, H = img.size
    x0, y0 = max(x0, 0), max(y0, 0)
    x1, y1 = min(x1, W), min(y1, H)
    if x1 <= x0 or y1 <= y0:
        return -1
    px = img.load()
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            p = px[x, y]
            r, g, b = p[0], p[1], p[2]
            if r - b > HOT_RB and r > HOT_RMIN:
                n += 1
    return n


def center(rect):
    return ((rect[0] + rect[2]) * 0.5, (rect[1] + rect[3]) * 0.5)


def inside(inner, outer):
    return (inner[0] >= outer[0] - 1 and inner[1] >= outer[1] - 1 and
            inner[2] <= outer[2] + 1 and inner[3] <= outer[3] + 1)


def click_once(hwnd, x, y, first):
    """点一次。第 1 发走真实事件（必须先抢到前台，否则会落到压在上面的窗口
    上 —— 见 tools/verify_particles.py 里"返回值被忽略"那段教训）；
    之后一律 `PostMessage` 直投，与前台/遮挡无关。返回走的哪条路。"""
    if first and force_foreground(hwnd):
        click_client(hwnd, x, y)
        return "real"
    click_client_post(hwnd, x, y)
    return "post"


def click_until(hwnd, box, want_warm, tries=3, settle=1.1):
    """点 box 的中心，直到 warm_pixels 的"有/无"符合 want_warm。

    重试是安全的：判据不符 ⇒ 那一发**没送达**（真送达了状态必然翻转，
    1.1s 后必然拍得到），所以重试不会"把状态翻回去"。
    返回 (实得像素数, 点了几次, 路径)。"""
    cx, cy = center(box)
    got, path = -1, "none"
    for k in range(tries):
        path = click_once(hwnd, cx, cy, first=(k == 0))
        time.sleep(settle)
        img, _ = capture_client(hwnd)
        got = warm_pixels(img, box)
        if (got > 0) == want_warm:
            return got, k + 1, path
    return got, tries, path


def toggle_probe(hwnd, tap, box, tag, name, shots, check, skip):
    """点两次：第一次状态翻转、第二次翻回来。返回 (初始, 翻转后, 复原后)。

    ⚠ 第 2 发（"翻回来"）只有在第 1 发成功翻转后才有意义 —— 否则"状态本来
    就没变"会白送一个 PASS（否定测试里就出现过：A2-1 FAIL 但 A2-2 PASS）。
    所以第 1 发没翻动时，第 2 发记 **SKIP** 而不是 PASS。"""
    img, _ = capture_client(hwnd)
    img.save(os.path.join(OUT_DIR, shots[0]))
    n0 = warm_pixels(img, box)
    init_warm = n0 > 0

    n1, t1, p1 = click_until(hwnd, box, want_warm=not init_warm)
    img, _ = capture_client(hwnd)
    img.save(os.path.join(OUT_DIR, shots[1]))
    flipped = (n1 > 0) != init_warm
    check("%s-1 点一下 → 状态翻转" % tag, flipped,
          "init_warm=%s → warm=%d tries=%d via=%s" % (init_warm, n1, t1, p1))

    if not flipped:
        skip("%s-2 再点一下 → 翻回原状态" % tag,
             "第 1 发没翻动 → 这一发无信息量（状态没变也会 PASS）")
        return init_warm, n1, n0

    n2, t2, p2 = click_until(hwnd, box, want_warm=init_warm)
    img, _ = capture_client(hwnd)
    img.save(os.path.join(OUT_DIR, shots[2]))
    check("%s-2 再点一下 → 翻回原状态" % tag, (n2 > 0) == init_warm,
          "warm=%d tries=%d via=%s" % (n2, t2, p2))
    return init_warm, n1, n2


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--keep", action="store_true", help="跑完不关窗口（调试）")
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    if not os.path.exists(exe):
        print("找不到可执行文件:", exe)
        return 2
    os.makedirs(OUT_DIR, exist_ok=True)

    kill_stale_editors()

    env = dict(os.environ)
    # ini 关掉 → 面板布局走默认（level 工作区把 Hierarchy 打开）
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_LOG_RECTS"] = "1"
    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    print("launched pid", proc.pid)

    fails = []
    skipped = []

    def check(name, ok, detail=""):
        print("  [%s] %s %s" % ("PASS" if ok else "FAIL", name, detail))
        if not ok:
            fails.append(name)

    def skip(name, detail=""):
        print("  [SKIP] %s %s" % (name, detail))
        skipped.append(name)

    try:
        hwnd = find_window("Venn Editor")
        if not hwnd:
            print("未找到窗口")
            return 2
        user32.ShowWindow(hwnd, 9)
        time.sleep(2.0)
        # 摆到 (0,0)：不给位置的话每次启动右移 38px（窗口级联），坐标会飘。
        user32.SetWindowPos(hwnd, 0, 0, 0, 1600, 900, 0x0002 | 0x0004)
        time.sleep(2.5)

        row = wait_rect_of(tap, "HIER-RECT row0", 8.0)
        lock = wait_rect_of(tap, "HIER-LOCK row0", 6.0)
        eye = wait_rect_of(tap, "HIER-EYE row0", 6.0)
        if not row:
            print("拿不到 HIER-RECT row0（Hierarchy 没开？）")
            with tap.lock:
                print([l for l in tap.lines if "HIER" in l][-8:])
            return 3
        if not lock:
            print("拿不到 HIER-LOCK row0（行尾锁定图标没画出来）")
            return 3
        print("row0=%s lock=%s eye=%s" % (row, lock, eye))

        # 布局：🔒 占最右一格，👁 在它左边一格（与 UE Outliner 一致）。
        check("A1 行尾两格都在行矩形内（眼在锁左边）",
              eye is not None and inside(lock, row) and inside(eye, row) and
              eye[2] <= lock[0] + 1,
              "lock=%s eye=%s row=%s" % (lock, eye, row))

        toggle_probe(hwnd, tap, lock, "A2", "lock",
                     ("hier_lock_before.png", "hier_lock_after.png",
                      "hier_lock_back.png"), check, skip)

        toggle_probe(hwnd, tap, eye, "A3", "eye",
                     ("hier_eye_before.png", "hier_eye_after.png",
                      "hier_eye_back.png"), check, skip)

        print("截图 →", OUT_DIR)
    finally:
        if args.keep:
            print("--keep：留窗口 pid", proc.pid)
        else:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()

    tail = ("（另有 %d 条 SKIP：%s）" % (len(skipped), ", ".join(skipped))
            if skipped else "")
    if fails:
        print("结果: FAIL: " + ", ".join(fails) + tail)
        return 1
    print("结果: PASS" + tail)
    return 0


if __name__ == "__main__":
    sys.exit(main())
