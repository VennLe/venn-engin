#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/verify_launcher_create.py —— 启动器 M2 验收：四步向导 + 创建引擎

路线图 §7-M2 的验收条款：
  「新建后目录结构对账（用 `template.json` 的 `files[]`）、`game.json` 的
    分辨率/标题、重复创建被拒、失败无残留。」

这个脚本**全程点按钮**，不绕过 UI：
  ① 进新建向导（`MYVK_LAUNCHER_PAGE=new`），点类型卡片 → 断言日志里
     出现 `LAUNCHER-WIZARD type=<id>`（证明这一下真的点到了）；
  ② 下一步到"位置"，再下一步到"画面"；
  ③ 点"创建" → 停在"完成"页；
  ④ 关掉启动器，对着磁盘一条条对账。

**每种游戏类型各跑一整轮** —— `kGameTypes[]` 里有几种就跑几轮，数量由
`read_game_type_ids()` 从源码读出来（不在测试里再写一份）。之后补两轮：
故意重名（必须被拒、且不留残留）、不给 TEST_ROOT（应当默认用上次那个目录）。

⚠ `MYVK_LAUNCHER_TEST_ROOT` / `TEST_NAME` 只填向导的**初值** ——
  四步一步不少，按钮一个不落地真点。不这么做的话，测试得去模拟
  "往系统文件夹对话框里敲一串带中文的临时路径"，那件事在 CI 上做不可靠。

覆盖（每轮 1~11 与 16；前缀按类型分：C=third_person / F=first_person /
I=isometric / B=card_battler —— 编号不撞。12~17 只在第一轮和尾部出现）：
  ?1  类型卡片**全部画在客户区里**、点哪张就报哪个 type、表里没有的不许画
  C2  下一步确实换页（换页后才出现 `LAUNCH-INPUT-root`）
  C3  再下一步出现分辨率/画质/将写入清单（`LAUNCH-COMBO-res` / `-quality` / `LAUNCH-WRITES`）
  C4  创建成功 → 出现 `LAUNCH-DONE`
  ?5  `venn.project.json` 存在且 name/gameType/template 都对
  ?6  `template.json.files[]` 里每个文件都在（**用清单对账**，不手写文件名）
  ?7  `assets/game.json` 的 title == 工程名
  ?8  `assets/game.json` 的 width/height == 1920x1080（向导默认档）
  ?9  `config/render.json` 的 enable 键**全量**（= 引擎的 enable 参数个数）
  ?10 High 档真的打开 ssao/taa、关掉 dof
  ?11 场景里**没有** Player 实体、只有 GameStart 出生点（第八轮起的契约：
      玩家是运行时产物；2026-10-22 改写，原来查的是 player.mode）
  ?16 场景相机与**模板场景**逐字段一致（distance/pitch/yaw/fov）
      —— 2026-11-04 重写：原先按类型硬编码 TP 4.5/0.35、FP 8.0/0.20，
         49efa67 把模板相机统一成 14.0/0.26 之后就变成过时记账了。
         详见 check_project 里 C16 上方那段。
  C12 新工程进了 recent.json
  C13 重名创建被拒（日志 `PROJ-CREATE FAILED at Validate`）
  C14 被拒后**没有** `<name>.tmp` 残留
  C15 被拒后原工程的文件没被改动（mtime 不变）
  C17 第二次新建默认用上次那个目录（不是回落 C 盘 Documents）

用法：
    python tools/verify_launcher_create.py
"""

import json
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import launcher_kit as kit  # noqa: E402

# 截图落点（`run_wizard` 里用；主流程会把它指到本次的 work 目录）
WORK_FOR_SHOTS = os.path.join(kit.WORK_ROOT, "launcher_create")

# `engine/project/ProjectFile.cpp` 里的 `kGameTypes[]` —— 类型 id 的**唯一**来源。
PROJECTFILE_CPP = os.path.join(kit.REPO, "engine", "project", "ProjectFile.cpp")

# 兜底表（顺序必须与 `kGameTypes[]` 一致）。正常情况下不走到这里 ——
# 见 `read_game_type_ids()`。
GAME_TYPE_IDS_FALLBACK = ["first_person", "third_person",
                          "isometric", "card_battler"]

_GAME_TYPE_RE = re.compile(r'\{\s*GameType::\w+\s*,\s*"([a-z_]+)"')


def read_game_type_ids():
    """按 `kGameTypes[]` 的**顺序**返回类型 id 列表（= 向导卡片的下标顺序）。

    ⚠⚠ 为什么不在这里再写一份硬编码表：`LAUNCH-TYPE-<i>` 锚点的下标就是
      这张表的顺序。写两份必然出现"加了第 5 种游戏类型、测试还在点前 4 张"，
      而那种情况下**第 5 张卡没有任何用例点过** —— 界面上点不到就等于没交
      付，可测试是全绿的。

    读不到（文件挪位置 / 改写法）时退回 `GAME_TYPE_IDS_FALLBACK` 并**打出
    一行 WARN** —— 静默退回等于把"这条检查已经失效"藏起来。
    """
    try:
        with open(PROJECTFILE_CPP, encoding="utf-8") as f:
            src = f.read()
    except OSError as e:
        kit.log("WARN 读不到 %s（%s），类型 id 用兜底表" % (PROJECTFILE_CPP, e))
        return list(GAME_TYPE_IDS_FALLBACK)
    parts = src.split("kGameTypes[]")
    if len(parts) < 2:
        kit.log("WARN %s 里找不到 kGameTypes[]，类型 id 用兜底表"
                % PROJECTFILE_CPP)
        return list(GAME_TYPE_IDS_FALLBACK)
    body = parts[1].split("};")[0]
    ids = _GAME_TYPE_RE.findall(body)
    if not ids:
        kit.log("WARN kGameTypes[] 里没解析出 id，类型 id 用兜底表")
        return list(GAME_TYPE_IDS_FALLBACK)
    if ids != GAME_TYPE_IDS_FALLBACK:
        kit.log("WARN kGameTypes[] = %s，与兜底表 %s 不同 —— 兜底表该跟着更新了"
                % (ids, GAME_TYPE_IDS_FALLBACK))
    return ids


def check_templates_fresh(type_ids):
    """`build/bin/templates/` 必须不旧于仓库里的 `templates/`。

    ⚠⚠ 为什么单开一条前置检查（2026-11-04 实测踩到）：**启动器读的是
      `<exe>/templates`**（`project::templatesRoot()` 的候选链），不是仓库
      里的 `templates/`。改完模板不重新 `make build`，`VennTemplatesCopy`
      就没跑 —— 新建出来的工程是**上一版**的场景。

      症状极具误导性：C16 报"场景相机与模板不一致: distance=6.0(期望 9.0)"
      ，看着像引擎把相机改坏了（我第一反应就是去翻 `ProjectCreate` 有没有
      重写相机 —— 没有）。真因是那次拷的是 6 分钟前的模板。
      本项目的自检（K33）读的是仓库 `templates/`，所以它**照样全绿**。

    返回失败信息列表（空 = 新鲜）。
    """
    fails = []
    for tid in type_ids:
        repo = os.path.join(kit.TEMPLATES, tid, "template.json")
        built = os.path.join(kit.BIN, "templates", tid, "template.json")
        if not os.path.isfile(built):
            fails.append("Z2 build/bin/templates/%s 不存在（先 make build，"
                         "模板由 VennTemplatesCopy 拷过去）" % tid)
            continue
        # 比的是**整个目录**里最新的那个文件 vs 被拷过去的同名文件 ——
        # 只比 template.json 会漏掉"只改了场景/脚本"的情况（最常见）。
        for sub in ("template.json", "assets/scenes/main.json"):
            rp = os.path.join(kit.TEMPLATES, tid, sub)
            bp = os.path.join(kit.BIN, "templates", tid, sub)
            if not os.path.isfile(rp):
                continue
            if not os.path.isfile(bp) or \
                    os.path.getmtime(rp) > os.path.getmtime(bp) + 1e-6:
                fails.append("Z2 build/bin/templates/%s/%s 比仓库旧 —— 启动器"
                             "会用旧模板建工程。先 make build。" % (tid, sub))
    return fails


def run_wizard(work, name, type_index, recent_path, expect_ok=True):
    """跑一遍四步向导。返回 (ok, fails, log_text, screenshot_ok)。"""
    fails = []
    proc, tap, hwnd = kit.launch(
        recent_path=recent_path,
        env_extra={
            "MYVK_LAUNCHER_PAGE": "new",
            "MYVK_LAUNCHER_TEST_ROOT": work,
            "MYVK_LAUNCHER_TEST_NAME": name,
            # 分辨率**钉死**成 1920x1080 那一档（2026-10-24）。
            # 2026-10-23 起向导默认推荐"本机桌面分辨率"，于是 C8/F8 在
            # 4K 机器上期望 1920x1080 就永远红 —— 那台机器上向导写
            # 3840x2160 完全正确。本用例验的是"创建流程写出的工程"，不是
            # "分辨率推荐值"，后者由 verify_launcher_display.py 覆盖。
            "MYVK_LAUNCHER_TEST_RES": "1",
        })
    if not hwnd:
        proc.kill()
        return False, ["启动器窗口没出现（向导跑不了）"], tap.text(), False

    shot_ok = False
    type_ids = read_game_type_ids()
    try:
        # ---- ① Step1 类型 ----
        if not tap.wait_rect("LAUNCH-TYPE-0", timeout=25.0):
            fails.append("C1 向导 Step1 没画出类型卡片（缺 LAUNCH-TYPE-0）")
            return False, fails, tap.text(), False

        # ①-a **每一张**类型卡片都必须画出来，而且**落在客户区里**。
        #
        # ⚠⚠ 为什么"有矩形"不够：`logRect` 不判可见性（只按坐标去重）。卡片
        #   被挤出窗口时矩形照样打得出来，照着那个坐标点 = 点在窗口外面，
        #   日志里一条错都没有。症状是"点了第 3 张卡没反应"，而这里如果只
        #   `wait_rect` 就会一路绿到底。2026-10-31 在面板滚出视野的按钮上
        #   踩过同一类坑（"有矩形却点不到"）。
        #
        # 这张表从 2 张扩到 4 张时（2×2 布局）实测过：卡片 380×258，
        # 第二行 y=435..693，客户区 1400×880，`LAUNCH-BTN-next` 在 771 ——
        # 都在里面。这条断言就是钉住这件事的。
        cw, ch = kit.client_size(hwnd)
        for i in range(len(type_ids)):
            tag = "LAUNCH-TYPE-%d" % i
            r = tap.last_rect(tag)
            if not r:
                fails.append("C1 缺类型卡片 %s（向导只画了 %d 张，"
                             "kGameTypes[] 有 %d 种）" % (tag, i, len(type_ids)))
                continue
            if r[0] < 0 or r[1] < 0 or r[2] > cw or r[3] > ch:
                fails.append("C1 %s 出界 %s（客户区 %dx%d）—— 卡片排到窗口外面，"
                             "点它等于点空气" % (tag, r, cw, ch))
        # 表里没有的类型不许画出来（画了说明卡片表比枚举长，多出来的那张
        # 没有 kGameTypes[] 兜底，`gameTypeAt()` 会越界退化成 third_person）。
        if tap.last_rect("LAUNCH-TYPE-%d" % len(type_ids)) is not None:
            fails.append("C1 向导画出了第 %d 张类型卡片，但 kGameTypes[] 只有 %d 种"
                         " —— 多出来那张会被 gameTypeAt() 静默退化成 third_person"
                         % (len(type_ids) + 1, len(type_ids)))
        if fails:
            return False, fails, tap.text(), False
        kit.log("CHECK C1 all %d type cards on screen (client %dx%d) OK"
                % (len(type_ids), cw, ch))

        tag = "LAUNCH-TYPE-%d" % type_index
        if not kit.click_anchor(hwnd, tap, tag):
            fails.append("C1 点不到 %s" % tag)
            return False, fails, tap.text(), False
        kit.capture(hwnd, os.path.join(WORK_FOR_SHOTS, "step1_%s.png" % name))
        want_type = type_ids[type_index]
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
    """对着磁盘对账。`kind` 是 `kGameTypes[]` 里的类型 id（4 种都适用）。"""
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
        fails.append("%s8 game.json %sx%s，期望 1920x1080（用例显式指定）"
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
    # 27 = 24 个 enable 参数（`render::enableParamCount()`）
    #      + 3 个数值档位项（E26：shadow.cascades / shadow.distance /
    #        lshadow.maxLights，见 `render::qualityValueCount()`）。
    # ⚠ 这个数字会随特性增减而变 —— 变了测试就红，**这是有意的**：
    #   它的意思是"启动器写的键数已经和引擎对不上了，去核对一遍"。
    # 2026-10-29：22 → 24。核对过，多出来的两条是真实特性（角色光 /
    #   自动曝光那一批 A 类着色特性），不是垃圾键 —— 引擎自己的
    #   `verify_project`（J11）按 `enableParamCount()+qualityValueCount()`
    #   动态算，一直是 PASS，所以是**这里记账过时**，不是引擎多写了。
    want = 27
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
    #   ⚠ 2026-11-04 补：**这句话对 first_person / third_person 已经不成立了**
    #     —— 49efa67 之后两份模板共用同一个相机（14.0 / 0.26）。真正的类型
    #     差异载体变成了"拷了哪套模板"（C6 用 files[] 对账：小镇 vs 8×8 棋盘
    #     vs 牌桌；脚本分别是 main.vks / main.vks / battle.vks）与 C16 的
    #     "相机 == 模板相机"。别再假设 FP 和 TP 的相机不一样。
    start_scene = manifest.get("startScene", "scenes/main.json")
    with open(os.path.join(root, "assets", start_scene),
              encoding="utf-8") as f:
        scene = json.load(f)
    names = [e.get("name") for e in scene.get("entities", [])]
    if "Player" in names:
        fails.append("%s11 场景里又出现了 Player 实体（第八轮起玩家是运行时"
                     "产物，位置由 GameStart 决定）" % prefix)
    elif "GameStart" not in names:
        fails.append("%s11 场景里没有 GameStart（玩家没有出生点）" % prefix)
    else:
        # ⚠ 只打**条数 + 前几个名字**：小镇模板有 157 个实体，整行打出来会
        #   把同一轮的其它 CHECK 挤出屏幕，而真正要看的信息只有两个
        #   —— "没有 Player" 和 "有 GameStart"，它们已经在句子后半段了。
        kit.log("CHECK %s11 scene entities=%d %s... (no Player, has GameStart) OK"
                % (prefix, len(names), names[:4]))

    # ---- C16 相机取景 ----
    #
    # ⚠⚠ 这条断言被**推翻重写**过两次，把来龙去脉记在这里免得又写回硬编码：
    #
    #   ① 最初按类型硬编码：first_person=(8.0, 0.20)、third_person=(4.5, 0.35)。
    #   ② 提交 49efa67 把新建工程的初始场景换成"二次元小镇"，两份模板的相机
    #      被统一成 target(0,0,1.6) / yaw π/2 / pitch 0.26 / distance 14.0
    #      （理由是 30 m 视距下 GameStart 标记只有 14×38 px，"标记有可读大小"
    #      会判红）。那次只同步了 `verify_launcher_project.py`，**漏了本脚本**
    #      → 于是 C16 / F16 长期是一条"记账过时"的红，不是真 bug。
    #   ③ 现在改成「**创建出来的相机 == 模板场景的相机**」：
    #      · 模板以后调参它自动跟随，不会再变成过时记账；
    #      · "创建流程往场景里写了个默认相机 / 拷错了模板"照样会被抓住；
    #      · 它一次管住 `kGameTypes[]` 里的**全部**类型，不是只有 2 种。
    #
    # ⚠ 只比 `distance` 就够了吗：不够。等距（24.0 / 0.98）和牌桌
    #   （9.0 / 0.50）**恰好**是 distance 不同，但真正区分"俯视棋盘"和
    #   "牌桌视平线"的是 pitch，所以两个都查，顺带查 yaw/fov。
    tpl_scene_path = os.path.join(kit.TEMPLATES, kind, "assets",
                                  manifest.get("startScene", "scenes/main.json"))
    try:
        with open(tpl_scene_path, encoding="utf-8") as f:
            tpl_cam = (json.load(f).get("camera") or {})
    except OSError as e:
        fails.append("%s16 读不到模板场景 %s（%s）" % (prefix, tpl_scene_path, e))
        return
    cam = scene.get("camera") or {}
    cam_bad = []
    for key in ("distance", "pitch", "yaw", "fov"):
        want_v = tpl_cam.get(key)
        if want_v is None:
            continue
        got_v = cam.get(key)
        if got_v is None or abs(float(got_v) - float(want_v)) > 1e-6:
            cam_bad.append("%s=%s(期望 %s)" % (key, got_v, want_v))
    if cam_bad:
        fails.append("%s16 场景相机与模板不一致: %s（模板 %s）"
                     % (prefix, ", ".join(cam_bad), tpl_scene_path))
    else:
        kit.log("CHECK %s16 camera matches template (distance=%s pitch=%s) OK"
                % (prefix, cam.get("distance"), cam.get("pitch")))
    # ⚠ pitch 是**弧度**、正数 = 相机在目标上方（2026-10-21 踩过一次度/弧度
    #   混用：写成 -12.0 会被夹到 -89°，相机沉到地面以下朝天看）。这条与
    #   "跟模板一致"正交 —— 模板自己写错也要拦。
    got_pitch = cam.get("pitch")
    if got_pitch is None or not (-1.5533 < float(got_pitch) < 1.5533):
        fails.append("%s16 camera.pitch=%s 不在 ±89° 内（单位应为弧度）"
                     % (prefix, got_pitch))


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
    type_ids = read_game_type_ids()
    kit.log("PROBE game-types=%d %s" % (len(type_ids), type_ids))

    # ⚠ 先验"启动器会读到哪一版模板"，再验"建出来的对不对" —— 顺序反了的话
    #   模板是旧的会产生一堆**看起来像引擎坏了**的红（见 check_templates_fresh）。
    stale = check_templates_fresh(type_ids)
    if stale:
        for s in stale:
            kit.log("CHECK FAIL " + s)
        return kit.finish(stale)

    # ================= 每种游戏类型各跑一整轮 =================
    #
    # 用户要求（2026-11-04）："游戏类型的设置应该是在启动器创建游戏的时候
    # 去设置的"。所以验收也不能只验两种类型 —— **每一种**都要真的点卡片、
    # 走完四步、落到磁盘，并且对出来的 `venn.project.json` 里
    # `gameType` / `template` 都等于那个 id。
    #
    # ⚠ 文件名用类型 id 拼，不按下标 —— "第 3 轮"在表变长之后指的就不是
    #   同一个东西了，而 `create_3.log` 这种名字会让人翻错日志。
    # 前缀按类型固定分配（不是按下标），这样增删类型时老判据的编号不漂。
    ROUNDS = [
        ("third_person", "TpGame",   "C", "create_third_person.log"),
        ("first_person", "FpGame",   "F", "create_first_person.log"),
        ("isometric",    "IsoGame",  "I", "create_isometric.log"),
        ("card_battler", "CardGame", "B", "create_card_battler.log"),
    ]
    # 只在 `kGameTypes[]` 里真的存在时才跑那一轮 —— 少一种类型时是"表变了"，
    # 不该变成"这四条断言全红"，但**也必须留一条红**说明有人改了枚举：
    known = {tid for tid, _, _, _ in ROUNDS}
    for tid in type_ids:
        if tid not in known:
            fails.append("Z1 kGameTypes[] 里有类型 %r 但没有对应的向导用例"
                         "（ROUNDS 表要与 ProjectFile.cpp 同步）" % tid)

    rounds = [(tid, nm, px, lf) for tid, nm, px, lf in ROUNDS
              if tid in type_ids]
    roots = {}
    for tid, nm, px, lf in rounds:
        idx = type_ids.index(tid)
        ok, f, log_text, _ = run_wizard(work, nm, idx, recent_path,
                                        expect_ok=True)
        fails += f
        root = os.path.join(work, nm)
        roots[tid] = root
        with open(os.path.join(work, lf), "w", encoding="utf-8") as fh:
            fh.write(log_text)
        if not ok or f:
            continue            # 向导本身就没走完，往下对账只会产生噪音
        check_project(root, nm, tid, fails, px)

    # ---- C12 进了最近列表（用第一个成功建出来的工程查一次就够）----
    root1 = roots.get("third_person") or next(
        (r for r in roots.values() if os.path.isdir(r)), None)
    if root1 and os.path.isdir(root1):
        paths = [it.get("path") for it in kit.read_recent(recent_path)]
        if kit.project_key(root1) not in [kit.project_key(p or "") for p in paths]:
            fails.append("C12 新工程没进 recent.json（现有 %s）" % paths)
        else:
            kit.log("CHECK C12 recent.json has %s OK" % os.path.basename(root1))

    # ================= 重名必须被拒 =================
    name1 = "TpGame"
    if os.path.isdir(os.path.join(work, name1)):
        root1 = os.path.join(work, name1)
        # 记下 mtime，事后证明"被拒"没有碰过原工程
        before = {n: os.path.getmtime(os.path.join(root1, n))
                  for n in ("venn.project.json", "assets/game.json")}
        ok, f, log3, _ = run_wizard(work, name1, type_ids.index("third_person"),
                                    recent_path, expect_ok=False)
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

    # ================= 最后：下次新建默认用"上次那个目录" =================
    #
    # 需求（2026-10-24 用户原话）："创建新游戏会设置游戏目录时不要每次都是
    # C 盘的目录，如果之前有创建过的游戏目录，默认会以之前的目录为准。"
    #
    # 复现方式必须**不**给 TEST_ROOT —— 给了就等于绕过了这条需求本身。
    # 前面已经成功建过好几个工程，`newproject.json` 里应该记着 work。
    saved = os.path.join(work, "newproject.json")
    if not os.path.isfile(saved):
        fails.append("C17 创建成功后没有写出 newproject.json（%s）" % saved)
    else:
        proc, tap, hwnd = kit.launch(
            recent_path=recent_path,
            env_extra={"MYVK_LAUNCHER_PAGE": "new"})   # ⚠ 没有 TEST_ROOT
        try:
            if not hwnd:
                fails.append("C17 第四轮启动器没起来")
            else:
                tap.wait_line("LAUNCHER-WIZARD dir-default=", timeout=25.0)
                line = next((l for l in tap.text().splitlines()
                             if "LAUNCHER-WIZARD dir-default=" in l), None)
                want_key = kit.project_key(work)
                got = kit.project_key(line or "")
                if line and "source=saved" in line and want_key in got:
                    kit.log("CHECK C17 新建向导默认目录=上次目录(%s) OK" % work)
                else:
                    fails.append("C17 新建向导默认目录不是上次那个：%r（期望 %s）"
                                 % (line, work))
                # 正面断言之外再补一条反向：**不能**又是 Documents。
                if line and "source=documents" in line:
                    fails.append("C17 又回落到 Documents（C 盘）了")
        finally:
            kit.shutdown(proc, hwnd)

    return kit.finish(fails)


if __name__ == "__main__":
    sys.exit(main())
