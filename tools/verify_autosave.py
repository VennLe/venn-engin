"""端到端验证：编辑器「崩溃 / 断电自动保存」（H2，2026-10-31）。

==============================================================================
为什么单开一条
==============================================================================
这个特性的**全部价值**都藏在"写到哪个文件"这一个决定里，而它在画面和
主日志上都看不出来 —— 写对了是静默的，写错了也是静默的（只是过一阵子
用户发现好档变成了半小时前的自己）。所以要钉的不是"有没有存"，是这三件：

  · 它**不碰**用户的正式场景文件（B 组，判字节）；
  · 它写的是 `<场景>.autosave.json`，内容就是这个场景（C 组）；
  · 它**不是无脑定时器** —— 场景没改过就不写（D 组，负判据）。

==============================================================================
判据
==============================================================================
A0 · `MYVK_EDITOR_AUTOSAVE_SEC=1.5` 被读进去了（`EDIT-AUTOSAVE config sec=1.5`）。
     ⚠ 少了这条，后面全部超时红都会长得像"功能没接"，而真原因可能只是
       环境变量名拼错。
A1 · **段一（FORCE=0，刚打开、dirty=false）**：4 秒内**一条** `ok=1` 都没有。
     这是本用例的主判据之一。只测"会存"的话，"每 1.5 秒存一次、不管有没有
     改动"照样全绿 —— 而那种实现会让 autosave 的 mtime 永远新鲜，用户看到
     "有备份"却发现里面是空的。
B1 · **段二（FORCE=1）**：`EDIT-AUTOSAVE path=... ok=1 sec=1.5 entities=2`。
B2 · 磁盘上真有 `assets/scenes/_autosave_fixture.autosave.json`。
B3 · **正式档逐字节没变**（MD5 与启动前相同）。
     ⚠ 换成"mtime 没变"也能过，但只有字节才是用户真正在乎的那件事。
C1 · autosave 里的 `entities` 数 == 夹具的 2，且第一块的 guid 就是夹具里那个
     （证明存的是**这个**场景，不是某个默认空场景）。
E1 · **段三**：同目录已有 autosave 时再启动 → `EDIT-AUTOSAVE-STALE path=...
     size=<非 0>`。
E2 · **不自动加载**：段三的日志里 `Scene saved` / 实体数仍是夹具的 2 ——
     stale 只提示，不替用户做决定。

用法：
    python tools/verify_autosave.py
"""

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from safe_fs import safe_remove, safe_rmtree               # noqa: E402  （与下方 verify_* 导入同一批）
from verify_add_menu import LogTap, kill_stale_editors               # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
EDITOR_EXE = os.path.join(BIN, "Editor.exe")

FIXTURE_REL = "assets/scenes/_autosave_fixture.json"
FIXTURE = os.path.join(BIN, FIXTURE_REL)
AUTO_REL = "assets/scenes/_autosave_fixture.autosave.json"
# ⚠ 日志里打的是**已经剥掉 `assets/` 前缀**的规范写法（相对资产根）：
#   那是编辑器里通用的口径（Content 面板 / 场景树都这么显示），也是
#   `scene.save` 落盘路径反算出来的形式。两种写法指向同一个文件，
#   但断言必须认它实际打出来的那一个。
AUTO_LOG_REL = "scenes/_autosave_fixture.autosave.json"
AUTO = os.path.join(BIN, AUTO_REL)

CFG_RE = re.compile(r"EDIT-AUTOSAVE config sec=([\d.]+)")
WRITE_RE = re.compile(r"EDIT-AUTOSAVE path=(\S+) ok=(\d) sec=([\d.]+) "
                      r"entities=(\d+)")
STALE_RE = re.compile(r"EDIT-AUTOSAVE-STALE path=(\S+) size=(\d+)")
OPEN_RE = re.compile(r"Editor: auto-opened scene '(\S+)' \((\d+) entities\)")

results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print("  [%s] %s %s" % ("PASS" if ok else "FAIL", name, detail))
    return ok


def lines_of(tap):
    with tap.lock:
        return list(tap.lines)


def find_all(tap, rx):
    return [m for m in (rx.search(l) for l in lines_of(tap)) if m]


def make_fixture():
    ents = []
    for k in range(2):
        ents.append({
            "name": "Auto%d" % k,
            "guid": "%016x" % (0x6a17000000000000 + k),
            "transform": {"position": [2.0 * k, 0.0, 1.0],
                          "rotation": [0.0, 0.0, 0.0],
                          "scale": [1.0, 1.0, 1.0]},
            "parent": -1,
            "mesh": {"kind": "builtin", "shape": "cube", "size": 1.0},
            "material": {"baseColor": [0.6, 0.7, 0.4, 1.0],
                         "roughness": 0.7, "metallic": 0.0},
        })
    scene = {"version": 2, "generator": "verify_autosave",
             "camera": {"target": [0, 0, 1], "yaw": 0.61, "pitch": 0.31,
                        "distance": 9.0},
             "physics": {"enabled": False, "groundPlane": False,
                         "fixedTimeStep": 0.016666, "maxSubSteps": 4},
             "entities": ents}
    os.makedirs(os.path.dirname(FIXTURE), exist_ok=True)
    with open(FIXTURE, "w", encoding="utf-8") as f:
        json.dump(scene, f, ensure_ascii=False, indent=1)


def md5_of(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def launch(exe, run_sec, force):
    """起一次编辑器，跑 `run_sec` 秒后关掉，返回日志文本。

    ⚠ 不用 `MYVK_FRAMES`：那个是"跑满 N 帧就退"，帧率在无人值守下不可控
      （关掉 vsync 后一帧可能只有 2 ms），2 秒的间隔根本攒不够。
      这里改成"让它自己跑，到点就 terminate" —— 用例判的是**时间**驱动
      的行为，就得按时间来。
    """
    env = dict(os.environ)
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_EDITOR_SCENE"] = FIXTURE_REL
    env["MYVK_EDITOR_AUTOSAVE_SEC"] = "1.5"
    if force:
        env["MYVK_EDITOR_AUTOSAVE_FORCE"] = "1"
    else:
        env.pop("MYVK_EDITOR_AUTOSAVE_FORCE", None)
    proc = subprocess.Popen([exe], cwd=BIN, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True,
                            encoding="utf-8", errors="replace", bufsize=1)
    tap = LogTap()
    tap.start(proc)
    try:
        time.sleep(run_sec)
    finally:
        text = "\n".join(lines_of(tap))
        proc.terminate()
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()
    return text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=EDITOR_EXE)
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    if not os.path.isfile(exe):
        check("0 找不到 Editor.exe", False, exe)
        return 1

    make_fixture()
    for p in (AUTO,):
        try:
            safe_remove(p)
        except OSError:
            pass
    before = md5_of(FIXTURE)

    kill_stale_editors()

    # ---------------- 段一：刚打开、dirty=false，不许写 ----------------
    t1 = launch(exe, 5.0, force=False)
    cfg = CFG_RE.search(t1)
    check("A0 MYVK_EDITOR_AUTOSAVE_SEC 被读进去", cfg is not None,
          "cfg=%s" % (cfg.group(1) if cfg else None))
    wrote1 = [m for m in (WRITE_RE.search(l) for l in t1.splitlines()) if m]
    check("A1 场景没改过（dirty=false）→ 4 秒内一条都不写", len(wrote1) == 0,
          "writes=%d 首条=%s" % (len(wrote1),
                                (wrote1[0].groups() if wrote1 else None),))
    check("A2 段一没在磁盘上留下 autosave", not os.path.isfile(AUTO),
          AUTO)

    # ---------------- 段二：FORCE=1，必须写 ----------------
    t2 = launch(exe, 5.0, force=True)
    wrote2 = [m for m in (WRITE_RE.search(l) for l in t2.splitlines()) if m]
    ok2 = [m for m in wrote2 if m.group(2) == "1"]
    check("B1 FORCE=1 → EDIT-AUTOSAVE ok=1（写的是 .autosave.json）",
          len(ok2) >= 1 and ok2[0].group(1) == AUTO_LOG_REL,
          "line=%s n=%d" % ((ok2[0].groups() if ok2 else None), len(ok2)))
    check("B2 磁盘上真有那个 autosave", os.path.isfile(AUTO)
          and os.path.getsize(AUTO) > 64,
          "%s size=%s" % (AUTO, os.path.getsize(AUTO)
                          if os.path.isfile(AUTO) else "missing"))
    check("B3 正式档逐字节没变（自动保存不覆盖用户文件）",
          os.path.isfile(FIXTURE) and md5_of(FIXTURE) == before,
          "before=%s after=%s" % (before,
                                  md5_of(FIXTURE)
                                  if os.path.isfile(FIXTURE) else "missing"))

    # ---------------- C：写进去的是这个场景 ----------------
    ent_n, guid0 = -1, None
    if os.path.isfile(AUTO):
        try:
            with open(AUTO, "r", encoding="utf-8") as f:
                doc = json.load(f)
            ents = doc.get("entities") or []
            ent_n = len(ents)
            guid0 = (ents[0].get("guid") if ents else None)
        except Exception as e:                      # noqa: BLE001
            print("  读 autosave 失败：", e)
    check("C1 autosave 的内容就是当前场景（2 个实体 + 同一个 guid）",
          ent_n == 2 and (guid0 or "").lower() == "%016x" % 0x6a17000000000000,
          "entities=%s guid=%s" % (ent_n, guid0))

    # ---------------- 段三：stale 检测（只提示、不加载）----------------
    # ⚠ 光"数实体"证不了"没自动加载"：autosave 里就是这份场景，两个方向
    #   数出来一样。往 autosave 里塞一个**只在它里面存在**的标记实体，
    #   加载了会看见它，没加载就看不见 —— 这才是有区分度的判据。
    marker = "AutoStaleMarker"
    try:
        with open(AUTO, "r", encoding="utf-8") as f:
            doc = json.load(f)
        doc.setdefault("entities", []).append({
            "name": marker, "guid": "%016x" % 0x6a17deadbeef0000,
            "transform": {"position": [9.0, 9.0, 9.0],
                          "rotation": [0.0, 0.0, 0.0],
                          "scale": [1.0, 1.0, 1.0]},
            "parent": -1,
            "mesh": {"kind": "builtin", "shape": "cube", "size": 1.0},
        })
        with open(AUTO, "w", encoding="utf-8") as f:
            json.dump(doc, f, ensure_ascii=False, indent=1)
    except Exception as e:                          # noqa: BLE001
        print("  改 autosave（埋标记）失败：", e)

    t3 = launch(exe, 4.0, force=False)
    stale = [m for m in (STALE_RE.search(l) for l in t3.splitlines()) if m]
    check("E1 已有 autosave 时启动 → EDIT-AUTOSAVE-STALE 且 size > 0",
          len(stale) >= 1 and int(stale[0].group(2)) > 0,
          "line=%s" % ((stale[0].groups() if stale else None),))
    # 基线 = 段一"干净打开夹具"时的实体数（它含编辑器自带的那一个，不是夹具
    # 的 2）—— 拿它当尺子，两种实现之间唯一的差别就只剩"autosave 有没有被加载"。
    base_n = None
    o1 = [m for m in (OPEN_RE.search(l) for l in t1.splitlines()) if m]
    if o1:
        base_n = o1[0].group(2)
    opened = [m for m in (OPEN_RE.search(l) for l in t3.splitlines()) if m]
    check("E2 stale 只提示、不自动加载（实体数与干净打开一致，且看不到标记）",
          base_n is not None and bool(opened)
          and opened[0].group(2) == base_n and marker not in t3,
          "base=%s open=%s marker=%s"
          % (base_n, (opened[0].group(2) if opened else None),
             marker in t3))

    # 收尾：夹具与 autosave 都不许留在仓库里
    for p in (FIXTURE, AUTO):
        try:
            safe_remove(p)
        except OSError:
            pass

    n_pass = sum(1 for _, ok, _ in results if ok)
    print("\n%d/%d PASS" % (n_pass, len(results)))
    if n_pass == len(results):
        print("结果: PASS")
        return 0
    print("结果: FAIL: %d 项" % (len(results) - n_pass))
    return 1


if __name__ == "__main__":
    sys.exit(main())
