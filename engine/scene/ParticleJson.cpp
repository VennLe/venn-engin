#include "scene/ParticleJson.h"

#include <nlohmann/json.hpp>

namespace scene {

using json = nlohmann::json;

namespace {

// 与 SceneSerializer.cpp 里同名的那几个是**各自 TU 的内部 helper**：
// 语义就是"三个/四个 float 打包成数组"，没有第二种解释，所以重复一份
// 不构成"两套定义"（真正会漂移的是字段清单，那份在下面搬过来的函数里，
// 全仓唯一）。
json vec3ToJson(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }
json vec4ToJson(const glm::vec4& v) {
    return json::array({v.x, v.y, v.z, v.w});
}

// 缺分量 / 不是数组 → 返回默认值。与 `SceneSerializer.cpp` 那两个逐字一致：
// 这段语义只有一种（"读不到就用默认"），重复不影响正确性。
glm::vec3 vec3FromJson(const json& j, const glm::vec3& def) {
    if (!j.is_array() || j.size() < 3) return def;
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}
glm::vec4 vec4FromJson(const json& j, const glm::vec4& def) {
    if (!j.is_array() || j.size() < 4) return def;
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>(),
            j[3].get<float>()};
}

} // namespace

json particleEmitterToJson(const ecs::ParticleEmitterComponent& p) {
    json j;
    // ---- Main / Playback ----
    j["rate"] = p.rate;
    j["rateOverDistance"] = p.rateOverDistance;
    j["duration"] = p.duration;
    j["looping"] = p.looping;
    j["prewarm"] = p.prewarm;
    j["startDelay"] = p.startDelay;
    j["lifetime"] = p.lifetime;
    j["lifetimeVariance"] = p.lifetimeVariance;
    j["startSpeed"] = p.startSpeed;
    j["speedVariance"] = p.speedVariance;
    j["spread"] = p.spread;
    // ---- Emission（bursts 是数组；burns[0] 同时也写成 "burst" 作为
    //      老读法 / 老写法的兼容入口）----
    {
        json arr = json::array();
        for (int b = 0; b < ecs::kMaxBursts; ++b) {
            const ecs::ParticleBurst& s = p.bursts[b];
            json e;
            e["time"] = s.time;
            e["count"] = s.count;
            e["cycles"] = s.cycles;
            e["interval"] = s.interval;
            arr.push_back(e);
        }
        j["bursts"] = arr;
        j["burst"] = p.bursts[0].count;
    }
    // ---- Shape ----
    j["shape"] = static_cast<int>(p.shape);
    j["shapeExtents"] = vec3ToJson(p.shapeExtents);
    j["shapeRadius"] = p.shapeRadius;
    j["radiusThickness"] = p.radiusThickness;
    j["arc"] = p.arc;
    j["shapeRotation"] = p.shapeRotation;
    j["emitFrom"] = static_cast<int>(p.emitFrom);
    // ---- Velocity over Lifetime ----
    j["velOverLifeEnabled"] = p.velOverLifeEnabled;
    j["velOverLifeX"] = vec4ToJson(p.velOverLifeX);
    j["velOverLifeY"] = vec4ToJson(p.velOverLifeY);
    j["velOverLifeZ"] = vec4ToJson(p.velOverLifeZ);
    j["velOverLifeWorldSpace"] = p.velOverLifeWorldSpace;
    j["orbital"] = vec3ToJson(p.orbital);
    j["orbitalOffset"] = p.orbitalOffset;
    j["radial"] = p.radial;
    j["speedModifier"] = vec4ToJson(p.speedModifier);
    // ---- Limit Velocity ----
    j["limitVelEnabled"] = p.limitVelEnabled;
    j["limitSpeed"] = p.limitSpeed;
    j["limitDampen"] = p.limitDampen;
    // ---- Force ----
    j["forceEnabled"] = p.forceEnabled;
    j["force"] = vec3ToJson(p.force);
    j["forceRandomize"] = p.forceRandomize;
    // ---- Rotation over Lifetime ----
    j["startRotation"] = p.startRotation;
    j["startRotationVariance"] = p.startRotationVariance;
    j["angularVelocity"] = p.angularVelocity;
    j["angularVelocityVariance"] = p.angularVelocityVariance;
    // ---- Noise ----
    j["noiseEnabled"] = p.noiseEnabled;
    j["noiseStrength"] = p.noiseStrength;
    j["noiseFrequency"] = p.noiseFrequency;
    j["noiseScrollSpeed"] = p.noiseScrollSpeed;
    j["noiseDamping"] = p.noiseDamping;
    j["noiseOctaves"] = p.noiseOctaves;
    // ---- Physics ----
    j["gravity"] = vec3ToJson(p.gravity);
    j["drag"] = p.drag;
    j["simulationSpace"] = static_cast<int>(p.simulationSpace);
    // ---- Size / Color over Lifetime ----
    j["sizeOverLife"] = vec4ToJson(p.sizeOverLife);
    j["alphaOverLife"] = vec4ToJson(p.alphaOverLife);
    {
        json ramp = json::array();
        for (int s = 0; s < p.colorRamp.count && s < ecs::kMaxColorStops; ++s) {
            json e;
            e["t"] = p.colorRamp.stops[s].w;
            e["color"] = vec3ToJson(glm::vec3(p.colorRamp.stops[s]));
            ramp.push_back(e);
        }
        j["colorRamp"] = ramp;
        j["colorRampCount"] = p.colorRamp.count;
        // 老的三个字段继续写（外部工具 / 老版本的读取路径），
        // 取渐变带上 t = 0 / 0.5 / 1 的采样值。
        j["colorStart"] = vec3ToJson(p.colorRamp.eval(0.0f));
        j["colorMid"] = vec3ToJson(p.colorRamp.eval(0.5f));
        j["colorEnd"] = vec3ToJson(p.colorRamp.eval(1.0f));
    }
    // ---- Renderer ----
    j["startSize"] = p.startSize;
    j["sprite"] = static_cast<int>(p.sprite);
    j["blend"] = static_cast<int>(p.blend);
    j["renderMode"] = static_cast<int>(p.renderMode);
    j["sortMode"] = static_cast<int>(p.sortMode);
    j["stretchScale"] = p.stretchScale;
    j["emissive"] = p.emissive;
    j["softParticles"] = p.softParticles;
    j["softDistance"] = p.softDistance;
    j["enabled"] = p.enabled;
    return j;
}

ecs::ParticleEmitterComponent particleEmitterFromJson(const json& j) {
    ecs::ParticleEmitterComponent p;
    if (!j.is_object()) return p;
    const json null;
    // ---- Main / Playback ----
    p.rate = j.value("rate", p.rate);
    p.rateOverDistance = j.value("rateOverDistance", p.rateOverDistance);
    p.duration = j.value("duration", p.duration);
    p.looping = j.value("looping", p.looping);
    p.prewarm = j.value("prewarm", p.prewarm);
    p.startDelay = j.value("startDelay", p.startDelay);
    p.lifetime = j.value("lifetime", p.lifetime);
    p.lifetimeVariance = j.value("lifetimeVariance", p.lifetimeVariance);
    p.startSpeed = j.value("startSpeed", p.startSpeed);
    p.speedVariance = j.value("speedVariance", p.speedVariance);
    p.spread = j.value("spread", p.spread);
    // ---- Emission ----
    // 新格式：bursts 数组。老格式：单个 "burst"（= 时刻 0 的一次爆发）。
    // 两条都认，这样老场景能原样打开。
    if (j.contains("bursts") && j["bursts"].is_array()) {
        const json& arr = j["bursts"];
        for (int b = 0; b < ecs::kMaxBursts && b < static_cast<int>(arr.size());
             ++b) {
            const json& e = arr[static_cast<size_t>(b)];
            if (!e.is_object()) continue;
            p.bursts[b].time = e.value("time", p.bursts[b].time);
            p.bursts[b].count = e.value("count", p.bursts[b].count);
            p.bursts[b].cycles = e.value("cycles", p.bursts[b].cycles);
            p.bursts[b].interval = e.value("interval", p.bursts[b].interval);
        }
    } else if (j.contains("burst")) {
        p.bursts[0].time = 0.0f;
        p.bursts[0].count = j.value("burst", 0);
        p.bursts[0].cycles = 1;
        p.bursts[0].interval = 0.0f;
    }
    // ---- Shape ----
    p.shape = static_cast<ecs::ParticleShape>(
        j.value("shape", static_cast<int>(p.shape)));
    p.shapeExtents = vec3FromJson(j.contains("shapeExtents")
                                      ? j["shapeExtents"] : null,
                                  p.shapeExtents);
    p.shapeRadius = j.value("shapeRadius", p.shapeRadius);
    p.radiusThickness = j.value("radiusThickness", p.radiusThickness);
    p.arc = j.value("arc", p.arc);
    p.shapeRotation = j.value("shapeRotation", p.shapeRotation);
    p.emitFrom = static_cast<ecs::ParticleEmitFrom>(
        j.value("emitFrom", static_cast<int>(p.emitFrom)));
    // ---- Velocity over Lifetime ----
    p.velOverLifeEnabled = j.value("velOverLifeEnabled", p.velOverLifeEnabled);
    p.velOverLifeX = vec4FromJson(
        j.contains("velOverLifeX") ? j["velOverLifeX"] : null, p.velOverLifeX);
    p.velOverLifeY = vec4FromJson(
        j.contains("velOverLifeY") ? j["velOverLifeY"] : null, p.velOverLifeY);
    p.velOverLifeZ = vec4FromJson(
        j.contains("velOverLifeZ") ? j["velOverLifeZ"] : null, p.velOverLifeZ);
    p.velOverLifeWorldSpace =
        j.value("velOverLifeWorldSpace", p.velOverLifeWorldSpace);
    p.orbital = vec3FromJson(j.contains("orbital") ? j["orbital"] : null,
                             p.orbital);
    p.orbitalOffset = j.value("orbitalOffset", p.orbitalOffset);
    p.radial = j.value("radial", p.radial);
    p.speedModifier = vec4FromJson(
        j.contains("speedModifier") ? j["speedModifier"] : null,
        p.speedModifier);
    // ---- Limit Velocity ----
    p.limitVelEnabled = j.value("limitVelEnabled", p.limitVelEnabled);
    p.limitSpeed = j.value("limitSpeed", p.limitSpeed);
    p.limitDampen = j.value("limitDampen", p.limitDampen);
    // ---- Force ----
    p.forceEnabled = j.value("forceEnabled", p.forceEnabled);
    p.force = vec3FromJson(j.contains("force") ? j["force"] : null, p.force);
    p.forceRandomize = j.value("forceRandomize", p.forceRandomize);
    // ---- Rotation over Lifetime ----
    p.startRotation = j.value("startRotation", p.startRotation);
    p.startRotationVariance =
        j.value("startRotationVariance", p.startRotationVariance);
    p.angularVelocity = j.value("angularVelocity", p.angularVelocity);
    p.angularVelocityVariance =
        j.value("angularVelocityVariance", p.angularVelocityVariance);
    // ---- Noise ----
    p.noiseEnabled = j.value("noiseEnabled", p.noiseEnabled);
    p.noiseStrength = j.value("noiseStrength", p.noiseStrength);
    p.noiseFrequency = j.value("noiseFrequency", p.noiseFrequency);
    p.noiseScrollSpeed = j.value("noiseScrollSpeed", p.noiseScrollSpeed);
    p.noiseDamping = j.value("noiseDamping", p.noiseDamping);
    p.noiseOctaves = j.value("noiseOctaves", p.noiseOctaves);
    // ---- Physics ----
    p.gravity = vec3FromJson(j.contains("gravity") ? j["gravity"] : null,
                             p.gravity);
    p.drag = j.value("drag", p.drag);
    p.simulationSpace = static_cast<ecs::ParticleSimulationSpace>(
        j.value("simulationSpace", static_cast<int>(p.simulationSpace)));
    // ---- Size / Color over Lifetime ----
    p.sizeOverLife = vec4FromJson(
        j.contains("sizeOverLife") ? j["sizeOverLife"] : null, p.sizeOverLife);
    p.alphaOverLife = vec4FromJson(
        j.contains("alphaOverLife") ? j["alphaOverLife"] : null,
        p.alphaOverLife);
    if (j.contains("colorRamp") && j["colorRamp"].is_array() &&
        !j["colorRamp"].empty()) {
        const json& ramp = j["colorRamp"];
        const int n = glm::clamp(
            j.value("colorRampCount", static_cast<int>(ramp.size())), 1,
            ecs::kMaxColorStops);
        for (int s = 0; s < n; ++s) {
            const size_t idx = static_cast<size_t>(
                s < static_cast<int>(ramp.size()) ? s : ramp.size() - 1);
            const json& e = ramp[idx];
            glm::vec3 c(1.0f);
            if (e.is_object()) {
                c = vec3FromJson(e.contains("color") ? e["color"] : null, c);
                p.colorRamp.stops[s].w =
                    e.value("t", p.colorRamp.stops[s].w);
            }
            p.colorRamp.stops[s] = glm::vec4(c, p.colorRamp.stops[s].w);
        }
        p.colorRamp.count = n;
    } else {
        // 老格式：三段固定颜色 → 塞进渐变带（t = 0 / 0.5 / 1）
        const glm::vec3 c0 = vec3FromJson(
            j.contains("colorStart") ? j["colorStart"] : null,
            glm::vec3(1.0f, 0.75f, 0.25f));
        const glm::vec3 c1 = vec3FromJson(
            j.contains("colorMid") ? j["colorMid"] : null,
            glm::vec3(1.0f, 0.35f, 0.05f));
        const glm::vec3 c2 = vec3FromJson(
            j.contains("colorEnd") ? j["colorEnd"] : null,
            glm::vec3(0.25f, 0.04f, 0.02f));
        p.colorRamp.count = 3;
        p.colorRamp.stops[0] = glm::vec4(c0, 0.0f);
        p.colorRamp.stops[1] = glm::vec4(c1, 0.5f);
        p.colorRamp.stops[2] = glm::vec4(c2, 1.0f);
    }
    p.colorRamp.sort();
    // ---- Renderer ----
    p.startSize = j.value("startSize", p.startSize);
    // 贴图形状（ParticleSprite）。老场景没有这个键 → 保持默认 Disc，
    // 也就是**逐像素不变**。
    p.sprite = static_cast<ecs::ParticleSprite>(
        glm::clamp(j.value("sprite", 0), 0,
                   static_cast<int>(ecs::ParticleSprite::Count) - 1));
    p.blend = static_cast<ecs::ParticleBlend>(
        j.value("blend", static_cast<int>(p.blend)));
    p.renderMode = static_cast<ecs::ParticleRenderMode>(
        j.value("renderMode", static_cast<int>(p.renderMode)));
    p.sortMode = static_cast<ecs::ParticleSortMode>(
        j.value("sortMode", static_cast<int>(p.sortMode)));
    p.stretchScale = j.value("stretchScale", p.stretchScale);
    p.emissive = j.value("emissive", p.emissive);
    p.softParticles = j.value("softParticles", p.softParticles);
    p.softDistance = j.value("softDistance", p.softDistance);
    p.enabled = j.value("enabled", true);
    return p;
}

} // namespace scene
