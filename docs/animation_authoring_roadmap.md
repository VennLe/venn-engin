# 动画系统：对着 Blender 那条流程线做体检（2026-10-28）

> 需求原文是"按 `动画制作流程 → 各编辑器面板 → 关键参数含义` 完善引擎的动画系统"。
> 这份文档先做**映射**（Blender 的每一节在引擎里对应什么、现在到哪一步），
> 再给**分阶段落地计划**，最后记录这一轮（R5）实际做了什么。
>
> ⚠ 本文所有"现状"都带 `文件:行号`，改代码后请顺手更新 —— 一份不带证据的
>   差距表，三个月后就是谣言。

---

## 0 · 一句话结论

引擎的动画系统**不是缺功能，是缺"作者链"**。

运行时那一层（B1 层遮罩 / B2 事件 / B3 根运动 / IK / 弹簧骨 / 状态机 / 根运动）
做得比多数 hobby 引擎细，而且有 126 条数值断言撑着。但是**从"有一份动画"到
"做一份动画"，中间没有工具**：

- ~~骨骼关键帧**打不了**（clip 只从模型文件导入，没有写回通路）~~ —— ✅ R7 已补
  （2026-10-28，`engine/animation/ClipEdit.h` + Character 窗口 Pose Mode）；
- ~~曲线**编不了**~~ —— ✅ R6 已补（`editor/CurveEditorPanel.*`）；三处插值仍是
  各写各的，只有过场相机段接了 `Curve`；
- ~~片段**排不了**（没有 Dope Sheet）~~ —— ✅ R8 已补
  （`editor/DopeSheetWindow.*`）；NLA 仍未做（R9）；
- 表情**动画不了**（morph 只能手拨滑条，不进 clip）。

所以这份计划的重心全部在**作者链**，不再往运行时堆新特性。

**2026-10-29 补充（重排的由来）**：R6–R8 把"调动画"这一段打通了，但对照完整的
Blender 流程，**作者链的头三环是空的**：

- **骨架只能导入，不能编辑** —— 没有 Edit Mode、没有 `Roll`、没有 `Connected` /
  `Deform` / `Inherit Rotation` 开关（第二节）→ **S-A 已补**（还差从零建 Armature）；
- **蒙皮只能看，不能改** —— 权重是 `Mesh` 里的裸数组，没有顶点组、没有自动权重、
  没有权重绘制（第三节）→ **S-B 已补**（还差 bone-heat）；
- **约束只有三个特性，没有一套栈** —— IK 是函数调用，不能排序、不能叠、
  不能统一打 `Influence`（第五节）→ **S-C 已补**（还差运行时与 LookAt/Spring 入栈）；
- 另外，R6 的曲线视图**只画相机**，骨骼的旋转曲线仍然画不了（第七节）。

也就是说：**"有一份动画之后"通了，"有一个模型之后"没通。** 新增的 S-A / S-B /
S-C 就是补这一段，见第 3 节。

---

## 1 · 映射表

`状态` 列的图例：**✅ 有** / **🟡 半有**（能用但缺关键一半）/ **❌ 无**

> ⚠ 本节按**作者链的真实顺序**排（建骨架 → 绑定 → 蒙皮 → 姿态 → 摄影表/曲线 →
>   约束/驱动 → 复用）。顺序不是排版喜好：**上游没有，下游就是空中楼阁** ——
>   相机曲线编辑器做得再顺，也画不出一根骨骼的旋转曲线。
>
> 2026-10-29 重排：上一版按"基础流程 / 时间轴 / Dope Sheet / Graph …"排，
> 把"骨架从哪来"这件事整个跳过了。那是**从 R5 往回看**的顺序（先做了什么写什么），
> 不是**从作者往外看**的顺序（要先有什么才能做什么）。

### 〇 · 时间基准（这一条先钉住，后面每一节都受它影响）

| 概念 | 引擎现状 | 状态 | 证据 |
|---|---|---|---|
| 帧 / 当前帧 / 播放头 | **秒**（float）到处跑，没有帧号 | 🟡 | `Skeleton.h:37,45`、`AnimatorSystem.h:69`；UI 在 `ToolkitEditors.cpp:3446`、`EditorApp.cpp:5862` |
| 帧率 FPS | **整条链没有 fps 字段**。只有导入期 FBX 固定 30fps（`FbxLoader.cpp:455`）和运行循环 1/60（`GameLoop.h:44`） | ❌ | "24fps 动画在 30fps 引擎里怎么采样"这件事**从来没被决定过** |
| 起止帧 | `AnimationClip::duration` / `EditClip::duration` | ✅ | `Skeleton.h:45`、`ClipEdit.h` |

**为什么排在最前**：没有帧号，`G` 平移就只能按"秒"给位移；Snap、按住 Shift
吸附到整帧、按帧对齐循环长度（R8 的 `alignCycle` 现在是按秒给的）全都做不干净。
但它一旦改动，`AnimTrack.times`、`EditClip`、`.seq.json` 三处语义一起变 ——
**攒批做，不要零敲**。

### 一 · 整体链路 —— 断在 1、2、3 环

| Blender 步骤 | 引擎对应 | 状态 |
|---|---|---|
| 建骨架（Armature）+ Edit Mode 摆骨 | **只能改不能建**：S-A 的 `Rig` 页能加删骨 / 改长度 / Roll / Connected，但从零建一套 Armature（`Shift+A`）还没有 | 🟡 |
| 绑定模型（`Ctrl+P` 自动权重） | S-B 的**包络自动权重**（只算 Deform 骨）；bone-heat 留后 | 🟡 |
| 蒙皮 / 权重绘制 | S-B：`WeightSet` 顶点组 + `Weights` 页 + 视口色带 / 笔刷 | ✅ |
| 姿态模式摆姿势 + `I` 打帧 | R7 已落地 | ✅ |
| Dope Sheet 调节奏 | R8 已落地 | ✅ |
| Graph Editor 调曲线 | S-D：`BoneCurvePanel` 画的是骨骼（每个标量分量一条曲线，手柄 / 缓动 / 贝塞尔都在）；相机那条仍是 R6 的 `CurveEditorPanel`。**运行时还不读缓动表**（只影响预览） | 🟡 |
| 约束（IK/FK/限位）+ 驱动 | S-C 有了约束栈（Limit / Copy / ChildOf / IK），**但预览之外还没接运行时**；驱动仍无 | 🟡 |
| Action 复用 / NLA 混合 | Action 概念有（`EditClip`），NLA 无 | 🟡 |

一句话：**从"有一份动画"到"调一份动画"通了；从"有一个模型"到"有一份动画"
这一段（1→2→3）仍然是断的。**

### 二 · 建骨架与编辑模式（Armature / Edit Mode）

| Blender | 引擎 | 状态 | 证据 / 缺口 |
|---|---|---|---|
| 骨架对象（Armature） | 没有"骨架资产"这种可创建对象。`assets::Skeleton` 只能由 `MeshLoader::parseSkeleton` / `FbxLoader` 从文件建出来 | ❌ | `Skeleton.h:81` |
| `Shift+A` 新建骨架 | 只能**往已有骨架上加骨**（`RigEdit::addBone` / Rig 页的 `Add child bone`），不能从零建一套 Armature：ContentBrowser 里没有骨架类型 | 🟡 | S-A |
| 编辑模式（`Tab`） | ✅ R7 的 Pose Mode（摆姿势）+ **S-A 的 Rig 页**（改骨头本身）。`EditModes.h` 的十项里仍然没有它们 —— 它们是窗口内的页签，不是顶层编辑模式 | 🟡 | `CharacterWindow.cpp` 的 `drawRigPanel` |
| `E` 挤出新骨（父子自动连） | `Add child bone`：在选中骨的尾巴上接一根，方向/长度沿用父骨 | ✅ | S-A |
| `G` / `R` / `S` 摆骨、骨长 | `head`（DragFloat3）+ `length` + `Align to child`（把朝向/长度对齐到第一根子骨）。**没有视口拖拽** | 🟡 | S-A |
| 命名 `.L` / `.R` + 镜像 | `flipSide` 认 `.L/.R`、`_L/_R`、`Left/Right`；`Mirror to opposite` 按**自动认出来的**镜像轴写过去 | ✅ | S-A |
| Connected（骨尾连父骨头） | 有了：`connected` 标志 + 改父骨尾时子骨的头跟着走（`propagateConnected`） | ✅ | S-A |
| Inherit Rotation | 标志位存下来了（`inheritRot`），**还没人读** —— 约束栈用的是 pose 的局部 TRS，不碰这条标志；它要等"父骨旋转是否传给子骨"进求值 | 🟡 | S-A |
| Deform（是否参与变形） | 标志位存下来了（`deform`），视口里用灰色骨线区分；**S-B 的自动权重已经消费它**（关掉的骨权重恒 0） | ✅ | S-A / S-B |
| Head / Tail 数值 | `head` 三个数 + `length`。Tail 由 `head + dir·length` 导出（glTF 的关节本来就没有尾巴） | ✅ | S-A |
| **Roll（扭转）** | 有了：`roll` 是**独立标量**（相对导入朝向的扭转），视口画三轴给反馈 | ✅ | S-A |
| Rest Pose | 改完 `Save` 成 `.rig.json` 才算资产；`Revert` 回导入态 | 🟡 | S-A |

**S-A（2026-10-29）把这一节从"整片空白"补到了"能改"**：Roll / Connected /
Deform / Inherit / 加删骨 / 左右镜像都有了数据通路与验收。
**还缺的是手感与剩下的消费者**：视口拖骨（`G/R/S`）、从零建一套 Armature 资产，
以及 `Inherit Rotation` 进求值（S-C）。`Deform` 已经被 S-B 的自动权重消费。

### 三 · 绑定模型与蒙皮（权重）

| Blender | 引擎 | 状态 | 证据 / 缺口 |
|---|---|---|---|
| `Ctrl+P` → With Automatic Weights | 有：**包络 / 距离衰减**（`w = clamp(1−d/R)^p`，只算 Deform 骨）。bone-heat 留后 | 🟡 | S-B `WeightSet::autoAssign` |
| Armature Modifier | 引擎没有 modifier 栈。蒙皮是渲染路径里的**硬分支**（pushConstants 多一个 `jointBase`） | ❌ | `Renderer.cpp` 的蒙皮绘制 |
| Preserve Volume（dual quaternion） | 只有**线性混合蒙皮（LBS）**，关节大角度会变瘦 | 🟡 | — |
| Use Deform Bones Only | ✅ `deform` 标志在自动权重里生效（关掉的骨权重恒 0） | ✅ | S-B `AutoWeightParams::deformOnly` |
| Vertex Group（顶点组） | `animation::WeightSet`：每顶点一张 `(bone, weight)` 定长小表（8 条），**不塞进 `assets::Mesh`**，烘焙时才落成四槽 | ✅ | S-B `engine/animation/WeightSet.*` |
| 权重绘制模式（色带 / 笔刷） | `Weights` 页 + 视口色带（红 1.0 / 蓝 0.0）+ 笔刷 Add / Subtract / Set | ✅ | S-B `drawWeightsPanel` / `drawWeightsOverlay` |
| Auto Normalize / X Mirror / Front Faces Only / Blur | 全有：`Auto normalize`（笔刷默认开）、`X mirror`（按 `.L/.R` 换边）、`Front faces`（只刷朝相机的面）、`Smooth`（环邻域模糊） | ✅ | S-B `WeightSet::paint / blur / mirrorVertex` |
| 常见要修的关节（肩肘膝髋脊椎） | 能修：选骨 → 刷权重 → `Bake to mesh`（会 `waitIdle` 后重传顶点缓冲） | ✅ | S-B `applyWeights` |

⚠ 这条最容易被低估：**引擎能渲染一个蒙好的模型，但改不动它的蒙皮**。所以
"权重穿模"这类问题目前只有一条出路：回 DCC。`Preserve Volume` 缺失在
90° 以上的肘弯尤其明显（二分之一的体积丢失）。

**S-B（2026-10-29）把这一节从"整片空白"补到了"能改"**，但三条底线要记住：
① **没编辑过的顶点烘焙出去逐位不变**（`WEIGHTS-BAKE changed=0`）；
② **每顶点权重和恒为 1**（和为 0 会被 `pbr.vert` 当成"不蒙皮"，人一动就粘着皮）；
③ **只给 Deform 骨算权重**（否则控制器骨会把网格一起拽走）。
`WEIGHTS-AUTO-LOWCOVER` 还会把"这具资产的静止世界位置和网格对不上"这种
**静默失败**变成日志里能看见的数（实测 6.3% 顶点落空）。


### 四 · 姿态模式（Pose Mode）

| Blender | 引擎 | 状态 | 证据 |
|---|---|---|---|
| 左键选骨 | 骨骼树列表 + 视口点选 | ✅ | `CharacterWindow.cpp` 的 `drawPosePanel` |
| `R` 旋转 | 三轴旋转 gizmo（拖拽角按**世界轴**算，再经父骨世界四元数共轭转成局部增量） | ✅ | `drawBoneGizmo` |
| `G` 移动 / `S` 缩放 | **没有平移/缩放 gizmo**，只能靠 `I` 打帧后的数值回读 | 🟡 | `CharacterWindow.h:219` 只有旋转 gizmo |
| `I` 插关键帧（Loc / Rot / Scale） | Rotation / Location / Scale 可分别打，`K` 全打 | ✅ | `keySelectedBone` |
| Whole Character（只记有变化的骨） | 没有"只记变化骨"的批量键；靠"碰过哪根打哪根"的瞬态层近似 | 🟡 | `m_touched*` |
| 旋转底层是四元数（Graph 里 X/Y/Z/W 四条） | 存的是四元数，但**曲线编辑器画不到骨骼**（见第七节） | 🟡 | — |
| 摆姿势不自动打帧 | 刻意这样：瞬态层，跳帧即丢 | ✅ | `clearTransient()` |

### 五 · 骨骼约束（Bone Constraints）

| Blender | 引擎 | 状态 | 证据 / 缺口 |
|---|---|---|---|
| **约束栈**（自上而下依次算，每条有 `Influence` 0~1 且可打帧） | `animation::ConstraintStack`：骨上一条**有序**的表，按 `items` 顺序求值；每条有 `enabled` / `influence`（可打帧）| ✅ | S-C `engine/animation/Constraint.*` |
| IK（`Chain Length` / `Pole Target` / `Iterations` / `Stretch`） | 两骨**解析解**（余弦定理）+ 极向量两种模式 + `stretch`；链长固定 2（`ChainLength` 靠 root/mid/end 三个下标表达），无迭代解（FABRIK 留给多骨链）| 🟡 | S-C 把 `TwoBoneIK` **薄包装**进栈 |
| Copy Rotation / Copy Location | 有：世界旋转 / 世界头部位置（+偏移）+ 轴掩码 | ✅ | `applyCopyRotation` / `applyCopyLocation` |
| Limit Rotation（肘不能反折） | 有：局部欧拉每轴 min/max + 轴掩码；**限幅以内一个字节都不写** | ✅ | `applyLimitRotation` |
| Child Of（手抓扶手，`Influence` 0→1 过渡） | 有：rest 时自动推出 inverse（= Blender 的 `Set Inverse`），世界变换 = 目标 × 偏移 | ✅ | `applyChildOf` |
| Action Constraint | 无 | ❌ | — |
| Look At / Spring Bone / Stretch | `LookAt.h` / `SpringBone.h` —— 有，但仍在 `applyPerformance` 里按固定顺序跑，**还没搬进栈** | 🟡 | 同左 |

⚠ 关键判断（S-C 之前）：IK/LookAt/Spring 是"**三个特性**"，不是"**一套约束**"——
特性之间不能排序、不能叠、不能统一打 `Influence` 关键帧。S-C 之后前三条有了栈，
`LookAt` / `SpringBone` 仍是栈外的固定段（它们有**跨帧状态**，搬进"无状态求值
器"要先把状态也变成数据，留给 R10 之后）。

**S-C（2026-10-29）落地了什么**：五条约束（`LimitRotation` / `CopyRotation` /
`CopyLocation` / `ChildOf` / `TwoBoneIK`）+ 一条有序栈 + JSON 往返 + Rig 页的
编辑段。三条底线写死在求值器里：

1. **`influence <= 0` 或关掉 → 一个字节都不写**（不是"乘 0 写回去"：
   `slerp(q,q,0)` 在浮点下不是恒等，长跑会让角色慢慢转歪）；
2. **限幅以内也一个字节都不写**（否则"给没超限的骨加个限位"会让整条轨道漂）；
3. **世界矩阵按需重算**（`LimitRotation` 不看世界；`Copy*` / `ChildOf` / IK 必须
   看到**上一条改完之后**的世界矩阵，否则"先转上臂再复制给前臂"少一个旋转量，
   且只在两条以上约束时复现）。

**还差**：`LookAt` / `SpringBone` 进栈（需要把跨帧状态数据化）、
`Action Constraint`、**运行时（`AnimatorSystem`）读同一份栈** —— 那一步要动
`engine/ecs/Components.h`（GLOBAL_GLOBS → 全量回归），所以按第 5 节约束 1 攒批。

### 六 · 动画摄影表（Dope Sheet）

**✅ 有**（R8，2026-10-28，`editor/DopeSheetWindow.{h,cpp}`）。通道树（骨骼层级 /
实体属性）+ 一行一根骨 + 关键帧菱形 + 框选 / `G` 平移 / `M` 镜像 / `Y` 对齐循环
长度 + `Ctrl+滚轮` 绕鼠标缩放 / `Shift+滚轮` 平移 / 纵向滚动。

- 打开两条路：`MYVK_EDITOR_DOPE=<路径>`（独立打开一份 `.anim.json`）、
  Character 窗口 Pose Mode 里的 `Dope sheet` 按钮（**绑定**形态：数据源由宿主
  推过来，存盘被拒 `reason=bound` —— 一个文件两个写者是后存的静默盖掉先存的）；
- 关键帧类型 `Extreme / Breakdown / Inbetween` 只在**形状**上编码（实心+外圈 /
  空心 / 小圆点），**完全不进求值**，只管读节奏；
- 批量操作的三条实现（`moveKeys` / `mirrorKeys` / `alignCycle`）住在
  `engine/animation/ClipEdit`，与注入脚本共用同一份 —— 验收因此不依赖鼠标坐标；
- **还差**：帧号（见第〇节）、实体属性轨（要先动 `Components.h`）、
  `S` 缩放键间距、`Shift+D` 复制。

### 七 · 曲线编辑器（Graph Editor）

| Blender | 引擎 | 状态 | 证据 |
|---|---|---|---|
| F-Curve（值随时间的曲线） | R5 落地 `animation::Curve`（`Interp` / `HandleType` / `Extrapolate` / 7 种 `EaseShape`） | ✅ 数据层 | `Curve.h:47,78,87` |
| 曲线视图 | R6 落地 `editor/CurveEditorPanel.*` | ✅ 视图 | — |
| **相机那条曲线** | 数据源写死 `sequence::SequenceAsset`，通道是 `PosX/Y/Z / Yaw / Pitch / Fov` | ✅ | `CurveEditorPanel.h:107,116` |
| **骨骼那条曲线** | S-D 落地 `editor/BoneCurvePanel.*` + `engine/animation/CurveBake.*`：`EditTrack` 的每个标量分量一条曲线 | ✅ | `CurveBake.h` |
| 骨骼轨道的插值 | `EditTrack::ease`（每键 `interp / ease / shape / handle / 两根手柄`）；`assets::AnimTrack` 仍是 `bool step`，**一个字节没改** | ✅ | `ClipEdit.h` 的 `KeyEase` |
| 手柄（`V` 键切 5 种） | 相机：V = 换算并写回（写完就是 Free）。骨骼：V = **存类型**，AutoClamped 会一直跟邻居重算 | ✅ | `BoneCurvePanel.cpp` |
| 缓入缓出（Sine / Quad / Cubic / Back / Bounce / Elastic） | 7 种形状 × 4 种方向，两套曲线共用同一个 `easeFn` | ✅ | `Curve.h:64` |
| 约束 `Influence` 能进曲线 | 无约束，自然也没有 | ❌ | — |
| 曲线修改器（Cycle / Noise / Sine） | `Extrapolate::Cycle*` 三种有；Noise/Sine 无 | 🟡 | `Curve.h:87` |

⚠ 一句话（S-D 之前）：**曲线核心与视图都齐了，但没接到骨骼上**。把 `AnimTrack`
换成 `Curve` 是第七节唯一要做的事，也是"动作有生命"的前提（`Auto Clamped` 手柄 +
缓入缓出）。这件事的代价在于 `AnimTrack` 是导入产物、`Curve` 是编辑产物 ——
跟 R7 一样，得**单向烘焙**（`EditClip` → `Curve`），不能就地改。

✅ **S-D 已落地（2026-10-29）**，见下面的 S-D 章节。两条曲线**没有**合并成
一个面板 —— 手柄的归属根本不同（相机是"一段一组归一化、四通道共用"，骨骼是
"每键绝对偏移、分量独立"）。

### 八 · 姿态库（Pose Library）

| Blender | 引擎 | 状态 | 证据 / 缺口 |
|---|---|---|---|
| 摆好姿势 → `+` 保存 → 一键套用 | `EditPose` + `storePose` / `applyEditPose`，控制台 `posestore:` / `poseapply:` | ✅ | `ClipEdit.h:119,146,177,270` |
| 缩略图 / 资产级复用（跨角色） | 没有。姿态**绑在 clip 里**（`.animclip.json` 的 `poses` 数组），换角色就没有 | 🟡 | `ClipEdit.h:146` |
| Idle 基准姿 / 手势预设 | 能存能取，但没有独立库文件，也没有"应用时忽略缺失骨"的跨骨架兼容 | 🟡 | — |

### 九 · Action 与 NLA

| Blender | 引擎 | 状态 | 证据 |
|---|---|---|---|
| Action（可复用动画） | 导入的只读 `assets::AnimationClip`（`Skeleton.h:43`）+ 可写回的 `animation::EditClip`（`ClipEdit.h`） | ✅ | — |
| Action Editor（改名 / 切片段） | Character 窗口的 clip 列表（点击即播）+ Pose Mode 的基础片段选择 | 🟡 | `CharacterWindow.cpp:1170` |
| NLA（片段当 strip 拼） | **无** | ❌ | `Components.h:241` 的 `layers` 是"按骨覆盖"，不是时间轴片段 |
| 上层混合（走路 + 挥手） | 层 + 骨骼遮罩能做到 | ✅ | `AnimatorSystem.cpp:141` |
| Blend Mode / 速度缩放 / 交叉淡入 | 无 | ❌ | — |

### 十 · 速查表 → 术语对照

| Blender | 本引擎 | 备注 |
|---|---|---|
| Armature / Bone | `assets::Skeleton` / `Skeleton::Joint` | 导入产物只读；**编辑态是 `animation::RigEdit`**（S-A） |
| Edit Mode（骨骼） | `RigEdit` + Character 窗口的 `Rig` 页 | S-A；还差从零建 Armature |
| Rest Pose / Roll | `Joint::restLocal`（4×4）/ `EditBone::roll`（标量） | S-A：Roll **可单独编辑**，烘焙时才进 `restLocal` |
| Connected / Inherit Rotation / Deform | `EditBone::connected / inheritRot / deform` | S-A 存下来；`deform` 已被 S-B 消费，`inheritRot` 等 S-C |
| Vertex Group / Weight Paint | `animation::WeightSet` + `Weights` 页 | S-B（`assets::Mesh` 零改动） |
| Preserve Volume（DQS） | ❌（只有 LBS） | — |
| Keyframe | `assets::AnimTrack.times/values[i]`（平行数组）；实体侧 `ecs::TransformKey` | 两种，语义不同 |
| Key Type（极端/间断/中间） | `KeyType`（R8），纯标记 | 不进求值 |
| F-Curve | `animation::Curve`（R5） | **骨骼轨道还没接过来** |
| Interpolation | `Interp`（Constant/Linear/Bezier） | 骨骼轨道仍是 `bool step` |
| Handle | `HandleType` + `{dt,dv}` **相对**手柄（R5） | Blender 存绝对坐标 |
| Easing / Extrapolation | `EaseMode × EaseShape`（7 种）/ `Extrapolate`（5 种） | — |
| Constraint | ❌（LookAt / TwoBoneIK / SpringBone 是三个独立特性） | 地基缺口 4 |
| NLA Strip / Driver | ❌ / ❌ | R9 / R10 |
| Pose Library | `EditPose`（存在 clip 里） | 不能跨角色 |
| Shape Key | `MorphRig` 通道，**不可打帧** | 表情动画做不了 |

### 十一 · 最小示例（"抬手"）—— 当作验收剧本

拿它当"这条链通不通"的体检表，比任何功能列表都直观：

| 步骤 | 现在能不能做 |
|---|---|
| 1. 帧 1，手臂下垂，`I` → LocRotScale | ✅ Pose Mode 能打 |
| 2. 帧 20，选上臂，绕 X 转 90°，`I` | ✅ |
| 3. 播放：自动补间 | ✅ 线性补间 |
| 4. Graph Editor 选上臂 Rotation 曲线，手柄改 Auto Clamped，做起手慢/中间快/落手缓 | ❌ **曲线编辑器打不开骨骼通道**（第七节） |
| 5. 加 IK，拖手部 IK 目标，上臂/前臂自动跟 | 🟡 能：约束栈里加一条 `TwoBoneIK`（选链三骨 + 目标点 + `Stretch`），控制器骨也有（S-A 的 `deform=false`）。**但预览之外（Game.exe）还没读这份栈** |
| 6. 权重检查：肩不穿模、肘不塌陷 | 🟡 有工具了（S-B 的 `Weights` 页 + 色带 + 笔刷），但体积损失只能"看着不对就回 DCC"—— 没有 `Preserve Volume` |
| 7. Action 命名 `wave`，Push Down 备用 | 🟡 能存成 `.animclip.json`，没有 Push Down（R9） |

**七步里 5 步能走完（1/2/3/5/6），第 4 步是纯空白，第 7 步缺上游（R9 的 Push Down）。**


## 2 · R5 落地了什么（2026-10-28）

### 2.1 `engine/animation/Curve.{h,cpp}` —— 全仓唯一的曲线求值器

做之前，**三份互不相干的插值实现**：

| 位置 | 能力 |
|---|---|
| `assets::sampleTrack`（`Skeleton.cpp:97`） | STEP / 线性 / slerp，**无缓动** |
| `AnimationEditor::easeKey`（`ToolkitEditors.cpp:3207`） | 硬编码 smoothstep |
| `sequence::sampleCamera`（`SequenceAsset.cpp`） | linear / smoothstep / step |

现在它们上面的"缓动与贝塞尔"收敛到一份：

- **插值**：`Interp::{Constant, Linear, Bezier}`
- **缓动**：`EaseMode::{None,In,Out,InOut}` × `EaseShape::{Linear,Sine,Quad,Cubic,Back,Bounce,Elastic,Smooth}`
  —— 7 个形状只写 7 条公式（统一成 "In" 形，Out/InOut 由它镜像），不存在
  "In 和 Out 是两套无关实现"。
- **手柄**：`HandleType::{Auto,AutoClamped,Vector,Aligned,Free}`；自动手柄用中心差分，
  `AutoClamped` 在局部极值处压平（**不过冲**）。
- **外推**：`Hold / Linear / Cycle / CycleOffset / CycleFlipped`
- **求根**：`bezierSolveU` = Newton 8 次 + 二分 20 次兜底。**二分不能省** ——
  手柄接近竖直时 Newton 会来回震荡，而这条路径每帧都跑。

三条设计决定（都会在代码里咬人，所以写在这里）：

1. **手柄存相对偏移 `{dt, dv}`**，不存绝对坐标。关键帧时间被拖走时手柄自动跟随，
   而且"斜率 × 段长/3"本来就是相对量。
2. **`out.x >= 0`、`in.x <= 0` 是硬约束**。x 非单调 → 贝塞尔在 x 上无唯一解 →
   表现是"镜头在某一刻往回调了一下"，而且**日志里什么都看不出来**。加载期钳回。
3. **缓动与贝塞尔互斥**。设了手柄还要再被 smoothstep 压一次的话，曲线永远到不了
   手柄指的位置，作者会觉得"手柄失灵"。

### 2.2 第一个真实消费者：过场相机段支持 `bezier`

`sequence::Ease` 加了第 4 档 `bezier`，`SeqCameraKey` 带一组**归一化**手柄
（x = 这段时间的百分比，y = 进度百分比）。归一化不是偷懒：这一段只有一个
标量时间重映射 `w`，position / yaw / pitch / fov 共用它，所以一组手柄就该管四个通道。
（Blender 存绝对值，因为它的每个通道是独立 F-Curve。）

`sampleCamera` 里四个模式收敛成**一个 `w` 出口**：

```cpp
const float w = animation::evaluateSegmentEase(ip, em, sh, a.outHandle,
                                               a.inHandle, u);
```

⚠⚠ **老三个模式逐位不变**，这是整个改动能不能上线的唯一门槛：

- `linear` → `Interp::Linear + EaseMode::None` → 恒等映射，`w == u`；
- `smooth` → `Linear + InOut + Smooth` → `easeFn` 里**特判成 Hermite smoothstep**，
  逐位等于 R4 的 `smoothstep01()`；
- `step` → 早退（它连 `out.t` 都要保持 `a.t` —— 它不是插值，是选择）。

证据：`SequenceSelfTest` 的 `seq_sample_legacy_bitwise`（11 个采样点，逐位）+
`seq_step_keeps_a_time`。

> ⚠ 为什么 `Smooth + InOut` 要特判：通用公式是"半段形状 × 0.5"，
> 在 u=0.25 处给 0.25，而 smoothstep 给 0.15625。差 0.09 在运镜上是**看得出来的
> 起手速度变化**，而这一层的目的从来不是"改观感"。

### 2.3 时间轴编辑器的 UI

`TimelineEditorWindow` 的 ease 下拉多了 `bezier`；选中 bezier 段时属性栏多两行：
说明 + 四个手柄拖条（out x / out y / in x / in y）。日志锚点 `TLED-BEZIER`，
自动化 token `sethandle:<out|in>:<x|y>:<值>`。

⚠ 日志打在 `IsItemDeactivatedAfterEdit` 那一侧（松手才打）—— 拖一下会触发几十次
`DragFloat`，每次都打的话 `TLED-` 日志会被自己刷满。

### 2.4 验收

| 用例 | 断言数 | 说明 |
|---|---|---|
| `verify_anim_runtime` | **+27 条** `ANIM-TEST curve_* / ease_* / handle_* / segment_*` | 贝塞尔求根、四种外推、七种缓动、三种手柄、两入口一致性 |
| `verify_sequencer` / `--selftest sequence` | **+7 条** `SEQ-TEST seq_sample_legacy_bitwise` 等 | 老模式逐位不变 + 手柄真的改曲线 + 手柄写反被钳回 + round-trip |
| `verify_timeline_editor` | 沿用 | 属性栏多两行，`TLED-PROP-BEZIER` 锚点 |

---

## 3 · 阶段计划（按作者链排序）

> 排序原则：**先让"已有数据"可编辑，再造"没有的数据"**。反过来的话，新造的数据
> 没有工具用，只能躺在 JSON 里。
>
> 2026-10-29 重排：上一版从 R5 往后接力（R6 Graph → R7 Pose → R8 Dope →
> R9 NLA → R10 Drivers），做完三轮才发现**这条链的头两环没人管**：
> 骨架从哪来、权重怎么改。所以新增 **S-A / S-B / S-C** 三个阶段插在 R9 之前。
> R5–R8 已落地、不再改；R9/R10 顺延但不取消。

| 阶段 | 内容 | 规模 | 状态 |
|---|---|---|---|
| R5 | 曲线求值核心 `animation::Curve` | M | ✅ |
| R6 | 曲线视图 `CurveEditorPanel`（相机通道） | M | ✅ |
| R7 | 骨骼关键帧 + Pose Mode + 姿态库 | L | ✅ |
| R8 | Dope Sheet + 关键帧类型 + 批量操作 | M | ✅ |
| **S-A** | **骨架作者：可编辑 Rig（骨长 / Roll / Connected / Deform / Inherit）+ Edit Mode** | **L** | **✅ 已落地（2026-10-29）** |
| **S-B** | **蒙皮与权重：顶点组 + 自动权重 + 权重绘制** | **L** | **✅ 已落地（2026-10-29）** |
| **S-C** | **约束栈：Limit / Copy / Child Of + IK 迁入栈（Chain / Pole / Influence）** | **M/L** | **✅ 已落地（2026-10-29）** |
| **S-D** | **骨骼曲线接入 Graph Editor（`EditTrack` ⇄ `Curve` 单向烘焙）** | **M** | **✅ 已落地（2026-10-29）** |
| R9 | NLA | L | 待做 |
| R10 | Drivers + Motion Paths + 序列帧导出 | M/L | 待做 |

### S-A · 骨架作者（Rig Edit）· 规模 L —— ✅ 已落地（2026-10-29）

**为什么排第一**：没有 `Roll`，程序化摆手脚必然绕错轴；没有 `Connected`，加一根
武器骨就要手算局部矩阵；没有 `Deform` 标志，IK 控制器骨会跟着网格一起变形。

- **数据层**：`engine/animation/RigEdit.{h,cpp}`（**`engine/assets/Skeleton.h`
  一个字节没改**，守第 5 节约束 1）：
  - `EditBone { name, parent, srcJoint, head, dir, length, roll, deform, connected,
    inheritRot, restLocalOrig, externalChain, restHeadOrig, restDirOrig }`；
  - **`roll` 单独存**，语义是"相对导入朝向的扭转"（0 = 保持导入的样子）——
    这样"只修手"不会顺手把整具骨架的初始朝向也改一遍；
  - **骨长与朝向是猜出来的**（`inferBoneGeometry`）：glTF/FBX 的关节**没有骨长**，
    旋转矩阵也不告诉你是哪个轴朝外。规则是"到**第一根**子骨的距离"，叶骨沿父骨
    方向延一半。⚠ 这条是启发式，写在这里是因为改它会让所有骨的长度一起变；
  - **烘焙**（`bake`）：没动过的骨逐位照抄 `restLocalOrig / invBind`，动过的骨
    （及其子孙）由 `head/dir/roll` 重算 `restLocal`、由 `inverse(restWorld)` 重算
    `invBind`。`nodeMap` 清空（加/删骨之后 glTF node 下标不再对应）。
  - **镜像轴是从数据里认的**（`detectMirrorAxis`）：在每对 `.L`/`.R` 上量三根轴的
    分离量取最大者。实测 `anime_girl.fbx` 的左右是 **Y** 轴（肩的 x 只有 ±0.03、
    y 是 ±0.11），写死 X 的表现在画面上是"镜像完人朝后仰"而日志干净 ——
    和 Roll 是同一个坑，只是换成左右。
- **UI**：Character 窗口左栏第四页 `Rig`（不在的话页签不出现）——
  骨树（`*` = 动过）、`Head / Length / Roll(deg) / Deform / Connected /
  Inherit rotation`、`Align to child`、`Mirror to opposite`、`Add child bone` /
  `Delete bone`、镜像轴下拉、`Save / Load / Revert`。
  视口叠加（`drawPreviewOverlay` 的 ①b）画 rest 骨线与**选中骨的三轴** ——
  三轴是必须的：一根线绕自己转是**看不出来**的，roll 没有三轴就没有反馈。
- **验收**（`tools/verify_rig_edit.py`，**结果: PASS（28 项）**）：
  - A 组 17 条 `rig_*` 断言（`MYVK_ANIM_TEST`，纯数值、不开窗）：
    其中 `rig_bake_identity` 钉"零编辑烘焙逐位一致"，`rig_rest_no_deform` 钉
    "改 rest 之后静止姿态**不**变形"（`invBind` 重算正确），
    `rig_bake_untouched_keeps` 钉"只重算脏子树"；
  - B 组 `roll:90` → 局部 X 轴绕着骨轴转了 **90.000°**（且仍垂直于骨轴）；
  - C 组 改一根骨，**非子孙**的世界位置用 `%a` 比字符串**逐位不变**；
  - D 组 两次存盘比对：未编辑骨的 `rest` 矩阵 16 个数逐位相同（没被烘焙污染）；
  - E 组 镜像（选**最对称**的一对 `thigh.L/R`，自身差 0.00 mm）；
  - F 组 13 个 UI 锚点 + 零 VUID + 零 `[ERROR]`。
- **还差**（留给后面）：
  - 从零**新建一套 Armature 资产**（`Shift+A`）—— 现在只能"往已有骨架上加骨"，
    ContentBrowser 里没有骨架类型；
  - 视口里**拖骨**（现在改 head/len 靠数值 + `Align to child`，没有 `G/R/S` 手感）；
  - `Deform / Inherit Rotation` 只是**存下来了**，还没人读（S-B / S-C 才消费）。

### S-B · 蒙皮与权重（Skin & Weights）· 规模 L —— ✅ 已落地（2026-10-29）

**为什么排第二**：S-A 让骨架"能改"，但网格怎么跟上去还是黑的。这里补的是
**"改得动蒙皮"**：顶点组、自动权重、权重绘制三件，缺一件都得回 DCC。

- **数据层**：`engine/animation/WeightSet.{h,cpp}`（**`assets::Mesh` 与
  `Skeleton.h` 一个字节没改**，守第 5 节约束 1）：
  - 每顶点一张 `(bone, weight)` 小表，**定长存**（`entries[v * kMaxInfluence]` +
    `count[v]`，`kMaxInfluence = 8`）—— `vector<vector<>>` 的堆块在每帧几万顶点
    的随机访问下是纯亏；GPU 只吃 4 槽，多出来的在 `bake` 里按权重截断；
  - 同 `RigEdit` 一样存 `origJoints / origWeights` 快照：**没编辑过的顶点烘焙时
    逐位照抄**（`memcmp` + 拷贝），这是 S-B 唯一的安全网 —— 读出来 → 排序 →
    写回去这条路**一定**会以 1e-7 量级漂移，所以"零编辑烘焙 changed=0"是硬断言；
  - **排序必须全序**：`entryBetter` = 权重降序 + **骨下标升序**。只按权重排会让
    等权条目在不同 `std::sort` 下换位，同一份工程两次烘焙出不同的 `JOINTS_0`；
  - `digest()`（FNV-1a 64）给跨进程确定性用；`dumpVertex` 用 `%a` 打十六进制
    浮点，是为了让"逐位"能被真的比，而不是比 `%.6f`。
- **自动权重**（包络 / 距离衰减，**bone-heat 留后**）：
  `w = clamp(1 − d/R, 0, 1)^falloff`，`R = 骨长 × radiusScale`，归一化后取前
  `maxInfluence` 条。**只给 `deform` 骨算** —— S-A 那个标志的第一个真实消费者
  （`WEIGHTS-AUTO-SKIP-BONE`）。
  - ⚠ `EditBone::head` **已经是绝对位置**（骨架空间），不要再乘一次
    `RigEdit::restWorld()`：乘了的表现是"每根骨的作用半径中心离身体远一倍"
    （日志里 chest 从 z=0.5 变 z=1.0），画面上是权重整体偏移。S-B 为此返工一次。
  - `WEIGHTS-AUTO-COVER` / `WEIGHTS-AUTO-LOWCOVER` 是"静默失败"的探针：实测
    `anime_girl.fbx` 有 **714/11338 = 6.3%** 顶点落在所有骨的作用半径之外
    （那具资产的节点默认变换不是绑定姿态，腿骨的静止世界位置在骨盆上方 ——
    `restWorld × invBind = I` 让它在静止时完全看不出来）。落空率 > 50% 会 WARN。
- **UI**：Character 窗口左栏第四页 `Weights`（在 `Rig` 与 `Animation` 之间，
  只对**有骨架**的模型出现）—— 网格选择、统计（顶点数 / 未赋权 / 影响数直方图 /
  权重和区间）、`AUTO WEIGHTS`（Radius / Falloff / Max infl / Deform bones only）、
  `BRUSH`（Add·Subtract·Set / Weight / Brush size / Softness / Auto normalize /
  X mirror / Front faces / Target bone）、`Normalize All` / `Smooth` /
  `Bake to mesh` / `Save` / `Load` / `Revert` / `Show weights`。
  视口叠加画**权重色带**（红 1.0 / 蓝 0.0，按屏幕坐标暴力投影前 N 个顶点）。
- **注入脚本**：`MYVK_EDITOR_WEIGHTS_SCRIPT`（`list / mesh / sel / auto / autoall /
  paint / mode / autonorm / norm / blur / clear / mirror / bake / digest / dump /
  stats / save / load / revert`），收尾打
  `WEIGHTS-SCRIPT applied=N failed=N meshes=N active=N sel=N dirty=N`。
- **验收**（`tools/verify_weights.py`，**结果: PASS（37 项）**）：
  - A 组 15 条 `weights_*` 自检（`ANIM-TOTAL pass=253 fail=0`）：
    `weights_bake_identity` 钉"零编辑逐位一致"、`weights_auto_sum_one` 钉
    "每顶点权重和恒为 1"、`weights_auto_only_deform` 钉"关掉 deform 的骨权重为 0"、
    `weights_bake_mask_scoped` 钉"被关掉的骨只影响真用到它的顶点"
    （否则约定 ① 在最常见路径上失效：关一根骨 = 一万个顶点全重算）；
  - B 组 真资产跑一遍（7 张网格 / 11338 顶点）：覆盖率有数、`sum ∈ [1,1]`、
    `unweighted=0`、`over4=0`、烘焙后**顶点缓冲重传**过；
  - C 组 关掉 `shin.L` 的 deform → `SKIP-BONE` 命中、摘要变、仍无孤立顶点；
  - D 组 跨进程摘要一致；三种笔刷模式（Add/Subtract/Set）在关归一化时数值
    各按定义走，开归一化后 `sum == 1`；
  - E 组 **零编辑烘焙 `changed=0` 且不重传**（S-B 最重要的一条）、Revert 后仍 0；
  - F 组 `venn.weights` 的字段 / 版本 / 稀疏条目 / 往返摘要不变；
  - G 组 镜像找对侧（6/6 命中）+ 骨名换边（`.L` ↔ `.R`）；
  - H 组 24 个 UI 锚点 + 零 VUID + 零 `[ERROR]` + `failed=0 dirty=0`。
- **两条约定**（写进代码注释，改之前先读）：
  - **重传顶点缓冲前必须 `device().waitIdle()`**：`Mesh::upload` 内部
    `delete m_vertexBuffer`，在飞的命令缓冲还引用它 → 不先等空闲就是
    `VUID-vkDestroyImage/DestroyBuffer` + 花屏。只在松手时付一次全同步。
  - **权重和恒为 1**：`pbr.vert` 按 `weights` 之和 > 0 分支，和为 0 与全 0 无法
    区分 → 顶点留在网格空间，角色一动就"身上粘着一坨皮"。所以笔刷默认
    `Auto normalize`，`normalizeAll` 也是个显式按钮。
- **还差**（留给后面）：
  - **bone-heat 自动权重**（现在只有包络：外壳类角色够用，手指/耳廓这种薄片
    会互相抢权重）；
  - 骨架侧的 `Preserve Volume`（dual quaternion，只有 LBS）；
  - 权重**渐变/软选择**（现在笔刷是硬圆 + 柔度曲线，没有"按拓扑距离"的刷法）；
  - `WEIGHTS-AUTO-LOWCOVER` 暴露的**资产问题**（`anime_girl.fbx` 的节点默认变换
    不是绑定姿态）—— 这是导入器该修的，不该让权重工具兜。


### S-C · 约束栈（Constraint Stack）· 规模 M/L —— ✅ 已落地（2026-10-29）

**为什么排第三**：S-A/S-B 让骨架和蒙皮"能改"，但"骨该怎么动"仍然只有三条写死的
后处理。约束栈把这件事变成**数据**：顺序、开关、`Influence` 都在骨上。

- **数据层**：`engine/animation/Constraint.{h,cpp}`（**`assets/Skeleton.h` 与
  `ecs/Components.h` 一个字节没改**，守第 5 节约束 1）：
  - `Constraint { type, enabled, influence, bone, target, axisMask, offset,
    minDeg[3], maxDeg[3], limitMask, ik*…, stretch }`，五型：
    `LimitRotation / CopyRotation / CopyLocation / ChildOf / TwoBoneIK`；
  - `ConstraintStack` = `vector<Constraint>`，**`items` 的顺序就是求值顺序**；
    `add / removeAt / moveUp / moveDown / clear / clearBone / digest`；
  - **骨一律用下标**（`bone` / `target` / `ikRoot|Mid|End` 都是 joint 下标）：
    约束栈跟的是**一具具体骨架**，改名或换骨架时就该失效，而不是悄悄绑到另一根
    同名骨上。落盘时名字+下标都写（名字给人看、也让"重导入后下标变了"可见），
    读盘只用下标并校验越界 —— **不按名字猜**；
  - `digest()`（FNV-1a 64）把顺序也吃进去：`C1/C2` 就是靠它证明"移动之后摘要
    必变、移回来必复原"。
- **五条约束怎么算**（每条都写在 `Constraint.cpp` 的注释里）：
  - `LimitRotation`：局部欧拉每轴 min/max（`glm::eulerAngles` 那一套约定），
    **限幅以内直接返回、不 decompose→recompose**；
  - `CopyRotation`：目标骨的**世界**旋转 → 反解成自己的局部旋转（父骨世界旋转
    共轭），轴掩码 ≠ 7 时才过一次欧拉（那一步有 ~1e-7 精度损失，掩码 = 7 走纯
    四元数、逐位精确）；
  - `CopyLocation`：世界头部位置 + offset → 换回父骨空间；**不是**直接抄局部
    平移（父骨被别的约束转过之后会差一个父骨旋转——"跟着走却走错方向"）；
  - `ChildOf`：世界变换 = 目标骨世界 × **rest 时量出来的偏移**（等价于 Blender
    的 `Set Inverse`，只是我们不让用户点那一下）。⚠ 对**目标骨的后代**是恒等
    操作 —— 父子链本来就会带它走，所以它真正的用途是**跨层级**（另一条链上的
    武器/道具骨跟手）；
  - `TwoBoneIK`：**薄包装** `solveTwoBoneIK`，不重写余弦定理（`maxReachScale =
    0.999` 那个 NaN 坑与"极向量从当帧动画反推"只该有一份实现）。`stretch` 是
    解算前的一步：按倍数放大 mid/end 的**局部平移长度**（glTF 里骨长就是子骨的
    平移量），不动旋转 —— 于是"够得更远"与"朝向对不对"互不干扰。
- **求值器**（`ConstraintStack::evaluate`）：`world` 懒算、每条写完置脏、
  `restWorld` 更懒（只有 `ChildOf` 要）；返回一份账
  `{total, applied, skipped, failed, clamped, ikSolved}`，`CONSTRAINT-EVAL` 日志
  与面板上的统计都吃它。
- **UI**：Character 窗口 **Rig 页**里的 `CONSTRAINTS` 折叠段（不新开页签 ——
  左栏 380px 装不下第 6 个页签，见 `draw()` 里 `leftW` 那段注释）：只列**当前
  选中骨**的条目（保持全局顺序）、启用勾选、类型下拉 + Add / Delete / Move up /
  Move down、`Influence` 滑条、按类型展开的参数（限位三轴 min/max、目标骨、
  轴掩码、偏移、IK 的链三骨 / 目标点 / `Stretch`）。
  预览的采样链 `samplePoseInto()` 里一行 `m_constraints.evaluate(skel, m_pose)`
  —— 空栈时立刻返回，所以"没有约束 = 逐位等于旧路径"。
- **注入脚本**：`MYVK_EDITOR_CONSTRAINT_SCRIPT`
  （`add / sel / del / move / influence / enable / target / limit / mask / ik /
  iktarget / stretch / clear / list / digest / eval / save / load`），收尾打
  `CONSTRAINT-SCRIPT applied=N failed=N n=N sel=N dirty=N joints=N`。
- **验收**（`tools/verify_constraints.py`，**结果: PASS**）：
  - A 组 23 条 `constraint_*` 自检（`ANIM-TOTAL pass=276 fail=0`）：
    限幅夹到 20.000°、**限幅内 `memcmp` 逐位不变**、influence 0.5 = 40°（slerp）、
    复制旋转世界差 0.000°、复制位置距离 0.000、轴掩码只动 X、
    ChildOf 偏移误差 0.000（**对照组** 0.273 证明不是空转）、
    **顺序 A = 60° / 顺序 B = 20°**、IK 入栈与直接求解**逐位相同**、
    IK influence 0 逐位不动、influence 0.5 的误差严格落在两端之间、
    `stretch 0.5` 把 0.223 的误差压到 0.00095；
  - B 组 真资产接线：三条条目全落在合法骨上、`applied + skipped == total`、
    `failed == 0`、**跨进程摘要一致**；
  - C 组 `move` 的往返（摘要变 → 复原）；
  - D 组 `influence 0` / 关掉 → `applied == 0 && skipped == 1`（不是"算个 0"）；
  - E 组 `venn.constraints` 字段 / 版本 / 限位数值 / **存读往返摘要不变**；
  - F 组 IK 在真骨架上解算成功（`ik=1`）；
  - G 组 17 个 UI 锚点（分三次开窗：限位 / 复制 / IK）+ 零 VUID + 零 `[ERROR]`。
- **还差**（留给后面）：
  - **运行时读同一份栈**：`AnimatorSystem::applyPerformance` 还是写死的三段，
    搬过来要动 `ecs/Components.h`（GLOBAL_GLOBS → 全量回归），按约束 1 攒批；
  - `LookAt` / `SpringBone` 进栈（它们是**有状态**的，`evaluate` 目前无状态）；
  - `Action` 约束、`Limit Location`；
  - `stretch` 目前只拉 mid/end 两段平移，**没有**"骨骼不拉长、只把末端伸出去"
    那种拉伸 IK 语义（Blender 的 `Stretch To` 是另一回事）。


### S-D · 骨骼曲线（Graph Editor 接到骨骼上）· 规模 M —— ✅ 已落地（2026-10-29）

**为什么排第四**：S-A/S-B/S-C 让"骨该怎么动"能改骨架、能画权重、能挂约束，
但这三件事全都踩在**同一条匀速直线**上 —— `assets::AnimTrack` 只有 `bool step`
（常量 / 线性）。所以一次"抬手"永远是两条匀速线段：起手和落手一个速度，
谁看都是机器做的。曲线是把**节奏**变成数据的最后一环，也是"动作有生命"的前提。

- **数据层**：`engine/animation/CurveBake.{h,cpp}`（新建）+ `ClipEdit.{h,cpp}`
  加两个结构（**`assets/Skeleton.h` 与 `ecs/Components.h` 一个字节没改**，
  守第 5 节约束 1）：
  - `KeyEase { interp, ease, shape, handle, outHandle, inHandle }` —— 一个**键**
    的插值口味。挂在 `EditTrack::ease` 上，与 `times` **平行、允许更短**、
    全默认即视作没有（口径同 `types`），于是老文件读进来 `ease` 是空的、
    一个字节都不用补；
  - 手柄存**绝对偏移** `(秒, 值)`，不是相机那种归一化 `w`。这一条就是
    "两条曲线不能合成一个面板"的全部理由（见下面 UI 那段）；
  - `ResolvedTrack { track, ease, curves, comps, hasEase }` —— 解析后的
    可采样单位：`curves` 是**预烘**的（translation / scale 各 3 条，
    rotation 4 条）。烘一次、采多次，而不是每个采样点现算一遍切线；
  - API：`bakeTrackCurve`（EditTrack → Curve）/ `applyCurveToTrack`
    （Curve → EditTrack）/ `sampleEditTrack` / `sampleResolvedTrack` /
    `listBoneChannels` / `boneChannelId` / `curveDigest`（FNV-1a 64，
    与 R7 的 `clipDigest` 同族，用来证明"移过去再移回来必复原"）。
- **四条"省了就错"的约定**（每条都写在 `CurveBake.h` 头注释里，每条都有断言）：
  - ⚠⚠ ① **没有缓动 = 逐位不变**。`ease` 为空或全默认时
    `sampleResolvedTrack` 直接 `return assets::sampleTrack(rt.track, time)`，
    一个浮点运算都不多做。这是"打开一次曲线编辑器再关掉，所有老动画
    不许有一丝变化"的依据 —— 少了这句，每次开关都以 1e-7 的量级漂，
    长跑之后角色会慢慢转歪，而画面上完全看不出来（同约束 7 的坑）；
  - ⚠⚠ ② **旋转的缓动分两条路**，因为两者数学上不是一回事：
    `Linear + 缓动` 是 `slerp(qa, qb, easeFn(u))` —— 只重映射时间，走的还是
    那条大圆弧，缓动恒等时**转发** `sampleTrack`（同一个 `glm::slerp`、
    同一份 `u`）；`Bezier` 是四分量各自解再归一（**nlerp 语义**，同一条弧
    另一种速度分配），也是唯一能手工塑造四元数速度的路子，Blender 的
    四元数 F-Curve 同样是逐分量插值后归一。**合并成一条就错了**：拿 nlerp
    实现 Linear，全部老旋转轨的中间帧都会变（肘部走弧线走扁了）；
  - ⚠ ③ **插值与手柄属于"键"，不属于"分量"**。一个 `EditTrack` 就是一条
    glTF channel，四个分量**共用关键帧**（`times` 只有一份）。给
    `rotation.x` 设贝塞尔，y/z/w 也跟着贝塞尔 —— 分开设会让四分量用不同的
    时间重映射，四元数离开单位球再被归一，表现成"转轴在中途晃了一下"。
    **值**当然各分量独立。`AutoClamped` 手柄是按**本分量自己的邻居**现算的，
    所以默认情况下每个分量的切线是对的；只有手拖过的手柄（Free / Aligned /
    Vector）是四分量共用 —— 那也有道理：一根骨在某一时刻的节奏只有一个；
  - ⚠ ④ **写回不增删帧**。`applyCurveToTrack` 第一件事是比较
    `c.keys.size()` 与轨道帧数，不等就 `++st.skipped` 整条拒绝。曲线编辑器
    改的是**形状**（时刻 / 值 / 插值 / 手柄），加删关键帧是 Dope Sheet 的活；
    两边都能改帧数，`EditClip` 的 `findTrack` 不变量就该开始飘了。
- **写回的自检**：`applyCurveToTrack` 写完立刻再烘一遍，与传入曲线在 33 个
  采样点上比对，`worstDeviation` 正常**恰好 0.0**（同一份数据、同一个求值器）。
  非 0 就说明写回漏了字段（手柄、`interp`、`ease` 三者之一）—— 这是"存了
  但没影响结果"这类 bug 唯一便宜的探针。同时返回
  `{keys, written, changed, merged, skipped, worstDeviation, bitIdentical}`：
  **没改就写回时 `changed == 0`**，`merged > 0` 则是丢帧（被挪到同一时刻）。
- **`step` 与现代插值互斥**：`EditTrack::step` 是老字段（`Interp::Constant`）。
  写过任何非 Constant 的 `interp` 就把 `tr.step = false`，然后
  `pruneDefaultEase()` 收掉全默认的尾巴；反过来，`step == true` 且该键没被
  显式设过 `interp` 时，`resolveEaseTable` 兜底成 Constant —— 否则
  "老文件里标了 step 的轨道打开曲线编辑器就变线性"。
- **UI**：新建 `editor/BoneCurvePanel.{h,cpp}`，而**不是**给
  `CurveEditorPanel` 加一个分支。理由写在头注释里：相机一条段只有**一组
  归一化手柄**（四通道共用），骨骼**每键存绝对偏移**（分量独立）；靠 `if`
  隔离的代价是每次改动都要重答"这个手柄归谁"，答错的症状是"拖了手柄
  另一条通道跟着变"。共用只有 `animation::Curve` 求值器与几个 UI 习惯，
  画布代码重复约 150 行 —— 但那是**两种不同东西**的画布。
  五条面板自己的约束：①画布不另写求值（只调 `sampleResolvedTrack`）；
  ②改形状不改帧数；③拖动中不重排（否则拖到邻居身上就跳出去）；
  ④`V` **存类型**（与相机面板"V 换算并写回、写完就是 Free"正好相反 ——
  AutoClamped 得一直跟着邻居重算）；⑤旋转 Linear 段运行时走 slerp、
  画布上没有那条线 → `crossCheck()` 跳过并计数。
- **注入脚本**：`MYVK_EDITOR_BONEC_SCRIPT`
  （`sleep / path / reload / chan / sel / selall / setkey / dragkey / interp /
  ease / shape / handle / sethandle / drag / head / fit / digest / check /
  save / saveforce`），收尾打
  `BONEC-SCRIPT applied=N failed=N`。与鼠标手势共用 `act*` 那一族，所以
  "能不能自动验"不取决于把某个点的像素坐标算准没有。
- **验收**（`tools/verify_bone_curves.py`，**结果: PASS（43 项）**）：
  - A 组 32 条 `curvebake_*` 自检（`ANIM-TOTAL pass=308 fail=0`，从 276
    长上来），并点名四条底线：`curvebake_default_bitwise` /
    `curvebake_rotation_linear_bitwise` / `curvebake_writeback_identical`
    （`changed=0`）/ `curvebake_writeback_refuse`（帧数不等拒绝）。
    其余覆盖：分量数与分量名、无表逐位、默认表逐位、**slerp ≠ nlerp**
    （探针必须取 `u=0.25`，见下）、`step` 逐位与 mid、缓动
    `mid=0.15625`（smoothstep(0.5)×1.0 手算）、端点不动、贝塞尔烘焙、
    摘要往返、分量隔离、拖时间、同刻合并、`step` 互斥、`prune`、
    插帧时缓动表对齐、旋转贝塞尔归一、通道表 7 条、整条叠加链、JSON 往返；
  - B 组 打开真片段：`tracks=2 keys=6 chans=7`，且**第一帧**的
    `BONEC-CHECK samples>0` —— "绿灯挂在没跑上"是这类读数最危险的形态
    （见下面那条事故）；
  - C 组 七个通道挨个切：rotation 的 `skipped=63`、translation 的
    `skipped=0`，`xcheck` 全 0。`skipped` 是**诚实读数不是遮掩**：
    旋转 Linear 段运行时走 slerp，画布上是分量曲线（nlerp），两者本来就
    该不一样；装作差 0 才是错的；
  - D 组 缓动手算：`t=0.125` 处 `0.125 → 0.078125`（= smoothstep(0.25) ×
    0.5），且端点逐位不动；
  - E 组 `T` 切插值：linear → bezier → constant 摘要两两不同，回 linear
    复原，**`keys` 恒为 3**（帧数一个都不许变）；
  - F 组 手柄：`handle=vector` 落盘的是**类型**，拖 out 手柄后
    `0.3878 → 变大`，类型翻成 `free`；
  - G 组 跨进程摘要一致 + 文件里真的出现 `keyEase`（**稀疏写**：只写非默认，
    带 `i` 下标）+ 未编辑过的轨道不写；
  - H 组 只读护栏：文件打不开时 `Save` 被拒（`reason=read-only`）；
  - I 组 20 个 UI 锚点（`BONEC-OPEN / VIEW / CHECK / MAP / SAMPLE / DIGEST /
    CHANNEL / SEL / SETKEY / INTERP / EASE / HANDLETYPE / DRAG-END /
    REORDER / SAVE / SAVE-BLOCKED / SCRIPT / REJECT / WIN / PATH / HEAD /
    FIT / CANVAS / CURSOR / HANDLE-* / INSP-*`）+ 零 VUID + 零 `[ERROR]`。
- **还差**（留给后面）：
  - **运行时（`AnimatorSystem`）不读缓动表** —— 它吃的是
    `assets::AnimationClip`，而缓动住在 `EditClip` 上。要搬过去同样得动
    `ecs/Components.h`（GLOBAL_GLOBS → 全量回归），与 S-C 的"运行时读同一份
    约束栈"是同一批活，一起做；
  - 曲线修改器（Noise / Sine）与循环外推（`Extrapolate::Cycle*`）在骨骼轨
    上**未接**：`Curve` 有，`bakeTrackCurve` 只是把区间内的键投影出来；
  - Action 级曲线（一个曲线驱动多根骨）未做 —— 现在是"一条 channel 一个
    分量一条曲线"，与 Blender 的 Action 层级还差一层；


### R6 · Graph Editor（曲线视图）· 规模 M —— ✅ 已落地（2026-10-28）

把 R5 的数据层画出来。（落地证据：`editor/CurveEditorPanel.{h,cpp}` +
`tools/verify_curve_editor.py` 结果: PASS；核心断言是"画布投影 ≡ 运行时采样"
差 < 1e-4 —— 两者数学等价但**浮点不逐位**，`glm::mix` 是 `a*(1-w)+b*w`、
`evalSegment` 是 `a+(b-a)*w`。）

- **改动点**：新建 `editor/CurveEditorPanel.*`（画布 + 通道列表 + 侧栏）；
  画 `evaluateCurve` 采样出的折线、关键帧点、两根手柄、外推段（虚线）；
  拖关键帧/手柄 → 写回；`T` 切插值、`V` 切手柄类型（对齐 Blender 的肌肉记忆）。
- **数据源**：先接**过场相机段的归一化 w 曲线**（数据已经在 `SeqCameraKey` 上，
  零风险）；再接实体 TRS（要动 `engine/ecs/Components.h`，见第 5 节的横切约束）。
- **验收**：`verify_curve_editor.py` —— 拖动关键帧后 `CURVE-KEY t=…` 变化、
  曲线采样点在拖动手柄后单调变化、截图与基线差异 > 阈值。
- **依赖**：R5（已完成）。

### R7 · 骨骼关键帧编辑 + Pose Mode · 规模 L —— ✅ 已落地（2026-10-28）

这是"能不能做动画"的分水岭。

- **改动点（实际）**：
  1. **clip 写回通路** —— 新建 `engine/animation/ClipEdit.{h,cpp}`：`EditClip` 是一份
     **独立的、可写回的资产**（`format=venn.animclip` / `version=1`，浮点直接存 JSON
     number → float→double 精确提升，"最短可回读"导出 → 逐位往返）。`Skeleton`
     仍然是**只读导入产物**，一个字节没动 —— 见下面那条 ⚠ 的取舍；
  2. `CharacterWindow` 加 Pose Mode：骨骼树选骨 → 三轴旋转 gizmo（拖拽角度按
     **世界轴**算，再经父骨世界四元数共轭转成局部增量）→ `I` 打帧
     （Rotation / Location / Scale 可分别打；`K` 全打）；
  3. 关键帧列表 + 姿态库（`posestore:/poseapply:`、`EditPose`、`capturePose`）。
- **采样链的关键取舍**：`Skeleton::sample` 是兼容壳 = `samplePose + poseToJoints`
  （`Skeleton.cpp:124`），所以拆成「基础片段采样 → 编辑层覆盖 → 瞬态层覆盖 →
  `poseToJoints`」三段式后，**无编辑数据时天然逐位一致**，由 `MYVK_ANIM_TEST`
  的 `refactor_identity` 组钉住。
- **两条必须守住的语义**：
  - **过载层空则一个字节不动**：`overlayEditPose` 开头 `if (resolved.empty()) return 0;`
    —— 这是"打帧只影响打过帧的骨"的实现基础（验收 C1 用对照骨
    `upperarm.L` 的局部 + 世界 dump 与无编辑基线逐字符比对证明）；
  - **瞬态层**：Blender/UE 的语义是"摆姿势不自动打帧"，但采样每帧从头重算，
    所以摆出来的角度必须存在独立一层（`m_touched*`）；拖时间轴 / 播放 / Stop 时
    `clearTransient()` —— "未打帧的姿态在跳帧时丢弃"。
- **三个坑（写在这里省下次踩）**：
  - 关节**用名字不用下标**：下标是导入顺序的产物，模型重导一次就整体平移，
    症状是"右臂的帧跑到左腿上"且不报错；`resolveEditTracks` 只用
    `kJointMatchCaseInsensitive`，**刻意不带 `kJointMatchSubstring`**（包含匹配在
    换骨架后会静默命中另一根骨）；
  - `touchBone()` 第一次碰某根骨时必须**快照三个通道的当前值**，否则
    `setrot` 只改旋转会把平移/缩放顶成初始值（`0/1`）—— 冒烟实测踩到过；
  - **循环里的 `PushID` 绝不能用"看起来唯一"的名字**：`anime_girl.fbx` 有 4 个
    动画栈，归一化掉 `Rig|` 前缀后是 `[Idle, Walk, Idle, Walk]` —— 两行 `Walk`
    同帧同 ID，ImGui 弹**模态**告警窗（"2 visible items with conflicting ID!"），
    之后**所有鼠标点击全被它吃掉**。界面看着完全正常，只是点哪都没反应
    （`verify_character_window` 的"点动画条目触发播放"就是这么红的）。
    一律用下标当 ID；锚点（`logRect`）照旧按名字打，脚本要的是"哪一段"。
    ⚠ 资产侧的重名还没治 —— 见任务 #329。
- **⚠ 横切（已按推荐路线处置）**：`engine/assets/**` 在 `GLOBAL_GLOBS` —— 改
  `Skeleton.h` 会触发全量回归。**选了"把新数据放到 `engine/animation/` 下自建
  结构"那条**（见 §5 约束 1）：`ClipEdit` 自带一套，`Skeleton` 保持只读导入产
  物，这轮因此一条全量回归都不用跑。
- **验收（`tools/verify_pose_mode.py`，结果: PASS）**：存盘 → 重开 → 骨骼姿态
  逐位一致（`%a` + FNV-1a 摘要，三个时刻三个进程）；打两帧 → 播放 → 关节矩阵
  摘要两两不同；Pose Mode 里打帧不影响非 Play 态的其他骨骼。另加 B 组 slerp
  角度 0/30/60°、F 组坏骨名只报 `POSE-RESOLVE-MISS` 而好轨道照样生效。

### R8 · Dope Sheet + 关键帧类型 · 规模 M —— ✅ 已落地（2026-10-28）

"只看哪一帧有没有键"的那个视角。R6 的曲线图能调手感，但**读节奏**要看点阵：
十根竖线挤在一起时，曲线图看不出"这一段缺一帧"。

- **改动点（实际）**：
  1. `editor/DopeSheetWindow.{h,cpp}` —— 一棵通道树 + 一张网格**同处一块画布**
     （两个 `BeginChild` 各自滚动会让通道名和菱形错行 → 点到了隔壁骨）；
     骨架模式按 `topoOrder` 走、**缩进 = 有键的祖先个数**（用全部祖先会让同层
     骨缩进不同，一眼就觉得层级乱了）；折叠时摘要行才铺点（展开时下面三行已经
     把帧摆出来了，再叠一层就是同一个帧画两遍）；骨架里找不到的骨**照样列出**
     并追加在末尾（"我的帧去哪了"的唯一界面线索）；
  2. 关键帧类型 `KeyType`（`Key / Extreme / Breakdown / Inbetween`）+ 与 `times`
     **平行且允许更短**的 `types` 表（老文件、手改坏的 JSON 都能读，缺项一律当
     `Key`）—— **完全不进求值**，`setKeyTypes` 只写类型表；
  3. 三个批量操作住在 `engine/animation/ClipEdit`（`moveKeys` / `mirrorKeys` /
     `alignCycle`），UI 与注入脚本共用同一份实现 —— 于是"能不能自动验"不取决于
     把某颗按钮的坐标算对没有；
  4. 缩放 / 横向滚动：`Ctrl+滚轮` 绕鼠标锚点缩放、`Shift+滚轮` 平移、滚动 = 纵向。
- **三个坑（写在这里省下次踩）**：
  - **`float` 不保"间距不变"**：`(a+d)−(b+d)` 与 `a−b` 在指数变化时舍入不同。
    所以自检挑二进制精确的数（2⁻⁴ 的整数倍 + 0.25 位移）才能断言"逐位"；
    真实 1/30 秒时刻只能按 1e-6 容差验 —— 用容差又会把"逐个累加实现"这种真 bug
    放过去，所以**验收用精确数**、真实时刻只做量级检查；
  - **镜像用 double 中间值**：`static_cast<float>(2.0*pivot − static_cast<double>(t))`，
    这样与 Python `float32(2*pivot−t)` 逐位一致，验收才敢用 `==`；
  - **对齐循环的末帧走特判**（`lo + (hi−lo)·scale` 的舍入可能差 1 ulp）：
    "对齐到 1.2 秒"必须精确等于 1.2，否则循环点永远差一丁点。
- **两条刻意的语义选择**：
  - `mirrorKeys` 是**时间**镜像，**不是左右骨互换** —— 后者需要骨架左右轴契约
    （Mixamo 是 X、Blender 导出是 −X），做错不报错、只是人朝后仰；
  - `Esc` 取消用**整份 `EditClip` 值拷贝**回滚，不是"记录反向位移"：
    float 上 `(t+d)−d ≠ t`，长拖下来原节奏会一点点漂掉且看不出是取消造成的。
- **验收（`tools/verify_dope_sheet.py`，结果: PASS，51 条断言）**：夹具由脚本
  **现造**（不碰 `assets/`），时间戳全取 2⁻⁴ 的整数倍 ——
  B 组框选 10 帧一起挪 0.25 秒，每个时间戳与 9 个 gap **逐位**相等（逐个累加
  实现会在这里漂）；C 组镜像后每帧落到 `2·pivot−t` 上（逐位）且**间距序列反序**
  （= 前后半段对称），镜像两次回到原样、落盘逐位等于原始；D 组设类型后
  `times`/`values` 逐位不动（求值输入不变）+ 只有非默认才写 `types` 字段；
  E 组 `zoom` 绕播放头（锚点相对位置不变、跨度减半）、`pan` 只挪 `t0`、
  每颗菱形 `px ≡ px0+(t−t0)·pps`（最差 0.000 px）；F 组十个控件锚点齐全 +
  全程零 VUID/`[ERROR]`。
  求解器层面"类型不影响采样"由 `MYVK_ANIM_TEST` 的 `dope_types_do_not_change_sampling`
  与 `dope_types_independent` 钉（`ANIM-TOTAL pass=221 fail=0`）。

### R9 · NLA · 规模 L

- 片段轨：一个 Action 落成 strip，可缩放/循环/错开/叠加。
- **与现有 `AnimLayer` 的关系必须写清楚**：层是"按骨覆盖的当前姿态"，NLA 是
  "时间轴上排好的片段"。两者不是同一个东西，别硬合并（硬合并的下场是
  "为什么我把片段拖上去，角色的上半身被整段替换了"）。
- **验收**：Walk + 挥手 两段错开叠加，上下半身分别动（复用 `verify_anim_layers`
  的上下带判据）。

### R10 · Drivers + Motion Paths + 序列帧导出 · 规模 M/L

- **Drivers**：表达式求值器（只做四则 + 几个函数，**不要**做完整脚本语言）+
  变量（属性引用 / 距离 / 时间）+ 驱动器列表 UI。
- **Motion Paths**：采样未来 N 帧的骨骼世界位置 → 画折线。纯 debug 视图，不参与求值。
- **序列帧导出**：`RenderAnimation` —— 指定帧范围 + 输出目录，逐帧 `capture` 到 PNG，
  再做一次简单的 MP4 封装（或直接给 ffmpeg 命令）。

### 顺带要补的两件小事

- **`ANIM-` 前缀冲突**：编辑器 `AnimationEditor` 用 `ANIM-KEY/PLAY/PAUSE`
  （`ToolkitEditors.cpp:3289`），运行时 `AnimatorSystem` 用 `ANIM-TICK/SKIN/EVENT`。
  `TLED-` 特意避开了 `SEQ-`，这里没有贯彻 —— 场景在播时断言 `ANIM-KEY` 会混。
  建议编辑器侧改成 `ANIMED-`。（R7 新增的日志一律用 `POSE-` / `POSELIB-` 前缀，
  不碰这两个空间，故意错开。）
- ~~**`editor/EditModes.h:22-31` 的过期注释**~~ —— ✅ 2026-10-28 已重写为十项
  全有真实编辑器的表格（`Select` / `LandscapeEditor.cpp` / `FoliageEditor.cpp` /
  `ToolkitEditors.cpp` 六个 / `WaterEditor.cpp`），并说明 `editModeImplemented()`
  保留是**承载 UI 契约**（能不能点、点进去有没有面板），不是历史遗留。
  同批把散在 `EditModes.cpp` / `ToolkitEditors.h` / `EditorApp.cpp` / `Toolbar.cpp` /
  `ModeRail.cpp` 的「九/六→十/七」也一并改了。

---

## 4 · 为什么不先做 NLA / Pose Mode

> 2026-10-28 补注：Pose Mode 已经落地（R7 ✅）。这一节保留当时的推理，
> 因为"**R8 Dope Sheet 要在 R7 之后才顺**"这个结论正好被验证了：打完帧之后
> 下一步就想调节奏。NLA 仍然排在 R9、仍然吃 R6。

看起来"没有 NLA"比"没有曲线视图"严重，但顺序反过来会白做：

- NLA 的每个 strip **要有自己的进出场曲线**才有意义（不然片段之间只能硬切或
  crossfade），而曲线视图是调这个曲线的唯一工具 → **R6 必须在 R9 前**；
- Pose Mode 打完帧之后，作者第一件想干的事是"把这几帧的节奏调一调"，
  那需要 Dope Sheet → **R8 和 R7 可以并行，但都吃 R6**；
- Drivers 是"锦上添花"，但它要的表达式求值器和状态机条件求值可以共用骨架。

---

## 5 · 八条硬约束（下一轮开工前先读这段）

1. **`engine/ecs/**` 与 `engine/assets/**` 在 `GLOBAL_GLOBS`**（`tools/run_regression.py:999`）。
   动 `Components.h` / `Skeleton.h` = 按项目纪律要跑全量。所以 R6 的"接实体 TRS"
   和 R7 的"clip 写回"要么**攒批**，要么把新数据放在 `engine/animation/` 里自建结构。
   （R7 走了后一条：`engine/animation/ClipEdit.{h,cpp}`，`Skeleton` 零改动。）
2. **逐位不变是硬门槛，不是"差不多"**。老资产（`.seq.json`、场景里的 `animClip`）
   的观感挂在浮点结果上。任何"统一实现"的改动，必须有一个**独立**的参考实现
   做逐位比对（`seq_sample_legacy_bitwise` 就是这个模式）。
3. **求值只留一个入口**。`evaluateCurve`（带容器）与 `evaluateSegmentEase`
   （不分配）必须共用同一份段内计算 —— 断言 `curve_two_entries_agree` 在钉这条。
   新加插值模式时，**两个入口一起加**，否则"过场相机"和"骨骼轨道"会走出两条曲线。
4. **改 rest 必须同时改 `invBind`，而且要有"零编辑 = 逐位不变"的断言**（S-A/S-B）。
   `jointMat = invMeshNode · world · invBind`：只改前半段、不重算后半段，模型会整体
   缩进/偏移/歪斜，**运行时一条日志都不会报**。所以 `RigEdit` 烘焙出来、且一格没改
   时，必须与纯导入路径的 `joints` **逐位相同**（`%a` 比对 + FNV 摘要），
   这条比任何新功能都优先。
5. **每顶点权重和恒为 1，且没编辑过的顶点烘焙出去逐位不变**（S-B）。前者是因为
   `pbr.vert` 用"权重和 > 0"判断要不要蒙皮 —— 和为 0 与全 0 无法区分，顶点会留在
   网格空间，**角色一动就"身上粘着一坨皮"**；后者是因为"读出来 → 排序 → 写回四槽"
   这条路一定会以 1e-7 量级漂移，所以未触碰的顶点必须 `memcmp` 后照抄。
   断言：`weights_auto_sum_one` / `weights_bake_identity` / `WEIGHTS-BAKE changed=0`。
6. **重传顶点缓冲前必须 `device().waitIdle()`**（S-B）。`Mesh::upload` 内部
   `delete m_vertexBuffer`，在飞的命令缓冲还引用它 —— 不先等空闲就是
   `VUID-vkDestroyBuffer-00867`，画面上是花屏。只在松开笔刷/点 `Bake to mesh` 时
   付一次全同步，不在每帧付。
7. **"没生效"必须是逐位不动，不能用乘 0 实现**（S-C）。`influence = 0`、条目
   `enabled = false`、`LimitRotation` 落在限幅以内 —— 这三种情况都必须**提前
   return、一个字节都不写**。原因和 S-B 那条同源：`slerp(q, q, 0)` 在浮点下不是
   恒等，"乘个 0 再写回去"会以 1e-7 的量级漂，长跑之后角色会慢慢转歪，而画面上
   完全看不出来。断言：`constraint_limit_inside_bitwise` /
   `constraint_*_influence0` / `constraint_limit_disabled_bitwise`（全是 `memcmp`）。
8. **"少了数据的那一路"不许多算一步**（S-D）。轨道没有缓动表时，采样必须
   **转发** `assets::sampleTrack`，而不是"走一遍曲线、结果碰巧一样"：
   平移/缩放确实逐位等价（`v0+(v1-v0)*w` 就是 `a+(b-a)*u`），但**旋转不等价**
   —— 老路是 `slerp`，分量曲线是 nlerp（同弧不同速，正中点相同、其余差几个
   百分点）。所以 `sampleRotationEased` 里那句 `if (w == u) return
   sampleTrack(...)` 是**结构性**的，不是优化。同源的三条：写回没改时
   `changed=0`、曲线帧数与轨道不等时整条拒绝、`keyEase` 全默认时不落盘。
   断言：`curvebake_default_bitwise` / `curvebake_rotation_linear_bitwise` /
   `curvebake_writeback_identical` / `curvebake_writeback_refuse`。
