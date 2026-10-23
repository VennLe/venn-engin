// ============================================================
// assets/VMeshLoader —— `.vmesh` 的运行时加载（唯一的"读自研格式"入口）
//
// 这一层的全部工作可以用一句话概括：**把文件里的字节搬到 GPU**。
// 没有任何几何修正、没有坐标轴换算、没有蒙皮烘焙 —— 那些在离线
// `AssetC` 里已经做完了，这里再做一次就是"两处各修一半"的开始。
//
// 与 `FbxLoader` 的关系：
//   · `FbxLoader`   = 从**创作格式**导入（慢，但能读源文件）
//   · `VMeshLoader` = 从**烤好的格式**加载（快，运行时默认走这条）
// 两者的产物同为 `assets::Model`，上层（编辑器 / 运行时）无感。
//
// ⚠ 顶点是**按字节**从文件拷进 `assets::Vertex` 的。这依赖两份结构体
//   布局完全一致：`assetio::Vertex` 的每个字段偏移都有 static_assert，
//   这里再锁一次 sizeof —— 任何一边加字段都会编译期红。
// ============================================================

#include "assets/MeshLoader.h"

#include "assets/AssetManager.h"
#include "assets/AssetPath.h"
#include "assets/Vmeta.h"
#include "core/Logger.h"

#include "assetio/MeshFormat.h"
#include "assetio/Vertex.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace assets {

namespace {

// 布局锁：两个 Vertex 必须逐字节兼容（见文件头说明）
static_assert(sizeof(assets::Vertex) == sizeof(assetio::Vertex),
              "assets::Vertex 与 assetio::Vertex 布局不一致 —— "
              "按字节直拷会读出错位数据");
static_assert(sizeof(assets::Vertex) == assetio::kVertexSize,
              "assets::Vertex 尺寸变了：.vmesh 格式版本需要同步");

std::vector<std::uint8_t> slurp(const std::string& path, bool& ok) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { ok = false; return {}; }
    f.seekg(0, std::ios::end);
    const std::streamoff n = f.tellg();
    if (n <= 0) { ok = (n == 0); return {}; }
    f.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(n));
    f.read(reinterpret_cast<char*>(buf.data()), n);
    ok = f.good() || f.eof();
    return buf;
}

std::uint64_t mtimeOf(const std::string& path) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(path, ec);
    if (ec) return 0;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count());
}

} // namespace

bool vmeshIsFresh(const std::string& sourcePath, const std::string& vmeshPath) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(vmeshPath, ec) || ec) return false;
    const std::uint64_t bin = mtimeOf(vmeshPath);

    // 源文件比 .vmesh 新 = 缓存过期。这条判据必须存在，否则"我改了 FBX
    // 但画面没变"会变成一个只能靠删文件解决的谜题（而删文件这件事
    // 恰恰是新手最不会想到的）。
    const std::uint64_t src = mtimeOf(sourcePath);
    if (src != 0 && bin != 0 && src > bin) return false;

    // `.vmeta` 也要一起看（2026-10-22）：`import.scale` / `generateLods` /
    // `importAnimations` 全都是**烘进** .vmesh 的参数。用户改了 sidecar
    // 却没重烤的话，加载器会拿旧数据当"新配置"用 —— 那种"设置明明改了、
    // 一点效果都没有"的 bug 比崩溃更难查，因为它看起来完全正常。
    const std::uint64_t meta = mtimeOf(vmetaPathFor(sourcePath));
    if (meta != 0 && bin != 0 && meta > bin) return false;

    return true;
}

std::unique_ptr<Model> loadVMeshModel(rhi::Device& device,
                                      rhi::CommandPool& cmdPool,
                                      const TextureContext& ctx,
                                      AssetManager& assets,
                                      const std::string& vmeshAbsPath,
                                      const std::string& name,
                                      const std::string& sourceRelPath) {
    bool ok = false;
    const auto tLoad0 = std::chrono::steady_clock::now();
    const std::vector<std::uint8_t> bytes = slurp(vmeshAbsPath, ok);
    if (!ok) {
        VK_LOG_ERROR("VMESH load failed (read): %s", vmeshAbsPath.c_str());
        return nullptr;
    }

    assetio::SourceModel src;
    std::string err;
    if (!assetio::readMeshFile(bytes.data(), bytes.size(), src, &err)) {
        VK_LOG_ERROR("VMESH load failed (parse): %s (%s)", vmeshAbsPath.c_str(),
                     err.c_str());
        return nullptr;
    }

    // .vmeta：材质 id 的稳定映射仍走 sidecar。这一步不能省 ——
    // 场景里的"材质覆盖"挂在材质 **id** 上，id 一变覆盖就丢了。
    Vmeta vmeta = beginModelImport(name);

    auto model = std::make_unique<Model>();
    model->name = name;

    // ---------------- 材质 ----------------
    // 先建材质再建子网格：子网格只按下标引用，顺序无所谓，但先建完
    // 才能保证"缺材质"不会在循环中间才被发现。
    std::vector<Material*> mats(src.materials.size(), nullptr);
    for (std::size_t i = 0; i < src.materials.size(); ++i) {
        const assetio::SourceMaterial& sm = src.materials[i];
        const std::string key = name + "/mat" + std::to_string(i);

        // ⚠ 全部走 `tryLoadTexture`（容错版）：贴图丢了要**退化成默认材质**，
        //   不能让整个模型加载失败。这条分支以前是死代码 —— 因为
        //   `loadTexture` 在文件读不出来时直接抛，根本走不到下面的 `!albedo`。
        Texture* albedo = nullptr;
        // ⚠ 贴图路径来自 `.vmesh`，是**相对资产根**的（`models/xxx.png`）。
        //   `Texture::fromFile` 只按**进程 cwd** 打开，不认资产根 —— 编辑器
        //   从 `build/bin` 启动时相对路径一条都打不开，`tryLoadTexture` 静默
        //   返回 nullptr，材质退化成纯白，角色就成了一整只石膏像（贴图明明
        //   在盘上、`--info` 也显示 albedo 有值）。TerrainPass 踩过同一个坑
        //   （docs/launcher_roadmap.md「地面模糊重影」），修法一样：先过
        //   `resolveAssetPath` 把 `<exe>/assets` 这个根补上。
        const auto resolve = [](const std::string& p) {
            return p.empty() ? p : assets::resolveAssetPath(p);
        };
        if (!sm.albedoPath.empty()) {
            albedo = assets.tryLoadTexture(resolve(sm.albedoPath), key + "/albedo", true);
        } else if (!sm.albedoRaw.empty()) {
            // 离线期没解析出来 → 运行期再试一次原始路径。
            // 这一步的意义是"资产目录结构变了但贴图还在"，能救回来。
            albedo = assets.tryLoadTexture(resolve(sm.albedoRaw), key + "/albedo", true);
        }
        if (!albedo && (!sm.albedoRaw.empty() || !sm.albedoPath.empty())) {
            VK_LOG_WARN("VMESH-TEX-MISSING model=%s mat=%s path=%s",
                        name.c_str(), sm.name.c_str(),
                        (sm.albedoRaw.empty() ? sm.albedoPath : sm.albedoRaw).c_str());
        }

        Texture* normal = nullptr;
        if (!sm.normalPath.empty())
            normal = assets.tryLoadTexture(resolve(sm.normalPath), key + "/normal", false);
        else if (!sm.normalRaw.empty())
            normal = assets.tryLoadTexture(resolve(sm.normalRaw), key + "/normal", false);
        if (!normal && (!sm.normalRaw.empty() || !sm.normalPath.empty())) {
            VK_LOG_WARN("VMESH-TEX-MISSING model=%s mat=%s kind=normal path=%s",
                        name.c_str(), sm.name.c_str(),
                        (sm.normalRaw.empty() ? sm.normalPath : sm.normalRaw).c_str());
        }

        Material* m = assets.makeMaterialPBR(
            key, albedo, normal, nullptr, sm.baseColor, sm.roughness, sm.metallic,
            sm.emissive, sm.doubleSided);
        if (m) {
            m->source.setModel(name);
            const std::string mid =
                resolveMaterialId(vmeta, name, sm.name, static_cast<int>(i));
            assets.assignMaterialId(m, mid);
            m->captureImportDefaults();
            VK_LOG_INFO("ASSET-MAT-ID model=%s key=%s id=%s",
                        assetLogSafe(name).c_str(), assetLogSafe(key).c_str(),
                        assetLogSafe(m->id).c_str());
        }
        mats[i] = m;
    }

    // ---------------- 子网格 ----------------
    for (std::size_t i = 0; i < src.subMeshes.size(); ++i) {
        const assetio::SourceSubMesh& ss = src.subMeshes[i];
        const std::string subName =
            name + "/" + (ss.name.empty() ? ("sub" + std::to_string(i)) : ss.name);

        // 逐字节搬运。`Vertex` 是平凡可拷贝的 POD，两个类型布局一致
        // （见文件顶部 static_assert），所以这里不需要逐字段赋值。
        std::vector<Vertex> verts(ss.vertices.size());
        if (!verts.empty()) {
            std::memcpy(verts.data(), ss.vertices.data(),
                        verts.size() * sizeof(Vertex));
        }
        std::vector<std::uint32_t> idx = ss.indices;

        Mesh* mesh = assets.registerMesh(subName, std::move(verts), std::move(idx));
        if (!mesh) continue;

        MeshSource msrc;
        msrc.kind = MeshSource::Kind::FBX;
        // 存**源**路径（.fbx），不是 .vmesh：场景文件里引用的永远是
        // 源资产，换台机器只要重新烤一次就行。存 .vmesh 的话，
        // "源文件被挪走"和"缓存被删"两种情况的错误信息会混在一起。
        msrc.path = sourceRelPath;
        msrc.name = name;
        msrc.subMeshIndex = static_cast<int>(i);
        mesh->setSource(msrc);
        mesh->setNoLod(!vmeta.import.generateLods);

        SubMesh sub;
        sub.mesh = mesh;
        sub.material = (ss.material >= 0 &&
                        ss.material < static_cast<int>(mats.size()))
                           ? mats[static_cast<std::size_t>(ss.material)]
                           : nullptr;
        sub.transform = ss.transform;
        sub.name = subName;
        model->subMeshes.push_back(std::move(sub));
    }

    // ---------------- 骨架 + 动画 ----------------
    if (!src.joints.empty()) {
        auto sk = std::make_shared<Skeleton>();
        sk->joints.resize(src.joints.size());
        for (std::size_t j = 0; j < src.joints.size(); ++j) {
            const assetio::SourceJoint& sj = src.joints[j];
            Skeleton::Joint& jt = sk->joints[j];
            jt.name = sj.name;
            jt.parent = sj.parent;
            jt.restLocal = sj.restLocal;
            jt.invBind = sj.invBind;
            jt.externalChain = sj.externalChain;
        }
        sk->topoOrder = src.topoOrder;
        sk->invMeshNode = src.invMeshNode;
        // nodeMap 是 glTF 的 node→joint 映射；FBX 没有这个概念，
        // 留空（`nodeToJoint` 对空 map 返回 -1，调用方已经处理）。

        sk->clips.resize(src.clips.size());
        for (std::size_t c = 0; c < src.clips.size(); ++c) {
            const assetio::SourceClip& sc = src.clips[c];
            AnimationClip& clip = sk->clips[c];
            clip.name = sc.name;
            clip.duration = sc.duration;
            clip.tracks.resize(sc.tracks.size());
            for (std::size_t t = 0; t < sc.tracks.size(); ++t) {
                const assetio::SourceTrack& st = sc.tracks[t];
                AnimTrack& tr = clip.tracks[t];
                tr.joint = st.joint;
                tr.path = static_cast<AnimTrack::Path>(st.path);
                tr.step = st.step;
                tr.times = st.times;
                tr.values = st.values;
            }
        }
        model->skeleton = sk;
    }

    model->center = glm::vec3(0.0f);
    model->radius = src.radius;

    finishModelImport(name, vmeta);

    VK_LOG_INFO("VMESH loaded: %s (%u submeshes, %s, %d joints, %u clips, "
                "radius=%.3f, src=%s, %.1f ms)",
                vmeshAbsPath.c_str(),
                static_cast<std::uint32_t>(model->subMeshes.size()),
                model->skinned() ? "skinned" : "static",
                model->skeleton ? model->skeleton->jointCount() : 0,
                static_cast<std::uint32_t>(model->skeleton ? model->skeleton->clips.size() : 0),
                static_cast<double>(model->radius), sourceRelPath.c_str(),
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - tLoad0).count());

    return model;
}

} // namespace assets
