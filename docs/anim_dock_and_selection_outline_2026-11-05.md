# 选中轮廓高光 + 底部动画坞（2026-11-05）

> 两条用户需求一次做完：
> 1. 「当选择方式是普通选择时，鼠标点击场景中的物体应该要呈现出轮廓边缘高光的效果。」
> 2. 「动画、骨骼、运镜相关的面板要和场景内容对照着看……浓缩到一个操作面板，放在
>    整个编辑器的底部，类似 UE5 和 Blender 那样。」
>
> 判据：`tools/verify_selection_outline.py`（A/B/C 组）、
> `tools/verify_anim_dock.py`（A/B/C/D/E 组）。两条都进了 `run_regression.py`
> 的 DEFAULT + TRIGGERS。

---

## 一、选中轮廓高光

### 1.1 第一版为什么不成立（反向外壳）

UE 的选中高亮是"反向外壳"：把选中的网格再画一遍，顶点沿**法线**外扩，正面剔除，
于是只剩物体外面一圈壳。这条路在本仓**撞了两次**：

| 现象 | 原因 |
|---|---|
| 屋檐 / 方块屋顶的描边是**虚线** | 硬边网格的法线**逐面分裂**。相邻两个面朝各自法线方向外扩 → 棱角处裂开一条缝 |
| 薄片（屋顶平面、墙面）**完全没有边** | 单面网格没有背面，`FRONT` 剔除之后什么都不剩 |

改"沿包围盒中心做屏幕空间外扩"能治第一条，但治不了第二条，而且要把顶点着色器的
蒙皮 / morph / instancing / MDI 四条分支再复制一份（"改了蒙皮忘了改描边"的经典坑）。

### 1.2 最终实现：几何缓冲打标记 + 后处理扩张

```
深度预通道（已有）                        后处理合成（已有）
  选中物体 → outGeo.a = -视图距离   ──→   找"中心不是负、邻域有负"的像素 → 染暖橙
  普通物体 → outGeo.a = +视图距离
```

* **零成本**：几何缓冲本来就写（NPR 描边 / SSAO 都在用），这里只改 a 通道的符号。
* **遮挡免费正确**：深度预通道只保留最近表面，被挡住的选中物压根不出现在缓冲里。
* **薄片 / 硬边都成立**：判据是"像素属不属于选中物"，与网格拓扑无关。

改动点：

| 文件 | 改动 |
|---|---|
| `assets/shaders/depth_only.frag` | 读 `push.pbr.x`（1 = 选中），把 `outGeo.a` 写成**负**距离 |
| `assets/shaders/post.frag` | `geoEdge()` 先取 `abs()`（NPR 描边不受影响）；新增 `selectionOutline` 段：8 邻域 ×`pc.camfx.w` 像素宽，中心是选中物则不画（线在**外侧**） |
| `engine/render/Renderer.h/.cpp` | `DrawItem::entityId/selected`、`setSelectionOutline(ids)`、`DrawBatch::selected` 进**聚合键**（每批只能推一次 `push.pbr.x`） |
| `engine/render/PostProcess.h/.cpp` | `camfx.z = 开关`、`camfx.w = 线宽`（这两个通道原本是"保留"，不新增推常量字段） |
| `editor/EditorApp.h/.cpp` | `syncSelectionOutline()`：每帧在 `drawFrame` 之前喂 id，**只在普通选择方式 + 编辑态**喂 |

### 1.3 三个容易踩的细节

1. **深度预通道的推常量阶段位**从 `VERTEX` 扩到了 `VERTEX | FRAGMENT`。改了范围就必须
   同步改**所有** `vkCmdPushConstants` 的 stageFlags，否则刷
   `VUID-vkCmdPushConstants-offset-01795`。
2. **后处理那段必须取 `abs()`**。不取的话"选中物的深度是负数"，与邻居的相对深度差爆掉，
   会沿着选中物描出一整片黑。
3. **几何缓冲用最近邻采样器**（`m_geoNearestSampler`）。线性过滤会把 a 通道的符号
   插成 0 附近的值，一条 2px 的线会被检出两条、或者整条消失。

### 1.4 为什么只在"普通选择"下画

框选 / 套索 / 刷选的过程中选中集合每拖一下都变，描边会跟着闪；而且那三种手势本身
已经有矩形 / 套索线 / 刷痕作为可视反馈。判据在 `EditorApp::syncSelectionOutline`，
非 Pick 模式传**空列表**（= 一条都不画，与"这个功能不存在"逐像素等价）。

实测（`_venn_verify/selout_selected.png` 对照 `selout_before.png`）：
选中后视口内新增暖橙像素 **+325**，取消选中回到 **0**，右上天空对照区变化 **0.00%**。

---

## 二、底部动画坞（AnimationDock）

### 2.1 为什么不是"再写一套时间轴"

六张面板里已经积累了关键帧选择 / 镜像 / 对齐 / 注入脚本 / NLA 拖拽 / 运镜
nudge-then-bake 的大量逻辑。重写等于把那些判据全部作废。所以坞只做**壳**：

1. 一条标签条（六选一，同一时刻只开一张）
2. 一条传输条（回起点 / 上一帧 / 播暂 / 下一帧 / 到终点 + 帧号）
3. 一条时间标尺（拖播放头 = 直接改 `EditorContext::animTime()`）

内容面板用 `pushDockHint()` 钉进坞的矩形 —— **面板代码一行没改**（NLA 是唯一
补了 `applyDockHint` 的，它之前不支持托管）。

### 2.2 布局：三条互不重叠的横条

```
状态栏 → 底栏 → 动画坞 → Content 抽屉（停靠时）→ 主体（模式栏 / 层级 / 视口 / 属性）
```

* 坞的高度是 `EditorApp::m_animDockH`，**引用**传给 `AnimationDock::draw`，拖上边缘
  就地改；`computeLayout` 每帧把它夹到"主体至少还剩 `kMinViewport`"。
* ⚠ 坞和 Content 抽屉的高度必须塞进**同一次** `setBottomInset`。分两次 apply 的话
  第二次会用"只含抽屉"的 inset 覆盖掉坞 → 坞画在主体上面、视口却没变矮。
* 坞的显隐**没有自己的开关位**：`anyOpen()` = 六张面板里有没有一张开着。于是菜单勾选、
  底栏托盘、快捷键、落盘全部沿用面板表那一套，一个字都不用新加。

### 2.3 六张面板从视口标签条上摘掉

`ViewportTabs::isTabHosted()` 多了一条 `!AnimationDock::ownsPanel(id)`。不摘的话点开
摄影表会把 3D 视口整块顶掉 —— "对着场景调动画"在布局上又不成立了。

`drawPanels()` 也 `continue` 掉这六张：`cine`（运镜）在表里有真实 draw 指针，不跳过
就会被画两次（一次浮在屏幕中央、一次钉在坞里）。

### 2.4 实测

| 场景 | 结果 |
|---|---|
| 打开摄影表 | `ANIMDOCK open=1 tab=dope height=300 bodyY=1028`；视口 (310,139)-(1260,534)，坞 (0,542)-(1600,842)，**不重叠** |
| 六张全关 | 坞不出现，视口高度 395 → **695** |
| 点「运镜」标签 | `tab=cine`，运镜面板的 `CINE-*` 锚点落在 `ANIMDOCK-BODY` 里 |
| 点 ▶ / 再点 | `ANIMDOCK play=1` → `play=0` |
| 点标尺 | 播放头矩形 720 → 799 → 1359（真的跟着走） |

### 2.5 传输条的帧号读数：一个被 i18n 判据逼出来的真错

第一版为了"不进 i18n 词表"，把读数写成自拼格式串：

```cpp
std::snprintf(buf, sizeof(buf), "F %d  %.2fs", frame, ctx.animTime());
```

它确实绕开了 `TR()`，但**没绕开判据**：`verify_i18n_coverage.py` 的 D 组
（"裸格式串"）判据是"剥掉 `%d/%s` 之后还剩 ≥2 个字母就算有英文"，
`"F %d  %.2fs"` 剩 `F` + `s` → 当场 2 项报红。而红得有道理：中文界面下
那一格永远挂着英文 `F`。

修法不是豁免，而是**真的翻**：

```cpp
std::snprintf(buf, sizeof(buf), TR("Frame %d  %.2fs"), frame, ...);
// ZhTable: {"Frame %d  %.2fs", "帧 %d  %.2fs"}
```

* 末尾的 `s`（秒）按 `CharacterWindow` 的 `%.3fs` 惯例**保留不翻** —— 单位
  缩写翻成"秒"会让整格变宽、把传输条推到换行。
* 同一批里 `"ANIMDOCK-TAB-%s"` 也报了 D 组，但那条是**纯日志锚点**
  （`verify_anim_dock.py` 按 `ANIMDOCK-TAB-<id>` 找标签矩形），标 `// i18n-keep`。

⚠ 教训：**"我不想让它进词表" ≠ "它不在界面上"**。D 组就是为这类"自拼
字符串"开的 —— 包一层 `TextUnformatted` 不会让英文变成中文。

### 2.6 截图复核带出来的三个真 bug（都不是"坞"本身）

坞做完后按惯例截图自查，放大看标题行发现字被切了一半，顺着这条线又挖出两个
同源的毛病。**三个都是"面板搬进矮窗口"才暴露的**，浮动窗时代（800×600）看不出来。

| 现象 | 根因 | 修法 |
|---|---|---|
| 标签文字下半截齐刷刷没了；`0.00s` 被右沿切掉 | `kHeaderH = 30` / `kRulerH = 22` 写死，本机字号 22px，按钮内文字比窗口裁剪矩形高 | `headerH() = 字号 + 14`、`rulerH() = 字号 + 8`；传输条宽度改成 `CalcTextSize(帧号格式串) + 按钮宽` 实算 |
| 摄影表画布顶出坞底 15px（`verify_dope_sheet` F2） | `std::max(avail.y - reserve, 200)` —— 200 是**下限**，短窗口里硬撑 | 先保"不出窗"（`min(bodyH, avail.y)`），200 只当舒适值 |
| 曲线画布顶出坞底 36px、最高那个键只剩 1.3px 在画里（A8） | `drawBody` 里 `avail.y - 5.2 行` **重复预留**（header 行已在同一窗口画过，`GetContentRegionAvail()` 已经扣过）+ 180 下限 | `bodyH = avail.y`，只在 `< 60` 时兜底；画布同样去掉 160 下限 |

第四个是**锚点陈旧**（不是布局 bug，是日志 dedup 的锅）：`CURVED-KEY` 的去重键
只含 `i:t:v:sel`，画布一挪而键值没变就再也不重打 → 脚本读到上一帧的坐标，
数值全对、整体偏 14px，看着像"键画歪了"。去重键必须**带上 px/py**
（锚点是被当坐标用的，坐标就是它的语义）。

第五个是**判据跨帧**：`actDragKeysPx` 用 `m_plot`（上一帧画布）换算，而面板
刚从浮动窗钉进坞的一两帧里窗口还在收位（上沿 690 → 706、宽 1590 → 1577）。
`verify_curve_editor` 原来拿"全文最后一条 MAP"算期望，差值 0.29 全记在
"值没跟着像素走"上。改成 `map_at(text, "CURVED-DRAG-END")` —— 取**拖动那一刻**
生效的那张映射表。产品行为本来就是对的（稳定布局里两者一致）。

> **共同点**：面板从"独立浮动窗"搬进"共享的矮条"时，所有"至少这么高/这么宽"
> 的硬下限都会变成溢出，所有"窗口很大所以无所谓"的常量都会变成裁切。
> 搬完必须放大量截图看一遍 —— 用例只判锚点在不在、在不在一起，判不出"字被切了"。

---

## 三、改到的文件

**新增**
* `editor/AnimationDock.h/.cpp` —— 坞的壳（标签 / 传输条 / 标尺 / 高度拖拽）
* `tools/verify_anim_dock.py`、`tools/verify_selection_outline.py`
* 本文

**修改**
* `editor/EditorApp.h/.cpp` —— `drawAnimationDock` / `drawAnimationDockContent` /
  `syncSelectionOutline`；`computeLayout` 的第三段 inset；`drawPanels` 跳过六张；
  onImGui 里删掉那五张"每帧无条件画"
* `editor/ViewportTabs.cpp` —— 六张不再是标签页
* `editor/NlaPanel.cpp` —— 补 `applyDockHint` + 稳定 ID `###panel_nla`
* `engine/i18n/ZhTable.cpp` —— 传输条读数词条 `Frame %d  %.2fs`（见 §2.5）
* `assets/shaders/depth_only.frag` / `post.frag` —— 选中标记与扩张
* `engine/render/{Renderer,PostProcess}.{h,cpp}` —— 选中集合与开关
* `tools/verify_dope_sheet.py` —— F2 判据从"窗口 >600×300"改成"窗口 == 坞的内容区"
  （面板搬家了，旧阈值永远过不了，而那**不是**回归）
* `editor/DopeSheetWindow.cpp` / `editor/CurveEditorPanel.cpp` —— 画布的硬下限
  改成"剩下多少用多少"（§2.6 ②③），`CURVED-KEY` 去重键带 px/py（§2.6 ④）
* `tools/verify_curve_editor.py` —— 期望改用 `map_at(text, "CURVED-DRAG-END")`
  （§2.6 ⑤）
* `tools/_shot_anim_dock.py` —— 出图小工具（不参与闸门，同 `_probe_*.py` 惯例）
* `tools/run_regression.py` —— 两条新用例进 DEFAULT + TRIGGERS

## 四、留白（明确没做）

* **坞的标签不能拖拽重排**：顺序写死在 `kTabIds[]`（动作 → 骨骼 → 片段 → 镜头）。
  真要做，参照 `PanelDragDrop` 那一套，但六张的语义顺序比用户偏好更重要。
* **标尺范围（0~5s）是坞自己的状态，不是片长**：片长要问 `AnimatorComponent` + 资产，
  而资产是异步加载的。等"坞能读到绑定 clip"时再改成自动贴合。
* **运镜面板的 nudge-then-bake 走的是视口相机**：坞把它钉在底部之后，视口仍然完整可见，
  所以这条链路反而更好用了 —— 但它和 `CameraSystem` 的耦合没动。
* **选中描边不做彩色 / 不做多选框内填充**：只做"外轮廓一圈"。框选多选时描 N 圈
  （`ids>=2` 已按集合处理，实测可用）。
