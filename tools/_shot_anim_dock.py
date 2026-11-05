"""出图用的小工具（不作为回归用例）：截一张"动画坞 + 完整视口"的对照图。

⚠ 为什么单独写一个而不是塞进 `verify_anim_dock.py`：那个用例的判据全是
  **矩形**（日志锚点），截图只会让它在慢机器上更容易抖。出图是"给人看的"，
  跟"给闸门看的"分开 —— 这也是本仓 `_probe_*.py` 的既有惯例。

用法：
    python tools/_shot_anim_dock.py            # 摄影表页
    python tools/_shot_anim_dock.py cine       # 顺便切成运镜页再截一张
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from capture_window import capture_client                     # noqa: E402
from verify_anim_dock import OUT_DIR, Session, TABS           # noqa: E402


def shot(sess, name):
    img, ok = capture_client(sess.hwnd)
    path = os.path.join(OUT_DIR, name)
    img.save(path)
    print("%-34s ok=%d  %s  %dx%d" % (name, ok, path, img.width, img.height))
    return path


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    with Session(opened=["dope"]) as s:
        shot(s, "animdock_dope.png")
        print("ANIMDOCK-FULL", s.rect("ANIMDOCK-FULL"))
        print("VP-RECT      ", s.rect("VP-RECT"))
        if len(sys.argv) > 1:
            tab = sys.argv[1]
            r = s.rect("ANIMDOCK-TAB-%s" % tab, fresh=False)
            if r:
                s.click((r[0] + r[2]) * 0.5, (r[1] + r[3]) * 0.5, settle=1.4)
                shot(s, "animdock_%s.png" % tab)
                print("tab after click:", "cine" in s.text())


if __name__ == "__main__":
    main()
