"""G06~G10（M2 手感地基）的回归用例。

三条互相独立的面，缺一条就有洞：

  A. **无窗口自检**（`Game.exe --selftest <name>`）
     输入映射 / 角色控制器 / 接触事件 / 相机系统 / 玩家装配。这五项都是
     纯逻辑（no window / no Vulkan），毫秒级，且断言写在 C++ 里 ——
     脚本只负责"跑起来、数够条数、别红"。

  B. **场景读盘锚点**（`--scene runtime_test/player_scene.json`）
     A 组证明"算法对"，但证明不了"字段真的从 JSON 读进来了"。
     这里用四条日志钉住四个组件：
       · `GAME-PHYS ... triggers=1 materials=1`   → Trigger / PhysicsMaterial（G08）
       · `GAME-PLAYER attached 'Player'`          → CharacterController（G07）
       · `CAM-ACTIVE name=Player priority=10`     → 相机选择按优先级（G09）
       · `CAM-POSE ... fov=55.0`                  → fov 来自组件而不是默认值（G09）

  C. **读 → 写 → 再读 往返**（`--dump-scene`）
     `saveScene` 与 `loadScene` 是两套代码。只测读，"存档丢字段"永远
     暴露不出来。这里把刚读进来的场景写出去，逐字段比对 + 再读回来。

  D. **老场景基线不变**（`scenes/sandbox.json`）
     sandbox 里没有任何新组件 → 必须 `GAME-PLAYER none` 且**一条
     `CAM-ACTIVE` 都没有**（CameraSystem 在无相机组件时什么都不做）。
     这是"新特性默认关 = 改动前逐位一致"在运行时侧的落点。

  E. **编辑器 UI 面**（Inspect 分区真的画出来了）
     特性准入的五件套之一："界面点不到的不算交付"。断言用的是
     **分区体**锚点（INS-*-BODY）而不是 HEADER —— header 只要函数被调用
     就会打，只有 body 才能证明这一段真的展开着、控件真的建出来了
     （项目纪律：锚点记在提前返回之前 = 只能证明函数被调用过）。

为什么不用 `MYVK_ASSET_DIR` 指仓库：
    资产根是按优先级找的，指过去会连带换掉 shader / texture 的解析根。
    测试只该换**被测的东西** —— 所以这里把场景夹具抄进 build/bin 的
    assets/scenes（那本来就是运行时可写目录），其余一律走默认。

用法：
    python tools/verify_runtime_m2.py
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Game.exe")
BIN_DIR = os.path.dirname(DEFAULT_EXE)
EDITOR_EXE = os.path.join(BIN_DIR, "Editor.exe")
SCENE_REL = "scenes/runtime_test/player_scene.json"
FIXTURE_DIR = os.path.join(ROOT, "assets", "scenes", "runtime_test")

# 自检名 → 断言条数下限。
#
# ⚠ 用**下限**而不是精确值：新增断言不该让脚本变红；但"用例被删光了
#   仍然 pass=0 fail=0 → PASS"是必须堵住的洞（这类假绿比红更贵）。
SELFTESTS = {
    "input":     ("INPUT-TOTAL",     28),
    "character": ("CHAR-TOTAL",      18),
    "contact":   ("CONTACT-TOTAL",   11),
    "camera":    ("CAM-TOTAL",       13),
    "player":    ("PLAYER-TOTAL",     9),
}

PHYS_RE = re.compile(
    r"GAME-PHYS world built . (\d+) body\(ies\) "
    r"\(ground=(\d+) triggers=(\d+) materials=(\d+)\) "
    r"from (\d+) rigid entities")
ACTIVE_RE = re.compile(r"CAM-ACTIVE name=(\S+) priority=(-?\d+)")
POSE_RE = re.compile(r"CAM-POSE name=(\S+) pos=\(([-\d.]+),([-\d.]+),([-\d.]+)\) "
                     r"yaw=([-\d.]+) pitch=([-\d.]+) fov=([-\d.]+)")


# ------------------------------------------------------------------ 运行

def run_exe(exe, args, frames=None):
    """跑一次 Game.exe，返回 (rc, lines)。不开超时轮询 —— 这些都会自己退。"""
    cmd = [exe] + list(args)
    if frames is not None:
        cmd += ["--frames", str(frames)]
    env = dict(os.environ)
    # 隔离用户配置：别让开发机上残留的 %LOCALAPPDATA% 设置影响判定
    env.pop("MYVK_ASSET_DIR", None)
    try:
        p = subprocess.run(cmd, cwd=BIN_DIR, env=env, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True,
                           encoding="utf-8", errors="replace", timeout=180)
    except subprocess.TimeoutExpired:
        return -1, ["<timeout>"]
    return p.returncode, p.stdout.splitlines()


def stage_fixture():
    """把夹具场景抄进 build/bin/assets/scenes（运行时可写目录）。"""
    dst = os.path.join(BIN_DIR, "assets", "scenes", "runtime_test")
    os.makedirs(dst, exist_ok=True)
    for name in os.listdir(FIXTURE_DIR):
        if name.endswith(".json"):
            shutil.copy2(os.path.join(FIXTURE_DIR, name),
                         os.path.join(dst, name))
    return dst


def run_editor(exe, select, tab, frames=90):
    """起一次编辑器，选中 `select`、切到 `tab` 页签，抓矩形锚点日志。

    ⚠ 只改**被测的东西**：场景 + 选中项 + 页签。不注入布局/偏好路径，
    所以永远不会读写用户配置（自动化下默认路径不读、写一律拒绝）。
    """
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"          # 矩形锚点必须显式打开
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_EDITOR_NO_HUD"] = "1"
    env["MYVK_EDITOR_SCENE"] = os.path.join(ROOT, "assets", SCENE_REL
                                            .replace("/", os.sep))
    env["MYVK_EDITOR_SELECT"] = select
    env["MYVK_EDITOR_INSP_TAB"] = tab
    env["MYVK_FRAMES"] = str(frames)
    env.pop("MYVK_ASSET_DIR", None)
    try:
        p = subprocess.run([exe], cwd=BIN_DIR, env=env, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True,
                           encoding="utf-8", errors="replace", timeout=180)
    except subprocess.TimeoutExpired:
        return ["<timeout>"]
    return p.stdout.splitlines()


def rect_ok(lines, anchor):
    """锚点存在且矩形非退化（宽高都 > 0）。

    ⚠ 只判"出现过"是不够的：被裁到 0 宽度也算出现过，而那正是
    "分区被滚出可视区 / 被折叠"的形态。
    """
    pat = re.compile(re.escape(anchor) + r"=\((-?\d+),(-?\d+)\)-\((-?\d+),(-?\d+)\)")
    for line in lines:
        m = pat.search(line)
        if not m:
            continue
        x0, y0, x1, y1 = (int(m.group(i)) for i in range(1, 5))
        if x1 - x0 >= 8 and y1 - y0 >= 4:
            return True
    return False


# ------------------------------------------------------------------ 主流程

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    if not os.path.exists(exe):
        print("找不到可执行文件，请先构建：", exe)
        print("RESULT: FAIL: missing Game.exe")
        return 1
    if not os.path.isdir(FIXTURE_DIR):
        print("缺测试场景夹具：", FIXTURE_DIR)
        print("RESULT: FAIL: missing fixture")
        return 1

    fails = []

    def check(name, ok, detail=""):
        print("%s  %s%s" % ("PASS" if ok else "FAIL", name,
                            ("  (%s)" % detail) if (detail and not ok) else ""))
        if not ok:
            fails.append("%s | %s" % (name, detail))

    stage_fixture()
    tmpdir = tempfile.mkdtemp(prefix="venn_m2_")

    # ================= A. 五项无窗口自检 =================
    print("[A] 无窗口自检（G06 输入 / G07 角色 / G08 接触 / G09 相机 / G10 玩家）")
    for name, (total_tag, floor) in SELFTESTS.items():
        rc, lines = run_exe(exe, ["--selftest", name])
        m = None
        for line in lines:
            if total_tag in line:
                m = re.search(r"pass=(\d+) fail=(\d+)", line)
        check("  %-10s 退出码 0" % name, rc == 0, "rc=%s" % rc)
        check("  %-10s 有 %s 汇总行" % (name, total_tag), m is not None,
              "日志里没有汇总行")
        if not m:
            continue
        npass, nfail = int(m.group(1)), int(m.group(2))
        check("  %-10s fail=0" % name, nfail == 0, "fail=%d" % nfail)
        check("  %-10s pass >= %d（用例没被删空）" % (name, floor),
              npass >= floor, "pass=%d" % npass)

    # ================= B. 场景读盘锚点 =================
    print()
    print("[B] 场景读盘锚点（四个新组件真的从 JSON 读进来了）")
    rc, lines = run_exe(exe, ["--scene", SCENE_REL, "--start-state", "playing",
                              "--trace-states"], frames=40)
    check("  场景加载成功（entities / skipped）",
          any("GAME-SCENE path=" in l and "entities=6 skipped=0" in l
              for l in lines),
          "没有 'entities=6 skipped=0'")

    phy = None
    for line in lines:
        m = PHYS_RE.search(line)
        if m:
            phy = m
            break
    check("  G07+G08 物理世界（4 体 / 地面 1 / 触发 1 / 材质 1 / 刚体实体 2）",
          phy is not None and [int(phy.group(i)) for i in range(1, 6)]
          == [4, 1, 1, 1, 2],
          (phy.groups() if phy else "没有 GAME-PHYS world built 行"))
    check("  G07 角色控制器接管玩家实体",
          any("GAME-PLAYER attached 'Player'" in l for l in lines),
          "没有 'GAME-PLAYER attached'")

    act = None
    for line in lines:
        m = ACTIVE_RE.search(line)
        if m:
            act = m
            break
    check("  G09 按优先级选相机（Player 10 > FallbackCam 0）",
          act is not None and act.group(1) == "Player" and act.group(2) == "10",
          (act.groups() if act else "没有 CAM-ACTIVE 行"))

    fovs = set()
    for line in lines:
        m = POSE_RE.search(line)
        if m and m.group(1) == "Player":
            fovs.add(float(m.group(7)))
    check("  G09 fov 取自组件（55.0，不是默认 45.0）", fovs == {55.0},
          "见过的 fov=%s" % sorted(fovs))
    check("  日志无 ERROR / 校验层报错",
          not any("[ERROR" in l or "VUID" in l for l in lines),
          "出现错误行")

    # ================= C. 读 → 写 → 再读 =================
    print()
    print("[C] 序列化写路径：读 → 写 → 再读")
    dump = os.path.join(tmpdir, "roundtrip.json")
    rc, lines = run_exe(exe, ["--scene", SCENE_REL, "--dump-scene", dump],
                        frames=3)
    check("  --dump-scene 写出成功",
          any("GAME-SCENE dumped path=" in l for l in lines),
          "没有 'GAME-SCENE dumped'")

    src_path = os.path.join(BIN_DIR, "assets", SCENE_REL.replace("/", os.sep))
    got = err = None
    if os.path.exists(dump):
        try:
            with open(dump, encoding="utf-8") as f:
                got = json.load(f)
            with open(src_path, encoding="utf-8") as f:
                src = json.load(f)
        except Exception as e:      # noqa: BLE001
            err = str(e)
    check("  往返文件可解析", got is not None and err is None, str(err))

    if got is not None:
        by_name = {e["name"]: e for e in got.get("entities", [])}
        src_by = {e["name"]: e for e in src.get("entities", [])}
        check("  实体数一致", len(got.get("entities", []))
              == len(src.get("entities", [])),
              "dump=%d src=%d" % (len(by_name), len(src_by)))

        # 逐字段比对四个组件块。**逐字段**而不是"存在即可"：少写一个
        # 字段（比如忘了 sprintMultiplier）在"存在即可"下是绿的。
        def cmp_block(ent, key, label):
            a = src_by.get(ent, {}).get(key)
            b = by_name.get(ent, {}).get(key)
            if not isinstance(a, dict):
                check("  %s（源场景无此块，跳过）" % label, True)
                return
            if not isinstance(b, dict):
                check("  %s 往返后仍在" % label, False, "dump 里没有 %r" % key)
                return
            missing = [k for k in a if k not in b]
            diff = []
            for k in a:
                if k not in b:
                    continue
                va, vb = a[k], b[k]
                if isinstance(va, (int, float)) and isinstance(vb, (int, float)):
                    if abs(float(va) - float(vb)) > 1e-5:
                        diff.append("%s %r->%r" % (k, va, vb))
                elif isinstance(va, list) and isinstance(vb, list):
                    if len(va) != len(vb) or any(
                            abs(float(x) - float(y)) > 1e-5
                            for x, y in zip(va, vb)):
                        diff.append("%s %r->%r" % (k, va, vb))
                elif va != vb:
                    diff.append("%s %r->%r" % (k, va, vb))
            check("  %s 逐字段一致（%d 个字段）" % (label, len(a)),
                  not missing and not diff,
                  "缺=%s 变=%s" % (missing, diff))

        cmp_block("Player", "character", "G07 CharacterController")
        cmp_block("Player", "camera", "G09 Camera(跟随)")
        cmp_block("FallbackCam", "camera", "G09 Camera(固定机位)")
        cmp_block("TriggerZone", "trigger", "G08 Trigger")
        cmp_block("BouncyCrate", "physicsMaterial", "G08 PhysicsMaterial")
        cmp_block("BouncyCrate", "rigidbody", "G08 刚体（对照：老字段不能丢）")

        # 再读回来：写到 build/bin 里再用正常路径加载一次
        rel_out = "scenes/runtime_test/_roundtrip.json"
        dst_out = os.path.join(BIN_DIR, "assets", rel_out.replace("/", os.sep))
        try:
            shutil.copy2(dump, dst_out)
        except OSError as e:
            check("  往返文件放回资产目录", False, str(e))
        else:
            rc, lines2 = run_exe(exe, ["--scene", rel_out,
                                       "--start-state", "playing",
                                       "--trace-states"], frames=40)
            check("  往返文件能被重新加载（entities=6 skipped=0）",
                  any("entities=6 skipped=0" in l for l in lines2),
                  "没有 'entities=6 skipped=0'")
            check("  往返之后角色仍然接管",
                  any("GAME-PLAYER attached 'Player'" in l for l in lines2),
                  "没有 GAME-PLAYER attached")
            check("  往返之后触发体与材质仍在（triggers=1 materials=1）",
                  any("triggers=1 materials=1" in l for l in lines2),
                  "没有 'triggers=1 materials=1'")
            try:
                os.remove(dst_out)
            except OSError:
                pass

    # ================= D. 老场景基线不变 =================
    print()
    print("[D] 老场景（sandbox，无任何新组件）行为不变")
    if not os.path.exists(os.path.join(BIN_DIR, "assets", "scenes",
                                       "sandbox.json")):
        check("  sandbox.json 存在（跳过本组）", True)
    else:
        rc, lines = run_exe(exe, ["--scene", "scenes/sandbox.json",
                                  "--start-state", "playing",
                                  "--trace-states"], frames=40)
        check("  sandbox 加载成功",
              any("GAME-SCENE path=" in l and "skipped=0" in l for l in lines),
              "加载失败")
        check("  G07 没有角色控制器 → 不接管（GAME-PLAYER none）",
              any("GAME-PLAYER none" in l for l in lines),
              "打出了 attached")
        check("  G09 没有相机组件 → 一条 CAM-ACTIVE 都没有",
              not any("CAM-ACTIVE" in l for l in lines),
              "出现了 CAM-ACTIVE")
        check("  G08 没有触发体（triggers=0 materials=0）",
              any("triggers=0 materials=0" in l for l in lines),
              "没有 'triggers=0 materials=0'")

    # ================= E. 编辑器 UI 面 =================
    print()
    print("[E] 编辑器 UI 面（分区体锚点：真的展开了、控件真的建出来了）")
    if not os.path.exists(EDITOR_EXE):
        check("  Editor.exe 存在（跳过本组）", True)
    else:
        cases = [
            ("Player", "physics", "INS-PHYS-CHAR-BODY",
             "G07 Physics 页签：角色控制器段"),
            ("TriggerZone", "physics", "INS-PHYS-TRIGGER-BODY",
             "G08 Physics 页签：触发体段"),
            ("BouncyCrate", "physics", "INS-PHYS-MAT-BODY",
             "G08 Physics 页签：物理材质段"),
            ("Player", "world", "INS-CAM-COMP-BODY",
             "G09 World 页签：相机组件段"),
        ]
        for select, tab, anchor, label in cases:
            lines = run_editor(EDITOR_EXE, select, tab)
            check("  %s" % label, rect_ok(lines, anchor),
                  "锚点 %s 没出现或矩形退化" % anchor)
        # 相机列表（同一次 World 页签运行里就有了，单独断言更清楚）
        lines = run_editor(EDITOR_EXE, "Player", "world")
        check("  G09 相机列表（谁在赢）", rect_ok(lines, "INS-CAM-LIST"),
              "锚点 INS-CAM-LIST 没出现或矩形退化")

    # ================= 收尾 =================
    try:
        for f in os.listdir(tmpdir):
            os.remove(os.path.join(tmpdir, f))
        os.rmdir(tmpdir)
    except OSError:
        pass

    if fails:
        print()
        print("失败项：")
        for f in fails:
            print("  -", f)
        print("RESULT: FAIL: %d 项断言未通过" % len(fails))
        return 4
    print()
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
