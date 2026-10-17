"""E03 验证：设置层级与持久化（5 层覆盖链 + 落盘 + 坏文件回落）。

`docs/editor_roadmap.md` §2 的 E03 要的是"渲染设置在 5 个作用域里可覆盖、
可继承、可落盘"。5 个作用域从**最泛**到**最具体**：

    EditorPrefs → Project → Scene → Object → Camera

读的时候从最具体往最泛回退；一层都没有 = 出厂默认。这份脚本把它拆成
可以机器判定的几件事：

  A. **纯数据自检**（`MYVK_RENDER_SETTINGS_TEST=1`，无窗口无 Vulkan）
     A1 退出码 0；A2 `SET-TOTAL pass=N fail=0`
     —— 这一组覆盖 5 层优先级、解析、未知键/类型拒绝、应用到绑定、
        只抓非默认、落盘往返、坏文件不崩。全部是纯内存逻辑，秒级。

  B. **无文件 = 零动作**（硬约束：与 E03 之前逐像素一致）
     B1 全程**一条 `RENDER-SET` 日志都没有**（日志本身就是行为变化）
     B2 `SHADOW-DBG cascades=4`（出厂默认未被改动）

  C. **合法 json 端到端生效**
     C1 `RENDER-SET layer=project values=1 rejected=0`（层被读到了）
     C2 `RENDER-SET applied=shadow.cascades value=2 from=project`
     C3 ⚠ 真正落到渲染：`SHADOW-DBG cascades=2`
     C4 ⚠ **文件路径与 env 路径写的是同一份存储**：同一份 json 与
        `MYVK_SHADOW_CASCADES=2` 两次运行的 `splits=` 段**逐字符相同**
        （这是"文件层不是影子结构体"的证据）

  D. **非法 json → WARN + 回落默认 + 不崩**（验收条款原话）
     D1 有一条 `RENDER-SET ... 解析失败`
     D2 `SHADOW-DBG cascades=4`
     D3 退出码 0

  E. **键级拒绝**（未知参数 / 类型不符）
     E1 两条 WARN 点名
     E2 被拒的值**不生效**（cascades 仍是 4）
     E3 退出码 0 —— 一条坏键不该毁掉整份文件之外的东西

  F. **回退顺序**（能落到磁盘的两层：prefs < project）
     F1 两层同时给 → `from=project`（更具体的赢）
     F2 只给 prefs → `from=prefs`
     —— Scene/Object/Camera 三层没有磁盘入口，由 A 组的自检覆盖。

  G. **自动化守卫**（这条守卫在保护所有像素基线）
     G1 默认路径 `config/render.json` 存在时，`MYVK_FRAMES` 跑**不读**它

  J. **示例文件自洽** —— `config/render.example.json` 必须真能被解析且
     每个键都生效（文档里的例子不能是假的）

  I. **UI 面**（五件套第③件的机器检查）
     I1 `MYVK_LOG_RECTS=1` 下能打出 `SET-EXPORT` / `SET-RELOAD` 两个矩形
     I2 两个矩形都落在客户区内 —— "控件被挤到面板折叠区外面"不算交付

⚠ 本脚本不验证"用户改参数 → 自动落盘"那条链路：自动化运行**禁止写**
  （与 EditorLayoutIO 同一条规则），所以那条链路由 A 组的
  `E1_save_ok` / `E2` / `E3`（落盘往返 + 逐位相同）覆盖写的一半，
  由 C 组覆盖读的一半。

用法：
    python tools/verify_settings_layers.py
"""

import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN_DIR = os.path.join(ROOT, "build", "bin")
EDITOR_EXE = os.path.join(BIN_DIR, "Editor.exe")

# 默认路径（与 render/RenderSettingsIO.cpp 的 kDefaultProjectPath 对齐）
DEFAULT_PROJECT_REL = os.path.join("config", "render.json")

TOTAL_RE = re.compile(r"SET-TOTAL pass=(\d+) fail=(\d+)")
SET_TEST_RE = re.compile(r"SET-TEST (\S+) result=(FAIL)")
APPLIED_RE = re.compile(r"RENDER-SET applied=(\S+) value=(\S+) from=(\S+)")
LAYER_RE = re.compile(r"RENDER-SET layer=(\S+) source='([^']*)' values=(\d+) "
                      r"rejected=(\d+)")
CHAIN_RE = re.compile(r"RENDER-SET chain=(\S+) total=(\d+)")
CASC_RE = re.compile(r"SHADOW-DBG cascades=(\d+)")
SPLITS_RE = re.compile(r"splits=(\S+)")
WIN_RE = re.compile(r"Window created: .*?\((\d+) x (\d+)\)")
RECT_RE = re.compile(r"(SET-EXPORT|SET-RELOAD)=\(([\d.]+),([\d.]+)\)-"
                     r"\(([\d.]+),([\d.]+)\)")

# A 组自检的断言条数。改 RenderSettingsIO.cpp 的 runSettingsSelfTest()
# 增删断言时必须同步改这里 —— 这个数字就是"有没有用例被静默跳过"的锚点。
SELFTEST_EXPECT = 20

DEFAULT_CASCADES = 4


def clean_env(**extra):
    """剥掉全部 `MYVK_*` 再叠 extra —— 测试的输入必须自述。"""
    env = {k: v for k, v in os.environ.items() if not k.startswith("MYVK_")}
    env.update({k: str(v) for k, v in extra.items()})
    return env


def run(exe, env_extra=None, timeout=240):
    p = subprocess.run([exe], cwd=BIN_DIR, env=clean_env(**(env_extra or {})),
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       timeout=timeout)
    return p.returncode, p.stdout.decode("utf-8", errors="replace")


def editor(**env_extra):
    """跑一次编辑器。`MYVK_FRAMES` 同时也是"自动化"的判定条件。"""
    env_extra.setdefault("MYVK_FRAMES", 40)
    env_extra.setdefault("MYVK_SHADOW_DBG", 1)   # 让 cascades 可观测
    return run(EDITOR_EXE, env_extra)


def write_json(path, body):
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(body)


def main():
    if not os.path.isfile(EDITOR_EXE):
        print("找不到可执行文件:", EDITOR_EXE)
        return 2

    fails = []

    def check(name, ok, detail=""):
        print("  [%s] %s%s" % ("OK  " if ok else "FAIL", name,
                              ("  <- " + detail) if detail else ""))
        if not ok:
            fails.append(name)

    print("=" * 68)
    print("E03 · 设置层级与持久化（5 层覆盖链）")
    print("  Editor.exe : %s" % EDITOR_EXE)
    print("=" * 68)

    tmpdir = tempfile.mkdtemp(prefix="venn_settings_")
    try:
        # ------------------------------------------------------ A. 自检
        print("\n--- A. 纯数据自检（MYVK_RENDER_SETTINGS_TEST=1，无窗口）---")
        rc, txt = run(EDITOR_EXE, {"MYVK_RENDER_SETTINGS_TEST": 1,
                                   "MYVK_NO_WINDOW_SAVE": 1})
        check("A1 退出码 0", rc == 0, "rc=%d" % rc)

        m = TOTAL_RE.search(txt)
        if not m:
            check("A2 出现 SET-TOTAL", False, "没找到 SET-TOTAL 行")
        else:
            got_p, got_f = int(m.group(1)), int(m.group(2))
            check("A2 SET-TOTAL pass=%d fail=0" % SELFTEST_EXPECT,
                  got_p == SELFTEST_EXPECT and got_f == 0,
                  "actual pass=%d fail=%d" % (got_p, got_f))

        bad = SET_TEST_RE.findall(txt)
        check("A3 没有任何 SET-TEST result=FAIL", not bad,
              str(bad[:5]) if bad else "")

        # ------------------------------------------------------ B. 无文件零动作
        print("\n--- B. 无 config/render.json → 零行为变化 ---")
        rc, txt = editor()
        check("B1 一条 RENDER-SET 日志都没有（无文件 = 零动作）",
              "RENDER-SET" not in txt,
              "找到 %d 条" % txt.count("RENDER-SET"))
        mc = CASC_RE.search(txt)
        check("B2 级联数仍是出厂的 %d" % DEFAULT_CASCADES,
              bool(mc) and int(mc.group(1)) == DEFAULT_CASCADES,
              (mc.group(0) if mc else "没有 SHADOW-DBG 行") + " rc=%d" % rc)

        # ------------------------------------------------------ C. 合法 json
        print("\n--- C. 合法 json 端到端生效 ---")
        good = os.path.join(tmpdir, "render_good.json")
        write_json(good, '{\n  "version": 1,\n  "shadow.cascades": 2\n}\n')
        rc_good, txt_good = editor(MYVK_RENDER_SETTINGS=good)

        ml = LAYER_RE.search(txt_good)
        check("C1 layer=project values=1 rejected=0",
              bool(ml) and ml.group(1) == "project"
              and int(ml.group(3)) == 1 and int(ml.group(4)) == 0,
              ml.group(0) if ml else "没有 RENDER-SET layer= 行")

        ma = None
        for cand in APPLIED_RE.finditer(txt_good):
            if cand.group(1) == "shadow.cascades":
                ma = cand
                break
        check("C2 applied=shadow.cascades value=2 from=project",
              bool(ma) and ma.group(2) in ("2", "2.0")
              and ma.group(3) == "project",
              ma.group(0) if ma else "没有 shadow.cascades 的 applied 行")

        mc = CASC_RE.search(txt_good)
        check("C3 落到渲染：SHADOW-DBG cascades=2",
              bool(mc) and int(mc.group(1)) == 2,
              mc.group(0) if mc else "没有 SHADOW-DBG 行")

        # C4：文件层与 env 层写同一份存储 → 级联分割点必须逐字符相同
        rc_env, txt_env = editor(MYVK_SHADOW_CASCADES=2)
        ms_file = SPLITS_RE.search(txt_good)
        ms_env = SPLITS_RE.search(txt_env)
        check("C4 文件路径与 env 路径的 splits 逐字符相同",
              bool(ms_file) and bool(ms_env)
              and ms_file.group(1) == ms_env.group(1),
              "file=%s env=%s" % (ms_file.group(1) if ms_file else "?",
                                  ms_env.group(1) if ms_env else "?"))

        # ------------------------------------------------------ D. 非法 json
        print("\n--- D. 非法 json → WARN + 回落默认 + 不崩 ---")
        bad_json = os.path.join(tmpdir, "render_broken.json")
        write_json(bad_json, '{ "shadow.cascades": }\n')
        rc, txt = editor(MYVK_RENDER_SETTINGS=bad_json)
        check("D1 有一条『解析失败』WARN", "解析失败" in txt,
              "解析失败 出现 %d 次" % txt.count("解析失败"))
        mc = CASC_RE.search(txt)
        check("D2 回落到出厂 %d" % DEFAULT_CASCADES,
              bool(mc) and int(mc.group(1)) == DEFAULT_CASCADES,
              mc.group(0) if mc else "没有 SHADOW-DBG 行")
        check("D3 退出码 0（不崩）", rc == 0, "rc=%d" % rc)
        check("D4 坏文件不建立覆盖层", "RENDER-SET chain=" not in txt,
              "chain= 行出现 %d 次" % txt.count("RENDER-SET chain="))

        # ------------------------------------------------------ E. 键级拒绝
        print("\n--- E. 未知键 / 类型不符 → 只拒那一条 ---")
        mixed = os.path.join(tmpdir, "render_mixed.json")
        write_json(mixed,
                   '{\n'
                   '  "no.such.param": 1,\n'
                   '  "shadow.cascades": [1, 2],\n'
                   '  "ssao.enabled": true\n'
                   '}\n')
        rc, txt = editor(MYVK_RENDER_SETTINGS=mixed)
        check("E1 未知键被点名", "未知参数" in txt,
              "未知参数 出现 %d 次" % txt.count("未知参数"))
        check("E2 类型不符被点名", "值被拒" in txt,
              "值被拒 出现 %d 次" % txt.count("值被拒"))
        mc = CASC_RE.search(txt)
        check("E3 被拒的值不生效（cascades 仍是 %d）" % DEFAULT_CASCADES,
              bool(mc) and int(mc.group(1)) == DEFAULT_CASCADES,
              mc.group(0) if mc else "没有 SHADOW-DBG 行")
        check("E4 同一份文件里合法的键照常生效",
              'applied=ssao.enabled value=true' in txt,
              "applied 行=%s" % ([
                  m.group(0) for m in APPLIED_RE.finditer(txt)] or "无"))
        check("E5 退出码 0", rc == 0, "rc=%d" % rc)

        # ------------------------------------------------------ F. 回退顺序
        print("\n--- F. 回退顺序：prefs < project ---")
        prefs = os.path.join(tmpdir, "render_prefs.json")
        write_json(prefs, '{ "shadow.cascades": 3 }\n')

        rc, txt = editor(MYVK_RENDER_PREFS=prefs, MYVK_RENDER_SETTINGS=good)
        froms = {m.group(1): m.group(3) for m in APPLIED_RE.finditer(txt)}
        check("F1 两层都给了 → 更具体的 project 赢",
              froms.get("shadow.cascades") == "project",
              "from=%s" % froms.get("shadow.cascades"))

        rc, txt = editor(MYVK_RENDER_PREFS=prefs)
        froms = {m.group(1): m.group(3) for m in APPLIED_RE.finditer(txt)}
        mc = CASC_RE.search(txt)
        check("F2 只有 prefs → from=prefs 且 cascades=3",
              froms.get("shadow.cascades") == "prefs"
              and bool(mc) and int(mc.group(1)) == 3,
              "from=%s cascades=%s" % (froms.get("shadow.cascades"),
                                       mc.group(1) if mc else "?"))

        # ------------------------------------------------------ G. 自动化守卫
        print("\n--- G. 自动化守卫：默认路径不读 ---")
        default_path = os.path.join(BIN_DIR, DEFAULT_PROJECT_REL)
        existed = os.path.isfile(default_path)
        if existed:
            check("G0 默认路径本来就存在（跳过本条以免误删用户文件）", True,
                  "已存在，不覆盖")
        else:
            write_json(default_path, '{ "shadow.cascades": 2 }\n')
            try:
                rc, txt = editor()
                mc = CASC_RE.search(txt)
                check("G1 MYVK_FRAMES 下不读默认路径（cascades 仍是 %d）"
                      % DEFAULT_CASCADES,
                      "RENDER-SET" not in txt and bool(mc)
                      and int(mc.group(1)) == DEFAULT_CASCADES,
                      "cascades=%s RENDER-SET=%d"
                      % (mc.group(1) if mc else "?", txt.count("RENDER-SET")))
            finally:
                try:
                    os.remove(default_path)
                except OSError:
                    pass
            check("G2 临时文件已清理", not os.path.isfile(default_path),
                  default_path)

        # ------------------------------------------------------ H. 不污染安装目录
        print("\n--- H. 安装目录不新增文件 ---")
        leftovers = [p for p in os.listdir(os.path.join(BIN_DIR, "config"))
                     if p.startswith("render")] if os.path.isdir(
                         os.path.join(BIN_DIR, "config")) else []
        check("H1 build/bin/config 下没有测试残留的 render*.json",
              not leftovers, str(leftovers))

        # ------------------------------------------------------ J. 示例文件
        print("\n--- J. 仓库里的示例文件本身必须是能用的 ---")
        example = os.path.join(ROOT, "config", "render.example.json")
        if not os.path.isfile(example):
            check("J0 config/render.example.json 存在", False, example)
        else:
            rc, txt = editor(MYVK_RENDER_SETTINGS=example)
            ml = LAYER_RE.search(txt)
            check("J1 示例文件被完整解析（rejected=0，_ 开头的键被忽略）",
                  bool(ml) and int(ml.group(4)) == 0,
                  ml.group(0) if ml else "没有 RENDER-SET layer= 行")
            applied_ids = {m.group(1) for m in APPLIED_RE.finditer(txt)}
            want = {"shadow.enabled", "shadow.cascades", "post.tonemap",
                    "post.exposure", "cg.lift", "pom.fade"}
            check("J2 示例里列出的每个键都真的生效",
                  want <= applied_ids,
                  "缺=%s" % sorted(want - applied_ids))

        # ------------------------------------------------------ I. UI 面
        print("\n--- I. UI 面（Engine 面板的两个按钮真的被画出来）---")
        rc, txt = editor(MYVK_ENGINE_PANEL=1, MYVK_LOG_RECTS=1)
        mw = WIN_RE.search(txt)
        win_w, win_h = (int(mw.group(1)), int(mw.group(2))) if mw else (0, 0)
        # logRect 只在矩形变化时打，所以同一个 tag 取**最后一条**是真值
        rects = {}
        for mm in RECT_RE.finditer(txt):
            rects[mm.group(1)] = tuple(float(x) for x in mm.groups()[1:])
        check("I1 两个按钮都打出了矩形（控件存在且已布局）",
              len(rects) == 2, "矩形=%s" % sorted(rects))

        ok_inside = bool(rects) and win_w > 0 and win_h > 0
        detail = []
        for tag, (x0, y0, x1, y1) in sorted(rects.items()):
            inside = (0 <= x0 < x1 <= win_w) and (0 <= y0 < y1 <= win_h)
            ok_inside = ok_inside and inside
            detail.append("%s=(%g,%g)-(%g,%g)" % (tag, x0, y0, x1, y1))
        check("I2 两个按钮都在客户区内（不是被挤到折叠区外）",
              ok_inside,
              ("win=%dx%d " % (win_w, win_h)) + " ".join(detail))

    finally:
        for name in os.listdir(tmpdir):
            try:
                os.remove(os.path.join(tmpdir, name))
            except OSError:
                pass
        try:
            os.rmdir(tmpdir)
        except OSError:
            pass

    # ---------------------------------------------------------- 结论
    print("\n" + "=" * 68)
    if fails:
        print("结果: FAIL: %d 项未通过 -> %s" % (len(fails), fails))
        return 1
    print("结果: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
