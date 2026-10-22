#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/verify_multi_terrain.py —— 多块地形不再撞描述符池上限

## 它看守的那个 bug（2026-10-22 用户报障）

```
[VK_CHECK] Vulkan error -1000069000 at engine/rhi/DescriptorSet.inl:61
[ERROR] [validation] vkDestroyDevice(): VkDevice ... has 3 leaked objects
VkSampler 0xc27..., 0xc28..., 0xc29...
[ERROR] Fatal: Vulkan call failed
```

`-1000069000` = `VK_ERROR_OUT_OF_POOL_MEMORY`。`DescriptorSet.inl:61` 是
`vkAllocateDescriptorSets` —— 那句话在 `DescriptorAllocator` 里，**所有**
pass 的描述符分配都走它，所以行号只能说明"某个池满了"，指不出是哪个。

定位结论：`TerrainPass` 的池按 **4 块地形**写死（`4 sets / 44 samplers`，
每块地形 = 1 set + 11 sampler：4 层 albedo + 4 层 normal + splat + height
+ hole）。场景里第 5 块地形一进来就炸。复现方式极其普通：把模板场景 copy
一份、再复制 4 个 Terrain 实体。

那 3 个泄漏的 sampler 是**次生**现象，但很有诊断价值：`createTerrain` 里
sampler 建在描述符集**之前**，`allocate` 一 throw，那个 Instance 还挂在
局部 `unique_ptr` 上、从没进过 `m_instances` —— 裸句柄没人回收。

## 修法（两个层次，缺一不可）

1. `DescriptorAllocator` 改成**池链**：`VK_ERROR_OUT_OF_POOL_MEMORY` /
   `VK_ERROR_FRAGMENTED_POOL` 时追加一个同规格的池重试（上限 8 个）。
   容量估小 = 多建一个池，不再是崩溃。
2. `TerrainPass::Instance` 补析构函数回收 3 个 sampler —— 异常路径也要干净。

## 断言

  T1  场景里 N 块地形全部起得来（`TERRAIN-INSTANCE` 行数 == N）
  T2  **零** `[VK_CHECK]`（池满不再抛）
  T3  **零** `leaked objects`（裸 sampler 被回收）
  T4  走了扩容（`DESCRIPTOR-POOL 扩容` 至少一条 —— 证明这条路径真的被执行，
      而不是"恰好没踩到"）
  T5  退出码 0，`Application shutdown` 有

用法：
    python tools/verify_multi_terrain.py
"""

import json
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import launcher_kit as kit  # noqa: E402

KIND = "third_person"
NAME = "MultiTerrain"
# 6 块：首池 4 块，第 5、6 块必然触发扩容。留 2 块余量而不是刚好 5，
# 是为了在"首池容量被谁调大/调小"时仍然稳定压在扩容路径上。
TERRAINS = 6
FRAMES = 120          # 2 秒 @60fps：够建完所有实例 + 画几帧


def make_project(work):
    """模板 copy 成工程，再把 Terrain 实体复制成 TERRAINS 块。"""
    root = os.path.join(work, NAME)
    if os.path.isdir(root):
        # ⚠ 用 fresh_dir 的"改名让位"而不是 rmtree：宿主有批量删除保护
        kit.fresh_dir(root)
    os.makedirs(root, exist_ok=True)
    shutil.copytree(os.path.join(kit.TEMPLATES, KIND), root, dirs_exist_ok=True)

    scene_path = os.path.join(root, "assets", "scenes", "main.json")
    with open(scene_path, encoding="utf-8") as f:
        scene = json.load(f)
    base = next(e for e in scene["entities"] if "terrain" in e)
    # ⚠ 每块地形**共用同一个资产路径**：这条用例测的是"实例数 / 描述符容量"，
    #   不是"磁盘上有几份资产"。共用正好把变量压到只剩实例数。
    for i in range(1, TERRAINS):
        t = json.loads(json.dumps(base))          # deep copy
        t["name"] = "Terrain%d" % (i + 1)
        t["transform"]["position"] = [float(700 * i), 0.0, 0.0]
        scene["entities"].append(t)
    with open(scene_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(scene, f, ensure_ascii=False, indent=2)
        f.write("\n")

    # game.json 标题（与本用例的断言无关，但少了它日志里认不出是哪个工程）
    gj = os.path.join(root, "assets", "game.json")
    with open(gj, encoding="utf-8") as f:
        g = json.load(f)
    g["title"] = NAME
    with open(gj, "w", encoding="utf-8", newline="\n") as f:
        json.dump(g, f, ensure_ascii=False, indent=2)
        f.write("\n")
    return root


def main():
    if not os.path.isfile(kit.EDITOR):
        kit.log("结果: FAIL: Editor.exe 不存在（先 make build）")
        return 1

    work = os.path.join(kit.WORK_ROOT, "multi_terrain")
    work = kit.fresh_dir(work)
    os.makedirs(work, exist_ok=True)
    kit.log("PROBE work=%s terrains=%d" % (work, TERRAINS))

    root = make_project(work)
    assets = os.path.join(root, "assets")

    env = kit.clean_env({
        "MYVK_ASSET_DIR": assets,
        "MYVK_EDITOR_SCENE": "scenes/main.json",
        "MYVK_FRAMES": str(FRAMES),
        # 不写 imgui.ini：本用例反复跑，别污染用户的布局
        "MYVK_NO_WINDOW_SAVE": "1",
        # 验证层必须开：T2/T3 的两条断言全靠它报
        "MYVK_VALIDATION": "1",
    })

    proc = subprocess.run([kit.EDITOR], cwd=root, env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, encoding="utf-8", errors="replace",
                          timeout=300)
    out = proc.stdout or ""
    with open(os.path.join(work, "multi_terrain.log"), "w",
              encoding="utf-8") as f:
        f.write(out)

    fails = []

    # ---- T1 每块地形都建起来了 ----
    n_inst = len(re.findall(r"TERRAIN-INSTANCE path=", out))
    if n_inst != TERRAINS:
        fails.append("T1 TERRAIN-INSTANCE 只建了 %d 块，期望 %d"
                     % (n_inst, TERRAINS))
    else:
        kit.log("CHECK T1 %d blocks of terrain instantiated OK" % n_inst)

    # ---- T2 零 VK_CHECK（池满不再 throw）----
    vk = [l for l in out.splitlines() if "[VK_CHECK]" in l]
    if vk:
        fails.append("T2 出现 VK_CHECK（描述符池又满了）: %s" % vk[:2])
    else:
        kit.log("CHECK T2 no VK_CHECK / no OUT_OF_POOL_MEMORY OK")

    # ---- T3 零泄漏 ----
    leaks = [l for l in out.splitlines() if "leaked objects" in l]
    if leaks:
        fails.append("T3 vkDestroyDevice 报告泄漏: %s" % leaks[:2])
    else:
        kit.log("CHECK T3 no leaked objects OK")

    # ---- T4 确实走了扩容 ----
    #
    # ⚠ 这条是**前提断言**，不是装饰：如果哪天首池被调大到 >= 6 块，
    #   T2/T3 会"因为没有走到扩容而通过"—— 那不是修好的证据，只是没测到。
    #   所以要么看到扩容日志，要么明确写着"没触发"。
    grew = [l for l in out.splitlines() if "DESCRIPTOR-POOL" in l]
    if not grew:
        fails.append("T4 没有 DESCRIPTOR-POOL 扩容日志 —— 本用例根本没压在"
                     "扩容路径上（首池容量被调大了？要么提高 TERRAINS，"
                     "要么改回这个断言）")
    else:
        kit.log("CHECK T4 %s OK" % grew[0].split("] ", 1)[-1])
        if len(grew) > 1:
            kit.log("         共扩容 %d 次" % len(grew))

    # ---- T5 正常退出 ----
    if proc.returncode != 0:
        fails.append("T5 退出码 %d，期望 0" % proc.returncode)
    elif "Application shutdown" not in out:
        fails.append("T5 没有 'Application shutdown'（onShutdown 没跑完）")
    else:
        kit.log("CHECK T5 exit rc=0 + shutdown OK")

    return kit.finish(fails)


if __name__ == "__main__":
    sys.exit(main())
