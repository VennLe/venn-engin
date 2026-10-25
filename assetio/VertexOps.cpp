#include "assetio/VertexOps.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <queue>
#include <unordered_map>

#include <glm/glm.hpp>

namespace assetio {

namespace {

inline std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    // splitmix64 的一步：把强相关的整数（量化后的坐标）打散。
    // 直接用 `h * 1099511628211 ^ v` 这类 FNV 式混合，在"坐标是
    // 0.1 的整数倍"这种高度规律的数据上会退化成很多碰撞。
    v += 0x9e3779b97f4a7c15ull;
    v = (v ^ (v >> 30)) * 0xbf58476d1ce4e5b9ull;
    v = (v ^ (v >> 27)) * 0x94d049bb133111ebull;
    v = v ^ (v >> 31);
    return h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2));
}

inline std::uint64_t quantize(float v, float invEps) {
    const double q = static_cast<double>(v) * static_cast<double>(invEps);
    // clamp 到 int64 范围：坏数据里的 NaN/Inf 会被 lround 变成 UB
    if (!(q > -9.0e15 && q < 9.0e15)) return 0xFFFFFFFFull;
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(std::llround(q)));
}

std::uint64_t vertexKey(const Vertex& v, float posEps) {
    // 位置：按 eps 量化（吃浮点噪声）
    // 法线 / UV：固定较严的量化 —— 见 VertexOps.h 里那段说明
    const float invPos = 1.0f / (posEps > 0.0f ? posEps : 1.0e-6f);
    const float invNrm = 1.0f / 1.0e-3f;    // ≈0.06°，足够区分硬边
    const float invUv = 1.0f / 1.0e-6f;

    std::uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < 3; ++i) h = mix(h, quantize(v.pos[i], invPos));
    for (int i = 0; i < 3; ++i) h = mix(h, quantize(v.normal[i], invNrm));
    for (int i = 0; i < 2; ++i) h = mix(h, quantize(v.uv[i], invUv));
    // 关节下标与权重：权重按 1e-5 量化（通常只有 2~4 位有效小数）
    for (int i = 0; i < 4; ++i) h = mix(h, static_cast<std::uint64_t>(v.joints[i]));
    for (int i = 0; i < 4; ++i) h = mix(h, quantize(v.weights[i], 1.0e5f));
    for (int i = 0; i < 4; ++i) h = mix(h, quantize(v.color[i], 1.0e5f));
    // ---- 2026-10-25（§13）：新通道也必须进焊接键 ----
    //
    // 不把 uv2 / tangent 放进来，后果不是"多余顶点被多焊了几份"，而是
    // **反过来的**：两个 uv2 不同的顶点被判成同一个，UV2 只剩其中一份。
    // 面部 SDF 贴图在脸上会出现一块用错了坐标的区域 —— 那是最难定位
    // 的一类 bug（几何完全正确，只有阴影纹理歪）。焊接键宁可多留顶点。
    for (int i = 0; i < 2; ++i) h = mix(h, quantize(v.uv2[i], invUv));
    for (int i = 0; i < 4; ++i) h = mix(h, quantize(v.tangent[i], 1.0e5f));
    // morphBase 是**导入后**才定的下标，同一个顶点在两次烤之间应当一致；
    // 放进键里是为了让"带 morph 的顶点"不会被焊到"不带 morph 的顶点"上
    // —— 后者会静默丢掉形变。
    h = mix(h, static_cast<std::uint64_t>(v.morphBase));
    // morphCount 也进键：morphBase 在焊接阶段存的是归属哈希，两个顶点
    // 哈希相同但参与 target 数不同是不可能的（哈希已经编码了全部记录），
    // 这里加一道纯属双保险 —— 焊接错一次的代价是"某块几何静默失去形变"，
    // 而多一个 u32 的哈希代价是零。
    h = mix(h, static_cast<std::uint64_t>(v.morphCount));
    return h;
}

bool vertexEqual(const Vertex& a, const Vertex& b, float posEps) {
    // 哈希碰撞极少，但不能靠"碰撞了就认为是同一个"——那样焊错一次
    // 就是整块几何错位。这里做一次真实的逐字段比较。
    const float pe = posEps > 0.0f ? posEps : 1.0e-6f;
    for (int i = 0; i < 3; ++i)
        if (std::fabs(a.pos[i] - b.pos[i]) > pe) return false;
    for (int i = 0; i < 3; ++i)
        if (std::fabs(a.normal[i] - b.normal[i]) > 1.0e-3f) return false;
    for (int i = 0; i < 2; ++i)
        if (std::fabs(a.uv[i] - b.uv[i]) > 1.0e-6f) return false;
    for (int i = 0; i < 4; ++i)
        if (a.joints[i] != b.joints[i]) return false;
    for (int i = 0; i < 4; ++i)
        if (std::fabs(a.weights[i] - b.weights[i]) > 1.0e-5f) return false;
    for (int i = 0; i < 4; ++i)
        if (std::fabs(a.color[i] - b.color[i]) > 1.0e-5f) return false;
    // 与 vertexKey 一一对应：键里有的字段这里也必须比，否则哈希说"同"
    // 而实际"不同"时会直接把差异抹掉（碰撞本来就极少，但一旦发生就是
    // 一次静默的数据丢失）。
    for (int i = 0; i < 2; ++i)
        if (std::fabs(a.uv2[i] - b.uv2[i]) > 1.0e-6f) return false;
    for (int i = 0; i < 4; ++i)
        if (std::fabs(a.tangent[i] - b.tangent[i]) > 1.0e-5f) return false;
    if (a.morphBase != b.morphBase) return false;
    if (a.morphCount != b.morphCount) return false;
    return true;
}

// Forsyth 的打分常数（取自原文 "Linear-Speed Vertex Cache Optimization"）
constexpr float kCacheDecayPower = 1.5f;
constexpr float kLastTriScore = 0.75f;
constexpr float kValenceBoostScale = 2.0f;
constexpr float kValenceBoostPower = 0.5f;

} // namespace

// ------------------------------------------------------------ ACMR

float measureAcmr(const std::vector<std::uint32_t>& indices,
                  std::uint32_t vertexCount, int cacheSize) {
    const std::size_t triCount = indices.size() / 3;
    if (triCount == 0) return 0.0f;

    // 用 "上次使用到现在的三角形数" 近似 FIFO 位置。
    // 比真的维护一个 32 元列表更简单，且对 ACMR 的估计是等价的：
    // 一个顶点只要在最近 cacheSize 个三角形里出现过，就还在缓存里
    // （前提是缓存是完美 FIFO —— 真实硬件近似如此）。
    std::vector<std::int64_t> lastUse(vertexCount, -1000000);
    std::int64_t shaded = 0;
    for (std::size_t t = 0; t < triCount; ++t) {
        for (int c = 0; c < 3; ++c) {
            const std::uint32_t v = indices[t * 3 + c];
            if (v >= vertexCount) continue;
            if (t - lastUse[v] >= cacheSize) ++shaded;
            lastUse[v] = static_cast<std::int64_t>(t);
        }
    }
    return static_cast<float>(shaded) / static_cast<float>(triCount);
}

// ------------------------------------------------------------ 焊接

void weldVertices(std::vector<Vertex>& vertices,
                  std::vector<std::uint32_t>& indices, float positionEpsilon,
                  std::vector<std::uint32_t>* remapOut) {
    if (vertices.empty() || indices.empty()) {
        if (remapOut) remapOut->clear();
        return;
    }

    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> buckets;
    buckets.reserve(vertices.size() / 2 + 8);

    std::vector<Vertex> out;
    out.reserve(vertices.size());
    std::vector<std::uint32_t> remap(vertices.size(), 0);

    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const std::uint64_t key = vertexKey(vertices[i], positionEpsilon);
        auto& bucket = buckets[key];
        std::uint32_t found = 0xFFFFFFFFu;
        for (std::uint32_t cand : bucket) {
            if (vertexEqual(out[cand], vertices[i], positionEpsilon)) {
                found = cand;
                break;
            }
        }
        if (found == 0xFFFFFFFFu) {
            found = static_cast<std::uint32_t>(out.size());
            out.push_back(vertices[i]);
            bucket.push_back(found);
        }
        remap[i] = found;
    }

    // remap 必须在**早退之前**交出去：早退时 remap 是恒等映射，
    // 但调用方（morph 重映射）照样需要它。返回一个空 vector 会让
    // "没焊"和"没算"变成两种状态 —— 而调用方无从分辨，只能猜。
    if (remapOut) *remapOut = remap;

    if (out.size() == vertices.size()) return;   // 没得焊（已经是最简）

    for (std::uint32_t& idx : indices)
        if (idx < remap.size()) idx = remap[idx];

    vertices = std::move(out);
}

// ------------------------------------------------------------ 法线

void recomputeNormals(std::vector<Vertex>& vertices,
                      const std::vector<std::uint32_t>& indices) {
    if (vertices.empty() || indices.size() < 3) return;

    std::vector<glm::vec3> accum(vertices.size(), glm::vec3(0.0f));
    for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
        const std::uint32_t i0 = indices[t], i1 = indices[t + 1], i2 = indices[t + 2];
        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size())
            continue;
        const glm::vec3 p0(vertices[i0].pos[0], vertices[i0].pos[1], vertices[i0].pos[2]);
        const glm::vec3 p1(vertices[i1].pos[0], vertices[i1].pos[1], vertices[i1].pos[2]);
        const glm::vec3 p2(vertices[i2].pos[0], vertices[i2].pos[1], vertices[i2].pos[2]);
        // **不归一化**的叉积，其长度正比于三角形面积 —— 面积加权就在这一步
        const glm::vec3 n = glm::cross(p1 - p0, p2 - p0);
        accum[i0] += n;
        accum[i1] += n;
        accum[i2] += n;
    }

    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const glm::vec3 n = accum[i];
        const float len2 = glm::dot(n, n);
        if (len2 > 1.0e-20f) {
            const glm::vec3 u = n / std::sqrt(len2);
            vertices[i].normal[0] = u.x;
            vertices[i].normal[1] = u.y;
            vertices[i].normal[2] = u.z;
        } else {
            // 退化三角形（面积为 0）构不成方向。给 (0,0,1) 而不是 (0,0,0)：
            // 全 0 法线在着色器里 normalize 之后是 NaN，会污染整个像素。
            vertices[i].normal[0] = 0.0f;
            vertices[i].normal[1] = 0.0f;
            vertices[i].normal[2] = 1.0f;
        }
    }
}

// ------------------------------------------------------------ 缓存重排

void optimizeForCache(std::vector<std::uint32_t>& indices, std::uint32_t vertexCount) {
    const std::size_t triCount = indices.size() / 3;
    if (triCount < 2 || vertexCount == 0) return;

    // activeTris[v]：还有多少"尚未输出"的三角形引用 v。
    // 它是打分里的 valence 项 —— 引用得越多，越应该晚点把它挤出缓存。
    std::vector<int> activeTris(vertexCount, 0);
    for (std::uint32_t v : indices)
        if (v < vertexCount) ++activeTris[v];

    // 模拟的 32 元 FIFO。第 0 位 = 最近使用。
    std::vector<std::uint32_t> fifo;
    fifo.reserve(kPostTransformCacheSize);
    std::vector<int> cacheTag(vertexCount, -1);

    auto touch = [&](std::uint32_t v) {
        const auto it = std::find(fifo.begin(), fifo.end(), v);
        if (it != fifo.end()) fifo.erase(it);
        fifo.insert(fifo.begin(), v);
        if (static_cast<int>(fifo.size()) > kPostTransformCacheSize) {
            cacheTag[fifo.back()] = -1;
            fifo.pop_back();
        }
        for (int i = 0; i < static_cast<int>(fifo.size()); ++i)
            cacheTag[fifo[static_cast<std::size_t>(i)]] = i;
    };

    auto vertexScore = [&](std::uint32_t v) -> float {
        if (activeTris[v] == 0) return -1.0f;
        const int pos = cacheTag[v];
        if (pos < 0) return 0.0f;
        if (pos < 3) return kLastTriScore;
        if (pos >= kPostTransformCacheSize) return 0.0f;
        float s = 1.0f - static_cast<float>(pos - 3) /
                             static_cast<float>(kPostTransformCacheSize - 3);
        s = std::pow(s, kCacheDecayPower);
        s += kValenceBoostScale *
             std::pow(static_cast<float>(activeTris[v]), -kValenceBoostPower);
        return s;
    };

    // 三角形分 = 三个顶点分之和。负分（顶点已无剩余三角形）按 0 计：
    // 那种顶点**不需要再着色**，所以它对这条三角形的代价贡献本来就是 0，
    // 把负分加进去只会让"已经免费"的三角形反而被排到最后。
    // ⚠ 未输出的三角形，它的三个顶点 activeTris 必然 ≥ 1（每条被引用的
    //   三角形都被计过数，只有输出时才减），所以负分只会在**越界下标**
    //   （坏数据）时出现 —— 那时按 0 处理正好让它自然排到后面去。
    auto triScore = [&](std::size_t t) -> float {
        float s = 0.0f;
        for (int c = 0; c < 3; ++c) {
            const std::uint32_t v = indices[t * 3 + c];
            if (v >= vertexCount) continue;
            const float vs = vertexScore(v);
            if (vs > 0.0f) s += vs;
        }
        return s;
    };

    struct Item {
        float score;
        std::uint32_t tri;
        bool operator<(const Item& o) const { return score < o.score; }
    };

    std::priority_queue<Item> heap;
    for (std::size_t t = 0; t < triCount; ++t) heap.push({triScore(t), static_cast<std::uint32_t>(t)});

    std::vector<std::uint8_t> emitted(triCount, 0);
    std::vector<std::uint32_t> out;
    out.reserve(indices.size());

    // 惰性堆：分数只会因为"别的三角形被输出"而变化（cacheTag 上升、
    // activeTris 下降），而且**只会变小**（除了刚被 touch 的顶点会变大）。
    // 所以弹出时重算一次，如果比当前堆顶还小就放回去等下一轮 —— 
    // 这是标准做法，比维护"反向索引 + 局部更新"简单得多。
    while (out.size() < indices.size() && !heap.empty()) {
        const Item top = heap.top();
        heap.pop();
        if (emitted[top.tri]) continue;

        const float now = triScore(top.tri);
        if (!heap.empty() && now < heap.top().score - 1.0e-6f) {
            // 分数变差了 → 放回去，等更高分的先出
            heap.push({now, top.tri});
            continue;
        }

        const std::uint32_t t = top.tri;
        emitted[t] = 1;
        for (int c = 0; c < 3; ++c) {
            const std::uint32_t v = indices[t * 3 + c];
            out.push_back(v);
            if (v < vertexCount && activeTris[v] > 0) --activeTris[v];
            if (v < vertexCount) touch(v);
        }
    }

    // 兜底：堆逻辑万一漏掉（理论上不会），把没输出的按原序补上，
    // 保证"三角形一个不多一个不少"这条硬性质 —— 少一条就是模型破洞。
    for (std::size_t t = 0; t < triCount; ++t) {
        if (!emitted[t]) {
            for (int c = 0; c < 3; ++c) out.push_back(indices[t * 3 + c]);
        }
    }
    if (out.size() == indices.size()) indices.swap(out);
}

// ------------------------------------------------------------ 组合

bool indicesFitU16(const std::vector<std::uint32_t>& indices) {
    for (std::uint32_t i : indices)
        if (i > 0xFFFFu) return false;
    return true;
}

// ------------------------------------------------------------ morph 的三步

// 第 1 步（焊接**前**）：把"这个顶点参与哪些 target、偏移是多少"编码进
// `morphBase` / `morphCount`，让焊接把它们当作顶点身份的一部分。
//
// 为什么要编码而不是直接留哨兵：`weldVertices` 只看 `Vertex` 的字段。
// 如果 morph 归属不在字段里，一个"在 target_3 里往前鼓 2mm"的顶点会和
// 一个"不在任何 target 里"的完全相同的顶点焊在一起 —— 形变静默丢失，
// 而且几何完全正确，只有做表情的时候才看得出。
//
// 编码用 64 位 FNV 的低 32 位。碰撞概率 2^-32，再乘上"morphCount 也必须
// 相等"这一道，实际风险可以忽略。焊接之后这里存的值会被
// `buildMorphStream` 覆盖成真正的流下标 —— 所以它是**临时值**，
// 不要在任何别处读它。
void prepareMorphIdentity(SourceSubMesh& sm) {
    const std::size_t V = sm.vertices.size();
    if (sm.morphTargets.empty()) {
        for (Vertex& v : sm.vertices) {
            v.morphBase = kNoMorphBase;
            v.morphCount = 0;
        }
        return;
    }

    std::vector<std::uint64_t> h(V, 0xcbf29ce484222325ull);
    std::vector<std::uint32_t> cnt(V, 0);
    for (std::size_t t = 0; t < sm.morphTargets.size(); ++t) {
        const SourceMorphTarget& mt = sm.morphTargets[t];
        for (std::size_t k = 0; k < mt.vertices.size(); ++k) {
            const std::uint32_t v = mt.vertices[k];
            if (v >= V) continue;
            h[v] = mix(h[v], static_cast<std::uint64_t>(t));
            if (k < mt.positionDeltas.size()) {
                const glm::vec3& p = mt.positionDeltas[k];
                h[v] = mix(h[v], quantize(p.x, 1.0e5f));
                h[v] = mix(h[v], quantize(p.y, 1.0e5f));
                h[v] = mix(h[v], quantize(p.z, 1.0e5f));
            }
            if (k < mt.normalDeltas.size()) {
                const glm::vec3& n = mt.normalDeltas[k];
                h[v] = mix(h[v], quantize(n.x, 1.0e5f));
                h[v] = mix(h[v], quantize(n.y, 1.0e5f));
                h[v] = mix(h[v], quantize(n.z, 1.0e5f));
            }
            ++cnt[v];
        }
    }
    for (std::size_t v = 0; v < V; ++v) {
        sm.vertices[v].morphBase = cnt[v]
                                       ? static_cast<std::uint32_t>(h[v] ^ (h[v] >> 32))
                                       : kNoMorphBase;
        sm.vertices[v].morphCount = cnt[v];
    }
}

// 第 2 步（焊接**后**）：按 remap 把 delta 表的顶点下标改到新空间。
//
// 两个旧顶点焊成一个新顶点时，它们的归属**必定相同**（焊接键里含归属
// 哈希），所以取第一条、丢掉重复记录即可。结果按新下标升序，保证
// "同一份 FBX 烤两次逐字节相同"。
void remapMorphTargets(SourceSubMesh& sm, const std::vector<std::uint32_t>& remap) {
    if (sm.morphTargets.empty() || remap.empty()) return;
    const std::size_t V = sm.vertices.size();
    for (SourceMorphTarget& mt : sm.morphTargets) {
        std::vector<std::uint32_t> idx;
        idx.reserve(mt.vertices.size());
        for (std::uint32_t oldV : mt.vertices) {
            const std::uint32_t nv =
                (oldV < remap.size()) ? remap[oldV] : kNoMorphBase;
            if (nv < V) idx.push_back(nv);
        }
        std::sort(idx.begin(), idx.end());
        idx.erase(std::unique(idx.begin(), idx.end()), idx.end());

        // 重新取值：新下标 nv 对应哪个旧下标。用"排序后的位置"反查会
        // 丢掉"这条记录原来带的是什么" —— 所以先建映射再取。
        std::unordered_map<std::uint32_t, std::size_t> firstOld;
        for (std::size_t k = 0; k < mt.vertices.size(); ++k) {
            const std::uint32_t oldV = mt.vertices[k];
            const std::uint32_t nv =
                (oldV < remap.size()) ? remap[oldV] : kNoMorphBase;
            if (nv >= V) continue;
            firstOld.emplace(nv, k);   // 只保留第一条（emplace 不覆盖）
        }

        std::vector<std::uint32_t> newV;
        std::vector<glm::vec3> newP, newN;
        newV.reserve(idx.size());
        newP.reserve(idx.size());
        newN.reserve(idx.size());
        for (std::uint32_t nv : idx) {
            auto it = firstOld.find(nv);
            if (it == firstOld.end()) continue;
            const std::size_t k = it->second;
            newV.push_back(nv);
            newP.push_back(k < mt.positionDeltas.size() ? mt.positionDeltas[k]
                                                        : glm::vec3(0.0f));
            newN.push_back(k < mt.normalDeltas.size() ? mt.normalDeltas[k]
                                                      : glm::vec3(0.0f));
        }
        mt.vertices = std::move(newV);
        mt.positionDeltas = std::move(newP);
        mt.normalDeltas = std::move(newN);
    }
}

// 第 3 步（所有会改下标的操作都做完之后）：把"每 target 的稀疏表"翻成
// "扁平流 + 每个顶点的 (base, count)"。
//
// 顺序规则：按顶点升序；同一顶点内按 target 升序。这两条决定了字节级的
// 输出，任何一条变了"烤两次相同"就没了 —— 所以显式排序，不依赖
// `unordered_map` 的遍历顺序。
void buildMorphStream(SourceSubMesh& sm) {
    const std::size_t V = sm.vertices.size();
    sm.morphStream.clear();

    if (sm.morphTargets.empty()) {
        for (Vertex& v : sm.vertices) {
            v.morphBase = kNoMorphBase;
            v.morphCount = 0;
        }
        return;
    }

    struct Slot {
        std::uint32_t target;
        glm::vec3 pos;
        glm::vec3 nrm;
    };
    std::vector<std::vector<Slot>> per(V);
    for (std::size_t t = 0; t < sm.morphTargets.size(); ++t) {
        const SourceMorphTarget& mt = sm.morphTargets[t];
        for (std::size_t k = 0; k < mt.vertices.size(); ++k) {
            const std::uint32_t v = mt.vertices[k];
            if (v >= V) continue;
            Slot s;
            s.target = static_cast<std::uint32_t>(t);
            s.pos = k < mt.positionDeltas.size() ? mt.positionDeltas[k] : glm::vec3(0.0f);
            s.nrm = k < mt.normalDeltas.size() ? mt.normalDeltas[k] : glm::vec3(0.0f);
            per[v].push_back(s);
        }
    }

    std::uint32_t cursor = 0;
    for (std::size_t v = 0; v < V; ++v) {
        std::vector<Slot>& lst = per[v];
        if (lst.empty()) {
            sm.vertices[v].morphBase = kNoMorphBase;
            sm.vertices[v].morphCount = 0;
            continue;
        }
        std::sort(lst.begin(), lst.end(), [](const Slot& a, const Slot& b) {
            return a.target < b.target;
        });
        sm.vertices[v].morphBase = cursor;
        sm.vertices[v].morphCount = static_cast<std::uint32_t>(lst.size());
        for (const Slot& s : lst) {
            SourceMorphDelta d;
            d.position = s.pos;
            d.target = s.target;
            d.normal = s.nrm;
            d.vertex = static_cast<std::uint32_t>(v);
            sm.morphStream.push_back(d);
            ++cursor;
        }
    }
}

OptimizeStats optimizeSubMesh(SourceSubMesh& sm, bool doWeld, bool doCache) {
    OptimizeStats st;
    st.verticesIn = static_cast<std::uint32_t>(sm.vertices.size());
    st.indicesIn = static_cast<std::uint32_t>(sm.indices.size());
    st.acmrIn = measureAcmr(sm.indices, st.verticesIn);

    // 顺序是**有强制理由**的，不能重排：
    //   ① 归属编码必须在焊接前 —— 否则焊接看不到 morph，会把
    //      "带形变的顶点"和"不带形变的相同顶点"合并；
    //   ② delta 重映射必须紧跟焊接 —— remap 只在那一刻有效；
    //   ③ 流化必须最后 —— 它要在最终顶点下标空间上写 morphBase。
    prepareMorphIdentity(sm);

    if (doWeld) {
        const std::size_t before = sm.vertices.size();
        std::vector<std::uint32_t> remap;
        weldVertices(sm.vertices, sm.indices, 1.0e-5f, &remap);
        st.welded = sm.vertices.size() < before;
        remapMorphTargets(sm, remap);
    }

    if (doCache) optimizeForCache(sm.indices, static_cast<std::uint32_t>(sm.vertices.size()));

    buildMorphStream(sm);

    st.verticesOut = static_cast<std::uint32_t>(sm.vertices.size());
    st.indicesOut = static_cast<std::uint32_t>(sm.indices.size());
    st.acmrOut = measureAcmr(sm.indices, st.verticesOut);
    st.index16 = indicesFitU16(sm.indices);
    return st;
}

} // namespace assetio
