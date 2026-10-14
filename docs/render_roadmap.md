# Venn 引擎渲染优化任务清单

> 生成于 2026-10-13，基于"对照成熟引擎（UE5 / Godot 4 量级）的差距评估"。  
> 现状基线：MSAA+HDR(R16G16B16A16)、Bloom/FXAA/ACES、CSM 4级@2048、点/聚光局部阴影、  
> 分簇光照、IBL(irradiance+GGX prefilter)、静态天空立方体、解析式高度雾、SSAO、  
> FrameGraph(SSGI/SSR/GodRays/DoF/MotionBlur)、色差/暗角/后处理体积、shader 变体+热重载、  
> GPU instancing、视锥剔除/BVH/LOD、蒙皮(SSBO 关节矩阵)、纹理四件套(mipmap/各向异性/BC/流式)、  
> 多相机/通用 RenderTarget、AlphaMode{Opaque,Blend}、RenderStats。

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
  **球谐/辐照度探针 + 逐方向查表**，或上 R5-1 反射探针时一起做。
  ② 路线图原文里"可选 2 pass 模糊"没做 —— 预积分 LUT 已把交界线柔化，屏幕空间再糊一遍
  的收益低于成本（且会破坏"Default 逐像素不变"的对齐口径）。
  ③ **默认刻度 8 对细几何是"已饱和"的**：`curv = fwidth(几何法线) × 8`，一个 ~200px 的球
  其 `fwidth` 只有 ~0.013（curv≈0.10），而鼻梁那种十几像素宽的特征 `fwidth` 可到 ~0.2
  → curv 被 clamp 到 1（= 最强散射行）。也就是说默认值在脸上基本是"拉满"的，
  想让皮肤含蓄一点就把 `MYVK_SSS` 调到 3~5；想让它成为可调的艺术参数，应该把刻度
  从"全局常量"升级成**材质字段**（`Material::sssCurvatureScale`）。

### R4-3 POM 视差遮挡贴图

- **改动点**：材质加 heightMap 槽 + 高度尺度；`pbr.frag` relief mapping（步进 ray-march）；LOD 距离渐出（远处退回 normal map）。
- **验收**：砖墙强视角下纹理凹凸视差正确；heightScale=0 时逐像素不变。
- **规模**：M

### R4-4 延迟贴花（DBuffer）

- **改动点**：贴花体积（AABB + 朝向）→ prepass 后 DBuffer pass（法线/反照率/粗糙度三层）→ 前向里混合；先做平面贴花（贴地标记/弹孔）。
- **规模**：M

---

## 阶段 5 · 反射与 GI

### R5-1 反射探针

- **目标**：室内反射（SSR 只反射屏幕内的东西）。
- **改动点**：探针组件（AABB + capture 按钮/自动频率）→ 复用通用 RenderTarget API（**本轮刚做好**：`createRenderTarget()` + 立方体 6 面渲染→`CubeMap` 转换）→ 粗糙度混合（mip= roughness，**GGX prefilter 烘焙管线现成**）→ box projection 修正 + SSR 按深度/roughness 权重混合。
- **验收**：镜面球在室内（背对窗口）反射出探针内容；探针关=基线不变。
- **规模**：L

### R5-2 （远期）lightmap / 光照探针 烘焙

- **目标**：静态场景 GI 不再依赖有噪的 SSGI。
- **改动点**：CPU 路径追踪器（借用 ray query？Vulkan 现在没开 ray tracing——先用 CPU MC 或 irradiance 点云），烘 irradiance probe SH 网格（比 lightmap 便宜，先做探针）。
- **规模**：L（暂缓）

### R5-3 （远期）DDGI 动态探针 GI

- **规模**：L（依赖 R5-2 的探针基础设施）

---

## 阶段 6 · 内容生态（差距最大，按游戏内容需要挑）

### R6-1 GPU 粒子系统 ⭐ 最大缺失

- **目标**：compute 模拟（发射器/生命周期/速度场/碰撞）+ billboard/ribbon/mesh 渲染 + 帧图事件。
- **改动点**：
  1. `engine/fx/` 新模块：ParticleEmitter 组件（rate/burst/shape/lifetime/color-over-life/size-over-life 曲线）；
  2. compute pass：spawn/kill/simulate（环形 buffer + alive list compaction）；
  3. 渲染：billboard instanced（instancing 现成）+ soft particles（**依赖深度图采样 + R4-1**）；
  4. 编辑器：粒子面板（曲线编辑先用滑条组）。
- **验收**：无粒子组件=基线不变；喷泉/火焰 demo；帧率不塌（R0-1 断言粒子 pass 耗时）。
- **规模**：L

### R6-2 水体（第一版：平面水面）

- **改动点**：Gerstner 波（法线位移）+ 菲涅尔 + 折射（采样不透明 HDR）+ 深度衰减颜色混合 + 简单焦散（噪声贴图滚动）；反射先接 SSR。
- **规模**：M

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

## 推荐执行顺序（把"性价比/依赖"折算后的直线序）

```
R0-1 GPU时戳 ──► R0-2 视图模式 ──► R1-1 TAA ──► R4-1 AlphaMask ✅(小,穿插) ──► R3-1 阴影硬化 ✅
      │                                  │
      ▼                                  ▼
  R2-1 Hi-Z ──► R1-2 自动曝光 ──► R1-3 LUT ──► R3-2 体积雾 ✅ ──► R4-2 SSS ✅ ──► R5-1 反射探针
                                                  R3-3 光源修饰 ✅（区域光/IES/light function）
      │
      ▼
  R2-2 indirect ──► R2-3 bindless ──► (R7-x 架构层)
                              │
                              ▼
                    R6-1 GPU粒子（随时可插，游戏内容需要时优先级提到最前）
```

**如果一次只挑一个开工**：`R0-1 GPU 时戳 + R0-2 视图模式`（组合半天到一天，为全部后续任务提供观测与调试工具）；然后 `R1-1 TAA`（时域地基，运动矢量已现成）。
