#include "assets/MeshLoader.h"

#include "assets/Vmeta.h"
#include "assets/AssetManager.h"
#include "assets/AssetPath.h"
#include "rhi/Device.h"
#include "rhi/CommandPool.h"

#include "core/Logger.h"

// 离线资产格式：`AssetManager::loadFBX` 要先探一下同名的 `.vmesh`。
// 只用 vmeshPathFor()，不需要几何层，所以 include 头就够。
#include "assetio/MeshFormat.h"

#include <cstring>
#include <filesystem>
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

// 材质的稳定身份（W0-2）：`<模型路径>#<glTF 材质名>`。
//
// 以前材质"身份"就是缓存键 `models/xxx.glb/mat0` —— **数组下标**。模型重导出
// 时材质顺序一变，所有引用静默错位（mat0 从 Skin 变成 Hair，没有任何报错）。
//
// 两条回落：
//   · 材质名为空 → `#mat<idx>`：匿名材质至少稳定在"位置"上，且一眼看得出
//   · 同名 → 追加 `#2`/`#3`（按**前面出现过几个同名**算，确定性且幂等 ——
//     同一份文件读两次得到同一串 id）
std::string gltfMaterialName(const tinygltf::Model& gm, int matIdx,
                             const std::string& base) {
    if (matIdx < 0 || matIdx >= static_cast<int>(gm.materials.size()))
        return std::string();

    std::string n = gm.materials[static_cast<size_t>(matIdx)].name;
    if (n.empty()) return std::string();

    int dup = 0;
    for (int i = 0; i < matIdx; ++i) {
        if (gm.materials[static_cast<size_t>(i)].name == n) ++dup;
    }
    if (dup > 0) {
        const std::string orig = n;
        n += "#" + std::to_string(dup + 1);
        VK_LOG_WARN("ASSET-MAT-DUPNAME model=%s name=%s id=%s",
                    assetLogSafe(base).c_str(),
                    assetLogSafe(orig).c_str(),
                    assetLogSafe(n).c_str());
    }
    return n;
}

// glTF material → 引擎 Material
Material* gltfMaterial(AssetManager& assets, const tinygltf::Model& gm,
                       int matIdx, const std::string& base, Vmeta* vm) {
    const auto& gmat = gm.materials[matIdx];
    const std::string key = base + "/mat" + std::to_string(matIdx);

    // ⚠ 已经导入过就**原样返回**，不按参数重新推导：
    //   用户在编辑器里改过的材质参数（W0-3 的覆盖）不能被一次重导入静默刷掉。
    //   "源文件变了要不要重导"是 W2-2 的策略（sourceHash），不在这里决定。
    if (Material* cached = assets.findMaterial(key)) return cached;

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
    if (mat) {
        mat->source.setModel(base);
        // W0-2：稳定身份；W0-3：记下导入时的原始参数（算 overrides 用）
        // W2-2：id 走 .vmeta 的映射（重导出换了顺序也不串味）；
        // 没有 sidecar 时 resolveMaterialId 自己生成并记进映射。
        const std::string nm = gltfMaterialName(gm, matIdx, base);
        const std::string mid =
            vm ? resolveMaterialId(*vm, base, nm, matIdx)
               : (base + "#" + (nm.empty() ? ("mat" + std::to_string(matIdx))
                                           : nm));
        assets.assignMaterialId(mat, mid);
        mat->captureImportDefaults();
        // 锚点：用来断言"7 个 id 与 7 个 glTF 材质名一一对应"。
        // ⚠ 这里刻意打 `key=` 与 `id=` 两列 —— 只打 id 的话，
        //   "id 改回数组下标"这条否定测试仍然会过（下标也是唯一字符串）。
        VK_LOG_INFO("ASSET-MAT-ID model=%s key=%s id=%s",
                    assetLogSafe(base).c_str(),
                    assetLogSafe(key).c_str(),
                    assetLogSafe(mat->id).c_str());
    }
    return mat;
}

// ---- 蒙皮 / 动画解析（2026-10-11 引入，见 assets/Skeleton.h）----

// JOINTS_0 的分量读取（UNSIGNED_BYTE / SHORT / UINT 都合法）
uint32_t readJoint(const AccessorView& v, size_t i) {
    const uint8_t* p = v.data + i * v.stride;
    switch (v.compType) {
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
            return *p;
        case TINYGLTF_COMPONENT_TYPE_SHORT: {
            int16_t x = 0;
            std::memcpy(&x, p, 2);
            return static_cast<uint32_t>(x);
        }
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

// 读 JOINTS_0 第 i 个元素的第 k 分量（k=0..3）
uint32_t readJointAt(const AccessorView& v, size_t i, int k) {
    const uint8_t* p = v.data + i * v.stride;
    switch (v.compType) {
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
            return p[k];
        case TINYGLTF_COMPONENT_TYPE_SHORT: {
            int16_t x = 0;
            std::memcpy(&x, p + k * 2, 2);
            return static_cast<uint32_t>(x);
        }
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
            uint16_t x = 0;
            std::memcpy(&x, p + k * 2, 2);
            return x;
        }
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: {
            uint32_t x = 0;
            std::memcpy(&x, p + k * 4, 4);
            return x;
        }
        default:
            return 0;
    }
}

glm::vec4 readVec4(const AccessorView& v, size_t i) {
    const uint8_t* p = v.data + i * v.stride;
    if (v.compType == TINYGLTF_COMPONENT_TYPE_FLOAT)
        return {readFloat(p), readFloat(p + 4), readFloat(p + 8),
                readFloat(p + 12)};
    // 归一化小整型（规范化 accessor）极少见于 WEIGHTS_0，防御性支持
    float c[4] = {0, 0, 0, 0};
    for (int k = 0; k < v.comps && k < 4; ++k) {
        switch (v.compType) {
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                c[k] = p[k] / 255.0f;
                break;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
                uint16_t x = 0;
                std::memcpy(&x, p + k * 2, 2);
                c[k] = x / 65535.0f;
                break;
            }
        }
    }
    return {c[0], c[1], c[2], c[3]};
}

// glTF skin → 引擎 Skeleton（不含动画；动画单独 parseAnimations）。
// meshNodeWorld：蒙皮 mesh 节点的世界矩阵 —— 蒙皮公式要把关节换到
// mesh 节点空间（invMeshNode），这样 push.model 可以原样乘上去，
// 渲染代码无需感知节点树。
std::shared_ptr<Skeleton> parseSkeleton(const tinygltf::Model& gm,
                                        int skinIdx,
                                        const glm::mat4& meshNodeWorld) {
    if (skinIdx < 0 || skinIdx >= static_cast<int>(gm.skins.size()))
        return nullptr;
    const auto& skin = gm.skins[skinIdx];

    auto sk = std::make_shared<Skeleton>();
    const int n = static_cast<int>(skin.joints.size());
    sk->joints.resize(n);

    // node 下标 → joint 下标
    for (int j = 0; j < n; ++j)
        sk->nodeMap[skin.joints[j]] = j;
    auto isJoint = [&](int node) {
        return sk->nodeMap.find(node) != sk->nodeMap.end();
    };

    // 父链查找表：node → 其在场景里的父 node（buildParentMap 一次）
    std::unordered_map<int, int> parentOf;
    std::function<void(int, int)> walkParents = [&](int node, int parent) {
        parentOf[node] = parent;
        if (node >= 0 && node < static_cast<int>(gm.nodes.size()))
            for (int c : gm.nodes[node].children) walkParents(c, node);
    };
    if (gm.defaultScene >= 0 &&
        gm.defaultScene < static_cast<int>(gm.scenes.size())) {
        for (int r : gm.scenes[gm.defaultScene].nodes) walkParents(r, -1);
    } else {
        for (size_t r = 0; r < gm.nodes.size(); ++r) walkParents((int)r, -1);
    }

    // 场景根 → node 的静止世界矩阵（供 externalChain 用）
    std::unordered_map<int, glm::mat4> nodeWorld;
    std::function<glm::mat4(int)> worldOf = [&](int node) -> glm::mat4 {
        auto it = nodeWorld.find(node);
        if (it != nodeWorld.end()) return it->second;
        const glm::mat4 local =
            node < 0 || node >= static_cast<int>(gm.nodes.size())
                ? glm::mat4(1.0f)
                : nodeLocalMatrix(gm.nodes[node]);
        const glm::mat4 w = parentOf.count(node) && parentOf[node] >= 0
                                ? worldOf(parentOf[node]) * local
                                : local;
        nodeWorld[node] = w;
        return w;
    };

    for (int j = 0; j < n; ++j) {
        const int node = skin.joints[j];
        auto& J = sk->joints[j];
        if (node >= 0 && node < static_cast<int>(gm.nodes.size())) {
            J.name = gm.nodes[node].name;
            J.restLocal = nodeLocalMatrix(gm.nodes[node]);
        }
        const int p = parentOf.count(node) ? parentOf[node] : -1;
        if (p >= 0 && isJoint(p)) {
            J.parent = sk->nodeMap[p];
        } else {
            // 骨架外的祖先链（armature 节点等）整体作为 externalChain
            J.parent = -1;
            J.externalChain = p >= 0 ? worldOf(p) : glm::mat4(1.0f);
        }
    }

    // inverseBindMatrices：MAT4 float accessor
    if (skin.inverseBindMatrices >= 0) {
        AccessorView v;
        if (accessorView(gm, skin.inverseBindMatrices, v)) {
            const size_t cnt = std::min(v.count, static_cast<size_t>(n));
            for (size_t j = 0; j < cnt; ++j) {
                const uint8_t* p = v.data + j * v.stride;
                glm::mat4 m(1.0f);
                // glTF 列主序 = glm 内存布局，直接拷
                std::memcpy(&m, p, sizeof(glm::mat4));
                sk->joints[j].invBind = m;
            }
        }
    }

    // 拓扑序：保证 sample 时 parent 先于 child 被处理
    sk->topoOrder.clear();
    sk->topoOrder.reserve(n);
    std::vector<bool> done(n, false);
    for (int pass = 0; pass < n; ++pass) {
        bool progressed = false;
        for (int j = 0; j < n; ++j) {
            if (done[j]) continue;
            if (sk->joints[j].parent < 0 || done[sk->joints[j].parent]) {
                done[j] = true;
                sk->topoOrder.push_back(j);
                progressed = true;
            }
        }
        if (!progressed) break;   // 环（异常数据），剩下的按原序兜底
    }
    for (int j = 0; j < n; ++j)
        if (!done[j]) sk->topoOrder.push_back(j);

    sk->invMeshNode = glm::inverse(meshNodeWorld);
    return sk;
}

// glTF animations → Skeleton::clips（轨道按 node→joint 映射）
void parseAnimations(const tinygltf::Model& gm, Skeleton& sk) {
    for (const auto& ganim : gm.animations) {
        AnimationClip clip;
        clip.name = ganim.name.empty() ? ("clip" + std::to_string(sk.clips.size()))
                                       : ganim.name;
        for (const auto& ch : ganim.channels) {
            const int joint = sk.nodeToJoint(ch.target_node);
            if (joint < 0) continue;   // 非骨架节点（网格/object 层动画）
            if (ch.sampler < 0 ||
                ch.sampler >= static_cast<int>(ganim.samplers.size()))
                continue;
            const auto& gs = ganim.samplers[ch.sampler];

            AnimTrack t;
            t.joint = joint;
            if (ch.target_path == "translation")
                t.path = AnimTrack::Translation;
            else if (ch.target_path == "rotation")
                t.path = AnimTrack::Rotation;
            else if (ch.target_path == "scale")
                t.path = AnimTrack::Scale;
            else
                continue;   // weights（形变动画）暂不支持

            AccessorView tin, tout;
            if (!accessorView(gm, gs.input, tin)) continue;
            if (!accessorView(gm, gs.output, tout)) continue;
            t.times.resize(tin.count);
            for (size_t i = 0; i < tin.count; ++i)
                t.times[i] = readFloat(tin.data + i * tin.stride);
            t.values.resize(tout.count);
            for (size_t i = 0; i < tout.count; ++i)
                t.values[i] = readVec4(tout, i);
            t.step = (gs.interpolation == "STEP");
            // CUBICSPLINE 的输出是 3 倍密度（in/out tangent），本引擎
            // 的角色只出 LINEAR/STEP；遇到三次样条按线性近似。
            if (gs.interpolation == "CUBICSPLINE" && tout.count == tin.count * 3) {
                std::vector<glm::vec4> simplified(tin.count);
                for (size_t i = 0; i < tin.count; ++i)
                    simplified[i] = t.values[i * 3 + 1];   // 取节点值段
                t.values = std::move(simplified);
            }

            if (!t.times.empty())
                clip.duration = std::max(clip.duration, t.times.back());
            clip.tracks.push_back(std::move(t));
        }
        if (!clip.tracks.empty()) sk.clips.push_back(std::move(clip));
    }
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

    // W2-2：导入设置 sidecar。读它 → 判定"源变没变 / 参数被改没被改" →
    // 材质 id 从它的映射里取（重导出换序不串味），最后按需写回。
    Vmeta vmeta = beginModelImport(name);

    // glTF 规范是 **Y-up**；引擎世界是 **Z-up** 右手系。
    // 在节点树根部预乘一个"绕 X +90°"的旋转：(x,y,z) → (x,-z,y)，
    // glTF 的 +Y(up)/+Z(forward) 就变成引擎的 +Z(up)/-Y(forward)。
    // 旋转保持手性，法线与绕序无需特殊处理；子节点的局部 TRS 也随之
    // 一并转换（decompose 出来的还是正确的 Z-up 姿态）。
    glm::mat4 rootXform =
        glm::rotate(glm::mat4(1.0f), 1.57079637f, glm::vec3(1, 0, 0));
    // W2-2：导入缩放乘在**根**上 —— 位移、骨骼、包围盒一起缩放，不会出现
    // "模型缩小了但骨架没缩"这种错位。默认 1.0 是单位阵，逐位等于原行为。
    if (vmeta.import.scale > 0.0f && vmeta.import.scale != 1.0f) {
        rootXform = glm::scale(glm::mat4(1.0f),
                               glm::vec3(vmeta.import.scale)) * rootXform;
    }
    const glm::mat4& kYupToZup = rootXform;

    glm::vec3 bmin(1e30f), bmax(-1e30f);

    // 第一个带 skin 的 mesh 节点的世界矩阵（含 Y-up→Z-up 根旋转），
    // 供 parseSkeleton 计算 invMeshNode 用。
    glm::mat4 skinnedMeshWorld(1.0f);
    int skinnedSkinIdx = -1;
    bool foundSkin = false;

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

                // 记录蒙皮 mesh 节点（骨架挂在第一个带 skin 的节点上）
                if (!foundSkin && node.skin >= 0) {
                    foundSkin = true;
                    skinnedMeshWorld = world;
                    skinnedSkinIdx = node.skin;
                }

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

                    // ---- 蒙皮：JOINTS_0 / WEIGHTS_0 ----
                    // 只有这个 primitive 属于带 skin 的 mesh 时才读。
                    // JOINTS_0 存的是 glTF skin 里的 joint 下标（0..N-1），
                    // 与 Skeleton::joints 对齐；渲染时加 PushConstants 的
                    // jointBase 定位到 SSBO。权重和 > 0 才走蒙皮分支。
                    if (node.skin >= 0) {
                        auto itJ = prim.attributes.find("JOINTS_0");
                        auto itW = prim.attributes.find("WEIGHTS_0");
                        AccessorView aJ, aW;
                        const bool hasJ = itJ != prim.attributes.end() &&
                                          accessorView(gm, itJ->second, aJ);
                        const bool hasW = itW != prim.attributes.end() &&
                                          accessorView(gm, itW->second, aW);
                        if (hasJ && hasW) {
                            for (size_t i = 0; i < aPos.count; ++i) {
                                verts[i].joints = glm::uvec4(
                                    readJointAt(aJ, i, 0),
                                    aJ.comps > 1 ? readJointAt(aJ, i, 1) : 0,
                                    aJ.comps > 2 ? readJointAt(aJ, i, 2) : 0,
                                    aJ.comps > 3 ? readJointAt(aJ, i, 3) : 0);
                                verts[i].weights = readVec4(aW, i);
                            }
                        }
                    }

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
                        // W2-2：generateLods=false → 这条网格不建 LOD 链
                        sm.mesh->setNoLod(!vmeta.import.generateLods);
                    }
                    if (prim.material >= 0 &&
                        prim.material < static_cast<int>(gm.materials.size())) {
                        sm.material =
                            gltfMaterial(assets, gm, prim.material, name,
                                         &vmeta);
                    }
                    model->subMeshes.push_back(std::move(sm));
                }
            }

            for (int c : node.children) walk(c, world, depth + 1);
        };

    if (gm.defaultScene >= 0 &&
        gm.defaultScene < static_cast<int>(gm.scenes.size())) {
        for (int n : gm.scenes[gm.defaultScene].nodes)
            walk(n, kYupToZup, 0);
    } else {
        for (size_t n = 0; n < gm.nodes.size(); ++n)
            walk(static_cast<int>(n), kYupToZup, 0);
    }

    // ---- 蒙皮骨架 + 动画 ----
    if (foundSkin) {
        model->skeleton = parseSkeleton(gm, skinnedSkinIdx, skinnedMeshWorld);
        if (model->skeleton) {
            parseAnimations(gm, *model->skeleton);
            VK_LOG_INFO("glTF: skinned model, %d joints, %d clips",
                        model->skeleton->jointCount(),
                        static_cast<int>(model->skeleton->clips.size()));
        }
    }

    if (bmin.x <= bmax.x) {
        model->center = (bmin + bmax) * 0.5f;
        model->radius = glm::length(bmax - bmin) * 0.5f;
    }

    // W2-2：把本次见到的材质名补进映射并写回 sidecar（内容没变就不写）
    finishModelImport(name, vmeta);

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

Model* AssetManager::loadFBX(const std::string& path,
                             const std::string& name) {
    const std::string key = name.empty() ? path : name;
    auto it = m_models.find(key);
    if (it != m_models.end()) return it->second.get();

    // ---- 优先走烤好的 .vmesh（2026-10-22 离线资产链）----
    //
    // 目录约定：`models/girl.fbx` 的缓存是**同名**的 `models/girl.vmesh`。
    // 用 sidecar 而不是"换一种资产格式"有三个好处：
    //   1. 场景文件里引用的永远是 .fbx，老存档一行都不用改；
    //   2. 缓存删掉就自动退回 ufbx 解析，不会"资产缺失"；
    //   3. 源文件比缓存新 → 自动走 FBX，避免"改了源、画面没变"。
    //
    // ⚠ 这里**不检查** .vmeta 的 import 参数是否变过：那些参数已经烘进
    //   .vmesh 了，改了必须重新跑 AssetC。判据只能是 mtime，因为
    //   importSignature 存在 sidecar 里、而 vmesh 里没有 —— 想判就得
    //   把签名写进 vmesh，那是格式 v2 的事。
    // ---- 情况 0：传进来的**本身就是 .vmesh**（引擎自带的烤制资产）----
    //
    // ⚠ 2026-10-22 实测的坑：默认角色资产是 `default_character.vmesh`，
    //   场景里没有同名 FBX。`vmeshPathFor()` 把它当成"源"，于是
    //   `vmeshIsFresh(src, bin)` 里 src == bin；而 `.vmeta` 是**加载成功之后**
    //   才写的（finishModelImport），它的 mtime 永远比 .vmesh 新 →
    //   下一条判据立刻判"缓存过期" → 回退去按 FBX 解析一个 .vmesh →
    //   `Failed to load FBX: ...vmesh`，抛出后再也加载不出来。
    //
    //   症状极具迷惑性：**第一次一切正常，第二次起永远失败**（把 .vmeta
    //   删掉又好一次）。所以这里直接短路：源就是烤制资产，不经任何
    //   "新鲜度"判断 —— 它没有可比较的源文件。
    if (assetio::vmeshPathFor(path) == path) {
        if (auto model = loadVMeshModel(*m_ctx.device, *m_ctx.cmdPool, m_ctx,
                                        *this, path, key,
                                        makeAssetRelative(path))) {
            Model* raw = model.get();
            m_models[key] = std::move(model);
            return raw;
        }
        VK_LOG_ERROR("VMESH 直接加载失败 key=%s path=%s",
                     assetLogSafe(key).c_str(), path.c_str());
        return nullptr;
    }

    const std::string binPath = assetio::vmeshPathFor(path);
    if (vmeshIsFresh(path, binPath)) {
        if (auto model = loadVMeshModel(*m_ctx.device, *m_ctx.cmdPool, m_ctx,
                                        *this, binPath, key,
                                        makeAssetRelative(path))) {
            Model* raw = model.get();
            m_models[key] = std::move(model);
            return raw;
        }
        // 读失败（文件损坏 / 版本不兼容）→ 落到下面走 FBX。
        // 打 WARN 而不是 ERROR：这是"缓存坏了但能自愈"，不是事故。
        VK_LOG_WARN("MESH-BIN-FALLBACK key=%s bin=%s (parse failed, using FBX)",
                    assetLogSafe(key).c_str(), binPath.c_str());
    } else if (std::filesystem::exists(binPath)) {
        VK_LOG_INFO("MESH-BIN-STALE key=%s（源比缓存新，回退 FBX 解析）",
                    assetLogSafe(key).c_str());
    }

    auto model = loadFBXModel(*m_ctx.device, *m_ctx.cmdPool, m_ctx, *this,
                              path, key);
    Model* raw = model.get();
    m_models[key] = std::move(model);
    return raw;
}

} // namespace assets
