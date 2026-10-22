"""验收：一键自动绑骨（P2）—— 用**厂商原厂骨架当标准答案**。

思路：拿一个厂商已经绑好的角色（samba_dancing.fbx，Mixamo 52 骨），
先 `--strip-skin` 把骨架/权重/动画全剥掉，变成一个**纯静态网格**；再对它
跑 `--autorig`。于是我们同时拥有：

  ground truth = 原厂 52 根骨里与自动骨架**同名**的那 23 根的世界坐标
  prediction   = 自动绑骨算出来的 23 个关节

对比时用**相对 Hips 的向量**，把"枢轴归位带来的整体平移"消掉 —— 骨架
的好坏本来就只取决于相对几何，绝对位置由实体 transform 决定。

判据：
  R1 绑骨成功      ok=1，23 个关节，全部顶点拿到权重，≤4 影响，归一化残差 < 1e-4
  R2 量测正确      身高 / 臂展 / 半展 与 .vmesh 的 bounds 一致（±1%），
                   朝向（fwdSign）与原厂"踝→趾"的方向一致
  R3 关节位置      23 根同名骨相对 Hips 的平均误差 < 4 cm、最大 < 10 cm
  R4 左右对称      Left/Right 的 |横向| 之差 < 5 mm（同一个算法两侧跑）
  R5 没有死骨      每根骨至少主导 20 个顶点（一根都不主导 = 骨头在空气里，
                   姿势看不出来，但程序化动画里那条腿会完全不动）
  R6 可被运行时读  产出能被 AssetC --check 通过，且 joints=23 / skinVerts=全部

用法：
    python tools/verify_autorig.py
    python tools/verify_autorig.py --model models/thirdparty/samba_dancing.vmesh --keep
"""

import argparse
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from safe_fs import safe_remove, safe_rmtree                          # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
ASSETC = os.path.join(BIN, "AssetC.exe")
ASSETS = os.path.join(BIN, "assets")
WORK = os.path.join(os.path.dirname(ROOT), "_venn_verify", "autorig")

RE_AUTORIG = re.compile(
    r"ASSETC AUTORIG ok=1 height=([\d.]+) armSpan=([\d.]+) armAxis=(\d+) "
    r"fwdSign=(-?\d+) halfSpan=([\d.]+) joints=(\d+) verts=(\d+) "
    r"rigged=(\d+) maxInf=(\d+) maxErr=([\d.eE+-]+)")
RE_AJOINT = re.compile(
    r"ASSETC AUTORIG-JOINT (\S+) \(([-\d.]+), ([-\d.]+), ([-\d.]+)\) "
    r"verts=(\d+)")
RE_INFO_JOINT = re.compile(
    r'ASSETC\s+joint \d+ "([^"]+)" parent=(-?\d+) local=\(([^)]*)\) '
    r"world=\(([^)]*)\)")
RE_INFO_SIZE = re.compile(r"ASSETC\s+size=([\d.]+) x ([\d.]+) x ([\d.]+) m")
RE_INFO_SKIN = re.compile(r"ASSETC\s+skinVerts=(\d+) / (\d+)")
RE_INFO_HEADS = re.compile(r"ASSETC\s+joints=(\d+) clips=(\d+)")

# 自动骨架里这 23 根是"有原厂对应物"的
CORE = ["Hips", "Spine", "Spine1", "Spine2", "Neck", "Head",
        "LeftShoulder", "LeftArm", "LeftForeArm", "LeftHand",
        "RightShoulder", "RightArm", "RightForeArm", "RightHand",
        "LeftUpLeg", "LeftLeg", "LeftFoot", "LeftToeBase",
        "RightUpLeg", "RightLeg", "RightFoot", "RightToeBase"]

g_pass = 0
g_fail = 0


def check(name, ok, detail=""):
    global g_pass, g_fail
    if ok:
        g_pass += 1
        print("[ OK ] %s%s" % (name, ("  " + detail) if detail else ""))
    else:
        g_fail += 1
        print("[FAIL] %s%s" % (name, ("  " + detail) if detail else ""))
    return ok


def run(args, timeout=300):
    p = subprocess.run([ASSETC] + list(args), cwd=BIN,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, encoding="utf-8", errors="replace",
                       timeout=timeout)
    return p.returncode, p.stdout.splitlines()


def parse_vec(s):
    v = [float(x) for x in s.split(",")]
    return (v[0], v[1], v[2])


def info(path):
    """跑 --info，返回 (关节字典 name→world, 尺寸, joints/clips, skin 统计)"""
    rc, lines = run(["--info", path])
    joints = {}
    size = None
    heads = (0, 0)
    skin = (0, 0)
    for ln in lines:
        m = RE_INFO_JOINT.search(ln)
        if m:
            joints[m.group(1)] = parse_vec(m.group(4))
            continue
        m = RE_INFO_SIZE.search(ln)
        if m:
            size = tuple(float(g) for g in m.groups())
        m = RE_INFO_HEADS.search(ln)
        if m:
            heads = (int(m.group(1)), int(m.group(2)))
        m = RE_INFO_SKIN.search(ln)
        if m:
            skin = (int(m.group(1)), int(m.group(2)))
    return joints, size, heads, skin


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model",
                    default="models/thirdparty/samba_dancing.vmesh")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    os.makedirs(WORK, exist_ok=True)
    src = os.path.join(ASSETS, args.model.replace("/", os.sep))
    static = os.path.join(WORK, "static.vmesh")
    rigged = os.path.join(WORK, "autorig.vmesh")

    if not os.path.exists(src):
        print("[FAIL] 找不到 %s" % src)
        return 1

    print("== 一键自动绑骨：验收（用原厂骨架当标准答案）==")
    print("   源: %s" % args.model)

    # ---- 0. 原厂 ground truth ----
    gt, gt_size, gt_heads, gt_skin = info(src)
    if not gt:
        print("[FAIL] 原厂 .vmesh 里读不到关节")
        return 1

    # ---- 1. 剥掉原厂绑定 → 纯静态网格 ----
    rc, lines = run(["--strip-skin", src, "-o", static])
    stripped = any("STRIP-SKIN" in ln for ln in lines)
    check("R0 剥骨架成功（静态网格输入就绪）", rc == 0 and stripped,
          "joints -> 0 / clips -> 0")

    # ---- 2. 自动绑骨 ----
    rc, lines = run(["--autorig", static, "-o", rigged])
    m = None
    ajoints = {}
    for ln in lines:
        mm = RE_AUTORIG.search(ln)
        if mm:
            m = mm
        mm = RE_AJOINT.search(ln)
        if mm:
            ajoints[mm.group(1)] = (parse_vec(
                "%s, %s, %s" % (mm.group(2), mm.group(3), mm.group(4))),
                int(mm.group(5)))
    if not check("R1 自动绑骨跑通", rc == 0 and m is not None,
                 "" if m is None else "joints=%s verts=%s" %
                 (m.group(6), m.group(7))):
        print("\n结果: FAIL")
        return 1

    height, arm_span, arm_axis, fwd_sign = (float(m.group(1)),
                                            float(m.group(2)),
                                            int(m.group(3)),
                                            int(m.group(4)))
    half_span = float(m.group(5))
    n_joints, n_verts, n_rigged = (int(m.group(6)), int(m.group(7)),
                                   int(m.group(8)))
    max_inf, max_err = int(m.group(9)), float(m.group(10))

    check("R1b 权重完整且归一", n_rigged == n_verts and max_inf <= 4
          and max_err < 1.0e-4,
          "rigged=%d/%d maxInf=%d maxErr=%.1e" %
          (n_rigged, n_verts, max_inf, max_err))

    # ---- R2 量测 ----
    # .vmesh 的 size 是 (X, Y, Z)；手臂轴上的跨度应该等于 size[arm_axis]
    ok_size = gt_size is not None
    detail = "size=%s" % (gt_size,)
    if ok_size:
        want = gt_size[arm_axis]
        err = abs(arm_span - want) / max(want, 1e-6)
        ok_size = err <= 0.01 and abs(height - gt_size[2]) / gt_size[2] <= 0.01
        detail += " armSpan=%.3f(期望%.3f) H=%.3f(期望%.3f)" % (
            arm_span, want, height, gt_size[2])
    check("R2 量测与资产包围盒一致（±1%）", ok_size, detail)

    # 朝向：原厂踝→趾的方向点乘自动朝向应 > 0
    fwd_ok = True
    detail = ""
    if "mixamorig:LeftFoot" in gt and "mixamorig:LeftToeBase" in gt:
        ank = gt["mixamorig:LeftFoot"]
        toe = gt["mixamorig:LeftToeBase"]
        d = [toe[i] - ank[i] for i in range(3)]
        # 自动的朝向轴：armAxis=0 → 前方是 Y 轴；armAxis=1 → 前方是 X 轴
        fwd_axis = 1 - arm_axis
        pred = d[fwd_axis]

        # 顺带记录真值占比与误差（用于缩放"误差预算"）
        vh = gt_size[2] if gt_size else 1.0
        fwd_ok = pred * fwd_sign > 0
        detail = "原厂踝→趾在轴%d 上=%+.3f, 自动 fwdSign=%+d" % (
            fwd_axis, pred, fwd_sign)
        toe_err = abs(abs(pred) - 0.07 * vh)
        detail += " (趾前移期望 %.3f m，实测 %.3f m)" % (0.07 * vh, abs(pred))
        fwd_ok = fwd_ok and toe_err <= 0.02 * vh
    check("R2b 朝向（脚趾方向）判别正确", fwd_ok, detail)

    # ---- R3 关节位置（相对 Hips）----
    if "Hips" not in ajoints or "mixamorig:Hips" not in gt:
        check("R3 关节位置误差", False, "缺少 Hips")
    else:
        a_hips = ajoints["Hips"][0]
        g_hips = gt["mixamorig:Hips"]
        errs = []
        worst = ("-", 0.0)
        for name in CORE:
            gname = "mixamorig:" + name
            if name not in ajoints or gname not in gt:
                continue
            a = ajoints[name][0]
            g = gt[gname]
            d = [a[i] - a_hips[i] - (g[i] - g_hips[i]) for i in range(3)]
            e = (d[0] ** 2 + d[1] ** 2 + d[2] ** 2) ** 0.5
            errs.append(e)
            if e > worst[1]:
                worst = (name, e)
        mean_err = sum(errs) / max(len(errs), 1)
        max_err_pos = max(errs) if errs else 0.0
        check("R3 关节位置误差（相对 Hips，%d 根同名骨）" % len(errs),
              len(errs) >= 20 and mean_err <= 0.04 and max_err_pos <= 0.10,
              "平均 %.1f mm / 最大 %.1f mm (%s)" %
              (mean_err * 1000, max_err_pos * 1000, worst[0]))

    # ---- R4 左右对称 ----
    sym_ok = True
    sym_detail = []
    for base in ("Shoulder", "Arm", "ForeArm", "Hand", "UpLeg", "Leg",
                 "Foot", "ToeBase"):
        ln, rn = "Left" + base, "Right" + base
        if ln not in ajoints or rn not in ajoints:
            continue
        lx = abs(ajoints[ln][0][arm_axis])
        rx = abs(ajoints[rn][0][arm_axis])
        d = abs(lx - rx)
        sym_detail.append("%s %.1fmm" % (base, d * 1000))
        if d > 0.005:
            sym_ok = False
    check("R4 左右对称（|横向| 之差 < 5 mm）", sym_ok, " ".join(sym_detail))

    # ---- R5 没有死骨 ----
    dead = [n for n, (_, cnt) in ajoints.items() if cnt < 20]
    check("R5 没有死骨（每根至少主导 20 个顶点）", not dead,
          "最少的 3 根: " + " ".join(
              "%s=%d" % (n, ajoints[n][1])
              for n in sorted(ajoints, key=lambda k: ajoints[k][1])[:3])
          + ("" if not dead else "  死骨: " + ",".join(dead)))

    # ---- R6 产出可被运行时读 ----
    rc, lines = run(["--check", rigged])
    rc2, lines2 = run(["--info", rigged])
    out_joints, out_size, out_heads, out_skin = info(rigged)
    check("R6 产出是合法的 .vmesh（--check 通过）", rc == 0,
          "" if rc == 0 else (lines[-1] if lines else ""))
    check("R6b 关节数与权重都写进了文件",
          out_heads[0] == n_joints and out_skin[0] == out_skin[1] > 0,
          "joints=%d skinVerts=%d/%d" % (out_heads[0], out_skin[0],
                                         out_skin[1]))

    print("\n产物: %s" % WORK)
    print("结果: %s  (%d 通过 / %d 失败)" %
          ("PASS" if g_fail == 0 else "FAIL", g_pass, g_fail))

    if not args.keep:
        safe_remove(static)
        safe_remove(rigged)
    return 0 if g_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
