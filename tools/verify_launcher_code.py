#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/verify_launcher_code.py —— 启动器 M5-b 验收：外置代码根（目录联接）

路线图 §7-M5-b：**T1 外置代码根（junction）+ 编辑器侧一致性校验**。
验收条款原文：「外置后改一行 `.vks` → 编辑器热重载生效（`VKS-RELOAD-*`）」。

这条链路有三个环节，每个都可能静默坏掉：

  ① 启动器真的建了**目录联接**（而不是"复制一份过去"—— 那样用户在外部
     目录里改代码，工程里那份不会变，而且两边都不会报错）；
  ② 编辑器的脚本扫描**穿过联接**看到了那些 `.vks`（引擎一行没改，靠的是
     `resolveAssetPath` 与文件系统的透明性 —— 但这只在联接真的存在时成立）；
  ③ 改外部文件 → 热重载真的触发。

覆盖（D1~D9）：
  D1  勾上「把脚本文件夹放到工程外」这一下被吃到（日志 `code-external=1`）
  D2  `venn.project.json` 记了 `code.linked=true` 且 `code.root` = 外部目录
  D3  `assets/scripts` 是**联接**：在外部目录写一个标记文件，透过工程里的
      路径能读到（行为断言，不靠 `os.path.islink` —— 那个对 junction 在
      不同 Python 版本上行为不一致）
  D4  外部目录里被种进了模板自带的 `main.vks`
  D5  编辑器日志 `PROJ-CODE-LINK ok`（一致性校验通过）
  D6  `precompiled N script(s), 0 with errors`（场景引用的脚本**穿过联接**
      被找到了 —— 模板现在只把 `scripts/main.vks` 挂在 GameStart 上，所以
      N 恒为 1：预编译跟着场景引用走，没挂载的脚本不编译）
  D7  改外部 `main.vks` → 日志出现 `VKS-RELOAD-`（**在 Play 态下**，
      编辑态脚本不 tick，见该段注释）
  D8  工程里那份 `main.vks` 内容跟着变了（说明是**同一份文件**，不是副本）
  D9  编辑器正常退出

⚠ D3 是这一条的关键：junction 与"复制目录"在 UI 上**看不出区别**，
  只有"在 A 处写、B 处能读到"能分开它们。

用法：
    python tools/verify_launcher_code.py
"""

import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import launcher_kit as kit  # noqa: E402

MARKER = "ext_code_marker.txt"
MARKER_TEXT = "written through the junction"


def main():
    if not os.path.isfile(kit.LAUNCHER) or not os.path.isfile(kit.EDITOR):
        kit.log("结果: FAIL: Launcher.exe / Editor.exe 不存在（先 make build）")
        return 1

    work = os.path.join(kit.WORK_ROOT, "launcher_code")
    work = kit.fresh_dir(work)
    os.makedirs(work, exist_ok=True)
    ext = os.path.join(work, "ExtCode")
    name = "LinkedGame"
    root = os.path.join(work, name)
    kit.log("PROBE work=%s ext=%s" % (work, ext))

    fails = []
    proc, tap, hwnd = kit.launch(
        recent_path=os.path.join(work, "recent.json"),
        env_extra={
            "MYVK_LAUNCHER_PAGE": "new",
            "MYVK_LAUNCHER_TEST_ROOT": work,
            "MYVK_LAUNCHER_TEST_NAME": name,
            "MYVK_LAUNCHER_TEST_CODE_EXTERNAL": ext,
        })
    if not hwnd:
        proc.kill()
        kit.log("结果: FAIL: 启动器窗口没出现")
        return 1

    editor_hwnd = 0
    try:
        # ---- 推进到 Step2 ----
        if not tap.wait_rect("LAUNCH-TYPE-1", timeout=25.0):
            fails.append("向导 Step1 没出现")
            return kit.finish(fails)
        kit.click_anchor(hwnd, tap, "LAUNCH-TYPE-1")
        kit.click_anchor(hwnd, tap, "LAUNCH-BTN-next")
        if not tap.wait_rect("LAUNCH-INPUT-root", timeout=20.0):
            fails.append("没有进入 Step2")
            return kit.finish(fails)

        # ---- D1 真点复选框 ----
        if not kit.click_anchor(hwnd, tap, "LAUNCH-CHK-external"):
            fails.append("D1 点不到「把脚本文件夹放到工程外」复选框")
            return kit.finish(fails)
        if not tap.wait_line("LAUNCHER-WIZARD code-external=1", timeout=8.0):
            fails.append("D1 勾选没有生效（日志里没有 code-external=1）")
        else:
            kit.log("CHECK D1 external checkbox toggled OK")

        # ---- 继续到 Step3 并创建 ----
        if not kit.click_anchor(hwnd, tap, "LAUNCH-BTN-next"):
            fails.append("点不到 Step2 的下一步")
            return kit.finish(fails)
        if not tap.wait_rect("LAUNCH-COMBO-res", timeout=20.0):
            fails.append("没有进入 Step3")
            return kit.finish(fails)
        if not kit.click_anchor(hwnd, tap, "LAUNCH-BTN-create"):
            fails.append("点不到创建")
            return kit.finish(fails)

        # ---- 创建成功 → **自动**进编辑器（2026-10-22 需求 A）----
        # 以前这里还要点 `LAUNCH-BTN-doneOpen`；现在 `commitWizard()` 成功
        # 就直接 requestSpawn 了。⚠ 不点不代表不测 —— `LAUNCHER-SPAWN ok`
        # 本身就是断言（没起来就 red），后面的 D5/D6 全部依赖它。
        if not tap.wait_line("LAUNCHER-SPAWN ok", timeout=45.0):
            fails.append("创建之后没有自动拉起编辑器（没有 LAUNCHER-SPAWN ok）")
            return kit.finish(fails)
        pid = kit.parse_spawn_pid(tap.text())
        if not pid:
            fails.append("日志里没有 LAUNCHER-SPAWN 的 pid，无法断言退出码")

        # ---- D2 工程文件 ----
        with open(os.path.join(root, "venn.project.json"), encoding="utf-8") as f:
            p = json.load(f)
        code = p.get("code", {})
        if not code.get("linked"):
            fails.append("D2 code.linked=%r，期望 true" % code.get("linked"))
        elif kit.project_key(code.get("root", "")) != kit.project_key(ext):
            fails.append("D2 code.root=%r，期望 %r" % (code.get("root"), ext))
        else:
            kit.log("CHECK D2 code.linked=true root=%s OK" % code.get("root"))

        # ---- D3 行为断言：联接 vs 副本 ----
        scripts_dir = os.path.join(root, "assets", "scripts")
        marker_in_ext = os.path.join(ext, MARKER)
        marker_via_link = os.path.join(scripts_dir, MARKER)
        with open(marker_in_ext, "w", encoding="utf-8", newline="\n") as f:
            f.write(MARKER_TEXT + "\n")
        if not os.path.isfile(marker_via_link):
            fails.append("D3 在外部目录写的文件透过 assets/scripts 读不到"
                         "（不是联接，或者指向别处）")
        else:
            with open(marker_via_link, encoding="utf-8") as f:
                got = f.read().strip()
            if got != MARKER_TEXT:
                fails.append("D3 透过联接读到 %r，期望 %r" % (got, MARKER_TEXT))
            else:
                kit.log("CHECK D3 assets/scripts is a real junction OK")

        # ---- D4 外部目录里被种进了模板脚本 ----
        ext_vks = os.path.join(ext, "main.vks")
        if not os.path.isfile(ext_vks):
            fails.append("D4 外部目录里没有被种进 main.vks")
        else:
            kit.log("CHECK D4 main.vks seeded into external dir OK")

        # ---- D5~D8 打开编辑器 ----
        editor_raw = os.path.join(scripts_dir, "main.vks")
        if not os.path.isfile(editor_raw):
            fails.append("D4 工程里看不到 main.vks（联接没生效）")
            return kit.finish(fails)

        # 契约反转（2026-10-19）：拉起之后启动器**不退出**，收进任务栏常驻。
        # 这里以前等它自己退出，现在只等"最小化"这一行 —— 再等退出只会白等 40s。
        if not tap.wait_line("LAUNCHER-MINIMIZED", timeout=30.0):
            fails.append("拉起后启动器没有最小化（LAUNCHER-MINIMIZED 没出现）")
        elif proc.poll() is not None:
            fails.append("拉起后启动器退出了（应常驻任务栏）")

        editor_hwnd, title = kit.wait_title(kit.EDITOR_TITLE, timeout=90.0)
        if not editor_hwnd:
            fails.append("等不到 Venn Editor 窗口")
            kit.log("\n".join(tap.text().splitlines()[-25:]))
            return kit.finish(fails)
        kit.log("CHECK editor title=%r" % title)

        # ---- D5 一致性校验通过 ----
        if not tap.wait_line("PROJ-CODE-LINK ok", timeout=25.0):
            # 也可能压根没打（检查被跳过）—— 两种情况都要红
            if tap.has_line("PROJ-CODE-LINK"):
                line = next(l for l in tap.text().splitlines()
                            if "PROJ-CODE-LINK" in l)
                fails.append("D5 一致性校验报警: %s" % line.split("] ", 1)[-1])
            else:
                fails.append("D5 没有 PROJ-CODE-LINK 日志（校验没跑）")
        else:
            kit.log("CHECK D5 PROJ-CODE-LINK ok OK")

        # ---- D6 脚本穿过联接被编译 ----
        #
        # 契约只有两条：**至少有一个脚本被找到**（证明扫描穿过了联接）+ `0
        # with errors`。数量**不写死** —— 它是模板内容，写死就等于"模板改
        # 一个脚本就得改验收脚本"。
        #
        # ⚠⚠ 2026-10-22 把阈值从 `>= 2` 改成 `>= 1`：第八轮（2026-10-21）
        #   用户要求"初始场景里不摆放任何物体"，场上的光球 / 传送门全撤了，
        #   于是场景里只剩 GameStart 上挂的 `scripts/main.vks` 一处引用。
        #   引擎的预编译是**跟着场景引用走**的（没被任何实体挂载的脚本不会
        #   编译），所以从那时起这里恒为 1 —— 不是"联接坏了"，旧断言的
        #   前提（"模板场景挂 2 个脚本"）早就不成立了。
        #   `pickup.vks` / `portal.vks` 仍然在模板里（`template.json` 的
        #   files[] 还列着），只是暂时没有实体引用它们。
        m = None
        deadline = time.time() + 30.0
        while time.time() < deadline:
            m = re.search(r"precompiled (\d+) script\(s\), (\d+) with errors",
                          tap.text())
            if m:
                break
            time.sleep(0.2)
        if m is None:
            lines = [l for l in tap.text().splitlines() if "precompiled" in l]
            fails.append("D6 没有 precompiled 行（实际: %s）"
                         % (lines[-1].split("] ", 1)[-1] if lines else "无该行"))
        elif m.group(2) != "0":
            fails.append("D6 有脚本编译失败（%s 个），模板脚本语法有问题"
                         % m.group(2))
        elif int(m.group(1)) < 1:
            fails.append("D6 一个脚本都没编译到（扫描没穿过联接？）")
        else:
            kit.log("CHECK D6 precompiled %s script(s), 0 with errors "
                    "(via junction) OK" % m.group(1))

        # ---- D7/D8 改外部文件 → 热重载 ----
        #
        # ⚠⚠ **必须先按 Space 进 Play**，否则这一条永远红 —— 不是联接坏了。
        #   原因在 `EditorApp::update`：
        #       if (m_ctx->isEditing() && m_ctx->animPlaying()) { runAnimScripts(..); }
        #   编辑态只有"动画预览在播"时才 tick 脚本，默认不播 → 实例表里
        #   `inst.progVersion` 一直是 0 → `ScriptRuntime::tick` 里那条
        #   `if (inst.progVersion != 0) { …VKS-RELOAD-*… }` 根本进不去。
        #   热重载**只对"跑过一次的实例"有对账意义**（变量保不保、挂在哪条
        #   语句上），所以 Play 态才是它的自然归属。
        kit.press_space(editor_hwnd)
        if not tap.wait_line("Runtime scripts:", timeout=20.0):
            fails.append("D7 按 Space 之后没进 Play（没有 `Runtime scripts:` "
                         "汇总行）—— 热重载在编辑态不可观测")
        else:
            kit.log("CHECK D7a 进入 Play（运行态脚本已 tick）OK")

        # 加一行 `let` 声明（S1 是幂等声明，加在末尾不会破坏语法）
        with open(ext_vks, "a", encoding="utf-8", newline="\n") as f:
            f.write("\nlet hot_token = 4242\n")
        if not tap.wait_line("VKS-RELOAD", timeout=30.0):
            fails.append("D7 改外部 .vks 之后没有 VKS-RELOAD（热重载没触发）")
        else:
            line = next(l for l in tap.text().splitlines() if "VKS-RELOAD" in l)
            kit.log("CHECK D7 %s OK" % line.split("] ", 1)[-1])

        # 停回编辑态再收尾：Play 态直接关窗也能退，但留着会让 D9 的
        # "正常退出"混进"运行态资源清理"的变量，少一个可怀疑的环节
        kit.press_space(editor_hwnd)
        time.sleep(0.6)

        # 工程里那份必须跟着变 —— 这是"同一份文件"的直接证据
        with open(editor_raw, encoding="utf-8") as f:
            body = f.read()
        if "hot_token" not in body:
            fails.append("D8 工程里的 main.vks 没有跟着变（那是副本不是联接）")
        else:
            kit.log("CHECK D8 project-side main.vks mirrors the external file OK")

        # ---- D9 正常退出 ----
        kit.capture(editor_hwnd, os.path.join(work, "editor_linked.png"))
        kit.close_window(editor_hwnd)
        rc = kit.wait_pid_exit(pid, timeout_ms=90000) if pid else None
        if rc is None:
            fails.append("D9 编辑器没有正常退出")
        elif rc != 0:
            fails.append("D9 编辑器退出码 rc=%d，期望 0" % rc)
        else:
            kit.log("CHECK D9 editor exit rc=0 OK")

        with open(os.path.join(work, "code.log"), "w", encoding="utf-8") as f:
            f.write(tap.text())

    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=10)
        if editor_hwnd:
            kit.close_window(editor_hwnd)
            time.sleep(1.0)

    return kit.finish(fails)


if __name__ == "__main__":
    sys.exit(main())
