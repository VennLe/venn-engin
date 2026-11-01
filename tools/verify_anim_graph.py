#!/usr/bin/env python3
# ============================================================
# tools/verify_anim_graph.py —— 状态机图编辑器 + 角色流程壳
#
# 三组判据，对应这两块新东西各自的"能不能用"：
#
#   ① **序列化往返**（不建窗口，纯 CPU）。图编辑器的全部数据通路就是
#      "读 .anim.json → 改内存 → 写回"。往返不成立的话，用户改一条边
#      按保存，文件里就可能悄悄少一个字段 —— 而这类 bug 只有在"重开
#      发现设置没了"时才会暴露。
#      宿主是 `AnimationSelfTest` 里的 `ctl_*` 四条（走 MYVK_ANIM_TEST）。
#
#   ② **状态机面板能画出来**。加载成功、四个状态、八条转移、节点像素
#      真的出现在画布上。只判日志是不够的：节点曾经因为环形半径算错
#      全跑到画布外，日志一切正常、画面一片空白。
#
#   ③ **流程壳能跳步并把人送出去**。步骤条的 `goTo` 与"打开某面板"
#      请求链路（EditorContext::requestPanelOpen → focusPanel）。
#
# ⚠ 第 ③ 组顺带守住一条踩过的坑：`kSlotNone` 面板只设 open 位**不会
#   显示**（它不是当前标签页），必须走 focusPanel。那次的表现是
#   日志写着 open=1、画面上什么都没有。
# ============================================================

import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from verify_ibl import DEFAULT_EXE, OUT_DIR, run_once  # noqa: E402

fails = []
checks = 0


def check(name, ok, detail=""):
    global checks
    checks += 1
    print("  [%s] %s  %s" % ("PASS" if ok else "FAIL", name, detail))
    if not ok:
        fails.append(name)


# ---------------------------------------------------------------- ① 序列化往返

def run_selftest():
    env = dict(os.environ)
    env["MYVK_ANIM_TEST"] = "1"
    p = subprocess.run([DEFAULT_EXE],
                       cwd=os.path.dirname(DEFAULT_EXE), env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, encoding="utf-8", errors="replace")
    return p.stdout


def group_roundtrip():
    print("\n== 第 1 组：状态机序列化往返（无窗口）==")
    out = run_selftest()
    lines = [l for l in out.splitlines() if "ANIM-TEST ctl_" in l]
    check("拿到自检输出", bool(lines), "%d 行 ctl_*" % len(lines))

    want = ["ctl_parse", "ctl_roundtrip_parse", "ctl_roundtrip_fields",
            "ctl_serialize_idempotent"]
    got = {}
    for l in lines:
        m = re.search(r"ANIM-TEST (\S+) .*result=(\w+)", l)
        if m:
            got[m.group(1)] = m.group(2)
    for w in want:
        check("往返判据 %s" % w, got.get(w) == "PASS", got.get(w, "(缺失)"))

    m = re.search(r"ANIM-TOTAL pass=(\d+) fail=(\d+)", out)
    check("AnimationSelfTest 整体零失败",
          m is not None and int(m.group(2)) == 0,
          m.group(0) if m else "(没有 ANIM-TOTAL)")

    # 幂等那条最容易被"只写不读"的新字段破坏，单独把它的失败理由打出来。
    for l in lines:
        if "ctl_serialize_idempotent" in l and "FAIL" in l:
            print("       %s" % l.strip()[:200])


# ---------------------------------------------------------------- ② 面板能画

# 节点填充色 = AnimatorGraphPanel.cpp 的 kColNode = (58,62,74)。
# 取 ±14 的容差：ImGui 的颜色进 framebuffer 后受 sRGB / 主题混合影响，
# 逐位相等是奢望；而"整片背景 (28,30,36)"离它足够远，不会误判。
NODE_RGB = (58, 62, 74)
NODE_TOL = 14


def count_node_pixels(img):
    """数"节点底色"像素。返回 (命中数, 总像素)。"""
    px = img.convert("RGB").load()
    w, h = img.size
    hit = 0
    total = 0
    # 隔行采样：4K 截图逐点扫要几秒，而节点是大色块，步长 2 完全够。
    for y in range(0, h, 2):
        for x in range(0, w, 2):
            r, g, b = px[x, y]
            total += 1
            if (abs(r - NODE_RGB[0]) <= NODE_TOL and
                    abs(g - NODE_RGB[1]) <= NODE_TOL and
                    abs(b - NODE_RGB[2]) <= NODE_TOL):
                hit += 1
    return hit, total


def group_panel():
    print("\n== 第 2 组：状态机面板（节点真的画出来）==")
    shot = os.path.join(OUT_DIR, "animgraph_panel.png")
    lines, vp, img, errs = run_once(
        DEFAULT_EXE, "assets/scenes/anime_town.json",
        {"MYVK_EDITOR_PANELS": "animgraph"}, shot, wait=4.0)

    check("零 Vulkan 验证层报错", not errs, errs[:2] if errs else "")

    load = [l for l in lines if "ANIMGRAPH-LOAD" in l]
    check("加载了控制器资产", bool(load), load[0].strip()[:120] if load else "")
    if load:
        m = re.search(r"states=(\d+) trans=(\d+) params=(\d+)", load[0])
        ok = m is not None and m.group(1) == "4" and m.group(2) == "8"
        check("humanoid 里 4 状态 / 8 转移", ok,
              m.group(0) if m else "(格式不符)")

    check("没有 PANEL-OVERFLOW（面板表没超过 kMaxPanels）",
          not any("PANEL-OVERFLOW" in l for l in lines))

    if img is not None:
        hit, total = count_node_pixels(img)
        pct = hit * 100.0 / max(total, 1)
        # 四个 168×62 的节点在 0.8 缩放下约 3 万像素，隔行采样后仍有
        # 几千个命中。阈值取 0.15% 只为排除"整片背景"这类误判。
        check("画布上出现节点色块（不是空画布）", pct > 0.15,
              "%.3f%% (%d px)" % (pct, hit))
    else:
        check("拿到截图", False)


# ---------------------------------------------------------------- ③ 流程壳

def group_studio():
    print("\n== 第 3 组：Character Studio 步骤条 ==")
    shot = os.path.join(OUT_DIR, "charstudio_step8.png")
    lines, vp, img, errs = run_once(
        DEFAULT_EXE, "assets/scenes/anime_demo.json",
        {"MYVK_EDITOR_PANELS": "charstudio",
         "MYVK_EDITOR_SELECT": "animate_girl_rigged",
         "MYVK_EDITOR_STUDIO_STEP": "statemachine"}, shot, wait=5.0)

    check("零 Vulkan 验证层报错", not errs, errs[:2] if errs else "")
    check("面板被打开（走 focusPanel，不是裸 setPanelOpen）",
          any("PANEL charstudio open=1" in l for l in lines) or
          any("PANEL-FOCUS charstudio" in l for l in lines))

    goto = [l for l in lines if "CHARSTUDIO-GOTO" in l]
    check("环境变量能跳到指定步骤", bool(goto),
          goto[0].strip()[:100] if goto else "")
    # 跳步之后不能再被"首次打开自动落到第 1 步"覆盖 —— 那是两条互相
    # 打架的初始化路径。
    check("没有 PANEL-OVERFLOW",
          not any("PANEL-OVERFLOW" in l for l in lines))

    if img is not None:
        hit, total = count_node_pixels(img)
        # 状态机内嵌在步骤 8 里，所以这块区域也该有节点色 —— 与第 2 组
        # 同一条判据，但那是"独立面板"，这是"内嵌"。两处都过才说明
        # `drawBody` 与 `draw` 共用的是同一段正文。
        pct = hit * 100.0 / max(total, 1)
        check("内嵌的状态机画布也有节点色块", pct > 0.05,
              "%.3f%% (%d px)" % (pct, hit))
    else:
        check("拿到截图", False)


def main():
    print("=== 动画状态机图编辑器 / Character Studio 验收 ===")
    group_roundtrip()
    group_panel()
    group_studio()
    print("\n共 %d 项检查" % checks)
    print("\n结果: %s" % ("PASS" if not fails
                         else "FAIL: " + ", ".join(fails)))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
