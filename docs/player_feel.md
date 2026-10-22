# 运行时手感四件事（P5）

> 2026-10-22 · 用户报障四条，逐条定位到根因并修复
> 验收：`tools/verify_player_feel.py`（R0~R9，10 条，约 18 秒）
> 相关：`engine/animation/Locomotion.*`、`engine/animation/AnimatorSystem.*`、
> `engine/scene/ModelInstance.*`、`runtime/RuntimeApp.*`、`assetio/ProcAnim.cpp`
> **P6 补充**：本文 §2 里那条"跑起来之后脚还是陷"的现象，P6 查到了更硬的根因
> —— `CharacterController` 的**接触窗口（±0.02 m）比一帧的位移还窄**，快速下落
> 和跑上坡都会整帧跨过它，然后一路穿下去。修法与探针见
> `docs/player_controls.md` §3。本文关于 hips 只许向上的那一半仍然有效。

---

## 0 · 四个症状 → 四个根因

| 用户原话 | 根因 | 修在哪 |
|---|---|---|
| ① 脚陷进地面；角色没生在 GameStart | 程序化走路动画把髋部往**下**压 3.2 cm（跑步 5.6 cm）；编辑器 Play 用编辑态摆的位置，不看 GameStart | `assetio/ProcAnim.cpp` 的 hips 偏移改成只向上；`EditorContext::play` 加 `placePlayerAtGameStart` |
| ② 必须点视口才能操作、操作时隐藏鼠标、Esc 退出 | 启动就 `GLFW_CURSOR_DISABLED`（既没"点一下才操作"，也没"退出操作"这个态） | `RuntimeApp` 加 `m_mouseCaptured` 两态 |
| ③ **移动没朝视线方向** | 模型正面朝 **-Y**，而实体 yaw 语义是"0 = 朝 +X" → **横着平移** | `ModelInstanceOptions::yawOffsetDeg`，默认角色 +90° |
| ④ 状态切换机械僵硬 | 切 clip 是硬切：改 `clipName` + `time=0`，同一帧内腿的相位突变 | `AnimatorSystem` 双采样 + `blendPose`，0.2 s smoothstep |

---

## 1 · ③ 为什么"没朝视线方向"（最关键的一条）

### 症状

不是"转得慢"或"歪一点"，而是**侧身滑行**：人朝屏幕右，却在往屏幕深处走。

### 根因链

```
Mixamo 源模型（Y-up）正面朝 +Z
        ↓ ufbx target_axes = right_handed_z_up，映射 (x, y, z) → (x, -z, y)
引擎坐标系（Z-up）里正面落在 -Y
        ↓ 而 PlayerController 的朝向语义是
tc->rotation.z = atan2(dir.y, dir.x)     // yaw=0 → 正面朝 +X
        ↓ 两者差 90°
模型朝 -Y、位移朝 +X  →  横着走
```

### 怎么确认的（别靠猜）

把相机钉在 -X 侧看向 +X（`yaw=0`），然后：
- 按 **W**（走 +X）→ 截图是**侧面**；
- 按 **D**（走 -Y）→ 截图是**正脸朝镜头**。

两句合起来只有一个解：模型局部正面 = **-Y**。

### 修法

`scene::ModelInstanceOptions::yawOffsetDeg`（度，绕 Z）。装配时**预乘**旋转：

```cpp
m = glm::rotate(mat4(1), radians(opt.yawOffsetDeg), vec3(0,0,1)) * sub.transform;
```

⚠ 不能改成"给 `t->rotation.z += offset`"：子变换自己可能带 x/y 旋转，
欧拉角逐分量相加在那种情况下**不等于**绕世界 Z 转。预乘矩阵再 decompose
才是同一个几何。

默认角色用 **+90°**（`-Y` → `+X`）。两个入口都要传，且必须同源：

- `runtime/RuntimeConfig.h` → `PlayerVisual::modelYaw = 90.0f`
- `editor/EditorScene.cpp` → `kDefaultModelYaw = 90.0f`

差一个符号的症状就是"编辑器里朝前、Game.exe 里横着走"。

### 机器判据（R2）

**人形的左右跨度必须大于前后跨度**：

```
GAME-PLAYER facing yaw=90.0 spanX=0.364 spanY=1.947 shouldersAlong=Y ok=1
```

`spanY=1.947` 是 T-pose 的**臂展**（不是肩宽 —— 别拿 0.45 当基准，量到的是
臂展），`spanX=0.364` 是身体厚度。忘掉补偿时两者整好互换 → 红。

**反向验证过**：把 `modelYaw` 临时改回 0 → `spanX=1.947 spanY=0.364`，
只有 R2 红、其余 9 条仍绿（判据精确、无耦合）。

⚠ 刻意**不用截图比对**：角色像素包围盒的 `w/h` 实测在 0.367~0.429 之间飘
（手臂前后摆会撑宽包围盒），拿来断言就是一条 flaky 用例。

---

## 2 · ① 脚陷地 + 出生点

### 脚陷：hips 只许向上

`ProcAnim` 三段动画的髋部垂直偏移原来是"围绕一个负值振荡"：

| clip | 原式 | 范围 | 后果 |
|---|---|---|---|
| Idle | `0.006·sin` | ±6 mm | 吐气半段压脚 |
| Walk | `0.016·(-cos2w) - 0.016` | **[-32, 0] mm** | 支撑相脚陷 3.2 cm |
| Run | `0.042·(-cos2w) - 0.014` | **[-56, +28] mm** | 脚陷 5.6 cm |

最低点正好是**支撑腿接近伸直**的那一帧 —— 腿长不变、髋却往下走，脚只能往
地里去。改成同振幅、只向上的形式：

| clip | 新式 | 范围 |
|---|---|---|
| Idle | `0.006·(sin+1)/2` | [0, 6 mm] |
| Walk | `0.016·(1-cos2w)/2` | [0, 16 mm] |
| Run | `0.042·(1-cos2w)/2` | [0, 42 mm] |

Jump 的 `-0.115·crouch` 之类**故意保留**：那是蹲姿，膝盖会弯着吸收，而且
腾空相人本来就不在地上。

改完必须**重烤** `.vmesh`：

```bash
AssetC.exe assets/models/thirdparty/samba_dancing.fbx \
  --strip-skin --autorig --procanim --anim-fps 30 \
  -o assets/models/characters/default_character.vmesh
```

⚠ 烤完检查目录里**没有** `default_character.vmesh.vmeta` —— 那个文件会让
"第二次加载起永远失败"（`.vmeta` 的 mtime 永远比 `.vmesh` 新 → 判过期 →
回退按 FBX 解析 `.vmesh` → 抛异常）。第一次一切正常，极具迷惑性。

### 装配侧的脚底锚点（R1）

```
GAME-PLAYER model lift=0.902 m (bounds z: -0.902..0.902) yaw=90.0 deg -> bottom=0.0000
```

`bottom = lo.z + lift`，按构造恒为 0。这条锚点的价值不在"现在对"，
而是**有人改抬升公式时立刻红**。

### 出生点：Play 也以 GameStart 为准

语义分界：

- **编辑态**：Player 摆哪儿只是"看着方便"，GameStart 才是出生点；
- **运行态**：玩家必须出生在 GameStart 上。

`EditorContext::play` 在 `sceneFromJson` 之后、相机设置之前调
`placePlayerAtGameStart`（与 `spawnPlayerFromGameStart` 同一口径：脚底 =
GameStart 的 position，yaw = `rotation.z + gameStart.yaw`，胶囊中心再抬
`halfHeight + radius`）。同时把 `PlayerComponent::started` 清掉 —— 否则
上一轮 Play 的起步状态会让惯性/起步动画直接跳过。

GameStart 的标记只在 `isEditing()` 时画（叠加层），运行态天然不出现。

**R0 判据**：场景里把 GameStart 故意摆在 `(5,3,0)`（不是原点），断言
`spawned at GameStart feet=(5.00,3.00,0.00)`。只测原点的话，"用没用
GameStart"根本看不出来。

---

## 3 · ② 鼠标两态

### 状态机

```
        左键点 3D 视口                      Esc / 暂停
未捕获 ─────────────────→ 操作中 ──────────────────────→ 未捕获
captured=0                captured=1                    captured=0
光标可见                   隐藏 + 锁定                    光标可见
玩家输入冻结                WASD 生效                     玩家输入冻结
```

实现要点：

- **只有一个 bool**（`m_mouseCaptured`）。不要再单独存"光标可见"——
  同一件事的两种写法，存两份必然有一天不一致（症状是"鼠标看不见但角色不动"）。
- 捕获 = `Window::setCursorVisible(false)`，它内部就是
  `GLFW_CURSOR_DISABLED`（隐藏 + 锁定 + 无限转视角），并吞掉一次 warp 的
  鼠标位移（否则进出的那一帧镜头会猛转）。
- **回到 Gameplay 不自动捕获**：从菜单点"继续"之后仍然要玩家再点一次视口。
  自动捕获的话"鼠标突然消失、镜头开始跟鼠标转"，玩家会以为鼠标失灵。
- 未捕获时**只门控 `m_player->update`**，不动 `tickPlayerAnimation`：
  否则人会僵在出生点一动不动，看起来像卡死；点了之后才突然开始呼吸。

### 判据（R3~R6、R8）

| # | 断言 |
|---|---|
| R3 | `GAME-MOUSE captured=0` 出现在 `captured=1` 之前，且 `UI2-PAUSE cursor=1` |
| R4 | **点击前按 W，位移 < 0.02 m**（门控真的生效，不是"看起来没动"） |
| R5 | `GAME-MOUSE captured=1 reason=click` |
| R6 | 点击后按 W，Δx > 0.5 m 且 `state=walk` |
| R8 | Esc → `UI2-PAUSE open=1 cursor=1`（退出操作态、光标回来） |

R4 是这一组里最容易写成假绿的一条：只断言"点击后能走"的话，
"没捕获也能走"这个 bug 抓不到。

---

## 4 · ④ 状态切换平滑过渡

### 底座早就有了

`animation::Pose` + `samplePose` + `blendPose` + `poseToJoints` 在 A0-5 就拆好了
（`AnimatorSystem.h` 的注释甚至写了" A2 只需要在这里加第二次 samplePose 和
一条 blendPose"）。P5 只是把它接上。

### 接线

`SkinnedMeshComponent` 加四个字段（不进存档 —— 过渡是"这一帧正在发生的事"）：

```cpp
std::string fadeFromClip;
float fadeFromTime = 0.0f;
float fadeT = 0.0f;
float fadeDur = 0.0f;
bool fading() const { return fadeDur > 0.0f && !fadeFromClip.empty(); }
```

`Locomotion::setClip` 换 clip 时记下旧 clip 与**当时的**播放头（有
`AnimatorComponent` 时要取 `an->time`，取错会从旧片段第 0 帧淡入 —— 腿会
"缩回去再迈出来"）。

`AnimatorSystem` 采样：

```cpp
if (sk.fading()) {
    sk.fadeT += dt / sk.fadeDur;          // dt，不是 dt*speed（过渡是时间语义）
    if (advance && playing) sk.fadeFromTime += dt * speed;  // 旧 clip 继续过相位
    if (sk.fadeT >= 1 || !fromClip) { 收尾，回到单 clip 直采 }
    else {
        samplePose(skel, fromClip, sk.fadeFromTime, m_poseFrom);
        samplePose(skel, clip,     time,             m_pose);
        blendPose(m_poseFrom, m_pose, fadeWeight(sk.fadeT), m_poseMix);
        poseToJoints(skel, m_poseMix, sk.jointMatrices);
    }
}
```

三个要点：

1. **权重用 smoothstep，不用线性**。线性权重下角速度在两端突变，过渡的两头
   各有一个肉眼可见的顿挫 —— "做了过渡还是僵硬"就是这么来的。smoothstep 的
   导数在 0/1 处为 0，与两侧稳态衔接得上。
2. **旧 clip 的播放头要继续走**。冻住的话混合出来是"腿在半空中不动"，
   比硬切更怪。
3. **只对换 clip 生效，不作用于 idle 重放**。同一段动画归零重播，淡入淡出
   只会让它看起来卡一下。

时长 0.20 s（`LocomotionConfig::blendSeconds`）。0.18~0.25 是人能感觉到"顺"
又不会觉得"反应变慢"的区间。

### 判据（R7）

```
GAME-ANIM state=walk clip=Walk (from='Idle') ... blend=0.20s
ANIM-FADE begin from=Idle to=Walk dur=0.20s
```

⚠ 两条都要：`blend=0.20s` 只证明"配置读进来了"，**不证明真的在混**。
而 `ANIM-TICK ... fading=N` 也不能拿来断言 —— 它每 60 帧才打一行，
一次过渡只有 12 帧，采样点几乎必然落在过渡之外（实测 `fading_seen=0`：
功能是好的、判据是瞎的）。所以加了 `ANIM-FADE begin`，一帧一条。

---

## 5 · 五个坑

**① 模型朝向不做任何日志就永远查不出来。**
"横着走"看起来像控制器算错了方向，实际几何层就错了。所以 `facing`
自检不是可选项：它把"装配后左右跨度落在哪个轴"变成一行可读的数。

**② `.vmeta` 被误留在资产目录。**
重烤之后必须 `ls` 一眼。症状是"第一次加载正常、第二次起必崩"，而且删掉
`.vmeta` 又好一次 —— 最容易误判成"缓存问题"。

**③ `ANIM-TICK` 不能当高频锚点。**
每 60 帧一行，任何短于 60 帧的事件（过渡、一次性动作）都采不到。
要么加专用锚点，要么接受"这条判据是瞎的"。

**④ 截图管道 + Vulkan validation = 死锁。**
`subprocess.Popen(stdout=PIPE)` 时，validation 的日志几十毫秒就能填满
64 KB 管道缓冲，子进程阻塞在 `write` 上 → 窗口永远不出现
（表现是"没找到窗口"，看起来像崩溃）。**stdout 必须直接写文件**。

**⑤ 构建失败先看有没有进程占着 exe。**
`ld returned 1 exit status` 且**没有任何 undefined reference**，
就是 `Launcher.exe` / `Game.exe` / `Editor.exe` 正在运行、文件被锁。
`tasklist | grep` 一眼就出来，别去怀疑代码。

---

## 6 · 验收

```bash
python tools/verify_player_feel.py     # R0~R9，10 条，约 18 秒
python tools/verify_char_locomotion.py # P4 的 15 条（回归确认）
```

已登记 `run_regression.py` 的 `DEFAULT` + `TRIGGERS`，`check_regression_map.py`
补了两条夹具（`Locomotion` / `ProcAnim` 的期望是**并集**：它们早已被
`verify_char_locomotion` / `verify_procanim` 等占着）→ 映射自检 **PASS**。

---

## 7 · 还没做

| 项 | 现状 |
|---|---|
| 转向时的交叉淡入 | 转身靠 `lerpAngle` 插值 bodyYaw，没有"转身动画" |
| 分方向移动动画 | 只有一段 Walk，没有走↔后退↔横移的区分 |
| 根运动 | 位移由碰撞驱动，步子与位移可能轻微不同步（滑步） |
| 相机碰撞回弹 | 弹簧臂有，但没有"贴墙时横向偏移" |
| 手柄 | `LookYaw/LookPitch` 轴已接，未做手柄操作态切换 |
