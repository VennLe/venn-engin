# 编辑器 / UI 架构设计（Editor & UI Architecture）

> **目标定位**：做出一个**成熟、好用、高效**的 3A 级游戏开发引擎。
>
> **第一约束**（本文档存在的理由）：**任何引擎功能，都必须有与之匹配的编辑器操作界面。**
> 一个只能靠环境变量打开的效果，等于"引擎里有、但美术用不到"——它不计入功能交付。
>
> **适用范围**：`editor/` 全部、`engine/render` 的设置类结构体、以及**新增功能的准入规则**。
> 后续每新增一个渲染/引擎特性（R5-x / R6-x / R7-x …），必须同时交付它在本文档
> 第 9 节定义的"五件套"，否则视为未完成。

---

## 0 · 现状诊断（实测，2026-10-14）

> ⚠⚠ **本节是"动手前"的问题快照，不是现状**。写它的时候 E01 还没做，所以下面那些
> "完全无 UI ❌ 缺失"是**当时的结论**，用来回答"为什么要做 E01/E02/E03"。
> **当前真实状态**（2026-10-19 复跑 `tools/check_feature_surface.py`）：
> **35 / 35 个特性全部有 UI 面，无 UI 面 = 0**；`MYVK_*` 263 个（141 已登记 /
> 123 白名单 / **裸 env 漏网 0**）；注册表已接管 109 / 141（77%）。
> 剩余缺口是**参数级**（32 个仍只有 env → E10/E17），不是"点不到"。
> 逐项进度看 `docs/editor_roadmap.md` §6 状态表。

设计不能凭空来。先把"现在到底缺什么"量化清楚。

### 0.1 硬数据

| 指标 | 实测值 | 说明 |
|---|---|---|
| `MYVK_*` 环境变量总数 | **158** | `engine/` + `editor/` + `assets/` + `tools/` 去重后 |
| 已有编辑面板 | 9 个 | Hierarchy / Inspector / Content / Viewport / Scripts / Stats / Engine / Preferences / CharacterWindow |
| 编辑器代码规模 | 38 文件 / ~15.3k 行 | `EditorApp.cpp` 单文件 2.4k 行、111 KB |
| **"功能↔UI" 的注册机制** | **不存在** | 全仓 grep `manifest` / `FeatureRegistry` → 无 |

### 0.2 特性 → UI 覆盖矩阵（关键结论）

把已交付的渲染特性逐项对照"编辑器里能不能点到"：

| 特性组 | 路线图项 | 编辑器 UI 入口 | 判定 |
|---|---|---|---|
| GPU 时戳剖析 | R0-1 | Stats 面板（`GPU-TIMING` 分段） | ✅ 完整 |
| 调试视图模式 | R0-2 | Engine 面板 `View mode` 下拉（8 档） | ✅ 完整 |
| TAA | R1-1 | Engine 面板（开关 + blend + clamp） | ✅ 完整 |
| 自动曝光 | R1-2 | Engine 面板（6 个参数） | ✅ 完整 |
| 色彩分级 LUT | R1-3 | Engine 面板（lift/gamma/gain/对比/饱和 + reset） | ✅ 完整 |
| 动态分辨率 | R1-4 | Engine 面板（auto / scale / min / max / sharpen） | ✅ 完整 |
| Hi-Z 遮挡剔除 | R2-1 | Engine 面板（开关 + bias） | ✅ 完整 |
| MDI + GPU 剔除 | R2-2 | Engine 面板（开关 + 遥测） | ✅ 完整 |
| Bindless | R2-3 | **无开关**（隐式常开） | ⚠ 可接受（无用户可调项） |
| 阴影级联硬化 | R3-1 | 仅 Inspector「Casts Shadow」；**级联数/距离/分辨率/硬化/Bias 全无 UI** | ❌ **缺失** |
| Froxel 体积雾 | R3-2 | Engine 面板仅暴露 开关 + Density；**另 6 个参数无 UI** | ⚠ 部分 |
| 区域光 / IES / light function | R3-3 | Inspector 灯属性（IES `nameCombo` + 灯形） | ✅ 基本完整 |
| AlphaMode Mask | R4-1 | Inspector 材质（Alpha Mode / Cutoff） | ✅ 完整 |
| 预积分 SSS | R4-2 | **完全无 UI** | ❌ **缺失** |
| POM 视差 | R4-3 | Inspector 材质（高度图 + Height Scale） | ✅ 完整 |
| 延迟贴花 | R4-4 | Inspector 对象级（贴图/尺寸/淡出）；**全局开关无 UI** | ⚠ 部分 |
| **SSAO** | — | **完全无 UI** | ❌ **缺失** |
| **SSGI** | — | **完全无 UI** | ❌ **缺失** |
| **SSR** | — | **完全无 UI** | ❌ **缺失** |
| **God rays** | — | **完全无 UI** | ❌ **缺失** |
| 天空盒 | — | Engine 面板开关；天顶/地面色/分辨率无 UI | ⚠ 部分 |
| IBL | — | Engine 面板开关；采样数无 UI | ⚠ 部分 |
| LOD | — | Engine 面板开关 + force 档；bias / dbg 无 UI | ⚠ 部分 |

**统计**：约 23 个特性组中，**6 组在编辑器里完全点不到**，6 组只有半个入口，11 组完整。

### 0.3 根因（三条，都是结构性的）

1. **两套平行的控制世界，互不知晓。**
   * **环境变量路径**：`Renderer` 构造函数读 `getenv` → 写进 `PostSettings` / `ScreenSpaceSettings` / …。
     *给自动化测试用的*，启动即定，运行中改不了。
   * **UI 路径**：面板直接改 `r.postSettings().exposure` 等**同一个**结构体字段。
     *给人用的*，运行时可改，但**不落盘**（关掉编辑器就没了）。
   两条路都能改同一个字段，但**谁都没有义务覆盖全部字段**。于是字段加了、env 加了、
   面板忘了——就成了"只有测试能用、人用不了"的功能。

2. **没有"功能登记"这一步。** 一个特性交付时，代码里加了 `if (MYVK_XXX)`，
   但没有任何地方记录"这个特性应该有 UI"。缺失是**静默**的——没人会收到提醒。

3. **面板是"手写长函数"，不是"声明式清单"。** `drawEnginePanel()` 是 500 行的
   `if/Checkbox/Slider` 堆叠。新增一个参数 = 在长函数里再插一段。
   参数一多（158 个 env 的量级），必然漏、必然乱、必然没人愿意维护。

### 0.4 因此，本设计要交付的不是"更多面板"，而是**结构性的契约**

> 让"每个功能都有 UI"成为**系统的性质**，而不是**人的记性**。

---

## 1 · 设计原则

| # | 原则 | 一句话 | 违反的代价 |
|---|---|---|---|
| **P1** | **单一数据源（SSOT）** | 每个可调项在全局只有**一份**权威存储，UI 与 env 都写它 | 双份状态必然不同步，出现"改了 UI 但渲染没变" |
| **P2** | **声明式优先** | 能从元数据生成的 UI，**绝不手写** | 手写必然漏、必然风格不一 |
| **P3** | **分级暴露** | 每个参数标 `Basic/Advanced/Debug` 三档，默认只显示 Basic | 158 个旋钮平铺 = 没人能用 |
| **P4** | **双向可测** | 每个控件有**稳定 ID**，回归脚本能驱动它 | UI 一改，自动化全碎（本项目的回归体系是命根子） |
| **P5** | **全链路可序列化** | 任何设置都能存/读/继承/覆盖 | 调好的画面关掉就丢，无法交接 |
| **P6** | **交付即闭合** | 新功能的"完成"定义**包含** UI 面 | 就是现在这个局面 |
| **P7** | **成本可见** | UI 上直接显示该效果的 GPU 开销 | 3A 引擎里，性能是**可设计**的，不是事后测的 |

**P3 与 P7 是"3A 引擎"与"demo 渲染器"的分水岭。**
成熟引擎（UE / Unity / Frostbite / Decima）参数都是几千个，
它们能用的原因不是"参数少"，而是**分级 + 预设 + 覆盖链 + 成本可见**四件套。

---

## 2 · 分层架构总览

```
┌──────────────────────────────────────────────────────────────────────────┐
│  L5  自动化契约层   Automation Contract                                   │
│      控件稳定 ID · 驱动 API · 与 run_regression.py 的 TRIGGERS 打通        │
├──────────────────────────────────────────────────────────────────────────┤
│  L4  交互层   Interaction                                                 │
│      选择 / Gizmo / 命令栈(Undo) / 快捷键 / 命令面板 / Console / 通知      │
├──────────────────────────────────────────────────────────────────────────┤
│  L3  面板与工作区   Panels & Workspaces                                    │
│      PanelRegistry · 工作区布局(Level/Render/Anim/Cinematic) · 视口浮层     │
├──────────────────────────────────────────────────────────────────────────┤
│  L2  UI 生成层   Declarative UI                                            │
│      ParamDesc → 控件（自动）  ｜  复杂控件手写（曲线/节点图/浏览器）        │
├──────────────────────────────────────────────────────────────────────────┤
│  L1  设置层级   Settings Scopes & Override Chain                           │
│      Editor偏好 → 项目设置 → 场景设置 → 对象设置 → 相机覆盖                │
├──────────────────────────────────────────────────────────────────────────┤
│  L0  特性注册表   FeatureRegistry  ★ 唯一真相源（SSOT）                    │
│      FeatureDesc / ParamDesc / 五路自动派生                                │
└──────────────────────────────────────────────────────────────────────────┘
                    ▲                                    ▲
              engine/render/*.h                   tools/check_feature_surface.py
              （现有设置结构体）                    （强制机制，防退化）
```

**核心思想：L0 是唯一的"登记处"。** L1/L2/L3 的相当一部分内容、
以及 env 读取代码、序列化代码、回归触发映射，全部**从 L0 派生**。
手写的东西从"每个参数一次"降到"每个特性一次"。

---

## 3 · L0：特性注册表（FeatureRegistry）★ 核心

### 3.1 数据结构

放在 `engine/render/FeatureRegistry.h`（纯头文件 + 一处静态注册表，无第三方依赖）。

```cpp
namespace render {

// 参数类型 —— 决定 L2 生成什么控件、L1 怎么序列化
enum class ParamType : uint8_t {
    Bool, Float, Int, Enum, Color3, Color4, Vec2, Vec3,
    TextureRef,   // 资产引用（Content 浏览器选择）
    AssetPath,    // 字符串路径
};

// 暴露档位（P3）：默认只显示 Basic
enum class Tier : uint8_t { Basic, Advanced, Debug };

// 设置作用域（L1）：参数属于哪一级
enum class Scope : uint8_t {
    Engine,    // 全引擎（后处理、DRS…）→ 项目设置
    Scene,     // 每场景（雾、曝光、环境）→ 场景设置
    Object,    // 每对象（材质、贴花、灯）→ Details
    Camera,    // 每相机（DoF、暗角、色差）→ Details
    Editor,    // 编辑器自身偏好（字号、导航灵敏度）→ Preferences
};

struct ParamDesc {
    const char* id;          // 稳定标识（= 序列化键 = 控件 ID 的一部分）
    const char* label;       // 显示名（可本地化）
    ParamType   type;
    Tier        tier;
    const char* tooltip;     // ⚠ 必填：空串视为"未写文档"，自检会 FAIL
    const char* unit;        // "m" / "ms" / "px" / "" —— 显示在控件右侧
    // 范围（Float/Int）
    float  min = 0.0f, max = 1.0f;
    const char* format = "%.2f";
    // 枚举项（Enum，'\0' 结尾）
    const char* const* enumItems = nullptr;
    int    enumCount = 0;
    // 默认值（用于 Reset / 预设）
    float  defFloat = 0.0f;
    int    defInt = 0;
    bool   defBool = false;
    // 环境变量名（可空 = 该参数无 env 入口，纯 UI）
    const char* env = nullptr;
    // 该参数变化时是否需要**资源重建**（true = 会触发 recreate，
    // UI 上要给出"这一项改了会掉帧/会重建"的提示）
    bool   heavy = false;
};

struct FeatureDesc {
    const char* id;            // "decals"
    const char* displayName;   // "延迟贴花（DBuffer）"
    const char* category;      // "材质表现力" / "后处理" / "可见性"
    const char* roadmapRef;    // "R4-4" —— 与 docs/render_roadmap.md 对齐
    Scope       scope;
    Tier        maxTier;       // 该特性最深暴露到哪一档
    const char* panelSection;  // 落在哪个面板的哪个分区（L3）
    const char* gpuTag;        // 对应 GpuTimer 的 section 名（P7 成本显示）
    const char* regressionTest;// "verify_decals" —— 与 TRIGGERS 对齐
    const ParamDesc* params;
    int paramCount;
    // 开关本身也是参数（通常 params[0]）；若该特性无开关（纯参数组）置 null
    const char* enableParamId;
};

// ---- 全局注册表 ----
const FeatureDesc* allFeatures(int* countOut);
const FeatureDesc* findFeature(const char* id);
const ParamDesc*   findParam(const char* featureId, const char* paramId);

// ---- 五路自动派生（见 3.3）----
void  loadEnvOverrides(const FeatureDesc& f, void* target);   // env → 结构体
void  resetToDefaults(const FeatureDesc& f, void* target);    // 默认值
// L2 的 UI 生成器、L1 的序列化器都只吃 FeatureDesc + void* 基址

} // namespace render
```

### 3.2 登记示例（把现有的 `ScreenSpaceSettings` 接进来）

```cpp
// ---- 特性：延迟贴花（R4-4）----
static const ParamDesc kDecalParams[] = {
  { "enabled", "Enable", ParamType::Bool, Tier::Basic, 
    "开关延迟贴花（DBuffer）", "", 0,0,"", nullptr,0, 0,0,false,
    "MYVK_DECALS", /*heavy=*/false },
  { "maxPerFrame", "Max / frame", ParamType::Int, Tier::Advanced,
    "每帧上限，超出丢弃并告警", "", 1,64,"%d", nullptr,0, 0,64,false,
    "MYVK_DECAL_MAX", false },
  { "debugView", "Debug view", ParamType::Bool, Tier::Debug,
    "把贴花 coverage 以原色输出（排查用）", "", 0,0,"", nullptr,0, 0,0,false,
    nullptr, false },
};

static const FeatureDesc kFeatures[] = {
  { "decals", "延迟贴花（DBuffer）", "材质表现力", "R4-4",
    Scope::Engine, Tier::Advanced, "Engine/Rendering",
    "decals",              // GpuTimer 段名 → 成本可显示
    "verify_decals",       // 回归用例
    kDecalParams, 3, "enabled" },

  // ---- 漏网之鱼，现在必须补登记 ----
  { "ssao", "SSAO 环境光遮蔽", "后处理", "—",
    Scope::Scene, Tier::Advanced, "Scene/Ambient",
    "ssao", "verify_ssao",
    kSsaoParams, 5, "enabled" },

  { "sssr", "屏幕空间反射", "后处理", "—",
    Scope::Scene, Tier::Basic, "Scene/Reflections",
    "ssr", "verify_ssr",
    kSsrParams, 3, "enabled" },

  // ... ssgi / godrays / sss / shadowCascade / froxelFog / ... 
};
```

### 3.3 五路自动派生 —— 登记一次的收益

| 派生 | 原来要手写的地方 | 现在 |
|---|---|---|
| **① env 读取** | `Renderer` 构造函数里 `getenv("MYVK_SSR")` 一行一个 | `loadEnvOverrides(feature, &m_ssr)` 一句遍历 |
| **② UI 控件** | `drawEnginePanel()` 里每个参数 3~5 行 `Checkbox/Slider` | `drawFeatureUI(feature, &m_ssr)`（L2 生成器） |
| **③ 序列化** | 手写 JSON 字段读写 | 遍历 `ParamDesc`，按 `id` 写/读（L1） |
| **④ 工具提示** | 每个 `IsItemHovered` + `SetTooltip` 手写 | `ParamDesc::tooltip` 自动挂（**且必填**） |
| **⑤ 回归触发** | `run_regression.py::TRIGGERS` 手写映射 | 由 `regressionTest` 字段生成 / 交叉校验 |

**这就是"结构性保证"**：一个参数只要进了 `ParamDesc`，五处**同时**到位；
一个参数不进 `ParamDesc`，`check_feature_surface.py` 会报"未登记的裸 env"。

### 3.4 与现有代码的衔接（不做大爆炸重构）

现有 4 个设置结构体**保持不动**（它们是 SSOT 的"存储"，只是缺"元数据"）：

| 结构体 | 位置 | 作用域 |
|---|---|---|
| `PostSettings` | `engine/render/PostProcess.h:64` | Engine |
| `CameraEffectSettings` | `Renderer.h`（`m_cameraFx`） | Camera |
| `GridSettings` | `Renderer.h:137` | Editor |
| `ScreenSpaceSettings` | `engine/render/ScreenSpaceSettings.h:39` | Scene |

注册表**只加不换**：给每个已有字段补一行 `ParamDesc`（`id` = 驼峰字段名）。
`loadEnvOverrides` / `drawFeatureUI` / 序列化器按 `offsetof` + 类型分派读写，
所以字段布局不变、现有代码零改动。**这是增量迁移，不是重写。**

---

## 4 · L1：设置层级与覆盖链

### 4.1 五级作用域（对标 UE 的 Project / World / Actor / Editor Preferences）

| 级别 | 存哪 | 覆盖什么 | 例子 |
|---|---|---|---|
| **Editor Preferences** | `%APPDATA%/venn/editor.json` | 编辑器自身（不进游戏包） | 字号、导航灵敏度、栅格、吸附、快捷键 |
| **Project Settings** | `<proj>/config/render.json` | 全项目默认渲染 | 曝光基调、档位预设、MSAA、DRS 策略 |
| **Scene Settings** | 存进 `.scene` 文件 | 该场景覆盖项目 | 雾密度、环境光、SSAO、后处理体积 |
| **Object Settings** | 存进实体组件 | 该对象 | 材质、贴花、灯属性、LOD 策略 |
| **Camera Overrides** | 存进相机组件 | 该相机 | DoF / 运动模糊 / 暗角 / 色差 |

### 4.2 覆盖解析（读时从最具体往最泛回退）

```cpp
// 求某参数的有效值：相机覆盖 > 对象 > 场景 > 项目 > 默认
float resolve(const ParamDesc& p, Scope ownerScope, ...) {
    if (ownerScope == Scope::Camera && camHasOverride(p.id)) return camGet(p.id);
    if (sceneHasOverride(p.id))  return sceneGet(p.id);
    if (projectHas(p.id))        return projectGet(p.id);
    return p.defFloat;                       // 注册表里的默认值
}
```

**UI 表现（P3 + P5 的结合）**：每个控件左侧有**覆盖指示器**——
`●` 本场景已覆盖 / `○` 继承自上层 / `◇` 用默认值。
右键 → `Reset to inherited` / `Copy` / `Paste` / `Show in …`。
这是 UE Details 面板的招牌交互，也是"几千个参数还能用"的关键。

### 4.3 落盘格式

```jsonc
// <proj>/config/render.json —— 项目设置
{ "version": 1,
  "qualityLevel": "high",                  // Low/Medium/High/Ultra/Cinematic
  "features": {
    "decals":  { "enabled": true, "maxPerFrame": 64 },
    "taa":     { "enabled": true, "blend": 0.1 },
    "ssr":     { "enabled": false }
  } }

// 场景里 —— 只写"与项目不同的部分"（稀疏覆盖，diff 友好）
{ "overrides": { "fog": { "enabled": true, "density": 0.02 } } }
```

**零成本保留现有回归基线**：所有默认值 = 现在的默认值（全关），
`render.json` **不存在**时行为与今天**逐像素一致**。

---

## 5 · L2：UI 生成层

### 5.1 类型 → 控件映射（自动生成）

| `ParamType` | 控件 | 说明 |
|---|---|---|
| `Bool` | `Checkbox` + 工具提示 | 开关类 |
| `Float` | `SliderFloat`（有范围）/ `DragFloat`（无范围） | 范围来自 `min/max`，后缀来自 `unit` |
| `Int` | `SliderInt` / `DragInt` | |
| `Enum` | `Combo` | 项来自 `enumItems` |
| `Color3/4` | `ColorEdit3/4` | |
| `Vec2/Vec3` | `DragFloat2/3` | |
| `TextureRef` | 缩略图按钮 + 拖放（复用 `ThumbnailCache` / `EditorDragDrop`） | 需要 Content 浏览器配合 |
| `AssetPath` | 只读文本框 + 「…」按钮 | |

生成器同时负责：**工具提示**（自动挂 `tooltip`）、
**覆盖指示器**（读 L1 的覆盖状态）、**成本徽标**（读 `gpuTag` 的实测毫秒）、
**heavy 警告**（`heavy=true` 的参数旁挂 ⚠ 图标，且改动时走 `CommandStack`）。

### 5.2 分级折叠（P3）—— 面板顶部的三档开关

```
[ Basic ● ]  [ Advanced ]  [ Debug ]          ← 全局档位（Preferences 可改默认）
──────────────────────────────────────────────
▾ 延迟贴花（DBuffer）                     [ ✓ ]   ← Basic 档只看到这一个
  Max / frame   [  64 ]   (GPU 0.02 ms)          ← Advanced 档才出现
  Debug view    [  ]                               ← Debug 档才出现
```

**再叠加一层"搜索框"**（对标 UE Details 的过滤）：顶部输入 `ssr` 直接筛出相关特性，
3A 引擎里这是**唯一**能让几千参数可用的手段。

### 5.3 手写面板的边界（哪些**不**自动生成）

自动生成适合"标量/开关/枚举"，不适合下列，**保持手写**：

* **ContentBrowser** —— 文件树 + 缩略图 + 拖放（已有，`ContentBrowser.cpp` 42 KB）
* **曲线编辑器** —— 后续动画/相机曲线
* **节点图** —— 后续材质编辑器 / RenderGraph 可视化（R7-1）
* **视口浮层** —— gizmo、视图模式 HUD、工具栏
* **遥测可视化** —— GPU 时戳条形图、簇统计、MDI 批次统计

即 **"结构自动、布局手写"**：面板**里**的参数自动铺，面板**怎么摆**手工定。

---

## 6 · L3：面板系统与工作区

### 6.1 面板登记（PanelRegistry）

现有 `EditorContext::PanelVisibility`（一堆 `bool hierarchy = true;`）升级为表驱动：

```cpp
struct PanelDesc {
    const char* id;            // "hierarchy"
    const char* title;         // "Hierarchy"（可本地化，带 "###id" 稳定 ID）
    const char* menuPath;      // "Window/General/Hierarchy"
    const char* shortcut;      // "Ctrl+1"
    const char* icon;          // EditorIcons.h 里的字形
    bool  defaultOpen;
    bool  floating;            // 停靠 or 浮动
    void (*draw)(EditorApp&);  // 绘制回调
};
```

收益：**View 菜单、快捷键、布局持久化、ICON 全部从表生成**。
新增面板 = 加一行，不再改 3 处。

### 6.2 工作区（Workspace）—— 3A 引擎的必备

不同岗位用不同的面板组合。用 `SplitLayout`（已有分栏树）保存多套比例：

| 工作区 | 布局 | 给谁用 |
|---|---|---|
| **Level**（默认） | Hierarchy ▏Viewport▕ Inspector ＋ Content ＋ Stats | 关卡美术 |
| **Rendering** | Viewport 全宽 ＋ Engine 面板 ＋ GPU 时戳条形图 ＋ 视图模式 | TA / 图形程序 |
| **Character** | Viewport ＋ CharacterWindow 全宽（已有） | 角色美术 |
| **Animation** | Viewport ＋ 时间轴 ＋ 曲线编辑器（待建） | 动画师 |
| **Cinematic** | Viewport 无 HUD 满屏 ＋ 相机序列器（待建） | 导演 |
| **Debug** | Viewport ＋ Console ＋ Stats ＋ 各 Debug 档参数 | 程序 |

**QoL**：`Window ▾` 菜单切工作区；F11 全屏视口（已有 `drawFullscreenGame`）；
布局随窗口自适应（`SplitLayout` 已实现比例 + 可拖 + 可折叠）。

### 6.3 面板清单（现有 / 待建）

| 面板 | 状态 | 归属工作区 | 备注 |
|---|---|---|---|
| Hierarchy | ✅ 已有 | Level | 场景树 |
| Inspector | ✅ 已有 | 全部 | 对象属性。**左列 11 个图标页签 + 右侧一页内容**（Object / Tool / Render / Output / Scene / World / Particles / Physics / Material / Animation / All），见 `E11.1` |
| ContentBrowser | ✅ 已有 | 全部 | 资产 |
| Viewport | ✅ 已有 | 全部 | 视口 + 浮层 |
| Stats | ✅ 已有 | Debug/Rendering | 帧统计 + GPU 时戳 |
| Engine | ✅ 已有（需重构） | Rendering | **改为按 `FeatureDesc` 分区生成** |
| Preferences | ✅ 已有 | — | 编辑器偏好 |
| CharacterWindow | ✅ 已有 | Character | 角色预览 |
| Scripts | ✅ 已有 | Debug | 脚本日志 |
| **Console** | ✅ 已有（E16，2026-10-14） | Debug | 命令 + 日志（`VK_LOG_*` 汇入） |
| **Command Palette** | ✅ 已有（E18，2026-10-15） | — | Ctrl+Shift+P，模糊搜索所有命令 |
| **Renderer Graph** | ❌ 待建（阻塞于 R7-1） | Rendering | RenderGraph 可视化 |
| **GPU Profile 时间轴** | ✅ 已有（E25，2026-10-15） | Rendering | 时戳甘特图 + 60 帧历史 |
| **Quality Settings** | ✅ 已有（E26，2026-10-19） | Authoring | 画质档位 + 平台预设 + 差异预览 + 一致性检查 |
| **Asset Preview** | ⚠ 部分 | — | 材质/网格预览（`CharacterWindow` 是雏形） |

---

## 7 · L4：交互层

### 7.1 已有的底座（复用，不重建）

| 能力 | 位置 | 状态 |
|---|---|---|
| 选择 / 拾取 | `PickingSystem.cpp` | ✅ |
| Gizmo 变换 | `GizmoController.cpp`（47 KB） | ✅ |
| 撤销/重做 | `Command.h/cpp`（`CommandStack` + `LambdaCommand` / `TransformEditCommand` / `SceneSnapshotCommand`…） | ✅ |
| 拖放 | `EditorDragDrop.h` | ✅ |
| 快捷键 | `EditorApp::handleShortcuts()` | ✅ |
| 吸附 | `EditorContext::snapMoveRef()` 等 | ✅ |

### 7.2 需要补的三块

**① 撤销覆盖扩展到"设置类改动"（当前是空白）**

现状：只有变换/碰撞体/图元/场景快照可撤销。**改渲染参数不可撤销**——
拖一下 Exposure 滑条就回不去了。

修法：生成器里的每次提交（`IsItemDeactivatedAfterEdit`）包一个
`LambdaCommand{ before, after }` 推进 `CommandStack`。**自动生成 → 自动可撤销**，
这正是 P2 的红利。

**② Console 面板**

* 输入：`exposure 1.5` / `r.SSR 1` / `vw.Decal 1` 之类的**命令**（Unreal 风格）。
* 输出：把 `VK_LOG_INFO/WARN/ERROR` 汇入（现在只进 stdout/文件）；支持级别过滤 + 关键字搜索。
* 与命令面板共用**命令注册表**（命令 = `{name, help, handler}`，同样表驱动）。

**③ 命令面板（Command Palette）**

`Ctrl+Shift+P` → 模糊搜索**所有**菜单项与命令。这是"功能完全"的**可发现性**保障：
3A 引擎命令上千，靠菜单翻找不可行。

### 7.3 通知 / 进度

`toast` 式通知（右上角）+ 长任务进度条（烘焙光照贴图、编译着色器）。
由 `VK_LOG_WARN/ERROR` 自动升级为可见通知（现在只写日志，用户看不到）。

---

## 8 · L5：自动化契约（与回归体系打通）

本项目的回归体系是**命根子**（现 42 例 + 变更影响分析）。UI 一变，自动化就碎——所以
**UI 必须可被脚本驱动**，这是硬约束。

### 8.1 控件稳定 ID

沿用项目已有的 `###id` 惯例（`Begin("Preferences###prefs")`）。
规范：**`###<featureId>.<paramId>`**，例如 `###decals.enabled`。
自动生成的控件天然满足（`id` 来自 `ParamDesc`）。

### 8.2 已有的自动化设施（继续沿用）

| 设施 | 用途 |
|---|---|
| `MYVK_EDITOR_NO_HUD=1` | 关 HUD，供像素级对照 |
| `MYVK_LOG_RECTS=1` | 打印控件矩形（`VP-ADD-*` / `VM-COMBO` 等标签） |
| `capture_window.py` / `LogTap` / `wait_rect` | 抓图 + 日志断言 + 等控件 |
| `MYVK_VIEW_MODE=<n>` / `MYVK_SSR=1` … | env 预置状态（**这是 env 存在的正当理由**） |

### 8.3 新增：UI 驱动断言

给 `tools/` 加一个轻量驱动层（基于已有 `LogTap` + 注入点击）：
`set_feature_param("ssr", "enabled", 1)` → 打开面板 → 点控件 → 校验日志。
这样**每一个特性的 UI 面都能被回归覆盖**，而不是只有引擎侧被覆盖。

### 8.4 与 `TRIGGERS` 打通（关键联动）

`run_regression.py` 已有的 `TRIGGERS` 表把"源文件改动 → 回归用例"映射起来。
现在**再挂一条**：`FeatureDesc::regressionTest` → `ParamDesc` 所在的 **UI 文件**。
于是"只改了 `EditorApp.cpp` 里的 SSAO 分区"也能**精确**触发 `verify_ssao`，
而不是因为 `EditorApp.cpp` 是横切文件就全量重跑。

---

## 9 · 强制机制：新功能的 Definition of Done

### 9.1 五件套（缺一不可）

任何新特性（R5-x / R6-x / R7-x …）交付时，**必须**同时给出：

| # | 交付物 | 落点 | 自检怎么查 |
|---|---|---|---|
| **1** | **实现** | 引擎/着色器 | 编译通过 |
| **2** | **注册表登记** | `FeatureRegistry.h` 的 `kFeatures[]` | `check_feature_surface.py` 查 `id` 存在 |
| **3** | **UI 面** | 至少 Basic 档有一个开关/参数在某个面板 | 查 `panelSection` 指向的绘制代码里有 `featureId` |
| **4** | **序列化** | `Scope` 对应的落盘文件 | 查序列化器是否覆盖（表驱动则自动） |
| **5** | **回归用例 + 触发映射** | `tools/verify_<x>.py` + `run_regression.py::DEFAULT` + `TRIGGERS` | `check_regression_map.py` + `check_feature_surface.py` |

> **规则（写进 `MEMORY.md`）**：**没有 UI 面的特性，不算交付。**
> 只靠 `MYVK_*` 环境变量打开的效果，视为"仅测试可用"，**不计入完成度**。

### 9.2 `tools/check_feature_surface.py`（新增，与 `check_regression_map.py` 成对）

秒级、不跑 GUI。检查项：

| 组 | 检查 | 失败含义 |
|---|---|---|
| **A** | 每个 `FeatureDesc` 的 `id` / `displayName` / `roadmapRef` 非空 | 登记不完整 |
| **B** | 每个 `ParamDesc` 的 `tooltip` 非空 | 参数没写文档 = 用户看不懂 |
| **C** | 每个特性的 `panelSection` 在 `editor/` 里能 grep 到绘制代码 | **功能没 UI**（头号检查） |
| **D** | 每个 `env` 里出现过的 `MYVK_*` 都在某个 `ParamDesc` 里登记 | **裸 env 漏网**（它只能靠环境变量开） |
| **E** | 每个 `regressionTest` 在 `run_regression.py::DEFAULT` 里 | 没有回归保护 |
| **F** | 每个 `gpuTag` 在 `GpuTimer` 段名里有对应 | 成本不可见（P7） |
| **G** | 每个 `heavy=true` 的参数在 UI 上有 ⚠ 标记 | 用户不知道改它会重建 |

**输出**：覆盖率表格 + 缺失清单 + 非零退出码。
CI / 回归前跑一次。**这是"以后新功能也会想着 UI"的机器保证。**

---

## 10 · 落地路线（三期）

### E1 · 地基（先补齐现存欠账）

1. `FeatureRegistry.h`：结构 + `kFeatures[]`（先把**现有** 23 组登记进去）。
2. `loadEnvOverrides()` / `resetToDefaults()`：接管 `Renderer` 构造函数的 env 读取。
3. `drawFeatureUI()`：L2 生成器（含 tier 折叠 + 搜索 + 覆盖指示）。
4. `Engine` 面板瘦身：500 行手写 → 遍历 `kFeatures[]` 生成。
5. **补齐 6 个"无 UI"特性**：SSAO / SSGI / SSR / God rays / SSS / 阴影级联硬化。
6. `tools/check_feature_surface.py`：G 项先立起来（`C`/`D` 两项一开始会红，**这就是工作清单**）。
7. 序列化：`ProjectSettings`（`config/render.json`）读写。

**验收**：`check_feature_surface.py` 的 C/D 组全绿 = 23 组特性**全部有 UI 面**。

### E2 · 可用性（成熟度）

8. 设置层级：Scene / Object / Camera 覆盖 + 覆盖指示器 + `Reset to inherited`。
9. Console 面板 + 命令注册表；`VK_LOG_*` 汇入。
10. 命令面板（`Ctrl+Shift+P`）。
11. 工作区（6 套）+ 布局持久化。
12. 画质档位（Low→Cinematic）+ 平台预设 + `Quality Settings` 面板。
13. 撤销覆盖到设置类改动。
14. GPU 时戳时间轴面板（P7 的可视化）。
15. 通知 / 进度系统。

### E3 · 生产力（3A 特征）

16. 资产编辑器框架（材质节点图 / 曲线编辑器）。
17. RenderGraph 可视化（与 R7-1 同步）。
18. 多视口 / 资产预览面板。
19. 烘焙流水线 UI（光照贴图 / 反射探针，与 R5-2 同步）。
20. 打包/发布 UI：**Debug 档与调试 UI 在 Shipping 构建里剔除**。

---

## 11 · 各特性 → 对应 UI 的落点（补全清单）

下表是 E1 阶段第 5 步的**具体工作清单**（`check_feature_surface.py` 的 C 组红项）：

| 特性 | 作用域 | 面板/分区 | Basic 档必须有的控件 | Advanced 档补充 |
|---|---|---|---|---|
| **SSAO** | Scene | Engine ▸ Scene/Ambient | `Enable` | Radius / Strength / Bias / 质量档 |
| **SSGI** | Scene | Engine ▸ Scene/Global Illum | `Enable` | Strength / Radius |
| **SSR** | Scene | Engine ▸ Scene/Reflections | `Enable` + `Strength` | 步数 / 厚度 / 粗糙度截止 |
| **God rays** | Scene | Engine ▸ Scene/Atmosphere | `Enable` | 密度 / 权重 / 衰减 |
| **SSS** | Object | Inspector ▸ 材质 | `Subsurface` 开关 + 颜色 | 曲率 / 散射半径 |
| **阴影级联硬化** | Scene | Engine ▸ Shadows | 级联数 + 最大距离 | 分辨率 / Snap / Lambda / 硬化半影 |
| **Froxel 雾**（补全） | Scene | Engine ▸ Atmosphere | Enable + Density | Albedo / HeightFalloff / Blob / PhaseG |
| **贴花**（补全） | Engine | Engine ▸ Rendering | 全局 `Enable` | Max/frame / Debug view |
| **天空盒**（补全） | Scene | Engine ▸ Sky | `Enable` | 天顶色 / 地面色 / 分辨率 |
| **IBL**（补全） | Scene | Engine ▸ Sky | `Enable` | 采样数 / 强度 |
| **LOD**（补全） | Project | Engine ▸ LOD | `Enable` | Bias / 强制档 / Debug |

---

## 12 · 与未来功能的耦合点（R5 ~ R7 预留）

架构必须**现在就**给未做的功能留好位置，否则又会走回"功能有了、界面没有"的老路。

| 路线图项 | 需要的 UI 面 | 归属层 |
|---|---|---|
| **R5-1 反射探针** | 场景里放置探针的 gizmo + 探针属性面板（分辨率/影响范围）+ 探针预览球 + 「烘焙」按钮 | L3 面板 + L4 gizmo |
| **R5-2 lightmap / 光照探针** | 烘焙设置面板 + 进度条 + 光照贴图预览 | L3 + L4 通知/进度 |
| **R5-3 DDGI** | 探针网格可视化 + 参数面板 | L3 |
| **R6-1 GPU 粒子** ⭐ | **粒子编辑器**（发射器模块栈 + 曲线 + 预览）| L3（E3 的资产编辑器框架） |
| **R6-2 水体** | 水面参数面板（波浪/泡沫/折射）+ 视口内交互 | L2 + L3 |
| **R6-3 植被风** | 风场方向/强度面板 + 视口风可视化 | L2 |
| **R6-4 地形** | **地形雕刻工具**（笔刷 / 高度图导入 / splat 图层）+ 工具栏 | L4 工具 + L3 面板 |
| **R6-5 动态天空** | 大气参数面板（日期/时间/浊度）+ 时间轴 | L2 + L3 |
| **R7-1 RenderGraph** | **图可视化面板**（pass 节点 + 资源边 + 时戳叠加） | L3（E3） |
| **R7-2 细粒度同步** | 时间轴/依赖可视化 | L3 |
| **R7-3 ray tracing** | 加速结构可视化 + RT 设置面板 | L2 + L3 |

> **准入规则（再强调一次）**：以上每一项的"完成"，都包含它的 UI 面。
> 在 `FeatureRegistry` 里登记 = 自动获得 env / UI / 序列化 / 触发映射 / 成本显示五件套。

---

## 附录 A · 现有代码与本架构的对照

| 架构层 | 现有承接物 | 需要做 |
|---|---|---|
| L0 注册表 | 无（4 个设置结构体裸奔） | **新建** |
| L1 设置层级 | 无（只有内存中的结构体） | **新建** 序列化 + 覆盖链 |
| L2 UI 生成 | `drawEnginePanel()` 手写 500 行 | 改为生成器 |
| L3 面板/工作区 | `SplitLayout` + `PanelVisibility` + 9 面板 | 表驱动 + 工作区 |
| L4 交互 | `CommandStack` / `Gizmo` / `Picking` / `Shortcuts` | 补 Console / 命令面板 / 设置撤销 |
| L5 自动化 | `LogTap` / `MYVK_LOG_RECTS` / `NO_HUD` | 补控件 ID 规范 + UI 驱动 |

### 附录 A.1 · 落地进度（2026-10-15）

阶段 A 的地基与 B/C 两个阶段已经落地，表里"需要做"那列多数已兑现：

| 架构层 | 现在的承接物 | 状态 |
|---|---|---|
| L0 注册表 | `engine/render/FeatureRegistry.*`（29 组 / 110 参数，`bindParam(id,&成员,kind)`） | ✅ E01 |
| L1 设置层级 | **仍是单层**：`editor/EditorLayoutIO.*` 只做"布局 + 扁平偏好表"落盘（`config/editor_layout.json` / `config/editor_prefs.json`）。5 层覆盖链**未做** | ⚠ E03 |
| L2 UI 生成 | `editor/FeatureUIGen.*`（`ParamDesc` → 控件，控件 ID = `###<featureId>.<paramId>`） | ✅ E02 |
| L3 面板/工作区 | `editor/PanelRegistry.*`（加一行 = 菜单/快捷键/图标/落盘四路派生）+ `editor/Workspace.*`（六套工作区）+ `editor/SplitLayout.*`（可视化/双击折叠/最大化/比例预设/最小宽度） | ✅ E04~E06 / E08 |
| L4 交互 | `editor/ConsolePanel.*`（E16，命令行 `set/get/reset/params/preset/stat`）；命令面板 / 设置撤销 / 通知进度仍未做 | 🟡 部分 |
| L5 自动化 | `LogTap` + `MYVK_LOG_RECTS` + `MYVK_EDITOR_NO_HUD` + **`VP-OVERLAY` 一类"只在状态变化时打"的状态日志**；每个面板一支 `verify_*.py` | ✅ |

**三条后来才确立的实现约定**（写代码前先读）：

1. **编辑器侧的注入 env 一律带 `MYVK_EDITOR_` 前缀**（`MYVK_EDITOR_CB_TYPE` /
   `_INSP_SEARCH` / `_HIER_FILTER` / `_VP_LAYOUT` …）。`tools/check_feature_surface.py`
   的白名单是**按前缀放行**的 —— 不带前缀的注入点会被判成"裸 env 漏网"。
   这条同时也让"渲染特性 env"（`MYVK_SSAO`…）与"编辑器注入 env"一眼可分。
2. **视图浮层是视口窗口的 ImGui 子窗口**（不是独立顶层窗口）——
   视口窗口每帧都被 `BringWindowToDisplayFront`，独立窗口会被它盖住。
   显隐分两层：总闸 `m_showOverlay` × 四块各自的开关（`EditorContext::ViewportOverlay`）。
3. **主题与语言是"横切展示层"，不进业务逻辑**（2026-10-17 确立）：
   - **主题**（`engine/ui/ImGuiTheme.*`）：尺寸度量与颜色**解耦**，颜色收敛到 `ThemeTokens`
     的语义槽；调色板集中在 `themeVennDark()` / `deriveDark()` / `deriveLight()`。
     **调用点只认 `ui::tokens()` 的槽名，不写字面色值**（否则浅色主题下必留"深色孤岛"）。
   - **语言**（`editor/Localization.*`）：**源码里只写英文**，中文是 `tr()` 的**运行时映射**；
     窗口标题一律走 `trId(en,id)` 保住 `###id`。任何"把中文写回业务逻辑"的做法都算违反契约
     （会污染字符串比较 / 序列化 key / ImGui ID）。
   - 两者都通过**偏好表**持久化（`theme` / `language` 两个 int 键），与其它偏好同一套落盘路径。

## 附录 B · 设计取舍（诚实记录）

1. **不引入 ImGui docking 分支**（沿用 `SplitLayout` 手工分栏）。
   理由：项目已有可用的分栏树 + 折叠 + 比例持久化，docking 分支会带进 `imgui.ini`
   外部状态与回归不友好（像素级对照需要确定性布局）。
2. **env 变量保留、不废除。** 它是自动化测试**预置状态**的正当手段（P4）。
   架构只是**强制**它必须同时在注册表里登记，从而保证 UI 侧也有入口。
3. **默认值一律保持"全关"**（与今天一致）。`render.json` 缺失时行为逐像素不变——
   **不破坏现有全量回归的基线**（E14 收口时 42 例），这是硬约束。
4. **不追求"自动生成一切"。** 复杂控件（曲线/节点图/浏览器）手写更划算，
   自动生成只覆盖"标量/开关/枚举"这类占绝大多数的参数。

## 附录 C · 环境变量归类（现状，158 个）

`SHADOW_`(9) · `EDITOR_`(9) · `VFOG_`(8) · `CG_`(8) · `DRS_`(7) · `AE_`(6) ·
`TAA_`/`SKY_`/`SHADER_`/`LOD_`/`LOCAL_`/`DOF_`(各 4) · `VIGNETTE_`/`SSGI_`/`SSAO_`/
`POM_`/`MB_`/`HIZ_`/`FOG_`(各 3) · 其余 2 个以内。
**其中约 60% 目前编辑器里没有任何入口** —— 这就是 E1 阶段要清零的欠账。

---

## 附录 D · 运行时（`Game.exe`）进场后的分层（2026-10-17 起）

M1（`docs/ship_roadmap.md` §4）把"只有一个 `Editor.exe`"这个结构性缺陷抹掉了。
本附录记下**从那天起**必须遵守的三件事 —— 它们是"编辑器能编译"再也不能证明的东西。

### D.1 三个 CMake 目标 / 两个产物

| target | 源 | 产物 | 说明 |
| --- | --- | --- | --- |
| `VennEngine`（STATIC） | `engine/**` | `VennEngine.lib` | 只依赖第三方（Vulkan/GLFW/ImGui/tinygltf/stb）。**不认识 `editor/`、不认识 `runtime/`** |
| `VennEditor` | `editor/**` | `Editor.exe`（`OUTPUT_NAME`） | 链 `VennEngine` |
| `VennRuntime` | `runtime/**` | `Game.exe`（`OUTPUT_NAME`） | 链 `VennEngine` |

**依赖方向单向**：`runtime → engine ← editor`。

> ⚠ 改名陷阱：`add_executable(Editor ...)` 改成 `VennEditor` 之后，所有
> `add_dependencies(Editor ...)` / `target_link_options(Editor ...)` /
> `myvk_copy_runtime_assets(Editor)` 都要同步改，否则 CMake 配置阶段就报
> target 不存在。产物名靠 `set_target_properties(... OUTPUT_NAME Editor)` 保住。
> `add_library(venn ALIAS VennEngine)` 保住旧引用。

> ⚠ 资产拷贝：**必须**是"无输出文件的 custom target + `ALL`"（现在叫
> `VennShadersCopy` / `VennAssetsCopy`，`if(NOT TARGET)` 守卫全局只建一次）。
> **禁用 `add_custom_command(TARGET ... POST_BUILD)`** —— 它只在**重新链接**
> 时跑，改了 shader/贴图不会生效（老坑，见 MEMORY）。

### D.2 编辑器那边**不许**再自己实现的三件事

它们已经搬到引擎层，编辑器与运行时**共用同一份**（`engine/physics/ScenePhysics.{h,cpp}`）：

1. 场景 → 物理世界的构建（显式形状 / Auto 网格 AABB / 兜底 0.5m 球 / 地面半空间 / 密度→质量 / 休眠开关）
2. 运动学体同步（实体 → 刚体，步进前）
3. 写回实体变换（只写动态体，步进后）

以及两个数学工具：`eulerXYZFromMatrix()`（站内约定 **R = Rx·Ry·Rz**）、`scaleShapeForWorld()`（缩放烘进尺寸）。

**为什么这是硬约束**：`ship_roadmap.md` §15 风险 #10 是"编辑器里调好的手感出包后不一样"。
靠"两边都记得同步改"是防不住的 —— 只有**同一份二进制**才防得住。
`tools/verify_runtime_loop.py` 的 D 组就是把这件事钉成断言：同一场景、同一 `MYVK_FIXED_DT`，
编辑器 Play 与 `Game.exe` 的 `PHYS-BODY` 行**逐字符相同**。

### D.3 两个新接缝

1. **`core::Application::ownsFrameRendering()`**（默认 `false`）
   运行时重写成 `true`：`GameLoop` 把"渲染"当作一个**可计数的相位**，所以不让基类在
   `onUpdate` 之后补画一次。⚠ 重写成 `true` 之后**必须**在 `onUpdate` 里真的画一次。
2. **`core::setLogSink()` 换消费者**
   编辑器挂 Console 面板的环形缓冲，运行时挂文件 sink（`runtime/RuntimeLog.cpp`）。
   sink 是**单消费者**，这是它的设计前提 —— 别再挂第二个。

### D.4 `engine/script/` 与 namespace `vks`

`.vks` 脚本引擎从 `editor/script/` 搬到 `engine/script/`（`git mv`，历史可 `--follow`），
namespace 从 `editor` 改成 **`vks`**（选它而不是 `engine::script`：`.vks` 就是语言名，
也不与目录级通用名撞车）。

> ⚠ 在 `editor/EditorApp.h` 里前置声明它时，`namespace vks { class ScriptEngine; }`
> 必须写在 `namespace editor` **外面** —— 写进去会变成嵌套的 `editor::vks`，
> 把外层的 `::vks` 遮住（症状看起来像"少 include"）。这与 `physics` 的处理完全同源。

### D.5 运行时侧的五件套同样成立

`runtime/**` 已进 `run_regression.py` 的 `WATCH_ROOTS`（之前不在 → 改运行时一个字节
都不会被影响分析看见）。三条用例全部登记在 `TRIGGERS` 的 `runtime/**` 名下，
**三条一起跑**是故意的：只挂一条会让 `--narrow` 把另外两条判成"不受影响"。

---

## 附录 E · 设置层级（E03，2026-10-17 起）

E01 把"每个旋钮"收敛成了**一处存储**，但存储只有一层（默认值 + env）。
E03 在它外面套了**五层覆盖链**：

```
Editor Preferences   跨项目（config/render_prefs.json）
  └─ Project         config/render.json      ← 唯一有磁盘写入口的一层
      └─ Scene       场景文件内嵌块          （schema 改动属 E20）
          └─ Object  实体组件
              └─ Camera  相机组件
```

读的时候从**最具体往最泛**回退，一路没有就用出厂默认。实现全在
`engine/render/RenderSettingsIO.{h,cpp}`，引擎层持有唯一一份
（`render::activeChain()`），编辑器与运行时**共用** —— 与
`engine/physics/ScenePhysics.*` 同一个理由：同一个参数不该因为"谁来跑"
而有两套值。

### E.1 两个独立的应用标记（这条最容易写错）

`FeatureRegistry` 的每条绑定有 **两个**"处理过没有"：

| 标记 | 谁置位 | 数据源 |
| --- | --- | --- |
| `applied` | `loadEnvOverrides()` | `MYVK_*` |
| `settingsApplied` | `takeUnappliedBindings()` | 覆盖链 |

**为什么不能合成一个**：绑定是**分阶段**发生的（构造期一批，
`createMainTarget` 之后又来一批 post 参数）。如果共用一个标记，第二个阶段
调用 `loadEnvOverrides()` 时会认为"这批已经处理过"，env 就写不进去了；
反过来先应用文件层再应用 env，env 又能把文件值盖掉 —— 所以每个阶段的
顺序被钉死成：

```cpp
bindXxxFeatureParams();
render::loadStartupLayers();   // 只读一次，幂等
render::applyToRegistry();     // 文件层（只处理未处理过的绑定）
render::loadEnvOverrides();    // env 永远最后写 = 永远赢
```

### E.2 校验规则必须与 env 完全一致

文件层的值只走 env 那一套（拒绝非有限值 / `minAccept,maxAccept` /
`clampLo,clampHi`），**不**额外按 ParamDesc 的 UI 范围 `lo,hi` 夹紧。

> ⚠ 已经踩过：`lshadow.bias` 的 UI 范围是 `0~1`，而它的默认 z 分量是
> `1.2`。按 UI 夹紧的话，一次"导出 → 重载"就会把默认值本身改坏，
> 而且**不报错**（只是画面悄悄变了）。

### E.3 三条硬约束（等价于 E03 之前的"逐像素不变"）

1. **无 `config/render.json` = 零动作**：不读、不写、**一条日志都不打**。
   `verify_settings_layers` 的 B1 就是数 `RENDER-SET` 的出现次数（必须是 0）。
2. **首次运行不自动生成文件**：只有"用户真的改过参数"（关闭编辑器时落盘）
   或点了 Engine 面板的 **Export current** 才写。否则一次手动会话就会在
   仓库里留下一个 json，把所有像素基线悄悄换掉。
3. **自动化运行不读默认路径、写一律拒绝**（`MYVK_FRAMES` /
   `MYVK_NO_WINDOW_SAVE` 即为自动化）。显式 `MYVK_RENDER_SETTINGS=<path>`
   照读 —— 那是脚本自己要的路径。

### E.4 落盘的形状是"最小覆盖集"

`captureFromRegistry()` 只抓**与默认值不同**的键，所以：

- 导出 → 重载是**逐位往返**（自检 `E3_roundtrip_bitwise`）；
- 文件里读不到"和默认一样"的噪音；
- 用户改回默认值后，那一项会**从文件里消失**（而不是留一个等于默认的键）。

### E.5 校验入口

- 无窗口：`Editor.exe` + `MYVK_RENDER_SETTINGS_TEST=1`（`SET-TEST` /
  `SET-TOTAL`，与 `MYVK_PHYS_TEST` 同一条"在 Vulkan 之前 return"的路）
- 端到端：`tools/verify_settings_layers.py`（已进 `DEFAULT` + `TRIGGERS`）
- 格式说明与示例：`config/README.md`、`config/render.example.json`
  （示例文件本身被 J 组当输入校验 —— **文档里的例子不能是假的**）
