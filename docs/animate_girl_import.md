# Animate_Girl 角色导入：从"静态雕塑"到"会走路的默认角色"

> 2026-10-23 · 用户报障："当前这个初始化的角色模型太粗糙了，
> `E:\dev\UE-resources\Animate_Girl` 这个目录中有一个角色模型和对应的角色动画，
> 你把它导入我的引擎中作为初始化角色模型试试看，并且要绑定好动画。"
>
> 工具：`tools/blender_import_animategirl.py`（Blender 4.5.9）
> 产物：`assets/models/characters/animate_girl.{fbx,vmesh,glb}`
> 状态：**离线管线打通、资产自检全绿；引擎侧蒙皮绘制有阻塞 bug（§5）**

---

## 0 · 一句话结论

用户给的"一个模型 + 一组动画"**并不能直接插到一起用**：模型是纯静态网格
（没有骨骼、没有权重），动画是 65 骨的 Mixamo 骨架（没有网格）。中间缺的
那一层是**蒙皮权重**。补法是 Blender 骨热绑骨 + 视觉烘帧（`Copy Location/
Rotation` + `nla.bake`），产物四段 clip 名字精确对齐引擎的
`LocomotionConfig`（Idle/Walk/Run/Jump）。

**资产侧全部达标**（见 §4 的自检数字），**渲染侧还没通**（见 §5）。

---

## 1 · 资产真相（先量，别猜）

| 文件 | 内容 | 关键统计 |
|---|---|---|
| `animate-girl.fbx` | `Yuzuha_Swimwear_Body_1` | 27394 顶点 / 7 材质 / **0 顶点组 / 0 骨骼** / 1181 个断开壳 |
| `resources/Standing Idle.fbx` | 纯动画 | **65 骨**（`mixamorig:*`）/ 181 帧（6.00s）/ 无网格 |
| `resources/Walking.fbx` | 纯动画 | 65 骨 / 30 帧（0.97s） |
| `resources/Fast Run.fbx` | 纯动画 | 65 骨 / 17 帧（0.53s） |
| `resources/Jump.fbx` | 纯动画 | 65 骨 / 31 帧（1.00s） |
| `resources/Dwarf Idle.fbx` | 纯动画 | 65 骨 / 211 帧（7.03s，未采用） |

判定方法：扫文件里的 `LimbNode / Deformer / Skin / Cluster / AnimCurve` 关键字 +
`AssetC --check`（后者对动画文件直接报 `no mesh found in file`）。

---

## 2 · 三个必须踩过去的技术坑

### 坑 1 · Mixamo 的 rest 姿势是坏的

`arm.data.bones[...].head_local` 给出来的 bind pose 整体**比网格高 0.95 m**：

```
rest: Hips z=1.96  LeftFoot z=1.04  Head z=2.33     ← 悬空、比例也不对
pose: Hips z=1.00  LeftFoot z=0.09  Head z=1.37     ← 这才是那个人
```

骨热蒙皮拿 rest 去算 → 骨头和网格一个在上一个在下 → 报
`Bone Heat Weighting: failed to find solution for one or more bones`，
然后**所有顶点 0 权重，而且不抛异常**（`parent_set` 照样建了 65 个空组）。
第一次跑出来的 `verts_with_weight=0/27394` 就是这么来的。

修法：`bpy.ops.pose.armature_apply(selected=False)` 把**当前姿势**烘成 rest。
代价是原来那条 action 失效（F-curve 的局部旋转是相对旧 rest 的），所以四段
clip 一律**从源文件重新导入**（见坑 3）。

### 坑 2 · 整块网格上骨热必失败，逐壳就能成

网格有 **1181 个断开的壳**（发丝、睫毛、衣服片、绑带）。在这种拓扑上
骨热的线性系统无解。实测对照：

| 做法 | 结果 |
|---|---|
| 整块直接 `ARMATURE_AUTO` | 0 / 27394 |
| 焊接重复点（27394 → 17535，壳 1181 → 123）+ 整块绑 | 0 / 17535 |
| **清理 → `separate LOOSE` → 逐壳绑 → join** | **123/123 壳成功，17535/17535 顶点有权重** |

所以流程写死成"清理 → 分壳 → 逐壳绑 → 合回"。合回时顶点组按名字合并，
不需要手工搬权重。

### 坑 3 · 烘帧不能搬缩放，clip 名不能带前缀

* `Copy Transforms` 约束会把源骨的**缩放**一起搬过去。源骨架 object scale 是
  **0.01**（Mixamo 厘米制），于是目标每根骨被缩到 1/100 —— 引擎里整个人
  **炸成星形**、网格拉成面条，而日志/断言全绿。
  改用 `Copy Location` + `Copy Rotation`（世界空间），缩放不参与。
* 源骨架的缩放还要**乘**同一套均匀缩放（`arm.scale = c * s`，不是赋值）：
  直接赋值会覆盖掉那个 0.01 → 骨架放大 100 倍 → 骨热一个顶点都找不到。
* FBX 导出器会给 take 名加 `<armature对象名>|` 前缀（`Armature|Idle`），
  而引擎 `Skeleton::findClip` 是**逐字**比较 → 一个 clip 都找不到 = 角色站着不动。
  用 `bake_anim_use_nla_strips=True` 并把 **NLA 轨名写成裸 clip 名**，
  take 名才干净。
* 非根骨的 `location` 通道必须砍掉（人形骨架是刚性的）：留着会被单位约定
  放大，症状同上。砍完 263 条 fcurve = 65 骨 × 4（四元数）+ 根骨 3（平移）。

---

## 3 · 管线（一条命令）

```bat
"D:/blender/blender.exe" --background --factory-startup ^
    --python tools/blender_import_animategirl.py ^
    -- --out assets/models/characters/animate_girl.fbx

build/bin/AssetC.exe assets/models/characters/animate_girl.fbx ^
    -o assets/models/characters/ -q
```

脚本内部分九步：导入网格 → 烘变换/脚底归零/水平居中 → 导入骨架并**按姿势**
缩放对位 → `armature_apply` 修 rest → 清理 + 逐壳绑骨 + join → 四段 clip
导入 + 约束 + `nla.bake` → 砍非根骨平移/缩放 → NLA 轨 → 导出。

日志锚点：`AGIRL BIND shell ok=123/123`、`AGIRL BIND joined verts=17535
weighted=17535 (100.0%)`、`AGIRL CLIP Idle frames=1..181 dur=6.000s
dropped_loc=192 dropped_scale=195`。

---

## 4 · 资产自检（全部达标）

```
AssetC --info assets/models/characters/animate_girl.vmesh
  joints=65 clips=4 materials=7 skinVerts=25626/25626
  size=1.025 x 0.801 x 1.626 m  height(Z)=1.626 m
  clip "Idle" 6.00 s
  clip "Walk" 0.97 s
  clip "Run"  0.53 s
  clip "Jump" 1.00 s
```

GLB 版本另做了独立校验（直接解析 glTF 二进制）：

```
meshes 1  skins 1  anims ['Idle','Jump','Run','Walk']
joints in skin: 65
verts with weights=26134  sum min=1.0000 max=1.0000  outliers=0 (0.00%)
```

`player` 侧也认了：`PLAYER-DEFAULT bound path=models/characters/animate_girl.glb
meshes=7 skinned=1 joints=65 clips=4 height=1.626 capsule=0.260/0.553 yaw=90.0`
—— `verify_char_locomotion.py` 在切成新角色时 **16/16 PASS**
（C6 走起来播 Walk、C7 回 Idle、C8 满 8 秒重放、C9 按 K 强播 Jump 全对）。

---

## 5 · 阻塞：引擎侧一画就塌（**未解决**）

**症状**：静止姿势就已经塌成一片贴在原点附近的地面上，不是"播错动画"。
把 clip 清空（`MYVK_EDITOR_IMPORT` 直接拖入）一样塌 → 与动画无关。

**已经排除的**：

| 假设 | 证伪证据 |
|---|---|
| 权重没归一化 | GLB 里 26134 个顶点权重和**精确等于 1.0000**，0 例外 |
| 关节数超上限（65 > 64） | 着色器 `jointMats[]` 是 bindless 运行时定长；buffer 按需 `newCap` 扩容 |
| 轨道与关节错位 | `extractAnimations` 的 `boneSet` 与 `buildSkeleton` 用的是**同一份** `skin.clusters` 顺序 |
| 只是 FBX 路径的问题 | 换原生 glTF（`loadGLTF`）一样塌，塌法不同但都塌 |
| 资产本身坏了 | Blender 回读同一个文件渲染完全正常（见下图对比） |

**当前怀疑**：`jointMat = invMeshNode · world(joint) · invBind` 这条链对
"mesh 节点带非单位变换、骨架挂在 Armature 子节点下"的资产算错了 ——
AutoRig 产出的 23 骨资产因为 `invMeshNode = I`、静止时 `jointMat ≡ I`，
所以这条路从来没被验证过。

**下一步**（下一步开工就照这个顺序）：

1. 在 `MYVK_ANIM_TEST` 里加一条：对 `animate_girl` 断言 t=0 时
   `jointMatrices[i]` 与单位阵的差 < 1e-5。**静止塌 = 这一条立刻红**，
   比看截图快得多。
2. 打印 `invMeshNode` 与 `invBind[0]`，和 AssetC `--info` 里的
   `joint world` 手算一遍 `I · world · invBind` 是否等于 I。
3. 若确认是节点变换，改 `VMeshLoader`/`MeshLoader` 里 `invMeshNode` 的取法
   （应该取**骨架根节点的父**而不是 mesh 节点）。

---

## 6 · 为什么默认角色暂时没换

`kDefaultCharacterPath` **仍然指向 `default_character.vmesh`**（旧的 23 骨
AutoRig 资产）。理由写在 `editor/EditorScene.cpp` 的注释里：根因没钉死之前
把一个"一进场景就塌"的角色设成所有人的默认，比"暂时还是粗糙模型"更糟。

切换只需要改一行（`editor/EditorScene.cpp:216`），资产和管线都已就位；
`bindDefaultCharacter` 也已经能按扩展名分派 `loadGLTF` / `loadFBX`。
