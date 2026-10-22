#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/launcher_kit.py —— 启动器三个验收脚本共用的小工具

为什么单独一个文件：`verify_launcher_{ui,create,open,display,code}` 都要做
同样四件事 —— 起 Launcher.exe、持续排空它的 stdout、按锚点取控件矩形、
点控件、正常关窗。五份各写一遍的话，其中一处修了 bug（比如 `PostMessageW`
的 argtypes）另几处不会跟着修 —— 这个项目已经因为"同一个坑分三处各踩一次"
浪费过很多时间。

⚠ 这是**横切文件**：改它等于同时影响六个用例。`run_regression.py` 的
   `TRIGGERS` 里已经把它挂到那些用例上（见 `LAUNCHER_KIT` 常量）。
"""

import ctypes
import ctypes.wintypes as wt
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(REPO, "build", "bin")
LAUNCHER = os.path.join(BIN, "Launcher.exe")
EDITOR = os.path.join(BIN, "Editor.exe")
GAME = os.path.join(BIN, "Game.exe")
TEMPLATES = os.path.join(REPO, "templates")
WORK_ROOT = os.path.join(os.path.dirname(REPO), "_venn_verify")

TITLE = "Venn Launcher"
EDITOR_TITLE = "Venn Editor"
WM_CLOSE = 0x0010

sys.path.insert(0, os.path.join(REPO, "tools"))
from capture_window import find_window  # noqa: E402

# ⚠⚠ ctypes 默认把参数当 **32 位 int**：64 位进程的 HWND 高 32 位会被截断，
#   消息发到一个不存在的窗口，PostMessageW **静默返回 0** —— 现象是"发了
#   关窗消息，进程照常跑"。必须显式声明 argtypes（本项目已踩过）。
_user32 = ctypes.windll.user32
_user32.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint,
                                 ctypes.c_size_t, ctypes.c_ssize_t]
_user32.PostMessageW.restype = ctypes.c_bool
_user32.IsWindow.argtypes = [ctypes.c_void_p]
_user32.IsWindow.restype = ctypes.c_bool

# `TAG=(x0,y0)-(x1,y1) center=(cx,cy)`
# ⚠ tag 的字符类**必须含数字**：`LAUNCH-RECENT-0` 这种带下标的锚点用
#   `[A-Za-z-]+` 解析不出来，`last_rect` 会静默返回 None（记忆里踩过）。
# ⚠ 坐标必须**逐个**捕获（不能整段 `\(-?\d+,-?\d+\)-…` 抓成一个组再转
#   float —— 那会拿到 "(10,8)-(1090,54)" 这种串，`float()` 直接抛
#   ValueError。第一次写就是这么错的）。
RECT_RE = re.compile(
    r"([A-Za-z0-9_\-]+)=\((-?\d+),(-?\d+)\)-\((-?\d+),(-?\d+)\)"
    r" center=\((-?\d+),(-?\d+)\)")


def log(msg):
    print(msg, flush=True)


class LogTap:
    """后台线程持续读 stdout。

    ⚠⚠ **必须**装着。`Popen(stdout=PIPE)` 而主线程不读的话，子进程日志写满
    管道缓冲（Windows 上 64 KB）就会阻塞在 write()，主循环整个停住 ——
    现象是"窗口一直不出现、日志停在某行中间、进程还活着"，与"启动崩了"
    长得一模一样。
    """

    def __init__(self):
        self.lines = []
        self.lock = threading.Lock()

    def start(self, proc):
        threading.Thread(target=self._pump, args=(proc,), daemon=True).start()

    def _pump(self, proc):
        for raw in proc.stdout:
            with self.lock:
                self.lines.append(raw.rstrip("\r\n"))

    def last_rect(self, tag):
        """返回 (x0, y0, x1, y1)；找不到返回 None。取**最后一条** = 最新坐标。"""
        rx = re.compile(r"\b" + re.escape(tag) + r"=\(")
        with self.lock:
            found = None
            for line in self.lines:
                if rx.search(line):
                    m = RECT_RE.search(line)
                    if m:
                        found = tuple(float(m.group(i)) for i in range(2, 6))
            return found

    def wait_rect(self, tag, timeout=20.0):
        end = time.time() + timeout
        while time.time() < end:
            r = self.last_rect(tag)
            if r:
                return r
            time.sleep(0.1)
        return None

    def has_line(self, needle):
        with self.lock:
            return any(needle in l for l in self.lines)

    def count_line(self, needle):
        with self.lock:
            return sum(1 for l in self.lines if needle in l)

    def wait_line(self, needle, timeout=20.0):
        end = time.time() + timeout
        while time.time() < end:
            if self.has_line(needle):
                return True
            time.sleep(0.1)
        return False

    def text(self):
        with self.lock:
            return "\n".join(self.lines)


def clean_env(extra=None):
    """干净的子进程环境：先剔除所有可能从父 shell 漏进来的自动化变量。"""
    env = dict(os.environ)
    for k in list(env):
        if k.startswith("MYVK_"):
            del env[k]
    if extra:
        env.update(extra)
    return env


def launch(recent_path=None, env_extra=None, cwd=None, timeout=45.0):
    """起 Launcher.exe，返回 (proc, tap, hwnd)。失败时 hwnd = 0。"""
    extra = {"MYVK_LOG_RECTS": "1"}
    if recent_path:
        extra["MYVK_LAUNCHER_RECENT"] = recent_path
    if env_extra:
        extra.update(env_extra)

    proc = subprocess.Popen(
        [LAUNCHER], cwd=cwd or BIN, env=clean_env(extra),
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, encoding="utf-8", errors="replace", bufsize=1)
    tap = LogTap()
    tap.start(proc)

    # 自己写等待循环：子进程崩了就立刻返回真实退出码，不要干等满 timeout
    # 再 kill（那时只能看到 "没有锚点"，与"UI 没画出来"长得一模一样）。
    hwnd = 0
    end = time.time() + timeout
    while time.time() < end:
        if proc.poll() is not None:
            log("WARN 启动器在窗口出现前就退出了 rc=%d" % proc.returncode)
            return proc, tap, 0
        hwnd = find_window(TITLE, timeout=0.5)
        if hwnd:
            break
    if hwnd:
        # 窗口一出现就置前 + 等一下：`Application` 是"先渲染到内存、再露面"，
        # 露面的那一帧控件还在结算布局。不置前的话第一次 `click()` 会撞上
        # "只激活窗口"的老毛病（见 focus_window）。
        focus_window(hwnd)
        time.sleep(0.6)
    return proc, tap, hwnd


def close_window(hwnd):
    if not hwnd or not _user32.IsWindow(ctypes.c_void_p(hwnd)):
        return False
    return bool(_user32.PostMessageW(ctypes.c_void_p(hwnd), WM_CLOSE, 0, 0))


def is_iconic(hwnd):
    """窗口是不是"最小化到任务栏"。

    ⚠ 这是 `LAUNCHER-MINIMIZED` 的**外部**佐证：日志只能证明"我们调了
      minimize()"，IsIconic 才能证明"Windows 真的把它收进任务栏了"。
    """
    if not hwnd or not _user32.IsWindow(ctypes.c_void_p(hwnd)):
        return False
    return bool(_user32.IsIconic(ctypes.c_void_p(hwnd)))


def restore_window(hwnd):
    """把最小化的窗口点回前台（等价于点任务栏按钮）。"""
    if not hwnd or not _user32.IsWindow(ctypes.c_void_p(hwnd)):
        return False
    _user32.ShowWindow(ctypes.c_void_p(hwnd), SW_RESTORE)
    return True


def shutdown(proc, hwnd, wait=60.0):
    """正常关窗并等退出，返回 (done, rc)。

    ⚠ 不能 `kill()`：启动器在 `onShutdown` 里保存最近列表，
    强杀会让"列表已落盘"这条断言永远假红。
    """
    delivered = close_window(hwnd)
    if not delivered:
        log("WARN WM_CLOSE 没送达（hwnd=%s）" % hwnd)
    try:
        proc.wait(timeout=wait)
        return True, proc.returncode
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=10)
        return False, proc.returncode


# ---------------------------------------------------------------- 点击

MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004

SW_RESTORE = 9
SWP_NOSIZE, SWP_NOMOVE, SWP_SHOWWINDOW = 0x0001, 0x0002, 0x0040
HWND_TOPMOST = -1
HWND_NOTOPMOST = -2


def focus_window(hwnd):
    """把窗口带回最前（best-effort，失败不报错）。

    ⚠⚠ 为什么必须有：`click()` 走的是**真实鼠标事件**（`SetCursorPos` +
      `mouse_event`），打到的是**光标底下最上层的窗口**，不是我们指定的 hwnd。
      窗口不在最前时，第一次点击只做"激活窗口"，**不会**进 ImGui ——
      现象是"点了没反应、日志一个字都没有"，而且完全看时序（实测同一脚本
      连跑三次，第二次偶发红在向导 Step1→Step2）。
    ⚠ 不用 `AttachThreadInput`：那套要挂到当前前台线程上，写错会把整个
      桌面的输入搞乱。这里用"置顶 → 立刻取消置顶"的老办法，够用且无副作用。
    """
    if not hwnd or not _user32.IsWindow(ctypes.c_void_p(hwnd)):
        return False
    _user32.ShowWindow.argtypes = [ctypes.c_void_p, ctypes.c_int]
    _user32.SetForegroundWindow.argtypes = [ctypes.c_void_p]
    _user32.SetWindowPos.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                     ctypes.c_int, ctypes.c_int,
                                     ctypes.c_int, ctypes.c_int, ctypes.c_uint]
    h = ctypes.c_void_p(hwnd)
    _user32.ShowWindow(h, SW_RESTORE)
    flags = SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW
    _user32.SetWindowPos(h, ctypes.c_void_p(HWND_TOPMOST & 0xFFFFFFFFFFFFFFFF),
                         0, 0, 0, 0, flags)
    _user32.SetWindowPos(h, ctypes.c_void_p(HWND_NOTOPMOST & 0xFFFFFFFFFFFFFFFF),
                         0, 0, 0, 0, flags)
    _user32.SetForegroundWindow(h)
    return True


def client_to_screen(hwnd, cx, cy):
    pt = wt.POINT(int(cx), int(cy))
    _user32.ClientToScreen(ctypes.c_void_p(hwnd), ctypes.byref(pt))
    return pt.x, pt.y


def client_size(hwnd):
    """客户区尺寸（逻辑像素）。

    ⚠ 用 `GetClientRect` 而不是 `GetWindowRect`：外框含标题栏 + 边框，
      且随 DPI 缩放变化 —— 拿它断言"窗口是 1080×720"在 125%/150% 下必红。
    """
    r = wt.RECT()
    _user32.GetClientRect(ctypes.c_void_p(hwnd), ctypes.byref(r))
    return (r.right - r.left, r.bottom - r.top)


def click(hwnd, cx, cy):
    # ⚠ 先置前：否则第一下点击只激活窗口，进不了 ImGui（见 focus_window）
    focus_window(hwnd)
    sx, sy = client_to_screen(hwnd, cx, cy)
    _user32.SetCursorPos(int(sx), int(sy))
    time.sleep(0.12)
    _user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.05)
    _user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
    time.sleep(0.35)


def click_rect(hwnd, rect):
    cx = (rect[0] + rect[2]) * 0.5
    cy = (rect[1] + rect[3]) * 0.5
    click(hwnd, cx, cy)


def click_anchor(hwnd, tap, tag, timeout=15.0):
    r = tap.wait_rect(tag, timeout=timeout)
    if not r:
        return False
    click_rect(hwnd, r)
    return True


# ---------------------------------------------------------------- 键盘

WM_KEYDOWN, WM_KEYUP = 0x0100, 0x0101
VK_SPACE, SC_SPACE = 0x20, 0x39


def press_space(hwnd):
    """给窗口 Post 一次 Space（编辑器里 = 播放 / 暂停）。

    ⚠ 用 `PostMessage` 而不是 `keybd_event`：后者打给**当前前台窗口**，
      本机鼠标可能正停在别的窗口上 → 事件落到别处，现象是"按了没反应"
      而日志毫无痕迹。PostMessage 直接进目标窗口的消息队列。
    ⚠ lParam 的 bit 16-23 必须填**扫描码**（Space = 0x39）：填 0 时
      GLFW 会去 `MapVirtualKey` 反查，对不带扩展位的键通常也能猜对，
      但方向键那类会猜成小键盘键（记忆里踩过）—— 这里按规范填满。
    """
    if not hwnd or not _user32.IsWindow(ctypes.c_void_p(hwnd)):
        return False
    down = (SC_SPACE << 16) | 1
    up = down | (1 << 30) | (1 << 31)
    ok = _user32.PostMessageW(ctypes.c_void_p(hwnd), WM_KEYDOWN, VK_SPACE, down)
    time.sleep(0.08)
    ok = _user32.PostMessageW(ctypes.c_void_p(hwnd), WM_KEYUP, VK_SPACE, up) and ok
    return bool(ok)


# ---------------------------------------------------------------- 工程

def make_project(dest, kind, name=None, with_project_json=True):
    """把 templates/<kind>/ 拷成一个工程（等价于"创建"的最小版本）。

    ⚠ 只在**需要"已有工程"做输入**的用例里用（M1 的最近列表）。
    M2 的创建用例走真正的 `project::createFromTemplate`（由 Launcher 的
    向导触发），不许用这个函数 —— 否则就是拿脚本代替被测对象，自证。
    """
    if os.path.isdir(dest):
        # ⚠ 用 fresh_dir 的"改名让位"而不是 rmtree：宿主有批量删除保护，
        #   一回合删超 50 个文件会把**整个脚本**打掉（见 fresh_dir 注释）。
        fresh_dir(dest)
    os.makedirs(dest, exist_ok=True)
    shutil.copytree(os.path.join(TEMPLATES, kind), dest, dirs_exist_ok=True)
    if with_project_json:
        proj = {
            "format": 1,
            "name": name or os.path.basename(dest),
            "template": kind,
            "gameType": kind,
            "startScene": "scenes/main.json",
            "code": {"mode": "vks", "root": "scripts", "linked": False},
            "createdAt": "2026-10-19T00:00:00Z",
            "engine": {"name": "venn", "config": "Debug"},
        }
        with open(os.path.join(dest, "venn.project.json"), "w",
                  encoding="utf-8", newline="\n") as f:
            json.dump(proj, f, ensure_ascii=False, indent=2)
            f.write("\n")
    return dest


def write_recent(path, items):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    doc = {"format": 1, "items": items}
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, ensure_ascii=False, indent=2)
        f.write("\n")


def read_recent(path):
    """读最近列表；文件坏了按空列表返回（与服务端同一套"坏文件不崩"语义）。"""
    try:
        with open(path, encoding="utf-8") as f:
            doc = json.load(f)
        return doc.get("items", [])
    except Exception:
        return []


def capture(hwnd, out_path):
    """截图（人眼复核排版用）。返回是否成功。"""
    if not out_path:
        return False
    from capture_window import capture_client
    try:
        img, ok = capture_client(hwnd)
        if not ok or img is None:
            return False
        os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
        img.save(out_path)
        return True
    except Exception as e:  # noqa: BLE001
        log("WARN 截图失败: %s" % e)
        return False


def window_title(hwnd):
    """读窗口标题。⚠ 必须显式声明 argtypes，否则 64 位 HWND 会被截断。"""
    if not hwnd:
        return ""
    _user32.GetWindowTextW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p,
                                       ctypes.c_int]
    _user32.GetWindowTextLengthW.argtypes = [ctypes.c_void_p]
    n = _user32.GetWindowTextLengthW(ctypes.c_void_p(hwnd))
    buf = ctypes.create_unicode_buffer(n + 2)
    _user32.GetWindowTextW(ctypes.c_void_p(hwnd), buf, n + 1)
    return buf.value


def wait_title(prefix, timeout=60.0):
    """等一个标题以前缀开头的可见窗口，返回 (hwnd, title)。"""
    end = time.time() + timeout
    while time.time() < end:
        h = find_window(prefix, timeout=0.5)
        if h:
            return h, window_title(h)
    return 0, ""


def project_key(path):
    """路径比较用的规范形式（小写 + 正斜杠）。

    ⚠ Windows 路径大小写不敏感：拿原串比会把 `D:/a/Beta` 与 `d:/A/beta`
    当成两个工程 —— 最近列表里就会出现"同一个工程两条"。C++ 侧的
    `RecentList::remove` 也按大小写不敏感比，两边必须一致。
    """
    return (path or "").replace("\\", "/").rstrip("/").lower()


# ---------------------------------------------------------------- 子进程

SYNCHRONIZE = 0x00100000
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
_kernel32 = ctypes.windll.kernel32
_kernel32.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_bool, ctypes.c_uint32]
_kernel32.OpenProcess.restype = ctypes.c_void_p
_kernel32.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
_kernel32.WaitForSingleObject.restype = ctypes.c_uint32
_kernel32.GetExitCodeProcess.argtypes = [ctypes.c_void_p,
                                         ctypes.POINTER(ctypes.c_uint32)]
_kernel32.GetExitCodeProcess.restype = ctypes.c_bool
_kernel32.CloseHandle.argtypes = [ctypes.c_void_p]

WAIT_OBJECT_0 = 0
STILL_ACTIVE = 259
INFINITE = 0xFFFFFFFF


def wait_pid_exit(pid, timeout_ms=90000):
    """等某个 pid 退出，返回它的退出码；等超时返回 None。

    为什么不用 `Popen.wait()`：编辑器是**孙进程**（启动器拉起它），Python
    手里没有它的 Popen 对象。而"关掉窗口之后它到底退没退、退出码是多少"
    正是 M3 要验证的东西 —— 少这一条就只能靠"窗口不见了"猜。
    """
    h = _kernel32.OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                              False, int(pid))
    if not h:
        return None
    try:
        if _kernel32.WaitForSingleObject(h, timeout_ms) != WAIT_OBJECT_0:
            return None
        code = ctypes.c_uint32(0)
        if not _kernel32.GetExitCodeProcess(h, ctypes.byref(code)):
            return None
        return int(code.value)
    finally:
        _kernel32.CloseHandle(h)


def parse_spawn_pid(text):
    """从日志里取 `LAUNCHER-SPAWN ok pid=NNNN`。取不到返回 0。"""
    m = re.search(r"LAUNCHER-SPAWN ok pid=(\d+)", text)
    return int(m.group(1)) if m else 0


PROCESS_TERMINATE = 0x0001


def kill_pid(pid, timeout_ms=20000):
    """强制结束一个 pid（**只用于我们自己拉起来的验收进程**）。返回已退出。

    ⚠ 这不是给 `verify_launcher_open/code` 用的 —— 那两个要断言编辑器的
      退出码（`rc == 0`），必须走 `close_window` + `wait_pid_exit`。
      这里是给"创建即自动拉起"带来的**副作用**收尾用的，见
      `dismiss_spawned`。
    """
    if not pid:
        return True
    h = _kernel32.OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE |
                              PROCESS_QUERY_LIMITED_INFORMATION,
                              False, int(pid))
    if not h:
        # 打不开 = 已经没有这个进程（或者已经被回收），当作已退出
        return True
    try:
        _kernel32.TerminateProcess.argtypes = [ctypes.c_void_p, ctypes.c_uint]
        _kernel32.TerminateProcess(h, 1)
    finally:
        _kernel32.CloseHandle(h)
    return wait_pid_exit(pid, timeout_ms) is not None


def dismiss_spawned(tap, wait=30.0):
    """把本脚本**自己触发的**编辑器关掉，返回它的 pid（0 = 压根没拉起）。

    ⚠⚠ 为什么需要它：2026-10-22 起「点创建 → 直接进编辑器」是**真契约**
      （用户需求 A），于是**每一个**走完向导的验收脚本都会顺手拉起一个
      编辑器。对 `verify_launcher_open/code` 那是被测对象；但对
      `verify_launcher_create/display`（只关心"工程建出来了没有"）它是
      纯副作用 —— 留着的话三个工程就是三个编辑器窗口，下一个用例的
      `find_window("Venn Editor")` 前缀匹配会**认错人**（拿到上一轮那个
      还没退干净的），断言就会时红时绿。

    ⚠ 用 `taskkill` 式强杀而不是 `close_window`：这里没有"退出码"要断言，
      而窗口可能还停在首帧 / 被别的窗口盖着，`find_window` 未必立刻找得到。
      按 pid 杀是唯一无歧义的做法。
    """
    end = time.time() + wait
    pid = 0
    while time.time() < end and not pid:
        pid = parse_spawn_pid(tap.text())
        if not pid:
            time.sleep(0.1)
    if pid:
        kill_pid(pid)
        log("NOTE 已清掉自动拉起的编辑器 pid=%d（创建即进编辑器是需求 A 的"
            "副作用，本用例不测它）" % pid)
    return pid


def drive_wizard(hwnd, tap, type_index=1, timeout=40.0):
    """把四步向导点完，停在「创建」之前，返回失败描述列表（空 = 一路顺）。

    ⚠ `verify_launcher_create.py` **不用**这个函数：它要逐步断言
      （点卡片之后必须看到 `LAUNCHER-WIZARD type=...`、换页之后必须出现
      新锚点）。这里只负责"推进"，给不需要中间断言的用例用。
    """
    fails = []
    if not tap.wait_rect("LAUNCH-TYPE-%d" % type_index, timeout=25.0):
        fails.append("向导 Step1 没画出类型卡片")
        return fails
    if not click_anchor(hwnd, tap, "LAUNCH-TYPE-%d" % type_index):
        fails.append("点不到 LAUNCH-TYPE-%d" % type_index)
        return fails
    if not click_anchor(hwnd, tap, "LAUNCH-BTN-next"):
        fails.append("点不到 LAUNCH-BTN-next（Step1）")
        return fails
    if not tap.wait_rect("LAUNCH-INPUT-root", timeout=20.0):
        fails.append("没有进入 Step2（缺 LAUNCH-INPUT-root）")
        return fails
    if not click_anchor(hwnd, tap, "LAUNCH-BTN-next"):
        fails.append("点不到 LAUNCH-BTN-next（Step2）")
        return fails
    if not tap.wait_rect("LAUNCH-COMBO-res", timeout=20.0):
        fails.append("没有进入 Step3（缺 LAUNCH-COMBO-res）")
        return fails
    if not click_anchor(hwnd, tap, "LAUNCH-BTN-create"):
        fails.append("点不到 LAUNCH-BTN-create")
        return fails
    if not tap.wait_rect("LAUNCH-DONE", timeout=timeout):
        fails.append("创建没有成功（没出现 LAUNCH-DONE）")
    return fails


def fresh_dir(path):
    """返回一个**保证干净**的工作目录；**一个文件都不删**。

    ⚠⚠ 绝不能用 `shutil.rmtree(path, ignore_errors=True)` 了事：删失败的
    后果是"上一轮的工程还在"，于是本轮的"创建"被非空目录拒掉，而所有
    断言读的都是**上一轮的旧文件** —— 全绿，但一个字都没验。
    实测踩过一次（`_venn_verify` 下的目录被索引器短暂占用）。

    ⚠⚠ 还有第二种失败：宿主/沙箱的**批量删除保护**（按**回合累计**计，阈值 50 个
    文件）。一个工程目录 15~30 个文件 → 一个回合里连跑三个 GUI 用例就撞。
    ⚠⚠ 而且是**硬杀**：超预算的 `rmtree` 会让**整个脚本进程**直接退出（日志里
    只有一行 `[safe-delete][SAFE_DELETE_BULK_CONFIRM_REQUIRED]`，没有 traceback
    —— `ignore_errors=True` **接不住**，它不在 Python 层）。所以这里
    **一个文件都不删**：
      `launcher_code` → 改名让位 `launcher_code.old-183501`（`rename` 不计数），
      再新建一个空的 `launcher_code`。旧目录留着（它已不在本轮任何路径上，
      正确性不受影响）；要腾地方就人工清一次 `_venn_verify`。

    返回值就是本次要用（且已经被 `makedirs` 建好）的目录。
    """
    if os.path.isdir(path):
        trash = "%s.old-%s" % (path, time.strftime("%H%M%S"))
        try:
            os.rename(path, trash)
            log("NOTE 旧工作目录已让位：%s（未删除，必要时人工清理）"
                % os.path.basename(trash))
        except OSError as e:
            # 连改名都不行（目录被某个进程占着）→ 换个兄弟目录，别硬碰
            alt = "%s.new-%s" % (path, time.strftime("%H%M%S"))
            log("WARN 改不掉名 %s（%s）→ 改用 %s"
                % (path, e.__class__.__name__, os.path.basename(alt)))
            os.makedirs(alt, exist_ok=True)
            return alt
    os.makedirs(path, exist_ok=True)
    return path


def finish(fails, keep_msg=None):
    """统一收尾：打印失败项并给出**必须的**结果行。

    ⚠⚠ 脚本结尾必须是 `结果: PASS` / `结果: FAIL: ...`（ASCII 冒号，
    `run_regression.py` 取最后一条）。这一条已经踩过五次。
    """
    if fails:
        for f in fails:
            log("CHECK FAIL " + f)
        log("结果: FAIL: %d 项不达标" % len(fails))
        return 1
    if keep_msg:
        log(keep_msg)
    log("结果: PASS")
    return 0
