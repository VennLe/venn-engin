#include "fx/EffectAsset.h"

#include "scene/ParticleJson.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>

namespace fx {

using json = nlohmann::json;

// ---------------------------------------------------------------- 时长

float EffectAsset::effectiveDuration() const {
    float total = std::max(duration, 0.0f);

    for (const ecs::ParticleEmitterComponent& p : particles) {
        // 一轮发射什么时候结束：
        //   循环的发射器没有终点 → 用本体时长兜底（附魔类特效就是这种）
        //   非循环 → startDelay + duration
        float emitEnd = std::max(p.startDelay, 0.0f);
        if (!p.looping) emitEnd += std::max(p.duration, 0.0f);

        // burst 可能落在 emitEnd 之后（作者常把最后一波爆发放在尾巴上）
        for (int b = 0; b < ecs::kMaxBursts; ++b) {
            const ecs::ParticleBurst& s = p.bursts[b];
            if (s.count <= 0) continue;
            const int cycles = std::max(s.cycles, 1);
            const float last = std::max(p.startDelay, 0.0f) + std::max(s.time, 0.0f) +
                               static_cast<float>(cycles - 1) * std::max(s.interval, 0.0f);
            emitEnd = std::max(emitEnd, last);
        }

        // 最后出生的那颗粒子还要活完它的一生。
        // variance 是"±比例"，所以最长寿命是 lifetime × (1 + variance) ——
        // 用 lifetime 本身会让最长的那些粒子被截断（少数粒子定格在空中）。
        const float life = std::max(p.lifetime, 0.0f) *
                           (1.0f + std::max(p.lifetimeVariance, 0.0f));
        total = std::max(total, emitEnd + life);
    }

    return std::clamp(total, 0.01f, kMaxEffectDuration);
}

// ---------------------------------------------------------------- 解析

namespace {

// 读一个 float，非法就保留默认并把原因记进 warnings。
// 不抛异常：一份资产里一处笔误不该让整场战斗没有特效。
float readFloat(const json& j, const char* key, float def,
                std::vector<std::string>* warns) {
    if (!j.contains(key)) return def;
    if (!j[key].is_number()) {
        if (warns) {
            warns->push_back(std::string("field '") + key +
                             "' is not a number; using default");
        }
        return def;
    }
    return j[key].get<float>();
}

std::string readString(const json& j, const char* key, const std::string& def) {
    if (!j.contains(key) || !j[key].is_string()) return def;
    return j[key].get<std::string>();
}

} // namespace

EffectAsset parseEffectAsset(const json& j, std::string* err) {
    EffectAsset a;
    if (err) err->clear();

    if (!j.is_object()) {
        if (err) *err = "effect asset is not a JSON object";
        return a;   // valid 仍为 false
    }

    a.name = readString(j, "name", std::string());

    // ---- 时序 ----
    a.duration = readFloat(j, "duration", a.duration, &a.warnings);
    if (!(a.duration > 0.0f)) {
        // ⚠ 这条必须夹：0 / 负数时长会让宿主"立刻可回收"或"永不回收"，
        //   后者的表现是池被一个看不见的特效永久占满 —— 之后所有命中都
        //   静默没有火花，而日志一条都不打。
        a.warnings.push_back("duration <= 0; clamped to default");
        a.duration = 0.6f;
    }

    // ---- 打击感 ----
    a.hitstop = readFloat(j, "hitstop", 0.0f, &a.warnings);
    a.hitstop = std::clamp(a.hitstop, 0.0f, 0.5f);   // 半秒以上就不是"顿帧"了
    a.shake = std::clamp(readFloat(j, "shake", 0.0f, &a.warnings), 0.0f, 1.0f);
    a.shakeRadius = std::max(readFloat(j, "shakeRadius", a.shakeRadius, &a.warnings),
                             0.0f);
    a.flash = std::clamp(readFloat(j, "flash", 0.0f, &a.warnings), 0.0f, 1.0f);
    a.flashDuration = std::max(readFloat(j, "flashDuration", 0.0f, &a.warnings), 0.0f);
    // `flash` 给了、`flashDuration` 没给 = 作者以为"跟着 duration 走"。
    // 不管的话这个特效**一点都不闪**，而且不报错 —— 属于"配了没效果"里
    // 最难查的一类。这里补一个默认值并记一条 warning，让它至少是看得见的。
    // ⚠ 上限 0.25：闪白超过 1/4 秒就开始"糊住整个画面"，比不闪更糟。
    if (a.flash > 0.0f && a.flashDuration <= 0.0f) {
        a.flashDuration = std::min(a.duration, 0.25f);
        a.warnings.push_back("flash set without flashDuration -> using " +
                             std::to_string(a.flashDuration) + "s");
    }
    if (j.contains("flashColor") && j["flashColor"].is_array() &&
        j["flashColor"].size() >= 3) {
        a.flashColor = glm::vec3(j["flashColor"][0].get<float>(),
                                 j["flashColor"][1].get<float>(),
                                 j["flashColor"][2].get<float>());
    }

    // ---- 声音 ----
    a.sound = readString(j, "sound", std::string());
    a.volume = std::max(readFloat(j, "volume", a.volume, &a.warnings), 0.0f);
    a.pitchVariance =
        std::clamp(readFloat(j, "pitchVariance", a.pitchVariance, &a.warnings),
                   0.0f, 0.5f);

    // ---- 粒子层 ----
    if (j.contains("particles")) {
        if (!j["particles"].is_array()) {
            // ⚠ 这条是**致命**的（不是 warning）：作者把 particles 写成对象
            //   （`"particles": {...}`）时，静默忽略会得到"一个只有震屏的
            //   特效"，看起来像"粒子参数没生效"，而那是最难查的一类。
            if (err) *err = "'particles' must be an array";
            return a;
        }
        const json& arr = j["particles"];
        const int count = static_cast<int>(arr.size());
        const int take = std::min(count, kMaxEffectLayers);
        if (count > kMaxEffectLayers) {
            a.warnings.push_back("particles truncated from " +
                                 std::to_string(count) + " to " +
                                 std::to_string(kMaxEffectLayers) + " layers");
        }
        a.particles.reserve(static_cast<std::size_t>(take));
        for (int i = 0; i < take; ++i) {
            // 复用场景那边的解析 —— 与"场景里存的发射器"逐字段同一份实现。
            a.particles.push_back(scene::particleEmitterFromJson(arr[i]));
        }
    }

    a.valid = true;
    return a;
}

EffectAsset loadEffectAssetFile(const std::string& absPath, std::string* err) {
    std::ifstream in(absPath, std::ios::binary);
    if (!in) {
        if (err) *err = "cannot open: " + absPath;
        return EffectAsset{};
    }
    json j;
    try {
        in >> j;
    } catch (const std::exception& ex) {
        if (err) *err = std::string("JSON parse error: ") + ex.what();
        return EffectAsset{};
    }
    return parseEffectAsset(j, err);
}

// ---------------------------------------------------------------- 自检

namespace {

int g_fxAssetFail = 0;
int g_fxAssetPass = 0;

void fxCheck(const char* name, bool ok, const std::string& detail = {}) {
    if (ok) {
        ++g_fxAssetPass;
        std::printf("FX-ASSET PASS %s\n", name);
    } else {
        ++g_fxAssetFail;
        std::printf("FX-ASSET FAIL %s%s%s\n", name, detail.empty() ? "" : " -- ",
                    detail.c_str());
    }
}

json parseOrEmpty(const char* text) {
    try {
        return json::parse(text);
    } catch (...) {
        return json::object();
    }
}

} // namespace

int runEffectAssetSelfTest() {
    g_fxAssetFail = 0;
    g_fxAssetPass = 0;

    // ---- E1 最小资产：只有名字也能用（全部走默认）----
    {
        std::string err;
        const EffectAsset a = parseEffectAsset(parseOrEmpty(R"({"name":"min"})"), &err);
        fxCheck("E1 minimal-valid", a.valid && a.name == "min" && err.empty());
        fxCheck("E1 minimal-defaults",
                a.duration > 0.0f && a.particles.empty() && a.shake == 0.0f &&
                a.hitstop == 0.0f && a.sound.empty());
    }

    // ---- E2 完整资产：每个打击感字段都读对 ----
    {
        std::string err;
        const EffectAsset a = parseEffectAsset(parseOrEmpty(R"({
            "name":"slash_a",
            "duration":0.45,
            "hitstop":0.07,
            "shake":0.35,
            "shakeRadius":9.5,
            "flash":0.25,
            "flashDuration":0.08,
            "flashColor":[1.0,0.9,0.7],
            "sound":"sfx/hit.wav",
            "volume":0.8,
            "pitchVariance":0.12
        })"), &err);
        fxCheck("E2 full-valid", a.valid && err.empty());
        fxCheck("E2 timing", std::fabs(a.duration - 0.45f) < 1e-5f &&
                             std::fabs(a.hitstop - 0.07f) < 1e-5f);
        fxCheck("E2 shake", std::fabs(a.shake - 0.35f) < 1e-5f &&
                            std::fabs(a.shakeRadius - 9.5f) < 1e-5f);
        fxCheck("E2 flash", std::fabs(a.flash - 0.25f) < 1e-5f &&
                            std::fabs(a.flashDuration - 0.08f) < 1e-5f &&
                            std::fabs(a.flashColor.y - 0.9f) < 1e-5f);
        fxCheck("E2 sound", a.sound == "sfx/hit.wav" &&
                            std::fabs(a.volume - 0.8f) < 1e-5f &&
                            std::fabs(a.pitchVariance - 0.12f) < 1e-5f);
    }

    // ---- E3 particles 写成对象 → **致命**（不是静默忽略）----
    {
        std::string err;
        const EffectAsset a = parseEffectAsset(
            parseOrEmpty(R"({"name":"bad","particles":{"rate":10}})"), &err);
        fxCheck("E3 particles-not-array-rejected",
                !a.valid && !err.empty() && err.find("array") != std::string::npos,
                err);
    }

    // ---- E4 空数组合法：纯震屏 / 纯闪白特效（爆炸白闪就长这样）----
    {
        std::string err;
        const EffectAsset a = parseEffectAsset(
            parseOrEmpty(R"({"name":"pure","shake":0.5,"particles":[]})"), &err);
        fxCheck("E4 empty-particles-valid", a.valid && a.particles.empty() &&
                                            a.shake == 0.5f, err);
    }

    // ---- E5 层数超限：截断 + 留 warning（不静默丢）----
    {
        std::string err;
        json j = parseOrEmpty(R"({"name":"many","particles":[{},{},{},{},{}]})");
        const EffectAsset a = parseEffectAsset(j, &err);
        fxCheck("E5 layers-truncated",
                a.valid && static_cast<int>(a.particles.size()) == kMaxEffectLayers,
                std::to_string(a.particles.size()));
        fxCheck("E5 truncated-warns", !a.warnings.empty(),
                a.warnings.empty() ? "no warning" : a.warnings.front());
    }

    // ---- E6 duration <= 0 被夹：防"池被一个看不见的特效永久占满"----
    {
        std::string err;
        const EffectAsset a = parseEffectAsset(
            parseOrEmpty(R"({"name":"zero","duration":0})"), &err);
        fxCheck("E6 zero-duration-clamped", a.valid && a.duration > 0.0f &&
                                            !a.warnings.empty());
        const EffectAsset b = parseEffectAsset(
            parseOrEmpty(R"({"name":"neg","duration":-3})"), &err);
        fxCheck("E6 negative-duration-clamped", b.valid && b.duration > 0.0f);
    }

    // ---- E7 粒子字段**真的**被读进来（证明复用了场景那份解析）----
    {
        std::string err;
        const EffectAsset a = parseEffectAsset(parseOrEmpty(R"({
            "name":"spark",
            "particles":[{
                "rate":250.0,
                "lifetime":1.4,
                "startSpeed":6.0,
                "emissive":4.5,
                "bursts":[{"time":0.0,"count":18,"cycles":1,"interval":0.1}]
            }]
        })"), &err);
        const bool ok = a.valid && a.particles.size() == 1 &&
                        std::fabs(a.particles[0].rate - 250.0f) < 1e-4f &&
                        std::fabs(a.particles[0].lifetime - 1.4f) < 1e-4f &&
                        std::fabs(a.particles[0].startSpeed - 6.0f) < 1e-4f &&
                        std::fabs(a.particles[0].emissive - 4.5f) < 1e-4f &&
                        a.particles[0].bursts[0].count == 18;
        fxCheck("E7 particle-fields-read", ok,
                a.particles.empty() ? "no layer" : "bursts/count mismatch");
    }

    // ---- E8 effectiveDuration 必须罩住"粒子寿命比本体长" ----
    //   这是"火花定格在空中"的唯一判据：本体 0.3 秒喷完，每颗飞 2 秒。
    {
        std::string err;
        const EffectAsset a = parseEffectAsset(parseOrEmpty(R"({
            "name":"longtail",
            "duration":0.3,
            "particles":[{"duration":0.0,"looping":false,
                          "lifetime":2.0,"lifetimeVariance":0.0,
                          "bursts":[{"time":0.0,"count":8,"cycles":1,"interval":0}]}]
        })"), &err);
        const float d = a.effectiveDuration();
        fxCheck("E8 effective-covers-particle-life", a.valid && d >= 2.0f,
                "effective=" + std::to_string(d));
        // 本体比粒子长时，不能被粒子反过来缩短
        const EffectAsset b = parseEffectAsset(parseOrEmpty(R"({
            "name":"shorttail","duration":3.0,
            "particles":[{"lifetime":0.5,"lifetimeVariance":0.0}]
        })"), &err);
        fxCheck("E8 effective-not-shorter-than-duration",
                b.effectiveDuration() >= 3.0f);
    }

    // ---- E9 文件层：不存在的路径返回 invalid，且错误可读 ----
    {
        std::string err;
        const EffectAsset a = loadEffectAssetFile("__no_such_effect__.veffect", &err);
        fxCheck("E9 missing-file-rejected", !a.valid && !err.empty(), err);
    }

    // ---- E10 `flash` 给了、`flashDuration` 没给 → 补默认值 + 一条 warning ----
    //
    // 这一条防的是"配了没效果"里最难查的一类：作者写 `"flash": 0.3` 就以为
    // 会闪，实际一点都不闪，而且**没有任何错误**。补默认值不如报错激进，
    // 但比静默好 —— 至少 `FX-ASSET` 的 WARN 行会在控制台里出现一次。
    {
        std::string err;
        const EffectAsset a = parseEffectAsset(parseOrEmpty(R"({
            "name":"flash_only","duration":0.4,"flash":0.3,"particles":[]
        })"), &err);
        fxCheck("E10 flash-defaults-to-duration",
                a.valid && a.flashDuration > 0.0f &&
                    a.flashDuration <= 0.25f + 1e-5f &&
                    !a.warnings.empty(),
                "flashDuration=" + std::to_string(a.flashDuration));
    }

    std::printf("FX-ASSET-TOTAL pass=%d fail=%d\n", g_fxAssetPass, g_fxAssetFail);
    return g_fxAssetFail;
}

} // namespace fx
