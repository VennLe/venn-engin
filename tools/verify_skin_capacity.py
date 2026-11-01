"""端到端验证：蒙皮关节 SSBO 的**扩容策略**（P3，2026-10-31）。

==============================================================================
它验的东西 / 为什么值得单开一条
==============================================================================
`updateSkinning` 里那块 SSBO 是按场景里所有蒙皮角色**累加**着用的。它的
容量原来是这样长的：

    if (offset + n > capacity)  { newCap = max(cap*2, offset+n+64); 重建 }

也就是**边填边扩**：填到第 N 个角色才发现不够，于是**当帧**重建 buffer +
重写描述符。那一帧是一次卡顿尖刺（Vulkan 分配 + descriptor write + 驱动
重新绑定）。症状是"进场景 / 又多一个角色时卡一下" —— 而**单角色测试永远
量不到**这个尖刺，因为它一次都不会触发扩容。

改成"先量总需求再扩一次"之后，判据就是那条锚点的**条数**：

    ANIM-SKIN-CAP grow from=256 to=<新容量> need=<总量>

  A1 多角色场景里它**恰好出现 1 次**（不是每个角色一次）。
     ⚠ 这条是核心：出现 N 次 = 又退回边填边扩，而画面完全正常。
  A2 `from=256` —— 初值没被偷偷改过（改了这条用例就失去意义）。
  A3 `need` == Σ 每具骨架的骨数（从 `ANIM-SKIN ... joints=` 那几行加出来）。
     ⚠ 只验"扩了"不够：`need` 算漏一类实体时扩容照样发生，然后靠
       `ANIM-SKIN-OVERFLOW` 兜底跳过角色 —— 画面上少一个人，日志里只有 WARN。

  B  分配正确性：每个角色的 `jointBase` **互不相同、且区间不重叠**。
     ⚠ 这是"扩容之后 offset 还对"的唯一判据。重叠的症状是两个角色显示
       同一个姿势，而这在单角色场景里**不可能被发现**。

  C  没有 `ANIM-SKIN-OVERFLOW`（扩容算对了就不该有人被跳过）。

==============================================================================
现场怎么造
==============================================================================
`verify_anim_layers.bake_template()` 烤一份模板（那一套"必须先有子网格
实体才有像素"的坑见那边文件头），然后把角色子树**深拷贝 N 份**。

⚠ 拷贝时必须一起改 `parent` 下标：模板里父子关系写的是**数组下标**
  （`"parent": 3`），只复制实体不改下标 = 所有拷贝都挂在原来那个根下面，
  骨数是一个角色的 N 倍、`jointBase` 却只用第一份 —— 扩容/分配两条判据
  会以完全不同的方式红。
⚠ 拷贝时**删掉 `guid`**：留着就是重复 guid → `SCENE-GUID-DUP` + 加载时
  被改成随机值，而这条用例并不关心 guid。

用法：
    python tools/verify_skin_capacity.py
"""

import argparse
import copy
import json
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from safe_fs import safe_remove, safe_rmtree               # noqa: E402  （与下方 verify_* 导入同一批）
from verify_anim_layers import bake_template, _base_env              # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors               # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
GAME_EXE = os.path.join(BIN, "Game.exe")
EDITOR_EXE = os.path.join(BIN, "Editor.exe")
OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify")

# 与 verify_anim_layers 用**不同**的模板路径：谁后跑谁覆盖，验的是哪个模型
# 就取决于执行顺序了（那条经验写在 bake_template 的注释里）。
TPL = os.path.join(OUT_DIR, "skin_capacity_tpl.json")
SCENE = os.path.join(OUT_DIR, "skin_capacity_scene.json")
CAM = "0,0,6,0,0,0"

# 1 个角色 ≈ 21 骨，初始容量 256 → 4 个角色正好塞得下、**不会扩容**。
# 要触发扩容就得超过 256：这里给 16 个（≈336），跨过初值一档。
N_CHARS = 16
FRAMES = 60

CAP_RE = re.compile(r"ANIM-SKIN-CAP grow from=(\d+) to=(\d+) need=(\d+)")
SKIN_RE = re.compile(r"ANIM-SKIN entity=(\d+) jointBase=(\d+) time=([\d.-]+) "
                     r"clip=\[([^\]]*)\] play=(\d) joints=(\d+)")   # ⚠ `\d+`：65 根骨

results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print("[%s] %s  %s" % ("PASS" if ok else "FAIL", name, detail))
    return ok


# ---------------------------------------------------------------- 造现场

def subtree_of(scene, root):
    """root 及其所有后代的下标（按 parent 链）。"""
    ents = scene["entities"]
    keep = {root}
    changed = True
    while changed:
        changed = False
        for i, e in enumerate(ents):
            if i in keep:
                continue
            p = e.get("parent", -1)
            if p in keep:
                keep.add(i)
                changed = True
    return sorted(keep)


def clone_once(scene, idxs, ordinal):
    """把 idxs 这一组实体整体复制一份追加到末尾，返回新的根下标。

    ⚠ 只复制**这一棵子树**，`parent` 从旧下标映射到新下标；子树根（它的
    parent 不在本次集合里）改 -1。
    ⚠ 根的判定不能写 `idxs[0]`：`subtree_of` 返回的是**排序后的下标**，
      根不一定排在最前（模板里角色根前面还有地面 / 灯光）。写死 `[0]`
      会把某棵子树挂回原来的根下面 —— 骨数变成一个角色的 N 倍。
    """
    ents = scene["entities"]
    keep = set(idxs)
    base = len(ents)
    remap = {old: base + k for k, old in enumerate(idxs)}
    for old in idxs:
        e = copy.deepcopy(ents[old])
        e.pop("guid", None)          # 留着 = 重复 guid（见文件头）
        e.pop("_guidWhy", None)
        p = e.get("parent", -1)
        e["parent"] = -1 if p not in keep else remap[p]
        # 平移一点，避免 16 个角色叠在同一个位置（叠着不影响判据，但截图里
        # 看不出"确实有 16 个"，排查时误导）。
        if "transform" in e and "position" in e["transform"]:
            e["transform"]["position"][0] += 1.5 * ordinal
        ents.append(e)
    return base


def build_scene():
    with open(TPL, encoding="utf-8") as f:
        tpl = json.load(f)
    scene = {"version": tpl.get("version", 2),
             "generator": "verify_skin_capacity",
             "camera": tpl.get("camera", {"target": [0, 0, 1], "yaw": 0.6,
                                          "pitch": 0.3, "distance": 12}),
             "entities": copy.deepcopy(tpl["entities"])}
    # 找**带 skinned 的那个根**（不按名字钉：演示角色换过好几次资产名）
    root = -1
    for i, e in enumerate(scene["entities"]):
        if e.get("skinned"):
            root = i
            break
    if root < 0:
        raise SystemExit("模板里没有带 `skinned` 的实体 —— 烤模板那条路坏了？")
    idxs = subtree_of(scene, root)
    for k in range(1, N_CHARS):
        clone_once(scene, idxs, k)
    os.makedirs(os.path.dirname(SCENE), exist_ok=True)
    with open(SCENE, "w", encoding="utf-8") as f:
        json.dump(scene, f, ensure_ascii=False, indent=1)
    return len(scene["entities"]), len(idxs)


# ---------------------------------------------------------------- 跑

def run_game(scene_rel, frames, wait=180.0):
    env = _base_env()
    env["MYVK_LOG_DT"] = ""
    argv = [GAME_EXE, "--scene", scene_rel, "--frames", str(frames)]
    proc = subprocess.Popen(argv, cwd=BIN, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    try:
        out, _ = proc.communicate(timeout=wait)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, _ = proc.communicate(timeout=30)
    return (out or b"").decode("utf-8", "replace").replace("\x00", "")


def main():
    # ⚠ `global` 必须在**本作用域第一次用到**这个名字之前（`--exe` 的默认值
    #   就是它），写在后面是 `SyntaxError`，而这条用例第一次跑不起来时
    #   看起来像"脚本没了"。
    global GAME_EXE
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=GAME_EXE)
    ap.add_argument("--editor", default=EDITOR_EXE)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    GAME_EXE = os.path.abspath(args.exe)
    if not os.path.isfile(GAME_EXE):
        check("0 找不到 Game.exe", False, GAME_EXE)
        return 1
    os.makedirs(OUT_DIR, exist_ok=True)

    print("== 蒙皮 SSBO 扩容策略（P3）==")
    kill_stale_editors()
    bake_template(os.path.abspath(args.editor), tpl=TPL, cam=CAM)
    n_ent, per_char = build_scene()
    print("场景 %d 个实体（%d 角色 × %d 个实体）" % (n_ent, N_CHARS, per_char))

    text = run_game(os.path.relpath(SCENE, BIN), FRAMES)
    dump = os.environ.get("SKIN_CAP_DUMP")
    if dump:
        with open(dump, "w", encoding="utf-8") as f:
            f.write(text)
    lines = text.splitlines()

    caps = [m for m in (CAP_RE.search(l) for l in lines) if m]
    skins = [m for m in (SKIN_RE.search(l) for l in lines) if m]

    # A1 扩容"每个帧资源槽一次"，不是"每个角色一次"
    #
    # ⚠⚠ 这里**不是** `== 1`：骨骼 SSBO 是**逐帧资源槽**各一份（`m_frames`，
    #   双缓冲 → 2 份），每份都有自己的 `jointCapacity`。两个槽各自第一次
    #   画这一帧时都会长一次，所以正常就是 2 条。第一版判据写成 `== 1`，
    #   于是每条都是假红（本机 2026-10-31 实测 caps=2）。
    #   要区分的是另一件事：**"边填边扩"会是 16 条（每角色一次）× 每槽**。
    #   所以判两条：条数很少，且每一对 (from,to,need) **完全相同** ——
    #   后者才证明"总量是一次算对的"，而不是边走边加。
    same = len(set(c.groups() for c in caps)) <= 1
    check("A1 ANIM-SKIN-CAP 每槽只长一次（不是每个角色长一次）",
          bool(caps) and len(caps) <= 4 and same,
          "caps=%s（每角色一次会是 %d 条）"
          % ([c.groups() for c in caps][:4], N_CHARS))

    # A2 初值没被改过
    check("A2 扩容前容量就是初值 256（初值被改过这条用例就没意义了）",
          bool(caps) and caps[0].group(1) == "256",
          "from=%s" % (caps[0].group(1) if caps else None))

    # A3 need == Σ joints（从 ANIM-SKIN 那几行加出来）
    joints = {}
    for m in skins:
        joints[m.group(1)] = int(m.group(6))
    total = sum(joints.values())
    check("A3 扩容算的 need == 所有角色骨数之和（没漏算一类实体）",
          bool(caps) and int(caps[0].group(3)) == total and total > 256,
          "need=%s total=%d chars=%d"
          % (caps[0].group(3) if caps else None, total, len(joints)))

    # B jointBase 互不重叠
    bases = sorted(int(m.group(2)) for m in skins)
    spans = []
    for m in skins:
        b = int(m.group(2))
        spans.append((b, b + int(m.group(6))))
    spans.sort()
    overlap = [(a, b) for (a, b), (c, d) in zip(spans, spans[1:]) if b > c]
    check("B 每个角色的 jointBase 互不重叠（扩容后 offset 还对）",
          len(spans) == N_CHARS and not overlap,
          "spans=%d overlap=%s bases=%s" % (len(spans), overlap[:3], bases[:6]))

    # C 没有溢出兜底
    check("C 没有 ANIM-SKIN-OVERFLOW（不该有人被跳过）",
          "ANIM-SKIN-OVERFLOW" not in text,
          "（有的话 = need 算漏了，画面上少一个人）")

    if not args.keep:
        for p in (SCENE,):
            try:
                safe_remove(p)
            except OSError:
                pass

    n_pass = sum(1 for _, ok, _ in results if ok)
    print("\n%d/%d PASS" % (n_pass, len(results)))
    if n_pass == len(results):
        print("结果: PASS")
        return 0
    print("结果: FAIL: %d 项" % (len(results) - n_pass))
    return 1


if __name__ == "__main__":
    sys.exit(main())
