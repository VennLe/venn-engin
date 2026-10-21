"""端到端验证：建模模式的多边形层 + 三个新拓扑操作（2026-10-21）。

被验证的用户需求（原话）：
  1) "Modeling 建模模式中，对于立方体，为什么显示出的每个正方形面都是两个
     三角形拼接而成的，而不是一整个的正方形。这种对建模操作显示是不对的。"
  2) "建模模式应该包含一下操作：挤出选区，内插面，倒角，环切。"

--------------------------------------------------------------------------
断言分组
--------------------------------------------------------------------------
  A. 多边形层（"一整张正方形面"）
     A1 立方体绑定后 `MODEL-BIND ... polys=6`
        —— 6 = 六个四边形。报 12 就说明共面合并没生效（每个面还是两个
           三角形），那正是用户看到"被对角线劈开"的根。
     A2 三角形数仍是 12（渲染网格不变，只是**面**的视图变了）
  B. 内插面（Inset）
     B1 `MODEL-INSET faces=6 newVerts=24` —— 6 个面 × 4 个新内角
     B2 `polys=30` —— 6 个内面 + 24 个边环四边形
  C. 倒角（Bevel）
     C1 `MODEL-BEVEL corners=8 cuts=12 caps=8` —— 立方体 8 角 12 棱
     C2 `polys=14` —— 6 个被切掉角的面 + 8 个角上的切面
  D. 环切（Loop cut）
     D1 `MODEL-LOOPCUT faces=4 newQuads=8` —— 一圈穿过 4 个面，每个拆成 2 个
     D2 `polys=10` —— 6 - 4 + 8

--------------------------------------------------------------------------
为什么每个操作都**重新启动一次编辑器**
--------------------------------------------------------------------------
建模操作的输入是"当前网格"，三步连跑的话第二、三步面对的是上一步改过的
网格（顶点数/面数全变），断言只能写成"比上次多"这种没信息量的形式。
而撤销是**整场景快照**，`toolkit/model/<id>/<rev>` 这种运行时注册的网格
没有 source 描述，回滚不可靠。
所以每个 phase 都从"全新立方体"开始 —— 数字是确定的，也就真的能断言。

用法：python tools/verify_modeling_topology.py
"""

import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from capture_window import find_window                       # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors       # noqa: E402
import verify_toolkit_modes as vtm                           # noqa: E402

EXE = vtm.EXE
OUT_DIR = vtm.OUT_DIR


def parse(line, key):
    m = re.search(r"%s=(\d+)" % key, line)
    return int(m.group(1)) if m else -1


def wait_event(tap, pattern, timeout=8.0):
    """等一条**事件**行（不是 logRect 锚点行）。"""
    rx = re.compile(pattern)
    rect_like = re.compile(re.escape(pattern.split("=")[0]) + r"=\(")
    deadline = time.time() + timeout
    while time.time() < deadline:
        with tap.lock:
            for line in reversed(tap.lines):
                if rx.search(line) and not rect_like.search(line):
                    return line
        time.sleep(0.12)
    return None


def launch():
    kill_stale_editors()
    os.makedirs(OUT_DIR, exist_ok=True)
    env = dict(os.environ)
    env.update({
        "MYVK_LOG_RECTS": "1",
        "MYVK_NO_WINDOW_SAVE": "1",
        "MYVK_EDITOR_NO_HUD": "1",
        "MYVK_LIGHT_GIZMOS": "0",
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
    return proc, tap


def stop(proc):
    try:
        proc.terminate()
        proc.wait(timeout=8)
    except Exception:                            # noqa: BLE001
        pass
    kill_stale_editors()


def phase(name, button, pattern, expect):
    """跑一个 phase：全新立方体 → Modeling → 全选 → 点按钮 → 断言字段。

    expect: {字段名: 期望值}，字段名取自日志（faces / newVerts / polys …）
    """
    print("\n[%s]" % name)
    proc, tap = launch()
    try:
        hwnd = find_window("Venn Editor", timeout=60)
        if not hwnd:
            vtm.check(False, "%s：找不到编辑器窗口" % name)
            return
        time.sleep(2.5)
        if not vtm.switch_mode(tap, hwnd, "modeling"):
            vtm.check(False, "%s：切不到 Modeling 模式" % name)
            return
        time.sleep(0.8)

        bind = wait_event(tap, r"MODEL-BIND ")
        if not bind:
            vtm.check(False, "%s：MODEL-BIND 没出现（选中项没有网格？）" % name)
            return
        # 每个 phase 的第一件事都是确认输入确实是那 6 面立方体
        vtm.check(parse(bind, "polys") == 6,
                  "%s：立方体合并成 6 个四边形（polys=%d）"
                  % (name, parse(bind, "polys")))
        vtm.check(parse(bind, "tris") == 12,
                  "%s：渲染三角形仍是 12（面视图变了、几何没变）" % name)

        ok, _ = vtm.act(tap, hwnd, "MD-SELALL", name + "-全选",
                        r"MODEL-SEL ", body_tag="TK-modeling")
        if not ok:
            return
        ok, logged = vtm.act(tap, hwnd, button, name + "-按钮",
                             pattern, body_tag="TK-modeling")
        if not ok:
            return
        if not logged:
            vtm.check(False, "%s：点了 %s 没有对应日志（%s）"
                      % (name, button, pattern))
            return
        line = wait_event(tap, pattern)
        if not line:
            vtm.check(False, "%s：抓不到 %s 的日志行" % (name, pattern))
            return
        print("      ·", line.strip()[-120:])
        for k, v in expect.items():
            got = parse(line, k)
            vtm.check(got == v, "%s %s=%d（期望 %d）" % (name, k, got, v))
        vtm.shoot(hwnd, "modeling_%s.png" % button.lower())
    finally:
        stop(proc)


def main():
    if not os.path.exists(EXE):
        print("找不到可执行文件，请先构建：", EXE)
        return 1

    print("=" * 74)
    print("建模多边形层 + 拓扑刷子（每个 phase 独立启动一次）")
    print("=" * 74)

    phase("B 内插面", "MD-INSET", r"MODEL-INSET ",
          {"faces": 6, "newVerts": 24, "polys": 30})
    phase("C 倒角", "MD-BEVEL", r"MODEL-BEVEL ",
          {"corners": 8, "cuts": 12, "caps": 8, "polys": 14})
    phase("D 环切", "MD-LOOPCUT", r"MODEL-LOOPCUT ",
          {"faces": 4, "newQuads": 8, "polys": 10})

    print("\n截图：%s" % OUT_DIR)
    if vtm.fails:
        print("\n失败 %d 项：" % len(vtm.fails))
        for f in vtm.fails:
            print("  -", f)
        return 1
    print("\n全部通过。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
