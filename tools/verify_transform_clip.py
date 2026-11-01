"""端到端验证：实体 TRS 轨道的运行时消费者（C5，2026-10-31）。

被验证的缺口（原话出处：`docs/remaining_work_2026-10-27.md` §2.3 C5）：
    `autoplay` 字段有并落盘，但**运行时无消费者** → 门 / 电梯 / 机关在
    Game.exe 里全是死的。

四组：
  A. 内核（`--selftest transformclip`，35 条）
     A1 总数与零失败
     A2 六条代表断言必须真的出现过（只数总数的话，换个断言名也能凑够 35）
  B. **编辑器 Play**（`MYVK_EDITOR_PLAY=1`）
     这条路径与 Game.exe 是**两套接线**（EditorApp 的内嵌循环 vs RuntimeApp），
     只测一边很容易漏。B 组证明编辑器里点 Play 机关也活。
  C. Game.exe 运行时
     C1 有推进（ANIMCLIP-TICK 至少 3 条）
     C2 pos.z 单调不减（不是"抖动"或"随机跳"）
     C3 2 秒后真的回绕（t 从 ~1.0 回到 ~0.0）—— 证明 loop 生效
     C4 零 ERROR / 零 VUID
  D. 否定测试：`autoplay=false` 的同一份夹具
     **一条 ANIMCLIP-TICK 都不许有**。没有这一组的话，"无论 autoplay 都播"
     这个 bug 测不出来（它是"多的行为"，不是"少的行为"）。

用法：python tools/verify_transform_clip.py
"""

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
GAME = os.path.join(BIN, "Game.exe")
EDITOR = os.path.join(BIN, "Editor.exe")
SRC_SCENES = os.path.join(ROOT, "assets", "scenes")
DST_SCENES = os.path.join(BIN, "assets", "scenes")

fails = []
n_pass = 0


def check(ok, msg):
    global n_pass
    print("    %-76s %s" % (msg, "PASS" if ok else "FAIL"))
    if not ok:
        fails.append(msg)
    else:
        n_pass += 1
    return ok


def sync_fixtures():
    """夹具必须落到 build/bin/assets/scenes/ —— `--scene` 是相对 assets 根的。

    ⚠⚠ 这一条踩过不止一次：新加的资产**不会**自动进 `build/bin/`，
      症状是"加了完全没效果"或"`--scene` 打不开但不报错"。
    """
    os.makedirs(DST_SCENES, exist_ok=True)
    for name in ("_clip_fixture.json", "_clip_fixture_off.json"):
        src = os.path.join(SRC_SCENES, name)
        dst = os.path.join(DST_SCENES, name)
        if os.path.exists(src):
            with open(src, "rb") as f:
                data = f.read()
            with open(dst, "wb") as f:
                f.write(data)


def run(cmd, env_extra, timeout=180):
    env = dict(os.environ)
    env.update(env_extra)
    try:
        p = subprocess.run(cmd, cwd=BIN, env=env, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True,
                           encoding="utf-8", errors="replace", timeout=timeout)
        return p.stdout or ""
    except subprocess.TimeoutExpired as e:
        # ⚠ 超时要**返回已有输出**，不能返回空串：空串会让"没跑起来"和
        #   "跑起来但没日志"看起来一样。
        out = e.stdout or ""
        if isinstance(out, bytes):
            out = out.decode("utf-8", "replace")
        return out


def lines_with(text, needle):
    return [l for l in text.splitlines() if needle in l]


def tick_samples(text):
    """抓 ANIMCLIP-TICK 的 (t, z) 序列。"""
    out = []
    for l in lines_with(text, "ANIMCLIP-TICK"):
        mt = re.search(r"\bt=(-?[\d.]+)", l)
        mz = re.search(r"pos=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\)", l)
        if mt and mz:
            out.append((float(mt.group(1)), float(mz.group(3))))
    return out


def main():
    if not os.path.exists(GAME):
        print("找不到可执行文件，请先构建：", GAME)
        return 1
    sync_fixtures()
    os.makedirs(os.path.join(ROOT, "..", "_venn_verify"), exist_ok=True)

    # ============================================================ A 内核
    print("\n[A] 内核自检")
    out = run([GAME, "--selftest", "transformclip"], {})
    m = re.search(r"TOTAL pass=(\d+) fail=(\d+)", out)
    if not m:
        check(False, "A1 自检没跑起来（输出里没有 TOTAL）")
        print(out[-1200:])
        return 1
    npass, nfail = int(m.group(1)), int(m.group(2))
    check(nfail == 0, "A1 零失败（pass=%d fail=%d）" % (npass, nfail))
    check(npass >= 35, "A1 断言条数够（%d >= 35）" % npass)
    # A2：只数总数是不够的 —— 有人把 6 条删掉、换成 6 条无关的也能凑够。
    for key, label in (
            ("C1 dt=0 不写 transform", "C1 没推进就不写 transform"),
            ("A6 t=1/4 处是 smoothstep", "A6 插值必须是 smoothstep"),
            ("B3 loop 回绕用 fmod", "B3 循环回绕"),
            ("B5 duration=0 不产生 NaN", "B5 duration=0 的兜底"),
            ("D1 playing 跟 autoplay 走", "D1 播放状态跟 autoplay"),
            ("E2 scale 也参与混合", "E2 scale 也参与混合"),
    ):
        check(key in out, "A2 代表断言在：%s" % label)

    # ============================================================ B 编辑器 Play
    print("\n[B] 编辑器 Play 里机关也活")
    out = run([EDITOR], {
        "MYVK_EDITOR_PLAY": "1",
        "MYVK_EDITOR_SCENE": "scenes/_clip_fixture.json",
        "MYVK_FRAMES": "150",
        "MYVK_ANIMCLIP_LOG": "1",
        "MYVK_EDITOR_NO_HUD": "1",
        "MYVK_NO_WINDOW_SAVE": "1",
    }, timeout=240)
    b_ticks = tick_samples(out)
    check(len(b_ticks) >= 2,
          "B1 编辑器 Play 里轨道在推进（ANIMCLIP-TICK %d 条）" % len(b_ticks))
    check(any(z > 0.05 for _, z in b_ticks),
          "B2 位置真的变了（max z=%.3f）"
          % (max((z for _, z in b_ticks), default=0.0)))
    check(not re.search(r"\[ERROR\]|VUID-", out),
          "B3 零 ERROR / 零 VUID")

    # ============================================================ C Game.exe
    print("\n[C] Game.exe 运行时")
    out = run([GAME, "--scene", "scenes/_clip_fixture.json", "--frames", "260"], {
        "MYVK_ANIMCLIP_LOG": "1",
    })
    ticks = tick_samples(out)
    check(len(ticks) >= 3, "C1 至少 3 条采样（%d）" % len(ticks))
    if ticks:
        zs = [z for _, z in ticks]
        # 单调不减：允许相等（60 帧采样可能落在同一位置），不许下降
        mono = all(zs[i + 1] >= zs[i] - 1e-4 for i in range(len(zs) - 1))
        # ⚠ 但**回绕那一下必须下降** —— 所以"全局单调"只在没有回绕时成立。
        #   这里改成：任意一条都必须在 [0, 4.2] 内（不是乱飞），
        #   且整段里至少出现过一次"z 从大变小"（= 回绕）。
        inrange = all(-0.05 <= z <= 4.2 for z in zs)
        check(inrange, "C2 所有采样都在轨道范围内 [0, 4]（max=%.2f）"
              % max(zs))
        wrapped = any(zs[i + 1] < zs[i] - 0.5 for i in range(len(zs) - 1))
        check(wrapped, "C3 loop 真的回绕了（出现过 z 骤降）")
        # C2b：**只有回绕才允许下降**，而且下降幅度必须是"整段"级别的。
        #   ⚠ 第一版写的是"除回绕外单调（上升数 >= 总数-1）"，它假设只回绕
        #     一次 —— 260 帧 ≈ 4.3 s 会回绕 **两** 次，于是判据恒假。
        #   现在改成看**下降的性质**而不是次数：一次抖动会掉几个像素，
        #   一次回绕会从 4 掉到 0（> 0.5）。这样同时钉住"不随机跳"和
        #   "回绕确实发生"，且与跑了多久无关。
        drops = [zs[i] - zs[i + 1] for i in range(len(zs) - 1)
                 if zs[i + 1] < zs[i] - 1e-4]
        check(all(d > 0.5 for d in drops),
              "C2b 只有回绕才下降（%d 次下降，最小 %.2f）"
              % (len(drops), min(drops) if drops else 0.0))
    check(not re.search(r"\[ERROR\]|VUID-", out), "C4 零 ERROR / 零 VUID")

    # ============================================================ D 否定
    print("\n[D] 否定测试：autoplay=false")
    out = run([GAME, "--scene", "scenes/_clip_fixture_off.json",
               "--frames", "120"], {"MYVK_ANIMCLIP_LOG": "1"})
    off_ticks = tick_samples(out)
    check(len(off_ticks) == 0,
          "D1 autoplay=false → 一条推进都没有（%d 条）" % len(off_ticks))
    # 但场景本身要读进来（否则"没推进"可能只是"场景没加载"）
    check("_clip_fixture_off.json" in out or "SCENE-ANIMCLIP" in out,
          "D2 场景与组件确实加载了（否则 D1 是假绿）")

    print("\n==== %s ====" %
          ("全部断言 PASS（%d 条）" % n_pass if not fails
           else "断言 %d 项 FAIL（过 %d 条）" % (len(fails), n_pass)))
    for f in fails:
        print("  -", f)
    if fails:
        return 1
    print("结果: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
