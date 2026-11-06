# UE5 Mannequin 骨架：把角色骨骼/绑定/动画按 UE 的结构做

> 2026-11-06 · 用户原话：「把角色模型的动作，姿势，骨骼绑定，动画完全按照 ue5
> 中的风格进行改造，参考 `E:\dev\UE-project\UE_Basics\Content\Characters\Mannequins`」
> 验收：`MYVK_MANNEQUIN_TEST=1 Editor.exe`（32 条）+ `tools/verify_char_locomotion.py` C5e2
> 实现：`engine/character/MannequinSkeleton.{h,cpp}`、`engine/character/MannequinSelfTest.cpp`

---

## 0 · 先看清"UE 的结构"是什么（全部实证，不是凭记忆）

对着真实工程列出来的：

```
Content/Characters/Mannequins/
├── Meshes/
│   ├── SK_Mannequin.uasset        ← **独立骨架资产**（35 根骨）
│   └── SKM_Manny_Simple.uasset    ← 引用它的网格
│       SKM_Quinn_Simple.uasset
├── Anims/
│   ├── Unarmed/
│   │   ├── ABP_Unarmed.uasset     ← 动画蓝图（状态机）
│   │   ├── BS_Idle_Walk_Run.uasset← **二维混合空间**，轴 Speed × Direction
│   │   ├── MM_Idle.uasset
│   │   ├── Walk/MF_Unarmed_Walk_{Fwd,Bwd,Left,Right,Fwd_Left,Fwd_Right,Bwd_Left,Bwd_Right}
│   │   ├── Jog/…（同样 8 个方向；**UE5 用 Jog，没有单独的 Run**）
│   │   └── Jump/MM_Jump, MM_Fall_Loop, MM_Land, MM_Dash, MM_WallJump
│   ├── Rifle/…  Pistol/…  Death/…
├── Rigs/CR_Mannequin_Body, CR_Mannequin_FootIK, CR_Mannequin_Procedural, PA_Mannequin
├── Materials/  Textures/
```

命名前缀：`SK_` 骨架 / `SKM_` 蒙皮网格 / `MM_` 男（Manny）序列 / `MF_` 女（Quinn） /
`BS_` 混合空间 / `ABP_` 动画蓝图 / `AO_` 瞄准偏移 / `CR_` Control Rig / `PA_` 后处理 AnimBP。

**核心不是命名好看，而是"一份骨架被所有角色和所有动画共享"** —— 这是动画能跨角色
复用的前提。骨名、骨数、骨层级三者固定，缺一不可。

### 35 根骨（`strings -a SK_Mannequin.uasset` 数出来的）

```
root  pelvis
spine_01 spine_02 spine_03 spine_04 spine_05     ← 5 节脊柱
neck_01 neck_02  head                            ← 2 节脖子
clavicle_l/r  upperarm_l/r  lowerarm_l/r  hand_l/r
thigh_l/r  calf_l/r  foot_l/r  ball_l/r          ← 有 ball（脚掌前段）
ik_foot_root  ik_foot_l/r  ik_hand_root  ik_hand_gun  ik_hand_l/r
weapon_l/r
```

`ik_*` / `weapon_*` 是 UE 的**非形变骨**（`bDeform=false`）：IK 目标与挂点，不参与蒙皮。

## 1 · 引擎原来的骨架差在哪

`default_character.vmesh`（= `alicia.vmesh`）是 **Mixamo 命名、128 根骨**：

| UE5 Mannequin | 引擎原资产 |
|---|---|
| `root` / `pelvis` | 无独立 root；`Hips` |
| `spine_01`…`spine_05`（5 节） | `Spine` / `Spine1` / `Spine2`（3 节） |
| `neck_01` / `neck_02` | `Neck`（1 节） |
| `clavicle_l/r` `upperarm_l/r` `lowerarm_l/r` `hand_l/r` | `LeftShoulder` `LeftArm` `LeftForeArm` `LeftHand` |
| `thigh_l/r` `calf_l/r` `foot_l/r` `ball_l/r` | `LeftUpLeg` `LeftLeg` `LeftFoot` `LeftToeBase` |
| `ik_*` `weapon_*` | 完全没有 |
| 35 根 | **128 根**（多出来的是手指、表情、裙摆之类的一堆辅助骨） |

骨数、层级、命名三者都不同 → 动画不能互换。这就是"看起来像 UE"和"真的是 UE 结构"
的分界。

## 2 · 重定向：两条硬约束

### ① 不动一个顶点的权重 → 因此**下标一个都不能动**

顶点里存的是 `Vertex::joints`（`glm::uvec4`）。骨架一旦重排，这些下标就全指错骨 ——
而 `Mesh` 只暴露 `vertices()` 的 const 引用，没有改写入口，改它就要动 GPU 侧缓冲。
**所以"重排骨架 + 重写顶点下标"这条路根本不该走。**

采用的做法：

1. 源骨**每一根留在原下标**，其中映射到 UE 骨的改成 UE 名；
2. UE 表里源里没有的骨（`root`、`spine_04/05`、`neck_02`、`ik_*`、`weapon_*`）
   **追加到末尾**；
3. 因此**顶点下标与动画轨道下标都不用动**，一个字节的顶点数据都不碰。
   这不是靠自觉，是机制上就不可能错。

代价：源里 UE 没有的辅助骨原样保留（`alicia` 是 106 根），所以装配后是
**141 根 = 128 + 13**，其中 35 根是 UE 骨。删掉它们就得重排下标、动顶点 ——
代价远大于收益。

### ② 重定向前后，原骨的世界姿势必须逐位相同

父子关系变了（`neck_01` 的父从 `Spine2` 变成 `spine_05`），做法是**先算世界矩阵、
再反解局部**：

```cpp
restLocal = inverse(parentWorld) * world;   // world 是照抄来的
```

反例：先算 `restLocal`（照抄）再求世界 —— 整个人会平移一格，而且**不报错**。
自检 `mq_world_preserved` / `mq_jointmat_preserved` 就是钉这个的，实测
`actual=0.000000`（23 个槽 × 5 个时刻 × 2 条 clip = 230 个采样点）。

`invBind` 必须**照抄源的**，不能拿新世界的逆去算：静止帧看不出来，一动就错。

## 3 · 接进装配路径

```cpp
character::upgradeModelSkeletonForPlay(*model);   // 两个宿主都只调这一个
```

* `editor/EditorScene.cpp::bindDefaultCharacter`（编辑器 Play）
* `runtime/RuntimeApp.cpp::attachPlayerModel`（Game.exe）

`MYVK_MANNEQUIN=0` 关掉（A/B 与回归归因用）。锚点：
`MANNEQUIN retarget ok=1 bones=141 ue=35 copy=22 synth=13 aux=106 missing=0 src=128`

## 4 · 撞出来的一个既有 bug：骨骼 SSBO 容量

接上之后 **画面全黑 + 满屏 validation 错误**：

```
vkUpdateDescriptorSets(): dstBinding (0) ... but VkDescriptorSet ... is in use by VkCommandBuffer
```

根因不在重定向，在**容量常数**：

* `updateSkinning` 每帧被调用**两次**（主场景 `resetSkinning=true`、离屏目标 `false`
  追加），所以真实需求是 **2 × Σjoints**；
* `kInitialJoints` 原来是 **256**，而 alicia 是 **128 根骨** → 128+128 = 256，
  恰好不触发 `capNeed > frame.jointCapacity`；
* 骨数变成 141 → 282 → 当场扩容 → 扩容里 `writeGlobalDescriptors()` 在**录制中途**
  重写描述符 → 命令缓冲进入 invalid state。

改成 **4096**（256 KB，够 14 个 141 骨角色）。这不是"调大一点的性能选择"，是
**正确性下限**：256 从一开始就只够一个 128 骨的角色的两遍。

⚠ 扩容路径本身仍有隐患：它会把**所有** frame slot 的描述符一起重写，包括 GPU 上
还在跑的那些。真要根治得把扩容挪到录制之前。现在只是把它压到不会触发的量级。

## 5 · 门禁

| 门禁 | 结果 |
|---|---|
| `MYVK_MANNEQUIN_TEST=1` | **52 pass / 0 fail**（含 §7 的 `.vskel` 与挂点 20 条） |
| `verify_skeleton_asset`（新） | 11 / PASS |
| `verify_char_locomotion` | 23 / PASS（新增 C0c 骨架资产、C5e2 `ue=35`、C5e3 `source=asset`） |
| `verify_player` / `verify_player_feel` / `verify_player_jump` | PASS / 10 / 11 |
| `verify_view_rig` / `verify_player_character` / `verify_procanim` / `verify_anim_runtime` | PASS / 11 / 20 / 466 |
| `verify_vmesh` / `verify_morph` / `verify_vertex_format` / `MYVK_ASSET_TEST` | PASS / 33 / PASS / 46 |
| `verify_frame_pacing` | 12 / PASS |
| `check_regression_map` | PASS（顺手补了 3 条过期的对话夹具） |

自检里几条关键的：`mq_world_preserved=0`、`mq_jointmat_preserved=0`、
`mq_ue_bones=35`、`mq_track_index_stable=0`（轨道下标一条没变）、
`mq_idempotent_world=0`（对 UE 骨架再跑一遍不变）、
`mq_vskel_matrices=0`（`.vskel` 往返后 36 根骨的 `restLocal`/`invBind` 逐元素不变）、
`mq_socket_renamed=1`（`weapon_r` 的骨名跟着 `RightHand → hand_r` 改了）。

## 6 · 还没做的（这条线剩下的）

| 项 | 状态 | 说明 |
|---|---|---|
| 动画拆成独立序列（`Anims/Unarmed/...`） | **未做** | 现在 clip 还在 vmesh 里，名字是 `Idle/Walk/Run/Jump`（骨架已独立，动画还没） |
| 8 方向移动集 | **未做** | UE 是 `_Fwd/_Bwd/_Left/_Right` + 4 对角；现有资产只有朝前 |
| `BS_Idle_Walk_Run` 二维（Speed × Direction） | **未做** | 现在是 1D，只有 Speed 轴 |
| `ABP_Unarmed` 命名与资产化 | **未做** | 现在叫 `humanoid.anim.json` |
| A-pose 参考姿势 | **未做** | 现有资产是 T-pose 绑定；改成 A-pose 要同时平移所有 clip 的关键帧才不破坏动画 |
| socket 的实际消费者 | **半做** | 数据层做完（§7）；`weapon_r` 挂武器/`eye` 挂相机的装配还没接 |

## 7 · 骨架独立成资产（`.vskel`）

### 7.1 抄的是 `USkeleton` 的**真实语义**

改造前骨架是 `SourceModel::joints`，内嵌在 `.vmesh` 里。UE 那边不是这样：
`SK_Mannequin` 是**独立资产**，被 `SKM_Manny` / `SKM_Quinn` / 几百条
`AnimSequence` 共同引用。差别不在于"多一个文件"，而在于三件具体的事：

| | 内嵌（改造前） | 独立资产（现在） |
|---|---|---|
| 两个角色用同一套骨 | 骨表存两份，改一处漏一处 | 一份，改一次全生效 |
| 挂点（武器/视线） | 没归属地，只能硬编码 | 归骨架所有，随骨名一起走 |
| "这两者是不是同一套骨" | 无法检查 | `boneDigest` 摘要比对 |

还有一个容易看漏的事实：**UE 的 Skeleton 资产里没有静止姿势**。
`USkeleton` 只持有 `FReferenceSkeleton`（`RefBoneInfo` = 名字 + 父下标），
姿势在 SkeletalMesh 的 `RefBonePose` 里。所以
`assets/characters/mannequin/Rigs/SK_Mannequin.vskel` 的 `restLocal`
**全是单位阵 —— 那是资产的语义，不是数据没填**。自检
`mq_emit_reference_pose` 专门钉这一点（有人往里塞姿势会红）。

### 7.2 目录按 UE 的 `Mannequins/`

```
assets/characters/mannequin/
├── Meshes/                      # SKM_*（本工程的蒙皮网格仍在 assets/models/characters/）
├── Rigs/SK_Mannequin.vskel      # 35 根 UE 骨 + 3 个挂点（由 MannequinBoneDef 导出）
└── Anims/                       # 动画序列（下一刀）

assets/models/characters/
├── alicia.vmesh                 # 引用 ↓（SKELREF 区块，sections 6→7）
└── alicia.vskel                 # 128 根骨 + 3 个挂点（从 vmesh 提取）
```

**没有把 `alicia.vmesh` 搬进 `Meshes/`**：场景 JSON 与十几条验收脚本
都按 `models/characters/alicia.*` 引用它，搬一次等于全库改路径，而
"目录好看"不值这个风险。`SK_Mannequin.vskel` 那份才必须在新目录里 ——
它是所有角色共享的**定义**。

### 7.3 格式（`assetio/SkelFormat.h`，容器与 `.vmesh` 共用）

```
[ 32B 文件头 ] [ N × 32B 区块表 ] [ 区块 ... ]
  BONES(1)   joints / topoOrder / invMeshNode / skeletonToEngine / boneDigest
  SOCKET(2)  挂点表（没有挂点就不写这个区块）
  STR(3)     字符串表
```

容器层这一刀抽到了 `assetio/ChunkFile.h`：`.vmesh` 与 `.vskel` 用**同一份**
"文件头 + 区块表 + CRC + 字符串表"。两个格式各写一遍的差别会从"排版不同"
长成"一个能跳过未知区块、另一个不能"，而症状是**老读端读到新文件时从中间
开始错位** —— 最贵的那种 bug。

`boneDigest` = FNV-1a over 每根骨的 `(名字, '\0', 父下标)`。名字后补 `'\0'`
是必须的：不补的话 `["ab","c"]` 和 `["a","bc"]` 算出同一个值，而摘要的
全部用途就是"判两份骨表是不是同一份"。读盘时**重算一遍并与文件里的比**，
不等就硬拒（CRC 只能说明"字节没坏"，说明不了"两处字段互相矛盾"）。

### 7.4 加载：三条判据 + 静默退回

`VMeshLoader` 在 `src.skeletonPath` 非空时尝试独立骨架，三条全满足才采用：

1. 那个 `.vskel` 读得出来；
2. `boneDigest` 与 mesh 内嵌骨表**相等**；
3. （隐含）骨数/层级因此天然一致。

任一条不满足 → **打一行 WARN 并退回内嵌骨架**。这是刻意的取舍，与"贴图
丢了退化成默认材质"同一个口径：一条坏引用不该让角色消失。代价是"失败
是静默的"，所以判据必须盯住成功那一行 —— 这就是
`verify_char_locomotion` 的 **C5e3**（`VMESH-SKEL source=asset`）。

实测锚点（alicia，2026-11-06）：

```
VMESH-SKEL source=asset ref=models/characters/alicia.vskel bones=128 sockets=3 digest=f918ced8aeadedd7
MANNEQUIN retarget ok=1 bones=141 ue=35 copy=22 synth=13 aux=106 missing=0
```

### 7.5 挂点（socket）

数据：`Skeleton::sockets`（不是 `Model`）—— 归属与 UE 一致，同一套骨架的
所有 mesh 共享。

| 挂点 | 骨（UE） | 骨（Mixamo 资产） |
|---|---|---|
| `weapon_r` | `hand_r` | `RightHand` |
| `weapon_l` | `hand_l` | `LeftHand` |
| `eye` | `head` | `Head` |

骨名按**候选表**解析（`hand_r` → `RightHand` → `Right_Hand` → `Bip01_R_Hand`），
解析不到就**不建**这个挂点 —— 建一个挂 `-1` 号骨的空挂点比没有更难查。

偏移一律单位阵：UE 的 `SK_Mannequin` 上 `weapon_r` 的偏移是美术在 Skeleton
资产里手调的，我们没有那个数值。编一个"看起来像握着枪"的偏移比留 0 更糟，
它会让下一个人以为那是从 UE 抄来的。

**重定向必须跟着改名**：`RightHand → hand_r` 之后，挂点的 `boneName` 也要换，
否则 `resolveSockets()` 一律返回 `-1`，表现为"武器不跟手"且**没有任何日志**。
自检 `mq_socket_renamed` 钉的就是这一条。

### 7.6 一个副作用发现：重烤 vmesh **不幂等**

`--extract-skel` 最初会把 `.vmesh` 一起重写（走了正常的 `writeMeshFile`
路径）。实测 alicia 从 3285226 B 变成 3284930 B —— 少 296 B，来自
`reduceClip`：动画抽帧按容差丢帧，在**已经抽过**的曲线上再跑一遍还会再丢
几帧。所以：

* 只导骨架时**完全不写** `.vmesh`（日志 `vmesh untouched`）；
* 只加骨架引用（`--skel-ref`）时把 `reduceAnimation` 关掉，焊接/重排照旧
  （那两个幂等）。结果：3285226 → 3285300，**+74 B = SKELREF 区块 + 一个字符串**，
  文件标志位 `flags=0xf` 原样保留。

### 7.7 工具

```bash
# 从模型提取骨架资产（默认带 3 个挂点）
AssetC models/characters/alicia.vmesh --extract-skel models/characters/alicia.vskel

# 把骨架引用写进 vmesh（sections 6 → 7）
AssetC models/characters/alicia.vmesh --skel-ref models/characters/alicia.vskel

# 体检 / 校验（--info 按扩展名自动分流：.vskel 走 SKELINFO）
AssetC --info models/characters/alicia.vskel
AssetC --check characters/mannequin/Rigs/SK_Mannequin.vskel

# UE 标准骨表 → 资产（一次性导出；骨表在 engine/character，assetio 不认识它）
MYVK_MANNEQUIN_EMIT=assets/characters/mannequin/Rigs/SK_Mannequin.vskel Editor.exe
```
