#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/verify_launcher_display.py —— 启动器 M4 验收：画面设置真的生效

路线图 §7-M4 的验收条款：
  「改分辨率后 `Game.exe` 窗口客户区尺寸；改画质后渲染截图**显著变化像素
    占比** > 0.5% 且 > 10× 基线。」

这一条针对的是路线图 P4：`game.json` 里的 `quality` 过去是**空挂字段**
（读进来、被 env 覆盖、然后只打一行日志）——"选了 Cinematic 画面不变，
也没有任何提示"。现在画质走 `config/render.json`，这个脚本负责证明它
**真的进了渲染管线**，而不只是文件里有那几个键。

覆盖（R1~R9）：
  R1  高分辨率工程 → `Game.exe` 客户区 == 1920x1080
  R2  低分辨率工程 → `Game.exe` 客户区 == 1280x720
      （**必须两档**：只测一档的话，窗口默认值恰好等于那一档就分不出
        "生效了"和"根本没读" —— 这是最典型的假绿形态）
  R3  `game.json` 里没有废弃的 `quality` 键
  R4  `config/render.json` 被运行时读到（日志 `layer=Project`）
  R5  High 与 Low 的渲染截图**显著变化像素占比** > 0.5%
  R6  同上且 > 10× 基线
  R7  基线本身 <= 0.3%（否则画面本身不确定，R5 的数字没有意义）
  R8  截图不是全黑/全白（`mean` 落在 2..253）
  R9  三次运行都正常退出 rc == 0

⚠ 为什么必须带基线：两次启动窗口会偏 1~2 px，整图位移能把"占比"顶到
  很高 —— 那样断言只证明"两张图不一样"，不证明"画质变了"。

⚠ 基线**不是 0**，实测 **0.158%**（同配置跑两次）。原因：High 档带 TAA，
  它按帧抖动采样点 —— 两次运行落在不同帧号，边缘像素就会有零点几像素的
  差。所以 R7 的上限是 **0.3%** 而不是 0.05%：
    · 它必须**明显小于** R5 的目标值（0.5%），否则"控制组"和"实验组"
      的量级一样，判据就没有分辨力；
    · 它也不能写 0 —— 那等于要求"带 TAA 的画面两次逐像素相同"，
      永远为假，只会把真 bug 淹在假红里。
  主判据是 **10 倍**（R6）而不是绝对上限：绝对上限只用来兜住"画面本身
  崩了/糊了"这种极端情况。

用法：
    python tools/verify_launcher_display.py
"""

import json
import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import launcher_kit as kit  # noqa: E402

PIX_TARGET_MIN = 0.005     # R5：0.5%
PIX_CONTROL_MAX = 0.003    # R7：0.3%（实测基线 ~0.16%，见下面 R7 的注释）
PIX_FACTOR_MIN = 10.0      # R6：10 倍基线
DIFF_THRESHOLD = 8         # 逐通道差值阈值（0..255）


# ---------------------------------------------------------------- 像素判据

def significant_ratio(img_a, img_b):
    """显著变化像素占比；尺寸不同返回 None。

    ⚠⚠ **别用"整图平均差"**：角色只占屏 4%，"站着 vs 跪着"的平均差只有
    0.41/255 —— 阈值 0.5 直接假红。这个项目已经被这条坑过两次。
    """
    from PIL import ImageChops
    if img_a is None or img_b is None or img_a.size != img_b.size:
        return None
    diff = ImageChops.difference(img_a.convert("RGB"), img_b.convert("RGB"))
    hist = diff.convert("L").histogram()
    total = img_a.size[0] * img_a.size[1]
    return sum(hist[DIFF_THRESHOLD + 1:]) / float(total)


def mean_abs(img):
    """全黑/全白检测：平均亮度落在 2..253 之外就认为"没画出东西"。"""
    import numpy as np
    return float(np.asarray(img.convert("L"), dtype=np.float64).mean())


# ---------------------------------------------------------------- 运行

def run_game(root, tag, frames=300):
    """跑一次 Game.exe，返回 (截图 或 None, rc, 日志文本, 客户区宽高)。

    ⚠ 用 **argv `--frames`** 而不是 `MYVK_FRAMES`：
      `renderSettingsAutomationRun()` 一旦看到 `MYVK_FRAMES` 就会**跳过**
      项目层 `render.json`（除非显式给了 `MYVK_RENDER_SETTINGS`）。
      那就变成"测了一条被关掉的路径"，而日志里一个字都不会说。
      （`Game.exe` 认 argv，`Editor.exe` 不认 —— 见记忆里那条。）
    """
    env = kit.clean_env({
        "MYVK_ASSET_DIR": os.path.join(root, "assets"),
        "MYVK_HIDE_UI": "1",        # HUD 的 FPS 每帧不同，会污染像素对比
        "MYVK_RUNTIME_AUDIO": "0",  # 无声卡环境别让音频初始化拖时间
    })
    proc = subprocess.Popen(
        [kit.GAME, "--frames", str(frames)], cwd=root, env=env,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, encoding="utf-8", errors="replace", bufsize=1)
    tap = kit.LogTap()
    tap.start(proc)

    title = os.path.basename(os.path.normpath(root))
    img = None
    size = (0, 0)
    hwnd, _ = kit.wait_title(title, timeout=60.0)
    if hwnd:
        time.sleep(1.2)          # 等预热：着色器变体 / 纹理上传
        size = client_size(hwnd)
        shot = os.path.join(kit.WORK_ROOT, "launcher_display",
                            "%s_%s.png" % (title, tag))
        if kit.capture(hwnd, shot):
            from PIL import Image
            img = Image.open(shot).convert("RGB")
    try:
        proc.wait(timeout=180)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=10)
    return img, proc.returncode, tap.text(), size


def client_size(hwnd):
    """窗口客户区尺寸（物理像素）。"""
    import ctypes
    import ctypes.wintypes as wt
    u = ctypes.windll.user32
    u.GetClientRect.argtypes = [ctypes.c_void_p, ctypes.POINTER(wt.RECT)]
    r = wt.RECT()
    u.GetClientRect(ctypes.c_void_p(hwnd), ctypes.byref(r))
    return r.right, r.bottom


def make_one(work, name, res_index, quality_index, type_index=1):
    """用**真向导**建一个工程（附带分辨率/画质初值）。返回失败描述列表。"""
    recent = os.path.join(work, "recent.json")
    proc, tap, hwnd = kit.launch(
        recent_path=recent,
        env_extra={
            "MYVK_LAUNCHER_PAGE": "new",
            "MYVK_LAUNCHER_TEST_ROOT": work,
            "MYVK_LAUNCHER_TEST_NAME": name,
            "MYVK_LAUNCHER_TEST_RES": str(res_index),
            "MYVK_LAUNCHER_TEST_QUALITY": str(quality_index),
        })
    if not hwnd:
        proc.kill()
        return ["启动器窗口没出现（建 %s 失败）" % name]
    try:
        return kit.drive_wizard(hwnd, tap, type_index=type_index)
    finally:
        # 需求 A（2026-10-22）：创建成功即自动拉起编辑器。本用例只借向导
        # 建工程，那个编辑器是副作用 —— 三个工程就是三个窗口，不关掉的话
        # `run_game` 的 `find_window` 会被它们干扰。
        kit.dismiss_spawned(tap)
        kit.shutdown(proc, hwnd)
        if proc.poll() is None:
            proc.kill()


def main():
    if not os.path.isfile(kit.LAUNCHER) or not os.path.isfile(kit.GAME):
        kit.log("结果: FAIL: Launcher.exe / Game.exe 不存在（先 make build）")
        return 1

    work = os.path.join(kit.WORK_ROOT, "launcher_display")
    work = kit.fresh_dir(work)
    os.makedirs(work, exist_ok=True)
    kit.log("PROBE work=%s" % work)

    fails = []

    # ---- 三个工程：高分辨率 / 低分辨率 + High / 低分辨率 + Low ----
    # 画质对比时**分辨率必须相同**，否则"占比"里混进了分辨率的差异。
    specs = [
        ("HiRes", 1, 2),    # 1920x1080 + High
        ("LoRes", 0, 2),    # 1280x720  + High
        ("LoQual", 0, 0),   # 1280x720  + Low
    ]
    for name, res, qual in specs:
        f = make_one(work, name, res, qual)
        if f:
            fails += ["建 %s 失败: %s" % (name, x) for x in f]
    if fails:
        return kit.finish(fails)
    kit.log("CHECK 三个工程已建好（HiRes / LoRes / LoQual）")

    hi = os.path.join(work, "HiRes")
    lo = os.path.join(work, "LoRes")
    lq = os.path.join(work, "LoQual")

    # ---- R3 废弃键不许出现 ----
    for root in (hi, lo, lq):
        with open(os.path.join(root, "assets", "game.json"), encoding="utf-8") as f:
            g = json.load(f)
        if "quality" in g:
            fails.append("R3 %s/game.json 里有废弃的 quality 键（P4 回归）"
                         % os.path.basename(root))
    if not any(x.startswith("R3") for x in fails):
        kit.log("CHECK R3 no deprecated 'quality' key OK")

    # ---- R1 1920x1080 ----
    # ⚠ 只看截图尺寸不够：截图是 `capture_client` 按客户区抓的，但抓失败
    #   时会是 None。两条一起用（`size` 来自 GetClientRect，与截图同源
    #   但独立于截图是否成功）。
    img_hi, rc1, log_hi, size_hi = run_game(hi, "res")
    if size_hi != (1920, 1080):
        fails.append("R1 HiRes 客户区 %sx%s，期望 1920x1080" % size_hi)
    elif img_hi is not None and img_hi.size != (1920, 1080):
        fails.append("R1 HiRes 截图 %sx%s 与客户区不一致"
                     % img_hi.size)
    else:
        kit.log("CHECK R1 HiRes client=1920x1080 OK")

    # ---- R2 1280x720 ----
    img_hi_lo, rc2, log_lo, size_lo = run_game(lo, "high_a")
    if size_lo != (1280, 720):
        fails.append("R2 LoRes 客户区 %sx%s，期望 1280x720" % size_lo)
    else:
        kit.log("CHECK R2 LoRes client=1280x720 OK")

    # ---- R3 项目层真的被读到 ----
    for tag, lt in (("HiRes", log_hi), ("LoRes", log_lo)):
        if "RENDER-SET" not in lt:
            fails.append("R4 %s 的日志里没有 RENDER-SET（render.json 没被读）" % tag)
    if not any(x.startswith("R4") for x in fails):
        # 提取一行给人看（层数 + 键数）
        line = next((l for l in log_lo.splitlines() if "RENDER-SET chain=" in l),
                    "(chain 行没找到)")
        kit.log("CHECK R4 %s OK" % line.split("] ", 1)[-1])

    # ---- R5/R6/R7 画质真的改变像素 ----
    img_low, rc3, log_lq, size_lq = run_game(lq, "low")
    img_hi_lo2, rc4, log_lo2, _ = run_game(lo, "high_b")   # 基线的另一半

    if img_hi_lo is None or img_low is None or img_hi_lo2 is None:
        fails.append("R5 截图不可用（HiRes/LoQual/基线里有 None）")
    elif img_hi_lo.size != img_low.size or img_hi_lo.size != img_hi_lo2.size:
        fails.append("R5 三张截图尺寸不一致：%s / %s / %s"
                     % (img_hi_lo.size, img_low.size, img_hi_lo2.size))
    else:
        # ---- R8 非全黑/全白 ----
        m_hi, m_low = mean_abs(img_hi_lo), mean_abs(img_low)
        if not (2.0 < m_hi < 253.0) or not (2.0 < m_low < 253.0):
            fails.append("R8 截图接近全黑/全白（mean=%.1f/%.1f），渲染没出画"
                         % (m_hi, m_low))
        else:
            kit.log("CHECK R8 mean=%.1f/%.1f (not blank) OK" % (m_hi, m_low))

        base = significant_ratio(img_hi_lo, img_hi_lo2)
        cand = significant_ratio(img_hi_lo, img_low)
        kit.log("CHECK R7 baseline=%.4f%%  (limit %.2f%%)"
                % (base * 100.0, PIX_CONTROL_MAX * 100.0))
        kit.log("CHECK R5 high-vs-low=%.4f%%  (limit %.2f%%)"
                % (cand * 100.0, PIX_TARGET_MIN * 100.0))
        if base > PIX_CONTROL_MAX:
            fails.append("R7 基线 %.4f%% 超过上限 %.2f%%"
                         "（画面本身不确定，High/Low 对比无意义）"
                         % (base * 100.0, PIX_CONTROL_MAX * 100.0))
        if cand < PIX_TARGET_MIN:
            fails.append("R5 画质差异只有 %.4f%%，低于 %.2f%%："
                         "render.json 里写了但没进渲染"
                         % (cand * 100.0, PIX_TARGET_MIN * 100.0))
        elif base > 0 and cand < PIX_FACTOR_MIN * base:
            fails.append("R6 画质差异 %.4f%% 不到基线的 %.0f 倍（基线 %.4f%%）"
                         "—— 只证明了'两张图不一样'"
                         % (cand * 100.0, PIX_FACTOR_MIN, base * 100.0))
        elif cand >= PIX_TARGET_MIN:
            kit.log("CHECK R6 cand/base=%.1f x OK"
                    % (cand / base if base > 0 else float("inf")))

    # ---- R9 三次运行正常退出 ----
    for tag, rc in (("HiRes", rc1), ("High-a", rc2), ("Low", rc3), ("High-b", rc4)):
        if rc != 0:
            fails.append("R9 %s 退出码 rc=%s，期望 0" % (tag, rc))
    if not any(x.startswith("R9") for x in fails):
        kit.log("CHECK R9 all runs exit rc=0 OK")

    return kit.finish(fails)


if __name__ == "__main__":
    sys.exit(main())
