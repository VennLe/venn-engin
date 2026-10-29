#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""端到端验证：角色表现链 A0 + A1（导入 → 存盘 → 时间推进 → 逐实体蒙皮 → 动画器接管）。

被验证的是 `docs/character_pipeline_roadmap.md` 的 A0 五项与 A1。它们是**一条
因果链**，缺一环后面全废，所以分组也按因果走：

  A. 角色进得来（A0-1）
     · 在这之前 `SkinnedMeshComponent` 全仓库只有一处创建点，而且只作用于
       角色预览窗口的私有场景 → 从 Content 拖进视口的骨骼模型只是一堆静态
       网格（T-pose），Game.exe 走同一条导入路径 → 永远看不到蒙皮角色。
     · 现在 `engine/scene/ModelInstance` 是唯一装配入口，两条导入路径共用。
     · 断言锚点：`MODEL-INSTANCE ... skinned=1 joints=N`。

  B. 属性面板有东西（A0-1 的"看得见"，也是准入五件套的 UI 面）
     · `INS-ANIM-HEADER` / `INS-ANIM-TIME` 矩形出现 = Animation 页签真的
       画出了骨架信息与时间轴。以前这一页只有三个控件、而且 clip 下拉是空的
       （组件根本不存在）。

  C. 存盘往返（A0-2）
     · 选好 clip → `scene.save` → 读回 JSON 断言 `skinned` 块字段 →
       重启读档 → `SCENE-SKINNED ... ok=1 joints=N`。
     · ⚠ 同时反向验一条硬约束：**老场景没有 `skinned` 键 → 不建组件**。
       否则所有既有场景的渲染基线都会飘（"特性默认关 = 逐像素一致"）。

  D. 时间真的在推进（A0-3）+ 逐实体 jointBase（A0-4）
     · `ANIM-TICK ... dt>0` 出现 = 系统在 Play 态推进时间；
     · `ANIM-WRAP clip=.. dur=..` 出现 = 循环回绕真的发生了（dur=1.0 的
       Idle 跑满一秒就会绕）；
     · `ANIM-SKIN entity=.. jointBase=..` = 逐实体分配了骨骼 SSBO 下标。
     · ⚠ 这一组**不能**只断言 `ANIM-TICK` 出现：dt=0（编辑态）也会打。
       必须解析出 dt > 0 的那一条。

  E. 姿势真的在变（肉眼验收）
     · 切到 Walk、把 `time` 钉到 0 / 0.35，各截一张，像素均值差必须超过阈值。
     · ⚠ 为什么不"Play 态隔 0.4 秒抓两帧"、也不"同一个 Idle 钉两个时间"：
       两者实测都只有 **0.02/255** —— anime_girl 的 Idle 是坐姿呼吸，半秒内
       胸腔起伏不到一个像素。换到 Walk（全身位移）才有判别力。
     · 这不是"证明动画算对了"（那是 `MYVK_ANIM_TEST` 数值断言的活），而是
       "证明这条链一路走到了屏幕上"—— 采样算对了但没上传、或者 jointBase
       填错，都会在这里露馅。

  F. 多实例的 jointBase（A0-4 的否定测试）
     · 同一个 FBX 实例化 3 次 → 三条 `ANIM-SKIN` 的 `jointBase` 必须**两两不等**。
     · 旧实现把 base 存在 `unordered_map<const Mesh*, int>`，三个实例共享同一批
       `Mesh*`（AssetManager 去重）→ 共用一个 base → 三个人摆同一副姿势。
       回退到它，这一组必红。

  G. AnimatorComponent 接管 time（A1，2026-10-19）
     · G1 点 `Add Animator` → `INS-ANIM-ROOTMOTION` 出现（组件真的加上了，
       而且这是准入五件套里的 UI 面）。
     · G2 有 animator 的实体上 `anim.time` 必须打 `via=anim` —— 写 `sk.time`
       也能让当帧画面看着对，但下一帧就被镜像覆盖回去了。
     · G2b `scene.save` 之后 JSON 里有 `animator` 块，且 `time` 就是钉的那个值。
     · G3 重开：`SCENE-ANIMATOR time=0.250` = **存盘重开时间轴位置还在**；
       再钉 0.60 截一张与 G2 比像素 —— 差异只能来自 animator 驱动的姿势。

运行：python tools/verify_character_import.py
结尾一定是 `结果: PASS` / `结果: FAIL: ...`（run_regression.py 取**最后一条**）。
"""

from __future__ import annotations

import json
import os
import re
import sys
import tempfile
import time

import ctypes

from PIL import ImageChops

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from capture_window import capture_client, find_window, user32        # noqa: E402
from verify_add_menu import kill_stale_editors                        # noqa: E402
from verify_player import close, launch                               # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify")

MODEL = "assets/models/characters/animate_girl.fbx"
# 相机摆到 (3,-3,1.5) 看向原点（yaw=0 → 相机在 +X 侧，见 Camera::angleDir）。
# E 组用更近的机位：Idle 是坐姿、模型只占屏 100 px 左右，姿势差在这么大的
# 面积上会被背景稀释；Walk 站起来 + 腿在摆，拉近后差异才够硬。
CAM = "3,-3,1.5,45,19.5"
CAM_CLOSE = "1.8,-1.8,1.2,45,16"
# 多实例专用：三个模型沿 +X 排 0 / 1.5 / 3 m，得退到 ~8 m 外才全在画里
# （用 CAM_CLOSE 时第二、三个会跑出视口，截图上只剩一个 —— 那这张"三个姿势
# 不同"的截图就没有验收价值了）。
CAM_WIDE = "6,-6,3.2,45,18"

# 滚轮一格（Windows 的 WHEEL_DELTA）。只给 `scroll_tag_into_view` 用。
MOUSEEVENTF_WHEEL = 0x0800

# anime_girl.fbx 自带 4 个 clip（`MODEL-CLIPS` 锚点会打出来）：
#   Idle / Walk / Rig|Idle / Rig|Walk
# E 组固定用 Walk —— Idle 的呼吸幅度太小，做像素对照没有判别力。
CLIP = "Walk"

failures: list[str] = []


def check(msg: str, ok: bool) -> bool:
    print("    %-64s %s" % (msg, "PASS" if ok else "FAIL"))
    if not ok:
        failures.append(msg)
    return ok


def has(tap, needle: str) -> bool:
    return tap.has_line(needle)


def find_line(tap, needle: str):
    """返回第一条包含 needle 的日志行（历史累积，启动早期的行也在）。"""
    with tap.lock:
        for ln in tap.lines:
            if needle in ln:
                return ln
    return None


def find_lines(tap, needle: str) -> list[str]:
    """所有包含 needle 的日志行（多实例要数条数）。"""
    with tap.lock:
        return [ln for ln in tap.lines if needle in ln]


def kv(line: str, key: str) -> str | None:
    """从 `key=value` 形式的日志行里取值（value 到空白为止）。"""
    if not line:
        return None
    m = re.search(r"\b" + re.escape(key) + r"=([^\s]+)", line)
    return m.group(1) if m else None


def wait_line(tap, needle: str, timeout: float = 10.0):
    end = time.time() + timeout
    while time.time() < end:
        ln = find_line(tap, needle)
        if ln:
            return ln
        time.sleep(0.1)
    return None


def count_errors(tap) -> int:
    with tap.lock:
        return sum(1 for ln in tap.lines
                   if "[ERROR]" in ln or "VUID-" in ln)


def mean_abs_diff(img_a, img_b) -> float:
    """两张 RGB 图的平均逐通道绝对差（0..255）。**只作信息打印**，不作断言。

    ⚠ 这是个被稀释的度量：角色只占屏 ~4%，另外 96%（UI、地面、天空）逐像素
      相同，于是"整图均值"被压到 0.4/255 量级 —— 哪怕姿势天差地别。第一版
      就栽在这里（Idle 0.02、Walk 0.41，都过不了 0.5 的阈值，而截图肉眼一看
      就是"站着"和"跪着"）。
    """
    a = img_a.convert("RGB")
    b = img_b.convert("RGB")
    if a.size != b.size:
        return 255.0
    da = a.tobytes()
    db = b.tobytes()
    total = 0
    for i in range(0, len(da), 3):
        total += abs(da[i] - db[i]) + abs(da[i + 1] - db[i + 1]) + \
            abs(da[i + 2] - db[i + 2])
    return total / (len(da))


def diff_ratio(img_a, img_b, rect=None, thresh: int = 8) -> float:
    """**显著变化像素的占比**（0..1）。这才是这个场景该用的口径。

    只看"有多少像素真的变了"，不看"变了的像素变了多少"——于是画面里有
    96% 的静止区域也不会把信号稀释掉。`rect` 一般传视口矩形（`VP-RECT`），
    把左右面板彻底排除在外；传 None 或非法矩形就退化成整图。

    用 `ImageChops.difference` 走 C 实现（345k 像素的纯 Python 循环要 0.5 秒）。
    """
    a = img_a.convert("RGB")
    b = img_b.convert("RGB")
    if a.size != b.size:
        return 1.0
    box = (0, 0, a.size[0], a.size[1])
    if rect:
        x0, y0, x1, y1 = (int(v) for v in rect)
        # DPI / 客户区原点对不上时别让 crop 变成空图（空图的 ratio 恒为 1，
        # 会变成"永远通过"的假绿）—— 落回整图反而更安全。
        cx0, cy0 = max(0, x0), max(0, y0)
        cx1, cy1 = min(a.size[0], x1), min(a.size[1], y1)
        if cx1 - cx0 > 40 and cy1 - cy0 > 40:
            box = (cx0, cy0, cx1, cy1)
    ca = a.crop(box)
    cb = b.crop(box)
    hist = ImageChops.difference(ca, cb).convert("L").histogram()
    total = sum(hist)
    if total == 0:
        return 1.0
    return sum(hist[thresh + 1:]) / float(total)


# ============================================================
# A + B + C 的前半段：导入 → 面板 → 存盘
# ============================================================

def case_import_and_save(exe: str):
    """返回 (存盘路径, ok)。ok=False 表示前置失败，后续组直接跳过。"""
    print("\n[A] 导入骨骼模型 → 主场景里真的建了 SkinnedMeshComponent")
    os.makedirs(OUT_DIR, exist_ok=True)
    tmpdir = tempfile.mkdtemp(prefix="anim_scene_")
    scene_path = os.path.join(tmpdir, "anim_scene.json").replace("\\", "/")

    env = {
        "MYVK_EDITOR_SCENE": scene_path,
        "MYVK_EDITOR_IMPORT": MODEL,
        "MYVK_EDITOR_INSP_TAB": "animation",
        "MYVK_EDITOR_CAM": CAM,
        # 第 40 帧执行一次（那时布局/面板/命令表都就绪）
        "MYVK_EDITOR_CONSOLE_CMD": "scene.save",
    }
    proc, hwnd, tap = launch(exe, env, "1440x900")
    if not hwnd:
        check("编辑器启动", False)
        return None, False

    ok_all = True
    try:
        inst = wait_line(tap, "MODEL-INSTANCE", 12.0)
        ok_all &= check("MODEL-INSTANCE 出现（唯一装配入口被调用）",
                        inst is not None)
        if inst:
            ok_all &= check(
                "  路径 = %s" % MODEL, kv(inst, "path") == MODEL)
            ok_all &= check(
                "  skinned=1（**A0-1 的核心**：拖进来会建蒙皮组件）",
                kv(inst, "skinned") == "1")
            joints = int(kv(inst, "joints") or "0")
            ok_all &= check("  joints=%d > 0（骨架真的解析出来了）" % joints,
                            joints > 0)

        auto = find_line(tap, "IMPORT-AUTO")
        ok_all &= check("IMPORT-AUTO skinned=1（导入结果回读一致）",
                        auto is not None and kv(auto, "skinned") == "1")

        print("\n[B] Animation 页签真的画出来了（UI 面）")
        hdr = tap.last_rect("INS-ANIM-HEADER")
        ok_all &= check("INS-ANIM-HEADER 矩形存在（页面不空白）",
                        hdr is not None)
        tl = tap.last_rect("INS-ANIM-TIME")
        ok_all &= check("INS-ANIM-TIME 矩形存在（时间轴控件在）",
                        tl is not None)
        if tl:
            ok_all &= check("  时间轴有宽度（%.0f px）" % (tl[2] - tl[0]),
                            (tl[2] - tl[0]) > 40)

        # 截图（人工复核用；像素断言在 E 组）
        img, cap_ok = capture_client(hwnd)
        if cap_ok:
            img.save(os.path.join(OUT_DIR, "char_import_edit.png"))

        print("\n[C1] 存盘：场景 JSON 里必须有 skinned 块")
        # 等到第 40 帧的 scene.save 真的执行（日志是 "Scene saved:"）
        saved = wait_line(tap, "Scene saved:", 20.0)
        ok_all &= check("scene.save 执行", saved is not None)

        ok_all &= check("存盘路径 = 临时场景", os.path.isfile(scene_path))
        skin_json = None
        if os.path.isfile(scene_path):
            with open(scene_path, encoding="utf-8") as f:
                doc = json.load(f)
            for e in doc.get("entities", []):
                if "skinned" in e:
                    skin_json = e["skinned"]
                    break
        ok_all &= check("JSON 里有 skinned 块", skin_json is not None)
        if skin_json:
            ok_all &= check("  kind=fbx", skin_json.get("kind") == "fbx")
            ok_all &= check("  clipName 非空（%s）" % skin_json.get("clipName"),
                            bool(skin_json.get("clipName")))
            ok_all &= check("  loop=true（默认值）",
                            skin_json.get("loop") is True)
            ok_all &= check("  playing=true", skin_json.get("playing") is True)
            ok_all &= check("  speed=1.0",
                            abs(float(skin_json.get("speed", 0)) - 1.0) < 1e-6)

        ok_all &= check("全程零 [ERROR] / 零 VUID", count_errors(tap) == 0)
    finally:
        close(proc, hwnd)

    return scene_path, ok_all


# ============================================================
# C 的后半段：读档重建
# ============================================================

def case_reload(exe: str, scene_path: str):
    print("\n[C2] 读档：skinned 块必须重建出组件")
    ok_all = True
    env = {
        "MYVK_EDITOR_SCENE": scene_path,
        "MYVK_EDITOR_INSP_TAB": "animation",
        "MYVK_EDITOR_CAM": CAM,
    }
    proc, hwnd, tap = launch(exe, env, "1440x900")
    if not hwnd:
        check("编辑器启动（读档）", False)
        return False
    try:
        ln = wait_line(tap, "SCENE-SKINNED", 12.0)
        ok_all &= check("SCENE-SKINNED 出现", ln is not None)
        if ln:
            ok_all &= check("  ok=1（模型加载成功、骨架有效）",
                            kv(ln, "ok") == "1")
            ok_all &= check("  clip 与存盘一致（%s）" % kv(ln, "clip"),
                            bool(kv(ln, "clip")))
            joints = int(kv(ln, "joints") or "0")
            ok_all &= check("  joints=%d > 0" % joints, joints > 0)
        ok_all &= check("零 [ERROR] / 零 VUID", count_errors(tap) == 0)

        # ---- 反向断言：老场景没有 skinned 键 → 不建组件 ----
        # 用同一个场景文件删掉 skinned 块，读进来不该出现 SCENE-SKINNED。
        # ⚠ 这条是"逐像素一致"硬约束的守卫：如果哪天有人把读档改成
        #   "默认建组件"，既有场景的渲染基线会整体飘。
        with open(scene_path, encoding="utf-8") as f:
            doc = json.load(f)
        for e in doc.get("entities", []):
            e.pop("skinned", None)
        old_path = scene_path.replace(".json", "_old.json")
        with open(old_path, "w", encoding="utf-8") as f:
            json.dump(doc, f)
    finally:
        close(proc, hwnd)

    print("\n[C3] 老场景（没有 skinned 键）→ 不建组件（硬约束）")
    env2 = {"MYVK_EDITOR_SCENE": old_path}
    proc, hwnd, tap = launch(exe, env2, "1440x900")
    if not hwnd:
        check("编辑器启动（老场景）", False)
        return False
    try:
        wait_line(tap, "Scene loaded:", 12.0)
        time.sleep(1.0)
        ok_all &= check("没有 SCENE-SKINNED（老场景不凭空长组件）",
                        not has(tap, "SCENE-SKINNED"))
        ok_all &= check("零 [ERROR] / 零 VUID", count_errors(tap) == 0)
    finally:
        close(proc, hwnd)
    return ok_all


# ============================================================
# D：Play 态的时间推进
# ============================================================

def case_play(exe: str, scene_path: str):
    print("\n[D] Play 态：时间推进 + 循环回绕 + 逐实体 jointBase")
    ok_all = True
    env = {
        "MYVK_EDITOR_SCENE": scene_path,
        "MYVK_EDITOR_PLAY": "1",
        "MYVK_EDITOR_CAM": CAM,
    }
    proc, hwnd, tap = launch(exe, env, "1440x900")
    if not hwnd:
        check("编辑器启动（Play）", False)
        return False
    try:
        # ANIM-TICK 每 60 帧一条；Play 态 dt > 0，编辑态 dt == 0。
        # ⚠ 必须解析出 dt > 0 的那一条，否则"编辑态也在打日志"会假通过。
        end = time.time() + 25.0
        advanced = None
        while time.time() < end:
            with tap.lock:
                for ln in tap.lines:
                    if "ANIM-TICK" not in ln:
                        continue
                    d = kv(ln, "dt")
                    if d is not None and float(d) > 0.0:
                        advanced = ln
                        break
            if advanced:
                break
            time.sleep(0.15)
        ok_all &= check("ANIM-TICK 出现 dt>0（Play 态真的在推进时间）",
                        advanced is not None)

        if advanced:
            ents = int(kv(advanced, "entities") or "0")
            ok_all &= check("  entities=%d >= 1（系统看到了那个蒙皮实体）"
                            % ents, ents >= 1)

        wrap = wait_line(tap, "ANIM-WRAP", 8.0)
        ok_all &= check("ANIM-WRAP 出现（循环回绕执行了）", wrap is not None)
        if wrap:
            dur = float(kv(wrap, "dur") or "0")
            t = float(kv(wrap, "t") or "-1")
            ok_all &= check("  回绕后 t=%.3f 落在 [0, dur=%.2f)" % (t, dur),
                            0.0 <= t < dur + 1e-6)

        skin = wait_line(tap, "ANIM-SKIN", 8.0)
        ok_all &= check("ANIM-SKIN 出现（逐实体分配 jointBase）",
                        skin is not None)
        if skin:
            base = int(kv(skin, "jointBase") or "-99")
            ok_all &= check("  jointBase=%d >= 0" % base, base >= 0)

        ok_all &= check("零 [ERROR] / 零 VUID", count_errors(tap) == 0)
    finally:
        close(proc, hwnd)
    return ok_all


# ============================================================
# E：姿势真的上屏（确定性对照）
# ============================================================

def case_pose_pixels(exe: str, scene_path: str):
    """把 clip 切到 Walk、`time` 钉到两个值各截一张，像素差必须显著。

    ⚠ 两条踩过的坑：

    1) **不能用"Play 态隔 0.4 秒抓两帧"**：实测只差 **0.02/255** ——
       anime_girl 的 Idle 是坐姿呼吸，半秒内胸腔起伏不到一个像素。拿它当
       断言既没判别力、又会随模型换掉而随机红。
    2) **同一 clip 里钉两个时间也不够**：Idle 全程都是呼吸，t=0 与 t=0.5 的
       姿势差同样只有 0.02/255（实测）。所以除了钉时间，还得**换 clip** ——
       这个模型带 Idle / Walk 两套，Walk 是全身位移，腿部相位差是肉眼级。

    两次运行只有 clipName/time 不同（同相机、同 HUD、同场景文件），
    差异就只能是"动画真的算到了屏幕上"。
    """
    print("\n[E] 像素对照：Walk 的 time=0.00 与 time=0.35 姿势必须不同")
    shots = {}
    rect = None
    # 三次运行：同配置跑两遍拿到**抖动基线**（TAA 相位 / 时间戳文本之类的
    # 噪声），第三遍才改时间。阈值由此自校准 —— 判据必须是"信号显著高于
    # 噪声"，而不是一个拍脑袋的绝对数（换模型/换分辨率都不该红）。
    runs = (("a", "0.0"), ("a2", "0.0"), ("b", "0.35"))
    for tag, t in runs:
        env = {
            "MYVK_EDITOR_SCENE": scene_path,
            "MYVK_EDITOR_CAM": CAM_CLOSE,
            "MYVK_EDITOR_NO_HUD": "1",
            # ⚠ 必须先选中模型根：`anim.time` 作用在"当前选中"的
            #   SkinnedMeshComponent 上（与 Inspector 时间轴同一个目标）。
            #   少了这一行，命令会静默返回 "no SkinnedMeshComponent"，
            #   表现成"命令没执行"。
            "MYVK_EDITOR_SELECT": "animate_girl",
            # `;` 分隔多条：先切 clip，再钉时间（一个 env 只能传一串）。
            "MYVK_EDITOR_CONSOLE_CMD": "anim.clip %s;anim.time %s" % (CLIP, t),
        }
        proc, hwnd, tap = launch(exe, env, "1440x900")
        if not hwnd:
            check("编辑器启动（time=%s）" % t, False)
            return False
        try:
            ln = wait_line(tap, "ANIM-SETTIME", 20.0)
            if not ln or kv(ln, "ok") != "1":
                check("anim.time %s 执行成功（%s）" % (t, ln or "没有该行"),
                      False)
                return False
            sc = find_line(tap, "ANIM-SETCLIP")
            if not sc or kv(sc, "ok") != "1" or kv(sc, "clip") != CLIP:
                check("anim.clip %s 执行成功（%s）" % (CLIP, sc or "没有该行"),
                      False)
                return False
            time.sleep(0.6)     # 让设值后的那一帧真的画出来
            img, ok = capture_client(hwnd)
            if not ok:
                check("截图（time=%s）" % t, False)
                return False
            img.save(os.path.join(OUT_DIR, "char_pose_%s.png" % tag))
            shots[tag] = img
            if rect is None:
                rect = tap.last_rect("VP-RECT")
            if count_errors(tap) != 0:
                check("time=%s 零 [ERROR] / 零 VUID" % t, False)
        finally:
            close(proc, hwnd)

    noise = diff_ratio(shots["a"], shots["a2"], rect)
    signal = diff_ratio(shots["a"], shots["b"], rect)
    mad = mean_abs_diff(shots["a"], shots["b"])
    print("    (信息) 视口矩形=%s 抖动基线=%.3f%% 变化占比=%.3f%% 整图均值差=%.2f/255"
          % (rect, noise * 100.0, signal * 100.0, mad))
    ok_noise = check("抖动基线 %.3f%% < 0.20%%（同配置两跑几乎逐像素相同）"
                     % (noise * 100.0), noise < 0.0020)
    ok_signal = check("变化占比 %.3f%% > 0.50%%（姿势真的换了）"
                      % (signal * 100.0), signal > 0.0050)
    ok_ratio = check("变化占比是抖动的 %.1f 倍 > 10（信号远高于噪声）"
                     % ((signal / noise) if noise > 1e-9 else 9999.0),
                     noise < 1e-9 or signal > 10.0 * noise)
    return ok_noise and ok_signal and ok_ratio


# ============================================================
# F：多实例 → jointBase 必须逐实例（A0-4 的否定测试）
# ============================================================

def case_multi_instance(exe: str):
    """同一个 FBX 实例化 3 次 → 三条 `ANIM-SKIN` 的 `jointBase` 两两不等。

    这是 A0-4 的**否定测试**，也是整条角色链唯一能抓住"回退到旧实现"的断言：

    · 旧实现把 `jointBase` 存在 `unordered_map<const Mesh*, int>` 里。同一个
      FBX 导入三次会被 `AssetManager` 去重成**同一批 `Mesh*`**，于是三个
      实例共用同一个 base —— 三个角色摆同一副姿势（第三个覆盖前两个）。
    · 新实现把 base 写进**逐实体**的 `SkinnedMeshComponent::jointBase`。
      三个实例拿到 0 / 21 / 42 这种两两不等的偏移。

    断言只认"两两不等"而不是"等于 0/21/42"：偏移由 `updateSkinning` 的
    累加顺序决定，钉死具体数字会让这个用例因为无关的排序改动而红。

    顺手也验一下 `anim.time <t> <name>`：三个实例要能分别指名（名字靠
    `EditorScene::instantiateModel` 的唯一化后缀 `animate_girl_1/_2`）。
    """
    print("\n[F] 多实例：三个 ANIM-SKIN 的 jointBase 必须两两不等（A0-4）")
    tmpdir = tempfile.mkdtemp(prefix="anim_multi_")
    scene_path = os.path.join(tmpdir, "multi.json").replace("\\", "/")
    names = ["animate_girl", "animate_girl_1", "animate_girl_2"]
    times = ["0.0", "0.35", "0.7"]
    cmds = ["anim.clip %s" % CLIP]
    cmds += ["anim.time %s %s" % (t, n) for t, n in zip(times, names)]

    env = {
        "MYVK_EDITOR_SCENE": scene_path,   # 不存在的路径 → 新文档
        "MYVK_EDITOR_IMPORT": ";".join([MODEL] * 3),
        "MYVK_EDITOR_CAM": CAM_WIDE,
        "MYVK_EDITOR_NO_HUD": "1",
        "MYVK_EDITOR_SELECT": names[0],
        "MYVK_EDITOR_CONSOLE_CMD": ";".join(cmds),
    }
    proc, hwnd, tap = launch(exe, env, "1440x900")
    if not hwnd:
        check("编辑器启动（多实例）", False)
        return False
    ok_all = True
    try:
        # ---- 1) 三次导入都走了唯一装配入口 ----
        insts = find_lines(tap, "MODEL-INSTANCE")
        ok_all &= check("MODEL-INSTANCE 出现 3 次（次数=%d）" % len(insts),
                        len(insts) == 3)
        roots = [kv(ln, "root") for ln in insts]
        ok_all &= check("  三个 root 两两不同（%s）" % ",".join(roots),
                        len(set(roots)) == len(roots) == 3)
        ok_all &= check("  三个都 skinned=1",
                        all(kv(ln, "skinned") == "1" for ln in insts))

        # ---- 2) 核心断言：逐实体 jointBase ----
        skins = find_lines(tap, "ANIM-SKIN")
        # 三个实例各一条（base 变化才打；每帧重打会把这条断言淹掉）
        ok_all &= check("ANIM-SKIN 至少 3 条（条数=%d）" % len(skins),
                        len(skins) >= 3)
        bases = [int(kv(ln, "jointBase") or "-99") for ln in skins]
        ents = [kv(ln, "entity") for ln in skins]
        ok_all &= check("  三个 entity 两两不同（%s）" % ",".join(ents),
                        len(ents) >= 3 and len(set(ents[:3])) == 3)
        ok_all &= check("  jointBase 两两不等（%s）← A0-4 核心"
                        % ",".join(str(b) for b in bases[:3]),
                        len(bases) >= 3 and len(set(bases[:3])) == 3)
        ok_all &= check("  没有 -1（说明首帧顺序修对了）",
                        all(b >= 0 for b in bases))

        # ---- 3) 三个实例能分别点名（anim.time 的 name 参数）----
        sets = find_lines(tap, "ANIM-SETTIME")
        ok_ok = [ln for ln in sets if kv(ln, "ok") == "1"]
        ok_all &= check("三条 ANIM-SETTIME ok=1（条数=%d）" % len(ok_ok),
                        len(ok_ok) >= 3)
        set_ents = [kv(ln, "entity") for ln in ok_ok]
        ok_all &= check("  三次设的是三个不同实体（%s）" % ",".join(set_ents),
                        len(set_ents) >= 3 and len(set(set_ents[:3])) == 3)

        # ---- 4) 截图（人工复核三个姿势是否真的不同）----
        time.sleep(0.6)
        img, cap_ok = capture_client(hwnd)
        if cap_ok:
            img.save(os.path.join(OUT_DIR, "char_multi_3inst.png"))

        ok_all &= check("零 [ERROR] / 零 VUID", count_errors(tap) == 0)
    finally:
        close(proc, hwnd)
    return ok_all


# ============================================================
# G：AnimatorComponent 接管 time（A1，2026-10-19）
# ============================================================

def scroll_tag_into_view(hwnd, tap, tag, margin=48.0, max_steps=14):
    """把 Inspector 内容区滚到某个控件**真的落在客户区里**，返回最新矩形。

    ⚠ 为什么需要这一步（2026-10-27 实测，一条只值 30 秒的断言花了半小时）：

      `ui::logRect` 对**被裁剪掉的**控件照样打日志 —— 它只做去重，不做可见性
      判断。于是"矩形在日志里"完全推不出"控件在屏幕上"。Animation 页的
      `Add Animator` 就长在页面下方：

        窗口 1440x900 → 客户区只有 **1418x844**
        `INS-ANIM-ADDANIM` = (1192,858)-(1307,886)   ← 整块在客户区外

      此时 `click_tag` 会：拿到矩形（成功）→ 把鼠标 SendInput 到客户区外
      （毫无反应）→ 断言红。而它的两行前置断言（"按钮存在"、"点了"）
      **全是 PASS** —— "点了没反应但日志全绿"的经典形态。

      所以判据必须是"点之前先确认它在屏幕内"，而不是"矩形拿到了"。
      滚轮这一步就是用户会做的动作（滚动找按钮），对布局漂移免疫。
    """
    from verify_asset_drag import force_foreground, move_to

    class _ClientRect(ctypes.Structure):
        _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                    ("right", ctypes.c_long), ("bottom", ctypes.c_long)]

    rc = _ClientRect()
    user32.GetClientRect(hwnd, ctypes.byref(rc))
    cw, ch = rc.right, rc.bottom

    if not force_foreground(hwnd):
        return tap.last_rect(tag)
    # 指针落在内容区中部偏右（Inspector 停靠在右侧）。⚠ 别贴右边框：
    # 那里是滚动条，滚轮事件会被它吃掉（实测 x=cw-60 滚不动，x=cw-100 能滚）。
    move_to(hwnd, cw - 100, int(ch * 0.5))
    time.sleep(0.4)

    for _ in range(max_steps):
        r = tap.last_rect(tag)
        if not r:
            return None
        cy = (r[1] + r[3]) * 0.5
        if margin <= cy <= ch - margin:
            return r
        # 负 = 向下滚内容（控件 y 变小）
        delta = -120 if cy > ch - margin else 120
        user32.mouse_event(MOUSEEVENTF_WHEEL, 0, 0, delta, 0)
        time.sleep(0.25)
    return tap.last_rect(tag)


def click_tag(hwnd, tap, tag, timeout=8.0):
    """按日志矩形点一个 ImGui 控件。

    ⚠ 必须先 `force_foreground`：窗口不在最前时，第一下鼠标只用来**激活
      窗口**，进不了 ImGui —— 日志里一个字都不会有（"点了没反应"那一族）。
    """
    from verify_add_menu import click as _click
    from verify_add_menu import wait_rect
    from verify_asset_drag import force_foreground

    r = wait_rect(tap, tag, timeout)
    if not r:
        print("    (诊断) 等不到锚点 %s" % tag)
        return False
    if not force_foreground(hwnd):
        print("    (诊断) force_foreground 失败")
        return False

    # ⚠ 落点取矩形的**上 1/4**，不是中心。
    #
    #   被面板底边裁掉一半的控件（`Add Animator` 正好如此：矩形
    #   (1192,900)-(1307,928)，而客户区高 944、面板底边约 914 —— 只有
    #   y=900~914 那 14 px 在面板里），中心 y=914 恰好压在裁切线上，
    #   鼠标点下去等于点"面板外"。
    #
    #   上 1/4 对**正常控件**也一样可用（ImGui 的命中区是整个矩形），
    #   而且不依赖"面板底边到底在哪"这种随主题/DPI 变化的魔数。
    class _ClientRect(ctypes.Structure):
        _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                    ("right", ctypes.c_long), ("bottom", ctypes.c_long)]

    rc = _ClientRect()
    user32.GetClientRect(hwnd, ctypes.byref(rc))
    cx = (r[0] + r[2]) * 0.5
    cy = r[1] + (r[3] - r[1]) * 0.25
    # 兜底：万一把窗口缩到控件真在客户区外，也别把鼠标点到别的程序上。
    cy = min(max(cy, 4.0), rc.bottom - 6.0)
    cx = min(max(cx, 4.0), rc.right - 6.0)
    _click(hwnd, cx, cy)
    return True


def write_scene_with_animator(scene_path: str) -> str:
    """把 A0 存盘的场景复制一份，给挂了 skinned 的实体加上 `animator` 块。

    为什么由测试自己写这一块：G1 的"点按钮"发生在窗口里，没法在同一进程里
    再触发一次保存（控制台命令都在第 40 帧跑，早于点击）。所以这里模拟
    "**用户已经存过盘**"的工程状态，之后的读写全部由引擎完成 ——
    G2 让引擎把 animator 写进 JSON，G3 让引擎读回来。
    ⚠ 故意**不写 `model` 字段**：那正好覆盖"骨架来源回退到 skinned"这条路径
      （JSON 里有 model 的话就测不到了）。
    """
    with open(scene_path, encoding="utf-8") as f:
        doc = json.load(f)
    n = 0
    for e in doc.get("entities", []):
        if "skinned" in e:
            e["animator"] = {
                "defaultClip": CLIP,
                "controllerPath": "",
                "time": 0.0,
                "speed": 1.0,
                "playing": False,
                "loop": True,
                "rootMotion": 0,
            }
            n += 1
    assert n == 1, "期望恰有一个 skinned 实体，实际 %d" % n
    out = scene_path.replace(".json", "_anim.json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump(doc, f)
    return out


def case_animator(exe: str, scene_path: str):
    """A1 的 GUI 侧：UI 面 → 写盘 → 读档 → 像素。

    三件事分开证明（缺一件就证明不了"time 的权威从 UI 挪到系统"）：

    G1 · `Add Animator` 按钮真的建了组件（准入五件套里的 UI 面）。
         判据是**点完之后 `INS-ANIM-ROOTMOTION` 出现** —— 那一段只在
         `AnimatorComponent` 存在时才画，比"按钮变灰"可靠（灰不灰是 ImGui
         自己算的，与组件无关）。
    G2 · 有 animator 的实体上，`anim.time` 写的是**anim 那一份**（日志
         `via=anim`），并且 `scene.save` 把 `animator` 块写进了 JSON。
         ⚠ `via=sk` 也能让画面看着正常一帧 —— 下一帧就被镜像覆盖回去了。
    G3 · 重开工程：`SCENE-ANIMATOR ... time=0.250` 说明**存盘时的时间被读回来了**
         （这是验收原文"存盘重开时间轴位置还在"）；再钉到 0.60 截一张，
         与 G2 那张比像素 —— 差异只能来自 animator 驱动的姿势。
    """
    ok_all = True

    # ---------------- G1：UI 面 ----------------
    print("\n[G1] Add Animator：按钮建组件（A1 的 UI 面）")
    env1 = {
        "MYVK_EDITOR_SCENE": scene_path,
        "MYVK_EDITOR_SELECT": "animate_girl",
        "MYVK_EDITOR_INSP_TAB": "animation",
        "MYVK_EDITOR_CAM": CAM_CLOSE,
        "MYVK_EDITOR_NO_HUD": "1",
    }
    # ⚠ 窗口从 1440x900 提到 **1440x1200**（2026-10-27）。这一改动是本轮
    #   唯一"非功能"的改动，必须记账，因为它踩的坑很典型：
    #
    #   `Add Animator` 按钮长在 Animation 页的末尾。1440x900 时客户区只有
    #   **1418x844**，而按钮矩形是 (1192,858)-(1307,886) —— **整块在
    #   客户区之外**。这个坑极难自证，因为：
    #     · `ui::logRect` 对**被裁剪的**控件照样打日志（只去重，不做可见性判断）
    #       → "INS-ANIM-ADDANIM 存在" PASS
    #     · `click_tag` 只回报"矩形拿到了 + 点发出去了"
    #       → "点了 Add Animator" PASS
    #     · 但鼠标被 SendInput 到客户区外面 → 按钮压根没收到
    #       → "Animator 段出现" FAIL
    #   三行前置断言全绿、最后一行红，而根因在窗口高度上。
    #
    #   加高到 1200（屏幕 2560x1440）后：按钮落在 y=984（客户区 1144），
    #   余量 130 px —— 实测点击生效（`INS-ANIM-ROOTMOTION` 出现，
    #   位置 y=1079）。1000 高**不够**：那时按钮在 y=900、面板底边约 914，
    #   只剩 14 px 在面板里，ImGui 的命中测试照样点不中。
    #
    #   另外 `click_tag` 的落点改成"矩形上 1/4"，`scroll_tag_into_view`
    #   负责在必要时滚一滚 —— 两者是给别的用例兜底的，这里其实用不到。
    proc, hwnd, tap = launch(exe, env1, "1440x1200")
    if not hwnd:
        check("编辑器启动（G1）", False)
        return False
    try:
        ok_all &= check("INS-ANIM-TIMETEXT 存在（time/duration 文本，A1 新增）",
                        wait_rect_local(tap, "INS-ANIM-TIMETEXT", 10.0) is not None)
        ok_all &= check("INS-ANIM-ADDANIM 存在（还没有动画器时的入口）",
                        tap.last_rect("INS-ANIM-ADDANIM") is not None)
        # ⚠ 先滚进可见区再点：`INS-ANIM-ADDANIM` 在页面下方，1440x900 下
        #   会整块落在客户区外面（见 `scroll_tag_into_view` 的长注释）。
        r_btn = scroll_tag_into_view(hwnd, tap, "INS-ANIM-ADDANIM")
        ok_all &= check("Add Animator 在可见区内（滚轮滚进去）", r_btn is not None)
        clicked = bool(r_btn) and click_tag(hwnd, tap, "INS-ANIM-ADDANIM", 5.0)
        ok_all &= check("点了 Add Animator", clicked)
        got = wait_rect_local(tap, "INS-ANIM-ROOTMOTION", 8.0)
        ok_all &= check("点完之后 Animator 段出现（组件真的加上了）", got is not None)
        ok_all &= check("零 [ERROR] / 零 VUID（G1）", count_errors(tap) == 0)
    finally:
        close(proc, hwnd)

    # ---------------- G2：写盘 ----------------
    print("\n[G2] anim.time 写的是 anim 那一份，且 scene.save 带上 animator 块")
    anim_scene = write_scene_with_animator(scene_path)
    env2 = {
        "MYVK_EDITOR_SCENE": anim_scene,
        "MYVK_EDITOR_SELECT": "animate_girl",
        "MYVK_EDITOR_CAM": CAM_CLOSE,
        "MYVK_EDITOR_NO_HUD": "1",
        "MYVK_EDITOR_CONSOLE_CMD": "anim.clip %s;anim.time 0.25;scene.save" % CLIP,
    }
    shot_a = None
    rect = None
    proc, hwnd, tap = launch(exe, env2, "1440x900")
    if not hwnd:
        check("编辑器启动（G2）", False)
        return False
    try:
        ln = wait_line(tap, "SCENE-ANIMATOR", 12.0)
        ok_all &= check("SCENE-ANIMATOR 出现（读档建了组件）", ln is not None)
        if ln:
            ok_all &= check("  ok=1", kv(ln, "ok") == "1")
            # 故意不写 model → 走回退
            ok_all &= check("  model=0（JSON 里没写 model → 回退用 skinned）",
                            kv(ln, "model") == "0")
        st = wait_line(tap, "ANIM-SETTIME", 15.0)
        ok_all &= check("anim.time 执行成功", st is not None and kv(st, "ok") == "1")
        if st:
            ok_all &= check("  via=anim（写的是 AnimatorComponent 那一份，"
                            "不是被镜像覆盖的 sk.time）", kv(st, "via") == "anim")
        ok_all &= check("scene.save 执行", wait_line(tap, "Scene saved:", 20.0) is not None)
        time.sleep(0.6)
        img, cap = capture_client(hwnd)
        if cap:
            img.save(os.path.join(OUT_DIR, "char_anim_t025.png"))
            shot_a = img
        else:
            check("截图（G2）", False)
        rect = tap.last_rect("VP-RECT")
        ok_all &= check("零 [ERROR] / 零 VUID（G2）", count_errors(tap) == 0)
    finally:
        close(proc, hwnd)

    # JSON 里必须有 animator 块，且 time 是刚钉的 0.25
    print("\n[G2b] 存盘的 JSON 里有 animator 块")
    anim_json = None
    if os.path.isfile(anim_scene):
        with open(anim_scene, encoding="utf-8") as f:
            doc = json.load(f)
        for e in doc.get("entities", []):
            if "animator" in e:
                anim_json = e["animator"]
                break
    ok_all &= check("JSON 里有 animator 块（引擎写的，不是上面那份手写）",
                    anim_json is not None)
    if anim_json:
        t_saved = float(anim_json.get("time", -1.0))
        ok_all &= check("  time=%.3f ≈ 0.25（钉住的时间被存下来了）" % t_saved,
                        abs(t_saved - 0.25) < 1e-3)
        ok_all &= check("  playing=false（anim.time 顺手暂停了）",
                        anim_json.get("playing") is False)
        ok_all &= check("  clip 是 Walk（anim.clip 生效）",
                        anim_json.get("defaultClip") == CLIP)
        ok_all &= check("  没有 model 字段（回退路径，见 write_scene_with_animator）",
                        "model" not in anim_json)

    # ---------------- G3：重开 + 像素 ----------------
    print("\n[G3] 重开：存盘的时间被读回来；时间→姿势→像素这条链通")
    env3 = {
        "MYVK_EDITOR_SCENE": anim_scene,
        "MYVK_EDITOR_SELECT": "animate_girl",
        "MYVK_EDITOR_CAM": CAM_CLOSE,
        "MYVK_EDITOR_NO_HUD": "1",
        "MYVK_EDITOR_CONSOLE_CMD": "anim.time 0.60",
    }
    shot_b = None
    proc, hwnd, tap = launch(exe, env3, "1440x900")
    if not hwnd:
        check("编辑器启动（G3）", False)
        return False
    try:
        ln = wait_line(tap, "SCENE-ANIMATOR", 12.0)
        ok_all &= check("SCENE-ANIMATOR 出现（重开）", ln is not None)
        if ln:
            t_back = float(kv(ln, "time") or "-1")
            ok_all &= check("  time=%.3f ≈ 0.25（**存盘重开时间轴位置还在**）"
                            % t_back, abs(t_back - 0.25) < 1e-3)
        st = wait_line(tap, "ANIM-SETTIME", 15.0)
        ok_all &= check("anim.time 0.60 执行成功", st is not None and kv(st, "ok") == "1")
        time.sleep(0.6)
        img, cap = capture_client(hwnd)
        if cap:
            img.save(os.path.join(OUT_DIR, "char_anim_t060.png"))
            shot_b = img
        else:
            check("截图（G3）", False)
        ok_all &= check("零 [ERROR] / 零 VUID（G3）", count_errors(tap) == 0)
    finally:
        close(proc, hwnd)

    if shot_a is not None and shot_b is not None:
        sig = diff_ratio(shot_a, shot_b, rect)
        print("    (信息) 视口矩形=%s 变化占比=%.3f%%（t=0.25 vs t=0.60）"
              % (rect, sig * 100.0))
        ok_all &= check("变化占比 %.3f%% > 0.50%%（Animator 驱动的姿势真的换了）"
                        % (sig * 100.0), sig > 0.0050)
    else:
        ok_all &= check("两张截图都拿到了", False)

    return ok_all


def wait_rect_local(tap, tag, timeout=8.0):
    """等一个矩形日志出现（`verify_add_menu.wait_rect` 的同义包装，
    放在这里是为了不把 8 处 import 分散到函数体里）。"""
    from verify_add_menu import wait_rect
    return wait_rect(tap, tag, timeout)


def main() -> int:
    print("=" * 72)
    print("角色表现链 A0：导入 / 存盘 / 时间推进 / 逐实体蒙皮")
    print("=" * 72)

    if not os.path.isfile(EXE):
        print("结果: FAIL: 找不到 %s（先 ./make.cmd build）" % EXE)
        return 1

    kill_stale_editors()

    scene_path, ok_import = case_import_and_save(EXE)
    if ok_import and scene_path:
        case_reload(EXE, scene_path)
        case_play(EXE, scene_path)
        case_pose_pixels(EXE, scene_path)
        case_multi_instance(EXE)
        # A1（2026-10-19）：AnimatorComponent 接管 time 的权威
        case_animator(EXE, scene_path)
    else:
        print("\n（A/C1 失败 → 跳过后续组）")

    print("\n" + "=" * 72)
    if failures:
        print("结果: FAIL（%d 项未通过）" % len(failures))
        for f in failures:
            print("   - %s" % f)
        return 1
    print("结果: PASS（角色链 A0+A1：MODEL-INSTANCE → INS-ANIM → SCENE-SKINNED "
          "往返 → ANIM-TICK/WRAP/SKIN → 像素差 → 多实例 jointBase → "
          "Animator 接管 time，全部通过）")
    print("=" * 72)
    return 0


if __name__ == "__main__":
    sys.exit(main())
