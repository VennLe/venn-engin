# 默认操控 · 真实跳跃 · 脚踩地形 · 角色预览（P6）

> 2026-10-22 · 承接 `docs/character_locomotion.md`（P4）与 `docs/player_feel.md`（P5）
> 代码：`engine/core/InputMap.cpp`、`engine/physics/CharacterController.*`、
> `runtime/PlayerController.cpp`、`editor/EditorApp.cpp`、`editor/CharacterWindow.*`、
> `engine/render/Renderer.cpp`、`assetio/ProcAnim.cpp`
> 验收：`tools/verify_player_jump.py`（10 条）、`tools/verify_player_ground.py`（8 条）、
> `tools/verify_char_preview.py`（12 条）

---

## 0 · 一句话

**WASD 走、右键长按跑、空格跳，全是出厂设置**，不用在角色面板里绑任何键；
跳起来 **0.70 m / 0.75 s**，空中按方向键能改落点、轨迹是抛物线；
**脚永远踩在地形表面上**，不允许陷到地面以下；
角色面板的预览视口**真的会播动画**，带栅格地面和 gizmo，胶囊看得见、点得中，
角色和胶囊的位置・旋转・大小都能分别调。

---

## 1 · 出厂键位（不需要绑键）

| 动作 | 键 | 在哪定义 |
|---|---|---|
| 前后左右走 | **W / S / A / D** | `InputMap.cpp` 的 `MoveForward` / `MoveBack` / `MoveLeft` / `MoveRight` |
| 跑（长按） | **鼠标右键**（+ 手柄 LB） | `InputMap.cpp` 的 `Sprint` |
| 跳（敲击） | **空格**（+ 手柄 A） | `InputMap.cpp` 的 `Jump` |

角色的 Walk / Run / Jump 三段动画**不需要绑键**：它们是状态机
（`engine/animation/Locomotion.*`）自动选的 —— 走 → `Walk`、跑到阈值以上 →
`Run`、离地 → `Jump`。角色面板的 Animation 页每行行尾那个徽标（`WASD` /
`RMB` / `Space`）是**只读提示**，不是按钮。

> ⚠ `config/input.json` 是**存档优先**：老工程里存过 `Sprint = LeftShift`，
> 出厂值改了也不会覆盖它。要吃到新默认值，在输入设置里点一次"恢复默认"。
> 角色面板改的是 `AnimKeyMapComponent`（场景数据，"按 K 播 Jump"那种），
> 与 `Sprint` 这类动作绑定是两套东西，不要混。

---

## 2 · 跳跃为什么是这组数

| 量 | 值 | 怎么来的 |
|---|---|---|
| 起跳初速 `v₀` | **3.70 m/s** | 按身高缩放：`sqrt(2·g·0.39·h)`，夹到 [3.0, 5.0] |
| 跳跃高度 | **0.70 m** | `v²/2g`（实测 0.673 m，差值是 dt 离散化） |
| 滞空时间 | **0.754 s** | `2v/g`（实测 0.73 s） |
| 空中操控 | `airControl = 0.45` | 空中按方向键能改落点，但不如地面灵活 |
| Jump clip 时长 | **0.75 s** | 与滞空对齐，落地那一帧正好是缓冲段 |

**重力只有一个来源**：`physics::kGravity`（`CharacterController.h` 顶层常量）。
编辑器 Play 里没有 `PlayerController`（那是 runtime 概念），所以那边**手写了
同一套抛物线**（`pc->velocityZ` + `physics::kGravity`）——
改一处必须改两处，`verify_player_jump.py` 的 P7 就是盯编辑器这条腿的。

### 2.1 "双脚离地"的跳跃姿势

模型是 `samba_dancing.fbx` 经 AutoRig + ProcAnim **程序化烤**出来的四段动画，
没有可以直接换的 Mixamo clip。所以走的是**重写 `poseJump`** 这条路：
去掉开头的蹲下蓄力，相位改成 蹬伸(0–0.12) → 收腿(0.22–0.48) → 最高点 →
伸腿(0.62–0.80) → 落地缓冲(0.84–1.0)，大腿前屈 58°→78°、膝屈 82°→104°。

关键是给了个**可断言的数**：`PROCANIM-JUMP jumpFootLift`（实测 0.3714 m）
= 空中相里脚底相对髋部抬起来多少。理由很实际 —— 背面静帧里"腿收了"和
"腿没动"几乎分不出来，没有这个数就只能靠眼睛猜。

---

## 3 · 脚不许踩到地形下面

### 3.1 症状

用户报"角色踩在地形上时脚底会陷到地面以下"。探针把这件事拆成三个数：

```
GAME-FEET f=506 feetZ=18.0360 surfZ=18.1886 gap=-0.1525
```

`feetZ` = 实体原点（**实体原点就是脚底**），`surfZ` = 高度场在 (x,y) 的插值
（与贴地用的同一个 `physics::heightAt`），`gap < 0` 就是陷进去了。

### 3.2 根因：接触窗口比"一帧走多远"还窄

`CharacterController` 的贴地判定是"竖直射线打到地面的距离 − 应有距离"落在
一个窗口里才算踩到。窗口原来是死的 **±0.02 m**，但一帧的位移能比它宽：

| 情形 | 一帧跨过的竖直距离 | 后果 |
|---|---|---|
| 快速下落（vz −4.7、dt 1/60） | 0.078 m | 整帧跨过窗口 → 下一帧 `excess = −0.05` 被判成"这不是地面" → 重力继续 → **一路穿到 −277 m** |
| 跑上坡（水平 0.15 m/frame、46° 坡） | 0.155 m | 同上 → `ground` 翻 0 → **永远贴不回去** |

"在地形内部"是**不可恢复**状态：竖直射线从胶囊中心往下打不到任何东西
（地面在头顶），控制器再也不可能自己贴回地面。所以表现是"越掉越深"。

> 一条看不见的因果：P5 为了修"跳跃像被吸下来"，加了 `excess > 0.02 && vz < −1.5
> → 不算地面`。这条**同时**让"快速下落"永远落不了地 —— 修 A 弄坏 B 的典型。

### 3.3 修法：容差按"这一帧可能跨过的距离"给

```cpp
const float hStep  = glm::length(hv) * dt;                       // 水平步长
const float maxRise = hStep * sqrt(1 - cosSlope²) / cosSlope;    // 最陡可行走坡上的等效抬升
const float sweep  = max(0, -vz) * dt + maxRise + 0.02f;         // ← 扫掠窗口
if (excess < -sweep || excess > snap) return false;
...
if (excess < 0.0f) pos.z -= excess;    // 踩进去了就抬出来（最多一帧步长）
```

这就是一次**扫掠判定**：只要这一帧的位移能从地面上方跨到下方，就必须算踩到。
抬出来的量最多一帧步长，所以不会把人弹飞。

### 3.4 第二道保险

`PlayerController::update` 里还有一条兜底：`!onGround` 且脚底低于地表
**超过 0.25 m** → 抬到地表 + 打 `GAME-PLAYER-RESCUE` WARN。
正常贴地误差是毫米级，超过 25 cm 只可能是真的陷进去了；打日志而不是静默修补，
是为了**下次再出这事能查到**。

### 3.5 预览里的同一件事

角色面板的预览场景里，模型 pivot 在几何中心、脚在 −0.902 m。预览根留在原点
的话，那层栅格地面会从**大腿上穿过去**（"人半身埋在地里"）。所以
`rebuildPreviewScene` 把预览根抬 `footLift = −boundsMin.z`，脚底 = 栅格平面 = z 0，
胶囊也从 z 0 起，取景目标跟着抬。

---

## 4 · 预览视口：能看动画 / 有栅格 / 能点选

### 4.1 动画真的在动（`syncPreviewSkinning`）

症状："时间轴在走、进度条在跑，模型一动不动"。

根因：预览场景**没有 `AnimatorSystem`**，只有渲染层一条兜底
`if (sk.jointMatrices.size() != n) 重新取姿势`。而实例化时
`jointMatrices` 已经被填满 `jointCount` 个 —— 条件永远不成立，于是渲染的
永远是实例化那一刻的静止姿势。

修法：`syncPreviewSkinning` 每帧把面板自己算出来的 `m_jointMats` 推进
`sk->jointMatrices`（先按骨架节点数对齐长度）。

> 这条只能用**像素**证明。`verify_char_preview.py` 的 P3 是隔 0.45 s 截两张、
> 数预览矩形内的差异像素（阈值 0.4%）。实测 24%，而且差异只集中在人物所在
> 的中段行、上下两端为 0 —— 这才排除了"整块画面在闪"的误判。

### 4.2 栅格地面 + gizmo

预览栅格是**独立参数**（`Renderer::previewGridEnabled()` /
`previewGridSettings()`），默认开：`extent 4 m`、`minor 0.10 m`、
`major 0.50 m`、`fade 2.4→4.0`、`alpha 0.50`。工具栏有个 `Grid` 复选框。
gizmo 是叠加层的坐标轴（X 红 / Y 绿 / Z 蓝 + 末端方块 + 字母），画在**选中
对象**的原点上：模型 → 包围盒底（脚底），胶囊 → 胶囊脚底。

### 4.3 点选规则：点线选胶囊、点身体选模型

第一版用"射线 vs 两个 AABB，取更近的那个"，结果**永远选不到模型**：
胶囊天生比人粗（`r = 0.16h`，人正面的厚度只有 ±0.18 m），相机从正面看过来
永远先撞胶囊的 AABB。实测现象是反的 —— 点身体中心选到胶囊、点脚底选到模型。

现在的判定分三层：

1. 鼠标离胶囊**线框** ≤ 9 px → 胶囊；
2. 射线命中模型包围盒 → 模型；
3. 射线命中胶囊包围盒 → 胶囊（兜底，线框细，点进去也算）。

第 1 步量的是 `m_capScrSegs` —— **overlay 画线时顺手记下来的真实屏幕线段**。
不用"到中轴的垂距 ≈ r"：默认机位下 `off = (37, 29)`，母线到中轴的**垂距**
只有 37 px 而不是 47 px，按 47 去算会差 10 px，点在线上却判定 12 px 远。

> ⚠ 拾取必须带 `m_capsulePos`。漏了它的时候胶囊整体高 0.9 m（模型 pivot 在
> 几何中心），表现就是"点脚底能中、点胸口点不中"，查了很久。

### 4.4 位置 / 旋转 / 大小

视口下方那条控件：`Model / Capsule` 单选 + `Show capsule` + `Gizmo` 复选框，
然后 `Pos (x/y/z)` / `Yaw` / `Size`（模型是一个等比缩放；胶囊是 `R` 半径 +
`HH` 半高两个字段，总高 = `2 × (HH + R)`）。
`Pos` 是**只在预览里平移**，不动玩家实体 —— 胶囊要挪位置该去改
`CharacterControllerComponent`。

---

## 5 · 七个坑（都在这轮踩过）

1. **二进制比源码旧**：锚点日志没打出来，第一反应是"日志没写"，实际是
   `Editor.exe`（16:15）比 `CharacterWindow.cpp`（16:18）旧。
   **改完先看时间戳，再怀疑代码。**
2. **"垂距 ≈ r" 不是线框位置**（见 4.3 第 1 步）。画什么就量什么。
3. **AABB 取近者让模型永远点不中**（见 4.3）。重叠物体不能用深度排序。
4. **拾取忘了 `m_capsulePos`**：症状是"高 0.9 m 的幽灵胶囊"。
5. **Space 语义冲突**：编辑器里 Space 原本是 Play/暂停。Play 态下必须归
   玩家跳跃，否则"跳一下游戏被暂停"。现在只在 `isEditing()` 时才切 Play。
6. **预览模型半身埋进栅格**：模型 pivot 在几何中心，根没抬。
7. **验收采样窗口混进自由落体段**：出生点在离地 1.4 m 的空中，而且
   **点击视口之前玩家是冻结的** —— "站着不陷"必须先等落稳再采样。

---

## 6 · 验收

| 脚本 | 条数 | 最有代表性的一条 |
|---|---|---|
| `tools/verify_player_jump.py` | 10（J0~J9） | J2 高度 0.55~0.90 m、J3 滞空 0.60~0.95 s —— **数值**判据，不是"看着像" |
| `tools/verify_player_ground.py` | 8（G0~G7） | G4 跑步（含下坡）全程 `gap ≥ −0.02`，且 G5 证明这段路真在坡上 |
| `tools/verify_char_preview.py` | 12（P0~P8） | P3 两张截图的像素差异 ≥ 0.4% —— 唯一不靠日志的判据 |

三条都登记进了 `tools/run_regression.py` 的 `DEFAULT` 与影响分析表
（`check_regression_map.py` 通过）。

实测值（2026-10-22）：

```
J2 高度 0.673 m（理论 0.70）   J3 滞空 0.73 s（理论 0.754）
J6 空中位移 3.74 m             J7 跑 5.89 m/s（走 3.0）
G3 走路 gap ∈ [0.0000, +0.0002]   G4 跑步 gap ∈ [+0.0002, +0.0086]
P3 预览像素差异 24.2%          P7 EDIT-JUMP v0=3.72 expectH=0.70m
```

---

## 7 · 还没做

- **IK**：脚踩斜坡时脚掌不贴面（胶囊贴合 ≠ 脚掌贴合）。
- **根运动**：位移仍由控制器算，clip 里没有根位移。
- **手指骨 / 分方向移动动画**：只有一段 `Walk`，前后左右共用一个循环。
- **`.anim.json` 控制器资产**：状态机目前是代码里的固定四态，没有可编辑的
  状态图。
- **胶囊 Yaw 对点选没有影响**：胶囊是旋转对称的，转它看不出差别 —— 控件留
  着只是为了"位置/旋转/大小三个自由度都在"，别以为是坏的。
