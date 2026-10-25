#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""一次性探针：看 6 个 .vmesh 在导入后 uv2 / tangent 到底有没有值。

只为决定 verify_vertex_format.py 的判据怎么写，不是正式用例。
"""
import json, os, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EDITOR = os.path.join(ROOT, "build", "bin", "Editor.exe")
WORK = os.path.join(os.path.dirname(ROOT), "_venn_verify", "vfmt_probe")

CASES = [
    ("models/anime_girl.fbx", "models/anime_girl"),
    ("models/characters/alicia.fbx", "models/characters/alicia"),
    ("models/characters/animate_girl.fbx", "models/characters/animate_girl"),
    ("models/thirdparty/samba_dancing.fbx", "models/thirdparty/samba_dancing"),
]

if os.path.isdir(WORK):
    shutil.rmtree(WORK)
root = os.path.join(WORK, "root")
os.makedirs(os.path.join(root, "scenes"))
ents = []
for i, (rel, stem) in enumerate(CASES):
    d = os.path.join(root, os.path.dirname(rel))
    os.makedirs(d, exist_ok=True)
    for ext in (".fbx", ".vmesh"):
        s = os.path.join(ROOT, "assets", stem + ext)
        if os.path.isfile(s):
            shutil.copy2(s, os.path.join(d, os.path.basename(stem) + ext))
    ents.append({
        "name": "M%d" % i,
        "transform": {"position": [i * 3.0, 0.0, 0.0],
                      "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0]},
        "parent": -1, "visible": True, "castShadow": True,
        "mesh": {"kind": "fbx", "path": rel,
                 "modelKey": "%s#vfmt%d" % (rel, i), "subMeshIndex": 0},
    })

scene = {"version": 1, "generator": "vfmt probe", "entities": ents}
with open(os.path.join(root, "scenes", "probe.json"), "w", encoding="utf-8") as f:
    json.dump(scene, f, ensure_ascii=False, indent=2)

env = os.environ.copy()
env.update({"MYVK_ASSET_DIR": root, "MYVK_EDITOR_SCENE": "scenes/probe.json",
            "MYVK_FRAMES": "60", "MYVK_NO_WINDOW_SAVE": "1"})
p = subprocess.run([EDITOR], capture_output=True, text=True, env=env, timeout=300)
out = (p.stdout or "") + (p.stderr or "")
for line in out.splitlines():
    if "VFMT" in line or "VMESH loaded" in line or "VMESH-FAIL" in line:
        print(line.strip())
print("exit=%d" % p.returncode)
