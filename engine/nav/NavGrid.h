#pragma once
// ============================================================
// nav/NavGrid —— 格子导航网格 + A* / 视线 / 平滑（C8-1）
//
// ------------------------------------------------------------
// 为什么是"格子"而不是三角网 navmesh
// ------------------------------------------------------------
// 三角网 navmesh（Recast 那一路）要先做体素化 → 区域划分 → 轮廓提取
// → 多边形三角化，四步每一步都是几千行，而且**每一个中间结果都需要一套
// 自己的人工检查工具**才敢改。这个引擎的物理世界本来就已经有"射一条
// 向下的线，拿到地面高度与法线"这件事（`PhysicsWorld::raycast`），而
// 关卡是**编辑器里手搭的、格子友好的**（大世界/室内/棋盘）。
//
// 所以第一版取格子法：**一个格子就是一个可行走采样点**。代价是斜向
// 贴墙会有一格左右的抖动（关掉斜向切角处理就能忍），换来的是：
//   · 全部是 `std::vector<uint8_t>`，可以整份塞进存档 / 打印 / 画出来；
//   · A* 与视线判定是**纯函数**，`--selftest nav` 不需要设备、不需要窗口；
//   · 出了 bug 能靠"把格子打印成一张 ASCII 图"定位 —— 三角网做不到。
//
// ⚠ 本文件**不 include 任何 physics / rhi / scene**。烘焙（要射线的
//   那一半）在 `nav/NavBake.h`，代理移动在 `nav/NavSystem.h`。
//   把射线拉进这里就等于把"能无设备自检"这条丢掉 —— 而这正是选格子法
//   的第一个理由。
//
// ------------------------------------------------------------
// 坐标与高度
// ------------------------------------------------------------
//   · 世界是 **右手系 Z-up**（见项目笔记）。格子铺在 XY 平面上，每格存
//     一个地面高度 `height`。所以导航是"2.5D"的：走不了桥下/楼上同一
//     个 XY 的两层（第一版非目标，见 docs/navigation.md §7）。
//   · `origin` 是格子 (0,0) 的**角点**（不是中心）。格心 =
//     `origin + ((c+0.5)*cellSize, (r+0.5)*cellSize, 0)`。
//   · `cell[(r*cols)+c]` = `NavCell::Walk` / `Blocked`。
// ============================================================

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace nav {

enum class NavCell : std::uint8_t {
    Blocked = 0,
    Walk = 1,
};

// 一次烘焙/建网要用到的形状参数。
//
// ⚠ `cellSize` 同时是**移动精度**与**内存**：0.5 m、200×200 米 = 160k 格
//   = 160 KB 状态 + 640 KB 高度，A* 展开几千个节点是毫秒级。0.25 m 会把
//   这两项各乘 4，收益只是"贴墙更紧"。
struct NavGridDesc {
    glm::vec3 min{-24.0f, -24.0f, -60.0f};
    glm::vec3 max{24.0f, 24.0f, 60.0f};
    float cellSize = 0.5f;
    // 代理尺寸：半径为 0 时只检查"这一格有没有地面"（烘焙用射线的圆形
    // 采样近似掉墙；见 NavBake）。
    float agentRadius = 0.35f;
    float agentHeight = 1.7f;
    float maxSlopeDeg = 46.0f;
    // 相邻格能跨过去的**最大高度差**。台阶 0.35 m 的关卡要 ≥ 0.4；
    // 设小了表现为"每个台阶都变成一堵墙"，设大了表现为"能走上垂直墙面"。
    float maxStep = 0.4f;
};

struct NavGrid {
    glm::vec3 origin{0.0f};
    float cellSize = 0.5f;
    int cols = 0;
    int rows = 0;
    float maxStep = 0.4f;
    std::vector<std::uint8_t> cell;   // rows*cols，见 NavCell
    std::vector<float> height;        // rows*cols；Blocked 格的值无意义

    bool valid() const {
        return cols > 0 && rows > 0 &&
               cell.size() == static_cast<std::size_t>(cols) * rows &&
               height.size() == cell.size() && cellSize > 1e-6f;
    }
    int index(int c, int r) const { return r * cols + c; }
    bool inBounds(int c, int r) const { return c >= 0 && r >= 0 && c < cols && r < rows; }
    bool walkable(int c, int r) const {
        return inBounds(c, r) && cell[static_cast<std::size_t>(index(c, r))] != 0;
    }
    float heightAt(int c, int r) const { return height[static_cast<std::size_t>(index(c, r))]; }
    void set(int c, int r, bool walk, float h) {
        const std::size_t i = static_cast<std::size_t>(index(c, r));
        cell[i] = walk ? 1u : 0u;
        height[i] = h;
    }

    // 世界点 → 格子（可能越界，调用方自己 inBounds）。
    glm::ivec2 cellOf(const glm::vec3& p) const {
        return glm::ivec2(static_cast<int>(std::floor((p.x - origin.x) / cellSize)),
                          static_cast<int>(std::floor((p.y - origin.y) / cellSize)));
    }
    glm::vec3 centerOf(int c, int r) const {
        return glm::vec3(origin.x + (static_cast<float>(c) + 0.5f) * cellSize,
                         origin.y + (static_cast<float>(r) + 0.5f) * cellSize,
                         heightAt(c, r));
    }
    // 网格覆盖的世界矩形（画/裁剪用）
    glm::vec3 worldMin() const { return origin; }
    glm::vec3 worldMax() const {
        return glm::vec3(origin.x + static_cast<float>(cols) * cellSize,
                         origin.y + static_cast<float>(rows) * cellSize,
                         origin.z);
    }
    std::size_t walkableCount() const;

    // 统计（日志 / 面板）
    NavGridDesc desc() const;   // 从网格反推一个 desc（只填 min/max/cellSize/maxStep）
};

// 从一张**高度数组**建网（纯 CPU，不需要物理）。
//
// 语义：`heights` 为空 / 值为 NaN / 小于 `min.z - 1` 的格记为 Blocked。
// 这条路径存在的理由是**自检与地形**：`world_scale` 的高度场已经能给出
// 每个采样的地面高度，不需要为了建一次网去造一个物理世界。
//
// 返回 false = 参数非法（尺寸不一致 / cellSize ≤ 0 / 范围退化）。
bool buildFromHeights(NavGrid& out, const NavGridDesc& d, const std::vector<float>& heights);

// ---- 寻路 ----------------------------------------------------------------
struct NavPath {
    std::vector<glm::vec3> pts;    // 世界坐标（z = 该格地面高度）
    bool reached = false;          // 终点格是否可达（false = 只走到最近的可达点）
    int  expanded = 0;             // A* 展开了多少个节点（性能 / 用例判据）
    float length = 0.0f;           // 折线长度（米，含 z 差）
    void clear() {
        pts.clear();
        reached = false;
        expanded = 0;
        length = 0.0f;
    }
};

// 最靠近 p 的可行走格（BFS 一圈圈外扩，半径上限 `maxRadius` 米）。
// 返回 false = 半径内一格都没有。
bool nearestWalkable(const NavGrid& g, const glm::vec3& p, float maxRadius, glm::ivec2& out);

// a → b 之间"能直线走过去吗"。
//
// 判据是**沿线段按 cellSize/2 采样，每一格都必须可行走，且相邻采样点
// 高度差 ≤ maxStep**。不做真正的胶囊扫掠（没有物理）—— 于是它的语义是
// "这条线不会跨过墙/坑"，用于路径平滑，不用于碰撞。
bool lineWalkable(const NavGrid& g, const glm::vec3& a, const glm::vec3& b);

// A*（8 邻域，octile 启发）。
//
// ⚠ 三条刻意的规矩（每一条都对应一种"看起来能用但很怪"的路径）：
//   ① **不许斜向切角**：斜着迈的那一步，两个正交邻格也必须可行走。
//      否则路径会从两堵墙的对角缝里"穿"过去，走起来是撞墙角。
//   ② **不许越过 maxStep**：邻格高差超了当成不可通行的边（不是"格不可走"）。
//   ③ 队列用 `(f, index)` 全序，不用 `(f)` 一个数比 —— 相等 f 的节点
//      按什么顺序展开决定了路径形状，若靠堆的内部顺序，同一张图两次跑
//      得出两条不同路径，用例就会偶发红。
//
// 起点/终点会先吸附到最近可行走格（≤ 2 格 = 2*cellSize 米）。
bool findPath(const NavGrid& g, const glm::vec3& from, const glm::vec3& to, NavPath& out);

// 视线平滑：贪心地"能直连就直连"，把 A* 的锯齿折成几段长直线。
// 输入必须已经含首尾点；点数 < 3 直接返回。
void smoothPath(const NavGrid& g, NavPath& path);

// 调试：把网格打成 ASCII（`.` 可走 / `#` 可走但被墙挤住…第一版只用 `.` / `#`）。
// 行序是 **r 从大到小**（打印出来与俯视图一致：Y 向上）。
std::string toAscii(const NavGrid& g, int maxCols = 96, int maxRows = 40);

} // namespace nav
