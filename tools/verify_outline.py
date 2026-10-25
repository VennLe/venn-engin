"""端到端验证：NPR 描边（深度 + 法线边缘检测）。

## 这一刀在验什么

描边的实现**不是** inverted-hull（不外画一遍几何、不需要平滑法线、
不动顶点格式），而是：

  1. 深度预通道本来就要把不透明几何画一遍 → 顺手多写一个颜色附件
     （`HdrTarget::kGeometryFormat`，G16R16F）：
        rgb = 世界空间法线（-1..1）
        a   = 视图空间线性距离（米，= 1/gl_FragCoord.w）
  2. `post.frag` 采这张图，8 邻域取 max：
        相对深度差  |d0-d1| / min(d0,d1)  > 阈值
        法线夹角    1 - dot(n0,n1)        > 阈值
     两者都过 smoothstep 软化后乘到显示域颜色上。

所以本脚本要证明的是四件事，缺一条这个实现就不成立：

  A. **关掉必须回到基线。** `MYVK_OUTLINE=0` 与 `STRENGTH=0` 两张图都要
     与"未开描边"的运行**逐像素相同**（max diff = 0）。这是全仓的硬纪律：
     新功能关闭时不能改变任何像素。`strength=0` 那条尤其重要 —— 它走的是
     "着色器里整段执行但 mix 权重为 0"的路径，比"整个 if 跳过"更容易写错。

  B. **开了必须真的画出线。** 与关掉的图相比，显著差异像素 ≥ 5000 且
     变暗像素的平均降幅 ≥ 40/255（实测 87.6 —— 线是黑紫色的）。

  C. **天空不能有线。** 几何缓冲的 a 通道被清成 0，是"这里没有几何"的
     哨兵。判据写错（把 0 当"距离 0 的物体"）的症状是**整片天空被描成
     黑边** —— 而且不报任何错、日志干净。所以上部 12% 的天空区域里
     "新增的暗像素"必须恰好为 0。

  D. **线宽参数真的接线了。** width 1.5 → 3.0，差异像素数至少要涨 30%
     （实测 13792 → 24963，+81%）。

## 为什么这么跑

  · 用 **Game.exe**（运行时），不用编辑器：编辑器视口默认不摆角色，
    而运行时的 `createPlayer` 会走 `instantiateModel` —— 那是唯一既有
    完整子网格、又有第三人称跟随相机的路径。
  · 场景用 `runtime_test/player_scene.json`：地面 + 胶囊体 + 台阶 + 箱子
    + 弹力箱，朝向各不相同，一次覆盖"轮廓 / 遮挡边 / 硬棱边 / 平滑曲面"
    四种情况。**没有骨骼动画**，所以两次运行的画面逐像素可复现
    （已断言，见 `determinism`）。
  · 每轮 `--title` 给唯一标题：`find_window` 是**前缀**匹配，默认标题
    会同时命中 "Venn Editor"，跑到一半抓到编辑器窗口截图就全错。

## 期望输出

    结果: PASS（... 共 N 项）

零 Vulkan 验证层报错是前置条件（有 VUID 时后续像素断言没有意义）。
"""

import argparse
import os
import subprocess
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from capture_window import capture_client, find_window, user32  # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Game.exe")
BIN_DIR = os.path.dirname(DEFAULT_EXE)
OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify")

SCENE = "scenes/runtime_test/player_scene.json"
# ⚠ 这个变量在场景里的 Player 自带 `mesh` 时**不生效**（player_scene.json
#   的 Player 是胶囊体 primitive）。留着是给"换成没有 mesh 的 Player 场景"
#   时用的；第一次跑以为模型没加载，其实是场景覆盖了环境变量。
#   对这个用例反而更好：胶囊体是平滑曲面，正好验证"平滑面内部不该出线"。
PLAYER_MODEL = "models/characters/alicia.fbx"

# 窗口标题必须唯一（见文件头）
TITLE = "VennOutlineTest"

# 显著差异的判据（单通道最大差）
DIFF_EPS = 16

# 每轮启动后等多久再截图。这套场景是静态的（无骨骼动画），相机在无输入
# 时收敛后就不再动；9 秒足够跑过 PSO 编译 + 光照聚簇初始化那几帧。
WAIT_SEC = 9.0


def clean_env(**extra):
    """剥掉全部 MYVK_* 再叠 extra —— 测试的输入必须自述。

    不剥的话，开发机上残留的 MYVK_OUTLINE=1 会让"A 组（关描边）"实际
    上开着描边，然后 B 组与 A 组无差异 → 报"描边没画出来"，而代码是对的。
    """
    env = {k: v for k, v in os.environ.items() if not k.startswith("MYVK_")}
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_ENV_DBG"] = "0"
    env["MYVK_RUNTIME_PLAYER_MODEL"] = PLAYER_MODEL
    env.update({k: str(v) for k, v in extra.items()})
    return env


def run_once(exe, tag, extra_env, wait=WAIT_SEC):
    """跑一轮，返回 (RGB 数组, 日志行, 错误行)。

    返回 numpy 数组而不是 PIL 图：后面全是差分，转一次就够。
    """
    shot = os.path.join(OUT_DIR, "outline_%s.png" % tag)
    if os.path.exists(shot):
        os.remove(shot)

    proc = subprocess.Popen(
        [exe, "--title", TITLE, "--scene", SCENE],
        cwd=os.path.dirname(exe), env=clean_env(**extra_env),
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, encoding="utf-8", errors="replace", bufsize=1)
    tap = LogTap()
    tap.start(proc)

    hwnd = None
    try:
        hwnd = find_window(TITLE, timeout=25.0)
        if not hwnd:
            return None, [], ["未找到游戏窗口（被别的进程抢了标题？）"]
        user32.ShowWindow(hwnd, 9)
        # 显式摆到 (0,0) + 固定尺寸：Win32 的级联摆放会让两次启动的客户区
        # 差几十像素，截图尺寸一变，后面的逐像素差分就全错位。
        user32.SetWindowPos(hwnd, 0, 0, 0, 1280, 720, 0x0002 | 0x0004)
        time.sleep(wait)

        img = capture_client(hwnd)[0]
        if img is not None:
            img.save(shot)
        with tap.lock:
            lines = list(tap.lines)
    finally:
        if hwnd:
            user32.PostMessageW(hwnd, 0x0010, 0, 0)   # WM_CLOSE
        time.sleep(0.4)
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()

    errs = [l for l in lines
            if ("VUID" in l or "VALIDATION" in l or " ERROR" in l
                or "Error:" in l or "Assertion" in l or "assert" in l)]
    arr = None if img is None else np.asarray(img.convert("RGB"), dtype=np.int16)
    return arr, lines, errs


def diff_max(a, b):
    """逐像素的**单通道最大绝对差**（0..255）。

    不用 RGB 欧氏距离：描边是"把某些像素压暗"，三通道变化量差别大，
    欧氏距离会把"只变了蓝通道"和"三通道全变"混在一起算。
    """
    h = min(a.shape[0], b.shape[0])
    w = min(a.shape[1], b.shape[1])
    return np.abs(a[:h, :w] - b[:h, :w]).max(axis=2)


class Checker:
    def __init__(self):
        self.rows = []
        self.failures = []

    def check(self, name, ok, detail=""):
        self.rows.append((name, bool(ok), detail))
        print("  [%s] %-28s %s" % ("PASS" if ok else "FAIL", name, detail))
        if not ok:
            self.failures.append("%s —— %s" % (name, detail))

    def result(self, summary):
        print()
        print("=" * 72)
        if self.failures:
            print("结果: FAIL（%d 项）" % len(self.failures))
            for f in self.failures:
                print("   - %s" % f)
            print("=" * 72)
            return 1
        print("结果: PASS（%s，共 %d 项）" % (summary, len(self.rows)))
        print("=" * 72)
        return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    args = ap.parse_args()

    if not os.path.exists(args.exe):
        print("找不到可执行文件: %s" % args.exe)
        return 2
    os.makedirs(OUT_DIR, exist_ok=True)
    kill_stale_editors()

    print("=" * 72)
    print("NPR 描边：深度 + 法线边缘检测（深度预通道的几何缓冲）")
    print("=" * 72)
    print("exe   : %s" % args.exe)
    print("scene : %s" % SCENE)
    print()

    # ---- 四轮运行 ----
    # 组名 → env。off 跑两次是为了证明"关掉是确定性的"（那次 max 必须是 0）。
    runs = [
        ("off", {"MYVK_OUTLINE": "0"}),
        ("off2", {"MYVK_OUTLINE": "0"}),
        ("on", {"MYVK_OUTLINE": "1"}),
        ("s0", {"MYVK_OUTLINE": "1", "MYVK_OUTLINE_STRENGTH": "0"}),
        ("w3", {"MYVK_OUTLINE": "1", "MYVK_OUTLINE_WIDTH": "3"}),
    ]
    imgs = {}
    all_errs = []
    for tag, env in runs:
        print("[%s] 启动 ..." % tag)
        arr, lines, errs = run_once(args.exe, tag, env)
        imgs[tag] = arr
        all_errs.extend(errs)
        if arr is None:
            print("      截图失败")
        else:
            print("      %dx%d  lines=%d  errs=%d"
                  % (arr.shape[1], arr.shape[0], len(lines), len(errs)))
    print()

    c = Checker()

    # ---- 前置：有图、无报错 ----
    ok_imgs = all(v is not None for v in imgs.values())
    c.check("shots_available", ok_imgs,
            "五轮都拿到截图" if ok_imgs else "有一轮没截到图")
    if not ok_imgs:
        return c.result("")

    sizes = {v.shape for v in imgs.values()}
    c.check("shots_same_size", len(sizes) == 1,
            "全部 %dx%d" % (imgs["off"].shape[1], imgs["off"].shape[0])
            if len(sizes) == 1 else "尺寸不一致: %s" % sorted(sizes))

    c.check("no_validation_errors", not all_errs,
            "零报错" if not all_errs else "%d 条: %s"
            % (len(all_errs), all_errs[0].strip()[:90]))

    # ---- A. 关掉必须回到基线 ----
    d_det = diff_max(imgs["off"], imgs["off2"])
    c.check("determinism", int(d_det.max()) == 0,
            "两次关描边运行逐像素相同" if d_det.max() == 0
            else "max=%d，画面不可复现（有动画/物理在跑？）" % int(d_det.max()))

    d_s0 = diff_max(imgs["off"], imgs["s0"])
    c.check("strength_zero_is_baseline", int(d_s0.max()) == 0,
            "strength=0 与关描边逐像素相同" if d_s0.max() == 0
            else "max=%d（mix 权重没生效？）" % int(d_s0.max()))

    # ---- B. 开了必须真的画出线 ----
    d_on = diff_max(imgs["off"], imgs["on"])
    cnt_on = int((d_on > DIFF_EPS).sum())
    pct_on = cnt_on * 100.0 / d_on.size
    c.check("outline_changes_image", cnt_on >= 5000,
            "%d 像素显著差异（%.2f%%），max=%d" % (cnt_on, pct_on, int(d_on.max())))

    mask = d_on > DIFF_EPS
    if mask.any():
        # 变暗：off 的亮度 - on 的亮度。线是暗紫，所以这个值应当很大。
        lum_off = imgs["off"].mean(axis=2)
        lum_on = imgs["on"].mean(axis=2)
        drop = float((lum_off[mask] - lum_on[mask]).mean())
        c.check("outline_darkens", drop >= 40.0,
                "变暗像素平均降幅 %.1f/255" % drop)
    else:
        c.check("outline_darkens", False, "没有差异像素可统计")

    # ---- C. 天空不能有线 ----
    # 上部 12% 是纯天空（场景里地平线在画面 1/4 处）。几何缓冲的 a 被清成
    # 0 之后，这里只可能有一个结果：一个暗像素都没有。
    h = imgs["off"].shape[0]
    sky = slice(0, int(h * 0.12))
    sky_on = imgs["on"][sky].mean(axis=2)
    new_dark = int((sky_on < 64).sum())
    c.check("sky_has_no_outline", new_dark == 0,
            "天空新增暗像素 %d 个" % new_dark
            + ("（哨兵正确）" if new_dark == 0
               else "（a=0 被判成'距离 0 的物体'了）"))

    # ---- D. 线宽接线 ----
    d_w3 = diff_max(imgs["off"], imgs["w3"])
    cnt_w3 = int((d_w3 > DIFF_EPS).sum())
    c.check("width_scales_coverage", cnt_w3 >= int(cnt_on * 1.3),
            "width 3.0 → %d 像素，width 1.5 → %d（%.2fx）"
            % (cnt_w3, cnt_on, cnt_w3 / float(max(cnt_on, 1))))

    print()
    print("对比图: %s" % os.path.join(OUT_DIR, "outline_on.png"))
    return c.result("基线可复现 / 天空哨兵 / 线宽接线 / 像素差异")


if __name__ == "__main__":
    sys.exit(main())
