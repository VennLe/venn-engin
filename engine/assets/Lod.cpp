#include "assets/Lod.h"

#include "core/Logger.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace assets {

// ============================================================
// LodChain
// ============================================================

void LodChain::add(Mesh* mesh, float switchRatio) {
    // 阈值必须严格递减，否则选层会永远卡在某一级上（区间退化成空集）。
    // 写错了就直接夹住并报一声 —— 静默地歪掉比报错难查得多。
    if (!m_levels.empty() && switchRatio >= m_levels.back().switchRatio) {
        VK_LOG_WARN("LodChain::add: 阈值 %.4f 没有比上一级 %.4f 更小，夹到一半",
                    static_cast<double>(switchRatio),
                    static_cast<double>(m_levels.back().switchRatio));
        switchRatio = m_levels.back().switchRatio * 0.5f;
    }

    LodLevel lv;
    lv.mesh = mesh;
    lv.switchRatio = switchRatio;
    if (mesh) {
        lv.vertices = static_cast<uint32_t>(mesh->vertices().size());
        lv.triangles = static_cast<uint32_t>(mesh->indices().size() / 3);
    }
    m_levels.push_back(lv);
}

void LodChain::truncate(size_t count) {
    if (m_levels.size() > count) m_levels.resize(count);
}

uint64_t LodChain::totalTriangles() const {
    uint64_t n = 0;
    for (const LodLevel& l : m_levels) n += l.triangles;
    return n;
}

size_t LodChain::selectLevel(float ratio, float bias, int current) const {
    if (m_levels.empty()) return 0;
    const size_t last = m_levels.size() - 1;
    if (last == 0) return 0;

    // bias **除**在比值上：bias > 1 → 比值变小 → 更快越过阈值落到低模，
    // 也就是"宁可掉画质也要省"（和 Unreal / Unity 的 LOD Bias 同向）。
    // 反过来 bias < 1 就是"宁愿多花点三角形也要保画质"。
    // 注意方向别写反：进入第 i 级的条件是"比值 **<** 阈值"，所以让比值
    // 变小才是往低模走。
    if (!(bias > 0.0f)) bias = 1.0f;
    const float r = ratio / bias;

    // 没有上一帧的选择（物体第一次出现）：直接按阈值落位，不做迟滞。
    // 从高模开始往下走，只要还低于下一级的阈值就继续退 —— 最终停在第
    // 一个"退不过去"的级上。写成"返回第一个满足 r < 阈值的 i"是错的：
    // 相邻两级的阈值都满足时（比如 r=0.08 同时小于 0.25 和 0.10），
    // 那样会停在更**细**的一级，等于把每个物体都高估一档。
    if (current < 0) {
        size_t lv = 0;
        while (lv < last && r < m_levels[lv + 1].switchRatio) ++lv;
        return lv;
    }

    const size_t cur = static_cast<size_t>(
        std::min<int>(current, static_cast<int>(last)));

    // 1) 能不能往更粗的一级走？要"明显小于"那一级的进入阈值。
    for (size_t i = cur + 1; i <= last; ++i) {
        if (r <= m_levels[i].switchRatio / kLodHysteresis) return i;
    }

    // 2) 能不能往更细的一级走？要"明显大于"当前级的进入阈值
    //    （即比值已经涨出本级区间的上界）。
    size_t up = cur;
    while (up > 0 && r > m_levels[up].switchRatio * kLodHysteresis) --up;
    return up;
}

// ============================================================
// 顶点聚类简化
// ============================================================

namespace {

// 把一个浮点坐标映射到格号。返回 int32 是因为格号可能为负（顶点在
// 包围盒原点以下时），但这里已经用 min 平移过了，实际非负；留着 int32
// 只是为了后面拼键时有确定的宽度。
inline int32_t cellIndex(float v, float lo, float invCell) {
    const int32_t c = static_cast<int32_t>((v - lo) * invCell);
    return c < 0 ? 0 : c;
}

// 格号 (x,y,z) 拍成一个 64 位键。gridRes 上限取 1<<16，三轴各 16 位
// 够用且不会撞键。
inline uint64_t cellKey(int32_t x, int32_t y, int32_t z) {
    return (static_cast<uint64_t>(x) << 42) |
           (static_cast<uint64_t>(y) << 21) |
           static_cast<uint64_t>(z);
}

struct CellAccum {
    glm::vec3 pos{0.0f};
    glm::vec3 nrm{0.0f};
    glm::vec2 uv{0.0f};
    uint32_t count = 0;
    uint32_t outIndex = 0;
};

} // namespace

LodMeshData decimateClustered(const std::vector<Vertex>& vertices,
                              const std::vector<uint32_t>& indices,
                              int gridRes) {
    LodMeshData out;
    if (vertices.empty() || indices.size() < 3) return out;
    if (gridRes < 2) gridRes = 2;
    if (gridRes > (1 << 16)) gridRes = 1 << 16;

    // ---- 1) 包围盒 ----
    glm::vec3 lo(vertices[0].pos), hi(vertices[0].pos);
    for (const Vertex& v : vertices) {
        lo = glm::min(lo, v.pos);
        hi = glm::max(hi, v.pos);
    }
    glm::vec3 ext = hi - lo;
    // 退化成一个点/一条线时没有可简化的东西
    const float maxExt = std::max(ext.x, std::max(ext.y, ext.z));
    if (!(maxExt > 0.0f)) return out;

    // 三个轴共用同一个格边长：用最长轴定格边长，否则扁平的物体在短轴上
    // 会只落进一格、瞬间被压成一层皮。
    const float cell = maxExt / static_cast<float>(gridRes);
    const float invCell = 1.0f / cell;
    const float minArea2 = maxExt * maxExt * 1e-8f;

    // ---- 2) 顶点落格 + 合并 ----
    std::unordered_map<uint64_t, uint32_t> map;
    map.reserve(vertices.size() / 2 + 1);
    std::vector<CellAccum> cells;
    cells.reserve(vertices.size() / 2 + 1);
    std::vector<uint32_t> vtxCell(vertices.size(), 0);

    for (size_t i = 0; i < vertices.size(); ++i) {
        const glm::vec3& p = vertices[i].pos;
        const uint64_t key = cellKey(cellIndex(p.x, lo.x, invCell),
                                     cellIndex(p.y, lo.y, invCell),
                                     cellIndex(p.z, lo.z, invCell));
        auto it = map.find(key);
        uint32_t idx;
        if (it == map.end()) {
            idx = static_cast<uint32_t>(cells.size());
            map.emplace(key, idx);
            CellAccum c;
            c.outIndex = idx;
            cells.push_back(c);
        } else {
            idx = it->second;
        }
        vtxCell[i] = idx;

        CellAccum& c = cells[idx];
        c.pos += p;
        c.nrm += vertices[i].normal;
        c.uv += vertices[i].uv;
        ++c.count;
    }

    // ---- 3) 收拢成顶点数组 ----
    out.vertices.resize(cells.size());
    for (size_t i = 0; i < cells.size(); ++i) {
        const CellAccum& c = cells[i];
        const float inv = 1.0f / static_cast<float>(c.count ? c.count : 1);
        Vertex v;
        v.pos = c.pos * inv;
        const glm::vec3 n = c.nrm * inv;
        const float len = glm::length(n);
        // 法线全抵消（对称形状的一格）时退回 +Z，别留一个零法线让着色器
        // 算出 NaN 亮度。
        v.normal = (len > 1e-6f) ? (n / len) : glm::vec3(0.0f, 0.0f, 1.0f);
        v.uv = c.uv * inv;
        // joints/weights 保持全 0：蒙皮网格不生成 LOD
        out.vertices[i] = v;
    }

    // ---- 4) 重建索引：丢掉退化的 ----
    out.indices.reserve(indices.size());
    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        const uint32_t a = vtxCell[indices[t + 0]];
        const uint32_t b = vtxCell[indices[t + 1]];
        const uint32_t c = vtxCell[indices[t + 2]];
        if (a == b || b == c || a == c) continue;   // 塌成一点/一条线
        const glm::vec3& pa = out.vertices[a].pos;
        const glm::vec3& pb = out.vertices[b].pos;
        const glm::vec3& pc = out.vertices[c].pos;
        const glm::vec3 cr = glm::cross(pb - pa, pc - pa);
        if (glm::dot(cr, cr) <= minArea2) continue;  // 面积过小
        out.indices.push_back(a);
        out.indices.push_back(b);
        out.indices.push_back(c);
    }

    out.ok = !out.vertices.empty() && out.indices.size() >= 3;
    return out;
}

LodMeshData decimateClustered(const Mesh& mesh, int gridRes) {
    return decimateClustered(mesh.vertices(), mesh.indices(), gridRes);
}

} // namespace assets
