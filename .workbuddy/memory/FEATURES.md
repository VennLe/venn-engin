# venn —— 已完成体系的判据（MEMORY.md 的续册，2026-11-01 拆出）

> 为什么拆：MEMORY.md 一旦超 14.5 KB 注入会被**截尾**（尾部「路线图」最先丢）。
> 本册只放**已完成体系**的判据；跨体系纪律仍在 `MEMORY.md`。
> ⚠ **碰这些体系之前先读本册那一节**，别只盯着 MEMORY.md。

## 着色 A1~A6 ＋ 动画 B1~B3（10-26 ✅；权威 → `anime_gap_review_2026-10-26.md`；用法 → `character_animation_guide.md`）
- ⚠⚠ **A 类槽（ramp/matcap/SDF/detail/aniso/eye）+ 角色光只在 `VENN_BINDLESS` 单元里**，`MYVK_BINDLESS`
  **默认关** → 静默全失效。色带 `u=0` 被 REPEAT 回绕 → 夹首末纹素。⚠ **"不吃 X"要对照材质自证**；
  **先确认被测物在画面里再谈它动没动**；**"暗档 ≠ 更暗"**。
- ⚠ B1 遮罩=**根骨+父链子树**；B2 区间**半开**、换 clip **归零 `eventPrev`**；**B3 剥离=钳回基准**。
- ⚠⚠ **glTF `joints` 是 Y-up、FBX 已 Z-up** → 写 `TransformComponent` 前必须过 `Skeleton::skeletonToEngine`，
  否则角色**朝天上飘**（yaw 用 **swing-twist**）。⚠ **手写 `skinned` 实体一像素不画** → 先 `IMPORT` + `save` 烤模板。

## 过场时间轴 C1（10-26 ✅；权威 → `docs/cutscene_sequencer.md`）
- ⚠⚠ 播放器只产出 `pose()`、**不持有 `CameraSystem*`**（传指针 = 视口变自由相机）；`tick` 在 `update`
  **之前**；播完 `hasPose()==false`；`dt==0` 不推进。

## 相机运镜 CINE + 多机位（10-30 ✅；权威 → `docs/camera_move.md`；判据 `camera_move` 23 / `multi_cam` 44）
- **四层分离**：姿态(+roll/shift)/镜头/rig(look·path·handheld)/效果 —— 混一起 = "改了没反应"。
  ⚠ **动词是算子不是状态**（`+15°` 在当前朝向上再摇）→ 设值写法单次看不出、**连推两次立刻红**。
- ⚠⚠ "一顿一顿"根因是**没做弧长参数化**（`kArcSub=64`，10.3%→0.48%）；判据用**直线 + 速度差 30 倍**
  手调手柄（折线测的是"弯"）。⚠ `cameraEulerFor` 基**只能** `(f,−r,u)`（`(f,r,u)` 行列式 −1 → 镜像）。
- ⚠⚠ **叠加序**：焦距→路径→注视→roll/shift→手持→震动；阻尼 `1−exp(−dt/τ)`；快门存**角度**不存帧数。
  ⚠ 镜头→渲染只能宿主做（`applyCameraLensEffects`，**只在配了时写**否则 `MYVK_DOF` 失效）；`clipNear=0`
  = **不设**；新字段**只在非默认时写 JSON**；`fov` 是**残留值** → 判 `fovEff`。
- **多机位**：`shots[]`（区间半开；`camera:0` = 回落关键帧轨；**早于首个 marker 返回 −1 不是 0**）。
  ⚠⚠ 切镜前必须 `clearDesiredPose()`（否则机位**完全不动**而 `CAM-CUT` 照打）；目标被删要**回落 priority
  + 清 guid**；绑 guid 走 `parseGuidHex`（`atoll` 读成十进制）。

## 序列化 / 存档 G16·G17-1~3（10-27 ✅；逐条判据 → `persistence_roadmap.md` §12.9）
- ⚠⚠ `setGuid` **先摘旧键**、`destroy` **先 erase 再清零**（写反 = 旧 guid 复活）；hex 补满 16 位；版本校验在
  `scene.clear()` **之前**。⚠ **空实体不进场景文件**，但**存档必须全量**（`includeEmpty=true`）。
- ⚠⚠ 父子写 `parentGuid`**不写下标**；**「已销毁」是粘性**的（`kDeadHash`，别 erase → 怪读档复活）。
  应用序 **销毁 → 覆盖 → 悬挂父（无条件跑）**。

## 关节 J5（10-27 ✅；判据 → `verify_joints` A~H）
- 拾取：self 过**本实体**逆、target 过 **target** 逆（写反 = 一 Play 拽飞）；点空退相机平面；**Esc 只退
  拾取不清选中**。改关节 = 组件 + `rebuildJoints`，**绝不** `buildSceneWorld`。
- ⚠⚠ **`kBuiltins[]` 的 arity 就是公开签名**（写成标量 → 按 vec3 写的脚本**编译期** not runnable）；位移
  判据用**末态−起点**。⚠ 可视化用 **verts 精确值**；退化基必须 `return`。⚠ `NOTIF-PUSH` **镜像每条 WARN**。

## 对话 C2（10-26 ✅；权威 → `docs/dialogue.md`）
- ⚠⚠ **`cue()` 绝不给场景加组件**（会被 `scene.save` 写盘）；**cue 优先于 `autoStart`**；起点**永远**用资产
  `start`。语义：`goto/next` 悬空 = **整份加载失败**｜**有 choices 时 `advance()` 不走**｜`cps<=0` = **立刻全出**。
  ⚠ **`Press::NeedChoice` 运行时不可达** → 别断言。`DLG-END` 打**两条**。

## 步态 / 步幅匹配 A4·A4b（10-30 ✅；权威 → `docs/gait_cycle.md` §五~七）
- ⚠⚠ 承重判据 = **`dFwd<0`（方向）+ 最低点 +12cm 绝对门**；教科书 duty **不可用**（比例带宽被根运动带跑，
  八轮改阈值全败）。走/跑只认**三条相对关系**。
- ⚠⚠ 相位推进**必须积分**（4~32 子步，否则"60fps 不滑、30fps 滑"）；只推**循环** clip；**缓存键带毫秒
  时长**。⚠ 斜坡 `rate` 分子永远**水平投影**。

## 表情 B6 + 口型包络（11-01 ✅；权威 → `docs/expression_preset.md`；判据 `verify_expression` 41 / 自检 49）
- ⚠⚠ **驱动只写自有通道**（当前预设列 + 眨眼 + 口型）；**上一帧拥有、这一帧不再拥有的显式写 0**，且
  **`ownedChannels` 只存本帧活跃**（含补 0 的 → 清空预设后每帧写 0，滑条永远推不上）。
- ⚠⚠ **`ExpressionComponent` 不能并进 `MorphComponent`**（合并 = "每帧写满"与"推滑条"抢同一块内存 →
  症状是推滑条没反应）。⚠ **不自动挂组件**（眨眼默认开 → 老场景角色立刻眨眼 = 违反"旧场景逐像素不变"）。
- ⚠⚠ 眨眼抖动 = **确定性 LCG（种子取实体 id）**，首帧按 id 抽相位；`rand()` 只能写松断言。口型输入 =
  **Voice 总线 RMS**（非全局 RMS/`voicePlaying()`），**指数**平滑（攻 0.02 / 放 0.09），线性 = 帧率相关。
- ⚠⚠ **`A || ImGui::TreeNode(...)` 是 ID 栈杀手**（短路少调 `TreeNode` 但 `TreePop` 照跑 → `PopID() too many
  times` → **同帧之后所有控件 ID 错位**，整段锚点消失，日志只一条 imgui-error）。⚠ Inspector 段要放
  **49 根 morph 滑条之前**。细则 → `verify_details_archive.md` §十一。

## 脚本/编辑器（细则 → 归档；权威 → `docs/script_roadmap.md`；**能力评估** → `docs/script_language_assessment.md`）
- ⚠⚠ **能力实测（10-30）：6861 行 / 84 内建 / 93 自检绿，但组件覆盖 6/42=14%**（相机 29 字段全不碰）→
  **白名单字段投影层** `prop/setProp`（字段名编译期解析），不换 Lua。⚠ `setIntensity` 对无灯实体**静默返回 0**。
- **`ScriptRuntime.{h,cpp}` = 唯一 tick 内核**。相位 **`声明区 → start → 接触事件 → 顶层 → update`**；
  热重载用 `ScriptAsset::version()`。⚠⚠ **`ScriptProgram`/`HookBlock` 构造析构必须 out-of-line**；前向声明
  类型的定义**必须在 `namespace vks` 里**（C2872）。⚠⚠ **编辑态脚本不 tick** → 测热重载必须进 Play。
- **加面板 = 表里加一行**（`PanelRegistry` 派生菜单/快捷键/图标/落盘）；面板间**只准依赖 `EditorContext`**，
  动引擎走 `EditorHost`。⚠ 快捷键排他；⚠⚠ 粒子分叉/辉光**多发射器**；⚠ 循环里 `PushID` **不能用名字**。
- ⚠ **Inspector 13 页签**：加 = `kTabs[]` 一行 + 两处 `static_assert`；**落盘写 id 不写下标**；`All` **不是
  融合视图** → 新页签锚点也要进 `drawTabAll`；布局一变，按 rect 点控件的用例会漂。

## 二次元UI主题（10-27✅；→ `docs/anime_ui_restyle.md`）
- ⚠⚠ **`c^(1/2.2)`**（交换链把 ImGui 浮点色当线性值又编码一次）；**老 5 套主题系数一个都不许动**；圆角进
  `ThemeMetrics` 后 padding 一动**面板矩形全变**；**主题下标只能追加**。

## 界面语言默认 = 系统语言（10-29 ✅；→ `docs/system_language_2026-10-29.md`；判据 `verify_system_lang`）
- 出厂 = **系统语言**（`g_lang` 用 `-1` 哨兵惰性求值）；env > 存档 > 系统；**自动化跑默认英文**；prefs
  **不写**与出厂相同的 `language`。⚠⚠ `it.id == "language"` 比的是**指针** → 必须 `strcmp`。

