"""B3 根运动 —— 端到端验收（编辑器里真跑，读日志锚点 + 落盘往返 + UI）

==============================================================================
⚠ 先说清楚这个脚本**不验什么**，以及为什么
==============================================================================
手上的资产 —— `anime_girl.fbx` 的 `Walk` / `Idle` / `Run` / `RigWalk` ——
**四段全是原地动画**（根骨一步都不动）。实测方法：把 `rootMotion` 打开、
`playing` 打开，四段各跑一遍，`ANIM-ROOTMOTION ... dist=` 一直是 0.0000。

所以"角色真的往前走了 1 米"这件事**在真实资产上没法验**。位移的语义
（增量怎么算 / 钳回基准 / 跨圈丢弃 / Y-up→Z-up / 水平只提 xz）全部由
数值自检的 `rm_*` 27 条断言覆盖，其中三条做过否定测试：

  · 去掉跨圈丢弃     → `rm_wrap_discard` 红（两圈只走了 0.98 m）
  · 不做 Y-up→Z-up   → `rm_yup_to_zup` 红（z=0, y=1，正是"朝天上飞"）
  · 改成"减每帧增量" → `rm_in_place` 红（模型自己走了 0.966 m = 双倍位移）

本脚本只验**接线**（"引擎里那条路径真的被走到、真的读了配置"）：

  ① 自动选骨：`ANIM-ROOTMOTION cfg=1 n=0 bone=<真名>` —— `n=0` 是这份
     资产本来的样子，而 `bone=` 必须是一根**真实存在**的骨（不是 `(none)`）。
     ⚠ `cfg>0` 但 `n=0` 也要打日志，这是刻意的：这正是"开了但不走"的
       最常见形态，没有这条日志的话用户只能靠猜。
  ② 骨名填错：`ANIM-ROOTMOTION-BONE-MISS` 一条 + `bone=(none)`。
     **不能**静默退化成"用根骨"—— 那会让填错名字的人以为填对了。
  ③ 落盘往返：`scene.save` 之后 `rootMotion` / `rootMotionBone` 还在，
     而 `rootPrevWorld` / `rootBasePos` / `rootDelta` 一个都没有
     （运行时字段落盘会让"下次读档第一帧"凭空吃一段位移）。
  ④ UI 面：Root motion 下拉与根骨输入框真的画出来了
     （`INS-ANIM-ROOTMOTION` / `INS-ANIM-ROOTBONE`）。

==============================================================================
⚠ 取景/场景沿用 `verify_anim_layers` 烤出来的模板
==============================================================================
那份模板是 `MYVK_EDITOR_IMPORT` + `scene.save` 烤出来的（Ground + 根 +
7 个 submesh 子实体）。**手写 `skinned` 实体在视口里一个像素都不画**
（渲染的是每个 submesh 的子实体），这是 B1 那一轮花了 3/4 时间踩的坑。
本脚本不截图（没有像素判据），但照样用它 —— 省一次导入，而且保证
"验的就是玩家真会看到的那份场景"。
==============================================================================
"""

import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from capture_window import find_window, user32                               # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors, wait_rect            # noqa: E402
from verify_anim_layers import (CAM, DEFAULT_EXE, OUT_DIR, SEL, TPL,      # noqa: E402
                                _base_env, _close, bake_template,
                                ensure_template)

CLIP = "Walk"          # 有 4 段可用，挑最短的一段；反正都是原地动画
FIXED_DT = "0.0166667"  # 1/60：仿真时间与墙钟解耦，日志条数才可数

ROOT_RE = re.compile(r"ANIM-ROOTMOTION cfg=(\d+) n=(\d+) bone=(\S+) "
                     r"dist=([\d.]+) yaw=(-?[\d.]+)")

results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print("[%s] %s  %s" % ("PASS" if ok else "FAIL", name, detail))


# ============================================================
# 变体：模板上只改 `animator.rootMotion` / `rootMotionBone`
# ============================================================

def _root_of(doc):
    for e in doc["entities"]:
        if e.get("skinned"):
            return e
    raise SystemExit("[FAIL] 模板里没有蒙皮实体")


def build_variant(tag, mode=1, bone="", playing=True):
    with open(TPL, encoding="utf-8") as f:
        doc = json.load(f)
    root = _root_of(doc)
    root["skinned"]["clipName"] = CLIP
    root["skinned"]["time"] = 0.0
    root["skinned"]["playing"] = False     # 权威在 animator 那一份
    root["skinned"]["loop"] = True
    ent = {
        "defaultClip": CLIP,
        "controllerPath": "",
        "time": 0.0,
        "speed": 1.0,
        "playing": playing,
        "loop": True,
        "rootMotion": mode,
    }
    if bone:
        ent["rootMotionBone"] = bone
    root["animator"] = ent
    doc["generator"] = "verify_root_motion"
    path = os.path.join(OUT_DIR, "root_motion_%s.json" % tag)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, ensure_ascii=False)
    return path


# ============================================================
# 跑一轮并收日志
#
# ⚠ 不复用 `verify_anim_layers.run_shot`：那个等的是"2 条 ANIM-TICK +
#   一张非空视口图"，而 ANIM-ROOTMOTION 每 60 帧才一条 —— 2 条 TICK 只
#   够 120 帧，抓到的条数依赖首帧对齐，会**偶发**抓不到（本脚本的第一版
#   就是这么假失败过一次）。这里显式等够 N 条 TICK 再收。
# ============================================================

def run_anim(scene, extra_env=None, needle="ANIM-ROOTMOTION ", want=2,
             need_ticks=4, timeout=45.0, select=None):
    env = _base_env()
    env["MYVK_EDITOR_SCENE"] = scene
    env["MYVK_EDITOR_CAM"] = CAM
    env["MYVK_FIXED_DT"] = FIXED_DT
    if select:
        env["MYVK_EDITOR_SELECT"] = select
    env.update(extra_env or {})

    kill_stale_editors()
    proc = subprocess.Popen([DEFAULT_EXE], cwd=os.path.dirname(DEFAULT_EXE),
                            env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True,
                            encoding="utf-8", errors="replace", bufsize=1)
    tap = LogTap()
    tap.start(proc)
    hwnd = None
    try:
        hwnd = find_window("Venn Editor")
        if not hwnd:
            raise SystemExit("[FAIL] 编辑器窗口没出现")
        user32.ShowWindow(hwnd, 9)
        wait_rect(tap, "VP-RECT", 20.0)
        end = time.time() + timeout
        while time.time() < end:
            with tap.lock:
                ticks = sum(1 for l in tap.lines if "ANIM-TICK " in l)
                hits = sum(1 for l in tap.lines if needle in l)
            if ticks >= need_ticks and hits >= want:
                break
            time.sleep(0.25)
        with tap.lock:
            return list(tap.lines)
    finally:
        _close(proc, hwnd)


def main():
    # ⚠ 用 `ensure_template`（判"是不是按这份资产生成的"），不是判文件在不在。
    #   资产一搬，旧模板里每条 mesh.path 都指空 —— 症状是 ①/cfg=1 但 bone=-
    #   和「模板里没有蒙皮实体」这种完全看不出因果的红。
    ensure_template()

    # ---- ① 自动选骨 ----
    # 不看 `n` 是几（这份资产一律 0），只看 cfg 对了、骨名解出来了。
    lines = run_anim(build_variant("auto", mode=1, bone=""), want=2)
    txt = "\n".join(lines)
    m = ROOT_RE.findall(txt)
    ok_cfg = bool(m) and all(r[0] == "1" for r in m)
    bones = sorted({r[2] for r in m})
    real_bone = bool(bones) and bones[0] != "(none)"
    check("① 自动选骨：cfg=1 且 bone= 是真实骨名", ok_cfg and real_bone,
          "bone=%s cfg_hits=%d" % (",".join(bones) or "-", len(m)))
    # 顺带把"原地动画"这件事本身变成一条记录（不是失败，是资产事实）
    dists = [float(r[3]) for r in m]
    check("① 这份资产是原地动画（dist 恒为 0 → n=0 是预期的）",
          all(d == 0.0 for d in dists) or not dists,
          "dists=%s" % (dists[:3] if dists else "[]"))

    # ---- ② 骨名填错 ----
    lines_bad = run_anim(build_variant("badbone", mode=1,
                                       bone="no_such_bone_xyz"),
                         want=1)
    txt_bad = "\n".join(lines_bad)
    has_miss = "ANIM-ROOTMOTION-BONE-MISS" in txt_bad
    bones_bad = {r[2] for r in ROOT_RE.findall(txt_bad)}
    none_bone = bones_bad == {"(none)"}
    check("② 骨名填错 → BONE-MISS 且 bone=(none)（不静默改用根骨）",
          has_miss and none_bone,
          "miss=%s bone=%s" % (has_miss, ",".join(sorted(bones_bad)) or "-"))

    # ---- ③ 落盘往返 ----
    # ⚠ 用**真骨名**跑这一组：`rootMotionBone` 只在非空时写文件，
    #   空的时候"往返成功"是平凡成立的（键本来就不该在）。
    real = bones[0] if real_bone else ""
    v_save = build_variant("save", mode=1, bone=real)
    run_anim(v_save, {"MYVK_EDITOR_CONSOLE_CMD": "scene.save"},
             needle="ANIM-ROOTMOTION ", want=1, timeout=30.0)
    saved_rm = None
    saved_bone = None
    runtime_leak = []
    if os.path.isfile(v_save):
        with open(v_save, encoding="utf-8") as f:
            doc = json.load(f)
        root = _root_of(doc)
        an = root.get("animator") or {}
        saved_rm = an.get("rootMotion")
        saved_bone = an.get("rootMotionBone")
        runtime_leak = [k for k in
                        ("rootPrevValid", "rootPrevWorld", "rootBasePos",
                         "rootDelta", "eventPrev", "eventClip")
                        if k in an]
    ok_save = (saved_rm == 1 and saved_bone == real and not runtime_leak)
    check("③ scene.save：rootMotion/rootMotionBone 保留、运行时字段不落盘",
          ok_save, "rootMotion=%s bone=%s leak=%s"
          % (saved_rm, saved_bone, runtime_leak or "none"))

    # ---- ④ UI 面 ----
    # 只画锚点，**不点控件**：Root motion 下拉在面板深处，可能落在裁剪区
    # 外，点下去点到别的东西比不点更糟（B1 的 ⑨ 同一个纪律）。
    env_ui = {"MYVK_EDITOR_PANELS": "inspector",
              "MYVK_EDITOR_INSP_TAB": "animation"}
    lines_ui = run_anim(build_variant("ui", mode=1, bone=""),
                        extra_env=env_ui, needle="INS-ANIM-ROOT",
                        want=1, need_ticks=2, timeout=30.0,
                        # 实体名跟模板里那份资产的文件名走（见 verify_anim_layers.SEL）
                        select=SEL)
    txt_ui = "\n".join(lines_ui)
    has_combo = "INS-ANIM-ROOTMOTION" in txt_ui
    has_bone = "INS-ANIM-ROOTBONE" in txt_ui
    check("④ Inspector 画出 Root motion 下拉 + 根骨输入框",
          has_combo and has_bone,
          "combo=%s bone=%s" % (has_combo, has_bone))

    # ---- 汇总 ----
    bad = [r for r in results if not r[1]]
    print("-" * 66)
    for name, ok, _ in results:
        print("%-5s %s" % ("PASS" if ok else "FAIL", name))
    print("结果: " + ("PASS" if not bad else "FAIL: %d 组未通过" % len(bad)))
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
