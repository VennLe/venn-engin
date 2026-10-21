#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/verify_template_play.py —— 模板自带的"基本游戏环境"真的跑得起来吗

启动器创建工作后，用户第一眼看到的是工程里的 `.vks`。这个脚本要证明三件事：

  P1  三个模板脚本都**编译通过**（`precompiled 3 script(s), 0 with errors`）
  P2  进 Play 之后 `on start` 真的执行了（`GAME-READY` / `GAME-MODE`）
  P3  `on update` 真的每帧在跑（`SCORE 0 / 5` 是 update 里第一条会触发
      的日志：`lastPicked` 初值 -1、黑板初值 0，所以第一帧必然打一次）
  P4  全程没有运行期脚本错误（`Script runtime error` 零条）

⚠ 这个脚本**不验证"创建流程"**（那是 verify_launcher_create.py /
  verify_launcher_project.py 的事），所以用 `kit.make_project` 直接铺一个
  工程——如果这里也走向导，一个失败要排查两件事。

⚠ 需要 `MYVK_LOG_SCRIPT_PRINT=1`：脚本的 `print` 默认只进编辑器面板，
  不落日志（用户脚本可能每帧打印）。这个开关在 `EditorApp::onInit` 里
  装的 printSink 上。

用法：
    python tools/verify_template_play.py                 # 默认第三人称
    python tools/verify_template_play.py --type first_person
"""

import argparse
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import launcher_kit as kit  # noqa: E402

WORK = os.path.join(kit.WORK_ROOT, "template_play")

# 与 `project::createFromTemplate` 的**白名单**一致（template.json.placeholders）。
# `kit.make_project` 只是 copytree，不替换占位符 —— 这里补上，因为本脚本要
# 断言 "GAME-READY 打出来的确实是工程名"。
# ⚠ 真正走 C++ 创建流程的验收在 verify_launcher_create.py（F6/F7），
#   这里不重复验证"创建"，只验证"模板脚本里的占位符位置对、替换后能跑"。
PLACEHOLDERS = ("__PROJECT_NAME__", "__TEMPLATE_ID__")


def substitute(root, name, kind):
    for dirpath, _dirs, files in os.walk(root):
        for fn in files:
            if not fn.endswith((".vks", ".json", ".md")):
                continue
            p = os.path.join(dirpath, fn)
            with open(p, encoding="utf-8", newline="") as f:
                text = f.read()
            out = text.replace("__PROJECT_NAME__", name).replace(
                "__TEMPLATE_ID__", kind)
            if out != text:
                with open(p, "w", encoding="utf-8", newline="\n") as f:
                    f.write(out)


def find_window(title, timeout=45.0):
    return kit.find_window(title, timeout=timeout)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--type", default="third_person",
                    choices=["first_person", "third_person"])
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    if not os.path.isfile(kit.EDITOR):
        kit.log("结果: FAIL: Editor.exe 不存在（先 make build）")
        return 1

    os.makedirs(kit.WORK_ROOT, exist_ok=True)
    root = os.path.join(WORK, args.type)
    name = "TemplatePlay"
    kit.make_project(root, args.type, name=name)
    substitute(root, name, args.type)
    kit.log("PROBE project=%s type=%s" % (root, args.type))

    env = kit.clean_env({
        "MYVK_ASSET_DIR": os.path.join(root, "assets"),
        "MYVK_EDITOR_SCENE": "scenes/main.json",
        "MYVK_LOG_SCRIPT_PRINT": "1",
    })
    proc = subprocess.Popen(
        [kit.EDITOR], cwd=root, env=env,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, encoding="utf-8", errors="replace", bufsize=1)
    tap = kit.LogTap()
    tap.start(proc)

    hwnd = 0
    deadline = time.time() + 45.0
    while time.time() < deadline:
        if proc.poll() is not None:
            kit.log("结果: FAIL: 编辑器在窗口出现前就退出了 rc=%d"
                    % proc.returncode)
            return 1
        hwnd = kit.find_window("Venn Editor", timeout=0.5)
        if hwnd:
            break
    if not hwnd:
        proc.kill()
        kit.log("结果: FAIL: 编辑器窗口没出现")
        return 1

    fails = []
    try:
        # ---- P1 三个脚本全部编译通过 ----
        m = None
        deadline = time.time() + 40.0
        while time.time() < deadline and m is None:
            m = re.search(r"precompiled (\d+) script\(s\), (\d+) with errors",
                          tap.text())
            if m is None:
                time.sleep(0.2)
        if m is None:
            fails.append("P1 没有 precompiled 行（场景没引用到模板脚本？）")
        elif m.group(2) != "0":
            fails.append("P1 模板脚本有 %s 个编译失败" % m.group(2))
        elif int(m.group(1)) != 1:
            # 2026-10-21 第八轮：模板场景改成"地形 + 出生点"，场上没有光球 /
            # 传送门了，所以只剩挂在 GameStart 上的 main.vks 一个脚本。
            # pickup.vks / portal.vks 仍随模板发布（示例），只是没被引用。
            fails.append("P1 编译了 %s 个脚本，空场景模板应只有 main.vks 一个"
                         % m.group(1))
        else:
            kit.log("CHECK P1 precompiled 1 script(s) (main.vks), 0 errors OK")

        # ---- P2/P3 进 Play，看 on start / on update 的日志 ----
        kit.press_space(hwnd)
        if not tap.wait_line("Runtime scripts:", timeout=25.0):
            fails.append("P2 按 Space 之后没进 Play（无 `Runtime scripts:`）")
        else:
            kit.log("CHECK P2 进入 Play（脚本开始 tick）OK")

        def need(pattern, name, timeout=25.0):
            end = time.time() + timeout
            while time.time() < end:
                hit = re.search(pattern, tap.text())
                if hit:
                    kit.log("CHECK %s %s OK" % (name, hit.group(0)))
                    return True
                time.sleep(0.2)
            fails.append("%s 没出现（%s）" % (name, pattern))
            return False

        need(r"VKS-PRINT scripts/main\.vks: GAME-READY " + name, "P2a on start")
        need(r"VKS-PRINT scripts/main\.vks: GAME-MODE " + args.type,
             "P2b GAME-MODE")
        # update 里的第一条（lastPicked=-1 与黑板 0 不等 → 必然打一次）
        need(r"VKS-PRINT scripts/main\.vks: SCORE 0 / 5", "P3 on update")

        # ---- P4 无运行期错误 ----
        errs = [l for l in tap.text().splitlines()
                if "Script runtime error" in l or "not runnable" in l]
        if errs:
            fails.append("P4 有 %d 条脚本运行期错误，首条: %s"
                         % (len(errs), errs[0].split("] ", 1)[-1]))
        else:
            kit.log("CHECK P4 无脚本运行期错误 OK")

    finally:
        try:
            kit.press_space(hwnd)
            time.sleep(0.4)
        except Exception:
            pass
        kit.capture(hwnd, os.path.join(WORK, "play_%s.png" % args.type))
        kit.close_window(hwnd)
        try:
            proc.wait(timeout=60)
        except subprocess.TimeoutExpired:
            proc.kill()

    rc = proc.returncode
    with open(os.path.join(WORK, "play_%s.log" % args.type), "w",
              encoding="utf-8") as f:
        f.write("\n".join(tap.lines))
    if rc != 0:
        fails.append("编辑器退出码 rc=%s，期望 0" % rc)
    else:
        kit.log("CHECK P5 editor exit rc=0 OK")

    if fails:
        for f in fails:
            kit.log("CHECK FAIL " + f)
        kit.log("结果: FAIL")
        return 1
    kit.log("结果: PASS")
    if args.keep:
        kit.log("NOTE 工作目录保留：%s" % root)
    return 0


if __name__ == "__main__":
    sys.exit(main())
