#pragma once
// ============================================================
// assets/Lod —— 网格细节层级（Level of Detail）
//
// 一个逻辑网格可以挂一条 **LOD 链**：同一形状的若干份网格，从高模到低模
// 依次排列。渲染层每帧算出物体的"屏幕占高比"，从中挑一层画 —— 远处的
// 东西自动切到低模，顶点/三角形数量掉一个量级。
//
// 链的来源有两类（生成在 AssetManager 里，见 AssetManager.h 的 buildLods）：
//
//   * **程序化图元**：拿同样的参数重新生成，只把细分参数逐级减半
//     （球 48×24 → 24×12 → 12×6 → 6×3）。这是真正的低模：形状一致、
//     拓扑干净、UV 连续。立方体/平面本来就已经是最简（12 / 2 个三角形），
//     再减就没有形状了，所以它们**不生成**链（只有 1 层，等于没 LOD）。
//
//   * **导入网格**（OBJ / glTF / FBX 的静态部分）：顶点聚类简化
//     （vertex clustering）—— 把包围盒切成 N³ 的格子，每个格子里落到的
//     顶点合并成一个（位置取平均、法线归一化平均、UV 取平均），原先**跨
//     格子**的三角形保留，落在同一个格子里的三角形塌成一点、直接丢弃。
//     实现简单、对任意拓扑（非流形、多壳、无索引）都成立，缺点是形状精度
//     不如 QEM 边坍缩 —— 但对"远处的小东西"完全够用，作为引擎内建的自动
//     LOD 是合理取舍。
//
// **蒙皮网格不做自动 LOD**（2026-10-24 修正；W2-3 曾经尝试做过，已撤回）：
//
//   W2-3 的实现是按主 joint 分组合并（键 = (主 joint, 空间格)），思路本身
//   没错（跨骨合并会把肢体焊在一起），但它救不了另外两件事：
//
//   ① 选层口径：LOD 按**每个子网格自己的包围球**算屏幕占高比。角色的脸
//      只有 19 cm，第三人称 4 m 外比值 ≈ 0.06，直接落到第 3 级 ——
//      1571 顶点压到 100 上下，"脸上还有没有眼睛"就没法看了。
//   ② UV 被平均：聚类把一格里的 UV 求平均，而脸/眼/眉是**同一张图集上
//      切开的小岛**，平均后采样点落到图集空白处 → 脸变纯白、眼睛消失。
//
//   实测症状（远离时）：脸是一块白板、腿消失、鞋子悬空。修掉缓存键撞车
//   （见 lodKey 的注释）之后这两条依然存在，所以判定为"顶点聚类不适合
//   骨骼网格"。与 UE5 一致：骨骼网格体 LOD 需要专门管线（按 Model 统一
//   选层 + 手动/工具生成），不是自动减面能凑的。
//
//   于是 `ensureLod` 见到 `meshLooksSkinned` 就只建第 0 级，并打
//   `LOD-SKIP-SKIN`。要重新打开，得先有"skeletal mesh LOD"这条路。
//
// 例外：`.vmeta` 里 `import.generateLods=false` 的网格不建链
// （见 `Mesh::noLod()`），与蒙皮无关，是用户明确要求的。
//
// 阈值口径：**屏幕占高比** = 物体投影到视口上的高度 / 视口高度
//            = 包围球半径 / (到相机距离 × tan(fovY/2))
// 一个刚好占满整个视口高度的球，这个值是 0.5。所以阈值都 ≤ 0.5。
// ============================================================

#include "assets/Mesh.h"

#include <cstdint>
#include <vector>

namespace assets {

// 每级的"屏幕占高比"上限：比值 **小于等于** 该值就用这一级。
// level 0 的阈值是"永远满足"，即默认层。
inline constexpr float kLodThresholds[] = {1.0f, 0.25f, 0.10f, 0.04f};
inline constexpr int kMaxLodLevels = 4;

// 迟滞系数：切换层级要"越过阈值的这个倍数"，避免物体正好骑在阈值上时
// 每帧来回跳（跳一下就要换网格 + 重绑 GPU 缓冲，是实打实的开销，而且
// 视觉上会闪）。往下切（变低模）要求比值 ≤ 阈/1.15，往回切要求 ≥ 阈×1.15。
inline constexpr float kLodHysteresis = 1.15f;

struct LodLevel {
    Mesh* mesh = nullptr;
    uint32_t vertices = 0;
    uint32_t triangles = 0;
    // **进入本级的阈值**：屏幕上占得比它还小（ratio < switchRatio）就用本级。
    // 于是各级的适用区间是 [switchRatio[i+1], switchRatio[i])。
    //
    // 第 0 级（高模）没有下级可退，这个值不参与判断 —— 给 1.0 只是占位。
    // 阈值必须**随级数递减**（LodChain::add 会检查）。
    float switchRatio = 1.0f;
};

class LodChain {
public:
    void clear() { m_levels.clear(); }
    // 逐级追加；第 0 级应当第一个加、阈值给 1.0（它不参与判断）
    void add(Mesh* mesh, float switchRatio);
    // 只保留 index < count 的前 count 级
    void truncate(size_t count);

    size_t levelCount() const { return m_levels.size(); }
    // 链有效 = 至少有 2 级，且每级的网格都可用、三角数确实在递减。
    // 只有 1 级的链叫"没生成 LOD"，渲染层直接跳过。
    bool valid() const { return m_levels.size() >= 2; }

    const LodLevel& level(size_t i) const { return m_levels[i]; }
    const std::vector<LodLevel>& levels() const { return m_levels; }

    // 选层。ratio = 屏幕占高比。
    // bias **除**在比值上：bias > 1 让比值变小、更快落到低模（和 Unreal /
    // Unity 的 LOD Bias 同向：数字越大越省、画质越低）。
    // current < 0 表示没有上一帧的选择（即不做迟滞，直接按阈值落位）。
    //
    // 迟滞只影响"从当前层出发"的迁移，因此**同一个 ratio、不同的 current**
    // 可能给出不同的结果 —— 这正是它存在的意义。
    size_t selectLevel(float ratio, float bias, int current) const;

    // 各级三角形之和（调试/统计用）
    uint64_t totalTriangles() const;

private:
    std::vector<LodLevel> m_levels;
};

// ---------------- CPU 侧简化 ----------------

struct LodMeshData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    bool ok = false;   // 简化失败（输入为空 / 参数非法）时为 false
};

// 顶点聚类简化。gridRes = 每个轴切成多少格（越大越保形、压得越少）。
// 返回的数据可直接 Mesh::upload。
//
// 细节：
//   * 顶点位置 = 同格内顶点的**平均**（不是取格心）—— 取格心会让形状整体
//     向格心塌陷、看起来"缩了一圈"；取平均能保住大致的体积。
//   * 法线 = 归一化的算术平均；UV 同理（对平铺贴图会有轻微错位，可接受）。
//   * 落进同一格的三角形退化成零面积，直接丢弃；同时剔除面积小于
//     extent²×1e-8 的退化三角形，避免留下看不到却参与光栅化的垃圾。
//   * 不保留蒙皮属性（joints/weights）—— 蒙皮网格不生成 LOD，见文件头。
LodMeshData decimateClustered(const std::vector<Vertex>& vertices,
                              const std::vector<uint32_t>& indices,
                              int gridRes);

// 从网格自身的顶点/索引做聚类简化（走 hasBounds 算包围盒）
LodMeshData decimateClustered(const Mesh& mesh, int gridRes);

} // namespace assets
