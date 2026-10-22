# 渲染资源的生命周期约定

> 这份文档只回答一个问题：**什么时候可以销毁一个 Vulkan 对象**。
> 写它的直接原因是一次编辑器闪退（2026-10-22），根因是把"全局实例集的同步"
> 放进了离屏预览的路径里 —— 于是预览在**录制途中**把主场景地形的 buffer 拆了。

---

## 1 · 铁律

> **一条命令缓冲在 recording 状态时，它引用过的任何对象都不能销毁。**

违反它的表现**不是**"画错一帧"，而是整条命令缓冲当场作废：

```
[ERROR] [validation] vkCmdBeginRenderPass(): was called in VkCommandBuffer 0x... 
which is now in an invalid state (instead of recording state) because the
following objects bound to the command buffer were invalidated:
- VkBuffer 0x... was destroyed
- VkSampler 0x... was destroyed
  which was used by VkDescriptorSet 0x...
- VkBuffer 0x... was destroyed
```

之后**每一条** `vkCmd*` 都会再报一次同类的错（每个 VUID 各 10 条，然后
`duplicate_message_limit` 静音）。最终 `vkQueueSubmit` 返回
`-4 = VK_ERROR_DEVICE_LOST`，`VK_CHECK` 抛异常 → `Fatal: Vulkan call failed`
→ 进程退出。

### 读日志的正确姿势

**只有第一条 `invalid state` 是真的。** 它列出的对象就是被销毁的那批；
后面几十条是同一件事的回声。而"排在最后的报错"往往是**余波**，不是原因 ——
本例里最后 8 条是：

```
[VK_CHECK] Vulkan error -4 at engine/rhi/CommandPool.cpp:56
[WARN ] TERRAIN-LAYER-TEX-FAIL idx=0 albedo=... (Vulkan call failed)
```

看着像"地形贴图加载失败导致崩溃"，其实是设备已经丢了、`fromFile` 里随便
哪个 `VK_CHECK` 都会抛。**别从后往前读。**

---

## 2 · `uploadSceneState()` 的三个调用方不是一类东西

`Renderer::uploadSceneState(scene, extent, resetSkinning, camOverride,
offscreenSlot, faceView)` 里有三类活：

| 活 | 作用域 | 能不能给离屏目标做 |
|----|--------|--------------------|
| 相机 UBO / 视锥 / TAA 抖动 | 逐场景 | ✅ 必须做（各用各的槽） |
| `updateSkinning` / `buildDrawLists` | 逐场景 | ✅ 必须做 |
| **`syncTerrains` / `syncWaters`** | **全局唯一实例集** | ❌ **绝对不行** |

地形/水体的实例集是**全局一份**，键是**主场景里的实体 id + 资产路径**。
离屏目标（角色预览 / 额外视角 / 反射探针的面）传进来的是**另一个场景**，
那个场景里通常一个地形实体都没有 → `items` 为空 → 与 `m_terrainRequested`
对不上 → `same == false` → 走进"全量重建"：

```cpp
m_terrain->clearInstances();      // waitIdle + 销毁 VBO/IBO + 8 张层贴图 sampler
```

而离屏目标是在 `recordFrame` **录制途中**被调用的（`recordOffscreenTarget`
→ `uploadSceneState`），主场景地形的 VBO/IBO 与层贴图 sampler **正绑在
那条命令缓冲上** —— 于是第 1 节的铁律被违反。

### 修法（已落地）

```cpp
// uploadSceneState 末尾
if (offscreenSlot < 0) {          // < 0 == 主场景（调用方不带离屏槽）
    syncTerrains(scene);
    syncWaters(scene);
}
```

顺手把 `syncTerrains` 头注释里那句"必须在录制命令之前"从**注释**变成了
**结构**：唯一能走到这里的路径就是录制前的主场景上传。

---

## 3 · 判据怎么选：`offscreenSlot < 0`

`Renderer.h` 里这个参数的定义本来就是"写哪个相机 UBO 槽"：
`>= 0` = 该离屏目标的本帧槽，`-1` = 主链 UBO。所以它天然就是
"这是不是主场景"的判据，不用另加一个 bool（多一个 bool 就多一处能忘）。

| 调用点 | offscreenSlot | 时机 |
|--------|---------------|------|
| `recordFrame` 主场景 | `-1`（默认） | vkBeginCommandBuffer **之前** |
| `recordOffscreenTarget`（预览/额外视角） | `target.slotIndex()` | **录制途中** |
| 反射探针逐面 | `kProbeFaceUboSlotBase + face` | **录制途中** |

> `resetSkinning` **不要**拿来当这个判据。它现在同时被用来区分
> "统计口径算不算主场景"，语义已经够满了；再叠一层"要不要同步全局实例"，
> 下次改其中一件事就会误伤另一件。

---

## 4 · 同类坑：逐帧重建 = TDR = DEVICE_LOST

`-4` 还有另一个来路，`Renderer.cpp` 里已经写了警告：**单次 GPU 提交超过
Windows 的 TDR 2 秒上限，驱动会重置设备**，`vkQueueSubmit` 一样返回 -4。
地形重建（`waitIdle` + 销毁/新建 sampler + 重传 3 MB 顶点）单帧上百毫秒，
逐帧跑几分钟就能把驱动拖垮 —— 这就是 `kTerrainRetryFrames = 120` 与
`m_terrainDegraded` 的由来。**任何"每帧都比对一次、不一致就重建"的写法
都要先问：判据会不会恒为"不一致"。**

---

## 5 · 验收

`tools/verify_preview_terrain.py`（8 条断言）

| 断言 | 抓什么 |
|------|--------|
| P0 | 场景带地形读进来（`TERRAIN-INSTANCE`） |
| P1 | Shift+A ▸ Player ▸ Third-Person Player 点击成功 |
| P2 | 预览目标真的建出来（`Preview render size`）—— 否则是空跑 |
| P3 | 0 条 `invalid state` |
| P4 | 0 条 `[VK_CHECK]` |
| P5 | 地形只建 1 次（`TERRAIN-INSTANCE=1 TERRAIN-LOAD=1`） |
| P6 | 0 条 `TERRAIN-LAYER-TEX-FAIL` |
| P7 | 点完之后编辑器还活着 |

**反向验证已做**：把 `if (offscreenSlot < 0)` 临时改回无条件调用，
用例精确变红 —— P3/P4 红、`TERRAIN-LOAD=2`、进程 `exit=1`，症状与用户
现场逐字一致。所以它不是一条"永远绿"的用例。

⚠ **这条用例必须带地形。** 空场景下主场景与预览场景的 `items` 都是空，
`same` 恒真，再坏也抓不到 —— 别为了跑得快把 `Terrain` 实体删掉。
