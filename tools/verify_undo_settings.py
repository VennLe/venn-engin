"""端到端验证：E19 撤销覆盖扩展到"设置类改动"。

背景：E18 之前"拖一下 Exposure 滑条就回不去了" —— 只有变换 / 碰撞体 /
图元参数可撤销，**渲染设置参数改了就没了**（它们由 E02 的生成器直接写
注册表绑定的地址，不经过任何命令）。

本脚本验证四件事（走与 UI **完全同一条** hook：onFeatureParamUndo）：

  1. **改一次就进历史**：`UNDO-PARAM push`，且历史名字是人话
     （`Set SSR Strength: 0.500 -> 1.500`）。
  2. **500ms 内连续编辑合并**：第二次改走 `UNDO-PARAM merge`，
     命令栈深度**不增**（depthDelta=0）—— 拖两下不该留下两条历史。
  3. **undo 回到旧值 / redo 回到新值**：undoOk=1 / redoOk=1。
  4. **kind 无关**：Float（ssr.strength）与 Bool（ssr.enabled）各验一遍
     —— 生成器的所有控件类型都接进来了，不是只接了滑条。

自检命令 `undo.selftest` 跑的就是真实路径，日志形如：
    UNDO-SELFTEST param=ssr.strength kind=3 merged=1 undoOk=1 redoOk=1
                  restored=1 depthDelta=0
（kind 就是 render::ParamKind 的枚举值：0=Bool 1=Int 2=Enum 3=Float
  4=Color3 5=Vec3）

用法：
    python tools/verify_undo_settings.py
"""

import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import find_window                                    # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors                    # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")

SELFTEST_RE = re.compile(
    r"UNDO-SELFTEST param=(\S+) kind=(\d+) merged=(\d+) undoOk=(\d+) "
    r"redoOk=(\d+) restored=(\d+) depthDelta=(-?\d+)")
PUSH_RE = re.compile(r'UNDO-PARAM push id=(\S+) name="([^"]*)"')
MERGE_RE = re.compile(r'UNDO-PARAM merge id=(\S+) name="([^"]*)"')

FAILS = []


def check(name, cond, detail=""):
    print("  %s %s%s" % ("[PASS]" if cond else "[FAIL]", name,
                         ("  " + str(detail)) if detail != "" else ""),
          flush=True)
    if not cond:
        FAILS.append(name)
    return cond


def run_once(exe, selftest_arg, timeout=30.0):
    """起编辑器，跑一条 undo.selftest，返回 stdout 行。"""
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_EDITOR_CONSOLE_CMD"] = "undo.selftest " + selftest_arg
    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    hwnd = find_window("Venn Editor", 20.0)
    end = time.time() + timeout
    while time.time() < end:
        with tap.lock:
            if any("UNDO-SELFTEST" in l for l in tap.lines):
                break
        time.sleep(0.2)
    time.sleep(0.8)
    with tap.lock:
        lines = list(tap.lines)
    try:
        from capture_window import user32
        if hwnd:
            user32.PostMessageW(hwnd, 0x0010, 0, 0)
    except Exception:
        pass
    time.sleep(0.5)
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=6)
        except subprocess.TimeoutExpired:
            proc.kill()
    return lines


def group(exe, arg, want_kind, label):
    print("\n== %s（undo.selftest %s）==" % (label, arg))
    kill_stale_editors()
    lines = run_once(exe, arg)

    st = None
    for l in lines:
        m = SELFTEST_RE.search(l)
        if m:
            st = m
    if not check("拿到 UNDO-SELFTEST", st is not None,
                 "" if st else "没等到自检日志"):
        return None

    print("  %s" % st.group(0))
    check("自检参数 = %s" % arg, st.group(1) == arg, st.group(1))
    check("kind = %d（%s）" % (want_kind, "Bool" if want_kind == 0 else "Float"),
          int(st.group(2)) == want_kind, st.group(2))
    check("第二次编辑被合并（depthDelta == 0）", int(st.group(7)) == 0,
          st.group(7))
    check("undo 回到旧值（undoOk=1）", st.group(4) == "1", st.group(4))
    check("redo 回到新值（redoOk=1）", st.group(5) == "1", st.group(5))
    check("自检结束已复原（restored=1）", st.group(6) == "1", st.group(6))

    pushes = [PUSH_RE.search(l) for l in lines]
    pushes = [m for m in pushes if m]
    merges = [MERGE_RE.search(l) for l in lines]
    merges = [m for m in merges if m]
    check("只 push 了一条历史（UNDO-PARAM push 出现 1 次）",
          len(pushes) == 1, "push=%d" % len(pushes))
    check("第二次编辑走 merge（UNDO-PARAM merge 出现 1 次）",
          len(merges) == 1, "merge=%d" % len(merges))
    if pushes and merges:
        name = pushes[0].group(2)
        print("  历史名字: %s" % name)
        check("历史名字是人话（'Set ... -> ...'）",
              name.startswith("Set ") and " -> " in name, name)
        check("merge 后名字的 after 变了（before 保持不变）",
              merges[0].group(2) != name, merges[0].group(2))
    return st


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    args = ap.parse_args()
    exe = os.path.abspath(args.exe)
    if not os.path.exists(exe):
        print("未找到可执行文件，请先构建：", exe)
        return 2

    group(exe, "ssr.strength", 3, "第 1 组：Float 参数（滑条那类）")
    group(exe, "ssr.enabled", 0, "第 2 组：Bool 参数（勾选框那类）")

    print("\n结果: " + ("PASS" if not FAILS else "FAIL: " + ", ".join(FAILS)))
    return 0 if not FAILS else 1


if __name__ == "__main__":
    sys.exit(main())
