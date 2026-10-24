# 角色表现链（Character Pipeline）现状分析 + 改造路线图

> 2026-10-18 · 对齐 `docs/ship_roadmap.md` 的 **G11 / G12 / G13**
> 这是"补齐一条 3D 游戏链路"系列的**第一篇**（后续：音频、游戏 UI、存档与 GUID、打包各独立成篇）。
> 所有结论都对着磁盘代码核过，行号是 2026-10-18 的版本。
>
> **进度**：**A0 ✅（2026-10-18）/ A1 ✅（2026-10-19）/ A2a ✅（2026-10-22）/
> A2b ✅（crossfade，2026-10-22）** → 下一项 **A3 层与遮罩**。
> A1 的落地记录在 §2.y（含两轮否定测试的实测结果）。
> A2a 的落地记录在 §A2 开头（硬切版移动状态机 + 默认角色 + 按键映射）。
> A2b 只补了 crossfade（0.2 s smoothstep，双采样混合），**没有**做 `.anim.json`
> 控制器资产 —— 那一项并进 A3 一起做（都要"多层/多状态"，一次画完 UI）。
> 运行时侧的手感修复（脚陷地 / 出生点 / 鼠标两态 / 朝向补偿）
> 见 `docs/player_feel.md`。
> **P6（2026-10-22）** 又补了 A2 的两处短板 + A7 的预览视口：Jump 动画重写成
> "双脚离地"、跳跃改成 0.70 m / 0.75 s 的真实抛物线、接触窗口按一帧位移扫掠
> （脚不再陷进地形）、角色面板预览能真的播动画并带栅格/gizmo/可选胶囊。
> **不改上面这个进度指针**（A2 早就 ✅，A3 仍是下一项）——
> 详见 `docs/player_controls.md`。
> ⚠ 下面 §0 的 D0~D8 是**动手前的诊断快照**：D0/D1/D2/D3/D6/D7/D8 都已被 A0/A1 修掉，
> 保留它是为了回答"当初为什么要这么排"。**当前**未修的是 D4/D5（分层 / 驱动层 = A3~A6）。

---

## 0 · 一页结论

**"动画资产 → 骨骼 → 蒙皮渲染"这段是通的，而且质量不低；断的是外面四圈：**
**"角色进不了场景" → "进来了没人推进时间" → "推进了只有单 clip" → "多角色会串姿势"。**

| # | 断点 | 关键证据 | 玩家的感受 | 级别 |
|---|------|----------|-----------|------|
| **D0** | **主场景里根本创建不出 `SkinnedMeshComponent`** | `EditorScene.cpp:283-325` 不判 `model->skinned()`；`CharacterWindow.cpp:113-163` 是唯一创建点且只作用于私有预览场景 | **Game.exe 里永远看不到蒙皮角色** | **P0** |
| **D1** | 没有任何系统推进 `time` | `time` 全仓库唯一写入点 `CharacterWindow.cpp:928`（ImGui 滑条） | 角色定格在第 0 帧 | **P0** |
| D2 | 无 `loop`、无结束语义 | `Components.h:116-126` 只有 `playing`；`sampleTrack` 末尾直接 clamp | 播一次就僵住 | P1 |
| D3 | `jointBase` 以 **mesh 指针** 为键 | `Renderer.h:1441`；`Renderer.cpp:4808-4809` | 三个敌人共用一个姿势 | P1 |
| D4 | `sample()` 三合一 + `thread_local` 静态缓冲 | `Skeleton.cpp:77, 101` | 无法混合 / 无法分层 / 无法 IK | P1 |
| D5 | 驱动层为零（状态机/混合/层/根运动/事件/IK） | `docs/ship_roadmap.md:523-584` 只有规划 | 转身僵硬、滑步、无声 | P1 |
| D6 | 动画字段不进存档 | `SceneSerializer.cpp` 里 `grep SkinnedMeshComponent` **零命中** | 存盘重开回到 T-pose | P1 |
| D7 | 编辑器 Play 的挂载点没接 | `EditorApp.cpp:1066-1085` 顺序里没有动画 | Play = 静态世界 | P1 |
| D8 | 没有 UI 面 | `InspectorPanel.cpp:1708-1769` 只有 3 个控件 | 调不出、看不见、没法调 | P1 |

**一句话**：现在缺的不是"再来一个动画功能"，而是**从资产到屏幕的那根轴还没连上**。
D0/D1/D6 三项加起来约等于"让角色真的出现并动起来"，工作量远小于 D5（驱动层）。

---

## 1 · 已经通了的部分（先说清楚，免得重造轮子）

### 1.1 资产层：两条导入路径产出同一个抽象

- **glTF**：tinygltf → `MeshLoader` → `Skeleton{ joints, invBind, externalChain, nodeMap, clips }`
  （`engine/assets/Skeleton.h:52-86`）
- **FBX**：ufbx → 同一套 `Skeleton`。FBX 没有 glTF 那种 sampler，
  所以导入时**按 30fps 逐帧重采样**成关键帧（`engine/assets/FbxLoader.cpp:380`，`kFps = 30`）
- `AnimTrack` 统一成 T/R/S 三路，R 用四元数 + `slerp`，支持 glTF 的 `STEP` 插值
  （`engine/assets/Skeleton.cpp:42-66`）
- 上层（`updateSkinning`）**不需要知道格式**——这个抽象是对的，别动它

### 1.2 GPU 蒙皮：接得比预想的完整

- `assets/shaders/pbr.vert:39-44`：`jointBuf` = SSBO of `mat4`，所有蒙皮模型共享一块大 buffer
- `:116-130`：四权重线性混合位置与法线（`skinMat` 一次算、位置法线共用）
- `jointBase` 有**三路来源**，全部已实现：MDI 实例记录 `aux.x` / 合批 `push.skin.x` / 基线 `push.skin.x`
- 空间推导写得非常清楚（`Skeleton.h` 头注释）：`invMeshNode` 与 `push.model` 里的
  `meshNodeWorld` 恰好约掉 → **静态网格与蒙皮网格共用同一条 draw 代码**，只多一个 `jointBase`

### 1.3 采样数学：标准做法，正确

`Skeleton::sample`（`Skeleton.cpp:69-125`）三段：
1. 每根骨骼从 `restLocal` 分解出 TRS，再用轨道**覆盖分量**（T/R/S 三条轨道正确叠加，不会互相盖掉）
2. 沿 `topoOrder` 累乘世界矩阵（保证 parent 先于 child）
3. `joint = invMeshNode * world * invBind`

**结论：1.1 ~ 1.3 不需要改。** 问题全在"谁调用它、什么时候调用、能不能调用两次"。

---

## 2 · 九个断点，逐条

分三类：**进不来**（D0 / D6）、**不动**（D1 / D2 / D7）、**做不好**（D3 / D4 / D5 / D8）。

### D0 · 角色进不了场景（P0，最致命，且最容易被误判为"已经有了"）

三条独立证据，缺一不可：

1. `editor/CharacterWindow.cpp:113-163` 的 `instanceModelInto` 是全仓库**唯一**创建
   `SkinnedMeshComponent` 的地方（`grep "add<ecs::SkinnedMeshComponent>"` 只有这一处）。
   而它是 `CharacterWindow` 的私有成员，作用对象是 `m_previewScene` —— 一个**独立的预览场景**。
2. `editor/EditorScene.cpp:283-325`（Content 拖拽 / 导入的唯一落地函数 `instantiateModel`）：
   只 `attachRenderable(MeshComponent + MaterialComponent)`，**从不判断 `model->skinned()`**。
   所以把一个带骨骼的 FBX 拖进视口，得到的是**一堆静态网格摆成 T-pose**。
3. `engine/scene/SceneSerializer.cpp`：`grep SkinnedMeshComponent` → **零命中**。
   存档不写、读档不建。而 Game.exe 走的是同一个 `SceneSerializer::load`。

→ 三条叠加的结论：**主场景里任何加载途径都得不到 `SkinnedMeshComponent`；
Game.exe 永远显示不出蒙皮角色。** 你在 CharacterWindow 里看到角色能动，是因为那是另一条路。

> 这一条不改，后面全部无意义。它是 A0 的第一项。

### D1 · 时间无人推进（P0）

- `time` 的唯一写入点：`editor/CharacterWindow.cpp:928` `sk->time = m_animTime;`，
  `m_animTime` 由 ImGui 滑条 / 播放按钮驱动（`:699`）
- `grep "\.time\s*+="` 全仓库**零命中**
- `Renderer.cpp:4785` 只是 `skel.sample(clip, sk.time, scratch)` —— 采样器是**纯函数**，不推进时间

→ `time` 恒为 0（或用户手改）→ 角色永远定格在第 0 帧。
这也是"断言动画在动"这件事在过去无法自动化的根因：**没有时间轴就没有可断言的量。**

### D2 · 没有 loop、没有结束语义（P1）

- `Components.h:116-126` 只有 `model / clipName / time / speed / playing`，**没有 `loop`**
- `sampleTrack` 在 `time >= times.back()` 直接返回末值 → 播完停在最后一帧
- 加时间的人必须自己 `fmod`，而且 clip 时长得从 `findClip(name)->duration` 现查

→ 接上时间后角色会"播一次然后僵住"，看起来像一个更诡异的 bug。

### D3 · `jointBase` 以 mesh 指针为键 → 多实例串姿势（P1，做游戏必踩）

- `Renderer.h:1441`：`std::unordered_map<const assets::Mesh*, int> m_skinJointBase`
- `updateSkinning`（`Renderer.cpp:4808-4809`）对每个实体，把它 model 的**所有 subMesh 指针**
  映射到该实体的 `jointBase`
- 两个敌人用同一个 FBX → `AssetManager` 缓存 `unordered_map<string, unique_ptr<Model>>`
  （`AssetManager.h:116`）→ `Model*` 与它下面的 `Mesh*` **完全相同** → 第二个实体覆盖第一个的条目

→ 三个敌人共享一个 `jointBase` → **全部显示同一个姿势**（后写的赢）。
现在只有一个角色所以没暴露；只要场景里放第二个敌人就立刻现形。

### D4 · `sample()` 三合一 + `thread_local` 静态缓冲（P1，架构级）

- `Skeleton::sample` 一次做完"采轨道 → 拼世界 → 乘 invBind"，**中间没有任何可插入点**
- `Skeleton.cpp:77` `static thread_local std::vector<TRS> local;`
  `Skeleton.cpp:101` `static thread_local std::vector<glm::mat4> world;`

→ 四个直接后果：
1. **无法 crossfade**（需要两份 pose 再混合）
2. **无法分层**（每层一份 pose）
3. **无法 IK**（要在"局部旋转"或"世界矩阵已算、invBind 未乘"上改）
4. **嵌套调用会互相踩静态缓冲**（A 层采样里调 B 层 → 缓冲被覆盖）

> 修法：拆成 `samplePose(clip → Pose)` 与 `poseToJoints(Pose → mat4[])`，中间全部显式传缓冲。
> 这个拆分是 A2/A3/A6 的**共同前置**，不做就得每次重写一遍。

### D5 · 驱动层为零（P1，工作量主体）

无 `AnimatorComponent`、无状态机、无参数、无 crossfade、无层/遮罩、无根运动、无事件、无 IK。

`docs/ship_roadmap.md:523-584` 已经有 G11/G12/G13 的规划，方向对，但**缺三块**：
1. **采样从渲染层剥离**（现在采样在 `Renderer::updateSkinning` 里 → 无窗口测不了）
2. **动画资产怎么表达与落盘**（控制器 JSON 的 schema、事件存哪、序列化契约）
3. **Animator 面板**（项目准入纪律要求"UI 面"，没有面板 = 不算交付）

### D6 · 动画字段不进存档（P0/P1）

- 整个组件都不存 → `clipName / speed / playing` 全丢
- 后果：摆好一个会动的角色，存盘、重开 → 回到 T-pose，还得重新在 Inspector 里选 clip

### D7 · 编辑器 Play 的挂载点没接（P1）

- `EditorContext.h:15-42` 的设计写得很清楚：运行态副本 `m_runtimeScene`，
  "脚本、**动画**全部只在这份副本上跑"。设计是对的。
- `EditorApp.cpp:1063` 的注释却写"动画跑在编辑场景上" —— **与设计矛盾**，
  实际是两者都还没有动画系统（这条注释要随手改掉）
- `EditorApp.cpp:1066-1085` Play 顺序：`advancePlayTime → 脚本 → 玩家 → 物理 → 碰撞`，
  **没有动画的位置**
- `runtime/RuntimeApp.cpp` 的 `simulate()` 同一顺序，也没有

→ Play 按钮目前只是"让物理动起来"，世界在动画层面是死的。

### D8 · 没有 UI 面（P1，准入纪律要求）

`InspectorPanel.cpp:1708-1769` 的 Animation 页签只有三个控件：clip 下拉、Speed、Playing。
没有时间轴、没有当前状态、没有参数、没有事件列表。

按项目五件套准入（实现 / 注册 / UI 面 / 序列化 / 用例），**现在的动画连"半件"都算不上**。

---

## 3 · 目标架构

### 3.1 数据流（一条主链，五个插槽）

```
controller.json ──┐
clip (in Model) ──┼──→ [状态机: 参数/转移] ──→ 当前 clip + 权重
AnimatorComponent ┘         │
                            ▼
                    samplePose(clipA, tA) → PoseA
                    samplePose(clipB, tB) → PoseB      ← 两状态 crossfade
                            │
                            ▼
                    blendPose(A, B, w) → Pose          ← ① 混合（local TRS 空间）
                            │
                            ▼
                    分层：Base 结果 + Upper 覆盖遮罩骨骼 ← ② 层与遮罩
                            │
                            ▼
                    两骨 IK / FABRIK                    ← ③ IK（混合之后）
                            │
                            ▼
                    root motion 提取 → TransformComponent ← ④ 根运动（写世界矩阵之前）
                            │
                            ▼
                    poseToJoints: topo 累乘 → invBind    ← 局部 → 世界 → joint
                            │
                            ▼
              sk.jointMatrices ──→ Renderer 拷进 SSBO（只做搬运）
```

### 3.2 采样后处理顺序（写死，别乱）

```
采样 → 混合 → 遮罩 → IK → 根运动提取 → 局部转世界 → 乘 invBind → 上传
```

三条**硬性约束**，每条都对应一个肉眼可见的 bug：

| 约束 | 违反后的症状 |
|------|-------------|
| 混合必须在 **local TRS** 空间 | 世界空间混合 → 子骨骼被双重累积 → 角色"越混越扭" |
| IK 必须在**混合之后** | IK 结果被 blend 抹平 → 斜坡上脚还是在穿地 |
| 根运动提取必须在**写世界矩阵之前** | 位移被蒙皮再算一次 → 角色位移翻倍（像在滑冰） |

### 3.3 职责分工（这是本次改造最重要的一张表）

| 层 | 只做这些 | 绝对不许做 |
|----|---------|-----------|
| `Renderer::updateSkinning` | 分配 `jointBase`、把 `jointMatrices` 拷进 SSBO、绑描述符 | 采样、推进时间、混合、查 clip |
| `engine::animation::AnimatorSystem` | 推进 time、跑状态机、采样、混合、遮罩、IK、根运动 | 碰任何 `VkBuffer` / 描述符 |
| `ecs::AnimatorComponent` | 存"怎么播"：控制器路径、参数值、层权重、根运动模式 | 存骨架、存关节矩阵 |
| `ecs::SkinnedMeshComponent` | 存"骨架是谁"、"关节矩阵输出在哪" | 存播放逻辑 |

**这样拆的直接收益**：`MYVK_ANIM_TEST=1` 可以**不开窗口**跑完整状态机 + 数值断言关节矩阵。
（现在采样埋在 Renderer 里，任何动画逻辑都必须开 GUI 才能测 → 这正是它至今没有回归用例的原因。）

### 3.4 关键设计决策

| 决策点 | 选择 | 理由 | 被否掉的方案 |
|--------|------|------|-------------|
| 新组件 or 扩字段 | **新增 `AnimatorComponent`** | `SkinnedMeshComponent` 要能被"无动画的骨架摆件"复用；一实体多动画槽是分层的前提 | 往 `SkinnedMeshComponent` 塞 15 个字段 → 序列化 / Inspector / 校验全纠缠 |
| 状态机存哪 | 独立 JSON 资产 `assets/animators/*.anim.json`，**自己加载，不进 `AssetManager`** | `engine/assets/**` 在 `GLOBAL_GLOBS`（横切）；把动画域的东西留在动画域 | 塞进 `Model` / 塞进场景 JSON（一个角色一套，无法共享） |
| 事件存哪 | **控制器资产里按 clip 名索引** | clip 是**共享资产**，改它会污染所有用它的角色；而"同一个 Walk，胖角色和瘦角色脚步帧本来就不同" | 给 `AnimationClip` 加 `events[]` → 要碰 `engine/assets/**` + 语义错位 |
| 混合空间 | local TRS | 正确性（见 3.2） | 直接 `lerp(mat4)` → 会插出缩放与剪切，肉眼是"角色变瘦" |
| 采样位置 | 从 Renderer 剥离到 `engine/animation/` | 无窗口自检、职责单一、将来可并行 | 继续留在 `updateSkinning`（等于把测试能力焊死） |
| `jointBase` 键 | **逐 `DrawItem`** | 同 Model 多实例必须各自一份 | 按 mesh 指针（现状，多实例串姿势） |
| 默认行为 | **没有 `AnimatorComponent` = 走旧路径，逐像素不变** | 项目硬约束"特性默认关 = 逐像素一致" | 默认开状态机 → 所有现有场景基线全飘，全量回归无从比较 |
| 引入第三方？ | **不引** | 手写状态机 ~800 行；Assimp 的动画层解决不了状态机，Unity/UE 的也不能直接用 | 引 ozz-animation（解决采样，不解决状态机，还多一份 ABI） |

---

## 4 · 任务路线图

规模刻度：`S` < 半天 · `M` ≈ 1 天 · `L` ≈ 2~3 天 · `XL` > 3 天
（参照：R6-1 GPU 粒子整链 ≈ `XL`）

---

### A0 · 地基：让角色进得来、存得住、动起来（P0）

> **这一阶段做完，Game.exe 里就能看到一个会走路的角色** —— 只有一个 clip、没有过渡，但它是活的。
> ⚠ 本阶段碰三个横切文件（`Renderer.h`、`engine/ecs/Components.h`、`Skeleton.cpp`），
> **攒在一起改完、只跑一次全量回归**。拆成五次提交 = 跑五次全量。

#### A0-1 · 抽取模型实例化的公共入口（`M`）

- 新建 `engine/scene/ModelInstance.{h,cpp}`：把 `CharacterWindow::instanceModelInto`
  （`editor/CharacterWindow.cpp:113-163`）搬过去，并补上缺失的那一段：

```cpp
if (model->skinned() && parent.valid()) {
    ecs::SkinnedMeshComponent sk;
    sk.model    = model;
    sk.playing  = true;      // ⚠ 预览场景传 false，主场景传 true
    // ...
}
```

- 三个调用方全部改走它：`EditorScene.cpp:283-325`（拖拽导入）、
  `CharacterWindow.cpp`（预览，删掉私有副本）
- **验收**：把 `assets/models/anime_girl.fbx` 拖进视口 → 层级里出现根节点 + 子网格；
  Inspector 切到 Animation 页签**不再空白**（`sk` 非空，能看到 clip 下拉有内容）
- **锚点**：`MODEL-INSTANCE path=.. root=.. meshes=.. skinned=1`

#### A0-2 · 序列化（`S`）

- `SceneSerializer` 增加 `"skinned": { "modelKey", "clipName", "time", "speed", "playing" }`
- ⚠ 重建顺序：必须在 `resolveMesh()` **之后**（那时 model 已加载，能拿到 `skeleton` 判 `skinned()`）
- ⚠ **老场景没有这个键 → 不建组件 → 渲染结果逐像素等于今天**（这是硬约束，别默认建）
- **验收**：选好 clip 存盘 → 重启 → Animation 页签里 clip 还是那个；`speed/playing` 也一致
- **锚点**：`SCENE-SKINNED entity=.. clip=.. speed=.. playing=..`（存 + 读各一条）

#### A0-3 · 时间推进（最小可用版，`M`）

- 新建 `engine/animation/AnimatorSystem.{h,cpp}`：
  `void tick(scene::Scene& scene, float dt);`
  ```
  for each SkinnedMeshComponent sk:
      if (!sk.playing) continue;
      const float dur = resolveDuration(sk);          // findClip(clipName)->duration
      sk.time += dt * sk.speed;
      if (sk.loop && dur > 0) sk.time = fmod(sk.time, dur);
      else                    sk.time = min(sk.time, dur);
  ```
- `SkinnedMeshComponent` 加 `bool loop = true;`（一次加完，见 A1 的字段总表）
- 挂载点（**两处，顺序一致**）：
  - `editor/EditorApp.cpp:1066` 的 Play 分支，放在 `stepPhysics` **之前**
    （根运动是物理的输入 → 将来 A4 不用挪位置）
  - `runtime/RuntimeApp.cpp` 的 `simulate()` 同位置
- 顺手改掉 `EditorApp.cpp:1063` 那句过期注释（"动画跑在编辑场景上"）
- **验收**（这是整条链第一个可自动化的量）：
  - `MYVK_ANIM_TEST=1` 无窗口：喂 120 帧 `dt=1/60` → 断言 `time ≈ 2.0`（容差 1e-4）
  - `loop=true, dur=1.2` → 喂 3 秒 → `time ≈ 0.6`
  - `loop=false` → 喂 3 秒 → `time == dur`（逐位）
- **锚点**：`ANIM-TICK f=%llu entities=%d`（每 60 帧）、`ANIM-WRAP clip=.. t=.. dur=..`

#### A0-4 · `jointBase` 改为逐绘制项（`M`，多角色前置）

- `Renderer.h:921` `DrawItem` 加 `int32_t jointBase = -1;`
- `updateSkinning` 不再写 `m_skinJointBase`，改为：算完把 `sk.jointBase` 写回**组件**
- `buildDrawLists` 建项时 `item.jointBase = sk ? sk->jointBase : -1`
- 三条绘制路径（基线 / 合批 / MDI）改读 `item.jointBase`；
  `ShadowPass.h:110` / `LocalShadowPass.h:110` 的参数从 `const map*` 改成"逐 item 读"
- 删掉 `m_skinJointBase`
- **验收**（否定测试必须有）：同一 FBX 实例化 3 次、给三个不同 `time` →
  三条 `ANIM-SKIN` 的 `jointBase` **两两不等** + 截图三个姿势不同；
  退回旧实现时该断言必须变红
- **锚点**：`ANIM-SKIN entity=%u jointBase=%d time=%.3f`
- ⚠ 碰 `Renderer.h`（GLOBAL_GLOBS）→ 全量

#### A0-5 · 采样剥离（`M`，A2/A3/A6 的共同前置）

- 新建 `engine/animation/Pose.{h,cpp}`：
  ```cpp
  struct Pose {                       // 每根骨骼的局部 TRS，长度 = jointCount
      std::vector<glm::vec3> translation;
      std::vector<glm::quat> rotation;
      std::vector<glm::vec3> scale;
      void resize(int n);
      void setRest(const assets::Skeleton& s);
  };
  void samplePose(const assets::Skeleton&, const assets::AnimationClip*,
                  float time, Pose& out);          // clip=nullptr → rest
  void poseToJoints(const assets::Skeleton&, const Pose&,
                    std::vector<glm::mat4>& out);
  void blendPose(const Pose& a, const Pose& b, float w, Pose& out);
  ```
- ⚠ **不要**保留 `Skeleton.cpp` 里那两个 `thread_local` 缓冲 —— 它们正是"嵌套调用互相踩"的来源
- `Skeleton::sample` 保留为兼容壳：内部 = `samplePose + poseToJoints`
  （CharacterWindow 和现有断言还在用它）
- `SkinnedMeshComponent` 加 `std::vector<glm::mat4> jointMatrices;`（输出缓冲，避免每帧分配）
- `updateSkinning` 只剩"取 `sk.jointMatrices` → memcpy 进 SSBO"（`Renderer.cpp:4775-4812` 大幅缩水）
- **验收**（重构不变性断言，最硬的回归保护）：
  对同一 clip、同一 `t`，`sample()` 与 `samplePose→poseToJoints` 产出的关节矩阵
  **逐位相同**（`memcmp` 级）
- ⚠ 碰 `engine/assets/**` → 全量

**A0 总验收（一句话场景）**：`Game.exe` 打开沙盒 → 有个角色真的在走（关节在动）；
退出重开 → 它还在走，姿势/速度不变。
**A0 规模：`L`**（约 R6-1 的 1.2 倍）

### 2.x · A0 实施记录（2026-10-18，两处**原计划写错了**）

**① "蒙皮网格不能做 LOD" 的理由不是 `jointBase`。**
`engine/assets/Lod.h` 原来写着"因为 jointBase 按 mesh 存，减面后对不上"。查实
后是另一回事：`buildDecimatedLods` 只合并 **位置 / 法线 / UV**，**不处理
`Vertex::joints` / `weights`** —— 减面会直接丢权重，蒙皮必然烂。`jointBase`
跟 LOD 无关（它只决定 SSBO 里的偏移）。`AssetManager::meshLooksSkinned` 让这条
链停在 1 级，理由已改。**结论：写"因为 X 所以 Y"之前先读 X 的实现。**

> **2026-10-24 补记：结论回到"停在 1 级"，但理由换了第二次。**
> W2-3 之后 `decimateClustered` 真的能安全减面了（按主 joint 分组 + 权重归一化，
> `--selftest lod` 的 B/C 组钉着），于是当时把链打开了。结果用户看到的是
> **"脸部变成一个空壳、眼睛消失、腿不见了、鞋子悬空"**。查下来是两件与
> joints 无关的事：
>
> * **选层口径错**：LOD 按**每个子网格自己的包围球**算屏幕占高比。脸只有
>   19 cm，第三人称 4 m 外比值 ≈ 0.06 → 直接落到第 3 级，1571 顶点被压到 ~100。
> * **UV 被平均**：脸/眼/眉是同一张图集上切开的小岛，聚类把一格的 UV 求平均后
>   采样点落到图集空白处 → 脸是一块纯白、眼睛眉毛全没。
>
> 附带查出一个**静默丢数据**的硬 bug：`lodKey` 只用 `MeshSource::name` 拼键，
> 而那是**模型**的 key —— 一个 FBX 的二十个子网格全撞在 `alicia#lod1` 上，
> `registerMesh` 命中缓存后把 19 个子网格的减面结果直接扔掉（症状同上：那 19 个
> 子网格换上了别人的低模）。修法是键里加 `subMeshIndex`（`--selftest lod` E 组）。
>
> 但**修完键撞车，脸依然是白板** —— 说明①②两条才是主因。所以最终处置是：
> 蒙皮网格一律不建链（`LOD-SKIP-SKIN`），并保留 `MYVK_LOD_OFF=1` 作为
> "渲染异常时先排除 LOD"的 A/B 开关。与 UE5 对齐：骨骼网格体 LOD 走专门管线
> （按 Model 统一选层 + 手动/工具生成），**不是顶点聚类能凑的**。

**② 像素断言的口径：整图均值会被稀释到毫无判别力。**
实测（1418×844，角色占屏约 4%）：

| 对照 | 整图平均差 | 显著变化像素占比（视口内，阈值 8） |
|------|-----------|--------------------------------|
| Idle `t=0` vs `t=0.5`（同 clip 两个时间） | 0.02/255 | ~0.1% |
| Walk `t=0` vs `t=0.35`（换 clip） | **0.41/255** | **2.48%** |
| Walk `t=0` vs `t=0`（同配置跑两遍） | 0.00/255 | **0.000%** |

- Idle 是坐姿呼吸，半秒内胸腔起伏不到一个像素 → **同一 clip 里钉两个时间也没用**，
  必须换 clip（anime_girl 带 `Idle` / `Walk` / `Rig|Idle` / `Rig|Walk`）。
- 0.41/255 的"整图均值"看着像噪声，实际是"站着"和"跪着"的区别 —— 所以判据改成
  **显著变化像素占比**（`ImageChops.difference` → 直方图），并且同配置跑两遍拿到
  **抖动基线**再自校准（实测 0.000%，阈值取"占比 > 0.5% 且 > 10× 基线"）。
  这不是给某个模型调参：换成任何占屏比例不同的资产都成立。

**③ 顺带补的自动化设施**（都是为了"能无人值守地钉住一个姿势"）：
- `MYVK_EDITOR_IMPORT` 支持 `;` 分隔多路径（第 i 个沿 +X 排 1.5 m）→ 多实例一次起完
- `MYVK_EDITOR_CONSOLE_CMD` 支持 `;` 分隔多命令（"先切 clip 再钉时间"要两刀）
- `anim.time <秒> [实体名]` / `anim.clip <名字> [实体名]` 两个控制台命令
- `EditorScene::instantiateModel` 对重名根节点加 `_1` / `_2` 后缀（否则 `Scene::find`
  只命中第一个，多实例断言会退化成"只改到第一个实例"的假绿）

**④ 断言侧两个坑（都已写进代码注释）**：
- 自检行 `ANIM-TEST ... detail=..` 的 detail **不能含空格** —— 脚本按
  `detail=(\S*)` 解析，一个空格就让整行失配、断言在脚本眼里"不存在"。现在 detail
  用逗号分隔，脚本另外加了一条"有 `ANIM-TEST` 前缀就必须被正则吃掉"的守卫。
- `jointBase` 进**排序键**是 A0-4 的隐藏前提：批内只推一次 `push.skin.x`，两个
  同 mesh、不同 `jointBase` 的实例混进一批 → 共用一个姿势（改完 `DrawItem` 忘了
  改 `buildOpaqueBatches` 就是这个症状）。

**否定测试实测**（两处回退都必须变红，已验）：
| 回退 | 症状 |
|------|------|
| `updateSkinning` 里 `sk.jointBase = 0`（模拟按 `Mesh*` 共享） | F 组 `jointBase 两两不等（0,0,0）` FAIL |
| `AnimatorSystem` 里 `samplePose(skel, clip, 0.0f, ...)`（忽略 time） | E 组 `变化占比 0.000% > 0.50%` FAIL |

---

### A1 · `AnimatorComponent` 上线（`M`） ✅ 已完成（2026-10-19）

**目标**：`time` 的权威从 UI 挪到系统；为混合准备好"多份 pose"。

- `ecs::AnimatorComponent`（**字段一次加全，别分批** —— 每批都要重新跑全量）：
  ```cpp
  struct AnimatorComponent {
      assets::Model* model = nullptr;   // 空 → 回退用 SkinnedMeshComponent.model
      std::string controllerPath;       // 空 = 单 clip 直放模式
      std::string defaultClip;
      float time = 0.0f, speed = 1.0f;
      bool playing = true, loop = true;
      // 根运动（A4 用，先占位）
      enum class RootMotionMode : int { Off = 0, FromClip = 1, DeltaOnly = 2 };
      RootMotionMode rootMotion = RootMotionMode::Off;
      // 运行时（不序列化）
      int stateIndex = -1;
      float stateTime = 0.0f;
      // 参数黑板：float/bool/trigger 三张表（A2 用）
  };
  ```
- `AnimatorSystem::tick` 走新路径：`samplePose → poseToJoints → sk.jointMatrices`
- **没有 `AnimatorComponent` 的实体走旧路径** → 逐像素不变
- Inspector 的 Animation 页签补：时间轴滑条 + `time/duration` 文本 + `loop` 复选框
  （交互照抄 `CharacterWindow::drawTransportBar`，`editor/CharacterWindow.cpp:662-711`）
- **验收**：Inspector 拖时间轴 → 视口姿势跟着变（两帧截图不同）；
  存盘重开时间轴位置还在

#### 2.y · A1 实施记录（2026-10-19）

**交付物**

| 文件 | 改动 |
|---|---|
| `engine/ecs/Components.h` | **新增 `AnimatorComponent`**（字段一次加全：model / controllerPath / defaultClip / time / speed / playing / loop / rootMotion + 三个运行时字段与三张参数黑板） |
| `engine/animation/AnimatorSystem.{h,cpp}` | 两条路径分叉（有 / 没有 animator），加 `lastAnimatorCount()`；新增锚点 `ANIM-CLIP-FROM-DEFAULT` / `ANIM-ANIMATOR` / `ANIM-MODEL-MISMATCH` |
| `engine/scene/SceneSerializer.cpp` | `animator` 块读写；顺手把 `kind/path/modelKey` 的拼装抽成 `modelSourceToJson` / `modelFromSource`，skinned 与 animator 共用（两处各写一份 kind 判断，迟早有一处漏掉 glTF） |
| `editor/InspectorPanel.{h,cpp}` | 时间轴 / speed / playing / loop 改绑**权威字段**；新增 `INS-ANIM-TIMETEXT`（time / duration 文本）与 Animator 段（state / controller / defaultClip / rootMotion + `Add Animator`）；Add Component 菜单加 `Animator`（只对已挂蒙皮的实体可用） |
| `editor/EditorApp.cpp` | `anim.time` / `anim.clip` 写 anim 那一份，日志加 `via=anim\|sk` |
| `engine/animation/AnimationSelfTest.cpp` | +11 条（第 13~19 组） |
| `engine/i18n/ZhTable.cpp` | +13 条（词典 1721） |
| `tools/verify_character_import.py` | 新增 [G] 组（G1 UI 面 / G2 写盘 / G2b JSON / G3 重开 + 像素） |
| `tools/verify_anim_runtime.py` | 期望清单 +11；把写死的"16 项"改成 `len(EXPECTED)` |

**权威关系（写进两个文件的注释，因为它是这次唯一容易搞反的东西）**

- 没有 `AnimatorComponent` → `SkinnedMeshComponent` 的 time/speed/playing/loop 是权威（A0 老路径，逐位不变）。
- 有 → **`AnimatorComponent` 的四项是权威**，`AnimatorSystem` 每帧把它们**镜像**回 sk。
  镜像不是可有可无：`Renderer::updateSkinning` 的兜底采样、`ANIM-SKIN` 日志、控制台、截图断言全吃 `sk.time`。
  **镜像的写入点只有系统一处**；Inspector / 控制台都写权威那一边。

**三个刻意的取舍**

1. **`AnimatorComponent::model` 只用于"sk 没给"的情况**，两处都给且不同时打一条 `ANIM-MODEL-MISMATCH`。
   自动挑一个更糟：采样骨架与网格骨架不一致时，关节数一样连越界都没有，画面只是"对不上"。
2. **`defaultClip` 只补一次**（`appliedDefaultClip` 闸门）。没有它，用户清空 clipName 想看待机姿势时会被系统每帧填回去 ——
   Inspector 上"empty = rest pose"那句提示就成了谎话。
3. **不加没人用的 `m_poseB`**。A2 的 crossfade 才需要第二份采样缓冲；A1 加一个空字段，后来的人会读成"混合已经支持了"。
   A1 准备好的是结构（采样 / 推进 / 输出全在一个系统里），A2 只需在 `tick` 里加第二次 `samplePose` + `blendPose`。

**验收（`verify_anim_runtime.py` 27 条 + `verify_character_import.py` [G] 组）**

```
A1 新增 11 条数值断言（全部 PASS，ANIM-TOTAL pass=27 fail=0）
  animator_authoritative   有人仍写 sk.time → 被镜像回 anim.time（dt=0 下可判）
  animator_count           系统认出了走 Animator 路径的实体
  animator_advance         120 帧 @1/60 → anim.time = 2.000
  animator_mirror          sk.time 是 anim.time 的镜像
  animator_paused          anim.playing=false 压过 sk.playing=true
  animator_speed           anim.speed=2 压过 sk.speed=1
  animator_speed_mirror    speed 也镜像
  animator_default_clip    空 clipName → 补 defaultClip
  animator_default_once    补过之后清空就真的空着
  animator_model_fallback  anim.model 空 → 用 sk.model
  animator_joint_bitwise   Animator 路径的关节矩阵与参考实现 memcmp 级相同
```

**否定测试（两轮，都必须红）**

| 注入的回退 | 变红的断言 |
|---|---|
| `time` 改回永远取 `sk.time` + 去掉镜像 | `animator_authoritative`(99≠0) / `animator_advance`(0≠2) / `animator_speed` / `animator_speed_mirror` / `animator_joint_bitwise` —— **5 红** |
| `playing` 改回取 `sk.playing` + 取消 `appliedDefaultClip` 闸门 | `animator_paused`(0.0167≠0) / `animator_default_once`(clip 又被填回来) / `animator_joint_bitwise` —— **3 红** |

**这一轮踩到的两个坑**

1. **⚠ `animator_paused` 第一版是假断言**：断言打在 `anim(f).time` 上。但就算系统完全无视 `anim.playing`，
   也没有任何人去写 `anim.time`，它照样停在 0 —— 那条断言在**两个世界里都绿**。
   否定测试第一轮它没红，才被发现。改成断言**镜像值** `comp(f).time` 之后第二轮立刻红。
   **教训：断言的落点必须是"两种实现会不一样的那个量"，不是"顺手能拿到的那个量"。**
2. **`verify_anim_runtime.py` 里写死了"16 项断言全部出现过"**：新增自检先红一次，然后很容易被改成"27 项"了事 ——
   而真正该断的是"EXPECTED 里每一项都出现过"。已改成 `len(EXPECTED)`。

**明确没做（免得下次重启讨论）**

- **控制器资产 / 状态机 / crossfade**：A2。
- **参数黑板的序列化**：三张表是运行时（A2 按控制器资产的 default 初始化），不落盘。
- **根运动真的驱动位移**：`rootMotion` 字段与下拉先摆出来，A4 才接线；UI 上明确写着"占位"。
- **导入骨骼模型时自动挂 animator**：不做 —— 那会让 A0 的存盘基线整体多一个块。
  入口是 Inspector 的 Animator 段 / Add Component 菜单里的 `Animator`（两处都只对已挂蒙皮的实体可用）。


---

### A2 · 状态机 + crossfade（G11，`XL`）

> **A2a ✅ 已完成（2026-10-22，P4）**：先落了**硬切**的移动状态机 + 默认角色资产 +
> 按键映射。
> **A2b ✅ 已完成（2026-10-22，P5）**：crossfade 接上了 —— `Pose` / `blendPose`
> 是 A0-5 就备好的底座，`AnimatorSystem` 加了双采样 + smoothstep 权重。
> 用法与设计见 `docs/character_locomotion.md` + `docs/player_feel.md`，
> 代码在 `engine/animation/Locomotion.*` 与 `engine/animation/AnimatorSystem.*`。
> **仍未做**：`.anim.json` 控制器资产（并进 A3）。
>
> A2a 与本节规划的三处**有意偏离**（都是为了先把手感跑通、再谈过渡）：
> 1. **没有参数表**：状态由**物理真值**判（水平速度 / 是否贴地），不由输入轴判。
>    输入轴判的经典 bug 是"按着 W 撞墙"→ 人在原地蹬腿；物理真值天然免疫。
> 2. **没有控制器资产**：`LocomotionConfig` 是 C++ 结构体，字段来自
>    `RuntimeConfig::PlayerVisual`（Game.exe）与硬编码默认（编辑器 Play）。
>    等 A2b 做 `.anim.json` 时再把它读进来。
> 3. **四态硬切，无淡入淡出**：切 clip 只改 `clipName` 与 `time=0`。肉眼能看出
>    跳帧，但**腿真的在动**（这比"过渡漂亮但腿不动"优先）。
>
> A2a 的验收在 `tools/verify_char_locomotion.py`（15 条，C0~C10），
> `run_regression.py` 已登记。本节下面 §A2 原有的验收表（数值级 blending）**仍然有效**，
> 是 A2b 的准入条件。

**目标**：`Speed > 0.1` 从 Idle 切 Walk，切换用 0.2s 淡入淡出。

#### 控制器资产 schema（`assets/animators/*.anim.json`）

```json
{
  "version": 1,
  "defaultState": "Idle",
  "parameters": [
    { "name": "Speed", "type": "float",   "default": 0.0 },
    { "name": "Grounded", "type": "bool", "default": true },
    { "name": "Jump",  "type": "trigger" }
  ],
  "states": [
    { "name": "Idle", "clip": "Idle", "speed": 1.0, "loop": true  },
    { "name": "Walk", "clip": "Walk", "speed": 1.0, "loop": true  },
    { "name": "Jump", "clip": "Jump", "speed": 1.0, "loop": false }
  ],
  "transitions": [
    { "from": "Idle", "to": "Walk", "duration": 0.20,
      "conditions": [ { "param": "Speed", "op": ">", "value": 0.1 } ] },
    { "from": "Walk", "to": "Idle", "duration": 0.25,
      "conditions": [ { "param": "Speed", "op": "<=", "value": 0.1 } ] },
    { "from": "Any",  "to": "Jump", "duration": 0.10, "hasExitTime": false,
      "conditions": [ { "param": "Jump", "op": "trigger" } ] }
  ]
}
```

**实现要点**

- 参数：`float` / `bool` / `trigger`；**trigger 在一次转移被采纳后立即清零**
  （这是最容易写错的一条：不清零 → 每次回到 Idle 就立刻再跳一次）
- 转移求值：**每帧最多采纳一条**（防一帧内 A→B→C 抖动）；
  `from: "Any"` 按**数组顺序**定优先级（别用随机 / 别用 map 遍历顺序）
- `hasExitTime / exitTime`：`exitTime ∈ [0,1]` 是归一化时刻，只在 `stateTime >= exitTime * dur` 后才可转
- crossfade：`w` 由 `duration` 线性推进；**`w<=0 → a`、`w>=1 → b` 逐位短路**
  （`slerp(q,q,w)` 在浮点下不是恒等，这条不短路会引入 1e-7 的漂移，久了会"慢慢转歪"）
- 加载期校验（不通过就**禁用控制器并报错**，不许崩）：状态名重复、`clip` 不存在、
  转移引用未知状态、`from == to`、条件引用未知参数

**验收**（全部可自动化、可无窗口）

| # | 断言 | 口径 |
|---|------|------|
| 1 | 状态序列 | 喂 `Speed` = `[0, .5, .5, 0, 0]` 每帧一步 → `ANIM-STATE` 序列 = `Idle→Walk→Idle` |
| 2 | 转换耗时 | 每段 `duration ± 1 帧`（0.2s @60fps → 11~13 帧） |
| 3 | 混合数值 | `w=0.5` 时某骨骼 `local.rotation` = `slerp(qA,qB,.5)`，误差 < 1e-5 |
| 4 | 边界逐位 | `w=0` 与纯 A 的关节矩阵 `memcmp` 相同；`w=1` 同理 |
| 5 | trigger 单次 | 连续两帧设同一 trigger → `ANIM-TRIGGER consumed=1` 只出现一次 |

**锚点**：`ANIM-STATE from=%s to=%s t=%.3f dur=%.2f`、`ANIM-BLEND a=%s b=%s w=%.3f`、
`ANIM-TRIGGER name=%s consumed=1`

---

### A3 · 层与遮罩（G12 前半，`L`）

- `AnimatorLayer{ name, controller, weight, mask }`
- `BoneMask` 两种来源：**骨骼名列表** / **前缀通配**（`"spine*"`、`"arm_*"`）
  —— 前缀通配很实用，因为 Blender 导出的骨骼名通常是 `spine.001` 这种
- 语义：Base 先算 → 上层**只覆盖被遮罩骨骼**，其余骨骼保持 Base
- 顺序：`Base → Layer1 → Layer2 …`（数组顺序 = 叠加顺序，写进文档别让人猜）
- **验收**：Base=Walk、Upper=Fire、遮罩=上半身 →
  断言**腿部骨骼矩阵 == 纯 Walk 的输出**、**手臂骨骼矩阵 == 纯 Fire 的输出**
  （两组独立断言，这是 G12 的核心，少一组就证明不了遮罩真的生效）
- UI：面板里的层列表（名称 / 权重 / 遮罩编辑）
- **锚点**：`ANIM-LAYER name=%s weight=%.2f masked=%d`

---

### A4 · 根运动（G12 后半，`L`）

- `rootBone`：默认取 `joints` 里 `parent == -1` 的第一个；可在控制器里覆盖
- 三种模式：
  - `Off` —— 动画不驱动位移（"原地跑 + 代码移动"）
  - `FromClip` —— 位移完全由动画给（过场 / 精确对齐）
  - `DeltaOnly` —— 位移进物理、pose 保原位（脚不打滑但不离地）
- 提取：`Δ = pose[root].translation(t) − pose[root].translation(t−dt)`
  → 乘实体当前 yaw → 加到 `TransformComponent.position`
- ⚠ **必须把 Δ 从 pose 的 root 位移里减掉**，否则蒙皮再算一次 → **位移翻倍**（几乎必踩）
- ⚠ **与 `PlayerController` 的契约**：`FromClip` 模式下 `PlayerController` 必须把水平速度清零，
  否则"动画走 1 米 + 代码走 1 米 = 2 米"
- **验收**（"不滑步"是既有总验收 #5 的口径）：
  1. 播一个完整循环（左右脚各一次）→ 实际位移 / clip 内 root 位移 ∈ **[0.95, 1.05]**
  2. `Off` 模式 → 位移**恰好 0**（逐位）
  3. **否定测试**：关掉提取后比值应远低于 0.95（证明这条断言有判别力）
- **锚点**：`ANIM-ROOT mode=%d delta=(%.4f,%.4f,%.4f) yaw=%.3f`

---

### A5 · 动画事件（G12 后半，`M`）

- 事件表在**控制器资产**里（见 3.4 的决策）：`"events": { "Walk": [ {"time":0.35,"name":"FootstepL"} ] }`
- 派发条件：`prevTime < e.time <= time`；**`loop` 时必须处理跨圈**
  （`prev=1.15, time=0.05, dur=1.2` → 也要派发 `time ∈ (1.15, 1.2] ∪ [0, 0.05]` 的事件）
- 消费端：`ScriptComponent` 回调 +（G14 之后）`AudioSourceComponent`；现在先落日志
- ⚠ **FBX 是 30fps 重采样**（`FbxLoader.cpp:380`）→ 短于 1/30 秒的窗口会落空；
  事件时间要按采样后的时间轴对齐，别按 DCC 里的秒数硬填
- **验收**：3 个事件的 clip 播 2 圈 → 恰好 6 次、顺序正确、时间戳误差 < 1 帧
- **锚点**：`ANIM-EVENT clip=%s name=%s t=%.3f`

---

### A6 · IK（G13，`M`）

- **两骨 IK**（大腿/小腿）用**解析解**（余弦定理），不用迭代 —— 快且稳定
- 脚目标：从脚踝向下 `PhysicsWorld::raycast`（`engine/physics/PhysicsWorld.h:142` **已有**）
- **骨盆补偿**：双脚目标高度差 > 阈值时降 `pelvis`，否则腿会拉直成劈叉
- FABRIK（多骨链）作为手部抓握的可选项，最后做
- ⚠ IK **必须在混合之后**；权重 0 → **逐位等于**无 IK
- **验收**：
  1. 斜坡站立：脚骨世界 Z 与地面差 **< 2cm**；**对照组**（关 IK）应 > 10cm
     —— 没有对照组就证明不了"这条断言有效"
  2. 极端目标（脚在头顶）→ 全关节矩阵 `std::isfinite` 通过，无 NaN
  3. `weight=0` → 与关闭 IK 逐位相同
- **锚点**：`ANIM-IK foot=%d target=%.3f actual=%.3f weight=%.2f`

---

### A7 · Animator 面板（UI 面，`L`）

**为什么必须做**：项目五件套准入要求"UI 面（点得到）"，没有面板的功能不算交付。
**但现在别做** —— 控制器 schema 没定稿就画面板，等于把 schema 焊死在 UI 里。

- 新面板 `editor/AnimatorPanel.{h,cpp}`，在 `panelTableVec()` **加一行**（菜单/快捷键/持久化/绘制四路自动派生）
- 三区布局（用自研 `SplitLayout`，**不引 ImGui docking / node editor** —— 项目硬约束）：
  - **左**：状态列表（名称 / clip / loop）+ 新建 / 删除 / 改名
  - **中**：状态机图（节点 + 有向边；点节点选中，右侧出详情；拖节点改布局坐标 → 落进控制器 JSON）
  - **下**：事件时间轴（横轴 = 当前状态 clip 时长，事件画竖线，双击新建、拖动改时间）
- 顶部：参数面板（float 滑条 / bool 勾选 / trigger 按钮）+ **当前状态高亮**（读 `ANIM-STATE`）
- ⚠ 快捷键：面板已占 `Alt+1..8`，这块要用 `Alt+9` 或 `Ctrl+Alt+7` —— **先查归档一的排他表再定**
- ⚠ 必须参与 `collectSplitOccluders()`（否则分隔条被面板盖住会整条消失）；默认**不 dock**
- **验收锚点**：`ANIM-PANEL-STATE-<name>`、`ANIM-PANEL-EVENT-<i>`、`ANIM-PANEL-PARAM-<name>`
  （锚点名字里带 `<` `>` 不行，实际用 `ANIM-PANEL-STATE` + `name=..` 的形式）

---

### A8 · 多角色与性能（`M`，可随时插）

- **热点**：`sample()` 里每帧对每根骨骼跑一次 `glm::decompose(restLocal)`
  （`Skeleton.cpp:79`）→ **这是最贵的**，而 rest 分解是常量 → 预计算进 `Skeleton`，只算一次
- buffer 扩容（`Renderer.cpp:4788-4801`）：角色数变化时重建 buffer + 重写描述符 → 首帧卡顿；
  改成一进场景就按 `N × 128` 预留
- **预算锚点**：`ANIM-COST entities=%d sampleMs=%.2f blendMs=%.2f totalMs=%.2f`
- 目标：20 个角色（每角色 ~63 骨骼）@60fps
- 平行化（Job 系统）**先别做** —— 先量，单线程够用就不要引线程池
  （项目现在完全没有 Job 系统，引进来是一整个新底座）

---

### A9 · clip 资产化（`XL`，**明确后置**）

- 现状：clip 埋在 `Model.skeleton.clips` 里 → 不能跨模型共享、不能单独导入一段动画、改名要动模型
- 目标：`assets/animations/*.anim`（clip 独立序列化，按**骨骼名**引用而非下标）
- 需要 **retarget**（骨骼名映射 + 比例缩放）→ 这是资产管线级改动
- **不要在 A0~A7 之前做**：现在的耦合虽然脏，但它让"一个 FBX = 一套骨架 + 一组 clip"
  这个心智模型极其简单，先跑通再解耦

---

## 5 · 验收锚点总表

| 锚点 | 时机 | 断言什么 | 用在 |
|------|------|---------|------|
| `MODEL-INSTANCE path=.. skinned=1 joints=..` | 拖入模型 | `model->skinned()` 时真的建了组件 | A0-1 |
| `MODEL-CLIPS root=.. n=.. first=..` | 拖入模型 | 到底有几个 clip、叫什么（"不动"的第一现场） | A0-1 |
| `IMPORT-AUTO path=.. slot=.. root=.. skinned=1` | `MYVK_EDITOR_IMPORT` | 自动化导入路径同上 | A0-1 |
| `INS-ANIM-HEADER` / `INS-ANIM-TIME`（矩形） | 每帧 | Animation 页签真的画出来了 | A0-1 |
| `SCENE-SKINNED entity=.. clip=..` | 存 / 读档 | 动画字段往返一致 | A0-2 |
| `ANIM-TICK f=.. entities=.. dt=..` | 每 60 帧 | 系统真的在跑（`dt>0` 才是 Play 态） | A0-3 |
| `ANIM-WRAP clip=.. t=.. dur=..` | 跨圈时 | `fmod` 正确 | A0-3 |
| `ANIM-SKIN entity=.. jointBase=.. time=..` | 变化时 | 多实例 `jointBase` 两两不等 | A0-4 |
| `ANIM-SETTIME ok=.. entity=.. t=..` | `anim.time` | 时间轴/命令行真的钉住了 | A0-5 |
| `ANIM-SETCLIP ok=.. entity=.. clip=..` | `anim.clip` | clip 切换生效 | A0-5 |
| `ANIM-TEST <name> actual=.. expected=.. tol=..` | `MYVK_ANIM_TEST=1` | 逐项数值断言（含逐位重构不变性） | A0-3/A0-5 |
| `ANIM-TOTAL pass=.. fail=..` | 同上 | 汇总；脚本还要核对"每项都出现过" | A0-3/A0-5 |
| `ANIM-STATE from=.. to=.. t=.. dur=..` | 转移采纳 | 状态序列与耗时 | A2 |
| `ANIM-BLEND a=.. b=.. w=..` | 每帧（变化时） | 混合权重推进 | A2 |
| `PLAYER-DEFAULT bound path=.. joints=.. clips=.. height=.. capsule=..` | 建角色 | 默认模型/骨骼/动画真的装上了，且三处比例同源 | **A2a** |
| `EDIT-ANIM state=.. clip=.. (from=..) speed=..` | 状态变化 | 编辑器 Play 的移动状态机（`GAME-ANIM` 同格式） | **A2a** |
| `EDIT-ANIM-IDLE retrigger=N idle=..s` | 静止计满 | 每 N 秒重放 idle 一次（`=2 idle=16.01s` 证明节奏对） | **A2a** |
| `CHAR-CLIP-KEY-<clip>`（矩形） | 每帧 | Animation 页每条 clip 行尾真的画了键槽 | **A2a** |
| `CHAR-KEYBIND clip=.. key=.. label=..` | 绑键 | 键码翻译与"一键一段"的回收都正确 | **A2a** |
| `EDIT-ANIM-KEY-TRIGGER clip=.. key=..` / `GAME-ANIM-KEY-TRIGGER` | 按键 | 强播生效（顺带证明 `animKeys` 存读闭环） | **A2a** |
| `ANIM-TRIGGER name=.. consumed=1` | trigger 消耗 | 只消耗一次 | A2 |
| `ANIM-LAYER name=.. weight=.. masked=..` | 每帧（变化时） | 层权重与遮罩 | A3 |
| `ANIM-ROOT mode=.. delta=..` | 每帧（变化时） | 根运动提取 | A4 |
| `ANIM-EVENT clip=.. name=.. t=..` | 跨过事件时刻 | 次数/顺序/时间戳 | A5 |
| `ANIM-IK foot=.. target=.. actual=..` | 每帧（变化时） | 脚贴地 | A6 |
| `ANIM-COST entities=.. totalMs=..` | 每 120 帧 | 性能预算 | A8 |

**通用纪律**（都踩过）：
- 新断言必须做**否定测试**（退回 bug 版本要变红）
- 锚点名字拼错 → 断言**永远为假**且静默通过；改完必须跑一遍看它变绿
- 脚本结尾必须是 `结果: PASS` / `结果: FAIL: ...`（ASCII 冒号，取最后一条）
- 峰值类指标容差不能比瞬时值更紧

---

## 6 · 回归与准入

### 6.1 五件套对照（按阶段）

| 阶段 | ① 实现 | ② FeatureRegistry | ③ UI 面 | ④ 序列化 | ⑤ 用例 + TRIGGERS |
|------|-------|-------------------|--------|---------|------------------|
| A0-1 | ✅ | 不需要（不是渲染特性） | 拖拽即入口 | — | `verify_character_import.py`（新） |
| A0-2 | ✅ | — | — | ✅ | 并入 A0-1 |
| A0-3 | ✅ | `MYVK_ANIM_TEST` 要走 `bindParam` | — | — | `verify_anim_runtime.py`（新） |
| A1 | ✅ | — | Inspector 时间轴 | ✅ | 并入 A0-3 |
| A2 | ✅ | — | 面板（A7 补） | ✅ 控制器资产 | 状态机数值断言 |
| A3 | ✅ | — | 面板层列表 | ✅ | 遮罩双组断言 |
| A4 | ✅ | — | 面板模式下拉 | ✅ | 不滑步比值 |
| A5 | ✅ | — | 面板时间轴 | ✅ | 事件次数/顺序 |
| A6 | ✅ | — | 面板权重 | ✅ | 坡地 + NaN + 权重 0 |
| A7 | ✅ | 面板登记 | ✅ 本体 | ✅ | 面板结构断言 |

### 6.2 新文件 → `TRIGGERS` 登记（**不做就会被静默跳过**）

```
engine/scene/ModelInstance.*        → verify_character_import
engine/animation/**                 → verify_anim_runtime
editor/AnimatorPanel.*              → verify_anim_panel（或并入 rewrite 的 particles 模式）
engine/render/Renderer.*            → 已在 GLOBAL_GLOBS（全量）
engine/ecs/Components.h             → 已在 GLOBAL_GLOBS（全量）
engine/assets/Skeleton.*            → 已在 GLOBAL_GLOBS（全量）
editor/SceneSerializer 相关          → verify_scene_roundtrip 类的既有用例
```

⚠ 改完 `TRIGGERS` **必须跑 `check_regression_map.py`**（改表 + 同步 fixture 是原子操作）。
⚠ 新增 `verify_*.py` 必须进 `DEFAULT` + 补 `TRIGGERS`。

### 6.3 横切警告（决定"分几次提交"）

| 文件 | 在 GLOBAL_GLOBS？ | 影响 |
|------|------------------|------|
| `engine/ecs/**` | ✅ | 加 `AnimatorComponent` = 全量 |
| `engine/assets/**` | ✅ | 改 `Skeleton::sample` = 全量 |
| `engine/render/Renderer.h/.cpp` | ✅ | 改 `DrawItem` = 全量 |
| `engine/scene/Scene.h` | ✅ | 碰它 = 全量 |
| `engine/animation/**`（新目录） | ❌ | 只要登记 TRIGGERS 就**不会**全量 |
| `editor/**`（除 `PanelRegistry`） | ❌ | 登记 TRIGGERS 即可 |

→ **A0 的五项攒成一次改完，只跑一次全量回归。** 拆开就是五次。

---

## 7 · 十个坑（本项目特有，逐条给症状）

1. **`engine/ecs/**`、`engine/assets/**`、`Renderer.h/cpp` 全是横切**
   → 分批改 = 分批全量。**A0 一次改完。**
2. **`Skeleton::sample` 的 `static thread_local` 缓冲**（`Skeleton.cpp:77, 101`）
   → 拆 Pose 时**必须删掉**，否则 A3 的层嵌套会互相踩，症状是"上层一动，下层姿势跳一下"。
3. **`glm::mat4` 直接 lerp 会插出缩放/剪切** → 肉眼是"角色混合时变瘦"。必须在 TRS 上混。
4. **`w=0/1` 不短路会漂移** → `slerp(q,q,w)` 不是恒等，长跑后角色会"慢慢转歪"。
   断言口径：`w=0` 必须与纯 A **逐位**相同。
5. **trigger 不清零 = 无限跳** → 每次进 Idle 立刻又被 Jump 转走。
6. **根运动提取后必须从 pose 里减** → 否则位移翻倍，角色像在滑冰。（几乎必踩）
7. **FBX 是 30fps 重采样**（`FbxLoader.cpp:380`）→ 短于 1/30 秒的事件窗口落空；
   事件时间按采样后的轴对齐，别按 DCC 里的秒数填。
8. **`Entity.id` 是运行时序号**（G16 还没做）→ **不要**把动画状态按实体 id 存盘，
   只能存在组件里（跟着实体走）。
9. **Z-up** → 根运动的 yaw 要走"实体朝向"，不能直接读 clip 里的欧拉角
   （glTF 是 Y-up，`Skeleton.h` 头注释解释了为什么蒙皮能与静态网格共用 push —— 别在动画层破坏这个约定）。
10. **Animator 面板不要引 docking / node editor** → 项目硬约束，用 `SplitLayout` + 自绘。
    另外新面板必须进 `collectSplitOccluders()`，否则分隔条被它盖住会**整条消失**。

---

## 8 · 关键路径与推进顺序

```
A0（地基，必须一次做完）
 ├─→ A1（AnimatorComponent）
 │    └─→ A2（状态机 + crossfade）
 │         ├─→ A3（层 + 遮罩）─┐
 │         ├─→ A4（根运动）   ├─→ A6（IK）
 │         └─→ A5（事件）     │
 │              └→ A7（面板，等 A3/A4/A5 的 UI 需求齐了再一次画完）
 └─→ A8（多角色，第二个角色进场景时就必须做）
A9（clip 资产化）—— 明确后置
```

- **关键路径** = `A0 → A1 → A2 → A3/A4`
- A5 只依赖 A2，可与 A3/A4 并行
- A7 在 A2 之后就能开工，但**最好等 A3/A4/A5 的需求齐了再一次画完**
  （否则事件时间轴要改两遍、层列表要改两遍）
- A8 在 A0 之后随时能插，但**只要场景里出现第二个角色就必须先做**（否则串姿势）

### 每个阶段的"完成长什么样"

| 阶段 | 肉眼可验收的结果 |
|------|-----------------|
| A0 | Game.exe 里角色在走（单 clip、无过渡、可能滑步） |
| A1 | Inspector 能拖时间轴定格任意姿势；存盘重开姿势还在 |
| A2 | 停下→走路有 0.2s 淡入淡出，**不跳帧** |
| A3 | 上半身开枪、下半身跑步，互不打架 |
| A4 | 走路**不滑步**（这是"手感"最大的一档） |
| A5 | 脚落地那一下能触发事件（日志里能看到 `FootstepL/R` 交替） |
| A6 | 站在斜坡上脚不穿地 |
| A7 | 不写 JSON 也能在面板里搭出状态机 |
| A8 | 20 个敌人同屏各自播各自的 |
