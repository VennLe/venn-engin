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
    # ---- 表演层（2026-10-25）：L2 LookAt + L4 弹簧骨 ----
    # 全是数值断言：链识别 / 距离守恒 / 收敛 / 限幅。
    # 头发歪 3cm 与歪 5cm 在 1080p 截图上长得一样，这些只能断数值。
    "spring_rig_count",         # 匹配前缀且父骨也匹配 → 2 根（不是 3）
    "spring_rig_restlen",       # 链长取自静止姿势（不是运行时姿势）
    "spring_rig_nomatch",       # 前缀对不上 → 0 根，不崩
    "spring_first_frame",       # 第一帧只初始化、不改姿态（返回 0）
    "spring_len_attach",        # 到 attach 骨的距离 == 链长
    "spring_len_chain",         # 链内相邻质点的距离 == 链长
    "spring_converged",         # 静止 60 帧后位置不再变（收敛，不震荡）
    "spring_lag",               # 头突然转 90° → 发梢落后于纯动画位置
    "lookat_resolve_auto",      # 空名字 → 自动找到 head
    "lookat_resolve_byname",    # 按名字精确命中
    "lookat_resolve_missing",   # 找不到 → -1（而不是静默取第 0 根）
    "lookat_unclamped_180",     # 不限幅时确实转得过去
    "lookat_clamped_45",        # 限幅 45° 时停在 45°
    "lookat_weight_zero",       # weight=0 → 姿态逐位不变
    "spring_integration_first_frame",  # 走完整 tick 路径的第一帧逐位相同
    # ---- 第八刀（2026-10-25）：Skeleton::findJoint 抽取 + 两骨 IK + 挂点 ----
    # 名字匹配那一组不是"函数有返回值"，而是**那个标志位真的有用**：
    # 不带 SkipEndTips 会命中 `Head_end`（骨尖辅助骨，2），带的时候命中真头（3）。
    "find_joint_exact",          # 精确优先于数组顺序（hand_l 在 hand_l_end 之后）
    "find_joint_substring",      # 不区分大小写 + 包含：HEAD → mixamorig:Head
    "find_joint_endtip_exact",   # 精确写出 Head_end 时必须拿到它（精确不过滤）
    "find_joint_endtip_hit",     # 不带标志位 → 命中骨尖（这就是标志位的理由）
    "find_joint_endtip_skip",    # 带标志位 → 命中真头
    "find_joint_empty",          # 空名字 → -1（不是 0）
    "find_joint_missing",        # 对不上 → -1
    "find_prefix_count",         # 前缀批量匹配
    "find_prefix_empty",         # 空前缀**不**匹配一切
    # IK 组：脚离地 2cm 和 20cm 在 1080p 里是同一只脚，只能断数值。
    "ik_resolve_auto_root",      # 空名字 → 自动挑 thigh
    "ik_resolve_auto_mid",       # → 最长子骨（小腿）
    "ik_resolve_auto_end",       # → 再下一跳（脚）
    "ik_resolve_byname",         # 显式名字
    "ik_resolve_broken",         # 名字找得到但不成链 → 整条拒绝
    "ik_resolve_missing",        # 名字对不上 → 拒绝
    "ik_control_gt_10cm",        # **对照组**：不解算时差 >10cm（没有它等于没测）
    "ik_applied",                # 真的写了 pose
    "ik_reach_error",            # 末端落在目标上（<2mm）
    "ik_bend_deg",               # 余弦定理的定值（d=0.728 → 弯 49.0°）
    "ik_weight_zero",            # weight=0 → 姿态逐位不变
    "ik_no_target",              # 无目标 → 不动
    "ik_deterministic",          # 同一输入两次逐位相同（选解析解的核心理由）
    "ik_pole_follows_anim_a",    # 极向量从当帧动画反推：A 侧
    "ik_pole_follows_anim_b",    # 同一目标、换弯曲平面 → 弯向另一边
    "ik_unreachable_finite",     # 够不着也不能出 NaN
    "ik_unreachable_straight",   # 够不着时腿伸直（弯 <8°）
    "ik_unreachable_error",      # 并且**如实**报出剩余误差（9.20）
    "ik_too_close_fold",         # 目标贴脸 → 折到底（≈180°）
    "ik_pole_degenerate_fallback",  # 腿完全伸直时极向量退化 → 退到 poleVector
    "ik_integration_count",      # 组件真的被系统看见了
    "ik_integration_reached",    # 走完整 tick 路径后关节矩阵里的末端在目标上
    # 挂点组：位置可以走两条完全独立的算法（jointMatrices 反推 vs 纯 FK）
    "attach_count",              # 真的摆好了
    "attach_follows_anim",       # 与纯 FK 独立算出的位置一致（互校）
    "attach_moved",              # 并且确实跟着动画转过去了
    "attach_missing_count",      # 骨名对不上 → 计 1 次失败
    "attach_missing_keeps_transform",  # 且不改写 TransformComponent
    # ---- 状态机（2026-10-25）：A2b，把 animator 上的占位字段接成真的 ----
    # 这一组是"七个假开关"里五个的验收：controllerPath 被读、stateIndex 与
    # stateTime 被读写、三张参数表被条件消费。
    "sm_load_ok",               # 内存 JSON 能解析成有效控制器
    "sm_validate_rejects",      # 三类配置错误全部被拦下（未知状态 / 重名 / 未声明参数）
    "sm_init_dt_zero_state",    # dt=0 也要初始化到 defaultState（编辑器语义）
    "sm_init_dt_zero_clip",     # 且 clip 真的切过去了
    "sm_init_dt_zero_time",     # 但 stateTime 不推进
    "sm_init_state_time",       # dt>0 时 stateTime 开始走
    "sm_transition_count",      # 条件成立 → 本帧采纳 1 条转移
    "sm_transition_state",      # stateIndex 真的变了
    "sm_transition_clip",       # clipName 真的变了
    "sm_state_time_reset",      # 转移后 stateTime 归零（hasExitTime 靠它）
    "sm_fade_dur",              # transition.duration 真的进了 fadeDur
    "sm_fading",                # 真的进入了交叉淡入（不是硬切）
    "sm_one_per_frame",         # 多条同时满足时一帧只走一步
    "sm_trigger_consumed",      # trigger 被采纳后立即清零
    "sm_trigger_no_reentry",    # 清零后不会重新触发（否则无限起跳）
    "sm_exit_load",
    "sm_exit_blocked",          # hasExitTime 未到点 → 不许走
    "sm_exit_passed",           # 过点后放行
    "sm_phase_continuous",      # restart=false 按归一化相位换算播放头
    "sm_clip_missing_ignored",  # 目标 clip 不存在 → 忽略该转移（不切 T-pose）
    "sm_clip_missing_kept_clip",# 且保持原 clip
    "sm_load_fail_fallback",    # 路径不存在 → 不崩，回落到单 clip 直放
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
        if total[0] + total[1] != len(seen):
            # ⚠ 比的是 pass+fail 而不是只比 pass：只比 pass 的话，
            #   任何一条 FAIL 都会连带报一条"计数不一致"，让人以为是
            #   两个独立的问题（2026-10-25 加弹簧骨断言时踩到）。
            failures.append("ANIM-TOTAL pass=%d fail=%d 与脚本数到的 %d 行不一致"
                            % (total[0], total[1], len(seen)))

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
