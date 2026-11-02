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
- ⚠ **Inspector 14 页签**（11-03 加 `Gameplay`）：加 = `kTabs[]` 一行 + 两处 `static_assert`；**落盘写 id
  不写下标**；`All` **不是融合视图** → 新页签锚点也要进 `drawTabAll`；布局一变，按 rect 点控件的用例会漂。
- ⚠⚠ **`kBuiltins[]` 是*按位置*对应 `enum class Builtin`**，`static_assert` 只比**长度**：删一条再在末尾
  补一条 → 从此位置起**整表错位一格**（`getAttr` 跑进 `setTimeScale` 分支，报
  `setTimeScale(): scale must be > 0`，错误行完全不像 arity 问题）。**改内建动词一律原位改**；
  加新动词先 `grep '"名字",'`（同名两条 → 先命中的那条赢，arity 对不上 = **编译期** not runnable）。

## 二次元UI主题（10-27✅；→ `docs/anime_ui_restyle.md`）
- ⚠⚠ **`c^(1/2.2)`**（交换链把 ImGui 浮点色当线性值又编码一次）；**老 5 套主题系数一个都不许动**；圆角进
  `ThemeMetrics` 后 padding 一动**面板矩形全变**；**主题下标只能追加**。

## 界面语言默认 = 系统语言（10-29 ✅；→ `docs/system_language_2026-10-29.md`；判据 `verify_system_lang`）
- 出厂 = **系统语言**（`g_lang` 用 `-1` 哨兵惰性求值）；env > 存档 > 系统；**自动化跑默认英文**；prefs
  **不写**与出厂相同的 `language`。⚠⚠ `it.id == "language"` 比的是**指针** → 必须 `strcmp`。


## 玩法数值与回合调度 P2（11-03 ✅；权威 → `docs/gameplay_stats.md`；判据 `--selftest gameplay` 111 + `verify_gameplay` A~F）
- ⚠⚠ **组件 = 权威数据，`AttributeSet` = 派生视图**：读前必 `buildAttributes()`；改是
  build → 改 → `storeBases` 写回。在组件里缓存算好的值 = 两份真值（"面板显示 94、扣血按 100 算"）。
- ⚠⚠ **`consume()` 不能"探斜率"**（`value(base+1)-v0`）：满血单位 `base` 正卡在 `maxValue` 上，
  斜率是 **0** → 扣不动 → "打人完全没伤害"（满血是最常见状态，只测半血的用例看不出来）。
  按定义算 `k=(1+addPct/100)*mul` 再反解 `base = target/k - flat`。
- ⚠⚠ **`StatusSet::stacks()` 必须累加同名实例**（`shield` + `shield#2`）：`if (find(id)) return …`
  只报 1 层 → "两个护盾只挡了一次"。⚠ **存档恢复走 `restore()` 不走 `add()`**（add 按叠加规则
  重算 → 3 层毒读回变 1 层）；状态存**完整定义**；属性 `min`/`max` 必须和 `base` 一起存。
- ⚠⚠ **求值顺序固定** `Override → Flat → AddPct → MulPct → clamp`，**clamp 只在最后做一次**；
  `AddPct` 相加（三条 +10% = +30%）而 `MulPct` 连乘。
- ⚠⚠ **伤害必须回传明细**（`steps` 固定五步，**含"没发生"的步**）：只读最终血量的话
  "护甲没生效"和"易伤没生效"可以给出完全一样的数；不记空步则加暴击后下标整体错位。
  `dealt` 是**真正扣掉的血**（过量不虚高），脚本判死亡必须读它。
- ⚠⚠ **行动条（CTB）**：`advance(dt)` 一次只出**一个**人（剩下的留给 `advance(0)`，否则会对已
  被击杀/已移除的实体做行动）；同槽先后用 **`order`（加入序号）** 不用 gauge 大小（浮点抖会让
  出手顺序在两次运行间翻转），`preview()` 与 `advance()` **必须同一套 tie-break**。
  ⚠ **1.5 倍速"连着出手两次"是正确行为**（长期 5:3），改成"交替"等于把倍速抹平（自检 K13）。
  ⚠ 顺序条印**时刻**（`timeline()`）不是剩余槽量 —— 后者会让同一人多次出手印成同一个时间。
- ⚠ **`damage` 是"U8 HUD 扣血"和"P2 五步结算"同一个动词**（arity 1..2），靠宿主**返回 -1**
  （目标没 `GameplayComponent`）分流；`0` 是合法"打不动"，不能当哨兵。
- ⚠ `tickRound()` 返回**实例数**不是层数（2 层毒 = 1 条 dot、伤害 5×2=10）；
  `getAttr` 返回 0 有两种含义 → 必须配 `gameplayHasAttr`。
- ⚠ 判据口径：C 组**不能先 `hit`**（console 命令同一 tick 跑完，页签首帧已扣过血）；E 组
  `hit 30` 打 armor=100 期望 **85**（70 = "护甲漏了"的数值，正好测不出来）；F 组要开
  `MYVK_LOG_SCRIPT_PRINT=1` 才看得到脚本 `print`（行是 `VKS-PRINT <path>: <value>`）。

## 牌堆 P3-3（11-03 ✅；权威 → `docs/cards.md`；判据 `--selftest gameplay` M 组 80 + `verify_cards` A~E 33）
- ⚠⚠ **身份是 `uid` 不是下标**：手牌重排（拖拽/排序）下标全变，存档存"第 3 张"
  读回来是另一张。⚠ **uid 永不复用**；⚠ **读档必须重算 `nextUid`**（取最大 +1），
  否则新牌撞号 → 症状"打出这张、那张消失了"。
- ⚠⚠ **第五个区 `Limbo`（结算区）**：打出的牌 `Hand → Limbo → Discard/Exhaust`，
  **不能**立刻进弃牌堆 —— 否则"弃牌越多伤害越高"这类效果在结算里读到**它自己**，
  每张多算一次且只在跨过倍数时跳变。**`Limbo` 非空 = 一张牌正在结算**（哨兵）。
- ⚠⚠ **`move()` 的 `pos` 是目标位置，不要 -1 补偿**：契约 `move(uid,to,pos)` 之后
  `uidAt(to,pos)==uid`。写成"插到原数组第 pos 个之前"会**差一位**，而差一位在
  UI 上像"拖动有点飘"，不会被报成 bug。
- ⚠⚠ **手牌满就停止抽**（剩下的留牌库，**不烧牌**）；**牌库空自动把弃牌洗回**
  且**必须计数**（`reshuffles()`/`lastDrawReshuffled()`）。不计数的话"洗回没发生"
  的唯一表现是"中盘抽牌效果全哑火"，`draw()` 返回 0 与正常跑长得一样。
- ⚠⚠ **`clear()` 不清定义表**：读档序 `clear → setDefs → deserialize`，漏中间一步
  整副牌变未知卡，而未知卡**不报错**（打出来只是没效果）。
- ⚠ **`draw()` 不收成员 `Random`**，一律传 `Random&`（调用方给 `streams.gameplay`）。
- ⚠ 存档与 P2 **相反**：状态存**完整定义**（要冻结），卡牌**只存 defId**（内容是配表）。
  ⚠ **`limbo` 必须写进存档**（否则中途存档那张牌凭空消失）；写端与
  `hasNonMeshContent` 两处名单要同时加 `cards`。
- ⚠ 同一个读数**不许两处两种字段名**：Runtime 用 `discard=`/`exhaust=`，
  Inspector 第一版用 `disc=`/`exh=` → C 组立刻红。已统一并在两处注释互相指名。
- ⚠ 特性面的 `ui` 名单要填**真实存在的锚点**（`check_feature_surface` 只验字符串
  出现过，拦不住填命令名）；自检总数变了要同步改 A 组的 `pass=N`（111 → 191）。
- 面板 46（`Debug ▸ Cards`）；Inspector 页签**不变**（牌堆是 Gameplay 第 ⑤ 段）；
  组件 46（`CardDeckComponent`）；14 条 `cards.*` 命令回显 `GP-CARDS-CMD name=…`。

## 游戏类型 4 种（11-04 ✅；权威 → `docs/launcher_roadmap.md` §13；判据 `--selftest project` K33 16 条 + `verify_launcher_create` 四轮）
- **用户要求**："游戏类型的设置应该是在启动器创建游戏的时候去设置的"。所以它是一个
  **新建向导第 1 步的单选项**，不是一个独立的下拉框。
- `ProjectFile.h` 的 `GameType`：`first_person 0 / third_person 1 / isometric 2 /
  card_battler 3` + `kGameTypeCount` + `gameTypeAt(i)`。**只能追加** —— `LAUNCH-TYPE-<i>`
  锚点的下标就是这张表的顺序，中间插一条会让"老判据点的第 2 张卡"变成另一种类型。
- ⚠⚠ **单轴，不拆"相机 × 玩法"两轴**：两轴有 4×3 组合而一半没有模板 → 用户会在
  "选完发现打不开"上白费一次创建。代价是相机与玩法耦合，但模板本来就是"一键给一套
  能跑的东西"。
- ⚠⚠ **卡片表与枚举只准一份真值**：向导里 `cards[]` 只存文案，`GameType` 由
  `gameTypeAt(i)` 反查；`verify_launcher_create.py` 也从 `kGameTypes[]` **读** id 顺序
  （`read_game_type_ids()`），不在测试里再写一份。
- ⚠⚠ **2×2 布局的宽度上限是 380**：452 时两行 = 614px，而向导内容区只有 ~560px →
  第二行掉出窗外。而 `logRect` **不判可见性**，卡片"有矩形却点不到"，日志一条错都没有。
  用例已显式比客户区（`C1 all N type cards on screen`）。
- ⚠⚠ **`build/bin/templates/` 是启动器真正读的那份**（`templatesRoot()` = `<exe>/templates`），
  改完模板不 `make build` → 新建工程拿到**上一版场景**。症状是"相机/实体不对"，第一
  反应会去翻 `ProjectCreate`（那里根本不碰场景）。`verify_launcher_create` 的 Z2 前置
  检查专门拦这条，报错直说"先 make build"。
- 两个新模板：`templates/isometric/`（8×8=64 格棋盘 + Ally/Enemy + `tickRound` 脚本）、
  `templates/card_battler/`（牌桌 + `cards` 组件 10 张牌库 + 读 `assets/data/cards.csv`）。
  ⚠ 卡牌模板的 `GameStart` 摆在 `z=-60`：`spawnOnRun=false` 只关掉**运行时**生成，
  编辑器仍会在出生点画预览角色，正好挡住牌桌。
- K33 把"卡片存在"升级成"卡片可用"：逐 id 验 `manifest-load / manifest-gametype /
  start-scene-declared / start-scene-exists` —— 失败发生在用户填完名字**之前**。

## ui2 变换 / Scroll / Grid / 拖放（11-04 ✅；权威 → `docs/ui_hud_roadmap.md` §17；判据 UI2 自检 132 条 + `verify_ui_editor --only i` 10 条）
- 顶点层：`Ui2Prim::RoundBoxRotated = 4`（CPU 算好四角当 pos，`inHalf/inUv` 仍是
  **未旋转局部系** → 圆角转过去还是正圆角）；`Ui2Xform` = 2×3 仿射；
  三条状态栈（变换/透明度/裁剪）**每次从索引 0 重算**（"除回去"遇 opacity=0 会 NaN）。
- ⚠⚠ `ui2.vert` 的分支必须是 `inMode > 3.5` 单独一支。写成 `> 2.5` 会把 mode 4 也吃进去
  → 旋转后的框被"中心+半宽"再展开一次，画成两倍大且位置错。
- 裁剪**走 scissor 不剪几何**（剪角点 = 改中心/半宽，frag 的 `sdRoundBox` 会变直角）。
  `drawCalls()` = scissor run 数。⚠ offset≥0、`x+w ≤ outExtent`，**越界触发
  VUID-vkCmdDraw-None-07845 → 整帧 UI 消失**，floor/ceil 后必须夹。
- ⚠⚠ 拖放合法性**只有 `dropAccepts()` 一处**（绘制侧决定高亮、松手时决定放不放）。
  写两份 = "高亮了放不进去" + "没高亮却能放"，必修两遍。
  **拖过不产出点击**（否则一次拖拽结算两次）；**落点是自己不算放下**；**不设移动阈值**。
- ⚠⚠ `scale` 下界 **0.01**：`scale == 0` 让逆变换失效 → 控件连自己都点不到，"放回来"
  没法用鼠标做。命中三道修正：逆变换 / `hasClip && !inside` / 滚出可视化区。
- ⚠ 自检失败先怀疑期望：Scroll 容器**自己也是面板**（滚出视野后仍剩 6 顶点）；
  部分裁剪记 **1 clipped / draws=3**（只有被裁的那一个 quad 记 scissor）。
- ⚠ `dropAccepts` / `placeRec` 之类**必须写在匿名命名空间外**（内部链接 → LNK2019）。
- 编辑器：`setrot/setscale/setopacity/setscroll/setgrid/setdrag` 六条命令 +
  属性栏四段（`UIED-ROT/SCALE/OPACITY/SCROLL/GRID/DRAGTAG/DROPACCEPT`）+ `kAdd[]` 6→8。
  ⚠ 改 `kAdd[]` 必重跑 A 组（`logRect` 不判可见性，按钮会被挤出面板还"有矩形"）。
- 验收脚本的期望计数**从 `len(steps)` 推导**，不写死（写死会在改脚本时把 FAIL 说成
  实现坏了）。示例文档只有 `hud/{hpbar,hpnum,hint}`、`title`、`resume`、`topline`。

## 伤害数字层 + 卡牌表现模板（11-04 ✅；权威 → `docs/cards.md` §十一；判据 `--selftest gameplay` 243 条 O/P 组 + `--selftest script` C21 + `verify_cards` G 组）
- **伤害数字是独立一层**（`gameplay/DamageNumberLayer`），**不进 `UiDocument`**：
  飘字位置每帧变、来自世界坐标、且不能被父节点的 `pushClip` 裁掉。
  `HudModel` 里那个 `std::vector<DamagePopup>` 已被它整体替换（成员名还是 `popups`）。
- ⚠⚠ **合并默认开**：同点（≤1m）同类 + 年龄差 < 0.35s → `37 x3`。合并**不重置 `t`**
  （重置 = 每帧挨打的目标留下一条永生数字）。调试要逐条就给 `merge=false`。
- ⚠⚠ **漂移必须确定**（`serial` 整数散列，不用 `rand()`）→ 否则"连拍两帧比像素"永远 flaky。
- ⚠ `t` 是**归一化年龄**（0..1），寿命只影响 `tick` 步长；`lround(0.4)=0` 会把 0.4 显示成
  "0"（像伤害被吞）→ 非零小数一律显示 1。
- `worldToScreen` **全仓只有一份**（`gameplay::projectToScreen`），`runtime::Hud::worldToScreen`
  转调它。两份必然有一份多翻一次 Y（"数字上下镜像但还在屏幕附近"，最难发现）。
  ⚠ 单位阵测不出"相机背后"（w 恒为 1），要用零矩阵；测"屏幕外"要挪 **x** 不是 z。
- **卡牌表现模板** = `gameplay/CardFx.h` 一张表 `CardFxKind → { .veffect, 飘字, 落点, 高度 }`
  + `assets/fx/card_{play,hit,buff}.veffect`。改手感改 `.veffect`，不改代码。
  **抽/弃/消耗刻意什么都不出**（牌堆变化是 UI 的事，喷火花会让人以为场上出了事）。
- ⚠⚠ `castCard` 只**记账**（`CardFxEvent` 队列，上限 64），`RuntimeApp::dispatchCardFx()`
  每帧统一播（排在 `runScripts` 之后，否则飘字慢半拍）。**不能在 `castCard` 里直接播**：
  `GameplayRuntime` 编辑器与运行时共用，编辑器里点 `cards.cast` 就喷火且关不掉。
  编辑器 Play 也接了动词（否则脚本报 not available）但没有 HUD 层 → 事件丢弃 + `ED-CARD-FX`。
- ⚠ 飘字用**实际数值**：伤害取 `DamageResult::dealt`，治疗取 `after-before`。
  ⚠ 日志里的 `heal=` **仍是请求量**（口径不能悄悄改）。
- 新脚本动词 `castCard(idx)` / `drawCards(n)`：**只返回 0 不报错**（手牌空还按是正常用法）；
  `drawCards` 用**手牌差分**而不是解析 `cardDraw()` 那行给人看的字符串。
- ⚠⚠ `.veffect` 的 `_comment` 里**不许出现未转义的 ASCII 双引号**（换成「」）——
  JSON 非法 → 运行时 `FX-PLAY failed`，而仓库那份看着完全正常，因为编辑器读
  `build/bin/assets/` 的**副本**。`verify_cards` G 组现在比 mtime（改完要 `make build`）。
- 计数联动：`verify_cards` A 组断言 `pass=243`（加了 O/P 组），`run_regression` 的
  `verify_cards` TRIGGERS 已加 `DamageNumbers.*` / `CardFx.*`。

## 导航 / 寻路 / AI（C8，11-05 ✅；权威 → `docs/navigation.md`；判据 `--selftest nav` 94 + `verify_nav` 66 + `--selftest script` C22 5 条）
- **是格子网，不是 NavMesh**。选格子法的理由：`std::vector<uint8_t>` + `std::vector<float>`
  可整份存 JSON、可打印 ASCII、A*/视线/吸附都是纯函数 → 自检不要 GPU、0.8 秒跑 94 条。
  代价：斜向贴墙有一格抖动；**2.5D**（桥下/楼上同一 XY 走不了）。
  重构入口：`engine/nav/NavGrid.{h,cpp}` 头注释写明"**不 include 任何 physics / rhi / scene**"。
- ⚠⚠ **只有 `RigidBodyComponent` 才会建物理体**（`buildSceneWorld` 按刚体遍历，
  `CollisionComponent` 只回答"形状从哪来"）。只挂碰撞体 → `bodies=1`（只有无限地面）、
  `walk=9216`（96×96 全可走）、**烘焙还报成功**。
  → 判据必须断言 `walk < cells`，**只断言 `walk > 0` 的话这条错永远绿**。
- ⚠ 编辑器烘网格用**临时建的物理世界**（`buildSceneWorld(..., enabledOverride=1)`），
  不是 `m_physics`（后者只在 Play 里存在）。强制开物理是刻意的：作者常还没勾"启用物理"，
  而烘焙只读几何、不推进模拟。
- **A\* 六条规矩**（每条对应一种"看起来能用但很怪"）：① 斜迈时两个正交邻格也要可走（不许切角）；
  ② 高差 > `maxStep` 的边不通（格本身仍可走）；③ 队列用 **`(f, index)` 全序**（只比 f →
  同 f 节点按堆内部顺序展开 → 两次跑出两条路径）；④ 起终点吸附到最近可走格（`2·cell`）；
  ⑤ 终点不可达 → 走到"离终点最近的已到达点" + `reached=false`；⑥ 首尾尽量用调用方精确坐标。
- ⚠⚠ **`length` 是"绕过去了"的唯一判据**：直接穿墙和绕过去都能返回"一条路径"，只有折线长度能分开
  （`findPath` 的用例钉 `len > 12`）。**平滑只可能变短**（变长 = 视线判定比 A* 还严）。
- **两条移动路**：有 `CharacterControllerComponent` → 只写期望速度 `cc.velocity.xy`（位移交给物理，
  自动处理台阶/斜坡/贴地）；没有 → 直接写 `TransformComponent.position`，z 贴网格。
- ⚠⚠ **`tickNav` 必须排在物理之前**：无胶囊的代理写 Transform，物理 `syncKinematicFromScene` 要读它；
  有胶囊的写期望速度，物理这一步才积分。排后面 = 永远晚一帧（"跟着影子走"）。
- ⚠ `repathInterval` 是**防抖不是性能优化**：目标每帧微动（追玩家）时不设下限会每帧跑 A*。
- ⚠⚠ **运行态一律不序列化**（`path`/`next`/`repathTimer`/`reached`/`failed`/`lastExpanded`）——
  存下来会让"读档后沿着上一次的路径走"。`hasGoal` 由 `goal` 是否存在推出，不单独存 bool。
- **网格资产优先级**：`assets/nav/<场景名>.vnav.json` → 现场烘（以玩家为中心 48×48 m，
  `MYVK_NAV_CELL` 可调）→ `GAME-NAV source=none (agents will idle)`。
  为什么存资产：现场烘"作者机器上能过去的路"在玩家机器上可能不通，且 200×200 现场烘是
  几十毫秒 → 换关卡卡顿尖刺。
- ⚠⚠ `.vnav.json` 位图用**字符串 `'0'/'1'`**（不压 base64：要能文本编辑、git diff 看出哪片格子变了）；
  高度 3 位小数。**读端只认 `'1'` 为可走**（反过来会把"位图被截断"变成"整张图乱走"）；
  长度必须 = `cols*rows` 否则**拒绝整份**（`bitmap size 9215 != cols*rows 9216`）。
- ⚠ `setupNav()` 夹在中间：**玩家实体建好之后**（兜底范围以玩家为中心）、**`PlayerController::attach`
  之前**（attach 要读物理世界 heightfield）。所以兜底中心扫的是"第一个带 `CharacterControllerComponent`
  的实体"，不是 `m_player->entity()`（那时还是空指针）。
- ⚠ `navGridIo(false, …)` 读失败**保留旧网格**（清掉会让 20 个代理集体静止，更难归因）；
  `nav.save` 要**先 `create_directories` 父目录**（`ofstream` 不替你建，报 `open-failed`）。
- ⚠⚠ **多行日志会骗过"等某一行"的等待条件**：`nav.ascii` 一次打 96 行 → 自动化要等**最后一条**
  命令的锚点（`GP-NAV-CMD name=clear`）再快照。
- 面板 `nav/Navigation`（12 个 `NAV-*` 锚点）+ 九条命令 `nav.bake/clear/state/ascii/path/agent/stop/
  save/load/tick`；`nav.tick [n]` 是编辑态手动推进代理（"挂上代理它到底会不会走"）。
- **脚本四动词（C8-2）**：`navGoto(x,y,z)` / `navArrived()` / `navFailed()` / `navStop()`，
  全部作用于 **self**。判据 `--selftest script` **C22**（5 条，MockHost）+ `verify_nav` **F 组**
  （真宿主跑一份 `.vks`，断言**到达点 x > 3** 而不是只断言那行日志）。
- ⚠⚠ **`navGoto` 返回"受理"不是"走到了"**（求路在下一帧 tick 里）；返回 0 **不报错**
  （没有网格 = 运行时状态，报错会让整份脚本 not runnable）。
- ⚠⚠ **`navArrived()` 必须和 `navFailed()` 成对**：目标在墙外时 arrived 永远 0，
  `while (!navArrived())` 会**永远转**而且不报错（配额只管每帧次数）。
- ⚠⚠ **编辑器 Play 以前根本不 tick 导航** —— 动词接得再对也没人推，症状是
  "`navGoto` 返回 1、日志正常、NPC 站着"。现在 Play 循环里排了 `m_nav.tick()`
  （脚本之后、物理之前）；**编辑态故意不 tick**（推进代理会改编辑场景 Transform 并进
  undo 栈，作者只想看网格时不该被巡逻兵挪场景）。
- ⚠ 没有 `navGoto(e, x, y, z)`：`kBuiltins[]` 的 arity 表达不了"第 0 个参数换类型"，
  做一个"传错了不报错"的重载比不做更糟。脚本要驱动谁就挂在谁身上。
- 已记账未做（`navigation.md` §十）：局部避障、动态障碍（`setCellBlocked`）、
  烘焙分块（配合 G19 流送跨段拼接）、`navGotoTo(e,…)` 显式实体重载。
