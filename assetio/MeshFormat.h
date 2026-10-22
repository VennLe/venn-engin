#pragma once
// ============================================================
// assetio/MeshFormat —— 自研模型二进制 `.vmesh`（格式版本 1）
//
// ---------------------------- 为什么要自研 ----------------------------
// 不是"不想用 glTF"。而是运行时**不该**再解析一遍创作格式：
//   · FBX 是几十种曲线/插值/表达式的集合，ufbx 再快也要建整棵
//     节点树、解 ASCII 或二进制 chunk、跑坐标系换算；
//   · 这些东西**每次启动**都要重算一遍，而结果一个字都不会变。
// 把"解析 + 修正"的结果一次烤进 `.vmesh`，运行时只剩 memcpy。
//
// ---------------------------- 容器结构 ----------------------------
//   [ 32B 文件头 ]
//   [ N × 32B 区块表 ]
//   [ 区块 0 ][ 区块 1 ] ...     每个 8 字节对齐
//
// 为什么是"区块表 + 偏移"而不是"顺序拼接"：
//   顺序拼接一旦某个区块的语义扩展了（比如 MESH 里加个字段），
//   老版本读新文件就会从中间开始错位，只能整体报"文件损坏"。
//   有了区块表，读端可以**只取自己认识的区块**，其余跳过 ——
//   这正是"版本不对"和"文件截断"能分开报的前提。
//
// 每个区块带 CRC32：坏一个字节就能立刻定位到**哪个区块**坏了，
// 而不是等到渲染出一堆 NaN 三角形再去猜。
//
// ---------------------------- 版本策略 ----------------------------
//   文件头 version  = 容器版本（区块表布局）。变了 = 老读端直接拒。
//   区块头 version  = 该区块自身的布局版本。变了 = 只有用它的人受
//                     影响，其余区块照读。
// 两者分开，是为了避免"材质加了一个字段 → 所有模型都得重烤"。
// ============================================================

#include "assetio/SourceModel.h"

#include <cstdint>
#include <string>
#include <vector>

namespace assetio {

constexpr std::uint32_t kMeshFileVersion = 1;
constexpr char kMeshFileMagic[8] = {'V', 'N', 'M', 'E', 'S', 'H', '\x1A', '\0'};

// 区块 id。**不要复用**已经用过的数字（删掉的功能也要留着占位），
// 否则老文件里的 id 会被新语义解释成完全不同的东西。
enum SectionId : std::uint32_t {
    kSectionMesh = 1,
    kSectionSkeleton = 2,
    kSectionAnimation = 3,
    kSectionMaterial = 4,
    kSectionString = 5,
};

// 文件级标志
enum FileFlags : std::uint32_t {
    kFlagSkinned = 1u << 0,
    kFlagAnimation = 1u << 1,
    kFlagOptimized = 1u << 2,   // 已做过焊接 + 缓存重排
    kFlagIndex16 = 1u << 3,     // 全部索引都能塞进 u16
};

struct WriteOptions {
    bool weld = true;            // 合并属性相同的顶点
    bool optimizeCache = true;   // Forsyth 缓存重排
    bool reduceAnimation = true; // 关键帧抽帧
    float rotationTolDeg = 0.1f;
    float positionTol = 1.0e-4f;
    float scaleTol = 1.0e-4f;
};

// 写盘统计（`AssetC --verbose` 打印，回归脚本也用它断言"优化真的生效"）
struct WriteStats {
    std::size_t bytes = 0;
    std::size_t sections = 0;
    std::uint32_t verticesIn = 0;
    std::uint32_t verticesOut = 0;
    std::uint32_t indicesIn = 0;
    std::uint32_t indicesOut = 0;
    // ⚠ 必须从 0 起累加。这两个字段曾经默认 3.0（"最坏情况"），而
    //   writeMeshFile 是 `st.acmrIn += os.acmrIn` —— 于是每个模型都凭空
    //   多了 3.0/submeshCount（7 个子网格时虚高 0.43）。`acmr` 是用来
    //   判断"重排有没有用"的指标，虚高的基线会让"优化无效"也看起来
    //   有效。被判据依赖的数字，默认值就是它的一部分。
    float acmrIn = 0.0f;
    float acmrOut = 0.0f;
    std::size_t keysIn = 0;
    std::size_t keysOut = 0;
    std::uint32_t subMeshes = 0;
    std::uint32_t joints = 0;
    std::uint32_t clips = 0;
    std::uint32_t materials = 0;
    std::uint32_t rawGeometryBytes = 0;   // 未压缩的顶点+索引字节数
};

// 只读的"体检报告"（`AssetC --info`，不需要把整个模型读进内存）
struct MeshFileInfo {
    std::uint32_t version = 0;
    std::uint32_t flags = 0;
    std::uint64_t sourceHash = 0;
    std::uint32_t sectionCount = 0;
    std::uint32_t subMeshCount = 0;
    std::uint32_t jointCount = 0;
    std::uint32_t clipCount = 0;
    std::uint32_t materialCount = 0;
    std::uint64_t vertexCount = 0;
    std::uint64_t indexCount = 0;
    std::uint64_t bytes = 0;
    bool index16 = false;
};

// 序列化。**按值传** `model`：优化会就地改几何/关键帧，传引用的话
// 调用方会在不知情的情况下拿到一份"被削过"的数据。
std::vector<std::uint8_t> writeMeshFile(SourceModel model,
                                        const WriteOptions& opt,
                                        WriteStats* stats = nullptr);

// 反序列化。失败时 `err` 里是**具体到区块**的原因。
bool readMeshFile(const void* data, std::size_t size, SourceModel& out,
                  std::string* err = nullptr);

// 只读头/区块表，不解几何数据。
bool inspectMeshFile(const void* data, std::size_t size, MeshFileInfo& info,
                     std::string* err = nullptr);

// `models/girl.fbx` → `models/girl.vmesh`（换扩展名）
std::string vmeshPathFor(const std::string& sourcePath);

// 供 AssetC 的 `--info` 与错误信息用：区块 id → 名字
const char* sectionName(std::uint32_t id);

} // namespace assetio
