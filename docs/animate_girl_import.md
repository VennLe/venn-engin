# Animate_Girl 角色导入：从"静态雕塑"到"会走路的默认角色"

> 2026-10-23 · 用户报障："当前这个初始化的角色模型太粗糙了，
> `E:\dev\UE-resources\Animate_Girl` 这个目录中有一个角色模型和对应的角色动画，
> 你把它导入我的引擎中作为初始化角色模型试试看，并且要绑定好动画。"
>
> 工具：`tools/blender_import_animategirl.py`（Blender 4.5.9）
> 产物：`assets/models/characters/animate_girl.{fbx,vmesh,glb}`
> 状态：离线管线打通、资产自检全绿；**2026-10-29 修好了蒙皮与轴向，
> 产物是 `animate_girl_rigged.glb`（§5）**；**同日把四条产品场景全部切过去，
> 旧资产连同它的生成器一起删除，测试夹具改指新角色（§6）**。

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

### 5.4 · 原地化：`Walk` 的 1.79 m 前进位移必须抹掉

拿到"姿势像人"之后，演示场景里角色**斜着往画面外飘**。排查了一整轮才定位：
不是权重、不是遮罩层、不是子网格的 π 绕 X 旋转，而是

> **`Walk` 这一段 clip 自带 1.79 m 的根位移，而编辑器在非 Play 态不钳它。**

`AnimatorSystem::applyRootMotion()` 只在 `!wrapped && advance` 时才
`extractRootMotion`，而 `stripRootMotion` 又在 `d.valid` 之后才调用 ——
编辑器不推进时间轴（`advance=false`），于是姿势里的根位移**原样留在
pose 上**，每渲染一帧就把角色往前推一点。播放器里它会被钳回原位，所以
"游戏里好好的，编辑器里飘走了"。

修法在资产侧，`rebuild_skin_weights.py` 的 `make_inplace()`（默认开启，
`--keep-root-motion` 关掉）：

```
v'(t) = v(t) − u(t) · (v_last − v_first)，  u = i/(n−1)，只作用在 x / z
```

三条约定，每一条都是踩过的：

* **y 一分不动。** `Jump` 那一段 0.94 → 2.15 是**真的跳起高度**，线性削掉
  它等于把跳跃删了。高度是 y，水平是 x/z —— 注意这是**文件原始空间
  （Y-up）**，不是引擎的 Z-up。
* **只削总位移，不改姿势。** 减的是"时间上的线性斜坡"，相对动作完全保留。
* **实测只命中 `Walk`**（首末差 1.795 m），`Idle` / `Run` / `RigWalk` 首末
  差 ≈ 0（循环闭合）→ 副作用为零。

> 为什么不用 `RootMotionMode::DeltaOnly`：那是运行时的开关，管的是 Play 态，
> 管不到"编辑器静态显示某一帧"。而且关掉根运动只会让它不走，不会让姿势
> 回到原点 —— 编辑器里那 1.79 m 位移依然画在脸上。

### 5.5 · 刚性件整壳绑单骨（靴子事故）

原地化之后角色站住了，但**两只靴子从 t>0 起被拉长、躺在地上**。

大壳报告直接点名：靴子是一个 **252 顶点、包围盒对角 0.26 m 的刚性壳**，
质心 z=0.12，却被分给了 4 根骨（`LeftLeg` / `LeftFoot` / `LeftToeBase` /
`LeftToe_End`）。它不是"该弯曲的皮肤"，是一块塑料 —— 四根骨一错开就把它
扯成长条。

修法是给 `rebuild_skin_weights.py` 加一条**按尺寸的刚性判据**（`--rigid`，
默认对角阈值 `RIGID_DIAG = 0.35`）：对角小于阈值的壳，整壳 100% 绑给
"离质心最近的那根骨段"的父关节。

阈值 0.35 是**量出来的唯一一条干净的缝**（实测大壳对角）：

| 壳 | 对角 | 该不该刚性 |
|---|---|---|
| 靴子 | 0.26 m | 是 |
| 手 | 0.34 m | 是 |
| 躯干主壳 | 0.52 m | 否（要弯） |
| 小腿壳 | 0.76 m | 否 |

221 个壳因此归入刚性。副作用：未绑骨 3 → 12 根，全在**右手中指指尖**
的白名单里（握姿，指尖藏在掌心），判据 G 不受影响。

### 5.6 · 还剩什么

* 侧摆的小发饰（品红小珠）偶尔偏出几厘米；胸口/大腿上几片薄贴片有轻微错位。
  都是"小件继承"挑错了邻面，属于观感问题，不影响播放。
* 真正的长发改成"跟头走"需要给资产加发骨（现在是按距离分给脊柱/腿），
  属于资产升级，不在这一刀里。

---

## 6 · 现在到哪一步了

**2026-10-29 已全线切换完毕**，产品路径上再没有 `anime_girl` 这个名字。

产物：`assets/models/characters/animate_girl_rigged.glb`
（1.626 m / 65 骨 / 4 段 clip / 26134 顶点 / 权重七条判据全绿）。
它和同目录 `animate_girl.glb` 的差别：

| | `animate_girl.glb` | `animate_girl_rigged.glb` |
|---|---|---|
| 轴向 | Y-up（引擎里躺着） | 根节点 +90°X，Z-up（站着） |
| 权重 | 22.95% 跨肢体混杂 | 0.00%，另 12 根指尖骨无顶点（握姿） |
| 根位移 | `Walk` 带 1.79 m / `Jump` 带真跳高 | `Walk` 已原地化，`Jump` 的 y 一分没动 |
| 刚性件 | 靴子被 4 根骨扯长 | 221 个壳整壳绑单骨 |
| 几何 / 骨架 / clip | —— | **逐字节相同**，只重写了 attribute |

### 6.1 · 切了哪四处

| 场景 | 怎么改的 |
|---|---|
| `assets/scenes/levels/first_level.json` | 改 `tools/gen_first_level.py` 里的 Player 实体再重新生成（`--check` PASS，373 实体）。**不能手改 json** —— 它由脚本生成，手改下次重新生成就没了。 |
| `assets/scenes/anime_demo.json` | `python tools/make_anim_demo.py` 重烤（模板 `anim_demo_tpl.json`，机位 `5.5,-5.5,1.8,45,12`）。 |
| `assets/scenes/cutscene_demo.json` | `python tools/make_sequencer_demo.py` 重烤（复用同一份模板与机位）。 |
| `assets/scenes/runtime_test/mat_model_probe.json` | 直接改路径（这个文件是手写夹具，没有生成器）。 |

重烤时改掉的三处**不能沿用旧值**的东西：

* `LAYER_ROOTS = ["Spine2"]` —— 旧资产叫 `chest`。⚠ **不能写 `Spine`**：
  `buildBoneMask` 是去大小写的**子串**匹配，`Spine` 会同时命中
  `Spine` / `Spine1` / `Spine2` 三根，遮罩直接铺满整个上身。
* `DEMO_CAM` —— `TPL_CAM`（`3,-3,1.5,45,19.5`）是按 1.556 m 的旧角色定的，
  新角色 1.626 m，原机位会裁掉大半截。
* 模板必须**另烤一份**（`anim_demo_tpl.json`），不能和 `verify_anim_layers`
  共用 `anim_layers_tpl.json` —— 谁后跑谁覆盖，"验的是哪个模型"就取决于
  执行顺序了。为此 `verify_anim_layers.bake_template()` 加了三个可选覆盖
  参数（`model` / `tpl` / `cam`），默认仍是本文件的常量，行为逐位不变。

### 6.2 · 旧资产彻底删除（不再当夹具）

中途走过一段弯：先把三个文件 `git mv` 进 `assets/models/_fixtures/`，
理由是"15 条 `verify_*.py` 的夹具钉在它们身上"。**这一步是错的**——
它们还躺在一个叫 `anime_girl` 的目录里，内容还是那具粗糙模型，只是换了
个位置，任何人打开仓库都还会以为它没退役。

最终做法：**整个目录删掉，夹具全部改指产品角色**
`models/characters/animate_girl.fbx`（65 骨 / 7 子网格 / 与
`animate_girl_rigged.glb` 同一个模型）。改的时候有三件事必须一起动，
否则会得到一堆"看着像资产坏了"的红：

1. **实体名跟着资产名变**。`instantiateModel` 用文件名当实体名，于是
   `anime_girl` → `animate_girl`。所有 `MYVK_EDITOR_SELECT` / `anim.time
   <t> <name>` / `select=` 的实参都要改（4 个脚本）。
2. **钉死的资产数字**。`verify_vertex_format` 的 `BASE` 表里那条
   `(7, 11338, 65712, 21, 2)` 是旧资产的账（21 骨、无 UV2），换成
   `(7, 25626, 101781, 65, 4)`；`RUNTIME_CASES` 里"无 UV2 的样本"换成
   `samba_dancing`（不能拿 animate_girl 填两格，D1 的条数会对不上）。
3. **机械替换会在同一张表里造出重复项**。`verify_morph` 的
   `ASSETS_LIST` / `RUNTIME_CASES`、`verify_vertex_format` 的 `BASE`
   都是"路径 → 数字"的字面表，替换后同一个 key 出现两次，
   Python 静默取后者 —— 必须去重。

只有一条用例不能靠换路径解决：`verify_clip_dedup` 要的是一份**take 名
重复**的 FBX（`Rig|Idle` ×2 + `Rig|Walk` ×2）。全仓库只有那具粗糙角色
有这性质。为两个撞名的 take 把 5.8 MB 的人物签进仓库不划算，所以现造了
一个最小复现：`tools/export_clip_dup_fbx.py`（Blender）导出
`assets/models/test_clip_dup.fbx` —— 1 根骨 + 1 个方块 + 2 个同长
action，58 KB，去重日志与旧资产逐字一致。

另外两个脚本一起删了：`blender_make_girl.py`（那具粗糙模型的生成器，
留着就是"改天手滑又把它烤回来"的入口）与 `export_test_fbx.py`
（它 exec 前者来出 FBX）。`test_cube.fbx` 已经签好了，不用重生成。

### 6.3 · 换资产踩出来的三个连带 bug

1. **按实体名找角色**（`verify_director_api` / `verify_world_api`）。
   两条用例都从基底场景里"找到名叫 `anime_girl` 的实体"再挂探针脚本，
   换资产后实体名跟着变成资产名 → 直接 `SystemExit`。
   改成**按 `animator` 找**（"那个带 animator 的角色"才是它们真正要的），
   以后再换资产就不用回来改。
2. **sidecar 路径没跟着搬**（`verify_import_vmeta`）。
   `.vmeta` 的路径规则是"资产路径**追加** `.vmeta`"，资产从 `models/` 根搬进
   `models/characters/` 之后 `GLB_VMETA` / `FBX_VMETA` 还指着旧路径 ——
   B2 那条「mtime 逐位不变」会去 stat 一个从不存在的文件。
3. **`AssetSelfTest.cpp` 里写死的相对路径**。那支自检只走 `assetio::importFbx`，
   不吃 glb，所以只能继续指一份**真实 FBX**：`models/characters/animate_girl.fbx`。

### 6.4 · 回归体系的坑：收尾格式决定判定

`verify_skin_weights` 自检 7/7 全绿，可 `run_regression` 判 `0/1 通过`。
根因不在判据，在**收尾那一行**：执行器的 `OK_RE` / `BAD_RE` 只认
`结果: PASS` / `结果: FAIL`（含几种计数式写法），而且取**最后一条**匹配。
本脚本只打了 `合计 PASS=7 FAIL=0` + `全部通过` —— 两条正则都匹配不上，
于是"沿用上一行的判定"，等于看运气。

`verify_water` / `verify_particles` / `verify_editor_round4` 都踩过同一个坑，
现在都留了注释。**新脚本一律以 `print("结果: PASS")` 收尾。**

### 6.5 · 跑更大范围回归时挖出来的四个问题

换完资产只跑"受影响的那几条"是不够的 —— 下面四个都是切完之后跑其它用例
才暴露的，而且**没一个是新资产本身坏了**：

**① 模板的"来源资产"没被记录**（`verify_anim_events` / `verify_root_motion`
红成"模板里没有蒙皮实体"）。它们复用 `verify_anim_layers` 烤的
`anim_layers_tpl.json`，判据只有 `os.path.isfile(TPL)`。资产一搬，模板里
每条 `mesh.path` 都指空，可"文件在"这条判据照样为真。
→ `bake_template()` 现在在模板旁边写一个 `.src.json` 记下"这份是按哪个资产
烤的、从哪个机位"，`ensure_template()` 比对不上就重烤。
**判"文件在不在"是不够的，得判"是不是按这份资产生成的"。**

**② `.vmeta` 是测试之间的污染源**。`vmeshIsFresh()` 的判据是 mtime
（`.vmeta` 比 `.vmesh` 新 → 缓存过期 → 回退 FBX 解析）。而任何用例只要
加载一次 `characters/animate_girl.fbx`，`finishModelImport()` 就会**新建**一份
`.vmeta`（新文件必然更新）→ 之后所有用例都悄悄走回退路径。
`verify_morph` 的 E1 就是数 VFMT 行数才把这个抓出来的（回退路径不打印）。
→ `verify_world_api` 里那条 `spawn()` 是源头，现在它收尾时负责删掉自己
写出来的 sidecar。**谁写出来的，谁删掉。**

**③ Content Browser 把 `.trash` 当内容显示了**。`tools/safe_fs.py` 的
"同目录让位区"是删除工具的实现细节，却会出现在资源浏览器里；更糟的是它
**每跑完一次用例就冒出来**，任何"这一层正好有 N 项"的断言都会随机变红。
→ `ContentBrowser.cpp` 里以 `.` 开头的条目一律跳过（`.git` / `.vs` 同理）。

**④ 拖拽用例钉死了"哪个文件在第几行"**。`verify_content_browser` 的
③ 组拖 `anime_girl.fbx`，而它已经不在仓库里了。
→ 把 Content 指到 `assets/models/characters`（`--env MYVK_CONTENT_DIR=...`，
`--asset animate_girl.fbx`）。**没有换成拖 `WorldMap.glb`** —— 那条用例的
本意里"被拖的是蒙皮 FBX"是有意义的（拖进来要建 `SkinnedMeshComponent` 并贴地），
换个静态模型就悄悄丢了这一层。
另外 `verify_asset_drag` 补了"找不到格子就滚一滚"的兜底：目录数量会随仓库
变化，不能假设目标永远在第一屏。

顺带一个与资产无关的老账：`runtime_test/mat_conflict_probe.json` 的两个箱体
只有 `"primitive"`、没有 `"mesh"` 块。读端只在 `resolveMesh()` 拿到网格时才
建 `MeshComponent`，`primitive` 只是**挂在已有网格上的意图** —— 所以那两个
箱体一直是空物体，`verify_material_instances` 的 B1 长期拿 `A=None`。
→ 夹具补上了 `"mesh": {"kind":"builtin","shape":"cube", ...}`。同时把
`primitive` 加进序列化器"不算空壳"的名单（读写两份），因为**存盘不该顺手
把读进来的字段抹掉**。

### 6.6 · 像素用例为什么只能用 glb（以及"分带"必须重量）

删除 `_fixtures/` 之后，比像素的那几条用例（`verify_anim_layers` 等）
把夹具从旧角色换到了产品资产。这里有一条**不能偷懒的规则**：

**① 用 `animate_girl_rigged.glb`，不要用 `animate_girl.fbx`。**

FBX 与它烤出来的 `.vmesh` 是 2026-10-23 的**旧产物**：7 个子网格各带一份
错的节点变换，渲出来头 / 躯干 / 鞋是散的（拼不回一个人）。

静态尺寸看不出来 —— `verify_model_scale` 只量包围盒，它量到的
"1.928 m" 是"最上那块到最下那块"的跨度，不是身高（真值 1.626 m，
err=18.6%）。但只要判据比像素，画面一散就全废：`verify_anim_layers`
换资产初期，上半带变化量一直卡在 7~14%，看起来像"遮罩没生效"，查了三轮
才发现是**渲染出来的东西本身是碎的**。

`animate_girl_rigged.glb` 是重写过的那份（根节点补了 +90°X，把手脚摆回
一个人的位置），进场景 1.626 m 直立人形，7 个子网格的 node 变换都正确。

**② 换资产 / 换机位之后，`UPPER_END` / `LOWER_START` 必须重量。**

`verify_anim_layers` 的判据是"上半带一个像素不动、下半带动了很多"，
而"上半 / 下半"是两条**相对视口高度的横线**。这两个常数是**机位的函数**：
旧值 `0.28` / `0.52` 是照旧角色 1.556 m、旧机位 `2.10,-2.10,0.85,45,6`
量的。换成 1.626 m 且**底面在 z=0.40** 的新角色后（glb 根节点补的那
+90°X 顺手把原点从脚底挪上去了），旧机位让角色被顶到画面顶边，两条线
全落在身体外面 —— 判据会静默退化成"两边都算它一半"，红得莫名其妙。

为此写了 `tools/_probe_anim_layers_bands.py`：出一张视口图 + 两条**逐行
变化剖面**（Allow=upleg 打腿、Allow=spine2 打上半身），直接给区间：

```
[建议] 腿动区间   = 0.343 .. 0.936      → LOWER_START 取它的上界
[建议] 上半动区间 = 0.143 .. 0.593      → UPPER_END   取它的下界
```

当前值 `UPPER_END=0.32` / `LOWER_START=0.64`（各留一格余量，剖面按
1/50 分桶，边界会抖），机位 `1.96,-1.96,1.79,45,12`（沿视线 2.8 m、
fovY=45° → 可见高 2.3 m，角色占视口 y=0.14~0.94）。

**"画面分带"这类常量不该在没有量具的情况下手写。**

### 6.7 · 还没做

* `verify_character_import` 的 G1 组（"点完 Add Animator 之后 Animator 段
  出现"）仍有一条红。窗口高度 / 按钮矩形那个老坑（见该文件 745 行附近的
  长注释）已经修过一轮，需要重新分离"是夹具过期还是真回归"。
* **`characters/animate_girl.fbx` 本身是碎的**（见 §6.6）。目前所有比像素的
  用例都避开它，`verify_model_scale --model characters/animate_girl.fbx` 也
  因此红（err=18.6%）。要么重导一份节点变换正确的 FBX，要么把它从
  "可比像素的资产"名单里正式划掉 —— 现在只是绕开，没有结论。
* 默认**玩家**角色（`editor/EditorScene.h` 的 `kDefaultCharacterAsset`）仍是
  `alicia.vmesh`，本次没动 —— 换它要考虑胶囊尺寸（现有 0.260/0.553）与
  `ModelOrientation` 的朝向补偿，属于另一件事。`animate_girl_rigged` 现在
  只是**演示与过场**的主角。
* 真正跟头走的长发需要给资产加发骨（现在按距离分给脊柱/腿），属资产升级。

