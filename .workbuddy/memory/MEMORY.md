# venn —— 项目长期笔记

> 2026-10-09 起项目更名为 **venn**（原 MyVulkanEngine），**仓库目录 = `E:\code\cpp\venn-engin`**
> （CMake 工程名是 `Venn`，两者不一致是刻意的）。工程里**只有一个可执行文件
> `Editor.exe`**：内置 demo 与独立的 Sandbox 示例程序已按用户要求**全部删除**，
> venn 启动不再加载任何样例（`resetToEmptyScene`）。
>
> **改目录名后必须删 `build/` 重新 configure** —— CMakeCache.txt / VerifyGlobs.cmake 里
> 全是绝对路径，直接 `cmake --build` 会报 "CMakeCache.txt directory ... is different"。

## 项目定位与分层（新增代码必须放对层）
从零手写的 Vulkan 引擎（学习向，C++17）。
`engine/core`(Application 主循环/Window/Input/Logger/Time/WindowGeometry) → `engine/rhi`(Vulkan 封装) →
`engine/render`(Renderer 组织一帧/HdrTarget/ShadowPass/PostProcess/FrameResources) →
`engine/scene`(Scene/Camera/Light + SceneSerializer) → `engine/assets`(Mesh/Texture/Material/加载器) ；
还有 `engine/ui`(ImGuiManager)、`engine/ecs`(World/Entity/Components)。
应用层只剩 `editor`(EditorApp + 面板 + 自研脚本语言 + 编辑器态场景)。原来并行的
`game`(SandboxGame) 已删除。依赖方向 `editor → render/ui → scene/assets → rhi → core`；
**engine/ 不认识 editor/**，唯一接缝是 `Application::activeScene()` /
`Renderer::setViewportSize` / `viewportTextureId`；一次性收尾走 `Application::onShutdown()`。
**CMake 目标名**：静态库 `venn`（`project(Venn VERSION 0.2.0)`）+ 可执行 `Editor`。

## 构建与运行（Windows + MinGW）
```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=C:/msys64/ucrt64/bin/gcc.exe \
  -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe \
  "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"   # 必须（CMake4 拒绝 GLFW 的 3.4）
cmake --build build -j 8
```
Vulkan SDK 头文件在 `E:/code/vulkan/core/Include`。只有一个目标 `Editor`；资源拷贝
（shaders/models/scenes/scripts/fonts/icons）由 CMake 函数 `myvk_copy_runtime_assets(target)` 统一负责。

**Makefile**（`Makefile` + `make.cmd`，驱动 `mingw32-make`）：`build / run / clean / rebuild /
distclean / package / smoke / info / help / install-make-alias`。
**`make run` = 打开编辑器**（唯一入口）。**没有 `sandbox` / `play` 目标** ——
Sandbox 已删，游戏直接在编辑器视口里跑。
- 配方须**同时兼容 cmd.exe 与 sh** → 只用 `cmake -E`、正斜杠、`cmake -E env`
- `echo` 与 `.cmd/.bat` **整文件**必须纯 ASCII（cmd 用 GBK，否则乱码被当命令 → 死循环）
- 配置戳 `build/.configured-$(BUILD_TYPE)`；`:=` 立即展开变量必须定义在依赖者之前
- `make run` 用 `cmake -E chdir build/bin`（ImGui 把 imgui.ini 写在 CWD）；`package` 用 `cmake -E tar`

**环境变量**：`MYVK_FRAMES=N`(跑 N 帧退出；**同时抑制窗口几何存档**) /
`MYVK_EDITOR_PLAY=1`(编辑器启动即 Play) / `MYVK_NO_WINDOW_SAVE=1`(禁止写窗口几何存档) /
`MYVK_VALIDATION=0` / `MYVK_SHADER_DIR` / `MYVK_ASSET_DIR` /
`MYVK_SCENE_TEST=1` / `MYVK_FREEZE_ANIM=1` / `MYVK_HIDE_UI=1`(关全部 UI，像素对比必备) /
`MYVK_GRID=0`(关视口地平面栅格；默认开) /
`MYVK_CONTENT_DIR=assets/models`(Content 面板初次打开进哪个目录，默认 assets) /
`MYVK_LOG_RECTS=1`(打印关键控件矩形，给自动化脚本定位用；见"工具") /
**编辑器自动化钩子（2026-10-09 新增，只在设了变量时生效）**：
`MYVK_EDITOR_SCENE=<路径>`(启动即打开该场景而非空场景) /
`MYVK_EDITOR_SELECT=<名字>`(启动后按名字选中实体，让手柄/检查器有内容) /
`MYVK_EDITOR_GIZMO=move|rotate|scale`(启动即切手柄模式) /
`MYVK_MSAA / LIGHTS / CLUSTER_CULL=0 / DEPTH_PREPASS=0 / LIGHT_GIZMOS=0 / TILE_SIZE /
CLUSTER_SLICES / MAX_LIGHTS_PER_CLUSTER / CLUSTER_STATS=1`。

## 渲染管线约定（改动前必读）
Pass 顺序：`ShadowPass → DepthPrePass(独立 RP) → ClusterBuild(compute) → Forward(MSAA resolve，
深度 LOAD) → PostProcess(Bloom+Tonemap) → 交换链`。**compute 不能在 RP 内部执行** → 深度预通道必须
独立成 RP；该 RP **无条件执行**（只把"画几何"包进开关），否则前向 RP 的 `LOAD` 深度未定义、画面废。
- **Forward 内部子顺序**：`不透明 → 地平面栅格 → 透明`。栅格夹在中间是**刻意的**：吃深度测试
  （墙/柱子/箱子能正确挡住它）+ 不写深度（玻璃能叠在它上面）+ 排在透明之前。
  它**绝不能**做成 ImGui 叠加层 —— ImGui 永远压在 3D 之上，栅格会穿透一切。
- 描述符 **set 0**：0 FrameUBO / 1 shadowMap / 2 lights / 3 clusters / 4 lightIndices / 5 clusterStats
  （灯数据全挤进 set 0：Vulkan 只保证 4 个 set 同时绑定）。set 1/2/3 = albedo/normal/ORM。
- compute 管线复用 `m_globalSetLayout` → dispatch 必须绑 `frame.globalUBOSet`。
- 推常量 **112B**：`mat4 model + vec4 baseColor + vec4 pbr(metal/rough/normalScale/ao) + vec4 emissive`
  （`render::PushConstants`；栅格另有 48B 的 `GridPushConstants`）；顶点 `pos3+normal3+uv2` stride 32。
- ⚠️ **旁路管线必须让 push constant range 与主管线逐字段相同**：set 0 是整个 Pass 一次性绑定的，
  而 Vulkan 判"管线布局兼容"时**把 push constant range 的范围与阶段也算进去**。若新管线只声明
  自己的小 range（栅格本来想声 48B），立刻报 VUID-...-08600「set 0 被不兼容的
  vkCmdBindDescriptorSets 打断」，**后续所有 draw（含透明）全报错**。→ 栅格管线**故意**声明与
  主前向管线一样的 112B（实际只推 48B，合法）。别想着"换个 layout 重绑一次 set 0"——那会
  打断 set 0 的既有绑定，反而更糟。
- 交换链/纹理均 SRGB，**着色器内不做手动 gamma**；视口/剪刀动态，resize 不必重建管线。
- 绕序：`addFace(n,u,v,half)` 中 **u×v 必须等于 n**，正面 `COUNTER_CLOCKWISE`（配 `projMatrix` Y 翻转）；
  `offsetAlongNormal`：立方体面 true，单个面（地面）必须 **false**。

## 分簇光照（Clustered Forward）
- **簇编址三处必须一致**（C++ / `cluster_build.comp` / `pbr.frag`）：
  `idx=(slice*gridY+tileY)*gridX+tileX`，切片 `d(k)=clusterNear·(zf/clusterNear)^(k/slices)`。
- 切片起点用 `clusterNear` 而非 `zNear`（glm::perspective 未定 ZERO_TO_ONE，NDC z∈[-1,1]）。
- **固定槽位** `lightIndices[ci*cap+i]`（非原子追加）→ 结果**可复现**，截图对比才有意义。
- `maxLightsPerCluster` **唯一数据源是 C++**（经 `ubo.lightParams.w` 传给片元），**严禁在
  pbr.frag 硬编码**。只花显存不花时间 → 默认 **128**。
- 射灯只按 range 球剔除（锥角在片元算）；衰减用窗口化平方反比 `win²/d²`，range 处严格归零。
- 透明（Blend）不写深度、按相机距离**降序**排序、**不参与深度预通道**。
- 遥测 `stats` SSBO：0 单簇最大 / 1 溢出簇数 / 2 配对数 / 3 空簇数（`beginFrame` 回读并清零）。

## editor/ 编辑器层约定
三条铁律：**① 编辑器模式不跑游戏逻辑 ② 渲染目标是 ImGui 子窗口而非全屏 ③ 编辑态与运行态分离**。
- **运行态 = 同进程的内存副本**（`m_runtimeScene`，最核心）：`EditorContext::activeScene()` 是唯一
  分派点 —— `usingRuntime() ? m_runtimeScene : m_editorScene`。点 Play 把编辑态
  `sceneToJson` → `sceneFromJson(m_runtimeScene)`（**内存拷贝，不落盘、不起进程**），
  于是**视口自己就变成了游戏画面**（复用同一条离屏渲染链，不需要第二个窗口）。
  - **不做跨进程**：曾用 `Editor.exe --play <快照>` 另起进程弹独立游戏窗口（`GameProcess`/`PlayerApp`），
    已**整体删除**。原因：用户要的是"就在原来视口里显示"；同进程零渲染器改动即可达成。
  - 运行态相机：`play()` 里对**副本**调 `setUseGlobalInput(false)` + `setFlyMode(true)`；
    **此后编辑器不再驱动它**（视口在 Play 期间不处理任何键鼠导航），怎么动由游戏逻辑决定。
    编辑态相机不受影响（详见"视口相机与导航"一节）。
  - `pause()` 只翻状态；`stop()` 丢弃 runtime scene、复位 `m_gameFullscreen`、`validateSelection()`。
  - 脚本只在 Play 态跑，且只作用于 **runtime scene**；编辑态只 `preloadScripts()`（编译不执行）。
- **Play 期间编辑器只读**（gizmo/Inspector/层级增删被 `!isEditing()` 挡住 —— 那边跑的是副本，
  改了也同步不回去；想做"边跑边编辑"要另开一轮设计）。
- **命令模式**：值编辑 `RawBytesEditCommand`，结构编辑 `SceneSnapshotCommand`（整场景 JSON）；
  统一入口 `structuralEdit(name,fn)`（捕 before → 执行 → 捕 after）。破坏性操作**两段式**
  （ECS 遍历期间不能增删 → 攒 pending 队列，树画完再 `flushPending()`）。
- **视口解耦**：`setViewportSize` 让 3D 输出写离屏图，`viewportTextureId()` 给 `ImGui::Image`。
  纹理稳定性：draw data 在 drawFrame 前已构建 → ① recreate 保留场景输出图 ② `createViewportTextures`
  比较未变则不重注册 ③ ViewportPanel 尺寸刚变那帧不画图（`m_resizePending`）。
- **布局**：SplitLayout 二叉分栏树（比例 0..1 → 自适应）；主体面板必须 **Always 落位 +
  NoMove/NoResize**；分隔条用前景 DrawList 画线 + 手动命中。标题带状态须加稳定 ID（`###viewport`）。
- **自研脚本 .vks**（`editor/script/ScriptEngine.{h,cpp}`）：`ScriptHost{scene,entity}` 是唯一接缝
  （安全性是结构性的）；热重载 mtime 轮询 0.5s，**编译失败保留上一版**；数值用 double，
  `rotate` 单位是**度**，步数上限防死循环。
- **全局观感走 `ImGuiManager::applyVennStyle()`**（`init()` 里 `StyleColorsDark()` 之后调）：
  尺寸 `FramePadding(9,5)/ItemSpacing(9,7)/WindowPadding(10,8)/圆角 4px`；配色 accent
  `(0.16,0.53,0.94)`、`WindowBg(0.106,0.110,0.118)`、`MenuBarBg == TitleBg(0.113,0.117,0.125)`
  —— 工具条刻意用同一个颜色，和菜单栏**连成一条顶栏**（UE 观感的关键）。
- **面板宽度按窗口宽度自适应**：`kLeftFrac 0.175 / min 300 / max 520`、`kRightFrac 0.205 / min 330 / max 560`；
  用户拖过分隔条后 `m_userAdjusted=true` 不再自动重算，仅"首帧或宽度变化 >1.25×/<0.8×"时重算。
  （原因：最大化 3840 时固定 320px 侧栏只占 8%，挤成一坨。）
- **快捷键**：`F2` 重命名（Hierarchy / Content 各自处理，Content 只在鼠标悬停本面板时生效）、
  `Ctrl+D` 复制、`Ctrl+N` 新建场景、`Delete` 删除、`Backspace` 上一级。
- **Content 面板的右键菜单是"显式单 popup"**：`DrawEntry` 只写 `m_ctxRequest/m_ctxEntry`，
  `drawGrid()` 循环外 `OpenPopup(kEntryCtxPopup)` + `drawEntryContextMenu()`；`BeginPopup`
  返回 false 就自动把 `m_ctxOpen` 清掉（状态自愈，不需要额外关闭逻辑）。见坑点 #30。

### Hierarchy 的"文件夹"节点（2026-10-09 新增）
- **显式标记**：`HierarchyComponent::group`（**不要**靠"这个节点没有网格组件"去猜 —— 空节点会被误判）。
  序列化字段 `je["folder"]`；右键 `Mark as Folder` / `Unmark as Folder`；新建走 `uniqueName()`
  保证 `Folder` / `Folder 2` / … 唯一；`duplicateEntity` 要连 `group` 一起复制。
- **树行的图标不能塞进 `TreeNodeEx` 的 label**（label 是纯文本，字体里没有文件夹字形）→
  label 传空串，图标 + 名字全部自己用 `DrawList` 画：
  `行左边缘 = ImGui::GetCursorScreenPos().x`（**必须在 `TreeNodeEx` 之前取**）
  `文本起点 = 行左边缘 + ImGui::GetTreeNodeToLabelSpacing()`
  （后者实现 = `FontSize + FramePadding.x * 2`，**恰好等于** imgui_widgets.cpp 里的 `text_offset_x`）。
  文件夹名字用暖色 `IM_COL32(238,202,132,255)`。图标在 `editor/EditorIcons.h`
  （`drawFolderIcon` 52px 给 Content / `drawTreeFolderIcon` 小号给 Hierarchy）。

### 模型导入 = "根节点 + 子网格"，落点贴地（2026-10-09 新增）
- `EditorScene::instantiateModel()` 返回 `ImportResult{root, entities}`：**一定要建一个空根节点**
  当把手 —— glTF 节点的 `transform` 是**世界变换**，直接分解成 TRS 塞进子节点之后，
  它们彼此没有共同父级，没法"整体平移到落点"。
- `EditorScene::alignImportToGround(scene, picking, imported, target)`：联合所有子网格的世界 AABB →
  `shift = (target.x - c.x, target.y - box.min.y, target.z - c.z)`。
  **拿不到 CPU 侧包围盒时只平移、不贴地**（宁可不贴也不要塞到地面以下）。
  会打一行 `alignImportToGround: box.min.y=… bottom=… (target.y=…) root=(…)` 供自动化断言。
- 两个入口共用这套逻辑：**拖进视口**（落点 = 鼠标射线 ∩ y=0 平面）与 **双击 Content 里的模型**
  （落点固定为世界原点）。
- Content 侧**只有模型能拖**（`.gltf/.glb/.obj`）：`BeginDragDropSource` 必须带
  `ImGuiDragDropFlags_SourceAllowNullID` —— 缩略图条目是无 ID 的 item，不带会 `IM_ASSERT(0)`
  直接崩（退程码 `0xc0000409`）。视口侧拖拽中画绿色高亮边框 + 落点说明文字。

## 视口相机与导航（先分清"谁的相机"）
**两者的存储模型相同（`scene::Camera` 的自由飞行），但归属完全不同 —— 这是最容易做错的地方：**
- **编辑态 = 编辑器视口的导航相机**，由编辑器**完全驱动**：视口悬停时按 UE 编辑器的习惯操作。
- **Play 态 = 游戏自己的相机**，编辑器**不碰**：视口在 Play 期间**不响应任何键鼠导航**
  （用户明确要求"运行前的视口要好用，运行后归游戏管"），只负责渲染 + 全屏切换。
  ⚠️ 上一版把导航做进了 Play 态，方向是错的。

### 编辑态导航（完全按 UE 编辑器）
| 操作 | 行为 |
|---|---|
| 按住**右键**拖动 | 自由旋转（水平 + 俯仰） |
| 按住**中键**拖动 | 平移 |
| **单独滚轮** | 沿视线**推 / 拉相机**（`kNavDollyPerNotch=0.6f` 米/格 × sens） |
| **右键 + 滚轮** | 调导航灵敏度（±`kSensitivityStep=0.1f`/格） |
| **右键 + WASD** | 前后左右飞行（**只在按住右键时**） |
| **右键 + Q / E** | 下降 / 上升；**Shift** 加速 ×3 |
| **左键** | 点选物体 / 拖 gizmo 手柄 —— **不参与导航** |

- **滚轮必须有两条路，靠"是否按住右键"分流**（用户明确要求）：单独滚轮 = 推拉相机，
  右键 + 滚轮 = 灵敏度。判定复用 `lookDown`，和 WASD 门控同一个开关，所以不会有第三种语义。
  HUD 上的滚轮入口（鼠标停在浮层里）**同样要求按住右键**，保持一致。
- **"飞行只在按住右键时生效"是必须的**：`W/E/R` 同时是手柄模式快捷键，不门控就会
  "按 W 往前飞的同时把手柄切成平移模式"。UE 就是这么分的；`EditorApp::handleShortcuts`
  里用 `const bool flying = IsMouseDown(Right)` 门控那三个快捷键。
- 常量在 `ViewportPanel.cpp`：`kNavLookPerPixel=0.0025f` / `kNavPanPerPixel=0.01f` /
  `kNavMoveSpeed=5.0f` / `kNavDollyPerNotch=0.6f` / `kSensitivityStep=0.1f`。
- `EditorContext::applyEditorCameraMode()` = `setUseGlobalInput(false)` + `setFlyMode(true)`。
  **每次场景重建都必须重设**：新建/打开/重建 demo → `onSceneReplaced()`（内含调用），
  undo/redo（整场景快照恢复）→ 各自入口里显式调用。**飞行模式不在序列化范围内**，
  漏掉就表现为"导航突然变回老式轨道相机"。

### ⚠️ Camera 飞行模式的不变式（改任何 setter 前必读）
`position()` 与 `m_target` 通过 `target = position - angleDir()*distance` **互为逆运算**。
所以**任何动 target / distance / yaw / pitch 的接口都必须同步另一侧**，
否则飞行模式下表现为"拖了没反应 / 画面静止 / 绕空点打转"：

- `syncFlyTarget()`（位置→target，`moveLocal`/`look` 用）
- `setTarget` / `setDistance` / `setYaw` / `setPitch`（target→位置方向，反推 `m_flyPos`）
- `orbit` / `zoomBy` / `panWorld` / `dolly`（轨道风格入口，飞行模式下自动改写成等价动作）
- `Camera::update()`（全局输入路径）也要按 `m_fly` 分支，否则与上面的入口脱节

新增任何改相机姿态的接口，都要问一句"飞行模式下这条还成立吗"。

### 灵敏度与浮层
- **导航灵敏度**（`EditorContext::m_navSensitivity`）：默认 **1**，clamp `0.05..20`，
  移动与旋转**共用**。视口右上角 HUD 可 `InputFloat` 直接填（≤0 忽略）、
  **按住右键 + 滚轮**可调（上滑变大 / 下滑变小）。**只作用于编辑态**。
  （单独滚轮是推拉相机，不碰这个值 —— 见上面的导航表。）
- **Play 态浮层**只有"全屏 / 恢复"按钮 + 播放状态，**刻意不含导航控件**。
- **浮层必须是 viewport 的 child window**（`SetCursorScreenPos` + `BeginChild`，在
  `ImGui::End()` **之前**画）：ImGui 每帧把聚焦窗口提到最前，独立顶层窗口会被 3D 图盖住。
- **浮层尺寸必须按字体度量算**（`CalcTextSize` / `GetFrameHeightWithSpacing` /
  `GetTextLineHeightWithSpacing` + `GetStyle().WindowPadding`）。写死像素的话，
  用户在 Settings 里放大字号后文字就被裁掉半截。也**不要**在视口工具栏里再塞全屏按钮 ——
  那条工具栏本来就很长，会被挤出右边缘（实测被裁成 "Fulls…"）。

## Gizmo 拖拽与吸附（`editor/PickingSystem` + `editor/GizmoController`）
**两个曾经的致命缺陷（用户报"拖拽方向反 + 速度越来越快"），已修，改这里前必读：**
- **① 轴线/射线最近点的参数符号**（`PickingSystem::closestOnAxis`，Ericson 5.1.9）：
  `s = (b*e - c*d) / (a*c - b*b)`。曾把分母写成 `b*b - a*c`（差一个负号）→ 参数整体取反
  → **往右拖物体往左跑**。现在分母保留 `b*b - a*c` 只用于判平行，参数用等价的
  `(c*d - b*e) / (b*b - a*c)`。**改符号前先想清楚分子分母是不是同时翻了。**
- **② 拖拽参考线必须"冻结"**（正反馈环）：`applyDrag` 若每帧都用物体**当前**位置当轴线原点，
  则物体自己走过的位移会被重新算进参数 → `D_new = Δ + D_old` → **越拖越快**（指数发散）。
  正确做法：`beginDrag` 里快照 `m_dragOrigin / m_dragAxis / m_dragWorldLen`，`applyDrag`
  全程只用这份快照（`m_origin / m_axisDir / m_worldLen` 每帧都在变，不能碰）。
- **三套独立的吸附**（`EditorContext`）：`snapMove/snapMoveStep`、`snapRotate/snapRotateStep`、
  `snapScale/snapScaleStep`，各带 `...Ref()` + `snapForCurrentMode()/snapStepForCurrentMode()/
  toggleSnapForCurrentMode()`。**量化语义各不相同，别统一**：
  | 模式 | 量化对象 | 默认步长（对齐 UE5） |
  |---|---|---|
  | 平移 | **位移增量**（米） | 0.1 m（UE5 的 10 uu → 本引擎 1u=1m） |
  | 旋转 | **角度增量**（度） | 10° |
  | 缩放 | **倍率 factor**（不是缩放绝对值！） | 0.1 |
  缩放量化倍率的原因：量化绝对值会让第一下拖拽从 0.37 直接跳到 0.4，像抽搐了一下。
- **UI**：视口工具栏**第二行** `drawSnapRow()`（`snapField()` 勾选框 + DragFloat + Presets 弹窗 +
  `current: … snap on/off`）；**G 键** = 切换当前工具的吸附（`!flying` 门控内）。
  拖拽时右上角浮层显示实时读数 `Move X: +0.600 m  [snap]`（`GizmoController::readout()`，
  只在 `dragging()` 期间有效，`endDrag`/非拖拽帧会被清空）。
- 旋转**始终在"拖拽起始姿态"上叠加**（`R = D*R0` 或 `R0*D`），不做增量累乘。

### 手柄的三种粒度（2026-10-09 重写：从"只有单轴"扩到 7 种 `Handle`）
`enum class Handle : int { None=-1, AxisX/Y/Z=0/1/2, PlaneYZ=3, PlaneXZ=4, PlaneXY=5, Screen=6 }`。
- **平面枚举名 = 该平面的法线**（`PlaneYZ` 的法线是 X）。颜色按**法线**取 → 法线 X 的方片是红的，
  和 UE / Blender 的"红方块 = 法线朝 X 的面板"一致，不用记额外规则。
- 平移/缩放：单轴（沿线/单轴拉伸）+ 平面双轴（平面内自由移动 / 两轴一起缩放，取两腿平均）
  + 中心 `Screen`（**视平面**内自由移动 / **等比**缩放）。旋转：三轴环 + 外圈 arcball 自由旋转。
- 平面方片画在两根轴夹角处（`kPlaneT0 0.30 ~ kPlaneT1 0.62` 相对手柄长度），命中用
  `pointInQuad()`（四角叉积同号），**不是**逐边求交。命中顺序：旋转先判三轴环再判外圈
  （正交视图下"视轴环"会和某根轴环重合，应优先给具体轴）；平移/缩放的顺序是
  中心 → 平面 → 单轴。
- **arcball**：屏幕偏移映射到单位虚拟球（球外投到赤道），`angle = atan2(|p0×p1|, p0·p1)`，
  轴从相机系（`right/up/-fwd`）转到世界系；自由旋转**固定按世界空间**施加。
- **平面拖拽必须兜底**：射线几乎与平面平行时交点跑无穷远、位移瞬间爆炸 →
  `kMaxDragUnits = 2000`，超了就丢弃该帧。
- **本引擎欧拉序是 `R = Rx·Ry·Rz`**，而 `glm::eulerAngles` 用的是 **YXZ**，语义不同**不能直接用**
  → 自己写 `eulerFromRotation()`。GLM 列主序下 `R_ij = m[j][i]`：
  `y = asin(m[2][0])`、`x = atan2(-m[2][1], m[2][2])`、`z = atan2(-m[1][0], m[0][0])`，
  `|sy| > 0.99999` 走万向锁分支（x/z 只有一个自由度，全归到 x）。
- API：`hotHandle()/handleName()`（**旧的 `hotAxis()/axisName()` 已删**）。

## 地平面参考栅格（`assets/shaders/grid.{vert,frag}` + `Renderer::GridSettings`）
- **解析式、零顶点缓冲**：VS 用 `gl_VertexIndex` 拼 4 顶点铺在 y = `yOffset` 的大四边形；
  格线在 FS 里按 `fract(p/step + 0.5) - 0.5` 求"到最近线的距离"，除 `fwidth(p)` 转成**像素距离**
  → 1px 抗锯齿线宽，**不依赖 MSAA**，远处靠 `smoothstep` 淡出避免摩尔纹。
- **三种图层**：最小格（1 m，暗）× `minorStrength` → 主格（10 m，亮，线宽 ×1.4）→
  世界轴（X 轴红 / Z 轴蓝，与视口 gizmo 同配色）。默认 `extent 60 / minor 1 / major 10`。
- **与地板共面必须压 z-fighting**：VS 里 `clip.z -= 1e-6 * clip.w`（等价于全距离恒定的 NDC 偏移）。
  地板顶面正好在 y = 0（`Floor` pos.y=-0.25 / scale.y=0.5）。
- 管线：`TRIANGLE_STRIP` / 无顶点布局 / `depthTest=true` / `depthWrite=false` / `blendEnable=true` /
  `cullMode=NONE`。推常量 48B（`params/color/fade`）。
- **开关**：`Renderer::gridEnabled()`（`bool&`），由 `EditorApp::onUpdate` 每帧写
  `= isEditing() && ctx.showGrid()` —— **Play 期间强制关**（视口是游戏画面）。
  UI 三处等价入口：视口右上角 HUD 勾选框 `Ground grid (1 m)`（**默认开**）、View 菜单、`MYVK_GRID=0`。
- 数值调参在 `drawEnginePanel()` 的 `Viewport grid (y = 0)` 段（开关**刻意不放**在那里）。
- ⚠️ 栅格是**世界空间**的，与相机无关；相机在水面/地板上方才能看到它，且会被墙正确遮挡。

## 全屏与窗口几何

### 全屏（两种，别再混）
- **Play 态全屏 = 窗口真全屏 + 视口独占整屏**：`EditorContext::m_gameFullscreen` 只存**逻辑**
  状态（本类不认识 Window），真正的窗口切换在 `EditorApp::syncWindowFullscreen()`
  → `Window::setFullscreen(true)` → `glfwSetWindowMonitor(win, primaryMonitor, 0,0,
  mode->width, mode->height, mode->refreshRate)`。退出时回到**进入全屏前记住的窗口几何**。
  快捷键 **F 切换** / **Esc 退出**（都只在 Play 态生效）；按钮两处：主工具栏（`isEditing()` 时禁用）
  + 视口右上角播放浮层（全屏时工具栏不画，浮层那个是唯一图形化退出口）。
- **F11 = 只切窗口全屏**（面板照旧），任何模式都可用；View 菜单也有
  （`EditorContext::windowFullscreenRequest()` 请求标志 → EditorApp 消费，和 `quitRequested` 同套路）。
- **切换必须做边沿检测**：`EditorApp::m_appliedWindowFullscreen` 记住"上次告诉 GLFW 的值"，
  只在变化时动窗口。若每帧无条件 `setFullscreen(want)`，编辑态下 `want=false` 会把 F11
  打开的纯窗口全屏立刻关掉；F11 在"只看游戏"模式下也会看起来像按键失灵。
- `Window::setFullscreen` 里除 `glfwSetWindowMonitor` 外**必须再置 `m_resized = true`**：
  framebuffer 回调通常会置位，但漏掉时会拿着旧尺寸的交换链渲染一整帧（画面被拉伸）。

### 窗口几何：首次居中 70%，之后记住上次关闭时的样子
- `engine/core/WindowGeometry.{h,cpp}`（新）：`applyStartupGeometry(window, fraction, 存档文件, 兜底W,H)`
  + `saveWindowGeometry(window, 存档文件)`。
  - 无存档 → 主显示器**工作区**（`glfwGetMonitorWorkarea`，已排除任务栏）居中、宽高 = 工作区 × fraction。
  - 有存档 → 用存档（含 `maximized`）；**必须先做可见性校验**（`visibleOnSomeMonitor`），
    换显示器 / 拔外接屏后存档可能落在屏幕外 —— 那样窗口开了也看不到、拖不回来。
- `EditorApp`：存档名 `editor_window.ini`（和 `imgui.ini` 一样放 CWD）、`fraction = 0.70f`；
  `onInit()` **第一件事**就落位，`onShutdown()` 保存。
  - `core::Application` 为此新增 `virtual void onShutdown()`（主循环退出前、窗口还活着时调用）。
  - 落位走的是"置 resize 标志 → Renderer 下一帧重建交换链"，和用户手动拖窗口同一条路。
  - **自动化跑不写存档**：`MYVK_FRAMES` 或 `MYVK_NO_WINDOW_SAVE` 存在时跳过
    （`automationRun()`）；`tools/capture_window.py` 默认给子进程注入 `MYVK_NO_WINDOW_SAVE=1`
    —— 它会故意缩放窗口压测，否则会把用户的存档改成压测尺寸。
- `Window` 新增的几何 API：`windowPos/windowSize/setWindowPos/setWindowSize/maximized/maximize/
  primaryMonitorWorkArea/windowedGeometry`。注意**几何是屏幕坐标**（glfwGetWindowSize），
  与 `framebufferSize`（像素，高 DPI 下不同）不要混用。`windowedGeometry()` 在全屏时返回
  "进入全屏前"那份 —— 否则下次启动会拿全屏尺寸当窗口尺寸。

## venn 命名与图标（三层落地）
引擎/编辑器统一叫 **venn**：窗口标题 `Venn Editor`（**Sandbox 已删，没有第二个窗口了**），
日志头 `=== Venn Engine - Editor ===`。图标形象是**滑稽可爱豚鼠**（矢量卡通风：
奶油色身体 + 焦糖色斑块、大眼、龅牙、抿嘴笑、炸毛），源图在 `design/venn_logo_source.png`。

三层各自独立、缺一不可：
1. **exe 资源**：`assets/icons/venn.rc`（`1 ICON "venn.ico"`）+ CMake `enable_language(RC OPTIONAL)`
   → 资源管理器图标、任务栏早期默认图标。`venn.ico` 含 16/24/32/48/64/128/256 七档。
2. **运行时**：`Window::setIconFromFile()` 用 stbi + `glfwSetWindowIcon`
   （Win32 下即 WM_SETICON ICON_SMALL/ICON_BIG → 标题栏左上角 + Alt+Tab + 任务栏）。
   `Application::run()` 在 `init()` **之前**调，路径走 `assets::resolveAssetPath("icons/venn_icon.png")`。
3. **PNG 落盘**：`assets/icons/venn_icon.png`（512×512 透明），供运行时读取 + 单张够用
   （系统自行降采样，不必为每个尺寸单独出图）。

抠图要点：抹水印 → 抠中性浅灰背景（`sat<=10 && dist<=16`）→ 去地面阴影 →
**洪泛填洞还原封闭内部**（否则眼白/牙齿会被一起打掉）→ 腐蚀 1px 去浅色镶边。

## 资产溯源与序列化
Mesh/Material/Texture 各带"来源描述"（`*Source`），序列化只写描述、加载时由 AssetManager 重建
（JSON 可读可手改可进版本库）；glTF 派生资源用"模型键 + primitive 下标"；路径入 JSON 前过
`assets::makeAssetRelative()`；层级父子用**数组下标**；相机与方向光也在同一个 JSON 里。

## 已知坑点（血泪清单）
1. PowerShell 里 `-DXXX=3.5` 会被拆开 → **必须加引号**。
2. `Window.h` 里 **vulkan.h 必须在 glfw3.h 之前**（`glfwCreateWindowSurface` 被 `#if VK_VERSION_1_0` 包着）。
3. 每张交换链图像需**独立**的 render-finished 信号量；析构时 Swapchain 必须先于 Surface 销毁。
4. imgui master(≥1.93) InitInfo 变了（RenderPass 移到 `PipelineInfoMain.RenderPass`）；无 docking 分支
   → `ImGuiWindowFlags_NoDocking` **未定义**。
5. tinyobjloader 字段 `config.vertex_color`；tinygltf master 已重构 → 固定 v2.9.4。
6. **深度图当纹理采样**必须用 `DEPTH_STENCIL_READ_ONLY_OPTIMAL`（非 SHADER_READ_ONLY）。
7. **glTF ORM 陷阱**：`metallicRoughness` R 通道常写 0，本引擎 ORM 约定 R=AO → 必须 CPU 侧重打包。
8. 全屏三角形用 `gl_VertexIndex`（Vulkan GLSL），不是 `gl_VertexID`。
9. `World::clear()` 归零 generation → 旧句柄可能"碰巧" alive；`Scene::clear()` 必须重置 `m_lightEntity`。
10. **本环境 PowerShell 输出不回显、bash coreutils 不可靠** → 结果写文件再 Read/Grep；日志用
    `[System.IO.File]::WriteAllText(p,s,UTF8Encoding($false))`；长命令用后台任务（其输出可被捕获）。
11. `enum class` 底层类型必须是整型；嵌套类型当类型名用要写全限定名（`Scene::LightStats`）。
12. **头文件前向声明的类型，定义不能放进匿名命名空间**（否则是另外两个类型，`StmtPtr/ExprPtr` 对不上）。
13. `std::filesystem::path` **不能**直接赋给 `std::string` → 用 `.generic_string()`。
14. 引号 include 先查**当前文件所在目录** → `editor/script/*.cpp` 里写 `#include "ScriptEngine.h"`。
15. **同一文件的多处修改必须串行下发**（并行 Edit 第二个会静默回滚第一个）。不同文件可并行。
16. **"编译通过 + 日志无错" ≠ 画面正确**，一定要看截图。
17. 相机视线要避开柱子（柱子在 (±4,±4)，正对角视线必穿 (4,4)）；默认 editor 相机 distance=12/pitch=0.28
    会从盒外看房间外墙 → 一片暗灰。
18. `ImGui::BeginDragDropSource()` 默认要求上一个 item 有非零 ID → 无 ID 的 item 必须传
    `ImGuiDragDropFlags_SourceAllowNullID`，否则 `IM_ASSERT(0)` → 退出码 `0xc0000409`。
19. **ImGui 每帧把聚焦窗口提到最前** → 想让视口上的 HUD/覆盖层可见，**必须是 viewport 的 child window**
    （`SetCursorScreenPos` 定位 + 在 `ImGui::End()` 之前画）；独立的顶层窗口会被 3D 图盖住。
    （旧坑：CreateProcess 的 `bInheritHandles` / 环境块继承 —— 跨进程方案已删除，见 editor/ 一节。）
20. MinGW 的 `os_defines.h` **已定义 `NOMINMAX`** → 自己再 `#define` 会告警重定义。
30. **ImGui 的 popup 必须"每帧都被提交"，否则帧末会被直接关掉**：菜单一弹出来就正好
    盖在光标下，触发它的那个 item 立刻不再 hovered —— 若把 `BeginPopup*` 塞进
    `if (hovered)` 里，下一帧不再执行、popup 没提交，菜单**只闪一帧**就消失。
    正解：`drawEntry` 里只置"请求"（记下目标快照），`BeginPopup` 放到循环**之外**每帧
    无条件调用；且 `OpenPopup` / `BeginPopup` 必须在**同一 ID 栈深度**（同一窗口、无 PushID），
    否则 `GetID()` 哈希不同，永远打不开。
31. **自动化输入：扩展键（Delete/方向键/Insert/Home/End）用 `keybd_event` 必须同时给扫描码**。
    GLFW 从 `lParam` 的 `(KF_EXTENDED|0xff)` 取 scancode 再查 `keycodes[]`，Delete 的表项是
    `keycodes[0x153]`。只传 `KEYEVENTF_EXTENDEDKEY` 而 `bScan=0` → 系统回退 `MapVirtualKey(VK)=0x53`，
    扩展位丢了 → 查表得 0 → **键被整个丢弃**（现象：F2 有效、Delete 死活没反应）。
    正解：`keybd_event(vk, MapVirtualKeyW(vk,0), extended?0x0001:0, 0)`。
32. **这台机器上鼠标会被外部挪动**：`SetCursorPos` 之后 0.35s 内光标会平滑漂走
    （实测采到 `(255,750)→(313,817)` 这种曲线），而空闲时又完全静止。
    引擎里没有任何 warp 光标的代码。所以自动化点击不能"设一次就点"，
    必须**闭环**（连续两次读回都在 ±2 内）+ 点击后校验画面再重试。
33. **调试日志要限流**：把 `logRect(tag,...)` 放进递归的 `drawNode` 里，同一 tag 每帧被调
    N 次、值一直变 → 两分钟写了 30MB，而且脚本取"最后一行"拿到的是随机某一行（坐标全错）。
    必须"每帧只记一次"。
34. **ImGui 颜色经 SRGB 交换链后会明显变亮**：`ImVec4(0.62,0.24,0.22)` 在屏幕上约是
    `(206,140,133)` 的**浅鲑红**。按 `*255` 去写像素判据（找红色按钮）会一个像素都匹配不到。
35. **自动化测试用的"红色确认按钮"定位**：用"新旧两张图里**新增**的红色区域"，能天然排除
    3D 场景里本来就有的红盒子。
36. **改 exe 图标要在 CMake 里 `enable_language(RC OPTIONAL)`**：MSYS2 ucrt64 自带
    `windres`；`if(CMAKE_RC_COMPILER)` 判可用后再把 `.rc` 加进 `target_sources`。
    新增后**必须重新 configure**（CMakeLists 变了会自动触发）。
37. **`SetForegroundWindow` 会静默失败，而窗口不在前台时 ImGui 的拖拽完全收不到输入**。
    Windows 的硬规则：只有**当前前台进程**（或被它启动的进程）才允许改前台窗口。
    症状极具迷惑性 —— 坐标对所有、`SetCursorPos` 返回 1、`mouse_event` 也不报错，
    但拖拽中间态截图里**连跟随提示都没有**，`BeginDragDropSource` 一次都没点火。
    绕法：先按下再松开一次 ALT（`keybd_event(VK_MENU,…)`），系统就认为"用户在操作"，
    随后的 `SetForegroundWindow` 才会被放行 —— 见 `tools/verify_asset_drag.py::force_foreground`。
38. **解析自家日志的正则别用行首锚定**。日志行有 `[INFO ] ` 前缀，写 `^TAG=` 的
    `re.match` 会一条都匹配不到，且**不报错**（脚本静默地"什么都找不到"）。
    用 `re.search`，标签用 `[^=]+` 而不是 `.+`（`.+` 贪婪会把 `=(x,y)-(x,y)` 吞错位）。
39. **改项目根目录名会被"以它为 cwd 的进程"挡住**。`mv` 报
    "另一个进程正在使用此文件" / `System.IO.IOException`，而**子目录改名完全正常**
    ⇒ 锁在根目录本身（VS Code 的扩展宿主以工作区根为 cwd 是最常见的一种）。
    先让用户关掉那个窗口再改；改完必须删 `build/` 重新 configure（CMake/Ninja 缓存里
    全是绝对路径），并同步所有硬编码绝对路径（`.workbuddy/skills/*`、文档）。
21. **ImGui 固定像素尺寸的浮层会被字号缩放裁掉** → 宽度用 `CalcTextSize(...)` 取所有文本的最大值、
    高度用 `GetFrameHeightWithSpacing()/GetTextLineHeightWithSpacing()/WindowPadding` 累加。
    （`fontScale` 是用户可调的，实测写死 236px 时提示行被裁成半截。）
22. **`Camera` 飞行模式的 setter 不变式**：`m_target` 与 `m_flyPos` 互为逆运算，任何只改一边的
    接口都会让另一种模式"拖了没反应"。新增接口务必按 `m_fly` 分支（详见"视口相机与导航"）。
23. **视口工具栏已经很长**：往里加按钮会被挤出右边缘裁掉（实测 `Fullscreen (F)` 变成 `Fulls…`）。
    再加控件前先想"能不能挪到浮层 / 主工具栏"。
24. **验证"输入驱动"的行为别只靠像素差分**：编辑器场景是大片平滑渐变，轻微视角变化就会让
    60%+ 像素变化 → 差分恒等于"全变了"，没有区分度。可靠读数是**界面上的数字**：
    Inspector 的 `Target/Yaw/Pitch` 与 HUD 的 `Sensitivity`（截图后直接读数比对最实在）。
    注入真实输入：`SetCursorPos` 让窗口收到**真**的 WM_MOUSEMOVE（只 PostMessage 会被真实鼠标
    消息覆盖 → 视口不再 hovered），再 `PostMessage(WM_MOUSEWHEEL / WM_*BUTTONDOWN)`。
    ⚠️ **WM_MOUSEWHEEL 的 wParam 高字是"这一次的 delta"（WHEEL_DELTA=120），不是累计格数**；
    传累计值会让效果成倍（第一次就踩了：3 格变成 9 格）。
25. **窗口几何存档 vs 自动化**：`tools/capture_window.py` 会故意缩放窗口压测 —— 让它给子进程
    注入 `MYVK_NO_WINDOW_SAVE=1`，否则用户的"上次窗口大小"会被压测尺寸覆写。
    另外该工具用 `terminate()` 强杀进程，`onShutdown()` 不会执行（这也是为什么它是安全的）。
26. **"方向反 + 越拖越快"是两个独立缺陷叠加**（符号取反 × 自激正反馈），只修一个会得到
    "方向对了但仍在加速"。数值可复现的 bug **先用小脚本把公式跑一遍**再动 C++，
    比反复截图快得多。**验证用的环境变量钩子用完必须删干净**（`grep -rn "TEMP-"` 收尾）。
27. **改某条管线的 push constant range = 有可能打断整个 Pass 的 set 0 绑定**
    （VUID-...-08600，报错会出现在**后续无关的 draw 上**，极易误判）。想给旁路管线"缩 range"
    之前先读"渲染管线约定"最后一条。
28. `ContentBrowser` 面板会**扫描进程 CWD 的所有图片**做缩略图 → 在项目根目录随手丢验证截图
    会被当成资源"加载"（日志刷 `Texture loaded:`）。验证产物请用固定前缀并及时清理。
29. **"验证层静默"要当成一等验收项**：`grep -c "VUID\|ERROR"` 必须为 0。校验信息在
    `capture_window.py` 的 stdout 里，很容易被"截图看着没问题"盖过去。

## 工具
- `tools/capture_window.py`：启动 exe → 缩放压测 → `PrintWindow` 截图（不受遮挡；进程需 DPI 感知）。
  参数 `--no-resize / --size WxH / --out / --wait / --env K=V / --exe / --title / --args`。
  **默认目标已经是 `build/bin/Editor.exe` / 标题 "Venn Editor"**（Sandbox 已删）。
  窗口查找用 `EnumWindows` + 标题**前缀**匹配。
  **会给子进程注入 `MYVK_NO_WINDOW_SAVE=1`**（压测会缩放窗口，别污染窗口几何存档）。
  抓 Play 态画面：`--env MYVK_EDITOR_PLAY=1`；**想抓全屏布局**得改 `EditorApp::onInit()` 里
  `m_ctx->gameFullscreenRef() = true;` 临时置位（无命令行开关），抓完记得撤。Play 态 HUD 在视口右上角。
  运行：`& "C:\Users\v_lweiili\.workbuddy\binaries\python\envs\default\Scripts\python.exe" tools\capture_window.py`
  **注意**：改了面板默认落位要先删 `build/bin/imgui.ini`（`FirstUseEver` 只在无记录时生效）；
  想回到"首次启动的 70% 居中"要删 `build/bin/editor_window.ini`。
- `tools/verify_asset_drag.py`：**真鼠标**端到端拖拽验证（需求 3 就是靠它验的）。
  带 `MYVK_LOG_RECTS=1` 启动 → 读 `CB-CELL <文件>` / `VP-RECT` 矩形 → 摆成固定尺寸 → 强制前台 →
  `SetCursorPos` + `mouse_event` 拖拽 → 截图 + 断言 `alignImportToGround` 的 `bottom == 0`。
  `--asset / --out / --debug-shot / --size / --env`。**改任何拖拽交互都先跑它。**
- `tools/compare_png.py a.png b.png`：逐像素对比。回归时配 `MYVK_FREEZE_ANIM=1 MYVK_HIDE_UI=1`，
  同配置连跑两次应 0 差异。对照实验：`maxLightsPerCluster` 必须 ≥ 灯数（否则参考图自己被截断 = 假差异）。
- **`editor/DebugRects.h` + `MYVK_LOG_RECTS=1`**：把控件真实矩形打一行日志
  （`CB-RECT up=(10,480)-(38,508) center=(24,494)`）。**ImGui 坐标 == 客户区像素，和 PrintWindow
  截图 1:1**，所以自动化脚本可以直接拿这些坐标点控件，不用在截图里"猜"按钮位置（猜坐标踩过坑：
  DPI 缩放 / 窗口被 ini 恢复成最大化 / 用户拖过分隔条，写死的坐标会整体偏掉而且**不报错**，只是点不中）。
  矩形变化超过 4px 才重打一行；同一个 tag 每帧只允许记一次（见坑点 #33）。
  现有 tag：`CB-CELL <文件名>`、`CB-RECT up|reload|cell0`、`VP-RECT`、`HIER-RECT row0`。
  **加了新控件就给它加一个 tag** —— 这是让界面可被自动化验证的唯一途径。
- 启动方式：双击 exe 即可（着色器按 exe 绝对路径找）。查找顺序：`MYVK_SHADER_DIR` → `exe目录/shaders`
  → `cwd/shaders` → `cwd/assets/shaders`。
- **验证产物放项目外**：`/e/code/cpp/_venn_verify/`（截图 / 日志 / 场景 json）。
  删掉的内置 demo / Sandbox 备份在 `/e/code/cpp/_venn_removed_demo/`。
  ⚠️ ContentBrowser 会扫进程 CWD 的图片当资源（见坑点 #28），别往项目里丢截图。

## VS Code（IntelliSense，与 CMake 编译解耦）
手写 `includePath` 必须与 CMakeLists 的 `target_include_directories` 同步；SDK 的 `Include` 根目录
（**不要**加 `Include/glm`，会与 third_party/glm 冲突）；报 "无法打开 vulkan.h" 先怀疑 `.vscode` 配置缺失。
