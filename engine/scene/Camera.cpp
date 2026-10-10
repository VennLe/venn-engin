#include "scene/Camera.h"
#include "core/Input.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <cmath>

namespace scene {

using core::Input;

// 世界 up 轴：**Z 朝上**的右手系（与 Blender / 3ds Max 一致）。
// 相机相关的所有 up 假设都从这里取，改坐标系只动这一行。
static const glm::vec3 kWorldUp{0.0f, 0.0f, 1.0f};

Camera::Camera() = default;

void Camera::update(float dt) {
    // 编辑器视口接管输入时，这里不做事（见 setUseGlobalInput 的说明）
    if (!m_useGlobalInput) return;

    double dx = 0, dy = 0;
    Input::mouseDelta(&dx, &dy);

    // 飞行模式走一套完全不同的数学（转自己 + 沿本地轴平移）。若不分支，
    // 下面的轨道写法会直接改 m_target 而不动 m_flyPos，两者立刻对不上 ——
    // 表现为"相机纹丝不动 / 原地打转"。
    if (m_fly) {
        if (Input::mouseDown(GLFW_MOUSE_BUTTON_LEFT)) {
            look(static_cast<float>(dx) * 0.005f,
                 static_cast<float>(dy) * 0.005f);
        }
        const float speed =
            m_moveSpeed * dt * (Input::keyDown(GLFW_KEY_LEFT_SHIFT) ? 3.0f : 1.0f);
        float f = 0.0f, r = 0.0f, u = 0.0f;
        if (Input::keyDown(GLFW_KEY_W)) f += 1.0f;
        if (Input::keyDown(GLFW_KEY_S)) f -= 1.0f;
        if (Input::keyDown(GLFW_KEY_D)) r += 1.0f;
        if (Input::keyDown(GLFW_KEY_A)) r -= 1.0f;
        if (Input::keyDown(GLFW_KEY_E)) u += 1.0f;
        if (Input::keyDown(GLFW_KEY_Q)) u -= 1.0f;
        if (f != 0.0f || r != 0.0f || u != 0.0f) {
            moveLocal(glm::vec3(r * speed, u * speed, f * speed));
        }
        return;
    }

    // 旋转：左键拖拽
    if (Input::mouseDown(GLFW_MOUSE_BUTTON_LEFT)) {
        m_yaw -= static_cast<float>(dx) * 0.005f;
        m_pitch -= static_cast<float>(dy) * 0.005f;
        m_pitch = glm::clamp(m_pitch, -kPitchLimit, kPitchLimit);
    }

    // 缩放：滚轮
    double scroll = Input::scrollDelta();
    if (scroll != 0.0) {
        m_distance -= static_cast<float>(scroll) * 0.5f;
        m_distance = glm::clamp(m_distance, 0.5f, 100.0f);
    }

    // 平移：WASD（相机相对）+ QE（世界升降）
    float speed = m_moveSpeed * dt * (Input::keyDown(GLFW_KEY_LEFT_SHIFT) ? 3.0f : 1.0f);
    glm::vec3 forwardDir = glm::normalize(glm::vec3(m_target - position()));
    glm::vec3 rightDir = glm::normalize(glm::cross(forwardDir, kWorldUp));

    if (Input::keyDown(GLFW_KEY_W)) m_target += forwardDir * speed;
    if (Input::keyDown(GLFW_KEY_S)) m_target -= forwardDir * speed;
    if (Input::keyDown(GLFW_KEY_D)) m_target += rightDir * speed;
    if (Input::keyDown(GLFW_KEY_A)) m_target -= rightDir * speed;
    if (Input::keyDown(GLFW_KEY_E)) m_target += kWorldUp * speed;
    if (Input::keyDown(GLFW_KEY_Q)) m_target -= kWorldUp * speed;
}

// ---------------------------------------------------------------- 显式控制
//
// 这几个是"轨道模式"风格的入口（脚本 / 通用观察相机在用）。**飞行模式下会自动
// 改写成等价的"改相机位置 / 原地转头"操作** —— 否则它们只动 m_target，
// 而 m_flyPos 是独立存的，两者立刻脱节：画面上表现为"拖了没反应"，
// 或者相机绕着一个空点打转。四行 if 换掉一整类难查的 bug。

void Camera::orbit(float dYawRad, float dPitchRad) {
    m_yaw += dYawRad;
    m_pitch = glm::clamp(m_pitch + dPitchRad, -kPitchLimit, kPitchLimit);
    if (m_fly) syncFlyTarget();  // 位置不动、目标点转 → 原地转头
}

void Camera::zoomBy(float amount) {
    // 距离越远，每格滚轮走得越多 —— 否则拉远之后会"滚不动"
    const float delta = amount * 0.12f * glm::max(m_distance, 1.0f);
    m_distance = glm::clamp(m_distance - delta, 0.5f, 200.0f);
    if (m_fly) {
        // 飞行模式下"拉近"= 沿视线前进同样的距离（distance 同步缩掉，
        // 目标点仍落在相机正前方，语义保持一致）
        m_flyPos += forwardAxis() * delta;
        syncFlyTarget();
    }
}

void Camera::panWorld(const glm::vec3& d) {
    if (m_fly) {
        // 飞行模式没有"观察中心"可平移 —— 等价动作是让相机自己挪过去
        m_flyPos += d;
        syncFlyTarget();
        return;
    }
    m_target += d;
}

void Camera::dolly(float amount) {
    if (m_fly) {
        m_flyPos += forwardAxis() * amount;
        syncFlyTarget();
        return;
    }
    const glm::vec3 fwd = glm::normalize(m_target - position());
    m_target += fwd * amount;
}

// ---------------------------------------------------------------- 显式设定
//
// syncFlyTarget() 是"位置 → target"方向的同步；下面这两个是**相反方向**：
// 把 target / distance 当权威状态，反推出 m_flyPos。两者必须成对存在，
// 否则飞行模式下改 target/distance 就变成"改了，但画面没反应"。
//
// 典型调用者：Focus（把 target 挪到物体上、再算一个合适的 distance）。
// 两种模式下语义一致 —— 保持朝向不变，把相机摆到"目标在正前方 d 米"处。

void Camera::setTarget(const glm::vec3& t) {
    m_target = t;
    if (m_fly) m_flyPos = m_target + angleDir() * m_distance;
}

void Camera::setDistance(float d) {
    m_distance = glm::clamp(d, 0.5f, 100.0f);
    if (m_fly) m_flyPos = m_target + angleDir() * m_distance;
}

// ---------------------------------------------------------------- 自由飞行
//
// 场景：Play 时想给玩家一个 UE 视口那样的第一人称视角。
// 轨道相机的 position 是**推出来**的（target + dir*distance），如果直接
// 把 look() 实现成 orbit()，相机就会绕着目标点打转 —— 那是"环绕观察"，
// 不是"转头"，手感完全不对。所以飞行模式额外存一份 m_flyPos。
//
// 关键是怎么让两种模式共享同一套 yaw/pitch：
//   轨道模式：position = m_target + dir * distance
//   飞行模式：m_target  = position - dir * distance   （m_target 落在相机正前方）
// 两个式子互为逆运算，所以 setFlyMode 只是把某个量"移项"，位置与朝向
// 都不会跳 —— 见下面的实现，两种切换都只用一行赋值。

glm::vec3 Camera::angleDir() const {
    // Z-up 右手系：yaw 在水平面 XY 里转（yaw=0 → 相机在 +X 一侧），
    // pitch 是相对水平面的仰角（正 = 相机在上方俯视）。
    // yaw 增大 = 视线向右转：forward = -angleDir，d(forward)/d(yaw) 在
    // yaw=0 时 = (0, +cp, 0)，而看向 -X 时相机右方恰好是 +Y —— 自洽。
    const float cp = std::cos(m_pitch);
    return {cp * std::cos(m_yaw), -cp * std::sin(m_yaw), std::sin(m_pitch)};
}

void Camera::syncFlyTarget() {
    // m_target 保持在相机正前方 distance 米 —— 这样 viewMatrix() 与
    // target() 的既有语义（"相机看向哪里"）在两种模式下完全一致。
    m_target = m_flyPos - angleDir() * m_distance;
}

void Camera::setFlyMode(bool on) {
    if (on == m_fly) return;

    if (on) {
        // 轨道 → 飞行：把当前推出来的位置固化成 m_flyPos（m_fly 仍为 false，
        // position() 走的还是轨道分支）。随后 syncFlyTarget 算出的
        // m_target 恰好等于原来的轨道目标点 —— 画面完全不动。
        m_flyPos = position();
        m_fly = true;
        syncFlyTarget();
    } else {
        // 飞行 → 轨道：m_target 已在正确位置，position() 切回轨道分支后
        // 算出来仍是 m_flyPos —— 同样不跳。
        m_fly = false;
    }
}

void Camera::look(float dYawRad, float dPitchRad) {
    // 符号约定（推导见下）：yaw 增大 = 视线向右转；pitch 增大 = 视线向下压。
    //
    //   视线 forward = -angleDir()
    //   d(forward)/d(yaw) ∝ normalize(cross(forward, 世界up)) = 相机右方向
    //     （Z-up 下 up = (0,0,1)，该恒等式依然成立）
    //   forward.z = -sin(pitch) → pitch 变小 ⇒ 抬高视线（往上看）
    //
    // 于是鼠标右移(dx>0)配 +yaw 就是"往右看"，鼠标上移(dy<0)配 +pitch
    // 就是"往上看" —— 与 UE 视口按住鼠标转头的手感一致。
    m_yaw += dYawRad;
    m_pitch = glm::clamp(m_pitch + dPitchRad, -kPitchLimit, kPitchLimit);
    if (m_fly) syncFlyTarget();
}

glm::vec3 Camera::forwardAxis() const {
    return glm::normalize(m_target - position());
}

glm::vec3 Camera::rightAxis() const {
    const glm::vec3 f = forwardAxis();
    glm::vec3 r = glm::cross(f, kWorldUp);
    if (glm::length(r) < 1e-5f) return glm::vec3(1.0f, 0.0f, 0.0f);
    return glm::normalize(r);
}

void Camera::moveLocal(const glm::vec3& delta) {
    if (!m_fly) {
        // 非飞行模式退化成"平移观察目标"（与 panWorld 一致）
        m_target += delta;
        return;
    }
    m_flyPos += rightAxis() * delta.x + kWorldUp * delta.y + forwardAxis() * delta.z;
    syncFlyTarget();
}

glm::mat4 Camera::viewMatrix() const {
    return glm::lookAt(position(), m_target, kWorldUp);
}

glm::mat4 Camera::projMatrix(float aspect) const {
    glm::mat4 proj = glm::perspective(glm::radians(m_fovY), aspect, m_zNear, m_zFar);
    proj[1][1] *= -1.0f;  // Vulkan 裁剪空间 Y 翻转
    return proj;
}

glm::vec3 Camera::position() const {
    // 飞行模式：位置是独立存的（见 setFlyMode 的推导）；
    // 轨道模式：由 yaw/pitch/distance 从目标点反推出来。
    if (m_fly) return m_flyPos;
    return m_target + angleDir() * m_distance;
}

} // namespace scene
