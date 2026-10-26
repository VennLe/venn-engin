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
#include "assets/RampAsset.h"

// 同名材质冲突要打 WARN（W0-1）。以前这里没有日志依赖 ——
// 冲突静默合并正是"三个 bug 里最难发现的那个"的根本原因。
#include "core/Logger.h"

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
    // 植被：草叶（三张交叉面片）/ 树（干 + 两层冠）。size = 世界高度（米）。
    // 与 cube/sphere 同级的内置形状 —— 撒点用得上，且**能被场景序列化
    // 重建**（MeshSource 里 shape = "grass" / "tree"）。
    Mesh* grass(float size = 0.6f, const std::string& name = "grass");
    Mesh* tree(float size = 5.0f, const std::string& name = "tree");
    // 花（细杆 + 花冠）。见 Mesh.h 的说明。
    Mesh* flower(float size = 0.35f, const std::string& name = "flower");
    // 石头（位移 + 平面着色的低面数球）。见 Mesh.h 的说明。
    Mesh* rock(float size = 1.4f, const std::string& name = "rock",
               int seed = 0);
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

    // **容错版**：读不出来就返回 nullptr，不抛。
    //
    // 为什么必须有它：`Texture::fromFile` 在文件打不开时 `throw`
    // （`Texture.cpp:665`），而调用点在**模型导入**的循环里 —— 一张贴图丢
    // 就让整个模型加载失败、实体变空。而"贴图丢了"恰恰是资产链上最常见的
    // 现场：换机器 / 漏拷 `textures/` 目录 / 改了相对路径。正确语义是
    // **退化到默认材质继续渲染**，而不是拒绝整个模型。
    //
    // 所以：凡是**加载期从资产文件里读**的贴图都走这个版本，由调用方决定
    // 打哪条 WARN（`VMESH-TEX-MISSING` / `FBX-TEX-MISSING`）。
    // 而 glTF / 用户显式指定的贴图仍走 loadTexture —— 那条路上失败该报错。
    Texture* tryLoadTexture(const std::string& path,
                            const std::string& name = "", bool srgb = true);

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

    // ============================================================
    // Toon 色带资产（A6，2026-10-26）
    //
    // 材质槽 `rampMap` 指向本管理器持有的 `RampAsset::texture` —— 材质本身
    // **不拥有**色带（与其它纹理槽同一条语义：Texture* 是弱引用）。
    // 缓存键：
    //   · 从磁盘读 → 相对 assets/ 的路径（同一份 .vramp 全场共享一张纹理）
    //   · 程序化新建 → 调用方给的 name
    // ------------------------------------------------------------
    // 读 `.vramp`；失败（文件缺失 / JSON 坏 / 目录不对）返回 nullptr 并打
    // WARN，**不抛** —— 与 tryLoadTexture 同一条容错纪律。
    RampAsset* loadRampAsset(const std::string& relPath,
                             const std::string& name = "");
    // 程序化新建（3 档预设）。同名已存在则直接复用。
    RampAsset* makeRampAsset(const std::string& name);
    RampAsset* findRampAsset(const std::string& key) const;
    // 编辑色带之后必须调它：重新烘 256×1 纹理并**换掉** texture 指针。
    // 调用方随后要把所有引用该色带的材质的 `rampMap` 指到新纹理上
    // （见 InspectorPanel 的 commit 路径）。
    void rebuildRampTexture(RampAsset& ramp);
    const TextureContext& textureContext() const { return m_ctx; }

    // ---- 材质稳定身份（W0-2）----
    // 按 `Material::id`（`models/xxx.glb#Skin`）查。线性扫描 —— 材质数量在
    // 几百级，且只发生在读场景时，不做二级索引。
    Material* findMaterialById(const std::string& id) const;
    // 把 id 写进材质；若该 id 已被**另一个**材质占用则追加 `#2`/`#3` 并打
    // `ASSET-MAT-COLLIDE`（id 撞车同样不能静默）。
    void assignMaterialId(Material* m, const std::string& id);

    // ---- 材质实例（W1）----
    // 从母材质派生一个实例：值语义浅拷贝（参数 + 纹理指针），`parent` 指向母材质，
    // `overridden = 0`（一开始全跟随）。
    // ⚠ **只支持两级**：`parent` 本身是实例时拒绝并打 WARN —— 深度嵌套会让
    //   "改了母材质谁该变"变成需要拓扑排序的问题，收益远小于复杂度。
    // 同名的已存在实例会**复用**（同一份场景读两次不该每次多长一个材质）。
    Material* makeInstanceOf(const Material* parent, const std::string& name);

    // ---- 母材质变更的传播（W1-2）----
    // 把母材质里**未被实例覆盖**的字段刷到所有子实例上。返回被更新的实例数。
    //
    // ⚠ 为什么不做自动脏检测：`Material` 是裸结构体（没有 setter）。
    //   加 setter 要改 40+ 处写入点，漏一个就静默不同步。
    // ⚠ 已知代价：O(材质数)，每次拖动滑条调用一次。材质数在几百级，
    //   遍历成本可忽略 —— 不做"脏集合"优化。
    int materialChanged(const Material* parent);

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
    // Toon 色带（A6）。键同 m_textures 的约定：磁盘资产用相对路径，
    // 程序化资产用调用方给的名字。
    std::unordered_map<std::string, std::unique_ptr<RampAsset>> m_ramps;
    // 重建色带时**退役**的旧纹理（见 rebuildRampTexture 的说明）：
    // 留几帧再释放，避免销毁还在在途 CB 里的对象。
    std::vector<std::unique_ptr<Texture>> m_rampRetired;

    // 同名材质冲突的编号源（W0-1）。只增不减 —— 编号进了存档，
    // 复用会让"同一个名字"在两次加载里指到不同的材质。
    std::uint32_t m_matConflictSeq = 0;

    // 按来源生成各级；只负责往 chain 里 add，不动别的状态
    void buildBuiltinLods(const Mesh& base, const MeshSource& src,
                          LodChain& chain);
    void buildDecimatedLods(Mesh& base, LodChain& chain);
};

} // namespace assets

// ---------------- 内联实现 ----------------

#include "assets/MeshLoader.h"
#include "assets/Lod.h"
#include "assets/AssetPath.h"
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
//
// ⚠ `subMeshIndex` 是**必须**拼进来的（2026-10-24，事故修复）。
//   `MeshSource::name` 对 glTF / FBX / `.vmesh` 存的是**模型**的缓存键
//   （见 MeshSource 的注释），一个模型有几十个子网格、全都拿同一个 name。
//   只用 name 拼键的后果不是"稍微重复"，而是**静默丢数据**：
//   第一个子网格把自己的减面结果注册成 `alicia#lod1`，后面 19 个子网格
//   再调 `registerMesh("alicia#lod1", ...)` 会命中缓存，各自算出来的
//   减面顶点直接扔掉，于是脸、腿、头发统统去画那个"最先注册的"低模。
//   症状（本次实测）：近距离正常，一拉远脸变成一块白板、腿消失 ——
//   因为那 19 个子网格换上了别人的几何，UV 落在贴图的空白区。
//   程序化图元 subMeshIndex = -1，不受影响；OBJ 的 name 就是它自己的键。
inline std::string lodKey(const Mesh& base, const std::string& name, int lv) {
    std::string stem = name;
    if (stem.empty()) {
        stem = "mesh0x" + std::to_string(
            static_cast<unsigned long long>(
                reinterpret_cast<uintptr_t>(&base)));
    }
    const int sub = base.source().subMeshIndex;
    if (sub >= 0) stem += "#s" + std::to_string(sub);
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
    if (base->noLod()) {
        // W2-2：`.vmeta` 里 generateLods=false —— 用户明确要求别建链
        // （小道具、UI 图标这类东西减面反而更丑，省一次 CPU 减面）。
    } else if (src.kind == MeshSource::Kind::Builtin) {
        buildBuiltinLods(*base, src, *chain);
    } else if (base->hasMorph()) {
        // ============================================================
        // 有 morph 的网格**不建 LOD 链**（§15）
        // ============================================================
        // LOD 各级是**独立减面出来的 Mesh**，它们没有 delta 流
        // （`morphTargetCount() == 0`），`Model::subMeshIndexOf()` 也找不到
        // 它们 —— 于是 `MorphComponent` 求值出来的权重没有落点，
        // `DrawItem::morph.targets` 恒为 0，着色器里那道 `mi.z > 0` 的硬开关
        // 直接关掉。症状是：一旦掉到低模，**几何/法线/UV 全对，只有脸不动**
        // —— 静默失效，没有任何报错或日志，人眼要盯着角色走近才看得出来。
        //
        // 本可以做的补救是「把 delta 重定向到减面后的最近面上」，但那需要
        // 在减面时建立「新顶点 → 原顶点集合」的反查表（聚类已经把原来的顶点
        // 对应关系丢掉了），是一条独立的管线，不能顺手塞进这里。
        // 在此之前，宁可不要 LOD —— 与上面蒙皮那一支同样的取舍：
        // 多画几十万三角形是可接受的代价，让主角的脸静默定格不是。
        VK_LOG_INFO("LOD-SKIP-MORPH '%s'（网格带 morph 偏移，不做自动减面，见 ensureLod 注释）",
                    src.name.c_str());
    } else if (meshLooksSkinned(*base)) {
        // ============================================================
        // 蒙皮网格**不自动减面**（2026-10-24，事故修复）
        // ============================================================
        // 顶点聚类简化对"贴图密度均匀的静态道具"够用，但对角色是最坏的
        // 组合，实测把整个角色毁掉（脸变白板、腿消失、鞋子悬空）：
        //
        //   ① 选层口径错：LOD 是按**每个子网格自己的包围球**算屏幕占高比的。
        //      脸只有 19 cm 高，在 4 m 外的第三人称视角里比值 ≈ 0.06 →
        //      直接落到第 3 级（阈值 0.04），1571 顶点被压到 100 上下。
        //      人眼对"脸"的敏感度和对"远处一棵树"完全不是一回事。
        //   ② UV 会被平均掉：聚类把一格里的 UV 求平均，而角色的脸/眼/
        //      眉都是**同一张图集上切开的小岛**，平均后采样点落到图集的
        //      空白处 —— 这就是"脸变成一块纯白、眼睛消失"的直接原因。
        //   ③ 关节权重本来就要重新归一化，越减越糊。
        //
        // UE5 的骨骼网格体 LOD 也不是自动生成的：它按 **Model** 统一选层，
        // 而且默认不给你生成，要靠 Skeletal Mesh Editor 里手动加+LOD 设置。
        // 这里保持同样的克制：**没有专门的骨骼 LOD 管线之前，宁可不要 LOD**。
        // 代价是大场景里的角色多画几十万三角形；错误地减面毁掉主角的脸，
        // 代价比这高得多。
        VK_LOG_INFO("LOD-SKIP-SKIN '%s'（蒙皮网格不做自动减面，见 ensureLod 注释）",
                    src.name.c_str());
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

inline Mesh* AssetManager::grass(float size, const std::string& name) {
    auto it = m_meshes.find(name);
    if (it != m_meshes.end()) return it->second.get();
    auto mesh = std::make_unique<Mesh>();
    mesh->upload(*m_ctx.device, *m_ctx.cmdPool, makeGrassVertices(size),
                 makeGrassIndices());
    Mesh* raw = mesh.get();
    tagBuiltinMesh(raw, "grass", size, name);
    m_meshes[name] = std::move(mesh);
    return raw;
}

inline Mesh* AssetManager::tree(float size, const std::string& name) {
    auto it = m_meshes.find(name);
    if (it != m_meshes.end()) return it->second.get();
    auto mesh = std::make_unique<Mesh>();
    mesh->upload(*m_ctx.device, *m_ctx.cmdPool, makeTreeVertices(size),
                 makeTreeIndices());
    Mesh* raw = mesh.get();
    tagBuiltinMesh(raw, "tree", size, name);
    m_meshes[name] = std::move(mesh);
    return raw;
}

inline Mesh* AssetManager::flower(float size, const std::string& name) {
    auto it = m_meshes.find(name);
    if (it != m_meshes.end()) return it->second.get();
    auto mesh = std::make_unique<Mesh>();
    mesh->upload(*m_ctx.device, *m_ctx.cmdPool, makeFlowerVertices(size),
                 makeFlowerIndices());
    Mesh* raw = mesh.get();
    tagBuiltinMesh(raw, "flower", size, name);
    m_meshes[name] = std::move(mesh);
    return raw;
}

inline Mesh* AssetManager::rock(float size, const std::string& name, int seed) {
    auto it = m_meshes.find(name);
    if (it != m_meshes.end()) return it->second.get();
    auto mesh = std::make_unique<Mesh>();
    std::vector<Vertex> verts = makeRockVertices(size, seed);
    // 三角形汤：索引就是 0..n-1。Mesh 的绘制路径是 index buffer，
    // 走"无索引"要改渲染层，不值得为一份内置网格动。
    std::vector<uint32_t> idx(verts.size());
    for (std::size_t i = 0; i < idx.size(); ++i) {
        idx[i] = static_cast<uint32_t>(i);
    }
    mesh->upload(*m_ctx.device, *m_ctx.cmdPool, std::move(verts), std::move(idx));
    Mesh* raw = mesh.get();
    tagBuiltinMesh(raw, "rock", size, name);
    m_meshes[name] = std::move(mesh);
    return raw;
}

inline Mesh* AssetManager::loadModel(const std::string& path,
                                     const std::string& name) {    std::string key = name.empty() ? path : name;
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

inline Texture* AssetManager::tryLoadTexture(const std::string& path,
                                             const std::string& name, bool srgb) {
    if (path.empty()) return nullptr;
    try {
        return loadTexture(path, name, srgb);
    } catch (const std::exception&) {
        // 具体原因已经由 `Texture::fromFile` 打了 ERROR（路径 + stbi 的原因），
        // 这里再打一遍就是重复刷屏。调用方会补一条带 model/mat 的 WARN，
        // 那条才是排查时真正需要的上下文。
        return nullptr;
    } catch (...) {
        return nullptr;
    }
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

// ---------------- Toon 色带（A6，2026-10-26）----------------

inline RampAsset* AssetManager::findRampAsset(const std::string& key) const {
    auto it = m_ramps.find(key);
    return it == m_ramps.end() ? nullptr : it->second.get();
}

inline void AssetManager::rebuildRampTexture(RampAsset& ramp) {
    // 1 texel 的过渡在 256 宽下已经是 0.4% —— 肉眼等价于硬边，
    // 所以不需要为"色带必须硬切"再上 point 采样（那是采样器级改动，
    // 会影响共享同一批 sampler 的其它纹理）。
    constexpr uint32_t kWidth = 256;
    std::vector<uint8_t> px;
    assets::bakeRampPixels(ramp, kWidth, px);

    auto tex = std::make_unique<Texture>();
    tex->fromPixels(m_ctx, px.data(), kWidth, 1, true);

    // ⚠⚠ 旧纹理**不能立刻销毁**：它可能还在在途命令缓冲里（材质页每帧
    //   重算，但上一帧的 CB 可能还没执行完）。违反"recording 状态时引用
    //   的对象不能销毁"的直接后果是 invalid state 刷屏 → submit -4 →
    //   DEVICE_LOST（见 docs/render_resource_lifetime.md）。
    //   做法：先挪进"退役区"多留几帧。一张 256×1 只占 1 KB，留 64 张
    //   也就 64 KB —— 用这点内存换掉一整类崩溃。
    if (ramp.texture) {
        m_rampRetired.push_back(std::move(ramp.texture));
        while (m_rampRetired.size() > 64) m_rampRetired.erase(m_rampRetired.begin());
    }
    // **先建新的再换** —— 不能让 texture 空一帧：材质页会把空指针位置写成
    // 缺省白，那一帧角色会闪一下全亮。
    ramp.texture = std::move(tex);

    // 同一份色带可能被多个材质引用（共享资产语义）。它们的 `rampMap`
    // 现在指向**已被换下来**的旧纹理，必须一起改过来 —— 漏一个就是
    // "改了色带，某个部件没变"，最难查的那种不一致。
    int fixed = 0;
    for (auto& kv : m_materials) {
        Material* m = kv.second.get();
        if (m && m->ramp == &ramp) {
            m->rampMap = ramp.texture.get();
            ++fixed;
        }
    }
    VK_LOG_INFO("RAMP-REBUILD key=%s bands=%u retargeted=%d", ramp.name.c_str(),
                static_cast<unsigned>(ramp.steps()), fixed);
}

inline RampAsset* AssetManager::loadRampAsset(const std::string& relPath,
                                              const std::string& name) {
    const std::string key = name.empty() ? relPath : name;
    if (auto* hit = findRampAsset(key)) return hit;
    if (relPath.empty()) return nullptr;

    const std::string full = assets::resolveAssetPath(relPath);
    RampAsset ramp;
    if (!assets::loadRamp(ramp, full)) {
        VK_LOG_WARN("RAMP-MISSING path=%s", relPath.c_str());
        return nullptr;
    }
    ramp.name = key;
    // 落盘路径统一存**相对**形式，换机器还能找到（同 makeAssetRelative
    // 的纪律；这里直接把调用方给的相对路径存回去）。
    ramp.path = relPath;
    rebuildRampTexture(ramp);

    auto owned = std::make_unique<RampAsset>(std::move(ramp));
    RampAsset* raw = owned.get();
    m_ramps[key] = std::move(owned);
    VK_LOG_INFO("RAMP-LOAD key=%s bands=%u edge=%.3f", key.c_str(),
                static_cast<unsigned>(raw->steps()), raw->edge);
    return raw;
}

inline RampAsset* AssetManager::makeRampAsset(const std::string& name) {
    const std::string key = name.empty() ? "ramp" : name;
    if (auto* hit = findRampAsset(key)) return hit;

    RampAsset ramp = assets::makeRampPreset(assets::RampPreset::ThreeStep, key);
    rebuildRampTexture(ramp);

    auto owned = std::make_unique<RampAsset>(std::move(ramp));
    RampAsset* raw = owned.get();
    m_ramps[key] = std::move(owned);
    VK_LOG_INFO("RAMP-NEW key=%s bands=%u", key.c_str(),
                static_cast<unsigned>(raw->steps()));
    return raw;
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

// detail 字段里不能有空格（项目铁律）。材质名/模型路径里带空格是常见的
// （"Stone Wall"、"anime girl.glb"），所以日志里统一换下划线。
inline std::string assetLogSafe(const std::string& s) {
    std::string o = s;
    for (char& c : o) {
        if (c == ' ' || c == '\t') c = '_';
    }
    return o;
}

// "参数完全相同"的判据（W0-1）。//
// ⚠ 浮点用精确相等：两边的来源都是同一份文件的 `json.value(...)`，读两次
//   逐位相同。用 eps 反而会让"真的想改一点点"被吞掉（比如 0.5 → 0.5000001）。
inline bool sameMaterialParams(const Material* m, Texture* albedo,
                               Texture* normal, Texture* orm,
                               const glm::vec4& baseColor, float roughness,
                               float metallic, const glm::vec3& emissive,
                               bool doubleSided) {
    if (!m) return false;
    return m->albedoMap == albedo && m->normalMap == normal && m->ormMap == orm &&
           Material::sameVec4(m->baseColorFactor, baseColor) &&
           m->roughness == roughness && m->metallic == metallic &&
           Material::sameVec3(m->emissive, emissive) &&
           m->doubleSided == doubleSided;
}

inline Material* AssetManager::makeMaterialPBR(
    const std::string& name, Texture* albedo, Texture* normal, Texture* orm,
    const glm::vec4& baseColor, float roughness, float metallic,
    const glm::vec3& emissive, bool doubleSided) {
    std::string key = name;
    auto it = m_materials.find(key);
    if (it != m_materials.end()) {
        // 参数一致 → 真共享（500 个物体用同一组参数是常见情形，共享合理）
        if (sameMaterialParams(it->second.get(), albedo, normal, orm, baseColor,
                               roughness, metallic, emissive, doubleSided))
            return it->second.get();
        // ⚠ 同名但参数不同 —— 以前这里直接 `return it->second.get()`，
        //   后者的参数被**静默丢弃**：Ctrl+D 复制实体后两个物体共用第一份
        //   材质，"我改了 B，A 也变了"，一个 WARN 都没有。
        //
        //   现在两种做法都不对，所以走第三条路：不覆盖、不报错返回 null
        //   （那会让物体变无材质），而是**给唯一后缀另建一个**。
        //
        //   先在同一基名的后缀里找参数相同的 —— 保证幂等：同一份场景文件
        //   读两次得到同一组材质，不会每次加载都多长一个 `#7`。
        const std::string base = key;
        for (const auto& kv : m_materials) {
            const Material* cand = kv.second.get();
            if (cand == it->second.get()) continue;
            if (kv.first.size() <= base.size()) continue;
            if (kv.first.compare(0, base.size(), base) != 0) continue;
            if (kv.first[base.size()] != '#') continue;
            if (sameMaterialParams(cand, albedo, normal, orm, baseColor,
                                   roughness, metallic, emissive, doubleSided))
                return kv.second.get();
        }
        key = base + "#" + std::to_string(++m_matConflictSeq);
        VK_LOG_WARN("ASSET-MAT-COLLIDE name=%s renamed=%s",
                    assetLogSafe(base).c_str(), assetLogSafe(key).c_str());
    }

    auto mat = std::make_unique<Material>();
    mat->name = key;
    // 程序化材质的稳定身份就是它的缓存键（`Material::id` 对所有 kind 都有效，
    // 序列化写出去以后，读端"先按 id 找"这条路对老场景也是通的）。
    mat->id = key;
    mat->albedoMap = albedo;
    mat->normalMap = normal;
    mat->ormMap = orm;
    mat->baseColorFactor = baseColor;
    mat->roughness = roughness;
    mat->metallic = metallic;
    mat->emissive = emissive;
    mat->doubleSided = doubleSided;

    Material* raw = mat.get();
    m_materials[key] = std::move(mat);
    return raw;
}

inline Material* AssetManager::findMaterialById(const std::string& id) const {
    if (id.empty()) return nullptr;
    for (const auto& kv : m_materials) {
        if (kv.second && kv.second->id == id) return kv.second.get();
    }
    return nullptr;
}

inline void AssetManager::assignMaterialId(Material* m, const std::string& id) {
    if (!m) return;
    std::string want = id.empty() ? m->name : id;

    // id 撞车：同一个 `#Skin` 落在两个材质上 → 后到的追加编号。
    // 与同名材质冲突是同一类问题，所以共用同一个计数器和同一句 WARN 的格式。
    const Material* clash = findMaterialById(want);
    if (clash && clash != m) {
        const std::string base = want;
        want = base + "#" + std::to_string(++m_matConflictSeq);
        VK_LOG_WARN("ASSET-MAT-COLLIDE id=%s renamed=%s",
                    assetLogSafe(base).c_str(), assetLogSafe(want).c_str());
    }
    m->id = want;
}

inline Material* AssetManager::makeInstanceOf(const Material* parent,
                                              const std::string& name) {
    if (!parent) return nullptr;
    if (parent->isInstance()) {
        // 两级限制：实例的实例 → 拒绝，退化成"直接用母材质"
        VK_LOG_WARN("MATERIAL-INSTANCE parent=%s name=%s rejected=two-level",
                    assetLogSafe(parent->id.empty() ? parent->name : parent->id)
                        .c_str(),
                    assetLogSafe(name).c_str());
        return const_cast<Material*>(parent);
    }

    auto it = m_materials.find(name);
    if (it != m_materials.end()) {
        Material* m = it->second.get();
        if (m != parent) {
            m->parent = parent;
            m->source.kind = MaterialSource::Kind::Instance;
        }
        return m;
    }

    auto mat = std::make_unique<Material>();
    *mat = *parent;  // 值语义：参数与纹理指针一起拷过来
    mat->name = name;
    mat->id.clear();
    mat->parent = parent;
    mat->overridden = 0;
    mat->source.kind = MaterialSource::Kind::Instance;

    Material* raw = mat.get();
    m_materials[name] = std::move(mat);
    assignMaterialId(raw, name);
    VK_LOG_INFO("MATERIAL-INSTANCE parent=%s name=%s overrides=0",
                assetLogSafe(parent->id.empty() ? parent->name : parent->id)
                    .c_str(),
                assetLogSafe(name).c_str());
    return raw;
}

inline int AssetManager::materialChanged(const Material* parent) {
    if (!parent) return 0;
    const Material::Params src = parent->captureParams();
    int updated = 0;
    int skipped = 0;
    for (auto& kv : m_materials) {
        Material* inst = kv.second.get();
        if (!inst || inst->parent != parent) continue;
        // 只刷"未覆盖"的那几位：覆盖位是用户明确要求"这一件不一样"
        const std::uint32_t follow =
            Material::kOvScalarMask & ~inst->overridden;
        if (!follow) {
            ++skipped;
            continue;
        }
        Material::Params p = inst->captureParams();
        Material::mergeParams(p, src, follow);
        inst->applyParams(p);
        ++updated;
    }
    if (updated || skipped) {
        VK_LOG_INFO("MATERIAL-SYNC parent=%s updated=%d skipped=%d",
                    assetLogSafe(parent->id.empty() ? parent->name : parent->id)
                        .c_str(),
                    updated, skipped);
    }
    return updated;
}

} // namespace assets
