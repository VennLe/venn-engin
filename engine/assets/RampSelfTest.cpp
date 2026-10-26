// ============================================================
// assets/RampSelfTest.cpp —— 色带的算术 / 编码 / 往返
//
// 五组断言，每一组都对应一个"看起来对、其实错"的失败模式：
//   A 档位查找（含第一条交界的软化）
//   B 线性 → sRGB 编码（错了只会让暗部悄悄变暗）
//   C 烘像素与 CPU 采样一致（GPU 与自检走同一条规则）
//   D normalize 的数据自愈（乱序 / 越界 / 首档不在 0）
//   E .vramp 读写往返（含 bands 数量与颜色逐位相同）
// ============================================================

#include "assets/RampSelfTest.h"

#include "assets/RampAsset.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace assets {

namespace {

int g_pass = 0;
int g_fail = 0;

void check(const char* name, bool ok, const std::string& detail = "") {
    if (ok) {
        ++g_pass;
        std::printf("  [PASS] %s%s%s\n", name, detail.empty() ? "" : "  ",
                    detail.c_str());
    } else {
        ++g_fail;
        std::printf("  [FAIL] %s%s%s\n", name, detail.empty() ? "" : "  ",
                    detail.c_str());
    }
}

bool near3(const glm::vec3& a, const glm::vec3& b, float eps = 1.0e-4f) {
    return std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps &&
           std::fabs(a.z - b.z) < eps;
}

std::string fmt3(const glm::vec3& c) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "(%.3f,%.3f,%.3f)", c.x, c.y, c.z);
    return buf;
}

} // namespace

int runRampSelfTest() {
    g_pass = 0;
    g_fail = 0;
    std::printf("RAMP-TEST begin (no window, no device)\n");

    // ---------------- A. 档位查找 ----------------
    {
        RampAsset r = makeRampPreset(RampPreset::ThreeStep);
        r.softness = 0.0f;   // 先关软化，单独测"找档"
        const glm::vec3 dark = r.bandAt(0).color;
        const glm::vec3 mid = r.bandAt(1).color;
        const glm::vec3 lit = r.bandAt(2).color;
        char d[96];
        std::snprintf(d, sizeof(d), "u=0.10 -> %s", fmt3(sampleRamp(r, 0.10f)).c_str());
        check("A1 u<edge 落到第 0 档", near3(sampleRamp(r, 0.10f), dark), d);
        std::snprintf(d, sizeof(d), "u=0.60 -> %s", fmt3(sampleRamp(r, 0.60f)).c_str());
        check("A2 中间档", near3(sampleRamp(r, 0.60f), mid), d);
        std::snprintf(d, sizeof(d), "u=0.95 -> %s", fmt3(sampleRamp(r, 0.95f)).c_str());
        check("A3 最亮档", near3(sampleRamp(r, 0.95f), lit), d);
        // 越界必须夹紧（半兰伯特在 ndl=0/1 处正好到 0/1，浮点误差会溢出一丁点）
        check("A4 u<0 夹到 0", near3(sampleRamp(r, -0.5f), dark));
        check("A5 u>1 夹到 1", near3(sampleRamp(r, 1.5f), lit));

        // 软化：交界中点应当是两个档的 50/50
        r.softness = 0.10f;   // 交界 [0.45, 0.55]
        const glm::vec3 half = sampleRamp(r, 0.50f);
        const glm::vec3 want = glm::mix(dark, mid, 0.5f);
        std::snprintf(d, sizeof(d), "mid=%s want=%s (edge=%.2f soft=%.2f)",
                      fmt3(half).c_str(), fmt3(want).c_str(), r.edge,
                      r.softness);
        check("A6 交界中点 = 两档均值", near3(half, want, 5.0e-3f), d);
        // 交界外必须**完全**回到硬档（不是"渐变到很远"）
        check("A7 交界外仍是纯档位",
              near3(sampleRamp(r, 0.30f), dark, 1.0e-4f));
    }

    // ---------------- B. sRGB 编码 ----------------
    {
        // 线性 1.0 → 255；线性 0.0 → 0；线性 0.5 → 188（sRGB 标准值）
        RampAsset r;
        r.bands = {RampBand{0.0f, glm::vec3(0.5f)}};
        r.edge = 0.5f;
        r.softness = 0.0f;
        std::vector<uint8_t> px;
        bakeRampPixels(r, 4, px);
        char d[64];
        std::snprintf(d, sizeof(d), "byte=%u (want 188)", px[0]);
        // 0.5 线性 → 1.055*0.5^(1/2.4)-0.055 = 0.7354 → 188
        check("B1 线性 0.5 → sRGB 188", px[0] == 188, d);
        check("B2 alpha 恒 255", px[3] == 255);

        r.bands = {RampBand{0.0f, glm::vec3(1.0f)}};
        bakeRampPixels(r, 4, px);
        check("B3 线性 1.0 → 255", px[0] == 255);
        r.bands = {RampBand{0.0f, glm::vec3(0.0f)}};
        bakeRampPixels(r, 4, px);
        check("B4 线性 0.0 → 0", px[0] == 0);
        // 与"直接写线性字节"的对照：0.5 若直接写会是 128，差 60 个码值 ——
        // 这就是"忘了编码"的表现（暗部整体压暗一档）。
        check("B5 不是线性直写（128 是错误的）", px[0] != 128);
    }

    // ---------------- C. 烘像素 ↔ CPU 采样 ----------------
    {
        RampAsset r = makeRampPreset(RampPreset::FourStep);
        constexpr uint32_t kW = 256;
        std::vector<uint8_t> px;
        bakeRampPixels(r, kW, px);

        // ⚠ 这里**独立实现**一遍 sRGB 编码，不复用 RampAsset.cpp 里的那个：
        //   复用的话"编码公式写错"会让两边一起错，自检就恒绿了。
        const auto srgb8 = [](float lin) {
            const float c = lin < 0.0f ? 0.0f : (lin > 1.0f ? 1.0f : lin);
            const float e = (c <= 0.0031308f)
                                ? (c * 12.92f)
                                : (1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f);
            int v = static_cast<int>(e * 255.0f + 0.5f);
            return v < 0 ? 0 : (v > 255 ? 255 : v);
        };

        int worst = 0;
        int worstX = -1;
        for (uint32_t x = 0; x < kW; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) /
                            static_cast<float>(kW);
            const glm::vec3 c = sampleRamp(r, u);
            const int want[3] = {srgb8(c.r), srgb8(c.g), srgb8(c.b)};
            for (int k = 0; k < 3; ++k) {
                const int got = static_cast<int>(px[x * 4 + k]);
                if (std::abs(got - want[k]) > worst) {
                    worst = std::abs(got - want[k]);
                    worstX = static_cast<int>(x);
                }
            }
        }
        char d[96];
        std::snprintf(d, sizeof(d), "max|dbyte|=%d at texel %d", worst, worstX);
        // 只允许 ±1（8 bit 量化 + 采样点对齐）。差得更多 = 采样规则与
        // 烘像素规则分叉（GPU 与自检会给出两个答案）。
        check("C1 每个纹素 = sampleRamp 的 sRGB 编码（±1）", worst <= 1, d);

        // 档位之间必须**出现跳变**，否则"色带没烘进去"（全是同一档色）。
        int jumps = 0;
        for (uint32_t x = 1; x < kW; ++x) {
            const int a = px[(x - 1) * 4 + 2];
            const int b = px[x * 4 + 2];
            if (std::abs(a - b) > 8) ++jumps;
        }
        std::snprintf(d, sizeof(d), "jumps=%d", jumps);
        // 4 档 = 3 条交界。第一条带 0.02 软化（占 256*0.02 ≈ 5 个纹素，
        // 相邻差可能不到 8），其余两条是硬切 → 至少 2 处跳变。
        check("C2 色带里出现了档位跳变（≥2 处）", jumps >= 2, d);
        // 对照：单档色带**不该**有任何跳变 —— 证明 C2 测的是"分档"，
        // 而不是"任何像素差"。
        RampAsset flat;
        flat.bands = {RampBand{0.0f, glm::vec3(0.5f)}};
        flat.normalize();
        bakeRampPixels(flat, kW, px);
        int flatJumps = 0;
        for (uint32_t x = 1; x < kW; ++x)
            if (std::abs(static_cast<int>(px[(x - 1) * 4 + 2]) -
                         static_cast<int>(px[x * 4 + 2])) > 8)
                ++flatJumps;
        std::snprintf(d, sizeof(d), "flat jumps=%d", flatJumps);
        check("C3 单档色带没有任何跳变（对照）", flatJumps == 0, d);
    }

    // ---------------- D. normalize 的数据自愈 ----------------
    {
        RampAsset r;
        r.bands = {RampBand{0.9f, glm::vec3(1.0f)},
                   RampBand{0.2f, glm::vec3(0.5f)},
                   RampBand{-3.0f, glm::vec3(1.5f, -0.5f, 9.0f)}};
        r.edge = 1.7f;
        r.softness = -2.0f;
        r.normalize();
        bool sorted = true;
        for (size_t i = 1; i < r.bands.size(); ++i)
            if (r.bands[i].pos < r.bands[i - 1].pos) sorted = false;
        check("D1 档位按 pos 升序", sorted);
        check("D2 首档 pos 被拉回 0", r.bands.front().pos == 0.0f);
        bool inRange = true;
        for (const auto& b : r.bands)
            if (b.pos < 0.0f || b.pos > 1.0f) inRange = false;
        check("D3 pos 全部夹进 [0,1]", inRange);
        char d[96];
        std::snprintf(d, sizeof(d), "edge=%.3f soft=%.3f color=%s", r.edge,
                      r.softness, fmt3(r.bands[2].color).c_str());
        check("D4 edge 夹到 [0.02,0.98]", r.edge >= 0.02f && r.edge <= 0.98f, d);
        check("D5 softness 夹到 [0,0.30]",
              r.softness >= 0.0f && r.softness <= 0.30f);
        check("D6 颜色被夹到 [0,4]",
              r.bands[2].color.z <= 4.0f && r.bands[2].color.y >= 0.0f, d);

        // 空 bands 不能崩（读一份坏 .vramp 的最坏情况）
        RampAsset empty;
        empty.normalize();
        check("D7 空 bands 自愈成 1 档", empty.bands.size() == 1);
    }

    // ---------------- E. .vramp 读写往返 ----------------
    {
        namespace fs = std::filesystem;
        const fs::path dir =
            fs::temp_directory_path() / "venn_ramp_selftest";
        std::error_code ec;
        fs::create_directories(dir, ec);
        const std::string path = (dir / "roundtrip.vramp").string();

        RampAsset src = makeRampPreset(RampPreset::FourStep, "roundtrip");
        src.edge = 0.42f;
        src.softness = 0.07f;
        const bool saved = saveRamp(src, path);
        check("E1 保存成功", saved, path);

        RampAsset dst;
        const bool loaded = loadRamp(dst, path);
        check("E2 读取成功", loaded);
        char d[96];
        std::snprintf(d, sizeof(d), "bands %u vs %u edge %.3f vs %.3f",
                      static_cast<unsigned>(src.steps()),
                      static_cast<unsigned>(dst.steps()), src.edge, dst.edge);
        check("E3 档数一致", src.steps() == dst.steps(), d);
        check("E4 edge/softness 一致",
              std::fabs(src.edge - dst.edge) < 1.0e-5f &&
                  std::fabs(src.softness - dst.softness) < 1.0e-5f, d);
        bool sameColors = true;
        for (size_t i = 0; i < src.bands.size() && i < dst.bands.size(); ++i) {
            if (!near3(src.bands[i].color, dst.bands[i].color, 1.0e-5f) ||
                std::fabs(src.bands[i].pos - dst.bands[i].pos) > 1.0e-5f)
                sameColors = false;
        }
        check("E5 每个档的位置与颜色逐位一致", sameColors);

        // 坏文件必须**返回 false 而不是抛**（资产链上文件随时可能被改坏）
        const std::string bad = (dir / "broken.vramp").string();
        {
            std::FILE* f = std::fopen(bad.c_str(), "wb");
            if (f) {
                std::fputs("{ not json", f);
                std::fclose(f);
            }
        }
        RampAsset broken;
        check("E6 坏 JSON 返回 false（不抛）", !loadRamp(broken, bad));
        check("E7 不存在的文件返回 false",
              !loadRamp(broken, (dir / "nope.vramp").string()));

        // 清理：只删本自检自己写的两个文件（用 safe_fs 那条纪律的理由：
        // 宿主对批量删除有硬阈值，这里只有 2 个文件，但仍走 fs::remove
        // 而不是通配 —— 绝不误删目录里的其它东西）。
        fs::remove(path, ec);
        fs::remove(bad, ec);
        fs::remove(dir, ec);
    }

    std::printf("RAMP-TEST total pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail;
}

} // namespace assets
