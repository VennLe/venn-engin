#pragma once
// ============================================================
// assetio/VertexOps —— 导入期的几何修正
//
// 四件事，每一件都在**离线**做，运行时不重复：
//   · weldVertices    合并属性完全相同的顶点
//   · recomputeNormals 缺法线时按面积加权重建
//   · optimizeForCache 顶点缓存友好的三角形重排（Forsyth）
//   · measureAcmr     量化上面那一步到底有没有用
//
// 为什么焊接放在这里而不是在 FBX 解析里"顺手做掉"：
//   解析器的任务是**忠实还原文件**，焊接是有损/有策略的（epsilon、
//   哪些属性参与哈希）。把它单拎出来，才能对"焊接前后渲染是否一致"
//   单独下断言 —— 而这一步恰恰是最容易焊出问题的（把硬边焊成软边）。
// ============================================================

#include "assetio/SourceModel.h"

#include <cstdint>
#include <vector>

namespace assetio {

// 一个子网格经过修正后的前后对比。所有字段都是**可断言**的整数/浮点，
// 这样"优化真的生效了"不靠肉眼看日志，而靠脚本比对。
struct OptimizeStats {
    std::uint32_t verticesIn = 0;
    std::uint32_t verticesOut = 0;
    std::uint32_t indicesIn = 0;
    std::uint32_t indicesOut = 0;
    float acmrIn = 0.0f;    // 平均每三角形顶点着色调用次数（越接近 0.5 越好）
    float acmrOut = 0.0f;
    bool index16 = false;   // 索引已能塞进 u16
    bool welded = false;
};

// 顶点缓存模拟。32 是真实的"后变换缓存"典型容量（NVIDIA / AMD 都在
// 24~32 条之间），用它算出来的 ACMR 和 GPU 上的实际值同阶。
constexpr int kPostTransformCacheSize = 32;

// 模拟 FIFO 缓存算 ACMR = 顶点着色次数 / 三角形数。
// 完美复用是 0.5（每个顶点只算一次、每个三角形 3 个顶点），
// 完全不复用是 3.0。这个数直接决定顶点着色器的工作量。
float measureAcmr(const std::vector<std::uint32_t>& indices,
                  std::uint32_t vertexCount,
                  int cacheSize = kPostTransformCacheSize);

// 合并**所有属性**都相同的顶点（位置/法线/UV/关节/权重/颜色）。
//
// ⚠ epsilon 只作用于位置；法线与 UV 用较严的量化：
//   位置放宽是为了吃掉导出器的浮点噪声（同一个点被写成 0.1000001 与
//   0.0999999），而法线一旦放宽，硬边会被焊成软边 —— 那是"看起来
//   变了但说不出哪里不对"的一类回归，宁可不焊。
// ⚠ 蒙皮权重**必须**参与哈希：同一个位置在两组骨骼权重下是两个顶点，
//   焊掉之后角色会有一部分顶点跟着错误的骨头动。
void weldVertices(std::vector<Vertex>& vertices,
                  std::vector<std::uint32_t>& indices,
                  float positionEpsilon = 1.0e-5f);

// 面积加权法线重建（顶点法线 = 相邻三角形面法线 × 面积 的归一化和）。
// 只在"法线全是 (0,0,0)"时才该被调用 —— 有法线就去覆盖它等于
// 把美术做的硬边/自定义法线抹平。
void recomputeNormals(std::vector<Vertex>& vertices,
                      const std::vector<std::uint32_t>& indices);

// 顶点缓存友好的三角形重排（Forsyth 2006）。
// 只重排三角形的**顺序**，不改绕序、不改顶点数据 —— 所以是**无损**的：
// 画出来的东西逐像素相同，只是 GPU 的顶点着色次数变少。
void optimizeForCache(std::vector<std::uint32_t>& indices,
                      std::uint32_t vertexCount);

// 把上面几步串起来（顺序有讲究）：
//   1. 焊接（先减顶点数，后面的缓存模拟才有意义）
//   2. 重排（在最终顶点下标空间上做）
//   3. 索引窄化（只影响写盘时的字节宽度）
OptimizeStats optimizeSubMesh(SourceSubMesh& sm, bool doWeld = true,
                              bool doCache = true);

// 索引是否能塞进 u16
bool indicesFitU16(const std::vector<std::uint32_t>& indices);

} // namespace assetio
