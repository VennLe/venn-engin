"""端到端验证：图元生成参数的**存盘 / 读盘往返**。

需求 2 让内置图元有了可拖拽的生成参数（Cube 的边长、Sphere 的分段数……）。
参数改了就得重建几何，而重建走的是 AssetManager 的按名缓存 —— 于是有一个
很容易漏掉的坑：**如果生成参数没进缓存键，改参数会命中旧几何**。同理，如果
参数没进场景 JSON，重新打开场景就会退回默认形状。

所以这个脚本跑一次完整的闭环：

  1. 空场景起步（MYVK_EDITOR_SCENE 指向一个临时路径）
  2. Shift+A 加 Cube，把 Size 从 1.0 拖到 1.9   → 断言缓存键变成 s1.9000
  3. Ctrl+S 存盘
  4. **直接读 JSON**，断言 primitive 块存在且 size≈1.9
  5. 重新启动编辑器并载入这份场景，选中那个 Cube
  6. 断言 MESH-PARAMS 打出来的 key **还是** builtin/cube/s1.9000

第 6 步是真正的闭环：它同时证明了 JSON 写对了、读对了、并且读回来的参数
确实被用来重建了几何（而不是停在默认值上）。

用法：
    python tools/verify_primitive_roundtrip.py
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import find_window, user32                  # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors          # noqa: E402
from verify_asset_drag import force_foreground, move_to         # noqa: E402
from verify_mesh_params import (REBUILD_RE, add_via_menu, drag_h,  # noqa: E402
                                find_line, is_shape, wait_rect)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")

# 临时场景放系统临时目录：测试不应该往仓库里写文件。
# （resolveAssetPath 对绝对路径是安全的 —— fs::path(root) / 绝对路径 会用
#   右边替换左边，所以最后拿到的还是这个绝对路径。）
TMP_DIR = tempfile.mkdtemp(prefix="venn_roundtrip_")
TMP_SCENE = os.path.join(TMP_DIR, "roundtrip.json")

WM_KEYDOWN, WM_KEYUP = 0x0100, 0x0101
VK_CONTROL = 0x11
VK_S = 0x53
SC_CTRL = 0x1D
SC_S = 0x1F


def press_ctrl_s(hwnd):
    """Ctrl+S 存盘。

    ⚠ Ctrl 必须是**真实**键盘事件：GLFW 用 GetKeyState 读修饰键，PostMessage
    投递的 Ctrl 在那里看不见（Shift+A 那个脚本也是踩过之后才这么写的）。
    而真实事件只进前台窗口的队列 —— 所以先确认前台真的成功了再发。
    """
    ok = False
    for _ in range(5):
        if force_foreground(hwnd):
            ok = True
            break
        time.sleep(0.2)
    if not ok:
        print("  WARN: 编辑器不是前台窗口，Ctrl+S 可能不生效")

    user32.keybd_event(VK_CONTROL, SC_CTRL, 0, 0)
    time.sleep(0.12)
    user32.PostMessageW(hwnd, WM_KEYDOWN, VK_S, (SC_S << 16) | 1)
    time.sleep(0.06)
    user32.PostMessageW(hwnd, WM_KEYUP, VK_S, (SC_S << 16) | 0xC0000001)
    time.sleep(0.12)
    user32.keybd_event(VK_CONTROL, SC_CTRL, 0x0002, 0)
    time.sleep(0.8)


def launch(exe, scene=None, select=None):
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    if scene:
        env["MYVK_EDITOR_SCENE"] = scene
    if select:
        env["MYVK_EDITOR_SELECT"] = select
    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()          # verify_add_menu 的 LogTap 是"先建后 start"
    tap.start(proc)
    return proc, tap


def settle(hwnd, tap, w=1600, h=900):
    """把窗口摆成固定尺寸并等 ImGui 重排完成，返回视口中心 client 坐标。"""
    user32.ShowWindow(hwnd, 9)
    time.sleep(2.0)
    user32.SetWindowPos(hwnd, 0, 0, 0, w, h, 0x0002 | 0x0004)
    time.sleep(2.5)
    force_foreground(hwnd)
    time.sleep(0.8)
    vp = wait_rect(tap, "VP-RECT", 10.0)
    if not vp:
        return None
    return (int((vp[0] + vp[2]) * 0.5), int((vp[1] + vp[3]) * 0.5))


def stop(proc):
    proc.terminate()
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    if not os.path.exists(exe):
        print("找不到可执行文件，请先构建：", exe)
        return 1

    kill_stale_editors()
    print("临时场景:", TMP_SCENE)

    fails = []

    def check(name, ok, detail=""):
        print("  %s  %s%s" % ("PASS" if ok else "FAIL", name,
                              ("  [%s]" % detail) if detail else ""))
        if not ok:
            fails.append("%s  %s" % (name, detail))

    # ============ 阶段 1：加图元、改参数、存盘 ============
    print("[1] 空场景 -> 加 Cube -> 拖 Size -> Ctrl+S")
    proc, tap = launch(exe, scene=TMP_SCENE)
    try:
        hwnd = find_window("Venn Editor")
        if not hwnd:
            print("未找到窗口")
            return 2
        vc = settle(hwnd, tap)
        if not vc:
            print("缺 VP-RECT")
            return 3
        vx, vy = vc

        move_to(hwnd, vx, vy)
        time.sleep(0.4)
        if not add_via_menu(tap, hwnd, vx, vy, "Mesh", "Cube", "Cube"):
            print("Shift+A 菜单没走通，中止")
            return 4

        hdr = wait_rect(tap, "INS-MESH-HEADER", 6.0)
        if hdr:
            from verify_add_menu import click
            click(hwnd, (hdr[0] + hdr[2]) * 0.5, (hdr[1] + hdr[3]) * 0.5)
            time.sleep(0.6)

        sizeRect = wait_rect(tap, "MESH-PARAM Size", 6.0)
        if not sizeRect:
            check("拿到 Size 控件矩形", False, "缺失")
        else:
            move_to(hwnd, vx, vy)
            time.sleep(0.2)
            drag_h(hwnd, sizeRect, +90, label="Size")
            m = find_line(tap, REBUILD_RE.pattern, 6.0, where=is_shape("cube"))
            if not m:
                check("拖 Size 后几何重建", False, "没有 rebuild 日志")
            else:
                check("拖 Size 后几何重建", True, "size=%.4f" % float(m.group(3)))
                check("缓存键带上新边长", "s1.0000" not in m.group(8),
                      m.group(8))
                size1 = float(m.group(3))

        press_ctrl_s(hwnd)
        saved = find_line(tap, r"Scene saved: '(.+)'", 5.0)
        check("Ctrl+S 触发存盘", bool(saved),
              saved.group(1) if saved else "没有 'Scene saved' 日志")
    finally:
        stop(proc)

    # ============ 阶段 2：直接读 JSON ============
    print("\n[2] 检查落盘的 JSON")
    if not os.path.exists(TMP_SCENE):
        check("场景文件已生成", False, TMP_SCENE)
        print("\nRESULT: FAIL")
        return 4
    check("场景文件已生成", True,
          "%d 字节" % os.path.getsize(TMP_SCENE))

    with open(TMP_SCENE, encoding="utf-8") as f:
        data = json.load(f)

    cubes = [e for e in data.get("entities", [])
             if (e.get("mesh") or {}).get("shape") == "cube"]
    check("JSON 里有 cube 实体", bool(cubes), "%d 个" % len(cubes))
    if cubes:
        # 结构是平铺的：{"kind":"cube","size":1.9,"radius":0.5,...}
        # 见 SceneSerializer.cpp 的 primitiveParamsToJson
        prim = cubes[0].get("primitive")
        check("cube 带 primitive 块", bool(prim), repr(prim))
        if prim:
            got = prim.get("size")
            check("落盘的 size 与拖拽后的值一致",
                  got is not None and abs(float(got) - size1) < 1e-3,
                  "json=%.4f  期望=%.4f" % (float(got if got is not None else -1),
                                          size1))
            check("落盘的 kind 是 cube", prim.get("kind") == "cube",
                  repr(prim))

    # ============ 阶段 3：重新载入，断言参数活着回来 ============
    print("\n[3] 重新载入场景，断言参数与几何都还原")
    proc, tap = launch(exe, scene=TMP_SCENE, select="Cube")
    try:
        hwnd = find_window("Venn Editor")
        if not hwnd:
            print("未找到窗口")
            return 2
        if settle(hwnd, tap) is None:
            print("缺 VP-RECT")
            return 3

        hdr = wait_rect(tap, "INS-MESH-HEADER", 10.0)
        if hdr:
            from verify_add_menu import click
            click(hwnd, (hdr[0] + hdr[2]) * 0.5, (hdr[1] + hdr[3]) * 0.5)
            time.sleep(0.6)

        want_key = "builtin/cube/s%.4f" % size1
        m = find_line(tap, r"MESH-PARAMS kind=Cube labels=(\S+) key=(\S+)", 10.0)
        check("读盘后仍是 Cube 图元", bool(m),
              m.group(0) if m else "没有 MESH-PARAMS 日志")
        if m:
            check("参数集合已还原", m.group(1) == "Size", m.group(1))
            check("缓存键 = 存档时的值（几何用存档参数重建）",
                  m.group(2) == want_key, "got=%s want=%s" % (m.group(2), want_key))
    finally:
        stop(proc)

    print()
    if fails:
        print("RESULT: FAIL")
        for f in fails:
            print("  -", f)
        return 4
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
