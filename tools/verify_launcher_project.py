#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/verify_launcher_project.py —— 启动器 M0 验收：工程能被"手动"打开

大路线图 §7-M0 的三条验收（原文写的是"手工"）：

  ① 场景里有 Ground + Player，Hierarchy 能看到两个物体；
  ② 视口里相机位置符合类型（FP 在眼位 / TP 在斜后方）；
  ③ 关掉后 `config/editor_layout.json` 出现在**工程目录**（不是仓库根）。

这三条里 ③ 是最关键的 —— 它同时验证 D5（CWD = 工程根）与 P2
（`config/*` 全相对 CWD）。**不成立就停下改文档，别做 UI。**

这个脚本做的是"替代手工"，但**不设任何自动化帧控**：
`editorLayoutAutomationRun()` 看到 `MYVK_FRAMES` / `MYVK_NO_WINDOW_SAVE`
就一律拒写布局存档 —— 一旦设了，③ 就永远不可能通过（假绿）。

流程：
  1. 把 `templates/<type>/` 拷到临时工程目录（模拟"创建"）
  2. 手写 `venn.project.json`（M2 之前创建引擎还不存在）
  3. 启动 `Editor.exe`，**cwd = 工程根**，只给
     `MYVK_ASSET_DIR` / `MYVK_EDITOR_SCENE` / `MYVK_LOG_RECTS`
  4. 等窗口出现 → 读 `Editor: auto-opened scene ... (N entities)`
  5. `WM_CLOSE` 正常关窗（不是强杀 —— 强杀不会走 onShutdown，布局不会写）
  6. 断言 `config/editor_layout.json` 在**工程目录**里、且**不在仓库根**

用法：
    python tools/verify_launcher_project.py
    python tools/verify_launcher_project.py --type third_person --keep
"""

import argparse
import ctypes
import json
import os
import shutil
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(REPO, "build", "bin")
EDITOR = os.path.join(BIN, "Editor.exe")
TEMPLATES = os.path.join(REPO, "templates")
# 与既有 verify_* 脚本同一个输出根（记忆：截图/临时产物都在 <repo>/../_venn_verify）
WORK_ROOT = os.path.join(os.path.dirname(REPO), "_venn_verify")

WM_CLOSE = 0x0010

sys.path.insert(0, os.path.join(REPO, "tools"))
from capture_window import find_window  # noqa: E402
from verify_add_menu import LogTap      # noqa: E402

# ⚠⚠ ctypes 默认把参数当 **32 位 int**：64 位进程的 HWND 高 32 位会被截断，
#   消息发到一个不存在的窗口，PostMessageW 静默返回 0 —— 现象是"发了关窗
#   消息，进程照常跑，布局一条都没写"。必须显式声明 argtypes。
_user32 = ctypes.windll.user32
_user32.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint,
                                 ctypes.c_size_t, ctypes.c_ssize_t]
_user32.PostMessageW.restype = ctypes.c_bool
_user32.IsWindow.argtypes = [ctypes.c_void_p]
_user32.IsWindow.restype = ctypes.c_bool


def log(msg):
    print(msg, flush=True)


def close_window(hwnd):
    """正常关闭（GLFW 会把它翻成 glfwWindowShouldClose）。

    返回是否**送达**。⚠ 不检查返回值的话，"发错窗口"和"发对了但窗口不理"
    看起来一模一样，而前者是本项目已经踩过的坑。
    """
    if not _user32.IsWindow(ctypes.c_void_p(hwnd)):
        return False
    return bool(_user32.PostMessageW(ctypes.c_void_p(hwnd), WM_CLOSE, 0, 0))


def make_project(dest, kind):
    """拷模板 + 写 venn.project.json。M2 的创建引擎出现之前，这就是它。"""
    if os.path.isdir(dest):
        # ⚠ 不删，改名让位：宿主有**批量删除保护**（按回合累计 50 个文件，
        #   超了直接杀掉整个脚本，`ignore_errors` 接不住）。见 launcher_kit.fresh_dir。
        os.rename(dest, "%s.old-%s" % (dest, time.strftime("%H%M%S")))
    os.makedirs(dest, exist_ok=True)
    shutil.copytree(os.path.join(TEMPLATES, kind),
                    dest, dirs_exist_ok=True)

    # player.mode 的权威在场景里；venn.project.json 的 gameType 只做展示，
    # 但两者必须一致 —— 否则加载时会打一条 PROJ-WARN，验收脚本会看见它。
    proj = {
        "format": 1,
        "name": os.path.basename(dest),
        "template": kind,
        "gameType": kind,
        "startScene": "scenes/main.json",
        "code": {"mode": "vks", "root": "scripts", "linked": False},
        "createdAt": "2026-10-19T00:00:00Z",
        "engine": {"name": "venn", "config": "Debug"},
    }
    with open(os.path.join(dest, "venn.project.json"), "w",
              encoding="utf-8", newline="\n") as f:
        json.dump(proj, f, ensure_ascii=False, indent=2)
        f.write("\n")
    return proj


def run_editor(root, timeout=45.0):
    """启动编辑器，返回 (stdout 全文, 进程返回码)。"""
    env = dict(os.environ)
    # ⚠ 先清掉可能从父 shell 继承来的自动化变量 —— 它们会让 ③ 永远假红。
    for k in ("MYVK_FRAMES", "MYVK_NO_WINDOW_SAVE", "MYVK_EDITOR_LAYOUT",
              "MYVK_EDITOR_PROJECT", "MYVK_EDITOR_PLAY", "MYVK_HIDE_UI",
              "MYVK_EDITOR_NO_HUD"):
        env.pop(k, None)
    env["MYVK_ASSET_DIR"] = os.path.join(root, "assets")
    env["MYVK_EDITOR_SCENE"] = "scenes/main.json"
    env["MYVK_LOG_RECTS"] = "1"

    proc = subprocess.Popen(
        [EDITOR], cwd=root, env=env,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, encoding="utf-8", errors="replace", bufsize=1)

    # ⚠⚠ **必须持续排空 stdout**。`Popen(stdout=PIPE)` 而主线程不读的话，
    #   子进程的日志写满管道缓冲区（Windows 上 64 KB）就会**阻塞在 write()**，
    #   主循环整个停住 —— 现象是"窗口一直不出现、日志停在某一行的中间、
    #   进程还活着"。实测就这么被卡过（`MYVK_LOG_RECTS=1` 的矩形日志很快
    #   就能写满）。`LogTap` 在后台线程里逐行取走，和既有 verify_* 同一套。
    tap = LogTap()
    tap.start(proc)

    # ⚠ 自己写"等窗口"的循环，而不是直接 `find_window(timeout=45)`：
    #   后者在**子进程已经崩了**的情况下会干等满 45 秒，然后被 kill，
    #   最后只看到 "rc=-1 + 没有布局文件"—— 与"CWD 契约不成立"长得一模一样。
    #   实测就是这么误判过一次。现在进程一退出就立刻返回它的真实退出码。
    hwnd = 0
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            log("CHECK WARN 编辑器在窗口出现前就退出了 rc=%d" % proc.returncode)
            return "\n".join(tap.lines), proc.returncode
        hwnd = find_window("Venn Editor", timeout=0.6)
        if hwnd:
            break
    if not hwnd:
        proc.kill()
        proc.wait(timeout=10)
        return "\n".join(tap.lines), -1

    # 让场景加载 + 第一帧布局稳定下来，再关窗（布局是 onShutdown 写的，
    # 但 iframe 的矩形要等 SplitLayout 算过一轮才是最终值）。
    time.sleep(2.5)
    delivered = close_window(hwnd)
    if not delivered:
        log("CHECK WARN WM_CLOSE 没送达（hwnd=0x%X）" % hwnd)
    try:
        proc.wait(timeout=60)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=10)
    return "\n".join(tap.lines), proc.returncode


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--type", default="first_person",
                    choices=["first_person", "third_person"])
    ap.add_argument("--keep", action="store_true", help="保留临时工程目录")
    args = ap.parse_args()

    if not os.path.isfile(EDITOR):
        log("结果: FAIL: Editor.exe 不存在（先 make build）")
        return 1

    os.makedirs(WORK_ROOT, exist_ok=True)
    root = os.path.join(WORK_ROOT, "proj_m0_" + args.type)
    proj = make_project(root, args.type)
    log("PROBE project=%s type=%s" % (root, args.type))

    # 先读**输入文件**（模板产物），后跑编辑器 —— 断言要拿"输入"做期望，
    # 而不是拿"输出"反推期望（那是自证）。
    scene_path = os.path.join(root, "assets", "scenes", "main.json")
    with open(scene_path, encoding="utf-8") as f:
        scene = json.load(f)
    template_entities = len(scene.get("entities", []))

    out, rc = run_editor(root)
    with open(os.path.join(WORK_ROOT, "proj_m0_%s.log" % args.type), "w",
              encoding="utf-8") as f:
        f.write(out)

    fails = []

    # ---- ① 场景被打开，模板里的实体一个不少 ----
    # ⚠ 期望是 `模板实体数 + 1`：`Scene::lightEntity()` 会被场景的 `light`
    #   块懒创建成一个**实体**，而它不在 `entities[]` 里。所以
    #   `objectCount()` = Ground + Player + Sun = 3。
    #   写成"≥2"会变成永远为真的假绿 —— 这里刻意写死等号。
    n_entities = None
    for line in out.splitlines():
        if "auto-opened scene" in line and "entities)" in line:
            try:
                n_entities = int(line.rsplit("(", 1)[1].split(" ")[0])
            except (IndexError, ValueError):
                pass
    want_entities = template_entities + 1
    if n_entities is None:
        fails.append("① 没有 'Editor: auto-opened scene ... (N entities)' 行")
    elif n_entities != want_entities:
        fails.append("① 实体数 %s，期望 %s（模板 %s + 场景太阳 1）"
                     % (n_entities, want_entities, template_entities))
    else:
        log("CHECK ① entities=%s (模板 %s + 太阳) OK"
            % (n_entities, template_entities))

    # ---- ② 相机符合类型 ----
    # ⚠ 断言的是**模板产物**里的 camera.distance 与 player.mode，
    #   而不是运行期相机位置 —— 后者在 edit 态是导航相机（用户随时会动），
    #   拿它做断言等于"用实现验证实现"。
    want_dist = 8.0 if args.type == "first_person" else 4.5
    got_dist = scene.get("camera", {}).get("distance")
    if abs(float(got_dist or -1) - want_dist) > 1e-6:
        fails.append("② camera.distance=%s，期望 %s" % (got_dist, want_dist))
    else:
        log("CHECK ② camera.distance=%s OK" % got_dist)

    # 类型标记（first / third）的权威。
    #
    # ⚠ 2026-10-29 改判据：以前读 `Player` 实体的 `player.mode`，可模板早就
    #   不是"预放一个 Player 实体"了 —— 现在是 **Terrain + GameStart（出生点）**
    #   （见 gen_templates.py 的场景注释："空场景：地形 + 出生点"），玩家实体
    #   在 Play 时才由 GameStartComponent 生成。所以那段断言一直在找一个
    #   不存在的实体（`player.mode=None`），红着但没人发现。
    #   新的落点：`GameStart` 实体的 `gameStart.mode` —— 同一份类型信息，
    #   只是跟着模板一起搬了家（`player.mode` 的替代者，仍是**场景内的**
    #   权威，`venn.project.json` 的 gameType 依旧只做展示）。
    start = next((e for e in scene.get("entities", [])
                  if e.get("name") == "GameStart"), None)
    want_mode = "first" if args.type == "first_person" else "third"
    got_mode = (start or {}).get("gameStart", {}).get("mode")
    if got_mode != want_mode:
        fails.append("② gameStart.mode=%s，期望 %s" % (got_mode, want_mode))
    else:
        log("CHECK ② gameStart.mode=%s OK" % got_mode)

    # ---- ③ 布局落在工程目录，而不是仓库根 ----
    layout = os.path.join(root, "config", "editor_layout.json")
    repo_layout = os.path.join(REPO, "config", "editor_layout.json")
    if not os.path.isfile(layout):
        fails.append("③ 工程目录里没有 config/editor_layout.json"
                     "（CWD 契约不成立，见路线图 M0 止损条件）")
    else:
        log("CHECK ③ %s OK" % layout)
        # 否定测试：仓库根那份**必须没被这次运行写过**。
        # 只看"工程目录有"是不够的 —— 万一两处都写了，D5 就是假的。
        if os.path.isfile(repo_layout):
            t_proj = os.path.getmtime(layout)
            t_repo = os.path.getmtime(repo_layout)
            # 仓库根那份可能是很早以前手跑留下的；只在**它比工程那份更新**
            # 时才判定"这次写错地方了"。
            if t_repo >= t_proj:
                fails.append("③ 仓库根的 config/editor_layout.json 比工程的更新"
                             "（CWD 没生效）")

    if not args.keep:
        # ⚠ 不删：宿主有批量删除保护（按回合累计 50 个文件），超了会**杀掉整个
        #   脚本**（`ignore_errors=True` 接不住，它不在 Python 层）。旧工程留在
        #   `_venn_verify` 下不影响正确性 —— 每次都用新目录。
        log("NOTE 工作目录保留：%s" % root)

    # ⚠ returncode 必须**在**失败分支之前打：`onShutdown` 没跑（= 进程被
    #   kill）与"跑了但拒绝写盘"是两种完全不同的原因，只看到"没有布局文件"
    #   会误判成 CWD 契约不成立（那会导致按止损条件推翻整个方案）。
    log("CHECK editor-exit rc=%s (0/正常退出 才说明 onShutdown 跑过)" % rc)
    log("CHECK layout-save-log=%s" % ("有" if "LAYOUT-SAVE" in out else "无"))

    if fails:
        for f in fails:
            log("CHECK FAIL " + f)
        log("结果: FAIL: %d 项不达标" % len(fails))
        return 1
    log("结果: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
