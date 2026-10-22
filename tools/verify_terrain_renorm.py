#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/verify_terrain_renorm.py —— 抬山保存后资产"变砖"与它的连锁反应

## 用户报障（2026-10-22 第十一轮）

```
[ERROR] TERRAIN-INSTANCE-FAIL path=terrain/ground/terrain.vterrain
        reason=height_not_normalized min=0.000 max=8.000
[ERROR] TERRAIN-CREATE-FAIL entity=1 path=terrain/ground/terrain.vterrain
[VK_CHECK] Vulkan error -4 at engine/render/Renderer.cpp:6619   # vkQueueSubmit
[ERROR] Fatal: Vulkan call failed
```

`-4` 是 `VK_ERROR_DEVICE_LOST`，但它出现在 `vkQueueSubmit` —— 那是"GPU 已经
没了"的**次生**表现，不是根因。真正的原因有三层，一层叠一层：

### ① 写端与读端的值域契约互相矛盾（根因）

抬升笔刷**故意**允许高度冲到 `kMaxNorm = 8`（不留余量的话，笔心先撞 1.0、
外圈继续涨，收尾必然是一块"平顶山"）。而 `.r32` 的契约是归一化 `[0,1]`，
老版本 `saveAssets()` 又"内存里是什么就写什么"：

    抬山 → 保存 → 磁盘 max=8.000 → 下次读盘 height_not_normalized → 打不开

`max=8.000` 之所以是**整数**，就是撞到了 `kMaxNorm` 那个 clamp。

### ② 失败被"放大"成每帧全量重建（→ DEVICE_LOST）

`syncTerrains()` 判断"集合变没变"用的是
`items.size() == m_terrainEntities.size()`，而 `m_terrainEntities` **只装
创建成功的**。只要有一个实体建失败，这个等式永远不成立 → 每一帧都
`clearInstances()`（`waitIdle` + 销毁 sampler/描述符集 + 读 500 KB +
重传 3 MB 顶点）。单帧上百毫秒 → 冲破 Windows TDR 的 2 秒上限 → 驱动
重置 → `DEVICE_LOST`。

也就是说："一份打不开的资产"被放大成了"整个编辑器被 GPU 丢掉"。

### ③ 存量坏数据无法自愈

读端硬失败 → 用户连打开都做不到，更别提重新保存。

## 修法

* `loadTerrain` 遇到 `max > 1` 不再硬失败，而是按最高点重整：
  `heights /= hi`、`maxHeight *= hi`。世界高度是 `h × maxHeight`，
  同乘同除**结果恒等** —— 地形长得一模一样，数值回到规范区间。
  打 `TERRAIN-RENORMALIZE`，并置 `TerrainAsset::rescaled`。
* `saveAssets()` 落盘前把内存那份收敛回 `[0,1]`（连同 `maxHeight` 与
  撤销栈一起缩放），保证磁盘上永远规范。这样存量坏数据"打开一次、
  保存一次"就自愈了。
* `syncTerrains()` 的判据换成"上次**请求过**的全集"（`m_terrainRequested`），
  并且失败后按 `kTerrainRetryFrames`（120 帧 ≈ 2 秒）节流重试。

## 三组用例

  A  干净模板（控制组）—— **不允许**出现 RENORMALIZE（救援路径不能误伤）
  B  高度 ×8 的坏资产 —— 必须救援、地形必须建出来、零 VK_CHECK、
     `maxHeight` 恰好被放大成 `8 × 5.193805 = 41.5504`
  C  尺寸被砍半的坏资产 —— 建不出来，但失败日志**不能逐帧刷屏**
     （这正是 DEVICE_LOST 的成因），且要看到 `TERRAIN-DEGRADED` 节流

用法：
    python tools/verify_terrain_renorm.py
"""

import json
import os
import re
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import launcher_kit as kit  # noqa: E402

KIND = "third_person"
FRAMES = 120
# 模板里的 maxHeight（见 tools/gen_templates.py 的 terrain_max_height()）
TPL_MAXH = 5.193805
# 抬山撞顶的那个倍数（LandscapeEditor.cpp 的 kMaxNorm）
PEAK_NORM = 8.0


def make_project(work, name):
    """模板 copy 成工程。返回 (root, assets, vterrain_abs)。"""
    root = os.path.join(work, name)
    os.makedirs(root, exist_ok=True)
    shutil.copytree(os.path.join(kit.TEMPLATES, KIND), root, dirs_exist_ok=True)

    gj = os.path.join(root, "assets", "game.json")
    with open(gj, encoding="utf-8") as f:
        g = json.load(f)
    g["title"] = name
    with open(gj, "w", encoding="utf-8", newline="\n") as f:
        json.dump(g, f, ensure_ascii=False, indent=2)
        f.write("\n")

    assets = os.path.join(root, "assets")
    vt = os.path.join(assets, "terrain", "ground", "terrain.vterrain")
    return root, assets, vt


def run(assets, root, work, tag):
    env = kit.clean_env({
        "MYVK_ASSET_DIR": assets,
        "MYVK_EDITOR_SCENE": "scenes/main.json",
        "MYVK_FRAMES": str(FRAMES),
        "MYVK_NO_WINDOW_SAVE": "1",
        "MYVK_VALIDATION": "1",
    })
    proc = subprocess.run([kit.EDITOR], cwd=root, env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, encoding="utf-8", errors="replace",
                          timeout=300)
    out = proc.stdout or ""
    with open(os.path.join(work, "%s.log" % tag), "w", encoding="utf-8") as f:
        f.write(out)
    return proc, out


def main():
    if not os.path.isfile(kit.EDITOR):
        kit.log("结果: FAIL: Editor.exe 不存在（先 make build）")
        return 1

    work = kit.fresh_dir(os.path.join(kit.WORK_ROOT, "terrain_renorm"))
    os.makedirs(work, exist_ok=True)
    fails = []

    # ========================================================
    # A 控制组：干净模板不该被"救援"
    # ========================================================
    root, assets, vt = make_project(work, "RenormClean")
    proc, out = run(assets, root, work, "a_clean")

    if "TERRAIN-RENORMALIZE" in out:
        fails.append("A 干净模板也触发了 TERRAIN-RENORMALIZE —— 救援路径误伤")
    else:
        kit.log("CHECK A 干净资产未触发重整 OK")

    if not re.search(r"TERRAIN-INSTANCE path=", out):
        fails.append("A 干净模板的地形没建出来")
    else:
        kit.log("CHECK A 干净模板地形实例正常 OK")

    # ========================================================
    # B 高度 ×8（模拟"抬山撞顶后保存"）
    # ========================================================
    root, assets, vt = make_project(work, "RenormBad")
    with open(vt, encoding="utf-8") as f:
        vj = json.load(f)
    cols, rows = vj["cols"], vj["rows"]
    hp = os.path.join(os.path.dirname(vt), vj.get("height", "height.r32"))
    n = cols * rows
    with open(hp, "rb") as f:
        hs = list(struct.unpack("<%df" % n, f.read(4 * n)))
    scaled = [h * PEAK_NORM for h in hs]
    with open(hp, "wb") as f:
        f.write(struct.pack("<%df" % n, *scaled))
    kit.log("PROBE B 把 height.r32 整体 ×%.0f（峰值 %.3f → %.3f，maxHeight 仍是 %.6f）"
            % (PEAK_NORM, max(hs), max(scaled), vj["maxHeight"]))

    proc, out = run(assets, root, work, "b_scaled")

    m = re.search(r"TERRAIN-RENORMALIZE path=\S+ peak=([\d.]+) "
                  r"maxHeight ([\d.]+) -> ([\d.]+)", out)
    if not m:
        fails.append("B1 没有 TERRAIN-RENORMALIZE —— 坏资产仍然被硬拒")
    else:
        peak, old, new = float(m.group(1)), float(m.group(2)), float(m.group(3))
        kit.log("CHECK B1 救援触发 peak=%.3f maxHeight %.4f -> %.4f OK"
                % (peak, old, new))
        if abs(peak - PEAK_NORM) > 0.05:
            fails.append("B1 peak 报的是 %.3f，期望 ≈%.0f" % (peak, PEAK_NORM))
        # 世界高度恒等：新 maxHeight 必须等于 old × peak
        want = old * peak
        if abs(new - want) > 1e-2:
            fails.append("B2 maxHeight 重整后 = %.4f，期望 %.4f（世界高度没守住）"
                         % (new, want))
        else:
            kit.log("CHECK B2 世界高度恒等 maxHeight=old×peak=%.4f OK" % new)

    # 救援之后地形必须真的建出来 —— 这是"工程能打开"的可观测代理
    if not re.search(r"TERRAIN-INSTANCE path=", out):
        fails.append("B3 救援后地形仍然没建出来")
    else:
        kit.log("CHECK B3 救援后地形实例建成 OK")

    # 用户报的那三行，一条都不许再出现
    for tag, needle in (("B4", "TERRAIN-INSTANCE-FAIL"),
                        ("B5", "TERRAIN-CREATE-FAIL"),
                        ("B6", "height_not_normalized")):
        if needle in out:
            fails.append("%s 仍然出现 %s" % (tag, needle))
        else:
            kit.log("CHECK %s 不再出现 %s OK" % (tag, needle))

    vk = [l for l in out.splitlines() if "[VK_CHECK]" in l]
    if vk:
        fails.append("B7 出现 VK_CHECK（DEVICE_LOST 没治好）: %s" % vk[:2])
    else:
        kit.log("CHECK B7 no VK_CHECK / no DEVICE_LOST OK")

    if "leaked objects" in out:
        fails.append("B8 vkDestroyDevice 报告泄漏")
    else:
        kit.log("CHECK B8 no leaked objects OK")

    # ========================================================
    # C 尺寸砍半（永远修不好）→ 测的是"不逐帧重试"
    # ========================================================
    root, assets, vt = make_project(work, "RenormTrunc")
    with open(vt, encoding="utf-8") as f:
        vj = json.load(f)
    cols, rows = vj["cols"], vj["rows"]
    hp = os.path.join(os.path.dirname(vt), vj.get("height", "height.r32"))
    with open(hp, "rb") as f:
        raw = f.read()
    with open(hp, "wb") as f:
        f.write(raw[: len(raw) // 2])          # 尺寸不对 → height_size
    kit.log("PROBE C height.r32 砍半 %d → %d 字节（永远修不好）"
            % (len(raw), len(raw) // 2))

    proc, out = run(assets, root, work, "c_trunc")

    # ⚠ 这条是**本用例的核心**。修复前是"每帧一次"，120 帧 = 120 条；
    #   那 120 次 waitIdle + 重建正是 TDR → DEVICE_LOST 的成因。
    #   修复后：首帧 1 次 + 120 帧放行 1 次重试 = 2 次，留 1 次余量。
    n_fail = len(re.findall(r"TERRAIN-CREATE-FAIL", out))
    if n_fail > 3:
        fails.append("C1 TERRAIN-CREATE-FAIL 出现 %d 次（%d 帧）—— 还在逐帧"
                     "重试，这正是 DEVICE_LOST 的成因" % (n_fail, FRAMES))
    else:
        kit.log("CHECK C1 失败只记录 %d 次 / %d 帧（没有逐帧重建）OK"
                % (n_fail, FRAMES))
    if n_fail == 0:
        fails.append("C1 一次 TERRAIN-CREATE-FAIL 都没有 —— 用例没压在失败路径上")

    if "TERRAIN-DEGRADED" not in out:
        fails.append("C2 没有 TERRAIN-DEGRADED —— 节流标记没生效")
    else:
        line = next(l for l in out.splitlines() if "TERRAIN-DEGRADED" in l)
        kit.log("CHECK C2 %s OK" % line.split("] ", 1)[-1])

    vk = [l for l in out.splitlines() if "[VK_CHECK]" in l]
    if vk:
        fails.append("C3 坏资产把 GPU 拖进了 DEVICE_LOST: %s" % vk[:2])
    else:
        kit.log("CHECK C3 坏资产不再导致 DEVICE_LOST OK")

    if proc.returncode != 0:
        fails.append("C4 退出码 %d，期望 0（坏资产不该让编辑器崩）"
                     % proc.returncode)
    else:
        kit.log("CHECK C4 exit rc=0 OK")

    return kit.finish(fails)


if __name__ == "__main__":
    sys.exit(main())
