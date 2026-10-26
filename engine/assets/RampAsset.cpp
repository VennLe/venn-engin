// ============================================================
// assets/RampAsset.cpp —— Toon 色带的构造 / 磁盘格式 / 像素烘焙
//
// 设计取舍见头文件。这里只记两条**实现上的**坑：
//
// ① 烘焙必须做**线性 → sRGB 编码**。纹理格式是 R8G8B8A8_SRGB，硬件在
//    采样时会把字节解码回线性。若把线性值直接当字节写（0.45 → 115），
//    采样回来是 srgb_to_linear(115/255) = 0.16 —— 色带整体被压暗一档，
//    而且**只有暗部明显**（sRGB 曲线在低端最陡），看起来像"阴影算错了"。
//    这与 heightMap / normalMap 必须用 UNORM 是同一条纪律的两面。
//
// ② 软化只作用在**第一条交界**（band0 ↔ band1）。赛璐璐画风里只有明暗
//    交界线需要过渡；内部多出来的档（中间调 / 高光档）都是硬切，给它们
//    也加软化会让整张脸糊成写实。所以 `softness` 是"明暗线宽度"，
//    不是"全局模糊" —— 名字与语义要一起读。
// ============================================================

#include "assets/RampAsset.h"

#include "assets/Texture.h"   // unique_ptr<Texture> 的完整类型（见头部的说明）

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>

#include <nlohmann/json.hpp>

namespace assets {

// ---- 特殊成员（必须在这里，不能在头里 =default）----
RampAsset::RampAsset() = default;
RampAsset::~RampAsset() = default;
RampAsset::RampAsset(RampAsset&&) noexcept = default;
RampAsset& RampAsset::operator=(RampAsset&&) noexcept = default;

namespace {

// 线性 [-0,1] → sRGB 编码后的 8 bit。与 GLSL 的 pow(c, 1/2.4) 同一条曲线，
// 用分段公式（sRGB 标准）避免低端斜率误差。
uint8_t toSrgb8(float linear) {
    const float c = std::min(std::max(linear, 0.0f), 1.0f);
    const float s = (c <= 0.0031308f) ? (c * 12.92f)
                                      : (1.055f * std::pow(c, 1.0f / 2.4f) -
                                         0.055f);
    const int v = static_cast<int>(std::lround(s * 255.0f));
    return static_cast<uint8_t>(std::min(std::max(v, 0), 255));
}

} // namespace

void RampAsset::normalize() {
    if (bands.empty()) {
        bands.push_back(RampBand{0.0f, glm::vec3(1.0f)});
    }
    for (RampBand& b : bands) {
        b.pos = std::min(std::max(b.pos, 0.0f), 1.0f);
        b.color = glm::clamp(b.color, glm::vec3(0.0f), glm::vec3(4.0f));
    }
    std::stable_sort(bands.begin(), bands.end(),
                     [](const RampBand& a, const RampBand& b) {
                         return a.pos < b.pos;
                     });
    // 第一档必须从 0 开始：u < bands[0].pos 的那一段总得有个颜色，
    // 让它"继承第一档"比让美术必须手动把第一档拖到 0 更不容易出错。
    bands.front().pos = 0.0f;
    // 相邻档位重合（pos 完全相同）会让"找 band"出现二义性 → 推开 1e-4。
    for (size_t i = 1; i < bands.size(); ++i) {
        if (bands[i].pos <= bands[i - 1].pos) {
            bands[i].pos = std::min(1.0f, bands[i - 1].pos + 1.0e-4f);
        }
    }
    // 交界：钳到 [0.02, 0.98] —— 贴到 0 或 1 时"明暗闸门"会恒 0/恒 1，
    // 阴影要么永远不衰减、要么永远衰减，是两种静默失效。
    edge = std::min(std::max(edge, 0.02f), 0.98f);
    softness = std::min(std::max(softness, 0.0f), 0.30f);
}

RampAsset makeRampPreset(RampPreset preset, const std::string& name) {
    RampAsset r;
    r.name = name;
    r.edge = 0.5f;

    // ⚠ 这些颜色是**乘在 albedo 上的线性系数**，不是"最终颜色"。
    //   所以暗部 0.45/0.49/0.62 的意思是"暗部保留 45% 的 R、并把蓝
    //   相对抬高到 1.38 倍"—— 这就是"阴影偏冷"在数值上的样子。
    const glm::vec3 kDark(0.45f, 0.49f, 0.62f);
    const glm::vec3 kMid(0.72f, 0.74f, 0.80f);
    const glm::vec3 kLit(1.00f, 1.00f, 1.00f);

    switch (preset) {
    case RampPreset::TwoStep:
        r.softness = 0.0f;
        r.bands = {RampBand{0.0f, kDark}, RampBand{0.5f, kLit}};
        break;
    case RampPreset::FourStep:
        r.softness = 0.02f;
        r.bands = {RampBand{0.0f, kDark},
                   RampBand{0.38f, kMid * 0.88f},
                   RampBand{0.58f, kMid},
                   RampBand{0.78f, kLit}};
        break;
    case RampPreset::ThreeStep:
    default:
        r.softness = 0.04f;
        r.bands = {RampBand{0.0f, kDark},
                   RampBand{0.5f, kMid},
                   RampBand{0.75f, kLit}};
        break;
    }
    r.normalize();
    return r;
}

glm::vec3 sampleRamp(const RampAsset& ramp, float u) {
    u = std::min(std::max(u, 0.0f), 1.0f);
    if (ramp.bands.empty()) return glm::vec3(1.0f);

    // ---- 第一条交界：按 softness 线性过渡 ----
    // 与烘焙像素**必须**用同一段代码，否则 CPU 自检与 GPU 结果会分叉。
    const float half = ramp.softness * 0.5f;
    const float b1 = ramp.bandAt(1).pos;
    if (half > 0.0f && u >= b1 - half && u <= b1 + half) {
        const float t = (u - (b1 - half)) / (2.0f * half);
        return glm::mix(ramp.bandAt(0).color, ramp.bandAt(1).color, t);
    }

    // ---- 其余：按 pos 找档，硬切 ----
    size_t idx = 0;
    for (size_t i = 0; i < ramp.bands.size(); ++i) {
        if (u >= ramp.bands[i].pos) idx = i;
        else break;
    }
    return ramp.bands[idx].color;
}

void bakeRampPixels(const RampAsset& ramp, uint32_t width,
                    std::vector<uint8_t>& outRGBA) {
    if (width < 1) width = 1;
    outRGBA.assign(static_cast<size_t>(width) * 4u, 0);
    for (uint32_t x = 0; x < width; ++x) {
        // 采样**纹素中心**：与着色器的 `(floor(u*n)+0.5)/n` 是同一条规则。
        // 用纹素左边界会让整条色带偏移半格，交界位置逐帧对不上。
        const float u = (static_cast<float>(x) + 0.5f) /
                        static_cast<float>(width);
        const glm::vec3 c = sampleRamp(ramp, u);
        uint8_t* px = &outRGBA[static_cast<size_t>(x) * 4u];
        px[0] = toSrgb8(c.r);
        px[1] = toSrgb8(c.g);
        px[2] = toSrgb8(c.b);
        px[3] = 255;
    }
}

bool saveRamp(const RampAsset& ramp, const std::string& path) {
    if (path.empty()) return false;
    nlohmann::json j;
    j["version"] = 1;
    j["edge"] = ramp.edge;
    j["softness"] = ramp.softness;
    j["bands"] = nlohmann::json::array();
    for (const RampBand& b : ramp.bands) {
        j["bands"].push_back({{"pos", b.pos},
                              {"color", {b.color.r, b.color.g, b.color.b}}});
    }

    // 目录不存在就建（第一次存到 assets/ramps/ 时目录往往还没有）。
    // ⚠ 只用 create_directories 末级路径 —— 不做递归扫描，也不删任何东西。
    const std::string full = path;
    const size_t slash = full.find_last_of("/\\");
    if (slash != std::string::npos) {
        std::error_code ec;
        std::filesystem::create_directories(full.substr(0, slash), ec);
    }

    std::ofstream out(full, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << j.dump(2) << "\n";
    return out.good();
}

bool loadRamp(RampAsset& out, const std::string& path) {
    if (path.empty()) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception&) {
        // 坏 JSON 不能让整条资产链挂掉：调用方退化到 3 档预设。
        return false;
    }

    RampAsset r;
    r.path = path;
    r.edge = j.value("edge", 0.5f);
    r.softness = j.value("softness", 0.04f);
    if (j.contains("bands") && j["bands"].is_array()) {
        for (const auto& b : j["bands"]) {
            RampBand band;
            band.pos = b.value("pos", 0.0f);
            if (b.contains("color") && b["color"].is_array() &&
                b["color"].size() >= 3) {
                band.color = glm::vec3(b["color"][0].get<float>(),
                                       b["color"][1].get<float>(),
                                       b["color"][2].get<float>());
            }
            r.bands.push_back(band);
        }
    }
    if (r.bands.empty()) r = makeRampPreset(RampPreset::ThreeStep);
    r.normalize();
    out = std::move(r);
    return true;
}

} // namespace assets
