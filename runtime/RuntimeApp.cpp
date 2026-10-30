#include "RuntimeApp.h"

#include "assets/AssetManager.h"
#include "assets/ModelOrientation.h"
#include "assets/AssetPath.h"
#include "assets/Model.h"
#include "core/Build.h"
#include "core/Input.h"
#include "core/Logger.h"
#include "dialogue/DialogueSystem.h"
#include "ecs/Components.h"
#include "i18n/I18n.h"
#include "physics/PhysicsWorld.h"
#include "scene/CameraMove.h"   // 镜头光学换算（fStop→光圈 / 快门角→模糊）
#include "scene/CameraSystem.h"
#include "scene/ModelInstance.h"
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"
#include "script/ScriptEngine.h"
#include "script/ScriptRuntime.h"
#include "sequence/SequencerSystem.h"
#include "ui2/UiCanvas.h"

#include <GLFW/glfw3.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

namespace runtime {

namespace {

// ============================================================
// 运行时切一段动画（C6 `playAnim` 的落地）
// ============================================================
// 返回 false = 切不了（没 Animator / 没骨架 / 骨架里没有这段 clip）。
//
// ⚠⚠ 必须**同时**写两个组件：
//   · `SkinnedMeshComponent::clipName` 是采样层真正读的字段；
//   · `AnimatorComponent::defaultClip` 只在 `!appliedDefaultClip` 时补一次
//     （见 AnimatorSystem.cpp 那个分支）。
//   只写后者 = **什么都没发生**：脚本回 true、日志干净、画面不动。
//   这类 bug 的特征是"越看代码越对"，因为代码确实对 —— 只是写错了对象。
//
// ⚠ 走 crossfade 而不是硬切 `time = 0`：硬切在腿/手臂上是同一帧内的
//   跳变（B 组那次用户原话是"机械、僵硬"）。0.2 s 是"看得出在过渡、
//   又不拖沓"的档位，与 AnimController 的默认淡入同量级。
bool switchAnimClip(scene::Scene& scene, ecs::Entity e, const std::string& clip) {
    ecs::World& w = scene.world();
    auto* sk = w.get<ecs::SkinnedMeshComponent>(e);
    auto* an = w.get<ecs::AnimatorComponent>(e);
    // ⚠ 三种失败必须分开报。"没有 Animator / 没有骨架 / 没有这段 clip"
    //   合并成一句话的后果是：开发者拼错一个 clip 名，看到的提示是
    //   "has no Animator component"，于是去查组件挂没挂 —— 方向从一开始
    //   就错了。可用 clip 列表直接打出来，拼错这件事一秒就能发现。
    if (!sk || !an) {
        VK_LOG_WARN("RUNTIME-SWITCH-ANIM-MISS entity=%u clip=%s "
                    "reason=no-animator (needs SkinnedMesh + Animator)",
                    static_cast<unsigned>(e.id), clip.c_str());
        return false;
    }
    if (!sk->model || !sk->model->skeleton) {
        VK_LOG_WARN("RUNTIME-SWITCH-ANIM-MISS entity=%u clip=%s "
                    "reason=no-skeleton",
                    static_cast<unsigned>(e.id), clip.c_str());
        return false;
    }
    if (clip.empty()) {
        VK_LOG_WARN("RUNTIME-SWITCH-ANIM-MISS entity=%u clip=<empty> "
                    "reason=empty-clip",
                    static_cast<unsigned>(e.id));
        return false;
    }

    const assets::Skeleton& skel = *sk->model->skeleton;
    // 骨架里没有这段 clip 就不切 —— 切了会让采样回落到 rest pose（T-pose），
    // 而"角色突然摆成 T 字"看起来像渲染 bug，实际是名字拼错了。
    if (!skel.findClip(clip)) {
        std::string avail;
        for (const auto& c : skel.clips) {
            if (!avail.empty()) avail += "|";
            avail += c.name;
        }
        VK_LOG_WARN("RUNTIME-SWITCH-ANIM-MISS entity=%u clip=%s "
                    "reason=no-such-clip available=[%s]",
                    static_cast<unsigned>(e.id), clip.c_str(), avail.c_str());
        return false;
    }

    if (!sk->clipName.empty() && sk->clipName != clip) {
        sk->fadeFromClip = sk->clipName;
        sk->fadeFromTime = sk->time;
        sk->fadeDur = 0.2f;
        sk->fadeT = 0.0f;
    }
    const std::string from = sk->clipName;
    sk->clipName = clip;
    sk->time = 0.0f;
    sk->playing = true;

    an->defaultClip = clip;
    // 标成"已经补过"：否则下一次 AnimatorSystem 会拿 defaultClip 再覆盖一遍
    // （值相同、无害，但会在切到别的 clip 后又把它改回去）。
    an->appliedDefaultClip = true;
    an->time = 0.0f;
    an->playing = true;

    // 锚点：**这一行是"脚本说切、宿主真切了"的唯一直接证据**。
    // 没有它，端到端只能看到 `ANIM-FADE`（那是采样层的反应，切错对象时
    // 一样可能出现）—— 而 `playAnim` 最典型的失败模式正是"改了个没人读
    // 的字段，日志全绿、画面不动"。
    VK_LOG_INFO("RUNTIME-SWITCH-ANIM entity=%u from=[%s] to=%s",
                static_cast<unsigned>(e.id), from.c_str(), clip.c_str());
    return true;
}

// ============================================================
// 按出生点生成玩家（仿 UE5 PlayerStart → Spawn Pawn）
// ============================================================
// 触发条件：场景里**没有**启用的 CharacterControllerComponent。
//   · 老场景（自己摆了 Player 实体）→ 直接返回，行为一点不变；
//   · 新模板场景（只有地形 + GameStart）→ 在这里造一个最小可玩的玩家。
//
// 为什么玩家不是场景里的实体：用户要"初始场景里不摆放任何物体"，
// 出生点只标位置；玩家是运行时产物，存盘里也不该出现它。
//
// ------------------------------------------------------------
// 外观（P1，2026-11）：胶囊占位体 / 外部骨骼模型
// ------------------------------------------------------------
// 空 `game.json.player.model` → 还是原来那个圆柱占位体，逐位不变。
// 给了模型 → 走 `scene::instantiateModel`（**唯一**会同时建
// `SkinnedMeshComponent` + 每个 submesh 一个可绘制子实体的路径）。
//
// ⚠ 为什么不能只写一个 `skinned` 块：它只建"骨架 + 动画状态"，
//   不建任何带 `MeshComponent` 的实体 —— 而渲染器的绘制列表是从
//   `MeshComponent` 收的（Renderer.cpp:3255 附近）。于是"架子建好了、
//   日志 SCENE-SKINNED ok=1、视口里什么都没有"。
//   这个坑在手工拼场景 JSON 时踩过一次，所以这里的顺序是硬的。

// 模型世界包围盒。算法与编辑器 CharacterWindow::computeModelBounds 相同
// （8 个角点各乘一次 SubMesh 变换再取 min/max —— 对带旋转的部件不会算出
// 偏小的盒子）。返回 false = 没有可用 bounds。
//
// `yawDeg` 把**装配时的朝向补偿**也算进去（见 ModelInstanceOptions::
// yawOffsetDeg）。抬脚底只需要 z，用 0 即可；而"朝向对不对"必须用真实
// 补偿角算 —— 见下面 facing 自检。
bool modelWorldBoundsYaw(const assets::Model& m, float yawDeg, glm::vec3* lo,
                         glm::vec3* hi) {
    const glm::mat4 rot =
        glm::rotate(glm::mat4(1.0f), glm::radians(yawDeg),
                    glm::vec3(0.0f, 0.0f, 1.0f));
    bool first = true;
    for (const auto& sub : m.subMeshes) {
        if (!sub.mesh || !sub.mesh->hasBounds()) continue;
        const glm::vec3 a = sub.mesh->boundsMin();
        const glm::vec3 b = sub.mesh->boundsMax();
        for (int i = 0; i < 8; ++i) {
            const glm::vec3 c((i & 1) ? b.x : a.x, (i & 2) ? b.y : a.y,
                              (i & 4) ? b.z : a.z);
            const glm::vec3 w =
                glm::vec3(rot * sub.transform * glm::vec4(c, 1.0f));
            if (first) {
                *lo = *hi = w;
                first = false;
            } else {
                *lo = glm::min(*lo, w);
                *hi = glm::max(*hi, w);
            }
        }
    }
    return !first;
}

bool modelWorldBounds(const assets::Model& m, glm::vec3* lo, glm::vec3* hi) {
    return modelWorldBoundsYaw(m, 0.0f, lo, hi);
}

// 把模型挂到玩家实体上。返回 true = 挂上了（此时调用方不该再建占位体）。
//
// 三件必须一起做的事：
//   ① `instantiateModel` 建子网格实体（可绘制）+ 玩家身上的蒙皮组件；
//   ② **抬到脚底**：模型 pivot 在几何中心（导入时归位过），而玩家实体的
//      位置语义是**脚**（见 PlayerController::update 的 `feet`）—— 不抬
//      就有一半身体埋在地下。抬升量取**实测下沿** `-lo.z` 而不是 `h/2`：
//      带裙摆 / 武器的模型几何中心不等于半高。
//   ③ 胶囊跟模型走（`fitBody`）：解剖比例取"高 h → 半径 0.16h、
//      半高 h/2-r"，即胶囊总高**等于**模型身高 —— 这就是 1:1。
bool attachPlayerModel(scene::Scene& scene, assets::AssetManager& am,
                       ecs::Entity player,
                       const RuntimeConfig::PlayerVisual& v) {
    if (v.model.empty()) return false;

    const std::string resolved = assets::resolveAssetPath(v.model);
    assets::Model* model = nullptr;
    try {
        // 与场景里 `kind:"fbx"` 走同一个入口：`.vmesh` 烤制版存在时
        // loadFBX 内部会优先用它（见 engine/assets/FBX 加载的重定向）。
        model = am.loadFBX(resolved, v.model);
    } catch (const std::exception& ex) {
        VK_LOG_ERROR("GAME-PLAYER model load failed path='%s' error=%s",
                     v.model.c_str(), ex.what());
        return false;
    }
    if (!model || !model->valid()) {
        VK_LOG_ERROR("GAME-PLAYER model invalid path='%s'", v.model.c_str());
        return false;
    }

    glm::vec3 lo(0.0f), hi(0.0f);
    const bool hasBounds = modelWorldBounds(*model, &lo, &hi);
    const float height = hasBounds ? std::max(hi.z - lo.z, 0.05f) : 1.8f;

    scene::ModelInstanceOptions opt;
    opt.parent = player;
    opt.autoPlay = true;          // 绑上去就该动（与 CharacterWindow 一致）
    opt.sourcePath = v.model;     // 打开 MODEL-INSTANCE 锚点
    // 朝向补偿（P5 / 2026-10-24 重做）：**问骨架**，别信写死的角度。
    // 与 `editor/EditorScene.cpp::bindDefaultCharacter` 同一套判定 —— 两处
    // 装配必须给出同一个朝向，否则"编辑器里朝前、Game.exe 里倒着走"。
    float modelYaw = v.modelYaw;
    glm::vec2 localFwd(0.0f);
    float rawYaw = modelYaw;
    const bool modelYawDerived =
        assets::modelForwardYawDeg(*model, &modelYaw, &localFwd, &rawYaw);
    if (!modelYawDerived) modelYaw = v.modelYaw;
    opt.yawOffsetDeg = modelYaw;
    VK_LOG_INFO("GAME-PLAYER forward local=(%.3f,%.3f) rawYaw=%.1f yaw=%.1f "
                "derived=%d (declared=%.1f)",
                static_cast<double>(localFwd.x),
                static_cast<double>(localFwd.y),
                static_cast<double>(rawYaw), static_cast<double>(modelYaw),
                modelYawDerived ? 1 : 0, static_cast<double>(v.modelYaw));
    const scene::ModelInstanceResult r = scene::instantiateModel(scene, model, opt);
    if (!r.root.valid() || r.entities.empty()) {
        VK_LOG_ERROR("GAME-PLAYER instantiate failed path='%s'",
                     v.model.c_str());
        return false;
    }

    ecs::World& w = scene.world();

    // ② 抬到脚底。子实体的局部变换是 instantiateModel 写的，这里只加 z。
    if (hasBounds) {
        const float lift = -lo.z;
        for (ecs::Entity ce : r.entities) {
            if (auto* t = w.get<ecs::TransformComponent>(ce))
                t->position.z += lift;
        }
        VK_LOG_INFO("GAME-PLAYER model lift=%.3f m (bounds z: %.3f..%.3f) "
                    "yaw=%.1f deg -> bottom=%.4f",
                    static_cast<double>(lift), static_cast<double>(lo.z),
                    static_cast<double>(hi.z),
                    static_cast<double>(modelYaw),
                    static_cast<double>(lo.z + lift));

        // ---- 朝向自检（P5）----
        //
        // 判据：**人形的"左右"跨度必须大于"前后"跨度**。
        //   实测这具模型：臂展（左右）≈ 1.95 m、身体厚（前后）≈ 0.36 m ——
        //   差距够大，是个很稳的判据。漏掉 90° 补偿时两者整好互换
        //   （X 变成 1.95、Y 变成 0.36），这条立刻红。
        //
        //   ⚠ 为什么不比截图：`w/h` 的像素判据实测在 0.367~0.429 之间飘
        //   （手臂前后摆会撑宽包围盒），拿来断言就是 flaky。几何量是确定的。
        //   ⚠ 也不要拿"肩宽"当基准：装配的是 T/A-pose，包围盒量到的是**臂展**。
        glm::vec3 fLo(0.0f), fHi(0.0f);
        if (modelWorldBoundsYaw(*model, modelYaw, &fLo, &fHi)) {
            const float spanX = fHi.x - fLo.x;
            const float spanY = fHi.y - fLo.y;
            VK_LOG_INFO("GAME-PLAYER facing yaw=%.1f spanX=%.3f spanY=%.3f "
                        "shouldersAlong=%s ok=%d",
                        static_cast<double>(modelYaw),
                        static_cast<double>(spanX),
                        static_cast<double>(spanY),
                        spanX < spanY ? "Y" : "X",
                        spanX < spanY ? 1 : 0);
            // 带符号那一条：补偿后的脚尖方向必须落在 +X。
            // 只比臂展轴的话 180° 相反也照样绿（见 EditorScene 的同名注释）。
            const float cy = std::cos(glm::radians(modelYaw));
            const float sy = std::sin(glm::radians(modelYaw));
            const glm::vec2 face(cy * localFwd.x - sy * localFwd.y,
                                 sy * localFwd.x + cy * localFwd.y);
            const bool forwardOk = modelYawDerived ? (face.x > 0.9f) : true;
            VK_LOG_INFO("GAME-PLAYER face=(%.3f,%.3f) forwardOk=%d",
                        static_cast<double>(face.x),
                        static_cast<double>(face.y), forwardOk ? 1 : 0);
            if (!forwardOk)
                VK_LOG_WARN("GAME-PLAYER 朝向补偿疑似差 180°（脸落到 -X）");
        }
    }

    // ③ 胶囊 / 眼高跟模型走
    if (auto* cc = w.get<ecs::CharacterControllerComponent>(player)) {
        if (v.fitBody) {
            const float radius = glm::clamp(height * 0.16f, 0.15f, 0.60f);
            cc->radius = radius;
            cc->halfHeight = std::max(0.10f, height * 0.5f - radius);
            cc->stepHeight = glm::clamp(height * 0.20f, 0.20f, 0.50f);
        } else {
            cc->radius = v.radius;
            cc->halfHeight = v.halfHeight;
        }
        // 跳跃初速按身高缩放：跳起 0.39 × 身高（1.8 m → 0.70 m）。
        // 与 `editor/EditorScene.cpp` 的 bindDefaultCharacter 同一个公式，
        // 与 Jump clip 的 0.75 s 滞空对齐（2026-10-22）。
        cc->jumpSpeed = glm::clamp(std::sqrt(2.0f * 9.81f * 0.39f * height),
                                   3.0f, 5.0f);
        cc->airControl = 0.45f;
        VK_LOG_INFO("GAME-PLAYER capsule radius=%.3f halfHeight=%.3f "
                    "totalHeight=%.3f (model=%.3f fit=%d) jumpV0=%.2f "
                    "jumpH=%.2fm",
                    static_cast<double>(cc->radius),
                    static_cast<double>(cc->halfHeight),
                    static_cast<double>(2.0f * (cc->halfHeight + cc->radius)),
                    static_cast<double>(height), v.fitBody ? 1 : 0,
                    static_cast<double>(cc->jumpSpeed),
                    static_cast<double>(cc->jumpSpeed * cc->jumpSpeed /
                                        (2.0f * 9.81f)));
    }

    // 取景跟上身高：2.05 m 的壮汉不该用 1.6 m 的镜头距离（会显得脸贴脸）。
    // 1.9 倍身高 ≈ 让角色占屏 1/3，是第三人称的常规构图。
    if (auto* camC = w.get<ecs::CameraComponent>(player)) {
        camC->targetOffset = glm::vec3(0.0f, 0.0f, height * 0.42f);
        camC->distance = glm::clamp(height * 2.0f, 2.5f, 6.5f);
        VK_LOG_INFO("GAME-PLAYER camera distance=%.2f targetOffset.z=%.2f",
                    static_cast<double>(camC->distance),
                    static_cast<double>(camC->targetOffset.z));
    }

    // ①' 起手 clip：给了就用给的，否则用 instantiateModel 挑的第一段
    if (auto* sk = w.get<ecs::SkinnedMeshComponent>(player)) {
        if (!v.clip.empty()) {
            if (sk->model && sk->model->skeleton &&
                !sk->model->skeleton->findClip(v.clip)) {
                VK_LOG_WARN("GAME-PLAYER clip '%s' 不在 '%s' 里"
                            "（可用片段见 MODEL-CLIPS），改用第一段",
                            v.clip.c_str(), v.model.c_str());
            } else {
                sk->clipName = v.clip;
            }
        }
        // 有 AnimatorComponent 时它是权威，得把选好的 clip 交过去 ——
        // 否则 AnimatorSystem 的"defaultClip 只补一次"会把空的补上、把
        // 我们刚设的覆盖掉（顺序依赖，写在这里最安全）。
        if (auto* an = w.get<ecs::AnimatorComponent>(player))
            an->defaultClip = sk->clipName;

        // ② 状态机控制器（A2b，2026-10-25）
        //
        // 非空 → **确保**有 AnimatorComponent 再挂路径。为什么是"确保"
        // 而不是假定有：运行时的 Player 由 `instantiateModel` 建出来，
        // 默认不带 AnimatorComponent（探测日志里 `animators=0` 就是这个
        // 原因）。而状态机的全部状态（stateIndex / 参数黑板）都存在
        // 那个组件上 —— 没有组件就没有地方放它们。
        if (!v.controller.empty()) {
            if (!w.get<ecs::AnimatorComponent>(player)) {
                ecs::AnimatorComponent a;
                // 从"当前正在播的那一段"接手：挂上控制器的瞬间画面不该变
                // （第一帧状态机会硬切到 defaultState，所以这里给的
                //  defaultClip 只是"状态机还没接管时的兜底"）。
                a.defaultClip = sk->clipName;
                a.time = sk->time;
                a.speed = sk->speed;
                a.playing = sk->playing;
                a.loop = sk->loop;
                w.add<ecs::AnimatorComponent>(player, a);
            }
            w.get<ecs::AnimatorComponent>(player)->controllerPath = v.controller;
            VK_LOG_INFO("GAME-PLAYER controller=%s", v.controller.c_str());
        }
    }

    VK_LOG_INFO("GAME-PLAYER visual=model path=%s meshes=%d skinned=%d "
                "height=%.3f m (1:1)",
                v.model.c_str(), static_cast<int>(r.entities.size()),
                r.skinned ? 1 : 0, static_cast<double>(height));
    return true;
}

ecs::Entity spawnPlayerFromGameStart(scene::Scene& scene,
                                     assets::AssetManager& am,
                                     const RuntimeConfig::PlayerVisual& vis,
                                     const physics::Heightfield* terrain) {
    ecs::World& w = scene.world();

    bool hasCharacter = false;
    w.each<ecs::CharacterControllerComponent>(
        [&](ecs::Entity, ecs::CharacterControllerComponent& cc) {
            if (cc.enabled) hasCharacter = true;
        });
    if (hasCharacter) return {};

    glm::vec3 feet(0.0f);
    float yaw = 0.0f;
    bool found = false;
    // 2026-10-24：出生点同时携带"怎么生成玩家"的配置（与编辑器 Play 同源）。
    ecs::PlayerMode mode = ecs::PlayerMode::ThirdPerson;
    bool spawnOnRun = true;
    w.each<ecs::GameStartComponent>(
        [&](ecs::Entity e, ecs::GameStartComponent& g) {
            if (found || !g.enabled) return;
            const auto* tf = w.get<ecs::TransformComponent>(e);
            if (!tf) return;
            feet = tf->position;
            yaw = tf->rotation.z + g.yaw;
            mode = g.mode;
            spawnOnRun = g.spawnOnRun;
            found = true;
        });

    // 出生点关掉"生成玩家"= 这个场景没有玩家（纯观览 / 过场）。
    if (!found || !spawnOnRun) {
        VK_LOG_INFO("GAME-PLAYER skipped found=%d spawnOnRun=%d",
                    found ? 1 : 0, spawnOnRun ? 1 : 0);
        return {};
    }

    // ---- 出生点贴地（2026-10-24）----
    // 场景文件里的 GameStart 是一个**手写的** z 值：地形高度取决于高度图，
    // 换个工程 / 重新刷过地形之后那个 z 十有八九是错的 —— 表现就是"一进游戏
    // 人是埋在地里的，或者悬在空中再往下掉一截"。
    //
    // 这里用物理世界已经建好的高度场把它压回地表：只在**低于**地表时抬，
    // 高于地表不动（那是"故意摆在台阶/桥面/二楼平台"的情况，抬或者压都会
    // 破坏作者的意图；高于地表就让重力自己去落）。
    if (found && terrain && terrain->valid()) {
        const float gz = physics::heightAt(*terrain, feet.x, feet.y);
        if (feet.z < gz - 1.0e-3f) {
            VK_LOG_INFO("GAMESTART-SEAT runtime before_z=%.3f after_z=%.3f "
                        "terrainH=%.3f at=(%.3f,%.3f)",
                        static_cast<double>(feet.z), static_cast<double>(gz),
                        static_cast<double>(gz), static_cast<double>(feet.x),
                        static_cast<double>(feet.y));
            feet.z = gz;
        }
    }

    const ecs::Entity e = scene.createObject("Player");
    if (!e.valid()) return {};
    if (auto* tf = w.get<ecs::TransformComponent>(e)) {
        tf->position = feet;
        tf->rotation = glm::vec3(0.0f, 0.0f, yaw);
        tf->scale = glm::vec3(1.0f);
    }

    ecs::CharacterControllerComponent cc;
    cc.bodyYaw = yaw;
    w.add<ecs::CharacterControllerComponent>(e, cc);

    // ⚠ 必须给玩家挂一个 `CameraComponent`，否则**相机根本不跟随**。
    //   这是 `CameraSystem::update` 的第一条规则：场景里没有任何启用的
    //   CameraComponent → `selectActive` 找不到 → 直接 return，此时
    //   PlayerController 每帧写的"期望姿态"一个字都不会落进
    //   `Scene::camera()`。症状是：角色自己走得挺好，镜头钉在出生点，
    //   人越走越小（2026-11 实测截图上就是这个）。
    //   编辑器侧 `createPlayer` 是同一个做法，两边保持一致。
    const bool firstPerson = mode == ecs::PlayerMode::FirstPerson;
    {
        ecs::CameraComponent camC;
        if (firstPerson) {
            // 第一人称：相机就在眼睛上。distance = 0（SpringArm 对 0 有
            // 专门分支：position = pivot），注视点抬到眼高。
            camC.targetOffset = glm::vec3(0.0f, 0.0f, 1.7f * 0.42f + 0.75f);
            camC.distance = 0.0f;
            camC.minDistance = 0.0f;
        } else {
            // 注视点抬到胸部高度（眼高按身高的 0.92 处折算，与编辑器同一比例）
            camC.targetOffset = glm::vec3(0.0f, 0.0f, 1.8f * 0.42f);
            camC.distance = 3.6f;   // 1.8 m 的角色占屏 ~1/3，第三人称的常规取景
            camC.minDistance = 0.6f;
        }
        w.add<ecs::CameraComponent>(e, camC);
    }

    // 外观：先试外部模型。挂上了就**不再建**占位体 —— 两个都在会看到
    // "角色身体里插着一根蓝柱子"。
    //
    // 第一人称**不装身体**：相机就在头颅里，装了只会看到脸的内侧。这也
    // 正好符合"第一人称看不见自己"的常识。
    bool bound = false;
    if (!firstPerson) bound = attachPlayerModel(scene, am, e, vis);

    if (!bound && !firstPerson) {
        // 占位身体：一个胶囊（圆柱 + 圆顶感靠颜色/光照，不做真胶囊网格）。
        // ⚠ 材质名必须唯一：AssetManager 按名缓存，重名会和其他物体共用。
        ecs::MeshComponent mc;
        mc.mesh = am.cylinder(cc.radius, "runtime.player.body", 16);
        w.add<ecs::MeshComponent>(e, mc);
        ecs::MaterialComponent matc;
        matc.material = am.makeMaterial("runtime.player.mat", nullptr,
                                        glm::vec4(0.62f, 0.70f, 0.86f, 1.0f),
                                        0.45f, 0.0f);
        w.add<ecs::MaterialComponent>(e, matc);
    }

    VK_LOG_INFO("GAME-PLAYER spawned at GameStart found=%d "
                "feet=(%.2f,%.2f,%.2f) yaw=%.3f mode=%s visual=%s",
                found ? 1 : 0, static_cast<double>(feet.x),
                static_cast<double>(feet.y), static_cast<double>(feet.z),
                static_cast<double>(yaw), firstPerson ? "first" : "third",
                firstPerson ? "none(first-person)"
                            : (bound ? "model" : "capsule"));
    return e;
}

} // namespace

RuntimeApp::RuntimeApp(const RuntimeConfig& cfg)
    // G30：出货版在标题栏挂 git 号 —— 玩家截图/录屏发过来时，那是判断
    // "他跑的是哪一版"的唯一线索（也是我们要求他们附带的最小信息）。
    // ⚠ dev 版**不加**：现有几十条 GUI 用例按窗口标题精确匹配找窗口。
    : core::Application(
          core::kShipping
              ? (cfg.title + " [" + core::kGitDescribe + "]")
              : cfg.title,
          cfg.width, cfg.height),
      m_cfg(cfg) {
    m_loop.config().maxFps = cfg.maxFps;
    m_loop.config().unfocusedFps = cfg.unfocusedFps;
    m_loop.config().timeScale = cfg.timeScale;
    // GAME-LOOP 每 60 帧一行：只在 verbose 下打（正常玩不需要刷屏）。
    m_loop.setLogInterval(cfg.verbose ? 60 : 0);

    // G30：物理逐体转储是**开发钩子** —— shipping 版里这段不存在
    // （`MYVK_PHYS_DUMP` 这个字符串也不在二进制里）。
#if !VENN_SHIPPING
    if (const char* dump = std::getenv("MYVK_PHYS_DUMP")) {
        const int n = std::atoi(dump);
        if (n > 0) m_physicsDumpEvery = n;
    }
#endif
    // M2 的两个调试口子。刻意都用 `MYVK_RUNTIME_` 前缀：它与其他运行时
    // 开关同一条规矩（前缀放行），不需要再往白名单里加裸名字。
    if (const char* d = std::getenv("MYVK_RUNTIME_PLAYER_DUMP")) {
        const int n = std::atoi(d);
        if (n > 0) m_playerDumpEvery = n;
    }
    if (const char* c = std::getenv("MYVK_RUNTIME_CONTACT_TRACE")) {
        m_contactTrace = (c[0] != '0');
        m_contacts.setTrace(m_contactTrace);
    }
    // 字幕自动推进（C2 验收用）。按**秒**给间隔，0/缺省 = 关。
    // ⚠ 它走的是同一条 `pressAdvance()`，不是"跳过字幕"的另一条分支 ——
    //   否则用例验的就不是玩家真正走的那条路了。
    if (const char* a = std::getenv("MYVK_DLG_AUTO")) {
        const double v = std::atof(a);
        if (v > 0.0) m_dlgAutoSec = static_cast<float>(v);
    }
}

RuntimeApp::~RuntimeApp() = default;

// ---------------------------------------------------------------- 启动

void RuntimeApp::onInit() {
    VK_LOG_INFO("GAME-INIT runtime booting (scene='%s')", m_cfg.scenePath.c_str());

    // ---- 界面语言（U7）----
    // 词典与查表在 `engine/i18n/`（与编辑器**同一份**），这里只决定"用哪种"。
    //
    // 优先级：`MYVK_LANG` > `--language` / `MYVK_RUNTIME_LANGUAGE` / game.json
    //         > **系统语言**（2026-10-29）。
    // 最后一档是新的：`language` 字段缺省 / 空 / `auto` / `system` 时不再
    // 掉到英文，而是问 `i18n::systemLanguage()` —— 中文 Windows 上装好就
    // 是中文。原来写死 `"en"` 的那份默认值等于"默认值不对"。
    // ⚠ `MYVK_LANG` 不合 `MYVK_RUNTIME_*` 前缀纪律，是**唯一**的例外：
    //   路线图 U7 的验收条款就是按这个名字写的（`MYVK_LANG=zh`），
    //   它已在 `check_feature_surface.py` 的 ALLOW_EXACT 里显式登记。
    //   正规入口仍是 `--language` / game.json 的 `language`。
    //
    // ⚠ 认不出的值**不改语言**并打 WARN：静默回退英文会让"game.json 拼错"
    //   看起来像"配了但不生效"，那是最难查的一类。
    //   （空 / auto 是**认得的**值，不走 WARN 那条路。）
    {
        const char* envLang = std::getenv("MYVK_LANG");
        const std::string configured =
            (envLang && *envLang) ? std::string(envLang) : m_cfg.language;
        const char* src = (envLang && *envLang)          ? "env:MYVK_LANG"
                          : configured.empty()           ? "system"
                          : "config";
        // "auto" / "system" 是给人写在 game.json 里的自动档
        const bool autoLang =
            configured.empty() || configured == "auto" || configured == "system";
        const int idx = autoLang ? i18n::systemLanguage()
                                 : i18n::languageFromCode(configured.c_str());
        if (idx < 0) {
            VK_LOG_WARN("I18N-BADLANG value='%s' — 保留 %s（可用：en | zh | auto）",
                        configured.c_str(), i18n::languageCode(i18n::language()));
        } else {
            i18n::setLanguage(idx);
        }
        // 锚点（U7 验收）：中文模式是真实条数，英文模式**必须是 0**
        // —— 表根本没构造，这就是"英文 = 零开销"的观测量。
        // `src` 说明这一票是谁投的（system / config / env）。
        VK_LOG_INFO("I18N-KEY lang=%s count=%zu src=%s",
                    i18n::languageCode(i18n::language()),
                    i18n::translationCount(), src);
    }

    // 全屏在这里切（窗口已经存在；setFullscreen 会记住窗口模式下的几何）
    if (m_cfg.fullscreen) window().setFullscreen(true);

    // ---- 脚本运行时（S0）----
    m_scriptRuntime = std::make_unique<vks::ScriptRuntime>();
    m_scripts = &m_scriptRuntime->engine();
    installScriptServices();
    m_menuScene = std::make_unique<scene::Scene>();

    // 输入映射：出厂绑定 + 压一层 Gameplay（菜单进栈时会再压 Menu）
    //
    // G06：再叠一层**玩家改过的绑定表**（编辑器 Preferences ▸ Bindings 写的
    // 同一份文件）。文件不存在是正常状态 —— 从来没改过键位就一直是出厂绑定，
    // 所以这条路径**不报错**，只把"用的是哪一份"打出来。
    m_input = core::InputMap::makeDefault();
    {
        const std::string rel =
            m_cfg.inputMapPath.empty() ? "input_map.json" : m_cfg.inputMapPath;
        const std::string path = assets::resolveAssetPath(rel);
        const bool fromFile = m_input.loadFromFile(path);
        VK_LOG_INFO("GAME-INPUT source=%s path=%s",
                    fromFile ? "file" : "factory", path.c_str());
        // 逐条把**关键动作**的实际绑定点名打出来：这是"读进来了"与"真的
        // 生效了"之间唯一能被脚本断言的桥梁（改绑后这里不再是 Space）。
        VK_LOG_INFO("GAME-INPUT Jump=%s Sprint=%s Interact=%s",
                    m_input.describeBinding("Gameplay", "Jump").c_str(),
                    m_input.describeBinding("Gameplay", "Sprint").c_str(),
                    m_input.describeBinding("Gameplay", "Interact").c_str());
    }
    VK_LOG_INFO("GAME-INPUT %s", m_input.describe().c_str());

    // ---- 状态栈先立起来，再加载内容 ----
    // 顺序是有意为之：Loading 是"内容还没进来"的那一帧的真实状态；
    // 日志里先看到 Boot>Loading 再看到 GAME-SCENE，读起来是对的。
    m_stateStack.setTrace(m_cfg.traceStates);
    m_stateStack.push(makeBuiltinState(StateId::Boot), m_svc);
    m_stateStack.replace(makeBuiltinState(StateId::Loading), m_svc);

    loadStartScene();
    buildPhysics();
    preloadScripts();

    // ---- 出生点（GameStart，2026-10-21 第八轮）----
    // 模板场景现在是"空场景"（只有地形 + 出生点），玩家由这里生成 ——
    // 与 UE 的 PlayerStart → Spawn Pawn 同一个思路。老场景自带
    // CharacterController 时这一步什么都不做，行为完全不变。
    spawnPlayerFromGameStart(activeScene(), assets(), m_cfg.player,
                             m_binding.world ? m_binding.world->heightfield()
                                             : nullptr);

    // ---- 玩家（G10）：场景里有角色控制器才接管 ----
    m_player = std::make_unique<PlayerController>(m_input);
    if (m_player->attach(activeScene(), m_binding.world.get())) {
        // 相机交给 CameraSystem（PlayerController 给意图），别再让
        // scene::Camera 自己读 WASD —— 否则两边会互相打架。
        activeScene().camera().setUseGlobalInput(false);
        VK_LOG_INFO("GAME-PLAYER attached '%s'",
                    activeScene().world().name(m_player->entity())
                        ? activeScene().world().name(m_player->entity())->c_str()
                        : "?");
    } else {
        m_player.reset();
        VK_LOG_INFO("GAME-PLAYER none (no CharacterControllerComponent)");
    }
    scene::activeCameraSystem().setTrace(m_cfg.traceStates);

    // ---- 音频（G14-5）----
    //
    // ⚠ 位置有讲究：必须在 `loadStartScene()` 之后（要读场景里的
    //   AudioSourceComponent），但可以在 `syncAudio` 第一次跑之前——
    //   设备打开是同步的，所以第一帧就有正确的格式供 Mixer init。
    //   设备打不开**不是错误**：`init` 内部进 Failed，所有 API 变空操作。
    m_audio.init(m_cfg.audio);

    // ---- 服务面接线 ----
    m_svc.simulate = [this](float dt) { simulate(dt); };
    m_svc.updateVariable = [this](float vdt) {
        // M2 的相机系统接在这里：PlayerController 已经算好"意图姿态"，
        // CameraSystem 负责优先级 / 混合 / 震动，最后写进 scene::Camera。
        // 用**真实帧时长**（不是固定步）：镜头不该被 timeScale 冻结。
        if (m_player)
            scene::activeCameraSystem().setDesiredPose(m_player->cameraPose());
        // ---- 过场时间轴（C1）----
        // ⚠⚠ 位置是硬约束：**玩家之后、`update()` 之前**。
        //   · 排在玩家之前 → 每帧被玩家的相机意图覆盖（过场根本不动）
        //   · 排在 update 之后 → 这一帧的运镜丢掉（画面退化成玩家视角）
        // 用同一个 vdt：过场是表现层的东西，暂停时该一起停。
        //
        // 时间轴本身**不碰相机系统**（见 SequencerSystem.h），这里显式把
        // 它的意图喂进去 —— 走 desired 才能让震动叠在运镜上。
        // 没有 pose（没在播 / 播完）时**什么都不做**，玩家相机自然接管。
        sequence::SequencerSystem& seq = sequence::activeSequencerSystem();
        seq.tick(activeScene(), vdt);
        if (seq.hasPose())
            scene::activeCameraSystem().setDesiredPose(seq.pose());
        // ---- 多机位切镜（运镜体系 §七）----
        // ⚠⚠ 位置同样是硬约束：**seq.tick 之后、update 之前**（理由见
        //   `syncDirectorCamera` 的注释）。它可能 `clearDesiredPose()`，
        //   所以必须排在上面两行之后 —— 排在前面会被它们又覆盖回去。
        syncDirectorCamera();
        scene::activeCameraSystem().update(activeScene(), vdt);
        applyCameraLensEffects();

        // ---- 对话 / 字幕（C2）----
        // ⚠ 位置同样是硬约束：**时间轴之后**。`kind=Line` 的事件是在
        //   `seq.tick` 里 `cue()` 出来的，排在它前面这一帧的字幕就要等到
        //   下一帧才起步（起播晚一帧在过场里是很明显的"嘴型对不上"）。
        // 用同一个 vdt：对话框在暂停时该一起停。
        dialogue::DialogueSystem& dlg = dialogue::activeDialogueSystem();
        dlg.tick(activeScene(), vdt);

        // ---- 自动推进（仅 `MYVK_DLG_AUTO`）----
        if (m_dlgAutoSec > 0.0f) {
            m_dlgAutoTimer += vdt;
            if (m_dlgAutoTimer >= m_dlgAutoSec) {
                m_dlgAutoTimer = 0.0f;
                const dialogue::DialoguePlayer* p = dlg.activePlayer(activeScene());
                if (p) {
                    // 有选项 → 选第一个（`pressAdvance` 在选项上会一直返回
                    // NeedChoice，不另开一条路就永远卡住）。
                    if (p->atChoice() && p->fullyRevealed())
                        dlg.choose(activeScene(), 0);
                    else
                        dlg.pressAdvance(activeScene());
                }
            }
        }

        // 对话期间把跳跃挡掉：字幕推进也用 Space（= `Jump`），而
        // `wasPressed` 是边沿查询不是消费 —— 不挡的话"按空格翻页"会顺带
        // 起跳一次（见 PlayerController::setJumpBlocked）。
        if (m_player)
            m_player->setJumpBlocked(dlg.active(activeScene()));
    };
    m_svc.renderWorld = [this] {
        m_renderedWorldThisFrame = true;
        renderer().drawFrame(activeScene());
    };
    m_svc.timeScale = m_cfg.timeScale;

    // ---- Loading → MainMenu（→ Playing）----
    //
    // 默认流程**一定经过 MainMenu**（这样"清空 env 也能进主菜单"这件事
    // 是字面成立的），只是 M1 还没有菜单 UI（G15 才有）——没有可点的
    // "开始游戏"，所以 `startState != mainmenu` 时立刻接着进 Playing，
    // 等价于"菜单上自动按了继续"。
    //
    // `--start-state mainmenu` 停下来，用来验证 MainMenu 作为**不透明**
    // 状态的行为（此时世界不渲染，画面是空场景）。
    m_stateStack.replace(makeBuiltinState(StateId::MainMenu), m_svc);
    if (m_cfg.startState != "mainmenu") {
        m_stateStack.push(makeBuiltinState(StateId::Playing), m_svc);
    }

    // ---- HUD 外观（`config/ui.json`，U8）----
    // 文件不存在 = 出厂默认，这是**正常状态**，不打 WARN（同 E03 的纪律）。
    {
        UiConfig uc;
        const std::string p = defaultUiConfigPath();
        if (loadUiConfig(p, &uc)) m_hud.setConfig(uc);
    }

    // ---- UI 文档（#312）----
    //
    // 只在配置指定了 `.ui.json` 时才读。读失败**不阻止启动**（同场景加载：
    // 窗口照开、日志说清楚原因）—— 但也不静默降级成"硬编码 HUD"，因为
    // 那会让作者以为"我的文档没被用上，是因为文档不生效"，而不是"路径错了"。
    if (!m_cfg.uiDoc.empty()) {
        if (ui2::loadUiDocument(m_cfg.uiDoc, m_uiDoc)) {
            VK_LOG_INFO("UIDOC-LOAD path=%s ok=1 widgets=%d name=%s ref=%dx%d",
                        m_cfg.uiDoc.c_str(),
                        ui2::countWidgets(m_uiDoc.widgets),
                        m_uiDoc.name.c_str(), m_uiDoc.refW, m_uiDoc.refH);
            for (const auto& w : m_uiDoc.warnings)
                VK_LOG_WARN("UIDOC-WARN %s", w.c_str());
        } else {
            VK_LOG_ERROR("UIDOC-LOAD path=%s ok=0 err=%s", m_cfg.uiDoc.c_str(),
                         m_uiDoc.error.c_str());
        }
    }

    // ---- 游戏 UI 的绘制挂点（U6/U8）----
    // `engine` 不认识 `runtime`（分层是 runtime → engine），所以由这里把
    // "画什么"注进去。回调在 Post 输出 pass 内、ImGui 之前被调用。
    ui2::setGameUiBuilder([this](ui2::UiCanvas& c, uint32_t w, uint32_t h) {
        if (w == 0 || h == 0) return;

        // ① HUD（血条 / 准星 / 伤害数字）
        //
        // 投影矩阵在这里算而不是缓存：相机每帧都可能被 CameraSystem 改，
        // 伤害数字要**贴着世界里的位置**——晚一帧就会"数字追着目标跑"。
        const auto& cam = activeScene().camera();
        const glm::mat4 vp =
            cam.projMatrix(static_cast<float>(w) / static_cast<float>(h)) *
            cam.viewMatrix();
        m_hud.build(c, m_hudModel, vp, w, h, ui2::frameFont());

        // ①b UI 文档（#312）—— 在 HUD 之后、字幕之前。
        //
        // 顺序的理由：文档是"这个游戏自己的界面层"（美术摆的面板/文本/按钮），
        // 它该盖住引擎的调试 HUD（血条那一套是兜底）；而对话框与暂停菜单
        // 是**交互层**，永远压在它上面（否则文档里一个满屏面板会把菜单吞掉）。
        if (m_uiDoc.valid) {
            buildUiBindings();
            const ui2::UiDocDrawStats docStats =
                ui2::drawDocument(c, m_uiDoc, m_uiBindings, w, h,
                                  ui2::frameFont());
            // 锚点节流：**第 1 帧** + 之后每 60 帧一行 `UIDOC-BOUNDS` 加一行
            // `UIDOC-STATS`。
            //
            // 为什么非要有第 1 帧那一次：验收脚本要量"绑定随时间变化"，而
            // 第 60 帧往往**正好**是脚本扣血的那一帧（hud_damage.vks 就是
            // `frame == 60`）。只在 60 的整数倍打，拿到的样本全在扣血之后，
            // 血条 fill 一条直线 —— 明明绑对了也证明不了，看起来像绑定没通。
            // 第 1 帧一定是"满血"，和后面一对比，变化才看得见。
            if (++m_uiDocFrames == 1 || m_uiDocFrames % 60 == 0) {
                const auto placed = ui2::placeDocument(m_uiDoc, m_uiBindings, w, h);
                for (const auto& p : placed) {
                    VK_LOG_INFO("UIDOC-BOUNDS id=%s type=%s x=%.1f y=%.1f "
                                "w=%.1f h=%.1f fill=%.1f",
                                p.id.c_str(), ui2::typeName(p.type),
                                static_cast<double>(p.rect.x),
                                static_cast<double>(p.rect.y),
                                static_cast<double>(p.rect.w),
                                static_cast<double>(p.rect.h),
                                static_cast<double>(p.fill.w));
                }
                // `drawn` 用 drawDocument 真返回的数，别写死 1 —— 写死之后
                // 这份日志只能证明"进了绘制函数"，证明不了"画了几个"。
                VK_LOG_INFO("UIDOC-STATS widgets=%d drawn=%d skipped=%d "
                            "texts=%d out=%ux%u bindings=%d",
                            static_cast<int>(placed.size()),
                            docStats.widgets, docStats.skipped, docStats.texts,
                            w, h,
                            static_cast<int>(m_uiBindings.numbers.size()));
            }
        }

        // ② 字幕框（C2）—— 在 HUD 之后、暂停菜单之前。
        //    顺序有讲究：对话框要盖住血条（左下角会重叠），但暂停菜单要盖住
        //    对话框（暂停时对话还在，不该压在菜单上面）。
        const dialogue::DialogueSystem& dlgSys = dialogue::activeDialogueSystem();
        if (const dialogue::DialoguePlayer* p = dlgSys.activePlayer(activeScene())) {
            SubtitleModel sm;
            sm.visible = true;
            sm.id = p->currentId();
            if (const dialogue::DlgNode* n = p->current()) {
                sm.speaker = n->speaker;
                sm.text = p->shownText();
                sm.totalChars = p->lineChars();
                sm.shownChars = p->revealed();
                sm.typing = !p->fullyRevealed();
                // ⚠ 选项**等正文出完才显示**：一边打字一边把选项摆出来，
                //   玩家会先看到按钮再读到问题，而"按空格"到底是"补完
                //   文字"还是"选第一个"就说不清了。
                sm.atChoice = p->atChoice() && p->fullyRevealed();
                if (sm.atChoice) {
                    sm.focus = m_dlgFocus;
                    sm.choices.reserve(n->choices.size());
                    for (const dialogue::DlgChoice& ch : n->choices)
                        sm.choices.push_back(ch.text);
                }
            }
            m_subtitle.build(c, sm, w, h, ui2::frameFont());
        }

        // ③ 暂停菜单盖在最上面
        if (m_lastUiState > 0)
            m_pause.build(c, w, h, ui2::frameFont(), m_focus.focusId());
    });

    VK_LOG_INFO("GAME-INIT ready — state=%s entities=%zu bodies=%d scripts=%zu",
                m_stateStack.topName(), activeScene().world().entityCount(),
                m_binding.bodies, m_scripts->scriptCount());
}

// ---------------------------------------------------------------- UI 文档绑定
//
// 谁往绑定表里填：**宿主**。脚本 `setHealth` / `setFlag` 改的是 HudModel 与
// GameFlags，这里每帧把它们抄成"字符串 → 数"的一张表，`UiDocument` 只读。
// 于是 UI 不认识脚本引擎、不认识 ECS —— 而"脚本变量能驱动界面文本"这件事
// 成立（`{affection}` 就是脚本里那个旗标）。
//
// ⚠ 每帧重建而不是增量维护：旗标可能被脚本**删掉**（`clear()`），增量维护
//   会留下"上一个场景的旗标还在界面上"；而这张表只有几十项，重建的代价
//   与一次 `std::map` 遍历同量级。
void RuntimeApp::buildUiBindings() {
    m_uiBindings.clear();
    for (const auto& kv : m_flags.entries())
        m_uiBindings.setNumber(kv.first, static_cast<float>(kv.second));

    // 保留名放最后 = **HUD 口径优先**：旗标里也可以叫 `health`（某些玩法会这么
    // 写），但界面上的 `{health}` 必须是血条那个数 —— 否则血条与数字不一致，
    // 而这种不一致只在"某个脚本恰好设了同名旗标"时出现，几乎查不到。
    m_uiBindings.setNumber("health", m_hudModel.health);
    m_uiBindings.setNumber("maxHealth", m_hudModel.maxHealth);
    m_uiBindings.setNumber("healthShown", m_hudModel.healthShown);
    m_uiBindings.setNumber("healthRatio", m_hudModel.healthRatio());
    m_uiBindings.setNumber("ammo", static_cast<float>(m_hudModel.ammo));
    m_uiBindings.setText("hint", m_hudModel.hint);
}

void RuntimeApp::loadStartScene() {
    if (m_cfg.scenePath.empty()) {
        VK_LOG_INFO("GAME-SCENE path='' entities=%zu (empty world)",
                    activeScene().world().entityCount());
        return;
    }
    const std::string resolved = assets::resolveAssetPath(m_cfg.scenePath);
    const scene::SceneIoResult r =
        scene::loadScene(activeScene(), assets(), resolved);
    if (!r.ok) {
        // 场景加载失败**不该**让运行时起不来：窗口照开，世界是空的，
        // 日志里说清楚原因（出货版给玩家看"一片空白"至少要有据可查）。
        VK_LOG_ERROR("GAME-SCENE load failed path='%s' error=%s",
                     resolved.c_str(), r.error.c_str());
        return;
    }
    VK_LOG_INFO("GAME-SCENE path=%s entities=%d skipped=%d",
                resolved.c_str(), r.entities, r.skipped);

    // 读→写往返自检（`--dump-scene`）：把**刚读进来**的场景再写一份。
    // 单独测读路径是不够的 —— `saveScene` 是另一套代码，少写一个字段
    // 在"只读不写"的验证里永远暴露不出来。
    if (!m_cfg.dumpScenePath.empty()) {
        const scene::SceneIoResult w =
            scene::saveScene(activeScene(), m_cfg.dumpScenePath);
        if (w.ok)
            VK_LOG_INFO("GAME-SCENE dumped path=%s entities=%d skipped=%d",
                        m_cfg.dumpScenePath.c_str(), w.entities, w.skipped);
        else
            VK_LOG_ERROR("GAME-SCENE dump failed path=%s error=%s",
                         m_cfg.dumpScenePath.c_str(), w.error.c_str());
    }
}

// ---------------------------------------------------------------- 世界控制
// （C6 第二批：`spawn` / `loadScene` / 剧情旗标）

bool RuntimeApp::requestSceneSwitch(const std::string& relPath) {
    if (relPath.empty()) {
        VK_LOG_WARN("RUNTIME-SCENE-REQ rejected reason=empty-path");
        return false;
    }
    const std::string resolved = assets::resolveAssetPath(relPath);

    // 先验"能打开"。理由不是怕 `loadScene` 崩，而是**怕受理一个注定失败的
    // 请求**：受理了就得等下一帧才知道失败，而那一刻旧世界已经被清掉了
    // （`loadSceneFromJson` 第一件事就是 `scene.clear()`）。脚本那边看到的
    // 现象是"`loadScene` 返回了 true，然后屏幕黑了" —— 最难查的一类。
    // 在这里挡掉，拼错路径当场报错、旧场景原地不动。
    {
        std::ifstream probe(resolved, std::ios::binary);
        if (!probe.good()) {
            VK_LOG_WARN("RUNTIME-SCENE-REQ rejected path=%s reason=cannot-open "
                        "(nothing was scheduled)",
                        resolved.c_str());
            return false;
        }
    }

    // 同一帧内连叫两次：后说的算（"先切 A 再切 B"的结果只该是 B）。
    // 留一行 INFO 是因为"我明明写了切 A"这类困惑必须有据可查。
    if (!m_pendingScenePath.empty() && m_pendingScenePath != relPath) {
        VK_LOG_INFO("RUNTIME-SCENE-REQ superseded old='%s' new='%s'",
                    m_pendingScenePath.c_str(), relPath.c_str());
    } else {
        VK_LOG_INFO("RUNTIME-SCENE-REQ path=%s (applies at the next frame start)",
                    resolved.c_str());
    }
    m_pendingScenePath = relPath;
    return true;
}

void RuntimeApp::applyPendingSceneSwitch() {
    if (m_pendingScenePath.empty()) return;
    const std::string rel = m_pendingScenePath;
    m_pendingScenePath.clear();
    const std::string resolved = assets::resolveAssetPath(rel);

    // ① 先让"函数内静态单例"撒手。这两个系统各自持有一个**指向旧世界实体**
    //    的播放器；不 reset 的话，换场景后它们会继续 tick 一个已经不存在的
    //    实体（表现：字幕/运镜卡在上一关的最后一帧，而且点不动）。
    //    ⚠ 顺序：**先**这两个，再动场景。反过来的话 reset 里读场景就已经
    //      在新世界上了。
    sequence::activeSequencerSystem().resetRuntime(activeScene());
    dialogue::activeDialogueSystem().resetRuntime(activeScene());
    // 导演覆盖也得撒手：它记着的是**旧世界**的 guid。不清的话新场景里
    // 那一串 hex 找不到实体 → 每帧一条 `CAM-CUT-MISS`，然后回落到 priority
    // （画面是对的，但日志被刷满，而且"新场景第一镜"要多等一帧）。
    scene::activeCameraSystem().clearDirectorCamera();
    m_cutSig.clear();

    // ② 玩家控制器握着实体句柄 + 物理世界指针，必须在新世界建好之前放掉。
    if (m_player) {
        m_player->detach();
        m_player.reset();
    }

    // ③ 3D 声源跟着旧世界一起没了，先掐掉（2D 音效/音乐不停：那是"整局"
    //    的声音，换关卡把 BGM 打断才是 bug）。
    m_audio.stopAll();

    // ④ 剧情旗标**不清**。换场景要留着 —— 那正是分支的用法。
    //    只有"新游戏"才清，那是将来 New Game 按钮的事。

    const scene::SceneIoResult r =
        scene::loadScene(activeScene(), assets(), resolved);
    if (!r.ok) {
        // 打开成功但解析失败：`loadSceneFromJson` 已经 `clear()` 过了，
        // 只剩一个空世界。这时**必须**把话说清楚 —— 空世界 + 一句 ERROR
        // 至少能查；换成静默的话就是"换了关之后什么都没有"。
        VK_LOG_ERROR("RUNTIME-SCENE-SWITCH failed path=%s error=%s "
                     "(the world is now empty)",
                     resolved.c_str(), r.error.c_str());
        return;
    }

    // ⑤ 与 `onInit` 同一套重建：物理 → 脚本 → 出生点 → 玩家接管。
    buildPhysics();
    preloadScripts();   // 内部先 `clear()`，所以旧实例不会带着旧实体活着
    spawnPlayerFromGameStart(activeScene(), assets(), m_cfg.player,
                             m_binding.world ? m_binding.world->heightfield()
                                             : nullptr);
    m_player = std::make_unique<PlayerController>(m_input);
    const bool attached =
        m_player->attach(activeScene(), m_binding.world.get());
    if (attached) {
        activeScene().camera().setUseGlobalInput(false);
    } else {
        // 新场景没有角色控制器是**合法**的（菜单场景 / 纯过场场景）。
        // 但不能静默：日志里要看得出"这一关没有玩家"。
        m_player.reset();
    }

    ++m_sceneSwitches;
    VK_LOG_INFO("RUNTIME-SCENE-SWITCH path=%s entities=%d skipped=%d "
                "bodies=%d scripts=%zu player=%d count=%d",
                resolved.c_str(), r.entities, r.skipped, m_binding.bodies,
                m_scripts ? m_scripts->scriptCount() : 0, attached ? 1 : 0,
                m_sceneSwitches);
}

ecs::Entity RuntimeApp::spawnModelEntity(const std::string& relPath,
                                         glm::vec3 pos) {
    if (relPath.empty()) {
        VK_LOG_WARN("RUNTIME-SPAWN-FAIL reason=empty-path");
        return ecs::Entity{};
    }
    const std::string resolved = assets::resolveAssetPath(relPath);
    try {
        // ---- .obj：单网格（与编辑器拖放同一条分派）----
        if (assets::isObjPath(relPath)) {
            assets::Mesh* mesh = assets().loadModel(resolved, relPath);
            if (!mesh || !mesh->valid()) {
                VK_LOG_WARN("RUNTIME-SPAWN-FAIL path=%s reason=mesh-load",
                            relPath.c_str());
                return ecs::Entity{};
            }
            const ecs::Entity e = activeScene().createObject(relPath);
            ecs::World& w = activeScene().world();
            if (auto* t = w.get<ecs::TransformComponent>(e)) t->position = pos;
            ecs::MeshComponent mc;
            mc.mesh = mesh;   // params 留 None：几何由文件决定
            w.add<ecs::MeshComponent>(e, mc);
            // ⚠ OBJ 不带材质（格式里就没有）。不给的话渲染层拿到的是空
            //   MaterialComponent —— 那等于**看不见**。这里给一块中性灰，
            //   与编辑器 `paletteMaterial(..., "editor_obj_default")` 同一个
            //   意思（同一个资产名，两边颜色不会漂）。
            uint8_t c[4] = {190, 190, 195, 255};
            assets::Material* mat = assets().makeMaterial(
                "editor_obj_default", assets().solid("editor_obj_default_tex", c),
                glm::vec4(1.0f), 0.6f, 0.0f);
            w.add<ecs::MaterialComponent>(e, ecs::MaterialComponent{mat});
            VK_LOG_INFO("RUNTIME-SPAWN entity=%u path=%s pos=(%.2f,%.2f,%.2f) "
                        "skinned=0",
                        e.id, relPath.c_str(), static_cast<double>(pos.x),
                        static_cast<double>(pos.y), static_cast<double>(pos.z));
            return e;
        }

        // ---- 其余按 FBX / glTF 分派（两者都产出 Model + SubMesh）----
        assets::Model* model = assets::isFbxPath(relPath)
                                   ? assets().loadFBX(resolved, relPath)
                                   : assets().loadGLTF(resolved, relPath);
        if (!model || !model->valid()) {
            VK_LOG_WARN("RUNTIME-SPAWN-FAIL path=%s reason=model-load",
                        relPath.c_str());
            return ecs::Entity{};
        }
        scene::ModelInstanceOptions opt;
        opt.subNameSuffix = "_spawn";
        opt.position = pos;
        // 实体名取文件名主干（`models/slime.vmesh` → `slime`），而不是整条
        // 路径：Hierarchy / 日志里出现 `models/slime.vmesh` 这种名字没法看。
        // ⚠ 不做**唯一化**（编辑器那边有 `uniqueRootName`）：刷十只史莱姆
        //   就该叫十次 `slime`，玩家在场景里本来就是这么理解的。
        {
            std::string stem = relPath;
            const size_t slash = stem.find_last_of("/\\");
            if (slash != std::string::npos) stem = stem.substr(slash + 1);
            const size_t dot = stem.find_last_of('.');
            if (dot != std::string::npos) stem = stem.substr(0, dot);
            opt.rootName = stem.empty() ? std::string("Spawned") : stem;
        }
        opt.autoPlay = true;
        opt.sourcePath = relPath;    // 打开 MODEL-INSTANCE 锚点
        // ⚠ `dropToGround` **关着**：spawn 的位置是**调用方给的世界坐标**，
        //   不是"请把脚放地上"。刷一个悬空的宝箱是正常需求，自动落地会
        //   让"我在天上刷了一只鸟"变成"地上多了一只鸟"。
        const scene::ModelInstanceResult inst =
            scene::instantiateModel(activeScene(), model, opt);
        if (!inst.root.valid()) {
            VK_LOG_WARN("RUNTIME-SPAWN-FAIL path=%s reason=instantiate",
                        relPath.c_str());
            return ecs::Entity{};
        }
        VK_LOG_INFO("RUNTIME-SPAWN entity=%u path=%s pos=(%.2f,%.2f,%.2f) "
                    "skinned=%d joints=%d meshes=%zu",
                    inst.root.id, relPath.c_str(), static_cast<double>(pos.x),
                    static_cast<double>(pos.y), static_cast<double>(pos.z),
                    inst.skinned ? 1 : 0, inst.joints, inst.entities.size());
        return inst.root;
    } catch (const std::exception& ex) {
        // 加载器会抛（坏文件 / 不支持的版本）。这一层必须兜住：脚本的
        // 一条 `spawn()` 不该把整局游戏带走。
        VK_LOG_WARN("RUNTIME-SPAWN-FAIL path=%s reason=exception what=%s",
                    relPath.c_str(), ex.what());
        return ecs::Entity{};
    }
}

void RuntimeApp::buildPhysics() {    // 与编辑器**同一份**绑定逻辑（engine/physics/ScenePhysics.cpp）——
    // 这是"编辑器里调好的手感，出包一样"的结构保证。
    m_binding = physics::buildSceneWorld(activeScene());
    if (!m_binding.enabled) {
        VK_LOG_INFO("GAME-PHYS disabled by scene settings");
        return;
    }
    VK_LOG_INFO("GAME-PHYS world built — %d body(ies) (ground=%d triggers=%d "
                "materials=%d) from %d rigid entities",
                m_binding.bodies, m_binding.ground ? 1 : 0, m_binding.triggers,
                m_binding.materials, m_binding.rigidEntities);
}

void RuntimeApp::preloadScripts() {
    if (!m_scripts) return;
    m_scripts->clear();
    int failed = 0;
    std::size_t count = 0;
    activeScene().world().each<ecs::ScriptComponent>(
        [&](ecs::Entity, ecs::ScriptComponent& s) {
            if (!s.enabled || s.path.empty()) return;
            ++count;
            vks::ScriptAsset* a = m_scripts->get(s.path);
            if (a && !a->lastError().ok()) ++failed;
        });
    if (count)
        VK_LOG_INFO("GAME-SCRIPT precompiled %zu reference(s), %d with errors",
                    count, failed);
}

// ---------------------------------------------------------------- 每帧

void RuntimeApp::onUpdate(float dt) {
    // 时间缩放的权威在服务面（状态可以改它，M3 的子弹时间就靠这个）
    m_loop.config().timeScale = m_svc.timeScale;

    // ---- 待执行的换场景请求（C6 第二批）----
    // ⚠ **必须是这一帧的第一件事**，理由有两条：
    //   ① 上一帧的脚本可能在 tick 里叫了 `loadScene`，此刻没有任何人
    //      在遍历世界 —— 销毁它是安全的；
    //   ② 放在渲染/输入之后会让"新场景的第一帧"混进旧场景的输入与相机状态
    //      （表现是换关瞬间镜头闪一下上一个位置）。
    // 没请求时它是一条 if + 空字符串判断，代价可忽略。
    applyPendingSceneSwitch();

    // 失焦降频：GLFW 没有"焦点变化"以外更便宜的做法，每帧读一次窗口属性
    // 就是最省的（`glfwGetWindowAttrib` 是纯状态查询，不触发任何系统调用）。
    // ⚠ GLFW 里**没有** glfwGetWindowFocus —— 焦点就是窗口属性 GLFW_FOCUSED。
    const bool focused =
        glfwGetWindowAttrib(window().handle(), GLFW_FOCUSED) != 0;

    m_renderedWorldThisFrame = false;

    // 输入采样每帧一次（在固定步之前）。鼠标位移在这里被消费 ——
    // 之后任何地方再读 Input::mouseDelta 都会拿到 0。
    m_input.poll();

    LoopCallbacks cb;
    // ① 事件泵：Application::run 里已经 pollEvents 过，这里不重复。
    //
    // ⚠ **UI tick 放在这里**（不是 fixedUpdate 里）：这是"在状态栈之外"
    //   最直接的位置 —— pollInput 每帧无条件调一次，不受 blocksUpdate
    //   影响，也不受 timeScale 影响（将来做子弹时间 UI 不该冻住）。
    //   同时它在 fixedUpdate **之前**，所以 `route()` 天然早于
    //   PlayerController::update() —— 顺序是路线图 U6 的硬要求。
    cb.pollInput = [this] { tickUi(1.0f / 60.0f); };
    // ② 固定步：状态栈按 blocksUpdate 决定谁收到
    cb.fixedUpdate = [this](float step) {
        m_svc.frame = time().frameCount();
        m_stateStack.update(m_svc, step);
    };
    // ③ 变量更新
    cb.variableUpdate = [this](float vdt) {
        if (m_svc.updateVariable) m_svc.updateVariable(vdt);
    };
    // ④ 渲染
    cb.render = [this] {
        m_stateStack.render(m_svc);
        // 本帧没有任何状态画世界（例如不透明的 MainMenu 在栈顶）→ 仍然
        // 提交一帧空场景：ImGui 的帧必须闭环，交换链也要推进，
        // 否则窗口会一直停在上一帧（甚至第一帧都没呈现过）。
        if (!m_renderedWorldThisFrame && m_menuScene)
            renderer().drawFrame(*m_menuScene);
    };

    m_loop.tick(dt, focused, cb);

    // ---- 音频（G14-5）----
    //
    // ⚠ 放在 `tick` **之后**：本帧的相机姿态 / 角色位置 / 暂停状态都已经
    //   定下来了，listener 晚一帧就会"声音追着镜头跑"。
    // ⚠ 用 `dt` 而不是固定步长：`dt` 只参与**包络插值**（ducking / 音量平滑），
    //   播放位置由声卡 block 推进（§3.6）。所以 `timeScale=0` 时音乐照常。
    syncAudio(dt, focused);

    // ---- UI 动作 → 状态迁移（U6/U8）----
    //
    // ⚠ 迁移**只在这里**做：本帧的 m_stateStack.update 已经跑完，此刻改栈
    //   不会让遍历中的 vector 失效（GameStateStack.h 的第 2 条约定）。
    //   tickUi / Paused::onUpdate 都只置意图位。
    if (m_svc.popRequested) {
        m_svc.popRequested = false;
        m_stateStack.pop(m_svc);
    } else if (m_pauseRequested) {
        m_pauseRequested = false;
        // 只有 Playing 能暂停：Boot / Loading / MainMenu / GameOver 压一个
        // 暂停菜单没有意义（而且 MainMenu 本来就不透明，菜单会画在黑底上）。
        if (std::strcmp(m_stateStack.topName(), "Playing") == 0)
            m_stateStack.push(makeBuiltinState(StateId::Paused), m_svc);
    }

    // 栈空 = 没有任何阶段了 → 正常退出（不黑窗卡死）
    if (m_stateStack.empty() || m_svc.quitRequested) {
        VK_LOG_INFO("GAME-STATE stack empty / quit requested → shutting down");
        window().invalidate();
    }
}

// ---------------------------------------------------------------- 音频

void RuntimeApp::syncAudio(float dt, bool focused) {
    // 设备不可用 / 关掉了：仍然要 `update`（失败重试计时、AUDIO-STATS 计数），
    // 但不去枚举声源 —— 那是白跑一遍 ECS。
    if (!m_audio.running()) {
        m_audio.update(dt);
        return;
    }

    // ---- 失焦静音（只有用户显式打开才生效，§3.6 表第三行）----
    if (m_cfg.audio.muteOnFocusLost)
        m_audio.setMuted(m_cfg.audio.mute || !focused);

    // ---- listener = 相机 ----
    //
    // ⚠ 引擎是 **Z-up**，所以世界 up 轴就是 +Z（不是 +Y）。
    //   写错了 pan 会整体歪 90°，而且"左/右"听起来只是"薄一点" ——
    //   这种错只能靠断言（spatial_left_source_left_louder）抓。
    const scene::Camera& cam = activeScene().camera();
    glm::vec3 lp = cam.position();
    // 允许场景里挂一个 AudioListenerComponent 覆盖**听者位置**
    // （例如做耳朵在角色头上、而相机在身后 3 m 的第三人称混音）。
    // ⚠ 只取位置，不取朝向：朝向的唯一权威是 CameraSystem（它混合了
    //   多个来源），实体旋转不一定等于相机朝向，取错了是静默的整体歪。
    bool listenerFromEntity = false;
    activeScene().world().each<ecs::AudioListenerComponent>(
        [&](ecs::Entity e, const ecs::AudioListenerComponent& c) {
            if (listenerFromEntity || !c.enabled) return;
            lp = glm::vec3(activeScene().worldMatrix(e)[3]);
            listenerFromEntity = true;
        });
    {
        const glm::vec3 f = cam.forwardAxis();
        const glm::vec3 up(0.0f, 0.0f, 1.0f);
        const float a[3] = {lp.x, lp.y, lp.z};
        const float b[3] = {f.x, f.y, f.z};
        const float u[3] = {up.x, up.y, up.z};
        m_audio.setListener(a, b, u);
    }
    if (listenerFromEntity && !m_audioListenerLogged) {
        // 只打一次就够：每帧打会把日志淹掉（R15）
        m_audioListenerLogged = true;
        VK_LOG_INFO("AUDIO-LISTENER source=entity pos=%.2f,%.2f,%.2f",
                    static_cast<double>(lp.x), static_cast<double>(lp.y),
                    static_cast<double>(lp.z));
    }

    // ---- 声源 → SourceUpdate ----
    //
    // ⚠ 暂停语义（§3.6）：**BGM 继续，SFX/Voice 淡出**。
    //   做法不是"停掉整个音频系统"，而是把 SFX/Voice 报成 `playing=false`
    //   → `applySources` 走它本来就会走的 20 ms 淡出（不新增第三条停止路径）。
    const bool paused = pausedNow();

    m_audioSrcs.clear();
    activeScene().world().each<ecs::AudioSourceComponent>(
        [&](ecs::Entity e, const ecs::AudioSourceComponent& a) {
            if (a.clip.empty()) return;

            audio::SourceUpdate u;
            // ⚠ id 必须带代际：实体槽位会复用，只用 `id` 的话"上一具尸体
            //   的音效"会被认成"新实体的音效"，一直播下去（ABA）。
            u.id = (static_cast<std::uint64_t>(e.generation) << 32) | e.id;
            u.clip = a.clip;
            u.bus = (a.bus >= 1 && a.bus <= 3) ? a.bus
                                               : static_cast<int>(audio::BusId::SFX);
            u.loop = a.loop;
            u.spatial = a.spatial;
            u.playing = a.playOnAwake;
            if (paused && u.bus != static_cast<int>(audio::BusId::Music))
                u.playing = false;
            u.volume = a.volume;
            u.pitch = a.pitch;
            u.minDist = a.minDist;
            u.maxDist = a.maxDist;
            u.rolloff = a.rolloff;
            if (a.spatial) {
                const glm::vec3 wp = glm::vec3(activeScene().worldMatrix(e)[3]);
                u.pos[0] = wp.x;
                u.pos[1] = wp.y;
                u.pos[2] = wp.z;
            }
            m_audioSrcs.push_back(u);
        });

    m_audio.applySources(m_audioSrcs.data(), m_audioSrcs.size());
    m_audio.update(dt);
}

// ---------------------------------------------------------------- UI tick

bool RuntimeApp::pausedNow() const {
    // ⚠ 暂停的权威是**状态栈的栈顶名**，不是某个 bool 成员。
    //   多一个 bool 就多一份可能与栈不一致的状态（最典型的症状是
    //   "菜单关掉了但角色还是不能动"）。
    return std::strcmp(m_stateStack.topName(), "Paused") == 0;
}

void RuntimeApp::tickUi(float dt) {
    (void)dt;

    // 输出尺寸与 PostProcess 画 UI 时**同源**（见 Renderer::uiOutputExtent）：
    // 拿窗口尺寸或渲染分辨率算命中框都会整体偏一小段比例 —— 屏幕上看着
    // 光标就在按钮上，却点不中。
    const VkExtent2D out = renderer().uiOutputExtent();
    const uint32_t outW = out.width;
    const uint32_t outH = out.height;

    const bool paused = pausedNow();

    // ---- ① 光标 / 输入上下文：**只在状态变化时**动 ----
    //
    // 每帧都 pushContext 会让上下文栈无限增长（然后 Gameplay 永远回不来）。
    const int state = paused ? 1 : 0;
    if (state != m_lastUiState) {
        m_lastUiState = state;
        if (paused) {
            // 压 Menu：Gameplay 整组**结构性地**失效 —— 于是"暂停时按 W
            // 角色不动"不是靠某个 if，而是靠只有栈顶上下文被求值。
            m_input.pushContext("Menu");
            // 菜单要用鼠标点，光标得看得见，并且锁在窗口内（GLFW 的
            // DISABLED 之外没有单独的 clip 模式；可见是菜单的硬需求）。
            // ★ P5：进菜单 = **退出操作态**。Esc 因此天然满足"退出操作时
            //   显示鼠标"，不需要再为 Esc 单独写一条路径。
            m_mouseCaptured = false;
            window().setCursorVisible(true);
            m_focus.clearFocus();
        } else {
            if (m_input.depth() > 1) m_input.popContext();
            // ★ P5：回到 Gameplay **不自动捕获** —— 玩家必须再点一次视口。
            //   否则"从菜单点继续 → 鼠标突然消失、镜头开始跟着鼠标转"，
            //   玩家会以为鼠标失灵了。
            m_mouseCaptured = false;
            window().setCursorVisible(true);
            m_focus.endFrame();
        }
        VK_LOG_INFO("UI2-PAUSE open=%d context=%s cursor=%d",
                    state, m_input.topContext().c_str(),
                    window().cursorVisible() ? 1 : 0);
    }

    // ---- ①b 点击视口 → 进入操作态（P5）----
    //
    // 只有 Gameplay 且未捕获时才收这个左键：暂停菜单里的点击属于 UI。
    // 命中区域就是整个客户区 —— 游戏态没有吃点击的 UI（HUD 的血条只是
    // 绘制，没有交互）。将来加可点击 HUD 时，这里要换成 ui2 的 hit test。
    if (!paused && !m_mouseCaptured && core::Input::mousePressed(0)) {
        m_mouseCaptured = true;
        m_mouseIdleLogged = false;          // 下次退出操作时再提示一次
        window().setCursorVisible(false);   // DISABLED = 隐藏 + 锁定 + 无限转
        VK_LOG_INFO("GAME-MOUSE captured=1 reason=click "
                    "(WASD on / cursor hidden; Esc releases)");
    }
    if (paused && m_mouseCaptured) {
        // 兜底：任何绕过 state 变化的路径（脚本强切状态栈）都不该留在捕获态
        m_mouseCaptured = false;
        window().setCursorVisible(true);
        VK_LOG_INFO("GAME-MOUSE captured=0 reason=paused");
    }

    // ---- ② 暂停开关（状态迁移的**意图**，真正改栈在 onUpdate 末尾）----
    if (paused) {
        // 暂停时 Gameplay 整组失效，所以 ESC 走 Menu 的 MenuBack。
        // ⚠ 用 wasPressed（边沿）：按住 ESC 会每帧开关一次。
        if (m_input.wasPressed("MenuBack")) m_svc.popRequested = true;
    } else if (m_input.wasPressed("Pause")) {
        m_pauseRequested = true;
    }

    // ---- ③ 没有菜单 → UI 不吃任何输入 ----
    if (!paused) {
        m_svc.uiAction.clear();

        // ---- 对话输入（C2）----
        // ⚠ 只在这一支里处理：暂停时 Space 是菜单确认键，两条路各吃一次
        //   按下会出现"在暂停菜单里选掉了一个对白选项"。
        dialogue::DialogueSystem& dlg = dialogue::activeDialogueSystem();
        if (const dialogue::DialoguePlayer* p = dlg.activePlayer(activeScene())) {
            // 换行 → 焦点复位（见 RuntimeApp.h 的 m_dlgFocusId 说明）
            if (p->currentId() != m_dlgFocusId) {
                m_dlgFocusId = p->currentId();
                m_dlgFocus = 0;
            }
            const bool atChoice = p->atChoice() && p->fullyRevealed();
            if (atChoice) {
                // 方向键**不进 InputMap**：它不是玩法动作（重绑没有意义），
                // 而且 Menu 上下已经用 Up/Down 了，两处共用一个物理键才一致。
                // ⚠ 刻意**不**用 W/S：那是移动键，同一个物理键既是"后退"
                //   又是"选下一项"，在选项上按一下会连人一起往后退。
                const int n = p->choiceCount();
                if (core::Input::keyPressed(GLFW_KEY_UP))
                    m_dlgFocus = (m_dlgFocus + n - 1) % n;
                if (core::Input::keyPressed(GLFW_KEY_DOWN))
                    m_dlgFocus = (m_dlgFocus + 1) % n;
                if (m_dlgFocus < 0 || m_dlgFocus >= n) m_dlgFocus = 0;
            }
            if (m_input.wasPressed("Jump")) {
                if (atChoice)
                    dlg.choose(activeScene(), m_dlgFocus);
                else
                    dlg.pressAdvance(activeScene());   // 两段式：先补完再翻页
            }
        }

        // HUD 一直有内容（血条 + 准星），所以这条恒为 true。
        // `ui.enabled=0` 的守卫不在这里 —— 在 Renderer：通道没建就根本
        // 不会走到 buildOverlay（逐位不变的保证在那儿）。
        ui2::setGameUiActive(true);
        return;
    }

    m_focus.beginFrame(m_pause.focusItems(outW, outH));

    // 打开菜单就**立起第一项**（"继续"）。不这么做的话，菜单刚出现时
    // 没有任何高亮，玩家必须按一下方向键才看得到选中的是哪个 —— 而且
    // `UI2-FOCUS` 要等到那一下才打，自动化根本分不清"菜单还没开"和
    // "开了但没焦点"。（`focusFirst` 内部只在**变化时**打锚点。）
    if (!m_focus.hasFocus()) m_focus.focusFirst();

    // 鼠标：窗口坐标 → 输出像素（高 DPI 下 framebuffer != window）
    double cx = 0.0, cy = 0.0;
    core::Input::cursorPos(&cx, &cy);
    int winW = 1, winH = 1;
    window().windowSize(&winW, &winH);
    const float px = static_cast<float>(
        cx * (winW > 0 ? static_cast<double>(outW) / winW : 1.0));
    const float py = static_cast<float>(
        cy * (winH > 0 ? static_cast<double>(outH) / winH : 1.0));

    // 悬停跟手：鼠标停在某个按钮上就聚焦它。不这么做的话，玩家"看着
    // 光标在按钮上"却要点两次（第一次只是把焦点拉过来）。
    if (m_focus.hitIndex(px, py) >= 0) m_focus.hoverAt(px, py);

    ui2::UiNav nav;
    nav.moveY = (m_input.wasPressed("MenuDown") ? 1 : 0) -
                (m_input.wasPressed("MenuUp") ? 1 : 0);
    nav.moveX = (m_input.wasPressed("MenuRight") ? 1 : 0) -
                (m_input.wasPressed("MenuLeft") ? 1 : 0);
    nav.tab = m_input.wasPressed("MenuTab");
    nav.activate = m_input.wasPressed("MenuConfirm");

    // 左键点中按钮 = 激活。走**同一条 route**，不另开一条路径 ——
    // 两条路径迟早会在"点中和键盘选中不一样"这种地方分叉。
    if (!nav.activate && core::Input::mousePressed(GLFW_MOUSE_BUTTON_LEFT)) {
        const int hit = m_focus.hitIndex(px, py);
        if (hit >= 0) {
            m_focus.setFocus(m_focus.items()[static_cast<std::size_t>(hit)].id);
            nav.activate = true;
        }
    }

    std::string act;
    const bool consumed = m_focus.route(nav, &act);
    (void)consumed;
    if (!act.empty()) {
        m_svc.uiAction = act;
        VK_LOG_INFO("UI2-ACTION id=%s consumed=1", act.c_str());
    } else {
        m_svc.uiAction.clear();
    }

    // 暂停菜单这一帧确实有东西要画
    ui2::setGameUiActive(true);
}

// ---------------------------------------------------------------- 仿真

// ============================================================
// 玩家四段基础动画（P3 / P4）
// ============================================================
// 状态机的**内容**已经搬到 `animation/Locomotion`（2026-10-22）：编辑器
// Play 与运行时以前各有一半（编辑器只有位移、没有走路动画），现在两边
// 调同一个函数。这里只负责三件运行时才有的事：
//   ① 取"物理真值"（PlayerController 的水平速度 / 贴地标志）；
//   ② 把 RuntimeConfig 的 player 块翻译成 LocomotionConfig；
//   ③ 轮询 `AnimKeyMapComponent` 的按键映射（角色面板里绑的那几个键）。
void RuntimeApp::syncDirectorCamera() {
    sequence::SequencerSystem& seq = sequence::activeSequencerSystem();
    scene::CameraSystem& cs = scene::activeCameraSystem();

    const std::uint64_t want = seq.shotCameraGuid();

    if (want == 0) {
        // 没有 marker / 这一段是"回关键帧轨"。释放覆盖（幂等，同值不做事）。
        cs.clearDirectorCamera();
        return;
    }

    // ⚠⚠ 先清 desired。玩家控制器在 `updateVariable` 开头就喂过一次，
    //   时间轴有 pose 时上面也喂过 —— 那份 desired 会让 CameraSystem 走
    //   "用 desired"分支，marker 选中的机位**根本不动**（画面停在玩家视角
    //   或导演轨上），而 `CAM-CUT` 日志照样打出来。表现是"切镜不生效，
    //   但日志说切了"，极难查。
    cs.clearDesiredPose();

    const bool changed = (cs.directorCamera() != want);
    cs.setDirectorCamera(want);

    if (changed) {
        // 混合时长是**这一次 marker** 的属性，不是相机的属性 —— 每次切换
        // 都重新取。0 = 硬切（不调用 blendCut，姿态直接跳过去）。
        const float b = seq.shotBlendSeconds();
        if (b > 0.0f) cs.blendCut(b);

        // 日志去重：只在"机位真的变了"打一条。`SEQ-SHOT` 已经打过一次
        // （时间轴视角），这一条是**运行时视角**，用来证明那条结论真的
        // 落到了相机上 —— 两条都要能单独数（用例分别断言）。
        const std::string sig = std::to_string(seq.shotIndex()) + "|" +
                                std::to_string(want) + "|" +
                                std::to_string(static_cast<int>(b * 100.0f));
        if (m_cutSig != sig) {
            m_cutSig = sig;
            VK_LOG_INFO("CAM-CUT-RT shot=%d cam=%s blend=%.2f",
                        seq.shotIndex(), ecs::guidToHex(want).c_str(),
                        static_cast<double>(b));
        }
    }
}

void RuntimeApp::applyCameraLensEffects() {
    const ecs::Entity e = scene::activeCameraSystem().activeEntity();
    ecs::World& w = activeScene().world();
    const auto* cc = e.valid() ? w.get<ecs::CameraComponent>(e) : nullptr;
    if (!cc) return;

    const std::uint64_t guid = w.guid(e);
    render::CameraEffectSettings& fx = renderer().cameraEffects();

    // 换相机：把**上一次自己写进去的**两项复位。
    // ⚠ 只复位这两项，别的（色差 / 暗角 / 体积覆盖）不碰 —— 那不是镜头
    //   参数管的东西，顺手清掉会让"换相机 = 画面回暖"变成一个谜。
    if (guid != m_lensCam) {
        fx.dof = false;
        fx.motionBlur = false;
        m_lensCam = guid;
    }

    // ---- 景深 ----
    float focus = 0.0f;
    float aperture = 0.0f;
    if (cc->dofEnabled) {
        focus = cc->focusDistance;
        if (cc->focusGuid != 0) {
            const ecs::Entity te = w.find(cc->focusGuid);
            if (te.valid()) {
                const glm::vec3 tp = glm::vec3(activeScene().worldMatrix(te)[3]);
                focus = glm::length(tp - activeScene().camera().position());
            }
        }
        aperture = scene::fStopToDofAperture(
            cc->fStop, cc->focal > 0.0f ? cc->focal : 50.0f);
        fx.dof = true;
        fx.dofFocus = focus;
        fx.dofAperture = aperture;
    }

    // ---- 快门（运动模糊）----
    float blur = 0.0f;
    if (cc->shutterAngle > 0.0f) {
        blur = scene::shutterToBlurStrength(cc->shutterAngle);
        fx.motionBlur = true;
        fx.motionBlurStrength = blur;
    }

    // ---- 日志（只在变化时打）----
    char sig[256];
    std::snprintf(sig, sizeof(sig), "cam=%llx dof=%d focus=%.2f ap=%.2f sh=%.0f blur=%.2f",
                  static_cast<unsigned long long>(guid), cc->dofEnabled ? 1 : 0,
                  focus, aperture, cc->shutterAngle, blur);
    if (m_lensSig != sig) {
        m_lensSig = sig;
        VK_LOG_INFO("CAM-LENS %s", sig);
    }
}

void RuntimeApp::tickPlayerAnimation(float dt) {
    if (!m_player || !m_player->attached()) return;

    // 只在初始化时求一次实体句柄？不行 —— `attach` 可能在 Play 中途重建。
    const ecs::Entity pe = m_player->entity();
    if (!pe.valid()) return;

    ecs::World& w = activeScene().world();
    ecs::SkinnedMeshComponent* sk = w.get<ecs::SkinnedMeshComponent>(pe);
    // 没有蒙皮模型（占位胶囊体）→ 这条链整段不参与，行为与引入之前一致。
    if (!sk || !sk->model || !sk->model->skeleton) return;
    const assets::Skeleton& skel = *sk->model->skeleton;

    const RuntimeConfig::PlayerVisual& pv = m_cfg.player;

    animation::LocomotionConfig cfg;
    cfg.idleClip = pv.idleClip;
    cfg.walkClip = pv.walkClip;
    cfg.runClip = pv.runClip;
    cfg.jumpClip = pv.jumpClip;
    cfg.walkSpeed = pv.walkSpeed;
    cfg.runSpeed = pv.runSpeed;
    cfg.restartOnSwitch = pv.restartOnSwitch;
    cfg.idleTriggerSec = pv.idleTriggerSec;
    // A4 步幅匹配（2026-10-30）：四个数全部来自 game.json 的 player 块。
    cfg.strideMatch = pv.strideMatch;
    cfg.strideMinRate = pv.strideMinRate;
    cfg.strideMaxRate = pv.strideMaxRate;
    cfg.walkStrideSpeed = pv.walkStrideSpeed;
    cfg.runStrideSpeed = pv.runStrideSpeed;
    cfg.strideCurve = pv.strideCurve;

    // 走 / 跑分界自动值的输入：控制器最大速度（没有就退回 4.5）
    const auto* cc = w.get<ecs::CharacterControllerComponent>(pe);
    const float maxSpeedHint = (cc && cc->maxSpeed > 0.0f) ? cc->maxSpeed : 4.5f;

    ecs::AnimatorComponent* an = w.get<ecs::AnimatorComponent>(pe);

    // ---- 按键映射（P4）：先轮询，按下就在这一帧强播 ----
    //
    // 放在状态机**之前**：同一帧里"按了键"应当压过"刚走起来"，否则玩家
    // 按一下挥手的瞬间正好在走路的话，那一下会被 loco 覆盖掉（看起来像
    // "按键没反应"，而日志里 key 是打了的 —— 最难查的一类）。
    if (const auto* km = w.get<ecs::AnimKeyMapComponent>(pe)) {
        int key = 0;
        // 运行时的判定就用 `core::Input`：Game.exe 里没有 ImGui 抢键盘
        // 这回事（编辑器侧必须换 `ui_keys::pressed`，理由见 Locomotion.h）。
        if (const std::string* clip =
                animation::pollKeyTrigger(*km, &core::Input::keyPressed, &key)) {
            animation::playTriggerClip(m_playerAnim, *sk, an, skel, *clip,
                                       km->holdSeconds, "GAME");
            VK_LOG_INFO("GAME-ANIM-KEY-TRIGGER clip=%s key=%d", clip->c_str(),
                        key);
        }
    }

    const glm::vec3 v = m_player->velocity();
    const float hs = std::sqrt(v.x * v.x + v.y * v.y);   // 水平速度（m/s）
    const bool onGround = m_player->onGround();

    // ---- 状态机（A2b）：走控制器资产时，物理真值喂进参数黑板 ----
    //
    // 三条约定（写进 docs/character_pipeline_roadmap.md，资产里按这三个名字声明）：
    //   Speed    = 水平速度 m/s（**不是**输入轴大小 —— 撞墙时输入满档但
    //              速度为 0，腿不该还在跑）
    //   Grounded = 是否贴地
    //   Jump     = 一次性 trigger（只在真正起跳的那一帧为 true）
    //
    // 用**约定名**而不是"映射表"：多一层映射的结果是每个工程的控制器都要
    // 额外配一遍映射，而真正的差异（走跑分界、跳跃条件）本来就在控制器的
    // 转移条件里 —— 那才是该写的地方。控制器没声明这三个参数时，写进去
    // 的条目不会被任何条件读到，是死数据（无害）。
    if (an && !an->controllerPath.empty()) {
        an->floatParams["Speed"] = hs;
        an->boolParams["Grounded"] = onGround;
        if (m_player->justJumped()) an->triggerParams["Jump"] = true;
        // 状态机接管 → `Locomotion` 让位。
        // ⚠ 必须 return：两套逻辑同时写 `sk.clipName` 的话，每帧互相覆盖，
        //   症状是"角色在两个动作之间高频抖动"，而日志里两边都在正常输出。
        return;
    }

    animation::updateLocomotion(m_playerAnim, *sk, an, skel, hs, onGround,
                                maxSpeedHint, dt, cfg, "GAME");
}

void RuntimeApp::simulate(float dt) {
    ++m_simFrames;

    // HUD 数据跟着**游戏时间**走（不是真实帧时长）：暂停时 Paused 截断
    // 下层 update → simulate 不跑 → 伤害数字冻在屏幕上，那是对的
    // （画面定住了，数字还在往上飘会很怪）。
    m_hudModel.tick(dt);

    runScripts(dt);

    // 顺序（与编辑器 Play 一致，见 ship_roadmap §4 G10）：
    //   脚本 → 玩家 → 物理 → 接触事件
    // 玩家必须先于物理：它写入的变换/代理速度是这一步接触求解的输入。
    //
    // ⚠ P5：**没点视口之前不推进玩家**（用户要求"必须左键点视口后才能
    //   开始交互"）。注意只门控这一处，不动 tickPlayerAnimation —— 否则
    //   人会僵在出生点一动不动，看起来像卡死；点了之后才突然开始呼吸。
    if (m_player) {
        if (m_mouseCaptured)
            m_player->update(activeScene(), m_binding.world.get(), dt);
        else if (!m_mouseIdleLogged) {
            // 只喊一次：这是"按了 W 却没反应"最常见的原因，日志里必须有。
            m_mouseIdleLogged = true;
            VK_LOG_INFO("GAME-MOUSE captured=0 (click the viewport to "
                        "take control; player input is frozen until then)");
        }
    }

    // ---- 玩家四段动画（P3）----
    // 必须排在玩家更新**之后**（要用这一帧的速度 / 贴地）、AnimatorSystem
    // **之前**（它是真正推进播放头并采样的那一步；晚一帧就会看到姿势滞后
    // 一帧，跑起来时最明显）。
    tickPlayerAnimation(dt);

    // ---- 骨骼动画（A0-3）----
    // 排在物理之前，与编辑器 Play 同位置：将来 A4 的根运动会把位移写进
    // TransformComponent，物理必须读到它这一帧的最终值。
    // A4b：相位速率曲线是"一次 tick 全体生效"的开关，设在 tick **之前**。
    m_animator.setStrideCurve(m_cfg.player.strideCurve);
    m_animator.tick(activeScene(), dt);

    if (m_binding.world) {
        // 顺序与编辑器 Play 一致：脚本 → 物理（M2 会在中间插入"玩家"，
        // 在物理之后插入"碰撞"）。
        physics::syncKinematicFromScene(activeScene(), *m_binding.world,
                                        m_binding.entityToBody, dt);
        m_binding.world->step(dt);
        physics::writeBackToScene(activeScene(), *m_binding.world,
                                  m_binding.entityToBody);
        pumpContactEvents();
    }

    if (m_playerDumpEvery > 0 &&
        (m_simFrames % static_cast<std::uint64_t>(m_playerDumpEvery)) == 0u &&
        m_player) {
        const glm::vec3 p = m_player->characterCenter();
        VK_LOG_INFO("GAME-PLAYER-POS f=%llu pos=(%.5f,%.5f,%.5f) ground=%d "
                    "arm=%.4f",
                    static_cast<unsigned long long>(m_simFrames), p.x, p.y, p.z,
                    m_player->onGround() ? 1 : 0, m_player->springArmDistance());

        // ---- 脚底 vs 地表（2026-10-22）----
        //
        // 用户报"角色在地形上脚陷进去了"。这条探针把两件事拆开：
        //   feetZ = 实体原点。**实体原点是脚底**（见 PlayerController 顶部
        //           的注释），所以它就是要和地表比的那个数；
        //   surfZ = 地形高度场在 (x,y) 处的插值高度（与贴地用的是同一个
        //           `heightAt`）。
        // gap = feetZ - surfZ。**gap < 0 就是脚在地面以下**。
        //
        // 没有地形时不打 —— 否则会在所有"平地场景"里报一堆 gap，把真正
        // 的问题淹掉。
        if (m_binding.world && m_binding.world->hasHeightfield()) {
            const ecs::Entity pe = m_player->entity();
            const auto* tc = activeScene().world().get<ecs::TransformComponent>(pe);
            const physics::Heightfield* hf = m_binding.world->heightfield();
            if (tc && hf) {
                const float surf = physics::heightAt(*hf, tc->position.x,
                                                     tc->position.y);
                VK_LOG_INFO("GAME-FEET f=%llu feetZ=%.4f surfZ=%.4f gap=%+.4f",
                            static_cast<unsigned long long>(m_simFrames),
                            static_cast<double>(tc->position.z),
                            static_cast<double>(surf),
                            static_cast<double>(tc->position.z - surf));
            }
        }
    }

    if (m_physicsDumpEvery > 0 &&
        (m_simFrames % static_cast<std::uint64_t>(m_physicsDumpEvery)) == 0u) {
        dumpPhysicsBodies("PHYS-BODY");
    }
}

int RuntimeApp::runScripts(float dt) {
    if (!m_scriptRuntime) return 0;

    // ⚠ `kSelfTimed`：仿真态由**实例自己**累加本地时间（修 P3）。
    //   以前是 `m_simFrames * dt` 再乘 timeScale —— 倍速从 1.0 改到 0.5 的
    //   瞬间，脚本看到的 `time` 会倒退一半。
    m_scriptRuntime->tick(activeScene(), vks::ScriptRuntime::kSelfTimed, dt,
                          time().frameCount());
    return static_cast<int>(m_scriptRuntime->lastStats().executed);
}

// 宿主能力只填**一次**（S0 之前是每帧每个实体重建六个 std::function ——
// 那是纯浪费，而且把"结构"藏进了循环里）。
void RuntimeApp::installScriptServices() {
    vks::ScriptServices svc;

    // ---- HUD（U8）----
    // 三个窄动作接到 m_hudModel 上（engine 不认识 runtime，所以走
    // std::function 而不是指针）。`damage` 顺带在实体头顶弹数字。
    svc.setHealth = [this](float v) { m_hudModel.setHealth(v); };
    svc.damage = [this](float v) {
        m_hudModel.setHealth(m_hudModel.health - v);
    };
    svc.popup = [this](float x, float y, float z, float amount) {
        m_hudModel.addPopup(glm::vec3(x, y, z), amount);
    };

    // ---- 音频（G14-8）----
    // `playSound` 是一次性 2D 音效（不跟实体位置走）：脚本里不知道该实体
    // 此刻在哪，跟过去反而要每次查 Transform。要 3D 就用组件。
    svc.playSound = [this](const std::string& clip) {
        m_audio.playOneShot(clip, audio::BusId::SFX, nullptr, 1.0f);
    };
    svc.stopAllSounds = [this]() { m_audio.stopAll(); };
    svc.setBusVolume = [this](const std::string& bus, float v) {
        const int idx = audio::busIndexFromName(bus.c_str());
        if (idx < 0) {
            // ⚠ 认不出就留痕：静默忽略会让"我把音量调到 0.3 了怎么没变"
            //   变成一次排查（与 busIndexFromName 返回 -1 的设计同源）。
            VK_LOG_WARN("GAME-SCRIPT setBusVolume: 未知总线 '%s'"
                        "（可用：master|music|sfx|voice）",
                        bus.c_str());
            return;
        }
        m_audio.setBusGain(static_cast<audio::BusId>(idx), v);
    };

    // ---- 玩家 / 输入（S3）----
    // 玩家是"被 PlayerController 接管的那个实体" —— 只有 runtime 知道，
    // engine 不知道（这正是把它做成注入的理由）。
    svc.act.player = [this]() {
        return m_player ? m_player->entity() : ecs::Entity{};
    };
    // ⚠ 传的是**动作名**（InputMap 的 "Jump"），不是键码。
    svc.act.pressed = [this](const std::string& a) { return m_input.wasPressed(a); };
    svc.act.held = [this](const std::string& a) { return m_input.isHeld(a); };
    svc.act.released = [this](const std::string& a) { return m_input.wasReleased(a); };
    svc.act.axis = [this](const std::string& a) { return m_input.axis(a); };

    // ---- 演出（C6 导演 API）----
    //
    // 三件都直接落到引擎里"函数内静态单例"上 —— 一个场景只该有一个播放头
    // （`activeSequencerSystem()` / `activeDialogueSystem()` 就是为这个存在
    // 的），所以宿主不自己持有第二份。
    svc.act.playSequence = [this](const std::string& path) {
        return sequence::activeSequencerSystem().cue(activeScene(), path);
    };
    svc.act.showDialogue = [this](const std::string& id) {
        // 宿主取玩家实体：它是"谁在说话"的默认宿主。没有玩家时传无效句柄
        // —— 对话本来就不依赖宿主（DLG-START 里 host=0 一样能读）。
        const ecs::Entity host = m_player ? m_player->entity() : ecs::Entity{};
        return dialogue::activeDialogueSystem().cue(activeScene(), host, id);
    };
    svc.act.playAnim = [this](ecs::Entity e, const std::string& clip) {
        return switchAnimClip(activeScene(), e, clip);
    };

    // ---- 世界控制（C6 第二批）----
    //
    // 旗标：直接从宿主自己的表上读写。`RUNTIME-FLAG` 只在**写**时打一行
    // （读太频繁，每帧打会把日志淹掉）；坏情况是"脚本设的键名拼错、
    // 另一处读不到"，而那条线索正好在这行里。
    svc.act.setFlag = [this](const std::string& name, double v) {
        m_flags.set(name, v);
        VK_LOG_INFO("RUNTIME-FLAG set name=%s value=%.4f total=%zu",
                    name.c_str(), v, m_flags.size());
        return true;
    };
    svc.act.getFlag = [this](const std::string& name) {
        return m_flags.get(name);
    };
    // 刷实体：**当场**建（不是排队）。理由见 `spawn()` 的脚本侧注释：
    // 返回值要能立刻用。ECS 容器允许在 `each` 之外追加实体，而
    // `ScriptRuntime::tick` 遍历的是它自己的实例表（`m_instances`），
    // 不是在遍历 `each<ScriptComponent>` —— 所以这一刻追加是安全的。
    svc.act.spawn = [this](const std::string& path, glm::vec3 pos) {
        return spawnModelEntity(path, pos);
    };
    svc.act.loadScene = [this](const std::string& path) {
        return requestSceneSwitch(path);
    };

    // ---- 关节（J5-4）----
    //
    // 每一条动作都要**同时**改两处，缺一处就是"脚本改了、画面没动"：
    //   ① `JointComponent`  —— 作者侧数据（Inspector / 存档看到的这份）
    //   ② `physics::rebuildJoints` —— 求解器侧（真正约束物体运动的那份）
    //
    // ⚠ 绝不用 `buildSceneWorld` 重建：它连刚体一起重来，场景里每个物体
    //   的位置与速度都回到初始值。改一个关节把整局撞回原点，没人能接受。
    //
    // ⚠ 每条失败路径都有 `GAME-JOINT` WARN：脚本侧只拿到 false，看不到
    //   原因（"没有组件"和"类型名拼错"要做的下一步完全不同）。
    const auto jointName = [this](ecs::Entity e) -> std::string {
        if (!e.valid()) return std::string("<none>");
        const std::string* n = activeScene().world().name(e);
        return n ? *n : std::string("<unnamed>");
    };
    const auto jointOf = [this](ecs::Entity e) -> ecs::JointComponent* {
        if (!e.valid()) return nullptr;
        ecs::World& w = activeScene().world();
        if (!w.alive(e)) return nullptr;
        return w.get<ecs::JointComponent>(e);
    };
    const auto refreshJoints = [this]() {
        return physics::rebuildJoints(activeScene(), m_binding);
    };

    svc.act.joints.set = [this, jointOf, refreshJoints, jointName](
                             ecs::Entity self, ecs::Entity other,
                             const std::string& kind) -> bool {
        ecs::JointComponent* jc = jointOf(self);
        if (!jc) {
            VK_LOG_WARN("GAME-JOINT set: '%s' has no Joint component "
                        "(add it in the Inspector, Physics tab)",
                        jointName(self).c_str());
            return false;
        }
        ecs::JointKind k;
        if (!ecs::parseJointKind(kind.c_str(), k)) {
            VK_LOG_WARN("GAME-JOINT set: unknown kind '%s' for '%s' "
                        "(expect fixed / ball / hinge / slider / distance)",
                        kind.c_str(), jointName(self).c_str());
            return false;
        }
        ecs::World& w = activeScene().world();
        if (!w.alive(other)) {
            VK_LOG_WARN("GAME-JOINT set: '%s' targets a dead/invalid entity",
                        jointName(self).c_str());
            return false;
        }
        if (!w.has<ecs::RigidBodyComponent>(self) ||
            !w.has<ecs::RigidBodyComponent>(other)) {
            VK_LOG_WARN("GAME-JOINT set: both ends need a RigidBody "
                        "('%s' has=%d, '%s' has=%d)",
                        jointName(self).c_str(),
                        w.has<ecs::RigidBodyComponent>(self) ? 1 : 0,
                        jointName(other).c_str(),
                        w.has<ecs::RigidBodyComponent>(other) ? 1 : 0);
            return false;
        }
        jc->kind = k;
        jc->target = w.guid(other);
        // 上一次可能被 `jointOn(e,false)` 关过 —— 重新配一次就该生效，
        // 不然脚本会陷入"我明明 joint() 了还是不动"。
        jc->enabled = true;
        const std::size_t built = refreshJoints();
        VK_LOG_INFO("GAME-JOINT set self=%u other=%u kind=%s built=%zu",
                    self.id, other.id, ecs::jointKindName(k), built);
        return true;
    };
    svc.act.joints.anchors = [this, jointOf, refreshJoints, jointName](
                                 ecs::Entity self, glm::vec3 a,
                                 glm::vec3 b) -> bool {
        ecs::JointComponent* jc = jointOf(self);
        if (!jc) {
            VK_LOG_WARN("GAME-JOINT anchors: '%s' has no Joint component",
                        jointName(self).c_str());
            return false;
        }
        jc->anchorSelf = a;
        jc->anchorTarget = b;
        refreshJoints();
        VK_LOG_INFO("GAME-JOINT anchors self=%u a=(%.3f,%.3f,%.3f) "
                    "b=(%.3f,%.3f,%.3f)",
                    self.id, a.x, a.y, a.z, b.x, b.y, b.z);
        return true;
    };
    svc.act.joints.axis = [this, jointOf, refreshJoints, jointName](
                              ecs::Entity self, glm::vec3 ax) -> bool {
        ecs::JointComponent* jc = jointOf(self);
        if (!jc) {
            VK_LOG_WARN("GAME-JOINT axis: '%s' has no Joint component",
                        jointName(self).c_str());
            return false;
        }
        jc->axis = ax;
        refreshJoints();
        VK_LOG_INFO("GAME-JOINT axis self=%u axis=(%.3f,%.3f,%.3f)", self.id,
                    ax.x, ax.y, ax.z);
        return true;
    };
    svc.act.joints.limit = [this, jointOf, refreshJoints, jointName](
                               ecs::Entity self, bool on, float lo,
                               float hi) -> bool {
        ecs::JointComponent* jc = jointOf(self);
        if (!jc) {
            VK_LOG_WARN("GAME-JOINT limit: '%s' has no Joint component",
                        jointName(self).c_str());
            return false;
        }
        if (jc->kind != ecs::JointKind::Hinge &&
            jc->kind != ecs::JointKind::Slider) {
            VK_LOG_WARN("GAME-JOINT limit: '%s' is a %s joint - only hinge / "
                        "slider support a limit",
                        jointName(self).c_str(),
                        ecs::jointKindName(jc->kind));
            return false;
        }
        jc->useLimit = on;
        jc->lowerLimit = lo;
        jc->upperLimit = hi;
        refreshJoints();
        VK_LOG_INFO("GAME-JOINT limit self=%u on=%d range=[%.3f,%.3f]",
                    self.id, on ? 1 : 0, lo, hi);
        return true;
    };
    svc.act.joints.motor = [this, jointOf, refreshJoints, jointName](
                               ecs::Entity self, float speed,
                               float force) -> bool {
        ecs::JointComponent* jc = jointOf(self);
        if (!jc) {
            VK_LOG_WARN("GAME-JOINT motor: '%s' has no Joint component",
                        jointName(self).c_str());
            return false;
        }
        if (jc->kind != ecs::JointKind::Hinge &&
            jc->kind != ecs::JointKind::Slider) {
            VK_LOG_WARN("GAME-JOINT motor: '%s' is a %s joint - only hinge / "
                        "slider support a motor",
                        jointName(self).c_str(),
                        ecs::jointKindName(jc->kind));
            return false;
        }
        // force <= 0 = 关掉。这个约定与 Inspector 里的提示一致
        // （"max force 0 = the motor cannot push at all"）—— 留一个
        // 推不动的马达开着，等于让用户以为"速度设了却没反应"。
        jc->useMotor = force > 0.0f;
        jc->motorSpeed = speed;
        jc->maxMotorForce = force;
        refreshJoints();
        VK_LOG_INFO("GAME-JOINT motor self=%u speed=%.3f force=%.3f on=%d",
                    self.id, speed, force, jc->useMotor ? 1 : 0);
        return true;
    };
    svc.act.joints.enable = [this, jointOf, refreshJoints, jointName](
                                ecs::Entity self, bool on) -> bool {
        ecs::JointComponent* jc = jointOf(self);
        if (!jc) {
            VK_LOG_WARN("GAME-JOINT enable: '%s' has no Joint component",
                        jointName(self).c_str());
            return false;
        }
        jc->enabled = on;
        const std::size_t built = refreshJoints();
        VK_LOG_INFO("GAME-JOINT enable self=%u on=%d built=%zu", self.id,
                    on ? 1 : 0, built);
        return true;
    };
    svc.act.joints.remove = [this, jointOf, refreshJoints, jointName](
                                ecs::Entity self) -> bool {
        ecs::JointComponent* jc = jointOf(self);
        if (!jc) {
            VK_LOG_WARN("GAME-JOINT remove: '%s' has no Joint component",
                        jointName(self).c_str());
            return false;
        }
        activeScene().world().remove<ecs::JointComponent>(self);
        const std::size_t built = refreshJoints();
        VK_LOG_INFO("GAME-JOINT remove self=%u built=%zu", self.id, built);
        return true;
    };
    svc.act.joints.kindOf = [jointOf](ecs::Entity self) -> const char* {
        const ecs::JointComponent* jc = jointOf(self);
        return jc ? ecs::jointKindName(jc->kind) : nullptr;
    };
    svc.act.joints.target = [this, jointOf](ecs::Entity self) -> ecs::Entity {
        const ecs::JointComponent* jc = jointOf(self);
        if (!jc || jc->target == ecs::kInvalidGuid) return ecs::Entity{};
        return activeScene().world().find(jc->target);
    };

    m_scriptRuntime->setServices(std::move(svc));

    m_scriptRuntime->setPrintSink(
        [](const vks::ScriptInstance& inst, const std::string& line) {
            VK_LOG_INFO("GAME-SCRIPT print %s: %s", inst.path.c_str(),
                        line.c_str());
        });
    // ⚠ 措辞与 S0 之前逐字相同（`GAME-SCRIPT not runnable` /
    //   `GAME-SCRIPT runtime error`）：verify 脚本 grep 的就是这两句。
    m_scriptRuntime->setErrorSink(
        [](const vks::ScriptInstance& inst, const std::string& msg) {
            const bool compileErr =
                inst.errorKind == vks::ScriptInstance::ErrorKind::Compile;
            VK_LOG_WARN("GAME-SCRIPT %s (%s): %s",
                        compileErr ? "not runnable" : "runtime error",
                        inst.path.c_str(), msg.c_str());
        });
}

// ---------------------------------------------------------------- 接触事件

void RuntimeApp::pumpContactEvents() {
    if (!m_binding.world) return;
    m_events.clear();
    m_contacts.collect(*m_binding.world, m_events);
    if (m_events.empty()) return;

    // ★ S3：翻译成 `vks::ScriptContact` 投递给脚本事例。
    //   `ContactEvent` 用的是 EntityId（没有 generation），这里必须用
    //   `World::handle()` 补成完整句柄 —— 否则"旧 id 复用"的碰撞会派发到
    //   刚被删掉的那个实体的继任者身上。
    {
        ecs::World& w = activeScene().world();
        for (const physics::ContactEvent& e : m_events) {
            vks::ScriptContact sc;
            switch (e.phase) {
                case physics::ContactPhase::Enter:
                    sc.kind = vks::ScriptContact::Kind::Enter;
                    break;
                case physics::ContactPhase::Stay:
                    sc.kind = vks::ScriptContact::Kind::Stay;
                    break;
                case physics::ContactPhase::Exit:
                    sc.kind = vks::ScriptContact::Kind::Exit;
                    break;
            }
            sc.a = w.handle(e.a);
            sc.b = w.handle(e.b);
            sc.trigger = e.trigger;
            m_scriptRuntime->postContact(sc);
        }
    }

    // 事件本身在 G08 里已经派发到 ContactTracker；这里只负责"让脚本 /
    // 日志看得见"（GAME-CONTACT 行默认只在 trace 下打，正常玩不刷屏）。
    if (m_contactTrace) {
        for (const physics::ContactEvent& e : m_events) {
            VK_LOG_INFO("GAME-CONTACT %s a=%u b=%u trigger=%d stay=%d",
                        physics::contactPhaseName(e.phase), e.a, e.b,
                        e.trigger ? 1 : 0, e.stay);
        }
    }
}

// ---------------------------------------------------------------- 转储

// 格式与编辑器的 `MYVK_PHYS_DUMP` **逐字一致** ——
// tools/verify_runtime_determinism.py 就是拿两边的同一帧逐行比数字的。
void RuntimeApp::dumpPhysicsBodies(const char* tag) {
    if (!m_binding.world) return;
    ecs::World& w = activeScene().world();
    for (const auto& kv : m_binding.entityToBody) {
        const physics::RigidBody* b = m_binding.world->body(kv.second);
        if (!b || !b->isDynamic()) continue;
        const ecs::Entity e = w.handle(kv.first);
        const std::string* nm = e.valid() ? w.name(e) : nullptr;
        const auto* tc = e.valid() ? w.get<ecs::TransformComponent>(e) : nullptr;
        const glm::vec3 p = tc ? tc->position : b->position;
        VK_LOG_INFO("%s f=%llu name='%s' pos=(%.5f,%.5f,%.5f) "
                    "vel=(%.5f,%.5f,%.5f) awake=%d",
                    tag, static_cast<unsigned long long>(m_simFrames),
                    nm ? nm->c_str() : "?", static_cast<double>(p.x),
                    static_cast<double>(p.y), static_cast<double>(p.z),
                    static_cast<double>(b->linearVelocity.x),
                    static_cast<double>(b->linearVelocity.y),
                    static_cast<double>(b->linearVelocity.z), b->awake ? 1 : 0);
    }
}

// ---------------------------------------------------------------- 收尾

void RuntimeApp::onShutdown() {
    // ---- S6 / G5：退出时把**运行态**场景写一份 ----
    //
    // `--dump-scene` 是"读进来立刻再写一份"，它证明的是**读盘的规范化形状**；
    // 而"世界黑板进不进存档"必须**跑完之后**再写 —— 脚本只有在 Play 里
    // 才写 `setWorldAttr`，所以 dump 出来的永远是空的。
    //
    // 与 `--dump-scene` 同一个写端（`scene::saveScene`），所以往返形状一致：
    // 拿它当输入再起一次 Game.exe 就是一次真正的 roundtrip。
    if (const char* out = std::getenv("MYVK_RUNTIME_SAVE_SCENE")) {
        if (out[0] != '\0') {
            const scene::SceneIoResult w =
                scene::saveScene(activeScene(), std::string(out));
            if (w.ok) {
                VK_LOG_INFO("GAME-SCENE-SAVED path=%s entities=%d vars=%zu",
                            out, w.entities, activeScene().scriptVars().size());
            } else {
                VK_LOG_WARN("GAME-SCENE-SAVE-FAIL path=%s %s", out,
                            w.error.c_str());
            }
        }
    }

    // ⚠ 音频要**先**关：它有一条自己的线程，必须在窗口 / 日志之前停掉，
    //   否则设备回调可能在我们销毁 Mixer 之后还被调一次。
    {
        const audio::MixStats as = m_audio.stats();
        VK_LOG_INFO("AUDIO-SUMMARY backend=%s device=%s running=%d voices=%d "
                    "sr=%u ch=%u mem=%.0f",
                    m_audio.backendName(), m_audio.deviceName().c_str(),
                    m_audio.running() ? 1 : 0, as.voices,
                    m_audio.deviceFormat().sampleRate,
                    m_audio.deviceFormat().channels,
                    static_cast<double>(m_audio.cacheResidentBytes() +
                                        m_audio.streamResidentBytes()));
    }
    m_audio.shutdown();
    m_audioSrcs.clear();

    m_loop.logNow();
    VK_LOG_INFO("GAME-LOOP-SUMMARY ticks=%llu fixed=%llu var=%llu render=%llu "
                "sim=%.3fs dropped=%llu",
                static_cast<unsigned long long>(m_loop.stats().ticks),
                static_cast<unsigned long long>(m_loop.stats().fixedSteps),
                static_cast<unsigned long long>(m_loop.stats().variableSteps),
                static_cast<unsigned long long>(m_loop.stats().renders),
                static_cast<double>(m_loop.stats().simTime),
                static_cast<unsigned long long>(m_loop.stats().droppedSteps));
    VK_LOG_INFO("GAME-STATE-SEQ %s", m_stateStack.topSequenceString().c_str());
    if (m_player) {
        VK_LOG_INFO("GAME-PLAYER-SUMMARY travelled=%.3fm arm=%.3fm contacts=%llu",
                    static_cast<double>(m_player->travelledDistance()),
                    static_cast<double>(m_player->springArmDistance()),
                    static_cast<unsigned long long>(m_contacts.totalEvents()));
    } else {
        VK_LOG_INFO("GAME-PLAYER-SUMMARY none");
    }
    if (m_binding.world) {
        const physics::PhysicsStats& st = m_binding.world->stats();
        VK_LOG_INFO("GAME-PHYS-SUMMARY steps=%llu sim=%.3fs bodies=%d dyn=%d "
                    "manifolds=%d",
                    static_cast<unsigned long long>(st.steps),
                    static_cast<double>(st.simTime), st.bodies, st.dynamic,
                    st.manifolds);
    }
}

} // namespace runtime
