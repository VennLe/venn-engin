#include "assets/SkeletonAsset.h"

#include "core/Logger.h"

#include "assetio/SkelFormat.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <vector>

namespace assets {

namespace {

std::vector<std::uint8_t> slurp(const std::string& path, bool& ok) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { ok = false; return {}; }
    f.seekg(0, std::ios::end);
    const std::streamoff n = f.tellg();
    if (n < 0) { ok = false; return {}; }
    f.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(n));
    if (n > 0) f.read(reinterpret_cast<char*>(buf.data()), n);
    ok = f.good() || f.eof();
    return buf;
}

std::uint64_t mtimeOf(const std::string& path) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(path, ec);
    if (ec) return 0;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch())
            .count());
}

// 缓存条目。存的是**解析后的 IR**，不是 `Skeleton`：
// `Skeleton` 会被调用方就地改（重定向换 shared_ptr、RigEdit 改 restLocal），
// 存它等于让两次装配共享可变状态 —— 那种 bug 的症状是"第二个角色
// 的骨架长得像第一个"，而且顺序一变就复现/消失。
struct Entry {
    std::uint64_t mtime = 0;
    assetio::SourceSkeletonAsset data;
};

std::map<std::string, Entry>& cache() {
    static std::map<std::string, Entry> s;
    return s;
}

// 取（必要时解析）一份 IR。失败返回 nullptr 并写 err。
const assetio::SourceSkeletonAsset* acquire(const std::string& absPath,
                                            std::string* err) {
    const std::uint64_t mt = mtimeOf(absPath);
    auto it = cache().find(absPath);
    if (it != cache().end() && mt != 0 && it->second.mtime == mt)
        return &it->second.data;

    bool ok = false;
    const std::vector<std::uint8_t> bytes = slurp(absPath, ok);
    if (!ok) {
        if (err) *err = "read error";
        return nullptr;
    }
    assetio::SourceSkeletonAsset sk;
    if (!assetio::readSkelFile(bytes.data(), bytes.size(), sk, err)) {
        // 解析失败**不进缓存**：下一个调用方应该再试一次（文件可能正在
        // 被 AssetC 重写）。进缓存的话"再试一次"永远拿到同一个失败。
        return nullptr;
    }
    Entry e;
    e.mtime = mt;
    e.data = std::move(sk);
    Entry& slot = cache()[absPath] = std::move(e);
    return &slot.data;
}

} // namespace

bool loadSkeletonAsset(const std::string& absPath, Skeleton& out,
                       std::string* err) {
    const assetio::SourceSkeletonAsset* sk = acquire(absPath, err);
    if (sk == nullptr) return false;

    out = Skeleton{};
    out.joints.resize(sk->joints.size());
    for (std::size_t j = 0; j < sk->joints.size(); ++j) {
        const assetio::SourceJoint& sj = sk->joints[j];
        Skeleton::Joint& jt = out.joints[j];
        jt.name = sj.name;
        jt.parent = sj.parent;
        jt.restLocal = sj.restLocal;
        jt.invBind = sj.invBind;
        jt.externalChain = sj.externalChain;
    }
    out.topoOrder = sk->topoOrder;
    out.invMeshNode = sk->invMeshNode;
    out.skeletonToEngine = sk->skeletonToEngine;
    out.boneDigest = sk->boneDigest;
    // `nodeMap` 是 glTF 的 node→joint 映射，骨架资产里没有这个概念；
    // 空 map 下 `nodeToJoint` 一律返回 -1，调用方已经处理。

    out.sockets.resize(sk->sockets.size());
    for (std::size_t i = 0; i < sk->sockets.size(); ++i) {
        const assetio::SourceSocket& ss = sk->sockets[i];
        Skeleton::Socket& ds = out.sockets[i];
        ds.name = ss.name;
        ds.boneName = ss.boneName;
        ds.local = ss.local;
        ds.bone = out.findJoint(ss.boneName);
    }
    return true;
}

std::size_t skeletonAssetCacheSize() { return cache().size(); }

void clearSkeletonAssetCache() { cache().clear(); }

} // namespace assets
