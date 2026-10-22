#pragma once
// ============================================================
// terrain/Terrain —— 地形资产（.vterrain）+ 分块 LOD 网格生成（W4-1 / W4-2）
//
// ------------------------------------------------------------
// 资产三件套
// ------------------------------------------------------------
//   assets/terrain/<name>/
//     height.r32       32 位浮点高度，**列主序**，归一化 0..1
//     splat.rgba8      4 层权重，每个像素 4 字节（R=A 层 G=B 层 B=C 层 A=D 层）
//     terrain.vterrain JSON：尺寸 / 米每像素 / 最大高度 / 4 个层材质 id
//
// 为什么高度用裸 `.r32` 而不是 PNG：
//   16 位 PNG 只有 65536 级，200 m 高度范围下量化步长 3 mm —— 角色走上去
//   能看到台阶，而且"高度图 ↔ 物理"会因此系统性偏 1.5 mm，把 1 cm 的验收
//   判据吃掉 15%。32 位浮点 256² 只有 1 MB，不值得为它省。
//
// 为什么权重是一张 RGBA8 而不是四张灰度图：
//   D5 的决定 —— 地形 4 层需要 albedo×4 + normal×4 + 权重，共 9 张贴图。
//   前向管线的纹理槽已经占满 7 个（`kTextureSetCount`），再叠 4 个 set 会
//   撞上低端设备 `maxBoundDescriptorSets = 8` 的线。把 4 层权重塞进一个
//   像素的四个通道 → 权重只占**一个**纹理槽，地形的整个材质面只需要
//   1 个 set。
//
// ------------------------------------------------------------
// 网格：平坦顶点 + 着色器位移
// ------------------------------------------------------------
// 顶点**不带高度**（pos.z 恒 0）。高度在 `terrain.vert` 里按 uv 采样高度图
// 再位移 —— 这样同一份顶点缓冲在 CPU 侧是"格子"，而起伏完全由高度图决定：
//   · 改高度图不需要重建网格；
//   · 不同 LOD 的同一位置采到**完全相同**的高度（同一个纹素），所以
//     相邻块在共享边上天然对齐 —— 裙边只需要兜住"粗边 vs 细边"的
//     插值误差，不用兜住"两份不同的高度数据"。
//
// 顶点只有 24 字节：`pos(vec3) + uv(vec2) + skirt(float)`。
// 法线也是 shader 里算的（对高度图做中心差分），与
// `physics::normalAt` 用的是**同一个算子** —— 于是"看起来的坡度"与
// "物理判定的坡度"不会打架。
//
// ------------------------------------------------------------
// LOD：块内减采样 + 裙边缝合（D5 指定用 skirt）
// ------------------------------------------------------------
// 地形被切成 `kChunkCells × kChunkCells` 的块，每块预生成 4 级网格
// （步长 1 / 2 / 4 / 8 个格子）。每级都带**四条裙边**：把块的边界再向下
// 拉 `skirtDepth` 米形成一圈垂直的墙。
//
// 为什么用裙边而不是顶点共享：顶点共享要求相邻块的 LOD 差不超过 1，并且
// 要重排索引去构造过渡三角形（quadtree 的经典麻烦）。裙边只是"多画
// 4×2×(n+1) 个顶点"，而且**任意** LOD 差都能兜住 —— 第一版选它。
// ⚠ 裙边能兜住的前提是"插值误差 < skirtDepth"，这条由自检
//   （C3：maxGap < skirtDepth）守着，不是靠眼力。
// ============================================================

#include "physics/Heightfield.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace terrain {

// LOD 级数（步长 1 / 2 / 4 / 8）
static constexpr int kLodCount = 4;
// 每块的格数（LOD0）。必须是 2 的幂，且 (cols-1) 要能被它整除。
static constexpr int kChunkCells = 32;
// 裙边深度（米）。2 m 对 60 m 起伏的地形很宽裕（实测最大插值误差 ~0.4 m）。
static constexpr float kSkirtDepth = 2.0f;

// .vterrain 的解析结果（只读，加载后不再变）
struct TerrainDesc {
    int cols = 0;   // X 方向**采样点数**（不是格数）
    int rows = 0;   // Y 方向采样点数
    float metersPerPixel = 1.0f;
    float maxHeight = 1.0f;   // 归一化高度 → 米
    float splatRepeat = 1.0f; // 权重图的 UV 重复次数（1 = 铺满整块地形）
    float layerTiling = 8.0f; // 每层 albedo 的世界平铺尺寸（米 / 一次重复）

    // 相对 `.vterrain` 所在目录的文件名
    std::string heightFile = "height.r32";
    std::string splatFile = "splat.rgba8";
    // 挖洞遮罩（可选文件）。每采样点 1 字节：255 = 实心，0 = 挖穿。
    // **允许缺失** —— 旧资产没有这个文件，读不到就整张填 255，
    // 与"从来没挖过洞"等价。新增的洞用单独的灰度图而不是复用 splat 的
    // 某个通道：splat 的 RGBA 四个通道分别属于 4 个材质层，任何一路
    // 拿去存可见性都会让"层权重"和"可见性"互相污染 —— 而这两个量的
    // 语义完全无关（挖穿之后那一格的权重根本没有意义）。
    std::string holeFile = "holes.r8";

    // 4 个层材质 id（与 `.vmat` 的 id 同域，用 AssetManager 解析）
    std::array<std::string, 4> layerIds;
};

struct TerrainAsset {
    TerrainDesc desc;
    // 归一化高度，列主序：heights[col * rows + row]
    std::vector<float> heights;
    // RGBA8 权重，同样列主序：splat[(col * rows + row) * 4 + c]
    std::vector<uint8_t> splat;
    // 挖洞遮罩，列主序，每采样点 1 字节（255 = 实心 / 0 = 挖穿）。
    // 长度恒等于 cols*rows —— 缺文件时由 loadTerrain 填满 255，
    // 渲染侧不用判空（少一个分支就少一处"偶尔不 discard"的坑）。
    std::vector<uint8_t> holes;

    std::string path;  // 相对 assets/ 的 .vterrain 路径（日志 / 序列化用）
    bool ok = false;

    // 读盘时发现高度值超出 [0,1]（2026-10-22 第十一轮）。
    //
    // 来源：编辑器的抬升笔刷允许冲到 kMaxNorm = 8（那是"山峰不撞平顶"的
    // 余量，见 LandscapeEditor.cpp 里 kMaxNorm 的说明），而老版本的
    // `saveAssets()` 把这份**未收敛**的数据原样写进了 height.r32。下次读盘
    // 撞上 [0,1] 契约 → 工程直接打不开。
    //
    // 现在 `loadTerrain` 不再硬失败，而是**按最高点重整**：
    //     heights /= hi ， desc.maxHeight *= hi
    // 世界高度是 `h * maxHeight`，两边同乘同除，**结果恒等** —— 地形长得
    // 一模一样，只是数值回到规范区间。置这个位是为了让上层知道
    // "**磁盘上那份还是脏的**，下次保存会写回规范数据"。
    bool rescaled = false;

    glm::vec2 worldSize() const {
        return glm::vec2(static_cast<float>(desc.cols - 1) * desc.metersPerPixel,
                         static_cast<float>(desc.rows - 1) * desc.metersPerPixel);
    }
    // 交给物理层的高度场视图（**不拷贝**，指向本对象内部的 heights）
    physics::Heightfield heightfield() const;
};

// 只读 `.vterrain` 的 JSON（**不读** height.r32 / splat.rgba8）。
//
// 给编辑器的 Inspector 用：那一段只需要"多大、多高、哪 4 层"，把 528 KB
// 的二进制读进面板纯属浪费（而且 panel 里没有地方放它）。
// 校验规则与 `loadTerrain` **完全是同一份**（`parseDesc`），所以面板上
// 显示得出尺寸 ⇔ 真的加载得了 —— 不会出现"显示 257² 但加载报 size_too_small"。
bool loadTerrainDesc(const std::string& relPath, TerrainDesc& out,
                     std::string* err);

// 加载 `.vterrain` 及其两份二进制。失败时 out.ok = false，err 里是原因。
//
// ⚠ 尺寸校验是硬性失败而不是"尽力而为"：heights 少了 1 个字节，后面
//   每一次 heightAt 都会越界读 —— 而越界读在这种裸指针视图里不会崩，
//   只会给出一个看似合理的错误高度。
//
// quiet = true 时**不**打 TERRAIN-LOAD 成功日志。给地形笔刷的原地刷新
// 路径用：那条路径每秒会重读两次，日志会被灌满；它自己有 TERRAIN-REFRESH。
// 失败日志（TERRAIN-FAIL）不受 quiet 影响，永远打。
bool loadTerrain(const std::string& relPath, TerrainAsset& out,
                 std::string* err = nullptr, bool quiet = false);

// ------------------------------------------------------------
// 值域收敛（2026-10-22 第十一轮）
// ------------------------------------------------------------

// 把 `heights` 收敛回 [0,1]，同时把 `desc.maxHeight` 同比放大。
//
// 世界高度 = `h × maxHeight`，两边同乘同除 → **结果恒等**：地形形状、
// 物理高度、撤销栈里的"米"全都不用改，只是数值回到规范区间。
//
// 返回值是实际应用的缩放系数 `inv = 1/峰值`；返回 1.0 表示"本来就在
// 范围内，什么都没改"。调用方拿到它去缩放自己的派生数据（编辑器的撤销栈
// 存的是归一化高度，必须同比例缩，否则撤销一次地形会跳回 8 倍高）。
//
// 为什么这条逻辑住在 terrain 层而不是编辑器：`.r32 ∈ [0,1]` 是**资产契约**。
// 抬升笔刷只是"一个会产生越界值的生产者"，把收敛写在编辑器里的话，
// 以后任何新的写入者（地形导入、程序化生成、脚本）都得把它复制一遍 ——
// 而这个 bug 的原始形态正是"写端和读端各自以为对方会收敛"。
float normalizeHeightRange(TerrainAsset& asset);

// 只改 `.vterrain` 里的 `maxHeight` 一个字段，其余字段原样保留
// （读 JSON → 改一个键 → 写回，不重新手拼，免得漏字段）。
bool rewriteMaxHeight(const std::string& relPath, float maxHeight);

// ------------------------------------------------------------
// 网格
// ------------------------------------------------------------
struct TerrainVertex {
    glm::vec3 pos;  // 格子坐标（z 恒 0，高度在 VS 里加）
    glm::vec2 uv;   // 高度图 / 权重图的采样坐标（纹素中心对齐，见 .cpp）
    float skirt;    // 0 = 表面顶点，1 = 裙边底面顶点
};

struct TerrainMeshData {
    std::vector<TerrainVertex> verts;
    std::vector<std::uint32_t> idx;
    // 该级与**上一级**在共享边上的最大高度差（米）。LOD0 恒 0。
    // 自检拿它与 kSkirtDepth 比 —— 这是"裙边真的够深"的唯一证据。
    float edgeGap = 0.0f;
};

struct TerrainChunk {
    int cx = 0;
    int cy = 0;
    // 块中心（世界 XY，相对地形原点）与包围半径 —— LOD 选择只看这两个数
    glm::vec2 center{0.0f};
    float radius = 0.0f;
    std::array<TerrainMeshData, kLodCount> lods;
};

// 把 (cols-1)×(rows-1) 的格子切成块，逐块逐级生成网格。
// 返回的 vector 长度 = chunksX * chunksY，下标 = cy * chunksX + cx。
std::vector<TerrainChunk> buildChunks(const TerrainAsset& asset);

// 块数（供调用方算 GPU 缓冲大小）
inline int chunkCountX(const TerrainAsset& a) {
    return (a.desc.cols - 1) / kChunkCells;
}
inline int chunkCountY(const TerrainAsset& a) {
    return (a.desc.rows - 1) / kChunkCells;
}

// 距离 → LOD 级。纯函数（自检与渲染共用同一份，避免"两边各写一个公式"）。
//
//   距离 < chunkSize * 1.5   → LOD0
//   每超出一次阈值就降一级   → 阈值每次 ×2
inline int lodForDistance(float dist, float chunkSize, int lodCount = kLodCount) {
    int lod = 0;
    float threshold = chunkSize * 1.5f;
    while (lod + 1 < lodCount && dist > threshold) {
        ++lod;
        threshold *= 2.0f;
    }
    return lod;
}

} // namespace terrain
