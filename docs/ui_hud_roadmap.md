# 游戏 UI / HUD 现状分析 + 改造路线图

> 2026-10-18 · 对齐 `docs/ship_roadmap.md` 的 **G15**（游戏 UI / HUD）
> 这是"补齐一条 3D 游戏链路"系列的**第二篇**（第一篇：`character_pipeline_roadmap.md`；后续：音频、存档与 GUID、打包）。
> 所有结论都对着磁盘代码核过，行号是 2026-10-18 的版本。

---

## 0 · 一页结论

**现状不是"少了一个 HUD 组件"，是"一个像素都没有"。**

运行时（`Game.exe`）里玩家能看到的文字与图形 = **0**。`engine/ui/` 下只有两个文件
（`ImGuiManager` / `ImGuiTheme`），且都只为编辑器服务：ImGui 的 `io`、字体图集、
descriptor pool 全部由 `EditorApp` 驱动；`RuntimeApp` 里连一行 UI 调用都没有。

要做出"血条 + 准星 + 伤害数字 + 暂停菜单"，缺的是**四层地基 + 一处 i18n 归属错误**：

| 层 | 现在有什么 | 还差什么 | 断点 |
|---|---|---|---|
| 渲染 | 全屏后处理 quad、3D 管线、ImGui 管线 | **2D 正交批渲染通道**（顶点格式 / 图集 / 混合 / 挂点） | D0 |
| 字体 | ImGui 私有的位图图集（stb_truetype 烘焙） | **SDF 图集**（任意字号锐利）+ **可打包的 CJK 字形** | D1 |
| 布局 | ImGui 的"窗口级"布局（`SplitLayout` + 面板矩形） | **控件级锚点/弹性布局**（分辨率无关） | D2 |
| 输入 | `InputMap` 的上下文栈（G06 已完成） | **焦点栈 / 方向键导航 / 指针捕获** | D3 |
| i18n | `editor::tr()` + 609 条词典，**锁在 `editor` 命名空间** | 运行时能查同一份词典 | D4 |
| 数据 | 无血量 / 无伤害事件 | `HudModel`（纯数据）+ 脚本填值 | D5 |

**一句话判断**：四件套里少任何一件，都只能做出一块"位置对、但放大就糊 / 换分辨率就歪 /
键鼠同时驱动角色和菜单"的矩形。这不是危言耸听 —— 项目里已经踩过同类坑：
Inspector 的 `All` 页签、粒子面板的逐发射器编辑，都是在"缺一层抽象"时用 if 硬凑，
最后返工两轮。

**总规模：`L`**（≈ A0 的 1.5 倍；A0 是 9 个断点，这里是 6 个断点，但渲染侧要新建一整条通道）。
**建议拆成 3 个里程碑**：M1 地基（U0~U2，一块能上屏的矩形）、M2 内容（U3~U5，能画字和按钮）、
M3 交互（U6~U8，能玩）。

---

## 1 · 现状盘点（逐项对着代码）

### 1.1 渲染：UI 唯一能上屏的位置，已经被 ImGui 占了

`PostProcess::renderScene()`（`engine/render/PostProcess.cpp:828`）把一帧分成两条路径：

| 模式 | 谁在用 | 输出目标 | ImGui 画在哪 |
|---|---|---|---|
| **A** | `Game.exe`（`RuntimeApp`） | 交换链 | `renderScene` 的 `overlay` 回调里，**和 FXAA 在同一个 render pass 内**（`:915-921`） |
| **B** | `Editor.exe` 的视口 | 离屏图 `m_sceneOutput` | `renderScene` **不画**（传 `nullptr`），之后 `renderUiToSwapchain()`（`:956`）另开一个 CLEAR pass 画到交换链 |

而两个输出 pass 的 `finalLayout` 是**写死**的（`:367-369`）：

```cpp
makeOutputPass(VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,        m_postRenderPass);     // 模式 A
makeOutputPass(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, m_offscreenRenderPass); // 模式 B
```

**这条决定了插入点**：

- 模式 A 的 pass 结束时图已经是 `PRESENT_SRC_KHR` → **不能在它之后再开一个 render pass
  往同一张交换链图上画**（要重新 transition，且 `PRESENT_SRC` 作为 `initialLayout` 的
  pass 有严格限制，验证层会直接报 VUID）。
- 所以游戏 UI 的唯一干净挂点是：**Post 输出 pass 内部、ImGui `overlay` 之前**。
  这也正好是"PostProcess 之后、交换链之前"的物理位置，满足 `ship_roadmap` 的顺序契约。

**两个模式各差一个 hook**：

| 模式 | 现状 | 要加 |
|---|---|---|
| A | `recordComposite(..., &overlay)` 已经是"一个回调" | 把它拆成"两个回调"，或让 `Renderer` 传入一个复合回调 `gameUi` + `imgui` |
| B | `recordComposite(..., nullptr)` —— 连 overlay 都没有 | 传 `&gameUiOverlay`（HUD 画进离屏图 → 编辑器视口能预览） |

⚠ **模式 B 的语义要明确**：HUD 画进离屏图意味着**编辑器视口里也能看到血条**。
这是"预览"功能，不是 bug；但它会改变编辑器截图的像素 → 必须默认关闭
（`ui.hudInEditor=false`），否则所有编辑器像素基线全漂。

### 1.2 字体：ImGui 的图集是"它自己的"，游戏 UI 拿不到

`engine/ui/ImGuiManager.cpp:147-150`：

```cpp
info.PipelineInfoMain.RenderPass  = renderPass;              // = m_postRenderPass
info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
```

ImGui 后端在 `ImGui_ImplVulkan_Init` 内部**自建**：字体图集（`R8` 位图，stb_truetype 烘焙）、
采样器、descriptor pool、pipeline。这些全部是后端的私有资源，**没有公开句柄**，
游戏 UI 想复用只能走非公开接口（`ImGui_ImplVulkan_GetTexID` 之类）→ 等于把渲染绑死在 ImGui 上，
与"不能拿编辑器 ImGui 凑"的结论直接冲突。

**更致命的是字形来源**（`ImGuiManager.cpp:100-130`）：

```cpp
const char* kCjkCandidates[] = {
    "C:/Windows/Fonts/Deng.ttf",    // 等线
    "C:/Windows/Fonts/simhei.ttf",  // 黑体
    "C:/Windows/Fonts/simsun.ttc",  // 宋体
    ...
};
```

中文字形是**从 Windows 系统字体合并**进来的。这有两个问题：

1. **打包即失效**：玩家机器上（尤其 Linux/macOS）不一定有这些路径 → 中文全是方块。
2. **许可证**：`simhei/simsun/Deng` 都是微软/方正授权字体，**不可再分发**。
   把游戏发出去 = 侵权。

→ 必须换成 **OFL 授权**的 CJK 字体（Noto Sans SC / 思源黑体 / 文泉驿），
并且**内嵌到 `assets/`**，走 SDF 图集，而不是运行时读系统字体。

`assets/fonts/` 目录现在也不在仓库里（只有 `build/bin/assets/fonts/` 下几份
ImGui 自带的英文 TTF，由 CMake 从 `third_party/imgui/misc/fonts/` 拷过去）。
构建脚本 `myvk_copy_runtime_assets`（`CMakeLists.txt:494-568`）的拷贝列表里
**没有 `assets/ui/`** → 新目录要显式加一行，否则运行时找不到图集。

### 1.3 布局：编辑器那一套是"窗口级"，不是"控件级"

编辑器有 `SplitLayout`（分隔条 + `m_occluders` 遮挡）、`LayoutRects`（五个槽位）、
`computeLayout()`（每帧显式设 Pos/Size）。这套东西解决的是"面板摆在哪"，
**不是**"血条离左下角 24px、宽度跟最大血量成比例"。

控件级布局要的东西一个都没有：

- 锚点 / 边距 / 尺寸模式（fixed / fit-content / fill）；
- 参考分辨率缩放（1280×720 与 3840×2160 都要能看）；
- 文本测量（`measure(text, size) → width`）—— 连这个都依赖字形度量表。

### 1.4 输入：有上下文栈，没有焦点

`engine/core/InputMap.h` 已经有：`InputContext{name, actions, axes}`、上下文栈、
`ActionState{held, pressed, released, value}`、手柄按键/轴绑定、`makeDefault()` 里的
"移动/视角/跳跃/冲刺/交互/**暂停**"。这一层是 G06 的成果，**可以直接用**。

缺的是"UI 侧"：

- **焦点**：当前选中的按钮是哪个；Tab / 方向键怎么移动；循环还是钳制；
- **指针捕获**：鼠标点了按钮之后，这一帧的点击**不能再**触发视角旋转 / 射击；
- **消费语义**：`UiInput::route(...)` 要返回 `consumed`，且必须在**角色控制器读输入之前**调用
  （顺序反了就是"点按钮的同时角色跳了一下"）；
- **暂停时的输入归属**：`StateId::Paused`（`runtime/GameState.h:71`）已存在且
  `blocksUpdate=true, blocksRender=false`，但**UI 自己的 tick 不能被它截断** ——
  否则暂停菜单收不到输入。这是最容易写错的一处。

### 1.5 i18n：609 条词典锁在 `editor` 命名空间

`editor/Localization.h:29` → `namespace editor`；`editor/Localization.cpp` 1031 行，
其中 `kZh[]` 是 609 条 `{英文, 中文}`；对外只有 `tr()` / `trId()` / `setLanguage()`，
调用点用宏 `TR(s)` 包了 **792 处**。

运行时 `runtime/**` 依赖 `engine/**`（单向），**不能**反向依赖 `editor/**`
（分层纪律，`MEMORY.md` 第 2 段）。所以现在运行时**一条译文都取不到**。

`tr()` 的实现本身是干净的（哈希查表、缺译文回退英文、英文模式零开销），
可以直接搬，问题只在"放哪"和"谁是权威"。

### 1.6 数据：没有任何"要显示的东西"

`runtime/` 里没有 `HealthComponent`、没有伤害事件、没有 `HudModel`。
物理有接触事件（`physics::ContactEvents`），但"谁扣了谁多少血"是玩法层的事，
现在由 `.vks` 脚本自由发挥。

→ UI 不能直接读 ECS（否则 UI 与场景耦合、也没法在编辑器里预览假数据）。
需要一个**纯数据模型** `HudModel`，由脚本/GameState 填，UI 只读。

---

## 2 · 六个断点（D0~D5）

| 编号 | 断点 | 症状 | 谁受影响 |
|---|---|---|---|
| **D0** | 没有 2D 正交批渲染通道 | 任何 UI 图元都上不了屏；想画就只能在 ImGui 的 `DrawList` 里画（= 绑死编辑器） | 全部 |
| **D1** | 没有 SDF 字体 / 图集生成链 | 文字只能靠 ImGui 的私有图集；字号一变就重烘焙；打包后中文全方块 | 文本、按钮、菜单 |
| **D2** | 没有控件级布局 | 只能手写绝对像素坐标 → 换分辨率全歪；文本宽度算不出来 → 居中/右对齐做不了 | 全部 |
| **D3** | 没有焦点 / 输入路由 | 暂停菜单打开时角色还在走；方向键不知道该选哪个按钮；点 UI 会同时触发射击 | 菜单、设置 |
| **D4** | `tr()` 在 `editor` 侧 | 运行时无字典；游戏内切语言做不到 | 全部文本 |
| **D5** | 没有 UI 数据模型 | 血条画什么？伤害数字谁生成？ | HUD |

依赖顺序：**D0 → D1 → D2 → (D3, D4) → D5**。
D4 可以并行（纯 CPU，不依赖渲染），D5 最后（依赖 D0~D3 全部就位）。

---

## 3 · 关键技术判断（含对 `ship_roadmap` G15 的两处纠正）

### 3.1 ⚠ 纠正一：`maxBoundDescriptorSets = 8` **不是**"不能再要一个 set"

`ship_roadmap.md` G15 第 5 条写的是：

> 「descriptor set 是真实风险：Intel Iris Xe `maxBoundDescriptorSets = 8`，**现在恰好用满** →
> 运行时 UI **不能**再要一个独立 set → 方案：图集复用已有的采样器纹理槽…」

**这条判断是错的**，而且照它做会更糟：

- `maxBoundDescriptorSets` 是**单次绘制时同时绑定**的 set 数上限，不是全局 set 总数上限。
  UI 是一条**独立 pipeline**，一次绘制只绑 **1 个** set（图集 sampler + 一个 UBO），
  1 ≤ 8，完全合法。ImGui 自己就是这么干的 —— 它有自己的一套 set，从没碰过全局那 8 个。
- 反过来，"复用 set0 的空余 binding"是不可行的：`Renderer.cpp:806-963` 里 set0 的
  **25 个 binding 已经全部占满**（0/1/5/6/11~19/23/24 + 20~22 的 bindless 数组），
  再加一个就要改 25 处的布局、所有管线的 `setLayouts`、以及 set0 的写入代码 —— 
  这正是项目最忌讳的"改一处要同步改 N 处"。

**结论**：UI 用**独立 descriptor set layout（1 个 set）**，只在 `ui2` 的
pipeline 里出现，全局那 8 个 set 一个都不动。

⚠ 但要量一件事（U0 的第一项）：`VkPhysicalDeviceLimits::maxBoundDescriptorSets`
的真实值，以及**每条 pipeline 同时绑定的 set 数**是否真的没超。打完一行
`UI2-CAPS maxBoundDescriptorSets=8 maxPerStageDescriptorSamplers=...` 再动手，
不靠记忆。

### 3.2 ⚠ 纠正二：SDF 字体**不能**在运行时从系统字体生成

`ship_roadmap` 说"一张图集（`stb_rect_pack` 打 2048²）+ SDF 字体 atlas"，
但没说字形从哪来。如果沿用 `ImGuiManager` 的做法（读 `C:/Windows/Fonts/*.ttf`）：

- 打包后玩家机器上没有这些文件 → 中文全方块；
- 自带分发违反字体许可证。

**结论**：字形必须在**构建期离线生成**，产物（PNG + JSON）提交进 `assets/ui/fonts/`。
字体源用 **OFL** 授权的 Noto Sans SC / 思源黑体，子集化后随仓库分发（OFL 允许）。

### 3.3 插入点由 `finalLayout` 决定，不是"想插哪插哪"

见 §1.1。**唯一挂点 = Post 输出 pass 内、ImGui overlay 之前**。
这条写进文档是为了防止后来者"顺手在 `renderUiToSwapchain` 里加 HUD" —— 
那条路径只在编辑器模式跑，且画在交换链上会盖住编辑器面板。

### 3.4 一份 UI pipeline 可以同时用于两个输出 pass

`m_postRenderPass` 与 `m_offscreenRenderPass` 由**同一个 lambda** 生成
（`PostProcess.cpp:317-369`），差别只有 `finalLayout`。而 Vulkan 的 render pass
兼容性判定**不考虑** `initialLayout` / `finalLayout`，只看附件格式、采样数、
load/store op、子 pass 结构 —— 三者全同。

→ **一份 pipeline 够用**，模式 A/B 共用。省掉一半代码，也省掉"两份 pipeline 不同步"的坑。

⚠ 但这是**脆弱契约**：将来谁给输出 pass 加第二个附件、或把 `MSAASamples` 从 1 改掉，
这条立刻断。U0 要写一个自检（`MYVK_UI2_TEST=1` 的 `pipeline_compat` 项）钉住它。

### 3.5 为什么是 SDF，不是位图

| | 位图（ImGui 现在） | SDF |
|---|---|---|
| 字号变化 | 每个字号烘焙一份图集（内存 ×N） | 一份图集覆盖 8~64px |
| 放大 | 糊 | 锐利（靠 `smoothstep(0.5-w, 0.5+w, dist)`） |
| 描边 / 阴影 / 外发光 | 重新烘焙 | fragment 里改阈值，零成本 |
| 4K / 高 DPI | 必须重建 | 直接缩放 |
| 生成成本 | 低 | 中（构建期一次） |

游戏 UI 要"血条随分辨率缩放、伤害数字会放大弹出" —— 位图在这两件事上都不行。

**通道数选择**：先做 **单通道 SDF（R8）**。MSDF（三通道）对中文这种笔画密集的字更锐利，
但生成慢 3 倍、图集大 3 倍、shader 要 median 组合。留一个 `channels` 字段，
将来要换不用改上层接口。

### 3.6 为什么不 vendor yoga / nuklear / RmlUi

- **yoga**（Flexbox）：C++ 实现干净，但它只做布局，仍然要自己写渲染 + 文本测量 + 焦点；
  引入第三方还要处理 CMake / 许可证 / 与现有 `engine` 分层的关系。
- **nuklear / RmlUi**：自带渲染器，会把"UI 通道"变成一个黑盒，与"插入点在 Post pass 内"
  的硬契约冲突（它们默认自己开 pass）。
- 自研布局核心约 **600~800 行**（单遍 measure + arrange），可控、可测、可加
  "把每个控件的最终矩形打进日志"这种**给自动化用的锚点** —— 这正是项目一贯的做法
  （`VP-RECT` / `INS-ANIM-TIME` / `BAR-TRAY-*`）。

**结论**：自研。但布局能力**分级**：U4 只做"锚点 + 尺寸模式 + row/column"，
不做完整 Flexbox（`flex-grow/shrink/basis` 那一套）。

### 3.7 圆角 / 边框用 SDF 几何直绘，不要九宫格贴图

`sdRoundBox(p, halfSize, radius)` 在 fragment 里算距离 → 一个全屏级的
"圆角矩形"只需 4 个顶点。血条、按钮、面板底、进度条全部可以零贴图。
**贴图只用于图标和字形**。

这条能砍掉一整个"UI 素材制作"环节（否则每个 UI 元素都要一张 PNG + 九宫格参数）。

### 3.8 颜色空间：交换链是 **sRGB**，UI shader 必须做一次反编码

`engine/rhi/Swapchain.cpp:42-48` 优先选 `B8G8R8A8_SRGB`。这意味着：

- 附件是 sRGB 格式 → 硬件在写入时自动做 **linear → sRGB** 编码；
- 如果 shader 直接输出 `0.5`（当作 sRGB 值），屏幕读到的是 **188**，不是 128；
- 正确做法：shader 里 `linear = pow(srgb, 2.2)`（或用精确的 `srgbToLinear`），
  硬件再编码回去 → 屏幕上就是设计时填的那个色号。

`ship_roadmap` G15 验收第 1 条已经提醒"按 sRGB 二次编码后的实测值定阈值"，
但没说**怎么修**。修法就是上面这句。U2 的验收会钉住它。

### 3.9 布局的坐标系与缩放

- **逻辑像素**：UI 全部用"参考分辨率下的像素"描述（`refHeight = 1080`）。
- **缩放**：`uiScale = min(H / 1080, W / 1920)`（可切 `matchHeight` 只按高度）。
- **顶点**：`ndc.x = px / W * 2 - 1`，`ndc.y = 1 - py / H * 2`（y 向下），
  除以的是**输出分辨率**（`m_outExtent`，`PostProcess.cpp:648`），
  与 3D 的 DRS 无关 —— 满足"UI 不跟 DRS"的契约。
- ⚠ 窗口最小化时 `extent = 0` → 除零。`Renderer` 已有 `if (extent.width == 0) return;`
  的守卫，UI 要沿用。

### 3.10 伤害数字：世界 → 屏幕投影的三个坑

```cpp
// w <= 0 = 在相机背后，必须整体丢弃（不能只 clamp）
vec4 clip = viewProj * vec4(worldPos, 1.0);
if (clip.w <= 1e-5) discard;
vec2 ndc = clip.xy / clip.w;
```

1. **背后剔除**：`w <= 0` 时 `ndc` 会翻转，数字会出现在屏幕上完全错误的位置。
2. **屏幕外钳制**：目标在屏幕边缘外时，数字要么钳到边上（带箭头），要么不画。
   不能让它飘到 `[0,W]` 之外（scissor 会裁掉，但布局会算错）。
3. **遮挡**：数字要不要被墙挡住？第一版**不做**（需要深度测试，而 UI pass 没有深度附件）。

---

## 4 · 目标架构

```
engine/
  ui2/                        ← 新建（与 ui/ 并列：ui/ 是编辑器 ImGui，ui2/ 是游戏 UI）
    Ui2Settings.h/cpp           FeatureRegistry 登记（ui.enabled / ui.scale / ...）
    SdfFont.h/cpp               图集加载 + 字形度量 + measure()/wrap()
    UiAtlas.h/cpp               图集页（单页 2048²，多页留接口）
    UiRenderer.h/cpp            顶点/索引环形缓冲 + 批合并 + scissor + pipeline
    UiCanvas.h/cpp             绘制命令：rect / roundRect / line / image / text
    UiLayout.h/cpp             锚点 + 尺寸模式 + row/column + 参考分辨率
    UiWidget.h/cpp             控件树：Panel/Text/Button/ProgressBar/Image/NineSlice
    UiFocus.h/cpp              焦点栈 + 方向键导航 + 指针捕获 + consumed
  i18n/
    I18n.h/cpp                 语言枚举 + tr() + setLanguage() + addTranslation()
    ZhTable.cpp                609 条词典（从 editor/Localization.cpp 原样搬）

assets/
  ui/fonts/noto_sc_sdf.png     构建期生成（R8，2048²）
  ui/fonts/noto_sc_sdf.json    字形度量表
  ui/atlas.png                 图标图集（可选，先空）
  shaders/ui2.vert / ui2.frag

runtime/
  ui/
    Hud.h/cpp                  血条 / 准星 / 伤害数字（对象池）
    HudModel.h                 ▸ 纯数据 POD，脚本/GameState 填
    PauseMenu.h/cpp            继续 / 设置 / 退出

tools/
  gen_sdf_atlas.py             离线：TTF → SDF 图集 + JSON
  check_font_atlas.py          断言：图集覆盖词典里所有汉字
  verify_ui2.py                GUI 回归
  __init__ 的 ui2 自检：MYVK_UI2_TEST=1（无窗口，毫秒级）
```

### 数据流（一条血条的完整路径）

```
脚本 hp = hp - 10
   → HudModel.health（纯数据）
   → Hud::build(UiCanvas&, HudModel&)      // 只读模型，产出绘制命令
   → UiLayout::arrange(root, viewport)     // 布局算矩形
   → UiCanvas 吐出顶点（pos/uv/color/mode）
   → UiRenderer::flush(cmd)                // 合批 → 1~3 个 draw call
   → Post 输出 pass 内、ImGui overlay 之前
```

**关键约束**：`Hud` 不认识 ECS，`UiRenderer` 不认识 Scene，`Scene` 不知道 UI 存在。
三者之间只有 `HudModel` 一个 POD。

### 与编辑器 ImGui 的关系

| | 编辑器 ImGui | 游戏 UI（ui2） |
|---|---|---|
| 驱动者 | `EditorApp::onImGui` | `RuntimeApp` / `EditorApp` 的 Play 态 |
| 渲染挂点 | Post pass 内最后一个回调 | Post pass 内**倒数第二个**回调 |
| 字体 | ImGui 私有位图图集 | 自己的 SDF 图集 |
| 输入 | ImGui `io` | `UiFocus` + `InputMap` 的 `UI` 上下文 |
| 默认 | 永远开 | 运行时开；**编辑器视口默认关**（`ui.hudInEditor=false`） |

两者**共存但不共享任何资源**。编辑器 UI 永远盖在游戏 UI 之上（因为画在它后面）。

---

## 5 · 路线图 U0 ~ U8

> 与 A0 一样，**每个阶段都要过"特性准入五件套"**：
> ① 实现 ② `FeatureRegistry` 登记 ③ UI 面（点得到）④ 序列化 ⑤ 回归用例 + `TRIGGERS`。
> 只有 `MYVK_*` 开关、界面点不到的，不算交付。
>
> ⚠ 横切文件（`engine/render/Renderer.*`、`engine/ecs/**`、`engine/core/**`、
> `engine/render/FeatureRegistry.*`）**每动一次都应视作触发全量回归**。
> 但按项目纪律：**不自动跑全量**，等明确指令。策略建议见 §9。

---

### U0 · 地基量测与骨架（规模 S）

**目标**：把"能不能做"变成"量过了"，并把新目录/新开关立起来。**不写任何渲染代码。**

**改动**
1. `engine/ui2/Ui2Settings.{h,cpp}`：参数 + `FeatureRegistry` 登记
   - `ui.enabled`（Bool，默认 **true**：运行时 UI 系统总开关）
   - `ui.hudInEditor`（Bool，默认 **false**：编辑器视口是否画 HUD）
   - `ui.scale`（Float，默认 1.0，范围 0.5~3.0）
   - `ui.debugBounds`（Bool，默认 false：把每个控件矩形画成描边）
   - `ui.fontSize`（Float，默认 16，范围 8~64）
   - ⚠ `bindParam` 要在成员默认初始化**之后**、`loadEnvOverrides()`**之前**。
2. `CMakeLists.txt` 的 `myvk_copy_runtime_assets` 加 `assets/ui` 一项
   （现有列表只有 models/textures/icons/fonts/game.json —— 不加则运行时找不到图集）。
3. 一行能力日志（`Renderer` 初始化时）：
   ```
   UI2-CAPS maxBoundDescriptorSets=%u maxPerStageDescSamplers=%u
            swapFormat=%d outExtent=%ux%u
   ```
4. **不建** `UiRenderer`，不建 shader。

**验收**
- `UI2-CAPS` 锚点出现，`maxBoundDescriptorSets >= 2`（UI 只绑 1 个 set，留 1 个余量）。
- 开关全默认时，编辑器与运行时的截图与改动前**逐位相同**（`ui.enabled=true` 但没有 UI 数据
  = 零绘制，不进任何 pass）。
- `check_feature_surface.py` 通过（新参数都有 UI 面 → 需要先在 `Inspector` 的
  Render 页加一个 "UI" 分区，哪怕只有一个 checkbox；这是"UI 面"硬要求）。
- `check_regression_map.py` 通过（`engine/ui2/**` 进 `GLOBAL_GLOBS`？→ **不进**。
  `ui2` 是**只在 UI 通道用**的模块，不该让所有用例因它变红。登记到 `verify_ui2` 的
  `TRIGGERS` 里即可）。

**否定测试**：无（还没功能）。但有**守卫**：默认关时若截图有差 → 直接红。

---

### U1 · SDF 字体离线管线（规模 M）

**目标**：`assets/ui/fonts/*.png + *.json` 存在、可加载、字形度量正确。**还不画到屏幕上。**

**改动**
1. `tools/gen_sdf_atlas.py`
   - 输入：OFL 字体 TTF（`third_party/fonts/NotoSansSC-Regular.otf` 或
     `SourceHanSansSC-Regular.otf`，**随仓库提交**）
   - 字符集：ASCII 0x20~0x7E + 拉丁补充 + **词典里出现的全部汉字**（从
     `engine/i18n/ZhTable.cpp` 自动扫描）+ `，。！？：；、“”‘’（）《》…—` 
     + 数字/百分号（伤害数字要用）
   - 流程：轮廓 → 高分辨率栅格化（4× 超采样）→ **EDT 距离场** → 归一化到 R8
     → shelf packing 到 2048² → 导出 PNG + JSON
   - JSON：
     ```json
     {"font":"Noto Sans SC","size":48,"channels":1,"sdfRange":8,
      "atlas":{"w":2048,"h":2048},
      "metrics":{"ascent":38,"descent":10,"lineGap":0},
      "glyphs":{"0x8840":{"uv":[0,0,48,48],"adv":26,"bearing":[0,-38]}}}
     ```
2. `engine/ui2/SdfFont.{h,cpp}`：加载 JSON + PNG（走现有 `assets::resolveAssetPath` +
   现有的 stb_image；图集走 `VK_FORMAT_R8_UNORM`，`srgb=false` —— 
   **SDF 是数据不是颜色**，不能当 sRGB 采样）。
3. `tools/check_font_atlas.py`：断言
   - 图集非空、边长 2048、R8；
   - `glyphs` 数量 ≥ 900；
   - 关键字符全在：`血 量 暂 停 继 续 设 置 退 出 伤 害` + `0123456789%/-`；
   - **扫描 `ZhTable.cpp` 的译文，任何汉字不在图集里 → 红**（这条防"切中文出现方块"）。

**验收**
- `MYVK_UI2_TEST=1` 输出 `UI2-FONT glyphs=%d atlas=%dx%d range=%.1f result=PASS`。
- 无窗口自检 3 项：`measure_ascii`（"A" 的宽度与解析值一致 ±0.5px）、
  `measure_cjk`（"血" 宽度 ≈ 字号）、`missing_glyph`（未收录字符返回 `.notdef` 宽度，**不崩**）。
- ⚠ **否定测试**：把 `sdfRange` 从中值改成 2× → `measure_*` 不变但
  `edge_sharpness` 自检（栅格化 16px 后边缘梯度）必须变化 → 证明 range 真的进了 shader 常量。

**交付物**：一张 PNG（约 2~4MB）+ 一个 JSON（约 200KB）+ 生成脚本（可复现）。

---

### U2 · UiRenderer：正交批 + 管线 + 挂点（规模 L）

**目标**：**屏幕上出现一块矩形**。这是整个路线图的"Hello World"，也是最难的一段。

**改动**
1. `assets/shaders/ui2.vert / ui2.frag`
   - 顶点：`vec2 pos`（像素）、`vec2 uv`、`uint color`（RGBA8）、`vec2 p1`、`float mode`
     - `mode = 0` 纯色矩形，`1` 贴图，`2` SDF 文字，`3` SDF 圆角矩形
   - frag：
     ```glsl
     // sRGB 附件：先反编码，硬件会再编码回去
     vec3 c = srgbToLinear(color.rgb);
     if (mode == 3) { float d = sdRoundBox(...); alpha *= smoothstep(...); }
     if (mode == 2) { float d = texture(atlas, uv).r; alpha *= smoothstep(edge-w, edge+w, d); }
     ```
2. `engine/ui2/UiRenderer.{h,cpp}`
   - 顶点格式 **32 字节**（对齐到 4）
   - **每帧环形缓冲**（frames-in-flight 3 份 × 4MB，`HOST_COHERENT`，map 一次常驻）
   - 合批键 = `(atlas, scissor, mode)`；`flush()` 时连续相同键合并
   - scissor 用 `VkRect2D` + `vkCmdSetScissor`（管线开 `dynamic scissor`）
   - 混合：`SRC_ALPHA / ONE_MINUS_SRC_ALPHA`，颜色不预乘（预乘会在 sRGB 下出错）
   - 独立 descriptor set layout（**1 个 set**：`combinedImageSampler` @ binding 0）
   - descriptor pool：**自建**（per-frame，3 组），不从全局池拿
3. `engine/render/PostProcess.cpp`
   - `renderScene(...)` 签名加 `const std::function<void(VkCommandBuffer)>& gameUi`
   - 模式 A：`recordComposite(..., [&](VkCommandBuffer c){ gameUi(c); overlay(c); })`
   - 模式 B：`recordComposite(cmd, m_sceneOutput[fi].fb, m_offscreenRenderPass, ...,
     &gameUi)`（现在传的是 `nullptr`）
   - ⚠ **`renderUiToSwapchain` 里绝对不加 gameUi**（那条路径画到交换链，会盖住编辑器面板）
4. `engine/render/Renderer.cpp`
   - 建 `m_uiRenderer`（懒建，`ui.enabled` 首次为真时）
   - `recordFrame` 里组 `gameUi` 回调；编辑器模式下 `ui.hudInEditor=false` → 传空
5. 调试入口：控制台 `ui.rect x y w h color`（画一个方块，给自动化用）

**验收**
- **像素判据**：`ui.rect` 画一个铺满视口的纯色 `#808080` →
  取中心 1×1：
  - 若附件是 sRGB：shader 输出前做了 `srgbToLinear(0.5)` → 硬件编码回来 → **实测 128±1**；
  - 若漏掉反编码 → 实测 **188±2**。
  - 断言必须**实测**这两个值之一并锁定，不能凭"应该是 128"。
- **顺序契约**：开 TAA + 自动曝光，UI 区域像素与关 TAA 时**逐位相同**。
  （游戏 UI 画在 FXAA 之后，天然不受影响；一旦有人把它挪进后处理链，这条必红。）
- **`ui.enabled=false`** → 截图与改动前**逐位相同**。
- **管线兼容**：`MYVK_UI2_TEST=1` 的 `pipeline_compat` 项 —— 用同一份 pipeline
  分别在 `m_postRenderPass` 与 `m_offscreenRenderPass` 上各画一帧，验证层零 VUID。
- **draw call**：单图集 + 无 scissor 变化时 `UI2-STATS draws=1`。

**否定测试**
| 回退 | 应红的断言 |
|---|---|
| shader 去掉 `srgbToLinear` | 中心像素 188 而非 128 |
| 把 `gameUi` 挪到 `recordComposite` **之前** | TAA/曝光像素一致性 |
| 合批键去掉 `atlas` | `draws==1`（两页图集时变 2） |
| 在 `renderUiToSwapchain` 里也画 gameUi | 编辑器截图出现 HUD（像素差） |

---

### U3 · 文本排版（规模 M）

**目标**：能画一段文字，换行、对齐、行高都对。

**改动**
1. `UiCanvas::text(pos, utf8, size, color, align, maxWidth)`：
   - UTF-8 解码（**在字符边界二分截断** —— 项目在 Hierarchy 列头踩过非边界截断的坑）
   - 字形推进 = `advance * (size / atlasSize)` + kerning（先不做 kerning，CJK 不需要）
   - 换行：按空格 + **CJK 逐字断行**；行首禁则（`，。！？：；、` 不能出现在行首）→
     第一版可以先不做禁则，但**留 TODO 和断言位**
2. `UiCanvas::measure(utf8, size) → vec2`
3. 阴影/描边：同一段文字画 2 次（偏移 + 不同 alpha），或者 shader 里 `mode=4`。
   第一版用"画两次"，简单且可复用。
4. `assets/shaders/ui2.frag` 加 `mode=2` 的 SDF 采样 + `smoothstep` 宽度
   （宽度要按 `size/atlasSize` 缩放，否则小字会糊、大字会有硬边）。

**验收**
- `UI2-TEXT` 锚点：`text="血量" size=32 w=%.1f h=%.1f lines=%d`
- 无窗口自检：
  - `measure_ratio`：同一串在 16px 与 32px 下宽度比 = 2.0 ± 2%
  - `wrap_cjk`：`"这是一段很长的中文文本用于测试换行"` 在 maxWidth=200 下 → `lines == 3`（±1）
  - `utf8_boundary`：截断位置必须落在字符边界（把截断点喂给解码器不产生 U+FFFD）
- 像素：`ui.text "血"` 在 16/32/64px 下都能看见（非空像素占比 > 2%），
  且**边缘锐利度**（梯度最大值）随字号增长不衰减 —— 这条证明 SDF 生效。

---

### U4 · 布局（规模 M）

**目标**：同一套 UI 在 1280×720 / 1920×1080 / 3840×2160 下锚点位置按比例一致。

**改动**
1. `UiLayout.{h,cpp}`
   - 节点：`Node{ rect, anchor{min,max}, offset, sizeMode{Fixed,Fit,Fill}, dir{Row,Column}, padding, gap }`
   - **单遍 arrange**：先 `measure`（自底向上算 fit 尺寸），再 `arrange`（自顶向下定位）
   - 参考分辨率：`UiLayout::setReference(1920,1080)` + `scale = min(W/refW, H/refH) * ui.scale`
   - 锚点 9 宫格 + `offset`（像素，已乘 scale）
2. `ui.debugBounds=1` → 每个节点画描边 + 打一行 `UI2-BOUNDS id=%s x=%d y=%d w=%d h=%d`
   （自动化锚点，仿 `VP-RECT`）

**验收**
- **分辨率无关**：同一 UI 在 1280×720 与 1920×1080 下，左下角血条的
  `x/(W), y/(H), w/(W), h/(H)` 四个比值差 **< 0.2%**（≈ ±1px）。
- **锚点**：`UI2-BOUNDS id=healthbar ...` 的 `x+w` 与"右边缘 - 24*scale"一致（±1px）。
- **否定测试**：把锚点改成绝对像素 → 上面两条必红。

---

### U5 · 控件与交互态（规模 M）

**目标**：按钮能点、进度条能涨、面板能画底。

**改动**
1. `UiWidget.{h,cpp}`：`Panel / Text / Button / ProgressBar / Image / Toggle`
2. 交互态：`normal / hover / pressed / disabled`（每态一个色值）
3. `UiCanvas` 加 SDF 圆角矩形 + 1px 边框（`sdRoundBox` + `abs(d)-borderWidth`）
4. 事件：`UiFocus::hitTest(x,y)` → 返回控件 id；`onClick` 回调（`std::function`）
5. 锚点：`UI2-CLICK id=resume`、`UI2-HOVER id=resume`

**验收**
- 点击按钮 → `UI2-CLICK id=...` 出现 1 次（不是 2 次 —— `pressed` 和 `released` 要分清）。
- 悬停时按钮底色变化：截图差异 > 0.5% 显著像素（复用 A0 学到的
  "显著变化像素占比"度量，不要用整图平均差）。
- 血条 50% → 像素上"亮部宽度 / 轨道宽度" = 0.5 ± 2%（**量几何，不是量颜色**）。

---

### U6 · 输入与焦点（规模 L）

**目标**：暂停菜单打开时，角色**一个像素都不动**；方向键能在按钮间移动。

**改动**
1. `UiFocus.{h,cpp}`
   - 焦点栈（`std::vector<WidgetId>`，栈顶响应键盘）
   - 方向键导航：按控件矩形的**几何关系**找最近邻（不是按数组顺序 —— 数组顺序会让
     "上下左右"变成"随便跳"）
   - `Tab` / `Shift+Tab` 循环
   - `route(mouse, keys) → bool consumed`
2. `InputMap` 对接
   - 已有 `UI` 上下文（`InputMap::makeDefault()` 里应该有"暂停"）；暂停时 `push("UI")`
   - **顺序**：`UiFocus::route()` 必须在 `PlayerController::update()` **之前**调用
3. `RuntimeApp::onUpdate`：UI tick 放在**状态栈之外**
   ```cpp
   // 每帧都跑，不受 blocksUpdate 影响 —— 否则暂停菜单收不到输入
   tickUi(dt);
   m_stateStack.update(m_svc, dt);   // Paused 会截断下层的 update
   ```
4. 指针捕获：`cursorVisible` / `clipCursor`（暂停时显示光标、锁在窗口内）

**验收**
- **暂停时角色不动**：脚本记录玩家位置 → 开暂停菜单 → 按 W 两秒 → 
  `PLAYER-POS` 前后**逐位相同**（不是"差不多"）。
- **焦点导航**：菜单有三项，按 ↓ 两次 → `UI2-FOCUS id=quit`；再按 ↓ 回到 `resume`（循环）。
- **消费语义**：鼠标点在按钮外 200px 处 → `consumed=false` → 视角仍能转（对照）。
- **否定测试**：把 `tickUi` 挪到 `m_stateStack.update()` 之后 → 暂停菜单点不动（锚点缺失）。
  把 `route()` 挪到 `PlayerController` 之后 → 暂停时位置变化（断言红）。

---

### U7 · 运行时 i18n 搬家（规模 M）

**目标**：`runtime` 能 `tr("Health")` 拿到「生命」；`editor` 的 792 处 `TR()` **零改动**。

**改动**
1. `engine/i18n/I18n.{h,cpp}` + `ZhTable.cpp`
   - `kZh[]` 609 条 **原样搬**（含 `%` 占位符、`\n`）
   - API：`i18n::tr() / setLanguage() / language() / languageCount() / languageName()`
   - `namespace i18n`（不是 `engine`，避免与 `editor::` 撞）
2. `editor/Localization.{h,cpp}` 变成**转发壳**：
   ```cpp
   const char* tr(const char* en) { return i18n::tr(en); }
   void setLanguage(int i) { i18n::setLanguage(i); }
   ```
   **保留 `editor::tr` 这个符号** → 792 处 `TR(...)` 与所有 `trId()` 调用点**一个字都不用改**。
   `trId` 留在 editor（它服务于 ImGui 的窗口 ID，天然是编辑器概念）。
3. 运行时语言来源
   - `RuntimeConfig` 加 `language`（`assets/game.json` 可配，`en` / `zh`）
   - 控制台/设置界面能切（G20 的运行时设置还没做 → 先用 env `MYVK_LANG=zh` 与
     `game.json`，留 `i18n::addTranslation()` 给脚本）
4. ⚠ **不复制词典**。两份一定漂移（A0 已经证明"同一个实现抄两份"会被忘掉一处：
   `applySubTransform` 当初就是两份）。**单一权威在 `engine/i18n/ZhTable.cpp`**。
5. `tools/check_i18n.py`：扫描 `ZhTable.cpp` 的 `%` 占位符与英文是否逐字符一致
   （数量、顺序、类型）—— 这条以前只能靠人眼，现在可以机器判。

**验收**
- `I18N-KEY lang=zh count=609`（启动日志）与英文模式 `count=0`（不建表 = 零开销）。
- 同一界面在 `MYVK_LANG=zh` 下与英文下截图**不同**（显著像素 > 0.5%）；
  切回英文后与原始截图**逐位相同**（证明"没有译文时原样返回"）。
- `check_i18n.py` 全绿（% 占位符零漂移）。
- ⚠ 与 U1 的耦合：`check_font_atlas.py` 必须证明**词典里所有汉字都在图集里**
  → 否则切中文是方块雨。这条是 U1/U7 的**联合验收**。

---

### U8 · HUD 与暂停菜单（规模 L）

**目标**：能玩的画面 —— 血条、准星、伤害数字、暂停菜单。

**改动**
1. `runtime/ui/HudModel.h`（纯数据）
   ```cpp
   struct DamagePopup { glm::vec3 worldPos; float amount; float t; };  // t: 0..1 寿命
   struct HudModel {
       float health = 100, maxHealth = 100;
       bool showCrosshair = true;
       std::vector<DamagePopup> popups;   // 固定容量 32，环形复用
       int ammo = -1;                     // <0 = 不显示
       std::string hint;                  // 交互提示 "[E] 开门"
   };
   ```
2. `runtime/ui/Hud.{h,cpp}`
   - `build(UiCanvas&, const HudModel&, const mat4& viewProj, extent)`
   - 血条：左下锚点，SDF 圆角 + 双色（掉血部分延迟回落 = "白色残影"，1 行插值）
   - 准星：中心，4 条线 + 中心点（SDF 直绘，不用贴图）
   - 伤害数字：世界 → 屏幕（§3.10 的三个坑），**上浮 + 淡出**，对象池
   - `HUD-STATS popups=%u health=%.1f`
3. `runtime/ui/PauseMenu.{h,cpp}`
   - 三项：继续 / 设置 / 退出；`blocksUpdate=true, blocksRender=false`
   - 背景：半透明黑（`sdRoundBox` + alpha 0.6）—— **不模糊**（模糊要额外 RT，先不做）
   - ESC 开/关（`InputMap` 的 `pause` 动作已经有了）
4. `RuntimeApp`：`m_hud` / `m_pause` 成员；`HudModel` 每帧从 GameServices 填
5. 序列化：`config/ui.json`（血条位置、缩放、准星样式）—— **不进场景 JSON**
   （HUD 不是场景对象；进场景会让"打开别的关卡 HUD 配置被覆盖"）
6. 脚本接口（先做读写字段，G26/G27 之后再做事件）：
   ```lua
   hud.health = 50
   hud.popup(entity, 12)
   ```

**验收（一句话场景）**
> `Game.exe` 打开关卡 → 左下角有血条，中心有准星；
> 脚本扣血 30 → 血条宽度缩短到 70%（量几何）；
> 击杀木桶 → 伤害数字出现在木桶上方，1 秒内消失；
> 按 ESC → 暂停菜单出现、物理冻结、按 W 角色不动、方向键能选"继续"；
> 再按 ESC → 菜单消失、角色能动。

**否定测试**
- 把伤害数字的 `w<=0` 剔除删掉 → 相机背后的目标会在屏幕上冒数字（断言"相机背后不画"红）。
- 把 UI tick 挪到状态栈之后 → 暂停菜单点不动。
- 把 `ui.hudInEditor` 默认改成 true → 编辑器像素基线全红（这条守卫生效）。

---

### U9（可选，不在本轮）· 编辑器集成

- 视口内直接拖 UI（需要 gizmo + 选中）
- UI 存成资产（`.ui.json`）→ 需要 G18 Prefab 的子树序列化
- 实时预览 + 数据绑定可视化

**不做**，理由：先把"运行时能玩"打通，编辑器再吃这层红利。

---

## 6 · 锚点总表（自动化用）

| 锚点 | 时机 | 断言什么 | 用在 |
|---|---|---|---|
| `UI2-CAPS maxBoundDescriptorSets=.. swapFormat=..` | 初始化 | 能力足够、set 没超 | U0 |
| `UI2-FONT glyphs=.. atlas=..x.. range=..` | 字体加载 | 图集非空、字形够 | U1 |
| `UI2-TEST <name> actual=.. expected=.. tol=.. result=PASS` | `MYVK_UI2_TEST=1` | 度量/布局/焦点数值 | U1~U6 |
| `UI2-STATS draws=.. verts=..` | 每 60 帧 | 合批有效（draws ≤ 4） | U2 |
| `UI2-TEXT text=".." size=.. w=.. h=.. lines=..` | 每次排版 | 文本测量 | U3 |
| `UI2-BOUNDS id=.. x=.. y=.. w=.. h=..` | `ui.debugBounds=1` | 布局矩形 | U4 |
| `UI2-CLICK id=..` / `UI2-HOVER id=..` | 交互 | 点击/悬停命中 | U5 |
| `UI2-FOCUS id=..` | 焦点变化 | 导航正确 | U6 |
| `I18N-KEY lang=.. count=..` | 启动 | 词典条数 | U7 |
| `HUD-STATS popups=.. health=..` | 每 60 帧 | HUD 数据流 | U8 |
| `GAME-STATE push=Paused` | 状态迁移 | 暂停真的进了栈 | U8 |

⚠ **锚点纪律**（项目已踩过四次）：
1. 锚点必须 **ASCII 冒号 + 空格分隔的 `k=v`**，脚本用正则抓；
   `detail=` 之类的**不能含空格**（A0 的 `ANIM-TEST` 就栽在这里，引擎 PASS 脚本判 FAIL）。
2. 负向断言只能用"提前返回**之后**"的锚点（`INS-*` 的教训）。
3. 脚本结尾必须是 `结果: PASS` / `结果: FAIL: ...`（ASCII 冒号，`run_regression.py` 取最后一条）。
4. 锚点拼错 = 断言永远为假（`BAR-TRAY-tray_particles` 的教训）→ 改完必须跑一遍看到它变绿。

---

## 7 · 风险与坑（预判）

| # | 风险 | 症状 | 对策 |
|---|---|---|---|
| 1 | **sRGB 双重编码** | UI 颜色比设计稿亮一大截（128 → 188） | shader 里 `srgbToLinear`，验收钉实测值 |
| 2 | **`finalLayout=PRESENT_SRC`** | 想在 Post 之后新开 pass → 验证层 VUID-.. 或黑屏 | 只挂在 Post 输出 pass 内、ImGui 之前 |
| 3 | **管线兼容性脆断** | 将来给输出 pass 加附件/改采样数 → UI pipeline 失效 | U0 的 `pipeline_compat` 自检钉住 |
| 4 | **帧内动态顶点缓冲** | 3 帧 in-flight 共用一块 → 竞态闪烁 | 每帧一套环形缓冲，**不跨帧复用** |
| 5 | **CJK 字形覆盖不全** | 切中文出现方块 | `check_font_atlas.py` 扫词典，缺字即红 |
| 6 | **字体许可证** | 打包分发侵权 | 只用 OFL（Noto/思源/文泉驿），随仓库提交 |
| 7 | **图集尺寸爆炸** | 2048² 装不下 2500 字 | 只收"词典里出现的字"（约 600~900），不是全 CJK |
| 8 | **窗口最小化** | `extent=0` → 除零 / NaN | 沿用 Renderer 的 `width==0 return` 守卫 |
| 9 | **UI 抢输入** | 点按钮同时角色跳 | `consumed` 语义 + `route()` 必须在玩家控制之前 |
| 10 | **暂停时 UI 停更** | 菜单点不动 | UI tick 放在状态栈**之外** |
| 11 | **编辑器像素基线** | `ui.hudInEditor` 默认 true → 所有截图红 | 默认 false，play 态才可选开 |
| 12 | **伤害数字在相机背后** | 数字出现在错误位置 | `clip.w <= 0` 整体丢弃 |
| 13 | **`-0.0f` / 浮点相等** | 布局断言随机红 | 比较用绝对值差 + 容差，不用 `==`（A0 的 `dt_zero_no_advance` 教训） |
| 14 | **ImGui 与游戏 UI 同帧顺序** | HUD 盖住编辑器面板 | 先 gameUi 后 ImGui，且编辑器模式 HUD 默认关 |

---

## 8 · 回归与测试策略

### 8.1 三类用例

| 类型 | 跑法 | 覆盖 | 时长 |
|---|---|---|---|
| **无窗口自检** | `MYVK_UI2_TEST=1 Editor.exe` | 字体度量、布局、焦点导航、合批计数 | 毫秒级 |
| **GUI 像素** | `tools/verify_ui2.py` | 上屏、颜色、分辨率无关、交互态 | ~40s（5 次启动） |
| **端到端** | `tools/verify_hud.py` | 血条/伤害数字/暂停菜单 + 状态冻结 | ~30s（3 次启动） |

⚠ GUI 用例**同时只能有一个**（`kill_stale_editors` 会无差别杀 `Editor.exe`/`Game.exe`）。

### 8.2 像素判据（复用 A0 的结论，不要再踩）

- **不要用整图平均差**：UI 元素只占屏幕几个百分点，平均差会被稀释到没有判别力。
- 用**显著变化像素占比**：`ImageChops.difference` → 直方图 → 阈值 8 → 
  `ratio = sum(h[9:]) / total`，裁到**受影响的矩形**（不是全图）。
- 判据 = `ratio > 0.5%` **且** `> 10 × 同配置两跑的基线`（自校准）。
- 纯色矩形的中心像素是**例外**：那个用"实测值 ±2"直接比（几何量，不是统计量）。

### 8.3 `TRIGGERS` 登记（改完必须跑 `check_regression_map.py`）

```
engine/ui2/**            → verify_ui2
runtime/ui/**            → verify_hud
engine/i18n/**           → verify_ui2（i18n 组）+ verify_inspector（编辑器切语言不能崩）
assets/shaders/ui2.*     → verify_ui2
tools/gen_sdf_atlas.py   → verify_ui2（字体图集变了，所有文本断言都要重跑）
```

`DEFAULT` 里加 `verify_ui2`、`verify_hud`。

⚠ **`engine/ui2/**` 不进 `GLOBAL_GLOBS`**：它只被 UI 通道引用，
不像 `Renderer.*` / `Components.h` 那样横切。放进 `GLOBAL_GLOBS` 会让全量永远点名，
失去"增量"的意义。

### 8.4 否定测试清单（每条都必须真的跑一遍看到红）

| 阶段 | 注入的错误 | 应红的断言 |
|---|---|---|
| U2 | shader 去掉 `srgbToLinear` | 中心像素 188 而非 128 |
| U2 | `gameUi` 挪到后处理链之前 | TAA 开关下 UI 像素不一致 |
| U2 | 合批键去掉 atlas | `draws==1` 失败 |
| U3 | SDF `smoothstep` 宽度不按字号缩放 | 小字模糊度 / 大字硬边 |
| U4 | 锚点改绝对像素 | 1280/1920 比例一致 |
| U5 | `pressed` 与 `released` 都触发 click | `UI2-CLICK` 出现 2 次 |
| U6 | `route()` 挪到玩家控制之后 | 暂停时角色位置变化 |
| U6 | UI tick 挪到状态栈之后 | 暂停菜单点不动 |
| U7 | 词典里改一个 `%s` → `%d` | `check_i18n.py` 红 |
| U8 | 删掉 `w<=0` 剔除 | 相机背后出现伤害数字 |

---

## 9 · 与其它路线图的依赖 + 全量回归策略

### 依赖

| 本路线 | 依赖 | 说明 |
|---|---|---|
| U0~U5 | 无（可立即开工） | 纯渲染 + 布局，不碰玩法 |
| U6 | ✅ G06 `InputMap` | 已完成（`engine/core/InputMap` 有上下文栈） |
| U6 | G07 玩家控制器 | 已完成（`PlayerController`），只是在它**之前**插一层 |
| U8 | `StateId::Paused` | 已存在（`runtime/GameState.h:71`），只需接 UI |
| U8 | G14 音频（可选） | 按钮音效；没有也能做完 |
| U9 | G18 Prefab | 才需要 UI 资产化 |

### 全量回归策略

本路线**必然**要动这些横切文件：

- `engine/render/PostProcess.{h,cpp}`（**加参数**，签名变更 → 所有调用点）
- `engine/render/Renderer.{h,cpp}`（新成员 + 回调组装）
- `engine/render/FeatureRegistry.{h,cpp}`（新特性登记，在 `GLOBAL_GLOBS`）
- `CMakeLists.txt`（资产拷贝）
- `editor/Localization.{h,cpp}`（变转发壳 —— 792 处调用点虽然不改，但**行为路径变了**）

→ **A0 + U0~U2 应该合并成一次全量**。理由：A0 已经改了 `Renderer` 的
`updateSkinning` / `buildDrawLists` **顺序**和阴影两条路径的签名，U2 又要再动
`PostProcess` 的签名 —— 这两批改动落在同一片代码上，分开跑等于把同一片区域验证两次，
还多一次归因成本。

**建议的批次**：
1. **第一批全量**：A0（已完成）+ U0 + U1 + U2 → 确认"UI 通道加进来了，别的东西没红"。
2. **第二批**：U3~U6（纯 `ui2` 内部 + 少量 `RuntimeApp`）→ 增量跑 `verify_ui2` 即可。
3. **第三批全量**：U7（i18n 搬家，**动了 792 处调用的底层**）+ U8。

⚠ 按纪律：**不自动跑全量**，等明确指令。上面只是建议的分批点。

---

## 10 · 验收总表（A0 风格：一句话场景）

**U0~U2（M1 地基）**
> 控制台 `ui.rect 0 0 1920 1080 #808080` → 整个画面变成灰色，
> 取中心像素实测 128（sRGB 正确）；开 TAA 再跑一次 → 同一个像素**不变**；
> `ui.enabled 0` → 画面回到 3D，与改动前逐位相同。

**U3~U5（M2 内容）**
> `ui.text "血量 100/100"` 出现在屏幕左上；把窗口从 1280×720 拉到 1920×1080，
> 它相对左边缘的距离按比例变化（±1px）；点一下旁边的按钮 → 按钮变暗一帧再恢复。

**U6~U8（M3 交互）**
> `Game.exe` 打开关卡：左下血条、中心准星；脚本扣血 30 → 血条 70%；
> 打木桶 → 数字上浮淡出；ESC → 暂停菜单、物理冻结、W 不动；
> 方向键选"退出" → 回车 → 进程正常退出（不是崩）。

**横向硬约束（每一批都要过）**
1. `ui.enabled=0` → **逐位不变**。
2. `ui.hudInEditor=0`（默认）→ 编辑器截图**逐位不变**。
3. `check_feature_surface.py` 全绿（新参数 34+ 项全部有 UI 面、裸 env 零漏网）。
4. `check_regression_map.py` 全绿（新用例进 `DEFAULT` + `TRIGGERS` 同步）。
5. `MYVK_UI2_TEST=1` 无窗口自检 `fail=0`。
6. 所有新增断言都做过**否定测试**并确认变红过一次。

---

## 11 · 工作量估算（按"规模"排序，不给天数）

| 阶段 | 规模 | 主要成本 | 可并行 |
|---|---|---|---|
| U0 | S | 参数 + 拷贝 + 日志 | 否（前置） |
| U1 | M | 离线生成器（距离场 + 打包）是唯一"算法活" | ✅ 可与 U2 并行（接口先定） |
| U2 | L | 管线 + 环形缓冲 + 挂点 + sRGB | 否（最难，必须单独做） |
| U3 | M | UTF-8 / 换行 / SDF 采样 | ✅ U4 可以并行 |
| U4 | M | 单遍 measure+arrange | ✅ |
| U5 | M | 控件 + 交互态 | 依赖 U3/U4 |
| U6 | L | 焦点 + 路由顺序（最容易返工） | 否 |
| U7 | M | 搬家 + 转发壳 + 占位符校验 | ✅ 全程可并行（纯 CPU） |
| U8 | L | HUD + 菜单 + 投影 + 对象池 | 依赖 U1~U6 |

**建议顺序**：U0 → U2（先把矩形画出来，验证整条链路）→ U1 → U3 → U4 → U5 →
U7（可与 U3~U6 穿插）→ U6 → U8。

⚠ **为什么不先做 U1**：字体生成器是"离线工具"，做完要等 U2 才能看到效果；
先做 U2 能用 `ui.rect` 立刻验证挂点/sRGB/顺序这三个**最容易翻车**的点，
失败的成本最低（一张纯色图，出问题一眼就能定位）。
U1 做完时 U2 已就绪，可以直接看到"血"字上屏。

---

## 12 · 一页速查

- **插入点**：`PostProcess::renderScene` 的输出 pass 内、ImGui overlay **之前**。
  模式 A（交换链）与模式 B（离屏图）各一个 hook。**`renderUiToSwapchain` 里不加。**
- **descriptor**：独立 set（1 个），**不动全局 8 个**。`maxBoundDescriptorSets` 是
  "同时绑定数"，不是"总数"。
- **颜色**：交换链是 sRGB → shader 里 `srgbToLinear`，硬件会编码回去。
- **字体**：构建期离线生成，**OFL 字体随仓库提交**，不用系统字体。
- **布局**：逻辑像素 + 参考分辨率缩放，除的是**输出分辨率**（不是 DRS 分辨率）。
- **输入**：`route()` 在玩家控制之前；UI tick 在状态栈之外。
- **i18n**：单一权威 `engine/i18n/ZhTable.cpp`；`editor::tr` 变转发，792 处零改动。
- **默认关**：`ui.enabled=0` 与 `ui.hudInEditor=0` 时必须逐位不变。

---

## 13 · 执行进度（2026-10-18 第一轮）

**已完成并全绿：U0 / U1 / U2 / U3 / U4 / U5**

| 阶段 | 交付物 | 验收锚点 |
|---|---|---|
| U0 | `engine/ui2/Ui2Settings.*`、`assets/ui` 进 `VENN_ALWAYS_ASSET_DIRS`、5 个 `ui.*` 登记 | `UI2-CAPS maxBoundDescriptorSets=32` · `UI2-TOTAL pass=39 fail=0` |
| U2 | `UiCanvas` / `UiRenderer` / `ui2.{vert,frag}` / `PostProcess` 的 `GameUiDrawFn` | `ui.rect` 中心像素 **128** · 零 VUID · TAA 无关 |
| U1 | `tools/gen_sdf_atlas.py` → `assets/ui/fonts/noto_sc_sdf.{png,json}`（786 字形 / 2048² / 0.39 MB R8，OFL） | `UI2-FONT glyphs=786 … top=1` · **词典 681 个非 ASCII 字缺 0 个** |
| U3 | `UiCanvas::text` / `measure` + `SdfFont`（UTF-8 解码、缺字回退 '?'） | `verts=42`（6+36）· `text_advance_uses_adv` · `sdf_smooth_shrinks` |
| U4 | `engine/ui2/UiLayout.*`（9 锚点 + `placeRect`） | `layout_margin_ratio=0.6667` · 超宽屏右锚贴 2560 |
| U5 | `engine/ui2/UiWidget.*`（按钮三态 + 命中 + `ui.click` 排队） | `UI2-CLICK hit=1/0` · 差分 bbox 宽高比 3.75 |

**两条路线图判断被实测推翻（已改代码注释）：**
1. §"一份 pipeline 两模式通用"——**错**。两个输出 pass 的 `subpass dependency` 不同，而
   dependency 参与兼容性判定 → 模式 B 每帧 2028 条 `VUID-vkCmdDraw-renderPass-02684`。
   已改为 `UiRenderer::pipelineFor(VkRenderPass)` 按 pass 懒建 + 缓存。
2. §"`VP-RECT` 足够定位 UI 元素"——不够。模式 B 下离屏图被 ImGui **等比缩放**贴进视口，
   屏幕坐标 = 设计像素 × `min(outW/1920,outH/1080)` × (视口宽/outW)，中间那段很容易漏。
   验收改用**差分包围盒**，不猜坐标。

**剩余：U7（i18n 搬 `engine/i18n/ZhTable.cpp`，`editor::tr` 变转发壳）→ U6（焦点栈 + `consumed`，
UI tick 在状态栈**之外**）→ U8（血条/准星/伤害数字/暂停菜单 + `config/ui.json`）。**
⚠ 按回归纪律：U7 动了 792 处调用的底层，**开工前需明确指令是否跑全量**。

---

## 14 · 执行进度（2026-10-18 第二轮：U7 → U6 → U8）

**U7 / U6 / U8 全部完成并全绿。`docs/ui_hud_roadmap.md` 的 U 系列到此收口（U9 可选，未做）。**

| 阶段 | 交付物 | 验收锚点 / 实测 |
|---|---|---|
| U7 | 词典 1559 条搬到 `engine/i18n/ZhTable.cpp`；`engine/i18n/I18n.{h,cpp}` 是唯一查表入口；`editor/Localization.cpp` 从 2104 行降到 **101 行**转发壳，`TR()` **792 处零改动** | `I18N-KEY lang=zh count=1559` · 不设语言 → `lang=en count=0`（**表不构造**，英文零开销） |
| U6 | `engine/ui2/UiFocus.{h,cpp}`（几何导航 `along + 2×lateral`、方向无候选**回绕**）、`UiCanvas::setGameUiBuilder/Active`、`tickUi` 挂在 `cb.pollInput`（早于 fixedUpdate、不受 `blocksUpdate`/`timeScale` 影响） | `UI2-FOCUS id=…`（**只在变化时**打）· `UI2-PAUSE open=1 context=Menu cursor=1` · `UI2-TOTAL pass=56 fail=0` |
| U8 | `runtime/ui/{HudModel,Hud,PauseMenu,UiConfig,HudSelfTest}`、脚本内置 `setHealth/damage/popup`、`HUD-STATS` 锚点 | `HUD-TOTAL pass=22 fail=0` · `HUD-STATS … bar=15,638,221x11 fill=…` · `damage(30)` → `health=70.0 fill=168.0`（= 240×0.7） |

**新增回归用例 `tools/verify_hud.py`（已进 `DEFAULT` 与 `TRIGGERS`）**：A 无窗口自检 / B 上屏几何
+ 准星否定测试 / C 暂停菜单 + 焦点环 + **暂停时按 W 位置逐位不变** / D 夹具脚本扣血。

### 本轮实测踩到的四个坑（都已写进代码注释）

1. **⚠⚠ Vulkan 的 NDC y 轴向下**（+1 在屏幕**底部**），与 OpenGL 相反。`ui2.vert` 原来写的是
   `1 - px.y/H*2`，于是整层 UI **上下镜像**（血条跑到顶部、`100 / 100` 倒着写）。
   正确写法是 `px.y/H*2 - 1`，因为 3D 那边 `proj[1][1] *= -1` 已经把 GL 约定翻成 Vulkan，
   UI 这里**只剩零次**。这个 bug 只有截图能看出来，日志全是对的。
2. **UI 输出尺寸必须与 PostProcess 同源**：命中框要用 `Renderer::uiOutputExtent()`
   （= `post->outputExtent()`），拿窗口尺寸或 `renderExtent()` 算会整体偏一小段比例 ——
   屏幕上看光标就在按钮上却点不中。
3. **焦点导航自测必须用"数组顺序故意打乱"的布局**。垂直单列里"几何最近"和"数组下一个"
   恰好一致，是最容易假绿的地方；`testFocus()` 因此用左右两列 + 乱序数组。
   ⚠ 反过来说，`verify_hud.py` 的环状断言**可以**按数组顺序，因为那边确实是单列 ——
   这个"可以"写在脚本注释里了，不要照抄到别处。
4. **`PostMessage` 的方向键必须带 `KF_EXTENDED` 位（lParam bit24）**。不带的话 GLFW 的 win32
   后端把扫描码 `0x50` 翻成**小键盘 2**，界面一动不动，而日志里**没有任何痕迹**。

### 三条纪律（值得抄进任何 HUD 类功能的实现）

- **暂停的权威是状态栈顶名**（`topName()=="Paused"`），不是 bool 成员；而"暂停时按 W 不动"
  靠的是 `pushContext("Menu")` 让 Gameplay 整组**结构性**失效 —— 不是靠 if。
  验收因此同时断言两件事：位置**逐位不变** 且 `UI2-FOCUS` **变了**（证明 W 到了菜单而不是被吞掉）。
- **状态迁移只在 `onUpdate` 末尾做**：`tickUi` / `Paused::onUpdate` 只置意图位
  （`m_pauseRequested` / `m_svc.popRequested`）。当场 pop 会让遍历中的 vector 失效。
- **像素断言必须带对照区**。两次启动的窗口只要偏 1~2 像素，中心区差异就冲到 100% ——
  断言照样"通过"，但它证明的只是"两张图不一样"。`verify_hud` 的 B2 因此要求
  **对照区 < 2% 且中心区 ≥ 10× 对照**（实测 2.219% vs 0.000%）。

### 图集必须重新生成（U7 的连带工作）

词典从 776 → 1559 条后，`engine/i18n/ZhTable.cpp` 有 **956 个非 ASCII 字符**，
而旧图集（786 字形 / 2048²）缺了 **279 个**汉字 —— 这些字在界面上会显示成方块，
且"跑起来了、有字了"的画面上完全看不出来。已重生成：
`tools/gen_sdf_atlas.py --atlas 2048 --atlas-h 3072` → **1059 字形 / 2048×3072 / 0.54 MB**，
字典缺失 **0**。
⚠ 顺带给生成器加了 `--atlas-h`：2048² 只差 2.4% 装不下，4096² 只用 28%，
多出来的空白既进 PNG（提交产物）也进显存。图集尺寸由 JSON 的 `atlas.w/h` 驱动，
运行时**不假设 2 的幂**。
⚠ `verify_ui2.py` 与 `gen_sdf_atlas.py` 里的 `DICT_SRC` 都已指向新路径 —— 搬家时
只改一处会让"词典全覆盖"这条硬断言**静默失去意义**（旧路径只剩 101 行转发壳，
数字从 956 掉到 211，红得"变小"，最难发现）。

---

## 15 · 执行进度（2026-10-27：#312 UI 编辑器最小版）

**目标：让"界面上有什么"从 C++ 里搬出来，变成一份能拖着改的资产。**

在此之前，血条坐标写死在 `runtime/ui/Hud.cpp`、暂停菜单写死在 `PauseMenu.cpp` ——
改一个按钮的位置要重编译。这一轮之后，一份 `.ui.json` 就能描述整层界面，
编辑器里拖控件 / 点九宫格换锚点 / 存盘，运行时 `Game.exe --ui-doc <path>` 直接读。

| 交付物 | 内容 |
|---|---|
| `engine/ui2/UiDocument.{h,cpp}`（约 1100 行） | 文档格式 + `parseUiDocument` / `toJson` / `saveUiDocument`（临时文件 + rename）+ `placeDocument` / `drawDocument` + `UiBindings` |
| `engine/ui2/UiLayout.cpp` 新增 `placeRectIn(parent, …)` | 子控件锚点相对**父矩形**；顶层相对屏幕 —— 两处共用同一套"左/中/右、上/中/下"符号约定 |
| `editor/UiEditorWindow.{h,cpp}`（约 1500 行） | 三栏：控件树（230）/ 设计画布（16:9）/ Inspector（330）+ 底部校验条 |
| `runtime/RuntimeConfig` + `RuntimeApp` | `uiDoc` 配置项（json / env `MYVK_RUNTIME_UI_DOC` / `--ui-doc`）+ 每帧重建 `UiBindings` 后 `drawDocument` |
| `assets/ui/docs/hud_demo.ui.json` | 7 个控件（含 3 个嵌套子控件），带中文 `"//"` 注释，兼作 B 组的"往返不丢东西"夹具 |
| `tools/verify_ui_editor.py`（约 680 行）+ 回归登记 | A~H 八组 **46 条**，已进 `DEFAULT` / `TRIGGERS` / `check_regression_map` |

**验收实测**

| 门 | 结果 |
|---|---|
| `verify_ui_editor` | **46/46 PASS** · `结果: PASS` |
| `MYVK_UI2_TEST=1` 数值自检 | **`UI2-TOTAL pass=86 fail=0`**（53 → 86，新增 30 条 `uidoc_*` + 3 条清单项） |
| `verify_ui2` / `verify_hud` / `verify_dialogue` | PASS（图集重生成后逐个复跑） |
| `verify_i18n_coverage` | **73 项**（A1=5 / B=1 / B2=66 / E=1），与基线一致 |
| `check_regression_map.py` | PASS |

### 六条约束（写在 `UiEditorWindow.h` 头注释里，代码要服从它）

1. **校验只有一份**：存盘前把文档 `toJson` 再 `parseUiDocument` 一遍，能解析回来才算通过。
2. **读盘失败 → 只读**：坏文件不许被残片覆盖（D3 断言原文件 SHA 不变）。
3. **换锚点保持屏幕位置**：`actSetAnchorKeepRect` 用 `placeRectIn` 的逆运算反解 x/y。
4. **拖动只改 x/y**，不做尺寸手柄（最小版），尺寸走 Inspector。
5. **画布缩放与 `ui.scale` 无关**：设计画布走 `placeDocumentAt(canvas, scale=1)`。
6. **不做运行时预览**：编辑器和 `Game.exe` 各读同一份文件，不内置"边编边跑"。

### 这一轮踩到的六个坑（都已写进代码注释）

1. **"保持屏幕位置"不等于"rect 不变"**。`resume` 从 `bottom-right` 换到 `top-right`：
   文档里 `rect` 的 x/y 是**距锚点那一侧**的距离，所以 y 必须从 24 变成 992，
   而**屏幕矩形**仍是 `1656,992,240,64`。验收脚本第一版拿文档 rect 去对屏幕 rect，
   把正确实现判成 FAIL —— 判据要分开写（C2 查文档、C3 查屏幕）。
2. **`drawn=1` 是写死的**。`UIDOC-STATS` 里那个 `drawn` 原来直接抄了源码里的字面量，
   于是这份日志只能证明"进了绘制函数"，证明不了"画了 7 个"。改成 `drawDocument`
   真返回的 `UiDocDrawStats`，并加 `skipped` / `texts`。
3. **锚点日志的节流点要包含第 1 帧**。验收夹具是 `frame == 60` 扣血，而节流是
   "每 60 帧一行"—— 拿到的样本全在扣血之后，`fill` 一条直线，明明绑对了也证明不了。
   改成 `m_uiDocFrames == 1 || … % 60 == 0` 后，fill 序列从"213.3 213.3 213.3"
   变成 **"213.3 → 149.3"**（= 70%），绑定是否通一眼可见。
4. **绑到不存在的键时 Progress 要按 0 算**。`w.value` 默认 1.0，缺失时会画出**满槽**——
   "看起来血是满的"是最容易骗过验收的假象。`bind` / `bindMax` 缺失一律 0（空槽）。
5. **`reason=(\S+)` 会被 NOTIF-PUSH 的回显行截走**。日志里 `UIED-SAVE-BLOCKED … reason=read-only`
   紧接着还有一条 `text="UIED-SAVE-BLOCKED … reason=read-only"`，`\S+` 一路吃掉结尾的引号，
   断言拿到的值是 `read-only"`，永远不等。改 `[\w-]+`。
6. **`UI2-TEST` 的 actual/expected 允许有空格**。`HP 70/100`、`70 70.5` 会被 `(\S+)` 判成
   "行格式解析失败"，红字全指向解析器而不是产品。改 `(.+?)` 靠后面的字面量做边界。

### 回归纪律上的一处连带修复

图集（2048×3072 / 1138 字形）早就装不下词典了：**HEAD 上就已缺 56 个汉字**，
这一轮新增的 33 条 UI 编辑器词条又多了 8 个（`§九剧品宫旗校议`）。这类缺失
"跑起来、有字、界面上看不出来"，只有图集覆盖率断言会红。已重生成：

    python tools/gen_sdf_atlas.py --atlas-h 4096     # 1200 字形 / 2048×4096 / 0.61 MB

⚠ 2048×3072 需要 6.7 M 像素，差 6% 装不下；4096² 只用 66.8%。`--atlas-h` 是 U7 那一轮
加的，这次直接复用 —— 字号 48 / spread 6 / ascent 48 三个数**一个都没动**，
所以字形位图逐位相同，只有打包位置变了：`verify_hud`（白字墨迹 ≥ 1%）与
`verify_dialogue`（字幕底边对齐）复跑都 PASS，证明文本像素没有漂移。
