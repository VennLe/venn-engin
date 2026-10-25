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

// sizeof 相等**不能**证明布局一致：把两个字段互换位置，sizeof 一个字节
// 都不变，直拷的结果是"法线读成 UV"这种运行期才会暴露的错位。
// 所以逐字段对偏移 —— 这是唯一能被编译器证明"两份结构体是同一张表"的写法。
#define VENN_VERTEX_LAYOUT_LOCK(f)                                            \
    static_assert(offsetof(assets::Vertex, f) == offsetof(assetio::Vertex, f), \
                  "assets::Vertex::" #f " 偏移与 assetio::Vertex 不一致")
VENN_VERTEX_LAYOUT_LOCK(pos);
VENN_VERTEX_LAYOUT_LOCK(normal);
VENN_VERTEX_LAYOUT_LOCK(uv);
VENN_VERTEX_LAYOUT_LOCK(joints);
VENN_VERTEX_LAYOUT_LOCK(weights);
VENN_VERTEX_LAYOUT_LOCK(color);
VENN_VERTEX_LAYOUT_LOCK(uv2);
VENN_VERTEX_LAYOUT_LOCK(morphBase);
VENN_VERTEX_LAYOUT_LOCK(morphCount);
VENN_VERTEX_LAYOUT_LOCK(tangent);
#undef VENN_VERTEX_LAYOUT_LOCK

static_assert(assets::kNoMorphBase == assetio::kNoMorphBase,
              "morphBase 哨兵两份定义不一致 —— 判'这个顶点有没有 morph'会分叉");
static_assert(assets::kNoMorphTarget == assetio::kNoMorphTarget,
              "kNoMorphTarget 两份定义不一致 —— 断点会被当成真 target 去写权重");

// morph delta 也要双向锁死：读端按 `sizeof(MorphDelta)` 整块 memcpy，
// 布局差一个字段就是"整张脸按 Y 轴偏移形变"这种不可能靠肉眼定位的错。
static_assert(sizeof(assets::MorphDelta) == sizeof(assetio::SourceMorphDelta),
              "MorphDelta 与 SourceMorphDelta 尺寸不一致");
static_assert(offsetof(assets::MorphDelta, position) ==
                  offsetof(assetio::SourceMorphDelta, position),
              "MorphDelta.position 偏移不一致");
static_assert(offsetof(assets::MorphDelta, target) ==
                  offsetof(assetio::SourceMorphDelta, target),
              "MorphDelta.target 偏移不一致");
static_assert(offsetof(assets::MorphDelta, normal) ==
                  offsetof(assetio::SourceMorphDelta, normal),
              "MorphDelta.normal 偏移不一致");
static_assert(offsetof(assets::MorphDelta, vertex) ==
                  offsetof(assetio::SourceMorphDelta, vertex),
              "MorphDelta.vertex 偏移不一致");

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
            // 透明模式（P0-3）：从 IR 带到运行时材质。
            // ⚠ 必须在 captureImportDefaults() **之前** —— 否则"导入时的
            //   默认值"记的是 Opaque，而材质实际是 Blend，diffParams 会认为
            //   用户手工改过 alphaMode，存档里多一条无意义的覆盖项，
            //   而且母材质更新时这个字段再也不会跟随。
            switch (sm.alphaMode) {
                case assetio::SourceMaterial::AlphaMode::Blend:
                    m->alphaMode = Material::AlphaMode::Blend;
                    break;
                case assetio::SourceMaterial::AlphaMode::Mask:
                    m->alphaMode = Material::AlphaMode::Mask;
                    break;
                case assetio::SourceMaterial::AlphaMode::Opaque:
                default:
                    break;
            }
            m->alphaCutoff = sm.alphaCutoff;
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
    // morph 滑条的**跨子网格**视图（§15）：每个子网格各自一份 channel 表，
    // 循环结束后合成模型级的 MorphRig。同名滑条在这里合并。
    std::vector<std::vector<MorphChannel>> perSubChannels;
    std::vector<std::uint32_t> targetCounts;
    perSubChannels.reserve(src.subMeshes.size());
    targetCounts.reserve(src.subMeshes.size());
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

        // ---- morph（§14 / §15）----
        // 逐字节搬运（布局已在文件顶部锁死）。target 名字是**子网格内局部**
        // 的，与 delta 里的 `target` 同一个编号空间；滑条名是**跨子网格**
        // 的语义键（同名合并，见 assets/MorphRig.h）。
        if (!ss.morphStream.empty() || !ss.morphTargets.empty()) {
            std::vector<MorphDelta> md(ss.morphStream.size());
            if (!md.empty()) {
                std::memcpy(md.data(), ss.morphStream.data(),
                            md.size() * sizeof(MorphDelta));
            }
            std::vector<std::string> names;
            names.reserve(ss.morphTargets.size());
            for (const assetio::SourceMorphTarget& mt : ss.morphTargets)
                names.push_back(mt.name);
            std::vector<MorphChannel> chans;
            chans.reserve(ss.morphChannels.size());
            for (const assetio::SourceMorphChannel& sc : ss.morphChannels) {
                MorphChannel c;
                c.name = sc.name;
                c.keys.reserve(sc.keys.size());
                for (const assetio::SourceMorphChannelKey& sk : sc.keys)
                    c.keys.push_back({sk.targetWeight, sk.target});
                chans.push_back(std::move(c));
            }
            // 模型级滑条的原始表（下面 MorphRig 建完之后就不再需要，
            // 但 Mesh 上留一份方便诊断/自检对照）。
            perSubChannels.push_back(chans);
            targetCounts.push_back(
                static_cast<std::uint32_t>(ss.morphTargets.size()));
            mesh->setMorph(std::move(md), std::move(names), std::move(chans));
        } else {
            perSubChannels.emplace_back();
            targetCounts.push_back(0u);
        }

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
    // 滑条合并必须在**所有子网格**都装完之后（同名跨桶合并）。
    model->morphRig.build(perSubChannels, targetCounts);
    if (!model->morphRig.empty()) {
        std::size_t driving = 0;
        for (const MorphRigChannel& c : model->morphRig.channels())
            if (c.drives) ++driving;
        VK_LOG_INFO("VMESH-MORPH model=%s channels=%zu driving=%zu "
                    "subMeshes=%zu",
                    assetLogSafe(name).c_str(), model->morphRig.channelCount(),
                    driving, model->subMeshes.size());
    }
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

    // ---- 顶点通道体检（§13）----
    // 单独一行而不是塞进上面那行：上面那行的正则被 verify_vmesh 的 C3~C6
    // 钉着（`\(... ms\)`），改格式会让四条断言连带红 —— 那是判据的错位，
    // 不是真的回归。新通道有自己的判据，就给它们自己的行。
    //
    // 为什么要打这个：uv2 / tangent / morphBase 现在**没有任何消费者**，
    // 所以"导入器到底有没有把它们填进来"是这一刀唯一能验的东西。
    // 靠人眼看字节是不现实的，靠截图更不可能（改不改变像素都一样）。
    {
        std::size_t nUv2 = 0, nTan = 0, nMorph = 0, nBad = 0, nTotal = 0;
        std::size_t nDeltas = 0, nTargets = 0;
        for (const assetio::SourceSubMesh& sm : src.subMeshes) {
            nDeltas += sm.morphStream.size();
            nTargets += sm.morphTargets.size();
            for (const assetio::Vertex& v : sm.vertices) {
                ++nTotal;
                if (v.uv2[0] != 0.0f || v.uv2[1] != 0.0f) ++nUv2;
                if (v.tangent[3] != 0.0f) ++nTan;
                if (v.morphBase != assetio::kNoMorphBase) ++nMorph;
                // "有 base 没 count / 有 count 没 base" 是矛盾状态。
                // 读端（MeshFormat.cpp）已经硬拒过一次，这里再数一遍是
                // 为了让**日志本身**能当判据 —— 验收脚本读的就是这一行。
                if ((v.morphBase == assetio::kNoMorphBase) != (v.morphCount == 0))
                    ++nBad;
            }
        }
        VK_LOG_INFO("VMESH-VFMT stride=%zu verts=%zu uv2=%zu tangent=%zu "
                    "morphVerts=%zu morphDeltas=%zu morphTargets=%zu morphBad=%zu",
                    assetio::kVertexSize, nTotal, nUv2, nTan, nMorph, nDeltas,
                    nTargets, nBad);
    }

    return model;
}

} // namespace assets
