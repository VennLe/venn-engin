// ============================================================
// character/ViewRigSelfTest.cpp
// ============================================================

#include "character/ViewRigSelfTest.h"

#include "character/ViewRig.h"

#include "assets/Model.h"
#include "assets/Skeleton.h"
#include "core/Logger.h"
#include "ecs/Components.h"
#include "scene/CameraSystem.h"
#include "scene/Scene.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace character {

namespace {

int g_pass = 0;
int g_fail = 0;

// 输出格式与 `animation/AnimationSelfTest.cpp` 完全一致（同一套脚本习惯）。
// ⚠ detail 里**不能有空格**：工具用 `detail=(\S*)` 解析。
void report(const char* name, double actual, double expected, double tol,
            const std::string& detail = {}) {
    const bool ok = std::fabs(actual - expected) <= tol;
    VK_LOG_INFO("VIEWRIG-TEST %s actual=%.6f expected=%.6f tol=%.6f "
                "detail=%s result=%s",
                name, actual, expected, tol, detail.c_str(),
                ok ? "PASS" : "FAIL");
    if (ok) ++g_pass; else ++g_fail;
}

void reportFlag(const char* name, bool ok, const std::string& detail = {}) {
    VK_LOG_INFO("VIEWRIG-TEST %s actual=%d expected=1 tol=0 detail=%s "
                "result=%s",
                name, ok ? 1 : 0, detail.c_str(), ok ? "PASS" : "FAIL");
    if (ok) ++g_pass; else ++g_fail;
}

// ------------------------------------------------------------
// 手工搭一具"两根骨"的骨架：root 在原点、head 在 (0,0,1.6)。
//
// 与 `AnimationSelfTest` 同一条原则：**不加载资产**（Mesh 是 GPU 资源），
// 期望值因此可以手算。这里连 Model 都只塞一个骨架 —— `resolveEye` 需要
// 的只有 `model->skeleton` 与 `sk.jointMatrices`。
// ------------------------------------------------------------
struct RigFixture {
    scene::Scene scene;
    ecs::Entity player;
    ecs::Entity mesh;
    assets::Model model;

    explicit RigFixture(const std::string& headName = "Head") {
        auto skel = std::make_shared<assets::Skeleton>();
        assets::Skeleton::Joint root;
        root.name = "Root";
        root.parent = -1;
        root.restLocal = glm::mat4(1.0f);
        assets::Skeleton::Joint head;
        head.name = headName;
        head.parent = 0;
        head.restLocal = glm::translate(glm::mat4(1.0f),
                                        glm::vec3(0.0f, 0.0f, 1.6f));
        skel->joints.push_back(root);
        skel->joints.push_back(head);
        skel->topoOrder = {0, 1};
        model.name = "selftest";
        model.skeleton = skel;

        player = scene.createObject("Player");
        mesh = scene.createObject("Body");
        scene.setParent(mesh, player);

        ecs::SkinnedMeshComponent sk;
        sk.model = &model;
        sk.jointMatrices.assign(2, glm::mat4(1.0f));
        // jointMat = invMeshNode · boneWorld · invBind。这里三个里两个是
        // 单位阵，所以"头骨在世界里的位置"直接等于矩阵的平移部分。
        sk.jointMatrices[1] = glm::translate(glm::mat4(1.0f),
                                             glm::vec3(0.0f, 0.0f, 1.55f));
        scene.world().add<ecs::SkinnedMeshComponent>(player, sk);

        ecs::PlayerComponent pc;
        pc.eyeBone = "head";
        scene.world().add<ecs::PlayerComponent>(player, pc);
    }

    ecs::PlayerComponent& pc() {
        return *scene.world().get<ecs::PlayerComponent>(player);
    }
};

} // namespace

int runViewRigSelfTest() {
    g_pass = 0;
    g_fail = 0;
    VK_LOG_INFO("VIEWRIG-TEST begin (no window)");

    // ---- A. 身体度量：1.8 m 的标准人 ----
    {
        const BodyMetrics m = fitBody(1.8f);
        report("body_radius_1p8", m.radius, 0.288, 1e-6);
        report("body_halfheight_1p8", m.halfHeight, 0.612, 1e-6);
        report("body_capsule_total", 2.0 * (m.halfHeight + m.radius), 1.8, 1e-6);
        report("body_eye_1p8", m.eyeHeight, 1.656, 1e-6);
        report("body_focus_1p8", m.focusHeight, 0.756, 1e-6);
        report("body_camdist_1p8", m.camDistance, 3.6, 1e-6);
        // 跳起 0.39 × 身高 → v0 = √(2·9.81·0.702) = 3.7106
        report("body_jump_1p8", m.jumpSpeed, 3.7105, 0.002);

        // 极小 / 极大：夹住的边界（0.5 m 的玩偶不该拿到 8 cm 的胶囊）
        const BodyMetrics tiny = fitBody(0.5f);
        report("body_radius_min", tiny.radius, 0.15, 1e-6);
        const BodyMetrics giant = fitBody(3.0f);
        report("body_radius_max", giant.radius, 0.48, 1e-6);

        // 身高 <= 0（资产没 bounds）→ 按 1.8 m，而不是 0
        const BodyMetrics none = fitBody(0.0f);
        report("body_zero_height", none.height, 1.8, 1e-6);
    }

    // ---- B. 眼位：来自头骨 ----
    {
        RigFixture f;
        const EyeAnchor eye = resolveEye(f.scene, f.player, f.pc());
        reportFlag("eye_valid", eye.valid);
        reportFlag("eye_from_bone", eye.fromBone, eye.bone);
        // 1.55（头骨原点）+ 0.02（`eyeOffset` 的默认前推）—— 后者是防止
        // "身体可见的第一人称"看到口腔内侧的余量。
        report("eye_height_bone", eye.heightAboveFeet, 1.57, 1e-6);
        report("eye_world_z", eye.position.z, 1.57, 1e-6);
        reportFlag("eye_bone_name", eye.bone == "Head", eye.bone);
    }

    // ---- C. 骨架里没有头骨 → 退回 eyeHeight（不是"没有相机"）----
    {
        RigFixture f("Spine");
        ecs::PlayerComponent& pc = f.pc();
        pc.eyeHeight = 1.44f;
        const EyeAnchor eye = resolveEye(f.scene, f.player, pc);
        reportFlag("eye_fallback_valid", eye.valid);
        reportFlag("eye_fallback_not_bone", !eye.fromBone);
        report("eye_fallback_height", eye.heightAboveFeet, 1.44, 1e-6);
    }

    // ---- D. 眼位跟着玩家转 + 偏移按玩家局部轴套 ----
    {
        RigFixture f;
        auto* tf = f.scene.world().get<ecs::TransformComponent>(f.player);
        tf->position = glm::vec3(10.0f, 2.0f, 0.0f);
        tf->rotation.z = 3.14159265f * 0.5f;   // 绕 Z 转 90°
        ecs::PlayerComponent& pc = f.pc();
        pc.eyeOffset = glm::vec3(0.1f, 0.0f, 0.0f);
        const EyeAnchor eye = resolveEye(f.scene, f.player, pc);
        // 局部 +X 转 90° → 世界 +Y
        report("eye_yaw_offset_x", eye.position.x, 10.0, 1e-5);
        report("eye_yaw_offset_y", eye.position.y, 2.1, 1e-5);
        report("eye_yaw_offset_z", eye.position.z, 1.55, 1e-5);
    }

    // ---- E. 骨架没有蒙皮数据（刚导入 / 占位胶囊）→ 兜底 ----
    {
        scene::Scene s;
        ecs::Entity e = s.createObject("Player");
        ecs::PlayerComponent pc;
        pc.eyeHeight = 1.62f;
        s.world().add<ecs::PlayerComponent>(e, pc);
        const EyeAnchor eye = resolveEye(s, e, pc);
        reportFlag("eye_no_skin_valid", eye.valid);
        report("eye_no_skin_height", eye.heightAboveFeet, 1.62, 1e-6);
    }

    // ---- F. 视线基与 Camera 角度 ----
    {
        glm::vec3 look(0.0f), right(0.0f);
        lookBasis(0.0f, &look, &right);
        report("look_yaw0_x", look.x, 1.0, 1e-6);
        report("look_yaw0_y", look.y, 0.0, 1e-6);
        report("right_yaw0_y", right.y, -1.0, 1e-6);   // 面朝 +X 时右手边是 -Y

        lookBasis(3.14159265f * 0.5f, &look, &right);
        report("look_yaw90_x", look.x, 0.0, 1e-6);
        report("look_yaw90_y", look.y, -1.0, 1e-6);

        float cy = 0.0f, cp = 0.0f;
        cameraAnglesFromLook(0.0f, 0.3f, &cy, &cp);
        report("cam_yaw_from_look0", std::fabs(cy), 3.14159265, 1e-5);
        report("cam_pitch_from_look", cp, -0.3, 1e-6);

        // 视线 yaw=90° → 相机 yaw 落在 -90°（±π 边界由 wrapPi 归一）
        cameraAnglesFromLook(3.14159265f * 0.5f, 0.0f, &cy, &cp);
        report("cam_yaw_from_look90", cy, -3.14159265f * 0.5f, 1e-5);

        report("wrap_pi_pos", std::fabs(wrapPi(3.14159265f * 3.0f)), 3.14159265f,
               1e-5);
        report("wrap_pi_neg", std::fabs(wrapPi(-3.14159265f * 3.0f)),
               3.14159265f, 1e-5);
    }

    // ---- G. 阻尼：帧率无关 ----
    //
    // 同一条指数逼近，用 60 fps 与 144 fps 各跑 0.5 秒，终点必须一致。
    // 换成一阶 lerp（`value += (t-v)*k`）时两者会差 3~5% —— 那正是
    // "同一次转头在两台机器上手感不同"的来源。
    {
        EyeDamp a, b;
        const glm::vec3 target(1.0f, 2.0f, 3.0f);
        const glm::vec3 start(0.0f);
        a.value = start; a.init = true;
        b.value = start; b.init = true;
        for (int i = 0; i < 30; ++i) a.step(target, 1.0f / 60.0f, 0.05f);
        for (int i = 0; i < 72; ++i) b.step(target, 1.0f / 144.0f, 0.05f);
        report("damp_fps_x", a.value.x, b.value.x, 1e-4);
        report("damp_value", a.value.x, 1.0 - std::exp(-0.5f / 0.05f), 2e-3);

        EyeDamp hard;
        hard.step(target, 0.016f, 0.0f);
        report("damp_tau0", hard.value.x, 1.0, 0.0);
        EyeDamp zeroDt;
        zeroDt.value = start; zeroDt.init = true;
        zeroDt.step(target, 0.0f, 0.05f);
        report("damp_dt0_snap", zeroDt.value.x, 1.0, 0.0);
    }

    // ---- H. 过渡：首尾精确 + 最短角差 ----
    {
        scene::CameraPose from;
        from.position = glm::vec3(0.0f);
        from.yaw = 0.0f;
        from.pitch = 0.0f;
        from.fov = 60.0f;
        scene::CameraPose to;
        to.position = glm::vec3(10.0f, 0.0f, 0.0f);
        to.yaw = 1.0f;
        to.pitch = 0.5f;
        to.fov = 80.0f;

        ViewTransition tr;
        tr.begin(from, 0.2f);
        const scene::CameraPose at0 = tr.step(to, 0.0f);
        report("blend_start_pos", at0.position.x, 0.0, 1e-6);
        report("blend_start_fov", at0.fov, 60.0, 1e-6);
        reportFlag("blend_running", tr.running());

        const scene::CameraPose mid = tr.step(to, 0.1f);
        report("blend_mid_pos", mid.position.x, 5.0, 1e-4);   // smoothstep(0.5)=0.5
        report("blend_mid_fov", mid.fov, 70.0, 1e-4);

        const scene::CameraPose end = tr.step(to, 0.15f);
        report("blend_end_pos", end.position.x, 10.0, 0.0);
        report("blend_end_fov", end.fov, 80.0, 0.0);
        reportFlag("blend_stopped", !tr.running());

        // 跨 ±π：3.13 → -3.13 直插会绕一整圈（中点 ≈ 0），
        // 最短角差的中点必须仍在 ±π 附近。
        scene::CameraPose a, b;
        a.yaw = 3.13f;
        b.yaw = -3.13f;
        ViewTransition wrap;
        wrap.begin(a, 0.2f);
        const scene::CameraPose wmid = wrap.step(b, 0.1f);
        report("blend_shortest_arc", std::fabs(wmid.yaw), 3.14159, 0.02);

        // 0 秒 = 硬切：直接返回目标，且状态不残留
        ViewTransition hard;
        hard.begin(a, 0.0f);
        reportFlag("blend_zero_not_running", !hard.running());
        const scene::CameraPose cut = hard.step(b, 0.016f);
        report("blend_zero_target", cut.yaw, b.yaw, 0.0);
    }

    // ---- I. 可见性：藏身体但留影子 ----
    {
        RigFixture f;
        ecs::Entity a = f.scene.createObject("MeshA");
        ecs::Entity b = f.scene.createObject("MeshB");
        f.scene.world().add<ecs::MeshComponent>(a, ecs::MeshComponent{});
        f.scene.world().add<ecs::MeshComponent>(b, ecs::MeshComponent{});
        // forEachRenderable 要 Mesh + Material 两件套（与渲染层同一口径）
        f.scene.world().add<ecs::MaterialComponent>(a, ecs::MaterialComponent{});
        f.scene.world().add<ecs::MaterialComponent>(b, ecs::MaterialComponent{});
        f.scene.setParent(a, f.player);
        f.scene.setParent(b, f.player);

        const int n = applyBodyVisibility(f.scene, f.player, false, true);
        report("visibility_touched", static_cast<double>(n), 2.0, 0.0);
        const auto* va = f.scene.world().get<ecs::VisibilityComponent>(a);
        reportFlag("visibility_hidden", va && !va->visible);
        reportFlag("visibility_shadow_kept",
                   va && va->castShadow && va->castShadowHidden);

        // 主渲染看不到、阴影里还在 —— 两个遍历器说的必须一致
        int renderable = 0, casters = 0;
        f.scene.forEachRenderable(
            [&](ecs::Entity, ecs::MeshComponent&, ecs::MaterialComponent&) {
                ++renderable;
            });
        f.scene.forEachShadowCaster([&](ecs::Entity, ecs::MeshComponent&) {
            ++casters;
        });
        report("visibility_renderable", static_cast<double>(renderable), 0.0, 0.0);
        report("visibility_casters", static_cast<double>(casters), 2.0, 0.0);

        // 关掉 bodyShadowWhenHidden → 影子也没了（UE 里就是 bCastHiddenShadow=0）
        applyBodyVisibility(f.scene, f.player, false, false);
        casters = 0;
        f.scene.forEachShadowCaster([&](ecs::Entity, ecs::MeshComponent&) {
            ++casters;
        });
        report("visibility_casters_off", static_cast<double>(casters), 0.0, 0.0);

        // 变回可见：影子照旧、主渲染回来（castShadowHidden 不该被复位成
        // "以后可见时也不投影"）
        applyBodyVisibility(f.scene, f.player, true, true);
        renderable = 0;
        f.scene.forEachRenderable(
            [&](ecs::Entity, ecs::MeshComponent&, ecs::MaterialComponent&) {
                ++renderable;
            });
        report("visibility_restored", static_cast<double>(renderable), 2.0, 0.0);
    }

    // ---- J. 换视角不动动画 ----
    //
    // UE 的 Toggle View 只是换相机与可见性；如果顺带把 clip 名 / 播放头
    // 重置了，切回来的第一帧角色会**从头迈步**（明显的"卡一下"）。
    {
        RigFixture f;
        auto* sk = f.scene.world().get<ecs::SkinnedMeshComponent>(f.player);
        sk->clipName = "Walk";
        sk->time = 1.234f;
        ecs::PlayerComponent& pc = f.pc();
        pc.mode = ecs::PlayerMode::ThirdPerson;

        const bool changed =
            setViewMode(f.scene, f.player, ecs::PlayerMode::FirstPerson);
        reportFlag("viewmode_changed", changed);
        reportFlag("viewmode_is_first",
                   pc.mode == ecs::PlayerMode::FirstPerson);
        report("viewmode_clip_kept", 0.0, sk->clipName == "Walk" ? 0.0 : 1.0, 0.0);
        report("viewmode_time_kept", sk->time, 1.234, 1e-6);

        // 同一模式再切一次 → 不算变化（否则"每帧调一次"会把过渡不断重置）
        const bool again =
            setViewMode(f.scene, f.player, ecs::PlayerMode::FirstPerson);
        reportFlag("viewmode_idempotent", !again);
    }

    const int failures = g_fail;
    VK_LOG_INFO("VIEWRIG-TOTAL pass=%d fail=%d", g_pass, g_fail);
    return failures;
}

} // namespace character
