#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""回归包装：骨骼动画的**无窗口数值自检**（角色链 A0-3 / A0-5）。

真正的断言在引擎里（`engine/animation/AnimationSelfTest.cpp`）——它不初始化
Vulkan、不开窗口，`Editor.exe` 跑完打印就退出，所以这一项是回归里最快的。

本脚本只做三件事：
  1) 跑 `MYVK_ANIM_TEST=1 Editor.exe`，把 `ANIM-TEST` 行原样转成人读的表；
  2) 断言 `ANIM-TOTAL fail=0`，并且**每一项都出现过**（防止某个断言被
     `#if 0` 掉之后脚本还绿）；
  3) 把 `ANIM-TOTAL` 的计数与脚本侧看到的行数对账（防止"引擎只跑了 3 项
     就提前 return"这种静默缩水）。

⚠ 为什么值得单独一个用例：这条链过去**一个自动化用例都没有**，因为采样
   埋在 `Renderer::updateSkinning` 里 —— 任何动画逻辑都必须开 GUI + 有 GPU
   才能验证。A0-5 把它剥到 `engine/animation/` 之后才有了这个入口。

运行：python tools/verify_anim_runtime.py
结尾一定是 `结果: PASS` / `结果: FAIL: ...`。
"""

from __future__ import annotations

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")

# 期望出现的断言清单。**新增一条自检就必须在这里补一行** ——
# 否则"引擎里删掉了它"不会被发现（脚本只看到剩下那些是 PASS）。
EXPECTED = [
    "tick_120f",            # 120 帧 @1/60 → 2.0 s（时间真的在加）
    "tick_entities",        # 系统看到了那个实体
    "wrap_loop",            # loop 时 fmod 回绕
    "clamp_noloop",         # 不循环时钳到末帧（逐位）
    "paused_time",          # playing=false 时不推进
    "dt_zero_no_advance",   # dt=0 只刷新姿势、不推时间（编辑态语义）
    "refactor_bitwise",     # 拆分前后关节矩阵 **memcmp 级**相同
    "shell_equivalent",     # Skeleton::sample 兼容壳与 Pose 路径等价
    "blend_w0_bitwise",     # 混合端点 w=0 逐位短路
    "blend_w1_bitwise",     # w=1 同理
    "blend_mid",            # w=0.5 == slerp(qa,qb,.5)
    "rest_hand_x",          # 静止姿势的世界位置
    "hierarchy_hand_x",     # 父旋转 90° 后子骨骼落点（拓扑序正确）
    "hierarchy_hand_y",
    "joint_matrices_filled",  # 真的把关节矩阵写进组件
    "null_model_noop",      # 坏输入不崩
    # ---- A1（2026-10-19）：AnimatorComponent 接管 time 的权威 ----
    "animator_authoritative",   # 有人写 sk.time → 被镜像回 anim.time
    "animator_count",           # 系统认出了走 Animator 路径的实体
    "animator_advance",         # 120 帧 @1/60 → anim.time = 2.0
    "animator_mirror",          # sk.time 是 anim.time 的镜像
    "animator_paused",          # anim.playing=false 压过 sk.playing=true
    "animator_speed",           # anim.speed=2 压过 sk.speed=1
    "animator_speed_mirror",    # speed 也镜像
    "animator_default_clip",    # 空 clipName → 补 defaultClip
    "animator_default_once",    # 只在第一次补；之后清空就真的空着
    "animator_model_fallback",  # anim.model 空 → 用 sk.model
    "animator_joint_bitwise",   # Animator 路径的关节矩阵与参考实现逐位相同
]

TEST_RE = re.compile(r"ANIM-TEST\s+(\S+)\s+actual=(\S+)\s+expected=(\S+)\s+"
                     r"tol=(\S+)(?:\s+detail=(.*?))?\s+result=(PASS|FAIL)")
TOTAL_RE = re.compile(r"ANIM-TOTAL\s+pass=(\d+)\s+fail=(\d+)")

# 只要行里有这个前缀就必须被 TEST_RE 吃掉。漏掉一条 = 断言在脚本眼里
# 静默消失（已踩：detail 里带空格导致 refactor_bitwise 匹配失败，引擎明明
# PASS，脚本却报"缺少断言"）。宁可在这里红，也别让断言无声明地消失。
PREFIX = "ANIM-TEST"


def main() -> int:
    print("=" * 72)
    print("骨骼动画自检（MYVK_ANIM_TEST=1，无窗口 / 无 Vulkan）")
    print("=" * 72)

    if not os.path.isfile(EXE):
        print("结果: FAIL: 找不到 %s（先 ./make.cmd build）" % EXE)
        return 1

    env = dict(os.environ)
    env["MYVK_ANIM_TEST"] = "1"
    proc = subprocess.run([EXE], cwd=os.path.dirname(EXE), env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, encoding="utf-8", errors="replace",
                          timeout=120)
    out = proc.stdout or ""

    failures: list[str] = []
    seen: list[str] = []
    unmatched: list[str] = []
    for line in out.splitlines():
        if PREFIX not in line:
            continue
        m = TEST_RE.search(line)
        if not m:
            unmatched.append(line.strip())
            continue
        name, actual, expected, tol, detail, res = m.groups()
        seen.append(name)
        print("    %-24s actual=%-12s expected=%-12s %s"
              % (name, actual, expected, res))
        if res != "PASS":
            failures.append("%s: actual=%s expected=%s tol=%s%s"
                            % (name, actual, expected, tol,
                               (" (%s)" % detail) if detail else ""))

    if unmatched:
        failures.append("%d 条 ANIM-TEST 行的格式解析不了（细节详见下）"
                        % len(unmatched))
        for u in unmatched[:3]:
            print("    !! 解析失败: %s" % u)

    total = None
    for line in out.splitlines():
        m = TOTAL_RE.search(line)
        if m:
            total = (int(m.group(1)), int(m.group(2)))

    print()
    if proc.returncode != 0:
        failures.append("进程退出码 %d（自检有失败项时应当是 0，"
                        "非 0 也可能是崩溃）" % proc.returncode)
    if total is None:
        failures.append("没有 ANIM-TOTAL 行（自检没跑完？）")
        print("    ANIM-TOTAL 缺失")
    else:
        print("    ANIM-TOTAL pass=%d fail=%d" % total)
        if total[1] != 0:
            failures.append("ANIM-TOTAL fail=%d" % total[1])
        if total[0] != len(seen):
            failures.append("ANIM-TOTAL pass=%d 与脚本数到的 %d 行不一致"
                            % (total[0], len(seen)))

    missing = [n for n in EXPECTED if n not in seen]
    if missing:
        failures.append("缺少断言: %s（被删了？还是提前 return 了？）"
                        % ", ".join(missing))
        print("    缺少断言: %s" % ", ".join(missing))
    else:
        # ⚠ 这里不能写死数字：写死的话"新增自检"会先红一次，然后被人改成
        #   新数字了事 —— 而真正该断的是"EXPECTED 里的每一项都出现过"。
        print("    %d 项断言全部出现过" % len(EXPECTED))

    print()
    print("=" * 72)
    if failures:
        print("结果: FAIL（%d 项）" % len(failures))
        for f in failures:
            print("   - %s" % f)
        print("=" * 72)
        return 1
    print("结果: PASS（时间推进 / 循环回绕 / 逐位重构不变性 / 混合端点 / "
          "层次顺序，共 %d 项）" % len(seen))
    print("=" * 72)
    return 0


if __name__ == "__main__":
    sys.exit(main())
