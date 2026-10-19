#pragma once
// ============================================================
// physics/Heightfield —— 规则网格高度场 + 查询（G21 / W4-3）
//
// ------------------------------------------------------------
// 为什么单独一个文件、而不是塞进 Narrowphase
// ------------------------------------------------------------
// 高度场**不是**一个刚体形状。它是"一张 2.5D 的地表函数"，所有查询都
// 是 f(x, y) 的解析运算，O(1)，不需要 broadphase、不需要接触流形、
// 也不需要求解器：
//
//   · `heightAt`  —— 双线性插值（角色贴地、放置物体、撒植被都靠它）
//   · `normalAt`  —— 中心差分（坡度判定，60° 站不住就靠它）
//   · `raycast`   —— 沿射线步进 + 二分细化（编辑器的点击、角色的向下探测）
//
// 真要把高度场切成几万个三角形塞进 broadphase，会得到同一组答案，代价
// 是每帧几万个 AABB 与接触测试。所以第一版**明确不支持**刚体在高度场上
// 滑动（见 docs/world_scale_roadmap.md §9 W4-3 的非目标）。
//
// ------------------------------------------------------------
// 存储布局：列主序（column-major）
// ------------------------------------------------------------
//     heights[col * rows + row]
//
// 也就是"先沿 Y 走完一列，再进下一列"。这是 `.r32` 文件里的原始顺序
// ——**加载时不做任何重排**，所以"磁盘上的字节 ↔ 内存里的下标"是一一
// 对应的（W4 验收：地形与高度图逐像素对应）。
//
// ------------------------------------------------------------
// ⚠ 数值值与米的分离
// ------------------------------------------------------------
// `.r32` 里存的是**归一化**高度（0..1），乘以 `heightScale`（= `.vterrain`
// 里的 `maxHeight`）才是米。这样同一张高度图配不同的 maxHeight 就能得到
// 不同的起伏幅度，而**插值永远在归一化域里做**（不会因为换了倍率而改变
// 采样点的相对权重 —— 线性运算其实无所谓，但让 shader 侧与 CPU 侧完全
// 同构更省心）。
// ============================================================

#include <glm/glm.hpp>

namespace physics {

struct Heightfield {
    // 采样点数（**不是**格子数）。256 格 → 257 个采样点。
    // 用 2^n+1 是因为地形网格按 (cols-1)/kChunkCells 分块，块边界必须落在
    // 采样点上，否则相邻块的裙边对不齐。
    int cols = 0;
    int rows = 0;
    float metersPerPixel = 1.0f;  // 相邻采样点的世界距离（米）
    float heightScale = 1.0f;     // 归一化高度 → 米（= .vterrain 的 maxHeight）
    glm::vec2 origin{0.0f};       // 采样点 (0,0) 的世界 XY
    const float* heights = nullptr;  // cols*rows，列主序

    bool valid() const {
        return cols >= 2 && rows >= 2 && heights != nullptr &&
               metersPerPixel > 0.0f;
    }
    int sampleCount() const { return cols * rows; }

    // 世界尺寸（米）：(cols-1) 个间隔
    glm::vec2 worldSize() const {
        return glm::vec2(static_cast<float>(cols - 1) * metersPerPixel,
                         static_cast<float>(rows - 1) * metersPerPixel);
    }

    // 越界**钳制**到边缘。地形之外沿用边界高度 —— 这比返回 0 安全得多：
    // 返回 0 会让"走出地形一步"变成掉进坑里。
    float raw(int col, int row) const;
    float height(int col, int row) const { return raw(col, row) * heightScale; }

    // 世界 XY → **连续**采样坐标（可以越界、可以是负的；调用方自己决定
    // 钳制还是放弃）。整数部分 = 左下角采样点，小数部分 = 插值权重。
    glm::vec2 gridOf(const glm::vec2& worldXY) const {
        return (worldXY - origin) / metersPerPixel;
    }
    // 采样点的世界坐标（z = 该点高度）
    glm::vec3 worldOf(int col, int row) const {
        return glm::vec3(origin.x + static_cast<float>(col) * metersPerPixel,
                         origin.y + static_cast<float>(row) * metersPerPixel,
                         height(col, row));
    }
};

// 双线性插值，O(1)。
//
// ⚠ 与 `terrain.vert` 里的顶点位移**必须同构**：GPU 侧采样用
//   `uv = (grid + 0.5) / vec2(cols, rows)`（纹素中心对齐），
//   等效于这里的"四个角点按小数部分加权"。
//   两侧不对齐 → 角色站在地面上会浮空/陷进地面几厘米，
//   而且看起来像"物理错了"，其实是采样约定错了。
float heightAt(const Heightfield& hf, float x, float y);

// 法线：中心差分（与 shader 侧一致）。
// 步长取半个像素 —— 比一个像素小会让法线对单个采样点的噪声过敏，
// 大太多又会把陡坡磨平。
glm::vec3 normalAt(const Heightfield& hf, float x, float y);

struct HeightHit {
    bool hit = false;
    float t = 0.0f;        // 沿射线的距离（dir 已归一化时就是米）
    glm::vec3 point{0.0f};
    glm::vec3 normal{0.0f};
};

// 射线 vs 高度场。dir 会**归一化**；maxT <= 0 表示"不限"。
//
// 做法：沿射线以 ~半像素的步长推进，找 f(t) = ray.z - height(x(t), y(t)) 的
// 符号翻转，再在翻转区间里二分 24 次。步进而不是解方程，是因为高度场是
// 双线性分片函数 —— 逐格解是可能的，但边界情形的代码量远超收益。
//
// ⚠ 从地表**以下**出发（f(0) < 0）直接判定为命中 t=0：那意味着"起点已经在
//   地下了"，继续步进会走到地形外面才发现符号翻转，返回一个荒谬的远点。
bool raycast(const Heightfield& hf, const glm::vec3& origin,
             const glm::vec3& dir, float maxT, HeightHit& out);

} // namespace physics
