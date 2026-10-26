#pragma once
// ============================================================
// assets/Material —— PBR 材质
//
// 参数集对齐 glTF 2.0 的 metallic-roughness 工作流：
//   baseColorFactor / metallicFactor / roughnessFactor / emissive
//   + albedo / normal / ORM 三张纹理
//
// ORM 贴图 = Occlusion-Roughness-Metallic 打包（glTF 标准做法）：
//   R = AO, G = Roughness, B = Metallic
// 三个通道塞进一张图，省采样器和显存。
// ============================================================

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

namespace assets {

class Texture;
// A6：色带资产（弱引用）。**必须是 struct** —— 与 RampAsset.h 的定义一致，
// MSVC 对 struct/class 的名字修饰不同，声明错一个就会在链接期才炸（见
// MEMORY 里 2026-10-23 那次 LNK2019）。
struct RampAsset;

// ============================================================
// MaterialSource —— 材质的"来源描述"（供场景序列化重建）
//
//   Procedural —— 由因子 + 纹理槽直接构成，可以完整序列化
//                 （纹理槽自身也有 TextureSource，见 Texture.h）
//   Model      —— 由 glTF 解析而来，重建方式是重新加载该模型，
//                 所以这里只记模型缓存键即可（贴图一并回来）
//   Instance   —— 母材质的实例（W1）。参数值语义拷贝自母材质，
//                 但只有 `overridden` 位标出的字段才随本地修改。
//   VMAT       —— 来自一个独立的材质资产文件（`assets/materials/*.vmat`，W2-1）。
//                 与 Model 的区别：Model 的身份由模型文件里的材质名决定，
//                 VMAT 的身份由文件自己写（`"id"` 字段），所以**跨关卡可共享**。
// ============================================================
struct MaterialSource {
    enum class Kind { Procedural, Model, Instance, VMAT };

    Kind kind = Kind::Procedural;
    std::string modelKey;  // Model：AssetManager 里的模型缓存键
    std::string vmatPath;  // VMAT：相对 assets/ 的 .vmat 路径

    void setModel(const std::string& key) {
        kind = Kind::Model;
        modelKey = key;
    }
};

struct Material {
    std::string name;

    // ---- 透明模式 ----
    // Opaque：参与深度预通道，不透明管线绘制
    // Blend ：不参与深度预通道，混合管线绘制（不写深度、按到相机距离降序排序）
    // Mask  ：alpha 裁切（alpha test）。参与深度预通道、走不透明管线（写深度），
    //         但片元在 alpha < alphaCutoff 时直接 discard —— 于是"镂空"的
    //         部分既不写颜色也不写深度，从背面能直接看到透光。植被叶子、
    //         栅栏、铁丝网靠它，代价只有一次比较。**筛孔边缘是硬的**
    //         （没有半透明过渡），要高频道效果再叠抖动模式（见 ditherEnabled）。
    // 最终 alpha = baseColorFactor.a × albedo 贴图的 alpha 通道
    enum class AlphaMode { Opaque, Blend, Mask };
    AlphaMode alphaMode = AlphaMode::Opaque;

    // Mask 模式的裁切阈值：alpha < cutoff 的片元被丢掉。
    // 默认 0.5 是业界惯例（也正好是"sRGB 贴图里 0.5 线性亮度"这种直觉值）。
    // 只在 alphaMode == Mask 时参与着色器编译（其它模式这段代码整段编掉）。
    float alphaCutoff = 0.5f;

    // 抖动裁切（stochastic alpha test）：Mask 模式下不 discard，而是把
    // alpha 与一个屏幕空间蓝噪声阈值比较后写成 0/1 —— 得到"筛网边缘有
    // 概率穿透"的效果。配合 TAA（R1-1）多帧累积，边缘会被时间滤波成
    // 柔和的半透明观感，同时**保留写深度**（不像 Blend 那样排序崩坏）。
    // 关闭时（默认）Mask 走原样的硬 discard 路径 → 逐像素不变。
    bool ditherAlpha = false;

    // ============================================================
    // 渲染顺序（P0-2/B 类，2026-10-25）
    //
    // 为什么材质上需要"顺序"这个字段：
    //   透明物过去只按 `viewDepthSq` 单键降序排。这对"玻璃、烟雾"这类
    //   物理上真的分前后的东西是对的，但对**二次元角色**完全不成立 ——
    //   脸皮、眼白、虹膜、睫毛、眉毛、刘海几乎在同一个球面上，到相机的
    //   距离差不到 1 mm，谁前谁后由浮点噪声决定，于是逐帧闪烁。
    //   更糟的是二次元有一条硬规矩：**眉毛要穿透刘海**。这两者物理上
    //   就是"刘海在前"，距离排序给出的一定是错的答案。
    //
    // 所以答案只能是"由美术指定"，引擎不再猜：
    //   renderQueue —— 主键，这个部件属于哪个"带"（数值小 = 先画 = 在下）
    //   sortOrder   —— 次键，同带内的微调（0 是默认，正数往上层推）
    //
    // 排序键变成 (renderQueue, sortOrder, -viewDepthSq)：
    //   前两个是**语义**，最后一个是**物理**。同一带内仍然按距离排，
    //   所以玻璃/烟雾那类东西的既有行为一分不变；跨带时距离完全不参与，
    //   "眉毛永远在刘海之上"就是一条字面意义上的保证。
    //
    // ⚠ 这两个字段和 alphaMode 是**正交**的：alphaMode 决定"怎么混合"，
    //   renderQueue 决定"什么时候画"。一个不透明材质照样可以有
    //   renderQueue（比如想让脸皮排在身体之后，好在它上面叠东西）。
    //
    // 默认 2000 / 0 = 现有行为（所有材质同带、同序 → 只按距离排）。
    // 这条"默认值等于旧行为"不是巧合，是 P0-1 那条"关 = 基线"约定
    // 在这一层的延续：没设过这两个字段的场景，逐像素与改造前一致。
    // ------------------------------------------------------------
    // 参考带序（美术起点，**不是引擎规则** —— 引擎只保证升序绘制）：
    //   1900 背景/天空   2000 不透明身体/衣服   2400 脸皮
    //   2900 后发        3000 前发/刘海         3100 眼白+虹膜
    //   3200 睫毛        3300 眉毛              3400 腮红/贴花
    // 注意头发(3000) 必须小于眼睛(3100)：二次元的眼睛要压在刘海上。
    // "按部件类别自动分类"行不通，就是这个反例 —— 头发既要在脸之前，
    // 又要在眼睛之后，任何类别枚举都表达不了。
    std::int32_t renderQueue = 2000;
    std::int32_t sortOrder = 0;

    // 该材质是否走"覆盖带"（不写深度 + 深度测试恒通过）。
    //
    // 判据是 `renderQueue >= kOverlayQueueStart`，**不是**另存一个 bool：
    // 两个字段描述同一件事就会有不一致的那一天（设了 queue=3100 却忘了
    // 勾 overlay，于是眼睛又被脸皮挡回去）。
    static constexpr std::int32_t kOverlayQueueStart = 3000;
    bool isOverlay() const { return renderQueue >= kOverlayQueueStart; }

    // ---- 着色模型（R4-2）----
    // Default：标准金属-粗糙度 BRDF（漫反射 = max(N·L,0)）。
    // Skin   ：在此之上叠一层**预积分次表面散射** —— 用 (N·L, 曲率) 查一张
    //          预烘焙的散射 LUT 代替朴素的 max(N·L,0)，光按扩散核"绕过"
    //          明暗交界线，且红通道散射最远 → 鼻梁/耳缘/指尖泛红透光。
    //
    // "曲率"由片元处**几何法线**的屏幕空间导数（fwidth）近似，所以这个
    // 效果**不需要任何额外顶点数据或贴图**：同一张网格换成 Skin 就有反应，
    // 平坦的地方（脸颊正面）几乎与 Default 一致，只有法线变化快的地方
    // （轮廓边缘、鼻梁、耳缘）才明显。
    //
    // 语义上等价于"给这个材质换一个漫反射响应函数"，所以走**关键字变体**
    // （bit4 VENN_SHADING_SKIN）而不是运行时 uniform：Default 材质那段代码
    // 整段编掉 → 逐像素不变。
    //
    // Toon（A1，2026-10-25）：二次元赛璐璐。与 Skin 同为"换一个漫反射响应"，
    // 但方向相反 —— Skin 是让明暗交界**更软**（光绕过交界线），Toon 是让
    // 它**变成一条线**（半兰伯特 + 两档量化）。阴影部分是乘冷色调而不是
    // 单纯变暗，这是二次元和写实最本质的一条差异：阴影有颜色，不是没有光。
    //
    // 三者**互斥**（一个材质只能是一种着色模型）。互斥关系由
    // keywordsOf() 的 switch 保证 —— 它不会同时置两个位。
    enum class ShadingModel { Default, Skin, Toon };
    ShadingModel shadingModel = ShadingModel::Default;

    // 枚举 → 存档/材质文件里的字符串。序列化与 Inspector 共用这一个函数，
    // 免得"UI 加了 Toon、存档却只认 skin/default"这类单边改动。
    static const char* shadingModelName(ShadingModel m) {
        switch (m) {
            case ShadingModel::Skin: return "skin";
            case ShadingModel::Toon: return "toon";
            case ShadingModel::Default:
            default: return "default";
        }
    }
    static ShadingModel shadingModelFromName(const std::string& s) {
        if (s == "skin") return ShadingModel::Skin;
        if (s == "toon") return ShadingModel::Toon;
        return ShadingModel::Default;
    }

    // ---- 因子（线性空间）----
    glm::vec4 baseColorFactor{1.0f};
    float metallic = 0.0f;
    float roughness = 0.5f;
    float normalScale = 1.0f;
    float ao = 1.0f;
    glm::vec3 emissive{0.0f};  // 自发光，HDR 下可 > 1 触发 bloom

    // ---- 纹理槽 ----
    // nullptr 表示使用内置默认纹理；此时值等价于"中性"，只剩因子生效
    Texture* albedoMap = nullptr;  // 默认白 → albedo = baseColorFactor
    Texture* normalMap = nullptr;  // 默认 (0.5,0.5,1) → 平坦法线
    Texture* ormMap = nullptr;     // 默认白 → ao/rough/metallic 均为 1

    // ---- 单通道因子贴图（UE5 风格：每个因子旁边可以再挂一张图）----
    //
    // 与上面的 ormMap 是**相乘**关系，不是二选一：
    //     rough = roughnessFactor × ormMap.g × roughnessMap.r
    //     metal = metallicFactor  × ormMap.b × metallicMap.r
    // 未绑定时绑定内置的纯白（1,1,1）→ 乘法退化成恒等，行为与加这两个槽
    // 之前逐像素一致。glTF 导进来的材质走打包的 ORM，手工搭的材质则可以
    // 一张一张单独指定（只有粗糙度图、没有金属度图是很常见的）。
    //
    // 只读 R 通道（灰度图），所以按 **线性** 采样（srgb=false）：粗糙度是
    // 一个物理量，不是颜色，做 sRGB 解码会把数值整体抬亮。
    Texture* roughnessMap = nullptr;   // 默认白 → 只吃 roughnessFactor
    Texture* metallicMap = nullptr;    // 默认白 → 只吃 metallicFactor

    // 自发光贴图（RGB 颜色贴图，srgb=true）。
    // 这里是**相加**而不是相乘：emissive 因子默认是 0（不发光），若做乘法
    // 那么"拖一张自发光图进来却什么都不亮"——得先去把因子调上去才知道生效。
    //     emissive = emissiveFactor + emissiveMap.rgb
    // 未绑定时绑定内置纯黑（0,0,0）→ 加法退化成恒等。
    Texture* emissiveMap = nullptr;    // 默认黑

    // ---- 视差遮挡贴图（POM，R4-3）----
    //
    // heightMap：单通道高度场（只读 R，**线性**采样 srgb=false —— 高度是几何量
    //   不是颜色）。挂了它 → 关键字 bit5 VENN_PARALLAX 置位，片元在采样
    //   albedo / normal / orm / roughness / metallic / emissive **之前**先沿
    //   切线空间视向把 UV 推开一段 —— 于是平面上的砖缝/凹槽会随视角"错位"，
    //   看起来真的有凹凸。没挂它 → 那段代码整段编掉，逐像素不变。
    //
    // heightScale：视差深度，单位是 **UV**（不是米）—— 一条射线在 UV 空间里
    //   最多能推出多远。0.02~0.1 是常见区间（砖墙取小的、石板取大的）。
    //   **= 0 时 offset 恒为 0 → 与没挂高度图逐像素相同**，这正是
    //   "关闭 = 基线"那条约定的可断言形式。
    //
    // ⚠ 高度图**不能**当普通贴图理解：它描述的是"表面起伏"，所以：
    //   · 采样必须是线性的（sRGB 解码会把 0.5 抬成 0.21，高度全塌）；
    //   · 地址模式最好是 REPEAT（与 albedo 一致，否则 UV 推出边界会出现
    //     一条硬缝 —— 视差一旦生效，UV 必然会被推出 [0,1]）；
    //   · 没有高度数据时应当绑**纯黑**（0）而不是白：白 = 处处等高 = 射线
    //     会一路走到层数上限，退化成一个大偏移。黑 = 处处最低 = 立即停下
    //     → offset 0。这就是缺省槽位绑 m_defaultBlack 的原因。
    Texture* heightMap = nullptr;
    float heightScale = 0.05f;

    // ============================================================
    // A 类语义槽（2026-10-25，材质页 32 B → 96 B）
    //
    // 这四个槽解决的是 P0-2/A 那条"同像素叠加"：SDF 阴影 / ramp /
    // 腮红贴花 / 眼睛 matcap。它们不参与排序，是在着色器里当场算完的。
    //
    // **为什么这些参数只能放在这里**：push 常量恰好 128 B 已经满了，
    // 纹理 set 也满 7 个 —— 逐材质浮点在全引擎**只有材质页这一个地方**
    // 能放（见 render/Bindless.h 的容量账）。
    //
    // **缺省值全部是乘法恒等**（贴图缺省白、强度缺省 0 或 1），于是
    // "没挂贴图"必然逐像素回到基线。这不是巧合，是 P0-1 那条
    // "关 = 基线"约定在这一层的可断言形式：
    //     rampMap = null   → 程序化 ramp × 1  = 原来的程序化 ramp
    //     matcapStrength=0 → 加项恒为 +0
    //     sdfShadowMap=null→ 受光系数 × 1
    //     detailMap = null → albedo × 1
    // ------------------------------------------------------------
    // Toon 的明暗渐变带。**只读 R**，按 NdotL 映射后的位置采样。
    // 为什么需要它：程序化 ramp（A1）只有固定的两三档，而二次元的
    // 明暗交界"硬到什么程度、阴影是什么颜色"是每个角色一套的画风，
    // 只能由美术画成一张 1×N 的图。
    //   rampSteps = 0 → 直接按 NdotL 采样（贴图自带多少档就是多少档）
    //   rampSteps > 0 → 先把 NdotL 量化成这么多档，再采
    //                   （避免贴图被线性采样插成连续渐变 —— 那就不叫
    //                    赛璐璐了。量化后采样点落在每个色带的中心。）
    Texture* rampMap = nullptr;
    float rampSteps = 0.0f;

    // MatCap 球面高光（眼睛、发丝、金属小件）。
    // 按**视图空间法线**的 xy 当 UV 采样 —— 于是高光"贴在物体身上"
    // 而不是随光源动，这正是二次元想要的：眼睛的高光不该因为角色转身
    // 就消失。matcapRotate 用来把高光转到美术想要的那个角度。
    Texture* matcapMap = nullptr;
    float matcapStrength = 0.0f;   // 0 = 不参与（缺省）
    float matcapRotate = 0.0f;     // 弧度

    // 面部 SDF 阴影图（左右对称的一张图，只读 R）。
    // 与"用法线算阴影"的本质区别：法线算出来的明暗交界线会跟着脸的
    // 几何抖，而 SDF 图里的交界是由**美术画的曲线**决定的 —— 二次元
    // 脸上那条干净的鼻影就是这么来的。**缺省白 = 处处受光**。
    //
    // ⚠ 现在还只是通道打通 + 乘法调制，完整的算法（按头部空间算投影
    //   方向、UV 要和头部 unwrap 对齐）是下一刀的事 —— 见 §11.9。
    Texture* sdfShadowMap = nullptr;
    float sdfShadowStrength = 1.0f;

    // ---- 面部 SDF 的采样参数（A1，2026-10-26）----
    // SDF 图是"正脸投影"下的灰度遮罩，运行时按光照方向偏移采样点
    // （见 pbr.frag 的 faceSdfMask）。shift 就是那个偏移的幅度：
    //   0    = 不随光动（图怎么画就怎么显示，只适合固定机位的立绘）
    //   0.15 = 常规（光转 90° 时采样点移动 15% 的脸宽）
    //   >0.3 = 夸张（阴影几乎完全由光的方向决定，图只提供形状）
    // ⚠ 依赖"脸皮的 UV 就是正脸展开"这个资产约定 —— UV 不对，阴影就会
    //   出现在脸上任意位置。这是资产问题，着色器兜不住。
    float sdfFaceShift = 0.15f;

    // ---- Toon 色带资产（A6，2026-10-26）----
    // `ramp` 是**弱引用**（AssetManager 持有，材质不拥有）；`rampMap` 是它
    // 烘出来的 256×1 纹理，之所以两个都留着：渲染只认 `rampMap`（与其它
    // 纹理槽一致），而编辑/存盘/换档位要认 `ramp`（拿到 bands 数据）。
    // ⚠ 重建纹理后 **`rampMap` 必须重新指向** `ramp->texture.get()`，
    //   否则材质页还在采旧纹理（旧对象已被 unique_ptr 释放）。
    // `rampPath` 是相对 assets/ 的 .vramp 路径；空 = 程序化 / 还没存盘。
    std::string rampPath;
    RampAsset* ramp = nullptr;
    // 明暗交界（band0 ↔ band1）的位置与软化宽度。**与资产里的同名值
    // 是同一组数**：资产负责烘像素，这里负责着色器的"阴影闸门"，两边
    // 不同步就会出现双重明暗线。所以编辑色带时要一起写。
    float rampEdge = 0.5f;
    float rampSoftness = 0.04f;

    // ---- 角色光通道（A5，2026-10-26）----
    // 勾上 = 这个材质只吃"角色光"（专用主光 + 常量环境色），不吃场景
    // IBL / 太阳 / CSM。二次元角色几乎都要勾；道具/场景**不要**勾。
    // 缺省 false = 与引入这条通道之前逐像素相同（"关 = 基线"）。
    bool useCharacterLight = false;

    // ---- 头发各向异性高光（A4，2026-10-26）----
    // Kajiya-Kay 天使环。二次元头发的"亮"不是一片，是绕着头顶的一条带子 ——
    // 由**切线**（顶点属性 location 9，沿 UV.x）而不是法线决定，见
    // pbr.frag 的 hairAnisoSpec。
    //
    // anisoStrength：0 = 关（缺省，逐像素等于基线）；0.6~1.2 是头发常用区间。
    //   超过 1.5 会把整条环推到削顶（一条死白的带子），不像高光了。
    // anisoShift：把环沿**法线**推着走的位置参数。
    //   0     = 主环贴着 N·T 的原始位置
    //   ±0.3  = 环上下移动，配合美术手画的需求
    //   ⚠ 它不是"亮度"参数 —— 调它只会让环移动，不会变亮变暗。
    //
    // ⚠ 这两个数只对 **Toon** 材质生效（Kajiya-Kay 是赛璐璐语汇，
    //   Default/Skin 那条 PBR 分支没有这个项）。
    float anisoStrength = 0.0f;
    float anisoShift = 0.0f;

    // ---- 眼睛专用着色（A3，2026-10-26）----
    // 二次元的眼睛**不是算出来的，是画出来的**：底色（巩膜白 + 虹膜渐变 +
    // 瞳孔）是美术出的一张图，场景光不该改变它；立体感只靠两件事 ——
    // 一层"贴上去"的高光片（MatCap 或下面的眼神光）、以及虹膜的**视差**
    // （眼窝是凹的，侧看时虹膜会朝观察者一侧偏）。
    //
    // eyeStrength：Unlit 混合权重。0 = 关（缺省，逐像素等于基线）；
    //   1 = 完全不吃场景光（只保留 albedo 底色 + 眼神光）。
    //   中间值可以给"半透的塑料眼"之类留余地，但二次元角色几乎总是 1。
    // ⚠ 它是**混合权重**而不是布尔：写成 mix() 才能让"0 = 基线"逐像素成立；
    //   写成 `if (eyeStrength > 0)` 也能关，但美术拿不到 0.8 这一档。
    //
    // eyeIrisDepth：虹膜视差的强度（沿切线平面偏移 UV 的比例）。
    //   0    = 不偏移（正视平贴，像贴纸）
    //   0.02 = 常规（人眼的眼球半径/可视宽度量级）
    //   0.05 = 夸张（"玻璃珠"那种水润感）
    //   ⚠ 只对 Unlit 的眼睛有意义的量；它偏移的是 **matUV**（所有贴图
    //     采样点一起动），所以虹膜、瞳孔、巩膜会一起位移 —— 这是对的，
    //     因为它们本来就在同一张图里、贴在同一个凹面上。
    //
    // eyeCatchlight：程序化眼神光的强度。二次元眼睛那颗"白点"是画上去的，
    //   位置固定在**头的朝向**上（视图空间法线），不随光源走 —— 这与 MatCap
    //   同理，区别是它不需要美术再出一张图。
    //   0 = 关（缺省）；1.0 左右是常规；>2 会削顶成死白。
    float eyeStrength = 0.0f;
    float eyeIrisDepth = 0.0f;
    float eyeCatchlight = 0.0f;

    // 细节/贴花（腮红、纹身、衣服印花）。**相乘**调制 albedo，
    // detailTiling 控制平铺次数。缺省白 → 乘法恒等。
    Texture* detailMap = nullptr;
    float detailStrength = 1.0f;
    float detailTiling = 1.0f;

    // glTF 双面标记（当前管线仍按背面剔除处理，留作扩展）
    bool doubleSided = false;

    // ---- 来源溯源（序列化用）----
    MaterialSource source;

    // ============================================================
    // 稳定身份（W0-2）
    //
    // 以前材质的"身份"是 `m_materials` 的缓存键，而模型材质那个键是
    // `models/xxx.glb/mat0` —— **数组下标**。模型重导出时材质顺序一变，
    // 所有引用静默错位（mat0 从 Skin 变成 Hair，没有任何报错）。
    //
    // `id` 是给人看、给存档用的语义身份：`models/xxx.glb#Skin`。
    // ⚠ 缓存键（`name`）**保持不变** —— `resolveMaterial` / `findMaterial` /
    //   `Model::subMeshes[i].material` 多处依赖它，改名会动 5 处以上。
    //   加一个并列字段、只让序列化用它，是成本最低且不破坏任何现有路径的做法。
    // ============================================================
    std::string id;

    // ============================================================
    // 可覆盖字段的参数集（W1 的位掩码就是这个字段序）
    //
    // 把「可被实例覆盖 / 被存档记录的标量字段」收成一个可拷贝的
    // 结构，是为了三件事共用一份定义：
    //   · W0-3：导入模型的材质，靠它算出"用户到底改过哪几个字段"
    //   · W1-1：实例的 `overridden` 位掩码（位序写死在下面）
    //   · W1-2：母材质变更时按位刷新未覆盖字段
    //
    // ⚠ 纹理槽（11 个）**不进参数集**：换纹理是"结构变化"，
    //   一律视为覆盖（见 kOvAllTextures），不进位掩码。
    // ============================================================
    struct Params {
        glm::vec4 baseColorFactor{1.0f};
        float metallic = 0.0f;
        float roughness = 0.5f;
        float normalScale = 1.0f;
        float ao = 1.0f;
        glm::vec3 emissive{0.0f};
        AlphaMode alphaMode = AlphaMode::Opaque;
        float alphaCutoff = 0.5f;
        bool ditherAlpha = false;
        ShadingModel shadingModel = ShadingModel::Default;
        bool doubleSided = false;
        float heightScale = 0.05f;
        // P0-2/B：渲染顺序（2026-10-25 追加，位序 12/13）
        std::int32_t renderQueue = 2000;
        std::int32_t sortOrder = 0;
        // A 类语义槽的六个标量（2026-10-25，位序 14..19）
        float rampSteps = 0.0f;
        float matcapStrength = 0.0f;
        float matcapRotate = 0.0f;
        float sdfShadowStrength = 1.0f;
        float detailStrength = 1.0f;
        float detailTiling = 1.0f;
        // A6/A1（2026-10-26，位序 20/21）：色带的交界位置与宽度
        // ⚠ 只进位掩码的**标量**。`rampPath` / `sdfFaceShift` /
        //   `useCharacterLight` 刻意不进：它们不是"画风旋钮"，而是
        //   "这个材质是哪一类东西"的身份标记（换色带 / 开关角色光属于
        //   结构变化，与换纹理槽同一条纪律）。
        float rampEdge = 0.5f;
        float rampSoftness = 0.04f;
        // A4（2026-10-26，位序 22/23）：头发各向异性高光的强度与环位置
        float anisoStrength = 0.0f;
        float anisoShift = 0.0f;
        // A3（2026-10-26，位序 24/25/26）：眼睛的 unlit 权重 / 虹膜视差 / 眼神光
        float eyeStrength = 0.0f;
        float eyeIrisDepth = 0.0f;
        float eyeCatchlight = 0.0f;
    };

    // 位序（W1-1）—— ⚠ 一旦发布就不能改：存档里是裸数字。
    // 升位序必须同时升 `kMaterialVersion` 并写迁移。
    enum OverrideBits : std::uint32_t {
        kOvBaseColor   = 1u << 0,
        kOvMetallic    = 1u << 1,
        kOvRoughness   = 1u << 2,
        kOvNormalScale = 1u << 3,
        kOvAo          = 1u << 4,
        kOvEmissive    = 1u << 5,
        kOvAlphaMode   = 1u << 6,
        kOvAlphaCutoff = 1u << 7,
        kOvDitherAlpha = 1u << 8,
        kOvShadingModel = 1u << 9,
        kOvDoubleSided = 1u << 10,
        kOvHeightScale = 1u << 11,
        // ---- 12/13（2026-10-25 追加，P0-2/B 渲染顺序）----
        // 追加而不插队：存档里是裸数字，插队等于把所有旧存档的
        // heightScale 之后全部错位。这条纪律见文件上方的位序说明。
        kOvRenderQueue = 1u << 12,
        kOvSortOrder   = 1u << 13,
        // ---- 14..19（2026-10-25 追加，A 类语义槽的标量）----
        // 同样追加不插队。这六个字段是"每角色一套画风"的旋钮，
        // 必须能被 .vmat 共享、被实例覆盖，所以必须进位掩码。
        // ⚠ 纹理槽不进这里（换纹理是结构变化，一律视为覆盖），
        //   所以四个新贴图槽**没有**对应的位 —— 与已有的 7 个槽一致。
        kOvRampSteps         = 1u << 14,
        kOvMatcapStrength    = 1u << 15,
        kOvMatcapRotate      = 1u << 16,
        kOvSdfShadowStrength = 1u << 17,
        kOvDetailStrength    = 1u << 18,
        kOvDetailTiling      = 1u << 19,
        // ---- 20/21（2026-10-26 追加，A6 色带的交界位置/宽度）----
        kOvRampEdge          = 1u << 20,
        kOvRampSoftness      = 1u << 21,
        // ---- 22/23（2026-10-26 追加，A4 头发各向异性）----
        // 同样是"画风旋钮"而不是身份标记：同一个头发材质要做"干发/湿发"
        // 两种劲度，靠的就是实例覆盖这两个数。
        kOvAnisoStrength     = 1u << 22,
        kOvAnisoShift        = 1u << 23,
        // ---- 24/25/26（2026-10-26 追加，A3 眼睛）----
        // 三个都是"每个角色一套画风"的旋钮（同一张眼睛图要做"普通/水润/
        // 死鱼眼"），所以进位掩码可以被实例覆盖。
        kOvEyeStrength       = 1u << 24,
        kOvEyeIrisDepth      = 1u << 25,
        kOvEyeCatchlight     = 1u << 26,
        // ⚠ 掩码跟着扩。位序 27..31 是最后 **5** 个空位 —— 用完之后
        //   `Params` 这个"C 风格 POD 快照"就得换结构（位掩码是 u32），
        //   届时要升 kMaterialVersion 并写迁移，不能硬塞第二个 u32。
        kOvScalarMask  = (1u << 27) - 1u,
    };

    // 取当前参数 / 写回参数
    Params captureParams() const {
        Params p;
        p.baseColorFactor = baseColorFactor;
        p.metallic = metallic;
        p.roughness = roughness;
        p.normalScale = normalScale;
        p.ao = ao;
        p.emissive = emissive;
        p.alphaMode = alphaMode;
        p.alphaCutoff = alphaCutoff;
        p.ditherAlpha = ditherAlpha;
        p.shadingModel = shadingModel;
        p.doubleSided = doubleSided;
        p.heightScale = heightScale;
        p.renderQueue = renderQueue;
        p.sortOrder = sortOrder;
        p.rampSteps = rampSteps;
        p.matcapStrength = matcapStrength;
        p.matcapRotate = matcapRotate;
        p.sdfShadowStrength = sdfShadowStrength;
        p.detailStrength = detailStrength;
        p.detailTiling = detailTiling;
        p.rampEdge = rampEdge;
        p.rampSoftness = rampSoftness;
        p.anisoStrength = anisoStrength;
        p.anisoShift = anisoShift;
        p.eyeStrength = eyeStrength;
        p.eyeIrisDepth = eyeIrisDepth;
        p.eyeCatchlight = eyeCatchlight;
        return p;
    }
    void applyParams(const Params& p) {
        baseColorFactor = p.baseColorFactor;
        metallic = p.metallic;
        roughness = p.roughness;
        normalScale = p.normalScale;
        ao = p.ao;
        emissive = p.emissive;
        alphaMode = p.alphaMode;
        alphaCutoff = p.alphaCutoff;
        ditherAlpha = p.ditherAlpha;
        shadingModel = p.shadingModel;
        doubleSided = p.doubleSided;
        heightScale = p.heightScale;
        renderQueue = p.renderQueue;
        sortOrder = p.sortOrder;
        rampSteps = p.rampSteps;
        matcapStrength = p.matcapStrength;
        matcapRotate = p.matcapRotate;
        sdfShadowStrength = p.sdfShadowStrength;
        detailStrength = p.detailStrength;
        detailTiling = p.detailTiling;
        rampEdge = p.rampEdge;
        rampSoftness = p.rampSoftness;
        anisoStrength = p.anisoStrength;
        anisoShift = p.anisoShift;
        eyeStrength = p.eyeStrength;
        eyeIrisDepth = p.eyeIrisDepth;
        eyeCatchlight = p.eyeCatchlight;
    }

    // 逐字段比较，返回"不同的那几位"（0 = 完全一致）。
    // ⚠ 浮点用精确相等而不是 eps：来源同为 `json.value(...)`，
    //   同一份文件读两次逐位相同，不需要容差（见 W0-1 的判据说明）。
    static bool sameVec4(const glm::vec4& a, const glm::vec4& b) {
        return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
    }
    static bool sameVec3(const glm::vec3& a, const glm::vec3& b) {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }
    static std::uint32_t diffParams(const Params& a, const Params& b) {
        std::uint32_t d = 0;
        if (!sameVec4(a.baseColorFactor, b.baseColorFactor)) d |= kOvBaseColor;
        if (a.metallic != b.metallic) d |= kOvMetallic;
        if (a.roughness != b.roughness) d |= kOvRoughness;
        if (a.normalScale != b.normalScale) d |= kOvNormalScale;
        if (a.ao != b.ao) d |= kOvAo;
        if (!sameVec3(a.emissive, b.emissive)) d |= kOvEmissive;
        if (a.alphaMode != b.alphaMode) d |= kOvAlphaMode;
        if (a.alphaCutoff != b.alphaCutoff) d |= kOvAlphaCutoff;
        if (a.ditherAlpha != b.ditherAlpha) d |= kOvDitherAlpha;
        if (a.shadingModel != b.shadingModel) d |= kOvShadingModel;
        if (a.doubleSided != b.doubleSided) d |= kOvDoubleSided;
        if (a.heightScale != b.heightScale) d |= kOvHeightScale;
        if (a.renderQueue != b.renderQueue) d |= kOvRenderQueue;
        if (a.sortOrder != b.sortOrder) d |= kOvSortOrder;
        if (a.rampSteps != b.rampSteps) d |= kOvRampSteps;
        if (a.matcapStrength != b.matcapStrength) d |= kOvMatcapStrength;
        if (a.matcapRotate != b.matcapRotate) d |= kOvMatcapRotate;
        if (a.sdfShadowStrength != b.sdfShadowStrength) d |= kOvSdfShadowStrength;
        if (a.detailStrength != b.detailStrength) d |= kOvDetailStrength;
        if (a.detailTiling != b.detailTiling) d |= kOvDetailTiling;
        if (a.rampEdge != b.rampEdge) d |= kOvRampEdge;
        if (a.rampSoftness != b.rampSoftness) d |= kOvRampSoftness;
        if (a.anisoStrength != b.anisoStrength) d |= kOvAnisoStrength;
        if (a.anisoShift != b.anisoShift) d |= kOvAnisoShift;
        if (a.eyeStrength != b.eyeStrength) d |= kOvEyeStrength;
        if (a.eyeIrisDepth != b.eyeIrisDepth) d |= kOvEyeIrisDepth;
        if (a.eyeCatchlight != b.eyeCatchlight) d |= kOvEyeCatchlight;
        return d;
    }
    // 按位把 b 的字段刷进 a（用于母材质变更传播 / 读档应用 overrides）
    static void mergeParams(Params& a, const Params& b, std::uint32_t bits) {
        if (bits & kOvBaseColor) a.baseColorFactor = b.baseColorFactor;
        if (bits & kOvMetallic) a.metallic = b.metallic;
        if (bits & kOvRoughness) a.roughness = b.roughness;
        if (bits & kOvNormalScale) a.normalScale = b.normalScale;
        if (bits & kOvAo) a.ao = b.ao;
        if (bits & kOvEmissive) a.emissive = b.emissive;
        if (bits & kOvAlphaMode) a.alphaMode = b.alphaMode;
        if (bits & kOvAlphaCutoff) a.alphaCutoff = b.alphaCutoff;
        if (bits & kOvDitherAlpha) a.ditherAlpha = b.ditherAlpha;
        if (bits & kOvShadingModel) a.shadingModel = b.shadingModel;
        if (bits & kOvDoubleSided) a.doubleSided = b.doubleSided;
        if (bits & kOvHeightScale) a.heightScale = b.heightScale;
        if (bits & kOvRenderQueue) a.renderQueue = b.renderQueue;
        if (bits & kOvSortOrder) a.sortOrder = b.sortOrder;
        if (bits & kOvRampSteps) a.rampSteps = b.rampSteps;
        if (bits & kOvMatcapStrength) a.matcapStrength = b.matcapStrength;
        if (bits & kOvMatcapRotate) a.matcapRotate = b.matcapRotate;
        if (bits & kOvSdfShadowStrength) a.sdfShadowStrength = b.sdfShadowStrength;
        if (bits & kOvDetailStrength) a.detailStrength = b.detailStrength;
        if (bits & kOvDetailTiling) a.detailTiling = b.detailTiling;
        if (bits & kOvRampEdge) a.rampEdge = b.rampEdge;
        if (bits & kOvRampSoftness) a.rampSoftness = b.rampSoftness;
        if (bits & kOvAnisoStrength) a.anisoStrength = b.anisoStrength;
        if (bits & kOvAnisoShift) a.anisoShift = b.anisoShift;
        if (bits & kOvEyeStrength) a.eyeStrength = b.eyeStrength;
        if (bits & kOvEyeIrisDepth) a.eyeIrisDepth = b.eyeIrisDepth;
        if (bits & kOvEyeCatchlight) a.eyeCatchlight = b.eyeCatchlight;
    }

    // ---- 导入快照（W0-3）----
    // 只有 `Kind::Model` 的材质会填：记录"模型刚加载出来时"的参数。
    // 有了它才能回答"用户到底改过哪几个字段"，进而只把差的那些写进存档
    // （只写覆盖项，见 world_scale_roadmap D2）。
    Params importDefaults;
    bool hasImportDefaults = false;

    void captureImportDefaults() {
        importDefaults = captureParams();
        hasImportDefaults = true;
    }

    // ---- 实例语义（W1-1）----
    // 母材质（nullptr = 自己是母材质）。值语义：实例的参数字段就是真值，
    // `overridden` 只决定"母材质变了要不要跟进"。
    // ⚠ 永远指向 AssetManager 里由 unique_ptr 持有的对象 —— 材质在本进程内
    //   不销毁，所以裸指针安全（`m_materials` 只增不删）。
    const Material* parent = nullptr;
    std::uint32_t overridden = 0;

    bool isInstance() const { return parent != nullptr; }

    // 两级限制：实例不能再当母材质（见 AssetManager::makeInstanceOf）
    bool canBeParent() const { return parent == nullptr; }

    // 把 child 里**未被覆盖**的字段刷成 parent 的值（W1-2 的核心算法）。
    // 返回 1 = 真的刷了，0 = 全被覆盖（跳过）。
    //
    // 抽成静态自由函数而不是留在 AssetManager 里，是为了让自检能在
    // **没有 AssetManager、没有 GPU、没有窗口** 的机器上直接测这段逻辑
    // （AssetManager 的构造需要一个 TextureContext = 真设备）。
    static int syncInstanceParams(Material& child, const Material& parent) {
        const std::uint32_t follow = kOvScalarMask & ~child.overridden;
        if (!follow) return 0;
        Params p = child.captureParams();
        mergeParams(p, parent.captureParams(), follow);
        child.applyParams(p);
        return 1;
    }
};

} // namespace assets
