# Venn 引擎渲染优化任务清单

> 生成于 2026-10-13，基于"对照成熟引擎（UE5 / Godot 4 量级）的差距评估"。  
> 现状基线：MSAA+HDR(R16G16B16A16)、Bloom/FXAA/ACES、CSM 4级@2048、点/聚光局部阴影、  
> 分簇光照、IBL(irradiance+GGX prefilter)、静态天空立方体、解析式高度雾、SSAO、  
> FrameGraph(SSGI/SSR/GodRays/DoF/MotionBlur)、色差/暗角/后处理体积、shader 变体+热重载、  
> GPU instancing、视锥剔除/BVH/LOD、蒙皮(SSBO 关节矩阵)、纹理四件套(mipmap/各向异性/BC/流式)、  
> 多相机/通用 RenderTarget、AlphaMode{Opaque,Blend}、RenderStats。
>
> **配套文档**：`docs/editor_roadmap.md`（E 系列，人的入口）+ `docs/ship_roadmap.md`
> （**G 系列**，游戏运行时与出货，2026-10-17 新建）。三者分工 **R = 画得对 / E = 用得顺 / G = 出得去**。  
> ⚠ **R 系列里有 5 项（R5-2 / R5-3 / R6-2 ~ R6-5）是 G 系列的硬前置** ——
> 它们的"运行时消费端"在 `ship_roadmap.md` 的 G21~G25，**R 侧落地时 G 侧的消费路径才第一次真正被用到**。
> 换言之：这几项**不是"可选的美化"**，而是"开放世界能不能做出来"的分水岭。

## 使用约定

- **全局约定（继承既有惯例）**：
  - 所有新效果**默认关**，且"关闭 = 基线逐像素不变"（每项都要写对照组验证）。
  - 完整回归 `tools/run_regression.py` **跑一次**即可；新用例加进 `DEFAULT` 列表，结果行必须含 `结果: PASS`。
  - 新 FrameGraph pass 走 `kPasses` 注册表模式（`FrameGraph.h` + `static_assert` + watchShaderFiles 加 `.spv`/`.msaa.frag.spv`）。
  - 每完成一项：验证脚本 → 单跑 PASS → 完整回归 → `.workbuddy/memory` 落盘。
- **规模标注**：S = 半天～1 天；M = 2~4 天；L = 一周以上。
- **依赖标注**：`←` 表示开工前应先完成的项目（硬依赖加 `!`）。

---

## 阶段 0 · 工具先行（为后面一切调优铺路，性价比最高）

### R0-1 GPU 时戳剖析 ✅ 已完成（2026-10-13）

- **交付**：`engine/render/GpuTimer.h/.cpp`（9 段 × begin/end 时戳查询池），  
  `Renderer::recordFrame` 各 pass 打点，`MYVK_GPU_TIMING=1` 每 30 帧  
  `GPU-TIMING frame=.. shadows=.. ...` 日志，编辑器统计面板 GPU 耗时瀑布，  
  `tools/verify_gpu_timing.py`（11 断言，回归第 24 例）。默认开，`MYVK_GPU_TIMER=0` 关。
- **实测参考**（空场景 1600×900 MSAA）：frame 0.35ms ≈ forward 0.13 + post 0.10
  - shadows 0.05 + local_sh 0.05 + cluster 0.01 + depth 0.01。
- **坑点**：见 MEMORY.md #86~#88（条件段必须无条件打点 / fence 初始 signaled 要  
  markSubmitted / timestampValidBits 是队列族属性）。

### R0-2 视图模式 ✅ 已完成（2026-10-13）

- **交付**：`Renderer::ViewMode` 共 8 种（Lit / Unlit / Normal / Roughness /  
  Metallic / Wireframe / Overdraw / LightComplexity）。5+1 种走 pbr.frag 的  
  `FrameUBO.viewModeParams.x` 运行时分支（全局单值，不为它翻倍 .spv）；  
  Wireframe = `PipelineDesc::polygonMode = LINE`，Overdraw = additive 管线。  
  `MYVK_VIEW_MODE=<0..7>` env + 面板下拉（VM-COMBO）。
- **实测参考**：`tools/verify_viewmodes.py` 24 断言全 PASS（回归第 25 例）。
- **E14 追加（2026-10-15）**：`Renderer::ViewMode` 加了 `Count` 哨兵 +
  `kViewModeCount = static_cast<int>(ViewMode::Count)`（=8）。视口左上角的
  模式条按它自动长出格子（`static_assert(kViewModeCount == 8)` 守着短标签表），
  以后加一种模式只要改枚举、`Count` 前的取值检查全部自动跟上。
  `VM-COMBO labels=8` 是 `verify_viewmodes` 的硬锚点，**没有被动过**。
- **坑点**：见 MEMORY.md #89~#91（动态数字面板会毁掉逐像素对照轮 /  
  灰度断言阈值不能设 3 / 热度 ramp 的通道分配）。
- **目标**：视口下拉切换调试视图。
- **改动点**：复用 shader 变体系统加 `MYVK_VIEWMODE_*` 关键字（Unlit=albedo、Normal=G*0.5+0.5、Roughness/Metallic=灰度）、Wireframe 需管线 `VK_PRIMITIVE_TOPOLOGY_LINE_LIST` 或 geometry 扆补（先用 line pipeline 变体最省）、Overdraw=加法混合 log 色映射、LightComplexity=输出簇内灯数。
- **验收**：每种模式截图中心像素/直方图断言显著不同；默认 Lit 与基线逐像素一致。
- **依赖**：无（变体系统现成）。
- **规模**：M

---

## 阶段 1 · 时域地基（现代渲染的地基，TAA 是重中之重）

### R1-1 TAA（时域抗锯齿）✅ 已完成（2026-10-13）

- **交付**：
  - `assets/shaders/fg_taa.frag`：YCoCg 空间 3×3 邻域**方差钳制** + 时间混合；  
    重投影复用 `clipFromView`（与运动模糊同一条链路）；历史失效的三种情形  
    （push 的 valid=0 / w≤0 / uvPrev 出屏）直接输出当前帧；
  - `FrameGraph` 的 **TAA 预 pass**（链的头）：每帧两张持久图  
    `m_taaOut`（视图固定，做链源头与 PostProcess 输入）+ `m_taaHist`  
    （被采样）+ 一次 out→hist 的 `vkCmdCopyImage`；
  - `Renderer` 帧首 Halton(2,3) 子像素抖动（**按帧槽各持一个相位计数器**，  
    见下），`ScreenSpaceSettings::taa / taaBlend / taaClamp`，  
    `MYVK_TAA / MYVK_TAA_BLEND / MYVK_TAA_CLAMP / MYVK_TAA_PHASE` env，  
    面板 "TAA" 勾选框 + Blend / Clamp 滑块。
- **实测参考**：MSAA=1 下 Laplacian>20 计数 **10361 → 4877（−53%）**，  
  且变化 14.8× 集中在几何边缘（排除"其实就是全局模糊"）；两次独立运行  
  可见差异为 **0 像素**（收敛 ⇒ 无鬼影）。
- **顺带修掉一个既有错误**：运动模糊的重投影矩阵原本算成  
  `viewProj × inverse(view)` —— 两者相乘恒等于 `proj`，于是它只对"投影变化"  
  有反应，相机真正平移/旋转时反而**不动**（verify_camfx 的 B2/B3 能过是因为  
  走了 `MYVK_MB_SIM_YAW` 钩子直接覆盖该矩阵）。现改为  
  `prevViewProj(未抖动) × inverse(view)`，运动模糊第一次真正跟相机运动挂钩。
- **坑点**：见 MEMORY.md #92~#97（历史图布局迁移不能省 / 抖动相位必须按槽 /  
  历史图"视图必须恒定"是 PostProcess 的硬约束 / 抗锯齿判据要用二阶差分 +  
  边缘局部性 / 收敛判据容差 / 回归开头三例的焦点时序 flaky）。
- **回归**：26 用例全通过（`_reg_r11_round2.log`）。首次回归 23/26，最开头  
  3 例（frustum_cull / bvh / ibl）失败为空图/拾取超时；逐一单跑均 PASS，  
  定性为窗口焦点时序 flaky，非本任务回归。
- **留给后续**：动矢量缓冲（逐物体速度，处理蒙皮/半透明）、历史锐化  
  （YCoCg 上做 unsharp）、与 SSR/SSGI 的去噪联动。

### R1-1 原始计划（存档）

- **目标**：UE 默认 AA 的等价物；成为 SSR/SSGI 去噪、软阴影、DoF 高质量采样的共享底座。
- **复用**：运动模糊已算好 `m_prevClipFromView` / `clipFromView`（**运动矢量链路是现成的**，只差把速度写进 velocity buffer 或直接在 TAA pass 里重投影）。
- **改动点**：
  1. 帧首抖动投影（subpixel jitter，Halton(2,3) 序列，投影矩阵乘 shear 或 P00/P11 缩放偏移）；
  2. `assets/shaders/fg_taa.frag`：深度加权历史重投影、颜色盒 clamp（AABB clipping）、velocity clamp、lum 保留；
  3. FrameGraph 链尾追加（kPasses 扩到 6，注意 MSAA 情形下历史图用 resolve 后的 HDR）；
  4. 与 MSAA 互斥或共存策略（先做"TAA on 时强制 1x MSAA"，`HdrTarget` 走非 MSAA 路径已有）。
- **坑点预警**：半透明物体无深度 → 速度图里是背景速度，先接受（UE 也有此问题）；蒙皮物体需要顶点级历史或接受重投影误差。
- **验收**：A) TAA 关=基线逐像素不变；B) TAA 开时几何边缘（屋顶/箱子棱）与基线 MSAA4 相比边缘更平滑（对比 MSAA4 截图边缘像素方差）；C) 静态相机连续 N 帧收敛无鬼影（前后帧差异随帧数递减）；D) 相机缓慢平移无拖影（差异集中且方向正确）。
- **依赖**：R0-1（调 TAA 参数需要看 pass 耗时，软依赖）
- **规模**：M~L

### R1-2 自动曝光（直方图 eye adaptation）✅ 已完成（2026-10-13）

- **交付**：
  - `assets/shaders/fg_lum_downsample.frag`：场景色 → **64×64 块平均  
    log2 亮度图**（每纹素 4×4 分层采样）。走片元而不是 compute ——  
    HdrTarget 只把"颜色写"依赖到 FRAGMENT，compute 读它属于未声明依赖  
    （见下"坑点"）。
  - `assets/shaders/fg_lum_histogram.comp`：128 箱直方图。每 workgroup  
    先在 shared memory 里攒，最后每箱只对全局发一次原子。
  - `assets/shaders/fg_lum_average.comp`：单 invocation 扫 128 箱，  
    **两端各裁 2% 百分位**（丢纯黑背景与太阳高光）→ 平均 log2 亮度 →  
    写进 HOST_VISIBLE 结果缓冲。
  - `PostProcess`：专用的 **亮度图 RenderPass**（同步语义面向 compute：  
    WAR 与 RAW 都写进 subpass 依赖 → 全链**不需要任何显式 VkImage 屏障**）、  
    三步测量链的录制（`recordAutoExposure`）、CPU 侧回读 + 指数平滑  
    （`beginFrame(frameIndex, dt)`，`alpha = 1 - exp(-speed·dt)`）。
  - `PostSettings`：`autoExposure / exposureKey(0.18) / exposureSpeed(3.0) /
    exposureMin(0.02) / exposureMax(8.0) / exposureBias`；  
    `effectiveExposure()` 让合成与亮部提取共用同一个曝光源。
  - `PostVolume`：新增 `autoExposureBias` 覆盖项（沿用"浮点哨兵 = -1"  
    约定 → 恰好 -1.0 的偏置无法表达，已在头文件写明）。
  - env：`MYVK_EXPOSURE`（手动值，回归的对照入口）、`MYVK_AUTOEXP`、  
    `MYVK_AE_KEY/SPEED/MIN/MAX/BIAS`、`MYVK_AE_SIM_LUM`（把测量亮度钉死，  
    让收敛曲线确定可断言）、`MYVK_AUTOEXP_DBG`（每帧一行曲线日志）。
  - 面板：`Auto exposure (histogram)` 勾选 + key / speed / EV bias /  
    min / max 滑条 + 当前值读数（`AUTOEXP-COMBO` 日志供自动化）。
- **实测参考**（`verify_auto_exposure.py`，8 轮）：
  - 关闭：exposure **逐位等于**手动值 0.42，`samples` 恒 0（测量链一个  
    pass 都没录），同配置两次运行视口**逐像素一致（平均差 0.0000）**；
  - 收敛：SIM_LUM=4.0 → 单调下降收敛到 **0.04500**（= key/lum，尾部波动 0）；  
    SIM_LUM=0.02 → 单调上升、被 `exposureMax` 夹在 **8.0**；
  - **真实直方图**：亮天空（6,6,6）avgLum=**3.06** → 曝光 **0.0588**；  
    暗天空（0.1,0.05）avgLum=**0.061** → 曝光 **2.947**（**50×** 差异）；
  - 开启 vs 手动 3.0：视口平均差 **68.7**。
- **坑点**：见 MEMORY.md **#98~#100**（HDR 只有 FRAGMENT 可见性 → 测量  
  必须先走片元；亮度测量图用独立 RenderPass 声明 compute 依赖可免显式  
  屏障；回读必须挂在 in-flight 栅栏之后且延迟一帧）。
- **回归**：27 用例全通过（`_reg_r12_round1.log`）。
- **留给后续**：直方图的时间累积（多帧平均降噪）、按区域加权测光  
  （中心测光 / 点测光）、与 R1-3 色彩分级 LUT 的曝光-调色顺序约定。

### R1-2 原始计划（存档）

- **目标**：明暗场景切换曝光自动收敛，替代手动 exposure 滑条。
- **改动点**：compute shader 读 HDR 降采样图建 72/128 bin 亮度直方图 → 平均亮度指数平滑 → 传进合成 push 的 `exposure`（与手动值二选一，`autoExposure` 开关）。
- **复用**：FrameGraph/后处理体积体系（`PostVolume` 加 `autoExposureBias` 覆盖项）。
- **验收**：亮场景→暗场景切换时曝光值随时间收敛（日志曲线断言单调收敛）；关闭时 exposure 精确等于手动值。
- **规模**：M
- **与实现的差异**：①"compute 读 HDR 降采样图"实际拆成"片元降采样 → compute  
  直方图"（HDR 对 compute 没有可见性依赖，见交付说明）；②直方图箱数取 128 的  
  上界；③收敛验收从"切场景"改为"SIM_LUM 钉死亮度 + 真实天空亮暗两轮"两条，  
  前者给确定性的单调收敛曲线，后者证明整条测量链真的在读画面。

### R1-3 色彩分级 LUT（32³）✅ 已完成（2026-10-13）

- **交付**：
  - `assets/shaders/post.frag`：合成前加第 3 个推常量 vec4 `lut`（缩放 / 偏置 /  
    开关 / 混合权重），LUT 采样坐标 `uvw = c·(N-1)/N + 1/(2N)`（纹素中心），  
    插在 **tonemap 之后、暗角之前**（显示域正是 3D LUT 的定义域；暗角属  
    "镜头"而非"调色"）。合成 set 数 2→3、推常量 32B→48B。
  - `PostProcess`：`kLutSize=32`、`bakeLut()`（CPU 烘 R16G16B16A16_SFLOAT  
    3D LUT，**参数变了才重烘**）、`createLutImage3D`、`m_lutDirty`、  
    `lutIdentity()`、`markLutDirty()`。
  - 调色数学固定顺序：`lift c += l·(1-c)` → `gamma c = c^(1/g)` →  
    `gain c *= g` → `contrast c = (c-0.5)·k + 0.5` → `saturation
    c = mix(lum, c, s)`。
  - `PostVolume`：LUT 混合权重覆盖项。env：`MYVK_CG`、`MYVK_CG_CONTRAST /
    SATURATION / LIFT / GAMMA / GAIN / BLEND`、`MYVK_CG_DBG`（每帧一行  
    `LUT-INFO`）。面板：调色面板（勾选 + 5 组滑条 + 混合权重）。
- **实测参考**（`verify_color_lut.py`，8 轮 / 6 组断言）：
  - 关闭：两次运行视口**逐像素一致（平均差 0.0000）**；
  - 开着但参数全默认 = **恒等映射**：`identity=1`，与基线**无可见差异  
    （>18 像素 = 0、平均差 0.06）** —— 顺带钉死采样坐标映射（恒等式是仿射  
    映射，硬件三线性可精确重建）；
  - `contrast=1.8` → 亮度 std **上升**；`contrast=0.5` → **下降**；  
    `saturation=0` → 视口近乎中性灰（|R-G| < 2）；`blend=0.5` 的变化量严格  
    落在恒等与全开之间。
- **坑点**：见 MEMORY.md **#101~#104**（LUT 是固定尺寸资源但描述符集要跟着  
  描述符池重建；构造时立即烘一次，别延迟到 `beginFrame`；恒等=仿射映射）。
- **回归**：28 用例全通过。
- **留给后续**：曲线编辑器、LUT 的 3D 纹理可视化调试、与自动曝光的  
  曝光-调色顺序约定（当前：曝光在 tonemap 前、LUT 在 tonemap 后）。

### R1-3 原始计划（存档）

- **目标**：影视感调色的标准载体：lift/gamma/gain/contrast/saturation → 烘成 32³ LUT → 合成 pass 里一次采样。
- **改动点**：编辑器调色面板（lift/gamma/gain 3 组滑条）→ CPU 或 compute 烘 LUT（先 CPU 烘，尺寸小无所谓）→ `post.frag` 合成前采样 LUT；`PostVolume` 挂 LUT 混合权重。
- **验收**：默认 LUT=恒等映射 → 逐像素不变；改 contrast 输出显著变化。
- **规模**：S~M

### R1-4 （选做）动态分辨率 / TSR 式升采样 ✅ 已完成（2026-10-13）

- **交付**：
  - `PostProcess`：**分辨率解耦** —— `m_outExtent`（全分辨率输出：合成  
    viewport、FXAA 中间图、离屏输出图）与 `m_sceneExtent = 输出 × renderScale`  
    （HdrTarget / 深度 / SSAO / FrameGraph / TAA 历史 / bloom / 分簇 / 光照簇  
    全跟它缩）；两处尺寸共用一个 `scaledRenderExtent()`。
  - `assets/shaders/post.frag`：`upsampleScene()`（线性升采样 + 对比度自适应  
    边缘锐化；`pc.ups.z <= 0.5` 时**逐字** `return texture(hdrTex, uv).rgb`  
    → 关闭即基线）。推常量 48B→64B。
  - 自动缩放控制律：`scale' = scale·√(target/measured)`（像素数 ∝ scale²）；  
    3% 死区 + 1/16 量化 + ≥12 帧冷却（缩放比一变就 waitIdle + 重建整套资源）。
  - `RenderTarget::setRenderScale`、`Renderer::applyRenderScale /
    renderExtent / gpuFrameMs`、DRS 7 个 `PostSettings` 参数。
  - env：`MYVK_DRS`、`MYVK_DRS_TARGET / SCALE / MIN / MAX / SHARPEN`、  
    `MYVK_RENDER_SCALE`。面板：DRS 勾选 + auto/target/scale/min/max/sharpen  
    （`DRS-COMBO` 日志）。
- **实测参考**（`verify_drs.py`，8 轮 / 5 组断言）：全 PASS。
- **坑点**：见 MEMORY.md **#105~#109**（输出/渲染分辨率必须共用同一个计算  
  函数；离屏输出图判"能否复用"要用输出分辨率；关闭要用"逐字返回"而不是  
  "数值接近"；控制器别给目标值做无理由的下限夹紧；读分辨率日志要取最后一条）。
- **回归**：两轮 28/29（唯一失败 `verify_nav_gizmo`，定性为框架焦点时序  
  flaky —— 单跑 4/4 全 PASS、只挂走系统输入队列的鼠标点击断言）。
- **留给后续**：TSR 的完整形态（历史重投影 + 颜色钳制）、按内容自适应锐化、  
  与 R2-1 遮挡剔除的联合（剔除减少的提交量会让 DRS 目标更早达到）。

### R1-4 原始计划（存档）

- **目标**：按 GPU 耗时自动调 render scale；TAA 历史上采样。
- **依赖**：`← R0-1`（需要 GPU 耗时反馈）、`← R1-1`（历史采样底座）。
- **规模**：L（可无限后置）

---

## 阶段 2 · 可见性与 GPU-driven（性能天花板）

### R2-1 Hi-Z 遮挡剔除（两帧延迟）✅ 已完成（2026-10-13）

- **交付**：
  - `assets/shaders/hiz_build.comp`（**MSAA 双形态**）：深度图（可 MSAA，  
    取所有样本的 **max** —— 保守）→ 单采样 R32_SFLOAT 的 mip0。  
    `hiz_build.msaa.comp.spv` 由 `-DVENN_MSAA_DEPTH=1` 生成（与 `ssao.frag`  
    那套完全一致）。
  - `assets/shaders/hiz_downsample.comp`：mip k-1 → mip k，2×2 取 max。
  - `assets/shaders/occlusion_test.comp`：一个 invocation 一个 AABB（`local_size 64`）。  
    8 角点过 `viewProj` → 屏幕 uv 矩形 + NDC z 范围；按物体占屏大小选 mip  
    （让它在该 mip 上占 1~2 纹素）；采样矩形 4 角 + 中心取 max；  
    `zNear > hizNdc + bias` 判遮挡。**保守判据**（角点在相机后 / 被近远裁 /  
    uv 出屏 / Hi-Z 采样 = 天空）一律判**可见**，绝不剔。
  - `engine/render/HiZOcclusion.h/.cpp`：R32_SFLOAT 全 mip 链金字塔  
    （`SAMPLED | STORAGE`，创建后一次 `UNDEFINED → GENERAL` 后**永久停留**  
    —— storage image 只有这一种布局）+ AABB / vis（均 HOST_VISIBLE）缓冲  
    （`kMaxObjects = 8192`，**vis 初始化为全 1**）+ 4 条管线  
    （build 单/多采样、downsample、occlusion_test）+ `record()` 逐帧录制  
    （build → 逐级 barrier → downsample → test → `COMPUTE→HOST` 让 vis 对  
    主机可见）。
  - `engine/rhi/DescriptorSet`：新增 `VK_DESCRIPTOR_TYPE_STORAGE_IMAGE`  
    支持（池的 size 数组 `[3]`→`[4]`）+ `writeStorageImage()`。
  - `engine/render/HdrTarget`：新增 `depthImage(frameIndex)` 访问器  
    （Hi-Z 的 compute 屏障要的是 **image**，不是 view）。
  - `Renderer`：`buildDrawLists` 收集候选世界 AABB（**含本帧已被剔的**）+  
    按两帧前的 `m_occludedSet` 过滤；`drawFrame` 开头在 in-flight 栅栏后回读  
    （用同一 slot 的 `m_hizSlotEntities` 做下标→实体映射）；新增  
    `RenderStats::culledOcclusion`；`recordFrame` 前向 pass 之后录 compute。
  - env：`MYVK_HIZ`（默认关）、`MYVK_HIZ_BIAS`、`MYVK_HIZ_DBG`（每帧  
    `HIZ-STATE`）。面板：`Occlusion (Hi-Z)` 勾选 + bias 滑条（`HIZ-COMBO`）；  
    `CULL-STATS` 追加 `occl / occlOn / hizMips` 三个字段。
- **实测参考**（`verify_hiz.py`，3 轮 / 5 组断言）。场景：一面正对相机的墙挡住  
  墙后 3 个球，墙前 1 个球做"不许误剔"的正对照：
  - 关闭：`occl=0` / `occlOn=0`，**没有 HIZ-STATE 日志**（零 compute），  
    两次运行视口逐像素一致（平均差 0.0000）；
  - 打开：`drawCalls 6 → 3`（3 个被挡的球不再提交）、`occl=3`、  
    GPU 判定 `occluded=3`、金字塔 **10 级**（视口 608×492 → maxDim 608）；
  - **开 vs 关视口逐像素不变（>18 像素 = 0、平均差 0.0000）** —— 这条钉死了  
    "只剔了本来就被挡住的东西"，一旦误剔可见物体这里立刻爆。
- **坑点**：见 MEMORY.md **#111~#113**（深度附件写只对片元可见 → compute 要  
  自补 image barrier；深度值→NDC 用 `d*2-1`；**候选表必须含已被剔的物体**  
  否则被剔者永不重测；两帧延迟回读的时序不变量、vis 首帧必须全 1）。
- **回归**：**30/30 全通过**（`_venn_verify/_reg_hiz_full.log`）。新增的  
  `verify_hiz` 首跑 PASS；此前两轮偶发 flaky 的 `verify_nav_gizmo` 本轮也  
  PASS（60s 级的 `verify_ibl` / `verify_camfx` / `verify_auto_exposure` 均正常）。
- **留给后续**：R2-2 multi-draw indirect（把 vis 直接写成 draw 参数，省掉  
  CPU 回读这一趟）、用"上一帧 viewProj"做测试以消除两帧延迟的 pop、  
  时间性重投影 / 深度 mip 的更激进保守化。


### R2-1 原始计划（存档）

- **目标**：被建筑物挡住的物体不再提交，解决"BVH 只管视锥外"的问题。
- **复用**：深度预通道（CSM 依赖的 prepass 已有）；BVH/绘制列表体系。
- **改动点**：
  1. prepass 后 compute 把深度图降采样成 mip 链（Hi-Z，`VK_IMAGE_VIEW_TYPE_2D_ARRAY` 或 per-mip view）；
  2. compute pass 遍历绘制实例：AABB → 采样 Hi-Z → 输出可见实例列表（compacted）；
  3. 两帧延迟防崩溃（Hi-Z 是上一帧的视角）；先用"CPU 读回可见列表"落地，再演进成 indirect。
- **验收**：遮挡场景（大盒子挡住 N 个小物体）帧时间/绘制统计里被挡物体剔除（drawCalls 下降）；渲染结果逐像素不变（剔除不该改变画面）。
- **依赖**：R0-1（验证收益要看 GPU 耗时，软依赖）
- **规模**：M~L
- **与实现的差异**：① 金字塔用 **per-mip view + 逐级 dispatch**，没用  
  2D_ARRAY（`hiz_downsample` 每级一个 storage view，更直白）；② "输出可见  
  实例列表（compacted）"先落地成 **vis 标志位数组**（每物体 1 bit），  
  compaction 留给 R2-2 的 indirect；③ 深度读取要处理 **MSAA**（编两份）；  
  ④ 验收加了"墙前 1 个球不许被剔"的正对照 + "drawCalls 严格下降"。

### R2-2 Multi-draw indirect + GPU 剔除 ✅ 已完成（2026-10-13）

- **交付**：
  - `assets/shaders/hiz_test.glsl`（新）：`vennHizOccluded()` 公共判据 ——
      
    R2-1 的 `occlusion_test.comp` 与 R2-2 的 `mdi_compact.comp` **共用同一份
      
    数学**（两边判据一旦有逐字差异，"CPU 说可见、GPU 说被挡"会变成随机丢
      
    物体的疑难杂症）。
  - `assets/shaders/mdi_compact.comp`（新）：`local_size 64`；binding 0 = Hi-Z
      
    图、1 = AABB、2 = Catalog、3 = ObjBatch、4 = Instances（writeonly）、
      
    5 = Indirect（裸 `uint cmds[]`）；push 112 B。核心是**一次 `atomicAdd`
      
    兼做槽位分配与计数**：`slot = atomicAdd(cmds[base+1], 1u)` 的旧返回值 =
      
    批内第几个可见实例 = 紧凑化槽位，[1] 的终值 = 本帧要画几个 —— 不需要
      
    第二遍 finalize pass。`firstInstance`（[4]）= 该批候选数的前缀和，任何
      
    槽位都落在预留区间内。
  - `assets/shaders/pbr.vert`：新增 `VENN_MDI` 分支（从结构化的
      
    `MdiInstance { mat4 model; ivec4 aux; }` 取 model + jointBase）；main()
      
    重构成"先定 modelMat/jointBase 再做变换"，三条路径（MDI / 合批 / 基线）
      
    只换数据来路、**数学逐位不变**。
  - `engine/render/MdiDraw.h/.cpp`（新）：4 条缓冲**构造时一次分到底**
      
    （catalog / objBatch / instances / indirect，全 HOST_VISIBLE；indirect 额外
      
    带 `INDIRECT_BUFFER` usage 兼当 SSBO 与间接命令）+ `mdi_compact` 管线
      
    （`kMaxObjects=8192` / `kMaxBatches=4096`，约 1.5 MB/槽）。
      
    `readBack` 只累加各命令的 `instanceCount` —— **回读量 O(批数)**，不是
      
    R2-1 的 O(物体数)。
  - `engine/assets/Mesh.h/.cpp`：新增 `bindGeometry(cmd)`（只绑 VB/IB），
      
    `drawInstanced` 改为复用它。
  - `Renderer`：set 0 binding 15 = 紧凑化实例表（`kGlobalSetBindings` 15→16）；
      
    `buildMdiFrameData()` 生成 catalog/objBatch/命令/提交组，并把候选 AABB
      
    重排成批序（Hi-Z 测试与紧凑化必须同一种编号）；`recordFrame` 深度预通道
      
    之后插入 **Pass 2.5**（`recordPyramid` + `compact`）；前向不透明块四分支
      
    （调试 / MDI / 合批 / 基线）；`recordOffscreenTarget` 在 MDI 打开时**整体
      
    跳过**；`drawFrame` 回读块 MDI 时只回读 draw 参数。
  - env：`MYVK_MDI`（默认关）、`MYVK_MDI_DBG`（每 30 帧 `MDI-STATS`）；
      
    打开时强制 occlusion=1 / instancing=1 / 深度预通道。面板：
      
    `Multi-draw indirect` 勾选 + 统计。`CULL-STATS` 追加
      
    `mdi / mdiSub / mdiBatches / mdiInst`。
- **实测参考**（`verify_mdi.py`，6 组断言）。场景：墙挡住墙后 8 个物体、墙前
    
  2 个做正对照，**共用网格与材质** → 候选 11 → 3 批 → 3 组：
  - 关闭：`mdi=0` / `mdiSub=0`、无 `MDI-STATS`、两次运行逐像素一致；
  - 打开：`mdiSub=3`（11 → 3）、`drawCalls 12 → 4`、GPU 候选 11 / 画出 3 /
      
    剔除 8；
  - **开 vs 关视口逐像素不变（>18 像素 = 0、平均差 0.0000）**，全程零 VUID。
- **坑点**：见 MEMORY.md **#114~#118**（`vkCmdDrawIndexedIndirect` **不带**
    
  VB/IB 必须自己绑 —— 漏了会拿"上一次绑定的残留缓冲"解释 `firstIndex`，
    
  画面形状对、顶点数据错而**验证层一声不吭**；提交组键必须含网格；
    
  Pass 2.5 要放 `recordFrame` 而不是 `recordSceneInto`；`beginFrame` 的长度
    
  校验 catalog/objBatch 是"每物体"、commands 是"每批"；binding 15 必须在
    
  set 0 layout 里声明）。
- **回归**：**31/31 全通过**（`_venn_verify/_regression/_reg_mdi_full2.log`）。
    
  首轮 30/31，唯一失败 `verify_character_window` 是已知的输入注入时序 flaky，
    
  单跑立刻 PASS。顺带给 `run_regression.py` 加了**增量跑**（跳过"上次 PASS
    
  且产物指纹未变"的用例，30 例命中时 25 分钟 → 18.6 秒，见 MEMORY.md #119）。
- **留给后续**：`mdiSub == mdiBatches`（组键含网格，而批键本来就是
    
  "掩码+材质+网格"）—— 收益落在"**GPU 生成 draw 参数 + 零 CPU 回读 + 零帧
    
  延迟**"，不是命令条数。要真落到那句 O(材质数)，前置是**全局几何 arena**
    
  （所有网格共用一份顶点/索引大缓冲，靠命令里的 `firstIndex`/`vertexOffset`
    
  寻址）；再往后是 `vkCmdDrawIndexedIndirectCount`（命令条数也由 GPU 写）
    
  与 `VK_DISPATCH_INDIRECT`。

### R2-2 原始计划（存档）

- **目标**：一次 `vkCmdDrawIndexedIndirect` 画全部同类（同管线同材质集）物体，CPU 提交量从 O(物体数) 降到 O(材质数)。
- **改动点**：instance buffer 结构化（world matrix + material id per instance）、indirect buffer 由 R2-1 的 compute 写 draw 参数（VK_DISPATCH_INDIRECT 或 draw-count 读回）、`Renderer::recordSceneInto` 前向 pass 改 indirect 路径（保留旧路径做 A/B）。
- **依赖**：`←! R2-1`（可见性 compaction 的输出就是 indirect 输入）
- **规模**：L
- **与实现的差异**：① "indirect buffer 由 R2-1 的 compute 写"落地成**新写一个
    
  `mdi_compact.comp`**（R2-1 的 `occlusion_test.comp` 保持"只写 vis 标志"的
    
  职责不变），两者共用 `hiz_test.glsl`；② 走的是"**CPU 预填其余字段 +
    
  GPU 只写 instanceCount**"，不是 `VK_DISPATCH_INDIRECT` / draw-count 读回；
    
  ③ 提交路径改在 `recordFrame`（主视口）而不是 `recordSceneInto` —— 后者只
    
  服务离屏目标，且离屏在 MDI 打开时被整体跳过；④ 提交组键加了"网格"
    
  （见坑点 #114/#115）。

### R2-3 Bindless / descriptor indexing ✅ 已完成（2026-10-13）

- **交付**：
  - `engine/rhi/Device`：创建逻辑设备时启用**一整组** Vulkan 1.2 特性 ——
      
    `descriptorIndexing` + `runtimeDescriptorArray` +
      
    `shaderSampledImageArrayNonUniformIndexing` +
      
    `descriptorBindingPartiallyBound` +
      
    `descriptorBindingSampledImageUpdateAfterBind`（外加 1.0 核心的
      
    `shaderSampledImageArrayDynamicIndexing`）。**要么全开要么全不开**：
      
    缺任一项就整体不 set，`supportsBindless()` 返回 false（"半开"的症状是
      
    管线建不出来，或者数组尾部未初始化的槽被采样而验证层不一定报）。
  - `engine/rhi/DescriptorSet`：`DescriptorAllocator` 支持池级
      
    `VkDescriptorPoolCreateFlags`；`makeSetLayout` 支持 per-binding flags
      
    （`VkDescriptorSetLayoutBindingFlagsCreateInfo`）；新增
      
    `writeTextureAt(set, binding, arrayElement, ...)` —— 按**数组下标**写单个元素。
  - `engine/render/Bindless.h/.cpp`（新）：每帧槽一份 update-after-bind 描述符集
      
    —— binding 0 = `sampler2D textures[]`（运行时大小数组，**PARTIALLY_BOUND**
      
    所以未注册的槽合法；容量按设备 `maxPerStageDescriptorSamplers` 收窄，上限
      
    4096）、binding 1 = `GpuMaterialPage pages[]`（每帧槽一份 HOST_VISIBLE 页表，
      
    4096 页 × 32 B）。`textureIndex()` 惰性注册返**稳定**下标（sampler/view
      
    变了就重写该槽 —— 纹理流收尾会重建 sampler）；`materialPage()` 每帧惰性
      
    分配页并写表（同指针去重）；`bind()` 把 set 1 绑上。
  - `assets/shaders/pbr.frag`：`#if VENN_BINDLESS` 分支把六个贴图槽从
      
    "set 1..6 各一个 sampler2D" 换成
      
    `#define albedoMap textures[gMat.tex0.x]` 等六条宏（`gMat` 在 main 开头
      
    由 `pages[uint(push.emissive.w + 0.5)]` 查一次）—— **函数体里那 6 处
      
    `texture(<槽>, uv)` 一个字没改**，与非 bindless 的那份逐字同源，不可能
      
    因为手抄而分叉。页号借 push 常量最后 16 字节的空闲位（`emissive.w`，
      
    非 bindless 时推 1.0、从无编译单元读取）—— push 被硬件卡在 128 字节，
      
    另开字段会直接越过 `maxPushConstantsSize`。
  - `CMakeLists.txt`：`-DVENN_BINDLESS=1` 再编 8 份
      
    `pbr.bindless.k<N>.frag.spv`（bindless 是**全局开关**、与材质关键字正交，
      
    所以不占用掩码位，只多一族文件）。
  - `Renderer`：set 1..6 的布局改走 `pipelineTexSetLayout()`（bindless 时是
      
    那一份数组布局；grid/sky 同步，否则踩"set 0 被中途打断"那条 VUID）；
      
    前向变体 `fragBaseName` 与调试管线片元按开关切换；`recordFrame` /
      
    `recordSceneInto` 每个前向 pass 绑一次 set 1；`materialPageCode()` 逐 draw
      
    把页号压进 push；`bindMaterialTextures()` 在 bindless 下**空转**（每 draw 的
      
    描述符绑定从 6 次降到 0 次）；`beginFrame` 在 in-flight 栅栏后复位本槽页表；
      
    `BINDLESS-STATS` 日志 + `CULL-STATS` 追加 `bl / blTex / blPages`；
      
    热重载监听 bindless 那一族。**注意**：bindless 改的是描述符布局与 .spv，
      
    所以面板勾选框走 `setBindlessEnabled()`（waitIdle + `createPipelines()`），
      
    不能像 MDI 那样只翻一个 bool。
  - env：`MYVK_BINDLESS`（默认**关**）、`MYVK_BINDLESS_DBG`（每 30 帧统计）。
  - 顶点着色器**一份没改**：`pbr.vert` / `pbr.inst.vert` / `pbr.mdi.vert` 三份
      
    原样复用（顶点阶段从不采样材质贴图，set 1 的声明与它无关）。
- **实测参考**（`verify_bindless.py`，7 组断言）。场景：6 个物体 × 6 个材质、
    
  每材质的 **6 个贴图槽全部非空**（任一条下标映射错位都会立刻改画面），
    
  贴图池只放 14 张（相邻材质**故意共享**）：
  - 关闭：`bl=0` / `blTex=0` / `blPages=0`、无 `BINDLESS-STATS`、两次运行
      
    逐像素一致（平均差 0.0000）；
  - 打开：`bl=1`、`blTex=17`（= 14 场景贴图 + 3 缺省图；**不去重会是 39**）、
      
    `blPages=6`（= 材质数）、`setsBound` ≈ 1/帧；
  - **开 vs 关视口逐像素不变（>18 像素 = 0、平均差 0.0000）**，且
      
    `drawnOpaque` / `drawCalls` 与基线**完全相同**（bindless 只减描述符绑定，
      
    不动几何提交）；bindless + MDI 同时开着也逐像素不变（覆盖间接路径那条
      
    独立的 push 填充分支）。全程零 VUID。
- **坑点**：见 MEMORY.md **#120~#123**（glslang 对采样器数组的非常量下标要求
    
  `GL_EXT_nonuniform_qualifier`，且 `#extension` 必须在任何非预处理记号之前；
    
  数组容量要按设备 `maxPerStageDescriptorSamplers` 收窄；页表必须**按帧槽
    
  分离**否则会撕裂在途读取；`#ifdef` vs `#if` —— 默认定义成 0 的开关不能用
    
  `#ifdef` 判断）。
- **回归**：**32/32 全通过**（`_venn_verify/_regression/_reg_bindless_full.log`）。
    
  因为 build/bin（新增 8 份 `pbr.bindless.k*.frag.spv` + 重链接的 Editor.exe）
    
  与 `tools/*.py` 都变了，产物指纹失效 → 增量机制正确地判定为"全部重跑"
    
  （25m05s，零跳过）。
- **留给后续**：`Material` 的 6 个 `Texture*` 槽仍然在 CPU 侧；真正"材质也在
    
  GPU"（可见性缓冲 / 逐片元材质索引）是 R2-4；`VK_EXT_descriptor_buffer` 可以
    
  把 set 1 的两条 binding 也省成裸地址；容量 4096 目前是固定数组，改成
    
  运行时数组 + `VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT` 可省掉
    
  一部分池预留。

### R2-3 原始计划（存档）

- **目标**：材质不再各占 set 1..6；纹理通过 `texture2D[]` + 非常量索引访问，per-draw 只 push 材质页。
- **改动点**：`Device` 开 `VK_EXT_descriptor_indexing`（或 1.2 feature），建全局大池（`maxPerStageDescriptorSamplers` 上限查询）、`Material` 注册返回 texture index、着色器全部 binding 改数组索引。
- **依赖**：R2-2 的前置（软），可独立做。
- **规模**：M~L（改动面广：所有 pbr 变体 shader）
- **与实现的差异**：① 用 **Vulkan 1.2 feature**（`VkPhysicalDeviceVulkan12Features`）
    
  而不是 `VK_EXT_descriptor_indexing` 扩展 —— 1.2 已把同一组特性核心化；
    
  ② "所有 pbr 变体 shader"实际只改了 **片元**（顶点阶段不采样材质贴图，
    
  三份 vert 原样复用），规模从 M~L 降到 M；③ `Material` 本身**不注册**任何
    
  index（那会给每个材质对象加一层与渲染器绑定的状态），改成渲染时惰性注册
  - 每帧页表 —— 材质对象保持"纯数据"。

### R2-4 （远期）变体爆炸对策：visibility buffer / 前向+簇内材质分拣

- **目标**：控制 `variantSwitches` 增长。
- **先做观测**：R0-1/R0-2 落地后统计真实场景的变体切换次数，再决定是否动架构。
- **规模**：L（暂缓）

---

## 阶段 3 · 光影质量

### R3-1 阴影硬化三件套 ✅ 已完成（2026-10-13）

- **目标**：①级联间混合带（消除接缝）②texel snapping（相机移动阴影不"游动"）③接触硬化（近距离不再漏光）。
- **交付**：
  - `FrameUBO` 尾部新增 `vec4 shadowFilter`（x=混合带宽 y=接触硬化强度）→ **688 B**；
      
    `pbr.frag` 同步声明（新字段只能加在末尾）。
  - **① 级联间混合带**（`pbr.frag::shadowFactor`）：接收点落在"本级远端最后
      
    `band` 比例"内时，同时采样下一级并按视深度线性混合（`sampleCascade` 单级
      
    采样，box 外返回 1）。`MYVK_SHADOW_BLEND`（0..0.5，默认 0 = 关 = 基线）。
  - **③ 接触硬化**（同处）：按接收点在级联内的**相对视深度**缩放 PCF 核半径
      
    （近端收、远端放，`radiusScale = mix(1, 1+4·harden, t)`）。`MYVK_SHADOW_HARDEN`
      
    （0..1，默认 0 = 关 = 基线）。
  - **② texel snapping**（`ShadowPass::updateCascades`）：球心在光轴坐标系里
      
    量化到纹素网格后再重建 lookAt；`ShadowPass::setTexelSnapping(bool)` /
      
    `MYVK_SHADOW_SNAP`（默认 1 = 旧行为）。
- **⚠ 途中修掉一个真 bug（重要）**：量化原先写成"对 `view * mid` 取整再补回
    
  `view[3][0/1]`" —— 但 `mid` 是 lookAt 的 target，`view*mid == (0,0,-dist)`
    
  **恒成立**，量化一个常量等于什么都没做（开关、日志都"正常"，阴影边缘却
    
  照样连续滑动）。正确做法：用光轴基 `(lx,ly)` 把球心的**世界位置**投影后
    
  量化，再 `center = mid + lx·(sx-ax) + ly·(sy-ay)` 重建 lookAt。
- **验收手段（关键教训）**：texel snapping 的效果在 `MYVK_SHADOW_RES=2048` 下
    
  是**亚像素级**（一纹素 ≈ 1cm 世界 ≈ 0.05 屏幕像素），且相机一移动"整幅
    
  画面的视差位移"会把它彻底淹没 —— 靠截图像素差分**根本测不出来**（实测
    
  snap 开/关差分在小数点后 5 位完全相同；降到 256 也一样）。最终改为断言
    
  **画面之外的量**：`MYVK_SHADOW_MATRIX_DBG=1` 打第 0 级光空间矩阵的平移列
    
  （×1e6），断言"量化开 = 分段常数 + 一次整格跳、量化关 = 每步都漂移"。
- **验证**：`tools/verify_shadow_hardening.py`（4 组断言，已进回归 DEFAULT）：
    
  · 组1 关闭态两次运行逐像素一致（max diff = 0）
    
  · 组2 量化：snap 段长 `[5,4]` + 整格跳 977 vs nosnap 每步 121（无重复）
    
  · 组3 混合带：接缝峰值台阶 36.0 → 18.3（−49%），变化局限在 27 行（视口 5.8%）
    
  · 组4 接触硬化：近 0.738 vs 远 0.568（1.30×），3397 像素被改
- **规模**：M


### R3-2 Froxel 体积雾 ✅ 已完成（2026-10-13）

- **目标**：局部雾团、灯光散射（点/聚光在雾里的光柱）、被物体遮挡——替换现在解析式高度雾的"均匀介质"假设。
- **交付**：
  - **`render/VolumetricFog.{h,cpp}`**：`RGBA16F` 3D froxel 图（每 texel =
    `(inscatter.rgb, transmittance)`）+ 两条 compute 管线 + 1×1×1 缺省"透明"图
    （关闭时 set0/binding16 绑它）。网格 = 屏幕 / 16 × 64 层。
  - **`assets/shaders/fog_inject.comp`**：每 froxel 反投影视锥小块 → 世界 AABB →
    密度（高度衰减 + 雾团）→ 采 HdrTarget 深度做积分截断 → 遍历相交灯 + 太阳求
    单位长度内散射（HG 相位）。**MSAA 双形态**（`-DVENN_MSAA_DEPTH=1` →
    `sampler2DMS`）。
  - **`assets/shaders/fog_integrate.comp`**：每 (x,y) 列沿 z 前向积分
    （`T *= exp(-ext·len)`）写回 (inscatter, T)。
  - **`pbr.frag`**：`binding 16 sampler3D vfogTex` + `ubo.vfogParams`（网格 + 开关）；
    合成 `color = color·T + inscatter`。
  - **`FrameUBO`** 末尾追加 `vec4 vfogParams` → **总 704 字节**（原 688）。
    `kGlobalSetBindings` 16 → 17。
  - 环境变量 `MYVK_VFOG` / `_DENSITY` / `_ALBEDO` / `_FALLOFF` / `_BLOB` /
    `_PHASE_G`；`MYVK_VFOG_DBG=1` 打 `VFOG-DBG`。
  - ⚠ **命名冲突**：`MYVK_FOG*` 已被**解析式高度雾**占用 → 全程用 `MYVK_VFOG*`。
    两条雾是独立路径（高度雾解析、froxel 逐格积分），可同时开。
- **验收**：`tools/verify_volumetric_fog.py`（4 组，已进回归 DEFAULT）：
  · 关闭态两次运行差 **0.000000**；开启全屏均值差 0.88；
  · 射灯锥区 3.61 vs 左上角 0.00（**ratio 3609×**，证局部形状）；
  · 密度 0.01→0.05→0.12 差异 **0.31→1.37→2.66** 单调；
  · 高度衰减 0.0→0.5 差异 **1.63→0.005**（雾变淡）。
- **规模**：L

### R3-3 区域光 / IES / light function ✅ 已完成（2026-10-14）

- **目标**：面光源柔高光、IES 光域网贴图（IES parser → 灯附加纹理）、light function 投影贴图。
- **交付**：
  - **数据层**：`scene::LightExtInstance`（32B）走**并行 SSBO**（set0/binding17），
    下标与 `lights[]` 严格对齐 —— `LightInstance` 刻意保持 64 字节不动。
    `Scene::collectLights(out, mods*)` 同步填 CPU 侧 `LightModifierDesc`（名字）。
  - **`render/LightTextureLibrary.{h,cpp}`**：把 cookie 与 IES 剖面烘进**同一张**
    `sampler2DArray`（256²，16 层，`R8G8B8A8_UNORM`，binding18）。第 0 层恒为
    全白（恒等）。内建 7 层：`white` / `cookie:grid|rings|bars` /
    `ies:narrow|wide|batwing`。支持运行时加载 `.ies`（**LM-63 Type C parser**：
    跳关键字行 → 头行 10 数 → 竖/横角数组 → candela 网格 → 双线性重采样到
    `(u=水平角, v=竖直角)`）。
  - **着色器**（`pbr.frag::shadeLocalLightMod`）：
    ① IES = 按"灯→片元"方向的角度采样柱面展开剖面；
    ② light function = 把片元投影到灯锥截面 → `uvc` → 采样 cookie；
    ③ 区域光 = **球光代表点近似**（漫反射取最近球面软化接触；高光沿反射光取
       球面最近点 → 柔高光），等效半径 `r = sqrt(hw·hh)`。
  - **组件**：`SpotLightComponent` 加 `areaWidth/areaHeight/iesProfile/
    lightFunction`（名字）；`SceneSerializer` 存取；Inspector 加 "Modifiers"
    段（Area W/H 滑条 + IES / Light Function 下拉）。
  - 环境变量 `MYVK_IES` / `MYVK_COOKIE` / `MYVK_AREA_LIGHT`（默认全 **关**）；
    `MYVK_IES_FILE="name:path;…"` 运行时加载 IES；`MYVK_LIGHTX_DBG=1` 打
    `LIGHTX-DBG`。`kGlobalSetBindings` 17 → 19。
- **⚠ 关键设计**：三个开关全关时逐灯记录全零 → 着色器走**原样的**
  `shadeLocalLight`（不是"新函数里 mod=1"）→ **基线逐位一致**（避免浮点
  重结合导致最后一位不同）。开关是每灯常量，分支在 warp 上一致，无发散代价。
- **⚠ 教训（测试场景必须先确认信号可见）**：首版测试场景把射灯摆在相机
  正下方（光池在 FOV 之外），结果 cookie/IES 的差分**全是 0**，一度以为
  着色器没生效。实际上是"信号不在画面里"。→ 写验证脚本前先用光池包围盒
  确认光池可见（脚本里 `bright_bbox` 直接做了这个自检）。
- **验收**：`tools/verify_light_modifiers.py`（6 组，已进回归 DEFAULT）：
  · 组1 默认三修饰全关 + 两次运行差 **0.000000**；
  · 组2 cookie：均值差 5.63，光池中线差值**振荡穿越 10 次**（周期性网格，
    不是均匀调暗），光池内 17.65 vs 左上角 0.00；
  · 组3 IES：亮像素 **52756 → 6144**（收窄到 12%）；
  · 组4 区域光：面积 4 / 10 / 22 → 差异 **90.99 < 97.23 < 107.31** 单调；
  · 组5 名字拼错 → ies=0、差异 0.000000（安全回退）；
  · 组6 `.ies` 文件加载：nodes 7 → 8 且新层被用上（差异 7.97）。
- **规模**：M

---

## 阶段 4 · 材质表现力

### R4-1 AlphaMode 加 `Mask`（alpha test）+ 抖动裁切 ✅ 已完成（2026-10-13）

- **目标**：植被叶子、栅栏、铁丝网必须；`Mask` + cutoff 滑条 + 可选抖动模式（配合 TAA 随机化）。
- **改动点**：`Material::AlphaMode` 枚举加 `Mask`、材质面板 cutoff 滑条、`pbr.frag` 加 `discard` 分支（或 `alphaToCoverage` 借 MSAA，两条都做）。
- **验收**：`Mask` 材质的镂空格网从背面能看到透光；Opaque/Blend 路径逐像素不变。
- **规模**：S（**性价比极高，建议提前**）
- **交付**：`AlphaMode::Mask` + `alphaCutoff` + `ditherAlpha`（Bayer 抖动裁切）；
  关键字 **bit3 `VENN_ALPHA_MASK`**（裁切阈值走现有 push 常量的 `skin.w`）；
  材质面板加 "Alpha Mode" 下拉 + Mask 专属的 Cutoff 滑条 / Dither 开关；
  `SceneSerializer` 写 `alphaMode` 字符串（并保留旧 `alphaBlend` 布尔做兼容）。
  验收脚本 `tools/verify_alpha_mask.py`（已进回归 DEFAULT）。

### R4-2 预积分 SSS（皮肤）✅ 已完成（2026-10-14）

- **目标**：最便宜的次表面散射入门——预烘焙散射 profile LUT（曲率→颜色偏移），前向里按曲率采样。
- **改动点**：`Material` 加 `ShadingModel` 枚举（Default/Skin）→ `pbr.frag` 分支：diffuse 乘预积分 LUT（LUT 用脚本生成 256×256 PNG 烘成 BC）+ 可选 2 pass 模糊（先不做）。
- **验收**：anime_girl 皮肤在强侧光下鼻梁/耳缘泛红透光；ShadingModel=Default 逐像素不变。
- **规模**：M
- **交付**：
  - **LUT 生成**：`tools/make_sss_lut.py` → `assets/textures/sss_lut.dds`
    （BC1 + 完整 mip，`srgb=false`）。模型：`D(cosθ,curv) = Σ_φ P(s)·max(0,cos(θ−φ)) / Σ_φ P(s)`，
    `P(s)=exp(−s²/2σ²)`、`s = r·φ`、`r = 1/curv`，σ 逐通道 `(500,300,150) mm`（R 渗得最远）。
    逐行逐通道归一化（N·L=1 处归一到 1）→ **曲率=0 行逐点等于 `max(N·L,0)`**。
  - **着色器**：关键字 **bit4 `VENN_SHADING_SKIN`**（变体 16 → **32**）；
    `pbr.frag::skinDiffuse` 用 `textureLod` 采样 LUT，方向光与
    `shadeLocalLight`/`shadeLocalLightMod` 的漫反射乘子从 `NdotL` 换成查表值；
    曲率 `curv = clamp(length(fwidth(几何法线)) * ubo.sssParams.x, 0, 1)` ——
    **取法线贴图扰动之前**的几何法线（否则贴图高频细节让整张脸变噪点）。
  - **接线**：`FrameUBO` 704 → **720** 字节（末尾 `sssParams`）；set0 从 19 →
    **20** 个 binding（binding19 = `sampler2D sssLut`，纹理解析失败时用程序化
    Lambert 表兜底，绝不黑屏）。开关在**材质字段**上（`Material::shadingModel`），
    尺度在 `MYVK_SSS`（默认 8；`=0` → 曲率恒 0 → 退化成 Lambert 对照组）。
  - **编辑器**：材质面板加 "Shading Model" 下拉（带撤销）；`SceneSerializer`
    写 `shadingModel`（`"skin"` / 缺省即 default）。
- **⚠ 关键坑（采样坐标必须钉到纹素中心）**：LUT 采样器是 **REPEAT** 地址模式，
  而两条轴的端点恰好会取到 `u=1`（正对光）与 `v=0`（平面：`fwidth(几何法线)=0`
  → 曲率恒 0）。边界上的双线性会与"绕到另一侧"的纹素各取一半 —— 曲率 0 的行
  掺进曲率 1 的红边（实测 v=0 读出 0.179，而表里是 0.098），**皮肤所有平坦
  区域无端泛红**。修法：`uv = (x*(N−1)+0.5)/N` 把坐标钉到纹素中心，与
  `make_sss_lut.py` 的 `linspace` 布点逐格对齐。修复后 "Skin(尺度0) vs Default"
  的差从 **5.614 → 0.204**（27 倍）。
- **验收**：`tools/verify_sss.py`（A 组 4 项数值 + B 组 5 项渲染，已进回归 DEFAULT）：
  · A1 曲率0 行 vs `max(N·L,0)` 误差 **0.00000**；A2 曲率1/N·L=0 → RGB=(0.206,0.120,0.058)；
  · A3 曲率1/N·L=1 → (1,1,1)；A4 固定 N·L 下 R 随曲率**单调** 0.294→0.385；
  · B1 Default 球 `MYVK_SSS` 0 vs 16 → **0.000000**（Default 逐像素不变，满足验收）；
  · B2 Skin 0 vs 16 → **6.41**；B3 ΔRGB=(+13.46,+4.79,+0.95)，**ΔR/ΔB = 14.1×**（泛红方向）；
  · B4 Skin(=0) vs Default → **0.2041**（只剩 LUT 量化噪声，同时是采样边界不变量的守卫）；
  · B5 `MYVK_SSS=4` → 0.514 < 6.41/3（曲率轴单调、未饱和）。
  · **否定测试**：故意把采样坐标退化回边界坐标 → B4 读到 **5.6143**，证明该断言非空。
- **已知边界（留给后续）**：① **只作用于"直接光"的漫反射** —— 环境/IBL 的漫反射
  （`irradianceMap` 余弦卷积那一路）没有过 LUT，因为那张表需要"光的来向"，而 IBL 是
  半球积分、没有单一方向。所以背光面的泛红只来自直接光，逆光时偏冷。真想补，得改用
  **球谐/辐照度探针 + 逐方向查表**。
  （曾记"或上 R5-1 反射探针时一起做" —— R5-1 已于 2026-10-15 交付，但**只做了镜面
  反射探针**，IBL 漫反射这一路仍未过 LUT，该欠账**仍开放**，留给 R5-2 辐照度探针。）
  ② 路线图原文里"可选 2 pass 模糊"没做 —— 预积分 LUT 已把交界线柔化，屏幕空间再糊一遍
  的收益低于成本（且会破坏"Default 逐像素不变"的对齐口径）。
  ③ **默认刻度 8 对细几何是"已饱和"的**：`curv = fwidth(几何法线) × 8`，一个 ~200px 的球
  其 `fwidth` 只有 ~0.013（curv≈0.10），而鼻梁那种十几像素宽的特征 `fwidth` 可到 ~0.2
  → curv 被 clamp 到 1（= 最强散射行）。也就是说默认值在脸上基本是"拉满"的，
  想让皮肤含蓄一点就把 `MYVK_SSS` 调到 3~5；想让它成为可调的艺术参数，应该把刻度
  从"全局常量"升级成**材质字段**（`Material::sssCurvatureScale`）。

### R4-3 POM 视差遮挡贴图 ✅ 已完成（2026-10-14）

- **改动点**：材质加 heightMap 槽 + 高度尺度；`pbr.frag` relief mapping（步进 ray-march）；LOD 距离渐出（远处退回 normal map）。
- **验收**：砖墙强视角下纹理凹凸视差正确；heightScale=0 时逐像素不变。
- **规模**：M
- **交付**：
  - **材质**：`Material::heightMap`（`Texture*`，缺省黑）+ `heightScale`（默认 0.05，**UV 单位**）。
    `ShaderKeywords.cpp` 置位 **bit5 `VENN_PARALLAX`**（只看"有没有挂图"，与 `normalScale`
    同一条取舍）；关键字 5 → 6 个，**变体 32 → 64**（非 bindless 64 份 + bindless 64 份）。
  - **着色器**：`pbr.frag::parallaxUv(N, worldPos, uv)` —— 切线空间层步进
    `Vt = normalize(transpose(TBN) * normalize(toEye))`；**层数随倾斜度插值**
    `mix(maxLayers, 8.0, abs(Vt.z))`；`stepUv = (Vt.xy / max(abs(Vt.z), 0.08)) * scale * layerDepth`；
    出循环后**两点线性细化**（否则掠射有明显分层条纹）。`main()` 里六处采样
    （albedo / orm / roughness / metallic / normal / emissive）统一改用 `matUV`，
    **TBN 仍用未位移的 `vUV`**。
  - **三道闸（"关 = 逐像素不变"的落实）**：① `scale <= 0` 第一步 `return uv`
    （逐材质 `heightScale=0` 与全局 `MYVK_POM=0` 都走这里）；② 距离淡出
    `1-smoothstep(pomParams.x, pomParams.y, dist)`（超 `y` 完全退回普通法线贴图）；
    ③ **纹素密度淡出** `1-smoothstep(0.02, 0.08, length(fwidth(uv)))` —— 与距离无关的兜底，
    掠射近处每像素跨好几个纹素时同样退回。
  - **接线**：逐材质深度走 **`push.skin.y`**（push 被硬件卡在 128B，`x/z/w` 都有主，
    `y` 是**最后一个空闲位**）；全局参数走 `FrameUBO` 720 → **736B** 末尾 `pomParams`
    （x/y 淡出区间、z 层数上限、w 全局倍率兼开关）。第 7 张贴图槽 = **set 7**
    （`kTextureSetCount` 6 → **7**；Vulkan 规范只保证 4 个绑定集，本引擎实际要求 ≥ 8）。
    bindless 路径把 height 下标塞进 `GpuMaterialPage.tex1.z`（原是 0 的空闲位），
    缺省槽位必须绑 **`m_defaultBlack`**（白 = 处处等高 → 射线走满退化成大偏移；黑 = 处处最低
    → 循环立刻停 → offset 0）。
  - **编辑器**：材质面板加 `Height Scale` 滑条（带撤销）+ `height` 贴图槽；
    `SceneSerializer` 写/读 `heightMap` / `heightScale`。`Texture::makeCheckerboard` 补 `srgb`
    参数（原先硬编码 sRGB）并贯穿 `TextureSource` 序列化 —— 法线/粗糙度/**高度图**这类
    数据贴图必须线性。
  - **调试口**：启动打 `POM: scale=… fade=… m layers=…`；`CULL-STATS` 尾部追加 ` pom=%u`。
- **验收**：`tools/verify_pom.py`（A4 + B2 + C2 + D3 + E1 = 12 项，已进回归 DEFAULT）。
  场地 = 一面竖墙（plane 绕 X 转 +90° → 法线 -Y；面内轴 u=+X、v=+Z），相机在 -Y 侧：
  · A1 无高度图 vs `heightScale=0` → **0.000000**（早退生效）；A2 同配置两次 → 0；
  · A3 全局 `MYVK_POM=0` vs 无高度图 → 0；A4 变体路由 `pom=0/1`（scale=0 也进变体）；
  · B2 POM 开 vs 基线（掠射 80°）→ **16.05**（真的改了画面）；
  · C1 20° 入射差异 **5.2744** / 80° 入射差异 **16.0499** = **3.0×**（位移 ∝ tan 入射角）；
  · C2 45° 镜像机位：左 **+21 px** / 右 **−21 px**（**反号 = 视差方向正确**，不是静位移）；
  · D1 淡出区间 2..8 m（正视 9.4 m）vs 同机位基线 → **0.000000**；
    D2 同机位放远区间 → **5.2744**（对照，证明 D1 不是假阳性）；
    D3 相机 70 m + 淡出 20..60 → **0.001193**，且远/近基线差异 98.72（墙确实还在画面里）；
  · E1 drawCalls 2 → 2（POM 只改采样坐标，不改提交量）；全程零 VUID。
  · **完整回归 38/38**（DEFAULT 37 → 38；首轮暴露的 3 个 flaky 已一并加固：
    抓图 0×0 改用 `capture_nonempty`、"展开折叠段再读日志"改成幂等 + 重试）。
- **⚠ 关键坑（这次踩了 4 个，全是"验证方法"而非实现）**：
  ① **高度场空间尺度必须 ≫ 位移**：8 格密棋盘（格宽 0.125 UV）下射线走一两步就撞到相邻凹格
  （`h=0`）提前 `break`，位移被格宽截断、不随角度增长 → C1 只有 **1.2×**；换 2 格（格宽 0.5 UV）
  才到 3.0×。
  ② **淡出验证的机位要核算几何**：掠射机位 `y=-1.7` 看着"距墙 10 m"，实际正前方 1.7 m 就撞墙，
  全屏根本淡不掉（D1 读 12.83，被误读成"淡出没接"）。
  ③ **测方向必须 45° 镜像**：位移沿 u 轴，屏幕水平的投影是 `r·u = cosθ` —— 70° 时左右镜像的
  水平分量**同号**，会把方向正确的实现判成"反了"。
  ④ **手写互相关必须先裁视口**：整张截图左右是 ImGui 面板（1258 px 里只有 ~625 px 是视口），
  大片"零位移"把互相关硬拉回 0 → 位移恒测成 0 px。
  另外：**测"均匀位移"要"处处等高"的高度图** —— 棋盘高度图下凹槽位移恒 0，列剖面互相关被
  两半平均掉，同样测不出（故 C2 单开一张全白高度图的场景）。
- **留给后续**：① 内存/性能 —— 层步进是**固定上限循环**（默认 32，实测 48），每个 PBR 片元
  最多 48 次高度图采样；真上生产要改成**高度图 mip 链做层级跳跃**（coarse-to-fine）或用
  **cone step mapping**。② 目前只有**单层高度场**，没有 self-shadow（凸台不会在凹槽上投阴影），
  视觉上"掠射光照下的凹凸感"仍靠法线贴图的明暗。
  ③ 高度图槽占了**最后一个绑定集位**（set7 = 第 7 个）—— 再加贴图槽必须改架构。

### R4-4 延迟贴花（DBuffer）✅ 已完成（2026-10-14）

- **改动点**：贴花体积（AABB + 朝向）→ prepass 后 DBuffer pass（法线/反照率/粗糙度三层）→ 前向里混合；先做平面贴花（贴地标记/弹孔）。
- **验收**：贴花按盒体足迹精确贴在场景表面上（不是贴在盒体几何上）；盒体与表面不相交时被完全拒绝；贴花跟随实体 Transform 与 `size`；`MYVK_DECALS=0` / 组件 `enabled=false` / `opacity=0` / 距离淡尽 四条都逐像素回到基线。
- **规模**：M
- **交付**：
  - **场景侧**：`ecs::DecalComponent`（`size` / `albedo` / `normal` / `tint` / `opacity` / `roughness` /
    `metallic` / `normalScale` / `edgeFade` / `fadeStart` / `fadeEnd` / `enabled`）。
    `SceneSerializer` 写/读 `decal` 字段；实体**不需要 mesh 组件** —— 贴花只用它的 Transform。
    `Renderer::collectDecals()`（`Renderer.cpp:1766` 起）把组件转成 `DecalDrawItem`：
    ```cpp
    const glm::vec3 size = glm::max(dc.size, glm::vec3(1e-4f));
    const glm::mat4 base = scene.worldMatrix(e);   // 不含贴花尺寸缩放
    it.world       = base * glm::scale(glm::mat4(1.0f), size);  // 单位立方体 → 盒体
    it.worldToLocal = glm::inverse(base);          // ⚠ 不是 inverse(it.world)
    ```
  - **一条贴花 = 一个盒体**：渲染器自建单位立方体（`[-0.5,0.5]³`，24 顶点，**画正面 / cull back**）。
    片元**不采盒体自己的表面**，而是按 `gl_FragCoord` 从场景深度 `texelFetch` 重建场景表面的世界
    坐标 P（与 SSAO/SSR 同一套重建），再 `P → worldToLocal` 投到贴花局部空间：
    `|local|/halfSize` 的**最大值** `m > 1` → `discard`；否则按局部 Z 做平面投影取 uv
    `vec2(local.x*side, local.y)/size.xy + 0.5`（`side = sign(nL.z)`，让贴花两面都能看）。
    **这一条盒内测试同时替代了深度测试**：盒体在表面后面的像素，重建点必然落在盒外。
  - **pass 里没有深度附件**（有意为之）：引擎场景深度是 **MSAA** 的（跟 HdrTarget 走，实测 4x），
    而 DBuffer 是单采样；采样数不同没法共用同一个 RenderPass 的深度附件。所以这个 pass 干脆不要
    深度附件，管线 `depthTest/depthWrite` **全关**。着色器编两份：`decal.frag.spv`
    （`VENN_MSAA_DEPTH=0` → `sampler2D`）与 `decal.msaa.frag.spv`（`=1` → `sampler2DMS`），
    运行期按 `HdrTarget::samples()` 选 —— 与 `SSAOPass` 完全一致。
  - **三张 DBuffer**：反照率 / 世界法线 / 粗糙度-金属度，都是 `R8G8B8A8_UNORM`（**线性**，
    做 sRGB 解码会把法线/粗糙度整体扳偏 —— 与"数据贴图必须 `srgb=false`"同一条道理），
    第四通道统一是 **coverage**。全局 set 0 binding **20/21/22**（`kGlobalSetBindings` 20 → **23**）；
    关着时/无贴花时这三格绑 **`m_defaultTransparent`**（RGBA 0,0,0,0）—— **不能**用
    `m_defaultBlack`（alpha=255 会把整屏当成 coverage=1 的黑色贴花）。
  - **混合语义**（`PipelineDesc` 只支持一套，三张图共用）：
    `rgb = src·src.a + dst·(1-src.a)`、`alpha = src·1 + dst·(1-src.a)` ——
    "颜色按 alpha 混、覆盖度按 over 累加"。清屏后 `alpha=0` → 前向 Pass 直接跳过整段
    （这就是 A1/A3 两条"逐像素等于基线"断言的实现依据）；多张贴花叠加 coverage 累积到 1，
    不会出现"两张半透明贴花叠起来反而更淡"。
  - **接线**：贴花数据走 **SSBO** 而不是 push —— `DecalRecord`（std430，**192 B**）：
    `world`(0) + `worldToLocal`(64) + `tintOpacity`(128) + `sizeEdge`(144) +
    `params`(160: roughness/metallic/normalScale) + `fade`(176)。两个 `mat4` 就已经吃满
    push 的 128 B 硬件下限，立 SSBO 之后 push 只剩 **4 B**（记录下标）。
    `worldToLocal` 由 **CPU** 求逆 —— 片元里每个像素做一次 4×4 求逆纯浪费，且贴花是 TRS 组合、
    CPU 侧求逆更稳。`DecalBuffer` 自有 **set 0** = `{FrameUBO, 深度图, 贴花记录 SSBO}`，
    **set 1** = 逐贴花 `{albedo, normal}`（按帧槽、按贴花序号各一套）。
    `FrameUBO` 720 → 736 → **752 B**，末尾 `decalParams`（x = 全局开关，**不是新关键字位** ——
    贴花开关是全局的、可随时切，与 `vfogParams.w` / `viewModeParams` 同一条取舍；
    关键字位留给材质属性）。
  - **coverage 四重淡出**：`opacity` × 边缘软淡出（`1-smoothstep(1-ef, 1, m)`）×
    距离淡出（`1-smoothstep(fadeStart, fadeEnd, |camPos - 贴花中心|)`）× 贴图 alpha。
  - **GPU 计时**：`GpuTimer::kSectionCount` 9 → **10**，新增 `kDecals` 段；
    `GPU-TIMING` 日志尾部加 `decals=%.2f`。`CULL-STATS` 尾部追加 ` decal=%u`
    （`m_stats.decalDraws`）。
  - **诊断口**：启动打 `DECALS: on/off (dbuffer x3 RGBA8, max 64 decals/slot)`；
    `MYVK_DECAL_DBG` 打开后，请求数超 `kMaxDecals` 时打 `DECALS: %u requested, only %u slots
    -> extras dropped`。一帧上限 **64** 张（DBuffer 是单层，再多也只是把同一批像素反复覆盖）。
  - **编辑器**：实体检查器加贴花段（`Enabled` / `Size` / `Tint` / `Opacity` / `Roughness` /
    `Metallic` / `Normal Scale` / `Edge Fade` / `Fade Start` / `Fade End` + 两张贴图槽
    `decal-albedo`、`decal-normal`），带撤销；运行模式（run mode）下只读。
- **验收**：`tools/verify_decals.py`（A6 + B7 + C3 + D2 = **18 项**，已进回归 DEFAULT）。
  场地 = 6×6 m 地面足迹上的平面贴花 + 一张 6×6 m 贴花贴地，相机 `0,-9,7` 俯视：
  · **A1** `MYVK_DECALS=0`（场景里有贴花）vs 无贴花场景 → `>18=0 平均=0.000000`（pass 根本没录）；
    **A2** `enabled=false` → `0.000000`（`collectDecals` 就把它滤掉了）；
    **A3** `opacity=0`（pass 照跑）→ `0.000000`（coverage 乘了 opacity → 前向整段跳过）；
    **A4** 盒体悬空（最低点离地 **2.2 m**）→ `0.000000`（重建点全在盒外，盒内测试有效）；
    **A5** 同配置两次运行 → `0.000000`（画面本身稳定，后续断言可信）；
    **A6** `CULL-STATS decal = 0 / 0 / 1`（关 / 无贴花 / 打开）；
  · **B1** 启动日志 `on` / `off`（默认关，靠 `MYVK_DECALS` 打开）；
    **B2** 打开 vs 基线 → **`>18=28495`，`平均=4.4477`，`max=90`，`bbox=[421,379,804,574]`**
    （真的改了画面）；**B3** 差异包围盒面积占视口 **25.0%**（上限 45% → 差异确实局部）；
    **B4** 打开时 `decal=1`；**B5** `GPU-TIMING depth=0.01 ms decals=0.02 ms`（时戳段接上了）；
    **B6** 贴花随实体右移 4 m → 包围盒中心 x **612 → 789**（位姿确实来自实体 Transform）；
    **B7** 盒体 6 m → 11 m → 受影响像素 **28495 → 73076**（**2.56×**，`size` 参与了世界矩阵）；
  · **C1** 淡出区间 2..8 m、相机 11 m（早已淡尽）vs 无贴花 → `0.000000`；
    **C2** 同机位、区间放远作对照 → **4.4477**（证明 C1 不是假阳性）；
    **C3** 相机 76 m、区间 20..60 m → `>18=0 平均=0.000000`，且远/近基线差异 **28.80**
    （地面确实还在画面里，C3 不是"拍到空场景"的假通过）；
  · **D1** `drawCalls 2 / 2`（贴花只改画面，不改提交量）；**D2** 全程零 VUID。
  · **完整回归**：DEFAULT 38 → **39 例**（新增 `verify_decals`）。首次全量 **36/39** ——
    唯一的**真失败**是 `verify_collision_play` 抓到 `DecalBuffer` 析构漏掉 **20 个 Vulkan 对象**
    （见下"交付后由完整回归暴露的缺陷"）；另两例 `verify_drs`（锐度指标 2.290 vs 2.428，
    边缘值）与 `verify_drop_to_floor`（pick 命中 False = 注入点击被吞）单独复跑即 PASS，
    判为既有 flaky。修掉泄漏后定向复跑 `verify_decals` + `verify_collision_play` + 两例 flaky
    → **4/4 PASS**，缓存 39/39。
  · 可视化交付：`_venn_verify/decal_compare.png`（baseline / decal on / delta ×4 /
    盒体悬空被拒 / 贴花右移 4 m 五联图）。
- **⚠ 交付后由完整回归暴露的缺陷（已修）**：`~DecalBuffer()` 里写的是
  `m_frames.clear()` —— 只析构了 `std::vector` 的**元素**，而 `DecalFrame` 的成员
  （`VkImage` / `VkImageView` / `VkDeviceMemory` / `VkFramebuffer`）**都是裸句柄**，
  析构什么都不做 → 这些对象**永远不会被 `vkDestroy*`**。3 张 DBuffer × 2 帧 =
  6 image + 6 view + 6 memory + 2 framebuffer = **20 个对象泄漏**。
  而 `DecalBuffer` 是**无条件构造**的（不在 `MYVK_DECALS` 判断里），所以这个泄漏与
  "贴花开不开"无关，每次运行都会在 `vkDestroyDevice()` 时被验证层点名。
  对照 `engine/render/VolumetricFog.cpp` / `SSAOPass.cpp` —— 它们的析构都规规矩矩调了
  `destroyFrames()`，这里是唯一的异类。修法：把 `m_frames.clear()` 换成 `destroyFrames()`。
  **发现路径值得记住**：全套 39 例里**只有** `verify_collision_play` 会让编辑器
  **优雅退出**，因而也只有它的日志里会出现 `vkDestroyDevice(): ... leaked objects`
  （其余脚本都是强杀进程，泄漏报告根本不会产生）。所以它现在是本项目的
  **资源泄漏门**，在 `run_regression.py` 的 `TRIGGERS` 里挂上了**整个渲染层**
  （`RENDER_MODULES`），并且回归脚本会扫描本轮跑过的每个用例日志做泄漏兜底。
- **⚠ 关键坑（这次踩了 5 个，其中 ① 是真正的 head-line bug）**：
  ① **深度附件里存的是 NDC z，不是 `(ndc+1)/2`** —— 本引擎的投影由 `glm::perspective` 生成、
  **没有** `GLM_FORCE_DEPTH_ZERO_TO_ONE` → GLM 的 NDC z ∈ [-1,1]；而 Vulkan 的深度范围
  **恒为 [0,1]**（`VkViewport::minDepth=0/maxDepth=1` → `z_f = ndc_z`）。两者叠起来的后果是
  **深度附件里存的就是 `ndc_z` 本身**。所以重建时**不能**照抄 `space.glsl::vennViewPosFromDepth`
  的 `depth*2-1`（那是二次映射）—— 后果是重建点被拉近 ~1.7 倍（地面上的点全部飞到场地上空
  2~3 m），盒内测试随之全数失败，**一张贴花都看不见**（`diff on vs base: >18=0`）。
  **定位方法**（值得记住）：做**窄带身份测试** —— 地面在 9..16.5 m，按正确约定深度应落在
  `1.0005 - 0.2001/dist` = **`[0.9817, 0.9883]`**，而 `(ndc+1)/2` 约定下应是 `[0.9908, 0.9942]`；
  探针读到三档 `n=58246 d≈0.9817 / n=39522 d≈0.9850 / n=26030 d≈0.9883` —— 一击定案。
  ② **`worldToLocal` 必须是"米空间"的逆**：`it.world = base * scale(size)` 含尺寸缩放，
  若顺手写 `worldToLocal = glm::inverse(it.world)`，反投影出来的 `local` 是**单位立方体坐标**
  （`[-0.5,0.5]³`），而片元里"盒内测试"（`|local|` vs `size/2`）和"uv 投影"（`local.xy/size.xy`）
  两处都按**米**写 → 三套空间混用。正确写法 = `glm::inverse(base)`（**不含** size 缩放）。
  ③ **法线的矩阵要用转置**：局部 → 世界用 `transpose(mat3(rec.worldToLocal))`（刚体逆的转置
  = 旋转本身），**不能**用 `mat3(rec.world)` —— 那个里面带非均匀 `size` 缩放，会把法线扭歪。
  ④ **导数必须在所有 `discard` 之前算完**：被 `discard` 掉的辅助调用，其 `dFdx/dFdy` 是垃圾。
  `decal.frag` 里两处导数（重建法线的 `dFdx(P)/dFdy(P)`、贴图 uv 的 `dFdx(duv)/dFdy(duv)`）
  都提前无条件算完，贴图采样因此走 **`textureGrad(decalAlbedo, duv, ddxUv, ddyUv)`**
  而不再是 `texture()`。
  ⑤ **`re.Match.group(-1)` 抛 `IndexError`** —— Python ≥3.6 负数下标不再表示"最后一组"。
  `verify_decals.py` 的 `last_match` 默认参数从 `-1` 改 `1`（崩在 B1，A 组 6 项已全过，
  排查成本白白浪费了一轮）。
  另外三条**非坑但是重要取舍/方法**：
  · **调试值会被色调映射饱和** —— 早先所有"把调试量编码成颜色再读数"的推断
  （`|P.y|/20`、`local.z` 编码等）**全部作废**：调试写 `coverage=1`，前向用 albedo × ~2.3
  曝光再 tone-map（`x/(x+1)`），输出恒贴上界。**要改成饱和原色做分类编码**
  （黑/蓝/黄/品红/绿）+ 直接看截图，才能区分"哪条分支被走到"。
  · **顶点属性的 numeric type 必须与 `VkVertexInputAttributeDescription::format` 一致** ——
  `assets::Vertex` 的 joints 是 `R8G8B8A8_UINT` → GLSL 必须写
  `layout(location = 3) in uvec4 inJoints;`（**不能写 `ivec4`**），否则触发
  `VUID-VkGraphicsPipelineCreateInfo-Input-08733`，管线创建失败、画面完全没贴花 ——
  但 `CULL-STATS decal=1` 照样报 1，极具误导性。
  · **凸体画正面而不是背面**：对凸体来说正面的屏幕投影恰好等于整个盒体的屏幕投影，所以
  "表面点在盒内"的像素一定被覆盖到。代价：相机在盒体内部时（全为背面）一个片元都不产生 ——
  已知边界情形，贴花大到把相机包进去才会遇到。
- **留给后续**：
  ① **贴花贴图与 DBuffer 都没有 mip 链** —— 掠射角下 normal map 在 1:1 采样处出现细横向条纹
  摩尔纹（程序化纹理是单层，`textureGrad` 也拿不到 mip）。要修得给贴花贴图生成 mip，或按屏幕
  导数手动做 LOD 衰减（`textureGrad` 的现成接口）。
  ② **只有盒体（AABB）贴花** —— 没有投影式（projector）贴花；贴花也还没有自己的
  height/normal→DBuffer 的视差，贴花自身的凹凸仍是平法线。
  ③ **DBuffer 占了全局 set 0 的 3 个 binding（20/21/22）**，且 `coverage=0` 的像素也要走一遍
  前向的混合判断（有分支、不采样）。量大之后应改成"stencil 标记 + 只在被覆盖像素上前向混合"。
  ④ **单采样 DBuffer 与 MSAA 场景的几何边缘** —— 贴花贴着物体轮廓时，边缘的抗锯齿来自场景几何
  自身的 MSAA，而 DBuffer 侧是单采样，边界处可能出现 1 px 级的硬边。
  ⑤ **一帧 64 张上限**（`kMaxDecals`）—— 超出直接丢弃并 WARN。DBuffer 是单层，要做几百张贴花
  需要分块（clustered）或改成 forward+ 里的逐簇贴花列表。

---

## 阶段 5 · 反射与 GI

### R5-1 反射探针 ✅ 已完成（2026-10-15）

- **目标**：室内反射（SSR 只反射屏幕内的东西）。
- **改动点**：探针组件（AABB + capture 按钮/自动频率）→ 复用通用 RenderTarget API → 粗糙度混合（mip = roughness，GGX prefilter 烘焙管线）→ box projection 修正 + set 0 上按盒体范围混合。
- **验收**：镜面球在室内（背对窗口）反射出探针内容；探针关=基线不变。
- **规模**：L
- **交付**：
  - **场景侧**：`ecs::ReflectionProbeComponent`（`engine/ecs/Components.h:379`）——
    `extents`（**盒体半轴**，米，沿实体局部轴，默认 `10,10,5`）/ `intensity` / `boxProjection` /
    `blendDistance` / `enabled`。探针的**位置就是实体 Transform**，不另存一份心点。
    `SceneSerializer` 写/读 `probe` 字段（`reflectionProbeToJson` / `reflectionProbeFromJson`）；
    **不带 mesh / material / decal 但有 `probe` 的实体也要被保留**（`SceneSerializer.cpp:1145`
    的存活条件是四者取或 —— 漏一个探针实体就会被序列化整个吃掉）。
  - **场景收集**：`Scene::forEachReflectionProbe(fn)`（`engine/scene/Scene.h:80`）遍历 `each<>`，
    **`if (!pc.enabled) return;` 先滤掉**，只把启用的推给回调。**关掉的探针连盒体都不进**
    → 与"关了 = 基线"直接挂钩。
  - **捕获链路**（`engine/render/ReflectionProbe.*`，`ReflectionProbeSystem`）：
    ① 逐面设相机 → 渲染进 6 层 capturing 立方体（`HdrTarget::kColorFormat`，带
       `VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT`）；
    ② 6 面 → cube array 图（**逐面 `copyFace()`，每面一个 `VkImageCopy` region，
       `baseArrayLayer = f / layerCount = 1`**）；
    ③ GGX prefilter（`ibl_prefilter.comp` 同源）烘 `mips = prefilterMips` 级链，粗糙度 → mip；
    ④ 结果进 set 0 的 `samplerCubeArray`（binding **23**），探针参数走 SSBO（binding **24**）。
    关着时/无探针时 binding 23 绑 **1×1 全黑 cube array（6 层）兜底图** —— **不能"不绑"**：
    pbr.frag 声明了该 binding，只要 layout 里有这个资源，创建任何用 pbr.frag 的管线时都必须有
    有效描述符，否则报 `VUID-VkGraphicsPipelineCreateInfo-layout-07988`（与 SSAO 缺省白图 /
    贴花全透明图同一条既有约定）。
  - **全局 set 0 布局**：`kGlobalSetBindings` → **25**（23 = `samplerCubeArray probeMap`，
    24 = `ProbeBuffer` SSBO）。`ProbeGpu`（`ReflectionProbe.h:74`）= `mat4 worldToLocal`（世界→局部，
    只含旋转+平移）/ `vec4 extents`（xyz = 盒体半轴，w = `blendDistance`）/ `vec4 params`
    （x 强度 / y 启用 / z 盒投影 / w 保留）/ `vec4 worldPos`（xyz = 探针世界坐标，盒投影用）——
    std430 **112 B**，`static_assert` 钉住大小。
    ⚠ GLSL 侧 `struct Probe` 必须**提到 interface block 外**（结构体不能定义在块里）；
    `static_assert` 只保证**大小**，**字段顺序与语义只能靠人同步**。
  - **`FrameUBO` 752 → 768 B**（`RenderTypes.h`，`static_assert(sizeof(FrameUBO) == 768)`）：
    末尾追加 `probeParams`（offset **752**）。`envUint(name, min, out)` 的语义要记住 ——
    **缺失或小于 min 都返回 false 且不改 out**，不是增量赋值。
  - **重捕判定三条或关系**：① 显式请求；② 数据变了 或 `captures == 0`（首次必烘）；
    ③ `autoCaptureFrames > 0` 且过期。**`autoCaptureFrames` 默认 0（关）** ——
    自动频率是 opt-in，默认行为是"变了才烘"。
  - **提交纪律**：**一个探针 = 一次独立提交**（`m_commandPool->beginOneTime()` 搬进探针循环）。
    `rhi::CommandPool::beginOneTime()` 每次 `vkAllocateCommandBuffers` 出一个独立缓冲，
    `endAndSubmit()` 内部 `vkQueueSubmit` + `vkQueueWaitIdle` + `vkFreeCommandBuffers` ——
    **同一个 cmd 不能录两次、也不能提交两次**，所以多探针必须各取各的缓冲。
  - **编辑器**：实体检查器加探针段（`Inspector ▸ Reflection Probe`，`sectionHeader` **默认折叠**），
    含 `enabled` / `extents` / `intensity` / `boxProjection` / `blendDistance` + `Capture` 按钮，
    带撤销，运行模式只读；`INS-PROBE-HEADER` 是"探针节存在"的日志锚点。
  - **默认值**：`captureSize = 128`（夹到 8..1024）/ `prefilterSize = 64`（上夹 512）/
    `prefilterMips = 5`（上夹 8，再按 `1+log2(size)` 夹）/ `prefilterSamples = 256` /
    `maxProbes = 4`（上夹 16）/ `autoCaptureFrames = 0`（= 关）/ `enabled = false`。
  - **环境变量（8 个）**：`MYVK_PROBES` / `MYVK_PROBE_INTENSITY` / `MYVK_PROBE_SIZE` /
    `MYVK_PROBE_AF` / `MYVK_PROBE_MAX` / `MYVK_PROBE_PREFILTER_RES` / `MYVK_PROBE_PREFILTER_MIPS` /
    `MYVK_PROBE_SAMPLES`（另有诊断口 `MYVK_PROBE_DBG`）。前四个（`enabled` / `intensity` /
    `captureSize` / `autoCaptureFrames`）接进 `FeatureRegistry`（Engine 面板 + env + 序列化四路派生）；
    后三个是"装机参数"（决定显存与烘一次的时间），**只走 env 与代码默认值、不接 UI**。
  - **诊断口**：启动打 `PROBE: created enabled=%d capture=%d prefilter=%d mips=%d max=%d`、
    `PROBE-RES`、`PROBE-CAPTURE`、`PROBE-DECIDE`（逐帧判定，`changed` / `need`）；
    `MYVK_PROBE_DBG` 下每帧打 `PROBE-FACE`、并在回读解析后打三个**互相交叉验证**的指标：
    `PROBE-DUMP`（逐面加权平均亮度）/ `PROBE-NZ`（逐面**非黑纹素计数**）/
    `PROBE-MID`（逐面**中心纹素**实测色）。三个一起看才不会被"均值 0 但中心有颜色"
    这种自相矛盾的数据带偏 —— 均值会被"大面积暗 + 一小块亮"压扁（亮块占 1/64 时均值只剩 0.3），
    中心纹素只有一个采样点说不清整面，**非黑计数直接回答"这一面到底有没有内容"**。
- **验收**：`tools/verify_reflection_probe.py`（A4 + B7 + C4 + D3 + E1 + F2 = **21 项**，
  已进回归 DEFAULT）。场地 = 24×24×8 m 封闭屋子（地板 + 天花板 + 四壁），四面墙各一块自发光面板
  **PX 红 `(3,0,0)` / NX 绿 `(0,3,0)` / PY 蓝 `(0,0,3)` / NY 黄 `(3,3,0)`**，另加**天花板灯**
  `(0.6,0.6,3)` 与**地面标记** `(3,1.2,0)`（让 ±Z 两个面也有内容）；中心一颗金属镜面球
  （`metallic=1 roughness=0.05 R=0.6 @ z=1.2`），探针与球心重合（`PROBE_EXT=[12,12,9]`）。
  **关键机位 `CAM = "0,-4,1.6,90,5.7"`**：球心那个像素的法线朝向相机 → 反射方向 = **-Y
  = 相机背后** → 读到那块**黄**面板。**SSR 在这个机位必然失败**（屏幕里根本没有后方的东西），
  所以"球心出现黄色"只可能来自探针 —— 这就是验收的因果闭环。
  · **A1–A4**（关 = 基线的三条零噪声）：`MYVK_PROBES=0`（场景里有探针）/ `enabled=false`
    （全局开）/ 同配置两次运行 → **全部 `>18=0 均差=0.000000`**；`MYVK_PROBES` 关着时
    **零捕获、零资源分配**（日志里既无 `PROBE-CAPTURE` 也无 `PROBE-RES`）；
  · **B1–B7**：`PROBE: created enabled=1 capture=64 prefilter=64 mips=5 max=1`；
    `PROBE-CAPTURE probes=1 size=64x64 ... total=1`（**只烘一次**，静态场景不重烘，
    `PROBE-DECIDE` 首条 `changed=1 need=1`、其后 3600+ 条全 `0`）；
    `PROBE-NZ +X=1003 -X=1003 +Y=1003 -Y=1003 +Z=480 -Z=4096`（**六面都有内容**，
    -Z 那个 4096 = 整层 64×64 全亮 —— 地面那块橙标记被它看到）；
    `PROBE-MID` 六面中心色与摆放**逐面精确对应**：`+X=(3,0,0) -X=(0,3,0) +Y=(0,0,3)
    -Y=(3,3,0) +Z=(0.6,0.6,3) -Z=(3,1.2,0)` —— 六面互不相同，**面序对应正确**；
  · **C1/C3/C4/C5**：探针开 vs 关 `>18=68284 max=250 均差=16.7017 包围盒占比=0.568`；
    **球内平均差 65.653 vs 球外 7.0691（比值 9.3）**—— 差异集中在镜面球上（金属 F0=1 vs 介质 0.04）；
    球心区 `开=(109.8,86.3,0.9) 亮度=85.1` vs `关=(0,0,0) 亮度=0.0`，其中 **r-b=108.9**
    → 就是那块黄面板；`MYVK_PROBE_INTENSITY=2` 球心亮度 `85.1 → 111.7`（强度参数生效）；
  · **D1–D3**（盒投影 / 盒体）：**偏心探针**（`PROBE_DX=-5`，视差 ~24°）开/关盒投影
    球区平均差 **22.250**（盒投影真的在修正视差）；**小盒体**（半轴 2 m）包围盒占比 **0.109**
    vs **大盒体**（半轴 40 m）**0.568** —— `extents` 真的在决定作用范围；
  · **E1**（因果实验）：把黄面板从 -Y 墙搬到 +Y 墙重新捕获 → 球心 **85.1 → 65.2** ——
    反射内容确实来自**本次**捕获，不是缓存/常量；
  · **F1/F2**（面板）：`MYVK_EDITOR_SELECT=<探针实体>` → `INS-PROBE-HEADER` 出现；
    选中**无探针组件**的实体（`Mirror`）→ 探针节**不**出现（否面对照）。
  · 可视化交付：`_venn_verify/probe_compare.png`（**八联图** = 基线 / 探针开 / 开−基线 ×4 /
    强度×2 ×4 / 大盒 ×4 / 小盒 ×4 / 黄面板搬家 ×4 / 盒投影开−关 ×4，全部裁在视口矩形内）——
    基线里球是**纯黑剪影**（IBL 全黑、无环境可反射），探针一开球面立刻映出整间屋子
    （红/绿墙板在两侧、顶部蓝灯被球顶反射）——这就是本特性存在的理由的直观证据。
  · 回归接线：`verify_reflection_probe` 已进 `run_regression.py` 的 **`DEFAULT`**（**46 → 47 例**）
    与 **`TRIGGERS`**（`verify_reflection_probe → engine/render/ReflectionProbe.*`）；
    `check_regression_map.py` / `check_feature_surface.py` **均 PASS**。
    ⚠ **本轮的定点复跑**（非全量）：`verify_reflection_probe` / `verify_decals` /
    `verify_inspector` / `verify_add_menu` → **4/4 PASS**。**没有**跑全量回归
    （按既定纪律，等明确指令）—— 所以"是否引入回归"这一条**尚未被全量证实**。
- **⚠ 关键坑**：
  ① **（head-line bug，本轮真凶）"只有最后一个立方体面有内容" —— 根本不是拷贝/面序问题，
  而是相机 UBO 复用。** UBO 是**记录期写、执行期读**的内存：一次 `vkQueueSubmit` 里 6 个面若
  共用同一份 `FrameUBO`，GPU 执行第 0 面时那块内存里**已经是第 5 面写进去的矩阵**了 ——
  于是 6 个面**全部**按第 5 面的相机渲染。而绘制列表是**记录期**按各面相机剔除的（第 0 面的
  列表里只有 +X 那个物体），用"朝 -Z 的相机"去看它，它落在 90° 视野之外 → 该面只剩背景。
  表现 = **"只有最后一个面有内容"**，极易误诊成拷贝或面序出错。
  **修法**：`Renderer::kProbeFaceUboSlotBase = 64`，**每个面独占一个 UBO 槽**（base + 0..5），
  并且**一个探针一次独立提交**（否则跨探针又会撞）。
  **基址取 64 而不是紧邻 0**：离屏目标的槽 = `m_targets` 下标（主视口 + 子视口 + 预览窗口），
  留出余量才不会撞上。**离屏目标早就踩过同一个坑**（`FrameResources::OffscreenUbo`），
  这是第二次 —— 这条教训应按"渲染系统通用约束"记住。
  *排除过程值得记住*：先怀疑"`VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT` 图像的拷贝必须 6 层一组"
  → **WebSearch 证伪**（标准立方体贴图加载器普遍逐层建 region，SaschaWillems 的 loader 亦然）；
  再怀疑缺内存屏障 → 加全量 `VK_PIPELINE_STAGE_ALL_COMMANDS_BIT` 屏障后结果**完全不变**（不是同步）；
  最后做**切分实验**：`MYVK_PROBE_SAMEFACE=1`（6 个面全用第 5 面的相机剔除 + 渲染）
  → **6 层全都有内容**（`PROBE-NZ` 六面各 4）→ 一击定案：**拷贝链路没问题，问题在渲染**。
  （该临时钩子已完全移除，`grep -c "TEMP-DEBUG\|SAMEFACE"` = 0。）
  ② **mip 链长必须与分辨率自洽**：N×N 的图最多只有 `1+log2(N)` 级。`MYVK_PROBE_PREFILTER_RES=8`
  + `_MIPS=5` 是**规范违规**（8×8 只支持 4 级），VUID 直接报
  "mipLevels must be less than or equal to the number of levels in the complete mipmap chain"，
  图像创建失败、整条捕获链作废。**修法**：`createReflectionProbes()` 里把 `prefilterMips`
  夹紧到 `1+log2(prefilterSize)`（同时把 `prefilterSize` 上夹到 512、`prefilterMips` 上夹到 8）。
  ③ **探针混合在 IBL 总开关内部**（`pbr.frag` 的 `if (ubo.envParams.x > 0.5)`）——
  探针替换的就是 IBL 的镜面预过滤项，菲涅尔 / BRDF 表也从那一路来。语义上说得通
  （"关掉环境光"就包含探针），**但 `MYVK_PROBES=1, MYVK_IBL=0` 会静默失效**。
  本轮只在面板 hint 里写明，**没有**把探针混合移出 IBL 分支（那会削弱"关 IBL = 基线"
  的对齐口径）—— 是**有意识的取舍**，不是遗漏。
  ④ **`ambientScale` 会把探针一起乘没**：`ambient = (diffuse + specular) * ao * ambientScale * ssao`
  → 验收场地**必须** `ambientScale = 1.0`，否则探针再亮也是 0。
  ⑤ **`sectionHeader(name, defaultOpen=false, e)` 默认折叠**，参数行在 `if (!open) return;` **之后**
  → headless（`MYVK_EDITOR_SELECT`）**点不到**那些参数控件，只能断言"节标题在不在"。
- **留给后续**：
  ① **探针与 SSR 的权重混合还没做**（roadmap 原文要求"SSR 按深度/roughness 权重混合"）——
  当前是"盒体内的表面直接叠探针镜面项"，SSR 命中与否不影响探针项。要做得给 SSR 的命中掩码
  加一路输入（深度合理 + roughness 低才算命中），再按命中权重在两者间插值。
  ② **`blendDistance` 已进组件与 GPU 结构，但相邻探针之间的边界混合还没实现**（多探针
  `MYVK_PROBE_MAX > 1` 时仍会看到硬接缝）。
  ③ **捕获不含动态物体**（`uploadSceneState(scene, ext, resetSkinning=false)` 复用主场景状态，
  但捕获是一次性烘焙，镜头内的动态物体不会自动重烘，除非靠 `autoCaptureFrames` 或手动 Capture）。

### R5-2 （远期）lightmap / 光照探针 烘焙

> ⚠ **落地设计 → `world_scale_roadmap.md` §14（W9）**。那里的结论：做**辐照度探针网格
> （SH L2）**而不是 lightmap —— lightmap 要 UV2（`Vertex` 没有）+ `pbr.frag` 加纹理槽
> （7 个已满），而探针只要一个 SSBO。
> 另：**"室内不崩"的便宜解法（W6，探针当辐照度源）成本是 W9 的 1/10，排在 W9 之前**。

- **目标**：静态场景 GI 不再依赖有噪的 SSGI。
- **改动点**：CPU 路径追踪器（借用 ray query？Vulkan 现在没开 ray tracing——先用 CPU MC 或 irradiance 点云），烘 irradiance probe SH 网格（比 lightmap 便宜，先做探针）。
- **规模**：L（暂缓）

### R5-3 （远期）DDGI 动态探针 GI

- **规模**：L（依赖 R5-2 的探针基础设施）

---

## 阶段 6 · 内容生态（差距最大，按游戏内容需要挑）

> ⚠⚠ **2026-10-18 起，R5-2/R5-3/R6-2~R6-5 的落地设计、顺序与验收判据由
> `docs/world_scale_roadmap.md`（W 系列）接管。** 本文档下面这些段落保留"目标与改动点"，
> 但有三处已被实测推翻或改写：
>
> 1. **R6-4 地形不再是"可无限后置"**，它排在材质实例与"第一个真实关卡"**之后**，
>    但仍在地形之前 —— 因为"改一次 roughness 要点几百次"是关卡的前提，不是渲染特性。
> 2. **R6-3 植被的顶点风没有地方放权重**：`Vertex` 是 64 B（pos/normal/uv/joints/weights），
>    **无顶点色、无 UV2**。W5 的结论是**植被走独立顶点布局 + 独立管线**，不改 `Vertex`。
> 3. **R6-4 地形的 splat 装不进现有 7 个纹理 set**（albedo/normal/orm/rough/metal/emissive/height
>    正好用满，`RenderTypes.h:298-313`）。W4 的结论是**独立管线 + 权重打包成一张 RGBA8**。
>
> 另：**R5-1 的 `blendDistance`（相邻探针边界混合）一直是欠账**，W6 会顺手做掉，
> 因为"室内不崩"的便宜解法（探针当辐照度源）需要同一个权重函数。

### R6-1 GPU 粒子系统 ✅ 已完成（2026-10-15）

- **目标**：compute 模拟（发射器/生命周期/速度场/碰撞）+ billboard/ribbon/mesh 渲染 + 帧图事件。
- **改动点**：
  1. `engine/fx/` 新模块：ParticleEmitter 组件（rate/burst/shape/lifetime/color-over-life/size-over-life 曲线）；
  2. compute pass：spawn/kill/simulate（环形 buffer + alive list compaction）；
  3. 渲染：billboard instanced（instancing 现成）+ soft particles（**依赖深度图采样 + R4-1**）；
  4. 编辑器：粒子面板（曲线编辑先用滑条组）。
- **验收**：无粒子组件=基线不变；喷泉/火焰 demo；帧率不塌（R0-1 断言粒子 pass 耗时）。
- **规模**：L
- **基准声明**（用户两次强调的硬约束）：**以 Unity（Shuriken）的粒子模块划分为准，
  曲线编辑形态参照 Blender 的 Color Ramp**。所以面板不是"一堆平铺的滑条"，而是
  Unity 的 12 个模块；颜色是**带停靠点的渐变色带**而不是三个颜色选择器。
- **交付**：
  - **组件**：`ecs::ParticleEmitterComponent`（`engine/ecs/Components.h:426`）—— 纯 POD，
    含 3 个内建数组（`colorRamp.stops[8]` / `bursts[4]` / 各自一堆 `vec4`）。
    **纯 POD 是硬要求**：它同时是"整块进撤销栈"（`RawBytesEditCommand`）与
    Copy/Paste Properties（`podKindOf → PK_ParticleEmitter`）的前提。
    配套枚举：`ParticleShape`(7) / `ParticleEmitFrom`(3) / `ParticleBlend`(2) /
    `ParticleRenderMode`(4) / `ParticleSortMode`(4) / `ParticleSimulationSpace`(2)。
    每个模块一个 `xxxEnabled` 且**默认等于"不产生影响"** —— 老场景加载进来逐位一致。
  - **模拟**（`assets/shaders/particle_sim.comp`，552 行）：spawn / kill / integrate 一趟做完
    （固定步长，`dtSlot = elapsedSteps × dt`，见下"速度与帧率解耦"）。随机数是
    **(槽位, 出生时刻) 的纯哈希** —— 没有 CPU/GPU 状态，这是"给定步号可复现"的根据。
  - **计数**（`particle_count.comp`）：一趟 compute 写**两条间接命令**（加色 / Alpha）+
    可选的桶排序（`ParticleSortMode::ByDistance` 等）。
    ⚠ **这个管线的 `VkPushConstantRange` 必须显式声明**（`ParticleSystem.cpp`
    的 `createPipelines()`）：漏了不仅每帧刷 `VUID-...-layout-07987`，而且
    `vkCmdPushConstants` 落到哪儿是未定义 —— 最要命的是 `cfg.y`（总槽数）取到 0
    → `instanceCount = 0` → **一颗粒子都不画**，日志里只有那几行 VUID。
  - **渲染**：`particle.vert` instanced billboard（4 种 Render Mode：Billboard /
    Stretched / Horizontal / Vertical）+ `particle.frag` 软粒子。
    两张紧凑表（加色 / Alpha）在同一个 SSBO 的两段里，用 push 常量的
    `extra.x = aliveBase` 区分 —— **x 必须逐 blend 不同**。
    软粒子按 `gl_FragCoord.xy / screen` 算屏幕 uv（**不能**用 `vUv`：那是四边形上的
    参数，会变成"每颗粒子按自己圆心附近的深度判"，画面一片硬边）。
    ⚠ `particle.frag` 里的深度换算**不能**复用 `space.glsl` 的 `vennViewDepthFromNdc`：
    深度缓冲里存的已经就是裁剪空间 ndc z（本引擎的投影没有
    `GLM_FORCE_DEPTH_ZERO_TO_ONE`），再 `×2-1` 会把 9.6 m 算成 5.2 m，
    粒子整体被判成"在场景后面"而全部丢弃（`decal.frag` 顶部记过同一个坑）。
    关软化时 `fade` 退化成 `sceneZ + 1e-4 >= vViewZ ? 1 : 0`（硬遮挡）。
  - **速度与帧率解耦**：`m_particleSimAccum += m_frameDt` → 攒够 `1/simHz` 走一步，
    一帧 0..`kMaxSimStepsPerFrame` 步；**即使 0 步也要派发**（`upload()` 每帧清计数器，
    不派发就留下"计数器全 0、间接命令还是上一帧值"的不一致状态）。
    池按 frame-in-flight 分槽，所以每槽要按 `elapsedFrames` 补步
    （`m_particleSlotFrame[slot]`）—— 不补的话每池只以 1/3 速度前进。
  - **Prewarm**：不重跑 GPU，把**模拟时钟直接拨过去**（`m_particlePrewarmSteps`）——
    固定步长下"先跑一整轮"与"把时钟拨过去"等价，而后者只要一条赋值。
  - **编辑器**（`InspectorPanel::drawParticleEmitterSection`）：Unity Shuriken 的
    **12 个模块** `Playback / Emission / Shape / Velocity over Lifetime /
    Limit Velocity / Force over Lifetime / Colour over Lifetime / Size over Lifetime /
    Rotation over Lifetime / Noise / Renderer / Soft Particles`。
    `module()` 这个 lambda 统一负责：勾选框（固有模块勾住但 `BeginDisabled`）+
    折叠标题 + **模块关掉 ⇒ 字段置灰**（`BeginDisabled`，Unity 行为）+
    `Reset to default`（回滚粒度 = 模块）+ 一键 `Expand all / Collapse all`。
    ⚠ 折叠键必须带 **`fx/` 前缀**：`sectionHeader` 的折叠状态存在 `m_sectionOpen`
    里，`INSP-SECTIONS` 日志把所有 `true` 的键打出来 —— 12 个模块裸名叫
    `playback` / `shape` … 会污染 `verify_inspector.py` 的"Crate == Ball"断言。
    ⚠ 一键展开必须用 **`ImGuiCond_Always` 强推一帧**（`m_fxForceModules`）：
    `SetNextItemOpen(cond=Appearing)` 对**已经存在**的 ImGui 节点不再生效，
    光改 `m_sectionOpen` 是点不动的。
    ⚠ `module()` 里"被搜索过滤掉"必须是**裸 `return;`**：这个 lambda 的返回类型是
    **推导**的，混进一个 `return false;` 会把返回类型推成 `bool`，而末尾没有
    return 就是 UB —— MSVC 在 Debug 下会在末尾塞一条 trap 指令，于是"打开粒子
    面板"变成整进程 `0xC000001D`（Illegal instruction），**一行日志都不给**。
  - **渐变色带**（`drawColorRampEditor`）：Unity 的"带停靠点的色带"与 Blender
    Color Ramp 的共同子集 —— 预览带（逐列 `AddRectFilled`，Reinhard + sRGB，
    停靠点画竖线）+ 每行 `ColorEdit3(HDR)` + 位置滑条 + 删除 + `Add stop`。
    **`Add stop` 插在最大空隙的中间、取该处的 `eval()` 颜色** ⇒ "加点从不改变外观"，
    只是多了个能拖的控制点（插在末尾会让新点立刻改变形状，用户以为点错了）。
    拖中**不排序**（`ramp.sort()` 只在 `commit()` 里调），预览用已排序的局部队本、
    行控件与标记用原数组（标记要跟鼠标走）。
    撤销记**整条带子**（`sizeof(ParticleColorRamp)`）：拖完会重排，按单个停靠点记
    会让"撤销"写回**别的**停靠点。
  - **搜索的两级过滤**：`sectionKeywords("Particle Emitter")` 决定整段出不出现；
    `fxModuleKeywords(key)` 决定段里哪些模块出现。
    ⚠ "兜底"的判据必须与"逐模块放行"的判据**同源**：兜底条件若只看模块**名**、
    放行条件却还看模块**关键词**，两者会打架 —— 搜 `billboard`（只在 renderer 的
    关键词表里）会得到"12 个模块全显示"，过滤等于没生效。
    ⚠ `FX-PARAMS` 的 `modules=` 只登记"**画出来的、且展开的**"模块；一个都没有时
    写的是字面量 `(none)` 而不是空串（验证脚本要归一化）。
  - **序列化**：写 `bursts` 数组 **+** `burst = bursts[0].count`；写 `colorRamp` 数组
    **+** `colorStart/Mid/End = ramp.eval(0/0.5/1)`。读则反之回落（老格式
    `burst` → 时刻 0 的一次爆发：`time=0 / cycles=1 / interval=0`；
    `colorStart/Mid/End` → t = 0 / 0.5 / 1 三个停靠点）。
    两头都写 = 老版本编辑器打开新场景仍有颜色（只是丢掉渐变），
    新版本打开老场景颜色**逐位相同**。
  - **环境变量**：`MYVK_FX` / `_CAPACITY` / `_EMITTERS` / `_HZ` / `_SIZE` /
    `_SOFTNESS` / `_INTENSITY` / `_FREEZE` / `_DBG`。前四个之一（`enabled`）接进
    `FeatureRegistry`（`fx.enabled` / `fx.sizeScale` / `fx.softness` /
    `fx.intensity` / `fx.freezeFrame`），Engine 面板 + env + 序列化四路派生。
  - **诊断**：`FX: created (enabled=…)` / `FX-DUMP`（每 30 帧一条 GPU 侧计数，
    **必须 `MYVK_FX_DBG=1` 才打**）/ **`FX-EMITTER`**（`MYVK_FX_DBG=2`，把交给 GPU 的
    发射器数据用 `%.9g` 逐值打出来）/ `FX-FREEZE` / `FX-PREWARM` /
    `FX-POOL`（池耗尽警告：`rate × lifetime > 每发射器容量`）/ `FX: … emitters dropped`。
    `GPU-TIMING` 新增 `particles=` 段。
    ⚠ **`FX-EMITTER` 不是"更啰嗦的日志"，它是唯一能精确验证"旧格式与新格式等价"的手段**
      —— 见下面"已知不确实性"：像素比较做不到。
  - **验证**：`tools/verify_particles.py`（A~G 共 38 条断言，`check_feature_surface.py`
    登记 `id="fx"`，`run_regression.py` 的 `TRIGGERS` / `RENDER_MODULES` 已接入）。
- **已知不确实性（重要，不是 bug，但也关不掉）**：
  冻结帧能保证**状态**逐位可复现，**不能**保证**像素**逐像素一致。实测
  （`MYVK_FX_FREEZE=120`，各配置跑 3~4 次取最大两两差）：

  | 配置 | alive | 像素平均差 |
  |---|---|---|
  | 1 颗、静止、无重力 | 1 | 0.0082 |
  | 4 颗、静止、无重力 | 4 | 0.0029（n>18 = 0） |
  | 4 颗、只受重力 | 4 | 0.0548 |
  | 364 颗羽流 + 软粒子 | 364 | 1.59 ~ 2.13 |
  | 364 颗羽流 + 软粒子关掉 | 364 | 0.058 |

  两条来源：① **池按 frame-in-flight 分槽**，每槽只在自己被派发的那一刻推进，
  冻结后各槽停在**各自的最后一步**上（差 1~2 步）→ 恰好落在边界上的那颗粒子
  生或死不同（这条解释了为什么差异**不随速度放大**：10 倍初速反而更小 ——
  差的是**集合**不是位移）；② **紧凑表顺序来自 `atomicAdd` 的完成顺序**，
  加色混合数学上无序、但 8 位 UNORM 下每个片元的 `+=` 都要量化一次，
  差值随**重叠的部分覆盖率像素**增长（软粒子打开时 `fade` 连续 ⇒ 到处都在贡献
  一点点 ⇒ 27 倍差）。
  ⇒ 所以验收口径是"**状态逐位一致**（B1）+ **像素差有界**（B7 ≤ 4.0）",
  不主张逐像素一致。
- **未做（明确记下）**：
  ① **ribbon / mesh 渲染模式**（roadmap 原文要求）—— 只做了 4 种 billboard；
  ② **粒子碰撞**（`Collides with` 平面/世界）—— 完全没做，只有软粒子的深度遮挡；
  ③ **曲线编辑仍是 4 点滑条组**（roadmap 允许："曲线编辑先用滑条组"）；
  ④ **帧图事件**（`Bursts` 之外的帧事件回调）没做；
  ⑤ ⚠ **`duration = 0`（无限循环）时 Burst 完全不发射** —— Unity / Blender 此时
     应该发一次。根因在 `particle_sim.comp` 的发射窗口判据
     `(ev > tCyclePrev && ev <= tCycle) || (cycleRestart && ev <= tCycle)`：
     `duration = 0` 时 `tCycle = t = 6.0 s`，`ev = 0.0` 永远不在 `(0, 6.0]` 里。
     **是一条待确认的产品语义问题**，验证脚本目前绕开它（不依赖 burst 发射）。

### R6-2 水体（第一版：平面水面）

- **改动点**：Gerstner 波（法线位移）+ 菲涅尔 + 折射（采样不透明 HDR）+ 深度衰减颜色混合 + 简单焦散（噪声贴图滚动）；反射先接 SSR。
- **规模**：M

#### 2026-10-21 第七轮：可被 W/E/R 操作 + 浪况合成单一参数

用户两条要求：
1. "水体创建后必须是在 3D 视口中可以移动的，可以通过 W/E/R 控制位置、旋转、缩放。"
2. "浪花波纹要通过一个浮点参数控制，0 = 无任何波纹的平静水面，10 = 汹涌的浪涛。"

**① W/E/R 手柄**
- 病灶：水体**没有网格组件**，而 gizmo 只认 `TransformComponent`；`WaterPass`
  建模用 `body.center / level / rotation / halfExtent`，完全忽略实体变换 →
  "拖了手柄水面不动"。
- 做法（`editor/WaterEditor.cpp`）：把 Transform 当**手柄载体**，每帧成对同步：
  `syncTransformFromBody`（body→Transform，手柄站到水面上）→ `gizmo.update`
  → `applyTransformToBody`（Transform→body）。调用点成对夹住 gizmo，
  只做一半会互相打架。
- ⚠ **缩放必须乘"比值"**：gizmo 写的是**绝对**倍率（`m_startScale*factor`），
  每帧直接乘会把累计量反复叠进去 —— 实测一次拖拽把半径从 18 干到 2.4e9。
  改成 `halfExtent *= 本帧倍率 / 上帧倍率`（`m_scaleApplied`），并夹到
  [0.5, 2000] m 兜底。
- 另：`PickingSystem::pick` 补了**水面拾取**（射线 vs 水平圆/矩形），否则只能
  在层级里选中它。水体没有网格 → 不在 BVH 里，所以放在 PICK-DBG 自检**之后**。

**② 浪况 0..10**
- `WaterBody::waves`（0..10）成为唯一输入；振幅/波长/细波/泡沫全部由
  `deriveWaves()` 派生（**只有这一份映射**，`WaterPass` 与面板都调它）：
  `amp = 0.60·t^1.35`、`λ = 3.2+16.8t`、`ripple = 1.70t`、`foam = 0.95·t^2.2`，
  `t = waves/10`。**`t == 0` 直接返回全零**（不走 pow，避免 1e-9 残留让法线抖）。
- 旧的 `waveHeight / waveLength / ripple / foam` 降级为 legacy：仍读写 JSON
  （兼容），但**不参与渲染**。老场景（无 `waves`）由 `waveHeight` 反推出等效
  浪况，保证"打开旧场景水面还是原样"。
- 面板：4 个会互相打架的滑块合成 1 个 `Waves`，且必须放**首屏** —— 面板是
  滚动子窗口、比窗口还高，放在水位后面时滑块落在 y=759 而客户区只有 753。
- **验收**：`tools/verify_water_motion.py`（7 组）：
  · move/rotate/scale 三组真的拖 `GZ-AXIS-X` / `GZ-RING-Z`，断言 `WATER-XFORM`
    里 center / rot / halfExtent 变了且没串到别的分量；
  · 水面**高频能量** 0→4→10 单调（0.15 → 0.50 → 0.92），waves=0 是镜面
    （< waves=10 的 1/3），并有 waves=0 的**控制组**（两次差 < 0.05）；
  · 面板滑块真的点得动（0 → 10）；
  · 老场景（只有 waveHeight=0.55）迁移后能量 0.88 > waves=4 的 0.50。
- ⚠ **NO_HUD 会连带关掉手柄**（实测 GZ-ORIGIN 一条都不打）→ 拖手柄的用例
  不能设 `MYVK_EDITOR_NO_HUD`。
- ⚠ `logRect` 给的是**客户区**坐标，`SetCursorPos` 要**屏幕**坐标 →
  必须用 `verify_asset_drag.move_to(hwnd,…)` 换算，否则整段拖拽平移、抓不到手柄。

#### 2026-10-21 第八轮：Water 模式内也能用 W/E/R

用户："水体不是用鼠标去拖动大小的，而是 W 拖 X/Y/Z 轴位置、R 缩放尺寸。"

- **病灶**：`ViewportPanel` 把 `EditMode::Water` 算进 `brushMode` →
  `modeToolActive` 为真 → 变换手柄**整个被关掉**。水体在那套逻辑里只能靠
  鼠标拖拽定尺寸，正是用户不想用的方式。
- **修法**：Water 移出 `brushMode`，改成按"有没有选中"分工：
  · 选中水体 → 手柄接管（W 平移 / E 旋转 / R 缩放）；
  · 没选中     → 左键拖拽才创建新水体。
  `WaterEditor::viewportInput` 在 `hasSelection()` 时直接返回（只画选中轮廓），
  防止"在手柄上按下的一瞬间又新建一块水"。
- 缩放仍走 `applyTransformToBody` 的**比值**累乘（见上一轮），一次拖拽 = ×2。
- **验收**：`verify_water_motion.py` 增加一组 `editmode=water` 的 scale 拖拽，
  断言 `halfExtent 18 → 36`（其余断言同前）。

### R6-3 植被风（顶点风动画）

- **改动点**：全局风参数（方向/强度/gust 噪声）→ 顶点着色器按权重（顶点色 R 或贴图）摆动；instanced 草地（instancing 现成）。
- **规模**：S~M

### R6-4 地形（heightmap + splat 材质混合）

- **规模**：L（可无限后置）

### R6-5 动态天空（Hillaire 大气散射）

- **目标**：昼夜循环；太阳高度驱动天空颜色 + **IBL 重烘**（烘焙管线现成）+ 雾颜色联动。
- **改动点**：大气 LUT（transmittance/multi-scatter/sky-view 三张）compute 重烘（频率降档）→ `sky.frag` 采样；CSM 光方向随太阳；探针/IBL 脏标记重烘。
- **规模**：L

---

## 阶段 7 · 架构演进（在前面的效果稳定后再动）

### R7-1 声明式 RenderGraph

- **目标**：kPasses 手写数组 + 手写屏障 → 资源声明（读/写/深度）自动推导屏障、资源别名、按需裁剪 pass。
- **规模**：L

### R7-2 细粒度同步 + 异步 compute

- **目标**：重建路径 `waitIdle` 全局停机 → timeline semaphore；剔除/SSAO compute 与图形队列重叠。
- **规模**：L

### R7-3 （更远）ray tracing 后端

- **前提**：R2-3 bindless（RT 的资源绑定模型）。
- **规模**：XL

---

## 阶段 8 · 编辑器与 UI（**贯穿所有阶段**，不是"最后再做"）

> 完整架构见 **`docs/editor_architecture.md`**（为什么这么设计）；
> 可执行任务清单见 **`docs/editor_roadmap.md`**（E01~E29，按顺序做哪一件）。
> **进度**
> · **E01 特性注册表**已完成（2026-10-14）—— 28 组 / 103 参数 / 0 未接线，
>   自检 `check_feature_surface.py` 裸 env 漏网 = 0，39 例回归全通过。
> · **阶段 B 布局范式（E04~E08）**主体完成（2026-10-14）—— 面板表驱动 / 六套工作区 /
>   分栏增强 / 多视口 / 菜单与工具栏重构。E07、E08 各留少量按计划后延的条目
>   （逐条记账见 `docs/editor_roadmap.md` §3 落地记录）。
> · **E02 UI 生成层 / E03 设置层级与落盘**（阶段 A 剩余）**均已完成** ——
>   E02 `editor/FeatureUIGen.*` 2026-10-14、E03 `engine/render/RenderSettingsIO.*`
>   （`SettingsLayer` 就在这个头里）+ 逐 `ParamDesc` 落盘 2026-10-17。**阶段 A 地基至此全绿**
>   （E01 ✅ / E02 ✅ / E03 ✅）。~~原记录说"跳过未做、这两个文件都不存在"——
>   那是 10-14 的口径，已被各自系列做掉。~~
> · **E26 画质档位**已完成（2026-10-19）：三档数据（22 个 enable + 3 个数值项）
>   收在 `render/QualityPresets.*`，编辑器面板 `editor/QualitySettingsPanel.*`、
>   启动器 `ProjectDisplay::writeQuality` 共用 `fillLayerForPreset` 一个落盘入口。
> · 下一步（编辑器轨）：布局向 **E14 视口面板重构** / **E20 通知与进度** /
>   **E22~E24**；面板可视化受 **R7-1 RenderGraph** 阻塞，烘焙受 **R5-2 lightmap** 阻塞。
>   逐条状态见 `docs/editor_roadmap.md` §6 状态表。
> 这里只放路线图层面的约束与排期。

### 定位

引擎的**可操作性**本身就是 3A 引擎的核心竞争力。一个只能靠 `MYVK_*` 环境变量
打开的效果，**在编辑器里点不到 = 美术用不上 = 不算交付**。

**实测（2026-10-19，`tools/check_feature_surface.py` → PASS）**：

| 指标 | 实测 |
|---|---|
| 特性总数 / 有 UI 面 | 35 / **35（100%）** |
| 无 UI 面的特性 | **0** ← 已清零（2026-10-14 还是 8 项：SSAO · SSGI · SSR · God rays · SSS · 阴影级联硬化 · 解析式高度雾 · Instancing） |
| `MYVK_*` 环境变量 | 263（141 已登记 / 123 白名单 / **0 裸 env**） |
| 注册表已接管 | 109 / 141（77%），剩 32 个只有 env → E10 / E17 的输入 |
| 软警告 | 21 条 `*_DBG` 诊断项未归到 Debug 档（不 FAIL，属 E17 收尾） |

> ⚠ **粒度提醒**：这是**特性级**检查（"这个特性在编辑器里有没有入口"）。
> **"有 UI 面" ≠ "每个参数都有 UI"** —— 例如天空盒有开关，但天顶色 / 分辨率可能仍只有 env。
> 参数级覆盖要等 E02 生成层落地后按 `ParamDesc` 逐项校验（`docs/editor_architecture.md` 9.2 节）。

### 硬性规则（**准入**）

任何新特性（R5-x / R6-x / R7-x …）交付时，**五件套缺一不可**：

1. **实现** —— 引擎 / 着色器
2. **登记** —— `FeatureRegistry`（`FeatureDesc` + `ParamDesc`）
3. **UI 面** —— 至少 Basic 档有一个入口落在某个面板
4. **序列化** —— 按 `Scope` 落到 项目 / 场景 / 对象 / 相机
5. **回归用例 + 触发映射** —— `verify_*.py` + `run_regression.py::DEFAULT` + `TRIGGERS`

**把关**：`tools/check_feature_surface.py`（秒级、不跑 GUI、可进 CI）。
**缺 UI 面 → 直接 FAIL，不计入完成度。**

### 排期（三期 → 已细化为 28 个任务，见 `docs/editor_roadmap.md`）

| 期 | 内容 | 验收 |
|---|---|---|
| **阶段 A 地基** | `FeatureRegistry`(SSOT) ✅ · UI 生成层 ✅ · 设置层级与落盘 ✅ | `check_feature_surface.py` C/D 组**全绿**（D 组已达标：裸 env 漏网 = 0） |
| **阶段 B 布局范式** | 面板注册表 ✅ · 六套工作区 ✅ · 分栏增强 ✅ · 多视口 🟡 · 菜单/工具栏 🟡 | 工作区可切、布局可落盘 ✅（2026-10-14） |
| **阶段 C 核心面板** | Engine 重构 · 补齐无 UI 特性 · Inspector · Hierarchy · ContentBrowser · Viewport · Stats · Console · Preferences | 每个面板有独立 `verify_*.py` 且 PASS |
| **阶段 D 交互理念** | 命令注册表+面板 · 设置可撤销 · 覆盖指示器 · 通知/进度 · Gizmo/选择 · 快捷键表 | 能不看文档完成"开 SSR、调强度、存成项目预设" |
| **阶段 E 生产力** | 资产编辑器框架 · GPU 时间轴 · 画质档位 · RenderGraph 可视化 · 打包发布 · 烘焙流水线 | 与 R7-1 / R5-2 / R6-1 **同步交付**各自工具 |


### 与各 R 项的耦合（**必须成对交付**）

| 路线图项 | 附带必需的 UI 面 |
|---|---|
| R5-1 反射探针 ✅ | 属性面板 ✅（`Inspector ▸ Reflection Probe`：enabled / extents / intensity / boxProjection / blendDistance + Capture，带撤销）· 探针放置 gizmo ✗ · 预览球 ✗ · 独立烘焙按钮 ✗（仅面板内 Capture） |
| R5-2 lightmap 烘焙 | 烘焙设置面板 + 进度条 + 贴图预览 |
| R6-1 GPU 粒子 ✅(2026-10-15) | 粒子编辑器 ✅ 12 个 Unity 模块 + 带停靠点色带 + 一键展开折叠 + 两级搜索；**仍缺**视口内预览窗口 / 贝塞尔曲线 / ribbon·mesh 渲染模式 |
| R6-4 地形 | **地形雕刻工具**（笔刷 / 高度图导入 / splat 图层） |
| R7-1 RenderGraph | **图可视化面板**（pass 节点 + 资源边 + 时戳叠加） |

---

## 推荐执行顺序（把"性价比/依赖"折算后的直线序）

```
R0-1 GPU时戳 ──► R0-2 视图模式 ──► R1-1 TAA ──► R4-1 AlphaMask ✅(小,穿插) ──► R3-1 阴影硬化 ✅
      │                                  │
      ▼                                  ▼
  R2-1 Hi-Z ──► R1-2 自动曝光 ──► R1-3 LUT ──► R3-2 体积雾 ✅ ──► R4-2 SSS ✅ ──► R4-3 POM ✅ ──► R4-4 延迟贴花 ✅ ──► R5-1 反射探针 ✅
                                                  R3-3 光源修饰 ✅（区域光/IES/light function）
      │
      ▼
  R2-2 indirect ──► R2-3 bindless ──► (R7-x 架构层)
                              │
                              ▼
                    R6-1 GPU粒子 ✅（2026-10-15 完成；下一步 R6-2 水体）

── 并行的编辑器轨（阶段 8，可执行清单见 docs/editor_roadmap.md）──
  阶段 A 地基 ──► 阶段 B 布局 ──► 阶段 C 核心面板 ──► 阶段 D 交互 ──► 阶段 E 生产力
   ✅E01 ──► ✅E04~E08（阶段 B 主体）──► ✅E02 生成层（E09/E10 已有东西可挂）
   ✅ **R5-1 已完成（2026-10-15）**：FeatureRegistry 落地的红利兑现了 —— 反射探针
     直接在注册表里登记参数，一交付就自带 UI 面（`Inspector ▸ Reflection Probe`），
     **没有**产生"只有 env"的欠账。这也是本项目第一次在新特性上走完"五件套"。
   ✅ **E02 已于 2026-10-14 落地**：`editor/FeatureUIGen.*` 在，往 `kFeatures[]` 加一行
     `ParamDesc` 就会自动长出控件（`FeatureUIGen.cpp` 按 `ParamKind` 派发）。E09/E10
     已不必手写控件。剩下的是**参数级覆盖**（32 个仍只有 env 的项），归 E10/E17。
   ✅ **E26 画质档位（2026-10-19）**：档位 = 22 个 enable + 3 个数值项，编辑器面板
     `Quality Settings` 与启动器共用 `render::fillLayerForPreset` 一个落盘入口。
```

**如果一次只挑一个开工**：`R0-1 GPU 时戳 + R0-2 视图模式`（组合半天到一天，为全部后续任务提供观测与调试工具）；然后 `R1-1 TAA`（时域地基，运动矢量已现成）。
**编辑器侧**：注册表（E01 ✅）与布局范式（E04~E08 ✅/🟡）已经做完 —— 面板、工作区、
分栏、多视口、菜单/工具栏这一层不再是"手写一遍"的活。**E02 生成层也已完成（2026-10-14）**，
`ParamDesc` 能自动变成控件；E03 落盘（2026-10-17）与 E26 画质档位（2026-10-19）随后补齐，
**阶段 A 地基全绿**。真实缺口在**参数级覆盖**（32 个仍只有 env 的项 → E10/E17）与
**面板级欠账**（E20 通知 / E22~E24 / E07 后延四项）。
（E07 后延的四项 —— 子视口性能降级 / 独立视图模式与 gizmo / 点击激活 / 视口矩形数组 ——
都落在 **E14 视口面板重构**上，顺序上跟着 E14 一起做最省。）
