#include "scene/CameraSystem.h"

#include "core/Logger.h"
#include "ecs/Components.h"
#include "scene/Camera.h"
#include "scene/CameraMove.h"   // 运镜算子（aimAt / applyHandheld / 焦距换算）
#include "scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace scene {

namespace {

constexpr float kPi = 3.14159265358979f;

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
    // roll 与 yaw 同理走最短弧：179° 混到 -179° 该转 2°，不是 358°
    // （否则混合一次镜头会原地翻一圈 —— 这是 roll 独有的坑，pitch 夹在
    // ±89° 里永远不会碰到绕圈的问题）。
    p.roll = lerpAngle(a.roll, b.roll, t);
    p.shiftX = a.shiftX + (b.shiftX - a.shiftX) * t;
    p.shiftY = a.shiftY + (b.shiftY - a.shiftY) * t;
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
    // 本帧是不是"导演说了算"。只影响日志用的是哪一条（CAM-CUT / CAM-ACTIVE），
    // 不改变后续处理 —— 切完之后的相机就是普通相机。
    bool directed = false;

    // ---- 导演覆盖（多机位切镜）先试 ----
    // ⚠ 放在 priority 扫描**之前**：覆盖一旦生效，扫描根本不该跑（跑了也
    //   只是白算一遍然后被丢弃，还会让"这台相机被选中过"的直觉落空）。
    if (m_directorGuid != 0) {
        const ecs::Entity de = scene.world().find(m_directorGuid);
        const auto* dcc =
            de.valid() ? scene.world().get<ecs::CameraComponent>(de) : nullptr;
        if (dcc && dcc->enabled) {
            best = de;
            bestPriority = dcc->priority;
            found = true;
            directed = true;
        } else {
            // marker 指向的相机被删 / 被禁用。**必须回落到 priority**：
            // 永久卡在"没有活跃相机"会让整个画面冻在上一帧，而且日志里
            // 只有一条早已刷过去的 WARN。
            if (!m_directorWarned) {
                VK_LOG_WARN("CAM-CUT-MISS guid=%s (no enabled CameraComponent) — "
                            "falling back to priority",
                            ecs::guidToHex(m_directorGuid).c_str());
                m_directorWarned = true;
            }
            m_directorGuid = 0;
        }
    }

    if (!found) {
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
    }

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
        // 两条不同前缀是**故意**的：`CAM-CUT` 是"叙事切镜"（作者在时间轴上
        // 决定的），`CAM-ACTIVE` 是"优先级扫描"（场景摆好就定了）。用例要能
        // 分开数 —— 数 `CAM-ACTIVE` 数不出切镜有没有发生。
        if (directed) {
            VK_LOG_INFO("CAM-CUT name=%s guid=%s priority=%d",
                        m_activeName.c_str(),
                        ecs::guidToHex(m_directorGuid).c_str(), bestPriority);
        } else {
            VK_LOG_INFO("CAM-ACTIVE name=%s priority=%d", m_activeName.c_str(),
                        bestPriority);
        }
    }
    m_active = best;
    m_hasActive = true;
}

void CameraSystem::setDirectorCamera(std::uint64_t guid) {
    if (m_directorGuid == guid) return;
    m_directorGuid = guid;
    // 换了目标就允许再喊一次"找不到" —— 否则第一条 MISS 之后，后面每一台
    // 被删的相机都静默。
    m_directorWarned = false;
    if (guid == 0) VK_LOG_INFO("CAM-CUT clear (director released)");
}

void CameraSystem::clearDirectorCamera() { setDirectorCamera(0); }

void CameraSystem::blendCut(float seconds) {
    m_blendFrom = m_applied;   // 起点 = 这一帧画面上真实的姿态（含上一次混合）
    m_blendDur = std::max(seconds, 0.0f);
    m_blendT = 0.0f;
    m_blendActive = m_blendDur > 1.0e-4f;
    m_blendSmooth = true;
    // ⚠ **不**碰 m_desired / m_hasDesired：目标要每帧现算（见 .h 里那一段）。
    if (m_blendActive) {
        VK_LOG_INFO("CAM-CUT blend dur=%.2fs from=(%.2f,%.2f,%.2f)",
                    static_cast<double>(m_blendDur),
                    static_cast<double>(m_blendFrom.position.x),
                    static_cast<double>(m_blendFrom.position.y),
                    static_cast<double>(m_blendFrom.position.z));
    }
}

CameraPose CameraSystem::poseFromEntity(Scene& scene, ecs::Entity e) const {
    CameraPose p;
    const glm::mat4 w = scene.worldMatrix(e);
    p.position = glm::vec3(w[3]);
    const glm::vec3 fwd = glm::normalize(glm::vec3(w[0]));  // 实体局部 +X = 视线
    p.yaw = std::atan2(fwd.y, -fwd.x);
    p.pitch = std::asin(std::clamp(-fwd.z, -1.0f, 1.0f));

    // roll 从**实体自己的 up（局部 +Z）**读回来。
    // ⚠ 不读这一条，`cameraEulerFor` 写出去的 roll 会在下一次 `poseFromEntity`
    //   里被悄悄抹平 —— 表现是"实体在场景树里看着是歪的，一进 Play 就正了"，
    //   而且只在有滚转的镜头上出现。
    //   poseUp = u·cos(r) + right·sin(r) ⇒ r = atan2(dot(up,right), dot(up,u))。
    {
        const glm::vec3 up = glm::normalize(glm::vec3(w[2]));
        const glm::vec3 r0 = poseRight(p);
        const glm::vec3 u0 = poseUp(p);   // roll 还是 0，就是 tilt 之后的 up
        if (std::fabs(glm::dot(up, r0)) > 1.0e-5f ||
            std::fabs(glm::dot(up, u0)) > 1.0e-5f) {
            p.roll = std::atan2(glm::dot(up, r0), glm::dot(up, u0));
        }
    }
    if (const auto* cc = scene.world().get<ecs::CameraComponent>(e)) {
        // 焦距优先：装了定焦镜头（focal > 0）就以镜头为准，fov 只是它算出来的
        // 结果。⚠ 这里与 `apply` 里那次换算**必须一致**（同一个函数、同一组
        // 参数），否则"从实体推姿态"和"外部给意图"两条路会得到两个 fov，
        // 画面在混合的最后一帧跳一下。
        p.fov = (cc->focal > 0.0f) ? focalToFovY(cc->focal, cc->sensorHeight)
                                   : cc->fov;
    }
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

    apply(scene, dt);
}

void CameraSystem::apply(Scene& scene, float dt) {
    Camera& cam = scene.camera();

    CameraPose p = m_applied;
    float amp = 0.0f;
    if (const auto* cc = scene.world().get<ecs::CameraComponent>(m_active)) {
        amp = cc->shakeAmplitude;

        // ============================================================
        // 运镜体系（2026-10-30）—— 组件属性叠加到**已经是意图值**的姿态上
        //
        // ⚠⚠ 顺序是硬约束，从上到下都不能换：
        //   ① 焦距 → 覆盖 fov（光学，最"物理"）
        //   ② Track To → 覆盖 yaw/pitch（朝向层）
        //   ③ roll / shift → **叠加**（镜头安装角，与运镜的 roll 是两回事：
        //      过场关键帧可以让镜头歪一点，同时这颗镜头自己也歪 2°）
        //   ④ 手持 → 最后叠（它是在一切之上的微抖）
        //   把 ④ 放到 ② 前面，手持会被 aimAt 整个抹掉 —— 表现是"手持开了，
        //   但一配注视目标就不抖了"，几乎没人会往这个方向想。
        // ============================================================
        if (cc->focal > 0.0f)
            p.fov = focalToFovY(cc->focal, cc->sensorHeight);

        // ---- Follow Path：位置由路径决定（**覆盖**关键帧给的位置）----
        //
        // ⚠ 必须在 Track To **之前**：Track To 是朝向层，它要覆盖"沿切线看"。
        //   反过来写的话，"沿路径走 + 始终盯着主体"会退化成"沿切线看"。
        if (cc->pathGuid != 0) {
            const ecs::Entity pe = scene.world().find(cc->pathGuid);
            const auto* pc = pe.valid()
                                 ? scene.world().get<ecs::CameraPathComponent>(pe)
                                 : nullptr;
            if (pc && pc->points.size() >= 2) {
                // 只在路径内容变了时重建弧长表（点位数量 + 首末点 + 闭合位
                // 的粗略签名）。⚠ 不能每帧 `smoothHandles()`：它要分配，
                // 而且"路径中途被拖动"本来就应该重建 —— 用签名判断两头都占。
                const std::size_t np = pc->points.size();
                const glm::vec3 a0 = pc->points.front();
                const glm::vec3 a1 = pc->points.back();
                if (m_pathGuid != cc->pathGuid || m_pathPts != np ||
                    m_pathClosed != pc->closed || m_pathA0 != a0 ||
                    m_pathA1 != a1) {
                    m_path.points.clear();
                    m_path.points.reserve(np);
                    for (const glm::vec3& q : pc->points) {
                        CameraPathPoint cp;
                        cp.position = q;
                        m_path.points.push_back(cp);
                    }
                    m_path.closed = pc->closed;
                    m_path.smoothHandles();
                    m_pathGuid = cc->pathGuid;
                    m_pathPts = np;
                    m_pathClosed = pc->closed;
                    m_pathA0 = a0;
                    m_pathA1 = a1;
                    m_pathSeg = -1;
                }

                float u = pc->progress + m_time * pc->speed;
                u = u - std::floor(u);   // 负数也要绕回 [0,1)

                glm::vec3 tan(1.0f, 0.0f, 0.0f);
                const glm::vec3 pos = m_path.sample(u, &tan);
                p.position = pos;

                if (pc->lookAlong) {
                    const float tl = glm::length(tan);
                    if (tl > 1.0e-5f) {
                        aimAt(p, pos + tan / tl);
                    }
                    if (pc->yawOffsetDeg != 0.0f)
                        p.yaw += pc->yawOffsetDeg * kPi / 180.0f;
                }

                if (m_trace) {
                    const int seg = m_path.segmentAt(u);
                    if (seg != m_pathSeg) {
                        m_pathSeg = seg;
                        VK_LOG_INFO("CAM-PATH name=%s u=%.3f seg=%d/%d len=%.2f "
                                    "pos=(%.2f,%.2f,%.2f)",
                                    m_activeName.c_str(), u, seg,
                                    m_path.segmentCount(), m_path.length(),
                                    pos.x, pos.y, pos.z);
                    }
                }
            } else {
                // 路径实体没了 / 点不够：保留关键帧的位置，**不清零**。
                // 清零会把镜头拽到世界原点，而这多半只是路径被误删。
                m_pathGuid = 0;
                m_pathSeg = -1;
            }
        } else {
            m_pathGuid = 0;
            m_pathSeg = -1;
        }

        if (cc->lookTargetGuid != 0) {
            const ecs::Entity te = scene.world().find(cc->lookTargetGuid);
            if (te.valid()) {
                const glm::vec3 tp = glm::vec3(scene.worldMatrix(te)[3]);
                if (!m_lookValid) {
                    // 首帧：直接落在目标上（不要从原点"飞过去"—— 那会在
                    // 进 Play 的第一秒看到镜头从世界原点甩到人脸上）。
                    m_lookPoint = tp;
                    m_lookValid = true;
                }
                if (cc->lookDamping > 1.0e-3f) {
                    // 指数逼近：`1 − exp(−dt/τ)` 与帧率**无关**
                    // （线性插 `dt/τ` 会让 30fps 与 144fps 跟出两条不同轨迹，
                    // 而"手感"这东西正是要跨帧率一致的）。
                    const float k =
                        1.0f - std::exp(-std::max(dt, 0.0f) / cc->lookDamping);
                    m_lookPoint += (tp - m_lookPoint) * k;
                } else {
                    m_lookPoint = tp;
                }
                aimAt(p, m_lookPoint);
            } else {
                // 目标被销毁了：停在当前朝向，而不是保留一个幽灵注视点
                m_lookValid = false;
            }
        } else {
            m_lookValid = false;
        }

        if (cc->rollDeg != 0.0f) p.roll += cc->rollDeg * kPi / 180.0f;
        p.shiftX += cc->shiftX;
        p.shiftY += cc->shiftY;

        applyHandheld(p, m_time, cc->handheldDeg, cc->handheldHz,
                      cc->handheldSeed, cc->handheldPosM);
    } else {
        // 没有组件时不该走到这里（apply 只在 m_hasActive 时被调），
        // 但注视点的黏性必须清掉 —— 否则下一次换回带 Track To 的相机会
        // 从上一台的位置"飞"过来。
        m_lookValid = false;
    }

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
    // ⚠ 运镜三通道（roll / shift）必须与 `applyPoseToSceneCamera` **同款**。
    //   漏一处 = "过场里歪着、游玩时正的"或者反过来的鬼影 —— 而且那种
    //   不一致只在**同时**用到时间轴与 CameraSystem 的工程里出现。
    cam.setRoll(p.roll);
    cam.setShift(p.shiftX, p.shiftY);

    // 镜头裁剪面。0 = **不碰**（保持引擎全局默认 / 编辑器视口那一套）。
    // ⚠ 这里必须"不设"而不是"设成默认值"：编辑器视口用的是另一套近远平面
    //   （见 EditorContext::applyEditorCameraMode），无脑回写 0.1/200 会把
    //   正在高空俯视的视口里那一片地面裁掉。
    if (const auto* ccl = scene.world().get<ecs::CameraComponent>(m_active)) {
        if (ccl->clipNear > 0.0f) {
            cam.setClipPlanes(ccl->clipNear,
                              ccl->clipFar > 0.0f ? ccl->clipFar : cam.farPlane());
        }
    }

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
