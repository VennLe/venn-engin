#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/anime_style.py —— 二次元风格的**唯一**参数源（天空 / 光照 / 常用色）。

为什么要有这个文件：天空那套数字（`environment` 块里 18 个字段）现在被
**两个**烘焙脚本同时需要 ——
  · `tools/make_anime_town.py`  → `assets/scenes/anime_town.json`
  · `tools/gen_templates.py`    → `templates/*/assets/scenes/main.json`
（新建工程第一眼看到的那张场景）。

各写一份的后果不是"重复几行"，而是**改一处忘一处**：小镇的云量调高了、
新建工程还是老样子，然后有人问"为什么新建游戏和示例场景的天空不一样"。
所以：这里定义，两边 import，谁也别再抄数字。

字段含义与默认值见 `engine/render/SkyParams.h`：

| 字段 | 含义 |
|------|------|
| `zenith` / `horizon` / `ground` | 三段底色（线性空间，不是 sRGB） |
| `cloudCoverage` | 云量 0..1（0.5 约等于"晴天带几朵"） |
| `cloudThickness` | 云层厚度：越大越"堆"、缝隙越少 |
| `cloudScale` | 云的尺度：越大朵越碎 |
| `cloudHeight` | 云层高度（米），只影响视差与地平线处的透视压缩 |
| `cloudSpeed` | 云的漂移速度（每秒），0 = 定住 |
| `cloudShadow` | 云自身的明暗落差（0 = 平贴贴纸） |
| `cloudEdgeSharpness` | 边缘硬度：二次元要**偏硬**，写实才要 soft |
| `exposure` | 天空的曝光系数（只作用于天空，不动场景） |
| `starAmount` / `starBrightness` | 星点密度 / 亮度（白天全 0） |
| `sunDirection` | **指向太阳**的单位向量（Z-up） |
| `sunIntensity` / `sunAngularRadius` | 太阳盘亮度 / 张角（弧度，0.017 ≈ 真实太阳） |
| `horizonGlow` | 地平线附近的暖色辉光强度 |

⚠ 这里**只管天空**，不管场景里的方向光（`light` 块）。两者要"看起来是
同一个太阳"，但那是场景作者的事：模板的 `light.direction` 与这里的
`sunDirection` 目前**故意不同向**（模板光是从右上打下来的近景主光，
而太阳在画面左侧偏后），因为模板要保证地面有明确的明暗关系。改任何一个
之前先看一眼两者的方向，别让影子朝着太阳。
"""

# 晴天、略多云。数值与 `SkyParams::clearDay()` 同源，只把云量与
# 地平线暖色往上调一点 —— 空场景里天空占画面的比例很大，云太少会显得
# 像一块塑料布。
ANIME_DAY_ENV = {
    "zenith": [0.16, 0.36, 0.82],
    "horizon": [0.60, 0.76, 0.93],
    "ground": [0.40, 0.48, 0.36],
    "cloudColor": [1.0, 1.0, 1.0],
    "cloudCoverage": 0.52,
    "cloudThickness": 0.94,
    "cloudScale": 1.25,
    "cloudHeight": 900.0,
    "cloudSpeed": 0.012,
    "cloudShadow": 0.58,
    "cloudEdgeSharpness": 0.60,
    "exposure": 0.92,
    "starAmount": 0.0,
    "starBrightness": 0.0,
    "sunDirection": [0.522, -0.326, 0.788],
    "sunIntensity": 1.0,
    "sunAngularRadius": 0.017,
    "horizonGlow": 0.30,
}


def anime_day_env():
    """返回一份**深拷贝**。

    ⚠ 必须复制：调用方会往返回值里塞东西（或者干脆整块替换），
      共用同一个 dict 会让"改了模板的天空"顺手把小镇也改了，
      而且症状只在**下一次重新生成资产**时才出现。
    """
    import copy

    return copy.deepcopy(ANIME_DAY_ENV)
