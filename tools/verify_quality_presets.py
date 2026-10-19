"""端到端验证：E26 画质档位（docs/editor_roadmap.md §6 · E26）。

背景：E09 的 Engine 面板把 32 个特性摊成一张长表，想"把画面调到能跑"
的人要逐条读一遍才知道该关谁。而启动器新建工程时已经能选
Low~Cinematic 了 —— 进编辑器之后反而没有对应入口。本任务补这一步。

交付物：
  · engine/render/QualityPresets —— enable 清单（原有）+ **数值档位**
    （shadow.cascades / shadow.distance / lshadow.maxLights）+ 平台预设
    + presetDiff（差异预览与一致性检查**共用**这一份比较逻辑）；
  · engine/render/QualityPresets::fillLayerForPreset —— 落盘的唯一写入口
    （启动器 ProjectDisplay::writeQuality 也改走它，否则两个入口写出
     两份不同的 render.json）；
  · editor/QualitySettingsPanel —— 选档 / 平台 / 差异预览 / Apply /
    Check / Save；
  · `Game.exe --selftest quality` 15 条纯数据自检。

断言分组：

  A 组（无窗口）`--selftest quality` 15 条全绿、退出码 0。
  B 组（GUI，一次启动）
    B1 面板打开（`QUAL-RECT`），5 个档位锚点都有坐标且落在面板内
    B2 点 Low → 差异预览出现（`QUAL-DIFF-0`）—— 证明"选档≠应用"
    B3 点 Apply → `QUALITY-SNAPSHOT ... ssao=0 ssr=0 taa=0 cascades=1`
       **验收原文**："切到 Low 后断言 SSR 生效值为 0"
    B4 Apply 之后自检 `mismatches=0`（应用结果与档位定义逐项一致）
    B5 点 Epic → Apply → `ssao=1 ssr=1 taa=1 lshadow=1`、cascades=4
    B6 点 Save → `QUALITY-UI save ... keys=25`，且文件内容与档位逐项一致
       （22 个 enable + 3 个数值；Low 的 cascades 是 1、Cinematic 是 4）
    B7 退出码 0、无 VUID / VALIDATION
  C 组（GUI，第二次启动）读回落盘文件 → `RENDER-SET chain=project=25`
      且 `QUALITY-UI check preset=Epic mismatches=0`（**档位能被读回来**）

用法：
    python tools/verify_quality_presets.py
"""

import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import launcher_kit as kit                                        # noqa: E402
from capture_window import find_window                             # noqa: E402
from safe_fs import safe_remove

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
EDITOR = os.path.join(BIN, "Editor.exe")
GAME = os.path.join(BIN, "Game.exe")
OUT = os.path.join(os.path.dirname(ROOT), "_venn_verify", "e26")

SNAP_RE = re.compile(
    r"QUALITY-SNAPSHOT preset=(\S+) ssao=(\d+) ssr=(\d+) taa=(\d+) "
    r"cascades=(\d+) dist=([\d.]+) lights=(\d+)")
CHECK_RE = re.compile(r"QUALITY-UI check preset=(\S+) mismatches=(\d+)")

FAILS = []


def check(name, cond, detail=""):
    kit.log("CHECK %s %s%s" % (name, "OK" if cond else "FAIL",
                               ("  " + str(detail)) if detail != "" else ""))
    if not cond:
        FAILS.append(name)


def wait_count(tap, needle, n, timeout=20.0):
    """等日志里 needle 出现次数 >= n。

    ⚠ 不能用 `has_line`：那对"前面已经出现过"永远为真（这个项目踩过）。
      计数必须在动作**之前**取。
    """
    end = time.time() + timeout
    while time.time() < end:
        if tap.count_line(needle) >= n:
            return True
        time.sleep(0.1)
    return False


def last_match(lines, rx):
    found = None
    for line in lines:
        m = rx.search(line)
        if m:
            found = m
    return found


# ---------------------------------------------------------------- A 组

def run_selftest():
    kit.log("---- A 组：--selftest quality ----")
    proc = subprocess.run([GAME, "--selftest", "quality"], cwd=BIN,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, encoding="utf-8", errors="replace",
                          timeout=120)
    out = proc.stdout or ""
    ok_lines = out.count("QUALITY OK ")
    fail_lines = out.count("QUALITY FAIL ")
    check("A1 selftest 退出码 0", proc.returncode == 0, "rc=%d" % proc.returncode)
    check("A2 15 条全过", ok_lines == 15 and fail_lines == 0,
          "ok=%d fail=%d" % (ok_lines, fail_lines))
    check("A3 汇总行 failures=0", "QUALITY-TEST end failures=0" in out)
    m = re.search(r"QUALITY-TEST begin presets=(\d+) values=(\d+) "
                  r"platforms=(\d+) enable=(\d+)", out)
    if m:
        check("A4 表规模 5 档 / 3 数值 / 5 平台 / 22 开关",
              (m.group(1), m.group(2), m.group(3)) == ("5", "3", "5"),
              "presets=%s values=%s platforms=%s enable=%s"
              % (m.group(1), m.group(2), m.group(3), m.group(4)))
    else:
        check("A4 表规模", False, "没找到 QUALITY-TEST begin 行")


# ---------------------------------------------------------------- 起编辑器

def launch_editor(env_extra, timeout=45.0):
    env = kit.clean_env(env_extra)
    proc = subprocess.Popen([EDITOR], cwd=BIN, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = kit.LogTap()
    tap.start(proc)
    hwnd = find_window(kit.EDITOR_TITLE, timeout)
    if hwnd:
        # 显式摆一个够大的窗口：面板宽 560，5 个中文单选按钮要一行放下
        # （被裁掉的控件锚点**照样有坐标**，但点下去点到别处 —— 记忆里的坑）。
        try:
            from capture_window import user32
            user32.ShowWindow(hwnd, 9)                       # SW_RESTORE
            time.sleep(1.0)
            user32.SetWindowPos(hwnd, 0, 0, 0, 1600, 1000,
                                0x0002 | 0x0004)             # NOMOVE|NOZORDER
            time.sleep(1.5)
        except Exception as e:                               # noqa: BLE001
            kit.log("WARN 窗口准备失败 %s" % e)
    return proc, tap, hwnd


def base_env(cfg_path):
    return {
        "MYVK_LOG_RECTS": "1",
        "MYVK_NO_WINDOW_SAVE": "1",     # 自动化判定为真 → layout 不落盘
        "MYVK_EDITOR_NO_HUD": "1",
        "MYVK_GRID": "0",
        "MYVK_EDITOR_PANELS": "quality",
        # 显式给路径 = 允许写（E03 的守卫：自动化 + 无显式路径时**拒绝**写）
        "MYVK_RENDER_SETTINGS": cfg_path,
        "MYVK_VALIDATION": "1",
    }


# ---------------------------------------------------------------- B 组

def run_gui_apply_and_save(work):
    kit.log("---- B 组：GUI 选档 / 应用 / 落盘 ----")
    cfg_path = os.path.join(work, "config", "render.json")
    if os.path.exists(cfg_path):
        safe_remove(cfg_path)          # 单文件删除，不触发批量保护

    proc, tap, hwnd = launch_editor(base_env(cfg_path))
    if not hwnd:
        FAILS.append("B0 编辑器窗口没出现")
        return None
    try:
        if not tap.wait_rect("QUAL-RECT", 30.0):
            FAILS.append("B1 画质面板没打开（QUAL-RECT 没出现）")
            return None
        rect = tap.last_rect("QUAL-RECT")
        check("B1 面板打开", rect is not None, "rect=%s" % (rect,))

        # ---- 5 个档位锚点都在面板内（被裁掉的控件点了也白点）----
        anchors = []
        for i in range(5):
            r = tap.wait_rect("QUAL-PRESET-%d" % i, 6.0)
            anchors.append(r)
        inside = [(r is not None and r[0] >= rect[0] - 4 and r[2] <= rect[2] + 4)
                  for r in anchors]
        check("B2 5 个档位锚点都在面板内", all(inside),
              "inside=%s rect=%s" % (inside, rect))

        # ---- 点 Low：只改"预览目标"，不该改参数 ----
        n_sel = tap.count_line("QUALITY-UI select preset=")
        if not kit.click_anchor(hwnd, tap, "QUAL-PRESET-0"):
            FAILS.append("B3 点不到 Low（QUAL-PRESET-0）")
        ok = wait_count(tap, "QUALITY-UI select preset=Low", n_sel + 1, 10.0)
        check("B3 选中 Low", ok)
        # 差异预览出现 = "选档 ≠ 应用"这条设计真的成立
        check("B4 差异预览出现（QUAL-DIFF-0）",
              tap.wait_rect("QUAL-DIFF-0", 6.0) is not None)
        check("B5 选档没有立刻改参数（snapshot 未动）",
              tap.count_line("QUALITY-SNAPSHOT") == 0,
              "snapshot=%d" % tap.count_line("QUALITY-SNAPSHOT"))

        # ---- Apply → Low ----
        n_ap = tap.count_line("QUALITY-UI apply preset=")
        if not kit.click_anchor(hwnd, tap, "QUAL-APPLY"):
            FAILS.append("B6 点不到 Apply")
        ok = wait_count(tap, "QUALITY-UI apply preset=Low", n_ap + 1, 15.0)
        check("B6 Apply 生效", ok)
        time.sleep(0.8)
        m = last_match(tap.text().splitlines(), SNAP_RE)
        if not m:
            FAILS.append("B7 没有 QUALITY-SNAPSHOT 行")
        else:
            preset, ssao, ssr, taa, casc, dist, lights = m.groups()
            check("B7 Low 的开关全关（ssao/ssr/taa=0）"
                  " **验收原文：切到 Low 后 SSR 生效值为 0**",
                  preset == "Low" and ssao == "0" and ssr == "0" and taa == "0",
                  "preset=%s ssao=%s ssr=%s taa=%s" % (preset, ssao, ssr, taa))
            check("B8 Low 的数值项（cascades=1 dist=60 lights=1）",
                  casc == "1" and dist == "60" and lights == "1",
                  "cascades=%s dist=%s lights=%s" % (casc, dist, lights))

        m = last_match(tap.text().splitlines(), CHECK_RE)
        check("B9 应用后自检 mismatches=0（与档位定义逐项一致）",
              m is not None and m.group(1) == "Low" and m.group(2) == "0",
              m.group(0) if m else "no check line")

        # ---- 切 Epic 再应用 ----
        n_sel = tap.count_line("QUALITY-UI select preset=")
        if kit.click_anchor(hwnd, tap, "QUAL-PRESET-3"):
            wait_count(tap, "QUALITY-UI select preset=Epic", n_sel + 1, 10.0)
        n_ap = tap.count_line("QUALITY-UI apply preset=")
        kit.click_anchor(hwnd, tap, "QUAL-APPLY")
        wait_count(tap, "QUALITY-UI apply preset=Epic", n_ap + 1, 15.0)
        time.sleep(0.8)
        m = last_match(tap.text().splitlines(), SNAP_RE)
        if not m:
            FAILS.append("B10 没有 Epic 的 QUALITY-SNAPSHOT")
        else:
            preset, ssao, ssr, taa, casc, dist, lights = m.groups()
            check("B10 Epic 的开关（ssao/ssr/taa=1, cascades=4）",
                  preset == "Epic" and ssao == "1" and ssr == "1"
                  and taa == "1" and casc == "4",
                  "preset=%s ssao=%s ssr=%s taa=%s cascades=%s"
                  % (preset, ssao, ssr, taa, casc))

        # ---- Save ----
        n_sv = tap.count_line("QUALITY-UI save ")
        if not kit.click_anchor(hwnd, tap, "QUAL-SAVE"):
            FAILS.append("B11 点不到 Save")
        ok = wait_count(tap, "QUALITY-UI save ", n_sv + 1, 15.0)
        check("B11 Save 生效", ok)
        time.sleep(0.4)
        return cfg_path
    finally:
        kit.shutdown(proc, hwnd)


def verify_file(cfg_path, expect_preset="Epic"):
    if not os.path.exists(cfg_path):
        FAILS.append("B12 render.json 没写出来")
        return
    with open(cfg_path, encoding="utf-8") as f:
        r = json.load(f)
    keys = [k for k in r if not k.startswith("_") and k != "version"]
    check("B12 落盘 25 个键（22 开关 + 3 数值）", len(keys) == 25,
          "keys=%d" % len(keys))
    # Epic：数值项 = cascades 4 / dist 120 / lights 8
    want = {"shadow.cascades": 4, "shadow.distance": 120.0,
            "lshadow.maxLights": 8}
    bad = {k: r.get(k) for k, v in want.items() if r.get(k) != v}
    check("B13 数值项与 Epic 定义一致", not bad, "bad=%s" % bad)
    # 该开 / 该关各一条（同一份 enable 清单）
    check("B14 Epic 开关（ssao/decals 开、dof 关）",
          r.get("ssao.enabled") is True and r.get("decals.enabled") is True
          and r.get("dof.enabled") is False,
          "ssao=%s decals=%s dof=%s"
          % (r.get("ssao.enabled"), r.get("decals.enabled"), r.get("dof.enabled")))


# ---------------------------------------------------------------- C 组

def run_gui_reload(work):
    kit.log("---- C 组：重开工程读回档位 ----")
    cfg_path = os.path.join(work, "config", "render.json")
    if not os.path.exists(cfg_path):
        FAILS.append("C0 没有可读的 render.json")
        return
    proc, tap, hwnd = launch_editor(base_env(cfg_path))
    if not hwnd:
        FAILS.append("C1 编辑器窗口没出现（第二轮）")
        return
    try:
        if not tap.wait_rect("QUAL-RECT", 30.0):
            FAILS.append("C1 画质面板没打开（第二轮）")
            return
        # 读盘：22 个 enable + 3 个数值 = 25
        ok = tap.wait_line("RENDER-SET chain=project=25", 25.0)
        check("C2 读回 25 个覆盖项", ok,
              next((l for l in tap.text().splitlines() if "RENDER-SET chain=" in l), ""))
        # 面板打开时会自检一次（mismatches 应为 0，因为落盘就是 Epic）
        end = time.time() + 15.0
        m = None
        while time.time() < end:
            m = last_match(tap.text().splitlines(), CHECK_RE)
            if m:
                break
            time.sleep(0.2)
        check("C3 读回后自检 mismatches=0（Epic）",
              m is not None and m.group(1) == "Epic" and m.group(2) == "0",
              m.group(0) if m else "no check line")
    finally:
        kit.shutdown(proc, hwnd)


# ---------------------------------------------------------------- main

def main():
    if not os.path.isdir(OUT):
        os.makedirs(OUT, exist_ok=True)
    work = kit.fresh_dir(OUT)
    kit.log("work dir = %s" % work)

    if not os.path.exists(EDITOR) or not os.path.exists(GAME):
        kit.log("找不到 build/bin 下的 exe —— 先 ./make.cmd build")
        return kit.finish(["exe 不存在"])

    run_selftest()

    cfg = run_gui_apply_and_save(work)
    if cfg:
        verify_file(cfg, "Epic")
        run_gui_reload(work)

    return kit.finish(FAILS)


if __name__ == "__main__":
    sys.exit(main())
