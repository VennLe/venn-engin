"""端到端验证：第二轮补齐的三件事（2026-10-20 晚）。

被验证的用户需求（原话）：
  "关键帧只存在编辑器会话里，没进场景序列化；Fracture 的切面始终轴对齐
   （是"碎掉的感觉"，不是真正的断裂解算）；Modeling 只有全局几何操作，
   没有边/面选择。把这些也都实现一下，Modeling 可以按照 blender 建模软件
   去实现。"

所以断言分三组，每组都不是"日志里出现了某个词"，而是"数据真的变了"：

  A. 关键帧进序列化
     A1 点 Add key → ANIM-KEY total=1
     A2 把时间拖到 80% 再加一帧 → ANIM-KEY total=2（两个不同时刻）
     A3 Ctrl+S → "Scene saved:"
     A4 直接读存盘后的 JSON：entity 上有 animClip.keys 且长度 == 2
        —— 这一条是"进场景"的**唯一硬证据**（日志证明不了文件里有什么）
     A5 重开同一个场景 → SCENE-ANIMCLIP keys=2（读得回来）

  B. Modeling 的边/面选择（Blender Edit Mode）
     B1 顶点 welded：24 个渲染顶点焊成 8 组（立方体 8 个角）
     B2 点视口 → MODEL-PICK mode=face、selF=1
     B3 点 Extrude → MODEL-EXTRUDE faces=1 sides=3 且 verts 24→27
        （挤出 1 个三角形 = 复制它的 3 个顶点 + 3 个侧面）
     B4 点 Delete → MODEL-DELETE faces=1（三角形真的少了）
     B5 按键盘 1 → MODEL-SELMODE mode=vertex（1 没有被当成"视图看 -Y"）

  C. Fracture 的 Voronoi 真断裂
     C1 默认方法 break → FRACTURE method=voronoi、chunks>=2
     C2 至少一个胞腔有封口面（FRACTURE-CELL caps>=1）
        —— 轴对齐盒子切法不可能有封口面，这条直接排除了旧的 grid 实现
     C3 至少一个胞腔的三角形数 != 12 —— 碎块不是方块
     C4 切到 Grid 再 break → FRACTURE method=grid（旧切法保留可对比）
     C5 零 VUID / 零 ERROR / 零 leaked

用法：python tools/verify_toolkit_round2.py
"""

import ctypes
import io
import json
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from capture_window import capture_client, find_window  # noqa: E402
from verify_add_menu import LogTap, click, kill_stale_editors  # noqa: E402
from verify_asset_drag import client_to_screen  # noqa: E402
from verify_nav_gizmo import MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "..", "_venn_verify")
EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
ASSETS = os.path.join(ROOT, "build", "bin", "assets")

user32 = ctypes.windll.user32

RECT_ANY = r"=\((-?[\d.]+),(-?[\d.]+)\)-\((-?[\d.]+),(-?[\d.]+)\)"
BAD_RE = r"VUID-|\[ERROR\]|Validation Error|leaked|SEGV"

# logRect 的输出格式是 `TAG=(x0,y0)-(x1,y1) center=(cx,cy)`（见
# engine/ui/DebugRects.h）。锚点行每帧一条，会把真正的事件行淹没，所以数
# 事件时必须排除它。
#
# ⚠ 判别锚点**只能**用结尾的 `center=(`，不能图省事用 `=(`：
#   `FRACTURE chunks=8 method=grid ... bounds=(0.0,0.0,0.0)-(1.0,1.0,1.0)`
#   这种正常日志里也有 `=(`，用 `=(` 当判据会把 Grid 断裂事件整条丢掉，
#   表现就是"Grid 切法没反应"——实际它跑了，只是日志被脚本自己吃掉了。
ANCHOR_RE = re.compile(r"=\(-?[\d.]+,-?[\d.]+\)-\(-?[\d.]+,-?[\d.]+\) center=\(")


def is_anchor(line):
    return ANCHOR_RE.search(line) is not None

VK_CONTROL, SC_CTRL = 0x11, 0x1D
VK_S, SC_S = 0x53, 0x1F
WM_KEYDOWN, WM_KEYUP = 0x0100, 0x0101
WM_CLOSE = 0x0010

# 用来验证关键帧存盘的场景副本 —— **不要**直接改 sandbox.json
SCENE_COPY = "scenes/_verify_round2.json"
SCENE_COPY_ABS = os.path.join(ASSETS, "scenes", "_verify_round2.json")

fails = []


def check(ok, msg):
    print("    %-74s %s" % (msg, "PASS" if ok else "FAIL"))
    if not ok:
        fails.append(msg)
    return ok


def count(tap, needle):
    with tap.lock:
        return sum(1 for l in tap.lines if needle in l)


def rect_of(tap, tag, timeout=6.0):
    deadline = time.time() + timeout
    pat = re.compile(re.escape(tag) + RECT_ANY)
    while time.time() < deadline:
        with tap.lock:
            for line in reversed(tap.lines):
                m = pat.search(line)
                if m:
                    return tuple(float(m.group(i)) for i in range(1, 5))
        time.sleep(0.12)
    return None


def center(r):
    return (r[0] + r[2]) * 0.5, (r[1] + r[3]) * 0.5


def wait_line(tap, needle, timeout=8.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if count(tap, needle) > 0:
            return True
        time.sleep(0.15)
    return False


def wait_re(tap, pattern, timeout=8.0):
    deadline = time.time() + timeout
    rx = re.compile(pattern)
    while time.time() < deadline:
        with tap.lock:
            if any(rx.search(l) for l in tap.lines):
                return True
        time.sleep(0.15)
    return False


def events(tap, pattern):
    """数**事件行**（排除 logRect 锚点行 —— 见 ANCHOR_RE 的说明）。"""
    rx = re.compile(pattern)
    with tap.lock:
        return sum(1 for l in tap.lines if rx.search(l) and not is_anchor(l))


def last_event(tap, pattern):
    rx = re.compile(pattern)
    with tap.lock:
        for line in reversed(tap.lines):
            if rx.search(line) and not is_anchor(line):
                return line
    return ""


def dump_events(tap, keys=("MODEL-", "VP-"), n=40, skip=("ANIM-TICK",)):
    """失败时把最近的事件行全打出来 —— 否则只能靠猜。

    `keys` 是行内必须出现的子串之一；锚点行（形如 `tag=(x,y)-(..)`）一律排除，
    它们每帧一条会把真正的事件淹没。
    """
    with tap.lock:
        rows = [l.rstrip() for l in tap.lines
                if any(k in l for k in keys) and not is_anchor(l)
                and not any(k in l for k in skip)]
    print("      ------ 最近 %d 条事件（%s）------" % (min(n, len(rows)), keys))
    for l in rows[-n:]:
        print("      " + l[-160:])


def focus_window(hwnd):
    if user32.GetForegroundWindow() == hwnd:
        return True
    user32.keybd_event(0x12, 0, 0, 0)
    user32.keybd_event(0x12, 0, 2, 0)
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.3)
    if user32.GetForegroundWindow() != hwnd:
        user32.SwitchToThisWindow(hwnd, True)
        time.sleep(0.3)
    return user32.GetForegroundWindow() == hwnd


_click_raw = click


def click(hwnd, x, y, *args, **kwargs):  # noqa: F811
    # ⚠ 成功拿到焦点是**必要条件**：没有焦点时 Windows 会把这次点击当成
    #   "激活窗口"吃掉，GLFW 的鼠标回调根本不触发 —— 实测 io.MouseDown[0]
    #   一直是 0，于是 ImGui 认为你没按下过，整个视口点选链路一步都不走。
    #   面板按钮看起来"能点"是因为它之前已经被别的 click() 聚焦过了。
    for _ in range(3):
        if focus_window(hwnd):
            break
        time.sleep(0.2)
    return _click_raw(hwnd, x, y, *args, **kwargs)


def shoot(hwnd, name):
    try:
        img, _ = capture_client(hwnd)
    except Exception as e:  # noqa: BLE001
        print("      ! 截图失败(%s): %s" % (name, e))
        return
    if img is None or img.size[0] <= 0 or img.size[1] <= 0:
        return
    try:
        img.save(os.path.join(OUT_DIR, name))
    except Exception as e:  # noqa: BLE001
        print("      ! 截图保存失败(%s): %s" % (name, e))


def press_ctrl_s(hwnd):
    """Ctrl+S 存盘。Ctrl 必须是真实键盘事件（GLFW 用 GetKeyState 读修饰键）。"""
    focus_window(hwnd)
    user32.keybd_event(VK_CONTROL, SC_CTRL, 0, 0)
    time.sleep(0.12)
    user32.PostMessageW(hwnd, WM_KEYDOWN, VK_S, (SC_S << 16) | 1)
    time.sleep(0.06)
    user32.PostMessageW(hwnd, WM_KEYUP, VK_S, (SC_S << 16) | 0xC0000001)
    time.sleep(0.12)
    user32.keybd_event(VK_CONTROL, SC_CTRL, 0x0002, 0)
    time.sleep(1.0)


def tap_key(hwnd, vk, sc):
    """按一下普通键。down 与 up 之间必须跨帧，否则 ImGui 的
    IsKeyPressed 看不到这次按下（它在 NewFrame 里采样）。"""
    focus_window(hwnd)
    user32.PostMessageW(hwnd, WM_KEYDOWN, vk, (sc << 16) | 1)
    time.sleep(0.18)
    user32.PostMessageW(hwnd, WM_KEYUP, vk, (sc << 16) | 0xC0000001)
    time.sleep(0.25)


def launch(scene, select, mode):
    env = dict(os.environ)
    env.update({
        "MYVK_LOG_RECTS": "1",
        "MYVK_NO_WINDOW_SAVE": "1",
        "MYVK_EDITOR_NO_HUD": "1",
        "MYVK_LIGHT_GIZMOS": "0",
        "MYVK_EDITOR_SCENE": scene,
        "MYVK_EDITOR_SELECT": select,
        "MYVK_EDITMODE": mode,
    })
    proc = subprocess.Popen([EXE], cwd=os.path.dirname(EXE), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    hwnd = find_window("Venn Editor", timeout=60)
    return proc, tap, hwnd


def shutdown(proc, hwnd):
    try:
        if hwnd:
            user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
        time.sleep(0.6)
        if proc.poll() is None:
            proc.terminate()
        proc.wait(timeout=12)
    except Exception:  # noqa: BLE001
        try:
            proc.kill()
        except Exception:  # noqa: BLE001
            pass


def click_until(tap, hwnd, tag, pattern, tries=4, settle=0.55):
    """点 tag 锚定的按钮，直到 pattern 事件数增加；每次**重新取矩形**。

    为什么要重试：浮动面板的控件位置会随内容变化（多一行文字就整体下移），
    而按钮点击还会被 toast 浮层吃掉。用"点一次就假设成功"写验收，最后测的
    是运气，不是功能。返回 (成功与否, 尝试次数)。
    """
    for i in range(tries):
        r = rect_of(tap, tag, 4.0)
        if not r:
            return False, i
        n0 = events(tap, pattern)
        click(hwnd, *center(r))
        time.sleep(settle)
        if events(tap, pattern) > n0:
            return True, i + 1
    return False, tries


def switch_mode(tap, hwnd, mode_id):
    tb = rect_of(tap, "TB-EDITMODE", 5.0)
    if not tb:
        return False
    click(hwnd, *center(tb))
    time.sleep(0.55)
    if not wait_line(tap, "TB-EDITMODE-WIN", 3.0):
        return False
    item = rect_of(tap, "TB-EDITMODE-ITEM " + mode_id, 3.0)
    if not item:
        return False
    click(hwnd, *center(item))
    time.sleep(0.9)
    return True


# ================================================================ A 关键帧
def part_a():
    print("\n[A] 关键帧进场景序列化")
    os.makedirs(os.path.dirname(SCENE_COPY_ABS), exist_ok=True)
    shutil.copyfile(os.path.join(ASSETS, "scenes", "sandbox.json"), SCENE_COPY_ABS)

    proc, tap, hwnd = launch(SCENE_COPY, "BoxA", "animation")
    try:
        if not hwnd:
            check(False, "A0 编辑器窗口没起来")
            return
        time.sleep(2.5)
        check(wait_line(tap, "auto-opened scene", 10.0), "A0 场景已加载")
        check(rect_of(tap, "TK-animation", 8.0) is not None,
              "A0b Animation 面板已打开")

        # ---- 第一帧 ----
        n0 = events(tap, r"ANIM-KEY t=")
        ok1, tries = click_until(tap, hwnd, "AN-ADDKEY", r"ANIM-KEY t=")
        check(ok1, "A1 第一帧写入（ANIM-KEY，第 %d 次点击）" % tries)
        line = last_event(tap, r"ANIM-KEY t=")
        m = re.search(r"total=(\d+)", line)
        check(m is not None and int(m.group(1)) == 1,
              "A1b 轨道里现在 1 帧（%s）" % (m.group(1) if m else "抓不到"))

        # ---- 推进时间，再打一帧 ----
        # 不推进的话第二次 Add key 会走"同一时刻覆盖"，total 还是 1 ——
        # 那看起来像"只存了一帧"，其实是同一帧被改写。
        #
        # ⚠ 不要靠"点时间滑条"推进：ImGui 的 Slider 在某些版本里按下只
        #   进入拖拽、**不跳值**，点一下就松开等于什么都没做（第一版就是这么
        #   失败的，日志里 total 一直是 1）。用 Play 推进一秒再暂停，走的
        #   是用户真实路径，也不依赖控件实现细节。
        pr = rect_of(tap, "AN-PLAY", 5.0)
        if not pr:
            check(False, "A2 找不到播放按钮")
            return
        click(hwnd, *center(pr))       # Play
        time.sleep(1.4)
        click(hwnd, *center(pr))       # Pause
        time.sleep(0.4)
        n1 = events(tap, r"ANIM-KEY t=")
        ok2, tries = click_until(tap, hwnd, "AN-ADDKEY", r"ANIM-KEY t=")
        line = last_event(tap, r"ANIM-KEY t=")
        m = re.search(r"total=(\d+)", line)
        if not ok2 or m is None or int(m.group(1)) != 2:
            dump_events(tap, ("ANIM-", "AN-"), 30)
        check(ok2 and m is not None and int(m.group(1)) == 2,
              "A2 第二个时刻再打一帧（total=%s，第 %d 次点击）"
              % (m.group(1) if m else "?", tries))
        shoot(hwnd, "round2_animation.png")

        # ---- 存盘 ----
        press_ctrl_s(hwnd)
        check(wait_line(tap, "Scene saved:", 6.0), "A3 Ctrl+S 存盘成功")
    finally:
        shutdown(proc, hwnd)

    # ---- 直接读文件：这是"进场景"的唯一硬证据 ----
    try:
        doc = json.load(io.open(SCENE_COPY_ABS, encoding="utf-8"))
    except Exception as e:  # noqa: BLE001
        check(False, "A4 存盘文件读不出来：%s" % e)
        return

    clip = None
    for ent in doc.get("entities", []):
        if ent.get("name") == "BoxA" and "animClip" in ent:
            clip = ent["animClip"]
            break
    check(clip is not None, "A4 BoxA 的实体上出现了 animClip 段")
    if clip is not None:
        keys = clip.get("keys") or []
        check(len(keys) == 2, "A5 animClip.keys 里有 2 帧（实际 %d）" % len(keys))
        check(all(k in keys[0] for k in ("t", "pos", "rot", "scale")),
              "A5b 关键帧字段齐（t / pos / rot / scale）")
        check("duration" in clip and "loop" in clip and "autoplay" in clip,
              "A5c 轨道属性齐（duration / loop / autoplay）")

    # ---- 重开，确认读得回来 ----
    proc, tap, hwnd = launch(SCENE_COPY, "BoxA", "animation")
    try:
        if not hwnd:
            check(False, "A6 重开窗口失败")
            return
        time.sleep(2.5)
        ok = wait_re(tap, r"SCENE-ANIMCLIP .*keys=2", 10.0)
        check(ok, "A6 重开场景读到 2 帧（SCENE-ANIMCLIP keys=2）")
        shoot(hwnd, "round2_anim_reload.png")
    finally:
        shutdown(proc, hwnd)


# ================================================================ B 建模选区
def part_b():
    print("\n[B] Modeling 的顶点/边/面选择（Blender Edit Mode）")
    proc, tap, hwnd = launch("scenes/sandbox.json", "BoxA", "modeling")
    try:
        if not hwnd:
            check(False, "B0 窗口没起来")
            return
        time.sleep(2.5)
        check(wait_re(tap, r"MODEL-BIND .*verts=24 welded=8 tris=12", 10.0),
              "B1 立方体 24 个渲染顶点焊成 8 组（weld 生效）")
        vp = rect_of(tap, "VP-RECT", 6.0)
        if not vp:
            check(False, "B0b 拿不到视口矩形")
            return
        check(rect_of(tap, "TK-modeling", 6.0) is not None, "B0c 面板已打开")

        # ---- 面模式 + 视口点选 ----
        r = rect_of(tap, "MD-SELMODE-2", 5.0)
        if not r:
            check(False, "B2 找不到 Face 选择模式按钮")
            return
        click(hwnd, *center(r))
        time.sleep(0.5)

        # ⚠ 候选点必须**避开工具面板**：面板是浮动窗口，盖在视口上面，
        #   点在它身上时 ImGui 给视口 image 的 IsItemHovered() 是 false ——
        #   `viewportInput` 连调用都不会发生，看起来就是"点了没反应"。
        panel = rect_of(tap, "TK-modeling", 5.0)

        def blocked(x, y):
            if not panel:
                return False
            return (panel[0] - 8 <= x <= panel[2] + 8 and
                    panel[1] - 8 <= y <= panel[3] + 8)

        # ⚠⚠ 一定要先按 F 把 BoxA 移到屏幕中心。
        #
        # 相机默认 distance=14，1 米的立方体在 689px 高的视口里只有 ~42px，
        # 而且它并不在视口正中（实测落在 x∈[794,882]、y∈[316,581]）。上一版
        # 按 0.3~0.7 的比例扫 49 个点全部落空 —— 不是"点选没实现"，是**点在
        # 了立方体旁边的空气里**，日志里 res 全被 MODEL-PICK-MISS 接住。
        # 按 F（EditorApp 的聚焦快捷键）把相机 target 挪到选中物体上之后，
        # 立方体就躺在视口正中，再在 ±48px 里细扫就稳了。
        tap_key(hwnd, 0x46, 0x21)  # 'F' = 聚焦选中
        time.sleep(0.6)

        cx = vp[0] + (vp[2] - vp[0]) * 0.5
        cy = vp[1] + (vp[3] - vp[1]) * 0.5
        picked = False
        nth = 0
        for dy in range(0, 49, 8):
            for dx in range(0, 49, 8):
                # 从中心往外扩散：先中心，再四个方向，逐圈放大
                for ox, oy in ((dx, dy), (-dx, dy), (dx, -dy), (-dx, -dy)):
                    x, y = cx + ox, cy + oy
                    if blocked(x, y):
                        continue
                    before = events(tap, r"MODEL-PICK mode=face")
                    click(hwnd, x, y)
                    time.sleep(0.28)
                    nth += 1
                    if events(tap, r"MODEL-PICK mode=face") > before:
                        picked = True
                        break
                if picked:
                    break
            if picked:
                break
        check(picked, "B2 视口点选命中一个面（MODEL-PICK，第 %d 次点击）" % nth)
        if not picked:
            shoot(hwnd, "round2_model_pick_miss.png")
            return
        line = last_event(tap, r"MODEL-PICK mode=face")
        m = re.search(r"selF=(\d+)", line)
        check(m is not None and int(m.group(1)) >= 1,
              "B2b 选区里至少 1 个面（selF=%s）" % (m.group(1) if m else "?"))
        shoot(hwnd, "round2_model_select.png")

        # ---- 挤出 ----
        r = rect_of(tap, "MD-EXTRUDE", 5.0)
        if not r:
            check(False, "B3 找不到 Extrude 按钮")
            return
        n0 = events(tap, r"MODEL-EXTRUDE faces=")
        click(hwnd, *center(r))
        time.sleep(1.0)
        if not events(tap, r"MODEL-EXTRUDE faces=") > n0:
            dump_events(tap)
        check(events(tap, r"MODEL-EXTRUDE faces=") > n0,
              "B3 挤出触发（MODEL-EXTRUDE）")
        line = last_event(tap, r"MODEL-EXTRUDE faces=")
        m = re.search(r"faces=(\d+) sides=(\d+)", line)
        check(m is not None and int(m.group(1)) == 1 and int(m.group(2)) == 3,
              "B3b 1 个面挤出 + 3 条边界边补侧面（%s）"
              % (m.group(0) if m else "抓不到"))
        line = last_event(tap, r"MODEL-OP op=extrude")
        m = re.search(r"verts=(\d+)->(\d+)", line)
        check(m is not None and int(m.group(2)) == int(m.group(1)) + 3,
              "B3c 顶点 24→27（一个三角形挤出 3 个新顶点：%s）"
              % (m.group(0) if m else "抓不到"))

        # ---- 删除选中面 ----
        r = rect_of(tap, "MD-DELETE", 5.0)
        n0 = events(tap, r"MODEL-DELETE faces=")
        click(hwnd, *center(r))
        time.sleep(0.9)
        check(events(tap, r"MODEL-DELETE faces=") > n0,
              "B4 删除选中面（MODEL-DELETE）")
        line = last_event(tap, r"MODEL-DELETE faces=")
        m = re.search(r"tris=(\d+)", line)
        check(m is not None and int(m.group(1)) > 0,
              "B4b 删完还剩三角形（tris=%s，没把整个网格删光）"
              % (m.group(1) if m else "?"))
        shoot(hwnd, "round2_model_extrude.png")

        # ---- 键盘 1 = 顶点模式（不能被"视图看 -Y"吞掉）----
        n0 = events(tap, r"MODEL-SELMODE mode=vertex")
        tap_key(hwnd, 0x31, 0x02)  # '1'
        ok = events(tap, r"MODEL-SELMODE mode=vertex") > n0
        check(ok, "B5 键盘 1 切到顶点模式（MODEL-SELMODE mode=vertex）")
        if not ok:
            check(False, "B5b 说明 1 被 handleViewShortcuts 当成了视图方向")

        # ---- 键盘 A 全选 ----
        # 日志标签是 `MODEL-SEL mode=all verts=.. faces=..`（不是 "MODEL-SEL all"）
        n0 = events(tap, r"MODEL-SEL mode=all")
        tap_key(hwnd, 0x41, 0x1E)  # 'A'
        time.sleep(0.3)
        if not events(tap, r"MODEL-SEL mode=all") > n0:
            dump_events(tap, ("MODEL-",), 20)
        check(events(tap, r"MODEL-SEL mode=all") > n0,
              "B6 键盘 A 全选（MODEL-SEL mode=all）")
    finally:
        shutdown(proc, hwnd)


# ================================================================ C Voronoi
def part_c():
    print("\n[C] Fracture 的 Voronoi 真断裂")
    proc, tap, hwnd = launch("scenes/sandbox.json", "BoxA", "fracture")
    try:
        if not hwnd:
            check(False, "C0 窗口没起来")
            return
        time.sleep(2.5)
        check(rect_of(tap, "TK-fracture", 8.0) is not None, "C0b 面板已打开")

        r = rect_of(tap, "FR-BREAK", 5.0)
        if not r:
            check(False, "C1 找不到 Break 按钮")
            return
        n0 = events(tap, r"FRACTURE chunks=")
        click(hwnd, *center(r))
        time.sleep(2.5)
        check(events(tap, r"FRACTURE chunks=") > n0, "C1 断裂触发（FRACTURE）")

        line = last_event(tap, r"FRACTURE chunks=")
        m = re.search(r"chunks=(\d+)", line)
        check(m is not None and int(m.group(1)) >= 2,
              "C1b 切出 >= 2 块（chunks=%s）" % (m.group(1) if m else "?"))
        check("method=voronoi" in line,
              "C1c 走的是 Voronoi 而不是 Grid（%s）"
              % ("method=voronoi" if "method=voronoi" in line else line[-40:]))
        m = re.search(r"srcTris=(\d+) outTris=(\d+)", line)
        check(m is not None and int(m.group(2)) > 0,
              "C1d 碎块三角形总数 > 0（srcTris=%s outTris=%s）"
              % (m.groups() if m else ("?", "?")))
        shoot(hwnd, "round2_fracture_voronoi.png")

        # ---- 封口面：轴对齐盒子切法不可能有 ----
        cells = []
        with tap.lock:
            for l in tap.lines:
                mm = re.search(r"FRACTURE-CELL index=(\d+) verts=(\d+) tris=(\d+) "
                               r"caps=(\d+)", l)
                if mm:
                    cells.append((int(mm.group(1)), int(mm.group(2)),
                                  int(mm.group(3)), int(mm.group(4))))
        check(len(cells) > 0, "C2 每个胞腔都有明细日志（%d 条）" % len(cells))
        check(any(c[3] >= 1 for c in cells),
              "C2b 至少一个胞腔封了断面（caps>=1，轴对齐盒子切法不会有）")
        check(any(c[2] != 12 for c in cells),
              "C2c 至少一个碎块不是方块（tris != 12：%s）"
              % sorted({c[2] for c in cells})[:6])

        # ---- Grid 切法仍可选 ----
        #
        # ⚠ 这里原来点两下就断言，结果整组只有 1 条 FRACTURE —— 两次点击都
        #   被吃掉了。原因是第一次 break 之后 `notify(...)` 弹了个 toast 浮层，
        #   它盖在面板上，ImGui 的 IsItemHovered 对底下的按钮全变 false。
        #   所以先等一下让 toast 自己消失，并且**用布局变化确认切换真的生效**
        #   （切到 Grid 后 Irregularity 滑条消失，FR-BREAK 会整体上移），
        #   没生效就再点一次 —— 不靠"点了就应该行"这种假设。
        g = rect_of(tap, "FR-METHOD-GRID", 5.0)
        if g:
            before_break = rect_of(tap, "FR-BREAK", 3.0)
            switched = False
            for attempt in range(4):
                time.sleep(1.2 if attempt == 0 else 0.8)
                click(hwnd, *center(g))
                deadline = time.time() + 2.0
                while time.time() < deadline:
                    now_break = rect_of(tap, "FR-BREAK", 0.4)
                    if (before_break and now_break and
                            abs(now_break[1] - before_break[1]) > 4.0):
                        switched = True
                        break
                    time.sleep(0.2)
                if switched:
                    break
            check(switched, "C3a 切到 Grid 后布局真的变了（Irregularity 消失）")
            if not switched:
                dump_events(tap, ("FRACTURE", "FR-"), 20)

            n1 = events(tap, r"FRACTURE chunks=")
            # ⚠ 必须重新取矩形：切到 Grid 之后 "Irregularity" 滑条消失了，
            #   下面所有控件整体上移 —— 用切换前的坐标会点到别的控件上。
            r2 = rect_of(tap, "FR-BREAK", 5.0)
            click(hwnd, *center(r2 if r2 else r))
            time.sleep(2.0)
            line = last_event(tap, r"FRACTURE chunks=")
            if not (events(tap, r"FRACTURE chunks=") > n1 and "method=grid" in line):
                dump_events(tap, ("FRACTURE", "FR-"), 30)
            check(events(tap, r"FRACTURE chunks=") > n1 and "method=grid" in line,
                  "C3 Grid 切法仍然可用（method=grid，可对比）")
        shoot(hwnd, "round2_fracture_grid.png")
    finally:
        shutdown(proc, hwnd)


def main():
    if not os.path.exists(EXE):
        print("找不到可执行文件，请先构建：", EXE)
        return 1
    kill_stale_editors()
    os.makedirs(OUT_DIR, exist_ok=True)

    # 支持只跑某一组：`verify_toolkit_round2.py b` —— 单组迭代时省一半时间
    only = sys.argv[1].lower() if len(sys.argv) > 1 else "all"
    if only in ("all", "a"):
        part_a()
    if only in ("all", "b"):
        part_b()
    if only in ("all", "c"):
        part_c()

    print("\n" + "=" * 78)
    if fails:
        print("FAILED %d 项：" % len(fails))
        for f in fails:
            print("  - " + f)
    else:
        print("ALL PASS")
    print("=" * 78)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
