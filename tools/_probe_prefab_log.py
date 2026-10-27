"""诊断探针：编辑器里 `prefab.create` / `prefab.instantiate` 之后，
`PREFAB-OVERRIDE` / `PREFAB-INFO` / 任何 WARN 到底打了什么。

只做一件事：起编辑器、跑同一条命令串、把相关日志**原样**打出来。
用来定位"引擎自检里记账成功、编辑器里记账丢失"这种跨层问题。
"""

import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import launcher_kit as kit                                        # noqa: E402
from capture_window import find_window                            # noqa: E402
import verify_prefab as V                                         # noqa: E402

ROOT = V.ROOT
BIN = V.BIN
EDITOR = V.EDITOR
OUT = os.path.join(os.path.dirname(ROOT), "_venn_verify", "g18probe")


def main():
    work = kit.fresh_dir(OUT)
    mode = sys.argv[1] if len(sys.argv) > 1 else "create"
    if mode == "gizmo":
        V.write_prefab(os.path.join(V.PREFAB_DIR, "vtest_c.vprefab.json"),
                       scale=1.0)
        scene_path = V.scene_two_instances(
            os.path.join(work, "scene_g.json"), V.REL_C, with_overrides=False)
        env = kit.clean_env({
            "MYVK_EDITOR_SCENE": scene_path,
            "MYVK_LOG_RECTS": "1",
            "MYVK_NO_WINDOW_SAVE": "1",
            "MYVK_EDITOR_SELECT": "Crate",
            "MYVK_EDITOR_GIZMO": "move",
            "MYVK_VALIDATION": "0",
        })
    else:
        scene_path = V.scene_b(os.path.join(work, "scene.json"))
        for stale in ("vtest_a.vprefab.json", "vtest_a_2.vprefab.json"):
            p = os.path.join(V.PREFAB_DIR, stale)
            if os.path.exists(p):
                os.remove(p)
        env = kit.clean_env({
            "MYVK_EDITOR_SCENE": scene_path,
            "MYVK_NO_WINDOW_SAVE": "1",
            "MYVK_EDITOR_SELECT": "Crate",
            "MYVK_EDITOR_CONSOLE_CMD":
                "prefab.create vtest_a;prefab.info;"
                "prefab.instantiate %s 3 0 0;prefab.info" % V.REL_A,
        })
    proc = subprocess.Popen([EDITOR], cwd=BIN, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = kit.LogTap()
    tap.start(proc)
    hwnd = find_window(kit.EDITOR_TITLE, 45.0)
    if not hwnd:
        print("没有窗口")
        return 2
    try:
        end = time.time() + 40.0
        if mode == "gizmo":
            while time.time() < end and not tap.last_rect("GZ-ORIGIN"):
                time.sleep(0.2)
            time.sleep(2.0)
        else:
            while time.time() < end and tap.count_line("PREFAB-INFO ok=1") < 2:
                time.sleep(0.2)
            time.sleep(1.0)
        with tap.lock:
            lines = list(tap.lines)
    finally:
        kit.shutdown(proc, hwnd)

    print("---- 相关日志 ----")
    for line in lines:
        if ("PREFAB" in line or "GZ-" in line or "gizmo" in line
                or "WARN" in line or "ERROR" in line or "auto-select" in line
                or "SCENE" in line):
            print(" ", line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
