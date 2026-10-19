#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/gen_templates.py —— 生成 templates/{first_person,third_person}/

启动器路线图 D6：「游戏类型 = 模板包，不是代码里的 if 分支」。
加一种类型（俯视角 / 横版 / 载具）= **加一个目录**，不改一行 C++。

这个脚本是**确定性**的：同样的输入跑两次，产出的字节完全一致
（json 用 sort_keys 之外的固定构造顺序 + indent=2 + 强制 \\n）。
所以 `templates/` 可以随仓库提交，改模板只改这一个脚本。

------------------------------------------------------------
两条刻意的设计（与 launcher_roadmap.md 原文的差异，已记进 §11 台账）
------------------------------------------------------------
① **模板不生成 `assets/input_map.json`**。
   `editor/InputMapIO.h` 明写"刻意不在仓库里塞一份 input_map.json 当作
   默认资产：那样出厂表就会被钉死"。模板同理 —— 拷一份进工程等于让
   Preferences ▸ Bindings 的"恢复默认"变成"恢复这份拷贝"。
   引擎在文件缺失时用 `InputMap::makeDefault()`，这是**正常状态**。

② **模板材质全部用 `kind: "procedural"`**，不引用任何 `.vmat`。
   D6 要求"模板不允许引用 build/bin/assets 里的模型/材质"，并建议用
   "扫模板里所有 path 字段"来自检。用 procedural 更彻底：模板里
   **一个路径都没有**，自检不必猜哪些路径算"工程外"。
   场景因此完全自包含 —— 把 build/bin/assets 整个改名，模板工程照样能开。

③ **模板不写 `config/render.json` 的开关集合**，只放一个 `{}`。
   E03 的纪律是"无文件 = 零动作、默认全关"。画质由创建向导的 Step 3
   写入（M4）——模板是"骨架"，画质是"用户选择"，两者不该混。

用法：
    python tools/gen_templates.py            # 生成
    python tools/gen_templates.py --check    # 只校验（不写盘），CI 用
"""

import argparse
import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_ROOT = os.path.join(REPO, "templates")

# ============================================================
# 两种游戏类型的差异——每一行都能在 PlayerComponent 里找到依据
# ============================================================
# `launcher_roadmap.md` §4 的差异清单，这里就是它的**数据化**。
# ⚠ 只放"玩家/相机"相关的差异。渲染/画质不在这里（那是 render.json 的事）。
TYPES = {
    "first_person": {
        "label": "First person",
        "summary": "相机在眼睛位置，鼠标直接转视角",
        "player": {
            # mode 决定相机怎么摆（SceneSerializer.cpp:1933 的 "first"/"third"）
            "mode": "first",
            "eyeHeight": 1.7,        # 眼睛离"脚"的高度
            "moveSpeed": 3.0,
            "lookSpeed": 0.0025,
            "yaw": 0.0,
            "pitch": 0.0,            # 平视
            "camDistance": 0.0,      # 第一人称不参与
            "turnSpeed": 12.0,       # 第一人称不参与（保留默认，便于切换）
        },
        # 场景相机：第一人称由 PlayerController 每帧摆到眼位，
        # 这里的值只是"编辑器里第一次打开时"的观感。
        "camera": {"distance": 8.0, "pitch": -12.0, "targetOffset": [0.0, 0.0, 1.0]},
        "body_visible": True,        # 占位胶囊（第一人称的实体本身还是要有）
        "script": "first_person",
    },
    "third_person": {
        "label": "Third person",
        "summary": "相机在角色斜后方，鼠标环绕 + 角色转身",
        "player": {
            "mode": "third",
            "eyeHeight": 1.7,
            "moveSpeed": 3.0,
            "lookSpeed": 0.0025,
            "yaw": 0.0,
            # ⚠ -0.35 = 视线略微向下 → 相机落到角色斜上方俯视。
            #   驱动相机时传的是 -pitch（见 EditorApp::runPlayers），
            #   所以负数在这里是"低头"、在相机那里是"俯视"。
            "pitch": -0.35,
            # 默认 3.5 拍不到全身，模板给宽一点
            "camDistance": 4.5,
            "turnSpeed": 12.0,       # 角色朝移动方向平滑转身
        },
        "camera": {"distance": 4.5, "pitch": -18.0, "targetOffset": [0.0, 0.0, 1.2]},
        "body_visible": True,
        "script": "third_person",
    },
}


# ============================================================
# 场景
# ============================================================

def procedural(name, base, metallic=0.0, roughness=0.6, emissive=None):
    """一个程序化材质块。

    ⚠ 字段名逐字对齐 `SceneSerializer.cpp` 的 `resolveMaterial`：
      kind / name / baseColor(vec4) / metallic / roughness / emissive(vec3) /
      doubleSided / alphaMode。多写一个不存在的键不会报错，但少写一个不会
      用默认值——所以这里把所有**有默认值意识**的键都写上。
    """
    out = {
        "kind": "procedural",
        "name": name,
        "baseColor": base,
        "metallic": metallic,
        "roughness": roughness,
        "emissive": emissive or [0.0, 0.0, 0.0],
        "doubleSided": False,
        "alphaMode": "opaque",
    }
    return out


def transform(pos, scale=None):
    return {
        "position": [float(v) for v in pos],
        "rotation": [0.0, 0.0, 0.0],
        "scale": [float(v) for v in (scale or (1.0, 1.0, 1.0))],
    }


def build_scene(kind, spec):
    """一个模板的起始场景：Ground + Player。

    ⚠ 实体顺序是 Ground 在前、Player 在后：Hierarchy 面板按创建顺序排，
      地面在下面、玩家在上面 —— 与人看场景的心智模型一致（M0 验收第 ① 条
      要在 Hierarchy 里"看到两个物体"）。
    """
    p = spec["player"]
    cam = spec["camera"]

    ground = {
        "name": "Ground",
        "transform": transform((0.0, 0.0, 0.0)),
        "parent": -1,
        "visible": True,
        "castShadow": False,
        # locked = 视口点选跳过它、不给变换手柄（引擎自带地面就是这么钉住的）
        "locked": True,
        "mesh": {"kind": "builtin", "shape": "plane", "size": 60.0,
                 "name": "template_ground"},
        "material": procedural("template_ground",
                               [0.26, 0.27, 0.29, 1.0],
                               metallic=0.0, roughness=0.85),
    }

    player = {
        "name": "Player",
        "transform": transform((0.0, 0.0, 0.0)),
        "parent": -1,
        "visible": spec["body_visible"],
        "castShadow": True,
        # 占位胶囊（r=0.35, 圆柱段半高 0.55 → 总高约 1.8）。
        # ⚠ 真正的角色模型要走角色详情窗口绑定（character_pipeline A1），
        #   模板**不承诺**创建完就能看见一个第三人称小人。
        "mesh": {"kind": "builtin", "shape": "cylinder", "size": 1.0,
                 "name": "template_player"},
        "primitive": {"kind": "cylinder", "size": 0.9, "radius": 0.35,
                      "segments": 16, "rings": 1},
        "material": procedural("template_player",
                               [0.62, 0.70, 0.86, 1.0],
                               metallic=0.0, roughness=0.45),
        # 角色控制器（移动 / 贴地 / 跳跃）
        "character": {
            "radius": 0.35,
            "halfHeight": 0.55,
            "stepHeight": 0.35,
            "slopeLimitDeg": 46.0,
            "groundSnap": True,
            "groundSnapDistance": 0.3,
            "gravityScale": 1.0,
            "maxSpeed": 4.5,
            "sprintMultiplier": 2.0,
            "jumpSpeed": 5.0,
            "acceleration": 40.0,
            "airControl": 0.25,
            "turnSpeed": p["turnSpeed"],
            "pushStrength": 1.0,
            "enabled": True,
        },
        # 玩家（第一/第三人称）——**这一块就是两种类型最核心的差异**
        "player": {
            "mode": p["mode"],
            "eyeHeight": p["eyeHeight"],
            "moveSpeed": p["moveSpeed"],
            "lookSpeed": p["lookSpeed"],
            "yaw": p["yaw"],
            "pitch": p["pitch"],
            "camDistance": p["camDistance"],
            "turnSpeed": p["turnSpeed"],
        },
        "camera": {
            "fov": 55.0,
            "priority": 10,
            "enabled": True,
            "targetOffset": list(cam["targetOffset"]),
            "distance": cam["distance"],
            "minDistance": 1.0,
            "collideWithWorld": False,
            "shakeAmplitude": 0.0,
            "shakeFrequency": 24.0,
        },
        # 脚本（T1 · .vks）—— 把模板自带的 `assets/scripts/main.vks` **挂到
        # 玩家身上**。
        #
        # ⚠ 这一行不是装饰。`EditorApp::preloadScripts()` 只收集**场景里
        #   被 `ScriptComponent` 引用到的**脚本（不是扫描目录）—— 场景不引用
        #   的话，模板自带的那份 `main.vks` 永远不会被编译、也不会热重载。
        #   于是"改一行 .vks 保存 → 立刻生效"这条路径在新工程里根本不存在，
        #   而界面上没有任何提示（脚本文件在那儿躺着，看起来一切正常）。
        #
        # 附带好处：新工程一打开，Console 里就有一条 `HELLO <工程名>`，
        # "脚本管线通了"这件事**肉眼可见**，不用去猜。
        "script": {
            "path": "scripts/main.vks",
            "enabled": True,
            "timeScale": 1.0,
        },
    }

    return {
        "version": 1,
        "generator": "gen_templates.py (%s) —— 起始场景，材质全部程序化，零外部路径"
                     % kind,
        "camera": {
            "target": [0.0, 0.0, 1.0],
            "yaw": 0.0,
            "pitch": cam["pitch"],
            "distance": cam["distance"],
            "fov": 55.0,
        },
        "light": {
            "direction": [0.42, 0.66, -0.62],
            "color": [1.0, 0.96, 0.9],
            "intensity": 3.2,
            "castsShadow": True,
            "ambientScale": 1.0,
        },
        "physics": {
            "enabled": True,
            "groundPlane": True,
            "fixedTimeStep": 0.0166666675,
            "maxSubSteps": 4,
            "allowSleep": True,
        },
        "entities": [ground, player],
    }


# ============================================================
# 起始脚本（T1 · .vks）
# ============================================================
# ⚠ `let` 是幂等声明、`on start` 只跑一次（见 script_roadmap S1/S4）。
#   留一行 `print("HELLO", …)` 是有用的：它同时证明
#   ① 脚本被找到了 ② 被编译了 ③ 被实例化了 —— 三件事任一失败都不出声。
FIRST_PERSON_VKS = '''\
# main.vks —— __PROJECT_NAME__ · 第一人称模板（__TEMPLATE_ID__）
#
# 改这个文件，保存，编辑器里立刻热重载（不用重编译引擎）。
# 这一行 print 是"脚本真的跑起来了"的观测点。
let t = 0

on start {
    print("HELLO", "__PROJECT_NAME__")
    print("MODE", "first_person")
}

on update {
    t = t + dt
}
'''

THIRD_PERSON_VKS = '''\
# main.vks —— __PROJECT_NAME__ · 第三人称模板（__TEMPLATE_ID__）
#
# 改这个文件，保存，编辑器里立刻热重载（不用重编译引擎）。
# 这一行 print 是"脚本真的跑起来了"的观测点。
let t = 0

on start {
    print("HELLO", "__PROJECT_NAME__")
    print("MODE", "third_person")
}

on update {
    t = t + dt
}
'''


# ============================================================
# game.json / render.json / template.json
# ============================================================
def build_game_json(spec):
    """运行时出厂配置。

    ⚠ **不写 `quality`**。路线图 P4 实测：`RuntimeConfig::quality` 是空挂
      字段（读进来 → env 覆盖 → 只打印日志，没有任何消费者）。M4 把它
      明确废弃，画质的权威是 `config/render.json`（D3 的"配置单源"）。
      模板如果还写 quality，就是亲手再造一个"两份数据漂移"。

    ⚠ `title` 用占位符，创建时替换成工程名。
    ⚠ `scene` 是**资产相对路径**（与 project.startScene 同一个值）。
    """
    return {
        "_comment": "__PROJECT_NAME__ 的运行时出厂配置。"
                    "优先级: 命令行 > 环境变量(MYVK_RUNTIME_*) > 本文件 > 内置默认。"
                    "画质不在这里 —— 见 config/render.json。",
        "scene": "scenes/main.json",
        "title": "__PROJECT_NAME__",
        "width": 1280,
        "height": 720,
        "fullscreen": False,
        "maxFps": 60,
        "unfocusedFps": 15,
        "timeScale": 1.0,
        "language": "zh",
        "startState": "playing",
        "audio": {
            "enabled": True,
            "device": "default",
            "master": 1.0,
            "music": 0.7,
            "sfx": 1.0,
            "voice": 1.0,
            "mute": False,
            "muteOnFocusLost": False,
            "duckDepth": 0.75,
            "maxVoices": 64,
        },
    }


def build_template_json(kind, spec):
    """模板清单。

    `placeholders` 是**白名单**（P9）：创建时只替换这里列出的串，
    绝不做"全目录字符串替换" —— 那会改坏材质名 / 资产 id。
    `files` 给自检对账用：创建出来的工程里必须**逐个存在**。
    """
    return {
        "format": 1,
        "id": kind,
        "label": spec["label"],
        "summary": spec["summary"],
        "gameType": kind,
        "startScene": "scenes/main.json",
        "placeholders": ["__PROJECT_NAME__", "__TEMPLATE_ID__"],
        "files": [
            "assets/game.json",
            "assets/scenes/main.json",
            "assets/scripts/main.vks",
            "config/render.json",
        ],
    }


# ============================================================
# 写盘（确定性）
# ============================================================
def dump_json(obj):
    return json.dumps(obj, ensure_ascii=False, indent=2) + "\n"


def write_text(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    # newline="\n"：Windows 上也不要 CRLF，否则同一份模板在两台机器上
    # 生成出的字节不同，`--check` 会假报"漂移"。
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def generate_texts():
    """返回 {相对路径: 文本}。生成与校验共用这一份，**不会漂移**。"""
    files = {}
    for kind, spec in TYPES.items():
        base = kind
        files[base + "/template.json"] = dump_json(build_template_json(kind, spec))
        files[base + "/assets/scenes/main.json"] = dump_json(build_scene(kind, spec))
        files[base + "/assets/game.json"] = dump_json(build_game_json(spec))
        files[base + "/config/render.json"] = "{}\n"
        vks = FIRST_PERSON_VKS if kind == "first_person" else THIRD_PERSON_VKS
        files[base + "/assets/scripts/main.vks"] = vks
    return files


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="只校验 templates/ 与脚本是否一致，不写盘")
    args = ap.parse_args()

    files = generate_texts()
    drift = []
    written = 0
    for rel, text in sorted(files.items()):
        abs_path = os.path.join(OUT_ROOT, rel)
        exists = os.path.isfile(abs_path)
        old = None
        if exists:
            with open(abs_path, "r", encoding="utf-8", newline="") as f:
                old = f.read()
        if old == text:
            continue
        if args.check:
            drift.append(rel)
        else:
            write_text(abs_path, text)
            written += 1

    if args.check:
        if drift:
            print("TEMPLATES-DRIFT %d file(s) differ from gen_templates.py:"
                  % len(drift))
            for d in drift:
                print("  " + d)
            print("结果: FAIL: templates/ 与 gen_templates.py 不一致")
            return 1
        print("TEMPLATES-CHECK ok files=%d" % len(files))
        print("结果: PASS")
        return 0

    print("TEMPLATES-GEN written=%d total=%d root=%s"
          % (written, len(files), OUT_ROOT))
    for kind in TYPES:
        print("  %-14s -> %s" % (kind, os.path.join(OUT_ROOT, kind)))
    print("结果: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
