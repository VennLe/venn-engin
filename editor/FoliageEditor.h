#pragma once
// ============================================================
// editor/FoliageEditor —— 植被编辑模式（顶栏 "Foliage"）
//
// ------------------------------------------------------------
// 为什么植被是"一堆普通实体"而不是一个新的渲染系统
// ------------------------------------------------------------
// 一个独立的植被渲染系统要有：实例缓冲、逐实例视锥剔除、LOD 交叉淡入、
// 风场着色器、按距离的密度衰减 —— 那是 W5 的一整轮活。而本引擎的
// `Renderer::buildOpaqueBatches` **已经**把"同网格 + 同材质"的实体合成
// 一次 instanced draw，所以几百株草在 GPU 侧本来就是一次绘制。
//
// 于是这一版直接把每株草做成一个实体：
//   · 复用现有的拾取 / 层级 / 序列化 / 阴影 / 剔除 —— 一行都没抄；
//   · 存盘即存盘（不像自建缓冲那样要额外写序列化）；
//   · 代价是 CPU 侧的 DrawItem 数量随株数线性增长（几千株没问题，
//     上万株就该上真系统了）。
//
// ------------------------------------------------------------
// "看起来不像撒上去的"是怎么解决的
// ------------------------------------------------------------
// 第一版只有"同一种网格 + 同一个纯色材质 + 严格贴合法线"，撒出来一片死板。
// 这一版补了四件事，正好对应人眼判断"这是不是自然分布"的四个线索：
//   1. **逐实例颜色抖动** —— 5 个材质变体（亮度/色相微差），随机分配。
//      没有它，一片草是同一个绿，无论密度多高都像贴图平铺。
//   2. **随机倾斜** —— 完全对齐法线的草像列队，加 ±jitter 之后才有"长出来"的感觉。
//   3. **坡度/高度过滤** —— 陡壁上不长草、雪线以上不长树。这是最强的真实感线索：
//      植被与地貌的对应关系比植被本身更像"真的"。
//   4. **边缘密度衰减** —— 笔刷边缘硬切会出现一个明显的圆盘边界。
//
// ------------------------------------------------------------
// 归属用一个文件夹实体
// ------------------------------------------------------------
// 所有实例都挂在名为 "Foliage" 的分组实体下（`HierarchyComponent::group`
// = true，层级面板显示成目录）。于是：
//   · 层级面板可以整个折叠起来，不占地方；
//   · "擦除 / 清空"只需要遍历子节点，不必给每株草挂一个标记组件
//     （也就**不需要**改场景序列化 —— 加组件是要进 SceneSerializer 的）。
// ============================================================

#include "EditorContext.h"
#include "LandscapeEditor.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

struct ImDrawList;

namespace editor {

class FoliageEditor {
public:
    FoliageEditor(EditorContext& ctx, LandscapeEditor& land)
        : m_ctx(ctx), m_land(land) {}

    void drawPanel();
    // 视口里的一帧输入。返回 true = 吃掉左键（视口不该再点选）
    bool viewportInput(const glm::vec2& vpPos, const glm::vec2& vpSize,
                       bool hovered, ImDrawList* dl);

    // 每帧同步：找到/建立 "Foliage" 分组，重新统计株数
    void sync();

    int totalCount() const { return m_total; }

private:
    // 五种内置植被。全部程序化生成，不依赖任何美术资产。
    //
    // 每种都带自己的"生态约束"（maxSlopeDeg / minH / maxH）而不是全局一个：
    // 草能长在较陡的坡上，树不行；石头哪都能待。写进表里之后，撒点的过滤
    // 逻辑只有一份，加新种类只要加一行。
    struct Kind {
        const char* name;      // 显示名
        const char* id;        // 日志 / 材质键用的稳定 id
        const char* shape;     // 内置网格形状（"grass"/"sphere"/"tree"/"flower"）
        float size;            // 网格自身高度（米），实例再乘随机缩放
        glm::vec4 color;       // 基色
        float roughness;
        int density;           // 一次落笔撒几株（初值，面板可改）
        float minScale;
        float maxScale;
        float squashY;         // 竖向压扁（岩石 0.55，其余 1）
        float maxSlopeDeg;     // 超过这个坡度就不长
        float minH;            // 高度带下限（相对地形高度范围，0..1）
        float maxH;            // 高度带上限
        int variants;          // 颜色变体数（材质实例数）
    };
    static const Kind& kind(int i);
    static constexpr int kKindCount = 5;

    void drawPalette();
    void drawBrush();
    void drawFilters();
    void drawStats();

    ecs::Entity ensureRoot();
    void recount(bool logCounts);

    // 一次落笔（撒 or 擦）
    void stamp(const glm::vec3& worldHit, bool erase);
    void spawn(int k, const glm::vec3& pos, const glm::vec3& normal,
               float scale, float yaw, int variant, float lateral);
    void eraseAt(const glm::vec3& worldHit, bool anyKind);
    void clearAll(int kindIndex);

    assets::Mesh* meshFor(int k);
    // 每个 (种类, 变体) 一份材质 —— 变体就是"同一种草的五种绿"
    assets::Material* materialFor(int k, int variant);
    float rnd();   // [0,1)，确定性 xorshift

    EditorContext& m_ctx;
    LandscapeEditor& m_land;

    ecs::Entity m_root{};
    std::string m_rootName = "Foliage";

    // ---- 笔刷 ----
    int m_kind = 0;              // 0 草 / 1 灌木 / 2 树 / 3 花 / 4 石头
    float m_radius = 10.0f;      // 米
    int m_density = 24;          // 每次落笔株数
    float m_minScale = 0.7f;
    float m_maxScale = 1.4f;
    float m_tiltJitter = 0.35f;  // 0 = 严格贴合法线，1 = ±14° 随机歪
    float m_edgeFalloff = 0.15f; // 0 = 均匀，1 = 全挤在笔心
    bool m_alignNormal = true;   // 贴合地形坡度
    bool m_eraseMode = false;
    bool m_eraseAllKinds = false;
    bool m_filterSlope = true;
    bool m_filterHeight = true;

    // ---- 拖拽状态 ----
    bool m_stroking = false;
    glm::vec3 m_lastStamp{0.0f};
    uint32_t m_rng = 0x9E3779B9u;

    // ---- 统计 ----
    int m_total = 0;
    int m_counts[kKindCount] = {0, 0, 0, 0, 0};
    int m_spawnSeq = 0;
    int m_lastLoggedTotal = -1;
};

} // namespace editor
