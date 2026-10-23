# Alicia Solid：默认角色换成二次元少女（骨骼 + 四段基础动画）

> 2026-10-23 · 用户报障（原话）：
> "这个角色动画有问题的话那就不要了，那么请你在网上帮我找一个比较好点的
> 二次元角色 3d 模型绑定上作为初始化角色模型，要求骨骼和对应的基本动画都要有"
>
> 工具：`tools/blender_import_alicia.py`（Blender 4.5.9）
> 资产：`assets/models/characters/alicia.{fbx,vmesh}`（2.0 MB / 7.5 MB 源）
> 状态：**已接入为默认角色，带贴图，`verify_char_locomotion.py` 19/19 全绿**

---

## 0 · 一句话结论

`Animate_Girl` 的问题是**那套资产本身**（无权重网格 + 借来的骨架 + 坏 rest），
不是引擎。换成自带权重和干净骨架的 **Alicia Solid（VRM）** 之后，同一套
FBX → vmesh → 蒙皮渲染链路直接就跑通了：**128 骨、4 段动画、100% 顶点有权重**，
进 Play 就在呼吸、按 W 就走、按 K 就跳。

必须放弃的是"搬 Mixamo 动画"这条路 —— 不是难，是**数学上不成立**（§2 坑 2-甲）。

---

## 1 · 为什么是 Alicia Solid

### 找资产的过程（哪些路走不通）

| 来源 | 结果 |
|---|---|
| itch.io / Fab / Sketchfab | Cloudflare 挡成 403，脚本抓不到 |
| OpenGameArt | 能进，但没有"二次元 + 带骨骼 + 带动画"的成套资产 |
| Sketchfab 免费区 | 多数只有静态网格，动画是视频不是骨骼动画 |
| **GitHub raw（UniVRM 官方测试模型）** | ✅ 直连可下，7.5 MB 单文件 |

### 选中理由

| 项 | 值 |
|---|---|
| 文件 | `AliciaSolid_vrm-0.51.vrm`（glTF 2.0 容器 + VRM 0.51 扩展） |
| 直链 | `raw.githubusercontent.com/vrm-c/UniVRM/master/Tests/Models/Alicia_vrm-0.51/` |
| title / author | `Alicia Solid` / `© DWANGO Co., Ltd.` |
| 主页 | https://3d.nicovideo.jp/alicia/ |
| 许可（VRM meta 原文） | `allowedUserName: Everyone` / `commercialUssageName: Allow` / `violentUssageName: Disallow` / `sexualUssageName: Disallow` / `licenseName: Other` |
| 风格 | 蓝发制服二次元少女，卡通渲染贴图（toon 材质已经烘在贴图里，PBR 下直接能看） |
| 骨骼 | `skins[0].joints = 62`，`humanoid.humanBones = 55` 项标准 VRM 人形 |
| 蒙皮 | 99.8% 顶点有权重 → **不需要绑骨**，这一整块最费力的活直接省掉 |

> ⚠ 商用"允许"但**暴力/性表现禁止**，且要遵守
> https://3d.nicovideo.jp/alicia/rule.html 。当测试/占位角色用没问题；
> 真发产品前请自己过一遍那个 rule 页面。

---

## 2 · 四个必须踩过去的坑

### 坑 1 · AssetC 只吃 FBX，不吃 glb

`AssetC` 里有一道 `isFbx` 闸门：喂 `.glb` 进去直接 `SKIP not_fbx`，不报错、
不产出。所以**Blender 必须导出 FBX**，不能图省事导 glb（`MYVK_EDITOR_IMPORT`
走的那条原生 glTF 路径是给运行时调试用的，不产 `.vmesh`）。

导出参数（**这四个开关的组合是试错试出来的，见坑 2-乙**）：

```python
bpy.ops.export_scene.fbx(
    bake_anim=True,
    bake_anim_use_all_actions=True,      # take 名 = "<armature>|<action>"
    bake_anim_use_nla_strips=False,
    bake_anim_force_startend_keying=False,
    bake_anim_step=1.0,
    bake_anim_simplify_factor=0.0,
    path_mode="COPY", embed_textures=True)
```

代价是 take 名会带 `ALICIA|` 前缀 —— 由**载入层**剥掉（坑 3），而不是逼
Blender 去产裸名。

### 坑 2 · 播出来是静态 T-pose（本轮最大，两层根因）

症状：`ANIM-SKIN` 显示 `clip=[Idle] play=1 joints=128`、帧率正常、日志全绿，
**画面却是不动的 T-pose**。两层根因叠在一起，缺一层都复现不出来。

#### 甲 · 换算链把 rest **local** 当世界矩阵用了

`build_clip()` 的第一版写的是：

```python
A[n]   = (A[父] · R_rest[父]⁻¹) · D_local[n] · R_rest[n]   # ❌
basis  = rest_off[n]⁻¹ · A[父]⁻¹ · A[n]
```

对非根骨，rest local 和 rest world 差着整条父链。结果 `basis` 恰好抵消回 rest
旋转 —— **delta 被自己吃掉了**。直接解析 `.vmesh` 二进制可以证伪：`Idle` 里
`LeftArm` 的旋转是 `0.5736 / 0.8192`，与 rest quat **逐位相同**。

正确写法是 Blender 官方 `matrix_basis` 的语义：**绕骨自己的头转**（世界轴），
父骨的搬动先继承下来。

```python
# 1) 逐骨刚体搬动 M —— "这块骨连同整个子树被搬动了多少"
M = {}
for n in order:
    p = parent_of[n]
    R  = (pose[n][0] if pose.get(n) else I3).to_4x4()
    h  = rest4[n].to_translation()
    about = Matrix.Translation(h) @ R @ Matrix.Translation(-h)
    M[n] = (M[p] @ about) if p else about

# 2) 目标世界矩阵 → Blender 的 basis
for n in order:
    p   = parent_of[n]
    L4  = rest4[n]
    pw  = M[n] @ L4
    if p:
        rel  = rest4[p].inverted() @ L4
        ppw  = M[p] @ rest4[p]
        basis = rel.inverted() @ ppw.inverted() @ pw
    else:
        basis = L4.inverted() @ pw
    pb = arm.pose.bones[n]
    pb.matrix_basis = basis
```

> 关键认识：**蒙皮只关心 `W_t · W_rest⁻¹`**。骨轴 roll 怎么歪都不影响画面 ——
> 所以"用世界轴写 delta"是安全区，"搬别人的世界旋转"不是。

姿态函数里的角度不是猜的，是 `tools/blender_probe_alicia_axes.py` 用
"**顶点实际位移**"量出来的（给某根骨绕某个轴转 45°，看手附近的顶点跑到哪）：

| 骨 | rest 朝向 | 动作 | 世界轴旋转 |
|---|---|---|---|
| 左/右臂 | ∓X（T-pose 水平） | 下垂 | `RY(∓72°)` |
| 手臂 | — | 前后摆 | `RX(swing) @ RY(drop)`（先下垂再摆） |
| 腿 | −Z | 迈步 | `RX(±25°)`，正值朝 +Y（前） |

换下一个 VRM 资产时，先跑这个探针再改角度 —— 不同 rig 的 rest 朝向不一样。


**自检必须是可证伪的物理判据，不能看 `err` 是不是 0。** 第一版自检
`err=0.00000` 却在错，因为错的是 `A` 这个量的**语义**。现在的判据是位置：

```
SELF Idle hand head=(-0.160, 0.001, 0.852) M_err=0.00000
```

T-pose 时 `LeftHand` 在 `z=1.264`（肩高），下垂后应在 `z≈0.85`（胯部）。
脚本里硬编码了这条：`if hand.head.z > 1.10: log("SELF-FAIL ...")`。

#### 乙 · FBX 的"节点基础变换"= 导出那一刻评估出来的 pose

即使曲线全对，导出时如果姿态停在最后一个 clip 的末帧，FBX 会把**那个 pose**
写成节点的基础变换。AssetC 拿它当 `restLocal` / `invBind` → 于是
`W_t · W_rest⁻¹ ≡ I` → 无论曲线怎么变都是原地不动。

所以导出前**必须把所有 pose bone 复位**：

```python
arm.animation_data.action = None
for t in list(arm.animation_data.nla_tracks):
    arm.animation_data.nla_tracks.remove(t)
for name, _rng, _loop in made:
    bpy.data.actions[name].use_fake_user = True      # 清 action 后别被回收

for pb in arm.pose.bones:
    pb.matrix_basis = Matrix.Identity(4)
sc.frame_set(1)
bpy.context.view_layer.update()
log("POSE-RESET LeftHand head=(...) 应为 (−0.497, −0.008, 1.264)")
```

实测输出与期望**逐位相同**，说明 rest 干净了。

#### 四种导出模式对照（都实测过，别再试了）

| 模式 | 结果 |
|---|---|
| NLA strips + 复位 | 四段 clip **全静止**（AssetC `keys 0`） |
| NLA strips + `use_nla=False` | 同上（导出器照当前 rest 姿势烤） |
| NLA strips 静音 / 挪到帧 100+ / `extrapolation=NOTHING` | 导出器直接跳过，**一个 take 都不出** |
| **all-actions + 复位** | ✅ rest 干净 + 曲线完整（`keys 3684`） |

#### 顺手验证的一件事：蒙皮管线本身没问题

临时加过 `MYVK_SKIN_DEBUG=1`（把所有关节矩阵替成"整体上移 0.5 m"）。角色
**确实浮起来了** → 权重是对的、GPU 属性绑定是对的、着色器用的是这些矩阵。
问题从头到尾都在**姿态数据**，不在蒙皮。（这个开关和配套的 `ANIM-DIAG` 日志
已删，别再加回来。）

### 坑 3 · `findClip` 是逐字比较，`|` 前缀必须在**读资产时**剥掉

`Skeleton::findClip(name)` 的实现是 `c.name == name`，**没有归一化、没有
大小写折叠、没有 trim**。名字差一个字符就静默返回 `nullptr` → 不报错 →
角色站成 rest（T-pose）→ 看起来又像"动画坏了"。

Blender 的 `bake_anim_use_all_actions` 产出的 take 名是 `ALICIA|Idle`。
Unity / UE / glTF 转换器一律把 `|` 之后当 clip 名，这里照办 —— 归一化放在
**两个读资产的地方**，`findClip` 的逐字语义保持不变：

| 文件 | 作用 |
|---|---|
| `assetio/FbxImport.cpp` | AssetC 烤 `.vmesh` 时的 clip 名 |
| `engine/assets/FbxLoader.cpp` | 运行期直接读 `.fbx` 的兜底路径 |

```cpp
const std::size_t bar = clip.name.find('|');
if (bar != std::string::npos && bar + 1 < clip.name.size())
    clip.name = clip.name.substr(bar + 1);
```

> 只剥**第一个** `|` 之前的部分，且 `|` 后面必须有内容才剥（`"A|"` 保留）。

⚠ AssetC **只链 `VennAssetIO`、不链 `VennEngine`**（CMakeLists 第 768 行有
注释）。改 `engine/assets/FbxLoader.cpp` 对 AssetC **完全无效** —— 两处都要改，
否则 `AssetC --info` 还会显示 `ALICIA|Idle`。

`EditorScene.cpp` 里留了一条逐字诊断（有意保留，别删）：

```
PLAYER-DEFAULT clipNames=[Idle,Walk,Run,Jump] clipName=[Idle]
```

`AnimatorSystem` 里也有一条**正式保留**的警告，专门治这种静默失败：

```
ANIM-CLIP-MISSING n=1 clipName=[Idle2] available=[Idle,Walk,Run,Jump]
                 → 已静默退回 rest（findClip 是逐字比较）
```

### 坑 4 · 验收陷阱：Character 预览窗口盖在主视口上

**这条最坑，因为它是"误判"而不是"故障"。**

`createPlayer` 成功后会**自动打开 Character 预览窗口**。里面渲染的是场上那个
角色的**另一份副本**，编辑态、`clip` 空、`play=0` —— 也就是静止 rest。
它盖在 3D 视口上面，于是：

- 截"主视口"的图 → 实际拍到预览窗口里的 T-pose
- 日志里 `ANIM-SKIN` 同时出现两个实体：
  `entity=1 clip=[] play=0`（预览副本，静止）
  `entity=2 clip=[Idle] play=1 joints=128`（真身，**一直在动**）

正式判据已经写进 `verify_char_locomotion.py` 的 **C5c0 / C5c**：先点
`VTAB-viewport` 标签把预览窗口挤下去，**等 0.9 s 让自动布局切完**，再比较
间隔 **1.0 s** 的两帧像素差异。

两个数字都是有来历的：

- **1.0 s 不是随便取的**。Idle 的呼吸/重心浮动幅度很小，0.45 s 只有 ~0.03%
  —— 跟视口里 tooltip 文字刷新同量级，分不开。1.0 s 实测 0.47~0.6%。
- **阈值 0.15%**。第一次跑出 0.871% 判 PASS 但角色其实是 T-pose，那 0.871%
  全来自 `Click the viewport to take control` 这行字的刷新，角色区域 diff
  实测 **0 pixel**。所以判据必须**把角色矩形内单独算**（`diff_ratio(a,b,rect)`），
  不能全窗口算。

---

## 3 · 管线（两条命令）

```bash
# 0)（可选）换资产时先标定骨轴，确认 pose_* 里的角度还对得上
"D:/blender/blender.exe" --background --factory-startup \
    --python tools/blender_probe_alicia_axes.py

# 1) VRM → FBX（12 网格 + 128 骨 + 4 段程序化 clip）
"D:/blender/blender.exe" --background --factory-startup \
    --python tools/blender_import_alicia.py -- \
    --vrm E:/code/cpp/_venn_tmp/dl/AliciaSolid.vrm \
    --out assets/models/characters/alicia.fbx

# 2) FBX → vmesh（AssetC 烤：顶点烘到静止世界空间、生成 invBind）
build/bin/AssetC.exe assets/models/characters/alicia.fbx \
    -o assets/models/characters
```

第 2 步之后**必须**用 `--info` 确认 `skinVerts=21310/21310` 和四段 clip 的
`tracks=384` —— 少了任何一项都是坑 2 复现。另外确认材质那几行 `albedo=` 不是
`-`（贴图断链的话角色会渲染成纯白，见 §6）。

产物里除了 `alicia.{fbx,vmesh}` 还有一个 **`alicia.fbm/`**：Blender 用
`path_mode="COPY"` 把 6 张 PNG 拷到那儿，FBX 里存的是**相对路径**
（`alicia.fbm/Alicia_body.png`）。它必须跟着资产树一起走，否则贴图全丢。
（脚本自己那套"落盘到 `alicia_tex/`"已改成写系统临时目录、导出后删除 ——
两套图并存只会让人分不清哪套才是真正被引用的。）

`tools/blender_import_alicia.py` 里**没有 Mixamo 相关代码**了（`--src`、
`import_src`、retarget 全删）—— 只留 `--vrm` / `--out`。不是"忘了"，是那条
路被 §2 坑 2-甲 证伪了。

---

## 4 · 资产自检（`AssetC --info` 原文）

```
ASSETC   submeshes=20 vertices=21310 indices=95394 index16=1
ASSETC   joints=128 clips=4 materials=12
ASSETC   size=1.229 x 0.633 x 1.652 m  height(Z)=1.652 m
ASSETC   clip "Idle" 2.03 s  tracks=384
ASSETC   clip "Walk" 1.03 s  tracks=384
ASSETC   clip "Run"  0.63 s  tracks=384
ASSETC   clip "Jump" 1.00 s  tracks=384
ASSETC   skinVerts=21310 / 21310          ← 100%，没有"漏绑"的顶点
```

- `tracks=384 = 128 × 3`：每根骨一颗平移 + 一颗旋转 + 一颗缩放，四段一致。
- `height(Z)=1.652`：与 VRM 里量到的 1.652 m 一致 → 单位没被缩放。
- 关节顺序是 `Hips → LeftUpLeg → LeftLeg → LeftFoot → …`（深度优先），
  根骨 `Hips` 在 `z=0.9686`，与 VRM 一致。

---

## 5 · 引擎侧接入

`editor/EditorScene.cpp`：

```cpp
constexpr const char* kDefaultCharacterPath =
    "models/characters/alicia.vmesh";
```

**换默认角色 = 换这个常量指向的文件**，别的地方不用动。

`bindDefaultCharacter` 里按扩展名分派（`.glb/.gltf` 走 `loadGLTF`，
其余走 `loadFBX` —— `loadFBX` 会先探同名 `.vmesh`，命中就用烤好的那份）。
夹带的两条日志都是**验收锚点**，不是调试残留：

```
PLAYER-DEFAULT clipNames=[Idle,Walk,Run,Jump] clipName=[Idle]     # 坑 3
PLAYER-DEFAULT bound path=models/characters/alicia.vmesh meshes=20
             skinned=1 joints=128 clips=4 height=1.652 capsule=…    # C1b/C1c
```

`AnimatorSystem::tick` 的 `ANIM-TICK` 也会报 `lastclip=[…] lasttime=…`
（方括号包住，空名可见；**两条路径都收**：默认角色没有 `AnimatorComponent`，
只收 animator 那条会永远打空字符串）。

---

## 6 · 贴图：从"纯白石膏像"到有颜色（三段断点）

角色一开始是**纯白**的。查下来是三段各自断掉，缺任何一段都白：

| # | 断点 | 现象 | 修法 |
|---|---|---|---|
| 1 | VRM 材质是 **MToon**，FBX 导出器只认 Principled | FBX 里搜不到任何 `.png`，12 个材质 `albedo=-` | 导出前把材质**压平**成 `Image Texture → Principled.Base Color` |
| 2 | AssetC 的 `--asset-root` 没人传 | `relativizeTo(p, "")` 直接返回空 → 路径全空 | 没传时按输入路径往上找最近的 `assets` 目录 |
| 3 | `Texture::fromFile` **只按进程 cwd** 打开 | `can't fopen`，材质静默退回默认 | 先过 `assets::resolveAssetPath` 补上 `<exe>/assets` 这个根 |

第 3 条 `TerrainPass` 踩过同一个坑（`docs/launcher_roadmap.md`「地面模糊重影」），
所以是**第二次**踩 —— 凡是"文件里存的相对路径"要交给 `Texture::fromFile`，
都必须先 `resolveAssetPath`。`VMeshLoader` 和 `FbxLoader` 两处都补了。

修复后（`AssetC --info`）：

```
ASSETC   material "Alicia_body" albedo=models/characters/alicia.fbm/Alicia_body.png
ASSETC   material "Alicia_wear" albedo=models/characters/alicia.fbm/Alicia_wear.png
ASSETC   material "Alicia_hair" albedo=models/characters/alicia.fbm/Alicia_hair.png
```

贴图文件落在 `<fbm 名>.fbm/`（Blender `path_mode="COPY"` 的产物），是**相对
FBX 的路径**，可以跟着资产树一起拷走。

### 还剩一个看得见的缺陷：脸

`Alicia_face.png` 有 **97% 像素完全不透明** —— 它就是脸本身（肤色底 + 很淡
的五官），真正的眼睛在 `Alicia_eye` / `Alicia_eye_white` 两块**独立网格**上，
位置在脸皮**里面**，靠 MToon 的描画顺序盖上去。`.vmesh` 没有 `alphaMode`、
导入的材质一律 Opaque，于是眼睛被脸皮整片挡住，看起来就是"白脸 + 一个红嘴巴"。

试过两条"离线删面"的偏方，都不成立（详见 `tools/blender_import_alicia.py`
里那段「关于抠形（已放弃）」）：按贴图透明删面会把身体的头整块删掉（body
贴图头部本来就透明，等 face 层盖住）；只删叠层则毫无变化。

**正确修法是给 `.vmesh` 加 `alphaMode` / 描画顺序** —— 要 bump 格式版本 +
动载入器 + 加着色器变体，回归面很大，不在"换默认角色"这一刀的范围里。

---

## 7 · 已知遗留

1. **`Animate_Girl` 的塌陷机制还在**（`docs/animate_girl_import.md §5`）。
   Alicia 恰好绕开了它：Alicia 的网格节点是单位变换，而那个资产是"网格在
   Armature 子节点下且带非单位变换"。看起来 `invMeshNode · world · invBind`
   这条链在后者上算错了。**没修**，但也不再挡路 —— 新资产不触发。
2. **脸部（眼睛）看不清** —— 见 §6 末，属于格式缺 `alphaMode`，不是这笔改动的活。
3. 四段动画是**程序化正弦/阶跃拼的**，能看、能动、方向对；但不是动捕，
   脚会和地面轻微打滑。真要手感动捕，得先解决 VRM 骨轴重建（§2 坑 2-甲），
   或者直接买带干净 rest 的 FBX（Mixamo 官方下载的那种就没这个毛病）。
4. `default_character.vmesh`（23 骨兜底资产）**保留在盘上**：好几个物理
   用例在自己的场景 JSON 里显式指名它，删掉会让它们从"跑起来"变成
   "找不到模型"。
