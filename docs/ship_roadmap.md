# Venn 游戏运行时任务清单（Ship Roadmap · G 系列）

> 生成于 **2026-10-17**，基于"用本引擎完整开发出一款 3D 游戏还缺什么"的差距评估。
>
> **与既有两份路线图的关系**：
>
> | 文档 | 系列 | 回答的问题 |
> | --- | --- | --- |
> | `docs/render_roadmap.md` | **R** | 引擎**画得对不对**（渲染能力） |
> | `docs/editor_roadmap.md` | **E** | 人**用得顺不顺**（编辑器入口） |
> | **本文档** | **G** | 游戏**跑不跑得起来、玩不玩得动、发不发得出去**（运行时与出货） |
>
> **R 系列做能力，E 系列做入口，G 系列做成品。** 三者成对交付（见 §12 准入契约）。
>
> ⚠ **进度（2026-10-19 复核）**：M1（G01~G05）与 M2（G06~G10）已交付。
> M3 ~ M7 里的大部分任务**已经被各自的专门路线图接管**（动画 → 角色链、
> 音频 → `audio_roadmap`、HUD → `ui_hud_roadmap`、存档 → `persistence_roadmap`、
> 地形 → `world_scale_roadmap`、脚本 → `script_roadmap`）。
> **开工前先看下面这张对照表**，别按 §6~§10 里"未开工"的旧口径重复劳动。
>
> | 任务 | 实测状态（对着代码核过，不是对着文档猜） | 权威路线图 |
> | --- | --- | --- |
> | G11~G13 动画运行时 I/II/III | ❌ 未做。**A0 ✅（采样/推进）+ A1 ✅（`AnimatorComponent` 接管 time、单 clip 直放、`animator` 存档块）**，但**仍没有**状态机 / 层与遮罩 / 根运动 / 动画事件 / IK | `character_pipeline_roadmap.md` A2~A6 |
> | G14 音频 | ✅ 已完成（自研 WASAPI，`audio_roadmap.md` G14-0~G14-9 全绿） | `audio_roadmap.md` |
> | G15 游戏 UI / HUD | ✅ 已完成（`runtime/ui/`：UiRenderer / SDF 字体 / 布局 / 控件 / HUD / 暂停菜单） | `ui_hud_roadmap.md` U0~U8 |
> | G16 稳定 GUID | ❌ 未开工。`engine/scene/` 里没有 `guid` 字段 | `persistence_roadmap.md` §3 |
> | G17 存档系统 | ❌ 未开工。没有 `engine/scene/SaveGame.*` | 同上 §5 |
> | G18 Prefab | ❌ 未开工 | 同上 §6 |
> | G19 关卡流送 | ❌ 未开工 | 同上 §7 |
> | G20 运行时设置与暂停菜单 | 🟡 **部分**：暂停菜单有（`runtime/ui/PauseMenu`，三项），但"设置"页**没有内容** —— 语言 / 画质 / 音量还没接 | 本文 §7 |
> | G21 地形 | ✅ 已完成（`world_scale_roadmap.md` W4：`TerrainPass` + `physics::Heightfield` + Inspector 段） | `world_scale_roadmap.md` W4 |
> | G22 水体 | ❌ 未做（阻塞于 R6-2） | `render_roadmap.md` R6-2 |
> | G23 植被与风 | ❌ 未做（W5 是下一项） | `world_scale_roadmap.md` W5 |
> | G24 动态天空与大气 | ❌ 未做（阻塞于 R6-5） | `render_roadmap.md` R6-5 |
> | G25 光照烘焙的运行时消费端 | ❌ 未做（阻塞于 R5-2） | `render_roadmap.md` R5-2 |
> | G26~G28 `.vks` 实体/事件/协程 | ✅ 已完成（`script_roadmap.md` S2~S4） | `script_roadmap.md` |
> | G29 资产打包 `.pak` | ✅ 已完成（`--pak` → 一个 `game.pak`，含 shader 直读） | `package_roadmap.md` §7 §8 |
> | G30 Shipping 剔除 | ✅ 已完成（`make ship` / `VENN_SHIPPING` / `verify_shipping.py`） | 本文 §10 |
> | G31 崩溃回收（minidump） | ❌ 未做（没有 `MiniDumpWriteDump` 相关代码） | 本文 §10 |
> | G32 运行时性能 HUD / 控制台 | ❌ 未做 | 本文 §10 |
>
> 本文档是可执行版：
> 每个任务给到**可直接开工的改动点** + **机器可验证的验收条款**，能直接并进
> `tools/run_regression.py` 与项目五件套纪律。
>
> 现状基线（2026-10-17）：**渲染器已接近 3A demo 水准**（TAA / 自动曝光 / DRS / HiZ /
> MDI / Bindless / POM / Decals / 反射探针 / GPU 粒子 / **GPU 蒙皮**），**编辑器已是成熟工具**
> （11 面板 + 3 条带、特性 34/34 有 UI 面、命令面板、时间轴、5 套主题、中英切换），
> **物理 v1 完整**（凸体 + GJK/EPA + PGS + 关节 + 休眠）。缺的**不是画面，是"游戏运行时层"**。

---

## 0 · 系列定位与编号约定

- **编号**：`G01 ~ G32`，按里程碑推进。里程碑是**验收单元**，任务是**开工单元**。
- **规模标注**：**S** = 半天～1 天；**M** = 2~4 天；**L** = 一周以上。
- **依赖标注**：`←` 表示开工前应先完成；**硬依赖加 `!`**（不先做就做不下去）。
- **状态标记**：无标记 = 待做；`✅` = 已完成；`🟡` = 部分完成。
- **每份任务的"验收"必须是机器可判的**：产物存在 / 日志有锚点 / `verify_*.py` 有断言。

### ⚠ 本系列的三个总纪律（先读）

1. **不做完 P0 六项就不叫"游戏"**。P0 = 运行时框架 / 游戏 UI / 音频 / 动画运行时 / 输入映射 / 存档。
   这六项里**任意一项缺失**，做出来的都只是"能走动的技术 demo"。
2. **顺序不可随意调换**：`G02`（脚本引擎搬家）是**唯一的路障**——不做它，运行时用不了脚本，
   后面 M3~M6 的内容全部无法"用脚本串起来"。**它必须第一个做。**
3. **别先做地形/水/植被**。它们"看起来最像游戏"，但那是**画面**不是**游戏**。
   一个地形漂亮、没有主菜单、角色滑步、没有音效的项目，仍然不是游戏。

---

## 1 · "完成一个游戏"的定义（本系列的总验收）

本系列的目标不是"能渲出好看的画面"，而是**这条链路端到端走通**：

```
冷启动 → 主菜单 → 选/加载关卡 → 操控角色 → 与场景/敌人交互（物理 + 脚本 + 音效 + 动画反馈）
      → HUD 显示状态 → 暂停 → 存档 → 读档 → 退出（干净释放，无泄漏）
```

**总验收信号**（M7 结束时必须同时成立，缺一不可）：

| # | 信号 | 判定方式 |
| - | --- | --- |
| 1 | 存在独立的 `Game.exe`（不是 `Editor.exe` 加参数） | 构建产物存在且能脱离编辑器运行 |
| 2 | 冷启动到主菜单**不依赖任何 `MYVK_*` env** | 清空环境变量运行通过 |
| 3 | 从主菜单能进关卡、能操控、能暂停、能存读档 | 端到端 GUI 用例（G32 的 `verify_game_flow.py`） |
| 4 | 有声音（至少：环境音 + 脚步 + UI 音） | 音频设备被打开，波形非零 |
| 5 | 角色动画**不滑步**（速度与动画匹配） | 根运动量测：位移/动画周期比在 ±5% |
| 6 | 打成一个可分发的包，在**干净目录**里能跑 | 拷贝到空目录 → 双击可运行 |
| 7 | 崩溃能生成 minidump 且日志有栈 | 故意触发 → dump 文件存在 |
| 8 | 编辑器**仍然**可用（拆分没有回退它的能力） | E 系列全量用例不红 |

---

## 2 · 现状盘点：有什么 / 缺什么

### 2.1 ✅ 已经有的家底（**不要重复造**）

| 层 | 已有 |
| --- | --- |
| **渲染** | MSAA+HDR / Bloom / FXAA / ACES / TAA / 自动曝光 / Color LUT / DRS / CSM 4 级 / 局部阴影 / 分簇光照 / IBL / 静态天空盒 / 高度雾 / SSAO / SSGI / SSR / GodRays / DoF / 运动模糊 / 色差 / 暗角 / 后处理体积 / 变体 + 热重载 / GPU instancing / 视锥剔除 / BVH / LOD / HiZ / MDI / Bindless / POM / Decals / 反射探针 / GPU 粒子 / 多相机 / RenderTarget / RenderStats |
| **蒙皮** | ✅ **已经在 GPU 上**（`Renderer.h:52` `binding 6: jointMatrices SSBO`；`Renderer::updateSkinning()` 遍历 `SkinnedMeshComponent` 采样关节矩阵写入 SSBO）→ **不需要**"转 GPU 蒙皮"这一步 |
| **物理** | 凸体（球/盒/胶囊/凸包）/ SAP 广相 / GJK-EPA 窄相 / SAT 盒盒 / PGS + 热启动 + split impulse / 定步长累加器 / 5 种关节 / 岛级休眠 / 序列化 / Inspector Physics 面板 |
| **场景与资产** | `Scene` / ECS / `TransformComponent` / `SceneSerializer` / `AssetManager` / tinygltf / `Skeleton` + `AnimationClip` + `sample()` / `SkinnedMeshComponent` / Persona 风格动画预览窗口 |
| **编辑器** | 11 面板 + 3 条带 / 特性注册表（34/34 有 UI 面）/ 6 工作区 / 布局与偏好落盘 / 命令面板 71 条 / 撤销 / 通知与进度 / GPU 时间轴 / Console / 5 套主题 / 中英切换（`tr()` + 词典 ~609 条）/ Hierarchy 锁与眼 |
| **脚本** | `.vks` 自研极简语言 + 热重载 + `ScriptHost` 接缝（**已在 `editor/script/`，namespace `editor`**） |
| **输入** | `engine/core/Input`（原始键鼠查询） |

### 2.2 ❌ 缺的（本系列要补的）

| 断档 | 具体缺什么 | 任务 |
| --- | --- | --- |
| **运行时框架** | 没有 `Game.exe`、没有游戏循环、没有状态栈；`ScriptEngine` 位置违反分层 | **G01~G05** |
| **输入映射** | 只有原始键鼠；无 Action/Axis、无上下文栈、无手柄、无重绑定 | G06 |
| **角色控制器** | `PlayerComponent` 是简易移动（无 Sweep / 台阶 / 坡度 / 贴地） | G07 |
| **物理玩法接口** | 有接触对但**无回调**；无触发体；`friction/restitution` 混在 `CollisionComponent` 里 | G08 |
| **相机系统** | 只有单个 `Camera` 类；无 `CameraComponent`、无混合栈、无震动/跟随 | G09 / G10 |
| **动画运行时** | 只有 `sample()`；**无状态机、无混合、无层、无根运动、无事件、无 IK** | **G11~G13** |
| **音频** | ~~**零**~~ → **已有**（2026-10-18 完成，见 G14 段） | **G14 ✅** |
| **游戏 UI/HUD** | 只有编辑器 ImGui（不能给玩家用） | **G15** |
| **持久标识** | `Entity.id` 是运行时序号，重启就变 → 存档对不上号 | **G16**（设计已定 → `persistence_roadmap.md`） |
| **存档** | `SceneSerializer` 只能存整份关卡，无增量进度存档 | **G17**（同上） |
| **预制体** | 无 | **G18**（同上） |
| **关卡流送** | 无异步加载 / 无分块 | **G19**（同上） |
| **运行时设置** | 无（设置只活在编辑器偏好里） | G20 |
| **世界规模** | 无地形 / 水 / 植被 / 动态天空 / 烘焙（对应 R6-2~R6-5、R5-2~R5-3） | G21~G25（**主序已改** → `world_scale_roadmap.md`） |
| **材质实例** | **无**。材质是共享资产（`Material*` 指向 `AssetManager` 缓存），靠"每个实体起唯一名字"伪装成逐物体；<br>实测 3 个 bug：同名静默合并 / 身份是数组下标 / 导入模型的覆盖不能持久化 | **W0~W1**（本表原本**没有这一项**） |
| **脚本能力** | `.vks` 只能做属性动画；无实体查询、无事件、无跨帧状态 | **G26~G28** |
| **出货** | 无资产打包、无 Shipping 剔除、无崩溃回收、无运行时性能 HUD | **G29~G32** |

---

## 3 · 依赖总览与推荐执行顺序

```
┌──────────────────────────────────────────────────────────────┐
│ M1 · 运行时骨架（P0 阻断，必须先做）                          │
│                                                              │
│  G02 脚本引擎搬家 ★路障★ ──► G01 CMake 三目标拆分            │
│         │                          │                         │
│         └──────────────┬───────────┘                         │
│                        ▼                                     │
│              G03 GameLoop（固定步长）                         │
│                        │                                     │
│                        ▼                                     │
│              G04 GameState 栈                                 │
│                        │                                     │
│                        ▼                                     │
│              G05 Runtime 启动配置                             │
└────────────────────────┬─────────────────────────────────────┘
                         │
      ┌──────────────────┼──────────────────┬──────────────────┐
      ▼                  ▼                  ▼                  ▼
┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐
│ M2 手感地基   │  │ M3 表现层     │  │ M4 内容/存档  │  │ M6 脚本扩展   │
│ G06 输入映射  │  │ G11 动画 I    │  │ G16 GUID     │  │ G26 实体/组件 │
│ G07 角色控制  │  │ G12 动画 II   │  │ G17 存档     │  │ G27 事件/输入 │
│ G08 物理接口  │  │ G13 动画 III  │  │ G18 Prefab   │  │ G28 协程/状态 │
│ G09 相机系统  │  │ G14 音频      │  │ G19 流送     │  └──────────────┘
│ G10 玩家装配  │  │ G15 游戏 UI   │  │ G20 运行时设置│
└──────┬───────┘  └──────┬───────┘  └──────┬───────┘
       │                 │                 │
       └─────────────────┴────────┬────────┘
                                  ▼
                     ┌──────────────────────────┐
                     │ M5 · 世界规模（R 系列对接）│
                     │ G21 地形 ← R6-4!          │
                     │ G22 水体 ← R6-2!          │
                     │ G23 植被风 ← R6-3!        │
                     │ G24 动态天空 ← R6-5!      │
                     │ G25 烘焙消费 ← R5-2/R5-3! │
                     └────────────┬─────────────┘
                                  ▼
                     ┌──────────────────────────┐
                     │ M7 · 出包与运维            │
                     │ G29 资产打包 .pak         │
                     │ G30 Shipping 剔除          │
                     │ G31 崩溃回收 + 日志        │
                     │ G32 性能 HUD + 控制台      │
                     └──────────────────────────┘
```

**关键路径**：`G02 → G01 → G03 → G04 → G06/G07 → G11 → G15/G14 → G17 → G29`。
其余任务可以并行插空，**但这条链断了游戏就出不来**。

---

## 4 · 阶段 M1 · 运行时骨架（G01 ~ G05，P0 阻断）

### ✅ M1 交付记录（2026-10-17）

**结论：5/5 任务完成，`Game.exe` 与 `Editor.exe` 并存且共用同一个 `VennEngine.lib`。**

**交付物**

| 位置 | 内容 |
| --- | --- |
| `engine/script/ScriptEngine.{h,cpp}` | G02：`git mv` 自 `editor/script/`，namespace → **`vks`**（选它而不是 `engine::script`：`.vks` 就是语言名，也不与目录级通用名撞车） |
| `engine/physics/ScenePhysics.{h,cpp}` | **计划外但必要**：把"场景⇄物理"的绑定（建世界 / 运动学同步 / 写回实体 / `eulerXYZFromMatrix` / `scaleShapeForWorld`）从 `EditorApp.cpp` 提出到引擎层，编辑器与运行时**共用同一份**。这是 §15 风险 #10「编辑器里调好的手感出包后不一样」的**结构性**对策 —— 不共用一份，靠"两边都记得同步改"是防不住的 |
| `runtime/GameLoop.{h,cpp}` | G03：累加器 + `maxSubSteps` + 丢余量 + `timeScale` + 帧率上限 / 失焦降频 |
| `runtime/GameState.{h,cpp}`、`GameStateStack.{h,cpp}` | G04：`blocksUpdate` / `blocksRender` 两维独立 → "Paused 停更新但继续出帧"是**结构**保证，不是每处 `if` 记得写 |
| `runtime/RuntimeConfig.{h,cpp}`、`RuntimeLog.{h,cpp}`、`RuntimeApp.{h,cpp}`、`main.cpp` | G05：CLI > env > `game.json` > 默认；日志落 `%LOCALAPPDATA%/Venn/<game>/logs/` |
| `assets/game.json`、`assets/scenes/sandbox.json` | 出厂配置 + 示例关卡（Ground + 3 个立方体，用来做确定性断言） |
| `CMakeLists.txt` / `Makefile` | `VennEngine`(STATIC) + `VennEditor`(→`Editor.exe`) + `VennRuntime`(→`Game.exe`)；`make game` / `make run-game` |
| `tools/verify_runtime_{build,loop,states}.py` | 三条新回归用例（已进 `DEFAULT` + `TRIGGERS`） |

**与计划的偏差（都是主动选择，不是偷工）**

1. **不建 `runtime/CMakeLists.txt`**，改为在根 `CMakeLists.txt` 里 `file(GLOB_RECURSE) runtime/*.cpp`。
   理由：项目既有约定就是根文件统一 GLOB（新 `.cpp` 自动进构建），多一个子目录 CMake 会制造第二种约定。
2. **资产拷贝改成全局 `VennShadersCopy` / `VennAssetsCopy`**（`if(NOT TARGET)` 守卫 + `ALL`），
   而不是"每个 target 各拷一份"—— 两个 exe 等于把 shaders/assets 拷两遍，且 `POST_BUILD` 的老坑还在。
3. **新增 `MYVK_FIXED_DT`**（把每帧 dt 钉成常量）—— 验收条款要求"逐位相同"，而真实墙钟 dt 每帧都在抖，
   没有这个口子那条断言只能退化成"差值小于 eps"（那就不是逐位了）。已在 `check_feature_surface.py`
   的 `ALLOW_EXACT` 里登记（纯测试钩子）。
4. **`Application::ownsFrameRendering()`** 新虚函数：`GameLoop` 把"渲染"当一个**可计数的相位**，
   所以运行时不让基类在 `onUpdate` 之后再画一次；默认 `false` → 编辑器行为逐位不变。

**验证入口（可复跑）**

```bash
python tools/verify_runtime_build.py     # 结构：两 exe / 引擎库零 editor+runtime / 重命名历史
python tools/verify_runtime_loop.py      # 定步长：自检 3 条 + GAME-LOOP 字段 + 编辑器 vs Game 逐字符
python tools/verify_runtime_states.py    # 状态栈 + 配置优先级 + 日志落点
python tools/check_regression_map.py     # 映射表自检（79/86 已登记，86 例在 DEFAULT —— 2026-10-19 复跑）
python tools/check_feature_surface.py    # 35/35 有 UI 面、裸 env 漏网 0（2026-10-19 复跑）
```

### 全量回归基线（2026-10-19）

第一轮 `--all` **76/86** → 修掉 SDF 图集与删除保护后 **79/86** → 再修 `verify_ui2` 的按钮断言 **81/86**。
剩下的 5 个**已逐条修完**（同日，连跑 5/5 PASS）。它们**都不是"实现坏了"**——四条是过期的期望，
一条是真产品缺陷，改法记在这里免得下一轮又从头查：

| 用例 | 当时的现象 | 定性 | 改法 |
|---|---|---|---|
| `verify_camfx` B2 | n=2364 < 3000 | **阈值过期**：3000 是"场景里有贴图"时标定的；本场景材质纯色，1.5° 偏航的 ~24px 拖尾只在**几何轮廓**上留差异（差分只在 269×171 一块） | 阈值 → **1500**（对照 B3=766、静止=0），并新增"全强度差异像素 > 低强度 ×1.5" |
| `verify_taa` C1 | n=68 < 200 | **期望过脆**：MSAA=4 已抗好锯齿，TAA 只做亚像素修边；"显著变化"该由 MSAA=1 的 D3（715 px）去证 | C1 改成**双向**：视口内最大差 ≥16（噪声地板 4）**且** n < 视口 0.1% **且** Laplacian 不升（实测 3071→2249） |
| `verify_pom` C1 | 2.7× < 3.0× | **代理量饱和**：平均差在位移超过反照率格宽（6px）后撞顶 —— 实测 45° 的 25.70 反而比 80° 的 14.94 大（非单调）；换互相关量也不行，掠射端退化成假峰（实测 **+0 px**） | 阈值 → **2.0×**，断言措辞改成"单调显著变大"，并把"为什么不能断言 ∝tanθ"写进注释；2× 足以判红"固定 UV 偏移"（≈1.0×） |
| `verify_particles` | 有时 E1b、有时 H0b、有时 G1b | **GUI 交互不稳 + 计数型瞬时量的绝对容差**：`fx_click` 原来"既不等也不验"；`alive` 跨运行实测差 1 / 5 / 10，固定 8 扛不住 | ① 新增 `fx_ensure_picker`（**先验证弹层开着**再点条目，两步各自重试）；② `fx_click` 支持 `confirm` + 重读矩形；③ `alive` 容差 → `max(8, 5%)` |
| `verify_notifications` | `CONSOLE-DUMP info=2048 err=0`，而 `NOTIF-DUMP error=1` | **真产品缺陷**：一次普通启动打 2000+ 条 INFO，把启动期那条 ERROR 顶出 2048 的环形缓冲 —— **用户也看不见**，正好违背 Console 面板存在的理由 | `ConsolePanel::evictOverflow()`：溢出时**优先丢最老的 INFO**，WARN/ERROR 留到最后。现打 `info=2047 err=1` |

**否定测试（5 条改过的判据，全部实测）**：camfx 用"MB 没生效/静态不模糊"两种坏输入 → n=0 变红；
taa 用"TAA 没跑（max=0）"与"TAA 把画面重画了（n=623、Laplacian 上升）"→ 都变红；
pom 用"固定 UV 偏移（角度无关）"→ 倍数 1.00 变红；正例 3/3 绿。

本条之外，`frustum_cull` / `bvh` / `ibl` / `csm` 偶发全红（单跑全 PASS）——**别补跑"凑干净"**。

**M1 明确*不*做的事**（避免后面误判"已经有了"）：没有菜单 UI（G15）、没有输入映射（G06）、
没有玩家（G10）、运行时没有任何 ImGui 界面（硬约束：不复用编辑器的 `io`）。
所以 `MainMenu` 现在是**不透明**状态（世界不渲染），`Game.exe` 起来后默认直接进 `Playing`
（等价于"菜单上自动按了继续"）。

**下一项**：`M2 · G06 输入映射` —— 在此之前先做 §13 提到的 `E03 设置层级`
（**该前置已于 2026-10-17 完成**：`engine/render/RenderSettingsIO.*` 已存在，逐 `ParamDesc`
落盘到项目 `config/render.json`，`MYVK_*` 退为覆盖层；E26 画质档位 2026-10-19 又在其上
补了 `render::fillLayerForPreset` 单一落盘入口。原记录"目前不存在"是 M1 当时的口径。）

### ✅ G01 · CMake 三目标拆分（引擎库 / 编辑器 / 运行时）

- **目标**：把"一个 `Editor.exe`"拆成"一个引擎静态库 + 两个可执行文件"，让 `Game.exe` 存在。
- **改动点**
  1. `CMakeLists.txt` 拆三个 target：
     - `VennEngine`（**STATIC**，收 `engine/**`）—— 只依赖第三方（Vulkan / GLFW / ImGui / tinygltf / stb），
       **不认识 `editor/` 也不认识 `runtime/`**；
     - `VennEditor`（`editor/**` → `Editor.exe`）—— 链 `VennEngine`；
     - `VennRuntime`（**新建 `runtime/**`** → `Game.exe`）—— 链 `VennEngine`。
  2. 头文件目录用 `target_include_directories(VennEngine PUBLIC engine)`（**PUBLIC**，两个消费者都要）→
     现有 `#include "render/Renderer.h"` 全部照旧可用，**不产生大规模 include 改写**。
  3. 资产拷贝的 `add_custom_target(... ALL)` 迁到 `VennEngine`（**两个 exe 都要资产**）；
     ⚠ 保持 `ALL`（**禁用** `POST_BUILD`，见 MEMORY「资产拷贝必须 ALL」）。
  4. `VennRuntime` 追加 `add_subdirectory(runtime)`；`runtime/main.cpp` 先只做一个 `Window` + 清屏
     （**证明拆分成活**，不做别的）。
- **涉及文件**：`CMakeLists.txt`（改）、`runtime/CMakeLists.txt`（新建）、`runtime/main.cpp`（新建）
- **验收**
  1. `build/bin/Editor.exe` 与 `build/bin/Game.exe` **同时**存在；
  2. 两者都能起来（`Game.exe` 起一个深色空窗口即可）；
  3. E 系列既有回归**不因拆分变红**（拆分是纯结构改动）；
  4. `cl /Zs` 级别的硬证据：`VennEngine` 的源文件列表里**不含** `editor/` 或 `runtime/` 任何文件。
- **规模**：**M**
- **依赖**：`G02`（先搬家再拆，否则引擎库里会混进 editor 的脚本引擎）；⚠ **CMakeLists.txt 在
  `GLOBAL_GLOBS`** → 本轮**必然全量重跑**，按项目纪律是预期行为。

### ✅ G02 · 脚本引擎搬家：`editor/script/` → `engine/script/` ★唯一路障★

- **目标**：消除分层冲突——**运行时必须能用脚本，而 `engine/` 不认识 `editor/`**。
- **改动点**
  1. `git mv editor/script/ScriptEngine.{h,cpp} engine/script/`；
  2. namespace `editor` → 改 `engine::script`（**或保留 `vks`**，避免与既有 `engine::` 命名空间打架）；
  3. `ScriptHost` 接缝**保持原样**（它的注释已写明"换 Lua 只需按这个接缝重实现"）——
     **这是本任务的价值：把接缝从编辑器层解放到引擎层**；
  4. 更新引用点：`editor/EditorApp.cpp`、`editor/` 下的脚本面板（`ScriptPanel` / 脚本检查器页签）等
     **约 5~8 处 include + 类型名**；
  5. ⚠ `ScriptHost` 现在暴露的引擎能力里若有"编辑器专属"的东西（如 `structuralEdit` 撤销栈），
     **要抽成可选注入**：运行时没有撤销栈，不能让引擎层硬依赖它。
- **涉及文件**：`editor/script/ScriptEngine.{h,cpp}` → `engine/script/`、所有引用点
- **验收**
  1. 编译器不再有"engine 引用 editor 符号"的路径（`VennEngine` 能独立编译）；
  2. `verify_character_window`（脚本动画相关用例）**复跑 PASS** —— 证明搬家没改行为；
  3. `git log --follow` 能看到重命名历史（用 `git mv` 而非删+建）。
- **规模**：**S**（改动量小，但**必须先做**）
- **依赖**：无（**它是所有其他 G 任务的前置**）

### ✅ G03 · `GameLoop`（固定步长 + 帧管线）

- **目标**：一个**与编辑器无关**的游戏主循环，物理/逻辑/渲染的解耦顺序被固定下来。
- **改动点**
  1. `runtime/GameLoop.{h,cpp}`：
     ```
     while (!quit) {
         pollInput();                       // 事件泵
         acc += deltaTime;
         while (acc >= 1/60) { fixedUpdate(1/60); acc -= 1/60; }   // 物理 + 玩法逻辑
         variableUpdate(deltaTime - acc);   // 相机 / 动画插值 / UI
         render(scene, camera);             // ⚠ 只调 Renderer，不碰面板
     }
     ```
     ⚠ 复用物理已有的**定步长累加器**约定（`fixedTimeStep` / `maxSubSteps`），**不要另立一套**；
  2. **顺序契约**（与编辑器 Play 态一致，保证"编辑器里调好了，出包一样"）：
     `脚本 → 玩家 → 物理 → 碰撞 → 动画 → 音频 → UI → 渲染`；
  3. 时间源：`Time` 已有（`engine/core`），加**游戏时间缩放**（`timeScale`，用于子弹时间/暂停）；
  4. 帧率策略：默认 vsync 开（Intel Iris Xe 别无谓空转），`--unlocked` 可选关；
     加**帧率上限**与**后台降频**（窗口失焦 → 降到 15fps，省电且不易过热）。
- **涉及文件**：`runtime/GameLoop.{h,cpp}`（新建）、`runtime/main.cpp`（接线）
- **验收**
  1. `MYVK_RUNTIME=1` 下日志有 `GAME-LOOP tick=.. fixed=.. var=.. render=..`（每 60 帧一行）；
  2. **物理确定性与编辑器一致**：同一场景、同一输入序列，编辑器 Play 与 `Game.exe` 跑 300 帧后
     实体位置**逐位相同**（这是"编辑器里调好了出包一样"的硬证据）；
  3. 时间缩放 = 0 时物理**完全停住**（位置零漂移）。
- **规模**：**M**
- **依赖**：`!G01`、`!G02`

### ✅ G04 · `GameState` 栈（Loading / MainMenu / Playing / Paused / GameOver）

- **目标**：把"游戏当前处于哪个阶段"变成**栈结构**，压栈即暂停下层。
- **改动点**
  1. `runtime/GameState.h`：`struct GameState { virtual void onEnter/onExit/onUpdate/onRender; bool blocksUpdate, blocksRender; }`；
  2. `runtime/GameStateStack`：`push/pop/replace`；
     - `Playing` 之上压 `Paused` → `Paused::blocksUpdate = true`（下层物理停）、`blocksRender = false`（画面还看得见）；
     - `MainMenu` 之下不压任何东西；
  3. 五态：`Boot → Loading → MainMenu → Playing ⇄ Paused → GameOver`；
  4. ⚠ **不引入 ImGui docking 那一套**（项目硬约束）；状态自己的 UI 走 G15 的运行时 UI。
- **涉及文件**：`runtime/GameState.{h,cpp}`、`runtime/GameStateStack.{h,cpp}`（新建）
- **验收**
  1. `MYVK_RUNTIME_TRACE_STATES=1` 打印状态迁移序列，脚本可断言 `Boot>Loading>MainMenu>Playing>Paused>Playing>MainMenu`；
  2. `Paused` 时物理**零漂移**；`Paused` 时画面**仍在渲染**（帧计数继续涨）；
  3. 栈空时自动退出（不黑窗卡死）。
- **规模**：**M**
- **依赖**：`!G03`

### ✅ G05 · Runtime 启动配置（场景加载 / 参数 / 日志 / 窗口）

- **目标**：`Game.exe` 有**自己的**启动流程，不再借用编辑器的 `EditorApp`。
- **改动点**
  1. `runtime/RuntimeConfig`：从 `--scene <path>` / `game.json`（**游戏自己的**配置文件）读：
     起始场景、窗口尺寸、是否全屏、目标帧率、音频设备、语言、画质档位；
  2. 窗口：复用 `engine/core/Window`（**已修好启动闪白**：隐藏创建 + 深灰刷 + 首帧后 `show()` +
     若需缓冲用 `MYVK_STARTUP_DELAY` 的口子）→ **运行时直接享受这个修复**；
  3. 分层日志：`runtime` 自己的 logger tag（`GAME-*`），沿用 `VK_LOG_*`；日志**落盘到
     `%LOCALAPPDATA%/Venn/<game>/logs/`**（不是仓库根，出货版不能往安装目录写）；
  4. 命令行优先级：**CLI > env > `game.json` > 内置默认**；
  5. ⚠ 运行时**不读** `config/editor_layout.json` / `editor_prefs.json`（那是编辑器的）。
- **涉及文件**：`runtime/RuntimeConfig.{h,cpp}`（新建）、`runtime/main.cpp`
- **验收**
  1. 清空全部 `MYVK_*` 环境变量后仍能按 `game.json` 正常启动（**总验收 #2**）；
  2. `--scene` 指定的场景被加载（日志 `GAME-SCENE path=.. entities=..`）；
  3. 日志文件生成在**用户目录**，不在仓库路径下。
- **规模**：**M**
- **依赖**：`!G03`、`!G04`

---

## 5 · 阶段 M2 · 手感地基（G06 ~ G10）

> **这一段决定"好不好玩"**。渲染决定"看不看得下去"，手感决定"想不想继续玩"。

### ✅ M2 交付记录（2026-10-17）

**代码与自检全绿**，回归用例 `verify_runtime_m2`（44 条断言）+ `verify_input_bindings`
（44 条断言）已进 `DEFAULT` + `TRIGGERS`。

| 项 | 落点 | 状态 |
|---|---|---|
| G06 输入映射 | `engine/core/InputMap.{h,cpp}`（**新建**，`core::Input` 一格未改） | ✅ 实现 + 自检 48 条 |
| G07 角色控制器 | `engine/physics/CharacterController.{h,cpp}`（新建，运动学胶囊五段式步进） | ✅ 实现 + 自检 18 条 |
| G08 接触事件 | `engine/physics/ContactEvents.{h,cpp}`（新建）+ `ScenePhysics` 支持触发体与材质覆盖 | ✅ 实现 + 自检 11 条 |
| G09 相机系统 | `engine/scene/CameraSystem.{h,cpp}`（新建，编辑器与运行时**共用一份**） | ✅ 实现 + 自检 13 条 |
| G10 玩家装配 | `runtime/PlayerController.*` + `runtime/SpringArm.*`（新建） | ✅ 实现 + 自检 9 条 |
| 五个新组件 | `engine/ecs/Components.h` | ✅ 实现 |
| **序列化** | `SceneSerializer` 四个块（`character` / `trigger` / `physicsMaterial` / `camera`）+ 运行时 `--dump-scene` 往返自检 | ✅ 读→写→再读闭环已验证 |
| **UI 面** | Tool 页签 Add Component 四项；Physics 页签「角色控制器 / 触发体 / 物理材质」三段；World 页签「相机组件」段 + 相机优先级列表；**Preferences ▸ Bindings 页（G06 重绑定）** | ✅ 分区体锚点 `INS-PHYS-CHAR-BODY` / `INS-PHYS-TRIGGER-BODY` / `INS-PHYS-MAT-BODY` / `INS-CAM-COMP-BODY` / `INS-CAM-LIST` / `INS-BIND-BODY` / `INS-BIND-ROW-*` 机器可验 |
| **回归** | `tools/verify_runtime_m2.py`（A 自检 / B 读盘锚点 / C 读写往返 / D 老场景基线不变 / E UI 面）+ `tools/verify_input_bindings.py`（A~I 九组） | ✅ 分别约 40 秒 / 25 秒 |
| 测试场景 | `assets/scenes/runtime_test/player_scene.json`（7→6 实体，覆盖四个组件） | ✅ |

#### G06 键位重绑定：三件前置 + 面板（2026-10-17 补齐）

上一版这里写着" ⚠ 尚未交付"：`InputMap` 只活在 `RuntimeApp` 里、没有落盘、撤销栈也只认
场景快照。三件前置按顺序补完之后，面板是最后 1/3 的工作量，落点如下：

| 前置 | 落点 | 关键决定 |
|---|---|---|
| ① 编辑器持有绑定表 | `EditorContext::inputMap()`（按值持有一份 `core::InputMap`） | **从不 `poll()`**：编辑器的键鼠归 `core::Input` 与各面板。让它开始采集，两套输入会在同一个窗口上互相抢 |
| ② 落盘 | `editor/InputMapIO.{h,cpp}`（新）+ `assets/input_map.json`，编辑器写、运行时读（`--input-map` / `MYVK_RUNTIME_INPUT_MAP`） | **出厂绑定的唯一事实源仍是 `makeDefault()`**，文件只是覆盖；仓库里**故意不放**一份 `input_map.json` 当默认资产 —— 那样出厂表就有两份，改了 `makeDefault` 忘了改 JSON 会出现"新玩家键位不对" |
| ③ 非场景状态的撤销 | `EditorContext::inputMapEdit()`（模板）+ 现成的 `LambdaCommand` + **整表 JSON 快照** | 复用 `SceneSnapshotCommand` 的取舍理由：绑定表几百字节、操作频率是"人按一下键"，精确回滚要处理"新加/替换/多来源里的哪一条"，快照一行搞定且绝不出错 |
| 面板 | `editor/KeyBindingsPage.{h,cpp}`（新）→ **Preferences 第 7 页 "Bindings"** | 不做成新面板：面板表的 `Alt+1..9` 已经**用满**，第 10 个要么没快捷键要么动 `kDigitKeys[9]` 那条脆弱的排他规则；Preferences 本来就是全局设置的家（E17），加一页不动布局、不动面板数 |

面板本体：上下文下拉（`Gameplay` / `Menu`）+ 动作表三列（动作 / 当前绑定 / Rebind·Reset·Clear）
+ 冲突**逐行红线** + 顶部 `Save / Reload / Reset all`（二次确认）+ `* unsaved` 标记。
捕获是**模态**的：`handleShortcuts` 第一行 `if (inputCaptureActive()) return;`，否则玩家把
Jump 绑到 Space 的那一刻会先把场景跑起来。

**三个写入口的语义**（都写进了代码注释，因为它们都容易想当然）：
* `setActionBinding` 只替换**同一来源类型** —— 改键盘不该顺手把手柄 A 也解绑；
* `clearActionBinding` 只解**指定来源**，且**动作定义保留** —— 动作是轴的锚点，删了整根轴会静默失效；
* `resetActionBinding` 从 `makeDefault()` 里抄那一条 —— 出厂表只有一份，面板不另存。

**G06 面板自己的两个坑**：
8. **冲突检测不能只比 `(source, code)`**。手柄轴的**正负两半**建在同一根轴上
   （`MoveForward` / `MoveBack` 都在 `LEFT_Y`，`scale` ±1），只比 code 会让出厂表报 **8 对假冲突**，
   面板上满屏红字，真正的"两个动作抢同一个键"反而被淹掉。判据要带方向：同向才算抢。
9. **捕获不能走 `core::Input::keyPressed`**。它在 `io.WantCaptureKeyboard` 为真时**一律返回
   false**，而打开 Preferences + ImGui 键盘导航恰好就是那个状态 → 面板会"按什么都没反应"。
   捕获是编辑器自己的模态操作，直接 `glfwGetKey` 判边沿，不参与 UI 门控。

**过程中踩到并写进代码注释的坑**（都是"想当然"型，值得记住）：
1. **`sweepCapsule` 是"推出式"的，不会"停下"**。用它做向下找落点，撞到台阶上棱会被
   棱的斜法线**一边下滑一边往回推**，最后退回台阶正面的墙根（落点 z 从本该的 1.074
   一路退到 0.915）→ 上台阶分支每帧"成功"但一点没往前。正解：二分求"最高不穿透高度"
   （`dropUntilContact`）。
2. **向下吸附不能凌驾于"不许穿模"之上**。胶囊搭在台阶棱上时，中心**正下方**是下一层
   地面，吸附会把它按进台阶立面 → 刚踩上去就被下一帧抵消。正解：按下去之后用
   `capsuleFree` 验一次，穿就这一帧不按。
3. **"站不住"和"是墙"是两个判断**。台阶的接触是**上棱**（法线 nz≈0.64），它既不是
   地面也不是墙 —— 共用 `cosSlope` 一个阈值会让爬台阶的每一帧都被清零水平速度，
   0.3 m 的台阶要蹭五十多帧。
4. **上台阶的判据要用"意图的一半"**，不能是固定 5 mm。太松的话"贴着 0.6 m 台阶往上
   挪了 2 cm、落在它的顶棱上"也算成功 → 角色被一步一步抬上它根本跨不上去的高度。
5. **`m_pitch` 的符号**：`scene::Camera` 的 pitch 是**仰角**（正 = 相机在上方俯视，
   见 `Camera::angleDir` 的注释）。默认写成 -0.30 → 相机落在注视点下方、镜头朝上，
   第三人称变成"从脚底板往上拍"。鼠标 dy 向下为正，所以是 `pitch += dy`。
6. **第一帧的姿态必须初始化**。`CameraSystem` 在第一帧渲染之前就会把 `m_pose` 写进
   场景相机，而第一个固定步可能还没跑 → 屏幕上先闪一帧默认的 `fov=45`，再跳到组件的
   `55`（日志里前一行 CAM-POSE 就是证据）。正解：`attach()` 里就成型。
7. **测试的期望值本身可能是错的**。原 B1/B2 是"走 150 帧后断言还在 0.3 m 台阶上"，
   可 150 帧 × 4.5 m/s = 11 m，角色早跨过去了 —— 期望 z=1.2、实测永远 0.9，看起来
   像实现坏了。改成"台阶顶面加宽 + 末端砌一道矮墙"，让角色**停在台阶顶上**再断言。

---


### G06 · 输入映射（Action / Axis / 上下文栈 / 手柄）

- **目标**：游戏逻辑只认**动作名**（`Jump` / `MoveForward`），不认键码。
- **改动点**
  1. **不改** `engine/core/Input`（编辑器在用它）→ **新建** `engine/core/InputMap.{h,cpp}`：
     - `Action`（枚举 + 名字）与 `Axis`（一对 Action 或单值）；
     - `Binding{ key, gamepadButton, gamepadAxis, modifiers, deadzone, scale }`；
     - `InputContext`（`Gameplay` / `Menu` / `UI`）**压栈**，只有栈顶上下文活跃 → 开菜单时
       输入不再传给角色（这是绝大多数"打开菜单角色还在跑"bug 的根治办法）；
  2. 查询 API：`WasPressed(action)` / `IsHeld(action)` / `GetAxis(axis)`；
  3. 手柄：**用 GLFW 的 gamepad API**（`glfwGetGamepadState`），**不引 SDL**（离线构建偏好）；
  4. **重绑定**：绑定表存 JSON；编辑器侧做 key-capture（E 系列的活，见 §12）；
  5. 死区处理在轴合成之后（摇杆漂移不至于让角色爬）。
- **涉及文件**：`engine/core/InputMap.{h,cpp}`（新建）；`runtime/` 侧接线
- **验收**
  1. `MYVK_INPUT_TEST=1` 无窗口自检：注入虚拟按键序列 → 断言 `WasPressed` 只在一帧为真（**边沿正确**）；
  2. 上下文切换：压 `Menu` 后 `Gameplay` 的 `Jump` **不再触发**；
  3. 手柄无设备时**不崩**（退化为空状态）；
  4. 绑定 JSON 往返：改一个键 → 存 → 读 → 生效。
- **规模**：**M**
- **依赖**：`!G03`

### G07 · 角色控制器（Kinematic Capsule：Sweep / 台阶 / 坡度 / 贴地）

- **目标**：第三人称手感的地基。**不要用刚体滑块做角色**（会被台阶卡住、会抖、会被斜坡弹飞）。
- **改动点**
  1. `engine/physics/CharacterController.{h,cpp}`：
     - **胶囊 Sweep**（复用已有 GJK/EPA）；分步推进（按剩余距离迭代，最多 N 次）避免穿透；
     - **台阶高度**（`stepHeight`，默认 0.35m）：水平移动被挡 → 试着抬高 `stepHeight` 再落回；
     - **最大坡度**（`slopeLimit`，默认 46°）：法线夹角超限则该面视为墙；
     - **贴地**（`groundSnap`）：走下坡时把角色吸附到地面（否则会"小跳"）；
     - **不产生反弹**：角色不接受冲量，只读碰撞结果；
  2. `ecs::CharacterControllerComponent{ height, radius, stepHeight, slopeLimit, groundSnap, gravityScale, maxSpeed, jumpSpeed }`；
  3. **与物理的分工**：角色是**运动学体**（kinematic）——写进物理世界当运动学代理，
     这样它能推动动态刚体（推箱子），但**不被动态刚体推走**；
  4. ⚠ **与 `PlayerComponent` 的关系**：`PlayerComponent` 是现有"便携移动"（编辑器 Play 态在用）。
     **不要删它** → 新增 `CharacterControllerComponent`，两者**互斥**（同时挂时角色控制器优先，
     并在 Inspector 里给提示）。
- **涉及文件**：`engine/physics/CharacterController.{h,cpp}`（新建）、`engine/ecs/Components.h`（加组件）、
  `editor/InspectorPanel.cpp`（加 Physics 页里的角色控制器段）、`editor/SceneSerializer`（加序列化）
- **验收**
  1. `MYVK_CHAR_TEST=1` 无窗口自检（照 `MYVK_PHYS_TEST` 的做法）：
     - **台阶**：0.3m 台阶能上、0.6m 上不去（停在台阶前不穿模）；
     - **斜坡**：40° 站得住、55° 滑下去；
     - **贴地**：走下 10° 坡，Y 坐标**不出现"离地再落回"的振荡**（连续帧 Y 单调下降）；
     - **不穿透**：以 50 m/s 撞 0.1m 薄墙，**不穿过去**（扫掠分步生效）；
  2. 手感回归：`tools/verify_character_controller.py`（GUI 侧，跑 `MYVK_RUNTIME` 的测试场景）。
- **规模**：**L**（手感是磨出来的）
- **依赖**：`!G06`（要读输入）

### G08 · 物理玩法接口（接触事件 + 触发体 + 物理材质）

- **目标**：让游戏逻辑能**知道"撞上了"**——这是玩法脚本的入口。
- **改动点**
  1. **接触 diff**（`engine/physics/CollisionWorld`）：
     - 每步保存"本帧接触对集合"（`(entityA, entityB)` 排序后的键）；
     - 与上一帧做差 → `OnEnter`（新增）/ `OnStay`（两帧都有）/ `OnExit`（上一帧有本帧无）；
     - ⚠ **接触键必须稳定**（用 GUID 排序，**不是**运行时 index —— 见 G16）；
  2. `ecs::TriggerComponent{ shape, mask, onEnter/onStay/onExit 派发 }`：
     只报重叠、**不产生冲量**（物理世界里当 sensor）；
  3. `ecs::PhysicsMaterial{ staticFriction, dynamicFriction, restitution, density }`：
     从 `CollisionComponent` 里**拆出来**（现在是混在一起的）→ ⚠ 拆的时候**保持默认值逐字段等价**
     （否则所有既有物理用例的轨迹会变 → `verify_drop_to_floor` / `verify_collision_play` 会红）；
  4. **事件派发到脚本**：`ScriptHost` 加 `onCollisionEnter(entity, entity)` / `onTriggerEnter(..)`
     回调钩子 → 具体语言绑定见 G27。
- **涉及文件**：`engine/physics/CollisionWorld.*`（改）、`engine/physics/PhysicsMaterial.h`（新建）、
  `engine/ecs/Components.h`（加 `TriggerComponent`、拆 `PhysicsMaterial`）、
  `editor/InspectorPanel.cpp`（Physics 页加"触发体"勾选 + 材质段）
- **验收**
  1. `MYVK_PHYS_TEST=1` **扩展自检**：脚本化地让 A 穿过 B 的触发盒 → 断言事件序列是
     `Enter, Stay×N, Exit`（**且 Enter 恰好 1 次、Exit 恰好 1 次**）；
  2. 拆 `PhysicsMaterial` 后 `verify_drop_to_floor` / `verify_collision_play` **复跑 PASS**
     （证明默认值等价）；
  3. 触发体**不改变**动态体轨迹（对照：同一场景开关触发体，动态体 300 帧后位置逐位相同）。
- **规模**：**M**
- **依赖**：`!G16`（接触键要 GUID）；`G07` 可并行

### G09 · 相机系统（`CameraComponent` + 混合栈 + 震动/跟随）

- **目标**：把"一个 `Camera` 类"变成"多台相机的调度系统"。
- **改动点**
  1. `ecs::CameraComponent{ projection(fov/near/far/ortho), priority, viewportRect, blend }`；
  2. `CameraSystem`：每帧按 `priority` 挑**活跃相机**（或做 `SetActiveCamera`）；
  3. **混合栈**（`CameraBlendStack`）：源相机 → 目标相机 `lerp`（位置/朝向/FOV 各自曲线）——
     用于过场、切镜头、进入载具；
  4. **相机震动**（`CameraShake`）：perlin/衰减包络，**加性偏移**（不污染相机本身的姿态）；
  5. 与渲染的对接：`Renderer::render(scene, camera)` **已支持多相机**（渲染路线图里"多相机/通用
     RenderTarget"是既有的）→ 只需要"谁来当主相机"这件事从编辑器移到 `CameraSystem`。
- **涉及文件**：`engine/scene/CameraSystem.{h,cpp}`（新建）、`engine/ecs/Components.h`（加 `CameraComponent`）、
  `editor/InspectorPanel.cpp`（加 Camera 段）、序列化
- **验收**
  1. 两台相机切换 `priority` → 渲染用的相机变了（日志 `CAM-ACTIVE name=..`）；
  2. 混合：A→B 用了指定秒数，中途采样位置在 A、B 之间且**曲线单调**；
  3. 震动：把震动幅度设 0 → 相机矩阵与无震动**逐位相同**（默认关 = 基线不变）。
- **规模**：**M**
- **依赖**：`!G03`

### G10 · 玩家装配（第三人称 SpringArm + 相机跟随 + 输入接线）

- **目标**：把 G06+G07+G09 串成**一个能操控的角色**——M2 的验收就是"走起来手感对"。
- **改动点**
  1. `runtime/PlayerController`：读 `InputMap` 的 `Move` 轴 → 相对相机朝向 → 喂给
     `CharacterController`；读 `Look` 轴 → 转 SpringArm；
  2. **SpringArm**：从角色头部的 pivot 向后探射线，遇到墙**缩短臂长**（不让相机穿墙）；
  3. **相机跟随**：位置阻尼（`damping`）+ 抬头/低头限位；
  4. `Jump` 接线：贴地才可跳，二段跳由脚本决定（不在控制器里写死）；
  5. 移动速度与朝向：**转身要在移动方向上有插值**（直接瞬转看着像机器人）。
- **涉及文件**：`runtime/PlayerController.{h,cpp}`（新建）、`runtime/SpringArm.{h,cpp}`（新建）
- **验收**
  1. 测试场景（`assets/scenes/` 下游新建 `runtime_test/`）：走到墙边相机**不穿墙**（臂长缩短）；
  2. 走 10m 用时与 `maxSpeed` 相符（±5%）；
  3. **相机不抖**：静止 300 帧，相机位置极差 < 0.5mm；
  4. `tools/verify_game_flow.py` 的第一段（"能操控"）PASS。
- **规模**：**M**
- **依赖**：`!G06`、`!G07`、`!G09`

---

## 6 · 阶段 M3 · 表现层（G11 ~ G15）

### G11 · 动画运行时 I：`AnimatorComponent` + 状态机 + crossfade

- **目标**：**这是本系列最被低估的一块**。你已有 `Skeleton` + `sample()` + GPU 蒙皮，
  但**没有任何驱动层**——角色只能"原地播完一整段动画"。
- **改动点**
  1. `ecs::AnimatorComponent{ skeleton, controller(clipSet), speed, params(黑board), rootMotionMode }`；
  2. `AnimatorController` **资产**（JSON）：`States[]` + `Transitions[]`，
     转移条件 = `float / bool / trigger` 比较（`Speed > 0.1`、`Jump(trigger)`）；
  3. **Crossfade**：两状态各出一个 pose，`lerp(poseA, poseB, w)` **逐骨骼**混合；
     ⚠ 混合在**骨骼局部空间**做（别在蒙皮后的世界空间做）；
  4. **采样后处理顺序**（写死，别乱）：
     `sample(clip)</queue 层采样 → 混合 → 遮罩 → IK → 根运动提取 → 写关节矩阵 SSBO`；
  5. **不引第三方**（Assimp 的动画层不引；tinygltf 已经在，够用）。
- **涉及文件**：`engine/animation/Animator.{h,cpp}`、`AnimatorController.{h,cpp}`（新建）；
  `engine/ecs/Components.h`（加 `AnimatorComponent`）；序列化
- **验收**
  1. `MYVK_ANIM_TEST=1` 无窗口自检：
     - 状态机转移：喂参数序列 → 断言状态序列（含 trigger 恰好消耗一次）；
     - crossfade：权重 0.5 时，某骨骼姿态 = A/B 姿态的**线性插值**（数值断言）；
     - 权重 0/1 时**逐位等于**纯 A/纯 B（退回基线不飘）；
  2. 姿态输出：断言写进 SSBO 的关节矩阵数量 = 骨骼数。
- **规模**：**L**
- **依赖**：`!G03`

### G12 · 动画运行时 II：层与遮罩 + 根运动 + 动画事件

- **目标**：让"上半身开枪 + 下半身跑"能同时发生，让"跑步不滑步"，让"脚步帧能出声"。
- **改动点**
  1. **层**（`AnimatorLayer`）：`Base` / `UpperBody`，每层自己的状态机 + 权重；
  2. **骨骼遮罩**（`BoneMask`）：按骨骼名列表生成权重向量 → 上层只覆盖被遮罩骨骼；
  3. **根运动**（`RootMotion`）：
     - 采样时把 root bone 的相对位移**提取出来**，写回 `TransformComponent` 的 position/yaw；
     - `rootMotionMode ∈ {Off, FromClip, DeltaOnly}`（`Off` = 动画不驱动位移，用于"原地跑 + 代码移动"）；
     - ⚠ **不提取会出现"跑步动画在原地滑步"**——这是最常见的"游戏看着不对"的根因；
     - ⚠ 提取后**要归一化掉**（否则 root bone 的位移会被蒙皮再算一次 → 角色位移翻倍）；
  4. **动画事件**：clip 上打时间戳（`{time, name, payload}`）→ 播放跨过该时刻时派发 →
     接音频（脚步声）与脚本（命中判定窗口）。
- **涉及文件**：`engine/animation/AnimatorLayer.{h,cpp}`、`RootMotion.{h,cpp}`、`AnimationEvent.h`（新建）
- **验收**
  1. **不滑步**（**总验收 #5**）：一个走路 clip，播放一个完整周期（左右脚各一次）的**实际位移**
     与 clip 内 root 位移**比值 ∈ [0.95, 1.05]**；
  2. 遮罩：UpperBody 权重 1、Base 权重 1、遮罩指向"只上半身"→ 断言**腿部骨骼矩阵 == Base 的输出**、
     **手臂骨骼矩阵 == UpperBody 的输出**；
  3. 事件：一个含 3 个事件的 clip 播放 2 个周期 → 事件恰好派发 6 次，**顺序正确、时间戳误差 < 1 帧**。
- **规模**：**L**
- **依赖**：`!G11`

### G13 · 动画运行时 III：IK（脚贴地 + 手抓）

- **目标**：把"站在斜坡上脚插进地面"这类穿帮修掉。
- **改动点**
  1. **两骨 IK**（大腿-小腿-脚）：目标 = 射线从脚踝向下打的落点 → 调大腿/小腿旋转；
  2. **骨盆补偿**：双脚目标高度差大时，降 `pelvis` 高度（否则腿会拉直成"劈叉"）；
  3. **FABRIK**（多骨链）：手部抓握/贴墙；
  4. ⚠ **IK 是后处理**：在混合**之后**做（否则会被 blending 抹掉）；权重为 0 时**逐位等于**无 IK。
- **涉及文件**：`engine/animation/IkSolver.{h,cpp}`（新建）
- **验收**
  1. 斜坡站立：脚部骨骼世界 Y 与地面高度差 **< 2cm**（无 IK 时 > 10cm，对照组证明有效性）；
  2. 权重 0 → 姿态与关闭 IK **逐位相同**；
  3. 极端目标（脚目标在身体正上方）**不产生 NaN**（夹持 + 退化保护）。
- **规模**：**M**
- **依赖**：`!G12`

### G14 · 音频（miniaudio：3D 衰减 + 总线 + 流式音乐）

> **2026-10-18 更新：本项已独立成篇 → `docs/audio_roadmap.md`**
> （现状盘点 + 七个断点 D0~D6 + `G14-0 ~ G14-9` 的完整拆分 + 锚点表 + 否定测试清单）。
> 下面这段保留为**简版存档**，但其中四处已被修正，**冲突时以 `audio_roadmap.md` 为准**：
> ① ⚠ **不要用高层 `ma_engine` / `ma_sound`** —— 它们把混音状态关在库里，无法离线渲染
> → `--selftest` 测不了；只取"设备层"，混音自己写（与"采样不许留在 Renderer 里"同一条纪律）。
> ② "默认关 = 基线不变" → 改为 **产品默认开 + 自动化默认关**（回归脚本注入 `MYVK_RUNTIME_AUDIO=0`）。
> ③ 音频**不进** `engine/assets/AssetManager`（那是 GPU 资源管理器，且会把无窗口自检拖死）；
> 用 `engine/audio/AudioClipCache`。
> ④ `MYVK_AUDIO_TEST` / `MYVK_AUDIO` 不合 `MYVK_RUNTIME_*` 前缀（`check_feature_surface` D 组会报
> 裸 env）→ 改为 `--selftest audio` / `MYVK_RUNTIME_AUDIO`。
>
> ⚠ 另：本机实测取不到外网（两个代理端口都不通），miniaudio 的 4.2 MB 头文件**下不下来**；
> `audio_roadmap.md` §3.4 给了"自研 WASAPI 后端（零依赖）"的对照决策，推荐后者 + 设备层接口隔离。

> ✅ **2026-10-18：G14-0 ~ G14-9 全部完成**（自研 WASAPI 后端，零第三方依赖）。
> 判定信号：`Game.exe --selftest audio` → `AUDIO-TOTAL pass=118 fail=0`（**无声卡、无窗口**
> 机器上同样全绿）；`tools/verify_audio.py` 六组 33 条 check → `结果: PASS`（33 秒）；
> 真机 `AUDIO-INIT backend=wasapi device=扬声器_(...) sr=48000 ch=2 fallback=0`。
> 实测数字与**九个落地坑**（含"`Editor.exe` 只认 `MYVK_FRAMES`、不认 argv `--frames`"）
> 记在 `audio_roadmap.md` §13 / §13.1。下面这段"简版存档"**已作废**（它描述的是
> miniaudio + `ma_sound` 的方案），只看 §5 的 G14-0~G14-9。

- **目标**：**从零到有**。这是"没有它就不叫游戏"的六项之一。
- **改动点**
  1. 选 **miniaudio**（**单头文件**、MIT、Windows 走 WASAPI、无额外依赖 → 与离线构建偏好一致）；
     `third_party/miniaudio.h` + `#define MINIAUDIO_IMPLEMENTATION` **只在一个 .cpp 里**；
  2. `engine/audio/AudioSystem`：初始化设备 / 加载 clip / 声部池（一次性音效复用 `ma_sound`）；
  3. **3D 衰减**：`ma_sound_set_position` + `minDist / maxDist / rolloff`；**每帧同步**
     `AudioSourceComponent` 的位置 → sound；listener = `AudioListenerComponent`（挂相机）；
  4. **总线**：`Master / Music / SFX / Voice` + **ducking**（对话时压背景）；
  5. **流式音乐**：大文件走 `ma_sound` 的 streaming 模式（**不要**整个 decode 进内存）；
  6. 格式：`.wav`（`stb_vorbis` 现成可加 `.ogg`）；
  7. `ecs::AudioSourceComponent{ clip, loop, volume, pitch, spatial, minDist, maxDist, rolloff, bus, playOnAwake }` +
     `ecs::AudioListenerComponent{}`（**场景里恰好一个**，多个时报警取第一个）。
- **涉及文件**：`third_party/miniaudio.h`（新增）、`engine/audio/AudioSystem.{h,cpp}`（新建）、
  `engine/ecs/Components.h`、`engine/assets/AssetManager`（加音频加载路径）、
  `editor/InspectorPanel.cpp`（加 Audio 段）、序列化
- **验收**
  1. `MYVK_AUDIO_TEST=1` 无窗口自检：用 miniaudio 的**空后端（null device）**跑：
     - 加载 wav → 断言采样数/采样率正确；
     - 3D 衰减：距离 2×maxDist 时增益**低于**距离 minDist 时（数值断言）；
     - ducking：突话触发后 Music 总线增益**下降**，结束后**恢复**；
     - 声部池：并发播 64 个音效**不爆**（超限策略 = 抢最旧）；
  2. 关闭音频（`MYVK_AUDIO=0`）→ 其余系统行为**逐位不变**（默认关 = 基线不变）；
  3. 设备不可用（无声卡 / 独占被占）→ **不崩**，日志警告，静默降级。
- **规模**：**M**
- **依赖**：`!G03`

### G15 · 游戏 UI / HUD（`UiRender` + 图集 + SDF 字体 + 控件）

- **目标**：玩家看到的界面。**不能拿编辑器 ImGui 凑**。
- **为什么不能直接用 ImGui**：没有控制器导航；缩放不跟分辨率；皮肤是编辑器风；
  `io` 被编辑器占着；且它会诱使人把 UI 写进 `editor/`。
- **改动点**
  1. `engine/ui2/UiRender`：
     - 一批 **Quad** + **一张图集**（`stb_rect_pack` 打 2048²）+ **SDF 字体** atlas；
     - 按材质**合批**（同一图集 = 一次 draw call）；
  2. 控件层：`Image / Text / Button / ProgressBar / NineSlice / Layout(锚点)`；
  3. **渲染位置是硬契约**：UI 通道 **必须在 PostProcess 之后、交换链之前** ——
     放到前面会被 TAA 拖影、被自动曝光压暗；且要在**交换链分辨率**下渲染（**不跟 3D 的 DRS**）；
  4. **输入**：走 G06 的 `UI` 上下文；控制器导航（D-pad 焦点移动）**从一开始就留结构**；
  5. ⚠ **descriptor set 是真实风险**：Intel Iris Xe `maxBoundDescriptorSets = 8`，**现在恰好用满** →
     运行时 UI **不能**再要一个独立 set → 方案：图集复用**已有的采样器纹理槽**，
     或把 UI 的采样器/图集信息塞进**已有的 set0 空余 binding / push constant**。
     **这件事要在动手前先量一遍**（见 §15）。
- **涉及文件**：`engine/ui2/UiRender.{h,cpp}`、`UiWidget.{h,cpp}`、`MsdfFont.{h,cpp}`（新建）、
  `assets/shaders/ui.vert/ui.frag`（新建）、`engine/render/Renderer.cpp`（**加 UI 通道，注意顺序契约**）
- **验收**
  1. **像素判据**：纯色 UI 全屏铺 → 中心像素**上屏实测**值符合预期（⚠ 按 sRGB 二次编码后的
     **实测值**定阈值，见 MEMORY）；
  2. **分辨率无关**：同一 UI 在 1280×720 与 1920×1080 下，锚点位置**按比例一致**（±1px）；
  3. **顺序契约**：把 UI 通道临时移到 PostProcess 之前 → 断言**必红**（否定测试）；
  4. **UI 不参与 TAA/曝光**：开 TAA + 自动曝光，UI 区域像素**逐位不变**（对照关 TAA）；
  5. `MYVK_UI2=0` 时渲染结果与改动前**逐位相同**（默认关）。
- **规模**：**L**
- **依赖**：`!G03`；⚠ 动手前先做 §15 的 descriptor set 量测

---

## 7 · 阶段 M4 · 内容与存档（G16 ~ G20）

> ⚠⚠ **G16~G19 的权威设计已独立成文 → `docs/persistence_roadmap.md`**（10-18）。
> 下面四段保留为"目标 / 改动点 / 验收"的原始摘要，**但与新文档冲突时以新文档为准**。
> 新文档实测出三件本节没写的事：①组件里唯一的实体引用只有 `HierarchyComponent::parent`；
> ②场景 `version` **只写不读**（迁移表没有挂载点）；③`sceneFromJson` 是"解析 + GPU 上传"
> 的混合体 → 所以多了一个前置任务 **G16-5 序列化器三段拆分**，否则 G19 的异步铁律无解。
> ⚠ 另一处纠正：G16 验收 2 的"补发的 GUID 在同一次会话内稳定"**做不到**（随机补发）——
> 能做到的是"同一个 World 内稳定 + 首次保存即固化"。

### G16 · 稳定 GUID（实体持久标识）

- **目标**：**这是存档的前置**。`Entity.id` 是运行时序号（重启就变）→ 存档对不上号。
- **改动点**
  1. `ecs::Entity` 加 `Guid`（64 位：随机 + 时间戳 + 进程盐；或 UUIDv4 截断）；
  2. **编辑器分配**：新建实体时分配并**写进场景 JSON**（`"guid": "..."`）；
  3. **加载时去重**：场景文件里缺 GUID 的旧实体 → **加载时补发并标记 dirty**（向后兼容，
     不能因为老场景没 GUID 就打不开）；
  4. **Prefab 实例**（G18）与**存档**（G17）都靠它做引用。
- **涉及文件**：`engine/ecs/Entity.h`、`engine/scene/SceneSerializer.*`、`editor/SceneHierarchy.cpp`（显示？可选）
- **验收**
  1. 场景存→读**两轮**，同一实体的 GUID **恒定**；
  2. 老场景（无 GUID）能加载，且补发的 GUID 在**同一次会话内稳定**；
  3. 两个场景合并时 GUID **不冲突**（各 1000 个实体，交集为空）。
- **规模**：**M**
- **依赖**：`!G03`（无 GUI 依赖，可早就做 —— **建议 M1 之后立刻插入**）

### G17 · 存档系统（增量进度 + 槽位 + 版本迁移）

- **目标**：玩家进度能存能读。**不是**"再存一份完整场景"。
- **改动点**
  1. **增量语义**：存档只记**与关卡初始状态的差异**：
     - 实体级：`guid → { transform, 组件覆盖值, 是否被销毁 }`；
     - 全局：脚本的持久变量（G28）、已拾取列表、已开门列表、玩家状态（血量/位置）；
  2. `SaveGame{ version, slotName, timestamp, scenePath, entities[], globals{} }` → JSON（或二进制）；
  3. **版本迁移**：`version` 字段 + 迁移函数表（`v1→v2` 补默认字段）→
     ⚠ **档从老版本升上来必须能读**（不然玩家会骂）；
  4. **原子写**：先写 `.tmp` → `fsync` → 改名（断电不产生半截档）；
  5. 槽位 UI 走 G15 / G20。
- **涉及文件**：`engine/scene/SaveGame.{h,cpp}`（新建）、`runtime/SaveGameState`（新建）
- **验收**
  1. **往返**：存 → 改 → 读 → 世界状态**与存档时刻逐位相同**（位置/旋转/缩放/自定义组件）；
  2. **增量正确**：一个 1000 实体的场景，只改 1 个实体 → 存档文件大小**远小于**整场景
     （断言 < 整场景的 20%）；
  3. **版本迁移**：手工造一个 v1 档 → v2 代码能读出来且**关键字段不丢**；
  4. **原子性**：写档中途杀进程 → 原档**完好可读**。
- **规模**：**L**
- **依赖**：`!G16`

### G18 · Prefab（资产 + 实例 + 覆盖 + Apply/Revert）

- **目标**：一处改、处处生效；同时又允许实例级微调。
- **改动点**
  1. `Prefab` = 一份**子树 JSON**（根 + 子实体 + 各自组件，实体带 **prefab 局部 id**）；
  2. **实例** = `PrefabRefAsset{Guid} + OverrideTable{ path → 值 }`；
  3. 加载时：先展开 Prefab 默认值 → 再叠 Override；
  4. **Apply / Revert**（编辑器侧）：把实例改动写回 Prefab / 丢弃覆盖；**带撤销**（走 E 系列命令系统）；
  5. ⚠ **Prefab 之间的 GUID 不能直接复用**（同一 Prefab 摆 10 份 → 10 份都要有自己的 GUID）→
     实例化时**重新分配 GUID**，Prefab 内的引用**改指向"局部 id"**，运行时再解析成实例 GUID。
- **涉及文件**：`engine/scene/Prefab.{h,cpp}`（新建）、`editor/ContentBrowser.cpp`（拖拽创建）、
  `editor/SceneHierarchy.cpp`（Apply/Revert 右键）、`editor/EditorCommands.*`（命令登记）
- **验收**
  1. 摆 3 份同一 Prefab → GUID **互不相同**、组件值相同；
  2. 改 Prefab 源 → 3 份实例**同时**变（未覆盖的字段）；
  3. 实例上覆盖一个字段 → Apply 后 Prefab 源变化、**其余实例也变**；Revert 后回到 Prefab 值；
  4. Apply/Revert **可撤销**（Ctrl+Z 回到操作前）。
- **规模**：**L**
- **依赖**：`!G16`

### G19 · 关卡流送与异步加载

- **目标**：从"一次加载整个世界"变成"走到哪加载到哪"。
- **改动点**
  1. **分块**：先按 `Scene` 切分（最小可用），关卡 = 若干 sub-scene + 一个 `LevelManifest`；
  2. **触发器/距离**驱动加载与卸载（用 G08 的触发体）；
  3. ⚠⚠ **异步加载的铁律**：**后台线程解析、主线程上传 Vulkan**——
     `vkCreateBuffer/DeviceMemory` **绝不能**在后台线程调；用 pending 队列在帧首 drain；
  4. 过渡：加载期间弹 G15 的加载 UI（不是卡住）；已加载块**先出画面**，新块到了再补；
  5. 卸载：延迟 N 帧再真删（避免"刚卸载又被同一触发器加载回来"的抖动）。
- **涉及文件**：`engine/scene/LevelStreaming.{h,cpp}`（新建）、`engine/assets/AssetManager`（后台解析队列）、
  `runtime/GameLoop`（帧首 drain pending）
- **验收**
  1. **无 validation error**（后台线程误调 Vulkan API 会被验证层抓到 → 这是硬证据）；
  2. 加载一个 50MB 场景**不产生可感知卡顿**（最长帧 < 50ms；对照同步加载会长帧）;
  3. 往返穿过加载边界 10 次 → 实体数**不泄漏**（回到起点后实体数 == 初始值）；
  4. 卸载延迟生效（反复穿越不抖动）。
- **规模**：**L**
- **依赖**：`!G08`（触发）、`!G03`

### G20 · 运行时设置与暂停菜单（含语言/画质/音量）

- **目标**：玩家能改设置、能暂停、能退出。
- **改动点**
  1. `Paused` 状态（G04）承载菜单：继续 / 设置 / 存档 / 读档 / 回主菜单 / 退出；
  2. 设置项：分辨率 / 全屏 / 画质档位（**对接 E26 的预设**）/ 音量（4 条总线）/ **语言**
     （**复用 `editor/Localization` 的词典与 `tr()`** → ⚠ 但那个住在 `editor/`，按分层
     需要把 `Localization` **也迁到 `engine/ui/`**，或复制词典，见 §12）；
  3. **应用**：改分辨率 → 重建交换链（**已有 DRS/交换链重建的经验**）；
  4. 落盘：用户设置存**用户目录**（同 G05 的日志目录），**不写仓库**。
- **涉及文件**：`runtime/SettingsMenu.{h,cpp}`（新建）；`editor/Localization.*` → `engine/ui/Localization.*`（迁）
- **验收**
  1. 分辨率切换后渲染正常（无 validation error、无黑屏）；
  2. 音量改动**立即生效**（总线增益数值断言）；
  3. 语言切换后 UI 文案变、**布局不丢**（`trId` 的 `###` ID 稳定）；
  4. 设置落盘 → 重启**保持**。
- **规模**：**M**
- **依赖**：`!G04`、`!G14`、`!G15`

---

## 8 · 阶段 M5 · 世界规模（G21 ~ G25，多为 R 系列对接）

> ⚠⚠ **2026-10-18 起，本阶段的执行主序由 `docs/world_scale_roadmap.md` 接管。**
> 那份文档在磁盘上量了 12 件事，其中三条**否掉了本阶段的排法**：
>
> 1. **G21~G25 全是"渲染特性"，但真正的断点在最前面 —— 材质没有"实例"这一层。**
>    实测三个 bug：同名材质静默合并（后者参数全丢）/ 材质身份是数组下标
>    （`models/anime_girl.glb/mat0`）/ 导入模型的材质覆盖**存盘即丢**（dump 出 `null`）。
>    → 新增 **W0（止血）+ W1（实例）+ W2（导入）+ W3（第一个真实关卡）**，
>    排在 G21 之前。**"世界规模"的起点是"一个能存能开、能统一调参的关卡"，不是地形。**
> 2. **"世界规模"不是性能问题**：实测 50 / 200 / 500 个盒子，60 帧总耗时
>    2.55 / 2.62 / 2.60 s，GPU `frame=` 4.39 / 4.74 / 4.55 ms —— **物体数 ×10，耗时不变**。
>    所以本阶段的目标是"**有没有**"与"**调得动**"，不是"扛不扛得住"。
> 3. **本阶段原判的两处"缺失"不成立**：Z-up 转换已解决（`MeshLoader.cpp:674`、
>    `FbxLoader.cpp:503`）；LOD 已有完整实现（`assets/Lod.{h,cpp}`），**只缺蒙皮 LOD**。
>
> 下面 G21~G25 的**技术内容仍然有效**（`world_scale_roadmap.md` 逐条引用了），
> 但顺序、以及新增的材质实例整块，按新文档走。

> ⚠ **本阶段每一项都有 R 系列的硬前置**（`!`）。**渲染侧没落地时不要提前做**——
> 做出来的只会是空壳 UI。**线性关卡的游戏可以整段后置**。

### G21 · 地形（← R6-4 硬前置）

- **目标**：高度场地形 + 运行时消费。
- **改动点**：高度图资产（16/32 位）/ 地形 LOD（quadtree + 与已有 LOD 系统对接）/
  **运行时流式采样**（大世界分 tile 加载，与 G19 合并）/ splat 贴图混合 /
  物理：**高度场碰撞**（已有凸体物理不支持高度场 → 需新增）/
  编辑器侧雕刻笔刷（**E 系列 E24 + E08 的活**）。
- **验收**：地形与高度图**逐像素对应**（采样点高度与物理射线命中点高度差 < 1cm）；
  LOD 切换处**无裂缝**（相邻 tile 边缘顶点焊接）；性能：4km² 地形在目标 GPU 上 > 60fps。
- **规模**：**L**　**依赖**：`!R6-4`

### G22 · 水体（← R6-2 硬前置）

- **目标**：水面渲染 + 运行时浮力。
- **改动点**：水面网格 / 法线动画 / 折射与屏幕空间反射（复用 SSR）/ 深浅与泡沫 /
  **运行时**：浮力（`BuoyancyComponent`，按浸没有效体积算力）/ 水下后处理切换。
- **验收**：水面**不穿帮**（相机半沉入时切换正确、无闪烁）；浮力：立方体静止浮在水面时
  **吃水深度稳定**（±2cm）；水下状态切换**无 1 帧跳变**。
- **规模**：**L**　**依赖**：`!R6-2`

### G23 · 植被与风（← R6-3 硬前置）

- **目标**：实例化植被 + 风场。
- **改动点**：植被类型（草/灌木/树）/ **实例化 + 距离剔除**（复用既有 instancing + LOD）/
  风场（全局方向 + 噪声 + 局部扰动）/ 运行时：风与角色交互（踩草）。
- **验收**：10 万株草在目标 GPU 上 > 60fps（**必须用 instancing 证据：draw call 数 << 株数**）；
  风停（风力 0）时植被**逐位静止**（默认关）；草的"踩踏"影响半径可配。
- **规模**：**L**　**依赖**：`!R6-3`

### G24 · 动态天空与大气（← R6-5 硬前置）

- **目标**：真实感天空 + **昼夜循环**（游戏节奏的载体）。
- **改动点**：大气散射（或物理近似）/ 太阳/月亮/星空 / 云 /
  **运行时**：`TimeOfDayComponent{ hours, dayLengthSec, latitude }` 驱动太阳方向与色温 →
  自动接 CSM 阴影方向、IBL 环境光、雾色（**这四个必须一起变，否则画面割裂**）。
- **验收**：一天循环内**无亮度突变**（连续 600 帧亮度差有界）；日出/日落时**阴影方向连续**（无跳变）；
  时间流速设为 0 时太阳**静止**（默认关）。
- **规模**：**M**　**依赖**：`!R6-5`

### G25 · 光照烘焙的运行时消费端（← R5-2 / R5-3）

- **目标**：烘焙产物能被**运行时**读进来用（不是只在编辑器里看预览球）。
- **改动点**
  1. `lightmap`：按 GUID 关联"实体 → lightmap 索引与 UV 偏移" → 前向着色时采样（**要动 `pbr.frag`**）；
  2. **DDGI 探针网格**：运行时按位置插值最近 8 个探针 → 间接光；
  3. 资产打包（G29）时**把烘焙产物打进去**；
  4. ⚠ **改 `pbr.frag` 属于横切**（`GLOBAL_GLOBS`）→ 必然全量回归；且**必须默认关** +
     "关闭 = 与当前逐像素一致"。
- **验收**：开 lightmap 后**静态物体不再需要实时阴影**也能有正确间接光（对照：关掉方向光，
  场景仍可见）；关 lightmap → 与当前**逐像素一致**；DDGI 探针**无缝插值**（移动相机无接缝跳变）。
- **规模**：**L**　**依赖**：`!R5-2`（lightmap 那一路）、`!R5-3`（DDGI 那一路，可分开做）

---

## 9 · 阶段 M6 · 脚本能力扩展（G26 ~ G28）

> 你**已经有**一套自研 `.vks`（注释里写明了"如果要换 Lua，只需按 `ScriptHost` 这个接缝重实现"）。
> **建议顺序：先把 `.vks` 扩到能写游戏逻辑，真的不够再换 Lua。** 你的判断是对的——
> 自己写更可控、安全性是结构性的（语言里根本没有文件/网络/系统调用）。

### G26 · `.vks` 实体与组件访问

- **目标**：脚本能"找到实体、读写组件"——这是写玩法的最低要求。
- **改动点**（扩 `ScriptHost` + 语言内建函数）
  1. 实体查询：`find("Name")` / `findTag("Enemy")` / `allInRadius(pos, r)` / `self()` / `parent()` / `children()`；
  2. 组件读写：`getPosition(e) / setPosition(e, v3)` / `getHealth/setHealth` …；
     通用形式 `getComp(e, "Health")`；⚠ **写组件要过正确的 setter**（不能绕过 dirty 标记 → 否则
     物理/渲染读到旧值）；
  3. **向量/四元数**作为一等值（现在只有 `double`）→ 语言层面加 `vec3`；否则每次都要拆分量；
  4. ⚠ **性能**：脚本每帧跑 N 个实体 × M 行 → 加**调用配额**（每帧最多 K 次内建调用，超了报错），
     防止一个死循环脚本锁死帧。
- **验收**
  1. `MYVK_VKS_TEST=1` 无窗口自检：脚本 `setPosition` 后**同一帧**物理/渲染就读到新值（脏标记生效）；
  2. `find("X")` 找不到 → 返回空而不是崩；
  3. 配额：故意写死循环 → **帧时间不超预算**，日志报错且脚本被停（引擎不挂）。
- **规模**：**M**　**依赖**：`!G02`

### G27 · `.vks` 事件与输入

- **目标**：脚本能**响应**（否则只能被动动画）。
- **改动点**
  1. 事件钩子：`onStart() / onUpdate(dt) / onCollisionEnter(other) / onTriggerEnter(other) /
     onAnimationEvent(name)`（对接 G08、G12）；
  2. 输入：`axis("Move")` / `pressed("Jump")` / `held("Fire")`（对接 G06）；
  3. **生命周期**：`onStart` 前实体必须**已完全构造**（组件都在）→ 定义清楚"Start 在哪个相位跑"。
- **验收**
  1. 一个"按键 → 物体上跳"的脚本，注入按键 → 物体位移**符合预期**（端到端）；
  2. `onCollisionEnter` 里读到 `other` 的 GUID 与"实际被撞的实体"一致；
  3. `onStart` 里刚创建的实体**下一帧**才能被 `find` 找到（定义清楚，不搞玄学）。
- **规模**：**M**　**依赖**：`!G26`、`!G06`、`!G08`

### G28 · `.vks` 跨帧状态与协程（`Wait` / 序列）

- **目标**：脚本能"等一会儿再干下一件事"——这是写关卡逻辑的关键。
- **改动点**
  1. **脚本实例持久化**：`ScriptComponent` 的脚本实例**跟着实体存活**（现在似乎是每帧求值）；
     脚本内变量跨帧保留；
  2. **协程**：`wait(sec)` / `waitUntil(pred)` / `waitFrames(n)` → 让脚本"挂起"而不是阻塞帧；
     实现 = 每帧恢复可运行协程（**不是线程**，绝不引线程）；
  3. 序列（`Sequence`）：`play → 等 2 秒 → 开门 → 等玩家进入 → 关门` 这种"分镜式"逻辑有直接表达；
  4. 热重载（**已有**）要对协程友好：重载时**保留等待位置**或**明确重启**（二选一，别一半一半）。
- **涉及文件**：`engine/script/ScriptEngine.*`（扩）、`engine/ecs/Components.h`（`ScriptComponent` 加实例槽）
- **验收**
  1. `wait(1.0)` → 实际恢复时刻误差 < 1 帧（`MYVK_VKS_TEST`）；
  2. 1000 个协程同时挂着 → 帧时间**不劣化**（O(活跃协程) 而非 O(总数)）；
  3. 热重载：改脚本后协程**行为确定**（有一致语义，且用例钉死它）。
- **规模**：**M**　**依赖**：`!G26`

---

## 10 · 阶段 M7 · 出包与运维（G29 ~ G32）

### G29 · 资产打包（`.pak` + 索引 + 压缩）

> 🟡 **2026-10-28 部分交付（E28-1）**：出包的**第一刀**已经能用了 ——
> `Game.exe --package` / 编辑器 `File ▸ Package project...` → `dist/<Name>/`
> （exe + shaders + assets + config + build.json，模板工程实测 321 个文件 / 38.6 MB）。
> 详见 `docs/package_roadmap.md`，判据 `tools/verify_package.py`（39 条）。
>
> ⚠ **本节的目标仍未达成**：现在打的是"整棵 assets + 白名单引擎资产"（宁可多带，
> 保证不漏），**没有引用闭包、没有 .pak、没有压缩**。下一步是 G29-a（先写引用
> 扫描器 + 未解析即失败），再 G29-b（.pak）。
>
> ✅ **2026-10-28 更新：G29-a + G29-b 都已交付。**
> · G29-a 引用闭包：只打被引用的资产，悬空引用 = 打包失败（`tools/verify_package.py` I 组）。
> · G29-b 资产包：`--pak` → 单个 `assets.pak`（自带 LZ4 block 编解码，无外部依赖），
>   运行时按需解包到内容寻址缓存（K/J 组各 27/12 条）。
> · G29-c shader 进包 + G29-d 直读：`shaders/` 也收进同一个 `game.pak`，
>   shader 走 `pak:` 虚拟路径**直读包内存**（不落盘）。
>   实测模板工程分发从 **317 个文件 / 38.8 MB** 变成 **5 个文件 / 32.3 MB**。
> 仍未做：**资产直读**（贴图/场景还走"按需落缓存"）、**pak 当默认**、
> Shipping 剔除（G30）。详见 `docs/package_roadmap.md` §7 §8。


- **目标**：从"几千个小文件散在磁盘上"变成"一个包"。**这不是可选项**——
  散装文件的加载慢、易被误删、无法校验完整性。
- **改动点**
  1. `.pak` 格式：`header / 目录索引（path → offset,size,compressed,hash）/ 数据块`；
     压缩用 **zstd**（快且小；离线构建需引入，属可接受的单库）；
  2. `AssetManager` 加**虚拟文件系统层**：`open(path)` 先在 pak 里找，找不到再落磁盘
     （**调试期直接读散文件，出包只读 pak** —— 用 `MYVK_ASSET_PAK=0/1` 切）；
  3. **打包工具**：`tools/pack_assets.py`（或 CMake 的自定义 target）→ 只打**被引用的**资产
     （从场景/材质/预制体做**引用闭包**）；
  4. ⚠ **引用闭包必须有"未解析清单"**：闭包算完若有未解析引用 → **打包失败**（不能静默漏文件）；
  5. 流式资产（大纹理/音频）**不打进 pak**（要保持文件级别随机读）。
- **验收**
  1. **干净目录测试**（**总验收 #6**）：把 pak + exe 拷到空目录 → 能跑；
  2. 排除测试：故意漏一个材质引用 → **打包报错**（不是运行时黑屏）；
  3. 大小：demo 场景 pak 大小 **< 散文件总和的 70%**（压缩有效）；
  4. 加载时间：pak 冷启动比散文件**不慢**（索引命中）；
  5. `MYVK_ASSET_PAK=0` 时与今天行为一致（默认不变）。
- **规模**：**L**　**依赖**：`!G01`、`!G05`

### G30 · Shipping 构建剔除（编辑器 / 调试 / 校验层）

> ✅ **2026-10-28 已交付**。一句话：`make ship` → `build-ship/bin/Game.exe`，
> 那份 exe **不是**"dev 版加了个宏"，而是**成分不同**的一份二进制。
> 判据 → `tools/verify_shipping.py`（A~D 共 39 条，`--no-build` 可跳过构建）。
>
> 交付了什么（对应原计划的四条）
>
> | 计划 | 实际做法 | 判据 |
> |---|---|---|
> | CMake 加 `VENN_SHIPPING` | `-DVENN_SHIPPING=ON` + `add_compile_definitions(VENN_SHIPPING=0/1)`；**Editor / Launcher 的 target 根本不声明** | B2~B5（`build-ship/build.ninja` 里搜不到 Editor.exe / Launcher.exe） |
> | 不启 validation | `rhi::Instance` 在 shipping 下**强制** `requestValidation=false`（env 也不给翻盘口子） | C10（日志 `INSTANCE build=shipping … validation=off`） |
> | 开发命令不注册 | `--selftest` / `--dump-scene` / `--package` / `MYVK_RUNTIME_SELFTEST` / `MYVK_FIXED_DT` / `MYVK_PHYS_DUMP` 全部 `#if !VENN_SHIPPING`（**编译期**） | C4（四个开发字符串在出货 exe 里各 **0** 次，dev exe 里各 > 0 —— 对照区） |
> | 剥符号 | MSVC 链接带 `/DEBUG` → 符号在**单独的** `Game.pdb`；出包时归档进 `dist/<Name>/debug/` | B6 / D4 / D5（pdb 在包里、**不在 exe 旁边**） |
> | 版本号 | `git describe --tags --always --dirty` → `core::kGitDescribe`；`--version` / 日志 `GAME-BUILD` / shipping 标题栏 `[<git>]` | A1~A3 / C1~C3 / D3 |
>
> 实测（2026-10-28，本机）：dev `Game.exe` **11.7 MB**（Debug）→ shipping
> `Game.exe` **4.44 MB**（RelWithDebInfo）。四个开发字符串在出货 exe 里各 0 次。
> 打包体积不变（多一个 `debug/Game.pdb`，**发版时可以把它单独留档再删掉**）。
>
> 三条设计决定（都有理由，别随手改）
>
> 1. **风味只有一个事实源**：`engine/core/Build.h` 的 `core::kShipping`。
>    任何地方都不许自己写 `#ifdef NDEBUG` / `#ifdef _DEBUG` 去猜 —— 猜错的
>    方向恒是"以为关掉了，其实还开着"。
> 2. **判据必须是数字**：这一整套改动最大的风险是**假绿**（宏写了、`#if`
>    漏了，症状是"游戏照常跑"）。所以判据不是"能起来"，而是"这四个字符串
>    在出货 exe 里出现 0 次"，并配 dev exe 的**对照区**（否则"扫到 0 次"
>    也可能只是扫描方法失效）。
> 3. **出包工具 = dev 版，打进去的 exe = 出货版**：`Game.exe --package X
>    --engine build-ship/bin/Game.exe`。shipping 版**没有** `--package`
>    （玩家手上的 exe 不该带着一个能把自己的资产吐出来的工具）；包里的
>    `build.json` 记 `flavor` / `git`，pdb 归档默认**跟着风味走**
>    （shipping 归档、dev 不归档 —— Debug 的 pdb 上百 MB）。
>
> ⚠ 三个踩过的坑
>
> - `add_dependencies(VennEditor Shaders)` 在 shipping 下 **configure 直接失败**
>   （"non-existent target"）。凡是按名字点 target 的地方都要跟着 `if(NOT
>   VENN_SHIPPING)`。同族：`myvk_copy_runtime_assets(VennEditor)`。
> - `make ship` 第一版写成 `cmake -E env … cmd /c msvc-build.cmd`：Git Bash 会把
>   `/c` 当路径改写成 `C:/`，cmd 进**交互式**模式等 stdin —— 现象是"挂着十分钟
>   一声不响、连 build-ship/ 都没建出来"。改成 **target-specific `export`** 变量
>   （与一直好用的 `make build` 同一条路径）。
> - `--version` 是**出货版保留**的唯一开发味参数：玩家报问题时第一句话是
>   "哪个版本"，而他们没有别的办法告诉你。它必须在开日志 / 建窗口之前返回
>   （也不能顺手写日志 —— 出包时的风味探测就靠它，探测不该有副作用）。
>
> 还没做：**崩溃转储**（G31，`VENN_SHIPPING` 只是"不弹断言框"，真正的
> `MiniDumpWriteDump` 在 G31）；**调试绘制**本来就不在 runtime（编辑器专属），
> 所以"编译期剔除 gizmo"这一条在 runtime 上是空集。
>
> 原计划的验收 1 提到"用 `strings` 搜编辑器面板字符串" —— 实测这条**没有
> 意义**：Editor.exe 是另一个 target，runtime 从来没有链过它。真正会漏的
> 是"开发命令字符串"，所以判据换成了那四个（更小、更准、可对照）。
>
> 原计划保留（对照）
>
> - **目标**：出包版本**不能**带编辑器、调试绘制、开发命令。
> - **规模**：**M**　**依赖**：`!G29`

### G31 · 崩溃回收与日志（minidump + 栈 + 回传）

- **目标**：**玩家崩溃时你能看到现场**，而不是"我这边不崩啊"。
- **改动点**
  1. Windows：`SetUnhandledExceptionFilter` + `MiniDumpWriteDump` → 写到用户目录
     （`Crashpad` 可作后续增强，先手写够用）；
  2. 崩溃时**先 flush 日志**（否则 dump 到了、日志还在缓冲区）→ 日志缓冲区要**行缓存 + 可 flush**；
  3. 日志**自动轮转**（保留最近 3 个），大小上限；
  4. 崩溃报告：dump 路径 + 版本号 + 最近 N 行日志 → 打一个 zip 让玩家发过来。
- **验收**
  1. `MYVK_CRASH_TEST=1` 故意 `nullptr` 解引用 → dump 文件生成、日志含版本号与栈符号；
  2. 日志轮转：连续写超上限 → 只保留 3 个文件；
  3. 正常退出**不产生** dump（不误报）。
- **规模**：**S**
- **依赖**：`!G05`

### G32 · 运行时性能 HUD 与开发者控制台

- **目标**：能**在游戏里**看到性能，能不开 IDE 调参数。
- **改动点**
  1. 运行时 overlay（走 G15）：`fps / frame ms（CPU/GPU）/ draw call / 三角数 / 实体数 /
    physics ms / animation ms / audio voices`；
  2. **图形化帧时间**（最近 120 帧柱状，超过 16.6ms 的标红）——比单一 fps 数字有信息量得多；
  3. **开发者控制台**（`~` 键）：跑命令（切场景、给物品、调参数、作弊）；
     命令来自**已有的 E18 命令注册表**（⚠ 运行时只注册"运行时安全"的子集）；
  4. ⚠ **必须默认关**（`MYVK_RUNTIME_HUD=0`），且关掉后渲染**逐像素不变**。
- **涉及文件**：`runtime/DebugOverlay.{h,cpp}`、`runtime/DevConsole.{h,cpp}`（新建）
- **验收**
  1. HUD 显示的数字与外部量测**一致**（fps 与日志 `GAME-LOOP` 的帧计数比值 ±2%）；
  2. 默认关 → 逐像素不变；
  3. 控制台命令效果可断言（`scene.load X` → 日志出现 `GAME-SCENE path=X`）。
- **规模**：**M**　**依赖**：`!G15`、`!G04`

---

## 11 · 每个任务的通用 Definition of Done（runtime 版）

**任何 G 任务"完成"= 下列五件套齐备**（对齐项目既有五件套纪律）：

| # | 交付物 | 检查方式 |
| - | --- | --- |
| 1 | **实现**：功能可用（且**运行时**与**编辑器**两侧都能跑到，需要 UI 面时补齐） | 人工 + 截图 |
| 2 | **登记**：新增 env / 特性写进 `engine/render/FeatureRegistry`（引擎侧）或运行时配置表 | `check_feature_surface.py` 不报红 |
| 3 | **UI 面**：编辑器里能配（除"纯运行时"的项，如 HUD / 崩溃回收） | `check_feature_surface.py` C 组绿 |
| 4 | **序列化**：组件能存能读（新增组件必须同时进 `SceneSerializer`） | 场景往返测试 |
| 5 | **回归用例**：新增 `tools/verify_*.py` 并入 `run_regression.py` 的 `DEFAULT` + `TRIGGERS` | 该用例 PASS |

**外加五条本项目铁律**：

- **默认不破坏基线**：新增开关默认**关**，且"关 = 行为与改动前**逐像素/逐位**一致"。
- **`MYVK_EDITOR_NO_HUD=1` 下视口仍像素干净**。
- **新增 `MYVK_*` 一律过 `check_feature_surface.py`**：
  ⚠ `MYVK_EDITOR_*` 按**前缀**自动放行；**不带前缀的新开关会被判"裸 env 漏网"必须清零**
  （本轮 `MYVK_STARTUP_DELAY` 就踩过 → 要加进 `ALLOW_EXACT`）。运行时的 `MYVK_RUNTIME*` 同理，**要么登记白名单、要么走前缀**。
- **改 `TRIGGERS` + 同步 `check_regression_map.py` 的 fixture 是一个原子操作**（改完必跑 `check_regression_map.py`，很可能先红）。
- **⚠ 不要自动跑全量回归**：只有用户明确下达"开始全量回归"才跑 `--all`；日常只跑受影响用例。

---

## 12 · 与 R / E 系列的准入契约

### 12.1 G 完成时，E 系列必须同步交付的编辑器 UI 面

| G 项 | 必须同步的编辑器 UI 面 | 说明 |
| --- | --- | --- |
| ✅ G06 输入映射 | **Preferences ▸ Bindings 页**（key-capture + 冲突检测逐行标记 + 单项 Reset/Clear + 整表 Reset all） | ✅ 2026-10-17 交付：登记进命令系统（`LambdaCommand` + 整表快照，可撤销），落盘 `assets/input_map.json`，运行时读同一份。**不做成新面板**的理由见 §5 的 G06 小节 |
| ✅ G07 角色控制器 | Inspector ▸ Physics 加"角色控制器"段 | 与 `RigidBody` 段互斥提示 |
| ✅ G08 物理接口 | Inspector ▸ Physics 加"触发体"勾选 + `PhysicsMaterial` 段 | 材质从 `Collision` 段里**拆出** |
| ✅ G09 相机系统 | Inspector ▸ Camera 段 + 相机列表（优先级可调） | |
| G11~G13 动画 | **Animator 面板**（状态机图 + 转移条件编辑 + 层/遮罩 + 事件时间轴） | 这是 E24 资产编辑器框架的一个具体用户 |
| G14 音频 | Inspector ▸ Audio 段 + ContentBrowser 音频预览播放 | 完整拆分 → `docs/audio_roadmap.md` |
| G16 GUID | 可选：Hierarchy 显示 GUID / 复制 GUID | 别默认显示（噪音） |
| G17 存档 | 编辑器**不提供**存档 UI（那是运行时的事）；但要有"清空测试存档"命令 | |
| G18 Prefab | ContentBrowser 拖拽创建 + Hierarchy 右键 Apply/Revert | 带撤销 |
| G20 运行时设置 | 编辑器**不重复做**；但画质档位复用 E26 | |
| G26~G28 脚本 | Console 打印 / 脚本错误面板（**已有**）/ 内建函数帮助 | |

### 12.2 反向：E 系列改动对 G 的影响

- **`editor/Localization` 需迁到 `engine/ui/Localization`**（G20 要用）→ 迁移是**引擎层**改动
  → `engine/ui/**` 在 `GLOBAL_GLOBS` → **触发全量回归**（预期行为，别惊讶）。
- **E26 画质档位**（未做）一旦落地，G20 的运行时设置页**直接复用它的预设表**，
  ⚠ **不要另写一份档位定义**（两份定义必然漂移）。

### 12.3 分层的铁律（本系列最容易违反的地方）

```
runtime/  ──依赖──►  engine/  ◄──依赖──  editor/
   │                                          │
   └──────────── 互不依赖 ────────────────────┘

✅ 允许：runtime → engine，editor → engine
❌ 禁止：engine → runtime，engine → editor，runtime → editor
```

⚠ **`G02`（脚本引擎搬家）与 §12.2（`Localization` 迁移）都是为了让上面这张图成立。**
任何"先在 `editor/` 里做原型、以后再说"的做法，最终都会变成第二个 `ScriptEngine` 问题。

---

## 13 · 硬约束（做任何 G 任务都不能违反）

1. **依赖方向单向**：`runtime → engine ← editor`（见 §12.3）。**`engine/` 不认识 `editor/`、不认识 `runtime/`**。
2. **编辑器能力不回退**：拆分 / 搬家 / 迁移**不能**让 `Editor.exe` 少一个功能；E 系列用例不许红。
3. **新效果默认关**，且"关 = 与改动前逐像素/逐位一致"（`config/*.json` 缺失时行为不变）。
4. **新增 `MYVK_*` 必须登记**（`check_feature_surface.py`）；`MYVK_EDITOR_*` 按前缀放行，**其余要进白名单**。
5. **新增 `engine/**` 文件 = 横切**（`GLOBAL_GLOBS`）→ **必然全量回归**，这是设计如此、不是 bug。
6. **不引入 ImGui docking**（既有硬约束）；运行时 UI **也不得**复用编辑器 ImGui 的 `io`。
7. **不引重型第三方**：优先单头文件（miniaudio / stb 系）；引 zstd 这类要写进文档说明理由。
8. **Vulkan 调用只在主线程**（G19 的异步加载尤其要守）。
9. **运行时不写安装目录**：日志/存档/设置一律落用户目录。
10. **回归期间不构建、不跑 GUI 测试**；GUI 用例**同时只能有一个**（`kill_stale_editors` 无差别杀进程）。

---

## 14 · 里程碑判定信号

| 里程碑 | 内容 | 判定信号（机器可判） |
| --- | --- | --- |
| **M1 · 运行时骨架** ✅ | G01~G05 | `Editor.exe` 与 `Game.exe` **同时**存在并都能起；`Game.exe` 清空 env 也能进主菜单；物理确定性与编辑器 Play 逐位一致 |
| **M2 · 手感地基** | G06~G10 | 角色能走/能跳/不被台阶卡/相机不穿墙；`verify_character_controller.py` PASS |
| **M3 · 表现层** | G11~G15 | **不滑步**（位移/动画周期比 ±5%）；有声音（波形非零）；HUD 在 PostProcess 之后且不参与 TAA |
| **M4 · 内容与存档** | G16~G20 | 存→改→读**逐位还原**；同一 Prefab 3 份实例 GUID 互不相同但改源全变 |
| **M5 · 世界规模** | G21~G25 | 4km² 地形 > 60fps；昼夜循环无亮度突变（各 R 前置满足后才做） |
| **M6 · 脚本能力** | G26~G28 | "按键 → 上跳"端到端；`wait(1.0)` 误差 < 1 帧；死循环脚本不锁帧 |
| **M7 · 出包与运维** | G29~G32 | **干净目录**里双击可跑；崩溃出 dump；HUD 数字与外部量测一致 |

**M1 + M3 是投入产出比最高的两段**：M1 把"只有一个编辑器"这个结构性缺陷抹掉；
M3 把"看得见但玩不动"补齐（**音频 + 动画驱动 + 玩家 UI**是"像游戏"的三根支柱）。

> **M1 的三条信号 2026-10-17 全部达成**，但要说清口径：「进主菜单」现在是**状态栈里
> 的 `MainMenu` 阶段**（日志 `GAME-STATE-SEQ Boot>Loading>MainMenu>Playing`），
> **不是一张能点的菜单 UI** —— 那要等 G15。所以第三条"逐位一致"是用
> `PHYS-BODY` 转储逐字符比对来判的（`tools/verify_runtime_loop.py` 的 D 组）。

---

## 15 · 已知的墙与风险

| # | 风险 | 说明与对策 |
| - | --- | --- |
| 1 | ⚠⚠ **descriptor set 撞上限**（**最硬的一条**） | 本机 Intel Iris Xe `maxBoundDescriptorSets = 8`，**现在恰好用满 8 个**（MEMORY 已录）。**G15 运行时 UI **不能**再要独立 set** → 动手前先量：图集复用已采样器槽 / 挤进 set0 空余 binding / 走 push constant。**这件事必须在 G15 开工前做完量测**，否则做到一半发现要改架构 |
| 2 | 只有 1 块核显 | 目标帧率要**现实**：1080p 60fps 是可争取的；**别对标 4K**。G23（10 万株草）与 G21（4km² 地形）的验收目标已按此定 |
| 3 | 蒙皮**已经在 GPU** | ✅ 不是风险（`binding 6: jointMatrices SSBO`）。真正的墙是 #1 |
| 4 | 动画混合的坐标系 | ⚠ 混合必须在**骨骼局部空间**做；在蒙皮后做会得到"看着对但一转身就错"的结果 |
| 5 | 根运动与代码移动**打架** | `RootMotionMode` 必须显式选择；两者同时驱动位移 = 角色瞬移 |
| 6 | 异步加载触碰 Vulkan | ⚠ 后台线程**只能解析**，上传必须回主线程（G19 的验收就是抓这个） |
| 7 | `CollisionComponent` 拆材质 | 拆的时候**默认值必须逐字段等价**，否则既有物理用例全红（G08 的对照断言就是为此） |
| 8 | 存档 vs 老档 | 有玩家就会有老档 → **版本迁移从第一版就要有**（G17 的 v1→v2 用例是模板） |
| 9 | `SceneSerializer` 的运行时数据污染 | ⚠ 快照**只覆盖构建输入**；`assets/scenes/**`、`assets/scripts/**` 是运行时自己会写的目录，**别放进快照**（否则每轮都算变了） |
| 10 | 编辑器与运行时**行为漂移** | 编辑器 Play 用 `PlayerComponent`、运行时用 `CharacterController` → 必须有一条**够强的对照用例**（G03 的"逐位一致"）钉住，否则"编辑器里调的"和"出包的"会越差越远 |

---

## 16 · 明确的非目标（这一版不做）

写下来是为了**防止范围蔓延**——这些都是成熟引擎有、但**不影响"完整开发出一个 3D 游戏"**的东西：

- 网络 / 多人（单人游戏不需要；网络同步是另一个量级的工程）
- 移动端 / 主机后端（Windows 桌面优先）
- 可视化脚本（蓝图式节点图）——`.vks` 文本脚本够用；节点图是 E24 之后的事
- 关卡编辑器的"团队协作 / 版本控制集成 / 冲突解决"
- 程序化生成的世界（PCG）框架
- 资源热更新的 CDN / 发布运营后台
- 材质节点图（E24 的一部分，但**不属于"出游戏"的必要条件**）
- 虚拟纹理 / Nanite 级别的几何（渲染路线图的 R7 系列）
- 布料 / 软体 / 破碎（物理 v1 明确不做的延续）
- 联机存档云同步

---

## 附录 A · 缺口速查（对照成熟引擎，2026-10-17）

| 维度 | Venn 现状 | 成熟引擎基线 | 相关任务 |
| --- | --- | --- | --- |
| 可执行文件 | **1 个**（`Editor.exe`） | 引擎库 + 编辑器 + 运行时 | **G01** |
| 游戏循环 | 编辑器 Play 态代用 | 固定步长 + 状态栈 | **G03 / G04** |
| 输入 | 原始键鼠 | Action/Axis + 上下文 + 手柄 + 重绑定 | G06 |
| 角色控制 | 简易移动（`PlayerComponent`） | Kinematic Capsule + 台阶/坡度/贴地 | **G07** |
| 物理事件 | 有接触对，**无回调** | Enter/Stay/Exit + 触发体 + 材质 | **G08** |
| 相机 | 单个 `Camera` | 组件化 + 混合栈 + 震动 | G09 |
| 动画 | `sample()`（**无驱动层**） | 状态机 + 混合 + 层 + 根运动 + 事件 + IK | **G11~G13** |
| 音频 | ~~**零**~~ → WASAPI + 自研 Mixer（2026-10-18 ✅） | 3D + 总线 + 流式 | **G14 ✅** |
| 运行时 UI | **零**（只有编辑器 ImGui） | 图集 + SDF + 锚点布局 + 控制器导航 | **G15** |
| 持久标识 | 运行时序号（会变） | GUID | **G16**（设计 → `persistence_roadmap.md`） |
| 存档 | 只能存整份关卡 | 增量 + 槽位 + 版本迁移 | **G17**（同上） |
| 预制体 | 无 | 资产 + 实例覆盖 + Apply/Revert | **G18**（同上） |
| 关卡流送 | 一次全加载 | 分块 + 异步（主线程上传） | **G19**（同上） |
| 世界 | 无地形/水/植被/动态天空 | 四件套 | G21~G24（→ `world_scale_roadmap.md` W4~W7） |
| 光照 | 全实时 | + lightmap / DDGI | G25（→ 同上 W6/W9） |
| 材质 | 共享资产 + 唯一命名伪装逐物体；**无实例层**、身份是数组下标 | 母材质 + 实例 + 稀疏覆盖 | **W0~W2**（→ 同上） |
| 脚本能力 | 只能属性动画 | 实体/组件 / 事件/输入 / 协程 | **G26~G28** |
| 出货 | 无打包、无剔除 | `.pak` + shipping + dump | **G29~G31** |
| 运行时可观测 | 无 | 性能 HUD + 开发者控制台 | G32 |

---

## 附录 B · 关键架构决策记录（ADR）

| # | 决策 | 理由 | 备选与否决原因 |
| - | --- | --- | --- |
| 1 | **运行时 UI 自研**（不用 ImGui） | ImGui 无控制器导航、缩放不跟分辨率、`io` 被编辑器占着、皮肤是编辑器风 | 否决 ImGui：会诱使把 UI 写进 `editor/`，且玩家看到的是"编辑器风"界面 |
| 2 | **音频选 miniaudio** | 单头文件、MIT、Windows 走 WASAPI、无额外依赖（符合离线构建偏好） | 否决 FMOD/Wwise（商业授权）、否决 OpenAL（多一层依赖） |
| 3 | **继续用 `.vks`，暂不换 Lua** | 自己写更可控；安全性**是结构性的**（语言里没有文件/网络/系统调用）；`ScriptHost` 接缝已留好 | 否决立刻换 Lua：为一个"每帧给实体做动画 + 写点玩法逻辑"的需求背一整套通用语言语义与 GC 不划算 |
| 4 | **角色用 Kinematic Capsule，不用刚体滑块** | 刚体做角色会被台阶卡、会抖、会被斜坡弹 | 否决刚体：第三人称手感的地基是扫掠式运动学 |
| 5 | **CMake 三目标拆分**（含静态库 `VennEngine`） | 依赖方向可见、`Game.exe` 能脱离编辑器、编辑功能不回退 | 否决"一个 exe 加 `--game` 开关"：分层看不出来，且 shipping 剔除做不干净 |
| 6 | **GUID 用 64 位而非 UUID 文本** | 存档/Prefab 引用频繁比较，64 位够用且省空间 | 否决纯随机 UUIDv4 文本：比较成本高，场景文件膨胀 |
| 7 | **`.pak` 用 zstd** | 压缩率与速度平衡好；出包体积与加载时间双赢 | 否决 LZ4（压缩率偏低）、否决不压缩（体积过大） |
| 8 | **物理材质从 `CollisionComponent` 拆出** | 职责分离：碰撞形状 ≠ 表面材质 | ⚠ 拆时**默认值必须逐字段等价**，否则既有物理用例全红 |

---

> **下一步（本系列的第一个动作）**：`G02`（脚本引擎搬家）→ `G01`（CMake 拆分）。
> 这两步做完，"引擎 → 游戏"这扇门才真正打开，M2~M7 才是在同一条轨道上往上叠。
>
> ⚠ 提醒：`G01` 要动 `CMakeLists.txt`（**在 `GLOBAL_GLOBS`**）→ 那一轮**必然全量回归**，
> 按项目纪律这是预期行为，不是异常（详见 §13 第 5 条）。
