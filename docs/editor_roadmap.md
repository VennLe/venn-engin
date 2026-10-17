# 编辑器 / UI 升级任务列表（Editor UI Roadmap）

> **本文档是 `docs/editor_architecture.md` 的可执行版本。**  
> 架构文档回答"为什么这么设计"（六层 L0~L5 + 准入契约）；  
> 本文档回答"**按什么顺序做哪一件**"，每个任务给到可直接开工的改动点与机器可验证的验收条款。
>
> 定位：**面向 3A 级成熟引擎的编辑器**。设计哲学三条 —— **数据驱动 / 所见即所得 / 高度可定制**。  
> 落地形态：围绕 **编辑 → 预览 → 调试** 闭环的有机系统，而不是功能堆砌。
>
> 与渲染路线图（`docs/render_roadmap.md`，R 系列）的关系：  
> **R 系列做"引擎能力"，E 系列做"人的入口"。** 二者**成对交付**——  
> 任何 R 项完成时，它在 E 系列里的对应 UI 面必须同时落地（见 §8 准入契约）。
>
> 与游戏运行时路线图（`docs/ship_roadmap.md`，**G 系列**，2026-10-17 新建）的关系：  
> **G 系列做"成品"**（能跑、能玩、能发）。三者的分工是 **R = 画得对 / E = 用得顺 / G = 出得去**。  
> ⚠ **G 系列反过来会要求 E 系列的配套**（如 G11~G13 动画运行时需要 Animator 面板、
> G18 Prefab 需要 ContentBrowser 拖拽与 Hierarchy 右键）—— 对照表见 `ship_roadmap.md` §12.1。  
> ⚠ 另：`G02` 会把 `ScriptEngine` 从 `editor/script/` 搬到 `engine/script/`、
> `G20` 会把 `Localization` 迁到 `engine/ui/`（分层要求，见其 §12.3）；这两次搬家**都会动到
> `engine/**`（= `GLOBAL_GLOBS`）→ 各自那一轮必然全量回归**。

---

## 0 · 三个层面 → 任务映射

用户的三个诉求与本文档阶段的对应关系：

| 层面       | 核心问题                | 对应阶段 | 任务号       |
| -------- | ------------------- | ---- | --------- |
| **布局范式** | 空间怎么组织？不同工种怎么各得其所？  | 阶段 B | E04 ~ E08 |
| **核心面板** | 每个面板是否达到 3A 水准？     | 阶段 C | E09 ~ E17 |
| **交互理念** | 一切操作是否可发现、可撤销、可自动化？ | 阶段 D | E18 ~ E23 |
| **生产力**  | 有没有"成熟引擎的分水岭"工具（资产编辑器 / 时间轴 / 出包 / 烘焙）？ | 阶段 E | E24 ~ E29 |

**但阶段 A（地基）必须最先做**——不做它，后面 26 个任务全是"每个参数手写一遍"，  
做完一轮就腐化一轮。这是本文档与"直接开面板"路线最大的区别。

---

## 1 · 依赖总览与推荐执行顺序

```
                        ┌──────────────────────────────┐
                        │  阶段 A · 地基（必须最先）    │
                        │  E01 特性注册表 (SSOT)        │
                        │        │                     │
                        │        ├──► E02 UI 生成层     │
                        │        └──► E03 设置层级+落盘 │
                        └──────────┬───────────────────┘
                                   │
        ┌──────────────────────────┼──────────────────────────┐
        ▼                          ▼                          ▼
┌───────────────┐        ┌──────────────────┐        ┌────────────────┐
│ 阶段 B 布局    │        │ 阶段 C 核心面板   │        │ 阶段 D 交互     │
│ E04 面板注册表 │◄──────►│ E09 Engine 重构   │◄──────►│ E18 命令面板    │
│ E05 工作区     │        │ E10 补齐 6 特性   │        │ E19 设置可撤销  │
│ E06 分栏增强   │        │ E11 Inspector     │        │ E20 覆盖指示器  │
│ E07 多视口     │        │ E12 Hierarchy     │        │ E21 通知/进度   │
│ E08 菜单/工具栏│        │ E13 ContentBrowser│        │ E22 Gizmo/选择  │
└───────────────┘        │ E14 Viewport      │        │ E23 快捷键表    │
                         │ E15 Stats         │        └────────────────┘
                         │ E16 Console (新建)│                 │
                         │ E17 Preferences   │                 │
                         └───────────────────┘                 │
                                   │                           │
                                   └─────────────┬─────────────┘
                                                 ▼
                                    ┌────────────────────────┐
                                    │ 阶段 E · 生产力（3A）   │
                                    │ E24 资产编辑器框架      │
                                    │ E25 GPU 时戳时间轴      │
                                    │ E26 画质档位            │
                                    │ E27 RenderGraph 可视化  │
                                    │ E28 打包/发布           │
                                    │ E29 烘焙流水线          │
                                    └────────────────────────┘
```

**如果一次只做一件**（性价比最高的直线序）：

```
E01 ──► E02 ──► E09 ──► E10 ──► E04 ──► E05 ──► E11 ──► E18 ──► E19 ──► E14 ──► E16 ──► E12 ──► E13 ──► E23
```

理由：

- **E01→E02→E09→E10** 是一条闭环，走完即得到"**23 组渲染特性全部有 UI 面**"这个肉眼可见的质变；
- **E04→E05** 让"不同工种各得其所"落地，且是后续所有面板的容器；
- **E11**（Inspector）是使用频次最高的面板，投入产出比最高；
- **E18→E19** 是"可发现性 + 可撤销"两个成熟度硬指标；
- **E14→E16** 把视口与日志这两块"每天看 8 小时"的面板做到位。

---

## 2 · 阶段 A · 地基（E01 ~ E03）

> **为什么必须最先做**：现状 `Renderer` 构造函数里散着 157 个 `MYVK_*` env 读取，  
> `drawEnginePanel()` 手写 500 行，两者**各写各的、谁都没义务覆盖全部字段**（见架构文档 §0.3）。  
> 不先建单一事实源（SSOT），后面每加一个面板都要再手写一遍，**做完即腐化**。

### E01 · 特性注册表 FeatureRegistry（SSOT） ✅ 已完成（2026-10-14）

- **目标**：每个可调项**只登记一次**，成为 env / UI / 序列化 / 工具提示 / 回归触发 的唯一事实源。
- **改动点**
  1. 新建 `engine/render/FeatureRegistry.h/.cpp`：
     ```cpp
     struct ParamDesc {
         const char* id;          // "ssr.strength"（同时是控件稳定 ID 与 env 名派生源）
         const char* label;       // "Strength"
         ParamKind   kind;        // Bool / Int / Float / Enum / Color3 / Color4
         void*       ptr;         // 直接指向现有设置结构体字段（零拷贝）
         float lo, hi, step;      // Float/Int 的滑条范围
         const char* const* enumNames; int enumCount;
         ParamTier   tier;        // Basic / Advanced / Debug
         const char* tooltip;
         const char* envName;     // 显式覆盖（默认由 id 派生：ssr.strength → MYVK_SSR_STRENGTH）
     };

     struct FeatureDesc {
         const char* id;          // "ssr"
         const char* name;        // "Screen Space Reflections"
         const char* roadmapId;   // "R3-1"
         FeatureScope scope;      // Project / Scene / Object / Camera / Editor
         const char* panelPath;   // "Engine/Scene/Reflections"
         const char* gpuSection;  // "ssr"（对应 GpuTimer 段名，用于成本显示）
         const char* testName;    // "verify_ssr"（对应回归用例，用于三向对齐）
         const ParamDesc* params; int paramCount;
     };

     extern const FeatureDesc kFeatures[];
     extern const int kFeatureCount;
     ```
  2. 把**现有 23 组**特性全部登记进 `kFeatures[]`（SSAO / SSGI / SSR / God rays / SSS / POM /  
     AlphaMask / 贴花 / 体积雾 / 解析高度雾 / 天空 / IBL / 阴影级联 / 局部光源阴影 / LOD /  
     TAA / 自动曝光 / 色彩分级 / 泛光 / DOF / 运动模糊 / 暗角 / FXAA…）。
  3. `loadEnvOverrides()`：遍历 `kFeatures[]` × `params[]`，按 `envName` 读环境变量覆盖。  
     **接管 `Renderer` 构造函数里现有的 env 读取**（逐个搬迁，搬一个删一处）。
  4. `resetToDefaults()` / `serialize(json)` / `deserialize(json)`：按注册表递归。
- **涉及文件**：`engine/render/FeatureRegistry.h/.cpp`（新建）、`engine/render/Renderer.cpp`（搬迁 env 读取）、  
  `engine/render/ScreenSpaceSettings.h`、`RenderTypes.h`、`CMakeLists.txt`
- **验收**
  - `CULL-STATS` / `GPU-TIMING` 等既有日志**逐字不变**（证明搬迁是纯等价变换）；
  - 现有全量回归（当时 39 例）**全部 PASS**（env 通路必须与搬迁前完全等价）——**这是本任务唯一的硬门槛**；
  - `check_feature_surface.py` 的"裸 env 漏网"仍为 0。
- **规模**：**L**（数据搬运量大，但机械）
- **依赖**：无

### E02 · UI 生成层（ParamDesc → 控件） ✅ 已完成（2026-10-14）

- **目标**：一个 `ParamDesc` 自动得到"正确的控件 + 稳定 ID + 工具提示 + tier 折叠 + 覆盖指示"。
- **改动点**
  1. `editor/FeatureUIGen.h/.cpp`：`drawParamUI(const ParamDesc&, const char* featureId)`。  
     类型映射：`Bool→Checkbox` / `Float→SliderFloat` / `Int→SliderInt` / `Enum→Combo` /  
     `Color3→ColorEdit3` / `Color4→ColorEdit4`。
  2. **控件 ID 规范**：`PushID(featureId)` + `##<paramId>` → 稳定 ID `###ssr.strength`。  
     （沿用项目既有 `###id` 惯例，见架构文档 §8.1。）
  3. **tier 折叠**：面板顶部三档开关 `Basic | Advanced | Debug`，跨面板持久化。  
     `Debug` 档的项加醒目色点。**加载 `render.json` 时若某档有非默认值，自动升到最低可见档**  
     （避免"我明明调过但看不见"）。
  4. **搜索框**：按 label / id / envName 模糊过滤，命中即展开所在分区并高亮。
  5. `drawFeatureUI(featureId)`：画一个特性的完整分区（标题 + 启用开关 + 参数 + 成本标签 + 复位按钮）。
- **涉及文件**：`editor/FeatureUIGen.h/.cpp`（新建）、`editor/EditorApp.cpp`（接入）
- **验收**
  - `MYVK_LOG_RECTS=1` 能打印出 `###ssr.strength` 的矩形（证明 ID 稳定）；
  - 新增 `tools/verify_feature_ui.py`：用 env 预置 → 打开面板 → 断言控件存在且能改值  
    （基于 `LogTap` + 注入点击，见 E-自动化）。
- **规模**：**M**
- **依赖**：E01

### E03 · 设置层级与持久化 ✅ 已完成（2026-10-17）

- **目标**：渲染设置在 **5 个作用域**里可覆盖、可继承、可落盘。
- **改动点**
  1. 作用域：`Editor Preferences`（跨项目）→ `Project`（`config/render.json`）→ `Scene`  
     （场景文件内）→ `Object`（实体组件）→ `Camera`（相机组件）。读时**从最具体往最泛回退**。
  2. 落盘：`config/render.json`（键 = `"<featureId>.<paramId>"`，值 = 标量/数组）。  
     缺失文件 → 全部走引擎默认（**与今天逐像素一致**，见 §9 硬约束）。
  3. 首次运行的 `render.json` **不自动生成**（保持"无文件 = 无覆盖"的语义，避免污染回归基线）。  
     由用户首次改参数时才写入，或在 Preferences 里点"导出当前设置"。
- **涉及文件**：`engine/render/RenderSettingsIO.h/.cpp`（新建）、`config/`（新建目录）、  
  `editor/EditorApp.cpp`
- **验收**
  - 无 `config/render.json` 时，全量回归（当时 39 例）**全部 PASS**（零行为变化）；
  - 写入合法 json → 重启后参数保持；写入非法 json → 报 WARN 并回落默认（不崩溃）。
- **规模**：**M**
- **依赖**：E01

#### 交付记录（2026-10-17）与计划的三处偏差

1. **`FeatureRegistry` 多暴露了一层"绑定视图"**（`BindingView` /
   `takeUnappliedBindings` / `collectBindings` / `resetSettingsApplied`）。
   **为什么非加不可**：同一个参数可能既被 `config/render.json` 覆盖、又被
   `MYVK_*` 覆盖，而 **env 是测试钩子，必须永远赢**。所以每个绑定阶段
   的顺序被钉死成 `applyToRegistry() → loadEnvOverrides()`，两边各记各的
   "处理过没有"（`settingsApplied` / `applied` 两个独立标记）。
   只加一个标记会让第二个绑定阶段（`createMainTarget` 之后那批 post 参数）
   把第一批的 env 值盖回去。
2. **校验与 env 完全一致，不额外按 UI 范围夹紧**。已经踩过：`lshadow.bias`
   的 UI 范围是 `0~1`，而默认 z 分量是 `1.2` —— 按 UI 夹会让"导出→重载"
   一次就把默认值改坏。所以文件层只做 env 那一套（非有限值拒绝 /
   `minAccept,maxAccept` / `clampLo,clampHi`）。
3. **UI 入口放在 Engine 面板的顶部**（`drawEngineSettingsIO`：
   `Export current` / `Reload from disk` + 覆盖计数）。原本写在生成区
   **之后**（三十多个特性块后面），实测按钮被挤到 `y=3079`（窗口只有 900 高）
   —— **点不到 = 不算交付**。`verify_settings_layers` 的 I2 就是这条断言
   （否定测试：挪回底部立刻变红）。

另外两件计划里没写但必须做的事：

- **`engine/render/RenderSettingsIO.*` 进了 `RENDER_MODULES`**（否则
  `check_regression_map.py` 的 F 组会报"没被覆盖的 render 文件"）。
- **`config/README.md` + `config/render.example.json`**：示例文件被
  `verify_settings_layers` 的 J 组当作**输入**校验（每个键都必须真生效）——
  文档里的例子不能是假的。

**Scene / Object / Camera 三层只提供了数据通路，没有磁盘入口**：
`SettingsChain::setLayer` / `parseLayerText` 已经能接收它们（五层优先级
由自检 B1 逐层验证），但"场景文件内嵌块"与"实体/相机组件"的 schema
改动属于 E20（覆盖指示器）那一轮的事 —— 动 `SceneSerializer` 会牵到
所有场景夹具。

**验收对照**：

| 条款 | 证据 |
| --- | --- |
| 无 `render.json` → 零行为变化 | `verify_settings_layers` B1（**一条 `RENDER-SET` 日志都没有**）+ B2（`SHADOW-DBG cascades=4`） |
| 合法 json → 重启后保持 | C1/C2/C3（`applied=shadow.cascades value=2 from=project` → `cascades=2`）+ 自检 E1/E2/E3（落盘往返**逐位相同**） |
| 非法 json → WARN + 回落默认 + 不崩 | D1/D2/D3/D4 |
| 文件层不是影子结构体 | C4：同一份 json 与 `MYVK_SHADOW_CASCADES=2` 的 `splits=` **逐字符相同** |

⚠ **"改参数 → 自动落盘"这一段没有端到端用例**，因为自动化运行**禁止写**
（与 E07 布局/偏好同一条规则：`MYVK_FRAMES` / `MYVK_NO_WINDOW_SAVE` 存在
即为自动化）。覆盖方式是拆开验：写的一半靠自检的 `saveLayerFile` 往返，
读的一半靠 C 组。这是**已知的覆盖缺口**，不是"测过了"。

---

## 3 · 阶段 B · 布局范式（E04 ~ E08）

> 3A 引擎的布局具备四个特征：**面板可停靠可浮动** / **工作区可切换** / **视口可分裂** / **布局可序列化**。  
> 本项目已有 `SplitLayout`（比例 + 拖拽 + 折叠）与 `imgui.ini` 浮窗持久化，**地基够用、但要补齐上面四点**。

### E04 · PanelRegistry（面板表驱动） ✅ 已完成（2026-10-14）

- **目标**：新增一个面板**只加一行**，View 菜单 / 快捷键 / 图标 / 持久化四路自动派生。
- **改动点**
  1. 把 `EditorContext::PanelVisibility`（一堆 `bool hierarchy = true;`）升级为：
     ```cpp
     struct PanelDesc {
         const char* id;            // "hierarchy"
         const char* title;         // "Hierarchy"
         const char* menuPath;      // "Window/General/Hierarchy"
         const char* shortcut;      // "Ctrl+1"
         const char* icon;          // EditorIcons.h 字形
         bool  defaultOpen;
         bool  floating;
         void (*draw)(EditorApp&);
     };
     extern const PanelDesc kPanels[]; extern const int kPanelCount;
     ```
  2. `drawMenuBar()` 的 Window 菜单**从表生成**（现在的硬编码项删掉）。
  3. 快捷键**从表注册**（配合 E23）。
  4. 面板显隐状态改存 `std::vector<uint8_t>`（按 `id` 索引），落盘到 `config/editor_layout.json`。
- **涉及文件**：`editor/PanelRegistry.h/.cpp`（新建）、`editor/EditorContext.h/.cpp`、  
  `editor/Toolbar.cpp`、`editor/EditorApp.cpp`
- **验收**：新增一个空面板（只加一行表项）→ View 菜单出现它、快捷键生效、重启后显隐保持。
- **规模**：**M**
- **依赖**：无（可与 E01 并行）

### E05 · Workspace 工作区 ✅ 已完成（2026-10-14）

- **目标**：不同工种一键切到"自己的"面板组合与比例。
- **改动点**
  1. 六套工作区（见架构文档 §6.2）：**Level**（默认）/ **Rendering** / **Character** /  
     **Animation** / **Cinematic** / **Debug**。
  2. 每套 = `{ 面板显隐集, SplitLayout 比例, 视口布局, 当前视图模式 }`。  
     用现有 `SplitLayout` 保存比例（已有分栏树）。`EditorContext::relayout()` 现有的  
     "本帧用 Always"机制正好用于切换工作区。
  3. `Window ▾` 菜单切工作区；`Ctrl+Alt+1..6` 快捷切换；标题栏显示当前工作区名。
  4. 用户拖动后的自定义布局**自动成为"当前工作区的修改"**，可 `Save Workspace` 覆盖或  
     `Reset Workspace` 还原出厂。
- **涉及文件**：`editor/Workspace.h/.cpp`（新建）、`editor/SplitLayout.cpp`、`EditorApp.cpp`
- **验收**：切到 Rendering 工作区 → 断言 `MYVK_LOG_RECTS` 里 Engine 面板可见、Hierarchy 不可见；  
  切回 Level → 复原。
- **规模**：**M**
- **依赖**：E04

### E06 · 分栏（SplitLayout）增强 ✅ 已完成（2026-10-14）

- **目标**：把"能用"的分栏提升到"好用"。
- **改动点**
  1. **分割线可视化**：悬停高亮、拖动时显示当前比例百分比 tooltip。
  2. **双击分割线 = 折叠/展开**该侧（当前只有单侧折叠按钮）。
  3. **面板最大化**：`Shift+Space` 让鼠标所在面板临时占满工作区（再按还原）——调参时极常用。
  4. **比例预设**：`50/50`、`70/30`、`30/70` 快选（右键分割线弹菜单）。
  5. **最小宽度约束**：每个面板设 `minW`，避免拖到 0 宽后找不回来（现有是硬编码常量）。
- **涉及文件**：`editor/SplitLayout.h/.cpp`（277 行，主战场）
- **验收**：`MYVK_LOG_RECTS` 能打印分割线矩形；拖动后比例落在合法区间；最大化/还原后比例不变。
- **规模**：**S**
- **依赖**：无

### E07 · 多视口（Viewport Splitting）🟡 部分完成（2026-10-14 / 详见 E14 落地记录）

- **目标**：一次看最多 4 个视角（透视 / 顶视 / 前视 / 相机视角），对标 UE 的 4-View。
- **改动点**
  1. 视口布局枚举：`Single / TwoVertical / TwoHorizontal / Quad`。工具栏加布局切换按钮。
  2. **每个子视口独立的**相机（位置/朝向/fov）、视图模式、是否显示 gizmo、是否显示统计。
  3. 只有一个"活动视口"接受输入（点击即激活，边框高亮）；其余只渲染。
  4. 实现要点：`ViewportPanel` 现在把视口矩形存 `EditorContext::layout()`；需要改成  
     子视口矩形数组，并给 `Renderer` 传**多个 view/proj**（或分多次 pass 渲染到 HDR 的子区域）。
  5. **性能保护**：多视口默认**降级**（子视口关闭 TAA / 用 1/2 分辨率），活动视口全质量。
- **涉及文件**：`editor/ViewportPanel.h/.cpp`、`editor/EditorContext.h`、`engine/render/Renderer.cpp`
- **验收**：Quad 布局下 `MYVK_LOG_RECTS` 打出 4 个子视口矩形且互不重叠；  
  点击切换活动视口后 gizmo 跟随；`GPU-TIMING` 总量增长在预算内。
- **规模**：**L**
- **依赖**：E04（工作区要能记住视口布局）


### E08 · 菜单栏与工具栏重构 ✅ 已完成（2026-10-14）

- **目标**：菜单覆盖"一个编辑器该有的全部入口"；工具栏从"一排按钮"变成"可配置的条"。
- **改动点**
  1. 菜单栏补全：
     - `File`：New / Open / **Open Recent ▸**（最近 10 个场景）/ Save / Save As / **Import ▸** / Export ▸ / Exit
     - `Edit`：Undo / Redo / **History ▸**（撤销栈可视化）/ Cut / Copy / Paste / Duplicate / Delete /  
       **Select All / Select None / Invert** / **Project Settings ▸** / **Editor Preferences ▸**
     - `View`：**View Mode ▸**（12 种）/ **Panel ▸**（从 E04 生成）/ **Workspace ▸**（E05）/  
       Fullscreen / **Reset Layout**
     - `Build`（新）：**Reload Shaders** / Reload Assets / **Package ▸**（E28）
     - `Window`：面板显隐 + 工作区
     - `Help`：**Shortcut Reference**（E23）/ API Docs / **About**（版本 + GPU 型号 + 后端）
  2. 工具栏：分成可折叠的**工具组**（Transform / Snapping / Play / View / Build），  
     每组可显隐（右键菜单），状态落盘。按钮全部带 tooltip 且显示快捷键。
  3. **播放控制条（Transport）** 扩展：`Play/Pause/Stop` 已有，补  
     **时间轴拖拽条 + 当前帧号 + 帧率倍率（0.25× / 1× / 2×）+ 单帧步进**。
- **涉及文件**：`editor/Toolbar.h/.cpp`（356 行）、`editor/EditorApp.cpp`（`drawTransportBar`）
- **验收**：`Shortcut Reference` 打开后断言快捷键表非空；`Open Recent` 有内容；  
  `About` 能读出 GPU 型号字符串。
- **规模**：**M**
- **依赖**：E04、E05

---

## 4 · 阶段 C · 核心面板（E09 ~ E17）

> 这一阶段是"**每一个功能面板**"的升级。原则：**能用生成器的用生成器，必须手写的才手写**  
> （曲线 / 节点图 / 浏览器这类交互复杂的，手写更划算——见架构文档附录 B.4）。

### E09 · Engine 面板重构（手写 500 行 → 注册表驱动） ✅ 已完成（2026-10-14）

- **目标**：`drawEnginePanel()` 从 500 行手写缩到 < 80 行，且**自动覆盖全部 23 组特性**。
- **改动点**
  1. 结构化为**分区树**：`Scene / Lighting / Atmosphere / Shadows / Reflections / PostProcess /
     Rendering / Debug`，每个分区 = 若干 `FeatureDesc`。
  2. 每特性渲染：标题行（启用开关 + 名称 + **roadmapId 徽标** + **GPU 成本**）+  
     参数区（`drawFeatureUI`）+ 右上角"复位"小按钮。
  3. **顶部三档**（Basic/Advanced/Debug，来自 E02）+ **搜索框**（跨分区过滤）。
  4. **成本显示**：从 `GpuTimer` 读该特性的 `gpuSection` 耗时，显示为  
     `SSR · 0.42 ms`。这是"性能意识"从面板就可见（对标 UE 的 GPU Profiler 徽标）。
  5. 面板顶部一条 **`Preset ▾`**：`Low / Medium / High / Epic / Cinematic` + `Custom`（E26 提供数据）。
- **涉及文件**：`editor/EditorApp.cpp`（`drawEnginePanel` 1573~2065 行，删掉重写）、  
  `editor/FeatureUIGen.cpp`
- **验收**
  - `check_feature_surface.py` 的 C 组（无 UI 面特性）**清零**；
  - `drawEnginePanel` 行数 < 100（客观指标）；
  - 该面板内所有控件 ID 唯一且稳定（`MYVK_LOG_RECTS` 无重复 `###`）。
- **规模**：**M**
- **依赖**：E01、E02

### E10 · 补齐 6 个"完全无 UI 入口"的特性 ✅ 已完成（2026-10-14）

- **目标**：把架构文档 §0.2 实测出的 6 组"只有 env、编辑器点不到"的特性补齐。
- **清单**（E09 完成后，这些**天然被生成器覆盖**，本任务只负责核对与补默认值/tooltip）：
  | 特性                                                    | 作用域    | 分区                          | Basic 档必须有            |
  | ----------------------------------------------------- | ------ | --------------------------- | --------------------- |
  | SSAO                                                  | Scene  | Engine ▸ Scene/Ambient      | `Enable`              |
  | SSGI                                                  | Scene  | Engine ▸ Scene/Global Illum | `Enable`              |
  | SSR                                                   | Scene  | Engine ▸ Scene/Reflections  | `Enable` + `Strength` |
  | God rays                                              | Scene  | Engine ▸ Scene/Atmosphere   | `Enable`              |
  | SSS                                                   | Object | Inspector ▸ 材质              | `Subsurface` 开关 + 颜色  |
  | 阴影级联硬化                                                | Scene  | Engine ▸ Shadows            | 级联数 + 最大距离            |
  | 另有 5 组"半个入口"需要补全：**Froxel 雾**（8 参暴露 2）、**贴花**（缺全局开关）、 |        |                             |                       |
  | **天空盒**（只有开关）、**IBL**（只有开关）、**LOD**（只有开关）。            |        |                             |                       |
- **涉及文件**：`engine/render/FeatureRegistry.cpp`（登记）、`editor/InspectorPanel.cpp`（SSS 那项）
- **验收**：`check_feature_surface.py` **C/D 组全绿**，退出码 0。  
  **这是本阶段最有说服力的一个验收信号**——它同时证明了生成器有效和欠账清零。
- **规模**：**S**（E09 完成后基本是登记工作）
- **依赖**：E09

### E11 · Inspector 升级（使用频次最高的面板） ✅ 已完成（2026-10-15）

- **目标**：从"属性列表"升级为"**分区折叠 + 多选编辑 + 组件搜索 + 覆盖指示**"的成熟检视器。
- **改动点**
  1. **分区折叠 + 记忆**：Transform / Visibility / Mesh / Material / Collision / Decal / Lights /  
     Script，每区可折叠且折叠状态**按分区名持久化**（换实体不重置）。
  2. **组件搜索**：顶部输入框，过滤**所有分区与其字段**（大项目里属性几十项，靠找不可行）。
  3. **多选编辑**：选中多个实体时，显示**公共组件**；编辑时同步应用到全部（值不同则显示 `—`）。  
     这也是"批量改参数"的唯一可行手段。
  4. **右键菜单**：`Copy Properties` / `Paste Properties` / `Reset to Default` / `Remove Component`。
  5. **字段级拖拽**：所有 `float` 字段行支持**拖拽微调**（ImGui 原生），并支持  
     **拖到另一个字段上做绑定**（如把 Base Color 拖到 Emissive 上）。
  6. **覆盖指示器**（与 E20 共用）：字段左侧 `●` 本地覆盖 / `○` 继承 / `◇` 默认；  
     右键 `Reset to inherited`。
  7. `Shading Model` / `Alpha Mode` 等枚举项加**图标与说明 tooltip**（现在只有文字）。
- **涉及文件**：`editor/InspectorPanel.cpp`（1785 行）、`InspectorPanel.h`
- **验收**：新增 `tools/verify_inspector.py` —— 断言分区折叠状态跨实体保持、  
  多选编辑同步到 N 个实体、Copy/Paste Properties 往返一致。
- **规模**：**L**
- **依赖**：E20（覆盖指示器）、E03（层级）

#### E11.1 · Inspector 加"左侧图标导航条"（页签化） ✅ 已完成（2026-10-16）

- **动机**：E11 把分区做成了可折叠，但**仍然是一条滚动条堆到底** —— 选中一个
  球之后要滚过 Transform / Visibility / Mesh / Material / Collision / Decal /
  Reflection Probe / Particle Emitter / Light / Script 才找得到要调的那一项。
  用户提出照 Blender 属性编辑器的组织方式改造：**左列一竖排图标 + 右侧只显示
  当前页的内容**。
- **改动点**
  1. `editor/InspectorIcons.h`（新建）：`IconPen`（归一化坐标 → 屏幕坐标的
     小笔）+ 11 个矢量图标（cube / wrench / camera / export / photo / globe /
     sparkle / ball+gravity / sphere / curve / list）。颜色**由调用方给**
     （常态 TextDisabled、悬停/选中 Text）—— 与 `EditorIcons.h` 的"资产图标
     颜色写死"是两套契约，头注释里写清了分工。
  2. `InspectorPanel::InspTab` 枚举 + `kTabs[]` 表（**顺序必须一致**，有
     `static_assert` 兜项数、`All` 必须在末尾）+ `drawNavBar()`（
     `InvisibleButton` + 自绘圆角高亮 + 左侧指示条 + tooltip + 每项
     `logRect("INS-NAV <id>")`）。
  3. 页签 → 分区映射：Object=Transform/Visibility/Mesh；Tool=Script/Add
     Component；Render=Light/Decal/Probe；Output=只读信息（引擎暂无输出设置，
     不编假控件）；Scene=统计；World=相机/主光；Particles；Physics；Material；
     Animation=Skinned Mesh；**All**=旧的堆叠视图。
  4. **没选中物体时不再整页退出**：World / Scene / Output 与"选中谁"无关，
     仍然可用；其余页签画一行"先选个物体"（并发 `INS-TAB-EMPTY` 日志锚点）。
  5. 搜索词非空时**忽略页签选择、跨全部页签摊平命中项**（与 Preferences 窗口
     的搜索行为对齐）。
  6. 选中页签落盘（`LayoutState::inspTab`，**存 id 字符串不存下标** ——
     往中间插一个页签不会让老配置跳到别的页上）。
  7. 新增环境变量 `MYVK_EDITOR_INSP_TAB=<名字|下标>`。
- **⚠ 兼容性决定**：**自动化运行（`MYVK_FRAMES` / `MYVK_NO_WINDOW_SAVE`）下
  默认页签 = `all`**。项目里已有 9 个验证脚本靠 `INS-MESH-HEADER` /
  `INS-COLLISION-HEADER` / `INS-PROBE-HEADER` / `INS-FX-EXPAND-ALL` 这些矩形
  锚点断言"那个分区画出来了"；分区一旦按页签分散，不指定页签就只看得到
  Object 页 → 那 9 个脚本会**整批误报**。默认"全部"一次性保住它们，
  想单测某一页再用环境变量指定。交互时默认仍是 Object 页。
- **涉及文件**：`editor/InspectorIcons.h`（新建）、`editor/InspectorPanel.{h,cpp}`、
  `editor/EditorLayoutIO.{h,cpp}`、`editor/EditorApp.cpp`
- **验收**：`tools/verify_inspector_tabs.py`（25 项断言，5 组：导航条结构 /
  页签隔离正负对照 / 无选中行为 / 搜索跨页签 / 像素）。**否定测试**：把页签
  派发改成"永远画全部"重生后，10 条隔离断言**全部变红**并报出"多了/缺了
  哪些锚点" → 证明它们不是空断言。
- **规模**：M
- **坑（写给后来人）**：`INS-MESH/COLLISION/PROBE-HEADER` 记在
  `sectionHeader()` 之后、`if (!open) return;` **之前** —— 它们必须"折叠着
  也在"，否则脚本点不到展开。代价是**被搜索词过滤掉时那几行日志照样会打**
  （"函数被调用了" ≠ "分区可见"）。想判断"某分区可见"只能用记在提前返回
  **之后**的锚点（`INS-MATERIAL-HEADER` / `INS-OUTPUT-HEADER`）。

### E12 · Hierarchy 升级 ✅ 已完成（2026-10-14）

- **目标**：从"树"升级为"可搜索、可拖拽、带状态列"的场景管理器。
- **改动点**
  1. **搜索 / 过滤**：按名字模糊匹配（保留祖先链）；`type:` / `tag:` 前缀语法。
  2. **类型图标**：Mesh / Light / Camera / Decal / Collider / Script / Group 各有图标（`EditorIcons.h`）。
  3. **拖拽**：拖到另一项上 = 改父子；拖到项之间 = 排序（现在只有选择/重命名）。
  4. **状态列**（3A 标配）：每行右侧 `👁` 可见 / `🔒` 锁定 / `☑` 可选。  
     锁定 = 视口不可拾取（E22 依赖它）。
  5. **面包屑**：选中深层对象时，顶部显示 `Root / Level / Props / Crate` 可点击跳转。
  6. **右键菜单**补全：`Focus` / `Rename`（F2 已有）/ `Duplicate` / `Delete` /  
     `Create Child ▸` / `Save as Prefab`。
  7. 多选高亮 + `Ctrl` 加选 / `Shift` 连选（当前选择逻辑在 `EditorContext`）。
- **涉及文件**：`editor/SceneHierarchy.cpp`（658 行）、`editor/EditorContext.h`
- **验收**：新增 `tools/verify_hierarchy.py` —— 搜索过滤命中数、拖拽改父子后场景树深度变化、  
  锁定后视口拾取不命中。
- **规模**：**M**
- **依赖**：E22（锁定↔拾取联动）

### E13 · ContentBrowser 升级 ✅ 已完成（2026-10-15）

- **目标**：从"文件网格"升级为"可拖拽、可过滤、有缩略图的资产浏览器"。
- **改动点**
  1. **缩略图尺寸滑条**（列表/中/大/超大四档）+ **列表/网格视图切换**。
  2. **面包屑 + 前进/后退/上级**（现在有 `drawNavRow`，补齐历史栈）。
  3. **类型过滤**：`All / Model / Texture / Material / Scene / Script / Shader`，按扩展名映射。
  4. **拖放全链路**（关键）：拖资产到
     - **Viewport** → 在鼠标射线与地面交点生成实体（模型/材质即时套用）；
     - **Hierarchy** → 生成实体；
     - **Inspector 贴图槽** → 换贴图（`drawTextureChip` 已有槽位，接上 drop target）。
  5. **文件操作**：右键 `Rename` / `Duplicate` / `Delete` / `Reveal in Explorer` /  
     `Show References`（谁引用了它——需要资产引用扫描，可先做简版）。
  6. **新建菜单**：`New ▸ Material / Scene / Script / Folder`。
  7. **异步缩略图**：`ThumbnailCache`（102 行）扩展为**后台线程 + LRU**，滚动不卡。
- **涉及文件**：`editor/ContentBrowser.cpp`（1087 行）、`ThumbnailCache.cpp`、`EditorDragDrop.h`
- **验收**：新增 `tools/verify_content_browser.py` —— 拖资产到视口后实体数 +1；  
  类型过滤切换后可见项数量变化正确。
- **规模**：**L**
- **依赖**：无（但拖放到 Inspector 依赖 E11）

### E14 · Viewport 升级（每天看 8 小时的面板） ✅ 已完成（2026-10-15）

- **目标**：浮层信息**可配置、不遮挡**，视图模式一键切，相机可存书签。
- **改动点**
  1. **浮层布局重构**：现有控件（gizmo 模式 / 空间 / 吸附 / grid / HUD / Focus / nav gizmo）  
     挤在固定位置，改为**左上=视图模式条 / 右上=统计 HUD / 左下=相机信息 / 右下=导航 gizmo**，  
     每块可单独显隐（右键菜单），状态落盘。
  2. **视图模式条**（12 种，来自 R0-2）：从 `Combo` 改为**图标快捷条 + 数字键 1~9 切换**，  
     当前模式名常驻显示。
  3. **统计 HUD 可配置**：`FPS / 帧时间 / drawCalls / 三角形 / VRAM / 分辨率 / 视图模式`  
     逐项开关（现在是一整块）。
  4. **相机书签**：`Ctrl+数字` 存当前视角、数字键跳转（关卡美术的刚需）。
  5. **视口设置**：网格间距（1m/0.5m/0.1m）、单位显示、背景（天空/纯色/环境球）、  
     FOV 可视角度。
  6. **无 HUD 模式**（`MYVK_EDITOR_NO_HUD`）下浮层全关——**回归基线的硬要求**，不能破坏。
- **涉及文件**：`editor/ViewportPanel.cpp`（1652 行）、`ViewportPanel.h`、`editor/EditorApp.cpp`
- **验收**：`MYVK_EDITOR_NO_HUD=1` 时视口像素与**改造前逐像素一致**（A 组回归）；  
  视图模式条能切到全部 12 种；相机书签存取往返一致。
- **规模**：**L**
- **依赖**：E04（浮层显隐落盘）

### E15 · Stats 面板升级 ✅ 已完成（2026-10-14）

- **目标**：从"一列数字"升级为"**CPU/GPU 分栏 + 时戳条形图 + 历史曲线**"的性能面板。
- **改动点**
  1. **CPU / GPU 分栏**：CPU 侧（帧时间拆解、剔除、场景统计）/ GPU 侧（时戳分段）。
  2. **时戳条形图**：`GpuTimer` 的 10 段（含 `kDecals`）画成**横向堆叠条**，  
     一眼看出哪个 pass 最贵；悬停显示精确 ms（R0-1 的数据已就绪，只是没有可视化）。
  3. **历史曲线**：`FPS / 帧时间` 画 120 帧滚动曲线（现在只有瞬时值）。
  4. **计数器可钉住**：任意指标可"钉"到视口 HUD 或状态栏。
  5. **告警**：帧时间超阈值 / VRAM 超预算 → 高亮（配合 E21 通知）。
- **涉及文件**：`editor/EditorApp.cpp`（`drawStatsPanel` 1227~1489 行）
- **验收**：`GPU-TIMING` 日志与条形图数值一致；曲线缓冲长度正确。
- **规模**：**M**
- **依赖**：无

### E16 · Console 面板（新建） ✅ 已完成（2026-10-14）

- **目标**：3A 引擎必备的**日志 + 命令**二合一面板。现在日志只进 stdout/文件，  
  用户**看不到**自己的 WARN/ERROR。
- **改动点**
  1. **日志汇入**：把 `VK_LOG_INFO/WARN/ERROR` 同时投递到内存环形缓冲（保留最近 N 条），  
     面板内显示；**级别过滤**（Info/Warn/Error）+ **关键字搜索** + **自动滚动** +  
     `Clear` / `Copy` / `Save to file`。
  2. **命令输入**（Unreal 风格）：`r.SSR 1` / `exposure 1.5` / `vw.Decal 1` / `stat gpu`。  
     命令 = `{name, help, handler}`，**表驱动**，与 E18 命令面板**共用同一个注册表**。
  3. **自动补全**：输入时列出候选命令 + 参数提示（Tab 补全）。
  4. 与 `--explain` / 回归工具共用输出格式（便于自动化断言）。
- **涉及文件**：`editor/ConsolePanel.h/.cpp`（新建）、`engine/core/Log.h`（加 sink 回调）、  
  `engine/render/Renderer.cpp`（注册渲染命令）
- **验收**：新增 `tools/verify_console.py` —— 注入 `r.SSR 1` 命令后日志出现 SSR 开启；  
  注入一条 WARN 后 Console 能过滤出它。
- **规模**：**M**
- **依赖**：E01（命令要能读写注册表）

### E17 · Preferences 升级 ✅ 已完成（2026-10-14）

- **目标**：从"一列设置"升级为"**分类导航 + 搜索 + 单项重置 + 导入导出**"。
- **改动点**
  1. **左侧分类树**：`General / Appearance / Viewport / Input / Rendering / Assets / Scripting /
     Experimental`。右侧详情区。
  2. **搜索**：跨分类过滤设置项。
  3. **单项重置**：每个设置行有小复位按钮；顶部 `Reset All`（带二次确认）。
  4. **导入 / 导出**：设置存为 `config/editor_prefs.json`，可导出分享。
  5. **前后对比**：改渲染相关偏好时显示"影响哪些面板/重启是否生效"提示  
     （有些设置需要重建管线，必须明确告知）。
- **涉及文件**：`editor/EditorApp.cpp`（`drawSettingsPanel` 2065~2300 行）
- **验收**：搜索"font"能命中字号设置；Reset All 后偏好回到默认且落盘文件同步。
- **规模**：**M**
- **依赖**：E01（渲染类偏好来自注册表）

---

### 阶段 C 落地记录（2026-10-14 ~ 2026-10-15）

阶段 C 的 9 项全部落地，**每项都有自己的 `verify_*.py` 且 PASS**（里程碑 M3 的判定信号）。
按"交付形态 → 机器可验证的锚点"列一下：

| 任务 | 交付形态（关键实现） | 验收锚点（机器可判） |
| --- | --- | --- |
| E09 | `drawEnginePanel` 改为**注册表驱动**：分区树（Scene/Lighting/Atmosphere/Shadows/Reflections/PostProcess/Rendering/Debug）+ 每特性标题行（开关 + roadmapId 徽标 + GPU 成本）+ 参数区 + 复位按钮；顶部 Basic/Advanced/Debug 三档 + 跨分区搜索 | `check_feature_surface.py` 退出码 0；面板内控件 ID 唯一（`###<featureId>.<paramId>`） |
| E10 | 6 组"只有 env、点不到"的特性全部登记进 `FeatureRegistry`（SSAO/SSGI/SSR/God rays/SSS/级联硬化）+ 5 组"半个入口"补全（雾 / 贴花 / 天空盒 / IBL / LOD） | `check_feature_surface.py` **C/D 组全绿**（本轮实测：C 组 0、纯 env 漏网 0） |
| E11 | Inspector：分区**按名折叠**并跨实体保持 / 组件搜索（分区名 + 字段关键词）/ 右键 Copy·Paste·Reset（POD 字节快照，写入走 `RawBytesEditCommand` 可撤销）/ Shift 微调 | `tools/verify_inspector.py`：`insp.selftest` 往返 OK + 折叠状态跨三实体一致 + `inten` 只留 Light 分区 |
| E12 | Hierarchy：名字模糊过滤（**保留祖先链**）+ `type:xxx` 前缀语法 / 类型图标 / 拖资产到树=实例化 / 每行状态列 👁🔒☑（走 pending 队列，树画完统一经 `structuralEdit` 落库） | 冒烟 0 WARN/ERROR/VUID；`type:light` 命中 3 光源且祖先灰显保留；`HIER-IMPORT rel=... count=N` |
| E13 | ContentBrowser：类型过滤矩阵 / 列表+四档缩略图 / 三级降级面包屑 / 前进后退历史栈 / 拖资产到视口（贴地）与 Hierarchy（实例化） | `tools/verify_content_browser.py`：`All == Models + Folders` 等 10 条过滤矩阵 + 两条落点断言全 PASS |
| E14 | Viewport：四角浮层（左上模式条 / 右上统计 HUD 10 行逐项开关 / 左下相机信息 + **9 个书签** / 右下导航球）；两层显隐（总闸 + 四块各自开关）；Shift 层快捷键；Viewport Display 右键菜单；状态随偏好落盘 | `tools/verify_viewport.py`（见下） |
| E15 | Stats 面板：分栏 + 条形图 + 曲线 | 归入 `verify_gpu_timing` / 面板矩形断言 |
| E16 | Console 面板（新建）：日志汇入 + 级别过滤 + 命令行（`set/get/reset/params/preset/stat/insp.selftest`） | `CONSOLE-DUMP lines=.. info=.. warn=.. err=..` 锚点 |
| E17 | Preferences：分类树 + 搜索 + 重置 + 导入导出；`config/editor_prefs.json` 落盘 | 见 E14 的 V7 断言组（同一套落盘机制） |

#### E14 的三条硬约束与取证

1. **`MYVK_EDITOR_NO_HUD=1` 必须与改造前逐像素一致**（A 组回归的硬要求）。
   取证：整窗（1578×844，界面上所有可见部分都算上）与改造前参考图
   `tools/vp_nohud_baseline.png` 逐像素比对，**差异像素 0**；同参数两次抓图也 0。
   ⚠ **不要用日志里的 `VP-RECT` 去裁图**：`logRect` 按 tag 去抖（同 tag 只在
   变化 >4px 时重打），而视口首帧布局用的是"启动那一刻的窗口尺寸"——
   resize 与首帧的先后不同会留下不同首值（同一份二进制实测能拿到
   `(310,184)-(1238,776)` 与 `(310,184)-(1260,832)` 两种），
   早期那张 `_venn_verify/e14/nohud_baseline.png` 就是按偏高的首值裁的。
   现在一律用**全窗口**图比对，或视口内部那块稳定锚点 `VP_INNER=(330,200,1200,750)`。
2. **四个浮层开关全关 == NO_HUD**（总闸之外每一块都得把绘制关干净）。
   取证：走 `MYVK_EDITOR_PREFS_PATH` 给一份四开关全 0 的偏好 → 视口内部
   与 NO_HUD 逐像素 0 差异。（整窗会有 1560 像素差异，来源是工具栏那个
   "HUD" 勾选框的高亮色 —— 总闸关 vs 开，与浮层无关。）
3. **`Shift+N` 切的是模式索引 `N-1`**，不是"第 N 个按钮"：Shift+2 → unlit、
   Shift+5 → metallic、Shift+6 → wireframe。数字键本体（1/3/7 视图方向、
   Alt+N 面板显隐）已被占满，所以三件事全挂 Shift 层：
   `Shift+1..8` 切模式 / `Ctrl+Shift+N` 存书签 / `Alt+Shift+N` 跳书签。

#### 阶段 C 收口：全量回归 3 例失败的定性（2026-10-15）

一轮全量（42 例）跑出 3 例红：`verify_bvh` / `verify_csm` / `verify_ibl`。
逐条查证后**全部是验证侧（harness）问题，不是渲染/拾取回归** —— 证据与修法：

1. **`verify_csm` / `verify_ibl`：抓到了空白帧。**
   两例的**数值组断言全 PASS**（CSM 的分割点逐项等于解析公式、级联裁剪矩阵、
   距离分级单调性；IBL 的天空立方体/辐照度/预过滤/BRDF 表回读），
   只有像素组红。查截图：`csm_on.png` / `csm_cascades1.png` /
   `ibl_no_sky.png` / `ibl_redblue_sky.png` / `ibl_redblue_sphere.png`
   全都恰好 **4460 字节 = 1280×800 纯白（整窗单色）**。
   `PrintWindow(PW_RENDERFULLCONTENT)` 在 Vulkan 交换链窗口上**偶发**返回
   "尺寸正常但未合成"的白帧；空图 vs 正常图做差 → "差异铺满整窗 / 采样区全 255"，
   看起来极像"阴影开关改变了整个画面"。
   修法：`tools/capture_window.py` 新增 `capture_stable()` + `_looks_blank()`
   （整窗下采样后 min/max 差 < 3 即判空帧并重试），`capture_nonempty` 转发到它；
   csm/ibl 的 `capture_client` 调用换成 `capture_stable`。
   修后两例单跑 **PASS**；另跑的 7 支同样走 `capture_nonempty` 的用例
   （auto_exposure / color_lut / drs / hiz / light_modifiers / nav_gizmo /
   shader_variants）**7/7 PASS**，证明加固无副作用。

2. **`verify_bvh`：右下角"空地"被 E14 挪过来的导航球吃掉了。**
   该用例靠"点视口右下角空地应打不中任何物体"验证射线拾取。E14 的四角布局把
   导航球从**左下**搬到了**右下**（左上模式条 / 右上统计 / 左下相机信息 /
   右下导航球），四个角全被浮层（子窗口）占据 —— 子窗口会先吃掉落在自己身上的
   点击，视口的 pick 根本不会执行（症状：`miss 日志 0 -> 0`）。
   有意思的是用例里原本就有"**不能点左下角** —— 那里是导航小图标"的注释，
   说明这个坑在 E14 之前就存在，只是随导航球换了角。
   修法：该用例统一以 `MYVK_EDITOR_NO_HUD=1` 启动（与 csm/ibl 同辙），
   四角浮层全灭后右下角才是真正的空地。
   修后单跑 **PASS**，且预热点击从"10 次/21.8s 全落空"变成"1 次/2.0s 收到" ——
   反过来印证了诊断。

**结论**：阶段 C 的**有效结果 = 42/42**（全量 39 PASS + 上述 3 例修复后单跑 PASS；
另 7 支受 `capture_nonempty` 改动影响的用例也已定向复筛 PASS）。
按用户要求，**不再为"凑一个干净的 42/42"自动补跑全量** —— 全量回归只在用户
明确下达指令时执行（见 `MEMORY.md` 的「回归执行纪律」）。

#### E07 的四个尾巴（多视口的"独立"那一半）

E07 的目标拆成两半：**能同时出多个视角**（已落地：`Single/2V/2H/Quad`
四种布局、`SUBVIEW: count -> N` 日志、布局随工作区落盘）与**每个子视口
各自独立**（未完成）。未完成的四项如实记账，不假装能用：

- 子视口**性能降级**（非活动视口关 TAA / 半分辨率）—— 现在的实现是
  "每个子视口各渲染一次"，没有质量分级；
- 子视口**独立视图模式 / 独立 gizmo** —— 目前视图模式是 `Renderer` 全局的
  一个字段，四个子视口共用；
- **点击激活**子视口 —— 只有"活动视口"接受输入这条还是隐式的；
- 视口矩形**数组化** —— `EditorContext::viewportRect()` 仍是单矩形。
  （连带：键盘导航模式会把 `IsItemHovered` 压成 false，所以判据用的是矩形。）

---

## 5 · 阶段 D · 交互理念（E18 ~ E23）

> 三条成熟度硬指标：**可发现（命令面板）** / **可撤销（设置类改动）** / **可自动化（控件 ID）**。  
> 本阶段把"操作"这件事从"我记得这个按钮在哪"升级为"我能搜到、能撤回、能写脚本驱动"。

### E18 · 命令注册表 + 命令面板（Ctrl+Shift+P） ✅ 已完成（2026-10-15）

- **目标**：上千个操作**模糊搜索直达**，这是"功能完全"的可发现性保障。
- **改动点**
  1. `editor/CommandRegistry.h/.cpp`：`registerCommand({ id, label, category, shortcut, handler, when })`。  
     `when` = 可用性谓词（如"有选中实体时才可用"）。
  2. **所有菜单项 / 工具栏按钮 / 快捷键 / Console 命令全部注册进同一张表**（消除三处硬编码）。
  3. **命令面板 UI**：`Ctrl+Shift+P` 弹出居中输入框，模糊搜索，  
     ↑↓ 选择、Enter 执行、右侧显示快捷键与分类；无匹配时给"最接近"建议。
  4. 最近使用（MRU）置顶。
- **涉及文件**：`editor/CommandRegistry.h/.cpp`（新建）、`editor/EditorApp.cpp`、  
  `editor/Toolbar.cpp`、`EditorContext.cpp`
- **验收**：新增 `tools/verify_command_palette.py` —— 打开面板、输入 `ssr`、  
  断言候选中含 "Toggle SSR"、执行后日志出现断言。
- **规模**：**M**
- **依赖**：E04（面板）、E16（共用注册表）

### E19 · 撤销覆盖扩展到"设置类改动" ✅ 已完成（2026-10-15）

- **目标**：**改渲染参数也能撤销**。现状：拖一下 Exposure 滑条就回不去了。
- **改动点**
  1. 生成器（E02）在 `ImGui::IsItemDeactivatedAfterEdit()` 时包一个  
     `LambdaCommand{ before, after }` 推进 `CommandStack`（现有 `Command.h` 已有 `LambdaCommand`）。
  2. **合并连续编辑**：同一控件在 500ms 内的连续拖动合并为一条撤销记录（避免滑条拖出 40 条历史）。
  3. **扩展 `CommandStack` 支持"参数设置"类命令**：需要能捕捉"哪个 feature/param"以便  
     `Edit ▸ History` 显示成 "Set SSR Strength: 0.5 → 0.8"。
  4. Inspector 的所有字段同样接入（现在只有变换/碰撞体/图元可撤销）。
- **涉及文件**：`editor/Command.h/.cpp`、`editor/FeatureUIGen.cpp`、`editor/InspectorPanel.cpp`
- **验收**：新增 `tools/verify_undo_settings.py` —— 注入点击改 SSR Strength → 断言 undo 后  
  日志显示恢复到旧值；重做后回到新值。
- **规模**：**M**
- **依赖**：E02、E11

### E20 · 覆盖指示器与 Reset to inherited

- **目标**：让"这个值是从哪来的"一目了然。
- **改动点**
  1. 指示器：`●` 本作用域显式设置 / `○` 继承自上层作用域 / `◇` 引擎默认。  
     悬停显示来源（如 "继承自 Project Settings"）。
  2. 右键该指示器 → `Reset to inherited`（删掉本层覆盖）。
  3. 顶部一条 **`Overrides: N`** 汇总，点击展开只看被覆盖项。
  4. 与 E03 的覆盖链、E11 的 Inspector 共用同一套渲染逻辑。
- **涉及文件**：`editor/FeatureUIGen.cpp`、`engine/render/RenderSettingsIO.cpp`
- **验收**：新增 `tools/verify_overrides.py` —— 在 Project 层设值 → Scene 层显示 `○`；  
  在 Scene 层覆盖 → 显示 `●`；Reset 后回到 `○`。
- **规模**：**S**
- **依赖**：E03、E02


### E21 · 通知 / 进度系统 ✅ 已完成（2026-10-15）

- **目标**：`WARN`/`ERROR` 自动变成**可见通知**；长任务有**进度条**。
- **改动点**
  1. **Toast 通知**：右上角堆叠，4 秒自动淡出；按级别配色  
     （Info 灰 / Warn 黄 / Error 红）；点 Error 直接打开 Console 定位到那条。  
     由 `VK_LOG_WARN/ERROR` **自动升级**产生（现在只写日志，用户看不见）。
  2. **进度条**：异步任务（编译着色器 / 烘焙光照贴图 / 导入资产）显示  
     `标题 + 百分比 + 取消按钮`；可最小化到状态栏。
  3. **状态栏常驻**：当前工具提示 / 选中数 / 保存状态（`●` 未保存）/ 后台任务数。
  4. **`Show Notification History`**（Help 菜单）：错过的通知可回看。
- **涉及文件**：`editor/NotificationCenter.h/.cpp`（新建）、`engine/core/Log.h`、  
  `editor/EditorApp.cpp`（`drawStatusBar` 1043~1116 行）
- **验收**：新增 `tools/verify_notifications.py` —— 注入一条 `VK_LOG_ERROR` →  
  断言状态栏出现 Error 计数、Console 里能定位。
- **规模**：**S**
- **依赖**：E16、E18

### E22 · Gizmo 与选择增强

- **目标**：把"摆场景"这件事做到专业工具的手感。
- **改动点**
  1. **框选**（`Q` 拖拽矩形选择）/ **加选**（`Ctrl`）/ **连选**（`Shift`）。
  2. **循环选择**：`Alt+Click` 在重叠物体间循环（现有 `PickingSystem` 已有射线拾取）。
  3. **多选整体变换**：多选时 gizmo 位于选区中心，变换整体应用（现在只有单选）。
  4. **对齐 / 分布工具**：`Align ▸ Min/Max/Center X/Y/Z` + `Distribute ▸ X/Y/Z`。
  5. **吸附可视化**：拖拽时显示吸附目标网格线与吸附点（现在只有数值吸附）。
  6. **本地/世界空间参考**：gizmo 本地空间时显示对象三轴。
  7. **锁定对象**不参与拾取（依赖 E12 的锁定列）。
- **涉及文件**：`editor/GizmoController.cpp`（1079 行）、`editor/PickingSystem.cpp`、  
  `editor/ColliderView.cpp`、`editor/EditorContext.h`
- **验收**：新增 `tools/verify_selection.py` —— 框选后选中数正确；对齐后包围盒对齐误差 < 1e-4。
- **规模**：**L**
- **依赖**：E12

### E23 · 快捷键表驱动 + 快捷键总览

- **目标**：快捷键从"硬编码在 `handleShortcuts()`"变成"**可查、可改、可导出**"。
- **改动点**
  1. 快捷键定义并入 `CommandRegistry`（E18）的 `shortcut` 字段，**统一分发**。
  2. **可重绑定**：Preferences ▸ Input 页，点击录制新组合键；冲突检测 + 警告。
  3. **总览窗口**：`Help ▸ Shortcut Reference`，按分类列出全部快捷键，可搜索、可打印/导出。
  4. **首次发现**：菜单项右侧显示快捷键；工具栏按钮 tooltip 也显示。
  5. 落盘到 `config/editor_keys.json`。
- **涉及文件**：`editor/CommandRegistry.h/.cpp`、`editor/EditorApp.cpp`（`handleShortcuts` 重构）
- **验收**：新增 `tools/verify_shortcuts.py` —— 重绑定 `Ctrl+D` 到其它命令后按 `Ctrl+D` 生效；  
  冲突时出现 WARN；总览表项数 == 注册命令数。
- **规模**：**M**
- **依赖**：E18、E04

#### 阶段 D 落地记录（E18 / E19 / E21，2026-10-15）

| 任务 | 交付形态（关键实现） | 验收锚点（机器可判） |
| --- | --- | --- |
| E18 | `editor/CommandRegistry.h/.cpp`（新建）：`CommandDesc{id,label,category,shortcut,keywords,when,run}` + 单例注册表（`add` 重复 id 覆盖 / `search` 模糊打分 / `closest` 用 LCS 比例给"最接近" / `recent` MRU / `findByShortcut`）；命令面板 UI（`Ctrl+Shift+P`，居中 560×400，↑↓ + Enter + 右侧显示快捷键与分类，无匹配给建议）。**命令全部从既有注册表派生**：`render::kFeatures[]`（61 组里 32 组带 `.enabled`）→ 面板表 → `Renderer::kViewModeCount` → 工作区 → `ConsolePanel::commands()` → 11 条固有动作。实测 `CMD-REGISTRY count=70`（含 E21 新增的 `notif.*` 与 `help.notificationHistory`） | `tools/verify_command_palette.py`：注册表 >= 50 条 / `CMD-PALETTE query="ssr"` 榜首 `feature.toggle.ssr`（label 恰为路线图要求的 `Toggle SSR`）/ `MYVK_EDITOR_CMDPAL_EXEC=1` → `CMD-RUN id=feature.toggle.ssr via=palette` + `FTR-UI ssr.enabled`（执行真走 UI 同一套副作用）/ `query=zzzz` → `hits=0` + `best="Toggle Hi-Z occlusion"` |
| E19 | `Command.h` 新增 `ParamEditCommand`（`label + paramId + void* target + before/after 字节快照`，`mergeAfter` 只换 after，`name()` 返回**预拼**好的 `"Set SSR Strength: 0.600 -> 1.500"`）与 `CommandStack::topCommand()`；`FeatureUIGen` 在 **`SetTooltip` 之前**（tooltip 会提交新 item，之后 `IsItemDeactivatedAfterEdit` 问的就是 tooltip）抓 `IsItemActivated/IsItemDeactivatedAfterEdit` 并把 before/after 交给宿主钩子；`EditorApp::onFeatureParamUndo` 做 **500ms 同参数合并**（三重条件：同一命令对象 + 同 paramId + 时间窗 + 栈顶就是它） | `tools/verify_undo_settings.py`：`UNDO-SELFTEST param=ssr.strength kind=3 merged=1 undoOk=1 redoOk=1 restored=1 depthDelta=0`；Float 与 Bool 两类 kind 各一遍；`UNDO-PARAM push` 恰 1 次 / `merge` 恰 1 次；历史名字 `Set SSR: off -> on`（**不重复** `SSR SSR`） |
| E21 | `editor/NotificationCenter.h/.cpp`（新建）：Toast（右上角堆叠 <= 6 条、4s 生命周期含 0.25s 淡入 / 0.8s 淡出、按级别配色）/**历史**（环形 256，`Help ▸ Show Notification History`，点条目 = 打开 Console 定位）/**进度任务**（`beginTask/setTaskProgress/requestCancel/endTask`，标题+百分比+Cancel+最小化到状态栏）/**状态栏常驻**（工具提示 / 选中数 / `* unsaved` / `tasks N` / `errors N` / `warns N` / 未读角标）；落盘 `config/editor_notifications.json`。**通道选择**：不复用 `ConsolePanel` 的 sink（那是单消费者且会收到 Console 自己的回显行），而是给 `engine/core/Logger.h` 加了第二条**正交**通道 `setLogHook`；只有 `Warn/Error` 自动升级，`Info` 只能显式 `push`（否则启动几百条 Info 会把屏幕糊满），同文本在存活期内**去重**（`repeat` 计数） | `tools/verify_notifications.py`（5 组）：注入 `MYVK_EDITOR_TOAST=error` → `NOTIF-PUSH level=Error source=log` + `NOTIF-STATUSBAR errors=1`（**状态栏出现 Error 计数**）+ `CONSOLE-DUMP err=1`（**Console 里能定位**）+ `NOTIF-DUMP history=1` 而 INFO 行数 >= 10×（证明 Info 不自动升级）；`MYVK_EDITOR_TOAST_CLICK=0` → `NOTIF-CLICK ... focus=console` + `NOTIF-FOCUS tab=console ok=1`；`MYVK_EDITOR_ASYNC_TASK=1` → `NOTIF-TASK-BEGIN/STEP(0/0.27/0.5/0.77/1.0)/END cancelled=0` + `"... - done"` 通知 + 状态栏出现过 `tasks=1`；`MYVK_EDITOR_ASYNC_TASK_CANCEL=1` → `NOTIF-TASK-CANCEL` + `END cancelled=1` 且**不发**完成通知 |

**顺手补的欠账**：`verify_undo_settings` 与 `verify_notifications` 已进
`run_regression.py` 的 `DEFAULT` + `TRIGGERS`（`DEFAULT` 现 45 例；
`check_regression_map.py` 仍 **PASS**，无死映射 / 无全横切登记）。

**E21 带来的一个连锁影响（已处理，留档）**：E14 的 `verify_viewport.py` 有一条
V3 = "与 E14 改造前的**整窗**参考图逐像素一致（连面板边框都没动）"。状态栏新增
四个常驻项后，这条按设计不再成立。实测差异 **4692 px，bbox = x[170,871] ×
y[825,840]**，状态栏（客户端底部 `kStatusH = 26` px）以上 **0 差**。
处理方式是**拆断言、不重采基线**：V3 改成"状态栏那一条以上整窗 0 差"，
另加 V3b = "**全部**差异都落在那一条内、且**非零**"（有人把状态栏改回去
→ V3b 变红）。不重采基线的理由：重采等于把当前二进制的像素无条件盖成"参考"，
V3 就退化成"自己跟自己比"。修后 `verify_viewport.py` **40 项断言全过**。

**E21 的两条设计取舍**（写下来免得下次又想"简化"回去）：

1. **为什么不挂第二个 sink**：`core::Logger` 的 sink 语义是"日志的显示端"，
   由 `ConsolePanel` 独占；通知中心关心的是"要不要弹给用户看"，两件事正交。
   混在一起还有两个具体坏处：Console 自己打印的回显行（`> cmd`、命令输出）
   会一起变成 toast；以及"有没有 Console 面板"会决定"有没有通知"。
2. **为什么 Info 不自动升级**：一次启动就是 50~几百条 INFO，全弹出来等于
   没有通知。所以自动升级只覆盖 `Warn/Error`（路线图的原文就是
   "由 `VK_LOG_WARN/ERROR` **自动升级**产生"），Info 通知只能显式 `push`。
   验收里专门留了一条"INFO 行数 >= 10× 通知条数"来钉住这个边界。

---

## 6 · 阶段 E · 生产力（E24 ~ E29，3A 特征）

> 这一阶段的功能是"**成熟引擎的分水岭**"：有它们不一定好用，没它们一定不专业。

### E24 · 资产编辑器框架（材质节点图 / 曲线编辑器）

- **目标**：提供可复用的"**资产编辑器**"容器（独立窗口 + 工具栏 + 画布 + 属性侧栏），  
  首批落地两个编辑器。
- **改动点**
  1. `editor/AssetEditorHost.h/.cpp`：统一的资产编辑器外壳  
     （标题栏 + 面包屑 + 工具栏 + 画布 + 右侧属性 + 底部状态）。
  2. **材质节点图**：节点（输出 / PBR 输入 / 常量 / 贴图采样 / 运算）+ 连线 +  
     预览球 + 编译到 shader。**先做"只读展示现有材质"再逐步加编辑**。
  3. **曲线编辑器**（时间轴用）：关键帧贝塞尔曲线 + 手柄 + 切线模式。
  4. 与 ContentBrowser 双击资产联动（E13）。
- **涉及文件**：`editor/AssetEditorHost.*`、`editor/MaterialGraph.*`、`editor/CurveEditor.*`（均新建）
- **验收**：材质图能展示场景里任一材质并高亮其连接；曲线编辑器能编辑一条测试曲线。
- **规模**：**L**
- **依赖**：E13、E05（工作区）

### E25 · GPU 时戳时间轴 ✅ 已完成（2026-10-15）

- **目标**：把 `GpuTimer` 的 10 段画成**时间轴甘特图**（对标 UE GPU Profiler）。
- **改动点**
  1. 横向时间轴，每段一个色块，长度 = 耗时；悬停显示名称/ms/占比。
  2. 多帧历史（最近 60 帧）堆叠视图，能看出抖动。
  3. 点击某段 → 联动 Console 过滤该 pass 的日志。
  4. 归入 Rendering 工作区（E05）。
- **涉及文件**：`editor/GpuTimelinePanel.h/.cpp`（新建）、`engine/render/GpuTimer.h`
  → **实际落地**还改了 `editor/ConsolePanel.h/.cpp`（新增 `setSearchFilter`）、
  `editor/EditorApp.h/.cpp`（接线：每帧采样 + env 钩子 + 面板表一行）、
  `editor/Workspace.cpp`（`kRenderOpen` 加一项）。
- **验收**：时间轴总长与 `GPU-TIMING` 日志的帧总耗时一致（误差 < 5%）。
  → 已由 `tools/verify_gpu_timeline.py`（37 项断言）覆盖，实测最大值 **2.65%**；
  落地形态与设计取舍见 §6 末尾「阶段 E 落地记录」。
- **规模**：**M**
- **依赖**：E05、E15

### E26 · 画质档位（Quality Settings 面板）

- **目标**：一次切到"低/中/高/极高/电影级"，含**平台预设**。
- **改动点**
  1. 档位定义 = 一批 `feature.param` 的目标值集合（数据来自 E01 注册表）。  
     例：`Low` 关 SSR/SSGI/Godrays、级联 1 级、TAA 关；`Cinematic` 全开 + 级联 4 级 + 超采样。
  2. `Quality Settings` 面板：档位单选 + **逐项差异预览**（切档后哪些参数会变）。
  3. **平台预设**：`PC High / PC Low / Console / Mobile / VR`，可另存为自定义档。
  4. **一致性检查**：切档后跑一次"档位自检"（该开的开了、该关的关了），差异列表面板内显示。
  5. 档位写入 `config/render.json` 的 `quality` 键，并作用于 E09 的 `Preset ▾`。
- **涉及文件**：`editor/QualitySettingsPanel.h/.cpp`（新建）、`engine/render/RenderSettingsIO.cpp`
- **验收**：新增 `tools/verify_quality_presets.py` —— 切到 Low 后断言 SSR env 生效值为 0；  
  切回 Cinematic 后为 1；差异列表项数与注册表一致。
- **规模**：**M**
- **依赖**：E01、E03、E09

### E27 · RenderGraph 可视化（与 R7-1 同步）

- **目标**：把渲染管线画成**有向图**（pass 节点 + 资源边），可交互。
- **改动点**
  1. 节点 = pass（含时戳耗时着色），边 = 纹理/缓冲依赖（读写关系）。
  2. 悬停高亮上下游；点击节点 → 只看该 pass 的输入输出资源。
  3. **与 R7-1 RenderGraph 同步交付**——R7-1 落地时本面板必须同时可用（准入契约）。
- **涉及文件**：`editor/RenderGraphViewer.h/.cpp`（新建）
- **验收**：节点数 == 实际 pass 数；边与代码里的依赖声明一致。
- **规模**：**M**
- **依赖**：R7-1（渲染侧）

### E28 · 打包 / 发布 UI（含 Shipping 剔除）

- **目标**：从编辑器一键出包；**Debug 档与调试 UI 在 Shipping 构建里剔除**。
- **改动点**
  1. `File ▸ Package ▸` 对话框：目标平台 / 构建配置（Debug/Development/Shipping）/  
     输出目录 / 是否含编辑器 / 是否含调试符号。
  2. **性能与体积**：Shipping 下
     - 剔除 `Debug` tier 的全部参数与控件；
     - 剔除 Console / GPU 时间轴 / RenderGraph 视图等调试面板；
     - 剔除 C++ 侧的调试代码路径（用编译期宏 `VENN_SHIPPING`）。
  3. **打包进度**（复用 E21 进度条）+ 结果通知（含产物路径、体积）。
  4. **资产裁剪**：只包含被引用的资产（依赖 E13 的引用扫描）。
- **涉及文件**：`editor/PackageDialog.h/.cpp`（新建）、`CMakeLists.txt`（`VENN_SHIPPING` 宏）、  
  `main.cpp`、各调试面板（条件编译）
- **验收**：新增 `tools/verify_shipping_trim.py` —— 以 `VENN_SHIPPING` 构建后断言  
  `kFeatures[]` 里无 `Debug` tier 项、Console 面板注册项不存在。
- **规模**：**L**
- **依赖**：E21、E13

### E29 · 烘焙流水线 UI（光照贴图 / 反射探针）

- **目标**：把"离线烘焙"变成有进度、有预览、可取消的编辑器流程。
- **改动点**
  1. 烘焙设置面板（质量档 / 分辨率 / 采样数 / 是否增量）。
  2. 进度条 + 剩余时间估计 + 取消（复用 E21）。
  3. 结果预览：光照贴图缩略图、探针预览球。
  4. **与 R5-1 / R5-2 同步交付**（反射探针 / 光照贴图落地时本面板必须同时可用）。
- **涉及文件**：`editor/BakePanel.h/.cpp`（新建）
- **验收**：烘焙完成后产物存在且预览可读；取消后无残留临时文件。
- **规模**：**M**
- **依赖**：R5-1 / R5-2（渲染侧）、E21
  · **R5-1 反射探针已于 2026-10-15 落地** → 本任务的**探针那一路已可开工**
    （烘焙设置档 / 进度条 + 取消 / 探针预览球）；**光照贴图那一路仍等 R5-2**。
  · 另注：R5-1 交付时**已在 `Inspector ▸ Reflection Probe` 落了属性面板 + `Capture` 按钮**
    （带撤销、运行模式只读）—— 所以本任务不要再重复做一个"探针设置"面板，
    只需补"批量烘焙 + 进度 + 预览"。**探针放置 gizmo 仍缺**，那是 E15/E14 视口侧的活。

#### 阶段 E 落地记录（E25，2026-10-15）

**本轮只做了 E25**，原因是其余五项各有一个**未满足的前置**（见下面的"为什么先做 E25"）。
E25 全部依赖（E05 工作区 / E15 统计面板 / R0-1 的 `GpuTimer`）都已就绪。

| 任务 | 交付形态（关键实现） | 验收锚点（机器可判） |
| --- | --- | --- |
| E25 | `editor/GpuTimelinePanel.h/.cpp`（新建）：① **本帧甘特图**（10 段色块按占比铺满，`InvisibleButton` + 手动命中测试 → 悬停读名称/ms/占比/历史均值，`gap` 画成右端灰条）；② **最近 60 帧堆叠**（旧在上，行高 4px，选中段在历史里保留原色、其余压暗，于是"这一段的趋势"纵向一眼可读）；③ **段明细表**（名称/ms/占比/历史均值，段名是按钮 = 与甘特图同一个动作）；④ **点段联动 Console**（复用现成的搜索框，不发明新协议）。**历史由面板自己攒**：`GpuTimer` 只存最近一帧（它不该管跨帧历史），面板每帧 `sample()` 一次写进 60 槽环形缓冲 | `tools/verify_gpu_timeline.py`（6 组 37 项）：**时间轴总长 vs `GPU-TIMING`**（`seg_sum≈last` 最大差 **2.65%**、`gap/last` 最大 **2.38%**、面板中位 vs GPU-TIMING 中位差 **0.26%**）/ `frames` 涨到 **60** 封顶 / `MYVK_EDITOR_GPUTIMELINE_CLICK=forward` → `GPU-TIMELINE-CLICK seg=forward index=6` + `CONSOLE-FILTER text=forward` + 选中态在后续快照保持（换 `depth` → `index=2`，证明是查表不是硬编码）/ `MYVK_EDITOR_WORKSPACE=rendering` → `PANEL-STATE gputimeline=open` / 采样+快照后**视口逐像素 0 差** / 全轮零验证层报错 |

**"时间轴总长"到底等于什么**（验收条款的落点，值得写下来）：
`GpuTimer::lastTotalMs()` 是**首 begin → 末 end 的跨度**，而甘特图能画的只有 10 段的
**各自时长**（绝对起点没暴露）。于是 `lastTotal = seg_sum + gap`。验收要求"时间轴总长
与 `GPU-TIMING` 的帧总耗时一致（误差 < 5%）"——本面板把三个数都报出来，断言
`gap ≥ 0 且 gap/last < 5%`。这比"让总长恒等于 total"有信息量：它同时证明了
**各段确实覆盖了整帧**，没有大片没被计时的黑洞。

**三条设计取舍**：

1. **历史由面板自己攒，不改 `GpuTimer`**：`GpuTimer` 是"最近一帧"的持有者，
   给它加历史就要跟 in-flight 对齐，那是它不该管的事。面板采样是**纯读**，
   每帧一次（`onUpdate` 里，**面板关着也在积累**）→ 打开即见完整 60 帧。
2. **point-to-filter 复用 Console 的搜索框**：段名（`forward` / `shadows`…）
   恰好是日志里会出现的词，所以"联动"就是"把段名填进那个已有的搜索框"。
   不新增过滤机制，用户还能接着手改。
3. **面板之间不互相引用**：`draw(bool*, ConsolePanel*)` 把 Console 从**调用点**
   传进来（由 `EditorApp` 接线），而不是面板自己去 `CommandRegistry` / 单例里找。
   这是 E04 起就有的硬约束，E25 沿用。

**本轮踩到的三个坑（都有普遍性）**：

1. ⚠ **`MYVK_FRAMES=N` 会让进程"几秒内跑完"** —— 实测 `MYVK_FRAMES=900` 只活
   **2.07 秒**（无 vsync 时帧率 ~450fps）。于是"启动 → 等日志 → 截图"的用例
   拿到的是**已退出的窗口**：`GetClientRect` 返回 `0×0`、`PrintWindow` 返回
   `False`、`capture_stable()` 一直返回 `None`。**结论：需要截图的用例不要用
   `MYVK_FRAMES` 控帧，改成"等条件出现 + 主动关窗"**（`MYVK_NO_WINDOW_SAVE=1`
   已经足够让 `automationRun()` 为真，写盘照样全程禁止）。
2. **`MYVK_GPU_TIMELINE` 被判"裸 env"**：编辑器侧注入的 env 必须带
   `MYVK_EDITOR_` 前缀（`check_feature_surface.py` 的白名单按前缀放行）→ 改名
   `MYVK_EDITOR_GPU_TIMELINE`。引擎侧的 `MYVK_GPU_TIMING` 属于另一套（不在白名单
   范围内，走"诊断项"表）。
3. **`onInit` 里没有 `logPanelStates()` 调用点**：给 `MYVK_EDITOR_GPUTIMELINE`
   这种"启动即改面板状态"的钩子补了一次显式调用 —— 否则日志里只剩
   `loadEditorLayoutFromDisk` 打的 `closed`，"钩子生效了"这件事没有证据。

**为什么先做 E25（其余五项的状态）**：

| 任务 | 状态 | 阻塞原因 |
| --- | --- | --- |
| E24 资产编辑器框架 | 待做 | 依赖 E13 / E05（**均已就绪**）—— L 级，可立刻开工 |
| E26 画质档位 | 待做 | ✅ 依赖的 **E03（设置层级与持久化）已于 2026-10-17 落地**（`engine/render/RenderSettingsIO.*` + `config/render.json`）—— 准入契约已满足 |
| E27 RenderGraph 可视化 | 待做 | ⚠ **准入契约未满足**：依赖渲染侧 **R7-1「声明式 RenderGraph」**，`docs/render_roadmap.md` 里它还在阶段 7（未做）。现有 `render/FrameGraph` 只覆盖"前向之后"的 5 个 pass，不是全管线 |
| E28 打包 / 发布 UI | 待做 | 依赖 E21 / E13（**均已就绪**）—— L 级，需动 `CMakeLists.txt` 加 `VENN_SHIPPING` 宏 |
| E29 烘焙流水线 UI | 待做 | 🟡 **准入契约部分满足（2026-10-15）**：**R5-1 反射探针已落地** ✅（`engine/render/ReflectionProbe.*` + `Inspector ▸ Reflection Probe` 属性面板 + `Capture` 按钮），但 **R5-2 lightmap 仍未落地** ✗ —— 面板的"光照贴图缩略图"那一半还是空的。可先做反射探针那一路（设置档 + 进度条 + 探针预览球），光照贴图那一路等 R5-2 |

**顺手补的欠账**：`verify_gpu_timeline` 已进 `run_regression.py` 的
`DEFAULT` + `TRIGGERS`（`DEFAULT` 现 **46 例**；`check_regression_map.py`
**PASS**：40 例已登记 / 46 例，6 例按设计无条件重跑；无死映射、无全横切登记）。
`check_feature_surface.py` **PASS**（32/32 特性有 UI 面、裸 env 漏网 0）。

---

## 7 · 每个任务的通用 Definition of Done

**任何 E 任务"完成"= 下列五件套齐备**（与架构文档 §9.1 一致）：

| # | 交付物                                                               | 检查方式                            |
| - | ----------------------------------------------------------------- | ------------------------------- |
| 1 | **实现**：功能与 UI 都在                                                  | 人工 + 截图                         |
| 2 | **登记**：写进 `FeatureRegistry` / `PanelRegistry` / `CommandRegistry` | `check_feature_surface.py` 不报红  |
| 3 | **UI 面**：能点到（不是只有 env）                                            | `check_feature_surface.py` C 组绿 |
| 4 | **序列化**：能存能读                                                      | `config/*.json` 往返测试            |
| 5 | **回归用例**：新增 `verify_*.py` 并入 `run_regression.py` 的 `DEFAULT`      | 该用例 PASS                        |

**外加三条本项目铁律**：

- **控件 ID 稳定**：格式 `###<featureId>.<paramId>`（自动化断言的前提）。
- **默认不破坏基线**：新增 UI 默认值必须保证"用户没动过 = 行为与此前逐像素一致"。
- **`MYVK_EDITOR_NO_HUD=1` 必须仍然像素干净**（否则 A 组"逐像素一致"断言会碎）。

---

## 8 · 与渲染路线图的准入契约（关键）

**R 系列完成时，它的 E 系列 UI 面必须同时落地**，否则不算完成。对照表：

| R 项                  | 必须同步交付的 UI 面                    | E 任务                 |
| -------------------- | ------------------------------- | -------------------- |
| R5-1 反射探针 ✅(2026-10-15) | 属性面板 ✅ + Capture 按钮 ✅（已随 R5-1 交付）；**仍缺**：探针 gizmo 放置 / 预览球 / 独立烘焙流程 | E29（+ E14 出 gizmo） |
| R5-2 lightmap / 光照探针 | 烘焙设置 + 进度 + 预览                  | E29                  |
| R5-3 DDGI            | 探针网格可视化 + 参数面板                  | E24（复用资产编辑器）         |
| R6-1 GPU 粒子 ✅(2026-10-15) | 粒子编辑器 ✅ —— **以 Unity Shuriken 的 12 个模块为准**（Playback / Emission / Shape / Velocity / Limit Velocity / Force / Colour / Size / Rotation / Noise / Renderer / Soft Particles），含模块勾选、模块关掉即置灰、逐模块 Reset、`Expand all`/`Collapse all`、两级搜索过滤；颜色 = **带停靠点的渐变色带**（Blender Color Ramp 形态：预览带 + 拖动 + 增删 + `Add stop`）。**仍缺**：① 视口里的粒子预览窗口（现在只能看场景里的实体）；② 曲线仍是 4 点滑条组，不是贝塞尔手柄（roadmap 允许"先用滑条组"）；③ 帧事件 / ribbon / mesh 渲染模式。 | E24（部分前置已在 R6-1 内落地） |
| R6-2 水体              | 水面参数面板 + 视口交互                   | E09（登记即得）            |
| R6-3 植被风             | 风场面板 + 视口风可视化                   | E09（登记即得）            |
| R6-4 地形              | **地形雕刻笔刷工具** + 工具栏 + splat 图层   | E24 + E08            |
| R6-5 动态天空            | 大气参数面板（日期/时间/浊度）+ 时间轴           | E09 + E08（Transport） |
| R7-1 RenderGraph     | 图可视化面板                          | E27                  |
| R7-3 ray tracing     | 加速结构可视化 + RT 设置面板               | E27 + E09            |

> **准入规则**：在 `FeatureRegistry` 里登记 = 自动获得 **env / UI / 序列化 / 工具提示 / 成本显示**  
> 五件套。这是"以后升级功能时也会带 UI"的**机制保证**，而不是靠记性。

---

## 9 · 硬约束（做任何 E 任务都不能违反）

1. **不破坏全量回归基线**（E14 收口时是 **42 例**）。所有新 UI 默认值保持"全关"，`config/*.json` 缺失时  
   行为与此前**逐像素一致**。
2. **`MYVK_EDITOR_NO_HUD=1` 下视口必须像素干净**——HUD 噪声会污染"逐像素一致"断言。
3. **不引入 ImGui docking 分支**（架构文档附录 B.1）：docking 会带进 `imgui.ini`  
   外部状态，破坏像素级对照的确定性。沿用并增强 `SplitLayout`。
4. **env 变量保留**（自动化预置状态的正当手段），但**必须同时在注册表登记**。
5. **UI 改动同样走变更影响分析**：`editor/` 改动 → `TRIGGERS` 映射到对应 `verify_*`；  
   登记表要同步补（`tools/check_regression_map.py` 守护）。
6. **回归运行期间不构建、不跑 GUI 测试**。

---

## 10 · 里程碑（可验证的阶段性成果）

| 里程碑             | 内容                                      | 判定信号                                                 |
| --------------- | --------------------------------------- | ---------------------------------------------------- |
| **M1 · 特性面补全** ✅ | E01 / E02 / E09 / E10                   | `check_feature_surface.py` **退出码 0**（实测 **32 个特性全部有 UI 面**、裸 env 漏网 0） |
| **M2 · 布局成熟** ✅ | E04 / E05 / E06                         | 工作区可切换、布局可落盘、分割线可视化（**2026-10-14 达成**） |
| **M3 · 核心面板达标** ✅ | E11 / E12 / E13 / E14 / E15 / E16 / E17 | 每个面板有独立 `verify_*.py` 且 PASS（**2026-10-15 达成**；E11/E13/E14 三支脚本为本轮新增） |
| **M4 · 交互成熟**   | E18 / E19 / E20 / E21 / E23             | 命令面板可搜、设置可撤销、快捷键可重绑定（**E18/E19/E21 于 2026-10-15 达成；E03 于 2026-10-17 达成**；E20 待做、E23 待做） |
| **M5 · 生产力工具**  | E07 / E22 / E24 / ✅E25 / E26             | 多视口、资产编辑器、时间轴、画质档位可用                                 |
| **M6 · 3A 收口**  | E08 / E27 / E28 / E29                   | 出包可用、Shipping 剔除生效、烘焙有 UI                            |

**M1 是投入产出比最高的里程碑**——它把"功能有了但人用不了"这个结构性缺陷一次性抹掉。

---

## 附录 · 现状与目标的差距速查（2026-10-14 初测 / 2026-10-15 复核）

**2026-10-15 复核**（阶段 A 地基 + B 布局 + C 面板做完之后重测；"现状"列已是现在的值）：

| 维度       | 现状（2026-10-15）              | 目标                                                                        | 相关任务                              |
| -------- | --------------- | ------------------------------------------------------------------------- | --------------------------------- |
| 面板数      | **11 + 3 条带**（E18 命令面板 / E21 通知历史 / **E25 GPU 时间轴** 陆续进来） | +3（Quality / RenderGraph / Bake） | ✅ E18 / ✅ E21 / ✅ E25 / E26 / E27 / E29 |
| 特性↔UI 覆盖 | **32/32（100%）**   | 100%                                                                      | ✅ E09 / E10 |
| 工作区      | **6 套 + 落盘**     | 6 套 + 落盘                                                                  | ✅ E05                               |
| 布局持久化    | **`config/editor_layout.json` + `editor_prefs.json`** | 全量                                             | ✅ E04 / E05 / E17                         |
| 多视口      | **1/2/4 分裂可用**（子视口不独立：共用视图模式与 gizmo）  | 1/2/4 分裂且各自独立                                                | 🟡 E07                               |
| 命令系统     | **统一注册表（71 条）+ 模糊搜索 + `Ctrl+Shift+P` 命令面板** | 统一注册表 + 模糊搜索                        | ✅ E16 / E18                               |
| 快捷键      | 硬编码、不可改         | 表驱动 + 可重绑定 + 总览                                                           | E23                               |
| 撤销覆盖     | 变换/碰撞/图元/场景 + Inspector 字段 + **渲染设置参数（500ms 合并）** | + 渲染设置                                        | ✅ E11 / E19                               |
| 设置层级     | **5 层覆盖链 + `config/render.json` 落盘**（prefs → project 接磁盘；scene/object/camera 已通数据通路，等 E20） | 5 层覆盖链 + 落盘                                                               | ✅ E03 / E20                         |
| 通知/进度    | **Toast + 进度条 + 历史 + 状态栏常驻计数 + 落盘** | Toast + 进度条 + 历史                                                          | ✅ E21                               |
| GPU 时戳可视化 | **本帧甘特图 + 60 帧历史堆叠 + 点段联动 Console**（另有 Stats 的单帧瀑布条） | 甘特图 + 多帧历史 + 联动                                    | ✅ E25                               |
| 日志可见性    | **Console 面板（级别过滤 + 搜索 + 命令行）**     | 级别过滤+搜索                                                       | ✅ E16                               |
| 编辑器代码量   | **23,614 行**（.cpp 19,391 + .h 4,223） | 结构化增长（生成器替代手写）                                          | 全部                                |

阶段 C 的逐项实现与取证见 §4 末尾的「阶段 C 落地记录」。

---

## 11 · 物理引擎（PHYS-1）✅ v1 已完成（2026-10-16）

> 由用户需求引入的**引擎级新子系统**（此前引擎没有物理）。不属于原 E/R 系列，
> 单独记为 PHYS。范围经用户拍板：**完整刚体核心** + **仅 Play 态仿真**。

### 11.1 交付范围（v1）

| 层            | 内容                                                                                     |
| ------------ | -------------------------------------------------------------------------------------- |
| 数学 / 形状    | 凸体（球 / 盒 / 胶囊 / 凸包）、AABB、`Shape` 与质量属性（质量或密度二选一）                                |
| 广相           | **SAP**（扫描剪枝，主轴 x）                                                                     |
| 窄相           | **GJK / EPA**（凸-凸）+ **SAT 盒盒裁剪**（生成 4 点流形）                                            |
| 求解器         | 顺序冲量 **PGS** + **热启动** + **Baumgarte** 位置修正 + **split impulse**（位置误差不污染速度）                 |
| 积分           | 半隐式欧拉 + **定步长累加器**（`fixedTimeStep` / `maxSubSteps`）                                      |
| 关节           | Fixed / Ball / Hinge / Slider / Distance                                                |
| 休眠           | **岛级休眠**（并查集把连成一串的刚体当一个岛，整岛一起睡）                                                   |
| ECS 组件       | `ecs::RigidBodyComponent`（`bodyType` / `shape` / 形状尺寸 / 质量 / 摩擦 / 弹性 / 阻尼 / 初速度 / `useGravity` / `allowSleep`） |
| 场景级设置      | `scene::PhysicsSettings`（重力 / 无限地面 / 定步长 / substeps / 迭代次数 / Baumgarte / slop / 休眠阈值）        |
| 序列化         | 已接 `SceneSerializer`：顶层 `physics` 段（**仅在与默认值不同时写**）+ 每实体 `rigidbody` 段                    |
| UI            | **Inspector ▸ Physics**：`Physics World`（世界参数，可 Reset）+ `Rigid Body`（选中物体）+ 复用 `Collision` 段        |

### 11.2 关键接线（`editor/EditorApp.cpp`）

- `buildPhysicsWorld()`：Play 时按场景建世界。**刚体形状取世界米**（显式形状不乘实体缩放；
  `Auto` 依次尝试 Collision 的 hull/capsule → 网格 AABB → 0.5m 球），并**把世界缩放烘进形状尺寸**。
- `stepPhysics(dt)`：运动学体**先**读实体位置反算速度 → `step()` → **只对动态体**写回
  `TransformComponent`（位置 + 由旋转矩阵反解的欧拉角；**刻意不写 scale**）。
- 每帧顺序：**脚本 → 玩家 → 物理 → 碰撞**。`CollisionWorld` 里 `physicsDriven` 的实体
  **不再参与碰撞解算**（位置归物理引擎管），但**仍然挡住相机**。
- ⚠ **只有真正 `Stop` 才销毁物理世界**——`Pause` 走的是同一分支，不能顺手 `reset()`。

### 11.3 验证入口

| 入口                                  | 用途                                                            |
| ----------------------------------- | ------------------------------------------------------------- |
| `MYVK_PHYS_TEST=1`                  | **无窗口、无 Vulkan** 的物理自检（`editor/main.cpp` 在 `app.run()` 前返回）；输出 `PHYS-TEST <name> result=PASS\|FAIL` + `PHYS-TOTAL pass=n fail=m`。**29/29 PASS** |
| `MYVK_PHYS_DUMP=<N>`                | Play 态每 N 帧打一行 `PHYS-BODY`（位置**从实体读**，用来证明写回链路真的生效）             |
| `MYVK_EDITOR_PLAY=1`                | 启动即进 Play 态（配合 `MYVK_EDITOR_SCENE` 观察仿真）                        |
| `tools/verify_physics.py`           | 回归用例，已进 `run_regression.py` 的 `DEFAULT`                       |

### 11.4 v1 明确不做（后续 PHYS-2 候选）

连续碰撞检测（CCD）/ 软体 / 布料 / 破碎 / 车辆轮胎模型 / 多层场景（高度场地形）/
物理调试可视化（接触点、AABB、岛着色）/ 与动画系统的双向耦合（布娃娃）。
另有：**碰撞事件回调**（进入/离开/持续）与**触发体（trigger）**尚未开放给脚本。

## 12 · 全量回归（2026-10-16）✅ 首轮 46/50 → 4 项全部定案

`python tools/run_regression.py --all`，**50 例 / 49m39s**。首轮 46/50；逐项定位后修掉 3 项
**测试度量缺陷** → 复跑 PASS；剩下 1 项**真产品缺陷**已按用户裁定修掉（见 12.1）。
收尾又清掉 E 组送达与粒子 3 项"差 1"口径（见 12.2）。

| 用例 | 首轮 | 处理 |
| --- | --- | --- |
| `verify_add_menu` | FAIL 断言 2 | ✅ 修：扫描区间夹进 `TR-BAR` 矩形（原先扫整窗，被右侧 Inspector 的 R/G/B 轴标签污染） |
| `verify_gpu_timeline` | FAIL T2e | ✅ 修：丢掉 `frames<30` 的启动样本（把两侧采样窗口对齐；**阈值仍 5%**） |
| `verify_particles` | FAIL B1/E1b/F2（另一轮 +G1b） | ✅ 全部修 —— **产品代码一行未动**，全是口径/注入路径问题，见 12.2 |
| `verify_material_roundtrip` | FAIL 3 项 | ✅ 修：材质行自适应列宽，见 12.1 |

### 12.1 ✅ 已修：材质槽方块被图标条挤没（2026-10-16）

- **现象**：已绑定的 `metallic / roughness / emissive` 槽方块只有 **48px**，`MAT-CHIP` 打出来是
  `shows=".png"` —— **文件名读不出来**（三个槽看起来一个样）。
- **根因**：面板 330px 本身没错（符合 `kRightMin`），但 **E11.1 的图标导航条占掉 48px**，内容列只剩
  ~251px；而材质行是固定 3 列 `88 + 10 + 88 + 10 = 196`，行首还缩进 ~73px →
  槽位只剩 `251 − 196 − 4 ≈ 48`。
- **修法（用户裁定：自适应列宽）**：**标签列固定 88**（标签必须读得全），让**值列**按可用宽收缩
  （下限 44px），**槽位保底 96px**。宽面板（`inner ≥ 292`）值列照样顶回 88 →
  **与改造前逐像素一致**。另给槽位 tooltip 补上**完整文件名**
  （330px 面板装不下 18 字符的名字，画面里 `fitText` 只能留 ~8 个字形）。
- **结果**：槽位 **48px → 89px**；`verify_material_roundtrip` / `verify_material_chips` 均 PASS。
- **防复发**：`verify_material_chips.py`（断言"面板拖窄时方块不越界 / 绑定后显示文件名"）
  **已补进 `DEFAULT` + `TRIGGERS`** —— 它原先不在清单里，所以这个缺陷一直没被拦住；
  `check_regression_map.py` 里"改 `InspectorPanel.cpp` 影响哪几例"的 fixture 同步成 **5 例**。
- **未选的备选**：① 提高 `SplitLayout::kRightMin`（330 → ~400）—— 会牵动 `verify_viewport` 的
  `CONVERGED_VP` 及一批截图基线；② 窄面板时槽位换行到第二行 —— 材质段高度近翻倍、纵向版式大改。

### 12.2 ✅ 收尾：E 组点击送达 + 粒子 3 项"差 1"口径（2026-10-16）

**全部是测试侧改动，产品代码一行未动**（详细叙事 → `.workbuddy/memory/2026-10-16.md` §18）。

- **E1b 复挂的真因 = `force_foreground` 的返回值被忽略**。上一轮记成"事件被系统丢掉"，加的
  "重抢前台 + 重试"在下一轮单跑里**又挂了**（点 3 次全未送达）。实际是它返回 False 时窗口还被
  终端压着，`mouse_event` 那几发**真实**点击全落到**压在上面的窗口**上了 —— 重试多少次都一样。
  新加一条与前台/遮挡无关的注入路径 **`click_client_post`**（`PostMessage(WM_LBUTTONDOWN/UP)`，
  位移仍由 `mouse_set_client` 的 `PostMessage(WM_MOUSEMOVE)` 提供），`click_for` 两路轮换。
  - 证据：**真实路径**与**强制直投路径**（把 `force_foreground` 打成永远 False）**各自**跑 E 组，
    均 1 次点击到位。
  - `verify_bvh` 的"预热点击"是同一根因（注释里早记下"机器忙时 12 次尝试全落空"），已换成
    `click_vp`：**10 次尝试/21.8s 全落空 → 1 次/2.0s**，用例 45s → 24s。
- **粒子 B1/B3/F2/G1b 改口径**（`§15.3` 早写明"应当先改断言口径"，这次先**量出分布**再定）：
  冻结帧跨运行**只有界**（实测 5 次：`alive` 极差 4、`maxAlive` 1、`simFrame` 120..121）——
  两条机制都关不掉：池按 frames-in-flight 分槽（各槽停在自己最后一步）+ 累加器允许一帧补多步。
  - `B1` 拆成"确定字段精确比 + 跨槽字段有界比"；`B3` 改 `[120, 127]`；**新增 `B3b`**：
    "冻结后真的停住"用**同一趟内多个样本互等**来精确验（更严）。
  - `F2` 的 `dispatches > simFrame` 在这台机器上**恒不成立**（vsync 60fps 与 simHz 60 相等 →
    实测 182 vs 183）→ F 组加 `MYVK_FX_HZ=8`，判据改 `dispatches >= 2*simFrame`（旧行为下必红，
    这才真的在验"速度与帧率解耦"）。
  - `G1b`（两次独立运行的瞬时计数）改有界。
  - ⚠ 引擎 `FX-FREEZE` 日志那句 "state is now reproduced bit-for-bit" 是**过度承诺**，
    `B1` 原来的期望正是照它写的 —— 改它要动 `engine/**`（欠一次全量），**暂留**，下次顺手改。
- **复跑**：`verify_particles` **35 项全 PASS**、`verify_bvh` PASS、两个自检脚本 PASS。

## 13 · 编辑器打磨七连（2026-10-17）✅ 全部落地

用户一次性下达 7 点体验清单，**全部完成 + 建验证**。5 个新脚本全 PASS，受影响既有用例复跑全 PASS。
（详细叙事 → `.workbuddy/memory/2026-10-17.md`）

| # | 需求 | 定论 |
| --- | --- | --- |
| 1 | 主题美化 + 多风格可选 | `engine/ui/ImGuiTheme.{h,cpp}` 新建；`ThemeId` 5 套；**VennDark 逐字段照抄 → 默认逐像素不变**；修掉浅色主题下的"深色孤岛"（新增 `Token.barBg` + 视口叠加片浅字） |
| 2 | 启动闪白 → 缓冲 2s | 隐藏创建 + `applyDarkBackgroundBrush()` 深灰底 + 首帧后 `show()`；新增 `MYVK_STARTUP_DELAY=1` 让缓冲可测又不写盘 |
| 3 | 胶囊线框规范 | `ColliderView.cpp` 重写：**12 贯通经线 + 4 纬线环**，球冠完整闭合；几何量测 H/W=3.571 全符合解析 |
| 4 | 吸附默认全开 | 构造默认 `false→true` + 同步 prefs `def`（否则老存档顶回）；新增 `MYVK_EDITOR_SNAP` |
| 5 | 栅格 1m×1m + 扩大 | 确认 `minor=1.0/major=10.0`；`extent 60→250`（500m 见方）、fade 同步 |
| 6 | 中英切换（**硬约束**） | `editor/Localization.{h,cpp}` 新建；`tr()`/`trId()`；**代码零中文字面量**（机械包装 792 处 `TR(...)`）；词典 ~609 条 |
| 7 | Hierarchy 锁/眼失效 | **真因 = `ItemHoverable` overlap 语义**（见 13.1）；修法 = 节点加 `ImGuiTreeNodeFlags_AllowOverlap`；另补太阳缺 `VisibilityComponent` |

### 13.1 ✅ 关键真因与坑（值得记牢）

- **`ItemHoverable` overlap**（`imgui.cpp:5121`）：叠在 `TreeNodeEx` 行矩形上的 `InvisibleButton`
  因节点**先提交**而被 `HoveredId != id` 挡掉 → **永远收不到 hover/click**。
  **修法 = 节点加 `ImGuiTreeNodeFlags_AllowOverlap`**（命中改 front-to-back）。**已做否定测试**
  （去掉它 → 断言必红），确认 load-bearing。
- **偏好存档 schema 门**：偏好是**逐键覆盖** → **老存档会把新默认顶回**（改默认必踩）。
  修法 = 新增 **`kPrefsVersion=2`**（与布局的 `kFormatVersion` **两套独立门**）；老版本整份作废回出厂默认。
- **ImGui 窗口名兼作 ID** → 翻译文案会改 ID 丢布局 → 必用 `trId(name, id)`。
- **太阳懒创建的可见性缺口**：`Scene::light()` 只加 `NameComponent`+`DirectionalLightComponent`，
  **没有 `VisibilityComponent`**（对比 `Scene::createObject`）→ 补齐 + `sunVisible()` 语义。
- **胶囊拟合立方体必然得球**：`r=√2/2·half`、`halfHeight=0` —— **不是 bug**，已钉成用例期望。
- **sRGB 二次编码对像素判据的影响**：上屏实测亮度 **深色 85~132 / 浅色 225~245** →
  阈值必须按实测定，不能按理论 sRGB 亮度。

### 13.2 ✅ 定性为**既有缺陷**：`verify_collider_show` 栅格撞色

断言"关线框后像素消失"报 `pixels=176>40` FAIL。用**改动前**旧栅格参数复跑**数字一样 176** →
与本轮无关。真因 = 判据矩形外扩 8px 框进了**地面栅格线**，其色在 ±40 容差内**与线框青色撞色** →
给该用例 env 加 **`MYVK_GRID=0`**（附注释）→ 复跑 PASS（3774→0→3062）。

### 13.3 回归注册（原子同步完成）

`run_regression.py` 的 `DEFAULT` **+5 例**、`TRIGGERS` **+4 条**（`verify_startup_show` **故意不打**
映射 —— 它全依赖 `engine/core/**` = `GLOBAL_GLOBS`，按"全量+点名"铁律处理）；
`check_regression_map.py` → **PASS（48/56 已登记）**。

