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

④ **工程自带一份能跑通的玩法代码**（`assets/scripts/*.vks`）。
   引擎的 C++ 是"能力"（渲染 / 物理 / 角色控制器），玩法住在脚本里。
   模板给的是**基本游戏环境**：地面 + 角色 + 5 个可收集光球 + 一个
   传送门 + 一份三段式主脚本（参数 / 状态 / 钩子）。新建的工程打开
   就能玩、Console 就有 `GAME-READY`，而不是一句 `print("HELLO")`。

用法：
    python tools/gen_templates.py            # 生成
    python tools/gen_templates.py --check    # 只校验（不写盘），CI 用
"""

import argparse
import json
import math
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
    """一个模板的起始场景：Ground + Player + 5 个光球 + 传送门。

    ⚠ 实体顺序是有语义的：Hierarchy 面板按创建顺序排，**也是脚本实例的
      创建顺序**。Player 排在光球之前，于是 `main.vks` 的 `on start`
      （重置世界黑板 `picked`）先于 `pickup.vks` 的实例执行 —— 反过来排的话，
      新工程第一次进 Play 就可能带着上一次残留的分数。

    ⚠ 光球用的是**环状分布**而不是随手写 5 个坐标：拾取半径 1.6 m 而环半径
      4.0 m，相邻两颗间距 4.7 m，不会"走一步连吃三个"；也保证第三人称
      （相机距离 4.5）在出生点就能看见至少两颗 —— "这里有东西可以捡"
      这件事必须一眼可见，否则模板教不会任何人。
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

    # ---- 5 个可收集光球（金色自发光，半径 0.3）----
    #
    # ⚠ **每个光球的材质名都不一样**（template_pickup_1..5）。
    #   程序化材质按 name 进 AssetManager 缓存，同名即共享；而脚本的
    #   `setEmissive()` 改的是**共享材质本身**。共用名字的话，将来某个
    #   光球改成"被捡走后变红"，会一次改红全部 5 个 —— 而且没有任何报错。
    pickups = []
    for i in range(5):
        a = -math.pi / 2 + i * (2.0 * math.pi / 5.0)   # 从 -Y 起，均分一圈
        x = round(4.0 * math.cos(a), 4)
        y = round(4.0 * math.sin(a), 4)
        n = i + 1
        pickups.append({
            "name": "Pickup_%d" % n,
            "transform": transform((x, y, 1.0)),
            "parent": -1,
            "visible": True,
            "castShadow": True,
            "mesh": {"kind": "builtin", "shape": "sphere", "size": 0.3,
                     "name": "template_pickup_%d" % n},
            "primitive": {"kind": "sphere", "size": 0.3, "radius": 0.3,
                          "segments": 24, "rings": 16},
            "material": procedural("template_pickup_%d" % n,
                                   [0.95, 0.78, 0.25, 1.0],
                                   metallic=0.15, roughness=0.3,
                                   emissive=[0.55, 0.38, 0.05]),
            # 每颗光球都是"同一份脚本的独立实例"：`taken` 各自一份。
            "script": {"path": "scripts/pickup.vks", "enabled": True,
                       "timeScale": 1.0},
        })

    # ---- 传送门：场地 -Y 一端的发光地标 ----
    #
    # ⚠ 它**不承担通关判定**（收齐光球即通关，见 main.vks）。原因是
    #   "出生点能不能看见它"取决于 Play 态相机的朝向与距离，而这在编辑器里
    #   受玩家 yaw / spring arm 影响 —— 一个模板不该把"第一局一定能走完"
    #   押在镜头朝向上。它在这里的职责是**视觉奖励**：收齐后由
    #   portal.vks 点亮 + 自转，让玩家看到"世界对完成有反应"。
    #   直径 1.5 m、高 3 m：在 60 m 的地面上是一个明确的地标，又不挡路。
    portal = {
        "name": "Portal",
        "transform": transform((0.0, -8.0, 0.0), scale=(1.0, 1.0, 2.0)),
        "parent": -1,
        "visible": True,
        "castShadow": False,
        "mesh": {"kind": "builtin", "shape": "cylinder", "size": 1.5,
                 "name": "template_portal"},
        "primitive": {"kind": "cylinder", "size": 1.5, "radius": 0.75,
                      "segments": 32, "rings": 1},
        "material": procedural("template_portal",
                               [0.15, 0.55, 0.55, 1.0],
                               metallic=0.0, roughness=0.25,
                               emissive=[0.05, 0.45, 0.55]),
        # 终点逻辑住在 portal.vks（setEmissive 只对自己生效，见该文件头）
        "script": {"path": "scripts/portal.vks", "enabled": True,
                   "timeScale": 1.0},
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
        "entities": [ground, player] + pickups + [portal],
    }


# ============================================================
# 起始脚本（T1 · .vks）—— 工程的"基本游戏环境"
# ============================================================
# 这里给的不是一句 `print("HELLO")`，而是一份**能通关的小游戏**：
#   收齐 5 个光球 → 传送门点亮 → 走进去 → GAME-COMPLETE。
#
# 为什么值得写这么长（而不是留个 hello world）：
#   ① 模板是用户读到的**第一份代码**，它示范的写法会被照抄。留一句 print
#      相当于告诉用户"脚本只能打印"。
#   ② 它同时是一份活文档：`on start` / `on update` / `let` 跨帧状态 /
#      世界黑板 / 实体句柄 / 输入读取，全部在这 100 行里出现一次。
#   ③ Console 里的 `GAME-READY` 是"脚本真的跑起来了"的观测点；后面的
#      `SCORE 3 / 5`、`GAME-COMPLETE t= 12.4` 是"玩法真的在跑"的观测点。
#
# ⚠ print 的内容一律用**英文**：脚本文件是 UTF-8，而 Console 的字库/编码
#   链路不是本模板能保证的（注释里的中文不进运行时，随便写）。
MAIN_VKS = '''\
# ============================================================
# main.vks --- __PROJECT_NAME__ 的游戏逻辑主脚本
# 模板: __TEMPLATE_ID__   (VKS, 挂在 Player 实体上)
# ============================================================
#
# 【引擎负责什么，脚本负责什么】
#   C++ 引擎负责"能力"：渲染、物理、角色控制器（走/跑/跳）、输入采集。
#   "玩法"全部写在这个 .vks 里 --- 不需要编译 C++，改一行、保存，
#   编辑器立刻热重载（Console 里会多一行 VKS-RELOAD）。
#
# 【本工程的脚本分工】
#   assets/scripts/main.vks     本文件：挂在 Player 上，管全局流程 / 胜负
#   assets/scripts/pickup.vks   挂在 5 个 Pickup_* 光球上，管"被捡走"
#   assets/scripts/portal.vks   挂在 Portal 上，管"收齐后点亮自己"
#
#   ⚠ 为什么"点亮传送门"不写在主脚本里：setEmissive 是**只对自己生效**的
#     （它没有实体参数，改的是执行它的那个实体的材质）。想在 A 脚本里改
#     B 的材质，只能给 B 自己挂一份脚本。这条限制是有意的 —— 见规则 ②。
#
# 【VKS 速查】
#   let x = 0                 声明（文件开头的 let 先于 on start 生效）
#   x = x + dt                赋值；变量跨帧存活（不是局部变量）
#   if (...) { ... } else { ... }
#   while (...) { ... }       foreach (e in 列表) { ... }
#   on start { ... }          实例第一次执行前跑一次
#   on update { ... }         每帧跑一次
#   实体: self()  player()  find("名字")  exists(e)  name(e)
#   变换: pos(e)  setPos(e, vec3)  rot(e)  setRot(e, vec3)
#         visible(e)  setVisible(e, b)
#   材质: setEmissive(r, g, b)        <-- 只改【自己】的材质，见规则 ②
#   查询: dist(a, b)   near(center, 半径[, 上限])
#   黑板: worldAttr(k[, 默认])  setWorldAttr(k, v)   全局、进存档
#         attr(e, k[, 默认])    setAttr(e, k, v)     逐实体
#   输入: pressed("Jump")  held("Sprint")  released("Interact")
#         axis("MoveForward")     <-- 参数是 InputMap 里的动作名，不是键码
#   表现: print(...)  popup(数字)
#   协程: wait(秒)  waitFrames(n)  waitUntil(条件)   挂起这一句，不卡帧
#   数学: sin cos atan2 abs floor ceil round sqrt pow min max clamp lerp
#
# 【三条硬规则】
#   ① 角色移动交给 C++ 的 character 组件。不要在脚本里 setPos(player)：
#      物理步进会把位置改回去，表现为"走两步被拽回原地"，且日志无痕迹。
#   ② setEmissive 只能改【自己】的材质（没有实体参数），而且材质是按名字
#      共享的。想让某个物体单独发光，给它一个独立的材质名 —— 场景里 5 个
#      光球就是这么做的（template_pickup_1..5）。共用名字时，改一个会改全部。
#   ③ "靠近了没有"用 dist() 判断，不要用 on collide：编辑器的 Play 态没有
#      接触事件（只有运行时 Game.exe 有），同一份脚本在编辑器里会一动不动。
# ============================================================

# ---------------- 可调参数（集中在文件开头，方便调手感） ----------------
let TOTAL = 5              # 光球总数（= 场景里 Pickup_* 的个数）
let TIME_LIMIT = 120.0     # 限时（秒）；写 0 表示不限时

# ---------------- 运行期状态（跨帧存活） ----------------
let t = 0                  # 本局已过秒数
let picked = 0             # 已收集光球数
let lastPicked = -1        # 上一次读到的黑板值（只在变化时打日志）
let done = 0               # 0=进行中  1=通关  2=超时

on start {
    print("GAME-READY", "__PROJECT_NAME__")
    print("GAME-MODE", "__TEMPLATE_ID__")
    print("GAME-OBJ collect", TOTAL, "orbs before the timer runs out")

    # 从零开始计分：世界黑板是进存档的，重开一局要清掉上一次的进度。
    # 正式项目想"接着上次玩"，把这两行换成读档即可。
    setWorldAttr("picked", 0)
    setWorldAttr("total", TOTAL)   # 给 portal.vks 读（脚本之间靠黑板通信）
    lastPicked = -1
    setHealth(100)
}

on update {
    t = t + dt

    # ---- 1. 收集进度：只在黑板变化时打日志（否则每帧一行，刷爆 Console）----
    picked = worldAttr("picked", 0)
    if (picked != lastPicked) {
        lastPicked = picked
        print("SCORE", picked, "/", TOTAL)
    }

    # ---- 2. 胜负判定 ----
    #   "收齐 = 通关"。刻意的：终点不该依赖"玩家必须找到一个特定位置"，
    #   模板的第一局要保证**一定能走完**。传送门是奖励性的视觉反馈
    #   （portal.vks 负责点亮），不是必需条件。
    if (done == 0 && picked >= TOTAL) {
        done = 1
        print("GAME-COMPLETE t=", t)
        popup(100)
        setHealth(100)
    }
    if (done == 0 && TIME_LIMIT > 0.0 && t > TIME_LIMIT) {
        done = 2
        print("GAME-TIMEOUT t=", t)
    }
}
'''

PICKUP_VKS = '''\
# ============================================================
# pickup.vks --- 光球：玩家走近 1.6 m 就收走，并给世界黑板 +1
# 挂在场景里每一个 Pickup_* 上（同一份脚本、5 个独立实例）
# ============================================================
#
# 【为什么要 taken 这个守卫】
#   捡到之后只是 setVisible(false) --- 实例还在跑。没有 taken 的话，
#   玩家站在原地会每帧 +1，分数瞬间爆掉（而且不报错，只有分数看得出）。
#
# 【实例变量 vs 世界黑板】
#   taken    逐实体一份（不存盘）-> 用来自我节流
#   picked   全局一份（进存档）  -> 用来记总分
#
# 【动画】
#   自转 + 上下浮动。这不是装饰：静止的小球在场景里很难被注意到，
#   而"会动的、发光的"是玩家一眼就能识别出的可交互物。
# ============================================================

let taken = 0
let R = 1.6                # 拾取半径（米）
let SPIN = 90.0            # 自转速度（度/秒）
let BOB = 0.18             # 上下浮动幅度（米）
let base = vec3(0, 0, 0)   # 出生位置（start 里记下，浮动以它为基准）

on start {
    base = pos(self())
}

on update {
    # 自转 + 浮动
    r = rot(self())
    setRot(self(), vec3(r.x, r.y, r.z + SPIN * dt))
    setPos(self(), vec3(base.x, base.y, base.z + sin(time * 2.0) * BOB))

    # 拾取（全部逻辑就这三行）
    if (taken == 0 && dist(pos(self()), pos(player())) < R) {
        taken = 1
        setWorldAttr("picked", worldAttr("picked", 0) + 1)
        setVisible(self(), false)
        popup(10)
        print("PICKUP", name(self()), worldAttr("picked", 0))
    }
}
'''

PORTAL_VKS = '''\
# ============================================================
# portal.vks --- 终点地标：一直自转；玩家收齐光球后点亮自己并加速
# 挂在场景里的 Portal 实体上
# ============================================================
#
# 【它不负责"通关判定"】
#   通关是"收齐 5 个球"，写在 main.vks 里。这一份只做**视觉反馈** ——
#   模板的第一局必须保证一定能走完，而"走到某个位置"依赖镜头朝向和玩家
#   找路的运气，不该是通关条件。
#
# 【为什么需要单独一份脚本】
#   `setEmissive(r, g, b)` **只对自己生效**（没有实体参数，改的是执行它的
#   那个实体的材质）。主脚本挂在 Player 上，它没办法把 Portal 点亮 ——
#   所以"点亮"必须住在 Portal 自己身上。这也是引擎刻意的设计：脚本能动的
#   只有"自己 + 几个窄动作"，不能随手改别人。
#
# 【脚本之间怎么通信】
#   世界黑板（`worldAttr` / `setWorldAttr`）。main.vks 在 start 里写
#   `total`，pickup.vks 每收一个把 `picked` +1，这里读 `picked >= total`。
#   黑板是**全局**的，任何脚本都能读 —— 比"脚本之间互相持有句柄"简单得多。
# ============================================================

let SPIN = 40.0            # 自转速度（度/秒）
let lit = 0                # 是否已经点亮（只做一次）

on start {
    setEmissive(0.05, 0.36, 0.45)     # 未激活：暗青
}

on update {
    r = rot(self())
    setRot(self(), vec3(r.x, r.y, r.z + SPIN * dt))

    # 收齐了就亮 + 转得更快（worldAttr 的第二参数是默认值：主脚本还没跑时兜底）
    if (lit == 0 && worldAttr("picked", 0) >= worldAttr("total", 5)) {
        lit = 1
        SPIN = SPIN * 2.5
        setEmissive(0.2, 0.95, 0.8)   # 激活：亮青绿
        print("GAME-PORTAL-ON")
    }
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
            "README.md",
            "assets/game.json",
            "assets/scenes/main.json",
            "assets/scripts/main.vks",
            "assets/scripts/pickup.vks",
            "assets/scripts/portal.vks",
            "config/render.json",
        ],
    }


# ============================================================
# 工程自带的 README（给"创建完不知道从哪下手"的人看）
# ============================================================
# ⚠ 用 `{{...}}` 而不是 `__X__` 做二次占位：`__PROJECT_NAME__` /
#   `__TEMPLATE_ID__` 是**创建时**由 `ProjectCreate` 按白名单替换的，
#   这里要在**生成时**就替换掉，两套占位符必须能区分开，否则会互相吃掉。
README_TEMPLATE = '''\
# __PROJECT_NAME__

由 **Venn 启动器** 从 `{{KIND}}`（{{LABEL}}）模板创建的工程 —— {{SUMMARY}}

## 怎么玩

在启动器里打开这个工程（或直接打开 `venn.project.json`），按 Play：

1. `WASD` 移动，`Shift` 疾跑，`Space` 跳跃，`E` 交互（这是默认绑定）
2. 场上 5 个金色光球，走近 1.6 m 自动收集
3. 收齐 5 个 → Console 打印 `GAME-COMPLETE`，场地一端的传送门同时由暗青变亮
   青绿并加速自转（这一下是 `portal.vks` 干的，用来演示"两个脚本靠世界黑板
   通信"）
4. 120 秒内没收齐 → `GAME-TIMEOUT`（把 `main.vks` 的 `TIME_LIMIT` 改 0 可取消）

## 目录结构

| 路径 | 作用 |
|---|---|
| `venn.project.json` | 工程身份（名字 / 类型 / 起始场景 / 代码模式），启动器写，别手改 |
| `assets/scenes/main.json` | 起始场景：地面 / 玩家 / 5 个光球 / 传送门 |
| `assets/scripts/main.vks` | **游戏逻辑主脚本**（挂在 Player 上，管流程与胜负） |
| `assets/scripts/pickup.vks` | 光球逻辑（挂在每个 Pickup_* 上，5 个实例各一份状态） |
| `assets/scripts/portal.vks` | 终点逻辑（挂在 Portal 上，收齐后点亮自己） |
| `assets/game.json` | 运行时出厂配置（窗口尺寸、限帧、音频） |
| `config/render.json` | 画质开关（创建向导写入；删成 `{}` = 全部默认） |

## 游戏逻辑写在哪

**写在 `.vks` 脚本里，不写 C++。** 引擎的 C++ 提供能力（渲染 / 物理 /
角色控制器 / 输入采集），脚本提供玩法。好处是：改完保存就生效，不用编译。

改 `assets/scripts/main.vks` → 保存 → 编辑器 Play 态**自动热重载**
（Console 出现 `VKS-RELOAD`）。手感参数都集中在文件开头：

    let TOTAL = 5              # 光球总数（= 场景里 Pickup_* 的个数）
    let TIME_LIMIT = 120.0     # 限时（秒），写 0 = 不限时

脚本语言的完整说明见 `assets/scripts/main.vks` 开头的「VKS 速查」，
以及引擎的 `docs/script_roadmap.md`。

## 常见的下一步

- **加一个可拾取物**：场景里复制任意 `Pickup_*`，它的 `script.path` 已经是
  `scripts/pickup.vks`，复制即生效；再把 `main.vks` 的 `TOTAL` 改成新总数。
- **加自己的逻辑**：新建 `assets/scripts/my_game.vks`，在实体的
  Inspector ▸ Script 里指向它。一个实体一份实例，变量互不干扰。
- **读输入**：`pressed("Jump")` / `held("Sprint")` / `axis("MoveForward")`，
  参数是 Preferences ▸ Bindings 里的**动作名**，不是键码。
- **存分数**：`setWorldAttr("score", 10)` 写世界黑板（进存档）；
  `setAttr(self(), "hp", 5)` 写逐实体变量（不进存档）。

## 三个别做的事

1. **不要在脚本里 `setPos(player())`**：移动归角色控制器 / 物理管，
   脚本改位置会被下一步物理拉回去（表现为"走两步被拽回原地"，无报错）。
2. **不要用 `on collide` 做靠近判定**：编辑器的 Play 态没有接触事件，
   同一份脚本在编辑器里会一动不动。用 `dist()`。
3. **不要手改 `venn.project.json` 的 `code` 段**，除非你确认
   `assets/scripts` 是不是指向外部目录的联接。
'''


def build_readme(kind, spec):
    """README 里的类型名 / 摘要按模板参数化（两种类型各一份）。"""
    text = README_TEMPLATE
    text = text.replace("{{KIND}}", kind)
    text = text.replace("{{LABEL}}", spec["label"])
    text = text.replace("{{SUMMARY}}", spec["summary"])
    return text


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
        # ⚠ 两个类型的脚本内容**逐字相同**（差异在场景的 player/camera 段）：
        #   玩法不该因为"第一人称"就分叉成两份要各自维护的源码。
        files[base + "/assets/scripts/main.vks"] = MAIN_VKS
        files[base + "/assets/scripts/pickup.vks"] = PICKUP_VKS
        files[base + "/assets/scripts/portal.vks"] = PORTAL_VKS
        files[base + "/README.md"] = build_readme(kind, spec)
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
