"""端到端验证：语音播完查询 + 对话「语音没播完不翻页」（D1，2026-10-31）。

被验证的缺口（原话出处：`docs/remaining_work_2026-10-27.md` §2.4 D1）：
    `startVoice` 返回句柄，但**无「播完了」查询/回调** → 「语音播完再进下一句」
    做不了 → **卡住 C2 的语音版**。

补的两层：
  · 引擎侧 —— `MixStats::busVoices[]` + `AudioSystem::activeVoicesOnBus()` →
    `AudioSystem::voicePlaying()` / `voiceSerial()`（按**总线**统计，不是全局
    声部：后台循环音乐会让「这一句播完了吗」永远为真）。
  · 对话侧 —— `DlgNode::voice` + `VoiceHooks` + `Press::WaitingVoice`。
    **钩子由宿主注入，默认不装 = 老行为逐位不变。**

四组：
  A. 内核（`--selftest dialogue`，语音组 38 条）
     A1 总数与零失败
     A2 代表断言必须真的出现过（只数总数的话，删掉关键几条也能凑够）
  B. 有语音 → 正文出完后**被挡**，语音播完才放行（`MYVK_DLG_VOICE_FAKE=2`）
     B1 钩子确实装上了（DLG-VOICE-FAKE）
     B2 这一句起了语音（DLG-VOICE，limit=7.00）
     B3 挡了不止一次且 `stalls` 在涨（不是偶发一条）
     B4 播完后真的翻到 v2
     B5 **v2 没有 voice → 一条都不许挡**（否则「每句都等」这个 bug 测不出来）
  C. 否定测试：老资产（无 voice 字段）+ 钩子恒为「在播」
     **一条 waiting-voice 都不许有**。没有这一组的话，「不管有没有语音都设门」
     这个实现会全绿 —— 症状是所有老对话每句白等 3 秒。
  D. 超时兜底：钩子**恒为真**（假装播不完）
     对话仍然必须自己走出去（DLG-END 必须出现）。这是防「永久卡死在一页」的
     唯一保险 —— 音频线程挂掉时 `playing()` 会永远返回 true。

用法：python tools/verify_dialogue_voice.py
"""

import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
GAME = os.path.join(BIN, "Game.exe")
SRC = os.path.join(ROOT, "assets")
DST = os.path.join(BIN, "assets")

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
    """夹具必须落到 `build/bin/assets/` 下 —— `--scene` 是相对 assets 根的。

    ⚠⚠ 这一条踩过不止一次：新加的资产**不会**自动进 `build/bin/`，
      症状是"加了完全没效果"或"`--scene` 打不开但不报错"。
    """
    pairs = [
        (os.path.join(SRC, "scenes", "_dlg_voice_fixture.json"),
         os.path.join(DST, "scenes", "_dlg_voice_fixture.json")),
        (os.path.join(SRC, "scenes", "_dlg_novoice_fixture.json"),
         os.path.join(DST, "scenes", "_dlg_novoice_fixture.json")),
        (os.path.join(SRC, "dialogues", "_voice_fixture.dlg.json"),
         os.path.join(DST, "dialogues", "_voice_fixture.dlg.json")),
    ]
    for src, dst in pairs:
        if not os.path.exists(src):
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(src, dst)


def run(cmd, env_extra, timeout=240):
    env = dict(os.environ)
    env.update(env_extra)
    try:
        p = subprocess.run(cmd, cwd=BIN, env=env, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True,
                           encoding="utf-8", errors="replace", timeout=timeout)
        return p.stdout or ""
    except subprocess.TimeoutExpired as e:
        # ⚠ 超时也要**返回已有输出**：返回空串会让"没跑起来"和"跑起来但没日志"
        #   看起来完全一样。
        out = e.stdout or ""
        if isinstance(out, bytes):
            out = out.decode("utf-8", "replace")
        return out


def lines_with(text, needle):
    return [l for l in text.splitlines() if needle in l]


def waiting_lines(text, node_id=None):
    out = []
    for l in lines_with(text, "DLG-PRESS waiting-voice"):
        if node_id and ("id=" + node_id + " ") not in l:
            continue
        out.append(l)
    return out


def stalls_of(lines):
    out = []
    for l in lines:
        m = re.search(r"stalls=(\d+)", l)
        if m:
            out.append(int(m.group(1)))
    return out


def main():
    if not os.path.exists(GAME):
        print("找不到可执行文件，请先构建：", GAME)
        return 1
    sync_fixtures()

    # ============================================================ A 内核
    print("\n[A] 内核自检（--selftest dialogue 的语音组）")
    out = run([GAME, "--selftest", "dialogue"], {})
    m = re.search(r"DLG-TOTAL pass=(\d+) fail=(\d+)", out)
    if not m:
        check(False, "A1 自检没跑起来（输出里没有 DLG-TOTAL）")
        print(out[-1200:])
        return 1
    npass, nfail = int(m.group(1)), int(m.group(2))
    check(nfail == 0, "A1 零失败（pass=%d fail=%d）" % (npass, nfail))
    # 语音组 38 条 + 原有的 ~103 条。取 138 留一点余量，但**不许**掉到 130 以下
    # —— 那说明有人整组删了。
    check(npass >= 138, "A1 断言条数够（%d >= 138）" % npass)
    # A2：只数总数是不够的 —— 删掉关键几条、换成无关的也能凑够条数。
    for key, label in (
            ("voice_limit_v1", "超时上限按字数算（不是只取下界 3）"),
            ("voice_reveal_not_blocked", "补字那一下不许被语音挡"),
            ("voice_blocked", "补完字之后才挡"),
            ("voice_v2_not_blocked", "没有 voice 的那句不许挡"),
            ("voice_timeout_releases", "超时兜底必须放行"),
            ("voice_nohook_moves", "钩子不装 = 老行为"),
            ("voice_playfail_moves", "起播失败不设门"),
            ("sys_voice_plays", "钩子在 autoStart 之前装"),
            ("sys_voice_cue_plays", "cue 槽位当场拿到钩子"),
            ("rt_voice_omits_empty", "老资产不长出 voice 空串"),
    ):
        check(key in out, "A2 代表断言在：%s" % label)

    base = {"MYVK_DLG_AUTO": "0.3"}

    # ============================================================ B 挡 / 放行
    print("\n[B] 有语音：正文出完后被挡，播完才放行（fake=2.0s）")
    out = run([GAME, "--scene", "scenes/_dlg_voice_fixture.json",
               "--frames", "220"], dict(base, MYVK_DLG_VOICE_FAKE="2.0"))
    check("DLG-VOICE-FAKE sec=2.00" in out,
          "B1 假钩子确实装上了（否则后三条是假绿）")
    voice = lines_with(out, "DLG-VOICE node=v1")
    check(bool(voice) and "limit=7.00" in voice[0],
          "B2 v1 起了语音，上限 7.00（%s）" % (voice[0].split("DLG-VOICE ")[-1]
                                              if voice else "无"))
    wl = waiting_lines(out, "v1")
    check(len(wl) >= 2, "B3 被挡了不止一次（%d 次）" % len(wl))
    st = stalls_of(wl)
    check(bool(st) and st == sorted(st) and st[-1] >= 2,
          "B3b stalls 单调递增到 >=2（%s）" % (st[:3] + ["..."] + st[-1:]
                                               if len(st) > 4 else st))
    check(bool(lines_with(out, "DLG-LINE id=v2")),
          "B4 语音播完后真的翻到了 v2")
    check(not waiting_lines(out, "v2"),
          "B5 v2 没有 voice → 一条 waiting-voice 都不许有")
    check(not re.search(r"\[ERROR\]|VUID-", out), "B6 零 ERROR / 零 VUID")

    # ============================================================ C 否定
    print("\n[C] 否定测试：老资产（无 voice 字段）+ 钩子恒为「在播」")
    out = run([GAME, "--scene", "scenes/_dlg_novoice_fixture.json",
               "--frames", "200"], dict(base, MYVK_DLG_VOICE_FAKE="999"))
    check("DLG-VOICE-FAKE sec=999.00" in out,
          "C1 钩子确实装了且恒为真（否则 C2 是假绿）")
    check(not lines_with(out, "DLG-VOICE node="),
          "C1b 没有 voice 的资产 → 一条 DLG-VOICE 都没有")
    check(len(waiting_lines(out)) == 0,
          "C2 一条 waiting-voice 都不许有（%d 条）"
          % len(waiting_lines(out)))
    # 反面：对话本身必须真的在跑，否则"没挡"只是"没开始"
    check(bool(lines_with(out, "DLG-LINE id=")), "C3 对话确实在推进")
    check(not re.search(r"\[ERROR\]|VUID-", out), "C4 零 ERROR / 零 VUID")

    # ============================================================ D 超时兜底
    print("\n[D] 超时兜底：钩子恒为真也必须自己走出去（fake=999s）")
    out = run([GAME, "--scene", "scenes/_dlg_voice_fixture.json",
               "--frames", "300"], dict(base, MYVK_DLG_VOICE_FAKE="999"))
    wl = waiting_lines(out, "v1")
    check(len(wl) > 0, "D1 确实被挡过（%d 次）" % len(wl))
    # 计时**硬钳**在上限：任何一条都不许出现 wait > limit。
    # ⚠ 第一版写的是"最后一条必须 ≈ 7.00/7.00"，它假设被挡到最后一次按下的
    #   时刻正好等于上限 —— 实际最后一次按在 6.70，下一次按（+0.3 s 自动间隔）
    #   已经越线，于是那一按**直接放行、不打 waiting 行**。判据跟着实现走：
    #   看"有没有越过"（恒假的东西才是有意义的约束）+ "有没有挡到边上"。
    pairs = [re.search(r"wait=([\d.]+)/([\d.]+)", l) for l in wl]
    pairs = [(float(m.group(1)), float(m.group(2))) for m in pairs if m]
    over = [w for w, lim in pairs if w > lim + 1e-3]
    check(bool(pairs) and not over,
          "D2 计时从不越过上限（%d 条，越线 %d 条）" % (len(pairs), len(over)))
    top = max((w for w, _ in pairs), default=0.0)
    check(top >= 7.0 - 0.5, "D2b 一直挡到上限边上（最远 waited=%.2f/7.00）" % top)
    check(bool(lines_with(out, "DLG-END")),
          "D3 到点自己放手了（DLG-END 出现，没永久卡死）")
    check(bool(lines_with(out, "DLG-LINE id=v2")), "D4 真的走到了下一句")
    check(not re.search(r"\[ERROR\]|VUID-", out), "D5 零 ERROR / 零 VUID")

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
