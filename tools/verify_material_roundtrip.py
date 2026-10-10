"""端到端验证：Material 新增的三个纹理槽的**存盘 / 读盘往返**。

需求 2 给材质加了 roughnessMap / metallicMap / emissiveMap 三个独立槽位。
`verify_material_chips.py` 证明了"拖进去能生效"，但那只是**内存里**生效 ——
还差一环：**存盘之后能不能活着回来**。

漏掉这一环的代价和 primitive 参数那次一模一样：`SceneSerializer` 忘了写某个
槽，编辑器里看着一切正常，重新打开场景图就没了。而材质是**共享资产**
（AssetManager 是按名字缓存的），读盘时 `makeMaterialPBR` 命中缓存后并不会
碰这几个新字段 —— 必须显式补写，很容易漏。

跑一次完整闭环：

  1. 空场景起步（MYVK_EDITOR_SCENE 指向一个临时路径 → 不污染仓库）
  2. 选中自带的 Ground，把 Cube_BaseColor.png 分别拖进
     metallic / roughness / emissive 三个槽
  3. Ctrl+S 存盘
  4. **直接读 JSON**，断言三个槽都落了盘、kind=file、且 sRGB 语义正确
     （颜色贴图 emissive=1，数据贴图 metallic/roughness=0）
  5. 重新启动编辑器载入这份场景，选中 Ground
  6. 断言三个槽的 MAT-CHIP **又显示回文件名**、方块宽度也回到"已绑定"的样子

第 6 步才是真正的闭环：它同时证明 JSON 写对了、读对了、并且读回来的纹理
确实被挂回了材质（而不是停在 nullptr 上）。

用法：
    python tools/verify_material_roundtrip.py
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import find_window, user32                      # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors              # noqa: E402
from verify_asset_drag import force_foreground, move_to             # noqa: E402
from verify_material_chips import drag                              # noqa: E402
from verify_mesh_params import wait_rect                            # noqa: E402
from verify_primitive_roundtrip import press_ctrl_s                 # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
DEFAULT_ASSET = "Cube_BaseColor.png"
DEFAULT_CONTENT_DIR = "assets/models/Cube"

# 临时场景放系统临时目录：测试不应该往仓库里写文件。
TMP_DIR = tempfile.mkdtemp(prefix="venn_matroundtrip_")
TMP_SCENE = os.path.join(TMP_DIR, "mat_roundtrip.json")

# 三个槽 + 期望的 sRGB 语义（颜色贴图 1 / 数据贴图 0）。
# 这个字段必须跟着落盘 —— 它决定了读回来时会不会被 gamma 一次。
SLOTS = (("metallic", 0), ("roughness", 0), ("emissive", 1))

MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004


def launch(exe, scene, content_dir, select=None):
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_EDITOR_SCENE"] = scene
    env["MYVK_CONTENT_DIR"] = content_dir
    if select:
        env["MYVK_EDITOR_SELECT"] = select
    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    return proc, tap


def settle(hwnd, tap, size="1600x1220"):
    """摆好窗口尺寸，等 ImGui 重排完成。"""
    user32.ShowWindow(hwnd, 9)
    time.sleep(2.0)
    tw, th = (int(v) for v in size.lower().split("x"))
    user32.SetWindowPos(hwnd, 0, 0, 0, tw, th, 0x0002 | 0x0004)
    time.sleep(2.5)
    force_foreground(hwnd)
    time.sleep(1.0)
    return wait_rect(tap, "VP-RECT", 15.0)


def stop(proc):
    proc.terminate()
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()


def chip_of(tap, slot):
    """读某个槽最新一次的 MAT-CHIP 内容（None = 还没打过）。"""
    rx = re.compile(r'MAT-CHIP %s shows="([^"]*)"' % re.escape(slot))
    with tap.lock:
        out = None
        for line in tap.lines:
            m = rx.search(line)
            if m:
                out = m.group(1)
    return out


def wait_chip(tap, slot, pred, timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        v = chip_of(tap, slot)
        if v is not None and pred(v):
            return v
        time.sleep(0.1)
    return None


def chip_looks_like(shown, want):
    """方块里显示的应当是**文件名**：允许左边被截断（"..."），但去掉省略号后
    必须是文件名的后缀 —— 既排除整条路径，也排除缓存键之类的内部标记。"""
    if not shown:
        return False
    tail = shown[3:] if shown.startswith("...") else shown
    return bool(tail) and want.endswith(tail)


def tex_matches(tj, asset):
    """纹理 JSON 里的路径是不是这张图。"""
    for key in ("path", "name"):
        v = tj.get(key) or ""
        if v.replace("\\", "/").endswith(asset):
            return True
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--asset", default=DEFAULT_ASSET)
    ap.add_argument("--content-dir", default=DEFAULT_CONTENT_DIR)
    ap.add_argument("--select", default="Ground")
    ap.add_argument("--size", default="1600x1220")
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

    # ============ 阶段 1：拖三张图进槽、Ctrl+S ============
    print("[1] 拖图进 metallic/roughness/emissive -> Ctrl+S")
    proc, tap = launch(exe, TMP_SCENE, args.content_dir, select=args.select)
    try:
        hwnd = find_window("Venn Editor")
        if not hwnd:
            print("未找到窗口")
            return 2
        if settle(hwnd, tap, args.size) is None:
            print("缺 VP-RECT")
            return 3

        cell = wait_rect(tap, "CB-CELL " + args.asset, 15.0)
        if not cell:
            print("Content 面板里找不到", args.asset)
            return 3

        for slot, _srgb in SLOTS:
            r = wait_rect(tap, "MAT-SLOT " + slot, 12.0)
            if not r:
                check("拿到 %s 槽的矩形" % slot, False, "缺失")
                continue
            ok = False
            for attempt in range(3):
                r = wait_rect(tap, "MAT-SLOT " + slot, 6.0)
                cell = wait_rect(tap, "CB-CELL " + args.asset, 6.0)
                drag(hwnd, cell, r, slot)
                if wait_chip(tap, slot, lambda v: v != "(empty)", 4.0):
                    ok = True
                    break
                print("  retry %d: %s 没收到拖放（首个拖拽偶发丢事件）"
                      % (attempt + 1, slot))
                time.sleep(0.5)
            check("%s 已绑定贴图" % slot, ok, repr(chip_of(tap, slot)))

        # 回到视口停一下，确保 UI 状态已经写完
        move_to(hwnd, 60, 60)
        time.sleep(0.8)

        press_ctrl_s(hwnd)
        with tap.lock:
            saved = any("Scene saved" in l for l in tap.lines)
        check("Ctrl+S 触发存盘", saved, "没有 'Scene saved' 日志")
        # 再给文件系统一点时间落盘
        for _ in range(20):
            if os.path.exists(TMP_SCENE) and os.path.getsize(TMP_SCENE) > 0:
                break
            time.sleep(0.2)
    finally:
        stop(proc)

    # ============ 阶段 2：直接读 JSON ============
    print("\n[2] 检查落盘的 JSON")
    if not os.path.exists(TMP_SCENE):
        check("场景文件已生成", False, TMP_SCENE)
        print("\nRESULT: FAIL")
        return 4
    check("场景文件已生成", True, "%d 字节" % os.path.getsize(TMP_SCENE))

    with open(TMP_SCENE, encoding="utf-8") as f:
        data = json.load(f)

    ents = data.get("entities", [])
    ent = None
    for e in ents:
        if e.get("name") == args.select:
            ent = e
            break
    check("JSON 里有 '%s' 实体" % args.select, ent is not None,
          "%d 个实体" % len(ents))

    if ent is not None:
        mat = ent.get("material")
        check("实体带 material 块", bool(mat),
              "键: %s" % sorted(mat.keys()) if mat else "缺失")
        if mat:
            for slot, srgb in SLOTS:
                tj = mat.get(slot + "Map")
                check("material.%sMap 已落盘" % slot, tj is not None,
                      repr(tj))
                if not isinstance(tj, dict):
                    continue
                check("  %sMap.kind = file" % slot, tj.get("kind") == "file",
                      repr(tj.get("kind")))
                check("  %sMap 指向 %s" % (slot, args.asset),
                      tex_matches(tj, args.asset),
                      repr(tj.get("path") or tj.get("name")))
                check("  %sMap.srgb = %d" % (slot, srgb),
                      tj.get("srgb") == bool(srgb), repr(tj.get("srgb")))

    # ============ 阶段 3：重新载入，断言贴图活着回来 ============
    print("\n[3] 重新载入场景，断言三个槽又挂上了同一张图")
    proc, tap = launch(exe, TMP_SCENE, args.content_dir, select=args.select)
    try:
        hwnd = find_window("Venn Editor")
        if not hwnd:
            print("未找到窗口")
            return 2
        if settle(hwnd, tap, args.size) is None:
            print("缺 VP-RECT")
            return 3

        basename = args.asset.replace("\\", "/").split("/")[-1]
        for slot, _srgb in SLOTS:
            r = wait_rect(tap, "MAT-SLOT " + slot, 12.0)
            check("读盘后仍能拿到 %s 槽" % slot, r is not None)
            got = wait_chip(tap, slot, lambda v: chip_looks_like(v, basename),
                            10.0)
            check("%s 槽又显示回文件名" % slot, got is not None,
                  "显示的是 %r，期望以 %r 结尾" % (chip_of(tap, slot), basename))
            if r:
                w = r[2] - r[0]
                check("  %s 方块是\"已绑定\"的宽度" % slot, w > 60,
                      "宽 %dpx" % w)
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
