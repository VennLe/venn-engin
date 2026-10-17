"""端到端验证：Inspector 的左侧图标导航条 + 按页签分区（2026-10-16）。

改造前的 Inspector 是"一条滚动条堆到底"：选个物体之后要滚过 Transform /
Visibility / Mesh / Material / Collision / Decal / Reflection Probe /
Particle Emitter / Light / Script 才找得到要调的那一项。现在变成
**左列 11 个图标 + 右侧一页内容**（Blender 属性编辑器的组织方式）。

页签 → 内容映射（与 InspectorPanel.cpp 的 kTabs[] 逐项对应）：

    object      Transform / Visibility / Mesh
    tool        Script / Add-Component
    render      全局渲染细节（FeatureRegistry 挑出的渲染分组）/ 物体级渲染属性 /
                Light / Decal / Reflection Probe
    output      视口与输出的只读信息（引擎暂无输出设置）
    scene       场景统计
    world       相机 / 主光
    particles   GPU 粒子发射器（含"在原点新建"与全局预览开关）
    physics     物理世界参数 + 运行态统计 + 刚体 + 碰撞体
    material    材质
    animation   Skinned Mesh（无蒙皮时画"空页签"提示）
    all         旧的堆叠视图（**自动化运行下的默认页签**）

> ⚠ 为什么 `all` 存在、而且是自动化的默认值：项目里已有 9 个验证脚本靠
>   `INS-MESH-HEADER` / `INS-COLLISION-HEADER` / `INS-PROBE-HEADER` /
>   `INS-FX-EXPAND-ALL` 这些矩形锚点断言"那个分区画出来了"。分区一旦按页签
>   分散，不指定页签就只看得到 Object 页 → 那 9 个脚本会整批误报。默认
>   "全部"一次性保住了它们；想单测某一页再用 MYVK_EDITOR_INSP_TAB 指定。

断言分五组：

  A. **导航条结构**：11 个 `INS-NAV <id>` 都在、排在同一列、竖直不重叠、
     顺序与 kTabs[] 一致、是方形且尺寸合理；`INSP-TAB` 日志确认选中页签。
  B. **页签隔离**（核心）：逐个页签跑，断言"该页该有的锚点在 / 不该有的
     不在"。这是"页面真的换了内容"的直接证据，比去截图里认控件可靠。
  C. **无选中物体**：World / Scene / Output 仍然可用（以前是整页退出）。
  D. **搜索跨页签**：搜索词非空时忽略页签选择，跨全部页签摊平命中项。
  E. **像素**：object 与 material 两页的内容区**显著不同**（真的换了内容，
     不是只有日志变了）；同一页签两次独立运行逐像素一致。
     全程零 VUID / 零 Fatal。

用法：
    python tools/verify_inspector_tabs.py
"""

import json
import os
import re
import subprocess
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_client, find_window, user32      # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors             # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = os.path.join(ROOT, "..", "_venn_verify")

# 相机拉远俯视，让立方体完整可见（页签断言不看画面，但像素组要用）
CAM = "0,-11,4.5,90,11"

# kTabs[] 的顺序 —— 导航条**必须**按这个顺序从上往下排。
TAB_ORDER = ["object", "tool", "render", "output", "scene", "world",
             "particles", "physics", "material", "animation", "all"]

# 每个页签：必须出现的锚点 / 必须不出现的锚点。
# `need` 与 `deny` 同时成立，才说明"这一页只画了它自己那一份内容"。
#
# ⚠ 用 `INS-FX-HEADER` 而不是 `INS-FX-EXPAND-ALL`：后者在"Expand all"
#   按钮上，而粒子段默认是**折叠**的（defaultOpen=false）→ 不注入展开就
#   根本画不到。段标题那一条则与折叠无关，是稳定的正面证据。
#
# ⚠ deny 组之所以成立，是因为这些锚点对应的是**分区函数是否被调用**：
#   页签派发不调它，锚点就必然不在。也就是说它测的是"页签过滤"，
#   不是"搜索过滤"—— 搜索过滤要另找过滤敏感的锚点，见 D 组。
TAB_CASES = {
    "object": {
        "need": ["INS-MESH-HEADER"],
        "deny": ["INS-COLLISION-HEADER", "INS-PROBE-HEADER",
                 "INS-MATERIAL-HEADER", "INS-ADD-COMPONENT",
                 "INS-CAMERA-HEADER", "INS-OUTPUT-HEADER", "INS-FX-HEADER"],
    },
    "tool": {
        "need": ["INS-ADD-COMPONENT"],
        "deny": ["INS-MESH-HEADER", "INS-COLLISION-HEADER",
                 "INS-PROBE-HEADER", "INS-MATERIAL-HEADER",
                 "INS-CAMERA-HEADER", "INS-OUTPUT-HEADER", "INS-FX-HEADER"],
    },
    "render": {
        "need": ["INS-RENDER-HEADER", "INS-OBJ-RENDER-HEADER",
                 "INS-PROBE-HEADER"],
        "deny": ["INS-MESH-HEADER", "INS-COLLISION-HEADER",
                 "INS-MATERIAL-HEADER", "INS-ADD-COMPONENT",
                 "INS-CAMERA-HEADER", "INS-OUTPUT-HEADER", "INS-FX-HEADER"],
    },
    "output": {
        "need": ["INS-OUTPUT-HEADER"],
        "deny": ["INS-MESH-HEADER", "INS-COLLISION-HEADER",
                 "INS-PROBE-HEADER", "INS-MATERIAL-HEADER",
                 "INS-CAMERA-HEADER", "INS-FX-HEADER"],
    },
    "scene": {
        "need": ["INS-SCENE-STATS"],
        "deny": ["INS-MESH-HEADER", "INS-CAMERA-HEADER", "INS-SUN-HEADER",
                 "INS-COLLISION-HEADER", "INS-OUTPUT-HEADER"],
    },
    "world": {
        "need": ["INS-CAMERA-HEADER", "INS-SUN-HEADER"],
        "deny": ["INS-MESH-HEADER", "INS-SCENE-STATS", "INS-COLLISION-HEADER",
                 "INS-OUTPUT-HEADER", "INS-MATERIAL-HEADER"],
    },
    "particles": {
        "need": ["INS-FX-HEADER"],
        "deny": ["INS-MESH-HEADER", "INS-COLLISION-HEADER",
                 "INS-PROBE-HEADER", "INS-MATERIAL-HEADER",
                 "INS-ADD-COMPONENT"],
    },
    "physics": {
        # 世界参数那一段**不依赖选中物体**，所以它是最稳的正面证据；
        # 刚体段 + 碰撞体段则要选中一个实体才画。
        "need": ["INS-PHYS-WORLD-HEADER", "INS-PHYS-BODY-HEADER",
                 "INS-COLLISION-HEADER"],
        "deny": ["INS-MESH-HEADER", "INS-PROBE-HEADER",
                 "INS-MATERIAL-HEADER", "INS-ADD-COMPONENT",
                 "INS-FX-HEADER",
                 "INS-CAMERA-HEADER", "INS-SUN-HEADER"],
    },
    "material": {
        "need": ["INS-MATERIAL-HEADER"],
        "deny": ["INS-MESH-HEADER", "INS-COLLISION-HEADER",
                 "INS-PROBE-HEADER", "INS-ADD-COMPONENT", "INS-FX-HEADER"],
    },
    # 场景里没有蒙皮模型 → 这一页必然画"空页签"提示；那行日志就是它的锚点。
    "animation": {
        "need": [],
        "deny": ["INS-MESH-HEADER", "INS-COLLISION-HEADER",
                 "INS-MATERIAL-HEADER", "INS-PROBE-HEADER", "INS-FX-HEADER"],
        "empty": "INS-TAB-EMPTY tab=animation need=skinned mesh",
    },
    # All = 旧的堆叠视图：上面所有分区应当同时在场（且不该冒出头部的三段）。
    "all": {
        "need": ["INS-MESH-HEADER", "INS-COLLISION-HEADER",
                 "INS-PROBE-HEADER", "INS-MATERIAL-HEADER",
                 "INS-ADD-COMPONENT", "INS-FX-HEADER"],
        "deny": ["INS-CAMERA-HEADER", "INS-SUN-HEADER", "INS-SCENE-STATS",
                 "INS-OUTPUT-HEADER", "INS-ANIM-HEADER"],
    },
}

# 每个"只在某一个页签里画的段标题"归属哪个页签。
#
# 为什么用一张表 + 自动展开 deny，而不是在每个 deny 列表里手写：新增页签
# 或新增分段时最容易漏的就是"忘了把它加进别的页签的 deny" —— 那会让隔离
# 断言悄悄变成空断言（别的页签里它本来就不该有，可就是没人查）。表一写，
# 漏项不可能发生。
SECTION_OWNER = {
    "INS-MESH-HEADER": "object",
    "INS-COLLISION-HEADER": "all",     # 出现在 All / physics / object 三个页
    "INS-PROBE-HEADER": "render",
    "INS-MATERIAL-HEADER": "material",
    "INS-ADD-COMPONENT": "tool",
    "INS-CAMERA-HEADER": "world",
    "INS-SUN-HEADER": "world",
    "INS-SCENE-STATS": "scene",
    "INS-OUTPUT-HEADER": "output",
    "INS-ANIM-HEADER": "animation",
    "INS-FX-HEADER": "particles",
    # R7-1 物理 / 2026-10-16 Render 页签改造新增的四段
    "INS-RENDER-HEADER": "render",
    "INS-OBJ-RENDER-HEADER": "render",
    "INS-PHYS-WORLD-HEADER": "physics",
    "INS-PHYS-BODY-HEADER": "physics",
}

# 例外：这些"段标题"会出现在**多个**页签里（上面的 SECTION_OWNER 只记了
# 一个"主窝"）。做 deny 展开时必须跳过它们，否则会在它真正该出现的另一个
# 页签里被误判成"不该出现"。
SHARED_ANCHORS = {"INS-COLLISION-HEADER"}   # All / physics 两页都画

for _tab, _case in TAB_CASES.items():
    for _anchor, _owner in SECTION_OWNER.items():
        if _anchor in SHARED_ANCHORS:
            continue
        if _tab == _owner or _anchor in _case["need"]:
            continue
        if _anchor not in _case["deny"]:
            _case["deny"].append(_anchor)

# 所有 INS-* 锚点（判断"某个锚点在不"用的全集）
ALL_ANCHORS = list(SECTION_OWNER.keys()) + ["INS-FX-EXPAND-ALL"]


# ---------------------------------------------------------------- 场景

def material(name):
    return {
        "kind": "procedural", "name": name,
        "baseColor": [0.72, 0.36, 0.26, 1.0], "metallic": 0.0,
        "roughness": 0.45, "normalScale": 1.0, "ao": 1.0,
        "emissive": [0.0, 0.0, 0.0], "doubleSided": False,
        "alphaMode": "opaque", "alphaBlend": False,
        "alphaCutoff": 0.5, "ditherAlpha": False, "albedo": None,
    }


def build_scene(path):
    """一个"厨房水槽"实体：同时挂 mesh / material / collision / probe /
    particleEmitter。

    为什么要一次挂满：页签隔离断言靠的是**同一个实体**在不同页签下
    "露出的分区不同"。如果每页用不同的实体，就分不清"是页签把分区过滤掉
    了"还是"这个实体本来就没有那个组件"——那样 deny 组会退化成空断言。
    """
    ent = {
        "name": "KitchenSink",
        "transform": {"position": [0.0, 0.0, 0.0], "rotation": [0, 0, 0],
                      "scale": [1, 1, 1]},
        "visible": True, "castShadow": True, "parent": -1,
        "mesh": {"kind": "builtin", "shape": "cube", "size": 2.4,
                 "name": "vt_cube"},
        "material": material("vt_mat"),
        "collision": {},
        "probe": {},
        "particleEmitter": {},
    }
    doc = {
        "version": 1,
        "generator": "verify_inspector_tabs.py",
        "light": {"direction": [-0.35, -0.55, -0.76],
                  "color": [1.0, 0.98, 0.95], "intensity": 1.0,
                  "castsShadow": True, "ambientScale": 0.40},
        "entities": [ent],
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)
    return 1


# ---------------------------------------------------------------- 运行

def run_editor(exe, scene, tab=None, select="KitchenSink", extra=None):
    """跑一轮编辑器。

    返回 dict：
      anchors  —— 本帧出现过的 INS-* 锚点集合
      nav      —— 页签名 → 矩形 (x0,y0,x1,y1)
      tab_line —— INSP-TAB selected= 报出来的页签（"" = 没等到）
      img      —— 整窗截图 ndarray（或 None）
      lines    —— 原始日志行
      err      —— 含 VUID / VkError / Fatal 的行
    """
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_EDITOR_NO_HUD"] = "1"
    env["MYVK_GRID"] = "0"
    env["MYVK_LIGHT_GIZMOS"] = "0"
    env["MYVK_EDITOR_SCENE"] = scene
    env["MYVK_EDITOR_CAM"] = CAM
    if select:
        env["MYVK_EDITOR_SELECT"] = select
    else:
        env.pop("MYVK_EDITOR_SELECT", None)
    if tab is not None:
        env["MYVK_EDITOR_INSP_TAB"] = tab
    else:
        env.pop("MYVK_EDITOR_INSP_TAB", None)
    if extra:
        env.update(extra)

    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)

    res = {"anchors": set(), "nav": {}, "tab_line": "", "img": None,
           "lines": [], "err": []}
    try:
        hwnd = find_window("Venn Editor")
        if not hwnd:
            res["err"].append("未找到编辑器窗口")
            return res
        user32.ShowWindow(hwnd, 9)
        time.sleep(1.2)
        user32.SetWindowPos(hwnd, 0, 0, 0, 1280, 800, 0x0002 | 0x0004)

        # 等"当前页签"这条日志 —— 它出现即说明这一帧的页签已经定下来了。
        deadline = time.time() + 20.0
        while time.time() < deadline:
            for ln in list(tap.lines):
                m = re.search(r"INSP-TAB selected=(\w+)", ln)
                if m:
                    res["tab_line"] = m.group(1)
            if res["tab_line"]:
                break
            time.sleep(0.25)
        # 再多等一会儿，让各分区的 logRect（去抖后只在首帧打）落地
        time.sleep(1.6)

        for name in TAB_ORDER:
            r = tap.last_rect("INS-NAV " + name)
            if r:
                res["nav"][name] = r
        for a in ALL_ANCHORS:
            if tap.last_rect(a):
                res["anchors"].add(a)

        img, _ok = capture_client(hwnd)
        if img is not None:
            res["img"] = np.array(img)

        with tap.lock:
            res["lines"] = list(tap.lines)
        for ln in res["lines"]:
            u = ln.upper()
            if "VUID-" in u or "VKERROR" in u or "FATAL" in u:
                res["err"].append(ln[:160])
    finally:
        try:
            proc.terminate()
            proc.wait(timeout=6)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass
    return res


def has_line(res, needle):
    return any(needle in ln for ln in res["lines"])


def content_crop(img, nav):
    """从整窗截图里裁出 Inspector 的**内容区**（导航条右边那一块）。

    用导航条自己的矩形反推，不写死坐标 —— 窗口尺寸 / DPI / 字体缩放变了
    也照样裁得对（写死坐标在 DPI 缩放下偏十几像素，而且是静默偏）。
    y 只取导航条覆盖的那一段，避开底部状态栏（那里的历史计数会变）。
    """
    if img is None or "object" not in nav or "all" not in nav:
        return None
    x0 = int(max(v[2] for v in nav.values())) + 4
    y0 = int(min(v[1] for v in nav.values()))
    y1 = int(max(v[3] for v in nav.values()))
    x1 = min(img.shape[1], x0 + 240)
    return img[y0:y1, x0:x1]


# ---------------------------------------------------------------- 主流程

def main():
    exe = DEFAULT_EXE
    if not os.path.exists(exe):
        print("找不到 %s" % exe)
        return 2

    kill_stale_editors()
    os.makedirs(OUT_DIR, exist_ok=True)
    scene = os.path.join(OUT_DIR, "_insp_tabs_scene.json")
    build_scene(scene)

    failures = []

    def check(name, ok, detail=""):
        print("  %s %s%s" % ("✓" if ok else "✗", name,
                             ("   " + str(detail)) if detail else ""))
        if not ok:
            failures.append(name)
        return ok

    # ---------------------------------------------------------- A 导航条结构
    print("\n[A] 导航条结构（tab=all）")
    base = run_editor(exe, scene, tab="all")
    check("零 VUID / 零 Fatal", not base["err"], base["err"][:2])
    check("MYVK_EDITOR_INSP_TAB 生效（选中 all）",
          base["tab_line"] == "all", "tab_line=%r" % base["tab_line"])
    check("11 个导航项矩形都在", len(base["nav"]) == len(TAB_ORDER),
          "拿到 %d 个" % len(base["nav"]))

    if len(base["nav"]) == len(TAB_ORDER):
        xs0 = [base["nav"][k][0] for k in TAB_ORDER]
        xs1 = [base["nav"][k][2] for k in TAB_ORDER]
        check("全部落在同一列（左缘极差 <= 2px）",
              max(xs0) - min(xs0) <= 2.0, "极差 %.1f" % (max(xs0) - min(xs0)))
        check("全部宽度一致（右缘极差 <= 2px）",
              max(xs1) - min(xs1) <= 2.0, "极差 %.1f" % (max(xs1) - min(xs1)))

        sizes = [(base["nav"][k][2] - base["nav"][k][0],
                  base["nav"][k][3] - base["nav"][k][1]) for k in TAB_ORDER]
        square = all(abs(w - h) <= 1.0 for w, h in sizes)
        sane = all(16.0 <= w <= 44.0 for w, _ in sizes)
        check("是正方形且尺寸合理（16..44px）", square and sane,
              "%.0fx%.0f" % sizes[0])

        by_y = sorted(TAB_ORDER, key=lambda k: base["nav"][k][1])
        check("竖直顺序与 kTabs[] 一致", by_y == TAB_ORDER,
              "实际 " + ",".join(by_y))

        spans = sorted((base["nav"][k][1], base["nav"][k][3], k)
                       for k in TAB_ORDER)
        overlap = [spans[i + 1][2] for i in range(len(spans) - 1)
                   if spans[i + 1][0] < spans[i][1] - 0.5]
        check("相邻项在竖直方向不重叠", not overlap,
              ("重叠: " + ",".join(overlap)) if overlap else "")

        # All 与上面十项之间刻意留了一道空隙（它语义不同：不是"页"）
        gap = base["nav"]["all"][1] - base["nav"]["animation"][3]
        check("All 与 animation 之间有分隔空隙", gap > 12.0, "间隙 %.0fpx" % gap)

    # ---------------------------------------------------------- B 页签隔离
    print("\n[B] 页签隔离（逐页跑，正/负对照）")
    caps = {"all": base}
    for tab in TAB_ORDER:
        if tab == "all":
            continue
        r = run_editor(exe, scene, tab=tab)
        caps[tab] = r
        case = TAB_CASES[tab]
        miss = [a for a in case["need"] if a not in r["anchors"]]
        extra = [a for a in case["deny"] if a in r["anchors"]]
        detail = []
        if r["err"]:
            detail.append("VUID: " + r["err"][0][:60])
        if r["tab_line"] != tab:
            detail.append("tab_line=%r" % r["tab_line"])
        if miss:
            detail.append("缺 " + ",".join(miss))
        if extra:
            detail.append("多了 " + ",".join(extra))
        if "empty" in case and not has_line(r, case["empty"]):
            detail.append("未见 '%s'" % case["empty"])
        check("%-10s 只画自己那一份" % tab,
              not detail, "  ".join(detail))

    # 粒子页的"内容真的长出来"：注入展开后模块控件必须出现
    fx = run_editor(exe, scene, tab="particles",
                    extra={"MYVK_EDITOR_INSP_EXPAND": "Particle Emitter"})
    check("particles 页注入展开后出现模块控件",
          "INS-FX-EXPAND-ALL" in fx["anchors"], sorted(fx["anchors"]))

    # ---------------------------------------------------------- C 无选中物体
    print("\n[C] 无选中物体时的行为")
    noW = run_editor(exe, scene, tab="world", select=None)
    check("world 页仍画相机/主光（以前是整页退出）",
          "INS-CAMERA-HEADER" in noW["anchors"] and
          "INS-SUN-HEADER" in noW["anchors"], sorted(noW["anchors"]))
    noO = run_editor(exe, scene, tab="object", select=None)
    check("object 页无选中时不画网格段（走空页签）",
          "INS-MESH-HEADER" not in noO["anchors"] and
          has_line(noO, "INS-TAB-EMPTY tab=object"), sorted(noO["anchors"]))

    # ---------------------------------------------------------- D 搜索跨页签
    #
    # ⚠ 这里**只能**用 INS-MATERIAL-HEADER / INS-OUTPUT-HEADER 这类"记在
    #   提前返回之后"的锚点来判断"分区可不可见"。
    #   INS-MESH/COLLISION/PROBE-HEADER 记在 `sectionHeader()` 之后、
    #   `if (!open) return;` **之前** —— 它们要供脚本点击展开，所以必须
    #   "折叠着也在"。代价是：哪怕被搜索词过滤掉，那几行日志照样会打出来
    #   （函数被调用了≠分区可见）。第一版脚本正是拿 MESH-HEADER 去断言
    #   "搜 rough 时 Mesh 被过滤掉"，于是假红 —— 见 2026-10-16 的日志。
    print("\n[D] 搜索跨页签")
    sr = run_editor(exe, scene, tab="object",
                    extra={"MYVK_EDITOR_INSP_SEARCH": "rough"})
    check("停在 object 页搜 'rough' → 跨页签带出 Material 段",
          "INS-MATERIAL-HEADER" in sr["anchors"], sorted(sr["anchors"]))

    sm = run_editor(exe, scene, tab="material",
                    extra={"MYVK_EDITOR_INSP_SEARCH": "zzz_no_such_thing"})
    check("搜一个不存在的词 → Material 段被过滤掉（锚点消失）",
          "INS-MATERIAL-HEADER" not in sm["anchors"], sorted(sm["anchors"]))
    check("上一条的对照：同一页不搜索时 Material 段在",
          "INS-MATERIAL-HEADER" in caps["material"]["anchors"])

    # ---------------------------------------------------------- E 像素
    print("\n[E] 像素：换页真的换内容 + 确定性")
    c_obj = content_crop(caps["object"]["img"], caps["object"]["nav"])
    c_mat = content_crop(caps["material"]["img"], caps["material"]["nav"])
    if c_obj is None or c_mat is None:
        check("内容区截图裁剪成功", False, "拿不到导航矩形或截图")
    elif c_obj.shape != c_mat.shape:
        check("两页内容区尺寸一致", False, "%s vs %s" % (c_obj.shape,
                                                         c_mat.shape))
    else:
        check("两页内容区尺寸一致", True, str(c_obj.shape))
        d = np.abs(c_obj.astype(np.int16) - c_mat.astype(np.int16)).max(axis=2)
        n = int((d > 18).sum())
        # object 页是"变换+可见性+网格"，material 页是"材质因子+贴图槽"，
        # 两张图必须显著不同 —— 这条钉死"换页真的换了画面"，而不只是
        # 日志里多了/少了一行。
        check("object 与 material 的内容区显著不同", n > 400,
              "差异像素 %d / %d" % (n, d.size))

    rerun = run_editor(exe, scene, tab="object")
    c_re = content_crop(rerun["img"], rerun["nav"])
    if c_obj is None or c_re is None or c_obj.shape != c_re.shape:
        check("两次独立运行 object 页逐像素一致", False, "尺寸/截图不可比")
    else:
        n = int((np.abs(c_obj.astype(np.int16) - c_re.astype(np.int16))
                 .max(axis=2) > 2).sum())
        check("两次独立运行 object 页逐像素一致", n == 0, "差异像素 %d" % n)

    # ---------------------------------------------------------- 结论
    print("\n" + "=" * 68)
    if failures:
        print("结果: FAIL: %d 项未通过 -> %s" % (len(failures), failures))
        return 1
    print("结果: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
