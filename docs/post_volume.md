# 后处理体积（Post-Process Volume）

> 2026-11-02。补的是**入口**，不是能力 —— `scene::PostVolume` 从 R1-2/R1-3
> 起就是完整的，缺的只是"编辑器里放不出来"。

---

## 1 · 它是什么

场景里一个世界空间 AABB，相机进入后，它携带的一组**相机效果覆盖值**
按「优先级 + 过渡带权重」混进当前相机的 `CameraEffects`。

可覆盖的项（`engine/scene/PostVolume.h`）：

| 项 | 形态 | 说明 |
|---|---|---|
| 景深开关 / 焦距 / 光圈 / 最大模糊半径 | 三态 + 3 个浮点 | `dof` / `dofFocus` / `dofAperture` / `dofMaxRadius` |
| 运动模糊开关 + 强度 | 三态 + 浮点 | `motionBlur` / `motionBlurStrength` |
| 色差开关 + 强度 | 三态 + 浮点 | `chromatic` / `chromaStrength` |
| 暗角开关 + 强度 | 三态 + 浮点 | `vignette` / `vignetteStrength` |
| 调色 LUT 混合 | 浮点 0..1 | `cgLutBlend` |
| 自动曝光偏置 | 浮点 EV | `autoExposureBias` |

**"不覆盖"是显式的**：浮点用 `kNoOverride = -1`，三态用 `-1`。
面板上表现为**没勾选**；取消勾选写回 -1，**不是写 0**。
（0 是有效值：强度 0 = 关。两者混淆会让"关掉覆盖"变成"把效果关掉"。）

---

## 2 · 怎么用

1. 底栏 `+` 托盘 / `Window` 菜单里打开 **Post-process volumes**
   （面板 id `postvol`，无快捷键 —— 与 Physics / Quality / Sky 同款）。
2. **New volume** → 在**相机脚下**放一个 8×8×8 米的盒子，并自动选中。
   落点是相机而不是原点：原点 ±1 在大多数场景里离相机十米开外，
   建完什么都看不到 → 用户第一反应是"面板坏了"。
3. 拖 `min` / `max`（或先拖相机再点 New）把盒子挪到目标区域。
4. 勾一项覆盖（例如 Depth of field），设焦距。
5. 看面板底部 **Live (view camera)**：
   - `camera (x, y, z) — N of M active`
   - 只列**被覆盖**的项及其最终值
   - 若当前体积对相机没有影响，会明确写出"在盒子 + 过渡距离之外"

> ⚠ 体积生效与否**在画面上很难判断**：可能是没开、可能是相机不在盒子里、
> 也可能是被另一个高优先级体积压掉了。Live 那一段就是为了不用"改一下看画面
> 变没变"来二分。

### 控制台命令（与面板共用同一套实现）

```
pv.list                 列出本场景所有体积（含全部覆盖值）
pv.eval [x y z]         在某个点求值（默认相机）；打印 active 数与最终混合值
pv.add <name> [x y z]   加一个体积（默认圆心 = 相机，半边长 4 m）
pv.remove <index>       按 pv.list 的下标删
```

⚠ 命令与面板按钮都走 `postVolumeAdd` / `postVolumeRemove`（`PostVolumePanel.h`）。
分开写两套的后果是"命令能建、按钮建不出来"，而判据只覆盖其中一条。

---

## 3 · 语义细节（踩过才知道）

- **`blendDistance` 是盒外宽度**。0 = 硬切（进出体积时参数会突跳）；
  1 m 意味着相机在盒外 1 m 内时权重从 0 线性升到 1。
- **权重 >= 0.5 才"采用"**，< 0.5 且已有更高优先级的值时按权重插值
  （`PostVolume.cpp` 的 `blendFloat`）。所以"过渡带里调参数"不是完全无级
  的 —— 这是刻意的：半截参数比突跳更难查。
- **优先级升序叠加**：后写的高优先级覆盖前面的，与 UE"小体积压大体积"的
  直觉一致。
- **`autoExposureBias = -1.0` 表达不出来**（-1 是哨兵）→ 用 -0.99 代替。
  这是"浮点哨兵统一取 -1"的代价，写在这里免得以后踩。

---

## 4 · 判据

`tools/verify_post_volume.py`（43 条，四组）：

| 组 | 验什么 |
|---|---|
| A | 读场景 / 求值（盒内 active=1、盒外全 -1）/ **落盘往返** / `pv.add` 默认落在相机 / `pv.remove` |
| B | 面板锚点全在 + **未覆盖的项不画滑条**（`kNoOverride` 在 UI 上唯一的可见证据） |
| C | **点 New volume 按钮**真的能建（不是只有命令能建） |
| D | 老场景（没有 `postVolumes` 键）不许出事 |

⚠ A 组顺手抓到一个真 bug：`cgLutBlend` / `autoExposureBias` 只进了结构体，
**没进 `toJson` / `fromJson`** —— 设了 LUT 混合 0.7、存盘再读回来变 0.0，
而且一句报错都没有。已修（`SceneSerializer.cpp`）。

**不做像素判据**：焦点从 5 m 改到 6 m，1400×900 的静帧里可能只有几十个像素的
模糊半径差，低于噪声。像素留给 `verify_camera` / `verify_postfx`。

---

## 5 · 已知边界

- **视口里没有线框**。放一个体积、相机不在里面时，用户只能靠面板的 Live 段
  判断，视口里看不见那个盒子。补它要在渲染层加一条调试线通道
  （现有 `rp.jointGizmos` 是 mesh 路线，可以复用，但那是另一刀）。
- **面板编辑不进撤销栈**。与 `EnvironmentPanel`（天空）同款：连续拖拽改值
  只标 dirty，只有"新建 / 复制 / 删除"走 `structuralEdit` 快照。
  根因是 `structuralEdit` 的粒度是**整场景 JSON**（见 `panel_gap_review_2026-11-02`
  的 D1），这一条要一次改在命令层，而不是每个面板各修一遍。
- **体积不是实体**：Hierarchy 里没有它，点选 / 框选 / gizmo 都不适用。
