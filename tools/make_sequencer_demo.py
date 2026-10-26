"""生成「过场时间轴」演示场景 —— 配套 `docs/cutscene_sequencer.md`。

==============================================================================
这个脚本存在的意义
==============================================================================
时间轴在 Inspector 里只是一段控件（路径 / 播放 / 速度 / 优先级 / 时间滑条），
单看每一项都懂，合起来"到底怎么让镜头动起来"却没人说得清。这个脚本把
**最终形态**直接烤成一份场景：打开就能看到运镜、`SEQ-EVENT` 按时刻发。

它同时是一份"可执行的文档"—— 想改哪一项，看下面那几个常量。

==============================================================================
跑法
==============================================================================
    python tools/make_sequencer_demo.py
    # 然后（要看过场，必须进 Play）
    build/bin/Editor.exe            # 启动器里选 assets/scenes/cutscene_demo.json

⚠ 需要先有 `anime_girl.fbx` 导入出来的模板（脚本会自己烤，约 20 秒）。
   手写 `skinned` 实体在视口里一个像素都不画，必须走 `MYVK_EDITOR_IMPORT` +
   `scene.save` 这条路（原因见 B1 的落地记录）。
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import verify_anim_layers as V                                  # noqa: E402

OUT = os.path.join(V.ROOT, "assets", "scenes", "cutscene_demo.json")

# ---- 演示内容（改这里就能改场景）----
#
# 时间轴资产本身在 `assets/sequences/opening.seq.json`（8 秒、5 个关键帧、
# 4 个事件、第 4 秒有一次硬切）。这里只负责"谁播它"。
SEQ_PATH = "sequences/opening.seq.json"
# 角色在过场里也要动起来，否则镜头绕着个站桩的人转很怪。
CLIP = "Walk"
# 优先级给高一点：将来同一场景里再挂一段"待机运镜"（低优先级循环），
# 这段开场应该在它之上。⚠ 与 CameraComponent::priority 是两回事。
PRIORITY = 10


def build_demo():
    if not os.path.isfile(V.TPL):
        print("模板不在，先烤一份（约 20 秒）…")
        V.bake_template(V.DEFAULT_EXE)

    with open(V.TPL, encoding="utf-8") as f:
        doc = json.load(f)

    root = None
    for e in doc["entities"]:
        if e.get("skinned"):
            root = e
            break
    if root is None:
        raise SystemExit("[FAIL] 模板里没有蒙皮实体")

    # ---- 角色：播一段循环走（不配层、不配根运动 —— 那是动画演示的事）----
    root["skinned"]["clipName"] = CLIP
    root["skinned"]["time"] = 0.0
    root["skinned"]["playing"] = True
    root["skinned"]["loop"] = True
    root["transform"]["rotation"] = [0.0, 0.0, 0.0]
    root["animator"] = {
        "defaultClip": CLIP,
        "controllerPath": "",
        "time": 0.0,
        "speed": 1.0,
        "playing": True,
        "loop": True,
        "rootMotion": 0,        # Off：这份资产是原地动画，开根运动也看不见
    }

    # ---- 导演轨：一个空物体，只挂 SequencerComponent ----
    # ⚠ 它必须**没有网格**。时间轴是"场记"不是"道具"，挂个网格上去
    #   会在画面正中多出一块东西（而且很容易被误当成"渲染坏了"）。
    doc["entities"].append({
        "name": "Director",
        "parent": -1,
        "visible": True,
        "castShadow": False,
        "locked": False,
        "mesh": None,
        "material": None,
        "transform": {"position": [0.0, 0.0, 0.0],
                      "rotation": [0.0, 0.0, 0.0],
                      "scale": [1.0, 1.0, 1.0]},
        "sequencer": {
            "path": SEQ_PATH,
            "enabled": True,
            "playing": True,
            "loop": False,
            "startTime": 0.0,
            "speed": 1.0,
            "priority": PRIORITY,
        },
    })

    # ---- 相机实体：**没有它，运镜一个像素都不会动** ----
    # `CameraSystem::update` 在"没有活跃相机"时直接 return，时间轴算出来的
    # 姿态被**静默丢弃**（日志里 SEQ-EVENT 照发，画面纹丝不动）。这是排查
    # 时间轴时第一个要排掉的可能，所以演示场景里显式放一台。
    #
    # ⚠ `collideWithWorld=false`：过场相机不该被弹簧臂的撞墙逻辑推着走
    #   （那是第三人称跟随相机的东西）。
    doc["entities"].append({
        "name": "CutsceneCam",
        "parent": -1,
        "visible": True,
        "castShadow": False,
        "locked": False,
        "mesh": None,
        "material": None,
        # ⚠ 位置刻意写在**原点**，与第一帧关键帧（5.5,-1.2,1.9）不同。
        #   这样"第一帧相机在 (5.5,-1.2,1.9)"就只可能来自时间轴 —— 如果
        #   两处写成同一个数，那条断言等于什么都没测（相机本来就在那）。
        "transform": {"position": [0.0, 0.0, 0.0],
                      "rotation": [0.0, 0.0, 0.0],
                      "scale": [1.0, 1.0, 1.0]},
        "camera": {
            "fov": 45.0,
            "priority": 0,
            "enabled": True,
            "targetOffset": [0.0, 0.0, 0.76],
            "distance": 4.0,
            "minDistance": 0.6,
            "collideWithWorld": False,
            "shakeAmplitude": 0.0,
            "shakeFrequency": 24.0,
        },
    })

    doc["generator"] = "make_sequencer_demo"
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as f:
        json.dump(doc, f, ensure_ascii=False, indent=1)
    return OUT


def main():
    path = build_demo()
    with open(path, encoding="utf-8") as f:
        n = len(json.load(f)["entities"])
    print("已生成 %s" % path)
    print("实体数 = %d（Ground + 角色根 + 7 个子网格 + Director + 相机）" % n)
    print()
    print("打开它并进 Play：")
    print('  MYVK_EDITOR_SCENE="%s" build/bin/Editor.exe'
          % os.path.relpath(path, V.ROOT).replace("\\", "/"))
    print()
    print("应当看到的日志（下面四条缺任何一条都说明某一段没接上）：")
    print("  SEQ-ASSET name=opening dur=8.000 loop=0 keys=5 events=4")
    print("  SEQ-EVENT ... kind=Fade     t=0.0000 payload=[in]     ← 首帧就发")
    print("  SEQ-EVENT ... kind=Footstep t=1.2000 payload=[left]")
    print("  SEQ-EVENT ... kind=Line     t=4.0000 payload=[ch01_open_01]")
    print("  SEQ-EVENT ... kind=Music    t=6.5000 payload=[calm_loop]")
    print()
    print("CAM-POSE pos=(...) 每帧一行 ← 相机真的被时间轴推着走（需 --trace-states）")
    print()
    print("踩坑速查：")
    print("  只有 SEQ-ASSET 没有 SEQ-EVENT → Director 的 playing 为假，或路径填错")
    print("  SEQ-EVENT 在发但画面不动   → 场景里没有 CameraComponent 实体")
    print("                                （没有活跃相机时姿态被静默丢弃）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
