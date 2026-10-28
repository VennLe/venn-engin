#include "LandscapeEditor.h"

#include "DebugRects.h"
#include "Localization.h"
#include "ModeRail.h"
#include "PanelLayout.h"
#include "PickingSystem.h"

#include "assets/AssetPath.h"
#include "assets/AssetManager.h"
#include "core/Logger.h"
#include "physics/Heightfield.h"
#include "scene/Scene.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace editor {

namespace {

using namespace terrain;

// 刷高度太快/太慢都会让人以为"笔刷坏了"。这个系数把 strength=1 时的
// 抬升速度定在 ~7 m/s（maxHeight=60 的地形）：
//   delta_normalized = strength * kRaiseRate * dt
// 0.35 * 0.35 * 1s = 0.1225 → 7.3 m。低一点的 strength 是"轻推"，
// 满 strength 是"一秒钟堆出一座小丘"，都在直觉范围内。
constexpr float kRaiseRate = 0.35f;

// 噪声的峰值速率（归一化高度/秒）。比起抬高，噪声要更"细"：它是往表面
// 撒一层细节，不是在堆土。0.5 的振幅 × 0.8 ≈ 每秒钟最多改 0.4 个全高，
// 实际因为在 ±0.5 之间摇摆，肉眼看到的是"表面逐渐长出颗粒"。
constexpr float kNoiseRate = 0.80f;

// 上传节流：拖动时每 80 ms 把内存推给渲染层一次。
// 为什么要节流而不是每帧推：每次是一次 264 KB 的 R32 图重传 + 一次
// vkQueueWaitIdle，60 Hz 下推 257² 是纯浪费；80 ms 的延迟肉眼看不出来。
//
// 2026-10-21：这个节流**只针对上传**了。以前它同时决定"多久写一次盘"，
// 而磁盘往返（写 264 KB → 渲染层读回 264 KB）是笔刷延迟的主因，现已改
// 成拖拽期间完全不落盘，抬笔才 saveAssets。
constexpr float kSaveInterval = 0.08f;

// 撤销栈上限。一笔扫过整片地形的脏矩形按 1 MB 估，24 笔 ~24 MB 封顶；
// 实际多数笔划只有几十 KB。
constexpr std::size_t kUndoDepth = 24;

// 归一化高度的上限。**不是 1.0**。
//
// 1.0 只代表"等于 maxHeight"，不是天花板。以前抬升笔刷写的是
// `h = clamp(h, 0, 1)`，于是笔心先撞到 1.0、外圈继续涨 —— 最终一定是一块
// 平顶（用户看到的就是"想刷山峰，结果刷出火山/台地"）。
//
// 留 8 倍余量之后：山峰可以一直长高、剖面保持锥形；同时 kMaxNorm 还是
// 一个硬兜底，防止误操作把高度刷到天上（maxHeight=60 → 480 m 封顶）。
constexpr float kMaxNorm = 8.0f;

// 抬升方向的"剩余空间"系数：1.0 以下返回 1（手感与以前完全一致），
// 超过 1.0 之后按剩余比例线性衰减到 0 —— 渐近线式逼近上限，
// **永远不会出现"整片都被削成同一高度"的平台**。
float raiseHeadroom(float h) {
    if (h < 1.0f) return 1.0f;
    return std::max(0.0f, (kMaxNorm - h) / (kMaxNorm - 1.0f));
}

// 新建地形的合法边长（(cols-1) 必须能被 kChunkCells=32 整除 —— 这是
// 分块网格的硬约束，见 terrain::parseDesc）。
constexpr int kSizeChoices[3] = {129, 257, 513};
const char* kSizeLabels[3] = {"129 x 129  (128 m)", "257 x 257  (512 m)",
                              "513 x 513  (1 km)"};

// 新建地形的实体高度。
//
// 2026-10-24：0.02 → 0。用户明确要求"初始化地形的地面要尽量保持和栅格地面
// 是同一个平面，干脆就直接把原栅格地面当作地形地面去操作"。原先那 2 cm
// 是为了躲模板场景里那块也叫 Ground 的面片（共面 z-fighting）；现在策略变了
// —— **有地形时栅格本身不再画**（见 Renderer 里 grid 那一段），共面对象只剩
// 地形自己，2 cm 就没有理由继续留着。留着反而带来两个副作用：
//   · 角色/箱子按 z=0 算高度时会浮空 2 cm（贴地验收的 ±0.08 余量被吃掉 1/4）；
//   · 视口里的"栅格平面"与"地形最低点"不是同一个平面，对齐时对不准。
constexpr float kGroundEpsilon = 0.0f;

// 新建地形默认的 4 个层材质。
//
// 层 0 = 材质仓库里的 `grass_ground`（用户要求："以后地形地面就默认用
// grass_ground 来就行了"）。它带 albedo + normal + arm 三张贴图，所以新建
// 出来的地形一落地就是"有草的土面"，而不是原来那种纯色块。
// 层 1 给 `concrete_moss` —— 仓库里另一个材质，正好能当"铺装 / 石阶"用。
// 层 2/3 保留引擎自带的岩石 / 雪，把"陡坡出石、高处积雪"的语义接上
// （`generateDefaultSplat` 的门限就是按这四个层写的）。
const char* kLayerIds[4] = {"materials/library/grass_ground",
                            "materials/library/concrete_moss",
                            "materials/terrain_rock", "materials/terrain_snow"};

// 层按钮的兜底显示名。只在材质 id 解析不出名字时用 —— 正常情况下按钮上
// 显示的是**材质自己的名字**（见 shortMaterialName），因为 layerOverrides
// 允许用户把任意一层换成任意材质，写死的 "Grass/Dirt/Rock/Snow" 一旦换了
// 材质就会说错话。
const char* kLayerNames[4] = {"Grass", "Moss", "Rock", "Snow"};

// "materials/library/grass_ground" → "grass_ground"
// 层按钮只有 96 px 宽，整条 id 塞不下；取末段既短又能认出是谁。
std::string shortMaterialName(const std::string& id) {
    if (id.empty()) return std::string();
    const std::size_t slash = id.find_last_of('/');
    std::string base = (slash == std::string::npos) ? id : id.substr(slash + 1);
    const std::string ext = ".vmat";
    if (base.size() > ext.size() &&
        base.compare(base.size() - ext.size(), ext.size(), ext) == 0) {
        base.resize(base.size() - ext.size());
    }
    return base;
}

// 笔刷权重：t = 0（笔心）→ 1；t = 1（边缘）→ 0。
// falloff 越大越"软"：0 → 指数 3（中心强、边缘几乎没有），1 → 0.6（一大片
// 都很均匀，只有最外圈衰减）。
float brushWeight(float t, float falloff) {
    if (t >= 1.0f) return 0.0f;
    const float e = 3.0f + (0.6f - 3.0f) * std::clamp(falloff, 0.0f, 1.0f);
    return std::pow(1.0f - t, e);
}

// 一行"标题 + 满宽滑条"。
//
// 为什么不用 ImGui 自带的 `SliderFloat("Radius", ...)`：标签是画在控件
// **右边**的，而控件宽度取 -1（占满）时标签直接被窗口右边缘裁掉 ——
// 表现就是"只有滑条没有字"。所以标题单独一行、控件自带 `##` 隐藏 id。
void labeledSlider(const char* label, const char* id, float* v, float lo,
                   float hi, const char* fmt, bool logScale = false) {
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderFloat(id, v, lo, hi, fmt,
                       logScale ? ImGuiSliderFlags_Logarithmic : 0);
}

void labeledSliderInt(const char* label, const char* id, int* v, int lo,
                      int hi) {
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderInt(id, v, lo, hi);
}

std::size_t sampleIndex(int rows, int col, int row) {
    return static_cast<std::size_t>(col) * static_cast<std::size_t>(rows) +
           static_cast<std::size_t>(row);
}

// ------------------------------------------------------------
// 确定性 value noise
// ------------------------------------------------------------
// 为什么不用 Perlin/simplex：value noise 三个倍频就够了，而且**只依赖
// 坐标和种子**——同一笔刷落在同一个地方永远得到同一层细节，截图可复现。
// 用 std::rand 的话"这刷好看、下一刷糊成一团"根本没法调试。
std::uint32_t hash2i(int x, int y, std::uint32_t seed) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u +
                      static_cast<std::uint32_t>(y) * 668265263u +
                      seed * 1274126177u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return h;
}

float hash01(int x, int y, std::uint32_t seed) {
    return static_cast<float>(hash2i(x, y, seed) & 0x00FFFFFFu) / 16777216.0f;
}

float valueNoise(float x, float y, std::uint32_t seed) {
    const int xi = static_cast<int>(std::floor(x));
    const int yi = static_cast<int>(std::floor(y));
    const float fx = x - static_cast<float>(xi);
    const float fy = y - static_cast<float>(yi);
    // smoothstep 插值：线性插值会让格子边界出现明显的十字纹
    const float ux = fx * fx * (3.0f - 2.0f * fx);
    const float uy = fy * fy * (3.0f - 2.0f * fy);
    const float a = hash01(xi, yi, seed);
    const float b = hash01(xi + 1, yi, seed);
    const float c = hash01(xi, yi + 1, seed);
    const float d = hash01(xi + 1, yi + 1, seed);
    return (a * (1.0f - ux) + b * ux) * (1.0f - uy) +
           (c * (1.0f - ux) + d * ux) * uy;
}

// 多倍频叠加。返回 [0,1]，0.5 是"没有变化"。
float fbm(float x, float y, int octaves, std::uint32_t seed) {
    float sum = 0.0f;
    float amp = 1.0f;
    float norm = 0.0f;
    float f = 1.0f;
    for (int i = 0; i < std::clamp(octaves, 1, 6); ++i) {
        sum += valueNoise(x * f, y * f, seed + static_cast<std::uint32_t>(i) * 7919u) * amp;
        norm += amp;
        amp *= 0.5f;
        f *= 2.07f;   // 非整数倍频，避免各层峰谷对齐成网格
    }
    return (norm > 0.0f) ? (sum / norm) : 0.5f;
}

// 新建地形的默认高度：三层正弦叠出来的缓坡丘陵。
//
// ------------------------------------------------------------
// 为什么四边必须**压到 0**
// ------------------------------------------------------------
// 视口的地平面栅格画在 z = 0（Renderer 的 GridSettings::planeOffset），
// 而地形表面是 `height * maxHeight`。地形块的四条边只要离开 0 面，就会
// 变成"浮在栅格上方的一块厚板"，而不是长在地面上的一片丘陵 —— 这就是
// "地形和栅格不在一个平面"。
//
// 所以这里乘一个**边界渐变**：距边界 12%（默认 512 m 地形约 61 m）以内
// 用 smoothstep 平滑降到 0。效果是地形四边与栅格共面，接缝处没有台阶；
// 中间该有的起伏一点没少。
//
// ⚠ 不能改成"整体减去最小值"：那样只有**最低点**贴地，其余边界仍然是
//   几米不等的落差（正弦的取值沿边界一直在变），四周会出现一圈高度各不
//   相同的墙 —— 看起来更像 bug。
//
// 为什么不是随机噪声：噪声第一眼像"坏的"，而且每次新建都长得不一样，
// 没法用截图做回归对比。三层不同频率的正弦叠加是**确定性**的。
//
// 2026-10-21 改：默认地貌从"悬在 27 m 高空的台子"改成"贴着地面的缓坡"。
//
//   原来的基面是 0.46 —— `maxHeight=60` 时整片地形挂在 **+27.6 m**，而且
//   512 m 见方。新建完地形，初始场景（Ground / Player / Pickup，都坐在
//   z≈0..1）直接被这一整块地盖在下面；Play 时相机在 z≈2.7，比它**低**
//   27 m，画面里只有地形的背面。用户的描述是"地形悬在栅格地面正上方，
//   还盖住了原本的场景"。
//
//   现在两步：
//     ① 正弦形状**归一化到 [0, 1]** —— 谷底严格等于 0，贴着地面；
//     ② 再乘 kDefaultPeak（0.12 → 60 m 地形约 7 m 起伏）。
//   于是"谷底落在地面上、山丘从地面长出来"。想做大起伏就自己刷 ——
//   画布越平，越不可能误盖住已有场景（UE 新建 Landscape 默认全平，
//   也是同一个理由）。
std::vector<float> generateDefaultHeights(int cols, int rows) {
    // 边缘渐变宽度（占边长的比例）。0.12 → 512 m 地形约 61 m 的过渡带。
    // 太小（<0.05）在低多边形 LOD 下会看出折线；太大（>0.25）中心就没
    // 起伏了 —— 默认地貌会变成"一个大馒头"。
    constexpr float kEdgeFade = 0.12f;
    // 默认起伏的峰值（归一化）。0.12 × 60 m ≈ 7.2 m —— 一眼看得出是"地形"，
    // 又不会把站在 z≈1 的角色/场景挡住。
    constexpr float kDefaultPeak = 0.12f;

    const std::size_t n = static_cast<std::size_t>(cols) * rows;
    std::vector<float> raw(n, 0.0f);
    float lo = 1.0e30f, hi = -1.0e30f;
    for (int c = 0; c < cols; ++c) {
        for (int r = 0; r < rows; ++r) {
            const float u = static_cast<float>(c) / static_cast<float>(cols - 1);
            const float v = static_cast<float>(r) / static_cast<float>(rows - 1);
            float h = 0.46f + 0.24f * std::sin(u * 6.4f) * std::cos(v * 5.4f) +
                      0.13f * std::sin((u * 2.1f + v * 3.3f) * 3.1f) +
                      0.04f * std::sin(u * 23.0f) * std::sin(v * 19.0f);
            // 到最近边界的归一化距离（0 = 在边界上，0.5 = 正中）
            const float edge = std::min(std::min(u, 1.0f - u),
                                        std::min(v, 1.0f - v));
            float fade = std::clamp(edge / kEdgeFade, 0.0f, 1.0f);
            fade = fade * fade * (3.0f - 2.0f * fade);   // smoothstep
            const float val = h * fade;
            raw[sampleIndex(rows, c, r)] = val;
            lo = std::min(lo, val);
            hi = std::max(hi, val);
        }
    }

    // 归一化到 [0, kDefaultPeak]。lo 一定是 0（边界被 fade 压平），
    // 但这里按实际 min/max 归一化，改公式之后也不会漂。
    const float span = std::max(hi - lo, 1.0e-6f);
    std::vector<float> heights(n, 0.0f);
    for (std::size_t i = 0; i < n; ++i) {
        heights[i] = (raw[i] - lo) / span * kDefaultPeak;
    }
    return heights;
}

// 按高度 + 坡度铺 4 层权重。
//
// 全刷成第 0 层（草地）时整块地形是一片均匀的绿：4 层材质里另外 3 层的
// 颜色一次都看不到，"层绘制"工具也就没有可对比的起点。
//   陡坡 → 岩石（2）   低处 → 草（0）   中间 → 泥土（1）   高处 → 雪（3）
//
// 坡度用**中心差分**算（与地形物理/法线同一套口径），不要用高度值硬切：
// 高但平坦的台地应该是草/雪，而不是因为"高"就变成岩石。
//
// ⚠ 层带门限是**按这片地形的实测分布**定的，不是教科书数值 —— 这点必须
//   写清楚，否则后人一看 "岩石从 28° 起" 就会当 bug "修正"回 30°。
//
//   地形生成器改过两次，每次层带都要跟着重测（这是最容易漏的一步）：
//     · 最初（无边界渐变）：高度 0.082..0.830（均值 0.461），
//       坡度 tanθ p50 0.150 / max **0.391（22.4°）** —— 门限 0.35 起，岩石
//       覆盖率 **0.0%**，材质表第 3 层一次都没显示过。
//     · 现在（四边压到 0 以便与栅格共面）：高度 0.000..0.729（均值 0.354），
//       坡度 p50 0.187 / p90 0.655 / p99 1.014 / **max 1.141（65.3°）**
//       —— 边界那一圈渐变带把最大坡度从 22° 抬到 65°，沿用旧门限会让
//       岩石飙到 35.7%（整块地形被一圈灰石头包住），雪只剩 1.5%。
//
//   当前这组门限的实测权重占比（257² 默认地貌）：
//     草 60.9% / 泥土 17.3% / 岩石 13.1% / 雪 8.7%
//   四层都露得出来，且草仍是压倒性的主色 —— "低处草、中段土、陡坡岩、
//   高处雪"才读得出来。
//
// 权重的**叠放次序**也有讲究：岩石先占位，雪和泥土再按 (1-岩石) 依次
// 让位，草取余量。早先的写法是 `wGrass = 1 - wDirt` 然后整体归一化 ——
// 那样岩石会被草地"稀释"一半（岩石 0.5 + 草 1 归一化成 0.33/0.67），
// 陡坡永远刷不出结实的岩石色。
std::vector<std::uint8_t> generateDefaultSplat(const std::vector<float>& heights,
                                              int cols, int rows,
                                              float metersPerPixel,
                                              float maxHeight) {
    std::vector<std::uint8_t> splat(
        static_cast<std::size_t>(cols) * rows * 4u, 0u);
    for (int c = 0; c < cols; ++c) {
        for (int r = 0; r < rows; ++r) {
            const int cl = std::max(c - 1, 0);
            const int cr = std::min(c + 1, cols - 1);
            const int rd = std::max(r - 1, 0);
            const int ru = std::min(r + 1, rows - 1);
            const float hL = heights[sampleIndex(rows, cl, r)];
            const float hR = heights[sampleIndex(rows, cr, r)];
            const float hD = heights[sampleIndex(rows, c, rd)];
            const float hU = heights[sampleIndex(rows, c, ru)];
            const float dzdx = (hR - hL) * maxHeight / (2.0f * metersPerPixel);
            const float dzdy = (hU - hD) * maxHeight / (2.0f * metersPerPixel);
            const float slope = std::sqrt(dzdx * dzdx + dzdy * dzdy);  // tanθ

            const float h = heights[sampleIndex(rows, c, r)];
            // 岩石：tanθ 0.40（21.8°）起、0.80（38.7°）满。
            // 起手点必须高于"缓坡"（p50 只有 0.187）而低于"边界渐变带"
            // （p90 0.655）—— 否则要么整片没石头，要么整圈全是石头。
            float wRock = std::clamp((slope - 0.40f) / 0.40f, 0.0f, 1.0f);
            // 雪：0.55 起、0.68 满（33 m .. 41 m，最高峰 43.7 m）；陡壁挂不住雪
            float wSnow = std::clamp((h - 0.55f) / 0.13f, 0.0f, 1.0f) *
                          (1.0f - wRock);
            // 层 1（2026-10-24 起是 `concrete_moss`，原来是 terrain_dirt）：
            // 0.36 起、0.46 满，0.50 起淡出、0.56 消失（再往上交给雪），
            // 并且给岩石和雪让位。海拔中段露出来的裸土 / 旧铺装，语义没变，
            // 只是材质从纯色泥土换成了带纹理的苔藓混凝土。
            float wMid = std::clamp((h - 0.36f) / 0.10f, 0.0f, 1.0f) *
                         std::clamp((0.56f - h) / 0.06f, 0.0f, 1.0f) *
                         (1.0f - wRock) * (1.0f - wSnow);
            float wGrass = std::max(0.0f, 1.0f - wRock - wSnow - wMid);

            // 四个通道加起来恒 = 1（见上面的叠放次序），不需要再归一化；
            // 保险起见仍按 sum 压一次，防浮点误差让 uint8 溢出。
            const float sum = wGrass + wMid + wRock + wSnow;
            const float k = (sum > 1.0f) ? (1.0f / sum) : 1.0f;
            const std::size_t base = sampleIndex(rows, c, r) * 4u;
            splat[base + 0] = static_cast<std::uint8_t>(wGrass * k * 255.0f);
            splat[base + 1] = static_cast<std::uint8_t>(wMid * k * 255.0f);
            splat[base + 2] = static_cast<std::uint8_t>(wRock * k * 255.0f);
            splat[base + 3] = static_cast<std::uint8_t>(wSnow * k * 255.0f);
        }
    }
    return splat;
}

} // namespace

// ============================================================
// 场景同步
// ============================================================
void LandscapeEditor::sync() {
    scene::Scene& sc = m_ctx.editorScene();

    // 只取第一份地形：本工程的地形是"一个场景一片地"（W4 的取舍）。
    // 真要多片地形，笔刷的目标应该由**选中项**而不是"第一个"决定 ——
    // 那时这里改成"选中项是地形就用它"即可。
    ecs::Entity found{};
    sc.forEachTerrain([&](ecs::Entity e, ecs::TerrainComponent&) {
        if (!found.valid()) found = e;
    });
    // 一份地形都没有：清空绑定（面板会切成"创建"向导）
    if (!found.valid()) {
        if (m_valid || !m_loggedMissing) {
            VK_LOG_INFO("LS-TERRAIN none (scene has no terrain entity)");
            m_loggedMissing = true;
        }
        clearLiveBinding();   // 组件可能已经没了，函数内部自己判空
        m_valid = false;
        m_entity = ecs::Entity{};
        m_source.clear();
        m_undo.clear();
        m_redo.clear();
        return;
    }
    m_loggedMissing = false;

    const auto* tc = sc.world().get<ecs::TerrainComponent>(found);
    if (!tc) return;
    const float hs = tc->heightScale;

    // 同一个实体、同一个资产 → 只在高度倍率变了才动（不必每帧读磁盘）
    if (m_valid && found.id == m_entity.id && tc->source == m_source &&
        std::abs(hs - m_heightScale) < 1.0e-4f) {
        return;
    }

    std::string err;
    terrain::TerrainAsset a;
    if (!terrain::loadTerrain(tc->source, a, &err)) {
        VK_LOG_ERROR("LS-LOAD-FAIL path=%s reason=%s", tc->source.c_str(),
                     err.c_str());
        m_valid = false;
        return;
    }
    const bool switched = (!m_valid || m_source != tc->source);
    // m_asset 要被整体替换 → 之前 publishToRenderer 挂出去的 &m_asset.heights
    // 等地址全部作废。必须在 move 之前清，否则渲染层可能读到已释放的 vector。
    clearLiveBinding();
    m_entity = found;
    m_source = tc->source;
    m_heightScale = (hs > 1.0e-4f) ? hs : 1.0f;
    m_asset = std::move(a);
    m_valid = true;
    // 换绑到**另一个**实体时，上面的 clearLiveBinding 清的是旧实体的组件；
    // 新实体的组件若在更早的绑定里留下过指针，这里再清一遍（此时 m_entity
    // 已经是 found，清的就是新组件）。刚读盘加载完，GPU 侧由
    // createInstanceResources 从盘上重传，所以顺手把脏位也归零。
    clearLiveBinding();
    m_strokeCount = 0;
    // 换了资产 → 旧的撤销记录指向的像素已经不是同一张图了
    bool justSwitched = false;
    if (switched) {
        m_undo.clear();
        m_redo.clear();
        m_snapshotValid = false;
        justSwitched = true;
    }
    VK_LOG_INFO("LS-TERRAIN path=%s cols=%d rows=%d mpp=%.2f maxH=%.1f "
                "heightScale=%.2f",
                m_source.c_str(), m_asset.desc.cols, m_asset.desc.rows,
                static_cast<double>(m_asset.desc.metersPerPixel),
                static_cast<double>(m_asset.desc.maxHeight),
                static_cast<double>(m_heightScale));

    // 刚换绑到一份地形（= 打开了一个带地形的工程 / 换成另一块地形）：
    // 把出生点抬到地表。**只在换绑这一刻做**，不做每帧 —— 每帧贴地的话
    // 用户就没法把出生点摆到台阶、桥面、二楼平台上了。
    if (justSwitched) seatGameStartsOnTerrain();
}

glm::mat4 LandscapeEditor::modelMatrix() const {
    if (!m_valid) return glm::mat4(1.0f);
    return m_ctx.editorScene().worldMatrix(m_entity);
}

// ============================================================
// 射线 / 高度查询
// ============================================================
bool LandscapeEditor::raycast(const glm::vec3& rayOrigin,
                              const glm::vec3& rayDir, glm::vec3& worldHit,
                              glm::vec3& worldNormal,
                              glm::vec3* localHit) const {
    if (!m_valid || m_asset.heights.empty()) return false;

    physics::Heightfield hf = m_asset.heightfield();
    // 组件的 heightScale 是**额外**倍率（Inspector 里那个滑条），
    // 不乘进来的话"把地形整体拉高 2 倍"之后笔刷会打偏一半。
    hf.heightScale = m_asset.desc.maxHeight *
                     std::max(m_heightScale, 1.0e-4f);

    const glm::mat4 model = modelMatrix();
    const glm::mat4 inv = glm::inverse(model);
    const glm::vec3 lo = glm::vec3(inv * glm::vec4(rayOrigin, 1.0f));
    const glm::vec3 ld = glm::vec3(inv * glm::vec4(rayDir, 0.0f));

    physics::HeightHit hit;
    if (!physics::raycast(hf, lo, ld, 0.0f, hit)) return false;

    worldHit = glm::vec3(model * glm::vec4(hit.point, 1.0f));
    const glm::mat3 nrmMat(model);
    worldNormal = glm::length(hit.normal) > 1.0e-6f
                      ? glm::normalize(nrmMat * hit.normal)
                      : glm::vec3(0.0f, 0.0f, 1.0f);
    if (localHit) *localHit = hit.point;
    return true;
}

float LandscapeEditor::heightAtWorld(float x, float y) const {
    if (!m_valid || m_asset.heights.empty()) return 0.0f;
    physics::Heightfield hf = m_asset.heightfield();
    hf.heightScale = m_asset.desc.maxHeight *
                     std::max(m_heightScale, 1.0e-4f);
    // 世界 XY → 地形局部 XY（忽略旋转带来的 Z 变化：本工程的地形都是
    // 平放的；真要做"斜着摆的地形"时这里要解一个平面方程）
    const glm::mat4 inv = glm::inverse(modelMatrix());
    const glm::vec3 lo = glm::vec3(inv * glm::vec4(glm::vec3(x, y, 0.0f), 1.0f));
    return physics::heightAt(hf, lo.x, lo.y);
}

glm::vec3 LandscapeEditor::normalAtWorld(float x, float y) const {
    if (!m_valid) return glm::vec3(0.0f, 0.0f, 1.0f);
    // 步长取一个采样间隔：比它小会把插值的折线也当坡度，比它大又会把
    // 真实的小起伏抹平。植被撒点用它决定"这株草立得多歪"。
    const float d = std::max(m_asset.desc.metersPerPixel, 0.5f);
    const float hl = heightAtWorld(x - d, y);
    const float hr = heightAtWorld(x + d, y);
    const float hd = heightAtWorld(x, y - d);
    const float hu = heightAtWorld(x, y + d);
    const glm::vec3 n(-(hr - hl) / (2.0f * d), -(hu - hd) / (2.0f * d), 1.0f);
    return (glm::length(n) > 1.0e-6f) ? glm::normalize(n)
                                      : glm::vec3(0.0f, 0.0f, 1.0f);
}

void LandscapeEditor::heightRangeWorld(float& lo, float& hi) const {
    lo = 0.0f;
    hi = 1.0f;
    if (!m_valid || m_asset.heights.empty()) return;
    float a = 1.0e30f, b = -1.0e30f;
    for (float v : m_asset.heights) {
        a = std::min(a, v);
        b = std::max(b, v);
    }
    const float s = m_asset.desc.maxHeight * std::max(m_heightScale, 1.0e-4f);
    lo = a * s;
    hi = b * s;
}

// ============================================================
// 创建地形
// ============================================================
bool LandscapeEditor::createTerrain(const std::string& name, int cols,
                                    int rows, float metersPerPixel,
                                    float maxHeight) {
    if (name.empty()) return false;
    if (cols < 3 || rows < 3) return false;
    if ((cols - 1) % terrain::kChunkCells != 0 ||
        (rows - 1) % terrain::kChunkCells != 0) {
        VK_LOG_ERROR("LS-CREATE-FAIL size=%dx%d reason=not_multiple_of_%d",
                     cols, rows, terrain::kChunkCells);
        return false;
    }
    if (metersPerPixel <= 0.0f || maxHeight <= 0.0f) return false;

    const std::string rel = "terrain/" + name + "/terrain.vterrain";
    const std::string abs = assets::resolveAssetPath(rel);
    const std::filesystem::path dir = std::filesystem::path(abs).parent_path();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        VK_LOG_ERROR("LS-CREATE-FAIL dir=%s reason=%s", dir.string().c_str(),
                     ec.message().c_str());
        return false;
    }

    std::vector<float> heights = generateDefaultHeights(cols, rows);
    std::vector<std::uint8_t> splat =
        generateDefaultSplat(heights, cols, rows, metersPerPixel, maxHeight);

    // ---- .vterrain（手写 JSON：字段与 terrain::parseDesc 一一对应）----
    {
        // 顺手把材质仓库读进来（惰性加载的第一次触发点之一）：上面那 4 个
        // 默认层材质 id 全是仓库里的条目，先确认它们在不在。
        // 不确认的后果是渲染层那边打一条 TERRAIN-LAYER-MISS，然后层层贴图
        // 静默退回纯色 —— 那时很难看出到底是"仓库没生成"还是"id 写错了"。
        const assets::MaterialLibrary& lib = m_ctx.materialLibrary();
        for (int i = 0; i < 4; ++i) {
            if (lib.size() > 0 && !lib.find(kLayerIds[i])) {
                VK_LOG_WARN("LS-LAYER-MISS idx=%d id=%s（材质仓库里没有这个 id，"
                            "这一层会退回纯色）", i, kLayerIds[i]);
            }
        }
        std::ofstream f(dir / "terrain.vterrain");
        if (!f) return false;
        f << "{\n"
          << "  \"type\": \"terrain\",\n"
          << "  \"version\": 1,\n"
          << "  \"name\": \"" << name << "\",\n"
          << "  \"cols\": " << cols << ",\n"
          << "  \"rows\": " << rows << ",\n"
          << "  \"metersPerPixel\": " << metersPerPixel << ",\n"
          << "  \"maxHeight\": " << maxHeight << ",\n"
          << "  \"splatRepeat\": 1.0,\n"
          << "  \"layerTiling\": 6.0,\n"
          << "  \"height\": \"height.r32\",\n"
          << "  \"splat\": \"splat.rgba8\",\n"
          << "  \"layers\": [\n";
        for (int i = 0; i < 4; ++i) {
            f << "    \"" << kLayerIds[i] << "\""
              << (i < 3 ? "," : "") << "\n";
        }
        f << "  ]\n}\n";
    }
    {
        std::ofstream f(dir / "height.r32", std::ios::binary);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(heights.data()),
                static_cast<std::streamsize>(heights.size() * sizeof(float)));
    }
    {
        std::ofstream f(dir / "splat.rgba8", std::ios::binary);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(splat.data()),
                static_cast<std::streamsize>(splat.size()));
    }

    VK_LOG_INFO("LS-CREATE path=%s cols=%d rows=%d mpp=%.2f maxH=%.1f",
                rel.c_str(), cols, rows, static_cast<double>(metersPerPixel),
                static_cast<double>(maxHeight));

    // ---- 场景里挂一个地形实体 ----
    scene::Scene& sc = m_ctx.editorScene();
    ecs::Entity created{};
    m_ctx.structuralEdit("Create Terrain", [&] {
        created = sc.createObject(name);
        ecs::TerrainComponent tc;
        tc.source = rel;
        sc.world().add<ecs::TerrainComponent>(created, tc);
    });
    if (!created.valid()) return false;

    // 坐在栅格平面上（kGroundEpsilon = 0，地形最低点与栅格严格同面）。
    // 山谷高度是 0（见 generateDefaultHeights），所以整片地的**最低点**
    // 正好落在栅格上 —— 这就是"地形建立在原地面基础上"。
    if (auto* tr = sc.world().get<ecs::TransformComponent>(created)) {
        tr->position.z = kGroundEpsilon;
    }
    VK_LOG_INFO("LS-CREATE-SEAT z=%.2f (same plane as grid)",
                static_cast<double>(kGroundEpsilon));

    m_ctx.select(created);
    m_ctx.notify(std::string(TR("Terrain created: ")) + name);
    m_ctx.dirty() = true;
    m_valid = false;   // 强制 sync 重新绑定
    sync();            // 立刻绑定（本帧面板就会切到笔刷 UI）
    // 新地形一落地就把出生点抬到地表 —— 否则它继续躺在 z=0，被新地形埋掉。
    // 顺序要在 sync() 之后：seat 依赖 m_valid / m_asset 已经指向新地形。
    seatGameStartsOnTerrain();
    // 取景也走"正常高度"那一档（2026-10-24）：刚建完地形紧接着就是刷地表，
    // 这时候把镜头甩到 500 m 外看全貌，还得自己飞回来。
    focusCameraAtPlayer();
    return m_valid;
}

// 把相机摆到"一眼能看见整片地形"的位置。
//
// ⚠ 不能用 setTarget + setDistance 这一对：轨道相机的 distance 被钳在
//   100 m 以内（Camera::setDistance），而地形是 512 m 见方 —— 结果就是
//   "聚焦"之后相机贴在地面上，画面里只有一片绿。所以这里直接摆位置 +
//   朝向。
//
// 朝向公式见 Camera::angleDir：angleDir = (cos(p)cos(yaw), -cos(p)sin(yaw),
//   sin(p))；相机在 target + angleDir·d，视线是 -angleDir。所以"站在 -Y
//   往 +Y 看"要 angleDir 的 y 分量为负 → yaw = +π/2，视线方向是
//   (0, +cos(p), -sin(p))，即朝 +Y 且向下俯 p 弧度。
//
// 摆位就一个约束：**视线正好穿过地形中心高度**。
//   水平后退量 A 拆到 x/y 上：相机在 (A·cos(yaw), -A·sin(yaw), z)，
//   视线落在原点时下降了 A·tan(p)，所以 z = hCenter + A·tan(p)。
//   yaw = π/2 就是"站在 -Y 往 +Y 看"（上式退化成 (0, -A, z)）。
void LandscapeEditor::placeCamera(float back, float pitch, float yaw) {
    if (!m_valid) return;
    scene::Camera& cam = m_ctx.editorScene().camera();
    // 地形"平均高度"：**直接从高度图算**，不要猜系数。
    // 猜的时候只要生成公式一改（振幅从 0.30 抬到 0.46），视线就会全部
    // 打到地面以下 —— 表现又是"一屏绿"。66k 个 float 求和只在切模式时
    // 跑一次，不值得为它省。
    float hMean = 0.46f;
    if (!m_asset.heights.empty()) {
        double sum = 0.0;
        for (float v : m_asset.heights) sum += v;
        hMean = static_cast<float>(sum /
                                   static_cast<double>(m_asset.heights.size()));
    }
    const float hCenter = std::max(hMean * m_asset.desc.maxHeight * m_heightScale,
                                   4.0f);
    const float z = hCenter + back * std::tan(pitch);
    const float px = back * std::cos(yaw);
    const float py = -back * std::sin(yaw);

    cam.setYaw(yaw);
    cam.setPitch(pitch);
    cam.setPosition(glm::vec3(px, py, z));
    VK_LOG_INFO("LS-FRAME back=%.1f pitch=%.2f yaw=%.2f pos=(%.1f,%.1f,%.1f)",
                static_cast<double>(back), static_cast<double>(pitch),
                static_cast<double>(yaw), static_cast<double>(px),
                static_cast<double>(py), static_cast<double>(z));
}

void LandscapeEditor::frameTerrain(float backFactor, float pitch) {
    if (!m_valid) return;
    const glm::vec2 size = m_asset.worldSize();
    placeCamera(std::max(size.x, size.y) * backFactor, pitch);
}

// 整片地形概览。
//
// 视距**按视口长宽比算**，不能给个固定系数：SplitLayout 下视口常年是
// 竖窄的（实测 440×520，aspect≈0.85），45° 竖直视场对应的水平半视场只有
// 19°；上一版固定 0.42·边长（215 m）时，512 m 见方的地形横向只装得下
// ~34%，画面就成了一条连绵的绿带 —— 看着像"地形全是草"，其实只是没装进去。
//
// 约束只有一条：**半个地形宽要落在水平半视场里**。
//   tan(fovY/2)·aspect = 水平半视场正切
//   注视点距离 d ≥ (0.5·span·1.06) / 水平半视场正切
// 相机沿视线后退 d，水平后退量 A = d·cos(pitch)。
void LandscapeEditor::focusCamera() {
    if (!m_valid) return;
    const float pitch = 0.72f;   // 41° 俯角：既有立体感，又能看清层带分布
    // 3/4 视角：yaw 偏离正轴 17°。正对着看时地形的边界与画面边框平行，
    // 大片平缓地表会被压成"一条色带"；斜一点能借地形的自身边界和明暗
    // 交界面读出起伏来。
    const float yaw = 1.5707963f - 0.30f;
    const glm::vec2 size = m_asset.worldSize();
    const float span = std::max(size.x, size.y);

    float aspect = 1.0f;
    const glm::vec2 vs = m_ctx.viewportSize();
    if (vs.x > 1.0f && vs.y > 1.0f) aspect = vs.x / vs.y;
    const float halfV =
        std::tan(m_ctx.editorScene().camera().fov() * 0.5f * 0.0174532925f);
    const float halfH = std::max(halfV * aspect, 1e-3f);
    const float needDist = (0.5f * span * 1.06f) / halfH;   // 留 6% 余量

    float back = needDist * std::cos(pitch);
    // 兜底：极长/极窄的视口下别把相机甩到几公里外（远景会糊成一片）
    back = std::clamp(back, span * 0.30f, span * 6.0f);
    placeCamera(back, pitch, yaw);
}

// ============================================================
// 正常视角：进入地形模式 / Ctrl+. 用（2026-10-24）
// ============================================================
// 用户原话："在点击 Landscape 进入地形模式时会把视野拉得非常远，此时再次
// 把视野调整到正常的高度和视角，并且对着角色的位置。"
//
// 上一版进入模式就调 `focusCamera()`（整片 512 m 地形全部装进画面）：视距
// 按视口长宽比反算出来 ~500 m 开外 —— 那个取景适合"看一眼地形全貌"，但每
// 次切模式都被甩到天上去，还怎么刷地形。
//
// 这里换成工作高度：水平退 18 m、俯角 28°，相机离地表约 9.6 m。笔刷圆圈
// （默认半径远小于 10 m）与地形起伏都在画面里，同时仍看得出层带分布。
//
// 朝向：**沿用当前 yaw**，不硬切到某个固定方位 —— 用户上一步可能正对着一
// 道山脊，硬转 180° 会让人瞬间失去方位感。位置是相对注视点算的，所以"对着
// 角色"这件事与 yaw 无关，永远成立。
void LandscapeEditor::focusCameraAtPlayer() {
    if (!m_valid) return;

    // 注视点：第一个启用的 GameStart（角色就是在那儿生成的）。
    // 没有出生点就退到地形中心 —— 与"按 0 落到地形上"的兜底同源。
    glm::vec3 target(0.0f);
    bool found = false;
    ecs::World& w = m_ctx.editorScene().world();
    w.each<ecs::GameStartComponent>(
        [&](ecs::Entity e, ecs::GameStartComponent& g) {
            if (found || !g.enabled) return;
            if (const auto* tf = w.get<ecs::TransformComponent>(e)) {
                target = tf->position;
                found = true;
            }
        });
    if (!found) target = glm::vec3(0.0f, 0.0f, heightAtWorld(0.0f, 0.0f));

    constexpr float kBack = 18.0f;       // 水平后退量（米）
    constexpr float kPitch = 0.49f;      // 28° 俯角
    scene::Camera& cam = m_ctx.editorScene().camera();
    const float yaw = cam.yaw();

    // 与 placeCamera 同一套几何：angleDir = (cos p cos yaw, -cos p sin yaw,
    // sin p)，相机 = target + angleDir·d，d = back / cos p。
    const glm::vec3 pos(target.x + kBack * std::cos(yaw),
                        target.y - kBack * std::sin(yaw),
                        target.z + kBack * std::tan(kPitch));
    cam.setYaw(yaw);
    cam.setPitch(kPitch);
    cam.setPosition(pos);
    VK_LOG_INFO("LS-FRAME-PLAYER target=(%.2f,%.2f,%.2f) source=%s "
                "back=%.1f pitch=%.2f yaw=%.2f pos=(%.2f,%.2f,%.2f)",
                static_cast<double>(target.x), static_cast<double>(target.y),
                static_cast<double>(target.z),
                found ? "gamestart" : "terrain-center",
                static_cast<double>(kBack), static_cast<double>(kPitch),
                static_cast<double>(yaw), static_cast<double>(pos.x),
                static_cast<double>(pos.y), static_cast<double>(pos.z));
}

void LandscapeEditor::focusCameraClose() {
    // 植被视角：贴近地面看单株（树 5 m 高）。俯角 16° 时相机在
    // 21 + 72·0.29 ≈ 42 m 高，72 m 外竖直可见 ≈ 83 m —— 一棵树约占
    // 屏高的 6%，撒点密度看得清，又不会一头扎进草里。
    frameTerrain(0.14f, 0.28f);
}

bool LandscapeEditor::createWithCurrentSettings() {
    return createTerrain(m_newName, m_newCols, m_newRows, m_newMpp,
                         m_newMaxHeight);
}

// ============================================================
// 落盘
// ============================================================
void LandscapeEditor::publishToRenderer(bool splatToo, bool holesToo) {
    // 渲染层只认"组件脏了"这件事 —— 但两种脏的代价差三个数量级：
    //
    //   dirty        → clearInstances + createTerrain：销毁/新建 sampler、
    //                  重建描述符集、重传 3 MB 顶点、还要等 GPU 空闲。
    //                  ~10 ms 一次，**只能**用于集合/层材质这种结构变更。
    //   heightsDirty → 只把那张 264 KB 的 R32 图原地重传（顶点着色器直接
    //                  采样高度图，顶点本身是平坦格子）。不碰任何 VkObject。
    //
    // 笔刷走后者。以前这里写的是 tc->dirty = true：一笔拖 0.5 秒就是十几次
    // 重建，而重建会销毁还在飞行中的帧所引用的 sampler —— 实测几秒钟后
    // 驱动返回 VK_ERROR_DEVICE_LOST，编辑器直接没了。
    if (auto* tc = m_ctx.editorScene().world().get<ecs::TerrainComponent>(
            m_entity)) {
        tc->heightsDirty = true;
        if (splatToo) tc->splatDirty = true;
        if (holesToo) tc->holesDirty = true;
        // 2026-10-21：把内存里这份挂上去，TerrainPass 就不再回磁盘重读。
        // 地址会在下一次 sync/m_asset 重赋值时失效，所以每次 publish 都重挂；
        // 换资产的地方由 clearLiveBinding() 显式清掉。
        if (m_valid && !m_asset.heights.empty()) {
            tc->liveHeights = &m_asset.heights;
            tc->liveSplat = m_asset.splat.empty() ? nullptr : &m_asset.splat;
            tc->liveHoles = m_asset.holes.empty() ? nullptr : &m_asset.holes;
        }
    }
    m_ctx.dirty() = true;
}

void LandscapeEditor::clearLiveBinding() {
    // editorScene 在 sync() 里才保证有效；这里只处理"已经绑过"的情况。
    if (!m_entity.valid()) return;
    if (auto* tc = m_ctx.editorScene().world().get<ecs::TerrainComponent>(
            m_entity)) {
        tc->liveHeights = nullptr;
        tc->liveSplat = nullptr;
        tc->liveHoles = nullptr;
        // 指向活数据的指针没了，渲染层只能读盘 —— 但盘上那份可能还是旧的
        // （拖拽期间没写）。所以顺手把"读盘"这条路也堵上：脏位清掉，
        // 等下一次 publish/save 重新提供数据。
        tc->heightsDirty = false;
        tc->splatDirty = false;
        tc->holesDirty = false;
    }
}

void LandscapeEditor::saveAssets(bool splatToo, bool holesToo) {
    if (!m_valid) return;
    const auto t0 = std::chrono::steady_clock::now();
    const std::string abs = assets::resolveAssetPath(m_source);
    const std::filesystem::path dir = std::filesystem::path(abs).parent_path();

    // ---- 落盘前把高度收敛回 [0,1]（2026-10-22 第十一轮）----
    //
    // 抬升笔刷**故意**允许冲到 `kMaxNorm = 8`（见文件顶部那段：不留余量的话
    // 笔心先撞 1.0、外圈继续涨，收尾必然是一块平顶）。但 `.r32` 的契约为
    // 归一化 [0,1]，而以前的这里是"内存里是什么就写什么"：
    //
    //     抬山 → 保存 → 磁盘上 max=8.000 → 下次读盘 height_not_normalized
    //     → TERRAIN-INSTANCE-FAIL → 工程打不开
    //
    // 收敛本身在 `terrain::normalizeHeightRange`（那是**资产契约**，不属于
    // 编辑器，所以住 engine/terrain 层、能被无窗口自检直接覆盖）。它会
    // 同乘同除地保住世界高度 —— 地形长得一模一样。
    //
    // ⚠ 撤销栈必须跟着缩。记录里存的是**归一化高度**（`StrokeRecord::height`
    //   是 vector<float>），不缩的话撤销一次地形会跳回 8 倍高。
    //   `m_asset.splat/holes` 与所有"米"量（m_radius / m_maxDelta /
    //   m_flattenHeight / m_carveCenterM）都不受影响。
    const float inv = terrain::normalizeHeightRange(m_asset);
    if (inv != 1.0f) {
        for (auto* stack : {&m_undo, &m_redo}) {
            for (StrokeRecord& rec : *stack) {
                for (float& h : rec.height) h *= inv;
            }
        }
        if (!terrain::rewriteMaxHeight(m_source, m_asset.desc.maxHeight)) {
            VK_LOG_ERROR("LS-SAVE-FAIL vterrain path=%s", abs.c_str());
        }
        VK_LOG_INFO("LS-SAVE-RENORM scale=%.4f maxHeight=%.4f "
                    "(undo=%zu redo=%zu)",
                    static_cast<double>(inv),
                    static_cast<double>(m_asset.desc.maxHeight), m_undo.size(),
                    m_redo.size());
    }

    {
        std::ofstream f(dir / m_asset.desc.heightFile, std::ios::binary);
        if (!f) {
            VK_LOG_ERROR("LS-SAVE-FAIL path=%s", abs.c_str());
            return;
        }
        f.write(reinterpret_cast<const char*>(m_asset.heights.data()),
                static_cast<std::streamsize>(m_asset.heights.size() *
                                             sizeof(float)));
    }
    if (splatToo) {
        std::ofstream f(dir / m_asset.desc.splatFile, std::ios::binary);
        if (f) {
            f.write(reinterpret_cast<const char*>(m_asset.splat.data()),
                    static_cast<std::streamsize>(m_asset.splat.size()));
        }
    }
    // 挖洞遮罩单独一份文件（holes.r8）。只在真的挖过洞之后才写 ——
    // 从没挖过就不写，旧资产目录里也就不会凭空多出一个文件。
    if (holesToo && !m_asset.holes.empty()) {
        std::ofstream f(dir / m_asset.desc.holeFile, std::ios::binary);
        if (f) {
            f.write(reinterpret_cast<const char*>(m_asset.holes.data()),
                    static_cast<std::streamsize>(m_asset.holes.size()));
        }
    }

    publishToRenderer(splatToo, holesToo);
    m_pendingSave = false;
    m_saveTimer = 0.0f;

    // 一笔只落一次盘（抬笔时），把这次 I/O 的代价记下来。以前它发生在
    // 每 80 ms 的拖拽路径上 —— 也就是每秒 12 次写 + 12 次回读。
    const float ms = std::chrono::duration<float, std::milli>(
                         std::chrono::steady_clock::now() - t0).count();
    VK_LOG_INFO("LS-SAVE ms=%.2f bytes=%zu splat=%d holes=%d",
                static_cast<double>(ms),
                m_asset.heights.size() * sizeof(float), splatToo ? 1 : 0,
                holesToo ? 1 : 0);
}

void LandscapeEditor::flush() {
    // 先收尾笔划再落盘：commitSnapshot 会把脏矩形压进撤销栈，之后
    // 这份数据才是"用户看到的最终态"。
    if (m_stroking) {
        m_stroking = false;
        commitSnapshot();
        m_pendingSave = true;
    }
    if (m_valid && m_pendingSave) saveAssets(true);
}

// ============================================================
// 笔划撤销
// ============================================================
//
// 一笔可能同时改三张图（高度 / 层权重 / 挖洞遮罩），而它们的**存储形状
// 不一样**（float / RGBA8 / R8）。这里用一套"矩形 + stride"的通用拷贝把
// 三路统一起来，免得每加一种通道就把 undo/redo 各抄一遍。
namespace {

// 闭区间包围盒；c1 < c0 表示"空"
struct DirtyBox {
    int c0 = 0, r0 = 0, c1 = -1, r1 = -1;
    bool valid() const { return c1 >= c0 && r1 >= r0; }
};

DirtyBox uniteBox(const DirtyBox& a, const DirtyBox& b) {
    if (!a.valid()) return b;
    if (!b.valid()) return a;
    DirtyBox u;
    u.c0 = std::min(a.c0, b.c0);
    u.r0 = std::min(a.r0, b.r0);
    u.c1 = std::max(a.c1, b.c1);
    u.r1 = std::max(a.r1, b.r1);
    return u;
}

// 把 src 里的矩形区间拷进 dst（从零开始，按行优先：外层 col、内层 row）。
// stride：每个采样点在 src 里占几个元素（高度/洞 = 1，权重 = 4）。
template <typename T>
void rectOut(std::vector<T>& dst, const std::vector<T>& src, int rows,
             const DirtyBox& rc, int stride) {
    dst.clear();
    if (!rc.valid()) return;
    const std::size_t n = static_cast<std::size_t>(rc.c1 - rc.c0 + 1) *
                          static_cast<std::size_t>(rc.r1 - rc.r0 + 1) *
                          static_cast<std::size_t>(stride);
    if (src.size() < n) return;
    dst.reserve(n);
    for (int c = rc.c0; c <= rc.c1; ++c) {
        for (int r = rc.r0; r <= rc.r1; ++r) {
            const std::size_t i = sampleIndex(rows, c, r) * static_cast<std::size_t>(stride);
            for (int k = 0; k < stride; ++k) dst.push_back(src[i + static_cast<std::size_t>(k)]);
        }
    }
}

// rectOut 的逆操作。src 的长度必须与矩形×stride 一致，否则整块跳过
// （宁可"这一路没恢复"也不要写错位 —— 错位会让地形长出一条斜条纹）。
template <typename T>
bool rectIn(std::vector<T>& dst, const std::vector<T>& src, int rows,
            const DirtyBox& rc, int stride) {
    if (!rc.valid()) return false;
    const std::size_t n = static_cast<std::size_t>(rc.c1 - rc.c0 + 1) *
                          static_cast<std::size_t>(rc.r1 - rc.r0 + 1) *
                          static_cast<std::size_t>(stride);
    if (src.size() != n || dst.size() < n) return false;
    std::size_t k = 0;
    for (int c = rc.c0; c <= rc.c1; ++c) {
        for (int r = rc.r0; r <= rc.r1; ++r) {
            const std::size_t i = sampleIndex(rows, c, r) * static_cast<std::size_t>(stride);
            for (int q = 0; q < stride; ++q) dst[i + static_cast<std::size_t>(q)] = src[k++];
        }
    }
    return true;
}

// 扫出"快照与现状不同"的包围盒。stride 同上。
template <typename T>
DirtyBox dirtyRectOf(const std::vector<T>& before, const std::vector<T>& now,
                 int cols, int rows, int stride) {
    DirtyBox rc{cols, rows, -1, -1};
    if (before.size() != now.size() || before.empty()) return rc;
    for (int c = 0; c < cols; ++c) {
        for (int r = 0; r < rows; ++r) {
            const std::size_t i = sampleIndex(rows, c, r) * static_cast<std::size_t>(stride);
            bool diff = false;
            for (int k = 0; k < stride; ++k) {
                if (before[i + static_cast<std::size_t>(k)] !=
                    now[i + static_cast<std::size_t>(k)]) {
                    diff = true;
                    break;
                }
            }
            if (!diff) continue;
            rc.c0 = std::min(rc.c0, c);
            rc.c1 = std::max(rc.c1, c);
            rc.r0 = std::min(rc.r0, r);
            rc.r1 = std::max(rc.r1, r);
        }
    }
    return rc;
}

} // namespace

// parts：位掩码，1 = 高度，2 = 权重，4 = 挖洞。
// 只拷**这一笔真的会改的**那几份：一笔抬升没必要把 264 KB 的权重也拷一遍
// （虽然一份也就 264 KB，但拖笔时每 60 ms 一次，能省就省）。
void LandscapeEditor::beginSnapshot(int parts) {
    if (!m_valid) return;
    m_snapshotParts = parts;
    if (parts & 1) m_snapshotHeights = m_asset.heights;
    else m_snapshotHeights.clear();
    if (parts & 2) m_snapshotSplat = m_asset.splat;
    else m_snapshotSplat.clear();
    if (parts & 4) m_snapshotHoles = m_asset.holes;
    else m_snapshotHoles.clear();
    m_snapshotValid = true;
}

void LandscapeEditor::dropSnapshot() {
    m_snapshotValid = false;
    m_snapshotParts = 0;
    m_snapshotHeights.clear();
    m_snapshotSplat.clear();
    m_snapshotHoles.clear();
}

void LandscapeEditor::commitSnapshot() {
    if (!m_snapshotValid || !m_valid) {
        dropSnapshot();
        return;
    }
    const int cols = m_asset.desc.cols;
    const int rows = m_asset.desc.rows;

    // 三路各自求脏矩形，再取并集。分着算的理由：一笔通常只碰其中一路
    // （抬升 / 层绘制 / 挖洞），而三张图的**尺寸**不同，用一个统一矩形
    // 去存会让"只改了高度"的笔画也把权重和遮罩各存一份。
    const DirtyBox rh = (m_snapshotParts & 1)
                        ? dirtyRectOf(m_snapshotHeights, m_asset.heights, cols, rows, 1)
                        : DirtyBox{};
    const DirtyBox rs = (m_snapshotParts & 2)
                        ? dirtyRectOf(m_snapshotSplat, m_asset.splat, cols, rows, 4)
                        : DirtyBox{};
    const DirtyBox rv = (m_snapshotParts & 4)
                        ? dirtyRectOf(m_snapshotHoles, m_asset.holes, cols, rows, 1)
                        : DirtyBox{};

    const DirtyBox all = uniteBox(uniteBox(rh, rs), rv);
    if (!all.valid()) {
        dropSnapshot();
        return;
    }

    StrokeRecord rec;
    rec.c0 = all.c0;
    rec.r0 = all.r0;
    rec.c1 = all.c1;
    rec.r1 = all.r1;
    // 用**并集**矩形填三份数据：三张图共用一个包围盒，写回时也就不必记三套。
    if (rh.valid()) rectOut(rec.height, m_snapshotHeights, rows, all, 1);
    if (rs.valid()) rectOut(rec.splat, m_snapshotSplat, rows, all, 4);
    if (rv.valid()) rectOut(rec.holes, m_snapshotHoles, rows, all, 1);

    dropSnapshot();
    m_undo.push_back(std::move(rec));
    while (m_undo.size() > kUndoDepth) m_undo.pop_front();
    m_redo.clear();
    const StrokeRecord& last = m_undo.back();
    VK_LOG_INFO("LS-UNDO-PUSH depth=%d bbox=(%d,%d)-(%d,%d) h=%zu s=%zu v=%zu",
                static_cast<int>(m_undo.size()), last.c0, last.r0, last.c1,
                last.r1, last.height.size(), last.splat.size(),
                last.holes.size());
}

// 把记录里的像素**原样写回**。记录里存的是一份状态快照 —— 到底是"改动前"
// 还是"改动后"由调用方决定（undoStroke 先抓 after 再写 before；redoStroke
// 先抓 before 再写 after）。这样一份结构就够两个方向用。
bool LandscapeEditor::applyRecord(const StrokeRecord& rec, bool /*useBefore*/) {
    if (!m_valid || rec.empty()) return false;
    const int rows = m_asset.desc.rows;
    const DirtyBox rc{rec.c0, rec.r0, rec.c1, rec.r1};
    bool any = false;
    if (!rec.height.empty()) any |= rectIn(m_asset.heights, rec.height, rows, rc, 1);
    if (!rec.splat.empty()) any |= rectIn(m_asset.splat, rec.splat, rows, rc, 4);
    if (!rec.holes.empty()) any |= rectIn(m_asset.holes, rec.holes, rows, rc, 1);
    return any;
}

// 抓当前状态（undo 时用来生成 redo 记录）
void LandscapeEditor::captureCurrent(const StrokeRecord& shape,
                                     StrokeRecord& out) const {
    out = shape;
    const int rows = m_asset.desc.rows;
    const DirtyBox rc{shape.c0, shape.r0, shape.c1, shape.r1};
    if (!shape.height.empty()) rectOut(out.height, m_asset.heights, rows, rc, 1);
    if (!shape.splat.empty()) rectOut(out.splat, m_asset.splat, rows, rc, 4);
    if (!shape.holes.empty()) rectOut(out.holes, m_asset.holes, rows, rc, 1);
}

// ============================================================
// 材质仓库联动
// ============================================================
bool LandscapeEditor::setLayerMaterial(int layer, const std::string& materialId) {
    if (layer < 0 || layer >= 4) return false;
    if (!m_valid) return false;
    if (materialId.empty()) return false;

    // 校验材质真的存在：先问仓库，仓库里没有就退回"文件在不在"。
    // 不校验的话手抖写错一个 id，画面上是"这一层突然变纯黑"，
    // 而日志里什么都没有 —— 那是本项目最容易误判成渲染 bug 的一类故障。
    const assets::MaterialLibrary& lib = m_ctx.materialLibrary();
    const assets::MaterialLibraryEntry* e = lib.find(materialId);
    if (!e && lib.size() > 0) {
        VK_LOG_WARN("LS-LAYER-MAT-UNKNOWN layer=%d id=%s（仓库里没有这个材质）",
                    layer, materialId.c_str());
        return false;
    }
    const std::string id = e ? e->id : materialId;

    auto* tc = m_ctx.editorScene().world().get<ecs::TerrainComponent>(m_entity);
    if (!tc) return false;
    if (tc->layerOverrides[layer] == id) return true;   // 没变

    tc->layerOverrides[layer] = id;
    // ⚠ 必须置 dirty：4 层贴图与层参数 UBO 都绑在实例的描述符集里，
    //   只有重建实例才换得掉（Renderer::syncTerrains 认这一位）。
    //   heightsDirty / splatDirty 走的是"原地重传一张图"，换不了材质。
    tc->dirty = true;
    m_ctx.dirty() = true;

    VK_LOG_INFO("LS-LAYER-MAT layer=%d id=%s albedo=%s normal=%s", layer,
                id.c_str(),
                (e && e->hasAlbedo()) ? e->albedoPath.c_str() : "-",
                (e && e->hasNormal()) ? e->normalPath.c_str() : "-");
    return true;
}

std::string LandscapeEditor::layerMaterial(int layer) const {
    if (layer < 0 || layer >= 4) return std::string();
    if (!m_valid) return std::string();
    // 组件覆盖优先 —— 与 TerrainPass::createTerrain 的取值顺序保持一致，
    // 否则面板上显示的和画面上看到的会是两回事。
    if (const auto* tc =
            m_ctx.editorScene().world().get<ecs::TerrainComponent>(m_entity)) {
        if (!tc->layerOverrides[layer].empty()) return tc->layerOverrides[layer];
    }
    return m_asset.desc.layerIds[static_cast<std::size_t>(layer)];
}

void LandscapeEditor::beginPaintLayer(int layer) {
    m_tool = Tool::PaintLayer;
    if (layer >= 0 && layer < 4) m_layer = layer;
    m_eraseLayer = false;
    VK_LOG_INFO("LS-TOOL paintlayer layer=%d", m_layer);
}

bool LandscapeEditor::isPaintingLayer() const {
    return m_tool == Tool::PaintLayer;
}

int LandscapeEditor::seatGameStartsOnTerrain() {
    if (!m_valid) return 0;
    ecs::World& w = m_ctx.editorScene().world();

    // 先收集再改：`each` 的循环体里改别的组件是安全的，但收集一次能让
    // "判定"与"落笔"两段分开 —— 后面若要加撤销命令，这里就是天然的分界。
    std::vector<ecs::Entity> targets;
    w.each<ecs::GameStartComponent>(
        [&](ecs::Entity e, ecs::GameStartComponent& g) {
            if (g.enabled) targets.push_back(e);
        });
    if (targets.empty()) return 0;

    int moved = 0;
    for (const ecs::Entity e : targets) {
        auto* tf = w.get<ecs::TransformComponent>(e);
        if (!tf) continue;
        const glm::vec3 before = tf->position;
        // 世界 XY 处的**地表高度**（含地形实体的平移与 heightScale）
        const float h = heightAtWorld(before.x, before.y);
        if (std::fabs(before.z - h) < 1e-3f) continue;
        tf->position.z = h;
        ++moved;
        const std::string* nmp = w.name(e);
        // 断言锚点：验收脚本靠它核对"出生点真的落在 terrainH 上"。
        VK_LOG_INFO("GAMESTART-SEAT name=%s before=(%.3f,%.3f,%.3f) "
                    "after=(%.3f,%.3f,%.3f) terrainH=%.3f source=%s",
                    nmp ? nmp->c_str() : "?", static_cast<double>(before.x),
                    static_cast<double>(before.y), static_cast<double>(before.z),
                    static_cast<double>(tf->position.x),
                    static_cast<double>(tf->position.y),
                    static_cast<double>(tf->position.z),
                    static_cast<double>(h), m_source.c_str());
    }
    if (moved > 0) m_ctx.dirty() = true;
    return moved;
}

bool LandscapeEditor::undoStroke() {
    if (!m_valid) return false;
    if (m_undo.empty()) {
        VK_LOG_INFO("LS-UNDO-EMPTY");
        return false;
    }
    StrokeRecord rec = std::move(m_undo.back());
    m_undo.pop_back();

    // 撤销 = 把 before 写回去；同时把 after 存进 redo 栈。
    // 先抓 after（就是现在的值），再写 before。
    StrokeRecord redo;
    captureCurrent(rec, redo);

    applyRecord(rec, /*useBefore=*/true);
    m_redo.push_back(std::move(redo));
    while (m_redo.size() > kUndoDepth) m_redo.pop_front();

    saveAssets(!rec.splat.empty(), !rec.holes.empty());
    VK_LOG_INFO("LS-UNDO depth=%d bbox=(%d,%d)-(%d,%d)", 
                static_cast<int>(m_undo.size()), rec.c0, rec.r0, rec.c1, rec.r1);
    m_ctx.notify(TR("Undo terrain stroke"));
    return true;
}

bool LandscapeEditor::redoStroke() {
    if (!m_valid) return false;
    if (m_redo.empty()) {
        VK_LOG_INFO("LS-REDO-EMPTY");
        return false;
    }
    StrokeRecord rec = std::move(m_redo.back());
    m_redo.pop_back();

    // redo 里存的是 after → 直接写回；再把当前（=before）压回 undo
    StrokeRecord undo;
    captureCurrent(rec, undo);

    applyRecord(rec, /*useBefore=*/true);   // rec 里已经是 after
    m_undo.push_back(std::move(undo));
    while (m_undo.size() > kUndoDepth) m_undo.pop_front();
    saveAssets(!rec.splat.empty(), !rec.holes.empty());
    VK_LOG_INFO("LS-REDO depth=%d", static_cast<int>(m_undo.size()));
    m_ctx.notify(TR("Redo terrain stroke"));
    return true;
}

// ============================================================
// 笔刷
// ============================================================
void LandscapeEditor::applyStroke(const glm::vec3& localHit, float dt,
                                  bool invert) {
    const TerrainDesc& d = m_asset.desc;
    const physics::Heightfield hf = m_asset.heightfield();
    const glm::vec2 g = hf.gridOf(glm::vec2(localHit.x, localHit.y));
    const float rs = m_radius / d.metersPerPixel;
    if (rs < 1.0f) return;

    const int c0 = std::max(0, static_cast<int>(std::floor(g.x - rs)));
    const int c1 = std::min(d.cols - 1, static_cast<int>(std::ceil(g.x + rs)));
    const int r0 = std::max(0, static_cast<int>(std::floor(g.y - rs)));
    const int r1 = std::min(d.rows - 1, static_cast<int>(std::ceil(g.y + rs)));
    if (c0 > c1 || r0 > r1) return;

    // 平滑要读"邻居的平均"，原地改会让先算的格子影响后算的格子 →
    // 整个笔刷朝一个方向拖尾。所以先把这一块拷出来。
    std::vector<float> patch;
    if (m_tool == Tool::Smooth) {
        patch.resize(static_cast<std::size_t>(c1 - c0 + 1) *
                     static_cast<std::size_t>(r1 - r0 + 1));
        for (int c = c0; c <= c1; ++c) {
            for (int r = r0; r <= r1; ++r) {
                patch[static_cast<std::size_t>(c - c0) *
                          static_cast<std::size_t>(r1 - r0 + 1) +
                      static_cast<std::size_t>(r - r0)] =
                    m_asset.heights[sampleIndex(d.rows, c, r)];
            }
        }
    }

    const float sign = invert ? -1.0f : 1.0f;
    const float flattenTarget =
        m_flattenHeight / std::max(d.maxHeight * m_heightScale, 1.0e-4f);
    // 一笔之内的位移限幅（归一化）。0 = 不限。
    const float maxDeltaNorm =
        (m_maxDelta > 1.0e-4f && m_snapshotValid)
            ? m_maxDelta / std::max(d.maxHeight * m_heightScale, 1.0e-4f)
            : 0.0f;

    for (int c = c0; c <= c1; ++c) {
        for (int r = r0; r <= r1; ++r) {
            const float dx = static_cast<float>(c) - g.x;
            const float dy = static_cast<float>(r) - g.y;
            const float t = std::sqrt(dx * dx + dy * dy) / rs;
            const float w = brushWeight(t, m_falloff);
            if (w <= 0.0f) continue;

            const std::size_t idx = sampleIndex(d.rows, c, r);
            float& h = m_asset.heights[idx];
            switch (m_tool) {
                case Tool::Raise: {
                    // 抬高 / 降低是**同一个算子**的符号差异（Shift 反向）。
                    //
                    // 抬升方向额外乘"剩余空间"系数（raiseHeadroom）：1.0 以下
                    // 恒为 1（手感与以前一模一样），超过 1.0 之后越接近
                    // kMaxNorm 越慢。这是"山峰"和"火山口"的分水岭 ——
                    // 硬钳在 1.0 的话，笔心先撞顶、外圈继续涨，收尾必然是一块
                    // 平顶；有了余量，笔心能继续长高，剖面始终是锥形。
                    const float room = (sign > 0.0f) ? raiseHeadroom(h) : 1.0f;
                    h += sign * m_strength * w * dt * kRaiseRate * room;
                    break;
                }
                case Tool::Smooth: {
                    // 3×3 邻域平均（在拷贝上取，避免方向性拖尾）
                    float sum = 0.0f;
                    int n = 0;
                    for (int dc = -1; dc <= 1; ++dc) {
                        for (int dr = -1; dr <= 1; ++dr) {
                            const int cc = c + dc;
                            const int rr = r + dr;
                            if (cc < c0 || cc > c1 || rr < r0 || rr > r1) {
                                sum += h;
                                ++n;
                                continue;
                            }
                            sum += patch[static_cast<std::size_t>(cc - c0) *
                                             static_cast<std::size_t>(r1 - r0 + 1) +
                                         static_cast<std::size_t>(rr - r0)];
                            ++n;
                        }
                    }
                    const float avg = sum / static_cast<float>(std::max(n, 1));
                    const float k = std::clamp(m_strength * w * dt * 8.0f,
                                               0.0f, 1.0f);
                    h += (avg - h) * k;
                    break;
                }
                case Tool::Flatten: {
                    const float k = std::clamp(m_strength * w * dt * 6.0f,
                                               0.0f, 1.0f);
                    h += (flattenTarget - h) * k;
                    break;
                }
                case Tool::Noise:
                case Tool::Erode:
                case Tool::PaintLayer:
                    break;   // 走各自的专用通道
            }
            h = std::clamp(h, 0.0f, kMaxNorm);
            if (maxDeltaNorm > 0.0f) {
                const float s = m_snapshotHeights[idx];
                h = std::clamp(h, std::max(0.0f, s - maxDeltaNorm),
                               std::min(kMaxNorm, s + maxDeltaNorm));
            }
        }
    }

    m_pendingSave = true;
}

// 噪声笔刷：往表面"长细节"。
//
// 为什么值得单独做一个工具：抬高/降低只能做**大形**，而真实地表的辨识度
// 几乎全在 1~20 m 尺度的颗粒上。手动抬高做不出这种层次，一刷噪声立刻就有。
void LandscapeEditor::applyNoiseStroke(const glm::vec3& localHit, float dt) {
    const TerrainDesc& d = m_asset.desc;
    const physics::Heightfield hf = m_asset.heightfield();
    const glm::vec2 g = hf.gridOf(glm::vec2(localHit.x, localHit.y));
    const float rs = m_radius / d.metersPerPixel;
    if (rs < 1.0f) return;

    const int c0 = std::max(0, static_cast<int>(std::floor(g.x - rs)));
    const int c1 = std::min(d.cols - 1, static_cast<int>(std::ceil(g.x + rs)));
    const int r0 = std::max(0, static_cast<int>(std::floor(g.y - rs)));
    const int r1 = std::min(d.rows - 1, static_cast<int>(std::ceil(g.y + rs)));
    if (c0 > c1 || r0 > r1) return;

    const float scale = std::max(m_noiseScale, 0.5f);
    for (int c = c0; c <= c1; ++c) {
        for (int r = r0; r <= r1; ++r) {
            const float dx = static_cast<float>(c) - g.x;
            const float dy = static_cast<float>(r) - g.y;
            const float t = std::sqrt(dx * dx + dy * dy) / rs;
            const float w = brushWeight(t, m_falloff);
            if (w <= 0.0f) continue;

            // 用**世界坐标**采噪声（而不是格坐标）：改 mpp 之后细节尺度
            // 不会跟着变，视觉上一致。
            const float wx = static_cast<float>(c) * d.metersPerPixel / scale;
            const float wy = static_cast<float>(r) * d.metersPerPixel / scale;
            const float n = fbm(wx, wy, m_noiseOctaves, m_noiseSeed) - 0.5f;

            float& h = m_asset.heights[sampleIndex(d.rows, c, r)];
            h += n * m_strength * w * dt * kNoiseRate * 2.0f;
            h = std::clamp(h, 0.0f, kMaxNorm);
        }
    }
    m_pendingSave = true;
}

// 热侵蚀：超过休止角的物质往低处搬。
//
// 这是"山看起来像山"的关键一步：正弦叠加出来的坡面处处光滑得像塑料，
// 而真实山坡在 ~30° 以上会碎成岩屑、坡脚堆积。热侵蚀就是这条规则：
// 相邻两格的高差超过 tan(休止角)×格距，就把超出的部分搬一半到低处。
//
// 用拷贝做（就地改会让搬运方向偏向遍历顺序，出现一条条斜纹）。
void LandscapeEditor::applyErodeStroke(const glm::vec3& localHit, float dt) {
    const TerrainDesc& d = m_asset.desc;
    if (d.cols < 3 || d.rows < 3) return;
    const physics::Heightfield hf = m_asset.heightfield();
    const glm::vec2 g = hf.gridOf(glm::vec2(localHit.x, localHit.y));
    const float rs = m_radius / d.metersPerPixel;
    if (rs < 1.0f) return;

    const int c0 = std::max(1, static_cast<int>(std::floor(g.x - rs)));
    const int c1 = std::min(d.cols - 2, static_cast<int>(std::ceil(g.x + rs)));
    const int r0 = std::max(1, static_cast<int>(std::floor(g.y - rs)));
    const int r1 = std::min(d.rows - 2, static_cast<int>(std::ceil(g.y + rs)));
    if (c0 > c1 || r0 > r1) return;

    const int pw = c1 - c0 + 3;     // 带一圈边框
    const int ph = r1 - r0 + 3;
    std::vector<float> src(static_cast<std::size_t>(pw) * ph, 0.0f);
    std::vector<float> dst = src;
    for (int c = c0 - 1; c <= c1 + 1; ++c) {
        for (int r = r0 - 1; r <= r1 + 1; ++r) {
            src[static_cast<std::size_t>(c - c0 + 1) *
                    static_cast<std::size_t>(ph) +
                static_cast<std::size_t>(r - r0 + 1)] =
                m_asset.heights[sampleIndex(d.rows, c, r)];
        }
    }

    const float talusTan =
        std::tan(std::clamp(m_erodeTalus, 5.0f, 80.0f) * 3.14159265f / 180.0f);
    const float cellMeters = d.metersPerPixel;
    // 归一化高度 → 米
    const float toMeters = std::max(d.maxHeight * m_heightScale, 1.0e-4f);
    const float rate = std::clamp(m_strength * dt * 26.0f, 0.0f, 0.45f);

    const int iters = std::clamp(m_erodeIterations, 1, 6);
    for (int it = 0; it < iters; ++it) {
        dst = src;
        for (int c = c0; c <= c1; ++c) {
            for (int r = r0; r <= r1; ++r) {
                const float dx = static_cast<float>(c) - g.x;
                const float dy = static_cast<float>(r) - g.y;
                const float t = std::sqrt(dx * dx + dy * dy) / rs;
                const float w = brushWeight(t, m_falloff);
                if (w <= 0.0f) continue;

                const std::size_t ci =
                    static_cast<std::size_t>(c - c0 + 1) *
                        static_cast<std::size_t>(ph) +
                    static_cast<std::size_t>(r - r0 + 1);
                const float h0 = src[ci] * toMeters;

                float diff[8];
                float total = 0.0f;
                int n = 0;
                for (int dc = -1; dc <= 1; ++dc) {
                    for (int dr = -1; dr <= 1; ++dr) {
                        if (dc == 0 && dr == 0) continue;
                        const std::size_t ni =
                            static_cast<std::size_t>(c + dc - c0 + 1) *
                                static_cast<std::size_t>(ph) +
                            static_cast<std::size_t>(r + dr - r0 + 1);
                        const float hn = src[ni] * toMeters;
                        const float dist =
                            cellMeters * ((dc != 0 && dr != 0) ? 1.41421356f : 1.0f);
                        const float excess = (h0 - hn) - talusTan * dist;
                        if (excess <= 0.0f) {
                            diff[n] = 0.0f;
                        } else {
                            diff[n] = excess;
                            total += excess;
                        }
                        ++n;
                    }
                }
                if (total <= 1.0e-6f) continue;

                // 一趟最多搬走"超出量"的 rate 倍 —— 迭代几次就自然收敛到
                // 休止角附近，而不是一次搬空（那会变成蜂窝）。
                const float move = total * rate * w;
                float moved = 0.0f;
                n = 0;
                for (int dc = -1; dc <= 1; ++dc) {
                    for (int dr = -1; dr <= 1; ++dr) {
                        if (dc == 0 && dr == 0) continue;
                        if (diff[n] <= 0.0f) {
                            ++n;
                            continue;
                        }
                        const std::size_t ni =
                            static_cast<std::size_t>(c + dc - c0 + 1) *
                                static_cast<std::size_t>(ph) +
                            static_cast<std::size_t>(r + dr - r0 + 1);
                        const float add = move * (diff[n] / total) / toMeters;
                        dst[ni] += add;
                        moved += add;
                        ++n;
                    }
                }
                dst[ci] -= moved;
            }
        }
        src = dst;
    }

    for (int c = c0; c <= c1; ++c) {
        for (int r = r0; r <= r1; ++r) {
            const float v = src[static_cast<std::size_t>(c - c0 + 1) *
                                    static_cast<std::size_t>(ph) +
                                static_cast<std::size_t>(r - r0 + 1)];
            m_asset.heights[sampleIndex(d.rows, c, r)] =
                std::clamp(v, 0.0f, kMaxNorm);
        }
    }
    m_pendingSave = true;
}

void LandscapeEditor::applyLayerStroke(const glm::vec3& localHit, float dt,
                                       bool invert) {
    const TerrainDesc& d = m_asset.desc;
    const physics::Heightfield hf = m_asset.heightfield();
    const glm::vec2 g = hf.gridOf(glm::vec2(localHit.x, localHit.y));
    const float rs = m_radius / d.metersPerPixel;
    if (rs < 1.0f) return;

    const int c0 = std::max(0, static_cast<int>(std::floor(g.x - rs)));
    const int c1 = std::min(d.cols - 1, static_cast<int>(std::ceil(g.x + rs)));
    const int r0 = std::max(0, static_cast<int>(std::floor(g.y - rs)));
    const int r1 = std::min(d.rows - 1, static_cast<int>(std::ceil(g.y + rs)));

    // 擦除 = 画回第 0 层（草地），而不是"把权重清零"：清零的像素四层
    // 全 0，shader 归一化时会出现 NaN/纯黑。
    const bool erase = invert || m_eraseLayer;
    const int layer = erase ? 0 : std::clamp(m_layer, 0, 3);

    for (int c = c0; c <= c1; ++c) {
        for (int r = r0; r <= r1; ++r) {
            const float dx = static_cast<float>(c) - g.x;
            const float dy = static_cast<float>(r) - g.y;
            const float t = std::sqrt(dx * dx + dy * dy) / rs;
            const float w = brushWeight(t, m_falloff);
            if (w <= 0.0f) continue;

            const std::size_t base = sampleIndex(d.rows, c, r) * 4u;
            const float k = std::clamp(m_strength * w * dt * 6.0f, 0.0f, 1.0f);
            // 朝 one-hot 插值：目标层 → 255，其余 → 0。插值天然保持
            // "四个通道加起来 ≈ 255"，不需要额外的归一化步骤。
            for (int i = 0; i < 4; ++i) {
                const float target = (i == layer) ? 255.0f : 0.0f;
                float v = static_cast<float>(m_asset.splat[base + i]);
                v += (target - v) * k;
                m_asset.splat[base + i] =
                    static_cast<std::uint8_t>(std::clamp(v, 0.0f, 255.0f));
            }
        }
    }

    m_pendingSave = true;
}

// ============================================================
// 形状雕刻（原"挖洞"，2026-10-21 重做）
// ============================================================
// 用户原话：
//   "挖洞的功能，不是将原地面中间剖开一个缺口，不能是破坏地面的拓扑结构；
//    最好是能否通过选择一简单的物体，在地面中拖动这个物体慢慢贯穿整个表面，
//    以这个物体的轮廓去给地面形变，构造出对应的类似洞穴的结构。"
//
// 旧实现是 UE 的 Visibility 层思路：写一张 holes 遮罩，片元着色器 discard
// 那些像素 → 地面上真的开一个洞。问题是那**就是**在破坏拓扑：洞边是硬边、
// 洞下面是空的（能看见天空/背面），而且高度图本身完全没变，物理和光照都
// 对不上。
//
// 现在改成"形状体压印"：选一个形状（球 / 方块 / 圆柱），把它沉到地表以
// 下，然后**按它的下表面削地形**。
//   · 球   → 下表面是球冠 → 压出碗状凹腔，边缘是光滑的收口
//   · 方块 → 平底 + 直壁的基坑，轮廓就是那个方形的边
//   · 圆柱 → 平底 + 直壁 + 圆轮廓的竖井
// 拖动 = 把形状体在地表里扫掠 → 一串连续的凹腔接成通道，也就是"贯穿"。
// 全程只改高度图：不开洞、不 discard、拓扑永远是完整的矩形网格。
//
// ⚠ 高度图**做不出真正的洞穴**（有拱顶、有悬垂、内部空腔）：每个 (x, y)
//   只能存一个高度，从上方看下去它必须是"单值曲面"。所以这里给的是
//   "以物体轮廓压出来的凹腔 / 通道"。要带顶的洞得换体素或 SDF，
//   那是另一条技术路线（本轮的取舍写进 docs/editor_roadmap.md §20）。
void LandscapeEditor::applyCarveStroke(const glm::vec3& localHit, float dt,
                                      bool invert) {
    const TerrainDesc& d = m_asset.desc;
    const physics::Heightfield hf = m_asset.heightfield();
    const glm::vec2 g = hf.gridOf(glm::vec2(localHit.x, localHit.y));
    const float rs = m_radius / d.metersPerPixel;
    if (rs < 1.0f) return;

    const int c0 = std::max(0, static_cast<int>(std::floor(g.x - rs)));
    const int c1 = std::min(d.cols - 1, static_cast<int>(std::ceil(g.x + rs)));
    const int r0 = std::max(0, static_cast<int>(std::floor(g.y - rs)));
    const int r1 = std::min(d.rows - 1, static_cast<int>(std::ceil(g.y + rs)));
    if (c0 > c1 || r0 > r1) return;

    // 形状体的几何**全在米里算**，最后再除回归一化。用归一化算的话，
    // 改 heightScale 之后"同样的笔刷压出来的坑"深度会跟着变，手感漂移。
    const float unitM = std::max(d.maxHeight * m_heightScale, 1.0e-4f);
    const float R = std::max(m_radius, d.metersPerPixel * 1.5f);
    // 形状体中心用**落笔那一刻**锚定的绝对高度（m_carveCenterM，单位米），
    // 整笔不再跟着当前地表走 —— 理由见 LandscapeEditor.h 里那个成员的注释。
    const float centerM = m_carveCenterM;
    // 方块半边长：同半径下方块看着比球"大一圈"，0.7 让三种形状的视觉体量接近。
    const float halfBox = R * 0.7f;

    // 每帧最多推进的比例。一帧压到位会很"跳"，而且拖动时落点间隔大于笔刷
    // 半径就会啃出锯齿；按速率逼近则像"物体被慢慢压进去"。
    const float rate = std::clamp(m_strength * dt * 14.0f, 0.0f, 1.0f);

    for (int c = c0; c <= c1; ++c) {
        for (int r = r0; r <= r1; ++r) {
            const float dx =
                (static_cast<float>(c) - g.x) * d.metersPerPixel;
            const float dy =
                (static_cast<float>(r) - g.y) * d.metersPerPixel;
            const float d2 = dx * dx + dy * dy;

            // 该点在形状体里的"半厚度"（中心 → 下表面）以及轮廓参数 t
            float half = 0.0f;
            float t = 0.0f;
            switch (m_carveShape) {
                case CarveShape::Sphere: {
                    if (d2 >= R * R) continue;
                    half = std::sqrt(R * R - d2);
                    t = std::sqrt(d2) / R;
                    break;
                }
                case CarveShape::Box: {
                    const float ax = std::fabs(dx);
                    const float ay = std::fabs(dy);
                    if (ax > halfBox || ay > halfBox) continue;
                    half = halfBox;
                    t = std::max(ax, ay) / halfBox;
                    break;
                }
                case CarveShape::Cylinder: {
                    if (d2 >= R * R) continue;
                    half = R;               // 上下都是平的
                    t = std::sqrt(d2) / R;
                    break;
                }
            }

            // 最外 5% 做窄过渡，避开"一个纹素的硬折线"；内部完全按形状的
            // 精确剖面走 —— 只有这样才叫"以物体的轮廓形变"。
            const float rim = std::clamp((1.0f - t) / 0.05f, 0.0f, 1.0f);
            const float k = rate * rim;
            if (k <= 0.0f) continue;

            const std::size_t idx = sampleIndex(d.rows, c, r);
            float& h = m_asset.heights[idx];
            if (!invert) {
                // 往下压：只在形状体的下表面**低于**当前地表时才动它。
                // 形状体整颗抬到地面以上时自然什么都不发生 ——
                // 这就是"物体压进去才有形变"。
                const float bottomN = (centerM - half) / unitM;
                if (bottomN < h) h += (bottomN - h) * k;
            } else {
                // Shift = 反向：按形状体的**上**表面堆出一个同轮廓的隆起
                const float topN = (centerM + half) / unitM;
                if (topN > h) h += (topN - h) * k;
            }
            h = std::clamp(h, 0.0f, kMaxNorm);
        }
    }
    m_pendingSave = true;
}

// ============================================================
// 坡道（Ramp）
// ============================================================
// 按下点 p0（高度 h0）→ 当前落点 p1（高度 h1），笔刷范围内的格子按
// "沿 p0→p1 方向的投影比例"在 h0..h1 之间线性插值。这是 UE 里 Ramp 工具的
// 核心用途：把一段陡坡改成可行走的坡道（或修一条上山的路）。
//
// ⚠ 插值的自变量是**世界 XY 上的投影**，不是到 p0 的距离：
//   后者会让坡道在垂直于 p0→p1 的方向上塌成一条沟。
void LandscapeEditor::applyRampStroke(const glm::vec3& localHit, float dt) {
    const TerrainDesc& d = m_asset.desc;
    const physics::Heightfield hf = m_asset.heightfield();
    const glm::vec2 g = hf.gridOf(glm::vec2(localHit.x, localHit.y));
    const float rs = m_radius / d.metersPerPixel;
    if (rs < 1.0f) return;

    m_rampEnd = glm::vec2(localHit.x, localHit.y);
    const glm::vec2 seg = m_rampEnd - m_rampStart;
    const float segLen2 = glm::dot(seg, seg);
    // 还没拖动（或几乎没动）→ 不做事。否则一整片会瞬间被压到 h0。
    if (segLen2 < (d.metersPerPixel * 2.0f) * (d.metersPerPixel * 2.0f)) return;

    // 终点高度：跟着地表走（拖动时地表本身可能已被前面的笔画改过）
    const float scale = std::max(d.maxHeight * m_heightScale, 1.0e-4f);
    m_rampEndH = m_asset.heights[sampleIndex(
        d.rows, std::clamp(static_cast<int>(std::round(g.x)), 0, d.cols - 1),
        std::clamp(static_cast<int>(std::round(g.y)), 0, d.rows - 1))];

    const int c0 = std::max(0, static_cast<int>(std::floor(g.x - rs)));
    const int c1 = std::min(d.cols - 1, static_cast<int>(std::ceil(g.x + rs)));
    const int r0 = std::max(0, static_cast<int>(std::floor(g.y - rs)));
    const int r1 = std::min(d.rows - 1, static_cast<int>(std::ceil(g.y + rs)));

    for (int c = c0; c <= c1; ++c) {
        for (int r = r0; r <= r1; ++r) {
            const float dx = static_cast<float>(c) - g.x;
            const float dy = static_cast<float>(r) - g.y;
            const float t = std::sqrt(dx * dx + dy * dy) / rs;
            const float w = brushWeight(t, m_falloff);
            if (w <= 0.0f) continue;

            // 这个格子在 p0→p1 方向上的投影比例（钳到 0..1）
            // ⚠ 采样点的局部米坐标是 origin + index*mpp（origin = -size/2），
            //   直接写 c*mpp 会整体偏半个地形 —— 坡道插值比例全错，表现是
            //   "坡道斜率对但位置偏了 256 m"。
            const glm::vec2 p(hf.origin.x + static_cast<float>(c) * d.metersPerPixel,
                              hf.origin.y + static_cast<float>(r) * d.metersPerPixel);
            const float s = std::clamp(
                glm::dot(p - m_rampStart, seg) / segLen2, 0.0f, 1.0f);
            const float target = m_rampStartH + (m_rampEndH - m_rampStartH) * s;

            const std::size_t idx = sampleIndex(d.rows, c, r);
            const float k = std::clamp(m_strength * w * dt * 6.0f, 0.0f, 1.0f);
            m_asset.heights[idx] += (target - m_asset.heights[idx]) * k;
            m_asset.heights[idx] = std::clamp(m_asset.heights[idx], 0.0f, kMaxNorm);
        }
    }
    (void)scale;
    m_pendingSave = true;
}

// ============================================================
// 整块地形操作
// ============================================================
void LandscapeEditor::wholeSmooth() {
    if (!m_valid) return;
    const TerrainDesc& d = m_asset.desc;
    beginSnapshot(1);   // 只动高度
    std::vector<float> src = m_asset.heights;
    for (int c = 0; c < d.cols; ++c) {
        for (int r = 0; r < d.rows; ++r) {
            float sum = 0.0f;
            int n = 0;
            for (int dc = -1; dc <= 1; ++dc) {
                for (int dr = -1; dr <= 1; ++dr) {
                    const int cc = std::clamp(c + dc, 0, d.cols - 1);
                    const int rr = std::clamp(r + dr, 0, d.rows - 1);
                    sum += src[sampleIndex(d.rows, cc, rr)];
                    ++n;
                }
            }
            m_asset.heights[sampleIndex(d.rows, c, r)] = sum / static_cast<float>(n);
        }
    }
    commitSnapshot();
    saveAssets(false);
    VK_LOG_INFO("LS-WHOLE action=smooth");
}

void LandscapeEditor::wholeNoise() {
    if (!m_valid) return;
    const TerrainDesc& d = m_asset.desc;
    beginSnapshot(1);   // 全图加噪只动高度
    const float scale = std::max(m_noiseScale, 0.5f);
    for (int c = 0; c < d.cols; ++c) {
        for (int r = 0; r < d.rows; ++r) {
            const float wx = static_cast<float>(c) * d.metersPerPixel / scale;
            const float wy = static_cast<float>(r) * d.metersPerPixel / scale;
            const float n = fbm(wx, wy, m_noiseOctaves, m_noiseSeed) - 0.5f;
            float& h = m_asset.heights[sampleIndex(d.rows, c, r)];
            h = std::clamp(h + n * m_wholeNoiseAmp * 2.0f, 0.0f, kMaxNorm);
        }
    }
    commitSnapshot();
    saveAssets(false);
    VK_LOG_INFO("LS-WHOLE action=noise amp=%.3f", 
                static_cast<double>(m_wholeNoiseAmp));
}

void LandscapeEditor::wholeReset() {
    if (!m_valid) return;
    const TerrainDesc& d = m_asset.desc;
    // 1|2 = 高度 + 权重（重置会同时改这两张）。挖洞遮罩不动：用户不会
    // 指望"重置形状"把他挖的洞也一起填上（洞是地貌之上的另一个决定）。
    beginSnapshot(1 | 2);
    m_asset.heights = generateDefaultHeights(d.cols, d.rows);
    m_asset.splat = generateDefaultSplat(m_asset.heights, d.cols, d.rows,
                                         d.metersPerPixel, d.maxHeight);
    commitSnapshot();
    saveAssets(true);
    VK_LOG_INFO("LS-WHOLE action=reset");
    m_ctx.notify(TR("Terrain reset to the default shape"));
}

// ============================================================
// 快捷键
// ============================================================
bool LandscapeEditor::handleShortcuts() {
    if (!m_ctx.isEditing()) return false;
    const ImGuiIO& io = ImGui::GetIO();

    // Ctrl+Z / Ctrl+Shift+Z（以及 Ctrl+Y）在刷地形时必须是**笔划**撤销，
    // 不能落到整场景快照上 —— 后者会把相机、选中项、所有实体一起弹回去。
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        if (io.KeyShift) redoStroke();
        else undoStroke();
        return true;
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
        redoStroke();
        return true;
    }
    // ---- Ctrl+. 回到"正常视角"（2026-10-24）----
    // 用户原话："……此时再次把视野调整到正常的高度和视角，并且对着角色的位置。"
    // 与进入地形模式同一条取景，随时可以把飞远/飞歪的镜头拉回来。
    // ⚠ 必须放在下面那条 `if (io.KeyCtrl || io.KeyAlt) return false;` **之前**，
    //   否则 Ctrl 一按下就直接短路，这个组合键永远收不到。
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Period, false)) {
        focusCameraAtPlayer();
        return true;
    }
    if (io.KeyCtrl || io.KeyAlt) return false;

    // `[` / `]` 调半径、`,` / `.` 调强度 —— 与 UE 一致（那里是 [ ] 调刷子大小）。
    //
    // ⚠ 这几个都是 OEM 键：中文键盘布局下 GLFW 可能把它们翻译成
    //   GLFW_KEY_UNKNOWN，ImGui 收不到（实测按 `]` 连事件都没有，而 Ctrl+Z
    //   是好的）。所以它们只是**补充**，主入口是视口里的滚轮
    //   （见 viewportInput 里的 MouseWheel 分支）—— 滚轮在什么布局下都到得了。
    const float oldRadius = m_radius;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, true))
        m_radius = std::max(1.0f, m_radius * 0.88f);
    if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, true))
        m_radius = std::min(500.0f, m_radius * 1.14f);
    if (m_radius != oldRadius) {
        VK_LOG_INFO("LS-BRUSH radius=%.2f", static_cast<double>(m_radius));
    }
    // ⚠ 上限必须和滑条 / 滚轮那一支保持一致（`kMaxStrength` / 500 m）。
    //   上一版这里还停在 1.0 与 200 m —— 于是"键盘调到 1.0 就上不去了"，
    //   用户按十几次发现没变化，看起来像键盘调强度没实现。
    const float oldStrength = m_strength;
    if (ImGui::IsKeyPressed(ImGuiKey_Comma, true))
        m_strength = std::max(0.01f, m_strength * 0.90f);
    if (ImGui::IsKeyPressed(ImGuiKey_Period, true))
        m_strength = std::min(kMaxStrength, m_strength * 1.10f);
    if (m_strength != oldStrength) {
        VK_LOG_INFO("LS-BRUSH strength=%.2f", static_cast<double>(m_strength));
    }
    return false;
}

// ============================================================
// 视口输入
// ============================================================
bool LandscapeEditor::viewportInput(const glm::vec2& vpPos,
                                    const glm::vec2& vpSize, bool hovered,
                                    ImDrawList* dl) {
    sync();
    if (!m_valid) return false;

    const ImGuiIO& io = ImGui::GetIO();
    const glm::vec2 mouse(io.MousePos.x, io.MousePos.y);
    const Ray ray =
        PickingSystem::rayFromViewport(m_ctx.editorScene().camera(), vpPos,
                                       vpSize, mouse);
    glm::vec3 hit(0.0f), nrm(0.0f), localHit(0.0f);
    const bool onTerrain =
        (hovered || m_stroking) &&
        raycast(ray.origin, ray.dir, hit, nrm, &localHit);

    // ---- 笔刷光标：地表上的圆环 + 衰减内圈 + 填充 ----
    // 用折线在世界里取点再投影到屏幕，而不是画一个屏幕空间的圆：
    // 俯视/侧视时地面上的圆在屏幕上是椭圆，画圆会明显错位。
    //
    // ⚠ 2026-10-23：整圈取**同一个高度**（不再逐点采地表高度）。
    //   以前每个环点都写 `wp.z = heightAtWorld(x,y)`，圆环"贴着地表"——
    //   一旦地表被笔刷推起来，环点就爬上自己刚堆出的山包，投影之后椭圆
    //   被拉歪、压扁、出现凹口（用户报的"笔刷的圆圈明显在变形"）。
    //   三条理由支持改成水平面：
    //     ① 笔刷作用区本来就是**水平**圆盘（applyStroke 按 XY 半径筛格点），
    //        圆环与它同面才是一致的；
    //     ② 水平圆的透视投影严格是圆锥曲线（椭圆）—— 无论地形怎么变，
    //        屏幕上的形状只由机位决定，不会随笔画自己抖；
    //     ③ 圆环是画在视口图像**之上**的 2D 覆盖层（无深度测试），贴着
    //        "地表"取点带来的遮挡关系一点也体现不出来，纯亏。
    //   代价：陡坡上圆环会插进/浮出地表 —— UE / Unity 的地形笔刷也是这么
    //   画的（一个平的圆盘），这是行业惯例，不是妥协。
    if (onTerrain && dl) {
        const glm::mat4 vp = PickingSystem::viewProj(
            m_ctx.editorScene().camera(), vpSize);
        const glm::mat4 model = modelMatrix();

        // 三种工具的配色：改高度=暖橙，改权重=层色，破坏性（降低/侵蚀）=冷色
        ImU32 col = IM_COL32(255, 190, 90, 230);
        ImU32 fill = IM_COL32(255, 190, 90, 34);
        switch (m_tool) {
            case Tool::Raise:
                col = io.KeyShift ? IM_COL32(110, 190, 255, 235)
                                  : IM_COL32(255, 190, 90, 235);
                fill = io.KeyShift ? IM_COL32(110, 190, 255, 30)
                                   : IM_COL32(255, 190, 90, 30);
                break;
            case Tool::Smooth:
                col = IM_COL32(160, 235, 220, 235);
                fill = IM_COL32(160, 235, 220, 26);
                break;
            case Tool::Flatten:
                col = IM_COL32(190, 230, 120, 235);
                fill = IM_COL32(190, 230, 120, 26);
                break;
            case Tool::Noise:
                col = IM_COL32(200, 160, 255, 235);
                fill = IM_COL32(200, 160, 255, 26);
                break;
            case Tool::Erode:
                col = IM_COL32(215, 160, 110, 235);
                fill = IM_COL32(215, 160, 110, 26);
                break;
            case Tool::PaintLayer:
                col = IM_COL32(255, 140, 220, 235);
                fill = IM_COL32(255, 140, 220, 26);
                break;
            case Tool::Carve:
                // 形状雕刻：压出凹腔用暖红，Shift 反向隆起用绿
                col = io.KeyShift ? IM_COL32(120, 230, 140, 235)
                                  : IM_COL32(255, 120, 90, 235);
                fill = io.KeyShift ? IM_COL32(120, 230, 140, 26)
                                   : IM_COL32(255, 90, 90, 26);
                break;
            case Tool::Ramp:
                col = IM_COL32(120, 200, 255, 235);
                fill = IM_COL32(120, 200, 255, 26);
                break;
        }
        if (!m_ctx.isEditing()) {
            col = IM_COL32(150, 150, 150, 160);
            fill = IM_COL32(150, 150, 150, 18);
        }

        // 局部点 → **裁剪空间**。刻意不直接返回屏幕坐标：屏幕坐标里看不出
        // "这个点其实在相机背后"（w<=0 时除法会把点翻到画面的另一侧）。
        const auto toClip = [&](const glm::vec3& lp) {
            return vp * (model * glm::vec4(lp, 1.0f));
        };

        // 近平面。裁剪空间的 w = 该点在视线方向上的深度，w 越小离相机越近；
        // w <= kNearW 一律当成"看不见"（贴到镜头上/在背后，屏幕坐标没有意义）。
        constexpr float kNearW = 0.05f;

        const auto clipToScreen = [&](const glm::vec4& c, ImVec2* out) {
            const float w = (c.w > kNearW) ? c.w : kNearW;
            out->x = vpPos.x + (c.x / w * 0.5f + 0.5f) * vpSize.x;
            out->y = vpPos.y + (c.y / w * 0.5f + 0.5f) * vpSize.y;
        };

        // 两条视线段的连线与近平面的交点（按 w 线性插值 —— 投影变换对 w
        // 是线性的，所以插出来的点**真的**落在近平面上）。
        const auto toNear = [&](const glm::vec4& a, const glm::vec4& b) {
            const float d = b.w - a.w;
            const float t = (d > 1.0e-6f || d < -1.0e-6f)
                                ? (kNearW - a.w) / d
                                : 0.0f;
            glm::vec4 x = a + (b - a) * t;
            x.w = kNearW;
            return x;
        };

        // 段数 64：半径 12 m 贴脸看时 48 段在屏幕上还能看出折角，64 段不会。
        constexpr int kSeg = 64;

        // 画一个水平圆盘（radiusScale = 半径倍率）。
        //
        // ⚠ 2026-10-24（第六轮）：**必须按近平面把圆裁成若干条可见弧段**，
        //   不能"有一个点在相机背后就整圈不画"。
        //   半径一旦超过"相机到圆心的水平距离"，圆的最远点必然跑到相机背后
        //   —— 工作取景（退 18 m / 抬 9 m，见 focusCameraAtPlayer）下半径
        //   只要超过 ~23 m 就会触发。老代码在这种情况下 `return false`，
        //   于是用户把半径调大（或把地形垫到相机高度以上）时**圆圈突然消失**。
        //   现在跨近平面的那一段插值出交点、圆被切成多条弧段，段与段之间
        //   不连（连起来会拉出一条横贯全屏的假边）。
        //
        // 返回值 = 整圈都在相机前方（只有这时填充才是凸多边形，能安全填充）。
        // behindOut = 有多少个环点落在相机背后（诊断 / 验收用：这个数 > 0 而
        // 圆环还在画，就证明近平面裁剪这条路径是通的）。
        int arcsDrawn = 0;
        int ringBehind = 0;
        const auto drawRing = [&](float radiusScale, ImU32 c, float thickness,
                                  ImU32 fillCol, int* behindOut) -> bool {
            const float r = m_radius * radiusScale;
            ImVec2 pts[kSeg + 2];
            int n = 0;
            bool allFront = true;
            glm::vec4 prevC(0.0f);
            bool prevFront = false;

            // 结束当前弧段（段内点数 < 2 时直接丢掉：一个点画不出线）
            const auto closeArc = [&]() {
                if (n >= 2) {
                    dl->AddPolyline(pts, n, c, 0, thickness);
                    ++arcsDrawn;
                }
                n = 0;
            };
            const auto push = [&](const glm::vec4& cc) {
                if (n < kSeg + 2) {
                    clipToScreen(cc, &pts[n]);
                    ++n;
                }
            };

            for (int i = 0; i <= kSeg; ++i) {
                const float a = 6.2831853072f * static_cast<float>(i) /
                                static_cast<float>(kSeg);
                const glm::vec4 c0 =
                    toClip(glm::vec3(localHit.x + std::cos(a) * r,
                                     localHit.y + std::sin(a) * r,
                                     localHit.z));
                const bool front = (c0.w > kNearW);
                if (!front) {
                    allFront = false;
                    if (behindOut) ++*behindOut;
                }
                if (i == 0) {
                    if (front) push(c0);
                } else if (front && prevFront) {
                    push(c0);
                } else if (front && !prevFront) {
                    // 从相机背后绕回可见：先落到近平面交点，再进这段弧
                    closeArc();
                    push(toNear(prevC, c0));
                    push(c0);
                } else if (!front && prevFront) {
                    // 从可见折向背后：以近平面交点收尾
                    push(toNear(prevC, c0));
                    closeArc();
                }
                prevC = c0;
                prevFront = front;
            }

            // 收尾最后一段。整圈没断过（n == kSeg+1）时才填充：
            // 水平圆的投影是凸的 → 用 Convex 版本（更快，也不会因为凹多边形
            // 三角化把填充画成一团乱麻）。
            if (n >= 2) {
                if (fillCol != 0 && allFront && n >= kSeg + 1) {
                    dl->AddConvexPolyFilled(pts, kSeg, fillCol);
                }
                dl->AddPolyline(pts, n, c, 0, thickness);
                ++arcsDrawn;
                n = 0;
            }
            return allFront;
        };

        // 外圈（带半透明填充：一眼看出"覆盖多大一片"，比单线好读得多）
        drawRing(1.0f, col, 1.8f, fill, &ringBehind);

        // 衰减内圈：满强度的核心范围。falloff 越大核心越小。
        if (m_falloff > 0.02f) {
            const float innerScale = 1.0f - 0.55f * m_falloff;
            if (innerScale > 0.12f) {
                drawRing(innerScale, (col & 0x00FFFFFFu) | 0x80000000u, 1.0f, 0,
                         nullptr);
            }
        }

        // 笔心：小十字 + 点。
        // ⚠ 以前这里取 `outer[0]` —— 那是圆环**角度 0 的那个点**（圆的最
        //   右侧），于是十字画在圆环边缘上，看着像"圆心跑偏了"。
        //   笔心必须投影命中点本身。
        const glm::vec4 centerC = toClip(localHit);
        if (centerC.w > kNearW) {
            ImVec2 c0;
            clipToScreen(centerC, &c0);
            dl->AddCircleFilled(c0, 2.5f, col);
            dl->AddLine(ImVec2(c0.x - 6.0f, c0.y), ImVec2(c0.x + 6.0f, c0.y),
                        col, 1.0f);
            dl->AddLine(ImVec2(c0.x, c0.y - 6.0f), ImVec2(c0.x, c0.y + 6.0f),
                        col, 1.0f);
        }

        // 诊断锚点：圆环被近平面切成几段、有多少点在相机背后。
        // 只在"段数 / 背后点数变了、半径跨过 1 m"时打一行 —— 供
        // `verify_brush_ring` 断言"调大半径之后圆环还在画"（老代码在这种
        // 情况下会整圈 `return false`，屏幕上什么都没有）。
        if (arcsDrawn != m_ringArcsLogged ||
            std::abs(m_radius - m_ringRadiusLogged) > 1.0f) {
            m_ringArcsLogged = arcsDrawn;
            m_ringRadiusLogged = m_radius;
            VK_LOG_INFO("LS-RING r=%.1f arcs=%d behind=%d",
                        static_cast<double>(m_radius), arcsDrawn, ringBehind);
        }
    }

    // ---- 拖拽：左键按下开始一笔，松开结算 ----
    const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    const bool navGesture = ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
                            ImGui::IsMouseDown(ImGuiMouseButton_Middle);

    // ---- 滚轮：调笔刷半径（Shift = 强度）----
    // 为什么不靠 `[` / `]` 做主入口：这两个是 OEM 键，在某些键盘布局下
    // GLFW 直接给出 KEY_UNKNOWN，ImGui 收不到 —— 功能就"看起来没实现"。
    // 滚轮走的是 io.MouseWheel，任何布局、任何输入法下都到得了。
    bool wheelConsumed = false;
    if (onTerrain && hovered && !m_stroking && !navGesture &&
        !io.KeyCtrl && io.MouseWheel != 0.0f) {
        const float n = io.MouseWheel;
        if (io.KeyShift) {
            m_strength = std::clamp(m_strength * std::pow(1.10f, n), 0.01f,
                                    kMaxStrength);
            VK_LOG_INFO("LS-BRUSH strength=%.2f", static_cast<double>(m_strength));
        } else {
            m_radius = std::clamp(m_radius * std::pow(1.12f, n), 1.0f, 500.0f);
            VK_LOG_INFO("LS-BRUSH radius=%.2f", static_cast<double>(m_radius));
        }
        wheelConsumed = true;
    }

    if (clicked && hovered && onTerrain && !navGesture) {
        m_stroking = true;
        m_strokeCount = 1;
        m_lastLocal = localHit;
        // ⚠️ 单位约定（这三个工具以前全错在这里）：
        //   `localHit` 是 `physics::raycast` 的返回值，而 renderer 里给它的
        //   `hf.heightScale` 是 `maxHeight * m_heightScale`，`Heightfield::
        //   height()` 里已经乘过了 —— 所以 **`localHit.z` 就是米，不用再乘
        //   任何东西**。以前这里又乘了一次 unitM，于是雕刻形状体被放到
        //   807 米高空（地形才 15 米高），压印自然什么也碰不到。
        // 反过来说，凡是和 `m_asset.heights[]` 直接打交道的量都是**归一化**，
        // 要经过 unitM 换算。所以下面三处一个是一路（米）、一个是另一路。
        const float unitM =
            std::max(m_asset.desc.maxHeight * m_heightScale, 1.0e-4f);

        // 形状雕刻：形状体的中心在**这一笔开始**时定死（单位米）。
        // 之后整笔都用它 —— 拖到哪儿就削到哪儿，深度不再累积（否则每帧
        // 都会把地表再推一截，形成自激循环，见 m_carveCenterM 的注释）。
        if (m_tool == Tool::Carve) {
            const float R = std::max(m_radius,
                                     m_asset.desc.metersPerPixel * 1.5f);
            m_carveCenterM = localHit.z - m_carveDepth * R;
        }
        // 压平的参考高度：按下那一刻笔心的**世界**高度（米）。
        // `applyStroke` 里会自己除以 unitM 换成归一化。
        m_flattenHeight = localHit.z;
        // 坡道的起点：`applyRampStroke` 直接拿它去和 `heights[idx]`（归一化）
        // 插值，所以这里要**换成归一化**，不能给米。
        m_rampStart = glm::vec2(localHit.x, localHit.y);
        m_rampStartH = localHit.z / unitM;
        m_rampEnd = m_rampStart;
        m_rampEndH = m_rampStartH;
        // 快照：位掩码 1=高度 2=权重 4=挖洞。
        // ⚠ 只存这一笔真的会改的那几路 —— 全存的话每笔要拷 ~600 KB，
        //   拖笔时每 60 ms 一次，纯属浪费；而且挖洞笔画的撤销记录里
        //   会混进一份没用的高度，undo 时把地形"顺手还原"了别的东西。
        int parts = 1;
        if (m_tool == Tool::PaintLayer) parts = 2;
        // Carve 只改高度图（不碰 holes 遮罩）→ 和 Raise 一样 parts = 1
        beginSnapshot(parts);
        // 按下立刻落一次 —— 单击（不拖）也必须有反应，否则用户会以为
        // "点一下没反应"，然后开始怀疑笔刷坏了。
        const float dt = std::min(io.DeltaTime, 0.05f);
        const bool invert = io.KeyShift;
        if (m_tool == Tool::PaintLayer) applyLayerStroke(localHit, dt, invert);
        else if (m_tool == Tool::Noise) applyNoiseStroke(localHit, dt);
        else if (m_tool == Tool::Erode) applyErodeStroke(localHit, dt);
        else if (m_tool == Tool::Carve) applyCarveStroke(localHit, dt, invert);
        else if (m_tool == Tool::Ramp) applyRampStroke(localHit, dt);
        else applyStroke(localHit, dt, invert);
        m_pendingSave = true;
        if (m_tool == Tool::Carve) {
            // 雕刻的"形状 + 下沉深度 + 落点"单独打一条：验收脚本要确认
            // 真的走的是形状压印（而不是又退回到改遮罩开洞）。
            VK_LOG_INFO("LS-CARVE shape=%d depth=%.2f size=%.1f centerZ=%.1f "
                        "at=(%.1f,%.1f) mode=%s",
                        static_cast<int>(m_carveShape),
                        static_cast<double>(m_carveDepth),
                        static_cast<double>(m_radius),
                        static_cast<double>(m_carveCenterM),
                        static_cast<double>(hit.x), static_cast<double>(hit.y),
                        invert ? "mound" : "carve");
        }
        VK_LOG_INFO("LS-STROKE-BEGIN tool=%d at=(%.1f,%.1f) radius=%.1f "
                    "strength=%.2f falloff=%.2f",
                    static_cast<int>(m_tool),
                    static_cast<double>(hit.x), static_cast<double>(hit.y),
                    static_cast<double>(m_radius),
                    static_cast<double>(m_strength),
                    static_cast<double>(m_falloff));
    }

    bool consumed = false;
    if (m_stroking) {
        consumed = true;   // 拖拽期间左键归笔刷，视口不该点选
        if (released) {
            m_stroking = false;
            commitSnapshot();
            // 抬笔：高度必写；权重只在画层时写；holes 已经不再产生
            saveAssets(m_tool == Tool::PaintLayer);
            VK_LOG_INFO("LS-STROKE-END strokes=%d undo=%d", m_strokeCount,
                        static_cast<int>(m_undo.size()));
        } else if (onTerrain && down && !navGesture) {
            const float dt = std::min(io.DeltaTime, 0.05f);
            const bool invert = io.KeyShift;

            // ---- 笔划插值 ----
            // 60 Hz 下鼠标一帧能移动几十像素，对应地面上好几米。只在"当前
            // 落点"刷一次的话，快速拖动画出来的是一串断开的圆点（半径 12 m
            // 时尤其明显）。沿上一落点 → 当前落点按半径的 1/4 步进插值，
            // 时间片平均分配，总位移量不变。
            const float step = std::max(m_radius * 0.25f,
                                        m_asset.desc.metersPerPixel * 1.5f);
            const glm::vec2 a(m_lastLocal.x, m_lastLocal.y);
            const glm::vec2 b(localHit.x, localHit.y);
            const float dist = glm::length(b - a);
            int steps = 1;
            if (dist > step) {
                steps = std::min(16, static_cast<int>(std::ceil(dist / step)));
            }
            const float sub = dt / static_cast<float>(steps);

            for (int i = 1; i <= steps; ++i) {
                const float f = static_cast<float>(i) / static_cast<float>(steps);
                glm::vec3 p = glm::mix(m_lastLocal, localHit, f);
                if (m_tool == Tool::PaintLayer) applyLayerStroke(p, sub, invert);
                else if (m_tool == Tool::Noise) applyNoiseStroke(p, sub);
                else if (m_tool == Tool::Erode) applyErodeStroke(p, sub);
                else if (m_tool == Tool::Carve) applyCarveStroke(p, sub, invert);
                else if (m_tool == Tool::Ramp) applyRampStroke(p, sub);
                else applyStroke(p, sub, invert);
            }
            m_lastLocal = localHit;

            m_saveTimer += dt;
            if (m_pendingSave && m_saveTimer >= kSaveInterval) {
                // 2026-10-21：拖拽期间**不落盘**，只把内存推给渲染层。
                // 以前这里是 saveAssets(...)：每 80 ms 写 264 KB~1 MB，
                // 然后 TerrainPass 再把它读回来 —— 磁盘往返就在这一帧里，
                // 于是"手已经拖过去几十像素，画面还停在原处"。
                publishToRenderer(m_tool == Tool::PaintLayer);
                m_pendingSave = false;
                m_saveTimer = 0.0f;
            }
            ++m_strokeCount;
        }
    }

    if (consumed && !m_stroking && m_pendingSave) saveAssets(true);
    return consumed || wheelConsumed || (onTerrain && hovered);
}

// ============================================================
// 面板
// ============================================================
void LandscapeEditor::drawPanel() {
    bool& open = panelOpenRef("landscape");
    if (!open) return;

    sync();

    // 2026-10-21：地形面板长在**左栏下半**（模式栏的当前模式参数区）。
    // 左栏被拖到折叠时退回浮动窗口，见 ModeRail.h 的 beginModeRailPanel。
    ImGuiWindowFlags wf = 0;
    const bool railed = beginModeRailPanel(m_ctx, wf);
    if (!railed) {
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowSizeConstraints(ImVec2(400.0f, 420.0f),
                                            ImVec2(FLT_MAX, FLT_MAX));
        ImGui::SetNextWindowSize(ImVec2(440.0f, 700.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.30f,
                                       io.DisplaySize.y * 0.10f),
                                ImGuiCond_FirstUseEver);
    }
    // ⚠ ID 必须是 `###panel_<面板表里的 id>` —— 与 EditorApp::drawPlaceholderPanel
    //   同一套命名（底栏托盘按这个名字找窗口）。
    if (!ImGui::Begin(TR("Landscape###panel_landscape"),
                      railed ? nullptr : &open, wf)) {
        ImGui::End();
        return;
    }
    logRect("LS-PANEL", ImGui::GetWindowPos(),
            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                   ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    ImGui::TextDisabled("%s", TR(editModeHint(EditMode::Landscape)));
    ImGui::Separator();

    if (!m_valid) {
        drawCreateSection();
    } else {
        drawStatsSection();
        ImGui::Separator();
        // 整块操作放在统计下面、笔刷上面：它和"统计"一样是地形级的东西，
        // 而笔刷是每次都要调的。放末尾会被挤到滚动区外 —— 用户找不到，
        // 自动化也点不到。
        drawWholeSection();
        ImGui::Separator();
        drawBrushSection();
    }

    ImGui::End();
}

void LandscapeEditor::drawCreateSection() {
    ImGui::SeparatorText(TR("Create a terrain"));
    ImGui::TextWrapped("%s",
                       TR("This scene has no terrain yet. A terrain is a "
                          "heightmap asset (height.r32 + splat.rgba8) plus one "
                          "entity in the scene."));

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##ls_name", m_newName, sizeof(m_newName),
                     ImGuiInputTextFlags_AutoSelectAll);
    logRect("LS-NAME", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    ImGui::SetNextItemWidth(-1.0f);
    int sizeIdx = 1;
    for (int i = 0; i < 3; ++i) {
        if (m_newCols == kSizeChoices[i]) sizeIdx = i;
    }
    if (ImGui::BeginCombo("##ls_size", kSizeLabels[sizeIdx])) {
        for (int i = 0; i < 3; ++i) {
            if (ImGui::Selectable(kSizeLabels[i], sizeIdx == i)) {
                m_newCols = kSizeChoices[i];
                m_newRows = kSizeChoices[i];
            }
        }
        ImGui::EndCombo();
    }
    logRect("LS-SIZE", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    labeledSlider(TR("Meters per sample"), "##ls_mpp", &m_newMpp, 0.5f, 8.0f,
                  "%.2f m");
    labeledSlider(TR("Max height"), "##ls_maxh", &m_newMaxHeight, 5.0f, 400.0f,
                  "%.0f m");

    ImGui::Spacing();
    const bool can = m_ctx.isEditing() && m_newName[0] != '\0';
    if (!can) ImGui::BeginDisabled();
    if (ImGui::Button(TR("Create terrain"), ImVec2(-1.0f, 30.0f))) {
        createWithCurrentSettings();
    }
    logRect("LS-CREATE-BTN", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (!can) ImGui::EndDisabled();
    if (!m_ctx.isEditing()) {
        ImGui::TextDisabled("%s", TR("Stop (Esc) before editing the terrain."));
    }
}

void LandscapeEditor::drawStatsSection() {
    const TerrainDesc& d = m_asset.desc;
    const glm::vec2 size = m_asset.worldSize();

    ImGui::SeparatorText(TR("Terrain"));
    // 2026-10-28：源文件路径与尺寸合成一行。
    // ⚠ 这不是"顺手挤一挤"：左栏参数面板只有 ~417px 高，而下面工具网格
    //   有 8 个按钮 —— 上面每多占一行（~20px），网格最后一行就往下掉 20px，
    //   掉出面板之后**有矩形却点不到**（旧注释记过这条坑）。少一行 = 网格
    //   多一行安全余量。
    ImGui::Text(TR("%s  ·  %d x %d samples  ·  %.0f x %.0f m"),
                m_source.c_str(), d.cols, d.rows,
                static_cast<double>(size.x), static_cast<double>(size.y));

    // 高度范围 + 平均：给"笔刷到底改没改到东西"一个数字证据
    float lo = 1.0f, hi = 0.0f;
    double sum = 0.0;
    for (float h : m_asset.heights) {
        lo = std::min(lo, h);
        hi = std::max(hi, h);
        sum += h;
    }
    const float scale = d.maxHeight * m_heightScale;
    const float mean =
        m_asset.heights.empty()
            ? 0.0f
            : static_cast<float>(sum / static_cast<double>(m_asset.heights.size()));
    ImGui::Text(TR("Height %.1f .. %.1f m  (mean %.1f · max %.0f m)"),
                static_cast<double>(lo * scale),
                static_cast<double>(hi * scale),
                static_cast<double>(mean * scale), static_cast<double>(scale));

    // 2026-10-28：三个按钮原来是一行 SameLine 硬排、"auto-saves while you
    // paint" 那句话再挤在同一行末尾 —— 窄栏里最后一个按钮直接被裁。
    // 现在按钮走响应式网格（够宽 3 列、窄了 2 列 + 换行），说明文字另起一行。
    const float avail = ImGui::GetContentRegionAvail().x;
    const int cols = panelGridColumns(avail, 110.0f, kBtnGap);
    const float bw = panelGridWidth(avail, cols, kBtnGap);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(kBtnGap, kBtnGap));
    if (panelGridButton(TR("Focus"), 0, cols, bw, kBtnRowH)) focusCamera();
    logRect("LS-FOCUS", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    // 2026-10-24：`Focus` 是"整片地形装进画面"（看全貌），这个是"回到角色身边"
    // （干活用）。两个都要留 —— 用户要的是平时贴着角色刷地形，偶尔才想看全貌。
    //
    // ⚠ 为什么快捷键之外还要一个按钮：`Ctrl+.` 在中文 Windows 上会被**微软拼音
    //   的"中英标点切换"**吃掉（实测真实按键到不了应用，同机单独按 `.` 却正常）。
    //   快捷键留着（用户点名要的），但必须有这条不经过键盘的路。
    if (panelGridButton(TR("Focus player"), 1, cols, bw, kBtnRowH))
        focusCameraAtPlayer();
    logRect("LS-FOCUS-PLAYER", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", TR("Camera back to the player at working "
                                   "height (Ctrl+.)"));
    }
    if (panelGridButton(TR("Save now"), 2, cols, bw, kBtnRowH)) saveAssets(true);
    logRect("LS-SAVE-NOW", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    // "auto-saves while you paint" 原来是按钮右边的一行 TextDisabled —— 单独
    // 占一行的说明文字在这里是奢侈品（见本函数开头关于 20px 的注释），
    // 改成按钮的 tooltip：想确认的时候悬停就有，不想看的时候不占地方。
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", TR("auto-saves while you paint"));
    }
    ImGui::PopStyleVar();
}

void LandscapeEditor::drawToolParams() {
    switch (m_tool) {
        case Tool::Raise:
            break;
        case Tool::Smooth:
            ImGui::TextDisabled("%s",
                                TR("Relaxes bumps. Lower strength = gentler "
                                   "slope, higher = melts detail away."));
            break;
        case Tool::Flatten:
            ImGui::TextDisabled("%s",
                                TR("Pulls the surface toward the height captured "
                                   "when you pressed the mouse button."));
            break;
        case Tool::Noise: {
            ImGui::SeparatorText(TR("Noise"));
            labeledSlider(TR("Feature size"), "##ls_noise_scale", &m_noiseScale,
                          1.0f, 120.0f, "%.1f m", true);
            labeledSliderInt(TR("Octaves"), "##ls_noise_oct", &m_noiseOctaves, 1,
                             6);
            ImGui::SetNextItemWidth(-1.0f);
            int seed = static_cast<int>(m_noiseSeed);
            ImGui::TextUnformatted(TR("Pattern seed"));
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::DragInt("##ls_noise_seed", &seed, 1.0f, 0, 99999)) {
                m_noiseSeed = static_cast<std::uint32_t>(std::max(seed, 0));
            }
            break;
        }
        case Tool::Erode: {
            ImGui::SeparatorText(TR("Erosion"));
            labeledSlider(TR("Angle of repose"), "##ls_erode_talus",
                          &m_erodeTalus, 10.0f, 70.0f, "%.0f deg");
            labeledSliderInt(TR("Iterations per frame"), "##ls_erode_iter",
                             &m_erodeIterations, 1, 6);
            ImGui::TextDisabled("%s",
                                TR("Material above the repose angle slides to "
                                   "lower neighbours. Repeats until the slope "
                                   "settles - this is what makes ridges read "
                                   "as rock instead of plastic."));
            break;
        }
        case Tool::PaintLayer: {
            ImGui::SeparatorText(TR("Layer"));
            // 四个层按钮。按钮上显示的是**该层当前真的挂着哪个材质**，
            // 不是写死的 "Grass/Dirt/Rock/Snow" —— 层材质可以由材质仓库
            // 随时换掉，写死的名字一旦换过就说错了。
            // 名称解析不出来时才退回 kLayerNames 的兜底。
            //
            // 2026-10-28：原来是 4 × 96px 硬排一行（合计 384 + 3 个间距），
            // 460 宽时刚好塞满、300 宽时后两个直接被裁。改成响应式网格。
            const float layAvail = ImGui::GetContentRegionAvail().x;
            const int layCols = panelGridColumns(layAvail, 104.0f, kBtnGap);
            const float layW = panelGridWidth(layAvail, layCols, kBtnGap);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                ImVec2(kBtnGap, kBtnGap));
            for (int i = 0; i < 4; ++i) {
                const bool on = (m_layer == i);
                const std::string id = layerMaterial(i);
                // 优先用仓库条目的显示名（短、且中英双语）；仓库里没有
                // （用户手写的 .vmat / .vterrain 里的老 id）才退回 id 末段。
                const assets::MaterialLibraryEntry* le =
                    m_ctx.materialLibrary().find(id);
                std::string label =
                    le ? std::string(trBilingual(le->label, le->labelZh))
                       : shortMaterialName(id);
                if (label.empty()) label = std::string(TR(kLayerNames[i]));
                label += "##lslayer";
                label += std::to_string(i);
                if (on) {
                    ImGui::PushStyleColor(
                        ImGuiCol_Button,
                        ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                }
                if (panelGridButton(label.c_str(), i, layCols, layW,
                                    kBtnRowH)) {
                    m_layer = i;
                    VK_LOG_INFO("LS-LAYER %d id=%s", i, id.c_str());
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", id.empty() ? "(unused)" : id.c_str());
                }
                if (on) ImGui::PopStyleColor();
            }
            ImGui::PopStyleVar();

            // ---------------------------------------------------- 材质仓库
            // 把"当前层用哪个材质"做成下拉。列表来自
            // assets/materials/library.json（材质仓库），所以往仓库里丢一个
            // .vmat 就能立刻拿来当笔刷用 —— 这正是这一轮的目的：
            // 地形不只是"抬高度"，还要能"贴纹理"。
            ImGui::SeparatorText(TR("Layer material"));
            const assets::MaterialLibrary& lib = m_ctx.materialLibrary();
            const std::string curId = layerMaterial(m_layer);
            const assets::MaterialLibraryEntry* cur = lib.find(curId);
            const char* preview =
                cur ? trBilingual(cur->label, cur->labelZh)
                    : (curId.empty() ? TR("(unused)") : curId.c_str());
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::BeginCombo("##ls_layer_mat", preview)) {
                if (lib.size() == 0) {
                    ImGui::TextDisabled("%s", TR("Material library is empty"));
                    ImGui::TextDisabled("%s",
                                        TR("Run tools/gen_material_library.py"));
                }
                for (const auto& e : lib.entries()) {
                    const bool sel = (e.id == curId);
                    // 有 albedo 的才能当"贴图笔刷"；没有的（纯数值材质）
                    // 也能选，只是刷出来是纯色 —— 标一下让人有预期。
                    std::string text = trBilingual(e.label, e.labelZh);
                    if (!e.hasAlbedo()) text += TR("  (flat)");
                    if (ImGui::Selectable(text.c_str(), sel)) {
                        if (setLayerMaterial(m_layer, e.id)) {
                            m_ctx.notify(std::string(TR("Layer material: ")) +
                                         trBilingual(e.label, e.labelZh));
                        }
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            TR("%s\nalbedo: %s\nnormal: %s"), e.id.c_str(),
                            e.hasAlbedo() ? e.albedoPath.c_str() : "-",
                            e.hasNormal() ? e.normalPath.c_str() : "-");
                    }
                }
                ImGui::EndCombo();
            }
            if (cur) {
                ImGui::TextDisabled("%s", cur->id.c_str());
                ImGui::TextDisabled(TR("albedo: %s"),
                                    cur->hasAlbedo() ? cur->albedoPath.c_str() : "-");
            }
            if (ImGui::Button(TR("Refresh library"), ImVec2(-1.0f, 0.0f))) {
                m_ctx.reloadMaterialLibrary();
            }
            // 擦除 = 画回第 0 层。勾选与 Shift 是同一个意思（两条路都留，
            // 因为有人习惯按住 Shift、有人习惯点勾选框）。
            ImGui::Checkbox(TR("Erase (paint back to grass)"), &m_eraseLayer);
            break;
        }
        case Tool::Carve: {
            ImGui::SeparatorText(TR("Carve"));
            // 形状体：球 / 方块 / 圆柱。尺寸复用上面的 Brush Size ——
            // 再给一个"尺寸"滑条只会让人犹豫两个滑条谁说话。
            const char* shapes[3] = {TR("Sphere"), TR("Box"), TR("Cylinder")};
            int si = static_cast<int>(m_carveShape);
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::Combo("##carve_shape", &si, shapes, 3)) {
                m_carveShape = static_cast<CarveShape>(si);
                VK_LOG_INFO("LS-CARVE shape=%d", si);
            }
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderFloat(TR("Sink depth"), &m_carveDepth, 0.0f,
                                   1.2f, "%.2f")) {
                VK_LOG_INFO("LS-CARVE depth=%.2f",
                            static_cast<double>(m_carveDepth));
            }
            // 现场算一下"最深能压多少米"，免得用户在 60 m 的地形上用 0.05
            // 的深度试半天看不出变化。
            ImGui::Text(TR("Depth: %.1f m  |  Size: %.1f m"),
                        static_cast<double>(m_carveDepth * m_radius),
                        static_cast<double>(m_radius));
            ImGui::TextDisabled("%s",
                                TR("Drag a shape through the surface: the "
                                   "terrain is carved down to the object's "
                                   "lower outline, so sweeping it digs a "
                                   "channel. Topology stays intact (no holes, "
                                   "no discarded pixels). Hold Shift to push a "
                                   "mound up with the same outline."));
            break;
        }
        case Tool::Ramp: {
            ImGui::SeparatorText(TR("Ramp"));
            ImGui::TextDisabled("%s",
                                TR("Press at the start of the slope and drag to "
                                   "the end: the brush blends the surface "
                                   "linearly between the two heights, giving a "
                                   "walkable grade instead of a cliff."));
            if (m_stroking) {
                ImGui::Text(TR("From %.1f m to %.1f m"),
                            static_cast<double>(m_rampStartH *
                                                m_asset.desc.maxHeight *
                                                m_heightScale),
                            static_cast<double>(m_rampEndH *
                                                m_asset.desc.maxHeight *
                                                m_heightScale));
            }
            break;
        }
    }
}

void LandscapeEditor::drawWholeSection() {
    // 整块操作。2026-10-28：原来"三个按钮挤一行（avail*0.32/0.30 + 振幅框）"，
    // 在 300~360 宽时每个按钮只剩 ~100px、振幅框只剩 ~80px —— 数字都显示不全。
    // 改成两行：先两个按钮，振幅框单独一行（带标签），最后再一个整宽按钮。
    ImGui::SeparatorText(TR("Whole terrain"));

    const float avail = ImGui::GetContentRegionAvail().x;
    const float halfW = panelGridWidth(avail, 2, kBtnGap);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(kBtnGap, kBtnGap));
    if (ImGui::Button(TR("Smooth all"), ImVec2(halfW, kBtnRowH)))
        wholeSmooth();
    logRect("LS-WHOLE-SMOOTH", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::Button(TR("Add noise"), ImVec2(halfW, kBtnRowH)))
        wholeNoise();
    logRect("LS-WHOLE-NOISE", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    ImGui::PopStyleVar();

    // ⚠ 不再单列一行 "Noise amplitude" 标题：拖动框自己的格式串就是
    //   `amp %.3f`，标签已经在里面了。省下 20px 留给工具网格（见 stats 段）。
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::DragFloat("##ls_whole_amp", &m_wholeNoiseAmp, 0.002f, 0.005f, 0.30f,
                     TR("amp %.3f"));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", TR("Noise amplitude"));
    }

    if (ImGui::Button(TR("Reset to default shape"), ImVec2(-1.0f, kBtnRowH)))
        wholeReset();
    logRect("LS-WHOLE-RESET", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
}

void LandscapeEditor::drawBrushSection() {
    // 工具按钮网格（8 个）。
    //
    // 2026-10-28：原来是硬编码 2 列 × 188px、行高 20、行距 2 —— 用户报
    // 「按钮太拥挤、过于紧凑」。三个数字都动了：
    //   · 行高 20 → 26（`kBtnRowH`），点得准，也是 UE 那里工具胶囊的高度；
    //   · 纵向间距 3 → 6（`kBtnGap`），横向 8 → 6（列宽按可用宽度均分，
    //     所以横向间距不再需要靠"留白"来凑）；
    //   · **列数按可用宽度算**：460 宽时是 3 列 3 行（高度 90，与原来 4 行
    //     88 几乎一样，不会把下面的控件顶出面板）；被拖到 300 宽时退成
    //     2 列 4 行，按钮仍然全在窗口里（硬编码 188 时右边那个会被裁掉）。
    //
    // ⚠ 网格**不设段标题**，直接顶在"整块操作"下面；`Brush` 这个标题挪到
    //   网格之后、滑条之前（读起来就是"笔刷参数"）。理由（2026-10-24 实测）：
    //   左栏下半在 1600×900 下只有 378 px（面板 446..842），统计 + 整块操作
    //   已经吃到 y≈734。再加 25 px 的 `SeparatorText("Brush")`，第 4 行
    //   （Paint layer / Carve）就整个掉到面板底边之外 —— 用户看得见、点不到，
    //   自动化更糟：中心落在面板**下方**，砸到底部通栏上把中间视口切成了
    //   Command Palette，之后所有视口操作全部落空（verify_landscape_ground
    //   的 B/C/D 三组就是这么整片 FAIL 的，症状是"按钮齐全、笔划全无"）。
    //   现在多了一件事托底：左栏模式列表可以折叠（表头上的箭头），折叠后
    //   参数面板直接多拿 ~310 px。
    const float avail = ImGui::GetContentRegionAvail().x;
    const int cols = panelGridColumns(avail, 132.0f, kBtnGap);
    const float btnW = panelGridWidth(avail, cols, kBtnGap);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(kBtnGap, kBtnGap));
    struct Item { Tool t; const char* label; };
    const Item tools[kToolCount] = {
        {Tool::Raise, "Raise / Lower"},
        {Tool::Smooth, "Smooth"},
        {Tool::Flatten, "Flatten"},
        {Tool::Ramp, "Ramp"},
        {Tool::Noise, "Noise"},
        {Tool::Erode, "Erode"},
        {Tool::PaintLayer, "Paint layer"},
        {Tool::Carve, "Carve"},
    };
    for (int i = 0; i < kToolCount; ++i) {
        const bool on = (m_tool == tools[i].t);
        if (on) {
            ImGui::PushStyleColor(
                ImGuiCol_Button,
                ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        if (panelGridButton(TR(tools[i].label), i, cols, btnW, kBtnRowH)) {
            m_tool = tools[i].t;
            // ⚠ 两个数字都要打出来，别再混用：`LS-TOOL-<i>` 这个**锚点**用的是
            //   按钮在网格里的视觉序号 i，而 `static_cast<int>(m_tool)` 是
            //   `Tool` 枚举值 —— 两者顺序不一样（枚举里 Noise/Erode/PaintLayer
            //   排在 Carve/Ramp 前面，按钮却把 Ramp 放第 4 格、Carve 放第 8 格）。
            //   只打枚举值时，"按第 8 格 → LS-TOOL 7"读起来像是点错了格。
            VK_LOG_INFO("LS-TOOL %d index=%d name=%s", static_cast<int>(m_tool),
                        i, tools[i].label);
        }
        if (on) ImGui::PopStyleColor();
        char tag[48];
        std::snprintf(tag, sizeof(tag), "LS-TOOL-%d", i);
        logRect(tag, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    }
    ImGui::PopStyleVar();   // ItemSpacing（见网格上方的说明）

    // "Brush" 段标题：网格选中之后就是笔刷参数（半径/强度/衰减/单笔上限）。
    ImGui::SeparatorText(TR("Brush"));
    labeledSlider(TR("Radius"), "##ls_radius", &m_radius, 1.0f, 500.0f, "%.1f m",
                  /*logScale=*/true);
    // 强度：对数滑条 + 上限 64（第十轮从 16 提上来）。UE 的 Tool Strength
    // 到 1.0 只是"标准档"，整地/整坡时 3~8 才好用；而 514 m 的整体地形上
    // 连 16 都得按好几秒 —— 上限再宽一点，用户才不会觉得"到头了"。
    labeledSlider(TR("Strength"), "##ls_strength", &m_strength, 0.01f,
                  kMaxStrength, "%.2f", /*logScale=*/true);
    labeledSlider(TR("Falloff"), "##ls_falloff", &m_falloff, 0.0f, 1.0f, "%.2f");
    labeledSlider(TR("Max displacement per stroke"), "##ls_maxdelta",
                  &m_maxDelta, 0.0f, 200.0f, "%.0f m");
    if (m_maxDelta < 1.0e-4f) {
        ImGui::TextDisabled("%s", TR("0 = unlimited"));
    }

    drawToolParams();

    // ---- 撤销 / 重做 ----
    // 2026-10-28：原来是写死 184 + 184（= 368 + 间距），左栏被拖到 300 时
    // 右边那个 Redo 直接出窗口。改成按可用宽度对半分。
    ImGui::Spacing();
    const bool canUndo = !m_undo.empty();
    const bool canRedo = !m_redo.empty();
    const float halfW = panelGridWidth(ImGui::GetContentRegionAvail().x, 2,
                                       kBtnGap);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(kBtnGap, kBtnGap));
    if (!canUndo) ImGui::BeginDisabled();
    if (ImGui::Button(TR("Undo stroke"), ImVec2(halfW, kBtnRowH))) undoStroke();
    logRect("LS-UNDO-BTN", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (!canUndo) ImGui::EndDisabled();
    ImGui::SameLine();
    if (!canRedo) ImGui::BeginDisabled();
    if (ImGui::Button(TR("Redo"), ImVec2(halfW, kBtnRowH))) redoStroke();
    logRect("LS-REDO-BTN", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (!canRedo) ImGui::EndDisabled();
    ImGui::PopStyleVar();
    ImGui::TextDisabled(TR("%d undo steps  ·  Ctrl+Z / Ctrl+Shift+Z"),
                        static_cast<int>(m_undo.size()));

    ImGui::Spacing();
    ImGui::TextDisabled("%s",
                        TR("Left-drag to paint. Shift inverts (lower / erase). "
                           "Right-drag moves the camera. Scroll = radius, "
                           "Shift+scroll = strength."));
    // 取景提示（2026-10-24）：进模式时镜头贴着角色，飞远了按这个回来。
    ImGui::TextDisabled("%s", TR("Ctrl+. snaps the view back to the player "
                                 "at a normal working height."));
    if (m_stroking) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "%s",
                           TR("painting..."));
    }
}

} // namespace editor
