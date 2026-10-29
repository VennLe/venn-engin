# Animate_Girl 角色导入：从"静态雕塑"到"会走路的默认角色"

> 2026-10-23 · 用户报障："当前这个初始化的角色模型太粗糙了，
> `E:\dev\UE-resources\Animate_Girl` 这个目录中有一个角色模型和对应的角色动画，
> 你把它导入我的引擎中作为初始化角色模型试试看，并且要绑定好动画。"
>
> 工具：`tools/blender_import_animategirl.py`（Blender 4.5.9）
> 产物：`assets/models/characters/animate_girl.{fbx,vmesh,glb}`
> 状态：离线管线打通、资产自检全绿；**2026-10-29 修好了蒙皮与轴向，
> 产物是 `animate_girl_rigged.glb`（§5）**。演示场景的迁移还没做（§6）。

---

## 0 · 一句话结论

用户给的"一个模型 + 一组动画"**并不能直接插到一起用**：模型是纯静态网格
（没有骨骼、没有权重），动画是 65 骨的 Mixamo 骨架（没有网格）。中间缺的
那一层是**蒙皮权重**。补法是 Blender 骨热绑骨 + 视觉烘帧（`Copy Location/
Rotation` + `nla.bake`），产物四段 clip 名字精确对齐引擎的
`LocomotionConfig`（Idle/Walk/Run/Jump）。

**几何与 clip 侧全部达标**（见 §4），**蒙皮与轴向当年不通**（§5）。
§5 记录了 2026-10-29 的结案：根因不是引擎，是"Y-up 喂给 Z-up" +
"骨热在 1181 个断壳上给出的权重是跨肢体混杂的"。修完的产物是
`animate_girl_rigged.glb`，七条数值判据全绿。

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

## 5 · 阻塞：引擎侧一画就塌（**2026-10-29 已解决，根因不是引擎**）

**当时的症状**：静止姿势就已经塌成一片贴在原点附近的地面上，不是"播错动画"。
把 clip 清空（`MYVK_EDITOR_IMPORT` 直接拖入）一样塌 → 与动画无关。

**当时排除的**（这张表本身是对的，值得保留）：

| 假设 | 证伪证据 |
|---|---|
| 权重没归一化 | GLB 里 26134 个顶点权重和**精确等于 1.0000**，0 例外 |
| 关节数超上限（65 > 64） | 着色器 `jointMats[]` 是 bindless 运行时定长；buffer 按需 `newCap` 扩容 |
| 轨道与关节错位 | `extractAnimations` 的 `boneSet` 与 `buildSkeleton` 用的是**同一份** `skin.clusters` 顺序 |
| 只是 FBX 路径的问题 | 换原生 glTF（`loadGLTF`）一样塌，塌法不同但都塌 |
| 资产本身坏了 | Blender 回读同一个文件渲染完全正常 |

**当时的怀疑是错的**。写在这里是为了记住那条教训：§5 把矛头指向
`invMeshNode · world · invBind`，可那三个量在**静止姿势下必然互相抵消**
（`world_rest · inv(world_rest) = I`），再怎么取错也不会让静止的模型变形。
一个"静止姿势变形"的症状，几何上就不可能由蒙皮矩阵引起。

### 真正的根因（两个，都在资产侧）

**根因 1 · Y-up 资产喂给了 Z-up 引擎。**
`animate_girl.glb` 是标准 glTF（+Y 上，Hips 在 y=1.112）。而这个仓库的
约定是**glTF 资产自带 Z-up**（旧资产 `anime_girl.glb` 的 Hips 在 z=-0.87）。
`MeshLoader` 里那个 `kYupToZup` 对蒙皮网格会被 `invMeshNode · world` 约掉
—— 蒙皮顶点只走 `jointMat`，不走 `SubMesh::transform` 的那一份。
所以资产是 Y-up，画出来就是**躺着**。

修法：给根节点（`Armature`）加一个 +90° 绕 X 的旋转。因为整棵树只有这一个
根，这就是"把轴烘进去"，子节点一个都不用动，`invBind` 也不用重算
（旋转在 `world(j)` 和 `meshNodeWorld` 两侧同时出现，静止时照样抵消）。

**根因 2 · 权重是跨肢体混杂的垃圾。**
见 §5.1。这条才是"腿被拉长、碎块飞散"的来源。

### 5.1 · 为什么"权重和 = 1"什么都没证明

这是整件事最值得记住的一句：

> **权重归一化只证明"权重是一组概率"，不证明"这组概率是对的"。**

实测反例 —— 2026-10-23 那版资产，胸口的一个顶点（y=1.21，正中偏前）：

```
Hips 31% + LeftUpLeg 29% + HeadTop_End 27% + RightUpLeg 13%     和 = 100%
```

四个方向同时拉同一个点。归一化满分，一播动画就撕开。
全资产按同一条判据统计：**22.95% 的顶点存在"手臂骨 + 腿骨同时拉"**，
另有 **16 根骨一个顶点都没绑到**（含 `Head` 和 `Neck`）。

成因是 Blender 骨热（`ARMATURE_AUTO`）在 1181 个断壳上的失败模式：
逐壳绑骨"成功"了（123/123 壳有顶点组、权重和 1.0、不抛异常），
但小壳上的热扩散无解，返回的是**跨肢体的任意组合**。

### 5.2 · 修法：不碰 Blender，直接重写 JOINTS_0 / WEIGHTS_0

`tools/rebuild_skin_weights.py`（约 1 秒跑完，纯 Python，确定性）：

1. 骨段 = "父关节 → 本关节"，权重记在**父关节**上。
   记给子关节的话，骨盆整圈会判给左右大腿（实测：质心 (0,1.018,-0.045)
   的壳，15 根影响骨里主导是 `LeftLeg`）。
2. **按壳限定候选骨**：先算连通壳，用壳质心附近的骨决定这个壳能用哪些骨。
3. **层级跳数约束**：只允许与主导骨在关节树里 ≤2 跳的骨参与混合。
   没有这条，垂在胯边的手会被及膝长发同时拉给 `LARM` 和 `LLEG`
   （8.4% 的顶点），一播动画就上下撕开。2 跳刚好：`Spine2 → LeftShoulder
   → LeftArm` 是 2 跳（肩该混），`LeftHand → LeftUpLeg` 要 6 跳（该断）。
4. **小件（包围盒对角 < 0.12 m，共 887 个）继承最近大壳顶点的权重**。
   纽扣、发饰、贴片本来就贴在身体表面，让它跟"贴着的那个面"一起动，
   而不是自己比"离哪根骨近"。
5. 壳内两轮邻接平滑（只对 >300 顶点的大壳）。
6. 收尾：掐掉残余的"手臂+腿"组合（不重新分配，只丢占比小的一侧）。

结果：病态混杂 **22.95% → 0.00%**，未绑骨 **16 → 0**（只剩 3 根右手中指
指尖骨，因为那只手是握姿、指尖藏在掌心里）。

### 5.3 · 验收：`tools/verify_skin_weights.py`

七条判据，每条都能单独变红：A 权重和 / B 影响数 / C 跨肢体混杂 /
D 主导骨距离 / E 左右不串边 / F 静止 `jointMat ≈ I` / G 骨覆盖。

**必须带 `--baseline` 跑**：同一个判据拿旧资产（`animate_girl.glb`）做对照，
它必须在 C / E / G 上判红（22.95% / 2.73% / 16 根未绑）。**判据必须能证伪
它要防的那个错，不能证伪的判据等于没测。**

过程中被实测否掉的两条判据（保留在这里，免得下次再写一遍）：

* **逐骨比左右总权重** —— 那只握拳的手，右手中指骨确实一个顶点都没有，
  红的是"解剖不对称"不是权重错。
* **整条肢体比总质量** —— 她是"一腿前一腿后"的站姿，`LLEG`/`RLEG` 的
  顶点数天然差 40%（旧资产同样 48%），比不出真假。

换成几何判据（"右半身的顶点不许被 Right 侧骨占去大头"）就干净了：
它只跟 `JOINTS_0` 的下标有关，与这具身体长什么样无关。

**刻意不做像素判据**：`rebuild_skin_weights.py` 是确定性的，数值判据覆盖的
是同一个事实，截图只是同一件事更慢更脆的版本。截图留给人工复核
（`tools/_shot.py`，跑完的图在 `_venn_verify/probe_agirl/`）。

### 5.4 · 还剩什么

* 侧摆的小发饰（品红小珠）偶尔偏出几厘米；胸口/大腿上几片薄贴片有轻微错位。
  都是"小件继承"挑错了邻面，属于观感问题，不影响播放。
* 真正的长发改成"跟头走"需要给资产加发骨（现在是按距离分给脊柱/腿），
  属于资产升级，不在这一刀里。

---

## 6 · 现在到哪一步了

`docs/animate_girl_import.md` 这一版之前，默认角色是 `alicia.vmesh`，
而粗糙的 `anime_girl` 是**动画演示场景**的主体 —— 用户看到"粗糙"的地方
是 `assets/scenes/anime_demo.json` 那个演示，不是玩家角色。

**2026-10-29 已经拿到一个可用的产物**：
`assets/models/characters/animate_girl_rigged.glb`
（1.626 m / 65 骨 / 4 段 clip / 26134 顶点 / 权重七条判据全绿）。
它和 `animate_girl.glb` 的关系只有两处差别：

| | `animate_girl.glb` | `animate_girl_rigged.glb` |
|---|---|---|
| 轴向 | Y-up（引擎里躺着） | 根节点 +90°X，Z-up（站着） |
| 权重 | 22.95% 跨肢体混杂 | 0.00%，另 3 根指尖骨无顶点（握姿） |
| 几何 / 骨架 / clip | —— | **逐字节相同**，只重写了两个 attribute |

### 还没做：把演示场景切过去

这不是"改一行"。`assets/scenes/anime_demo.json` 是按**旧资产的具体形状**
写的：7 个子网格实体、按 `#Skin` / `#Hair` / … 命名的材质 id、
分层遮罩的根骨名（`chest` / `hips`，新资产是 `mixamorig:Spine2` / `Hips`）、
事件表按 clip 名匹配。而 `tools/` 下另有 13 个 `verify_*.py` 的夹具与断言
钉在旧资产的旋转/缩放/骨名/clip 名上。

一次全切会让十几条用例同时变红，分不清"新模型坏了"还是"夹具过期了"。
所以正确的顺序是：

1. 先切 `first_level.json` 里那一个用 `.glb` 的实体（结构最简单）；
2. 再按 `tools/make_anim_demo.py` 重烤演示场景（要同时把 `LAYER_ROOTS`、
   `BASE_CLIP`/`LAYER_CLIP` 换成新资产的骨名与 clip 名）；
3. 最后才动那 13 个 verify 的夹具，一轮切一两个，切完立刻跑。

玩家默认角色（`editor/EditorScene.h` 的 `kDefaultCharacterAsset`）仍是
`alicia.vmesh`，本次没动 —— 换它要考虑胶囊尺寸（现有 0.260/0.553）与
`ModelOrientation` 的朝向补偿，属于另一件事。
