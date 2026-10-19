# 世界规模与内容生产 —— 方案与执行路线图（W 系列）

> 回答的问题（用户的原始提问，四块）：
> ① 地形 / 水体 / 植被 / 动态天空全无（R6-2~R6-4 未做）；天空是静态程序化，无大气散射。
> ② 没有烘焙：无 lightmap、无 DDGI（R5-2/R5-3）。全靠实时 CSM + SSAO/SSGI 近似撑着，
>    室外还凑合，室内和大世界会直接崩。
> ③ 没有材质实例：材质参数逐物体存，一个关卡几百件东西时，改一次 roughness 要点几百次。
> ④ 内容侧：`assets/` 一共 3.8 MB，只有一个 sandbox 场景。第一个真实角色 + 第一个真实关卡
>    会把导入器的坑全暴露出来（Z-up 转换、材质槽、LOD、骨骼命名）。
>
> 本文档的定位：**先量、再定序、再给设计**。凡是"实测"字样都附了文件行号或命令，
> 凡是判断都写了"为什么不是别的"。上承 `ship_roadmap.md` §8（G21~G25），
> 但**主序被改了**（见 §2）——改的理由是实测，不是偏好。

---

## 0 · 一句话结论

**这四块不是四件并列的事，是一条断在最前面的链。**

链的形状是：

```
母材质（可复用、可统一调参）
   └→ 材质实例（关卡里几百处引用 + 少量覆盖）
        └→ 真实关卡（几百件东西 + 一个室内）
             ├→ 导入管线（模型进来时材质身份要稳）
             ├→ 地形（地面）
             ├→ 植被 / 水体（地面上的东西）
             ├→ 动态天空（光照来源）
             └→ GI 烘焙（室内的存在前提）
```

**现在断在最前面的两环上**：材质身份不稳（实测 3 个 bug，§1 的 F1~F3），
以及没有"实例"这层概念。在这一环修好之前做地形，结果是"地形做出来了，
上面摆的每一件东西的材质还是要逐个调"——那正是用户抱怨的那件事。

> 换句话说：**地形是"有没有地面"，材质实例是"能不能摆东西"。**
> 后者是前者的前提，不是并列项。

---

## 1 · 实测：当前真实状态

> 纪律：以下每一条都在磁盘上量过。**凡与本轮提问原话不一致的，以实测为准，并在文中标明。**

### 1.1 修正用户原话里的三处（都不成立或已解决）

| # | 原话 | 实测 | 影响 |
|---|---|---|---|
| **C1** | "Z-up 转换"是会暴露的坑 | **已解决**。glTF 手动转（`MeshLoader.cpp:674-684`：Y-up→Z-up，decompose 后重算 Z-up 姿态）；FBX 用 `opts.target_axes = ufbx_axes_right_handed_z_up`（`FbxLoader.cpp:503`） | 这条**不用做**。W3 只需回归验证，不需要新代码 |
| **C2** | "LOD" 是坑 | **已有完整实现**：`assets/Lod.{h,cpp}`（345 行），4 级、屏幕占高比阈值 `{1.0, 0.25, 0.10, 0.04}`、迟滞 1.15、顶点聚类简化。**唯一缺口：蒙皮网格不生成 LOD**（`Lod.h` 文件头写明理由：减面会丢 `Vertex::joints/weights`） | 这条从"缺失"降级为"**蒙皮 LOD 缺失**"，是 W2-3 的一小块 |
| **C3** | "几百件东西"是性能问题 | **不是**。实测 50 / 200 / 500 个盒子：总耗时 2.55 / 2.62 / 2.60 s（60 帧），GPU `frame=4.39 / 4.74 / 4.55 ms`。**物体数 ×10，耗时不变** | 性能不是"世界规模"的瓶颈。瓶颈是**编辑体验**与**功能缺失** |

> ⚠ **C3 的测法**（复现用）：生成 N 个 `primitive: box` + 独立材质名的场景，
> `MYVK_GPU_TIMING=1 Game.exe --scene <N>.json --frames 60`，读 `GAME-LOOP-SUMMARY` 与
> `GPU-TIMING frame=`。数据在 §1.5。**结论只在"小盒子 + 集显 + 1280×720"这个区间成立**，
> 换成 10 万面模型 / 4K 分辨率要重测。

### 1.2 材质：三个已实测的 bug（这是本方案的第一优先级）

#### F1 · 同名材质**静默合并**，后者参数被完全丢弃

```
探针（已入库：assets/scenes/runtime_test/mat_conflict_probe.json）
两个 Box，材质名都叫 "shared_mat"，A 是 rough=0.2/橙色，B 是 rough=0.8/蓝色
实测 dump：A | name=shared_mat | rough=0.2 | base=橙
           B | name=shared_mat | rough=0.2 | base=橙     ← B 的参数消失
日志：无任何 WARN
```

根因在 `AssetManager.h:476-477`：

```cpp
auto it = m_materials.find(name);
if (it != m_materials.end()) return it->second.get();   // ← 直接返回旧的，参数全丢
```

**为什么这是致命的**：复制粘贴实体（Ctrl+D）是最常见的编辑动作。如果复制时材质名没被
唯一化，第二个及以后的所有副本会**静默共用第一份材质**；而用户看到的是"我改了 B 的
roughness，A 也变了"——或者更糟，"我改了 B，什么都没变"（改的是 A 那份）。

⚠ 顺带：这**恰好也是"材质已经共享"的反证**——现在"逐物体存"是**假象**，
靠"每个实体起一个唯一材质名"维持。一旦重名就串味。

#### F2 · 材质身份是**数组下标**，不是名字

```
glTF 里 7 个材质叫 Skin / Hair / Cloth / EyeWhite / Iris / Shoe / ...
引擎里它们叫 models/anime_girl.glb/mat0 ... /mat6      （MeshLoader.cpp:365）
```

```cpp
const std::string key = base + "/mat" + std::to_string(matIdx);
```

后果：
- Inspector 的 `name:` 显示的是 `models/anime_girl.glb/mat0`，**美术看不懂**
- 模型重导出时材质顺序一变 → 所有引用**静默错位**（mat0 从 Skin 变成 Hair，没人报错）
- 这也解释了为什么"材质槽"在用户眼里是坑：**槽位的身份是位置，不是语义**

#### F3 · 导入模型的材质修改**不能持久化**（实测丢失）

```json
// 写端 SceneSerializer.cpp:162-166 —— kind=="model" 只写两行
if (mat->source.kind == assets::MaterialSource::Kind::Model) {
    j["kind"] = "model";
    j["modelKey"] = mat->source.modelKey;
    return j;                      // ← roughness / baseColor / 所有参数都不写
}
```

探针（场景里给 Skin 写 `roughness: 0.05`、`baseColor: [1,0,0,1]`）：

```
实测 dump：GirlMesh0  material = null        ← 整个材质块变成 null
```

两层问题叠在一起：
1. **覆盖值根本不会被写出**（上面那段 `return`）
2. `resolveMaterial` 的 model 分支 `am.findMaterial(name)` 找不到时**返回 nullptr 且不打日志**
   （`SceneSerializer.cpp:205-229`，兜底加载后仍找不到就 `return m`，m 是 null）

**这意味着：今天在编辑器里把导入模型的任何材质参数调一下、存盘、重开，改动全部丢失，
而且物体可能直接变成"无材质"（走默认白）。** 这是"第一个真实角色"会撞上的第一堵墙。

### 1.3 材质上 GPU 的路径（决定了"实例"能做到多便宜）

| 项 | 现状 | 位置 |
|---|---|---|
| 材质**参数** | 走 push constant（`baseColorFactor` / `pbr` / `emissive`，共 48 字节） | `RenderTypes.h:257-268` |
| 材质**纹理** | 每张纹理一个 descriptor set（set 1..7） | `RenderTypes.h:298-313`，`kTextureSetCount = 7` |
| bindless（R2-3） | **已实现**（`render/Bindless.h`，纹理数组 + 材质页表），`pbr.bindless.frag` 变体已有 | `Renderer.cpp:3780` |
| bindless 默认 | **关**（`m_bindlessEnabled = false`，`Renderer.h:1249`） | — |
| 本机能力 | `maxBoundDescriptorSets=32`、`maxPerStageDescSamplers=64`、**`descriptor indexing available`** | 实测 `UI2-CAPS` 行 |

⚠ **这条推翻了我记忆里的一条旧笔记**（"Intel Iris Xe → maxBoundDescriptorSets=8，卡在线上"）。
本机实测是 **32**，且 descriptor indexing 可用。所以"加 set 会炸"这个前提不成立，
但**低端设备（Mali/Adreno/部分 AMD）确实只有 8** —— 所以仍然不该无脑加 set，
正确路径是**开 bindless**（它本来就是为了解决这件事而写的）。

> 这也是一条好消息：材质实例不需要新机制。**参数走 push constant，每 draw 本来就要推一遍，
> 所以"实例"在 GPU 侧是零成本的**——它纯粹是 CPU 侧的数据组织问题。

### 1.4 光照：为什么"室内会崩"（用户判断是对的，这里是机制）

| 通道 | 现状 | 位置 | 室内表现 |
|---|---|---|---|
| 环境光（漫反射） | **一张全局 irradiance cubemap**，对天空做余弦卷积，`init` 时生成一次 | `Environment.h:34-40`、`Environment.cpp` | **室内也被天空照亮** → 灰、漏光、没有墙角变暗 |
| 镜面环境 | 全局 prefilter cubemap + BRDF LUT | 同上 | 同上 |
| 间接光（屏幕空间） | **SSGI-lite**：12 样本、**无 albedo 调制**、**无时间累积**、加色注入 | `fg_ssgi.frag:1-20` | 只能看到屏幕内的东西；相机一转就闪 |
| 方向光遮挡 | CSM 4 级 | `ShadowPass.h:137` | 光照穿墙（**所有实时引擎的通病**，不是 bug） |
| 反射探针 | `ReflectionProbe`（R5-1）已有，`maxProbes=4` | `ReflectionProbe.h:97` | 可用，但**捕获不含动态物体**，且 `blendDistance` 未实现（相邻探针有硬接缝） |

**结论**：室内"崩"的根因不是 SSGI 差，是 **`irradiance` 只有一张全局图**。
补法有两条，成本差 10 倍：

- **便宜（W6）**：把 `ReflectionProbe` 的捕获结果**同时**用作辐照度源（它已经烘了 cube array），
  室内放一个探针 → 该体积内环境光换成室内捕获结果。**不改 `pbr.frag` 的纹理槽**
  （探针 cube array 已经在 binding 23 了，MEMORY 记着"缺省必须绑 1×1 全黑 cube array"）。
- **贵（W7）**：真正的 GI 烘焙（辐照度探针网格 SH / lightmap）。

### 1.5 世界规模：容量实测

| 指标 | 实测值 | 判定 |
|---|---|---|
| 500 物体，60 帧总耗时 | 2.60 s（50 物体是 2.55 s） | 物体数不是瓶颈 |
| GPU `frame=`（500 物体） | **4.55 ms** | 有余量 |
| GPU 分段（500 物体） | shadows 0.78 / depth 0.82 / forward 1.13 / post 1.06 | 均匀，无单点 |
| `dropped` | 15/60（受 `maxFps=60` 与首帧上传影响，非掉帧证据） | — |
| MDI 上限 | `kMaxObjects = 8192` | 够 |
| 光源上限 | `kMaxLights = 512` | 够 |
| 顶点结构 | 64 B（pos/normal/uv/joints/weights），**无顶点色、无 UV2** | ⚠ 植被风没地方放权重（见 W5） |
| 纹理槽 | 7 个（albedo/normal/orm/rough/metal/emissive/height），**已满** | ⚠ 地形 splat 装不进去（见 W4） |

### 1.6 内容侧：`assets/` 到底有什么

```
assets/ 共 5.5 MB（用户记的 3.8 MB 是加音频之前的数字；音频 1.1 MB 是上一轮 G14 加的）
  audio/     1.1 MB   7 个 wav 夹具（自检用）
  icons/     724 KB
  models/    2.1 MB   anime_girl.fbx(1.0M) / .glb(196K) / box01.glb / test_cube.fbx / Cube/
  scenes/     36 KB   sandbox.json + runtime_test/ 下 3 个 mini 场景
  shaders/   409 KB
  textures/  576 KB   只有 bc1_test / bc3_test / sss_lut —— **没有一张真实材质贴图**
```

**真实模型其实已经在仓库里**（修正"第一个真实角色还没来"）：

```
anime_girl.glb: 1 mesh / 7 materials / 23 nodes / 1 skin / 2 anims(Idle, Walk) / 0 embedded images
  材质名：Skin / Hair / Cloth / EyeWhite / Iris / Shoe / ...
```

三个信号：
1. **7 个材质槽**，全部是纯因子（`images=0`）——所以"材质槽的坑"不是数量，是**身份**（F2）
2. **2 段动画**（Idle / Walk）——角色链 A0 已经把蒙皮跑通了
3. **没有一张材质贴图** —— 所以"真实角色"这一关其实**过了**（用纯色因子跑起来的），
   **真正没过的是"真实关卡"**：`scenes/` 里**没有任何一份引用真实模型的关卡**（实测：
   `player_scene.json` 等全用 procedural 材质 + 内置图元）

### 1.7 导入管线：现状是"复制文件"

```cpp
void ContentBrowser::importFiles() { m_importRequest = true; }   // ContentBrowser.cpp:1173
// → openImportDialog() 用 GetOpenFileNameW 选文件 → 复制到 assets/
```

- **无 `.meta` / 无导入设置**：缩放、轴向、LOD 参数、材质映射**无处存**
- **无重导入策略**：模型更新了，手工改过的材质参数**会不会被覆盖**——没有定义
- **`Add ▸` 的 13 种类型里只有 2 种有加载器**（`ContentBrowser.cpp:1129-1135` 自己写明了）：
  `water` / `foliage` / `worldmap` / `animation` / `cinematic` / `media` / `physics` / `ui`
  点下去只是落一个 `{"type":...,"version":1}` 空文件。
  ⚠ 这与项目的**特性准入纪律**（五件套：实现 + FeatureRegistry + UI 面 + 序列化 + 用例）
  冲突：**UI 面已经承诺了 8 种能力，底下是空的**

### 1.8 地形 / 植被 / 水体：字面意义上的零

```
grep -r "terrain|Terrain|vegetation|foliage|grass" --include=*.{h,cpp,glsl,frag,vert,comp}
命中：0 处实现（只有 ContentBrowser 的图标表、InspectorPanel 的一句 tooltip、i18n 词典）
```

物理侧同样为零：`ShapeKind` 只有 `Sphere / Box / Capsule / Hull / Plane`（`PhysicsShape.h:36-43`），
**无高度场**；地面是"无限平面半空间"（`ScenePhysics.cpp:149-157`）。

---

## 2 · 结论：主序要改

`ship_roadmap.md` §8 把 M5 的 G21~G25 排成"地形 → 水体 → 植被 → 天空 → 烘焙"。
**这个顺序是"按渲染特性难度排的"，不是"按卡住内容的程度排的"。**

改后的主序（理由逐条写）：

| 新序 | 内容 | 为什么在这个位置 |
|---|---|---|
| **W0** | 止血：修 F1/F2/F3 三个材质 bug | **不修就会污染数据**。一旦关卡文件里积了错的材质引用（同名合并的、下标错位的），后面每做一步都要迁移一次 |
| **W1** | 材质实例（母材质 + 实例 + 覆盖） | 关卡的前提。"改一次 roughness 要点几百次"在这里解决 |
| **W2** | 导入管线（`.vmeta` + 材质身份 + 重导入策略） | 与 W1 同源：母材质要有自己的文件（`.vmat`），身份要稳 |
| **W3** | **第一个真实关卡** | 串起来验收。**这是"世界规模"的真正起点**，不是地形 |
| **W4** | 地形 | 有了关卡才知道"地面"具体要多大地、多精细。空做地形 = 猜 |
| **W5** | 植被与风 | 依赖地形（撒点要地表高度） |
| **W6** | 室内光照（探针当辐照度源） | 便宜版"室内不崩"，1 项任务解决 80% 问题 |
| **W7** | 动态天空 | 依赖 W6（太阳动了，环境光要跟着变，而 W6 给了"可变环境光"的通道） |
| **W8** | 水体 | 最独立，可以随时插队 |
| **W9** | GI 烘焙（辐照度探针网格） | 最贵，且 **W6 之后收益递减**。明确后置 |

⚠ **与 `ship_roadmap` 的关系**：G21~G25 的**技术内容大部分仍然有效**（我在下文逐条引用了），
但执行顺序、以及"材质实例"这一整块（`ship_roadmap` **完全没有这个任务**）要按本文档走。

---

## 3 · 目标与非目标

### 3.1 目标（可判定的）

1. 一个 **200 × 200 米**的室外 + **一个可进入的房间**，能用现有资产搭出来、存下来、重开还原。
2. 关卡里 **300+ 物体**，**改一次母材质的 roughness，全部生效**（1 次操作，不是 300 次）。
3. 单个物体可以"**只改这一件**"，且改动**能存盘、能撤销、重开还在**。
4. 地形能承载角色行走（高度场碰撞，脚不穿地、不浮空）。
5. 太阳角度变化时，天空 / 阴影方向 / 环境光 / 雾色**四者同步变**，无亮度突变。
6. 室内关掉方向光仍然可见（探针 GI），且**室外表现与今天逐像素一致**。

### 3.2 非目标（这一版不做，写明理由）

| 不做 | 理由 |
|---|---|
| 材质节点图（Node Graph） | `ship_roadmap:1097` 已明确非必要条件。实例 + `.vmat` 参数表够用 |
| 真正的 DDGI（动态探针） | 本机集显，重烘成本不可控；且 W6 + W9 已能给到"可用的室内" |
| 地形雕刻笔刷 | 属编辑器 E 系列（E24/E08）。本方案只做"运行时消费 + 高度图导入" |
| 100 万株植被 / GPU driven | 与"一个真实关卡"不匹配。先做 1 万株 instanced |
| 云 / 体积云 | 动态天空的第一版不含云（Hillaire 的三张 LUT 已经够撑住日出日落） |
| 分块流送（G19） | 200×200 一次加载得下（实测 500 物体 2.6 s，含初始化）。流送是"更大的世界"才需要 |
| 骨骼命名规范化 | 实测未暴露问题（anime_girl 的骨架能正常蒙皮）。真要做需要"按名字找 joint"接口（IK/挂点用），那是角色链 A6 的活 |

---

## 4 · 设计决策

> 每条都有"为什么不是别的"。这是本文档最该被反复读的部分。

### D1 · 材质实例 = **母材质浅拷贝 + parent 指针 + 覆盖位掩码**

**不是**"引用 + 逐字段求值"。

```cpp
struct Material {
    // ... 现有字段全部保留（pbr.frag 那边一个字节都不用改）...
    MaterialSource source;          // 现有，加一个 Kind::Instance
    const Material* parent = nullptr;   // 母材质（nullptr = 自己是母材质）
    uint32_t overridden = 0;            // 位掩码：哪几个字段被本地改过
};
```

求值方式是**值语义**：`*inst = *parent`（100 字节拷贝），字段直接读。
"哪些字段该跟随母材质"由 `overridden` 位掩码决定，母材质变脏时把**未覆盖**的字段刷一遍。

**为什么不用"逐字段问母材质"**：
`pbr.frag` 拿的是 push constant 里的**数值**，它不关心来源。若实例只存覆盖项，
CPU 侧每次推常量都要做一次"字段合并"（约 10 个字段 × 每物体 × 每帧）。
值语义让这条路是 `memcpy` 48 字节，零分支。

**为什么不用"继承链"（实例的实例）**：两级足够（母 → 实例），
且深度嵌套会让"改了母材质谁该变"变成需要拓扑排序的问题。**明确限制：只支持两级**。

### D2 · 覆盖的存储形态 = **只存被覆盖的字段**，不存全量

场景 JSON：

```json
"material": {
  "kind": "instance",
  "parent": "models/anime_girl.glb#Skin",
  "overrides": { "roughness": 0.9, "baseColor": [0.8, 0.2, 0.1, 1.0] }
}
```

**为什么只存覆盖项**（而不是存完整参数 + parent）：
存全量的话，母材质改了以后**所有实例的对应字段都是"值 ≠ 母材质"，于是全部被当成覆盖**
→ 改母材质不生效。这与 `persistence_roadmap.md` D3（Prefab 覆盖用显式路径表）
是**同一个道理**，只是这里字段是扁平的，位掩码就够了，不需要字符串路径。

⚠ 由此得出一条必须写进契约的限制：
**"还原到母材质默认值" 与 "显式覆盖成恰好等于默认值的那个数" 在存档里不可区分。**
后者会在母材质变化后不跟随。这是可接受的（Unity 的行为一致），但必须写进 UI：
覆盖的字段在 Inspector 里**用高亮标出**，并给 "Revert" 按钮。

### D3 · 材质身份 = `模型路径#glTF材质名`，**不再是数组下标**

```cpp
// Material 新增
std::string id;   // 稳定身份，如 "models/anime_girl.glb#Skin"
// 缓存键仍是 m_materials 的 key（"models/anime_girl.glb/mat0"），保持不变
```

- 序列化**写 `id`**，读时按 `id` 找；**找不到再按旧 key 回落**（兼容现有场景文件）
- glTF 材质名重名时追加 `#2`（`#3`…），并在导入时打 `ASSET-MAT-DUPNAME`
- glTF 材质名为空时回落到 `#mat<idx>`（这样"匿名材质"仍然稳定在"位置"上，且能看出）

**为什么不在 `Material` 里直接改名**：`m_materials` 的键被 `resolveMaterial`、`findMaterial`、
`Model::subMeshes[i].material` 多处依赖，改名要动 5 处以上。**加一个并列的字段、只让序列化用它**
是成本最低且不破坏任何现有路径的做法。

### D4 · `makeMaterialPBR` 遇到"同名但参数不同"→ **新建 + 唯一后缀 + WARN**

```cpp
auto it = m_materials.find(name);
if (it != m_materials.end()) {
    if (sameParams(it->second.get(), /*待建参数*/...)) return it->second.get();  // 真共享，合理
    // 冲突：不静默复用，也不覆盖——给唯一后缀
    name = name + "#" + std::to_string(++m_matConflictSeq);
    VK_LOG_WARN("ASSET-MAT-COLLIDE name=%s renamed=%s", orig, name.c_str());
}
```

**为什么不直接覆盖**：覆盖会让"先读到的赢、后读到的被吃",
方向和现在相反但同样静默。
**为什么不报错返回 nullptr**：那会让物体变无材质，比串味更糟。
**为什么参数相同时仍然共享**：500 个物体用同一组参数是常见情形（沙盒地面 + 箱子），
共享是合理优化，且能省材质数。判据是**逐字段比较**（浮点用 `==`，因为来源同为
`json.value(...)`，同一份文件读两次逐位相同）。

⚠ 这条改动会让"现有用例里故意同名"的场景**行为变化**（从共享变成两个材质）。
必须先 grep 一遍现有 `assets/scenes/*.json` 的材质名唯一性（实测：`sandbox.json`
4 个实体 4 个唯一名，安全）。

### D5 · 地形走**独立管线 + splat 权重打包成一张 RGBA8**

**不是**给前向管线加 4 个 set。

理由链：
1. 前向管线的纹理槽是 7 个（`kTextureSetCount = 7`），**已经满了**（height 是 R4-3 加的，正好用掉最后一位）
2. 地形 splat 需要：4 层权重的 albedo + 4 层 normal + 1 张权重图 = **9 张**，装不下
3. 本机 `maxBoundDescriptorSets=32`，**技术上**能加到 9 个 set；但低端设备只有 8
   （这是本引擎对设备的**实际要求 ≥ 8**，`RenderTypes.h:305-308` 写明）
4. 所以：**权重打包进一张 RGBA8**（4 层各 1 通道），地形的 4 层材质走**独立的 set 布局**
   （1 个 set：`sampler2D albedo[4]` + `sampler2D normal[4]` + 1 张 splat 权重图）

⚠ 这里有一个必须记住的坑（MEMORY 里已有的**铁律**）：
**subpass dependency 参与 pass 兼容性判定 → "一份 pipeline 两模式通用"是错的（VUID-02684）**。
地形是独立 pass（在 DepthPrePass 之后、Forward 之前，或者直接在不透明队列里用自己的管线），
所以它需要**自己的 `pipelineFor(RenderPass)` 缓存**，不能复用前向的。

### D6 · 植被：**独立顶点布局**，不改 `Vertex`

实测：`Vertex` 是 64 B（pos/normal/uv/joints/weights），**没有顶点色、没有 UV2**。
风的"摆动权重"没地方放。

三个选项：
- ⓐ 改 `Vertex` 加 `vec4 color`（+16 B）→ **横切**（`Mesh.h` 在 `GLOBAL_GLOBS`），
  所有几何生成器、所有网格都要重传，且**顶点带宽 +25%**
- ⓑ 借用 `weights.x`（静态网格全 0）→ **脏**：破坏"weights 之和 > 0 = 蒙皮"这条分支判据
- ⓒ **植被用自己的顶点结构**（pos/normal/uv/windWeight，40 B）+ 自己的管线

**选 ⓒ**。理由：植被不需要蒙皮、不需要 POM、不需要完整 PBR（双面 + 简单光照就够），
独立管线反而能**省**（跳过一堆分支）。而且植被本来就是"同一种草画 1 万次"，
它走 instanced，本来就不该和主前向共用逐物体推常量。

### D7 · 动态天空 = **重烘 LUT + 重烘 IBL**，`sky.frag` 一个字不改

实测：`sky.frag` 只有 27 行，就一句 `texture(skyMap, normalize(vDir)).rgb`。
天空的**全部内容**在 `Environment` 的 `skyCube` 里（`EnvSettings.zenith/ground` 两个颜色，
`Environment.h:64-65`）——**是 compute 生成的**，不是 shader 里算的。

所以动态天空的正确改法是：

```
太阳高度/方位 → ① 大气 LUT（transmittance / multiscatter / skyview，3 张 compute）
             → ② 合成 skyCube（一个全屏 compute 或 sky.frag 的 compute 版）
             → ③ 重跑 irradiance 卷积 + prefilter GGX（已有的两个 compute）
             → ④ BRDF LUT **不重烘**（与太阳无关，这是省下来的大头）
             → ⑤ 重烘 ReflectionProbe（可选，室内探针不该跟太阳变）
```

⚠ `sky.frag` 若是"采样 skyCube"就不需要改；**但如果要加星空/太阳圆盘**，
那些细节在 128² 的 cube 里会被糊掉 —— 那时才需要改 shader（加一条"视线与太阳夹角"的解析项）。
**第一版不做太阳圆盘**，先要"光照正确"。

### D8 · GI 的便宜版：**探针复用**，不新建烘焙管线

实测：`ReflectionProbe` 已经有一套完整设施（cube array、6 面捕获、预过滤、
`maxProbes=4`、binding 23 已接进 `pbr.frag`）。

所以"室内不崩"的最便宜做法是：**让辐照度也从探针取**。
- 现在 `pbr.frag` 的漫反射环境光 = `texture(irradianceMap, N)`（全局一张）
- 改后 = `mix(全局, texture(probeCubeArray, dir, probeLevel), weight)`，
  `weight` = 当前片元落在哪个探针的盒体内、以及盒体边界距离
- ⚠ **这要动 `pbr.frag`** → 属横切（`GLOBAL_GLOBS`）→ 必然全量回归，**且必须默认关**，
  关掉时逐像素与今天一致（`probeWeight = 0` 时 `mix` 退化成 `a`）

**为什么不直接做 lightmap**：lightmap 要求 UV2（`Vertex` 没有，见 D6 的同一堵墙）
+ 光照图资产格式 + 打包进 `.pak` + 改 `pbr.frag` 加纹理槽（**7 个已满**）。
探针方案不改 UV、不加纹理槽（探针 cube array 已经在 binding 23 了）。

---

## 5 · 阶段 W0 · 止血（3 个已实测的材质 bug）

> **为什么放在最前**：这三个 bug 都会**写坏数据**。W1 之后的关卡文件如果已经积了
> 同名合并的材质、下标错位的引用，"修 bug"就变成"迁移历史数据"。
> **规模合计 S，1 天内可完成，但必须最先做。**

### W0-1 同名材质冲突不再静默（修 F1）

- **改动**：`engine/assets/AssetManager.h:472-493` 的 `makeMaterialPBR`
- **要点**（按 D4）：参数相同 → 共享；参数不同 → 唯一后缀 + `ASSET-MAT-COLLIDE` WARN
- **前置检查**：`grep -c '"name"' assets/scenes/*.json` 确认现有场景无同名（实测 sandbox 安全）
- **探针**：`assets/scenes/runtime_test/mat_conflict_probe.json`（已入库，两个 Box 同名材质
  `shared_mat`，A 是 `rough=0.2`/橙、B 是 `rough=0.8`/蓝）
- **验收**：
  ```
  Game.exe --scene scenes/runtime_test/mat_conflict_probe.json \
           --dump-scene <abs>/_mat_probe_out.json --frames 5
  ```
  dump 出 `rough=0.2` 与 `rough=0.8` **两行不同**（现在是两行都是 0.2）；日志有 1 条
  `ASSET-MAT-COLLIDE`
- **规模**：**S**

### W0-2 材质稳定身份（修 F2）

- **改动**：`engine/assets/Material.h` 加 `std::string id`；
  `MeshLoader.cpp:365` 的 `key` 保留（缓存键），另算 `id = base + "#" + (gmat.name 非空 ? gmat.name : "mat" + idx)`；
  `FbxLoader.cpp:240` 同理（FBX 材质名）
- **序列化**：`materialToJson` 写 `j["id"] = mat->id`（**所有 kind 都写**）；
  `resolveMaterial` 先按 `id` 找、找不到按 `name` 回落
- **验收**：`Game.exe --selftest asset`（新增）断言：加载 anime_girl.glb 后，
  `findMaterialById("models/anime_girl.glb#Skin")` 命中，且 `mat0` 的 id **不是** `#Skin` 而是 `#Hair`……
  —— 具体哪一个是哪个由文件决定，所以断言写成"**7 个 id 与 7 个 glTF 材质名一一对应**"
- **规模**：**S**

### W0-3 导入模型的材质覆盖能存能读（修 F3）

- **改动**：`SceneSerializer.cpp:158-202`（写端 Model 分支）+ `205-229`（读端）
- **写端**（按 D2）：Model 材质也写 `overrides`（只写非默认字段）+ `id`；
  若是实例则写 `kind:"instance"` + `parent`
- **读端**：`kind=="model"` 时**先按 id 找**；找不到**必须打 `ASSET-MAT-MISS`**（现在静默 null）
- **⚠ 关键**：`resolveMaterial` 返回 nullptr 时，**调用方是谁**？实测 dump 出 `material:null`
  说明"无材质"是一条合法路径（`!mesh && !mat` 会 skip 实体）。所以要**保证 resolve 失败
  时仍然建一个占位材质**（默认白），而不是留 null —— 否则物体消失
- **验收**：探针 `_mat_model_probe.json` 往返后 `roughness == 0.05`；
  故意写一个不存在的 id → 日志有 `ASSET-MAT-MISS`、物体仍然可见（占位材质）
- **规模**：**M**（读端逻辑要重排）

### W0 的共同纪律

⚠ 三个改动都在 `engine/assets/**` 与 `engine/scene/**`，**后者在 `GLOBAL_GLOBS`**
→ 改完**必然点名全量回归**（按项目纪律：只有用户明确下令才跑）。

⚠ **否定测试**（每条都必须真的看到红）：
| 故意改坏 | 应红的断言 |
|---|---|
| W0-1 的判据改成"永远共享" | 探针 dump 出两行**相同** roughness |
| W0-2 的 id 改回下标 | 7 个 id 里出现 `#mat0` |
| W0-3 删掉 `ASSET-MAT-MISS` 日志 | "找不到时打日志"的断言红 |
| W0-3 让 resolve 失败返回 nullptr | "物体仍然可见"的断言红（实体被 skip） |

---

## 6 · 阶段 W1 · 材质实例

### W1-1 `Material` 加实例语义（D1 + D2）

- **改动**：`engine/assets/Material.h`
  - `MaterialSource::Kind` 加 `Instance`
  - `const Material* parent`、`uint32_t overridden`（位掩码，字段序见下）
  - `bool isInstance() const { return parent != nullptr; }`
  - `void makeInstanceOf(const Material& src)`（拷贝 + 设 parent + overridden = 0）
- **字段位序**（**必须写死并有 `static_assert` 保护**，因为位掩码要进存档）：
  ```
  bit0 baseColorFactor  bit1 metallic  bit2 roughness  bit3 normalScale
  bit4 ao               bit5 emissive  bit6 alphaMode  bit7 alphaCutoff
  bit8 ditherAlpha      bit9 shadingModel                  bit10 doubleSided
  bit11 heightScale
  ```
  纹理槽（7 个）**不进位掩码**：换纹理是"结构变化"，一律视为覆盖（`overridden |= kTextureBits`）
- **⚠ 陷阱**：位序一旦发布就不能改（存档里是裸数字）。
  升位序时**必须**升 `kMaterialVersion` 并写迁移（与场景 `version` 同一套纪律）
- **规模**：**S**

### W1-2 母材质变更的传播

- **改动**：`AssetManager` 加 `void materialChanged(Material* m)`；
  遍历所有 `m_materials`，对 `parent == m` 的实例，把**未覆盖**字段刷一遍（按位掩码）
- **调用点**：`editor/InspectorPanel.cpp:4783` 的 `drawMaterialSection`，
  在 `fieldEdit(...)` 提交后、且被编辑的是母材质时调用
- **为什么不做自动脏检测**：`Material` 是裸结构体，没有 setter。
  加 setter 要改 40+ 处写入点，漏一个就静默不同步。
- **⚠ 已知代价**：`materialChanged` 是 O(材质数)，每次拖动滑条调用一次。
  实测材质数在几百级，遍历成本可忽略（不做"脏集合"优化）
- **规模**：**S**

### W1-3 Inspector：实例的创建 / 识别 / 还原

- **改动**：`editor/InspectorPanel.cpp` 的 `drawMaterialSection`
- **UI 面**（这一条**必须有**，否则特性不算交付）：
  1. 顶部一行显示：`Instance of <parent id>` 或 `Material (asset)`
  2. **`Make Unique` 按钮**：从母材质派生一个实例（当前实体专用）。
     若已是实例则置灰
  3. **`Revert` 按钮**（每个被覆盖的字段右侧一个小 ↺）：清掉该位
  4. **被覆盖的字段用高亮**（`ImGui::PushStyleColor(ImGuiCol_Text, kOverrideColor)`），
     未覆盖的字段用正常色 —— 这是"我在改的是这一件还是全部"的唯一视觉线索
  5. 撤销：`Make Unique` 是结构变化 → 走 `structuralEdit`；`Revert` / 字段改动 → `RawBytesEditCommand`
- **锚点**：`INS-MAT-INSTANCE`（"Instance of ..." 那一行）、`INS-MAT-MAKEUNIQUE`（按钮）、
  `INS-MAT-REVERT-<field>`（字段旁的 ↺）
- **规模**：**M**

### W1-4 序列化（见 D2 的 JSON 形态）

- **写端**：`kind` 为 `"instance"` 时写 `parent` + `overrides`（只写被覆盖的字段）
- **读端**：
  1. 先按 `parent` 找母材质
  2. `makeInstanceOf(*parent)`
  3. 应用 `overrides`，同时置对应位
  - ⚠ **母材质找不到时**：打 `ASSET-MAT-MISS parent=...`，建一个**独立材质**（退化成一个普通
    material）并应用 overrides —— **不要让它变成 null**。这条保证"删了模型文件，
    关卡里的东西还在，只是没了共享关系"
- **规模**：**M**

### W1-5 自检与用例

- **`Game.exe --selftest material`**（新增，无窗口无设备）：
  - 实例的未覆盖字段跟随母材质（改母 → 实例变）
  - 被覆盖字段不跟随（改母 → 实例不变）
  - 位掩码往返：设置所有 12 位 → 序列化 → 反序列化 → 掩码相等
  - 两级限制：`makeInstanceOf(instance)` **拒绝**并打 WARN
- **`tools/verify_material_instances.py`**：
  - A 组（无窗口）：自检输出
  - B 组（无窗口）：场景往返 —— 300 个实例 → dump → 只出现 `parent` + 少量 `overrides`
  - C 组（编辑器）：`INS-MAT-INSTANCE` 存在 / 点 `Make Unique` 后变 `Asset` / `Revert` 后掩码清零
- **规模**：**M**

### W1 的 DoD

- [ ] 场景里 300 个物体引用同一个母材质，改母材质 1 次 → 300 个全变（**这是本条的目的**）
- [ ] 其中一个物体 `Make Unique` 改 roughness → 存盘 → 重开 → **只有它没变**
- [ ] 关掉实例特性（老场景没有 `kind:"instance"`）→ 与今天**逐像素一致**
- [ ] `verify_i18n_coverage.py` PASS（新增 UI 字面量都要进词典）

---

## 7 · 阶段 W2 · 导入管线

### W2-1 材质资产文件 `.vmat`

`ContentBrowser` 的 `Add ▸ Material` 已经在写 `.vmat`（`ContentBrowser.cpp:1129-1135`），
但**没有加载器**。W2-1 把它补上，与场景里的 `procedural` 块**同一套字段**：

```json
{
  "version": 1,
  "type": "material",
  "id": "materials/stone_wall",
  "baseColor": [0.42, 0.44, 0.47, 1.0],
  "metallic": 0.0, "roughness": 0.9,
  "albedo": {"kind":"file","path":"textures/stone_albedo.png"},
  ...
}
```

- **为什么单独一个文件**：母材质要能"被多个关卡共享"。放进场景里就只能复制粘贴
- **为什么字段与场景 `procedural` 块完全相同**：`materialToJson` / `resolveMaterial`
  可以直接复用，不需要第二套解析器 —— 这是"一处定义、两处用"的省法
- **验收**：建一个 `.vmat` → 两个关卡引用它 → 改 `.vmat` → 两个关卡都变

### W2-2 `.vmeta`（导入设置 sidecar）

```json
{
  "version": 1,
  "source": "models/anime_girl.glb",
  "sourceHash": "fnv1a:0x8a3f...",
  "import": {
    "scale": 1.0,
    "generateLods": true,
    "lodGridRes": [24, 12, 6],
    "importMaterials": true,
    "importAnimations": true,
    "materialIdScheme": "name"
  },
  "materials": { "Skin": "models/anime_girl.glb#Skin", "Hair": "..." }
}
```

- **`sourceHash`**：源文件变了才重新导入。用 FNV-1a（与 `persistence_roadmap` 的增量存档同一套，不引新哈希）
- **`materials` 映射表**：这是 F2 的落地 —— 材质的稳定身份**记在 `.vmeta` 里**，
  重导出时按 glTF 材质名重新绑定，而不是按下标
- **⚠ 重导入策略（必须写死）**：
  | 情况 | 行为 |
  |---|---|
  | 源没变 | 什么都不做 |
  | 源变了，几何/动画变了 | 重导几何与动画，**材质 id 映射按名字保留**；映射不上的打 `ASSET-MAT-REMAP-FAIL` |
  | 用户改了 `.vmeta` 的 import 参数 | 强制重导（不动材质 id） |
  | 场景里的材质覆盖 | **不碰**（覆盖属于场景，不属于导入） |

### W2-3 蒙皮 LOD（`Lod.h` 已写明的那块）

- **现状**：`meshLooksSkinned(*base)` 直接让链停在 1 级 → 蒙皮网格**没有 LOD**
- **根因**（`Lod.h` 文件头）：`decimateClustered` 只合并 pos/normal/uv，**丢 joints/weights**
- **改法**：按"主影响 joint"分组，**同组内**才允许合并
  ```cpp
  // 顶点的"主影响 joint" = weights 里最大分量的下标
  // 分组键 = (主 joint, 空间格) → 只有键相同的顶点才合并
  ```
- **验收**：
  - 蒙皮网格生成 ≥ 2 级
  - **权重归一化**：每个顶点的 `weights` 之和 ∈ [0.99, 1.01]（否则蒙皮会缩水）
  - LOD 切换时**角色不"塌"**：同一姿势下 LOD0 与 LOD2 的关节位置包围盒差 < 5%
- **规模**：**M**（算法不难，难在验证）
- **⚠ 顺带修正 `ship_roadmap:21` 的说法**：那里说"LOD 缺失"，实际是"**蒙皮** LOD 缺失"

### W2-4 `Add ▸` 的空壳：要么实现，要么下架

实测 13 种 Add 里 8 种是空文件。按**特性准入纪律**，这些"UI 面存在但功能为零"的项
是假交付。两个选择：

- ⓐ 全部实现（water / foliage / animation / cinematic / media / physics / ui / worldmap）
- ⓑ **从菜单里下架**，只留已实现的（scene / material / texture / model / audio）

**选 ⓑ（本阶段），并在 W4/W5/W8 逐项加回来**。理由：菜单里点下去生成一个打不开的文件，
比"没有这一项"更伤信任。而且下架是**可逆**的（加一行），实现不是。

⚠ 下架会红 `verify_content_browser`（如果它断言了 13 项）。
先 grep 再改，**并同步改用它做 fixture 的地方**。

---

## 8 · 阶段 W3 · 第一个真实关卡

> **这是"世界规模"的起点。** 目的不是做出一份好看的关卡，是**把链上的每一环都踩一遍**，
> 让剩下的问题自己浮出来。

### W3-1 关卡规格（先定死，不许边做边改）

| 项 | 值 |
|---|---|
| 尺寸 | 200 × 200 米 |
| 内容 | 地面（40×40 tile 或 1 片 plane）+ 6 个箱体堆 + 1 栋房（外墙 4 面 + 屋顶 + 门洞）+ **1 个室内房间**（8×8×3 m，1 盏点光） |
| 角色 | `anime_girl.glb` 实例 ×1，播 `Walk` |
| 材质 | **共 8 个母材质**（地面 / 墙 / 木箱 / 屋顶 / 玻璃 / 金属 / 角色皮肤 / 角色衣服），300+ 物体**全部引用这 8 个** |
| 光影 | 方向光（CSM）+ 室内点光 + 1 个探针（室内） |
| 存盘 | `assets/scenes/levels/first_level.json` |

**300+ 物体全部引用 8 个母材质**——这是本阶段的**核心验收动作**，
它同时检验 W1（实例）、W2（`.vmat`）、W0（身份稳定）。

### W3-2 生成而非手写

关卡 JSON 手写 300 个实体会错。写 `tools/gen_first_level.py`：

- 输出确定（**用固定种子 + `sorted` 遍历**，不用 `random` 无种子）
- 每件物体的命名有规则（`Crate_<i>_<j>`），可被用例断言
- 生成后**必须**能被 `Game.exe --scene` 加载 + `--dump-scene` 往返**逐字段相等**
  （除了浮点末位）

### W3-3 验收（6 条，全部可断言）

| # | 判据 | 怎么测 |
|---|---|---|
| 1 | 加载 < 2 s | `GAME-SCENE` 前后 `GAME-LOOP` 的 `sim=` |
| 2 | 60 fps @1280×720 | `GAME-LOOP-SUMMARY dropped` + `GPU-TIMING frame=<16.7` |
| 3 | **改一次母材质，300 个全变** | 改 `.vmat` 的 roughness → 截两张图 → 显著变化像素占比 > 5%（**不是平均差**，见 MEMORY 的像素断言纪律） |
| 4 | **单件覆盖能存盘** | 一个 Crate `Make Unique` 改色 → 存 → 重开 → dump 出 `kind:"instance"` + 1 个 override |
| 5 | 场景往返**逐字段相等** | `--dump-scene` → Python 深比较，允许 float 相对误差 1e-6 |
| 6 | 室内关掉方向光仍可见 | `MYVK_RENDER_SUN=0`（新增 env）→ 截图非全黑（室内平均亮度 > 0.05） |

⚠ 第 3 条**必须用显著变化像素占比**，不能用整图平均差 —— 角色占屏 4% 时平均差只有 0.41/255
（MEMORY 里记着这个坑）。

### W3-4 这一阶段会暴露什么（预告，写完文档就知道要盯哪）

- 材质身份（W0-2）不稳 → 重开后被覆盖的材质**错位到别的槽**
- 蒙皮 LOD（W2-3）没做 → 远处的角色**一直是高模**，300 件里最贵的那个
- 场景 `version` 只写不读（`persistence_roadmap` H2）→ 关卡文件格式一变就无法迁移
- 没有地形 → 地面必须用 plane，**角色走出 40 m 就掉下去**

---

## 9 · 阶段 W4 · 地形

### W4-1 资产与格式

```
assets/terrain/<name>/
  height.r32        # 32-bit 浮点高度（列主序，行列数在 .vterrain 里）
  splat.rgba8       # 4 层权重
  terrain.vterrain  # JSON：尺寸 / 米每像素 / 层材质 id / 最大高度
```

**为什么高度用裸 `.r32` 而不是 PNG**：16 位 PNG 只有 65536 级，200 m 高度范围下量化步长
3 mm —— 角色走上去会有可见的台阶。32 位浮点 1 MB / 256² 完全可接受。

### W4-2 渲染（按 D5）

- **独立管线**（自己的 set layout + 自己的 `pipelineFor(RenderPass)` 缓存）
- **splat 权重**：一张 RGBA8（4 层各 1 通道），`texture(splat, uv*repeat).rgb` 直接就是 3 个权重，
  `.a` 是第 4 个（或 `.a` = 1-sum，省一张）
- **层混合**：`albedo = Σ w_i × albedo_i`，`normal = normalize(Σ w_i × normal_i)`（**不能**只取权重最大的那个，会看到硬接缝）
- **顶点位移**：`pos.z = texture(height, uv).r * maxHeight`。⚠ 高度图采样必须**线性**（`srgb=false`）
- **LOD**：quadtree，按到相机距离分块。**相邻不同层之间必须缝合**，两种做法：
  - 裙边（skirt）：边缘顶点向下拉 2 m（简单、看不出、推荐第一版）
  - 顶点共享（更正确，但要重排索引）
  - **选 skirt**，理由：`Lod.cpp` 的顶点聚类已在别处验证过，地形不该引入第二种减面逻辑
- ⚠ **改 `Terrain` 管线 = 新 shader** → `assets/shaders/compile.bat` 要跑一遍，
  且**必须进 CMake 的 shader 拷贝列表**（MEMORY：资产拷贝必须 `add_custom_target(... ALL)`）

### W4-3 物理：高度场查询（G21 明确要求"需新增"）

- **不改 narrowphase**。新增 `physics::HeightfieldQuery`：
  ```cpp
  // 双线性插值，O(1)
  float heightAt(const Heightfield& hf, float x, float y);
  glm::vec3 normalAt(const Heightfield& hf, float x, float y);   // 中心差分
  bool raycast(const Heightfield& hf, const glm::vec3& o, const glm::vec3& d,
               float maxT, HeightHit& out);   // 沿 ray 步进 + 二分细化
  ```
- **接进 `CharacterController`**：`G07` 的贴地逻辑现在找"最近的地面"，改成"先问高度场"。
  ⚠ **必须保留 plane 回落**（现有用例全用 `groundPlane`）
- **刚体**：第一版**不支持**刚体在高度场上滑动（只给角色控制器用）。
  真要支持，得把高度场切块成三角形进 broadphase —— 那是独立一块工作（明确非目标）
- **验收**（G21 原文口径）：地形与高度图逐像素对应，**采样点高度与射线命中点高度差 < 1 cm**；
  角色在 30° 坡上**不下滑、不抖动**（位置逐帧位移 < 5 mm）

### W4-4 编辑器：只做"能看见"，不做雕刻

- ContentBrowser 双击 `.vterrain` → 视口里出现地形（等于把它实例化成一个实体）
- Inspector 出一个 Terrain 段：尺寸 / maxHeight / 4 层材质槽 / `Height scale`
- **不做**笔刷（E24 的活）

### W4 的规模与依赖

- **规模**：**L**（渲染 M + 物理 M + 序列化 S）
- **依赖**：无硬前置（bindless 不开也能做 —— 4 张材质走独立 set 就够）
- **⚠ 可选加速**：如果 W5 的植被要 1 万株，**开 bindless** 会省掉"每株一个 set"的问题。
  bindless 已经实现且本机可用，只是默认关。**建议在 W4 之前做一次 `pipeline.bindless=1` 的
  回归对照**（确认开了以后既有画面逐像素一致），这样 W4/W5 就有一条更宽的路

---

## 10 · 阶段 W5 · 植被与风

### W5-1 顶点布局（按 D6）

```cpp
struct FoliageVertex {   // 40 B
    glm::vec3 pos;       // 12
    glm::vec3 normal;    // 12
    glm::vec2 uv;        //  8
    float windWeight;    //  4  ← 0 = 根部（不动），1 = 叶尖（摆幅最大）
    float _pad;          //  4  凑 16 对齐
};
```

### W5-2 风：**全局风场 + 顶点摆动**，在 `foliage.vert` 里

```glsl
float phase = dot(pos.xz, vec2(0.31, 0.27)) + uTime * wind.speed;
float gust  = 0.5 + 0.5 * sin(phase) * sin(phase * 0.37 + 1.7);
vec3 offset = wind.dir * (gust * wind.strength * windWeight² * height);
```

- **`windWeight²`**：平方让根部几乎不动、叶尖摆得明显（线性会让整株平移，看起来像纸片整体挪）
- **风力 0 → 逐位静止**（默认关，与"关闭 = 基线"的纪律一致）
- ⚠ 风参数**必须进 `FeatureRegistry`**（`foliage.wind` / `_strength` / `_speed` / `_dir`），
  这样 Engine 面板 / env / 序列化三路自动派生

### W5-3 撒点：**编辑器里刷**还是**脚本生成**

第一版用 **`FoliageVolumeComponent`**（盒体 + 密度 + 种子 + 类型表），
运行时在 `onInit` 里按种子撒点（**不进场景文件**，只存 5 个参数）。

**为什么不像 Unity 那样把每个实例写进场景**：1 万株 = 1 万个实体 = 1 万次 ECS 遍历，
实测 500 物体已经要 2.6 s 初始化。存参数、运行时生成，是唯一能扛住 1 万株的做法。

- ⚠ **必须确定性**：种子 + 固定遍历顺序 → 每次生成的实例位置逐位相同（可断言）
- **实例数据进 MDI**（`kMaxObjects = 8192` → 单次 draw 上限 8192；1 万株要**分 2 批**）

### W5-4 验收

- 1 万株 @1280×720 > 60 fps（**且 draw call 数 << 株数** —— 这是 instancing 生效的证据）
- 风力 0 → 与"没有植被组件"**逐像素一致**（组件不生效时不该有任何画面变化）
- 种子相同 → 两次启动实例位置逐位相同（`FOLIAGE-SPAWN` 日志打前 10 个位置，用 `%.6f`）
- 密度变化 → 株数与密度**线性**（±2%）

### W5 的规模与依赖

- **规模**：**L**（新管线 + 新顶点布局 + 撒点 + 风 + 用例）
- **依赖**：W4（撒点要地表高度）。**可以并行做"平地上的植被"，但落地高度要等 W4**

---

## 11 · 阶段 W6 · 室内光照（便宜版，见 D8）

### W6-1 探针当辐照度源

- **现状**：`pbr.frag` 的漫反射环境光 = `texture(irradianceMap, N)`（全局一张）
- **改法**：加一个 `probeWeight`（按片元到最近探针盒体的距离算），
  `ambient = mix(globalIrr, texture(probeCubeArray, dir, highMip), probeWeight)`
  - 探针 cube array **已经在 binding 23 了**（MEMORY：缺省必须绑 1×1 全黑 cube array）
  - 取**最高 mip**（已经预过滤过，等价于辐照度）—— 不需要新资产
- **⚠ 横切**：改 `pbr.frag` → `GLOBAL_GLOBS` → 全量回归被点名
- **⚠ 默认关**：`probeWeight = 0` 时 `mix(a, b, 0) = a` → **逐像素与今天一致**
- **验收**：
  - 关：与今天逐像素一致（`verify_ibl` 的既有断言不动）
  - 开 + 室内放一个探针：室内关掉方向光**仍可见**；室外像素变化 < 1%
  - 探针边界：走出盒体时 `probeWeight` 连续降到 0（**无硬接缝**，
    `ReflectionProbe` 的 `blendDistance` 未实现是已知问题，这次顺手做）

### W6-2 顺带：`ReflectionProbe` 的 `blendDistance`

MEMORY / `render_roadmap` §R5-1 已记："`blendDistance` 已进组件与 GPU 结构，但相邻探针之间的
边界混合还没实现（多探针 `MYVK_PROBE_MAX > 1` 时仍会看到硬接缝）"。

W6-1 要算 `probeWeight`，**正好需要同一个权重函数** → 一次实现，两处受益。

### W6 的规模

- **规模**：**M**（权重函数 + shader 改动 + 两处消费 + 用例）
- **依赖**：无（反射探针已就绪）
- ⚠ **这是"室内不崩"的 80% 解法**，成本却是 W9 的 1/10

---

## 12 · 阶段 W7 · 动态天空

### W7-1 大气散射 LUT（按 D7）

三张 compute（与 `Environment` 现有的四个 compute 同一个框架）：

| LUT | 尺寸 | 输入 | 输出 |
|---|---|---|---|
| transmittance | 256×64 | 海拔、仰角 | RGB 透射率 |
| multiscatter | 32×32 | 太阳角 | RGB 多次散射项 |
| skyview | 192×108×? | 视线、太阳、海拔 | RGB 天空色 |

- **为什么是 Hillaire 而不是 Preetham**：Preetham 是"天空看起来对"，Hillaire 是"能量守恒"，
  后者才能让"太阳落下时环境光同步变暗"这件事自然发生（Preetham 的色温曲线是拟合的，
  驱动 IBL 会得到"天空变红了但地面还是亮的"）
- **⚠ 本机是集显**：三张 LUT 的 compute 在 Iris Xe 上的耗时**必须在 W7-0 实测**
  （`EnvSettings` 现在的 `prefilterSamples = 512` / `brdfSamples = 512`，重烘一遍
  可能是几十毫秒 → **必须降频**）

### W7-2 重烘节奏（这是本阶段最容易做错的地方）

```
太阳角度变化 > 0.5°   OR   每 N 秒（N 可配，默认 2）
   → 重烘 skyview (compute)
   → 重跑 irradiance 卷积（32²，便宜）
   → 重跑 prefilter GGX（128² × 5 mip × 512 样本 ← **贵**）
   → BRDF LUT **不重烘**（与太阳无关）
```

- ⚠ **prefilter 是最贵的**，两个降级手段：
  - 白天只重烘 irradiance（漫反射对天空变化最敏感），prefilter **按角度分档**（每 15° 一次）
  - 或者 prefilter 降到 64² × 3 mip（夜景看不出差别）
- **`TimeOfDayComponent`**：`{ hours, dayLengthSec, latitude }`，太阳方向由它算。
  ⚠ **时间流速为 0 时太阳静止**（默认关，这是 G24 的验收口径）

### W7-3 四者同步（G24 明确要求"必须一起变，否则画面割裂"）

| 消费者 | 改什么 |
|---|---|
| CSM 光方向 | `ShadowPass::setLightDir()`（已有） |
| IBL | 重烘（W7-2） |
| 雾色 | `pbr.frag` 的高度雾取 `skyMap`（`pbr.frag:1447`）→ **自动跟着变**，不用改 |
| 天空背景 | `sky.frag` 采样 skyCube → **自动跟着变**，不用改 |

**这就是 D7 的好处**：`sky.frag` / `pbr.frag` 的雾色两处**一个字都不用改**，
因为它们本来就在采样"唯一的那张 skyCube"。

### W7-4 验收

- 一天循环（600 帧）**无亮度突变**：相邻帧的全屏平均亮度差 < 0.02（连续）
- 日出/日落时**阴影方向连续**：`SHADOW-DIR` 日志逐帧打光方向，相邻帧夹角 < 0.5°
- 时间流速 = 0 → 太阳**静止**（`SUN-POS` 两次采样逐位相同）
- 关掉动态天空（`render.dynamicSky=0`）→ 与今天**逐像素一致**

### W7 的规模与依赖

- **规模**：**L**
- **依赖**：**W6**（环境光要能"按体积/探针"变，否则太阳动了室内还是死板的全局 irradiance）

---

## 13 · 阶段 W8 · 水体

> 最独立的一块，可以随时插队（甚至在 W4 之前做"一池水"）。

- **Gerstner 波**：法线位移（4 个波叠加），顶点着色器位移 + 解析法线
- **菲涅尔**：Schlick，`F0 = 0.02`
- **折射**：采样不透明 HDR（`sceneColor`，已有）+ 深度差做"浅水更透明"
- **反射**：先接 **SSR**（`fg_ssr.frag` 已有）；SSR 未命中时回落 `skyMap`
- **深浅色**：`mix(deepColor, shallowColor, saturate(depthDiff / fadeDistance))`
- **焦散**：第一版**不做**（噪声滚动贴图要额外的 UV 投影，收益/成本不匹配）
- **验收**：
  - 相机半沉入时**切换正确、无闪烁**（G22 口径）
  - 水面法线随波变化（相邻帧法线夹角 > 0 且 < 5°）
  - 关掉水体（组件不存在）→ 与今天**逐像素一致**

**规模**：**L**　**依赖**：无（`SSR` / `sceneColor` / 深度图都已有）

---

## 14 · 阶段 W9 · GI 烘焙（明确后置）

### 为什么是"辐照度探针网格 SH"，不是 lightmap

| 方案 | 需要的 | 代价 |
|---|---|---|
| **lightmap** | UV2（`Vertex` 没有，见 D6）+ 光照图资产 + `pbr.frag` 加纹理槽（**7 个已满**） | **L+**，且改两处横切 |
| **辐照度探针网格（SH L2）** | 一个 SSBO（探针位置 + 9 个 SH 系数）+ `pbr.frag` 一次三线性插值 | **M**，只改 `pbr.frag` 一处 |

**选 SH 探针**。9 个 float × 探针数，1000 个探针 = 36 KB，完全不是问题。

### 烘什么

- **CPU 路径追踪**（无 GPU ray tracing：`VkPhysicalDeviceFeatures` 里没开）
- 每个探针 256~1024 条光线，弹射 3 次
- 场景静态几何 + **自发光材质**（不需要灯光列表遍历，路径追踪天然处理）
- ⚠ **不烘动态物体**（角色会动）→ 探针只烘静态几何，动态物体的贡献靠实时 CSM/局部光

### 为什么后置

1. **W6 已经给了"室内可见"**（探针当辐照度源）。W9 是"室内好看"，不是"室内能用"
2. **烘焙需要关卡定型**。W3 的关卡还在改，烘出来的探针每次都要重来
3. **本机是集显**。CPU 烘 1000 探针 × 1024 光线 ≈ 几分钟到几十分钟（要实测），
   这个反馈循环在迭代期是不可接受的

### 验收（真要做的判据，先写着）

- 关掉方向光后场景**仍可见且有色彩分层**（不是一片灰）
- 探针**无缝插值**：移动相机无接缝跳变（相邻 5 cm 的两次采样，间接光差 < 2%）
- 关掉 GI（`render.gi=0`）→ 与今天**逐像素一致**

**规模**：**L**　**依赖**：**W6**（复用探针基础设施）+ **W3**（关卡定型）

---

## 15 · 每项的通用 DoD（与项目五件套对齐）

任何一项 W 任务"做完了"的定义（缺一不可）：

1. **实现**（代码在 `engine/` 或 `app/` 层）
2. **`FeatureRegistry`**：新特性必须有 `bindParam`（`bindParam` 在成员默认初始化**之后**、
   `loadEnvOverrides()` **之前**）+ 环境变量 + Engine 面板自动派生
3. **UI 面**：Inspector 段 + 锚点（`INS-*`）/ ContentBrowser 可及 / 控制台命令
4. **序列化**：进场景 JSON（或自己的资产文件），且**旧场景读进来逐位一致**
5. **用例 + `TRIGGERS` 登记**（改完必跑 `check_regression_map.py`）

⚠ **默认一律关**：`config/render.json` 缺失 → **逐像素等于今天**。
这是项目里反复用的"关闭 = 基线"约定，每一项都适用。

---

## 16 · 可断言锚点（新增的）

| 锚点 | 何时打 | 字段 | 用在哪 |
|---|---|---|---|
| `ASSET-MAT-COLLIDE` | 同名材质参数冲突 | `name= renamed=` | W0-1 |
| `ASSET-MAT-MISS` | 材质 id/parent 找不到 | `id= reason=` | W0-3 / W1-4 |
| `ASSET-MAT-REMAP-FAIL` | 重导入时材质名对不上 | `model= name=` | W2-2 |
| `ASSET-VMETA` | 读/写 `.vmeta` | `path= hash= changed=` | W2-2 |
| `MATERIAL-INSTANCE` | 建实例 / 析构 | `parent= overrides=` | W1 |
| `MATERIAL-SYNC` | 母材质变更传播 | `parent= updated= skipped=` | W1-2 |
| `TERRAIN-LOAD` | 地形加载 | `path= size= mpp= maxh=` | W4 |
| `TERRAIN-HIT` | 高度场射线 | `x= y= z=` | W4-3 |
| `FOLIAGE-SPAWN` | 撒点完成 | `count= seed= types=` | W5 |
| `SKY-REBAKE` | 重烘 | `sun= deg= ms= lod=` | W7 |
| `SUN-POS` | 每 N 帧 | `hours= dir= color=` | W7 |

⚠ **`detail=` 里不能有空格**（项目铁律）—— 设备名/材质名里的空格换成下划线。

---

## 17 · 否定测试清单（每条都必须真的跑一遍看到红）

| 故意改坏 | 应红的断言 |
|---|---|
| W0-1：`makeMaterialPBR` 永远复用同名 | 探针 dump 出两行**相同** roughness |
| W0-2：id 改回数组下标 | "7 个 id 与 glTF 材质名一一对应"红 |
| W0-3：resolve 失败返回 nullptr | "物体仍然可见"红（实体被 skip） |
| W1：位掩码改成"永远全跟随" | "改母材质，实例的覆盖字段不变"红 |
| W1：位掩码改成"永不跟随" | "改母材质，未覆盖字段跟着变"红 |
| W1：`overrides` 写成全量 | "改母材质后实例跟随"红（**这是 D2 的守卫**） |
| W2：`sourceHash` 恒等 → 永不重导 | "源变了会重导"红 |
| W2-3：蒙皮减面不按 joint 分组 | "权重和 ∈ [0.99,1.01]"红（顶点被吸到原点） |
| W4：高度图按 sRGB 采样 | "采样高度 vs 射线命中 < 1 cm"红 |
| W4：地形相邻 LOD 不缝合 | "地形 LOD 切换处无裂缝"红（深度图在接缝处断裂） |
| W5：风权重用线性而非平方 | 视觉断言较弱 → 改用"根部顶点位移 = 0"的**数值**断言 |
| W6：`probeWeight` 恒为 1 | "室外像素变化 < 1%"红 |
| W7：重烘不降频 | "600 帧无亮度突变"未必红，但**帧率会掉** → 加"重烘次数 ≤ 30"的计数断言 |

⚠ 否定测试做完**必须还原并重跑一次看到绿**（项目纪律）。

---

## 18 · 回归登记与横切警告

```python
"verify_material_instances": ["engine/assets/Material.h",
                              "engine/assets/Material.cpp",
                              "engine/assets/AssetManager.h",
                              "engine/scene/SceneSerializer.cpp",
                              "editor/InspectorPanel.cpp",
                              "assets/scenes/runtime_test/mat_conflict_probe.json"],
"verify_first_level":        ["assets/scenes/levels/first_level.json",
                              "tools/gen_first_level.py"],
"verify_terrain":            ["engine/terrain/**",
                              "engine/physics/Heightfield*",
                              "assets/shaders/terrain.*"],
"verify_foliage":            ["engine/foliage/**",
                              "assets/shaders/foliage.*"],
"verify_water":              ["engine/water/**",
                              "assets/shaders/water.*"],
"verify_sky":                ["engine/render/Environment.*",
                              "assets/shaders/sky_atmos.*"],
"verify_gi":                 ["engine/render/GlobalIllumination*"],
```

⚠⚠ **横切文件，不要写进 `TRIGGERS`**（写了在 `--narrow` 下会假跳过）：
`engine/ecs/**`、`engine/scene/**`、`assets/shaders/pbr.frag`、`engine/render/**` 的大部分
—— 以 `run_regression.py` 的 `GLOBAL_GLOBS` 为准。

⚠ 本轮会改的横切文件：`engine/assets/Material.h`（不是 `GLOBAL_GLOBS`，但被多方引用）、
`engine/scene/SceneSerializer.cpp`（**是** `GLOBAL_GLOBS`）、`assets/shaders/pbr.frag`（W6，**是**）。
→ **按纪律：只有用户明确下令才跑全量回归。**

⚠ `assets/scenes/**` 被多个用例读（`verify_runtime_*` / `verify_player` / `verify_audio`），
新增 `levels/first_level.json` 不影响它们，但**不要在 `sandbox.json` 上做实验**
（`verify_runtime_states` 的冷启动基线读它）。

---

## 19 · 进度台账

| 日期 | 任务 | 结果 |
|---|---|---|
| 2026-10-18 | **方案定稿**（本文档） | 实测 12 条事实（§1），**否掉用户原话三处**（C1 Z-up 已解决 / C2 LOD 已有只缺蒙皮 / C3 物体数不是性能瓶颈：50→500 耗时不变）。实测 **3 个材质 bug**（F1 同名静默合并 / F2 身份是下标 / F3 覆盖不持久化）。**主序被改**：内容链先行，地形第 4 位。**未写任何代码。** |
| 2026-10-18 | **W0 止血 ✅**（W0-1/2/3） | 三条都落盘并做了否定测试（见 §19.1）。 |
| 2026-10-18 | **W1 材质实例 ✅**（W1-1~W1-5） | `--selftest material` **pass=15 fail=0**；`verify_material_instances.py` **16 条全绿 / 23.4 s**；`check_regression_map` PASS；`verify_i18n_coverage` PASS（词典 1588 条）。 |
| 2026-10-19 | **W2-1 `.vmat` ✅** | 8 个母材质资产 + `kind:"vmat"` 读端（递归回 `procedural`，一套解析器两处用）+ 每次构建都拷。实测：两个 Box 引用同一个 `.vmat` → **恰好 1 条 `ASSET-VMAT`**、dump 里同为 `kind:"vmat"` + 同一 id；**改 `.vmat` 的 baseColor → 左右两个方块显著变化 8.16% / 3.86%**（同配置连开两次的基线 **0.000%**）。D 组 4 条 / 39.2 s 全绿。 |
| 2026-10-19 | **W2-2 `.vmeta` ✅** | `assets/Vmeta.{h,cpp}`：FNV-1a(64,文件) + import 签名 + 材质名→id 映射表 + 三态策略（源没变/源变了/参数被改）。`verify_import_vmeta.py` **18 条全绿 / 12.5 s**：`changed=0` 时 sidecar **mtime 逐位不变**（真的没写盘）；手改映射 `Skin→custom/skin_v9` 后 `ASSET-MAT-ID` 跟着变；改 hash + 塞 `Ghost` → `changed=1` + 恰好 1 条 `REMAP-FAIL` + 陈旧映射被删 + 已有映射保留；改 `import.scale` → `forced=1` 且签名刷新；**`scale=0.5` 时 FBX 归一化半径 0.822→0.411（比值 0.500）**。 |
| 2026-10-19 | **W2-3 蒙皮 LOD ✅** | `decimateClustered` 改为 **按主影响 joint 分组**（外层空间格 / 内层 joint 的嵌套键）+ 权重按 joint 累积、取前 4 归一化。`--selftest lod` **pass=9 fail=0**；实测 `anime_girl.glb` 蒙皮链从 **1 级 → 4 级，1648/814/382/184**（末级 8.96×），渲染层**接管了它（ents=1）**。`verify_lod.py` PASS / 67.0 s。 |
| 2026-10-19 | **W2-4 空壳下架 ✅** | `Add ▸` 13 项 → **5 项**（material/model/scene/texture/audio），下架 8 个没有加载器的空壳（W4/W5/W8 逐项加回）。 |
| 2026-10-19 | **W3 第一个真实关卡 ✅** | `tools/gen_first_level.py` → `assets/scenes/levels/first_level.json`：**373 个实体**，8 个母材质全部用上（crate 346 / metal 10 / wall 7 / cloth 2 / glass 2 / skin 2 / ground 1 / roof 1）+ 1 盏室内点光 + 1 个播 Walk 的角色。`verify_first_level.py` **10 条全绿**：加载 373 / 8 个母材质**各解析一次** / 往返 **0 处不同** / GPU **11.1 ms** / 增量 **24.0 ms（0.064 ms/实体，Debug）** / 改 `crate.vmat` 的 baseColor → **19.56%** 像素显著变化（基线 0.00%）/ 关太阳后室内 luma **0.268 vs 灯灭 0.014** / 单件覆盖往返仍是 `instance` + 1 个 override。 |
| 2026-10-19 | **W4 地形 ✅**（W4-1~W4-4） | `engine/terrain/Terrain.{h,cpp}`（资产 + 分块 LOD 网格）+ `engine/physics/Heightfield.{h,cpp}`（高度场查询）+ `engine/render/TerrainPass.{h,cpp}`（独立管线）+ `terrain.vert/frag` + `assets/terrain/hills/`（**257² 采样点 / 2 m → 512×512 m / maxHeight 60 m / 4 层 / 64 块**）。`--selftest terrain` **pass=27 fail=0**；`verify_terrain.py` **16 条全绿**（LOD：贴地 35712 tris → 搬出地形外 4096 tris = **11.5%**；改层材质 **76.47%**；关地形 **76.41%**；GPU **8.22/9.84 ms**）；`verify_terrain_editor.py` **23 条全绿**（双击 `.vterrain` → 视口 **32.03%**；再双击不叠第二份；关 Visible **76.29%**；Ctrl+S 往返；重开自动恢复；Add ▸ Terrain 建出来的资产能直接打开）。`Add ▸` 5 项 → **6 项**（加回 terrain）。 |

### 19.1 W0 + W1 的实测数字与坑

**W0-1（同名材质不再静默合并）**
- 改 `AssetManager::makeMaterialPBR`：参数逐字段相同 → 真共享；不同 → 唯一后缀
  `#1/#2/...` + `ASSET-MAT-COLLIDE`。
- ⚠ **幂等**：同一份场景读两次不能每次多长一个 `#7`。做法是冲突时先在
  `<base>#k` 里找参数相同的复用。这条不是洁癖 —— 编辑器会反复重载场景。
- 探针实测：`A rough=0.20 / B rough=0.80`（修复前**两行都是 0.20 且零日志**）。
- ⚠ **顺带修掉一个更严重的**：`FbxLoader` 里所有材质都叫 `modelName + "/mat"`，
  于是 `anime_girl.fbx` 的 7 个材质**在导入时就已经塌成 1 个**。现在键里带下标
  （`matIdx == 0` 保持旧键，老场景文件不失配）。

**W0-2（稳定身份）**
- `Material::id`，glTF 取 `模型路径#<glTF 材质名>`，同名追加 `#2`，匿名叫 `#mat<idx>`。
- 实测 7 条 `ASSET-MAT-ID`：`#Skin/#Hair/#Cloth/#EyeWhite/#Iris/#Shoe/#Lash`
  —— 与 glb 里的 `materials[].name` **逐一对应**。
- ⚠ 日志刻意同时打 `key=` 与 `id=`：只打 id 的话，"id 改回下标"的否定测试
  **不会红**（下标也是唯一字符串），这条断言就是摆设。

**W0-3（导入模型的材质覆盖能存能读）**
- 新增 `Material::Params`（12 个可覆盖标量）+ `importDefaults` 快照。
  写端只写"与快照不同"的字段；读端按 id → name → 重载模型 → 占位，四级回落。
- 实测往返：`{"kind":"model","id":"...#Skin","overrides":{"roughness":0.05,"baseColor":[1,0,0,1]}}`。
- ⚠⚠ **最贵的一课：`resolveMaterial` 返回 nullptr = 物体直接消失**。
  下方 `!mesh && !mat && !hasNonMeshContent` 会把实体排除出渲染列表，
  **一个日志都没有**。现在退化成 `#missing` 占位材质 + `ASSET-MAT-MISS`。
  占位名带 `#missing` 后缀是为了**不污染模型真正的缓存键**（否则模型以后
  加载成功时会被 W0-1 的冲突规则改名成 `mat0#1`，从此对不上）。

**W1（材质实例）**
- 值语义：`*inst = *parent`（12 个标量 + 7 个纹理指针），`overridden` 位掩码
  决定"母材质变了要不要跟进"。位序写死 `0x000..0xBFF`，A1 断言锁住。
- `makeInstanceOf` 拒绝二级实例（`MATERIAL-INSTANCE ... rejected=two-level`）。
- Inspector：`INS-MAT-INSTANCE`（`Material (asset)` / `Instance of ...`）、
  `INS-MAT-MAKEUNIQUE`、`INS-MAT-REVERT-<field>`；被覆盖的字段标签**变色**。
- ⚠ **覆盖位的记录方式是"整段 diff"**，不是在 12 个 `fieldEdit` 后面各加一行：
  函数开头存一份 `Params`，末尾 `diffParams` 一次。漏一处就静默不记覆盖，
  而 diff 是"结构上不可能漏"。
- ⚠ `Make Unique` 换掉了材质对象 → **参数基准也要跟着换**，否则本帧末尾的
  diff 会拿旧材质的参数当基准，把所有 12 个字段都算成覆盖。
- 编辑器实测：点按钮 → `MATERIAL-INSTANCE action=make-unique entity=2
  parent=sandbox_box_a_mat name=sandbox_box_a_mat@e2`，材质行那一片
  **显著变化像素 42.1%**（`INS-MAT-INSTANCE` 矩形内，阈值 8）。

**否定测试（每条都真的看到红，再还原看绿）**
| 故意改坏 | 实测结果 |
|---|---|
| `makeMaterialPBR` 永远复用同名 | 探针变 `A=0.20 / B=0.20`，`ASSET-MAT-COLLIDE` 0 条 |
| `gltfMaterialId` 改回数组下标 | 7 个 id 全变成 `#mat0..#mat6` |
| 去掉 `ASSET-MAT-MISS` 日志 | `ASSET-MAT-MISS` 0 条 |

⚠ 做否定测试时踩到一次**假绿**：第一次给 `makeMaterialPBR` 打补丁的 Python
脚本 `assert` 失败（文件是 **CRLF**，而我按 LF 拼的 old_string），构建**没变**，
于是跑出来的"两行 0.2/0.8"让我以为否定测试没生效 —— 其实是**改动根本没进去**。
教训：改坏之后必须先确认 `grep -c "error C"` 为 0 **且**二进制时间戳变了。


### 19.2 W2 的实测数字与坑

**W2-1（`.vmat`）**
- 读端 `kind == "vmat"` 的故事：读文件 → 把 doc 的 `kind` 改成 `procedural` →
  **递归回 `resolveMaterial`**。一套解析器两处用 —— 多一套就会有两套 bug。
- ⚠ **id 缓存不是正确性、只是省解析**：把 `findMaterialById` 短路掉之后，
  两个实体仍然拿到同一个材质（W0-1 的"参数逐字段相同 → 真共享"兜住了），
  只有 `ASSET-VMAT` 变成 **2 条**。所以 D1 断的是"解析一次"，
  D2/D3 断的是"写回形状 + 参数真的生效"，三条各管一件事。
- ⚠ **像素断言要"自校准"**：判据是"左右半屏的显著变化占比都 > 0.5% **且**
  > 10× 同配置连开两次的基线"。实测基线 **0.000%**（显式 `SetWindowPos(0,0)`
  之后两次启动逐像素一致），信号 8.16% / 3.86%。
- ⚠⚠ **`stdout=PIPE` 又不读 = 子进程卡死**：Windows 匿名管道只有几 KB，
  `Game.exe` 光启动就写 5.4 KB → 子进程卡在 `write` 上，**窗口都没建出来**，
  `find_window` 空转 20 s 返回 0。症状是"抓图全 None、差异全 0.000%"，
  完全看不出跟窗口有关。改成落文件。

**W2-2（`.vmeta`）**
- ⚠⚠ **`vmetaPathFor` 必须"追加"而不是"替换"扩展名**。替换式会让
  `anime_girl.glb` 和 `anime_girl.fbx` 抢同一个 sidecar，后加载的直接把前一个的
  材质映射冲掉（症状：换个模型材质全变成另一个的）。现在叫 `xxx.glb.vmeta`。
- ⚠⚠ **`beginModelImport` 里"算 hash"必须在"读 sidecar"之前**。反过来的话
  `loadVmeta` 会把 `meta.sourceHash` 覆盖成文件里记的旧值，于是
  "当前 == 记录" 恒成立 → `changed` 永远是 0（源变了也不重导）。
- ⚠ **"文件里有、源里没了"的名字要靠一个独立的 `seen` 集合判**。拿
  `meta.materials` 当"见过的"是错的 —— 它一开头就是文件的副本，于是
  `REMAP-FAIL` 变哑巴（实测：`changed=1` 但 0 条 REMAP-FAIL，`Ghost` 还留着）。
- ⚠ 陈旧的映射 **必须删掉**：留着的话每次加载都再报一次假警报，而且那个名字
  以后被重新用上时会继承一条陈旧 id。
- ⚠⚠ **ufbx 的 `target_unit_meters` 与"模型大小"是倒数关系**（实测：
  1.0 → 半径 0.822；0.5 → 1.644）。所以 `.vmeta` 的 `scale`（尺寸倍数）
  要写成 `1.0f / scale`。写反的话填 0.5 会把模型**放大**一倍，而"设置生效了"
  看起来完全正常。
- ⚠ FBX 侧顺手修了一处不一致：`fbxMaterial` 原来把 **subName**（`模型/mesh_0`）
  当 `modelName` 传给 `id` 和 `source.setModel()` —— id 变成 `模型/mesh_0#Skin`，
  与 glTF 的 `模型#Skin` 对不上，`.vmeta` 的映射表也就无从对齐；`setModel`
  拿着 subName 也重载不了模型。现在 `keyPrefix`（缓存键）与 `modelKey`（身份）分开。

**W2-3（蒙皮 LOD）**
- 分组键的两层：外层空间格（现有 `cellKey` 吃掉 48 位），内层 **主 joint**。
  用嵌套 map 而不是把 joint 混进 64 位键 —— 混进去会撞键，撞一次的后果是
  两根骨头的顶点被合到一起，且**没有任何报错**。
- joint 下标**不能求平均**（3 和 5 平均成 4 是另一根骨头）。做法：每格按 joint
  累积权重 → 取前 4 → **归一化到 1**。不归一化顶点会被吸向原点（模型缩水）。
- ⚠⚠ **自检第一版是假绿**：`gridRes=4` 时格边长 0.5、格线正好落在 `x=0`
  （两根骨头的分界），于是**没有任何格子跨过接缝** —— 把 joint 分组整个去掉
  仍然 `pass=9 fail=0`。改成 **奇数 `gridRes=5`**（格线在 -0.6/-0.2/0.2，中间那格
  真的横跨两根骨头）后，去掉分组立刻 `notOne=6 cross=12` 全红。
- 实测 `anime_girl.glb`：`LOD-BUILD kind=3 levels=4 tris=1648/814/382/184`。

**W2 否定测试（每条都真的看到红，再还原看绿）**
| 故意改坏 | 实测结果 |
|---|---|
| `.vmat` 的 id 缓存短路（`if (false && !id.empty())`） | `ASSET-VMAT` **1 → 2 条**（D1 红）；D2/D3 仍绿（真共享兜住，符合预期） |
| 减面分组键去掉 joint（`prim = 0`） | `LOD-T FAIL B2 notOne=6 / B4 cross=12` |
| `.vmeta` 的 `seen` 改用 `materials` 判 | `REMAP-FAIL` 0 条、`Ghost` 残留在映射里 |
| `scale` 直接写进 `target_unit_meters`（不取倒数） | F1 `r0.5=1.644 / r1=0.822` 比值 **2.000**（放大而不是缩小） |

### 19.3 W3 的实测数字与坑

**关卡本身**
- 373 个实体、8 个母材质。生成器用 **固定种子 + 有序遍历**，同一份脚本跑两次
  逐字节相同（`--check` 可以直接比对）。
- ⚠ **木箱用"单位立方体 × scale"，不要给每个尺寸单独生成几何**：图元网格
  只有一份，346 个木箱共享它；尺寸写进 `primitive` 会让每个箱子各生成一份。

**踩到的坑（每条都花了时间）**
| 现象 | 根因 | 修法 |
|---|---|---|
| 关卡里物体全部"浮空"、地面整片不见 | `camera.pitch` 写 **-0.279** 把相机放到了地面**下方**；地面法线朝 +Z、单面渲染 → 整片被背面剔除 | `pitch` 写**正**值。⚠ 画面上只是"地面没了"，日志一句提示都没有 |
| 把 `ground.vmat` 改成 `doubleSided` 后整屏变黑 | 那是从**下方**看到的背面（光照按翻转后的法线算） | 别用 doubleSided 遮症状，先修机位 |
| 场景往返永远差几项 | ① 生成器没写 `material: null`（写端会写）② 球体的 `segments/rings` 在 `mesh` 块里也要写（`primitive` 块是**另一套**） ③ 写了默认值的 `physics` 块（写端只写**非默认**） | 生成器按写端的规范化形状写 |
| 改母材质画面只变 **0.88%** | 探针选了 `roughness` —— 这套光照下它只改高光尺寸（直射项是 `sunColor*NdotL*shadow`，没有 IBL 反射） | 探针换 `baseColor`（直接进 albedo）→ **19.56%** |
| 同一份场景连测两次差 2 倍（19.0 / 51.8 ms） | 后台负载 / 热节流 | 性能类断言**取多次最小值**，阈值按"别病态"定，不按 60 fps 定 |
| `camera.pitch` 写成度数 `-18.0` | `Camera::setPitch` 直接 `clamp(p, ±1.5533)`，单位是**弧度** | 生成器写弧度（-0.3141593） |

**性能口径（重要）**
- GPU 帧时间 **11.1 ms < 16.7 ms** —— "60 fps"在 GPU 侧成立。
- CPU 侧 **48.5 ms/帧**（Debug 构建）：其中 ~24 ms 是引擎每帧固定开销
  （空沙盒也要 24.5 ms），373 个实体只多 **+24 ms（0.064 ms/实体）**。
  ⚠ 这印证了 §1 的实测（"50→500 耗时不变"是**模拟**侧），同时也说明
  **CPU 侧要 60 fps 必须换 Release**，这里不假装已经达到。
- MDI 打开反而更慢（`MYVK_MDI=1` 实测 61.9 ms/帧）——批处理仍在每帧重建。

**验收脚本六组的判据**
| 组 | 判据 | 实测 |
|---|---|---|
| A | 373 实体 / 8 个母材质各解析一次 / 角色 clip=Walk 且 joints>0 | 8 条 `ASSET-VMAT`、21 joints |
| B | 读→写往返逐字段相等（float 1e-6） | **0 处不同** |
| C | GPU < 16.7 ms；373 实体增量 < 60 ms（Debug） | 11.1 ms；+24.0 ms |
| D | 改 `.vmat` 的 baseColor → 显著变化 > 5%（自校准基线） | **19.56%**，基线 0.00% |
| E | 关太阳后室内可见；点光也关 → 明显更暗 | 0.268 vs **0.014**（19×） |
| F | 单件覆盖往返仍是 instance + 1 个 override | roughness 0.12 |

⚠ E 的关键：**只关太阳不够** —— 天空 IBL 会把室内照到 0.638（vs 灯灭 0.571，
只差 12%），必须同时 `ambientScale = 0` 才测得出"点光在干活"。

### 19.4 W4 的实测数字与坑

**资产与格式（W4-1）**
- `assets/terrain/hills/`：**257×257 采样点 / 2 m per 采样点 = 512×512 m**、
  `maxHeight` 60 m、4 层（草/土/岩/雪）、**64 块**（每块 32 格 = 64 m）、
  4 级 LOD（步长 1/2/4/8 格）。三件套 `height.r32` + `splat.rgba8` + `terrain.vterrain`。
- `--selftest terrain` **pass=27 fail=0**。其中：
  - **A4** 内存里的高度 == 文件里的 float（自己再读一遍 `.r32` 逐 `memcmp`）；
  - **C3** `maxGap = 0.8429 m < skirtDepth 2.0 m` —— 这是"裙边能不能兜住
    相邻 LOD 裂缝"的**唯一**判据，不是靠眼力；
  - **D3** 30° 坡稳态 `worstStep=0.00000 drift=0.00000 grounded=90/90 nz=0.866`；
  - **D4** 60° 坡滑落 **39.4 m**；**D5** 无高度场时回落 `z=0.90000`（= 胶囊半高）。
- 权重分布（`gen_terrain.py` 的输出）：`min=0.1006 max=0.8589 mean=0.4061`。
  草是**默认覆盖层**，其余三层"抢占"它的权重（第一版反过来写 —— 草按高度衰减
  → 台地上一棵草都没有，整片米黄）。

**渲染（W4-2）**
- `TERRAIN-PIPELINE sets=2 cull=none`。**set 0 复用的是主前向的同一份
  `VkDescriptorSetLayout`、push range 写满 128 B** —— 否则 set 0 的绑定被判
  "被打断"，后面所有 draw 报 VUID-08600（这条是本轮最大的坑，见下表）。
- LOD 实测：贴地 8 m → `lod0=6 lod1=24 lod2=34 lod3=0` / **35712 tris**；
  把机位搬到地形外 900 m → `lod0=0 ... lod3=64` / **4096 tris（11.5%）**。
- 4 层混合：`albedo = Σ w_i·albedo_i`、`normal = normalize(Σ w_i·normal_i)`。
  12 万条 `TERRAIN-LAYER` 日志里，`albedo=default normal=default` 是**预期**的 ——
  当前 4 个 `.vmat` 都没挂贴图，层色来自 `LayerBlock` 的 baseColor。

**物理（W4-3）**
- 不改 narrowphase：`physics::Heightfield` 只提供 `heightAt`（双线性 O(1)）/
  `normalAt`（中心差分，步长**半像素**，与 `terrain.frag` 同一个算子）/
  `raycast`（步进 + 24 次二分）。
- 接进 `CharacterController` 的方式是"**先问高度场、再问物理体、取更近的**"，
  `ground = nullptr`（默认）时 ⑥ 的代码路径与引入前**逐位相同** —— 现有
  A/B/C/D/E 五组 `groundPlane` 用例一个都没动。

**编辑器（W4-4）**
- 双击 `.vterrain` → 实体 + 立即渲染；**同一份资产重复双击 = 选中已有的那个**
  （判据是 `chunks` 仍是 64，不是 128 —— 叠一座山画面上几乎看不出来）。
- Inspector Terrain 段：`INS-TERRAIN-{HEADER,ASSET,VISIBLE,HEIGHTSCALE,
  LAYER0..3,REMOVE}` 十个锚点。只读（`.vterrain` 的尺寸/maxHeight/分块数）与
  可改（visible / heightScale / 4 层覆盖）**分区呈现**。
- `Add ▸ Terrain` 落的是**平地三件套**（65×65 @ 2 m = 128 m、4 块）；
  测试里"建完立刻双击打开"验证了它不是空壳（`chunks=68` = 64 + 新建的 4）。

**踩到的坑（每条都花了时间）**
| 现象 | 根因 | 修法 |
|---|---|---|
| 地形外沿高度差 **1.7e-4 m**（台阶状） | `bilerpOf` 落在**最后一个采样点**时 `floor` 给出 `N-1`，而格子只有 `N-2..N-1` → 权重算成 **2.0**（外插）→ `2·h[N-1] − h[N-2]` | `if (c0 >= cols-1) { c0 = cols-2; fc = 1.0f; }`；自检取**正好**最后一个采样点 |
| 自检 D1~D3 全红、**D4 反而通过** | 斜面上胶囊中心到底面的**竖直**距离是 `(halfHeight+radius)/n.z` —— 30° 坡是 **1.04 m** 而不是 0.90 m。按 0.90 摆 = 已陷进 0.12 m → `excess < -0.02` 判穿透 → 一路自由落体。而 D4 断言的是"往下滑"，自由落体也算 | 起点改成从空中落下（`heightAt + 1.5`，`onGround=false`）；D4 也先落 60 帧再断言 |
| LOD 验收假绿（near `lod0=6` vs far `lod0=7`） | `Camera::setDistance` 把场景里的 distance **钳在 0.5~100 m** —— 写 `700` 出来还是 100，"700 m 高空机位"根本不存在 | 把 **target 搬到地形外 900 m**（target 只是一个看向的点）→ far 全 lod3 |
| `terrain.frag` 里阴影和 `pbr.frag` 不一致 | CSM 三件套（`selectCascade`/`pcfCascade`/`shadowFactor`）必须**逐行一致**：同一张影子图、同一套 Zero-to-One 偏置（`proj.z` 不能再 `*0.5+0.5`）、同一套距离淡出 | 直接抄 `pbr.frag` 对应函数，一个字都不改 |
| 场景往返永远差 `mesh`/`material` 两项 | 写端对"没有网格的实体"**无条件**写 `"mesh": null, "material": null`（`first_level.json` 的灯与角色根实体也是这么写的） | 生成器显式写这两个 null —— 不是改写端（那会让已有场景全体失配） |
| 4 个层槽"画出来了但断言全 None" | `verify_add_menu.RECT_RE` 的 tag 字符类是 `[A-Za-z-]+`，**不含数字** → `INS-TERRAIN-LAYER0` 解析不出坐标，`last_rect` 静默返回 None | 字符类放宽到 `[A-Za-z0-9_-]+`。⚠ 这就是 MEMORY 里那条"锚点拼错 = 断言永远为假"的又一例 |
| 点 Inspector 的勾选框，画面 **0.00%** 变化 | 内容抽屉（底栏往上弹 320 px）**压在 Inspector 下半截**上：被裁掉的控件 `GetItemRectMin/Max` 照样有值，但点下去点到的是抽屉 | 测 Inspector 之前先点 `BAR-BTN-content` 把抽屉收起来（收回后同一断言 **76.29%**） |
| 自造的 `MYVK_EDITOR_CB_DIR` 钩子不生效 | `refresh()` 在 `!m_initializedOnce` 时会把 `m_rel` 重置成 `"assets"`，钩子跑在它前面就被吃掉（日志 `CB-ENTRIES dir=assets count=10`） | 用引擎现成的 `MYVK_CONTENT_DIR=assets/terrain/hills` |
| 地形管线"画了但 Inspector 说没加载" | `Renderer::m_textureSetLayout` 在 init 里是**建完管线之后**才创建 → 地形的缺省贴图必须**惰性创建**，`setTextureContext()` 每次建实例前刷 | `TerrainPass::setTextureContext` + 惰性 `m_defaultAlbedo/m_defaultNormal` |

**验收脚本（两组）**
| 脚本 | 组 | 实测 |
|---|---|---|
| `verify_terrain.py`（16 条，~40 s） | A 组装 / B 往返 / C LOD 按距离 / D 改层材质 / E 关地形 / E' 渲染层整条跳过 / F GPU / G `--selftest terrain` | 64 块 37696 tris；往返 0 处；far 11.5% tris；76.47% / 76.41%；8.22–9.84 ms；27 条自检全绿 |
| `verify_terrain_editor.py`（23 条，~40 s） | A 双击添加 / B 不叠第二份 / C 段内控件 / D 关可见 / E Ctrl+S 往返 / G Add ▸ Terrain / F 重开恢复 | 32.03%；chunks 64→64；4 槽 y=727/762/797/832；76.29%；terrain 块往返；新建资产 `chunks=68`；重开 `tris=30848` |

⚠ 两组都是 **GUI 用例**（前者 4 次开窗口抓图，后者真鼠标双击 + 前台 Ctrl+S），
不能与其它 GUI 用例并行跑。

（每完成一项，在此追加"实测数字 + 踩到的坑"，与 `audio_roadmap.md` §13 同款。）

---

## 20 · 建议的第一次动手

如果只做一件事，**做 W0-1**（10 行）：

```cpp
// engine/assets/AssetManager.h:476
auto it = m_materials.find(name);
if (it != m_materials.end()) {
    if (sameParams(it->second.get(), albedo, normal, orm, baseColor,
                   roughness, metallic, emissive, doubleSided))
        return it->second.get();
    name = name + "#" + std::to_string(++m_matConflictSeq);
    VK_LOG_WARN("ASSET-MAT-COLLIDE name=%s renamed=%s", orig, name.c_str());
}
```

它堵住的是"复制粘贴实体 → 材质静默串味"这条**每天都在发生**的漏水。
修完之后，`assets/scenes/runtime_test/mat_conflict_probe.json` 应该 dump 出两行不同的
roughness —— 那就是"材质实例"这条链的第一个可观测的进展。
