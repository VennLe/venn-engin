# 二次元风格 UI 改造（2026-10-27）

> 用户原话：「我的引擎的 UI 部分的外观设计显得有点粗糙简陋，我的定位是一款
> 二次元风格的 3d 游戏引擎，那么就要有浓厚的二次元可爱的风格。」后面跟了六条。
>
> 本文记录 **每一条落到哪个文件、怎么验、还差什么**。六条的编号与用户消息一致。

---

## 0. 一句话结论

| # | 需求 | 状态 | 验在哪 |
|---|---|---|---|
| 1 | 启动器窗口调大 | ✅ 1080×720 → **1400×880**（按工作区夹取） | `verify_theme_anime` B1 |
| 2 | 窗口圆角 + 顶栏不呆板 | ✅ 圆角 12（动漫档）/ 8（工程档）；顶栏掺强调色 + 渐变带 | A（度量）+ B3（渐变） |
| 3 | 面板配色错落有致 | ✅ 四档明度 + `cardBg/panelBgAlt` 拉开 + 选中行去饱和 | A（anime 层次） |
| 4 | 面板自由拖拽 / 合并 | ⬜ **未做**（见 §5：要做就得动布局内核） | — |
| 5 | 按钮/图标更像二次元 | ◐ 药丸按钮 + 危险色接 token + 控件加厚；**图标集未重画** | B4 + A |
| 6 | 主题多样化 | ✅ 5 → **10 套**，Preferences 里有一排**可点色卡** | A3 + C1/C2 |

`--selftest theme` 22 条 · `verify_theme_anime.py` 9 项 · `verify_theme_language.py` 12 项（既有，无回归）。

---

## 1. 主题系统（第 2/3/6 条的地基）

改动集中在 `engine/ui/ImGuiTheme.{h,cpp}`。

### 1.1 圆角从"全局"变成"每主题一份"

原来 `applyBaseMetrics()` 里写死 `WindowRounding = 4`，注释还写着"尺寸不属于主题"——
那是为了保住像素断言。但**圆角是风格**：动漫风要圆、工程风要直。

于是拆成 `ThemeMetrics`：

```cpp
struct ThemeMetrics {
    float windowRounding, childRounding, frameRounding, popupRounding;
    float scrollbarRounding, grabRounding, tabRounding;
    float windowBorder, childBorder, frameBorder, popupBorder;
    float framePadY;   // 控件"厚度"
};
```

**为什么这次可以拆、上次不行**：`WindowPadding / ItemSpacing / FramePadding.x` 一动，
面板矩形就全变，一批像素断言要重算；而圆角只动窗口**四个角的几十个像素**。
所以拆的边界划在"圆角与边框宽度可动，padding/spacing 仍全局共用"。

### 1.2 新增 5 套动漫主题（下标 5~9）

| 下标 | 名字 | 底 | 强调 |
|---|---|---|---|
| 5 | **Sakura Night** ★出厂 | 深紫黑 | 樱花粉 |
| 6 | Matcha Mint | 深墨绿 | 薄荷青绿 |
| 7 | Sunset Pop | 深葡萄紫 | 夕阳橘 |
| 8 | Cyber Neon | 近黑藏青 | 霓虹青 |
| 9 | Sakura Day | 奶油樱粉（浅色） | 樱粉 |

**只能追加，不能插队**：偏好落盘存的是**下标**，往中间插一套会让老配置
静默换成另一套主题。`--selftest theme` 的 A 组（`venn_window_bg` +
`venn_accent_*` + Daylight 两条）就是把 0~4 钉死的护栏。

### 1.3 ⚠ 压暗（`darkenThemeColors`）—— 这条最反直觉

本工程的交换链把 ImGui 的浮点色**当线性值再编码一次**（坑点 #34）：

```
屏幕值 ≈ 设定值 ^ (1/2.2)
```

所以 `windowBg = 0.106` 上屏是 **RGB 92**（中灰），不是深灰。
也就是说"深色主题"在这块屏上从来没有真黑过——用户抱怨的"硬朗/简陋"里，
有一半是**所有深色主题都被抬成了中灰**。

动漫档的做法是对整套颜色做一次 `c^1.45`：

| | 设定值 | 上屏 |
|---|---|---|
| 窗口底 | 0.094 → 0.032 | RGB 95 → **RGB 59** |
| 强调色 | 0.98 → 0.97 | 250 → 250（亮部几乎不动） |

幂运算而不是减法：保 0 与 1、保色相、只拉开暗部。**浅色档不压**（要的是奶油白）。

### 1.4 层次（第 3 条）

原来一个面板里只有两档明度（`panelBg` / `panelBgAlt`），堆起来就糊成一片。
动漫档把它拉开成四档，并且**接上 `ImGuiCol_ChildBg`**：

| 层 | 值（动漫深色档） |
|---|---|
| 窗口底 | 0.032 |
| `ChildBg`（卡片，`BeginChild(...,true)`） | = `cardBg` ≈ 0.066 |
| `panelBgAlt`（分组头 / 交替行） | ≈ 0.08 |
| `cardBgHover` | 再掺 26% 强调色 |

> 第一版忘了接 `ChildBg`，实测左右两栏与窗口底**只差 3 个色阶**，
> 截图里几乎看不出有"卡片"。

### 1.5 标题栏（第 2 条）

`ImGuiCol_TitleBgActive` 不再是"底色 + 一条线"，而是 **掺 14~20% 强调色**的一块带：

```cpp
t.tok.titleBarActive = mix(t.titleBgActive, accent, isLight ? 0.14 : 0.20);
```

未激活压回 `titleBar`（= `titleBg`，与改动前同值），于是"哪个面板是活的"一眼可见。
**只改颜色不动结构**：ImGui 1.93 的标题栏是硬编码矩形，要做圆角顶边只能改
`third_party/imgui`，不值得——圆角靠 `WindowRounding` 落在窗口四角，观感已经够。

---

## 2. 装饰件（`engine/ui/UiChrome.{h,cpp}`，新增）

ImGui 的原生绘制只有"平涂 + 1px 直线 + 方角"。需要自己画的四件事收敛到一处：

| 函数 | 用途 |
|---|---|
| `gradientRectV` | 垂直渐变（圆角版：先铺底再裁上半段，绕开 `AddRectFilledMultiColor` 不支持圆角） |
| `cardBackground` | 卡片：渐变 + 软边框 + 外发光 |
| `headerBand` | 顶栏：**左深右亮**的水平渐变 + 一条更亮的底边 |
| `pillButton` | 药丸按钮：圆角=半高、渐变底、悬停顶部高光线 |
| `statusPill` | 彩色小胶囊（Running / Dirty 这类状态词） |

**纪律**：

1. **颜色一律从 `ui::tokens()` 取**，不许写死——浅色主题下写死的深色渐变会变黑斑。
2. **不改布局**：`pillButton` 用 `InvisibleButton` 占位，尺寸由调用方给，
   语义与 `ImGui::Button` 一致（含 `SameLine`）。所以"按钮换形状"不会让
   任何面板矩形漂移。
3. **只自绘按钮这一类纯点击控件**。`Checkbox` / `Slider` 内部还有键盘导航、
   拖拽、tooltip，重画必丢——勾选框的"圆润"靠 `FrameRounding = 9` 拿到。

---

## 3. 启动器（第 1 条）

`launcher/LauncherApp.cpp`：

* `kWindowW/H` 1080×720 → **1400×880**；`kHeaderH` 54 → 76；`kLeftW` 330 → 380。
* `onInit` 里按工作区夹取（`kWinWPct = 0.94` / `kWinHPct = 0.90`）**再**居中。
  ⚠ 顺序不能反：先居中再缩，窗口会从中心往右下缩，看起来像"跑偏了"。
  夹过会打一行 `LAUNCHER-WINSIZE clamp=...`。
* 顶栏换成 `ui::headerBand` 渐变 + 标题放大 1.3×。
* 主 CTA（Open editor / Run game / + New game）换成 `ui::pillButton`，
  **尺寸与原来的 `ImGui::Button` 逐像素一致**，所以 `LAUNCH-BTN-*` 锚点不动、
  `verify_launcher_ui` 不用改。

---

## 4. 主题色卡（第 6 条）

`EditorApp::drawPrefRow` 里对 `id == "theme"` 特判，在下来框下面画 **10 枚色卡**：
上 = 强调色、下 = 窗口底，点一下立刻切换，悬停显示名字。

* 锚点 `PREFS-THEMECHIP-<i>`（脚本可点）。
* 日志 `EDITOR-THEME index=<i> name=<name>`。
* **落盘仍然只存下标**（走 `setChoice`），色卡不引入第二份状态。

`ui::themeSwatch(i, bg, accent)` 是配套的**纯查询**接口（不改当前主题）。

---

## 5. 第 4 条（Blender 式自由拖拽 / 合并）—— 三刀全部完成

> **2026-10-27 更新**：用户已选定路线 **B**（`SplitLayout` 升可变树）。
> **E29-1（槽位映射可写 + 落盘）/ E29-2（拖拽交换）/ E29-3（合并成标签组、
> 点标签切换、双击拆回）三刀已完成并全绿**，
> 后续可做（切分新格 / 标签拖出）→ `docs/editor_docking_roadmap.md` §7。
> 下面这一段保留为当初的决策记录。

现状：`editor/SplitLayout` 是一棵**写死的 3 节点分栏树**，5 个槽位
（Hierarchy / Content / Viewport / Inspector / Modes）由 `PanelDesc::layoutSlot`
常量表绑定。能拖的只有**分隔条**（改比例、双击折叠、右键预设、Shift+Space 最大化）；
"把面板拖到别处 / 两个面板合成标签页"做不到。

两条路：

| | 做法 | 代价 | 风险 |
|---|---|---|---|
| **A** | 换 `third_party/imgui` 为 **docking 分支** | 换一份第三方代码 | 现有 20+ 面板的 `Begin/SetNextWindowPos` 全部要改成 `DockSpace` 语义；锚点锚在旧矩形上，一批用例要重写。**且当前环境无外网**，拉不到分支 |
| **B** | 把 `SplitLayout` 从"固定树"升级成"**可变树 + 槽位→面板映射**" | 约 800 行：`PanelRegistry` 的 `layoutSlot` 从常量变运行时表；`SplitLayout` 支持四向 split / 合并；拖拽用 `BeginDragDropSource/Target` | 可控：不动第三方代码，锚点体系保留，可以**分批落地** |

推荐 **B**，分三刀：

1. **槽位映射**（先把 `layoutSlot` 变成运行时可写）：`slotPanel[5]` + 落盘。验收：现有用例全绿（行为不变）。
2. **拖拽换位**：把面板标题栏做成 drag source，四个槽位是 drop target，落下即**交换**。
3. **合并成标签组**：一个槽位容纳 `vector<panelId>`，槽顶画标签条（复用 `ViewportTabs` 的样式）。

每一刀都要补 `--selftest` + `verify_*`，因为"布局变了但用例仍然绿"是本仓库
反复踩过的坑（记忆：**布局一变，"按 rect 点控件"的用例会静默漂移**）。

---

## 6. 怎么验

```bash
# 纯数值（不建窗口、不要 GPU）
Game.exe --selftest theme          # 22 条：下标稳定 / 每套不同 / 明暗自洽 / 度量 / 越界夹取

# 端到端（要窗口，GUI 用例同时只能跑一个）
python tools/verify_theme_anime.py   # A 自检 + B 启动器 + C 色卡
python tools/verify_theme_language.py  # 既有：5 套主题按亮度分区 + 语言切换
```

`verify_theme_anime.py` 的三组：

* **A** 自检 22 条 + **分组存在性**（只数总数的话"整组被删掉"也能凑够）+
  `theme_count == 10`。
* **B** 启动器：客户区 `> 1080×720 且 <= 1400×880`；窗口底 **B>G 且 R>G 且亮度 < 90**
  （深紫，不写死 RGB，微调色号不该让用例红）；顶栏**左端比右端亮 6 以上**（渐变存在）。
* **C** 编辑器：**先点 `PREFS-TAB-Appearance` 切页**，再数 `PREFS-THEMECHIP-0..9`
  十个锚点；点第 6 枚后 Preferences 窗口内 **G 相对 R 的抬升**比点击前更明显 ——
  这是"色卡不是画着好看"的唯一证据。

  ⚠⚠ **C 的顺序不能反**：色卡只在 Appearance 页绘制，而 Preferences 默认停在
  Interface 页。先等色卡 = 白等 10×12 s 然后全红，而 C2 却是绿的（因为 C2 之前
  才点页签）。已踩过一轮，改成 C0（找到并点击页签）→ C1（数色卡）的硬依赖。

### 6.1 本轮实测（全部 PASS 的原始数字）

```
--selftest theme                 22/22   THEME-TOTAL pass=22 fail=0
--selftest save                  77/77   SAVE-TOTAL pass=77 fail=0（存档无回归）
verify_theme_anime.py             9/9
  B1 size=1400x880（旧 1080x720）
  B2 窗口底 rgb=(73,69,77) luma=71.1  ← 压暗前是 rgb=(95,90,99)
  B3 顶栏 left=123 right=68（渐变存在）
  C1 缺=无
  C2 before r=72 g=66 | after r=63 g=76（点 Matcha Mint 后 G 抬升 10、R 降 9）
verify_theme_language.py         12/12   （主题 0~4 亮度分区逐位不变）
verify_add_menu.py               全 PASS （播放条换 token 后底栏/菜单无漂移）
check_regression_map.py          PASS
```

`verify_theme_language` 的五套老主题数字（**证明 0~4 一个像素都没动**）：

| index | name | prefs-luma | TB | TR |
|---|---|---|---|---|
| 0 | venndark | 102.4 | 98.7 | 103.5 |
| 1 | uedark | 85.2 | 82.3 | 87.8 |
| 2 | blenderdark | 122.5 | 120.1 | 123.5 |
| 3 | unitydark | 109.9 | 106.9 | 111.0 |
| 4 | daylight | 245.3 | 239.3 | 237.3 |

---

## 7. 还没做的（按优先级）

1. **第 4 条**（见 §5，三刀）。
2. **图标集重画**：`editor/EditorIcons.h` / `InspectorIcons.h` 是矢量现画的，
   现在是"文件夹/文件"这类功能图形，不是动漫符号。要换的是设计不是代码结构——
   建议先出一张对照图再改，否则改完没人知道该长什么样。
3. **字体**：`kBaseFontSize = 18`，中文走系统 CJK 字体。圆体（比如思源黑体
   Light / 更圆的商用字体）在 18px 下容易糊；要做先解决"字体文件随仓库分发"的授权问题。
4. **动画/过渡**：动漫感很大一部分来自"慢一拍的高亮、弹性缩放"。ImGui 自己
   没有动画系统，要加得先有一个 per-widget 的时间源（`EditorApp` 已有 `m_now`）。
5. 浅色档（Sakura Day / Daylight）在`viewport` 3D 区域的对比度还没单独调过。
