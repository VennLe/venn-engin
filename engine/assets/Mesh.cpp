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
    // 水平面朝上：n=(0,1,0)，u=(0,0,1)，v=(1,0,0)，u×v=n
    // 注意 offsetAlongNormal=false：平面居中于原点（y=0），不沿法线偏移
    Mesh::addFace(verts, idx, {0, 1, 0}, {0, 0, 1}, {1, 0, 0}, h, false);
    return verts;
}

std::vector<uint32_t> makePlaneIndices() {
    return {0, 1, 2, 0, 2, 3};
}

// ------------------------------------------------------------
// UV 球：经纬网格参数化
//   y: 0..rings    → phi   = v * PI      （北极 → 南极）
//   x: 0..segments → theta = u * 2PI     （绕 Y 轴一周）
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

            glm::vec3 n{sinPhi * std::cos(theta), cosPhi,
                        sinPhi * std::sin(theta)};

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

} // namespace assets
