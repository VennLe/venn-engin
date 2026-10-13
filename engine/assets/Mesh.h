#pragma once
// ============================================================
// assets/Mesh —— 网格（CPU 数据 + GPU 顶点/索引缓冲）
// 顶点布局：pos(3f) + normal(3f) + uv(2f) = 32 字节
// 内置：立方体 / 平面生成器（CCW 正面、外法线）
// ============================================================

#include "rhi/VulkanCommon.h"
#include <cstddef>
#include <vector>

namespace rhi {
class Device;
class CommandPool;
class Buffer;
}

namespace assets {

class LodChain;   // assets/Lod.h

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
    // 32 → 64 字节。默认成员初始化保证所有旧代码（几何生成器逐字段
    // 赋值 pos/normal/uv）不需要改。

    static VkVertexInputBindingDescription bindingDescription() {
        VkVertexInputBindingDescription b{};
        b.binding = 0;
        b.stride = sizeof(Vertex);
        b.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return b;
    }

    static std::vector<VkVertexInputAttributeDescription> attributeDescriptions() {
        std::vector<VkVertexInputAttributeDescription> attrs(5);
        attrs[0].binding = 0;
        attrs[0].location = 0;
        attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
        attrs[0].offset = offsetof(Vertex, pos);
        attrs[1].binding = 0;
        attrs[1].location = 1;
        attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
        attrs[1].offset = offsetof(Vertex, normal);
        attrs[2].binding = 0;
        attrs[2].location = 2;
        attrs[2].format = VK_FORMAT_R32G32_SFLOAT;
        attrs[2].offset = offsetof(Vertex, uv);
        // 蒙皮：location 3 = 关节下标（uint），location 4 = 权重
        attrs[3].binding = 0;
        attrs[3].location = 3;
        attrs[3].format = VK_FORMAT_R32G32B32A32_UINT;
        attrs[3].offset = offsetof(Vertex, joints);
        attrs[4].binding = 0;
        attrs[4].location = 4;
        attrs[4].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        attrs[4].offset = offsetof(Vertex, weights);
        return attrs;
    }
};

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

    bool valid() const { return m_vertexBuffer != nullptr; }

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

} // namespace assets
