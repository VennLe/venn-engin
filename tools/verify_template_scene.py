"""端到端验证：新工程初始场景 = 空场景 + 土黄地形 + GameStart 出生点。

对应 2026-10-21 第八轮的三条产品要求里后两条：

  2. "游戏创建成功后初始化的场景不需要摆放任何物体，但地面要初始化一个
     完整的地形，颜色是土黄色。"
  3. "场景中心摆放一个 GameStart 组件，标记玩家初始化位置；视口里用简单
     粗糙的 Q 版人物模型呈现。"

断言分五组：

  [1] 场景结构：从模板建工程 → 场景 JSON 里**只有** Terrain + GameStart
      两个实体（没有任何道具），两者都没有 mesh。
  [2] 地形：vterrain 引用的资产存在、二进制尺寸对、4 层都指向土黄材质，
      且该材质 baseColor 是"土黄"（R>G>B 且 R 明显占优）。
  [3] 编辑器：地形真的被加载（TERRAIN-LOAD + 4 条 TERRAIN-LAYER），
      且出生点标记被画出来（GAMESTART-MARK 矩形落在视口内）。
  [4] 运行时：Game.exe 打出 `GAME-PLAYER spawned at GameStart found=1`，
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
CAM = "0,-7,4,90,14"      # 能同时看到地形起伏与出生点小人


def parse_rect(line):
    m = re.search(r"=\((-?[\d.]+),(-?[\d.]+)\)-\((-?[\d.]+),(-?[\d.]+)\)",
                  line)
    if not m:
        return None
    return tuple(float(v) for v in m.groups())


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
    print("新工程初始场景：空场景 + 土黄地形 + GameStart")
    print("=" * 68)

    # ================= 1) 建工程 + 场景结构 =================
    root = os.path.join(kit.WORK_ROOT, "template_scene", KIND)
    name = "SceneCheck"
    kit.make_project(root, KIND, name=name)
    assets = os.path.join(root, "assets")
    scene_path = os.path.join(assets, "scenes", "main.json")

    print("\n[1] 场景结构（不应摆放任何物体）")
    if not os.path.isfile(scene_path):
        print("  [FAIL] 场景没生成：%s" % scene_path)
        return 1
    doc = json.load(open(scene_path, encoding="utf-8"))
    ents = doc.get("entities", [])
    names = [e.get("name") for e in ents]
    check("场景只有 2 个实体（地形 + 出生点）", len(ents) == 2,
          "实际: %s" % names)
    kinds = []
    for e in ents:
        if "terrain" in e:
            kinds.append("terrain")
        elif "gameStart" in e:
            kinds.append("gameStart")
        elif "mesh" in e:
            kinds.append("mesh")
        else:
            kinds.append("other")
    check("实体是 terrain + gameStart", sorted(kinds) == ["gameStart", "terrain"],
          "实际: %s" % kinds)
    check("场景里没有任何网格物体（无道具）", "mesh" not in kinds)
    gs = next((e for e in ents if "gameStart" in e), None)
    check("GameStart 在场景中心 (0,0,0)",
          gs is not None and
          [round(v, 3) for v in gs["transform"]["position"]] == [0, 0, 0],
          "" if gs is None else str(gs["transform"]["position"]))
    check("GameStart 默认启用",
          gs is not None and gs["gameStart"].get("enabled", True) is True)

    # ================= 2) 地形资产 + 土黄色 =================
    print("\n[2] 地形资产")
    ter = next((e for e in ents if "terrain" in e), None)
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
    import struct as _struct
    hp = os.path.join(os.path.dirname(vpath), vj.get("height", "height.r32"))
    if os.path.isfile(hp):
        n = cols * rows
        with open(hp, "rb") as f:
            hs = _struct.unpack("<%df" % n, f.read(4 * n))
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

    # ================= 3) 编辑器：地形 + 标记 =================
    print("\n[3] 编辑器加载")
    kill_stale_editors()
    env = kit.clean_env({
        "MYVK_ASSET_DIR": assets,
        "MYVK_EDITOR_SCENE": "scenes/main.json",
        "MYVK_NO_WINDOW_SAVE": "1",
        "MYVK_LOG_RECTS": "1",
        "MYVK_EDITOR_CAM": CAM,
    })
    proc = subprocess.Popen([kit.EDITOR], cwd=root, env=env,
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
            check("地形被加载（TERRAIN-LOAD）", "TERRAIN-LOAD" in txt)
            n_layer = len(re.findall(r"TERRAIN-LAYER idx=", txt))
            check("4 层地形材质都解析到了", n_layer == 4, "n=%d" % n_layer)
            check("场景里没有光球/传送门（无 PICKUP/PORTAL 实体）",
                  "Pickup_" not in txt and "Portal" not in txt)

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
