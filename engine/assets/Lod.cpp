#include "assets/Lod.h"

#include "core/Logger.h"

#include <algorithm>
#include <array>
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
    // 顶点色**可以**求平均（它是"这一格被染成什么颜色"，不是索引）。
    // 不累这一项的话，Mesh Paint 画过的模型一拉远就回到白色 —— 而且
    // 因为 LOD 是按距离切换的，看起来像"颜色在某段距离突然消失"。
    glm::vec4 col{0.0f};
    // 第二套 UV（§13）同样可以求平均：它和 uv 是一样的语义。
    glm::vec2 uv2{0.0f};
    uint32_t count = 0;
    uint32_t outIndex = 0;

    // ---- 蒙皮（W2-3）----
    // joint 下标**不能求平均**（3 和 5 平均成 4 是另一根骨头），所以要按
    // "每个 joint 各累积多少权重"来合。一格里的顶点通常只受同一小组骨头
    // 影响，8 个槽足够；满了就把权重最小的那个挤掉。
    std::array<uint32_t, 8> jIdx{{0, 0, 0, 0, 0, 0, 0, 0}};
    std::array<float, 8> jW{{0, 0, 0, 0, 0, 0, 0, 0}};
    uint32_t jCount = 0;
};

// 把一个顶点的 4 个 (joint, weight) 累进格子里
inline void accumulateJoint(CellAccum& c, uint32_t j, float w) {
    if (w <= 0.0f) return;
    for (uint32_t k = 0; k < c.jCount; ++k) {
        if (c.jIdx[k] == j) { c.jW[k] += w; return; }
    }
    if (c.jCount < c.jIdx.size()) {
        c.jIdx[c.jCount] = j;
        c.jW[c.jCount] = w;
        ++c.jCount;
        return;
    }
    // 满了：替换权重最小的那个（保证大权重不会因为顺序被丢掉）
    uint32_t worst = 0;
    for (uint32_t k = 1; k < c.jCount; ++k)
        if (c.jW[k] < c.jW[worst]) worst = k;
    if (w > c.jW[worst]) { c.jIdx[worst] = j; c.jW[worst] = w; }
}

// 顶点的"主影响 joint" = weights 里最大分量的那个 joint 下标。
// 分组键要用它：不同骨头的顶点合到一起，减面后肢体就会连在一起。
inline uint32_t primaryJoint(const Vertex& v) {
    uint32_t best = 0;
    float bw = v.weights.x;
    if (v.weights.y > bw) { bw = v.weights.y; best = 1; }
    if (v.weights.z > bw) { bw = v.weights.z; best = 2; }
    if (v.weights.w > bw) { bw = v.weights.w; best = 3; }
    return v.joints[best];
}

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
    // 分两趟：先判"这网格是不是蒙皮的"（有任何一个顶点权重和 > 0）。
    // 非蒙皮走原来的"格子"键；蒙皮的在格子之外**再按主 joint 分组** ——
    // 这正是 W2-3 的核心：不同骨头的顶点一旦合到一起，减面后的肢体就会
    // 被"焊"在一块，一摆姿势就撕成碎片。
    bool skinned = false;
    for (const Vertex& v : vertices) {
        if (v.weights.x + v.weights.y + v.weights.z + v.weights.w > 0.0f) {
            skinned = true;
            break;
        }
    }

    // 外层键 = 空间格；内层键 = 主 joint（非蒙皮恒为 0，只多一层空壳）。
    // 用嵌套而不是把 joint 混进 64 位格键：格号已经吃掉 48 位，再塞 joint
    // 会撞键 —— 撞一次的后果是两根骨头的顶点被合到一起，且**没有任何报错**。
    std::unordered_map<uint64_t, std::unordered_map<uint32_t, uint32_t>> map;
    map.reserve(vertices.size() / 2 + 1);
    std::vector<CellAccum> cells;
    cells.reserve(vertices.size() / 2 + 1);
    std::vector<uint32_t> vtxCell(vertices.size(), 0);

    for (size_t i = 0; i < vertices.size(); ++i) {
        const Vertex& vin = vertices[i];
        const glm::vec3& p = vin.pos;
        const uint64_t key = cellKey(cellIndex(p.x, lo.x, invCell),
                                     cellIndex(p.y, lo.y, invCell),
                                     cellIndex(p.z, lo.z, invCell));
        const uint32_t prim = skinned ? primaryJoint(vin) : 0u;
        auto& inner = map[key];
        auto it = inner.find(prim);
        uint32_t idx;
        if (it == inner.end()) {
            idx = static_cast<uint32_t>(cells.size());
            inner.emplace(prim, idx);
            CellAccum c;
            c.outIndex = idx;
            cells.push_back(c);
        } else {
            idx = it->second;
        }
        vtxCell[i] = idx;

        CellAccum& c = cells[idx];
        c.pos += p;
        c.nrm += vin.normal;
        c.uv += vin.uv;
        c.uv2 += vin.uv2;
        c.col += vin.color;
        ++c.count;
        // 蒙皮权重：只有真正带权重的顶点才累（weights 全 0 的静态顶点
        // 混进来会把归一化分母搞乱）
        if (skinned) {
            const float wsum = vin.weights.x + vin.weights.y + vin.weights.z +
                               vin.weights.w;
            if (wsum > 0.0f) {
                accumulateJoint(c, vin.joints.x, vin.weights.x);
                accumulateJoint(c, vin.joints.y, vin.weights.y);
                accumulateJoint(c, vin.joints.z, vin.weights.z);
                accumulateJoint(c, vin.joints.w, vin.weights.w);
            }
        }
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
        v.uv2 = c.uv2 * inv;
        v.color = c.col * inv;
        // ---- 2026-10-25（§13）：切线**故意不带到 LOD 上** ----
        //
        // `Vertex v;` 的默认构造已经把 tangent 留成 0（= 没有切线）、把
        // morphBase 留成 kNoMorphBase（= 不参与 morph）。这是刻意接受的
        // 两条局限，不是漏写：
        //   · 切线：把两个手性相反的切线平均出来是一个**错**的切线，
        //     而错的切线比没有切线更难查（没有切线至少还能退回屏幕空间
        //     导数建 TBN）。真要做，得按"每组手性分别累加"来合。
        //   · morph：LOD 顶点是**重新聚类**出来的，和原顶点的 morph
        //     delta 没有对应关系。要让 LOD 支持形变，得在减面时同步
        //     重算 delta 流 —— 那是 morph 那一刀（§13.5）的事。
        // 两者都记进 §13.6「已知局限」。

        // ---- 蒙皮（W2-3）：把这一格累积到的 joint 权重收成 4 个槽 ----
        // 取权重最大的 4 个，然后**归一化到 1** —— 不归一化的话顶点会被
        // "缩水"（权重和 < 1 时剩下一部分变换被丢掉，顶点被吸向原点）。
        if (c.jCount > 0) {
            std::array<uint32_t, 8> order{{0, 1, 2, 3, 4, 5, 6, 7}};
            std::sort(order.begin(), order.begin() + c.jCount,
                      [&c](uint32_t a, uint32_t b) { return c.jW[a] > c.jW[b]; });
            float sum = 0.0f;
            const uint32_t take = std::min<uint32_t>(c.jCount, 4);
            for (uint32_t k = 0; k < take; ++k) sum += c.jW[order[k]];
            if (sum > 1e-8f) {
                glm::uvec4 jj{0, 0, 0, 0};
                glm::vec4 ww{0.0f, 0.0f, 0.0f, 0.0f};
                for (uint32_t k = 0; k < take; ++k) {
                    jj[k] = c.jIdx[order[k]];
                    ww[k] = c.jW[order[k]] / sum;
                }
                v.joints = jj;
                v.weights = ww;
            }
        }
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
