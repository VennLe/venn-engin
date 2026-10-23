# 编辑器进 Play 就崩：地形高度场的所有权（P7）

> 2026-10-23 · 用户报障一条，一次根因
> 验收：`tools/verify_editor_play_terrain.py`（T0~T5，7 条，约 45 秒）
> 相关：`engine/physics/PhysicsWorld.*`、`engine/physics/ScenePhysics.*`、
> `engine/physics/Heightfield.*`、`editor/EditorApp.cpp`（`buildPhysicsWorld` /
> `editorGroundHeight`）

## 0 · 一句话

**`PhysicsWorld` 借了别人的高度场指针，而那个别人是局部变量。**
编辑器每次进 Play 读地形都在读已经还回去的内存 —— P6 之前没人读，所以一直
没爆；P6 给编辑器 Play 加了贴地（要读地形高度），一按 Run 就 ACCESS
VIOLATION。

## 1 · 症状

用户原话：

> "引擎编辑器3D视口中，我添加了一个第三人称角色，然后直接点击运行游戏，
> 然后引擎编辑器直接崩溃退出了。"

三个关键限定，后来每一条都成了判据：

1. **有角色** —— 空场景进 Play 是好的；
2. **点的是 Run** —— 不是编辑态；
3. **崩** —— 不是黑屏、不是报错框，是进程直接没了。

## 2 · 定位

### 2.1 WER 给出了偏移

Windows 应用日志里三条记录（17:55:38）：

```
出错应用程序名称： Editor.exe，版本： 0.3.0.0
异常代码： 0xc0000005            ← ACCESS_VIOLATION
错误偏移： 0x00000000001ec64e
```

### 2.2 偏移落到哪一行

没有 pdb，直接反汇编那个地址：

```
$ objdump -d --start-address=0x1401ec58c --stop-address=0x1401ec660 Editor.exe
00000001401ec58c <_ZNK7physics11Heightfield3rawEii>:
    ...
    1401ec628:  mov 0x18(%rax),%rcx      ; rcx = hf->heights
    1401ec636:  mov 0x4(%rax),%eax       ; eax = hf->rows
    1401ec63b:  imul %rax,%rdx           ; col * rows
    1401ec644:  add %rdx,%rax
    1401ec647:  shl $0x2,%rax
    1401ec64b:  add %rcx,%rax
    1401ec64e:  movss (%rax),%xmm0       ; ← 崩在这
```

`+0x18` 正是 `Heightfield` 里 `heights` 的偏移（`cols`0 / `rows`4 /
`metersPerPixel`8 / `heightScale`12 / `origin`16..23 / `heights`24）。
也就是 **`heights[col * rows + row]` 这条读**。

而 `Heightfield::raw()` 里 col/row 是钳制过的：

```cpp
col = std::min(std::max(col, 0), cols - 1);
row = std::min(std::max(row, 0), rows - 1);
```

所以**不是越界下标**，是 `heights` 这个指针本身死了。

### 2.3 谁让它死的

`ScenePhysics::attachTerrain`：

```cpp
out.terrainHeights = asset->heights;        // 拷进 SceneBinding
out.terrainHf = std::make_unique<Heightfield>(hf);
out.terrainHf->heights = out.terrainHeights.data();   // ← 指向 binding 的 vector
out.world->setHeightfield(out.terrainHf.get());
```

`PhysicsWorld::setHeightfield` 当时是**借指针**（`m_terrain = hf`），注释写着
"调用方负责让它活得比世界长"。而两处调用方的活法完全不同：

| 调用方 | 写法 | 结果 |
|---|---|---|
| `RuntimeApp` | `m_binding = buildSceneWorld(...)`（**成员**） | 活得够久，侥幸没事 |
| `EditorApp` | `SceneBinding binding = buildSceneWorld(...)`<br>`m_physics = std::move(binding.world);` | 函数返回 → `terrainHeights` / `terrainHf` **析构** → 悬垂 |

原注释只想到了"`Heightfield` 结构体地址会随 `SceneBinding` 被移动"，所以用
`unique_ptr` 把**结构体**钉在堆上。但没人管**结构体里 `heights` 指向的那块
内存** —— 它属于 `terrainHeights` 这个 vector，vector 一析构就还回去了。

### 2.4 为什么以前不崩

| 版本 | 编辑器里有没有代码读 `heightfield()` |
|---|---|
| P6 之前 | **没有**。编辑器的碰撞求解走 `resolveBodyOverlaps`，刚体走 `PhysicsWorld` 但地形接触只影响刚体，而场景里通常没有刚体 |
| P6 起 | **有**。`EditorApp::editorGroundHeight()` 每帧调 `heightAt()` 做贴地 |

所以这个 bug 是 P6 埋的雷被 P7 引爆 —— 更准确地说，P6 **引用了**一颗早就
埋好的雷。

## 3 · 修法

`PhysicsWorld::setHeightfield` 从"借指针"改成**拷一份**，世界自持：

```cpp
if (hf && hf->valid()) {
    const std::size_t n = static_cast<std::size_t>(hf->sampleCount());
    m_terrainHeights.assign(hf->heights, hf->heights + n);
    m_terrainOwned = std::make_unique<Heightfield>(*hf);
    m_terrainOwned->heights = m_terrainHeights.data();
    m_terrain = m_terrainOwned.get();
}
```

- `m_terrainOwned` 用 `unique_ptr`：`PhysicsWorld` 对象本身走
  `unique_ptr` 移动，但万一将来有人按值搬世界，结构体地址也不会漂。
- `m_terrainHeights` 是成员 vector：`heights` 指向它，**与世界同寿**。
- 代价：`cols*rows` 个 float 一次拷贝。water_bowl 是 257×257 ≈ 258 KB，
  一次性，可忽略。
- `SceneBinding::terrainHeights` / `terrainHf` 保留（返回结构稳定 + 那份算好
  的 origin/mpp 有用），但**不再承担所有权职责** —— 头文件里已经写明"新代码
  不要拿 `terrainHf.get()` 当长期指针用"。

顺带把 `RuntimeApp` 那侧也变成"不再依赖运气"：它原本靠 `m_binding` 当成员
才没崩，现在这条隐性依赖没了。

## 4 · 为什么必须带地形才能复现

这是本节最值得记的一条**测试设计教训**：

- `sandbox.json`：Ground + BoxA/B/C，**没有 `TerrainComponent`**
  → `attachTerrain` 直接 `return`，`m_terrain` 是 `nullptr`
  → `editorGroundHeight` 走 `if (hf->valid())` 的 else，返回 0
  → **不崩**。
- `water_bowl.json`：`Terrain`（257×257, mpp=2.0, maxh=40）+ `Pond`
  → 高度场建起来 → `heights` 悬垂 → **必崩**。

所以 `verify_editor_play_terrain.py` 的 `DEFAULT_SCENE` 写死 `water_bowl`，
不用 `terrain_probe.json`（后者 `physics` 段是空的，"地形进不进物理世界"
取决于默认值，判据就不硬了）。

对照实验（同一份代码，只换场景）：

| 版本 | 场景 | 结果 |
|---|---|---|
| 修复前 | sandbox | alive |
| 修复前 | **water_bowl** | **CRASHED 0xC0000005** |
| 修复后 | water_bowl | alive，`z=1.708` 稳定 |

## 5 · 验收

```bash
python tools/verify_editor_play_terrain.py
```

| 判据 | 内容 |
|---|---|
| T0 | Shift+A ▸ Player ▸ Third-Person Player，`PLAYER-DEFAULT bound` |
| T1 | 点工具栏 Run（`TB-PLAY-run` 矩形），`Editor: PLAY in viewport` |
| **T2** | **进 Play 后 12 秒内进程活着**（旧版在这一步 `0xC0000005`） |
| T3 | `PHYS-TERRAIN attach cols=257 rows=257` —— 地形真的进了物理世界 |
| T4 | 连续 4 个 `PLAYER-STATE` 的 z 极差 < 0.02 m 且 z > 0.3 m（实测 1.708） |
| T5 | 0 条 `[VK_CHECK]` / `invalid state` |

实测：`7/7 PASS`，`z=['1.708','1.708','1.708','1.708'] span=0.0000`。

## 6 · 一条通用经验

**"调用方保证活得比我长"这种约定，只要有一个调用方用局部变量，就一定会
出事 —— 而且只在那一个调用方上出事。**

判断标准很简单：如果注释里要写"调用方负责让它活得比世界长"，那这个接口
就应该改成拷贝或 `shared_ptr`。物理世界里 258 KB 的地形数据，换一次
悬垂崩溃 + 一小时的 `objdump`，太划算了。
