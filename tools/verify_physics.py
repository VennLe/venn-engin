"""端到端验证：刚体物理引擎（PhysicsSelfTest 自检入口，2026-10-16）。

这套自检**不需要窗口、不需要 Vulkan**：`editor/main.cpp` 看到
`MYVK_PHYS_TEST=1` 就在 `app.run()` 之前直接跑 `physics::runPhysicsSelfTest()`
然后 return。所以它能在任何环境下跑（包括没装显卡驱动的 CI），而且结果是
**确定性的** —— 正因为如此，它是本套件里唯一能做"逐位可复现"断言的用例。

自检输出格式（PhysicsSelfTest.cpp）：

    PHYS-TEST <name> actual=<...> expected=<...> tol=<...> result=PASS|FAIL
    PHYS-TOTAL pass=<n> fail=<m>

断言分五组：

  A. **用例齐全**：`REQUIRED` 里每一个用例名都必须出现。删掉/改名一个用例
     时这条会失败 —— 否则"把失败的用例删掉"就成了最简单的作弊方式。
  B. **逐条通过**：每一条 PHYS-TEST 都必须是 result=PASS，零 FAIL。
  C. **总数**：PHYS-TOTAL 的 pass 与 fail 要和逐条统计**一致**（防止统计
     本身写错），fail 必须为 0，pass 必须 ≥ 用例数。
  D. **退出码**：`runPhysicsSelfTest()` 返回 fail 数，main 把它当退出码，
     所以进程退出码必须是 0。
  E. **确定性**：同一进程连跑两次，PHYS-TEST 行**逐字符相同**。物理求解里
     任何"按地址/时间/容器迭代顺序"的隐式依赖（哈希表遍历、未初始化的浮点、
      多线程归约顺序）都会让这条炸掉。自检里还有一条 `determinism.*` 用例
      做引擎内的逐位比较，这条是从**进程外**再钉一遍。

这一组的价值在于：物理是最容易"看着对、其实错"的子系统 —— 一叠箱子塌掉
可以有一百种原因，而"浮点数逐位一致"只有一种解释。多个已修的真实 bug
（SAT 边叉积轴尺度不一致、参考面外法线方向、休眠体仍在被重力加速）都是
先被这里的某条断言钉住、再回头定位的。

用法：
    python tools/verify_physics.py
"""

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")

# 用例清单 —— 与 PhysicsSelfTest.cpp 的 testXxx() 一一对应。
# 每一条都对应一个**可独立证伪**的物理事实，不是"跑起来了"。
REQUIRED = [
    # 积分器：半隐式欧拉的闭式解 z_n = z0 − g·dt²·n(n+1)/2。
    # 能同时鉴别"显式欧拉"和"重力倍率写错"。
    "freefall.z",
    "freefall.vz",
    # 恢复系数：接触瞬间速度已含一步 g·dt，期望是 |v0| + g·dt（不是 |v0|）。
    "restitution.vout",
    "restitution.inelastic.maxvz",
    # 库仑摩擦：v = v0 − μ·g·t，且动能摩擦**不能把物体推反**（clamp 到 0）。
    "friction.vx_after_1s",
    "friction.free.vx",
    # 休眠（岛级）：静置体必须在阈值时间内睡着，且醒来时速度必须是 0。
    "sleep.asleep_after_3s",
    "sleep.rest_z",
    "sleep.zero_velocity",
    # 四层堆叠：不塌、不互穿、整体睡着。
    "stack.top_z_after_5s",
    "stack.no_tunneling",
    "stack.asleep_count",
    # 箱-箱窄相：轴对齐 / 歪 0.1° / 真分离 三条判据。
    "boxbox.aligned.hit",
    "boxbox.aligned.depth",
    "boxbox.tilted.hit",
    "boxbox.tilted.depth",
    "boxbox.separated.no_hit",
    # 关节：距离关节长度守恒 / 球关节锚点重合 / 铰链轴。
    "joint.distance.max_err",
    "joint.distance.x_travel",
    "joint.ball.max_err",
    "joint.hinge.anchor_err",
    "joint.hinge.rotates",
    # 查询：射线命中距离 + 未命中（含"擦边不算中"的假阳性护栏）。
    "raycast.box.hit",
    "raycast.box.distance",
    "raycast.box.miss_offscreen",
    "raycast.sphere.miss_beside",
    # 广相：与 O(N²) 暴力对照，配对数必须完全一致。
    "broadphase.contact_pairs",
    # 确定性：同场景两次运行逐位相同。
    "determinism.bitwise_identical",
    "determinism.max_diff",
]


class Tap:
    """收集 PHYS-TEST / PHYS-TOTAL 行。"""

    def __init__(self):
        self.lines = []      # 所有 PHYS-TEST 原始行（保序）
        self.cases = []      # (name, result)
        self.total = None    # (pass, fail)
        self.other = []      # 其它 INFO 行（排查用）

    def feed(self, text):
        for raw in text.splitlines():
            line = raw.strip()
            if "PHYS-TEST" in line:
                self.lines.append(line)
                m = re.search(r"PHYS-TEST\s+(\S+).*?result=(\w+)", line)
                if m:
                    self.cases.append((m.group(1), m.group(2)))
            elif "PHYS-TOTAL" in line:
                m = re.search(r"pass=(\d+)\s+fail=(\d+)", line)
                if m:
                    self.total = (int(m.group(1)), int(m.group(2)))
            elif line:
                self.other.append(line)


def run_once(exe, timeout=180):
    env = dict(os.environ)
    env["MYVK_PHYS_TEST"] = "1"
    # 自检不建窗口，但为保险起见把窗口/存档相关的钩子也钉死，
    # 免得将来有人不小心让自检走到窗口路径上。
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    p = subprocess.run([exe], cwd=os.path.dirname(exe), env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       timeout=timeout)
    text = p.stdout.decode("utf-8", errors="replace")
    tap = Tap()
    tap.feed(text)
    return p.returncode, tap, text


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_EXE
    if not os.path.isfile(exe):
        print("找不到可执行文件: %s" % exe)
        return 1

    failures = []

    def check(name, ok, detail=""):
        mark = "OK  " if ok else "FAIL"
        print("  [%s] %s%s" % (mark, name, ("  <- " + detail) if detail else ""))
        if not ok:
            failures.append(name)

    print("=" * 68)
    print("物理自检 (MYVK_PHYS_TEST=1)")
    print("  可执行文件: %s" % exe)
    print("=" * 68)

    rc, tap, _ = run_once(exe)

    # ---------------------------------------------------------- A. 用例齐全
    print("\n--- A. 用例齐全 ---")
    names = [n for n, _ in tap.cases]
    missing = [w for w in REQUIRED if w not in names]
    check("全部 %d 组用例都出现" % len(REQUIRED), not missing,
          "缺失: %s" % missing if missing else "%d 条断言" % len(names))

    # ---------------------------------------------------------- B. 逐条通过
    print("\n--- B. 逐条通过 ---")
    failed = [n for n, r in tap.cases if r != "PASS"]
    for n, r in tap.cases:
        if r != "PASS":
            print("      FAIL: %s" % n)
    check("零 FAIL", not failed, "失败 %d 条" % len(failed))
    check("断言总数 ≥ %d" % len(REQUIRED), len(tap.cases) >= len(REQUIRED),
          "%d 条" % len(tap.cases))

    # ---------------------------------------------------------- C. 总数自洽
    print("\n--- C. 总数自洽 ---")
    if tap.total is None:
        check("出现 PHYS-TOTAL 行", False, "没找到")
    else:
        np_, nf = tap.total
        counted_pass = sum(1 for _, r in tap.cases if r == "PASS")
        counted_fail = sum(1 for _, r in tap.cases if r != "PASS")
        check("PHYS-TOTAL fail=0", nf == 0, "fail=%d" % nf)
        check("PHYS-TOTAL pass ≥ %d" % len(REQUIRED), np_ >= len(REQUIRED),
              "pass=%d" % np_)
        check("PHYS-TOTAL 与逐条统计一致",
              (np_, nf) == (counted_pass, counted_fail),
              "total=(%d,%d) 逐条=(%d,%d)" % (np_, nf, counted_pass,
                                              counted_fail))

    # ---------------------------------------------------------- D. 退出码
    print("\n--- D. 退出码 ---")
    check("进程退出码 0", rc == 0, "rc=%d" % rc)

    # ---------------------------------------------------------- E. 确定性
    print("\n--- E. 确定性（跨进程逐字符）---")
    rc2, tap2, _ = run_once(exe)
    if tap.lines != tap2.lines:
        diff = next((i for i, (a, b) in enumerate(zip(tap.lines, tap2.lines))
                     if a != b), min(len(tap.lines), len(tap2.lines)))
        detail = ("行数 %d vs %d" % (len(tap.lines), len(tap2.lines))
                  if len(tap.lines) != len(tap2.lines)
                  else "首个不同的行 #%d" % diff)
        check("两次运行 PHYS-TEST 逐字符相同", False, detail)
    else:
        check("两次运行 PHYS-TEST 逐字符相同", True,
              "%d 条" % len(tap.lines))
    check("两次退出码相同", rc == rc2, "%d vs %d" % (rc, rc2))

    # ---------------------------------------------------------- 结论
    print("\n" + "=" * 68)
    if failures:
        print("结果: FAIL: %d 项未通过 -> %s" % (len(failures), failures))
        return 1
    print("结果: PASS  (%d 条断言 / %d 组用例)"
          % (len(tap.cases), len(REQUIRED)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
