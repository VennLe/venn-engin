# 外部模型 → 可玩角色（1:1 尺度 / 第三人称 / 一键绑骨 / 程序化动画）

> 2026-10-22。这份文档讲的是**「一个网上下载的 FBX 角色，怎么变成一个能按 W
> 走的第三人称角色」**这一条端到端链。`.vmesh` 的字节布局、焊接/重排那些
> 离线修正，看 [fbx_asset_pipeline.md](fbx_asset_pipeline.md)；这里只管
> 资产进场景之后的四件事：**尺寸对不对、挂得上挂不上、有没有骨头、有没有动作**。

---

## 0 · 一页结论

| 能力 | 入口 | 验收脚本 | 当前结果 |
|------|------|---------|---------|
| 第三方 FBX 进场景，尺寸 1:1 | 拖进编辑器 / `MYVK_EDITOR_IMPORT` | `tools/verify_model_scale.py` | PASS |
| 第三人称绑定（模型当玩家） | `assets/game.json` 的 `player` 块 | `tools/verify_player_character.py` | 11/11 PASS |
| 一键自动绑骨（剥掉原厂骨架也能绑） | `AssetC --strip-skin --autorig` | `tools/verify_autorig.py` | 10/10 PASS |
| 自动生成 idle/walk/run/jump | `AssetC ... --procanim` | `tools/verify_procanim.py` | 19/19 PASS |

一条命令走完全链（离线，不开窗口）：

```bash
cd build/bin
./AssetC --strip-skin --autorig --procanim \
        <项目>/assets/models/thirdparty/samba_dancing.vmesh \
        -o <项目>/assets/models/thirdparty/samba_autorig.vmesh
```

然后让运行时用它当玩家（`build/bin/assets/game.json`）：

```json
{
  "player": {
    "model": "models/thirdparty/samba_autorig.vmesh",
    "clip": "Idle",
    "fitBody": true,
    "walkSpeed": 0.15, "runSpeed": 0.0,
    "idleClip": "Idle", "walkClip": "Walk", "runClip": "Run", "jumpClip": "Jump"
  }
}
```

或者只对一次启动生效（不落盘改了就跑）：`MYVK_RUNTIME_PLAYER_MODEL` /
`MYVK_RUNTIME_PLAYER_CLIP`。

---

## 1 · 1:1 尺度：先把"米"钉死

**世界单位 = 米，Z 轴朝上**（右手系）。这不是约定俗成，是有硬编码兜底的：

- 玩家胶囊 `radius=0.35` + `halfHeight=0.55` → 总高 **1.8 m**
  （`engine/physics/CharacterController.h:49-50`）
- 重力的 `-9.81`、跳跃初速、`maxSpeed=4.5 m/s`、冲刺 ×2 —— 全是按米/秒给的
- 相机近裁面、阴影级联的 split、terrain 的高度采样步长也都按米

所以"模型多高"不能靠肉眼，必须**量**：

```bash
./AssetC --info <file>.vmesh
# ASSETC   bounds min=(-0.973, -0.182, -0.902) max=(0.973, 0.182, 0.902)
# ASSETC   size=1.947 x 0.364 x 1.805 m  height(Z)=1.805 m
# ASSETC   world=(-0.973,-0.182,-0.900)..(0.973,0.182,0.900)
```

`size` 是**几何 AABB**（已乘各子网格的变换），`world=` 是同一个量的世界版
（`.vmesh` 的 pivot 会被归位到几何中心，两者差一个平移 —— 见 §6 坑 9）。

`verify_model_scale.py` 的做法值得抄：它不"相信"任何一个数字，而是在场景里
**立一根 1 m 的标尺**跟角色一起拍，再从像素高度反推 —— 相机投影错了、渲染
尺度错了、导入缩放错了，这三件事都会让 S3/S4 变红，而只读 `--info` 只能抓
第一类。

### pivot 与"脚底"
模型 pivot 在**几何中心**，而玩家实体的位置语义是**脚**（`PlayerController`
的 `teleport(feetPos)`）。所以挂模型时必须抬一次：

```
lift = -boundsMin.z        // 1.805 m 的模型 → 0.902 m
```

抬错这个数的两种症状长得很像但原因相反：不抬 = 角色**陷进地面半个身子**；
抬两倍 = 角色**悬空一米**。日志里 `GAME-PLAYER model lift=0.902 m
(bounds z: -0.902..0.902)` 就是这一行的现场。

**编辑器那条路**由 `scene::ModelInstanceOptions::dropToGround` 管（2026-10-22
起 Editors 的 `instantiateModel` 默认打开）：把 `position` 解释成"**地面点**"
而不是"模型原点"，装配完按世界 AABB 抬 `-min.z`，日志是
`MODEL-GROUND path=.. bounds.z=-0.902..0.902 lift=0.902 (bottom -> 0.000)`。
拖放路径后面还有 `alignImportToGround` 兜底，两者**幂等**（`shift.z` 会算出 0）。
运行时不用这个开关：玩家有胶囊要对齐，自己算 lift。

---

## 2 · 第三人称绑定：三条必须同时成立的事

### 2.1 必须走 `scene::instantiateModel`

`scene::instantiateModel` 才是"把模型摆进场景"的公共入口：它给**每个子网格**
建一个实体，挂 `MeshComponent` + `MaterialComponent`，并在根实体上挂
`SkinnedMeshComponent`。

**只加 `SkinnedMeshComponent` 是不够的** —— 渲染器的绘制列表是从
`MeshComponent` 收的（`engine/render/Renderer.cpp` 的 collect 阶段），没有
`MeshComponent` 的实体连绘制项都不会生成。症状是：日志里 `MODEL-INSTANCE`
好端端地打着 `skinned=1 joints=52`，画面里什么都没有。

### 2.2 必须挂 `CameraComponent`

`CameraSystem::update` 先 `selectActive()`，找不到**启用的**
`CameraComponent` 就**直接 return**。于是 `PlayerController` 每帧写进去的
期望相机姿态，一个字都落不到 `Scene::camera()`。

症状：**角色走得挺好，但镜头钉在出生点、人越走越小**。看起来像"相机没跟随"，
其实是"跟随逻辑跑了但没人接收"。

### 2.3 胶囊要跟着模型身高长

`fitBody=true` 时按模型高度 H 重算三组数（窄的模型用窄胶囊，否则会卡门框）：

| 字段 | 公式 | 1.805 m 的模型 |
|------|------|---------------|
| `radius` | `clamp(H*0.16, .15, .60)` | 0.289 |
| `halfHeight` | `H/2 - radius` | 0.614 |
| `stepHeight` | `clamp(H*0.20, .20, .50)` | 0.361 |
| 相机 `targetOffset.z` | `H*0.42` | 0.758 |
| 相机 `distance` | `clamp(H*2.0, 2.5, 6.5)` | 3.61 |

### 2.4 四态状态机

`tickPlayerAnimation()` 只在玩家实体**真的有 `SkinnedMeshComponent` +
骨架**时才参与（否则整段不生效，行为与引入之前逐位一致）。判据：

```
!onGround && jumpClip        → jump
hs >= runThresh && runClip   → run
hs >  walkSpeed && walkClip  → walk
否则                          → idle
```

- `runThresh`：`player.runSpeed` 给了就用它；`<= 0`（默认）= **自动**取
  `cc->maxSpeed × 1.15`。为什么不能写死 3.2：默认 `maxSpeed` 是 4.5、冲刺 ×2，
  写死的话**普通走路就已经超过阈值** → 永远在播跑步，而"走路在跑"这种错最难
  被当成 bug 看出来（腿在动、位置在变，只是不像走路）。
- 缺 clip 是**配置事实**，不是错误：`GAME-ANIM-MISSING state=walk clip='Walk'
  不在模型里（共 2 段）` 只喊一次（用人话讲就是"这个模型里没有叫 Walk 的动画"），
  然后保持当前姿势。悄悄不动会让"角色像块石头"变成一次盲查。

---

## 3 · 一键自动绑骨（`assetio/AutoRig`）

对标 Mixamo 的 "auto-rigger"：**给一个没有骨架（或骨架被剥掉）的静态网格，
产出一套能蒙皮的人形骨架**。

### 3.1 只有网格时，能知道什么

不知道谁是谁，只能量：

1. **身高 H** = 世界 AABB 的 z 跨度；**水平两轴**里更长的那个是**手臂轴**
   （T-pose 的臂展 1.947 m > 前后 0.364 m）。
2. **朝向**：脚部顶点的质心相对身体质心的偏移，一定指向**脚趾**方向
   （脚跟在最后、脚趾在最前）。本例测得 `fwdSign=-1`（−Y 方向是前）。
3. 其余全靠**解剖比例表**（对着原厂 Mixamo 骨架标定，单位是 H 的倍数）：

| 关节 | z/H | 说明 |
|------|-----|------|
| Hips | 0.552 | 骨盆 |
| Spine / Spine1 / Spine2 | 0.607 / 0.672 / 0.746 | 三节分摊 |
| Neck / Head | 0.829 / 0.886 | |
| HeadTop_End | 1.000 | 用于校验 H |
| 肩高 | 0.795 | 肩横 ±0.104H |
| 髋横 | ±0.0505H | 股骨头，**不用**网格质心（会被大腿内侧拽偏） |
| 膝 / 踝 | 0.290 / 0.057 | 踝的横向在 0.090H 量（在 ankle 高度量会被脚跟拽到身后） |
| 趾 | 前移 0.07H，z 0.006H | |

然后用"**沿高度切薄层取顶点质心**"把每个关节的横向位置吸附到真实网格上
（比例表只给 z，x/y 必须问网格）。

### 3.2 权重

- **距离反比**，4 次幂（`1/d⁴`）：幂次低 → 手腕带着小臂一起动；幂次高 → 骨段
  接缝处出现硬折角。
- **左右闸门**：`LeftXxx` 只吃 `lat >= -0.02H` 的顶点，反之亦然。没有这道闸门，
  左手会拽动右胸。
- 取前 `maxInfluences=4` 个再归一化（GPU 蒙皮的常规预算）。
- 闸门挡到没有任何骨可给的顶点 → 兜底给 Hips 全权重（宁可僵，不可消失）。

### 3.3 结果（`verify_autorig.py` 的判据）

把原厂 52 骨**剥掉**，对纯静态网格重新绑 23 根 Mixamo 同名骨，再逐根比位置
（比"相对 Hips 的向量"，因为 `.vmesh` 的 pivot 会归位）：

| 判据 | 实测 |
|------|------|
| 关节位置误差（22 根同名骨） | 平均 **13.0 mm** / 最大 **34.2 mm**（Spine2） |
| 左右对称（|横向| 之差） | **0.0 mm**（全部 8 对） |
| 死骨（主导顶点 < 20 个） | 无；最少的 Hips=188 |
| 权重归一化残差 | **2.4e-07** |
| 蒙皮覆盖 | 35440 / 35440 顶点 |

3 cm 级的偏差肉眼看不出来，但**"看起来能动"和"绑对了"是两件事** ——
比如 Hips 偏 5 cm，角色走路会一直轻微前倾；踝偏后 3 cm，脚会一直"踮着"。
这类错只有逐根比数值才能抓到。

---

## 4 · 程序化四段动画（`assetio/ProcAnim`）

### 4.1 为什么是**关键姿势表**，不是正弦叠加

第一版用 `thigh = A·sin(2πp)` + `knee = B·max(0, -sin(2πp))`。看着合理，
实测**脚抬到 0.3188 m**（真人 5~10 cm）。

原因：两个正弦**同相**，膝盖最大弯曲正好落在腿摆到最后方那一刻（趾离地瞬间）
—— 脚被甩到身后高高抬起，就是俗称的"驴踢"。

改成 4 个**关键姿势** + smoothstep 插值（线性插值会在每个关键姿势处出折角，
肉眼是"卡帧"）：

```cpp
// p       thigh   knee   ankle   toe    arm   elbow
{24.0f,  -6.0f,   4.0f,  0.0f, -20.0f, 20.0f},  // 0.00 脚跟着地
{ 2.0f,  -9.0f,   0.0f,  0.0f,  -4.0f, 22.0f},  // 0.25 支撑中期
{-20.0f, -34.0f, -16.0f, -14.0f, 18.0f, 26.0f}, // 0.50 趾离地
{10.0f,  -52.0f,  10.0f, 0.0f,   2.0f, 20.0f},  // 0.75 摆动中期
```

抬升降到 **0.184 m**（跑步 0.438 m）。

### 4.2 判据：怎么用**一个数**抓住"倒着走"

前向位移范围是**对称量** —— 把腿部摆动整号翻转，范围和最大值出现的位置
一模一样（`walkFootFwdMaxPhase` 恒为 0.000，抓不到）。

真正能抓的是**摆动相的平均前向速度**：真人走路时脚离地那一段一定是
**向前飞**（把脚从身后甩到身前），着地时相对髋**向后走**（身体越过支撑脚）。
方向反了这一个数就整号变负（"月球漫步"）。

实测 `walkSwingFwdVel = +0.558 m/s`。

### 4.3 手臂：最容易调错的一个数

T-pose 的手是**水平伸直**的（指尖 lateral 0.74 m）。`armDown` 是从 T-pose
往下压的角度：

- 压 72° → 还剩 18° 外张，从斜后上方看就是**"张开翅膀滑翔"**
- 从正面/侧面截图**完全看不出来**（手确实在身侧、确实在摆）
- 真人自然站立时上臂距垂直 8~10° → `armDown = 80°`
- 量化判据：肩 0.19 m + 上臂 0.55 m，压到 80° 时手应落在 **0.28 m** 以内

实测 `walkHandLat 0.350 → 0.277`、`runHandLat 0.326 → 0.250`（改成 80° 之后），
而脚的四个数**一个都没变** —— 说明这两组量是解耦的。

### 4.4 四段的参数与产出

| clip | 时长 | 循环 | 轨道 | 说明 |
|------|------|------|------|------|
| `Idle` | 2.40 s | 是 | 9 | 只让少数关节轻微起伏（站着不能乱晃） |
| `Walk` | 1.00 s | 是 | 16 | 4 关键姿势 × 双腿 |
| `Run` | 0.70 s | 是 | 16 | 幅度更大 + 上身前倾 12° + 腾空相 |
| `Jump` | 0.90 s | **否** | 13 | 蹲 → 蹬 → 空中收腿 → 落地缓冲 |

- 循环片段**多采一帧**（末帧 == 首帧），否则循环处会"冻一下"
- 只为**真的在变**的关节建轨道（Idle 只有 9 条，Walk/Run 16 条）
- Hips 额外建 Translation 轨（上下颠 / 左右重心）
- Jump **不做**大幅 Hips 上移：位移是**物理的**（角色控制器有重力与初速），
  动画再抬一次就会"跳两倍高"
- 生成的名字就是运行时状态机找的那四个，不用额外配置

---

## 5 · 验收：四条脚本，各自守一段

| 脚本 | 组 | 守什么 |
|------|-----|-------|
| `verify_model_scale.py` | G1~S5 | 标尺 1 m 与角色同拍 → 像素反推身高（±6%）→ 1:1；实测标尺 112 px（期望 111.3）、角色 203 px → **1.812 m vs 几何 1.805 m（0.4%）** |
| `verify_player_character.py` | V0~V8 | 出生点造玩家 / 走 `instantiateModel` / 胶囊=身高 / 脚底对齐 / 关节进 SSBO / 按键真的走得动 / 与胶囊干板做差有像素 |
| `verify_autorig.py` | R0~R6b | 剥骨架 → 自动绑 → 逐根比位置 / 左右对称 / 无死骨 / 权重归一 / 产物合法 |
| `verify_procanim.py` | A0~A12 + B1~B6 | 离线数值（步幅/抬脚/摆动相速度/相位/手臂横向/idle 漂移）+ 运行时四态真的切得动 |

关键数值（本轮实测）：

```
PROCANIM-PROBE walkFootFwd=0.9558 walkFootLift=0.1842
               runFootFwd=1.0800  runFootLift=0.4383
               walkHandFwd=0.3657 idleDrift=0.0120
               walkFootFwdMaxPhase=0.000 walkSwingFwdVel=0.558
               walkHandLat=0.2774 runHandLat=0.2496 runHandFwd=0.4735
GAME-PLAYER   capsule radius=0.289 halfHeight=0.614 totalHeight=1.805 (model=1.805 fit=1)
GAME-PLAYER   visual=model meshes=2 skinned=1 height=1.805 m (1:1)
GAME-ANIM     state=idle → walk → run → jump
```

---

## 6 · 坑（都是这一轮真红过的）

1. **没有 `CameraComponent`，相机就不跟随**（§2.2）。症状极像"相机系统坏了"，
   但日志里 `CAM-ACTIVE` 一条都没有 —— 那才是线索。
2. **`SkinnedMeshComponent` 不产生绘制项**（§2.1）。`MODEL-INSTANCE ... skinned=1`
   打着，画面空的。
3. **`SceneSerializer` 的"空实体"判定名单漏项**：出生点（`gameStart`）被当成
   空实体计入 `skipped`，可它好好加载了 —— 于是"场景读进来有没有丢东西"这条
   断言一直在骗人。名单是**存、读两侧各一份**，改一处不够。
4. **`desc.maxSpeed` 会把冲刺截回走路速度**：`PlayerController` 里
   `speed = maxSpeed × sprintMultiplier` 算出 9 m/s，交到
   `stepCharacter` 又被 `desc.maxSpeed`（= 4.5）截回去 —— 冲刺是个**摆设**，
   而动画状态机的 run 阈值（5.175）永远够不到。物理层的"输入上限"必须留到
   冲刺速度（`physicsMaxSpeed()`）。
5. **`PostMessage` 合成的 Shift 会被 GLFW 当场撤销**：`glfwPollEvents` 末尾有一段
   "防 Shift 粘住"的补丁，用 `GetKeyState(vk)`（**物理**键状态）复核，没按着就
   强制 release。合成消息只改队列、不改物理状态 → 按下在**同一次轮询里**就被
   自己撤掉，一帧都留不下。W / 空格不受影响（没有这个补丁）。
   **修饰键必须用 `keybd_event`（真实键事件）+ 先 `SetForegroundWindow`。**
   同一段补丁的第二个后果：上一轮漏放 Shift，下一次启动 GLFW 会认为
   "开局就按着冲刺"，日志里凭空冒出 `run` —— **测试要先清修饰键状态**，
   否则"跑通了"和"被污染了"是同一种现象。
6. **正弦步态 = 驴踢**（§4.1）。判"像不像走路"不能看单帧截图（静帧永远像在走）。
7. **对称量抓不到方向错误**（§4.2）。要盯**带符号的速率**。
8. **手臂下垂角差 8°，斜后上方视角就变成"滑翔"**（§4.3）。正面/侧面看不出来。
9. **`.vmesh` 的 pivot 归位**：离线写出时整体平移 `-0.9024`（几何中心对齐原点），
   所以 `--info` 的关节世界坐标与原厂 FBX 差一个常量。**验证时必须比相对向量**，
   否则会误判成"绑骨整体偏了 0.9 m"。
10. **自动绑骨的股骨头不能直接量网格质心**：会被大腿内侧的顶点拽进去 ~2 cm；
    踝也不能在 ankle 高度量横向（会被脚跟拽到身后 3.3 cm）。比例表定 z、
    薄层质心定 xy，两者各管一半。
11. **自动化导入不留地**：`MYVK_EDITOR_IMPORT` 那条路只调
    `instantiateModel`，**不调** `alignImportToGround`（拖放路径才调）。
    于是脚本导进来的角色有一半埋在地里，而人手动拖进去是好的 —— 同一件事
    两条路两种结果，是文档里"自动化导入路径同上"那句承诺的漏项。
    修法：`ModelInstanceOptions::dropToGround`（见 §1）。
12. **视口左上角的导入通知条会污染像素包围盒**：`MYVK_EDITOR_IMPORT` 之后
    会弹一条提示，它跟空场景干板做差也是"变了的像素"。按"哪行变了"取上下界
    时，标尺从真实的 **110 px 被量成 307 px** → S3/S4 双双变红，而渲染其实
    完全正确。修法：4 倍降采样 → 连通域 → 取"**高度**最大且宽度 < 视口 60%"
    的那一坨（用高度不用面积：通知条又宽又矮，面积反而更大）。

---

## 7 · 这一轮**没有**做的

- **没有 IK**：脚不贴斜坡（`A6` 的阶段）。程序化动画的脚是按"平地"给的。
- **没有根运动**：位移完全由角色控制器产生，动画里的 Hips 只有上下颠与左右
  重心，没有前向位移 —— 所以现在走快了会**滑步**（`A4` 才解决）。
- **没有 crossfade**：四态之间是硬切（`A2`）。0.15~0.25 s 的淡入淡出能显著
  掩盖硬切，但不做也不会"错"。
- **只支持人形**：`AutoRig` 的比例表是人的。四足 / 鸟 / 机械要另写一套。
- **没有手指**：23 根骨到 `Hand` 为止（`HandThumb1` 那些没做）。

---

## 8 · 回归登记

四条脚本都已进 `tools/run_regression.py`：

- `DEFAULT`：`verify_model_scale` / `verify_player_character` /
  `verify_autorig` / `verify_procanim`
- `TRIGGERS`：
  - `verify_model_scale` ← `assetio/FbxImport.*`、`assetio/VertexOps.*`、
    `assetio/SourceModel.h`、`editor/EditorScene.*`、`engine/scene/ModelInstance.*`
  - `verify_player_character` ← `runtime/RuntimeApp.*`、`runtime/RuntimeConfig.*`、
    `runtime/PlayerController.*`、`engine/scene/SceneSerializer.*`、
    `engine/scene/ModelInstance.*`、`engine/animation/**`
  - `verify_autorig` ← `assetio/AutoRig.*`、`assetio/VertexOps.*`、`assetc/main.cpp`
  - `verify_procanim` ← `assetio/ProcAnim.*`、`assetc/main.cpp`、
    `runtime/PlayerController.*`、`runtime/RuntimeApp.*`

`assetio/MeshFormat.*` **故意不挂在新增的四条上**：它是格式本体，已由
`verify_vmesh` 独家看守（`assetio/**` + `assetc/**`）。挂多份只会稀释"谁该为
它负责"。`engine/render/Renderer.*`、`engine/assets/**` 是横切，按铁律不写。

`tools/check_regression_map.py` 的 A 组同步补了三条 fixture
（`assetc/main.cpp` 的期望从 1 个 owner 变成 3 个；
`assetio/AutoRig.cpp` / `assetio/ProcAnim.cpp` 各一条）。当前结果：**PASS**。
