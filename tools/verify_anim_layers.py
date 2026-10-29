"""端到端验证：动画层 / 骨骼遮罩（B1）。

背景：在这之前引擎只有**一份**姿势 —— 状态机切 clip 是整具身体一起换，
所以"上半身挥手 + 下半身走路"这一类玩法动画根本做不了。B1 给
`AnimatorComponent` 加了 `layers`：第 0 层是底座（今天的行为），第 1..N 层
各自播一段 clip，按骨骼遮罩盖到骨架上。

数值层面（权重、遮罩、端点逐位）由 `MYVK_ANIM_TEST=1` 的
`ANIM-TEST layer_* / mask_*` 26 条断言负责。**本脚本只验屏幕上的事**：
"遮罩真的只动了它该动的那半边身体"。

================================================================
两个取景上的坑（不写下来下次一定重踩）
================================================================

坑 1：**手写的 `skinned` 实体在视口里什么都不画。**
  `skinned` 块只建 `SkinnedMeshComponent`（它只提供 `jointBase`），真正被
  渲染的是**每个 submesh 一个子实体**（MeshComponent + MaterialComponent）。
  手写一个 `{"skinned": {...}, "mesh": null}` 的实体，日志里
  `SCENE-SKINNED ok=1 joints=21`、`ANIM-SKIN ok=1` 一切正常，**但屏幕上
  一个像素都没有** —— 于是像素判据全 0%，看起来像"遮罩没生效"（第一版
  就是这样白查了三轮）。`verify_model_scale.py` 的文件头也记着同一件事。
  唯一会同时建"组件 + 子网格实体"的地方是 `scene::instantiateModel`，
  所以本脚本**先烤一份模板**：`MYVK_EDITOR_IMPORT` + `scene.save`，
  之后所有变体都在这份模板上改 `animator.layers`（见 `bake_template`）。

坑 2（历史，已不成立，但结论保留）：底座和层都用 `Walk`。
  原来那条粗糙的 `anime_girl.fbx` 的 `Idle` 段数据是坏的 —— 骨骼缩放
  关键帧被 `Copy Transforms` 带进了 0.01 的缩放，播出来腿被压成尖刺
  （`docs/animate_girl_import.md` §5 有记录）。那时不能拿 Idle 当"正常
  姿势"的基准，否则"上半身没动"这条判据比对的是两坨尖刺。
  **2026-10-29 换成 FAB 角色之后 Idle 是干净的**，但结论仍然保留：
  底座 `Walk` @0.30 s、层 `Walk` @0 s —— "遮罩只换该换的骨"这件事跟
  "是不是两段不同的动画"无关，同段不同相位反而更干净：除了相位没有
  任何别的变量。唯一用 `Idle` 当上层的是 ④，它是"另一段动画也在正确
  的子树里生效"的对照，而 `Allow=spine2` 正好把腿挡在外面。
  ⚠ 骨名跟着新骨架换过：`thigh` → `upleg`、`chest` → `spine2`
  （`buildBoneMask` 是**去大小写子串**匹配，写 `Spine` 会同时命中
  Spine/Spine1/Spine2，所以必须写带数字的那一节）。

================================================================
判据（九组）
================================================================

  ① **同一场景跑两次**：显著变化 < 0.15%（噪声地板，逐像素抖动来自
     轮廓抗锯齿，**不能写 0**）。
  ② **层（Allow=upleg）把腿换成另一个相位**：下半带显著变化 > 0.5%。
  ③ **同一帧里上半带一个像素都不动**：< 噪声地板。②③ 是主判据 ——
     "只换该换的骨"在屏幕上就长这样；遮罩要是被忽略（整具身体都被覆盖），
     ③ 会跟着变成 5% 左右。
     ⚠ 上下是**两条线**（`UPPER_END` / `LOWER_START`），不是画面正中 ——
     手臂垂在大腿旁边，中线会把两者混在一起（见常量处的注释）。
  ④ **反方向（Allow=spine2，层 = Idle）**：上半带（头/发/手臂）动、
     下半带**一个像素不动**。
  ⑤ **Mask=All + 权重 1 = 完全替换**：与"直接播 Idle"的基线**逐像素相同**
     （实测 0 px）。这条钉住 `slerp(q, layer, 1)` 的端点 —— 端若不是逐位，
     长跑必歪。
  ⑥ **关掉必须回到基线**：权重 0 / clip 空 / 根骨名命中不到 → 都 ≈ 基线；
     权重 0.5 **既不是 0 也不是 1**。
     ⚠ "根骨名命中不到"是**最容易写反**的一条：匹配 0 根应该等于"这一层
     不生效"，而不是"整具身体都被覆盖"。
     ⚠ 权重只看"0.5 和 1 不是同一张图"，**不看"变化像素数单调"** ——
     那个量对位移会饱和（见 ⑥ 代码处的注释）。
  ⑦ **Deny 是镜像**：Deny=spine2 时下半带被换掉（与 ② 同量级）。
     写成同一个分支时这一条会红。
  ⑧ **运行时锚点**：`ANIM-LAYER configured=1 applied=1`、`ANIM-MASK mode=Allow`、
     以及未命中时的 `ANIM-MASK-UNRESOLVED`。证明层真的进了 AnimatorSystem。
  ⑨ **UI 面**：Inspector 上真的画出了 Animation Layers 段（`INS-LAYER-ADD`）。

⚠ 场景里 `playing` 全设 false（底座与层都不推进时间）：两次跑之间帧数
  不可能一致，而动画时间一动，"下半身像素完全相同"这条判据就永远不成立
  （腿在自己摆）。冻结之后整个姿势由 `time` 唯一决定。编辑器非 Play 态
  传的就是 `dt = 0`（`AnimatorSystem::tick` 的 `advance = dt > 0`），
  所以这条是"顺着设计走"，不是跟引擎对着干。
⚠ `MYVK_SHADOW=0`：角色投在**地面**上的影子会跟着腿动，而地面正好落在
  下半身裁剪区里 → 判据会被阴影污染。这不是实现错，是取景错。
⚠ TAA 关掉：历史缓冲会把上一帧混进来，"跑两次同量级"就没有意义了。

用法：
    python tools/verify_anim_layers.py [--exe path]
"""

import argparse
import json
import os
import subprocess
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_nonempty, find_window, user32      # noqa: E402
from verify_add_menu import (LogTap, kill_stale_editors,               # noqa: E402
                             wait_rect)
from verify_local_shadow import crop_pair                              # noqa: E402
from verify_shader_variants import OUT_DIR, ROOT                       # noqa: E402

DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")

# glb（**重写过的那份**）而不是 fbx。两条理由都是实测出来的：
#   ① 老 FBX 路线之所以被选中，是因为当时的 glb 是 Blender 原始导出（Y-up），
#      进引擎只有 0.37 m。`animate_girl_rigged.glb` 已经给根节点补了 +90°X，
#      进场景就是 1.626 m 直立人形（§5）。
#   ② `characters/animate_girl.fbx` 与它烤出来的 `.vmesh` 是**旧产物**：
#      7 个子网格各带一份错的节点变换，渲出来头/躯干/鞋是散开的（拼不回
#      一个人）。静态尺寸看不出来（`verify_model_scale` 只量包围盒），
#      但本脚本要比**像素**，画面一散判据就全废。
MODEL = "assets/models/characters/animate_girl_rigged.glb"
# 实体名 = 资产文件名（`instantiateModel` 的规则），别写死。
SEL = os.path.splitext(os.path.basename(MODEL))[0]
TPL = os.path.join(OUT_DIR, "anim_layers_tpl.json")

BASE_CLIP = "Walk"
# 底座的相位。层的 `time` **不落盘**（运行时字段，读档一律重置为 0），
# 所以"两个相位"只能这样凑：底座 0.30 s、层 0 s。
BASE_TIME = 0.30
LAYER_CLIP = "Walk"

# 机位：3/4 前侧、略俯视。**2026-10-29 换新角色后重量过**：
#
#   角色（`animate_girl_rigged.glb`）进场景是 **1.626 m 高、底面在 z=0.40**
#   （glb 根节点补的那 +90°X 把 Y-up 掰正，同时把原点从脚底挪到了 0.40）。
#   旧机位 `2.10,-2.10,0.85,45,6` 是照着旧资产 1.556 m 定的；换成 1.626 m
#   之后仍然能用，但角色只占画面 46%，两条分带全落在身体外面 —— 于是
#   "上半身没动"量到 7.1%，看着像遮罩失灵，其实是**取景漂移**（变化像素
#   全在轮廓抗锯齿上，信号被摊薄）。
#
#   现在这个机位让角色占视口 **y=0.14 ~ 0.94**（`_probe_anim_layers_bands.py`
#   的逐行剖面：上半身变化 0.143 起、腿的变化到 0.936 止），头在顶上、脚在
#   底下、左右都留白，不再有任何一刀切在身体外面。距离用 2.77 m（沿视线
#   2.8 m，fovY=45° → 可见高 2.3 m），再近透视就开始把脚放大到和头一样宽。
CAM = "1.96,-1.96,1.79,45,12"

# 烤模板用的机位（会写进模板的 camera 块，之后的变体都复用它）
TPL_CAM = CAM

ENV = {
    "MYVK_GRID": "0",
    "MYVK_TAA": "0",
    "MYVK_SHADOW": "0",
}

# 显著变化的判据（与其它像素用例同一口径：单通道阈值 8）
PIX_THR = 8
# 噪声地板上限：同一份场景跑两次实测 0.0x%（轮廓抗锯齿的亚像素抖动）。
# **不能写 0** —— 那等于要求两次进程的渲染逐字节一致，历史上从没成立过。
NOISE_MAX = 0.0015
# "变了很多"的下限
CHANGED_MIN = 0.005

# 视口内的两条横线（相对**视口高度**的比例，0 = 顶）。**不是一条中线**。
#
# 为什么不能用"画面正中"当分界：手臂垂在**大腿旁边**，而大腿一被遮罩换掉
# 就动，手一被遮罩换掉也动 —— 任何单条分界线都会把"手臂"和"大腿"混在一起
# （第一版用 0.52，"上半身没动"量出 5.2%，判据红而实现是对的）。
#
# 改成两条线，各取**只有一类骨能影响**的一段。2026-10-29 换资产 + 换机位后
# 由 `_probe_anim_layers_bands.py` 重量（逐行变化占比，分母 = 视口 608x531）：
#   · Allow=upleg  → 变化从 0.343 一直到 0.936（大腿根 → 鞋底）
#   · Allow=spine2 → 变化从 0.143 一直到 0.593（发顶 → 手）
# 两段在 0.343~0.593 重叠 = **手臂和大腿并排的那一段**，所以:
#   · 0.00 .. 0.32   只有头/发/肩/胸 —— spine2 子树能改，腿碰不到
#     （取 0.32 而不是 0.343：留一格余量，剖面按 1/50 分桶，边界会抖）
#   · 0.64 .. 1.00   只有膝下/鞋 —— 腿能改，spine2 子树碰不到
#     （取 0.64 而不是 0.593：手垂到 0.59，留两格余量）
# 实测上半带 leg 侧 0.000%、下半带 spine2 侧 0.000%，两侧都贴着噪声地板。
# ⚠ 这两个数是**针对当前机位与角色**量的。换机位/换模型必须重量 ——
#   否则判据会静默退化成"两边都算它一半"（每个都 5%，红得莫名其妙）。
UPPER_END = 0.32
LOWER_START = 0.64

GENERATOR = "verify_anim_layers"


# ============================================================
# 场景：烤一次模板，之后每个变体只改 animator.layers
# ============================================================

def _close(proc, hwnd):
    """关掉编辑器并**等它真的退出**。

    ⚠ `terminate()` 之后不 `wait()`，下一轮的 `find_window` 有机会摸到还没
      销毁的窗口 —— 于是截图截的是上一个进程的画面（或者一张正在消失的空
      图），而像素判据会把它当成"这一轮的渲染"。
    """
    if hwnd:
        try:
            user32.PostMessageW(hwnd, 0x0010, 0, 0)
        except Exception:
            pass
    time.sleep(0.5)
    if proc.poll() is None:
        proc.terminate()
    try:
        proc.wait(timeout=8)
    except subprocess.TimeoutExpired:
        proc.kill()
        try:
            proc.wait(timeout=4)
        except subprocess.TimeoutExpired:
            pass


def bake_template(exe, model=None, tpl=None, cam=None):
    """用"从 Content 拖进视口"的既有钩子烤一份**带子网格实体**的模板。

    走这条路而不是手写实体的理由见文件头坑 1。`scene.save` 在第 40 帧
    执行一次（`MYVK_EDITOR_CONSOLE_CMD`），那时的布局 / 面板 / 命令表都
    就绪了；`MYVK_EDITOR_CAM` 会写进模板的 camera 块。

    `model` / `tpl` / `cam` 都是**可选覆盖**，默认仍是本文件那几个常量 ——
    也就是说"这条用例验哪个模型、写到哪个路径、从哪个机位看"一个字没变。
    加这两个参数是为了让 `make_anim_demo.py` 能拿**另一份**资产烤它自己的
    演示场景（2026-10-29 起演示角色换成了 `animate_girl_rigged.glb`），
    同时不把本文件的夹具、断言、模板路径一起拖下水。共用一份 TPL 是不行的：
    谁后跑谁把对方那份覆盖掉，"验的是哪个模型"就取决于执行顺序了。
    """
    model = model or MODEL
    tpl = tpl or TPL
    cam = cam or TPL_CAM
    if os.path.isfile(tpl):
        os.remove(tpl)
    os.makedirs(os.path.dirname(tpl), exist_ok=True)
    # ⚠ 清场必须在 find_window 之前：上一轮的 Editor.exe 只要还活着，
    #   find_window 就会摸到**那具尸体**（旧窗口），后面的截图全落在它身上，
    #   而新开的那份收不到任何消息 —— 症状是"截图一直是 None"。
    kill_stale_editors()
    env = _base_env()
    env.update({
        "MYVK_EDITOR_SCENE": tpl,
        "MYVK_EDITOR_IMPORT": model,
        "MYVK_EDITOR_CAM": cam,
        "MYVK_EDITOR_CONSOLE_CMD": "scene.save",
    })
    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env,
                            stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    hwnd = find_window("Venn Editor")
    if not hwnd:
        raise SystemExit("[FAIL] 烤模板：编辑器窗口没出现")
    try:
        # 窗口尺寸无所谓（模板只存几何），但 resize 会让 RT 重建、拖慢
        # `scene.save` 的帧号门槛，所以干脆不动它。
        end = time.time() + 60.0
        while time.time() < end:
            with tap.lock:
                saved = any("Scene saved:" in l for l in tap.lines)
            if saved:
                break
            time.sleep(0.3)
        if not os.path.isfile(tpl):
            raise SystemExit("[FAIL] 烤模板：scene.save 没落盘")
    finally:
        _close(proc, hwnd)
    with open(tpl, encoding="utf-8") as f:
        doc = json.load(f)
    sub = sum(1 for e in doc["entities"] if e.get("parent") is not None
              and e.get("parent") >= 0 and e.get("mesh"))
    if sub < 1:
        raise SystemExit("[FAIL] 烤模板：没有子网格实体（角色不会出现在屏幕上）")
    _write_stamp(tpl, model, cam)
    return tpl


def _stamp_path(tpl):
    """模板的**来源戳**（旁边的小 sidecar，不写进模板本体）。

    为什么不写进模板 JSON：那是引擎 `scene.save` 的产物，往里塞自定义的
    顶层键意味着"模板文件"和"引擎 dump 的形状"开始分叉，而这一条路上的
    其余十个用例都假定它就是一份正常场景。放旁边一个 `.src.json` 零风险。
    """
    return tpl + ".src.json"


def _write_stamp(tpl, model, cam):
    try:
        with open(_stamp_path(tpl), "w", encoding="utf-8") as f:
            json.dump({"model": model, "cam": cam}, f,
                      ensure_ascii=False, indent=1)
    except OSError:
        pass        # 戳只是优化：写不进去就退化成"每次重烤"


def ensure_template(exe=DEFAULT_EXE, model=None, tpl=None, cam=None):
    """模板在、**而且是用同一份资产烤的** → 直接复用；否则重烤。

    为什么需要这一层（2026-10-29 踩的）：`verify_anim_events` /
    `verify_root_motion` / `verify_sequencer` 都复用 `verify_anim_layers`
    烤好的 `anim_layers_tpl.json`。原来它们只判 `os.path.isfile(TPL)` ——
    资产一换（旧 `anime_girl.fbx` 退役、全仓夹具改指
    `characters/animate_girl*`），模板里每一条 `mesh.path` 就都指着一个
    不存在的文件，而这三条用例的表现是「模板里没有蒙皮实体」或者
    「cfg=1 但 bone=-」这种**看不懂的红**。
    判"文件在不在"是不够的，得判"是不是按这份资产生成的"。
    """
    model = model or MODEL
    tpl = tpl or TPL
    cam = cam or TPL_CAM
    sp = _stamp_path(tpl)
    if os.path.isfile(tpl) and os.path.isfile(sp):
        try:
            with open(sp, encoding="utf-8") as f:
                st = json.load(f)
        except (OSError, ValueError):
            st = {}
        if st.get("model") == model:
            return tpl
        print("  [info] 模板的来源资产变了：%s → %s，重烤"
              % (st.get("model"), model))
    bake_template(exe, model, tpl, cam)
    return tpl


def make_layer(clip, weight=1.0, mode=None, roots=None, playing=False):
    """一层。`playing=False` + `fadeDur=0` 是**为了确定性**（见文件头）。"""
    lay = {
        "name": "L0",
        "clip": clip,
        "weight": weight,
        "speed": 1.0,
        "loop": True,
        "playing": playing,
        "fadeDur": 0.0,
    }
    if mode is not None:
        lay["mask"] = {"mode": mode, "roots": list(roots or [])}
    return lay


def build_variant(tag, layers=None, clip=BASE_CLIP, t=BASE_TIME):
    """在模板上改一处：底座的 clip/time + 是否有 layers。"""
    with open(TPL, encoding="utf-8") as f:
        doc = json.load(f)
    root = None
    for e in doc["entities"]:
        if e.get("skinned"):
            root = e
            break
    if root is None:
        raise SystemExit("[FAIL] 模板里没有蒙皮实体")
    root["skinned"]["clipName"] = clip
    root["skinned"]["time"] = t
    root["skinned"]["playing"] = False
    root["skinned"]["loop"] = True
    ent = {
        "defaultClip": clip,
        "controllerPath": "",
        "time": t,
        "speed": 1.0,
        "playing": False,
        "loop": True,
        "rootMotion": 0,
    }
    if layers:
        ent["layers"] = layers
    root["animator"] = ent
    doc["generator"] = GENERATOR
    path = os.path.join(OUT_DIR, "anim_layers_%s.json" % tag)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, ensure_ascii=False)
    return path


# ============================================================
# 跑一轮 + 截图
#
# 为什么不复用 `verify_shader_variants.run_once`：它等的是
# "SHADER-VARIANTS 出现两次"，而那条日志与**视口渲染目标**无关。实测本
# 场景（第一次加载骨骼模型 + 窗口被 SetWindowPos 改成 1280x800）在拿到
# 那条日志之后**还在重建 RT**，截图上是一句
# "rebuilding viewport render target..." 的灰底 —— 两次跑都截到同一张灰图，
# 于是"逐像素相同"假通过、"画面变了"永远 0 px（本脚本第一版的真实踩坑）。
#
# 所以就绪条件是三条：① VP-RECT 有了；② ANIM-TICK 出现两次（帧计数过了
# 60 帧 → 动画系统真的在跑）；③ 截图的方差足够大（灰底重建态方差 ≈ 0）。
# ============================================================

def _base_env():
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_ENV_DBG"] = "0"
    env["MYVK_LIGHT_GIZMOS"] = "0"
    env["MYVK_EDITOR_NO_HUD"] = "1"
    return env


def run_shot(exe, scene, extra_env, shot_path, cam, wait=40.0, select=None):
    env = _base_env()
    env["MYVK_EDITOR_SCENE"] = scene
    env["MYVK_EDITOR_CAM"] = cam
    if select:
        env["MYVK_EDITOR_SELECT"] = select
    env.update(extra_env)

    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    hwnd = None
    try:
        hwnd = find_window("Venn Editor")
        if not hwnd:
            return [], None, None, ["未找到编辑器窗口"]
        user32.ShowWindow(hwnd, 9)
        time.sleep(1.5)
        user32.SetWindowPos(hwnd, 0, 0, 0, 1280, 800, 0x0002 | 0x0004)
        time.sleep(2.5)

        vp = wait_rect(tap, "VP-RECT", 12.0)
        end = time.time() + wait
        while time.time() < end:
            with tap.lock:
                n = sum(1 for l in tap.lines if "ANIM-TICK " in l)
            if n >= 2:
                break
            time.sleep(0.25)

        img = None
        for _ in range(20):
            fresh = tap.last_rect("VP-RECT")
            if fresh:
                vp = fresh
            img = capture_nonempty(hwnd)
            if img is not None and float(np.asarray(
                    img.convert("RGB"), dtype=np.float32).std()) > 3.0:
                break
            time.sleep(0.4)
        if img is not None and shot_path:
            img.save(shot_path)

        with tap.lock:
            lines = list(tap.lines)
        errs = [l for l in lines
                if ("VUID" in l or "VALIDATION" in l or " ERROR" in l
                    or "Error:" in l)]
        return lines, vp, img, errs
    finally:
        _close(proc, hwnd)


def diff_mask(a, b, vp):
    """显著变化布尔图，**只统计视口矩形内**。

    ⚠ 必须裁到视口。第一版直接比整窗，实测信号被两样东西盖过：
      ① 菜单栏右侧那片状态文字每次启动都不一样（实测 132 px 全在
         y=8..20 / x=1236..1257，压根不在视口里）；
      ② 面板排版在两次进程之间有 1~2 px 抖动。
      于是"腿换了、上半身没动"这种真信号被稀释成 0.03%，判据全红。
    ⚠ `crop_pair` 吃的是 **PIL.Image**（内部先 `.convert("RGB")`）——
      先转 numpy 再传进去会报 `ndarray has no attribute convert`。
    """
    pa, pb, _x0, _y0 = crop_pair(a, b, vp)
    if pa is None:
        raise SystemExit("[FAIL] 两张截图尺寸不兼容（裁不出共同区域）")
    return np.abs(pa - pb).max(axis=2) > PIX_THR


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    args = ap.parse_args()

    results = []

    def check(name, ok, detail):
        results.append((name, bool(ok), detail))
        print("[%s] %s  %s" % ("PASS" if ok else "FAIL", name, detail))

    print("[info] 烤场景模板（MYVK_EDITOR_IMPORT + scene.save）...")
    bake_template(args.exe)

    def shoot(tag, layers=None, clip=BASE_CLIP, t=BASE_TIME, env=None,
              wait=40.0, select=None):
        scene = build_variant(tag, layers, clip, t)
        shot = os.path.join(OUT_DIR, "anim_layers_%s.png" % tag)
        e = dict(ENV)
        if env:
            e.update(env)
        lines, vp, img, errs = run_shot(args.exe, scene, e, shot, cam=CAM,
                                       wait=wait, select=select)
        if img is None:
            raise SystemExit("[FAIL] %s: 没拿到截图 errs=%s" % (tag, errs[:3]))
        return img, vp, lines

    def regions(mask, h, name):
        """上半带（只有头/发/肩）与下半带（只有小腿/脚）的显著变化占比。"""
        yu = int(h * UPPER_END)
        yl = int(h * LOWER_START)
        up = float(mask[:yu].mean())
        lo = float(mask[yl:].mean())
        print("      [%s] upper=%.3f%% lower=%.3f%%"
              % (name, up * 100.0, lo * 100.0))
        return up, lo

    # ---- 1) 基线两次 ----
    a0, vp, _ = shoot("base_a")
    a1, _, _ = shoot("base_b")
    m = diff_mask(a0, a1, vp)
    h = m.shape[0]      # 裁过之后的视口高度（所有 region 都按它算）
    noise = float(m.mean())
    check("① 同一场景跑两次：噪声地板", noise < NOISE_MAX,
          "%d px (%.4f%%)" % (int(m.sum()), noise * 100.0))

    # ---- 2)3) 腿被换成另一个相位 ----
    legs, _, lines_legs = shoot("legs",
                                [make_layer(LAYER_CLIP, 1.0, 1, ["upleg"])])
    m_legs = diff_mask(a0, legs, vp)
    up, lo = regions(m_legs, h, "Allow=upleg")
    check("② 遮罩内（下半身）显著变化", lo > CHANGED_MIN,
          "%.2f%% (> %.2f%%)" % (lo * 100.0, CHANGED_MIN * 100.0))
    check("③ 遮罩外（上半身）几乎不变", up < NOISE_MAX,
          "%.3f%% (< %.3f%%)" % (up * 100.0, NOISE_MAX * 100.0))
    check("③ 变化集中在遮罩内（下半 / 上半 > 20 倍）",
          lo > up * 20.0 + 0.002,
          "lower=%.2f%% upper=%.3f%%" % (lo * 100.0, up * 100.0))

    # ---- 4) 反方向：头/手臂换、腿一个像素不动 ----
    arms, _, _ = shoot("arms", [make_layer("Idle", 1.0, 1, ["spine2"])])
    m_arms = diff_mask(a0, arms, vp)
    up2, lo2 = regions(m_arms, h, "Allow=spine2")
    check("④ Allow=spine2 上半身动了", up2 > CHANGED_MIN,
          "%.2f%% (> %.2f%%)" % (up2 * 100.0, CHANGED_MIN * 100.0))
    check("④ Allow=spine2 下半身一个像素没动", lo2 < NOISE_MAX,
          "%.3f%% (< %.3f%%)" % (lo2 * 100.0, NOISE_MAX * 100.0))

    # ---- 5) Mask=All + 权重 1 = 完全替换 ----
    # 基线换成"直接播 Idle、t=0"（层那个 time 不落盘，读档就是 0）。
    idle0, _, _ = shoot("idle0", None, clip="Idle", t=0.0)
    allm, _, _ = shoot("all", [make_layer("Idle", 1.0)])
    r_all = float(diff_mask(idle0, allm, vp).mean())
    check("⑤ Mask=All + 权重 1 ≈ 直接播那段（端点逐位）", r_all < NOISE_MAX,
          "%.3f%% (< %.3f%%)" % (r_all * 100.0, NOISE_MAX * 100.0))

    # ---- 6) 关掉必须回到基线 ----
    w0, _, _ = shoot("w0", [make_layer(LAYER_CLIP, 0.0, 1, ["upleg"])])
    r_w0 = float(diff_mask(a0, w0, vp).mean())
    check("⑥ 权重 0 ≈ 基线", r_w0 < NOISE_MAX,
          "%.3f%% (< %.3f%%)" % (r_w0 * 100.0, NOISE_MAX * 100.0))

    nc, _, _ = shoot("noclip", [make_layer("", 1.0, 1, ["upleg"])])
    r_nc = float(diff_mask(a0, nc, vp).mean())
    check("⑥ 空 clip ≈ 基线", r_nc < NOISE_MAX,
          "%.3f%% (< %.3f%%)" % (r_nc * 100.0, NOISE_MAX * 100.0))

    # ⚠ 根骨名命中不到 → 该层不生效（**不是**"整具身体都被覆盖"）。
    # ⚠ 这条只能看**下半带**：未命中会打一条 WARN，编辑器顺手推一条通知
    #   toast 到视口上方 —— 那块像素在上半带里（实测 0.26%，是 UI 不是姿势）。
    #   这也正好是"分母要用对"的例子：拿整视口看，实现是对的、判据是红的。
    unres, _, lines_unres = shoot(
        "unres", [make_layer(LAYER_CLIP, 1.0, 1, ["no_such_bone"])])
    _, r_unres = regions(diff_mask(a0, unres, vp), h, "命中不到根骨")
    check("⑥ 根骨名命中不到 ≈ 基线（0 命中 ≠ 全部）", r_unres < NOISE_MAX,
          "%.3f%% (< %.3f%%)" % (r_unres * 100.0, NOISE_MAX * 100.0))

    # 权重 0.5：**不写"像素数单调"**。
    # ⚠ 试过，不成立：0.5 与 1.0 的腿都离开了基线，"变了的像素集合"几乎
    #   一样大（实测 6404 vs 6356）—— 变化像素数是**集合**量，对位移会饱和
    #   （同一条纪律见 MEMORY「代理量会饱和」）。真正能分开 0.5 与 1 的事实是
    #   "这两张图不是同一张"，以及"0.5 也不是基线"，两条都不饱和。
    #   `0.5 恰好在一半`这件事由数值用例 `mask_w05_scale=2.0` 钉住（那是
    #   逐位量，比像素代理稳得多）。
    half, _, _ = shoot("half", [make_layer(LAYER_CLIP, 0.5, 1, ["upleg"])])
    m_half = diff_mask(a0, half, vp)
    m_half_vs_full = diff_mask(half, legs, vp)
    _, lo_half = regions(m_half, h, "权重 0.5 vs 基线")
    _, lo_hf = regions(m_half_vs_full, h, "权重 0.5 vs 权重 1")
    check("⑥ 权重 0.5 既不是 0 也不是 1（下半带两张都不像）",
          lo_half > CHANGED_MIN and lo_hf > CHANGED_MIN,
          "vs base=%.2f%% vs full=%.2f%%" % (lo_half * 100.0, lo_hf * 100.0))

    # ---- 7) Deny 是镜像 ----
    deny, _, _ = shoot("deny", [make_layer(LAYER_CLIP, 1.0, 2, ["spine2"])])
    _, lo3 = regions(diff_mask(a0, deny, vp), h, "Deny=spine2")
    check("⑦ Deny=spine2 下半身被换掉", lo3 > CHANGED_MIN,
          "%.2f%% (> %.2f%%)" % (lo3 * 100.0, CHANGED_MIN * 100.0))

    # ---- 8) 运行时锚点 ----
    txt = "\n".join(lines_legs)
    has_layer = "ANIM-LAYER configured=1 applied=1" in txt
    check("⑧ ANIM-LAYER configured=1 applied=1", has_layer,
          "log=%s" % ("found" if has_layer else "missing"))
    has_mask = "ANIM-MASK mode=Allow" in txt
    check("⑧ ANIM-MASK mode=Allow（遮罩解算真的跑了）", has_mask,
          "log=%s" % ("found" if has_mask else "missing"))
    txt_unres = "\n".join(lines_unres)
    has_unres = "ANIM-MASK-UNRESOLVED" in txt_unres
    check("⑧ 未命中的根骨名打 ANIM-MASK-UNRESOLVED", has_unres,
          "log=%s" % ("found" if has_unres else "missing"))

    # ---- 9) UI 面 ----
    # 用 MYVK_EDITOR_SELECT 预选角色 ⇒ Inspector 绑到它 ⇒ 只有它才有
    # `INS-LAYER-ADD` 的矩形日志。**不点它**（按钮在长面板深处，可能落在
    # 裁剪区外 —— 点下去点到别的东西，那种"假点击"比不点更糟）。
    env_ui = {
        "MYVK_EDITOR_PANELS": "inspector",
        "MYVK_EDITOR_INSP_TAB": "animation",
    }
    _, _, lines_ui = shoot("ui", [make_layer(LAYER_CLIP, 1.0, 1, ["upleg"])],
                           env=env_ui, wait=20.0, select=SEL)
    txt_ui = "\n".join(lines_ui)
    tags = sorted({ln.split("RECT ")[1].split()[0]
                   for ln in lines_ui if "RECT " in ln})
    ui_ok = "INS-LAYER-ADD" in txt_ui
    check("⑨ Inspector 画出 Animation Layers 段（INS-LAYER-ADD）", ui_ok,
          "tags=%d %s" % (len(tags), ",".join(tags[-6:])))

    # ---- 汇总 ----
    bad = [r for r in results if not r[1]]
    print("-" * 66)
    for name, ok, _ in results:
        print("%-5s %s" % ("PASS" if ok else "FAIL", name))
    print("结果: " + ("PASS" if not bad else "FAIL: %d 组未通过" % len(bad)))
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
