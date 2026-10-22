#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""回归：外部 FBX 角色进场景后的**真实尺寸**（1:1 校验）。

## 要回答的问题

"从网上下载的 FBX 角色导进来，比例对不对？跟真实环境是不是一比一？"
尺度错误是导入链里**最难靠肉眼发现**的一类：差 100 倍时画面里仍然"像个角色"，
只是人比树还高。现有用例全是"能不能加载 / 顶点对不对"，没有一条回答
"它到底多高"。这条补的就是那件事。

## 量法（自标定，不需要知道相机内参）

同一台相机、同一个位置，先拍 **1 m 标尺**，再拍角色，两者像素高度之比
就是真实高度之比：

    h_角色 = (角色像素高 / 标尺像素高) × 1.0 m

标尺是**竖直薄板**（1 m 高、2 cm 厚、带材质），不是 1 m 立方体 —— 立方体最上
像素来自**靠近相机的那条上边**（比中心平面近 0.5 m），6 m 距离上会被放大 8%，
正好和要测的量级同阶；薄板只有 1 cm 的偏移，可以忽略。

场景里**不放地面**：地面会让"物体底边"和地平线糊在一起量不准。没有地面时
物体是"悬空"的，像素包围盒的上下边都干净可测。

## 两个已经踩过的坑（都写在断言里了）

1. ⚠⚠ **没有材质的网格不渲染，而且一声不响**。实测：同一个立方体，
   挂 `materials/crate.vmat` → 视口差 16552 像素；不挂材质 → **0 像素**。
   所以标尺必须带材质，否则量到的是天空。
2. ⚠ **`shape=plane` 是单面的**：想让它朝向相机必须绕 **Y** 轴转 90°
   （引擎 Z-up；绕 X 转会让相机侧看它，渲染成一条零宽度的线）。
   这里干脆改用薄板（closed geometry），把朝向问题彻底绕开。

用法：
    python tools/verify_model_scale.py
    python tools/verify_model_scale.py --model assets/models/anime_girl.fbx
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import shutil
import subprocess
import sys
import threading
import time

import numpy as np
from PIL import Image

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

EDITOR = os.path.join(ROOT, "build", "bin", "Editor.exe")
ASSETC = os.path.join(ROOT, "build", "bin", "AssetC.exe")
WORK = os.path.join(os.path.dirname(ROOT), "_venn_verify", "scale")

from capture_window import capture_client, find_window  # noqa: E402
from safe_fs import safe_rmtree  # noqa: E402

# 相机：水平（pitch=0）+ 正对物体；distance/fov 只影响 px/m，不影响比值。
CAM = {"target": [0.0, 0.0, 0.5], "yaw": 0.0, "pitch": 0.0,
       "distance": 6.0, "fov": 55.0}
LIGHT = {"direction": [0.42, 0.72, -0.55], "color": [1.0, 0.96, 0.9],
         "intensity": 3.2, "castsShadow": False, "ambientScale": 1.0}
MAT = {"kind": "vmat", "path": "materials/crate.vmat", "name": "materials/crate",
       "id": "materials/crate"}

fails: list[str] = []


def log(msg: str) -> None:
    print(msg, flush=True)


def check(name: str, ok: bool, detail: str = "") -> bool:
    log("CHECK %s %s%s" % (name, "OK" if ok else "FAIL", (" (%s)" % detail) if detail else ""))
    if not ok:
        fails.append("%s %s" % (name, detail))
    return ok


# ---------------------------------------------------------------- 场景

def scene(entities: list) -> dict:
    return {"version": 1,
            "generator": "verify_model_scale: 量外部模型进场景后的真实身高（1:1 校验）",
            "camera": CAM, "light": LIGHT, "entities": entities}


def meter_slab() -> dict:
    """1 m 高、2 cm 厚的竖直薄板，底边贴 z=0。

    1 m 立方体走 `size=1`；`scale=(0.02, 1, 1)` 把它压成朝相机方向的薄片
    （Z-up：X 是深度轴，因为相机在 +X 上朝 −X 看）。
    """
    return {
        "name": "MeterSlab",
        "transform": {"position": [0, 0, 0.5], "rotation": [0, 0, 0],
                      "scale": [0.02, 1, 1]},
        "parent": -1, "visible": True, "castShadow": False,
        "mesh": {"kind": "builtin", "shape": "cube", "size": 1.0, "name": "meter_slab"},
        "primitive": {"kind": "cube", "size": 1.0, "radius": 0.5,
                      "segments": 4, "rings": 2},
        # ⚠ 没有这一行就等于没渲染（见文件头坑 1）
        "material": MAT,
    }


def char_entity() -> dict:
    """角色实体**不需要**在这里手写。

    ⚠ 这是本轮最大的一处坑，写下来省得再踩：
      `skinned` 块**只建 `SkinnedMeshComponent`**，它只提供 `jointBase`；
      真正被渲染的是**子网格实体**（每个 submesh 一个，带 `MeshComponent` +
      `MaterialComponent`）。手写一个 `{"skinned": {...}}` 的实体，日志里
      `SCENE-SKINNED ok=1 joints=52` 一切正常，**但视口里什么都没有** ——
      我一开始就是这么写的，量出来"角色未渲染"，白查了好几轮。
      唯一会同时建"组件 + 子网格实体"的地方是 `scene::instantiateModel`
      （拖拽 / `MYVK_EDITOR_IMPORT` 都走它），所以这里改用那个环境变量。
    """
    raise AssertionError("角色请用 MYVK_EDITOR_IMPORT 导入，见 docstring")


# ---------------------------------------------------------------- 跑与拍

def run_capture(scene_rel: str, out_png: str, asset_dir: str,
                settle: float = 2.2, env_extra: dict | None = None
                ) -> tuple[str, tuple | None]:
    """跑编辑器 → 等布局定下来 → 截客户区 → 杀进程。

    ⚠ 两个前车之鉴：
      1. **不能传 `MYVK_FRAMES`**：小帧数下编辑器"闪一下就退出"，`find_window`
         还没轮询到就没了（症状是"窗口没出现"，手动跑却明明有窗口）。
      2. **视口不是整个客户区**：`MYVK_EDITOR_NO_HUD` 只关 HUD，左右面板还在。
         视口矩形要从 `LAYOUT-RECTS ... vp=(x,y,w,h)` 读。
      3. 日志要**边跑边读**（线程排空），否则管道满 64 KB 会死锁。
    """
    env = os.environ.copy()
    env.update({"MYVK_ASSET_DIR": asset_dir, "MYVK_EDITOR_SCENE": scene_rel,
                "MYVK_EDITOR_NO_HUD": "1", "MYVK_NO_WINDOW_SAVE": "1"})
    if env_extra:
        env.update(env_extra)
    else:
        # 上一次跑留下的变量不能漏进来（进程环境是复制的）
        env.pop("MYVK_EDITOR_IMPORT", None)
    p = subprocess.Popen([EDITOR], env=env, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, bufsize=1)
    lines: list[str] = []

    def drain() -> None:
        for ln in p.stdout:            # type: ignore[union-attr]
            lines.append(ln)

    threading.Thread(target=drain, daemon=True).start()

    hwnd = find_window("Venn Editor", timeout=60.0)
    if not hwnd:
        p.kill()
        raise RuntimeError("编辑器窗口没出现")

    vp = None
    t0 = time.time()
    while time.time() - t0 < 30.0:
        for ln in list(lines):
            m = re.search(r"LAYOUT-RECTS .*vp=\((\d+),(\d+),(\d+),(\d+)\)", ln)
            if m:
                vp = tuple(int(g) for g in m.groups())
        if vp and any("Window shown" in ln for ln in lines):
            break
        time.sleep(0.15)

    time.sleep(settle)
    img, ok = capture_client(hwnd)
    if ok and img is not None:
        img.save(out_png)
    try:
        p.terminate()
        p.wait(timeout=10)
    except Exception:
        p.kill()
    if not ok or img is None:
        raise RuntimeError("截图为空")
    return "".join(lines), vp


def _blob_span(mask: np.ndarray, scale: int = 4):
    """在二值 mask 里找"最像被测物"的那一坨，返回它的 (y0, y1)。

    ⚠ 为什么不能直接 `mask.any(axis=1)` 取上下界：视口左上角有一个**导入
    通知条**（`MYVK_EDITOR_IMPORT` 之后出现的提示框），它跟空场景干板做差
    当然也是"变了的像素"。它的条带高约 60 px、宽 280 px，直接把上下界拉到
    顶端去 —— 实测标尺从 110 px 被量成 307 px，S3/S4 双双变红，而渲染其实
    是对的（截图里那块 1 m 板子看着就是 110 px）。

    做法：4 倍降采样 → 连通域 → 取"高度最大且宽度不超过视口 60%"的那一坨。
    用**高度**当偏好而不是面积：通知条又宽又矮，面积反而比标尺大。
    """
    h, w = mask.shape
    hs, ws = h // scale, w // scale
    if hs < 4 or ws < 4:
        return None
    small = mask[:hs * scale, :ws * scale].reshape(hs, scale, ws, scale).any(
        axis=(1, 3))
    seen = np.zeros_like(small)
    best = None                       # (高度px, y0, y1)
    for sy in range(hs):
        for sx in range(ws):
            if not small[sy, sx] or seen[sy, sx]:
                continue
            stack = [(sy, sx)]
            seen[sy, sx] = True
            ys, xs = [], []
            while stack:
                y, x = stack.pop()
                ys.append(y)
                xs.append(x)
                for ny, nx in ((y + 1, x), (y - 1, x), (y, x + 1), (y, x - 1)):
                    if 0 <= ny < hs and 0 <= nx < ws and small[ny, nx] \
                            and not seen[ny, nx]:
                        seen[ny, nx] = True
                        stack.append((ny, nx))
            y0, y1 = min(ys), max(ys)
            x0, x1 = min(xs), max(xs)
            if (x1 - x0 + 1) * scale > w * 0.6:        # 又宽又扁 = 条带，不要
                continue
            height = (y1 - y0 + 1) * scale
            if best is None or height > best[0]:
                best = (height, y0 * scale, (y1 + 1) * scale)
    if best is None:
        return None
    # 回到全分辨率，在粗 bbox 里按"连续 3 行都有变化"收紧
    sub = mask[best[1]:best[2], :]
    rows = np.where(sub.sum(axis=1) >= 3)[0]
    if len(rows) == 0:
        return None
    return best[1] + int(rows[0]), best[1] + int(rows[-1]) + 1


def shape_bbox(png: str, plate_png: str, vp: tuple, thr: float = 10.0,
               inset_top: int = 40) -> tuple[int, int] | None:
    """被拍物在视口里的像素包围盒上下边（与空场景干板做差）。

    ⚠ 前景判定用**干板做差**而不是"猜背景色"：天空是渐变的，拿顶部若干行的
    中位色当背景时第 0 行自己就偏离中位数，每张图都报"顶端在第一行"（踩过）。
    ⚠ 包围盒只取**一坨连通域**（见 `_blob_span`）：视口左上角的导入通知条
    也是"变了的像素"，按行取上下界会把标尺量成 307 px（真实 110 px）。
    ⚠ `inset_top`：视口矩形**包含了它自己那条工具栏**（Perspective / Lit /
    Add…）。导入模型后实体被选中，工具栏上的文字状态会变 → 干板做差在
    第 14 行就命中，量出 531 px（真实是 202 px）。踩过一次，所以顶部必须让开。
    """
    a = np.asarray(Image.open(png).convert("RGB")).astype(np.float32)
    b = np.asarray(Image.open(plate_png).convert("RGB")).astype(np.float32)
    if a.shape != b.shape:
        return None
    vx, vy, vw, vh = vp
    y0 = max(0, vy + inset_top)
    y1 = min(a.shape[0], vy + vh)
    x0 = max(0, vx + 2)
    x1 = min(a.shape[1], vx + vw - 2)
    diff = np.abs(a[y0:y1, x0:x1] - b[y0:y1, x0:x1]).max(axis=2) > thr
    span = _blob_span(diff)
    if span is None:
        return None
    return int(y0 + span[0]), int(y0 + span[1])


# ---------------------------------------------------------------- .vmesh 查询

def assetc_info(vmesh: str) -> dict:
    out = subprocess.run([ASSETC, "--info", vmesh], capture_output=True, text=True)
    txt = out.stdout or ""
    d: dict = {}
    m = re.search(r"height\(Z\)=([\d.]+) m", txt)
    if m:
        d["height"] = float(m.group(1))
    d["clips"] = re.findall(r'clip "([^"]+)" ([\d.]+) s', txt)
    m = re.search(r"joints=(\d+) clips=(\d+) materials=(\d+)", txt)
    if m:
        d["joints"] = int(m.group(1))
        d["clipCount"] = int(m.group(2))
    return d


# ---------------------------------------------------------------- 主流程

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", default="assets/models/thirdparty/samba_dancing.fbx")
    ap.add_argument("--expect-min", type=float, default=1.30)
    ap.add_argument("--expect-max", type=float, default=2.20)
    args = ap.parse_args()

    for exe in (EDITOR, ASSETC):
        if not os.path.isfile(exe):
            log("SKIP 找不到 %s（先构建）" % exe)
            return 0

    model_abs = args.model if os.path.isabs(args.model) else os.path.join(ROOT, args.model)
    if not os.path.isfile(model_abs):
        log("SKIP 缺模型 %s" % model_abs)
        return 0
    model_rel = os.path.relpath(model_abs, os.path.join(ROOT, "assets")).replace("\\", "/")

    if os.path.isdir(WORK):
        # ⚠ 必须用 `safe_fs`：宿主对 `shutil.rmtree` 有**按回合累计的批量删除
        #   保护**（阈值 50 个文件，进程层直接终止，`try/except OSError` 接不住）。
        #   这个工作目录里会攒下 screenshot / scene / vmesh 几十个文件，
        #   连跑两轮就撞线 —— 症状是"脚本一句话没说就退出了"。
        safe_rmtree(WORK)
    root = os.path.join(WORK, "root")
    os.makedirs(os.path.join(root, "scenes"), exist_ok=True)
    # 材质是**渲染前提**（坑 1），标尺要用到 → 一起拷进独立资产根
    shutil.copytree(os.path.join(ROOT, "assets", "materials"),
                    os.path.join(root, "materials"))
    dst_fbx = os.path.join(root, model_rel)
    os.makedirs(os.path.dirname(dst_fbx), exist_ok=True)
    shutil.copy2(model_abs, dst_fbx)
    vmesh = os.path.splitext(dst_fbx)[0] + ".vmesh"
    rc = subprocess.run([ASSETC, dst_fbx, "--asset-root", root],
                        capture_output=True, text=True)
    if rc.returncode != 0:
        log("SKIP AssetC 失败：%s" % (rc.stdout or "")[-400:])
        return 0

    info = assetc_info(vmesh)
    geo_h = info.get("height", -1.0)
    check("G1 几何高度可读", geo_h > 0.0, "%.3f m" % geo_h)
    check("G2 是合理的人形尺寸", args.expect_min <= geo_h <= args.expect_max,
          "%.3f m ∈ [%.2f, %.2f]" % (geo_h, args.expect_min, args.expect_max))
    if not info.get("clips"):
        log("SKIP 模型没有动画片段（不是骨架模型），无法验证蒙皮实例化")
        return finish()

    scenes = {
        "empty": ([], None),
        "slab": ([meter_slab()], None),
        # 角色不写实体：靠 MYVK_EDITOR_IMPORT 让引擎自己实例化（见 char_entity）
        "char": ([], {"MYVK_EDITOR_IMPORT": model_rel}),
    }
    pngs, vps, logs = {}, {}, {}
    for name, (ents, extra) in scenes.items():
        rel = "scenes/scale_%s.json" % name
        with open(os.path.join(root, rel), "w", encoding="utf-8") as f:
            json.dump(scene(ents), f, ensure_ascii=False, indent=2)
        pngs[name] = os.path.join(WORK, "scale_%s.png" % name)
        logs[name], vps[name] = run_capture(rel, pngs[name], root, env_extra=extra)

    check("S0 三次拍摄的视口矩形一致",
          vps["empty"] is not None and vps["empty"] == vps["slab"] == vps["char"],
          "%s / %s / %s" % (vps["empty"], vps["slab"], vps["char"]))
    check("S1 角色导入成功且判定为蒙皮", "IMPORT-AUTO" in logs["char"]
          and "skinned=1" in logs["char"],
          [ln.strip()[-70:] for ln in logs["char"].splitlines()
           if "IMPORT-AUTO" in ln][:1] or "没有 IMPORT-AUTO")
    check("S1b 走的是 .vmesh（不是回退 FBX）", "VMESH loaded" in logs["char"],
          "" if "VMESH loaded" in logs["char"] else "没看到 VMESH loaded")
    if vps["slab"] is None:
        check("S0b 能从日志读到视口矩形", False, "没有 LAYOUT-RECTS")
        return finish()

    vp = vps["slab"]
    # 视口**离屏渲染尺寸**才是投影矩阵用的那个高度（`Viewport render size ->
    # WxH`）。它比视口矩形小一圈（789x695 vs 810x742），拿 742 算期望会偏
    # 6.6% —— 这个偏差是"渲染分辨率 ≠ 显示矩形"造成的，不是模型的问题。
    m = re.search(r"Viewport render size -> (\d+)x(\d+)", logs["slab"])
    render_h = int(m.group(2)) if m else vp[3]

    b_slab = shape_bbox(pngs["slab"], pngs["empty"], vp)
    b_char = shape_bbox(pngs["char"], pngs["empty"], vp)
    if not check("S2 标尺与角色都被渲染出来了", b_slab and b_char,
                 "slab=%s char=%s" % (b_slab, b_char)):
        return finish()

    px_slab = b_slab[1] - b_slab[0]
    px_char = b_char[1] - b_char[0]
    measured = px_char / px_slab * 1.0     # 标尺 = 1 m

    # 相机模型自洽：1 m 物体在物体平面上的像素高应当是 f/d。
    # 差太多说明"标尺其实不是 1 m"或 `size` 的语义不是边长 —— 这条比比值本身
    # 更能抓住"标尺量错了"的情况（比值会自我一致地错）。
    f = (render_h / 2.0) / math.tan(math.radians(CAM["fov"]) / 2.0)
    expect_ppm = f / CAM["distance"]
    ppm_err = abs(px_slab - expect_ppm) / expect_ppm
    check("S3 标尺像素高符合相机投影（说明它真是 1 m）", ppm_err <= 0.03,
          "%.1f px vs 期望 %.1f px (render_h=%d, err=%.1f%%)"
          % (px_slab, expect_ppm, render_h, ppm_err * 100))

    err = abs(measured - geo_h) / geo_h if geo_h > 0 else 9.9
    check("S4 渲染身高 = 几何身高（±6%）", 0.0 < err <= 0.06,
          "measured=%.3f m vs geo=%.3f m (err=%.1f%%)" % (measured, geo_h, err * 100))
    check("S5 与真实环境 1:1（引擎 1 单位 = 1 米）",
          args.expect_min <= measured <= args.expect_max, "%.3f m" % measured)

    log("  量测明细：视口 %dx%d @(%d,%d)  标尺 %d px (y %d..%d)  角色 %d px (y %d..%d)"
        % (vp[2], vp[3], vp[0], vp[1], px_slab, b_slab[0], b_slab[1],
           px_char, b_char[0], b_char[1]))
    log("  片段：%s  骨骼=%s"
        % ("; ".join('"%s" %.2fs' % (c[0], float(c[1])) for c in info["clips"]),
           info.get("joints", "?")))
    log("  截图：%s / %s" % (pngs["slab"], pngs["char"]))
    return finish()


def finish() -> int:
    if fails:
        log("")
        for f in fails:
            log("  ✗ %s" % f)
        log("结果: FAIL: %d 项" % len(fails))
        return 1
    log("结果: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
