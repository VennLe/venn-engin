#pragma once
// ============================================================
// assetio/SourceModel —— 导入管线的**中间表示**（IR）
//
// 这正是"FBX → 解析 → 中间数据 → 修正 → 引擎资产 → 序列化"里的
// 「中间数据」那一层。它存在的意义不是"好看"，而是三个具体问题：
//
//   1. **解析与上传解耦**。解析要 ufbx + 数学，上传要 VkDevice。
//      混在一起的后果是"想验证一次导入，必须先开窗口 + 有显卡"——
//      于是这条链从来没被无窗口自检覆盖过。
//   2. **修正集中在一处**。坐标轴、单位、三角化、UV 翻转、pivot 归位
//      都是"导入期问题"，全部在 IR 生成时做掉。IR 一旦产出，后面的
//      写盘 / 读盘 / 上传 **不再做任何几何修正** —— 否则两处各修一半，
//      就会出现"编辑器里对、游戏里歪"。
//   3. **离线与在线共用一份数据描述**。`AssetC`（离线）写盘用它，
//      运行时读盘还原成它，`FbxLoader` 直连也产出它。三者的结构体
//      如果是三份，迟早会不同步。
//
// ⚠ 这里的 IR **不是** 运行时的 `assets::Model`：
//   `assets::Model` 持有 GPU 资源（Mesh/Material 指针）和 shared_ptr，
//   IR 只有纯数据。两者之间的转换点在
//     · engine/assets/VMeshLoader.cpp（读盘 → Model）
//   这是刻意的：IR 要能被 memcpy 级别地序列化，就不能有指针。
// ============================================================

#include "assetio/Vertex.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace assetio {

// ------------------------------------------------------------
// 材质
//
// 只保留**能跨引擎复用**的那部分：PBR 因子 + 贴图路径。
// 引擎侧特有的东西（材质 id、`.vmeta` 覆盖、AssetManager 缓存键）
// 一律不进来 —— 那些是"引擎的材质身份"，不是"文件的材质内容"。
// ------------------------------------------------------------
struct SourceMaterial {
    std::string name;         // 文件里的材质名（FBX: material.name）
    // 贴图路径。两种都留着：
    //   albedoPath —— **相对资产根**的规范化路径（`models/tex/skin.png`）
    //   albedoRaw  —— 文件里写的原始字符串（可能是 `C:\Users\x\skin.png`）
    // 为什么两个都要：路径解析有可能失败（贴图不在资产目录里）。失败时
    // 运行期还能拿 raw 再试一次，或者让用户看到"它本来想找哪个文件"。
    std::string albedoPath;
    std::string albedoRaw;
    std::string normalPath;
    std::string normalRaw;

    glm::vec4 baseColor{1.0f, 1.0f, 1.0f, 1.0f};
    float roughness = 0.5f;
    float metallic = 0.0f;
    glm::vec3 emissive{0.0f};
    bool doubleSided = false;

    // ---- 透明模式（P0-3，2026-10-25，格式版本 2）----
    //
    // 这三个值以前**完全不存在**于 IR 里：`assets::Material` 有 alphaMode，
    // 但导入链一路把它丢光 —— 结果所有从 FBX 烤出来的材质在运行时都是
    // Opaque。后果不是"半透明不好看"，是**睫毛/头发卡片变成硬边不透明**、
    // 树叶镂空变成实心纸片，眼睛的透明边缘直接消失。
    //
    // 取值与 `assets::Material::AlphaMode` 一一对应（这里不 include
    // 引擎头的理由见文件头：IR 不认识 GPU 侧类型）：
    //   0 = Opaque   1 = Mask   2 = Blend
    enum class AlphaMode : std::uint8_t { Opaque = 0, Mask = 1, Blend = 2 };
    AlphaMode alphaMode = AlphaMode::Opaque;
    // 只在 Mask 模式下有意义；其它模式保留 0.5 作为"没设过"的标记。
    float alphaCutoff = 0.5f;
};

// ------------------------------------------------------------
// morph target（= blend shape / shape key）
//
// 稀疏表示：只记录**真正有偏移**的顶点。这不是"实现起来省事"，
// 是唯一可行的做法 —— 见 §13.3 算的那笔账：50 个 target × 3 万顶点
// 稠密化 ≈ 18 MB/子网格，而一层表情通常只动脸部几千个顶点。
//
// `vertices` 里存的是**子网格内局部顶点下标**（`SourceSubMesh::vertices`
// 的下标），不是 FBX 的逻辑顶点下标 —— 转换在 FbxImport 里做掉，
// 之后的每一环（焊接 / 重排 / 落盘 / 上传）都只认这一套下标。
// ------------------------------------------------------------
struct SourceMorphTarget {
    std::string name;
    std::vector<std::uint32_t> vertices;
    std::vector<glm::vec3> positionDeltas;   // 与 vertices 等长
    // 法线偏移。**可以空**（= 全部为 0）：FBX 里法线偏移是可选的，
    // Blender 默认不导出。空数组和"全是零"必须区分开吗？不需要 ——
    // 落盘时一律补齐成等长的零，读端不判空。省下的那点体积不值得
    // 让"空 / 零"变成两种状态。
    std::vector<glm::vec3> normalDeltas;
};

// ------------------------------------------------------------
// morph channel（MORPH 区块 v2，§15）
//
// **channel 与 target 不是一一对应的**，这是 FBX/Blender 的真实数据模型：
//   · channel = 滑条（Inspector 上的那一根），名字是"会动的那个东西"
//   · target  = 一个具体形状
// 常见情况下一个 channel 只有一个 target（权重直接传下去），但 Blender 的
// "in-between shape key" 允许一个滑条中途经过多个形状：
//
//     滑条 0.0 ─── 0.5 处是 A ─── 1.0 处是 B
//
// 这时一个 channel 有 2 个 keyframe → **2 个 target**，运行时按滑条权重在
// 两者之间线性插值（见 `assets::MorphRig::evaluate`）。
//
// 上一刀（§14）直接把 `ufbx_blend_channel::target_shape` 当成 target，等于
// 把中间的 A 整段丢掉 —— 滑条拖到 0.5 时看到的还是 0.5×B。数据上"少了
// 一半形状"，画面上是"表情对不上"，而且没有任何报错。
// ------------------------------------------------------------
struct SourceMorphChannelKey {
    // 这个 keyframe 在**滑条**上对应的位置（ufbx 的 target_weight）。
    // 单个 key 时恒为 1.0（= 滑条推到底就到位）。
    float targetWeight = 1.0f;
    // 指向 `SourceSubMesh::morphTargets` 的下标。
    // `kNoMorphTarget` = 这个 key 在本子网格里**没有偏移**，但它仍然是一个
    // 插值断点（见下面的说明），所以不能整条丢掉。
    std::uint32_t target = 0;
};

// "这个 key 在本子网格里没有对应 target"。
//
// 为什么需要它：子网格是**按材质**切出来的，同一个 channel 的不同 keyframe
// 可能只影响其中一部分材质桶。丢掉这种 key 会让分段插值的断点消失 ——
// 例如 channel 在 0.5 处是 A、1.0 处是 B，而本子网格只有 B 的偏移：
// 丢掉 A 之后 `w=0.5` 会让 B 立刻满值，正确行为是 B 在 0.5 处仍为 0。
// 所以保留断点、只是不往任何 target 写权重。
constexpr std::uint32_t kNoMorphTarget = 0xFFFFFFFFu;

struct SourceMorphChannel {
    std::string name;
    // 按 targetWeight 升序。空 = 无效 channel（落盘时跳过）。
    std::vector<SourceMorphChannelKey> keys;
};

// 扁平化的 delta 流里的一条记录（**写盘产物，不是导入产物**）。
//
// 布局刻意做成 32 字节、且与 GPU 侧 `GpuMorphDelta` 逐字节一致：
//   position(12) target(4) normal(12) vertex(4)
// 这样 VMeshLoader 可以直接 memcpy 到一个 SSBO，运行时零转换。
// 最后那 4 字节在 GPU 上是个"填充位"（着色器不读），在文件里存的是
// **顶点下标** —— 用来在加载时校验"流的顺序与顶点顺序一致"。
// 少了这道校验，一次"顶点重排了但流没跟着重排"会让整张脸按错误的
// 顶点形变，而几何、法线、UV 全都是对的，肉眼只能看到"脸歪了"。
struct SourceMorphDelta {
    glm::vec3 position{0.0f};   // 0
    std::uint32_t target = 0;   // 12
    glm::vec3 normal{0.0f};     // 16
    std::uint32_t vertex = 0;   // 28
};
static_assert(sizeof(SourceMorphDelta) == 32, "SourceMorphDelta 必须是 32 字节");
static_assert(offsetof(SourceMorphDelta, target) == 12, "SourceMorphDelta.target 偏移");
static_assert(offsetof(SourceMorphDelta, normal) == 16, "SourceMorphDelta.normal 偏移");
static_assert(offsetof(SourceMorphDelta, vertex) == 28, "SourceMorphDelta.vertex 偏移");

// ------------------------------------------------------------
// 子网格
//
// 一个 SubMesh = 一份顶点/索引 + 一个节点世界变换 + 一个材质。
// 这个粒度和 `assets::SubMesh` 一一对应 —— 两边都是"一次 draw call
// 需要的全部数据"，中间不再切分，免得换算来换算去。
// ------------------------------------------------------------
struct SourceSubMesh {
    std::string name;
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    glm::mat4 transform{1.0f};   // 已含 pivot 归位（见 FbxImport 末尾）
    int material = -1;           // 索引进 SourceModel::materials
    // morph target（空 = 这个子网格没有形变）。顺序即 target 下标，
    // 一旦落盘就不再变 —— 动画 / 表情驱动靠下标，名字只做诊断。
    std::vector<SourceMorphTarget> morphTargets;
    // 滑条（§15）。与 morphTargets 是**多对一**：一个 channel 可以有多条
    // key（in-between shape key），每条 key 指向一个 target。
    // 空 = 这个子网格没有 morph。
    std::vector<SourceMorphChannel> morphChannels;

    // 扁平 delta 流（vertex-major，按顶点序、同顶点内按 target 升序）。
    // 由 `VertexOps::optimizeSubMesh` 在**焊接与重排之后**生成，
    // 与 `vertices[i].morphBase / morphCount` 严格对应。
    // 导入器不填它，读端也不产出它 —— 它是"写盘那一刻"的派生物。
    std::vector<SourceMorphDelta> morphStream;
};

// ------------------------------------------------------------
// 骨架
//
// 字段与 `assets::Skeleton::Joint` 逐项对应，但**不共享类型** ——
// 理由同上（Skeleton.h 是引擎头，IR 要能被离线工具独立使用）。
// 两边的一致性由 VMeshLoader 的赋值语句保证（少一个字段会编译报错）。
// ------------------------------------------------------------
struct SourceJoint {
    std::string name;
    int parent = -1;
    glm::mat4 restLocal{1.0f};
    glm::mat4 invBind{1.0f};
    glm::mat4 externalChain{1.0f};   // glTF 外链；FBX 恒为单位阵
};

struct SourceTrack {
    // 与 `assets::AnimTrack::Path` 同值：0=T 1=R 2=S。
    // ⚠ 这里存 int 而不是枚举，是因为它要直接落盘成 u8；
    //   枚举的底层类型在不同编译器下不保证是 1 字节。
    int joint = -1;
    int path = 0;
    bool step = false;
    std::vector<float> times;
    std::vector<glm::vec4> values;   // T/S 用 xyz；R 用 (x,y,z,w)
};

struct SourceClip {
    std::string name;
    float duration = 0.0f;   // 原始时长。**压缩后仍然要保真**：
                             // 采样器按 duration 做循环回绕，缩了会变速。
    std::vector<SourceTrack> tracks;
};

// ------------------------------------------------------------
// 一次导入的产物
// ------------------------------------------------------------
struct SourceModel {
    std::string name;

    std::vector<SourceSubMesh> subMeshes;
    std::vector<SourceMaterial> materials;

    // 骨架：joints 为空 = 静态网格
    std::vector<SourceJoint> joints;
    std::vector<int> topoOrder;
    glm::mat4 invMeshNode{1.0f};
    std::vector<SourceClip> clips;

    // 模型包围：pivot 归位后 center 恒为 0，bounds 是"世界空间"的
    // 归一化包围盒（米）。radius 给相机取景 / 光源范围用。
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
    float radius = 1.0f;

    // 诊断
    bool ok = false;
    std::string error;
    std::uint64_t sourceHash = 0;   // 源文件 FNV-1a（判"源变了没有"）

    bool skinned() const { return !joints.empty(); }
    std::size_t vertexCount() const {
        std::size_t n = 0;
        for (const auto& s : subMeshes) n += s.vertices.size();
        return n;
    }
    std::size_t indexCount() const {
        std::size_t n = 0;
        for (const auto& s : subMeshes) n += s.indices.size();
        return n;
    }
    std::size_t triangleCount() const { return indexCount() / 3; }
};

} // namespace assetio
