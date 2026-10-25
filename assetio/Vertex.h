#pragma once
// ============================================================
// assetio/Vertex —— 磁盘格式里的顶点（**纯 POD，不依赖 Vulkan**）
//
// 为什么不在格式层直接用 `assets::Vertex`：
//   `engine/assets/Mesh.h` 为了 `bindingDescription()` 拉进了
//   `rhi/VulkanCommon.h`（vulkan.h / vk_mem_alloc.h）。离线编译器
//   （AssetC.exe）是**纯 CPU 工具**，不该为了写一个字节的结构体
//   把整个 Vulkan SDK 和 VMA 拉进 include 图 —— 那样它就不能在没有
//   显卡驱动 / 没有 Vulkan 环境的机器上跑（CI、构建机、美术机器）。
//
// 所以这里重新声明一份，布局与 `assets::Vertex` **逐字节一致**：
//   pos(12) normal(12) uv(8) joints(16) weights(16) color(16)
//   uv2(8) morphBase(4) morphCount(4) tangent(16)          = 112
// 一致性由 `engine/assets/VMeshLoader.cpp` 的 static_assert 双向锁死
// （那边同时看得见两个类型）。一旦有人给 `assets::Vertex` 加字段，
// 编译期就会红，而不是"读出来全是错位的数据"那种运行期怪 bug。
//
// ---------------------------- 2026-10-25：80 → 112（§13）----------------------------
// 顶点格式**一次改对**。三个新通道同时落地，理由不是"顺便"，是
// 重烤的成本结构：改顶点 stride 必须把全部 `.vmesh` 重烤一遍（MESH
// 区块的 stride 字段会直接拒掉旧文件）。分三次做 = 烤三次，而且
// 中间那两次的形状是**没人用过**的，出了问题也定位不到是谁的锅。
//
// 三个通道各自的"有没有"哨兵（都不额外占字节，也都不需要顶点级标志位）：
//
//   uv2        —— 由**材质**决定用不用（面部 SDF 采样要 UV1，见 pbr.frag
//                 的 sdfShadow 分支）。不存在"同一个网格里一半顶点有 UV2、
//                 一半没有"的合法场景，所以不需要逐顶点标志。
//   morphBase  —— `kNoMorphBase`(=0xFFFFFFFF) 表示"这个顶点不参与任何
//                 morph target"。这是个**下标**，0 是合法值，所以必须
//                 留一个哨兵；用 0 当"没有"会让第一个 target 的顶点
//                 静默丢形变。
//   tangent    —— `tangent[3] == 0` 表示"没有切线"。合法的手性只有 ±1，
//                 所以 w=0 是个干净的空位。着色器判 `w != 0` 就知道
//                 能不能用它建 TBN。
//
// 为什么第 92 字节那个 u32 早在 §13 就留了名字（当时叫 reserved0）：
//   108 不是 16 的倍数，而 std430/顶点属性对 vec4 类字段的起始偏移有
//   隐含期待（glm 的 vec4 在所有平台都按 16 对齐最稳）。与其让编译器
//   插一段**未定义内容**的隐式填充（那会让 `AssetSelfTest` 的 memcmp
//   在"数据完全相同"时随机红），不如显式命名一个 4 字节占位。
//   §14 起它叫 `morphCount`：当初留它的目的正是"以后再加一个 u32
//   不必再动 stride"，而 morph 的稀疏流正好需要第二个索引字段。
//   所以这一刀的顶点 stride **仍然是 112**，一个字节都没加。
// ============================================================

#include <cstddef>
#include <cstdint>

namespace assetio {

// morphBase 的哨兵：这个顶点不参与任何 morph target。
// ⚠ 必须和 `engine/assets/Mesh.h` 的同名常量一致（VMeshLoader 里有断言）。
constexpr std::uint32_t kNoMorphBase = 0xFFFFFFFFu;

struct Vertex {
    float pos[3];
    float normal[3];
    float uv[2];
    // 蒙皮：joints 是**骨架内下标**（0..jointCount-1），不是 SSBO 里的
    // 全局槽位。全局槽位在渲染时由 push.skin.x（jointBase）加上去 ——
    // 这样同一份 `.vmesh` 可以被任意多个实体复用，实体之间互不干扰。
    std::uint32_t joints[4];
    float weights[4];
    // 顶点色（Mesh Paint）。非蒙皮/未染色时全 1 = "不染色"。
    float color[4];

    // ---- 以下三个通道是 2026-10-25 追加的（§13），当前**没有消费者** ----
    //
    // 第二套 UV。用途：面部 SDF 阴影（一套 UV 给 albedo、另一套给
    // 头部 un-wrap 的阴影贴图）、以及将来的 lightmap。缺省全 0。
    float uv2[2];
    // morph target 的 delta 流起始下标（进 MORPH 区块的稀疏流）。
    // 语义、哨兵、以及"为什么是下标而不是按 target 数乘出来的偏移"，
    // 见 §13.3 —— 一句话：稀疏流只给真正会动的顶点分配槽位，密集
    // 排布在 50+ 个 shape key 时会吃掉几十 MB。
    // ⚠ 这两个必须带 NSDMI。`std::vector<Vertex>(n)` 走的是**值初始化**，
    //   没有 NSDMI 的成员只会被清零 —— 那会让 morphBase 变成 0，而 0 是
    //   一个合法的流下标（第一个 target 的第一个 delta）。症状是"某些
    //   顶点莫名其妙跟着表情动"，且只在真做了 morph 之后才暴露。
    std::uint32_t morphBase = kNoMorphBase;
    // 该顶点在 delta 流里的**记录条数**（§14）。与 morphBase 严格自洽：
    //   morphBase == kNoMorphBase  ⇔  morphCount == 0
    // 这个不改用"上下顶点 base 相减"来推：那要求顶点按 morphBase 排序，
    // 而顶点顺序由焊接 / 缓存重排决定，不归 morph 管。也不用终止哨兵
    // 记录：一条终止记录要 32 B，比每个顶点多 4 B 贵得多。
    //
    // 本字段在 §13 那一刀里叫 `reserved0`（"必须是 0，预留给标志位"）。
    // 改成 morphCount 是**它的第一次使用**，不是语义漂移：把 count 存在
    // 这里正是当初留这 4 字节的目的（"以后再要一个 u32 不必再动 stride"）。
    // 老 v2 资产的这两个字段恒为（哨兵, 0），读端解释成"没有 morph"——
    // 与真实情况一致，所以**不需要升 MESH 区块版本**。
    std::uint32_t morphCount = 0u;
    // 切线：xyz = 沿 UV.x 方向的单位向量，w = 副切线手性（±1）。
    // 0 = 没有切线（见文件头）。
    float tangent[4];

    // NSDMI 只加在**新**字段上（morphBase / morphCount）。老字段保持
    // "无初值"是有意的：`Vertex v{}` 和 `std::vector<Vertex>(n)` 都会
    // 把它们清零，而 `color` 必须在别处显式设 1（见 makeVertex）——
    // 两者的初始化时机不同，混在一起会让"忘了设 color"变成静默的黑模。
    // （`engine/assets/Vertex` 那边相反，它给 color 也写了 NSDMI，
    //   因为引擎侧到处 `Vertex v;` 默认构造。）
};

constexpr std::size_t kVertexSize = 112;

// 各字段偏移也要对上 —— `assets::Vertex` 的 attributeDescriptions()
// 用的是 offsetof，两边错一个字段就是"法线读成 UV"。
static_assert(sizeof(Vertex) == kVertexSize, "Vertex 必须是 112 字节");
static_assert(offsetof(Vertex, pos) == 0, "pos 偏移");
static_assert(offsetof(Vertex, normal) == 12, "normal 偏移");
static_assert(offsetof(Vertex, uv) == 24, "uv 偏移");
static_assert(offsetof(Vertex, joints) == 32, "joints 偏移");
static_assert(offsetof(Vertex, weights) == 48, "weights 偏移");
static_assert(offsetof(Vertex, color) == 64, "color 偏移");
static_assert(offsetof(Vertex, uv2) == 80, "uv2 偏移");
static_assert(offsetof(Vertex, morphBase) == 88, "morphBase 偏移");
static_assert(offsetof(Vertex, morphCount) == 92, "morphCount 偏移");
static_assert(offsetof(Vertex, tangent) == 96, "tangent 偏移（必须是 16 的倍数）");

inline Vertex makeVertex() {
    Vertex v{};
    v.color[0] = v.color[1] = v.color[2] = v.color[3] = 1.0f;
    return v;
}

} // namespace assetio
