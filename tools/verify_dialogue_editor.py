"""端到端验证：对话组件的**编辑器入口**（C2 第二步的遗留补齐）。

==============================================================================
这一条在验什么
==============================================================================
C2 第二步把"对话"整条链做完了 —— 资产、播放器、系统、字幕框、时间轴
`kind=Line` 接线、97 条断言、11 组端到端。但 `editor/` 里对
`DialogueComponent` 的引用是 **0 条**：

    grep -rn "DialogueComponent" editor/    →  （空）

也就是说引擎能播对话，开发者却**连组件都挂不上** —— 只能手改 scene.json
加一段 `"dialogue"` 块。本脚本钉的就是这个缺口被补上了。

  ① Inspector 的第 13 个页签 `dialogue` 被认出来（不是 unrecognized）
  ② `INS-DLG-HEADER` / `INS-DLG-PATH` / `INS-DLG-START` 三个控件真的画了
  ③ `INS-DLG-ASSET ok=1 lines=7 choices=1 start=l1` —— 资产真的解析了
     （`ImGui::TextDisabled` 画的文字**不进日志**，所以这条必须有独立锚点）
  ④ 没有 "Cannot load this dialogue"

⚠ 不验"点 Add Component 能挂上"：那需要往窗口注入鼠标点击，而菜单是
  `BeginPopup` 弹出的、时序不稳。那一半的判据是**代码可达性**（菜单项存在、
  走 `structuralEdit`），由 §308 的实现本身保证；能自动验的是"组件在场景里
  时页签画得出来"，那正是这条脚本做的。

==============================================================================
为什么必须显式打开 Inspector
==============================================================================
`MYVK_EDITOR_PANELS=inspector` 不能省：面板出厂**全关**，不打开的话
`drawTabDialogue` 一帧都不会跑，三个锚点一个都不会出现 —— 而编辑器看起来
一切正常（场景加载了、日志没有错）。

`MYVK_LOG_RECTS=1` 同样不能省：`ui::logRect()` 在没设这个变量时**直接
return**。症状是"全 False 但落盘是好的"，且等待会白等满超时。

用法：
    python tools/verify_dialogue_editor.py
"""

import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BIN = os.path.join(ROOT, "build", "bin")
EDITOR_EXE = os.path.join(BIN, "Editor.exe")

SCENE = "assets/scenes/cutscene_demo.json"
SELECT = "Director"          # 场景里挂了 dialogue 组件的那个实体
TAB = "dialogue"
EXPECT_LINES = 7             # assets/dialogues/ch01_open_01.dlg.json 的 lines 数
EXPECT_CHOICES = 1           # 其中带 options 的一行（q1）
EXPECT_START = "l1"

# 让编辑器自己退出。不认这个变量的话下面就得靠 kill，而"日志截到哪一帧"
# 会随机器性能浮动（锚点还没画出来就被杀了 = 假红）。
FRAMES = 180

results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print("[%s] %s  %s" % ("PASS" if ok else "FAIL", name, detail))
    return ok


def run_editor(wait_s=60.0):
    env = dict(os.environ)
    env["MYVK_EDITOR_SCENE"] = SCENE
    env["MYVK_EDITOR_SELECT"] = SELECT
    env["MYVK_EDITOR_INSP_TAB"] = TAB
    env["MYVK_EDITOR_PANELS"] = "inspector"
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_GRID"] = "0"
    env["MYVK_FRAMES"] = str(FRAMES)
    # 上一轮崩掉留下的编辑器实例会抢"Venn Editor"窗口标题，让这一轮找不到
    # 自己的窗口 —— 先清一遍（与 verify_sequencer 同一条纪律）。
    try:
        subprocess.run(["taskkill", "/F", "/IM", "Editor.exe"],
                       capture_output=True, timeout=10)
    except Exception:
        pass

    proc = subprocess.Popen([EDITOR_EXE], cwd=BIN, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        out, _ = proc.communicate(timeout=wait_s)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, _ = proc.communicate(timeout=20)
    return (out or b"").decode("utf-8", "replace").replace("\x00", "")


ASSET_RE = re.compile(
    r"INS-DLG-ASSET path=(\S+) ok=(\d+) lines=(\d+) choices=(\d+) start=(\S*)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=EDITOR_EXE)
    ap.add_argument("--dump", default="", help="把整份日志写到这个路径")
    args = ap.parse_args()

    print("== 对话组件的编辑器入口 ==")
    if not os.path.isfile(args.exe):
        check("0 找不到 Editor.exe", False, args.exe)
        return 1

    text = run_editor()
    if args.dump:
        with open(args.dump, "w", encoding="utf-8") as f:
            f.write(text)

    # ① 页签 id 认得出。写错 id 时编辑器会打 `INSP-TAB unrecognized "<id>"`
    #    并**静默**退回默认页签 —— 界面照常，只是不是你想看的那一页。
    bad_tab = ('unrecognized' in text and TAB in text and
               re.search(r'unrecognized[^\n]*%s' % TAB, text) is not None)
    check("① 页签 id `%s` 被认出来" % TAB, not bad_tab,
          "unrecognized 命中" if bad_tab else "ok")

    # ② 三个控件都画了（缺哪个就是哪一块没接上）
    for tag in ("INS-DLG-HEADER", "INS-DLG-PATH", "INS-DLG-START"):
        check("② 控件锚点 %s" % tag, tag in text,
              "出现" if tag in text else "缺失（drawTabDialogue 没跑到？）")

    # ③ 资产真的解析了
    assets = [m for m in (ASSET_RE.search(l) for l in text.splitlines()) if m]
    ok_asset = bool(assets) and assets[-1].group(2) == "1" and \
        int(assets[-1].group(3)) == EXPECT_LINES and \
        int(assets[-1].group(4)) == EXPECT_CHOICES and \
        assets[-1].group(5) == EXPECT_START
    check("③ 资产解析：ok=1 lines=%d choices=%d start=%s"
          % (EXPECT_LINES, EXPECT_CHOICES, EXPECT_START),
          ok_asset,
          "rows=%s" % [a.groups() for a in assets][:2])

    # ④ 没有加载失败
    check("④ 没有 Cannot load this dialogue",
          "Cannot load this dialogue" not in text, "")

    n_pass = sum(1 for _, ok, _ in results if ok)
    print("\n%d/%d PASS" % (n_pass, len(results)))
    # ⚠ 最后一行必须是 `结果: PASS` / `结果: FAIL` —— `run_regression.py` 的
    #   OK_RE 只认四种写法（`RESULT: PASS` / `全部断言 PASS` / `全部 PASS` /
    #   `结果: PASS`）。只打 "6/6 PASS" 的后果是 rc=0 却被判成 FAIL，
    #   而且失败提示里六条断言全是 PASS —— 第一眼根本看不出是**收尾行**的锅。
    if n_pass == len(results):
        print("结果: PASS")
        return 0
    print("结果: FAIL: %d 项" % (len(results) - n_pass))
    return 1


if __name__ == "__main__":
    sys.exit(main())
