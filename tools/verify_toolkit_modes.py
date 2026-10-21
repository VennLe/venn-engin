"""端到端验证：编辑模式下拉里其余 6 个模式真的能用（2026-10-20）。

被验证的用户需求（原话）：
  "模式下拉列表中的其他模式和功能也需要一一实现完整，具体方案可以参考
   UE5 中的思路去实现"

所以断言不是"面板弹出来了"，而是**每个模式的核心动作都改了场景 / 资产**：

  A. MeshPaint  点 Reset all colors → MP-CLEAR（顶点色被写回并重新上传）
  B. Modeling   点 Subdivide       → MODEL-OP op=subdivide（顶点数变多）
  C. Animation  点 Add key         → ANIM-KEY（时间轴记下一帧）
  D. PCG        点 Generate        → PCG-GEN placed>0（真的撒出了实体）
  E. BrushEdit  选 Sphere + 视口点击 → BRUSH-PLACE shape=sphere（放在 z=0 平面）
  F. Fracture   点 Fracture        → FRACTURE chunks>0（切成碎块）
     —— 放最后：它会把源物体隐藏掉，跑完再切别的模式就看不到 BoxA 了。
  G. 统一检查：切模式 → 新面板 `TK-<id>` 出现、`EDITMODE-PANELS ... open=1`、
     上一个模式的面板自动关掉（不叠加）
  H. 日志健康：零 VUID / 零 ERROR

前置：场景用 assets/scenes/sandbox.json（里面有 Ground / BoxA / BoxB / BoxC），
启动时用 MYVK_EDITOR_SELECT=BoxA 选中 BoxA —— 六个模式里有四个都要"选中项
有网格"才肯干活。

用法：python tools/verify_toolkit_modes.py
"""

import ctypes
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from capture_window import capture_client, find_window  # noqa: E402
from verify_add_menu import LogTap, click, kill_stale_editors  # noqa: E402
from verify_asset_drag import client_to_screen  # noqa: E402
from verify_nav_gizmo import MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP  # noqa: E402

MOUSEEVENTF_WHEEL = 0x0800
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "..", "_venn_verify")
EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")

user32 = ctypes.windll.user32

RECT_ANY = r"=\((-?[\d.]+),(-?[\d.]+)\)-\((-?[\d.]+),(-?[\d.]+)\)"
BAD_RE = r"VUID-|\[ERROR\]|Validation Error|leaked|SEGV"

# (EditMode 的 id, 面板表 id, 面板标题里的稳定 id)
# ⚠ 两个 id 不一样：EditMode::MeshPaint 的 `editModeId` 是 "mesh_paint"，
#   而面板表的 id 是 "meshpaint"。下拉锚点用前者，`TK-` 锚点用后者。
MODES = [
    ("mesh_paint", "meshpaint", "Mesh Paint"),
    ("modeling",   "modeling",   "Modeling"),
    ("animation",  "animation",  "Animation"),
    ("pcg",        "pcg",        "PCG"),
    ("brush_edit", "brushedit",  "Brush Editing"),
    ("fracture",   "fracture",   "Fracture"),
]

fails = []


def check(ok, msg):
    print("    %-72s %s" % (msg, "PASS" if ok else "FAIL"))
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
    """等某条日志匹配正则。**不要用子串**判 `EDITMODE-PANELS` ——
    那行中间还夹着 `landscape=0 foliage=0`，子串匹配一定失败。"""
    deadline = time.time() + timeout
    rx = re.compile(pattern)
    while time.time() < deadline:
        with tap.lock:
            if any(rx.search(l) for l in tap.lines):
                return True
        time.sleep(0.15)
    return False


def stopped(tap, tag, dwell=1.6):
    """面板是不是**停止绘制**了（= 关掉了）。

    ⚠ 不能用 `rect_of(...) is None`：logRect 是**只增不减**的日志，
    面板关掉之后旧行仍然在历史里 —— 于是"面板已关"这个断言永远失败，
    看起来像"面板关不掉"。真正的判据是"这一帧还在刷新吗"：锚点数量
    停住不动了，就说明 beginToolkitWindow 已经不再走到 logRect。
    """
    n0 = count(tap, tag)
    time.sleep(dwell)
    return count(tap, tag) == n0


def focus_window(hwnd):
    if user32.GetForegroundWindow() == hwnd:
        return
    user32.keybd_event(0x12, 0, 0, 0)
    user32.keybd_event(0x12, 0, 2, 0)
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.3)
    if user32.GetForegroundWindow() != hwnd:
        user32.SwitchToThisWindow(hwnd, True)
        time.sleep(0.3)


_click_raw = click


def click(hwnd, x, y, *args, **kwargs):
    focus_window(hwnd)
    return _click_raw(hwnd, x, y, *args, **kwargs)


def shoot(hwnd, name):
    try:
        img, _ = capture_client(hwnd)
    except Exception as e:                      # noqa: BLE001
        print("      ! 截图失败(%s): %s" % (name, e))
        return
    if img is None or img.size[0] <= 0 or img.size[1] <= 0:
        print("      ! 截图为空，跳过: %s" % name)
        return
    try:
        img.save(os.path.join(OUT_DIR, name))
    except Exception as e:                      # noqa: BLE001
        print("      ! 截图保存失败(%s): %s" % (name, e))


def switch_mode(tap, hwnd, mode_id):
    """走真实用户路径：点**左栏模式栏**那一行。

    ⚠ 2026-10-21：顶栏那颗下拉删掉了（用户说它和左栏列表重复），所以这里
      少一步"先点开弹层"—— 9 行一直可见，直接点行即可。
    """
    item = rect_of(tap, "MODE-RAIL-ITEM " + mode_id, 5.0)
    if not item:
        return False
    click(hwnd, *center(item))
    time.sleep(0.9)
    return True


def events(tap, pattern):
    """数**事件行**（不是 logRect 锚点行）。

    ⚠ 锚点和事件共用同一个名字（`logRect("PCG-GEN")` 与
      `VK_LOG_INFO("PCG-GEN kind=...")`）。锚点行每帧一条、形如
      `PCG-GEN=(x,y)-(x,y)`，所以 `count(tap, "PCG-GEN")` 永远在涨 ——
      用它做断言等于什么都没断言。这里只数**不匹配锚点格式**的行。
    """
    rx = re.compile(pattern)
    rect_like = re.compile(r"%s=\(" % re.escape(pattern.strip("^$")))
    with tap.lock:
        return sum(1 for l in tap.lines
                   if rx.search(l) and not rect_like.search(l))


def wheel(hwnd, cx, cy, notches=-2):
    """在客户区 (cx,cy) 处滚轮。notches 为负 = 向下滚（内容上移）。"""
    focus_window(hwnd)
    user32.SetCursorPos(*client_to_screen(hwnd, cx, cy))
    time.sleep(0.2)
    for _ in range(abs(notches)):
        user32.mouse_event(MOUSEEVENTF_WHEEL, 0, 0,
                           120 if notches > 0 else -120, 0)
        time.sleep(0.1)
    time.sleep(0.35)


def inside(r, body):
    """锚点中心是否落在参数区窗口内 —— 也就是"用户这一下点得到"。"""
    cx, cy = center(r)
    return (body[0] + 2 <= cx <= body[2] - 2) and \
           (body[1] + 2 <= cy <= body[3] - 2)


def reveal(tap, hwnd, tag, body_tag, tries=8):
    """把左栏参数区滚到 `tag` 可见，再返回它的矩形。

    为什么要滚：2026-10-21 把模式面板从浮动窗口挪进左栏下半之后，参数区
    只有 ~400px 高，而 Modeling / Fracture 这些面板的默认尺寸是 460×660、
    620 —— 内容天然比窗口长（面板自带滚动条，真实用户就是滚轮往下翻）。
    而 logRect 报的是**未裁剪**的 item 矩形，按钮滚出窗口后中心点在窗口
    外面，照着它点等于点在视口上 —— 表现为"点了没反应"，看起来像功能坏了。
    这里补的就是用户那一下滚轮。
    """
    for _ in range(tries):
        r = rect_of(tap, tag, 4.0)
        body = rect_of(tap, body_tag, 4.0)
        if not r or not body:
            return r
        if inside(r, body):
            return r
        down = center(r)[1] > body[3]
        cx = (body[0] + body[2]) * 0.5
        cy = (body[1] + body[3]) * 0.5
        wheel(hwnd, cx, cy, -2 if down else 2)
        new = rect_of(tap, tag, 2.0)
        if new and abs(center(new)[1] - center(r)[1]) < 1.0:
            break          # 滚不动了：已经到头，再滚也没用
    return rect_of(tap, tag, 3.0)


def act(tap, hwnd, tag, label, pattern, timeout=8.0, body_tag=None):
    """点一个 logRect 锚点上的按钮，断言它打出了匹配 `pattern` 的事件行。

    返回 (点到了, 日志出现)。
    """
    r = reveal(tap, hwnd, tag, body_tag or "TK-" + tag)
    if not r:
        check(False, "%s：找不到按钮锚点 %s" % (label, tag))
        return False, False
    before = events(tap, pattern)
    click(hwnd, *center(r))
    deadline = time.time() + timeout
    while time.time() < deadline:
        if events(tap, pattern) > before:
            return True, True
        time.sleep(0.15)
    return True, False


def main():
    if not os.path.exists(EXE):
        print("找不到可执行文件，请先构建：", EXE)
        return 1

    kill_stale_editors()
    os.makedirs(OUT_DIR, exist_ok=True)

    env = dict(os.environ)
    env.update({
        "MYVK_LOG_RECTS": "1",
        "MYVK_NO_WINDOW_SAVE": "1",
        "MYVK_EDITOR_NO_HUD": "1",
        "MYVK_LIGHT_GIZMOS": "0",
        # 六个模式里有四个要"选中项有网格"。sandbox 场景里 BoxA 是标准方盒，
        # 细分/破裂/上色都适用；不指定场景就是一份空文档，那些面板全都会
        # 停在"Select an entity first"上 —— 看起来像功能没实现。
        "MYVK_EDITOR_SCENE": "scenes/sandbox.json",
        "MYVK_EDITOR_SELECT": "BoxA",
        "MYVK_EDITMODE": "select",
    })
    proc = subprocess.Popen([EXE], cwd=os.path.dirname(EXE), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    try:
        hwnd = find_window("Venn Editor", timeout=60)
        if not hwnd:
            print("找不到编辑器窗口")
            return 1
        time.sleep(2.5)

        print("\n[前置] 场景与选中项")
        check(wait_line(tap, "auto-opened scene", 10.0),
              "P1 sandbox 场景已加载")
        check(count(tap, "auto-selected 'BoxA'") > 0, "P2 BoxA 已选中")
        vp = rect_of(tap, "VP-RECT", 6.0)
        if not vp:
            check(False, "拿不到视口矩形（VP-RECT）")
            return 1

        # ================================================== A MeshPaint
        print("\n[A] Mesh Paint（顶点色）")
        check(switch_mode(tap, hwnd, "mesh_paint"), "A0 切到 Mesh Paint 模式")
        check(wait_re(tap, r"EDITMODE-PANELS mode=mesh_paint\b.*panel=meshpaint open=1",
                        6.0), "A1 面板打开（open=1）")
        check(rect_of(tap, "TK-meshpaint", 5.0) is not None,
              "A2 面板矩形已上报（TK-meshpaint）")
        hit, logged = act(tap, hwnd, "MP-RESET", "A3", r"MP-CLEAR verts=", body_tag="TK-meshpaint")
        check(logged, "A3 顶点色重置并重新上传（MP-CLEAR）")
        shoot(hwnd, "toolkit_meshpaint.png")

        # ================================================== B Modeling
        print("\n[B] Modeling（几何操作）")
        check(switch_mode(tap, hwnd, "modeling"), "B0 切到 Modeling 模式")
        check(wait_re(tap, r"EDITMODE-PANELS mode=modeling\b.*panel=modeling open=1",
                        6.0), "B1 面板打开（open=1）+ MeshPaint 面板已关")
        check(rect_of(tap, "TK-modeling", 5.0) is not None,
              "B2 面板矩形已上报（TK-modeling）")
        check(stopped(tap, "TK-meshpaint"),
              "B3 上一个模式的面板不再绘制（不叠加）")
        before = None
        with tap.lock:
            hits = [l for l in tap.lines if "MODEL-OP op=subdivide" in l]
        hit, logged = act(tap, hwnd, "MD-SUBDIV", "B4", r"MODEL-OP op=subdivide",
                body_tag="TK-modeling")
        check(logged, "B4 细分生效（MODEL-OP op=subdivide）")
        if logged:
            with tap.lock:
                line = [l for l in tap.lines if "MODEL-OP op=subdivide" in l][-1]
            m = re.search(r"verts=(\d+)->(\d+)", line)
            if m:
                check(int(m.group(2)) > int(m.group(1)),
                      "B5 顶点数真的变多（%s -> %s）" % (m.group(1), m.group(2)))
            else:
                check(False, "B5 MODEL-OP 里抓不到 verts=a->b")
        shoot(hwnd, "toolkit_modeling.png")

        # ================================================== C Animation
        print("\n[C] Animation（transform 关键帧）")
        check(switch_mode(tap, hwnd, "animation"), "C0 切到 Animation 模式")
        check(wait_re(tap, r"EDITMODE-PANELS mode=animation\b.*panel=animation open=1",
                        6.0), "C1 面板打开（open=1）")
        check(rect_of(tap, "TK-animation", 5.0) is not None,
              "C2 面板矩形已上报（TK-animation）")
        hit, logged = act(tap, hwnd, "AN-ADDKEY", "C3", r"ANIM-KEY t=", body_tag="TK-animation")
        check(logged, "C3 关键帧记录成功（ANIM-KEY）")
        shoot(hwnd, "toolkit_animation.png")

        # ================================================== D PCG
        print("\n[D] PCG（程序化撒点）")
        check(switch_mode(tap, hwnd, "pcg"), "D0 切到 PCG 模式")
        check(wait_re(tap, r"EDITMODE-PANELS mode=pcg\b.*panel=pcg open=1", 6.0),
              "D1 面板打开（open=1）")
        check(rect_of(tap, "TK-pcg", 5.0) is not None,
              "D2 面板矩形已上报（TK-pcg）")
        hit, logged = act(tap, hwnd, "PCG-GEN", "D3", r"PCG-GEN kind=", timeout=40.0,
                body_tag="TK-pcg")
        check(logged, "D3 撒点成功（PCG-GEN）")
        if logged:
            with tap.lock:
                line = [l for l in tap.lines if "PCG-GEN" in l][-1]
            m = re.search(r"placed=(\d+)", line)
            check(m is not None and int(m.group(1)) > 0,
                  "D4 placed > 0（%s）" % (m.group(1) if m else "抓不到"))
            # 材质指针必须非空：空材质在颜色通道被 `continue` 掉，实体就在
            # 层级里、屏幕上一个都看不见（这条是"看得见"的唯一可断言证据）。
            mm = re.search(r"mat=(\S+)", line)
            check(mm is not None and mm.group(1) != "-",
                  "D6 实例拿到了非空材质（mat=%s）"
                  % (mm.group(1) if mm else "抓不到"))
        check(events(tap, r"PCG-CLEAR") > 0,
              "D5 重新生成前先清理旧输出（PCG-CLEAR）")
        shoot(hwnd, "toolkit_pcg.png")

        # ================================================== E BrushEdit
        print("\n[E] Brush Editing（盒体拼装）")
        check(switch_mode(tap, hwnd, "brush_edit"), "E0 切到 Brush Editing 模式")
        check(wait_re(tap, r"EDITMODE-PANELS mode=brush_edit\b.*panel=brushedit open=1",
                        6.0), "E1 面板打开（open=1）")
        check(rect_of(tap, "TK-brushedit", 5.0) is not None,
              "E2 面板矩形已上报（TK-brushedit）")
        sphere = reveal(tap, hwnd, "BR-SHAPE-2", "TK-brushedit")
        if sphere:
            click(hwnd, *center(sphere))
            time.sleep(0.4)
        # 视口里点一下 → 在 z=0 平面上落一个 brush
        px = vp[0] + (vp[2] - vp[0]) * 0.16
        py = vp[1] + (vp[3] - vp[1]) * 0.72
        before = events(tap, r"BRUSH-PLACE shape=")
        click(hwnd, px, py)
        deadline = time.time() + 8.0
        while time.time() < deadline and events(tap, r"BRUSH-PLACE shape=") <= before:
            time.sleep(0.15)
        ok = events(tap, r"BRUSH-PLACE shape=") > before
        check(ok, "E3 视口点击落下一个 brush（BRUSH-PLACE）")
        if ok:
            with tap.lock:
                line = [l for l in tap.lines if "BRUSH-PLACE" in l][-1]
            check("shape=sphere" in line,
                  "E4 用的是面板里选的 Sphere（%s）"
                  % (re.search(r"shape=(\w+)", line).group(1)
                     if re.search(r"shape=(\w+)", line) else "抓不到"))
        shoot(hwnd, "toolkit_brushedit.png")

        # ================================================== F Fracture
        print("\n[F] Fracture（切碎 + 刚体）")
        check(switch_mode(tap, hwnd, "fracture"), "F0 切到 Fracture 模式")
        check(wait_re(tap, r"EDITMODE-PANELS mode=fracture\b.*panel=fracture open=1",
                        6.0), "F1 面板打开（open=1）")
        check(rect_of(tap, "TK-fracture", 5.0) is not None,
              "F2 面板矩形已上报（TK-fracture）")
        hit, logged = act(tap, hwnd, "FR-BREAK", "F3", r"FRACTURE chunks=", timeout=20.0,
                body_tag="TK-fracture")
        check(logged, "F3 切碎成功（FRACTURE chunks=）")
        if logged:
            with tap.lock:
                line = [l for l in tap.lines if "FRACTURE chunks=" in l][-1]
            m = re.search(r"chunks=(\d+)", line)
            check(m is not None and int(m.group(1)) > 0,
                  "F4 chunks > 0（%s）" % (m.group(1) if m else "抓不到"))
        shoot(hwnd, "toolkit_fracture.png")

        # ================================================== G 回 Select
        print("\n[G] 回到选择模式")
        check(switch_mode(tap, hwnd, "select"), "G0 切回 Select")
        check(wait_line(tap, "EDITMODE-PANELS mode=select", 5.0), "G1 模式已切回")
        check(stopped(tap, "TK-fracture"), "G2 Fracture 面板已关掉")

        # ================================================== H 日志健康
        print("\n[H] 日志健康")
        with tap.lock:
            bad = [l for l in tap.lines if re.search(BAD_RE, l)]
        check(not bad, "H1 无 Vulkan 验证层 / ERROR（%d 条）" % len(bad))
        for l in bad[:5]:
            print("      !", l)

        print("\n截图：%s" % OUT_DIR)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=20)
        except Exception:                       # noqa: BLE001
            proc.kill()

    print("\n==== %s ====" % ("ALL PASS" if not fails else "FAILED %d" % len(fails)))
    for f in fails:
        print("  -", f)
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
