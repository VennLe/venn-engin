# venn —— 项目长期笔记（规则索引）

> 改动前必读的**规则**。实现细节 / 全量环境变量 / set0 描述符表 / 构建路径表 / 全量坑点表 →
> `render_details_archive.md`；叙事 → 各日 `YYYY-MM-DD.md`。
> 引擎：手写 Vulkan（学习向 C++17），`D:\A-VENN\code\cpp\venn-engin`（CMake 工程名 `Venn`，刻意不一致）。
> 世界坐标 **右手系 Z-up**。唯一可执行文件 `Editor.exe`，启动 `resetToEmptyScene`。
> 分层 `core→rhi→render→scene→assets`（另有 `ui`/`ecs`/`physics`；应用层只有 `editor`）；依赖单向，
> **engine/ 不认识 editor/**。**改目录名后必须删 `build/` 重新 configure**。

## 构建
- 入口 **`make build`**（`Makefile` + `make.cmd`）；`TOOLCHAIN=auto` → **本机走 MSVC**（MinGW 整条缺失）。
- **MSVC 四坑**：① `https_proxy` 与 `HTTPS_PROXY` 并存 → MSBuild 假报 `MSB6001: CL.exe 开关无效`；
  ② UTF-8 无 BOM + 中文注释 → `C2001`，对策 `/utf-8`；③ `.cmd`/`.bat` **必须纯 ASCII**；
  ④ Make 可能用 sh 跑配方 → `echo` 里不能有 `(` `)` `|`，`$(wildcard)` 探含空格路径必须转义。
- ⚠ **资产拷贝必须挂 `add_custom_target(... ALL)`，禁用 `add_custom_command(TARGET ... POST_BUILD)`**
  —— 后者只在**重新链接**时跑 → 改 shader/贴图**不生效**，会连续多轮测到旧资产。
  输出用 `${CMAKE_RUNTIME_OUTPUT_DIRECTORY}`（用 `$<TARGET_FILE_DIR:Editor>` 会循环依赖报错）；
  `assets/scenes|scripts` **保持 POST_BUILD**（编辑器运行时也会写）。其余构建约定见归档。

## 环境变量（全表见归档）
- 断言必备 **`MYVK_EDITOR_NO_HUD=1`**（关 FPS/帧时间等叠加层）+ **`MYVK_GRID=0`**。
- 编辑器 `MYVK_EDITOR_SCENE` → `EDITOR_CAM`（**顺序不能反**）；`MYVK_FRAMES` / `LOG_RECTS`。
- **`MYVK_SSS=f`**：皮肤曲率缩放（默认 8）；`=0` ⇒ 退化成 Lambert 的对照组；**`≥24` 已饱和**。

## 渲染管线（改动前必读）
Pass 序：`Shadow(4 级级联) → LocalShadow(12 层数组) → DepthPrePass → SSAO(compute) →
ClusterBuild(compute) → VolumetricFog(compute×2) → Forward(天空盒→不透明→栅格→透明) →
PostProcess(Bloom+Tonemap) → 交换链`。
- **compute 不能在 RP 内执行** → 深度预通道独立成 RP 且**无条件执行**（只把"画几何"包开关）。
- **栅格必须夹在不透明与透明之间**（吃深度测试 + 不写深度），且**绝不能**做成 ImGui 叠加层。
- **阴影**（细则见归档，最易忘三条）：矩阵必须 **Zero-to-One**（`glm::orthoRH_ZO`）；CSM 每级范围 =
  切片包围球、球心在**光轴基**量化到纹素网格（**不能量 `view*mid`**）；局部光 near =
  `clamp(range/20, 0.1, 2)`、深度偏移以**米**给再按参考深度换回 NDC。`ShadowPass` 在自己 RP 内绑 set0
  （**别搬进前向 RP**）。
- **分簇**：簇编址 C++ / `cluster_build.comp` / `pbr.frag` **三处逐字一致**；`maxLightsPerCluster`
  唯一来源是 C++（`ubo.lightParams.w`），**禁硬编码**；固定槽位 → 结果可复现。
- **FrameGraph**：新效果登记依赖、**关时不得录 pass**；读 MSAA 深度要另编 `*.msaa.*` 并进 CMake 列表；
  `recreateScreenSpace()` 早于 `postInputViews()`。
- ⚠ **`FrameUBO` = 720B**（末尾 `sssParams`）。改它必须同步**所有**声明它的着色器
  （`pbr.vert/frag`、`sky.vert`、`cluster_build.comp`、`shadow.vert`、`fog_inject.comp`）；新字段**只能加末尾**。
- ⚠ 推常量 **128B 全满**（`skin.w` = alpha cutoff，符号位 = 抖动开关）。**旁路管线的 range 必须与主管线
  逐字段相同**（Vulkan 把 range 与阶段算进"布局兼容"）→ 否则 VUID-08600，整个 Pass 的 set0 一起挂。
- **set0 = 20 binding**（0 FrameUBO … 18 lightTex / **19 sssLut**，全表见归档）。新增要同步
  `kGlobalSetBindings` + `writeGlobalSet` + 着色器 layout **三处**。
- 全 SRGB，**着色器内不做手动 gamma**；`addFace(n,u,v,half)` 中 **u×v 必须 = n**。

## 变体与关键字
`ShaderKeywords.h` 唯一事实来源；**位序三处逐字对齐**：`kKeywords[]` / `CMakeLists` 的
`SHADER_KEYWORDS` + `VARIANT_DEFINES_ALL` / `pbr.frag` 的 `#if`。
bit0 `DOUBLE_SIDED` / bit1 `ALPHA_BLEND` / bit2 `NORMAL_MAP` / bit3 `ALPHA_MASK` /
**bit4 `SHADING_SKIN`** → **32 变体**（**变体 ID = 掩码本身**）。
- 变体表由 CMake 循环生成（**不手写**）。⚠ 空串会被 `list(APPEND)` **丢弃 → 位序整体错位**：
  空项存哨兵 `"_"`、`list(JOIN DEFS " " DEFS_STR)` 拍平、末尾 `list(TRANSFORM ... REPLACE "^_$" "")`；
  每套必须 `separate_arguments(DEFS NATIVE_COMMAND ...)` 再喂 glslc。
- 硬约定：绑定缓存哨兵**不能是 0**（0 是合法掩码）→ `kBindsNothing = 0xFFFFFFFFu`，栅格后重置；
  `SHADER-VARIANTS` 日志必须打在前向 Pass **之后**；变体**共用同一 `VkPipelineLayout`**。
- `#extension` 必须在所有非预处理语句之前。全局开关（bindless / instancing / MDI）走独立 .spv。

## 材质（Alpha 模式 + 着色模型）
- `AlphaMode`：Opaque / **Blend**（不写深度、透明队列降序、**跳深度预通道**）/
  **Mask**（`alpha < cutoff` → `discard`、写深度、走**不透明队列**、**跳深度预通道**）。
  **队列分类唯一判据是关键字位** `hasKeyword(kw, kKeywordAlphaBlend)`。
- **cutoff 载体 = `push.skin.w`**；**符号位 = 抖动开关**（负 → Bayer 4×4）。唯一出口
  `Renderer::alphaCutoffLane(mat)`（非 Mask 恒推 +0.5）。MSAA 下开 a2c + 预映射
  `clamp((alpha-cutoff)*2+0.5)`；⚠ **a2c 只允许 `samples != 1`**（1x 下开会让创建管线报错）。
- **`ShadingModel{Default,Skin}`**：Skin 把漫反射乘子从 `NdotL` 换成预积分 LUT 查表
  `texture(sssLut, (N·L*0.5+0.5, curv))`；**方向光 / `shadeLocalLight` / `shadeLocalLightMod` 三处都要分支**；
  曲率 `clamp(length(fwidth(几何法线)) * sssParams.x, 0, 1)` —— **必须用扰动之前**的 N；
  采样用 `textureLod(...,0)`（隐式 LOD 会糊掉响应）。
- ⚠ **查表坐标必须钉到纹素中心** `(x*(N-1)+0.5)/N`：采样器是 **REPEAT**，而端点 `u=1` / `v=0`
  （平面 `fwidth(几何法线)=0` → 曲率恒 0）**正好落在纹理边界** → 双线性掺进"绕到另一侧"的纹素
  → **平坦皮肤无端泛红**（R4-2 实测 0.20 → 5.61）。**这条适用于所有查表类贴图。**
- 序列化：`alphaMode`（字符串，**保留旧 `alphaBlend` 布尔兼容**）+ `alphaCutoff` + `ditherAlpha` +
  `shadingModel`。**色彩空间是契约**：albedo/emissive `srgb=true`；**normal/ORM/数据表必须 false**。

## 断言纪律（血的教训）
- ⚠ **采样区必须落在被测物体上**（俯角 8° 时视口**上半是天空盒**，"视口正中"只采到天空 → 误判"没生效"）。
  先拿哨兵确认可见性；用**面板区**或"有物体 − 无物体"现算掩膜再腐蚀几像素。
- ⚠ **必须 `MYVK_EDITOR_NO_HUD=1`**（`drawOverlay` 会画 FPS/帧时间）。
- 色调映射压平高光 → **别用固定倍数阈值**，用"逐级落差 > ε"或单调性。**"全有/全无"的开关测不出"分级"**
  → 取**敏感区间内**的中间刻度。
- **亚像素效果不能靠截图验证**；**"输入驱动"的行为别只靠像素差分**。
- **新写的断言必须做"否定测试"**（故意把实现退化回 bug 版本 → 断言必须变红）。**临时诊断代码用完必须清**。
- **"同一份产物、失败项每次不同" = flaky**，先别怀疑功能，检查断言的**度量口径**：
  ① 是不是拿"整幅图统计量"比较**样本数不同**的两个画面（用差异带/面板区）；
  ② 同一行里两个阈值是否自洽（`n` 统计 `>18` 却说 `mx > 8` 就算显著 → mx=17 时自相矛盾）；
  ③ 是否依赖**注入时序**（点击/焦点）。三类本项目各踩过一次。
- **脚本结尾必须打 `结果: PASS` / `结果: FAIL: ...`**（ASCII 冒号）；**验证层静默是一等验收项**。

## 工具
- `tools/run_regression.py`：读末尾 `结果: PASS`；**增量**；**新增 `verify_*.py` 必须加进 `DEFAULT`**。
- 共用设施 `capture_window.py` / `verify_add_menu.py`（`LogTap` / `kill_stale_editors` / `wait_rect`）。
- 截图目录 `<repo>/../_venn_verify`；⚠ **禁止硬编码绝对盘符**。
- Python：`C:\Users\wumen\.workbuddy\binaries\python\envs\default\Scripts\python.exe`（3.13，numpy+pillow）。
- 离线烘焙：`make_bc_dds.py`（BC 编码器，可复用）、`make_sss_lut.py`（R4-2 LUT）。日志标签表见归档。

## 高频坑点（全表 58 条见归档 `#已知坑点全表`）
1. **同一文件的多处修改必须串行 Edit**（并行第二个会静默回滚第一个）；不同文件可并行。
2. `Window.h` 里 **vulkan.h 必须在 glfw3.h 之前**；`std::filesystem::path` 不能直接赋 `std::string`。
3. **改某条管线的 push constant range = 可能打断整个 Pass 的 set0 绑定**（VUID-08600）。
4. 深度图当纹理采样用 `DEPTH_STENCIL_READ_ONLY_OPTIMAL`；**全局描述符引用的图重建后必须
   `writeGlobalDescriptors`**；blit 生成 mip 的初始 barrier 必须全层 UNDEFINED→TRANSFER_DST。
5. **视口 HUD 必须是 viewport 的 child window**（ImGui 每帧把聚焦窗口提到最前）；popup 必须每帧提交。
6. **自动化**：跑前关 `imgui.ini` 与窗口几何存档；`SetForegroundWindow` 会静默失败；
   **"注入的第一次左键会被 Windows 拿去做窗口激活"**；进程 stdout 是块缓冲。
7. **改 GLSL/贴图后先 `cmp` 确认 `build/bin/` 里的副本真的变了**。**没排除构建陈旧之前，别去怀疑算法。**
8. **REPEAT 采样器的 0/1 边界**是陷阱（见 `材质`）。

## 路线图
✅ R0-1 / R0-2 / R1-1~R1-4 / R2-1~R2-3 / R3-1~R3-3 / R4-1 `AlphaMode::Mask` / **R4-2 预积分 SSS（皮肤）**。
**下一项 = R4-3 POM 视差遮挡贴图**。其后：R4-4 DBuffer 贴花 / R5-1 反射探针 /
R6-1 GPU 粒子（⭐ 最大缺失）/ R6-2 水体 / R6-3 植被风 / R6-4 地形 / R6-5 动态天空 / R7 架构演进。
