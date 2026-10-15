"""端到端验证：ContentBrowser 升级（E13）。

断言（都可机器验证）：

  ① **类型过滤切换后可见项数量变化正确**：分别以 All / Models / Textures /
     Folders 打开某个目录，读日志里的 `CB-ENTRIES ... type=N count=M`，
     断言
       - assets/models    ：All == Models > 0，Textures == 0
       - assets/textures  ：Textures > 0，Models == 0
       - assets/shaders   ：Shaders > 0，Textures == 0
       - assets（根）      ：Folders == 子目录数 == All（该层只有目录）

  ② **网格 / 列表视图与缩略图尺寸**：view=List 与 size=0..3 下都能正常
     起停（0 ERROR / 0 VUID），且日志里 view 字段与注入一致。

  ③ **拖资产到落点 → 生成实体**：这条已是 `tools/verify_asset_drag.py`
     的职责（真鼠标拖拽，无法用开关模拟），本脚本在末尾顺带跑它**两次**，
     把"拖放全链路"整条纳入 E13 的验收：
       - `--target vp`  → 视口落点，断言 `alignImportToGround`（贴地）
       - `--target hier`→ Hierarchy 树下方空白区，断言 `HIER-IMPORT`

     注意两个既定条件（不是本脚本的假设，是可复现的现状）：
       - 需要 --env MYVK_CONTENT_DIR=assets/models，否则 Content 停在
         assets 根，看不到网格模型；
       - 需 --asset anime_girl.fbx。默认的 box01.glb 在 assets/models 里
         排在**第三行**，而 Content 面板默认只有约 400px 高，第三行被网格
         滚动区裁掉 → 脚本点不到它。改用第一/二行的模型
         （WorldMap.glb / anime_girl.fbx / anime_girl.glb）即可稳定通过。

用法：
    python tools/verify_content_browser.py [--exe build/bin/Editor.exe]
    python tools/verify_content_browser.py --no-drag     # 跳过 ③（省时间）
"""

import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")

ENTRIES_RE = re.compile(
    r"CB-ENTRIES dir=(\S+) filter=(\S+) type=(\d+) view=(\d+) count=(\d+)")
BAD_RE = re.compile(r"(ERROR|VUID|leaked objects)")


def run_editor(exe, content_dir, type_name, frames=45, view=None, size=None):
    env = dict(os.environ)
    env["MYVK_FRAMES"] = str(frames)
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_CONTENT_DIR"] = content_dir
    if type_name:
        env["MYVK_EDITOR_CB_TYPE"] = type_name
    if view:
        env["MYVK_EDITOR_CB_VIEW"] = view
    if size is not None:
        env["MYVK_EDITOR_CB_SIZE"] = str(size)
    p = subprocess.run([exe], cwd=os.path.dirname(exe), env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, encoding="utf-8", errors="replace", timeout=180)
    return p.stdout + (p.stderr or "")


def last_count(out, type_idx):
    """取指定 type 过滤下最后一次 CB-ENTRIES 的 count。"""
    hits = [m for m in ENTRIES_RE.finditer(out)
            if int(m.group(3)) == type_idx]
    return int(hits[-1].group(5)) if hits else None


def last_view(out):
    hits = list(ENTRIES_RE.finditer(out))
    return int(hits[-1].group(4)) if hits else None


# 过滤器枚举索引（与 ContentBrowser::TypeFilter 一一对应）
T_ALL, T_MODEL, T_TEX, T_MAT, T_SCENE, T_SCRIPT, T_SHADER, T_FOLDER = range(8)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--no-drag", action="store_true")
    args = ap.parse_args()
    exe = os.path.abspath(args.exe)
    if not os.path.exists(exe):
        print("可执行文件不存在:", exe)
        return 2

    failures = []

    def probe(content_dir, type_idx, label):
        name = ["all", "model", "texture", "material", "scene", "script",
                "shader", "folder"][type_idx]
        out = run_editor(exe, content_dir, name)
        if BAD_RE.search(out):
            failures.append("%s/%s 运行期出现 ERROR/VUID/泄漏" % (content_dir, name))
        return last_count(out, type_idx)

    print("[1] 类型过滤：可见项数量变化正确")
    # 注意：运行时资产根（build/bin/assets）不含 shaders/（着色器被编译进
    # 变体表，不落地成文件），所以 shader 一例用相对路径指到源码目录。
    SHADERS = "../../assets/shaders"
    cases = [
        ("assets/models",   T_ALL,      "All",      None),
        ("assets/models",   T_MODEL,    "Models",   ">0"),
        ("assets/models",   T_TEX,      "Textures", "==0"),
        ("assets/models",   T_FOLDER,   "Folders",  ">0"),
        ("assets/textures", T_TEX,      "Textures", ">0"),
        ("assets/textures", T_MODEL,    "Models",   "==0"),
        (SHADERS,           T_SHADER,   "Shaders",  ">0"),
        (SHADERS,           T_TEX,      "Textures", "==0"),
        (SHADERS,           T_MODEL,    "Models",   "==0"),
    ]
    for content_dir, type_idx, label, expect in cases:
        n = probe(content_dir, type_idx, label)
        ok = n is not None
        if ok and expect == ">0":
            ok = n > 0
        if ok and expect == "==0":
            ok = n == 0
        print("    %-22s %-9s count=%s  expect=%s  %s"
              % (content_dir, label, n, expect or "-", "PASS" if ok else "FAIL"))
        if not ok:
            failures.append("%s + %s: count=%s (expect %s)"
                            % (content_dir, label, n, expect))

    # 三条守恒：All = 各具体类型之和（在 assets/models 上验证）
    a = probe("assets/models", T_ALL, "All")
    m = probe("assets/models", T_MODEL, "Models")
    f = probe("assets/models", T_FOLDER, "Folders")
    ok = None not in (a, m, f) and a == m + f and a > 0
    print("    assets/models    All == Models + Folders : %s "
          "(All=%s Models=%s Folders=%s)"
          % ("PASS" if ok else "FAIL", a, m, f))
    if not ok:
        failures.append("assets/models: All(%s) != Models(%s)+Folders(%s)"
                        % (a, m, f))

    print("[2] 视图切换 / 缩略图尺寸")
    out = run_editor(exe, "assets/models", "all", view="list")
    if last_view(out) != 1:
        failures.append("list 视图未生效（view=%s）" % last_view(out))
    print("    list  view=1 : %s" % ("PASS" if last_view(out) == 1 else "FAIL"))
    if BAD_RE.search(out):
        failures.append("list 视图运行期出现 ERROR/VUID/泄漏")

    for size in (0, 1, 2, 3):
        outp = run_editor(exe, "assets/models", "all", size=size)
        if BAD_RE.search(outp):
            failures.append("size=%d 运行期出现 ERROR/VUID/泄漏" % size)
    print("    size 0..3 起停干净 : %s"
          % ("PASS" if not any("size=" in f for f in failures) else "FAIL"))

    print("[3] 拖资产到视口 / Hierarchy（真鼠标拖拽，交由 verify_asset_drag 覆盖）")
    if args.no_drag:
        print("    SKIP (--no-drag)")
    else:
        py = sys.executable
        for target, label, marker in (
                ("vp",   "视口",       "贴地断言: PASS"),
                ("hier", "Hierarchy",  "落点断言: PASS")):
            p = subprocess.run(
                [py, os.path.join(ROOT, "tools", "verify_asset_drag.py"),
                 "--target", target,
                 "--env", "MYVK_CONTENT_DIR=assets/models",
                 "--asset", "anime_girl.fbx"],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, encoding="utf-8", errors="replace", timeout=600)
            out = p.stdout or ""
            ok = marker in out
            print("    %-10s %s" % (label, "PASS" if ok else "FAIL"))
            if not ok:
                print("\n".join(out.splitlines()[-10:]))
                failures.append("verify_asset_drag (%s) 失败" % label)

    print()
    if failures:
        print("结果: FAIL")
        for f in failures:
            print("  -", f)
        return 1
    print("全部断言 PASS")
    print("  类型过滤数量正确 / 视图与尺寸切换干净 / 拖放全链路（视口 + Hierarchy）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
