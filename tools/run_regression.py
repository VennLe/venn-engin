"""跑一轮回归：逐个启动 GUI 验证脚本，记录退出码与结果行。

GUI 测试要独占窗口，只能串行。而且**必须以退出码为准** —— 这批脚本的
结果行格式不统一（`RESULT: PASS` / `全部断言 PASS` / `全部 PASS`），
只靠文本匹配会漏判。失败时把该脚本的日志尾部打出来，省得再跑一遍。

用法：
    python tools/run_regression.py                      # 跑默认全套
    python tools/run_regression.py bvh player add_menu  # 只跑指定的几个
    python tools/run_regression.py --tail 60 bvh        # 只看尾部行数
"""

import argparse
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PY = sys.executable
# 单测完整日志的落盘目录（和截图产物放一起）
LOG_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify", "_regression")

DEFAULT = [
    "verify_frustum_cull", "verify_bvh", "verify_lod", "verify_ibl",
    "verify_csm", "verify_local_shadow", "verify_shader_variants",
    "verify_shader_hotreload", "verify_fxaa", "verify_instancing",
    "verify_nav_gizmo",
    "verify_character_window", "verify_add_menu", "verify_mesh_params",
    "verify_collider_gizmo", "verify_player", "verify_collision_play",
    "verify_drop_to_floor", "verify_material_roundtrip",
    "verify_primitive_roundtrip", "verify_screenspace", "verify_textures",
]

# 结果行：兼容四种写法。取**最后一条**，因为有的脚本会先打一堆中间断言。
OK_RE = re.compile(r"(?:RESULT:\s*PASS|全部断言 PASS|^全部 PASS\b|结果:\s*PASS)")
BAD_RE = re.compile(r"(?:RESULT:\s*FAIL|断言[^\n]*FAIL|结果:\s*FAIL)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tests", nargs="*", help="只跑这些（可省 verify_ 前缀）")
    ap.add_argument("--tail", type=int, default=25,
                    help="失败时打印日志尾部行数（默认 25）")
    ap.add_argument("--settle", type=float, default=2.0,
                    help="两个用例之间额外等待的秒数（默认 2）")
    args = ap.parse_args()

    names = args.tests or DEFAULT
    names = [n if n.startswith("verify_") else "verify_" + n for n in names]

    rows = []
    for i, n in enumerate(names):
        path = os.path.join(ROOT, "tools", n + ".py")
        if not os.path.exists(path):
            rows.append((n, None, "脚本不存在", ""))
            print("%-26s %s" % (n, "脚本不存在"))
            continue
        if i:   # 让上一个编辑器进程彻底退场、前台焦点回稳
            time.sleep(args.settle)
        t0 = time.time()
        p = subprocess.run([PY, path], cwd=ROOT, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True,
                           encoding="utf-8", errors="replace")
        out = p.stdout or ""
        ok = False
        for line in out.splitlines():
            if BAD_RE.search(line):
                ok = False
            elif OK_RE.search(line):
                ok = True
        mark = "PASS" if (p.returncode == 0 and ok) else "FAIL"
        rows.append((n, p.returncode, mark, out))
        # 每个用例的**完整**输出都落盘。失败时只打尾部 N 行，光靠它没法定位
        # 问题（比如"手柄目标是碰撞体"挂了，但尾部看到的已经是几行之后的连锁
        # 失败，看不到它前一刻的起点断言）。日志留在 _venn_verify/_regression/。
        os.makedirs(LOG_DIR, exist_ok=True)
        with open(os.path.join(LOG_DIR, n + ".log"), "w",
                  encoding="utf-8") as fh:
            fh.write(out)
        print("%-26s %-4s rc=%-3d %5.1fs" % (n, mark, p.returncode,
                                             time.time() - t0))
        if mark == "FAIL":
            tail = out.splitlines()[-args.tail:]
            for line in tail:
                print("      | " + line)
            print("      * 完整日志: %s"
                  % os.path.join(LOG_DIR, n + ".log"))

    bad = [r for r in rows if r[2] != "PASS"]
    print("\n%d/%d 通过" % (len(rows) - len(bad), len(rows)))
    if bad:
        print("失败：" + ", ".join(r[0] for r in bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
