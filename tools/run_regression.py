"""跑一轮回归：逐个启动 GUI 验证脚本，记录退出码与结果行。

GUI 测试要独占窗口，只能串行。而且**必须以退出码为准** —— 这批脚本的
结果行格式不统一（`RESULT: PASS` / `全部断言 PASS` / `全部 PASS`），
只靠文本匹配会漏判。失败时把该脚本的日志尾部打出来，省得再跑一遍。

=================================================================
增量：两道闸，**只跑本次改动真正关联到的用例**
=================================================================

全套 39 例要 30 分钟以上，其中绝大多数是几十~两百秒的 GUI 用例。问题是
"改了某一块渲染功能"以后，本来只需要重跑相关的几例。

第一道闸（**用例脚本**）：该用例的 .py + 它递归 import 的本地 tools 模块
没变 → 它的"判定逻辑"没变。

第二道闸（**变更影响分析**）：把仓库里的**源码**拍一份快照存进缓存，下次
运行时 diff 出"自上次回归以来改过哪些文件"，再按下面的表把文件映射成
**关联用例集**；只有落在这个集合里的用例才重跑。

    ┌─ 变更文件 ─┬─ 命中 GLOBAL_GLOBS ──→ 全量重跑（横切，安全兜底）
    │            ├─ 命中 TRIGGERS      ──→ 只重跑它登记的那些用例
    │            ├─ 命中 IGNORE        ──→ 无视（文档/记忆/编译辅助脚本）
    │            └─ 谁都没命中          ──→ ⚠ 全量重跑 + 点名（提示补表）

⚠ **"未登记"一律按全量处理**，绝不静默跳过 —— 这张映射表的价值是"把
  已经确认过归属的文件从全量里摘出来"，不是"猜哪些不重要"。所以它可以
  慢慢补：补得越多越快，没补的部分退化成今天的行为，**不会漏**。

⚠ **横切文件（GLOBAL_GLOBS）一改就全跑**：`CMakeLists.txt`、`engine/rhi/**`、
  `engine/render/Renderer.cpp`（所有 pass 的接线中枢）、`RenderTypes.h`
  （FrameUBO/set0 契约）、`pbr.frag`（前向主着色器）等等。文件粒度的映射
  看不出"Renderer.cpp 里改的是哪一段"，所以只能保守。想在"确实只动了一个
  特性的代码"时跳过这层，用 `--narrow`（见下）。

⚠ **新增文件（`+`）也会参与分析**：没人登记它就是"未登记" → 全跑。所以加
  新的 .cpp/.frag 时顺手补一行 TRIGGERS。

⚠ 快照**只覆盖构建输入**（源码/着色器/模型），**不含运行时数据**：
  `assets/scenes/**`、`assets/scripts/**` 是编辑器运行时自己会写的目录，
  放进快照会导致"每轮都算变了"。

用法：
    python tools/run_regression.py                      # 跑默认全套（只重跑关联用例）
    python tools/run_regression.py --explain            # 只看影响分析，不跑
    python tools/run_regression.py --all                # 无视一切缓存，全跑
    python tools/run_regression.py --narrow             # 横切文件不触发全量（作者显式断言）
    python tools/run_regression.py bvh player add_menu  # 只跑指定的几个（总是跑）
    python tools/run_regression.py --tail 60 bvh        # 只看尾部行数
    python tools/run_regression.py --no-cache           # 不读也不写缓存
"""

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PY = sys.executable
# 单测完整日志的落盘目录（和截图产物放一起）
LOG_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify", "_regression")
CACHE_PATH = os.path.join(LOG_DIR, "_cache.json")
# 产物目录：exe + 被拷过去的 shader/材质/模型/场景
BIN_DIR = os.path.join(ROOT, "build", "bin")
TOOLS_DIR = os.path.join(ROOT, "tools")

DEFAULT = [
    "verify_frustum_cull", "verify_bvh", "verify_lod", "verify_ibl",
    "verify_csm", "verify_local_shadow", "verify_shadow_hardening",
    "verify_volumetric_fog", "verify_light_modifiers",
    "verify_water", "verify_water_motion",
    "verify_template_scene", "verify_gamestart_edit",
    "verify_shader_variants",
    "verify_shader_hotreload", "verify_fxaa", "verify_instancing",
    # R4-1：AlphaMode::Mask（alpha 裁切）+ 抖动裁切
    "verify_alpha_mask",
    # R4-2：预积分 SSS（ShadingModel::Skin，bit4 VENN_SHADING_SKIN）
    "verify_sss",
    # A1：二次元素描（ShadingModel::Toon，bit6 VENN_SHADING_TOON）。
    # 它是"变体系统能不能容纳 NPR"的**活证明**：加一档着色模型 = 加一个
    # 关键字位 + 在白名单里多声明一个模型，不再动 2^N 的组合数。
    "verify_toon_shading",
    # NPR 描边（2026-10-25）：**不是** inverted-hull，而是深度预通道顺带
    # 写一张 G16R16F 的几何缓冲（世界法线 + 线性距离），post.frag 做
    # 8 邻域的深度/法线双判据边缘检测。五轮 Game.exe 截图：
    #   ① 关描边 ×2 逐像素相同（确定性）② 强度=0 与关描边逐像素相同
    #   ③ 开描边差异 ≥5000 px 且变暗 ≥40/255 ④ 天空新增暗像素恰好 0
    #   ⑤ 线宽 1.5→3 覆盖面积 +30% 以上。
    # 第 ② 条与第 ④ 条是最容易写错的两处：前者是"mix 权重 0"，后者是
    # "a=0 的哨兵被当成距离 0 的物体"（症状：整片天空被描成黑边）。
    "verify_outline",
    # P0-2/B：材质渲染顺序（renderQueue / sortOrder / 覆盖带 bit7）。
    # 用两颗球验证"谁画在上面"：跨带时距离完全不参与，>=3000 进覆盖带
    # （不写深度 + 深度测试恒通过）。这是"眼睛能压在脸皮和刘海之上"
    # 唯一能做对的方式。
    "verify_render_queue",
    # 材质页 32B → 96B 的四个 A 类语义槽（2026-10-25）：ramp / matcap /
    # SDF 阴影 / detail。八轮 Editor.exe 截图，只换材质不换机位，
    # 判据分两类：
    #   · **恒等类（恰好 0）**：ramp 白图 ×1、detail 白图强度 0.5、
    #     matcap 黑图强度 0.9 —— 三条都真的进了采样分支，所以它们同时
    #     证明"路径通"和"数学上是恒等"。这是"关 = 基线"在这一层的可断言形式。
    #   · **行为类**：中灰 ramp 变暗、全黑 ramp 方差塌掉、matcap 白变亮、
    #     detail 棋盘改变画面。
    # ⚠ 必须显式 MYVK_BINDLESS=1：页表只在 bindless 路径存在（§8.2 的
    #   结构事实），脚本自己断言了 Bindless: ready，否则失败信息会误导。
    "verify_material_page",
    # Inspector 左侧图标导航条 + 按页签分区（2026-10-16 的 UI 重构）。
    # 放在这里跑是有讲究的：它逐个页签启动 16 次编辑器，是本套件里最慢的
    # 一个；而它同时**看守着其它脚本的默认页签**（自动化默认 = all），
    # 所以失败信息应该尽量早出现。
    "verify_inspector_tabs",
    "verify_nav_gizmo",
    "verify_character_window", "verify_add_menu", "verify_mesh_params",
    "verify_collider_gizmo", "verify_player", "verify_collision_play",
    # 第三人称相机：俯仰范围（仰 55/俯 70）+ 弹簧臂球扫掠 + 地面钳制。
    # 它看守的三条断言都在**两个**二进制里（Game.exe 的两个无窗口自检 +
    # 编辑器 Play 的贴墙场景），run_regression 只跑 Python 脚本，所以
    # 自检由脚本自己 subprocess 拉起 —— 不需要额外登记自检用例。
    "verify_third_person_camera",
    "verify_drop_to_floor", "verify_material_roundtrip",
    # 材质槽小方块（空态是小方框 / 绑定后变宽并显示文件名 / 拖拽真的生效）。
    # ⚠ 它此前**不在** DEFAULT 里 → 2026-10-16 那种"槽位被图标条挤到 48px、
    #   文件名被裁成 .png"就没人拦（verify_material_roundtrip 是顺手抓到的）。
    "verify_material_chips",
    "verify_primitive_roundtrip", "verify_screenspace", "verify_textures",
    # 多相机 / 通用 RenderTarget + 相机特效（DoF / 运动模糊 / 色差 / 暗角 /
    # 后处理体积）
    "verify_camfx",
    "verify_gpu_timing",
    "verify_viewmodes",
    "verify_taa",
    "verify_auto_exposure",
    "verify_color_lut",
    "verify_drs",
    # Hi-Z 遮挡剔除（R2-1）：深度金字塔 + AABB 遮挡测试 + 两帧延迟回读
    "verify_hiz",
    # Multi-draw indirect + GPU 紧凑化（R2-2）：间接命令 + GPU 写 draw 参数
    "verify_mdi",
    # Bindless / descriptor indexing（R2-3）：纹理数组 + 材质页，per-draw
    # 只 push 页号、零描述符重绑（开关都必须逐像素回到基线）
    "verify_bindless",
    # POM 视差遮挡贴图（R4-3）：heightMap 槽 + 切线空间层步进 ray-march，
    # bit5 VENN_PARALLAX（heightScale=0 / 全局开关 = 逐像素回到基线）
    "verify_pom",
    # 延迟贴花 / DBuffer（R4-4）：深度预通道 → 盒体 pass 写三张 DBuffer
    # （反照率 / 世界法线 / 粗糙度-金属度）→ 前向按 coverage 混合。
    # MYVK_DECALS 默认关；关 / enabled=false / opacity=0 / 盒体悬空
    # 四条都必须逐像素回到基线。
    "verify_decals",
    # 反射探针（R5-1）：探针盒 → 6 面相机捕获 → 逐面拷进立方体 → GGX 预过滤
    # （roughness = mip）→ 片元落在盒内时替换 IBL 的预过滤项。MYVK_PROBES
    # 默认关；关 / enabled=false 两条都必须逐像素回到基线；场地是"封闭屋子 +
    # 镜面球 + 相机背后的自发光面板"，球心那个像素直接读"探针有没有烘进
    # 相机看不到的那面墙"。
    "verify_reflection_probe",
    # GPU 粒子系统（R6-1）：compute 模拟（spawn/kill/固定步长积分/曲线/噪声）
    # + 计数与可选桶排序 + 两个 indirect draw（加色 / Alpha）+ 软粒子。
    # MYVK_FX 默认关；关 / 开了但无发射器两条都必须逐像素回到基线；
    # 冻结帧（MYVK_FX_FREEZE）同配置两次逐像素一致是"确定性"的硬证据；
    # 面板结构与 12 个模块的字段清单走 FX-PARAMS 断言。
    "verify_particles",
    # ---- 阶段 C：核心面板（E11 / E13 / E14）----
    # E11 Inspector：分区折叠**跨实体**保持 / 组件搜索过滤 / Copy-Paste-Reset
    # 往返（POD 字节快照经可撤销命令写回）
    "verify_inspector",
    # E13 ContentBrowser：类型过滤矩阵 / 列表与四档缩略图尺寸 / 拖资产到
    # 视口（贴地）与 Hierarchy（实例化）两条落点
    "verify_content_browser",
    # 2026-10-17 布局改造：Content 从常驻分栏改成"底栏抽屉"。
    # 看守对象：底栏（通栏/32px/三个带 Logo 的按钮/右侧资源统计）、
    # 抽屉开合（点按钮 / 再点关 / 点外部关）、Dock in layout（停靠后
    # 视口变矮 ≈ 抽屉高、点外部不关）、上边缘拖拽改高度。
    "verify_bottom_bar",
    # 2026-10-17 晚：Content 三栏改造（树 | 类型单选 | 内容+搜索+排序）。
    # 跟着 bottom_bar 一起跑：两者都在同一个抽屉里，改一边很容易碰另一边。
    "verify_content_three_pane",
    # 2026-10-17 晚（第二条需求）：Content 右栏顶部的动作条 ——
    # Add（图标+文字，菜单**向上**弹，13 种资源类型 + 导入）/ Save All /
    # 上一层·下一层 / 路径框（相对项目根）。与三栏同处一个抽屉，一起跑。
    "verify_content_action_row",
    # 2026-10-17 晚（用户两条需求）：①3D 视口工具条重做（单行图标 + 九个
    # 下拉：工具/坐标/四个吸附/视口看法），②底栏 CMD 右侧的 + 面板托盘
    # （向上弹的勾选列表、带 logo 的面板按钮、按钮可收起、× 移除按钮、
    # 右键最小化）。顺带覆盖两张新面板（Physics / Particles）。
    "verify_viewport_bar",
    # E14 Viewport：四角浮层（模式条 / 统计 HUD / 相机信息 + 9 书签 / 导航球）、
    # NO_HUD 两次逐像素一致 + 与改造前参考图一致、四浮层全关 == NO_HUD、
    # Shift+数字 快捷键、Viewport Display 右键菜单显隐、偏好落盘跨进程往返
    "verify_viewport",
    # ---- 阶段 D：交互理念 ----
    # E18 命令注册表 + 命令面板：注册表规模 / Ctrl+Shift+P 打开 /
    # 模糊搜索（query=ssr 榜首是 feature.toggle.ssr）/ 执行真生效
    # （CMD-RUN + FTR-UI）/ 无匹配给"最接近"建议
    "verify_command_palette",
    # E19 撤销覆盖设置类改动：改一次进历史 / 500ms 合并（depthDelta=0）/
    # undo 回旧值 redo 回新值 / Float 与 Bool 两类 kind 都接进来了
    "verify_undo_settings",
    # E21 通知 / 进度：注入 ERROR → 状态栏 Error 计数 + Console 能定位（点
    # toast → NOTIF-CLICK/NOTIF-FOCUS）/ 进度任务跑完与取消 / Info 不自动升级
    "verify_notifications",
    # ---- 阶段 E：生产力 ----
    # E25 GPU 时戳时间轴：时间轴总长 == GPU-TIMING 帧总耗时（< 5%）/ 60 帧
    # 历史封顶 / 点段联动 Console 过滤 / 归入 Rendering 工作区 / 采样纯读
    "verify_gpu_timeline",
    # ---- 角色表现链 A0（2026-10-18）----
    # 骨骼动画的无窗口数值自检：时间推进 / 循环回绕 / 采样拆分后的**逐位
    # 不变性**（对照一份拆分前的参考实现）/ 混合端点短路 / 拓扑序。
    # 不开 Vulkan、不开窗口，整条几百毫秒 —— 放在这里是为了让"动画算错了"
    # 在套件前排就红，而不是等到半小时后那条 GUI 用例。
    "verify_anim_runtime",
    # 端到端一条因果链（六组）：导入建组件 → 面板有东西 → 存盘/读档往返
    # → Play 态时间推进 → 像素上真的在动 → **多实例 jointBase 两两不等**。
    # 最后一组是 A0-4 的否定测试：退回"按 Mesh* 共享 base"的旧实现时必红。
    # ⚠ 它是 GUI 用例（约 60 秒，7 次启动），不能与其它 GUI 用例并行。
    "verify_character_import",
    # ---- 物理（R7-1）：完整刚体核心 ----    # 不需要窗口、不需要 Vulkan：MYVK_PHYS_TEST=1 让 Editor 在 app.run() 前
    # 直接跑物理自检。**本套件里唯一能做逐位可复现断言**的用例（跨进程比对
    # PHYS-TEST 行），也是物理唯一"机器可判定"的正确性证据 —— 一叠箱子塌掉
    # 有一百种原因，浮点逐位一致只有一种解释。
    "verify_physics",
    # ---- 地形成为**真物理地面**（2026-10-22 第十轮）----
    # 起一个真工程跑 360 帧，断言：出生点地表高度 == 0（GameStart 不陷入）/
    # 玩家脚底贴地 / 平地与斜坡上的箱子都停在"地表 + 半高"而不是沉进去。
    # ⚠ 它会自己用 Python 重算双线性 `height_at()` 当**独立真值** —— 不读
    #   引擎的中间结果，否则就是拿被测对象的输出证明它对。
    "verify_terrain_physics",
    # ---- 多块地形不再撞描述符池上限（2026-10-22）----
    # 用户报障：`VK_ERROR_OUT_OF_POOL_MEMORY` at DescriptorSet.inl:61 +
    # `3 leaked objects`。根因是 TerrainPass 的池按 4 块地形写死。
    # 用例：6 块地形的场景跑 120 帧，断言零 VK_CHECK / 零泄漏 / 走了扩容。
    # ⚠ T4「必须看到 DESCRIPTOR-POOL 扩容」是**前提断言**：没有它的话，
    #   万一哪天首池被调大到 >= 6 块，T2/T3 会"因为没走到扩容"而假绿。
    "verify_multi_terrain",
    # ---- 抬山保存后地形变砖 / DEVICE_LOST（2026-10-22 第十一轮）----
    # 用户报障：`TERRAIN-INSTANCE-FAIL reason=height_not_normalized
    # min=0.000 max=8.000` + `VK_CHECK -4 at vkQueueSubmit`。
    # 根因三层：① 抬升笔刷允许 kMaxNorm=8，而 `.r32` 契约是 [0,1]，
    # 写端原样落盘 → 工程再也打不开；② syncTerrains 拿"成功数"比"请求数"，
    # 建失败就每帧全量重建 → 帧时间冲破 TDR 2 秒 → DEVICE_LOST；
    # ③ 存量坏资产无法自愈（读端硬拒）。
    # 用例三组：干净模板（不许误伤）/ 高度×8（必须救援）/ 尺寸砍半
    # （失败不许逐帧刷屏）。
    "verify_terrain_renorm",
    # ---- 自研二进制资产链（阶段六/七，2026-10-22）----
    # FBX → `AssetC` → `.vmesh` → 运行时加载器，整条离线链路 + 引擎内自检。
    # 判据分四组：离线编译（顶点/索引守恒、焊接、可复现）/ 工具自洽（开关
    # 真的生效、两次编译逐字节相同）/ 运行时（真的走 `.vmesh`、缓存缺失与
    # 过期都回退 FBX、`.vmesh` 更快）/ 引擎内 `MYVK_ASSET_TEST` 自检。
    # ⚠ **刻意不做截图比对**：autoPlay 模型的截图取决于 wall clock，比对会
    #   变成 flaky 用例；这条链的正确性由数值判据守。
    "verify_vmesh",
    # 顶点格式 80 → 112 B（§13，2026-10-25）：uv2 / morphBase / tangent
    # 三个通道落进 stride，老六个属性的偏移一个都没动。
    # 判据**全部是字节级**（A 文件头/B 结构不变量/C 确定性/D 运行期统计
    # 与文件解析互校/E 老 80 字节逐字节未变）—— 这一刀没有截图判据，
    # 理由写在脚本头：没有任何着色器读新通道、老字节也没变，渲染结果
    # 必然一样，一张"必然一样"的图证明不了什么。
    # ⚠ E 组依赖 `_venn_verify/vmesh_bak`（手术前的一次性备份）。
    #   没有备份时整组 SKIP 而不是红 —— 判据依赖临时产物就必须能自己退场，
    #   否则它会在任何新机器上永远红，然后被人删掉。
    "verify_vertex_format",
    # morph 数据链（§14，2026-10-25）：FBX blend shape → MORPH 区块 →
    # 顶点里的 morphBase/morphCount → 运行期 Mesh。
    # 与上一刀（verify_vertex_format）的分工：那一刀验**格式**（112 B、
    # 偏移没动、老 80 字节零变更），这一刀验**数据**（delta 是否与顶点
    # 对齐、焊接换下标后有没有跟着 remap、target 下标有没有在子网格间串）。
    # 判据同样是字节级：C 组在 Python 里独立实现了一遍 C++ 读端的同一道
    # 自洽校验 —— 第一版就因为漏加区块基准偏移而三项全红，而 C++ 是绿的，
    # 这正是"两条独立实现互校"要抓的东西。
    "verify_morph",
    # GPU 形变（§15，2026-10-25）：**像素级**。上面那条验到"数据是对的"
    # 为止，这一条回答"顶点着色器真的读了吗"。
    # 四次启动只有 console 命令不同（不设 / 设 0 / 设 1 / 设一个不存在的
    # 名字），判据 = 三张图逐像素比 + MORPH-FRAME 的槽位转储。
    # ⚠ 必须进 DEFAULT：它守着一个真实发生过的 bug —— `MorphRig::evaluate`
    #   把"多个材质桶"当成"in-between 关键帧链"，一个滑条只驱动一块脸，
    #   而数据层 34 项、日志、运行期计数**全绿**。只有像素能看出来。
    "verify_morph_gpu",
    # 两骨 IK + 挂点（§16，2026-10-25 第八刀）：**像素级**。
    # 上面两条验的是"形变数据"和"GPU 真的读了"，这一条验的是**逆运动学**
    # —— 从目标点反推关节旋转。
    # 四次启动只有 console 命令不同（不发 / weight 0 / 可达目标 / 够不着），
    # 判据 = 三张图逐像素比 + `ANIM-IK` 与 `IK-SET` 两行锚点。
    # ⚠ 必须进 DEFAULT：它守着三个**只有像素能看见**的失效模式：
    #   ① 求解器算对了但没写回 pose（`poseToJoints` 的顺序 / 覆盖问题）
    #      → 日志里 `actual` 全是 0.0000，画面纹丝不动；
    #   ② weight=0 时"接近不动"而不是"完全不动"（差几个像素，肉眼绝对看不出，
    #      但会让 IK 变成一盏永远漏电的灯）→ K1 逐像素等于没开；
    #   ③ 够不着时假装成功（腿伸直了却报 actual≈0）→ K5 要求 miss=1 且
    #      actual > 0.05。真实渲染里这一条最阴：角色看着没够到，日志说够到了。
    # ⚠ 它自己的判据也踩过一次单位坑（`diff_stats` 返回占比不是百分数），
    #   细节写在脚本头 —— 那次让 K2b 变成恒真，等于该判据根本不存在。
    "verify_ik",
    # ---- 2026-10-17：主题 / 语言 / 默认值 / 胶囊线框 / 启动露面 ----
    # 五套主题 + 中英映射替换 + 偏好存档 schema 门。判据只取 Preferences
    # 窗口内部的平均亮度（整窗均值会被 3D 视口拽平，第一版就栽在这）。
    "verify_theme_language",
    # 出厂默认值：吸附全开 / 栅格 1m 且放大 / 存档仍能覆盖默认。
    # 判据 = onInit 末尾那行 EDITOR-STATE（结构化，不数像素里的对勾）。
    "verify_editor_defaults",
    # 胶囊线框的**形状规范性**：正对投影下量轮廓的宽高比 / 对称 / 桶身直段 /
    # 球冠收窄；副用例把"对 1m 立方体拟合出来的其实是球（halfHeight=0）"
    # 钉成事实。
    "verify_capsule_shape",
    # 启动不闪白 + 缓冲两秒才露面：断言"首次可见时刻"落在 1.6~4.5s、露面后
    # 连拍无白帧。它需要 MYVK_STARTUP_DELAY=1 才跑得出缓冲（MYVK_NO_WINDOW_SAVE
    # 会顺带跳过缓冲，而那个变量又必须设 —— 否则退出时会覆盖用户的 imgui.ini）。
    # ⚠ 这一例**故意不登记 TRIGGERS**：它依赖的全是 engine/core/**（= GLOBAL），
    #   登记了反而命中"全横切登记"检查（--narrow 下会假跳过）。
    "verify_startup_show",
    # Hierarchy 行尾 🔒/👁 两个图标按钮（E12 的修补）。
    "verify_hierarchy_status",
    # ---- 阶段 M1：运行时骨架（G01~G05，2026-10-17）----
    # G01+G02 结构验证：两个 exe 并存 / VennEngine 里零 editor+runtime 源文件 /
    # 两个 exe 都链接同一个 VennEngine.lib / 脚本引擎已在 engine/script 且
    # 重命名历史可追。**不起窗口**，读 build.ninja 与文件树，毫秒级 ——
    # 它看守的那些病（引擎里 include 了 editor、另编一份源码冒充共享）不会让
    # 任何一条 GUI 用例变红，所以必须有这么一条"只看结构"的用例。
    "verify_runtime_build",
    # G03 定步长调度：自检三条（300 帧恰好 300 步 / 累加器与 stepFixed 逐位
    # 相同 / timeScale=0 零漂移）+ 真实主循环的 GAME-LOOP 字段语义 +
    # **编辑器 Play 与 Game.exe 的 PHYS-BODY 逐字符相同**（后者是"出包手感
    # 不漂移"的唯一机器证据）。
    "verify_runtime_loop",
    # G04+G05 状态栈与启动配置：迁移序列逐字（含 Paused 的阻断语义）/
    # 清空全部 MYVK_* 仍按 assets/game.json 起 / CLI>env>json 优先级 /
    # 日志落在 %LOCALAPPDATA% 且 --help 不建日志。
    "verify_runtime_states",
    # ---- 阶段 M2：手感地基（G06~G10，2026-10-17）----
    # 四组互相独立的面（缺一条就有洞）：
    #   A 五项无窗口自检（输入 / 角色 / 接触 / 相机 / 玩家）—— A 组证明算法对；
    #   B 场景读盘锚点（triggers=1 materials=1 / PLAYER attached / CAM-ACTIVE
    #     优先级 / fov 取自组件）—— 证明**字段真的从 JSON 读进来了**；
    #   C 读→写→再读往返（`--dump-scene`）—— `saveScene` 与 `loadScene` 是
    #     两套代码，只测读的话"存档丢字段"永远暴露不出来；
    #   D 老场景 sandbox 基线不变（PLAYER none / 零 CAM-ACTIVE / triggers=0）。
    # 全部无窗口，整条跑完约 3 秒。
    "verify_runtime_m2",
    # ---- 阶段 A · E03：设置层级与持久化（2026-10-17）----
    # 三条硬约束一起看守：①无 config/render.json 时**一条 RENDER-SET 日志
    # 都没有**（= 与 E03 之前逐像素一致）；②合法 json 真的落到渲染
    # （SHADOW-DBG cascades=2），且与 env 路径的 splits 逐字符相同
    # （= 文件层不是影子结构体）；③非法 json 只 WARN + 回落默认 + 不崩。
    # A 组是无窗口自检（毫秒级），B~G 组跑 7 次编辑器（约 35s）。
    "verify_settings_layers",
    # ---- G06 键位重绑定（M2 的最后一件，2026-10-17）----
    # 守的是"改了能存、存了能被游戏读到、读到了能撤销"这一整条链 ——
    # 它坏了不会花屏，只会让玩家下次进游戏键位又变回老样子：
    #   A 引擎自检（无窗口，H 组点名断言改绑/冲突/重置/清空）；
    #   B 运行时无文件 → 出厂绑定；C 编辑器改绑+存盘 → JSON 逐字段；
    #   D 运行时读**同一份**文件 → Jump 真的变成 F（且别的动作没丢）；
    #   E 编辑器再读回来；F 改绑 → undo → 存盘，文件退回出厂值；
    #   G Preferences ▸ Bindings 页的矩形锚点（UI 面真的存在）；
    #   H 冲突检测（把 Jump 改到被 W 占用的键）；
    #   I 自动化纪律（MYVK_FRAMES 下默认路径既不读也不写）。
    # 9 次启动、每次几帧，整条约 25 秒。
    "verify_input_bindings",
    # ---- 游戏 UI / HUD（U0~U8，2026-10-18）----
    # 三层：① 无窗口数值自检（`MYVK_UI2_TEST=1`，毫秒级）；② `UI2-CAPS`
    # 能力量测（把"maxBoundDescriptorSets 是同时绑定数不是总数"钉在实测值上）；
    # ③ 像素守卫（`ui.enabled=1` 但没有 UI 数据 → 与关掉整条通道逐位相同）。
    # ⚠ 它是 GUI 用例（2 次启动 + 1 次自检），不能与其它 GUI 用例并行。
    "verify_ui2",
    # ---- 中文化覆盖率（2026-10-18）----
    # 纯静态扫描，不开窗口、不开 Vulkan、几百毫秒。八组体检：
    #   A1/A2/A3 参数 label·tooltip·特性名、A4 Combo 选项、
    #   B 未包 TR 的界面字面量、B2 包了 TR 但词典缺、C 词典孤儿与重复键、
    #   D snprintf 格式串、E 控件显示格式串。
    # ⚠ 为什么必须进回归：缺译文**不会报错、不会崩**，只会静静地少翻一处。
    #   本项目已经栽过三次盲区（裸 snprintf 的 "tasks %d"、SliderFloat 的
    #   "%.1f deg"、辅助函数内部的 label），全都是"肉眼不看就永远发现不了"。
    #   放进套件前排没意义（它跟其它用例无依赖），放尾部是为了不打乱既有顺序。
    "verify_i18n_coverage",
    # ---- HUD / 暂停菜单 / 焦点栈（U6 + U8，2026-10-18）----
    # 四层：A 无窗口数值自检（`--selftest hud`：血量追随/残影/伤害数字寿命/
    # 对象池/血条几何/世界→屏幕 Y 方向/`config/ui.json` 往返）；
    # B 上屏几何与准星否定测试（含对照区，防"两张图整体不一样"的假绿）；
    # C 暂停菜单 + 焦点环 + **暂停时按 W 位置逐位不变**（U6 的验收条款）；
    # D 夹具脚本 `damage(30)` → 血条 70% + 伤害数字出现后消失。
    # ⚠ GUI 用例（2 次启动），不能与其它 GUI 用例并行。
    "verify_hud",
    # ---- G14 音频（G14-0~G14-9，2026-10-18）----
    # 六组互相独立的面，缺一组就有洞：
    #   A 无窗口自检（`Game.exe --selftest audio`，118 条断言）—— 解码 /
    #     声部池 / 总线 / ducking / 3D 空间化 / 流式全在离线渲染里判，
    #     **不需要声卡也不需要窗口**（§3.1 的前提，也是整条路线图能被验证
    #     的根本原因）；
    #   B `AUDIO-CONFIG` 那一行 —— 音频的失败模式几乎全在"配了但没生效"上；
    #   C 设备后端四态（null / wasapi / 假名回落 / `__none__` 干净失败）；
    #   D `saveScene`→`loadScene` 往返（只测读的话"存档丢字段"永远暴露不出来）
    #     + 老场景对照；
    #   E 编辑器 UI 面（`INS-AUDIO-HEADER` / `INS-AUDIO-PREVIEW` 矩形）+ 试听链
    #     + **Play 副本里的组件真的被消费**；
    #   F 脚本内置 `playSound` / `setBusVolume` / `stopAllSounds`。
    # ⚠ GUI 用例（3 次启动编辑器），不能与其它 GUI 用例并行。
    # ⚠ 它同时会给 build/bin/assets/audio 造一个 105 MB 的 10 分钟夹具
    #   （不提交仓库，见 .gitignore）；`--skip-long` 可跳过那组对照。
    "verify_audio",

    # W0 + W1（docs/world_scale_roadmap.md）：材质身份 / 实例 / 覆盖项。
    #   A `--selftest material`（无窗口无设备，15 条纯数据断言）；
    #   B 场景往返：同名材质不再静默合并 / 材质 id 是 `模型#材质名` /
    #     导入模型的覆盖能存能读 / 找不到 id 时**不能变 null** /
    #     实例只写被覆盖的字段；
    #   C 编辑器 UI：`INS-MAT-INSTANCE` / `INS-MAT-MAKEUNIQUE` + 真的点一下。
    # ⚠ 它也是 GUI 用例（1 次启动编辑器），不能与其它 GUI 用例并行。
    "verify_material_instances",

    # W2-2（docs/world_scale_roadmap.md §7）：导入设置 sidecar `.vmeta`。
    #   A 首次导入写出映射（7 条 = 模型路径#材质名）
    #   B 源没变 → changed=0 且**不写盘**（mtime 逐位不变）
    #   C 手改映射 → 材质 id 跟着改（这条是 W2-2 的目的）
    #   D 源变了 → changed=1 + ASSET-MAT-REMAP-FAIL + 陈旧映射被清掉
    #   E 改 import 参数 → forced=1
    #   F import.scale 真的缩了模型（读 FBX 的归一化半径，不开窗口）
    "verify_import_vmeta",

    # W3（docs/world_scale_roadmap.md §8）：第一个真实关卡。
    #   373 个实体、全部引用 8 个 `.vmat`。
    #   A 加载 / B 往返逐字段相等 / C 性能（GPU + 增量）/ D 改母材质全变
    #   E 关太阳后室内靠点光可见 / F 单件覆盖往返。
    # ⚠ 它也是 GUI 用例（3 次开窗口截图），不能与其它 GUI 用例并行。
    "verify_first_level",

    # W4（docs/world_scale_roadmap.md §9）：地形。
    #   A 组装（资产/4 层/cull=none/64 块）/ B 场景往返 / C LOD 按距离选
    #   D 改层材质像素断言 / E 关掉地形 / E' 渲染层整条跳过 / F GPU 帧时
    #   G `--selftest terrain` 27 条。
    # ⚠ GUI 用例（4 次开窗口抓图），不能与其它 GUI 用例并行。
    "verify_terrain",
    # W4-4：地形编辑器面 —— ContentBrowser 双击 `.vterrain` + Inspector
    #   Terrain 段。A 添加 / B 不叠第二份 / C 段内控件 / D 关可见像素
    #   E Ctrl+S 往返 / F 重开自动恢复。
    # ⚠ 也是 GUI 用例（真鼠标双击 + 前台 Ctrl+S）。
    "verify_terrain_editor",

    # S5（docs/script_roadmap.md §7）：脚本的**编辑器面**。
    #   脚本从"只能翻日志"变成"面板里看得见、点得到"：
    #   F1 Instances 页签 + 3 个实例锚点 / F2 Idle→Running（**文本 + 像素**
    #   双证，四种状态四种颜色就是为这条存在的）/ F3 只改一个数字的热重载
    #   保住变量（`VKS-RELOAD-KEEP`）/ F4 面板 Reset → `vars_after=0`
    #   F5 死循环 → `Failed` 红字 → Reset 解除再熔断 / F6 控制台
    #   `script.vars Boss` 打印 8 行键值。
    # ⚠ GUI 用例（2 次启动编辑器），不能与其它 GUI 用例并行。
    "verify_script_editor",

    # S6（docs/script_roadmap.md §7）：**示例关卡的端到端玩法**，
    # 也是 §8 那条总验收的机器化版本 —— 拿一个关卡、玩法全写在 `.vks` 里、
    # 一行 C++ 都不改就能玩。
    #   G1 三个巡逻兵在动（像素 + 天空对照区）/ G2 撞人扣血（HUD-STATS）
    #   G3 门按**时刻**开合（Δt=2.0000s）/ G4 5 个拾取物 → worldAttr=5
    #   G5 存盘往返（世界黑板保住、逐实体实例变量清零）
    #   G6 改一个数字热重载（不重启、位置连续）/ G7 scriptbench 帧时间预算
    # ⚠ 五次启动 Game.exe（含一次 12 秒长跑），不能与其它 GUI 用例并行。
    "verify_script",

    # ---- 引擎启动器（docs/launcher_roadmap.md，2026-10-19）----
    # 六个用例各管一层，缺一层就有一整类"静默坏掉"没人看：
    #   project  M0：工程能被**手动**打开（cwd=工程根 / 布局落在工程里）
    #   ui       M1：启动器窗口的排版（header / 两栏 / 最近列表 / 底栏）
    #   create   M2：四步向导 → 真建一个工程（16 条断言 + 截图复核）
    #   open     M3：拉起编辑器（`CreateProcessW` + 自建 env 块 + 启动器自退）
    #   display  M4：分辨率 / 画质预设真的落到 `render.json`（像素双证）
    #   code     M5：外置代码根 = **目录联接**（改外部文件 → 工程里跟着变）
    # ⚠ 除 `project` 全是 GUI 用例（各开 1~2 次窗口），**不能并行**。
    # ⚠ `project` 是唯一一个"必须让编辑器真的写布局存档"的用例 ——
    #   任何自动化帧控（`MYVK_FRAMES`）都会让它永远假绿，它自己刻意不设。
    "verify_launcher_project",
    "verify_launcher_ui",
    "verify_launcher_create",
    "verify_launcher_open",
    "verify_launcher_display",
    "verify_launcher_code",

    # E26（docs/editor_roadmap.md §6）：画质档位。
    #   19 条断言：A 组 `--selftest quality` 15 条纯数据；B 组真点
    #   Low/Epic → `QUALITY-SNAPSHOT` 断言 ssao/ssr/taa/cascades 的具体值、
    #   应用后自检 mismatches=0、落盘 25 个键；C 组重开工程读回同一档。
    #   **验收原文**："切到 Low 后断言 SSR 生效值为 0；切回 Cinematic 为 1"
    #   —— 这里用 Low↔Epic 达到同一条判据（Cinematic 只多一个 dof）。
    # ⚠ GUI 用例（2 次启动编辑器），不能与其它 GUI 用例并行。
    "verify_quality_presets",

    # ---- 外部模型进场景：尺度 / 第三人称 / 绑骨 / 动画（2026-10-22）----
    # 四件事各自一条，缺一条就有一整类"看起来能动、其实不对"没人看：
    #   scale    第三方 FBX 进场景后**真实尺寸**对不对（1:1，不是"看着差不多"）
    #   player   第三人称绑定：从出生点造出的玩家真的挂上了模型（脚底对齐 /
    #            胶囊跟身高 / 关节矩阵进 SSBO / 按键真的走得动）
    #   autorig  剥掉原厂 52 骨 → 一键自动绑骨 → 与厂商骨架**逐根比位置**
    #            （这是"通用算法能绑"的唯一机器证据，肉眼看不出 3 cm 的偏差）
    #   procanim 四条基础动作 idle/walk/run/jump：离线数值（步幅/抬脚/摆动相
    #            前向速度/手臂横向）+ 运行时四态真的切得动
    # ⚠ 后三条都会起 Game.exe（各 1 次，每次约 3 秒），不能与其它 GUI 用例并行。
    # ⚠ procanim 的 B 组要按 Shift —— **修饰键必须用真实键事件**，理由见脚本里
    #   `modifier()` 的注释（PostMessage 会被 GLFW 的防粘键补丁当场撤销）。
    "verify_model_scale",
    "verify_player_character",
    "verify_autorig",
    "verify_procanim",

    # ---- 角色预览不能拆掉主场景的地形（2026-10-22，编辑器闪退）----
    # 现场：新建 third_person 工程（场景里有 Terrain）→ Shift+A ▸ Player ▸
    #   Third-Person Player → 角色窗口打开的那一帧 `invalid state` 刷屏，
    #   最后 `vkQueueSubmit` 返回 -4 + `Fatal: Vulkan call failed` 直接退出。
    # 根因：`uploadSceneState()` 无条件 syncTerrains/syncWaters，而离屏预览
    #   是**录制途中**带着另一个（没有地形的）场景调进来的 → 判成"集合变了"
    #   → 在录制中间 clearInstances()，销毁了已绑在命令缓冲上的地形 buffer 与
    #   层贴图 sampler。修法：只有主场景（offscreenSlot < 0）才对这两个同步。
    # ⚠ 这条用例**必须带地形**：空场景下主/预览两边 items 都是空，判据
    #   `same` 恒真，再坏也抓不到（别把场景简化掉）。
    "verify_preview_terrain",

    # ---- 角色默认模型 + 移动/动画/按键映射（P4，2026-10-22）----
    # 用户要求：①新建角色就自带模型/骨骼/动画；②WASD 走起来要**播 Walk**；
    # ③静止满 8 秒触发 idle，之后每 8 秒继续；④任意 clip 可在角色面板绑键
    # 触发；⑤角色面板就在视口标签栏。
    # C0~C10 共 15 条断言，每条都能独立变红（第一轮实测 8 条红）。其中
    # C6（走路截图）是**唯一不靠日志**的判据：早先版本用阻塞式 hold_key，
    # 返回时键已松、人已回 idle —— 名字叫 walk 的截图里其实站着不动。
    # C9 还顺带守 `animKeys` 的存读闭环（Play 副本是重新反序列化的）。
    "verify_char_locomotion",

    # ---- 运行时"手感四件事"（P5，2026-10-22）----
    # 用户报障四条：①脚陷地 + 出生点不是 GameStart；②必须点视口才能操作、
    # 操作时隐藏鼠标、Esc 退出；③移动没朝视线方向（**最关键**）；④状态切换
    # 生硬要平滑过渡。
    # R0~R9 十条，每条都能独立变红（R2 已做反向验证：把 modelYaw 从 90 改回 0
    # → `spanX/spanY` 整好互换，只有 R2 红、其余 9 条仍绿）。
    # ⚠ R2 刻意用**几何判据**（人形左右跨度 > 前后跨度）而不是截图比对：
    #   像素宽高比实测在 0.367~0.429 之间飘，拿来断言就是 flaky。
    "verify_player_feel",

    # ---- 默认操控 + 真实跳跃（P6，2026-10-22）----
    # 用户要求：WASD 走 / 右键长按跑 / 空格跳，全部**内建**（不需要在角色面板
    # 里绑键）；跳跃动画换成"双脚离地"；高度 / 滞空 / 弧线 / 空中方向控制都要
    # 符合物理直觉。
    # J0~J9 十条，每条能独立变红。J2/J3 是**数值**判据（0.55~0.90 m、
    # 0.60~0.95 s），不是"看着像"；J5 用轨迹单调性证明是抛物线而不是三段折线；
    # J6 证明空气控制真的生效（落点横向位移 > 0.4 m）。
    "verify_player_jump",

    # ---- 脚踩地形表面，不许陷进去（P6，2026-10-22）----
    # 用户原话："角色在踩在地形的地面时，应该是脚部踩在上面的，不允许脚底
    # 踩到地形地面的下方。"
    # G0~G7 八条。**必须带地形**（terrain/hills），空场景 + groundPlane 再坏
    # 也抓不到 —— 这条用例第一次跑就抓到两个真 bug：
    #   ① 一帧竖直位移（0.078 m）比接触窗口（±0.02 m）还宽 → 快速下落整帧
    #      跨过地面 → 一路穿到 -277 m；
    #   ② 跑上坡时水平一帧走 0.15 m，坡面升起量同样超过窗口 → ground 翻 0，
    #      随后永远贴不回去。
    # 两者都改成"按这一帧可能跨过的竖直距离给容差"（sweep）才修掉。
    "verify_player_ground",

    # ---- 角色面板预览视口（P6，2026-10-22）----
    # 用户要求：预览要真的能**看动画播放**；视口要有栅格地面 + gizmo；胶囊
    # 要看得见、能用鼠标选中，并能分别调角色/胶囊的位置・旋转・大小。
    # P0~P8 十二条。P3 是**唯一不靠日志**的判据：隔 0.45 s 截两张，预览矩形
    # 内像素差异 ≥ 0.4%（第一版就是靠它抓到"时间轴在走、模型不动"）；
    # P5/P6 的点击坐标来自 `CHAR-PREVIEW-CAPSULE-SCREEN` 投影锚点，不猜。
    "verify_char_preview",

    # ---- 编辑器进 Play 时不崩（P7，2026-10-23）----
    # 用户原话："我添加了一个第三人称角色，然后直接点击运行游戏，然后引擎
    # 编辑器直接崩溃退出了。"
    # 根因是**地形高度场的所有权**：SceneBinding 是局部变量，只 move 出
    # world，`heightfield()->heights` 随即悬垂；P6 给编辑器 Play 加了
    # `editorGroundHeight()` 之后第一次读它就 ACCESS VIOLATION。
    # 关键在于**必须带地形**（water_bowl）：空场景/纯 groundPlane 的场景
    # 根本不会建高度场，这条用例就退化成"进 Play 不崩"，抓不到这个 bug。
    "verify_editor_play_terrain",

    # ---- Play 态的鼠标两态（P8，2026-10-23）----
    # 用户原话："游戏启动运行后，必须要鼠标左键在 3d 视口中点击后才能开始
    # 正常和游戏画面内的角色开始交互操作，并且此时鼠标是隐藏不显示的。
    # 按下 Esc 键，此时就正常显示鼠标。"
    # M0~M11 十三条。两个"不可辩驳"的判据：
    #   · M2/M5/M9 查 **GetCursorInfo 的 CURSOR_SHOWING 位** —— 日志说
    #     captured=1 只能证明"我们想隐藏"，真正藏光标的是 GLFW；系统层的
    #     显示位才是真的。
    #   · M3 断言"没点之前 WASD 不动"，和 M6"点了就动"是一对：只测后者的话，
    #     "进 Play 即接管"也能全绿，需求其实没实现。
    "verify_play_mouse_capture",

    # ---- 帧节奏（2026-10-23）----
    # 用户原话："角色在移动时尤其是鼠标移动转换视角时，明显感觉角色动作在
    # 卡顿，步伐也变得僵硬了起来。"
    # 探针证明位移与动画相位都是干净的，脏的是**时基**：主循环在 60 Hz 的
    # 屏上跑到 470~490 fps（呈现模式 MAILBOX 不锁帧），dt 抖动 31%。
    # 判据两组对照：锁帧后 dt 抖动 < 8% + 零尖刺；MYVK_VSYNC=0 时 > 200 fps
    # 且抖动 > 10%（证明开关是活的、而且这就是卡顿的来源）。
    "verify_frame_pacing",

    # ---- 地形笔刷圆环不随地形变形（2026-10-23）----
    # 用户原话："Raise/Lower 这个笔刷操作地形时，笔刷的圆圈明显在变形。"
    # 根因：环点逐点写 `wp.z = heightAtWorld()`（贴着被自己推起来的地表），
    # 投影出来就不再是椭圆。判据是**像素级**的：r(θ) 的非椭圆分量 < 2%，
    # 外加"笔心十字必须落在圆心"（旧代码画在 outer[0]，即圆的右边缘）。
    "verify_brush_ring",

    # ---- 地形共面 / 形状雕刻 / 坡道 / 力度 / 取景（2026-10-24 收编）----
    # 这条**之前不在套件里**，理由是"旧面板布局已漂移"（见 verify_brush_ring
    # 上的注释）。漂移的真相在 2026-10-24 才查清：8 个工具按钮的第 4 行被
    # 面板底边裁掉，`LS-TOOL-7` 的中心落在面板**下方**，点下去砸到底部通栏上、
    # 把中间视口切成 Command Palette —— 之后 B/C/D 三组整片假 FAIL。
    # 产品侧已把按钮网格压进可见区（并加 reveal() 兜底），脚本侧补了
    # F 组（Ctrl+. / Focus player 的取景几何），现在 20 条全绿，收编进套件，
    # 免得下次布局一动又悄悄烂掉。
    "verify_landscape_ground",

    # ---- 材质仓库 / 地形默认材质 / 出生点落地（2026-10-24）----
    # 用户原话（四件事一起提）：
    #   ① "GameStart 的标记任务比例不对需要和真人一比一还原"；
    #   ② "它的初始位置是在地形位置一下的……要初始化在地形地面上面"；
    #   ③ "初始化地形的地面要尽量保持和栅格地面是同一个平面"；
    #   ④ "打造一个材质仓库……以后地形地面就默认用 grass_ground"。
    # 静态段守磁盘产物（清单 / vmat / 三张贴图 / 生成脚本自检），
    # 运行段起一次编辑器走 `MYVK_LANDSCAPE_AUTOCREATE`（= 面板上点"创建
    # 地形"的同一条路径），断言：仓库被读进来 / 层 0 挂 grass_ground 且
    # **贴图真的加载了**（albedo=file 而不是退回纯色）/ 地形 z=0 /
    # 出生点被抬到 terrainH / 有地形时栅格退场 / 标记高宽比 2.64（Q 版 1.92）。
    # ⚠ GUI 用例（1 次启动编辑器），不能与其它 GUI 用例并行。
    "verify_material_library",

    # ---- 地形层材质：从目录刷新导入（2026-10-24 第六轮）----
    # 用户原话："那里的地形材质一般是指定一个目录里面，将对应的材质信息按照
    # 之前 E:\code\cpp\venn-resource 中的材质信息的形式去自动添加，这个通过
    # 一个刷新按钮去找到最新的材质信息"。
    # 走真实 UI 路径：选中 Terrain → 属性栏 Layer materials 上方的目录框
    # （由 `MYVK_MATERIAL_SOURCE` 给出）→ 点 `INS-MATSRC-REFRESH`。
    # 断言产物（.vmat / 三张贴图 / 清单 2→3 条 / 重载 count=3）以及**幂等**
    # （再点一次 added=0）。`MYVK_MATERIAL_NO_MIRROR=1` 防止测试材质被写回
    # 仓库的 assets/。
    # ⚠ GUI 用例（1 次启动编辑器），不能与其它 GUI 用例并行。
    "verify_material_import",
]

# 结果行：兼容四种写法。取**最后一条**，因为有的脚本会先打一堆中间断言。
OK_RE = re.compile(r"(?:RESULT:\s*PASS|全部断言 PASS|^全部 PASS\b|结果:\s*PASS)")
BAD_RE = re.compile(r"(?:RESULT:\s*FAIL|断言[^\n]*FAIL|结果:\s*FAIL)")
# 资源泄漏：验证层在 vkDestroyDevice() 时点名没被销毁的子对象。
LEAK_RE = re.compile(r"leaked objects that have not been destroyed")

# ============================================================
# 变更影响分析：监视哪些目录 / 文件怎么映射到用例
# ============================================================

# 快照覆盖的**构建输入**（相对 ROOT）。只放"改了会影响 build/bin 产物"的
# 东西；运行时数据（assets/scenes、assets/scripts）**不能**进来 ——
# 编辑器自己会写它们，进了快照就每轮都判成"变了"。
WATCH_ROOTS = [
    "engine",
    "editor",
    # 运行时（G01 起是**独立目标** VennRuntime → Game.exe）。之前不在这里，
    # 于是"只改了 runtime/ 一个字节"在影响分析里**完全不可见** —— 这是
    # 新增运行时那一轮必须补上的一个洞（不补就等于 runtime 没有回归）。
    "runtime",
    # 启动器（2026-10-19）：第三个应用层目标 VennLauncher → Launcher.exe。
    # ⚠ 和 runtime 一样的道理 —— 不进快照就等于"启动器没有回归"，
    #   改坏了 `LauncherApp.cpp` 也不会有人被叫醒。
    "launcher",
    # 工程模板（`templates/<type>/`）：启动器建工程时**原样拷走**它就是
    # 产品本身（第一人称/第三人称的初值差异全在这里）。它不是运行时数据
    # ——只有 `tools/gen_templates.py` 会写它，所以可以进快照。
    "templates",
    # 资产链（2026-10-22，阶段六/七）：`assetio/` 编进 `VennAssetIO` 静态库
    # （引擎和离线工具**共用**同一份格式读写），`assetc/` 编成 `AssetC.exe`。
    # ⚠ 和 runtime / launcher 一模一样的道理 —— 不在这里就等于"资产链没有
    #   回归"：改了 `MeshFormat.cpp` 的一个字段偏移，`verify_vmesh` 该被叫醒，
    #   但它连"变了"都看不见。只有编译输入，没有任何运行时数据，可以进快照。
    "assetio",
    "assetc",
    "assets/shaders",
    "assets/models",
    "assets/textures",
    # 出厂配置（G05）：只读、由人写，运行时不改它 → 可以进快照。
    # ⚠ `assets/scenes/**` 仍然**不能**进来（编辑器会写它们，见下）。
    "assets/game.json",
    "CMakeLists.txt",
    "Makefile",
    "make.cmd",
]

# 一改就必须全量重跑的**横切**文件。
# 判据：它被几乎所有用例间接依赖，或者它是"多处必须逐字对齐"的契约来源。
GLOBAL_GLOBS = [
    # 构建系统：变体表 / shader 编译列表 / 关键字 -D 全在这里生成
    "CMakeLists.txt", "Makefile", "make.cmd", "cmake/**",
    # 基础设施层：所有 pass 都压在它上面
    "engine/core/**", "engine/rhi/**",
    # FrameUBO / set0 描述符 / 变体位序 —— 三处逐字对齐的契约
    "engine/render/RenderTypes.h",
    "engine/render/ShaderKeywords.*",
    "engine/render/ShaderPath.*",
    "engine/render/VariantPipelines.*",
    "engine/render/FrameGraph.*",
    "engine/render/FrameResources.h",
    # 接线中枢：所有 pass 的录制 / 描述符写入 / 统计都在这里，
    # 文件粒度看不出改的是哪一段 → 只能保守。想跳过用 --narrow。
    "engine/render/Renderer.cpp", "engine/render/Renderer.h",
    # 特性注册表（E01）：**每个**特性的 env 初值都从这里的 applyEnv()
    # 走一遍。改一条解析分支 = 改某个特性在测试里的启动值 → 横切。
    # （往 kFeatures[] 加新参数其实不影响既有行为，但文件粒度分不出来，
    #   按"宁可多跑"处理；想精确跳过用 --narrow。）
    "engine/render/FeatureRegistry.*",
    # 场景 / ECS 容器：几乎每个用例的场地都由它搭
    "engine/scene/Scene.h", "engine/scene/Scene.cpp",
    "engine/scene/Transform.h", "engine/ecs/**", "engine/ui/**",
    # 资产层：Mesh / Texture / Material / AssetManager 是公共入口
    "engine/assets/**",
    # 物理：player / collision / drop_to_floor 三个用例，但 Scene 也用它
    "engine/physics/**",
    # 共享着色器片段：被全部 .frag/.vert include
    "assets/shaders/common.glsl",
    "assets/shaders/space.glsl",
    # 前向主着色器：所有不透明物体都走它
    "assets/shaders/pbr.frag",
    "assets/shaders/pbr.vert",
    "assets/shaders/fullscreen.vert",
    "assets/shaders/depth_only.frag",
    # ⚠ `assets/models/**` 与 `assets/textures/**` **故意不在这里**：
    #   它们只被个别用例读（模型 → character_window / lod；贴图 → sss 的
    #   LUT、ibl 的 brdf LUT…），放进横切清单会把所有图形用例一起拖下水。
    #   留在"未登记"状态的效果一样安全（没登记 → 全量 + 点名），
    #   而且还多一层提示：看到名字就知道该往 TRIGGERS 里补哪一条。
]

# 改了**不影响任何用例**的东西（文档 / 记忆 / 编译辅助脚本）。
IGNORE_GLOBS = [
    "docs/**",
    ".workbuddy/**",
    ".github/**",
    "assets/shaders/compile.bat",
    "assets/shaders/compile.sh",
]
IGNORE_SUFFIXES = (".md", ".spv", ".json.bak", ".orig", ".rej")

# 渲染层的**逐个模块** glob（= `engine/render/` 下非横切的全部 .cpp/.h）。
#
# ⚠ 刻意**不写** `engine/render/**`：那样会把"新增的、还没人认识的 render
#   文件"也判成"已登记"，从而**绕过**"未登记 → 全量"这条兜底 —— 新模块
#   本该触发全量，结果只跑了一个用例。逐个列出来就没有这个洞：
#   新加模块仍是"未登记" → 全量（安全）。
# `check_regression_map.py` 的 [F] 项会自动核对"engine/render 下的每个文件
# 要么在 GLOBAL_GLOBS、要么在这里"，所以漏列会被抓出来。
RENDER_MODULES = [
    "engine/fx/ParticleSystem.*",
    "engine/render/Bindless.*",
    "engine/render/CameraEffects.h",
    "engine/render/ClusteredLighting.*",
    "engine/render/DecalBuffer.*",
    "engine/render/Environment.*",
    "engine/render/Frustum.*",
    "engine/render/GpuTimer.*",
    "engine/render/HdrTarget.*",
    "engine/render/HiZOcclusion.*",
    "engine/render/LightTextureLibrary.*",
    "engine/render/LocalShadowPass.*",
    "engine/render/MdiDraw.*",
    "engine/render/OffscreenImage.h",
    "engine/render/PostProcess.*",
    # 画质预设表（2026-10-19，从 `editor/FeatureUIGen.cpp` 下沉到引擎层）：
    # 它决定的 22 个 `*.enabled` 会改变**走哪些 pass** → 场景里有没有泄漏、
    # 帧时间预算对不对都跟着它变，所以照 RENDER_MODULES 的规矩进泄漏门。
    "engine/render/QualityPresets.*",
    "engine/render/ReflectionProbe.*",
    "engine/render/RenderSettingsIO.*",
    "engine/render/RenderTarget.*",
    "engine/render/SSAOPass.*",
    "engine/render/ScreenSpaceSettings.h",
    "engine/render/ShaderHotReload.*",
    "engine/render/ShadowCull.h",
    "engine/render/ShadowPass.*",
    "engine/render/TerrainPass.*",
    "engine/render/VolumetricFog.*",
    # 水体：几何现生成 + 自己的 RenderPass/颜色拷贝图，是"泄漏"高发区
    # （视图借自 HdrTarget，resize 后忘了重设就是悬垂句柄）。
    "engine/render/WaterPass.*",
]

# 用例 ← 触发它的源码/着色器。
#
# ⚠ 只登记"我确认归属"的模块。没登记的用例 = 无条件重跑（安全退化），
#   所以这张表可以增量补，补漏不会漏测。`--explain` 会把"没登记的用例"
#   和"没登记的文件"都点名列出来，照着补即可。
#
# ⚠ **不要登记横切文件**：命中 GLOBAL_GLOBS 的文件永远先按"全量"处理，
#   写在这里也是死代码；更糟的是 `--narrow` 下它会变成一条"看着有归属、
#   其实没覆盖"的假映射，把该跑的用例静默跳过。所以下面这几例
#   **故意不登记**（它们的实现主体全在横切文件里）→ 无条件重跑：
#     verify_lod          —— LOD 选择在 Renderer.cpp/Renderer.h
#     verify_instancing   —— instancing 是 Renderer + pbr.vert 的路径
#     verify_alpha_mask   —— Mask 走 push.skin.w + pbr.frag
#     verify_pom          —— 位移在 pbr.frag::parallaxUv
#     verify_textures     —— Texture/AssetManager 在 engine/assets/**
#     verify_shader_variants —— 变体表由 ShaderKeywords/CMakeLists 生成
TRIGGERS = {
    # ---- 剔除 / LOD / 批次 ----
    "verify_frustum_cull":   ["engine/render/Frustum.*", "engine/scene/Bvh.*"],
    "verify_bvh":            ["engine/scene/Bvh.*"],
    "verify_mdi":            ["engine/render/MdiDraw.*",
                              "assets/shaders/mdi_compact.comp"],
    "verify_hiz":            ["engine/render/HiZOcclusion.*",
                              "assets/shaders/hiz_build.comp",
                              "assets/shaders/hiz_downsample.comp",
                              "assets/shaders/hiz_test.glsl",
                              "assets/shaders/occlusion_test.comp"],
    "verify_bindless":       ["engine/render/Bindless.*"],
    # ---- 阴影 ----
    "verify_csm":            ["engine/render/ShadowPass.*",
                              "engine/render/ShadowCull.h",
                              "assets/shaders/shadow.vert",
                              "assets/shaders/shadow.frag"],
    "verify_local_shadow":   ["engine/render/LocalShadowPass.*",
                              "assets/shaders/grid.frag",
                              "assets/shaders/grid.vert"],
    "verify_shadow_hardening": ["engine/render/ShadowPass.*",
                                "assets/shaders/shadow.vert"],
    # ---- 环境 / IBL / 雾 ----
    "verify_ibl":            ["engine/render/Environment.*",
                              "assets/shaders/ibl_brdf.comp",
                              "assets/shaders/ibl_irradiance.comp",
                              "assets/shaders/ibl_prefilter.comp",
                              "assets/shaders/sky_cube.comp"],
    "verify_volumetric_fog": ["engine/render/VolumetricFog.*",
                              "assets/shaders/fog_inject.comp",
                              "assets/shaders/fog_integrate.comp"],
    "verify_light_modifiers": ["engine/render/LightTextureLibrary.*",
                               "engine/render/ClusteredLighting.*"],
    # ---- 水体 ----
    # verify_water 测的是面板（创建/形状/滑块/水位/删除）；
    # verify_water_motion 测的是"手柄能拖 + 浪况单旋钮"。
    # ⚠ engine/water/** 不在 GLOBAL_GLOBS，所以必须在这里逐条登记 ——
    #   否则改 WaterBody 只会跑"碰巧命中"的用例，水面参数悄悄回归无人发现。
    "verify_water": ["engine/water/**", "engine/render/WaterPass.*",
                     "assets/shaders/water.*",
                     "assets/shaders/water_common.glsl",
                     "editor/WaterEditor.*"],
    "verify_water_motion": ["engine/water/**", "editor/WaterEditor.*",
                            "editor/ViewportPanel.*", "editor/PickingSystem.*",
                            "engine/render/WaterPass.*",
                            "assets/shaders/water_common.glsl"],
    # 新工程初始场景（地形 + GameStart）。断言落点：
    #   · 模板内容          → templates/** 与生成脚本
    #   · 出生点组件/序列化 → Components.h、SceneSerializer
    #   · GAMESTART-MARK 锚点 → ViewportPanel（标记画在这里）
    #   · 运行时出生        → RuntimeApp
    "verify_template_scene": ["templates/**", "tools/gen_templates.py",
                              "engine/ecs/Components.h",
                              "engine/scene/SceneSerializer.cpp",
                              "engine/render/TerrainPass.cpp",
                              "runtime/RuntimeApp.cpp"],
    # GameStart 在编辑器里的三条交互：点选 / 落地 / 双层聚焦。
    # 断言落点分别在 PickingSystem（点中）、ViewportPanel（落地）、
    # SceneHierarchy + EditorFocus（聚焦）。
    "verify_gamestart_edit": ["editor/PickingSystem.*",
                              "editor/ViewportPanel.*",
                              "editor/SceneHierarchy.*",
                              "editor/EditorFocus.h",
                              # [1b]：Player Start 段（视角模式 / 运行即生成 /
                              # 启用）只有 Inspector 里这一处编辑落点
                              "editor/InspectorPanel.*",
                              "engine/ecs/Components.h"],
    # ---- 屏幕空间 / 后处理链 ----
    "verify_screenspace":    ["engine/render/SSAOPass.*",
                              "engine/render/PostProcess.*",
                              "assets/shaders/ssao.frag",
                              "assets/shaders/ssao_blur.frag",
                              "assets/shaders/fg_ssgi.frag",
                              "assets/shaders/fg_ssr.frag",
                              "assets/shaders/fg_godrays.frag"],
    "verify_camfx":          ["engine/render/CameraEffects.h",
                              "engine/render/PostProcess.*",
                              "assets/shaders/fg_dof.frag",
                              "assets/shaders/fg_motionblur.frag",
                              "assets/shaders/fg_godrays.frag"],
    "verify_taa":            ["assets/shaders/fg_taa.frag",
                              "engine/render/PostProcess.*"],
    # ⚠ 2026-10-17 晚补：fxaa / local_shadow 的视口截图里**有地平面栅格**
    #   （编辑器默认开栅格）。栅格着色器一改，视口像素就变 → 这两个用例
    #   必须跟着跑（栅格修好之前它在地板上经常被深度冲突吃掉，所以这个
    #   依赖关系一直是隐形的）。
    "verify_fxaa":           ["assets/shaders/fxaa.frag",
                              "assets/shaders/grid.frag",
                              "assets/shaders/grid.vert"],
    "verify_auto_exposure":  ["engine/render/PostProcess.*",
                              "assets/shaders/fg_lum_average.comp",
                              "assets/shaders/fg_lum_downsample.frag",
                              "assets/shaders/fg_lum_histogram.comp"],
    "verify_color_lut":      ["engine/render/PostProcess.*",
                              "assets/shaders/post.frag"],
    "verify_drs":            ["engine/render/PostProcess.*",
                              "engine/render/HdrTarget.*",
                              "engine/render/RenderTarget.*",
                              "engine/render/OffscreenImage.h"],
    # 描边的输入是**深度预通道的颜色附件** —— 改 HdrTarget 的附件/布局、
    # 改 depth_only.frag 的输出、改 post.frag 的采样都会动到它。
    # FeatureRegistry 也登记：参数名或取值范围写错的表现是"面板上开不了"
    # 或"开到底也没有线"，而像素断言会先红在更早的一条上。
    "verify_outline":        ["engine/render/PostProcess.*",
                              "engine/render/HdrTarget.*",
                              "engine/render/RenderTarget.*",
                              "engine/render/FeatureRegistry.*",
                              "assets/shaders/post.frag",
                              "assets/shaders/depth_only.frag"],
    "verify_viewmodes":      ["engine/render/ScreenSpaceSettings.h"],
    # 材质页的形状由三处共同决定，任何一处单独改都会"编译过、画面错"：
    #   · Bindless.h/.cpp   —— C++ 侧的 96B 布局与页表填充（含 rampMode 三态）
    #   · pbr.frag          —— GLSL 侧的同名结构与四个宏
    #   · Material.h        —— 四个贴图槽与六个标量
    # 场景序列化也登记：新字段不往返的话，编辑器里设好、存盘就丢，
    # 表现是"重开场景 ramp 没了"，而不是崩溃。
    "verify_material_page":  ["engine/render/Bindless.*",
                              "engine/assets/Material.h",
                              "engine/scene/SceneSerializer.cpp",
                              "engine/assets/MaterialSelfTest.cpp",
                              "assets/shaders/pbr.frag"],
    # ---- 变体 / 热重载 / 时戳 ----
    # （verify_shader_variants 不登记：它的实现主体是 ShaderKeywords.* /
    #   VariantPipelines.* / CMakeLists.txt，全是横切文件 → 无条件重跑）
    "verify_shader_hotreload": ["engine/render/ShaderHotReload.*"],
    "verify_gpu_timing":     ["engine/render/GpuTimer.*"],
    # ---- 材质特性 ----
    "verify_sss":            ["assets/textures/sss_lut*"],
    # A1 二次元素描：着色器本体（pbr.frag）、关键字位（ShaderKeywords.*）、
    # 变体白名单（CMakeLists.txt）**全在横切表里**，所以这里只挂非横切的
    # 编辑器侧路径 —— shadingModel 下拉在 InspectorPanel，枚举 ↔ 字符串的
    # 往返（"toon" / "skin" / "default"）在 SceneSerializer。
    "verify_toon_shading":   ["editor/InspectorPanel.*",
                              "engine/scene/SceneSerializer.*"],
    # P0-2/B 渲染顺序：排序键在 Renderer.cpp、状态位在 VariantPipelines.* /
    # ShaderKeywords.*、字段在 Material.h —— **全在横切表里**。这里只挂
    # 真正属于本用例的非横切路径：材质参数的读写往返（SceneSerializer）
    # 和 Inspector 上那两个输入框。序列化漏一个字段的症状是"面板上调了、
    # 存档再打开又是 2000"，只有这个用例会发现。
    "verify_render_queue":   ["engine/scene/SceneSerializer.*",
                              "editor/InspectorPanel.*"],
    "verify_decals":         ["assets/shaders/decal.frag",
                              "assets/shaders/decal.vert",
                              "engine/render/DecalBuffer.*",
                              "engine/render/ScreenSpaceSettings.h"],
    # R5-1 反射探针：捕获/预过滤/盒投影整链都在 ReflectionProbe.*，片元侧的
    # 混合在 pbr.frag（已在横切表里）；组件与序列化不在 TRIGGERS 里是因为
    # 它们落在 SceneSerializer / Components.h —— 那两个本来就是横切。
    "verify_reflection_probe": ["engine/render/ReflectionProbe.*"],
    # R6-1 粒子系统：模拟/计数/绘制三条着色器 + 资源与描述符池全在
    # ParticleSystem.*；组件与序列化落在 Components.h / SceneSerializer.cpp
    # （本身就是横切，不登记）；面板（12 个 Shuriken 模块 / 色带 / 一键展开）
    # 在 InspectorPanel.* —— 所以它必须显式登记，否则改面板不会重跑本例的
    # D/E 两组断言。
    # ⚠ 这里原本写的是"InspectorPanel.* 是横切文件，所以面板那一组实际上无条件
    #   重跑" —— **这句是错的**：`GLOBAL_GLOBS` 里根本没有 `editor/**`，
    #   InspectorPanel.* 只是被 verify_inspector / verify_mesh_params 登记过，
    #   改它**不会**触发全量，于是本例的面板断言在"改面板"时被静默跳过。
    #   教训：判断"某文件是不是横切"只能去 `GLOBAL_GLOBS` 里查，别凭印象。
    "verify_particles":      ["engine/fx/ParticleSystem.*",
                              "assets/shaders/particle_sim.comp",
                              "assets/shaders/particle_count.comp",
                              "assets/shaders/particle_common.glsl",
                              "assets/shaders/particle.vert",
                              "assets/shaders/particle.frag",
                              "editor/InspectorPanel.*",
                              # 2026-10-18：H 组（逐发射器编辑 / 标题栏 ×）长在
                              # ParticlesPanel 上；I 组（层级右键 → 打开粒子
                              # 面板）的入口在 SceneHierarchy。这两个文件原来
                              # 只被 verify_viewport_bar / verify_hierarchy_status
                              # 登记 —— 改它们时本用例会**静默跳过**，H/I 两组
                              # 断言就白写了。（判断"横切"只看 GLOBAL_GLOBS，
                              # 这里两个都不是，所以可以登记。）
                              "editor/ParticlesPanel.*",
                              "editor/SceneHierarchy.*"],
    # ---- 编辑器 / UI ----
    # 主题表 / applyTheme 本体在 engine/ui/ImGuiTheme.*（新文件）——
    # ⚠ 但 `engine/ui/**` 是 GLOBAL_GLOBS，所以这条**不能只写它**，否则会被
    #   "全横切登记"检查判成"等于没登记"（--narrow 下假跳过）。下面挂的是
    #   主题/语言**真正会改到的编辑器侧文件**：
    #     · Localization.*     —— 英→中映射表（唯一的中文来源）
    #     · EditorLayoutIO.*   —— 偏好存档的 schema 版本门（kPrefsVersion）
    #     · Toolbar.*          —— 顶栏/传输条底色改走 ui::tokens().barBg
    #     · ViewportPanel.*    —— 视口叠加片在浅色主题下强制浅字
    #   （EditorApp.cpp 同理未登记 → 改它全量，保守正确。）
    # ⚠ U7 起词典本体在 `engine/i18n/**`：改那里同样会改编辑器的中/英显示，
    #   只登记 `editor/Localization.*`（已退化成 101 行转发壳）会漏掉这类改动。
    "verify_theme_language": ["engine/i18n/**",
                              "editor/Localization.*",
                              "editor/EditorLayoutIO.*",
                              "editor/Toolbar.*",
                              "editor/ViewportPanel.*"],
    # 出厂默认值（吸附全开 / 栅格 1m 与放大）。吸附默认值与步长在
    # EditorContext.h；栅格默认值在 engine/render/Renderer.h（= GLOBAL，
    # 所以这里必须另有非全横切的登记）；存档覆盖走 EditorLayoutIO.*。
    "verify_editor_defaults": ["editor/EditorContext.h",
                               "editor/EditorLayoutIO.*"],
    # 胶囊线框的顶点/边数组全部由 ColliderView.* 自己生成（点与边都在这里
    # 算完直接喂 AddLine）；拟合半径那一路在 engine/physics/**（= GLOBAL）。
    "verify_capsule_shape":  ["editor/ColliderView.*"],
    # E12 行尾状态列：图标绘制 / 命中矩形 / pending 队列全在 SceneHierarchy；
    # 可见性组件的语义在 engine/scene/Scene.cpp（= GLOBAL）。
    "verify_hierarchy_status": ["editor/SceneHierarchy.*",
                                "editor/InspectorIcons.h"],
    "verify_nav_gizmo":      ["editor/ViewportPanel.*",
                              "editor/GizmoController.*",
                              "engine/scene/Camera.*"],
    "verify_collider_gizmo": ["editor/ColliderView.*",
                              "editor/GizmoController.*"],
    "verify_add_menu":       ["editor/Toolbar.*", "editor/PrimitiveBuilder.*",
                              # E04/E05：View ▸ Panel / Window ▸ Panel 这些菜单项
                              # 是**从面板表生成**的，工作区子菜单也从预设生成 ——
                              # 表或预设改一个字，这一例的菜单断言就可能变。
                              "editor/PanelRegistry.*", "editor/Workspace.*"],
    # 底栏 + Content 抽屉：底栏与抽屉的外框都在 EditorApp；主体的让位高度
    # 走 SplitLayout 的 bottomInset；抽屉里画的是 ContentBrowser::drawBody；
    # 默认开关来自面板表与工作区预设（Workspace.*）。
    "verify_bottom_bar":     ["editor/EditorApp.*", "editor/SplitLayout.*",
                              "editor/ContentBrowser.*", "editor/ConsolePanel.*",
                              "editor/PanelRegistry.*", "editor/Workspace.*"],
    "verify_mesh_params":    ["editor/InspectorPanel.*"],
    "verify_character_window": ["editor/CharacterWindow.*", "assets/models/**"],
    # 角色表现链 A0（2026-10-18）。这些文件**都不在** `GLOBAL_GLOBS` ——
    # `engine/animation/**`（新目录：Pose / AnimatorSystem / 自检）和
    # `engine/scene/ModelInstance.*`（新文件）如果没人登记，就是"未登记 →
    # 全量"：能跑，但等于这两个模块没有增量回归（改一行全量跑两小时）。
    # `engine/ecs/**`、`engine/assets/**`、`engine/render/Renderer.*` 是横切，
    # 按铁律**不写**；`editor/EditorApp.cpp`（命令注册 / Play 挂载点 /
    # IMPORT 钩子）沿用项目惯例不登记（同文件里还有几十处别的接线）。
    "verify_anim_runtime":   ["engine/animation/**",
                              # 2026-10-25：SceneSerializer 新增 springBone /
                              # lookAt 两段的读写。这两个段落的字段名和默认值
                              # 一旦写错，症状是"存盘再开头发不飘了"，而
                              # 动画自检看不到 —— 所以改序列化就重跑它。
                              #
                              # ⚠ 2026-10-25（§16）：`Skeleton::findJoint` 的
                              #   三条断言（exact / substring / endtip）住在
                              #   动画自检里，但 `engine/assets/**` 在
                              #   `GLOBAL_GLOBS` —— 改 Skeleton.* 本来就全量，
                              #   按铁律**不写**（写了会在 --narrow 下假跳过）。
                              "engine/scene/SceneSerializer.*"],
    "verify_character_import": ["engine/animation/**",
                                "engine/scene/ModelInstance.*",
                                # SCENE-SKINNED / SCENE-ANIMATOR 四条锚点（存 + 读）
                                "engine/scene/SceneSerializer.*",
                                # MODEL-INSTANCE / 唯一化名字 / IMPORT 落点
                                "editor/EditorScene.*",
                                "editor/CharacterWindow.*",
                                # INS-ANIM-* 矩形 + Add Animator（A1）
                                "editor/InspectorPanel.*",
                                # anim.time / anim.clip 控制台命令（A1 起写 anim 那一份）
                                "editor/EditorApp.*",
                                # 逐 item jointBase 透传（阴影两条路径）
                                "engine/render/ShadowPass.*",
                                "engine/render/LocalShadowPass.*"],
    # 角色默认模型 + 移动状态机（P4，2026-10-22）。
    # `engine/animation/Locomotion.*` 是**编辑器 Play 与 Game.exe 共用**的那份
    # 状态机（此前运行时有一半、编辑器完全没有 → Play 时腿不动）。它自己的
    # 目录 `engine/animation/**` 已由 verify_anim_runtime / verify_character_import
    # 登记，这里多登记一遍是**允许**的（这张表是"文件→用例"，不是互斥）。
    # ⚠ `engine/ui/KeyCodes.*` 虽然是本轮新增的按键翻译表（GLFW ↔ ImGui，
    #   错了的表现是"绑了键但不触发"），但 `engine/ui/**` 在 `GLOBAL_GLOBS`
    #   里 → 改它本来就全量，写进来会在 `--narrow` 下假跳过。
    # ⚠ `engine/assets/MeshLoader.*`、`engine/ecs/Components.h` 同理在横切表
    #   里，按铁律**不写**。
    "verify_char_locomotion": ["engine/animation/Locomotion.*",
                               # 2026-10-25：`switchClip` 从 Locomotion 的匿名
                               # namespace 抽到了这里（状态机是第二个消费者）。
                               # 改它等于改 C6/C7 的切换行为 —— 必须一起跑。
                               "engine/animation/ClipSwitch.*",
                               "editor/CharacterWindow.*",
                               # bindDefaultCharacter / kDefaultCharacterPath
                               "editor/EditorScene.*",
                               # animKeys 的存 + 读（C9 的闭环全靠它）
                               "engine/scene/SceneSerializer.*",
                               # 编辑器没有的组件在 Play 里就不存在 → 接线在这儿
                               "editor/EditorApp.*",
                               # 2026-10-24：C5d/C5e 的模型装配断言跑在 Play 之后
                               # —— 出生点 → 玩家实体这一步在 EditorContext 的
                               # preparePlayerAtGameStart 里（含 animKeyMap 复制）
                               "editor/EditorContext.*",
                               # tickPlayerAnimation / idleTriggerSec
                               "runtime/RuntimeApp.*",
                               "runtime/RuntimeConfig.*",
                               # 新增的 8 条界面词
                               "engine/i18n/**",
                               # 默认角色资产本身（Alicia：128 骨 / 4 段 clip）。
                               # ⚠ 换默认角色 = 换这个 glob 底下的文件，所以
                               #   **只改 EditorScene 的路径常量不算"改了资产"**
                               #   —— 那条由 editor/EditorScene.* 覆盖。
                               "assets/models/characters/**"],
    # 运行时"手感四件事"（P5，2026-10-22）。与上一条大量重叠是**故意**的：
    # 两张表是"文件→用例"，一条修的是编辑器 Play 的腿，另一条修的是运行时的
    # 朝向/鼠标/过渡，改坏任何一边另一条都不会红。
    # ⚠ `engine/ecs/Components.h`（fade 字段）与 `engine/ui/**`（KeyCodes）
    #   在横切表里，按铁律不写。
    "verify_player_feel":    ["engine/animation/AnimatorSystem.*",  # crossfade
                              "engine/animation/Locomotion.*",    # blend 配置
                              "engine/scene/ModelInstance.*",     # yawOffsetDeg
                              "runtime/RuntimeApp.*",             # 鼠标两态
                              "runtime/RuntimeConfig.*",          # modelYaw
                              # 出生点口径（脚底 + 胶囊中心）
                              "runtime/PlayerController.*",
                              # Play 时按 GameStart 摆玩家
                              "editor/EditorContext.*",
                              # 编辑器侧 facing 自检
                              "editor/EditorScene.*",
                              # hips 不再下沉（脚陷的根因）
                              "assetio/ProcAnim.cpp",
                              "assets/models/characters/**"],
    # 默认操控 + 真实跳跃（P6，2026-10-22）。跳跃的物理在**两个地方**各写
    # 一遍（运行时走 CharacterController，编辑器 Play 手写同一套抛物线），
    # 所以两边都要盯着 —— 只改一边时另一条的 J/P 判据不会红，这正是本条
    # 用例存在的价值。
    # ⚠ 物理层（engine/physics/**）、Components.h、InputMap.cpp 都在
    #   GLOBAL_GLOBS 里，按铁律**不重复登记**。
    "verify_player_jump":    ["engine/animation/Locomotion.*",        # jump 状态
                              # poseJump 的重写 + Jump clip 时长 0.75 s
                              "assetio/ProcAnim.cpp",
                              "assets/models/characters/**",
                              "runtime/RuntimeApp.*",                 # 装配时的 jumpSpeed 公式
                              "editor/EditorApp.*"],                  # 编辑器 Play 的抛物线
    # 脚踩地形（P6，2026-10-22）。核心是 CharacterController 的**接触窗口**
    # （在 GLOBAL 里，改它必然全量）；这里登记的是"探针 + 高度场装配"这一侧。
    "verify_player_ground":  ["runtime/PlayerController.*",            # GAME-FEET / RESCUE
                              "runtime/RuntimeApp.*"],                # 探针
    # 角色面板预览视口（P6，2026-10-22）。三块拼起来才有"能看动画 + 有栅格 +
    # 能选中胶囊"：面板（投影/点选/叠加层）、蒙皮（syncPreviewSkinning ——
    # 预览不动的根因就在这儿）、预览栅格（Renderer 在 GLOBAL，不重复登记）。
    "verify_char_preview":   ["editor/CharacterWindow.*",              # 叠加层 / 点选 / 控件条
                              "engine/scene/ModelInstance.*",          # jointMatrices 接手
                              "editor/EditorApp.*",                    # Play 态 Space 语义
                              "assets/models/characters/**"],
    # 编辑器进 Play 不崩（P7，2026-10-23）。这条盯的是**地形高度场的所有权**
    # —— 谁能安全地 `heightfield()`。`PhysicsWorld::setHeightfield` 从"借指针"
    # 改成"拷一份"之后，"是不是又有人把裸指针借出去"就是它的核心风险。
    # ⚠ 按本节约定**只列非横切文件**：`engine/physics/**`、`engine/ecs/**`
    #   都在 `GLOBAL_GLOBS` 里，改它们一改就全量，写进 TRIGGERS 会被
    #   `check_regression_map.py` 标成"冗余的横切 glob"。
    "verify_editor_play_terrain": ["editor/EditorApp.*",                 # buildPhysicsWorld / editorGroundHeight
                                   # 2026-10-24：T0b/T0c 断言"加人称角色只
                                   # 配置出生点、不往视口塞身体"（PLAYER-START
                                   # 由 EditorScene::createPlayer 打），T1b 断言
                                   # 进 Play 才生成（EditorContext →
                                   # spawnPlayerAtGameStart）。三处缺一条就断链。
                                   "editor/EditorScene.*",
                                   "editor/EditorContext.*",
                                   "editor/ViewportPanel.*"],
    # Play 态鼠标两态（P8，2026-10-23）。核心风险是**"谁能改光标模式"**：
    # 状态机跨三个文件（Context 存状态 / 面板发请求 / App 落 GLFW），任何
    # 一环改错的表现都是"鼠标消失但角色不动"或"按 Esc 没反应"。
    # ⚠ `engine/core/Input.*` 在 GLOBAL_GLOBS 里（InputMap 同族），不重复登记。
    "verify_play_mouse_capture": ["editor/EditorContext.*",             # 两态状态 + 脏标记
                                  "editor/ViewportPanel.*",             # 点视口发请求
                                  "editor/EditorApp.*"],                # updatePlayMouse / Esc 分流
    # 帧节奏（2026-10-23）**故意不登记 TRIGGERS**：它的三个落点
    # （`core/RunMode.h` / `rhi/Swapchain.*` / `core/Time.h`）全在
    # GLOBAL_GLOBS 里 —— 那是"改了就全量重跑"的横切表。写进这张表会被
    # check_regression_map.py 判成"整条都指向横切文件"（--narrow 下假跳过）。
    # 也就是说：它是**无条件重跑**的用例，这正是对的。
    # 地形笔刷圆环（2026-10-23）。只有一处：圆环怎么取点、怎么投影。
    "verify_brush_ring":     ["editor/LandscapeEditor.*"],
    # 地形共面 / 雕刻 / 坡道 / 力度 / 取景（2026-10-24 收编）。落点全在
    # LandscapeEditor：默认起伏与共面（createTerrain）、形状压印（Carve）、
    # 坡道的最小段长、强度上限 kMaxStrength、取景几何（focusCamera /
    # focusCameraAtPlayer）。`editor/EditorApp.*` 是"进入地形模式就取景"的
    # 调用点；`editor/ViewportPanel.*` 那份 `Brush` 面板的可见区也在这里被断言。
    "verify_landscape_ground": ["editor/LandscapeEditor.*",
                                "editor/EditorApp.*",
                                "editor/ViewportPanel.*"],
    # 游戏 UI / HUD（U0 起，docs/ui_hud_roadmap.md）。    # ⚠ `engine/ui2/**` **故意不进** `GLOBAL_GLOBS`：它只被 UI 通道引用，
    #   不像 `Renderer.*` / `engine/ecs/**` 那样横切。放进去会让全量永远
    #   点名，增量回归就失去意义了（这是路线图 §8.3 的明确决定）。
    "verify_ui2":            ["engine/ui2/**",
                              # UI 的插入点 / 两个输出 pass 的 hook
                              "engine/render/PostProcess.*",
                              # 着色器（UI 顶点/片元，sRGB 反编码在这里）
                              "assets/shaders/ui2.*",
                              # 词典搬家（U7）：单一权威在 engine/i18n
                              "engine/i18n/**",
                              # 离线生成的 SDF 图集（变了 → 所有文本断言重跑）
                              "assets/ui/**",
                              # 面板上的 UI 分区（ui.* 参数的控件长在这里）
                              "editor/InspectorPanel.*"],
    # HUD / 暂停菜单 / 焦点栈（U6+U8，docs/ui_hud_roadmap.md）。
    # ⚠ 这里**只列非横切文件**：`engine/core/**`（Input / InputMap / Window）
    #   与 `engine/render/**` 的几张契约表都在 `GLOBAL_GLOBS` 里，一改就全量，
    #   写进 TRIGGERS 会在 `--narrow` 下假跳过。
    # ⚠ `assets/scenes/**` 不进快照（那是运行时会写的目录），所以夹具场景
    #   改动**不会**自动点名本用例 —— 改夹具时手动 `run_regression.py hud`。
    "verify_hud":            ["runtime/ui/**",              # HUD / 菜单 / config
                              "runtime/RuntimeApp.*",       # tickUi / 暂停迁移
                              "runtime/GameState.*",        # Paused 的 resume/quit
                              "runtime/main.cpp",           # --selftest hud 注册
                              "engine/ui2/UiFocus.*",       # 焦点栈本体
                              "engine/ui2/UiCanvas.*",      # 游戏 UI 的注入点
                              "engine/script/ScriptEngine.*",  # 脚本 damage/popup
                              "engine/render/PostProcess.*",   # uiOutputExtent 同源
                              "assets/shaders/ui2.*"],         # 同样是像素依赖
    # 中文化覆盖率：**取值域是整个 editor/**。
    # 判据是"屏幕上还有没有英文"，而任何一处新加的 UI 字面量都可能破坏它 ——
    # 没法收敛到几个文件。所以这里用 `editor/**` 而不是列举。
    # ⚠ 它是静态扫描（几百毫秒），全量触发也不心疼；`editor/**` 不在
    #   `GLOBAL_GLOBS` 里（那是横切渲染链的定义），所以这里不算"登记横切文件"。
    "verify_i18n_coverage":   ["editor/**",
                              # 词典本体：改词条自己也要重跑
                              # ⚠ 别把 engine/render/FeatureRegistry.* 写进来 ——
                              #   它在 GLOBAL_GLOBS 里，一改就全量重跑，列在这里
                              #   只会被 check_regression_map 判成"冗余横切"
                              #   （而且 --narrow 下会造成假跳过）。
                              "tools/verify_i18n_coverage.py"],
    "verify_material_roundtrip": ["engine/scene/SceneSerializer.*"],    # 材质槽小方块：控件与"三列布局"都在 InspectorPanel.*；而**槽位拿到的可用宽度
    # 由 SplitLayout 给的面板宽度决定** —— 2026-10-16 的"文件名被裁成 .png"正是
    # "图标条 48px + 值列写死 88 + 槽位列 88"把槽位挤到 48px 造成的，所以布局那个
    # 文件必须登记（改 `kRightMin` / 拖分栏都会改到它）。
    # 拖放载荷类型在 EditorDragDrop.*。
    # ⚠ `engine/assets/**` 与 `assets/shaders/pbr.frag` 都在 `GLOBAL_GLOBS` ——
    #   **别再写进这里**（`--narrow` 下会变成假跳过）。
    "verify_material_chips": ["editor/InspectorPanel.*",
                              "editor/SplitLayout.*",
                              "editor/EditorDragDrop.*"],
    "verify_primitive_roundtrip": ["engine/scene/SceneSerializer.*"],
    "verify_player":         ["editor/EditorScene.*"],
    # 第三人称相机三条：俯仰范围、弹簧臂、地面钳制。
    #   · 弹簧臂实现（球扫掠 / 穿透优先 / 地面钳制反解）→ engine/scene/SpringArm.*
    #   · 俯仰常量的单一来源                        → engine/scene/LookLimits.h
    #   · 运行时的钳制与相机段                      → runtime/PlayerController.*
    #   · 编辑器 Play 那条链（BVH + 地形的高度场）  → editor/EditorApp.*
    # 少登记任何一条，在 --narrow 下改那一条就会把本用例判成"不受影响"。
    "verify_third_person_camera": ["engine/scene/SpringArm.*",
                                   "engine/scene/LookLimits.h",
                                   "runtime/PlayerController.*",
                                   "editor/EditorApp.*"],
    # ⚠ 这一例同时是**资源泄漏门**：全套里只有它会让编辑器**优雅退出**，
    #   因而也只有它的日志里会出现 `vkDestroyDevice(): ... leaked objects`
    #   （其余脚本都是强杀进程，泄漏报告根本不会产生）。
    #   所以它必须跟着**整个渲染层**跑 —— 任何 render 模块少了一个
    #   vkDestroy*（image/view/memory/framebuffer…）只有它能发现。
    #   R4-4 的 DecalBuffer 析构漏调 destroyFrames()、漏掉 20 个对象，
    #   就是这么抓到的。挂 `RENDER_MODULES`（逐个模块，不是 `engine/render/**`）
    #   是**放宽**方向（多跑），不存在假跳过风险。
    "verify_collision_play":  ["editor/EditorScene.*"] + RENDER_MODULES,
    "verify_drop_to_floor":  ["editor/EditorScene.*"],
    # ---- 阶段 C：核心面板 ----
    # E11：分区折叠表 / 关键词表 / POD 快照全在 InspectorPanel 里
    "verify_inspector":      ["editor/InspectorPanel.*"],
    # E11.1：左侧图标导航条 + 按页签分区。页签表、页签派发、空页签提示全在
    #        InspectorPanel.*；11 个矢量图标在 InspectorIcons.h（新文件，
    #        只有这里会用到）。
    #        ⚠ **两条故意不登记**（登记了反而制造"假跳过"，见文件顶部铁律 2）：
    #          · `editor/EditorLayoutIO.*` —— 页签选择落盘确实在这里，但这个
    #            文件是**全编辑器共用的落盘守卫**（布局 / 偏好 / 通知历史都走它），
    #            只挂在本例名下会让 verify_viewport / verify_notifications
    #            在它变更时被判成"不受影响"；
    #          · `editor/EditorApp.cpp` —— 宿主接线（应用落盘的 inspTab、面板表），
    #            同一文件里还有几十处别的面板接线（项目既有约定：未登记 → 全量）。
    #        两者保持"未登记"= 变更时全量重跑，保守且与 E18/E19/E21 的写法一致。
    "verify_inspector_tabs": ["editor/InspectorPanel.*",
                              "editor/InspectorIcons.h"],
    # E13：过滤 / 面包屑 / 视图模式 / 缩略图档位在 ContentBrowser，
    #      缓存淘汰在 ThumbnailCache，拖放载荷定义在 EditorDragDrop.h
    "verify_content_browser": ["editor/ContentBrowser.*",
                               "editor/ThumbnailCache.*",
                               "editor/EditorDragDrop.h"],
    # 2026-10-17 晚：Content 三栏改造（目录树 / 类型单选列 / 内容+搜索+排序）。
    # 看守对象：三栏从左往右且都落在抽屉里、点树节点切目录、类型单选生效、
    # 排序图标紧贴搜索框右边、排序菜单四个勾选框、最近搜索、以及
    # "抽屉重开后不被视口盖住 / 分割线不画进抽屉"这两个层级回归。
    "verify_content_three_pane": ["editor/ContentBrowser.*",
                                  "editor/EditorApp.*",
                                  "editor/SplitLayout.*"],
    # 2026-10-17 晚：右栏顶部动作条（Add ▸ 13 类型 + 导入 / Save All /
    # 上一层·下一层 / 路径框）。Save All 走 EditorContext 的请求位，
    # 真正落盘在 EditorApp；导入对话框的 Win32 调用在 ContentBrowser。
    "verify_content_action_row": ["editor/ContentBrowser.*",
                                  "editor/EditorApp.*",
                                  "editor/EditorContext.h",
                                  "editor/Localization.cpp"],
    # E14：四角浮层 / 书签 / 显隐全在 ViewportPanel；相机 yaw/pitch 存取走
    #      引擎的 scene::Camera（书签存的就是它）
    "verify_viewport":       ["editor/ViewportPanel.*",
                              "engine/scene/Camera.*"],
    # 2026-10-17 晚：视口工具条（图标/下拉/吸附/视图菜单）+ 底栏面板托盘
    #   + 两张新面板。改动面横跨这几处，任何一个动了这一组都要重跑。
    "verify_viewport_bar":   ["editor/ViewportPanel.*",
                              "editor/EditorApp.*",
                              "editor/EditorContext.h",
                              "editor/EditorContext.cpp",
                              "editor/GizmoController.cpp",
                              "editor/PanelRegistry.*",
                              "editor/PhysicsPanel.*",
                              "editor/ParticlesPanel.*",
                              "editor/Localization.cpp"],
    # ---- 阶段 D：交互理念 ----
    # E18：注册表本体 + 模糊匹配 + 命令面板 UI 全在新文件 CommandRegistry；
    #      命令的**登记**（从特性/面板/视图模式/工作区/控制台五路派生）在
    #      EditorApp::registerEditorCommands。这里只登记前者 —— EditorApp.cpp
    #      未登记（改它 → 全量），是安全的保守行为。
    "verify_command_palette": ["editor/CommandRegistry.*"],
    # E19：设置类改动的撤销 —— 落地在 Command.h/.cpp（ParamEditCommand）+
    # FeatureUIGen（IsItemActivated/IsItemDeactivatedAfterEdit 钩子）。
    #       EditorApp::onFeatureParamUndo 是"宿主侧合并"的实现，但
    #       EditorApp.cpp 未登记（改它 → 全量），保守正确。
    "verify_undo_settings": ["editor/Command.*"],
    # E21：通知 / 进度全在本文件对（Toast / 历史 / 任务条 / 落盘 / 状态栏锚点）。
    #       宿主接线在 EditorApp.cpp（未登记 → 全量）；新增的 Logger hook 通道
    #       在 engine/core/Logger.h —— 它落在 GLOBAL_GLOBS 里（engine/core/**），
    #       所以那一侧一改本来就全量，不必也不该写进 TRIGGERS（写了会在
    #       --narrow 下假跳过）。
    "verify_notifications": ["editor/NotificationCenter.*"],
    # ---- 阶段 E：生产力 ----
    # E25：时间轴本体在新文件；Console 的 setSearchFilter（T4d 的
    #       CONSOLE-FILTER 锚点）也在本用例的断言链上，一并登记。
    #       宿主接线（采样/钩子/面板表一行/工作区清单）在 EditorApp.cpp 与
    #       Workspace.cpp —— 两者都未登记（改它们 → 全量），保守正确。
    "verify_gpu_timeline": ["editor/GpuTimelinePanel.*",
                            "editor/ConsolePanel.*"],
    # ---- 物理（R7-1）----
    # ⚠ **故意不登记**，与 verify_lod / verify_textures 等同一处理方式。
    #   理由：`engine/physics/**` 早就是 GLOBAL_GLOBS（player / collision /
    #   drop_to_floor 都吃它），所以改任何物理文件本来就会全量重跑 →
    #   verify_physics 必然被执行，功能上不需要 TRIGGERS 这一行。
    #   反过来，若把 `engine/physics/**` 登记到本用例名下，`--narrow` 模式下
    #   一次 CollisionWorld 的改动就只会跑 verify_physics，把
    #   verify_collision_play / verify_player / verify_drop_to_floor 判成
    #   "不受影响" —— 那才是真正的假跳过。宁可多跑。
    # ---- 地形物理（2026-10-22 第十轮）----
    # 实现主体 `terrainVsBody` 在 engine/physics/**（GLOBAL，同上"故意不登记"），
    # 所以这里只登记**不被别人看守**的两处输入：
    #   · runtime/PlayerController.*：出生校正（`attach` 里那次 unbury）在这里
    #   · templates/**：高度图与 `maxHeight` 是本用例的夹具，而且它和
    #     verify_template_scene 读**同一份** —— 两条都登记，模板一变就一起
    #     重跑，才不会在 --narrow 下互相把对方判成"不受影响"
    "verify_terrain_physics": ["templates/**", "tools/gen_templates.py",
                               "runtime/PlayerController.*"],
    # ---- 多块地形 / 描述符池（2026-10-22）----
    # 断言落点两处，都在这里：
    #   · 池链（扩容）      → engine/rhi/DescriptorSet.*（**已是 GLOBAL**，
    #     改它本来就全量；列在这里只是让"改这个文件该跑哪些用例"这件事
    #     在表里能查到，不会造成假跳过 —— 横切判定在 TRIGGERS 之前）
    #   · 首池容量与实例析构 → engine/render/TerrainPass.*
    # 夹具是 templates/third_person（复制成 6 块地形），与 template_scene /
    # terrain_physics 共享同一个 glob —— 三条一起重跑才是对的。
    "verify_multi_terrain": ["engine/render/TerrainPass.*", "templates/**"],
    # ---- 地形值域收敛 / 坏资产救援（2026-10-22 第十一轮）----
    # 断言分散在三层，所以三条都要登记 —— 少一条就会在 --narrow 下把
    # 本用例判成"不受影响"，而它恰恰是唯一看守那条链的用例：
    #   · 收敛数学 + 救援读盘 → engine/terrain/Terrain.*
    #   · 落盘前收敛 + 撤销栈缩放 → editor/LandscapeEditor.*
    #   · 失败不逐帧重建（TDR 那一层）→ engine/render/Renderer.*
    # ⚠ editor/LandscapeEditor.* 也会命中 verify_landscape_* 那几个用例，
    #   那是**对的**：同一个文件改了，两条都该跑。
    "verify_terrain_renorm": ["engine/terrain/Terrain.*",
                              "editor/LandscapeEditor.*",
                              "engine/render/Renderer.*",
                              "engine/render/TerrainPass.*",
                              "templates/**"],
    # ---- 材质仓库 / 地形默认材质 / 出生点落地（2026-10-24）----
    # 断言落在五个地方，一条都不能漏（少一条就在 --narrow 下被误判"不受
    # 影响"，而它是这条链上唯一的用例）：
    #   · 仓库产物与清单        → assets/materials/library/**（新目录）
    #   · 生成脚本              → tools/gen_material_library.py
    #   · 仓库索引 + 面板       → editor/MaterialLibraryPanel.*
    #     （`engine/assets/MaterialLibrary.*` **故意不登记**：`engine/assets/**`
    #      在 GLOBAL_GLOBS 里，写了会变成"看着有归属、其实没覆盖"的假映射）
    #   · 地形默认层 / 同面 / 出生点贴地 → editor/LandscapeEditor.*
    #   · 有地形时栅格退场      → engine/render/Renderer.*（同上，**GLOBAL**）
    #   · 运行期出生点贴地      → runtime/RuntimeApp.cpp
    # ⚠ `editor/ViewportPanel.*` 里是标记的绘制（GAMESTART-MARK 锚点），
    #   但它已经挂在 verify_gamestart_edit 名下；两条都登记，改标记就得
    #   两条一起重跑。
    "verify_material_library": ["assets/materials/library/**",
                                "assets/materials/library.json",
                                "tools/gen_material_library.py",
                                "editor/MaterialLibraryPanel.*",
                                "editor/LandscapeEditor.*",
                                "editor/InspectorPanel.*",
                                "editor/ViewportPanel.*",
                                "runtime/RuntimeApp.cpp",
                                "templates/**"],
    # ---- 材质包目录导入（2026-10-24 第六轮）----
    # 涉及面比看起来宽：引擎侧新增了 `assets/MaterialImport.*`（扫目录 +
    # 解析 glTF + 写 vmat + 合并清单），属性栏多了两行控件，仓库重载要
    # 让缩略图缓存失效。
    #   · INS-MATSRC-* 锚点      → editor/InspectorPanel.*
    #   · 导入后端本身            → engine/assets/MaterialImport.*
    #   · 清单合并 / 目录补扫     → engine/assets/MaterialLibrary.*
    #   · `materialLibraryRevision` → editor/EditorContext.*
    #   · 缩略图失效              → editor/MaterialLibraryPanel.*
    # ⚠ `engine/assets/**` 不在 GLOBAL_GLOBS 里（见本表末尾说明），所以
    #   后端两个文件必须显式写。
    "verify_material_import": ["editor/InspectorPanel.*",
                               "engine/assets/MaterialImport.*",
                               "engine/assets/MaterialLibrary.*",
                               "editor/EditorContext.*",
                               "editor/MaterialLibraryPanel.*"],
    # ---- 自研二进制资产链（阶段六/七，2026-10-22）----
    # 只登记**非横切**的两块，都是新顶层目录：
    #   · assetio/** —— 格式本体（MeshFormat / VertexOps / AnimOps /
    #     FbxImport / Binary）。引擎与离线工具共用，改一个字段偏移就是改契约。
    #   · assetc/**  —— 离线编译器（`AssetC.exe`）。判据里的顶点数 / ACMR /
    #     关键帧数 / 压缩比全由它打印，改它的输出格式这条用例立刻红。
    # ⚠ 刻意**不**登记（`engine/assets/**` 就在 GLOBAL_GLOBS 里，写了在
    #   `--narrow` 下会变成"看着有归属、其实没覆盖"的假映射）：
    #     engine/assets/VMeshLoader.*    —— 运行时 `.vmesh` → `assets::Model`
    #     engine/assets/AssetSelfTest.*  —— `MYVK_ASSET_TEST` 自检的 44 条断言
    #     engine/assets/MeshLoader.*     —— `loadFBX` 先探同名 `.vmesh`
    #     engine/assets/FbxLoader.*      —— FBX 直读那一路（回退路径）
    #   这四块改了**本来就全量**（横切判定在 TRIGGERS 之前），所以安全；
    #   列在这里只是让"改哪个文件该跑哪条用例"在表里查得到。
    # ⚠ `CMakeLists.txt` 同样是横切（新增 `VennAssetIO` / `VennAssetC` 两个
    #   目标就写在那儿），同理不登记。
    "verify_vmesh": ["assetio/**", "assetc/**"],
    # 顶点格式（§13）。触发面比 verify_vmesh **更窄**，这是刻意的：
    #   · `assetio/Vertex.h`        —— 布局本身（kVertexSize / offsetof）
    #   · `assetio/MeshFormat.*`    —— MESH 区块版本与 stride 校验
    #   · `assetio/VertexOps.*`     —— 焊接键：漏一个字段就是把两套 UV2 焊成一个
    #   · `assetio/FbxImport.*`     —— uv2 / tangent 的取值来源
    #   · `engine/assets/VMeshLoader.cpp` —— 按字节直拷 + 布局锁 + VFMT 日志
    #   · `engine/assets/AssetSelfTest.cpp` —— rt_new_channels 往返断言
    # ⚠ **故意不挂**：
    #   · `engine/assets/Mesh.h` —— 属性表与 stride 的来源，但它落在
    #     `GLOBAL_GLOBS`（`engine/assets/**` 是横切），登记在这里属于
    #     "冗余 glob"：写上去会让人以为它只影响本用例，实际影响全量。
    #     `check_regression_map.py` 的 E 组会把这类冗余单独列出来。
    #   · `engine/assets/Mesh.cpp`（程序化几何）/ `Lod.cpp` —— 前者不产生
    #     新通道，后者只做 uv2 平均（B 组看不到差异）。挂了会让 `--narrow`
    #     在每次改立方体生成器时白跑一遍。
    "verify_vertex_format": ["assetio/Vertex.h",
                             "assetio/MeshFormat.*",
                             "assetio/VertexOps.*",
                             "assetio/FbxImport.*",
                             "engine/assets/VMeshLoader.cpp",
                             "engine/assets/AssetSelfTest.cpp"],
    # morph 数据链（§14）的触发面几乎完全覆盖了上一刀 —— 这两条用例读的是
    # 同一批文件的不同侧面（一个是格式、一个是数据），所以故意让它们重叠：
    # 改 `FbxImport.cpp` 的 blend shape 解析会同时影响 uv2/tangent 与 morph，
    # 两个脚本都应该跑。
    # ⚠ 与上一刀同样的**刻意不挂**：
    #     · `engine/assets/Mesh.h` —— 横切（在 GLOBAL_GLOBS 里），写进来
    #       不影响行为，但会让读代码的人以为"改它只影响这条用例"。
    #     · `engine/assets/Mesh.cpp` —— `setMorph` 是纯 setter，没有逻辑。
    "verify_morph": ["assetio/SourceModel.h",
                     "assetio/MeshFormat.*",
                     "assetio/VertexOps.*",
                     "assetio/FbxImport.*",
                     "engine/assets/VMeshLoader.cpp",
                     "engine/assets/AssetSelfTest.cpp"],
    # GPU 形变（§15）的**像素**用例。这里只登记非横切的输入：
    #   · `engine/render/Renderer.*`、`engine/assets/**`、`pbr.vert` 全在
    #     GLOBAL_GLOBS 里 —— 写进来会触发"全横切登记"检查（--narrow 下
    #     假跳过），所以刻意不写（与 verify_morph 同一个理由）。
    #   · `editor/EditorApp.cpp` 挂着 `morph.set` 命令，`InspectorPanel.cpp`
    #     挂着滑条 —— 两者都走 `assets::setMorphWeight`，改任一条这条用例
    #     都该重跑。
    #   · `SceneSerializer.*` / `ModelInstance.*` 负责 MorphComponent 的
    #     存盘与装配：读档丢了组件，视口里就一个滑条都没有。
    "verify_morph_gpu": ["assetio/FbxImport.*",
                         "assetio/MeshFormat.*",
                         "assetio/SourceModel.h",
                         "editor/EditorApp.cpp",
                         "editor/InspectorPanel.cpp",
                         "engine/scene/ModelInstance.*",
                         "engine/scene/SceneSerializer.*"],
    # ---- §16 两骨 IK + 挂点（2026-10-25 第八刀）----
    # 这条用例的证据链是"console 命令 → 组件 → 求解器 → pose → 顶点 → 像素"，
    # 所以上面每一环的**落点**都得登记，否则改中间一环时它会被判成"不受影响"：
    #   · TwoBoneIK.*        求解器本体（余弦定理 / 极向量 / 可达区间）
    #   · BoneAttachment.*   挂点：`jointMat · invBind⁻¹` 反推骨骼世界矩阵
    #   · AnimatorSystem.*   三层的**施加顺序**（LookAt → IK → 弹簧骨）。
    #     顺序错了的表现不是崩，是"走路时脚被头发拽歪"这种没法归因的怪。
    #   · SceneSerializer.*  `ik` / `attach` 两段的读写：读档丢组件 = 视口里
    #     看不出任何异常，但 IK 已经没了（K1 会绿，因为基线也没有）
    #   · InspectorPanel.cpp `drawTabAnimation` 末尾的两段控件
    #   · EditorApp.cpp      `ik.set` / `ik.weight` / `ik.list` / `attach.bind`
    # ⚠ 刻意**不登记** `engine/assets/Skeleton.*`：`findJoint` 是本轮从
    #   Animator/LookAt 抽出来的公共实现，但 `engine/assets/**` 在
    #   `GLOBAL_GLOBS` 里 —— 改它本来就全量，写进来在 `--narrow` 下会假跳过。
    "verify_ik": ["engine/animation/TwoBoneIK.*",
                  "engine/animation/BoneAttachment.*",
                  "engine/animation/AnimatorSystem.*",
                  "engine/scene/SceneSerializer.*",
                  "editor/InspectorPanel.cpp",
                  "editor/EditorApp.cpp"],
    # ---- 阶段 M1：运行时骨架（G01~G05）----
    # 三条用例的实现主体全在 `runtime/**`（G01 拆出来的那个独立目标）。
    # 同一个 glob 挂到三条名下是**故意**的：改 runtime 的任何一行，
    # 三条都该重跑；只挂一条会让 `--narrow` 把另外两条判成"不受影响"
    # （那就是假跳过）。G05 的出厂配置 `assets/game.json` 只被
    # verify_runtime_states 的冷启动断言读，单独挂给它。
    # ⚠ 刻意**不登记**的：
    #     · `CMakeLists.txt` / `engine/core/**` / `engine/physics/**` —— 横切
    #     · `engine/script/**` —— 新目录；改它属"未登记" → 全量 + 点名，安全
    #     · `assets/scenes/sandbox.json` —— 夹具，但 `assets/scenes/**` 按
    #       约定不进快照（编辑器会写它们），故想改夹具时请手动点名这三条
    "verify_runtime_build":  ["runtime/**"],
    "verify_runtime_loop":   ["runtime/**"],
    "verify_runtime_states": ["runtime/**", "assets/game.json"],
    # ---- 阶段 M2 手感地基（G06~G10，2026-10-17）----
    # 只登记**非横切**的实现主体：
    #   runtime/PlayerController.* / runtime/SpringArm.* —— G10 总装，新文件
    #   engine/scene/CameraSystem.*                 —— G09，新文件（不是横切）
    #   engine/scene/SceneSerializer.*              —— 四个新组件的读写在这里
    #     （同一个 glob 也挂在 verify_material_roundtrip / verify_primitive_
    #      roundtrip 名下是**故意**的：序列化一改，三条都该重跑）
    # ⚠ 刻意**不**登记（横切，登记了在 --narrow 下会变假跳过）：
    #     engine/core/InputMap.*（G06）· engine/physics/CharacterController.*
    #     与 ContactEvents.*（G07/G08）· engine/ecs/Components.h（组件）
    #   —— 它们本来就在 GLOBAL_GLOBS 里，改它们必然全量。
    # ⚠ 场景夹具 `assets/scenes/runtime_test/**` 也**不**登记：按约定
    #   `assets/scenes/**` 不进快照（运行时会写它），改夹具请手动点名。
    "verify_runtime_m2": ["runtime/PlayerController.*",
                          "runtime/SpringArm.*",
                          "engine/scene/CameraSystem.*",
                          "engine/scene/SceneSerializer.*"],
    # ---- 阶段 A · E03 设置层级与持久化（2026-10-17）----
    # 覆盖链 / 解析 / 落盘全在 `engine/render/RenderSettingsIO.*`（**不**在
    # GLOBAL_GLOBS 里，所以可以登记）；编辑器侧的 Settings 块与"改过参数
    # 才落盘"在 EditorApp.*（`editor/**` 同样不是横切）。
    # ⚠ 刻意不登记：`engine/render/Renderer.cpp`（横切）、
    #   `engine/render/FeatureRegistry.*`（横切）—— 它们是本用例的接线点，
    #   但登记横切文件会被 check_regression_map 判成"全横切登记 = 假跳过"。
    #   `config/render.json` 是运行期产物、不进版本控制，也无从触发。
    "verify_settings_layers": ["engine/render/RenderSettingsIO.*",
                               "editor/EditorApp.cpp",
                               "editor/EditorApp.h",
                               "editor/main.cpp"],
    # ---- G06 键位重绑定（2026-10-17）----
    # 两个**新文件**（其它用例都不碰 → 登记安全）：落盘规则在 InputMapIO，
    # 面板在 KeyBindingsPage。`EditorApp.cpp` 已由 verify_settings_layers
    # 登记，这里再挂一份（多 owner）：改宿主接线这两条都该跑。
    # ⚠ 刻意**不**登记（否则别的用例会假跳过）：
    #     engine/core/InputMap.*        —— 横切（GLOBAL_GLOBS）
    #     editor/EditorContext.*        —— 十几个用例共用的中枢
    #     runtime/RuntimeApp.cpp / RuntimeConfig.*
    #                                   —— 运行时用例的公共接线
    #   这些走"没登记 = 全量 + 点名"，保守正确。
    "verify_input_bindings": ["editor/InputMapIO.*",
                              "editor/KeyBindingsPage.*",
                              "editor/EditorApp.cpp",
                              "editor/EditorApp.h"],
    # ---- G14 音频（G14-0~G14-9，2026-10-18）----
    # `engine/audio/**` 是**整块新子系统**（不在 GLOBAL_GLOBS 里，也没有别的
    # 用例碰它）→ 整目录登记是安全的，而且改任何一个音频文件都该重跑。
    # 其余是各层的接线点，多数**已经是多 owner**（改一次多处重跑是对的）：
    #   runtime/RuntimeConfig.*      —— audio 块 / `MYVK_RUNTIME_AUDIO_*` 解析
    #   runtime/RuntimeApp.*         —— syncAudio / 暂停时 SFX 静音
    #   runtime/main.cpp             —— `--selftest audio` / `audio-config` 注册
    #   engine/scene/SceneSerializer.* —— audio / audioListener 块的读写
    #   engine/script/ScriptEngine.* —— playSound / stopAllSounds / setBusVolume
    #   editor/InspectorPanel.*      —— Tool 页签的 Audio Source 段
    #   editor/EditorApp.*           —— 编辑器音频门面 + `audio.*` 控制台命令
    #   editor/EditorContext.*       —— requestAudioPreview / takeAudioPreview
    #   editor/ContentBrowser.*      —— 双击试听四态
    #   assets/game.json             —— B2 组读它的 music=0.70（出厂配置）
    # ⚠ 刻意**不**登记 `engine/ecs/Components.h`：`engine/ecs/**` 在
    #   GLOBAL_GLOBS 里，写进来会在 `--narrow` 下变成假跳过。
    # ⚠ `assets/scenes/runtime_test/audio_scene.json` + `audio_play.vks` 也
    #   不登记（`assets/scenes/**` 不进快照），改夹具请手动点名本用例。
    "verify_audio": ["engine/audio/**",
                     "runtime/RuntimeConfig.*",
                     "runtime/RuntimeApp.*",
                     "runtime/main.cpp",
                     "engine/scene/SceneSerializer.*",
                     "engine/script/ScriptEngine.*",
                     "editor/InspectorPanel.*",
                     "editor/EditorApp.*",
                     "editor/EditorContext.*",
                     "editor/ContentBrowser.*",
                     "assets/game.json"],

    # ⚠⚠ `engine/assets/**` **在 GLOBAL_GLOBS 里** —— 材质相关的改动本来就
    #   会点名全量，所以这里**不要**写 Material.h / AssetManager.h（写了在
    #   `--narrow` 下会假跳过）。真正能落在这里的只有下面这几个"不会被
    #   横切清单覆盖、但确实决定本用例成败"的文件。
    "verify_material_instances": ["engine/scene/SceneSerializer.*",
                                  "editor/InspectorPanel.*",
                                  "runtime/main.cpp",
                                  # 夹具本身就是判据（两个 Box 同名不同参数）
                                  "assets/scenes/runtime_test/mat_conflict_probe.json",
                                  "assets/scenes/runtime_test/mat_model_probe.json"],
    # W2-2：sidecar 的判据全在**模型源文件**上（hash / 材质名 / 单位），
    # 换一个模型就得重跑；`.vmeta` 本身是运行时产物（build/bin 下），不进快照。
    # LOD（W2-3 顺带登记）：判据全在**蒙皮模型**上（第 8 组用它验"蒙皮也能
    # 减面"），换模型就得重跑。`engine/assets/Lod.cpp` 是横切，不写进来。
    "verify_lod": ["assets/models/anime_girl.glb"],
    # W3：关卡的判据一半在生成器上（布局改了期望就得跟着改），一半在
    # 母材质上（改 .vmat 会直接改画面）。场景文件本身不进 WATCH_ROOTS
    # （编辑器会写它），所以这里显式点名。
    "verify_first_level": ["tools/gen_first_level.py",
                           "assets/scenes/levels/first_level.json",
                           "assets/materials/*.vmat",
                           "assets/scenes/sandbox.json"],
    "verify_import_vmeta": ["assets/models/anime_girl.glb",
                            "assets/models/anime_girl.fbx",
                            "assets/scenes/runtime_test/mat_model_probe.json"],

    # W4：地形。判据同时落在资产（生成器）、着色器、渲染 pass 与场景夹具上。
    # ⚠ **不登记** `engine/physics/**`、`engine/ecs/**`、
    #   `engine/scene/SceneSerializer.*`、`engine/render/Renderer.*` ——
    #   它们都在 GLOBAL_GLOBS 里（写了在 `--narrow` 下就是假跳过）。
    #   能落在这里的只有"不被横切覆盖、但确实决定本用例成败"的那几个。
    "verify_terrain": ["engine/terrain/**",
                       "engine/render/TerrainPass.*",
                       "assets/shaders/terrain.vert",
                       "assets/shaders/terrain.frag",
                       "assets/terrain/**",
                       "assets/materials/terrain_grass.vmat",
                       "assets/scenes/levels/terrain_probe.json",
                       "tools/gen_terrain.py",
                       "tools/gen_terrain_probe.py"],
    # W4-4：编辑器面。断言的锚点在 ContentBrowser / InspectorPanel 里，
    #   资产那头在 engine/terrain/Terrain.cpp（`loadTerrainDesc` 与加载器
    #   共用同一份校验）。`editor/**` **不在** GLOBAL_GLOBS，所以这里必须
    #   点名 —— 否则改面板时这条用例会被静默跳过。
    "verify_terrain_editor": ["engine/terrain/**",
                              "editor/ContentBrowser.*",
                              "editor/InspectorPanel.*",
                              "assets/terrain/**"],

    # S5：脚本的编辑器面。断言的锚点全在 `editor/EditorApp.cpp` 的
    # Instances 页签与 `engine/script/ScriptRuntime.cpp` 的状态串上。
    # ⚠ 三处**不能**写进来：
    #   · `engine/scene/**` / `engine/ecs/**` —— GLOBAL_GLOBS 里（写了在
    #     `--narrow` 下就是假跳过）；
    #   · `editor/InspectorPanel.*` 已经登记给 terrain_editor 了，但同一个
    #     文件出现在两条 TRIGGERS 里是**允许**的（这表是"文件→用例"，不是
    #     反过来），所以这里照样点名；
    #   · `editor/PanelRegistry.*` 也照点名 —— `MYVK_EDITOR_PANELS` 的 id 表
    #     在那儿，改一个 id 这条用例的 F1 就会全红，而 `editor/**` 不在横切
    #     名单里，不点名就会被静默跳过。
    "verify_script_editor": ["engine/script/**",
                             "editor/EditorApp.*",
                             "editor/EditorContext.*",
                             "editor/InspectorPanel.*",
                             "editor/PanelRegistry.*",
                             "editor/DebugRects.h"],

    # S6：示例关卡的端到端玩法 + 基准。
    # ⚠ `assets/scripts/**` 与 `assets/scenes/**` **不写**进来：它们不在
    #   `WATCH_ROOTS` 里（编辑器运行时会写这两个目录，进了快照就每轮都判成
    #   "变了"）。真正能触发的只有代码：脚本内核、运行时的存盘钩子、
    #   以及把关卡生出来的那个生成器。
    #   顺带一提：改了 `.vks` 之后手动跑这一例即可 —— 增量分析看不到它们，
    #   这是"运行时数据不进快照"的代价，记在这里免得下次当成 bug 查。
    "verify_script": ["engine/script/**",
                      "runtime/RuntimeApp.*",
                      "tools/gen_script_demo.py"],

    # ---- 引擎启动器（docs/launcher_roadmap.md）----
    # 六个用例共用一个新目录 `engine/project/**`（工程文件 / 最近列表 /
    # 显示与画质 / 创建与联接）。**它不是横切文件** —— 只有启动器和编辑器
    # 的 `MYVK_EDITOR_PROJECT` 分支读它，所以必须逐个点名（不点名就是
    # "改完没人跑"）。
    # ⚠ `launcher/**` 只列到文件，不写 `launcher/**`：五个用例对启动器的
    #   依赖面完全不同（ui 看排版、open 看 spawn、code 看联接），写通配等于
    #   把"改一个按钮"放大成"五个 GUI 用例全跑"。
    # ⚠ `engine/core/**`、`editor/EditorContext.*` 之类**故意不写** ——
    #   `engine/core/**` 在 GLOBAL_GLOBS 里（写了在 `--narrow` 下是假跳过）。
    "verify_launcher_project": ["templates/**",
                                "editor/EditorApp.*"],
    "verify_launcher_ui": ["launcher/LauncherApp.*",
                           "launcher/main.cpp",
                           "engine/project/RecentList.*"],
    "verify_launcher_create": ["launcher/LauncherApp.*",
                               "engine/project/ProjectCreate.*",
                               "engine/project/ProjectFile.*",
                               # C9/C10 断言的落点是 `render.json` 的**键数**与
                               # 开关值 —— 所以写盘那一层与预设表都要点名
                               # （E26 之后键数 = 22 enable + 3 数值）。
                               "engine/project/ProjectDisplay.*",
                               "engine/render/QualityPresets.*",
                               "templates/**"],
    "verify_launcher_open": ["launcher/LauncherSpawn.*",
                             "launcher/LauncherApp.*",
                             "engine/project/RecentList.*"],
    # M4：断言的落点在 `render.json` 与 `game.json` 的**回写**上，所以
    # 除了显示层自己，还要点名画质预设表与编辑器的场景钩子。
    "verify_launcher_display": ["launcher/LauncherApp.*",
                                "engine/project/ProjectDisplay.*",
                                "engine/render/QualityPresets.*",
                                "editor/EditorApp.*"],
    # M5：`createDirectoryJunction` 在 ProjectCreate 里，`PROJ-CODE-LINK`
    # 校验在 EditorApp 的 `MYVK_EDITOR_PROJECT` 分支里，热重载在
    # engine/script（已由 verify_script_editor 覆盖，这里也点名）。
    "verify_launcher_code": ["launcher/LauncherApp.*",
                             "engine/project/ProjectCreate.*",
                             "engine/project/ProjectFile.*",
                             "editor/EditorApp.*",
                             "templates/**"],
    # E26：断言的落点在 `QualityPresets`（档位定义）、`RenderSettingsIO`
    # （落盘/读回）、面板本身、编辑器的档位钩子（FeatureUIGen），以及
    # 启动器写画质那一层（ProjectDisplay —— 两个入口必须是同一份数据）。
    # ⚠ `engine/render/QualityPresets.*` 同时登记了 RENDER_MODULES，所以
    #   它还会被"泄漏门"扫一遍（改了它至少有一个优雅退出的用例跑过）。
    "verify_quality_presets": ["engine/render/QualityPresets.*",
                               "engine/render/RenderSettingsIO.*",
                               "editor/QualitySettingsPanel.*",
                               "editor/FeatureUIGen.*",
                               "editor/EditorApp.*",
                               "engine/project/ProjectDisplay.*",
                               "runtime/main.cpp"],

    # ---- 外部模型进场景：尺度 / 第三人称 / 绑骨 / 动画（2026-10-22）----
    # 这四条各管一段，**故意只登记自己那段**：
    #   · `assetio/MeshFormat.*` 不挂在这里 —— 它是格式本体，已由
    #     verify_vmesh 独家看守（改字段偏移两边一起坏，挂多份只会稀释
    #     "谁该为它负责"这件事）。
    #   · `engine/render/Renderer.*`、`engine/assets/**` 是横切，按铁律不登记。
    "verify_model_scale": ["assetio/FbxImport.*",     # 导入/变换烘焙
                           "assetio/VertexOps.*",     # 缩放/居中/翻转
                           "assetio/SourceModel.h",   # 世界 AABB 的定义
                           "editor/EditorScene.*",    # IMPORT-AUTO 落点
                           "engine/scene/ModelInstance.*"],
    "verify_player_character": ["runtime/RuntimeApp.*",       # 挂模型/相机/状态机
                                "runtime/RuntimeConfig.*",    # player 块解析
                                "runtime/PlayerController.*", # velocity()/sprint
                                "engine/scene/SceneSerializer.*",  # gameStart 存读
                                "engine/scene/ModelInstance.*",
                                "engine/animation/**"],
    "verify_autorig": ["assetio/AutoRig.*",
                       # 权重归一化与左右闸门都在 VertexOps 那套工具里
                       "assetio/VertexOps.*",
                       "assetc/main.cpp"],           # --autorig/--strip-skin
    "verify_procanim": ["assetio/ProcAnim.*",
                        "assetc/main.cpp",           # --procanim/--anim-fps
                        # 四态判据（run 阈值 / 冲刺倍率）跑在玩家控制器上
                        "runtime/PlayerController.*",
                        "runtime/RuntimeApp.*"],

    # 角色预览 / 主场景地形并存。⚠ `engine/render/Renderer.cpp` 不在这里 ——
    # 它是 GLOBAL_GLOBS，改它本来就全量重跑。这里登记的是"改了就会让
    # 预览链换一套资源"的几个部件。
    "verify_preview_terrain": ["editor/CharacterWindow.*",    # 预览场景与尺寸
                               "editor/ViewportPanel.*",      # Shift+A 菜单
                               "engine/render/RenderTarget.*",
                               "engine/render/HdrTarget.*",
                               "engine/render/TerrainPass.*"],
}


def _stat_sig(path):
    try:
        st = os.stat(path)
    except OSError:
        return b"<gone>"
    return ("%d|%d" % (st.st_size, st.st_mtime_ns)).encode()


def _stat_pair(path):
    """快照里存的 (大小, mtime_ns)。mtime 会让"只 touch 没改内容"也算变更
    —— 这是**故意**的（保守方向），比漏报安全。"""
    try:
        st = os.stat(path)
    except OSError:
        return None
    return [st.st_size, st.st_mtime_ns]


_LOCAL_IMPORT_RE = re.compile(r"^\s*(?:from|import)\s+([A-Za-z_]\w*)", re.M)


def _local_deps(path, seen):
    """递归收集该脚本 import 的**本地** tools 模块（`from verify_x import ...`）。

    只看本地：`numpy` / `PIL` 之类在 tools/ 下没有同名 .py，自然被过滤掉。
    """
    try:
        with open(path, "r", encoding="utf-8") as fh:
            src = fh.read()
    except OSError:
        return
    for mod in _LOCAL_IMPORT_RE.findall(src):
        sub = os.path.join(TOOLS_DIR, mod + ".py")
        if os.path.exists(sub) and sub not in seen:
            seen.add(sub)
            _local_deps(sub, seen)


def test_fingerprint(name):
    """**单个用例**的指纹：该脚本 + 它（递归）import 的本地 tools 模块。

    为什么要分到这一层：修一个脚本（比如给某个 flaky 断言加重试）不该让
    另外 30 多个用例白跑一遍。
    `run_regression.py` 自己**不参与任何用例的指纹**：它只是"跑法"
    （跳过策略 / 输出格式 / 影响分析表），改了它不影响任何用例的判定结果。
    """
    files = {os.path.join(TOOLS_DIR, name + ".py")}
    _local_deps(os.path.join(TOOLS_DIR, name + ".py"), files)
    h = hashlib.sha1()
    for f in sorted(files):
        h.update(os.path.basename(f).encode())
        h.update(b"|")
        h.update(_stat_sig(f))
        h.update(b"\n")
    return h.hexdigest()


# ---------- 变更影响分析 ----------

_GLOB_CACHE = {}


def _glob_re(pat):
    """把 glob 编译成正则。支持 `**`（跨目录）与 `*`（段内）。

    只用这两种 —— 够表达这张表里的所有条目，也避免 fnmatch 那些
    "`*` 也能跨 `/`" 的意外行为。
    """
    if pat in _GLOB_CACHE:
        return _GLOB_CACHE[pat]
    out, i, n = [], 0, len(pat)
    while i < n:
        c = pat[i]
        if c == "*":
            if pat.startswith("**", i):
                i += 2
                if i < n and pat[i] == "/":
                    i += 1          # `**/` 连零层目录一起匹配
                out.append(".*")
                continue
            out.append("[^/]*")
        elif c == "?":
            out.append("[^/]")
        else:
            out.append(re.escape(c))
        i += 1
    rx = re.compile("^" + "".join(out) + "$")
    _GLOB_CACHE[pat] = rx
    return rx


def _match_any(globs, rel):
    return any(_glob_re(g).match(rel) for g in globs)


def source_snapshot():
    """构建输入文件的 (相对路径 → (大小, mtime_ns))。

    只走 WATCH_ROOTS。文件本身（CMakeLists.txt）与目录都支持。
    """
    snap = {}
    for root in WATCH_ROOTS:
        full = os.path.join(ROOT, root)
        if os.path.isfile(full):
            pair = _stat_pair(full)
            if pair:
                snap[root.replace("\\", "/")] = pair
            continue
        if not os.path.isdir(full):
            continue
        for dirpath, dirnames, filenames in os.walk(full):
            dirnames[:] = [d for d in dirnames if d not in (".git", "build")]
            for fn in filenames:
                p = os.path.join(dirpath, fn)
                rel = os.path.relpath(p, ROOT).replace("\\", "/")
                pair = _stat_pair(p)
                if pair:
                    snap[rel] = pair
    return snap


def diff_snapshot(old, new):
    """→ (新增, 修改, 删除) 三个已排序的相对路径列表。old=None 视为全新。"""
    if not old:
        return sorted(new), [], []
    added = sorted(k for k in new if k not in old)
    removed = sorted(k for k in old if k not in new)
    changed = sorted(k for k in new if k in old and new[k] != old[k])
    return added, changed, removed


def impact_analysis(old_snap, new_snap, narrow=False):
    """把"变更文件集"映射成"该跑的用例集"。

    返回 dict：
      affected : set[str] | None   None = 全量重跑
      reason   : str               给 --explain 与人看的一句话
      lines    : list[str]         逐文件的判定（已格式化）
      unmapped : list[str]         没登记的文件（会触发全量）
      globalh  : list[str]         命中的横切文件
      no_trigger : list[str]       没有 TRIGGERS 登记的用例（无条件重跑）
    """
    added, changed, removed = diff_snapshot(old_snap, new_snap)
    hits = [("+", f) for f in added] + [("~", f) for f in changed] \
        + [("-", f) for f in removed]

    no_trigger = [n for n in DEFAULT if not TRIGGERS.get(n)]

    # 没有基准快照（首次跑 / 缓存格式升级）→ 不做逐文件判定。
    # 否则会把"整个仓库都是新增"当成两千条未登记变更打出来，没有信息量。
    if not old_snap:
        return {
            "affected": None,
            "reason": "没有基准快照（首次分析 / 缓存刚重建）",
            "lines": [], "unmapped": [], "globalh": [], "ignored": [],
            "no_trigger": no_trigger, "hits": [],
        }

    affected, lines = set(), []
    unmapped, globalh, ignored = [], [], []

    for tag, f in hits:
        # ⚠ 顺序很重要：**先判横切**。否则 `IGNORE_SUFFIXES` 里的通配后缀
        #   会把 `CMakeLists.txt` 这类"名字像文档、其实是构建输入"的文件
        #   一起吞掉（`.txt` 就踩过这个坑：CMakeLists.txt 被当成文档忽略 →
        #   改了构建配置却什么都不跑）。
        if _match_any(GLOBAL_GLOBS, f):
            globalh.append(f)
            lines.append("  %s %-52s ⚠ 横切文件 → 全量" % (tag, f))
            continue
        if _match_any(IGNORE_GLOBS, f) or f.endswith(IGNORE_SUFFIXES):
            ignored.append(f)
            lines.append("  %s %-52s 忽略（文档/辅助脚本）" % (tag, f))
            continue
        cases = [c for c, gl in TRIGGERS.items() if _match_any(gl, f)]
        if cases:
            affected.update(cases)
            lines.append("  %s %-52s → %s"
                         % (tag, f, ", ".join(sorted(cases))))
        else:
            unmapped.append(f)
            lines.append("  %s %-52s ⚠ 未登记 → 全量（请补 TRIGGERS）"
                         % (tag, f))

    no_trigger = [n for n in DEFAULT if not TRIGGERS.get(n)]

    if not hits:
        reason = "自上次回归以来源码没有任何变更"
    elif unmapped:
        reason = "%d 个文件没登记归属（保守起见全量）" % len(unmapped)
        affected = None
    elif globalh and not narrow:
        reason = "%d 个横切文件变了（文件粒度看不出改的是哪一段）" % len(globalh)
        affected = None
    elif globalh and narrow:
        reason = "只有横切文件变了，--narrow 下按已登记映射处理"
    else:
        reason = "变更全部落在已登记的映射里"

    return {
        "affected": affected,
        "reason": reason,
        "lines": lines,
        "unmapped": unmapped,
        "globalh": globalh,
        "ignored": ignored,
        "no_trigger": no_trigger,
        "hits": hits,
    }


def stale_build_warning(hits):
    """源码比 Editor.exe 还新 → 大概率忘了重建。早提醒，省下整轮白跑。"""
    exe = os.path.join(BIN_DIR, "Editor.exe")
    try:
        exe_t = os.stat(exe).st_mtime
    except OSError:
        return "找不到 %s（是不是还没构建过？）" % exe
    newest, newest_f = 0.0, ""
    for _tag, f in hits:
        p = os.path.join(ROOT, f.replace("/", os.sep))
        try:
            t = os.stat(p).st_mtime
        except OSError:
            continue
        if t > newest:
            newest, newest_f = t, f
    if newest > exe_t + 1.0:
        return ("`%s` 比 Editor.exe 还新 → 大概率**没重新构建**，"
                "这一轮跑的是旧二进制" % newest_f)
    return ""


def load_cache():
    try:
        with open(CACHE_PATH, "r", encoding="utf-8") as fh:
            data = json.load(fh)
    except (OSError, ValueError):
        return None
    if not isinstance(data, dict) or "src" not in data:
        return None        # 旧格式（bin 指纹）→ 视为没有缓存
    return data


def save_cache(src_snap, passed, tests):
    os.makedirs(LOG_DIR, exist_ok=True)
    tmp = CACHE_PATH + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump({"src": src_snap, "passed": sorted(passed), "tests": tests},
                  fh, indent=1)
    os.replace(tmp, CACHE_PATH)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tests", nargs="*", help="只跑这些（可省 verify_ 前缀）")
    ap.add_argument("--tail", type=int, default=25,
                    help="失败时打印日志尾部行数（默认 25）")
    ap.add_argument("--settle", type=float, default=2.0,
                    help="两个用例之间额外等待的秒数（默认 2）")
    ap.add_argument("--all", "-a", action="store_true",
                    help="无视缓存，全部重跑")
    ap.add_argument("--no-cache", action="store_true",
                    help="不读也不写缓存")
    ap.add_argument("--explain", action="store_true",
                    help="只打印变更影响分析，不实际跑")
    ap.add_argument("--narrow", action="store_true",
                    help="横切文件不触发全量（作者显式断言其它特性不受影响）")
    args = ap.parse_args()

    explicit = bool(args.tests)
    names = args.tests or DEFAULT
    names = [n if n.startswith("verify_") else "verify_" + n for n in names]

    use_cache = not args.no_cache
    cache = load_cache() if use_cache else None
    old_snap = (cache or {}).get("src")
    cur_snap = source_snapshot()

    # ---- 变更影响分析 ----
    imp = impact_analysis(old_snap, cur_snap, narrow=args.narrow)
    show = use_cache and not explicit

    if show or args.explain:
        print("== 变更影响分析 ==")
        if not old_snap:
            print("  （没有可用缓存 → 首次分析，视为全量）")
        elif imp["hits"]:
            print("  变更 %d 个文件（+新增 / ~修改 / -删除）：" % len(imp["hits"]))
            for ln in imp["lines"]:
                print(ln)
        else:
            print("  源码快照与上次一致（无变更）")
        # ⚠ "没登记 TRIGGERS"的用例只在本轮**确实有源码变更**时才无条件重跑。
        #   完全没有变更时它们也不可能受影响（"没变 ⇒ 结果不变"），
        #   所以那一轮可以直接一例都不跑 —— 这是"再跑一遍确认"的常见用法。
        if imp["hits"] and imp["no_trigger"]:
            print("  ⚠ 没登记 TRIGGERS、本轮无条件重跑：%s"
                  % ", ".join(imp["no_trigger"]))
        if imp["affected"] is None:
            print("  → 全量重跑（%s）" % imp["reason"])
        else:
            print("  → 关联用例 %d 个：%s"
                  % (len(imp["affected"]),
                     ", ".join(sorted(imp["affected"])) or "（无）"))
            print("  → 依据：%s" % imp["reason"])
        warn = stale_build_warning(imp["hits"])
        if warn:
            print("  ⚠ %s" % warn)
        print("")

    if args.explain:
        return 0

    # ---- 决定每个用例跳不跳 ----
    # affected is None → 全量（谁都不跳）
    passed_prev, tests_prev = set(), {}
    if cache:
        passed_prev = set(cache.get("passed") or [])
        tests_prev = cache.get("tests") or {}

    cached_ok = set()
    if use_cache and not args.all:
        for n in names:
            if n not in passed_prev:
                continue
            if tests_prev.get(n) != test_fingerprint(n):
                continue          # 用例自己的脚本变了
            if n in imp["no_trigger"] and imp["hits"]:
                continue          # 没登记归属：**有变更时**保守重跑
            if imp["affected"] is not None and n not in imp["affected"]:
                cached_ok.add(n)  # 本次改动跟它无关

    rows, skipped, ran = [], [], 0
    for n in names:
        path = os.path.join(TOOLS_DIR, n + ".py")
        if not os.path.exists(path):
            rows.append((n, None, "脚本不存在", ""))
            print("%-26s %s" % (n, "脚本不存在"))
            continue
        # 明确点名的不跳（"我就要看这个"）
        if (not explicit) and n in cached_ok:
            skipped.append(n)
            print("%-26s %s" % (n, "SKIP  (与本轮改动无关，上次 PASS)"))
            continue
        if ran:   # 让上一个编辑器进程彻底退场、前台焦点回稳
            time.sleep(args.settle)
        t0 = time.time()
        ran += 1
        p = subprocess.run([PY, path], cwd=ROOT, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True,
                           encoding="utf-8", errors="replace")
        out = p.stdout or ""
        ok = False
        for line in out.splitlines():
            if BAD_RE.search(line):
                ok = False
            elif OK_RE.search(line):
                ok = True
        mark = "PASS" if (p.returncode == 0 and ok) else "FAIL"
        rows.append((n, p.returncode, mark, out))
        # 每个用例的**完整**输出都落盘。失败时只打尾部 N 行，光靠它没法定位
        # 问题（比如"手柄目标是碰撞体"挂了，但尾部看到的已经是几行之后的连锁
        # 失败，看不到它前一刻的起点断言）。日志留在 _venn_verify/_regression/。
        os.makedirs(LOG_DIR, exist_ok=True)
        with open(os.path.join(LOG_DIR, n + ".log"), "w",
                  encoding="utf-8") as fh:
            fh.write(out)
        print("%-26s %-4s rc=%-3d %5.1fs" % (n, mark, p.returncode,
                                             time.time() - t0))
        if mark == "FAIL":
            tail = out.splitlines()[-args.tail:]
            for line in tail:
                print("      | " + line)
            print("      * 完整日志: %s"
                  % os.path.join(LOG_DIR, n + ".log"))

    # ---- 缓存回写：本轮跑过的用例按结果更新，没跑到的沿用旧的 ----
    # 只有 PASS 才进缓存（FAIL 会被丢弃 → 下次一定重跑，这正是我们要的）。
    # 快照存**本轮开始前**采集的那一份：它代表"这一轮测的是哪个源码状态"。
    if use_cache:
        keep = set(passed_prev)
        keep_tests = dict(tests_prev)
        if not explicit:
            # 非显式跑（整份 DEFAULT）：顺手清掉已经从清单里移除的用例
            keep &= set(names)
            keep_tests = {k: v for k, v in keep_tests.items() if k in keep}
        for n, _rc, mark, _out in rows:
            if mark == "PASS":
                keep.add(n)
                keep_tests[n] = test_fingerprint(n)
            else:
                keep.discard(n)
                keep_tests.pop(n, None)
        save_cache(cur_snap, keep, keep_tests)

    bad = [r for r in rows if r[2] != "PASS"]
    total = len(rows) + len(skipped)
    done = total - len(bad)
    print("\n%d/%d 通过" % (done, total))
    if skipped:
        print("（其中 %d 例与本轮改动无关，沿用上次 PASS 未重跑：%s）"
              % (len(skipped), ", ".join(skipped)))

    # ---- 横切检查：资源泄漏 ----
    # "谁的日志里有就算谁的" —— 这是**搭车**式检查：只有**优雅退出**的用例
    # 日志里才会出现 `vkDestroyDevice(): ... leaked objects`（强杀进程根本
    # 不产生报告）。所以不做全局扫描，只扫本轮真跑过的用例。
    # verify_collision_play 挂 `engine/render/**` 就是为保证"任何渲染层改动
    # 都至少有一个优雅退出的用例跑过"。
    leaks = [n for n, _rc, _mk, out in rows if out and LEAK_RE.search(out)]
    if leaks:
        print("\n== 横切检查 ==")
        print("  ✗ 资源泄漏：%s 的日志里有 vkDestroyDevice 泄漏报告"
              % ", ".join(leaks))
        print("    （一般是某个 pass 的析构漏了 destroyFrames()/vkDestroy*；"
              "搜日志里的 leaked objects 那行）")
    if bad:
        print("失败：" + ", ".join(r[0] for r in bad))
    return 1 if (bad or leaks) else 0


if __name__ == "__main__":
    sys.exit(main())
