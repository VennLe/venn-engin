"""临时冒烟探测：确认 Pose Mode 的日志锚点真的会出来。
不登记回归，跑完就删。"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")

env = dict(os.environ)
env["MYVK_CHARACTER_WINDOW"] = "1"
env["MYVK_CHARACTER_MODEL"] = "assets/models/anime_girl.fbx"
env["MYVK_NO_WINDOW_SAVE"] = "1"
env["MYVK_EDITOR_POSE_SCRIPT"] = sys.argv[1] if len(sys.argv) > 1 else "mode:on;list;sample:0.0"

proc = subprocess.Popen([EXE], cwd=os.path.join(ROOT, "build", "bin"), env=env,
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        text=True, encoding="utf-8", errors="replace")
try:
    out, _ = proc.communicate(timeout=40)
except subprocess.TimeoutExpired:
    proc.kill()
    out, _ = proc.communicate()

lines = (out or "").splitlines()
want = ("CHAR-POSE", "POSE-RESOLVE", "POSE-SAVE", "POSE-LOAD", "POSELIB")
for l in lines:
    if any(w in l for w in want):
        print(l)
print("--- 总行数 %d ---" % len(lines))
