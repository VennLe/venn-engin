"""端到端验证：GameStart 在编辑器里"点得到、拖得动、落得地、能聚焦"。

对应用户 2026-10-21 第九轮的三条反馈里与 GameStart 相关的两条：

  1. "GameStart 这个标记角色鼠标选中时要可以显示它的 Gizmos 轴，另外按住
     数字键盘 0 也要正常可以向下停留在地面。"
  3. "在找不到视口中的物体对象时（包括 GameStart），在 Hierarchy 中双击
     选择对应的对象即可在视口中聚焦到对应的对象。"

背景：GameStart **没有网格组件**，所以它既不在 BVH 里（点不中），也没有
包围盒（"落地"会跳过它）。这两件事是同一个根因的两种表现。

断言分三组：

  [1] 点视口里的标记 → 选中 + 变换手柄出现（`GZ-ORIGIN` 落在标记附近）
  [1b] 选中后 Inspector 里有 Player Start 段（视角模式 / 运行即生成 / 启用）
      —— 2026-10-24 起 GameStart 同时是"出生参数"，这里是唯一编辑落点
  [2] 按小键盘 0 → `drop-to-floor: 'GameStart'` 把原点落到地面
  [3] Hierarchy 双击 → `HIER-FOCUS` + 相机 target 变了

用法：
    python tools/verify_gamestart_edit.py
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_client, find_window, user32      # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors, wait_rect    # noqa: E402
from verify_asset_drag import force_foreground, move_to              # noqa: E402
from verify_nav_gizmo import MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(REPO, "..", "_venn_verify")
EXE = os.path.join(REPO, "build", "bin", "Editor.exe")
TPL = os.path.join(REPO, "templates", "third_person", "assets")

CAM = "0,-7,4,90,14"
# 出生点在场景里抬高 2 m：这样"按 0 落地"才有可测的位移
START_Z = 2.0


def build_scene(path):
    doc = json.load(open(os.path.join(TPL, "scenes", "main.json"),
                         encoding="utf-8"))
    for e in doc.get("entities", []):
        if "gameStart" in e:
            e["transform"]["position"][2] = START_Z
    json.dump(doc, open(path, "w", encoding="utf-8"), indent=1)


def center(rect):
    return ((rect[0] + rect[2]) * 0.5, (rect[1] + rect[3]) * 0.5)


def click(hwnd, xy):
    move_to(hwnd, xy[0], xy[1])
    time.sleep(0.30)
    user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.12)
    user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
    time.sleep(0.55)


def double_click(hwnd, xy):
    # ⚠ 不能复用 click()：它点完会 sleep 0.55 s 等画面稳定，两次点击间隔
    #   就超过了 ImGui 的 0.30 s 双击窗口 —— 那永远只是"两次单击"。
    move_to(hwnd, xy[0], xy[1])
    time.sleep(0.28)
    for _ in range(2):
        user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
        time.sleep(0.05)
        user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
        time.sleep(0.09)
    time.sleep(0.8)


def rect_of(lines, tag):
    for l in reversed(lines):
        if tag in l:
            m = re.search(r"=\((-?[\d.]+),(-?[\d.]+)\)-\((-?[\d.]+),(-?[\d.]+)\)", l)
            if m:
                return tuple(float(v) for v in m.groups())
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=EXE)
    args = ap.parse_args()

    if not os.path.isfile(args.exe):
        print("结果: FAIL: 找不到 %s" % args.exe)
        return 2
    os.makedirs(OUT_DIR, exist_ok=True)

    scene = os.path.join(OUT_DIR, "gs_edit_scene.json")
    build_scene(scene)

    fails = []

    def check(label, ok, extra=""):
        print("  %s  %s%s" % ("[PASS]" if ok else "[FAIL]", label,
                              ("  " + extra) if extra else ""))
        if not ok:
            fails.append(label)

    print("=" * 68)
    print("GameStart：可点选 / 可落地 / 可双击聚焦")
    print("=" * 68)

    kill_stale_editors()
    env = dict(os.environ)
    env.update({
        "MYVK_ASSET_DIR": TPL,
        "MYVK_EDITOR_SCENE": scene,
        "MYVK_NO_WINDOW_SAVE": "1",
        "MYVK_LOG_RECTS": "1",
        "MYVK_EDITOR_CAM": CAM,
        "MYVK_GRID": "0",
    })
    proc = subprocess.Popen([args.exe], cwd=os.path.dirname(args.exe), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    hwnd = None
    try:
        hwnd = find_window("Venn Editor", timeout=45.0)
        if not hwnd:
            print("  [FAIL] 编辑器窗口没出现")
            return 1
        user32.SetWindowPos(hwnd, 0, 0, 0, 1280, 800, 0x0002 | 0x0004)
        time.sleep(4.0)
        force_foreground(hwnd)
        time.sleep(0.8)

        with tap.lock:
            lines = list(tap.lines)
        mark = rect_of(lines, "GAMESTART-MARK")
        check("标记出现在视口里", mark is not None, str(mark))
        if not mark:
            return 1

        # ============ [1] 点标记 → 选中 + 手柄 ============
        print("\n[1] 鼠标点视口里的标记")
        n_gz_before = sum(1 for l in lines if "GZ-ORIGIN" in l)
        click(hwnd, center(mark))
        time.sleep(1.2)
        with tap.lock:
            lines = list(tap.lines)
        txt = "\n".join(lines)
        gz = rect_of(lines, "GZ-ORIGIN")
        check("点中了 GameStart（手柄出现 GZ-ORIGIN）", gz is not None,
              "n_before=%d" % n_gz_before)
        if gz:
            gc = center(gz)
            mc = center(mark)
            d = ((gc[0] - mc[0]) ** 2 + (gc[1] - mc[1]) ** 2) ** 0.5
            # 手柄原点在脚底，标记矩形中心在胸口 —— 只要在同一块区域就行
            check("手柄落在标记上（屏幕距离 < 标记高度）",
                  d < (mark[3] - mark[1]) * 0.9, "d=%.0fpx" % d)
        # ============ [1b] Inspector 的 "Player Start" 段 ============
        # 2026-10-24：GameStart 从"一个标记"升级成"出生参数"（视角模式 /
        # 运行时要不要生成玩家 / 朝向），编辑落点就是 Inspector 里这一段。
        # 它挂在 `drawMeshAndMaterial` 的末尾 —— 重构时最容易被顺手删掉，
        # 而删掉之后**编辑期没有任何别的地方**能改视角模式（视口菜单只负责
        # 建/选中出生点）。所以这段的存在性本身就是断言。
        print("\n[1b] Inspector 的 Player Start 段")
        hdr = rect_of(lines, "INS-GAMESTART-HEADER")
        check("GameStart 选中后出现 Player Start 段", hdr is not None, str(hdr))
        for tag, label in (("INS-GAMESTART-MODE", "视角模式下拉"),
                           ("INS-GAMESTART-SPAWN", "运行即生成玩家勾选框"),
                           ("INS-GAMESTART-ENABLED", "启用勾选框")):
            check("%s 在段内" % label, rect_of(lines, tag) is not None, tag)

        # ============ [2] 小键盘 0 → 落地 ============
        print("\n[2] 按小键盘 0（落地）")
        before = txt.count("drop-to-floor")
        # 真·小键盘 0：PostMessage 不好造，直接发 VK_NUMPAD0
        user32.keybd_event(0x60, 0, 0, 0)          # VK_NUMPAD0 down
        time.sleep(0.10)
        user32.keybd_event(0x60, 2, 0, 0)          # up
        time.sleep(1.0)
        with tap.lock:
            lines2 = list(tap.lines)
        drop = [l.strip() for l in lines2 if "drop-to-floor" in l][before:]
        check("按 0 打了落地日志", bool(drop), drop[-1][:110] if drop else "")
        if drop:
            check("落地的对象是 GameStart（不再被 'no mesh bounds' 跳过）",
                  "GameStart" in drop[-1] and "no mesh bounds" not in drop[-1],
                  drop[-1][-90:])
            m = re.search(r"dz=([-\d.]+)", drop[-1])
            if m:
                check("dz 约等于 -START_Z（从 %.1f 米落到地面）" % START_Z,
                      abs(float(m.group(1)) + START_Z) < 0.05,
                      "dz=%s" % m.group(1))

        # ============ [3] Hierarchy 双击 → 聚焦 ============
        print("\n[3] Hierarchy 双击聚焦")
        with tap.lock:
            lines3 = list(tap.lines)
        row = rect_of(lines3, "HIER-RECT row0")
        check("拿到 Hierarchy 行矩形", row is not None, str(row))
        if row:
            double_click(hwnd, center(row))
            time.sleep(1.0)
            with tap.lock:
                lines4 = list(tap.lines)
            hier = [l.strip() for l in lines4 if "HIER-FOCUS" in l]
            check("双击打了 HIER-FOCUS", bool(hier),
                  hier[-1][:100] if hier else "")
            # ⚠ 断言打在 FOCUS-ENTITY 上，不是 GZ-CAM：GZ-CAM 只在
            #   "位姿变化超过阈值"时打，是**间接**量；FOCUS-ENTITY 才是
            #   "相机被要求对准谁"的直接证据。
            fe = [l.strip() for l in lines4 if "FOCUS-ENTITY" in l]
            check("打了 FOCUS-ENTITY（相机对准了该实体）", bool(fe),
                  fe[-1][:110] if fe else "")
            # 相机真的动了：出生点标记在屏幕上的矩形必然变化
            mark_after = rect_of(lines4, "GAMESTART-MARK")
            check("画面真的变了（标记矩形随相机变化）",
                  mark_after is not None and mark_after != mark,
                  "before=%s after=%s" % (mark, mark_after))

        img, _ = capture_client(hwnd)
        img.save(os.path.join(OUT_DIR, "gs_edit.png"))

    finally:
        if hwnd:
            user32.PostMessageW(hwnd, 0x0010, 0, 0)
        time.sleep(0.6)
        if proc.poll() is None:
            proc.terminate()

    print("\n" + "=" * 68)
    if fails:
        print("结果: FAIL（%d 项）" % len(fails))
        for f in fails:
            print("  · " + f)
        return 1
    print("结果: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
