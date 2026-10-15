"""端到端验证：反射探针（R5-1：MYVK_PROBES）。

R5-1 引入的是**第三条环境光来源**（前两条是 R3-x 的 IBL 与 R4-x 的 SSR）：

    探针位置 → 6 面相机 → 6 张捕获图 → 逐面拷进立方体
             → GGX 预过滤成 mip 链（roughness = mip 索引）
             → 片元落在探针盒体内时**替换** IBL 的预过滤项

判定难点：IBL 的预过滤图和探针图都是"某个方向的入射辐射"，肉眼看不出
是哪一条在起作用。所以本脚本刻意构造**只有探针能给答案**的场地：

  · 一间封闭的屋子（地板 + 天花板 + 四面墙），唯一的光源是四块
    **自发光面板**（红/绿/蓝/黄，分别贴在四面墙上），外加**天花板灯**
    与**地面标记**各一块 —— 后两块纯粹是为了让探针的 ±Z 两个面也有
    内容（只有四面墙有灯的屋子，上下两面本来就该是黑的）。
  · 天空调成纯黑（ZENITH/GROUND 都 = 0）→ 整张 IBL 环境是黑的
    （天空立方体、辐照度、预过滤全黑）→ **关掉探针时环境光恒等于 0**。
  · 屋子正中央一颗金属镜面球（metallic=1, roughness≈0.05）。
  · **关键机位**：相机在 -Y 侧、朝 +Y 看球。镜面球**正视点**（球在屏幕上
    的正中心那个像素）的法线指向相机，反射方向 = **-Y** —— 也就是
    **相机背后**。那里恰好贴着黄色的自发光面板。
    于是"球心那个像素是不是黄的"直接等于"探针有没有把相机背后那面墙
    烘进去"。相机自己永远看不到那块面板，所以这不是屏幕空间效果
    （SSR 在这里必然失败）—— 这正是 R5-1 存在的理由。

断言分六组：

  A. **关着 = 基线**（本项目所有新效果的第一条约定）
     · A1 场景里根本没探针实体 vs 有探针实体但 MYVK_PROBES=0 → 逐像素一致；
     · A2 MYVK_PROBES=0 → 从不出现 PROBE-CAPTURE / PROBE-RES（关着时
          一次捕获都不跑、连资源都不分配）；
     · A3 组件 enabled=false（全局开着）vs 无探针实体 → 逐像素一致；
     · A4 同配置跑两次逐像素一致（去噪，其它断言的前提）。

  B. **捕获真的在跑**（画面之外的观测量，MYVK_PROBE_DBG）
     · B1 启动日志 `PROBE: created (enabled=1 capture=64 ...)`；
     · B2 `PROBE-RES capture=64x64 prefilter=64 mips=5 maxProbes=1`；
     · B3 `PROBE-CAPTURE probes=1 size=64x64 ...`（捕获真的发生）；
     · B4 `PROBE-DECIDE`：第一次 changed=1/need=1，之后恒
          changed=0/need=0 —— 去抖有效，静态场景不会每帧重烘；
     · B5 `PROBE-DUMP ... readback=1`，且 6 面平均亮度里至少 3 面 > 0；
     · B6 `PROBE-NZ` 六面非黑纹素**全部 > 0**（每面都真有内容）——
          这一条抓的是"某个面的相机矩阵退化成一条线 / 落到空方向"；
     · B7 `PROBE-MID` 六面中心色**互不相同**（面序真的对应；
          面序错位时会出现两个面同色）。

  C. **画面真的变了，而且变在该变的地方**
     · C1 开 vs 关：视口内显著差异像素数 > 阈值、平均差 > 阈值；
     · C3 差异**最集中在球上**：球心采样区的平均差 ≥ 球外区域的 4 倍，
          且绝对值 ≥ 20。⚠ **不**断言"差异必须局部"——探针按定义会给
          盒内**每一个**表面加环境镜面项（本场地 IBL 全黑，差额全归探针），
          整屋一起变亮是正确行为；真正该钉的是"作用范围由 extents 决定"，
          那交给 D2/D3 的小盒/大盒对照；
     · C4 球心在探针开着时**偏黄**（相机背后那块面板），关着时≈黑 ——
          这是"反射内容真的来自探针捕获"的直接读数；
     · C5 `MYVK_PROBE_INTENSITY=2` 让球变亮（强度参数真的进了着色）。

  D. **盒投影 / 盒体作用范围**
     · D1 把探针**偏离球心 5 m** 后，boxProjection=true vs false →
          球区像素显著不同。⚠ 必须偏心：探针与球心重合时视差只有
          atan(0.6/11.4) ≈ 3°，而面板张角有 20° —— 采样点还落在面板里，
          开不开画面几乎一样（实测 0.65）。这不是缺陷，是几何必然；
          偏心之后视差到 ~24°，才谈得上"修正"；
     · D2 小盒体（半轴 2 m）→ 显著变化的像素**只出现在球附近**
          （包围盒占比 ≤ 0.28）；
     · D3 大盒体（半轴 40 m）→ 影响铺开到整屋（包围盒占比 ≥ 0.40）。
          D2/D3 一起证明 `extents` 真的进了 shader、盒内测试真的在裁剪
          作用范围（少了 D2，"整屋都变"既可能是对的、也可能是盒内测试
          失效后全局生效）。

  E. **因果实验：重捕的内容跟着场景走**
     · E1 把黄色面板从 -Y 墙搬到 +Y 墙 → 球区画面显著不同，
          且球心**不再偏黄**（反射的是另一侧的墙）。这一条同时证明
          "探针图不是缓存住的旧内容"和"面序没有左右镜像"。

  F. **编辑器面板**（MYVK_EDITOR_SELECT，不参与像素比对）
     · F1 选中探针实体 → `INS-PROBE-HEADER` 出现；
     · F2 选中**没有**探针组件的实体 → 该节不出现（否面对照）。
     注：探针分区默认**折叠**（与 Inspector 里其它组件分区一致），参数行
     与 Capture 按钮要展开才画 —— 展开需要 UI 点击，那部分覆盖留给
     verify_inspector 那一类带点击的脚本（与 Decal / Mesh 分区同一处理）。

场地参数（Z-up 右手系）：
    屋子 24×24 m、高 8 m；球半径 0.6 m、球心 z=1.2；探针与球心重合。
    相机 eye=(0,-4,1.6)、yaw=90（朝 +Y 看）、pitch=5.7（略微俯视球心）。
    球在屏幕上的半径 ≈ 0.19×画面高 —— 采样区取 ±0.08h 稳稳落在球面内。

用法：
    python tools/verify_reflection_probe.py
    python tools/verify_reflection_probe.py --keep-scenes
"""

import argparse
import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verify_hiz import run_once                                    # noqa: E402
from verify_add_menu import kill_stale_editors                      # noqa: E402
from verify_local_shadow import diff_stats, region_diff             # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = os.path.join(ROOT, "..", "_venn_verify")

# ---- 日志格式（必须与 Renderer / ReflectionProbeSystem 的格式串一致）----
CREATED_RE = re.compile(r"PROBE: created \(enabled=(\d+) capture=(\d+) "
                        r"prefilter=(\d+) mips=(\d+) max=(\d+)")
RES_RE = re.compile(r"PROBE-RES capture=(\d+)x(\d+) prefilter=(\d+) "
                    r"mips=(\d+) maxProbes=(\d+)")
CAP_RE = re.compile(r"PROBE-CAPTURE probes=(\d+) size=(\d+)x(\d+) "
                    r"prefilter=(\d+) mips=(\d+) total=(\d+)")
DECIDE_RE = re.compile(r"PROBE-DECIDE changed=(\d+) frames=(\d+) auto=(\d+) "
                       r"captures=(\d+) need=(\d+)")
DUMP_RE = re.compile(r"PROBE-DUMP enabled=(\d+) count=(\d+) captures=(\d+) "
                     r"size=(\d+) pref=(\d+) mips=(\d+) readback=(\d+) "
                     r"lum=\(([-\d.,]+)\)")
NZ_RE = re.compile(r"PROBE-NZ \+X=(\d+) -X=(\d+) \+Y=(\d+) -Y=(\d+) "
                   r"\+Z=(\d+) -Z=(\d+)")
MID_RE = re.compile(
    r"PROBE-MID \+X=\(([-\d.]+),([-\d.]+),([-\d.]+)\) "
    r"-X=\(([-\d.]+),([-\d.]+),([-\d.]+)\) "
    r"\+Y=\(([-\d.]+),([-\d.]+),([-\d.]+)\) "
    r"-Y=\(([-\d.]+),([-\d.]+),([-\d.]+)\) "
    r"\+Z=\(([-\d.]+),([-\d.]+),([-\d.]+)\) "
    r"-Z=\(([-\d.]+),([-\d.]+),([-\d.]+)\)")

# ---- 场地 -------------------------------------------------------------
ROOM = 24.0            # 地板 / 天花板的边长
WALL_HALF = 12.0       # 墙心到原点的距离
WALL_T = 0.3           # 墙厚
CEIL_Z = 8.0
PANEL_THICK = 0.3      # 面板厚度（贴墙那一条边）
PANEL_W = 4.0          # 面板宽
PANEL_H = 3.0          # 面板高
# 面板中心高度：**必须让探针的水平视线从面板中部穿过**。探针在 z=1.2，
# 若面板中心放在 2.5（下半边从 z=1.0 起），那条水平视线离面板下边缘只有
# 0.2 m ≈ 0.7 个纹素 → 中心纹素恰好骑在边上，读回来的颜色是"面板 + 黑墙"
# 的一半。自检读数就成了半成品，B7 的"六面中心色互不相同"也就失去意义。
PANEL_Z = 1.6
PANEL_EMIT = 3.0       # 自发光强度（线性 HDR；1.0 太暗、10 会过曝成死白）

# 天花板灯 / 地面标记：让 ±Z 两个面也不是空的。判据是"六面都有内容"，
# 一间只有四面墙有灯的屋子，上下两面**本来就该是黑的** —— 那样 B6 会
# 误报。加两块小而显眼的发光体，六面就都有东西可看。
LAMP_EMIT = (0.6, 0.6, 3.0)    # 偏蓝白（与蓝面板 (0,0,3) 不同色）
MARK_EMIT = (3.0, 1.2, 0.0)    # 橙（与红 (3,0,0)、黄 (3,3,0) 都不同色）

SPHERE_R = 0.6
SPHERE_Z = 1.2
SPHERE_ROUGH = 0.05

PROBE_EXT = [12.0, 12.0, 9.0]     # 盒体半轴：刚好罩住整间屋子
# 小盒：半轴 2 m（球半径 0.6，盒子边界离球面还有 1.4 m —— 大于默认
# blendDistance=1.0，所以球面上的探针权重是满的）。地板在球正下方
# 1.2 m 处仍落在盒内，但已经在边界淡出区里 → 只留一小块微弱变化。
PROBE_EXT_SMALL = [2.0, 2.0, 2.0]
PROBE_EXT_BIG = [40.0, 40.0, 30.0]
PROBE_DX = -5.0                   # D1 用的"探针偏离球心"距离（米）

CAM = "0,-4,1.6,90,5.7"           # eye 在 -Y 侧，朝 +Y 看球心

# ---- 阈值（实测值写在括号里）----
PIX_IDENTICAL = 0.05      # A 组"逐像素一致"的门槛（均差）
SIGNAL_MEAN_MIN = 5.0     # C1 全视口平均差下限（实测 16.70）
SIGNAL_N_MIN = 20000      # C1 `>18` 像素数下限（实测 68284）
# C3：球区平均差 / 球外平均差的下限。⚠ 不能定得太高：探针按定义会给
# **盒内每一个表面**加环境镜面项（这里 IBL 是全黑的，所以那部分差额全是
# 探针的），只是镜面球金属度 1（F0=1）而墙/地板是介质（F0=0.04）。
# 实测比值 9.3 —— 25 倍的 F0 差被"球区只占球面投影"摊薄了。
CONCENTRATION_MIN = 4.0
SPHERE_DIFF_MIN = 20.0    # C3 球区平均差的绝对下限（实测 65.65）
CENTER_LUM_MIN = 12.0     # C4 球心亮度提升下限（实测 85.1）
CENTER_YELLOWNESS = 8.0   # C4 球心 r-b 差 / g-b 差的下限（实测 108.9）
# D 组：小盒 / 大盒的"受影响像素包围盒占比"。小盒实测 0.11~0.15
# （球自己的投影占地 0.117），大盒实测 0.568。
SMALL_BOX_BBOX_MAX = 0.28
BIG_BOX_BBOX_MIN = 0.40
# D1 必须在**探针偏离球心**的场地上量：探针与球心重合时视差只有
# atan(0.6/11.4) ≈ 3°，而面板张角有 20° —— 采样点还在面板里，
# 盒投影开不开画面几乎一样（实测 0.65）。这不是缺陷，是几何必然。
D1_MIN = 3.0              # D1 偏心场地球区差异下限（实测 22.25）
E1_CENTER_DROP = 8.0      # E1 搬走黄面板后球心亮度下降下限（实测 19.9）



def _rgba(c, a=255):
    return [c[0], c[1], c[2], a]


def make_material(name, base, metallic=0.0, roughness=1.0,
                  emissive=(0.0, 0.0, 0.0)):
    return {
        "kind": "procedural", "name": name,
        "baseColor": list(base) + [1.0],
        "metallic": metallic, "roughness": roughness,
        "normalScale": 1.0, "ao": 1.0,
        "emissive": list(emissive), "doubleSided": False,
        "alphaMode": "opaque", "alphaBlend": False, "alphaCutoff": 0.5,
        "ditherAlpha": False, "shadingModel": "default",
    }


def box(name, pos, scale, mat):
    """单位立方体 × 非均匀缩放 = 一块板 / 一面墙（不用旋转，省得纠结
    欧拉角约定）。size=1 是"单位边长"，缩放直接就是三轴尺寸。"""
    return {
        "name": name,
        "transform": {"position": list(pos), "rotation": [0.0, 0.0, 0.0],
                      "scale": list(scale)},
        "visible": True, "castShadow": False, "parent": -1,
        "mesh": {"kind": "builtin", "shape": "cube", "size": 1.0,
                 "name": name + "_mesh"},
        "material": mat,
    }


def build_scene(path, *, with_probe=True, probe_enabled=True,
                box_projection=True, extents=None, yellow_at_plus_y=False,
                probe_dx=0.0):
    """yellow_at_plus_y=True → 把那块黄色面板从 -Y 墙搬到 +Y 墙（E1 用）。
    probe_dx != 0 → 探针沿 X 偏离球心（D1 的视差实验用）。"""
    extents = extents or PROBE_EXT

    ents = []
    # 地板：plane 默认是 XY 平面（法线 +Z），铺在 z=0
    ents.append({
        "name": "Floor",
        "transform": {"position": [0.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0],
                      "scale": [1.0, 1.0, 1.0]},
        "visible": True, "castShadow": False, "parent": -1,
        "mesh": {"kind": "builtin", "shape": "plane", "size": ROOM,
                 "name": "room_floor_mesh"},
        "material": make_material("room_floor_mat", (0.12, 0.12, 0.13)),
    })
    # 天花板
    ents.append(box("Ceiling", (0.0, 0.0, CEIL_Z), (ROOM, ROOM, WALL_T),
                    make_material("room_ceil_mat", (0.10, 0.10, 0.11))))
    # 四面墙
    ents.append(box("WallPX", (WALL_HALF, 0.0, CEIL_Z * 0.5),
                    (WALL_T, ROOM, CEIL_Z),
                    make_material("room_wallpx_mat", (0.16, 0.16, 0.17))))
    ents.append(box("WallNX", (-WALL_HALF, 0.0, CEIL_Z * 0.5),
                    (WALL_T, ROOM, CEIL_Z),
                    make_material("room_wallnx_mat", (0.16, 0.16, 0.17))))
    ents.append(box("WallPY", (0.0, WALL_HALF, CEIL_Z * 0.5),
                    (ROOM, WALL_T, CEIL_Z),
                    make_material("room_wallpy_mat", (0.16, 0.16, 0.17))))
    ents.append(box("WallNY", (0.0, -WALL_HALF, CEIL_Z * 0.5),
                    (ROOM, WALL_T, CEIL_Z),
                    make_material("room_wallny_mat", (0.16, 0.16, 0.17))))

    # 镜面球：正中央，纯镜面
    ents.append({
        "name": "Mirror",
        "transform": {"position": [0.0, 0.0, SPHERE_Z],
                      "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0]},
        "visible": True, "castShadow": False, "parent": -1,
        "mesh": {"kind": "builtin", "shape": "sphere", "size": SPHERE_R,
                 "segments": 48, "rings": 24, "name": "probe_mirror_mesh"},
        "material": make_material("probe_mirror_mat", (1.0, 1.0, 1.0),
                                 metallic=1.0, roughness=SPHERE_ROUGH),
    })

    # 四面墙上的自发光面板（唯一光源）
    inset = WALL_HALF - WALL_T * 0.5 - PANEL_THICK * 0.5
    panels = [
        ("PanelPX", (inset, 0.0, PANEL_Z),
         (PANEL_THICK, PANEL_W, PANEL_H), (PANEL_EMIT, 0.0, 0.0)),
        ("PanelNX", (-inset, 0.0, PANEL_Z),
         (PANEL_THICK, PANEL_W, PANEL_H), (0.0, PANEL_EMIT, 0.0)),
        ("PanelPY", (0.0, inset, PANEL_Z),
         (PANEL_W, PANEL_THICK, PANEL_H), (0.0, 0.0, PANEL_EMIT)),
        ("PanelNY", (0.0, -inset, PANEL_Z),
         (PANEL_W, PANEL_THICK, PANEL_H),
         (PANEL_EMIT, PANEL_EMIT, 0.0)),
    ]
    if yellow_at_plus_y:
        # 把黄面板从 -Y 墙搬到 +Y 墙（放在另一处高度，免得和蓝面板共面打架）。
        # 球心那个像素反射的是** -Y 方向**，搬走之后那里就空了。
        panels = [
            ("PanelPX", (inset, 0.0, PANEL_Z),
             (PANEL_THICK, PANEL_W, PANEL_H), (PANEL_EMIT, 0.0, 0.0)),
            ("PanelNX", (-inset, 0.0, PANEL_Z),
             (PANEL_THICK, PANEL_W, PANEL_H), (0.0, PANEL_EMIT, 0.0)),
            ("PanelPY", (0.0, inset, PANEL_Z),
             (PANEL_W, PANEL_THICK, PANEL_H), (0.0, 0.0, PANEL_EMIT)),
            ("PanelPlusY", (0.0, inset, PANEL_Z + 4.0),
             (PANEL_W, PANEL_THICK, PANEL_H),
             (PANEL_EMIT, PANEL_EMIT, 0.0)),
        ]
    for nm, pos, sc, em in panels:
        ents.append(box(nm, pos, sc,
                        make_material(nm.lower() + "_mat", (0.0, 0.0, 0.0),
                                      emissive=em)))

    # 天花板灯 + 地面标记：±Z 两面也要有内容，见 LAMP_EMIT 的注释。
    ents.append(box("CeilingLamp", (0.0, 0.0, CEIL_Z - WALL_T - 0.15),
                    (4.0, 4.0, 0.3),
                    make_material("ceiling_lamp_mat", (0.0, 0.0, 0.0),
                                  emissive=LAMP_EMIT)))
    ents.append(box("FloorMark", (0.0, 0.0, 0.2), (3.0, 3.0, 0.3),
                    make_material("floor_mark_mat", (0.0, 0.0, 0.0),
                                  emissive=MARK_EMIT)))

    if with_probe:
        ents.append({
            "name": "Probe",
            "transform": {"position": [probe_dx, 0.0, SPHERE_Z],
                          "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0]},
            "visible": True, "castShadow": False, "parent": -1,
            # ⚠ 没有 "mesh" —— 探针是纯逻辑实体（和贴花一样）
            "probe": {"extents": list(extents), "intensity": 1.0,
                      "boxProjection": box_projection, "blendDistance": 1.0,
                      "enabled": probe_enabled},
        })

    doc = {
        "version": 1,
        "generator": "verify_reflection_probe.py",
        # 平行光强度 0：屋里唯一的光是自发光面板。
        # ambientScale 必须是 1：pbr.frag 里 `ambient = (diffuse+specular) *
        # ao * ambientScale * ssao` —— 把 ambientScale 设 0 会**连探针一起**
        # 乘没（探针提供的就是 specular 那一项）。
        "light": {"direction": [0.0, 0.0, -1.0], "color": [0.0, 0.0, 0.0],
                  "intensity": 0.0, "castsShadow": False,
                  "ambientScale": 1.0},
        "entities": ents,
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)


# ---- 日志读取 ---------------------------------------------------------

def last_value(lines, rx, group=1, cast=str):
    """最后一条匹配的第 group 组（默认第 1 组）。"""
    v = None
    for ln in lines:
        m = rx.search(ln)
        if m:
            v = cast(m.group(group))
    return v


def has(lines, needle):
    return any(needle in ln for ln in lines)


def all_values(lines, rx):
    out = []
    for ln in lines:
        m = rx.search(ln)
        if m:
            out.append(m.groups())
    return out


def parse_vp(vp, img):
    """VP-RECT 日志 → (x0,y0,x1,y1) 绝对坐标。拿不到就退整窗。"""
    if vp:
        vals = [float(v) for v in re.findall(r"-?\d+\.?\d*", str(vp))]
        if len(vals) >= 4:
            if vals[2] < vals[0] or vals[3] < vals[1]:
                return (vals[0], vals[1], vals[0] + vals[2], vals[1] + vals[3])
            return tuple(vals[:4])
    if img is not None:
        return (0.0, 0.0, float(img.width), float(img.height))
    return None


def sphere_boxes(vpv):
    """由视口矩形推出采样区。

    球在屏幕上正好在视口中心（相机看的就是球心），屏幕半径 ≈ 0.19×h，
    所以 ±0.08h 一定落在球面内。`out_left` 取画面左侧一条竖带：那是
    -X 墙，离球很远，用来做"差异是不是只出现在球上"的对照。
    """
    x0, y0, x1, y1 = [float(v) for v in vpv]
    w, h = x1 - x0, y1 - y0
    cx, cy = (x0 + x1) * 0.5, (y0 + y1) * 0.5
    return {
        "sphere": (cx - 0.08 * h, cy - 0.08 * h, cx + 0.08 * h, cy + 0.08 * h),
        "sphere_wide": (cx - 0.15 * h, cy - 0.15 * h,
                        cx + 0.15 * h, cy + 0.15 * h),
        "out_left": (x0 + 0.03 * w, y0 + 0.30 * h, x0 + 0.14 * w, y0 + 0.70 * h),
        "out_right": (x1 - 0.14 * w, y0 + 0.30 * h, x1 - 0.03 * w,
                      y0 + 0.70 * h),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--keep-scenes", action="store_true")
    args = ap.parse_args()

    exe = args.exe
    if not os.path.exists(exe):
        print("FAIL: 找不到可执行文件 %s" % exe)
        return 2

    os.makedirs(OUT_DIR, exist_ok=True)
    failures = []
    print("=" * 68)
    print("反射探针验证（R5-1：MYVK_PROBES）")
    print("=" * 68)

    kill_stale_editors()

    scenes = {
        "base":    dict(with_probe=False),
        "off":     dict(with_probe=True),
        "dis":     dict(with_probe=True, probe_enabled=False),
        "on":      dict(with_probe=True),
        "int2":    dict(with_probe=True),
        "bigbox":  dict(with_probe=True, extents=PROBE_EXT_BIG),
        "smallbox": dict(with_probe=True, extents=PROBE_EXT_SMALL),
        "moved":   dict(with_probe=True, yellow_at_plus_y=True),
        # D1：探针偏离球心 5 m —— 只有在这里视差才大到能被看见
        "bpx":     dict(with_probe=True, probe_dx=PROBE_DX),
        "npx":     dict(with_probe=True, probe_dx=PROBE_DX,
                        box_projection=False),
    }
    paths = {}
    for k, kw in scenes.items():
        paths[k] = os.path.join(OUT_DIR, "probe_%s.json" % k)
        build_scene(paths[k], **kw)
    print("\n场景已写出：无探针 / 探针(全局关) / enabled=false / 正常 / "
          "强度x2 / 无盒投影 / 大盒体 / 黄面板搬到 +Y / 偏心探针(开/关盒投影)")

    # ---- 通用环境 -----------------------------------------------------
    BASE_ENV = {
        "MYVK_GRID": "0",
        "MYVK_DEPTH_PREPASS": "1",
        "MYVK_SHADER_VARIANTS": "1",
        # HUD 里的 FPS / 帧时间是浮动的 → A 组"逐像素一致"根本没法定
        "MYVK_EDITOR_NO_HUD": "1",
        # SSAO 会往环境光上乘一层遮蔽，把"探针到底贡献了多少"搅浑
        "MYVK_SSAO": "0",
        # 固定曝光：自动曝光的收敛曲线会让开/关两轮的亮度不可比
        "MYVK_EXPOSURE": "1.0",
        # 天空调成纯黑 → 整张 IBL 环境是黑的 → 关掉探针时环境镜面项恒为 0
        "MYVK_SKY_ZENITH": "0,0,0",
        "MYVK_SKY_GROUND": "0,0,0",
        # 探针：小一点快一点；捕获 64、预过滤 64×5 级（64 支持 6 级，够用）
        "MYVK_PROBE_SIZE": "64",
        "MYVK_PROBE_PREFILTER_RES": "64",
        "MYVK_PROBE_MAX": "1",
        "MYVK_PROBE_SAMPLES": "64",
    }

    def shot(name):
        return os.path.join(OUT_DIR, "probe_%s.png" % name)

    all_lines = {}

    def step(name, scene_key, env_extra=None, save=True):
        env = dict(BASE_ENV)
        env["MYVK_EDITOR_CAM"] = CAM
        env.update(env_extra or {})
        for attempt in range(3):
            lines, vp, img, errs = run_once(
                exe, paths[scene_key], env, shot(name) if save else None)
            if img is not None or not save:
                break
            print("  [%s] 抓图为空，整轮重试 (%d/3)" % (name, attempt + 2))
            kill_stale_editors()
            time.sleep(1.0)
        all_lines[name] = lines
        if errs:
            print("  [%s] ⚠ 错误输出 %d 行，例：%s"
                  % (name, len(errs), errs[0][:130]))
            failures.append("%s: 有错误/验证层输出" % name)
        return img, vp

    print("\n[运行] 12 次编辑器（2 次不抓图，只查面板）")
    img_base, vp = step("base", "base", {"MYVK_PROBES": "1"})
    img_off, _ = step("off", "off", {"MYVK_PROBES": "0"})
    img_off2, _ = step("off2", "off", {"MYVK_PROBES": "0"})
    img_dis, _ = step("dis", "dis", {"MYVK_PROBES": "1"})
    img_on, _ = step("on", "on", {"MYVK_PROBES": "1", "MYVK_PROBE_DBG": "1"})
    img_int2, _ = step("int2", "int2",
                       {"MYVK_PROBES": "1", "MYVK_PROBE_INTENSITY": "2.0"})
    img_bigbox, _ = step("bigbox", "bigbox", {"MYVK_PROBES": "1"})
    img_smallbox, _ = step("smallbox", "smallbox", {"MYVK_PROBES": "1"})
    img_moved, _ = step("moved", "moved", {"MYVK_PROBES": "1"})
    img_npx, _ = step("npx", "npx", {"MYVK_PROBES": "1"})
    img_bpx, _ = step("bpx", "bpx", {"MYVK_PROBES": "1"})
    # 面板：MYVK_EDITOR_SELECT 会在视口里画出选中描边 → 不参与像素比对
    step("sel", "on", {"MYVK_PROBES": "1", "MYVK_EDITOR_SELECT": "Probe"},
         save=False)
    # 负面对照：选中**没有**探针组件的实体，探针那一节不该出现
    step("selmirror", "on",
         {"MYVK_PROBES": "1", "MYVK_EDITOR_SELECT": "Mirror"}, save=False)

    need = {"base": img_base, "off": img_off, "off2": img_off2, "dis": img_dis,
            "on": img_on, "int2": img_int2, "bigbox": img_bigbox,
            "smallbox": img_smallbox, "moved": img_moved, "npx": img_npx,
            "bpx": img_bpx}
    missing = [k for k, v in need.items() if v is None]
    if missing:
        print("FAIL: 没抓到截图：%s" % ", ".join(missing))
        return 2

    vpv = parse_vp(vp, img_base)
    if not vpv:
        print("FAIL: 拿不到视口矩形")
        return 2
    boxes = sphere_boxes(vpv)
    vp_area = max(1.0, (vpv[2] - vpv[0]) * (vpv[3] - vpv[1]))
    print("  视口 %s  球心采样区 %s" % (
        tuple(int(v) for v in vpv), tuple(int(v) for v in boxes["sphere"])))

    def ds(a, b, thr=18):
        return diff_stats(a, b, vpv, thr)

    def rd(a, b, box):
        return region_diff(a, b, box)

    def mean_color(img, box):
        x0, y0, x1, y1 = [int(v) for v in box]
        x0, y0 = max(x0, 0), max(y0, 0)
        x1, y1 = min(x1, img.width), min(y1, img.height)
        px = img.load()
        sr = sg = sb = 0
        n = 0
        for y in range(y0, y1):
            for x in range(x0, x1):
                p = px[x, y]
                sr += p[0]
                sg += p[1]
                sb += p[2]
                n += 1
        n = max(n, 1)
        return (sr / n, sg / n, sb / n)

    # ---------------------------------------------------------- A 组
    print("\n[A] 关着 = 基线")
    n1, _, m1, _ = ds(img_off, img_base)
    print("  A1 MYVK_PROBES=0（场景里有探针）vs 场景里没有探针：>18=%d 均差=%.6f"
          % (n1, m1))
    if n1 != 0 or m1 > PIX_IDENTICAL:
        print("     ✗ 全局关卡后仍改画面 —— 探针没被完全跳过")
        failures.append("A1: 全局关不等于基线")
    else:
        print("     ✓ 逐像素一致")

    off_lines = all_lines["off"]
    if has(off_lines, "PROBE-CAPTURE") or has(off_lines, "PROBE-RES"):
        print("     ✗ 关着时出现了 PROBE-CAPTURE / PROBE-RES")
        failures.append("A2: 关着时仍在捕获/分配资源")
    else:
        print("  A2 关着时零捕获、零资源分配（无 PROBE-CAPTURE / PROBE-RES）✓")

    n3, _, m3, _ = ds(img_dis, img_base)
    print("  A3 enabled=false（全局开）vs 无探针：>18=%d 均差=%.6f" % (n3, m3))
    if n3 != 0 or m3 > PIX_IDENTICAL:
        print("     ✗ 组件开关没生效")
        failures.append("A3: enabled=false 不等于基线")
    else:
        print("     ✓ 逐像素一致")

    n4, _, m4, _ = ds(img_off2, img_off)
    print("  A4 同配置两次运行：>18=%d 均差=%.6f" % (n4, m4))
    if n4 != 0 or m4 > PIX_IDENTICAL:
        print("     ✗ 两次运行不一致 —— 后面的像素断言全部不可信")
        failures.append("A4: 同配置两次运行不一致")
    else:
        print("     ✓ 逐像素一致（去噪前提成立）")

    # ---------------------------------------------------------- B 组
    print("\n[B] 捕获真的在跑（MYVK_PROBE_DBG 的自检读数）")
    on_lines = all_lines["on"]

    m = None
    for ln in on_lines:
        mm = CREATED_RE.search(ln)
        if mm:
            m = mm
    if not m:
        print("     ✗ 没找到启动日志 PROBE: created")
        failures.append("B1: 缺 PROBE: created")
    else:
        print("  B1 PROBE: created enabled=%s capture=%s prefilter=%s mips=%s "
              "max=%s" % m.groups())
        if m.group(1) != "1":
            print("     ✗ 启动时 enabled 不是 1")
            failures.append("B1: enabled 标志不对")
        else:
            print("     ✓")

    m = None
    for ln in on_lines:
        mm = RES_RE.search(ln)
        if mm:
            m = mm
    if not m:
        print("     ✗ 没有 PROBE-RES（资源没建起来）")
        failures.append("B2: 缺 PROBE-RES")
    else:
        print("  B2 PROBE-RES capture=%sx%s prefilter=%s mips=%s maxProbes=%s"
              % m.groups())
        ok = (int(m.group(1)) == 64 and int(m.group(3)) == 64
              and int(m.group(5)) == 1)
        print("     %s" % ("✓ 与 MYVK_PROBE_* 一致" if ok else
                           "✗ 尺寸/容量与 env 不符"))
        if not ok:
            failures.append("B2: PROBE-RES 参数不对")

    caps = all_values(on_lines, CAP_RE)
    if not caps:
        print("     ✗ 没有 PROBE-CAPTURE（捕获一次都没跑）")
        failures.append("B3: 缺 PROBE-CAPTURE")
    else:
        c = caps[-1]
        print("  B3 PROBE-CAPTURE probes=%s size=%sx%s prefilter=%s mips=%s "
              "total=%s（共 %d 次）" % (tuple(c) + (len(caps),)))
        if int(c[0]) != 1:
            print("     ✗ probes 应为 1")
            failures.append("B3: probes 数不对")
        else:
            print("     ✓ 捕获真的发生了")

    decs = all_values(on_lines, DECIDE_RE)
    if not decs:
        print("     ✗ 没有 PROBE-DECIDE")
        failures.append("B4: 缺 PROBE-DECIDE")
    else:
        first = decs[0]
        later = decs[1:]
        print("  B4 PROBE-DECIDE 第一条 changed=%s need=%s；之后 %d 条"
              % (first[0], first[4], len(later)))
        bad = [d for d in later if d[0] != "0" or d[4] != "0"]
        if first[0] != "1" or first[4] != "1":
            print("     ✗ 第一次没有判定为需要捕获")
            failures.append("B4: 首帧未捕获")
        elif bad:
            print("     ✗ 之后仍有多条 need=1 —— 去抖失效，每帧都在重烘")
            failures.append("B4: 去抖失效（%d 条）" % len(bad))
        else:
            print("     ✓ 只烘一次，静态场景不再重烘")

    m = None
    for ln in on_lines:
        mm = DUMP_RE.search(ln)
        if mm:
            m = mm
    if not m:
        print("     ✗ 没有 PROBE-DUMP")
        failures.append("B5: 缺 PROBE-DUMP")
    else:
        lum = [float(v) for v in m.group(8).split(",")]
        nz_lum = sum(1 for v in lum if v > 0.0)
        print("  B5 PROBE-DUMP readback=%s lum=%s（非零 %d 面）"
              % (m.group(7), tuple(round(v, 4) for v in lum), nz_lum))
        if m.group(7) != "1":
            print("     ✗ readback 没生效")
            failures.append("B5: 回读未生效")
        elif nz_lum < 3:
            print("     ✗ 只有 %d 面有内容 —— 捕获多半是空的" % nz_lum)
            failures.append("B5: 捕获内容太少")
        else:
            print("     ✓ 回读拿到了真实内容")

    m = None
    for ln in on_lines:
        mm = NZ_RE.search(ln)
        if mm:
            m = mm
    if not m:
        print("     ✗ 没有 PROBE-NZ")
        failures.append("B6: 缺 PROBE-NZ")
    else:
        nz = [int(v) for v in m.groups()]
        print("  B6 PROBE-NZ +X=%d -X=%d +Y=%d -Y=%d +Z=%d -Z=%d" % tuple(nz))
        if min(nz) <= 0:
            print("     ✗ 有面完全没有内容（面相机退化 / 朝向错）")
            failures.append("B6: 有面为空")
        else:
            print("     ✓ 六面都有内容")

    m = None
    for ln in on_lines:
        mm = MID_RE.search(ln)
        if mm:
            m = mm
    if not m:
        print("     ✗ 没有 PROBE-MID")
        failures.append("B7: 缺 PROBE-MID")
    else:
        g = [float(v) for v in m.groups()]
        mids = [tuple(round(v, 1) for v in g[i * 3:i * 3 + 3])
                for i in range(6)]
        names = ["+X", "-X", "+Y", "-Y", "+Z", "-Z"]
        print("  B7 PROBE-MID " + " ".join(
            "%s=%s" % (n, mids[i]) for i, n in enumerate(names)))
        if len(set(mids)) < 6:
            print("     ✗ 六面中心色有重复 —— 面序错位")
            failures.append("B7: 六面中心色有重复")
        else:
            print("     ✓ 六面中心色互不相同（面序对应正确）")

    # ---------------------------------------------------------- C 组
    print("\n[C] 画面真的变了，而且变在该变的地方")
    n1, mx1, m1, box = ds(img_on, img_off)
    frac = 0.0
    if box:
        frac = ((box[2] - box[0]) * (box[3] - box[1])) / vp_area
    print("  C1 探针开 vs 关：>18=%d max=%d 均差=%.4f 包围盒占比=%.3f"
          % (n1, mx1, m1, frac))
    if n1 < SIGNAL_N_MIN or m1 < SIGNAL_MEAN_MIN:
        print("     ✗ 画面几乎没变 —— 探针没进着色")
        failures.append("C1: 开关画面差异过小")
    else:
        print("     ✓ 差异显著")
    # ⚠ 这里**不**断言"差异必须局部"：探针按定义会给盒内**每一个**表面
    #   加环境镜面项（本场地的 IBL 是全黑的，所以全部差额都归探针），
    #   整屋一起变亮是正确行为，不是 bug。真正该钉的是"盒体的作用范围
    #   真的由 extents 决定" —— 那由 D2 的小盒/大盒对照来验。

    d_in = rd(img_on, img_off, boxes["sphere"])
    d_out = 0.5 * (rd(img_on, img_off, boxes["out_left"])
                   + rd(img_on, img_off, boxes["out_right"]))
    ratio = d_in / max(d_out, 1e-6)
    print("  C3 球内平均差=%.3f 球外=%.4f 比值=%.1f" % (d_in, d_out, ratio))
    if d_in < SPHERE_DIFF_MIN:
        print("     ✗ 球区变化太小（< %.1f）—— 镜面球没接到探针" % SPHERE_DIFF_MIN)
        failures.append("C3: 球区差异过小")
    elif ratio < CONCENTRATION_MIN:
        print("     ✗ 差异没集中在球上（比值 < %.1f）" % CONCENTRATION_MIN)
        failures.append("C3: 差异未集中在球上")
    else:
        print("     ✓ 差异最集中在镜面球上（金属 F0=1 vs 介质 0.04）")

    c_on = mean_color(img_on, boxes["sphere"])
    c_off = mean_color(img_off, boxes["sphere"])
    lum_on = 0.2126 * c_on[0] + 0.7152 * c_on[1] + 0.0722 * c_on[2]
    lum_off = 0.2126 * c_off[0] + 0.7152 * c_off[1] + 0.0722 * c_off[2]
    print("  C4 球心区 开=(%.1f,%.1f,%.1f) 亮度=%.1f ｜ 关=(%.1f,%.1f,%.1f) "
          "亮度=%.1f" % (c_on + (lum_on,) + c_off + (lum_off,)))
    yellow = min(c_on[0] - c_on[2], c_on[1] - c_on[2])
    if lum_on - lum_off < CENTER_LUM_MIN:
        print("     ✗ 球心没变亮 —— 相机背后那块面板没被反射出来")
        failures.append("C4: 球心亮度提升不足")
    elif yellow < CENTER_YELLOWNESS:
        print("     ✗ 球心不偏黄（r-b=%.1f g-b=%.1f）—— 面序可能左右镜像了"
              % (c_on[0] - c_on[2], c_on[1] - c_on[2]))
        failures.append("C4: 球心颜色不是黄面板")
    else:
        print("     ✓ 球心是相机背后那块黄色面板的反射（r-b=%.1f）✓"
              % (c_on[0] - c_on[2]))

    c_int2 = mean_color(img_int2, boxes["sphere"])
    lum_int2 = 0.2126 * c_int2[0] + 0.7152 * c_int2[1] + 0.0722 * c_int2[2]
    print("  C5 INTENSITY=2.0 球心亮度=%.1f（1.0 时是 %.1f）" % (lum_int2, lum_on))
    if lum_int2 <= lum_on:
        print("     ✗ 强度调大后没有变亮 —— intensity 没进着色")
        failures.append("C5: intensity 无效果")
    else:
        print("     ✓ 强度参数生效")

    # ---------------------------------------------------------- D 组
    print("\n[D] 盒投影 / 盒体参数")
    d_box = rd(img_bpx, img_npx, boxes["sphere_wide"])
    print("  D1 探针偏离球心 %.0f m：盒投影 开 vs 关 球区平均差=%.3f"
          % (PROBE_DX, d_box))
    if d_box < D1_MIN:
        print("     ✗ 视差这么大的场地里开关盒投影仍无差别 —— "
              "boxProjection 没进 shader")
        failures.append("D1: boxProjection 无效果")
    else:
        print("     ✓ 盒投影真的在修正视差")

    n_small, _, m_small, box_small = ds(img_smallbox, img_off)
    frac_small = 0.0
    if box_small:
        frac_small = ((box_small[2] - box_small[0]) * (box_small[3] - box_small[1])) / vp_area
    d_small = rd(img_smallbox, img_off, boxes["sphere"])
    print("  D2 小盒体（半轴 2 m）：>18=%d 均差=%.4f 包围盒占比=%.3f 球区差=%.3f"
          % (n_small, m_small, frac_small, d_small))
    if d_small < SPHERE_DIFF_MIN * 0.6:
        print("     ✗ 小盒体下球上没有反射 —— 盒体尺寸参数有问题")
        failures.append("D2: 小盒体失效")
    elif frac_small > SMALL_BOX_BBOX_MAX:
        print("     ✗ 小盒体的影响范围超出了盒子（包围盒 %.3f > %.2f）—— "
              "盒内测试没在裁剪作用范围" % (frac_small, SMALL_BOX_BBOX_MAX))
        failures.append("D2: 小盒体影响范围过大")
    else:
        print("     ✓ 只有球附近在变（盒体把作用范围裁住了）")

    n_big, _, m_big, box_big = ds(img_bigbox, img_off)
    frac_big = 0.0
    if box_big:
        frac_big = ((box_big[2] - box_big[0]) * (box_big[3] - box_big[1])) / vp_area
    print("  D3 大盒体（半轴 40 m）：>18=%d 均差=%.4f 包围盒占比=%.3f"
          % (n_big, m_big, frac_big))
    if frac_big < BIG_BOX_BBOX_MIN or frac_big < frac_small * 2.0:
        print("     ✗ 大盒体没有把影响铺开到整屋（%.3f vs 小盒 %.3f）—— "
              "extents 没有真的进 shader" % (frac_big, frac_small))
        failures.append("D3: extents 无效果")
    else:
        print("     ✓ extents 真的在决定作用范围（%.3f ≫ %.3f）"
              % (frac_big, frac_small))

    # ---------------------------------------------------------- E 组
    print("\n[E] 因果实验：重捕的内容跟着场景走")
    d_moved = rd(img_moved, img_on, boxes["sphere_wide"])
    c_moved = mean_color(img_moved, boxes["sphere"])
    lum_moved = 0.2126 * c_moved[0] + 0.7152 * c_moved[1] + 0.0722 * c_moved[2]
    print("  E1 黄面板从 -Y 墙搬到 +Y 墙：球区平均差=%.3f ｜ 球心 "
          "(%.1f,%.1f,%.1f) 亮度=%.1f"
          % (d_moved, c_moved[0], c_moved[1], c_moved[2], lum_moved))
    if d_moved < D1_MIN:
        print("     ✗ 搬走面板后球区没变 —— 探针图没有跟着场景更新")
        failures.append("E1: 探针内容没跟着场景走")
    elif lum_on - lum_moved < E1_CENTER_DROP:
        print("     ✗ 球心没有变暗（%.1f → %.1f）—— 反射的不是 -Y 方向"
              % (lum_on, lum_moved))
        failures.append("E1: 球心未随面板搬走而变暗")
    else:
        print("     ✓ 球心变暗 %.1f → %.1f：反射内容确实来自本次捕获"
              % (lum_on, lum_moved))

    # ---------------------------------------------------------- F 组
    print("\n[F] 编辑器面板（MYVK_EDITOR_SELECT）")
    # ⚠ 探针分区默认是**折叠**的（sectionHeader(..., defaultOpen=false)，
    #   与 Inspector 里其它"组件分区"一致），而 `if (!open) return;` 之后的
    #   参数行、Capture 按钮都要展开才画 —— 展开要点 UI。所以这里只断言
    #   "分区存在且归属正确"，参数行的覆盖留给 verify_inspector 那一类
    #   带点击的脚本（与 Decal / Mesh 分区的处理一致）。
    sel_lines = all_lines["sel"]
    mir_lines = all_lines["selmirror"]
    if not has(sel_lines, "INS-PROBE-HEADER"):
        print("     ✗ 选中探针实体后没有 INS-PROBE-HEADER —— "
              "Inspector 没画出探针这一节")
        failures.append("F1: Inspector 缺探针节")
    else:
        print("  F1 选中探针实体 → INS-PROBE-HEADER 出现 ✓")

    if has(mir_lines, "INS-PROBE-HEADER"):
        print("     ✗ 选中没有探针组件的实体时也画了探针节 —— "
              "面板没有按组件存在性判断")
        failures.append("F2: 探针节与组件存在性无关")
    else:
        print("  F2 选中无探针组件的实体（Mirror）→ 探针节不出现 ✓")

    # ---------------------------------------------------------- 可视化
    # 数值断言能证明"对"，但看不出"长什么样"。这张六联图是把
    # ① 探针开/关的画面差、② 强度、③ 盒体大小、④ 重捕后的内容
    # 一眼并排 —— 全在**视口矩形**内裁切（不含 ImGui 面板），
    # 每格左上角用纯黑底白字打标签（比浮层文字耐缩放）。
    try:
        from PIL import Image, ImageChops, ImageDraw
        import numpy as np

        x0, y0, x1, y1 = [int(round(v)) for v in vpv]

        def crop(im):
            return im.crop((x0, y0, x1, y1)).convert("RGB")

        def delta_image(a, b, gain=4):
            """|a-b| × gain：差异放大后再看，否则弱差异在缩略图里看不清。"""
            d = ImageChops.difference(a, b)
            arr = np.clip(np.asarray(d, dtype=np.int16) * gain, 0, 255)
            return Image.fromarray(arr.astype(np.uint8), "RGB")

        def label(im, text):
            d = ImageDraw.Draw(im)
            tw, th = d.textlength(text), 14
            d.rectangle((0, 0, tw + 10, th + 8), fill=(0, 0, 0))
            d.text((5, 4), text, fill=(255, 255, 255))
            return im

        # ⚠ 标签别写 `MYVK_PROBES=0`：`img_base` 是"场景里**没有**探针实体"
        #   那一份（A1 已证明它与"有实体但 PROBES=0"逐像素相同）。
        base_c = crop(img_base)
        on_c = crop(img_on)
        int2_c = crop(img_int2)
        big_c = crop(img_bigbox)
        small_c = crop(img_smallbox)
        moved_c = crop(img_moved)
        d_on = delta_image(base_c, on_c)
        d_int2 = delta_image(base_c, int2_c)
        d_big = delta_image(base_c, big_c)
        d_small = delta_image(base_c, small_c)
        d_moved = delta_image(base_c, moved_c)
        # `npx` = 偏心探针 + boxProjection **关**，`bpx` = 同一个偏心探针 + **开**
        # → 这一格单独回答"盒投影到底修了什么"（D1 的读数就是这两个的平均差）
        d_npx = delta_image(crop(img_npx), crop(img_bpx))

        # 六格：左边一列是"原图"，中间一列是"差异×4"，右边一列是"对照"
        panels = [
            ("baseline  (no probe entity)", base_c),
            ("probe ON", on_c),
            ("delta  probe ON - baseline  x4", d_on),
            ("delta  INTENSITY=2  x4", d_int2),
            ("delta  big box (40m)  x4", d_big),
            ("delta  small box (2m)  x4", d_small),
            ("delta  yellow panel moved  x4", d_moved),
            ("delta  box projection  off vs on  x4", d_npx),
        ]
        cols = 4
        rows = (len(panels) + cols - 1) // cols
        cw, ch = base_c.size
        gap = 6
        canvas = Image.new("RGB", (cols * cw + (cols + 1) * gap,
                                   rows * ch + (rows + 1) * gap), (24, 24, 28))
        for i, (cap, im) in enumerate(panels):
            r, c = divmod(i, cols)
            canvas.paste(label(im, cap), (gap + c * (cw + gap),
                                          gap + r * (ch + gap)))
        cmp_path = os.path.join(OUT_DIR, "probe_compare.png")
        canvas.save(cmp_path)
        print("\n[可视化] 对比图: %s" % cmp_path)
    except Exception as ex:  # noqa: BLE001
        # 可视化是**附加**产物，失败不该改变验收结论（与 verify_decals 一致）
        print("\n[可视化] 跳过（%s: %s）" % (type(ex).__name__, ex))

    # ---------------------------------------------------------- 汇总
    if not args.keep_scenes:
        for p in list(paths.values()):
            try:
                os.remove(p)
            except OSError:
                pass

    print("\n" + "=" * 68)
    # ⚠ 结果行必须用 `结果: PASS/FAIL` —— run_regression.py 的 OK_RE 只认
    #   「RESULT: PASS / 全部断言 PASS / 全部 PASS / 结果: PASS」四种写法，
    #   且取**最后一条**匹配。写成自由格式（比如 `PASS —— ...`）时进程退出码
    #   虽然还是 0，runner 的 ok 却是 False → 判 FAIL。这个坑吃过一次，留注释。
    if failures:
        print("结果: FAIL（%d 项）" % len(failures))
        for f in failures:
            print("  · %s" % f)
        return 1
    print("结果: PASS（R5-1 反射探针整链：捕获 → 拷贝 → 预过滤 → 盒投影 → "
          "着色，A 组 4 项 + B 组 7 项 + C 组 4 项 + D 组 3 项 + E 组 1 项 + "
          "F 组 2 项，全部通过）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
