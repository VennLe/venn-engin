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

坑 2：**`anime_girl.fbx` 的 `Idle` 段数据是坏的 —— 腿会被压成尖刺。**
  10-19 的 `char_import_edit.png`（Idle、t≈0）就是一根竖条，而同一天
  `anim.clip Walk; anim.time 0.6` 是正常 chibi。这不是渲染 bug，是那段
  clip 的骨骼缩放关键帧有问题（`docs/animate_girl_import.md` §5 记着
  Blender 那套 `Copy Transforms` 把源骨架的 0.01 缩放一起搬过去了）。
  后果：**不能拿 Idle 当"正常姿势"的基准**，否则"上半身没动"这条判据
  比对的是两坨尖刺。
  所以底座用 `Walk`，上层也用 `Walk`（相位不同：底座 0.30 s、层 0 s）——
  "遮罩只换该换的骨"这件事跟"是不是两段不同的动画"无关，同段不同相位
  反而更干净：除了相位没有任何别的变量。唯一用 `Idle` 当上层的是 ④：
  它的头/手臂是好的（坏的只有腿），而 `Allow=chest` 正好把腿挡在外面。

================================================================
判据（九组）
================================================================

  ① **同一场景跑两次**：显著变化 < 0.15%（噪声地板，逐像素抖动来自
     轮廓抗锯齿，**不能写 0**）。
  ② **层（Allow=thigh）把腿换成另一个相位**：下半带显著变化 > 0.5%。
  ③ **同一帧里上半带一个像素都不动**：< 噪声地板。②③ 是主判据 ——
     "只换该换的骨"在屏幕上就长这样；遮罩要是被忽略（整具身体都被覆盖），
     ③ 会跟着变成 5% 左右。
     ⚠ 上下是**两条线**（`UPPER_END` / `LOWER_START`），不是画面正中 ——
     手臂垂在大腿旁边，中线会把两者混在一起（见常量处的注释）。
  ④ **反方向（Allow=chest，层 = Idle）**：上半带（头/发/手臂）动、
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
  ⑦ **Deny 是镜像**：Deny=chest 时下半带被换掉（与 ② 同量级）。
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

# FBX 而不是 glb：glb 是 Blender 的原始导出，进引擎只有 0.37 m 高（Y-up 的
# 1.556 正好躺在水平轴上），FBX 那条路走 `ufbx` 的 right_handed_z_up 映射，
# 进场景就是 1.556 m 的人形。
MODEL = "assets/models/anime_girl.fbx"
TPL = os.path.join(OUT_DIR, "anim_layers_tpl.json")

BASE_CLIP = "Walk"
# 底座的相位。层的 `time` **不落盘**（运行时字段，读档一律重置为 0），
# 所以"两个相位"只能这样凑：底座 0.30 s、层 0 s。
BASE_TIME = 0.30
LAYER_CLIP = "Walk"

# 机位：3/4 前侧、抬到角色胸口高度（角色 1.556 m、脚在 z=0）、略俯视。
CAM = "2.10,-2.10,0.85,45,6"

# 烤模板用的机位（会写进模板的 camera 块，之后的变体都复用它）
TPL_CAM = "3,-3,1.5,45,19.5"

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
# 为什么不能用"画面正中"当分界：这个角色是**长腿 chibi**，手臂垂在大腿
# 旁边 —— 实测（把变化像素按行统计，分母 = 真实 VP-RECT 448x531）：
#   · 遮罩=thigh 的变化落在 0.28 ~ 0.72
#   · 遮罩=chest 的变化落在 0.00 ~ 0.48
# 两段在 0.28~0.48 重叠（前臂垂到大腿外侧），所以任何单条分界线都会把
# "手臂"和"大腿"混在一起（第一版用 0.52，"上半身没动"量出来 5.2%，判据红
# 而实现是对的）。改成两条线，各取**只有一类骨能影响**的一段：
#   · 0.00 .. 0.28   只有头/发/肩 —— chest 子树能改，thigh 子树碰不到
#   · 0.52 .. 1.00   只有膝下/脚 —— thigh 子树能改，chest 子树碰不到
# 实测上半带 thigh=0.000%、下半带 chest=0.000%，两侧都贴着噪声地板。
# ⚠ 这两个数是**针对当前机位与角色**量的。换机位/换模型必须重量 ——
#   否则判据会静默退化成"两边都算它一半"（每个都 5%，红得莫名其妙）。
UPPER_END = 0.28
LOWER_START = 0.52

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


def bake_template(exe):
    """用"从 Content 拖进视口"的既有钩子烤一份**带子网格实体**的模板。

    走这条路而不是手写实体的理由见文件头坑 1。`scene.save` 在第 40 帧
    执行一次（`MYVK_EDITOR_CONSOLE_CMD`），那时的布局 / 面板 / 命令表都
    就绪了；`MYVK_EDITOR_CAM` 会写进模板的 camera 块。
    """
    if os.path.isfile(TPL):
        os.remove(TPL)
    os.makedirs(OUT_DIR, exist_ok=True)
    # ⚠ 清场必须在 find_window 之前：上一轮的 Editor.exe 只要还活着，
    #   find_window 就会摸到**那具尸体**（旧窗口），后面的截图全落在它身上，
    #   而新开的那份收不到任何消息 —— 症状是"截图一直是 None"。
    kill_stale_editors()
    env = _base_env()
    env.update({
        "MYVK_EDITOR_SCENE": TPL,
        "MYVK_EDITOR_IMPORT": MODEL,
        "MYVK_EDITOR_CAM": TPL_CAM,
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
        if not os.path.isfile(TPL):
            raise SystemExit("[FAIL] 烤模板：scene.save 没落盘")
    finally:
        _close(proc, hwnd)
    with open(TPL, encoding="utf-8") as f:
        doc = json.load(f)
    sub = sum(1 for e in doc["entities"] if e.get("parent") is not None
              and e.get("parent") >= 0 and e.get("mesh"))
    if sub < 1:
        raise SystemExit("[FAIL] 烤模板：没有子网格实体（角色不会出现在屏幕上）")
    return TPL


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
                                [make_layer(LAYER_CLIP, 1.0, 1, ["thigh"])])
    m_legs = diff_mask(a0, legs, vp)
    up, lo = regions(m_legs, h, "Allow=thigh")
    check("② 遮罩内（下半身）显著变化", lo > CHANGED_MIN,
          "%.2f%% (> %.2f%%)" % (lo * 100.0, CHANGED_MIN * 100.0))
    check("③ 遮罩外（上半身）几乎不变", up < NOISE_MAX,
          "%.3f%% (< %.3f%%)" % (up * 100.0, NOISE_MAX * 100.0))
    check("③ 变化集中在遮罩内（下半 / 上半 > 20 倍）",
          lo > up * 20.0 + 0.002,
          "lower=%.2f%% upper=%.3f%%" % (lo * 100.0, up * 100.0))

    # ---- 4) 反方向：头/手臂换、腿一个像素不动 ----
    arms, _, _ = shoot("arms", [make_layer("Idle", 1.0, 1, ["chest"])])
    m_arms = diff_mask(a0, arms, vp)
    up2, lo2 = regions(m_arms, h, "Allow=chest")
    check("④ Allow=chest 上半身动了", up2 > CHANGED_MIN,
          "%.2f%% (> %.2f%%)" % (up2 * 100.0, CHANGED_MIN * 100.0))
    check("④ Allow=chest 下半身一个像素没动", lo2 < NOISE_MAX,
          "%.3f%% (< %.3f%%)" % (lo2 * 100.0, NOISE_MAX * 100.0))

    # ---- 5) Mask=All + 权重 1 = 完全替换 ----
    # 基线换成"直接播 Idle、t=0"（层那个 time 不落盘，读档就是 0）。
    idle0, _, _ = shoot("idle0", None, clip="Idle", t=0.0)
    allm, _, _ = shoot("all", [make_layer("Idle", 1.0)])
    r_all = float(diff_mask(idle0, allm, vp).mean())
    check("⑤ Mask=All + 权重 1 ≈ 直接播那段（端点逐位）", r_all < NOISE_MAX,
          "%.3f%% (< %.3f%%)" % (r_all * 100.0, NOISE_MAX * 100.0))

    # ---- 6) 关掉必须回到基线 ----
    w0, _, _ = shoot("w0", [make_layer(LAYER_CLIP, 0.0, 1, ["thigh"])])
    r_w0 = float(diff_mask(a0, w0, vp).mean())
    check("⑥ 权重 0 ≈ 基线", r_w0 < NOISE_MAX,
          "%.3f%% (< %.3f%%)" % (r_w0 * 100.0, NOISE_MAX * 100.0))

    nc, _, _ = shoot("noclip", [make_layer("", 1.0, 1, ["thigh"])])
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
    half, _, _ = shoot("half", [make_layer(LAYER_CLIP, 0.5, 1, ["thigh"])])
    m_half = diff_mask(a0, half, vp)
    m_half_vs_full = diff_mask(half, legs, vp)
    _, lo_half = regions(m_half, h, "权重 0.5 vs 基线")
    _, lo_hf = regions(m_half_vs_full, h, "权重 0.5 vs 权重 1")
    check("⑥ 权重 0.5 既不是 0 也不是 1（下半带两张都不像）",
          lo_half > CHANGED_MIN and lo_hf > CHANGED_MIN,
          "vs base=%.2f%% vs full=%.2f%%" % (lo_half * 100.0, lo_hf * 100.0))

    # ---- 7) Deny 是镜像 ----
    deny, _, _ = shoot("deny", [make_layer(LAYER_CLIP, 1.0, 2, ["chest"])])
    _, lo3 = regions(diff_mask(a0, deny, vp), h, "Deny=chest")
    check("⑦ Deny=chest 下半身被换掉", lo3 > CHANGED_MIN,
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
    _, _, lines_ui = shoot("ui", [make_layer(LAYER_CLIP, 1.0, 1, ["thigh"])],
                           env=env_ui, wait=20.0, select="anime_girl")
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
