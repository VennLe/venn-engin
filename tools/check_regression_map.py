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
         "assets/game.json")


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
          "verify_script_editor"})
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
