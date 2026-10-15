#pragma once
// ============================================================
// assets/AssetManager —— 资源缓存与工厂
// 按名字缓存 Mesh/Texture/Material/Model，避免重复上传 GPU
// 析构时统一释放（须发生在 Device 销毁之前）
// ============================================================

#include "assets/Mesh.h"
#include "assets/Lod.h"
#include "assets/Texture.h"
#include "assets/Material.h"
#include "assets/Model.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace assets {

class AssetManager {
public:
    explicit AssetManager(const TextureContext& ctx) : m_ctx(ctx) {}
    ~AssetManager() = default;  // unique_ptr 自动释放

    // ---- 网格 ----
    // 注意参数顺序：name 排在细分参数**前面**。这样 `sphere(0.5f)`、
    // `sphere(0.5f, "editor.sphere")` 这些既有写法都不用改，想改细分参数
    // 再往后加 `, segments, rings` 即可。
    //
    // name 同时是缓存键：**参数不同就必须给不同的键**，否则第二次调用会
    // 直接命中缓存、拿着旧参数的网格回去。编辑器里改图元生成参数走的就是
    // 这条路（见 editor/PrimitiveBuilder.h 的 primitiveKey）。
    Mesh* cube(float size = 1.0f, const std::string& name = "cube");
    Mesh* plane(float size = 10.0f, const std::string& name = "plane");
    Mesh* sphere(float radius = 1.0f, const std::string& name = "sphere",
                 int segments = 48, int rings = 24);
    Mesh* cylinder(float radius = 0.5f, const std::string& name = "cylinder",
                   int segments = 48);
    Mesh* loadModel(const std::string& path,
                    const std::string& name = "model");

    // ---- 模型（glTF / GLB）----
    // 解析 .gltf/.glb：网格 + PBR 材质 + 贴图，全部登记进本管理器
    Model* loadGLTF(const std::string& path, const std::string& name = "model");

    // ---- 模型（FBX）----
    // 解析 .fbx：静态网格 / 骨骼网格（骨骼 + 蒙皮 + 动画）。
    Model* loadFBX(const std::string& path, const std::string& name = "model");

    // ---- 纹理 ----
    // srgb 语义见 Texture::makeCheckerboard / makeSolid：数据贴图传 false。
    Texture* checker(const std::string& name, uint8_t a[4], uint8_t b[4],
                     uint32_t cell = 32, uint32_t size = 512,
                     bool srgb = true);
    Texture* solid(const std::string& name, uint8_t color[4]);
    Texture* loadTexture(const std::string& path, const std::string& name = "",
                         bool srgb = true);

    // ---- 材质 ----
    // 便捷版：只给 albedo 贴图，normal/orm 留空（渲染时绑中性缺省贴图）
    Material* makeMaterial(const std::string& name, Texture* albedo,
                           const glm::vec4& baseColor = glm::vec4(1.0f),
                           float roughness = 0.5f, float metallic = 0.0f);

    // 完整版：三张纹理槽 + 全部 PBR 因子
    Material* makeMaterialPBR(const std::string& name, Texture* albedo,
                              Texture* normal, Texture* orm,
                              const glm::vec4& baseColor = glm::vec4(1.0f),
                              float roughness = 0.5f, float metallic = 0.0f,
                              const glm::vec3& emissive = glm::vec3(0.0f),
                              bool doubleSided = false);

    // ---- 底层登记接口（供 MeshLoader 使用）----
    // 已有同名资源时直接复用，返回现有指针
    Mesh* registerMesh(const std::string& key, std::vector<Vertex> vertices,
                       std::vector<uint32_t> indices);
    Texture* registerTexture(const std::string& key, const uint8_t* rgba,
                             uint32_t width, uint32_t height, bool srgb);

    // ---- 按缓存键查找（序列化重建时按名字取回模型派生资源）----
    Mesh* findMesh(const std::string& key) const;
    Texture* findTexture(const std::string& key) const;
    Material* findMaterial(const std::string& key) const;

    // ---- 纹理流：每帧驱动流式中的纹理补传一个 mip 层 ----
    // 由 Application 主循环每帧调用（渲染提交之前）。没有流式中的
    // 纹理时是零开销空转。
    void tickTextureStreaming() {
        for (auto& [key, tex] : m_textures) tex->streamTick();
    }

    // ============================================================
    // LOD
    // ============================================================
    // 惰性为一个网格生成 LOD 链（已生成过就直接返回现有链）。
    //
    // 调用时机：网格**进入场景**时（Application 每帧对当前场景的可见网格
    // 调一次，见 core/Application.cpp）。不在网格创建时立刻生成，是为了
    // 不给"内容浏览器里预览一下"的临时网格白建层级 —— 那些网格一个字节
    // 的 GPU 缓冲都不该多占。
    //
    // 生成策略见 assets/Lod.h 文件头。这里只说所有权：链对象归本管理器
    // （m_lods），指针写回 Mesh。所以 AssetManager 必须先于 Mesh 释放 ——
    // 实际上 Mesh 本身就归它管，析构顺序天然成立。
    const LodChain* ensureLod(Mesh* base);
    const LodChain* findLod(const Mesh* base) const;

private:
    TextureContext m_ctx;
    std::unordered_map<std::string, std::unique_ptr<Mesh>> m_meshes;
    std::unordered_map<std::string, std::unique_ptr<Texture>> m_textures;
    std::unordered_map<std::string, std::unique_ptr<Material>> m_materials;
    std::unordered_map<std::string, std::unique_ptr<Model>> m_models;
    std::unordered_map<const Mesh*, std::unique_ptr<LodChain>> m_lods;

    // 按来源生成各级；只负责往 chain 里 add，不动别的状态
    void buildBuiltinLods(const Mesh& base, const MeshSource& src,
                          LodChain& chain);
    void buildDecimatedLods(Mesh& base, LodChain& chain);
};

} // namespace assets

// ---------------- 内联实现 ----------------

#include "assets/MeshLoader.h"
#include "assets/Lod.h"
#include "core/Logger.h"
#include "rhi/Device.h"
#include "rhi/CommandPool.h"

#include <algorithm>
#include <cstdint>

namespace assets {

// 网格是否蒙皮（任一顶点带非零权重）。
// 只看前若干个顶点就够判断 —— 导入器给同一个 primitive 的顶点填的属性
// 是一致的，没必要为了这个扫完几十万个顶点。留 64 个的余量。
inline bool meshLooksSkinned(const Mesh& m) {
    const std::vector<Vertex>& vs = m.vertices();
    const size_t n = std::min<size_t>(vs.size(), 64);
    for (size_t i = 0; i < n; ++i) {
        const glm::vec4& w = vs[i].weights;
        if (w.x + w.y + w.z + w.w > 0.0f) return true;
    }
    return false;
}

// 给 LOD 网格起缓存键。name 为空（来源不明的网格）时退回用指针地址 ——
// 不能留空，否则所有无名网格的第 1 级都会撞到同一个键、互相复用错网格。
inline std::string lodKey(const Mesh& base, const std::string& name, int lv) {
    std::string stem = name;
    if (stem.empty()) {
        stem = "mesh0x" + std::to_string(
            static_cast<unsigned long long>(
                reinterpret_cast<uintptr_t>(&base)));
    }
    return stem + "#lod" + std::to_string(lv);
}

inline void AssetManager::buildBuiltinLods(const Mesh& base,
                                           const MeshSource& src,
                                           LodChain& chain) {
    // 程序化图元：同样的形状参数重生成，只把细分逐级减半。
    // 立方体（12 三角形）/ 平面（2 三角形）没有可减的细分，循环会
    // 在第一轮就 break，链停在 1 级 —— 等于"这个网格不做 LOD"。
    for (int lv = 1; lv < kMaxLodLevels; ++lv) {
        int seg = 0, ring = 0;
        const bool isSphere = (src.shape == "sphere");
        const bool isCyl = (src.shape == "cylinder");
        if (isSphere) {
            seg = std::max(3, src.segments >> lv);
            ring = std::max(2, src.rings >> lv);
            if (seg == std::max(3, src.segments >> (lv - 1)) &&
                ring == std::max(2, src.rings >> (lv - 1))) {
                break;   // 已经到地板，再减不下去
            }
        } else if (isCyl) {
            seg = std::max(3, src.segments >> lv);
            if (seg == std::max(3, src.segments >> (lv - 1))) break;
        } else {
            break;       // cube / plane / 未知形状：不生成
        }

        std::vector<Vertex> verts = isSphere
            ? makeSphereVertices(src.size, seg, ring)
            : makeCylinderVertices(src.size, seg);
        std::vector<uint32_t> idx = isSphere
            ? makeSphereIndices(seg, ring)
            : makeCylinderIndices(seg);

        Mesh* m = registerMesh(lodKey(base, src.name, lv), std::move(verts),
                               std::move(idx));
        if (!m) break;
        const uint32_t tris = static_cast<uint32_t>(m->indices().size() / 3);
        // 必须真的更省：某些参数下（seg 已经很小）减半后三角形数不变
        if (tris == 0 || tris >= chain.level(chain.levelCount() - 1).triangles) {
            break;
        }
        chain.add(m, kLodThresholds[lv]);
    }
}

inline void AssetManager::buildDecimatedLods(Mesh& base, LodChain& chain) {
    // 导入网格：顶点聚类简化。
    //
    // 格分辨率不能写死。"32³ 对 100k 顶点的模型刚好，对 2k 顶点的模型却
    // 几乎压不动"—— 后者每个格子里落不到一个顶点，什么都不合并。所以这里
    // **按目标三角形数反解格分辨率**：每一级想要上一级的一半，二分找出
    // "能满足这个目标的最大格子数"（格子越大形状越保真，所以取满足条件的
    // 最细的那档）。
    //
    // 二分而不是线性扫，是因为每评估一次都要跑一整遍 O(V) 的聚类；
    // 8 次评估足够定位到 [2,256] 里的合适档位。三角形数对格分辨率不是
    // 严格单调的（会有小抖动），二分会落在附近 —— 对 LOD 来说足够。
    for (int lv = 1; lv < kMaxLodLevels; ++lv) {
        const size_t prevTris = chain.level(chain.levelCount() - 1).triangles;
        if (prevTris < 64) break;               // 已经够简了，别再压
        const size_t target = prevTris / 2;

        int lo = 2, hi = 256;
        LodMeshData best;
        int bestGrid = 0;
        while (lo <= hi) {
            const int mid = (lo + hi) / 2;
            LodMeshData d = decimateClustered(base, mid);
            const size_t t = d.indices.size() / 3;
            if (d.ok && t <= target && t > 0) {
                // 满足目标：记下来，再试试更细的（更保真）
                best = std::move(d);
                bestGrid = mid;
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }
        if (bestGrid == 0 || !best.ok) break;   // 连最粗都压不到目标
        const size_t tris = best.indices.size() / 3;
        // 必须是真的省：抖动出"只少 3 个三角形"的一级没有意义
        if (tris * 100 > prevTris * 85) break;

        Mesh* m = registerMesh(lodKey(base, base.source().name, lv),
                               std::move(best.vertices), std::move(best.indices));
        if (!m) break;
        chain.add(m, kLodThresholds[lv]);
    }
}

inline const LodChain* AssetManager::findLod(const Mesh* base) const {
    if (!base) return nullptr;
    auto it = m_lods.find(base);
    return it == m_lods.end() ? nullptr : it->second.get();
}

inline const LodChain* AssetManager::ensureLod(Mesh* base) {
    if (!base) return nullptr;
    if (const LodChain* c = findLod(base)) return c;

    auto chain = std::make_unique<LodChain>();
    chain->add(base, kLodThresholds[0]);

    const MeshSource& src = base->source();
    if (meshLooksSkinned(*base)) {
        // 蒙皮网格不生成（见 Lod.h 文件头）
    } else if (src.kind == MeshSource::Kind::Builtin) {
        buildBuiltinLods(*base, src, *chain);
    } else {
        buildDecimatedLods(*base, *chain);
    }

    const LodChain* raw = chain.get();
    m_lods.emplace(base, std::move(chain));
    base->setLodChain(raw);

    // 打一行日志：验证脚本靠它断言"真的生成了、三角形真的递减"，
    // 也方便人肉看某个模型到底有没有被简化。
    std::string tris;
    for (size_t i = 0; i < raw->levelCount(); ++i) {
        if (i) tris += "/";
        tris += std::to_string(raw->level(i).triangles);
    }
    VK_LOG_INFO("LOD-BUILD '%s' kind=%d levels=%llu tris=%s",
                src.name.c_str(), static_cast<int>(src.kind),
                static_cast<unsigned long long>(raw->levelCount()),
                tris.c_str());
    return raw;
}

// 便捷：给内置程序化网格补上"来源描述"（序列化重建用）
inline void tagBuiltinMesh(Mesh* mesh, const char* shape, float size,
                           const std::string& name, int segments = 0,
                           int rings = 0) {
    MeshSource src;
    src.kind = MeshSource::Kind::Builtin;
    src.shape = shape;
    src.size = size;
    src.segments = segments;
    src.rings = rings;
    src.name = name;
    mesh->setSource(src);
}

inline Mesh* AssetManager::cube(float size, const std::string& name) {
    auto it = m_meshes.find(name);
    if (it != m_meshes.end()) return it->second.get();
    auto mesh = std::make_unique<Mesh>();
    mesh->upload(*m_ctx.device, *m_ctx.cmdPool, makeCubeVertices(size),
                 makeCubeIndices());
    Mesh* raw = mesh.get();
    tagBuiltinMesh(raw, "cube", size, name);
    m_meshes[name] = std::move(mesh);
    return raw;
}

inline Mesh* AssetManager::plane(float size, const std::string& name) {
    auto it = m_meshes.find(name);
    if (it != m_meshes.end()) return it->second.get();
    auto mesh = std::make_unique<Mesh>();
    mesh->upload(*m_ctx.device, *m_ctx.cmdPool, makePlaneVertices(size),
                 makePlaneIndices());
    Mesh* raw = mesh.get();
    tagBuiltinMesh(raw, "plane", size, name);
    m_meshes[name] = std::move(mesh);
    return raw;
}

inline Mesh* AssetManager::sphere(float radius, const std::string& name,
                                  int segments, int rings) {
    auto it = m_meshes.find(name);
    if (it != m_meshes.end()) return it->second.get();
    // 细分参数兜底：0 或负数会让生成器出现除零 / 空网格
    if (segments < 3) segments = 3;
    if (rings < 2) rings = 2;
    auto mesh = std::make_unique<Mesh>();
    mesh->upload(*m_ctx.device, *m_ctx.cmdPool,
                 makeSphereVertices(radius, segments, rings),
                 makeSphereIndices(segments, rings));
    Mesh* raw = mesh.get();
    tagBuiltinMesh(raw, "sphere", radius, name, segments, rings);
    m_meshes[name] = std::move(mesh);
    return raw;
}

inline Mesh* AssetManager::cylinder(float radius, const std::string& name,
                                    int segments) {
    auto it = m_meshes.find(name);
    if (it != m_meshes.end()) return it->second.get();
    if (segments < 3) segments = 3;
    auto mesh = std::make_unique<Mesh>();
    mesh->upload(*m_ctx.device, *m_ctx.cmdPool,
                 makeCylinderVertices(radius, segments),
                 makeCylinderIndices(segments));
    Mesh* raw = mesh.get();
    tagBuiltinMesh(raw, "cylinder", radius, name, segments, 0);
    m_meshes[name] = std::move(mesh);
    return raw;
}

inline Mesh* AssetManager::loadModel(const std::string& path,
                                     const std::string& name) {
    std::string key = name.empty() ? path : name;
    auto it = m_meshes.find(key);
    if (it != m_meshes.end()) return it->second.get();
    auto mesh = loadOBJ(*m_ctx.device, *m_ctx.cmdPool, path);
    Mesh* raw = mesh.get();
    // loadOBJ 已写好 OBJ 来源与路径，这里补上缓存键
    MeshSource src = raw->source();
    src.name = key;
    raw->setSource(src);
    m_meshes[key] = std::move(mesh);
    return raw;
}

// 便捷：把缓存键写回纹理的来源描述（序列化重建时按同名复用）
inline void tagTextureName(Texture* tex, const std::string& name) {
    if (!tex) return;
    TextureSource src = tex->source();
    src.name = name;
    tex->setSource(src);
}

inline Texture* AssetManager::checker(const std::string& name, uint8_t a[4],
                                      uint8_t b[4], uint32_t cell,
                                      uint32_t size, bool srgb) {
    auto it = m_textures.find(name);
    if (it != m_textures.end()) return it->second.get();
    auto tex = std::make_unique<Texture>();
    tex->makeCheckerboard(m_ctx, size, size, cell, a, b, srgb);
    Texture* raw = tex.get();
    tagTextureName(raw, name);
    m_textures[name] = std::move(tex);
    return raw;
}

inline Texture* AssetManager::solid(const std::string& name, uint8_t color[4]) {
    auto it = m_textures.find(name);
    if (it != m_textures.end()) return it->second.get();
    auto tex = std::make_unique<Texture>();
    tex->makeSolid(m_ctx, color);
    Texture* raw = tex.get();
    tagTextureName(raw, name);
    m_textures[name] = std::move(tex);
    return raw;
}

inline Texture* AssetManager::loadTexture(const std::string& path,
                                          const std::string& name, bool srgb) {
    std::string key = name.empty() ? path : name;
    auto it = m_textures.find(key);
    if (it != m_textures.end()) return it->second.get();
    auto tex = std::make_unique<Texture>();
    tex->fromFile(m_ctx, path, srgb);
    Texture* raw = tex.get();
    tagTextureName(raw, key);
    m_textures[key] = std::move(tex);
    return raw;
}

inline Mesh* AssetManager::findMesh(const std::string& key) const {
    auto it = m_meshes.find(key);
    return it == m_meshes.end() ? nullptr : it->second.get();
}

inline Texture* AssetManager::findTexture(const std::string& key) const {
    auto it = m_textures.find(key);
    return it == m_textures.end() ? nullptr : it->second.get();
}

inline Material* AssetManager::findMaterial(const std::string& key) const {
    auto it = m_materials.find(key);
    return it == m_materials.end() ? nullptr : it->second.get();
}

inline Mesh* AssetManager::registerMesh(const std::string& key,
                                        std::vector<Vertex> vertices,
                                        std::vector<uint32_t> indices) {
    auto it = m_meshes.find(key);
    if (it != m_meshes.end()) return it->second.get();
    auto mesh = std::make_unique<Mesh>();
    mesh->upload(*m_ctx.device, *m_ctx.cmdPool, std::move(vertices),
                 std::move(indices));
    Mesh* raw = mesh.get();
    m_meshes[key] = std::move(mesh);
    return raw;
}

inline Texture* AssetManager::registerTexture(const std::string& key,
                                              const uint8_t* rgba,
                                              uint32_t width, uint32_t height,
                                              bool srgb) {
    auto it = m_textures.find(key);
    if (it != m_textures.end()) return it->second.get();
    auto tex = std::make_unique<Texture>();
    tex->fromPixels(m_ctx, rgba, width, height, srgb);
    Texture* raw = tex.get();
    m_textures[key] = std::move(tex);
    return raw;
}

inline Material* AssetManager::makeMaterial(const std::string& name,
                                            Texture* albedo,
                                            const glm::vec4& baseColor,
                                            float roughness, float metallic) {
    return makeMaterialPBR(name, albedo, nullptr, nullptr, baseColor, roughness,
                           metallic, glm::vec3(0.0f));
}

inline Material* AssetManager::makeMaterialPBR(
    const std::string& name, Texture* albedo, Texture* normal, Texture* orm,
    const glm::vec4& baseColor, float roughness, float metallic,
    const glm::vec3& emissive, bool doubleSided) {
    auto it = m_materials.find(name);
    if (it != m_materials.end()) return it->second.get();

    auto mat = std::make_unique<Material>();
    mat->name = name;
    mat->albedoMap = albedo;
    mat->normalMap = normal;
    mat->ormMap = orm;
    mat->baseColorFactor = baseColor;
    mat->roughness = roughness;
    mat->metallic = metallic;
    mat->emissive = emissive;
    mat->doubleSided = doubleSided;

    Material* raw = mat.get();
    m_materials[name] = std::move(mat);
    return raw;
}

} // namespace assets
