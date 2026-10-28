#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""验收：FBX take 名归一化后的**重名去重**（任务 #329）。

背景（这条用例为什么必须存在）
--------------------------------------------------------------------------
Blender 的 `bake_anim_use_all_actions` 把 take 名写成 `<armature>|<action>`，
导入时一律剥掉 `|` 前缀（Unity / UE / glTF 转换器都这么做，否则
`Skeleton::findClip("Idle")` 的逐字比较找不到 → 角色静默退回 T-pose）。

剥完之后**会撞名**：`anime_girl.fbx` 里是 4 个 AnimationStack ——
`Rig|Idle` ×2 + `Rig|Walk` ×2，归一化后 `[Idle, Walk, Idle, Walk]`。
撞名的代价不是"列表难看"：

  · `findClip(name)` 逐字取第一个 → 第二段永远播不到；
  · 角色窗口片段列表两行同名 → 两行 `Selectable` 同帧同 ID → ImGui 弹
    **模态**告警窗（"2 visible items with conflicting ID!"），之后**所有鼠标
    点击全被它吃掉** —— 界面看着完全正常，只是点哪都没反应
    （`verify_character_window` 的"点动画条目触发播放"就是这么红的）。

规则在 `assetio/ClipDedup.h`，**AssetC 与引擎 FbxLoader 共用同一份实现**
（这两个提取器本来是同一段逻辑的两份拷贝 —— 各写一遍，就是同一个 bug
犯两次）。本用例盯的正是"两边都治了"：

  A. 离线（AssetC 烤）—— 2 条 `action=drop`、输出 `clips=2`、名字唯一、
     时长与轨道数**一点没变**（去重只该少一段，不该改内容）；
  B. **不误伤** —— alicia 的 4 段名字本来就不同，必须 0 条 `FBX-CLIPDUP`、
     `clips=4` 原样。只测 A 不测 B 的话，"把动画全丢了"也能过；
  C. 运行时（FbxLoader 直读 .fbx）—— 同样是 2 条 drop + 收尾行
     `FBX clips count=2 names=[Idle,Walk]`。⚠ C 不能只断言 drop 条数：
     全丢光也是 2 条 drop，必须看收尾行的**段数 + 名字序列**；
  D. 仓库里的 `assets/models/anime_girl.vmesh` **已经重烤过** —— 它是签入
     仓库的产物，只改导入代码不重烤，磁盘上那份还是 4 段（谁读 .vmesh
     谁中招）；
  E. 全程零 `[ERROR]` / 零 Vulkan VUID。

用法：
    python tools/verify_clip_dedup.py
"""

import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
ASSETC = os.path.join(BIN, "AssetC.exe")
EDITOR = os.path.join(BIN, "Editor.exe")
ASSETS = os.path.join(ROOT, "assets")
WORK = os.path.join(os.path.dirname(ROOT), "_venn_verify", "clipdedup")

SRC_DUP = os.path.join(ASSETS, "models", "anime_girl.fbx")     # 有重名
SRC_OK = os.path.join(ASSETS, "models", "characters", "alicia.fbx")  # 无重名
COOK_DUP = os.path.join(WORK, "anime_girl.vmesh")
COOK_OK = os.path.join(WORK, "alicia.vmesh")

RE_DUP = re.compile(
    r"FBX-CLIPDUP take='([^']*)' name='([^']*)' dur=([\d.]+) "
    r"action=(drop|rename)(?: to='([^']*)')?")
RE_SUM = re.compile(r"ASSETC\s+submeshes=(\d+) joints=(\d+) clips=(\d+) "
                    r"materials=(\d+)")
RE_INFO = re.compile(r"ASSETC\s+joints=(\d+) clips=(\d+) materials=(\d+)")
RE_CLIP = re.compile(r'ASSETC\s+clip "([^"]*)" ([\d.]+) s\s+tracks=(\d+)')
RE_RT_CLIPS = re.compile(r"FBX clips count=(\d+) names=\[([^\]]*)\]")
RE_VUID = re.compile(r"VUID-|Validation Error|\[ERROR\]")

fails = []


def check(name, ok, detail=""):
    print("%-4s %s%s" % ("PASS" if ok else "FAIL", name,
                         ("  (" + detail + ")") if detail else ""),
          flush=True)
    if not ok:
        fails.append(name)
    return ok


def run(cmd, env_extra=None, timeout=180):
    env = os.environ.copy()
    if env_extra:
        env.update(env_extra)
    p = subprocess.run(cmd, cwd=BIN, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True,
                       encoding="utf-8", errors="replace",
                       env=env, timeout=timeout)
    return p.returncode, (p.stdout or "")


def dups(out):
    """→ [(take, name, dur, action, newName)]"""
    return [(m.group(1), m.group(2), float(m.group(3)), m.group(4),
             m.group(5) or "") for m in RE_DUP.finditer(out)]


def clips_of(out):
    return [(m.group(1), float(m.group(2)), int(m.group(3)))
            for m in RE_CLIP.finditer(out)]


# ------------------------------------------------------------------ A / B
def case_cook(name, src, out_path, want_clips, want_dup):
    """烤一份，返回 (summary, clips, dup_lines, full_out)"""
    os.makedirs(WORK, exist_ok=True)
    if os.path.isfile(out_path):
        os.remove(out_path)
    code, out = run([ASSETC, src, "--asset-root", ASSETS, "-o", out_path])
    d = dups(out)
    check("%s AssetC 退出码 0" % name, code == 0, "rc=%d" % code)
    check("%s 输出文件已生成" % name, os.path.isfile(out_path))
    if not os.path.isfile(out_path):
        return None, [], d, out
    _, info = run([ASSETC, "--info", out_path])
    m = RE_INFO.search(info)
    cl = clips_of(info)
    clips = int(m.group(2)) if m else -1
    names = [c[0] for c in cl]
    check("%s 段数 = %d" % (name, want_clips), clips == want_clips,
          "clips=%d" % clips)
    check("%s 名字两两不同（撞名同 ID 的根因没了）" % name,
          len(names) == len(set(names)), ",".join(names))
    # FBX-CLIPDUP 的条数与 action 必须对得上
    if want_dup == 0:
        check("%s 没有 FBX-CLIPDUP（正常资产不误伤）" % name, not d,
              "共 %d 条" % len(d))
    else:
        check("%s 有 %d 条 FBX-CLIPDUP 且全为 action=drop"
              % (name, want_dup),
              len(d) == want_dup and all(x[3] == "drop" for x in d),
              "; ".join("%s/%s/%s" % (x[0], x[1], x[3]) for x in d))
        check("%s take 名保留了 armature 前缀（可追溯到源）" % name,
              all("|" in x[0] for x in d),
              ",".join(x[0] for x in d))
    return (clips, cl), cl, d, info


def main():
    for exe in (ASSETC, EDITOR):
        if not os.path.isfile(exe):
            print("SKIP 缺少 %s" % exe)
            return 0
    if not (os.path.isfile(SRC_DUP) and os.path.isfile(SRC_OK)):
        print("SKIP 缺少 FBX 源资产")
        return 0

    # ================= A. 有重名的资产：去重 =================
    print("\n== A 离线烤 anime_girl（4 个栈 / 2 个名字）==")
    a, a_clips, a_dup, a_out = case_cook(
        "A", SRC_DUP, COOK_DUP, want_clips=2, want_dup=2)
    if a:
        # 时长必须原样：去重只该少一段，不该改内容
        durs = {c[0]: c[1] for c in a_clips}
        check("A 保留下来的两段时长不变（Idle 1.00 / Walk 1.00）",
              abs(durs.get("Idle", -1) - 1.0) < 1e-6
              and abs(durs.get("Walk", -1) - 1.0) < 1e-6,
              " ".join("%s=%.2fs" % kv for kv in sorted(durs.items())))
        # 轨道数不变：每段仍是 21 骨 × 3 通道 = 63
        check("A 每段的轨道数仍是 63（21 骨 × 3 通道）",
              all(c[2] == 63 for c in a_clips),
              ",".join(str(c[2]) for c in a_clips))
        # 几何一个字节没动：和仓库里那份 .vmesh 比 submeshes/vertices/indices
        probe = os.path.join(ASSETS, "models", "anime_girl.vmesh")
        _, pinfo = run([ASSETC, "--info", probe])
        mp = re.search(r"submeshes=(\d+) vertices=(\d+) indices=(\d+)", pinfo)
        mc = re.search(r"submeshes=(\d+) vertices=(\d+) indices=(\d+)",
                       a_out)
        check("A 几何账目与仓库 .vmesh 完全一致（只少了一段动画）",
              bool(mp and mc) and mp.groups() == mc.groups(),
              (mp.group(0) if mp else "-") + " vs " +
              (mc.group(0) if mc else "-"))
        # 仓库里那份也必须已经是 2 段（防止"只改代码不重烤"）
        mi = RE_INFO.search(pinfo)
        check("D 仓库里的 anime_girl.vmesh 已重烤（clips=2）",
              bool(mi) and int(mi.group(2)) == 2,
              mi.group(0) if mi else "没有 joints= 行")
        check("D 仓库里的 anime_girl.vmesh clips 名字唯一",
              len({c[0] for c in clips_of(pinfo)}) == len(clips_of(pinfo)),
              ",".join(c[0] for c in clips_of(pinfo)))

    # ================= B. 正常资产：一条都不该丢 =================
    print("\n== B 反例：alicia（4 段名字本来就不同）==")
    b, b_clips, b_dup, _ = case_cook(
        "B", SRC_OK, COOK_OK, want_clips=4, want_dup=0)
    if b:
        check("B 四段名字就是 Idle/Jump/Run/Walk",
              {c[0] for c in b_clips} == {"Idle", "Jump", "Run", "Walk"},
              ",".join(sorted(c[0] for c in b_clips)))

    # ================= C. 运行时直读 .fbx =================
    print("\n== C 运行时 FbxLoader（只放 .fbx，强制走回退路径）==")
    root = os.path.join(WORK, "root")
    if os.path.isdir(root):
        shutil.rmtree(root, ignore_errors=True)
    os.makedirs(os.path.join(root, "scenes"))
    os.makedirs(os.path.join(root, "models"))
    shutil.copyfile(SRC_DUP, os.path.join(root, "models", "anime_girl.fbx"))
    scene = {"version": 1, "generator": "verify_clip_dedup",
             "entities": [{
                 "name": "M0",
                 "transform": {"position": [0, 0, 0], "rotation": [0, 0, 0],
                               "scale": [1, 1, 1]},
                 "parent": -1, "visible": True, "castShadow": True,
                 "mesh": {"kind": "fbx", "path": "models/anime_girl.fbx",
                          "modelKey": "clipdedup#0", "subMeshIndex": 0}}]}
    import json
    with open(os.path.join(root, "scenes", "probe.json"), "w",
              encoding="utf-8") as f:
        json.dump(scene, f, ensure_ascii=False, indent=2)

    code, out = run([EDITOR], {
        "MYVK_ASSET_DIR": root.replace("\\", "/"),
        "MYVK_EDITOR_SCENE": "scenes/probe.json",
        "MYVK_FRAMES": "40",
        "MYVK_NO_WINDOW_SAVE": "1",
    })
    check("C Editor 退出码 0", code == 0, "rc=%d" % code)
    c_dup = dups(out)
    check("C 运行时也打了 2 条 FBX-CLIPDUP（action=drop）",
          len(c_dup) == 2 and all(x[3] == "drop" for x in c_dup),
          "; ".join("%s/%s" % (x[0], x[1]) for x in c_dup))
    m = RE_RT_CLIPS.search(out)
    check("C 收尾行 FBX clips count=2", bool(m) and m.group(1) == "2",
          m.group(0) if m else "没有 FBX clips 行")
    check("C 收尾行的名字序列就是 [Idle,Walk]",
          bool(m) and m.group(2) == "Idle,Walk",
          m.group(2) if m else "-")
    check("C 走的是 FBX 直读（不是 .vmesh）", "FBX loaded:" in out,
          "FBX loaded=%d / VMESH loaded=%d" %
          (out.count("FBX loaded:"), out.count("VMESH loaded")))

    # ================= E. 洁净度 =================
    print("\n== E 日志洁净度 ==")
    bad = [ln for ln in out.splitlines() if RE_VUID.search(ln)]
    check("E 零 [ERROR] / 零 VUID", not bad, "共 %d 条" % len(bad))
    if bad:
        for ln in bad[:5]:
            print("      | " + ln)

    print("\n" + "-" * 56)
    ok = not fails
    print("结果: %s" % ("PASS" if ok else "FAIL: %d 项" % len(fails)))
    if fails:
        for f in fails:
            print("  - " + f)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
