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

---

## 1 · 映射表

`状态` 列的图例：**✅ 有** / **🟡 半有**（能用但缺关键一半）/ **❌ 无**

### 二 · 基础流程（帧 / 关键帧 / 自动关键帧）

| Blender | 引擎里的对应物 | 状态 | 证据 / 缺口 |
|---|---|---|---|
| 帧、当前帧、播放头 | **秒**（float）到处跑，没有帧号 | 🟡 | `Skeleton.h:37,45`；`AnimatorSystem.h:69`。播放头 UI 在 `ToolkitEditors.cpp:3446`、底栏 `EditorApp.cpp:5862` |
| 帧率 FPS | **整条链没有 fps 字段**；只有导入期 FBX 固定 30fps（`FbxLoader.cpp:455`）和运行循环固定 1/60（`GameLoop.h:44`） | ❌ | 所以"24fps 动画在 30fps 引擎里怎么采样"这件事**从来没被决定过** |
| 起止帧 | `AnimationClipComponent::duration`；clip 用 `duration` | ✅ | `Components.h:581`、`Skeleton.h:45` |
| 按 `I` 插关键帧 | 实体 TRS：`Add key`（`ToolkitEditors.cpp:3481`）；骨骼：**没有** | 🟡 | 骨骼 clip 没有写回通路 |
| Auto Keying（改属性自动打帧） | 无。Pose Mode 的"摆姿势"刻意走**瞬态层**（`m_touched*`），跳帧即丢，不自动落轨 | ❌ | 与 Blender/UE 默认语义一致；要做的是 R7 之上的开关，不是改采样链 |

### 三 · 时间轴（Timeline）

| Blender | 引擎 | 状态 | 证据 |
|---|---|---|---|
| 播放 / 暂停 / 上下帧 | 底栏 `animPlay/animPause/animStop` + `Step`（固定 1/60） | ✅ | `EditorApp.cpp:5766-5913` |
| 当前帧输入框 | 各面板的 `Time` 滑条 | 🟡 | 没有"输入第 N 帧"，只有秒 |
| 关键帧菱形 | Animation 面板：自绘刻度带（不可拖）；`TimelineEditorWindow`：菱形**可拖** | 🟡 | `ToolkitEditors.cpp:3458`、`TimelineEditorWindow.cpp:992` |
| 框选 / G / S / Shift+D | 时间轴窗口只有单键拖 + Delete | 🟡 | **R8 的摄影表**有框选 / `G` 平移 / `M` 镜像 / `Y` 对齐循环（`DopeSheetWindow.cpp`）；时间轴窗口本身仍是单键拖，`S` / `Shift+D` 两处都还没有 |
| 关键帧类型（极端 / 间断 / 惯性） | R8 落地 `KeyType`（`Key/Extreme/Breakdown/Inbetween`）+ 形状编码 | ✅ | `engine/animation/ClipEdit.h`。纯标记、**不进求值** |

### 四 · 动画摄影表（Dope Sheet）

**✅ 有**（R8，2026-10-28，`editor/DopeSheetWindow.{h,cpp}`）。通道树（骨骼层级 /
实体属性）+ 一行一根骨 + 关键帧菱形 + 框选 / `G` 平移 / `M` 镜像 / `Y` 对齐循环
长度 + `Ctrl+滚轮` 绕鼠标缩放 / `Shift+滚轮` 平移 / 纵向滚动。

- 打开两条路：`MYVK_EDITOR_DOPE=<路径>`（独立打开一份 `.anim.json`）、
  Character 窗口 Pose Mode 里的 `Dope sheet` 按钮（**绑定**形态：数据源由宿主
  推过来，存盘被拒 `reason=bound` —— 一个文件两个写者是后存的静默盖掉先存的）；
- 关键帧类型 `Extreme / Breakdown / Inbetween` 只在**形状**上编码（实心+外圈 /
  空心 / 小圆点），**完全不进求值**，只管读节奏；
- 批量操作的三条实现（`moveKeys` / `mirrorKeys` / `alignCycle`）住在
  `engine/animation/ClipEdit`，与注入脚本共用同一份 —— 验收因此不依赖鼠标坐标。
- **还差**：帧号（整条链是秒，见第二节）、实体属性轨（要先动 `Components.h`）、
  通道多选与 `Shift+D` 复制。

### 五 · 曲线编辑器（Graph Editor）—— 动画灵魂

| Blender | 引擎 | 状态 | 证据 |
|---|---|---|---|
| F-Curve（值随时间的曲线） | **R5 刚落地**：`engine/animation/Curve.*` | ✅ 数据层 | 见第 2 节 |
| 插值 Constant / Linear / Bezier | `sampleTrack` 只有 `bool step`（`Skeleton.h:40`）；过场有 linear/smooth/step（`SequenceAsset.h:44`） | 🟡 | 骨骼轨道仍缺贝塞尔 |
| 切线 / 手柄（V 键切 5 种） | R5 落地 `HandleType`（Auto/AutoClamped/Vector/Aligned/Free）+ 自动钳制 | ✅ 数据层 | `Curve.h:70` |
| 侧栏参数（Frame / Value / Interp / Easing / Handle） | 过场时间轴的属性栏有 t/pos/yaw/pitch/fov/ease + **R5 新增 4 个手柄** | 🟡 | `TimelineEditorWindow.cpp:1188` |
| 曲线修改器（Cycle / Noise / Sine） | `Extrapolate::Cycle*` 三种循环外推有了；Noise/Sine **无** | 🟡 | `Curve.h:80` |
| 外推（Hold / Linear / Cycle） | R5 落地 5 种 | ✅ | `Curve.h:78` |
| **曲线视图（画曲线本身）** | **无** | ❌ | R6 |

⚠ 一句话：**灵魂已经会跳了，但还没有舞台**。下一步就是画出来。

### 六 · Action 与 NLA

| Blender | 引擎 | 状态 | 证据 |
|---|---|---|---|
| Action（一段可复用动画） | 导入的只读 `assets::AnimationClip`（`Skeleton.h:43`）+ R7 新增可写回的 `animation::EditClip`（`ClipEdit.h`，落盘 `.animclip.json`） | ✅ | `ClipEdit.h` |
| Action Editor（改名 / 切片段） | Character 窗口的 clip 列表（点击即播）+ Pose Mode 的基础片段选择 | 🟡 | `CharacterWindow.cpp:1170` |
| NLA（片段拖到轨道上，缩放/循环/叠加/混合） | **无** | ❌ | `AnimatorComponent::layers` 是"按骨覆盖的层"，**不是时间轴片段**（`Components.h:241`） |
| 上层片段混合（走路 + 挥手） | 层 + 骨骼遮罩能做到 | ✅ | `AnimatorSystem.cpp:141` |

### 七 · 骨骼与姿态动画

| Blender | 引擎 | 状态 | 证据 |
|---|---|---|---|
| Pose Mode（选骨 → 转 → 打帧） | 选骨 → 三轴旋转 gizmo → `I` 打帧（Rotation / Location / Scale 可分别打），带瞬态层（摆姿势不自动打帧） | ✅ | `CharacterWindow.cpp` 的 `drawPosePanel` / `drawBoneGizmo` / `keySelectedBone` |
| IK | 两骨解析 IK + 影响度可打帧 | ✅ | `TwoBoneIK.h:65` |
| 骨骼约束（Track To / Copy Location / Stretch To） | 只有 LookAt + 弹簧骨 | 🟡 | `LookAt.h:45`、`SpringBone.h:89` |
| 姿态库（Pose Library） | 存/取整骨姿态，随 `.animclip.json` 一起落盘 | ✅ | `ClipEdit.h` 的 `EditPose` / `storePose` / `applyEditPose` |

### 八 · 形态键（Shape Keys）

| Blender | 引擎 | 状态 | 证据 |
|---|---|---|---|
| Basis + Key 1..N，权重 0~1 | `MorphRig` 通道 + 子网格 target，GPU 形变已通 | ✅ | `MorphRig.h:35`、`Renderer.cpp:3463` |
| 权重可打关键帧 | **不可**。glTF 的 `weights` 通道被跳过（`MeshLoader.cpp:677`） | ❌ | 所以"表情动画"做不了，只能手拨 |
| 表情预设（一个表情 = 一组权重） | Inspector 滑条 + 控制台 `morph.set` | 🟡 | `InspectorPanel.cpp:2282` |

### 九 · 驱动器（Drivers）

**❌ 整节没有。** 唯一的"条件求值"是状态机转移里的比较（`AnimController.cpp:46`），
没有算术表达式、没有变量引用、没有函数。

### 十 · 运动路径（Motion Paths）

**❌ 没有。** 骨骼轨迹只能靠"播一遍看"，看不到未来几帧的弧线/过冲。

### 十一 · 输出与渲染动画

| Blender | 引擎 | 状态 |
|---|---|---|
| 分辨率 / 帧范围 / 帧率 | 出包链路有（`make ship`），**没有"导出序列帧"** | 🟡 |
| PNG 序列 → MP4 | 无（只有测试脚本的 PNG 截图） | ❌ |
| Render Animation | 无 | ❌ |

### 十二 · 速查表 → 术语对照

| Blender | 本引擎 | 备注 |
|---|---|---|
| Keyframe | `assets::AnimTrack.times/values[i]`（平行数组，**没有 Keyframe 结构体**）；实体侧是 `ecs::TransformKey` | 两种关键帧，语义不同 |
| F-Curve | `animation::Curve`（R5） | 骨骼轨道还没接过来 |
| Interpolation | `Interp`（Constant/Linear/Bezier） | 骨骼轨道仍是 `bool step` |
| Handle | `HandleType` + `{dt,dv}` **相对**手柄（R5） | Blender 存绝对坐标 |
| Easing | `EaseMode` × `EaseShape`（7 种形状） | — |
| Extrapolation | `Extrapolate`（5 种） | — |
| Action | `assets::AnimationClip` | — |
| NLA Strip | ❌ | — |
| Driver | ❌ | — |
| Shape Key | `MorphRig` 通道 | 不可打帧 |
| Constraint | LookAt / TwoBoneIK / SpringBone | 没有通用约束系统 |

---

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

## 3 · 后面五个阶段

> 排序原则：**先让"已有数据"可编辑，再造"没有的数据"**。反过来的话，新造的数据
> 没有工具用，只能躺在 JSON 里。

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

## 5 · 三条硬约束（下一轮开工前先读这段）

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
