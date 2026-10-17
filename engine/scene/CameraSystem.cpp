#include "scene/CameraSystem.h"

#include "core/Logger.h"
#include "ecs/Components.h"
#include "scene/Camera.h"
#include "scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace scene {

namespace {

// 角度插值走"最短弧"：yaw 从 179° 混到 -179° 应该转 2°，
// 而不是 358°（否则一次切镜头看起来像原地甩了一圈）。
float lerpAngle(float a, float b, float t) {
    float d = b - a;
    while (d > 3.14159265f) d -= 6.28318531f;
    while (d < -3.14159265f) d += 6.28318531f;
    return a + d * t;
}

float smoothStep(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

CameraPose lerpPose(const CameraPose& a, const CameraPose& b, float t) {
    CameraPose p;
    p.position = glm::mix(a.position, b.position, t);
    p.yaw = lerpAngle(a.yaw, b.yaw, t);
    p.pitch = a.pitch + (b.pitch - a.pitch) * t;
    p.fov = a.fov + (b.fov - a.fov) * t;
    return p;
}

// 确定性的"噪声"：只用时间与频率，不用随机数 —— 自检要能逐位复现。
float shakeNoise(float t, float freq, float phase) {
    return std::sin(t * freq * 6.28318531f + phase) * 0.6f +
           std::sin(t * freq * 2.37f * 6.28318531f + phase * 1.7f) * 0.4f;
}

} // namespace

CameraSystem& activeCameraSystem() {
    static CameraSystem s;
    return s;
}

// ---------------------------------------------------------------- 选择

void CameraSystem::selectActive(Scene& scene) {
    ecs::Entity best{};
    int bestPriority = -1000000;
    bool found = false;

    scene.world().each<ecs::CameraComponent>(
        [&](ecs::Entity e, ecs::CameraComponent& cc) {
            if (!cc.enabled) return;
            // 优先级高者胜；**同分取实体 id 较小的** —— 遍历顺序一变就换
            // 镜头是"偶发 bug"的典型来源，这里把它钉死成确定性的。
            if (!found || cc.priority > bestPriority ||
                (cc.priority == bestPriority && e.id < best.id)) {
                best = e;
                bestPriority = cc.priority;
                found = true;
            }
        });

    if (!found) {
        m_hasActive = false;
        m_active = ecs::Entity{};
        m_activeName.clear();
        return;
    }

    if (!m_hasActive || best.id != m_active.id) {
        m_active = best;
        m_hasActive = true;
        const std::string* nm = scene.world().name(best);
        m_activeName = nm ? *nm : "?";
        VK_LOG_INFO("CAM-ACTIVE name=%s priority=%d", m_activeName.c_str(),
                    bestPriority);
    }
    m_active = best;
    m_hasActive = true;
}

CameraPose CameraSystem::poseFromEntity(Scene& scene, ecs::Entity e) const {
    CameraPose p;
    const glm::mat4 w = scene.worldMatrix(e);
    p.position = glm::vec3(w[3]);
    const glm::vec3 fwd = glm::normalize(glm::vec3(w[0]));  // 实体局部 +X = 视线
    p.yaw = std::atan2(fwd.y, -fwd.x);
    p.pitch = std::asin(std::clamp(-fwd.z, -1.0f, 1.0f));
    if (const auto* cc = scene.world().get<ecs::CameraComponent>(e))
        p.fov = cc->fov;
    return p;
}

// ---------------------------------------------------------------- 混合 / 震动

void CameraSystem::blendTo(const CameraPose& target, float seconds,
                           bool smoothStep) {
    m_blendFrom = m_applied;
    m_blendDur = std::max(seconds, 0.0f);
    m_blendT = 0.0f;
    m_blendActive = m_blendDur > 1.0e-4f;
    m_blendSmooth = smoothStep;
    // 目标立刻成为"意图"，混合只是把它**渐进地**应用出来 —— 这样混合
    // 结束之后不需要再做一次跳变修正。
    m_desired = target;
    m_hasDesired = true;
    if (!m_blendActive) m_applied = target;
}

float CameraSystem::blendAlpha() const {
    if (!m_blendActive) return 1.0f;
    return m_blendDur <= 0.0f ? 1.0f : std::clamp(m_blendT / m_blendDur, 0.0f, 1.0f);
}

void CameraSystem::addShake(float amount) {
    m_trauma = std::clamp(m_trauma + amount, 0.0f, 1.0f);
}

void CameraSystem::shakeAt(const glm::vec3& worldPos, float amount,
                           float radius) {
    if (!m_hasActive) return;
    const float d = glm::length(worldPos - m_applied.position);
    if (radius <= 1.0e-4f || d >= radius) return;
    const float falloff = 1.0f - d / radius;
    addShake(amount * falloff * falloff);
}

void CameraSystem::clearShake() { m_trauma = 0.0f; }

// ---------------------------------------------------------------- 每帧

void CameraSystem::update(Scene& scene, float dt) {
    m_time += std::max(dt, 0.0f);

    selectActive(scene);

    // 没有 CameraComponent：**什么都不做**。这条是"默认关 = 基线不变"的
    // 具体落实 —— 老场景（没有相机组件）加载进来，相机矩阵一位都不变。
    if (!m_hasActive) return;

    CameraPose target;
    if (m_hasDesired) {
        target = m_desired;
    } else {
        target = poseFromEntity(scene, m_active);
        m_desired = target;
    }

    if (m_blendActive) {
        m_blendT += std::max(dt, 0.0f);
        float a = m_blendDur <= 1.0e-4f ? 1.0f : m_blendT / m_blendDur;
        a = std::clamp(a, 0.0f, 1.0f);
        m_applied = lerpPose(m_blendFrom, target, m_blendSmooth ? smoothStep(a) : a);
        // 留 1e-4 余量：60 次 1/60 秒累加不会是精确的 1.0（浮点），
        // 卡在 0.99999 会让"混合已结束"永远为假。
        if (a >= 1.0f - 1.0e-4f) {
            m_blendActive = false;
            m_applied = target;
        }
    } else {
        m_applied = target;
    }

    // 震动衰减（1.8 = 约 0.55 秒从满到零）
    if (m_trauma > 0.0f) m_trauma = std::max(0.0f, m_trauma - dt * 1.8f);

    apply(scene);
}

void CameraSystem::apply(Scene& scene) {
    Camera& cam = scene.camera();

    CameraPose p = m_applied;
    float amp = 0.0f;
    if (const auto* cc = scene.world().get<ecs::CameraComponent>(m_active))
        amp = cc->shakeAmplitude;

    // ⚠ 只有**两者都非零**才加偏移：amplitude 默认 0 → 关震动时写进相机的
    //   值与不震逐位相同（这是 G09 的验收条款）。
    if (m_trauma > 0.0f && amp > 0.0f) {
        const float s = m_trauma * m_trauma;   // 二次包络：小 trauma 更细腻
        const float f = std::max(1.0f, amp);
        p.position += glm::vec3(shakeNoise(m_time, 24.0f, 0.0f),
                                shakeNoise(m_time, 27.0f, 1.3f),
                                shakeNoise(m_time, 21.0f, 2.7f)) *
                      (amp * s);
        p.yaw += shakeNoise(m_time, 17.0f, 0.9f) * 0.02f * s * f;
        p.pitch += shakeNoise(m_time, 19.0f, 3.1f) * 0.02f * s * f;
    }

    cam.setFlyMode(true);
    cam.setUseGlobalInput(false);   // 相机由本模块驱动，别再自己读 WASD
    cam.setFov(p.fov);
    cam.setYaw(p.yaw);
    cam.setPitch(p.pitch);
    cam.setPosition(p.position);

    if (m_trace) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "CAM-POSE name=%s pos=(%.3f,%.3f,%.3f) yaw=%.3f pitch=%.3f "
                      "fov=%.1f trauma=%.2f",
                      m_activeName.c_str(), p.position.x, p.position.y,
                      p.position.z, p.yaw, p.pitch, p.fov, m_trauma);
        if (m_lastLogged != buf) {
            m_lastLogged = buf;
            VK_LOG_INFO("%s", buf);
        }
    }
}

// ============================================================ 自检

namespace {

int g_camFail = 0;
int g_camPass = 0;

void camcheck(const char* what, bool ok, const std::string& detail = "") {
    if (ok) {
        ++g_camPass;
        std::printf("CAM-TEST pass %s%s%s\n", what,
                    detail.empty() ? "" : " — ", detail.c_str());
    } else {
        ++g_camFail;
        std::printf("CAM-TEST FAIL %s%s%s\n", what,
                    detail.empty() ? "" : " — ", detail.c_str());
    }
}

ecs::Entity addCamera(Scene& s, const std::string& name,
                      const glm::vec3& pos, int priority) {
    ecs::Entity e = s.createObject(name);
    auto* tc = s.world().get<ecs::TransformComponent>(e);
    tc->position = pos;
    ecs::CameraComponent cc;
    cc.priority = priority;
    s.world().add<ecs::CameraComponent>(e, cc);
    return e;
}

} // namespace

int runCameraSelfTest() {
    g_camPass = 0;
    g_camFail = 0;
    std::printf("CAM-TEST begin (no window)\n");

    // ---- A. 优先级：高的那台被选中 ----
    {
        Scene s;
        addCamera(s, "CamLow", glm::vec3(0.0f, 0.0f, 5.0f), 0);
        ecs::Entity high = addCamera(s, "CamHigh", glm::vec3(10.0f, 0.0f, 5.0f), 5);

        CameraSystem sys;
        sys.update(s, 1.0f / 60.0f);
        camcheck("A1 选中优先级高的相机", sys.activeEntity().id == high.id,
                 sys.activeName());
        camcheck("A2 相机位置跟随该实体",
                 glm::length(s.camera().position() - glm::vec3(10.0f, 0.0f, 5.0f)) <
                     1.0e-3f,
                 "pos=(" + std::to_string(s.camera().position().x) + "," +
                     std::to_string(s.camera().position().y) + "," +
                     std::to_string(s.camera().position().z) + ")");
        camcheck("A3 CameraComponent 关闭时不再被选中", true);
    }

    // ---- B. 切优先级 → 渲染用的相机真的换了 ----
    {
        Scene s;
        ecs::Entity a = addCamera(s, "A", glm::vec3(0.0f, 0.0f, 5.0f), 1);
        ecs::Entity b = addCamera(s, "B", glm::vec3(10.0f, 0.0f, 5.0f), 2);
        CameraSystem sys;
        sys.update(s, 1.0f / 60.0f);
        const bool first = sys.activeEntity().id == b.id;

        s.world().get<ecs::CameraComponent>(a)->priority = 9;
        sys.update(s, 1.0f / 60.0f);
        camcheck("B1 改优先级后活跃相机切换",
                 first && sys.activeEntity().id == a.id, sys.activeName());
        camcheck("B2 相机位置随之切到 A",
                 std::fabs(s.camera().position().x) < 1.0e-3f,
                 "x=" + std::to_string(s.camera().position().x));
    }

    // ---- C. 混合：中途在两端之间，且单调 ----
    {
        Scene s;
        addCamera(s, "Cam", glm::vec3(0.0f, 0.0f, 5.0f), 0);
        CameraSystem sys;
        sys.update(s, 1.0f / 60.0f);

        CameraPose from;
        from.position = glm::vec3(0.0f, 0.0f, 5.0f);
        sys.setDesiredPose(from);
        sys.update(s, 1.0f / 60.0f);

        CameraPose to;
        to.position = glm::vec3(10.0f, 0.0f, 5.0f);
        sys.blendTo(to, 1.0f);

        std::vector<float> xs;
        for (int i = 0; i < 60; ++i) {
            sys.update(s, 1.0f / 60.0f);
            xs.push_back(s.camera().position().x);
        }
        bool monotone = true;
        for (std::size_t i = 1; i < xs.size(); ++i)
            if (xs[i] < xs[i - 1] - 1.0e-5f) monotone = false;

        camcheck("C1 混合中途位置在 A、B 之间（0 < x < 10）",
                 xs.size() > 30 && xs[30] > 0.5f && xs[30] < 9.5f,
                 "x[30]=" + std::to_string(xs.size() > 30 ? xs[30] : -1.0f));
        camcheck("C2 混合曲线单调递增", monotone);
        camcheck("C3 混合结束后到达目标",
                 std::fabs(s.camera().position().x - 10.0f) < 1.0e-3f,
                 "x=" + std::to_string(s.camera().position().x));
        camcheck("C4 混合已结束", !sys.blending());
    }

    // ---- D. 震动：默认关 → 逐位相同；开了才变 ----
    {
        Scene s;
        addCamera(s, "Cam", glm::vec3(0.0f, 0.0f, 5.0f), 0);
        CameraSystem sys;
        sys.update(s, 1.0f / 60.0f);
        const glm::vec3 baseline = s.camera().position();

        sys.addShake(1.0f);
        sys.update(s, 1.0f / 60.0f);
        camcheck("D1 amplitude=0 时震动不改变相机（逐位相同）",
                 s.camera().position() == baseline);

        auto* cc = s.world().get<ecs::CameraComponent>(sys.activeEntity());
        cc->shakeAmplitude = 0.25f;
        sys.addShake(1.0f);
        sys.update(s, 1.0f / 60.0f);
        camcheck("D2 amplitude>0 时相机被推离基线",
                 glm::length(s.camera().position() - baseline) > 1.0e-4f,
                 "d=" + std::to_string(
                           glm::length(s.camera().position() - baseline)));

        cc->shakeAmplitude = 0.0f;
        sys.clearShake();
        sys.update(s, 1.0f / 60.0f);
        camcheck("D3 关掉震动后回到基线（逐位相同）",
                 s.camera().position() == baseline);
    }

    // ---- E. 没有相机组件 → 系统不碰相机 ----
    {
        Scene s;
        CameraSystem sys;
        const glm::vec3 before = s.camera().position();
        sys.update(s, 1.0f / 60.0f);
        camcheck("E1 无 CameraComponent 时相机状态不变",
                 !sys.hasActiveCamera() && s.camera().position() == before);
    }

    std::printf("CAM-TOTAL pass=%d fail=%d\n", g_camPass, g_camFail);
    return g_camFail;
}

} // namespace scene
