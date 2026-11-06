#include "assetio/MeshFormat.h"

#include "assetio/AnimOps.h"
#include "assetio/Binary.h"
#include "assetio/ChunkFile.h"
#include "assetio/VertexOps.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace assetio {

namespace {
// 容器层（文件头 / 区块表 / CRC / 字符串表 / mat4）已搬到 `assetio/ChunkFile.h`
// —— `.vskel` 也要用同一份。这里只保留 `.vmesh` 自己的区块语义。
} // namespace

const char* sectionName(std::uint32_t id) {
    switch (id) {
        case kSectionMesh: return "MESH";
        case kSectionSkeleton: return "SKEL";
        case kSectionAnimation: return "ANIM";
        case kSectionMaterial: return "MAT";
        case kSectionMorph: return "MORPH";
        case kSectionSkeletonRef: return "SKELREF";
        case kSectionString: return "STR";
        default: return "?";
    }
}

std::string vmeshPathFor(const std::string& sourcePath) {
    const std::size_t dot = sourcePath.find_last_of('.');
    const std::size_t slash = sourcePath.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return sourcePath + ".vmesh";
    return sourcePath.substr(0, dot) + ".vmesh";
}

// ============================================================
// 写
// ============================================================
std::vector<std::uint8_t> writeMeshFile(SourceModel model,
                                        const WriteOptions& opt,
                                        WriteStats* statsOut) {
    WriteStats st;

    // ---------- 1. 几何修正 + 统计 ----------
    bool all16 = true;
    for (SourceSubMesh& sm : model.subMeshes) {
        const OptimizeStats os = optimizeSubMesh(sm, opt.weld, opt.optimizeCache);
        st.verticesIn += os.verticesIn;
        st.verticesOut += os.verticesOut;
        st.indicesIn += os.indicesIn;
        st.indicesOut += os.indicesOut;
        if (!os.index16) all16 = false;
        // ACMR 累加的是"每个子网格的比值"，最后除以子网格数取平均 ——
        // 不能把两边的顶点数/索引数各自相加再除，那样大子网格会
        // 把小网格的退化完全盖住。
        st.acmrIn += os.acmrIn;
        st.acmrOut += os.acmrOut;
        // ⚠ 这里必须用 `verticesIn` / `indicesIn`（**优化前**），不能用
        //   `sm.vertices.size()` —— optimizeSubMesh 是原地改的，读 sm 拿到
        //   的是已经焊过、重排过的那份。用它算"压缩比"会得到"1.1x"这种
        //   看起来很朴素、实际毫无意义的数字（真正的收益在顶点数和 ACMR 上，
        //   而这两条已经在上面单独统计了）。
        st.rawGeometryBytes += static_cast<std::uint32_t>(
            static_cast<std::size_t>(os.verticesIn) * sizeof(Vertex) +
            static_cast<std::size_t>(os.indicesIn) * 4);
    }
    if (!model.subMeshes.empty()) {
        st.acmrIn /= static_cast<float>(model.subMeshes.size());
        st.acmrOut /= static_cast<float>(model.subMeshes.size());
    }

    if (opt.reduceAnimation) {
        for (SourceClip& clip : model.clips) {
            const ReduceStats rs = reduceClip(clip, opt.rotationTolDeg,
                                             opt.positionTol, opt.scaleTol);
            st.keysIn += rs.keysIn;
            st.keysOut += rs.keysOut;
        }
    } else {
        for (const SourceClip& clip : model.clips) {
            for (const SourceTrack& t : clip.tracks) {
                st.keysIn += t.times.size();
                st.keysOut += t.times.size();
            }
        }
    }

    st.subMeshes = static_cast<std::uint32_t>(model.subMeshes.size());
    st.joints = static_cast<std::uint32_t>(model.joints.size());
    st.clips = static_cast<std::uint32_t>(model.clips.size());
    st.materials = static_cast<std::uint32_t>(model.materials.size());

    StringTable strs;

    // ---------- 2. 逐区块序列化 ----------
    Writer meshW;
    {
        meshW.u32(2);   // 区块版本（2 = 顶点 112 B，§13；1 = 80 B）
        meshW.u32(static_cast<std::uint32_t>(model.subMeshes.size()));
        meshW.u32(static_cast<std::uint32_t>(kVertexSize));
        meshW.u32(all16 ? 1u : 0u);
        // 目录（定长 112 字节/条），数据块跟在后面
        const std::size_t dirStart = meshW.size();
        for (std::size_t i = 0; i < model.subMeshes.size(); ++i) {
            meshW.u32(0);            // nameStr（下面回填）
            meshW.i32(0);            // material
            meshW.u32(0);            // vertexCount
            meshW.u32(0);            // indexCount
            meshW.u32(0);            // ofsVertices
            meshW.u32(0);            // ofsIndices
            for (int k = 0; k < 3; ++k) meshW.f32(0.0f);
            for (int k = 0; k < 3; ++k) meshW.f32(0.0f);
            for (int k = 0; k < 16; ++k) meshW.f32(0.0f);
        }

        constexpr std::size_t kDirEntry = 112;
        for (std::size_t i = 0; i < model.subMeshes.size(); ++i) {
            const SourceSubMesh& sm = model.subMeshes[i];
            const std::size_t e = dirStart + i * kDirEntry;

            meshW.align(8);
            const std::uint32_t ofsV = static_cast<std::uint32_t>(meshW.size());
            if (!sm.vertices.empty())
                meshW.bytes(sm.vertices.data(), sm.vertices.size() * sizeof(Vertex));

            meshW.align(8);
            const std::uint32_t ofsI = static_cast<std::uint32_t>(meshW.size());
            if (!sm.indices.empty()) {
                if (all16) {
                    for (std::uint32_t idx : sm.indices)
                        meshW.u16(static_cast<std::uint16_t>(idx));
                } else {
                    for (std::uint32_t idx : sm.indices) meshW.u32(idx);
                }
            }

            // 局部包围盒（模型级已有，但子网格级对阴影 / 拾取更有用）
            glm::vec3 lo(1e30f), hi(-1e30f);
            for (const Vertex& v : sm.vertices) {
                lo = glm::min(lo, glm::vec3(v.pos[0], v.pos[1], v.pos[2]));
                hi = glm::max(hi, glm::vec3(v.pos[0], v.pos[1], v.pos[2]));
            }
            if (sm.vertices.empty()) { lo = glm::vec3(0.0f); hi = glm::vec3(0.0f); }

            meshW.patchU32(e + 0, strs.add(sm.name));
            meshW.patchU32(e + 4, static_cast<std::uint32_t>(sm.material));
            meshW.patchU32(e + 8, static_cast<std::uint32_t>(sm.vertices.size()));
            meshW.patchU32(e + 12, static_cast<std::uint32_t>(sm.indices.size()));
            meshW.patchU32(e + 16, ofsV);
            meshW.patchU32(e + 20, ofsI);
            // 包围盒与变换：用 patchU32 写"float 的位模式"。
            // 为什么不先 w.f32 再回填：目录是**定长占位**的（数据块跟在
            // 后面），所以只能事后按偏移打补丁 —— 那就统一按位模式写，
            // 免得同一段代码里一半用 f32、一半用 patch，读起来更容易错。
            for (int k = 0; k < 3; ++k) {
                std::uint32_t bits = 0;
                std::memcpy(&bits, &lo[k], 4);
                meshW.patchU32(e + 24 + k * 4, bits);
                std::memcpy(&bits, &hi[k], 4);
                meshW.patchU32(e + 36 + k * 4, bits);
            }
            const std::size_t at = e + 48;
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r) {
                    std::uint32_t bits = 0;
                    std::memcpy(&bits, &sm.transform[c][r], 4);
                    meshW.patchU32(at + (c * 4 + r) * 4, bits);
                }
        }
    }

    Writer matW;
    {
        // 区块版本 2（2026-10-25）：末尾追加 alphaMode + alphaCutoff（P0-3）。
        // 只**追加**不插队 —— 读端按版本号决定要不要继续读，
        // 所以版本 1 的旧资产仍然能读（alphaMode 落回 Opaque，
        // 与改造前的行为逐像素一致，不需要重烤所有 .vmesh）。
        matW.u32(2);
        matW.u32(static_cast<std::uint32_t>(model.materials.size()));
        for (const SourceMaterial& m : model.materials) {
            matW.u32(strs.add(m.name));
            matW.u32(strs.add(m.albedoPath));
            matW.u32(strs.add(m.albedoRaw));
            matW.u32(strs.add(m.normalPath));
            matW.u32(strs.add(m.normalRaw));
            for (int k = 0; k < 4; ++k) matW.f32(m.baseColor[k]);
            matW.f32(m.roughness);
            matW.f32(m.metallic);
            for (int k = 0; k < 3; ++k) matW.f32(m.emissive[k]);
            matW.u32(m.doubleSided ? 1u : 0u);
            // ---- v2 追加 ----
            matW.u32(static_cast<std::uint32_t>(m.alphaMode));
            matW.f32(m.alphaCutoff);
        }
    }

    Writer skelW;
    {
        skelW.u32(1);
        skelW.u32(static_cast<std::uint32_t>(model.joints.size()));
        writeMat4(skelW, model.invMeshNode);
        for (const SourceJoint& j : model.joints) {
            skelW.u32(strs.add(j.name));
            skelW.i32(j.parent);
            writeMat4(skelW, j.restLocal);
            writeMat4(skelW, j.invBind);
            writeMat4(skelW, j.externalChain);
        }
        skelW.u32(static_cast<std::uint32_t>(model.topoOrder.size()));
        for (int idx : model.topoOrder) skelW.i32(idx);
    }

    Writer animW;
    {
        animW.u32(1);
        animW.u32(static_cast<std::uint32_t>(model.clips.size()));
        for (const SourceClip& c : model.clips) {
            animW.u32(strs.add(c.name));
            animW.u32(static_cast<std::uint32_t>(c.tracks.size()));
            animW.f32(c.duration);
            for (const SourceTrack& t : c.tracks) {
                animW.i32(t.joint);
                animW.u8(static_cast<std::uint8_t>(t.path & 0xFF));
                animW.u8(t.step ? 1u : 0u);
                animW.u16(0);
                const std::size_t n = std::min(t.times.size(), t.values.size());
                animW.u32(static_cast<std::uint32_t>(n));
                for (std::size_t k = 0; k < n; ++k) animW.f32(t.times[k]);
                for (std::size_t k = 0; k < n; ++k) {
                    const glm::vec4& v = t.values[k];
                    animW.f32(v.x);
                    animW.f32(v.y);
                    animW.f32(v.z);
                    animW.f32(v.w);
                }
            }
        }
    }

    // ---- MORPH 区块（§14）----
    //
    // 只有真的有 delta 才写。**不写空区块**是刻意的：区块表里的每一项
    // 都是一次"读者要跳过"的成本，而没有 morph 的资产占绝大多数（6 个
    // 资产里只有 1 个有）。空区块还会让 `--info` 的"有没有 morph"
    // 判断退化成"区块在不在且 deltaCount > 0"。
    Writer morphW;
    bool anyMorph = false;
    {
        std::uint32_t targetTotal = 0;
        std::size_t deltaTotal = 0;
        for (const SourceSubMesh& sm : model.subMeshes) {
            targetTotal += static_cast<std::uint32_t>(sm.morphTargets.size());
            deltaTotal += sm.morphStream.size();
        }
        st.morphTargets = targetTotal;
        st.morphDeltas = deltaTotal;
        for (const SourceSubMesh& sm : model.subMeshes)
            st.morphChannels +=
                static_cast<std::uint32_t>(sm.morphChannels.size());
        anyMorph = targetTotal > 0;

        if (anyMorph) {
            morphW.u32(2);   // 区块版本（§15：v2 = target 表 + channel 表）
            morphW.u32(static_cast<std::uint32_t>(model.subMeshes.size()));
            for (const SourceSubMesh& sm : model.subMeshes) {
                morphW.u32(static_cast<std::uint32_t>(sm.morphTargets.size()));
                morphW.u32(static_cast<std::uint32_t>(sm.morphStream.size()));
                for (const SourceMorphTarget& mt : sm.morphTargets)
                    morphW.u32(strs.add(mt.name));
                // delta 记录逐字节落盘（32 B/条，主机序 —— 与顶点同一约定）。
                // 布局与 GPU 侧 `GpuMorphDelta` 一致，加载时可以直接
                // memcpy 进 SSBO，运行期零转换。
                if (!sm.morphStream.empty()) {
                    morphW.bytes(sm.morphStream.data(),
                                 sm.morphStream.size() * sizeof(SourceMorphDelta));
                }
                // ---- channel 表（v2）----
                //
                // 与 target 表分开存，因为两者**不是**1:1（in-between shape
                // key 时一个滑条对应多个 target）。没有 channel 表的 v1 只能
                // 表达"一个滑条 = 一个 target"，这正是上一刀的模型。
                morphW.u32(static_cast<std::uint32_t>(sm.morphChannels.size()));
                for (const SourceMorphChannel& ch : sm.morphChannels) {
                    morphW.u32(strs.add(ch.name));
                    morphW.u32(static_cast<std::uint32_t>(ch.keys.size()));
                    for (const SourceMorphChannelKey& k : ch.keys) {
                        morphW.f32(k.targetWeight);
                        morphW.u32(k.target);
                    }
                }
            }
        }
    }

    // ---- SKELREF 区块（2026-11-06，独立骨架资产）----
    //
    // 只在真的引用了 `.vskel` 时才写。**不写空区块**：老资产（没重烤的）
    // 一个字节都不变，`sections=` 也还是 6 —— 回归脚本里的判据不会因为
    // "多了一条可选能力"集体变红。
    Writer skelRefW;
    const bool hasSkelRef = !model.skeletonPath.empty();
    if (hasSkelRef) skelRefW.u32(strs.add(model.skeletonPath));

    Writer strW;
    {
        strW.u32(static_cast<std::uint32_t>(strs.size()));
        for (const std::string& s : strs.all()) strW.str(s);
    }

    // ---------- 3. 组装容器 ----------
    std::vector<Chunk> secs;
    secs.push_back({kSectionMesh, 1, meshW.take()});
    if (!model.joints.empty()) secs.push_back({kSectionSkeleton, 1, skelW.take()});
    if (!model.clips.empty()) secs.push_back({kSectionAnimation, 1, animW.take()});
    if (!model.materials.empty()) secs.push_back({kSectionMaterial, 1, matW.take()});
    if (anyMorph) secs.push_back({kSectionMorph, 1, morphW.take()});
    if (hasSkelRef) secs.push_back({kSectionSkeletonRef, 1, skelRefW.take()});
    secs.push_back({kSectionString, 1, strW.take()});
    // 在 move 之前数：`--verbose` 与回归脚本会拿它断言"morph 区块确实
    // 写进去了"，事后重算一次等于把那行逻辑写两遍。
    const std::size_t chunkCount = secs.size();

    std::uint32_t flags = 0;
    if (!model.joints.empty()) flags |= kFlagSkinned;
    if (!model.clips.empty()) flags |= kFlagAnimation;
    if (opt.weld || opt.optimizeCache) flags |= kFlagOptimized;
    if (all16) flags |= kFlagIndex16;

    std::vector<std::uint8_t> out = writeChunkFile(
        kMeshFileMagic, kMeshFileVersion, flags, model.sourceHash, std::move(secs));

    st.bytes = out.size();
    st.sections = chunkCount;
    if (statsOut) *statsOut = st;
    return out;
}

// ============================================================
// 读
// ============================================================
namespace {

// 区块引用直接用容器层的 `ChunkEntry`（多了个 version 字段，无害）。
using SecRef = ChunkEntry;

// 把容器的表搬成 MeshFileInfo + (id → 引用) 列表。
//
// 文件头 / 区块表 / 边界检查 / 版本拒绝**全部**在
// `readChunkTable`（ChunkFile.cpp）里 —— 这里只做 `.vmesh` 特有的
// 字段搬运。这么拆的理由：`.vskel` 也要走同样的表，两份实现的差异
// 会从"排版不同"长成"一个能跳过未知区块、一个不能"。
bool parseTable(const void* data, std::size_t size, MeshFileInfo& info,
                std::vector<std::pair<std::uint32_t, SecRef>>& out,
                std::string* err) {
    ChunkFileHeader hdr;
    std::vector<ChunkEntry> entries;
    if (!readChunkTable(data, size, kMeshFileMagic, ".vmesh", kMeshFileVersion,
                        hdr, entries, &sectionName, err)) {
        return false;
    }
    info.version = hdr.version;
    info.flags = hdr.flags;
    info.sourceHash = hdr.sourceHash;
    info.sectionCount = hdr.chunkCount;
    info.bytes = hdr.bytes;
    out.reserve(entries.size());
    for (const ChunkEntry& e : entries) out.emplace_back(e.id, e);
    return true;
}

// STR 区块的解析（`readMeshFile` 与 `inspectMeshFile` 共用）。
//
// 为什么体检也要解它：`--info` 得能报出 `skeleton=models/.../alicia.vskel`。
// 没有这一行，"这个资产到底引用没引用独立骨架"就只能靠加载一次模型去猜 ——
// 而 `--info` 存在的意义正是**不加载**就把这件事查清楚。
bool readStringTable(const void* data,
                     const std::vector<std::pair<std::uint32_t, SecRef>>& secs,
                     std::vector<std::string>& out, std::string* err,
                     bool required) {
    for (const auto& [id, ref] : secs) {
        if (id != kSectionString) continue;
        Reader r(static_cast<const std::uint8_t*>(data) + ref.offset, ref.size);
        const std::uint32_t n = r.u32();
        // 上限保护：一个字符串表条目至少 4 字节，n 超过剩余字节数就是坏文件
        if (n > ref.size) {
            if (err) *err = "STR: bad string count";
            return false;
        }
        out.reserve(n);
        for (std::uint32_t i = 0; i < n && r.ok(); ++i) out.push_back(r.str());
        if (!r.ok()) {
            if (err) *err = "STR: truncated";
            return false;
        }
        return true;
    }
    if (required) {
        if (err) *err = "STR: missing section";
        return false;
    }
    return true;
}

} // namespace

bool inspectMeshFile(const void* data, std::size_t size, MeshFileInfo& info,
                     std::string* err) {
    info = MeshFileInfo{};
    std::vector<std::pair<std::uint32_t, SecRef>> secs;
    if (!parseTable(data, size, info, secs, err)) return false;

    info.index16 = (info.flags & kFlagIndex16) != 0;

    std::vector<std::string> strs;
    // 体检不因缺 STR 就失败（坏文件也应该能报结构），所以 required=false。
    if (!readStringTable(data, secs, strs, err, false)) return false;

    for (const auto& [id, ref] : secs) {
        Reader r(static_cast<const std::uint8_t*>(data) + ref.offset, ref.size);
        if (id == kSectionMesh) {
            info.meshVersion = r.u32();
            info.subMeshCount = r.u32();
            info.vertexStride = r.u32();
            r.u32();   // index16
            for (std::uint32_t i = 0; i < info.subMeshCount; ++i) {
                r.u32();   // nameStr
                r.i32();   // material
                const std::uint32_t vc = r.u32();
                const std::uint32_t ic = r.u32();
                info.vertexCount += vc;
                info.indexCount += ic;
                // 剩下：ofsV(4) + ofsI(4) + boundsMin(12) + boundsMax(12)
                //      + transform(64) = 96 字节。
                // ⚠ 这里必须和写端的 kDirEntry=112 **逐项对齐**：
                //   多跳或少跳 4 字节，读出来的就是下一条的名字/计数，
                //   症状是"顶点数是个天文数字"（已踩过一次）。
                r.skip(8 + 12 + 12 + 64);
            }
        } else if (id == kSectionSkeleton) {
            r.u32();
            info.jointCount = r.u32();
        } else if (id == kSectionAnimation) {
            r.u32();
            info.clipCount = r.u32();
        } else if (id == kSectionMaterial) {
            r.u32();
            info.materialCount = r.u32();
        } else if (id == kSectionMorph) {
            const std::uint32_t morphVer = r.u32();   // 区块版本（1 / 2）
            const std::uint32_t smc = r.u32();
            // 跨子网格累加。⚠ 这里和 MESH 区块的 subMeshCount 必须相等，
            //   校验放在 readMeshFile 里（这里只是体检报告，不负责拒绝）。
            for (std::uint32_t i = 0; i < smc; ++i) {
                const std::uint32_t tc = r.u32();
                const std::uint32_t dc = r.u32();
                info.morphTargetCount += tc;
                info.morphDeltaCount += dc;
                r.skip(static_cast<std::size_t>(tc) * 4 +   // 名字索引
                       static_cast<std::size_t>(dc) * sizeof(SourceMorphDelta));
                if (morphVer >= 2) {
                    const std::uint32_t cc = r.u32();
                    info.morphChannelCount += cc;
                    // 每条 key = targetWeight(4) + target(4)
                    for (std::uint32_t c = 0; c < cc; ++c) {
                        r.u32();               // channel 名索引
                        const std::uint32_t kc = r.u32();
                        r.skip(static_cast<std::size_t>(kc) * 8);
                    }
                } else {
                    // v1 没有 channel 表：channel 与 target 天然 1:1。
                    info.morphChannelCount += tc;
                }
            }
        } else if (id == kSectionSkeletonRef) {
            info.skeletonPath = resolveString(strs, r.u32());
        }
    }
    return true;
}

bool readMeshFile(const void* data, std::size_t size, SourceModel& out,
                  std::string* err) {
    MeshFileInfo info;
    std::vector<std::pair<std::uint32_t, SecRef>> secs;
    if (!parseTable(data, size, info, secs, err)) return false;

    out = SourceModel{};
    out.sourceHash = info.sourceHash;
    out.ok = false;

    // ---- 区块完整性校验 ----
    //
    // 这一步必须在解析之前做。理由：解析是"按长度字段往前走"的过程，
    // 一个被截断/改写过的长度字段会让解析器读到垃圾，然后在很远的地方
    // 报一个和真实原因毫无关系的错（"bad track count" 之类）。
    // CRC 先过一遍，错误信息就能精确到区块。
    //
    // ⚠ 只校验 CRC 不算完：CRC 只能说明"字节没坏"，不能说明"内容合法"
    //   （有人拿 v1 的写端配 v2 的读端，CRC 一样是对的）。所以下面每个
    //   区块仍然要逐字段做范围检查。
    {
        std::vector<ChunkEntry> entries;
        entries.reserve(secs.size());
        for (const auto& [id, ref] : secs) entries.push_back(ref);
        if (!verifyChunkCrc(data, size, entries, &sectionName, err)) return false;
    }

    std::vector<std::string> strs;
    // STR 必须**先**解：其余区块里的名字都是它的下标。
    if (!readStringTable(data, secs, strs, err, true)) return false;

    // MORPH 区块先暂存、循环结束后再合并到 subMeshes 上。
    // 不直接按顺序写进去的理由：区块顺序是**写端**的实现细节，
    // 读端不该依赖"MORPH 一定在 MESH 之后"。暂存之后合并，顺序就无关了。
    struct MorphBlock {
        std::vector<std::string> names;
        std::vector<SourceMorphDelta> deltas;
        std::vector<SourceMorphChannel> channels;
    };
    std::vector<MorphBlock> morphBlocks;

    for (const auto& [id, ref] : secs) {
        Reader r(static_cast<const std::uint8_t*>(data) + ref.offset, ref.size);

        if (id == kSectionMesh) {
            const std::uint32_t meshSecVersion = r.u32();
            const std::uint32_t count = r.u32();
            const std::uint32_t stride = r.u32();
            const std::uint32_t idx16 = r.u32();
            if (stride != kVertexSize) {
                // 这里给出"怎么修"而不是只说"不对"。老格式（80 B）的文件
                // 在 §13 之后是**必然**会被拒的，报错里的人话能省下
                // "为什么昨天还能加载"的那半小时。
                if (err) {
                    *err = "MESH: vertex stride " + std::to_string(stride) +
                           " != " + std::to_string(kVertexSize) + "（区块 v" +
                           std::to_string(meshSecVersion) +
                           "；此文件是旧顶点格式，用 AssetC 重烤一次即可）";
                }
                return false;
            }
            if (count > 65536) {
                if (err) *err = "MESH: absurd submesh count";
                return false;
            }
            struct Dir {
                std::uint32_t nameStr, material, vc, ic, ofsV, ofsI;
                float lo[3], hi[3];
                float m[16];
            };
            std::vector<Dir> dir(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                Dir& d = dir[i];
                d.nameStr = r.u32();
                d.material = r.u32();
                d.vc = r.u32();
                d.ic = r.u32();
                d.ofsV = r.u32();
                d.ofsI = r.u32();
                for (int k = 0; k < 3; ++k) d.lo[k] = r.f32();
                for (int k = 0; k < 3; ++k) d.hi[k] = r.f32();
                for (int k = 0; k < 16; ++k) d.m[k] = r.f32();
            }
            if (!r.ok()) {
                if (err) *err = "MESH: truncated directory";
                return false;
            }

            out.subMeshes.resize(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                const Dir& d = dir[i];
                SourceSubMesh& sm = out.subMeshes[i];
                sm.name = resolveString(strs, d.nameStr);
                sm.material = static_cast<int>(d.material);

                // 长度字段来自文件，先跟区块长度对账 —— 一个坏掉的
                // vertexCount 就是一次 8GB 的 resize。
                const std::uint64_t vbytes =
                    static_cast<std::uint64_t>(d.vc) * sizeof(Vertex);
                const std::uint64_t ibytes =
                    static_cast<std::uint64_t>(d.ic) * (idx16 ? 2 : 4);
                if (static_cast<std::uint64_t>(d.ofsV) + vbytes > ref.size ||
                    static_cast<std::uint64_t>(d.ofsI) + ibytes > ref.size) {
                    if (err) *err = "MESH: submesh payload out of bounds";
                    return false;
                }

                sm.vertices.resize(d.vc);
                if (d.vc) {
                    Reader vr(static_cast<const std::uint8_t*>(data) + ref.offset + d.ofsV,
                              vbytes);
                    if (!vr.bytes(sm.vertices.data(), static_cast<std::size_t>(vbytes))) {
                        if (err) *err = "MESH: truncated vertex data";
                        return false;
                    }
                }
                sm.indices.resize(d.ic);
                if (d.ic) {
                    Reader ir(static_cast<const std::uint8_t*>(data) + ref.offset + d.ofsI,
                              ibytes);
                    if (idx16) {
                        for (std::uint32_t k = 0; k < d.ic; ++k) sm.indices[k] = ir.u16();
                    } else {
                        for (std::uint32_t k = 0; k < d.ic; ++k) sm.indices[k] = ir.u32();
                    }
                    if (!ir.ok()) {
                        if (err) *err = "MESH: truncated index data";
                        return false;
                    }
                }
                for (int c = 0; c < 4; ++c)
                    for (int row = 0; row < 4; ++row)
                        sm.transform[c][row] = d.m[c * 4 + row];
            }
        } else if (id == kSectionMorph) {
            const std::uint32_t morphSecVersion = r.u32();   // 1 或 2（§15）
            const std::uint32_t smc = r.u32();
            if (smc > 65536) {
                if (err) *err = "MORPH: absurd submesh count";
                return false;
            }
            if (morphSecVersion < 1 || morphSecVersion > 2) {
                if (err) {
                    *err = "MORPH: unsupported section version " +
                           std::to_string(morphSecVersion) +
                           "（用 AssetC 重烤一次）";
                }
                return false;
            }
            morphBlocks.resize(smc);
            for (std::uint32_t i = 0; i < smc; ++i) {
                MorphBlock& mb = morphBlocks[i];
                const std::uint32_t tc = r.u32();
                const std::uint32_t dc = r.u32();
                // 两条长度都要先跟区块长度对账：`dc` 来自文件，
                // 直接 resize 一个坏值就是一次几个 GB 的分配。
                if (tc > 65536 || static_cast<std::uint64_t>(dc) > ref.size / 32) {
                    if (err) *err = "MORPH: absurd target/delta count";
                    return false;
                }
                mb.names.resize(tc);
                for (std::uint32_t k = 0; k < tc; ++k)
                    mb.names[k] = resolveString(strs, r.u32());
                mb.deltas.resize(dc);
                if (dc) {
                    const std::uint64_t bytes =
                        static_cast<std::uint64_t>(dc) * sizeof(SourceMorphDelta);
                    if (!r.bytes(mb.deltas.data(), static_cast<std::size_t>(bytes))) {
                        if (err) *err = "MORPH: truncated delta stream";
                        return false;
                    }
                }
                // ---- v2：channel 表 ----
                // v1 没有这一段（那时 channel 与 target 强制 1:1）。读 v1 时
                // 用 target 名合成一份等价表 —— 这样"老文件也能加载"，
                // 而不是逼着重烤（重烤仍然是推荐的，v1 的 offset_weights
                // 已经被丢掉了，那部分信息不可恢复）。
                if (morphSecVersion >= 2) {
                    const std::uint32_t cc = r.u32();
                    if (cc > 65536) {
                        if (err) *err = "MORPH: absurd channel count";
                        return false;
                    }
                    mb.channels.resize(cc);
                    for (std::uint32_t c = 0; c < cc; ++c) {
                        SourceMorphChannel& ch = mb.channels[c];
                        ch.name = resolveString(strs, r.u32());
                        const std::uint32_t kc = r.u32();
                        if (kc > 65536) {
                            if (err) *err = "MORPH: absurd key count";
                            return false;
                        }
                        ch.keys.resize(kc);
                        for (std::uint32_t k = 0; k < kc; ++k) {
                            ch.keys[k].targetWeight = r.f32();
                            ch.keys[k].target = r.u32();
                        }
                    }
                } else {
                    mb.channels.resize(tc);
                    for (std::uint32_t t = 0; t < tc; ++t) {
                        mb.channels[t].name = mb.names[t];
                        mb.channels[t].keys.push_back({1.0f, t});
                    }
                }
            }
            if (!r.ok()) {
                if (err) *err = "MORPH: truncated";
                return false;
            }
        } else if (id == kSectionSkeleton) {
            r.u32();
            const std::uint32_t n = r.u32();
            if (n > 4096) {
                if (err) *err = "SKEL: absurd joint count";
                return false;
            }
            out.invMeshNode = readMat4(r);
            out.joints.resize(n);
            for (std::uint32_t j = 0; j < n; ++j) {
                SourceJoint& jt = out.joints[j];
                jt.name = resolveString(strs, r.u32());
                jt.parent = r.i32();
                jt.restLocal = readMat4(r);
                jt.invBind = readMat4(r);
                jt.externalChain = readMat4(r);
            }
            const std::uint32_t topo = r.u32();
            if (topo > 4096) {
                if (err) *err = "SKEL: absurd topo count";
                return false;
            }
            out.topoOrder.resize(topo);
            for (std::uint32_t k = 0; k < topo; ++k) out.topoOrder[k] = r.i32();
            if (!r.ok()) {
                if (err) *err = "SKEL: truncated";
                return false;
            }
        } else if (id == kSectionMaterial) {
            const std::uint32_t matVersion = r.u32();
            const std::uint32_t n = r.u32();
            if (n > 65536) {
                if (err) *err = "MAT: absurd material count";
                return false;
            }
            out.materials.resize(n);
            for (std::uint32_t i = 0; i < n; ++i) {
                SourceMaterial& m = out.materials[i];
                m.name = resolveString(strs, r.u32());
                m.albedoPath = resolveString(strs, r.u32());
                m.albedoRaw = resolveString(strs, r.u32());
                m.normalPath = resolveString(strs, r.u32());
                m.normalRaw = resolveString(strs, r.u32());
                for (int k = 0; k < 4; ++k) m.baseColor[k] = r.f32();
                m.roughness = r.f32();
                m.metallic = r.f32();
                for (int k = 0; k < 3; ++k) m.emissive[k] = r.f32();
                m.doubleSided = r.u32() != 0;
                // v2 追加（P0-3）：版本 1 的资产读到这里就结束 ——
                // alphaMode 保持默认 Opaque，与改造前逐像素一致。
                if (matVersion >= 2) {
                    const std::uint32_t am = r.u32();
                    m.alphaMode = (am <= 2u)
                                      ? static_cast<SourceMaterial::AlphaMode>(am)
                                      : SourceMaterial::AlphaMode::Opaque;
                    m.alphaCutoff = r.f32();
                }
            }
            if (!r.ok()) {
                if (err) *err = "MAT: truncated";
                return false;
            }
        } else if (id == kSectionAnimation) {
            r.u32();
            const std::uint32_t n = r.u32();
            if (n > 65536) {
                if (err) *err = "ANIM: absurd clip count";
                return false;
            }
            out.clips.resize(n);
            for (std::uint32_t c = 0; c < n; ++c) {
                SourceClip& clip = out.clips[c];
                clip.name = resolveString(strs, r.u32());
                const std::uint32_t tracks = r.u32();
                clip.duration = r.f32();
                if (tracks > 65536) {
                    if (err) *err = "ANIM: absurd track count";
                    return false;
                }
                clip.tracks.resize(tracks);
                for (std::uint32_t t = 0; t < tracks; ++t) {
                    SourceTrack& tr = clip.tracks[t];
                    tr.joint = r.i32();
                    tr.path = r.u8();
                    tr.step = r.u8() != 0;
                    r.u16();
                    const std::uint32_t keys = r.u32();
                    // 每个 key 至少 5 个 float（时间 + 4 分量）
                    if (static_cast<std::uint64_t>(keys) * 20 > r.size()) {
                        if (err) *err = "ANIM: absurd key count";
                        return false;
                    }
                    tr.times.resize(keys);
                    tr.values.resize(keys);
                    for (std::uint32_t k = 0; k < keys; ++k) tr.times[k] = r.f32();
                    for (std::uint32_t k = 0; k < keys; ++k) {
                        tr.values[k].x = r.f32();
                        tr.values[k].y = r.f32();
                        tr.values[k].z = r.f32();
                        tr.values[k].w = r.f32();
                    }
                }
            }
            if (!r.ok()) {
                if (err) *err = "ANIM: truncated";
                return false;
            }
        } else if (id == kSectionSkeletonRef) {
            // 独立骨架资产的引用。路径为空 = 写了区块但内容坏，
            // 明确报出来比"静默退回内嵌"好 —— 后者会让"我明明链接了
            // 独立骨架，怎么骨架面板里还是内嵌那份"变成一个谜。
            out.skeletonPath = resolveString(strs, r.u32());
            if (out.skeletonPath.empty()) {
                if (err) *err = "SKELREF: empty skeleton path";
                return false;
            }
        }
    }

    // 包围盒：从子网格的局部 AABB × transform 重新算一遍。
    // 不落盘（能省 24 字节，但更重要的是"由几何推导的东西不要存两份"，
    // 两份就可能不一致）。
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (const SourceSubMesh& sm : out.subMeshes) {
        if (sm.vertices.empty()) continue;
        glm::vec3 localLo(1e30f), localHi(-1e30f);
        for (const Vertex& v : sm.vertices) {
            localLo = glm::min(localLo, glm::vec3(v.pos[0], v.pos[1], v.pos[2]));
            localHi = glm::max(localHi, glm::vec3(v.pos[0], v.pos[1], v.pos[2]));
        }
        for (int c = 0; c < 8; ++c) {
            const glm::vec3 p((c & 1) ? localHi.x : localLo.x,
                              (c & 2) ? localHi.y : localLo.y,
                              (c & 4) ? localHi.z : localLo.z);
            const glm::vec3 w = glm::vec3(sm.transform * glm::vec4(p, 1.0f));
            lo = glm::min(lo, w);
            hi = glm::max(hi, w);
        }
    }
    if (lo.x <= hi.x) {
        out.boundsMin = lo;
        out.boundsMax = hi;
        out.radius = std::max(glm::length(hi - lo) * 0.5f, 0.01f);
    } else {
        out.radius = 1.0f;
    }

    // ---- MORPH 区块合并 + 自洽校验 ----
    //
    // 这一段的校验不是"防御坏文件"，是**防止自己的写端出错**：
    // 顶点里的 `morphBase` 是 delta 流的下标，两者一旦错位，
    // 几何、法线、UV、骨骼全都正确，只有做表情时脸会歪 ——
    // 那是所有已知症状里最难定位的一种。所以在加载路径上硬断言。
    if (!morphBlocks.empty()) {
        if (morphBlocks.size() != out.subMeshes.size()) {
            if (err) {
                *err = "MORPH: submesh count " + std::to_string(morphBlocks.size()) +
                       " != MESH's " + std::to_string(out.subMeshes.size());
            }
            return false;
        }
        for (std::size_t i = 0; i < morphBlocks.size(); ++i) {
            MorphBlock& mb = morphBlocks[i];
            SourceSubMesh& sm = out.subMeshes[i];
            sm.morphTargets.resize(mb.names.size());
            for (std::size_t t = 0; t < mb.names.size(); ++t)
                sm.morphTargets[t].name = mb.names[t];
            for (const SourceMorphDelta& d : mb.deltas) {
                if (d.target >= mb.names.size() || d.vertex >= sm.vertices.size()) {
                    if (err) *err = "MORPH: delta refers to out-of-range target/vertex";
                    return false;
                }
                SourceMorphTarget& mt = sm.morphTargets[d.target];
                mt.vertices.push_back(d.vertex);
                mt.positionDeltas.push_back(d.position);
                mt.normalDeltas.push_back(d.normal);
            }
            sm.morphStream = std::move(mb.deltas);
            // ---- channel 表（§15）----
            // 校验只有两条，都是"读端与写端必须同意"的硬约束：
            //   · key.target 要么是 kNoMorphTarget，要么落在 target 表内
            //   · targetWeight ∈ [0,1]（分段插值按它排序/切段）
            sm.morphChannels = std::move(mb.channels);
            for (std::size_t c = 0; c < sm.morphChannels.size(); ++c) {
                SourceMorphChannel& ch = sm.morphChannels[c];
                if (ch.keys.empty()) {
                    if (err) {
                        *err = "MORPH: channel " + std::to_string(c) +
                               " has no keys";
                    }
                    return false;
                }
                for (const SourceMorphChannelKey& k : ch.keys) {
                    if (k.target != kNoMorphTarget &&
                        k.target >= sm.morphTargets.size()) {
                        if (err) {
                            *err = "MORPH: channel " + std::to_string(c) +
                                   " key refers to out-of-range target " +
                                   std::to_string(k.target);
                        }
                        return false;
                    }
                    if (!(k.targetWeight >= 0.0f && k.targetWeight <= 1.0f)) {
                        if (err) {
                            *err = "MORPH: channel " + std::to_string(c) +
                                   " key targetWeight out of [0,1]";
                        }
                        return false;
                    }
                }
            }

            // 顶点 ↔ 流的双向账目
            std::uint32_t cursor = 0;
            for (std::size_t v = 0; v < sm.vertices.size(); ++v) {
                const Vertex& vv = sm.vertices[v];
                if (vv.morphCount == 0) {
                    if (vv.morphBase != kNoMorphBase) {
                        if (err) {
                            *err = "MORPH: vertex " + std::to_string(v) +
                                   " has count 0 but a non-sentinel base";
                        }
                        return false;
                    }
                    continue;
                }
                if (vv.morphBase != cursor) {
                    if (err) {
                        *err = "MORPH: vertex " + std::to_string(v) + " base " +
                               std::to_string(vv.morphBase) + " != expected " +
                               std::to_string(cursor) + "（delta 流与顶点顺序不一致）";
                    }
                    return false;
                }
                if (static_cast<std::size_t>(vv.morphBase) + vv.morphCount >
                    sm.morphStream.size()) {
                    if (err) *err = "MORPH: vertex delta range out of bounds";
                    return false;
                }
                for (std::uint32_t k = 0; k < vv.morphCount; ++k) {
                    if (sm.morphStream[vv.morphBase + k].vertex != v) {
                        if (err) {
                            *err = "MORPH: delta stream vertex mismatch at slot " +
                                   std::to_string(vv.morphBase + k);
                        }
                        return false;
                    }
                }
                cursor += vv.morphCount;
            }
            if (cursor != sm.morphStream.size()) {
                if (err) *err = "MORPH: orphan delta records (stream longer than vertices)";
                return false;
            }
        }
    } else {
        // 没有 MORPH 区块时，顶点上不许出现非哨兵的 morphBase ——
        // 那说明写着"我有形变"但数据没了。
        for (const SourceSubMesh& sm : out.subMeshes) {
            for (std::size_t v = 0; v < sm.vertices.size(); ++v) {
                if (sm.vertices[v].morphBase != kNoMorphBase ||
                    sm.vertices[v].morphCount != 0) {
                    if (err) {
                        *err = "MESH: vertex " + std::to_string(v) +
                               " references morph data but MORPH section is missing";
                    }
                    return false;
                }
            }
        }
    }

    out.ok = true;
    return true;
}

} // namespace assetio