#pragma once
// ============================================================
// assetio/SkelFormat —— 独立骨架资产 `.vskel`（格式版本 1）
//
// ---------------------------- 为什么要独立成资产 ----------------------------
// 抄的是 UE 的 **Skeleton 资产**（`SK_Mannequin`）：
//
//     SK_Mannequin.uasset          ← 骨名 / 骨层级 / 静止姿势 / 挂点(socket)
//        ├── 被 SKM_Manny 引用
//        ├── 被 SKM_Quinn 引用
//        └── 被几百条 AnimSequence 引用
//
// 关键不是"文件多了一个"，而是**一份骨架被 N 个资产引用**。骨名一致才
// 能跨角色复用动画；骨层级一致才能跨角色重定向。这些都挂在骨架身上，
// 不挂在某个 mesh 身上。
//
// 本工程改造前：骨架是 `SourceModel::joints`，**内嵌在 `.vmesh` 里**。
// 后果不是抽象的"不优雅"，而是三件具体的事：
//   ① 两个角色用同一套骨架时，骨表存了两份；改一处漏一处 → "同一个
//      动画在 A 身上对、在 B 身上歪"；
//   ② 挂点（武器、视线）没有归属地，只能硬编码在代码里；
//   ③ 没有"这个 mesh 的骨架和那个动画的骨架是不是同一套"的可检查判据
//      —— UE 靠 Skeleton 资产的身份做这件事，我们靠 `boneDigest`。
//
// ---------------------------- 与 .vmesh 的关系 ----------------------------
// `.vmesh` 里的 SKEL 区块**保留**（兼容 + 兜底）：
//   · 老资产没有独立骨架 → 走内嵌那份，行为逐位不变；
//   · 有 `.vskel` 引用（`--skel-ref`）→ 优先用它，并用 `boneDigest`
//     校验两侧骨表一致。不一致就**告警并退回内嵌**，不是硬失败 ——
//     一个挂点资产坏了不该让角色消失。
//
// 动画（clip）这一刀仍留在 `.vmesh` 里。UE 的 AnimSequence 是独立资产，
// 那是下一刀的事（`docs/mannequin_skeleton.md` §6 有清单）。
//
// ---------------------------- 容器 ----------------------------
// 与 `.vmesh` 同一个容器（`assetio/ChunkFile.h`）：文件头 + 区块表 + CRC。
// 区块：
//   BONES(1)   joints / topoOrder / invMeshNode / skeletonToEngine / digest
//   SOCKET(2)  挂点表（可选区块：没有挂点就不写）
//   STR(3)     字符串表
// ============================================================

#include "assetio/SourceModel.h"

#include <cstdint>
#include <string>
#include <vector>

namespace assetio {

constexpr std::uint32_t kSkelFileVersion = 1;
constexpr char kSkelFileMagic[8] = {'V', 'N', 'S', 'K', 'E', 'L', '\x1A', '\0'};

// 区块 id。**不要复用**已经用过的数字。
enum SkelChunkId : std::uint32_t {
    kSkelChunkBones = 1,
    kSkelChunkSocket = 2,
    kSkelChunkString = 3,
};

enum SkelFileFlags : std::uint32_t {
    kSkelFlagSockets = 1u << 0,      // 带挂点表
    kSkelFlagEngineXform = 1u << 1,  // skeletonToEngine 不是单位阵
};

// ------------------------------------------------------------ 挂点
//
// 抄 UE 的 `USkeletalMeshSocket`：名字 + 挂在哪根骨 + 相对那根骨的变换。
// 运行时"把武器放到 weapon_r"就是查这张表，不必在代码里写死骨名 ——
// 写死骨名的后果是"换一个角色模型，武器就掉在地上"。
struct SourceSocket {
    std::string name;       // 挂点名（weapon_r / eye ...）
    std::string boneName;   // 挂在哪根骨上（名字，不是下标）
    glm::mat4 local{1.0f};  // 相对该骨的变换
};

// ------------------------------------------------------------ 骨架资产
//
// 字段与 `assets::Skeleton` 逐项对应，但**不共享类型**（`Skeleton.h` 是
// 引擎头，`assetio` 不能 include 它 —— 离线工具必须能在没有引擎的机器上跑）。
// 两边的一致性由加载器的赋值语句保证（少一个字段会编译报错）。
struct SourceSkeletonAsset {
    std::string name;

    std::vector<SourceJoint> joints;
    std::vector<int> topoOrder;
    glm::mat4 invMeshNode{1.0f};
    glm::mat4 skeletonToEngine{1.0f};

    std::vector<SourceSocket> sockets;

    // 骨表摘要（名字 + 父下标，按顺序）。用途只有一个：
    // **判"mesh 和骨架是不是同一套"** —— UE 靠 Skeleton 资产的身份，
    // 我们靠这个 64 位摘要（写入时算，读取时重算比对）。
    std::uint64_t boneDigest = 0;

    // 源文件 FNV-1a（从 `.vmesh` 提取时沿用它的 sourceHash）。
    std::uint64_t sourceHash = 0;

    // 诊断（与 `SourceModel` 同一套约定）
    bool ok = false;
    std::string error;

    int jointCount() const { return static_cast<int>(joints.size()); }
    bool valid() const { return !joints.empty(); }
    int findJoint(const std::string& name) const;
    int findSocket(const std::string& name) const;
};

// 骨表摘要。**顺序敏感**（重排骨表 = 顶点关节下标含义变了 = 不是同一套）。
std::uint64_t skeletonBoneDigest(const std::vector<SourceJoint>& joints);

// ------------------------------------------------------------ 默认挂点
//
// UE5 Mannequin 的挂点约定：`weapon_r` 挂 `hand_r`、`weapon_l` 挂 `hand_l`，
// 另外加一个 `eye` 挂 `head`（视线/相机锚点）。
//
// 骨名按**候选表**解析（`hand_r` → `RightHand` → `mixamorig:RightHand`）：
// 同一个挂点概念在不同来源的资产上落在不同名字的骨上，找不到就**不建**这个
// 挂点（而不是建一个挂在 -1 号骨上的空挂点 —— 那种东西查起来是灾难）。
//
// 偏移一律取单位阵：UE 的 `SK_Mannequin` 上 `weapon_r` 的偏移是美术在
// Skeleton 资产里手调的，我们没有那个数值。编一个"看起来像握着枪"的偏移
// 比留 0 更糟 —— 它会让下一个人以为那是从 UE 抄来的。
void applyDefaultSockets(const std::vector<SourceJoint>& joints,
                         std::vector<SourceSocket>& out);

// ------------------------------------------------------------ 读写
std::vector<std::uint8_t> writeSkelFile(const SourceSkeletonAsset& skel);

// 失败时 `err` 是**具体到区块**的原因。
bool readSkelFile(const void* data, std::size_t size, SourceSkeletonAsset& out,
                  std::string* err = nullptr);

// 只读头 / 区块表 + BONES 区块的计数，不解整张骨表。
struct SkelFileInfo {
    std::uint32_t version = 0;
    std::uint32_t flags = 0;
    std::uint64_t sourceHash = 0;
    std::uint64_t boneDigest = 0;
    std::uint32_t chunkCount = 0;
    std::uint32_t jointCount = 0;
    std::uint32_t topoCount = 0;
    std::uint32_t socketCount = 0;
    bool hasEngineXform = false;
    std::uint64_t bytes = 0;
};
bool inspectSkelFile(const void* data, std::size_t size, SkelFileInfo& info,
                     std::string* err = nullptr);

// `x/y/alicia.vmesh` → `x/y/alicia.vskel`（换扩展名）
std::string vskelPathFor(const std::string& sourcePath);

// 供错误信息用：区块 id → 名字
const char* skelSectionName(std::uint32_t id);

} // namespace assetio
