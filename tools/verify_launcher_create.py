#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/verify_launcher_create.py —— 启动器 M2 验收：四步向导 + 创建引擎

路线图 §7-M2 的验收条款：
  「新建后目录结构对账（用 `template.json` 的 `files[]`）、`game.json` 的
    分辨率/标题、重复创建被拒、失败无残留。」

这个脚本**全程点按钮**，不绕过 UI：
  ① 进新建向导（`MYVK_LAUNCHER_PAGE=new`），点"第三人称"卡片 → 断言日志里
     出现 `LAUNCHER-WIZARD type=third_person`（证明这一下真的点到了）；
  ② 下一步到"位置"，再下一步到"画面"；
  ③ 点"创建" → 停在"完成"页；
  ④ 关掉启动器，对着磁盘一条条对账。

第二轮换成第一人称，第三轮**故意重名**（必须被拒、且不留残留）。

⚠ `MYVK_LAUNCHER_TEST_ROOT` / `TEST_NAME` 只填向导的**初值** ——
  四步一步不少，按钮一个不落地真点。不这么做的话，测试得去模拟
  "往系统文件夹对话框里敲一串带中文的临时路径"，那件事在 CI 上做不可靠。

覆盖（C1~C16）：
  C1  卡片点击生效（日志有 type=）
  C2  下一步确实换页（换页后才出现 `LAUNCH-INPUT-root`）
  C3  再下一步出现分辨率/画质/将写入清单（`LAUNCH-COMBO-res` / `-quality` / `LAUNCH-WRITES`）
  C4  创建成功 → 出现 `LAUNCH-DONE`
  C5  `venn.project.json` 存在且 name/gameType/template 都对
  C6  `template.json.files[]` 里每个文件都在（**用清单对账**，不手写文件名）
  C7  `assets/game.json` 的 title == 工程名
  C8  `assets/game.json` 的 width/height == 1920x1080（向导默认档）
  C9  `config/render.json` 的 enable 键**全量**（= 引擎的 enable 参数个数）
  C10 High 档真的打开 ssao/taa、关掉 dof
  C11 场景里**没有** Player 实体、只有 GameStart 出生点（第八轮起的契约：
      玩家是运行时产物；2026-10-22 改写，原来查的是 player.mode）
  C12 新工程进了 recent.json
  C13 重名创建被拒（日志 `PROJ-CREATE FAILED at Validate`）
  C14 被拒后**没有** `<name>.tmp` 残留
  C15 被拒后原工程的文件没被改动（mtime 不变）
  C16 相机取景随类型不同（TP distance 4.5 / pitch 0.35；FP 8.0 / 0.20）
       —— 第一人称那轮的同类断言前缀是 `F`（F5~F16），两轮编号不撞。

用法：
    python tools/verify_launcher_create.py
"""

import json
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import launcher_kit as kit  # noqa: E402

# 截图落点（`run_wizard` 里用；主流程会把它指到本次的 work 目录）
WORK_FOR_SHOTS = os.path.join(kit.WORK_ROOT, "launcher_create")


def run_wizard(work, name, type_index, recent_path, expect_ok=True):
    """跑一遍四步向导。返回 (ok, fails, log_text, screenshot_ok)。"""
    fails = []
    proc, tap, hwnd = kit.launch(
        recent_path=recent_path,
        env_extra={
            "MYVK_LAUNCHER_PAGE": "new",
            "MYVK_LAUNCHER_TEST_ROOT": work,
            "MYVK_LAUNCHER_TEST_NAME": name,
        })
    if not hwnd:
        proc.kill()
        return False, ["启动器窗口没出现（向导跑不了）"], tap.text(), False

    shot_ok = False
    try:
        # ---- ① Step1 类型 ----
        if not tap.wait_rect("LAUNCH-TYPE-0", timeout=25.0):
            fails.append("C1 向导 Step1 没画出类型卡片（缺 LAUNCH-TYPE-0）")
            return False, fails, tap.text(), False
        tag = "LAUNCH-TYPE-%d" % type_index
        if not kit.click_anchor(hwnd, tap, tag):
            fails.append("C1 点不到 %s" % tag)
            return False, fails, tap.text(), False
        kit.capture(hwnd, os.path.join(WORK_FOR_SHOTS, "step1_%s.png" % name))
        want_type = "first_person" if type_index == 0 else "third_person"
        # ⚠ 断言"这一下真的被吃到了"：只断言"页面还在 Step1" 是不够的 ——
        #   吞掉全部点击也能通过（本项目的输入类断言踩过这个坑）。
        if not tap.wait_line("LAUNCHER-WIZARD type=" + want_type, timeout=8.0):
            fails.append("C1 卡片点击没有生效（日志里没有 type=%s）" % want_type)
        else:
            kit.log("CHECK C1 click %s -> type=%s OK" % (tag, want_type))

        # ---- ② Step2 位置 ----
        if not kit.click_anchor(hwnd, tap, "LAUNCH-BTN-next"):
            fails.append("C2 找不到 / 点不到 LAUNCH-BTN-next（Step1）")
            return False, fails, tap.text(), False
        if not tap.wait_rect("LAUNCH-INPUT-root", timeout=15.0):
            fails.append("C2 点下一步后没有出现 Step2（缺 LAUNCH-INPUT-root）")
            return False, fails, tap.text(), False
        kit.log("CHECK C2 step2 reached (LAUNCH-INPUT-root) OK")
        kit.capture(hwnd, os.path.join(WORK_FOR_SHOTS, "step2_%s.png" % name))

        # ---- ③ Step3 画面 ----
        if not kit.click_anchor(hwnd, tap, "LAUNCH-BTN-next"):
            fails.append("C3 找不到 / 点不到 LAUNCH-BTN-next（Step2）")
            return False, fails, tap.text(), False
        ok3 = True
        for tag in ("LAUNCH-COMBO-res", "LAUNCH-COMBO-quality", "LAUNCH-WRITES"):
            if not tap.wait_rect(tag, timeout=15.0):
                fails.append("C3 Step3 缺锚点 %s" % tag)
                ok3 = False
        if ok3:
            kit.log("CHECK C3 step3 reached (res/quality/writes) OK")
            kit.capture(hwnd, os.path.join(WORK_FOR_SHOTS,
                                          "step3_%s.png" % name))
        else:
            return False, fails, tap.text(), False

        # ---- ④ 创建 ----
        if not kit.click_anchor(hwnd, tap, "LAUNCH-BTN-create"):
            fails.append("C4 找不到 / 点不到 LAUNCH-BTN-create")
            return False, fails, tap.text(), False

        if expect_ok:
            if tap.wait_rect("LAUNCH-DONE", timeout=40.0):
                kit.log("CHECK C4 created -> LAUNCH-DONE OK")
                shot_ok = kit.capture(
                    hwnd, os.path.join(WORK_FOR_SHOTS, "done_%s.png" % name))
            else:
                fails.append("C4 点了创建但没有出现 LAUNCH-DONE（创建失败？"
                             "见上面的 PROJ-CREATE 日志）")
                return False, fails, tap.text(), False
        else:
            # 预期失败：不许出现 Done，且必须有一条可读的失败原因。
            if tap.wait_rect("LAUNCH-DONE", timeout=6.0):
                fails.append("C13 重名创建居然成功了（应该被拒）")
                return False, fails, tap.text(), False
            if not tap.wait_line("PROJ-CREATE FAILED at Validate", timeout=10.0):
                fails.append("C13 重名创建没有打出 'PROJ-CREATE FAILED at Validate'")
            else:
                kit.log("CHECK C13 duplicate rejected at Validate OK")
    finally:
        # 需求 A（2026-10-22）：创建成功即**自动**拉起编辑器。本用例只关心
        # "工程文件建出来了没有"，那个编辑器是纯副作用 —— 不关掉的话三轮
        # 向导会攒下三个编辑器窗口，后面用例的 `find_window("Venn Editor")`
        # 就会认错人。⚠ 重名那一轮预期创建失败、压根没有 spawn，所以按
        # `expect_ok` 区分，别白等。
        if expect_ok:
            kit.dismiss_spawned(tap)
        kit.shutdown(proc, hwnd)

    return True, fails, tap.text(), shot_ok


def check_project(root, name, kind, fails, prefix):
    """对着磁盘对账。`kind` = first_person / third_person。"""
    # ---- C5 身份文件 ----
    pj = os.path.join(root, "venn.project.json")
    if not os.path.isfile(pj):
        fails.append("%s5 缺 venn.project.json" % prefix)
        return
    n_before = len(fails)
    with open(pj, encoding="utf-8") as f:
        p = json.load(f)
    if p.get("name") != name:
        fails.append("%s5 name=%r，期望 %r" % (prefix, p.get("name"), name))
    if p.get("gameType") != kind:
        fails.append("%s5 gameType=%r，期望 %r" % (prefix, p.get("gameType"), kind))
    if p.get("template") != kind:
        fails.append("%s5 template=%r，期望 %r"
                     % (prefix, p.get("template"), kind))
    if len(fails) == n_before:
        kit.log("CHECK %s5 identity name=%s gameType=%s OK" % (prefix, name, kind))

    # ---- C6 目录结构：用 template.json 的 files[] 对账 ----
    # ⚠ 对账用**模板自己声明的清单**，不手写文件名 —— 手写的话模板加一个
    #   文件，测试不会红，那个文件被漏拷也永远发现不了。
    with open(os.path.join(kit.TEMPLATES, kind, "template.json"),
              encoding="utf-8") as f:
        manifest = json.load(f)
    missing = [rel for rel in manifest.get("files", [])
               if not os.path.isfile(os.path.join(root, rel))]
    if missing:
        fails.append("%s6 模板清单里的文件缺失: %s" % (prefix, missing))
    else:
        kit.log("CHECK %s6 template files[] all present (%d) OK"
                % (prefix, len(manifest.get("files", []))))

    # ---- C7/C8 game.json ----
    gj = os.path.join(root, "assets", "game.json")
    with open(gj, encoding="utf-8") as f:
        g = json.load(f)
    if g.get("title") != name:
        fails.append("%s7 game.json title=%r，期望 %r"
                     % (prefix, g.get("title"), name))
    else:
        kit.log("CHECK %s7 game.json title=%s OK" % (prefix, name))
    if (g.get("width"), g.get("height")) != (1920, 1080):
        fails.append("%s8 game.json %sx%s，期望 1920x1080（向导默认档）"
                     % (prefix, g.get("width"), g.get("height")))
    else:
        kit.log("CHECK %s8 game.json 1920x1080 OK" % prefix)
    # 路线图 P4：`quality` 这个空挂键必须**不被写进去**
    if "quality" in g:
        fails.append("%s8 game.json 里又出现了废弃的 quality 键（P4 回归）" % prefix)

    # ---- C9/C10 render.json ----
    rj = os.path.join(root, "config", "render.json")
    with open(rj, encoding="utf-8") as f:
        r = json.load(f)
    keys = [k for k in r if not k.startswith("_") and k != "version"]
    # 25 = 22 个 enable 参数（`render::enableParamCount()`）
    #      + 3 个数值档位项（E26：shadow.cascades / shadow.distance /
    #        lshadow.maxLights，见 `render::qualityValueCount()`）。
    # ⚠ 这个数字会随特性增减而变 —— 变了测试就红，**这是有意的**：
    #   它的意思是"启动器写的键数已经和引擎对不上了，去核对一遍"。
    want = 25
    if len(keys) != want:
        fails.append("%s9 render.json 写了 %d 个键，期望 %d（引擎 enable 参数数）"
                     % (prefix, len(keys), want))
    else:
        kit.log("CHECK %s9 render.json keys=%d (enable + quality values) OK"
                % (prefix, len(keys)))
    want_on = {"ssao.enabled": True, "taa.enabled": True, "dof.enabled": False}
    bad = [k for k, v in want_on.items() if r.get(k) is not v]
    if bad:
        fails.append("%s10 High 档开关不对: %s（实际 %s）"
                     % (prefix, bad, {k: r.get(k) for k in bad}))
    else:
        kit.log("CHECK %s10 High preset (ssao/taa on, dof off) OK" % prefix)

    # ---- C11 场景里**没有**玩家实体，只有一个出生点 ----
    #
    # ⚠⚠ 这条断言在 2026-10-22 被**改写**过（原样是查
    #   `entities[Player].player.mode == "third"/"first"`）。那个契约已经被
    #   用户改掉了，现在的契约是：
    #
    #     用户原话（第八轮，2026-10-21）："初始场景里不摆放任何物体"。
    #
    #   于是 `build_scene()` 只写 Terrain + GameStart，玩家由
    #   `RuntimeApp::spawnPlayerFromGameStart` 在**运行时**造出来，存盘里
    #   不该出现它（见那个函数上方那段注释）。所以"Player 实体缺失"现在
    #   **不是 bug 而是契约**，旧断言永远不可能再绿 —— 留着它只会让
    #   "红"变成噪音，真出现 Player 时反而没人看见。
    #
    #   两种类型的差异现在全在 `camera` 段（C16 查 distance / pitch）。
    with open(os.path.join(root, "assets", "scenes", "main.json"),
              encoding="utf-8") as f:
        scene = json.load(f)
    names = [e.get("name") for e in scene.get("entities", [])]
    if "Player" in names:
        fails.append("%s11 场景里又出现了 Player 实体（第八轮起玩家是运行时"
                     "产物，位置由 GameStart 决定）" % prefix)
    elif "GameStart" not in names:
        fails.append("%s11 场景里没有 GameStart（玩家没有出生点）" % prefix)
    else:
        kit.log("CHECK %s11 scene entities=%s (no Player, has GameStart) OK"
                % (prefix, names))

    # ---- C16 相机取景随类型不同（现在**唯一**的类型差异载体）----
    want_dist, want_pitch = {"first_person": (8.0, 0.20),
                             "third_person": (4.5, 0.35)}[kind]
    cam = scene.get("camera", {})
    got_dist = cam.get("distance")
    if abs(float(got_dist or -1) - want_dist) > 1e-6:
        fails.append("%s16 camera.distance=%s，期望 %s"
                     % (prefix, got_dist, want_dist))
    else:
        kit.log("CHECK %s16 camera.distance=%s OK" % (prefix, got_dist))
    # ⚠ pitch 是**弧度**、正数 = 相机在目标上方（2026-10-21 踩过一次
    #   度/弧度混用：写成 -12.0 会被夹到 -89°，相机沉到地面以下朝天看）。
    got_pitch = cam.get("pitch")
    if abs(float(got_pitch if got_pitch is not None else -99.0) - want_pitch) > 1e-6:
        fails.append("%s16 camera.pitch=%s，期望 %s rad（正数 = 相机在目标上方）"
                     % (prefix, got_pitch, want_pitch))
    else:
        kit.log("CHECK %s16 camera.pitch=%.2f rad OK" % (prefix, want_pitch))


def main():
    if not os.path.isfile(kit.LAUNCHER):
        kit.log("结果: FAIL: Launcher.exe 不存在（先 make build）")
        return 1

    work = os.path.join(kit.WORK_ROOT, "launcher_create")
    work = kit.fresh_dir(work)
    os.makedirs(work, exist_ok=True)
    recent_path = os.path.join(work, "recent.json")
    kit.log("PROBE work=%s" % work)

    fails = []

    # ================= 第一轮：第三人称 =================
    name1 = "TpGame"
    ok, f, log1, shot_ok = run_wizard(work, name1, 1, recent_path, expect_ok=True)
    fails += f
    root1 = os.path.join(work, name1)
    if ok and not f:
        with open(os.path.join(work, "create_tp.log"), "w", encoding="utf-8") as fh:
            fh.write(log1)
        check_project(root1, name1, "third_person", fails, "C")

        # ---- C12 进了最近列表 ----
        paths = [it.get("path") for it in kit.read_recent(recent_path)]
        if kit.project_key(root1) not in [kit.project_key(p or "") for p in paths]:
            fails.append("C12 新工程没进 recent.json（现有 %s）" % paths)
        else:
            kit.log("CHECK C12 recent.json has %s OK" % name1)

    # ================= 第二轮：第一人称 =================
    name2 = "FpGame"
    ok, f, log2, _ = run_wizard(work, name2, 0, recent_path, expect_ok=True)
    fails += f
    root2 = os.path.join(work, name2)
    if ok and not f:
        with open(os.path.join(work, "create_fp.log"), "w", encoding="utf-8") as fh:
            fh.write(log2)
        check_project(root2, name2, "first_person", fails, "F")

    # ================= 第三轮：重名必须被拒 =================
    if os.path.isdir(root1):
        # 记下 mtime，事后证明"被拒"没有碰过原工程
        before = {n: os.path.getmtime(os.path.join(root1, n))
                  for n in ("venn.project.json", "assets/game.json")}
        ok, f, log3, _ = run_wizard(work, name1, 1, recent_path, expect_ok=False)
        fails += f
        with open(os.path.join(work, "create_dup.log"), "w", encoding="utf-8") as fh:
            fh.write(log3)

        # ---- C14 没有 .tmp 残留 ----
        tmp = root1 + ".tmp"
        if os.path.exists(tmp):
            fails.append("C14 创建失败后留下了残留目录 %s" % tmp)
        else:
            kit.log("CHECK C14 no .tmp residue OK")

        # ---- C15 原工程没被改动 ----
        after = {n: os.path.getmtime(os.path.join(root1, n)) for n in before}
        changed = [n for n in before if abs(before[n] - after[n]) > 1e-6]
        if changed:
            fails.append("C15 被拒的重名创建改动了原工程: %s" % changed)
        else:
            kit.log("CHECK C15 original project untouched OK")
    else:
        fails.append("C15 第一轮没建出工程，重名用例没法跑")

    return kit.finish(fails)


if __name__ == "__main__":
    sys.exit(main())
