"""端到端验证：剧本编辑器（#310）—— 面板上真的能改剧本、能存盘、能拦住坏剧本。

==============================================================================
这一条在验什么
==============================================================================
#308 把对话组件接进了 Inspector，但那时编辑器只能改 **4 个配置项**
（路径 / 启用 / 打字速度 / 起始节点）—— 剧本文本本身仍然要开记事本写
JSON。本脚本钉的是"正文也能在引擎里改"这件事，分六次真启动编辑器：

  A 打开：窗口画出来了（8 个控件锚点）+ 资产真解析了（7 行 / start=l1）
  B 改一句 + 加一行 + 存到临时文件：文件里能看到改动，**中文原样**
  C 删掉一句被别人指向的台词 → 校验红 + Save 被拒 + 原文件一个字节没动
  D 改 id → 全表 next/goto 自动打补丁（`patched=1`），存出来的文件是通的
  E 打开一份坏文件 → 只读（readOnly=1）+ Save 被拒 + 坏文件没被截断
  G 存到**场景正在用的那份**路径 → 场景里的组件真的重读了（reloaded=1）

==============================================================================
两个刻意的取舍
==============================================================================
① **注入的是 ASCII 文本**。`MYVK_EDITOR_DLGED_SCRIPT` 走 `std::getenv`，
   而 Windows 的窄环境块按 ANSI 代码页转换 —— 中文进去会变成 GBK 字节，
   于是断言测到的是"编码没配好"而不是"编辑器对不对"。中文是否原样，
   改用**更硬**的判据：把原文件里的中文往返一遍，逐字节比对（见 B5）。

② **不用鼠标点按钮**。控件位置随内容高度漂移（选项多一行，下面的全下移），
   而 `MYVK_EDITOR_DLGED_SCRIPT` 调的 `act*` 正是按钮调的那一批函数
   （见 DialogueEditorWindow.h 的"动作"一节）—— 走同一条路，验的是真行为。
   矩形锚点仍然验一遍：它证明"面板真的画在屏幕上"，而不是只跑了逻辑。

用法：
    python tools/verify_dialogue_script.py
    python tools/verify_dialogue_script.py --only b     # 只跑某个用例
    python tools/verify_dialogue_script.py --keep       # 留下临时目录
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BIN = os.path.join(ROOT, "build", "bin")
EDITOR_EXE = os.path.join(BIN, "Editor.exe")

# 被编辑的资产（相对 assets/）。7 行、start=l1、q1 带两个选项。
ASSET = "dialogues/ch01_open_01.dlg.json"
ASSET_ABS = os.path.join(BIN, "assets", ASSET)
SCENE = "assets/scenes/cutscene_demo.json"   # Director 上挂着这份剧本

EXPECT_LINES = 7          # 原文件行数
EXPECT_START = "l1"

# 让编辑器自己退出（不认这个变量就得 kill，而"日志截到哪一帧"随机器浮动）
FRAMES = 150

# 断言用的中文（原文件里一定有的两段）
CN_SPEAKER = "千夏"
CN_TEXT = "又是这个梦"

results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print("[%s] %s  %s" % ("PASS" if ok else "FAIL", name, detail))
    return ok


def run_editor(extra_env, wait_s=90.0):
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"     # 不设它 ui::logRect 直接 return
    env["MYVK_GRID"] = "0"
    env["MYVK_FRAMES"] = str(FRAMES)
    # 自动化运行期间布局**不落盘**（MYVK_LOG_RECTS 存在即拒写）——
    # 否则这一轮的窗口位置会被下一个用例继承，位置断言互相干扰。
    for k, v in extra_env.items():
        if v is not None:
            env[k] = v

    # 上一轮崩掉的实例会抢窗口标题 → 先清一遍（与 verify_sequencer 同纪律）
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


def sha(path):
    with open(path, "rb") as f:
        return hashlib.sha1(f.read()).hexdigest()


def join(p):
    """给注入脚本用的路径：统一正斜杠。C++ 侧两种都吃，但日志断言里
    反斜杠容易看花眼。"""
    return p.replace("\\", "/")


# ------------------------------------------------------------------ 日志解析
OPEN_RE = re.compile(
    r"DLGED-OPEN path=(\S+) ok=(\d+) lines=(\d+) start=(\S*) choices=(\d+) "
    r"readOnly=(\d+)")
CHECK_RE = re.compile(
    r"DLGED-CHECK ok=(\d+) lines=(\d+) start=(\S*) warns=(\d+) err=(.*)$")
BLOCK_RE = re.compile(
    r"DLGED-SAVE-BLOCKED path=(\S+) reason=(\S+)(?: err=(.*))?$")
SAVE_RE = re.compile(r"DLG-SAVE path=(\S+) full=(\S+) lines=(\d+)")
RENAME_RE = re.compile(r"DLGED-RENAME old=(\S+) new=(\S+) patched=(\d+)")
SCENE_RE = re.compile(
    r"DLGED-SCENE-RELOAD path=(\S+) reloaded=(\d+) skipped=(\d+)")
NEW_RE = re.compile(r"DLGED-NEW id=(\S+) from=(\S+) total=(\d+)")


def last(regex, text):
    hits = [m for m in (regex.search(l) for l in text.splitlines()) if m]
    return hits[-1] if hits else None


def groups(m):
    return m.groups() if m else "none"


# ------------------------------------------------------------------ 临时目录
class Ctx:
    def __init__(self, tmp):
        self.tmp = tmp
        self.b_out = join(os.path.join(tmp, "b_edited.dlg.json"))
        self.d_out = join(os.path.join(tmp, "d_renamed.dlg.json"))
        self.e_bad = join(os.path.join(tmp, "e_broken.dlg.json"))
        self.f_out = join(os.path.join(tmp, "f_new.dlg.json"))


# ------------------------------------------------------------------ A 打开
def case_a(dump_path=""):
    text = run_editor({"MYVK_EDITOR_DLGED": ASSET})
    if dump_path:
        with open(dump_path, "w", encoding="utf-8") as f:
            f.write(text)

    m = last(OPEN_RE, text)
    check("A1 DLGED-OPEN ok=1 lines=%d start=%s readOnly=0" %
          (EXPECT_LINES, EXPECT_START),
          m is not None and m.group(2) == "1" and
          int(m.group(3)) == EXPECT_LINES and m.group(4) == EXPECT_START and
          m.group(6) == "0",
          "got=%s" % (groups(m),))

    check("A2 走的是 Inspector 那条请求通道（reason=edit-request）",
          "open=1 reason=edit-request" in text, "")

    m = last(CHECK_RE, text)
    check("A3 DLGED-CHECK ok=1（序列化后能原样解析回去）",
          m is not None and m.group(1) == "1" and
          int(m.group(2)) == EXPECT_LINES,
          "got=%s" % (groups(m),))

    need = ["DLGED-WIN", "DLGED-PATH", "DLGED-NODELIST", "DLGED-EDITOR",
            "DLGED-ID", "DLGED-TEXT", "DLGED-VALID", "DLGED-SAVE-BTN"]
    missing = [t for t in need if t not in text]
    check("A4 八个控件锚点都在 —— 面板真的画在屏幕上", not missing,
          "missing=%s" % missing)


# ------------------------------------------------------------------ B 编辑+存盘
def case_b(ctx):
    script = ";".join([
        "sel:1",
        "settext:edited line 2",
        "new:l9",
        "settext:appended line",
        "dump",
        "saveas:" + ctx.b_out,
    ])
    text = run_editor({"MYVK_EDITOR_DLGED": ASSET,
                       "MYVK_EDITOR_DLGED_SCRIPT": script})

    m = last(NEW_RE, text)
    check("B1 DLGED-NEW id=l9 total=%d（+Line 走得通）" % (EXPECT_LINES + 1),
          m is not None and m.group(1) == "l9" and
          int(m.group(3)) == EXPECT_LINES + 1,
          "got=%s" % (groups(m),))

    if not os.path.isfile(ctx.b_out):
        check("B2 存出来的文件存在", False, ctx.b_out)
        return
    with open(ctx.b_out, "r", encoding="utf-8") as f:
        raw = f.read()
    try:
        doc = json.loads(raw)
    except Exception as e:
        check("B2 存出来的文件是合法 JSON", False, str(e))
        return

    lines = doc.get("lines", [])
    check("B2 存出来的文件是合法 JSON，且有 %d 行" % (EXPECT_LINES + 1),
          len(lines) == EXPECT_LINES + 1, "lines=%d" % len(lines))
    check("B3 sel:1 那一句的正文真的改了",
          len(lines) > 1 and lines[1].get("text") == "edited line 2",
          "text=%r" % (lines[1].get("text") if len(lines) > 1 else None))
    check("B4 新加的那行在末尾（id=l9）",
          len(lines) > 0 and lines[-1].get("id") == "l9" and
          lines[-1].get("text") == "appended line",
          "last=%r" % (lines[-1].get("id") if lines else None))
    # 中文原样往返：这是"不丢东西"最硬的那一条（见文件头 ①）
    check("B5 中文原样落盘（%s / %s，没有 \\u 转义）" %
          (CN_SPEAKER, CN_TEXT),
          CN_SPEAKER in raw and CN_TEXT in raw and "\\u" not in raw,
          "esc=%s" % ("\\u" in raw))
    check("B6 start 没被改动", doc.get("start") == EXPECT_START,
          "start=%r" % doc.get("start"))

    m = last(CHECK_RE, text)
    check("B7 游离节点只是建议、不拦存盘（check 仍 ok=1）",
          m is not None and m.group(1) == "1", "got=%s" % (groups(m),))


# ------------------------------------------------------------------ C 坏剧本
def case_c(ctx):
    before = sha(ASSET_ABS)
    text = run_editor({"MYVK_EDITOR_DLGED": ASSET,
                       "MYVK_EDITOR_DLGED_SCRIPT": "sel:2;del;save"})

    m = last(CHECK_RE, text)
    err = m.group(5) if m else ""
    check("C1 删掉 q1 后校验变红，且点名是谁还指着它",
          m is not None and m.group(1) == "0" and "q1" in err and
          "not a line id" in err,
          "err=%r" % err)

    m = last(BLOCK_RE, text)
    check("C2 Save 被拒（DLGED-SAVE-BLOCKED）", m is not None,
          "got=%s" % (groups(m),))

    check("C3 原文件一个字节都没动", before == sha(ASSET_ABS),
          "before=%s after=%s" % (before[:8], sha(ASSET_ABS)[:8]))


# ------------------------------------------------------------------ D 改 id
def case_d(ctx):
    text = run_editor({"MYVK_EDITOR_DLGED": ASSET,
                       "MYVK_EDITOR_DLGED_SCRIPT":
                           "sel:2;setid:qfirst;saveas:" + ctx.d_out})

    m = last(RENAME_RE, text)
    # l2.next 指着 q1 → 正好一处补丁。数不对说明引用扫漏了。
    check("D1 DLGED-RENAME q1→qfirst patched=1",
          m is not None and m.group(1) == "q1" and m.group(2) == "qfirst" and
          m.group(3) == "1", "got=%s" % (groups(m),))

    if not os.path.isfile(ctx.d_out):
        check("D2 改名后的文件存在", False, ctx.d_out)
        return
    with open(ctx.d_out, "r", encoding="utf-8") as f:
        try:
            doc = json.load(f)
        except Exception as e:
            check("D2 改名后的文件能读", False, str(e))
            return
    by_id = {n.get("id"): n for n in doc.get("lines", [])}
    check("D2 改名后 l2.next 跟着变成 qfirst（没有悬空引用）",
          by_id.get("l2", {}).get("next") == "qfirst",
          "l2.next=%r" % by_id.get("l2", {}).get("next"))
    gotos = [c.get("goto") for c in by_id.get("qfirst", {}).get("choices", [])]
    check("D3 改名没碰选项的 goto（roof / sleep 还在）",
          gotos == ["roof", "sleep"], "gotos=%s" % gotos)
    m = last(CHECK_RE, text)
    check("D4 改名后校验仍然 ok=1", m is not None and m.group(1) == "1",
          "got=%s" % (groups(m),))


# ------------------------------------------------------------------ E 只读
def case_e(ctx):
    # 坏在"l1 的 next 指向不存在的 id"：解析会在校验阶段失败，但 nodes 里
    # 已经被填进前 3 条 —— 这正是必须只读的理由（存盘 = 截断）。
    bad = {
        "version": 1,
        "name": "broken",
        "start": "l1",
        "lines": [
            {"id": "l1", "speaker": CN_SPEAKER, "text": "one", "next": "missing"},
            {"id": "l2", "text": "two"},
            {"id": "l3", "text": "three"},
        ],
    }
    with open(ctx.e_bad, "w", encoding="utf-8") as f:
        json.dump(bad, f, ensure_ascii=False, indent=2)
    before = sha(ctx.e_bad)

    text = run_editor({"MYVK_EDITOR_DLGED": ctx.e_bad,
                       "MYVK_EDITOR_DLGED_SCRIPT": "save"})

    m = last(OPEN_RE, text)
    check("E1 打不开的资产：ok=0 且 readOnly=1",
          m is not None and m.group(2) == "0" and m.group(6) == "1",
          "got=%s" % (groups(m),))

    m = last(BLOCK_RE, text)
    check("E2 Save 被拒，理由是 read-only（不是 no-path / 校验）",
          m is not None and m.group(2) == "read-only",
          "got=%s" % (groups(m),))

    check("E3 坏文件没被截断式存回（内容哈希一致）", before == sha(ctx.e_bad),
          "before=%s after=%s" % (before[:8], sha(ctx.e_bad)[:8]))


# ------------------------------------------------------------------ F 从零新建
def case_f(ctx):
    """空手开窗 → New script → 存盘。

    这是"普通开发者第一次打开这个面板"最可能走的那条路（手上还没有资产），
    也是最容易漏测的一条：面板从空表起步，`l1/l2` 是**代码生成**的骨架，
    而 `l1 -> l2` 这条接线决定了"进 Play 会不会只播一句就没了"。
    """
    text = run_editor({
        # 空串 = 只请求开窗、不加载资产（等价于从 View 菜单点开）
        "MYVK_EDITOR_DLGED": "",
        "MYVK_EDITOR_DLGED_SCRIPT":
            "newscript;settext:hello from the editor;saveas:" + ctx.f_out,
    })

    check("F1 空手开窗（DLGED-OPEN path=(none)）",
          "(none)" in text and "reason=edit-request" in text, "")

    if not os.path.isfile(ctx.f_out):
        check("F2 New script 存出来的文件存在", False, ctx.f_out)
        return
    with open(ctx.f_out, "r", encoding="utf-8") as f:
        try:
            doc = json.load(f)
        except Exception as e:
            check("F2 New script 存出来的文件是合法 JSON", False, str(e))
            return
    lines = doc.get("lines", [])
    check("F2 骨架是两行、起点 l1", len(lines) == 2 and doc.get("start") == "l1",
          "lines=%d start=%r" % (len(lines), doc.get("start")))
    check("F3 骨架自带 l1->l2 接线（不然进 Play 只播一句）",
          len(lines) == 2 and lines[0].get("next") == "l2",
          "l1.next=%r" % (lines[0].get("next") if lines else None))
    m = last(CHECK_RE, text)
    check("F4 刚建出来的骨架就是可加载的（check ok=1）",
          m is not None and m.group(1) == "1", "got=%s" % (groups(m),))


# ------------------------------------------------------------------ G 场景刷新
def case_g():
    """存到场景正在用的那份 → 场景里的组件真的重读了。

    ⚠ 这一条会**临时改写仓库里的资产**，所以先把原字节读进内存，跑完立刻
      写回并在最后校验哈希。不这么做就只能验到 reloaded=0 —— 那是"没走
      匹配分支"，不是"走对了"。
    """
    with open(ASSET_ABS, "rb") as f:
        original = f.read()

    try:
        text = run_editor({"MYVK_EDITOR_SCENE": SCENE,
                           "MYVK_EDITOR_DLGED": ASSET,
                           "MYVK_EDITOR_DLGED_SCRIPT":
                               "sel:1;settext:temporary-text;save"})
    finally:
        with open(ASSET_ABS, "wb") as f:
            f.write(original)

    m = last(SAVE_RE, text)
    check("G1 DLG-SAVE 落到场景正在用的那份路径",
          m is not None and m.group(1) == ASSET and
          int(m.group(3)) == EXPECT_LINES,
          "got=%s" % (groups(m),))

    m = last(SCENE_RE, text)
    check("G2 存盘后场景里的组件重读了（reloaded=1、skipped=0）",
          m is not None and m.group(2) == "1" and m.group(3) == "0",
          "got=%s" % (groups(m),))

    check("G3 仓库里的资产已原样还原（哈希一致）",
          sha(ASSET_ABS) == hashlib.sha1(original).hexdigest(),
          "bytes=%d" % os.path.getsize(ASSET_ABS))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=EDITOR_EXE)
    ap.add_argument("--dump", default="", help="A 用例的整份日志写到这个路径")
    ap.add_argument("--keep", action="store_true", help="保留临时目录")
    ap.add_argument("--only", default="", help="只跑某个用例（a/b/c/d/e/f/g）")
    args = ap.parse_args()

    print("== 剧本编辑器（#310）==")
    if not os.path.isfile(args.exe):
        check("0 找不到 Editor.exe", False, args.exe)
        return 1
    if not os.path.isfile(ASSET_ABS):
        check("0 找不到被编辑的资产", False, ASSET_ABS)
        return 1

    tmp = tempfile.mkdtemp(prefix="venn_dlged_")
    ctx = Ctx(tmp)
    only = args.only.lower()

    try:
        if only in ("", "a"):
            case_a(args.dump)
        if only in ("", "b"):
            case_b(ctx)
        if only in ("", "c"):
            case_c(ctx)
        if only in ("", "d"):
            case_d(ctx)
        if only in ("", "e"):
            case_e(ctx)
        if only in ("", "f"):
            case_f(ctx)
        if only in ("", "g"):
            case_g()
    finally:
        if args.keep:
            print("临时目录: %s" % tmp)
        else:
            shutil.rmtree(tmp, ignore_errors=True)
        try:
            subprocess.run(["taskkill", "/F", "/IM", "Editor.exe"],
                           capture_output=True, timeout=10)
        except Exception:
            pass

    n_pass = sum(1 for _, ok, _ in results if ok)
    print("\n%d/%d PASS" % (n_pass, len(results)))
    # ⚠ 收尾行必须是 `结果: PASS` / `结果: FAIL` —— run_regression.py 的
    #   OK_RE 只认四种写法。只打 "17/17 PASS" 会 rc=0 却被判成 FAIL。
    if n_pass == len(results):
        print("结果: PASS")
        return 0
    print("结果: FAIL: %d 项" % (len(results) - n_pass))
    return 1


if __name__ == "__main__":
    sys.exit(main())
