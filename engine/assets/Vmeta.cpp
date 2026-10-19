#include "assets/Vmeta.h"

#include "assets/AssetPath.h"
#include "core/Logger.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <sstream>
#include <vector>

namespace assets {

using json = nlohmann::json;

namespace {

// `detail=` 里不能有空格（自检日志按空白切字段），所以路径一律走这一层。
std::string safe(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') c = '_';
    }
    return out;
}

std::string hex64(std::uint64_t h) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "fnv1a:0x%016llX",
                  static_cast<unsigned long long>(h));
    return buf;
}

} // namespace

// ============================================================
// 哈希
// ============================================================

std::uint32_t fnv1a32(const void* data, std::size_t size, std::uint32_t h) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

std::string fnv1aFile(const std::string& absPath) {
    std::ifstream in(absPath, std::ios::binary);
    if (!in) return std::string();
    // 文件 FNV-1a 用 64 位：32 位在几十 MB 的 .glb 上碰撞概率不再可忽略，
    // 而这是"要不要重建映射"的判据 —— 误判一次就把材质身份洗掉了。
    std::uint64_t h = 1469598103934665603ull;   // FNV-1a 64 offset basis
    char buf[64 * 1024];
    while (in) {
        in.read(buf, sizeof(buf));
        const std::streamsize n = in.gcount();
        for (std::streamsize i = 0; i < n; ++i) {
            h ^= static_cast<unsigned char>(buf[i]);
            h *= 1099511628211ull;              // FNV-1a 64 prime
        }
    }
    if (h == 0) return std::string();
    return hex64(h);
}

std::string importSignature(const VmetaImport& imp) {
    // ⚠ 顺序必须固定，且**每个字段都要进**：漏一个就会出现"用户改了这个参数
    //   却判不出被改过"，于是不重导（症状是"改了没用"）。
    std::ostringstream os;
    os << "scale=" << imp.scale << ";lods=" << (imp.generateLods ? 1 : 0)
       << ";grid=" << imp.lodGridRes[0] << "," << imp.lodGridRes[1] << ","
       << imp.lodGridRes[2] << ";mats=" << (imp.importMaterials ? 1 : 0)
       << ";anims=" << (imp.importAnimations ? 1 : 0)
       << ";scheme=" << imp.materialIdScheme;
    const std::string s = os.str();
    const std::uint32_t h = fnv1a32(s.data(), s.size());
    char buf[32];
    std::snprintf(buf, sizeof(buf), "fnv1a:0x%08X", h);
    return buf;
}

// ============================================================
// 路径
// ============================================================

std::string vmetaPathFor(const std::string& modelPath) {
    // ⚠ 是**追加**扩展名，不是替换：`anime_girl.glb` 与 `anime_girl.fbx` 是
    //   两个不同的源文件，替换式命名会让它们抢同一个 sidecar，后加载的那个
    //   直接把前一个的材质映射冲掉（症状是"换个模型材质全变成另一个的"）。
    return modelPath + ".vmeta";
}

// ============================================================
// 读 / 写
// ============================================================

bool loadVmeta(const std::string& absVmetaPath, Vmeta& out) {
    std::ifstream in(absVmetaPath, std::ios::binary);
    if (!in) return false;
    json j;
    try {
        in >> j;
    } catch (const std::exception& ex) {
        // 文件坏了不能让它挡住加载：退回"当作没有 sidecar"，重新生成一份。
        VK_LOG_WARN("ASSET-VMETA-BAD path=%s err=%s",
                    safe(absVmetaPath).c_str(), safe(ex.what()).c_str());
        return false;
    }
    if (!j.is_object()) return false;

    out = Vmeta{};
    out.version = j.value("version", 1);
    out.source = j.value("source", std::string());
    out.sourceHash = j.value("sourceHash", std::string());
    out.appliedImportSig = j.value("appliedImportSig", std::string());

    if (j.contains("import") && j["import"].is_object()) {
        const json& ji = j["import"];
        out.import.scale = ji.value("scale", 1.0f);
        out.import.generateLods = ji.value("generateLods", true);
        if (ji.contains("lodGridRes") && ji["lodGridRes"].is_array() &&
            ji["lodGridRes"].size() == 3) {
            for (int i = 0; i < 3; ++i)
                out.import.lodGridRes[i] = ji["lodGridRes"][i].get<int>();
        }
        out.import.importMaterials = ji.value("importMaterials", true);
        out.import.importAnimations = ji.value("importAnimations", true);
        out.import.materialIdScheme =
            ji.value("materialIdScheme", std::string("name"));
    }
    if (j.contains("materials") && j["materials"].is_object()) {
        for (auto it = j["materials"].begin(); it != j["materials"].end(); ++it) {
            if (it.value().is_string())
                out.materials[it.key()] = it.value().get<std::string>();
        }
    }
    out.prevMaterials = out.materials;
    out.existed = true;
    return true;
}

bool saveVmeta(const std::string& absVmetaPath, const Vmeta& v) {
    json j;
    j["version"] = v.version;
    j["source"] = v.source;
    j["sourceHash"] = v.sourceHash;
    j["appliedImportSig"] = v.appliedImportSig;
    json ji;
    ji["scale"] = v.import.scale;
    ji["generateLods"] = v.import.generateLods;
    ji["lodGridRes"] = json::array({v.import.lodGridRes[0],
                                    v.import.lodGridRes[1],
                                    v.import.lodGridRes[2]});
    ji["importMaterials"] = v.import.importMaterials;
    ji["importAnimations"] = v.import.importAnimations;
    ji["materialIdScheme"] = v.import.materialIdScheme;
    j["import"] = ji;
    json jm = json::object();
    for (const auto& kv : v.materials) jm[kv.first] = kv.second;
    j["materials"] = jm;

    std::ofstream outFile(absVmetaPath, std::ios::binary | std::ios::trunc);    if (!outFile) {
        VK_LOG_WARN("ASSET-VMETA-WRITE-FAIL path=%s",
                    safe(absVmetaPath).c_str());
        return false;
    }
    outFile << j.dump(2) << "\n";
    return outFile.good();
}

// ============================================================
// 导入会话
// ============================================================

Vmeta beginModelImport(const std::string& modelRelPath) {
    Vmeta meta;
    meta.source = modelRelPath;

    const std::string absModel = resolveAssetPath(modelRelPath);
    // ⚠ 顺序：先算**当前**源文件的 hash，再读 sidecar。
    //   反过来会让 loadVmeta 把 meta.sourceHash 覆盖成文件里记的旧值，
    //   于是"当前值 == 记录值"恒成立 → changed 永远是 0（源变了也不重导）。
    const std::string curHash = fnv1aFile(absModel);

    const std::string absMeta = resolveAssetPath(vmetaPathFor(modelRelPath));
    loadVmeta(absMeta, meta);   // 失败就当没有（existed 仍是 false）
    const std::string recordedHash = meta.sourceHash;
    meta.sourceHash = curHash;

    if (meta.existed) {
        meta.changed = !recordedHash.empty() && !curHash.empty() &&
                       recordedHash != curHash;
        // 参数被用户改过？只有**记过签名**的 sidecar 才判得出来。
        // 手写的 sidecar 没有签名 → 不判"被改过"，否则第一次加载就会把
        // 用户手写的映射按名字重建一遍。
        meta.forced = !meta.appliedImportSig.empty() &&
                      meta.appliedImportSig != importSignature(meta.import);
    }
    meta.source = modelRelPath;
    return meta;
}

bool finishModelImport(const std::string& modelRelPath, Vmeta& meta) {
    // 源变了 → 映射里有、但这次没见到的名字视为"源里没了"
    if (meta.changed) {
        std::vector<std::string> stale;
        for (const auto& kv : meta.prevMaterials) {
            if (meta.seen.find(kv.first) == meta.seen.end()) {
                VK_LOG_WARN("ASSET-MAT-REMAP-FAIL model=%s name=%s",
                            safe(modelRelPath).c_str(),
                            safe(kv.first).c_str());
                // ⚠ 必须**删掉**这条映射：留着的话下次加载还会再打一次
                //   REMAP-FAIL（永久的假警报），而且名字以后被重新用上时
                //   会继承一条陈旧的 id。
                stale.push_back(kv.first);
            }
        }
        for (const auto& k : stale) meta.materials.erase(k);
    }

    // 参数签名总是刷成"当前这组"（已经应用过了）
    const std::string sig = importSignature(meta.import);
    const bool sigChanged = sig != meta.appliedImportSig;
    meta.appliedImportSig = sig;
    meta.source = modelRelPath;

    const bool matsChanged = meta.materials != meta.prevMaterials;
    const bool needWrite = !meta.existed || meta.changed || meta.forced ||
                           sigChanged || matsChanged;

    VK_LOG_INFO("ASSET-VMETA path=%s hash=%s changed=%d forced=%d mats=%u",
                safe(vmetaPathFor(modelRelPath)).c_str(),
                meta.sourceHash.empty() ? "-" : meta.sourceHash.c_str(),
                meta.changed ? 1 : 0, meta.forced ? 1 : 0,
                static_cast<unsigned>(meta.materials.size()));

    if (!needWrite) return false;
    return saveVmeta(resolveAssetPath(vmetaPathFor(modelRelPath)), meta);
}

std::string resolveMaterialId(Vmeta& meta, const std::string& base,
                              const std::string& matName, int matIdx) {
    // 匿名材质（glTF 里常见）用下标当**名字**参与映射：
    // 这种材质本来就没有稳定身份，只能在没改源的情况下保持稳定。
    const std::string name = matName.empty()
                                 ? ("mat" + std::to_string(matIdx))
                                 : matName;

    meta.seen.insert(name);

    auto it = meta.materials.find(name);
    if (it != meta.materials.end() && !it->second.empty()) return it->second;

    std::string id;
    if (meta.import.materialIdScheme == "index") {
        id = base + "#mat" + std::to_string(matIdx);
    } else {
        id = base + "#" + name;
    }
    meta.materials[name] = id;
    return id;
}

} // namespace assets
