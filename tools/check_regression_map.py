"""自检 `run_regression.py` 的**变更影响分析表**（不跑 GUI，秒级）。

这张表（`GLOBAL_GLOBS` / `TRIGGERS` / `IGNORE_GLOBS`）是"只跑关联用例"的
唯一依据，而它刚好是最容易悄悄腐化的东西：加个新用例忘了登记 → 那一例
退化成"无条件重跑"（安全，但白跑）；把横切文件写进 `TRIGGERS` → `--narrow`
下会**假跳过**（不安全）。本脚本把这两类问题都变成红字。

检查项：
  A. 影响分析的行为分支（用合成快照，不碰仓库文件）
  B. 快照覆盖范围：该进的进、不该进的不进
  C. 映射覆盖率：哪些用例没登记（会无条件重跑）
  D. **死映射**：`TRIGGERS` 里写了不存在、且不带通配的文件路径
  E. **全横切登记**：某用例的 globs 全落在 `GLOBAL_GLOBS` 里 → 等于没登记，
     应移出 `TRIGGERS`（否则 `--narrow` 下会假跳过）

用法：python tools/check_regression_map.py
"""

import importlib.util
import os
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(TOOLS)

spec = importlib.util.spec_from_file_location(
    "rr", os.path.join(TOOLS, "run_regression.py"))
rr = importlib.util.module_from_spec(spec)
sys.modules["rr"] = rr
spec.loader.exec_module(rr)

fails = []


def mk(*paths, mt=100):
    """(路径...) → 合成快照（size|mtime 全同，只用来当"没变"的基准）。"""
    return {p: [100, mt] for p in paths}


OLD = mk("assets/shaders/decal.frag",
         "engine/render/DecalBuffer.cpp",
         "engine/render/Renderer.cpp",
         "engine/render/RenderTypes.h",
         "assets/shaders/pbr.frag",
         "CMakeLists.txt",
         "docs/render_roadmap.md",
         "editor/InspectorPanel.cpp",
         "assets/scenes/demo.json",
         "engine/render/Foo.cpp",
         # M1（G01/G05）：运行时目标与出厂配置 —— 让下面对它们的两条 fixture
         # 是"改了"而不是"新增"（新增会顺带触发"未登记点名"，那是另一回事）。
         "runtime/GameLoop.cpp",
         "assets/game.json",
         # 阶段六/七（2026-10-22）：自研二进制资产链的两个**新顶层目录**。
         # 同样先放进基准，让下面的 fixture 是"改了"而不是"新增"。
         "assetio/MeshFormat.cpp",
         "assetc/main.cpp")


def with_change(*paths, drop=()):
    d = dict(OLD)
    for p in paths:
        d[p] = [100, 999]      # mtime 变了 = 内容改了
    for p in drop:
        d.pop(p, None)
    return d


def run(name, old, new, narrow=False, want=None, want_unmapped=()):
    """want=None → 期望全量；want=set(...) → 期望**恰好**这些用例。"""
    imp = rr.impact_analysis(old, new, narrow=narrow)
    got = imp["affected"]
    ok = (got is None) if want is None else (got == set(want))
    for u in want_unmapped:
        ok = ok and (u in imp["unmapped"])
    print("  %-40s %s  affected=%s"
          % (name, "OK " if ok else "FAIL",
             "全量" if got is None else sorted(got)))
    if not ok:
        fails.append(name)
        print("       want=%s got=%s unmapped=%s global=%s"
              % ("全量" if want is None else sorted(want),
                 "全量" if got is None else sorted(got),
                 imp["unmapped"], imp["globalh"]))


print("[A] 影响分析行为分支")
run("无变更 → 谁都不跑", OLD, dict(OLD), want=())
run("decal.frag + DecalBuffer.cpp", OLD,
    with_change("assets/shaders/decal.frag", "engine/render/DecalBuffer.cpp"),
    want={"verify_decals", "verify_collision_play"})   # 后者是资源泄漏门
run("Renderer.cpp（横切）→ 全量", OLD,
    with_change("engine/render/Renderer.cpp"), want=None)
run("Renderer.cpp + --narrow → 无关联", OLD,
    with_change("engine/render/Renderer.cpp"), narrow=True, want=())
run("docs/*.md → 忽略", OLD,
    with_change("docs/render_roadmap.md"), want=())
run("CMakeLists.txt（横切，且后缀是 .txt）", OLD,
    with_change("CMakeLists.txt"), want=None)
run("pbr.frag（横切）→ 全量", OLD,
    with_change("assets/shaders/pbr.frag"), want=None)
run("新增**未登记**的 render 文件 → 全量 + 点名", OLD,
    with_change("engine/render/BrandNew.cpp"), want=None,
    want_unmapped=("engine/render/BrandNew.cpp",))
run("改一个已登记的 render 模块（ShadowPass）", OLD,
    with_change("engine/render/ShadowPass.cpp"),
    # ⚠ 2026-10-18 起 ShadowPass 也被 verify_character_import 登记（它要读
    #   `skc->jointBase` 往 push constant 里填蒙皮槽）→ 跟着同步 fixture。
    want={"verify_csm", "verify_shadow_hardening", "verify_collision_play",
          "verify_character_import"})
run("editor/InspectorPanel.cpp → 面板块全家族", OLD,
    with_change("editor/InspectorPanel.cpp"),
    # ⚠ 这里**必须**跟着 run_regression.py 的 TRIGGERS 同步长：
    #   网格参数（mesh_params）/ 分区折叠与搜索（inspector）/
    #   左侧页签导航条（inspector_tabs）/ 粒子面板 12 模块（particles）/
    #   材质槽小方块（material_chips —— 2026-10-16 补进 DEFAULT 时登记的）/
    #   Animation 页签的 INS-ANIM-* 锚点（character_import —— 2026-10-18）。
    #   UI 分区（ui2 —— 2026-10-18：Inspector 的 Render 页新增 "UI" 分组，
    #   `ui.*` 五个参数的控件就长在这里）。
    #   中文化体检（verify_i18n_coverage —— 2026-10-18：它的取值域是 editor/**，
    #   Inspector 是字面量最密的一个文件，所以必然会命中）。
    #   Tool 页签的 Audio Source 段（verify_audio —— G14-7：`INS-AUDIO-HEADER`
    #   与 `INS-AUDIO-PREVIEW` 两个锚点就画在这个文件里）。
    #   2026-10-16 加 E11.1 页签时，这条 fixture 先红了 —— 那是它在正常工作。
    #   材质实例（W1-3 —— 2026-10-18：`INS-MAT-INSTANCE` / `INS-MAT-MAKEUNIQUE`
    #   就长在 `drawMaterialSection` 里）。
    want={"verify_mesh_params", "verify_inspector",
          "verify_inspector_tabs", "verify_particles",
          "verify_material_chips", "verify_character_import",
          "verify_ui2", "verify_i18n_coverage", "verify_audio",
          "verify_material_instances",
          # W4-4（2026-10-19）：Terrain 段（`INS-TERRAIN-*` 十个锚点）
          # 就长在 `drawTerrainSection` 里。
          "verify_terrain_editor",
          # S5（2026-10-19）：Inspector 的 Script 段
          # （`INS-SCRIPT-STATE` / `INS-SCRIPT-RESET`）就长在这个文件的
          # `drawScript()` 里 —— 脚本的"实例状态"那一行是 S5 才加的，
          # 所以这条 fixture 必须跟着长一项。
          "verify_script_editor",
          # 2026-10-24：地形层材质槽多了一个"从材质仓库挑"的下拉
          # （`drawTerrainSection` 里的 `##terlayer_pick`）。
          # 改这个文件本来就该重跑材质仓库那条用例 —— 它验的正是
          # "属性栏里也能绑定对应的材质"。
          "verify_material_library",
          # 2026-10-24：GameStart 升级成"出生参数"，Inspector 新增
          # `drawGameStartSection`（`INS-GAMESTART-HEADER/MODE/SPAWN/ENABLED`）。
          # 那段是**编辑期唯一**能改视角模式（第一/第三人称）的落点，
          # 所以 verify_gamestart_edit 的 [1b] 组必须跟着它跑。
          "verify_gamestart_edit",
          # 2026-10-24（第六轮）：Layer materials 上方新增"材质包目录 + 档位
          # + Refresh"两行（`INS-MATSRC-DIR/TIER/REFRESH/STATUS`）。
          # 这条用例走的就是那几个控件，改 `drawMaterialSourceRow` 必重跑。
          "verify_material_import"})
run("assets/shaders/fg_taa.frag → taa", OLD,
    with_change("assets/shaders/fg_taa.frag"), want={"verify_taa"})
run("删除 decal.frag → 也算变更", OLD,
    with_change(drop=("assets/shaders/decal.frag",)),
    want={"verify_decals"})
run("多特性并发 → 并集", OLD,
    with_change("assets/shaders/decal.frag", "assets/shaders/fxaa.frag",
                "engine/render/GpuTimer.cpp"),
    want={"verify_decals", "verify_fxaa", "verify_gpu_timing",
          "verify_collision_play"})
run("无基准快照 → 全量", None, OLD, want=None)
# M1：改 runtime 的任意一行 → 三条运行时用例**一起**跑。
# 只挑出一条 = 假跳过（另外两条会跑在坏掉的运行时上而"通过"）。
run("runtime/GameLoop.cpp → 三条运行时用例", OLD,
    with_change("runtime/GameLoop.cpp"),
    want={"verify_runtime_build", "verify_runtime_loop", "verify_runtime_states"})
# G05 的出厂配置被 states 的冷启动断言读；G14 起它的 `audio` 块也被
# `verify_audio` 的 B 组读（`AUDIO-CONFIG ... music=0.70` 就是
# "game.json 真的生效"的唯一证据）→ 两个 owner。
run("assets/game.json → 只跑 verify_runtime_states + verify_audio", OLD,
    with_change("assets/game.json"),
    want={"verify_runtime_states", "verify_audio"})
# 阶段六/七（2026-10-22）：`assetio/**` 是**格式本体**——引擎与离线工具共用
# 同一份读写，改一处字段偏移两边一起坏，所以必须精确命中 verify_vmesh。
# 注意 `engine/assets/**`（运行时加载器 / 自检）是 GLOBAL，那条路径走全量，
# 不在这个 fixture 的期望里 —— 这也是"只登记非横切"的现场证明。
run("assetio/MeshFormat.cpp → verify_vmesh", OLD,
    with_change("assetio/MeshFormat.cpp"), want={"verify_vmesh"})
run("assetc/main.cpp → verify_vmesh", OLD,
    with_change("assetc/main.cpp"),
    # 2026-10-22 起 assetc 还挂了绑骨（--autorig）与程序化动画（--procanim）
    # 两组开关，它们各自有自己的验收脚本 → 三个 owner。
    want={"verify_vmesh", "verify_autorig", "verify_procanim"})
# 2026-10-22：自动绑骨 / 程序化动画是两条**独立**的离线链，各自只被自己的
# 用例读。合成一条的话，"把 AutoRig 的比例表改坏"会让 procanim 也重跑，
# 反过来一样 —— 跑得多但没人对结果负责，等于没登记。
# ⚠ 期望里**必然**带 verify_vmesh：它登记了 `assetio/**` 与 `assetc/**`
#   整层（.vmesh 的字节布局是格式总主，绑骨/动画也在往里写 joints 与 clips）。
#   所以这里的判据是"自己那条一定在"，不是"只有自己那条"。
run("assetio/AutoRig.cpp → 自动绑骨", OLD,
    with_change("assetio/AutoRig.cpp"),
    want={"verify_autorig", "verify_vmesh"})
run("assetio/ProcAnim.cpp → 四段程序化动画", OLD,
    with_change("assetio/ProcAnim.cpp"),
    # 2026-10-22 P5 起它也是 verify_player_feel 的 owner：walk/run/idle 的
    # hips 垂直偏移决定"脚会不会陷进地面"，那条用例 R1 盯的就是这件事。
    # P6 再加一条 verify_player_jump：poseJump 的重写（双脚离地 / 0.75 s）
    # 和 Jump 的初速是同一件事的两半。
    want={"verify_procanim", "verify_vmesh", "verify_player_feel",
          "verify_player_jump"})
# 2026-10-22 编辑器闪退：离屏预览把主场景地形拆了（详见 verify_preview_terrain
# 的文档头）。这两个文件的 owner 就是那一条 —— 谁改预览的资源生命周期或
# 地形实例的销毁时机，都必须重跑它。
# ⚠ 期望是**并集**不是"只有我"：CharacterWindow 早已被角色导入 / 角色窗口 /
#   i18n 覆盖率三条占着（它带 TR() 字符串），RenderTarget 被碰撞 Play 与 DRS
#   占着。判据是"自己那条一定在"，不是"别人不该在"。
run("editor/CharacterWindow.cpp → 预览与地形并存", OLD,
    with_change("editor/CharacterWindow.cpp"),
    # 2026-10-22 P4 起它也是 verify_char_locomotion 的 owner（按键槽 / 绑键
    # 捕获 / `CHAR-KEYBIND` 锚点都长在这个文件里）。
    # P6 再加一条 verify_char_preview：预览叠加层（胶囊线框 / gizmo / 选中框）、
    # 鼠标点选、位置-旋转-大小控件条 —— 整块都在这个文件里。
    want={"verify_character_import", "verify_character_window",
          "verify_i18n_coverage", "verify_preview_terrain",
          "verify_char_locomotion", "verify_char_preview"})
run("engine/render/RenderTarget.cpp → 预览与地形并存", OLD,
    with_change("engine/render/RenderTarget.cpp"),
    want={"verify_collision_play", "verify_drs", "verify_preview_terrain"})
# 2026-10-22 P4：移动状态机抽成 `engine/animation/Locomotion.*`，编辑器 Play
# 与 Game.exe 共用同一份。它错了的表现是"腿不动"或"停下不播 idle"——只有
# verify_char_locomotion 看得见，所以它必须是 owner 之一。
# ⚠ 期望里必然带 verify_anim_runtime / verify_character_import /
#   verify_player_character：它们都登记了 `engine/animation/**` 整层。
#   判据仍是"自己那条一定在"，不是"只有自己那条"。
run("engine/animation/Locomotion.cpp → 默认角色移动", OLD,
    with_change("engine/animation/Locomotion.cpp"),
    # 2026-10-22 P5 起多一条 owner：crossfade 的 `blendSeconds` 与"切 clip 时
    # 记下旧播放头"都长在这个文件里，verify_player_feel 的 R7 盯的就是它。
    # P6 再加 verify_player_jump：jump 态的进入/退出与落地回 idle 也在这台
    # 状态机里（跳跃物理在别处，但"什么时候算跳"在这儿）。
    want={"verify_char_locomotion", "verify_anim_runtime",
          "verify_character_import", "verify_player_character",
          "verify_player_feel", "verify_player_jump"})
# 2026-10-23 P11：默认角色换成 Alicia Solid（128 骨 / 4 段 clip，见
# docs/alicia_import.md）。**`kDefaultCharacterPath` 这个常量本身长在
# editor/EditorScene.cpp 里** —— "换默认角色"完全可以只改这一个文件、一点
# 都不碰 assets/models/characters/，所以这个文件的 owner 必须包含
# verify_char_locomotion（它的 C0/C1c 就是拿这个常量去对资产的）。
# ⚠ 期望是并集：同一文件还管 createPrimitiveOnGround 的落点（verify_player /
#   verify_collision_play / verify_drop_to_floor / verify_model_scale）和
#   一批 TR() 词条（verify_i18n_coverage）。
run("editor/EditorScene.cpp → 默认角色装配", OLD,
    with_change("editor/EditorScene.cpp"),
    want={"verify_char_locomotion", "verify_character_import",
          "verify_collision_play", "verify_drop_to_floor",
          # 2026-10-24：`spawnPlayerAtGameStart` 住在这个文件里 —— 编辑器 Play
          # 时才生成玩家实体（PLAYER-SPAWN 锚点），
          # verify_editor_play_terrain 的 T1b 就是断它。
          "verify_editor_play_terrain",
          "verify_i18n_coverage", "verify_model_scale", "verify_player",
          "verify_player_feel"})
# ⚠ 按键翻译表 `engine/ui/KeyCodes.*` **故意不做夹具**：`engine/ui/**` 在
#   `GLOBAL_GLOBS` 里，改它走全量，登记进 TRIGGERS 反而会在 --narrow 下
#   假跳过（check_regression_map 的 [E] 段会点名）。这里留一条反向断言，
#   把这个事实钉住 —— 哪天有人把 KeyCodes 从横切里摘出来，得知道要补映射。
run("engine/ui/KeyCodes.cpp → 横切（本来就全量，不该登记）", OLD,
    with_change("engine/ui/KeyCodes.cpp"),
    want=None)

print("\n[B] 快照覆盖范围（真实扫描仓库）")
s = rr.source_snapshot()
print("  条目数 = %d" % len(s))
bad = [k for k in s if k.startswith(("assets/scenes/", "assets/scripts/",
                                     "tools/", "docs/", ".workbuddy/",
                                     "build/"))]
print("  ⚠ 不该进快照的条目：%s" % (bad if bad else "（无）"))
if bad:
    fails.append("快照混进运行时数据")
for probe in ["engine/render/DecalBuffer.cpp", "assets/shaders/decal.frag",
              "editor/InspectorPanel.cpp", "CMakeLists.txt"]:
    if probe not in s:
        print("  ⚠ 快照缺 %s" % probe)
        fails.append("快照缺 " + probe)

print("\n[C] 映射覆盖率")
no_trig = rr.impact_analysis(OLD, dict(OLD))["no_trigger"]
print("  已登记 %d 例 / 共 %d 例" % (len([n for n in rr.DEFAULT
                                            if n in rr.TRIGGERS]),
                                     len(rr.DEFAULT)))
print("  无条件重跑（没登记 TRIGGERS）：%s" % (", ".join(no_trig) or "（无）"))
missing = [n for n in rr.DEFAULT if not os.path.exists(
    os.path.join(TOOLS, n + ".py"))]
if missing:
    print("  ⚠ DEFAULT 里有脚本不存在：%s" % missing)
    fails.append("DEFAULT 列了不存在的脚本")

print("\n[D] 死映射（写了不存在、又不带通配的路径）")
dead = []
for c, gl in rr.TRIGGERS.items():
    for g in gl:
        if any(ch in g for ch in "*?"):
            continue
        if not os.path.exists(os.path.join(REPO, g.replace("/", os.sep))):
            dead.append("%s: %s" % (c, g))
print("  %s" % (dead if dead else "（无）"))
if dead:
    fails.append("死映射")

print("\n[E] 全横切登记（globs 全落在 GLOBAL_GLOBS → --narrow 下会假跳过）")
fully_dead, redundant = [], []
for c, gl in rr.TRIGGERS.items():
    n_global = 0
    for g in gl:
        probe = g.replace("**", "probe/leaf").replace("*", "z").replace("?", "z")
        if rr._match_any(rr.GLOBAL_GLOBS, probe):
            n_global += 1
            redundant.append("%s: %s" % (c, g))
    if n_global == len(gl):
        fully_dead.append(c)
if redundant:
    print("  冗余的横切 glob（只是多余，无害）：")
    for r in redundant:
        print("      " + r)
print("  整条都指向横切文件的用例：%s" % (", ".join(fully_dead) or "（无）"))
if fully_dead:
    fails.append("全横切登记: " + ", ".join(fully_dead))

print("\n[F] RENDER_MODULES 完整性"
      "（engine/render 下每个文件要么在 GLOBAL_GLOBS、要么在 RENDER_MODULES）")
rnd = os.path.join(REPO, "engine", "render")
uncovered = []
for fn in sorted(os.listdir(rnd)):
    rel = "engine/render/" + fn
    if rr._match_any(rr.GLOBAL_GLOBS, rel):
        continue
    if rr._match_any(rr.RENDER_MODULES, rel):
        continue
    uncovered.append(rel)
print("  没被覆盖的 render 文件：%s"
      % (uncovered if uncovered else "（无 —— 新模块会被这里抓出来）"))
if uncovered:
    fails.append("RENDER_MODULES 漏列: " + ", ".join(uncovered))
leakgate = rr.TRIGGERS.get("verify_collision_play", [])
missing = [g for g in rr.RENDER_MODULES if g not in leakgate]
if missing:
    print("  ⚠ 泄漏门没挂上这些模块：%s" % missing)
    fails.append("泄漏门漏挂模块")

print("\n结果: %s" % ("PASS" if not fails else "FAIL: " + ", ".join(fails)))
sys.exit(1 if fails else 0)
