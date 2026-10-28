#pragma once
// ============================================================
// editor/FoliageEditor —— 植被编辑模式（左栏 Modes 里的 "Foliage"）
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
// 2026-10-28：按 UE5 的 Foliage 模式重做面板
// ------------------------------------------------------------
// 用户原话：「Modes 中的 Foliage 面板需要做重新设计优化，请参考 UE5 中对应的
// Foliage 功能去做全面的改造升级」。
//
// UE5 的 Foliage 面板有四个区块，这一版一一对上：
//
//   UE5                        本文件
//   ──────────────────────────────────────────────────────────
//   顶部工具条（Select /      `Tool` 四态 + `drawToolRow()`
//     Paint / Erase / Lasso      （Lasso 框选没有，换成单选 Single
//     / Single）                  ——"点一株"在撒树的时候比框选常用）
//   Foliage 类型树（每行带     `drawTypeList()`：眼睛（可见性）+ 名字 +
//     眼睛 + 名字 + 数量）        现有株数，点行 = 选中该类型
//   Brush 设置（Size /         `drawBrushSection()`：Size / Falloff /
//     Falloff / Density）         Per stamp
//   Details（选中类型的       `drawTypeSettings()`：密度 / 缩放范围 /
//     Mesh/Density/Scale/        随机倾斜 / 贴坡 / 坡度上限 / 高度带 /
//     Align/Cull/...）            颜色变体 + Reapply
//
// 三处"照 UE 抄下来才知道为什么"的设计：
//   1. **参数是"每类型"的，不是全局的**。过去切一次类型就把密度/缩放覆盖成
//      该类型的默认值 —— 用户给树调好的参数，切到草再切回来就没了。
//      现在 `m_types[]` 每类一份，切类型只换"当前编辑对象"。
//   2. **眼睛开关 = 可见性，不是删除**。隐藏的实例仍在场景里（存盘也还在），
//      只是重建网格时不画 —— "我先看看没有灌木的样子"不该要求先删掉再撒回来。
//   3. **Reapply**。改完密度/倾斜之后，已经撒下去的那些不会自己变。UE 的
//      Reapply 就是"把当前设置重新套到已有实例上"——没有它，用户只能清空重撒。
//
// ------------------------------------------------------------
// "看起来不像撒上去的"是怎么解决的
// ------------------------------------------------------------
// 第一版只有"同一种网格 + 同一个纯色材质 + 严格贴合法线"，撒出来一片死板。
// 这一版补了四件事，正好对应人眼判断"这是不是自然分布"的四个线索：
//   1. **逐实例颜色抖动** —— 5 个材质变体（亮度/色相微差），随机分配。
//   2. **随机倾斜** —— 完全对齐法线的草像列队，加 ±jitter 之后才有"长出来"的感觉。
//   3. **坡度/高度过滤** —— 陡壁上不长草、雪线以上不长树。这是最强的真实感线索。
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
//
// ⚠ 可见性（`TypeSettings::visible`）住在编辑器里，**不进场景文件**：
//   它和笔刷半径一样是"编辑时视图状态"。存了反而更糟 —— 读回来一片
//   隐藏的草，用户会以为场景丢了。
// ============================================================

#include "EditorContext.h"
#include "LandscapeEditor.h"
#include "PickingSystem.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

struct ImDrawList;

namespace editor {

class FoliageEditor {
public:
    FoliageEditor(EditorContext& ctx, LandscapeEditor& land)
        : m_ctx(ctx), m_land(land) {
        resetTypes();
    }

    void drawPanel();
    // 视口里的一帧输入。返回 true = 吃掉左键（视口不该再点选）
    bool viewportInput(const glm::vec2& vpPos, const glm::vec2& vpSize,
                       bool hovered, ImDrawList* dl);

    // 每帧同步：找到/建立 "Foliage" 分组，重新统计株数
    void sync();

    int totalCount() const { return m_total; }

    // ---- 工具（UE5 Foliage 模式顶部那排）----
    // Paint  = 拖着刷（原行为）
    // Erase  = 拖着擦
    // Single = 点一下撒一株（撒树/撒石头时最常用：不想一次落一片）
    // Select = 点一株选中它（选中之后右侧 Inspector 能改它，也能 Delete）
    enum class Tool : int { Paint = 0, Erase, Single, Select };
    static constexpr int kToolCount = 4;
    static const char* toolId(Tool t);     // 稳定 id（日志 / 断言用）

private:
    // 五种内置植被。全部程序化生成，不依赖任何美术资产。
    //
    // 每种都带自己的"生态约束"（maxSlopeDeg / minH / maxH）而不是全局一个：
    // 草能长在较陡的坡上，树不行；石头哪都能待。写进表里之后，撒点的过滤
    // 逻辑只有一份，加新种类只要加一行。
    //
    // ⚠ 这张表是**默认值**，不是运行时状态。真正被面板改的是 m_types[]。
    struct Kind {
        const char* name;      // 显示名
        const char* id;        // 日志 / 材质键用的稳定 id
        const char* shape;     // 内置网格形状（"grass"/"sphere"/"tree"/"flower"/"rock"）
        float size;            // 网格自身高度（米），实例再乘随机缩放
        glm::vec4 color;       // 基色
        float roughness;
        int density;           // 一次落笔撒几株（默认值）
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

    // 每种类型的**运行时**设置（UE 的 Foliage Type 那一堆字段）。
    // 从 Kind 表初始化（resetTypes），之后只有面板会改它。
    //
    // 2026-10-28：加了**自定义网格**与**随机区间**两组（用户要求
    // 「允许自己从本地资源拖模型绑定，偏移/旋转/缩放区间也能自己设」）。
    struct TypeSettings {
        bool visible = true;     // 眼睛：关掉 = 不画（仍在场景里）
        int density = 10;        // 一次落笔几株
        float minScale = 1.0f;
        float maxScale = 1.0f;
        float tilt = 0.35f;      // 随机倾斜（0 = 严格贴法线）
        float edge = 0.15f;      // 边缘密度衰减（0 = 均匀）
        bool align = true;       // 贴坡
        float slopeDeg = 35.0f;  // 坡度上限（0 = 不限）
        float minH = 0.0f;       // 高度带（0..1）
        float maxH = 1.0f;
        int variants = 3;        // 颜色变体数
        float sizeScale = 1.0f;  // 整体体型倍数（UE 的 Scale）

        // ---- 自定义网格（空 = 用内置程序化网格）----
        // **相对 assets/ 的路径**（与内容浏览器的拖拽载荷同格式）。
        // 支持 .obj（单网格）/ .fbx / .gltf / .glb（取**第一个子网格**：
        // 一个 Foliage Type 在 UE 里也只是一个 Static Mesh）。
        std::string meshPath;

        // ---- 随机区间（落点时逐实例采样）----
        // 位置偏移：**在落点基础上**再加一个随机偏移（米）。Z 方向常用正
        // 值把植被抬高一点，避免根插进地里。XY 常用于"撒得更散"。
        glm::vec3 offsetMin{0.0f, 0.0f, 0.0f};
        glm::vec3 offsetMax{0.0f, 0.0f, 0.0f};
        // 旋转区间：**度**。x = Pitch，y = Roll，z = Yaw 的附加抖动。
        // ⚠ Yaw 有一个"基础随机 0..360°"始终生效（撒点本来就该随机朝向），
        //   这里的 z 是在它**之上**再加的抖动 —— 与 UE 的 Random Yaw Angle 同义。
        glm::vec3 rotMinDeg{0.0f, 0.0f, 0.0f};
        glm::vec3 rotMaxDeg{0.0f, 0.0f, 0.0f};
    };
    TypeSettings m_types[kKindCount];
    void resetTypes();

    // ---- 面板分段（对齐 UE5 的四块）----
    void drawToolRow();
    void drawTypeList();
    void drawBrushSection();
    void drawTypeSettings();
    void drawMeshSlot();     // 网格来源槽（拖入 / 清除）
    void drawStats();
    void drawNoTerrainCard();

    ecs::Entity ensureRoot();
    void recount(bool logCounts);

    // ---- 自动化钩子（只在启动后跑一次）----
    // 为什么需要：`偏移/旋转/缩放区间`与`自定义网格`都要靠**拖 UI** 才能改，
    // 而 GUI 用例里拖 DragFloat 的数值是不可靠的（1px 差 0.02）。所以给一组
    // 环境变量，让用例能把区间/网格**精确**设好再断言随机结果：
    //   MYVK_FOLIAGE_KIND=0..4        作用于哪个类型（默认 0 = 草）
    //   MYVK_FOLIAGE_SCALE=0.5,2.0    缩放区间
    //   MYVK_FOLIAGE_OFFSET=2,2,0.5   偏移 ±值（米，X/Y/Z）
    //   MYVK_FOLIAGE_ROT=0,0,360      旋转 ±值（度，X/Y/Z）
    //   MYVK_FOLIAGE_MESH=models/x.obj 绑定自定义网格
    // 不设就完全是手测路径（一条都不影响）。
    void applyEnvOverrides();
    bool m_envApplied = false;

    // ---- 单个实例的"落点规格"（2026-10-28）----
    // 为什么单独抽出来：撒点、单株放置、Reapply **三条路必须用同一套
    // 随机规则**，否则"改完参数点 Reapply"得到的东西和"重新撒一遍"不一样，
    // 用户会觉得 Reapply 是坏的。这就是项目里那条"Play 手写同一条抛物线
    // → 改一处改两处"的老坑，用结构体提前堵掉。
    struct Placement {
        glm::vec3 pos{0.0f};
        glm::vec3 rot{0.0f};     // **弧度**（TransformComponent 的单位）
        float scaleXZ = 1.0f;
        float scaleZ = 1.0f;
        int variant = 0;
    };
    // 在 (wx, wy) 上按该类型的规则采样一个实例规格。
    // 返回 false = 被坡度/高度规则排除（reject 说明是哪一条）。
    bool samplePlacement(int k, float wx, float wy, Placement& out,
                         const char** reject);

    // 一次落笔（撒 or 擦）
    void stamp(const glm::vec3& worldHit, bool erase);
    void spawn(int k, const Placement& pl);
    void placeOne(const glm::vec3& worldHit);
    void eraseAt(const glm::vec3& worldHit, bool anyKind);
    void clearAll(int kindIndex);
    // UE 的 Reapply：把当前类型的设置重新套到**已有**实例上
    int reapplyCurrentType();
    // 可见性变化后要让渲染层重建这一帧的网格列表
    void applyVisibility();

    // 网格 / 材质指针的可见性开关：MeshComponent 置空 = 不画。
    void setTypeVisible(int k, bool on);

    assets::Mesh* meshFor(int k);
    // 该类型**当前实际用的**网格：自定义路径优先，否则内置程序化网格。
    // ⚠ 这是"实例归属"的唯一判据 —— 统计 / 擦除 / 可见性 / Reapply 全都
    //   拿实例的 MeshComponent 与它比对（见 matchesKind）。
    assets::Mesh* typeMesh(int k);
    // 实例是否属于某一类型（指针精确匹配，失败再按 shape 兜底）
    bool matchesKind(int k, const assets::Mesh* mesh);
    // 自定义网格：按路径加载（失败返回 nullptr 并记一条 WARN，不影响内置）
    assets::Mesh* loadCustomMesh(const std::string& relPath);
    // 网格自身的外形（贴地用）：高度与底面到原点的距离
    void meshExtent(int k, float& height, float& bottom);
    assets::Material* materialFor(int k, int variant);
    float rnd();   // [0,1)，确定性 xorshift

    EditorContext& m_ctx;
    LandscapeEditor& m_land;
    PickingSystem m_picking;   // Select 工具用

    ecs::Entity m_root{};
    std::string m_rootName = "Foliage";

    // ---- 工具 / 笔刷 ----
    Tool m_tool = Tool::Paint;
    int m_kind = 0;              // 当前选中类型（0 草 / 1 灌木 / 2 树 / 3 花 / 4 石头）
    float m_radius = 10.0f;      // 米
    bool m_onlyCurrent = true;   // 擦除/过滤只作用于当前类型（UE 的 "Filter by type"）

    // ---- 拖拽状态 ----
    bool m_stroking = false;
    glm::vec3 m_lastStamp{0.0f};
    uint32_t m_rng = 0x9E3779B9u;

    // ---- 统计 ----
    int m_total = 0;
    int m_counts[kKindCount] = {0, 0, 0, 0, 0};
    int m_spawnSeq = 0;
    int m_lastLoggedTotal = -1;

    // 类型可见性在"上一次同步"时的快照。变了才重建（每帧重建会白扔 CPU）。
    bool m_visApplied[kKindCount] = {true, true, true, true, true};

    // 每类型**当前实际用的网格**（内置或自定义）+ 它对应的路径。
    // 路径一变就重新解析 —— 这是"拖了新模型进去立刻生效"的关键一行。
    assets::Mesh* m_typeMesh[kKindCount] = {nullptr, nullptr, nullptr,
                                            nullptr, nullptr};
    std::string m_typeMeshPath[kKindCount];
};

} // namespace editor
