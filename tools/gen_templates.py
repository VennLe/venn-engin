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
        # ⚠ 单位是**弧度**、pitch 是**相机在目标上方**的俯角（正 = 相机在上）。
        #   2026-10-21 之前这里写的是度（-12.0 / -18.0），SceneSerializer 当
        #   弧度读 → setPitch 夹到 -89° → 新工程一打开相机沉在地面以下朝天看。
        "camera": {"distance": 8.0, "pitch": 0.20, "targetOffset": [0.0, 0.0, 1.0]},
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
        # 弧度、正数 = 相机在上方 → 越过角色的后肩往下看（第三人称该有的样子）。
        # 0.35 rad ≈ 20°，4.5m 距离下相机离地 ~2.7m，稳稳在地面栅格之上。
        "camera": {"distance": 4.5, "pitch": 0.35, "targetOffset": [0.0, 0.0, 1.2]},
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


# ============================================================
# 地形（新工程的地面）
# ============================================================
# 模板必须**自带**一块地形，而不是引 engine 的 assets/terrain/*：
# 新工程的资产根是它自己的 assets/，看不到引擎目录（见 ProjectCreate）。
TERRAIN_COLS = 257
TERRAIN_ROWS = 257
TERRAIN_MPP = 2.0       # 每像素 2 m → 514 m 见方
TERRAIN_MAX_H = 6.0     # 缓起伏，不是山地（新工程第一眼要能走）
TERRAIN_FLAT_R = 14.0   # 出生点周围压平（米）


def _hash2(ix, iy):
    n = (ix * 374761393 + iy * 668265263) & 0xFFFFFFFF
    n = ((n ^ (n >> 13)) * 1274126177) & 0xFFFFFFFF
    return ((n ^ (n >> 16)) & 0xFFFFFFFF) / 0xFFFFFFFF


def _value_noise(x, y):
    ix, iy = math.floor(x), math.floor(y)
    fx, fy = x - ix, y - iy
    sx = fx * fx * (3.0 - 2.0 * fx)
    sy = fy * fy * (3.0 - 2.0 * fy)
    a = _hash2(ix, iy)
    b = _hash2(ix + 1, iy)
    c = _hash2(ix, iy + 1)
    d = _hash2(ix + 1, iy + 1)
    top = a + (b - a) * sx
    bot = c + (d - c) * sx
    return top + (bot - top) * sy


def _fbm(x, y, octaves=4):
    total, amp, freq, norm = 0.0, 1.0, 1.0, 0.0
    for _ in range(octaves):
        total += _value_noise(x * freq, y * freq) * amp
        norm += amp
        amp *= 0.5
        freq *= 2.0
    return total / norm


def terrain_height(xm, ym):
    """地形高度（米）。**恒 >= 0，且出生点（原点）恰好是 0**。

    2026-10-22（第十轮）改的是"归一化基准"这件事 —— 这是一次真 bug 的修复：

      以前：`terrain_height` 把出生点周围 14 m 压平到 0，然后
            `build_terrain_bytes` 用**全局最低点**归一化 `(v - lo) / span`。
            出生点那个 0 于是被抬到 (0 - lo)/span ≈ 0.56，乘 maxHeight 之后
            **地面在 3.35 m**，而场景里 GameStart 写的是 z = (0,0,0)。
            结果是新工程一进来，玩家的脚在地面下 3.35 m（用户原话：
            "物理特性上不允许说有任何物体能陷入其中，包括 GameStart"）。

      现在：世界 z = 0 **就是**地面。山脊往上长（0 .. max），洼地压到 0
            （它们本来就是"低处"，压平之后是平地，不影响"能不能走"）。
    """
    h = (_fbm(xm / 90.0, ym / 90.0, 4) - 0.5) * 2.0
    # 幂次让谷底更平、脊线更圆，读起来像"自然土坡"而不是噪声
    h = math.copysign(abs(h) ** 1.25, h)
    # [-1, 1] → [0, TERRAIN_MAX_H]：0 是地面基准，往上的才是"坡"
    z = (h * 0.5 + 0.5) * TERRAIN_MAX_H
    r = math.hypot(xm, ym)
    if r < TERRAIN_FLAT_R:
        t = r / TERRAIN_FLAT_R
        z *= t * t          # 出生点周围压平到 0（抬升量按 t² 渐入）
    return z


def _terrain_raw():
    """所有采样点的高度（米）。

    ⚠ 写入顺序必须与改动前**逐字节一致**（`for r: for c:`）。这份数组在
      磁盘上是"逐行"写的，而读取侧（`physics::Heightfield` / `terrain.vert`）
      按列主序解释 —— 两边一直是同一个约定，看起来就是把地形沿对角线翻了一
      下。噪声地形翻不翻无所谓，但**改了写入顺序会让所有已生成工程的
      地形形状变掉**，所以这里保持原样。
    """
    half = (TERRAIN_COLS - 1) * 0.5
    raw = []
    for r in range(TERRAIN_ROWS):
        for c in range(TERRAIN_COLS):
            raw.append(terrain_height((c - half) * TERRAIN_MPP,
                                      (r - half) * TERRAIN_MPP))
    return raw


_MAX_HEIGHT_CACHE = []


def terrain_max_height():
    """`.vterrain` 的 maxHeight = 实际最高点（归一化的分母）。

    以前这里是常量 `TERRAIN_MAX_H`，而高度图被"全局 min..max"归一化 ——
    两个数字取的不是同一把尺子，maxHeight 只是个近似值。现在归一化只除以
    最高点，maxHeight 就**必须**等于它，否则地面高度会整体差一截。
    """
    if not _MAX_HEIGHT_CACHE:
        _MAX_HEIGHT_CACHE.append(max(_terrain_raw()))
    return _MAX_HEIGHT_CACHE[0]


def build_terrain_bytes():
    """返回 (height.r32, splat.rgba8) 的字节。"""
    import struct as _struct
    raw = _terrain_raw()
    hi = max(raw)
    # 只用"出生点以上"这一段做归一化：原点（0 m）→ 0.0，最高点 → 1.0。
    # 负数（理论上没有，`terrain_height` 保证 >= 0）也钳到 0。
    norm = [max(v, 0.0) / hi for v in raw]
    height = _struct.pack("<%df" % len(norm), *norm)
    # splat：4 层指向同一材质，权重全给第 0 层（想分层刷地表在编辑里改）
    splat = bytearray(TERRAIN_COLS * TERRAIN_ROWS * 4)
    for i in range(TERRAIN_COLS * TERRAIN_ROWS):
        splat[i * 4] = 255
    return height, bytes(splat)


def build_terrain_json():
    return {
        "type": "terrain",
        "version": 1,
        "name": "ground",
        "cols": TERRAIN_COLS,
        "rows": TERRAIN_ROWS,
        "metersPerPixel": TERRAIN_MPP,
        "maxHeight": round(terrain_max_height(), 6),
        "splatRepeat": 1.0,
        "layerTiling": 6.0,
        "height": "height.r32",
        "splat": "splat.rgba8",
        "layers": ["materials/template_ground"] * 4,
    }


# ============================================================
# 地面贴图（2026-10-21 第九轮：没有贴图的地面看起来"糊成一团"）
# ============================================================
# 用户报"视口里近处远处的地面明显模糊重影"。实测根因不是 TAA/阴影
# （开关前后逐像素相同），而是**材质一张贴图都没有**：几百像素的纯渐变
# 色带，眼睛读成"糊"+ 马赫带。
#
# 这里纯 Python 生成两张**无缝**贴图（不依赖 PIL：宿主裸 python 没装）：
#   · albedo —— 土黄底 + 两个八度噪声 + 碎石子
#   · normal —— 由同一张高度场求梯度，让地面在掠射角下真的"有颗粒"
TEX_SIZE = 256


def _periodic_noise(x, y, period, seed):
    """周期性 value noise：period 用整数取模 → 贴图左右/上下无缝。"""
    ix, iy = int(math.floor(x)), int(math.floor(y))
    fx, fy = x - ix, y - iy
    sx = fx * fx * (3.0 - 2.0 * fx)
    sy = fy * fy * (3.0 - 2.0 * fy)

    def h(i, j):
        i %= period
        j %= period
        n = (i * 374761393 + j * 668265263 + seed * 1442695040) & 0xFFFFFFFF
        n = ((n ^ (n >> 13)) * 1274126177) & 0xFFFFFFFF
        return ((n ^ (n >> 16)) & 0xFFFFFFFF) / 0xFFFFFFFF

    a, b = h(ix, iy), h(ix + 1, iy)
    c, d = h(ix, iy + 1), h(ix + 1, iy + 1)
    top = a + (b - a) * sx
    bot = c + (d - c) * sx
    return top + (bot - top) * sy


def _ground_height(u, v):
    """0..1 的表面高度场（无缝）。u/v ∈ [0,1)。

    2026-10-22（第十轮）：低频削掉、高频加足。

    上一版是 16/48/128 三个八度、幅度 0.34/0.30/0.22 —— 低频占主导，
    256 像素铺 6 米时读出来是"几米一块的大斑"，配上法线贴图就是一层
    **丝绸一样的方向性反光**。用户原话："地面光泽度太高了，需要粗糙点
    看起来才自然些。"

    现在：低频只留 0.10（给一点自然的明暗起伏），能量压到 48/128 两个
    八度上 —— 那是"土颗粒"的尺度（1 米内几十个起伏）。远处的 mip 会把
    高频平均掉，正好退化成"粗糙无光泽"，而不是"远处变成塑料"。
    """
    base = (_periodic_noise(u * 16.0, v * 16.0, 16, 1) - 0.5) * 0.10
    mid = (_periodic_noise(u * 48.0, v * 48.0, 48, 2) - 0.5) * 0.24
    fine = (_periodic_noise(u * 128.0, v * 128.0, 128, 3) - 0.5) * 0.36
    return 0.5 + base + mid + fine


def build_ground_textures():
    """返回 (albedo_png_bytes, normal_png_bytes)。"""
    n = TEX_SIZE
    h = [[_ground_height(x / n, y / n) for x in range(n)] for y in range(n)]

    # ---- albedo：土黄底 * 噪声明暗 + 少量深色石子 ----
    base = (0.66, 0.53, 0.28)
    albedo = bytearray(n * n * 3)
    for y in range(n):
        row = h[y]
        for x in range(n):
            v = row[x]
            # 第十轮：对比度从 ±10% 提到 ±20%，并且给一点"土不纯"的偏色
            # （纯色底 + 弱噪声 = 塑料）。底线仍然是"读起来是土黄"，
            # 不是"深褐泥巴"。
            shade = 0.86 + 0.26 * v
            # 逐纹素的细颗粒（period 必须整除贴图尺寸，否则左右/上下有缝：
            # 256 纹素 × 0.5 = 128 = 正好一个周期）
            grain = (_periodic_noise(x * 0.5, y * 0.5, 128, 7) - 0.5) * 0.10
            shade *= 1.0 + grain
            pebble = 1.0 if v < 0.25 else 0.0
            r = base[0] * shade * (0.86 if pebble else 1.0)
            g = base[1] * shade * (0.90 if pebble else 1.0)
            b = base[2] * shade * (0.97 if pebble else 1.0)
            i = (y * n + x) * 3
            albedo[i] = max(0, min(255, int(r * 255)))
            albedo[i + 1] = max(0, min(255, int(g * 255)))
            albedo[i + 2] = max(0, min(255, int(b * 255)))

    # ---- normal：中心差分梯度（环绕取样 = 无缝）----
    # 1.3 → 3.6：这个系数直接决定"平地上有没有颗粒感"。1.3 时法线最大
    # 只偏 7°，掠射角下看到的是一层光滑的亮膜；3.6 才像干土。
    strength = 3.6
    normal = bytearray(n * n * 3)
    for y in range(n):
        for x in range(n):
            hl = h[y][(x - 1) % n]
            hr = h[y][(x + 1) % n]
            hd = h[(y - 1) % n][x]
            hu = h[(y + 1) % n][x]
            nx = (hl - hr) * strength
            ny = (hd - hu) * strength
            nz = 1.0
            ln = math.sqrt(nx * nx + ny * ny + nz * nz)
            i = (y * n + x) * 3
            normal[i] = int((nx / ln * 0.5 + 0.5) * 255)
            normal[i + 1] = int((ny / ln * 0.5 + 0.5) * 255)
            normal[i + 2] = int((nz / ln * 0.5 + 0.5) * 255)

    return _png_rgb(n, n, albedo), _png_rgb(n, n, normal)


def _png_rgb(w, h, rgb):
    """最小 PNG 编码（zlib 已过滤=0）。确定性：同样的 RGB → 同样的字节。"""
    import struct as _struct
    import zlib as _zlib

    def chunk(tag, data):
        c = _struct.pack(">I", len(data)) + tag + data
        return c + _struct.pack(">I", _zlib.crc32(tag + data) & 0xFFFFFFFF)

    raw = bytearray()
    stride = w * 3
    for y in range(h):
        raw.append(0)                      # filter = None
        raw += rgb[y * stride:(y + 1) * stride]
    out = bytearray(b"\x89PNG\r\n\x1a\n")
    out += chunk(b"IHDR", _struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    out += chunk(b"IDAT", _zlib.compress(bytes(raw), 9))
    out += chunk(b"IEND", b"")
    return bytes(out)


# 新工程的默认地面材质：土黄。比 terrain_dirt 亮一档 —— 太暗会把起伏吃掉，
# 新建工程"第一眼看到一张有立体感的土坡"比"看到一块暗色平面"重要。
# ⚠ albedo / normal 必须带上：没有贴图的纯色地面在视口里就是一片糊。
TEMPLATE_GROUND_MAT = {
    "type": "material",
    "version": 1,
    "id": "materials/template_ground",
    "emissive": [0.0, 0.0, 0.0],
    "doubleSided": False,
    "normalScale": 1.0,
    "ao": 1.0,
    "alphaCutoff": 0.5,
    "alphaMode": "opaque",
    "shadingModel": "default",
    "baseColor": [0.66, 0.53, 0.28, 1.0],
    "metallic": 0.0,
    # 第十轮：0.92 → 1.0。土不会镜面反射，而地形着色器**没有**逐层粗糙度
    # 贴图（只有这一个标量），0.92 留下的是一个能整片滑动的宽高光 ——
    # 在平地上看起来就是"打了蜡"。拉满 = GGX 的 D 项退化成常数，
    # 高光只剩菲涅尔那一点点边缘，视觉上立刻变"干"。
    "roughness": 1.0,
    "albedo": {"kind": "file", "name": "template_ground_albedo",
               "path": "textures/template_ground_albedo.png", "srgb": True},
    "normal": {"kind": "file", "name": "template_ground_normal",
               "path": "textures/template_ground_normal.png", "srgb": False},
}


def build_scene(kind, spec):
    """起始场景：**只有地形 + 出生点**（2026-10-21 第八轮，用户要求）。

    之前这里是"Ground 平面 + 5 个光球 + 传送门 + 玩家胶囊"（一套能通关的
    小游戏）。用户要的是"新建工程进来是一张干净的地"：
      · 地面换成**真地形**（高度图 + splat，土黄色），不再是 20 m 平面；
      · 场上的道具全部取消；
      · 玩家不再作为实体存在 —— 由 `GameStart` 标位置，运行时生成
        （见 `RuntimeApp` 的 `spawnPlayerFromGameStart`）。

    ⚠ `GameStart` 上挂 `main.vks`：它是**世界逻辑**脚本（打印 GAME-READY /
    GAME-MODE / SCORE），不依赖某个道具实体。挂在这里而不是"没有挂载点"，
    模板自带的脚本示例才还能跑、还能热重载。
    """
    cam = spec["camera"]

    terrain = {
        "name": "Terrain",
        "transform": transform((0.0, 0.0, 0.0)),
        "parent": -1,
        "visible": True,
        "castShadow": False,
        # locked = 视口点选跳过它、不给变换手柄（引擎自带地面就是这么钉住的）
        "locked": True,
        "terrain": {
            "source": "terrain/ground/terrain.vterrain",
            "visible": True,
            "heightScale": 1.0,
            "layers": ["", "", "", ""],
        },
    }

    # 出生点：站在原点。生成器已把原点周围 14 m 压平到 0，所以玩家落地
    # 不会滑；朝向 0 = +X（与 third_person 的相机构图一致）。
    game_start = {
        "name": "GameStart",
        "transform": transform((0.0, 0.0, 0.0)),
        "parent": -1,
        "visible": True,
        "castShadow": False,
        "gameStart": {"yaw": 0.0, "enabled": True},
        "script": {"path": "scripts/main.vks", "enabled": True,
                   "timeScale": 1.0},
    }

    return {
        "version": 1,
        "generator": "gen_templates.py (%s) —— 空场景：地形 + 出生点" % kind,
        # ------------------------------------------------------------
        # 场景相机（编辑器第一次打开这个场景时的取景）
        # ------------------------------------------------------------
        # ⚠⚠ 三个单位陷阱（2026-10-21 一次性踩全了）：
        #   1) **yaw / pitch 是弧度**，不是度；
        #   2) **pitch 与"视线仰角"反号**：pitch = 相机在目标**上方**的角度，
        #      写负数 = 相机沉到地面以下朝天看；
        #   3) yaw = π 才是"站在角色背后"：yaw=0 时相机在 +X 侧看向 -X，
        #      而角色默认朝 +X —— 那是正对着脸拍。
        "camera": {
            "target": list(cam["targetOffset"]),
            "yaw": math.pi,
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
        "entities": [terrain, game_start],
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
            # 地形：vterrain + 两张二进制 + 默认材质
            "assets/terrain/ground/terrain.vterrain",
            "assets/terrain/ground/height.r32",
            "assets/terrain/ground/splat.rgba8",
            "assets/materials/template_ground.vmat",
            "assets/textures/template_ground_albedo.png",
            "assets/textures/template_ground_normal.png",
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
        # pickup/portal 两份脚本保留：它们是"脚本能干什么"的示例，新建工程
        # 里没引用（场景里已经没有光球/传送门），想抄请自行挂到实体上。
        files[base + "/assets/scripts/pickup.vks"] = PICKUP_VKS
        files[base + "/assets/scripts/portal.vks"] = PORTAL_VKS
        # 地形（新工程自带一整块土黄地面）+ 它的默认材质
        files[base + "/assets/terrain/ground/terrain.vterrain"] = \
            dump_json(build_terrain_json())
        files[base + "/assets/materials/template_ground.vmat"] = \
            dump_json(TEMPLATE_GROUND_MAT)
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

    # ---- 地形二进制（不进 generate_texts：`--check` 走的是文本比对）----
    if not args.check:
        height, splat = build_terrain_bytes()
        albedo, normal = build_ground_textures()
        for kind in TYPES:
            for name, data in (("height.r32", height),
                               ("splat.rgba8", splat)):
                p = os.path.join(OUT_ROOT, kind, "assets", "terrain",
                                 "ground", name)
                os.makedirs(os.path.dirname(p), exist_ok=True)
                with open(p, "wb") as f:
                    f.write(data)
            for name, data in (("template_ground_albedo.png", albedo),
                               ("template_ground_normal.png", normal)):
                p = os.path.join(OUT_ROOT, kind, "assets", "textures", name)
                os.makedirs(os.path.dirname(p), exist_ok=True)
                with open(p, "wb") as f:
                    f.write(data)

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
