// ============================================================
// assets/MaterialSelfTest.cpp —— W1-5 无窗口自检
//
// 断言三组东西：
//   A 参数集 / 位序         —— 位序一旦发布就不能改（存档里是裸数字）
//   B mergeParams 的语义    —— "只刷指定位"是母材质传播的正确性基础
//   C 实例的跟随 / 不跟随   —— 这条链的全部价值所在
//
// ⚠ 这里**不构造 AssetManager**（要真设备），所以测的是
//   `Material::syncInstanceParams` 这个纯函数；
//   `AssetManager::materialChanged` 只是"遍历所有材质 + 调它"。
// ============================================================

#include "assets/MaterialSelfTest.h"

#include "assets/Material.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace assets {
namespace {

int g_pass = 0;
int g_fail = 0;

void check(const char* what, bool ok, const std::string& detail) {
    if (ok) {
        ++g_pass;
        std::printf("[ OK ] %s%s%s\n", what, detail.empty() ? "" : "  ", detail.c_str());
    } else {
        ++g_fail;
        std::printf("[FAIL] %s%s%s\n", what, detail.empty() ? "" : "  ", detail.c_str());
    }
}

std::string f2s(float v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.4f", static_cast<double>(v));
    return std::string(b);
}

// 一份"每个字段都与默认不同"的参数，用来逐位验证 diffParams
Material::Params allDifferent() {
    Material::Params p;
    p.baseColorFactor = glm::vec4(0.1f, 0.2f, 0.3f, 0.4f);
    p.metallic = 0.11f;
    p.roughness = 0.22f;
    p.normalScale = 0.33f;
    p.ao = 0.44f;
    p.emissive = glm::vec3(0.5f, 0.6f, 0.7f);
    p.alphaMode = Material::AlphaMode::Mask;
    p.alphaCutoff = 0.66f;
    p.ditherAlpha = true;
    p.shadingModel = Material::ShadingModel::Skin;
    p.doubleSided = true;
    p.heightScale = 0.077f;
    p.renderQueue = 3100;
    p.sortOrder = 7;
    return p;
}

} // namespace

int runMaterialSelfTest() {
    g_pass = 0;
    g_fail = 0;
    std::printf("MAT-TEST begin (no window, no device)\n");

    // ---------------- A. 参数集 / 位序 ----------------
    {
        using M = Material;
        // A1 位序冻结：27 个字段 = 0x7FFFFFF。这条断言存在的唯一目的，
        //    是"有人手滑改了位序"时立刻炸 —— 存档里存的是裸数字。
        //    12 → 14 是 2026-10-25 追加 renderQueue/sortOrder（P0-2/B），
        //    14 → 20 是同日追加 A 类语义槽的六个标量（材质页 96B），
        //    20 → 22 是 2026-10-26 追加色带的 rampEdge/rampSoftness（A6），
        //    22 → 24 是同日追加头发各向异性的 anisoStrength/anisoShift（A4），
        //    24 → 27 是同日追加眼睛的 eyeStrength/eyeIrisDepth/eyeCatchlight（A3），
        //    **只追加不插队**，所以旧存档的低 24 位含义完全不变。
        //    ⚠ 位 27..31 是最后 **5** 个空位。用完之后 `Params` 这个 POD
        //      快照就要换结构（位掩码是 u32），那时必须升 kMaterialVersion。
        char mb[32];
        std::snprintf(mb, sizeof(mb), "mask=0x%05X",
                      static_cast<unsigned>(M::kOvScalarMask));
        check("A1 material_bits_frozen", M::kOvScalarMask == 0x7FFFFFFu, mb);

        // A2 每个位各自独立（改一个字段 → 只有那一位亮）
        const M::Params base;
        struct Case {
            const char* name;
            std::uint32_t bit;
            M::Params p;
        };
        M::Params p;
        std::vector<Case> cases;
        p = base;
        p.baseColorFactor = glm::vec4(0.9f);
        cases.push_back({"baseColor", M::kOvBaseColor, p});
        p = base;
        p.metallic = 0.9f;
        cases.push_back({"metallic", M::kOvMetallic, p});
        p = base;
        p.roughness = 0.9f;
        cases.push_back({"roughness", M::kOvRoughness, p});
        p = base;
        p.normalScale = 0.9f;
        cases.push_back({"normalScale", M::kOvNormalScale, p});
        p = base;
        p.ao = 0.9f;
        cases.push_back({"ao", M::kOvAo, p});
        p = base;
        p.emissive = glm::vec3(0.9f);
        cases.push_back({"emissive", M::kOvEmissive, p});
        p = base;
        p.alphaMode = M::AlphaMode::Blend;
        cases.push_back({"alphaMode", M::kOvAlphaMode, p});
        p = base;
        p.alphaCutoff = 0.9f;
        cases.push_back({"alphaCutoff", M::kOvAlphaCutoff, p});
        p = base;
        p.ditherAlpha = true;
        cases.push_back({"ditherAlpha", M::kOvDitherAlpha, p});
        p = base;
        p.shadingModel = M::ShadingModel::Skin;
        cases.push_back({"shadingModel", M::kOvShadingModel, p});
        p = base;
        p.doubleSided = true;
        cases.push_back({"doubleSided", M::kOvDoubleSided, p});
        p = base;
        p.heightScale = 0.9f;
        cases.push_back({"heightScale", M::kOvHeightScale, p});
        p = base;
        p.renderQueue = 3100;
        cases.push_back({"renderQueue", M::kOvRenderQueue, p});
        p = base;
        p.sortOrder = 9;
        cases.push_back({"sortOrder", M::kOvSortOrder, p});
        // ---- A 类语义槽的六个标量（2026-10-25）----
        // 每个都用与默认值不同的数：这里测的是"diffParams 认不认得出
        // 这个字段变了"，写个等于默认值的数会让这条 case **静默失效**
        // （diff 为 0，而 wrong 只看 d != bit，会立刻报出来 —— 但如果
        //  有人把默认值也改了，测试就会以一种很难懂的方式变绿）。
        p = base;
        p.rampSteps = 4.0f;
        cases.push_back({"rampSteps", M::kOvRampSteps, p});
        p = base;
        p.matcapStrength = 0.9f;
        cases.push_back({"matcapStrength", M::kOvMatcapStrength, p});
        p = base;
        p.matcapRotate = 0.9f;
        cases.push_back({"matcapRotate", M::kOvMatcapRotate, p});
        p = base;
        p.sdfShadowStrength = 0.9f;
        cases.push_back({"sdfShadowStrength", M::kOvSdfShadowStrength, p});
        p = base;
        p.detailStrength = 0.9f;
        cases.push_back({"detailStrength", M::kOvDetailStrength, p});
        p = base;
        p.detailTiling = 0.9f;
        cases.push_back({"detailTiling", M::kOvDetailTiling, p});
        // ---- A6（2026-10-26）：色带的交界位置/宽度 ----
        p = base;
        p.rampEdge = 0.31f;
        cases.push_back({"rampEdge", M::kOvRampEdge, p});
        p = base;
        p.rampSoftness = 0.11f;
        cases.push_back({"rampSoftness", M::kOvRampSoftness, p});
        // ---- A4（2026-10-26）：头发各向异性的强度 / 环位置 ----
        p = base;
        p.anisoStrength = 0.85f;
        cases.push_back({"anisoStrength", M::kOvAnisoStrength, p});
        p = base;
        p.anisoShift = -0.27f;
        cases.push_back({"anisoShift", M::kOvAnisoShift, p});
        // ---- A3（2026-10-26）：眼睛的 unlit 权重 / 虹膜视差 / 眼神光 ----
        p = base;
        p.eyeStrength = 1.0f;
        cases.push_back({"eyeStrength", M::kOvEyeStrength, p});
        p = base;
        p.eyeIrisDepth = 0.021f;
        cases.push_back({"eyeIrisDepth", M::kOvEyeIrisDepth, p});
        p = base;
        p.eyeCatchlight = 1.35f;
        cases.push_back({"eyeCatchlight", M::kOvEyeCatchlight, p});

        std::uint32_t seen = 0;
        int wrong = 0;
        for (const auto& c : cases) {
            const std::uint32_t d = M::diffParams(base, c.p);
            if (d != c.bit) ++wrong;
            seen |= d;
        }
        char sb[48];
        std::snprintf(sb, sizeof(sb), "wrong=%d seen=0x%03X", wrong,
                      static_cast<unsigned>(seen));
        check("A2 material_diff_each_bit_isolated", wrong == 0, sb);
        // 27 个字段全改 → 恰好是全集，且没有第 28 位
        check("A3 material_diff_all_bits", seen == M::kOvScalarMask, sb);
        check("A4 material_diff_identical_is_zero",
              M::diffParams(base, base) == 0u, "");
    }

    // ---------------- B. mergeParams 只刷指定位 ----------------
    {
        using M = Material;
        M::Params dst;
        dst.roughness = 0.1f;
        dst.metallic = 0.1f;
        M::Params src;
        src.roughness = 0.9f;
        src.metallic = 0.9f;
        M::Params out = dst;
        M::mergeParams(out, src, M::kOvRoughness);
        check("B1 merge_only_listed_bits", out.roughness == 0.9f &&
                                               out.metallic == 0.1f,
              "rough=" + f2s(out.roughness) + " metal=" + f2s(out.metallic));

        out = dst;
        M::mergeParams(out, src, 0u);
        check("B2 merge_zero_bits_is_noop",
              out.roughness == dst.roughness && out.metallic == dst.metallic,
              "rough=" + f2s(out.roughness));
    }

    // ---------------- C. 实例：跟随 / 不跟随 ----------------
    {
        using M = Material;
        M parent;
        parent.name = "parent";
        parent.id = "test#parent";
        parent.roughness = 0.5f;
        parent.metallic = 0.0f;
        parent.baseColorFactor = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f);

        // 实例 = 值语义浅拷贝 + parent + overridden = 0
        M inst;
        inst.applyParams(parent.captureParams());
        inst.name = "inst";
        inst.parent = &parent;
        inst.overridden = 0;

        check("C1 instance_is_copy_of_parent",
              inst.roughness == 0.5f && inst.isInstance(), "");
        check("C2 instance_parent_can_be_parent", parent.canBeParent(), "");
        check("C3 instance_cannot_be_parent", !inst.canBeParent(), "");

        // 母材质改 roughness → 未覆盖的实例跟着变
        parent.roughness = 0.25f;
        const int n1 = M::syncInstanceParams(inst, parent);
        check("C4 unoverridden_follows_parent",
              n1 == 1 && inst.roughness == 0.25f,
              "n=" + std::to_string(n1) + " rough=" + f2s(inst.roughness));

        // 实例本地覆盖 roughness → 之后再改母材质**不跟**
        inst.roughness = 0.95f;
        inst.overridden |= M::kOvRoughness;
        parent.roughness = 0.05f;
        const int n2 = M::syncInstanceParams(inst, parent);
        check("C5 overridden_does_not_follow",
              inst.roughness == 0.95f,
              "n=" + std::to_string(n2) + " rough=" + f2s(inst.roughness));

        // 其它未覆盖字段仍然跟随（"一位覆盖"不该冻结整个材质）
        parent.metallic = 0.75f;
        M::syncInstanceParams(inst, parent);
        check("C6 other_fields_still_follow", inst.metallic == 0.75f,
              "metal=" + f2s(inst.metallic));

        // 清掉覆盖位 → 重新跟随
        inst.overridden &= ~static_cast<std::uint32_t>(M::kOvRoughness);
        M::syncInstanceParams(inst, parent);
        check("C7 revert_clears_override", inst.roughness == 0.05f,
              "rough=" + f2s(inst.roughness));

        // 全 12 位都覆盖 → sync 直接跳过（这是"性能"也是"语义"的守卫：
        // 一个全定制的实例不该被母材质悄悄动到任何字段）
        inst.overridden = M::kOvScalarMask;
        const M::Params before = inst.captureParams();
        parent.roughness = 0.123f;
        parent.metallic = 0.456f;
        const int n3 = M::syncInstanceParams(inst, parent);
        const std::uint32_t touched =
            M::diffParams(before, inst.captureParams());
        check("C8 fully_overridden_is_skipped", n3 == 0 && touched == 0u,
              "n=" + std::to_string(n3) + " touched=" +
                  std::to_string(touched));
    }

    // ---------------- D. D2 的守卫：只存覆盖项 ----------------
    {
        // 场景：实例只覆盖 roughness。母材质改了 metallic 之后，
        // "只写覆盖项"的存档里 metallic **不该出现** —— 否则读回来会被
        // 当成"值 ≠ 母材质 = 覆盖"，从此不跟随。
        using M = Material;
        M parent;
        parent.roughness = 0.5f;
        parent.metallic = 0.0f;

        M inst;
        inst.applyParams(parent.captureParams());
        inst.parent = &parent;
        inst.roughness = 0.9f;
        inst.overridden |= M::kOvRoughness;

        const std::uint32_t written =
            M::diffParams(parent.captureParams(), inst.captureParams());
        char db[32];
        std::snprintf(db, sizeof(db), "bits=0x%03X",
                      static_cast<unsigned>(written));
        check("D1 only_overridden_bits_differ", written == M::kOvRoughness, db);
    }

    std::printf("MAT-TOTAL pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

} // namespace assets
