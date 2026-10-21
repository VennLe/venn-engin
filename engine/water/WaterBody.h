#pragma once
// ============================================================
// water/WaterBody —— 一块水体的纯数据描述 + 5 种水质预设
//
// ------------------------------------------------------------
// 为什么水体不写在 `.vwater` 资产里，而是直接挂在组件上
// ------------------------------------------------------------
// 地形是"关卡级"资源（一张 257² 的高度图 + 4 层材质，几十 MB），
// 所以它有 `.vterrain` 资产、有分块 LOD、有磁盘往返。水体不是：
//
//   · 几何是**现生成**的（圆形/矩形网格，最大 128² ≈ 3 万三角形），
//     没有任何需要落盘的纹理；
//   · 全部参数加起来不到 200 字节，比"资产路径 + 反序列化"还短；
//   · 用户的心智模型是"我在视口里拖出一个水塘，然后调它" ——
//     一个实体一块水体，Inspector 里就是它的参数，最直接。
//
// 所以这里只有一个 POD（`WaterBody`），实体持有一份（见
// `ecs::WaterComponent`），存盘走场景 JSON。
//
// ------------------------------------------------------------
// 五种预设不是"换个颜色"，是四组物理量一起变
// ------------------------------------------------------------
// 用户提的五种（山泉 / 湖 / 池塘 / 海 / 河）在真实世界里差的是：
//
//   · **吸收**：越清的吸收越弱 → 浅水也能看到水底。山泉 clarity 最高，
//     池塘最低（藻类悬浮物多，2 m 就看不见底）。
//   · **波长**：海是长涌浪（λ ≈ 30 m），池塘是短毛细波（λ ≈ 1.5 m）。
//   · **流速**：河 1.5 m/s 有明显流向条纹；池塘 0（静水）。
//   · **泡沫**：海浪尖与岸边大片白沫；山泉只有石头附近一点点。
//
// 单一 "color" 参数做不到这些差异 —— 用户说"不同类型的水，其透明度、
// 折射度、流速、水面波纹、涟漪都不一样"，这正是下面五组数的来源。
// ============================================================

#include <glm/glm.hpp>

#include <algorithm>

namespace water {

// 俯视轮廓。两种就够覆盖用户要的"圆形或矩形控制范围"：
//   Circle    —— 水塘 / 湖泊 / 山泉（径向网格，边缘是光滑的圆）
//   Rectangle —— 河道 / 海水（方格网格，长宽独立）
enum class Shape : int {
    Circle = 0,
    Rectangle = 1,
};

// 水质类型。**顺序即面板下拉顺序**，落盘存的是这个整数值。
enum class Kind : int {
    Spring = 0,  // 山泉水
    Lake,        // 湖水
    Pond,        // 池塘水
    Sea,         // 海水
    River,       // 河水
};
constexpr int kKindCount = 5;

inline const char* kindName(Kind k) {
    switch (k) {
        case Kind::Spring: return "Spring water";
        case Kind::Lake:   return "Lake water";
        case Kind::Pond:   return "Pond water";
        case Kind::Sea:    return "Sea water";
        case Kind::River:  return "River water";
    }
    return "Lake water";
}

// 面板上那一排按钮用的短名（2026-10-21）。
// 为什么要有第二个名字：5 个按钮挤在 440px 宽的面板里，各 ~85px ——
// "Spring water" 在那个宽度下会被截成 "Spring wa…"（中英双语都难看）。
// 长名留给统计行（`Kind: Sea water`）和提示文本，按钮只用短名。
inline const char* kindShortName(Kind k) {
    switch (k) {
        case Kind::Spring: return "Spring";
        case Kind::Lake:   return "Lake";
        case Kind::Pond:   return "Pond";
        case Kind::Sea:    return "Sea";
        case Kind::River:  return "River";
    }
    return "Lake";
}

inline const char* kindId(Kind k) {
    switch (k) {
        case Kind::Spring: return "spring";
        case Kind::Lake:   return "lake";
        case Kind::Pond:   return "pond";
        case Kind::Sea:    return "sea";
        case Kind::River:  return "river";
    }
    return "lake";
}

inline Kind kindFromIndex(int v) {
    return static_cast<Kind>(std::clamp(v, 0, kKindCount - 1));
}

// 一块水体。**全部字段都进参数面板**，没有"只有预设才知道"的隐藏项 ——
// 用户改预设之后再手调任何一个滑块都不会被下一次刷新覆盖。
struct WaterBody {
    // ---- 形状（俯视轮廓）----
    Shape shape = Shape::Circle;
    glm::vec2 center{0.0f, 0.0f};   // 世界 xz（米）
    // Circle: x = 半径；Rectangle: x/y = 半长（所以"尺寸"= 2×halfExtent）
    glm::vec2 halfExtent{12.0f, 12.0f};
    float rotation = 0.0f;          // 绕 z 轴（度），只对矩形有意义
    float level = 0.0f;             // 水面世界高度（米）

    // ---- 与地面的关系（2026-10-21 第六轮）----
    // 水体不是"一张漂在空中的纸"：水面的边界下沿必须落在**地形里**，
    // 否则从侧面看就是一块悬空的板子（用户说的"反重力"）。
    //
    // `bedLevel` = footprint 范围内的**最低**地面高度。它有两个用途：
    //   ① 侧壁（水体截面）从这里再往下沉 0.75 m → 任何地形起伏都遮得住；
    //   ② 面板显示"最深水深 = level - bedLevel"。
    // 场景里没有地形时它就是创建时的地面高度 —— 水体照样贴着地。
    float bedLevel = 0.0f;
    // 水位是否跟着地形自动走。创建 / 改范围时会把水面摆到"边缘最低点
    // + 一点余量"，这样水面永远略低于四周地势，不会从岸边溢出去。
    // 用户一旦手动拖过 Water level 滑块就置 false —— 否则每次改半径都会
    // 把他调好的水位顶回去。
    bool autoFit = true;

    // ---- 水质 ----
    Kind kind = Kind::Lake;
    glm::vec3 shallowColor{0.16f, 0.42f, 0.40f};  // 浅处（能看到底）的水色
    glm::vec3 deepColor{0.03f, 0.13f, 0.20f};     // 深处的水色
    // 表面不透明度基线：0 = 完全靠菲涅尔/厚度决定，1 = 立刻变成一堵墙。
    // 真实水体在正视时几乎透明（能看见底），所以默认给得低。
    float opacity = 0.28f;
    float refraction = 0.40f;   // 折射扭曲强度（0..1）
    float ior = 1.333f;         // 折射率（水 1.333，空气 1.0）
    float clarity = 1.0f;       // 清澈度倍率：越大越能看穿（吸收越弱）
    float flowSpeed = 0.0f;     // 表面流速（m/s）
    glm::vec2 flowDir{1.0f, 0.0f};  // 流向（会归一化）
    float waveHeight = 0.12f;   // 涌浪振幅（米，峰谷总高 = 2×它）
    float waveLength = 6.0f;    // 主波长（米）
    float ripple = 0.55f;       // 细波纹 / 涟漪强度（0..2）
    float foam = 0.35f;         // 岸边与浪尖泡沫（0..1）
    float roughness = 0.08f;    // 高光锐度（0 = 镜面，1 = 全漫反射）

    // 有效半径（圆形）或外接半径（矩形）—— 采样网格 / 视锥剔除 / 面板显示共用
    float footprintRadius() const {
        if (shape == Shape::Circle) return std::max(halfExtent.x, 0.5f);
        return std::max(glm::length(halfExtent), 0.5f);
    }

    // 最深水深（米）。水面低于地面时为 0 —— 那种情况下整片水面都被地形
    // 挡住，面板该显示 0 而不是负数。
    float maxDepth() const { return std::max(level - bedLevel, 0.0f); }
};

// 把预设写进 body（只动"水质"那一段，形状/位置/高度保持不动 ——
// 用户在视口里拖出来的范围不该因为换了个水质类型就跑掉）。
inline void applyPreset(WaterBody& b, Kind k) {
    b.kind = k;
    switch (k) {
        case Kind::Spring:
            // 山泉：极清、浅、湍急、短波、几乎没泡沫
            b.shallowColor = glm::vec3(0.42f, 0.72f, 0.68f);
            b.deepColor    = glm::vec3(0.10f, 0.34f, 0.36f);
            b.opacity      = 0.12f;
            b.refraction   = 0.55f;
            b.ior          = 1.333f;
            b.clarity      = 3.2f;
            b.flowSpeed    = 1.10f;
            b.flowDir      = glm::vec2(1.0f, 0.35f);
            b.waveHeight   = 0.05f;
            b.waveLength   = 1.8f;
            b.ripple       = 1.35f;   // 细碎石子的水花感
            b.foam         = 0.12f;
            b.roughness    = 0.04f;
            break;
        case Kind::Lake:
            // 湖：视野开阔、水色偏青绿、慢涌浪、泡沫少
            b.shallowColor = glm::vec3(0.18f, 0.44f, 0.44f);
            b.deepColor    = glm::vec3(0.02f, 0.12f, 0.20f);
            b.opacity      = 0.26f;
            b.refraction   = 0.40f;
            b.ior          = 1.333f;
            b.clarity      = 1.60f;
            b.flowSpeed    = 0.05f;
            b.flowDir      = glm::vec2(1.0f, 0.0f);
            b.waveHeight   = 0.10f;
            b.waveLength   = 6.0f;
            b.ripple       = 0.55f;
            b.foam         = 0.30f;
            b.roughness    = 0.07f;
            break;
        case Kind::Pond:
            // 池塘：死水 + 藻类，2 m 就看不见底，短毛细波，边缘浮沫多
            b.shallowColor = glm::vec3(0.24f, 0.34f, 0.18f);
            b.deepColor    = glm::vec3(0.05f, 0.11f, 0.04f);
            b.opacity      = 0.62f;
            b.refraction   = 0.18f;
            b.ior          = 1.333f;
            b.clarity      = 0.45f;
            b.flowSpeed    = 0.0f;
            b.flowDir      = glm::vec2(1.0f, 0.0f);
            b.waveHeight   = 0.02f;
            b.waveLength   = 1.5f;
            b.ripple       = 0.30f;
            b.foam         = 0.55f;
            b.roughness    = 0.16f;
            break;
        case Kind::Sea:
            // 海：深蓝、长涌浪、大泡沫、强高光
            b.shallowColor = glm::vec3(0.10f, 0.40f, 0.52f);
            b.deepColor    = glm::vec3(0.01f, 0.06f, 0.16f);
            b.opacity      = 0.34f;
            b.refraction   = 0.30f;
            b.ior          = 1.333f;
            b.clarity      = 1.20f;
            b.flowSpeed    = 0.35f;
            b.flowDir      = glm::vec2(0.85f, 0.52f);
            b.waveHeight   = 0.55f;
            b.waveLength   = 26.0f;
            b.ripple       = 0.85f;
            b.foam         = 0.80f;
            b.roughness    = 0.05f;
            break;
        case Kind::River:
            // 河：定向流动、水面被拉成流线、水色偏黄绿（含沙）
            b.shallowColor = glm::vec3(0.30f, 0.44f, 0.28f);
            b.deepColor    = glm::vec3(0.06f, 0.16f, 0.12f);
            b.opacity      = 0.40f;
            b.refraction   = 0.34f;
            b.ior          = 1.333f;
            b.clarity      = 0.90f;
            b.flowSpeed    = 1.50f;
            b.flowDir      = glm::vec2(1.0f, 0.0f);
            b.waveHeight   = 0.08f;
            b.waveLength   = 3.0f;
            b.ripple       = 1.00f;
            b.foam         = 0.35f;
            b.roughness    = 0.10f;
            break;
    }
}

// 默认水体 = 湖（新建时用）
inline WaterBody makeDefault(Kind k = Kind::Lake) {
    WaterBody b;
    applyPreset(b, k);
    return b;
}

} // namespace water
