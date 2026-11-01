#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""外部资产前置检查 —— 缺了就 `结果: SKIP:`，而不是报红。

## 为什么需要它

有一批角色用例（蒙皮权重 / 骨骼编辑 / Pose Mode / 角色详情窗口）用的是
`assets/models/characters/animate_girl.fbx`。这个模型是**从外部工程导进来
的**，`.vmesh` 里把贴图引用写成了**绝对路径**：

    E:/dev/UE-project/UE_Basics/Content/anime-girl/source/anime-girl/Textures/…

引擎加载时会 `[ERROR] Texture load failed: … (can't fopen)`，而这些用例里
几乎每一条都有一条"零 [ERROR]"的收尾断言 —— 于是**贴图所在的盘符一变，
四条用例就整片变红**，而代码一行没错。2026-10-31 实测就是这种局面
（E: 还在，但 `E:/dev/UE-project` 已经被挪走了）。

把它们改成"红"的代价：每次全量回归都要人工回忆"这四条是不是又是贴图"。
改成"跳过"的代价：少测四条 —— 但**看得出来少测了**，而且跳过的理由写在
日志第一行。

## 判据为什么从 `.vmesh` 里扫

不要写死 `E:/dev/UE-project` —— 那样模型重烤一次（贴图挪进工程内）之后，
这条检查会**继续跳过**，而用例本该恢复。扫"模型里所有的绝对路径"就自洽：
路径存在就跑，不存在就跳；哪天贴图进了工程（相对路径），检查自动失效。

用法：

    from prereq import skip_if_external_missing
    skip_if_external_missing("assets/models/characters/animate_girl.fbx")
"""

from __future__ import annotations

import os
import re
import sys

__all__ = ["external_roots", "missing_external_roots",
           "skip_if_external_missing"]

# Windows 绝对路径（`X:/…` 或 `X:\…`）。模型里存的是正斜杠那种。
_ABS_RE = re.compile(rb"[A-Za-z]:[/\\][\x20-\x7e]{3,300}")

# 同一模型同一轮里会被问很多次（`--check` 之类的路径），缓存一下。
_CACHE = {}


def _model_files(model_abs):
    """`.fbx` / `.glb` 旁边通常还有一个 `.vmesh` —— 两个都要看。

    只看 `.fbx` 会漏：运行时优先读 `.vmesh`，贴图引用写在那里面。
    """
    out = [model_abs]
    stem, _ext = os.path.splitext(model_abs)
    for cand in (stem + ".vmesh", stem + ".glb"):
        if os.path.isfile(cand):
            out.append(cand)
    return [p for p in out if os.path.isfile(p)]


def external_roots(model_abs):
    """模型里引用到的**绝对路径目录**（去重、只留目录部分）。"""
    if model_abs in _CACHE:
        return _CACHE[model_abs]
    roots = set()
    for path in _model_files(model_abs):
        try:
            with open(path, "rb") as f:
                blob = f.read()
        except OSError:
            continue
        for m in _ABS_RE.finditer(blob):
            raw = m.group(0).decode("ascii", "replace")
            # 去掉紧跟在文件名后面的二进制垃圾：只截到最后一个已知扩展名
            cut = raw
            for ext in (".png", ".jpg", ".jpeg", ".tga", ".dds", ".bmp"):
                i = cut.lower().find(ext)
                if i > 0:
                    cut = cut[:i + len(ext)]
                    break
            else:
                continue        # 没有图片扩展名 → 不是贴图引用，忽略
            d = os.path.dirname(cut.replace("\\", "/"))
            if d:
                roots.add(d)
    _CACHE[model_abs] = sorted(roots)
    return _CACHE[model_abs]


def missing_external_roots(model_abs):
    """返回**当前不存在**的那些外部目录（空列表 = 前置齐备）。"""
    return [r for r in external_roots(model_abs) if not os.path.isdir(r)]


def skip_if_external_missing(model_rel, root=None):
    """前置缺失时打 `结果: SKIP: …` 并以 0 退出（**不是** FAIL）。

    ⚠ 退出码必须是 0：`run_regression.py` 只有在 rc==0 时才可能判 PASS，
      而 SKIP 走的是另一条路（`SKIP_RE`）。用非 0 退出会被当成真红。
    """
    base = root or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    model_abs = os.path.join(base, model_rel.replace("/", os.sep))
    if not os.path.isfile(model_abs):
        print("结果: SKIP: 模型不存在 %s" % model_rel)
        sys.exit(0)
    miss = missing_external_roots(model_abs)
    if not miss:
        return
    print("前置缺失：%s 引用的贴图目录不存在 →" % model_rel)
    for m in miss:
        print("    %s" % m)
    print("（模型是从外部工程导进来的，贴图引用写成了绝对路径；"
          "贴图不在时引擎会打 [ERROR]，本用例的『零 ERROR』断言就必然红 ——"
          "这是**环境**问题，不是被测代码的问题。）")
    print("结果: SKIP: 外部贴图根缺失（见上）")
    sys.exit(0)
