#include "assetio/FbxImport.h"

#include "assetio/Binary.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
// ⚠ 必须在本文件里自己定义，**不能**指望 `rhi/VulkanCommon.h`：
//   engine 侧的 .cpp 都是通过它间接拿到的（那里有一份
//   `#define GLM_ENABLE_EXPERIMENTAL`），而 assetio 刻意不 include
//   任何 rhi 头。少了这一行，`gtx/matrix_decompose` 会直接 `#error`。
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <ufbx.h>

namespace fs = std::filesystem;

namespace assetio {

namespace {

// ufbx 的 ufbx_matrix 是列主序 3x4（12 个 float）：
//   cols[0]=(m00,m10,m20) cols[1]=(m01,m11,m21) cols[2]=(m02,m12,m22)
//   cols[3]=(m03,m13,m23) ← 平移
// 不能直接 memcpy 成 mat4：那会多读 4 个 float（越界）当第四列。
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

std::string str(const ufbx_string& s) {
    return s.length ? std::string(s.data, s.length) : std::string();
}

// ------------------------------------------------------------ 路径

std::string toSlashes(std::string s) {
    for (char& c : s)
        if (c == '\\') c = '/';
    return s;
}

} // namespace

std::string relativizeTo(const std::string& absPath, const std::string& assetRoot) {
    if (absPath.empty()) return std::string();
    if (assetRoot.empty()) return std::string();
    std::error_code ec;
    const fs::path p = fs::weakly_canonical(fs::path(absPath), ec);
    if (ec) return std::string();
    const fs::path root = fs::weakly_canonical(fs::path(assetRoot), ec);
    if (ec) return std::string();
    const fs::path rel = fs::relative(p, root, ec);
    if (ec || rel.empty()) return std::string();
    const std::string r = toSlashes(rel.generic_string());
    // `../` 开头 = 在资产根之外。这种路径**不能**进资产文件：
    // 它只在当前这台机器上成立，拷到别的机器就是死链。
    if (r.rfind("..", 0) == 0) return std::string();
    return r;
}

namespace {

// 贴图路径解析。FBX 里存的路径形态极不统一：
//   绝对路径（`C:\Users\me\tex.png`）、
//   相对 FBX 的（`textures/tex.png`）、
//   只剩文件名（`tex.png`）、
//   甚至 Mac 风格的 `Macintosh HD/...`。
// 按"最可能命中的顺序"依次试，第一个存在的就采用；都不存在时返回空，
// 由运行期按 albedoRaw 再试一次（并给出可读的缺失提示）。
std::string resolveTexture(const std::string& raw, const fs::path& fbxDir,
                           const std::string& assetRoot) {
    if (raw.empty()) return std::string();
    std::error_code ec;

    const fs::path rawPath(toSlashes(raw));
    auto tryPath = [&](const fs::path& p) -> std::string {
        if (p.empty()) return std::string();
        if (!fs::is_regular_file(p, ec) || ec) return std::string();
        return relativizeTo(p.string(), assetRoot);
    };

    // 1) 文件里写的就是资产根下的相对路径（Blender 导出的常见情况）
    if (rawPath.is_relative()) {
        if (std::string r = tryPath(fs::path(assetRoot) / rawPath); !r.empty()) return r;
    }
    // 2) 绝对路径（或已是可解析路径）
    if (std::string r = tryPath(rawPath); !r.empty()) return r;
    // 3) 相对 FBX 所在目录
    if (rawPath.is_relative()) {
        if (std::string r = tryPath(fbxDir / rawPath); !r.empty()) return r;
    }
    // 4) 只取文件名，在 FBX 同目录 / 同级 textures 目录里找
    const fs::path base = rawPath.filename();
    if (!base.empty()) {
        if (std::string r = tryPath(fbxDir / base); !r.empty()) return r;
        if (std::string r = tryPath(fbxDir / "textures" / base); !r.empty()) return r;
        if (std::string r = tryPath(fs::path(assetRoot) / "textures" / base); !r.empty())
            return r;
    }
    return std::string();
}

// ------------------------------------------------------------ 网格
struct PrimResult {
    std::vector<Vertex> verts;
    std::vector<std::uint32_t> indices;
    int materialIndex = -1;
};

// ufbx 数据模型：
//   index（面角）—— 每个面角一个"唯一元组"（位置+法线+UV）
//   vertex（逻辑顶点）—— 位置去重后的顶点，蒙皮权重挂在它上面
//   faces[f].index_begin → index 数组
//
// ⚠ 每个 index 生成一个 Vertex，意味着**面角数**就是顶点数。
//   `meshopt` 意义上的重复顶点正是在这里产生的（一个平滑球体有 8k
//   逻辑顶点、48k 面角），所以后面的 `weldVertices` 不是可选项 ——
//   它是这一层最有效的优化（实测 5~6 倍顶点削减）。
bool extractMesh(const ufbx_mesh& mesh, const ufbx_skin_deformer* skin,
                 std::vector<PrimResult>& out) {
    if (mesh.num_faces == 0 || mesh.num_indices == 0) return false;

    // 面 → 三角形（扇形三角剖分）+ 每个三角形的材质
    std::vector<std::uint32_t> tris;
    std::vector<int> triMaterial;
    tris.reserve(mesh.num_triangles * 3);
    triMaterial.reserve(mesh.num_triangles);

    for (std::size_t f = 0; f < mesh.num_faces; ++f) {
        const ufbx_face face = mesh.faces.data[f];
        if (face.num_indices < 3) continue;
        const int mat = mesh.face_material.count
                            ? static_cast<int>(mesh.face_material.data[f])
                            : -1;
        // 扇形剖分：(v0, vi, vi+1)。对凸多边形正确；对凹多边形的
        // 边界情况会切出错三角形 —— 但那类面在 FBX 里本来就该由
        // 导出器三角化，这里不做耳切（做了也会和导出器的选择不一致，
        // 反而更难对账）。
        for (std::uint32_t i = 1; i + 1 < face.num_indices; ++i) {
            tris.push_back(face.index_begin + 0);
            tris.push_back(face.index_begin + i);
            tris.push_back(face.index_begin + i + 1);
            triMaterial.push_back(mat);
        }
    }
    if (tris.empty()) return false;

    const std::size_t n = mesh.num_indices;
    std::vector<Vertex> verts(n);
    const bool hasNormal = mesh.vertex_normal.exists;
    const bool hasUv = mesh.vertex_uv.exists;
    const bool hasSkin = skin != nullptr;

    for (std::size_t i = 0; i < n; ++i) {
        Vertex& v = verts[i];
        const glm::vec3 p = toGlm(ufbx_get_vertex_vec3(&mesh.vertex_position, i));
        v.pos[0] = p.x;
        v.pos[1] = p.y;
        v.pos[2] = p.z;

        if (hasNormal) {
            const glm::vec3 nr = toGlm(ufbx_get_vertex_vec3(&mesh.vertex_normal, i));
            v.normal[0] = nr.x;
            v.normal[1] = nr.y;
            v.normal[2] = nr.z;
        }
        if (hasUv) {
            const glm::vec2 uv = toGlm(ufbx_get_vertex_vec2(&mesh.vertex_uv, i));
            // FBX 的 UV 原点在左下，引擎（与 glTF / Vulkan）在左上
            v.uv[0] = uv.x;
            v.uv[1] = 1.0f - uv.y;
        }
        v.color[0] = v.color[1] = v.color[2] = v.color[3] = 1.0f;

        if (hasSkin) {
            const std::uint32_t logical = mesh.vertex_indices.data[i];
            if (logical < skin->vertices.count) {
                const ufbx_skin_vertex sv = skin->vertices.data[logical];
                // 取权重最大的 4 根骨头并**重新归一化**。
                //
                // ⚠ 为什么不能直接取前 4 个：FBX 允许一根骨骼链上
                //   7~8 个影响，导出器给的是"已排序"的权重，但取前 4 个
                //   之后和可能 < 1（例如 0.4+0.3+0.2+0.05 = 0.95）——
                //   那会让顶点朝原点**收缩 5%**，在角色关节处表现为
                //   "动作一大就瘪进去"，而且极难定位。
                struct W { std::uint32_t cluster; float w; };
                W tmp[8];
                std::uint32_t nw = sv.num_weights;
                if (nw > 8) nw = 8;
                for (std::uint32_t k = 0; k < nw; ++k) {
                    const ufbx_skin_weight sw =
                        skin->weights.data[sv.weight_begin + k];
                    tmp[k] = {sw.cluster_index, static_cast<float>(sw.weight)};
                }
                std::sort(tmp, tmp + nw, [](const W& a, const W& b) {
                    return a.w > b.w;
                });
                if (nw > 4) nw = 4;
                float sum = 0.0f;
                for (std::uint32_t k = 0; k < nw; ++k) sum += tmp[k].w;
                for (std::uint32_t k = 0; k < nw; ++k) {
                    v.joints[k] = tmp[k].cluster;
                    v.weights[k] = (sum > 1.0e-8f) ? (tmp[k].w / sum) : 0.0f;
                }
                if (nw == 0) {
                    // 没有任何权重 = 这个顶点会塌到原点。挂到第 0 根骨头上
                    // 至少能跟着动，比一个"消失的顶点"容易发现。
                    v.joints[0] = 0;
                    v.weights[0] = 1.0f;
                }
            }
        }
    }

    // 按材质分桶（每个桶一个 primitive / 子网格 / 一次 draw call）
    std::map<int, std::vector<std::uint32_t>> buckets;
    for (std::size_t t = 0; t < tris.size() / 3; ++t)
        buckets[triMaterial[t]].push_back(static_cast<std::uint32_t>(t));

    if (buckets.size() == 1) {
        PrimResult pr;
        pr.verts = std::move(verts);
        pr.indices = std::move(tris);
        pr.materialIndex = buckets.begin()->first;
        out.push_back(std::move(pr));
        return true;
    }

    for (const auto& kv : buckets) {
        std::vector<std::uint32_t> pi;
        pi.reserve(kv.second.size() * 3);
        std::unordered_map<std::uint32_t, std::uint32_t> remap;
        for (std::uint32_t tri : kv.second) {
            for (int c = 0; c < 3; ++c) {
                const std::uint32_t gi = tris[tri * 3 + c];
                auto it = remap.find(gi);
                if (it == remap.end()) {
                    const std::uint32_t local = static_cast<std::uint32_t>(remap.size());
                    remap[gi] = local;
                    pi.push_back(local);
                } else {
                    pi.push_back(it->second);
                }
            }
        }
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

// ------------------------------------------------------------ 材质
SourceMaterial convertMaterial(const ufbx_material& mat, const fs::path& fbxDir,
                               const std::string& assetRoot) {
    SourceMaterial out;
    out.name = str(mat.name);
    if (out.name.empty()) out.name = "material";

    const ufbx_material_map& bc = mat.pbr.base_color;
    if (bc.has_value) {
        if (bc.value_components >= 3) {
            out.baseColor = {bc.value_vec3.x, bc.value_vec3.y, bc.value_vec3.z, 1.0f};
        }
        if (bc.value_components >= 4) out.baseColor.a = bc.value_vec4.w;
    } else if (mat.fbx.diffuse_color.has_value) {
        const ufbx_material_map& dc = mat.fbx.diffuse_color;
        out.baseColor = {dc.value_vec3.x, dc.value_vec3.y, dc.value_vec3.z, 1.0f};
    }

    if (mat.pbr.roughness.has_value)
        out.roughness = static_cast<float>(mat.pbr.roughness.value_real);
    if (mat.pbr.metalness.has_value)
        out.metallic = static_cast<float>(mat.pbr.metalness.value_real);

    // ⚠ 自发光必须乘 EmissiveFactor：FBX 里 EmissiveColor 恒写成 (1,1,1)，
    //   "到底发不发光"由 Factor 决定。只看 color 会让整只模型变成白色剪影。
    if (mat.pbr.emission_color.has_value) {
        out.emissive = {static_cast<float>(mat.pbr.emission_color.value_vec3.x),
                        static_cast<float>(mat.pbr.emission_color.value_vec3.y),
                        static_cast<float>(mat.pbr.emission_color.value_vec3.z)};
    }
    if (mat.fbx.emission_factor.has_value)
        out.emissive *= static_cast<float>(mat.fbx.emission_factor.value_real);

    // FBX 的 "Two Sided" 在 ufbx 里是**材质特性**（features.double_sided），
    // 不是 pbr 字段。漏了它的后果是：树叶/布料/睫毛这类靠单面卡片的
    // 模型在引擎里有一半面消失（背面剔除）。
    out.doubleSided = mat.features.double_sided.enabled;

    const ufbx_texture* tex = bc.texture;
    if (!tex) tex = mat.fbx.diffuse_color.texture;
    if (tex && tex->type == UFBX_TEXTURE_FILE && tex->filename.length) {
        out.albedoRaw = toSlashes(str(tex->filename));
        out.albedoPath = resolveTexture(out.albedoRaw, fbxDir, assetRoot);
    }

    // 法线贴图只能从 `fbx.normal_map` 拿 —— `ufbx_material_pbr_maps`
    // 里**没有** normal_map 字段（它属于 FBX 的 builtin 属性集）。
    // 写到 `pbr.normal_map` 会编译不过；写成 `pbr.bump` 则语义不对
    // （bump 是灰度高度图，不是切线空间法线）。
    const ufbx_texture* nt = mat.fbx.normal_map.texture;
    if (nt && nt->type == UFBX_TEXTURE_FILE && nt->filename.length) {
        out.normalRaw = toSlashes(str(nt->filename));
        out.normalPath = resolveTexture(out.normalRaw, fbxDir, assetRoot);
    }
    return out;
}

// ------------------------------------------------------------ 骨架
std::vector<SourceJoint> buildSkeleton(const ufbx_skin_deformer& skin,
                                       std::vector<int>& topoOrder) {
    const int n = static_cast<int>(skin.clusters.count);
    std::vector<SourceJoint> joints(static_cast<std::size_t>(n));
    std::vector<glm::mat4> W(static_cast<std::size_t>(n), glm::mat4(1.0f));

    for (int j = 0; j < n; ++j) {
        const ufbx_skin_cluster* cl = skin.clusters.data[j];
        const ufbx_node* bone = cl ? cl->bone_node : nullptr;
        SourceJoint& J = joints[static_cast<std::size_t>(j)];
        J.name = bone && bone->name.length ? str(bone->name)
                                          : ("bone" + std::to_string(j));
        J.parent = -1;
        // restLocal 从 **node_to_world** 推导（不是 node_to_parent）：
        //   ufbx 把轴/单位换算烘进 node_to_world，root 的 node_to_parent
        //   可能不含这些调整 —— 直接用 parent 链会得到与 invBind 不一致
        //   的静止姿势（模型蜷成一团）。这是踩过的第一个坑。
        if (bone) W[static_cast<std::size_t>(j)] = toGlm(bone->node_to_world);
        J.invBind = glm::inverse(W[static_cast<std::size_t>(j)]);
        J.externalChain = glm::mat4(1.0f);
    }

    std::unordered_map<const ufbx_node*, int> nodeToJoint;
    for (int j = 0; j < n; ++j) {
        const ufbx_skin_cluster* cl = skin.clusters.data[j];
        if (cl && cl->bone_node) nodeToJoint[cl->bone_node] = j;
    }

    for (int j = 0; j < n; ++j) {
        const ufbx_skin_cluster* cl = skin.clusters.data[j];
        const ufbx_node* bone = cl ? cl->bone_node : nullptr;
        if (!bone) continue;
        // 沿父链找最近的**也是骨骼**的祖先（中间可能有 Armature 之类的
        // 普通节点，它们不是 joint，但它们的变换要烘进 restLocal）。
        const ufbx_node* p = bone->parent;
        while (p) {
            auto it = nodeToJoint.find(p);
            if (it != nodeToJoint.end()) {
                joints[static_cast<std::size_t>(j)].parent = it->second;
                break;
            }
            p = p->parent;
        }
        if (joints[static_cast<std::size_t>(j)].parent == j)
            joints[static_cast<std::size_t>(j)].parent = -1;
    }

    for (int j = 0; j < n; ++j) {
        const int p = joints[static_cast<std::size_t>(j)].parent;
        if (p >= 0)
            joints[static_cast<std::size_t>(j)].restLocal =
                glm::inverse(W[static_cast<std::size_t>(p)]) *
                W[static_cast<std::size_t>(j)];
        else
            joints[static_cast<std::size_t>(j)].restLocal =
                W[static_cast<std::size_t>(j)];
    }

    // 拓扑序（父先于子）。用多趟扫描而不是递归：骨骼链可能有环
    // （坏数据），递归会栈溢出，而多趟扫描最多退化成一趟空转。
    topoOrder.clear();
    topoOrder.reserve(static_cast<std::size_t>(n));
    std::vector<bool> done(static_cast<std::size_t>(n), false);
    for (int pass = 0; pass < n; ++pass) {
        bool progressed = false;
        for (int j = 0; j < n; ++j) {
            if (done[static_cast<std::size_t>(j)]) continue;
            const int p = joints[static_cast<std::size_t>(j)].parent;
            if (p < 0 || done[static_cast<std::size_t>(p)]) {
                done[static_cast<std::size_t>(j)] = true;
                topoOrder.push_back(j);
                progressed = true;
            }
        }
        if (!progressed) break;
    }
    for (int j = 0; j < n; ++j)
        if (!done[static_cast<std::size_t>(j)]) topoOrder.push_back(j);

    return joints;
}

// ------------------------------------------------------------ 动画
std::vector<SourceClip> extractAnimations(const ufbx_scene& scene,
                                          const ufbx_skin_deformer& skin,
                                          const std::vector<SourceJoint>& joints) {
    std::vector<SourceClip> clips;
    const float kFps = 30.0f;   // 与 FbxLoader 一致：逐帧重采样成固定帧率

    const int nJoints = static_cast<int>(joints.size());
    std::vector<const ufbx_node*> boneNode(static_cast<std::size_t>(nJoints), nullptr);
    for (int j = 0; j < nJoints && j < static_cast<int>(skin.clusters.count); ++j) {
        const ufbx_skin_cluster* cl = skin.clusters.data[j];
        boneNode[static_cast<std::size_t>(j)] = cl ? cl->bone_node : nullptr;
    }
    std::unordered_map<const ufbx_node*, int> boneSet;
    for (int j = 0; j < nJoints; ++j)
        if (boneNode[static_cast<std::size_t>(j)])
            boneSet[boneNode[static_cast<std::size_t>(j)]] = j;

    for (std::size_t s = 0; s < scene.anim_stacks.count; ++s) {
        const ufbx_anim_stack* stackPtr = scene.anim_stacks.data[s];
        if (!stackPtr || !stackPtr->anim) continue;
        const ufbx_anim_stack& stack = *stackPtr;

        SourceClip clip;
        clip.name = stack.name.length ? str(stack.name) : ("clip" + std::to_string(s));
        // Blender 的 FBX 导出（`bake_anim_use_all_actions`，本仓的角色管线就是
        // 这么导的）把 take 名写成 "<armature 对象名>|<action 名>"，例如
        // `ALICIA|Idle`。Unity / UE / glTF 转换器一律把 `|` 之后当 clip 名，
        // 这里照办 —— 否则 `Skeleton::findClip("Idle")` 的逐字比较找不到，
        // 角色会**静默退回 rest 姿势**：画面是不会动的 T-pose，日志却全绿。
        // ⚠ 归一化只在**读资产**这一步；`findClip` 的逐字语义不变。
        //   只剥第一个 `|` 之前的；`|` 后面为空（`"A|"`）时原样保留。
        {
            const std::size_t bar = clip.name.find('|');
            if (bar != std::string::npos && bar + 1 < clip.name.size())
                clip.name = clip.name.substr(bar + 1);
        }
        const double t0 = stack.time_begin;
        const double t1 = stack.time_end;
        const double dur = (t1 > t0) ? (t1 - t0) : 0.0;
        clip.duration = static_cast<float>(dur);
        if (dur <= 0.0) continue;

        const int frames = static_cast<int>(std::ceil(dur * kFps)) + 1;
        std::vector<SourceTrack> trk(static_cast<std::size_t>(nJoints) * 3);
        for (int j = 0; j < nJoints; ++j) {
            trk[j * 3 + 0].joint = j;
            trk[j * 3 + 0].path = 0;   // Translation
            trk[j * 3 + 1].joint = j;
            trk[j * 3 + 1].path = 1;   // Rotation
            trk[j * 3 + 2].joint = j;
            trk[j * 3 + 2].path = 2;   // Scale
        }

        for (int f = 0; f < frames; ++f) {
            const double t = t0 + static_cast<double>(f) / kFps;
            const float ft = static_cast<float>(f) / kFps;
            for (int j = 0; j < nJoints; ++j) {
                const ufbx_node* bn = boneNode[static_cast<std::size_t>(j)];
                if (!bn) {
                    trk[j * 3 + 0].times.push_back(ft);
                    trk[j * 3 + 0].values.push_back(glm::vec4(0, 0, 0, 1));
                    trk[j * 3 + 1].times.push_back(ft);
                    trk[j * 3 + 1].values.push_back(glm::vec4(0, 0, 0, 1));
                    trk[j * 3 + 2].times.push_back(ft);
                    trk[j * 3 + 2].values.push_back(glm::vec4(1, 1, 1, 1));
                    continue;
                }
                const ufbx_transform lt = ufbx_evaluate_transform(stack.anim, bn, t);
                glm::vec3 tr = toGlm(lt.translation);
                glm::quat rot(static_cast<float>(lt.rotation.w),
                              static_cast<float>(lt.rotation.x),
                              static_cast<float>(lt.rotation.y),
                              static_cast<float>(lt.rotation.z));
                glm::vec3 sc = toGlm(lt.scale);

                // ufbx_evaluate_transform 返回的是**相对直接父节点**的
                // 局部变换；如果父节点不是骨骼（Blender 的 Armature 对象、
                // Maya 的组节点），要把那条父链的静止世界变换 P 乘进去，
                // 才能和 restLocal（由 node_to_world 推导）处在同一空间。
                // 不做这一步的症状：静止姿势对得上、一播动画就蜷缩。
                const ufbx_node* pn = bn->parent;
                const bool parentIsBone = pn && boneSet.find(pn) != boneSet.end();
                if (!parentIsBone) {
                    const glm::mat4 P = pn ? toGlm(pn->node_to_world) : glm::mat4(1.0f);
                    const glm::mat4 M = P * glm::translate(glm::mat4(1.0f), tr) *
                                        glm::mat4_cast(rot) *
                                        glm::scale(glm::mat4(1.0f), sc);
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
                trk[j * 3 + 1].values.push_back(glm::vec4(rot.x, rot.y, rot.z, rot.w));
                trk[j * 3 + 2].times.push_back(ft);
                trk[j * 3 + 2].values.push_back(glm::vec4(sc, 1.0f));
            }
        }

        for (auto& t : trk)
            if (!t.times.empty()) clip.tracks.push_back(std::move(t));
        if (!clip.tracks.empty()) clips.push_back(std::move(clip));
    }
    return clips;
}

std::uint64_t hashFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return 0;
    std::uint64_t h = 0xcbf29ce484222325ull;
    std::vector<char> buf(64 * 1024);
    while (f) {
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize n = f.gcount();
        if (n <= 0) break;
        h = fnv1a64(buf.data(), static_cast<std::size_t>(n), h);
    }
    return h;
}

} // namespace

// ============================================================
// 入口
// ============================================================
SourceModel importFbx(const std::string& absPath, const ImportOptions& opt) {
    SourceModel out;
    out.name = fs::path(absPath).stem().string();
    out.sourceHash = hashFile(absPath);

    ufbx_load_opts lo = {};
    lo.target_axes = ufbx_axes_right_handed_z_up;   // Y-up → Z-up
    // ⚠ 与 FbxLoader 逐字一致：target_unit_meters 与"尺寸倍数"是**倒数**。
    //   实测（anime_girl.fbx）：target=1.0 → 归一化半径 0.822，
    //   target=0.5 → 1.644（放大两倍）。所以 scale=2 要写 1/2。
    lo.target_unit_meters = (opt.scale > 0.0f) ? (1.0f / opt.scale) : 1.0f;
    lo.generate_missing_normals = true;
    lo.evaluate_skinning = false;   // 自己算蒙皮（走 Skeleton 路径）
    lo.clean_skin_weights = true;
    lo.ignore_missing_external_files = true;

    ufbx_error err;
    ufbx_scene* scene = ufbx_load_file(absPath.c_str(), &lo, &err);
    if (!scene) {
        out.error = err.description.data ? std::string(err.description.data) : "ufbx load failed";
        return out;
    }

    const fs::path fbxDir = fs::path(absPath).parent_path();

    // ---- 1. 找 skin（一个 FBX 通常只有一个骨架）----
    const ufbx_skin_deformer* skinForAnim = nullptr;
    std::vector<SourceJoint> joints;
    std::vector<int> topo;
    {
        for (std::size_t i = 0; i < scene->meshes.count; ++i) {
            const ufbx_mesh* meshPtr = scene->meshes.data[i];
            if (!meshPtr || meshPtr->skin_deformers.count == 0) continue;
            const ufbx_skin_deformer* skinPtr = meshPtr->skin_deformers.data[0];
            if (!skinPtr) continue;
            skinForAnim = skinPtr;
            joints = buildSkeleton(*skinPtr, topo);
            break;
        }
    }

    // ---- 2. 材质（按名字去重：材质身份靠名字，与 .vmeta 的映射同一套）----
    std::unordered_map<std::string, int> matByName;
    auto materialIndexOf = [&](const ufbx_material& m) -> int {
        const SourceMaterial sm = convertMaterial(m, fbxDir, opt.assetRoot);
        auto it = matByName.find(sm.name);
        if (it != matByName.end()) return it->second;
        const int idx = static_cast<int>(out.materials.size());
        out.materials.push_back(sm);
        matByName.emplace(sm.name, idx);
        return idx;
    };

    // ---- 3. 遍历节点树，展开网格 ----
    std::function<void(const ufbx_node*)> walk = [&](const ufbx_node* n) {
        if (!n) return;
        if (n->mesh) {
            const ufbx_mesh& mesh = *n->mesh;
            const ufbx_skin_deformer* skin =
                mesh.skin_deformers.count > 0 ? mesh.skin_deformers.data[0] : nullptr;

            std::vector<PrimResult> prims;
            if (extractMesh(mesh, skin, prims)) {
                // geometry_to_world 是把**几何空间**送到世界的那一步。
                // ufbx 已经把轴/单位换算烘进去了，所以这里拿到的就是
                // "引擎坐标系、米"。
                const glm::mat4 world = toGlm(n->geometry_to_world);
                const std::string meshBase =
                    n->name.length ? str(n->name) : std::string("mesh");

                for (std::size_t p = 0; p < prims.size(); ++p) {
                    PrimResult& pr = prims[p];
                    SourceSubMesh sm;
                    sm.name = out.name + "/" + meshBase + "_" + std::to_string(p);

                    if (skin) {
                        // 蒙皮网格：把顶点/法线烘到"静止世界空间"。
                        // 之后 invBind = inv(W_rest)，静止时 jointMat = I，
                        // SubMesh::transform 只承担归位平移 ——
                        // 这正是 glTF 路径的惯例，两条链必须一致，
                        // 否则同一角色在 .glb / .fbx 下差一个缩放。
                        const glm::mat3 lin(world);
                        for (Vertex& v : pr.verts) {
                            const glm::vec3 pw =
                                glm::vec3(world * glm::vec4(glm::vec3(v.pos[0], v.pos[1],
                                                                      v.pos[2]), 1.0f));
                            v.pos[0] = pw.x;
                            v.pos[1] = pw.y;
                            v.pos[2] = pw.z;
                            const glm::vec3 nn =
                                glm::normalize(lin * glm::vec3(v.normal[0], v.normal[1],
                                                               v.normal[2]));
                            v.normal[0] = nn.x;
                            v.normal[1] = nn.y;
                            v.normal[2] = nn.z;
                        }
                        sm.transform = glm::mat4(1.0f);
                    } else {
                        sm.transform = world;
                    }

                    sm.vertices = std::move(pr.verts);
                    sm.indices = std::move(pr.indices);
                    sm.material = -1;
                    if (opt.importMaterials && pr.materialIndex >= 0) {
                        if (pr.materialIndex < static_cast<int>(n->materials.count)) {
                            sm.material = materialIndexOf(*n->materials.data[pr.materialIndex]);
                        } else if (pr.materialIndex < static_cast<int>(mesh.materials.count)) {
                            sm.material = materialIndexOf(*mesh.materials.data[pr.materialIndex]);
                        }
                    }
                    out.subMeshes.push_back(std::move(sm));
                }
            }
        }
        for (std::size_t c = 0; c < n->children.count; ++c)
            walk(n->children.data[c]);
    };
    walk(scene->root_node);

    // ---- 4. 动画 ----
    out.joints = std::move(joints);
    out.topoOrder = std::move(topo);
    out.invMeshNode = glm::mat4(1.0f);
    if (skinForAnim && opt.importAnimations && !out.joints.empty())
        out.clips = extractAnimations(*scene, *skinForAnim, out.joints);

    // ---- 5. pivot 归位（与 FbxLoader 完全同一套）----
    // FBX 里的节点可以在世界任意远处（Blender 导出的常带几公里外的
    // armature 原点）。按导入资产语义把整只模型平移回原点附近。
    // 蒙皮一致性：T(-center) 乘在 SubMesh::transform 最左侧，蒙皮顶点
    // = push.model × jointMat × pos，骨骼和顶点一起被平移，姿势不变。
    {
        glm::vec3 wmin(1e30f), wmax(-1e30f);
        for (const SourceSubMesh& sm : out.subMeshes) {
            if (sm.vertices.empty()) continue;
            glm::vec3 lo(1e30f), hi(-1e30f);
            for (const Vertex& v : sm.vertices) {
                lo = glm::min(lo, glm::vec3(v.pos[0], v.pos[1], v.pos[2]));
                hi = glm::max(hi, glm::vec3(v.pos[0], v.pos[1], v.pos[2]));
            }
            for (int c = 0; c < 8; ++c) {
                const glm::vec3 p((c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y,
                                  (c & 4) ? hi.z : lo.z);
                const glm::vec3 w = glm::vec3(sm.transform * glm::vec4(p, 1.0f));
                wmin = glm::min(wmin, w);
                wmax = glm::max(wmax, w);
            }
        }
        if (wmin.x <= wmax.x) {
            const glm::vec3 center = (wmin + wmax) * 0.5f;
            for (SourceSubMesh& sm : out.subMeshes)
                sm.transform = glm::translate(glm::mat4(1.0f), -center) * sm.transform;
            out.boundsMin = wmin - center;
            out.boundsMax = wmax - center;
            out.radius = std::max(glm::length(wmax - wmin) * 0.5f, 0.01f);
        } else {
            out.radius = 1.0f;
        }
    }

    ufbx_free_scene(scene);
    out.ok = !out.subMeshes.empty();
    if (!out.ok && out.error.empty()) out.error = "no mesh found in file";
    return out;
}

} // namespace assetio
