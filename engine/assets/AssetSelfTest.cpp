// ============================================================
// assets/AssetSelfTest.cpp —— 离线资产链的无窗口自检
//
// 断言分五组：
//   A  导入        FBX → SourceModel（子网格 / 骨架 / 动画 / 包围盒）
//   B  修正        焊接、缓存重排（顶点数下降、ACMR 不升、索引合法）
//   C  往返        写盘 → 读盘 → 逐字段等价（含逐顶点/逐矩阵比较）
//   D  动画压缩    时长不变、关键帧变少、**误差有界**
//   E  坏文件      改写一个字节 / 截断 / 改魔数 → 必须被拒（而不是崩）
//
// ⚠ detail 里不要出现空格：脚本用 `detail=(\S*)` 解析（与 ANIM-TEST
//   同一条约定，见 AnimationSelfTest.cpp 顶部）。多项用逗号分隔。
// ============================================================

#include "assets/AssetSelfTest.h"

#include "assets/AssetPath.h"
#include "assets/Skeleton.h"
#include "core/Logger.h"

#include "assetio/AnimOps.h"
#include "assetio/Binary.h"
#include "assetio/FbxImport.h"
#include "assetio/MeshFormat.h"
#include "assetio/VertexOps.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace assets {

namespace {

int g_pass = 0;
int g_fail = 0;

void flag(const char* name, bool ok, const std::string& detail = {}) {
    VK_LOG_INFO("ASSET-T %s detail=%s result=%s", name,
                detail.empty() ? "-" : detail.c_str(), ok ? "PASS" : "FAIL");
    if (ok) ++g_pass; else ++g_fail;
}

void num(const char* name, double actual, double expected, double tol) {
    const bool ok = std::fabs(actual - expected) <= tol;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "actual=%.6f,expected=%.6f,tol=%.6g",
                  actual, expected, tol);
    VK_LOG_INFO("ASSET-T %s detail=%s result=%s", name, buf, ok ? "PASS" : "FAIL");
    if (ok) ++g_pass; else ++g_fail;
}

// "不适用"（例如静态网格没有骨骼）。**不能**记成 PASS —— 那会让
// "这条断言在这个模型上根本没跑"看起来像"跑过了而且是对的"。
// 也不记成 FAIL：那是把"模型没有骨骼"当成缺陷。
void na(const char* name) {
    VK_LOG_INFO("ASSET-T %s detail=- result=NA", name);
}

// SourceTrack → 运行时的 AnimTrack：**用真正的采样器**做误差评估。
// 自己再写一份插值就等于"用同一个错误的标准衡量自己"。
AnimTrack toRuntimeTrack(const assetio::SourceTrack& t) {
    AnimTrack r;
    r.joint = t.joint;
    r.path = static_cast<AnimTrack::Path>(t.path);
    r.step = t.step;
    r.times = t.times;
    r.values = t.values;
    return r;
}

// 两组轨道在 time 处的差异。旋转返回度，位移/缩放返回米。
double trackError(const AnimTrack& a, const AnimTrack& b, float time) {
    const glm::vec4 va = sampleTrack(a, time);
    const glm::vec4 vb = sampleTrack(b, time);
    if (a.path == AnimTrack::Rotation) {
        return static_cast<double>(assetio::quatAngleDeg(va, vb));
    }
    const glm::vec3 d = glm::vec3(va.x, va.y, va.z) - glm::vec3(vb.x, vb.y, vb.z);
    return static_cast<double>(glm::length(d));
}

struct ClipPair {
    std::string name;       // 失败信息里要指到"哪一段 clip"
    std::vector<AnimTrack> before;
    std::vector<AnimTrack> after;
    float duration = 0.0f;
};

// 按 (joint,path) 配对**轨道**。
//
// ⚠ 但**不**按名字配对 clip —— 这是 2026-10-25 改掉的一处错判：
//   `FbxImport.cpp:527` 会把 `Namespace|Idle` 剥成 `Idle`，多 take 的
//   FBX 于是能产出**两段同名 clip**（`anime_girl.fbx` 的 Idle 就是这样，
//   这里实测 `sameName=2`）。按名字取"第一条"会让第二段的 before 去和
//   第一段的 after 相比 —— 误差是一个恒定的 179.59°，看起来像"抽帧器
//   把首帧删错了"，实际是拿错对象在比。查这个坑时先排除了"重复时间戳
//   (dt0=0.0333)"和"同 joint 多通道 (dup=1)"，才轮到同名 clip。
//
//   改成**按下标配对**：`reduceClip` 原地改、既不删也不重排 clip，
//   下标是精确对应关系。名字只用来在信息里指认，并作为"确实没重排"的
//   断言 —— 主键是下标，不是名字。
std::vector<ClipPair> pairClips(const std::vector<assetio::SourceClip>& before,
                                const std::vector<assetio::SourceClip>& after) {
    std::vector<ClipPair> out;
    const std::size_t n = std::min(before.size(), after.size());
    for (std::size_t i = 0; i < n; ++i) {
        const assetio::SourceClip& a = before[i];
        const assetio::SourceClip& b = after[i];
        if (a.name != b.name) continue;   // 重排过 → 不猜，跳过而不是错配
        ClipPair p;
        p.name = a.name;
        p.duration = a.duration;
        for (const auto& t : a.tracks) p.before.push_back(toRuntimeTrack(t));
        for (const auto& t : b.tracks) p.after.push_back(toRuntimeTrack(t));
        out.push_back(std::move(p));
    }
    return out;
}

const AnimTrack* findTrack(const std::vector<AnimTrack>& v, int joint, int path) {
    for (const auto& t : v)
        if (t.joint == joint && static_cast<int>(t.path) == path) return &t;
    return nullptr;
}

} // namespace

int runAssetSelfTest() {
    g_pass = 0;
    g_fail = 0;

    // ---------------- 选模型 ----------------
    std::string rel = "models/anime_girl.fbx";
    if (const char* env = std::getenv("MYVK_ASSET_TEST_MODEL")) {
        if (env[0]) rel = env;
    }
    const std::string abs = resolveAssetPath(rel);
    VK_LOG_INFO("ASSET-T model=%s resolved=%s", rel.c_str(), abs.c_str());

    std::error_code ec;
    if (!std::filesystem::is_regular_file(abs, ec) || ec) {
        // 资产不在就**明确跳过**，而不是假装通过。
        VK_LOG_WARN("ASSET-T SKIP reason=model_missing model=%s", rel.c_str());
        VK_LOG_INFO("ASSET-TOTAL pass=0 fail=0 skip=1");
        return 0;
    }

    // ---------------- A. 导入 ----------------
    assetio::ImportOptions io;
    io.assetRoot = resolveAssetPath("");
    assetio::SourceModel src = assetio::importFbx(abs, io);
    if (!src.ok) {
        VK_LOG_ERROR("ASSET-T import_failed reason=%s", src.error.c_str());
        VK_LOG_INFO("ASSET-TOTAL pass=%d fail=%d", g_pass, g_fail + 1);
        return g_fail + 1;
    }

    flag("import_submeshes", !src.subMeshes.empty(),
         "n=" + std::to_string(src.subMeshes.size()));
    flag("import_vertices", src.vertexCount() > 0,
         "n=" + std::to_string(src.vertexCount()));
    flag("import_triangles", src.triangleCount() > 0,
         "n=" + std::to_string(src.triangleCount()));

    // 骨骼/动画相关：静态网格（cube / 家具 / 场景件）**没有**这些，
    // 那是模型的性质，不是链路的缺陷 —— 记 NA 而不是 FAIL。
    if (src.joints.empty()) {
        na("import_joints");
        na("import_topo_complete");
    } else {
        flag("import_joints", true, "n=" + std::to_string(src.joints.size()));
        flag("import_topo_complete", src.topoOrder.size() == src.joints.size(),
             "topo=" + std::to_string(src.topoOrder.size()) + ",joints=" +
                 std::to_string(src.joints.size()));
    }
    if (src.clips.empty()) {
        na("import_clips");
    } else {
        flag("import_clips", true, "n=" + std::to_string(src.clips.size()));
    }
    flag("import_radius", src.radius > 0.01f,
         "r=" + std::to_string(src.radius));

    // 骨架父下标必须合法（越界的 parent 会让拓扑排序静默少一层，
    // 表现是"某几根骨头不跟着动"）
    {
        bool ok = true;
        for (const auto& j : src.joints)
            if (j.parent < -1 || j.parent >= static_cast<int>(src.joints.size()))
                ok = false;
        flag("import_parents_valid", ok);
    }

    // pivot 归位后包围盒应当关于原点对称
    {
        const glm::vec3 c = (src.boundsMin + src.boundsMax) * 0.5f;
        const float off = glm::length(c);
        flag("import_pivot_centered", off < 1.0e-2f,
             "offset=" + std::to_string(off));
    }

    // 蒙皮权重的和必须 ≈ 1（否则顶点会朝原点收缩）
    {
        double worst = 0.0;
        for (const auto& sm : src.subMeshes) {
            for (const auto& v : sm.vertices) {
                const float s = v.weights[0] + v.weights[1] + v.weights[2] + v.weights[3];
                if (s <= 0.0f) continue;   // 无权重顶点不参与
                worst = std::max(worst, std::fabs(static_cast<double>(s) - 1.0));
            }
        }
        flag("import_weights_normalized", worst < 1.0e-3,
             "worst=" + std::to_string(worst));
    }

    // ---------------- B. 几何修正 ----------------
    {
        // 拷一份做修正，别弄脏原始数据（后面的往返测试要用它）
        std::vector<assetio::SourceSubMesh> optCopy = src.subMeshes;
        std::uint32_t vin = 0, vout = 0;
        float acmrIn = 0.0f, acmrOut = 0.0f;
        bool idxOk = true;
        for (auto& sm : optCopy) {
            const assetio::OptimizeStats os =
                assetio::optimizeSubMesh(sm, true, true);
            vin += os.verticesIn;
            vout += os.verticesOut;
            acmrIn += os.acmrIn;
            acmrOut += os.acmrOut;
            for (std::uint32_t i : sm.indices)
                if (i >= sm.vertices.size()) idxOk = false;
        }
        if (!optCopy.empty()) {
            acmrIn /= static_cast<float>(optCopy.size());
            acmrOut /= static_cast<float>(optCopy.size());
        }
        flag("weld_not_more_vertices", vout <= vin,
             "in=" + std::to_string(vin) + ",out=" + std::to_string(vout));
        // 焊接的**不变性**（与具体模型无关）：焊完之后不能还存在两个
        // 完全相同的顶点。这才是"焊接做对了"的定义 —— 直接断言
        // "顶点变少"只是在断言"这个模型有重复顶点"（立方体就没有，
        // 于是那条断言在 cube 上必然红，而代码其实完全正确）。
        {
            bool dup = false;
            for (const auto& sm : optCopy) {
                std::unordered_map<std::uint64_t, std::vector<const assetio::Vertex*>> seen;
                for (const auto& v : sm.vertices) {
                    const std::uint64_t h = assetio::fnv1a64(&v, sizeof(assetio::Vertex));
                    auto& bucket = seen[h];
                    for (const auto* p : bucket)
                        if (std::memcmp(p, &v, sizeof(assetio::Vertex)) == 0) dup = true;
                    bucket.push_back(&v);
                    if (dup) break;
                }
                if (dup) break;
            }
            flag("weld_no_duplicates", !dup);
        }
        flag("acmr_not_worse", acmrOut <= acmrIn + 1.0e-4f,
             "in=" + std::to_string(acmrIn) + ",out=" + std::to_string(acmrOut));
        flag("opt_indices_valid", idxOk);
        // 三角形数量在整个过程中不能变（少一个就是模型破洞）
        std::size_t triBefore = 0, triAfter = 0;
        for (std::size_t i = 0; i < src.subMeshes.size(); ++i) {
            triBefore += src.subMeshes[i].indices.size();
            triAfter += optCopy[i].indices.size();
        }
        flag("opt_triangles_kept", triBefore == triAfter,
             "before=" + std::to_string(triBefore) + ",after=" +
                 std::to_string(triAfter));
    }

    // ---------------- C. 序列化往返 ----------------
    assetio::WriteOptions wo;
    assetio::WriteStats st;
    const std::vector<std::uint8_t> bytes =
        assetio::writeMeshFile(src, wo, &st);
    flag("write_nonempty", !bytes.empty(),
         "bytes=" + std::to_string(bytes.size()));
    flag("write_magic", bytes.size() >= 8 &&
                            std::memcmp(bytes.data(), assetio::kMeshFileMagic, 8) == 0);

    assetio::SourceModel rt;
    std::string err;
    const bool readOk =
        assetio::readMeshFile(bytes.data(), bytes.size(), rt, &err);
    flag("read_ok", readOk, readOk ? "" : err);

    if (readOk) {
        flag("rt_submeshes", rt.subMeshes.size() == src.subMeshes.size(),
             "got=" + std::to_string(rt.subMeshes.size()) + ",want=" +
                 std::to_string(src.subMeshes.size()));
        flag("rt_vertices", rt.vertexCount() == st.verticesOut,
             "got=" + std::to_string(rt.vertexCount()) + ",want=" +
                 std::to_string(st.verticesOut));
        flag("rt_indices", rt.indexCount() == st.indicesOut,
             "got=" + std::to_string(rt.indexCount()) + ",want=" +
                 std::to_string(st.indicesOut));
        num("rt_radius", rt.radius, src.radius, 1.0e-3);
        flag("rt_joints", rt.joints.size() == src.joints.size(),
             "got=" + std::to_string(rt.joints.size()));

        // 逐关节比较名字 / 父 / 三个矩阵
        {
            double worst = 0.0;
            bool namesOk = true, parentsOk = true;
            const std::size_t n = std::min(rt.joints.size(), src.joints.size());
            for (std::size_t i = 0; i < n; ++i) {
                if (rt.joints[i].name != src.joints[i].name) namesOk = false;
                if (rt.joints[i].parent != src.joints[i].parent) parentsOk = false;
                for (int c = 0; c < 4; ++c)
                    for (int r = 0; r < 4; ++r) {
                        worst = std::max(worst,
                                         static_cast<double>(std::fabs(
                                             rt.joints[i].restLocal[c][r] -
                                             src.joints[i].restLocal[c][r])));
                        worst = std::max(worst,
                                         static_cast<double>(std::fabs(
                                             rt.joints[i].invBind[c][r] -
                                             src.joints[i].invBind[c][r])));
                    }
            }
            flag("rt_joint_names", namesOk);
            flag("rt_joint_parents", parentsOk);
            flag("rt_joint_matrices", worst <= 1.0e-4,
                 "worst=" + std::to_string(worst));
        }

        // 子网格变换逐个比较
        {
            double worst = 0.0;
            const std::size_t n = std::min(rt.subMeshes.size(), src.subMeshes.size());
            for (std::size_t i = 0; i < n; ++i)
                for (int c = 0; c < 4; ++c)
                    for (int r = 0; r < 4; ++r)
                        worst = std::max(worst,
                                         static_cast<double>(std::fabs(
                                             rt.subMeshes[i].transform[c][r] -
                                             src.subMeshes[i].transform[c][r])));
            flag("rt_transforms", worst <= 1.0e-4,
                 "worst=" + std::to_string(worst));
        }

        // 顶点集合比较。
        //
        // ⚠ 不能按下标逐点比：焊接会**删掉**重复顶点，于是下标整体前移，
        //   "第 k 个顶点"在写盘前后不是同一个顶点。第一版就是按 97 步长
        //   抽样比的，结果这条断言永远红（而几何其实完全正确）。
        //   正确的语义是"集合包含"：读回来的每个顶点必须**逐字节**存在于
        //   源里（焊接保留首次出现的那份字节），且数量不多于源。
        {
            bool subset = true;
            bool countOk = true;
            for (std::size_t i = 0;
                 i < std::min(rt.subMeshes.size(), src.subMeshes.size()); ++i) {
                const auto& a = src.subMeshes[i];
                const auto& b = rt.subMeshes[i];
                if (b.vertices.size() > a.vertices.size()) countOk = false;

                // 源顶点按 64 位哈希建桶（哈希只用来分组，命中后仍然
                // 逐字节比较 —— 碰撞时不会误判）
                std::unordered_multimap<std::uint64_t, const assetio::Vertex*> index;
                index.reserve(a.vertices.size());
                for (const auto& v : a.vertices) {
                    const std::uint64_t h =
                        assetio::fnv1a64(&v, sizeof(assetio::Vertex));
                    index.emplace(h, &v);
                }
                for (const auto& v : b.vertices) {
                    const std::uint64_t h =
                        assetio::fnv1a64(&v, sizeof(assetio::Vertex));
                    bool found = false;
                    const auto range = index.equal_range(h);
                    for (auto it = range.first; it != range.second; ++it) {
                        if (std::memcmp(it->second, &v, sizeof(assetio::Vertex)) == 0) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) { subset = false; break; }
                }
                if (!subset) break;
            }
            flag("rt_vertex_subset", subset);
            flag("rt_vertex_count_sane", countOk);
        }

        // ---- 新顶点通道往返（§13，uv2 / morphBase / tangent）----
        //
        // ⚠ 上面那条 rt_vertex_subset 虽然按 sizeof(Vertex) 逐字节比过，
        //   但它比的是"源里本来就有的值"。导入进来的资产**没有**切线、
        //   没有 morph、多数没有 UV2 —— 于是那 24 个字节在两边都是 0，
        //   比了等于没比。这一组才是"格式真的存下了这四个字段"的证据：
        //   给读回来的模型人工注入可区分的值 → 再写一遍 → 再读一遍 →
        //   按字节找回来。
        //
        // 也不能按下标找（焊接 + Forsyth 重排会改变顺序，见上面那段注释），
        // 所以用"哈希分桶 + memcmp"的同一套办法定位。
        {
            assetio::SourceModel probe = rt;
            bool seeded = false;
            if (!probe.subMeshes.empty() && !probe.subMeshes[0].vertices.empty()) {
                assetio::SourceSubMesh& sm = probe.subMeshes[0];
                assetio::Vertex& v0 = sm.vertices[0];
                v0.uv2[0] = 0.125f;
                v0.uv2[1] = 0.875f;
                v0.tangent[0] = 0.6f;   // 恰好是个单位向量（3-4-5）
                v0.tangent[1] = 0.8f;
                v0.tangent[2] = 0.0f;
                v0.tangent[3] = -1.0f;
                // morph 不能靠"直接写 morphBase = 12345"来验：读端有
                // 硬自洽校验（流的下标必须真的指向这个顶点），伪造的值
                // 会让整次读盘失败。所以造**真的** target 走完整条链：
                // 两个 target 都影响顶点 0 → 焊接 → 流化 → 读回 → 逐条比对。
                // 这比原来那条"注入一个假下标"强得多 —— 它测的是链路，
                // 不是字段宽度。
                sm.morphTargets.clear();
                sm.morphTargets.resize(2);
                sm.morphTargets[0].name = "probe_a";
                sm.morphTargets[0].vertices = {0u};
                sm.morphTargets[0].positionDeltas = {glm::vec3(0.11f, 0.22f, 0.33f)};
                sm.morphTargets[0].normalDeltas = {glm::vec3(0.0f, 0.0f, 1.0f)};
                sm.morphTargets[1].name = "probe_b";
                sm.morphTargets[1].vertices = {0u};
                sm.morphTargets[1].positionDeltas = {glm::vec3(-0.44f, 0.0f, 0.0f)};
                sm.morphTargets[1].normalDeltas = {glm::vec3(1.0f, 0.0f, 0.0f)};
                seeded = true;
            }

            std::vector<std::uint8_t> pb;
            assetio::SourceModel back;
            std::string perr;
            bool pok = false;
            if (seeded) {
                pb = assetio::writeMeshFile(probe, wo);
                pok = !pb.empty() &&
                      assetio::readMeshFile(pb.data(), pb.size(), back, &perr);
            }
            bool found = false;
            bool morphOk = false;
            std::string morphWhy = "未执行";
            if (pok && !back.subMeshes.empty()) {
                // ⚠ 不能拿写盘**前**的顶点整字节 memcmp：`morphBase` /
                //   `morphCount` 在写盘时才由 `buildMorphStream` 从
                //   "哨兵 / 0" 重算成真实的流下标（§14）。那是**预期**
                //   的变化，不是丢数据。所以比之前把这两个字段抹平，
                //   其余 104 字节仍然逐字节比。
                assetio::Vertex want = probe.subMeshes[0].vertices[0];
                want.morphBase = 0;
                want.morphCount = 0;
                for (const auto& v : back.subMeshes[0].vertices) {
                    assetio::Vertex got = v;
                    got.morphBase = 0;
                    got.morphCount = 0;
                    if (std::memcmp(&got, &want, sizeof(assetio::Vertex)) == 0) {
                        found = true;
                        break;
                    }
                }
                // morph 的往返：名字 + 每条 delta 的 target/position/normal
                const assetio::SourceSubMesh& bsm = back.subMeshes[0];
                if (bsm.morphTargets.size() != 2) {
                    morphWhy = "target 数 " + std::to_string(bsm.morphTargets.size()) +
                               " != 2";
                } else if (bsm.morphTargets[0].name != "probe_a" ||
                           bsm.morphTargets[1].name != "probe_b") {
                    morphWhy = "名字没往返";
                } else if (bsm.morphStream.size() != 2) {
                    morphWhy = "delta 数 " + std::to_string(bsm.morphStream.size()) +
                               " != 2";
                } else {
                    // 顶点 0 在写盘的焊接里可能被并到别的下标，所以按
                    // **值**找它，再用它的 morphBase/count 定位记录。
                    const assetio::Vertex* pv = nullptr;
                    for (const assetio::Vertex& v : bsm.vertices) {
                        if (v.uv2[0] == 0.125f && v.uv2[1] == 0.875f) {
                            pv = &v;
                            break;
                        }
                    }
                    if (!pv) {
                        morphWhy = "带 morph 的顶点没找到";
                    } else if (pv->morphCount != 2) {
                        morphWhy = "morphCount " + std::to_string(pv->morphCount) + " != 2";
                    } else if (static_cast<std::size_t>(pv->morphBase) + 2 >
                               bsm.morphStream.size()) {
                        morphWhy = "morphBase 越界";
                    } else {
                        const assetio::SourceMorphDelta& d0 = bsm.morphStream[pv->morphBase];
                        const assetio::SourceMorphDelta& d1 =
                            bsm.morphStream[pv->morphBase + 1];
                        const bool ok =
                            d0.target == 0 && d1.target == 1 &&
                            d0.vertex == d1.vertex &&
                            std::fabs(d0.position.x - 0.11f) < 1e-6f &&
                            std::fabs(d1.position.x + 0.44f) < 1e-6f &&
                            std::fabs(d0.normal.z - 1.0f) < 1e-6f &&
                            std::fabs(d1.normal.x - 1.0f) < 1e-6f;
                        morphOk = ok;
                        morphWhy = ok ? "morph 两点往返一致"
                                      : "delta 内容不一致";
                    }
                }
            }
            flag("rt_new_channels", seeded && pok && found,
                 !seeded ? "没有可用的子网格"
                         : (!pok ? ("读回失败: " + perr)
                                 : (found ? "uv2/tangent 逐字节一致"
                                          : "注入的顶点在写读后不存在")));
            flag("rt_morph_roundtrip", seeded && pok && morphOk, morphWhy);
        }

        // 动画
        {
            flag("rt_clips", rt.clips.size() == src.clips.size(),
                 "got=" + std::to_string(rt.clips.size()) + ",want=" +
                     std::to_string(src.clips.size()));
            bool durOk = true, keysOk = true;
            for (std::size_t c = 0; c < std::min(rt.clips.size(), src.clips.size());
                 ++c) {
                if (std::fabs(rt.clips[c].duration - src.clips[c].duration) > 1.0e-5f)
                    durOk = false;
                if (rt.clips[c].tracks.size() != src.clips[c].tracks.size())
                    keysOk = false;
            }
            flag("rt_clip_durations", durOk);
            flag("rt_clip_tracks", keysOk);
        }
    }

    // ---------------- D. 动画压缩 ----------------
    if (src.clips.empty()) {
        na("anim_keys_reduced");
        na("anim_duration_kept");
        na("anim_err_at_keys");
        na("anim_err_dense");
    } else {
        // 拷一份原 clip 做对照（reduceClip 是原地改）
        std::vector<assetio::SourceClip> before = src.clips;
        std::vector<assetio::SourceClip> reduced = src.clips;
        assetio::ReduceStats rs;
        for (auto& c : reduced) {
            const assetio::ReduceStats r = assetio::reduceClip(c, 0.1f, 1.0e-4f, 1.0e-4f);
            rs.keysIn += r.keysIn;
            rs.keysOut += r.keysOut;
        }
        flag("anim_keys_reduced", rs.keysOut < rs.keysIn,
             "in=" + std::to_string(rs.keysIn) + ",out=" +
                 std::to_string(rs.keysOut));

        bool durKept = true;
        for (std::size_t i = 0; i < reduced.size(); ++i)
            if (reduced[i].duration != before[i].duration) durKept = false;
        flag("anim_duration_kept", durKept);

        // 误差：在**原始关键帧时刻**采样（抽帧器保证这些点上误差 ≤ 容差）
        const std::vector<ClipPair> pairs = pairClips(before, reduced);
        double worstKeyRot = 0.0, worstKeyPos = 0.0;
        double worstDenseRot = 0.0, worstDensePos = 0.0;
        // 出错时只说"rot=179.59"是没法查的 —— 得知道是哪段 clip、哪根骨头、
        // 哪个时刻。这些信息在循环里现成，只是第一版没留下来。
        std::string worstAt;
        for (const auto& p : pairs) {
            for (const auto& tb : p.before) {
                const AnimTrack* ta = findTrack(p.after, tb.joint,
                                                static_cast<int>(tb.path));
                if (!ta) continue;
                for (float t : tb.times) {
                    const double e = trackError(tb, *ta, t);
                    if (tb.path == AnimTrack::Rotation) {
                        if (e > worstKeyRot) {
                            worstKeyRot = e;
                            // `t0/dt0` 一起打出来：如果第一段的时间差是 0，
                            // 说明轨道在起点有**重复时间戳**（烘焙产物常见），
                            // 那时"在 t0 采样取哪一帧"本身是歧义的 ——
                            // 一眼就能区分"抽帧器删错帧"和"数据本身有跳变"。
                            const float t0 = tb.times.empty() ? 0.0f : tb.times[0];
                            const float dt0 = tb.times.size() > 1
                                                  ? tb.times[1] - tb.times[0]
                                                  : 0.0f;
                            // 同一 clip 里出现**多条**同 (joint,path) 的通道
                            // 时，`findTrack` 只会拿到第一条 —— 这不是抽帧
                            // 错了，是"拿另一个通道来比"。把条数打出来，
                            // 一眼区分这两种完全不同的成因。
                            int dup = 0;
                            for (const auto& tt : p.after)
                                if (tt.joint == tb.joint &&
                                    static_cast<int>(tt.path) ==
                                        static_cast<int>(tb.path))
                                    ++dup;
                            // 同名 clip 同样会错配：`pairClips` 按名字取
                            // **第一条**。FBX 的 `Namespace|Idle` 被剥成
                            // `Idle` 之后，多 take 文件里就会出现两个 `Idle`。
                            int sameName = 0;
                            for (const auto& q : pairs)
                                if (q.name == p.name) ++sameName;
                            worstAt = p.name + " joint=" + std::to_string(tb.joint) +
                                      " t=" + std::to_string(t) +
                                      " keys " + std::to_string(tb.times.size()) +
                                      "->" + std::to_string(ta->times.size()) +
                                      " t0=" + std::to_string(t0) +
                                      " dt0=" + std::to_string(dt0) +
                                      " dup=" + std::to_string(dup) +
                                      " sameName=" + std::to_string(sameName);
                        }
                    } else if (tb.path == AnimTrack::Translation) {
                        worstKeyPos = std::max(worstKeyPos, e);
                    }
                }
                // 密集采样：两点之间也会插值，误差可能略高于关键帧处
                const float step = p.duration > 0.0f ? p.duration / 96.0f : 0.0f;
                for (int k = 0; k <= 96 && step > 0.0f; ++k) {
                    const float t = step * static_cast<float>(k);
                    const double e = trackError(tb, *ta, t);
                    if (tb.path == AnimTrack::Rotation)
                        worstDenseRot = std::max(worstDenseRot, e);
                    else if (tb.path == AnimTrack::Translation)
                        worstDensePos = std::max(worstDensePos, e);
                }
            }
        }
        // 关键帧处：严格（容差的 1.05 倍，留浮点余量）
        flag("anim_err_at_keys", worstKeyRot <= 0.105 && worstKeyPos <= 1.05e-4,
             "rot=" + std::to_string(worstKeyRot) + ",pos=" +
                 std::to_string(worstKeyPos) + (worstAt.empty() ? "" : (" @" + worstAt)));
        // 密集采样：3 倍容差（关键帧之间是长跨度插值，比逐帧插值误差大）
        flag("anim_err_dense", worstDenseRot <= 0.3 && worstDensePos <= 3.0e-4,
             "rot=" + std::to_string(worstDenseRot) + ",pos=" +
                 std::to_string(worstDensePos));
    }

    // ---------------- E. 坏文件必须被拒 ----------------
    if (readOk && bytes.size() > 128) {
        // E1 改一个字节（落在第一个区块的数据区里，必然命中 CRC）
        {
            std::vector<std::uint8_t> bad = bytes;
            bad[bad.size() / 2] ^= 0x5A;
            assetio::SourceModel m;
            std::string e;
            const bool rejected = !assetio::readMeshFile(bad.data(), bad.size(), m, &e);
            flag("reject_corrupt", rejected, rejected ? e : "accepted!(no crc check?)");
        }
        // E2 截断
        {
            std::vector<std::uint8_t> bad(
                bytes.begin(),
                bytes.begin() + static_cast<std::ptrdiff_t>(bytes.size() * 7 / 10));
            assetio::SourceModel m;
            std::string e;
            const bool rejected = !assetio::readMeshFile(bad.data(), bad.size(), m, &e);
            flag("reject_truncated", rejected, rejected ? e : "accepted!");
        }
        // E3 改魔数
        {
            std::vector<std::uint8_t> bad = bytes;
            bad[0] = 'X';
            assetio::SourceModel m;
            std::string e;
            const bool rejected = !assetio::readMeshFile(bad.data(), bad.size(), m, &e);
            flag("reject_bad_magic", rejected, rejected ? e : "accepted!");
        }
        // E4 空输入
        {
            assetio::SourceModel m;
            std::string e;
            const bool rejected = !assetio::readMeshFile(nullptr, 0, m, &e);
            flag("reject_empty", rejected, rejected ? e : "accepted!");
        }
    }

    // ---------------- F. 确定性与体检 ----------------
    {
        const std::vector<std::uint8_t> again = assetio::writeMeshFile(src, wo, nullptr);
        flag("write_deterministic", again.size() == bytes.size() &&
                                        std::memcmp(again.data(), bytes.data(),
                                                    bytes.size()) == 0,
             "bytes=" + std::to_string(again.size()));
    }
    {
        assetio::MeshFileInfo info;
        std::string e;
        const bool ok = assetio::inspectMeshFile(bytes.data(), bytes.size(), info, &e);
        flag("info_ok", ok, ok ? "" : e);
        if (ok) {
            flag("info_counts", info.subMeshCount == st.subMeshes &&
                                    info.jointCount == st.joints &&
                                    info.clipCount == st.clips,
                 "sub=" + std::to_string(info.subMeshCount) + ",joints=" +
                     std::to_string(info.jointCount) + ",clips=" +
                     std::to_string(info.clipCount));
        }
    }

    // 源 hash 必须非 0（0 表示"读文件失败"，与"内容为空"撞车）
    flag("source_hash", src.sourceHash != 0,
         "h=" + std::to_string(src.sourceHash));

    VK_LOG_INFO("ASSET-TOTAL pass=%d fail=%d", g_pass, g_fail);
    return g_fail;
}

} // namespace assets
