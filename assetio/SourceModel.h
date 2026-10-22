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
};

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
