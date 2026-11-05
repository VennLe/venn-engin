# 地面镜面反射 & 夜晚天色（2026-11-05）

两条美术反馈的定位与修复记录。两处都是**观感问题**，不是崩溃/错误 —— 所以
每一条都要写清楚"改的是哪个量、为什么原来会那样"，否则下次调参又会调回去。

---

## 一、「地面的镜面反射太强」

### 现象

第三人称小镇场景，相机贴地俯视时，地面（草皮 / 砖格 / 泥地）上出现明显的
**树冠与屋檐倒影**，远处地面还会泛出一条发白的水面状亮带 —— 放大截图后非常
刺眼（`_venn_verify/g4.png` 那一类构图）。

### 三条根因（叠在一起，各自都能单独让地面"像镜子"）

| # | 位置 | 原来的写法 | 为什么错 |
|---|---|---|---|
| 1 | `assets/shaders/fg_ssr.frag` | `fres = 0.04 + 0.96·(1-NdotV)^5` | 这是**光滑电介质**的菲涅尔。SSR 是全屏 pass，拿不到逐像素材质，于是对粗糙度 0.92 的草地用了光滑地面的曲线。15 m 外的地面 `NdotV ≈ 0.1` → `(0.9)^5 ≈ 0.59` → 反射率 0.6，再乘强度 0.6 → **地面上叠 36% 的倒影**。 |
| 2 | `assets/shaders/terrain.frag` | `F = F0 + (1-F0)·(1-VdotH)^5` | 上限硬编码 1.0。地形是连续的水平曲面，越远越掠射 → 反射率被推到 1.0，几十米外的草皮整片泛白，像结冰的湖。 |
| 3 | `terrain.frag` / `pbr.frag` | `specular = ... / (4·NdotV·NdotL)` | 掠射时 `NdotV → 0`，分母趋于 0，高光发散 → 地平线附近一条刺眼的白色亮带（grazing firefly）。 |

### 修复

1. **SSR 的菲涅尔按粗糙度收敛**（`fg_ssr.frag`）：
   ```glsl
   const float fresMax = max(1.0 - reflRough, 0.04);
   const float fres    = 0.04 + (fresMax - 0.04) * pow(1.0 - ndotv, 5.0);
   ```
   `reflRough` 来自新增的 push 常量 `pc.surface.x`，对应面板参数
   **`ssr.roughness`（Surface roughness，默认 0.72）**。默认档下掠射上限
   0.28，乘强度 0.6 后≈ 0.17 —— 只剩一圈很淡的氛围反光，不是倒影。

2. **地形菲涅尔与 pbr 统一**（`terrain.frag`）：上限改成
   `max(vec3(1 - roughness), F0)`，与 `common.glsl::vennFresnelSchlickRoughness`
   同一条公式。草皮（roughness 0.96）最多 4% 反射。

3. **掠射软钳**（`terrain.frag` 与 `pbr.frag`）：直接光高光的分母用
   `max(NdotV, 0.08)`，把 `1/NdotV` 的增长封住。只影响高光，不影响 IBL 的
   真实 NdotV。

### 怎么调

引擎面板 ▸ Scene ▸ Reflections ▸ **Surface roughness**（`ssr.roughness`）
或环境变量 `MYVK_SSR_ROUGHNESS`：

- `0.72`（默认）= 地面是哑光的，只留一点点天空反光；
- `0.1 ~ 0.2` = 湿地 / 抛光地砖，接近镜面；
- `1.0` = SSR 的菲涅尔退化成常量 0.04，几乎不反射。

⚠ 这是一个**全局假定**，不是逐材质的粗糙度。真正的逐像素材质粗糙度要等
G-buffer 加一条通道（现在 SSR 只吃深度）。在拿到之前，宁可"全局偏哑"。

### 判据

- 构图：相机 `1.6 m` 高、俯角 15°、15~25 m 外的地面；
- 采样：地面上的**树冠倒影区域**，开/关 SSR 的显著变化像素占比
  （`verify_screenspace` 组 5 已经在测"开关差异显著"，不测具体数值）；
- 目视：地面网格之间不再出现成块的绿色树影。

---

## 二、「夜晚天空太蓝、夜色太亮」

### 现象

天气预设点「星空夜」后：天顶是一片**饱和的宝蓝**，靠近地平线更是亮到像傍晚，
月亮周围一大片天被"洗白"，整体不像夜。

### 根因

1. **天顶色太饱和也太亮**：`zenith = (0.012, 0.022, 0.070)` 线性域 **B/R ≈ 5.8**，
   经 ACES 近似后上屏约 `RGB(23, 33, 83)` —— 标准的"蓝调摄影"而不是夜。
2. **地平线亮度偏高**：`(0.055, 0.080, 0.145)` 上屏约 `(66, 81, 120)`，比天顶亮
   两档多，地平线一圈发灰白。
3. **云色比天还亮很多**：`cloudColor = (0.16, 0.18, 0.26)`，夜里像贴了几块亮纸。
4. **月亮光晕铺得太开**：`sunIntensity 0.55` + `horizonGlow 0.55`。
   ⚠ 关键一点：`sky_model.glsl` 里**太阳/月亮项是在曝光之后相加的**
   （`col *= zenith.a` 之后再 `col += sunTint * sunAmt * ...`），
   所以 `Exposure` 滑条**压不暗月亮和它的光晕**，只能改 `sunIntensity` /
   `horizonGlow`。这一点以前没有写下来，调参时很容易误判。

### 修复（`engine/render/SkyParams.h::night()`）

| 字段 | 旧值 | 新值 | 说明 |
|---|---|---|---|
| `zenith` | 0.012 / 0.022 / 0.070 | **0.010 / 0.013 / 0.028** | B/R 5.8 → 2.8 |
| `horizon` | 0.055 / 0.080 / 0.145 | **0.024 / 0.030 / 0.048** | 亮度约砍到 1/2.6 |
| `ground` | 0.012 / 0.014 / 0.020 | **0.007 / 0.008 / 0.011** | 仰视时地平线以下更沉 |
| `exposure` | 1.00 | **0.90** | 只压底色渐变（不影响月亮/云/星） |
| `cloudColor` | 0.16 / 0.18 / 0.26 | **0.075 / 0.085 / 0.110** | 夜里云是被月光染的灰 |
| `cloudThickness` | 0.70 | **0.66** | 顺带薄一点 |
| `sunIntensity` | 0.55 | **0.42** | 月亮盘 + 光晕的整体强度 |
| `horizonGlow` | 0.55 | **0.22** | 光晕铺开的范围 |
| `starAmount` / `starBrightness` | 0.85 / 1.0 | 不变 | 天暗下去后星星对比反而更好 |

按 ACES 近似估算上屏值（含 exposure）：

- 天顶：(23, 33, 83) → **(19, 22, 38)**
- 地平线：(66, 81, 120) → **(34, 40, 56)**

### 注意（不回溯老工程）

预设是**按钮**，不是启动时的强制值。已经存过盘的老场景（`environment` 段里
写着旧数值）不会自动变 —— 打开「天空与大气」面板点一次 **Starry night** 即可。
`MYVK_SKY_PRESET=night` 启动的临时场景直接就是新值。

---

## 三、涉及文件

- `assets/shaders/fg_ssr.frag` —— 菲涅尔按粗糙度收敛（`pc.surface.x`）
- `assets/shaders/terrain.frag` —— 菲涅尔上限收敛 + 掠射软钳
- `assets/shaders/pbr.frag` —— 掠射软钳（直接光高光分母）
- `engine/render/ScreenSpaceSettings.h` —— `ssrSurfaceRoughness = 0.72`
- `engine/render/FrameGraph.cpp` —— `SsrPush.surface`（112 → 128 字节）
- `engine/render/FeatureRegistry.cpp` / `Renderer.cpp` —— `ssr.roughness` 参数与绑定
- `engine/render/SkyParams.h` —— `night()` 全部重调
- `engine/i18n/ZhTable.cpp` —— `Surface roughness`
- `tools/check_feature_surface.py` —— `MYVK_SSR_ROUGHNESS` 登记

## 四、没做的事（明确留白）

- **逐像素粗糙度进 SSR**：需要 G-buffer 加通道（HDR 目标的 alpha 被
  alpha-to-coverage 占着），影响面远超本次观感调整。
- **地形的 specular 走完整 split-sum**：现在仍是简化 GGX（没有逐层粗糙度贴图），
  本次只修了菲涅尔上限与掠射奇点。
- **夜晚的自动曝光**：`autoExposure` 默认关。若以后打开，它会把这套压暗后的
  夜空又拉亮回来 —— 那时要在 `PostProcess` 里做"按环境亮度分档"的 key 调整。
- **白天的整体发灰**（天空亮部 bloom 洗了一遍画面）：不在本次范围内，
  没有动 `post.bloomStrength` / `bloomThreshold`。

---

## 五、实测（同机、同场景、同机位）

场景 `anime_town.json`，`MYVK_EDITOR_NO_GRID=1` 关掉编辑器网格（否则白线
会污染像素判据），窗口 1600×900。数值是视口内地面区域的平均值。

| 情形 | 地面 lum | 地面 B/G | 天空 lum | 天空 B/G |
|---|---|---|---|---|
| 改前（旧构建，用户机位，夜） | 58.4 | **1.36** | 50.6 | **2.04** |
| 改后（场景机位，夜） | **27.9** | **1.04** | **24.2** | **1.37** |

⚠ 两次机位不同（用户那台是自由的，我这边只能取场景自带机位），所以这一行
**只作方向性对照**，不是逐像素回归基线。可判读的是两件事：地面不再被蓝天
染蓝（B/G 1.36 → 1.04 ≈ 回归中性），整体亮度砍半。

**根因归属（实测，不是推测）**：把 SSR 关掉，地面亮度只变 2~3%；把 IBL 关掉，
地面 lum 从 28 跳到 **153**。所以夜里的"湿地面"主要是**环境镜面（IBL
specular）**在反射夜空，不是 SSR。这带来一个必须记住的连锁关系：

> `setSkyParams()` → `m_skyDirty` → `tickSkyRebuild()` →
> `Environment::rebuild()` **会把 irradiance / prefilter 一起重烘**。
> 也就是说"改天空颜色"会连带改掉所有物体的环境光与镜面 ——
> 这既是修好地面泛蓝的原因，也是**改夜空会顺带改地面/墙面亮度**的原因。
> 反例提醒：`sky.ibl=false` 走的是旧近似路径 `envColor(N)`，那条路径
> 用的是**写死的**解析天际色，不跟天空预设 —— 夜里它会亮到 lum 153。
> 排查观感问题时不要拿它当"IBL 关掉"的参照。

**SSR 那两处改动怎么验证的**：`MYVK_SSR=1` + `MYVK_SSR_ROUGHNESS` 取
`0`（= 旧公式，因为 `fresMax=1` 时与原式逐字等价）与默认 `0.72` 各跑一帧，
同一机位比显著变化像素。地面区域 1.7%，方向正确但量不大 —— 所以
**不要把"地面像镜子"全记在 SSR 账上**（见上一条）。

