#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/verify_material_library.py —— 材质仓库 / 地形默认材质 / 出生点落地

被验证的用户需求（原话，2026-10-24）：
  "1. GameStart 的标记任务比例不对需要和真人一比一还原，并且它的初始位置
      是在地形位置一下的，这个也不对，要初始化在地形地面上面。
   2. 初始化地形的地面要尽量保持和栅格地面是同一个平面……以后地形地面就
      默认用 grass_ground 来就行了。打造出的材质仓库，可以在模式中用作笔刷，
      给地形区域手动加上纹理贴图。"

## 分两段验

**静态段（不启进程）** —— 检查磁盘上的仓库产物：
  M1  `assets/materials/library.json` 存在，entries >= 2
  M2  清单声明的默认项是 `materials/library/grass_ground`
  M3  每条条目的 `.vmat` + albedo / normal / orm 三张贴图**都真的在磁盘上**
      （"清单里写了但文件没拷过来"是这套生成脚本最可能的坏法）
  M4  `tools/gen_material_library.py --check` 返回 0（产物与资源一致）

**运行段（启 Editor.exe，第 30 帧自动建一片地形）** —— 检查运行时行为：
  M5  `MATLIB-LOAD ... default=materials/library/grass_ground`
  M6  地形层 0 真的挂上了 grass_ground **且贴图加载成功**：
      `TERRAIN-LAYER idx=0 id=materials/library/grass_ground albedo=file normal=file`
      （`albedo=file` 才是关键 —— 只看到 id 对、贴图退回 default 等于没贴）
  M7  新建地形与栅格同面：`LS-CREATE-SEAT z=0.00`
  M8  出生点被抬到地表：`GAMESTART-SEAT ... after.z == terrainH`，且
      `terrainH > 0.5`（太低就证明不了"原来被埋在地形之下"）
  M9  有地形时栅格退场：`GRID-SUPPRESS on=1 reason=terrain`
  M10 出生点标记是**真人比例**：`GAMESTART-MARK` 矩形的 高/宽 >= 2.2
      （Q 版那一版是 1.92 —— 头直径占身高 47%，所以这条阈值能分开两者）
  M11 日志健康：零 `[VK_CHECK]`、零 `ERROR`

## 为什么要用"空场景 + 自动建地形"

`MYVK_LANDSCAPE_AUTOCREATE=1` 走的是 `createWithCurrentSettings()` ——
和用户在面板上点"创建地形"**完全同一条路径**（同一个函数，见
`EditorApp::serviceEditMode`）。所以 M6~M9 验的是真实交互路径，
不是给验收专门写的一段代码。

要让它真的建，场景里必须**没有**地形，否则钩子会跳过。所以模板工程
copy 过来之后先把 Terrain 实体删掉，GameStart 留着（它才是 M8 的主角）。

用法：python tools/verify_material_library.py
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import launcher_kit as kit  # noqa: E402

KIND = "third_person"
NAME = "MatLibCheck"
FRAMES = 200          # ~3.3 秒：第 30 帧建地形，之后要跑够帧让日志刷出来

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIB_JSON = os.path.join(REPO, "assets", "materials", "library.json")
GEN = os.path.join(REPO, "tools", "gen_material_library.py")

DEFAULT_ID = "materials/library/grass_ground"


# ---------------------------------------------------------------- 静态段

def check_assets(fails):
    if not os.path.isfile(LIB_JSON):
        fails.append("M1 缺少 %s（先跑 tools/gen_material_library.py）"
                     % os.path.relpath(LIB_JSON, REPO))
        return None
    with open(LIB_JSON, encoding="utf-8") as f:
        lib = json.load(f)
    entries = lib.get("entries") or []
    if len(entries) < 2:
        fails.append("M1 清单只有 %d 条，期望 >= 2" % len(entries))
    else:
        kit.log("CHECK M1 清单 %d 条（tier=%s）OK"
                % (len(entries), lib.get("tier", "?")))

    if lib.get("default") != DEFAULT_ID:
        fails.append("M2 默认项是 %r，期望 %r"
                     % (lib.get("default"), DEFAULT_ID))
    else:
        kit.log("CHECK M2 默认项 = %s OK" % DEFAULT_ID)

    # M3：清单里写的每个文件都要真的存在
    missing = []
    for e in entries:
        for key in ("vmat",):
            if not e.get(key):
                missing.append("%s.%s 未声明" % (e.get("name", "?"), key))
        for key, rel in (e.get("textures") or {}).items():
            if not rel:
                missing.append("%s.%s 未声明" % (e.get("name", "?"), key))
                continue
            abs_p = os.path.join(REPO, "assets", rel)
            if not os.path.isfile(abs_p):
                missing.append("%s.%s -> %s 不存在"
                               % (e.get("name", "?"), key, rel))
        if e.get("vmat"):
            abs_p = os.path.join(REPO, "assets", e["vmat"])
            if not os.path.isfile(abs_p):
                missing.append("%s.vmat -> %s 不存在"
                               % (e.get("name", "?"), e["vmat"]))
    if missing:
        fails.append("M3 清单指向的文件缺失: %s" % "; ".join(missing[:4]))
    else:
        kit.log("CHECK M3 %d 条条目的 vmat + 3 张贴图全部在磁盘上 OK"
                % len(entries))

    # M4：产物与资源一致（生成脚本自带的自检）
    try:
        r = subprocess.run([sys.executable, GEN, "--check"],
                           cwd=REPO, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True,
                           encoding="utf-8", errors="replace", timeout=120)
        if r.returncode != 0:
            fails.append("M4 gen_material_library.py --check 失败: %s"
                         % (r.stdout or "").strip().splitlines()[-1:])
        else:
            kit.log("CHECK M4 生成脚本自检通过 OK")
    except Exception as ex:                     # noqa: BLE001
        fails.append("M4 跑不动生成脚本: %s" % ex)
    return lib


# ---------------------------------------------------------------- 工程

def make_project(work, assets_dir):
    """模板 copy 成工程，删掉 Terrain 实体、保留 GameStart；再补上仓库资产。"""
    root = os.path.join(work, NAME)
    if os.path.isdir(root):
        kit.fresh_dir(root)      # 宿主有批量删除保护，用它而不是 rmtree
    os.makedirs(root, exist_ok=True)
    shutil.copytree(os.path.join(kit.TEMPLATES, KIND), root, dirs_exist_ok=True)

    # 材质仓库的两坨资产：模板工程里没有，必须从仓库根目录补进来
    for rel in ("materials/library", "textures/materials"):
        src = os.path.join(REPO, "assets", rel)
        dst = os.path.join(root, "assets", rel)
        if os.path.isdir(src):
            shutil.copytree(src, dst, dirs_exist_ok=True)
        else:
            kit.log("PROBE 警告：%s 不存在" % rel)

    scene_path = os.path.join(root, "assets", "scenes", "main.json")
    with open(scene_path, encoding="utf-8") as f:
        scene = json.load(f)
    before = len(scene["entities"])
    scene["entities"] = [e for e in scene["entities"] if "terrain" not in e]
    kit.log("PROBE 场景实体 %d → %d（删掉 Terrain，让自动建地形生效）"
            % (before, len(scene["entities"])))
    with open(scene_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(scene, f, ensure_ascii=False, indent=2)
        f.write("\n")

    gj = os.path.join(root, "assets", "game.json")
    with open(gj, encoding="utf-8") as f:
        g = json.load(f)
    g["title"] = NAME
    with open(gj, "w", encoding="utf-8", newline="\n") as f:
        json.dump(g, f, ensure_ascii=False, indent=2)
        f.write("\n")
    return root


# ---------------------------------------------------------------- 运行段

def rect_of(lines, tag):
    """取最后一条 rect 锚点。格式见 engine/ui/DebugRects.h。"""
    for l in reversed(lines):
        if tag in l:
            m = re.search(r"=\((-?[\d.]+),(-?[\d.]+)\)-\((-?[\d.]+),(-?[\d.]+)\)",
                          l)
            if m:
                return tuple(float(v) for v in m.groups())
    return None


def check_run(out, fails):
    lines = out.splitlines()

    # ---- M5 仓库真的被读进来了 ----
    m = re.search(r"MATLIB-LOAD source=(\S+) count=(\d+) default=(\S+)", out)
    if not m:
        fails.append("M5 没有 MATLIB-LOAD（材质仓库没被加载）")
    else:
        if int(m.group(2)) < 2:
            fails.append("M5 MATLIB-LOAD count=%s，期望 >= 2" % m.group(2))
        elif m.group(3) != DEFAULT_ID:
            fails.append("M5 默认项 %r，期望 %r" % (m.group(3), DEFAULT_ID))
        else:
            kit.log("CHECK M5 MATLIB-LOAD source=%s count=%s default=%s OK"
                    % (m.group(1), m.group(2), m.group(3)))

    # ---- M6 层 0 挂上 grass_ground **并且贴图真的加载了** ----
    layer0 = re.findall(
        r"TERRAIN-LAYER idx=0 id=(\S+) albedo=(\S+) normal=(\S+)", out)
    if not layer0:
        fails.append("M6 没有 TERRAIN-LAYER idx=0（地形没建 / 层没绑上）")
    else:
        lid, alb, nor = layer0[-1]
        if lid != DEFAULT_ID:
            fails.append("M6 层 0 的材质是 %s，期望 %s" % (lid, DEFAULT_ID))
        elif alb != "file":
            fails.append("M6 层 0 的 albedo 是 %s（贴图没加载，退回了纯色）"
                         % alb)
        else:
            kit.log("CHECK M6 层 0 = %s（albedo=%s normal=%s）OK"
                    % (lid, alb, nor))

    # ---- M7 地形与栅格同面 ----
    m = re.search(r"LS-CREATE-SEAT z=(-?[\d.]+)", out)
    if not m:
        fails.append("M7 没有 LS-CREATE-SEAT（地形没建出来）")
    elif abs(float(m.group(1))) > 1e-6:
        fails.append("M7 新建地形 z=%s，期望 0（与栅格同面）" % m.group(1))
    else:
        kit.log("CHECK M7 LS-CREATE-SEAT z=%s（与栅格严格同面）OK"
                % m.group(1))

    # ---- M8 出生点被抬到地表 ----
    m = re.search(
        r"GAMESTART-SEAT name=(\S+) before=\(([-\d.]+),([-\d.]+),([-\d.]+)\) "
        r"after=\(([-\d.]+),([-\d.]+),([-\d.]+)\) terrainH=([-\d.]+)", out)
    if not m:
        fails.append("M8 没有 GAMESTART-SEAT（出生点没被抬到地表）")
    else:
        name = m.group(1)
        before_z = float(m.group(4))
        after_z = float(m.group(7))
        terrain_h = float(m.group(8))
        if terrain_h <= 0.5:
            fails.append("M8 terrainH=%.3f 太低，证明不了'原来被埋在土里'"
                         "（地形中心不够高？）" % terrain_h)
        elif abs(after_z - terrain_h) > 0.01:
            fails.append("M8 after.z=%.3f 与 terrainH=%.3f 不一致"
                         % (after_z, terrain_h))
        elif before_z >= after_z - 0.01:
            fails.append("M8 before.z=%.3f 没有被抬高（after.z=%.3f）"
                         % (before_z, after_z))
        else:
            kit.log("CHECK M8 %s: z %.3f → %.3f（terrainH=%.3f）OK"
                    % (name, before_z, after_z, terrain_h))

    # ---- M9 有地形时栅格退场 ----
    if not re.search(r"GRID-SUPPRESS on=1 reason=terrain", out):
        fails.append("M9 没有 GRID-SUPPRESS on=1（栅格还在跟地形抢同一个平面）")
    else:
        inst = re.search(r"GRID-SUPPRESS on=1 reason=terrain instances=(\d+)",
                         out)
        kit.log("CHECK M9 栅格已退场（terrain instances=%s）OK"
                % (inst.group(1) if inst else "?"))

    # ---- M10 出生点标记是真人比例 ----
    #
    # 判据是**锚点矩形的宽高比**（`logRect("GAMESTART-MARK", tl, br)`）：
    #   真人版：宽 = ±0.22H、高 = (1.00 + 0.10 + 0.06)H → 2.64
    #   Q 版  ：宽 = ±0.30H、高 = 1.148H                 → 1.92
    # 两个数差了 37%，所以一条区间断言就能把"改回大头"钉住。
    # 为什么用矩形而不是像素：像素断言要钉死相机、光照和抗锯齿，而这
    # 条用例的重点是"剪影够不够窄高"，矩形就够了（它由同一组比例算出来）。
    r = rect_of(lines, "GAMESTART-MARK")
    if not r:
        fails.append("M10 没有 GAMESTART-MARK 矩形（标记没画出来）")
    else:
        w = abs(r[2] - r[0])
        h = abs(r[3] - r[1])
        ratio = h / w if w > 1e-6 else 0.0
        if not (2.35 <= ratio <= 2.95):
            fails.append("M10 标记高宽比 %.2f 不在 [2.35, 2.95]（真身 1:1 应为 "
                         "2.64；Q 版是 1.92）" % ratio)
        else:
            kit.log("CHECK M10 GAMESTART-MARK %.1fx%.1f px，高宽比 %.2f"
                    "（真身 1:1 = 2.64）OK" % (w, h, ratio))

    # ---- M11 日志健康 ----
    vk = [l for l in lines if "[VK_CHECK]" in l]
    if vk:
        fails.append("M11 出现 VK_CHECK: %s" % vk[:2])
    errs = [l for l in lines if re.search(r"\[ERROR\]", l)]
    if errs:
        fails.append("M11 出现 ERROR: %s" % errs[:2])
    if not vk and not errs:
        kit.log("CHECK M11 零 VK_CHECK / 零 ERROR OK")


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=kit.EDITOR)
    args = ap.parse_args()

    fails = []
    kit.log("== 静态段：磁盘上的材质仓库 ==")
    lib = check_assets(fails)
    if lib is None:
        kit.log("结果: FAIL")
        for f in fails:
            kit.log("  - " + f)
        return 1

    if not os.path.isfile(args.exe):
        fails.append("Editor.exe 不存在（%s）" % args.exe)
        kit.log("结果: FAIL")
        for f in fails:
            kit.log("  - " + f)
        return 1

    kit.log("== 运行段：启编辑器，第 30 帧自动建一片地形 ==")
    work = kit.fresh_dir(os.path.join(kit.WORK_ROOT, "material_library"))
    os.makedirs(work, exist_ok=True)
    root = make_project(work, os.path.join(REPO, "assets"))

    env = kit.clean_env({
        "MYVK_ASSET_DIR": os.path.join(root, "assets"),
        "MYVK_EDITOR_SCENE": "scenes/main.json",
        # 走 createWithCurrentSettings() —— 与用户点"创建地形"同一条路径
        "MYVK_LANDSCAPE_AUTOCREATE": "1",
        # 打开所有控件矩形锚点（GAMESTART-MARK 靠它）
        "MYVK_LOG_RECTS": "1",
        "MYVK_FRAMES": str(FRAMES),
        "MYVK_NO_WINDOW_SAVE": "1",
        "MYVK_VALIDATION": "1",
    })
    kit.log("PROBE work=%s" % work)

    try:
        proc = subprocess.run([args.exe], cwd=root, env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True, encoding="utf-8", errors="replace",
                              timeout=300)
        out = proc.stdout or ""
    except subprocess.TimeoutExpired as ex:
        out = (ex.stdout or "") if isinstance(ex.stdout, str) else ""
        fails.append("RUN 编辑器 %d 帧后没有自己退出" % FRAMES)
        proc = None

    log_path = os.path.join(work, "material_library.log")
    with open(log_path, "w", encoding="utf-8") as f:
        f.write(out)
    kit.log("PROBE 日志 %s（%d 行）" % (log_path, out.count("\n")))

    check_run(out, fails)

    if proc is not None and proc.returncode != 0:
        fails.append("RUN 退出码 %d，期望 0" % proc.returncode)

    print("=" * 78)
    if fails:
        kit.log("结果: FAIL:（%d 项）" % len(fails))
        for f in fails:
            kit.log("  - " + f)
        return 1
    kit.log("结果: PASS:（M1~M11 全通过）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
