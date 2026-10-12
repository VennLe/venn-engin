"""端到端验证：shader 热重载（见 render/ShaderHotReload.h）。

背景：脚本 (.vks) 已经能 0.5s 轮询 mtime 热重载，但 shader 是离线 glslc
预编译的 —— 改一行片元代码要重启编辑器才能看到效果。现在 Renderer 在
drawFrame 开头轮询 build 目录里 .spv 的 mtime（每 15 帧 ≈ 0.25s），发现
变化就 waitIdle → 重跑 createPipelines（变体族惰性缓存被清空，下一帧
按需从磁盘重读新字节码）。

这个脚本**在同一进程内**完成"覆盖 .spv → 等重载日志 → 截图对比"——
重启进程的话就什么都不用验了。断言四组：

  1. **改动生效**：把 pbr.k0.frag.spv 覆盖成"强制绿色"版本（用 glslc
     编一个改了 color 的临时源），必须出现 `SHADER-RELOAD ok` 日志，
     且画面与基线差异巨大（场景里所有材质都是掩码 0 → 整屏变色）。
  2. **管线真的重建了**（防日志假打）：重载后的 SHADER-VARIANTS 里
     fwd_created 必须回到 1（缓存被清空后按需重建），而不是保持旧值。
  3. **还原完全复原**：把原 .spv 写回去，等第二条 SHADER-RELOAD，
     画面与基线**逐像素相同**（同一 SPIR-V → 同一管线 → 同一帧）。
     这是"热重载没有半点副作用"的铁证。
  4. **无误触发**：全程 SHADER-RELOAD ok 恰好 2 条；零 VUID / 零 Fatal。

⚠ 像素比对只统计视口矩形内（ImGui 面板 1~2px 排版抖动，见
  verify_local_shadow.py 的 crop_pair 注释）。
⚠ 需要构建产物目录里的 glslc：探测 $VULKAN_SDK，再退到 C:\\VulkanSDK\\*。
  找不到就整组 FAIL —— 本项目的构建本身依赖 glslc，这不冤。

用法：
    python tools/verify_shader_hotreload.py
"""

import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_client, find_window, user32      # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors, wait_rect   # noqa: E402
from verify_local_shadow import diff_stats, to_np, vp_box           # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = os.path.join(ROOT, "..", "_venn_verify")
SHADER_SRC = os.path.join(ROOT, "assets", "shaders")
# 运行时 resolveShaderPath 的第一候选是 exe 目录/shaders —— 构建也把产物
# 拷到那里。覆盖与还原都发生在这个目录（MYVK_SHADER_DIR 未设的前提）。
SPV_DIR = os.path.join(ROOT, "build", "bin", "shaders")
TARGET_SPV = os.path.join(SPV_DIR, "pbr.k0.frag.spv")
TMP_SRC = os.path.join(OUT_DIR, "_hot_pbr.frag")

CAM = "0,-34,15,90,16"

RELOAD_RE = re.compile(r"SHADER-RELOAD ok")
VAR_RE = re.compile(r"SHADER-VARIANTS fwd_created=(\d+) depth_created=(\d+)")


# ============================================================
# 场景：地面 + 3 个立方体，材质全部掩码 0（不透明/不双面/没法线图）
# —— 改 pbr.k0 一份 .spv 就能改变整屏，对照组干净。
# ============================================================
def material(name, base):
    return {"kind": "standard", "name": name,
            "baseColor": list(base) + [1.0],
            "metallic": 0.0, "roughness": 0.85, "normalScale": 0.0,
            "ao": 1.0, "emissive": [0, 0, 0],
            "doubleSided": False, "alphaBlend": False}


def entity(name, pos, mesh, mat):
    return {"name": name,
            "transform": {"position": list(pos), "rotation": [0, 0, 0],
                          "scale": [1, 1, 1]},
            "visible": True, "castShadow": True, "parent": -1,
            "mesh": {"kind": "builtin", "shape": mesh[0],
                     "size": mesh[1], "name": name.lower()},
            "material": mat}


def build_scene(path):
    ents = [entity("Ground", (0, 0, 0), ("plane", 80.0),
                   material("hr_ground", (0.70, 0.70, 0.73)))]
    for i, x in enumerate((-10.0, 0.0, 10.0)):
        ents.append(entity("Cube%d" % i, (x, 3.0, 2.0), ("cube", 4.0),
                           material("hr_cube%d" % i,
                                    (0.85, 0.45, 0.30) if i % 2 == 0
                                    else (0.30, 0.55, 0.85))))
    doc = {"version": 1, "generator": "verify_shader_hotreload.py",
           "light": {"direction": [-0.35, -0.5, -0.79],
                     "color": [1.0, 0.97, 0.92], "intensity": 0.35,
                     "castsShadow": True, "ambientScale": 0.25},
           "entities": ents}
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)


# ============================================================
# glslc 与改色源
# ============================================================
def find_glslc():
    sdk = os.environ.get("VULKAN_SDK")
    if sdk:
        for sub in ("Bin", "bin"):
            p = os.path.join(sdk, sub, "glslc.exe")
            if os.path.exists(p):
                return p
    for p in glob.glob(r"C:\VulkanSDK\*\Bin\glslc.exe"):
        return p
    return None


def make_green_source(dst):
    """读 pbr.frag，在 outColor 赋值前插一行强制绿色，写到 dst。"""
    with open(os.path.join(SHADER_SRC, "pbr.frag"), encoding="utf-8") as f:
        src = f.read()
    anchor = "    outColor = vec4(color, alpha);"
    assert anchor in src, "pbr.frag 里找不到 outColor 锚点 —— 改了源码就同步这里"
    src = src.replace(
        anchor,
        "    color = vec3(0.05, 0.90, 0.10); // HOTRELOAD-TEST 强制绿色\n"
        + anchor, 1)
    with open(dst, "w", encoding="utf-8") as f:
        f.write(src)


def compile_to(glslc, src, out_spv):
    r = subprocess.run(
        [glslc, "-I" + SHADER_SRC, src, "-o", out_spv],
        capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        raise RuntimeError("glslc 编译失败: %s" % r.stderr[:800])


# ============================================================
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    os.makedirs(OUT_DIR, exist_ok=True)
    if not os.path.exists(exe):
        print("找不到可执行文件，请先构建：", exe)
        return 1

    fails = []

    def check(name, cond, detail=""):
        print(("  [PASS] " if cond else "  [FAIL] ") + name +
              (("  " + str(detail)) if detail else ""))
        if not cond:
            fails.append(name)

    glslc = find_glslc()
    check("找到 glslc（$VULKAN_SDK 或 C:\\VulkanSDK\\*）", glslc is not None,
          glslc or "设 VULKAN_SDK 后重试")
    if not glslc:
        return 1
    check("目标 .spv 存在", os.path.exists(TARGET_SPV), TARGET_SPV)
    if not os.path.exists(TARGET_SPV):
        return 1

    kill_stale_editors()
    scene = os.path.join(OUT_DIR, "shader_hotreload.json")
    build_scene(scene)

    # ---- 备份原 .spv（还原组断言的前提；finally 里无论如何都还原）----
    backup = TARGET_SPV + ".hotreload-bak"
    shutil.copy2(TARGET_SPV, backup)

    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_ENV_DBG"] = "0"
    env["MYVK_LIGHT_GIZMOS"] = "0"
    env["MYVK_EDITOR_NO_HUD"] = "1"
    env["MYVK_SHADER_VARIANTS"] = "1"    # 用 fwd_created 验证缓存真被清了
    env["MYVK_FRUSTUM_DBG"] = "0"
    env["MYVK_SHADER_HOTRELOAD"] = "1"
    env["MYVK_EDITOR_SCENE"] = scene
    env["MYVK_EDITOR_CAM"] = CAM

    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    hwnd = None
    try:
        hwnd = find_window("Venn Editor")
        if not hwnd:
            check("启动编辑器", False, "未找到窗口")
            return 1
        user32.ShowWindow(hwnd, 9)
        time.sleep(1.5)
        user32.SetWindowPos(hwnd, 0, 0, 0, 1280, 800, 0x0002 | 0x0004)
        time.sleep(2.5)

        vp = wait_rect(tap, "VP-RECT", 10.0)
        end = time.time() + 20.0
        while time.time() < end:
            with tap.lock:
                n = sum(1 for l in tap.lines if "SHADER-VARIANTS " in l)
            if n >= 2:
                break
            time.sleep(0.25)
        time.sleep(1.0)

        with tap.lock:
            lines = list(tap.lines)
        base_vars = [int(m.group(1)) for m in map(VAR_RE.search, lines) if m]
        check("基线：变体已就绪（fwd_created >= 1）",
              bool(base_vars) and base_vars[-1] >= 1, base_vars[-3:])

        img_a, _ = capture_client(hwnd)
        img_a.save(os.path.join(OUT_DIR, "hotreload_before.png"))

        def reload_count():
            with tap.lock:
                return sum(1 for l in tap.lines if RELOAD_RE.search(l))

        def wait_reload(target, timeout=15.0):
            end = time.time() + timeout
            while time.time() < end:
                if reload_count() >= target:
                    time.sleep(0.8)  # 等首帧用新管线画完
                    return True
                time.sleep(0.2)
            return False

        # ---- 组 1：覆盖 .spv → 热重载生效 ----
        make_green_source(TMP_SRC)
        compile_to(glslc, TMP_SRC, TARGET_SPV)
        ok1 = wait_reload(1)
        with tap.lock:
            reloads1 = [l.strip() for l in tap.lines if RELOAD_RE.search(l)]
        check("覆盖 pbr.k0.frag.spv 后出现 SHADER-RELOAD ok", ok1,
              reloads1[-1] if reloads1 else "15s 内没等到日志")

        img_b, _ = capture_client(hwnd)
        img_b.save(os.path.join(OUT_DIR, "hotreload_after.png"))
        if img_a is not None and img_b is not None:
            n, mx, mean, box = diff_stats(img_a, img_b, vp)
            print("     基线 vs 强制绿：差异像素=%d 最大差=%d bbox=%s"
                  % (n, mx, box))
            check("画面变了（改动真的生效）", n > 3000 and mx > 60,
                  "n=%d max=%d" % (n, mx))
        else:
            check("画面变了（改动真的生效）", False, "截图失败")

        # ---- 组 2：管线真的重建（fwd_created 回到 1）----
        end = time.time() + 8.0
        after_vars = base_vars
        while time.time() < end:
            with tap.lock:
                lines = list(tap.lines)
            after_vars = [int(m.group(1))
                          for m in map(VAR_RE.search, lines) if m]
            if len(after_vars) > len(base_vars):
                break
            time.sleep(0.25)
        # 重载清空了惰性缓存；k0 马上又被绘制用到 → 第一条新日志就是 1。
        check("重载后 fwd_created 回到 1（缓存真的被清了）",
              bool(after_vars) and after_vars[-1] == 1,
              "before=%s after=%s" % (base_vars[-2:], after_vars[-2:]))

        # ---- 组 3：还原 → 与基线逐像素相同 ----
        shutil.copy2(backup, TARGET_SPV)
        ok2 = wait_reload(2)
        with tap.lock:
            reloads2 = [l.strip() for l in tap.lines if RELOAD_RE.search(l)]
        check("还原后出现第二条 SHADER-RELOAD ok", ok2,
              reloads2[-1] if reloads2 else "15s 内没等到日志")

        img_c, _ = capture_client(hwnd)
        img_c.save(os.path.join(OUT_DIR, "hotreload_restored.png"))
        if img_a is not None and img_c is not None:
            n, mx, mean, box = diff_stats(img_a, img_c, vp)
            print("     基线 vs 还原后：差异像素=%d 最大差=%d bbox=%s"
                  % (n, mx, box))
            check("还原后与基线逐像素相同", n == 0,
                  "n=%d max=%d" % (n, mx))
        else:
            check("还原后与基线逐像素相同", False, "截图失败")

        # ---- 组 4：无误触发 + 零验证层报错 ----
        check("SHADER-RELOAD ok 恰好 2 条（无误触发）", reload_count() == 2,
              reload_count())
        with tap.lock:
            lines = list(tap.lines)
        errs = [l for l in lines
                if ("VUID" in l or "VALIDATION" in l or " ERROR" in l
                    or "Error:" in l)]
        check("零 VUID / 零验证层报错", not errs, errs[:2])

        # 汇总行：run_regression.py 靠它（结果: PASS/FAIL）判定 ok，
        # 光有退出码不够 —— 每个用例的最后一行必须是这条。
        print("\n结果: %s" % ("PASS" if not fails
                              else "FAIL: " + ", ".join(fails)))
        return 1 if fails else 0
    finally:
        # 无论成败：还原原 .spv（防测试残留下一份绿色 shader）
        if os.path.exists(backup):
            shutil.copy2(backup, TARGET_SPV)
            os.remove(backup)
        if os.path.exists(TMP_SRC):
            os.remove(TMP_SRC)
        if hwnd:
            user32.PostMessageW(hwnd, 0x0010, 0, 0)
        time.sleep(0.5)
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=6)
            except subprocess.TimeoutExpired:
                proc.kill()


if __name__ == "__main__":
    sys.exit(main())
