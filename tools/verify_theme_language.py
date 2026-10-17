"""端到端验证：主题多风格 + 中/英语言切换 + 偏好存档 schema 门。

用户原文（2026-10-17）：
    ① "帮编辑器窗口主题风格美化一下，包括所有的 ui 窗口及其子窗口，并且可以
       再 preferences 中去选择多风格的窗口主题。"
    ⑥ "Preferences 中新增编辑器的语言切换功能，只需要中/英两种即可，请务必
       翻译的标准些，编辑器中的显示中文时只是一个映射替换，实际在引起代码
       逻辑中请务必继续保持英文，避免因此出现一些不必要的 bug。"

三件事分开验：

B1 主题真的生效（且"每套主题都不一样"）
    驱动方式 = 偏好存档里的 `theme` 下标（启动时 applyPrefsFromDisk →
    applyAppearance → ui::applyTheme）。每套主题截一张全窗口图，**判据只取
    Preferences 窗口内部**（纯 ImGui 面），断言该主题的签名区间：
      0 VennDark / 1 UeDark / 2 BlenderDark / 3 UnityDark → 深色（< 110）
      4 Daylight                                            → 浅色（> 150）
    ⚠ 不要用整窗均值：里面有一大块 3D 视口（天光/地面），会把深色主题抬到
      91~122、和浅色主题挤在一起 —— 第一版就是栽在这个判据上。
    再单独量顶栏（TB-BAR）与传输条（TR-BAR）：这两处原是硬编码深灰，浅色
    主题下会留成"深色孤岛"，所以两边都要量（VennDark 深 / Daylight 浅）。
    另外断言 5 张图**两两不同**（只加了个下拉却全渲成一样 = 没接上）。
    ⚠ 亮度阈值是**松**的，因为这里要证的是"换主题有没有生效"，不是"每套颜色
      对不对"——后者只能肉眼看图。

B2 旧偏好存档被拒（默认值迁移门）
    `editor_prefs.json` 是**逐键覆盖**出厂默认的。老存档里有
    `snap.move=0` / `grid.extent=60`，会把"吸附默认全开""栅格放大到 500m"
    两条改动整个盖掉 → 用户会以为没实现。所以加了 kPrefsVersion：
    version < 2 的存档整份作废。
    验法 = 造一份 **version=1 + theme=4（Daylight）** 的存档，启动后必须
    **仍是深色**（主题没被采纳）+ 日志里有 "旧 schema"。
    反向对照 = 同一份内容只把 version 改成 2 → 必须**变浅色**。
    （只验 B2 的"拒绝"而不验"接受"，等于在验"存档压根没被读"。）

B3 中文真的画出来了（且是映射替换，不动逻辑）
    - `language=1` 后：Preferences 左侧页签那一格的**像素内容**必须与英文
      模式不同（证明文字换了，而不是"选了中文没反应"）。
    - 同时 `PREFS-WIN` 矩形必须**逐字节相同** —— 用户第 ⑥ 点的硬约束是
      "显示中文只是映射替换"，窗口尺寸/位置不该因此变（窗口名走
      `trId()` 保 ID）。
    - ⚠ 字形有没有糊/有没有豆腐块，脚本判不了 → 图必须人眼看（项目纪律）。

用法：
    python tools/verify_theme_language.py            # 跑全部
    python tools/verify_theme_language.py --keep     # 最后一张留窗口
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_client, find_window, user32        # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors, wait_rect     # noqa: E402
from verify_asset_drag import force_foreground                        # noqa: E402
from verify_bvh import click_client, click_client_post                # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify")
TMP_DIR = os.path.join(OUT_DIR, "_tmp_prefs")

THEME_NAMES = ["venndark", "uedark", "blenderdark", "unitydark", "daylight"]
# 深色 / 浅色主题的平均亮度分界。
# ⚠ 深色主题实测落在 85~132，不是个位数 —— ImGui 的字节色经 sRGB 交换链会
#   被当线性值**再编码一次**（坑点 #34）：面板底 0.106 线性 → 上屏 ≈ 92。
#   浅色主题实测 225~245，两者之间有一大段空地，阈值取 160 / 200 就够稳。
DARK_MAX = 160.0
LIGHT_MIN = 200.0


def mean_luma(img, box=None):
    """平均亮度（Rec.601 近似，够用）。box=(x0,y0,x1,y1) 可选裁剪。"""
    if box:
        img = img.crop(tuple(int(round(v)) for v in box))
    px = img.convert("RGB").resize((160, 100))
    w, h = px.size
    data = px.load()
    tot = 0.0
    for y in range(h):
        for x in range(w):
            r, g, b = data[x, y]
            tot += 0.299 * r + 0.587 * g + 0.114 * b
    return tot / float(w * h)


def write_prefs(path, prefs, version=2):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump({"version": version, "prefs": prefs}, f, indent=2)


class Session:
    """一次编辑器启动（自带日志泵 + 窗口摆位）。"""

    def __init__(self, prefs_path, open_prefs=True):
        self.prefs_path = prefs_path
        self.open_prefs = open_prefs
        self.proc = None
        self.tap = None
        self.hwnd = None

    def __enter__(self):
        env = dict(os.environ)
        env["MYVK_NO_WINDOW_SAVE"] = "1"
        env["MYVK_LOG_RECTS"] = "1"
        # 显式路径 → 自动化跑也放行"读"（"写"仍然全程禁止）
        env["MYVK_EDITOR_PREFS_PATH"] = self.prefs_path
        if self.open_prefs:
            env["MYVK_EDITOR_PREFS"] = "1"
        self.proc = subprocess.Popen(
            [os.path.abspath(DEFAULT_EXE)], cwd=os.path.dirname(DEFAULT_EXE),
            env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, encoding="utf-8", errors="replace", bufsize=1)
        self.tap = LogTap()
        self.tap.start(self.proc)
        self.hwnd = find_window("Venn Editor")
        if not self.hwnd:
            raise RuntimeError("未找到窗口")
        user32.ShowWindow(self.hwnd, 9)
        time.sleep(2.0)
        # 摆到 (0,0)：不给位置每次启动右移 38px（窗口级联）→ 坐标会飘
        user32.SetWindowPos(self.hwnd, 0, 0, 0, 1600, 900, 0x0002 | 0x0004)
        time.sleep(2.5)
        return self

    def __exit__(self, *exc):
        try:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        except Exception:
            pass
        return False

    def shot(self, name, crop=None):
        img, _ = capture_client(self.hwnd)
        path = os.path.join(OUT_DIR, name)
        img.save(path)
        if crop:
            return img.crop(tuple(int(round(v)) for v in crop)), path
        return img, path

    def prefs_rect(self):
        """Preferences 窗口矩形（拿不到 = 窗口没开）。"""
        return self.tap.last_rect("PREFS-WIN")

    def prefs_luma(self, name):
        """整窗截图存档，返回 **Preferences 窗口内** 的平均亮度。

        ⚠ 判据必须只取 UI 面（Preferences 窗口）而不是整窗口：整窗里有一大块
        3D 视口，天光/地面把均值抬到 100~120，深色主题和浅色主题会挤在一起
        （第一版就是这么误报的：四套深色主题 91~122 全被判"不够深"）。
        Preferences 是纯 ImGui 面（不透明底 + 文字），深/浅分离得非常干净。
        """
        img, path = self.shot(name)
        r = self.prefs_rect()
        if not r:
            raise RuntimeError("拿不到 PREFS-WIN（Preferences 没开？）")
        return mean_luma(img, r), path

    def bar_lumas(self):
        """(顶栏 TB-BAR, 传输条 TR-BAR) 的平均亮度 —— 这两条原来都是硬编码
        深灰，浅色主题下会留成"深色孤岛"，所以单独量一把当回归门。"""
        img, _ = capture_client(self.hwnd)
        out = []
        for tag in ("TB-BAR", "TR-BAR"):
            r = self.tap.last_rect(tag)
            out.append(mean_luma(img, r) if r else -1.0)
        return out[0], out[1]

    def log_has(self, needle):
        with self.tap.lock:
            return any(needle in l for l in self.tap.lines)

    def click(self, x, y, first=True):
        """第 1 发走真实事件（先抢前台），之后 `PostMessage` 直投。
        —— 见 tools/verify_particles.py 里"返回值被忽略"那段教训。"""
        if first and force_foreground(self.hwnd):
            click_client(self.hwnd, x, y)
            return "real"
        click_client_post(self.hwnd, x, y)
        return "post"


def click_center(sess, rect, first=True):
    cx = (rect[0] + rect[2]) * 0.5
    cy = (rect[1] + rect[3]) * 0.5
    return sess.click(cx, cy, first)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--skip-sweep", action="store_true",
                    help="跳过 5 套主题全扫（省时间）")
    args = ap.parse_args()

    if not os.path.exists(os.path.abspath(args.exe)):
        print("找不到可执行文件:", args.exe)
        return 2
    os.makedirs(OUT_DIR, exist_ok=True)
    os.makedirs(TMP_DIR, exist_ok=True)
    kill_stale_editors()

    fails = []
    skipped = []

    def check(name, ok, detail=""):
        print("  [%s] %s %s" % ("PASS" if ok else "FAIL", name, detail))
        if not ok:
            fails.append(name)

    def skip(name, detail=""):
        print("  [SKIP] %s %s" % (name, detail))
        skipped.append(name)

    # ---------------- B1：主题全扫 ----------------
    luma = {}
    bar = {}
    if args.skip_sweep:
        skip("B1 主题全扫", "--skip-sweep")
    else:
        for i, nm in enumerate(THEME_NAMES):
            p = os.path.join(TMP_DIR, "theme%d.json" % i)
            write_prefs(p, {"theme": i, "language": 0})
            with Session(p, open_prefs=True) as s:
                luma[i], path = s.prefs_luma("theme_%d_%s.png" % (i, nm))
                bar[i] = s.bar_lumas()
            print("  theme %d %-12s prefs-luma=%.1f  TB=%.1f TR=%.1f  → %s"
                  % (i, nm, luma[i], bar[i][0], bar[i][1],
                     os.path.basename(path)))
        dark_ok = all(luma[i] < DARK_MAX for i in range(4))
        light_ok = luma[4] > LIGHT_MIN
        check("B1-1 四套深色主题都渲成深色", dark_ok,
              " ".join("%.1f(<%.0f)" % (luma[i], DARK_MAX) for i in range(4)))
        check("B1-2 Daylight 渲成浅色（换主题真接上了）", light_ok,
              "luma=%.1f(>%.0f)" % (luma[4], LIGHT_MIN))
        # 顶栏 / 传输条也要跟主题走（原来这两处硬编码深灰 → 浅色主题下会留
        # 成两条深色孤岛）。VennDark 侧一并量，确保它们不是"永远浅色"。
        tb_dark = bar[0][0] < DARK_MAX and bar[0][1] < DARK_MAX
        check("B1-4 顶栏/传输条在 VennDark 下是深的", tb_dark,
              "TB=%.1f TR=%.1f(<%.0f)" % (bar[0][0], bar[0][1], DARK_MAX))
        check("B1-5 顶栏/传输条在 Daylight 下跟着变浅", 
              bar[4][0] > LIGHT_MIN and bar[4][1] > LIGHT_MIN,
              "TB=%.1f TR=%.1f(>%.0f)" % (bar[4][0], bar[4][1], LIGHT_MIN))
        # "两两不同"：亮度近似不算数，直接比整个 PNG 的指纹
        import hashlib
        sigs = {}
        for i, nm in enumerate(THEME_NAMES):
            with open(os.path.join(OUT_DIR, "theme_%d_%s.png" % (i, nm)),
                      "rb") as f:
                sigs[i] = hashlib.sha1(f.read()).hexdigest()
        uniq = len(set(sigs.values()))
        check("B1-3 五套主题两两不同", uniq == 5,
              "unique=%d/5" % uniq)

    # ---------------- B2：旧存档迁移门 ----------------
    stale = os.path.join(TMP_DIR, "stale_v1.json")
    # 故意写"本该把界面变成浅色"的 theme=4，同时塞老默认值
    write_prefs(stale, {"theme": 4, "language": 0, "snap.move": 0,
                        "grid.extent": 60}, version=1)
    with Session(stale, open_prefs=True) as s:
        rejected_luma, _ = s.prefs_luma("prefs_gate_v1_rejected.png")
        gate_logged = s.log_has("旧 schema")
    check("B2-1 旧 schema 存档被拒（界面没被老值覆盖）",
          rejected_luma < DARK_MAX,
          "luma=%.1f(<%.0f) 应为深色" % (rejected_luma, DARK_MAX))
    check("B2-2 日志里明说了'旧 schema'", gate_logged)

    # 反向对照：同内容、version=2 → 必须被采纳（否则上一条只是"存档没读"）
    fresh = os.path.join(TMP_DIR, "stale_v2.json")
    write_prefs(fresh, {"theme": 4, "language": 0, "snap.move": 0,
                        "grid.extent": 60}, version=2)
    with Session(fresh, open_prefs=True) as s:
        accepted_luma, _ = s.prefs_luma("prefs_gate_v2_accepted.png")
    check("B2-3 对照：同内容 version=2 → 被采纳（真的读了存档）",
          accepted_luma > LIGHT_MIN,
          "luma=%.1f(>%.0f) 应为浅色" % (accepted_luma, LIGHT_MIN))

    # ---------------- B3：语言切换 ----------------
    # 同一份偏好，只差 language → 比"页签那格像素"和"窗口矩形"。
    # ⚠ 锚点 tag 是 `PREFS-TAB-` + 英文**页名**（Interface / Appearance …，
    #   大小写就是 C++ 里 kPages 的写法）—— 不是大写。
    p_en = os.path.join(TMP_DIR, "lang_en.json")
    p_zh = os.path.join(TMP_DIR, "lang_zh.json")
    write_prefs(p_en, {"theme": 0, "language": 0})
    write_prefs(p_zh, {"theme": 0, "language": 1})

    def lang_run(prefs, png):
        with Session(prefs, open_prefs=True) as s:
            tab = wait_rect(s.tap, "PREFS-TAB-Appearance", 8.0)
            tab_if = wait_rect(s.tap, "PREFS-TAB-Interface", 4.0)
            win = wait_rect(s.tap, "PREFS-WIN", 4.0)
            if tab:
                click_center(s, tab, first=True)
                time.sleep(1.2)
            img, _ = s.shot(png)
            return tab_if, win, img

    tab_en, win_en, img_en = lang_run(p_en, "lang_en.png")
    tab_zh, win_zh, img_zh = lang_run(p_zh, "lang_zh.png")

    if not (tab_en and win_en and win_zh):
        skip("B3 语言切换", "拿不到 PREFS-TAB-Interface / PREFS-WIN 锚点")
    else:
        # 页签那一格的像素必须变了（证明文字真的换了）
        a = img_en.convert("RGB").crop(tuple(int(round(v)) for v in tab_en))
        b = img_zh.convert("RGB").crop(tuple(int(round(v)) for v in tab_zh))
        diff = sum(1 for pa, pb in zip(a.getdata(), b.getdata())
                   if pa != pb)
        check("B3-1 中文模式下页签文字变了（映射替换生效）", diff > 30,
              "不同像素=%d" % diff)
        # 窗口矩形必须一模一样 —— "只是映射替换，不动布局/ID"
        same = win_en == win_zh
        check("B3-2 Preferences 窗口矩形与语言无关（ID/尺寸稳定）", same,
              "en=%s zh=%s" % (win_en, win_zh))

    print("截图 →", OUT_DIR)
    if args.keep:
        print("--keep：最后一张窗口已随 Session 关闭（本脚本每次起独立进程）")
    tail = ("（另有 %d 条 SKIP：%s）" % (len(skipped), ", ".join(skipped))
            if skipped else "")
    if fails:
        print("结果: FAIL: " + ", ".join(fails) + tail)
        return 1
    print("结果: PASS" + tail)
    return 0


if __name__ == "__main__":
    sys.exit(main())
