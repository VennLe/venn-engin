import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import launcher_kit as kit
import verify_multiselect as V

work = kit.fresh_dir(V.OUT + "_e")
scene = V.scene_four(os.path.join(work, "scene_e.json"))
proc, tap, hwnd = V.launch(scene, select="CubeA")
print("hwnd", hwnd)
time.sleep(4.0)
with tap.lock:
    lines = list(tap.lines)
print("lines", len(lines))
for l in lines:
    if "HIER" in l or "RECT" in l or "SELECT" in l:
        print(" ", l.rstrip()[:160])
try:
    V.force_foreground(hwnd)
    time.sleep(0.5)
finally:
    kit.shutdown(proc, hwnd)
