#include "assetio/SkelFormat.h"

#include "assetio/Binary.h"
#include "assetio/ChunkFile.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace assetio {

namespace {

std::string asciiLower(const std::string& s) {
    std::string o = s;
    for (char& c : o)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return o;
}

// 按候选名找一根骨（**大小写不敏感**，精确匹配）。
//
// 为什么不做"包含匹配"：挂点挂错骨比挂不上更糟。"weapon_r" 按包含去找
// 会先命中 `hand_r`，但也可能命中 `ik_hand_gun_child_r` 这类辅助骨 ——
// 那种错误在画面上表现成"枪的位置偏了一点"，几乎不可能反推到名字解析。
int resolveBone(const std::vector<SourceJoint>& joints, const char* const* cands,
                int n) {
    for (int c = 0; c < n; ++c) {
        const std::string want = asciiLower(cands[c]);
        for (std::size_t i = 0; i < joints.size(); ++i)
            if (asciiLower(joints[i].name) == want) return static_cast<int>(i);
    }
    return -1;
}

// 挂点名的候选骨。顺序 = 优先级。
const char* const kHandRCands[] = {"hand_r", "righthand", "right_hand",
                                   "bip01_r_hand"};
const char* const kHandLCands[] = {"hand_l", "lefthand", "left_hand",
                                   "bip01_l_hand"};
const char* const kHeadCands[] = {"head", "bip01_head"};
constexpr int kHandRCandCount = 4;
constexpr int kHandLCandCount = 4;
constexpr int kHeadCandCount = 2;

} // namespace

// ============================================================
// 摘要 / 查找
// ============================================================
std::uint64_t skeletonBoneDigest(const std::vector<SourceJoint>& joints) {
    // 逐根把 (名字, 父下标) 喂进 FNV-1a。名字后面补一个 '\0' 当分隔符，
    // 否则 ["ab","c"] 和 ["a","bc"] 会算出同一个摘要（拼接歧义）——
    // 而摘要的全部用途就是"判两份骨表是不是同一份"，撞了就白算了。
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (const SourceJoint& j : joints) {
        h = fnv1a64(j.name.data(), j.name.size(), h);
        const char sep = '\0';
        h = fnv1a64(&sep, 1, h);
        const std::int32_t p = j.parent;
        h = fnv1a64(&p, sizeof(p), h);
    }
    return h;
}

int SourceSkeletonAsset::findJoint(const std::string& name) const {
    const std::string want = asciiLower(name);
    for (std::size_t i = 0; i < joints.size(); ++i)
        if (asciiLower(joints[i].name) == want) return static_cast<int>(i);
    return -1;
}

int SourceSkeletonAsset::findSocket(const std::string& name) const {
    const std::string want = asciiLower(name);
    for (std::size_t i = 0; i < sockets.size(); ++i)
        if (asciiLower(sockets[i].name) == want) return static_cast<int>(i);
    return -1;
}

// ============================================================
// 默认挂点
// ============================================================
void applyDefaultSockets(const std::vector<SourceJoint>& joints,
                         std::vector<SourceSocket>& out) {
    out.clear();
    struct Want {
        const char* socket;
        const char* const* cands;
        int count;
    };
    const Want wants[] = {
        {"weapon_r", kHandRCands, kHandRCandCount},
        {"weapon_l", kHandLCands, kHandLCandCount},
        {"eye", kHeadCands, kHeadCandCount},
    };
    for (const Want& w : wants) {
        const int bone = resolveBone(joints, w.cands, w.count);
        if (bone < 0) continue;   // 找不到就不建（见头文件里的理由）
        SourceSocket s;
        s.name = w.socket;
        s.boneName = joints[static_cast<std::size_t>(bone)].name;
        s.local = glm::mat4(1.0f);
        out.push_back(std::move(s));
    }
}

// ============================================================
// 写
// ============================================================
std::vector<std::uint8_t> writeSkelFile(const SourceSkeletonAsset& skel) {
    StringTable strs;

    // ---- BONES ----
    Writer bonesW;
    {
        bonesW.u32(static_cast<std::uint32_t>(skel.joints.size()));
        bonesW.u64(skel.boneDigest != 0 ? skel.boneDigest
                                        : skeletonBoneDigest(skel.joints));
        writeMat4(bonesW, skel.invMeshNode);
        writeMat4(bonesW, skel.skeletonToEngine);
        for (const SourceJoint& j : skel.joints) {
            bonesW.u32(strs.add(j.name));
            bonesW.i32(j.parent);
            writeMat4(bonesW, j.restLocal);
            writeMat4(bonesW, j.invBind);
            writeMat4(bonesW, j.externalChain);
        }
        bonesW.u32(static_cast<std::uint32_t>(skel.topoOrder.size()));
        for (int idx : skel.topoOrder) bonesW.i32(idx);
    }

    // ---- SOCKET（可选）----
    //
    // 没有挂点就**不写这个区块**：区块表里每一项都是一次"读者要跳过"的
    // 成本，而且空区块会让 `--skel-info` 的"有没有挂点"退化成
    // "区块在不在并且 count > 0"。
    Writer sockW;
    const bool hasSockets = !skel.sockets.empty();
    if (hasSockets) {
        sockW.u32(static_cast<std::uint32_t>(skel.sockets.size()));
        for (const SourceSocket& s : skel.sockets) {
            sockW.u32(strs.add(s.name));
            sockW.u32(strs.add(s.boneName));
            writeMat4(sockW, s.local);
        }
    }

    // ---- STR（必须最后组装：上面的 strs.add 都发生完了）----
    Writer strW;
    {
        strW.u32(static_cast<std::uint32_t>(strs.size()));
        for (const std::string& s : strs.all()) strW.str(s);
    }

    std::uint32_t flags = 0;
    if (hasSockets) flags |= kSkelFlagSockets;
    // "是不是单位阵"要判，而不是拿一个 bool 传进来：
    // `skeletonToEngine` 是 glTF→Z-up / 导入缩放那条链的产物，忘了写进
    // 文件的后果是"根运动方向反了 90°"，而那种 bug 只在跑起来才看得见。
    if (skel.skeletonToEngine != glm::mat4(1.0f)) flags |= kSkelFlagEngineXform;

    std::vector<Chunk> chunks;
    chunks.push_back({kSkelChunkBones, 1, bonesW.take()});
    if (hasSockets) chunks.push_back({kSkelChunkSocket, 1, sockW.take()});
    chunks.push_back({kSkelChunkString, 1, strW.take()});

    return writeChunkFile(kSkelFileMagic, kSkelFileVersion, flags, skel.sourceHash,
                          std::move(chunks));
}

// ============================================================
// 读
// ============================================================
namespace {

struct Table {
    ChunkFileHeader hdr;
    std::vector<ChunkEntry> entries;
};

bool readTable(const void* data, std::size_t size, Table& t, std::string* err,
               bool verifyCrc) {
    if (!readChunkTable(data, size, kSkelFileMagic, ".vskel", kSkelFileVersion,
                        t.hdr, t.entries, &skelSectionName, err)) {
        return false;
    }
    if (verifyCrc &&
        !verifyChunkCrc(data, size, t.entries, &skelSectionName, err)) {
        return false;
    }
    return true;
}

// STR 必须**先**解：其余区块里的名字都是它的下标。
bool readStrings(const void* data, const Table& t, std::vector<std::string>& out,
                 std::string* err) {
    for (const ChunkEntry& e : t.entries) {
        if (e.id != kSkelChunkString) continue;
        Reader r(static_cast<const std::uint8_t*>(data) + e.offset,
                 static_cast<std::size_t>(e.size));
        const std::uint32_t n = r.u32();
        // 一个字符串表条目至少 4 字节，n 超过剩余字节数就是坏文件
        if (n > e.size) {
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
    if (err) *err = "STR: missing section";
    return false;
}

} // namespace

bool readSkelFile(const void* data, std::size_t size, SourceSkeletonAsset& out,
                  std::string* err) {
    out = SourceSkeletonAsset{};

    Table t;
    if (!readTable(data, size, t, err, true)) return false;

    std::vector<std::string> strs;
    if (!readStrings(data, t, strs, err)) return false;

    bool sawBones = false;
    for (const ChunkEntry& e : t.entries) {
        Reader r(static_cast<const std::uint8_t*>(data) + e.offset,
                 static_cast<std::size_t>(e.size));

        if (e.id == kSkelChunkBones) {
            const std::uint32_t n = r.u32();
            if (n > 4096) {
                if (err) *err = "BONES: absurd joint count";
                return false;
            }
            out.boneDigest = r.u64();
            out.invMeshNode = readMat4(r);
            out.skeletonToEngine = readMat4(r);
            out.joints.resize(n);
            for (std::uint32_t j = 0; j < n; ++j) {
                SourceJoint& jt = out.joints[j];
                jt.name = resolveString(strs, r.u32());
                jt.parent = r.i32();
                jt.restLocal = readMat4(r);
                jt.invBind = readMat4(r);
                jt.externalChain = readMat4(r);
                // 父下标必须落在表内或 -1。不查的后果是 `samplePose` 拿着
                // 一个越界下标去读 joints[parent] —— 段错误的现场离根因很远。
                if (jt.parent < -1 || jt.parent >= static_cast<int>(n)) {
                    if (err) {
                        *err = "BONES: joint " + std::to_string(j) + " ('" +
                               jt.name + "') has out-of-range parent " +
                               std::to_string(jt.parent);
                    }
                    return false;
                }
            }
            const std::uint32_t topo = r.u32();
            if (topo > 4096) {
                if (err) *err = "BONES: absurd topo count";
                return false;
            }
            out.topoOrder.resize(topo);
            for (std::uint32_t k = 0; k < topo; ++k) {
                const int idx = r.i32();
                if (idx < 0 || idx >= static_cast<int>(n)) {
                    if (err) *err = "BONES: topo index out of range";
                    return false;
                }
                out.topoOrder[k] = idx;
            }
            if (!r.ok()) {
                if (err) *err = "BONES: truncated";
                return false;
            }
            // 自洽校验：文件里存的摘要必须等于按内容重算的那个。
            // CRC 只能说明"字节没坏"，说明不了"两处字段互相矛盾" ——
            // 有人改了骨表忘了改摘要，CRC 一样是对的，而摘要错的后果是
            // "所有 mesh 都觉得自己的骨架不匹配"，退化成走内嵌兜底，
            // 静默且难查。所以这里硬拒。
            const std::uint64_t recomputed = skeletonBoneDigest(out.joints);
            if (recomputed != out.boneDigest) {
                if (err) {
                    char buf[160];
                    std::snprintf(buf, sizeof(buf),
                                  "BONES: digest mismatch (stored=%016llx "
                                  "actual=%016llx)",
                                  static_cast<unsigned long long>(out.boneDigest),
                                  static_cast<unsigned long long>(recomputed));
                    *err = buf;
                }
                return false;
            }
            sawBones = true;

        } else if (e.id == kSkelChunkSocket) {
            const std::uint32_t n = r.u32();
            if (n > 1024) {
                if (err) *err = "SOCKET: absurd socket count";
                return false;
            }
            out.sockets.resize(n);
            for (std::uint32_t i = 0; i < n; ++i) {
                SourceSocket& s = out.sockets[i];
                s.name = resolveString(strs, r.u32());
                s.boneName = resolveString(strs, r.u32());
                s.local = readMat4(r);
            }
            if (!r.ok()) {
                if (err) *err = "SOCKET: truncated";
                return false;
            }
        }
        // 不认识的区块**跳过** —— 这正是区块表存在的意义。
    }

    if (!sawBones) {
        // 骨架资产没有骨表 = 这个文件不是骨架（或者写端有 bug）。
        // 不能"当成空的继续走"：空的骨架会一路装到渲染层，表现成
        // "角色变成一堆散在原点上的三角形"，和文件损坏毫无关联。
        if (err) *err = "BONES: missing section";
        return false;
    }

    out.sourceHash = t.hdr.sourceHash;
    out.ok = true;
    return true;
}

bool inspectSkelFile(const void* data, std::size_t size, SkelFileInfo& info,
                     std::string* err) {
    info = SkelFileInfo{};

    Table t;
    // 体检不校验 CRC：它的用途是"看一眼这是什么"，坏文件也应该能报出
    // 结构信息（`--check` 才是校验那个入口）。
    if (!readTable(data, size, t, err, false)) return false;

    info.version = t.hdr.version;
    info.flags = t.hdr.flags;
    info.sourceHash = t.hdr.sourceHash;
    info.chunkCount = t.hdr.chunkCount;
    info.bytes = t.hdr.bytes;
    info.hasEngineXform = (t.hdr.flags & kSkelFlagEngineXform) != 0;

    for (const ChunkEntry& e : t.entries) {
        Reader r(static_cast<const std::uint8_t*>(data) + e.offset,
                 static_cast<std::size_t>(e.size));
        if (e.id == kSkelChunkBones) {
            info.jointCount = r.u32();
            info.boneDigest = r.u64();
            // 跳过 invMeshNode / skeletonToEngine 与整张骨表，直接取 topo 计数。
            // 每根骨 = name(4) + parent(4) + 3×mat4(192) = 200 字节。
            // ⚠ 这个 200 必须和写端逐项对齐：多跳或少跳 4 字节，读出来的
            //   就是别的字段当 topo 计数（症状："topo=1.6e9"）。
            constexpr std::size_t kJointBytes = 4 + 4 + 192;
            r.skip(128);   // 两个 mat4
            r.skip(static_cast<std::size_t>(info.jointCount) * kJointBytes);
            info.topoCount = r.u32();
        } else if (e.id == kSkelChunkSocket) {
            info.socketCount = r.u32();
        }
    }
    return true;
}

std::string vskelPathFor(const std::string& sourcePath) {
    const std::size_t dot = sourcePath.find_last_of('.');
    const std::size_t slash = sourcePath.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return sourcePath + ".vskel";
    return sourcePath.substr(0, dot) + ".vskel";
}

const char* skelSectionName(std::uint32_t id) {
    switch (id) {
        case kSkelChunkBones: return "BONES";
        case kSkelChunkSocket: return "SOCKET";
        case kSkelChunkString: return "STR";
        default: return "?";
    }
}

} // namespace assetio
