#!/usr/bin/env python3
# ============================================================
# verify_ik —— 两骨 IK 的像素级验收（第八刀）
#
# ---------------------------- 为什么数值自检还不够 ----------------------------
# `AnimationSelfTest` 已经把求解器本身验穿了（100 项，含余弦定理定值、
# 极向量跟随动画、够不着不出 NaN）。但它验的是"输入 pose → 输出 pose"，
# 中间还隔着三层，**任何一层断了画面都不会对**：
#
#   ① 组件有没有被 `AnimatorSystem` 看见（`w.get<TwoBoneIKComponent>`）
#   ② 求解器写回去的**局部旋转**有没有真的进 `poseToJoints`（顺序 / 覆盖）
#   ③ 关节矩阵有没有真的传到 GPU（jointBase / SSBO 槽位）
#
# 所以这里走**真实入口**：真实资产 + 真实导入 + 真实命令 + 截图。
#
# ---------------------------- 四次启动 ----------------------------
# 只有 console 命令不同：
#
#   base   不发命令                      ← 基线
#   off    ik.set ...; ik.weight 0       ← 组件在、权重 0
#   on     ik.set 0 0.15 0.25            ← 可达目标（实测 actual=0.0000）
#   far    ik.set 0 0.45 0.10            ← 够不着（实测 actual=0.1245）
#
# 目标点全部在 **glTF 场景空间**（Y-up），alicia 身高 1.652 m、脚在 y=0。
#
# ---------------------------- 判据 ----------------------------
#   K0/K0b/K0c  截到图 / 视口矩形非空 / 视口里**真有内容**（≥5% 非背景）
#               K0c 不能省：空场景会让 K1 平凡通过，上一刀就栽在这上面
#   K1   base == off **逐像素**   → weight=0 是"完全不动 pose"，不是"接近"
#   K2   on != base，差异 ≥ 0.02% → 求解结果真的到了顶点
#   K2b  on 的差异包围盒 ≤ 45%    → 动的是**一条腿**，不是整个人飞了
#
# ⚠⚠ 单位陷阱（本轮踩过，写在这里免得下一刀再踩）：
#   `mg.diff_stats()` 返回的三个数**都是占比 0..1，不是百分数**
#   （见 verify_morph_gpu.py 的 `return n / float(total), box / float(total),
#   content / float(total)`）。第一版我按百分数写阈值，后果是：
#     · K0c `content >= 1.5`   → 实际 0.66 被判成 "0.66%" 而红（真值 66%）
#     · K2b `bbox <= 45.0`     → 实测 0.13，0.13 <= 45 恒真，**等于没测**
#   恒真的判据比红判据更危险：它让你以为覆盖了，其实什么都没守住。
#   正确的阈值全部对齐 verify_morph_gpu.py：`>= 0.05` / `>= 0.0002` / `<= 0.45`。
#   K3   on 的 `ANIM-IK actual` < 0.005 且 miss=0  → 脚真的落在目标上
#   K4   off 的日志里**没有** ANIM-IK → solver 提前 return，系统认为没解算
#   K5   far 的 actual > 0.05 且 miss=1 → 够不着时**如实报**，不假装成功
#   K6   far != base 且无崩溃       → 伸直也是真的动了，且没出 NaN
#   K7   IK-SET resolved=1 且链是 LeftUpLeg>LeftLeg>LeftFoot
#        → 自动解析在**真实资产**上成立（不是只在自检的假骨架上）
#
# 用法：python tools/verify_ik.py
# ============================================================

import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import verify_morph_gpu as mg                                   # noqa: E402

ROOT = mg.ROOT
BIN = mg.BIN
OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify", "ik")
SCENE = "_ik_cam.json"

# 全身偏下的机位：腿要够大。yaw=-90° 把相机放到 -Y 侧朝 +Y 看（那一侧是
# 正面，与 verify_morph_gpu 的结论一致）。
#
# ⚠ 机位是判据的一部分，不是随手填的：
#   · 3.4 m 全身（第一版）：腿只有 ~15 px 宽，66° 屈膝只改 **83** 个像素
#     （0.0099%），刚好卡在噪声与阈值的边界上 —— 那种"能过但其实很脆弱"
#     的判据比红更糟。
#   · 1.9 m + 目标抬到膝盖高度：腿宽 ~28 px，同样的屈膝改几百像素，
#     信号/阈值比从 1.2× 提到 10× 量级。
CAM = {"target": [0.0, 0.0, 0.70], "yaw": -1.5708, "pitch": 0.02,
       "distance": 1.9, "fov": 50.0}

TARGET_OK = "0 0.15 0.25"     # 可达：实测 target=0.6560 actual=0.0000 bend=66.5°
TARGET_FAR = "0 0.45 0.10"    # 够不着：实测 target=0.9072 actual=0.1245

RE_IK_SET = re.compile(r"IK-SET ok=1 entity=(\d+) target=\S+ resolved=(\d+) "
                       r"chain=\[([^\]]*)\]")
RE_ANIM_IK = re.compile(r"ANIM-IK n=(\d+) miss=(\d+) bone=(\S+) target=([\d.]+) "
                        r"actual=([\d.]+) bend=([\d.]+)deg")

_n = 0
_bad = 0


def check(name, ok, detail=""):
    global _n, _bad
    _n += 1
    if not ok:
        _bad += 1
    print("CHECK %s %s%s" % (name, "OK" if ok else "FAIL",
                             (" (" + detail + ")") if detail else ""))


def write_cam_scene():
    """独立于 verify_morph_gpu 的 `_m15_cam.json`：那是**面部特写**。

    两个脚本共用一个场景文件的话，谁后跑谁改机位 —— 而"逐像素相同"这类
    判据必须建立在**机位固定**上，共用文件等于给自己埋一个偶发假红。
    """
    os.makedirs(mg.SCENE_DIR, exist_ok=True)
    scene = {
        "version": 1,
        "generator": "verify_ik.py",
        "camera": CAM,
        "light": {"direction": [0.42, 0.72, -0.55], "color": [1.0, 0.96, 0.9],
                  "intensity": 3.2, "castsShadow": True, "ambientScale": 1.0},
        "physics": {"enabled": False},
        "entities": [{
            "name": "Ground",
            "transform": {"position": [0.0, 0.0, 0.0],
                          "rotation": [0.0, 0.0, 0.0],
                          "scale": [1.0, 1.0, 1.0]},
            "parent": -1, "visible": True, "castShadow": False, "locked": True,
            "mesh": {"kind": "builtin", "shape": "plane", "size": 40.0,
                     "name": "ik_ground"},
            "material": {"kind": "procedural", "name": "ik_ground_mat",
                         "baseColor": [0.42, 0.44, 0.47, 1.0], "metallic": 0.0,
                         "roughness": 0.9, "normalScale": 0.0, "ao": 1.0,
                         "emissive": [0.0, 0.0, 0.0], "doubleSided": False,
                         "alphaBlend": False},
        }],
    }
    with open(os.path.join(mg.SCENE_DIR, SCENE), "w", encoding="utf-8") as f:
        json.dump(scene, f, ensure_ascii=False, indent=1)
    return "scenes/" + SCENE


def first(regex, lines):
    for ln in lines:
        m = regex.search(ln)
        if m:
            return m
    return None


def run(tag, cmd):
    mg.OUT_DIR = OUT_DIR
    return mg.run_case("ik_" + tag, "scenes/" + SCENE, cmd)


def main():
    print("=" * 72)
    print("两骨 IK 验收（真实资产 + 真实导入 + 截图）")
    print("=" * 72)
    scene = write_cam_scene()

    runs = {}
    for tag, cmd in (("base", None),
                     ("off", "ik.set %s; ik.weight 0" % TARGET_OK),
                     ("on", "ik.set %s" % TARGET_OK),
                     ("far", "ik.set %s" % TARGET_FAR)):
        print("\n--- %s ---%s" % (tag, ("  cmd: " + cmd) if cmd else ""))
        img, lines, vp = run(tag, cmd)
        runs[tag] = (img, lines, vp)
        print("    image=%s vp=%s lines=%d"
              % (img is not None, vp, len(lines)))

    imgs = {k: v[0] for k, v in runs.items()}
    vps = {k: v[2] for k, v in runs.items()}
    logs = {k: v[1] for k, v in runs.items()}

    check("K0 screenshot", all(i is not None for i in imgs.values()),
          ",".join(k for k, i in imgs.items() if i is None) or "all")
    if any(i is None for i in imgs.values()):
        return finish()

    rects = {k: tuple(v) for k, v in vps.items() if v}
    check("K0b viewport rect", len(rects) == 4 and len(set(rects.values())) == 1,
          str(set(rects.values())))
    rect = next(iter(rects.values())) if rects else (0, 0, 1, 1)

    # ---- K0c 视口里真有内容 ----
    #
    # ⚠ 这个数不是"角色占屏比例"，而是"与背景色差 >30 的像素比例"，而本场景
    #   的天空和地面都是浅灰（互相在 30 以内）→ 它约等于**角色剪影**。
    #   机位收到 1.9 m 后角色占了视口大半，实测 content = **0.66**（占比口径，
    #   即 66%），阈值取 0.05 与 verify_morph_gpu.py 一致。
    #   它的职责只有一个：**空场景必须红**。没有它的话 K1「逐像素相同」会在
    #   一片空白上平凡通过（上一刀的 morph 验收就栽在这里）。
    pct, bbox, content = mg.diff_stats(imgs["base"], imgs["base"], rect)
    check("K0c viewport has content", content is not None and content >= 0.05,
          "content=%.2f%%" % (100.0 * content if content is not None else -1.0))

    # ---- K1 weight=0 逐像素等于没有 IK ----
    p_off, _, _ = mg.diff_stats(imgs["base"], imgs["off"], rect)
    check("K1 weight0 == no-ik (pixelwise)", p_off is not None and p_off == 0.0,
          "diff=%.4f%%" % (100.0 * p_off if p_off is not None else -1.0))

    # ---- K2 / K2b 目标可达时画面真的变了，且只动局部 ----
    # 实测 p_on = 0.0265%（占视口），阈值 0.0002（0.02%）留 1.3× 余量。
    p_on, b_on, _ = mg.diff_stats(imgs["base"], imgs["on"], rect)
    check("K2 ik changes pixels", p_on is not None and p_on >= 0.0002,
          "diff=%.4f%%" % (100.0 * p_on if p_on is not None else -1.0))
    # 实测 b_on = 0.13（差异包围盒只占视口 13%）→ 动的是一条腿。
    # 阈值 0.45：超过它说明整屏或半个屏都在变，那不是 IK 而是相机/整体位姿变了。
    check("K2b change is local", b_on is not None and b_on <= 0.45,
          "bbox=%.2f%%" % (100.0 * b_on if b_on is not None else -1.0))

    # ---- K3 / K4 / K5 日志账 ----
    m_set = first(RE_IK_SET, logs["on"])
    check("K7 chain auto-resolved",
          bool(m_set) and m_set.group(2) == "1" and
          m_set.group(3) == "LeftUpLeg>LeftLeg>LeftFoot",
          m_set.group(3) if m_set else "no IK-SET")

    m_on = first(RE_ANIM_IK, logs["on"])
    ok_reach = (m_on is not None and m_on.group(2) == "0" and
                float(m_on.group(5)) < 0.005)
    check("K3 foot reaches target", ok_reach,
          ("actual=%s miss=%s bone=%s" % (m_on.group(5), m_on.group(2),
                                          m_on.group(3))) if m_on
          else "no ANIM-IK")

    # weight=0 时 solver 在写 pose 之前就 return（`applied=false`），所以系统
    # 的 `m_lastIK` 保持 0 —— 日志里**一条 ANIM-IK 都不该有**。
    # 这条同时证明"solver 没被调用"与"没被调用时也不刷日志"。
    m_off = first(RE_ANIM_IK, logs["off"])
    check("K4 weight0 not solved", m_off is None,
          ("unexpected: actual=%s" % m_off.group(5)) if m_off else "")

    m_far = first(RE_ANIM_IK, logs["far"])
    check("K5 unreachable reported honestly",
          m_far is not None and m_far.group(2) == "1" and
          float(m_far.group(5)) > 0.05,
          ("actual=%s miss=%s bend=%sdeg" % (m_far.group(5), m_far.group(2),
                                             m_far.group(6))) if m_far
          else "no ANIM-IK")

    # ---- K6 够不着也真的动了（腿伸直），并且没崩溃 ----
    p_far, _, _ = mg.diff_stats(imgs["base"], imgs["far"], rect)
    check("K6 unreachable still moves",
          p_far is not None and p_far >= 0.0002,
          "diff=%.4f%%" % (100.0 * p_far if p_far is not None else -1.0))
    check("K8 no crash", all(len(v[1]) > 50 for v in runs.values()),
          "minlines=%d" % min(len(v[1]) for v in runs.values()))

    return finish()


def finish():
    print("\n" + "=" * 72)
    if _bad:
        print("结果: FAIL（%d/%d 项）" % (_bad, _n))
        return 1
    print("结果: PASS（%d 项）" % _n)
    return 0


if __name__ == "__main__":
    sys.exit(main())
