#include "assets/Mesh.h"
#include "rhi/Device.h"
#include "rhi/CommandPool.h"
#include "rhi/Buffer.h"

#include <cmath>

#include <glm/gtc/constants.hpp>

namespace assets {

// ---------------- 网格上传 / 绘制 ----------------

Mesh::~Mesh() {
    // Buffer 由 rhi 层 RAII 管理；此处延迟到 Device 销毁前由 AssetManager 释放
    delete m_vertexBuffer;
    delete m_indexBuffer;
    m_vertexBuffer = nullptr;
    m_indexBuffer = nullptr;
}

void Mesh::upload(rhi::Device& device, rhi::CommandPool& cmdPool,
                  std::vector<Vertex> vertices, std::vector<uint32_t> indices) {
    if (vertices.empty() || indices.empty()) {
        throw std::runtime_error("Mesh::upload: empty mesh data");
    }
    m_vertices = std::move(vertices);
    m_indices = std::move(indices);
    m_indexCount = static_cast<uint32_t>(m_indices.size());

    // ---- 局部 AABB（算一次，供阴影范围 / 拾取 / 贴地使用）----
    m_boundsMin = m_vertices[0].pos;
    m_boundsMax = m_vertices[0].pos;
    for (const Vertex& v : m_vertices) {
        m_boundsMin = glm::min(m_boundsMin, v.pos);
        m_boundsMax = glm::max(m_boundsMax, v.pos);
    }
    m_hasBounds = true;

    delete m_vertexBuffer;
    delete m_indexBuffer;

    m_vertexBuffer = rhi::Buffer::createDeviceLocal(
        device, cmdPool, m_vertices.data(),
        m_vertices.size() * sizeof(Vertex),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)
        .release();

    m_indexBuffer = rhi::Buffer::createDeviceLocal(
        device, cmdPool, m_indices.data(),
        m_indices.size() * sizeof(uint32_t),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT)
        .release();
}

void Mesh::draw(VkCommandBuffer cmd) const {
    if (!m_vertexBuffer || !m_indexBuffer) return;

    VkBuffer vbs[] = {m_vertexBuffer->get()};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, vbs, offsets);
    vkCmdBindIndexBuffer(cmd, m_indexBuffer->get(), 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, m_indexCount, 1, 0, 0, 0);
}

// ---------------- 几何生成 ----------------

void Mesh::addFace(std::vector<Vertex>& verts, std::vector<uint32_t>& idx,
                   glm::vec3 n, glm::vec3 u, glm::vec3 v, float half,
                   bool offsetAlongNormal) {
    uint32_t base = static_cast<uint32_t>(verts.size());
    // 4 角顺序 (a,b) = (-,-) (+,-) (+,+) (-,+)，且 u×v = n ⇒ 从外侧看为 CCW
    glm::vec2 uvs[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    float signs[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    // 立方体的每个面需要沿法线偏移半尺寸；平面等单个面不需要偏移
    glm::vec3 center = offsetAlongNormal ? n * half : glm::vec3(0.0f);
    for (int i = 0; i < 4; ++i) {
        Vertex vtx;
        vtx.pos = center + u * (signs[i][0] * half) + v * (signs[i][1] * half);
        vtx.normal = n;
        vtx.uv = uvs[i];
        verts.push_back(vtx);
    }
    idx.push_back(base + 0);
    idx.push_back(base + 1);
    idx.push_back(base + 2);
    idx.push_back(base + 0);
    idx.push_back(base + 2);
    idx.push_back(base + 3);
}

// Y-up 临时坐标 → 引擎世界坐标（Z-up 右手系）的固定变换：
// 绕 X 轴 +90°，(x, y, z) → (x, -z, y)。旋转保持手性，所以法线方向、
// 三角形绕序（CCW）都不用改 —— 直接对 pos / normal 做同样的映射即可。
// 基元（cube/sphere/plane）先生成再过一遍这个函数，生成代码保持可读。
static inline glm::vec3 toZup(const glm::vec3& p) {
    return {p.x, -p.z, p.y};
}

std::vector<Vertex> makeCubeVertices(float size) {
    std::vector<Vertex> verts;
    std::vector<uint32_t> idx;
    float h = size * 0.5f;
    // 6 个面：n 为外法线，u×v = n
    Mesh::addFace(verts, idx, {0, 0, 1}, {1, 0, 0}, {0, 1, 0}, h);    // +Z
    Mesh::addFace(verts, idx, {0, 0, -1}, {-1, 0, 0}, {0, 1, 0}, h);  // -Z
    Mesh::addFace(verts, idx, {1, 0, 0}, {0, 0, -1}, {0, 1, 0}, h);   // +X
    Mesh::addFace(verts, idx, {-1, 0, 0}, {0, 0, 1}, {0, 1, 0}, h);   // -X
    Mesh::addFace(verts, idx, {0, 1, 0}, {1, 0, 0}, {0, 0, -1}, h);   // +Y
    Mesh::addFace(verts, idx, {0, -1, 0}, {1, 0, 0}, {0, 0, 1}, h);   // -Y
    for (auto& v : verts) {
        v.pos = toZup(v.pos);
        v.normal = toZup(v.normal);
    }
    return verts;
}

std::vector<uint32_t> makeCubeIndices() {
    std::vector<uint32_t> idx;
    for (uint32_t f = 0; f < 6; ++f) {
        uint32_t base = f * 4;
        idx.insert(idx.end(), {base + 0, base + 1, base + 2,
                               base + 0, base + 2, base + 3});
    }
    return idx;
}

std::vector<Vertex> makePlaneVertices(float size) {
    std::vector<Vertex> verts;
    std::vector<uint32_t> idx;
    float h = size * 0.5f;
    // 水平面朝上：n=(0,0,1)，u=(1,0,0)，v=(0,1,0)，u×v=n
    // 注意 offsetAlongNormal=false：平面居中于原点（z=0），不沿法线偏移
    Mesh::addFace(verts, idx, {0, 0, 1}, {1, 0, 0}, {0, 1, 0}, h, false);
    return verts;
}

std::vector<uint32_t> makePlaneIndices() {
    return {0, 1, 2, 0, 2, 3};
}

// ------------------------------------------------------------
// UV 球：经纬网格参数化
//   y: 0..rings    → phi   = v * PI      （北极 → 南极）
//   x: 0..segments → theta = u * 2PI     （绕 Z 轴一周）
// 绕序沿用全项目约定：从球外侧看为逆时针（CCW）
// ------------------------------------------------------------
std::vector<Vertex> makeSphereVertices(float radius, int segments, int rings) {
    std::vector<Vertex> verts;
    verts.reserve(static_cast<size_t>(segments + 1) * (rings + 1));

    for (int y = 0; y <= rings; ++y) {
        const float v = static_cast<float>(y) / static_cast<float>(rings);
        const float phi = v * glm::pi<float>();
        const float sinPhi = std::sin(phi);
        const float cosPhi = std::cos(phi);

        for (int x = 0; x <= segments; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(segments);
            const float theta = u * glm::two_pi<float>();

            // 极轴 = 世界 +Z（Z-up）。⚠ 必须用与 toZup 相同的旋转
            // (x,y,z)→(x,-z,y) —— 直接交换 y/z 分量是反射（det=-1），
            // 会把绕序翻成 CW，球就内外翻了。
            glm::vec3 n{sinPhi * std::cos(theta), -sinPhi * std::sin(theta),
                        cosPhi};

            Vertex vert;
            vert.pos = n * radius;
            vert.normal = n;  // 单位球上：外法线就等于归一化位置
            vert.uv = {u, v};
            verts.push_back(vert);
        }
    }
    return verts;
}

std::vector<uint32_t> makeSphereIndices(int segments, int rings) {
    std::vector<uint32_t> idx;
    idx.reserve(static_cast<size_t>(segments) * rings * 6);
    const uint32_t stride = static_cast<uint32_t>(segments + 1);

    for (int y = 0; y < rings; ++y) {
        for (int x = 0; x < segments; ++x) {
            const uint32_t a =
                static_cast<uint32_t>(y) * stride + static_cast<uint32_t>(x);
            const uint32_t b = a + stride;  // 下一圈同经度
            // 两个三角形组成一个网格四边形
            idx.push_back(a);
            idx.push_back(a + 1);
            idx.push_back(b);
            idx.push_back(a + 1);
            idx.push_back(b + 1);
            idx.push_back(b);
        }
    }
    return idx;
}

// ------------------------------------------------------------
// 圆柱：轴向 = 世界 +Z（Z-up），高 = 2×radius，居中于原点。
//   行 y=0 是顶圈（z=+h），y 递增往下到 y=segments? 不 —— 这里用
//   rings = 2（顶圈 / 底圈），侧面网格四边形沿用与球相同的
//   (a, a+1, b) 索引模式（a+1 = 同圈下一条边，b = 下一圈同角度）。
//   端盖用三角扇：注意角度参数化与球一致（cosθ, -sinθ），θ 递增
//   从 +Z 上方看是顺时针，所以顶盖要倒序扇出才是 CCW。
// ------------------------------------------------------------
std::vector<Vertex> makeCylinderVertices(float radius, int segments) {
    std::vector<Vertex> verts;
    verts.reserve(static_cast<size_t>(segments + 1) * 2 + 2);

    const float h = radius;  // 高 = 2×radius → 半高 = radius

    // 侧面顶点：两圈（顶 / 底），法线水平朝外
    for (int ring = 0; ring < 2; ++ring) {
        const float z = (ring == 0) ? h : -h;
        for (int x = 0; x <= segments; ++x) {
            const float theta = (static_cast<float>(x) /
                                 static_cast<float>(segments)) *
                                glm::two_pi<float>();
            glm::vec3 n{std::cos(theta), -std::sin(theta), 0.0f};
            Vertex v;
            v.pos = n * radius + glm::vec3(0.0f, 0.0f, z);
            v.normal = n;
            v.uv = {static_cast<float>(x) / static_cast<float>(segments),
                    ring == 0 ? 0.0f : 1.0f};
            verts.push_back(v);
        }
    }

    // 顶盖中心 + 底盖中心（法线沿 ±Z）
    Vertex topCenter;
    topCenter.pos = {0.0f, 0.0f, h};
    topCenter.normal = {0.0f, 0.0f, 1.0f};
    topCenter.uv = {0.5f, 0.5f};
    verts.push_back(topCenter);

    Vertex botCenter;
    botCenter.pos = {0.0f, 0.0f, -h};
    botCenter.normal = {0.0f, 0.0f, -1.0f};
    botCenter.uv = {0.5f, 0.5f};
    verts.push_back(botCenter);

    return verts;
}

std::vector<uint32_t> makeCylinderIndices(int segments) {
    std::vector<uint32_t> idx;
    idx.reserve(static_cast<size_t>(segments) * 6 + segments * 3 * 2);
    const uint32_t stride = static_cast<uint32_t>(segments + 1);
    const uint32_t topCenter = stride * 2;        // 侧面顶点之后
    const uint32_t botCenter = topCenter + 1;

    // 侧面（与球同模式：ring0 = 顶圈，ring1 = 底圈）
    for (int x = 0; x < segments; ++x) {
        const uint32_t a = static_cast<uint32_t>(x);
        const uint32_t b = a + stride;
        idx.push_back(a);
        idx.push_back(a + 1);
        idx.push_back(b);
        idx.push_back(a + 1);
        idx.push_back(b + 1);
        idx.push_back(b);
    }

    // 顶盖：θ 递增从 +Z 上方看是顺时针 → 扇形倒序（center, v+1, v）才是 CCW
    for (int x = 0; x < segments; ++x) {
        idx.push_back(topCenter);
        idx.push_back(static_cast<uint32_t>(x) + 1);
        idx.push_back(static_cast<uint32_t>(x));
    }
    // 底盖：从 -Z 下方看顺序正好相反
    for (int x = 0; x < segments; ++x) {
        const uint32_t base = stride + static_cast<uint32_t>(x);
        idx.push_back(botCenter);
        idx.push_back(base);
        idx.push_back(base + 1);
    }
    return idx;
}

} // namespace assets
