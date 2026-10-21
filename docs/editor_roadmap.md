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

#### 2026-10-17 布局改造：底栏 + Content 抽屉（用户需求）

> 原话："原本的 Content 面板移除掉，把 `7 entries | assets` 那部分位置和底部栏的
> 动画设置部分合并成一行，动画那个部分的操作 UI 全部移除。这一栏左侧放一个内容
> 侧滑菜单（前面有 Logo），紧接着是输出日志（同样带 Logo），再往右是 CMD。
> 点内容时 Content 面板从底部栏往上弹出，可拖拽高度；右上角有'停靠在布局中'选项，
> 勾选就固定，默认不勾；不勾时点面板外区域直接关闭。"

**落地形态**

| 位置 | 内容 |
|---|---|
| 左栏 | 只剩 Hierarchy（Content 不再占分栏） |
| 视口 | 一直延伸到主体底部（原视口下方播放条已删除） |
| 底栏（32px 通栏，状态栏上方） | `[📁] Content` `[📄] Output Log` `[⌨] CMD` …… 右侧 `N entries \| assets` |
| Content 抽屉 | 从底栏上沿往上弹，默认高 320px，横跨整窗；浮层或停靠两种形态 |

**关键实现点（改之前必读）**

1. `SplitLayout::syncPanelVisibility()` 里 `kSlotContent` **恒 false** —— 左栏下半
   收成 0，Hierarchy 吃满。`kSlotContent` 槽位保留在类型里（删掉会掉进
   `default: return true`，反而留一块空）。
2. `SplitLayout::setBottomInset(h)` = 主体底部让出的总高度（底栏 + 停靠抽屉）。
   ⚠ 它**不等于底栏高度** —— 一开始直接拿它当 `L.bottom.h` 用，底栏被撑成
   352px、内容画在顶部 32px 其余全白。`L.bottom` 现在由 EditorApp 显式写。
3. 抽屉是 **ImGui 无标题栏窗口**：标题行、`Dock in layout`、关闭按钮、上边缘
   拖拽把手全在 `EditorApp::drawContentDrawer()` 里画。`ContentBrowser` 只提供
   `drawBody()`（导航行 / 工具行 / 网格或列表 / 右键菜单 / 内联重命名）。
4. **点外部关闭**：`serviceContentDrawerDismiss()` 在所有窗口画完之后判定。
   ⚠ 两个坑：
   - `IsWindowHovered()` 默认**不**把"有 active item"算作悬停 —— 点击底栏按钮的
     松开那一帧它会返回 false，于是"点 Content 打开抽屉"同帧被当成"点了外部"关掉。
     必须加 `ImGuiHoveredFlags_AllowWhenBlockedByActiveItem`。
   - 判定必须排除"鼠标在底栏上"，否则按钮那一帧同样自相矛盾。
5. **默认关着**：`PanelDesc["content"].defaultOpen = false`，并且
   `Workspace.cpp` 的 `kLevelOpen` / `kCharOpen` 里删掉了 `content`
   （工作区预设会强制覆盖面板开关）。
6. 自动化钩子：`MYVK_EDITOR_CONTENT_DRAWER=1` 开抽屉、`MYVK_EDITOR_CONTENT_DOCK=1`
   开抽屉 + 勾停靠；`MYVK_CONTENT_DIR` 也算"要打开"（CB-ENTRIES / 拖拽那一族
   脚本本来就假定内容面板可见）。
7. 播放条（`LayoutRects::transport`）**矩形恒为 0×0**，`drawTransportBar()` 保留
   实现但不再被调用（用户要求"先全部移除"）。`kTransportH` 常量留着是为了
   `verify_add_menu` 之类的历史引用能编译。
8. 日志锚点：`BAR-BOTTOM` / `BAR-BTN-content|log|cmd` / `BAR-STATS` /
   `CONTENT-DRAWER` / `CONTENT-DRAWER-GRIP` / `CONTENT-DRAWER-DOCK` /
   `CONSOLE-PANEL`，另有 `CONTENT-DRAWER open=|closed=outside|dock=` 事件行。

**验收**：`tools/verify_bottom_bar.py`（25 项：形状 / 开合 / 点外部 / 停靠 /
拖拽高度 / 输出日志 / CMD / 零 VUID）。回归脚本 `verify_add_menu.py` 的第 1 组
断言已从 `TR-BAR` 改成 `BAR-BOTTOM`（原来校验播放条形状，现在校验底栏形状）。

#### 2026-10-17 晚 抽屉层级 + 分割线不渗透 + Content 三栏

> 用户反馈（原话）："底部的 Content 面板打开时从层级来说应该是覆盖其他面板
> 区域上的，Content 打开时 Hierarchy 和 Viewport 之间的分割线不应该渗透进来。
> Content 面板的结构需要改进一下：分左中右三个区域，左侧是当前游戏项目文件
> 目录列表（根目录是当前游戏项目的文件目录），中间是竖直一列资源类型单选框，
> 右侧是当前选中目录的具体内容，右上端有搜索框（能记录最近搜索），搜索框右侧
> 是排序 logo，点开有子菜单：名称/大小 + 升序/降序（都是勾选框）。"

**① 层级：抽屉必须稳定在上面**

ImGui 的绘制顺序是 `g.Windows` 的顺序，而**点中某个窗口会把它挪到顺序末尾**
（`FocusWindow → BringWindowToDisplayFront`）。所以"后画的在上层"只在同一帧内
成立 —— 点一下视口，视口就跑到抽屉后面去了；更常见的是"关掉抽屉再点 Content
重开"：那一帧视口正是最后被聚焦的窗口，于是抽屉上半截被视口盖住。

修法：`drawContentDrawer()` 末尾每帧把抽屉重新提到最前
（`imgui_internal.h` 的 `BringWindowToDisplayFront`）。两条保护：
- **有弹出层时不动**（`IsPopupOpen(nullptr, AnyPopupId|AnyPopupLevel)`）——
  右键菜单 / 排序菜单 / 下拉都在同一个列表里，硬提前会把它们压在抽屉下面；
- 停靠形态不提（它已经是布局的一部分，提上去会盖住底栏）。

顺带修掉一个**"点菜单顺带关抽屉"**：菜单盖在抽屉上方时 `g.HoveredWindow`
是那个 popup，`IsWindowHovered(抽屉)` 于是返回 false —— 于是"菜单开着时点抽屉
里别处"既关菜单、又把整个抽屉关掉。判定要用**上一帧的弹层快照**
（`m_contentPopupWasOpen`）：ImGui 在 `NewFrame` 里就把弹层关了，等到帧末
`IsPopupOpen` 已经 false，只看当前帧状态抓不到。IDE 行为是"第一下只关菜单"。

**② 分割线不渗透**

分隔条画在 `GetForegroundDrawList()`（压在所有面板之上），而抽屉是浮层、
不在分栏树里 → `Hierarchy | Viewport` 那条竖线一路画进抽屉内部。

修法：给 `SplitLayout` 一个**遮挡矩形**（`setOccluder`，浮层形态时 = 抽屉矩形），
把分隔条切成 0~2 段：
- 画只画可见段；命中判定也只用可见段（否则在抽屉里拖高度会误拖分栏）；
- 整条被盖住时连 `logRect` 都不打，并顺手清掉 `m_dragNode`（不清的话拖拽卡住）；
- 停靠形态不用管（`bottomInset` 已把分隔条下端收到抽屉上沿）。
回归锚点 `SPLIT-0/2` 记的是**第一段**（露出来的那段），拖拽脚本点矩形中心照旧有效。

**③ Content 三栏**

| 栏 | 内容 | 锚点 |
|---|---|---|
| 左（~18%，132~250px） | `↑ ⌂ ⟳ +` 四个图标 + **项目目录树**（根 = `assets/`，递归、展开状态跟节点走、点节点切目录） | `CB-TREE` / `CB-TREE-ROOT` / `CB-TREE-NODE <name>` |
| 中（~11%，96~140px） | **资源类型单选竖列**（All/Models/Textures/Materials/Scenes/Scripts/Shaders/Folders） | `CB-TYPES` / `CB-TYPE <name>` |
| 右（其余全部） | `搜索框 [排序] [网格][列表][S/M/L/XL]` + 网格或列表 | `CB-ITEMS` / `CB-SEARCH` / `CB-SORT` |

- 原来的**面包屑**整行删掉（树本身就把路径显示出来了），**工具栏**的三件东西
  拆到"中栏（类型过滤）/ 右栏顶行（搜索、排序、视图、尺寸）"。
- 排序菜单用**勾选框**但语义是二选一：点已勾中的那一项不会把它取消
  （否则会出现"既不按名称也不按大小"这种无意义状态）。目录**永远排文件前面**，
  与排序字段无关。
- 最近搜索只在**回车**（或从历史里选）时记录，边打边过滤不入历史
  （每敲一个字存一条会把列表刷满无用前缀）。空着点进搜索框就把历史摊开。
- 目录树有缓存（`m_dirCache`，`refresh()` 时清空），每帧递归扫盘会拖慢 UI。
- 网格/列表里原来给"底部统计行"预留的 `-footerH` 去掉 —— 那行文字早已搬到
  底栏，留着等于白扔一行高度（正好也是 `assets/models` 第三行露不出来的原因）。
- 自动化钩子：`MYVK_EDITOR_CB_HIST="a,b"` 预置最近搜索；`CB-ENTRIES` 日志后面
  追加了 `sort=` / `asc=` 两个字段（**前 5 个字段的顺序不能动**，脚本按位置取）。

**验收**：`tools/verify_content_three_pane.py`（32 项：三栏结构 / 层级重开 /
分割线裁切 / 树切目录 / 单选 / 排序四项 / 最近搜索 / 点菜单不连带关抽屉 / 零 VUID）。

#### 2026-10-17 深夜 Content 右栏顶部的"动作条"

> 用户反馈（原话）："Content 面板中右侧那一部分的搜索栏的上边加一些东西，
> 分别是几个按钮，一个是添加按钮，按钮是图标加文字一起的，添加按钮点击后会在
> 该按钮的上侧的位置打开一个子菜单，内容有：材质，模型，场景，动画，过场动画，
> 媒体，水，纹理，物理，音频，植被，用户界面，世界地图，还有导入按钮，点击导入
> 按钮会打开本地资源管理器选择资源进行导入，还有个保存所有的按钮，保存所有按钮
> 的右侧是两个小按钮分别是去上一层目录和下一层目录，再右边是一个路径框，里面会
> 显示当前项目文件资源的路径，即相对当前游戏项目的根目录的相对路径。"

在 `drawSearchRow()` **之前**插一行 `drawActionRow()`：

```
[ + Add ▾ ]  [ ▣ Save All ]  [ ‹ ] [ › ]  [ /assets/models .................. ]
```

| 控件 | 行为 | 锚点 |
|---|---|---|
| `+ Add ▾` | 图标 + 文字，点开**向上**弹的子菜单（13 种资源类型 + 导入） | `CB-ADD-BTN` / `CB-ADDITEM <key>` |
| `▣ Save All` | 置位 `EditorContext::saveAllRequest()`，由 EditorApp 落盘 | `CB-SAVEALL` |
| `‹ ›` | 沿**浏览历史**往返（不是父子层级 —— 那是左栏的 `↑`） | `CB-PREV` / `CB-NEXT` |
| 路径框 | 只读 `InputText`，显示相对项目根的路径，可选中复制 | `CB-PATH` / `CB-PATH rel=...` |

**几个必须记住的点：**

1. **子菜单要自己算位置**。ImGui 默认把 popup 放在锚点**下方**，而抽屉贴着屏幕
   底边 —— 往下弹直接出屏幕。`drawAddMenu()` 在 `BeginPopup` 之前
   `SetNextWindowPos(按钮左上.x, 按钮顶 - 估高 - 4, Always)`。
   高度只能**估**（`BeginPopup` 之前窗口没排版）：`rows × (行高 + ItemSpacing)
   + Separator + WindowPadding×2`，误差几像素，而菜单周围没有东西跟它对齐。
2. **`‹ ›` 是历史导航，不是父子层级**。前者的语义是"刚才看过什么"，后者是
   "上/下一层目录"——后者不成立（没有"选中的子目录"这个概念），而左栏已经有一个
   `↑` 走父子层级了。画成 `‹ ›` 而不是 `↑ ↓`：两个长得一样的箭头做不同的事是
   最糟的设计。`Alt+←/→` 同款。
3. **`navigateTo()` 里 push 顺序反了**（这次顺着 `‹` 按钮才暴露出来）：
   原来先 `m_rel = rel` 再 `pushHistory(rel)`，于是 `pushHistory` 的首次调用
   （它就是拿新目录初始化栈的那一次）把初始目录**吞了** —— 历史里只有
   `assets/scenes`，`‹` 从一启动就是灰的，要切两次才亮。改成先 push 当前、
   再 push 目标。
4. **导入必须走系统对话框**（`GetOpenFileNameW`），理由不是省事：用户要的是
   "本地资源管理器"，那意味着快速访问 / 网络位置 / 预览 / 最近文件夹，自研一个
   一条都比不了。
   - **`OFN_NOCHANGEDIR` 不能省**：不带它，对话框会把**进程当前目录**改成用户最后
     浏览的文件夹。而这套引擎的资源查找、`m_root`、`m_rel` 全建立在"工作目录 =
     项目根"上 —— 一导入就全盘错位，下次启动还开在那个目录。
   - 模态阻塞 → 点击只置 `m_importRequest`，真正的调用放在 `drawBody()` **帧末**
     （所有 ImGui 控件都提交完之后）。在遍历控件途中卡进 Win32 消息循环，本帧的
     顶点缓冲会对不上。
   - 多选返回格式是 `"<目录>\0文件1\0文件2\0\0"`，单选是 `"<完整路径>\0\0"`；
     **不能用 `std::wstring(buf.data())` 解析** —— 它遇到第一个 `\0` 就截断了，
     后面那段文件列表直接丢掉。要手动按 `\0` 走指针。
   - 同名不覆盖：走 `uniqueChildPath()` 自动加编号。
   - 需要显式链接 `comdlg32`（CMake 里 `if(WIN32) target_link_libraries(... comdlg32)`），
     否则 LNK2019 `__imp_GetOpenFileNameW`。
5. **Save All 不在面板里存**。面板只置位，落盘在 EditorApp（它才知道
   `scenePath()`，也才拿得到 `InputMap`）—— 顺手把 Ctrl+S 那段抽成
   `EditorApp::saveSceneNow()`，两个入口共用一条路径。面板里复制一份保存逻辑
   必然两边漂移。
6. **窄面板三级降级**（路径框是这一行信息量最大的东西，永远不牺牲它）：
   先摘掉 "Save All" 的文字 → 再整块拿掉 Save All → 路径框最小值 96 px。
7. 动作行的两个按钮是 `iconTextButton()`（`InvisibleButton` 拿命中 + DrawList
   自绘框/图标/文字），因为 `ImGui::Button` 画不了图标；宽度必须用
   `iconTextWidth()` **先算再画**，否则路径框会压在按钮身上。
8. 译文表里 **`Cinematic` 这个键已经被 Workspace 菜单占成"镜头"**（`unordered_map`
   是 `emplace`，先到先得），所以菜单项另开一个键 `"Cinematic Sequence"` = 过场动画。

**验收**：`tools/verify_content_action_row.py`（26 项：动作行位置顺序 / 菜单在按钮
上侧 / 13 项齐全且顺序对 / Add▸Material 真落盘 / ‹›历史与路径联动 / Save All
被接走 / 导入走到对话框那一步 / 零 VUID）。

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

#### 2026-10-17 栅格俯视抖动修复（`assets/shaders/grid.frag`）

> 用户反馈："3D 视图窗口中，当视野从高处俯视地面的时候，地面的栅格画面出现了
> 频繁的抖动现象。"

**量出来的事实**（不是推测）：

| 场景 | 同机位两次启动 | 机位移动 0.3 m（≈1.3 px） |
|---|---|---|
| 俯视 120 m | mean = **0.000000** | mean = **24.49**，n>18 = **221 194**（占视口 40%） |
| 俯视 400 m | 0.000000 | 4.26 / 45 978 |
| 贴地 1.2 m 平视 | 0.000000 | 3.08 / 23 025 |

→ 渲染本身是**完全确定**的（同一机位两次启动逐像素相同，TAA 不是元凶）；
抖动来自**空间走样**：120 m 高空时 1 m 小格只占 ~4.3 px，而线宽 1.4 px ——
一张周期 4 像素的高频图案，相机一动相位就整片重排。

**修法**（与 UE5 视口栅格同一套 "按像素密度收 LOD"）：

```glsl
const float kMinCellPx  = 5.0;    // 原来是 0.9
const float kFullCellPx = 12.0;   // 原来是 2.0
float gridCoverage(...) {
    const float lod = smoothstep(kMinCellPx, kFullCellPx, cellPx);
    if (lod <= 0.0) return 0.0;   // 整族淡出：这条连 fwidth 都不必算
    ...
}
```

再补一族 **100 m 超级格**（`majorStep * 10`）：LOD 是"逐级收掉"的，相机升到
400 m 以上时 10 m 格也掉到 5 px 以下，没有更粗的一级就会变成**一片空白**。

**复测**：120 m 俯视 24.49 → **7.14**（差异像素 221k → 53k，且差异图里只剩
沿格线的规则细带 —— 那是"线条整体平移 1px"的必然结果，不是高频噪点）；
贴地平视 3.08 → 3.06（不受影响，符合预期）；同机位重复启动仍是 0.000000。

#### 2026-10-17 晚 栅格"虚化 / 一片一片看不见" —— 共面深度冲突

> 用户反馈："3D 视口的地面栅格经常性的看到会出现虚化或部分不可见的状态，
> 请找出原因并解决。"

**根因不在格线代码里，在深度上。** 栅格与地板都在 `z = 0`，谁在前面完全由
浮点误差决定；原来只在**顶点**里做 `clip.z -= 1e-6 * clip.w`（NDC 上恒定
减 1e-6）。但这个四边形横跨 ±2000 m，光栅化器要在 4 km 跨度上插值
`clip.z`，插值误差与那 1e-6 是**同一量级** → 地板范围内时有时无。

判定它的决定性证据（不需要拆代码）：**同一场景里，地板多边形覆盖的像素上
格线消失，出了地板边界（深色背景）格线立刻出现** —— 1 m 小格顺着
"40 m 地板的边缘"整齐地开始，这不是 LOD（LOD 是连续淡出），是遮挡。

| 偏移量（顶点级） | 俯视 40 m / 45° 时地板内的格线 |
|---|---|
| `1e-6`（原值） | **完全没有**（p98=175 == 地板本色 167） |
| `1e-3`（实验） | 全部出现（p98=208）→ 确认是深度 |

但 `1e-3` 在 20 m 处等于把栅格抬高 4 m，旁边物体会穿模 —— 不能这么修。

**最终修法：把深度算到片元里。**

```glsl
// grid.frag
layout(set = 0, binding = 0) uniform FrameUBO { ... } ubo;  // 与 VS 逐字段一致
const float kDepthBias = 1e-5;
...
const vec4 clip = ubo.proj * ubo.view * vec4(vWorld, 1.0);
gl_FragDepth = clamp(clip.z / clip.w - kDepthBias, 0.0, 1.0);
```

- `vWorld` 本身就是"这个像素对应的地面点"（毫米级误差），在这里重算
  proj*view 得到的是**精确** NDC 深度，`1e-5` 就有 ~100 倍余量；
- `1e-5` 换算成世界距离是 4 cm@20 m、4 m@200 m —— 只是深度上"更靠前一点"，
  肉眼看不到位移；
- 代价是这条管线失去 early-Z。栅格本来就排在不透明几何之后、`depthWrite=false`，
  可以忽略。`grid.vert` 里那行偏移随之删掉（并留了注释说明为什么不能留在顶点里）。

**复测**（6 个机位截图 + 像素统计）：地板内 p98 从 175（= 没线）回到 **206.7**；
`assets/models` 等旧用例不受影响（栅格默认关在像素断言里）。回归地图里给
`verify_fxaa` / `verify_local_shadow` 补上了 `grid.*` 触发器 —— 它们的视口
截图里一直有栅格，只是以前被深度冲突吃掉所以依赖是隐形的。

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

### E26 · 画质档位（Quality Settings 面板）✅ 已完成（2026-10-19）

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
| E26 画质档位 | **✅ 已完成（2026-10-19）** | 见本文件 §17 的落地记录 |
| E27 RenderGraph 可视化 | 待做 | ⚠ **准入契约未满足**：依赖渲染侧 **R7-1「声明式 RenderGraph」**，`docs/render_roadmap.md` 里它还在阶段 7（未做）。现有 `render/FrameGraph` 只覆盖"前向之后"的 5 个 pass，不是全管线 |
| E28 打包 / 发布 UI | 待做 | 依赖 E21 / E13（**均已就绪**）—— L 级，需动 `CMakeLists.txt` 加 `VENN_SHIPPING` 宏 |
| E29 烘焙流水线 UI | 待做 | 🟡 **准入契约部分满足（2026-10-15）**：**R5-1 反射探针已落地** ✅（`engine/render/ReflectionProbe.*` + `Inspector ▸ Reflection Probe` 属性面板 + `Capture` 按钮），但 **R5-2 lightmap 仍未落地** ✗ —— 面板的"光照贴图缩略图"那一半还是空的。可先做反射探针那一路（设置档 + 进度条 + 探针预览球），光照贴图那一路等 R5-2 |

**顺手补的欠账**：`verify_gpu_timeline` 已进 `run_regression.py` 的
`DEFAULT` + `TRIGGERS`（`DEFAULT` 现 **46 例**；`check_regression_map.py`
**PASS**：40 例已登记 / 46 例，6 例按设计无条件重跑；无死映射、无全横切登记）。
`check_feature_surface.py` **PASS**（32/32 特性有 UI 面、裸 env 漏网 0）。

> 上面这组数字是 **E25 当轮**的口径；2026-10-19 复跑为 **79 例已登记 / 86 例**、
> **35/35 有 UI 面 / 裸 env 0**（新增 `verify_quality_presets` 等）。

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
| R6-4 地形 ✅(2026-10-20) | 地形雕刻 ✅ + splat 图层 ✅ + **笔划撤销/重做** ✅ + **6 个笔刷** ✅ + **滚轮调参** ✅ + **整块操作** ✅。落地形态：顶栏「编辑模式」下拉（UE Selection Mode 位）→ 选 Landscape 就开地形编辑器，选 Foliage 就开植被编辑器。**参数**：抬高/降低 · 平滑 · 压平 · 噪声（特征尺度 / 倍频数 / 图案种子）· 侵蚀（休止角 / 每帧迭代）· 层绘制（4 层 + Erase），共性参数 半径 / 强度 / 衰减 / **单笔最大位移**。**操作**：笔划插值（快速拖动不再是一串断开的圆点）、滚轮调半径（Shift = 强度）、Ctrl+Z/Y 笔划级撤销重做（脏矩形压缩，深度 24）、三档整块操作（全图平滑 / 全图加噪 / 重置为默认地貌）、笔刷光标带填充 + 衰减内圈 + 笔心十字且按工具换色。**仍缺**：① splat 的独立"层刷子可视化"（只有面板里 4 个层按钮，没有 UE 那种 Layers 缩略图列表）；② 笔刷形状只有圆形 + falloff，没有 UE 的方刷/纹理压印；③ 撤销栈只覆盖笔划，新建/重置这类"资产级"操作不进栈。 | E24 + E08 |
| R6-3 植被 ✅(2026-10-20) | 撒点编辑器 ✅ —— **5 个物种**（草 0.55 m / 灌木 1.1 m / 树 5 m / 花 0.35 m / 岩石 1.4 m，各有独立程序化网格与序列化分支）+ 半径 / 每笔株数 / 缩放区间 / 随机倾斜 / 贴合坡度 / 边缘密度衰减 / 擦除（默认只删当前种类，可切"擦所有种类"）/ 清空 / 上限 **6000**。**逐实例随机化**：5 档亮度变体材质（只动亮度与少量冷暖，避免"紫草粉树"）+ 倾斜抖动（按地表法线算 rx/ry 再加抖）+ 岩石额外随机翻滚 + 按形状区分下沉量（球/石沉 30%，竖直生长类只沉 2%）。**放置规则**：坡度上限 + 高度带上下限（逐物种不同：岩石 70°、树 26°），日志给出 `skippedSlope` / `skippedHeight` 让"没撒上"可诊断；滚轮调半径、Shift+滚轮调每笔株数。视口地表圆环光标 ✅。**仍缺**：① 风场（W5-2 的 `foliage.vert` 顶点摆动没做，现在植被是普通不透明实体）；② 1 万株级别的 instanced 专用管线（复用已有同网格合批，6000 是合批上限的安全线）；③ 泊松盘分布（现在是极坐标均匀采样 + `pow(u,·)` 边缘衰减，密堆时仍有轻微聚集）。 | E09（登记即得）            |
| R6-6 编辑模式全套 ✅(2026-10-20) | 顶栏「编辑模式」下拉的 **9 项全部落地**（之前只有 Select / Landscape / Foliage 可用，其余 6 项是灰的 TODO）。新增 `editor/ToolkitEditors.{h,cpp}` 六个编辑器：**Mesh Paint**（顶点色绘制，顶点色已接到 pbr 着色器与 LOD 合并）· **Modeling**（**Blender Edit Mode 式**：顶点 / 边 / 面三级选择，视口射线点选（按屏幕距离挑最近的顶点或边）+ Shift 加选 / Ctrl 减选 + 键盘 `1`/`2`/`3` 切层 + `A` 全选 / `Alt+A` 清空；选区操作 挤出 / 细分选中 / 翻转 / 删除 / 合并到中心，叠加 `G`/`S`/`R` 模态拖拽（左键确认、右键或 Esc 取消）；整网格 平滑 / 细分 / 沿 X 镜像。改网格 = 换 Mesh 指针，全部走 `structuralEdit` 可撤销）· **Fracture**（**Voronoi 真断裂**：撒种子 → 逐三角形用"种子与邻居的中垂面"做 3D Sutherland-Hodgman 裁剪 → 扇形三角化 → 断面按 2D 凸包封口，切面朝向任意；保留 Grid 切法可对比；可选刚体，源物体隐藏而非删除）· **Brush Editing**（box / cylinder / sphere 在 z=0 平面盖章 + 栅格吸附 + 碰撞体）· **Animation**（transform 关键帧时间轴 + 循环播放 + **关键帧随场景序列化**：新增 `AnimationClipComponent`，写进 `scene.json` 的 `animClip` 段，存盘重开还在；播放头 `time` 存盘、`playing` 由 `autoplay` 决定）· **PCG**（抖动网格采样 + 坡度/高度带过滤 + 对齐法线，输出挂在 "PCG Output" 分组下）。切模式由 `EditorApp::serviceEditMode` 做差量：开新面板 / 关旧面板 / sync / focus，日志 `EDITMODE-PANELS mode=... panel=... open=N`。**已补齐（2026-10-20 夜）**：① 关键帧进场景序列化（`animClip` 段）；② Fracture 换成 Voronoi 真断裂（不再是轴对齐盒子）；③ Modeling 补上边 / 面选择（Blender Edit Mode 形态）。**仍缺**：① 曲线编辑器（关键帧之间是 smoothstep 插值，没有贝塞尔手柄）；② Fracture 没有跨切面的 UV / 顶点色继承（新顶点直接拷自源三角形）；③ Modeling 没有环切 / 倒角 / 焊接阈值面板。 | E24 + E08 |
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
| **M5 · 生产力工具**  | E07 / E22 / E24 / ✅E25 / ✅E26            | 多视口、资产编辑器、时间轴、画质档位可用                                 |
| **M6 · 3A 收口**  | E08 / E27 / E28 / E29                   | 出包可用、Shipping 剔除生效、烘焙有 UI                            |

**M1 是投入产出比最高的里程碑**——它把"功能有了但人用不了"这个结构性缺陷一次性抹掉。

---

## 附录 · 现状与目标的差距速查（2026-10-14 初测 / 2026-10-15 复核）

**2026-10-15 复核**（阶段 A 地基 + B 布局 + C 面板做完之后重测；"现状"列已是现在的值）：

| 维度       | 现状（2026-10-15）              | 目标                                                                        | 相关任务                              |
| -------- | --------------- | ------------------------------------------------------------------------- | --------------------------------- |
| 面板数      | **12 + 3 条带**（+ **E26 画质档位**；E18 命令面板 / E21 通知历史 / E25 GPU 时间轴 陆续进来） | +2（RenderGraph / Bake） | ✅ E18 / ✅ E21 / ✅ E25 / ✅ E26 / E27 / E29 |
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


---

## 14 · 视口工具条重做 + 面板托盘（2026-10-17 晚）✅ 两条需求全部落地

用户一次性给两条需求（附 6 张 UE5 视口截图）。**新脚本 `verify_viewport_bar.py`
57 项断言全 PASS**；受影响的 6 个既有用例复跑全 PASS。**没有跑全量回归**
（用户指令："只有我给你发送开始全量回归的指令时你才全量回归"）。

| # | 需求（原话要点） | 定论 |
| --- | --- | --- |
| 1 | CMD **右侧**加 `+` 按钮，点开**向上**弹一列**勾选框**，勾上就在 `+` 右边长出一个**带 logo 的**面板按钮；点按钮打开面板；面板可**最小化**；按钮右上角**叉号**可把按钮移除 | 底栏托盘：`drawPanelPicker()` + `trayButton()`；候选**直接遍历 `panelTable()`**（不另造注册表），排除分栏面板与 Content/Console |
| 2 | 3D 视口上方的按钮按截图重做：**选择/移动/旋转/缩放** + 三类**吸附**（移动/网格/旋转/缩放） | `drawToolbar()` 重写成**一行 18 个控件 + 9 个下拉**（旧版两行、900+px 必被裁）；新增 `GizmoMode::Select` |

### 14.1 托盘的三态模型（第一版做错的地方）

用户要"点按钮开面板、点叉号移除**按钮**"—— 两个动作的落点不同。第一版把
"点按钮"直接做成 `setPanelOpen(false)`，结果**按钮跟着消失**，"再点打开"的
入口就没了（自动化 T6 立刻抓到）。现在拆成两个概念：

- `panelOpen(i)` = **勾选状态**（按钮在不在）→ 落盘、Window 菜单、快捷键共用；
- `panelTrayHiddenRef(i)` = **窗口临时收起**（按钮仍在）→ 会话内，存在
  `PanelRegistry`（`g_trayHidden`）而**不是** EditorApp 成员 —— 这样
  `setPanelOpen(i, true)` 能在一处把**所有**打开入口（Window 菜单 / Alt+N /
  命令面板 / 工作区切换）都顺手清掉，不会出现"勾着却按不开"。

### 14.2 这一轮修掉的 5 个真 bug（都是自动化抓出来的）

1. **勾上 Physics/Particles 面板"看不见"** —— 浮动面板被视口盖住。视口一旦被
   点中就被 ImGui 提到 `g.Windows` 末尾；Content 抽屉当年用
   `BringWindowToDisplayFront` 解决过同一个问题，这里复用同一招（`drawPanels`
   里对 `kSlotNone` 的面板提层，有弹层/折叠时不动）。
2. **叉号只会关面板、不移除按钮** —— 命中区只有 12×12，而日志矩形报的是整条
   右侧竖带（14×h），自动化按矩形中心点下去落在小方块外。现在**命中区 = 日志
   矩形 = 绘制区域**（`kTrayRemoveW` 一个常量管三处）。
3. **下拉弹层压住工具条** —— `OpenPopup` 不带定位时弹层在**鼠标位置**展开（正好
   盖住那一行）。新增 `openDropdown()` / `placeDropdown()`：记住刚点的按钮矩形，
   `SetNextWindowPos(..., Appearing)` 摆到它正下方。
4. **坐标系/光照菜单选完不关** —— `RadioButton` 不自己关弹层，弹层挂着把下一次
   点击吃掉（表现为"点了别的按钮没反应"）。补 `CloseCurrentPopup()`。
5. **搜索框宽 1554px 撑爆弹层** —— `SetNextItemWidth(max(230, GetWindowWidth()-16))`
   在弹层首帧拿到的是父窗口宽度，而弹层是 auto-resize 的（内容宽度**决定**窗口
   宽度），从窗口宽度反推内容宽度是死循环。改成固定 `kToolMenuW = 268`。

### 14.3 关键坑（值得记牢）

- **键盘输入必须走 `PostMessage(WM_CHAR)`**：`keybd_event` 在本项目里编辑器收不到
  （`verify_nav_gizmo` 早有记载：GLFW 按**扫描码**查表，扫描码 0 → KEY_UNKNOWN）。
  实测 `type_text("rot")` 用 keybd_event 一行日志都没有，换 WM_CHAR 立刻出
  `VP-TOOLSEARCH q=rot rows=1`。
- **`GizmoMode::Select` 是新增枚举值**：所有 `switch (gizmoMode())` 都要显式写它
  —— 旧代码的 `default:` 是"当 Scale 处理"，放任 Select 落进去会画出一把缩放手柄。
  `GizmoController::update` 在 Select 下必须**在几何计算之前 return**（并清
  `m_hover/m_dragging`），否则会留着上一次的手柄光标。
- **日志 tag 要统一小写词表**：工具条上四个按钮打 `VP-TOOLMODE rotate`，下拉里
  却打 `VP-TOOLMODE Rotate`（用了 label）→ 自动化按小写断言永远失败。
- **`logRect` 按 tag+4px 去抖** → 同一个矩形第二次不会再打，脚本拿到的是首值；
  逐项点击必须**给每个控件打自己的 tag**（`VP-VIEWDIRITEM Top` 等），不能按
  "第几行 + 固定像素"估位（MenuItem 行高随主题/翻译变）。
- **`PANEL-STATE` 日志格式是 `PANEL-STATE <id> open=<0|1> min=<0|1>`** —— 断言
  `"PANEL-STATE physics min=1"` 是另一个（不存在的）字符串。

### 14.4 两张新面板

- `editor/PhysicsPanel.{h,cpp}`：运行态只读快照（8 行 + steps/simTime）+ 场景级
  `scene::PhysicsSettings` 真编辑（World/Timestep/Solver/Sleeping 四段，**只在下一次
  Run 生效**，界面上写明）。
- `editor/ParticlesPanel.{h,cpp}`：发射器清单 + 三个预设（**Fire / Smoke / Sparks**，
  数值有出处：火焰=正重力浮力+加色、烟=长寿命+alpha、火花=真重力+拉伸）+ 全局
  `fx::ParticleSettings`（enabled/intensity/sizeScale/softness/simHz/容量）。

### 14.5 `verify_viewport` 的两处基准更新（**没有重采基线**）

工具条从两行并成一行 → 视口 3D 图像上沿 **184 → 145**：

- `CONVERGED_VP` 改为 `(305,145,1241,778)`（连起 3 次实测稳定；旧的
  `(310,184,1238,776)` 是两行时代的几何）；
- `V3b` 的天空采样框从"绝对坐标 (195,240)"改成"**相对视口上沿** 11~56px"。
  不这么改，工具条一变高变矮，采样点就落到天空竖直渐变别的段落上 —— 实测差
  1.14（阈值 1.0），看着像曝光被改，其实一个像素都没动；按相对深度比 → **0.101**。
- `panel_skeleton` 跳过 `x<10`：那一列是**窗口边框**（130 vs 面板内 93），在判据
  阈值边缘反复横跳（6.1 判否 / 6.2 判是），不是面板分隔线。

---

## 15 · 去重复 UI：视口模式条删除 + Hierarchy 对齐 UE Outliner（2026-10-17 第三轮）✅

用户的判断很干脆：**同一个东西摆两遍就是噪音**。

> 1. 3D 视口左上角的 Lit/Unl/Nrm… 这些在右上角的 Lighting mode 中已经有了，
>    就不再需要，可以移除了。
> 2. Hierarchy 面板中的文字 "drop a model here to…" 这段不要了去掉；搜索框前端
>    应该有个小放大镜图标；"Path: (no selection)" 也不要了；游戏项目列表加两个
>    列头，分别是项目标签、类型。

第二条的目标形态是 UE 的 **Outliner**：`🔍搜索框` 一行 + `👁 Item Label | Type` 列头
一行 + 每行 `名字 …… 类型 👁 🔒`。

### 15.1 视口左上角模式条整条删除

删干净的范围（少删一样就是"删了一半"，旧存档还会把勾选框带回来）：

| 删掉的 | 位置 |
| --- | --- |
| `drawViewModeBar()` 实现 + `kViewModeShort[]` | `editor/ViewportPanel.cpp` |
| 声明 + `m_modeBarRight/m_modeBarBottom` | `editor/ViewportPanel.h` |
| `draw()` 里的调用点与尺寸清零 | `editor/ViewportPanel.cpp` |
| `vp.overlay.modeBar` 偏好键 | `editor/EditorApp.cpp`（`prefTable`） |
| "显示"菜单 + 右键 Display 菜单里的两个勾选框 | `ViewportPanel.cpp` |
| 3 条译文（`View mode bar` ×2、`View mode bar (top-left)`） | `editor/Localization.cpp` |
| `VP-OVERLAY` 第 4 个字段 `modeBar` | 日志格式 + `tools/verify_viewport.py` 的 `OVERLAY_RE/KEYS` |

视图模式**仍然有两个入口**，一个都没少：右上 Lighting 下拉 + `Shift+1..8`
（`EditorApp::handleNavigation`，日志 `VP-MODEBAR key=N mode=xxx` 保留）。

**四角分工现在是**：左上 = 空，右上 = 统计 HUD，左下 = 相机信息 + 书签，
右下 = 导航 gizmo。统计 HUD 因此不再需要"往下躲模式条"的避让计算，
`statsY` 直接取 `vpPos.y + 10`。

### 15.2 Hierarchy 四件套

1. **搜索框放大镜** —— `drawMagnifierIcon()`（一个圆 + 一根斜柄，`AddCircle` +
   `AddLine`），画在 `InputTextWithHint` 的矩形内左侧。**不额外占行、不用
   `SameLine`**：靠临时把 `FramePadding.x` 撑大 `iconS + 6`，让文字起点自己右移，
   图标事后按 `GetItemRectMin()` 补画。
2. **两列列头 `Item Label | Type`** —— 表头底色用 `ImGuiCol_TableHeaderBg`，
   列间一条 `ImGuiCol_Separator` 竖线，底边一条横线，最左边一颗示意用 👁。
3. **每行右侧画类型名** —— `hierTypeLabel()` 映射到 UE 的叫法：
   `DirectionalLight / PointLight / SpotLight / StaticMeshActor / ScriptActor /
   ColliderActor / DecalActor / PlayerStart / Folder / Empty`。
   **`brief=true` 给一层短名兜底**（`DirLight / Mesh / Script / Collider / Decal /
   Player`）：默认 285px 的面板里类型列只分到 77px，`StaticMeshActor` 硬截成
   `StaticM..` 等于没说 → 策略是**全名放不下退短名、短名也放不下才 `..`**。
4. **删两行字** —— `drop a model here to create an entity`（`HIER-RECT drop`
   照旧上报，`verify_asset_drag` 靠它算落点）、以及 `Path: (no selection)`
   那一整行面包屑（`drawBreadcrumb()` 实现 + 调用点 + 头文件声明全删）。

名字后缀从此**只留状态**（`[hidden] / [noshadow] / [locked]`）—— "这是什么"
已经由类型列回答，再在名字后面缀一遍就是第二处重复。

### 15.3 这一轮的坑（都是自动化抓出来的）

1. **面包屑"没有选中就不占高度" → 整棵树随选中状态上下跳一行。**
   我把 `drawBreadcrumb()` 改成"没选中直接 `return`"，于是 `verify_hierarchy_status`
   的 A2/A3 全挂（`hover=0 active=0`，点位到了但按钮收不到 hover）。真因不在
   列头（A/B 把列头 `#if 0` 掉仍然挂），而在**布局抖动**：点行 → 选中 → 面包屑
   出现 → 树往下跳一行 → 鼠标已经不在那一行 → 再点又取消选中 → 又跳回来。
   **最终修法就是用户要的"删掉"**：整行不存在，就不存在"要不要给它留高度"的
   两难。中途试过 `Dummy(GetFrameHeight())` 占位也能 PASS，但那只是把抖动换成
   一条空白。
2. **`ready_needle` 变成永不出现的锚点 → 每个会话白等 25 秒。**
   `verify_viewport.launch()` 默认锚点是 `"VP-RECT modebar="`。模式条删掉后它
   再也不出现。改成 `"VP-OVERLAY master="`；`run_session()` 里那处
   `wait_count(tap, "VP-RECT modebar=", 2)` 同样改成 `"VP-RECT="`（注意
   `VP-RECT stats=` 不含这个子串，不会误命中）。
3. **删功能之后最容易留下的是"永远成立的空断言"。**
   `V1a NO_HUD 总闸是关的` 原来靠"没有 modebar 矩形"判定、`V2b` 里循环断言
   `VP-RECT modebar` 不出现 —— 模式条一删，两条都成了白送 PASS。V1a 改成查总闸
   自己的日志 `VP-OVERLAY master=0`；V2b 只留 `VP-RECT stats`。「模式条不存在」
   这件事由 `V0a` 单独负责（同时查矩形与 `VP-MODEBAR modes=`）。
4. **列头不能用 ImGui 的 `Table` / `Columns`。** 这和 §14.2 第 5 条是同一个病：
   `Columns` 会在列结束前把 `GetContentRegionAvail().x` 固定住，而这个面板的宽度
   还要被搜索框用（`SetNextItemWidth(-1)`）。现在列头是**手画的矩形 + 两段
   `AddText`**，类型列左边界 `m_typeColX` 每帧算一次、存在成员里给树内所有行共用
   —— 行在 `BeginChild` 之内、列头在之外，两边各算各的必然差 8px 内边距
   （有滚动条时差 14px）。
5. **列宽要"名字优先"。** 默认面板宽 285px，三列（名字/类型/两个状态图标）很挤。
   `typeX = max(mn.x + 146, mx.x - iconsW - 92)` 再 `min(.., mx.x - iconsW - 46)`
   保底；行内再补一条策略：**宽度不够先丢 `[hidden]/[locked]` 后缀、保名字**。
6. **截断必须在 UTF-8 字符边界上。** `ImGui::CalcTextSize` 只吃整串，自己二分
   截断时 `(byte & 0xC0) != 0x80` 才是字符起点，否则中文会被砍成半个字形乱码。
   `drawClippedText()` 二分的是**合法截断位置表**，不是字节数。

### 15.4 验证

- `tools/verify_hierarchy_status.py` **7 项全 PASS**（新增 `A0` 列头在首行上方、
  `A0b` 类型列边界落在行内且不压行尾 👁；锚点 `HIER-COLHDR=(x0,y0)-(x1,y1) typeX=<x>`）
- `tools/verify_viewport.py` **41 项全 PASS**（比上轮少 1 项 = 删掉的那条空断言；
  `V0a/V0b` 反向断言"没有模式条"、`V7d` 断言 `vp.overlay.modeBar` 不再落盘）
- `verify_viewport_bar`(55) / `verify_viewmodes`(27) / `verify_nav_gizmo` /
  `verify_prefs` / `verify_gizmo_scale` / `verify_bottom_bar`(25) /
  `verify_content_three_pane`(32) / `verify_command_palette` / `verify_inspector` /
  `verify_add_menu` / `verify_theme_language` / `check_regression_map` /
  `check_feature_surface`(34) —— 全 PASS
- ⚠ `verify_gizmo_scale` 第一次跑 D 组（平面手柄 XY 方片）报 `not grabbed`，
  **重跑即 PASS**（`handle=XY Δscale=(+0.6,+0.6,0)`）。这是鼠标注入在窗口没拿到
  前台时的老 flake，不是回归；`verify_asset_drag` 报 `CB-CELL=None` 同理是环境
  前置（Content 抽屉默认关），与本轮改动无关。
- 按纪律**没跑全量回归**（用户要求：等他发指令）。

## 16 · 顶栏重做 + 面板最小化 + 粒子类型化（2026-10-17 第四轮）✅

用户这一轮提了三件事，三件都落在"看起来像 UE，用起来真的动"这条线上：

> 1. Content 面板中点击 + 打开 character 面板后……右上角的叉号**无法关闭**；
>    Output log 和 CMD 按钮点击后都没有打开各自的面板；通过 + 打开的所有面板，
>    右上角都要有**最小化**按钮，点了要把窗口收回到 + 右侧那个按钮上。
> 2. 编辑器右上方的 1，2V，2H，4……请把这个参数**放到和 Perspective 一起
>    并排放置**，点击后展开下拉勾选列表去设置。原来的 Move（W/E/R）、View：lit、
>    Reload Shaders、Undo、Redo 全都不需要，移除掉后按 UE5 主工具条重做。
> 3. 粒子系统加多个时名字都一样……初始名字应该**和粒子类型有关**并用编号后缀；
>    右键名称要能重命名 / 删除；粒子类型再加落雪 / 下雨 / 闪电并真的实现效果。

### 16.1 底栏：叉号关不掉、按钮打不开、面板不能最小化

| 症状 | 真因 | 修法 |
| --- | --- | --- |
| Character 点 × 关不掉 | 注册表与窗口对象的同步写在 `panelDrawCharacter` 里，而 `drawPanels()` **只在 panelOpen 为真时派发** —— 点 × 后 `m_open=false`、注册表还勾着，下一帧又被 `open()` | 同步搬到 `drawCharacterWindow()`（每帧无条件），并用 `m_characterWasOpen` 区分"用户刚关的"与"刚被勾上的" |
| Output Log 点了像没开 | 面板**真的开了**，只是没设 `SetNextWindowPos/Size`，落在 ImGui 默认 (60,60) 的 440×169 小窗里 | 首次出现贴底栏上方、860×330、横向居中 |
| CMD 点了没反应 | 原来恒 `= true`，第二次点等于没反应 | 改成再点一次收起 |
| 没有最小化按钮 | ImGui 的标题栏是 `Begin()` 内部画完的，没有"往标题栏加控件"的入口 | 所有窗口画完后 `FindWindowByName` + `window->DrawList` 手画 + 手算命中（且要求 `g.HoveredWindow == w`，被压住时点不到才是对的）；位置占关闭 X 左边一格 |

`panelTrayEligible` ≠ `panelFloating`：Console 在底栏已有专属按钮（不该再长一个托盘按钮），但它是浮动窗口、必须有最小化按钮。最小化 = `panelTrayHiddenRef=true`（按钮留着、变暗），再点按钮回来 —— 和右键托盘按钮里的"最小化"是同一条路径。

### 16.2 顶栏按 UE5 重做 + 1/2V/2H/4 挪到视口工具条

新结构只有三段：`Select Mode ▾` │ `+ Add ▾` │（右对齐）`▶ ▶| ⏸ ■ ⋯`。

被删掉的东西**没有丢功能**，只是不再占这条最显眼的横条：Move/Rotate/Scale → 视口工具条四个图标 + `Q/W/E/R`；View: lit → 视口"光照"下拉 + `Shift+1..8`；Reload Shaders → Build 菜单 / `Ctrl+Shift+R`；Undo/Redo → Edit 菜单 / `Ctrl+Z/Y`。

`+ Add` 与视口里的 `Shift+A` 是同一批工厂函数（`createPrimitiveOnGround` /
`createSunlight` / `createDirectionalLight` / `createPlayer`），差别只有落点：视口有鼠标射线，顶栏没有 → 按 2m 间距的环排队摆，免得连点两次叠成"没加上"。

`1 / 2V / 2H / 4` 挪到"透视"右边成了下拉，并且**顺手把这件功能真正接通**：
- 名字上是"分屏布局"，不是分辨率（用户当时以为是分辨率）。下拉里每项都写了是哪种分屏，按钮上仍用用户认得的 `1/2V/2H/4`；
- 之前 `setSubviewCount` 只分配了离屏目标，**编辑器从来没有合成过它们** —— 点了等于没点；
- 现在：主渲染目标只渲染**一格**（分辨率不摊薄），其余格用 `Renderer` 已有的子视口目标，每格一台相机（格 0 = 用户透视相机，格 1..3 = 前 / 顶 / 右），格间 2px 缝；
- 交互（导航 / 手柄 / 点选 / 浮层）**只在左上那一格**，其余是只读预览；
- 切布局的两条入口（工作区 / 下拉）走同一个 `editor::applyViewportLayout()`，子视口数与 `VIEWPORT-LAYOUT` 日志只有一份实现。

### 16.3 粒子：类型化命名 + 右键菜单 + 三个新预设

- 预设 3 → 6：`Fire / Smoke / Sparks / Rain / Snow / Lightning`；
- 新名字 `<预设名> <序号>`（`Rain 1` / `Rain 2`），序号从 **1** 起（与 Hierarchy 的 `uniqueName` 从 2 起刻意不同：这里 1 就是第一个）；
- 行内重命名（记 id 不记下标）、复制（去掉尾部数字再编号：`Rain 1`→`Rain 3`）、删除，全部走 `structuralEdit`（可撤销）；
- 三个新特效的实现要点见 §16.4 第 4/5 条。

### 16.4 这一轮的坑

1. **`InvisibleButton` 什么都不画。** 顶栏播放条第一版只 `PushStyleColor(ImGuiCol_Button, 绿色)` 就以为"运行"会变绿 —— 截图里它是灰的。底色必须自己 `AddRectFilled`；禁用态的图标颜色也要自己压暗（手绘不走 `DisabledAlpha`）。
2. **同名函数被虚函数挡住。** `EditorApp` 有 `applyViewportLayout(int)`（`EditorHost` 那条窄接口），
   所以 `applyViewportLayout(*m_ctx, renderer(), layout)` 会解析到成员函数、报"参数个数不对"。
   写全 `editor::` 才能拿到自由函数。
3. **日志只有一个出口才不会撒谎。** `VP-TOOLMODE` 原来写在"点工具按钮"那个 lambda 里 ——
   从顶栏下拉或快捷键换的模式一行痕迹都没有，自动化无法证明三条入口改的是同一份状态。
   改成 `drawToolbar` 末尾按帧差分，谁改的都留一行。
4. **`setSubviewCount` 缩容必须先 `waitIdle`。** 它是**渲染中途**被调用的，上一次提交的
   命令缓冲可能还在跑，直接析构目标会撞
   `vkFreeDescriptorSets(): can't be called on a VkDescriptorSet that is currently in use`
   （真机验证层实测报出，6 行）。
5. **雪 / 闪电"点了没效果"的两个真因**：雪的 `shapeExtents.z` 只有 0.3（薄板）而 `originZ=6`，
   前几秒雪还在镜头上方 → 给一整块空气 `(7,7,4)`；闪电只靠两发 burst，中间 90% 时间是空屏
   → 改成"常驻电晕 + 周期性大闪"，并且初速不能大（发射轴是 +Z，速度一大整团飞出画面）。
6. **验收脚本自身的三个坑**：托盘按钮的矩形 tag 是 `BAR-TRAY-tray_<id>`（`trayButton` 把已经带
   `tray_` 前缀的 id 又拼了一遍）；`logRect` 按矩形去抖，"面板回到同一位置"不会再打点，
   断言要改成 `PANEL-FOCUS` 这种"每次点都打"的日志；最小化按钮的命中要求
   `g.HoveredWindow == 那个窗口`，所以"先开 Character 再点 Console 的最小化"必然失败 ——
   被盖住了，不是坏了。

### 16.5 验证

- `tools/verify_editor_round4.py` **51 项全 PASS**（A 顶栏结构 / B 选择模式下拉 / C Add 真加物体 /
  D 播放控制含"真的推进一帧" / E 分屏下拉含 quad→single / F 粒子命名+右键菜单+复制 / G 底栏三件事）
- 受影响面复跑：`verify_viewport`(41) / `verify_viewport_bar`(ALL PASS) / `verify_bottom_bar` /
  `verify_hierarchy_status` / `verify_add_menu` —— 全 PASS，零 VUID / ERROR
- ⚠ 首跑 `verify_editor_round4` 有一次 B1~G3 **全挂**（矩形都拿到、点击全无反应），重跑即全 PASS ——
  鼠标注入在窗口没拿到前台时的老 flake，与本轮改动无关
- 按纪律**没跑全量回归**（用户要求：等他发指令）

---

## 17 · E26 画质档位（2026-10-19）✅

> 依赖 E01 / E03 / E09，三条全部就绪之后开工。**这一节是 E26 的落地记录**，
> 与 §6 的原文（需求）对照着看。

### 17.1 交付物

| 文件 | 改动 |
| --- | --- |
| `engine/render/QualityPresets.{h,cpp}` | **扩展**：数值档位表（3 项）+ 平台预设（5 个）+ `presetDiff`（差异预览与一致性检查**共用**）+ `fillLayerForPreset`（落盘唯一入口）+ `runQualitySelfTest`（15 条） |
| `editor/QualitySettingsPanel.{h,cpp}` | **新建**：档位单选 / 平台下拉 / 差异预览 / Apply / Check / Save / 需重启提示 |
| `editor/EditorApp.{h,cpp}` | 面板表加一行（`quality`，Authoring 组，无快捷键）+ draw 薄壳 + onInit 建实例 |
| `engine/project/ProjectDisplay.cpp` | `writeQuality` 与 `describeWrites` 改走 `fillLayerForPreset` —— **启动器与编辑器写的是同一份数据** |
| `runtime/main.cpp` | `--selftest quality` 入口 |
| `engine/i18n/ZhTable.cpp` | +26 条（其中 5 条是格式串） |
| `tools/verify_quality_presets.py` | **新建**，20 条断言（A 4 + B 14 + C 2）+ A 组内部 15 条自检 —— 见 §17.4 |

### 17.2 三个刻意的设计（都写进了代码注释）

1. **选择与应用分开**。点档位只改"预览目标"，不立刻改参数 —— 否则差异预览
   恒为 0，用户永远看不到"切到 Low 会关掉什么"。
2. **差异与一致性检查共用 `presetDiff`**。两处各写一套比较逻辑必然漂移，
   而漂移的表现是"两边都说通过、只有画面不对"。
3. **落盘只写值，档位名只是标签**。`config/render.json` 里没有 `quality`
   键 —— 面板重开时用 `presetDiff` **从实际值反推**是哪一档（差 0 项即命中），
   反推不出来就显示 Custom。这与启动器 M4 解掉 `game.json.quality` 空挂
   时的判断完全一致：**值才是权威**，标签是显示。

### 17.3 数值档位表（只挑"当帧生效 + 明确的质量/性能权衡"）

| 参数 | Low | Medium | High | Epic | Cinematic |
| --- | --- | --- | --- | --- | --- |
| `shadow.cascades` | 1 | 2 | 3 | 4 | 4 |
| `shadow.distance` | 60 | 80 | **90** | 120 | 150 |
| `lshadow.maxLights` | 1 | 2 | 4 | 8 | 12 |

- **High 的 `shadow.distance` 必须是 90**（引擎默认值）—— 否则"默认工程点一下
  High，画面就变了"。这条有自检（Q11）盯着。
- **刻意不做**的两类：① 改了要重建资源的（`shadow.resolution` /
  `lshadow.resolution`）—— 切档后必须重启，放进表里等于在面板上撒谎；
  面板底部如实列出它们。② 美术意图参数（`ssao.radius` / `vfog.albedo`）——
  那是"风格"不是"质量"。
- 平台预设 = 档位别名（PC High→High / PC Low→Low / Game Console→Epic /
  Mobile→Low / VR→High），**不引入第二套数值表**。

### 17.4 验收（`tools/verify_quality_presets.py`，20 条全 PASS）

```
A 组（--selftest quality）
  A1 退出码 0 · A2 15 条全过 · A3 failures=0
  A4 表规模 presets=5 values=3 platforms=5 enable=22
B 组（GUI，第一次启动）
  B1 面板 rect=(448,108)-(1008,628)
  B2 5 个档位锚点都在面板内（inside=[T,T,T,T,T]）
  B3 点 Low → QUALITY-UI select preset=Low
  B4 差异预览出现（QUAL-DIFF-0）
  B5 选档没有立刻改参数（snapshot=0）
  B6 Apply 生效
  B7 preset=Low ssao=0 ssr=0 taa=0        ← **验收原文：SSR 生效值为 0**
  B8 cascades=1 dist=60 lights=1
  B9 应用后自检 mismatches=0
  B10 preset=Epic ssao=1 ssr=1 taa=1 cascades=4
  B11 Save 生效
  B12 落盘 25 个键（22 开关 + 3 数值）
  B13 数值项与 Epic 定义一致（4 / 120 / 8）
  B14 Epic 开关（ssao/decals 开、dof 关）
C 组（GUI，重开工程）
  C2 RENDER-SET chain=project=25 total=25
  C3 读回后自检 preset=Epic mismatches=0
结果: PASS
```

### 17.5 这一轮踩到的两个坑（都是"锚点有值但点不到"那一族）

| 现象 | 根因 | 修法 |
| --- | --- | --- |
| **B2 红**：第 5 个档位锚点（`QUAL-PRESET-4`）在面板右缘之外 | 5 个单选按钮一行放不下（英文 "Low quality"+"Medium quality"+… ≈ 550 px > 内容区 544 px）。**被 ImGui 裁掉的控件 `GetItemRectMin/Max` 照样返回真实坐标** → 锚点有值、点下去点到别处 | 档位按钮**每 3 个换一行**（3+2）。断言里加"锚点必须落在 `QUAL-RECT` 内"，把这类问题变成显式判据 |
| **C3 红**：重开工程后自检 `preset=High mismatches=7` | 面板默认 `m_selected = High`，而落盘的是 Epic —— 面板拿"预览目标"当"当前档位" | 首帧用 `presetDiff` **反推**当前档位（5 档各比一次，差 0 项即命中），并且必须在**画单选按钮之前**做。有未绑定参数时一律报 Custom（diff 会跳过未绑定的项，"差 0"可能只是"没比到"） |

### 17.6 "改一处要同步改 N 处"清单（这次的真实账）

1. `ProjectDisplay::writeQuality` 改成调 `fillLayerForPreset` → **键数 22 → 25**
2. → `verify_launcher_create.py` 的 `want = 22` 改 25（注释写清 22+3）
3. → `verify_launcher_display.py` 的 R4 只打印行，未断言数字，**不用改**
4. → `--selftest project` 的 **J11 挂了**（`keys=25 want=22`）—— 它断言的是
   `render::enableParamCount()`，而写盘现在多了 3 个数值项。改成
   `enableParamCount() + qualityValueCount()`。
   ⚠ 这条一开始被我判成"不用改"，实跑才发现 —— **"改一处要同步改 N 处"里的 N
   永远比你以为的多一个**，别靠推理，靠跑。
5. `check_regression_map.py`：`QualityPresets.*` 已在 `RENDER_MODULES`（启动器那轮加的），
   但 `verify_launcher_create` 的 `TRIGGERS` 缺 `ProjectDisplay.*` / `QualityPresets.*` —— 已补
6. i18n：面板里 5 条状态行是裸 `snprintf`（D 组抓 8 条，其中 3 条是锚点 tag）→
   状态行包 `TR`、tag 拼装改成 `std::string`（不经过 `snprintf`）

### 17.7 明确没做（写下来免得下次重启讨论）

- **不改 `editor/FeatureUIGen.cpp` 的顶栏 `Preset ▾`**：它已经调
  `render::applyPreset`，E26 的数值项是**加在 `applyPreset` 里**的，所以顶栏
  切档与面板切档行为逐位相同，不需要第二份 UI。
- **不写 `"quality"` 标签键**（见 §17.2 第 3 条）。
- **不做"自定义档另存为"**：E26 原文提到"可另存为自定义档"，但当前没有
  "命名档位集合"的资产格式（那需要一个新的资产类型）。面板的 Save 已经把
  当前档完整写进 `render.json`，改几个参数后仍然是 Custom —— 语义上等价，
  少一个需要维护的资产格式。

### 17.8 收口复跑（2026-10-19 最后一次全门复核）

| 门 | 结果 |
| --- | --- |
| `Game.exe --selftest quality` | `presets=5 values=3 platforms=5 enable=22` → **15/15 OK，failures=0** |
| `verify_quality_presets.py` | **20/20 PASS**（含 B7 `ssr=0`、B8 `cascades=1 dist=60`、B12 `keys=25`、C3 `preset=Epic mismatches=0`） |
| `Launcher.exe --selftest project` | **112 条 failures=0**（J11 修好后） |
| `verify_launcher_create` / `verify_launcher_project` / `verify_quality_presets` | **3/3 PASS**（34.3s / 9.6s / 49.8s） |
| `verify_launcher_display` | PASS 83.8s |
| `verify_settings_layers` | PASS 53.6s |
| `check_regression_map.py` | PASS（**79 例已登记 / 86 例**，无死映射、无全横切、`RENDER_MODULES` 无缺口） |
| `check_feature_surface.py` | PASS（**35/35 有 UI 面**，`MYVK_*` 263 个，裸 env 漏网 **0**，21 条 `*_DBG` 软警告留 E17） |
| `verify_i18n_coverage.py` | PASS（A1~F 全 0，词典 **1708** 条，重复键 0） |

⚠ 未跑全量：`engine/**` / `editor/**` / `runtime/**` / `launcher/**` / `CMakeLists.txt`
都在 `GLOBAL_GLOBS`（横切），按纪律**等明确下令**再 `--all`。

---

## 18 · 布局重排：模式栏 / 视口标签页 / 右栏上下结构（2026-10-21）✅

> 用户原话三条：
> 1. 创建第三人称模板后，初始视口**朝天看**（朝 +Z）、并且在地面栅格**之下**；
> 2. 所有功能面板（地形 / 粒子 / 物理…）打开时**停靠在 Viewport 上、按标签页切换**，
>    像浏览器那样；
> 3. Hierarchy 挪到右栏 Inspector **上方**，模式类型列表 + 当前模式的参数放**左侧**。
>
> 第 3 条有一处歧义（模式面板放左栏还是视口标签页），问过之后用户选**左栏**：
> 视口标签页只收"工具类面板"，模式面板挂在左栏。

### 18.1 交付物

| 文件 | 改动 |
| --- | --- |
| `editor/PanelDock.{h,cpp}` | **新建**：托管提示的全局单值（`pushDockHint` / `applyDockHint`） |
| `editor/ViewportTabs.{h,cpp}` | **新建**：视口上方 30px 浏览器式标签条 + 当前页状态 |
| `editor/ModeRail.{h,cpp}` | **新建**：左栏 9 行模式单选 + 当前模式参数区矩形 |
| `editor/SplitLayout.cpp` | `buildDefault()` 重写成 `[模式栏 | [视口 | 右栏]]`、`[Hierarchy 上 / Inspector 下]`；`kMinModeRail` |
| `editor/EditorLayoutIO.cpp` | `kFormatVersion` 1 → 2（节点语义变了，旧存档直接当没有） |
| `editor/EditorApp.cpp` | 8 个模式面板 `kSlotNone` → `kSlotModes`；标签条矩形切分；宿主式绘制；`focusPanel` / 托盘恢复时同步当前页 |
| `editor/ToolkitEditors.cpp` | `beginToolkitWindow(ctx, …)` 带 ctx，托管进左栏；Fracture 加 `FR-METHOD set=` 日志 |
| `editor/ViewportPanel.cpp` | 加 `NoTitleBar`（标题条已是标签） |
| `engine/scene/SceneSerializer.cpp` | `applyCamera` 加单位嗅探（度/弧度）+ `SCENE-CAMERA` 落位日志 |
| `tools/gen_templates.py` | 两个模板的 camera 改成**弧度 + 正 pitch + yaw=π** |
| `tools/verify_layout_v4.py` | **新建**，21 条断言（A 4 / B 5 / C 8 / D 4） |

### 18.2 三条需求各自的根因与修法

1. **相机朝天看 + 在地面之下** —— 不是相机代码，是**单位**。模板里写的是
   `pitch=-18.0`（度），`SceneSerializer` 当**弧度**读 → `setPitch` 夹到
   ±`kPitchLimit`(1.5533) → 相机几乎垂直向下俯视目标（等价于"镜头朝天"），
   且距离被压到近平面附近，落在地面栅格以下。修法三层：模板改弧度；
   `applyCamera` 嗅探 `|pitch| > kPitchLimit*1.001` 或显式 `deg` 字段时换算
   （老存档兜底）；补 `SCENE-CAMERA eye=… target=… yaw=… pitch=…` 让"相机在哪"
   可断言。实测 `eye=(-4.23,0,2.74) target=(0,0,1.2) yaw=3.14 pitch=0.35`。
2. **标签页** —— 本工程**不用 ImGui docking 分支**，所以是自绘：`layoutSlot ==
   kSlotNone` 的面板全部变成标签页（新面板自动跟上），标签条占视口顶部 30px，
   `syncViewportTabs()` 负责"刚打开就切过去 / 当前页被关就回 3D 视口"。
   面板宿主化后只画当前页，其余整块不提交。
3. **布局重排** —— `SplitLayout` 仍是一棵 3 节点树，变的是**语义**：
   `[0] 模式栏 | 其余`、`[1] 视口 | 右栏`、`[2] Hierarchy 上 / Inspector 下`。
   工作区预设的 ratio 全部改成 `{0, 0, 0.42}`。

### 18.3 三个踩过的坑（代码里都有注释）

1. **`applyDockHint` 的 `static` 不能放 header**：header-only 里的 `static`
   每个 TU 一份 → 宿主和面板各看各的矩形，表现为"面板钉在一个旧位置"。
   真实的两个函数搬进 `PanelDock.cpp`。
2. **模式列表高度不能写死**：行高是 `kRowH + ItemSpacing.y`，写死常量在字号
   变大后最后两行掉出窗口（列表是 `NoScrollbar`，直接看不见）。现在是函数 +
   `kRowGap` 常量，绘制侧和布局侧**同一份**。
3. **左栏下限是"内容宽度"决定的，不是列表宽度**：建模面板那排
   `Subdivide(150)+Flip(140)+Delete(110)` 是 SameLine 硬排的，320 宽时 Delete
   被窗口裁掉（点都点不到）。下限取 460 = 建模面板的设计宽度。

### 18.4 验收（只跑针对性，未跑全量）

| 门 | 结果 |
| --- | --- |
| `verify_layout_v4.py` | **21/21 PASS**（相机 4 · 模式栏 5 · 标签页 8 · 右栏 4） |
| `verify_toolkit_modes.py` | **34/34 PASS** |
| `verify_toolkit_round2.py` | **32/32 PASS** |
| `verify_i18n_coverage.py` | PASS（A1~F 全 0） |

**参数区比窗口长这件事**：模式面板原是按 620~660 高的浮动窗口写的，塞进
左栏下半（~400px）之后下半截控件在窗口外 —— 面板自带滚动条，真实用户滚轮
就能看到，但两个验收脚本原来是"按锚点矩形直接点"，锚点报的是**未裁剪**的
item 矩形，点到了窗口外面（表现成"点了没反应"）。给两个脚本补了
`reveal()`：目标不在参数区里就滚轮滚过去再点，与用户行为一致。
另外 `C3a` 原来判"切 Grid 后 FR-BREAK 的屏幕 y 变了"，滚到底时删掉一行会被
滚动条 clamp 回来 → 假失败；改成断言面板自己那条 `FR-METHOD set=grid`。

---

## 19 · 顶栏瘦身 + 笔刷落点/实时性修复（2026-10-21 第三轮）✅

> 用户原话两条：
> 1. 编辑器上的原本的模式类型的那个下拉框可以去掉了，因为是冗余的，还有旁边的
>    那个 Add 的列表可以把它的位置调整到**视口上侧工具条左侧的末尾**。
> 2. 编辑器中的地形模式部分，其对于地编方面的操作是有问题的，**鼠标操作的地区
>    和实际生效的位置是有不同的**，另外操作地编时也**不是实时的，延迟非常严重**。

第 1 条是"两处入口指向同一状态"的收口，第 2 条是**两个独立 bug**：一个是坐标系，
一个是 I/O。

### 19.1 交付物

| 文件 | 改动 |
| --- | --- |
| `editor/Toolbar.cpp` | 删掉 `editModeRow()`（模式下拉）与 `drawAddBarMenu()`（+Add）；顶栏只剩右对齐的播放控制 |
| `editor/Toolbar.h` | 删掉两个声明 + `m_addCounter` |
| `editor/ViewportPanel.cpp` | 新增 `kVpIconPlus`；左组末尾放 `+ Add`（复用视口已有的 `##vp_add` 弹层，**不新建第二套菜单**）；右组改成**按预算贪心保留** |
| `editor/ModeRail.cpp` | 每行补 `MODE-RAIL-ITEM <id>` 锚点 —— 顶栏下拉删掉后，脚本切模式的唯一入口 |
| `engine/render/TerrainPass.cpp` | `toRowMajor()` 转置；`refillImage2D` 支持常驻 staging；`refreshSurface` 优先吃内存、退路才读盘；`TERRAIN-REFRESH` 计时日志 |
| `engine/render/TerrainPass.h` | `refreshSurface(..., const terrain::TerrainAsset* live = nullptr)`；`Instance` 加 `uploadBuf` / `refreshCount` / `refreshMaxMs` |
| `engine/ecs/Components.h` | `TerrainComponent` 加 `liveHeights` / `liveSplat` / `liveHoles`（内存直传指针） |
| `engine/render/Renderer.cpp` | `TerrainRefresh` 带上 live 资产；`syncTerrains` 组好之后直传 |
| `editor/LandscapeEditor.{h,cpp}` | `publishToRenderer()` / `clearLiveBinding()` 与 `saveAssets()` 拆开：**拖拽只推内存、抬笔才落盘** |
| `tools/verify_world_tools.py` | 新增 A0 组 6 条断言；修 A1/A1b/A5a/A6a 四处过时断言 |

### 19.2 需求 1：冗余入口收口

顶栏那个"模式类型"下拉和左栏 ModeRail 的 9 行单选写的是**同一个状态**
（`ctx.editMode()`），留着只是让人犹豫该点哪个。删掉之后切换模式的唯一入口
就是左栏，顺带把"切模式"的脚本锚点从 `TB-EDITMODE-ITEM` 换成
`MODE-RAIL-ITEM <id>`（`verify_toolkit_*` / `verify_world_tools` 三份脚本同步）。

`+ Add` 搬到视口工具条左组末尾时踩到一个 ImGui 光标坑：

- 每画完一个 item，`GetCursorPosX()` 会被**收回到行首**；
- `BeginPopup/EndPopup` 还会把光标 x 直接打回 0。

于是"左组末端"不能用 `GetCursorPosX()` 取，必须用
`GetItemRectMax().x - GetWindowPos().x`。取错的表现是右组与左组**重叠**
（`+ Add` 压在 `Perspective` 上，截图里一眼看得出来）。右组同时从
"全有或全无"改成**按预算贪心保留**（7 项按重要性排：投影 > 布局 > 照明 >
显示 > 设置 > 网格 > 相机速度），窗口窄的时候先丢最不重要的。
实测 `VP-BAR-FIT taken=3/7 need=194 budget=205`，`TB-ADD` 结束 1046、
`VP-BAR-RIGHT` 从 1065 开始，不再重叠。

### 19.3 需求 2 之一：鼠标落点 ≠ 生效位置

**根因是存储序，不是射线、不是矩阵。**

| 环节 | 约定 |
| --- | --- |
| `.r32` / `.rgba8` / `.r8` 文件、`physics::Heightfield` | **列主序** `heights[col*rows + row]` |
| Vulkan 纹理 | **行主序** `texel(x,y) = data[y*w + x]` |
| `terrain.vert` | `uv = ((col+0.5)/cols, (row+0.5)/rows)` |

原样上传 = 采样点 `(col,row)` 读到 `h[row*cols + col]` —— 整片地形绕对角线
**镜像**。实测 `hills` 地形（257×257）：

- 峰值 CPU 认为在 `(64, -40)` 米；
- 不转置则画在 `(-40, 64)` 米，**偏差 147 米**；
- 65792/66049 = **99.6% 的采样点数值不同**。

修法：上传前 `toRowMajor()` 翻成行主序，`inst.asset.heights` 仍保持列主序
（下游的物理、笔刷插值全按列主序写，不动它们）。三张图（高度 / 权重 /
挖洞）都过这个转置。

### 19.4 需求 2 之二：不是实时、延迟严重

**根因是每 80 ms 一次"写盘 → 再读回来"。**

`refreshSurface` 原来是"编辑器写盘 → 渲染层 `loadTerrain` 重读 2 MB"，而且
`refillImage2D` 每次还要新建/销毁一个 VkBuffer + 一次 `vkQueueWaitIdle`。
一笔拖 0.5 秒就是十几次磁盘往返，全部算在帧时间里 → 手已经拖过去几十像素，
画面还停在原处。

两处修法：

1. **内存直传**：`TerrainComponent` 挂 `liveHeights/liveSplat/liveHoles`
   三个指针，渲染层直接用编辑器那份 `std::vector`，零 I/O。缺任何一份就
   **整份退回读盘**（部分直传会让三张图来自不同版本，比慢更糟）。
2. **拆开"推内存"与"落盘"**：新 `publishToRenderer()` 只置脏位 + 挂指针，
   拖拽期间走它；`saveAssets()` = 落盘 + publish，只在抬笔
   （`LS-STROKE-END`）/ 撤销 / 重做 / 手动保存时调。常驻 staging buffer
   之后每次刷新只多一次 264 KB 拷贝。

`clearLiveBinding()` 负责在换资产 / 解绑时把指针清成 `nullptr` 并清脏位 ——
指针指向 `m_asset` 里的 vector，`m_asset` 一旦被整体替换就是悬垂指针。

### 19.5 数据

| 指标 | 修前 | 修后 |
| --- | --- | --- |
| 峰值落点偏差 | 147 m（镜像） | 0 |
| 刷新数据来源 | `src=disk`（每次读 2 MB） | `src=memory`（7/7 次，0 次读盘） |
| 单次刷新耗时 | 未测（含磁盘往返） | `lastMs=1.40` / `maxMs=1.40` |
| 一笔 30 步的落盘次数 | ≈ 刷新次数（十几次） | **1 次**（抬笔时） |

### 19.6 验收（只跑针对性，未跑全量）

| 门 | 结果 |
| --- | --- |
| `verify_world_tools.py` | **32/32 PASS**（含新增 A0 组 6 条） |
| `verify_edit_modes.py` | ALL PASS |

脚本自身修掉的四处**过时断言**（都是"代码对了、断言先判死刑"）：

1. `A1` 只查 6 个工具按钮 —— 现在有 8 个；
2. `A5a/A6a` 按 `LS-TOOL <枚举值>` 断言，但按钮网格插过 Ramp 之后
   视觉第 4 格是 Ramp（枚举值 7）、第 5 格才是 Noise（枚举值 3）。改成
   按视觉序号取锚点、按 `name=Noise` / `name=Erode` 判成败；
3. `A1b` 滚轮调半径：滚轮分支的前置条件是 `onTerrain`（射线打到地形），
   刚自动建完地形那一两帧相机可能还在落位，射线打空时滚轮被视口拿去
   推拉相机 → 一条 `LS-BRUSH` 都没有。加最多 3 轮重试；
4. `verify_i18n_coverage.py` 的豁免表同步改名。

---

## 20 · 地形三件事：贴地 / 形状雕刻 / Raise 锥形（2026-10-21 第四轮）✅

> 用户原话三条：
>
> 1. 每次通过地编的方式生成的地形，为什么在编辑器中运行游戏时，发现生成的地形是在
>    原栅格地面的**正上方**的，并且是把原本的游戏初始化场景给**覆盖住**了，正常不应该
>    地形地貌是建立在原来栅格地面的基础上的嘛。
> 2. 地形编辑器中的挖洞的功能，不是将原地面中间剖开一个缺口，**不能是破坏地面的拓扑
>    结构**，挖洞功能最好我觉得是能否通过选择一个简单的物体的方式，通过在地面中，拖动
>    这个物体去慢慢贯穿整个机构表面来实现以这个物体的轮廓去给地面形变构造出对应的
>    类似洞穴的结构。
> 3. 地形编辑中的 Raise/Lower 在实际表现得效果来看，在抬升地形时的整个过程中，最后
>    收尾的时候，总是会成**一个小平面**，以致于每次想通过它做成一个山峰，结果变成了
>    一个类似**火山**的结构。

三条是三个彼此独立的根因，凑巧都在 `LandscapeEditor` 里。

### 20.1 需求 1：新建地形悬空 + 盖住初始场景

两个原因叠在一起：

1. `generateDefaultHeights()` 用的是**没归一化**的正弦叠加，值域大致 `[-1, 1]`
   再被硬钳到 `[0, 1]` —— 一起手就有一大片顶在 1.0（= `maxHeight` = 60 m）。
2. `createTerrain()` 建完实体**没有落座**，`position.z` 留在 0。模板场景
   `third_person` 的实体（`Ground` / `Player` / `Pickup_1..5` / `Portal`）
   全坐在 `z ≈ 0..1`，于是 60 m 高的地形把整个场景盖平了。

修法：

```cpp
// 原始正弦先算出来，再**整体归一化**到 [0, kDefaultPeak]。
constexpr float kDefaultPeak = 0.12f;      // 0.12 * 60 m = 7.2 m
const float span = std::max(hi - lo, 1.0e-6f);
heights[i] = (raw[i] - lo) / span * kDefaultPeak;
```

谷底严格为 0（四条边也全是 0，`verify_landscape_ground` 的 A1 一直盯着这条），
峰值 7.2 m —— 有地貌感，但不会把 1 米高的拾取物埋掉。

落座用的是一个**极小的 epsilon**，不是"随便垫高一点"：

```cpp
constexpr float kGroundEpsilon = 0.02f;    // 2 cm
tr->position.z = kGroundEpsilon;
VK_LOG_INFO("LS-CREATE-SEAT z=%.2f (ground plane + epsilon)", ...);
```

为什么不干脆 `z = 0`：模板场景里的 `Ground` 是一块**同样在 z=0 的平面**。
两者共面 → z-fighting → 地面会闪。2 cm 足够躲开深度精度，肉眼看不出来
（地形单位是米，2 cm 相当于 0.02 个采样点的高度）。

### 20.2 需求 2：挖洞 → 形状雕刻（Carve）

旧的 `Tool::Hole` 写的是 `holes.r8` 遮罩，片元着色器里 `discard` ——
**这就是字面意义上的"把地面剖开"**：拓扑被破坏，射线打不中，物理高度场
直接漏空。用户要的恰恰相反。

改成 **形状压印**：选一个形状体（球 / 方块 / 圆柱），把它从地表"按"下去，
只改**高度图**，一行遮罩都不写。

```cpp
enum class CarveShape : int { Sphere, Box, Cylinder };
float m_carveDepth = 0.35f;   // 0.0 .. 1.2，下沉深度（× 半径）
float m_carveCenterM = 0.0f;  // 形状体中心的绝对高度（米），落笔那一刻锚定
```

三个形状各自给一个"下表面半高" `half` 和归一化径向距离 `t`，然后：

```cpp
// 球：half = sqrt(R² - d²)  → 碗状凹坑
// 方：half = R * 0.7        → 平底直壁基坑
// 圆柱：half = R            → 竖井
const float bottomN = (centerM - half) / unitM;   // 米 → 归一化
if (bottomN < h) h += (bottomN - h) * k;          // 只往下削，不往上填
```

Shift 反向 `if (topN > h) h += (topN - h) * k;` —— 同一个轮廓**堆出**隆起，
所以"压出凹腔"和"垒出土丘"是同一个算子的两个符号。

**关键设计：形状体中心在落笔那一刻锚定，整笔不动。**
第一版是每帧按"当前地表高度"重算中心，结果 Shift 隆起每帧把中心往上顶一截，
自己喂自己，一路顶到 480 m（`A8f` 把一小片推到 8.0，`A9` 读到这个假顶点，
`A9b` 的"突破 1.0"变成了一次**假通过**）。锚定之后没有正反馈回路。

UI 侧顺手把工具网格的行高从 28 压到 22、`ItemSpacing` 收到 `(8, 5)`：
第 8 个按钮（Carve）原来下沿顶到 `y=859`，而地形面板底边是 842 ——
**按钮有 17 px 在面板外面**，点它的中心会打到底部 dock 上，把视口标签页
从 Viewport 切成 Console，之后所有视口操作全部落空。这类"UI 溢出导致
半段验收全 FAIL"的坑，上一轮已经吃过一次（`LS-TOOL-7`）。

### 20.3 需求 3：Raise 收尾成平顶（火山口）

根因就一行：

```cpp
h = std::clamp(h, 0.0f, 1.0f);     // ← 旧
```

笔心（权重 1.0）先撞到 1.0 停住，外圈（权重 0.5/0.2…）还在涨 ——
最后**必然**收成一块顶面平整、边缘变陡的台地。那就是用户说的"火山"。

修法分两步：

```cpp
constexpr float kMaxNorm = 8.0f;   // 归一化高度的上限，**不是 1.0**

float raiseHeadroom(float h) {
    if (h < 1.0f) return 1.0f;                     // 1.0 以下手感不变
    return std::max(0.0f, (kMaxNorm - h) / (kMaxNorm - 1.0f));
}
```

1. 天花板从 1.0 抬到 8.0（= 480 m），1.0 只是"等于 maxHeight"，不再是墙；
2. 抬升量乘 `raiseHeadroom(h)` —— 越接近上限越慢，是**渐近**而不是撞墙。

第 2 条才是"锥形"的来源：即使真的刷到很高，笔心也是缓缓收敛，
外圈慢慢追上来，剖面始终是锥形，不会被一刀切平。

### 20.4 顺带修掉的：`localHit.z` 单位误用（三处）

`physics::Heightfield::height()` 的定义是 `raw * heightScale`，而
`LandscapeEditor::raycast()` 给它的 `heightScale` 是
`maxHeight * m_heightScale` —— 所以 **`hit.point.z` 已经是米**。
三处代码把它继续当归一化用：

| 位置 | 旧写法 | 症状 | 新写法 |
| --- | --- | --- | --- |
| `m_carveCenterM` | `localHit.z * unitM - ...` | 形状体被放到 **807 m** 高空，压印什么也碰不到（`A8d` 恒为 0 个采样点变低） | `localHit.z - m_carveDepth * R` |
| `m_flattenHeight` | `localHit.z * maxHeight * heightScale` | Flatten 把地面直接顶到上限（烧成一片高台） | `localHit.z`（`applyStroke` 里自己除 `unitM`） |
| `m_rampStartH` | `localHit.z`（米）直接和 `heights[idx]`（归一化）插值 | Ramp 把整块地形推向 8.0 —— C1 是"炸了也算改了"的假通过 | `localHit.z / unitM` |

`A8d` 的日志把这条钉得很死：修前 `LS-CARVE ... centerZ=807.3`、压前后 absdiff
`0.00000`；修后 `centerZ=7.7`、`668 个采样点变低`。

### 20.5 数据

| 指标 | 修前 | 修后 |
| --- | --- | --- |
| 默认地形峰值 | 1.0（60 m，顶住天花板） | 0.12（7.2 m） |
| 默认地形谷底 | 0 | 0（严格，四条边也是 0） |
| 地形实体落座 | `z = 0`（与模板 `Ground` 共面） | `z = 0.02`（2 cm） |
| 挖洞是否改遮罩 | 是（写 `holes.r8` + `discard`） | 否（只改高度图） |
| 落笔形状体中心 | 807.3 m（单位错） | 7.7 m |
| 压印影响的采样点 | 0 | 668 |
| Raise 上限 | `clamp(h, 0, 1)` 硬墙 | `kMaxNorm=8` + 渐近余量 |
| 抬升顶点 / 同高采样点 | — | `top=1.113` / **1 个**（真锥形） |

### 20.6 验收（只跑针对性，未跑全量）

| 门 | 结果 |
| --- | --- |
| `verify_world_tools.py` | **全部 PASS**（A- 组 3 条、A8 组 8 条、A9 组 4 条为核心新增） |
| `verify_landscape_ground.py` | **17/17 PASS** |
| `verify_terrain_editor.py` | **23/23 PASS** |
| `verify_terrain.py` | **16/0 PASS** |
| `verify_edit_modes.py` | ALL PASS |
| `verify_layout_v4.py` | ALL PASS |
| `verify_toolkit_modes.py` | ALL PASS |
| `verify_toolkit_round2.py` | ALL PASS |
| `verify_i18n_coverage.py` | PASS（词典 1919 条，A1-A4/B/B2/D/E/F 全 0 缺） |

### 20.7 脚本自身修掉的四处问题（都是"代码对了，断言先判死刑"）

1. **`A9b` 在测机器速度。** 抬升量 ∝ `dt`，而引擎把 `dt` 钳在 0.05 s ——
   慢帧下累积速度腰斩。同一份 Raise 代码，上一跑 `top=1.56`、这一跑 `0.63`，
   看着像回归。改成**刷到达标（>1.10）就停**，上限 30 笔，与帧率无关。
2. **`verify_landscape_ground` 的 A2** 要求 `max > 0.30` —— 那是"默认地形能顶到
   1.0"时代的期望。现在需求正好反过来：有起伏（>0.05）但**不喧宾夺主**（≤0.25）。
3. **同脚本 B 组整段是 `Hole`**（写 `holes.r8`、断言挖穿点 > 0）。重写成
   Carve：选中它 → `LS-CARVE` → 采样点变低 → **`holes.r8` 不存在或全 255**
   （拓扑完整，这条才是用户要的）。
4. **`verify_terrain_editor` 的 D1 点空了。** `logRect` 报的是
   `GetItemRectMin/Max`（**未裁剪**）。Inspector 是滚动窗口，Terrain 段排在
   Transform / Visibility 之下，而本机桌面只有 1088×600（`settle` 想拉到
   1600×900 拉不上去）→ 勾选框排到可视区外，锚点照样有坐标，点下去落在
   面板外 → 前后截图**像素级一致**（sig=0.00%），看着像 Visible 开关坏了。
   修法：`InspectorPanel` 补一个 `INS-PANEL` 锚点（与 `CONSOLE-PANEL` /
   `FO-PANEL` 同写法），脚本加 `scroll_into_view()` 滚进可视区再点；
   顺带把 sig 的对比框改成**重新取一次 `VP-RECT`**（D 组时 Content Browser
   已收起，视口比 A 组高了一截，用旧 box 会漏掉下半屏）。修后 `sig=78.91%`。

## 21 · 建模：多边形面表 + 内插/倒角/环切（2026-10-21 第五轮）✅

用户原话：

> "Modeling 建模模式中，对于立方体，为什么显示出的每个正方形面都是两个三角形
> 拼接而成的，而不是一整个的正方形。这种对建模操作显示是不对的。另外，建模
> 模式应该包含一下操作：挤出选区，内插面，倒角，环切。这些的具体细节和实现
> 效果你可以参考下 blender 中。"

### 21.1 面为什么看起来是三角形

网格本体只有三角形（GPU 只吃三角形），所以"面"是**拓扑层**的事。原先的
`ToolkitEditors::Topo` 只有三角形与边，面是"共面两两合并"反推出来的 ——
这条反推的天花板就是四边形：

* 立方体 6 个面恰好都能两两合并 → 看起来是对的；
* 一旦**倒角**（八边形）或**环切**（细长条），一个面需要 3~4 个三角形才能
  拼回，两两合并只能得到几块碎片 → 选中一个面只高亮一块，用户看到的就是
  "这个面被切成了好几个三角形"。

### 21.2 修法：让面表跨操作存活

`Topo` 增加 `Poly{ tris, v, normal, area }` 与 `triPoly`（三角形 → 面），
并加一个 **`m_polyHint`**：`commitEditMesh()` 写回网格时记下"每个面由几个
三角形拼成"，`rebuildTopo()` 优先用它做**扇形三角化的逆运算**：

```
面 (v0,v1,v2,v3) → 三角形 (v0,v1,v2), (v0,v2,v3)
逆运算 v = {tri[0].a, tri[0].b} + {tri[k].c for k in 0..n-1}
```

没有提示时（刚加载 / 刚绑定）才退回"共面合并"。法线用 `newell()` 而不是
单三角形叉积 —— n 边形共面时挑错三角形会翻向。

### 21.3 三个新操作

| 操作 | 快捷键 | 实现要点 |
| --- | --- | --- |
| 内插面 Inset | `I` | 每个选中面沿其**面内**的两条边方向收缩 `dist`，外圈生成四边形环 |
| 倒角 Bevel | `B` | 只在**选中顶点**处切角：每个入射面沿边各让出 width，角落补一个 n 边形 cap |
| 环切 Loop cut | `Ctrl+R` | 找选中边所在的**四边形环带**，按边参数中点插一圈顶点并一分为二 |

日志：`MODEL-INSET / MODEL-BEVEL / MODEL-LOOPCUT`，全部按**多边形**计数
（`polys=` / `selF=`），这样"面是不是完整"本身就变成可断言的东西。

### 21.4 验收（`tools/verify_modeling_topology.py` 全 PASS）

| 断言 | 结果 |
| --- | --- |
| 立方体绑定即 `polys=6` | PASS（六张完整四边形，不再是 12 个三角面） |
| 内插 | `faces=6 newVerts=24 dist=0.080 verts=96 tris=60 polys=30 selF=6` |
| 倒角 | `corners=8 cuts=12 caps=8 verts=96 tris=44 polys=14 selV=12` |
| 环切 | `faces=4 cuts=1 newQuads=8 verts=40 tris=20 polys=10` |

⚠ 每个 phase 都**重新启动一次编辑器**：建模的输入是"当前网格"，连跑会让
断言退化成"比上次多"这种没信息量的形式。

---

## 22 · 水体：独立通道 + 拖拽生成 + 5 种水质（2026-10-21 第五轮）✅

用户原话（摘要）：水体要进左栏模式列表、参数进下方参数区；用圆形/矩形
**拖拽**生成；参数面板实时控制范围/颜色/类型；默认给几种水质（山泉/湖/
池塘/海/河），不同水质的透明度、折射、流速、波纹、涟漪都不同；水面要有
真实的涟漪与波浪细纹。

### 22.1 为什么水面必须是**独立 RenderPass**

折射要采样"本帧已经画完的场景颜色"。在同一个 RenderPass 里，颜色附件正在
被写，同时把它当纹理读就是 feedback loop，验证层直接报错。所以顺序固定：

```
前向 Pass 结束（resolve 出单采样 HDR）
  ↓ vkCmdCopyImage：resolve → sceneCopy（本通道持有，可采样）
  ↓ 水面 RenderPass（只有颜色附件，LOAD，单采样，**无深度附件**）
```

无深度附件的原因：HdrTarget 的深度图是 4x MSAA，而一个 RenderPass 里所有
附件的采样数必须一致 —— 想同时把深度当附件和纹理又回到 feedback loop。
于是水面走**单采样 + 手写深度测试**：`water.frag` 从深度图重建场景世界
坐标，比它到相机的距离，前景更近就 `discard`。

### 22.2 水感是怎么算的（六段）

1. **波形**：4 项方向不同的深水重力波叠加，`h = Σ A·sin(k(d·p)+ωt+φ)`，
   `ω = √(g·k)·0.35`（压低防混叠），振幅 ×0.55、波长 ×0.62 逐项衰减。
2. **法线**：解析梯度 + 三层噪声（顺流向压扁 4 倍的那层给"水流感"，两层
   各向同性给涟漪与细纹）。
3. **折射**：采样 sceneCopy，偏移量随水深衰减；偏移后的样本若采到比水面
   更近的几何（把岸边石头吸进水里 = 典型穿帮）就退回不偏移。
4. **吸收**：Beer–Lambert，`absorb = 1 - exp(-depth*0.65/clarity)`。
5. **反射**：菲涅尔 `f0 = ((ior-1)/(ior+1))²`（水 1.333 → 0.02），skyMap 采样。
6. **泡沫**：岸边（浅）+ 浪尖（**陡度 A/λ** 起白浪，不是"波高"）。

### 22.3 五个预设

`clarity` 高 = 山泉（几乎不吸收），低 = 池塘（2 m 就浑）：

| 水质 | clarity | 波高 | 流速 | 说明 |
| --- | --- | --- | --- | --- |
| 山泉水 | 3.2 | 0.05 | 1.1 | 清澈见底，细碎涟漪 |
| 湖水 | 1.6 | 0.10 | 0.05 | 默认，几乎静止 |
| 池塘水 | 0.45 | 0.02 | 0 | 浑、绿、无浪 |
| 海水 | 1.2 | 0.55 | 0.35 | 大浪 + 白沫 |
| 河水 | 0.9 | 0.08 | 1.5 | 明显流向条纹 |

### 22.4 面板形态（两次返工后的结论）

* **水质/形状用按钮行，不用 Combo**。5 选 1 的枚举不该"点开再点一项"，
  而且弹出层行高依赖主题 —— 自动化只能猜像素（实测猜错了：点"海水"选到
  了"河水"）。改成一排按钮后每个值有自己的锚点（`WATER-KIND sea`）。
* **按用途折叠成 4 段**（Shape / Water quality / Surface motion / New water
  body）。左栏参数区只有 ~400px 高，全部控件排下来近 1000px —— 一长串的
  直接后果就是"尺寸/水位这些最常用的滑块要滚下去才点得到"，而滚出去的
  控件看起来就是"功能坏了"。

### 22.5 验收（`tools/verify_water.py` 26/26 PASS）

A 模式入口与面板 / B 通道初始化 / C 建水体 + 像素变化 60.9% / D 预设与
形状（含 `WATER-MESH shape=rect tris=800`）/ E 参数实时生效（拖半径→
`radius=176.5`、拖水位→`level=160.66`）/ F 删除（bodies 2→1）/ G 零 VUID。

### 22.6 这一轮修掉的四个"真 bug"

1. **`WaterPass` 只在 `createPipelines()` 里 `setTargets`** → 视口 resize 后
   它保留 1600×900 的 `m_extent` 与已销毁的 resolve/depth 视图。一次爆 18 条
   验证错误（`vkCmdCopyImage` 越界、`vkCmdBeginRenderPass` 悬垂附件、
   `vkUpdateDescriptorSets` 悬垂 imageView）。修法：新钩子
   `Renderer::recreateWater()`，与粒子/SSAO 一样挂在 `recreateScreenSpace()`；
   并且 `setTargets` 早退分支**逐句柄比对**、视图真变了要**连 framebuffer
   一起重建**（原来只刷新字段，fb 还指着旧视图）。
2. **湖面上长白斑**：浪尖泡沫只看"相对波高"，平静的湖也被判成白浪。改成
   看陡度 `A/λ`（0.02 起、0.075 满）。
3. **`WATER-SIZE halfExtent=` 只对"半深"滑块生效**：`IsItemDeactivatedAfterEdit()`
   只认紧挨着的上一个 item，原来写在两个滑块之后 → 只拖"半宽"永远不打日志。
   两个滑块各挂一次。
4. **场景相机在栅格面之下仰视**：`assets/scenes/sandbox.json` 的
   `"pitch": -18.0` 是**度**，被单位嗅探换算成 -0.314 rad —— 而 `Camera`
   的约定是"正 = 在目标上方俯视"，负号 = 相机沉到地面以下往上仰。改写成
   弧度正值，并加 `SCENE-CAMERA-LOW` 告警（pitch<0 或 eye.z<=0 直接报），
   这类"渲染完全正常、只是看着怪"的问题以后能自证。

### 22.7 脚本自身修掉的三处脆弱点

1. **`open_section` 用 `rect_of` 判"已展开"** —— 拿到的是选区出现**之前**的
   历史矩形（那时新建段不带折叠头），于是它以为已展开、没点折叠头，后面的
   点击全落在旧坐标上（D 组 5 条连锁失败）。补 `drawn()`：先让面板滚一格
   再滚回来（`logRect` 对不动的矩形不重复输出），数行数有没有涨。
2. **拖滑块的坐标没过 `client_to_screen`** —— `SetCursorPos` 收屏幕坐标，
   直接喂客户区坐标会把鼠标点到别处，表现为"拖动毫无反应"。
3. **"先点一下再按下"**：间隔 < 双击阈值会被 ImGui 当成双击 →
   `SliderFloat` 进入文本输入模式，之后拖动同样没反应。

---

## 23 · 水体：从"一张纸"到有厚度的水体 + 贴地（2026-10-21 第六轮）✅

用户原话：

> 1. "水体不是只有一个水的平面，还是要有一定水的深度的，并且也要符合水体
>    反射和折射的视觉效果。而且创建水体的时候，其参数面板会什么会消失不见。"
> 2. "水体创建的位置应该是紧贴在地面上的，不能出现反重力的现象，如果水体
>    创建在比较低洼甚至凹凸不平的地形上，也要符合这个规律。"

### 23.1 为什么它是一张纸

上一轮的几何只有**顶面**：一个圆形/矩形网格，全部顶点在 `z = level` 上。
这在俯视时没问题，但从低角度一看就露馅——水面边缘直接断在半空，没有厚度；
地形一起伏，水面还会在山坡上"悬"着（用户说的"反重力"）。

根因是三件事叠在一起：

1. **没有侧壁**：水体只有上表面，边界没有截面。
2. **水位跟着点击点走**：`level = hit.z + 0.35`，不管那片地形是坡是坑。
3. **没有池底概念**：连"这块水有多深"这个量都不存在，`Beer–Lambert` 只能
   靠深度图反推，面板上也没有任何水深读数。

### 23.2 侧壁：一块水的截面

顶点加了一个 `part` 字段（0 = 水面 / 1 = 侧壁顶 / 2 = 侧壁底），
同一次 draw call 里两类几何一起画：

```
水面     z = 0（顶点着色器里加波浪）
侧壁顶   z = 0（**也加波浪**：不加就会在水面波动时脱开一条缝）
侧壁底   z = UBO.p4.y - level
```

侧壁的高度**不进顶点**：它只依赖"水面高度"和"池底高度"两个标量，放进
UBO。于是拖水位滑块 = 写 16 字节，不需要重建任何缓冲（否则每拖一格都要
`waitIdle` + 重新上传顶点）。

侧壁的着色走另一条路径（`shadeSkirt`），因为它的语义不同：它后面是土，
不是水底，所以**不做折射**，只做 Beer–Lambert（离水面越深越暗）+ 菲涅尔
+ 水面附近一条被噪声打散的水线。

### 23.3 贴地：水位跟**岸线**，不跟池底

```
        rim = footprint 边界一圈的最低地形   ← 水位 = rim + 0.18
   ┌────┴────┐
   │  ~~~    │        bed = footprint 内最低地形   ← 侧壁底 = bed - 0.75
   └─────────┘
```

* **水位跟岸线最低点**，不是池底：池底再深也不该把水面拽下去（那是"水漏了"
  的语义）。岸线决定的是一个水平面能抬多高而不从岸边溢出去。
* 抬 `+0.18 m` 是为了避开 z-fighting（波浪还会上下 ±0.1 m）。
* **池底 = 区域内最低地形**，侧壁从它再往下沉 0.75 m。地形有起伏时，
  统一的"最低点"保证任何角度的侧壁都被土埋住——多沉的部分不花代价。

采样器 `GroundProbe` 在**构造时加载一次**地形（`height.r32` + JSON），
之后每个点都是 O(1) 双线性插值。footprint 上撒约 150 个点（圆形 3 圈×48 段
+ 圆心，矩形 7×7）。**只在松手那一刻采样**——拖动过程里读盘会把帧率拖下去。

`autoFit` 会记住"用户有没有手动拖过水位"：拖过就不要再自动改回去。

### 23.4 面板："消失"的三个来源

| 现象 | 根因 | 修法 |
| --- | --- | --- |
| 建完一块就找不到"再建一块" | 选中态把"新建"段挪到最后并默认收起 | 首屏固定一行 `＋ 新建水体` |
| 面板看起来是空的 | 内容在"新建/选中"之间切换后 ImGui 保留了旧的 `scrollY` | 切换时 `SetScrollY(0)` |
| 点了"＋ 新建"没反应 | `select({})` 不清 `m_selectionName`，下一帧 `validateSelection()` 按名字把它找回来了 | 改用 `clearSelection()` |

第三条是这一轮最隐蔽的一个：日志（`WATER-NEW-ENTRY`）打了、按钮也确实
被点中，但面板纹丝不动——因为选中项在下一帧被"按名字找回来"了。

### 23.5 顺带调掉的两个观感问题

1. **岸边一条结冰似的白带**：岸线泡沫的 `smoothstep(0, 1.5, depth)` 在缓坡
   池塘上覆盖好几米宽。收窄到 0.10 m，再用阈值化噪声把它打成斑驳的碎块。
2. **水面像一块灰白塑料**：掠射角下菲涅尔接近全反射，一片灰色天空直接
   镜面反射就是"结冰"。给反射乘一点水色（`mix(vec3(1), shallow*1.8, 0.28)`），
   并把 `bodyMix` 里 `opacity` 的系数从 1.0 降到 0.5——2 m 深的湖原本
   82% 被水色盖住，一点底都透不出来。

### 23.6 验收（`tools/verify_water_depth.py` 全 PASS）

场景是 `scenes/water_bowl.json`：512 m 台地（14 m）+ 中心半径 120 m、深 12 m
的碗，碗底还有 ±1.2 m 起伏——只有这种地形才同时测得到"小水浅、大水满"。

| 断言 | 结果 |
| --- | --- |
| 侧壁生成 | `WATER-MESH ... tris=630 skirt=60` |
| 采到真地形 | `WATER-GROUND src=terrain bed=1.42 rim=1.42 level=1.60` |
| 水面不高于岸线 | `level ≤ rim + 0.25` |
| 半径 10 m → 176 m | `rim 1.42 → 14.00`，`depth 0.18 → 12.18` |
| 放大后仍不溢出 | `level=14.18 ≤ rim=14.00 + 0.25` |
| 面板回到新建态 | `WATER-UNBIND` + 再建一块 `WATER-CREATE 1 → 2` |
| 日志健康 | 零 VUID |

另外 `verify_water.py` 26/26 仍全 PASS（`skirt=60` / `skirt=160`）。

### 23.7 修掉的三个真 bug

1. **`rimMinZ` 被圆心污染**：`measureGround` 用第一个采样点初始化
   `rimMinZ`，而圆形的第一个点是**圆心**（碗底）。于是 176 m 的湖把
   rim 记成 2.00 m（碗底）而不是 14.00 m（台地），水面只盖住碗底一小圈。
   改成只用 rim 点算，`rimMinZ` 初始为 +∞。
2. **`select(ecs::Entity{})` 取消不了选择**（见 23.4 第三条）。
3. **水面与侧壁在波动时脱缝**：侧壁顶圈不加波浪位移，水面一波动上沿就
   露出缝隙。改成 `part < 1.5` 都加。

### 23.8 测试资产

* `assets/terrain/water_bowl/`（257²、2 m/px、maxHeight 40）：碗形，
  中心 2 m、台地 14 m，碗底带噪声。
* `assets/scenes/water_bowl.json`：地形 + 一块 25 m 的水塘，打开即可看效果。

⚠ 这两个目录**不在** CMake 的"每次拷贝"列表里（`scenes` 是运行时写的、
`terrain` 是地形编辑会改的），所以 `verify_water_depth.py` 自己做了
`ensure_scene_assets()`——否则场景打开失败会静默回退到默认场景，
地形不存在，贴地断言全部退化成 `src=flat`。
