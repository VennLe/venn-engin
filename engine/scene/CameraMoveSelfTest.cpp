// ============================================================
// scene/CameraMoveSelfTest —— 运镜算子 / 镜头光学 / 新通道隔离
//
// 无窗口、无设备、无场景：只跑 `CameraMove.*` 与 `Camera::view/proj`。
// 入口：`Game.exe --selftest cameramove`（或 `Editor.exe` 同参数）。
//
// 为什么这块必须有自检，而不能靠"打开编辑器看一眼"：
//   ① 运镜是**增量算子**，"少加一次"和"多加一次"在单帧画面里长得一样，
//      只有连推两次才暴露（A1）；
//   ② 拉焦 / 换算这类是**纯数学**，肉眼只能看出"有点糊"，看不出
//      "f/1.4 是 f/8 的 5.7 倍"这条比例对不对（B4）；
//   ③ roll / shift 是**新加进老结构**的三个通道，最大的风险不是"不生效"，
//      而是"默认 0 时悄悄改了老画面"（C1/C3 就是钉这个）。
//
// 判据纪律：每条都带一个**反向对照**（"如果是另一种实现，这条会红"）。
//   只写"结果等于 X"的断言在老实现上也常常能过，等于没写。
// ============================================================

#include "scene/CameraMove.h"
#include "scene/Camera.h"
#include "scene/CameraPath.h"
#include "scene/CameraSystem.h"
#include "scene/Scene.h"

#include "ecs/Components.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace scene {

namespace {

int g_pass = 0;
int g_fail = 0;

constexpr float kPi = 3.14159265358979f;
constexpr float kDeg = kPi / 180.0f;

void chk(const char* what, bool ok, const std::string& detail = "") {
    if (ok) {
        ++g_pass;
        std::printf("CAMMOVE-TEST pass %s%s%s\n", what,
                    detail.empty() ? "" : " — ", detail.c_str());
    } else {
        ++g_fail;
        std::printf("CAMMOVE-TEST FAIL %s%s%s\n", what,
                    detail.empty() ? "" : " — ", detail.c_str());
    }
}

std::string v3(const glm::vec3& v) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "(%.4f,%.4f,%.4f)", v.x, v.y, v.z);
    return buf;
}

bool near(float a, float b, float eps = 1.0e-4f) {
    return std::fabs(a - b) <= eps;
}

bool nearVec(const glm::vec3& a, const glm::vec3& b, float eps = 1.0e-4f) {
    return glm::length(a - b) <= eps;
}

// 位相等：不用 eps。用于"默认值下必须逐位不变"那几条 —— 那里用 eps 就
// 等于放过了一类真 bug（浮点上的"几乎不变"往往意味着多算了一次）。
bool bitEqual(const glm::vec3& a, const glm::vec3& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

bool matBitEqual(const glm::mat4& a, const glm::mat4& b) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (a[c][r] != b[c][r]) return false;
    return true;
}

CameraMoveSpec move(CameraMoveKind k, float amount) {
    CameraMoveSpec s;
    s.kind = k;
    s.amount = amount;
    return s;
}

// 一台"标准站位"的相机：位置 (5,0,2)、yaw=0（看向 −X）、pitch=0。
CameraPose basePose() {
    CameraPose p;
    p.position = glm::vec3(5.0f, 0.0f, 2.0f);
    p.yaw = 0.0f;
    p.pitch = 0.0f;
    p.fov = 45.0f;
    return p;
}

// 造一台带 CameraComponent 的相机实体。位置固定，用例靠坐标认人 ——
// 靠"名字"认的话，一旦 `m_activeName` 缓存出问题会红在很奇怪的地方。
ecs::Entity addCamera(Scene& s, const char* name, const glm::vec3& pos,
                      int priority) {
    const ecs::Entity e = s.createObject(name);
    if (auto* tr = s.world().get<ecs::TransformComponent>(e)) tr->position = pos;
    ecs::CameraComponent cc;
    cc.enabled = true;
    cc.priority = priority;
    s.world().add<ecs::CameraComponent>(e, cc);
    return e;
}

} // namespace

int runCameraMoveSelfTest() {
    g_pass = 0;
    g_fail = 0;
    std::printf("CAMMOVE-TEST begin (no window)\n");

    // ================================================================
    // A. 九个动词
    // ================================================================
    std::printf("-- A 运镜动词\n");

    // ---- A1 Pan 是**增量** ----
    //
    // 反向对照：如果实现是 `yaw = amount`（设值型），推两次会停在 15°，
    // 第二次推"没反应"；如果符号写反，30° 会变成 −30°。
    {
        CameraPose p = basePose();
        applyCameraMove(p, move(CameraMoveKind::Pan, 15.0f));
        const float once = p.yaw / kDeg;
        applyCameraMove(p, move(CameraMoveKind::Pan, 15.0f));
        const float twice = p.yaw / kDeg;

        CameraPose q = basePose();
        applyCameraMove(q, move(CameraMoveKind::Pan, 30.0f));

        chk("A1a Pan 15° 后 yaw=15°", near(once, 15.0f, 1.0e-3f),
            "yaw=" + std::to_string(once));
        chk("A1b Pan 推两次 = 30°（增量而非设值）", near(twice, 30.0f, 1.0e-3f),
            "yaw=" + std::to_string(twice));
        // 对照：分两次走完和一次走完必须落在同一个地方（线性、无历史依赖）
        chk("A1c 两次 15° == 一次 30°（无路径依赖）",
            near(p.yaw, q.yaw, 1.0e-5f),
            "step=" + std::to_string(p.yaw) + " one-shot=" + std::to_string(q.yaw));
        // 反向对照：yaw 只该动 yaw，position 一个字节都不许变
        chk("A1d 旋转不动位置（逐位相同）", bitEqual(p.position, basePose().position));
    }

    // ---- A2 平移三个方向：Dolly / Truck / Pedestal 必须**互不相同** ----
    //
    // 反向对照：如果三个动词全用了"世界前方向"，Dolly 和 Truck 会得到同一个
    // 结果；如果 Truck 用了世界 +X 而不是相机右方，yaw=90° 时才看得出差别，
    // 所以这里特意把 yaw 摆到 90° 再验一次。
    {
        CameraPose a = basePose();
        applyCameraMove(a, move(CameraMoveKind::Dolly, 2.0f));   // 沿视线前推
        CameraPose b = basePose();
        applyCameraMove(b, move(CameraMoveKind::Truck, 2.0f));   // 向右横移
        CameraPose c = basePose();
        applyCameraMove(c, move(CameraMoveKind::Pedestal, 2.0f)); // 升高

        // yaw=0 时 forward = (−1,0,0)，right = (0,1,0)
        chk("A2a Dolly +2 → 沿视线前推 (−1,0,0)·2",
            nearVec(a.position, glm::vec3(3.0f, 0.0f, 2.0f)), v3(a.position));
        chk("A2b Truck +2 → 向右横移 (0,1,0)·2",
            nearVec(b.position, glm::vec3(5.0f, 2.0f, 2.0f)), v3(b.position));
        chk("A2c Pedestal +2 → 世界竖直 +2",
            nearVec(c.position, glm::vec3(5.0f, 0.0f, 4.0f)), v3(c.position));
        chk("A2d 三个动词结果两两不同（没接成同一个分支）",
            !nearVec(a.position, b.position) && !nearVec(b.position, c.position) &&
                !nearVec(a.position, c.position));

        // yaw=90° 时 forward = (0,1,0) 而 right = (−1,0,0) —— 换一个朝向再验
        // 一次，抓"写死世界坐标轴"这种实现（那种实现在 yaw=0 上全部正确）。
        CameraPose d = basePose();
        d.yaw = 90.0f * kDeg;
        applyCameraMove(d, move(CameraMoveKind::Dolly, 2.0f));
        CameraPose e = basePose();
        e.yaw = 90.0f * kDeg;
        applyCameraMove(e, move(CameraMoveKind::Truck, 2.0f));
        chk("A2e yaw=90° 后 Dolly 走 +Y（跟着相机转）",
            nearVec(d.position, glm::vec3(5.0f, 2.0f, 2.0f)), v3(d.position));
        // yaw=90° 时 forward=(0,1,0)、right=(1,0,0)：看 +Y 时右手边就是 +X。
        // ⚠ 这里曾经把期望写成 −X（凭"相机朝北、右手该朝西"的直觉），
        //   实测才纠正 —— 世界是 Z-up 右手系，right = forward × up。
        chk("A2f yaw=90° 后 Truck 走 +X（跟着相机转）",
            nearVec(e.position, glm::vec3(7.0f, 0.0f, 2.0f)), v3(e.position));
    }

    // ---- A3 Tilt 夹紧 / Roll 归一化 ----
    {
        CameraPose p = basePose();
        applyCameraMove(p, move(CameraMoveKind::Tilt, 200.0f));
        chk("A3a Tilt 200° 被夹到俯仰上限（不会越过 90° 翻面）",
            near(p.pitch, Camera::kPitchLimit, 1.0e-5f),
            "pitch=" + std::to_string(p.pitch / kDeg) + "°");

        CameraPose q = basePose();
        applyCameraMove(q, move(CameraMoveKind::Roll, 350.0f));
        chk("A3b Roll 350° 归一化到 −10°",
            near(q.roll / kDeg, -10.0f, 1.0e-3f),
            "roll=" + std::to_string(q.roll / kDeg) + "°");
        chk("A3c Roll 结果落在 (−π, π]", q.roll > -kPi - 1.0e-5f && q.roll <= kPi + 1.0e-5f);
    }

    // ---- A4 amount == 0 必须**逐位**不变 ----
    //
    // 反向对照：如果实现里先做了一次 normalizeAngle 或 mat 往返，
    // 这里就会红 —— 而那种"零输入也有微小扰动"会让关键帧的静止段
    // 在每一帧产生极小的位移，坐标轴上限越拖越远。
    {
        bool all = true;
        const CameraPose base = basePose();
        for (int i = 0; i < static_cast<int>(CameraMoveKind::Count); ++i) {
            CameraPose p = base;
            applyCameraMove(p, move(static_cast<CameraMoveKind>(i), 0.0f));
            if (!bitEqual(p.position, base.position) || p.yaw != base.yaw ||
                p.pitch != base.pitch || p.roll != base.roll)
                all = false;
        }
        chk("A4 amount=0 时八个动词全部逐位不变", all);
    }

    // ---- A5 Orbit：绕支点、闭合、始终看向支点 ----
    {
        CameraPose p = basePose();      // (5,0,2)，看向 −X
        CameraMoveSpec s = move(CameraMoveKind::Orbit, 90.0f);
        s.pivot = glm::vec3(0.0f, 0.0f, 0.0f);
        s.pivotValid = true;
        applyCameraMove(p, s);

        chk("A5a Orbit 90° 后位置绕支点转到 (0,5,2)",
            nearVec(p.position, glm::vec3(0.0f, 5.0f, 2.0f), 1.0e-3f), v3(p.position));

        // 朝向必须重算：转 90° 之后镜头要对着支点，而不是保持原来的 −X
        const glm::vec3 toPivot = glm::normalize(-p.position);
        chk("A5b Orbit 后朝向指向支点（自动 aimAt）",
            nearVec(poseForward(p), toPivot, 1.0e-3f),
            "fwd=" + v3(poseForward(p)));

        // 闭合：转满 360° 回到出发点
        CameraPose q = basePose();
        for (int i = 0; i < 4; ++i) applyCameraMove(q, s);
        chk("A5c Orbit 4×90° 回到起点（闭合）",
            nearVec(q.position, basePose().position, 1.0e-3f), v3(q.position));
    }

    // ---- A6 Crane：正角度**升高**，且不自动看向支点（与 Orbit 的区别） ----
    //
    // 反向对照：如果 Crane 也用 aimAt（抄了 Orbit 的分支），A6b 会红；
    // 如果旋转轴的叉积方向写反（cross(up,h) 而不是 cross(h,up)），
    // 正角度会变成下降，A6a 会红。这两条错都"画面看起来在动"。
    {
        CameraPose p = basePose();      // (5,0,2)
        CameraMoveSpec s = move(CameraMoveKind::Crane, 10.0f);
        s.pivot = glm::vec3(0.0f, 0.0f, 0.0f);
        s.pivotValid = true;
        const float yaw0 = p.yaw;
        const float pitch0 = p.pitch;
        applyCameraMove(p, s);

        chk("A6a Crane +10° 使相机升高",
            p.position.z > 2.0f + 1.0e-3f, "z=" + std::to_string(p.position.z));
        chk("A6b Crane 不改朝向（摇臂不自动跟支点）",
            p.yaw == yaw0 && p.pitch == pitch0,
            "dyaw=" + std::to_string((p.yaw - yaw0) / kDeg));
        // 弧线：到支点的**距离**不变（绕点转，不是平移）
        chk("A6c Crane 保持到支点的距离（真绕支点）",
            near(glm::length(p.position), glm::length(basePose().position), 1.0e-3f),
            "r=" + std::to_string(glm::length(p.position)));
    }

    // ---- A7 兜底支点：不给支点时也要动（但不能荒唐） ----
    {
        CameraPose p = basePose();
        CameraMoveSpec s = move(CameraMoveKind::Orbit, 90.0f);   // pivotValid=false
        applyCameraMove(p, s);
        chk("A7 不给支点时 Orbit 仍产生位移（面板点一下不会毫无反应）",
            !nearVec(p.position, basePose().position, 1.0e-2f), v3(p.position));
    }

    // ---- A8 Handheld：确定性 / 常驻 / 默认关 ----
    {
        CameraPose p = basePose();
        applyHandheld(p, 0.7f, 0.0f, 1.6f, 0.0f);
        chk("A8a 振幅 0 时手持逐位不变", bitEqual(p.position, basePose().position) &&
                                              p.yaw == basePose().yaw);

        CameraPose a = basePose();
        CameraPose b = basePose();
        applyHandheld(a, 0.7f, 0.5f, 1.6f, 0.0f);
        applyHandheld(b, 0.7f, 0.5f, 1.6f, 0.0f);
        chk("A8b 同一时刻采样两次结果相同（确定性）", a.yaw == b.yaw && a.pitch == b.pitch);

        CameraPose c = basePose();
        applyHandheld(c, 1.4f, 0.5f, 1.6f, 0.0f);
        chk("A8c 不同时刻结果不同（真的在动）", c.yaw != a.yaw);
        chk("A8d 幅度受控（0.5° 的抖动，yaw 偏离 < 1°）",
            std::fabs(a.yaw - basePose().yaw) < 1.0f * kDeg,
            "dyaw=" + std::to_string((a.yaw - basePose().yaw) / kDeg) + "°");

        // 种子：相位不同 → 两条手持轨迹不该重合（两台相机并排不能同步抖）
        CameraPose d = basePose();
        applyHandheld(d, 0.7f, 0.5f, 1.6f, 2.1f);
        chk("A8e 不同相位得到不同抖动（种子生效）", d.yaw != a.yaw);
    }

    // ================================================================
    // B. 镜头光学
    // ================================================================
    std::printf("-- B 镜头光学\n");
    {
        // 50mm / 24mm 画幅：fovY = 2·atan(12/50) = 26.99°
        const float f50 = focalToFovY(50.0f, 24.0f);
        chk("B1 50mm/24mm → 26.99°",
            near(f50, 26.9915f, 0.02f), "fov=" + std::to_string(f50));

        // 往返
        const float back = fovYToFocal(f50, 24.0f);
        chk("B2 焦距→视场→焦距 往返自洽", near(back, 50.0f, 0.01f),
            "focal=" + std::to_string(back));

        // 单调：焦距越长视场越窄
        const float f24 = focalToFovY(24.0f, 24.0f);
        const float f85 = focalToFovY(85.0f, 24.0f);
        chk("B3 广角 > 标准 > 长焦（视场单调递减）", f24 > f50 && f50 > f85,
            std::to_string(f24) + " > " + std::to_string(f50) + " > " + std::to_string(f85));

        // 景深：f/1.4 的离焦量是 f/8 的 8/1.4 = 5.714 倍
        const float a14 = fStopToDofAperture(1.4f, 50.0f, 24.0f);
        const float a8 = fStopToDofAperture(8.0f, 50.0f, 24.0f);
        chk("B4 f/1.4 的离焦量是 f/8 的 5.71 倍", near(a14 / a8, 8.0f / 1.4f, 0.02f),
            "ratio=" + std::to_string(a14 / a8));
        chk("B5 50mm f/1.4 落在出厂默认 1.2（与手填 aperture 同一张画面）",
            near(a14, 1.2f, 0.01f), "a=" + std::to_string(a14));

        // 长焦更虚：同一 f 值下 85mm 比 50mm 虚 1.7 倍
        const float a85 = fStopToDofAperture(1.4f, 85.0f, 24.0f);
        chk("B6 同光圈下 85mm 比 50mm 虚 1.7 倍", near(a85 / a14, 85.0f / 50.0f, 0.02f),
            "ratio=" + std::to_string(a85 / a14));
    }

    // ================================================================
    // C. 新通道隔离（roll / shift 默认 0 不许动老画面）
    // ================================================================
    std::printf("-- C 新通道隔离\n");
    {
        Camera cam;
        cam.setFlyMode(true);            // 与运行时同一条路径（位置独立存）
        cam.setUseGlobalInput(false);
        cam.setPosition(glm::vec3(3.0f, -2.0f, 1.5f));
        cam.setYaw(0.63f);
        cam.setPitch(0.21f);
        cam.setFov(38.0f);

        // ---- C1 roll=0 → view 矩阵与 lookAt 逐位相同 ----
        const glm::mat4 expectView =
            glm::lookAt(cam.position(), cam.target(), glm::vec3(0.0f, 0.0f, 1.0f));
        chk("C1 roll=0 时 viewMatrix 逐位等于 lookAt（老画面不动）",
            matBitEqual(cam.viewMatrix(), expectView));

        // ---- C2 roll≠0 只改 up，不改视线方向 ----
        //
        // ⚠⚠ 判据落在 view 矩阵的**第三行**，不是第三列。`glm::lookAt` 生成的是
        //   Rᵀ（行 = right / up / −forward），列主序下第 2 **行**的三个元素才是
        //   `(−fx, −fy, −fz)`；第 2 列是 `(s.z, u.z, −f.z)` —— 它带着 up 的分量，
        //   roll 自然会改它。第一版取错成列，于是"实现对了、期望错了"（红）。
        auto behindDir = [](const glm::mat4& m) {
            return glm::vec3(m[0][2], m[1][2], m[2][2]);
        };
        auto upDir = [](const glm::mat4& m) {
            return glm::vec3(m[0][1], m[1][1], m[2][1]);
        };

        const glm::mat4 v0 = cam.viewMatrix();
        cam.setRoll(60.0f * kDeg);
        const glm::mat4 v1 = cam.viewMatrix();
        chk("C2a roll 不改变视线方向（第三行逐位相同）",
            behindDir(v0) == behindDir(v1), v3(behindDir(v1)));
        chk("C2b roll 改变了 up（否则 roll 根本没生效）", upDir(v0) != upDir(v1));
        // 正交性：旋转后的 up 仍与视线垂直
        chk("C2c roll 后 up 仍与视线垂直",
            near(glm::dot(upDir(v1), behindDir(v1)), 0.0f, 1.0e-5f),
            std::to_string(glm::dot(upDir(v1), behindDir(v1))));
        cam.setRoll(0.0f);

        // ---- C3 shift=0 → proj 与原始透视逐位相同 ----
        const float aspect = 16.0f / 9.0f;
        glm::mat4 expectProj = glm::perspective(
            glm::radians(cam.fov()), aspect, cam.nearPlane(), cam.farPlane());
        expectProj[1][1] *= -1.0f;
        chk("C3 shift=0 时 projMatrix 逐位等于原透视（老画面不动）",
            matBitEqual(cam.projMatrix(aspect), expectProj));

        // ---- C4 shift 的符号：正 shiftX → 内容左移（NDC.x 变负） ----
        //
        // 取视线正前方 10 米的点（本应在画面正中，ndc = (0,0)）。
        const glm::vec3 fwd = poseForward([] {
            CameraPose p;
            p.yaw = 0.63f;
            p.pitch = 0.21f;
            return p;
        }());
        const glm::vec3 probe = cam.position() + fwd * 10.0f;

        auto ndcOf = [&](const Camera& c) {
            const glm::vec4 clip =
                c.projMatrix(aspect) * c.viewMatrix() * glm::vec4(probe, 1.0f);
            return glm::vec2(clip.x / clip.w, clip.y / clip.w);
        };

        const glm::vec2 n0 = ndcOf(cam);
        chk("C4a 无 shift 时正前方 10 米落在画面正中",
            near(n0.x, 0.0f, 1.0e-4f) && near(n0.y, 0.0f, 1.0e-4f),
            std::to_string(n0.x) + "," + std::to_string(n0.y));

        cam.setShift(0.1f, 0.0f);
        const glm::vec2 n1 = ndcOf(cam);
        chk("C4b shiftX=0.1 → 该点 NDC.x 减少 0.2（内容左移）",
            near(n1.x - n0.x, -0.2f, 1.0e-3f),
            "d=" + std::to_string(n1.x - n0.x));
        chk("C4c shiftX 不影响 NDC.y",
            near(n1.y, n0.y, 1.0e-4f), "dy=" + std::to_string(n1.y - n0.y));

        cam.setShift(0.0f, 0.1f);
        const glm::vec2 n2 = ndcOf(cam);
        chk("C4d shiftY=0.1 → 该点 NDC.y 减少 0.2（内容下移）",
            near(n2.y, -0.2f, 1.0e-3f), "y=" + std::to_string(n2.y));

        // ---- C5 shift 不碰 view 矩阵 ----
        cam.setShift(0.3f, -0.2f);
        chk("C5 shift 不影响 viewMatrix", matBitEqual(cam.viewMatrix(), v0));
        cam.setShift(0.0f, 0.0f);
        chk("C6 shift 归零后 proj 回到基线（逐位相同）",
            matBitEqual(cam.projMatrix(aspect), expectProj));
    }

    // ================================================================
    // D. 姿态 ⇄ 实体欧拉（"把镜头定稿成一台相机实体"）
    // ================================================================
    //
    // ⚠ 这一组存在的理由：`TransformComponent::rotation` 是 **X→Y→Z 欧拉**，
    //   而 `CameraPose` 的朝向是 (yaw 绕世界 Z, pitch 绕水平轴)。两者**不是**
    //   同一种参数化 —— 把 pitch 填进 rotation.x 会得到一台绕自己横轴翻滚的
    //   相机（画面看着"转了但不对"）。D3 就是把这种朴素做法钉成"已知会错"，
    //   以后有人图省事直接赋值，这里有据可查。
    {
        const float yaws[] = {0.0f, 0.63f, 90.0f * kDeg, -120.0f * kDeg,
                              179.0f * kDeg};
        const float pitches[] = {0.0f, 0.21f, 45.0f * kDeg, -60.0f * kDeg,
                                 85.0f * kDeg};

        int badFwd = 0;
        int badUp = 0;
        std::string worst;
        float worstErr = 0.0f;

        for (float y : yaws) {
            for (float pt : pitches) {
                CameraPose p;
                p.yaw = y;
                p.pitch = pt;
                p.roll = 25.0f * kDeg;

                const glm::vec3 e = cameraEulerFor(p);
                // 按 TransformComponent 的顺序重建：R = Rx · Ry · Rz
                glm::mat4 m(1.0f);
                m = glm::rotate(m, e.x, glm::vec3(1, 0, 0));
                m = glm::rotate(m, e.y, glm::vec3(0, 1, 0));
                m = glm::rotate(m, e.z, glm::vec3(0, 0, 1));

                const glm::vec3 localX = glm::normalize(glm::vec3(m[0]));
                const glm::vec3 localZ = glm::normalize(glm::vec3(m[2]));
                const float ef = glm::length(localX - poseForward(p));
                const float eu = glm::length(localZ - glm::normalize(poseUp(p)));
                if (ef > 1.0e-4f) ++badFwd;
                if (eu > 1.0e-4f) ++badUp;
                if (ef > worstErr) {
                    worstErr = ef;
                    worst = "yaw=" + std::to_string(y / kDeg) +
                            " pitch=" + std::to_string(pt / kDeg);
                }
            }
        }
        chk("D1 25 组 (yaw,pitch,roll) 写回实体后局部 +X 仍是视线", badFwd == 0,
            worst.empty() ? "" : ("worst " + worst));
        chk("D2 同理局部 +Z 仍是 tilt 后的 up（roll 没丢）", badUp == 0);

        // 反向对照：朴素做法（rotation = (0, pitch, yaw)）在非零 pitch 上必然偏
        {
            CameraPose p;
            p.yaw = 0.63f;
            p.pitch = 45.0f * kDeg;
            glm::mat4 m(1.0f);
            m = glm::rotate(m, p.pitch, glm::vec3(1, 0, 0));
            m = glm::rotate(m, p.yaw, glm::vec3(0, 0, 1));
            const glm::vec3 naive = glm::normalize(glm::vec3(m[0]));
            const float dev = glm::length(naive - poseForward(p));
            chk("D3 反向对照：朴素赋值 (0,pitch,yaw) 确实偏了（所以这个函数不是摆设）",
                dev > 1.0e-2f, "dev=" + std::to_string(dev));
        }
    }

    // ================================================================
    // E. Follow Path（弧长参数化）
    // ================================================================
    //
    // ⚠ 这一组真正要钉的只有一条：**均匀参数 ≠ 匀速运动**。
    //   贝塞尔的参数 t 走 0.5，差不多是"半个参数区间"，而不是"一半路程"。
    //   直观看不出来（曲线本身还是那条曲线），表现是"拐弯处突然窜一下"，
    //   而作者的第一反应永远是"关键帧太少了"。
    {
        // E0 两点直线：Catmull-Rom 手柄退化成"三等分"控制点，长度精确可算
        {
            CameraPath p;
            p.points.push_back({{0.0f, 0.0f, 0.0f}, {}, {}});
            p.points.push_back({{10.0f, 0.0f, 0.0f}, {}, {}});
            p.smoothHandles();
            chk("E0a 两点路径长度 = 10", near(p.length(), 10.0f, 1.0e-3f),
                std::to_string(p.length()));
            chk("E0b sample(0) = 起点", nearVec(p.sample(0.0f), {0.0f, 0.0f, 0.0f}));
            chk("E0c sample(1) = 终点",
                nearVec(p.sample(1.0f), {10.0f, 0.0f, 0.0f}));
            chk("E0d sample(0.5) = 中点（直线路径上弧长比例就是位置比例）",
                nearVec(p.sample(0.5f), {5.0f, 0.0f, 0.0f}, 1.0e-3f),
                v3(p.sample(0.5f)));
            glm::vec3 tan(0.0f);
            p.sample(0.3f, &tan);
            chk("E0e 直线路径的切向是 +X",
                near(tan.y, 0.0f, 1.0e-5f) && tan.x > 0.0f, v3(tan));
        }

        // E1 **弧长 vs 参数**：一条**直线**，但两个手柄故意一短一长
        //    （c0 只伸出 0.1 m、c1 缩回 3 m）→ 参数速度从 0.3 m/单位 一路
        //    涨到 9 m/单位（30 倍）。弧长参数化应该完全抹平它。
        //
        // ⚠⚠ 为什么必须是直线：第一版用的是折线点 + 自动手柄，结果**两条都红**
        //   —— Catmull-Rom 在折点会外插出"过冲再回头"，而回头处即使弧长
        //   参数化得完美，**位移**也只有弧长的一小截（走 0.25 m 净移动 0.08 m）。
        //   也就是说那个判据量的是"曲线的弯"，不是"参数化的均匀性"。
        //   直线段上弦长 = 弧长，才只剩"参数化"这一个变量。
        {
            CameraPath p;
            CameraPathPoint a;
            a.position = {0.0f, 0.0f, 0.0f};
            a.outHandle = {0.1f, 0.0f, 0.0f};
            CameraPathPoint b;
            b.position = {10.0f, 0.0f, 0.0f};
            b.inHandle = {-3.0f, 0.0f, 0.0f};
            p.points.push_back(a);
            p.points.push_back(b);
            // ⚠ 不调 smoothHandles：那会把手工手柄覆盖成均匀的。

            const int kSteps = 40;
            auto spread = [&](bool arclen) {
                glm::vec3 prev;
                float mn = 1.0e30f, mx = 0.0f, sum = 0.0f;
                for (int i = 0; i <= kSteps; ++i) {
                    const float u = static_cast<float>(i) / kSteps;
                    glm::vec3 q;
                    if (arclen) {
                        q = p.sample(u);
                    } else {
                        // 对照实现：u 直接当**段内参数** t（不做弧长重映射）
                        const CameraPathPoint& pa = p.points[0];
                        const CameraPathPoint& pb = p.points[1];
                        const glm::vec3 c0 = pa.position + pa.outHandle;
                        const glm::vec3 c1 = pb.position + pb.inHandle;
                        const float t = u;
                        const float mt = 1.0f - t;
                        q = pa.position * (mt * mt * mt) +
                            c0 * (3.0f * mt * mt * t) + c1 * (3.0f * mt * t * t) +
                            pb.position * (t * t * t);
                    }
                    if (i > 0) {
                        const float d = glm::length(q - prev);
                        mn = std::min(mn, d);
                        mx = std::max(mx, d);
                        sum += d;
                    }
                    prev = q;
                }
                const float mean = sum / kSteps;
                return mean > 1.0e-6f ? (mx - mn) / mean : 0.0f;
            };

            const float sArc = spread(true);
            const float sNaive = spread(false);
            chk("E1a 弧长参数下每步位移均匀（离散度 < 3%）", sArc < 0.03f,
                "spread=" + std::to_string(sArc));
            chk("E1b 反向对照：按参数采样时位移极不均匀（> 100%）",
                sNaive > 1.0f, "spread=" + std::to_string(sNaive));
            chk("E1c 弧长参数确实优于参数采样（差一个量级）",
                sArc < sNaive * 0.1f,
                "arc=" + std::to_string(sArc) + " naive=" + std::to_string(sNaive));
            chk("E1d 直线路径长度 = 10（手柄拉长不改变长度）",
                near(p.length(), 10.0f, 1.0e-3f), std::to_string(p.length()));
            chk("E1e 手调手柄没被 smoothHandles 覆盖",
                near(p.points[0].outHandle.x, 0.1f, 1.0e-6f),
                std::to_string(p.points[0].outHandle.x));
        }

        // E2 闭合路径：u=1 与 u=0 同点；越界绕回
        {
            CameraPath p;
            p.closed = true;
            p.points.push_back({{1.0f, 0.0f, 0.0f}, {}, {}});
            p.points.push_back({{1.0f, 1.0f, 0.0f}, {}, {}});
            p.points.push_back({{0.0f, 1.0f, 0.0f}, {}, {}});
            p.points.push_back({{0.0f, 0.0f, 0.0f}, {}, {}});
            p.smoothHandles();

            chk("E2a 闭合路径的段数 = 点数", p.segmentCount() == 4,
                std::to_string(p.segmentCount()));
            chk("E2b 闭合路径 u=1 回到 u=0",
                nearVec(p.sample(1.0f), p.sample(0.0f), 1.0e-4f));
            chk("E2c 越界 u 绕回（1.25 与 0.25 同点）",
                nearVec(p.sample(1.25f), p.sample(0.25f), 1.0e-4f));
            chk("E2d 负 u 也绕回（−0.25 与 0.75 同点）",
                nearVec(p.sample(-0.25f), p.sample(0.75f), 1.0e-4f));
            chk("E2e 闭合路径有长度", p.length() > 3.0f,
                std::to_string(p.length()));
        }

        // E3 退化输入不崩（点数 < 2）
        {
            CameraPath p;
            chk("E3a 空路径不崩、返回原点",
                nearVec(p.sample(0.5f), glm::vec3(0.0f)) && p.length() == 0.0f);
            p.points.push_back({{3.0f, 4.0f, 5.0f}, {}, {}});
            chk("E3b 单点路径返回该点",
                nearVec(p.sample(0.5f), {3.0f, 4.0f, 5.0f}));
        }
    }

    // ================================================================
    // F. 快门（运动模糊强度）＋ roll 的反解闭环
    // ================================================================
    {
        chk("F1a 180° → 强度 1.0（电影标准 180 degree rule）",
            near(shutterToBlurStrength(180.0f), 1.0f, 1.0e-5f));
        chk("F1b 90° → 0.5（半帧的一半）",
            near(shutterToBlurStrength(90.0f), 0.5f, 1.0e-5f));
        chk("F1c 360° 封顶在 2.0", near(shutterToBlurStrength(360.0f), 2.0f),
            std::to_string(shutterToBlurStrength(360.0f)));
        chk("F1d 0 / 负数 = 关（0）",
            shutterToBlurStrength(0.0f) == 0.0f &&
                shutterToBlurStrength(-30.0f) == 0.0f);
        chk("F1e 单调：角度越大越糊",
            shutterToBlurStrength(45.0f) < shutterToBlurStrength(120.0f) &&
                shutterToBlurStrength(120.0f) < shutterToBlurStrength(200.0f));

        // F2 roll 闭环：模拟 `poseFromEntity` 的反解
        //    （roll = atan2(dot(up, right), dot(up, up0))），
        //    与 D1/D2 的**正解**互为逆运算。
        {
            int bad = 0;
            std::string worst;
            float worstErr = 0.0f;
            const float rolls[] = {0.0f, 15.0f * kDeg, -35.0f * kDeg,
                                   80.0f * kDeg};
            const float yaws[] = {0.0f, 1.1f, -2.4f};
            const float pitches[] = {0.0f, 30.0f * kDeg, -55.0f * kDeg};
            for (float y : yaws) {
                for (float pt : pitches) {
                    for (float rr : rolls) {
                        CameraPose p;
                        p.yaw = y;
                        p.pitch = pt;
                        p.roll = rr;
                        const glm::vec3 e = cameraEulerFor(p);
                        glm::mat4 m(1.0f);
                        m = glm::rotate(m, e.x, glm::vec3(1, 0, 0));
                        m = glm::rotate(m, e.y, glm::vec3(0, 1, 0));
                        m = glm::rotate(m, e.z, glm::vec3(0, 0, 1));
                        const glm::vec3 up = glm::normalize(glm::vec3(m[2]));

                        CameraPose q;
                        q.yaw = y;
                        q.pitch = pt;   // roll 还没读回来
                        const glm::vec3 r0 = poseRight(q);
                        const glm::vec3 u0 = poseUp(q);
                        const float rollBack =
                            std::atan2(glm::dot(up, r0), glm::dot(up, u0));
                        // 用 atan2 的差算，避免 ±π 附近直接比较
                        float d = rollBack - rr;
                        while (d > kPi) d -= 2.0f * kPi;
                        while (d < -kPi) d += 2.0f * kPi;
                        if (std::fabs(d) > 1.0e-4f) {
                            ++bad;
                            if (std::fabs(d) > worstErr) {
                                worstErr = std::fabs(d);
                                worst = "yaw=" + std::to_string(y) +
                                        " pitch=" + std::to_string(pt) +
                                        " roll=" + std::to_string(rr);
                            }
                        }
                    }
                }
            }
            // 36 组
            chk("F2 36 组 (yaw,pitch,roll) 写回实体后 roll 能原样读回来", bad == 0,
                worst.empty() ? "" : ("worst " + worst));
        }
    }

    // ============================================================
    // G. 导演覆盖 = 多机位切镜的落点（2026-10-30）
    // ============================================================
    //
    // 时间轴上的 marker 只说"这一段用哪台"，真正让画面换过去的是
    // `CameraSystem::setDirectorCamera`。这一组钉三件事：
    //   ① 覆盖**压过 priority** —— 不然"切了但没效果"：画面上还是
    //      优先级最高的那台，而日志里 `SEQ-SHOT` / `CAM-CUT` 都打了；
    //   ② 目标消失时**回落到 priority**，且把 guid 清零 —— 不回落的话
    //      画面会永久冻在最后一帧，日志里只有一条早已刷过去的 WARN；
    //   ③ `blendCut` 真的插值，且 `dt==0` 不推进。
    //
    // ⚠ `activeCameraSystem()` 是**单例**，跨自检不复位就没有"干净起点"
    //   （上一个用例留下的 directorGuid / 混合状态会以极奇怪的方式串进来）。
    {
        Scene s;
        CameraSystem& cs = activeCameraSystem();
        cs.clearDirectorCamera();
        cs.clearDesiredPose();
        cs.clearShake();

        const ecs::Entity eA = addCamera(s, "A", glm::vec3(0, 0, 5), 0);
        const ecs::Entity eB = addCamera(s, "B", glm::vec3(10, 0, 20), 10);
        const std::uint64_t gA = s.world().guid(eA);
        const std::uint64_t gB = s.world().guid(eB);

        // G1 基线：没有导演时 priority 说了算（10 > 0 → B）。
        //    这条同时是 G2 的**反向对照** —— 没有它，"覆盖生效"无法证明
        //    （万一 A 本来就是胜者，G2 在坏实现上照样绿）。
        cs.update(s, 1.0f / 60.0f);
        chk("G1 无导演时按 priority 选中 B", cs.activeEntity().id == eB.id,
            "active=" + std::to_string(cs.activeEntity().id));
        chk("G1 画面在 B 的位置", nearVec(s.camera().position(), glm::vec3(10, 0, 20), 1e-3f),
            v3(s.camera().position()));

        // G2 覆盖压过 priority：选 A（priority 0，输家）
        cs.setDirectorCamera(gA);
        cs.update(s, 1.0f / 60.0f);
        chk("G2 导演覆盖压过 priority（选 A）", cs.activeEntity().id == eA.id,
            "active=" + std::to_string(cs.activeEntity().id));
        chk("G2 画面真的换到 A 的位置", nearVec(s.camera().position(), glm::vec3(0, 0, 5), 1e-3f),
            v3(s.camera().position()));

        // G3 释放覆盖 → 立刻回 priority
        cs.clearDirectorCamera();
        cs.update(s, 1.0f / 60.0f);
        chk("G3 释放覆盖后回到 B", cs.activeEntity().id == eB.id,
            "active=" + std::to_string(cs.activeEntity().id));

        // G4 目标被销毁 → 回落 priority，且 guid 必须清零。
        //    ⚠ 清零这一步单独判：不清的话下一帧还会去找那个死 guid，
        //      warn 只打一次，之后整段过场静默地不切镜。
        cs.setDirectorCamera(gA);
        cs.update(s, 1.0f / 60.0f);
        s.world().destroy(eA);
        cs.update(s, 1.0f / 60.0f);
        // ⚠ 销毁会把最后一个实体**搬到被删的槽位**（swap-and-pop），所以
        //   `eB` 这个句柄已经过期 —— 必须按 guid 重新找。
        const ecs::Entity b2 = s.world().find(gB);
        chk("G4 目标被删后回落到 priority（B）",
            b2.valid() && cs.activeEntity().id == b2.id,
            b2.valid() ? ("active=" + std::to_string(cs.activeEntity().id))
                       : std::string("B 也找不到了"));
        chk("G4 导演 guid 被清零", cs.directorCamera() == 0,
            std::to_string(cs.directorCamera()));
    }

    // G5/G6 混合与 dt==0
    {
        Scene s;
        CameraSystem& cs = activeCameraSystem();
        cs.clearDirectorCamera();
        cs.clearDesiredPose();
        cs.clearShake();

        const ecs::Entity eA = addCamera(s, "A", glm::vec3(0, 0, 5), 0);
        const ecs::Entity eB = addCamera(s, "B", glm::vec3(10, 0, 20), 10);
        const std::uint64_t gA = s.world().guid(eA);
        const std::uint64_t gB = s.world().guid(eB);

        // 先硬切到 A，把画面稳定下来（blendFrom 要有一个确定的值）
        cs.setDirectorCamera(gA);
        cs.update(s, 1.0f / 60.0f);

        // G5 切到 B，blend=1.0s，走 0.5s。smoothStep(0.5)=0.5 →
        //    x 应该正好在**中点** 5。
        //    ⚠ 反向对照：没实现混合的话这里会是 10（一步到位）；
        //      混合被当成线性但 a 算错的话会是 2.5 或 7.5。
        cs.setDirectorCamera(gB);
        cs.blendCut(1.0f);
        cs.update(s, 0.5f);
        chk("G5 blend 走到一半时 x 在中点", near(s.camera().position().x, 5.0f, 0.3f),
            v3(s.camera().position()));
        chk("G5 blend 的一半不改 active（还是 B）", cs.activeEntity().id == eB.id,
            "active=" + std::to_string(cs.activeEntity().id));

        // 再走 0.6s → 混合结束，落到 B 的位置（不是卡在半路）
        cs.update(s, 0.6f);
        chk("G5b blend 结束后精确落在 B", nearVec(s.camera().position(), glm::vec3(10, 0, 20), 1e-3f),
            v3(s.camera().position()));

        // G6 dt==0 不推进混合：切回 A 但不给时间 → 画面还在 B
        cs.setDirectorCamera(gA);
        cs.blendCut(1.0f);
        cs.update(s, 0.0f);
        chk("G6 dt==0 时 blend 不推进（还在 B）",
            nearVec(s.camera().position(), glm::vec3(10, 0, 20), 1e-3f),
            v3(s.camera().position()));
        // 给够时间 → 到 A。证明上一条不是"混合压根没生效"
        cs.update(s, 1.0f);
        chk("G6b 给够时间后落到 A", nearVec(s.camera().position(), glm::vec3(0, 0, 5), 1e-3f),
            v3(s.camera().position()));

        cs.clearDirectorCamera();
        cs.clearDesiredPose();
    }

    std::printf("CAMMOVE-TOTAL pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail;
}
} // namespace scene
