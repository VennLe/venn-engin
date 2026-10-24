#include "SpringArm.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace scene {

namespace {

constexpr float kPi = 3.14159265f;

// 绕 dir 正交基（用于把 8 条探测射线摆成一个圆）
void orthoBasis(const glm::vec3& d, glm::vec3* u, glm::vec3* v) {
    // 取一条与 d 夹角最大的世界轴，避免叉乘退化
    const glm::vec3 axis = (std::fabs(d.z) < 0.9f) ? glm::vec3(0.0f, 0.0f, 1.0f)
                                                   : glm::vec3(1.0f, 0.0f, 0.0f);
    *u = glm::normalize(glm::cross(axis, d));
    *v = glm::cross(d, *u);
}

} // namespace

SpringArmResult resolveSpringArm(const SpringArmCast& cast,
                                 const glm::vec3& pivot,
                                 const glm::vec3& dir, float desiredDistance,
                                 float minDistance, float radius) {
    SpringArmResult out;
    out.distance = std::max(desiredDistance, 0.0f);

    if (!cast || out.distance < 1.0e-4f) {
        out.position = pivot + dir * out.distance;
        return out;
    }

    const float len = glm::length(dir);
    const glm::vec3 d = (len > 1.0e-6f) ? (dir / len) : glm::vec3(0.0f, -1.0f, 0.0f);

    // 多探一个相机半径：给相机本身留体积，否则近裁剪面会切进墙里。
    const float reach = out.distance + radius;

    float best = reach;          // 球心能走到的轴向距离

    // ---- ① 中心射线：沿轴向最远能走 reach - radius ----
    {
        const float hit = cast(pivot, d, reach);
        if (hit < reach) {
            best = std::min(best, hit - radius);
        }
    }

    // ---- ② 绕轴 8 条偏移射线：起点落在球面上 ----
    //
    // 球心前进 t 时，球面在偏移方向 u 上的点走到 pivot + u*radius + d*t。
    // 所以"这条射线在距离 t 处命中" ⟺ "球心在 t 处刚好碰到"。
    glm::vec3 u(1.0f, 0.0f, 0.0f), v(0.0f, 1.0f, 0.0f);
    orthoBasis(d, &u, &v);
    constexpr int kRing = 8;
    for (int i = 0; i < kRing; ++i) {
        const float a = (6.2831853f * static_cast<float>(i)) /
                        static_cast<float>(kRing);
        const glm::vec3 off = u * (std::cos(a) * radius) + v * (std::sin(a) * radius);
        const glm::vec3 o = pivot + off;
        const float hit = cast(o, d, reach);
        if (hit < best) best = hit;
    }

    // ---- ③ 从"球心能走多远"换算成臂长 ----
    //
    // best 的语义统一为**球心能前进的轴向距离**（中心射线已减半径，环射线
    // 因为起点就在球面上不必再减）。再让出 2 cm 余量，免得相机贴在墙面上
    // 与墙面 z-fighting。
    constexpr float kMargin = 0.02f;
    float arm = out.distance;
    if (best < reach) {
        // ⚠ 这里**不** clamp 到 minDistance：最小臂长只是构图上的舒适值，
        //   为了它把镜头留在墙体内部是本末倒置（UE 的 SpringArm 同样让
        //   碰撞结果优先）。只在硬下限处止步。
        arm = std::clamp(best - kMargin, kSpringArmHardMin, out.distance);
        if (minDistance > 0.0f && arm < minDistance) {
            // 走到这里说明"墙比舒适下限还近"。保持 arm（贴脸），
            // 并让调用方从 blocked 得知这不是正常构图。
        }
    }

    out.distance = arm;
    out.blocked = arm < desiredDistance - 1.0e-4f;
    out.position = pivot + d * arm;
    return out;
}

bool clampCameraAboveGround(const glm::vec3& pivot, float minZ,
                            glm::vec3* position, float* distance,
                            float* pitch) {
    if (!position) return false;
    if (position->z >= minZ) {
        // 没触发：把当前几何原样解出来（调用方通常会拿它覆盖原始 pitch，
        // 因为地面钳制可能在**更早**的帧生效过、而这一帧又回到正常）。
        const glm::vec3 toCam = *position - pivot;
        const float len = glm::length(toCam);
        if (distance) *distance = len;
        if (pitch && len > 1.0e-5f) {
            *pitch = std::asin(std::clamp(toCam.z / len, -1.0f, 1.0f));
        }
        return false;
    }

    position->z = minZ;
    const glm::vec3 toCam = *position - pivot;
    const float len = glm::length(toCam);
    if (distance) *distance = len;
    if (pitch && len > 1.0e-5f) {
        // asin 的分子是 (相机 - 枢轴).z：相机在枢轴上方 → 正 pitch。
        *pitch = std::asin(std::clamp(toCam.z / len, -1.0f, 1.0f));
    }
    return true;
}

// ============================================================ 自检

namespace {

int g_saPass = 0;
int g_saFail = 0;

void scheck(const char* what, bool ok, const std::string& detail = "") {
    if (ok) {
        ++g_saPass;
        std::printf("SPRINGARM-TEST pass %s%s%s\n", what,
                    detail.empty() ? "" : " — ", detail.c_str());
    } else {
        ++g_saFail;
        std::printf("SPRINGARM-TEST FAIL %s%s%s\n", what,
                    detail.empty() ? "" : " — ", detail.c_str());
    }
}

std::string num(float v) {
    char b[64];
    std::snprintf(b, sizeof(b), "%.4f", static_cast<double>(v));
    return std::string(b);
}

// 一整面无限大的墙（x = wallX 平面，法线 +X）
SpringArmCast fullWall(float wallX) {
    return [wallX](const glm::vec3& o, const glm::vec3& d, float maxDist) {
        if (d.x > -1.0e-6f) return maxDist;              // 不朝墙飞
        const float t = (wallX - o.x) / d.x;
        return (t >= 0.0f && t <= maxDist) ? t : maxDist;
    };
}

// 半面墙（x = wallX 平面，但只在 y >= yEdge 处存在）
//
// 这就是"射线够不到、球够得到"的最小可测模型：枢轴在原点、臂朝 -X，
// 中心射线的 y 恒为 0 → 从半墙旁边擦过去；而球扫掠的环射线起点带
// y = ±radius 的横向偏移，其中 +radius 那条会真的撞上。
SpringArmCast halfWall(float wallX, float yEdge) {
    return [wallX, yEdge](const glm::vec3& o, const glm::vec3& d, float maxDist) {
        if (d.x > -1.0e-6f) return maxDist;
        const float t = (wallX - o.x) / d.x;
        if (t < 0.0f || t > maxDist) return maxDist;
        const float hitY = o.y + d.y * t;
        return (hitY >= yEdge) ? t : maxDist;
    };
}

} // namespace

int runSpringArmSelfTest() {
    g_saPass = 0;
    g_saFail = 0;
    std::printf("SPRINGARM-TEST begin (no window)\n");

    const glm::vec3 pivot(0.0f);
    const glm::vec3 back(-1.0f, 0.0f, 0.0f);   // 相机在 -X 方向
    constexpr float kDesired = 5.0f;
    constexpr float kMin = 0.60f;
    constexpr float kRadius = 0.25f;

    // ---- S1 什么都不挡 → 臂长就是期望值 ----
    {
        const SpringArmResult r = resolveSpringArm(
            nullptr, pivot, back, kDesired, kMin, kRadius);
        scheck("S1 无碰撞查询时臂长 = 期望值", std::fabs(r.distance - kDesired) < 1.0e-5f,
               "arm=" + num(r.distance));
        scheck("S1b 无碰撞时不算 blocked", !r.blocked);
    }

    // ---- S2 正对整面墙：臂长 = 墙距 - 半径 - 余量 ----
    //
    // 这是"一条射线"也能算对的情形，写在这里当基准：命中 2.0，球心最多
    // 走到 2.0 - 0.25 = 1.75，再让 2 cm 余量 → 1.73。
    {
        const SpringArmResult r = resolveSpringArm(
            fullWall(-2.0f), pivot, back, kDesired, kMin, kRadius);
        const float expect = 2.0f - kRadius - 0.02f;
        scheck("S2 正墙：臂长 = 墙距 - 相机半径 - 余量",
               std::fabs(r.distance - expect) < 1.0e-4f,
               "arm=" + num(r.distance) + " expect=" + num(expect));
        scheck("S2b 正墙：blocked = 1", r.blocked);
        scheck("S2c 相机停在墙前", r.position.x > -2.0f,
               "cam.x=" + num(r.position.x));
    }

    // ---- S3 半墙：中心射线打不到，靠环上的横向偏移射线才发现 ----
    //
    // **这就是把射线换成球扫掠的全部意义**。如果这里 arm 还是 5.0，
    // 说明实现退化回了单射线 —— 而那正是"贴墙时镜头钻进墙里"的根因。
    {
        const SpringArmResult r = resolveSpringArm(
            halfWall(-1.0f, 0.15f), pivot, back, kDesired, kMin, kRadius);
        // 半径 0.25 > yEdge 0.15 → +y 那条环射线在 t=1.0 命中
        scheck("S3 半墙：斜向/边缘也能发现（单射线会漏）",
               r.distance < 1.2f, "arm=" + num(r.distance));
        scheck("S3b 半墙：blocked = 1", r.blocked);
    }

    // ---- S4 墙比"舒适下限"还近：宁可贴脸也不留在墙里 ----
    //
    // 这一条对应 UE 的 MinTargetArmLength 语义：它只是构图偏好。
    // 墙在 0.45 m，球半径 0.25 → 球心最多走到 0.20，臂长 0.18 < kMin 0.60。
    {
        const SpringArmResult r = resolveSpringArm(
            fullWall(-0.45f), pivot, back, kDesired, kMin, kRadius);
        scheck("S4 墙比最小臂长还近时，臂长可以突破 kMin",
               r.distance < kMin && r.distance > kSpringArmHardMin,
               "arm=" + num(r.distance) + " kMin=" + num(kMin));
        scheck("S4b 相机中心仍在墙前", r.position.x > -0.45f,
               "cam.x=" + num(r.position.x));
    }

    // ---- S5 贴到脸上：硬下限止步，但绝不到反方向去 ----
    {
        const SpringArmResult r = resolveSpringArm(
            fullWall(-0.10f), pivot, back, kDesired, kMin, kRadius);
        scheck("S5 极窄空间：臂长止于硬下限",
               r.distance >= kSpringArmHardMin - 1.0e-6f,
               "arm=" + num(r.distance) + " hardMin=" + num(kSpringArmHardMin));
        scheck("S5b 极窄空间：相机中心不越过墙", r.position.x > -0.10f,
               "cam.x=" + num(r.position.x));
    }

    // ---- S6 相机半径越大，臂越短（单调）----
    {
        const SpringArmResult a = resolveSpringArm(
            fullWall(-2.0f), pivot, back, kDesired, kMin, 0.10f);
        const SpringArmResult b = resolveSpringArm(
            fullWall(-2.0f), pivot, back, kDesired, kMin, 0.40f);
        scheck("S6 半径越大臂越短", b.distance < a.distance - 1.0e-4f,
               "r=0.10 -> " + num(a.distance) + ", r=0.40 -> " +
                   num(b.distance));
    }

    // ---- S7 地面钳制：反解出的 (pitch, distance) 必须能重建同一个位置 ----
    //
    // 符号错的那一版会在这里挂：`pivot.z - pos.z` 取反之后，用 pitch 重建
    // 出来的位置跑到枢轴**另一侧**（对地钳制来说就是跑到地底下）。
    {
        const glm::vec3 p(0.0f, 0.0f, 1.2f);       // 枢轴：头肩高度
        glm::vec3 cam = p + back * 4.0f;           // 4 m 的臂，水平
        cam.z = -1.0f;                             // 仰视到极限 → 相机在地底
        const float minZ = 0.25f;                  // 地表 0 + clearance 0.25

        float dist = 0.0f, pitch = 0.0f;
        const bool clamped =
            clampCameraAboveGround(p, minZ, &cam, &dist, &pitch);
        scheck("S7 相机遇地钳制报告发生过", clamped);
        scheck("S7b 钳制后相机在地表之上",
               cam.z >= minZ - 1.0e-6f, "cam.z=" + num(cam.z));
        scheck("S7c 反解 pitch 的符号：相机在枢轴下方 → pitch 为负",
               pitch < 0.0f, "pitch=" + num(pitch));

        // 用 (pitch, distance) 重建 Camera 的位置。做法与 CameraSystem 一致：
        // angleDir(pitch, yaw) = (cos(pitch)·cos(yaw), -cos(pitch)·sin(yaw),
        //                         sin(pitch))
        // 这条用例里相机在 -X 方向 → yaw = π（cos = -1, sin = 0）。
        const glm::vec3 dir(std::cos(pitch) * std::cos(kPi),
                            -std::cos(pitch) * std::sin(kPi), std::sin(pitch));
        const glm::vec3 rebuilt = p + dir * dist;
        const float err = glm::length(rebuilt - cam);
        scheck("S7d 用反解的 (pitch,distance) 重建位置 = 钳制后的位置",
               err < 1.0e-4f,
               "err=" + num(err) + " rebuilt=(" + num(rebuilt.x) + "," +
                   num(rebuilt.y) + "," + num(rebuilt.z) + ")");
        scheck("S7e 镜头仍然看着枢轴（不突然看天）", rebuilt.z < p.z,
               "rebuilt.z=" + num(rebuilt.z));
    }

    // ---- S8 不需要钳制时原样返回 ----
    {
        const glm::vec3 p(0.0f, 0.0f, 1.2f);
        glm::vec3 cam = p + back * 4.0f;
        cam.z = 1.2f;                              // 平视，离地很高
        const glm::vec3 before = cam;
        float dist = 0.0f, pitch = 0.0f;
        const bool clamped = clampCameraAboveGround(p, 0.25f, &cam, &dist, &pitch);
        scheck("S8 不需钳制时不报告", !clamped);
        scheck("S8b 不需钳制时位置不变", glm::length(cam - before) < 1.0e-6f);
        scheck("S8c 平视时 pitch = 0", std::fabs(pitch) < 1.0e-5f,
               "pitch=" + num(pitch));
        scheck("S8d 平视时 distance = 4", std::fabs(dist - 4.0f) < 1.0e-5f,
               "dist=" + num(dist));
    }

    std::printf("SPRINGARM-TOTAL pass=%d fail=%d\n", g_saPass, g_saFail);
    return g_saFail;
}

} // namespace scene
