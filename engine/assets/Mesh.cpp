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
    drawInstanced(cmd, 1, 0);
}

void Mesh::bindGeometry(VkCommandBuffer cmd) const {
    if (!m_vertexBuffer || !m_indexBuffer) return;

    VkBuffer vbs[] = {m_vertexBuffer->get()};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, vbs, offsets);
    vkCmdBindIndexBuffer(cmd, m_indexBuffer->get(), 0, VK_INDEX_TYPE_UINT32);
}

void Mesh::drawInstanced(VkCommandBuffer cmd, uint32_t instanceCount,
                         uint32_t firstInstance) const {
    if (!m_vertexBuffer || !m_indexBuffer) return;

    bindGeometry(cmd);
    vkCmdDrawIndexed(cmd, m_indexCount, instanceCount, 0, 0, firstInstance);
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

// ============================================================
// 植被：草 / 树
// ============================================================

std::vector<Vertex> makeGrassVertices(float size) {
    std::vector<Vertex> verts;
    if (!(size > 0.0f)) size = 1.0f;

    // 三张面片、彼此 60°。片数为什么不是 2：两张正交片在 45° 方向会
    // "看到侧面"（几乎没有宽度），画面上会周期性出现一条空档；三张
    // 刚好把最窄的方向从 0 提到 sin(30°)=0.5。
    constexpr int kBlades = 3;
    // 每张片子的高度略有差异（0.86 / 1.0 / 0.92），否则顶端齐平像一排栅栏。
    const float heightMul[kBlades] = {0.86f, 1.0f, 0.92f};
    const float width = size * 0.42f;   // 单张片子底部宽度
    const float topTaper = 0.30f;       // 顶部收窄比例（草尖）

    for (int b = 0; b < kBlades; ++b) {
        const float a = static_cast<float>(b) * 1.0471975512f;  // 60°
        const glm::vec3 dir(std::cos(a), std::sin(a), 0.0f);
        const glm::vec3 u = dir * (width * 0.5f);
        const float h = size * heightMul[b];
        const glm::vec3 up(0.0f, 0.0f, h);

        const glm::vec3 bl = -u;
        const glm::vec3 br = u;
        const glm::vec3 tr = u * topTaper + up;
        const glm::vec3 tl = -u * topTaper + up;
        // 朝上的法线：拿天光而不是面法线（薄片逆光时面法线会全黑）
        const glm::vec3 n(0.0f, 0.0f, 1.0f);

        verts.push_back(Vertex{bl, n, glm::vec2(0.0f, 0.0f)});
        verts.push_back(Vertex{br, n, glm::vec2(1.0f, 0.0f)});
        verts.push_back(Vertex{tr, n, glm::vec2(1.0f, 1.0f)});
        verts.push_back(Vertex{tl, n, glm::vec2(0.0f, 1.0f)});
    }
    return verts;
}

std::vector<uint32_t> makeGrassIndices() {
    std::vector<uint32_t> idx;
    for (uint32_t b = 0; b < 3; ++b) {
        const uint32_t base = b * 4;
        idx.insert(idx.end(), {base + 0, base + 1, base + 2,
                               base + 0, base + 2, base + 3});
    }
    return idx;
}

// ------------------------------------------------------------
// 花：细杆 + 顶上一小簇花冠
// ------------------------------------------------------------
// 为什么值得单独一份网格（而不是把草缩小染色）：草是"一丛贴地的片"，
// 花是"一根杆挑一个头"。同一个缩放下去，草的轮廓在 3 m 外就是一撮绿毛，
// 花则完全没有可辨认的形状 —— 撒出来之后整片地还是"只有草"。
//
// 结构：2 张杆片（交叉 90°，很窄）+ 3 张花冠片（在 70% 高处交叉）。
// 顶点数 5×4=20，三角形 10 个 —— 撒几百株也无所谓。
std::vector<Vertex> makeFlowerVertices(float size) {
    std::vector<Vertex> verts;
    if (!(size > 0.0f)) size = 1.0f;

    const glm::vec3 up(0.0f, 0.0f, 1.0f);   // 薄片统一朝天，逆光不黑

    // ---- 杆：两张交叉窄片，从地面到 74% 高 ----
    {
        const float stemH = size * 0.74f;
        const float stemW = size * 0.055f;
        for (int b = 0; b < 2; ++b) {
            const float a = static_cast<float>(b) * 1.5707963f;
            const glm::vec3 dir(std::cos(a), std::sin(a), 0.0f);
            const glm::vec3 u = dir * (stemW * 0.5f);
            verts.push_back(Vertex{-u, up, glm::vec2(0.0f, 0.0f)});
            verts.push_back(Vertex{u, up, glm::vec2(1.0f, 0.0f)});
            verts.push_back(Vertex{u + glm::vec3(0.0f, 0.0f, stemH), up,
                                   glm::vec2(1.0f, 1.0f)});
            verts.push_back(Vertex{-u + glm::vec3(0.0f, 0.0f, stemH), up,
                                   glm::vec2(0.0f, 1.0f)});
        }
    }

    // ---- 花冠：三张交叉片，从 62% 高长到顶端 ----
    // 底部比杆宽（size*0.10 vs 0.055），一眼能看出"头"。
    {
        const float headBase = size * 0.62f;
        const float headTop = size;
        const float headW = size * 0.20f;
        for (int b = 0; b < 3; ++b) {
            const float a = static_cast<float>(b) * 1.0471975512f;  // 60°
            const glm::vec3 dir(std::cos(a), std::sin(a), 0.0f);
            const glm::vec3 u = dir * (headW * 0.5f);
            verts.push_back(Vertex{glm::vec3(0.0f, 0.0f, headBase), up,
                                   glm::vec2(0.0f, 0.0f)});
            verts.push_back(Vertex{glm::vec3(0.0f, 0.0f, headBase), up,
                                   glm::vec2(1.0f, 0.0f)});
            verts.push_back(Vertex{u + glm::vec3(0.0f, 0.0f, headTop), up,
                                   glm::vec2(1.0f, 1.0f)});
            verts.push_back(Vertex{-u + glm::vec3(0.0f, 0.0f, headTop), up,
                                   glm::vec2(0.0f, 1.0f)});
            // 注意：上面两张底顶点重合（退化三角形的一半），是为了让花冠
            // 收在杆顶、展开到顶端 —— 用 4 个点画一个"倒梯形"。
        }
    }
    return verts;
}

std::vector<uint32_t> makeFlowerIndices() {
    std::vector<uint32_t> idx;
    for (uint32_t q = 0; q < 5; ++q) {
        const uint32_t base = q * 4;
        idx.insert(idx.end(), {base + 0, base + 1, base + 2,
                               base + 0, base + 2, base + 3});
    }
    return idx;
}

// ------------------------------------------------------------
// 石头：低面数球 + 顶点位移 + 逐面法线
// ------------------------------------------------------------
// 逐面法线意味着**不能共用顶点**（共用顶点就只能有一个法线），所以这里
// 输出的是三角形汤（每个三角形 3 个独立顶点）。324 个三角形 ≈ 972 个
// 顶点，撒几百块石头也没压力。
//
// 位移用三组不同频率的正弦组合（确定性，不需要种子表）：这样每次新建
// 拿到的石头形状一样，截图可回归；换个 seed 就换一种碎法。
std::vector<Vertex> makeRockVertices(float size, int seed) {
    std::vector<Vertex> verts;
    if (!(size > 0.0f)) size = 1.0f;
    const float radius = size * 0.5f;
    const float s = static_cast<float>(seed) * 0.37f;

    constexpr int kSeg = 9;    // 经度段
    constexpr int kRing = 6;   // 纬度段

    auto pointAt = [&](int seg, int ring) -> glm::vec3 {
        const float theta = 6.2831853072f * static_cast<float>(seg) /
                            static_cast<float>(kSeg);
        const float phi = 3.1415926536f * static_cast<float>(ring) /
                          static_cast<float>(kRing);
        const float sp = std::sin(phi);
        glm::vec3 n(sp * std::cos(theta), sp * std::sin(theta), std::cos(phi));
        // 位移：径向 ±30%，三组频率避免出现球面谐波那种规则的鼓包
        float d = 1.0f;
        d += 0.20f * std::sin(theta * 3.0f + s) * std::cos(phi * 2.0f + s * 0.7f);
        d += 0.12f * std::sin(theta * 7.0f + 1.3f + s * 1.7f) *
                    std::sin(phi * 5.0f + s);
        d += 0.07f * std::sin(theta * 13.0f + phi * 9.0f + s * 2.3f);
        // 竖向压扁：真石头不会是个球
        glm::vec3 p = n * radius * d;
        p.z *= 0.72f;
        return p;
    };

    auto addTri = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
        glm::vec3 n = glm::cross(b - a, c - a);
        if (glm::length(n) < 1.0e-9f) return;
        n = glm::normalize(n);
        verts.push_back(Vertex{a, n, glm::vec2(0.0f, 0.0f)});
        verts.push_back(Vertex{b, n, glm::vec2(1.0f, 0.0f)});
        verts.push_back(Vertex{c, n, glm::vec2(1.0f, 1.0f)});
    };

    for (int ring = 0; ring < kRing; ++ring) {
        for (int seg = 0; seg < kSeg; ++seg) {
            const glm::vec3 p00 = pointAt(seg, ring);
            const glm::vec3 p10 = pointAt(seg + 1, ring);
            const glm::vec3 p01 = pointAt(seg, ring + 1);
            const glm::vec3 p11 = pointAt(seg + 1, ring + 1);
            addTri(p00, p10, p11);
            addTri(p00, p11, p01);
        }
    }
    return verts;
}

namespace {

// 一段圆柱（无端盖）——树干用。轴向 +Z，底面在 base.z。
void addPrism(std::vector<Vertex>& verts, std::vector<uint32_t>& idx,
              const glm::vec3& base, float radius, float height, int sides) {
    const uint32_t start = static_cast<uint32_t>(verts.size());
    for (int s = 0; s <= sides; ++s) {
        const float a = 6.2831853072f * static_cast<float>(s) /
                        static_cast<float>(sides);
        const glm::vec3 n(std::cos(a), std::sin(a), 0.0f);
        const float x = base.x + n.x * radius;
        const float y = base.y + n.y * radius;
        const float u = static_cast<float>(s) / static_cast<float>(sides);
        verts.push_back(Vertex{{x, y, base.z}, n, {u, 0.0f}});
        verts.push_back(Vertex{{x, y, base.z + height}, n, {u, 1.0f}});
    }
    for (int s = 0; s < sides; ++s) {
        const uint32_t b = start + static_cast<uint32_t>(s) * 2;
        idx.insert(idx.end(), {b + 0, b + 1, b + 3,
                               b + 0, b + 3, b + 2});
    }
}

// 一个圆锥（无底盖）——树冠用。apex 在 base.z + height。
void addCone(std::vector<Vertex>& verts, std::vector<uint32_t>& idx,
             const glm::vec3& base, float radius, float height, int sides) {
    const uint32_t apex = static_cast<uint32_t>(verts.size());
    verts.push_back(Vertex{{base.x, base.y, base.z + height},
                           {0.0f, 0.0f, 1.0f}, {0.5f, 1.0f}});
    const float slope = radius / std::max(height, 1.0e-4f);
    for (int s = 0; s <= sides; ++s) {
        const float a = 6.2831853072f * static_cast<float>(s) /
                        static_cast<float>(sides);
        const glm::vec3 dir(std::cos(a), std::sin(a), 0.0f);
        // 侧面法线 = 径向 + 向上（坡度越大越朝上）
        const glm::vec3 n = glm::normalize(dir + glm::vec3(0.0f, 0.0f, slope));
        const float u = static_cast<float>(s) / static_cast<float>(sides);
        verts.push_back(Vertex{{base.x + dir.x * radius,
                                base.y + dir.y * radius, base.z},
                               n, {u, 0.0f}});
    }
    for (int s = 0; s < sides; ++s) {
        const uint32_t b = apex + 1u + static_cast<uint32_t>(s);
        idx.insert(idx.end(), {apex, b, b + 1});
    }
}

} // namespace

std::vector<Vertex> makeTreeVertices(float size) {
    std::vector<Vertex> verts;
    std::vector<uint32_t> idx;
    if (!(size > 0.0f)) size = 1.0f;

    const glm::vec3 base(0.0f, 0.0f, 0.0f);
    // 树干：占整体高度的 45%，半径 7%（1 m 高的"树"是苗，8 m 高才像树 ——
    // 尺寸由撒点时的缩放决定，这里只定义比例）
    addPrism(verts, idx, base, size * 0.07f, size * 0.45f, 6);
    // 树冠：两层锥体，下一层更宽 —— 交叠出"蓬松"的轮廓，也遮住树干顶端
    addCone(verts, idx, glm::vec3(0.0f, 0.0f, size * 0.30f), size * 0.34f,
            size * 0.42f, 8);
    addCone(verts, idx, glm::vec3(0.0f, 0.0f, size * 0.52f), size * 0.26f,
            size * 0.46f, 8);
    return verts;
}

std::vector<uint32_t> makeTreeIndices() {
    // 索引与 `size` 无关（只连顶点），所以这里用 1.0 重跑一遍几何构造、
    // 只为拿到索引表。两处必须**用同一个形状参数**（干 45%、两层冠
    // 0.30/0.42 与 0.52/0.46），否则顶点数与索引对不上。
    std::vector<Vertex> scratch;
    std::vector<uint32_t> idx;
    const float size = 1.0f;
    addPrism(scratch, idx, glm::vec3(0.0f), size * 0.07f, size * 0.45f, 6);
    addCone(scratch, idx, glm::vec3(0.0f, 0.0f, size * 0.30f), size * 0.34f,
            size * 0.42f, 8);
    addCone(scratch, idx, glm::vec3(0.0f, 0.0f, size * 0.52f), size * 0.26f,
            size * 0.46f, 8);
    return idx;
}

} // namespace assets
