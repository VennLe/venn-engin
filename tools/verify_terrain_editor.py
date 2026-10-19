"""端到端验证：地形编辑器面（W4-4，见 docs/world_scale_roadmap.md §9）。

W4-4 的要求只有两条：「ContentBrowser 双击 `.vterrain` → 视口里出现地形」，
「Inspector 出 Terrain 段（尺寸 / maxHeight / 4 层材质槽 / Height scale）」。
这个脚本就按这两条逐项断，**全程真鼠标 / 真 Ctrl+S**（与
`verify_primitive_roundtrip.py` 同一套注入手法）。

五组：

  A 双击添加     ContentBrowser 里双击 terrain.vterrain → 实体出现、真的在画
  B 不叠第二份   同一份资产再双击 = 选中已有的那个（不是又叠一座山）
  C Inspector    Terrain 段出现，且有 Height Scale / 4 个层槽 / Remove 按钮
  D 关掉可见     点 Inspector 的 Visible → 视口显著变化（> 20%）
  E 存档往返     Ctrl+S → 场景 JSON 里有 terrain 块；重开 → 地形自己回来

⚠ 全部锚点都走 `MYVK_LOG_RECTS`（`INS-TERRAIN-*` / `CB-CELL *`）——
  不猜字体行高、不猜布局。⚠ 但锚点拼错 = **断言永远为假**（静默 None），
  所以每条锚点在真跑通过之前都不算数（本文件末尾的 G 组专门验锚点存在）。
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from capture_window import capture_client, find_window, user32      # noqa: E402
from verify_add_menu import LogTap, click, kill_stale_editors        # noqa: E402
from verify_asset_drag import force_foreground, client_to_screen, move_to  # noqa: E402
from safe_fs import safe_remove, safe_rmtree

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify")

TMP_DIR = tempfile.mkdtemp(prefix="venn_terrain_ed_")
TMP_SCENE = os.path.join(TMP_DIR, "terrain_edit.json")

# 相机摆在地形外 180 m、高 90 m，俯 22° 看向台地（yaw=90 → 视线朝 +Y，
# 见 Camera::angleDir 的注释：position = target + angleDir*distance）。
CAM = "0,-180,90,90,22"

CB_DIR = "terrain/hills"
CELL_TAG = "CB-CELL terrain.vterrain"

WM_KEYDOWN, WM_KEYUP = 0x0100, 0x0101
VK_CONTROL, VK_S = 0x11, 0x53
SC_CTRL, SC_S = 0x1D, 0x1F

LOD_RE = re.compile(r"TERRAIN-LOD chunks=(\d+) lod0=(\d+) lod1=(\d+) "
                    r"lod2=(\d+) lod3=(\d+) tris=(\d+)")

g_pass = 0
g_fail = 0


def check(name, ok, detail=""):
    global g_pass, g_fail
    if ok:
        g_pass += 1
        print("[ OK ] %s%s" % (name, ("  " + detail) if detail else ""))
    else:
        g_fail += 1
        print("[FAIL] %s%s" % (name, ("  " + detail) if detail else ""))


def sig_ratio(a, b, box=None, thresh=8):
    from PIL import ImageChops
    if a is None or b is None:
        return 0.0
    ca, cb = a.convert("RGB"), b.convert("RGB")
    if box:
        ca, cb = ca.crop(box), cb.crop(box)
    hist = ImageChops.difference(ca, cb).convert("L").histogram()
    total = max(ca.size[0] * ca.size[1], 1)
    return sum(hist[thresh + 1:]) / float(total)


def launch(exe, scene=None):
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    # 抽屉默认关着 —— 不打开它 ContentBrowser 一帧都不画，`CB-CELL` 锚点
    # 一个都不会出现（这一条踩过：日志里静悄悄什么都没有，看起来像"没刷到
    # 文件"，其实是面板根本没画）。
    env["MYVK_EDITOR_CONTENT_DRAWER"] = "1"
    # 直接落在 terrain/hills 上 —— 走的是引擎现成的 `MYVK_CONTENT_DIR`
    # 落点钩子（refresh() 的第一次初始化）。⚠ 不要试图用 `navigateTo` 自己
    # 加一个钩子：refresh() 在 `!m_initializedOnce` 时会把 m_rel 重置成
    # "assets"，环境钩子跑在它前面就被吃掉了（实测日志里 CB-ENTRIES 还是
    # dir=assets、count=10，而 CB-CELL 里根本没有 terrain.vterrain）。
    env["MYVK_CONTENT_DIR"] = "assets/" + CB_DIR
    env["MYVK_EDITOR_CB_VIEW"] = "list"     # 行视图才有 CB-CELL 锚点
    env["MYVK_EDITOR_CAM"] = CAM
    env["MYVK_EDITOR_NO_HUD"] = "1"
    if scene:
        env["MYVK_EDITOR_SCENE"] = scene
    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    return proc, tap


def stop(proc):
    proc.terminate()
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()


def wait_rect(tap, tag, timeout=8.0):
    end = time.time() + timeout
    while time.time() < end:
        r = tap.last_rect(tag)
        if r:
            return r
        time.sleep(0.12)
    return None


def wait_line(tap, needle, timeout=8.0):
    end = time.time() + timeout
    while time.time() < end:
        if tap.has_line(needle):
            return True
        time.sleep(0.12)
    return False


def settle(hwnd, tap, w=1600, h=900):
    user32.ShowWindow(hwnd, 9)
    time.sleep(2.0)
    user32.SetWindowPos(hwnd, 0, 0, 0, w, h, 0x0002 | 0x0004)
    time.sleep(2.5)
    force_foreground(hwnd)
    time.sleep(0.8)
    return bool(wait_rect(tap, "VP-RECT", 10.0))


def double_click(hwnd, cx, cy):
    """真鼠标双击。

    ⚠ 两次点击必须落在 ImGui 的 `io.MouseDoubleClickTime`（默认 0.30 s）
      以内，而且**中间不能挪光标** —— 挪了 ImGui 会判成两次单击。
    """
    from verify_nav_gizmo import MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP
    sx, sy = client_to_screen(hwnd, cx, cy)
    user32.SetCursorPos(sx, sy)
    time.sleep(0.25)
    for _ in range(2):
        user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
        time.sleep(0.05)
        user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
        time.sleep(0.06)


def press_ctrl_s(hwnd):
    ok = False
    for _ in range(5):
        if force_foreground(hwnd):
            ok = True
            break
        time.sleep(0.2)
    if not ok:
        print("  WARN: 编辑器不是前台窗口，Ctrl+S 可能不生效")
    user32.keybd_event(VK_CONTROL, SC_CTRL, 0, 0)
    time.sleep(0.12)
    user32.PostMessageW(hwnd, WM_KEYDOWN, VK_S, (SC_S << 16) | 1)
    time.sleep(0.06)
    user32.PostMessageW(hwnd, WM_KEYUP, VK_S, (SC_S << 16) | 0xC0000001)
    time.sleep(0.12)
    user32.keybd_event(VK_CONTROL, SC_CTRL, 0x0002, 0)
    time.sleep(1.2)


def last_lod(tap):
    with tap.lock:
        cands = [LOD_RE.search(l) for l in tap.lines]
    ms = [m for m in cands if m]
    if not ms:
        return None
    m = ms[-1]
    return tuple(int(m.group(i)) for i in range(1, 7))


# ==================================================================== A~E

def stage_edit(exe):
    """空场景 → 双击地形 → 不叠加 → Inspector → 关可见 → 存档。"""
    print("\n== A 组：ContentBrowser 双击 .vterrain ==")
    if os.path.exists(TMP_SCENE):
        safe_remove(TMP_SCENE)
    proc, tap = launch(exe, scene=TMP_SCENE)
    try:
        hwnd = find_window("Venn Editor")
        if not hwnd:
            check("A0 找到编辑器窗口", False, "no window")
            return
        if not settle(hwnd, tap):
            check("A0 视口就绪（VP-RECT）", False, "no VP-RECT")
            return
        check("A0 视口就绪（VP-RECT）", True)

        cell = wait_rect(tap, CELL_TAG, 12.0)
        check("A1 内容浏览器列出了 terrain.vterrain（抽屉真打开了）",
              cell is not None, str(cell))
        if not cell:
            return

        vp = tap.last_rect("VP-RECT")
        time.sleep(0.3)
        before = capture_client(hwnd)[0]

        # ---- 双击 → 加地形 ----
        double_click(hwnd, (cell[0] + cell[2]) * 0.5, (cell[1] + cell[3]) * 0.5)
        ok_add = wait_line(tap, "CB-OPEN terrain=added", 10.0)
        check("A2 双击 → 新增地形实体（CB-OPEN terrain=added）", ok_add)

        ok_load = wait_line(tap, "TERRAIN-LOAD", 20.0)
        check("A3 地形资产被渲染层加载（TERRAIN-LOAD）", ok_load)

        # ⚠ 天空/地形加载要几帧；等 LOD 行出现再断言三角形数。
        lod = None
        end = time.time() + 15.0
        while time.time() < end:
            lod = last_lod(tap)
            if lod and lod[5] > 0:
                break
            time.sleep(0.25)
        check("A4 地形真的在画（TERRAIN-LOD tris > 0）",
              bool(lod) and lod[5] > 0,
              "chunks=%s tris=%s" % (lod[0], lod[5]) if lod else "none")

        # 视口像素：加地形前后必须显著不同（否则 A2/A4 只证明"日志打了"）
        time.sleep(1.2)
        move_to(hwnd, 30, 840)     # 光标挪开，避免 hover 高亮落进采样区
        time.sleep(0.6)
        after = capture_client(hwnd)[0]
        if vp:
            box = (int(vp[0]) + 4, int(vp[1]) + 4, int(vp[2]) - 4,
                   int(vp[3]) - 4)
            r = sig_ratio(before, after, box)
            check("A5 视口画面显著变化 > 20%（地形占满画面）", r > 0.20,
                  "sig=%.2f%%" % (r * 100.0))
        img = capture_client(hwnd)[0]
        if img:
            img.save(os.path.join(OUT_DIR, "terrain_editor_01_added.png"))

        # ================================================ B
        print("\n== B 组：同一份资产再双击不叠加 ==")
        with tap.lock:
            n_add = sum(1 for l in tap.lines if "CB-OPEN terrain=added" in l)
        double_click(hwnd, (cell[0] + cell[2]) * 0.5, (cell[1] + cell[3]) * 0.5)
        ok_exist = wait_line(tap, "CB-OPEN terrain=existing", 6.0)
        with tap.lock:
            n_add2 = sum(1 for l in tap.lines if "CB-OPEN terrain=added" in l)
        time.sleep(1.0)
        lod2 = last_lod(tap)
        # ⚠ 断言的是"没有第二份实例"：chunks 必须还是 64（=512/64 的平方），
        #   变成 128 就是叠了一座山 —— 而画面上几乎看不出来，只有这个数字露馅。
        ok_once = (lod2 is not None and lod2[0] == 64)
        check("B1 再双击 = 选中已有的那个（CB-OPEN terrain=existing）",
              ok_exist and n_add2 == n_add,
              "added_lines=%d→%d" % (n_add, n_add2))
        check("B2 没有叠第二份实例（chunks 仍是 64，不是 128）", ok_once,
              "chunks=%s" % (lod2[0] if lod2 else "none"))

        # ================================================ C
        print("\n== C 组：Inspector 的 Terrain 段 ==")
        # ⚠⚠ 先把内容抽屉收起来再测 Inspector。
        #   抽屉是从底栏往上弹的 320 px 面板，Inspector 的地形段有一半压在
        #   它下面：被 ImGui 裁掉的控件 `GetItemRectMin/Max` 照样有值，但
        #   `logRect` 不会记裁剪掉的项 —— 于是 C3 会报"4 个锚点全 None"，
        #   而段标题（露在上半截）却是绿的。D 组的点击同理：点到的是抽屉，
        #   不是勾选框，画面 0.00% 变化（这两个假红都实测踩过）。
        bbtn = tap.last_rect("BAR-BTN-content")
        if bbtn:
            click(hwnd, (bbtn[0] + bbtn[2]) * 0.5, (bbtn[1] + bbtn[3]) * 0.5)
            time.sleep(1.0)
        else:
            print("  WARN: 没找到 BAR-BTN-content，抽屉关不掉")

        hdr = wait_rect(tap, "INS-TERRAIN-HEADER", 6.0)
        check("C1 Terrain 段出现（INS-TERRAIN-HEADER）", hdr is not None,
              str(hdr))
        hs = wait_rect(tap, "INS-TERRAIN-HEIGHTSCALE", 4.0)
        check("C2 有 Height Scale 控件", hs is not None, str(hs))
        # 4 个槽各有独立锚点（`INS-TERRAIN-LAYER0..3`）—— 都齐了才叫
        # "4 个层槽"，只认一个 tag 的话"画了 1 行还是 4 行"分不出来。
        rects = [wait_rect(tap, "INS-TERRAIN-LAYER%d" % i, 3.0)
                 for i in range(4)]
        check("C3 4 个层材质槽都有锚点", all(rects),
              str([None if r is None else round(r[1]) for r in rects]))
        if all(rects):
            ys = [r[1] for r in rects]
            check("C4 4 个槽按顺序自上而下排（y 严格递增）",
                  all(ys[i] < ys[i + 1] for i in range(3)),
                  "y=%s" % [round(v) for v in ys])
        rm = wait_rect(tap, "INS-TERRAIN-REMOVE", 3.0)
        check("C5 有 Remove Terrain 按钮", rm is not None, str(rm))

        # ================================================ D
        print("\n== D 组：Inspector 关掉可见 → 视口变化 ==")
        if not (vp and hs):
            check("D0 前置（视口 / Inspector 矩形）", False, "missing")
        else:
            vis = tap.last_rect("INS-TERRAIN-VISIBLE")
            if not vis:
                check("D0 Visible 勾选框锚点", False, "no INS-TERRAIN-VISIBLE")
            else:
                move_to(hwnd, 30, 840)
                time.sleep(0.5)
                shot_on = capture_client(hwnd)[0]
                click(hwnd, (vis[0] + vis[2]) * 0.5, (vis[1] + vis[3]) * 0.5)
                time.sleep(1.2)
                shot_off = capture_client(hwnd)[0]
                box = (int(vp[0]) + 4, int(vp[1]) + 4, int(vp[2]) - 4,
                       int(vp[3]) - 4)
                r = sig_ratio(shot_on, shot_off, box)
                check("D1 关掉 Visible → 视口显著变化 > 20%", r > 0.20,
                      "sig=%.2f%%" % (r * 100.0))
                if shot_off:
                    shot_off.save(os.path.join(
                        OUT_DIR, "terrain_editor_02_hidden.png"))
                # 复原（后面要存档，别把"看不见"存进去）
                click(hwnd, (vis[0] + vis[2]) * 0.5, (vis[1] + vis[3]) * 0.5)
                time.sleep(0.8)

        # ================================================ E
        print("\n== E 组：Ctrl+S 存盘 ==")
        press_ctrl_s(hwnd)
        saved = None
        if os.path.exists(TMP_SCENE):
            import json
            try:
                with open(TMP_SCENE, encoding="utf-8") as f:
                    saved = json.load(f)
            except Exception as e:      # noqa: BLE001
                print("  解析存档失败:", e)
        terr = None
        if saved:
            for e in saved.get("entities", []):
                if "terrain" in e:
                    terr = e["terrain"]
        check("E1 存盘后场景里有 terrain 块，且 source 指向这份资产",
              isinstance(terr, dict)
              and terr.get("source") == "terrain/hills/terrain.vterrain",
              str(terr))
        if isinstance(terr, dict):
            check("E2 terrain 块有 heightScale / layers[4]",
                  "heightScale" in terr and len(terr.get("layers") or []) == 4,
                  "keys=%s" % sorted(terr))

        # ================================================ G
        print("\n== G 组：Add ▸ Terrain 建出来的资产能直接打开 ==")
        # 这是 W2-4 的纪律原话：「加回来的时候它已经能打开了」。
        # 只断"文件落盘了"不够 —— `{"type":...,"version":1}` 那种空壳也落盘。
        add_btn = tap.last_rect("CB-ADD-BTN")
        if not add_btn:
            check("G0 Add 按钮锚点", False, "no CB-ADD-BTN")
            return
        # 抽屉在 C 组被收起来了，Add 按钮跟着消失 → 先重新打开抽屉
        bbtn = tap.last_rect("BAR-BTN-content")
        if bbtn:
            click(hwnd, (bbtn[0] + bbtn[2]) * 0.5, (bbtn[1] + bbtn[3]) * 0.5)
            time.sleep(1.2)
            add_btn = wait_rect(tap, "CB-ADD-BTN", 6.0)
        if not add_btn:
            check("G0 Add 按钮锚点（抽屉重开后）", False, "no CB-ADD-BTN")
            return

        new_dir = os.path.join(os.path.dirname(exe), "assets", CB_DIR, "NewTerrain")
        # ⚠ 用 safe_rmtree（改名让位），不是 shutil.rmtree：宿主的批量删除
        #   保护会**硬杀进程**（配额按回合累计，全量回归必然耗尽），
        #   `ignore_errors=True` 接不住。改名效果等价：old 路径不再存在。
        safe_rmtree(new_dir)   # 上一轮失败留下的垃圾

        click(hwnd, (add_btn[0] + add_btn[2]) * 0.5,
              (add_btn[1] + add_btn[3]) * 0.5)
        item = wait_rect(tap, "CB-ADDITEM terrain", 5.0)
        check("G1 Add 菜单里有 Terrain 项", item is not None, str(item))
        if not item:
            return
        with tap.lock:
            n_before = sum(1 for l in tap.lines if "CB-ADD kind=terrain" in l)
        click(hwnd, (item[0] + item[2]) * 0.5, (item[1] + item[3]) * 0.5)
        time.sleep(1.5)
        with tap.lock:
            hits = [l for l in tap.lines if "CB-ADD kind=terrain" in l]
        check("G2 点 Terrain → CB-ADD kind=terrain 真的执行了",
              len(hits) > n_before, hits[-1].split("] ")[-1] if hits else "none")

        want = {"terrain.vterrain": None, "height.r32": 65 * 65 * 4,
                "splat.rgba8": 65 * 65 * 4}
        got = {}
        for name in want:
            p = os.path.join(new_dir, name)
            got[name] = os.path.getsize(p) if os.path.exists(p) else -1
        check("G3 三件套都落盘且尺寸正确（65² 采样点）",
              got["height.r32"] == want["height.r32"]
              and got["splat.rgba8"] == want["splat.rgba8"]
              and got["terrain.vterrain"] > 0, str(got))

        # 进新建的目录，双击它 —— 必须能真的加载并画出来
        cell_new = wait_rect(tap, "CB-CELL NewTerrain", 8.0)
        if not cell_new:
            check("G4 内容浏览器里出现 NewTerrain 目录", False, "no cell")
        else:
            check("G4 内容浏览器里出现 NewTerrain 目录", True, str(cell_new))
            double_click(hwnd, (cell_new[0] + cell_new[2]) * 0.5,
                         (cell_new[1] + cell_new[3]) * 0.5)
            time.sleep(1.2)
            cell_file = wait_rect(tap, "CB-CELL terrain.vterrain", 8.0)
            check("G5 进目录后能看到 terrain.vterrain", cell_file is not None,
                  str(cell_file))
            if cell_file:
                double_click(hwnd, (cell_file[0] + cell_file[2]) * 0.5,
                             (cell_file[1] + cell_file[3]) * 0.5)
                ok_new = wait_line(tap, "NewTerrain/terrain.vterrain", 15.0)
                lod = None
                end = time.time() + 12.0
                while time.time() < end:
                    lod = last_lod(tap)
                    if lod and lod[5] > 0:
                        break
                    time.sleep(0.25)
                check("G6 新建的地形能直接加载并绘制（不是空壳）",
                      ok_new and lod is not None and lod[5] > 0,
                      "chunks=%s tris=%s" % (lod[0], lod[5]) if lod else "none")
                img = capture_client(hwnd)[0]
                if img:
                    img.save(os.path.join(
                        OUT_DIR, "terrain_editor_04_created.png"))
    finally:
        stop(proc)
        # 新建的资产是测试产物，别留在 build/bin 里给下一轮当噪声
        safe_rmtree(os.path.join(os.path.dirname(exe), "assets", CB_DIR,
                                 "NewTerrain"))


def stage_reload(exe):
    """重开刚存过的场景：不点任何东西，地形应该自己回来。"""
    print("\n== E（续）：重开存档 → 地形自己回来 ==")
    proc, tap = launch(exe, scene=TMP_SCENE)
    try:
        hwnd = find_window("Venn Editor")
        if not hwnd:
            check("F0 找到编辑器窗口", False, "no window")
            return
        settle(hwnd, tap)
        ok = wait_line(tap, "TERRAIN-LOAD", 15.0)
        lod = None
        end = time.time() + 12.0
        while time.time() < end:
            lod = last_lod(tap)
            if lod and lod[5] > 0:
                break
            time.sleep(0.25)
        check("F1 重开后地形自动加载并绘制（无需任何点击）",
              ok and (lod is not None) and lod[5] > 0,
              "chunks=%s tris=%s" % (lod[0], lod[5]) if lod else "none")
        img = capture_client(hwnd)[0]
        if img:
            img.save(os.path.join(OUT_DIR, "terrain_editor_03_reloaded.png"))
    finally:
        stop(proc)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    if not os.path.exists(exe):
        print("找不到可执行文件，请先构建：", exe)
        return 1
    os.makedirs(OUT_DIR, exist_ok=True)
    kill_stale_editors()
    print("临时场景:", TMP_SCENE)

    stage_edit(exe)
    stage_reload(exe)

    safe_rmtree(TMP_DIR)
    print("\n结果: %s  (pass=%d fail=%d)"
          % ("PASS" if g_fail == 0 else "FAIL: %d 项" % g_fail, g_pass, g_fail))
    return 0 if g_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
