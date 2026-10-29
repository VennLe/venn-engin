// ============================================================
// animation/WeightSet.cpp
//
// 四件事：导入（读四槽）、烘焙（没动过的逐位照抄）、编辑（刷子/归一/平滑/
// 自动权重/镜像）、JSON 往返。**这里没有求值** —— 烘焙出来的东西交给
// `Mesh::upload()` 重新上传，渲染路径一行不改。
//
// 所有编辑操作都经 `WeightSet::xxx`，UI 与注入脚本共用同一份实现 ——
// 这样"能不能自动验"不取决于把某颗滑条拖到正确位置。
// ============================================================

#include "animation/WeightSet.h"

#include "animation/RigEdit.h"
#include "assets/Mesh.h"
#include "core/Logger.h"

#include <nlohmann/json.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace animation {

namespace {

using json = nlohmann::json;

// 权重表的条目排序：权重降序；权重相同按骨下标升序。
//
// ⚠ 必须**全序**。只按权重排序时，两条权重恰好相等的条目在不同 std::sort
//   实现下会换位，于是同一份工程两次烘焙出来的 `JOINTS_0` 不一样 ——
//   画面一致、逐位比对全红，最难查的一类。
bool entryBetter(const WeightEntry& a, const WeightEntry& b) {
    if (a.weight != b.weight) return a.weight > b.weight;
    return a.bone < b.bone;
}

// 一条顶点到一段骨（head→tail）的距离。
// 用线段而不是无限直线：腿的骨和另一条腿的骨离得很近，直线距离会把
// 大腿内侧的顶点吸到对面那条腿上（X 型腿的自动权重经典翻车）。
float pointSegmentDistance(const glm::vec3& p, const glm::vec3& a,
                           const glm::vec3& b) {
    const glm::vec3 ab = b - a;
    const float len2 = glm::dot(ab, ab);
    if (!(len2 > 1.0e-12f)) return glm::length(p - a);
    float t = glm::dot(p - a, ab) / len2;
    t = std::max(0.0f, std::min(1.0f, t));
    return glm::length(p - (a + ab * t));
}

} // namespace

// ============================================================
// 基本访问
// ============================================================

int WeightSet::influenceCount(int v) const {
    if (v < 0 || v >= vertexCount) return 0;
    return static_cast<int>(count[static_cast<std::size_t>(v)]);
}

float WeightSet::weightOf(int v, int bone) const {
    const int n = influenceCount(v);
    const std::size_t base = static_cast<std::size_t>(v) * kMaxInfluence;
    for (int i = 0; i < n; ++i) {
        if (entries[base + static_cast<std::size_t>(i)].bone == bone)
            return entries[base + static_cast<std::size_t>(i)].weight;
    }
    return 0.0f;
}

bool WeightSet::touchedAny() const {
    for (char t : touched)
        if (t) return true;
    return false;
}

void WeightSet::clear() {
    source.clear();
    mesh.clear();
    vertexCount = 0;
    entries.clear();
    count.clear();
    touched.clear();
    origJoints.clear();
    origWeights.clear();
}

// ============================================================
// 导入
// ============================================================

WeightSet WeightSet::fromMesh(const assets::Mesh& mesh, const std::string& sourcePath) {
    return fromVertices(mesh.vertices(), mesh.source().name, sourcePath);
}

WeightSet WeightSet::fromVertices(const std::vector<assets::Vertex>& verts,
                                  const std::string& meshName,
                                  const std::string& sourcePath) {
    WeightSet ws;
    ws.source = sourcePath;
    ws.mesh = meshName;
    ws.vertexCount = static_cast<int>(verts.size());
    if (ws.vertexCount <= 0) return ws;

    ws.entries.assign(static_cast<std::size_t>(ws.vertexCount) * kMaxInfluence,
                      WeightEntry{});
    ws.count.assign(static_cast<std::size_t>(ws.vertexCount), 0);
    ws.touched.assign(static_cast<std::size_t>(ws.vertexCount), 0);
    ws.origJoints.resize(static_cast<std::size_t>(ws.vertexCount));
    ws.origWeights.resize(static_cast<std::size_t>(ws.vertexCount));

    for (int v = 0; v < ws.vertexCount; ++v) {
        const assets::Vertex& sv = verts[static_cast<std::size_t>(v)];
        ws.origJoints[static_cast<std::size_t>(v)] = sv.joints;
        ws.origWeights[static_cast<std::size_t>(v)] = sv.weights;
        const std::size_t base = static_cast<std::size_t>(v) * kMaxInfluence;
        unsigned char n = 0;
        for (int k = 0; k < 4; ++k) {
            const float w = sv.weights[static_cast<glm::length_t>(k)];
            // 零尾巴不留（导入器常写 (1,0,0,0)）
            if (!(w > kWeightEps)) continue;
            ws.entries[base + n].bone = static_cast<int>(sv.joints[static_cast<glm::length_t>(k)]);
            ws.entries[base + n].weight = w;
            ++n;
        }
        ws.count[static_cast<std::size_t>(v)] = n;
    }
    return ws;
}

// ============================================================
// 烘焙
// ============================================================

int WeightSet::bake(assets::Vertex* verts, const std::vector<char>* deformMask) const {
    if (verts == nullptr || vertexCount <= 0) return 0;
    int changed = 0;
    for (int v = 0; v < vertexCount; ++v) {
        const std::size_t sv = static_cast<std::size_t>(v);
        assets::Vertex& dv = verts[sv];
        const bool hasMask = deformMask && !deformMask->empty();
        const int n = influenceCount(v);
        const std::size_t base = sv * kMaxInfluence;

        // mask 只对"**这张顶点的表里真的有被关掉的那根骨**"的顶点生效。
        //
        // ⚠ 一开始这里写的是"有 mask 就整条走出重算分支"，后果是：作者
        //   只要把**任何一根**骨设成不参与变形，其余一万个顶点就全部走
        //   "排序 + 归一化"那一遍，于是约定 ①（零编辑逐位不变）在最常见
        //   的路径上失效 —— 画面看不出来，验收的 `changed=0` 直接变红。
        bool masked = false;
        if (hasMask) {
            for (int i = 0; i < n && !masked; ++i) {
                const int bi = entries[base + static_cast<std::size_t>(i)].bone;
                if (bi < 0) continue;
                const std::size_t k = static_cast<std::size_t>(bi);
                if (k >= deformMask->size() || !(*deformMask)[k]) masked = true;
            }
        }

        // ---- 约定 ①：没动过 + 没有骨被 mask 掉 → 逐位照抄 ----
        if (!touched[sv] && !masked) {
            if (std::memcmp(&dv.joints, &origJoints[sv], sizeof(glm::uvec4)) != 0 ||
                std::memcmp(&dv.weights, &origWeights[sv], sizeof(glm::vec4)) != 0) {
                dv.joints = origJoints[sv];
                dv.weights = origWeights[sv];
                ++changed;
            }
            continue;
        }

        // ---- 收集（mask 掉的骨在这里丢掉）----
        std::vector<WeightEntry> list;
        list.reserve(kMaxInfluence);
        for (int i = 0; i < n; ++i) {
            const WeightEntry& e = entries[base + static_cast<std::size_t>(i)];
            if (!(e.weight > kWeightEps)) continue;
            if (e.bone < 0) continue;
            if (hasMask) {
                const std::size_t bi = static_cast<std::size_t>(e.bone);
                if (bi >= deformMask->size() || !(*deformMask)[bi]) continue;
            }
            list.push_back(e);
        }
        // 全序排序（见 entryBetter 的注释：权重相等时按骨下标定序）
        std::sort(list.begin(), list.end(), entryBetter);
        if (list.size() > 4) list.resize(4);

        // ---- 约定 ②：重归一化 ----
        float sum = 0.0f;
        for (const WeightEntry& e : list) sum += e.weight;
        glm::uvec4 jj(0, 0, 0, 0);
        glm::vec4 ww(0.0f, 0.0f, 0.0f, 0.0f);
        if (sum > kWeightEps) {
            const float inv = 1.0f / sum;
            for (std::size_t i = 0; i < list.size(); ++i) {
                jj[static_cast<glm::length_t>(i)] =
                    static_cast<unsigned>(list[i].bone);
                ww[static_cast<glm::length_t>(i)] = list[i].weight * inv;
            }
        }
        // sum == 0（全被 mask 掉 / 作者清空了）→ 写全 0，让 stats 数出来。
        // 这里**不**临时找一根骨兜底：兜底会把"作者故意清空"和"自动权重的
        // 遗漏"混成同一件事，而后者是要修的 bug。

        if (std::memcmp(&dv.joints, &jj, sizeof(glm::uvec4)) != 0 ||
            std::memcmp(&dv.weights, &ww, sizeof(glm::vec4)) != 0) {
            dv.joints = jj;
            dv.weights = ww;
            ++changed;
        }
    }
    return changed;
}

// ============================================================
// 编辑
// ============================================================

int WeightSet::setWeight(int v, int bone, float w) {
    if (v < 0 || v >= vertexCount || bone < 0) return 0;
    const std::size_t sv = static_cast<std::size_t>(v);
    const std::size_t base = sv * kMaxInfluence;
    int n = static_cast<int>(count[sv]);

    if (w < 0.0f) w = 0.0f;
    if (w > 1.0f) w = 1.0f;

    int slot = -1;
    for (int i = 0; i < n; ++i) {
        if (entries[base + static_cast<std::size_t>(i)].bone == bone) {
            slot = i;
            break;
        }
    }

    if (w <= kWeightEps) {
        if (slot < 0) return 0;
        // 删掉这一条：后面的往前挪（顺序保持插入序，不做隐式排序 ——
        // 排序留给 bake，那里才有"要哪四条"的信息）
        for (int i = slot; i + 1 < n; ++i)
            entries[base + static_cast<std::size_t>(i)] =
                entries[base + static_cast<std::size_t>(i + 1)];
        --n;
        count[sv] = static_cast<unsigned char>(n);
        touched[sv] = 1;
        return 1;
    }

    if (slot >= 0) {
        if (entries[base + static_cast<std::size_t>(slot)].weight == w) return 0;
        entries[base + static_cast<std::size_t>(slot)].weight = w;
        touched[sv] = 1;
        return 1;
    }

    if (n < kMaxInfluence) {
        entries[base + static_cast<std::size_t>(n)].bone = bone;
        entries[base + static_cast<std::size_t>(n)].weight = w;
        count[sv] = static_cast<unsigned char>(n + 1);
        touched[sv] = 1;
        return 1;
    }

    // 表满：替换最小的那条（保留"重要的"）。用 <= 让**并列时**也换 ——
    // 不换的话新手骨永远进不来，作者会看到"刷了半天没反应"。
    int minI = 0;
    for (int i = 1; i < n; ++i)
        if (entries[base + static_cast<std::size_t>(i)].weight <
            entries[base + static_cast<std::size_t>(minI)].weight)
            minI = i;
    if (entries[base + static_cast<std::size_t>(minI)].weight >= w &&
        entries[base + static_cast<std::size_t>(minI)].weight > kWeightEps)
        return 0;
    entries[base + static_cast<std::size_t>(minI)].bone = bone;
    entries[base + static_cast<std::size_t>(minI)].weight = w;
    touched[sv] = 1;
    return 1;
}

int WeightSet::addWeight(int v, int bone, float delta, float lo, float hi) {
    if (v < 0 || v >= vertexCount || bone < 0) return 0;
    float w = weightOf(v, bone) + delta;
    if (w < lo) w = lo;
    if (w > hi) w = hi;
    return setWeight(v, bone, w);
}

bool WeightSet::normalizeVertex(int v) {
    if (v < 0 || v >= vertexCount) return false;
    const std::size_t sv = static_cast<std::size_t>(v);
    const int n = static_cast<int>(count[sv]);
    if (n <= 0) return false;
    const std::size_t base = sv * kMaxInfluence;
    float sum = 0.0f;
    for (int i = 0; i < n; ++i) sum += entries[base + static_cast<std::size_t>(i)].weight;
    // 约定 ②：和已经是 1（或在阈值内）就不动 —— 否则每刷一次就多一次舍入，
    // "同一笔刷两遍 == 刷一遍"这条断言会红。
    if (sum <= kWeightEps) return false;
    if (std::fabs(sum - 1.0f) <= 1.0e-6f) return false;
    const float inv = 1.0f / sum;
    for (int i = 0; i < n; ++i)
        entries[base + static_cast<std::size_t>(i)].weight *= inv;
    touched[sv] = 1;
    return true;
}

int WeightSet::normalizeAll() {
    int c = 0;
    for (int v = 0; v < vertexCount; ++v)
        if (normalizeVertex(v)) ++c;
    return c;
}

int WeightSet::paintVertex(int v, int bone, float value, PaintMode mode,
                           bool autoNormalize) {
    if (v < 0 || v >= vertexCount) return 0;
    bool changed = false;
    switch (mode) {
        case kPaintAdd:
            changed = addWeight(v, bone, value) != 0;
            break;
        case kPaintSubtract:
            changed = addWeight(v, bone, -value) != 0;
            break;
        case kPaintSet:
            changed = setWeight(v, bone, value) != 0;
            break;
    }
    if (changed && autoNormalize) normalizeVertex(v);
    return changed ? 1 : 0;
}

int WeightSet::paint(const std::vector<int>& verts, int bone, float value,
                     PaintMode mode, bool autoNormalize) {
    int c = 0;
    for (int v : verts) c += paintVertex(v, bone, value, mode, autoNormalize);
    return c;
}

int WeightSet::clearBone(int bone) {
    if (bone < 0) return 0;
    int c = 0;
    for (int v = 0; v < vertexCount; ++v) {
        if (weightOf(v, bone) <= 0.0f) continue;
        if (setWeight(v, bone, 0.0f)) ++c;
        // 摘掉一根骨之后剩下的必须重新归一化，否则和 < 1
        normalizeVertex(v);
    }
    return c;
}

int WeightSet::blur(const std::vector<int>& ringOff, const std::vector<int>& ringIdx,
                    int rounds, float strength) {
    if (rounds <= 0 || vertexCount <= 0) return 0;
    if (strength <= 0.0f) return 0;
    if (strength > 1.0f) strength = 1.0f;
    if (ringOff.size() != static_cast<std::size_t>(vertexCount) + 1) return 0;

    int touchedCount = 0;
    std::vector<WeightEntry> acc;
    for (int r = 0; r < rounds; ++r) {
        // 每轮先攒平均再写回：一边算一边写会让"已处理的顶点"污染后处理的
        // 顶点，结果和顶点顺序相关（同一份数据两个遍历序两个结果）。
        std::vector<std::vector<WeightEntry>> next(static_cast<std::size_t>(vertexCount));
        std::vector<char> mark(static_cast<std::size_t>(vertexCount), 0);
        for (int v = 0; v < vertexCount; ++v) {
            const std::size_t sv = static_cast<std::size_t>(v);
            const int a0 = ringOff[sv];
            const int a1 = ringOff[sv + 1];
            if (a1 <= a0) continue;
            // 只对"本顶点或邻居里有表"的做：整张网格都往上扩散权重 =
            // 刷一滴墨水最后染满整件衣服。
            bool any = influenceCount(v) > 0;
            for (int k = a0; k < a1 && !any; ++k)
                if (influenceCount(ringIdx[static_cast<std::size_t>(k)]) > 0) any = true;
            if (!any) continue;

            // 累加：自己权重 1，每个邻居 1/deg
            acc.clear();
            const auto addIn = [&](int src, float scale) {
                const int n = influenceCount(src);
                const std::size_t base = static_cast<std::size_t>(src) * kMaxInfluence;
                for (int i = 0; i < n; ++i) {
                    const WeightEntry& e = entries[base + static_cast<std::size_t>(i)];
                    acc.push_back(WeightEntry{e.bone, e.weight * scale});
                }
            };
            const float deg = static_cast<float>(a1 - a0);
            addIn(v, 1.0f);
            for (int k = a0; k < a1; ++k)
                addIn(ringIdx[static_cast<std::size_t>(k)], 1.0f / std::max(deg, 1.0f));

            // 合并同骨
            std::sort(acc.begin(), acc.end(),
                      [](const WeightEntry& x, const WeightEntry& y) {
                          return x.bone < y.bone;
                      });
            std::vector<WeightEntry> merged;
            for (const WeightEntry& e : acc) {
                if (!merged.empty() && merged.back().bone == e.bone)
                    merged.back().weight += e.weight;
                else
                    merged.push_back(e);
            }
            std::sort(merged.begin(), merged.end(), entryBetter);
            if (static_cast<int>(merged.size()) > kMaxInfluence)
                merged.resize(kMaxInfluence);
            float sum = 0.0f;
            for (const WeightEntry& e : merged) sum += e.weight;
            if (sum <= kWeightEps) continue;
            for (WeightEntry& e : merged) e.weight /= sum;

            // 与旧值按 strength 混合
            std::vector<WeightEntry> outList;
            const int n = influenceCount(v);
            const std::size_t base = sv * kMaxInfluence;
            for (const WeightEntry& e : merged) {
                float old = 0.0f;
                for (int i = 0; i < n; ++i)
                    if (entries[base + static_cast<std::size_t>(i)].bone == e.bone) {
                        old = entries[base + static_cast<std::size_t>(i)].weight;
                        break;
                    }
                outList.push_back(WeightEntry{e.bone, old * (1.0f - strength) +
                                                            e.weight * strength});
            }
            // 旧值里有、平均里没有的骨（邻居都没它）按 (1-strength) 缩着留下
            for (int i = 0; i < n; ++i) {
                const WeightEntry& e = entries[base + static_cast<std::size_t>(i)];
                bool found = false;
                for (const WeightEntry& o : outList)
                    if (o.bone == e.bone) { found = true; break; }
                if (!found)
                    outList.push_back(WeightEntry{e.bone, e.weight * (1.0f - strength)});
            }
            std::sort(outList.begin(), outList.end(), entryBetter);
            if (static_cast<int>(outList.size()) > kMaxInfluence)
                outList.resize(kMaxInfluence);
            float sum2 = 0.0f;
            for (const WeightEntry& e : outList) sum2 += e.weight;
            if (sum2 <= kWeightEps) continue;
            for (WeightEntry& e : outList) e.weight /= sum2;

            next[sv] = std::move(outList);
            mark[sv] = 1;
        }
        // 写回
        for (int v = 0; v < vertexCount; ++v) {
            const std::size_t sv = static_cast<std::size_t>(v);
            if (!mark[sv]) continue;
            const std::size_t base = sv * kMaxInfluence;
            const int n = static_cast<int>(next[sv].size());
            for (int i = 0; i < kMaxInfluence; ++i) {
                if (i < n) entries[base + static_cast<std::size_t>(i)] = next[sv][static_cast<std::size_t>(i)];
                else entries[base + static_cast<std::size_t>(i)] = WeightEntry{};
            }
            count[sv] = static_cast<unsigned char>(n);
            touched[sv] = 1;
            ++touchedCount;
        }
    }
    return touchedCount;
}

// ============================================================
// 自动权重
// ============================================================

int WeightSet::autoAssign(const RigEdit& rig, const std::vector<glm::vec3>& vertPos,
                          const glm::mat4& meshToRig, const AutoWeightParams& p) {
    if (vertexCount <= 0) return 0;
    if (static_cast<int>(vertPos.size()) != vertexCount) {
        VK_LOG_WARN("WEIGHTS-AUTO-SKIP reason=verpos-mismatch want=%d got=%d",
                    vertexCount, static_cast<int>(vertPos.size()));
        return 0;
    }
    const int nb = rig.boneCount();
    if (nb <= 0) return 0;

    // ---- 骨的世界段（骨架空间 → 网格空间）----
    //
    // ⚠ **不要**再乘 `restWorld()` 的矩阵。`EditBone::head` 已经是**绝对位置**
    //   （骨架空间里 `restLocal` 累乘出来的那个空间），`world[i][3]` 也只是
    //   把同一个值抄了一遍（`restWorld` 的构造保证 `world[i] · head == head`）。
    //   多乘一次的表现在画面上是"包络整体离身体远一倍"：大骨向外的顶点被
    //   更远的骨抢走，躯干上留下一圈"没跟上"的皮，而日志一行都不报。
    //   第一次就踩了这个 —— 自检里断言了 `world[2][3]` 的位置是对的，
    //   于是失败只出现在"权重跑到哪里去了"上。
    std::vector<glm::vec3> head(static_cast<std::size_t>(nb));
    std::vector<glm::vec3> tail(static_cast<std::size_t>(nb));
    std::vector<float> radius(static_cast<std::size_t>(nb), 0.0f);
    std::vector<char> use(static_cast<std::size_t>(nb), 0);
    int usable = 0;
    for (int i = 0; i < nb; ++i) {
        const EditBone& b = rig.bones[static_cast<std::size_t>(i)];
        // 约定 ③：deform 关掉的骨不参与
        if (p.deformOnly && !b.deform) {
            // 打出来（而不是静默跳过）：自动权重"某根骨完全没分到权重"时，
            // 第一个要排除的可能就是它本来就没开 deform。
            VK_LOG_INFO("WEIGHTS-AUTO-SKIP-BONE i=%d name='%s' reason=not-deform",
                        i, b.name.c_str());
            continue;
        }
        head[static_cast<std::size_t>(i)] =
            glm::vec3(meshToRig * glm::vec4(b.head, 1.0f));
        tail[static_cast<std::size_t>(i)] =
            glm::vec3(meshToRig * glm::vec4(b.tail(), 1.0f));
        radius[static_cast<std::size_t>(i)] =
            std::max(b.length * p.radiusScale, p.minRadius);
        use[static_cast<std::size_t>(i)] = 1;
        ++usable;
        // 每根骨一行：自动权重"看起来没效果"时，问题几乎总在这四个数上
        //（骨在世界里不在你以为的位置 / 半径小到覆盖不到任何顶点）。
        // 一行日志省一次二分排查，而且只在按下"自动权重"时打一次。
        VK_LOG_INFO("WEIGHTS-AUTO-BONE i=%d name='%s' use=1 head=(%.4f,%.4f,%.4f) "
                    "tail=(%.4f,%.4f,%.4f) len=%.4f r=%.4f",
                    i, b.name.c_str(), static_cast<double>(head[static_cast<std::size_t>(i)].x),
                    static_cast<double>(head[static_cast<std::size_t>(i)].y),
                    static_cast<double>(head[static_cast<std::size_t>(i)].z),
                    static_cast<double>(tail[static_cast<std::size_t>(i)].x),
                    static_cast<double>(tail[static_cast<std::size_t>(i)].y),
                    static_cast<double>(tail[static_cast<std::size_t>(i)].z),
                    static_cast<double>(b.length),
                    static_cast<double>(radius[static_cast<std::size_t>(i)]));
    }
    if (usable <= 0) {
        VK_LOG_WARN("WEIGHTS-AUTO-SKIP reason=no-deform-bone bones=%d", nb);
        return 0;
    }

    const float falloff = std::max(p.falloff, 0.01f);
    const int keep = std::max(1, std::min(p.maxInfluence, kMaxInfluence));

    int assigned = 0;
    int outside = 0;    // 落在所有包络之外、被兜底塞给最近那根骨的顶点数
    std::vector<std::pair<float, int>> scored;
    for (int v = 0; v < vertexCount; ++v) {
        const glm::vec3 pos = vertPos[static_cast<std::size_t>(v)];
        scored.clear();
        float best = -1.0f;
        int bestBone = -1;
        float bestD = 1.0e30f;
        for (int i = 0; i < nb; ++i) {
            if (!use[static_cast<std::size_t>(i)]) continue;
            const float d = pointSegmentDistance(pos, head[static_cast<std::size_t>(i)],
                                                tail[static_cast<std::size_t>(i)]);
            // 无论如何记一下最近的那根（兜底用，见下面的 isoloated 分支）
            if (d < bestD) { bestD = d; bestBone = i; }
            const float r = radius[static_cast<std::size_t>(i)];
            if (d >= r) continue;
            float w = 1.0f - d / r;
            // 比 `kWeightEps` 还小的不给槽位：它既不影响画面，又白占一格
            //（GPU 只有四格）。更要紧的是**存盘一致性** —— 落盘时这种
            // 权重会被写进文件，而读盘侧按同样的阈值过滤，于是"存一次
            // 读一次"的摘要对不上。数据在源头干净，两端就不用各定一套规矩。
            if (w <= kWeightEps) continue;
            if (falloff != 1.0f) w = std::pow(w, falloff);
            scored.emplace_back(w, i);
            if (w > best) best = w;
        }

        const std::size_t sv = static_cast<std::size_t>(v);
        const std::size_t base = sv * kMaxInfluence;
        for (int i = 0; i < kMaxInfluence; ++i)
            entries[base + static_cast<std::size_t>(i)] = WeightEntry{};

        if (scored.empty()) {
            // 落在所有包络之外：塞给最近那根骨（约定 ②：不许留孤立顶点）。
            // 这是**有意的**硬边 —— 比"顶点原地不动、角色一动就露一坨皮"
            // 好得多，而且 stats().unweighted 会告诉我们有多少个。
            entries[base].bone = bestBone;
            entries[base].weight = 1.0f;
            count[sv] = 1;
            touched[sv] = 1;
            ++assigned;
            ++outside;
            continue;
        }

        std::sort(scored.begin(), scored.end(),
                  [](const std::pair<float, int>& x, const std::pair<float, int>& y) {
                      if (x.first != y.first) return x.first > y.first;
                      return x.second < y.second;
                  });
        if (static_cast<int>(scored.size()) > keep) scored.resize(static_cast<std::size_t>(keep));
        float sum = 0.0f;
        for (const auto& s : scored) sum += s.first;
        if (sum <= kWeightEps) {
            entries[base].bone = bestBone;
            entries[base].weight = 1.0f;
            count[sv] = 1;
        } else {
            const float inv = 1.0f / sum;
            for (std::size_t i = 0; i < scored.size(); ++i) {
                entries[base + i].bone = scored[i].second;
                entries[base + i].weight = scored[i].first * inv;
            }
            count[sv] = static_cast<unsigned char>(scored.size());
        }
        touched[sv] = 1;
        ++assigned;
    }

    // 覆盖率是这一层**唯一**的自检：包络覆盖不到一半时，"自动权重"其实
    // 全走的是"最近那根骨 w=1"的兜底 —— 结果是角色一动就从中间撕开，
    // 而单独看日志一切正常（每个顶点的权重和都是 1）。
    //
    // 真实触发过：`anime_girl.fbx` 那具骨架的 rest 姿态里**腿骨的世界位置
    // 落在骨盆上方**（第三方 FBX 的节点默认变换不是绑定姿态），于是腿上的
    // 顶点全部落在所有包络之外。这种数据问题只能在覆盖率上暴露出来。
    const double pct = assigned > 0 ? (100.0 * outside / assigned) : 0.0;
    VK_LOG_INFO("WEIGHTS-AUTO-COVER mesh='%s' verts=%d outside=%d pct=%.1f%% "
                "radiusScale=%.2f falloff=%.2f keep=%d",
                mesh.c_str(), assigned, outside, pct,
                static_cast<double>(p.radiusScale), static_cast<double>(p.falloff),
                keep);
    if (pct > 50.0) {
        VK_LOG_WARN("WEIGHTS-AUTO-LOWCOVER mesh='%s' outside=%.1f%%（包络压根没盖住"
                    "网格 —— 先看 WEIGHTS-AUTO-BONE 的骨位置是不是和模型对得上）",
                    mesh.c_str(), pct);
    }
    return assigned;
}

int WeightSet::mirrorVertex(const RigEdit& rig, int axis,
                            const std::vector<glm::vec3>& vertPos, int v, float eps) {
    if (v < 0 || v >= vertexCount) return -1;
    if (static_cast<int>(vertPos.size()) != vertexCount) return -1;
    const int ax = std::max(0, std::min(2, axis));
    const glm::vec3 p = vertPos[static_cast<std::size_t>(v)];
    glm::vec3 mp = p;
    mp[static_cast<glm::length_t>(ax)] = -mp[static_cast<glm::length_t>(ax)];

    // 找镜像位置最近的顶点。真实角色的左右**不会**精确对称（拓扑、蒙皮
    // 之后的位置都会有毫米级差），所以按容差找最近，而不是要求相等。
    int best = -1;
    float bestD = eps;
    for (int i = 0; i < vertexCount; ++i) {
        if (i == v) continue;
        const float d = glm::length(vertPos[static_cast<std::size_t>(i)] - mp);
        if (d <= bestD) { bestD = d; best = i; }
    }
    if (best < 0) return -1;

    const int n = influenceCount(v);
    const std::size_t base = static_cast<std::size_t>(v) * kMaxInfluence;
    const std::size_t dbase = static_cast<std::size_t>(best) * kMaxInfluence;
    std::vector<WeightEntry> outList;
    outList.reserve(static_cast<std::size_t>(n));
    int flipped = 0;
    for (int i = 0; i < n; ++i) {
        const WeightEntry& e = entries[base + static_cast<std::size_t>(i)];
        int bone = e.bone;
        if (bone >= 0 && bone < rig.boneCount()) {
            const std::string other =
                RigEdit::flipSide(rig.bones[static_cast<std::size_t>(bone)].name);
            if (!other.empty()) {
                const int k = rig.indexOf(other);
                if (k >= 0) { bone = k; ++flipped; }
            }
        }
        outList.push_back(WeightEntry{bone, e.weight});
    }
    if (flipped == 0) return -1;   // 骨名没有 .L/.R 标记 → 对面那半边无意义

    for (int i = 0; i < kMaxInfluence; ++i)
        entries[dbase + static_cast<std::size_t>(i)] = WeightEntry{};
    for (std::size_t i = 0; i < outList.size(); ++i)
        entries[dbase + i] = outList[i];
    count[static_cast<std::size_t>(best)] = static_cast<unsigned char>(outList.size());
    touched[static_cast<std::size_t>(best)] = 1;
    normalizeVertex(best);
    return best;
}

// ============================================================
// 统计 / 日志
// ============================================================

WeightStats WeightSet::stats() const {
    WeightStats s;
    s.vertices = vertexCount;
    if (vertexCount <= 0) return s;
    s.minSum = 1.0e30f;
    s.maxSum = -1.0e30f;
    double sumInfluence = 0.0;
    int effective = 0;
    for (int v = 0; v < vertexCount; ++v) {
        const std::size_t sv = static_cast<std::size_t>(v);
        const int n = static_cast<int>(count[sv]);
        if (touched[sv]) ++s.touched;
        if (n > s.maxInfluences) s.maxInfluences = n;
        if (n > 4) ++s.overFour;
        sumInfluence += n;
        float sm = 0.0f;
        for (int i = 0; i < n; ++i)
            sm += entries[sv * kMaxInfluence + static_cast<std::size_t>(i)].weight;
        if (n == 0 || sm <= kWeightEps) {
            ++s.unweighted;
            continue;
        }
        ++effective;
        if (sm < s.minSum) s.minSum = sm;
        if (sm > s.maxSum) s.maxSum = sm;
    }
    if (effective == 0) { s.minSum = 0.0f; s.maxSum = 0.0f; }
    s.avgInfluences = static_cast<double>(sumInfluence) /
                      static_cast<double>(std::max(vertexCount, 1));
    return s;
}

void WeightSet::logSummary(const char* tag) const {
    const WeightStats s = stats();
    VK_LOG_INFO("WEIGHTS-SUMMARY tag=%s mesh='%s' vtx=%d touched=%d unweighted=%d "
                "maxInf=%d over4=%d avgInf=%.3f sum=[%.6f,%.6f]",
                tag ? tag : "-", mesh.c_str(), s.vertices, s.touched, s.unweighted,
                s.maxInfluences, s.overFour, s.avgInfluences,
                static_cast<double>(s.minSum), static_cast<double>(s.maxSum));
}

void WeightSet::dumpVertex(int v, const char* tag) const {
    if (v < 0 || v >= vertexCount) return;
    const std::size_t sv = static_cast<std::size_t>(v);
    const int n = static_cast<int>(count[sv]);
    // 逐位可比的十六进制浮点（`%a`）：十进制打印会在第 7 位后四舍五入，
    // "两次操作结果一致"这种断言就永远只能近似。
    if (n == 0) {
        VK_LOG_INFO("WEIGHTS-DUMP tag=%s mesh='%s' v=%d n=0", tag ? tag : "-",
                    mesh.c_str(), v);
        return;
    }
    char buf[512];
    int off = 0;
    for (int i = 0; i < n && off < static_cast<int>(sizeof(buf)) - 64; ++i) {
        const WeightEntry& e = entries[sv * kMaxInfluence + static_cast<std::size_t>(i)];
        off += std::snprintf(buf + off, sizeof(buf) - static_cast<std::size_t>(off),
                             "%s%d:%a", i ? "," : "", e.bone,
                             static_cast<double>(e.weight));
    }
    VK_LOG_INFO("WEIGHTS-DUMP tag=%s mesh='%s' v=%d n=%d e=%s", tag ? tag : "-",
                mesh.c_str(), v, n, buf);
}

unsigned long long WeightSet::digest() const {
    // FNV-1a 64。只喂**有效条目**（count 决定），touched 与 orig 不进摘要 ——
    // 摘要要回答的是"权重表一样吗"，不是"内存布局一样吗"。
    unsigned long long h = 1469598103934665603ULL;
    const auto feed = [&h](const void* p, std::size_t n) {
        const unsigned char* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) {
            h ^= static_cast<unsigned long long>(b[i]);
            h *= 1099511628211ULL;
        }
    };
    for (int v = 0; v < vertexCount; ++v) {
        const std::size_t sv = static_cast<std::size_t>(v);
        const int n = static_cast<int>(count[sv]);
        feed(&n, sizeof(n));
        for (int i = 0; i < n; ++i) {
            const WeightEntry& e = entries[sv * kMaxInfluence + static_cast<std::size_t>(i)];
            feed(&e.bone, sizeof(e.bone));
            feed(&e.weight, sizeof(e.weight));
        }
    }
    return h;
}

// ============================================================
// 网格辅助
// ============================================================

void buildVertexRing(const assets::Mesh& mesh, std::vector<int>& ringOff,
                     std::vector<int>& ringIdx) {
    buildVertexRing(mesh.indices(), static_cast<int>(mesh.vertices().size()),
                    ringOff, ringIdx);
}

void buildVertexRing(const std::vector<uint32_t>& indices, int vertexCount,
                     std::vector<int>& ringOff, std::vector<int>& ringIdx) {
    ringOff.clear();
    ringIdx.clear();
    const std::size_t n = static_cast<std::size_t>(std::max(vertexCount, 0));
    if (n == 0) return;
    // CSR 两遍：先数度数，再填
    std::vector<int> deg(n + 1, 0);
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        const uint32_t a = indices[i], b = indices[i + 1], c = indices[i + 2];
        if (a >= n || b >= n || c >= n) continue;
        ++deg[a]; ++deg[b]; ++deg[c];
        ++deg[a]; ++deg[b]; ++deg[c];   // 每个三角形里每个顶点有两条邻边
    }
    ringOff.assign(n + 1, 0);
    for (std::size_t i = 0; i < n; ++i)
        ringOff[i + 1] = ringOff[i] + deg[i];
    ringIdx.assign(static_cast<std::size_t>(ringOff[n]), -1);
    const auto push = [&](uint32_t a, uint32_t b) {
        const std::size_t base = static_cast<std::size_t>(a);
        for (int k = ringOff[base]; k < ringOff[base + 1]; ++k) {
            if (ringIdx[static_cast<std::size_t>(k)] == static_cast<int>(b)) return;
            if (ringIdx[static_cast<std::size_t>(k)] < 0) {
                ringIdx[static_cast<std::size_t>(k)] = static_cast<int>(b);
                return;
            }
        }
    };
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        const uint32_t a = indices[i], b = indices[i + 1], c = indices[i + 2];
        if (a >= n || b >= n || c >= n) continue;
        push(a, b); push(b, a);
        push(b, c); push(c, b);
        push(c, a); push(a, c);
    }
    // 去掉"没填满"的空槽（同一对顶点在多个三角形里出现时，重复塞的已被
    // push 里的查重挡掉，于是末尾会剩 -1）。重排成紧凑 CSR。
    std::vector<int> off2(n + 1, 0);
    std::vector<int> idx2;
    idx2.reserve(ringIdx.size());
    for (std::size_t i = 0; i < n; ++i) {
        off2[i] = static_cast<int>(idx2.size());
        for (int k = ringOff[i]; k < ringOff[i + 1]; ++k) {
            const int nb = ringIdx[static_cast<std::size_t>(k)];
            if (nb >= 0) idx2.push_back(nb);
        }
    }
    off2[n] = static_cast<int>(idx2.size());
    ringOff.swap(off2);
    ringIdx.swap(idx2);
    (void)deg;
}

void meshVertexPositions(const assets::Mesh& mesh, std::vector<glm::vec3>& out) {
    const std::vector<assets::Vertex>& verts = mesh.vertices();
    out.resize(verts.size());
    for (std::size_t i = 0; i < verts.size(); ++i) out[i] = verts[i].pos;
}

// ============================================================
// JSON（venn.weights / version 1）
// ============================================================

bool saveWeights(const std::string& path, const WeightSet& ws, std::string& err) {
    err.clear();
    if (path.empty()) {
        err = "路径为空";
        return false;
    }
    json doc;
    doc["format"] = "venn.weights";
    doc["version"] = 1;
    doc["source"] = ws.source;
    doc["mesh"] = ws.mesh;
    doc["vertexCount"] = ws.vertexCount;

    // 稀疏：只存 count > 0 的顶点。角色网格动辄几万顶点，全量落盘是
    // 几十兆的 JSON，而"没有权重的顶点"本来就是 0。
    json arr = json::array();
    for (int v = 0; v < ws.vertexCount; ++v) {
        const int n = ws.influenceCount(v);
        if (n <= 0) continue;
        json o;
        o["v"] = v;
        json es = json::array();
        for (int i = 0; i < n; ++i) {
            const WeightEntry& e = ws.entries[static_cast<std::size_t>(v) * kMaxInfluence +
                                              static_cast<std::size_t>(i)];
            es.push_back(json::array({e.bone, e.weight}));
        }
        o["e"] = std::move(es);
        arr.push_back(std::move(o));
    }
    const std::size_t stored = arr.size();
    doc["verts"] = std::move(arr);

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        err = "写不了文件";
        return false;
    }
    f << doc.dump(1);
    if (!f) {
        err = "写入失败";
        return false;
    }
    VK_LOG_INFO("WEIGHTS-SAVE path='%s' mesh='%s' vtx=%d stored=%d", path.c_str(),
                ws.mesh.c_str(), ws.vertexCount, static_cast<int>(stored));
    return true;
}

bool loadWeights(const std::string& path, WeightSet& out, std::string& err) {
    err.clear();
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "打不开文件";
        return false;
    }
    json doc;
    try {
        f >> doc;
    } catch (const std::exception& e) {
        err = std::string("JSON 解析失败: ") + e.what();
        return false;
    }
    if (!doc.is_object() || doc.value("format", std::string()) != "venn.weights") {
        err = "不是 venn.weights 文件";
        return false;
    }
    WeightSet ws;
    ws.source = doc.value("source", std::string());
    ws.mesh = doc.value("mesh", std::string());
    ws.vertexCount = doc.value("vertexCount", 0);
    if (ws.vertexCount <= 0) {
        err = "vertexCount 非法";
        return false;
    }
    const std::size_t n = static_cast<std::size_t>(ws.vertexCount);
    ws.entries.assign(n * kMaxInfluence, WeightEntry{});
    ws.count.assign(n, 0);
    // 文件里的表是"作者写成这样的"，没有原槽位可抄 → 一律标成 touched。
    // 于是读回来的第一次烘焙会走"按权重排序取前四"那条路，而不是照抄。
    ws.touched.assign(n, 1);
    ws.origJoints.assign(n, glm::uvec4(0, 0, 0, 0));
    ws.origWeights.assign(n, glm::vec4(0.0f));

    const auto it = doc.find("verts");
    if (it != doc.end() && it->is_array()) {
        for (const json& o : *it) {
            const int v = o.value("v", -1);
            if (v < 0 || v >= ws.vertexCount) continue;
            const auto es = o.find("e");
            if (es == o.end() || !es->is_array()) continue;
            int k = 0;
            for (const json& e : *es) {
                if (k >= kMaxInfluence) break;
                if (!e.is_array() || e.size() < 2) continue;
                const int bone = e[0].get<int>();
                const float w = e[1].get<float>();
                // 只挡非法值（负权重 / NaN），**不按 `kWeightEps` 过滤**：
                // 那个阈值是"刷子认为多少算没有"，不是"存盘保留几位"。
                // 拿它当读盘过滤会让"存一次读一次"的条目数变少 ——
                // 摘要对不上，而每个顶点的权重和仍然正常（最难查的一类）。
                if (bone < 0 || !(w > 0.0f) || !std::isfinite(w)) continue;
                ws.entries[static_cast<std::size_t>(v) * kMaxInfluence +
                           static_cast<std::size_t>(k)] = WeightEntry{bone, w};
                ++k;
            }
            ws.count[static_cast<std::size_t>(v)] = static_cast<unsigned char>(k);
        }
    }
    out = std::move(ws);
    VK_LOG_INFO("WEIGHTS-LOAD path='%s' mesh='%s' vtx=%d", path.c_str(),
                out.mesh.c_str(), out.vertexCount);
    return true;
}

} // namespace animation
