#pragma once
// ============================================================
// editor/LandscapeEditor —— 地形编辑模式（顶栏 "Landscape"）
//
// ------------------------------------------------------------
// 它编辑的是**磁盘上的地形资产**，不是场景里的组件
// ------------------------------------------------------------
// `ecs::TerrainComponent` 只有三样东西：`.vterrain` 路径 / 可见 / 高度倍率。
// 真正的高度图与层权重住在 `assets/terrain/<name>/height.r32` 与
// `splat.rgba8` 里（W4 的决定，见 engine/terrain/Terrain.h 的文件头）。
//
// 所以笔刷的流程是：
//     射线打中高度场 → 改**内存里**的 TerrainAsset
//            → 每 ~80 ms 落盘一次（height.r32 / splat.rgba8）
//            → 把 TerrainComponent::heightsDirty / splatDirty 置位
//            → Renderer::syncTerrains 下一帧**原地重传那张贴图**
//
// ------------------------------------------------------------
// 为什么必须落盘才能看见变化（这条最容易踩）
// ------------------------------------------------------------
// `TerrainPass::createTerrain` 是**按路径重新 loadTerrain** 的，它不认任何
// 内存里的副本。所以 `saveAssets()` 是笔刷链路的一环，不是"用户按了保存
// 才做的事"。
//
// ------------------------------------------------------------
// 一笔 = 一个撤销单元（笔划快照）
// ------------------------------------------------------------
// 高度图不在场景里，`EditorContext::undo()` 那套整场景快照撤不掉它。所以
// 这里自带一条笔划撤销栈：
//   按下   → 把整份 heights（和 splat）拷进 m_snapshot*
//   松开   → 对比快照算出**实际变化的包围盒**，只把那一块压栈
//   Ctrl+Z → 把那块写回去
// 全图拷贝 264 KB 只发生在笔划期间（同一时刻最多一份），撤销栈里存的是
// 脏矩形 —— 一笔扫过半个地形通常也就几十 KB。
// ============================================================

#include "EditorContext.h"

#include "terrain/Terrain.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

struct ImDrawList;

namespace editor {

class LandscapeEditor {
public:
    explicit LandscapeEditor(EditorContext& ctx) : m_ctx(ctx) {}

    // 面板 UI（EditorApp 的 panelDrawLandscape 调）
    void drawPanel();

    // 视口里的一帧笔刷输入。返回 true = 本帧"吃掉"了左键（视口不该再点选）。
    // 由 ViewportPanel 在 editMode == Landscape 时调用。
    bool viewportInput(const glm::vec2& vpPos, const glm::vec2& vpSize,
                       bool hovered, ImDrawList* dl);

    // 笔刷快捷键（Ctrl+Z / Ctrl+Shift+Z / [ ] , .）。
    // **必须**在 EditorApp 的 Ctrl 分支之前调用：那里会把所有 Ctrl 组合键
    // 一把 return 掉，之后 Ctrl+Z 就变成"撤销整场景"了 —— 在刷地形时那是
    // 灾难（一笔撤不掉，反而把场景撤回去了）。
    // 返回 true = 本帧这些键归笔刷，调用方不要再处理。
    bool handleShortcuts();

    // 当前场景里有没有地形（没有时面板给"创建"向导，视口不接受笔刷）
    bool hasTerrain() const { return m_valid; }
    ecs::Entity entity() const { return m_entity; }
    const terrain::TerrainAsset* asset() const {
        return m_valid ? &m_asset : nullptr;
    }
    // 地形实体的高度倍率（TerrainComponent::heightScale）—— 采样时要乘上
    float heightScale() const { return m_heightScale; }

    // 世界射线 vs 地形。命中时 worldHit / worldNormal 是世界坐标，
    // localHit 是地形**局部**坐标（笔刷按局部坐标算格子）。
    // 植被编辑器也用它（撒点必须先知道"点在哪儿"）。
    bool raycast(const glm::vec3& rayOrigin, const glm::vec3& rayDir,
                 glm::vec3& worldHit, glm::vec3& worldNormal,
                 glm::vec3* localHit = nullptr) const;

    // 世界 XY → 地形高度（米）。没有地形时返回 0。
    float heightAtWorld(float x, float y) const;

    // 世界 XY 处的地表法线（单位向量）。植被"贴合坡度"与"陡坡过滤"用。
    glm::vec3 normalAtWorld(float x, float y) const;

    // 地表世界高度的 [lo, hi]（米）。植被按"高度带"过滤（雪线以上不长树）用。
    void heightRangeWorld(float& lo, float& hi) const;

    // 面板上的"把相机对到地形"（进入模式时也调一次）
    void focusCamera();      // 整片地形概览：视距按视口长宽比自适应
    void focusCameraClose(); // 更近的一档：植被模式进入时用。草只有 0.55 m
                             // 高，160 m 外是一堆像素点 —— 撒完看不见东西，
                             // 用户会以为"没撒上"。

    // 每帧 unconditional 调用：场景里的地形实体变了就重新读资产。
    void sync();

    // 编辑模式离开时把未落盘的改动写完（切回选择模式 / 退出前）
    void flush();

    // 按面板上那组"新建参数"建一片地形。面板按钮与自动化钩子共用 ——
    // 两处各调一次 createTerrain 迟早会漂移（一个改了默认参数另一个没改）。
    bool createWithCurrentSettings();

    // 撤销 / 重做（笔划粒度）。返回是否真的动了东西。
    bool undoStroke();
    bool redoStroke();
    int undoCount() const { return static_cast<int>(m_undo.size()); }

private:
    // 工具：前 5 个是"改高度"的算子，PaintLayer 走权重图，Erode/Noise
    // 是这一轮补的"让地形有地貌感"的两把刷子（见 .cpp 里的说明），
    // Hole 走**挖洞遮罩**（不碰高度，见 applyHoleStroke）。
    enum class Tool : int {
        Raise = 0,   // Shift = 降低
        Smooth,
        Flatten,     // Shift = 摊平时取"当前笔心高度"以外，见 m_flattenHeight
        Noise,
        Erode,
        PaintLayer,
        Hole,        // 挖穿 / Shift = 补回（对应 UE 的 Visibility 层）
        Ramp,        // 坡道：从按下点的高度线性过渡到抬起点的高度
    };
    static constexpr int kToolCount = 8;

    // 一笔的撤销记录（脏矩形 + 改动前的像素）
    struct StrokeRecord {
        int c0 = 0, r0 = 0, c1 = -1, r1 = -1;   // 闭区间；c1 < c0 = 空
        std::vector<float> height;              // 改动前的高度（空 = 没改高度）
        std::vector<std::uint8_t> splat;        // 改动前的权重
        std::vector<std::uint8_t> holes;        // 改动前的挖洞遮罩
        bool empty() const { return c1 < c0 || r1 < r0; }
    };

    // 统一的"把相机摆到地形前"：back = 水平后退距离（米），
    // pitch = 俯角（弧度），yaw 默认 π/2 = "站在 -Y 往 +Y 看"。
    // 三者共同决定相机位置（见 .cpp 的推导）。
    void placeCamera(float back, float pitch, float yaw = 1.5707963f);

    // 固定系数版：backFactor = 视距 / 地形边长。只给"刻意只取局部"的
    // 近景（focusCameraClose）用 —— 全景别用固定系数，见 focusCamera 注释。
    void frameTerrain(float backFactor, float pitch);

    void drawCreateSection();
    void drawBrushSection();
    void drawStatsSection();
    void drawToolParams();
    void drawWholeSection();

    bool createTerrain(const std::string& name, int cols, int rows,
                       float metersPerPixel, float maxHeight);

    // 一笔中的一个落点。dt 是这一步分到的时间片（插值会把一帧拆成多步）。
    void applyStroke(const glm::vec3& localHit, float dt, bool invert);
    void applyLayerStroke(const glm::vec3& localHit, float dt, bool invert);
    // 噪声：确定性 value noise，多倍频
    void applyNoiseStroke(const glm::vec3& localHit, float dt);
    // 热侵蚀：超过休止角的物质往低处搬
    void applyErodeStroke(const glm::vec3& localHit, float dt);
    // 挖洞：改的是**遮罩**不是高度（片元着色器按遮罩 discard）
    void applyHoleStroke(const glm::vec3& localHit, float dt, bool invert);
    // 坡道：按下点记高度，抬起点记高度，笔刷范围内线性插值
    void applyRampStroke(const glm::vec3& localHit, float dt);
    // 整块地形操作（全图平滑 / 全图噪声 / 重置），参数来自面板
    void wholeSmooth();
    void wholeNoise();
    void wholeReset();

    // 把内存里的高度/权重写回资产目录，并把 TerrainComponent 标脏
    void saveAssets(bool splatToo, bool holesToo = false);

    // ---- 笔划快照 ----
    void beginSnapshot(int parts);   // 位掩码：1=高度 2=权重 4=挖洞
    void commitSnapshot();          // 算出脏矩形压进 m_undo
    void dropSnapshot();
    bool applyRecord(const StrokeRecord& rec, bool useBefore);
    // 按 shape 的包围盒把**当前**资产内容抓成一份记录（undo 时生成 redo 用）。
    // 做成成员函数而不是匿名命名空间的自由函数：StrokeRecord 是私有嵌套
    // 类型，类外的自由函数既看不见它、也拿不到 m_asset。
    void captureCurrent(const StrokeRecord& shape, StrokeRecord& out) const;

    // 地形实体自己的世界矩阵（local → world）
    glm::mat4 modelMatrix() const;

    EditorContext& m_ctx;

    // ---- 绑定的地形 ----
    ecs::Entity m_entity{};
    std::string m_source;
    terrain::TerrainAsset m_asset;
    bool m_valid = false;
    float m_heightScale = 1.0f;

    // ---- 笔刷参数 ----
    Tool m_tool = Tool::Raise;
    float m_radius = 12.0f;          // 世界米
    // 强度：**没有硬性上限**（上限见 kMaxStrength）。UE 的 Tool Strength 也是
    // 这样 —— 滑条到 1.0 只代表"标准档"，大范围整地时 3~8 才好用。
    // 0.01 的下限保留：0 会让笔刷完全无反应，看起来像坏了。
    float m_strength = 0.35f;        // 0.01 .. kMaxStrength
    static constexpr float kMaxStrength = 16.0f;
    float m_falloff = 0.6f;          // 0 = 硬边，1 = 很软
    float m_maxDelta = 0.0f;         // 一笔之内最大位移（米），0 = 不限
    int m_layer = 0;                 // 0..3
    bool m_eraseLayer = false;
    float m_flattenHeight = 0.0f;    // 按下那一刻的世界高度（米）
    // 坡道：按下点 / 抬起点（局部坐标 + 各自的高度，归一化）
    glm::vec2 m_rampStart{0.0f};
    float m_rampStartH = 0.0f;
    glm::vec2 m_rampEnd{0.0f};
    float m_rampEndH = 0.0f;

    // 噪声参数
    float m_noiseScale = 18.0f;      // 特征尺度（米）
    int m_noiseOctaves = 3;
    std::uint32_t m_noiseSeed = 1337u;

    // 侵蚀参数
    float m_erodeTalus = 32.0f;      // 休止角（度）
    int m_erodeIterations = 2;       // 每帧迭代次数

    // 整块操作的参数
    float m_wholeNoiseAmp = 0.06f;   // 归一化高度振幅

    // ---- 拖拽状态 / 落盘节流 ----
    bool m_stroking = false;
    bool m_pendingSave = false;
    float m_saveTimer = 0.0f;
    int m_strokeCount = 0;
    glm::vec3 m_lastLocal{0.0f};     // 上一个落点（局部坐标）—— 插值用

    // ---- 撤销栈 ----
    std::deque<StrokeRecord> m_undo;
    std::deque<StrokeRecord> m_redo;
    std::vector<float> m_snapshotHeights;
    std::vector<std::uint8_t> m_snapshotSplat;
    std::vector<std::uint8_t> m_snapshotHoles;
    int m_snapshotParts = 0;   // 位掩码：1=高度 2=权重 4=挖洞
    bool m_snapshotValid = false;

    // ---- 新建地形的默认参数（面板可改）----
    char m_newName[64] = "landscape";
    int m_newCols = 257;
    int m_newRows = 257;
    float m_newMpp = 2.0f;
    float m_newMaxHeight = 60.0f;

    // 每帧"有没有地形"只在变化时打一行，避免刷屏
    bool m_loggedMissing = false;
};

} // namespace editor
