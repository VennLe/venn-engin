#pragma once
// ============================================================
// assets/AssetManager —— 资源缓存与工厂
// 按名字缓存 Mesh/Texture/Material/Model，避免重复上传 GPU
// 析构时统一释放（须发生在 Device 销毁之前）
// ============================================================

#include "assets/Mesh.h"
#include "assets/Texture.h"
#include "assets/Material.h"
#include "assets/Model.h"

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

    // ---- 纹理 ----
    Texture* checker(const std::string& name, uint8_t a[4], uint8_t b[4],
                     uint32_t cell = 32, uint32_t size = 512);
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

private:
    TextureContext m_ctx;
    std::unordered_map<std::string, std::unique_ptr<Mesh>> m_meshes;
    std::unordered_map<std::string, std::unique_ptr<Texture>> m_textures;
    std::unordered_map<std::string, std::unique_ptr<Material>> m_materials;
    std::unordered_map<std::string, std::unique_ptr<Model>> m_models;
};

} // namespace assets

// ---------------- 内联实现 ----------------

#include "assets/MeshLoader.h"
#include "rhi/Device.h"
#include "rhi/CommandPool.h"

namespace assets {

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
                                      uint32_t size) {
    auto it = m_textures.find(name);
    if (it != m_textures.end()) return it->second.get();
    auto tex = std::make_unique<Texture>();
    tex->makeCheckerboard(m_ctx, size, size, cell, a, b);
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
