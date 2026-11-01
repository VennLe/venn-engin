"""端到端验证：存档槽位 + CLI + 基线接线（G17-3，2026-10-31）。

被验证的缺口（原话出处：`docs/remaining_work_2026-10-27.md` §2.6 G17）：
    G17-1（文件层）+ G17-2（应用层）都落了，自检 77 条绿，但**没有槽位、
    没有 CLI、`captureBaseline` 没接线** —— 存档停在"库"的层面，玩家按不到。

这一轮补的三件：
  · `scene/SaveSlot.{h,cpp}`：槽位白名单（`slot1..3` / `quick` / `auto`）+
    目录（相对 CWD 的 `saves/`）+ 一次完整的存 / 读动作。
  · CLI：`--save` / `--load` / `--slot` / `--save-dir`；快捷键 F5 / F9。
  · `RuntimeApp` 里 `captureBaseline`：关卡加载完 / 换关之后各抓一次。

四组：
  A. 内核（`--selftest save`，K 组）
     A1 总数与零失败
     A2 代表断言必须真的出现过（只数总数的话，删掉关键几条也能凑够）
  B. 一次真实的 存 → 读 → 再存
     B1 `--save` 打了 `SAVE-WRITE`
     B2 档里那个移动过的实体**真的不在关卡初始位置**（否则"存了个空"也能过）
     B3 `globals.playerGuid` 是这个固定值（玩家是运行时生成的，只有它能对上）
     B4 `--load` 不给 `--scene` 时**自己进存档记的那一关**，且 `drift=0`
     B5 读档后立刻再存：位置与第一次存档一致（**这一条才是"真的恢复了吗"**）
  C. 边界
     C1 非法槽位当场拒绝，**不创建文件**（`--slot quik`）
     C2 读不存在的槽位：报错、`ok=0`、进程正常退出（不崩）
     C3 槽位名大小写不敏感：`--slot QUICK` 落到的还是 `quick.vsave`
  D. 零 ERROR / 零 VUID

用法：python tools/verify_save_slots.py
"""

import json
import os
import re
import shutil
import subprocess
import sys
from safe_fs import safe_remove, safe_rmtree               # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
GAME = os.path.join(BIN, "Game.exe")
SRC = os.path.join(ROOT, "assets")
DST = os.path.join(BIN, "assets")

SCENE = "scenes/_save_fixture.json"
SAVE_DIR = "_venn_saves"
# 玩家实体的固定 guid（RuntimeApp.cpp 里的 `kPlayerGuid`，"PLAYER"+01）。
PLAYER_GUID = "504c415945520001"

fails = []
n_pass = 0


def check(ok, msg):
    global n_pass
    print("    %-76s %s" % (msg, "PASS" if ok else "FAIL"))
    if not ok:
        fails.append(msg)
    else:
        n_pass += 1
    return ok


def sync_fixture():
    """夹具必须落到 `build/bin/assets/scenes/` —— `--scene` 是相对 assets 根的。

    ⚠⚠ 新加的资产**不会**自动进 `build/bin/`，症状是"加了完全没效果"或
      "`--scene` 打不开但不报错"。
    """
    src = os.path.join(SRC, "scenes", "_save_fixture.json")
    dst = os.path.join(DST, "scenes", "_save_fixture.json")
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.exists(src):
        shutil.copyfile(src, dst)


def run(args, env_extra=None, timeout=180):
    env = dict(os.environ)
    if env_extra:
        env.update(env_extra)
    try:
        p = subprocess.run([GAME] + args, cwd=BIN, env=env, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True,
                           encoding="utf-8", errors="replace", timeout=timeout)
        return p.returncode, (p.stdout or "")
    except subprocess.TimeoutExpired as e:
        out = e.stdout or ""
        if isinstance(out, bytes):
            out = out.decode("utf-8", "replace")
        return -1, out


def slot_files():
    d = os.path.join(BIN, SAVE_DIR)
    if not os.path.isdir(d):
        return {}
    out = {}
    for name in os.listdir(d):
        if name.endswith(".vsave"):
            out[name] = os.path.join(d, name)
    return out


def mover_z(path):
    """读一份 .vsave，返回 `Mover` 的 z。没有 → None。"""
    try:
        with open(path, encoding="utf-8") as f:
            doc = json.load(f)
    except Exception:                     # noqa: BLE001
        return None
    for e in doc.get("entities", []):
        if e.get("name") == "Mover":
            return e.get("transform", {}).get("position", [0, 0, 0])[2]
    return None


def header(path):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except Exception:                     # noqa: BLE001
        return {}


def main():
    if not os.path.exists(GAME):
        print("找不到可执行文件，请先构建：", GAME)
        return 1
    sync_fixture()

    # ============================================================ A 内核
    print("\n[A] 内核自检（--selftest save 的 K 组）")
    rc, out = run(["--selftest", "save"])
    m = re.search(r"SAVE-TOTAL pass=(\d+) fail=(\d+)", out)
    if not m:
        check(False, "A1 自检没跑起来（输出里没有 SAVE-TOTAL）")
        print(out[-1200:])
        return 1
    npass, nfail = int(m.group(1)), int(m.group(2))
    check(nfail == 0, "A1 零失败（pass=%d fail=%d）" % (npass, nfail))
    check(npass >= 101, "A1 断言条数够（%d >= 101）" % npass)
    for key, label in (
            ("slot_known_names", "白名单认 slot1..3/quick/auto"),
            ("slot_reject_unknown", "不认识的槽位被拒"),
            ("slot_case_insensitive", "槽位名大小写不敏感"),
            ("slot_path_invalid_empty", "非法槽位拼不出路径（安全）"),
            ("slot_save_incremental", "第一次存档是增量的"),
            ("slot_save_repeat_keeps_entry", "重复存档不是空档"),
            ("slot_load_z", "读档把变换放回去了"),
            ("slot_load_refreshes_baseline", "读档后基线已刷新"),
            ("slot_save_globals_ok", "vars / playerGuid 真的进档"),
            ("slot_load_vars_restored", "脚本变量读得回来"),
            ("slot_save_bad_name_no_file", "非法槽位不留文件"),
            ("slot_list_order", "槽位列表顺序 = 白名单顺序"),
    ):
        check(key in out, "A2 代表断言在：%s" % label)

    # ============================================================ B 存 → 读
    print("\n[B] 一次真实的 存 → 读 → 再存")
    safe_rmtree(os.path.join(BIN, SAVE_DIR))
    rc, out = run(["--scene", SCENE, "--frames", "90", "--save", "--slot", "quick",
                   "--save-dir", SAVE_DIR])
    check(bool(re.search(r"SAVE-WRITE slot=quick path=\S+ entities=(\d+)", out)),
          "B1 --save 打了 SAVE-WRITE")
    files = slot_files()
    check("quick.vsave" in files, "B1b 落到了 quick.vsave（%s）"
          % (sorted(files) if files else "无"))

    path = os.path.join(BIN, SAVE_DIR, "quick.vsave")
    z1 = mover_z(path)
    hdr = header(path)
    check(hdr.get("version") == 2, "B2 存档版本 = 2（%s）" % hdr.get("version"))
    # ⚠ 这条是"存了个空"的否定测试：Mover 有关卡初始位置 z=0，跑 90 帧之后
    #   应该已经离开了。如果实现只是把关卡原样写回去，这里会 ≈0。
    check(z1 is not None and z1 > 0.01,
          "B2b 档里那个实体真的动了（Mover z=%s）" % z1)
    check(hdr.get("globals", {}).get("playerGuid") == PLAYER_GUID,
          "B3 globals.playerGuid 是固定值（%s）"
          % hdr.get("globals", {}).get("playerGuid"))
    hash1 = hdr.get("sceneHash")

    # ⚠ 不给 --scene：要求它自己从存档里读出关卡。
    rc, out = run(["--load", "--frames", "2", "--slot", "quick",
                   "--save-dir", SAVE_DIR, "--save"])
    check("SAVE-LOAD-SCENE slot=quick scene=%s (from save)" % SCENE in out,
          "B4 --load 自己进了存档记的那一关")
    m = re.search(r"SAVE-READ slot=quick .*applied=(\d+).*drift=(\d)", out)
    check(bool(m) and m.group(1) == "1" and m.group(2) == "0",
          "B4b 读档成功且不是漂移拒读（%s）" % (m.group(0) if m else "无 SAVE-READ"))
    check("SAVE-DRIFT-REJECT" not in out, "B4c 没有漂移拒读")

    z2 = mover_z(path)
    check(z1 is not None and z2 is not None and abs(z2 - z1) < 0.01,
          "B5 读档后位置与第一次存档一致（%.4f → %.4f）"
          % (z1 if z1 is not None else -1, z2 if z2 is not None else -1))
    hash2 = header(path).get("sceneHash")
    check(hash1 is not None and hash1 == hash2,
          "B6 两次的 sceneHash 相同（%s / %s）" % (hash1, hash2))
    check(not re.search(r"\[ERROR\]|VUID-", out), "B7 零 ERROR / 零 VUID")

    # ============================================================ C 边界
    print("\n[C] 边界：非法槽位 / 空槽位 / 大小写")
    rc, out = run(["--scene", SCENE, "--frames", "5", "--save", "--slot", "quik",
                   "--save-dir", SAVE_DIR])
    check("unknown save slot" in out, "C1 非法槽位当场拒绝（%d）" % rc)
    check("quik.vsave" not in slot_files(), "C1b 非法槽位不留文件")

    rc, out = run(["--load", "--frames", "5", "--slot", "slot3",
                   "--save-dir", SAVE_DIR])
    check("SAVE-READ-FAIL slot=slot3" in out and
          "op=load slot=slot3 ok=0" in out,
          "C2 读空槽位：报错且 ok=0")
    check(rc == 0, "C2b 读空槽位不崩（退出码 %d）" % rc)

    rc, out = run(["--scene", SCENE, "--frames", "5", "--save", "--slot", "QUICK",
                   "--save-dir", SAVE_DIR])
    check("SAVE-WRITE slot=quick path=" in out,
          "C3 大小写不敏感（QUICK → quick.vsave）")
    check("QUICK.vsave" not in slot_files(), "C3b 没写出第二个文件")

    safe_rmtree(os.path.join(BIN, SAVE_DIR))

    print("\n==== %s ====" %
          ("全部断言 PASS（%d 条）" % n_pass if not fails
           else "断言 %d 项 FAIL（过 %d 条）" % (len(fails), n_pass)))
    for f in fails:
        print("  -", f)
    if fails:
        return 1
    print("结果: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
