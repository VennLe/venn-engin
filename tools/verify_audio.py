"""G14 音频：从"能解码"到"编辑器里听得见"的验收脚本。

三层互相独立，缺一层就有洞：

  A. **无窗口自检**（`Game.exe --selftest audio`）
     解码 / 声部池 / 总线 / ducking / 3D / 流式 / 确定性。这些断言都写在
     C++ 里（`engine/audio/AudioSelfTest.cpp`），脚本只负责"跑起来、
     数够条数、关键的几条真的绿"。⚠ 全部不需要声卡也不需要窗口 ——
     这是整条音频路线图能在任何机器上被验证的前提（§3.1）。

  B. **配置解析**（`--selftest audio-config`）
     `AUDIO-CONFIG` 那一行是 env / game.json 是否**真的**生效的唯一证据。
     音频的失败模式几乎全在"配了但没生效"上，所以这一层单独列。

  C. **设备后端**
     null（必定可用）/ wasapi（真设备）/ 假设备名（必须回落并留痕）/
     `__none__`（必须失败但**不能崩**）。⚠ 无声卡的机器上 C2 会 SKIP。

  D. **序列化往返**（`--scene` + `--dump-scene`）
     `saveScene` 与 `loadScene` 是两套代码，只测读，"存档丢字段"永远
     暴露不出来。这里把 audio 块逐字段比对（含 float32 精度）。
     外加**老场景对照**：没有 `audio` 键的场景必须一条 `AUDIO-PLAY` 都没有。

  E. **编辑器 UI 面 + Play 副本**
     `INS-AUDIO-HEADER` / `INS-AUDIO-PREVIEW` 矩形存在（"界面点得到"），
     控制台 `audio.preview` 走通（与 Inspector 的 ▶ 同一条链），
     以及 **Play 副本真的被消费** —— 挂组件 → Play → 出现
     `AUDIO-PLAY clip=audio/click.wav`。最后这条同时证明了"序列化接上了"
     和"组件不是死的"。

  F. **脚本内置函数**（`playSound` / `setBusVolume` / `stopAllSounds`）
     走的是与玩法脚本完全同一条 `ScriptHost` 注入链，不是测试专用捷径。

为什么不开真声音做断言：
    设备是共享资源、多一次运行就可能被别的进程占用；而且"听到什么"没法
    自动判定。所有能被判定的事情都在 A 组用离线渲染做完（`renderOffline`
    与实时音频走**同一条**代码路径）。

用法：
    python tools/verify_audio.py
    python tools/verify_audio.py --skip-long     # 跳过 10 分钟夹具（省磁盘）
"""

import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
from safe_fs import safe_remove

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Game.exe")
BIN_DIR = os.path.dirname(DEFAULT_EXE)
EDITOR_EXE = os.path.join(BIN_DIR, "Editor.exe")

SCENE_REL = "scenes/runtime_test/audio_scene.json"
LEGACY_SCENE_REL = "scenes/sandbox.json"
FIXTURE_DIR = os.path.join(ROOT, "assets", "scenes", "runtime_test")
SCRIPTS_DIR = os.path.join(ROOT, "tools")

LONG_NAME = "_long_10min.wav"

# 自检里**必须有而且必须绿**的条目。挑判据：每一条都对应路线图里
# 一个"踩过就会爆音 / 报不出来"的具体坑，而不是随便凑数。
REQUIRED_PASS = [
    # G14-1 解码
    "wav_24_vs_16_rms",              # 24-bit 按 3 字节打包（按 4 字节读会差 >50%）
    "wav_silence_exact_zero",
    # G14-2 混音
    "mixer_deterministic_memcmp",    # 两次渲染逐位相同 = 时基不来自 dt
    "mixer_volume_linear",
    "pool_steal_no_click",           # 抢断必须先淡出
    "clip_protection_64x_full",
    # G14-3/5 总线
    "bus_music_unaffected_by_sfx_mute",
    "bus_sfx_zero_silences",
    "master_mute_keeps_voices",      # mute ≠ 停播
    "vol_smoothing_no_step",         # 50 ms 平滑（否则拉链噪声）
    "duck_active_at_least_6db",
    "duck_release_back_to_unity",
    "duck_ignores_sfx_bus",
    # G14-4 3D
    "spatial_no_step_at_maxdist",    # 收尾斜坡（硬截断 = "啪"）
    "pan_center_power_unity",        # 等功率 pan（线性 pan 正中 -3 dB）
    "spatial_left_source_left_louder",
    "spatial_off_distance_independent",
    "spatial_stale_update_dropped",
    # G14-6 流式
    "stream_wrap_no_jump",
    "stream_underrun_silent_not_wrapped",
    "stream_not_cached",
    "stream_ring_is_three_seconds",
    "stream_resident_constant",
]

# 设备相关（C 组：本机有声卡才严格判定）
RE_WAVEFORM = re.compile(r"^\s*\[(PASS|FAIL)\]\s+(\S+)")

RE_TOTAL = re.compile(r"AUDIO-TOTAL pass=(\d+) fail=(\d+)")
RE_CONFIG = re.compile(
    r"AUDIO-CONFIG enabled=(\d) device=(\S+) null=(\d) master=([\d.]+) "
    r"music=([\d.]+) sfx=([\d.]+) voice=([\d.]+) mute=(\d) duck=([\d.]+) "
    r"maxvoices=(\d+) retry=(\d)")
RE_INIT = re.compile(
    r"AUDIO-INIT enabled=(\d) backend=(\S+) device=(\S+) sr=(\d+) ch=(\d+) "
    r"fallback=(\d)")
RE_PLAY = re.compile(
    r"AUDIO-PLAY id=(\d+),(\d+) clip=(\S+) bus=(\d+) loop=(\d) vol=([\d.]+) "
    r"stream=(\d)")
RE_DEVFAIL = re.compile(r"AUDIO-DEVFAIL backend=(\S+) reason=(\S+) fallback=(\d)")
RE_DEVWARN = re.compile(r"AUDIO-DEVWARN reason=device-not-found want='([^']*)'")
RE_PREVIEW_CHAIN = re.compile(r"AUDIO-PREVIEW clip=(\S+) vol=([\d.]+) ok=(\d)")
RE_PREVIEW_STATE = re.compile(r"AUDIO-PREVIEW path=(\S+) state=(\w+)")
RE_SUMMARY = re.compile(r"AUDIO-SUMMARY backend=(\S+) .* running=(\d)")
RE_RECT = r"=\((-?\d+),(-?\d+)\)-\((-?\d+),(-?\d+)\)"


# ------------------------------------------------------------------ 运行

def run(exe, args, frames=None, env_extra=None, timeout=180):
    cmd = [exe] + list(args)

    # ⚠⚠ **两个可执行目标的帧控方式不同**，混用会让脚本静默挂死：
    #      Game.exe   —— 认 argv `--frames N`（RuntimeConfig 解析）；
    #      Editor.exe —— argv 在 main() 里被 `(void)argc; (void)argv;`
    #                    **整个丢弃**，只认环境变量 `MYVK_FRAMES`。
    #    给编辑器传 `--frames` 不会有任何提示：它照常开窗、照常渲染、
    #    永不退出，脚本一路挂到超时。（G14-9 第一次跑就是这么被外部
    #    SIGTERM 掉的：三次编辑器启动各挂 240 s，而且因为是块缓冲，
    #    连一行输出都没留下。）
    is_editor = os.path.basename(exe).lower().startswith("editor")

    env = dict(os.environ)
    env.pop("MYVK_ASSET_DIR", None)
    # 宿主环境里残留的 MYVK_FRAMES 会让 Game 走"自动化"分支（默认关音频、
    # 不读存档），必须清掉 —— 我们只通过 frames 参数显式控制。
    env.pop("MYVK_FRAMES", None)
    # ⚠ 同理，先把所有音频相关的 env 清干净再注入：开发机上残留的
    #   `MYVK_RUNTIME_AUDIO=1` 会让"自动化默认关"那条断言假绿。
    for k in list(env):
        if k.startswith("MYVK_RUNTIME_AUDIO") or k.startswith("MYVK_EDITOR_AUDIO"):
            env.pop(k, None)
    if env_extra:
        env.update(env_extra)

    if frames is not None:
        if is_editor:
            env["MYVK_FRAMES"] = str(frames)
        else:
            cmd += ["--frames", str(frames)]

    try:
        p = subprocess.run(cmd, cwd=BIN_DIR, env=env, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True,
                           encoding="utf-8", errors="replace", timeout=timeout)
    except subprocess.TimeoutExpired as e:
        # ⚠ 超时时**保留已经吐出来的日志**：以前直接丢掉，于是"到底卡在
        #   哪一步"完全看不出来，只能靠猜。返回 -1 让调用方的 rc 判定失败。
        part = e.stdout or ""
        if isinstance(part, bytes):
            part = part.decode("utf-8", "replace")
        print("  ⚠ 超时 %ds（rc=-1）：%s" % (timeout, " ".join(cmd[1:]) or
                                          "(无参数)"))
        print("    已捕获 %d 行部分输出（下方断言会按 rc=-1 判红）"
              % len(part.splitlines()))
        return -1, part.splitlines()
    return p.returncode, p.stdout.splitlines()


def stage_fixtures():
    """把夹具抄进 build/bin/assets/scenes/runtime_test（运行时可写目录）。

    ⚠ 只换**被测的东西**：场景与脚本。资产根仍走默认 —— 指过去会连带换掉
    shader / texture 的解析根（`verify_runtime_m2.py` 同一条理由）。
    """
    dst = os.path.join(BIN_DIR, "assets", "scenes", "runtime_test")
    os.makedirs(dst, exist_ok=True)
    n = 0
    for name in os.listdir(FIXTURE_DIR):
        if name.endswith(".json") or name.endswith(".vks"):
            shutil.copy2(os.path.join(FIXTURE_DIR, name),
                         os.path.join(dst, name))
            n += 1
    return n


def gen_long_audio(path):
    """10 分钟 wav（不提交仓库：105 MB）。G14-6 的流式对照要它。"""
    if os.path.exists(path):
        return True
    out = os.path.join(BIN_DIR, "assets", "audio")
    py = sys.executable or "python"
    try:
        p = subprocess.run(
            [py, os.path.join(SCRIPTS_DIR, "gen_audio_assets.py"),
             "--out", os.path.join(BIN_DIR, "assets", "audio"),
             "--long", path],
            cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, encoding="utf-8", errors="replace", timeout=600)
    except subprocess.TimeoutExpired:
        return False
    if p.returncode != 0:
        print("  gen_audio_assets 失败：\n" + (p.stdout or "")[-800:])
    _ = out
    return os.path.exists(path)


def rect_ok(lines, anchor):
    """锚点存在且矩形非退化（宽高都 > 0）。

    ⚠ 只判"出现过"不够：被裁到 0 宽度也算出现过，而那正是"分区被滚出
    可视区 / 被折叠"的形态。
    """
    pat = re.compile(re.escape(anchor) + RE_RECT)
    for line in lines:
        m = pat.search(line)
        if not m:
            continue
        x0, y0, x1, y1 = (int(m.group(i)) for i in range(1, 5))
        if x1 - x0 >= 8 and y1 - y0 >= 4:
            return True
    return False


def f32(v):
    """把 JSON 里的十进制字面量转成它落进 float32 之后的值。

    ⚠ 必须这样比：`audio.volume = 0.35` 存档写出来是
    `0.3499999940395355`（float32）。直接用 `== 0.35` 判会假红。
    """
    return struct.unpack("f", struct.pack("f", float(v)))[0]


# ------------------------------------------------------------------ 主流程

def main():
    # ⚠ 输出重定向到管道时 Python 默认是块缓冲 —— 一旦某步卡住，外面
    #   "一个字都看不到"，排查只能靠猜（G14-9 第一次跑就吃了这个亏：
    #   日志全在缓冲区里，进程被 SIGTERM 后连一行都没留下）。强制行缓冲。
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except Exception:
        pass

    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--skip-long", action="store_true",
                    help="跳过 10 分钟长文件夹具（省 105 MB 磁盘）")
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    if not os.path.exists(exe):
        print("找不到可执行文件，请先构建：", exe)
        print("结果: FAIL: missing Game.exe")
        return 1
    if not os.path.exists(EDITOR_EXE):
        print("找不到 Editor.exe：", EDITOR_EXE)
        print("结果: FAIL: missing Editor.exe")
        return 1

    fails = []

    def check(name, ok, detail=""):
        print("%s  %s%s" % ("PASS" if ok else "FAIL", name,
                            ("  (" + detail + ")") if detail else ""))
        if not ok:
            fails.append(name)
        return ok

    def skip(name, why):
        # ⚠ 一律用 ASCII，避免终端编码问题（脚本要在各种机器上跑）
        print("SKIP  %s  (%s)" % (name, why))

    n_fixtures = stage_fixtures()
    print("staged %d fixture file(s) -> build/bin/assets/scenes/runtime_test"
          % n_fixtures)

    # ================= A. 无窗口自检 =================
    print()
    print("[A] 无窗口自检（解码 / 混音 / 总线 / 3D / 流式）")

    long_path = os.path.join(BIN_DIR, "assets", "audio", LONG_NAME)
    have_long = False
    if args.skip_long:
        skip("A-long", "--skip-long")
    else:
        have_long = gen_long_audio(long_path)
        if not have_long:
            check("A-long fixture 生成", False, long_path)

    rc, lines = run(exe, ["--selftest", "audio"])
    total = None
    for line in lines:
        m = RE_TOTAL.search(line)
        if m:
            total = (int(m.group(1)), int(m.group(2)))
    check("A1 --selftest audio 跑完", rc == 0 and total is not None,
          "rc=%d" % rc)
    if total:
        # ⚠ 用**下限**：新增断言不该让脚本变红；但"用例被删光了仍然
        #   pass=0 fail=0 → 绿"是必须堵住的洞。
        check("A2 断言条数 >= 110", total[0] >= 110, "pass=%d" % total[0])
        check("A3 零失败", total[1] == 0, "fail=%d" % total[1])

    passed = set()
    failed = set()
    for line in lines:
        m = RE_WAVEFORM.match(line)
        if m:
            (passed if m.group(1) == "PASS" else failed).add(m.group(2))

    missing = [n for n in REQUIRED_PASS if n not in passed]
    check("A4 关键断言逐条 PASS", not missing,
          "缺/红: %s" % (",".join(missing[:6]) if missing else "-"))

    if not args.skip_long:
        if have_long:
            # fixture 在 → 这几条必须**真的跑过**（SKIP 会把它们漏掉，
            # 而"流式只占 1 MB"正是靠这条对照撑住的）
            for n in ("stream_long_seconds", "stream_long_resident_small",
                      "inmem_long_resident_large",
                      "stream_long_vs_inmem_ratio"):
                check("A5 " + n, n in passed, "SKIP 或红")
        else:
            check("A5 长文件对照", False, "fixture 不可用")

    # ================= B. 配置解析 =================
    print()
    print("[B] 配置解析（env > game.json > 默认）")

    def cfg_of(env_extra, argv_extra=()):
        _, ls = run(exe, ["--selftest", "audio-config"] + list(argv_extra),
                    env_extra=env_extra)
        for line in ls:
            m = RE_CONFIG.search(line)
            if m:
                return {
                    "enabled": int(m.group(1)), "device": m.group(2),
                    "null": int(m.group(3)), "master": float(m.group(4)),
                    "music": float(m.group(5)), "sfx": float(m.group(6)),
                    "voice": float(m.group(7)), "mute": int(m.group(8)),
                    "duck": float(m.group(9)), "maxvoices": int(m.group(10)),
                    "retry": int(m.group(11)),
                }
        return None

    base = cfg_of({})
    check("B1 自动化默认关（--selftest 下 enabled=0）",
          base is not None and base["enabled"] == 0,
          str(base and base["enabled"]))
    check("B2 game.json 的 music=0.70 读进来了",
          base is not None and abs(base["music"] - 0.70) < 1e-6,
          str(base and base["music"]))

    b3 = cfg_of({"MYVK_RUNTIME_AUDIO": "1"})
    check("B3 MYVK_RUNTIME_AUDIO=1 → enabled=1",
          b3 is not None and b3["enabled"] == 1, str(b3 and b3["enabled"]))

    b4 = cfg_of({"MYVK_RUNTIME_AUDIO_MASTER": "0.5",
                 "MYVK_RUNTIME_AUDIO_SFX": "0"})
    check("B4 MASTER=0.5 / SFX=0 生效",
          b4 is not None and abs(b4["master"] - 0.5) < 1e-6
          and abs(b4["sfx"]) < 1e-6,
          str(b4 and (b4["master"], b4["sfx"])))

    b5 = cfg_of({"MYVK_RUNTIME_AUDIO_DUCK": "0", "MYVK_RUNTIME_AUDIO_MUTE": "1",
                 "MYVK_RUNTIME_AUDIO_MAXVOICES": "8"})
    check("B5 duck/mute/maxvoices 生效",
          b5 is not None and abs(b5["duck"]) < 1e-6 and b5["mute"] == 1
          and b5["maxvoices"] == 8,
          str(b5 and (b5["duck"], b5["mute"], b5["maxvoices"])))

    # 越界必须被夹住（负数会让相位反相，听起来像"声音从背后出来"）
    b6 = cfg_of({"MYVK_RUNTIME_AUDIO_MASTER": "-3",
                 "MYVK_RUNTIME_AUDIO_MUSIC": "99",
                 "MYVK_RUNTIME_AUDIO_MAXVOICES": "999"})
    check("B6 音量/池容量被夹到合法区间",
          b6 is not None and b6["master"] == 0.0 and b6["music"] == 1.0
          and b6["maxvoices"] == 64,
          str(b6 and (b6["master"], b6["music"], b6["maxvoices"])))

    # `_NULL=1` 必须隐含"我要音频"，否则它是静默无效的
    b7 = cfg_of({"MYVK_RUNTIME_AUDIO_NULL": "1"})
    check("B7 AUDIO_NULL=1 隐含 enabled=1",
          b7 is not None and b7["enabled"] == 1 and b7["null"] == 1,
          str(b7 and (b7["enabled"], b7["null"])))

    # ================= C. 设备后端 =================
    print()
    print("[C] 设备后端（null / wasapi / 假名 / __none__）")

    rc, lines = run(exe, [], frames=40,
                    env_extra={"MYVK_RUNTIME_AUDIO": "1",
                               "MYVK_RUNTIME_AUDIO_NULL": "1"})
    init = None
    for line in lines:
        m = RE_INIT.search(line)
        if m:
            init = m
    check("C1 NullBackend 打开（链路通、不需要声卡）",
          rc == 0 and init is not None and init.group(2) == "null"
          and init.group(1) == "1" and init.group(6) == "0",
          init.group(0)[:60] if init else "no AUDIO-INIT")
    summ = RE_SUMMARY.search("\n".join(lines))
    check("C1b AUDIO-SUMMARY running=1", bool(summ) and summ.group(2) == "1",
          summ.group(0)[:48] if summ else "-")

    real_ok = False
    rc, lines = run(exe, [], frames=40,
                    env_extra={"MYVK_RUNTIME_AUDIO": "1"})
    init = None
    devfail = None
    for line in lines:
        m = RE_INIT.search(line)
        if m:
            init = m
        m2 = RE_DEVFAIL.search(line)
        if m2:
            devfail = m2
    if init is not None and init.group(2) == "wasapi" and init.group(6) == "0":
        real_ok = True
        check("C2 真设备打开（wasapi / 48k / 立体声 / 未回落）",
              rc == 0 and int(init.group(4)) >= 8000 and init.group(5) == "2",
              init.group(0)[:72])
    elif devfail is not None:
        # 无声卡机器：只要"失败得干净"就算过，但必须留痕
        check("C2 无声卡时干净失败（不是崩）", rc == 0,
              devfail.group(0)[:64])
        skip("C2b 真设备回退判定", "no-sound-card")
    else:
        check("C2 真设备打开", False, "既没有 AUDIO-INIT 也没有 AUDIO-DEVFAIL")

    if real_ok:
        rc, lines = run(exe, [], frames=40,
                        env_extra={"MYVK_RUNTIME_AUDIO": "1",
                                   "MYVK_RUNTIME_AUDIO_DEVICE": "NoSuchDevice_Xyz"})
        fb = None
        warn = None
        for line in lines:
            m = RE_INIT.search(line)
            if m:
                fb = int(m.group(6))
            m2 = RE_DEVWARN.search(line)
            if m2:
                warn = m2
        check("C3 假设备名 → fallback=1 且 AUDIO-DEVWARN 留痕",
              rc == 0 and fb == 1 and warn is not None,
              "fallback=%s warn=%s" % (fb, bool(warn)))
    else:
        skip("C3 假设备名回落", "no-sound-card")

    rc, lines = run(exe, [], frames=40,
                    env_extra={"MYVK_RUNTIME_AUDIO": "1",
                               "MYVK_RUNTIME_AUDIO_DEVICE": "__none__"})
    devfail = None
    for line in lines:
        m = RE_DEVFAIL.search(line)
        if m:
            devfail = m
    check("C4 __none__ 哨兵 → DEVFAIL 且不回落、进程正常退出",
          rc == 0 and devfail is not None and devfail.group(3) == "0",
          devfail.group(0)[:64] if devfail else "no AUDIO-DEVFAIL")

    # 关掉音频：一条 AUDIO-PLAY 都不该有
    rc, lines = run(exe, ["--scene", SCENE_REL], frames=40,
                    env_extra={"MYVK_RUNTIME_AUDIO": "0"})
    check("C5 enabled=0 时零 AUDIO-PLAY",
          rc == 0 and not any(RE_PLAY.search(l) for l in lines), "")

    # ================= D. 序列化往返 =================
    print()
    print("[D] 场景序列化往返（含 float32 精度 + 老场景对照）")

    dump = os.path.join(BIN_DIR, "assets", "scenes", "_audio_dump.json")
    if os.path.exists(dump):
        safe_remove(dump)
    rc, lines = run(exe, ["--scene", SCENE_REL, "--dump-scene", dump],
                    frames=5)
    check("D1 --dump-scene 写出成功",
          rc == 0 and any("GAME-SCENE dumped path=" in l for l in lines), "")

    src = os.path.join(BIN_DIR, "assets", SCENE_REL.replace("/", os.sep))
    got = None
    if os.path.exists(dump):
        with open(dump, encoding="utf-8") as f:
            got = json.load(f)

    def find_entity(doc, name):
        for e in (doc or {}).get("entities", []):
            if e.get("name") == name:
                return e
        return None

    src_doc = None
    with open(src, encoding="utf-8") as f:
        src_doc = json.load(f)

    a_src = (find_entity(src_doc, "Speaker") or {}).get("audio")
    a_got = (find_entity(got, "Speaker") or {}).get("audio")
    check("D2 Speaker 的 audio 块还在", a_got is not None, str(a_got)[:40])
    if a_src and a_got:
        # 逐字段（而不是整 dict 相等）：少写一个键、多写一个键都要能定位
        bad = []
        for k, v in a_src.items():
            if k not in a_got:
                bad.append(k + ":missing")
                continue
            # float32 来回：0.35 → 0.3499999940395355
            if isinstance(v, float):
                if abs(f32(v) - float(a_got[k])) > 1e-9:
                    bad.append("%s:%r!=%r" % (k, v, a_got[k]))
            elif a_got[k] != v:
                bad.append("%s:%r!=%r" % (k, v, a_got[k]))
        check("D3 audio 逐字段相等（11 个字段）", not bad,
              ",".join(bad[:5]) if bad else "11/11")

    check("D4 audioListener 写回",
          (find_entity(got, "Speaker") or {}).get("audioListener") is True, "")

    # 老场景：没有 audio 键 → 加载不报错、且一条 AUDIO-PLAY 都没有
    rc, lines = run(exe, ["--scene", LEGACY_SCENE_REL], frames=30,
                    env_extra={"MYVK_RUNTIME_AUDIO": "1",
                               "MYVK_RUNTIME_AUDIO_NULL": "1"})
    err = [l for l in lines if "[ERROR" in l or "VUID" in l]
    check("D5 老场景（无 audio 键）：无错误、零 AUDIO-PLAY",
          rc == 0 and not err and not any(RE_PLAY.search(l) for l in lines),
          ("errors=%d" % len(err)) if err else "clean")

    # ================= E. 编辑器 UI 面 + Play 副本 =================
    print()
    print("[E] 编辑器（Inspector 段 / 试听链 / Play 副本）")

    ed_env = {
        "MYVK_LOG_RECTS": "1",
        "MYVK_NO_WINDOW_SAVE": "1",
        "MYVK_EDITOR_NO_HUD": "1",
        "MYVK_EDITOR_SELECT": "Speaker",
        "MYVK_EDITOR_INSP_TAB": "tool",
        "MYVK_EDITOR_SCENE": os.path.join(BIN_DIR, "assets",
                                          SCENE_REL.replace("/", os.sep)),
    }
    rc1, lines = run(EDITOR_EXE, [], frames=70, env_extra=ed_env)
    check("E1 INS-AUDIO-HEADER 矩形存在且非退化",
          rc1 == 0 and rect_ok(lines, "INS-AUDIO-HEADER"), "rc=%d" % rc1)
    check("E2 INS-AUDIO-PREVIEW（▶ 试听按钮）矩形存在",
          rc1 == 0 and rect_ok(lines, "INS-AUDIO-PREVIEW"), "")
    check("E3 EDITOR-AUDIO 打出了设备信息",
          rc1 == 0 and any("EDITOR-AUDIO enabled=" in l for l in lines), "")

    # 试听链：控制台命令走的是与 Inspector ▶ **完全相同**的那条路
    ed_env2 = dict(ed_env)
    ed_env2["MYVK_EDITOR_CONSOLE_CMD"] = "audio.preview audio/click.wav"
    ed_env2["MYVK_EDITOR_AUDIO"] = "0"   # 不抢共享设备，链路由 Null 验证不够：见下
    # ⚠ 这里**必须真开**（不是 Null）：要证明的是"Context 请求 → EditorApp →
    #   AudioSystem::playOneShot"整条链，Null 后端一样能走通，但真设备多一层
    #   WASAPI。而 E 组本来就只在有声卡的机器上有意义 —— 所以下面分开判。
    ed_env2["MYVK_EDITOR_AUDIO"] = "1"
    rc2, lines = run(EDITOR_EXE, [], frames=70, env_extra=ed_env2)
    chain = None
    for line in lines:
        m = RE_PREVIEW_CHAIN.search(line)
        if m:
            chain = m
    played = any(RE_PLAY.search(l) and "click.wav" in l for l in lines)
    check("E4 audio.preview → AUDIO-PREVIEW ok=1 且真的起了一个声部",
          rc2 == 0 and chain is not None and chain.group(3) == "1" and played,
          (chain.group(0)[:52] if chain else "no AUDIO-PREVIEW"))

    # Play 副本：组件必须活过 sceneToJson → sceneFromJson，并且被消费
    ed_env3 = dict(ed_env)
    ed_env3["MYVK_EDITOR_PLAY"] = "1"
    ed_env3["MYVK_EDITOR_AUDIO"] = "1"
    rc3, lines = run(EDITOR_EXE, [], frames=80, env_extra=ed_env3)
    play = [l for l in lines if RE_PLAY.search(l) and "click.wav" in l]
    check("E5 Play 副本里的 AudioSource 被消费（AUDIO-PLAY click.wav）",
          rc3 == 0 and any("Editor: PLAY in viewport" in l for l in lines)
          and len(play) >= 1,
          "rc=%d plays=%d" % (rc3, len(play)))

    # ================= F. 脚本内置函数 =================
    print()
    print("[F] 脚本内置音频函数（playSound / setBusVolume / stopAllSounds）")

    rc, lines = run(exe, ["--scene", SCENE_REL], frames=40,
                    env_extra={"MYVK_RUNTIME_AUDIO": "1",
                               "MYVK_RUNTIME_AUDIO_NULL": "1"})
    script_play = [l for l in lines
                   if RE_PLAY.search(l) and "tone_1k_mono.wav" in l]
    check("F1 playSound → AUDIO-PLAY clip=audio/tone_1k_mono.wav",
          rc == 0 and len(script_play) >= 1, "plays=%d" % len(script_play))

    bus_bad = any("setBusVolume" in l and "musik" in l for l in lines)
    check("F2 拼错的总线名必须留痕（不许静默忽略）", bus_bad, "")

    bus_good_bad = [l for l in lines
                    if "setBusVolume" in l and "'music'" in l]
    check("F3 合法的 music 总线不产生警告", not bus_good_bad,
          "warns=%d" % len(bus_good_bad))

    # 组件的字段真的被用上了（bus=3 / vol=0.35 来自场景，不是默认值）
    comp = [l for l in lines if RE_PLAY.search(l) and "click.wav" in l]
    ok_fields = False
    if comp:
        m = RE_PLAY.search(comp[0])
        ok_fields = m.group(4) == "3" and abs(float(m.group(6)) - 0.35) < 0.02
    check("F4 组件字段生效（bus=3 Voice / vol=0.35）", ok_fields,
          comp[0][:64] if comp else "no click.wav play")

    # ================= 收尾 =================
    print()
    if fails:
        print("结果: FAIL: %d 项未通过 -> %s" % (len(fails), ", ".join(fails)))
        return 1
    print("结果: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
