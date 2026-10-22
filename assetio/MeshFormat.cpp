#include "assetio/MeshFormat.h"

#include "assetio/AnimOps.h"
#include "assetio/Binary.h"
#include "assetio/VertexOps.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace assetio {

namespace {

// ------------------------------------------------------------ 字符串表
//
// 为什么要单独一个区块而不是"每个名字就地写一遍"：
//   骨骼名在 joint 里出现 1 次、在动画轨道里却要按 joint 引用；
//   材质名在 submesh 里出现。重复写不但浪费，更要命的是**没法按名字
//   对齐**（比如 `.vmeta` 的材质映射表要按名字查）。
// 去重之后，同名一定是同一个下标，比较成本从 strcmp 降到比整数。
class StringTable {
public:
    std::uint32_t add(const std::string& s) {
        const auto it = m_map.find(s);
        if (it != m_map.end()) return it->second;
        const std::uint32_t idx = static_cast<std::uint32_t>(m_strs.size());
        m_strs.push_back(s);
        m_map.emplace(s, idx);
        return idx;
    }
    const std::vector<std::string>& all() const { return m_strs; }
    std::size_t size() const { return m_strs.size(); }

private:
    std::vector<std::string> m_strs;
    std::unordered_map<std::string, std::uint32_t> m_map;
};

constexpr std::uint32_t kNoString = 0xFFFFFFFFu;
constexpr std::size_t kFileHeaderSize = 32;
constexpr std::size_t kSectionHeaderSize = 32;

void writeMat4(Writer& w, const glm::mat4& m) {
    // glm::mat4 是**列主序**，直接按 [col][row] 顺序写 16 个 float。
    // 读端按同样顺序填回 —— 中间不做转置，否则往返一次模型就镜像了。
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) w.f32(m[c][r]);
}

glm::mat4 readMat4(Reader& r) {
    glm::mat4 m(1.0f);
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) m[c][row] = r.f32();
    return m;
}

} // namespace

const char* sectionName(std::uint32_t id) {
    switch (id) {
        case kSectionMesh: return "MESH";
        case kSectionSkeleton: return "SKEL";
        case kSectionAnimation: return "ANIM";
        case kSectionMaterial: return "MAT";
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
        meshW.u32(1);   // 区块版本
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
        matW.u32(1);
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

    Writer strW;
    {
        strW.u32(static_cast<std::uint32_t>(strs.size()));
        for (const std::string& s : strs.all()) strW.str(s);
    }

    // ---------- 3. 组装容器 ----------
    struct Sec {
        std::uint32_t id;
        std::vector<std::uint8_t> payload;
    };
    std::vector<Sec> secs;
    secs.push_back({kSectionMesh, meshW.take()});
    if (!model.joints.empty()) secs.push_back({kSectionSkeleton, skelW.take()});
    if (!model.clips.empty()) secs.push_back({kSectionAnimation, animW.take()});
    if (!model.materials.empty()) secs.push_back({kSectionMaterial, matW.take()});
    secs.push_back({kSectionString, strW.take()});

    std::uint32_t flags = 0;
    if (!model.joints.empty()) flags |= kFlagSkinned;
    if (!model.clips.empty()) flags |= kFlagAnimation;
    if (opt.weld || opt.optimizeCache) flags |= kFlagOptimized;
    if (all16) flags |= kFlagIndex16;

    Writer w;
    w.bytes(kMeshFileMagic, 8);
    w.u32(kMeshFileVersion);
    w.u32(flags);
    w.u64(model.sourceHash);
    w.u32(static_cast<std::uint32_t>(secs.size()));
    w.u32(0);   // reserved（将来可能放"编译器版本"）

    const std::size_t tableStart = w.size();
    for (std::size_t i = 0; i < secs.size(); ++i) {
        w.u32(secs[i].id);
        w.u32(1);          // 区块版本
        w.u32(0);          // crc（回填）
        w.u32(0);          // reserved
        w.u64(0);          // offset（回填）
        w.u64(0);          // size（回填）
    }

    for (std::size_t i = 0; i < secs.size(); ++i) {
        w.align(8);
        const std::uint64_t off = w.size();
        w.bytes(secs[i].payload.data(), secs[i].payload.size());
        const std::uint64_t sz = w.size() - off;
        const std::uint32_t crc =
            crc32(secs[i].payload.data(), secs[i].payload.size());
        const std::size_t e = tableStart + i * kSectionHeaderSize;
        w.patchU32(e + 8, crc);
        w.patchU64(e + 16, off);
        w.patchU64(e + 24, sz);
    }

    st.bytes = w.size();
    st.sections = secs.size();
    if (statsOut) *statsOut = st;
    return w.take();
}

// ============================================================
// 读
// ============================================================
namespace {

struct SecRef {
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::uint32_t crc = 0;
};

bool parseTable(const void* data, std::size_t size, MeshFileInfo& info,
                std::vector<std::pair<std::uint32_t, SecRef>>& out,
                std::string* err) {
    if (size < kFileHeaderSize) {
        if (err) *err = "file too small for header";
        return false;
    }
    Reader r(data, size);
    char magic[8] = {};
    if (!r.bytes(magic, 8)) {
        if (err) *err = "truncated magic";
        return false;
    }
    if (std::memcmp(magic, kMeshFileMagic, 8) != 0) {
        if (err) *err = "bad magic (not a .vmesh file)";
        return false;
    }
    info.version = r.u32();
    info.flags = r.u32();
    info.sourceHash = r.u64();
    info.sectionCount = r.u32();
    r.u32();   // reserved

    if (info.version != kMeshFileVersion) {
        if (err) {
            *err = "unsupported container version " +
                   std::to_string(info.version) + " (expect " +
                   std::to_string(kMeshFileVersion) + ")";
        }
        return false;
    }
    // 区块数上限：32 字节一个区块头，再多的文件一定是坏的（或恶意）。
    if (info.sectionCount == 0 || info.sectionCount > 64) {
        if (err) *err = "bad section count";
        return false;
    }
    const std::size_t need =
        kFileHeaderSize + static_cast<std::size_t>(info.sectionCount) * kSectionHeaderSize;
    if (need > size) {
        if (err) *err = "truncated section table";
        return false;
    }

    for (std::uint32_t i = 0; i < info.sectionCount; ++i) {
        const std::uint32_t id = r.u32();
        r.u32();   // 区块版本（当前都读 1；将来按需判）
        const std::uint32_t crc = r.u32();
        r.u32();   // reserved
        const std::uint64_t off = r.u64();
        const std::uint64_t len = r.u64();
        if (off > size || len > size - off) {
            if (err) {
                *err = std::string("section ") + sectionName(id) +
                       " out of bounds";
            }
            return false;
        }
        out.emplace_back(id, SecRef{off, len, crc});
    }
    info.bytes = size;
    return true;
}

std::string resolveString(const std::vector<std::string>& strs, std::uint32_t idx) {
    if (idx == kNoString || idx >= strs.size()) return std::string();
    return strs[idx];
}

} // namespace

bool inspectMeshFile(const void* data, std::size_t size, MeshFileInfo& info,
                     std::string* err) {
    info = MeshFileInfo{};
    std::vector<std::pair<std::uint32_t, SecRef>> secs;
    if (!parseTable(data, size, info, secs, err)) return false;

    info.index16 = (info.flags & kFlagIndex16) != 0;

    for (const auto& [id, ref] : secs) {
        Reader r(static_cast<const std::uint8_t*>(data) + ref.offset, ref.size);
        if (id == kSectionMesh) {
            r.u32();
            info.subMeshCount = r.u32();
            r.u32();   // stride
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
    for (const auto& [id, ref] : secs) {
        const std::uint32_t crc =
            crc32(static_cast<const std::uint8_t*>(data) + ref.offset,
                  static_cast<std::size_t>(ref.size));
        if (crc != ref.crc) {
            if (err) {
                char buf[128];
                std::snprintf(buf, sizeof(buf),
                              "section %s crc mismatch (file corrupt)",
                              sectionName(id));
                *err = buf;
            }
            return false;
        }
    }

    std::vector<std::string> strs;
    // STR 必须**先**解：其余区块里的名字都是它的下标。
    for (const auto& [id, ref] : secs) {
        if (id != kSectionString) continue;
        Reader r(static_cast<const std::uint8_t*>(data) + ref.offset, ref.size);
        const std::uint32_t n = r.u32();
        // 上限保护：一个字符串表条目至少 4 字节，n 超过剩余字节数就是坏文件
        if (n > ref.size) {
            if (err) *err = "STR: bad string count";
            return false;
        }
        strs.reserve(n);
        for (std::uint32_t i = 0; i < n && r.ok(); ++i) strs.push_back(r.str());
        if (!r.ok()) {
            if (err) *err = "STR: truncated";
            return false;
        }
    }

    for (const auto& [id, ref] : secs) {
        Reader r(static_cast<const std::uint8_t*>(data) + ref.offset, ref.size);

        if (id == kSectionMesh) {
            r.u32();   // 版本
            const std::uint32_t count = r.u32();
            const std::uint32_t stride = r.u32();
            const std::uint32_t idx16 = r.u32();
            if (stride != kVertexSize) {
                if (err) {
                    *err = "MESH: vertex stride " + std::to_string(stride) +
                           " != " + std::to_string(kVertexSize);
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
            r.u32();
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

    out.ok = true;
    return true;
}

} // namespace assetio
