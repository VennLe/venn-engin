"""生成「二次元角色动画」演示场景 —— 配套 `docs/character_animation_guide.md`。

==============================================================================
这个脚本存在的意义
==============================================================================
引擎的动画能力（A1 状态机 / B1 分层遮罩 / B2 动画事件 / B3 根运动）在
Inspector 里是一段一段的控件，单看每一段都懂，合起来"到底要怎么配才能
做出边走边挥手"却没人说得清。这个脚本把**最终形态**直接烤成一份场景：
打开编辑器就能看到它、Inspector 里每一段都是填好的、日志里每个锚点都有。

它同时是一份"可执行的文档"—— 想改哪一项，看 `build_demo()` 里那几行。

==============================================================================
跑法
==============================================================================
    python tools/make_anim_demo.py
    # 然后
    build/bin/Editor.exe            # 启动器里选 assets/scenes/anime_demo.json

⚠ 需要先有 `anime_girl.fbx` 导入出来的模板（脚本会自己烤，约 20 秒）。
   模板是 `_venn_verify/anim_layers_tpl.json`（verify_anim_layers 的中间
   产物）—— **手写 `skinned` 实体在视口里一个像素都不画**，必须走
   `MYVK_EDITOR_IMPORT` + `scene.save` 这条路，原因见 B1 的落地记录。
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import verify_anim_layers as V                                  # noqa: E402

OUT = os.path.join(V.ROOT, "assets", "scenes", "anime_demo.json")

# ---- 演示用的角色资产 ----
#
# 2026-10-29 换掉了原来那个 `assets/models/characters/animate_girl.fbx`：那是 1181 个断壳
# 拼出来的低模，用户的原话是"太粗糙了不能用"。
#
# 新资产 = `E:\dev\UE-resources\Animate_Girl` 那个 27394 顶点的雕塑
# + Mixamo 65 骨骨架，由 `tools/rebuild_skin_weights.py` 重写了蒙皮权重、
# 并把根节点转成 Z-up（引擎对蒙皮网格不吃 `kYupToZup`，见
# `docs/animate_girl_import.md` §5）。
#
# ⚠ **`_rigged` 后缀不是可选的**：同目录的 `animate_girl.glb` 是没修权重、
#   没转轴的半成品 —— 喂进来会得到一个躺在地上、四肢被拉长的角色。
#
# ⚠ 模板必须**独立一份**（`anim_demo_tpl.json`），不能和
#   `verify_anim_layers.TPL` 共用：那条用例验的仍是旧资产，共用的话谁后跑
#   谁把对方那份覆盖掉，"验的是哪个模型"就取决于执行顺序了。
DEMO_MODEL = "assets/models/characters/animate_girl_rigged.glb"
DEMO_TPL = os.path.join(V.OUT_DIR, "anim_demo_tpl.json")
# 演示场景的机位。`verify_anim_layers.TPL_CAM`（"3,-3,1.5,45,19.5"）是按旧角色
# 1.556 m 定的，换成 1.626 m 的新角色之后那个机位会把人大半截裁出画面。
# 拉远到 5.5、俯角降到 12°，整个角色连脚下阴影一起框进来。
DEMO_CAM = "5.5,-5.5,1.8,45,12"

# ---- 演示内容（改这里就能改场景）----
#
# 底座相位 0.30 s、层相位 0 s：**这是刻意错开的**。层的 `time` 不落盘
# （运行时字段，读档重置为 0），而底座的 `time` 会写进文件 —— 于是
# "下半身在动画的第 0.30 秒、上半身在 0.00 秒"，两者不同步，遮罩一开
# 就能一眼看出"上半身被另一份姿势接管了"。
# ⚠ 想让两层同步，就把 BASE_TIME 设成 0（那就看不出层在起效）。
#
# ⚠⚠ 层**不要**用和底座同一段 clip：事件表是**按 clip 名**找的，两层播
#   同一段 → 同一个事件每帧发两遍（一条 `layer=[]`、一条 `layer=[层名]`）。
#   实测过（本脚本第一版就是 Walk + Walk，一圈吐出 4 条而不是 2 条）。
#   层的 clip 用另一段就没有这个问题 —— 因为那张表只属于底座那段。
BASE_CLIP = "Walk"
BASE_TIME = 0.30
LAYER_CLIP = "Idle"
LAYER_WEIGHT = 0.75
# 只接管胸腔以上（头 / 双臂 / 头发）。
#
# ⚠ 骨名跟着资产走：旧资产叫 `chest`，Mixamo 骨架里同一根骨是
#   `mixamorig:Spine2`。`buildBoneMask` 用的是**去大小写子串匹配**
#   （`name.find(want)`），所以这里写 `Spine2` 就够 —— 但**不能**写
#   `Spine`：那会同时命中 Spine / Spine1 / Spine2 三根，等于把半个躯干
#   也算进"上半身"，遮罩就看不出边界了。
LAYER_ROOTS = ["Spine2"]
# 脚步事件：一左一右。t 必须**小于** clip 时长，否则永远不发。
# （Walk 时长 1.0000 s，两个 t 都落在里面。）
EVENTS = [(0.15, "Footstep", "left"), (0.65, "Footstep", "right")]
# 根运动：Delta only = "只算位移给物理，引擎不替它移动"。
#
# ⚠ 这里的行为**换资产时就变了**，别照抄旧日志：
#   · 旧 `anime_girl.fbx` 的 Walk 是原地动画 → delta 恒为 0，
#     日志是 `cfg=1 n=0 bone=hips`（"配了根运动但角色不走"）。
#   · 新资产的 Walk 带约 1.79 m 的前进位移（Mixamo `Walking` 烘帧时没做
#     in-place）→ `n>0`，真的会算出行程。
#   配置值仍是 2（Delta only），所以**引擎不会替它移动**这个前提没变：
#   视觉上看到的是"骨骼带着身体在原地来回走"，实体本身留在原点。
ROOT_MOTION = 2


def build_demo():
    if not os.path.isfile(DEMO_TPL):
        print("模板不在，先烤一份（约 20 秒）…")
        V.bake_template(V.DEFAULT_EXE, DEMO_MODEL, DEMO_TPL, DEMO_CAM)

    with open(DEMO_TPL, encoding="utf-8") as f:
        doc = json.load(f)

    root = None
    for e in doc["entities"]:
        if e.get("skinned"):
            root = e
            break
    if root is None:
        raise SystemExit("[FAIL] 模板里没有蒙皮实体")

    # ---- 底座：权威字段写在 animator 上（skinned 那一份只做镜像）----
    root["skinned"]["clipName"] = BASE_CLIP
    root["skinned"]["time"] = BASE_TIME
    root["skinned"]["playing"] = False
    root["skinned"]["loop"] = True
    root["transform"]["rotation"] = [0.0, 0.0, 0.0]

    root["animator"] = {
        "defaultClip": BASE_CLIP,
        "controllerPath": "",       # 空 = 单 clip 直放（状态机在 A2）
        "time": BASE_TIME,
        "speed": 1.0,
        "playing": True,
        "loop": True,
        "rootMotion": ROOT_MOTION,
        # 附加层（B1）：骨骼遮罩用"根骨 + 父链子树"，不是一个一个列骨名。
        # `mode=1` 是 Allow —— 只接管这些根的子树。
        "layers": [{
            "name": "Upper body",
            "clip": LAYER_CLIP,
            "weight": LAYER_WEIGHT,
            "speed": 1.0,
            "loop": True,
            "playing": True,
            "fadeDur": 0.15,
            "mask": {"mode": 1, "roots": LAYER_ROOTS},
        }],
        # 动画事件（B2）：按 clip 分表，每张表按时间升序。
        "events": [{
            "clip": BASE_CLIP,
            "events": [{"t": t, "kind": k, "payload": p}
                       for (t, k, p) in EVENTS],
        }],
    }

    doc["generator"] = "make_anim_demo"
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as f:
        json.dump(doc, f, ensure_ascii=False, indent=1)
    return OUT


def main():
    path = build_demo()
    with open(path, encoding="utf-8") as f:
        n = len(json.load(f)["entities"])
    print("已生成 %s" % path)
    print("实体数 = %d（Ground + 角色根 + 7 个子网格）" % n)
    print()
    print("打开它：")
    print("  build/bin/Editor.exe            # 启动器里选 %s"
          % os.path.basename(path))
    print("或者直接命令行：")
    print('  MYVK_EDITOR_SCENE="%s" build/bin/Editor.exe'
          % os.path.relpath(path, V.ROOT).replace("\\", "/"))
    print()
    print("应当看到的日志：")
    print("  ANIM-LAYER configured=1 applied=1        ← 遮罩真的混进去了")
    print("  ANIM-MASK mode=Allow                     ← 遮罩解算跑了")
    print("  ANIM-EVENT ... kind=Footstep t=0.1500    ← 脚步事件在发")
    print("  ANIM-ROOTMOTION cfg=1 n=>0 bone=hips     ← 配了 Delta only：")
    print("                                             算出位移但引擎不替它走")
    print("                                             （换资产前这里是 n=0）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
