# B6 表情预设 + 眨眼/口型驱动（2026-11-01）

> 一句话：**在此之前引擎"能形变"，但不能"做表情"。** 这一层把一排原始
> blend-shape 滑条，变成"命名表情 + 自动眨眼 + 跟着语音张合的嘴"。

---

## 1 · 为什么需要这一层

morph 数据链早就通了：

```
FBX blend shape → MORPH 区块 → 顶点 morphBase/Count → 运行期 Mesh
→ assets::MorphRig（滑条名 → 每子网格每 target 权重）
→ ecs::MorphComponent::weights → GPU 形变
```

Inspector 上也有滑条，`morph.set` 命令也有。但**能形变 ≠ 能做表情**：

| 做不到 | 为什么 |
|---|---|
| 笑一次 | 要同时推 6 根滑条，**比例是美术定的**，玩家/程序现调调不出来 |
| 复用 | 同一份"微笑"要在十几个角色上一样 |
| 眨眼 | 那是**时间**驱动的，人手推滑条做不到 |
| 口型 | 要跟着**语音音量**走，随机或正弦更糟（嘴型与声音永远不同步） |

---

## 2 · 资产格式（`.expr.json`）

```json
{
  "version": 1,
  "name": "face",
  "presets": [
    { "name": "smile", "label": "微笑",
      "weights": { "mouth_smile": 0.8, "eye_squint": 0.3 } },
    { "name": "angry", "weights": { "brow_down": 0.9, "mouth_smile": 0.0 } }
  ]
}
```

| 字段 | 语义 |
|---|---|
| `version` | 必须 1。缺省可用但报一条警告（以后做 v2 迁移时靠它区分） |
| `name` | 资产名（缺省用文件名） |
| `presets[].name` | **键**，存档与脚本用，**不翻译** |
| `presets[].label` | 面板显示名，可翻译。空 → 用 `name`；**空时不写进文件** |
| `presets[].weights` | `通道名 → 权重 [0,1]`。**按名字**，不按 target 下标 |

### 三条语义（都有自检盯着）

1. **按名字而不是下标**。`MorphRig` 的下标是"建表顺序"（子网格序 × channel 序），
   重烤一次 FBX 就会变；资产要跨模型复用。
2. **未知通道是警告不是失败**。同一份脸挂到不同角色上，"这个角色没有
   `brow_angry`"是**正常**的 —— 但要报出来，否则"笑起来只有嘴动"会被当 rig 的 bug 查。
   Inspector 上直接显示"N 个通道在本模型上没有对应滑条"。
3. **越界钳到 [0,1] 且报**。静默钳位会让"我写 1.5 想夸张一点"与 1.0 完全一样。

> 现成的示例：`assets/expressions/alicia_demo.expr.json`。
> ⚠ Alicia 的 FBX 里通道本来就叫 `target_0..48`（49 个唯一名），所以那份
> 示例只能用 `target_N`。美术改过名的角色换成语义名即可，格式不变。

---

## 3 · 运行时语义

`ExpressionComponent`（`ecs/Components.h`）+ `anim::tickExpressions`（每帧）。

### 3.1 只写"自己拥有的通道" ⚠⚠

驱动与 Inspector 滑条写的是**同一张** `MorphComponent::weights`。所以驱动
每帧只碰三类通道：

1. 当前预设里列出的
2. 眨眼通道
3. 口型通道

其余**原样不动** —— 手动推的 `head_tilt` 不会被抹成 0。
反过来，**上一帧拥有、这一帧不再拥有的通道必须显式写 0**（换预设时旧表情
要归零），所以运行时存一份 `ownedChannels`。

> ⚠ 归零只做**一帧**：`ownedChannels` 存的是"本帧活跃"的通道，不含补 0 的。
> 把它们留在里面的话，清空预设之后每帧都往那根通道写 0 —— 用户再推滑条
> 就被抹掉。这一条有专门的判据（自检 `E5c`）。

### 3.2 强度是乘性

`有效权重 = clamp(预设值 × intensity, 0, 1)`。
`intensity = 0` 回到中性脸，但**不清**手动通道。

### 3.3 眨眼是加性的

`眨眼通道 = clamp(预设值 + blinkWeight(phase), 0, 1)`。
预设里已经写了 0.3 的闭眼，眨眼时要整只眼闭上，不是回到 0.3。

曲线三段（`blinkWeight`，纯函数）：

```
0 ──close──► 1 ──hold──► 1 ──open──► 0
```

两段过渡用 **smoothstep**（线性闭合会让眼睑像机械闸门）。
三段用时都 ≤ 0 → 返回 0（退化配置不许把脸定格在闭眼）。

**抖动是确定性的**（LCG，种子来自实体 id）：用 `rand()` 的话自检无法断言
"第 N 次眨眼的间隔落在 [0.85,1.15]×interval"，而且两次运行不同。
首帧按实体 id 抽相位，避免全场景同时眨眼（一眼就看出是程序）。

### 3.4 口型跟真实音量包络

输入是 **Voice 总线的 RMS 电平**（`Mixer` 在混块里算，`AudioSystem::voiceLevel()` 转发）：

- 为什么不用全局 RMS：一句轻声台词配一段响 BGM，全局 RMS 照样很大 → 嘴一直张着。
- 为什么不用"在播就张 0.8"：拖长的元音和急促的爆破音会得到一模一样的嘴，
  比不做更出戏。
- 平滑是**指数**的（帧率无关）：攻击 0.02 s、释放 0.09 s。线性逼近的收敛速度
  依赖 dt，60 fps 与 144 fps 下"口型韵律"会不一样 —— 而韵律是唯一能被人眼检验的东西。
- 没有声音时电平 0 → 自动闭嘴。设备没开（null 后端）同理。

---

## 4 · 怎么用

### 编辑器

1. 选中一个带 blend shape 的角色 → Inspector **Morph Targets** 下面有 **Expression**。
2. 第一次要点 **Add expression driver**（⚠ 不自动挂：默认 `blinkEnabled=true`，
   自动挂会让老场景里所有角色立刻开始眨眼，违反"打开旧场景画面逐像素不变"）。
3. `Preset asset` 填 `expressions/<名字>.expr.json` → 下拉里就能选表情。
4. `Blink` 开关 + 间隔 + **Blink now**（立即眨一次）。
5. `Lip sync` 开关 + 增益；右边 `level` 实时显示**驱动内部**那个包络
   （不是音频瞬时值 —— 看到的数就该是嘴张的幅度）。

### 控制台

```
expr.list [实体名]              # 列出可用预设
expr.setpreset smile [实体名]   # 切表情（none = 回到中性）
expr.blink [实体名]             # 立即眨一次
expr.state [实体名]             # 当前状态 + 拥有哪些通道
```

`expr.setpreset` 与面板下拉走**同一条路**（`anim::setExpressionPreset`），
所以命令跑通 = 面板跑通。日志彼此同源是刻意设计的（见 MEMORY "日志必须与执行同源"）。

---

## 5 · 存盘

`scene.json` 里新增 `expression` 块（**组件块顺序：mesh → material → skinned →
morph → expression → animator**）：

```json
"expression": {
  "preset": "expressions/alicia_demo.expr.json",
  "current": "smile",
  "intensity": 0.6,
  "blink": { "enabled": true, "interval": 2.5 },
  "lipSync": { "enabled": true, "gain": 1.5 }
}
```

**全部字段非默认才写**（与 `blend` / `blendCurve` / `camera` / `shots` 同一条纪律）：
一个刚加组件、什么都没改的实体存回去**不长键**，否则整个资产库一存盘就是一片脏 diff。
预设资产内容（`set`）**不进场景** —— 那是资产不是实例数据。

---

## 6 · 判据

| 层 | 位置 | 条数 |
|---|---|---|
| 数值内核 | `Game.exe --selftest expression`（`ExpressionSelfTest.cpp`） | **49** |
| 端到端 | `tools/verify_expression.py` | A/B/C/D 四组 |

自检五组：

- **A 眨眼曲线**：三段边界 + 中点 + 1/4 处（`smoothstep(0.25)=0.15625`，线性会得 0.25）
  + 睁眼段"同一段里后一个采样更小" + 超时 + 退化配置
- **B 解析**：正常 / 缺 version / 重名（先到赢）/ 越界钳位 / 非数字跳过 /
  空 weights / 语法错 / `presets` 不是数组 / version≠1
- **C 往返**：`toJson → parse` 逐字段相等；`label` 为空不长键；权重顺序稳定
- **D rig 适配**：`checkAgainstRig` 去重报缺失
- **E 驱动**：强度乘性 / **手动通道不被抹** / 换预设旧通道归零 / **归零只做一次** /
  dt=0 不推进 / 完整眨一次 / 口型包络起落 / 无 rig 不崩

端到端四组：

- **A** 自检全绿（fail=0、pass≥30、五段标题都在）
- **B** 编辑器（不进 Play）：`SCENE-EXPR ok=1`、八个 `INS-EXPR-*` 锚点、
  `EXPR-LIST` 三个预设、`EXPR-STATE` 切换前后、`EXPR-SET ok=0` 报未知预设、
  存盘只写非默认（`lipSync` 全程默认 → 文件里**没有**这个键）
- **C** 进 Play（`MYVK_EDITOR_PLAY=1`）：`owned>0`（驱动真的在跑）、
  `count>=3`（自动眨眼）、`MYVK_EXPR_LOG` 打的 `EXPR-BLINK` 行
- **D** 老场景（没有 `expression` 块）：不许建组件，`expr.state` 如实报 `no-component`

⚠ **C 组不能省**：A 组是手动调 `tickExpressions`，那一条路径上"Play 每帧会不会
调它"一行都没测到。

⚠ **不做像素判据**：单根滑条推到 0.6 通常只改几十个像素，那个量级低于 1080p 的
噪声；像素留给 `verify_morph_gpu` 那条已经稳定的链。

---

## 7 · 已知边界

- **D2 口型同步（按音素）没做**：现在是"音量 → 张嘴幅度"，能对上"在说话"，
  对不上"在说哪个字"。要上音素得先有配音的时长表。
- **B6 表情只驱动 morph**，不驱动纹理/材质切换（脸红、泪光那类要等 A 组的面部材质参数）。
- **Inspector 的滑条仍是原始通道**：`target_0..48` 这种名字对用户不友好，
  语义名只能靠美术在 DCC 里改名 —— 引擎侧不该猜。
