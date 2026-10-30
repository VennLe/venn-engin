# 相机运镜体系（Cinematography）

> 2026-10-30。对应代码：`engine/scene/CameraMoveKind.*`（动词表）、
> `engine/scene/CameraMove.*`（算子 / 光学 / 姿态基）、
> `engine/scene/CameraPath.*`（Follow Path，弧长参数化）、
> `engine/scene/CameraSystem.*`（每帧求值）、`editor/CinematographyPanel.*`（面板）。
> 判据：`Game.exe --selftest cameramove`（A~F 组）＋ `tools/verify_camera_move.py`。

---

## 一、四层参数，别混着调

运镜在别的软件里看着像"挪相机"，实际是四层互不相干的东西。这四层在本引擎
里落在四个不同的地方，混了就会出现"改了没反应"或者"改一处坏另一处"：

| 层 | 是什么 | 存在哪 | 谁来算 |
|---|---|---|---|
| **姿态** | 位置 + 朝向（yaw/pitch/roll）+ fov | `CameraPose` / `SeqCameraKey` | 关键帧、玩家控制器、路径 |
| **镜头** | 焦距、画幅、离轴、裁剪、景深、快门 | `CameraComponent` 的 lens 段 | `CameraSystem::apply` → `Camera` |
| **rig** | 注视目标、阻尼、手持、挂哪条路径 | `CameraComponent` 的 rig 段 | `CameraSystem::apply` 每帧 |
| **效果** | 景深 / 运动模糊 pass | `render::CameraEffectSettings` | 宿主（runtime）从镜头段换算 |

**判据**：`CINE-STATE` 一行把四层都打出来；`CAM-LENS` 单独打效果层换算后的
数字（`focus` / `aperture` / `blur`）—— 面板上填的 f/2.8 到底变成多虚，只有
这一行能给答案。

---

## 二、姿态的约定（写错一处镜头就朝反方向）

- **右手系、Z 轴朝上。**
- `yaw` 绕世界 +Z，`pitch` 正 = 相机在上方俯视。
- `forward = (−cosP·cosY, +cosP·sinY, −sinP)` —— `yaw = 0` 时看向 **−X**。
  这条与 `Camera::angleDir()` 互为反号（那边指向相机背后），**改一处必须改两处**，
  否则 Dolly 会变成往后推。
- `right = normalize(cross(forward, worldUp))`，`yaw = 0` 时 = +Y。
- `roll` 绕**视线**转，正 = 相机自身顺时针（画面里地平线右端下沉）。
- `shiftX / shiftY` 是**传感器比例的离轴偏移**（Blender 口径），0.1 = 成像中心
  挪 10% 传感器尺寸。

### 新通道的默认值必须"逐位回到加它们之前"

`roll / shiftX / shiftY` 三个字段的默认值都是 0，且：

- `Camera::viewMatrix()` 在 `roll == 0` 时走**原样的 `lookAt`**（不构造旋转
  矩阵再乘）；
- `Camera::projMatrix()` 在 shift 全 0 时**一个元素都不碰**。

这不是洁癖：老场景加载进来，view/proj 矩阵必须与上一版**逐位相同**，否则
所有渲染基线截图一起飘。自检 C1/C3/C6 就是钉这条（`matBitEqual`）。

---

## 三、动词是**算子**，不是状态

`applyCameraMove(pose, spec)` 全部是增量：

```
Pan       yaw   += amount          (度)
Tilt      pitch += amount          (夹 ±89°)
Roll      roll  += amount          (归一到 (−π, π])
Dolly     pos   += forward × amount (米)
Truck     pos   += right   × amount (米)
Pedestal  pos.z += amount           (米)
Orbit     位置绕 pivot 水平公转 + 转完看向 pivot
Crane     位置绕 pivot 在竖直平面内公转（朝向不自动跟）
```

**为什么必须是增量**：`Pan(+15°)` 的意思是"在**当前**朝向上再摇 15°"。
写成"把 yaw 设成 15°"在单次操作里看不出差别，连推两次立刻暴露（自检 A1：
推两次 = 30°，不是 15°）。

`amount == 0` 时**逐位不变**（`if (spec.amount == 0) return;` 在最前面）——
`cameramove_noop` 钉这条。

### Orbit / Crane 的支点

不给支点时**不退化**成"什么都不做"，而是用"平视方向 3 米"兜底（见
`flatForward`）。为什么不干脆拒绝：兜底给的是一个**可预期**的结果（就在
视线前方绕），而"点了没反应"会让作者以为是 bug。面板上这两个动词取
"视线前方 5 米"当支点 —— 那是"环绕面前这个东西"的默认意图。

---

## 四、三个动画方法

### 4.1 关键帧（方式 A）—— 已经在时间轴里

`SeqCameraKey` 早就有 `Ease` 与**归一化贝塞尔手柄**（见 `SequenceAsset.h`
的注释）：`x ∈ [0,1]` 是段内时间比例、`y` 是进度比例，四通道（position /
yaw / pitch / fov）**共用同一个时间重映射**。这是对的抽象：一个段只有
一条"节奏"，位置先到位而焦距还在动才是 Dolly Zoom 那种效果。

- 只打**转点**：起幅、中段、落幅。每帧都打 = 线性 = 监控摄像头。
- 缓入缓出用手柄，不用"再加十个键"。

### 4.2 Follow Path（方式 B）—— `CameraPath` + `CameraPathComponent`

```
相机实体 . pathGuid ──▶ 路径实体 . CameraPathComponent{ points, closed, progress, speed,
                                                        lookAlong, yawOffsetDeg }
```

**⚠⚠ 核心是弧长参数化，不是贝塞尔。** 贝塞尔的参数 `t` 不是匀速的：控制点
一密，这一段就窜一下。直接 `sample(u = t)` 得到的运镜在拐弯处一顿一顿，
而作者第一反应永远是"关键帧太少"。

`CameraPath::sample(u)` 里 u 走的是**走过的距离占全长的比例**（16 步/段的
弧长表）。同一段弧长步长下，实际位移的离散度：

| 实现 | 40 步位移离散度 |
|---|---|
| 按参数（`t = u`） | 218%（人造对照：手柄 0.1 m vs 3 m） |
| 按弧长 | **< 3%**（自检 E1 实测值见 `CAMMOVE-TEST` 输出） |

**⚠ 判据曲线的选择**：第一版用折线点 + 自动手柄做这条判据，结果**两条都红**
—— Catmull-Rom 在折点会外插出"过冲再回头"，回头处即使弧长参数化得完美，
**位移**也只有弧长的一小截（走 0.25 m 净移动 0.08 m）。那个判据量的是"曲线
的弯"，不是"参数化的均匀性"。换成**直线 + 手调手柄**（速度差 30 倍）才只剩
参数化一个变量。

其它语义：

- `closed`：`u ∈ [0,1)` 环绕；`sample(1) == sample(0)`（否则循环播放末帧跳一格）。
- **开放路径到端点就停住**，不弹回起点（弹回会让"落幅"变成跳切）。
- `speed` 是"每秒走完全长的比例"，`progress` 是相位起点 —— 位置 =
  `fract(progress + time × speed)`，是**时间的确定性函数**：时间轴拖回去对得上，
  暂停时不动。`speed == 0` 时由 `progress` 直接决定（面板拖滑块 = 手动推镜头）。
- `lookAlong`：朝向跟切线；`yawOffsetDeg` 让镜头"稍微往里拧一点"。
- 路径与关键帧同时存在时**路径赢**（与 Blender 的约束叠加同一语义）。

### 4.3 Track To（方式 C）—— `CameraComponent::lookTargetGuid`

- 存 **guid** 不是 `ecs::Entity` 下标（池子是 swap-and-pop 的）。
- 阻尼用**指数逼近** `1 − exp(−dt/τ)`，不是线性插值：线性插 `dt/τ` 会让
  30fps 与 144fps 跟出两条不同轨迹，而"手感"正是要跨帧率一致的东西。
- 首帧**直接落在目标上**（不从原点飞过去）。
- 目标被销毁 → 停在当前朝向，不保留幽灵注视点。

---

## 五、`CameraSystem::apply` 的叠加顺序（硬约束）

```
① 焦距 → 覆盖 fov
② Follow Path → 覆盖 position（+ 可选：朝向跟切线）
③ Track To → 覆盖 yaw/pitch
④ roll / shift 叠加（镜头安装角，与运镜的 roll 是两回事）
⑤ 手持 → 最后叠
⑥ 震动（trauma 包络）→ 最最后，且**不改** m_applied（干净值）
```

**为什么 ⑤ 必须在 ③ 之后**：手持在 Track To 之前叠，会被 `aimAt` 整个抹掉
—— 表现是"手持开了，但一配注视目标就不抖了"，几乎没人会往这个方向想。

**为什么 ② 必须在 ③ 之前**：反过来的话，"沿路径走 + 始终盯着主体"会退化成
"沿切线看"。

震动为什么不改 `m_applied`：震完的朝向与震前差一点点，连续震几次会"越震越歪"。
`CameraPose` 永远保持干净值，震动只在最后一步叠加 —— 关掉震动（默认
`amplitude = 0`）时写进相机的值与不震**逐位相同**。

---

## 六、写回实体（Bake）与欧拉分解

`TransformComponent::rotation` 是 **X→Y→Z 欧拉**，而 `CameraPose` 是
(yaw 绕世界 Z, pitch, roll) —— 两套参数**不能直接互换**。

`cameraEulerFor(pose)` 走一次基矩阵分解：先构造"局部 +X → forward、
局部 +Z → up"的正交基，再按 `Rx·Ry·Rz` 反解。

**⚠⚠ 基必须是右手基，`+Y` 只能取 `−right`。**
`(f, r, u)` 虽然更好念，但 `u = r × f` ⇒ `r × u = −f` ⇒ 行列式恒为 −1 ——
拿去反解欧拉会得到一组**镜像旋转**，重建之后局部 +X 直接反向（第一版就是这么
写的，自检 D1 在 yaw=90° 上当场红）。而 `(f, −r, u)` 同时满足三条：
+X = 视线、+Z = 上（Z-up 世界的自然约定，相机上挂道具不会横躺）、右手。

反向也读回来：`poseFromEntity` 从**实体自己的 up（局部 +Z）**算 roll
（`roll = atan2(dot(up, right), dot(up, up0))`）。不读这一条，写出去的 roll
会在下一次读回时被悄悄抹平 —— "实体在场景树里看着是歪的，一进 Play 就正了"，
且只在有滚转的镜头上出现。

自检 D1/D2（正解）+ F2（36 组闭环反解）双向钉住。

---

## 七、镜头光学

| 量 | 换算 | 备注 |
|---|---|---|
| 焦距 → 垂直视场角 | `fovY = 2·atan(sensorH / 2f)` | **只用传感器高度**，不用对角线 |
| f-stop → 景深光圈 | `1.2 × (f/50) × (1.4/N)` | 参考点 50mm f/1.4 = 1.2（= 出厂 `dofAperture`） |
| 快门角 → 模糊强度 | `deg / 180`（封顶 2.0） | 180° = 电影标准 = 1.0 |

**⚠ 快门存角度，不存 Blender 那种"帧数"**：0.5 帧在 24fps 是 180°、在 60fps
只有 90°，而本引擎的帧率在编辑器与玩家机器上都不一样 —— 用帧数存会让同一个
镜头在别人的机器上变糊或变清。180°/90°/0 三个点由 `cameramove_shutter_*` 钉住。

**⚠ 裁剪面 `0 = 不设`，不是"设成 0"**：`near = 0` 的投影矩阵是奇异的。而且
编辑器视口用的是另一套近远平面，无脑回写 0.1/200 会把正在高空俯视的视口里
那片地面裁掉。

**效果层的接线在宿主**（`RuntimeApp::applyCameraLensEffects`）：`scene` 不认识
`render`（单向依赖），所以镜头段 → `CameraEffectSettings` 的换算只能由宿主做。
语义是"**只在配了的时候写**"：`dofEnabled == false && shutterAngle == 0` 的相机
一个字节都不改 → `MYVK_DOF` 与后期体积的覆盖照旧生效。换相机时把**上一次自己
写进去的**两项复位（否则上一台的景深会粘住）。

---

## 八、面板（`Cinematography`）

面板表 id 是 `cine`（底栏"+"托盘 / View 菜单 / 布局落盘三路自动跟上，无快捷键
—— 与 Physics / Quality 同款）。

四段：

1. **相机列表** —— 按 priority 排序，活跃那台标绿。排查"画面为什么是俯拍"时
   一眼有答案（画面对着的那台不一定是你以为的那台）。
2. **镜头** —— 焦距 / 画幅 / 离轴 / 滚转 / 裁剪 / 景深 / 快门。焦距复选框的
   状态**从组件值推**（不另存 bool，否则会出现"面板勾着、组件里 focal=0"）。
   关掉焦距时把 fov 定死在当前位置 —— 否则下一帧会跳回组件里那个很久以前的值。
3. **rig** —— 注视目标下拉 / 阻尼 / 手持三项 / 路径（新建、进度、速度、闭合、
   沿切线、点位增删与拖动、只读回显曲线长度）。
4. **动词** —— 作用在**视口相机**上（nudge-then-bake），角度与距离**分开两格**
   （把 15 度当米用会一推把镜头甩出场景）。

最后是 **Bake view pose to entity**：把视口姿态 + `cameraEulerFor` 写进选中实体
的 Transform 与组件的 fov。

锚点（`logRect`）：`CINE-WIN` / `CINE-FOCAL` / `CINE-DOF` / `CINE-FSTOP` /
`CINE-SHUTTER` / `CINE-LOOK` / `CINE-DAMP` / `CINE-HANDHELD` /
`CINE-PATH-NEW` / `CINE-PATH-PROGRESS` / `CINE-PATH-SPEED` / `CINE-PATH-LOOK` /
`CINE-AMOUNT` / `CINE-VERB-{PAN,TILT,DOLLY,TRUCK,PEDESTAL,ROLL,ORBIT,CRANE}` /
`CINE-BAKE`。

日志：`CINE-STATE`（每帧一行，**变化才落盘**）、`CINE-VERB`（每次点击）、
`CINE-PATH action=new`、`CINE-BAKE`。

---

## 九、多机位切镜（2026-10-30 已做）

用户原文档第七节"多机位切换"。落点是**时间轴的第三个轨道**：一段过场里
不同时段用不同的相机，而不是整段只有一台（`priority` 静态决定的那个）。

### 9.1 数据：`.seq.json` 多一个 `shots` 数组

```json
{
  "duration": 8.0,
  "camera": [ ... 关键帧轨，照旧 ... ],
  "shots": [
    { "t": 0.0, "camera": "c2a0000000000001", "label": "A 全景" },
    { "t": 2.5, "camera": "c2a0000000000002", "blend": 0.4, "label": "B 特写" },
    { "t": 5.0 }
  ]
}
```

| 字段 | 含义 | 缺省 |
|---|---|---|
| `t` | **从这一秒起**生效，直到下一个 marker 或片尾 | 0 |
| `camera` | 相机实体 guid，**16 位小写 hex** | 省略 = `0` = 回落关键帧轨 |
| `blend` | 从上一段混过来用几秒。`0` = **硬切** | 0 |
| `label` | 面板与日志用（"A 机位 特写"） | 空 |

三条按设计钉死的语义：

- **区间是 `[marker.t, 下一个 marker.t)`**。`shotIndexAt` 在**早于第一个
  marker 时返回 −1**，不是 0 —— 照抄 `cameraKeyIndexAt`（早于首帧返回 0）
  的话，开场黑场那一秒会突然亮出第一个机位的画面。
- **`camera: 0` 是合法值**，意思是"这一段退回由关键帧轨驱动"。所以
  "空" 与 "没配" 是同一件事，都能用。
- **没有 `shots` 键的资产行为逐位不变**：`toJson` 不会凭空写出
  `"shots": []`（写了会让所有既有 `.seq.json` 的 diff 变脏），播放器三个
  输出恒为 `−1 / 0 / 0`，宿主一次事都不做。

### 9.2 落点：`CameraSystem` 的导演覆盖

播放器**仍然不认识 `CameraSystem`**（那条约束没破）。它只说"该用哪台"：

```cpp
seq.shotIndex()          // 0 基下标；−1 = 没有 marker 生效
seq.shotCameraGuid()     // 0 = 回落关键帧轨
seq.shotBlendSeconds()   // 0 = 硬切
```

`RuntimeApp::syncDirectorCamera()` 把这三样翻译成
`CameraSystem::setDirectorCamera(guid)` + `blendCut(秒)`。

⚠⚠ **必须 `clearDesiredPose()`**：玩家控制器每帧都会喂一次 desired，时间轴
有 pose 时也会喂。不清的话 `CameraSystem` 走"用 desired"分支，marker 选中
的机位**根本不动**（画面停在玩家视角），而 `CAM-CUT` 日志照样打出来 ——
表现是"切镜不生效，但日志说切了"。

⚠ **目标消失要回落**：marker 指的相机被删 / 被禁用时，`selectActive` 打一条
`CAM-CUT-MISS` 然后**按 priority 重选**，并把 guid 清零。不回落的话画面会
永久冻在最后一帧，而日志里只有一条早已刷过去的 WARN。

### 9.3 面板与自动化

`+ Shot` 按钮在播放头处插 marker，默认绑"场景里当前选中的那台相机"
（`selectedCameraGuid()` 会**沿父链向上找** —— 作者点中的经常是相机下面的
网格）。属性栏给 `t` / `blend` / `label` / `Bind to selection` / `Use keys`。

第三轨的**画法与另外两轨不同**：marker 划的是**区间**，所以画成一条横条
延伸到下一个 marker（或片尾），上面插一面小旗。扫一眼就知道"这几段分别由
哪台在播"。

注入脚本 verbs：`addshot` / `setshotcam:<hex 或 0>` / `setshotcamsel` /
`setshotblend:<秒>` / `setshotlabel:<文本>` / `setshottime:<秒>` /
`sel:shots:<i>`。

⚠ `setshotcam` 走 `parseGuidHex` 而**不是** `atoll`：后者会把 `00a1b2c3`
读成十进制 10123 —— 看着"绑上了"，实际绑的是一个不存在的实体。绑非 0 但
场景里不存在的 guid **会被拒**，否则文件里会留下死引用。

### 9.4 判据

| 层 | 命令 | 条数 |
|---|---|---|
| 数据 / 播放器 | `Game.exe --selftest sequence`（G 组） | 24 |
| 导演覆盖 / 混合 | `Game.exe --selftest cameramove`（G 组） | 12 |
| 端到端 | `python tools/verify_multi_cam.py` | 44 |

C 组的设计值得抄：场景里放两台相机（低优先级 + 高优先级），时间轴用
marker 强制先用**低**优先级那台 —— 于是 `CAM-CUT name=CamLow` 出现这件事
本身就证明了覆盖压过 `priority`，不需要额外造条件。

---

## 十、还没做

- **手动贝塞尔手柄**：`CameraPathPoint` 有 `in/outHandle` 字段，但面板只暴露
  "自动手柄"（Catmull-Rom）。要手调时直接改 JSON。
- **编辑器视口的景深 / 运动模糊**：目前只有 `Game.exe` 侧接线（编辑器视口是
  自由相机、走的不是 `CameraSystem`）。要补得先给编辑器一条"以选中相机预览"
  的路径。
- **Motion Blur 的 `Position` / `Steps`**：`CameraEffectSettings` 只有强度与
  采样数，没有"帧开始/中间/结束开窗"。
- **运镜曲线可视化**：marker 之间的混合只有一条小渐变块，没有"这条混合曲线
  长什么样"的编辑入口。
