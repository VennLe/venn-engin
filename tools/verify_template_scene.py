"""端到端验证：新工程初始场景 = 二次元小镇 + 出生点。

2026-11-01 换的判据。在此之前这条用例守的是 2026-10-21 那轮的要求
（"游戏创建成功后初始化的场景不需要摆放任何物体，但地面要初始化一个完整
的地形，颜色是土黄色"）。用户看过小镇之后改主意了，原话是：

    "把原来的那个游戏初始化场景移除掉，用这个新的替代"

于是现在：`assets/scenes/main.json` = 小镇（街道 / 12 栋房子 / 树 / 灌木 /
草地）+ `GameStart`；原来那张空地形场景降级成 `assets/scenes/terrain.json`，
只作为地形特性的夹具与用法样例存在。

断言分五组：

  [1] 初始场景结构（`main.json`）：实体数够多、五类东西都在（Ground / Road /
      House / Tree / Bush）、GameStart 在街心且 `mode` 跟模板类型走、
      `environment` 块完整、**零外部资产引用**。
      ⚠ 最后那条守的是 D6 纪律"模板不允许引用 build/bin/assets 里的模型/
        材质"：小镇的房子是 `kind: builtin` 的 cube 拼的、材质是
        `kind: procedural` 内联的 —— 所以模板工程**一个路径都不用带**。
        哪天有人把房子换成真实 .glb，这条会红，那正是它存在的意义。
  [2] 地形样例还在（`terrain.json`）：vterrain 存在、257x257、四层土黄材质、
      出生点处地表高度为 0。这条同时是"地形特性用例的夹具没被删"的哨
      （`verify_terrain_renorm` 直接打开这份场景）。
  [3] 编辑器：打开工程 → 场景真的加载（实体数与 JSON 对得上，不是空场景
      兜底）+ 出生点标记画在视口内。
  [4] 运行时：`Game.exe` 打出 `GAME-PLAYER spawned at GameStart found=1`，
      并且玩家位置 == GameStart 位置（不是回落到原点）。
  [5] 干净：编辑器与运行时都没有 VUID / ERROR。

用法：
    python tools/verify_template_scene.py
"""

import argparse
import json
import os
import re
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import launcher_kit as kit                                    # noqa: E402
from capture_window import capture_client, find_window, user32  # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors        # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(REPO, "..", "_venn_verify")
KIND = "third_person"
# 客户区坐标 + 角度：站在街心南侧 12 m、抬高 4 m，正对 +Y（街道纵深）。
# 这样一条街、两侧房子、天和街心那个出生点小人同时在画面里。
CAM = "0,-12,4,90,12"

# `environment` 块必须带全的字段（`SkyParams::FromJson` 认这些名字）。
ENV_KEYS = [
    "zenith", "horizon", "ground", "cloudColor", "cloudCoverage",
    "cloudThickness", "cloudScale", "cloudHeight", "cloudSpeed",
    "cloudShadow", "cloudEdgeSharpness", "exposure", "starAmount",
    "starBrightness", "sunDirection", "sunIntensity", "sunAngularRadius",
    "horizonGlow",
]


def parse_rect(line):
    m = re.search(r"=\((-?[\d.]+),(-?[\d.]+)\)-\((-?[\d.]+),(-?[\d.]+)\)",
                  line)
    if not m:
        return None
    return tuple(float(m.group(i)) for i in (1, 2, 3, 4))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    if not os.path.isfile(kit.EDITOR):
        print("结果: FAIL: Editor.exe 不存在（先 make build）")
        return 1

    os.makedirs(OUT_DIR, exist_ok=True)
    fails = []

    def check(label, ok, extra=""):
        print("  %s  %s%s" % ("[PASS]" if ok else "[FAIL]", label,
                              ("  " + extra) if extra else ""))
        if not ok:
            fails.append(label)

    print("=" * 68)
    print("新工程初始场景：二次元小镇 + GameStart 出生点")
    print("=" * 68)

    # ================= 1) 建工程 + 初始场景结构 =================
    root = os.path.join(kit.WORK_ROOT, "template_scene", KIND)
    name = "SceneCheck"
    kit.make_project(root, KIND, name=name)
    assets = os.path.join(root, "assets")
    scene_path = os.path.join(assets, "scenes", "main.json")

    print("\n[1] 初始场景结构（小镇 + 出生点）")
    if not os.path.isfile(scene_path):
        print("  [FAIL] 场景没生成：%s" % scene_path)
        return 1
    doc = json.load(open(scene_path, encoding="utf-8"))
    ents = doc.get("entities", [])
    names = [e.get("name", "") for e in ents]

    # 157 = 1 地面 + 26 路砖 + 12 栋 × 6 件 + 12 树 + 24 灌木 + 1 出生点。
    # 只判"≥ 100"：以后加几栋房子不该让这条用例红。
    check("实体数够多（不是空场景）", len(ents) >= 100, "实际 %d" % len(ents))

    def count_prefix(p):
        return sum(1 for n in names if n.startswith(p))

    houses = set()
    for n in names:
        m = re.match(r"House(\d+)_", n)
        if m:
            houses.add(m.group(1))
    check("有 12 栋房子", len(houses) == 12, "House* 实体 %d 件 / 房子 %d 栋"
          % (count_prefix("House"), len(houses)))
    check("有主路（Road*）", count_prefix("Road") >= 20,
          "%d 块路砖" % count_prefix("Road"))
    check("有地面（Ground）", "Ground" in names)
    check("有植被（Tree* / Bush*）",
          count_prefix("Tree") >= 5 and count_prefix("Bush") >= 10,
          "树 %d / 灌木 %d" % (count_prefix("Tree"), count_prefix("Bush")))

    gs = next((e for e in ents if "gameStart" in e), None)
    check("有出生点（GameStart）", gs is not None)
    if gs:
        check("出生点在街心 (0,0,0)",
              [round(v, 3) for v in gs["transform"]["position"]] == [0, 0, 0],
              str(gs["transform"]["position"]))
        check("GameStart 默认启用",
              gs["gameStart"].get("enabled", True) is True)
        # ⚠ mode 写错不会报错，只会"一按 Play 就变第三人称"（2026-10-24 踩过）
        want = "first" if KIND.startswith("first") else "third"
        check("GameStart.mode 跟模板类型走", gs["gameStart"].get("mode") == want,
              "%s（期望 %s）" % (gs["gameStart"].get("mode"), want))
        check("出生点挂了世界逻辑脚本",
              gs.get("script", {}).get("path") == "scripts/main.vks",
              str(gs.get("script", {}).get("path")))

    env = doc.get("environment")
    check("有 environment 块（天空不再是引擎默认）", isinstance(env, dict))
    if isinstance(env, dict):
        miss = [k for k in ENV_KEYS if k not in env]
        check("environment 18 个字段齐全", not miss, "缺 %s" % miss if miss else "")
        cov = env.get("cloudCoverage", 0.0)
        check("云量 > 0（天上真的有云）", cov > 0.1, "cloudCoverage=%.2f" % cov)

    # ---- 零外部资产引用（D6 纪律）----
    bad_ref = []
    for e in ents:
        mesh = e.get("mesh")
        if isinstance(mesh, dict) and mesh.get("kind") != "builtin":
            bad_ref.append("%s.mesh=%s" % (e.get("name"), mesh.get("kind")))
        matr = e.get("material")
        if isinstance(matr, dict) and matr.get("kind") != "procedural":
            bad_ref.append("%s.material=%s" % (e.get("name"), matr.get("kind")))
    check("场景自包含：mesh 全是 builtin、material 全是 procedural",
          not bad_ref, bad_ref[0] if bad_ref else "156 件全是内联图元")

    check("初始场景里没有地形实体（小镇用的是 Ground 平面）",
          not any("terrain" in e for e in ents))
    check("物理开着（groundPlane）",
          doc.get("physics", {}).get("enabled") is True and
          doc.get("physics", {}).get("groundPlane") is True)

    # ================= 2) 地形样例还在（不是初始场景）=================
    print("\n[2] 地形样例场景（scenes/terrain.json）")
    tpath = os.path.join(assets, "scenes", "terrain.json")
    check("terrain.json 存在", os.path.isfile(tpath))
    tdoc = json.load(open(tpath, encoding="utf-8")) \
        if os.path.isfile(tpath) else {}
    tents = tdoc.get("entities", [])
    ter = next((e for e in tents if "terrain" in e), None)
    check("terrain.json 里有地形实体", ter is not None,
          "实体 %s" % [e.get("name") for e in tents])
    src = ter["terrain"]["source"] if ter else ""
    vpath = os.path.join(assets, src.replace("/", os.sep))
    check("vterrain 存在", os.path.isfile(vpath), src)
    vj = json.load(open(vpath, encoding="utf-8")) if os.path.isfile(vpath) else {}
    cols, rows = vj.get("cols", 0), vj.get("rows", 0)
    check("地形网格是 257x257（一整块完整地形）",
          cols == 257 and rows == 257, "%dx%d" % (cols, rows))
    for f, expect in (("height.r32", cols * rows * 4),
                      ("splat.rgba8", cols * rows * 4)):
        p = os.path.join(os.path.dirname(vpath), f)
        got = os.path.getsize(p) if os.path.isfile(p) else -1
        check("%s 尺寸正确" % f, got == expect, "%d / 期望 %d" % (got, expect))

    # ---- 出生点处地表高度 == 0（2026-10-22 第十轮）----
    #
    # ⚠⚠ 这是"新工程一进来玩家陷进土里 3.35 m"那个 bug 的**回归哨**。
    #   当时的成因是归一化基准不一致：`terrain_height()` 把原点压平成 0，
    #   而 `build_terrain_bytes()` 用**全局 min..max** 归一化，于是原点那个
    #   0 被抬到 0.5578，乘 maxHeight 之后地面跑到 3.35 m —— 而 GameStart
    #   写的是 z=0，人就直接站在土里了。
    #   契约：**原点处的归一化值恰好是 0**，且 `maxHeight` 等于地形真正的
    #   最高点（值域 [0, hi] 才自洽，否则坡度全都虚高）。
    hp = os.path.join(os.path.dirname(vpath), vj.get("height", "height.r32"))
    if os.path.isfile(hp):
        n = cols * rows
        with open(hp, "rb") as f:
            hs = struct.unpack("<%df" % n, f.read(4 * n))
        mpp = vj.get("metersPerPixel", 0.0)
        maxh = vj.get("maxHeight", 0.0)
        # 与 C++ `physics::heightAt` 同源：世界原点 → 列主序纹素下标
        oc = int(round((0.0 - (-0.5 * (cols - 1) * mpp)) / mpp))
        orow = int(round((0.0 - (-0.5 * (rows - 1) * mpp)) / mpp))
        h0 = hs[oc * rows + orow] * maxh
        check("出生点 (0,0) 处地表高度 == 0（玩家不会陷进地面）",
              abs(h0) < 1e-3, "%.4f m（maxHeight=%.4f）" % (h0, maxh))
        check("高度值域真的落在 [0, 1]", min(hs) >= -1e-6 and max(hs) <= 1.0 + 1e-6,
              "min=%.4f max=%.4f" % (min(hs), max(hs)))
        check("归一化最高点 == 1.0（maxHeight 是实测峰值，不是硬编码常量）",
              abs(max(hs) - 1.0) < 1e-3,
              "最高纹素=%.6f" % max(hs))

    layers = vj.get("layers", [])
    check("4 层都指向同一材质", len(layers) == 4 and len(set(layers)) == 1,
          str(layers))
    mat_path = os.path.join(assets, (layers[0] if layers else "") + ".vmat")
    check("地形材质文件存在", os.path.isfile(mat_path),
          os.path.basename(mat_path))
    mat = json.load(open(mat_path, encoding="utf-8")) \
        if os.path.isfile(mat_path) else {}
    bc = mat.get("baseColor", [0, 0, 0, 1])
    # 土黄 = 暖色、R 明显占优、G 居中、B 低
    check("材质是土黄色（R>G>B 且 R 明显占优）",
          bc[0] > bc[1] > bc[2] and bc[0] > bc[2] + 0.2 and bc[0] > 0.3,
          "baseColor=%s" % [round(v, 3) for v in bc[:3]])
    # ⚠ 贴图是"地面看着糊"的根治手段：纯色材质在大面积上就是一片渐变
    #   （用户 2026-10-21 报的"模糊重影"）。albedo / normal 缺一张都要红。
    for slot in ("albedo", "normal"):
        tex = mat.get(slot)
        ok = isinstance(tex, dict) and tex.get("kind") == "file"
        tp = ""
        if ok:
            tp = tex.get("path", "")
            fp = os.path.join(assets, tp.replace("/", os.sep))
            ok = os.path.isfile(fp)
        check("材质带 %s 贴图且文件存在" % slot, ok,
              tp if tp else "（缺 %s 槽）" % slot)

    # ================= 3) 编辑器：场景真的加载 =================
    print("\n[3] 编辑器加载")
    kill_stale_editors()
    env_vars = kit.clean_env({
        "MYVK_ASSET_DIR": assets,
        "MYVK_EDITOR_SCENE": "scenes/main.json",
        "MYVK_NO_WINDOW_SAVE": "1",
        "MYVK_LOG_RECTS": "1",
        "MYVK_EDITOR_CAM": CAM,
        # 关掉栅格与 HUD：小镇本身就有 26 块路砖，叠一层栅格看不出路
        "MYVK_EDITOR_NO_GRID": "1",
    })
    proc = subprocess.Popen([kit.EDITOR], cwd=root, env=env_vars,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    hwnd = None
    try:
        hwnd = find_window("Venn Editor", timeout=45.0)
        if not hwnd:
            check("编辑器窗口出现", False)
        else:
            user32.SetWindowPos(hwnd, 0, 0, 0, 1280, 800, 0x0002 | 0x0004)
            time.sleep(4.5)
            with tap.lock:
                lines = list(tap.lines)
            txt = "\n".join(lines)

            # "Editor: auto-opened scene '<path>' (N entities)" —— 实体数
            # 是最硬的证据：空场景兜底时会打另一条日志（"no default scene"）。
            #
            # ⚠ 期望是 **JSON 实体数 + 1**：`Scene::lightEntity()` 会把场景的
            #   `light` 块懒创建成一个**实体**，而它不在 `entities[]` 里。
            #   写成"≥ N"就成了永远为真的假绿 —— 这里刻意写死等号
            #   （同一条纪律见 `verify_launcher_project` 的 ①）。
            m = re.search(r"auto-opened scene '.*?' \((\d+) entities\)", txt)
            want_n = len(ents) + 1
            check("初始场景被打开且实体数对得上",
                  m is not None and int(m.group(1)) == want_n,
                  "日志 %s / 期望 %d（JSON %d + 场景太阳 1）"
                  % (m.group(1) if m else "（没有这行）", want_n, len(ents)))

            mark = None
            for l in reversed(lines):
                if "GAMESTART-MARK" in l:
                    mark = parse_rect(l)
                    break
            check("出生点标记被画出来（GAMESTART-MARK）", mark is not None)
            if mark:
                vp = None
                for l in reversed(lines):
                    if "VP-RECT" in l:
                        vp = parse_rect(l)
                        break
                inside = (vp is None or
                          (mark[0] >= vp[0] - 1 and mark[2] <= vp[2] + 1 and
                           mark[1] >= vp[1] - 1 and mark[3] <= vp[3] + 1))
                check("标记落在视口内（不是画到面板上）", inside,
                      "mark=%s vp=%s" % (mark, vp))
                w = mark[2] - mark[0]
                h = mark[3] - mark[1]
                check("标记有可读大小（不是一条线）",
                      w > 20 and h > 40, "%.0fx%.0f" % (w, h))

            img, ok = capture_client(hwnd)
            img.save(os.path.join(OUT_DIR, "tpl_scene_editor.png"))
    finally:
        if hwnd:
            user32.PostMessageW(hwnd, 0x0010, 0, 0)
        time.sleep(0.6)
        if proc.poll() is None:
            proc.terminate()

    # ================= 4) 运行时：按出生点生成玩家 =================
    print("\n[4] 运行时出生")
    game = os.path.join(os.path.dirname(kit.EDITOR), "Game.exe")
    genv = kit.clean_env({
        "MYVK_ASSET_DIR": assets,
        "MYVK_SCENE": "scenes/main.json",
        "MYVK_RUNTIME_PLAYER_DUMP": "30",
    })
    gp = subprocess.run([game, "--frames", "120"],
                        cwd=root, env=genv,
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        text=True, encoding="utf-8", errors="replace",
                        timeout=180)
    out = gp.stdout or ""
    m = re.search(r"GAME-PLAYER spawned at GameStart found=(\d+) "
                  r"feet=\(([-\d.]+),([-\d.]+),([-\d.]+)\)", out)
    check("运行时按 GameStart 生成玩家", m is not None and m.group(1) == "1",
          "" if m else "（没有 GAME-PLAYER spawned 行）")
    if m:
        feet = [float(m.group(i)) for i in (2, 3, 4)]
        check("玩家出生在 GameStart 位置（0,0,0）",
              all(abs(v) < 0.01 for v in feet), str(feet))
    check("玩家被 PlayerController 接管", "GAME-PLAYER attached" in out)

    # ================= 5) 干净 =================
    print("\n[5] 日志干净")
    bad = [l for l in out.splitlines()
           if "VUID-" in l or "[ERROR" in l]
    check("运行时零 VUID / 零 ERROR", not bad,
          bad[0][:120] if bad else "")

    print("\n" + "=" * 68)
    if fails:
        print("结果: FAIL（%d 项）" % len(fails))
        for f in fails:
            print("  · " + f)
        return 1
    print("结果: PASS")
    if not args.keep:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
