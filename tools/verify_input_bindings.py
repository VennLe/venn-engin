#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""G06 键位绑定：自检 + 落盘往返 + 运行时生效 + 编辑器 UI 面。

## 这个用例守的是什么

G06 的**重绑定面板**是整个 M2 里唯一"改了要能存下来、存下来要能被游戏
读到、读到了要能撤销"的东西。它不是一条渲染链路，错了不会花屏 ——
错了的表现是"玩家改了键位，下次进游戏还是老的"，没有任何报错。
所以断言必须覆盖**整条链**，而不只是"面板画出来了"：

    A  引擎自检（无窗口）：改绑 / 冲突 / 单项重置 / 整表往返
    B  运行时没文件 → 出厂绑定（默认路径存在与否都该是这条路）
    C  编辑器改绑 + 存盘 → 文件内容逐字段正确
    D  运行时读那**同一份**文件 → 绑定真的换成了新键（Sprint 等其它动作没丢）
    E  编辑器再读回来 → source=file（读路径，不是"写了就完事"）
    F  撤销：改绑 → undo → 存盘，文件里退回出厂值（证明可撤销）
    G  编辑器 UI 面：Preferences ▸ Bindings 页真的画出来了（矩形锚点）
    H  冲突检测：把 Jump 改到 W（MoveForward 占着）→ 面板报冲突
    I  自动化纪律：`MYVK_FRAMES` 下**默认路径**既不读也不写

## 为什么用 `MYVK_EDITOR_REBIND` 而不是点 UI

键位捕获必须"按下真实按键"，自动化里没有可靠手段（PostMessage 到 GLFW
只在有焦点时成立）。编辑器侧因此开了一个脚本口子，它走的是**和 UI 完全
同一条写路径**（EditorContext::rebindAction → InputMap::setActionBinding
→ 撤销栈）。于是 A~F 测的是逻辑、G/H 测的是界面存在性，两边合起来等于
"面板能做的事都被测过了"。

键码用 GLFW 数字：70 = GLFW_KEY_F，87 = GLFW_KEY_W，83/340 = Shift，
32 = Space。

## 环境
    MYVK_EDITOR_INPUT_MAP   编辑器侧路径（**显式给**才允许自动化下读写）
    MYVK_RUNTIME_INPUT_MAP  运行时侧路径
    MYVK_EDITOR_PREFS=1     启动就打开 Preferences
    MYVK_EDITOR_PREFS_PAGE  Bindings（按名字，不按下标）
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from verify_add_menu import kill_stale_editors                   # noqa: E402
from verify_player import find_window                            # noqa: E402
from safe_fs import safe_remove

BIN = os.path.join(ROOT, "build", "bin")
EXE_EDITOR = os.path.join(BIN, "Editor.exe")
EXE_GAME = os.path.join(BIN, "Game.exe")
SCENE = os.path.join(ROOT, "assets", "scenes", "sandbox.json")

KEY_F = 70
KEY_W = 87
KEY_SPACE = 32
KEY_LEFT_SHIFT = 340

# GLFW 键码 → 可读名（只列本文件用到的；InputMap::describeOne 的事实源是
# keyName()，这里只为断言消息可读，不参与判定）。
KEYNAMES = {KEY_F: "F", KEY_W: "W", KEY_SPACE: "Space", KEY_LEFT_SHIFT: "Shift"}

failures: list[str] = []
passed = 0


def check(what: str, ok: bool, detail: str = "") -> bool:
    global passed
    if ok:
        passed += 1
        print("  %-58s PASS  %s" % (what, detail))
    else:
        failures.append(what)
        print("  %-58s FAIL  %s" % (what, detail))
    return ok


def run(exe: str, env: dict, frames: int = 8, timeout: int = 120) -> str:
    """跑一次（帧数受限，自动退出），返回合并输出。

    ⚠ `frames` **必须 > 0**：`Application::run()` 的判据是 `maxFrames > 0`，
      传 0 等于"永不退出" —— 整个脚本会挂在那儿直到外层超时（踩过：
      B 组第一次跑就是这个死法，日志停在 "[B] ..." 之后再没输出）。
      `MYVK_FRAMES` 同时是"自动化运行"的判据 —— 于是编辑器的**默认路径**
      读/写会被拒（见 editor/InputMapIO.h）。本文件凡是需要真正读写的
      用例都显式给了 `MYVK_EDITOR_INPUT_MAP` / `MYVK_RUNTIME_INPUT_MAP`。
    """
    e = dict(os.environ)
    e.update(env)
    e["MYVK_LOG_RECTS"] = "1"
    e["MYVK_NO_WINDOW_SAVE"] = "1"
    e["MYVK_FRAMES"] = str(max(frames, 1))
    # 语言钉成英文：日志里的可读绑定名（"Space" / "Shift"）与本地化无关，
    # 但状态栏文本会随语言变，钉住能少一个变量。
    e.setdefault("MYVK_EDITOR_LANGUAGE", "0")
    p = subprocess.run([exe], cwd=os.path.dirname(exe), env=e,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, encoding="utf-8", errors="replace",
                       timeout=timeout)
    return p.stdout


def lines_of(out: str, tag: str) -> list[str]:
    return [ln for ln in out.splitlines() if tag in ln]


def last_line(out: str, tag: str) -> str | None:
    ls = lines_of(out, tag)
    return ls[-1] if ls else None


def bindings_of(out: str) -> dict[str, str]:
    """把 `INS-BIND-AFTER ctx/Action=desc` 解成 {ctx/Action: desc}。

    ⚠ desc 里可能含 ` / `（多来源，如 `PadBtn0 / F`），所以不能按空格切 ——
    按 tag 的正则整体取出。
    """
    m: dict[str, str] = {}
    for ln in out.splitlines():
        r = re.search(r"INS-BIND-AFTER (\S+?)=(.*)$", ln.rstrip())
        if r:
            m[r.group(1)] = r.group(2).strip()
    return m


def key_of(desc: str | None) -> str | None:
    """从 `PadBtn0 / F` 里挑出**键盘**那一段（没有键盘绑定返回 None）。

    手柄侧的名字都以 Pad 开头，键盘侧是 `W` / `Space` / `LShift` 这种。
    """
    if not desc:
        return None
    for part in [p.strip() for p in desc.split("/")]:
        if part and not part.startswith("Pad"):
            return part
    return None


# ---------------------------------------------------------------- A
def group_a_selftest() -> None:
    print("\n[A] 引擎自检（无窗口，注入 RawInput）")
    out = run(EXE_GAME, {"MYVK_RUNTIME_SELFTEST": "input"}, frames=1)
    total = None
    for ln in out.splitlines():
        m = re.search(r"INPUT-TOTAL pass=(\d+) fail=(\d+)", ln)
        if m:
            total = (int(m.group(1)), int(m.group(2)))
    if total is None:
        check("A0 找到 INPUT-TOTAL", False, "运行输出里没有这行")
        return
    npass, nfail = total
    check("A1 自检零失败", nfail == 0, "pass=%d fail=%d" % (npass, nfail))
    # ⚠ 断言**下限**：以后再加断言不该让这条变红；但"pass=0 fail=0"那种
    #   假绿必须堵死（自检根本没跑）。
    check("A2 自检确实跑了（pass >= 40）", npass >= 40, "pass=%d" % npass)

    # A3~A5：重绑定那一组的**关键**几条必须真的存在。
    # 只数总数不点名的话，把 H 组整段删掉也还是绿的（总数照样够）。
    for tag, what in (
        ("H5 手柄绑定被保留", "A3 只替换同一来源类型（手柄绑定不丢）"),
        ("H7 新的 F 触发 Sprint", "A4 改绑后新键真的触发"),
        ("H8 重绑定后的表能往返", "A5 改绑后能序列化往返"),
        ("H13 重置后回到出厂", "A6 单项重置回到出厂"),
        ("H15 动作定义仍在", "A7 清空绑定不删动作（轴不失效）"),
    ):
        check(what, any(("INPUT-TEST pass " + tag) in ln
                        for ln in out.splitlines()))


# ---------------------------------------------------------------- B
def group_b_runtime_factory() -> None:
    print("\n[B] 运行时：没有绑定表 → 出厂绑定")
    out = run(EXE_GAME, {"MYVK_RUNTIME_SCENE": SCENE,
                         "MYVK_RUNTIME_INPUT_MAP": "definitely_not_here.json"},
              frames=3)
    ln = last_line(out, "GAME-INPUT source=")
    check("B1 source=factory", ln is not None and "source=factory" in ln,
          ln.strip() if ln else "没打这行")
    ln = last_line(out, "GAME-INPUT Jump=")
    check("B2 Jump 是出厂 Space", ln is not None and "Jump=Space" in ln,
          ln.strip() if ln else "没打这行")


# ---------------------------------------------------------------- C
def group_c_editor_write(path: str) -> bool:
    print("\n[C] 编辑器改绑 + 存盘（写路径）")
    if os.path.exists(path):
        safe_remove(path)
    out = run(EXE_EDITOR, {
        "MYVK_EDITOR_SCENE": SCENE,
        "MYVK_EDITOR_INPUT_MAP": path,
        "MYVK_EDITOR_REBIND": "Gameplay/Jump=%d;save" % KEY_F,
    }, frames=10)

    ln = last_line(out, "INS-BIND-INJECT")
    check("C1 注入走了同一条写路径（ok=1）",
          ln is not None and "ok=1" in ln, ln.strip() if ln else "没打这行")
    ln = last_line(out, "INS-BIND-SCRIPT")
    # applied=2 = 改绑 + 存盘各算一次
    check("C2 脚本 applied=2 failed=0",
          ln is not None and "applied=2 failed=0" in ln,
          ln.strip() if ln else "没打这行")
    ln = last_line(out, "INS-BIND saved")
    check("C3 文件真的写出来了", ln is not None and path in ln,
          ln.strip() if ln else "没打这行")
    check("C4 文件存在于磁盘", os.path.exists(path), path)

    got = bindings_of(out)
    check("C5 生效后的绑定表：Jump 的键盘是 F",
          key_of(got.get("Gameplay/Jump")) == "F",
          repr(got.get("Gameplay/Jump")))
    # ⚠ 关键：改 Jump 不该顺手把别的动作打乱（尤其 Sprint 的 Shift 与手柄）
    check("C6 Sprint 仍是 Shift（没被连带改掉）",
          key_of(got.get("Gameplay/Sprint")) == "LShift",
          repr(got.get("Gameplay/Sprint")))
    check("C7 MoveForward 仍是 W",
          key_of(got.get("Gameplay/MoveForward")) == "W",
          repr(got.get("Gameplay/MoveForward")))

    # 落盘内容逐字段核对（"文件存在即可"会让少写一个字段变绿）
    if not os.path.exists(path):
        return False
    try:
        with open(path, "r", encoding="utf-8") as f:
            doc = json.load(f)
    except Exception as exc:                       # noqa: BLE001
        check("C8 文件是合法 JSON", False, str(exc))
        return False
    check("C8 文件是合法 JSON", True, "%d 字节" % os.path.getsize(path))
    ctxs = doc.get("contexts", {})
    gj = ctxs.get("Gameplay", {}).get("actions", {}).get("Jump", [])
    keys = [b for b in gj if b.get("source") == "key"]
    # ⚠ 落盘里手柄按钮的 source 名是 `padButton`（InputMap 的
    #   sourceName() 就这么写）。第一版这里写的是 "gamepadbutton"，
    #   于是"手柄绑定还在"永远假红 —— **JSON 字段名以源码为准，不靠猜**。
    pads = [b for b in gj if b.get("source") == "padButton"]
    check("C9 Jump 有一条键盘绑定且 code=F",
          len(keys) == 1 and keys[0].get("code") == KEY_F,
          json.dumps(gj, ensure_ascii=False))
    check("C10 Jump 的手柄绑定还在（只替换同类来源）",
          len(pads) == 1, json.dumps(gj, ensure_ascii=False))
    check("C11 版本号仍在（老版本读新文件要能判废）",
          doc.get("version") == 1, str(doc.get("version")))
    return True


# ---------------------------------------------------------------- D
def group_d_runtime_reads(path: str) -> None:
    print("\n[D] 运行时读编辑器写的那份（生效路径）")
    out = run(EXE_GAME, {"MYVK_RUNTIME_SCENE": SCENE, "MYVK_RUNTIME_INPUT_MAP": path},
              frames=3)
    ln = last_line(out, "GAME-INPUT source=")
    check("D1 source=file", ln is not None and "source=file" in ln,
          ln.strip() if ln else "没打这行")
    ln = last_line(out, "GAME-INPUT Jump=")
    # 描述里可能有多来源（`PadBtn0 / F`），所以看的是"键盘那一段是 F"。
    check("D2 Jump 换成了 F（绑定真的生效了）",
          ln is not None and re.search(r"Jump=(?:Pad\S* / )?F\b", ln) is not None,
          ln.strip() if ln else "没打这行")
    # ⚠ "读到文件"与"只读到 Jump"是两件事：整表替换的 bug 会让其它动作全没
    check("D3 Sprint 仍有绑定（整张表都读进来了）",
          ln is not None and "Sprint=" in ln and "Sprint=(unbound)" not in ln,
          ln.strip() if ln else "没打这行")


# ---------------------------------------------------------------- E
def group_e_editor_reads(path: str) -> None:
    print("\n[E] 编辑器再读回来（读路径）")
    # `dump` 是"不改任何东西、只把读进来的整张表打出来"，于是这里断言的是
    # **编辑器内存里**的值，不是文件字节 —— 两者不一致正是最该抓的 bug。
    out = run(EXE_EDITOR, {"MYVK_EDITOR_SCENE": SCENE,
                           "MYVK_EDITOR_INPUT_MAP": path,
                           "MYVK_EDITOR_REBIND": "dump"}, frames=8)
    ln = last_line(out, "INS-BIND source=")
    check("E1 source=file（不是每次都从出厂开始）",
          ln is not None and "source=file" in ln,
          ln.strip() if ln else "没打这行")
    got = bindings_of(out)
    check("E2 读回来的内存表里 Jump 的键盘是 F",
          key_of(got.get("Gameplay/Jump")) == "F",
          repr(got.get("Gameplay/Jump")))
    # `INS-BIND loaded` 由 loadEditorInputMap 打 —— 它出现就说明走的是
    # "解析文件"这条路，而不是"读了但退回出厂"（后者 source 也会是 file 以外的值，
    # 但这条更直接：它是解析成功的凭据）。
    check("E3 确实解析了文件（INS-BIND loaded）",
          last_line(out, "INS-BIND loaded") is not None, "")


# ---------------------------------------------------------------- F
def group_f_undo(path: str) -> bool:
    print("\n[F] 改绑 → undo → 存盘（可撤销）")
    work = path + ".undo"
    out = run(EXE_EDITOR, {
        "MYVK_EDITOR_SCENE": SCENE,
        "MYVK_EDITOR_INPUT_MAP": work,
        "MYVK_EDITOR_REBIND": "Gameplay/Jump=%d;undo;save" % KEY_F,
    }, frames=10)
    got = bindings_of(out)
    check("F1 undo 之后 Jump 的键盘回到出厂 Space",
          key_of(got.get("Gameplay/Jump")) == "Space",
          repr(got.get("Gameplay/Jump")))
    ln = last_line(out, "INS-BIND-SCRIPT")
    check("F2 脚本 applied=3 failed=0（改绑 + undo + 存盘）",
          ln is not None and "applied=3 failed=0" in ln,
          ln.strip() if ln else "没打这行")
    if not os.path.exists(work):
        check("F3 存盘文件存在", False, work)
        return False
    with open(work, "r", encoding="utf-8") as f:
        doc = json.load(f)
    keys = [b for b in doc["contexts"]["Gameplay"]["actions"]["Jump"]
            if b.get("source") == "key"]
    check("F3 文件里也是 Space（撤销结果落了盘）",
          len(keys) == 1 and keys[0].get("code") == KEY_SPACE,
          json.dumps(keys, ensure_ascii=False))
    return True


# ---------------------------------------------------------------- G
def group_g_ui(path: str) -> None:
    print("\n[G] 编辑器 UI 面：Preferences ▸ Bindings")
    out = run(EXE_EDITOR, {
        "MYVK_EDITOR_SCENE": SCENE,
        "MYVK_EDITOR_INPUT_MAP": path,     # 指到一个不存在的新路径 → factory
        "MYVK_EDITOR_PREFS": "1",
        "MYVK_EDITOR_PREFS_PAGE": "Bindings",
    }, frames=12)

    def rect(tag: str):
        r = last_line(out, tag + "=")
        if not r:
            return None
        m = re.search(re.escape(tag) + r"=\((-?\d+),(-?\d+)\)-\((-?\d+),(-?\d+)\)",
                      r)
        return tuple(float(x) for x in m.groups()) if m else None

    def rect_ok(tag: str):
        # ⚠ 宽/高都要 > 0：被裁到 0 宽也算"出现过"，那种断言等于没测。
        r = rect(tag)
        return r is not None and (r[2] - r[0]) >= 8 and (r[3] - r[1]) >= 4

    check("G1 页签 Bindings 存在（PREFS-TAB-Bindings）",
          rect_ok("PREFS-TAB-Bindings"), repr(rect("PREFS-TAB-Bindings")))
    check("G2 绑定表画出来了（INS-BIND-BODY）",
          rect_ok("INS-BIND-BODY"), repr(rect("INS-BIND-BODY")))
    check("G3 表头来源行画出来了（INS-BIND-SOURCE）",
          rect_ok("INS-BIND-SOURCE"), repr(rect("INS-BIND-SOURCE")))
    check("G4 动作行存在（INS-BIND-ROW-Jump）",
          rect_ok("INS-BIND-ROW-Jump"), repr(rect("INS-BIND-ROW-Jump")))
    check("G5 动作行存在（INS-BIND-ROW-MoveForward）",
          rect_ok("INS-BIND-ROW-MoveForward"),
          repr(rect("INS-BIND-ROW-MoveForward")))
    # G6：出厂表**零冲突**（同一上下文里每个键只归一个动作）
    ln = last_line(out, "INS-BIND-CONFLICT")
    check("G6 出厂绑定表零冲突", ln is not None and "INS-BIND-CONFLICT none" in ln,
          ln.strip() if ln else "没打这行")

    # G7~G8：捕获态是**模态**的，它自己也是一块 UI（要能被断言）。
    # 真捕获必须按真键 → 用 MYVK_EDITOR_BIND_CAPTURE 直接进入这个状态。
    out2 = run(EXE_EDITOR, {
        "MYVK_EDITOR_SCENE": SCENE,
        "MYVK_EDITOR_INPUT_MAP": path,
        "MYVK_EDITOR_PREFS": "1",
        "MYVK_EDITOR_PREFS_PAGE": "Bindings",
        "MYVK_EDITOR_BIND_CAPTURE": "Gameplay/Jump",
    }, frames=10)
    check("G7 进入捕获态（INS-BIND-CAPTURE-ARMED）",
          last_line(out2, "INS-BIND-CAPTURE-ARMED") is not None,
          (last_line(out2, "INS-BIND-CAPTURE-ARMED") or "").strip())
    r = last_line(out2, "INS-BIND-CAPTURE=")
    check("G8 捕获提示画出来了（INS-BIND-CAPTURE）", r is not None,
          (r or "没打这行").strip())
    # ⚠ G9：捕获期间**表不画**（"按下去落到表里某个按钮上"是这种模态 UI
    #   最容易出的 bug）。锚点消失就是"表没画"的机器证据。
    check("G9 捕获期间动作表不画（INS-BIND-ROW-Jump 消失）",
          last_line(out2, "INS-BIND-ROW-Jump") is None,
          (last_line(out2, "INS-BIND-ROW-Jump") or "").strip())


# ---------------------------------------------------------------- H
def group_h_conflict() -> None:
    print("\n[H] 冲突检测：把 Jump 改到被占用的 W")
    out = run(EXE_EDITOR, {
        "MYVK_EDITOR_SCENE": SCENE,
        "MYVK_EDITOR_INPUT_MAP": os.path.join(tempfile.gettempdir(),
                                              "venn_bind_conflict.json"),
        "MYVK_EDITOR_REBIND": "Gameplay/Jump=%d" % KEY_W,
        "MYVK_EDITOR_PREFS": "1",
        "MYVK_EDITOR_PREFS_PAGE": "Bindings",
    }, frames=12)
    ln = last_line(out, "INS-BIND-CLASH")
    check("H1 捕获时当场报冲突（INS-BIND-CLASH）",
          ln is not None and "also=MoveForward" in ln,
          ln.strip() if ln else "没打这行")
    ln = last_line(out, "INS-BIND-CONFLICT")
    check("H2 表里的冲突汇总点名 MoveForward",
          ln is not None and "MoveForward" in ln,
          ln.strip() if ln else "没打这行")
    got = bindings_of(out)
    check("H3 冲突**不阻止**绑定（只警告，玩家有最终决定权）",
          key_of(got.get("Gameplay/Jump")) == "W",
          repr(got.get("Gameplay/Jump")))


# ---------------------------------------------------------------- I
def group_i_discipline() -> None:
    print("\n[I] 自动化纪律：默认路径不读不写")
    default = os.path.join(BIN, "assets", "input_map.json")
    backup = None
    if os.path.exists(default):
        with open(default, "rb") as f:
            backup = f.read()
        safe_remove(default)
    try:
        # 不给 MYVK_EDITOR_INPUT_MAP → 默认路径。自动化下既不读也不写。
        out = run(EXE_EDITOR, {
            "MYVK_EDITOR_SCENE": SCENE,
            "MYVK_EDITOR_REBIND": "Gameplay/Jump=%d;save" % KEY_F,
        }, frames=8)
        ln = last_line(out, "INS-BIND source=")
        check("I1 自动化下默认路径不读（source=factory）",
              ln is not None and "source=factory" in ln,
              ln.strip() if ln else "没打这行")
        ln = last_line(out, "INS-BIND save refused")
        check("I2 自动化下默认路径拒写", ln is not None,
              ln.strip() if ln else "没打这行（= 它真写了）")
        check("I3 默认路径确实没被写出来", not os.path.exists(default), default)
    finally:
        if backup is not None:
            with open(default, "wb") as f:
                f.write(backup)


# ---------------------------------------------------------------- main
def main() -> int:
    for exe in (EXE_EDITOR, EXE_GAME):
        if not os.path.exists(exe):
            print("找不到 %s —— 先构建" % exe)
            print("结果: FAIL: 缺可执行文件")
            return 1

    kill_stale_editors()
    tmpdir = tempfile.mkdtemp(prefix="venn_bind_")
    path = os.path.join(tmpdir, "input_map.json")

    group_a_selftest()
    group_b_runtime_factory()
    wrote = group_c_editor_write(path)
    if wrote:
        group_d_runtime_reads(path)
        group_e_editor_reads(path)
    else:
        check("D/E 组依赖 C 组写出的文件", False, "C 组没写出文件，跳过会导致假绿")
    group_f_undo(path)
    group_g_ui(os.path.join(tmpdir, "never_written.json"))
    group_h_conflict()
    group_i_discipline()

    print("\n" + "=" * 68)
    if failures:
        print("结果: FAIL: %d 项未通过 -> %s" % (len(failures), failures))
        return 1
    print("结果: PASS  (%d 条断言)" % passed)
    return 0


if __name__ == "__main__":
    sys.exit(main())
