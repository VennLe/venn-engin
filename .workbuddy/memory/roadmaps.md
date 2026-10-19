# venn —— 路线图索引（原 `MEMORY.md` 的「路线图」段，2026-10-19 拆出）

## 路线图（权威 → `docs/*_roadmap.md`）
- ⚠⚠ **启动器 → `launcher_roadmap.md`**（10-19 **M0~M5-b + M6 全部落地**，M5-c 未做）。
  四硬事实（读代码得到）：`Editor.exe` **丢弃 argv**（只能 env + CWD）· **CWD 就是隐式工程根**
  （config/imgui.ini/editor_window.ini 全相对 CWD）· `RuntimeConfig::quality` **空挂**而分辨率真生效
  · `engine/` **没有可链接产物**（生成 sln 只能生成编不过的）。
  **验收全绿**：`Launcher.exe --selftest project` = PROJ 43 + RECENT 14 + DISPLAY 23 + CREATE 32
  = **112 条 failures=0**；六个 `verify_launcher_*` 全 PASS；i18n 九组 PASS（词典 **1682 条**、
  重复键 0）；`check_regression_map` PASS。
  数字：`render.json keys=22`（画质全量平铺）· High-vs-Low 像素占比 **5.1212% = 55.1×**
  （基线 0.0929%）· `VKS-RELOAD-KEEP entity=2 path=scripts/main.vks vars=6`。
  五坑 P13~P17/P20/P21：**编辑态脚本不 tick**（热重载只在 Play 态可观测）· `BeginChild` 独立窗口
  让 `InvisibleButton` 收不到 hover（卡片点不动）· `createFromTemplate` 自己追加 recents 被启动器
  内存旧列表覆盖 · **批量删除保护硬杀脚本**（一回合 >50 文件，`ignore_errors` 接不住）→ 用例改成
  **一个文件都不删**（改名让位）· **真实鼠标点击要求窗口在最前**（第一下只"激活窗口"）。
  细则 → `launcher_roadmap.md` §11 台账。
- ⚠⚠ **脚本玩法 → `script_roadmap.md`**：**S0~S6 全 ✅ 收口**（10-19）。验收：
  `--selftest script` **85 条全绿** / `verify_script_editor.py` **21 条** / `verify_script.py` **23 条** /
  `--selftest scriptbench` 200 实例 × 100 语句/帧 = `ms≈92.5`（预算 200，Debug `/Od`）。
  **唯一遗留 = 编辑器 Play 还没接 `ContactTracker`**（`on collide/trigger/exit` 只在 `Game.exe` 派发；
  编辑器里做碰撞玩法要用**距离判定**代替）。
  七坑 P1~P7（S0~S3）：失忆 · 两份分叉的 tick 循环 · `time=t*ts` 跳变 · 错误不熔断 ·
  内建名运行期字符串比较 · 全绑定隐式 self · `frame` 是渲染帧号。
  S4 三坑：先 `++ip` 再压子帧 · `Frame&` 在 `push_back` 后悬垂 · `simFrame` 是 1-based。
  S5 四坑：`sync()` 没人调（编辑态表空）· `reset()` 每帧调 = 暂停即静默重启 · 状态日志块在
  `continue` 之后（`state=Failed` 查无此行）· 一行塞绝对路径把按钮推出窗口（被裁控件仍有 rect）。
  S6 三坑：`on collide` 做不出玩法（无 `ContactTracker` + 运动学体）· `Player` 朝 +X 而相机在 -X 侧
  （摆 -X 的东西全在背后）· `HUD-STATS` 是节流日志要轮询。
- ⚠⚠ **世界规模 → `world_scale_roadmap.md`**（W0~W9）。**W0~W4 全 ✅**；**下一项 = W5 植被**
  （D6 独立顶点布局 + `foliage.vert` 风场 + `FoliageVolume` 撒点）。四条实测：①**物体数不是瓶颈**
  （50/200/500 盒子 60 帧 2.55/2.62/2.60 s，GPU 4.4~4.7 ms）；②Z-up 已解决；③LOD 已有（只缺**蒙皮**
  LOD）；④`Vertex` 只有 64 B → 植被风走**独立顶点布局 + 独立管线**。
- **W4 地形要点**：高度图裸 `.r32`（16 位 PNG 会吃掉 1 cm 判据）；4 层权重打包一张 `RGBA8`；**前向
  Pass 内独立管线**；⚠ set 0 复用主前向**同一份** layout + push 写满 128 B（VUID-08600）；uv **纹素
  中心对齐** `(i+0.5)/N`；LOD 用 **skirt**；`cull=none`；`terrain.frag` 的 CSM 与 `pbr.frag` **逐行
  一致**。物理 `Heightfield` 不改 narrowphase；贴地 = "先问高度场、再问物理体、取更近"。9 坑 → §19.4。
- **渲染** R0~R4·R5-1·R6-1 ✅；未做 R5-2/R5-3/R6-2~R6-5（→ W5~W9）。**编辑器** B·C·E11.1·D·E25·**E26 ✅**，
  下一项 E20/E22。**运行时** M1+M2 ✅。**物理** v1 ✅（仅 Play 态）。
- **E26 画质档位（10-19）**：档位 = 22 个 `*.enabled` + 3 个数值项（`shadow.cascades` /
  `shadow.distance` / `lshadow.maxLights`）。**High 的 distance 必须 = 引擎默认 90**（否则点一下
  High 画面就变，有自检 Q11 盯）。落盘不走 `quality` 标签（值才是权威），面板用 `presetDiff`
  反推当前档；**差异预览与一致性检查必须共用这一个函数**。
- ⚠⚠ **角色链**：**A0 ✅ / A1 ✅（10-19）**，下一项 **A2 状态机 + crossfade**（→A3 层→A4 根运动→A5 事件→A6 IK→A7
  面板）。要点：唯一装配入口 `scene/ModelInstance` · `DrawItem::jointBase` 逐绘制项 · `skinned` 存档块 ·
  ⚠⚠ **两个组件各有一份 time/speed/playing/loop**：没有 `AnimatorComponent` → sk 的是权威（A0 老路径逐位不变）；
  有 → **anim 的是权威**，`AnimatorSystem` 每帧**镜像**回 sk（写入点只有这一处）。Inspector / 控制台
  `anim.time` 都写权威那一侧（写 sk 会被下一帧镜像覆盖 —— 日志 ok=1、画面回到原处）。
  `defaultClip` 只补一次（`appliedDefaultClip` 闸门），否则清空 clipName 看不出静止姿势。
- ⚠⚠ **UI/HUD**：**U0~U8 全 ✅**，仅剩可选 U9。三铁律：①`maxBoundDescriptorSets` 是"同时绑定数"→
  UI 独立 1 set 合法；②⚠⚠ **subpass dependency 参与 pass 兼容性判定** → "一份 pipeline 两模式通用"
  **错**（VUID-02684）；③⚠⚠ 顶点 RGBA8 写 `R|G<<8|B<<16|A<<24`。UI 只插 **Post 输出 pass 内、
  ImGui 之前**。
- ⚠⚠ **i18n 权威 `engine/i18n/ZhTable.cpp`**（**1720 条**，10-19）；`editor::tr` 只转发；英文模式
  `tr()` 在查表**前** return。⚠ 搬词典必须**同步所有 `DICT_SRC`** + **重生成图集**；`MYVK_LANG` 是
  `ALLOW_EXACT` 的例外。⚠ `emplace` = **先到先得**，加词条前先查重（已删过 4 条重复键）。
- ⚠ **存档/引用/Prefab → `persistence_roadmap.md`**（G16~G19，未开工）；下一项 **G16-1** =
  `World::Record.guid`。**前置 G16-5**：`sceneFromJson` 是"解析+GPU 上传"混合体 → 必拆
  `parse/instantiate/resolve`。⚠ **37 用例手写 `"parent": -1`** → parent **不许**改字符串。三决策：
  guid 放 `World::Record` · 存档只**覆盖值不增删组件** · Prefab 覆盖用**显式路径表**。
- ⚠ flaky：`verify_bvh`/`csm`/`ibl`/`particles` 偶发全红（单跑全 PASS）→ **别补跑"凑干净"**。
  ⚠ `FX-FREEZE` 日志 "bit-for-bit" 是**过度承诺** → 下次动 `engine/**` 顺手改。
