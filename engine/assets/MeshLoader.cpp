#include "assets/MeshLoader.h"
#include "assets/AssetManager.h"
#include "assets/AssetPath.h"
#include "rhi/Device.h"
#include "rhi/CommandPool.h"

#include "core/Logger.h"

#include <cstring>
#include <functional>

#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>

// ---- tinygltf ----
// 不让 tinygltf 自带 stb_image 的实现：项目里 Texture.cpp 已经有一份
// （否则 stb 的符号会重复定义）。改为注册自己的 LoadImageData 回调，
// 复用同一份 stb_image。
#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>

#include <stb_image.h>

namespace assets {

std::unique_ptr<Mesh> loadOBJ(rhi::Device& device, rhi::CommandPool& cmdPool,
                              const std::string& path) {
    tinyobj::ObjReaderConfig config;
    config.triangulate = true;
    config.vertex_color = false;

    tinyobj::ObjReader reader;
    if (!reader.ParseFromFile(path, config)) {
        if (!reader.Error().empty()) {
            VK_LOG_ERROR("loadOBJ: %s", reader.Error().c_str());
        }
        throw std::runtime_error("Failed to parse OBJ: " + path);
    }
    if (!reader.Warning().empty()) {
        VK_LOG_WARN("loadOBJ: %s", reader.Warning().c_str());
    }

    const auto& attrib = reader.GetAttrib();
    const auto& shapes = reader.GetShapes();

    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    // 去重表：多索引组合 → 顶点下标
    std::unordered_map<std::string, uint32_t> unique;
    bool needNormals =
        attrib.normals.empty();

    for (const auto& shape : shapes) {
        size_t faceVertCount = 0;
        for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); ++f) {
            size_t fv = shape.mesh.num_face_vertices[f];
            // 已配置 triangulate=true，理论恒为 3
            std::vector<glm::vec3> facePts;
            for (size_t v = 0; v < fv; ++v) {
                auto idx = shape.mesh.indices[faceVertCount + v];
                Vertex vert{};
                vert.pos = {attrib.vertices[3 * idx.vertex_index + 0],
                            attrib.vertices[3 * idx.vertex_index + 1],
                            attrib.vertices[3 * idx.vertex_index + 2]};
                if (idx.normal_index >= 0 &&
                    static_cast<size_t>(idx.normal_index) * 3 + 2 <
                        attrib.normals.size()) {
                    vert.normal = {attrib.normals[3 * idx.normal_index + 0],
                                   attrib.normals[3 * idx.normal_index + 1],
                                   attrib.normals[3 * idx.normal_index + 2]};
                }
                if (idx.texcoord_index >= 0) {
                    vert.uv = {attrib.texcoords[2 * idx.texcoord_index + 0],
                               1.0f - attrib.texcoords[2 * idx.texcoord_index + 1]};
                }
                facePts.push_back(vert.pos);

                std::string key = std::to_string(idx.vertex_index) + "/" +
                                  std::to_string(idx.normal_index) + "/" +
                                  std::to_string(idx.texcoord_index);
                auto it = unique.find(key);
                if (it != unique.end()) {
                    indices.push_back(it->second);
                } else {
                    uint32_t newIndex = static_cast<uint32_t>(vertices.size());
                    unique[key] = newIndex;
                    vertices.push_back(vert);
                    indices.push_back(newIndex);
                }
            }
            // 缺法线时用面法线近似
            if (needNormals && facePts.size() >= 3) {
                glm::vec3 n = glm::normalize(
                    glm::cross(facePts[1] - facePts[0], facePts[2] - facePts[0]));
                for (size_t v = 0; v < fv; ++v) {
                    size_t vi = faceVertCount + v;
                    auto idx = shape.mesh.indices[vi];
                    // 找到本面顶点对应的全局顶点并补法线
                    std::string key =
                        std::to_string(idx.vertex_index) + "/" +
                        std::to_string(idx.normal_index) + "/" +
                        std::to_string(idx.texcoord_index);
                    vertices[unique[key]].normal = n;
                }
            }
            faceVertCount += fv;
        }
    }

    auto mesh = std::make_unique<Mesh>();
    mesh->upload(device, cmdPool, std::move(vertices), std::move(indices));
    // 记录来源（路径转成相对资产目录，保证场景 JSON 可移植）
    MeshSource src;
    src.kind = MeshSource::Kind::OBJ;
    src.path = makeAssetRelative(path);
    mesh->setSource(src);
    VK_LOG_INFO("OBJ loaded: %s (%u verts, %u indices)", path.c_str(),
                (uint32_t)mesh->vertices().size(),
                (uint32_t)mesh->indices().size());
    return mesh;
}

// ============================================================
// glTF / GLB 加载
// ============================================================

namespace {

// 自定义图片解码：复用项目里那一份 stb_image（Texture.cpp 提供了实现）
bool stbDecodeImage(tinygltf::Image* image, const int image_idx,
                    std::string* err, std::string* warn, int req_width,
                    int req_height, const unsigned char* bytes, int size,
                    void* user_data) {
    (void)image_idx;
    (void)warn;
    (void)req_width;
    (void)req_height;
    (void)user_data;

    int w = 0, h = 0, comp = 0;
    unsigned char* px =
        stbi_load_from_memory(bytes, size, &w, &h, &comp, STBI_rgb_alpha);
    if (!px) {
        if (err) *err = std::string("stb_image: ") + stbi_failure_reason();
        return false;
    }
    image->width = w;
    image->height = h;
    image->component = 4;
    image->bits = 8;
    image->pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
    image->image.assign(px, px + static_cast<size_t>(w) * h * 4);
    stbi_image_free(px);
    return true;
}

// accessor 的"视图"：数据指针 + 步长 + 分量数
struct AccessorView {
    const uint8_t* data = nullptr;
    size_t stride = 0;
    size_t count = 0;
    int comps = 0;
    int compType = 0;
};

size_t componentSize(int compType) {
    switch (compType) {
        case TINYGLTF_COMPONENT_TYPE_BYTE:
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
            return 1;
        case TINYGLTF_COMPONENT_TYPE_SHORT:
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
            return 2;
        case TINYGLTF_COMPONENT_TYPE_INT:
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
        case TINYGLTF_COMPONENT_TYPE_FLOAT:
            return 4;
        case TINYGLTF_COMPONENT_TYPE_DOUBLE:
            return 8;
        default:
            return 0;
    }
}

bool accessorView(const tinygltf::Model& gm, int accIdx, AccessorView& out) {
    if (accIdx < 0 || accIdx >= static_cast<int>(gm.accessors.size()))
        return false;
    const auto& acc = gm.accessors[accIdx];
    if (acc.bufferView < 0) return false;
    const auto& bv = gm.bufferViews[acc.bufferView];
    const auto& buf = gm.buffers[bv.buffer];

    const size_t cs = componentSize(acc.componentType);
    const int comps = tinygltf::GetNumComponentsInType(
        static_cast<uint32_t>(acc.type));
    if (cs == 0 || comps <= 0) return false;

    out.comps = comps;
    out.compType = acc.componentType;
    out.count = static_cast<size_t>(acc.count);
    // byteStride 为 0 表示顶点属性紧密排列
    out.stride = bv.byteStride != 0 ? bv.byteStride : cs * static_cast<size_t>(comps);

    const size_t offset = bv.byteOffset + acc.byteOffset;
    if (offset >= buf.data.size()) return false;
    out.data = buf.data.data() + offset;
    return true;
}

float readFloat(const uint8_t* p) {
    float f = 0.0f;
    std::memcpy(&f, p, sizeof(float));
    return f;
}

glm::vec3 readVec3(const AccessorView& v, size_t i) {
    const uint8_t* p = v.data + i * v.stride;
    return {readFloat(p), readFloat(p + 4), readFloat(p + 8)};
}

glm::vec2 readVec2(const AccessorView& v, size_t i) {
    const uint8_t* p = v.data + i * v.stride;
    return {readFloat(p), readFloat(p + 4)};
}

uint32_t readIndex(const AccessorView& v, size_t i) {
    const uint8_t* p = v.data + i * v.stride;
    switch (v.compType) {
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
            return *p;
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
            uint16_t x = 0;
            std::memcpy(&x, p, 2);
            return x;
        }
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: {
            uint32_t x = 0;
            std::memcpy(&x, p, 4);
            return x;
        }
        default:
            return 0;
    }
}

// 节点的局部变换：优先用 matrix，否则用 TRS 组合
glm::mat4 nodeLocalMatrix(const tinygltf::Node& node) {
    if (node.matrix.size() == 16) {
        glm::mat4 m(1.0f);
        // glTF 的 matrix 是列主序，与 glm 的内存布局一致
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                m[c][r] = static_cast<float>(node.matrix[c * 4 + r]);
        return m;
    }

    glm::mat4 m(1.0f);
    if (node.translation.size() == 3)
        m = glm::translate(m, glm::vec3(static_cast<float>(node.translation[0]),
                                        static_cast<float>(node.translation[1]),
                                        static_cast<float>(node.translation[2])));
    if (node.rotation.size() == 4) {
        // glTF 四元数顺序是 (x, y, z, w)，glm::quat 构造是 (w, x, y, z)
        glm::quat q(static_cast<float>(node.rotation[3]),
                    static_cast<float>(node.rotation[0]),
                    static_cast<float>(node.rotation[1]),
                    static_cast<float>(node.rotation[2]));
        m *= glm::mat4_cast(q);
    }
    if (node.scale.size() == 3)
        m = glm::scale(m, glm::vec3(static_cast<float>(node.scale[0]),
                                    static_cast<float>(node.scale[1]),
                                    static_cast<float>(node.scale[2])));
    return m;
}

// glTF texture index → 引擎 Texture（按用途决定 sRGB / 线性）
Texture* gltfTexture(AssetManager& assets, const tinygltf::Model& gm,
                     int texIdx, const std::string& base, bool srgb) {
    if (texIdx < 0 || texIdx >= static_cast<int>(gm.textures.size()))
        return nullptr;
    const auto& tex = gm.textures[texIdx];
    if (tex.source < 0 || tex.source >= static_cast<int>(gm.images.size()))
        return nullptr;
    const auto& img = gm.images[tex.source];
    if (img.image.empty()) return nullptr;

    const std::string key = base + "/tex" + std::to_string(texIdx) +
                            (srgb ? "_srgb" : "_lin");
    return assets.registerTexture(key, img.image.data(),
                                  static_cast<uint32_t>(img.width),
                                  static_cast<uint32_t>(img.height), srgb);
}

// 从 glTF 组装本引擎约定的 ORM 贴图：R=AO, G=Roughness, B=Metallic
//
// 为什么不能直接把 glTF 的 metallicRoughness 贴图当 ormMap 用：
//   glTF 规范里该贴图的 R 通道是"未使用（ignored）"，导出器**常写 0**
//   （Khronos 官方 Cube 样例就是 R 全 0）。而本引擎的 ORM 约定 R=AO，
//   直接复用会把 AO 清零 → 环境光整项归零 → 物体发黑。
//   所以必须重新打包，把 R 换成真正的 occlusion（缺省 1.0）。
Texture* buildOrmTexture(AssetManager& assets, const tinygltf::Model& gm,
                         const tinygltf::Material& gmat,
                         const std::string& base) {
    const int mrIdx = gmat.pbrMetallicRoughness.metallicRoughnessTexture.index;
    const int ocIdx = gmat.occlusionTexture.index;
    if (mrIdx < 0 && ocIdx < 0) return nullptr;  // 无贴图 → 用缺省白，因子生效

    auto imageOf = [&](int texIdx) -> const tinygltf::Image* {
        if (texIdx < 0 || texIdx >= static_cast<int>(gm.textures.size()))
            return nullptr;
        const int src = gm.textures[texIdx].source;
        if (src < 0 || src >= static_cast<int>(gm.images.size())) return nullptr;
        const auto* img = &gm.images[src];
        return img->image.empty() ? nullptr : img;
    };

    const tinygltf::Image* mr = imageOf(mrIdx);
    const tinygltf::Image* oc = imageOf(ocIdx);
    if (!mr && !oc) return nullptr;

    const int w = mr ? mr->width : oc->width;
    const int h = mr ? mr->height : oc->height;
    if (w <= 0 || h <= 0) return nullptr;

    const float occStrength = static_cast<float>(gmat.occlusionTexture.strength);

    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t di = (static_cast<size_t>(y) * w + x) * 4;
            uint8_t ao = 255, rough = 255, metal = 255;

            if (mr && x < mr->width && y < mr->height) {
                const size_t si = (static_cast<size_t>(y) * mr->width + x) * 4;
                if (si + 2 < mr->image.size()) {
                    rough = mr->image[si + 1];  // G = 粗糙度
                    metal = mr->image[si + 2];  // B = 金属度
                }
            }
            if (oc && x < oc->width && y < oc->height) {
                const size_t si = (static_cast<size_t>(y) * oc->width + x) * 4;
                if (si < oc->image.size()) {
                    // glTF：occluded = lerp(color, color*occ, strength)
                    const float v = static_cast<float>(oc->image[si]) / 255.0f;
                    float a = 1.0f - occStrength * (1.0f - v);
                    if (a < 0.0f) a = 0.0f;
                    if (a > 1.0f) a = 1.0f;
                    ao = static_cast<uint8_t>(a * 255.0f + 0.5f);
                }
            }

            px[di + 0] = ao;
            px[di + 1] = rough;
            px[di + 2] = metal;
            px[di + 3] = 255;
        }
    }

    return assets.registerTexture(base + "/orm", px.data(),
                                  static_cast<uint32_t>(w),
                                  static_cast<uint32_t>(h), false);
}

// glTF material → 引擎 Material
Material* gltfMaterial(AssetManager& assets, const tinygltf::Model& gm,
                       int matIdx, const std::string& base) {
    const auto& gmat = gm.materials[matIdx];
    const std::string key = base + "/mat" + std::to_string(matIdx);

    const auto& pbr = gmat.pbrMetallicRoughness;

    glm::vec4 baseColor(1.0f);
    if (pbr.baseColorFactor.size() == 4) {
        baseColor = glm::vec4(static_cast<float>(pbr.baseColorFactor[0]),
                              static_cast<float>(pbr.baseColorFactor[1]),
                              static_cast<float>(pbr.baseColorFactor[2]),
                              static_cast<float>(pbr.baseColorFactor[3]));
    }

    glm::vec3 emissive(0.0f);
    if (gmat.emissiveFactor.size() == 3) {
        emissive = glm::vec3(static_cast<float>(gmat.emissiveFactor[0]),
                             static_cast<float>(gmat.emissiveFactor[1]),
                             static_cast<float>(gmat.emissiveFactor[2]));
    }

    Texture* albedo =
        gltfTexture(assets, gm, pbr.baseColorTexture.index, base, true);
    Texture* normal =
        gltfTexture(assets, gm, gmat.normalTexture.index, base, false);
    Texture* orm = buildOrmTexture(assets, gm, gmat, base);

    Material* mat = assets.makeMaterialPBR(
        key, albedo, normal, orm, baseColor,
        static_cast<float>(pbr.roughnessFactor),
        static_cast<float>(pbr.metallicFactor), emissive, gmat.doubleSided);
    // 标记来源：这个材质来自模型，重建方式是重新加载该模型
    if (mat) mat->source.setModel(base);
    return mat;
}

} // namespace

std::unique_ptr<Model> loadGLTFModel(rhi::Device& device,
                                     rhi::CommandPool& cmdPool,
                                     const TextureContext& ctx,
                                     AssetManager& assets,
                                     const std::string& path,
                                     const std::string& name) {
    (void)device;
    (void)cmdPool;
    (void)ctx;

    tinygltf::TinyGLTF loader;
    loader.SetImageLoader(stbDecodeImage, nullptr);

    tinygltf::Model gm;
    std::string err, warn;

    const bool isBinary =
        path.size() > 4 && path.compare(path.size() - 4, 4, ".glb") == 0;
    const bool ok = isBinary
                        ? loader.LoadBinaryFromFile(&gm, &err, &warn, path)
                        : loader.LoadASCIIFromFile(&gm, &err, &warn, path);

    if (!warn.empty()) VK_LOG_WARN("glTF warning: %s", warn.c_str());
    if (!ok) {
        VK_LOG_ERROR("glTF load failed: %s (%s)", path.c_str(), err.c_str());
        throw std::runtime_error("Failed to load glTF: " + path);
    }

    auto model = std::make_unique<Model>();
    model->name = name;

    glm::vec3 bmin(1e30f), bmax(-1e30f);

    std::function<void(int, const glm::mat4&, int)> walk =
        [&](int nodeIdx, const glm::mat4& parent, int depth) {
            if (nodeIdx < 0 || nodeIdx >= static_cast<int>(gm.nodes.size()))
                return;
            if (depth > 64) {  // 防御异常的深层/环形节点树
                VK_LOG_WARN("glTF: node hierarchy too deep, truncated");
                return;
            }
            const auto& node = gm.nodes[nodeIdx];
            const glm::mat4 world = parent * nodeLocalMatrix(node);

            if (node.mesh >= 0 &&
                node.mesh < static_cast<int>(gm.meshes.size())) {
                const auto& gmesh = gm.meshes[node.mesh];

                for (size_t p = 0; p < gmesh.primitives.size(); ++p) {
                    const auto& prim = gmesh.primitives[p];
                    // mode 缺省(-1)按三角形处理
                    if (prim.mode != TINYGLTF_MODE_TRIANGLES && prim.mode != -1) {
                        VK_LOG_WARN("glTF: skip non-triangle primitive (mode=%d)",
                                    prim.mode);
                        continue;
                    }

                    auto itPos = prim.attributes.find("POSITION");
                    if (itPos == prim.attributes.end()) continue;
                    AccessorView aPos;
                    if (!accessorView(gm, itPos->second, aPos)) continue;

                    std::vector<Vertex> verts(aPos.count);
                    for (size_t i = 0; i < aPos.count; ++i)
                        verts[i].pos = readVec3(aPos, i);

                    // NORMAL
                    auto itN = prim.attributes.find("NORMAL");
                    AccessorView aNrm;
                    const bool hasNormals =
                        itN != prim.attributes.end() &&
                        accessorView(gm, itN->second, aNrm);
                    if (hasNormals) {
                        for (size_t i = 0; i < aPos.count; ++i)
                            verts[i].normal = readVec3(aNrm, i);
                    }

                    // TEXCOORD_0
                    auto itUv = prim.attributes.find("TEXCOORD_0");
                    AccessorView aUv;
                    const bool hasUv = itUv != prim.attributes.end() &&
                                       accessorView(gm, itUv->second, aUv);
                    if (hasUv) {
                        // glTF 的 UV 原点在左上，与 Vulkan 图像行序一致 → 不翻转
                        // （OBJ 的 UV 原点在左下，所以 loadOBJ 里才要 1-v）
                        for (size_t i = 0; i < aPos.count; ++i)
                            verts[i].uv = readVec2(aUv, i);
                    }
                    // TANGENT 故意忽略：着色器用屏幕空间导数构造 TBN

                    for (const auto& v : verts) {
                        bmin = glm::min(bmin, v.pos);
                        bmax = glm::max(bmax, v.pos);
                    }

                    std::vector<uint32_t> indices;
                    if (prim.indices >= 0) {
                        AccessorView aIdx;
                        if (accessorView(gm, prim.indices, aIdx)) {
                            indices.resize(aIdx.count);
                            for (size_t i = 0; i < aIdx.count; ++i)
                                indices[i] = readIndex(aIdx, i);
                        }
                    }
                    if (indices.empty()) {  // 非索引几何
                        indices.resize(verts.size());
                        for (size_t i = 0; i < indices.size(); ++i)
                            indices[i] = static_cast<uint32_t>(i);
                    }

                    // 缺法线时按面累加再归一化（比逐面覆盖更平滑）
                    if (!hasNormals) {
                        for (auto& v : verts) v.normal = glm::vec3(0.0f);
                        for (size_t t = 0; t + 2 < indices.size(); t += 3) {
                            Vertex& v0 = verts[indices[t]];
                            Vertex& v1 = verts[indices[t + 1]];
                            Vertex& v2 = verts[indices[t + 2]];
                            const glm::vec3 n =
                                glm::cross(v1.pos - v0.pos, v2.pos - v0.pos);
                            v0.normal += n;
                            v1.normal += n;
                            v2.normal += n;
                        }
                        for (auto& v : verts) {
                            const float len = glm::length(v.normal);
                            v.normal = len > 1e-8f ? v.normal / len
                                                   : glm::vec3(0.0f, 1.0f, 0.0f);
                        }
                    }

                    const std::string meshBase =
                        gmesh.name.empty()
                            ? ("mesh" + std::to_string(node.mesh))
                            : gmesh.name;
                    const std::string subName =
                        name + "/" + meshBase + "_" + std::to_string(p);

                    SubMesh sm;
                    sm.mesh = assets.registerMesh(subName, std::move(verts),
                                                  std::move(indices));
                    sm.name = subName;
                    sm.transform = world;
                    // 记录网格来源：glTF 文件 + 模型键 + primitive 下标
                    if (sm.mesh) {
                        MeshSource msrc;
                        msrc.kind = MeshSource::Kind::GLTF;
                        msrc.path = makeAssetRelative(path);
                        msrc.name = name;  // GLTF 下 name 存"模型键"
                        msrc.subMeshIndex = static_cast<int>(p);
                        sm.mesh->setSource(msrc);
                    }
                    if (prim.material >= 0 &&
                        prim.material < static_cast<int>(gm.materials.size())) {
                        sm.material =
                            gltfMaterial(assets, gm, prim.material, name);
                    }
                    model->subMeshes.push_back(std::move(sm));
                }
            }

            for (int c : node.children) walk(c, world, depth + 1);
        };

    if (gm.defaultScene >= 0 &&
        gm.defaultScene < static_cast<int>(gm.scenes.size())) {
        for (int n : gm.scenes[gm.defaultScene].nodes)
            walk(n, glm::mat4(1.0f), 0);
    } else {
        for (size_t n = 0; n < gm.nodes.size(); ++n)
            walk(static_cast<int>(n), glm::mat4(1.0f), 0);
    }

    if (bmin.x <= bmax.x) {
        model->center = (bmin + bmax) * 0.5f;
        model->radius = glm::length(bmax - bmin) * 0.5f;
    }

    VK_LOG_INFO("glTF loaded: %s (%u submeshes, %u materials)", path.c_str(),
                static_cast<uint32_t>(model->subMeshes.size()),
                static_cast<uint32_t>(gm.materials.size()));
    return model;
}

Model* AssetManager::loadGLTF(const std::string& path,
                              const std::string& name) {
    const std::string key = name.empty() ? path : name;
    auto it = m_models.find(key);
    if (it != m_models.end()) return it->second.get();

    auto model = loadGLTFModel(*m_ctx.device, *m_ctx.cmdPool, m_ctx, *this,
                               path, key);
    Model* raw = model.get();
    m_models[key] = std::move(model);
    return raw;
}

} // namespace assets
