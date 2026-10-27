"""端到端验证：多选编辑（选择集 / Ctrl·Shift 点选 / 框选 / 批量操作 /
混合值）。对应任务 #316。

背景（需求原文）：
    "多选（selection() 返回单个 Entity）：多选选择集、框选 / 批量移动/旋转/
     缩放、批量改属性（混合值显示）/ 批量复制/删除"

五组判据，分工是**刻意的**：

  A 组（无窗口）`Editor.exe` + `MYVK_MULTISELECT_TEST=1`，25 条 `MULTI-TEST`：
    单选入口的零破坏、集合语义（Ctrl 增删 / Shift 范围 / 框选替换 / 全选
    跳过上锁）、支点、多目标撤销、以及 **批量数学**（增量 / 比例 / 绕支点
    旋转）的数值。数值语义全在这里 —— 它们错了的话界面看起来完全正常。

  B 组（GUI）**层级树里的真 Ctrl / Shift 点击**：修饰键 → 调哪个 API 的
    分派只有真鼠标能验（这条 glue 最容易写错，而写错了"看起来只是没反应"）。
    同一次启动里顺带验 Inspector 的混合值聚合（`INSP-MULTI ... pos=mixed`）。

  C 组（GUI）**框选**：真的在视口里拉一个框。判据是两条互相咬住的：
    框住全场景 → 选中全部；框住天空的一小块 → 一个都不选。只有前者的话，
    "框选 = 全选" 这种实现也会绿。

  D 组（GUI）**多选拖 gizmo**：真鼠标拖 X 轴。判据是"两个物体各走了**一样
    的位移**"（不是"都动了"）—— 只动主选中项、或子物体被父节点带着又自己
    写一遍（动两次），这两种错法都会被这一条抓出来。松开后按一次 Ctrl+Z，
    `MULTI-UNDO n=2` 必须出现且两个都回到原位（一条记录撤销整批）。

  E 组（GUI）**批量复制 / 删除**：Ctrl+D 复制 3 个 → `HIER-DUP n=3`；
    Del 删掉全部 → `HIER-DEL n=N`，存盘后场景里一个不剩。

总共 39 条（A 11 / B 9 / C 5 / D 9 / E 5）。

⚠ 这条用例有一只脚踩在"真鼠标"上，所以天生怕被抢前台：`mouse_event` 打给
  光标底下**最上层**的窗口，不是我们手里的 hwnd。整套五个组要跑两分多钟，
  期间任何东西把前台拿走（顺手切一下窗口、系统弹条通知），后续的层级点击
  就会**一条 `HIER-SELECT` 都不出现** —— 与"多选坏了"长得一模一样。
  第一版就撞上了：整套红 7 条，同样几个组单独跑全绿。
  现在 `click` / `drag` / `press_key` 每次发事件前都确认前台（被抢就抢回来，
  并打一条 `WARN`），`row_map` 与各条点击都用"等日志真的涨一行"的
  `click_expect`（还允许重试一次 —— 窗口刚激活时系统会吞掉第一次点击）。
  **跑这条时别碰鼠标。**

用法：
    python tools/verify_multiselect.py          # 全套
    python tools/verify_multiselect.py D         # 只跑 D 组（调试用）
"""

import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import launcher_kit as kit                                        # noqa: E402
from capture_window import find_window, user32                    # noqa: E402
from verify_asset_drag import force_foreground, move_to           # noqa: E402

MOUSE_LDOWN = 0x0002      # MOUSEEVENTF_LEFTDOWN
MOUSE_LUP = 0x0004        # MOUSEEVENTF_LEFTUP
WM_KEYDOWN = 0x0100
WM_KEYUP = 0x0101
VK_CONTROL = 0x11
SC_CTRL = 0x1D
VK_SHIFT = 0x10
SC_SHIFT = 0x2A
VK_D = 0x44
SC_D = 0x20
VK_S = 0x53
SC_S = 0x1F
VK_DELETE = 0x2E
SC_DELETE = 0x53
VK_Z = 0x5A
SC_Z = 0x2C

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
EDITOR = os.path.join(BIN, "Editor.exe")
OUT = os.path.join(os.path.dirname(ROOT), "_venn_verify", "g19")

FAILS = []


def check(name, cond, detail=""):
    kit.log("CHECK %s %s%s" % (name, "OK" if cond else "FAIL",
                               ("  " + str(detail)) if detail != "" else ""))
    if not cond:
        FAILS.append(name)


def wait_count(tap, needle, n, timeout=25.0):
    """等日志里 needle 出现次数 >= n（不能用 has_line —— 它对"以前出现过"
    永远为真）。"""
    end = time.time() + timeout
    while time.time() < end:
        if tap.count_line(needle) >= n:
            return True
        time.sleep(0.1)
    return False


def last_match(tap, rx):
    found = None
    with tap.lock:
        for line in tap.lines:
            m = rx.search(line)
            if m:
                found = m
    return found


# ------------------------------------------------------------------ 夹具

def cube(name, pos, scale=(1.0, 1.0, 1.0), parent=-1):
    return {
        "name": name,
        "transform": {"position": list(pos),
                      "rotation": [0.0, 0.0, 0.0],
                      "scale": list(scale)},
        "parent": parent,
        "visible": True,
        "mesh": {"kind": "builtin", "shape": "cube", "size": 1.0,
                 "name": "multi_test_" + name.replace(" ", "_")},
        "material": {"kind": "procedural", "name": "multi_test_mat",
                     "baseColor": [0.85, 0.35, 0.30, 1.0],
                     "metallic": 0.0, "roughness": 0.55,
                     "normalScale": 0.0, "ao": 1.0,
                     "emissive": [0, 0, 0],
                     "doubleSided": False, "alphaBlend": False},
    }


def write_json(path, obj):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(obj, f, indent=2, ensure_ascii=False)
    return path


def scene_four(path, z=0.5):
    """四个**平铺**立方体（无父子）。

    为什么必须平铺（D 组特别依赖）：gizmo 的目标表会跳过"祖先也在选中集里"
    的实体（否则子物体会被父节点带着走一遍、再自己走一遍）。平铺场景里没有
    这层关系，于是"两个都动了且位移相同"只可能来自多目标批量写入。

    位置沿 +Y 排开，间距 2m —— 相邻两两不重叠，框选的分界才有意义。
    """
    return write_json(path, {
        "version": 1,
        "generator": "verify_multiselect",
        "camera": {"target": [0.0, 3.0, z], "yaw": 1.5707963, "pitch": 0.0,
                   "distance": 16.0},
        "entities": [
            cube("CubeA", (0.0, 0.0, z)),
            cube("CubeB", (0.0, 2.0, z)),
            cube("CubeC", (0.0, 4.0, z)),
            cube("CubeD", (0.0, 6.0, z)),
        ],
    })


def load_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def find_entity(scene, name):
    for e in scene.get("entities", []):
        if e.get("name") == name:
            return e
    return None


# ------------------------------------------------------------------ A 组

def run_selftest():
    kit.log("---- A 组：MYVK_MULTISELECT_TEST ----")
    env = kit.clean_env({"MYVK_MULTISELECT_TEST": "1"})
    proc = subprocess.run([EDITOR], cwd=BIN, env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, encoding="utf-8", errors="replace",
                          timeout=180)
    out = proc.stdout or ""
    ok_lines = out.count("result=PASS")
    fail_lines = out.count("result=FAIL")
    check("A1 selftest 退出码 0", proc.returncode == 0, "rc=%d" % proc.returncode)
    check("A2 25 条全过", ok_lines == 25 and fail_lines == 0,
          "ok=%d fail=%d" % (ok_lines, fail_lines))
    m = re.search(r"MULTI-TOTAL pass=(\d+) fail=(\d+)", out)
    check("A3 汇总行 fail=0", m is not None and m.group(2) == "0",
          m.group(0) if m else "没有 MULTI-TOTAL 行")
    # 负数用例必须真的存在（否则"什么都通过"也会全绿）：混合语义与
    # 绕支点旋转是两块最容易写错、写错了也照样"能跑"的地方。
    for want in ("batch_add_mixed", "batch_mul_mixed", "batch_mul_zero_prior",
                 "orbit_180", "range_a_to_c", "all_skips_locked",
                 "validate_drops_dead", "multi_cmd_undo_all"):
        check("A4 有 %s" % want, want in out)
    if fail_lines:
        for line in out.splitlines():
            if "result=FAIL" in line:
                kit.log("  " + line.strip())


# ------------------------------------------------------------------ 起编辑器

def launch(scene_path, select="", gizmo="", console_cmd=""):
    env = kit.clean_env({
        "MYVK_EDITOR_SCENE": scene_path,
        "MYVK_LOG_RECTS": "1",          # HIER-ROW / GZ-* / VP-RECT 全靠它
        "MYVK_NO_WINDOW_SAVE": "1",     # 自动化判定为真 → layout 不落盘
    })
    if select:
        env["MYVK_EDITOR_SELECT"] = select
    if gizmo:
        env["MYVK_EDITOR_GIZMO"] = gizmo
    if console_cmd:
        env["MYVK_EDITOR_CONSOLE_CMD"] = console_cmd
    proc = subprocess.Popen([EDITOR], cwd=BIN, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = kit.LogTap()
    tap.start(proc)
    hwnd = find_window(kit.EDITOR_TITLE, 45.0)
    return proc, tap, hwnd


# ------------------------------------------------------------------ 鼠标 / 键盘

def ensure_foreground(hwnd):
    """每次发事件前都确认窗口还在前台；不在就抢回来并记一条 WARN。

    ⚠ 为什么不能只在每组开头抢一次：`click()` / `drag()` 走的是**真实**的
    SetCursorPos + mouse_event，事件打给"光标底下最上层的窗口"，不是我们手里
    的 hwnd。这五个组要跑两分多钟，期间只要有任何东西把前台抢走（用户顺手
    切了一下窗口、系统弹了条通知），**后面每一次点选都会一条 `HIER-SELECT`
    都不出现** —— 现象与"多选坏了"逐字相同，但根因在测试机被抢了焦点。
    实测就是一例：整套跑红 7 条，同样这几个组单独跑全绿。
    """
    if user32.GetForegroundWindow() == hwnd:
        return True
    ok = force_foreground(hwnd)
    kit.log("  WARN 前台被抢走，抢回来：%s" % ("成功" if ok else "失败"))
    return ok


def click_expect(tap, hwnd, xy, needle, mods=(), timeout=6.0, tries=2):
    """点一下并等 `needle` 出现次数 +1；没出现就**再点一次**。

    为什么允许重试一次：窗口刚从后台被抢回前台时，Windows 会把**第一次**
    点击当成"激活窗口"吞掉（ImGui 一条都收不到，见 launcher_kit.focus_window
    的说明）。那是操作系统的行为，不是产品的行为，不该记成产品失败。
    重试之后仍然没动静就返回 False，交给断言去红 —— 真坏了照样红。
    """
    n = tap.count_line(needle)
    for _ in range(tries):
        click(hwnd, xy, mods)
        if wait_count(tap, needle, n + 1, timeout):
            return True
        kit.log("  WARN 点击没产生 %s，重试一次（焦点被抢？）" % needle)
        ensure_foreground(hwnd)
        time.sleep(0.4)
    return False


def click(hwnd, xy, mods=()):
    """点一下（可带修饰键）。修饰键必须是**真实** keybd_event 事件：
    GLFW 读的是 GetKeyState，ImGui 的 io.KeyCtrl 也源自它 —— PostMessage
    造的假按键消息不会改这个状态。"""
    ensure_foreground(hwnd)
    for vk, sc in mods:
        user32.keybd_event(vk, sc, 0, 0)
        time.sleep(0.10)
    move_to(hwnd, int(xy[0]), int(xy[1]))
    time.sleep(0.20)
    user32.mouse_event(MOUSE_LDOWN, 0, 0, 0, 0)
    time.sleep(0.10)
    user32.mouse_event(MOUSE_LUP, 0, 0, 0, 0)
    time.sleep(0.35)
    for vk, sc in reversed(mods):
        user32.keybd_event(vk, sc, 0x0002, 0)
        time.sleep(0.10)


def drag(hwnd, src, dst, steps=14):
    ensure_foreground(hwnd)
    move_to(hwnd, int(src[0]), int(src[1]))
    time.sleep(0.30)
    user32.mouse_event(MOUSE_LDOWN, 0, 0, 0, 0)
    time.sleep(0.18)
    for k in range(1, steps + 1):
        move_to(hwnd, int(src[0] + (dst[0] - src[0]) * k / steps),
                int(src[1] + (dst[1] - src[1]) * k / steps))
        time.sleep(0.035)
    time.sleep(0.15)
    user32.mouse_event(MOUSE_LUP, 0, 0, 0, 0)
    time.sleep(0.6)


def press_key(hwnd, vk, sc, mods=(), extended=False):
    """发一个真实按键（WM_KEYDOWN/UP）。

    ⚠ `extended` 对 **Delete** 是必须的：它在扫描码表里和"小键盘小数点"
    共用 0x53，GLFW 的 translateKey 对 VK_DELETE 有这一句
        if (!(HIWORD(lParam) & KF_EXTENDED)) return GLFW_KEY_UNKNOWN;
    于是不带扩展位（lParam bit24）的 WM_KEYDOWN 会被当成"未知键"丢掉 ——
    现象是 `IsKeyPressed(ImGuiKey_Delete)` 永远 false，而同一套注入方式的
    Ctrl+D / Ctrl+S 都是好的（它们不是扩展键）。
    """
    ext = 0x01000000 if extended else 0
    ensure_foreground(hwnd)
    for m_vk, m_sc in mods:
        user32.keybd_event(m_vk, m_sc, 0, 0)
        time.sleep(0.12)
    user32.PostMessageW(hwnd, WM_KEYDOWN, vk, ext | (sc << 16) | 1)
    time.sleep(0.08)
    user32.PostMessageW(hwnd, WM_KEYUP, vk, ext | (sc << 16) | 0xC0000001)
    time.sleep(0.12)
    for m_vk, m_sc in reversed(mods):
        user32.keybd_event(m_vk, m_sc, 0x0002, 0)
    time.sleep(0.9)


def row_center(tap, index):
    r = tap.wait_rect("HIER-ROW %d" % index, 20.0)
    if not r:
        return None
    return ((r[0] + r[2]) * 0.5, (r[1] + r[3]) * 0.5)


SELECT_RE = re.compile(r"HIER-SELECT mode=(\S+) id=(\d+) name=(\S+) n=(\d+)")
SAVED_RE = re.compile(r"Scene saved: '([^']+)'")


def sel_n(tap):
    """最后一次 HIER-SELECT 的集合大小；没有则 -1（断言里会显形）。"""
    m = last_match(tap, SELECT_RE)
    return int(m.group(4)) if m else -1


def sel_mode(tap):
    m = last_match(tap, SELECT_RE)
    return m.group(1) if m else "-"


def sel_line(tap):
    m = last_match(tap, SELECT_RE)
    return m.group(0) if m else "-"


def row_map(tap, hwnd, n_rows=8):
    """点一遍前 n_rows 行，返回 {实体名: (行号, 中心点)}。

    为什么不直接假设"场景 JSON 的第 i 个实体 = 树里第 i 行"：场景加载器会
    自己补一个 DirectionalLight（它没有 Transform，但**是树里的一行**），
    行号从此整体偏移一位 —— 这一条真把 B/D 两组都坑过（Shift 范围少一项、
    Ctrl 点到了"本来就选中"的那一个 → n=0）。名字对名字才是稳的。
    """
    out, seen = {}, []
    for i in range(n_rows):
        c = row_center(tap, i)
        if not c:
            break
        # ⚠ 必须确认"这一次点击真的产生了新的 HIER-SELECT"。否则
        # `last_match` 会把**上一行**的名字当成本行的（现象是 row_map 里出现
        # 连续重复的名字，比如 `1:DirectionalLight 2:CubeB 3:CubeB`），
        # 然后整组断言都在点错的东西上，红得完全看不到真因。
        if not click_expect(tap, hwnd, c, "HIER-SELECT"):
            seen.append("%d:?" % i)
            continue
        name = last_match(tap, SELECT_RE).group(3)
        seen.append("%d:%s" % (i, name))
        out.setdefault(name, (i, c))
    # 只打一行摘要：逐行打坐标会把日志淹掉，而行号 ↔ 名字这一层映射
    # 恰恰是排查"点到别人身上"时唯一要看的东西。
    kit.log("  row_map " + " ".join(seen))
    return out


# ------------------------------------------------------------------ B 组

def run_ctrl_shift(work):
    kit.log("---- B 组：层级树 Ctrl / Shift 点选 + Inspector 混合值 ----")
    scene_path = scene_four(os.path.join(work, "scene_b.json"))
    proc, tap, hwnd = launch(scene_path, select="CubeA")
    if not hwnd:
        FAILS.append("B0 编辑器窗口没出现")
        return
    try:
        force_foreground(hwnd)
        time.sleep(1.0)

        # 先把"名字 → 第几行"问出来（见 row_map 的说明：加载器会补太阳，
        # 行号不是场景数组下标）。
        rows = row_map(tap, hwnd)
        check("B1 四个立方体都在树里",
              all(n in rows for n in ("CubeA", "CubeB", "CubeC", "CubeD")),
              str(sorted(rows.keys())))
        if not all(n in rows for n in ("CubeA", "CubeB", "CubeC", "CubeD")):
            return

        # 普通点击 = 单选（集合被替换成一项）
        click_expect(tap, hwnd, rows["CubeA"][1], "HIER-SELECT", timeout=10.0)
        check("B2 普通点击 = single 且 n=1",
              sel_mode(tap) == "single" and sel_n(tap) == 1, sel_line(tap))

        # Ctrl+点第二个 → n=2
        click_expect(tap, hwnd, rows["CubeB"][1], "HIER-SELECT mode=ctrl",
                     mods=[(VK_CONTROL, SC_CTRL)], timeout=10.0)
        check("B3 Ctrl+点 → n=2", sel_n(tap) == 2, sel_line(tap))

        # 再 Ctrl+点第三个 → n=3
        click_expect(tap, hwnd, rows["CubeC"][1], "HIER-SELECT mode=ctrl",
                     mods=[(VK_CONTROL, SC_CTRL)], timeout=10.0)
        check("B4 再 Ctrl+点 → n=3", sel_n(tap) == 3, sel_line(tap))

        # ---- Inspector 混合值：三个立方体的位置各不相同 → pos 必须 mixed ----
        # 就在这儿断言（三个立方体都带 Transform）；等到后面 Shift 选出
        # 太阳之后，"只有一个人带 Transform"会让聚合退化成 same，那是**对的**。
        m = last_match(tap, re.compile(
            r"INSP-MULTI n=(\d+) pos=(\S+) rot=(\S+) scale=(\S+)"))
        check("B5 Inspector 聚合出混合值（pos=mixed rot=same scale=same）",
              m is not None and m.group(2) == "mixed" and m.group(3) == "same"
              and m.group(4) == "same",
              m.group(0) if m else "没有 INSP-MULTI")
        check("B6 聚合时确实是多选（n>=2）",
              m is not None and int(m.group(1)) >= 2,
              m.group(0) if m else "-")

        # 同一个 Ctrl+点 = 取消（不是"再选一次"）
        click_expect(tap, hwnd, rows["CubeC"][1], "HIER-SELECT mode=ctrl",
                     mods=[(VK_CONTROL, SC_CTRL)], timeout=10.0)
        check("B7 同一个 Ctrl+点 = 取消 → n=2", sel_n(tap) == 2, sel_line(tap))

        # Shift+点 CubeD：锚点 = 上次点的 CubeB → 范围 B..D = **3** 项，
        # 而且必须**不含 CubeA**（少一项 / 多一项都会被抓）。
        click_expect(tap, hwnd, rows["CubeD"][1], "HIER-SELECT mode=range",
                     mods=[(VK_SHIFT, SC_SHIFT)], timeout=10.0)
        check("B8 Shift+点 → 范围 3 项",
              sel_n(tap) == 3 and sel_mode(tap) == "range", sel_line(tap))
        m = last_match(tap, SELECT_RE)
        check("B9 范围是 B..D（CubeA 不在里面）",
              m is not None and m.group(3) == "CubeD" and sel_n(tap) == 3,
              sel_line(tap))
    finally:
        kit.shutdown(proc, hwnd)


# ------------------------------------------------------------------ C 组

def viewport_rect(tap):
    r = tap.wait_rect("VP-RECT", 25.0)
    if not r:
        return None
    return r


def run_box_select(work):
    kit.log("---- C 组：视口框选 ----")
    scene_path = scene_four(os.path.join(work, "scene_c.json"))
    proc, tap, hwnd = launch(scene_path)
    if not hwnd:
        FAILS.append("C0 编辑器窗口没出现")
        return
    try:
        force_foreground(hwnd)
        time.sleep(1.2)
        vp = viewport_rect(tap)
        check("C1 拿到视口矩形", vp is not None, str(vp))
        if not vp:
            return
        x0, y0, x1, y1 = vp

        # ① 拉一个**几乎铺满**视口的框：四个立方体都该被选中。
        #    起点选在视口左上角内侧（那里是天空 / 空白，不会先点中物体 ——
        #    框选的起点必须是"点空"）。
        a = (x0 + 24, y0 + 24)
        b = (x1 - 24, y1 - 24)
        drag(hwnd, a, b, steps=16)
        ok = wait_count(tap, "BOX-SELECT", 1, 10.0)
        check("C2 拉框产生了 BOX-SELECT", ok,
              "n=%d" % tap.count_line("BOX-SELECT"))
        m = last_match(tap, re.compile(r"BOX-SELECT n=(\d+) additive=(\d+)"))
        check("C3 框住全场景 → 选中 4 个",
              m is not None and int(m.group(1)) == 4,
              m.group(0) if m else "-")

        # ② 再拉一个**很小**的框在右上角（天空）：一个都不该选中。
        #    没有这一条，"框选 = 全选" 那种实现也会绿。
        a2 = (x1 - 70, y0 + 24)
        b2 = (x1 - 24, y0 + 70)
        drag(hwnd, a2, b2, steps=8)
        ok = wait_count(tap, "BOX-SELECT", 2, 10.0)
        m = last_match(tap, re.compile(r"BOX-SELECT n=(\d+) additive=(\d+)"))
        check("C4 框住空白 → 一个都不选（不是全选）",
              ok and m is not None and int(m.group(1)) == 0,
              m.group(0) if m else "-")

        # ③ 点一下空白（不拖动）= 清空选中，而不是"拉了个 0 大小的框"
        click(hwnd, (x1 - 40, y0 + 40))
        check("C5 点空白 → selection cleared",
              wait_count(tap, "selection cleared", 1, 10.0),
              "cleared=%d" % tap.count_line("selection cleared"))
    finally:
        kit.shutdown(proc, hwnd)


# ------------------------------------------------------------------ D 组

GIZMO_END_RE = re.compile(
    r"gizmo end: handle=(\S+) '([^']*)' pos=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\)")
MULTI_DRAG_RE = re.compile(r"MULTI-DRAG n=(\d+) mode=(\S+) handle=(\S+)")


def run_multi_gizmo(work):
    kit.log("---- D 组：多选拖 gizmo + 一次撤销 ----")
    scene_path = scene_four(os.path.join(work, "scene_d.json"))
    out_json = os.path.join(work, "scene_d_after.json")
    proc, tap, hwnd = launch(scene_path, select="CubeA", gizmo="move")
    if not hwnd:
        FAILS.append("D0 编辑器窗口没出现")
        return
    try:
        force_foreground(hwnd)
        time.sleep(1.0)

        rows = row_map(tap, hwnd)
        if not all(n in rows for n in ("CubeA", "CubeB")):
            check("D1 两行都在树里", False, str(sorted(rows.keys())))
            return

        # 单选 CubeA，再 Ctrl+点 CubeB → 两个
        click_expect(tap, hwnd, rows["CubeA"][1], "HIER-SELECT", timeout=10.0)
        click_expect(tap, hwnd, rows["CubeB"][1], "HIER-SELECT mode=ctrl",
                     mods=[(VK_CONTROL, SC_CTRL)], timeout=10.0)
        check("D1 Ctrl+点后 n=2", sel_n(tap) == 2, sel_line(tap))

        # 手柄此时画在**两个物体包围盒的中心**（不上任何一个物体的位置）。
        # 先等"又打了一条 GZ-ORIGIN"——选中变了手柄就会挪，logRect 会重新打；
        # 直接读 wait_rect 拿到的是**上一帧**的旧坐标（那是单选时的位置）。
        n_org = tap.count_line("GZ-ORIGIN")
        wait_count(tap, "GZ-ORIGIN", n_org + 1, 6.0)
        time.sleep(0.3)
        origin = tap.wait_rect("GZ-ORIGIN", 10.0)
        tip = tap.wait_rect("GZ-AXIS-X", 10.0)
        check("D2 手柄上屏（GZ-ORIGIN + GZ-AXIS-X）",
              origin is not None and tip is not None,
              "origin=%s tip=%s" % (origin, tip))
        if not origin or not tip:
            return
        second = "CubeB"
        ox = (origin[0] + origin[2]) * 0.5
        oy = (origin[1] + origin[3]) * 0.5
        tx = (tip[0] + tip[2]) * 0.5
        ty = (tip[1] + tip[3]) * 0.5
        ux, uy = tx - ox, ty - oy
        ln = max((ux * ux + uy * uy) ** 0.5, 1e-3)
        ux, uy = ux / ln, uy / ln
        # 起点 3/4 处（中心是"自由移动"方块，命中半径 10px，会抓错）
        start = (ox + ux * ln * 0.75, oy + uy * ln * 0.75)
        end = (start[0] + ux * ln * 1.8, start[1] + uy * ln * 1.8)

        n_end = tap.count_line("gizmo end:")
        drag(hwnd, start, end, steps=14)
        check("D3 拖拽结束（gizmo end 出现）",
              wait_count(tap, "gizmo end:", n_end + 1, 10.0),
              "end=%d" % tap.count_line("gizmo end:"))

        m = last_match(tap, MULTI_DRAG_RE)
        check("D4 这次拖拽写了 2 个目标（MULTI-DRAG n=2）",
              m is not None and m.group(1) == "2" and m.group(2) == "Move",
              m.group(0) if m else "没有 MULTI-DRAG")

        # 存盘 → 两个物体应当各走了**一样**的位移
        press_key(hwnd, VK_S, 0x1F, mods=[(VK_CONTROL, SC_CTRL)])
        time.sleep(1.2)

        # 写回哪个文件不靠猜：读 "Scene saved: '<path>'" 那一条。
        ms = last_match(tap, SAVED_RE)
        scene_file = ms.group(1) if ms else scene_path
        if not os.path.exists(scene_file):
            check("D5 存盘后场景文件存在", False, scene_file)
            return
        sc = load_json(scene_file)
        a = find_entity(sc, "CubeA")
        b = find_entity(sc, second)
        ax = float(a["transform"]["position"][0]) if a else None
        bx = float(b["transform"]["position"][0]) if b else None
        check("D5 两个物体都动了（都离开 x=0）",
              ax is not None and bx is not None
              and abs(ax) > 0.05 and abs(bx) > 0.05,
              "a=%s b=%s" % (ax, bx))
        check("D6 位移**相同**（不是只有主选中项动了）",
              ax is not None and bx is not None and abs(ax - bx) < 1e-3,
              "a=%s b=%s" % (ax, bx))
        # 只改了 x：y / z 不该被牵动（"三个分量一起写"是常见错法）
        ay = float(a["transform"]["position"][1])
        by = float(b["transform"]["position"][1])
        check("D7 只动了 x（y 保持原值）",
              abs(ay - 0.0) < 1e-3 and abs(by - 2.0) < 1e-3,
              "ay=%.3f by=%.3f" % (ay, by))

        # ---- 一次撤销回滚整批 ----
        press_key(hwnd, VK_Z, SC_Z, mods=[(VK_CONTROL, SC_CTRL)])
        ok = wait_count(tap, "MULTI-UNDO", 1, 10.0)
        m = last_match(tap, re.compile(r"MULTI-UNDO '([^']*)' n=(\d+)"))
        check("D8 一次 Ctrl+Z 撤销整批（MULTI-UNDO n=2）",
              ok and m is not None and m.group(2) == "2",
              m.group(0) if m else "没有 MULTI-UNDO")

        press_key(hwnd, VK_S, 0x1F, mods=[(VK_CONTROL, SC_CTRL)])
        time.sleep(1.2)
        sc = load_json(scene_file)
        a = find_entity(sc, "CubeA")
        b = find_entity(sc, second)
        ax = float(a["transform"]["position"][0]) if a else None
        bx = float(b["transform"]["position"][0]) if b else None
        check("D9 撤销后两个都回到 x=0",
              ax is not None and bx is not None
              and abs(ax) < 1e-3 and abs(bx) < 1e-3,
              "a=%s b=%s" % (ax, bx))
    finally:
        kit.shutdown(proc, hwnd)


# ------------------------------------------------------------------ E 组

def run_batch_copy_delete(work):
    kit.log("---- E 组：批量复制 / 批量删除 ----")
    scene_path = scene_four(os.path.join(work, "scene_e.json"))
    proc, tap, hwnd = launch(scene_path, select="CubeA")
    if not hwnd:
        FAILS.append("E0 编辑器窗口没出现")
        return
    try:
        force_foreground(hwnd)
        time.sleep(1.0)

        # Ctrl+点两个立方体 → 3 个
        rows = row_map(tap, hwnd)
        if not all(n in rows for n in ("CubeA", "CubeB", "CubeC")):
            check("E1 三行都在树里", False, str(sorted(rows.keys())))
            return
        click_expect(tap, hwnd, rows["CubeA"][1], "HIER-SELECT", timeout=10.0)
        for nm in ("CubeB", "CubeC"):
            click_expect(tap, hwnd, rows[nm][1], "HIER-SELECT mode=ctrl",
                         mods=[(VK_CONTROL, SC_CTRL)], timeout=10.0)
        check("E1 选中 3 个", sel_n(tap) == 3, sel_line(tap))

        # Ctrl+D → 三个副本，且**一次**撤销记录
        press_key(hwnd, VK_D, SC_D, mods=[(VK_CONTROL, SC_CTRL)])
        ok = wait_count(tap, "HIER-DUP", 1, 12.0)
        m = last_match(tap, re.compile(r"HIER-DUP n=(\d+)"))
        check("E2 Ctrl+D 批量复制（HIER-DUP n=3）",
              ok and m is not None and m.group(1) == "3",
              m.group(0) if m else "没有 HIER-DUP")

        press_key(hwnd, VK_S, 0x1F, mods=[(VK_CONTROL, SC_CTRL)])
        time.sleep(1.4)
        sc = load_json(scene_path)
        names = [e.get("name") for e in sc.get("entities", [])]
        copies = [n for n in names if n and "Copy" in n]
        check("E3 场景里多了 3 个副本（真的是 3 个，不是 1 个）",
              len(copies) == 3, str(names))

        # Del → 删掉当前选中（3 个副本）
        press_key(hwnd, VK_DELETE, SC_DELETE, extended=True)
        ok = wait_count(tap, "HIER-DEL", 1, 12.0)
        m = last_match(tap, re.compile(r"HIER-DEL n=(\d+)"))
        check("E4 Del 批量删除（HIER-DEL n=3）",
              ok and m is not None and m.group(1) == "3",
              m.group(0) if m else "没有 HIER-DEL")

        press_key(hwnd, VK_S, 0x1F, mods=[(VK_CONTROL, SC_CTRL)])
        time.sleep(1.4)
        sc = load_json(scene_path)
        names = [e.get("name") for e in sc.get("entities", [])]
        # 只断言"副本没了 + 原来那 4 个还在"，不断言总数 —— 场景加载器可能
        # 自带太阳 / 地面，写死数量会随别处的改动一起红。
        check("E5 副本全没了、原有 4 个还在",
              not any("Copy" in (n or "") for n in names)
              and all(n in names for n in
                      ("CubeA", "CubeB", "CubeC", "CubeD")),
              str(names))
    finally:
        kit.shutdown(proc, hwnd)


# ------------------------------------------------------------------ main

def main():
    work = kit.fresh_dir(OUT)
    kit.log("work dir = %s" % work)

    if not os.path.exists(EDITOR):
        kit.log("找不到 build/bin/Editor.exe —— 先 ./msvc-build.cmd build VennEditor")
        return kit.finish(["exe 不存在"])

    # 只跑某一组（调试用）：`python tools/verify_multiselect.py E`
    only = sys.argv[1].upper() if len(sys.argv) > 1 else ""

    def want(g):
        return not only or only == g

    if want("A"):
        run_selftest()
    if want("B"):
        run_ctrl_shift(work)
    if want("C"):
        run_box_select(work)
    if want("D"):
        run_multi_gizmo(work)
    if want("E"):
        run_batch_copy_delete(work)

    return kit.finish(FAILS)


if __name__ == "__main__":
    sys.exit(main())
