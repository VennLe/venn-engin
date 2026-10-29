# 存档 / 引用 / Prefab 路线图（G16 ~ G19）

> **这份文档是权威**。`ship_roadmap` 的 G16~G19 只保留指针。
> 动手前先读 §1「现状实测」——里面的四条是**在磁盘上量出来的**，不是推断；
> 其中有两条会把"照抄 Unity 做法"的方案直接否掉。

---

## 0 · 一页纸结论

| 问题 | 今天 | 方案 | 任务 |
|---|---|---|---|
| 存档对不上号 | `Entity.id` 是运行时序号（`Entity.h:19`），重启/撤销/Play 都变 | `World::Record` 加 `guid`（64 位），双向索引 | **G16** |
| 引用对不上号 | 组件里**唯一**的实体引用是 `HierarchyComponent::parent`（`Components.h:52`），存的是 `EntityId` | 组件内引用**继续用 id**（同一次加载内有效）；**跨文件**引用用 guid | G16 |
| Prefab 覆盖对不上号 | 无 Prefab | 实例成员带 `(prefabGuid, localGuid)` + **显式覆盖路径表** | **G18** |
| 只有整份关卡 | `SceneSerializer` 只能整存整取 | 增量存档 = 基线哈希 + diff | **G17** |
| 没有槽位 / 迁移 | `version` **只写不读**（`SceneSerializer.cpp:29` / `:1362`） | 版本号 + 迁移函数表，**第一版就空跑一次** | G17 |
| 关不掉的异步 | `sceneFromJson` = 解析 + 上传 混在一个函数里 | 劈成 **parse / instantiate / resolve** 三段 | **G16-5** |
| 一加载整个世界 | 无分块 | `LevelManifest` + 触发器驱动 + 两段式异步 | G19 |

**执行顺序（不可并行，有硬依赖）**：

```
G16-1 World::guid  →  G16-2 场景 JSON   →  G16-3 自检  →  G16-4 编辑器接回
                                                    ↓
                                          G16-5 序列化器三段拆分
                                                    ↓
                        ┌───────────────────────────┴──────────┐
                     G17 存档                              G19 流送
                        ↓                                      ↑
                     G18 Prefab ──────────────────────────────┘（Prefab 不改流送，但共享 localGuid 机制）
```

**为什么 G16 必须是第一个**：G17 的每条存档记录、G18 的每个覆盖项、G19 的每个跨块父子引用，
键都是 guid。**先做任何一个，都会在三个地方各写一遍"临时用 id 顶着"，然后再拆一次。**

---

## 1 · 现状实测（`grep` / 读文件得到的，不是推断）

| # | 事实 | 位置 | 造成的后果 |
|---|---|---|---|
| **H1** | 组件里持有实体引用的地方**只有一处**：`HierarchyComponent::parent`（`EntityId`） | `engine/ecs/Components.h:52` | ✅ 好消息：G16 **不用**改 20 个组件的引用语义。⚠ 但意味着"引用"这件事今天几乎不存在，G17/G18 要**新建**引用机制 |
| **H2** | `"version"` 只被写，**读端从不校验** | `SceneSerializer.cpp:29`（写常量）、`:1362`（写字段）；`loadSceneFromJson`（`:1580`）全程没有 `contains("version")` | 迁移函数表**没有挂载点**。现在加 `kSaveVersion` 是"从 0 搭"，不是"复用" |
| **H3** | `sceneFromJson` 是 **解析 + GPU 上传** 的混合体：`resolveMesh` → `AssetManager::cube()` → `Mesh::upload(device,…)` | `SceneSerializer.cpp:388`、`:1682` | ① 无设备**无法**测读档；② G19 的"后台解析 / 主线程上传"**无处落脚**（必须重写这个函数才能拆线程） |
| **H4** | 撤销是**整场景 JSON 快照**：`structuralEdit` 抓 before/after 存 `SceneSnapshotCommand` | `editor/EditorContext.h:391`、`editor/Command.cpp:185` | ✅ 好消息：**guid 只要写进 JSON，撤销/Play 副本/Anim 快照三条路径自动带上 guid**，一行不用改 |
| **H5** | 选中项跨重建靠**名字**接回：`validateSelection` → `Scene::find(name)` → **返回第一个同名** | `editor/EditorContext.cpp:317-335`、`Scene.cpp:167` | 现有真 bug：重名实体撤销一次就选错。G16-4 顺手修掉 |
| **H6** | `duplicateEntity` 是**手写字段拷贝**（逐组件 `w.add`） | `editor/SceneHierarchy.cpp:59-95` | ⚠ "改一处要同步改 N 处"的第 6 个点：新增组件必须同步；**guid 绝不能拷** |
| **H7** | **37 个**用例**手写场景 JSON**，都用 `"parent": -1` | `tools/verify_*.py`（`"parent"` 共 **59 处**） | ⚠ **不许**把 `parent` 改成字符串，否则 37 个用例同时红 |
| **H8** | 用例读 `--dump-scene` 的方式是"**源的 key 必须都在 dump 里**"，不是全文相等 | `verify_runtime_m2.py:282` | ✅ 加 `guid` 字段**不会**打破现有往返断言（多出来的 key 没人管） |
| **H9** | `CMakeLists` 用 `GLOB_RECURSE CONFIGURE_DEPENDS engine/*.cpp` | `CMakeLists.txt:159` | ✅ 新增 `.cpp` 不用改 CMake（但会触发一次 reconfigure） |
| **H10** | `ContactEvents` 的键已经是 `(min<<32)\|max` 的 id 对，注释里**预留了换 guid** | `engine/physics/ContactEvents.h:17` | G16 之后可换成 guid，**接口不变**。不阻塞，可选 |

> ⚠ **H7 是这份方案与"标准做法"最大的分歧点**：Unity/Godot 的场景文件里父引用是"文件内 id"，
> 我们会**保留 `parent` 整数下标**。理由：下标由写端自己生成、文件内自洽；
> 改成字符串的唯一收益是"文件可重排"，代价是 37 个用例同时红。
> **跨文件**的父子（Prefab / 分块流送）用**新增的 `parentGuid`**，两条路并存（§3.3）。

---

## 2 · 三个决定（以及为什么不是别的）

### D1 · guid 放 `World::Record`，**不**放 `Entity` 句柄，也**不**做组件

| 方案 | 否决理由 |
|---|---|
| 塞进 `Entity{ id, generation, guid }` | ⚠⚠ `World::handle(id)` **从裸 id 造句柄**，它不知道 guid → 造出来的句柄 `guid == 0`。而 `Scene::setParent`（`Scene.cpp:26`）和 `worldMatrix` 的父链循环（`:54`）**都在调 `handle()`**。于是"沿父链取 guid"会静默拿到 0 —— 日志全对、行为全错，最难查的一类 |
| 做成 `IdentityComponent { uint64_t guid; }` | 每次 `guidOf(e)` 要走 `type_index` 哈希 + 稀疏集查表；`each<IdentityComponent>` 会多一次全量遍历；且组件池是"会被序列化器统一遍历"的语义，guid 不该走那条路（它要显式写在实体顶层） |
| **`World::Record` 加字段**（✅ 采用） | `Record` 本来就是"每实体的非组件态数据"（`generation` / `alive` / `name` 已经在里面，而 `name` **同时也**在 `NameComponent` 里 —— 这个类已有"DAL 重复存一份"的先例）。`guid()` 是 O(1) 数组下标；`find(guid)` 走一张 `unordered_map<guid, EntityId>` |

**代价**：`Record` 从 40 → 48 字节。**收益**：只改 `engine/ecs/World.{h,cpp}` 一个文件，且**所有**
创建路径（包括 `Scene.cpp:73` 直接 `m_world.create("DirectionalLight")` 那条绕过 `createObject` 的）
自动覆盖。

### D2 · 存档 = **逐字段覆盖**，**不**是"删了重建"

读档时**绝不**重建实体。理由：重建会换 `Entity.id` → 层级 `children` 列表、选中项、物理体、
脚本持有者全部失效。做法：按 guid 找到实体，把存档里的字段**覆盖**上去。

⚠ **由此得出必须写进契约的一条**：**存档只改值，不增删组件**。
"捡走一把枪"用 `_destroyed` 表达（销毁整个实体），不用"删掉某个组件"。
这条限制换来的是读档路径**不需要 AssetManager、不需要 GPU**（H3 的痛点绕开了）。

### D3 · Prefab 覆盖 = **显式路径列表**，**不**是"值与默认值比对"

理由：值与默认值比对在"prefab 源改过"之后必然失效 —— 场景里存的旧值全都 ≠ 新默认值，
于是全被当成覆盖，**改源不生效**（Unity 早期就栽在"隐式覆盖"上）。
所以覆盖必须**显式记录"哪些字段被改过"**，值仍写在正常字段里（§6.3）。

---

## 3 · G16 · 稳定 GUID

### 3.1 数据模型

```cpp
// engine/ecs/Entity.h —— 只加一个别名和一个哨兵，句柄结构**不动**
using EntityGuid = std::uint64_t;
constexpr EntityGuid kInvalidGuid = 0;   // 0 = 没有身份（不参与存档）
```

```cpp
// engine/ecs/World.h
EntityGuid guid(Entity e) const;          // 无效实体 / 无 guid → kInvalidGuid
Entity     find(EntityGuid g) const;      // 找不到 → Entity{}
bool       setGuid(Entity e, EntityGuid g);  // 已被别的存活实体占用 → false（调用方补发）
EntityGuid allocateGuid();                // 由 create() 内部调用，也供自检单测

// World.cpp 的 Record
struct Record {
    uint32_t    generation = 0;
    bool        alive = false;
    std::string name;
    EntityGuid  guid = kInvalidGuid;      // ← 新增
};
std::unordered_map<EntityGuid, EntityId> m_byGuid;   // ← 新增
```

**格式**：JSON 里是 **16 位小写 hex 字符串**，`"8f3a2b1c9d4e5f60"`。

> ⚠ 为什么不是 JSON number：`nlohmann` 能存 `uint64`，但**任何**中间工具（Python 的 `json` 会变
> float、JS 直接丢精度、Excel 会四舍五入）都会把它改成 `8.917e+18`。存档是**要被别的工具读**的，
> 字符串是唯一安全的表示。⚠ 解析必须走 `std::from_chars(…, 16)` 或 `sscanf("%16llx")`，
> **不要** `std::stoull(s, nullptr, 16)`（它会接受 `"0x..."` 和尾随垃圾，且抛异常）。

**分配**：

```cpp
// 进程级单例，不在 World 里 —— World 各持一份的话，"清空再加载"会重放同一串 guid，
// 场景合并时交集不为空（正是 G16 验收 3 要抓的）。
static std::mt19937_64 g_rng{seedFromRandomDevice()};
EntityGuid allocateGuid() {
    for (;;) {
        EntityGuid g = g_rng();
        if (g != kInvalidGuid) return g;   // 0 保留
    }
}
```

> **不用"时间戳 + pid + 计数"**：时钟回拨会撞，pid 只有 15 位有效，计数在重启后归零；
> 64 位均匀随机的碰撞概率在 100 万实体下是 2.7e-8，够用且没有隐藏假设。
> **不加 `MYVK_GUID_SEED`**：实测**没有任何用例做 dump 的全文比对**（H8），随机不会造成抖动；
> 真需要复现时再加，别提前加开关。

### 3.2 生命周期三处必须同步

| 位置 | 动作 |
|---|---|
| `World::create` | `r.guid = allocateGuid(); m_byGuid[r.guid] = id;` |
| `World::destroy` | `m_byGuid.erase(r.guid); r.guid = kInvalidGuid;`（**在** `m_free.push_back` 之前） |
| `World::clear` | `m_byGuid.clear();` |

**`setGuid` 的顺序陷阱**（这是本任务唯一的真 bug 风险）：

```cpp
bool World::setGuid(Entity e, EntityGuid g) {
    if (!alive(e) || g == kInvalidGuid) return false;
    auto it = m_byGuid.find(g);
    if (it != m_byGuid.end() && it->second != e.id) return false;  // 他人占用
    Record& r = m_records[e.id];
    if (r.guid != kInvalidGuid) m_byGuid.erase(r.guid);   // ⚠ 先摘旧键，再插新键
    r.guid = g;
    m_byGuid[g] = e.id;
    return true;
}
```
> ⚠ 忘了 `erase(r.guid)` 的后果：`create()` 先给的临时 guid 永远留在 map 里 →
> 一个"幽灵实体"能被 `find()` 命中，而且 `clear()` 之前一直存在。**这条必须进否定测试。**

### 3.3 场景 JSON 改动

只加字段，**不改既有字段的类型**（H7）：

```json
{
  "version": 2,                                   // ← 1 → 2（H2 的挂载点）
  "entities": [
    { "guid": "8f3a2b1c9d4e5f60",
      "name": "Crate",
      "parent": 3,                                // ← 不变：本文件内的下标
      "parentGuid": "1a2b3c4d5e6f7788",           // ← 新增，仅当父**不在本文件**时写
      "transform": { … } },
    …
  ]
}
```

**写端**：`guid` 每实体必写（顶层，紧跟 `name` 之后，**别插在 `parent` 后面** —— 让人读文件时
第一眼看到身份）。`parentGuid` 只在"父实体不在本次导出集合里"时写（Prefab 子树的根、
G19 分块的边界实体）。

**读端**（严格按这个顺序，别图省事）：

```cpp
// 1. 第一遍建实体时就取 guid，早于任何其它字段
const std::string gs = je.value("guid", std::string());
EntityGuid want = parseGuidHex(gs);           // 非法 / 空 → kInvalidGuid
// ⚠ create() 已经给了随机 guid，这里要用 setGuid **覆盖**
if (want != kInvalidGuid && !w.setGuid(e, want)) {
    ++dupCount;                                // 撞了 → 保留 create() 给的，记 dup
    VK_LOG_WARN("SCENE-GUID-DUP guid=%s action=reassign entity=%u", gs.c_str(), e.id);
} else if (want == kInvalidGuid) {
    ++assignedCount;                           // 老场景 / 手写用例 → 补发
}
```

**父引用解析**（第二遍）：

```cpp
if (je.contains("parentGuid") && je["parentGuid"].is_string()) {
    const EntityGuid pg = parseGuidHex(je["parentGuid"].get<std::string>());
    const ecs::Entity p = w.find(pg);                 // 跨文件 → 按 guid
    if (p.valid()) scene.setParent(e, p);
} else {
    // ⚠ 必须显式判 is_number_integer()。`je.value("parent", -1)` 遇到字符串会
    //   抛 type_error.302 —— 而 20 个用例写的是 `"parent": -1`，两种类型都会出现。
    const int idx = (je.contains("parent") && je["parent"].is_number_integer())
                        ? je["parent"].get<int>() : -1;
    if (idx >= 0 && idx < (int)created.size() && created[idx].valid())
        scene.setParent(e, created[idx]);
}
```

**汇总锚点**（一次加载一条，别每实体一条）：

```
SCENE-GUID assigned=3 dup=0 total=42 path=scenes/xxx.json
```

> ⚠ **修正 `ship_roadmap` 的一处措辞**：原文验收 2 说"补发的 GUID 在同一次会话内稳定"。
> 随机补发**做不到**这件事（同一个老文件加载两次，两批 guid 不同）。可做到的是：
> **同一个 World 内稳定**，且**首次保存后固化**。验收改成这两条（§3.5 的 `guid_legacy_*`）。

### 3.4 编辑器接回（G16-4）

`validateSelection` 改成 **guid 优先、名字兜底**：

```cpp
// m_selectionGuid 是新成员：选中时抓一次，之后一直用它
if (!m_selection.valid() && m_selectionGuid != ecs::kInvalidGuid) {
    const ecs::Entity found = sc.world().find(m_selectionGuid);
    if (found.valid()) { m_selection = found; … return; }
}
// 兜底：老路径（名字）。失败就打一条
VK_LOG_WARN("INS-SELECT reconnect failed name='%s' guid=%016llx", …);
```
> ⚠ 这条修的是 **H5 的现网 bug**（重名 → 撤销一次选错）。改完必须做否定测试：
> 造两个同名实体，选中第二个，删掉第一个，Ctrl+Z —— 修前会跳到第一个，修后停在第二个。

顺手加两条 Hierarchy 右键命令（`Copy GUID` / `Select by GUID…`），
锚点 `HIER-GUID copy=%016llx`。**不在 Hierarchy 里常显 guid**（噪音）。

### 3.5 验收（`Game.exe --selftest guid`，无窗口无设备）

| # | 断言 | 判据 |
|---|---|---|
| 1 | `guid_unique_100k` | 10 万次 `allocateGuid()`，`unordered_set` 大小 == 100000 |
| 2 | `guid_never_zero` | 100 万次里 0 个 `kInvalidGuid` |
| 3 | `guid_find_roundtrip` | 1000 实体，`find(guid(e)) == e` 全真 |
| 4 | `guid_set_duplicate_rejected` | 把 A 的 guid 塞给 B → `false`，A 的 `guid()` 不变 |
| 5 | `guid_no_ghost_after_set` | `setGuid(e, x)` 后旧 guid 的 `find()` 必须**失效**（3.2 的坑） |
| 6 | `guid_destroy_unmaps` | destroy 后 `find(guid)` 失效，且 guid 不复用给新实体 |
| 7 | `guid_clear_unmaps` | `clear()` 后全部 `find()` 失效 |
| 8 | `scene_guid_roundtrip` | 场景 A → json → 场景 B，两份 `guid` 集合**完全相同** |
| 9 | `scene_guid_stable_twice` | B → json2 → C，`json2 == json`（逐字节） |
| 10 | `scene_guid_assigned` | 手写场景（无 guid，10 实体）→ `SCENE-GUID assigned=10`，且 10 个互不相同 |
| 11 | `scene_guid_dup_reassign` | 手写场景把两个实体写成同一 guid → `dup=1`，加载后 10 个仍互不相同 |
| 12 | `scene_merge_no_collision` | 两个 World 各 1000 实体，guid 集合交集 == 0 |
| 13 | `scene_parent_by_guid` | 手工加 `"parentGuid"` 指到另一文件的实体 → 父子关系成立 |

**可测性前提**：8~13 需要 `Scene` + `sceneToJson/sceneFromJson`。`Scene` 是 `= default`（无设备），
`sceneFromJson` 只要 fixture **不含 mesh**，`resolveMesh` 会在 `if (!jm.is_object()) return nullptr;`
（`SceneSerializer.cpp:389`）直接返回 —— 所以**可以**用一个 `assets::AssetManager{assets::TextureContext{}}`
（device 全空）跑完整个往返。**这一点必须写在测试文件头部注释里**，否则下一个人会以为它需要 GPU。

### 3.6 坑

- ⚠ `Record` 加字段后，`World::create` 的 `m_records.emplace_back()` 造出的是**带默认 guid=0 的 Record**，
  然后 `create` 里立刻赋新值 —— 别忘了赋值在 `m_records.emplace_back()` **之后**（顺序写反会读到 0）。
- ⚠ `World::destroy` 里 `m_byGuid.erase` 要用**旧的** `r.guid`，而 `r.guid` 的清零必须在 erase **之后**。
- ⚠ 存档 / Prefab 读 guid 时**必须**接受"没有 guid 的老文件"，且**不能因为补发 guid 就把场景标记 dirty**
  （否则打开老场景立刻弹"未保存"，用户会骂）。dirty 只在真正编辑时置位。
- ⚠ `engine/ecs/**` 是**横切文件**（`run_regression.py` 的 `GLOBAL_GLOBS`）→ 改 `World.h` 会让
  **全量回归被点名**。按项目纪律：**只有用户明确下令才跑全量**。

---

## 4 · G16-5 · 把序列化器劈成三段（G17 / G19 的硬前置）

### 4.1 为什么必须做

H3：`sceneFromJson` 在同一趟里干了三件事 —— 建实体（纯 CPU）、解析 JSON 里的资源描述（纯 CPU）、
把网格/贴图**上传到 GPU**（主线程、要设备）。后果：

1. **G17 无处落脚**：读档要在**已经有世界**的情况下改字段，硬走 `sceneFromJson` 就等于"清空重建"，
   违反 D2；
2. **G19 无解**：`ship_roadmap` 的铁律是"后台解析 / 主线程上传"，而现在两者**在同一个函数体里交错**，
   想拆线程只能重写；
3. **自检无法覆盖读档**：每次 `--selftest` 都得起设备，跟音频那条"混音不能锁在库里"是同一类错误。

### 4.2 三段 API（✅ 已按此落地，实际签名见 §4.2.1）

```cpp
// engine/scene/SceneDocument.h（新建）—— 纯 POD，不 include 任何 rhi/vulkan 头
struct DocumentEntity {
    std::string     name;
    ecs::EntityGuid guid = ecs::kInvalidGuid;
    ecs::EntityGuid parentGuid = ecs::kInvalidGuid;
    int             parentIndex = -1;      // 文件内下标（与 parentGuid 二选一）
    nlohmann::json  raw;                   // 原样留一份：③ 段与存档 overlay 都用它
};
struct SceneDocument {
    int  formatVersion = 0;
    bool hasEntities = false;
    nlohmann::json globals;                // camera/cameras/light/physics/scriptVars/postVolumes
    std::vector<DocumentEntity> entities;  // 顺序 = 文件顺序（parentIndex 依赖它）
};

// ① 纯 CPU，无设备、无 AssetManager、可后台线程
bool parseSceneDocument(const nlohmann::json& root, SceneDocument& out, std::string& err);

// ② 纯 CPU：清空 + 相机/光照/物理/黑板 + 建实体 + guid + 外壳 + 父子 + 文件夹
void instantiateDocument(Scene&, const SceneDocument&,
                         std::vector<ecs::Entity>& created,                    // 与 entities 一一对应
                         std::unordered_map<ecs::EntityGuid, ecs::Entity>& guidMap,
                         DocumentStats& stats);

// ③ 主线程、要 AssetManager：只碰组件与资产
void resolveDocumentAssets(Scene&, assets::AssetManager&, const SceneDocument&,
                           const std::vector<ecs::Entity>& created, DocumentStats& stats);
```

#### 4.2.1 实际落地与上面草案的三处差异（**以本条为准**）

| 草案 | 实际 | 为什么 |
|---|---|---|
| `EntityDesc` 里每个组件一个 `std::optional<…>` | 只有 `raw`（原始 JSON 块） | 搬组件就得把每个 `xxxFromJson` 一起搬，而它们中间十几个要解析资产（mesh/material/skinned/morph/animator）。那样 ③ 段就变成"第二份实现"，以后加组件必漏改一处 —— 这恰恰是 `SceneEntityIO` 当初抽出来要避免的。**`raw` 让 ③ 段原样复用唯一实现。** |
| 单独一张 `AssetRequest` 表 | 没有；③ 段直接遍历实体 | 同上：拆 `AssetRequest` 等于把 `entityApplyComponents` 按组件再拆一遍。等 G19 真要做"资产预取清单"时再生成，那时才有第二个消费者 |
| `documentToJson(Scene&, const SceneDocument&)` | 未做 | 目前没有消费者（G18 的 Apply 还没到）。等它到了再加，**空接口比错接口便宜** |

> 另：常量从 `SceneSerializer.cpp` 的匿名 namespace 提到 `SceneDocument.h`
> （`kSceneFormatVersion`），`SceneSerializer.cpp` 里只留一个别名 —— 两份字面量会漂，一个别名不会。

**旧 API 变成三段的组合**（调用点零改动）：

```cpp
SceneIoResult sceneFromJson(Scene& s, assets::AssetManager& am, const std::string& t) {
    SceneDocument doc; parseSceneDocument(parse(t), doc, err);
    s.clear();
    instantiateDocument(s, doc, map);
    resolveDocumentAssets(s, doc, am);     // ← 只有这一句要设备
    return …;
}
```

> ✅ 顺带解决的一件小事：`documentToJson(Scene&, const SceneDocument&)` 让"只改版本文档再写回"
> 成为可能 —— G18 的 Apply（改 prefab 文件的某几个字段后写回）直接复用它。

### 4.3 迁移策略（别一次重写 2027 行）

分三步、每步都能编译能跑：

1. **抽 `EntityDesc` 的"值"部分**：先只把 `transform / visibility / locked / 灯光 / 物理 / 音频`
   这些**无资源指针**的组件搬过去；mesh/material/skinned 仍留在原路径（走 `raw` 旁路）。
2. **搬资源槽**：把 `resolveMesh / resolveMaterial / resolveSkinned` 的调用点收进
   `resolveDocumentAssets`，靠 `AssetRequest` 循环派发。
3. **删旧代码**：`loadSceneFromJson` 里剩下的东西全部删掉，改成对三段的调用。

⚠ 每一步都要跑一次 `verify_runtime_m2` 的 C 组（读→写→再读）——那是这条路的唯一护栏。

---

## 5 · G17 · 存档系统

### 5.1 增量语义：**基线哈希**，不做脏标记

**否掉"每个组件写入点打脏标记"**：那要给 40 个组件的所有写入点包一层，漏一个就永远存不上
（而且漏的时候没人发现 —— 静默丢进度，最坏的一种 bug）。

**采用**：关卡加载完成时，对每个实体算一次**规范化 JSON 的 FNV-1a 64 哈希**存成基线：

```cpp
// runtime/SaveGameState.h
std::unordered_map<ecs::EntityGuid, std::uint64_t> m_baseline;   // 1000 实体 = 8 KB
```

保存时对每个存活实体重新算哈希，**只写不同的**：

```cpp
for (each entity) {
    const EntityGuid g = w.guid(e);
    json je = entityToJson(scene, e);       // 与场景文件同格式的一个元素
    const std::uint64_t h = fnv1a(je.dump());
    auto it = m_baseline.find(g);
    if (it == m_baseline.end())      → 新增，写整条
    else if (it->second != h)        → 改过，写整条
    else                             → 跳过
}
for (guid in m_baseline) if (!w.find(guid).valid()) → 写 "destroyed"
```

**成本**：保存时 O(N × 字段数) 序列化，1000 实体约 2~5 ms。存档是低频操作，同步做；
`SAVE-WRITE ms=` 把真实数字打出来，**别拍脑袋加异步**。

> ⚠ **哈希必须基于 `json::dump()` 而不是自写遍历**：`nlohmann` 的 object 默认是 `std::map`，
> key 有序；float 用最短往返表示。两条都保证**同值 → 同字节**。
> 自己遍历 `je.items()` 容易漏字段顺序 / 浮点格式不一致，哈希会无缘无故地"总有变化"。

### 5.2 格式（`engine/scene/SaveGame.{h,cpp}`）

```json
{
  "version": 1,
  "game": "venn",
  "slot": "slot1",
  "timestamp": "2026-10-18T22:41:00Z",
  "playTime": 1234.5,
  "scenePath": "scenes/runtime_test/audio_scene.json",
  "sceneHash": "a1b2c3d4e5f60718",
  "entities": [
    { "guid": "8f3a2b1c9d4e5f60",
      "name": "Crate",
      "transform": { "position": [1,0,2], "rotation": [0,0,0], "scale": [1,1,1] },
      "visible": false,
      "audio": { "clip": "audio/click.wav", "volume": 0.35, … } },
    { "guid": "1a2b3c4d5e6f7788", "_destroyed": true }
  ],
  "globals": {
    "playerGuid": "9c8b7a6f5e4d3c2b",
    "playerHp": 40.0,
    "pickedUp": ["8f3a…", "1a2b…"],
    "vars": { "door.opened": 1.0, "quest.stage": 2.0 }
  }
}
```

**决定与理由**：

- **`entities` 用数组不用对象**：对象键是 guid 时，重复 guid 会被 JSON 解析器**静默合并**
  （掩盖 bug）；数组可以断言"guid 唯一"，还能保序（diff 稳定）。
- **每条是"该实体的完整 JSON"**（与场景文件的元素同构）：应用时直接复用
  `applyEntityJson(scene, e, je)`（G16-5 里从 `instantiateDocument` 抽出来的那个）——
  不写第二套字段解析。
- **存档文件 `dump()` 不缩进**（`root.dump()`）：它不是给人改的，1000 实体缩进能差 3 倍。
  场景文件仍缩进 2 格（人可读是优点）。
- **`sceneHash`** = 关卡加载完成后 `guid` 集合**排序后**的 FNV（**只含 guid，不含值**）。
  改关卡（加/删实体）→ 变；改箱子位置 → 不变。这是"结构指纹"，不是"内容指纹"。

### 5.3 应用 diff

```cpp
SaveApplyResult applySaveGame(scene::Scene&, const SaveGame&);
// 对每条：
//   1. w.find(guid) 无效 → ++missing，跳过（**不报错**，见 §5.7）
//   2. "_destroyed" → world.destroy(e)（同时要从 m_baseline 摘掉）
//   3. 否则 applyEntityJson(scene, e, je) —— 逐字段覆盖，不增删组件（D2）
// 然后写 globals
```

⚠ **顺序**：先 `_destroyed` 再覆盖。反过来的话，一个"既被销毁又有字段"的条目会先写值再删掉，
白干（且如果那个值指向另一个实体，会短暂出现悬空）。写端要保证一条里不同时出现两者，读端仍按此顺序。

⚠ 销毁一个父实体后，子实体的 `parent` 悬空 → `Scene::setParent(child, {})` 或统一在应用完所有条目后
跑一遍"悬挂父修正"：遍历 `HierarchyComponent`，`parent` 指向不存活实体 → 设为 `kInvalidEntity`
并 `VK_LOG_WARN("SAVE-ORPHAN guid=…")`。**这一步无论如何都要有**（存档被手改也会造出悬挂）。

### 5.4 槽位与路径

```cpp
// 由 runtime 传入，engine 层不拼目录（否则 engine 层测不了存档）
std::string saveDir();   // %LOCALAPPDATA%/Venn/<game>/saves/
                         //   LOCALAPPDATA 取不到 → %TEMP%/Venn/<game>/saves/（照抄 RuntimeLog.h:12 的规则）
```

| 槽 | 文件名 | 由谁写 |
|---|---|---|
| 1 / 2 / 3 | `slot1.sav` … | 主菜单 / 暂停菜单 |
| `quick` | `quick.sav` | F5 / F9 |
| `auto` | `auto.sav` | 关卡切换 / 检查点 |

⚠ **用户目录里已有 `%LOCALAPPDATA%/Venn/<game>/logs/`**（`runtime/RuntimeLog.cpp:25`）——
存档放同级的 `saves/`，别再发明第二个根。**别写进安装目录**（`ship_roadmap` 已明确）。

### 5.5 原子写

```cpp
// 1. 写 <slot>.sav.tmp
FILE* f = _wfopen(tmpPath, L"wb");
fwrite(data, 1, n, f);
fflush(f);
_commit(_fileno(f));        // ⚠ Windows 上 fsync 的等价物；缺这一步"写完但还在页缓存里"
fclose(f);
// 2. 备份旧档（可选但便宜）
if (exists(final)) fs::rename(final, final + ".bak");   // 失败不致命
// 3. 原子替换
fs::rename(tmpPath, finalPath);   // Windows 上走 MoveFileExW(REPLACE_EXISTING)
```

⚠ 三步的顺序不能换：**先 fsync 再 rename**。反过来 rename 之后崩，留下一个改名的空文件。
⚠ 第 2 步失败（`.bak` 被占用）**不能**阻断第 3 步 —— 否则"备份失败"会变成"存不了档"。
⚠ 读档时优先读 `final`，解析失败再退 `.bak`，并打 `SAVE-READ fallback=bak`。

### 5.6 版本迁移 —— **第一版就空跑一次**

H2：`version` 只写不读。这次两件事一起做：

1. 场景文件读端：`if (v > kFormatVersion) { err = "scene is newer"; return; }`（比我们新的档**拒读**，
   降级读会静默丢字段）
2. 存档侧：迁移函数表

```cpp
constexpr int kSaveVersion = 2;
using MigrateFn = void (*)(json& root);           // 把 root 从 v 升到 v+1
struct Migration { int from; MigrateFn fn; };
const Migration kMigrations[] = {
    {1, migrateV1ToV2},     // ← 真的会跑，见下
};
```

> ⚠⚠ **"空跑的迁移表"比"没有迁移表"更糟** —— 它给人虚假安全感，等真需要迁移时才发现机制是坏的。
> 所以 **v1 发出后立刻在同一批提交里加一个 v2 字段**（建议：`globals.playerStamina`），
> 配一个手写的 `tools/fixtures/save_v1.sav` + 用例 `save_migrate_v1_v2`。
> 这一次改动的全部价值就是**让迁移机制被真的走过一遍**。

迁移在 `readSaveGame` 里、**解析后立刻做**：

```cpp
int v = root.value("version", 0);
if (v <= 0 || v > kSaveVersion) → 拒绝（"save is newer than this build"）
while (v < kSaveVersion) { find kMigrations where from==v; if (!found) 拒绝; fn(root); ++v; }
if (v != kSaveVersion) → 拒绝
```

⚠ `v1` 里没有 `version` 字段的情况（第一版忘了写）要在 `migrateV1ToV2` 里当成 1，
并在迁移日志里打 `SAVE-MIGRATE from=0 to=2 assume=1`。

### 5.7 关卡漂移（存档 vs 关卡版本）

`sceneHash` 不一致时**不拒绝**，尽力应用：

```
SAVE-SCENE-DRIFT want=a1b2… got=ffee… missing=3 extra=1 path=scenes/level1.json
```
- `missing` = 存档里有、世界里没有的 guid → 跳过（关卡删了那个箱子）
- `extra` = 世界里有、存档里没有的 guid → 保留关卡默认值（关卡加了新箱子）
- 这是一条 **WARN，不是 ERROR** —— 玩家换了游戏版本还能继续玩，比"存档损坏"体验好得多。

⚠ 但如果 `missing / total > 0.5` → 判定"这不是同一个关卡"，**拒绝**并提示重新开始。
这个阈值要写进代码常量，不要散在 if 里。

### 5.8 验收（`Game.exe --selftest save`，无窗口）

| # | 断言 | 判据 |
|---|---|---|
| 1 | `save_roundtrip_bitwise` | 存 → 改 3 个实体的位置 → 读 → 逐字节等于存档时刻的 dump |
| 2 | `save_incremental_small` | 1000 实体只改 1 个 → 存档 < 整场景 **20%**（实测应 < 2%） |
| 3 | `save_only_one_changed` | 同上 → `entities.size() == 1` |
| 4 | `save_destroyed_recorded` | 删一个实体 → 存档里出现 `_destroyed`，读档后 `entityCount` 减 1 |
| 5 | `save_globals_roundtrip` | `globals` 的 hp / vars / pickedUp 逐字段相等 |
| 6 | `save_migrate_v1_v2` | 读 `fixtures/save_v1.sav` → `migrated=1`，新字段有默认值，老字段不丢 |
| 7 | `save_reject_newer` | 手工造 `version: 99` → 拒读，返回明确错误串 |
| 8 | `save_atomic_keeps_old` | 写到一半 `std::abort()`（子进程）→ 原 `.sav` 仍可解析 |
| 9 | `save_scene_drift_tolerated` | 存档里 1 个 guid 不存在 → `missing=1` 且其余全部应用 |
| 10 | `save_orphan_parent_fixed` | 存档里销毁了一个父亲 → 子实体 `parent == kInvalidEntity` |
| 11 | `save_no_component_add_remove` | 只改值 → 读档后组件**类型集合**不变（D2 的守卫） |

第 8 条的做法：`--selftest save-crash` 子模式，在写完 tmp、**rename 之前** `_exit(3)`。
父进程（`verify_persistence.py`）起它，然后检查原档可读。⚠ 这需要脚本侧配合，
纯 C++ 自检里做不到"自我杀进程后再断言"。

### 5.9 明确不做（写下来是为了防止后面偷偷加）

- ❌ **不做二进制存档**：JSON 够快（1000 实体 ~2 ms），可读可 diff 值千金。
- ❌ **不做增量链（父档 + N 个 delta）**：单机游戏槽位就 3 个，增量链的复杂度（要校验整条链）
  换不到任何东西。**每个槽位永远是完整档**。
- ❌ **不做自动存档的"多个备份轮转"**：一个 `.bak` 够了。
- ❌ **不存档组件增删**（D2）。
- ❌ **不做加密 / 混淆**：单机游戏，防作弊的价值 < 玩家改档的乐趣。

---

## 6 · G18 · Prefab

### 6.1 资产格式（与场景文件**同构**）

`assets/prefabs/crate.vprefab.json`：

```json
{
  "version": 1,
  "kind": "venn.prefab",
  "name": "Crate",
  "prefabGuid": "5e4d3c2b1a0f9e8d",
  "entities": [
    { "guid": "aaaa1111bbbb2222", "name": "Crate",     "parent": -1,  "transform": {…}, "mesh": {…} },
    { "guid": "cccc3333dddd4444", "name": "Lid",       "parent": 0,   "transform": {…}, "mesh": {…} },
    { "guid": "eeee5555ffff6666", "name": "Audio",     "parent": 0,   "audio": {…} }
  ]
}
```

**为什么同构（平铺 + `parent` 下标）而不是嵌套 `children`**：
平铺格式能直接复用 `parseSceneDocument` / `entityToJson`，**不用写第二套字段解析**。
"改一处要同步改 N 处"的风险最小。嵌套的唯一好处是"父子天然表达"，代价是要写一个
平铺↔嵌套的转换器（又是一处会漏字段的地方）。

⚠ 子树的根用 `"parent": -1`；如果日后要"挂到场景里的某个现有节点下"，那是**实例**的事
（`scene.setParent(instance.root, target)`），不改文件格式。

### 6.2 实例化：局部 id 映射

```cpp
// engine/scene/Prefab.h（新建）
struct PrefabAsset {
    std::string    path;                       // "prefabs/crate.vprefab.json"
    ecs::EntityGuid prefabGuid = 0;
    std::string    name;
    nlohmann::json root;                       // 原始 JSON（Apply 时要写回）
    scene::SceneDocument document;             // 解析后的（复用 G16-5）
};

struct PrefabInstanceResult {
    ecs::Entity root;
    std::vector<ecs::Entity> members;
    int overridesCleared = 0;
};
PrefabInstanceResult instantiatePrefab(scene::Scene&, const PrefabAsset&,
                                       const glm::vec3& position);
```

实例化五步（**顺序不能换**）：

```
1. 为每个 EntityDesc 分配**新** guid（不是拷贝 prefab 的），建 localGuid → 新guid 映射表
2. 按新 guid 建实体，写 name / transform / 组件值
3. **改写引用**：parentIndex/parentGuid 一律过映射表 → 指向实例内的新 guid
4. 每个成员挂 PrefabLinkComponent{ prefabGuid, localGuid, instanceRootGuid }
5. 根实体再挂 PrefabInstanceComponent{ prefabGuid, overrides: [] }
```

> ⚠⚠ **第 3 步必须是唯一入口**：`remapEntityRefs(json&, const map&)`，而不是散落在各个
> `xxxFromJson` 里。今天只有 `parent` 一处，但 G17 的 `globals.playerGuid`、将来的
> "门 → 钥匙"引用都会走这里。**散开写就等于以后每加一个引用点都要记得改五个地方。**

> ⚠ **同一 prefab 摆 3 份 → 3 份 guid 互不相同、`localGuid` 相同**。
> 这是 G18 验收 1 的判据，也是"改源 → 3 份同时变"能工作的前提。

### 6.3 覆盖表（D3）

场景里实例成员的写法（**在普通字段之外**多这么一块）：

```json
{ "guid": "9f8e7d6c5b4a3928",
  "name": "Crate",
  "transform": { "position": [5,0,1], … },        // ← 值照常写（含被覆盖的）
  "prefab": { "prefabGuid": "5e4d…", "localGuid": "aaaa…",
              "instanceRoot": "0a1b2c3d4e5f6071",
              "overrides": [ "transform.position", "audio.volume" ] } }
```

**加载时的叠加顺序（严格）**：

```
① 展开 prefab 默认值（该 localGuid 的那一份）
② 用场景文件里的字段值覆盖（场景里总是有完整值）
③ 对 overrides 里**没列到**的字段，把值**改回** prefab 默认值
   → 等价于：未被覆盖的字段永远跟随 prefab 源
```

第 ③ 步是关键：不做它，"改 prefab 源 → 实例跟着变"就不成立。

**覆盖路径语法（刻意只支持两级）**：

| 写法 | 含义 |
|---|---|
| `"transform"` | 整个组件块被覆盖（整块替换） |
| `"transform.position"` | 只覆盖这一个字段 |
| `"material"` | 整块 |
| `"audio.volume"` | 单个字段 |

⚠ **不支持数组下标**（`"material.textures[2]"`）。需要"只覆盖第 3 张贴图"这种粒度时，
改成覆盖整块 —— 简单、够用、不会写出半个 JSON pointer 解析器。

**覆盖是自动记录的，不是用户勾的**：Inspector 里改任何字段时，
如果该实体有 `PrefabLinkComponent`，把对应路径塞进实例根的 `overrides`（去重）。
锚点 `PREFAB-OVERRIDE add=transform.position guid=…`。

### 6.4 Apply / Revert

| 操作 | 语义 | 撤销怎么办 |
|---|---|---|
| **Revert** | 清空该实例的 `overrides` → 重载该实例的字段为 prefab 值 | `structuralEdit` 足够（只改场景内存） |
| **Apply** | 把实例的覆盖值**写回 prefab 文件**，然后**清空所有实例的对应覆盖** | ⚠ 见下 |

⚠⚠ **Apply 的撤销是个真正的坑**：`structuralEdit` 只快照**场景**，而 Apply 同时改了
**磁盘上的 prefab 文件**。撤销场景而文件不回滚 → 下一次重载就"复活"了。

```cpp
// 专用命令，不走 structuralEdit（它管不了文件）
class PrefabApplyCommand : public Command {
    std::string prefabPath;
    std::string prefabBefore, prefabAfter;   // 文件全文
    std::string sceneBefore,  sceneAfter;    // 场景 JSON
    void undo() override { writeFile(prefabPath, prefabBefore); restoreScene(sceneBefore); }
    void redo() override { writeFile(prefabPath, prefabAfter);  restoreScene(sceneAfter);  }
};
```

⚠ **Apply 后必须清所有实例的覆盖**：否则实例仍被旧覆盖值挡住，看起来"Apply 没生效"
（其实文件改了，但实例还显示旧值 —— 一模一样的结果，极难判断）。

⚠ **Apply 前要检查"这份 prefab 还开着没保存"**：编辑器有 Prefab 资产缓存，改文件后要
`assets.reloadPrefab(prefabGuid)`，否则内存里还是旧的。今天没有 prefab 缓存就简单；
一旦加了缓存，这条必须同步。

### 6.5 编辑器接线

| 位置 | 行为 |
|---|---|
| `ContentBrowser` | 拖 `.vprefab.json` 到视口 → `instantiatePrefab`（走 `structuralEdit("Instantiate Crate")`） |
| `SceneHierarchy` 右键 | `Apply to Prefab` / `Revert from Prefab` / `Select Prefab Asset` |
| `Inspector` | 顶部一行 `Prefab: Crate ▸` + 覆盖计数；点覆盖计数弹出列表，每项可单条 Revert |
| 图标 | 实例旁显示 prefab 角标（复用 `EditorIcons.h`） |

⚠ 菜单项在"选中项没有 `PrefabLinkComponent`"时必须 `BeginDisabled` —— 不能点了没反应。
锚点 `HIER-PREFAB apply=1 guid=…` / `HIER-PREFAB disabled=1`。

### 6.6 验收

| # | 断言 |
|---|---|
| 1 | `prefab_three_instances` — 摆 3 份 → 3 份成员 guid 互不相同、`localGuid` 相同、组件值相同 |
| 2 | `prefab_source_edit_propagates` — 改文件里 `Lid` 的 scale → 3 个实例的 `Lid` 全变 |
| 3 | `prefab_override_blocks` — 实例 2 覆盖 `Lid.scale` → 再改源 → 只有实例 1、3 变 |
| 4 | `prefab_apply_writes_file` — Apply → 文件里 `Lid.scale` 变成实例值，且 `overrides` 被清 |
| 5 | `prefab_apply_propagates_others` — Apply 之后，实例 1、3 也变成新值 |
| 6 | `prefab_revert_restores` — Revert → 字段回到 prefab 值，`overrides` 为空 |
| 7 | `prefab_undo_restores_file` — Apply 后 Ctrl+Z → **文件字节复原** + 场景复原（重载后仍对） |
| 8 | `prefab_roundtrip_scene` — 存场景 → 读 → 3 个实例的 `overrides` 逐条相等 |
| 9 | `prefab_no_guid_share` — 同一 prefab 两实例的 guid 集合交集为空 |

### 6.7 坑

- ⚠ `duplicateEntity`（`SceneHierarchy.cpp:59`）**必须**拷 `PrefabLinkComponent` 和
  `PrefabInstanceComponent`，但**不能**拷 guid（`createObject` 会给新的）。
  漏拷 → 复制出来的实例"脱钩"，改源不跟着变。
- ⚠ 实例化时**别把 prefab 文件里的 `guid` 当成真实 guid 用**。它是"局部 id"，
  在场景文件里没有任何实体持有它。混用会让 `find()` 永远落空。
- ⚠ 嵌套 prefab（prefab 里放 prefab）**第一版不做**。要做的化需要递归展开 + 深度上限 + 环检测。
  先写进"不做"，比做到一半崩掉好。
- ⚠ 删掉 prefab 资产 → 场景里的实例变成"孤儿"。加载时 `PREFAB-MISSING prefabGuid=… instances=3`，
  **保留实体**（当普通实体用），不要删 —— 玩家的关卡不能因为一个资源丢了就少一半。

---

## 7 · G19 · 关卡的流送（最小可用）

> 放在最后，且**刻意做小**。这一节的验收标准是"走通了"，不是"做好了"。

1. **`LevelManifest`**：`{"version":1,"chunks":[{"guid":"…","path":"scenes/lvl/part_a.json","bounds":[…],"trigger":"…"}]}`
2. **同步分块先行**：按触发器距离加载/卸载整个 sub-scene。**先把"分块"跑通，再加异步** ——
   两件事同时做，出问题时不知道是哪一件错了。
3. **两段式异步**（用 G16-5 的三段 API）：
   ```
   job 线程: 读文件 → parseSceneDocument          （纯 CPU，无设备，可并发）
   主线程帧首: instantiateDocument + resolveDocumentAssets  （要设备）
   ```
   ⚠⚠ **`vkCreateBuffer/DeviceMemory` 绝不能**在 job 线程调（`ship_roadmap` 的硬纪律）。
   用 pending 队列在帧首 drain。
4. **guid 冲突**：两个 chunk 各自加载，guid 撞了 → `STREAM-GUID-DUP` + 补发。
   ⚠ 这说明关卡制作出错（同一实体被导出了两遍），**必须 WARN 而不是静默修**。
5. **卸载**：延迟 N 帧（默认 30）再真删，避免"刚卸载就被同一触发器拉回来"的抖动。
6. **跨块父子**：`parentGuid`（§3.3）解析时如果父实体还没加载 → 记进 `m_pendingParents`，
   每次 chunk 加载后重试。⚠ **卸载时必须检查"我要删的实体有没有被别的 chunk 的实体当父"**
   —— 有就**拒绝卸载**并 WARN（`STREAM-UNLOAD-BLOCKED refs=2`）。第一版就这一条，不做引用计数。

---

## 8 · "改一处要同步改 N 处"清单

新增/修改一个字段时，**逐条打勾**。这是本路线图最容易被漏的部分。

| # | 位置 | 触发场景 |
|---|---|---|
| 1 | `World::Record` + `create` + `destroy` + `clear` | 加 guid |
| 2 | `entityToJson` / `entityFromJson`（`SceneSerializer.cpp`） | 加 guid / 新组件 |
| 3 | `parent` 读端的 `is_string()` / `is_number_integer()` 双分支 | 改引用 |
| 4 | `kFormatVersion` + 场景读端版本校验 + 迁移表 | 格式变更 |
| 5 | `duplicateEntity`（`SceneHierarchy.cpp:59`） | **加任何新组件**（手写拷贝） |
| 6 | `validateSelection`（`EditorContext.cpp:317`） | 换标识来源 |
| 7 | `SaveGame` 的 `version` + `kMigrations` + `fixtures/save_v1.sav` | 存档字段变更 |
| 8 | `sceneHash` 的算法（只含 guid 集合） | 结构定义变更 |
| 9 | `PrefabLinkComponent` / `PrefabInstanceComponent` 的序列化 + 拷贝 + Inspector | 加 prefab |
| 10 | `remapEntityRefs`（**唯一**的引用重写入口） | **加任何实体引用字段** |
| 11 | `run_regression.py` 的 `DEFAULT` + `TRIGGERS` + `check_regression_map.py` fixture | 加用例 |
| 12 | `engine/i18n/ZhTable.cpp` | 加任何 UI 字面量 |

⚠ 第 5 条是**现网已经存在的隐患**（不是这次引入的）：`duplicateEntity` 逐组件 `w.add`，
新组件忘加就"复制出来少东西"。这次顺手核对一遍现有组件清单，缺的补上。

---

## 9 · 锚点表

| 锚点 | 何时打 | 字段 | 用途 |
|---|---|---|---|
| `SCENE-GUID` | 每次加载（一条） | `assigned= dup= total= path=` | 补发/去重是否按预期 |
| `SCENE-GUID-DUP` | 撞 guid（每条） | `guid= entity= action=reassign` | 手改坏了能定位 |
| `SCENE-GUID-MISSING` | 读端解析失败（每条） | `raw= entity=` | 格式错 |
| `SAVE-WRITE` | 每次存 | `slot= path= bytes= entities= version= ms=` | 增量是否真的小 |
| `SAVE-READ` | 每次读 | `slot= version= migrated= entities= missing= destroyed= ms=` | 迁移走没走 |
| `SAVE-MIGRATE` | 每次迁移 | `from= to= assume=` | 迁移链路 |
| `SAVE-ATOMIC` | 每次存 | `tmp=1 fsync=1 renamed=1 bak=%d` | 原子写三步都做了 |
| `SAVE-SCENE-DRIFT` | hash 不一致 | `want= got= missing= extra=` | 关卡漂移 |
| `SAVE-ORPHAN` | 悬挂父被修 | `guid= parent= action=detach` | 存档被手改 |
| `PREFAB-INSTANTIATE` | 每次实例化 | `prefab= members= root= guid=` | 新 guid 真的分配了 |
| `PREFAB-OVERRIDE` | 记录覆盖 | `add=path guid=` | 覆盖是自动记的 |
| `PREFAB-APPLY` / `-REVERT` | 每次 | `prefab= fields= cleared= inst=` | Apply 清了别人的覆盖 |
| `PREFAB-MISSING` | 资产丢了 | `prefab= instances=` | 孤儿实例 |
| `STREAM-LOAD` / `-UNLOAD` | 每块 | `chunk= parse_ms= upload_ms= ents=` | 两段式耗时分布 |
| `STREAM-UNLOAD-BLOCKED` | 被引用 | `chunk= refs=` | 跨块父子 |

⚠ `detail=` 里**不能有空格**（项目纪律）；路径里的空格换 `_`。

---

## 10 · 用例与回归登记

**新用例 `tools/verify_persistence.py`**（六组）：

| 组 | 内容 | 窗口 | 设备 | 时长 |
|---|---|---|---|---|
| A | `--selftest guid`（13 条） | ❌ | ❌ | ~1 s |
| B | `--selftest save`（10 条） | ❌ | ❌ | ~3 s |
| C | 场景往返：`--dump-scene` 两轮 guid 恒定 | ❌ | ✅ | ~4 s |
| D | 存档：写 → 校验 JSON 结构 → 读 → 再写（字节相等） | ❌ | ✅ | ~6 s |
| E | 原子性：`--selftest save-crash` 子进程被杀 + 原档可读 | ❌ | ❌ | ~2 s |
| F | 编辑器：Prefab 实例化（1 次 GUI 启动，全部断言在这一次里做完） | ✅ | ✅ | ~15 s |

⚠⚠ **写 GUI 用例必记：`Editor.exe` 只认 `MYVK_FRAMES`，传 `--frames` 会静默挂死到超时**
（`editor/main.cpp` 里 `(void)argc; (void)argv;`）。这是 G14 花 8 分钟买来的教训。
⚠ **GUI 用例同时只能有一个**（`kill_stale_editors` 无差别杀进程）。
⚠ 结尾必须是 `结果: PASS` / `结果: FAIL: ...`（ASCII 冒号，取最后一条）。

**`TRIGGERS` 登记**：

```python
"verify_persistence": ["engine/scene/SceneSerializer.*",
                       "engine/scene/SaveGame.*",
                       "engine/scene/Prefab.*",
                       "engine/scene/SceneDocument.*",
                       "runtime/SaveGameState.*",
                       "editor/EditorContext.*",
                       "editor/SceneHierarchy.*",     # duplicateEntity / prefab 右键
                       "editor/ContentBrowser.*",
                       "tools/gen_persistence_fixtures.py"],
```

⚠ **`engine/ecs/**` 与 `engine/scene/Scene.{h,cpp}` 已经在 `GLOBAL_GLOBS` 里**
（`run_regression.py:330-352`）→ **不要**写进 `TRIGGERS`（写了在 `--narrow` 下会假跳过）。
本轮会改 `World.h`（加 guid）和 `Components.h`（加 `PrefabLinkComponent`）→ **都是横切**，
按纪律**只有明确下令才跑全量**。`engine/scene/SceneSerializer.*` **不在**横切名单里，可以正常登记。
⚠ 改完 `TRIGGERS` **必须**跑 `check_regression_map.py`（改表 + 同步 fixture 是原子操作）。

---

## 11 · 否定测试清单（每条都必须真的跑一遍看到红）

| 故意改坏 | 应该变红 |
|---|---|
| `setGuid` 里删掉 `m_byGuid.erase(r.guid)` | `guid_no_ghost_after_set`（幽灵实体） |
| `allocateGuid` 直接 `return d draws 0` | `guid_never_zero` |
| `World::destroy` 里不 erase guid | `guid_destroy_unmaps` |
| `guid()` 改成读到临时 guid（不 setGuid） | `scene_guid_roundtrip` |
| 读端接受 guid 但不 `setGuid` | `scene_guid_stable_twice`（每次 dump 都换 guid） |
| 存档改成"全量写" | `save_incremental_small`、`save_only_one_changed` |
| 基线哈希里含 `name` | 改个名字就整档重写（`save_only_one_changed` 仍绿，但 `save_incremental_small` 会红） |
| 读档先覆盖再销毁 | `save_destroyed_recorded`（顺序错） |
| 原子写去掉 `_commit` | **单跑仍然绿**（这条抓不住）→ 必须靠 E 组真杀进程 |
| Rename 之前不写 tmp（直接写 final） | `save_atomic_keeps_old` 红（原档被截断） |
| Prefab 实例化**复用** prefab 的 guid | `prefab_no_guid_share` 红 |
| 加载时去掉"③ 未覆盖字段跟随源" | `prefab_source_edit_propagates` 红 |
| Apply 后不清 `overrides` | `prefab_apply_propagates_others` 红 |
| `PrefabApplyCommand::undo` 只还原场景 | `prefab_undo_restores_file` 红 |
| `remapEntityRefs` 只改 `parent` 不改 `parentGuid` | `scene_parent_by_guid` 红 |

⚠ 做完**必须还原并重跑一次看到绿**。⚠ 有一条要特别注意：**"去掉 `_commit`" 在正常路径上测不出来**
（进程正常退出时页缓存会刷盘），只能靠 E 组的真杀进程。别以为没红就是覆盖到了。

---

## 12 · 执行顺序（可直接照做）

| # | 任务 | 规模 | 验收 |
|---|---|---|---|
| **G16-1** ✅ | `World::Record.guid` + `m_byGuid` + `guid/find/setGuid/allocateGuid` + 三处生命周期同步 | S | `--selftest guid` 第 1~7 条绿 |
| **G16-2** ✅ | 场景 JSON：写 `guid` / 读 `guid`(setGuid+dup+assigned) / `parentGuid` 双分支 / `kFormatVersion=2` + 读端版本校验 | M | 第 8~13 条绿；`verify_runtime_m2` C 组仍绿 |
| **G16-3** ✅ | `--selftest guid` **26 条**（原计划 13 条，见 §12.1） | M | `GUID-TOTAL pass=26 fail=0` |
| **G16-4** ◐ | `validateSelection` 改 **guid 优先**（逻辑抽成 `Scene::reconnectByGuidOrName`，可无窗口自检）· Hierarchy `Copy GUID` / `Select by GUID` **未做** | S | 同名实体重建后选中不错位（`selection_reconnect_by_guid` + 对照区 `selection_fixture_has_ambiguity`） |
| **G16-5** ✅ | 序列化器三段拆分（`SceneDocument` / `parseSceneDocument` / `instantiateDocument` / `resolveDocumentAssets`） | M | `verify_runtime_m2` C 组绿 + `--selftest save` 能无设备跑（**39 条**，见 §12.2） |
| **G17-1** ✅ | `SaveGame.{h,cpp}`：格式 + 原子写 + 迁移表 + `readSaveGame/writeSaveGame` | L | 第 1、4、5、7 条绿（**33 条**，见 §12.3） |
| **G17-2** ✅ | `SaveGameState.{h,cpp}`：基线哈希 + diff 生成 + `applySaveGame` + 悬挂父修正 | L | 第 1、2、3、9、10、11 条绿（**77 条**，见 §12.4） |
| **G17-3** | 槽位（`slot1..3` / `quick` / `auto`）+ 目录 + CLI（`--save/--load/--slot`）+ 控制台 `save.*` | M | `SAVE-WRITE` / `SAVE-READ` 锚点出现 |
| **G17-4** | `verify_persistence.py` A~E 组 + 回归登记 | M | `结果: PASS` |
| **G18-1** | `Prefab.{h,cpp}`：资产解析（复用 `parseSceneDocument`）+ `instantiatePrefab` + `remapEntityRefs` + 两个组件 | L | 验收 1、9 条绿 |
| **G18-2** | 覆盖表（自动记录 / 加载叠加 / `PrefabApplyCommand` 含文件级撤销） | L | 验收 2~7 条绿 |
| **G18-3** | 编辑器：Content 拖拽 + Hierarchy 右键 + Inspector prefab 行 + F 组用例 | M | F 组绿 |
| **G19-1** | `LevelManifest` + 同步分块加载/卸载 + 延迟卸载 + 悬挂父重试 | M | 两块无缝进出，`STREAM-LOAD` 成对出现 |
| **G19-2** | 异步两段式（job 线程 parse + 帧首 drain）+ 冲突/阻塞告警 | L | `parse_ms` 与 `upload_ms` 分开可见 |

**每完成一行**：跑该行的自检 → 补 `TRIGGERS` → `check_regression_map.py` → 在本文档 §13 追加
"实测数字 + 踩到的坑"。

### 12.1 G16-1~4 落地记录（2026-10-27）

**实测数字**

```
Game.exe --selftest guid   → GUID-TOTAL pass=26 fail=0
tools/verify_guid.py       → 8/8 PASS（结果: PASS）
真实场景（v1，11 实体）     → SCENE-GUID assigned=11 dup=0 total=11
v2 夹具（11 个 guid）       → SCENE-GUID assigned=0  dup=0 total=11
v2 夹具（第 2 个抄第 1 个） → SCENE-GUID assigned=0  dup=1 total=11
```

**为什么是 26 条而不是计划的 13 条**：原计划只覆盖 World 层（13 条）。但
"读端真的读了文件里的 guid"这件事，World 层测不出来 —— 读端若完全忽略该字段、
每次随机补发，World 层的断言**全绿**。所以补了 7 条场景往返（`scene_*`）
与 3 条选择接回（`selection_*`），加上 3 条 hex 表示，共 26 条。

**踩到的坑（都对应用例）**

| 坑 | 症状 | 钉住它的断言 |
|---|---|---|
| `setGuid` 忘了先 `m_byGuid.erase(旧guid)` | 一个"幽灵 guid"能被 `find()` 命中直到 `clear()`；其它断言全绿 | `guid_no_ghost_after_set` / `guid_set_chain_no_ghost`（期望 1，漏写是 100） |
| `destroy` 里"先清零再 erase"（顺序写反） | 旧键留在表里指向被复用的槽位 → 新实体起来后**旧 guid 复活**，旧存档的引用静默换了个物体 | `guid_destroy_unmaps` 的 `old=REVIVED`（否定测试实测到） |
| `guidToHex` 不补前导零 | 低位 guid 的实体"存一次再读"就变成另一个物体（`parseGuidHex` 长度检查判非法 → 补发新 guid） | `guid_hex_leading_zeros` |
| `parseGuidHex` 用 `std::stoull` | 接受 `"0x…"` 与尾随垃圾、非法输入抛异常 | `guid_hex_rejects_garbage` |
| **空实体不进场景文件** | 只挂 Transform/Name 的夹具写端算 `skipped` → json 只有 500 B、一个实体都没写，"往返"比的是**两份空文件**，而且 PASS | `scene_guid_roundtrip` 的 `n=<json 字节数>`（修好后 5082） |
| 版本校验放在 `scene.clear()` **之后** | 打开新版文件 → `ok=false` 但当前场景已被清空 | `scene_version_too_new` 的 `kept=1` |
| `je.value("parent", -1)` 遇字符串 `type_error` | 手写 `"parent": "-1"` 抛异常并打断整份加载 | 改成 `is_number_integer()` 先判（本文件 §3.3 早就写了，这次落实） |

**做过的否定测试**（改坏 → 看到红 → 还原 → 看到绿）

1. 去掉 `setGuid` 的 `erase` → `guid_no_ghost_after_set`（`old=ghost`）+ `guid_set_chain_no_ghost`（100 vs 1）红。
2. `destroy` 顺序写反 → `guid_destroy_unmaps`（`old=REVIVED`）红。
3. 写端不写 `guid` → `scene_guid_roundtrip`（`same=0`）红；`scene_guid_stable_twice` **仍绿**
   （两边都不写，长度还一样）—— 说明它抓的是**读端重算 guid**。
4. 读端不 `setGuid` 覆盖 → 两条都红，且 `stable_twice` 的 `len=5082/5082` **相同**、
   内容不同 —— 只判长度会漏，逐字节判据是必要的。

**未做（留给下一轮）**：G16-4 的 Hierarchy 右键 `Copy GUID` / `Select by GUID`（纯调试便利，
不影响任何验收）。

### 12.2 G16-5 落地记录（2026-10-27）

**实测数字**

```
Game.exe --selftest save        → SAVE-TOTAL pass=39 fail=0
tools/verify_save.py            → 14/14 PASS（结果: PASS，7.4 s）
tools/verify_runtime_m2.py      → RESULT: PASS（C 组 12 项逐字段全绿）
Game.exe --selftest guid        → GUID-TOTAL pass=26 fail=0（拆分无回归）
tools/verify_guid.py            → 8/8 PASS
真实场景（cutscene_demo）        → SCENE-GUID assigned=11 dup=0 total=11
dump 出来的文件再读            → SCENE-GUID assigned=0  dup=0 total=11
```

**做了什么**

* 新建 `engine/scene/SceneDocument.h`：`DocumentEntity` / `SceneDocument` /
  `DocumentStats` + 三个函数声明。**实现留在 `SceneSerializer.cpp`**（不是偷懒 ——
  `applyCamera` / `physicsFromJson` / `entityApplyComponentsImpl` 全在匿名 namespace 里，
  另起 TU 就只能复制一份，而复制的那份一定会漏改）。
* `entityFromJsonImpl` 原地劈成 `entityApplyHierarchyImpl`（纯 CPU）+ `entityApplyComponentsImpl`
  （要资产），公开壳 `entityApplyHierarchy` / `entityApplyComponents` 进 `SceneEntityIO.h`。
  ⚠ 保留一个**只有两行**的 `entityFromJsonImpl` 当"唯一串联点"：这样"拆完之后 Prefab
  与场景加载仍然走同一条路"在代码上无法被悄悄改坏。
* `loadSceneFromJson` 变成三段的**组合**，一行注释写明"想后台解析就把 ①② 搬走、只留 ③"。
* `kFormatVersion` 从匿名 namespace 提到头文件（`kSceneFormatVersion`），源文件里留别名。

**踩到的坑（都对应用例）**

| 坑 | 症状 | 钉住它的断言 |
|---|---|---|
| `Scene::light()` 找不到方向光组件时**自己造一个实体** | 断言写 `entityCount()==6` 永远红在 7，而**代码是对的** | `instantiate_clears_old` 改按**名字**判旧实体是否消失 |
| `--max-frames` **不是**有效参数（正解 `--frames`） | 进程不退出 → 脚本靠 45 s 超时 kill 收场，"不靠 kill"那句注释是假的。`verify_guid.py` 也中了这一枪（每跑一次白等 4 分钟），这轮一并修掉 | `verify_save.py` 三处全改 `--frames`，脚本从"卡到超时"变成 7.4 s |
| `--scene <绝对路径>` 打不开却不报错 | 运行时把场景名当**资产根下的相对路径**，绝对路径失败就退回默认流程**继续跑**，症状是"脚本 90 秒无输出" | `verify_save.py` C 组把 dump 产物落在 `build/bin` 下、用**相对文件名**再读 |
| `parse` 阶段顺手保留组件 optional | 会诱导 ③ 段变成第二份组件实现 | 用 `raw` + 一条 `parse_keeps_raw`（否定测试：`raw=object()` → 6 条红） |

**做过的否定测试**（改坏 → 看到红 → 还原 → 看到绿）

1. `instantiateDocument` 里的父子第二遍注释掉 → `instantiate_parent_by_index` /
   `instantiate_parent_by_guid` / `instantiate_folder` / `equiv_parent_shape` **4 条红**。
   （最后一条说明"等价性断言"确实能抓到"父子只在一段里做了"。）
2. `parseSceneDocument` 不保留 `raw`（改 `json::object()`）→ `parse_keeps_raw` +
   `instantiate_transform` + 两条父引用 + `equiv_parent_shape` **6 条红**。
3. `loadSceneFromJson` 里把 `scene.clear()` 挪到 parse **之前** → `parse_fail_keeps_scene`
   红且 `n=0`（当前场景被清空）—— 这条正是 G16-2 留下的债，拆分段最容易搬错位置。
4. `SceneDocument.h` 给 `instantiateDocument` 加一个 `assets::AssetManager&` 参数 →
   `verify_save.py` D3 红（`sig=dirty`）。**这条不用编译**，是纯静态护栏。
5. 写端不写 `guid` → `verify_save.py` C3 红（`got=(11,0,11)`，全部补发）。

**给下一轮的判据**

* 判断"这次改动有没有破坏自由 CPU 性"：看 `parseSceneDocument` / `instantiateDocument`
  的**参数表**。加了 `AssetManager*` 就是破了，`verify_save.py` D 组会红。
* `SceneDocument.globals` 用的是**白名单**（列出认识的 key），不是"排除 entities"。
  新增一个全局块（比如 G17 的 `globals.playerGuid`）要同时改两处：白名单 + ② 段的 `has()` 分支。
* `created` 与 `doc.entities` **一一对应**，无效槽位保留占位。任何"顺手压缩一下"都会让
  `parent` 下标全体错位 —— 而且不报错。

---

## 13 · 风险与明确不做

| 风险 | 缓解 |
|---|---|
| 改 `engine/ecs/**` 是**横切** → 全量回归被点名 | 集中一次改完（G16-1 一轮），别分五次 |
| 加 `guid` 打破某个用例的 dump 断言 | 实测 H8：只查"源 key ⊆ dump key"，安全；仍要跑一遍两个读 dump 的用例（`verify_audio` / `verify_runtime_m2`） |
| 老场景补发 guid → 每次加载都不同 | 首次保存即固化；`SCENE-GUID assigned=N` 让这件事**可见** |
| Prefab Apply 的文件级撤销 | 专用 Command（§6.4），别硬塞 `structuralEdit` |
| 存档 vs 关卡版本漂移 | `sceneHash` + 容忍策略 + 50% 阈值 |
| 三段拆分把序列化器改坏 | 每步都跑 `verify_runtime_m2` C 组；它比较**逐字段**，是唯一护栏 |

**明确不做**（写下来防止后面偷偷加）：

- ❌ 场景文件 `parent` 改字符串（H7：20 个用例）
- ❌ 二进制存档 / 加密
- ❌ 增量链（父档 + delta 链）
- ❌ 嵌套 Prefab（第一版）
- ❌ 存档里增删组件（D2）
- ❌ 跨 chunk 引用计数（G19 第一版用"拒绝卸载"）
- ❌ `MYVK_GUID_SEED`（没有用例需要，别加开关）

### G18 实测（2026-10-26，任务 #315）

**数字**：`--selftest prefab` **36 条**全过；`tools/verify_prefab.py` **45 条**全过
（A 4 + B 15 + C 12 + D 8 + E 6），4 次编辑器启动约 62 s。

**与上面计划的三处偏差**（都是"依赖还没做"导致的，不是偷工）：

| 计划 | 实际 | 为什么 |
|---|---|---|
| 实例成员带 `(prefabGuid, localGuid)` | 只有 `localId`（文件内稳定） | **G16 没做** —— 全仓没有 `EntityGuid`，而 Prefab 只需要"节点在文件内稳定"，`localId` 就够 |
| 复用 `parseSceneDocument` / `remapEntityRefs` | 把 `SceneSerializer.cpp` 里 600 行**单实体**字段处理机械抽成 `entityToJson` / `entityFromJson`（`scene/SceneEntityIO.h`） | **G16-5 没做** —— 没有三段 API。抽出来的好处一样：场景与 prefab 共用**唯一一份**实现，以后加组件不会漏改第二处 |
| 编辑器：Content 拖拽 + Hierarchy 右键 | Hierarchy 右键三项 + Inspector prefab 条 + **控制台六条 `prefab.*`** | 拖拽要动 Content 的拖放协议；命令那六条是同一份 `EditorContext` 方法，且顺带进了命令面板（E18 从控制台表派生） |

**新增了一条计划里没有的语义：落点记账。**
`instantiatePrefab(at)` 若 `at` 与母版里作者写的根位置不同，**当场记一条
`<localId>/transform.position` 覆盖**。不记的后果不是"少一条账"：用户把实例摆到
(3,0,0)，之后母版改了个缩放，再同步 —— 实例**自己跳回**母版坐标。而且现象像
"缩放把位置也带歪了"，排查方向完全是错的。

**踩到的坑**（写下来是因为每一个都花了时间）：

1. **改了 `engine/**` 必须把两个 exe 都重编**。`VennEditor` 用 `libVennEngine.a`
   静态链，只 `build VennRuntime` 的话 Editor.exe 还是旧引擎 —— 症状是
   "`--selftest` 里落点记账绿、编辑器里 `overrides=0`"，看起来像跨层 bug，
   实际是链接产物过期。
2. **覆盖路径的前缀必须是 `localId`**，不是组件名。一份 prefab 有多个节点，
   写 `transform.position` 根本不知道是谁的。
3. **`structuralEdit` 返回 `void`**，不能写成 `const bool ok = structuralEdit(...)`。
4. **E 组（真鼠标拖 gizmo）依赖 `MYVK_LOG_RECTS=1`** 才会打 `GZ-ORIGIN` /
   `GZ-AXIS-X`；同时**不能**设 `MYVK_EDITOR_NO_HUD=1` —— 整块视口叠加层不画，
   手柄也就没有矩形，症状是"GZ-ORIGIN 永远等不到"，很像"选中失败"。
5. **名字去重不是可选项**：3 份长得一样的实例是最典型的同名来源，而
   `Scene::find(name)`（撤销后接回选中、脚本按名字找）只命中第一个。
   `instantiatePrefab` 里用 `nameUse` 计数表生成 `Crate / Crate 2 / Crate 3`。
6. **可见性 / 锁定也是"字段"**。母版同步会重写外壳（`entityApplyShell`），
   实例上手改的 👁 / 🔒 会被悄悄改回去 → 这两处也要 `recordOverride`
   （`SceneHierarchy` 的 pending 切换里）。`visible` 恒写、`locked` 只在 true
   时写，所以"解锁"这条覆盖的 `getPath` 拿到 null → 不叠加 → 语义自动正确。

**明确不做（v1）**：嵌套 prefab（文件里出现 `prefab` 块只警告并忽略）、
删组件传播（`entityFromJson` 只加/改不删）、数组下标覆盖
（`material.textures[2]` —— 需要半个 JSON pointer 解析器，要那个粒度就覆盖整块）。

---

## 14 · 速查卡

```
◆ 身份
guid: uint64, 0=无; 存 World::Record; JSON 里是 16 位 hex 字符串（不是数字！）
create() 自动分配; setGuid() 用来覆盖（加载用）; 先 erase 旧键再插新键
find(guid) → Entity; guid(e) → uint64
进程级 mt19937_64，不在 World 里（否则清空重载会重放同一串）

◆ 场景 JSON（version 2）
guid: 每实体必写; parent: 下标（本文件内，不变）; parentGuid: 仅跨文件时写
读端: is_string() → guid; is_number_integer() → 下标（value<int> 遇字符串会抛）
没 guid → 补发 + assigned++; 撞了 → 保留 + dup++ + WARN

◆ 存档
增量 = 基线 FNV-1a(json.dump()) 比对，不做脏标记
每条 = 完整实体 JSON（复用 applyEntityJson）；按 guid 数组不用对象
原子写: tmp → fflush → _commit → .bak → rename（顺序不能换）
version + kMigrations；v1 发完立刻加 v2 空跑一次，别留空表
sceneHash = 排序后的 guid 集合（只含身份，不含值）

◆ Prefab（G18 已落地，2026-10-26）
文件与场景同构（平铺 entities[] + parent 下标），只多 localId（没有 guid，G16 未做）
单实体读写 = scene/SceneEntityIO.h 的 entityToJson / entityFromJson（与存场景同一份）
实例化 = 每个节点建实体 + PrefabLinkComponent{path, localId, instance, root}
         名字按 nameUse 计数表去重（Crate / Crate 2 / Crate 3）
覆盖 = 显式路径列表 `<localId>/<component>[.<field>]`，只在实例**根**上
       同步 = prefab 值 + 实例当前值盖回被覆盖的字段；name / parent 从 base 剔除
落点 ≠ 母版位置 → 当场记一条 `<localId>/transform.position`（否则改母版时实例跳回去）
Apply 写回资产后**必须**重新 loadPrefab + syncInstances（否则别的实例还是旧值）
撤销用 PrefabApplyCommand（文件 + 场景一起回滚；structuralEdit 管不了文件）
编辑器：层级右键三项 / Inspector 头部条 / 控制台六条 prefab.* / 命令面板自动有

◆ 坑
Editor.exe 只认 MYVK_FRAMES（--frames 会被丢弃 → 挂死到超时）
duplicateEntity 是手写字段拷贝 → 新组件必须同步；guid 不能拷
engine/ecs/** 是横切 → 全量回归需明确下令
存档里的字段只覆盖值，不增删组件
```

### 12.3 G17-1 落地记录（2026-10-27）

**实测数字**

```
Game.exe --selftest save     → SAVE-TOTAL pass=33 fail=0
tools/verify_persistence.py  → 8/8 PASS
--selftest sceneio / guid    → 39/39 · 26/26（`includeEmpty` 新参数无回归）
verify_runtime_m2.py         → RESULT: PASS（C 组 12 项逐字段）
check_regression_map.py      → PASS
```

**落地内容**

* `engine/scene/SaveGame.{h,cpp}`：`SaveGame` / `SaveEntity` / `SaveGlobals` /
  `SaveIoResult` + `fnv1a64` / `sceneStructureHash` / `captureSaveGame` /
  `saveGameToJson` / `saveGameFromJson` / `writeSaveGame` / `readSaveGame`。
* `engine/scene/PersistenceSelfTest.{h,cpp}`（33 条，`--selftest save`）四组：
  **A 结构指纹** · **B 格式逐字节往返** · **C 迁移** · **D 原子写/读**。
* `tools/verify_persistence.py`：A 自检 + **分组存在性** + `SAVE-MIGRATE` 锚点；
  B **磁盘侧**（文件真在、合法 JSON、version 对、guid 16 位、`.bak` 已生成）。
* `kSaveVersion = 2`。**v2 的唯一理由**是让迁移机制被真的走过一遍（§5.6 的原则：
  空跑的迁移表比没有更糟）。迁移函数 `migrateV1ToV2` 只补 `globals.playerStamina`。
* `entityToJson` 多一个 `bool includeEmpty = false` 参数。

**踩到的坑（都对应用例）**

| 坑 | 症状 | 钉住它的断言 |
|---|---|---|
| `--selftest save` 这个名字被 G16-5 先占了 | 两个功能抢一个子命令 → 后人会以为"存档自检只要跑这个就行" | G16-5 改名 `--selftest sceneio`（锚点 `SCENEIO-*`、脚本 `verify_sceneio.py`） |
| `captureSaveGame` 直接复用 `entityToJson`（**默认**过滤空壳） | 玩家 / 出生点 / 纯分组节点**一条都进不了档**（只挂 Transform 的实体返回 false）→ 读数 0 条 | `capture_entity_count`：修法是加 `includeEmpty=true` 让存档走**全量**语义 |
| `sceneStructureHash` 对每个 guid 单独算一次 FNV 再"混合" | 等于只在算最后一个 guid —— 加/删实体哈希都不变，语义完全失效 | `hash_detects_add` / `hash_detects_remove` / `hash_order_independent` |
| 自检为了测 `.bak` 兜底把主档**写坏后不收尾** | 磁盘长期留一份坏档 → `verify_persistence.py` B 组红，而人会去查"写端是不是坏了" | `file_restore_valid_after_negative`（凡是"故意造坏数据"的用例都要有收尾） |
| `Scene` 不可拷贝/不可移动 | "返回 Scene 的 lambda"编不过（`C2280`） | `hash_order_independent` 改成直接建两个 Scene |

**做过的否定测试**（改坏 → 看到红 → 还原 → 看到绿）

1. `sceneStructureHash` 不排序 → `hash_order_independent` 红（`fwd=847875,rev=96d7a8`）。
2. `migrateV1ToV2` 重建整份 `globals`（只留新字段）→ `migrate_v1_keeps_old` 红
   （`actual=4`，4 个老字段全丢）。
3. `captureSaveGame` 用默认 `includeEmpty=false` → `capture_entity_count` /
   `capture_entities_have_guid` / `capture_entities_have_name` **3 条红**（`actual=0`）。
4. `replaceFile` 换成 `std::rename` → **没红**。原因：MSVC 的 `rename` 本身就能覆盖，
   所以 `file_write_twice_replaces` 抓的是"**写第二遍**这个场景"，不是 rename 语义。
   `MoveFileExW` 保留的理由改成了 `MOVEFILE_WRITE_THROUGH`（真正落盘），并在代码里写清楚。

**未做（G17-1 的尾巴，不阻塞 G17-2）**

* `--selftest save-crash`：写到 tmp 之后、rename 之前 `_exit(3)`，父进程（脚本）断言
  旧档仍可读。需要 C++ 侧一个专门的 CLI 子模式 + 脚本起子进程，纯 C++ 自检做不到
  "自我杀进程后再断言"。
* 槽位目录（`%LOCALAPPDATA%/Venn/<game>/saves/`）、CLI（`--save/--load/--slot`）、
  控制台 `save.*` —— 都是 **G17-3**。本层的路径由调用方给（engine 层不拼目录）。

### 12.4 G17-2 落地记录（2026-10-27）

**实测数字**

```
Game.exe --selftest save     → SAVE-TOTAL pass=77 fail=0（G17-1 的 33 + 本层 44）
tools/verify_persistence.py  → 20/20 PASS（A1~A7 + B1~B7）
--selftest sceneio / guid    → 39/39 · 26/26（无回归）
verify_sceneio / verify_guid → PASS
verify_runtime_m2.py         → RESULT: PASS（C 组 12 项逐字段）
check_regression_map.py      → PASS
```

**新增 / 改动**

| 文件 | 内容 |
|---|---|
| `engine/scene/SaveGameState.{h,cpp}`（新） | `SaveGameState`（基线 / diff / `noteApplied`）· `applySaveGame` · `fixOrphanParents` · `kMaxMissingRatio` |
| `engine/scene/SaveGame.{h,cpp}` | 新增 `saveEntityJson` / `saveEntityHash`；`captureSaveGame` 多一个 `baseline` 参数（**全量与增量只剩一个实现**） |
| `engine/scene/PersistenceSelfTest.cpp` | 新增 E（增量 12 条）/ F（应用 24 条）/ G（形状 5 条）三组 |
| `tools/verify_persistence.py` | 断言下限 32 → **77**；A2 分段加 `incremental` / `apply` / `shape`；新增 A4~A7 四条**锚点**断言 |
| `tools/run_regression.py` | `TRIGGERS["verify_persistence"]` 加 `engine/scene/SaveGameState.*` |

**三个"看着能用其实会出事"的决定**

| 放弃了 | 改成 | 理由 |
|---|---|---|
| 存档的父子写 `parent` **下标**（与场景文件完全同构） | 写 `parentGuid`，`parent` 直接删 | ① 应用时根本不读层级（层级归关卡）；② 增量档只写一部分实体，下标会指错人；③⚠⚠ 下标让**销毁一个实体把后面实体的哈希全带翻** —— 而 `World` 的池子是 swap-and-pop，**只有最后一个元素会跳位**，所以这个 bug 在某些夹具下完全看不出来 |
| 应用时只覆盖"值组件"（transform / 音频 / 灯…）的白名单 | 复用 `entityApplyShell` + `entityApplyComponents` | 白名单 = 第二套字段解析，以后加组件必漏改，而**漏改不报错**（读档后那个字段静默不还原）。复用同一条路 = 存档与场景文件永远同源 |
| 销毁后把 guid 从基线里**摘掉** | 保留 guid、值记 `kDeadHash` | 摘掉之后下一次存档不再写 `_destroyed`，而它会被读进**还带着那个实体**的关卡里 → "打死的怪读档后复活" |

**踩到的坑（都对应用例）**

| 坑 | 症状 | 钉住它的断言 |
|---|---|---|
| `World` 池子是 swap-and-pop，销毁下标 i 时**只有最后一个元素**跳到 i | 用"销毁第一个"的夹具验"下标耦合"，父的下标纹丝不动 → 断言**假绿** | `diff_no_index_coupling` 的夹具改成"父**排在最后**"（创建顺序被算过，注释写死在代码里） |
| `captureSaveGame` 造 `_destroyed` 条目时 `raw` 留默认 `{}` | 与 `saveGameFromJson` 读出来的 `null` 不一致，"存→读→存"在两种形态间跳 | `diff_destroyed_flag` 断言 `raw.is_null()` |
| 父被销毁后，子实体的 `parentGuid` 写不出来（`w.guid(handle(dead)).`→0） | 销毁一个父会让**所有子实体**都被判成"改过"（不是 bug，是持久化的父引用真的变了） | `apply_orphan_fixture_total == 2` —— 写死 1 的期望会把这个行为当 bug 抹掉 |
| `--selftest save` 无设备跑应用层要 `AssetManager` | 直接 new 会崩 | 自检用 `AssetManager{TextureContext{}}`（device 为 null）；夹具里**不许有** `mesh`/`material`/`skinned` 块，`resolveMesh` 拿到 `null` 直接返回 nullptr |

**做过的否定测试**（6 次，全部实测到红 → 还原 → 绿）

1. `captureDiff` 不传基线 → **18 条红**（diff 全组 + apply 的销毁/悬挂父全组）。这条证明
   "增量"不是一个可选优化，而是**整组断言的前提**。
2. `saveEntityJson` 改回"传整世界下标表 + 不删 `parent`" → 第一次只有 2 条红
   （`json_has_parent_guid` / `json_parent_guid_value`），`diff_no_index_coupling` **假绿**
   → 改夹具（父排最后）后重做 → **4 条红**。
3. `noteApplied` 用 `erase` 代替 `kDeadHash` → `diff_dead_sticky` 红（`d1=0,d2=0`）。
4. `kMaxMissingRatio` 改 1.0 → `apply_drift_reject` + `apply_drift_reject_untouched` 红。
5. `applySaveGame` 里不调 `fixOrphanParents` → `apply_orphan_parent_fixed` /
   `apply_orphan_count` 红（`orphans=0`）。
6. `hashEntityJson` 只哈希 `transform` 子对象 → `diff_detects_visible` 红 +
   `hash_is_dump_fnv` 红 + `apply_orphan_fixture_total` 红（子的父引用变化看不见）。

**未做（G17-2 的边界，留给后面）**

* 槽位 / 目录 / CLI / 控制台命令 = **G17-3**；`SaveGameState` 的生命周期（关卡加载完成时
  `captureBaseline`）也由 runtime 在那一步接上。
* `--selftest save-crash`（G17-1 的尾巴）仍未做。
* ⚠ `HierarchyComponent::children` 存的是 `EntityId`（不带 generation）→ 槽位复用后
  "看起来还活着"。`fixOrphanParents` 里的 `alive()` 判据因此**可能漏判**；
  要彻底修得让 children 存 `{id, generation}`，那是 G16 的延伸，不在本轮。

### 12.9 判据速查（2026-10-29 从项目笔记挪来，原文照抄）

> 项目笔记（`.workbuddy/memory/MEMORY.md`）有体积上限，这一节的细则搬到这里，
> 笔记里只留两条最高频的。**口径不变**，只是换了住处。

* ⚠⚠ 三段 ①② **签名不许有 `AssetManager`**（`verify_sceneio.py` D 组静态盯着）。
* 存档原子写 `tmp → fsync → bak → MoveFileExW`，**顺序不能换**（换一处就不是原子的了）。
* `sceneHash` = guid **排序后**的顺序喂 FNV（不排序 = 加一个实体就全变）。
* ⚠⚠ `World` 池子是 **swap-and-pop** → 夹具不"把父排最后"时，`parentGuid` 那个 bug
  **测不出来**（这条本身就是"夹具设计决定能不能测出 bug"的样例）。
* 应用存档**复用** `entityApply*`；另写一套白名单 = 第二套解析，**漏改不报错**。
