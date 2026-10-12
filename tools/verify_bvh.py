"""端到端验证：场景空间加速结构（AABB 层次包围盒 / BVH）。

背景：这是"渲染性能"三件套里的第二件（视锥剔除 / 遮挡剔除 + 加速结构 /
LOD）。本脚本验证其中的**加速结构**部分：场景里所有可见网格实体的世界
空间 AABB 被组织成一棵 BVH，供渲染剔除与编辑器射线拾取共用。

断言分四组：

  1. **树是健康的**：节点数 / 深度都在"平衡二叉树"该有的量级 ——
     不是一条链（那样遍历就退化成 O(N)，白建）。
  2. **确实在剪枝**：视锥查询实际访过的节点数远小于总节点数。
  3. **结果与暴力扫描完全一致**：`BVH-DBG` 的 missing / extra 都为 0，
     并且 culled 的数值与"逐个实体直接测视锥"的基线（132 / 4）完全相同。
     加速结构可以慢，但**绝不能漏** —— 这一条是硬断言。
  4. **射线拾取**（BVH raycast）：点视口中心能选中最近的那个立方体，
     点空地则什么都不选中；且 `PICK-DBG` 的 same=1（与线性扫描一致）。

用法：
    python tools/verify_bvh.py
"""

import argparse
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import find_window, user32                       # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors, wait_rect     # noqa: E402
from verify_frustum_cull import build_scene, parse_stats              # noqa: E402
from verify_asset_drag import force_foreground, \
    MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP                          # noqa: E402
import verify_player as P                                            # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = os.path.join(ROOT, "..", "_venn_verify")

CAM = "0,0,3,180,0"          # 站在 (0,0,3) 朝 +X 平视
GROUND_TRUTH_CULLED = 132    # 该场景下"直接测视锥"的结果（BVH 之前就验过）
GROUND_TRUTH_DRAWN = 4


def launch(exe, scene):
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_FRUSTUM_DBG"] = "1"
    env["MYVK_BVH_DBG"] = "1"
    env["MYVK_EDITOR_SCENE"] = scene
    env["MYVK_EDITOR_CAM"] = CAM

    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    hwnd = None
    for _ in range(40):
        hwnd = find_window("Venn Editor")
        if hwnd:
            break
        time.sleep(0.25)
    if not hwnd:
        proc.kill()
        return None, None, tap
    user32.ShowWindow(hwnd, 9)
    time.sleep(1.5)
    user32.SetWindowPos(hwnd, 0, 0, 0, 1280, 800, 0x0002 | 0x0004)
    time.sleep(2.5)
    return proc, hwnd, tap


def close(proc, hwnd):
    if hwnd:
        user32.PostMessageW(hwnd, 0x0010, 0, 0)
    time.sleep(0.5)
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=6)
        except subprocess.TimeoutExpired:
            proc.kill()


def click_client(hwnd, cx, cy):
    """在视口里点一下 (cx, cy)（客户端坐标）。

    必须用这套注入，不能用 SetCursorPos + mouse_event 那一版：
    后者的"移动"在某些情况下不会变成 ImGui 看到的 MousePos（ImGui 的 GLFW
    后端在光标进入窗口后只吃光标位置回调，不轮询 glfwGetCursorPos），于是
    "点了空地"实际用的是**上一次**的鼠标位置 —— 会静默地点到别的东西上，
    断言就会以一种很奇怪的方式失败。
    """
    P.mouse_set_client(hwnd, cx, cy)
    time.sleep(0.35)
    user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.12)
    user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
    time.sleep(0.5)


def last_match(tap, needle, regex, timeout=10.0):
    """等日志里出现含 needle 的行，返回最后一个的 regex 匹配。"""
    end = time.time() + timeout
    found = None
    while time.time() < end:
        with tap.lock:
            for line in tap.lines:
                if needle in line:
                    m = regex.search(line)
                    if m:
                        found = m
        if found:
            # 再多等一会，让"稳定后"的条目覆盖掉首帧那条
            time.sleep(1.0)
            with tap.lock:
                for line in tap.lines:
                    if needle in line:
                        m = regex.search(line)
                        if m:
                            found = m
            return found
        time.sleep(0.2)
    return found


BVH_RE = re.compile(r"BVH-DBG brute=(\d+) bvh=(\d+) missing=(\d+) extra=(\d+)")
PICK_RE = re.compile(r"PICK-DBG bvh=(\d+)\(d=([\d.]+)\) linear=(\d+)"
                     r"\(d=([\d.]+)\) same=(\d+) rayNodes=(\d+) rayLeafTests=(\d+)")
SEL_RE = re.compile(r"pick: selected '([^']*)'")
MISS_RE = re.compile(r"pick: nothing hit")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    os.makedirs(OUT_DIR, exist_ok=True)
    if not os.path.exists(exe):
        print("找不到可执行文件，请先构建：", exe)
        return 1

    scene = os.path.join(OUT_DIR, "cull_test_scene.json")
    total = build_scene(scene)
    print("测试场景：%s（%d 个立方体）" % (scene, total))

    fails = []

    def check(name, cond, detail=""):
        print(("  [PASS] " if cond else "  [FAIL] ") + name +
              (("  " + str(detail)) if detail else ""))
        if not cond:
            fails.append(name)

    kill_stale_editors()

    # ================= 第 1 组：树结构 + 剪枝 + 与暴力一致 =================
    print("\n== 第 1 组：BVH 树结构 / 剪枝 / 与暴力扫描一致 ==")
    proc, hwnd, tap = launch(exe, scene)
    if not hwnd:
        print("  编辑器启动失败")
        return 2
    try:
        st = None
        end = time.time() + 15.0
        while time.time() < end and parse_stats(tap) is None:
            time.sleep(0.25)
        st = parse_stats(tap)
        bvh = last_match(tap, "BVH-DBG", BVH_RE)
        print("  CULL-STATS:", st)
        print("  BVH-DBG   :", bvh.group(0) if bvh else None)

        check("拿到 CULL-STATS", st is not None)
        check("拿到 BVH-DBG 自检", bvh is not None)
        if not st or not bvh:
            return 3

        nodes = int(st.get("nodes", 0))
        depth = int(st.get("depth", 0))
        visits = int(st.get("visits", 0))

        check("BVH 非空（nodes > 0）", nodes > 0, nodes)
        # 平衡二叉树：136 个物体 / 每叶 4 个 → 约 34 个叶子 → 深度约 6~7。
        # 上限放到 12 是留余量：真要退化成一条链，深度会接近 34。
        check("树是平衡的（3 <= depth <= 12）", 3 <= depth <= 12, depth)
        check("确实在剪枝（0 < visits < nodes）",
              0 < visits < nodes, "visits=%d nodes=%d" % (visits, nodes))
        # 剪枝力度：至少跳过一半的节点
        check("剪枝力度过半（visits <= nodes/2）", visits <= nodes // 2,
              "visits=%d nodes=%d" % (visits, nodes))

        brute = int(bvh.group(1))
        bvhN = int(bvh.group(2))
        missing = int(bvh.group(3))
        extra = int(bvh.group(4))
        check("BVH 一个都没漏（missing == 0）", missing == 0, missing)
        check("BVH 没有多留（extra == 0）", extra == 0, extra)
        check("与暴力扫描数量一致（bvh == brute）", bvhN == brute,
              "bvh=%d brute=%d" % (bvhN, brute))
        check("可见数与基线一致（brute == %d）" % GROUND_TRUTH_DRAWN,
              brute == GROUND_TRUTH_DRAWN, brute)

        # 与"BVH 之前"的剔除基线逐值对照 —— 换实现不该改变结果
        check("culled == %d（与线性扫描基线逐值相同）" % GROUND_TRUTH_CULLED,
              st.get("culled") == GROUND_TRUTH_CULLED, st.get("culled"))
        check("drawnOpaque == %d" % GROUND_TRUTH_DRAWN,
              st.get("drawnOpaque") == GROUND_TRUTH_DRAWN,
              st.get("drawnOpaque"))
    finally:
        close(proc, hwnd)

    # ================= 第 2 组：射线拾取（BVH raycast）=================
    print("\n== 第 2 组：射线拾取（走 BVH raycast）==")
    proc, hwnd, tap = launch(exe, scene)
    if not hwnd:
        print("  编辑器启动失败")
        return 4
    try:
        # 鼠标事件必须落在真正的活动窗口上才会被 GLFW 收到
        check("编辑器窗口拿到前台焦点", force_foreground(hwnd))

        vp = wait_rect(tap, "VP-RECT", 10.0)
        check("拿到 VP-RECT", vp is not None, vp)
        if not vp:
            return 5
        vx0, vy0, vx1, vy1 = vp

        # (a) **预热点击**（关键，别删）。
        #
        # 注入的**第一次**左键点击会被 Windows 拿去"激活窗口"，ImGui 收不到
        # 它 —— 而拾取的条件是 `hovered && IsMouseClicked(Left)`，于是这一下
        # 完全静默：既不选东西、也不打 `pick:` 日志，连 PICK-DBG 都不出
        # （`PickingSystem::pick` 根本没被调用）。
        #
        # 这曾经把这条用例坑成一个"看起来很合理"的假失败：**正中那一下
        # （全程第一次点击）丢失**，而右下角那一下（第二次）正常出日志 ——
        # 报出来就是"视口正中点不中 Cube_0"，像是在怀疑 BVH 射线或机位。
        # 实测（移走自检日志的早退之后仍然一行 PICK-DBG 都没有）才定位到
        # 是注入时序，不是拾取逻辑。
        #
        # 所以先右下角空地上点一次把窗口"激活"掉；顺便它自己就是一次
        # 有意义的对照（该处应当什么都打不中）。后面的断言才做数。
        click_client(hwnd, vx0 + (vx1 - vx0) * 0.94, vy0 + (vy1 - vy0) * 0.94)
        warm = last_match(tap, "pick: nothing hit", MISS_RE, timeout=6.0)
        check("预热点击能到达视口（第一次注入点击会被窗口激活吃掉）",
              warm is not None)

        # (b) 点视口正中：射线沿 +X，最近的是 (5,0,3) 的 Cube_0
        click_client(hwnd, (vx0 + vx1) / 2, (vy0 + vy1) / 2)
        sel = last_match(tap, "pick: selected", SEL_RE, timeout=6.0)
        pk = last_match(tap, "PICK-DBG", PICK_RE, timeout=6.0)
        print("  正中点击 ->", sel.group(1) if sel else None)
        print("  PICK-DBG :", pk.group(0) if pk else None)
        check("视口正中点中 Cube_0（沿视线最近的立方体）",
              sel is not None and sel.group(1) == "Cube_0",
              sel.group(1) if sel else "没等到 pick 日志")
        check("拾取自检 same == 1（BVH 与线性扫描同结果）",
              pk is not None and pk.group(5) == "1",
              pk.group(0) if pk else None)
        if pk:
            rayNodes = int(pk.group(6))
            rayLeaf = int(pk.group(7))
            st2 = parse_stats(tap)
            nodes2 = int(st2.get("nodes", 0)) if st2 else 0
            # 场景是一张"薄饼"（所有物体都在 z≈3 的一个平面里，相机还在
            # 饼内部），射线的起点就在根节点里 —— 顶层几个大盒子必然都被
            # 穿过，指望像视锥查询那样砍掉一半是过头的期待。这里只要求
            # "确实在剪枝 + 叶子层面大幅收敛"。
            check("射线遍历确实在剪枝（rayNodes < nodes）",
                  0 < rayNodes < nodes2,
                  "rayNodes=%d nodes=%d" % (rayNodes, nodes2))
            check("射线叶子测试远少于物体总数（< %d）" % (total // 2),
                  rayLeaf < total // 2,
                  "rayLeafTests=%d total=%d" % (rayLeaf, total))

        # (c) 点视口右下角：射线朝右下方，打不到任何立方体。
        #     注意**不能点左下角** —— 那里是导航小图标（轴指示器）的隐形
        #     按钮，它会先把这一下吃掉（NAV-GIZMO press），点选根本不会执行。
        #
        # ⚠ 判据是"这次点击**新增**了一条 pick 日志"，不能只看"日志里有
        #   `pick: nothing hit`" —— 上面那次预热点击本身就会打一条一样的，
        #   只看"有没有"的话这一条会**恒真**（预热把它喂饱了）。
        def miss_count():
            with tap.lock:
                return sum(1 for l in tap.lines if MISS_RE.search(l))

        before = miss_count()
        click_client(hwnd, vx0 + (vx1 - vx0) * 0.94, vy0 + (vy1 - vy0) * 0.94)
        miss = last_match(tap, "pick: nothing hit", MISS_RE, timeout=6.0)
        check("点空地 -> 什么都没选中（这一下真的产生了 pick）",
              miss is not None and miss_count() > before,
              "miss 日志 %d -> %d" % (before, miss_count()))
    finally:
        close(proc, hwnd)

    print("\n结果: %s" % ("PASS" if not fails else "FAIL: " + ", ".join(fails)))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
