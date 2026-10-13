"""跑一轮回归：逐个启动 GUI 验证脚本，记录退出码与结果行。

GUI 测试要独占窗口，只能串行。而且**必须以退出码为准** —— 这批脚本的
结果行格式不统一（`RESULT: PASS` / `全部断言 PASS` / `全部 PASS`），
只靠文本匹配会漏判。失败时把该脚本的日志尾部打出来，省得再跑一遍。

---- 增量：上次 PASS 的用例默认不再重跑 ----
全套 31 例要 25 分钟，其中绝大多数是几十~两百秒的 GUI 用例。改了某一块
渲染功能之后，"其余 30 例又全跑一遍"纯属浪费。所以本脚本会记一份缓存
（`_venn_verify/_regression/_cache.json`），**上次 PASS 且中间没有任何
产物变化**的用例直接跳过，只打印一行 `SKIP`。

跳过的前提是"代码真的没变"，所以缓存带一个**产物指纹**：`build/bin`
下所有文件（含 Editor.exe 与被拷过去的 .spv / 材质 / 模型 / 场景）+
`tools/*.py` 的 `(路径, 大小, mtime)` 摘要。指纹对不上 → 缓存整体作废、
全跑。这样"改了 shader 只触发拷贝、exe 本身没重链接"这种情况也能正确
识别（只看 exe 的 mtime 就会误跳过）。

⚠ 明确写出的用例名**永远跑**（那是"我就要看这个"的意思，不是"顺手带上"）。

用法：
    python tools/run_regression.py                      # 跑默认全套（跳过已 PASS 的）
    python tools/run_regression.py --all                # 无视缓存，全跑一遍
    python tools/run_regression.py bvh player add_menu  # 只跑指定的几个（总是跑）
    python tools/run_regression.py --tail 60 bvh        # 只看尾部行数
    python tools/run_regression.py --no-cache           # 不读也不写缓存
"""

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PY = sys.executable
# 单测完整日志的落盘目录（和截图产物放一起）
LOG_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify", "_regression")
CACHE_PATH = os.path.join(LOG_DIR, "_cache.json")
# 产物目录：exe + 被拷过去的 shader/材质/模型/场景
BIN_DIR = os.path.join(ROOT, "build", "bin")
TOOLS_DIR = os.path.join(ROOT, "tools")

DEFAULT = [
    "verify_frustum_cull", "verify_bvh", "verify_lod", "verify_ibl",
    "verify_csm", "verify_local_shadow", "verify_shadow_hardening",
    "verify_volumetric_fog",
    "verify_shader_variants",
    "verify_shader_hotreload", "verify_fxaa", "verify_instancing",
    "verify_nav_gizmo",
    "verify_character_window", "verify_add_menu", "verify_mesh_params",
    "verify_collider_gizmo", "verify_player", "verify_collision_play",
    "verify_drop_to_floor", "verify_material_roundtrip",
    "verify_primitive_roundtrip", "verify_screenspace", "verify_textures",
    # 多相机 / 通用 RenderTarget + 相机特效（DoF / 运动模糊 / 色差 / 暗角 /
    # 后处理体积）
    "verify_camfx",
    "verify_gpu_timing",
    "verify_viewmodes",
    "verify_taa",
    "verify_auto_exposure",
    "verify_color_lut",
    "verify_drs",
    # Hi-Z 遮挡剔除（R2-1）：深度金字塔 + AABB 遮挡测试 + 两帧延迟回读
    "verify_hiz",
    # Multi-draw indirect + GPU 紧凑化（R2-2）：间接命令 + GPU 写 draw 参数
    "verify_mdi",
    # Bindless / descriptor indexing（R2-3）：纹理数组 + 材质页，per-draw
    # 只 push 页号、零描述符重绑（开关都必须逐像素回到基线）
    "verify_bindless",
]

# 结果行：兼容四种写法。取**最后一条**，因为有的脚本会先打一堆中间断言。
OK_RE = re.compile(r"(?:RESULT:\s*PASS|全部断言 PASS|^全部 PASS\b|结果:\s*PASS)")
BAD_RE = re.compile(r"(?:RESULT:\s*FAIL|断言[^\n]*FAIL|结果:\s*FAIL)")


def fingerprint():
    """产物指纹：build/bin 下所有文件 + tools/*.py 的 (相对路径, 大小, mtime)。

    取的是**内容会不会变**，不是"源码改没改" —— 改了 shader 只触发拷贝、
    exe 本身没重链接时，只看 exe 的 mtime 会错误地跳过已失效的用例。

    `run_regression.py` **自己不算在内**：它是"跑法"（改的只是跳过策略、
    输出格式），不影响任何用例的判定结果。把它算进去的话，动一次本文件
    就让整份缓存作废、白跑 25 分钟。
    """
    h = hashlib.sha1()
    for base, suffix in ((BIN_DIR, None), (TOOLS_DIR, ".py")):
        if not os.path.isdir(base):
            h.update(b"<missing:%s>" % base.encode("utf-8"))
            continue
        for dirpath, _dirnames, filenames in os.walk(base):
            for fn in sorted(filenames):
                if suffix and not fn.endswith(suffix):
                    continue
                if suffix and fn == os.path.basename(__file__):
                    continue
                full = os.path.join(dirpath, fn)
                try:
                    st = os.stat(full)
                except OSError:
                    continue
                rel = os.path.relpath(full, base).replace("\\", "/")
                h.update(("%s|%d|%d\n" % (rel, st.st_size,
                                          int(st.st_mtime_ns))).encode())
    return h.hexdigest()


def load_cache():
    try:
        with open(CACHE_PATH, "r", encoding="utf-8") as fh:
            data = json.load(fh)
    except (OSError, ValueError):
        return None
    if not isinstance(data, dict) or "fingerprint" not in data:
        return None
    return data


def save_cache(fp, passed):
    os.makedirs(LOG_DIR, exist_ok=True)
    tmp = CACHE_PATH + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump({"fingerprint": fp, "passed": sorted(passed)}, fh, indent=1)
    os.replace(tmp, CACHE_PATH)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tests", nargs="*", help="只跑这些（可省 verify_ 前缀）")
    ap.add_argument("--tail", type=int, default=25,
                    help="失败时打印日志尾部行数（默认 25）")
    ap.add_argument("--settle", type=float, default=2.0,
                    help="两个用例之间额外等待的秒数（默认 2）")
    ap.add_argument("--all", "-a", action="store_true",
                    help="无视缓存，全部重跑")
    ap.add_argument("--no-cache", action="store_true",
                    help="不读也不写缓存")
    args = ap.parse_args()

    explicit = bool(args.tests)
    names = args.tests or DEFAULT
    names = [n if n.startswith("verify_") else "verify_" + n for n in names]

    use_cache = not args.no_cache
    fp = fingerprint() if use_cache else ""
    cache = load_cache() if use_cache else None
    cached_ok = set()
    if cache and cache.get("fingerprint") == fp and not args.all:
        cached_ok = set(cache.get("passed") or [])
    elif cache and cache.get("fingerprint") != fp and not explicit:
        print("产物有变化（build/bin 或 tools/*.py）→ 缓存作废，全部重跑\n")

    rows = []
    skipped = []
    ran = 0
    for n in names:
        path = os.path.join(TOOLS_DIR, n + ".py")
        if not os.path.exists(path):
            rows.append((n, None, "脚本不存在", ""))
            print("%-26s %s" % (n, "脚本不存在"))
            continue
        # 明确点名的不跳（"我就要看这个"）；否则上次 PASS 且产物没变就跳
        if (not explicit) and n in cached_ok:
            skipped.append(n)
            print("%-26s %s" % (n, "SKIP  (上次 PASS，产物未变)"))
            continue
        if ran:   # 让上一个编辑器进程彻底退场、前台焦点回稳
            time.sleep(args.settle)
        t0 = time.time()
        ran += 1
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

    # ---- 缓存回写：本轮跑过的用例按结果更新，没跑到的沿用旧的 ----
    # 只有 PASS 才进缓存（FAIL 会被丢弃 → 下次一定重跑，这正是我们要的）。
    if use_cache and fp:
        keep = set(cached_ok) if (cache and cache.get("fingerprint") == fp) \
            else set()
        if not explicit:
            keep &= set(names)
        for n, _rc, mark, _out in rows:
            if mark == "PASS":
                keep.add(n)
            else:
                keep.discard(n)
        save_cache(fp, keep)

    bad = [r for r in rows if r[2] != "PASS"]
    total = len(rows) + len(skipped)
    done = total - len(bad)
    print("\n%d/%d 通过" % (done, total))
    if skipped:
        print("（其中 %d 例沿用上次 PASS 未重跑：%s）"
              % (len(skipped), ", ".join(skipped)))
    if bad:
        print("失败：" + ", ".join(r[0] for r in bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
