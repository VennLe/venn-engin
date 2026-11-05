// ============================================================
// character/ViewRig.cpp —— 见头文件里那段"为什么要有这个文件"
// ============================================================

#include "character/ViewRig.h"

#include "assets/Model.h"
#include "assets/Skeleton.h"
#include "scene/CameraSystem.h"   // scene::CameraPose
#include "scene/Scene.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace character {

namespace {
constexpr float kPi = 3.14159265358979f;
constexpr float kTwoPi = 6.28318530717959f;
} // namespace

// ---------------------------------------------------------------- 身体度量

BodyMetrics fitBody(float height) {
    BodyMetrics m;
    // 资产缺 bounds 时按 1.8 m：这不是"猜一个好看的数"，而是 UI 里
    // "人体" 这个档位本身（胶囊、台阶、跳跃、镜头）的标定基准。
    m.height = (height > 0.05f) ? height : 1.8f;
    const float h = m.height;

    // 半径 0.16 h：1.8 m → 0.288 m（成人肩宽的一半左右）。
    // 夹在 [0.15, 0.60]：玩具角色（0.6 m）不该得到 0.096 m 的胶囊
    // （比手柄还细，走两步就穿墙），巨人（3 m）不该得到 0.48 m 以上
    // （门槛过不去）。
    m.radius = glm::clamp(h * 0.16f, 0.15f, 0.60f);
    // 胶囊**总高 = 模型身高**（这是 1:1 的全部含义）：cap 的总高是
    // 2*(halfHeight + radius)，所以 halfHeight = h/2 - r。
    m.halfHeight = std::max(0.10f, h * 0.5f - m.radius);
    // 台阶按 0.2 h：1.8 m → 0.36 m（一级普通楼梯）；夹在 [0.20, 0.50]。
    m.stepHeight = glm::clamp(h * 0.20f, 0.20f, 0.50f);
    // 跳跃：跳起 0.39 × 身高 = 普通人的立定跳竖直高度。v0 = √(2gh)。
    // 与 Jump clip 的 0.75 s 滞空对齐（见 docs/player_controls.md）。
    m.jumpSpeed = glm::clamp(std::sqrt(2.0f * 9.81f * 0.39f * h), 3.0f, 5.0f);
    // 第三人称取景：2.0 h → 角色占屏约 1/3。1.5 h 会"脸贴脸"，
    // 3 h 会让人在画面里小到看不清脚有没有打滑。
    m.camDistance = glm::clamp(h * 2.0f, 2.5f, 6.5f);
    // 注视点 0.42 h（1.8 m → 0.756 m，胸／肩之间）。比"眼睛"低，
    // 是因为第三人称的构图习惯是"人站在画面偏下、视线方向留白"。
    m.focusHeight = h * 0.42f;
    // 没有骨架时的眼高兜底：0.92 h（1.8 m → 1.656 m）。
    // 真实人眼在 0.93~0.94 h；取 0.92 是因为**模型**的头顶常带发髻/呆毛，
    // 几何高比人的实际身高多 2~4 cm。
    m.eyeHeight = h * 0.92f;
    return m;
}

// ---------------------------------------------------------------- 眼位

int findEyeBone(const assets::Skeleton& skel, const std::string& name) {
    if (name.empty()) return -1;
    // `kJointMatchDefault` = 不分大小写 + 精确失败后退化成"包含" +
    // 跳过 `Head_end` / `*_tip` 这类末端辅助骨。理由见 Skeleton.h 里
    // 那段注释：辅助骨的位置在**骨尖**上，拿它当眼睛会让人从额头顶端看出去。
    return skel.findJoint(name, assets::kJointMatchDefault);
}

EyeAnchor resolveEye(const scene::Scene& scene, ecs::Entity player,
                     const ecs::PlayerComponent& pc) {
    EyeAnchor out;
    const ecs::World& w = scene.world();

    const auto* ptf = w.get<ecs::TransformComponent>(player);
    const glm::vec3 origin =
        ptf ? glm::vec3(scene.worldMatrix(player)[3]) : glm::vec3(0.0f);

    // ---- 兜底：没有骨架 / 骨架里没有头骨 → 脚 + eyeHeight ----
    //
    // 这条路径**必须存在**：占位胶囊玩家、刚导入还没烤骨骼的模型、
    // 以及自检里手搭的场景都没有头骨。没有它，"第一人称"在资产缺一根
    // 骨的时候会直接没有相机（表现为整个视口黑掉）。
    const auto* sk = w.get<ecs::SkinnedMeshComponent>(player);
    const assets::Skeleton* skel =
        (sk && sk->model) ? sk->model->skeleton.get() : nullptr;
    if (!skel || skel->jointCount() <= 0) {
        out.valid = true;
        out.fromBone = false;
        out.position = origin + glm::vec3(0.0f, 0.0f, pc.eyeHeight);
        out.heightAboveFeet = pc.eyeHeight;
        return out;
    }

    const int joint = findEyeBone(*skel, pc.eyeBone);
    if (joint < 0 || joint >= static_cast<int>(sk->jointMatrices.size())) {
        out.valid = true;
        out.fromBone = false;
        out.position = origin + glm::vec3(0.0f, 0.0f, pc.eyeHeight);
        out.heightAboveFeet = pc.eyeHeight;
        return out;
    }

    // ---- 骨骼 → 世界：与渲染的蒙皮公式**逐项一致** ----
    //
    // 渲染那一侧（pbr.vert / Renderer 的 push.model）算的是
    //
    //     v_world = push.model · Σ w_j · jointMat_j · v_bind
    //     jointMat_j = invMeshNode · boneWorld_j · invBind_j
    //
    // 想知道"这根骨的原点跑到哪了"，就把**绑定姿势下它的原点**喂进同一个
    // 式子。那个点在绑定姿势的网格空间里是 `invBind⁻¹ · 0`（绑定矩阵把
    // 网格空间映射到骨骼空间，原点自然回到 0）：
    //
    //     p_mesh = jointMat_j · (invBind_j⁻¹ · 0)
    //     p_world = push.model · p_mesh
    //
    // ⚠ 只多一次 4×4 求逆（每帧一次，不是每骨一次）。但它换来的是
    //   "眼位与画面里那颗头**在同一个坐标系里**"—— 自己按 restLocal 累乘
    //   也能算出个位置，可是 glTF 的 Y-up→Z-up 藏在 invMeshNode 里、FBX
    //   的又已经烘进骨里；自己累乘就要在两套空间之间猜一次，猜错的症状
    //   是"第一人称视角在地板下面"。
    const glm::mat4 bindToMesh = glm::inverse(skel->joints[joint].invBind);
    const glm::vec4 meshOrigin =
        sk->jointMatrices[joint] * (bindToMesh * glm::vec4(0, 0, 0, 1));
    const glm::vec3 meshPos(meshOrigin);

    // `push.model` = 网格实体的世界矩阵（entityWorld · SubMesh::transform）。
    // 网格实体是玩家的**直接子级**（instantiateModel 就这么建的）。
    // 多个子网格理论上可以有各自的节点变换，但同一具骨架的骨骼位置只有
    // 一份 —— 取第一个带网格的子级即可，差异只可能来自建模软件里
    // "同一个 skin 挂在两个不同节点上"这种极罕见的结构。
    glm::mat4 meshWorld(1.0f);
    bool haveMeshWorld = false;
    if (const auto* h = w.get<ecs::HierarchyComponent>(player)) {
        for (ecs::EntityId cid : h->children) {
            const ecs::Entity ce = w.handle(cid);
            if (!w.get<ecs::MeshComponent>(ce)) continue;
            meshWorld = scene.worldMatrix(ce);
            haveMeshWorld = true;
            break;
        }
    }
    glm::vec3 pos = haveMeshWorld ? glm::vec3(meshWorld * glm::vec4(meshPos, 1.0f))
                                  : origin + meshPos;

    // 眼位微调：按**玩家局部**的 Z 轴旋转套上去。玩家只会绕 Z 转，
    // 用整条世界矩阵的旋转部分会把子网格的朝向补偿（yawOffset）也带进来，
    // 一个 3 cm 的偏移被 90° 补偿一拧就跑到耳朵外面去了。
    //
    // 这个偏移的语义 = UE 的 socket 相对变换（见 ViewRig.h 的说明）：
    // **作者的数据**。头骨在颅底，眼睛在它上方那 4~5 cm 由这里给。
    if (pc.eyeOffset != glm::vec3(0.0f)) {
        const float yaw = ptf ? ptf->rotation.z : 0.0f;
        const glm::mat4 rot = glm::rotate(glm::mat4(1.0f), yaw,
                                          glm::vec3(0.0f, 0.0f, 1.0f));
        pos += glm::vec3(rot * glm::vec4(pc.eyeOffset, 0.0f));
    }

    out.valid = true;
    out.fromBone = true;
    out.bone = skel->joints[joint].name;
    out.position = pos;
    out.heightAboveFeet = pos.z - origin.z;
    return out;
}

// ---------------------------------------------------------------- 视线基

float wrapPi(float rad) {
    rad = std::fmod(rad + kPi, kTwoPi);
    if (rad < 0.0f) rad += kTwoPi;
    return rad - kPi;
}

void lookBasis(float viewYaw, glm::vec3* look, glm::vec3* right) {
    // yaw=0 → +X；yaw 增大 → 向右转 → 视线朝 -Y。
    // 与 `Camera::angleDir` 的 `(cos y, -sin y)` 同向 —— 也就是说
    // **视线方向 = angleDir(viewYaw)**，而 Camera 的 yaw 是"相机那一侧"，
    // 所以要 +π 才是同一组角度（见 cameraAnglesFromLook）。
    const float c = std::cos(viewYaw);
    const float s = std::sin(viewYaw);
    if (look) *look = glm::vec3(c, -s, 0.0f);
    // right = look × up（Z-up 右手系）= (-sin, -cos, 0)
    if (right) *right = glm::vec3(-s, -c, 0.0f);
}

void cameraAnglesFromLook(float viewYaw, float lookPitch, float* camYaw,
                          float* camPitch) {
    if (camYaw) *camYaw = wrapPi(viewYaw + kPi);
    if (camPitch) *camPitch = -lookPitch;
}

// ---------------------------------------------------------------- 平滑

float dampFactor(float dt, float tau) {
    if (dt <= 0.0f || tau <= 0.0f) return 1.0f;
    return 1.0f - std::exp(-dt / tau);
}

const glm::vec3& EyeDamp::step(const glm::vec3& target, float dt, float tau) {
    if (!init || tau <= 0.0f || dt <= 0.0f) {
        value = target;
        init = true;
        return value;
    }
    value += (target - value) * dampFactor(dt, tau);
    return value;
}

// ---------------------------------------------------------------- 过渡

float ViewTransition::alpha() const {
    if (!active || duration <= 1.0e-5f) return 1.0f;
    const float x = glm::clamp(t / duration, 0.0f, 1.0f);
    // smoothstep：两端速度为 0。线性插值的过渡会让起步与收尾各有一个
    // 可见的"折角"，镜头看起来像被弹了一下。
    return x * x * (3.0f - 2.0f * x);
}

void ViewTransition::begin(const scene::CameraPose& from, float seconds) {
    fromPos = from.position;
    fromYaw = from.yaw;
    fromPitch = from.pitch;
    fromFov = from.fov;
    t = 0.0f;
    duration = std::max(seconds, 0.0f);
    // 0 秒 = 硬切。保留 active=false 是刻意的：让"没过渡"与"过渡结束"
    // 走**同一条**返回路径（直接返回 target），不存在第二套代码。
    active = duration > 1.0e-4f;
}

scene::CameraPose ViewTransition::step(const scene::CameraPose& target,
                                       float dt) {
    if (!active) return target;
    t += (dt > 0.0f ? dt : 0.0f);
    if (t >= duration) {
        active = false;
        return target;
    }
    const float w = alpha();
    scene::CameraPose p = target;
    p.position = glm::mix(fromPos, target.position, w);
    // 最短角差：起点 3.13 / 终点 -3.13 时直接 mix 会绕一整圈
    // （画面里就是"镜头原地转体 360°"）。差分先 wrap 到 (-π, π] 再加回去。
    p.yaw = wrapPi(fromYaw + wrapPi(target.yaw - fromYaw) * w);
    p.pitch = fromPitch + (target.pitch - fromPitch) * w;
    p.fov = fromFov + (target.fov - fromFov) * w;
    return p;
}

// ---------------------------------------------------------------- 可见性

int applyBodyVisibility(scene::Scene& scene, ecs::Entity player, bool visible,
                        bool castShadowWhenHidden) {
    ecs::World& w = scene.world();
    const auto* h = w.get<ecs::HierarchyComponent>(player);
    if (!h) return 0;

    int touched = 0;
    for (ecs::EntityId cid : h->children) {
        const ecs::Entity ce = w.handle(cid);
        if (!w.get<ecs::MeshComponent>(ce)) continue;
        ecs::VisibilityComponent* vis = w.get<ecs::VisibilityComponent>(ce);
        if (!vis) {
            w.add<ecs::VisibilityComponent>(ce, ecs::VisibilityComponent{});
            vis = w.get<ecs::VisibilityComponent>(ce);
            if (!vis) continue;
        }
        vis->visible = visible;
        // 不可见时是否仍投影（UE 的 bCastHiddenShadow）。可见时这个位
        // **没有意义**，但也不去动它 —— 它描述的是"藏起来的时候要不要影子"。
        if (!visible) vis->castShadowHidden = castShadowWhenHidden;
        ++touched;
    }
    return touched;
}

bool setViewMode(scene::Scene& scene, ecs::Entity player,
                 ecs::PlayerMode mode, int* outMeshes) {
    ecs::World& w = scene.world();
    ecs::PlayerComponent* pc = w.get<ecs::PlayerComponent>(player);
    if (!pc) return false;
    if (pc->mode == mode) return false;
    pc->mode = mode;

    const bool hide = (mode == ecs::PlayerMode::FirstPerson) &&
                      pc->hideBodyFirstPerson;
    const int n = applyBodyVisibility(scene, player, !hide,
                                      pc->bodyShadowWhenHidden);
    // ---- 朝向口径跟着视角走（2026-11-06）----
    //
    // UE 里"第一人称"和"第三人称"是**两个 Pawn 类**，各自把
    // `bOrientRotationToMovement` 写在构造里：第三人称 true、第一人称
    // false（改用 `bUseControllerRotationYaw`）。本工程是同一个玩家实体
    // 在两种视角间切，所以这一位必须在切换时跟着口径走 —— 否则切到第一
    // 人称后身体仍按移动方向转，侧步时头骨横移、画面平移一下。
    //
    // ⚠ 这里**覆盖**作者的设置。因为它不是手感偏好，是"眼睛长在头上"的
    //   几何后果（见 PlayerComponent::orientToMovement 的注释）。要保留
    //   手动值就得连带改相机口径，那不在本机制的范围内。
    pc->orientToMovement = (mode != ecs::PlayerMode::FirstPerson);
    if (outMeshes) *outMeshes = n;
    return true;
}

std::string describeView(const ecs::PlayerComponent& pc, const EyeAnchor& eye,
                         int meshes) {
    char buf[360];
    char meshText[16];
    if (meshes < 0) std::snprintf(meshText, sizeof(meshText), "?");
    else std::snprintf(meshText, sizeof(meshText), "%d", meshes);
    std::snprintf(
        buf, sizeof(buf),
        "mode=%s body=%s eye=%s fromBone=%d eyeZ=%.3f fov1=%.1f fov3=%.1f "
        "blend=%.2f lag=%.3f mesh=%s",
        pc.mode == ecs::PlayerMode::FirstPerson ? "first" : "third",
        pc.hideBodyFirstPerson ? "owner-no-see" : "visible",
        eye.fromBone ? (eye.bone.empty() ? "?" : eye.bone.c_str()) : "fallback",
        eye.fromBone ? 1 : 0, static_cast<double>(eye.heightAboveFeet),
        static_cast<double>(pc.fovFirst),
        static_cast<double>(pc.fovThird),
        static_cast<double>(pc.viewBlendSeconds),
        static_cast<double>(pc.eyeLagSeconds), meshText);
    return buf;
}

} // namespace character
