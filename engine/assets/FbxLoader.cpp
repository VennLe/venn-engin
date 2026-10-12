// ============================================================
// assets/FbxLoader —— FBX 模型加载（基于 ufbx 单头库）
//
// 与 glTF 加载器（MeshLoader.cpp 的 loadGLTFModel）产出**同一套**产物：
//   * assets::Model —— 若干 SubMesh（网格 + 材质 + 世界变换）
//   * assets::Skeleton —— 蒙皮骨骼 + 动画片段（无骨骼 = 静态网格体）
//
// 这正好对上 UE5 的"静态网格体（StaticMesh）"与"骨骼网格体（SkeletalMesh）"
// 两分法：
//   * FBX 里 mesh 无 skin_deformer → 静态网格体（skeleton 为空）
//   * FBX 里 mesh 带 skin_deformer → 骨骼网格体（带 Skeleton + clips）
//
// 坐标约定：让 ufbx 用 target_axes = right_handed_z_up 直接把 FBX 的
// Y-up 转成引擎的 Z-up，node_to_world / geometry_to_world / 蒙皮矩阵 /
// 动画采样全部自动落在引擎坐标系里，无需再手动乘 kYupToZup。
//
// 蒙皮公式（与 glTF 路径一致，见 Skeleton.h）：
//   jointMat(j) = invMeshNode * world(j) * invBind(j)
// 顶点着色器里 Σ w·jointMat(j)·pos。蒙皮网格的顶点在加载时被**烘到
// 静止世界空间**（引擎坐标系、米），invBind = inv(W_rest)、invMeshNode
// 取单位阵 —— 静止时 jointMat = I，蒙皮输出仍在静止世界空间，实体
// 变换（SubMesh::transform = T(-center)）只承担归位平移。
// ============================================================

#include "assets/MeshLoader.h"
#include "assets/AssetManager.h"
#include "assets/AssetPath.h"
#include "rhi/Device.h"
#include "rhi/CommandPool.h"

#include "core/Logger.h"

#include <cstring>
#include <functional>
#include <map>
#include <unordered_map>

#include <glm/gtx/matrix_decompose.hpp>

// ufbx 双文件库（ufbx.h 声明 + ufbx.c 实现）。实现单独作为
// third_party/ufbx/ufbx.c 编译进引擎库，这里只 include 头文件。
#include <ufbx.h>

namespace assets {

namespace {

// ---- FBX 单位换算：ufbx 输出的世界矩阵已经按 target_unit_meters 缩放，
// 这里固定成米，与引擎/glTF 一致。----

// ufbx 的 ufbx_matrix 是**列主序 3x4**（12 个 float，见 ufbx.h）：
//   cols[0] = (m00,m10,m20)  cols[1] = (m01,m11,m21)
//   cols[2] = (m02,m12,m22)  cols[3] = (m03,m13,m23)  ← 平移
// 这与 glm::mat4 的列主序布局一致，但 ufbx 只有 12 个 float，
// 不能直接 memcpy（会越界读 4 个 float，产生垃圾平移值）。
// 这里显式按列填入 glm::mat4，末行补 (0,0,0,1)。
glm::mat4 toGlm(const ufbx_matrix& m) {
    glm::mat4 r(1.0f);
    r[0] = glm::vec4(m.m00, m.m10, m.m20, 0.0f);
    r[1] = glm::vec4(m.m01, m.m11, m.m21, 0.0f);
    r[2] = glm::vec4(m.m02, m.m12, m.m22, 0.0f);
    r[3] = glm::vec4(m.m03, m.m13, m.m23, 1.0f);
    return r;
}

glm::vec3 toGlm(const ufbx_vec3& v) { return {v.x, v.y, v.z}; }
glm::vec2 toGlm(const ufbx_vec2& v) { return {v.x, v.y}; }

// ---- 从一个 mesh 的所有 primitive 里读顶点，转成引擎 Vertex ----
// 返回值：是否成功。每个 primitive 独立一个 SubMesh（材质可能不同）。
// skin 非空时读取蒙皮权重（JOINTS_0/WEIGHTS_0 语义）。
struct PrimResult {
    std::vector<Vertex> verts;
    std::vector<uint32_t> indices;
    int materialIndex = -1;
};

// ufbx 数据模型（见 ufbx.h 顶部 mesh 图解）：
//   * index（0..num_indices-1）：每个面角的唯一元组（位置+法线+UV）
//   * vertex（0..num_vertices-1）：逻辑顶点（位置去重，蒙皮权重按它）
//   * faces[f].index_begin → 指向 index 数组
//   * vertex_position.indices[index] → 位置 value 下标
//   * vertex_indices[index] → 该 index 对应的逻辑顶点
bool extractMesh(const ufbx_mesh& mesh, const ufbx_skin_deformer* skin,
                 std::vector<PrimResult>& out) {
    if (mesh.num_faces == 0 || mesh.num_indices == 0) return false;

    // 面 → 三角形索引（index 编号）。扇形三角剖分。
    std::vector<uint32_t> tris;
    tris.reserve(mesh.num_triangles * 3);
    // 记录每个三角形的材质（取该面第一个顶点的 face_material）
    std::vector<int> triMaterial;
    triMaterial.reserve(mesh.num_triangles);
    for (size_t f = 0; f < mesh.num_faces; ++f) {
        const ufbx_face face = mesh.faces.data[f];
        if (face.num_indices < 3) continue;
        const int mat = mesh.face_material.count
                            ? (int)mesh.face_material.data[f]
                            : -1;
        for (uint32_t i = 1; i + 1 < face.num_indices; ++i) {
            tris.push_back(face.index_begin + 0);
            tris.push_back(face.index_begin + i);
            tris.push_back(face.index_begin + i + 1);
            triMaterial.push_back(mat);
        }
    }
    if (tris.empty()) return false;

    // 构建顶点数组：按 index 遍历（每个 index 一个 Vertex）。
    const size_t n = mesh.num_indices;
    std::vector<Vertex> verts(n);
    const bool hasNormal = mesh.vertex_normal.exists;
    const bool hasUv = mesh.vertex_uv.exists;
    const bool hasSkin = skin != nullptr;

    for (size_t i = 0; i < n; ++i) {
        Vertex& v = verts[i];
        v.pos = toGlm(ufbx_get_vertex_vec3(&mesh.vertex_position, i));
        if (hasNormal)
            v.normal = toGlm(ufbx_get_vertex_vec3(&mesh.vertex_normal, i));
        if (hasUv) {
            v.uv = toGlm(ufbx_get_vertex_vec2(&mesh.vertex_uv, i));
            v.uv.y = 1.0f - v.uv.y;   // FBX UV 原点在左下，翻到左上
        }
        if (hasSkin) {
            const uint32_t logical = mesh.vertex_indices.data[i];
            if (logical < skin->vertices.count) {
                const ufbx_skin_vertex sv = skin->vertices.data[logical];
                uint32_t idx[4] = {0, 0, 0, 0};
                float w[4] = {0, 0, 0, 0};
                const uint32_t nw = sv.num_weights < 4 ? sv.num_weights : 4;
                for (uint32_t k = 0; k < nw; ++k) {
                    const ufbx_skin_weight sw =
                        skin->weights.data[sv.weight_begin + k];
                    idx[k] = sw.cluster_index;
                    w[k] = (float)sw.weight;
                }
                v.joints = glm::uvec4(idx[0], idx[1], idx[2], idx[3]);
                v.weights = glm::vec4(w[0], w[1], w[2], w[3]);
            }
        }
    }

    // 按材质分组：收集用到的材质下标集合，按材质把三角形分桶。
    // 无材质 → 单个 primitive（materialIndex=-1）。
    std::map<int, std::vector<uint32_t>> buckets;   // 材质 → 三角形索引
    for (size_t t = 0; t < tris.size() / 3; ++t)
        buckets[triMaterial[t]].push_back((uint32_t)t);

    if (buckets.size() == 1) {
        PrimResult pr;
        pr.verts = std::move(verts);
        pr.indices = std::move(tris);
        pr.materialIndex = buckets.begin()->first;
        out.push_back(std::move(pr));
        return true;
    }

    // 多材质：每个桶独立 primitive，索引重映射到局部顶点。
    for (const auto& kv : buckets) {
        std::vector<uint32_t> pi;
        pi.reserve(kv.second.size() * 3);
        std::unordered_map<uint32_t, uint32_t> remap;
        for (uint32_t tri : kv.second) {
            for (int c = 0; c < 3; ++c) {
                const uint32_t gi = tris[tri * 3 + c];
                auto it = remap.find(gi);
                if (it == remap.end()) {
                    const uint32_t local = (uint32_t)remap.size();
                    remap[gi] = local;
                    pi.push_back(local);
                } else {
                    pi.push_back(it->second);
                }
            }
        }
        // 重映射顶点：只有被引用的顶点才保留
        std::vector<Vertex> pv(remap.size());
        for (const auto& kv2 : remap) pv[kv2.second] = verts[kv2.first];

        PrimResult pr;
        pr.verts = std::move(pv);
        pr.indices = std::move(pi);
        pr.materialIndex = kv.first;
        out.push_back(std::move(pr));
    }
    return true;
}

// ---- FBX 材质 → 引擎 PBR 材质（尽力而为）----
// ufbx_material 提供 pbr.base_color / roughness / metalness 等 map。
// 每个 map 可能是纯因子（value_*）或贴图（texture）。
Material* fbxMaterial(AssetManager& assets, const ufbx_material& mat,
                      const std::string& modelName) {
    glm::vec4 baseColor(1.0f);
    float roughness = 0.5f, metallic = 0.0f;
    glm::vec3 emissive(0.0f);

    // base_color：优先 PBR，回退 FBX diffuse_color
    const ufbx_material_map& bc = mat.pbr.base_color;
    if (bc.has_value) {
        if (bc.value_components >= 3)
            baseColor = {bc.value_vec3.x, bc.value_vec3.y, bc.value_vec3.z, 1.0f};
        if (bc.value_components >= 4)
            baseColor.a = bc.value_vec4.w;
    } else if (mat.fbx.diffuse_color.has_value) {
        const ufbx_material_map& dc = mat.fbx.diffuse_color;
        baseColor = {dc.value_vec3.x, dc.value_vec3.y, dc.value_vec3.z, 1.0f};
    }

    if (mat.pbr.roughness.has_value)
        roughness = (float)mat.pbr.roughness.value_real;
    if (mat.pbr.metalness.has_value)
        metallic = (float)mat.pbr.metalness.value_real;
    if (mat.pbr.emission_color.has_value)
        emissive = {(float)mat.pbr.emission_color.value_vec3.x,
                    (float)mat.pbr.emission_color.value_vec3.y,
                    (float)mat.pbr.emission_color.value_vec3.z};

    // albedo 贴图：pbr.base_color.texture 优先，回退 fbx.diffuse_color.texture
    Texture* albedo = nullptr;
    const ufbx_texture* tex = bc.texture;
    if (!tex) tex = mat.fbx.diffuse_color.texture;
    if (tex && tex->type == UFBX_TEXTURE_FILE && tex->filename.length) {
        const std::string fp(tex->filename.data, tex->filename.length);
        albedo = assets.loadTexture(fp, modelName + "/albedo", true);
    }

    return assets.makeMaterialPBR(modelName + "/mat", albedo, nullptr, nullptr,
                                  baseColor, roughness, metallic, emissive,
                                  false);
}

// ---- 骨骼（skin_deformer → Skeleton）----
// 每个 cluster 对应一根骨骼；cluster->bone_node 是骨骼节点。
// cluster->geometry_to_bone 是"mesh 局部空间 → 骨骼空间"的逆绑定矩阵。
std::shared_ptr<Skeleton> buildSkeleton(const ufbx_skin_deformer& skin,
                                        const glm::mat4& meshNodeWorld) {
    // invMeshNode = inverse(geometry_to_world)：
    //   静止姿势下 world(j) * invBind(j) = geometry_to_world，于是
    //   jointMat(j) = inv(G2W) * G2W * v = v —— 蒙皮输出顶点落在
    //   **geometry 空间**，与静态网格路径一致：push.model（实体变换，
    //   即 SubMesh::transform = T(-center)·G2W）把它变换到世界。
    //   这正是 glTF 路径的"meshNodeWorld 恰好约掉"惯例（见 Skeleton.h）。
    //   若 invMeshNode 取单位阵，push.model 会把 G2W 乘两遍 —— 模型
    //   会被二次缩放/移位（预览视口里只剩一个像素点）。

    auto sk = std::make_shared<Skeleton>();
    const int n = (int)skin.clusters.count;
    sk->joints.resize(n);

    // cluster 下标 → joint 下标（就是顺序）
    // bone_node 的父链决定 joint 的 parent 关系。
    // restLocal 从 **node_to_world** 推导（而不是 node_to_parent）：
    //   ufbx 把轴/单位换算烘焙进 node_to_world，但 root 的 node_to_parent
    //   可能不含这些调整 —— 直接用 node_to_parent 链会得到与 invBind 不一致
    //   的静止姿势（模型蜷成一团）。用 node_to_world 保证静止时
    //   world_rest(j) * invBind(j) = geometry_to_world 严格成立。
    std::vector<glm::mat4> W(static_cast<size_t>(n), glm::mat4(1.0f));
    for (int j = 0; j < n; ++j) {
        const ufbx_skin_cluster* cl = skin.clusters.data[j];
        const ufbx_node* bone = cl->bone_node;
        auto& J = sk->joints[j];
        J.name = bone && bone->name.length
                     ? std::string(bone->name.data, bone->name.length)
                     : ("bone" + std::to_string(j));
        J.parent = -1;
        if (bone) W[static_cast<size_t>(j)] = toGlm(bone->node_to_world);
        // invBind = inv(W_rest)：顶点在加载时已烘到"静止世界空间"（米，
        // 引擎坐标系，见 walkNode 里的烘焙），静止时 jointMat = W·inv(W)
        // = I。注意不能用 ufbx 的 cluster->geometry_to_bone —— 它是
        // **文件原始空间**（cm / Y-up），而 node_to_world 已被 ufbx
        // 换算成引擎空间，两者不可直接组合（这正是模型蜷成一团的根因）。
        J.invBind = glm::inverse(W[static_cast<size_t>(j)]);
    }

    // 建立 node 指针 → joint 下标的映射，再沿 bone_node 的父链找 parent
    std::unordered_map<const ufbx_node*, int> nodeToJoint;
    for (int j = 0; j < n; ++j) {
        const ufbx_node* bone = skin.clusters.data[j]->bone_node;
        if (bone) nodeToJoint[bone] = j;
    }
    for (int j = 0; j < n; ++j) {
        const ufbx_node* bone = skin.clusters.data[j]->bone_node;
        if (!bone) continue;
        const ufbx_node* p = bone->parent;
        // 沿父链找最近的"也是骨骼"的祖先
        while (p) {
            auto it = nodeToJoint.find(p);
            if (it != nodeToJoint.end()) {
                sk->joints[j].parent = it->second;
                break;
            }
            p = p->parent;
        }
        // joint 0 也是合法的父节点 —— 只排除自引用
        if (sk->joints[j].parent == j)
            sk->joints[j].parent = -1;
    }

    // restLocal：由最近的骨骼祖先（或根）的 node_to_world 推导。
    // restLocal = inv(W_parent) * W_self —— 中间的非骨骼节点变换也被
    // 烘进这一段局部变换里，静止姿势与文件里的 world 完全一致。
    for (int j = 0; j < n; ++j) {
        const int p = sk->joints[static_cast<size_t>(j)].parent;
        if (p >= 0)
            sk->joints[static_cast<size_t>(j)].restLocal =
                glm::inverse(W[static_cast<size_t>(p)]) *
                W[static_cast<size_t>(j)];
        else
            sk->joints[static_cast<size_t>(j)].restLocal =
                W[static_cast<size_t>(j)];
    }

    // 拓扑序（parent 先于 child）
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
        if (!progressed) break;
    }
    for (int j = 0; j < n; ++j)
        if (!done[j]) sk->topoOrder.push_back(j);

    // ufbx 顶点在加载时已烘到"静止世界空间"（见 walkNode），蒙皮输出也
    // 落在这个空间：静止时 jointMat = W·invBind = I（invBind = inv(W_rest)）。
    // push.model（实体变换 = T(-center)）负责归位平移。
    sk->invMeshNode = glm::mat4(1.0f);

    // 诊断：静止姿势一致性。jointMat(rest) = W(j) * invBind(j) 应为单位阵。
    {
        float maxErr = 0.0f;
        for (int j = 0; j < n; ++j) {
            const glm::mat4 E =
                W[static_cast<size_t>(j)] *
                sk->joints[static_cast<size_t>(j)].invBind;
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    maxErr = glm::max(
                        maxErr, glm::abs(E[r][c] - (r == c ? 1.0f : 0.0f)));
        }
        const glm::vec3 rootT = glm::vec3(W[0][3]);
        VK_LOG_INFO(
            "FBX skin dbg: joints=%d maxRestErr=%.6f rootW.t=(%.3f,%.3f,%.3f) "
            "G2W.t=(%.3f,%.3f,%.3f)",
            n, maxErr, rootT.x, rootT.y, rootT.z, meshNodeWorld[3][0],
            meshNodeWorld[3][1], meshNodeWorld[3][2]);
    }
    return sk;
}

// ---- 动画：anim_stack → AnimationClip（逐帧采样骨骼节点 TRS）----
// 采样率固定 30fps（引擎角色动画足够），每根骨骼一条 T/R/S 轨道。
// 与 glTF 的 AnimTrack 结构对齐，Skeleton::sample 直接消费。
// 注意：采样需要 bone 的 **node 指针**（ufbx_evaluate_transform 作用在
// node 上），所以这里直接接收 skin_deformer 来拿 clusters 的 bone_node，
// 不再依赖 Skeleton 保存指针。
void extractAnimations(const ufbx_scene& scene, const ufbx_skin_deformer& skin,
                       Skeleton& sk) {
    const float kFps = 30.0f;

    // joint 下标 → bone node 指针
    const int nJoints = sk.jointCount();
    std::vector<const ufbx_node*> boneNode(nJoints, nullptr);
    for (int j = 0; j < nJoints && j < (int)skin.clusters.count; ++j)
        boneNode[j] = skin.clusters.data[j]->bone_node;

    // 骨骼节点集合：用于判断某 bone 的直接父节点是不是"骨骼"。
    // ufbx_evaluate_transform 返回的是**相对直接父节点**的局部变换；
    // 若父节点不是骨骼（如 Blender 的 Armature 对象），要把该父链的
    // 静止世界变换 P 乘进去，使采样出的局部姿势与 restLocal（由
    // node_to_world 推导）处于同一空间约定。
    std::unordered_map<const ufbx_node*, int> boneSet;
    for (int j = 0; j < nJoints; ++j)
        if (boneNode[j]) boneSet[boneNode[j]] = j;

    for (size_t s = 0; s < scene.anim_stacks.count; ++s) {
        const ufbx_anim_stack* stackPtr = scene.anim_stacks.data[s];
        if (!stackPtr || !stackPtr->anim) continue;
        const ufbx_anim_stack& stack = *stackPtr;

        AnimationClip clip;
        clip.name = stack.name.length
                        ? std::string(stack.name.data, stack.name.length)
                        : ("clip" + std::to_string(s));
        const double t0 = stack.time_begin;
        const double t1 = stack.time_end;
        const double dur = (t1 > t0) ? (t1 - t0) : 0.0;
        clip.duration = (float)dur;
        if (dur <= 0.0) continue;

        const int frames = (int)std::ceil(dur * kFps) + 1;

        // 每根骨骼预分配三条轨道（T/R/S）
        std::vector<AnimTrack> trk(nJoints * 3);
        for (int j = 0; j < nJoints; ++j) {
            trk[j * 3 + 0].joint = j;
            trk[j * 3 + 0].path = AnimTrack::Translation;
            trk[j * 3 + 1].joint = j;
            trk[j * 3 + 1].path = AnimTrack::Rotation;
            trk[j * 3 + 2].joint = j;
            trk[j * 3 + 2].path = AnimTrack::Scale;
        }

        // 逐帧采样
        for (int f = 0; f < frames; ++f) {
            const double t = t0 + (double)f / kFps;
            const float ft = (float)(f) / kFps;   // 相对动画起点（秒）
            for (int j = 0; j < nJoints; ++j) {
                const ufbx_node* bn = boneNode[j];
                if (!bn) {
                    // 无 node 的骨骼：用 rest 值填
                    trk[j * 3 + 0].times.push_back(ft);
                    trk[j * 3 + 0].values.push_back(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
                    trk[j * 3 + 1].times.push_back(ft);
                    trk[j * 3 + 1].values.push_back(
                        glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
                    trk[j * 3 + 2].times.push_back(ft);
                    trk[j * 3 + 2].values.push_back(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
                    continue;
                }
                // 求这一帧该 bone 的**局部**变换（相对父，动画采样）。
                // ufbx_evaluate_transform 返回的 local transform 已经
                // 是"在 target_axes 坐标系下"的（因为它内部应用了轴转换）。
                const ufbx_transform lt =
                    ufbx_evaluate_transform(stack.anim, bn, t);

                // 局部姿势空间修正：父节点不是骨骼时，采样值要乘父链
                // 的静止世界变换 P（否则静止姿势对不上，模型蜷缩）。
                glm::vec3 tr = toGlm(lt.translation);
                glm::quat rot(lt.rotation.w, lt.rotation.x, lt.rotation.y,
                              lt.rotation.z);
                glm::vec3 sc = toGlm(lt.scale);
                const ufbx_node* pn = bn->parent;
                const bool parentIsBone =
                    pn && boneSet.find(pn) != boneSet.end();
                if (!parentIsBone) {
                    const glm::mat4 P = pn ? toGlm(pn->node_to_world)
                                           : glm::mat4(1.0f);
                    const glm::mat4 M =
                        P * glm::translate(glm::mat4(1.0f), tr) *
                        glm::mat4_cast(rot) * glm::scale(glm::mat4(1.0f), sc);
                    glm::vec3 s2, t2, sk2(0.0f);
                    glm::quat r2(1.0f, 0.0f, 0.0f, 0.0f);
                    glm::vec4 persp(0.0f);
                    glm::decompose(M, s2, r2, t2, sk2, persp);
                    tr = t2;
                    rot = r2;
                    sc = s2;
                }

                trk[j * 3 + 0].times.push_back(ft);
                trk[j * 3 + 0].values.push_back(glm::vec4(tr, 1.0f));
                trk[j * 3 + 1].times.push_back(ft);
                trk[j * 3 + 1].values.push_back(
                    glm::vec4(rot.x, rot.y, rot.z, rot.w));
                trk[j * 3 + 2].times.push_back(ft);
                trk[j * 3 + 2].values.push_back(glm::vec4(sc, 1.0f));
            }
        }

        // 收进 clip（跳过没有实际采样的空 clip）
        for (auto& t : trk)
            if (!t.times.empty()) clip.tracks.push_back(std::move(t));
        if (!clip.tracks.empty()) sk.clips.push_back(std::move(clip));
    }
}

} // namespace

// ============================================================
// 公开入口
// ============================================================
std::unique_ptr<Model> loadFBXModel(rhi::Device& device,
                                    rhi::CommandPool& cmdPool,
                                    const TextureContext& ctx,
                                    AssetManager& assets,
                                    const std::string& path,
                                    const std::string& name) {
    (void)ctx;

    ufbx_load_opts opts = {};   // 全零 = 默认
    opts.target_axes = ufbx_axes_right_handed_z_up;   // Y-up → Z-up
    opts.target_unit_meters = 1.0f;                    // 单位：米
    opts.generate_missing_normals = true;
    opts.evaluate_skinning = false;   // 自己算蒙皮（走 Skeleton 路径）
    opts.clean_skin_weights = true;
    opts.ignore_missing_external_files = true;

    ufbx_error error;
    ufbx_scene* scene = ufbx_load_file(path.c_str(), &opts, &error);
    if (!scene) {
        VK_LOG_ERROR("FBX load failed: %s (%s)", path.c_str(),
                     error.description.data ? error.description.data : "?");
        throw std::runtime_error("Failed to load FBX: " + path);
    }

    auto model = std::make_unique<Model>();
    model->name = name;

    // 诊断：文件**声明的**坐标系与单位（target_axes 换算的依据）
    {
        const ufbx_coordinate_axes& ax = scene->settings.axes;
        VK_LOG_INFO("FBX axes: declared right=%d up=%d front=%d "
                    "unit_meters=%.4f",
                    (int)ax.right, (int)ax.up, (int)ax.front,
                    (double)scene->settings.unit_meters);
    }

    glm::vec3 bmin(1e30f), bmax(-1e30f);

    // 第一遍：找骨骼网格的 skin_deformer，构建共享 Skeleton。
    // 一个 FBX 通常只有一个 skin（或没有 = 静态网格）。
    std::shared_ptr<Skeleton> skeleton;
    const ufbx_skin_deformer* skinForAnim = nullptr;
    {
        for (size_t i = 0; i < scene->meshes.count && !skeleton; ++i) {
            const ufbx_mesh* meshPtr = scene->meshes.data[i];
            if (!meshPtr) continue;
            const ufbx_mesh& mesh = *meshPtr;
            if (mesh.skin_deformers.count > 0) {
                const ufbx_skin_deformer* skinPtr =
                    mesh.skin_deformers.data[0];
                const ufbx_skin_deformer& skin = *skinPtr;
                skinForAnim = &skin;
                // meshNodeWorld：这个 mesh 的第一个实例节点（若有）的
                // geometry_to_world；没有实例则单位阵。
                glm::mat4 meshNodeWorld(1.0f);
                if (mesh.instances.count > 0) {
                    meshNodeWorld =
                        toGlm(mesh.instances.data[0]->geometry_to_world);
                }
                skeleton = buildSkeleton(skin, meshNodeWorld);
            }
        }
    }

    // 遍历所有节点，展开 mesh。
    std::function<void(const ufbx_node*)> walkNode = [&](const ufbx_node* n) {
        if (!n) return;
        if (n->mesh) {
            const ufbx_mesh& mesh = *n->mesh;
            const ufbx_skin_deformer* skin =
                mesh.skin_deformers.count > 0 ? mesh.skin_deformers.data[0]
                                              : nullptr;

            std::vector<PrimResult> prims;
            if (extractMesh(mesh, skin, prims)) {
                // mesh 节点的世界变换（几何空间）
                const glm::mat4 world = toGlm(n->geometry_to_world);
                const std::string meshBase =
                    n->name.length ? std::string(n->name.data, n->name.length)
                                   : "mesh";
                for (size_t p = 0; p < prims.size(); ++p) {
                    PrimResult& pr = prims[p];
                    const std::string subName =
                        name + "/" + meshBase + "_" + std::to_string(p);

                    // 蒙皮网格：把顶点/法线从文件原始空间（cm / Y-up）
                    // **烘到引擎的"静止世界空间"**（米）。ufbx 的
                    // geometry_to_bone 不带轴/单位换算，而 node_to_world
                    // 带 —— 两者不可组合。烘焙后 invBind = inv(W_rest)，
                    // 静止时 jointMat = I，SubMesh::transform 只承担
                    // 归位平移（见下方归位段）。
                    if (skin) {
                        const glm::mat3 lin(world);
                        for (Vertex& v : pr.verts) {
                            v.pos = glm::vec3(world * glm::vec4(v.pos, 1.0f));
                            v.normal =
                                glm::normalize(lin * v.normal);
                        }
                    }

                    SubMesh sm;
                    sm.mesh = assets.registerMesh(subName, std::move(pr.verts),
                                                  std::move(pr.indices));
                    sm.name = subName;
                    // 静态网格：变换 = G2W（顶点还是文件原始空间）；
                    // 蒙皮网格：顶点已烘到世界，变换取单位阵（归位段会
                    // 左乘 T(-center)，正好是 push.model 需要的全部）。
                    sm.transform = skin ? glm::mat4(1.0f) : world;
                    if (sm.mesh) {
                        MeshSource msrc;
                        msrc.kind = MeshSource::Kind::FBX;
                        msrc.path = makeAssetRelative(path);
                        msrc.name = name;
                        msrc.subMeshIndex = (int)p;
                        sm.mesh->setSource(msrc);
                    }
                    // 材质
                    if (pr.materialIndex >= 0 &&
                        pr.materialIndex < (int)n->materials.count) {
                        sm.material =
                            fbxMaterial(assets,
                                        *n->materials.data[pr.materialIndex],
                                        subName);
                    } else if (pr.materialIndex >= 0 &&
                               pr.materialIndex < (int)mesh.materials.count) {
                        sm.material =
                            fbxMaterial(assets,
                                        *mesh.materials.data[pr.materialIndex],
                                        subName);
                    }
                    // 包围盒：先记 mesh 局部 AABB，walkNode 结束后统一
                    // 换算成世界空间做"归位"（见下方模型归位段）
                    if (sm.mesh && sm.mesh->hasBounds()) {
                        bmin = glm::min(bmin, sm.mesh->boundsMin());
                        bmax = glm::max(bmax, sm.mesh->boundsMax());
                    }
                    model->subMeshes.push_back(std::move(sm));
                }
            }
        }
        for (size_t c = 0; c < n->children.count; ++c)
            walkNode(n->children.data[c]);
    };
    walkNode(scene->root_node);

    // 挂骨架 + 解析动画
    if (skeleton) {
        model->skeleton = skeleton;
        if (skinForAnim) extractAnimations(*scene, *skinForAnim, *skeleton);
    }

    // ---------------- 模型归位（import pivot normalization）----------------
    // FBX 场景里的节点可以摆在世界任意远处（Blender 导出的 FBX 常带几公里
    // 外的 armature 原点）。按"导入资产"的语义（与 UE 的 Import Translation
    // 归零一致），把整只模型平移回原点附近：
    //   1. 用 mesh 局部 AABB × SubMesh::transform 的 8 角点算**世界** AABB
    //   2. center = AABB 中心；每个 SubMesh::transform 左乘 translate(-center)
    //   3. model->center 归零、radius 取 AABB 半对角
    // 蒙皮一致性：T(-center) 乘在 SubMesh::transform 最左侧，渲染时蒙皮
    // 顶点 = push.model × jointMat × pos —— 骨骼驱动的顶点与骨骼一起被
    // 平移，姿势不变。实体侧把 transform 分解成 TRS 后也只是平移分量
    // 减掉 center，旋转/缩放不受影响。
    if (bmin.x <= bmax.x && !model->subMeshes.empty()) {
        // 用第一个 submesh 的世界角点初始化，再逐个合并
        glm::vec3 wmin(1e30f), wmax(-1e30f);
        for (const auto& sm : model->subMeshes) {
            if (!sm.mesh || !sm.mesh->hasBounds()) continue;
            const glm::vec3 lo = sm.mesh->boundsMin();
            const glm::vec3 hi = sm.mesh->boundsMax();
            const glm::vec3 corners[8] = {
                {lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z},
                {lo.x, hi.y, lo.z}, {hi.x, hi.y, lo.z},
                {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z},
                {lo.x, hi.y, hi.z}, {hi.x, hi.y, hi.z},
            };
            for (const glm::vec3& c : corners) {
                const glm::vec3 w =
                    glm::vec3(sm.transform * glm::vec4(c, 1.0f));
                wmin = glm::min(wmin, w);
                wmax = glm::max(wmax, w);
            }
        }
        if (wmin.x <= wmax.x) {
            const glm::vec3 center = (wmin + wmax) * 0.5f;
            for (auto& sm : model->subMeshes) {
                sm.transform =
                    glm::translate(glm::mat4(1.0f), -center) * sm.transform;
            }
            model->center = glm::vec3(0.0f);
            model->radius =
                glm::max(glm::length(wmax - wmin) * 0.5f, 0.01f);
            VK_LOG_INFO("FBX normalized: world bounds (%.2f,%.2f,%.2f)-"
                        "(%.2f,%.2f,%.2f) -> center (%.3f,%.3f,%.3f) "
                        "pulled back to origin, radius=%.3f",
                        wmin.x, wmin.y, wmin.z, wmax.x, wmax.y, wmax.z,
                        center.x, center.y, center.z, model->radius);
        }
    } else if (bmin.x <= bmax.x) {
        model->center = (bmin + bmax) * 0.5f;
        model->radius = glm::length(bmax - bmin) * 0.5f;
    }

    VK_LOG_INFO("FBX loaded: %s (%u submeshes, %s, %d joints)",
                path.c_str(), (uint32_t)model->subMeshes.size(),
                skeleton ? "skinned" : "static",
                skeleton ? skeleton->jointCount() : 0);

    ufbx_free_scene(scene);
    return model;
}

} // namespace assets
