# `config/` —— 运行期配置（E03 设置层级）

这个目录里的东西是**运行期状态**，不是源码。仓库里只放格式说明和一个示例，
真正的文件是程序在用户机器上写出来的。

## 五个作用域（从最泛到最具体）

| 层 | 文件 | 谁写 | 说明 |
|---|---|---|---|
| Editor Preferences | `config/render_prefs.json` | 用户手动 / `MYVK_RENDER_PREFS` | 跨项目："我这台机器就是关着 SSAO 干活" |
| Project | `config/render.json` | Engine 面板的 **Export current** | 跟着项目走的那一份 |
| Scene | 场景 JSON 内嵌块 | — | 一个关卡自己的调色 |
| Object | 实体组件 | — | 单个物体的轮廓光更强 |
| Camera | 相机组件 | — | 特写机位用长焦景深 |

读的时候**从最具体往最泛回退**：Camera → Object → Scene → Project → Preferences
→ 引擎默认。一层都没有 = 出厂默认，**与这个功能存在之前逐像素一致**。

## 文件格式

顶层就是一个扁平对象，键是 `"<featureId>.<paramId>"`（参数 id 的唯一清单在
`engine/render/FeatureRegistry.cpp` 的 `kFeatures[]` 里，Engine 面板的控件也
是从同一份表长出来的）：

```json
{
  "version": 1,
  "shadow.cascades": 2,
  "post.tonemap": "Reinhard",
  "ssao.strength": 1.5,
  "cg.lift": [0.5, 0.25, 0.125],
  "pom.fade": [10, 26]
}
```

值的形状由参数类型决定：

| 参数类型 | JSON | 例 |
|---|---|---|
| Bool | `true` / `false`（也接受 `0` / `1` / `"on"` / `"off"`） | `"shadow.enabled": false` |
| Int | 整数 | `"shadow.cascades": 2` |
| Enum | 整数下标**或选项名** | `"post.tonemap": "Reinhard"` |
| Float | 数字 | `"ssao.strength": 1.5` |
| Color3 / Vec3 | `[x, y, z]` | `"cg.lift": [0.5, 0.25, 0.125]` |
| Float + aux（"一个键写两个字段"） | `[a, b]` | `"pom.fade": [10, 26]` |

以 `_` 开头的键和 `version` / `$schema` 会被忽略（留给注释）。

## 三条容易踩的规矩

1. **没有文件时程序不生成文件。** 只有"用户真的改了参数"（退出时落盘）或
   点了 **Export current** 才会写。否则一次手动会话就会在仓库里留下一个 json，
   把所有像素基线悄悄换掉。
2. **自动化运行不读默认路径、写一律拒绝**（`MYVK_FRAMES` / `MYVK_NO_WINDOW_SAVE`
   存在即为自动化）。要测这份文件请用显式路径 `MYVK_RENDER_SETTINGS=<path>`。
3. **坏文件不会让程序崩。** 语法错 → 一条 WARN + 整层忽略（回落默认）；
   单个键的类型不对/越界 → 只拒那一个键，同文件里其它键照常生效。

## 校验

`tools/verify_settings_layers.py`（已进 `run_regression.py` 的 `DEFAULT`）。
无窗口自检入口：`Editor.exe` + `MYVK_RENDER_SETTINGS_TEST=1`。
