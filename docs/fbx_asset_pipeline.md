# FBX 资产链（离线编译 → 自研二进制 → 运行时加载）

> 2026-10-22 · 阶段六（序列化）+ 阶段七（运行时加载）的落地记录
> 这是"补齐一条 3D 游戏链路"系列的**第二篇**（第一篇：`docs/character_pipeline_roadmap.md`）。
> 所有结论都对着磁盘代码核过，行号是 2026-10-22 的版本。
>
> **进度**：**4.1~4.4 / 4.6 / 4.7（抽帧部分）/ 4.8 / 5.1~5.5 ✅** → 未做 **4.5 贴图内嵌 / 4.7 量化 / 6.3~6.6**
> （见 §7，每条都写了"为什么这一轮不做"）。
>
> **配套**：本文只讲**格式与离线编译**。资产进场景之后的事（1:1 尺度校验、
> 第三人称绑定、一键自动绑骨、程序化生成 idle/walk/run/jump）在
> [external_model_pipeline.md](external_model_pipeline.md)。

---

## 0 · 一页结论

**七阶段里真正缺的只有第六、七阶段。** 前五个阶段（ufbx 集成、场景图遍历、
坐标/单位修正、骨骼蒙皮、PBR 材质）引擎里**早就通了**，而且质量不低 ——
`FbxLoader.cpp` 里那些注释（列主序 3x4、`target_unit_meters` 是倒数、
蒙皮顶点必须烘到静止世界空间）是踩过一遍才写得出来的。

所以这一轮**没有重写导入**，而是把导入的结果**烘成自有格式**，让运行时
彻底不碰 FBX：

```
离线（有磁盘、可以慢）                          运行时（有帧预算、必须快）
────────────────────────────────────────────  ──────────────────────────────
girl.fbx ──ufbx──→ SourceModel（IR）
                      │  焊接 / 重算法线 / Forsyth 重排
                      │  动画误差有界抽帧
                      ↓
                  writeMeshFile
                      ↓
                  girl.vmesh ────────────────→ VMeshLoader ─→ assets::Model
                                                                   ↓
                                                          Vulkan 资源 / 蒙皮 / LOD
```

| 阶段 | 内容 | 状态 | 证据 |
|------|------|------|------|
| 一 | 中间格式（IR）而不是 FBX→Vulkan 直连 | ✅ | `assetio/SourceModel.h`（7 种结构，151 行） |
| 二 | ufbx + 三层项目结构 | ✅ | 新增 `assetio/`（共享）+ `assetc/`（离线），依赖方向见 §1 |
| 三 | 静态网格导入 / 坐标单位修正 / 三角化 / 焊接 / 重算法线 | ✅ | 复用既有 `FbxImport.cpp`（702 行，纯 CPU） |
| 四 | 骨骼 / BindPose / 权重 / 曲线 / 30fps 重采样 / GPU 蒙皮 | ✅ | 复用既有 `Skeleton` + `pbr.vert`；本轮只加**抽帧** |
| 五 | PBR 材质 / stb_image / 贴图路径容错 | ✅ | 复用既有 `Material`；路径四级回退见 §6 坑 8，**运行期容错是这一轮才真正通**（坑 10） |
| **六** | **二进制格式 / 四个区块 / 缓存重排 / 动画压缩** | ✅ **本轮** | `assetio/MeshFormat.cpp`（738 行） |
| **七** | **加载器 / Vulkan 资源 / 缓存与回退** | ✅ **本轮** | `engine/assets/VMeshLoader.cpp`（258 行） |

**实测（anime_girl.fbx，1.05 MB）**

| 指标 | FBX 直读 | `.vmesh` | 变化 |
|------|---------|----------|------|
| 顶点 | 43808 | **11338** | −74% |
| 索引（三角形） | 65712（21904 tri） | 65712 | **一条不多一条不少** |
| ACMR（缓存命中，完美 0.5 / 最差 3.0） | 2.000 | **0.781** | 好于 1.0 |
| 动画关键帧 | 7812 | **651** | −92%，最大误差 0.0969° |
| 几何原始字节 | 3.59 MB | **1.01 MB** | **3.55x** |
| 加载耗时 | 43.3 ms | **16.0 ms** | **2.7x** |

自检 `MYVK_ASSET_TEST=1` → `ASSET-TOTAL pass=44 fail=0`；
端到端 `tools/verify_vmesh.py` → **34 条全 PASS**（A 离线 8 / B 工具 7 /
C 运行时 11 / **E 贴图容错 5** / D 引擎内自检 3）。

---

## 1 · 分层：为什么是 `assetio/` + `assetc/`，不是塞进 `engine/assets/`

既有依赖方向是硬约束（`docs/ship_roadmap.md` §13）：

```
runtime/ ──→ engine/ ←── editor/
```

离线编译器**必须能无显卡运行**（CI 机器上烤资产），而 `engine/` 一层
把 Vulkan 拖进来了 —— 一旦 `assetc` 链接 `VennEngine`，它就再也离不开显卡。
于是新增第三层，并且**只依赖 glm 和 ufbx**：

```
            ┌──────────────┐
runtime/ ──→│   engine/    │←── editor/
            └──────┬───────┘
                   │ PUBLIC
            ┌──────▼───────┐
            │  assetio/    │  ← VennAssetIO（静态库）
            └──────┬───────┘
                   │ PRIVATE
              glm + ufbx

            ┌──────────────┐
            │   assetc/    │  ← AssetC.exe，**只**链 VennAssetIO
            └──────────────┘
```

**硬边界**：`assetio` 不认识 Vulkan，也不认识 `engine/`。
落地手段是**顶点结构体双向锁死**，任何一边改字段立刻编译失败：

```cpp
// engine/assets/VMeshLoader.cpp
static_assert(sizeof(assets::Vertex) == sizeof(assetio::Vertex), "顶点布局必须逐字节一致");
static_assert(sizeof(assets::Vertex) == assetio::kVertexSize, "80 字节契约");
```

另一边 `assetio/Vertex.h` 里 6 条 `offsetof` 断言把**字段顺序**也钉死 ——
只锁 `sizeof` 是不够的，把 `uv` 和 `joints` 互换位置仍然全是 80 字节。

---

## 2 · `.vmesh` 格式规格

### 2.1 为什么是区块表，而不是顺序拼接

顺序拼接（一路 `write(mesh); write(skel); write(anim);`）的问题是**报错说不清**：
文件截断和"版本不对"都是"读到一半失败"，用户拿不到可行动的信息。
区块表让这两件事分开：

```
┌─────────────────────────────────────────────────────────┐
│ 文件头 32 B                                             │
│   magic   8  "VNMESH\x1A\0"   ← \x1A 防文本模式误读     │
│   version 4  kMeshFileVersion = 1                       │
│   flags   4  Skinned(1) Animation(2) Optimized(4) Index16(8) │
│   sourceHash 8  FNV-1a 64（源 .fbx 内容）               │
│   sectionCount 4                                        │
│   reserved 4                                            │
├─────────────────────────────────────────────────────────┤
│ 区块表 N × 32 B                                         │
│   id 4 | version 4 | crc32 4 | offset 4 | size 4 | pad 12 │
├─────────────────────────────────────────────────────────┤
│ MESH 区块（8 字节对齐起步）                              │
│ SKEL 区块                                                │
│ ANIM 区块                                                │
│ MAT  区块                                                │
│ STR  区块                                                │
└─────────────────────────────────────────────────────────┘
```

区块 id：`MESH=1 SKEL=2 ANIM=3 MAT=4 STR=5`。
`SKEL / ANIM / MAT` 在对应数据为空时**整块不写**（静态网格就是只有 2 块），
读端按"有这个 id 才解析"处理。

### 2.2 文件级 version 与区块 version 必须分开

这是格式设计里最容易做错的一处。合成一个版本号的后果：

> 材质加了一个 `clearcoat` 字段 → 版本 1→2 → **所有模型重烤**（几何一个字没变）。

拆开之后语义清晰：

- **文件级 version 变了** → 老读端**直接拒**（`readMeshFile` 报
  `"unsupported .vmesh version %u (expected %u)"`）。因为布局可能整体挪了。
- **区块 version 变了** → 只影响用这个区块的那一方。你可以让老读端
  "MESH 认 v1、MAT 只认 v2，遇到 MAT v2 就退化成默认材质"，而几何照读。

### 2.3 CRC32 必须**真的校验**，否则只是装饰

第一版写端算了 CRC 写进表，读端 `r.u32()` **直接丢掉** —— 等于零防护。
现在 `readMeshFile` 在解析任何区块**之前**先全量校验，错误信息精确到区块名：

```
section MESH crc mismatch (file corrupt)
```

但要说清楚 CRC 的能力边界：**它只能证"字节没坏"，不能证"内容合法"**。
所以每个区块解析时仍然要做范围检查 —— 所有长度字段**先与区块长度对账**，
再 `resize`。顺序反了就是一次 `bad_alloc` 或者越界读：

```cpp
// 先对账，再分配
if (count > sec.size / kVertexSize) return fail("MESH vertexCount 超出区块容量");
verts.resize(count);
```

### 2.4 子网格目录定长 112 字节

```
nameStr(4) material(4) vertexCount(4) indexCount(4)
ofsVertices(4) ofsIndices(4)          ← 相对 MESH 区块起点
boundsMin(12) boundsMax(12)
transform(64)                          ← mat4，逐网格的节点变换
```

定长的好处是 `--info` 可以**不解析几何**就跳着走完目录。
代价是"跳步必须与写端逐项对齐"，这一条真的栽过（§6 坑 4）。

### 2.5 STR 区块：名字去重

所有名字（子网格 / 材质 / 骨骼 / 动画片段 / 贴图路径）都进一张字符串表，
其余区块存**下标**。原因不只是省字节：`.vmeta` 的材质映射是**按名字查**的
（`resolveMaterialId`），表里同名字必然同下标，查表逻辑不用处理重复。

### 2.6 包围盒**不落盘**

`boundsMin/Max` 每个子网格都存了（绘制剔除要用），但**整体 AABB 不存** ——
它完全可以由子网格 AABB × transform 推导。存两份只有一个后果：某天忘了同步。

---

## 3 · 离线修正层：每一步为什么存在

### 3.1 焊接（`weldVertices`）

`positionEpsilon = 1e-5`，但**其他字段一律不许放宽**：

| 字段 | 容差 | 理由 |
|------|------|------|
| position | 1e-5（按 eps 量化，吃浮点噪声） | DCC 导出的"同一个点"常有 1e-7 抖动 |
| normal | **1e-3（≈0.06°）** | **硬边不许焊** —— 放宽到 1e-2 就把折边焊成圆角 |
| uv | 1e-6 | 缝线两侧 UV 不同必须分开，否则贴图拉花 |
| weights | 1e-5 | |
| joints / color | **精确** | 蒙皮索引错一位 = 顶点被别的骨头拽走 |

哈希用 splitmix64 打散后分桶，但**命中后仍然逐字段比较** —— 哈希只负责缩小
候选集，相等性判定不能交给哈希。

> ⚠ 判断焊接"生效"不能用 `vout < vin`：立方体本来就没有重复顶点，那是**模型的
> 性质**不是链路缺陷。自检因此改用**不变性**判据：焊完之后不允许还存在两个
> 完全相同的顶点（`weld_no_duplicates`）+ `weld_not_more_vertices`。

### 3.2 重算法线（面积加权）

按三角形面积加权累加面法线再归一化。**只对"位置相同但法线不同"的顶点生效**
（即焊接时被刻意保留的硬边顶点），否则会把 DCC 里精心调的软边法线抹平。

### 3.3 Forsyth 顶点缓存重排（`optimizeForCache`）

**ACMR**（Average Cache Miss Ratio）= **顶点着色次数 / 三角形数**，直接决定顶点
着色器的工作量。完美复用时每个顶点只着色一次、而它被多个三角形共享 → 趋近 0.5；
完全不复用时每个三角形都得重新着色 3 个顶点 → 3.0。

模拟方式：`t - lastUse[v] >= 32` 就近似判成已被 FIFO 挤出（32 = 常见后变换缓存
容量）。这个近似不精确，但**同一个模拟器同时量"重排前"和"重排后"**，所以
两个数字之间的比较是可信的 —— 不要拿它跟别的引擎的绝对数字比。

Forsyth 算法的骨架是**惰性堆 + 分数重算**：每次弹出分数最高的顶点，出堆后
重新算分，分数变差就**放回堆里**而不是硬取。少了这一步，重排会退化成
"贪心一次" ，ACMR 从 0.78 反弹到 1.4 左右。

### 3.4 动画：误差有界抽帧，而不是量化

这是本轮最重要的一个取舍。

**为什么不是量化**：运行时采样器 `assets::sampleTrack` 吃的是 `float`。
要量化（比如旋转存 16 bit）就必须**同时改采样器**和 `AnimTrack` 的存储类型。
那是一次跨层的接口变更，不该夹在"引入资产链"这一轮里做。接口已经留好位置
（`SourceTrack.path` 是 int、`reduceTrack` 带三个容差参数）。

**为什么抽帧是安全的**：它做的是**纯删冗余** —— 只删掉那些"删了以后插值
结果仍然在容差内"的关键帧，运行时零改动。默认容差 0.1°（旋转）/ 1e-4（位移、
缩放）。

三条实现纪律，每条都对应一个真实的坑：

1. **误差评估必须用运行时同一个插值**（`maxErrorInSpan` 里旋转走 slerp、
   其余线性）。用"相邻帧差值"当误差的话，离线算 0.05°、运行时插出 2°。
2. **`quatAngleDeg` 必须取 `|dot|`**。`q` 与 `-q` 是同一个旋转；不取绝对值时，
   一对符号翻转的相邻帧会被判成 180° 误差 → 压缩率恒为 0，而且看着"很保守"
   （不报错、只是什么都不删），最难查。
3. **末帧逐位保留、`duration` 不动**。循环播放按 `duration` 回绕，末帧被抽掉
   就是每次循环抖一下。

---

## 4 · 运行时：缓存与三条过期判据

### 4.1 场景文件里引用的**永远是 `.fbx`**

```
assets/models/girl.fbx       ← 场景 / .vmeta 里写的
assets/models/girl.vmesh     ← 派生物，可以随时删
```

`AssetManager::loadFBX` 接到 `girl.fbx` 时先探同名 `.vmesh`。
好处是**老存档零改动**，而且 `.vmesh` 丢了只是变慢，不会变坏。

### 4.2 三条过期判据

`vmeshIsFresh()` 依次看：

| 判据 | 含义 |
|------|------|
| `.vmesh` 不存在 | 首次运行 / 被删 |
| `mtime(.fbx) > mtime(.vmesh)` | 源改了 |
| **`mtime(.vmeta) > mtime(.vmesh)`** | **导入参数改了** |

第三条最容易漏。`import.scale` / `generateLods` / `importAnimations` 都是
**烘进 `.vmesh`** 的，改 `.vmeta` 而不重烤，用户会看到"比例改了但模型没变"，
而且没有任何报错。这一条是专门为它加的。

### 4.3 降级而不是报错

| 情况 | 行为 | 日志 |
|------|------|------|
| 缓存缺失 / 过期 | 回退 FBX 解析 | `INFO MESH-BIN-STALE` |
| `.vmesh` 解析失败（损坏 / 版本不符） | **降级**回 FBX | `WARN MESH-BIN-FALLBACK` |

第二行是刻意的：一个坏掉的派生文件**不该让工程打不开**。
日志级别也分了 —— 过期是正常现象（INFO），损坏是异常（WARN），
这样 `grep WARN` 才有信噪比。

### 4.4 加载器的接线细节

- 顶点**按字节 memcpy**（靠 `static_assert` 保证安全），不走逐字段赋值
- `MeshSource::kind = FBX`、`path = sourceRelPath`（存 `.fbx` 不存 `.vmesh`）——
  让 Inspector / 重新导入拿到的仍然是源路径
- 材质走既有的 `beginModelImport` / `resolveMaterialId` / `captureImportDefaults`
  / `finishModelImport` 四步，与 FBX 直读那条路**共用**同一套映射逻辑，
  这样 `.vmeta` 的材质覆盖在两条路上行为一致

---

## 5 · 怎么用

### 5.1 离线烤

```bat
build\AssetC.exe assets\models\girl.fbx
build\AssetC.exe -r assets\models            :: 整个目录（输出顺序已排序）
build\AssetC.exe --info assets\models\girl.vmesh
build\AssetC.exe --check assets\models\girl.vmesh
```

| 选项 | 作用 |
|------|------|
| `-o <path>` | 指定输出路径（默认同名 `.vmesh`） |
| `--asset-root <dir>` | 贴图相对路径的基准（默认工程 assets 根） |
| `--scale <f>` | 额外缩放（在 `.vmeta` 的 import.scale 之上再乘） |
| `--no-weld` | 关焊接 |
| `--no-optimize` | 关缓存重排 |
| `--no-anim-reduce` | 关抽帧 |
| `--rot-tol <deg>` | 抽帧旋转容差（默认 0.1） |
| `-r` / `--recursive` | 目录递归 |
| `--info` / `--check` | 只体检 / 校验 CRC 与结构，不写盘 |
| `-q` | 静默 |

输出行是**给脚本解析的契约**，改格式必须同步 `verify_vmesh.py`：

```
ASSETC OK <in>
ASSETC   submeshes=7 joints=21 clips=2 materials=7
ASSETC   vertices 43808 -> 11338  indices 65712 -> 65712  acmr 2.000 -> 0.781
ASSETC   keys 7812 -> 651
ASSETC   geometry 3.59 MB -> file 1.01 MB (3.55x) in 102.4 ms
ASSETC   wrote <out>
ASSETC TOTAL files=1 fail=0 geometry=3.59 MB output=1.01 MB
```

#### 5.1.1 `FBX-CLIPDUP`：take 名归一化之后的重名去重（2026-10-28，#329）

Blender 的 `bake_anim_use_all_actions` 会把 take 名写成 `<armature>|<action>`，
读入时一律剥掉 `|` 前缀（Unity / UE / glTF 转换器都这么做）。剥完之后**可能撞名**：
`anime_girl.fbx` 里就有 4 个 AnimationStack —— `Rig|Idle` ×2 + `Rig|Walk` ×2，
归一化后是 `[Idle, Walk, Idle, Walk]`。撞名的代价不是"列表难看"：

- `Skeleton::findClip(name)` 逐字取第一个匹配 → 第二段永远播不到；
- 角色窗口的片段列表两行同名 → ImGui 判 ID 冲突 → 弹**模态**告警窗，
  之后**所有鼠标点击全被它吃掉**（界面看着正常、点哪都没反应）。

规则（`assetio/ClipDedup.h`，`AssetC` 与引擎 `FbxLoader` **共用同一份实现**）：

| 情况 | 处置 | 理由 |
|------|------|------|
| 同名 **同时长**（容差 1e-4 s） | **丢弃**后一段 | 同一动作被导出了两遍；时长相同是最强的证据 |
| 同名 **不同时长** | **改名** `<name>_2`、`_3`… | 是撞名不是重复，丢数据不可逆，改名只是难看 |

决策发生在**逐帧采样之前**（丢掉的段不必再算 61 帧 × N 骨 × 3 通道）。
`AssetC` 与引擎各打一行（同一段文字，`clipDedupLine()` 生成）：

```
FBX-CLIPDUP take='Rig|Idle' name='Idle' dur=1.0000 action=drop (same name & same duration -> duplicate export)
FBX-CLIPDUP take='Rig|Walk' name='Walk' dur=1.0000 action=rename to='Walk_2' (same name, different duration -> keep both)
```

⚠ `anime_girl.vmesh` 因此从 `clips=4` 变成 `clips=2`（几何/骨骼/材质一字节没动，
只有动画段少了一段重复）。`verify_vertex_format.BASE` 已同步这一项。

### 5.2 引擎内自检（无窗口、不开 Vulkan）

```bat
set MYVK_ASSET_TEST=1
build\Editor.exe
:: ASSET-TOTAL pass=44 fail=0
```

挂载点在 `editor/main.cpp` 里，**在 Vulkan 初始化之前** `return`，
所以能在没显卡的机器上跑。换模型用 `MYVK_ASSET_TEST_MODEL`。

断言分六组：A 导入 / B 几何修正 / C 往返 / D 动画压缩 / E **坏文件必须拒**
/ F 确定性 + 体检。E 组和 F 组的"两次编译逐字节相同"是这套自检里最有价值的
部分 —— 前者守"不要相信任何来自磁盘的长度字段"，后者守**可复现构建**。

自检有第三种结果 `NA`（既不 PASS 也不 FAIL）：静态网格没有骨骼，
`anim_keys_reduced` 这类断言对它**不适用**，判 PASS 是自欺、判 FAIL 是误伤。

---

## 6 · 踩坑清单（全是真实红过的）

1. **`GLM_ENABLE_EXPERIMENTAL` 未定义** → `gtx/matrix_decompose.hpp`、
   `gtx/transform.hpp` 直接 `#error`。engine 侧各 `.cpp` 是从 `rhi/VulkanCommon.h`
   间接拿到这个 define 的，而 `assetio` 刻意不 include rhi 头 →
   **必须在 `#include <glm/glm.hpp>` 之后、其他 glm 头之前自己 define**。
2. **`ufbx_material` 没有 `feature_count` / `features[i]`** → 真名是
   `mat.features.features[]` 或 `mat.features.double_sided.enabled`。
3. **`mat.pbr.normal_map` 不存在** → 法线贴图在 `mat.fbx.normal_map`。
   （`pbr` 结构里的 bump 是灰度高度图，语义不同，不能顶替。）
4. **`--info` 读出顶点数 4275971567** → 目录步进多跳了 24 字节。
   写端 `kDirEntry = 112` 拆开是 `4+4+4+4 4+4 12+12 64`，剩余跳步必须是
   `8+12+12+64 = 96` 而不是 `8+24+24+64`。**注释里写清"必须与写端逐项对齐"**。
5. **压缩比 1.10x 是假数字** → `rawGeometryBytes` 读的是 `sm.vertices.size()`，
   而 `optimizeSubMesh` 是**原地改**的 —— 读到的是已焊已重排的数据。
   改用优化前快照 `os.verticesIn / os.indicesIn` 后变成真实的 3.55x。
6. **ACMR 恒虚高**（立方体报 5.000，而量纲上限是 3.0）→
   `WriteStats::acmrIn/acmrOut` 默认值写成了 `3.0f` 而不是 `0.0f`，
   而写盘时是 `+=`，于是每个模型凭空多 3.0。
   **教训：被统计依赖的数字，默认值就是契约的一部分。**
7. **自检 `rt_vertex_bytes` 恒红** → 断言假设"顶点顺序不变"，但焊接会**删掉**
   重复顶点、下标整体前移。改成**集合包含**语义：每个读回的顶点必须逐字节
   存在于源里（FNV-1a 64 分桶 + `memcmp` 确认），另加 `rt_vertex_count_sane`。
8. **贴图路径失效**（同级目录 / 大小写 / 只给文件名）→ `resolveTexture` 四级回退：
   资产根相对 → 原样绝对 → FBX 同目录相对 → 只取文件名（同目录 / `textures/`）。
   全部失败时按 `albedoRaw` 再试一次并打 `VMESH-TEX-MISSING`，走默认材质。
9. **`static_cast<long>` 截断向量** → 在 Windows 上 `long` 是 32 位。
   改用 `std::ptrdiff_t`（一处 `resize` 的越界，症状是随机崩溃）。
10. **"贴图缺失 → 默认材质" 是死代码**（本轮 E 组抓出来的**真 bug**）。
    `VMeshLoader` 里写着 `if (!albedo && ...) VK_LOG_WARN("VMESH-TEX-MISSING")`，
    但 `loadTexture` 在文件打不开时**直接抛**（`Texture.cpp:665`
    `throw std::runtime_error`），异常一层层冒到 `loadFBX` 的 catch 里 ——
    于是实际行为是**整个模型加载失败、实体变空**，那条 WARN 一次都不会打：

    ```
    [ERROR] Texture load failed: Skin (can't fopen)
    [WARN ] SceneSerializer: FBX load failed (models/anime_girl.fbx): Failed to load texture: Skin
    ```

    修法：`AssetManager::tryLoadTexture()`（catch 后返回 `nullptr`），
    `VMeshLoader` / `FbxLoader` 的**所有**资产期贴图读取都改走它。
    教训：**"写了容错分支"不等于"容错生效"** —— 只要上游会抛，`if (!ptr)`
    就是不可达代码。这种死分支编译器不会报警，只能靠用例去踩。

---

## 7 · 这一轮**没有**做的，以及为什么

| 项 | 状态 | 为什么先不做 |
|----|------|-------------|
| **4.5 贴图内嵌（BC7 / ASTC）** | 未做 | 工程里已有 `tools/make_bc_dds.py` 与 Texture 的 DDS 路径，但把压缩纹理塞进 `.vmesh` 会让一个模型文件从 1 MB 涨到几十 MB，并且需要和 Texture 的格式协商（BC7 vs ASTC 是**平台选择**，不该在离线烤时就定死）。当前 `.vmesh` 只存**路径**，压缩纹理走既有的 `.dds` 旁路。 |
| **4.7 量化** | 只做了抽帧 | 量化必须**同时改运行时采样器**（`AnimTrack` 是 float 的），那是跨层接口变更。抽帧已经拿到 −92%，先落地。容差参数已留位置。 |
| **6.1 GPU 蒙皮** | ✅ 已有 | `pbr.vert` 里 `jointBuf` SSBO + `push.skin.x`，本轮没动。 |
| **6.2 LOD** | ✅ 已有 | `engine/assets/Lod.cpp` + `Mesh::lodChain`。 |
| **6.3 实例化** | ✅ 已有 | `MdiDraw` + `Instancing`，`verify_instancing` 在看。 |
| **6.4 动画混合** | 未做 | 属于 `docs/character_pipeline_roadmap.md` 的 **A2**（状态机 + crossfade），不是资产链的问题。 |
| **6.5 异步加载** | 未做 | 需要 Job 系统先落地。当前 16 ms 的加载耗时还在"一次性卡一下能接受"的范围内。 |
| **6.6 内存映射** | 未做 | 单个 `.vmesh` 1 MB，`mmap` 的收益打不过复杂度。等有 200 MB 的大场景再说。 |

---

## 8 · 回归登记

`tools/verify_vmesh.py` 已登记进 `tools/run_regression.py`：

- `DEFAULT`：**94 条**（原 93 + 本条）
- `TRIGGERS`：`["assetio/**", "assetc/**"]`

**只登记了非横切的这两块**。`engine/assets/VMeshLoader.*` /
`AssetSelfTest.*` / `MeshLoader.*` / `FbxLoader.*` 全部落在
`engine/assets/**` —— 那是 `GLOBAL_GLOBS`，改它们**本来就全量**；
写进 `TRIGGERS` 会在 `--narrow` 下变成"看着有归属、其实没覆盖"的假映射。

> **另一个洞（本轮顺手补的）**：`assetio/` 和 `assetc/` **原本不在
> `WATCH_ROOTS`**。不进快照 = 影响分析里**完全看不见** —— 改了
> `MeshFormat.cpp` 的一个字段偏移，`verify_vmesh` 连"变了"都不知道。
> 这和当初 `runtime/`、`launcher/` 漏进 `WATCH_ROOTS` 是同一个错误。

`tools/check_regression_map.py` 的 A 组加了两条 fixture
（`assetio/MeshFormat.cpp` / `assetc/main.cpp` → 恰好命中 `verify_vmesh`），
让"表腐化"这件事有红字可看。当前结果：**PASS**。

**E 组（贴图容错）的做法值得单独记一笔**：它不去造"带贴图的模型"（那要
Blender，仓库里的 `anime_girl.fbx` 根本没有外部贴图），而是拿烤好的
`.vmesh` **把每条材质的 `albedoPath` / `albedoRaw` 下标改成它自己的名字**
（名字一定非空、一定不是真文件），然后**重算 MAT 区块的 CRC32**。

关键在最后半步：文件必须**仍然合法**。用"损坏文件"去测"缺贴图"，测到的
是另一条分支（`MESH-BIN-FALLBACK`），而真正的现场是"文件好端端的，只是
贴图被人删了 / 漏拷了"。这条用例第一次跑就抓出了坑 10 那个死代码。
