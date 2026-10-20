#pragma once
// ============================================================
// editor/ToolkitEditors —— 编辑模式下拉里其余 6 个模式的实现
//
// 顶栏那颗下拉（UE5 的 Selection Mode）列了 9 个模式。Select / Landscape /
// Foliage 各有自己的文件；本文件放剩下的 6 个：
//
//   MeshPaint   网格体绘制   —— 往顶点色上刷（UE 的 Vertex Color 通道）
//   Modeling    建模         —— 对选中网格做几何操作（细分/平滑/挤出/镜像…）
//   Fracture    破裂         —— 把选中物体切成碎块并给刚体
//   BrushEdit   笔刷编辑     —— 用 box/cylinder/sphere 拼装关卡几何
//   Animation   动画         —— transform 关键帧时间轴
//   PCG         程序化生成   —— 在地形上按规则撒实体
//
// ------------------------------------------------------------
// 为什么放在一个文件里，而不是六个 Editor
// ------------------------------------------------------------
// 它们共享同一条"输入 → 改选中实体 → 重建网格/组件"的链路：都要选中项、
// 都要 `assets().registerMesh`、都要一个 400×600 的面板、都要 logRect 锚点。
// 分成六个文件会让那套样板抄六遍（而且第一版只有几百行/模式）。
// 每个类仍然是独立的：
//   · 状态互不干扰（各自的笔刷/参数/撤销缓冲）；
//   · 面板窗口 id 各自独立（`###panel_meshpaint` 等），布局持久化照常；
//   · 顶栏切模式时由 EditorApp 决定开关哪个面板。
//
// ------------------------------------------------------------
// 一条贯穿全部六个模式的约定：**改网格 = 换 Mesh 指针**
// ------------------------------------------------------------
// `Mesh` 是 GPU 资源的持有者（VBO/IBO），不能原地改顶点。所以所有几何操作
// 都走：读旧顶点 → 复制一份改 → `registerMesh("toolkit/<唯一键>", ...)`
// 造一个新 Mesh → 把 `MeshComponent::mesh` 指向它。
//   · 旧 Mesh 不销毁：撤销（整场景快照）要靠它把指针换回去；
//   · 唯一键必须带上版本号 —— `registerMesh` 对同名键是"直接复用"，
//     复用同一个键就等于"第二次操作什么也没做"。
// ============================================================

#include "EditorContext.h"
#include "LandscapeEditor.h"

// `assets::Mesh` / `assets::Vertex` 在前面的类里按值/指针出现（成员
// `std::vector<assets::Vertex>`、`runOp` 的函数指针签名），前向声明不够 ——
// 模板实参必须是完整类型。EditorContext.h 只前向声明了 `assets::Mesh`。
#include "assets/Mesh.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

struct ImDrawList;

namespace editor {

// 面板窗口的公共开场/收尾。返回 false 时调用方直接 return（窗口被折叠）。
// 内部已经处理了 `###panel_<id>` 的命名约定与 logRect 锚点。
bool beginToolkitWindow(const char* id, const char* title, float w, float h);

// 当前选中实体的网格 + 世界矩阵。没有选中 / 没有网格时返回 false。
bool selectedMesh(EditorContext& ctx, ecs::Entity& outEntity,
                  assets::Mesh*& outMesh, glm::mat4& outWorld);

// ------------------------------------------------------------
// 1. 网格体绘制（Mesh Paint）
// ------------------------------------------------------------
class MeshPaintEditor {
public:
    explicit MeshPaintEditor(EditorContext& ctx) : m_ctx(ctx) {}

    void drawPanel();
    bool viewportInput(const glm::vec2& vpPos, const glm::vec2& vpSize,
                       bool hovered, ImDrawList* dl);
    void sync();
    void flush() {}

private:
    // 把 CPU 侧的顶点色上传成新网格，并把组件指过去
    void applyToMesh();
    // 射线方向与选中实体三角形求交（Möller–Trumbore），返回最近交点
    bool raycastMesh(const glm::vec3& origin, const glm::vec3& dir,
                     glm::vec3& hit);

    EditorContext& m_ctx;
    ecs::Entity m_entity{};
    assets::Mesh* m_src = nullptr;      // 绑定的源网格（只读）
    std::vector<assets::Vertex> m_verts;  // CPU 副本（带当前颜色）
    std::vector<std::uint32_t> m_idx;
    bool m_bound = false;
    int m_rev = 0;                      // 上传版本号（拼进注册键）
    float m_radius = 1.0f;              // 世界米
    float m_strength = 0.35f;
    glm::vec3 m_color{0.95f, 0.35f, 0.25f};
    bool m_erase = false;               // 抹回白色（不染色）
    bool m_painting = false;
    int m_strokeTouched = 0;
    float m_applyTimer = 0.0f;
    int m_paintedVertices = 0;
    float m_lastLog = 0.0f;
};

// ------------------------------------------------------------
// 2. 建模（Modeling）—— 按 Blender 的 Edit Mode 做的
// ------------------------------------------------------------
//
// Blender 的编辑模式有三层选择（顶点 / 边 / 面，键 1 / 2 / 3），所有操作都
// 作用在"当前选区"上，而不是整个网格。第一版这里是四个全局按钮（整个网格
// 细分 / 整个网格平滑），那不是建模，那是"给网格换个形状"。
//
// ------------------------------------------------------------
// 一个关键决定：内部**只存一份顶点选择**，边和面都从它推出来
// ------------------------------------------------------------
// 三份互相独立的选区（顶点集 / 边集 / 面集）必须在每次操作后同步 —— 这是
// 一个必然会写漏的地方。这里改成：
//   · 权威 = `m_sel`（weld 组 id 的集合）
//   · 选中的面 = 三个顶点**全部**在 `m_sel` 里的三角形
//   · 选中的边 = 两端点**都在** `m_sel` 里的边
// 点面时把该面的 3 个顶点加进去，点边时加 2 个，点顶点时加 1 个 —— 于是
// "切到顶点模式看到的就是这些点"天然成立，不需要任何同步代码。
//
// 顺带解决了一个更容易踩的坑：立方体的 8 个角在 GPU 缓冲里各有 3 份顶点
// （不同面法线不同），所以必须先按**位置** weld 成 8 组再谈"选一个角"。
// 否则拖一个角只会把其中一面拽出来，模型当场裂开。
class ModelingEditor {
public:
    // Blender 的 1 / 2 / 3
    enum class SelectMode : int { Vertex = 0, Edge = 1, Face = 2 };

    explicit ModelingEditor(EditorContext& ctx) : m_ctx(ctx) {}

    void drawPanel();
    bool viewportInput(const glm::vec2& vpPos, const glm::vec2& vpSize,
                       bool hovered, ImDrawList* dl);
    // 键盘：1/2/3 选择层级 · A 全选 · Alt+A 清空 · G 拖动 · E 挤出 ·
    // S 缩放 · R 旋转 · X 删除面 · M 合并。
    // 返回 true = 这次按键归建模，调用方**不要**再往下发 —— 否则 W/E/R/G/X
    // 会同时把 gizmo 模式 / 吸附也切一遍（E 挤出时顺手把手柄切成旋转）。
    bool handleKeys();
    void sync();
    void flush() {}

private:
    enum class Modal : int { None = 0, Grab, Scale, Rotate };

    // 焊接后的拓扑。每次上传网格都要重建（索引变了）。
    struct Topo {
        std::vector<std::uint32_t> weldOf;  // 原始顶点下标 → weld 组 id
        std::vector<std::vector<std::uint32_t>> weldVerts;  // 组 → 原始顶点
        std::vector<glm::vec3> weldPos;                     // 组 → 位置
        std::vector<std::array<std::uint32_t, 3>> tri;      // 三角形 → 3 个组
        std::vector<std::array<std::uint32_t, 3>> triVert;  // 三角形 → 原始顶点
        std::vector<glm::vec3> triNormal;                   // 三角形法线
        bool valid = false;
    };

    void bind();          // 选中变了 → 拉一份 CPU 数据 + 建拓扑
    void rebuildTopo();
    void upload(const char* op);
    void selectAll(bool on);

    std::vector<std::uint32_t> selFaces() const;   // 三个顶点全选中的面
    glm::vec3 selectionCenter() const;
    glm::vec3 selectionNormal() const;

    void extrude();            // E
    void deleteFaces();        // X
    void mergeSelection();     // M
    // onlySelected=false 时先全选再走同一条路径（一份实现，两个入口）
    void subdivideSelected(const char* opName = "subdivide-selected");
    void flipSelected();       // 翻转选中的面
    void smoothAll(float amount);
    void mirrorX();

    void beginModal(Modal m);
    void updateModal(const glm::vec2& mouse);
    void commitModal();
    void cancelModal();
    void drawOverlay(const glm::vec2& vpPos, const glm::vec2& vpSize,
                     ImDrawList* dl);
    // 视口点选：返回命中的三角形下标（物体空间），-1 = 没命中
    int pickTriangle(const glm::vec3& ro, const glm::vec3& rd, float& outT) const;

    EditorContext& m_ctx;
    ecs::Entity m_entity{};
    assets::Mesh* m_src = nullptr;   // 绑定的源网格（只读）
    std::vector<assets::Vertex> m_verts;
    std::vector<std::uint32_t> m_idx;
    Topo m_topo;
    int m_rev = 0;
    int m_vertices = 0;
    int m_triangles = 0;

    SelectMode m_selMode = SelectMode::Face;
    std::unordered_set<std::uint32_t> m_sel;  // weld 组 id

    // 模态变换（G / S / R）：进入时备份 weldPos，鼠标移动时**从备份重算**
    // 而不是在上一帧结果上累加 —— 累加会随着鼠标抖动缓慢漂移，而且按
    // ESC 回不去（所有中间态都被吃进顶点里了）。
    Modal m_modal = Modal::None;
    std::vector<glm::vec3> m_modalBase;
    glm::vec3 m_modalPivot{0.0f};
    glm::vec2 m_modalStart{0.0f};
    bool m_modalMoved = false;

    int m_selectedVerts = 0;
    // 本次操作的起始顶点数：日志里打 `verts=a->b`（已有脚本按它断言"真的
    // 变多了"）。upload 是在操作**之后**调的，只能靠这个成员记住起点。
    int m_beforeVerts = 0;
    int m_selectedFaces = 0;
    int m_selectedEdges = 0;
    float m_extrude = 0.25f;      // 面板上那个"挤出位移"
    float m_smoothAmount = 0.5f;  // 拉普拉斯平滑的混合系数
    bool m_smoothShade = true;
};

// ------------------------------------------------------------
// 3. 破裂（Fracture）
// ------------------------------------------------------------
//
// 两种切法：
//   Voronoi  真断裂 —— 撒种子点，用中垂面把源网格裁成胞腔。切面是任意
//            朝向的，碎块沿真实表面崩开（不再是"一堆轴对齐盒子"）。
//   Grid     旧版 —— 轴对齐格子 + 抖动。留着是因为它便宜且形状规整，
//            做"砖墙被打碎"这类其实更合适。
class FractureEditor {
public:
    enum class Method : int { Voronoi = 0, Grid = 1 };

    explicit FractureEditor(EditorContext& ctx) : m_ctx(ctx) {}

    void drawPanel();
    void sync() {}

private:
    void breakSelected();
    void breakVoronoi(ecs::Entity e, assets::Mesh* mesh,
                      const glm::mat4& world);
    void breakGrid(ecs::Entity e, assets::Mesh* mesh, const glm::mat4& world);

    EditorContext& m_ctx;
    int m_method = 0;           // 0 = Voronoi / 1 = Grid
    int m_chunks = 8;
    float m_gap = 0.06f;        // 碎块之间的缝（占块尺寸或胞腔的比例）
    int m_seed = 1337;
    bool m_addPhysics = true;
    bool m_hideSource = true;
    // Voronoi 的"不规则度"：种子点偏离规则格心的幅度。0 = 等大的胞腔，
    // 1 = 完全随机（会出现很小的碎片和很大的块，更像真实断裂）。
    float m_irregularity = 0.65f;
    int m_lastChunks = 0;
    int m_lastTriangles = 0;
};

// ------------------------------------------------------------
// 4. 笔刷编辑（Brush Editing）
// ------------------------------------------------------------
class BrushEditor {
public:
    explicit BrushEditor(EditorContext& ctx) : m_ctx(ctx) {}

    void drawPanel();
    bool viewportInput(const glm::vec2& vpPos, const glm::vec2& vpSize,
                       bool hovered, ImDrawList* dl);
    void sync() {}

private:
    void placeAt(const glm::vec3& world);

    EditorContext& m_ctx;
    int m_shape = 0;            // 0 = Box / 1 = Cylinder / 2 = Sphere
    glm::vec3 m_size{4.0f, 4.0f, 4.0f};
    float m_snap = 0.0f;        // 0 = 不吸附
    bool m_addCollision = true;
    bool m_dropToGround = true;
    int m_placed = 0;
};

// ------------------------------------------------------------
// 5. 动画（Animation）
// ------------------------------------------------------------
//
// ⚠ 轨道数据**不在这个类里**，在实体的 `ecs::AnimationClipComponent` 上。
//
// 第一版把 keys 放在 `m_keys`（编辑器成员）里，于是：切换选中丢、关面板丢、
// 存盘丢。现在数据挂在实体上，序列化顺手就带上了（`je["animClip"]`），
// 连带 undo 也成立 —— `EditorContext::structuralEdit` 的整场景快照本来
// 就包含它。
//
// 这个类只负责：面板 UI + 播放头推进 + 把当前时刻的插值写回 transform。
class AnimationEditor {
public:
    explicit AnimationEditor(EditorContext& ctx) : m_ctx(ctx) {}

    void drawPanel();
    // 每帧推进时间轴并写回 transform（只在 Animation 模式下被调用）
    void sync();
    void flush() {}

private:
    // 选中实体的轨道；不存在返回 nullptr（**按需创建**，不主动加组件 ——
    // 光点一下物体就让它多一个组件是很脏的行为）
    ecs::AnimationClipComponent* clip();
    ecs::AnimationClipComponent& ensureClip();

    void addKey();
    void deleteNearestKey();   // 删当前时刻最近的一帧
    void clearKeys();
    void jumpToKey(int index);
    void applyAt(float t);     // 把 t 时刻的插值写进 TransformComponent

    EditorContext& m_ctx;
    ecs::Entity m_entity{};
    bool m_scrubbing = false;   // 正在拖时间轴（此时不要被播放覆盖）
    int m_selectedKey = -1;
};

// ------------------------------------------------------------
// 6. 程序化生成（PCG）
// ------------------------------------------------------------
class PCGEditor {
public:
    // 需要 LandscapeEditor 才能问"这个 XY 处多高、坡度多少" —— 地形资产
    // 不挂在 EditorContext 上（它属于地形编辑器那套），而 PCG 的高度带 /
    // 坡度过滤**必须**和地形笔刷用同一份高度图，否则撒点会和地表错位。
    PCGEditor(EditorContext& ctx, LandscapeEditor& land)
        : m_ctx(ctx), m_land(land) {}

    void drawPanel();
    void sync() {}

private:
    void generate();
    void clearGenerated();

    EditorContext& m_ctx;
    LandscapeEditor& m_land;
    int m_kind = 2;             // 0 = Cube / 1 = Sphere / 2 = Rock / 3 = Grass
    int m_count = 120;
    float m_area = 200.0f;      // 采样方形边长（米）
    float m_centerX = 0.0f;
    float m_centerY = 0.0f;
    float m_minScale = 0.6f;
    float m_maxScale = 1.6f;
    float m_maxSlopeDeg = 32.0f;
    float m_minHeight = 0.0f;   // 归一化的地形高度带
    float m_maxHeight = 0.85f;
    int m_seed = 20261020;
    bool m_alignNormal = true;
    ecs::Entity m_root{};
    int m_generated = 0;
};

} // namespace editor
