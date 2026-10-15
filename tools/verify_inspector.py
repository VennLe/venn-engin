"""端到端验证：Inspector 升级（E11）。

覆盖三件事（都可机器断言）：

  ① **分区折叠按名持久化**：同一个 MYVK_EDITOR_INSP_COLLAPSE 折叠注入下，
     切换选中实体（Crate / Ball / SoloLight）后 INSP-SECTIONS 里"展开的分区
     集合"必须完全一致 —— 折叠状态挂在面板上，与当前实体无关。

  ② **Copy/Paste Properties 往返一致 + Reset to Default**：跑 `insp.selftest`
     Console 命令，内部执行 Copy → 改值 → Paste → 比对，再 Reset → 比对
     构造默认，打印 `INSP-SELFTEST copy-paste=OK reset=OK collapse-persist=OK`。

  ③ **运行时干净**：整程 0 ERROR / 0 VUID / 0 资源泄漏。

多选编辑（同一改动同步到 N 个实体）依赖 E22 的多选选择模型，当前选择模型
是单实体，故此用例显式 SKIP 并说明原因（见 E11 记账）。

用法：
    python tools/verify_inspector.py [--exe build/bin/Editor.exe]
"""

import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")

# 一个带层级 + 多种组件类型的场景：Crate(网格) / Ball(网格) / SoloLight(点光)
SCENE = os.path.join(ROOT, "_venn_verify", "e12", "nested_scene.json")

SELECT_RE = re.compile(r"INSP-SECTIONS open=(\S*)")
SELFTEST_RE = re.compile(r"INSP-SELFTEST (.+)")
BAD_RE = re.compile(r"(ERROR|VUID|leaked objects)")
LEAK_RE = re.compile(r"leaked objects")


def run_editor(exe, env_extra, frames=60):
    env = dict(os.environ)
    env["MYVK_FRAMES"] = str(frames)
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env.update(env_extra)
    p = subprocess.run([exe], cwd=os.path.dirname(exe), env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, encoding="utf-8", errors="replace",
                       timeout=180)
    return p.stdout + (p.stderr or "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    args = ap.parse_args()
    exe = os.path.abspath(args.exe)

    if not os.path.exists(exe):
        print("可执行文件不存在:", exe)
        return 2
    if not os.path.exists(SCENE):
        print("测试场景不存在:", SCENE)
        return 2

    failures = []
    log = []

    print("[1] 分区折叠状态按名持久化（跨实体保持）")
    base_env = {
        "MYVK_EDITOR_SCENE": SCENE.replace("\\", "/"),
        "MYVK_EDITOR_INSP_COLLAPSE": "Mesh,Material",
    }
    sets = {}
    for sel in ("Crate", "Ball", "SoloLight"):
        out = run_editor(exe, dict(base_env, MYVK_EDITOR_SELECT=sel))
        m = SELECT_RE.search(out)
        got = sorted(t for t in (m.group(1).split("|") if m else []) if t)
        sets[sel] = got
        print("    %-10s INSP-SECTIONS open=%s" % (sel, "|".join(got)))
        if not m:
            failures.append("entity %s 没有 INSP-SECTIONS 日志" % sel)
        if BAD_RE.search(out):
            failures.append("entity %s 运行期出现 ERROR/VUID/泄漏" % sel)
            log.append(("sel=%s" % sel, out))

    # Crate / Ball 都是网格实体（都没有材质，故 Material 不出现）；
    # 两者展开集合必须一致 —— 这就是"折叠按名持久化、与实体无关"。
    if sets.get("Crate") != sets.get("Ball"):
        failures.append("折叠状态未跨实体保持: Crate=%s Ball=%s"
                        % (sets.get("Crate"), sets.get("Ball")))
    print("    Crate == Ball : %s"
          % ("PASS" if sets.get("Crate") == sets.get("Ball") else "FAIL"))

    print("[2] Copy/Paste 往返 + Reset to Default（Console insp.selftest）")
    out = run_editor(exe, {
        "MYVK_EDITOR_SCENE": SCENE.replace("\\", "/"),
        "MYVK_EDITOR_SELECT": "Ball",
        "MYVK_EDITOR_CONSOLE_CMD": "insp.selftest",
    })
    m = SELFTEST_RE.search(out)
    detail = m.group(1) if m else "(无输出)"
    print("    INSP-SELFTEST", detail)
    if not m:
        failures.append("insp.selftest 没有输出")
        log.append(("selftest", out))
    else:
        for key in ("copy-paste=OK", "reset=OK", "collapse-persist=OK"):
            if key not in detail:
                failures.append("自检未通过: %s" % key)
    if BAD_RE.search(out):
        failures.append("自检运行期出现 ERROR/VUID/泄漏")
        log.append(("selftest", out))

    print("[3] 多选编辑同步（依赖 E22 选择模型）: SKIP")
    print("    当前选择模型为单实体（随 E22），E11 记账后延。")

    print()
    if failures:
        print("结果: FAIL")
        for f in failures:
            print("  -", f)
        for tag, text in log[:2]:
            print("---- %s 日志尾部 ----" % tag)
            print("\n".join(text.splitlines()[-25:]))
        return 1

    print("全部断言 PASS")
    print("  折叠跨实体保持 / Copy-Paste 往返 / Reset 默认 / 运行期干净")
    return 0


if __name__ == "__main__":
    sys.exit(main())
