"""端到端验证：第一／第三人称的视角装配（`character/ViewRig`）。

和 `verify_joints.py` 同一条纪律：**判据要能分开"对"与"错"**。
这个模块的自检（`engine/character/ViewRigSelfTest.cpp`）全部是**手算
期望值**的纯数学断言 —— 不加载资产、不开窗口，所以可以在 CI 里跑：

    MYVK_VIEWRIG_TEST=1 build/bin/Editor.exe

它打印的每一行都是

    VIEWRIG-TEST <name> actual=<x> expected=<y> tol=<t> detail=<...> result=PASS|FAIL

脚本只做三件事：跑、逐行解析、把 FAIL 连同 actual/expected 打出来。
**不重算** 任何期望值 —— 重算一遍等于把同一份推理抄两遍，抄错的时候
两遍一起错（这正是这条纪律存在的理由）。

------------------------------------------------------------
为什么这些断言值得跑
------------------------------------------------------------
视角这块的失败形态全是**静默**的：程序不崩、日志不报，只是
    · 第一人称的相机落在"眼前两米"（`Camera::position` 的反解写反了）
    · 镜头绕目标点打转（用了 `look()` 的语义去设位置）
    · 换视角时镜头硬切（过渡没接上）或原地转一整圈（yaw 没走最短角差）
    · 第一人称藏了身体连影子一起没了（`castShadowHidden` 没接）
    · 60 fps 与 144 fps 手感不同（平滑用了 `min(1, speed*dt)`）
每一条在截图里都只是"看着有点怪"。所以这里判的是**数**。

用法：
    python tools/verify_view_rig.py            # 全绿退出 0
    python tools/verify_view_rig.py --min 60   # 断言条数下限（防"一项没跑"）
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")

LINE = re.compile(
    r"VIEWRIG-TEST\s+(\S+)\s+actual=(\S+)\s+expected=(\S+)\s+tol=(\S+)"
    r"\s+detail=(.*?)\s+result=(PASS|FAIL)")
TOTAL = re.compile(r"VIEWRIG-TOTAL pass=(\d+) fail=(\d+)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--min", type=int, default=50,
                    help="断言条数下限（默认 50；少于此数视为'根本没跑起来'）")
    ap.add_argument("--exe", default=EXE)
    args = ap.parse_args()

    if not os.path.isfile(args.exe):
        print("找不到 %s —— 先编译：cmake --build build --target VennEditor"
              % args.exe)
        return 2

    env = dict(os.environ)
    env["MYVK_VIEWRIG_TEST"] = "1"
    # 自检在创建设备**之前**返回，所以不需要显卡/窗口；但程序会尝试初始化
    # 日志与本地化，工作目录必须是仓库根（资源按相对路径找）。
    proc = subprocess.run([args.exe], cwd=ROOT, env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=180)
    text = proc.stdout.decode("utf-8", "replace")

    rows = LINE.findall(text)
    total = TOTAL.search(text)
    fails = [r for r in rows if r[5] == "FAIL"]

    for name, actual, expect, tol, detail, _res in fails:
        print("FAIL  %-28s actual=%s expected=%s tol=%s  %s"
              % (name, actual, expect, tol, detail.strip()))

    if not total:
        print("没有 VIEWRIG-TOTAL 行 —— 自检根本没跑起来（看看上面的输出）")
        print(text[-2000:])
        return 2

    passed, failed = int(total.group(1)), int(total.group(2))
    print("\n断言 %d 条：PASS %d / FAIL %d（解析到 %d 行明细）"
          % (passed + failed, passed, failed, len(rows)))

    ok = True
    if failed or fails:
        ok = False
        print("✗ 有失败断言")
    if passed + failed < args.min:
        ok = False
        print("✗ 断言条数 %d < 下限 %d —— 用例被删了，或者跑的不是这份二进制"
              % (passed + failed, args.min))
    if len(rows) != passed + failed:
        ok = False
        print("✗ 明细行数 %d 与 TOTAL %d 对不上（有断言没打印）"
              % (len(rows), passed + failed))

    print("结果: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
