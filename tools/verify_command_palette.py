"""端到端验证：E18 命令注册表 + 命令面板（Ctrl+Shift+P）。

背景：E18 之前"同一个动作"在**四个地方各写一遍**（菜单 / 工具栏 / 快捷键 /
控制台命令），于是"加了动作但忘了加菜单"就变成"功能存在、用户找不到"，
而且没有任何机制能回答"这个编辑器一共有多少可执行动作"。

本脚本验证四组：

  1. **注册表非空且成规模**：`CMD-REGISTRY count=N`（N 应 >= 50 ——
     32 个特性开关 + 8 个面板 + 8 种视图模式 + 6 套工作区 + 控制台命令
     + 固有动作）。
  2. **打开面板 + 模糊搜索**：`MYVK_EDITOR_CMDPAL=ssr` 打开面板并预填查询，
     断言 `CMD-PALETTE open`、`query="ssr" hits>=1`，且榜首是
     `feature.toggle.ssr` / `Toggle SSR`。
  3. **执行真的生效**：加 `MYVK_EDITOR_CMDPAL_EXEC=1` → 自动执行榜首 →
     断言 `CMD-RUN id=feature.toggle.ssr via=palette` **且** `FTR-UI
     ssr.enabled`（证明 handler 真写了值、走了与 UI 相同的副作用路径）。
  4. **无匹配给"最接近"**：查询 `zzzz` → `hits=0` + `CMD-PALETTE nomatch`
     且 best 非空。

用法：
    python tools/verify_command_palette.py
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

REGISTRY_RE = re.compile(r"CMD-REGISTRY count=(\d+)")
QUERY_RE = re.compile(r'CMD-PALETTE query="([^"]*)" hits=(\d+)')
HIT_RE = re.compile(
    r'CMD-PALETTE-HIT rank=(\d+) id=(\S+) label="([^"]*)" '
    r'category=(\S+) shortcut="([^"]*)"')
RUN_RE = re.compile(r'CMD-RUN id=(\S+) label="([^"]*)" via=(\S+)')
FTRUI_RE = re.compile(r"FTR-UI (\S+)")
NOMATCH_RE = re.compile(r'CMD-PALETTE nomatch query="([^"]*)" best="([^"]*)"')

FAILS = []


def check(name, cond, detail=""):
    tag = "[PASS]" if cond else "[FAIL]"
    line = "  %s %s" % (tag, name)
    if detail != "" and (not cond or detail is not None):
        line += "  %s" % (detail,)
    print(line, flush=True)
    if not cond:
        FAILS.append(name)
    return cond


def launch(exe, env_extra, need_any, timeout=30.0):
    """起编辑器，等到 need_any 里的任意一条日志出现（或超时）。返回 tap。"""
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env.update(env_extra)
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
            if any(any(n in l for n in need_any) for l in tap.lines):
                break
        time.sleep(0.2)
    time.sleep(1.0)
    return proc, hwnd, tap


def close(proc, hwnd):
    try:
        from capture_window import user32
        if hwnd:
            user32.PostMessageW(hwnd, 0x0010, 0, 0)   # WM_CLOSE
    except Exception:
        pass
    time.sleep(0.6)
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=6)
        except subprocess.TimeoutExpired:
            proc.kill()


def lines_of(tap):
    with tap.lock:
        return list(tap.lines)


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    args = ap.parse_args()
    exe = os.path.abspath(args.exe)
    if not os.path.exists(exe):
        print("未找到可执行文件，请先构建：", exe)
        return 2

    kill_stale_editors()

    # ============ 第 1 组：注册表规模 + 打开 + 模糊搜索 ============
    print("\n== 第 1 组：注册表 + 打开面板 + 模糊搜索（query=ssr）==")
    proc, hwnd, tap = launch(
        exe, {"MYVK_EDITOR_CMDPAL": "ssr"},
        need_any=["CMD-PALETTE-HIT", "CMD-PALETTE nomatch"])
    lines = lines_of(tap)
    close(proc, hwnd)

    m = None
    for l in lines:
        mm = REGISTRY_RE.search(l)
        if mm:
            m = mm
    check("拿到 CMD-REGISTRY", m is not None)
    n_reg = int(m.group(1)) if m else 0
    check("注册表规模合理（>= 50 条命令）", n_reg >= 50, n_reg)

    check("命令面板真的打开了（CMD-PALETTE open）",
          any("CMD-PALETTE open" in l for l in lines))

    q = None
    for l in lines:
        mm = QUERY_RE.search(l)
        if mm and mm.group(1) == "ssr":
            q = mm
    check("搜索 query=ssr 有结果", q is not None and int(q.group(2)) >= 1,
          q.group(0) if q else "没等到 CMD-PALETTE query")

    hits = []
    for l in lines:
        mm = HIT_RE.search(l)
        if mm:
            hits.append({"rank": int(mm.group(1)), "id": mm.group(2),
                         "label": mm.group(3), "cat": mm.group(4),
                         "shortcut": mm.group(5)})
    ssr = [h for h in hits if h["id"] == "feature.toggle.ssr"]
    print("  候选（前几条）:")
    for h in hits[:5]:
        print("    rank=%d id=%-26s label=%-16s cat=%-8s key=%s"
              % (h["rank"], h["id"], h["label"], h["cat"], h["shortcut"]))
    check("候选里含 SSR 特性开关（feature.toggle.ssr）", bool(ssr),
          ssr[0]["label"] if ssr else "没找到")
    check("SSR 开关的 label 是 'Toggle SSR'",
          bool(ssr) and ssr[0]["label"] == "Toggle SSR",
          ssr[0]["label"] if ssr else "")
    check("SSR 排在榜首（模糊搜索把最短最贴的放第一）",
          bool(ssr) and ssr[0]["rank"] == 0,
          "rank=%d" % ssr[0]["rank"] if ssr else "")

    # ============ 第 2 组：执行（EXEC=1 → 跑榜首）============
    print("\n== 第 2 组：执行榜首 → 命令真的生效 ==")
    kill_stale_editors()
    proc, hwnd, tap = launch(
        exe, {"MYVK_EDITOR_CMDPAL": "ssr", "MYVK_EDITOR_CMDPAL_EXEC": "1"},
        need_any=["CMD-RUN", "FTR-UI"])
    lines = lines_of(tap)
    close(proc, hwnd)

    run = None
    for l in lines:
        mm = RUN_RE.search(l)
        if mm:
            run = mm
    check("拿到 CMD-RUN", run is not None)
    check("执行的是 SSR 开关（via=palette）",
          run is not None and run.group(1) == "feature.toggle.ssr" and
          run.group(3) == "palette",
          run.group(0) if run else "没有 CMD-RUN")
    check("副作用走的是 UI 同一套（FTR-UI ssr.enabled）",
          any(l.strip().endswith("FTR-UI ssr.enabled") or
              "FTR-UI ssr.enabled" in l for l in lines),
          "")

    # ============ 第 3 组：无匹配 → 最接近建议 ============
    print("\n== 第 3 组：无匹配给'最接近'（query=zzzz）==")
    kill_stale_editors()
    proc, hwnd, tap = launch(
        exe, {"MYVK_EDITOR_CMDPAL": "zzzz"},
        need_any=["CMD-PALETTE nomatch"])
    lines = lines_of(tap)
    close(proc, hwnd)

    q = None
    for l in lines:
        mm = QUERY_RE.search(l)
        if mm and mm.group(1) == "zzzz":
            q = mm
    check("query=zzzz hits=0", q is not None and int(q.group(2)) == 0,
          q.group(0) if q else "没等到 CMD-PALETTE query")
    nm = None
    for l in lines:
        mm = NOMATCH_RE.search(l)
        if mm:
            nm = mm
    check("拿到 CMD-PALETTE nomatch 且给了 best 建议",
          nm is not None and nm.group(2) != "",
          nm.group(0) if nm else "没等到 nomatch")
    check("无匹配时没有候选行",
          not any(HIT_RE.search(l) for l in lines))

    print("\n结果: " + ("PASS" if not FAILS else "FAIL: " + ", ".join(FAILS)))
    return 0 if not FAILS else 1


if __name__ == "__main__":
    sys.exit(main())
