#include "LandscapeEditor.h"

#include "DebugRects.h"
#include "Localization.h"
#include "PickingSystem.h"

#include "assets/AssetPath.h"
#include "assets/AssetManager.h"
#include "core/Logger.h"
#include "physics/Heightfield.h"
#include "scene/Scene.h"

#include <imgui.h>

#include <algorithm>
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

// 落盘节流：拖动时每 80 ms 写一次文件。
// 为什么要节流而不是每帧写：写盘会触发渲染层重传贴图（一次
// vkQueueWaitIdle），60 Hz 下写 257² 是纯浪费；80 ms 的延迟肉眼看不出来。
constexpr float kSaveInterval = 0.08f;

// 撤销栈上限。一笔扫过整片地形的脏矩形按 1 MB 估，24 笔 ~24 MB 封顶；
// 实际多数笔划只有几十 KB。
constexpr std::size_t kUndoDepth = 24;

// 新建地形的合法边长（(cols-1) 必须能被 kChunkCells=32 整除 —— 这是
// 分块网格的硬约束，见 terrain::parseDesc）。
constexpr int kSizeChoices[3] = {129, 257, 513};
const char* kSizeLabels[3] = {"129 x 129  (128 m)", "257 x 257  (512 m)",
                              "513 x 513  (1 km)"};

// 新建地形默认的 4 个层材质 —— 直接引用工程里已有的 .vmat
// （assets/materials/terrain_*.vmat）。写死名字是有意的：地形层是
// "草地/泥土/岩石/雪"这套通用语义，不是每个工程都该自己定一套。
const char* kLayerIds[4] = {"materials/terrain_grass", "materials/terrain_dirt",
                            "materials/terrain_rock", "materials/terrain_snow"};

const char* kLayerNames[4] = {"Grass", "Dirt", "Rock", "Snow"};

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

// 新建地形的默认高度：三层正弦叠出来的丘陵。
//
// ------------------------------------------------------------
// 为什么四边必须**压到 0**
// ------------------------------------------------------------
// 视口的地平面栅格画在 z = 0（Renderer 的 GridSettings::planeOffset），
// 而地形表面是 `height * maxHeight`。如果默认地貌整体离开 0 面（上一版
// 的高度区间是 0.082..0.830 → 4.9..49.8 m），那么地形块的四条边就是
// **4.9~49.8 m 的垂直悬崖**，整块地形看起来像"浮在栅格上方的一块厚板"，
// 而不是长在地面上的一片丘陵 —— 这就是"地形和栅格不在一个平面"。
//
// 所以这里乘一个**边界渐变**：距边界 12%（默认 512 m 地形约 61 m）以内
// 用 smoothstep 平滑降到 0。效果是地形四边与栅格共面，接缝处没有台阶；
// 中间该有的起伏一点没少。
//
// ⚠ 不能改成"整体减去最小值"：那样只有**最低点**贴地，其余边界仍然是
//   几米到几十米不等的落差（正弦的取值沿边界一直在变），四周会出现一圈
//   高度各不相同的墙 —— 看起来更像 bug。
//
// 为什么不是随机噪声：噪声第一眼像"坏的"，而且每次新建都长得不一样，
// 没法用截图做回归对比。三层不同频率的正弦叠加是**确定性**的：
//   0.46 基面 + 0.24 大丘（~48 m 宽）+ 0.13 中坡 + 0.04 细纹
std::vector<float> generateDefaultHeights(int cols, int rows) {
    // 边缘渐变宽度（占边长的比例）。0.12 → 512 m 地形约 61 m 的过渡带。
    // 太小（<0.05）在低多边形 LOD 下会看出折线；太大（>0.25）中心就没
    // 起伏了 —— 默认地貌会变成"一个大馒头"。
    constexpr float kEdgeFade = 0.12f;
    std::vector<float> heights(static_cast<std::size_t>(cols) * rows, 0.0f);
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
            heights[sampleIndex(rows, c, r)] = std::clamp(h * fade, 0.0f, 1.0f);
        }
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
            // 泥土：0.36 起、0.46 满，0.50 起淡出、0.56 消失（再往上交给雪），
            // 并且给岩石和雪让位
            float wDirt = std::clamp((h - 0.36f) / 0.10f, 0.0f, 1.0f) *
                          std::clamp((0.56f - h) / 0.06f, 0.0f, 1.0f) *
                          (1.0f - wRock) * (1.0f - wSnow);
            float wGrass = std::max(0.0f, 1.0f - wRock - wSnow - wDirt);

            // 四个通道加起来恒 = 1（见上面的叠放次序），不需要再归一化；
            // 保险起见仍按 sum 压一次，防浮点误差让 uint8 溢出。
            const float sum = wGrass + wDirt + wRock + wSnow;
            const float k = (sum > 1.0f) ? (1.0f / sum) : 1.0f;
            const std::size_t base = sampleIndex(rows, c, r) * 4u;
            splat[base + 0] = static_cast<std::uint8_t>(wGrass * k * 255.0f);
            splat[base + 1] = static_cast<std::uint8_t>(wDirt * k * 255.0f);
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
    m_entity = found;
    m_source = tc->source;
    m_heightScale = (hs > 1.0e-4f) ? hs : 1.0f;
    m_asset = std::move(a);
    m_valid = true;
    m_strokeCount = 0;
    // 换了资产 → 旧的撤销记录指向的像素已经不是同一张图了
    if (switched) {
        m_undo.clear();
        m_redo.clear();
        m_snapshotValid = false;
    }
    VK_LOG_INFO("LS-TERRAIN path=%s cols=%d rows=%d mpp=%.2f maxH=%.1f "
                "heightScale=%.2f",
                m_source.c_str(), m_asset.desc.cols, m_asset.desc.rows,
                static_cast<double>(m_asset.desc.metersPerPixel),
                static_cast<double>(m_asset.desc.maxHeight),
                static_cast<double>(m_heightScale));
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

    m_ctx.select(created);
    m_ctx.notify(std::string(TR("Terrain created: ")) + name);
    m_ctx.dirty() = true;
    m_valid = false;   // 强制 sync 重新绑定
    sync();            // 立刻绑定（本帧面板就会切到笔刷 UI）
    focusCamera();
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
void LandscapeEditor::saveAssets(bool splatToo, bool holesToo) {
    if (!m_valid) return;
    const std::string abs = assets::resolveAssetPath(m_source);
    const std::filesystem::path dir = std::filesystem::path(abs).parent_path();

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
    }
    m_ctx.dirty() = true;
    m_pendingSave = false;
    m_saveTimer = 0.0f;
}

void LandscapeEditor::flush() {
    if (m_valid && m_pendingSave) saveAssets(true);
    if (m_stroking) {
        m_stroking = false;
        commitSnapshot();
    }
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
                    // 抬高 / 降低是**同一个算子**的符号差异（Shift 反向）
                    h += sign * m_strength * w * dt * kRaiseRate;
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
            h = std::clamp(h, 0.0f, 1.0f);
            if (maxDeltaNorm > 0.0f) {
                const float s = m_snapshotHeights[idx];
                h = std::clamp(h, std::max(0.0f, s - maxDeltaNorm),
                               std::min(1.0f, s + maxDeltaNorm));
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
            h = std::clamp(h, 0.0f, 1.0f);
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
                std::clamp(v, 0.0f, 1.0f);
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
// 挖洞（UE 的 Landscape Visibility 层）
// ============================================================
// 挖的是**遮罩**不是高度：片元着色器按遮罩 discard，所以洞下面是"真的空"，
// 而不像"把高度压到 -100 m"那样留下一口能看见四周岩壁的深井。
//
// 遮罩是 per-sample 的 8 位灰度，笔刷按权重往 0（挖穿）或 255（补回）走。
// 为什么不 per-quad 二值化：那会让洞边全是锯齿（一格 2 m 的方块台阶），
// 而双线性采样的遮罩 + 0.5 阈值正好把边缘放在两格中间 —— 视觉上足够干净。
void LandscapeEditor::applyHoleStroke(const glm::vec3& localHit, float dt,
                                      bool invert) {
    const TerrainDesc& d = m_asset.desc;
    if (m_asset.holes.size() != m_asset.heights.size()) return;
    const physics::Heightfield hf = m_asset.heightfield();
    const glm::vec2 g = hf.gridOf(glm::vec2(localHit.x, localHit.y));
    const float rs = m_radius / d.metersPerPixel;
    if (rs < 1.0f) return;

    const int c0 = std::max(0, static_cast<int>(std::floor(g.x - rs)));
    const int c1 = std::min(d.cols - 1, static_cast<int>(std::ceil(g.x + rs)));
    const int r0 = std::max(0, static_cast<int>(std::floor(g.y - rs)));
    const int r1 = std::min(d.rows - 1, static_cast<int>(std::ceil(g.y + rs)));

    // 目标值：Shift = 255（补回），默认 0（挖穿）。
    const float target = invert ? 255.0f : 0.0f;
    for (int c = c0; c <= c1; ++c) {
        for (int r = r0; r <= r1; ++r) {
            const float dx = static_cast<float>(c) - g.x;
            const float dy = static_cast<float>(r) - g.y;
            const float t = std::sqrt(dx * dx + dy * dy) / rs;
            const float w = brushWeight(t, m_falloff);
            if (w <= 0.0f) continue;
            // 速率先到 1.0 再乘 —— 力度很大时不该"一帧挖穿"，那样拖出来的
            // 洞边会有毛刺（一帧的落点间隔比笔刷半径还大）。
            const float k = std::clamp(m_strength * w * dt * 5.0f, 0.0f, 1.0f);
            const std::size_t idx = sampleIndex(d.rows, c, r);
            float v = static_cast<float>(m_asset.holes[idx]);
            v += (target - v) * k;
            m_asset.holes[idx] =
                static_cast<std::uint8_t>(std::clamp(v, 0.0f, 255.0f));
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
            m_asset.heights[idx] = std::clamp(m_asset.heights[idx], 0.0f, 1.0f);
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
            h = std::clamp(h + n * m_wholeNoiseAmp * 2.0f, 0.0f, 1.0f);
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

    // ---- 笔刷光标：贴着地表的圆环 + 衰减内圈 + 填充 ----
    // 用 32 段折线在地表上取点再投影到屏幕，而不是画一个屏幕空间的圆：
    // 俯视/侧视时地面上的圆在屏幕上是椭圆，画圆会明显错位。
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
            case Tool::Hole:
                // 挖洞是破坏性的 → 红色（Shift 补回是绿色）
                col = io.KeyShift ? IM_COL32(120, 230, 140, 235)
                                  : IM_COL32(255, 90, 90, 235);
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

        // 把地表上的一个圆投影成屏幕折线。rings = 要画几圈（半径倍率）。
        auto projectRing = [&](float radiusScale, ImVec2* pts) -> bool {
            for (int i = 0; i <= 32; ++i) {
                const float a = 6.2831853072f * static_cast<float>(i) / 32.0f;
                const float r = m_radius * radiusScale;
                const glm::vec3 lp(localHit.x + std::cos(a) * r,
                                   localHit.y + std::sin(a) * r, 0.0f);
                const glm::vec3 wp0 = glm::vec3(model * glm::vec4(lp, 1.0f));
                glm::vec3 wp = wp0;
                wp.z = heightAtWorld(wp0.x, wp0.y);
                const glm::vec4 clip = vp * glm::vec4(wp, 1.0f);
                if (clip.w <= 1.0e-5f) return false;
                const glm::vec3 ndc = glm::vec3(clip) / clip.w;
                pts[i] = ImVec2(vpPos.x + (ndc.x * 0.5f + 0.5f) * vpSize.x,
                                vpPos.y + (ndc.y * 0.5f + 0.5f) * vpSize.y);
            }
            return true;
        };

        ImVec2 outer[33];
        if (projectRing(1.0f, outer)) {
            // 半透明填充：一眼看出"覆盖多大一片"，比单线好读得多
            dl->AddConcavePolyFilled(outer, 32, fill);
            dl->AddPolyline(outer, 33, col, 0, 1.8f);

            // 衰减内圈：满强度的核心范围。falloff 越大核心越小。
            if (m_falloff > 0.02f) {
                ImVec2 inner[33];
                const float innerScale = 1.0f - 0.55f * m_falloff;
                if (innerScale > 0.12f && projectRing(innerScale, inner)) {
                    dl->AddPolyline(inner, 33, (col & 0x00FFFFFFu) | 0x80000000u,
                                    0, 1.0f);
                }
            }

            // 笔心：小十字 + 点
            const ImVec2 c0 = outer[0];
            dl->AddCircleFilled(c0, 2.5f, col);
            dl->AddLine(ImVec2(c0.x - 6.0f, c0.y), ImVec2(c0.x + 6.0f, c0.y), col, 1.0f);
            dl->AddLine(ImVec2(c0.x, c0.y - 6.0f), ImVec2(c0.x, c0.y + 6.0f), col, 1.0f);
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
        // 压平的参考高度：按下那一刻笔心的**世界**高度
        m_flattenHeight = localHit.z * m_asset.desc.maxHeight * m_heightScale;
        // 坡道的起点
        m_rampStart = glm::vec2(localHit.x, localHit.y);
        m_rampStartH = localHit.z;
        m_rampEnd = m_rampStart;
        m_rampEndH = m_rampStartH;
        // 快照：位掩码 1=高度 2=权重 4=挖洞。
        // ⚠ 只存这一笔真的会改的那几路 —— 全存的话每笔要拷 ~600 KB，
        //   拖笔时每 60 ms 一次，纯属浪费；而且挖洞笔画的撤销记录里
        //   会混进一份没用的高度，undo 时把地形"顺手还原"了别的东西。
        int parts = 1;
        if (m_tool == Tool::PaintLayer) parts = 2;
        else if (m_tool == Tool::Hole) parts = 4;
        beginSnapshot(parts);
        // 按下立刻落一次 —— 单击（不拖）也必须有反应，否则用户会以为
        // "点一下没反应"，然后开始怀疑笔刷坏了。
        const float dt = std::min(io.DeltaTime, 0.05f);
        const bool invert = io.KeyShift;
        if (m_tool == Tool::PaintLayer) applyLayerStroke(localHit, dt, invert);
        else if (m_tool == Tool::Noise) applyNoiseStroke(localHit, dt);
        else if (m_tool == Tool::Erode) applyErodeStroke(localHit, dt);
        else if (m_tool == Tool::Hole) applyHoleStroke(localHit, dt, invert);
        else if (m_tool == Tool::Ramp) applyRampStroke(localHit, dt);
        else applyStroke(localHit, dt, invert);
        m_pendingSave = true;
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
            saveAssets(true, m_tool == Tool::Hole);
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
                else if (m_tool == Tool::Hole) applyHoleStroke(p, sub, invert);
                else if (m_tool == Tool::Ramp) applyRampStroke(p, sub);
                else applyStroke(p, sub, invert);
            }
            m_lastLocal = localHit;

            m_saveTimer += dt;
            if (m_pendingSave && m_saveTimer >= kSaveInterval) {
                saveAssets(m_tool == Tool::PaintLayer, m_tool == Tool::Hole);
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

    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSizeConstraints(ImVec2(400.0f, 420.0f),
                                        ImVec2(FLT_MAX, FLT_MAX));
    ImGui::SetNextWindowSize(ImVec2(440.0f, 700.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.30f,
                                   io.DisplaySize.y * 0.10f),
                            ImGuiCond_FirstUseEver);
    // ⚠ ID 必须是 `###panel_<面板表里的 id>` —— 与 EditorApp::drawPlaceholderPanel
    //   同一套命名。底栏托盘靠 `ImGui::FindWindowByName("###panel_landscape")`
    //   把这个浮动窗口提到最前面（否则点一下视口，视口就跑到 g.Windows 末尾，
    //   把面板整个盖住 —— 表现是"面板自己消失了"，但它其实还开着）。
    if (!ImGui::Begin(TR("Landscape###panel_landscape"), &open)) {
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
    ImGui::Text("%s", m_source.c_str());
    ImGui::Text(TR("%d x %d samples  ·  %.0f x %.0f m  ·  %.2f m / sample"),
                d.cols, d.rows, static_cast<double>(size.x),
                static_cast<double>(size.y),
                static_cast<double>(d.metersPerPixel));

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

    if (ImGui::Button(TR("Focus"))) focusCamera();
    logRect("LS-FOCUS", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::Button(TR("Save now"))) saveAssets(true);
    ImGui::SameLine();
    ImGui::TextDisabled(TR("auto-saves while you paint"));
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
            for (int i = 0; i < 4; ++i) {
                if (i) ImGui::SameLine();
                const bool on = (m_layer == i);
                if (on) {
                    ImGui::PushStyleColor(
                        ImGuiCol_Button,
                        ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                }
                if (ImGui::Button(TR(kLayerNames[i]), ImVec2(96.0f, 26.0f))) {
                    m_layer = i;
                    VK_LOG_INFO("LS-LAYER %d", i);
                }
                if (on) ImGui::PopStyleColor();
            }
            // 擦除 = 画回第 0 层。勾选与 Shift 是同一个意思（两条路都留，
            // 因为有人习惯按住 Shift、有人习惯点勾选框）。
            ImGui::Checkbox(TR("Erase (paint back to grass)"), &m_eraseLayer);
            break;
        }
        case Tool::Hole: {
            ImGui::SeparatorText(TR("Dig hole"));
            // 洞的实时统计：一眼看出"到底挖穿了多少"，而不是靠肉眼在绿色
            // 地形上找那个洞（俯视时洞边常常和阴影混在一起）。
            int dug = 0;
            for (std::uint8_t v : m_asset.holes) {
                if (v < 128u) ++dug;
            }
            ImGui::Text(TR("Dug samples: %d / %d (%.2f%%)"), dug,
                        static_cast<int>(m_asset.holes.size()),
                        m_asset.holes.empty()
                            ? 0.0f
                            : 100.0f * static_cast<float>(dug) /
                                  static_cast<float>(m_asset.holes.size()));
            ImGui::TextDisabled("%s",
                                TR("Paints the visibility mask. The shader drops "
                                   "those pixels, so you get a real opening "
                                   "instead of a very deep pit. Hold Shift to "
                                   "fill the hole back in."));
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
    // 一行三个按钮 + 一个振幅拖动框，尽量少占竖直空间。
    // ⚠ 这块**不能**放进折叠头：折叠时按钮根本不被渲染，rect 锚点也就
    //   没有 —— 自动化点不到，用户也想不到里面还有东西。同理也不能太长：
    //   面板高度固定 700，内容超出后下面的按钮会滚出可视区，看起来像
    //   "面板少了一半控件"。
    ImGui::SeparatorText(TR("Whole terrain"));

    const float avail = ImGui::GetContentRegionAvail().x;
    if (ImGui::Button(TR("Smooth all"), ImVec2(avail * 0.32f, 24.0f)))
        wholeSmooth();
    logRect("LS-WHOLE-SMOOTH", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::Button(TR("Add noise"), ImVec2(avail * 0.30f, 24.0f)))
        wholeNoise();
    logRect("LS-WHOLE-NOISE", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::DragFloat("##ls_whole_amp", &m_wholeNoiseAmp, 0.002f, 0.005f, 0.30f,
                     TR("amp %.3f"));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", TR("Noise amplitude"));
    }

    if (ImGui::Button(TR("Reset to default shape"), ImVec2(-1.0f, 24.0f)))
        wholeReset();
    logRect("LS-WHOLE-RESET", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
}

void LandscapeEditor::drawBrushSection() {
    ImGui::SeparatorText(TR("Brush"));

    // 2 × 4 的按钮网格（8 个工具）。工具多了之后横排会挤成一条，两列刚好够读。
    struct Item { Tool t; const char* label; };
    const Item tools[kToolCount] = {
        {Tool::Raise, "Raise / Lower"},
        {Tool::Smooth, "Smooth"},
        {Tool::Flatten, "Flatten"},
        {Tool::Ramp, "Ramp"},
        {Tool::Noise, "Noise"},
        {Tool::Erode, "Erode"},
        {Tool::PaintLayer, "Paint layer"},
        {Tool::Hole, "Hole"},
    };
    for (int i = 0; i < kToolCount; ++i) {
        if (i % 2 == 1) ImGui::SameLine();
        const bool on = (m_tool == tools[i].t);
        if (on) {
            ImGui::PushStyleColor(
                ImGuiCol_Button,
                ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        if (ImGui::Button(TR(tools[i].label), ImVec2(188.0f, 28.0f))) {
            m_tool = tools[i].t;
            // ⚠ 两个数字都要打出来，别再混用：`LS-TOOL-<i>` 这个**锚点**用的是
            //   按钮在网格里的视觉序号 i，而 `static_cast<int>(m_tool)` 是
            //   `Tool` 枚举值 —— 两者顺序不一样（枚举里 Noise/Erode/PaintLayer
            //   排在 Hole/Ramp 前面，按钮却把 Ramp 放第 4 格、Hole 放第 8 格）。
            //   只打枚举值时，"按第 8 格 → LS-TOOL 6"读起来像是点错了格。
            VK_LOG_INFO("LS-TOOL %d index=%d name=%s", static_cast<int>(m_tool),
                        i, tools[i].label);
        }
        if (on) ImGui::PopStyleColor();
        char tag[48];
        std::snprintf(tag, sizeof(tag), "LS-TOOL-%d", i);
        logRect(tag, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    }

    labeledSlider(TR("Radius"), "##ls_radius", &m_radius, 1.0f, 500.0f, "%.1f m",
                  /*logScale=*/true);
    // 强度：对数滑条 + 上限 16。UE 的 Tool Strength 到 1.0 只是"标准档"，
    // 整地/整坡时 3~8 才好用；原来卡在 1.0 让人以为"最大就这么快"。
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
    ImGui::Spacing();
    const bool canUndo = !m_undo.empty();
    const bool canRedo = !m_redo.empty();
    if (!canUndo) ImGui::BeginDisabled();
    if (ImGui::Button(TR("Undo stroke"), ImVec2(184.0f, 26.0f))) undoStroke();
    logRect("LS-UNDO-BTN", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (!canUndo) ImGui::EndDisabled();
    ImGui::SameLine();
    if (!canRedo) ImGui::BeginDisabled();
    if (ImGui::Button(TR("Redo"), ImVec2(184.0f, 26.0f))) redoStroke();
    logRect("LS-REDO-BTN", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (!canRedo) ImGui::EndDisabled();
    ImGui::TextDisabled(TR("%d undo steps  ·  Ctrl+Z / Ctrl+Shift+Z"),
                        static_cast<int>(m_undo.size()));

    ImGui::Spacing();
    ImGui::TextDisabled("%s",
                        TR("Left-drag to paint. Shift inverts (lower / erase). "
                           "Right-drag moves the camera. Scroll = radius, "
                           "Shift+scroll = strength."));
    if (m_stroking) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "%s",
                           TR("painting..."));
    }
}

} // namespace editor
