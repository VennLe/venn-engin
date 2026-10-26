#pragma once
// ============================================================
// assets/RampAsset —— Toon 明暗色带资产（A6，2026-10-26）
//
// 为什么"二次元的明暗分界"必须是一份**资产**而不是几个参数：
//   程序化 ramp（`toonBandFactor` 里那条 smoothstep + 两档量化）只能表达
//   "一个交界 + 两种颜色"。而真实画风里：
//     · 档数不是 2 —— 常见是 3 档（暗 / 中间 / 亮），逆光脸会到 4 档；
//     · **每一档的颜色是美术画的**，不是"亮色 × 系数"—— 暗部可能偏紫
//       （黄昏）、偏绿（树荫）、甚至比亮部更黄（烛光）；
//     · 交界硬到什么程度是画风（赛璐璐是 0 宽，轻赛璐璐有 2~3 px 渐变）。
//   这三件事都只能由一张"色带"表达，参数再多也只是描述同一张色带的
//   几个采样点。
//
// ---- 数据模型 ----
//   一张色带 = N 个 band（按 pos 升序），u ∈ [0,1] 是**半兰伯特**坐标：
//       u = NdotL * 0.5 + 0.5       （背光面 0，正对光 1）
//   band[i] 覆盖 [pos[i], pos[i+1])，最后一段到 1。颜色是**乘在 albedo
//   上的线性系数**（1 = 不改变 albedo）。
//   `edge` / `softness` 只对**第 0 个 band 与第 1 个 band 的交界**起作用
//   （那是"明暗交界线"，也是画风最敏感的一条）：
//       edge      —— 交界位置（默认 0.5，即 NdotL = 0）
//       softness  —— 交界宽度（0 = 硬边，0.02~0.05 = 轻微软化）
//   其余交界永远硬切 —— 赛璐璐的多档只在明暗线上有过渡。
//
// ---- 为什么烘成 256×1 纹理而不是当 uniform 传 ----
//   push 常量已满 128 B、材质页只剩 16 B（`spare`），装不下 N 个颜色。
//   而纹理采样对 GPU 是免费的（一张 256×1 常驻 L2），且**能按 u 做量化
//   采样**（采样点落在色带中心，于是色带内部是平的 —— 这是"关掉线性
//   过滤"的正确做法，见 pbr.frag 的 toonRampTint）。
//
// ---- 与"关 = 基线"的关系 ----
//   没挂 ramp 的材质：材质页 `params.x = -1` → 着色器**连采样都不做**，
//   走原来的程序化两档。所以这个资产存在与否，不影响任何现有材质。
// ============================================================

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace assets {

class Texture;
struct TextureContext;

// 一个色带档位。pos = 该档起始位置（半兰伯特 u），color = 线性乘子。
struct RampBand {
    float pos = 0.0f;
    glm::vec3 color{1.0f, 1.0f, 1.0f};
};

struct RampAsset {
    // ⚠⚠ 特殊成员函数**必须 out-of-line**（定义在 .cpp 里）。
    //   原因：成员 `texture` 是 `unique_ptr<Texture>`，而 Texture 在头里只有
    //   前向声明。编译器生成的析构 / 移动函数落在**每一个包含本头的 TU**，
    //   那里 Texture 还是不完全类型 → `error C2338: can't delete an
    //   incomplete type`。把四个函数挪到 .cpp（那里 include 了 Texture.h）
    //   就只在一处需要完整类型。
    //   ⚠ 只写析构不写移动 = 移动被**隐式删除**，于是 `RampAsset a = std::move(b)`
    //   会静默退化/报错 —— 四个一起声明，一个都不能漏。
    RampAsset();
    ~RampAsset();
    RampAsset(RampAsset&&) noexcept;
    RampAsset& operator=(RampAsset&&) noexcept;
    RampAsset(const RampAsset&) = delete;
    RampAsset& operator=(const RampAsset&) = delete;

    // 缓存键（AssetManager 用；`path` 非空时等于 path）
    std::string name;
    // 落盘路径（相对 assets/ 的 .vramp）。空 = 还没存过盘（纯内存资产，
    // 场景序列化只能存 band 数据 —— 见 SceneSerializer 的说明）。
    std::string path;

    // 至少 1 个。构造时给 3 档默认值（暗 / 中间 / 亮）。
    std::vector<RampBand> bands;
    // 明暗交界（band0↔band1）的位置与软化宽度，占 u ∈ [0,1]。
    float edge = 0.5f;
    float softness = 0.04f;

    // 生成的 256×1 RGBA8(sRGB) 纹理。由 AssetManager::rebuildRampTexture
    // 填充；材质页只认这个指针，所以**每改一次色带都要重建一次**。
    std::unique_ptr<Texture> texture;

    // 档数（= bands.size()）。给"告诉着色器量化成几档"用。
    uint32_t steps() const {
        return static_cast<uint32_t>(bands.size() < 1 ? 1 : bands.size());
    }

    // 按 index 取 band（越界夹紧）—— UI 与采样都用它，避免到处写 clamp。
    const RampBand& bandAt(size_t i) const {
        if (bands.empty()) {
            static const RampBand kWhite{0.0f, glm::vec3(1.0f)};
            return kWhite;
        }
        if (i >= bands.size()) return bands.back();
        return bands[i];
    }

    // 把 bands 排序 + 夹到 [0,1] + 保证首档从 0 开始（数据自愈）。
    // 每次编辑后调用，避免 UI 拖出"pos 逆序"这种非法状态让采样错乱。
    void normalize();
};

// ---- 预设 ----------------------------------------------------
// 2 档：纯赛璐璐（暗部偏蓝，交界硬）。
// 3 档：标准二次元（多一个中间调，脸不会一黑一白）。
// 4 档：逆光/复杂光照用的细档。
enum class RampPreset { TwoStep, ThreeStep, FourStep };
RampAsset makeRampPreset(RampPreset preset, const std::string& name = "ramp");

// ---- 磁盘格式（.vramp，JSON）--------------------------------
//   { "version": 1, "edge": 0.5, "softness": 0.04,
//     "bands": [ { "pos": 0.0, "color": [0.45, 0.49, 0.62] }, ... ] }
// 读失败返回 false（**不抛**），调用方退化到 3 档预设。
bool saveRamp(const RampAsset& ramp, const std::string& path);
bool loadRamp(RampAsset& out, const std::string& path);

// ---- 生成纹理像素 --------------------------------------------
// 把色带烘成 `width` × 1 的 RGBA8。**线性值 → sRGB 编码后再写**：
// 纹理格式是 R8G8B8A8_SRGB，硬件采样时会解码回线性 —— 若直接写线性
// 字节，解码后颜色会被整体压暗（0.5 → 0.21），色带就不是美术画的那个
// 颜色了。这个坑与 heightMap 必须用 UNORM 是同一条。
void bakeRampPixels(const RampAsset& ramp, uint32_t width,
                    std::vector<uint8_t>& outRGBA);

// 逐 texel 的采样（CPU 侧，与着色器的 toonRampTint **同一条规则**）。
// 只给自检/工具用 —— 渲染永远走着色器。返回线性 RGB。
glm::vec3 sampleRamp(const RampAsset& ramp, float u);

} // namespace assets
