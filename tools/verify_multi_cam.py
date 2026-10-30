"""多机位 marker 切镜验收（运镜体系 §七）。

    python tools/verify_multi_cam.py [--no-gui]

三组：

    A 组（内核，无窗口）
        `Game.exe --selftest sequence`   —— G 组：marker 的解析 / 排序 /
            区间语义（**早于第一个 marker 返回 −1**）/ 写回往返 / 播放器输出。
        `Game.exe --selftest cameramove` —— G 组：导演覆盖**压过 priority**、
            目标消失后回落、`blendCut` 真的插值、`dt==0` 不推进。

    B 组（编辑器，注入脚本）
        `+ Shot` 背后的那批 `act*` 真的能改数据、能存回、能读回；
        并且**不许**把 marker 绑到场景里不存在的 guid 上（要拒，且文件不变）。

    C 组（运行时，真画面）
        一个场景里放两台相机（低优先级 + 高优先级），时间轴用 marker
        强制先用**低**优先级那台 —— 于是"`CAM-CUT` 出现"这件事本身
        就证明了覆盖压过 priority，不需要额外造条件。

==============================================================================
三条刻意的取舍
==============================================================================
① **B 组不用鼠标点按钮**（同 `verify_timeline_editor.py`）：注入脚本调的
   `act*` 正是按钮调的那一批函数。但锚点仍然验一遍 —— 它证明面板真的
   画在屏幕上（"界面点不到 = 没交付"）。

② **C 组把 marker 放在 0.05s**，不放在 1.0s。`MYVK_FRAMES` 是"跑满 N 帧"，
   帧率随机器浮动，写死 1.0s 会让这条用例在快机器上"没跑到那个时间点"
   而红 —— 那种红是夹具的问题，不是功能的问题。

③ **B 组的标签只用 ASCII**。`MYVK_EDITOR_TLED_SCRIPT` 走 `std::getenv`，
   Windows 的窄环境块按 ANSI 代码页转换，中文进去会变成 GBK 字节 ——
   断言测到的就变成"编码没配好"了。中文是否原样往返由 A 组
   `seq_shot_label_utf8` 钉（那条不经过环境块）。
"""

import json
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import launcher_kit as kit                                        # noqa: E402
from safe_fs import safe_remove                                    # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
GAME = os.path.join(BIN, "Game.exe")
EDITOR = os.path.join(BIN, "Editor.exe")

ASSETS = os.path.join(BIN, "assets")
SRC_ASSET = "sequences/opening.seq.json"        # 5 帧 4 事件，B 组的底子

FAILS = []
PASSES = []


def check(what, ok, detail=""):
    if ok:
        PASSES.append(what)
        kit.log("  [PASS] %s%s" % (what, ("  — " + detail) if detail else ""))
    else:
        FAILS.append(what)
        kit.log("  [FAIL] %s%s" % (what, ("  — " + detail) if detail else ""))
    return ok


def last(rx, text):
    hits = [m for m in (rx.search(l) for l in text.splitlines()) if m]
    return hits[-1] if hits else None


# ================================================================ A 组

SEQ_TOTAL = re.compile(r"SEQ-TOTAL pass=(\d+) fail=(\d+)")
CAM_TOTAL = re.compile(r"CAMMOVE-TOTAL pass=(\d+) fail=(\d+)")


def _selftest(name, total_rx):
    proc = subprocess.run([GAME, "--selftest", name], cwd=BIN,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, encoding="utf-8", errors="replace",
                          timeout=180)
    return proc.stdout or ""


def run_kernel():
    kit.log("---- A 组：内核自检 ----")

    # ---- A1~A3 时间轴的 shots 组 ----
    out = _selftest("sequence", SEQ_TOTAL)
    m = last(SEQ_TOTAL, out)
    if not check("A0 时间轴自检跑出 SEQ-TOTAL", m is not None):
        return
    passed, failed = int(m.group(1)), int(m.group(2))
    check("A1 时间轴自检无失败项", failed == 0, "pass=%d fail=%d" % (passed, failed))
    check("A1b 条目数 >= 97（含 24 条 shots 组）", passed >= 97, "pass=%d" % passed)

    # 只数总数会让"某一组整段没跑"看不出来 —— 点名代表条目。
    must = [
        "seq_shot_parse_fields",
        "seq_shot_absent_not_written",     # 老资产不许长出 "shots": []
        "seq_shot_dur_derived",            # duration 要把 marker 算进去
        "seq_shot_idx_before_first",       # 早于第一个 marker → −1
        "seq_shot_idx_at_boundary",        # 半开区间：边界归**后**一段
        "seq_shot_bad_guid_warns",         # 短 guid 要警告，不是静默
        "seq_shot_roundtrip",
        "seq_shot_player_blend0",
        "seq_shot_none_idx",               # 老资产：一个 marker 都没有
    ]
    for name in must:
        mm = last(re.compile(r"SEQ-TEST %s .*result=(\w+)" % re.escape(name)), out)
        check("A2 %s 出现且 PASS" % name, mm is not None and mm.group(1) == "PASS",
              "none" if mm is None else mm.group(1))

    # ---- A4~A6 相机侧：导演覆盖 ----
    out = _selftest("cameramove", CAM_TOTAL)
    m = last(CAM_TOTAL, out)
    if not check("A4 运镜自检跑出 CAMMOVE-TOTAL", m is not None):
        return
    passed, failed = int(m.group(1)), int(m.group(2))
    check("A5 运镜自检无失败项", failed == 0, "pass=%d fail=%d" % (passed, failed))
    check("A5b 条目数 >= 81（含 12 条导演覆盖组）", passed >= 81, "pass=%d" % passed)

    must = [
        "G1 无导演时按 priority 选中 B",
        "G2 导演覆盖压过 priority（选 A）",
        "G3 释放覆盖后回到 B",
        "G4 目标被删后回落到 priority（B）",
        "G4 导演 guid 被清零",
        "G5 blend 走到一半时 x 在中点",
        "G6 dt==0 时 blend 不推进（还在 B）",
    ]
    for name in must:
        mm = last(re.compile(r"CAMMOVE-TEST (\w+) %s" % re.escape(name)), out)
        check("A6 %s" % name, mm is not None and mm.group(1) == "pass",
              "none" if mm is None else mm.group(1))


# ================================================================ B 组

OPEN_RE = re.compile(r"TLED-OPEN path=(\S+) ok=(\d+) keys=(\d+) events=(\d+) "
                     r"dur=([\d.]+) readOnly=(\d+) shots=(\d+)")
ADDSHOT_RE = re.compile(r"TLED-ADDSHOT t=([\d.]+) cam=(\S+) shots=(\d+) sel=(\d+)")
BLEND_RE = re.compile(r"TLED-SETSHOTBLEND t=([\d.]+) blend=([\d.]+)")
CAM_RE = re.compile(r"TLED-SETSHOTCAM t=([\d.]+) cam=(\S+)")
SAVE_RE = re.compile(r"SEQ-SAVE path=(\S+) full=(\S+) keys=(\d+) events=(\d+) "
                     r"shots=(\d+)")


def run_editor(extra_env, wait_s=120.0):
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_GRID"] = "0"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_FRAMES"] = "150"
    for k, v in extra_env.items():
        if v is not None:
            env[k] = v
    try:
        subprocess.run(["taskkill", "/F", "/IM", "Editor.exe"],
                       capture_output=True, timeout=10)
    except Exception:                                            # noqa: BLE001
        pass
    proc = subprocess.Popen([EDITOR], cwd=BIN, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        out, _ = proc.communicate(timeout=wait_s)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, _ = proc.communicate(timeout=20)
    return (out or b"").decode("utf-8", "replace").replace("\x00", "")


def run_editor_group(tmp):
    kit.log("---- B 组：编辑器（+ Shot / 绑定 / 存盘往返）----")
    saved = os.path.join(tmp, "mc_saved.seq.json").replace("\\", "/")

    # ---- B1 两个 marker + 字段 + 存到临时文件 ----
    script = ("head:2.0;addshot;setshotlabel:A cam;setshotblend:0.5;"
              "head:5.0;addshot;sel:shots:0;setshotlabel:A cam;setshotcam:0;"
              "saveas:" + saved)
    text = run_editor({"MYVK_EDITOR_TLED": SRC_ASSET,
                       "MYVK_EDITOR_TLED_SCRIPT": script})

    mo = last(OPEN_RE, text)
    check("B1 打开资产（TLED-OPEN ok=1，shots=0）",
          mo is not None and mo.group(2) == "1" and mo.group(7) == "0",
          "none" if mo is None else mo.group(0)[:100])

    adds = [m for m in (ADDSHOT_RE.search(l) for l in text.splitlines()) if m]
    check("B2 两次 + Shot 都落日志（shots 1 → 2）",
          len(adds) == 2 and adds[0].group(3) == "1" and adds[1].group(3) == "2",
          "n=%d" % len(adds))
    # 没选中任何相机 → 绑 0（回落关键帧轨）。这是**合法值**，不该被拒。
    check("B2b 没选相机时 marker 绑 0（cam=(keys)）",
          bool(adds) and adds[0].group(2) == "(keys)",
          "cam=%s" % (adds[0].group(2) if adds else "-"))

    mb = last(BLEND_RE, text)
    check("B3 setshotblend 生效（blend=0.50）",
          mb is not None and abs(float(mb.group(2)) - 0.5) < 1e-6,
          "none" if mb is None else mb.group(0))
    mc = last(CAM_RE, text)
    check("B3b setshotcam:0 = 显式回落到关键帧轨（合法，不是拒绝）",
          mc is not None and mc.group(2) == "(keys)",
          "none" if mc is None else mc.group(0))

    ms = last(SAVE_RE, text)
    check("B4 存盘写出 shots=2", ms is not None and ms.group(5) == "2",
          "none" if ms is None else ms.group(0)[:110])

    # ---- B5 文件内容：t / blend / label 对，且**没有** camera 键 ----
    data = {}
    try:
        with open(saved, encoding="utf-8") as f:
            data = json.load(f)
    except Exception as e:                                       # noqa: BLE001
        check("B5 临时文件能读回", False, str(e))
        return
    shots = data.get("shots") or []
    check("B5 文件里 shots 有两条", len(shots) == 2, "n=%d" % len(shots))
    if len(shots) == 2:
        check("B5b 第一条 t=2.0 blend=0.5 label='A cam'",
              abs(shots[0].get("t", -1) - 2.0) < 1e-3 and
              abs(shots[0].get("blend", 0) - 0.5) < 1e-6 and
              shots[0].get("label") == "A cam",
              json.dumps(shots[0], ensure_ascii=False))
        check("B5c 第二条 t=5.0（升序）", abs(shots[1].get("t", -1) - 5.0) < 1e-3,
              json.dumps(shots[1], ensure_ascii=False))
        # 绑 0 时**不写** camera 键（省略与写 "" 是一回事，但 diff 少一行噪声）
        check("B5d 绑 0 时文件里没有 camera 键",
              all("camera" not in s for s in shots),
              "keys=%s" % [sorted(s.keys()) for s in shots])

    # ---- B6 拒绝绑定场景里不存在的 guid（否则文件里会留死引用）----
    text2 = run_editor({"MYVK_EDITOR_TLED": saved,
                        "MYVK_EDITOR_TLED_SCRIPT":
                            "sel:shots:0;setshotcam:ffff0000ffff0001;save"})
    check("B6 绑不存在的 guid 被拒（TLED-SCRIPT-FAIL）",
          "TLED-SCRIPT-FAIL" in text2 and "TLED-REJECT" in text2,
          "fail=%d" % text2.count("TLED-SCRIPT-FAIL"))
    try:
        with open(saved, encoding="utf-8") as f:
            again = json.load(f)
        same = all("camera" not in s for s in (again.get("shots") or []))
    except Exception:                                            # noqa: BLE001
        same = False
    check("B6b 被拒之后文件里依然没有 camera 键", same)

    # ---- B7 重开：往返读回 ----
    text3 = run_editor({"MYVK_EDITOR_TLED": saved})
    mo3 = last(OPEN_RE, text3)
    check("B7 重开后 shots=2（存盘内容读得回来）",
          mo3 is not None and mo3.group(7) == "2" and mo3.group(2) == "1",
          "none" if mo3 is None else mo3.group(0)[:110])

    # ---- B8 锚点：面板真的把第三行画出来了 ----
    got = ["TLED-ADDSHOT", "TLED-TRACKS", "TLED-PROP"]
    missing = [a for a in got if a not in text3]
    check("B8 面板锚点齐（+Shot 按钮 / 轨道区 / 属性栏）", not missing,
          "缺=%s" % (missing or "-"))


# ================================================================ C 组

SCENE_REL = "scenes/_mc_cut.json"
SCENE_ABS = os.path.join(ASSETS, "scenes", "_mc_cut.json")
SEQ_REL = "sequences/_mc_cut.seq.json"
SEQ_ABS = os.path.join(ASSETS, "sequences", "_mc_cut.seq.json")

GUID_LOW = "c2a0000000000001"     # 低优先级：正常情况永远轮不到它
GUID_HIGH = "c2a0000000000002"    # 高优先级：没有 marker 时它是胜者


def write_fixtures():
    os.makedirs(os.path.dirname(SEQ_ABS), exist_ok=True)
    os.makedirs(os.path.dirname(SCENE_ABS), exist_ok=True)

    # marker 放在 0.05s：见文件头取舍 ②
    seq = {
        "name": "mc_cut",
        "duration": 2.0,
        "camera": [
            {"t": 0.0, "pos": [0.0, 0.0, 3.0], "yaw": 0.0, "pitch": 0.0, "fov": 45.0},
            {"t": 2.0, "pos": [0.0, 0.0, 3.0], "yaw": 0.0, "pitch": 0.0, "fov": 45.0},
        ],
        "shots": [
            {"t": 0.0, "camera": GUID_LOW, "label": "low"},
            {"t": 0.05, "camera": GUID_HIGH, "blend": 0.2, "label": "high"},
        ],
    }
    with open(SEQ_ABS, "w", encoding="utf-8") as f:
        json.dump(seq, f, ensure_ascii=False, indent=2)

    def cam(guid, name, pos, prio):
        return {"guid": guid, "name": name, "parent": -1,
                "transform": {"position": pos,
                              "rotation": [0.0, 0.0, 0.0],
                              "scale": [1.0, 1.0, 1.0]},
                "visible": True, "castShadow": False,
                "camera": {"fov": 45.0, "priority": prio, "enabled": True}}

    doc = {
        "version": 1,
        "generator": "verify_multi_cam.py",
        "camera": {"target": [0.0, 0.0, 1.5], "yaw": 0.0, "pitch": 0.25,
                   "distance": 8.0, "fov": 45.0},
        "light": {"direction": [0.42, 0.72, -0.55], "color": [1.0, 0.96, 0.9],
                  "intensity": 3.0, "castsShadow": True, "ambientScale": 1.0},
        "physics": {"enabled": False, "groundPlane": True,
                    "fixedTimeStep": 0.0166666675, "maxSubSteps": 4,
                    "allowSleep": True},
        "entities": [
            cam(GUID_LOW, "CamLow", [-6.0, 0.0, 3.0], 0),
            cam(GUID_HIGH, "CamHigh", [6.0, 0.0, 3.0], 10),
            # 导演物体：只挂时间轴，不挂相机
            {"guid": "c2a0000000000010", "name": "Director", "parent": -1,
             "transform": {"position": [0.0, 0.0, 0.0],
                           "rotation": [0.0, 0.0, 0.0],
                           "scale": [1.0, 1.0, 1.0]},
             "visible": True, "castShadow": False,
             "sequencer": {"path": SEQ_REL, "enabled": True, "playing": True,
                           "loop": False, "startTime": 0.0, "speed": 1.0,
                           "priority": 0}},
        ],
    }
    with open(SCENE_ABS, "w", encoding="utf-8") as f:
        json.dump(doc, f, ensure_ascii=False, indent=2)


SEQSHOT_RE = re.compile(r"SEQ-SHOT seq=(\S+) t=([\d.]+) i=(-?\d+)/(\d+) "
                        r"cam=(\S+) blend=([\d.]+)")
CUT_RE = re.compile(r"CAM-CUT name=(\S+) guid=(\S+) priority=(-?\d+)")
CUTRT_RE = re.compile(r"CAM-CUT-RT shot=(-?\d+) cam=(\S+) blend=([\d.]+)")


def run_runtime():
    kit.log("---- C 组：运行时切镜 ----")
    write_fixtures()
    env = kit.clean_env({
        "MYVK_NO_WINDOW_SAVE": "1",
        "MYVK_GRID": "0",
        "MYVK_FRAMES": "300",
    })
    proc = subprocess.run([GAME, "--scene", SCENE_REL], cwd=BIN, env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, encoding="utf-8", errors="replace",
                          timeout=240)
    out = proc.stdout or ""
    try:
        if os.path.exists(SCENE_ABS):
            safe_remove(SCENE_ABS)
    except Exception:                                            # noqa: BLE001
        pass

    shots = [m for m in (SEQSHOT_RE.search(l) for l in out.splitlines()) if m]
    check("C1 时间轴报出了机位切换（SEQ-SHOT ≥ 2 条）", len(shots) >= 2,
          "n=%d" % len(shots))

    # ⚠ 关键判据：第一段绑的是**低优先级**那台。
    #   如果没有覆盖机制，画面永远停在高优先级那台上 —— 而日志里
    #   `SEQ-SHOT` 照样会打（它只说"该用哪台"，还没落到相机上）。
    #   所以必须再看到 `CAM-CUT name=CamLow`，才算覆盖真的生效了。
    cuts = [m for m in (CUT_RE.search(l) for l in out.splitlines()) if m]
    names = [c.group(1) for c in cuts]
    check("C2 导演覆盖压过 priority（CAM-CUT name=CamLow 先出现）",
          "CamLow" in names, "cuts=%s" % (names or "-"))
    check("C3 第一段用的是低优先级那台（guid 对）",
          any(c.group(2) == GUID_LOW for c in cuts),
          "guids=%s" % ([c.group(2) for c in cuts] or "-"))
    check("C4 第二段切到高优先级那台", "CamHigh" in names,
          "cuts=%s" % (names or "-"))

    rts = [m for m in (CUTRT_RE.search(l) for l in out.splitlines()) if m]
    check("C5 运行时确认切镜落到相机上（CAM-CUT-RT ≥ 2 条）", len(rts) >= 2,
          "n=%d" % len(rts))
    if len(rts) >= 2:
        # 第一段是硬切（blend 缺省 0），第二段带 0.2s 混合
        check("C5b 第一段 blend=0.00（硬切）", abs(float(rts[0].group(3))) < 1e-6,
              "blend=%s" % rts[0].group(3))
        check("C5c 第二段 blend=0.20（文件里写的）",
              abs(float(rts[-1].group(3)) - 0.2) < 1e-6,
              "blend=%s" % rts[-1].group(3))

    bad = [x for x in out.splitlines() if "VUID" in x or "VALIDATION" in x]
    check("C6 无 VUID / VALIDATION 报错", not bad,
          "首条=%s" % (bad[0][:100] if bad else "-"))


def main():
    no_gui = "--no-gui" in sys.argv
    tmp = tempfile.mkdtemp(prefix="venn_mc_")
    try:
        run_kernel()
        if not no_gui:
            run_editor_group(tmp)
            run_runtime()
    finally:
        try:
            import shutil
            shutil.rmtree(tmp, ignore_errors=True)
        except Exception:                                        # noqa: BLE001
            pass

    kit.log("")
    kit.log("通过 %d 项，失败 %d 项" % (len(PASSES), len(FAILS)))
    if FAILS:
        kit.log("结果: FAIL: " + "; ".join(FAILS))
        return 1
    kit.log("结果: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
