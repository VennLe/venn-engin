#pragma once
// ============================================================
// assets/Mesh —— 网格（CPU 数据 + GPU 顶点/索引缓冲）
// 顶点布局：pos(3f) + normal(3f) + uv(2f) + joints(u4) + weights(4f)
//           + color(4f) + uv2(2f) + morphBase(u1) + morphCount(u1)
//           + tangent(4f) = 112 字节
// 内置：立方体 / 平面生成器（CCW 正面、外法线）
// ============================================================

#include "assets/MorphRig.h"
#include "rhi/VulkanCommon.h"
#include <cstddef>
#include <string>
#include <vector>

namespace rhi {
class Device;
class CommandPool;
class Buffer;
}

namespace assets {

class LodChain;   // assets/Lod.h

// morphBase 的哨兵：这个顶点不参与任何 morph target。
// ⚠ 必须与 `assetio::kNoMorphBase`（assetio/Vertex.h）同值，
//   VMeshLoader.cpp 里有 static_assert 锁死。
constexpr uint32_t kNoMorphBase = 0xFFFFFFFFu;

struct Vertex {
    glm::vec3 pos;
    glm::vec3 normal;
    glm::vec2 uv;
    // ---- 蒙皮（glTF JOINTS_0 / WEIGHTS_0）----
    // 非蒙皮网格保持全 0：着色器里按 weights 之和 > 0 分支走蒙皮路径，
    // 这样静态网格与蒙皮网格共用同一条管线，不用双管线。
    // joints 存的是"骨架内 joint 下标"（0..numJoints-1），渲染时加上
    // PushConstants::skin.x 的 jointBase 定位到 SSBO 里的具体矩阵。
    glm::uvec4 joints{0, 0, 0, 0};
    glm::vec4 weights{0.0f, 0.0f, 0.0f, 0.0f};
    // ---- 顶点色（Mesh Paint）----
    // 默认全 1 = "不染色"：着色器里直接乘 albedo，所以没画过的网格与
    // 加这个通道之前**逐像素一致**。放在蒙皮属性之后是因为：
    //   · glTF / FBX 的 COLOR_0 是可选的，加载器不填时拿默认白；
    //   · LOD 合并要做颜色平均（见 Lod.cpp）；
    //   · 不打断已有的 location 0..4 —— depth_only / shadow 那几个着色器
    //     只声明 0 号属性，多一个不会让它们的顶点布局失效。
    // 64 → 80 字节。80 不是 2 的幂，但 Vulkan 只要求**属性偏移**对齐到
    // 分量大小（vec4 = 4 字节），stride 本身没有对齐约束。
    glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};

    // ---- 以下三个通道 2026-10-25 追加（§13），格式已落盘但**暂无消费者** ----
    //
    // 第二套 UV（面部 SDF / lightmap）。缺省全 0 = "没有"。用不用由
    // **材质**决定，没有逐顶点标志位 —— 见 assetio/Vertex.h 的说明。
    glm::vec2 uv2{0.0f, 0.0f};
    // morph target 稀疏流的起始下标（MORPH 区块）。哨兵 = 不参与。
    //
    // ⚠ 必须带 NSDMI：`std::vector<Vertex>(n)` 是值初始化，没有 NSDMI
    //   的成员会被清零，而 0 是**合法**的流下标。症状见 assetio/Vertex.h。
    std::uint32_t morphBase{kNoMorphBase};
    // delta 流里的记录条数（§14）。与 morphBase 严格自洽：
    //   morphBase == kNoMorphBase  ⇔  morphCount == 0
    // 上一刀它叫 `reserved0`（"必须是 0"），这一刀是它的第一次使用 ——
    // 当初留这 4 字节就是为了"以后再要一个 u32 不必动 stride"。
    std::uint32_t morphCount{0u};
    // 切线：xyz = 沿 UV.x 的单位向量，w = 副切线手性（±1）。0 = 没有。
    glm::vec4 tangent{0.0f, 0.0f, 0.0f, 0.0f};

    static VkVertexInputBindingDescription bindingDescription() {
        VkVertexInputBindingDescription b{};
        b.binding = 0;
        b.stride = sizeof(Vertex);
        b.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return b;
    }

    // 主前向管线真正消费的属性集合。
    //
    // ⚠ 这个集合**不是**"结构体有几个通道"，而是"顶点着色器读哪几个
    //   location"，而且它**不是连续前缀**：
    //     pbr.vert  读 0..5（pos/normal/uv/joints/weights/color）+ 7/8（morph）
    //               + 9（tangent，A4 各向异性头发起才有人读）
    //     —— 只剩 6（uv2）无人读
    //   多喂一条没被消费的属性，校验层会报
    //       VUID-VkGraphicsPipelineCreateInfo-Input-07904
    //   而**少**喂一条被消费的是未定义行为（读到的全是 0，
    //   对 morph 来说正好是"静默不形变"，最难发现的一种）。
    //
    //   所以属性表按 location **挑着给**，不能再用"前 N 条"。
    //   加新通道时只有两处要改：这张表 + 顶点着色器的 location 声明。
    static std::vector<VkVertexInputAttributeDescription> attributeDescriptions() {
        return attributesFor({0, 1, 2, 3, 4, 5, 7, 8, 9});
    }

    // 阴影管线（shadow.vert / CSM 与局部光源共用一个着色器）。
    // 它不读顶点色，但读 morph —— 影子必须跟着表情走，否则脸在笑、
    // 影子还是静止的（见 §15.5）。
    static std::vector<VkVertexInputAttributeDescription> attributeDescriptionsShadow() {
        return attributesFor({0, 1, 2, 3, 4, 7, 8});
    }

    // 连续前缀版（0..count-1）。只给"真的只读前几条"的管线用：
    //   decal.vert 读 0..4 → 传 5
    static std::vector<VkVertexInputAttributeDescription> attributeDescriptions(
        int count);

private:
    // 位置 → 描述。位置 0..9 全表（按 location 升序，Vulkan 要求）。
    static std::vector<VkVertexInputAttributeDescription> attributesFor(
        std::initializer_list<int> locations);
    static VkVertexInputAttributeDescription attributeAt(int location);
};

// ============================================================
// morph delta（§14）—— 与 `assetio::SourceMorphDelta` 逐字节一致
//
//   position(12) target(4) normal(12) vertex(4)  = 32
//
// `target` = 该子网格内第几个 morph target（**子网格内局部**，与 joints
// 是"骨架内下标"同一个套路）；`vertex` = 这条记录属于哪个顶点。
//
// ⚠ `vertex` 在 GPU 上是**填充位**（着色器不读），在文件里存的是真下标。
//   它存在的唯一目的是让加载路径能校验"流的顺序 == 顶点顺序" —— 少了
//   这道校验，一次"顶点重排了但流没跟着重排"会让整张脸按错误的顶点
//   形变，而几何、法线、UV 全都正常。
// ============================================================
struct MorphDelta {
    glm::vec3 position{0.0f};
    std::uint32_t target = 0;
    glm::vec3 normal{0.0f};
    std::uint32_t vertex = 0;
};
static_assert(sizeof(MorphDelta) == 32, "MorphDelta 必须是 32 字节（GPU 侧按此布局）");
static_assert(offsetof(MorphDelta, target) == 12, "MorphDelta.target 偏移");
static_assert(offsetof(MorphDelta, normal) == 16, "MorphDelta.normal 偏移");
static_assert(offsetof(MorphDelta, vertex) == 28, "MorphDelta.vertex 偏移");

// ============================================================
// MeshSource —— 网格的"来源描述"
//
// 场景序列化需要知道一个 Mesh 是**怎么创建**出来的，才能重建它：
// 程序化形状要记尺寸，文件模型要记路径（glTF 还要记 primitive 下标）。
// 这里把它挂在 Mesh 上，属于"资产溯源"信息 —— 真实引擎做热重载
// 也依赖同一套东西。
// ============================================================
struct MeshSource {
    enum class Kind { Unknown, Builtin, OBJ, GLTF, FBX };

    Kind kind = Kind::Unknown;
    std::string shape;         // Builtin: "cube" / "plane" / "sphere"
    float size = 1.0f;         // Builtin 的尺寸/半径参数
    // Builtin 的细分参数。sphere 用 segments+rings，cylinder 只用 segments，
    // cube/plane 两个都是 0（忽略）。0 = "用生成器的默认值"。
    //
    // 为什么要存：编辑器允许回头改图元的生成参数（见
    // ecs::MeshComponent::primitive），改完 mesh 的缓存键里就带上了这组
    // 参数。存档只记来源描述，重建时必须能把同样一组参数喂回生成器，
    // 否则读回来的模型和存下去的不是一个东西。
    int segments = 0;
    int rings = 0;
    std::string path;          // OBJ / GLTF 的文件路径
    // 缓存键：
    //   Builtin / OBJ → 网格自身的 AssetManager 键
    //   GLTF          → **模型**的 AssetManager 键（重建 = 重新 loadGLTF）
    std::string name;
    int subMeshIndex = -1;     // GLTF：primitive 下标
};

class Mesh {
public:
    Mesh() = default;
    ~Mesh();

    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;

    // 从 CPU 数据上传 GPU
    void upload(rhi::Device& device, rhi::CommandPool& cmdPool,
                std::vector<Vertex> vertices, std::vector<uint32_t> indices);

    // ---- morph（§14）----
    //
    // delta 流：与顶点里的 `morphBase` / `morphCount` 严格对应
    // （`vertices[v].morphBase` 是该顶点在流里的起点，`morphCount` 是条数）。
    // 布局与磁盘上的 `assetio::SourceMorphDelta` 逐字节一致，将来接 GPU
    // 时可以整块 memcpy 进 SSBO —— 这一刀只把它落到 CPU 侧 + 校验。
    //
    // 单独一个 setter 而不是塞进 `upload()` 的参数：upload 的调用点有
    // 十几处（程序化图元、LOD、地形…），多一个参数就是十几处 `{}`。
    // morph 是**导入资产专有**的东西，用独立的入口更诚实。
    void setMorph(std::vector<MorphDelta> deltas, std::vector<std::string> names,
                  std::vector<MorphChannel> channels = {});
    const std::vector<MorphDelta>& morphDeltas() const { return m_morphDeltas; }
    const std::vector<std::string>& morphNames() const { return m_morphNames; }
    // 滑条（§15）。与 morphNames 是**多对一**：一个滑条可以挂多个 target。
    const std::vector<MorphChannel>& morphChannels() const {
        return m_morphChannels;
    }
    // target 数 = 这里 `morphBase` 能指向的范围。GPU 侧的权重数组按它开。
    std::uint32_t morphTargetCount() const {
        return static_cast<std::uint32_t>(m_morphNames.size());
    }
    bool hasMorph() const { return !m_morphDeltas.empty(); }

    // 录制绘制命令
    void draw(VkCommandBuffer cmd) const;

    // 只绑顶点/索引缓冲，不发绘制命令。给**间接绘制**用：
    // vkCmdDrawIndexedIndirect 自己不带 VB/IB，调用方必须先绑好 ——
    // 否则会拿上一次绑定的残留缓冲去解释 firstIndex/vertexOffset，
    // 画出"形状对但顶点数据错"的几何（这条路径最难发现的一类 bug）。
    void bindGeometry(VkCommandBuffer cmd) const;

    // 实例化绘制：instanceCount 个实例共享本次 VB/IB 绑定，VS 里用
    // gl_InstanceIndex（含 firstInstance 偏移）索引每实例 SSBO 槽。
    // draw() = drawInstanced(cmd, 1, 0) 的便捷写法。
    void drawInstanced(VkCommandBuffer cmd, uint32_t instanceCount,
                       uint32_t firstInstance) const;

    const std::vector<Vertex>& vertices() const { return m_vertices; }
    const std::vector<uint32_t>& indices() const { return m_indices; }

    const MeshSource& source() const { return m_source; }
    void setSource(const MeshSource& s) { m_source = s; }

    // W2-2：`.vmeta` 的 `import.generateLods=false` → 这条网格不建 LOD 链。
    // ⚠ 挂在 Mesh 上而不是 Model 上：LOD 链是按**网格**建的
    //   （`AssetManager::ensureLod(Mesh*)`），按 Model 挂会漏掉共享网格。
    bool noLod() const { return m_noLod; }
    void setNoLod(bool v) { m_noLod = v; }

    bool valid() const { return m_vertexBuffer != nullptr; }

    // 索引数（= 三角形数 × 3）。给诊断/回归用：`FOLIAGE-TYPE-MESH` 那行
    // 日志靠它证明"精细化之后三角形真的涨了"，而不是只看截图感觉。
    std::uint32_t indexCount() const { return m_indexCount; }
    std::uint32_t triangleCount() const { return m_indexCount / 3; }

    // ---- 局部空间 AABB（上传时从顶点算一次，之后只读）----
    // 用途：渲染层的阴影正交范围、编辑器的拾取/贴地都需要"物体的实际
    // 尺寸"。没有它就只能退化成拿实体原点当包围盒 —— 单个物体算出来
    // 半径是 0，阴影贴图只覆盖贴着原点的一小块，屏幕上会看到一块边界
    // 笔直的方形阴影区。
    bool hasBounds() const { return m_hasBounds; }
    const glm::vec3& boundsMin() const { return m_boundsMin; }
    const glm::vec3& boundsMax() const { return m_boundsMax; }

    // ---- LOD 链 ----
    // 由 AssetManager 惰性生成后挂上（见 AssetManager::ensureLod）。**不是**
    // 网格自身的几何数据，所以只是一根裸指针；链对象的所有权在
    // AssetManager::m_lods 里，生命周期覆盖所有 Mesh。
    //
    // 渲染层拿它选层级；没有链（或链只有 1 级）就是"不做 LOD"。
    const LodChain* lodChain() const { return m_lodChain; }
    void setLodChain(const LodChain* c) { m_lodChain = c; }

    // 在一个轴向对齐面上生成 4 个顶点（u×v = n 保证从外侧看 CCW）
    // offsetAlongNormal：立方体的面需沿法线偏移 half；单个面（如地面）传 false
    // 公开给几何生成器使用
    static void addFace(std::vector<Vertex>& verts, std::vector<uint32_t>& idx,
                        glm::vec3 n, glm::vec3 u, glm::vec3 v, float half,
                        bool offsetAlongNormal = true);

private:

    std::vector<Vertex> m_vertices;
    std::vector<uint32_t> m_indices;
    MeshSource m_source;
    std::vector<MorphDelta> m_morphDeltas;
    std::vector<std::string> m_morphNames;
    std::vector<MorphChannel> m_morphChannels;
    bool m_noLod = false;   // W2-2：禁用 LOD 链

    // 局部 AABB（upload 时算一次）。空网格 → m_hasBounds = false
    glm::vec3 m_boundsMin{0.0f};
    glm::vec3 m_boundsMax{0.0f};
    bool m_hasBounds = false;

    // LOD 链（不拥有；所有权在 AssetManager）
    const LodChain* m_lodChain = nullptr;

    rhi::Buffer* m_vertexBuffer = nullptr;
    rhi::Buffer* m_indexBuffer = nullptr;
    uint32_t m_indexCount = 0;
};

// ---- 几何生成（CPU 数据，调用方负责 upload）----
std::vector<Vertex> makeCubeVertices(float size);
std::vector<uint32_t> makeCubeIndices();
std::vector<Vertex> makePlaneVertices(float size);
std::vector<uint32_t> makePlaneIndices();

// UV 球：PBR 的标准测试形状
// （平面反射方向恒定，只有曲面才能体现金属的镜面变化与环境菲涅尔）
std::vector<Vertex> makeSphereVertices(float radius, int segments = 48,
                                       int rings = 24);
std::vector<uint32_t> makeSphereIndices(int segments = 48, int rings = 24);

// 圆柱：轴向 = 世界 +Z（Z-up），高 = 2×半径（和 Blender 默认圆柱同比例）。
// 侧面 48 边 + 上下两片三角扇端盖，全部外法线、CCW 绕序。
std::vector<Vertex> makeCylinderVertices(float radius, int segments = 48);
std::vector<uint32_t> makeCylinderIndices(int segments = 48);

// ------------------------------------------------------------
// 植被（W5-1 的第一步，2026-10-20；2026-10-28 精细化重写）
//
// 为什么植被要有自己的内置形状：撒点如果只能用 cube/sphere，"植被"看起来
// 就是一堆绿方块。草叶最少要**交叉的卡片**，树最少要有**干 + 冠**两段。
// 两者都是程序化的，不需要美术资产，也不需要外部文件 —— 与 cube / sphere
// 同级。
//
// 尺寸约定：`size` = 世界高度（米），XY 宽度由形状自己按比例算。
// 法线一律朝上（0,0,1）：草和树冠都是薄片，用面法线会在逆光时变黑，
// 朝上法线拿的是"天光"，观感稳定。材质**必须** doubleSided（背面要能看见）。
//
// ------------------------------------------------------------
// 2026-10-28：用户报"生成的 grass/tree/flower/bush/rock 都太粗糙"
// ------------------------------------------------------------
// 第一版全都压到了极致（草 3 张平面卡 = 2 个三角形、树 2 个八边锥、
// 灌木一颗 8×5 球、石头 9×6），理由是"撒几百株也无所谓"。问题在于
// **几百株同一种粗糙轮廓叠在一起，粗糙本身会被放大**：草是一撮绿毛、
// 树是一排圣诞树、石头是一地面包里滚出来的蛋。
//
// 所以这一版把每种的"可辨认特征"补上，代价是三角形涨 5~40 倍（一棵树
// ~1100 tri，仍然低于 UE 的 5k~20k 一个量级）：
//   · 草   = 9 片**带弯曲、带锥度、分 4 段**的叶片（72 tri）
//   · 花   = 2 株（弯曲茎 + 6 瓣花冠 + 花心）（~90 tri）
//   · 灌木 = 4 个位移球 + 8 张叶卡（~500 tri）
//   · 树   = 锥形弯曲树干 + 3 根树枝 + 6 团位移树冠（~1100 tri）
//   · 石头 = 14×9 四倍频位移 + 平面着色（~500 tri，形体更像岩块）
//
// ⚠ API 从"verts / indices 各一个函数"改成**一次出两者**：旧的两半写法
//   要求两份代码里的形状参数逐字相同（树的注释里专门警告过），而
//   grass/flower 的索引表是硬编码"每片 4 个顶点"的 —— 一改叶片数就整块
//   错位（顶点够、索引指到别的叶片上，画出来是一团乱线）。合成一个函数
//   之后这类错位在结构上不可能发生。
// ------------------------------------------------------------
void buildGrassMesh(float size, std::vector<Vertex>& verts,
                    std::vector<uint32_t>& idx);
void buildFlowerMesh(float size, std::vector<Vertex>& verts,
                     std::vector<uint32_t>& idx);
void buildBushMesh(float size, std::vector<Vertex>& verts,
                   std::vector<uint32_t>& idx);
void buildTreeMesh(float size, std::vector<Vertex>& verts,
                   std::vector<uint32_t>& idx);
// 石头：低面数、顶点随机位移、**逐面法线**（平面着色）的球。
// 为什么要单独一份而不是压扁的 sphere：光滑球压扁之后仍然处处是圆滑的
// 高光，一堆摆在一起像"塑料蛋"；平面着色 + 位移才有岩块的棱。
void buildRockMesh(float size, int seed, std::vector<Vertex>& verts,
                   std::vector<uint32_t>& idx);


} // namespace assets
