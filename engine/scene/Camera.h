#pragma once
// ============================================================
// scene/Camera —— 相机
//
// 两种模式，共用同一套 yaw/pitch/position 状态：
//
//   ① 轨道模式（默认）—— 通用观察相机（绕目标点打转）
//      "观察目标" m_target 是真正的旋转中心，相机始终看向它。
//
//   ② 自由飞行模式（setFlyMode(true)）—— **编辑器视口**用
//      位置**直接存**（m_flyPos），绕相机自身旋转（look），
//      沿相机本地轴平移（moveLocal）。这就是 UE 视口的 WASD 飞行手感：
//      按住右键转头 + WASD 平移，相机不绕任何点打转。
//      （Play 态的运行态相机也是这一种 —— 游戏视角本来就是第一人称式的。）
//
// 两种模式怎么做到"切换瞬间画面不跳"：
//   轨道模式下 position = m_target + dir * distance，
//   而 dir = angleDir() 指向**目标的背面** —— 于是两者互为逆运算：
//
//       m_target  = position - dir * distance     （m_target 在相机正前方 distance 米）
//       position  = m_target + dir * distance
//
//   飞行模式只是把等号左边的 position 独立存下来，m_target 仍按上式维护，
//   viewMatrix()/target() 一行都不用改。正因如此，来回切换时相机位置与
//   朝向都是连续的（见 Camera.cpp 里 setFlyMode 的说明）。
// ============================================================

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace scene {

class Camera {
public:
    Camera();

    // 处理输入（每帧调用；内部读取 core::Input 静态状态）
    void update(float dt);

    // 关掉全局输入（编辑器视口用）。
    // 视口是 ImGui 窗口 → ImGui 会 WantCaptureMouse，core::Input 的
    // mouseDown/mouseDelta 全部返回 0，相机就永远转不动。编辑器改为
    // 自己判断"鼠标是否悬停在视口内"再调下面这几个显式接口。
    void setUseGlobalInput(bool enabled) { m_useGlobalInput = enabled; }
    bool useGlobalInput() const { return m_useGlobalInput; }

    // ---- 显式控制（编辑器视口导航用）----
    void orbit(float dYawRad, float dPitchRad);
    void zoomBy(float amount);           // 滚轮：正数拉远
    void panWorld(const glm::vec3& d);   // 平移观察目标
    void dolly(float amount);            // 沿视线方向推拉

    // ---- 自由飞行（Play 模式的游戏视角，UE 风格）----
    void setFlyMode(bool on);
    bool flyMode() const { return m_fly; }

    // 绕**相机自身**旋转视角（第一人称"看"）。单位弧度。
    // 轨道相机绕的是 m_target，直接拿来当第一人称用会让相机自己绕着
    // 目标打转 —— 那是"环绕观察"，不是"转头"。
    void look(float dYawRad, float dPitchRad);

    // 沿相机本地轴平移：x = 右, y = 世界上方, z = 视线前方（单位：米）
    void moveLocal(const glm::vec3& delta);

    glm::vec3 forwardAxis() const;  // 视线方向（单位向量）
    glm::vec3 rightAxis() const;    // 相机右方向（单位向量）

    glm::mat4 viewMatrix() const;
    glm::mat4 projMatrix(float aspect) const;

    glm::vec3 position() const;
    const glm::vec3& target() const { return m_target; }
    float distance() const { return m_distance; }
    float yaw() const { return m_yaw; }
    float pitch() const { return m_pitch; }

    // 注意：这两个 setter 会**同步 m_flyPos**（见文件头的逆运算关系）。
    // 只改一边会让 target/distance 与 position 对不上 —— 那样 "聚焦到
    // 某物体"（Focus）在飞行模式下就会看着像没反应。实现里两种模式都照顾到。
    void setTarget(const glm::vec3& t);
    void setDistance(float d);
    void setFov(float fov) { m_fovY = glm::clamp(fov, 10.0f, 120.0f); }
    // 改朝向的两个 setter 同样要照顾飞行模式：位置不动、目标点跟着转，
    // 否则 Inspector 面板里的 Yaw / Pitch 滑块会"能拖，但画面没反应"。
    void setYaw(float y) {
        m_yaw = y;
        if (m_fly) syncFlyTarget();
    }
    void setPitch(float p) {
        m_pitch = glm::clamp(p, -1.52f, 1.52f);
        if (m_fly) syncFlyTarget();
    }
    float fov() const { return m_fovY; }
    float nearPlane() const { return m_zNear; }
    float farPlane() const { return m_zFar; }

private:
    // 由 yaw/pitch 得到的单位方向（指向相机**背后**，见文件头的公式推导）
    glm::vec3 angleDir() const;
    // 把 m_target 重新摆到"相机正前方 distance 米"处（飞行模式下位置/朝向一变就调）
    void syncFlyTarget();

    glm::vec3 m_target{0.0f, 0.5f, 0.0f};
    float m_yaw = -0.8f;    // 水平角（弧度）
    float m_pitch = 0.45f;  // 俯仰角（弧度），正 = 相机在上方俯视
    float m_distance = 7.0f;
    float m_fovY = 45.0f;
    float m_zNear = 0.1f;
    float m_zFar = 200.0f;
    float m_moveSpeed = 4.0f;
    bool m_useGlobalInput = true;

    // ---- 自由飞行模式 ----
    bool m_fly = false;
    glm::vec3 m_flyPos{0.0f, 0.0f, 0.0f};  // 飞行模式下相机的真实位置
};

} // namespace scene
