#pragma once
// ============================================================
// physics/CharacterController —— 运动学胶囊角色（G07）
//
// ------------------------------------------------------------
// 为什么角色不能是刚体
// ------------------------------------------------------------
// 用动态刚体 + 摩擦做角色，会同时踩到三个坑：
//   · 台阶：胶囊卡在棱上，要么挡住要么弹飞（真实刚体没有"迈腿"）
//   · 斜坡：静摩擦不够就一路下滑，够了一停就"粘"在坡上
//   · 数值：求解器为了维持站立每帧都在纠偏，站立时有肉眼可见的微抖
// 所以角色是**运动学**的：位置由本模块直接决定，只把结果写回世界当
// 运动学代理（于是它能推动箱子，但不会被箱子推走）。
//
// ------------------------------------------------------------
// 一步分五段（顺序不能换）
// ------------------------------------------------------------
//   ① 水平速度朝"期望速度"靠拢（地面全权 / 空中打折）
//   ② 竖直速度积分（重力）
//   ③ 水平扫掠：分小步推进 + 逐体做最小平移；撞墙的那一分量被消掉
//      （于是自然沿着墙滑）→ 这就是"不产生反弹"的实现方式
//   ④ 台阶：水平被挡住时，抬高 stepHeight 重试，再落回
//   ⑤ 贴地：向下探 groundSnapDistance；走下坡时吸附，避免"小跳"
//
// 坡度判定用的是**接触法线的 z 分量**：n.z ≥ cos(slopeLimit) 视为可站
// （地面 / 缓坡），否则视为墙（陡坡、台阶立面）。45° 坡的 n.z = 0.707；
// 所以 slopeLimit=46° 时 40° 站得住、55° 滑下去。
//
// ------------------------------------------------------------
// 分步长度为什么是 0.05 m
// ------------------------------------------------------------
// "按剩余距离切步"的粗粒度会在高速下穿过薄墙（50 m/s、1/60 秒 = 0.83 m，
// 一步就跨过 0.1 m 的墙）。这里把步长钉成 min(radius/2, 0.05)，最多 64 步；
// 50 m/s 时一步约 0.83 m → 17 步，每步都有机会发现墙面。
// ============================================================

#include "physics/Heightfield.h"
#include "physics/RigidBody.h"

#include <glm/glm.hpp>

#include <string>

namespace physics {

// 重力加速度（m/s²，Z-up 所以是**负值**）。
//
// 为什么放在头文件而不是 .cpp 的匿名命名空间里（2026-10-22）：编辑器
// Play 态的跳跃要**手工积分**同一条抛物线（它没有 PlayerController /
// 物理世界），如果那边自己写一个 -9.81，两边的跳跃高度迟早漂开 ——
// 而"编辑器里跳得比出包后高"这种差异只能靠肉眼发现。
constexpr float kGravity = -9.81f;

class PhysicsWorld;

struct CharacterDesc {
    // ---- 尺寸（position 是**胶囊中心**；总高 = 2×(halfHeight + radius)）----
    float radius = 0.35f;
    float halfHeight = 0.55f;

    // ---- 行走 ----
    float stepHeight = 0.35f;         // 能迈过去的最大台阶（米）
    float slopeLimitDeg = 46.0f;      // 超过这个坡度就站不住
    bool groundSnap = true;           // 下坡吸附（关掉会有"小跳"）
    float groundSnapDistance = 0.30f; // 吸附探测距离（米）

    // ---- 运动 ----
    float gravityScale = 1.0f;
    float maxSpeed = 4.5f;            // m/s（输入侧的上限，供 PlayerController 用）
    // 跳跃初速 3.7 m/s → 高度 v²/2g ≈ 0.70 m、滞空 0.75 s。
    // 之前的 5.0 m/s 是 1.27 m —— 那是游戏手感，不是人能跳的高度。
    float jumpSpeed = 3.7f;           // m/s
    float acceleration = 40.0f;       // m/s²（地面）
    // 空中操控 0.45：保留惯性（起跳前怎么跑，空中就怎么飞），但按住方向键
    // 能明显把落点掰过去。0.25 时几乎掰不动；0.8 以上变成空中走路，
    // 抛物线的手感就没了。
    float airControl = 0.45f;         // 空中加速度打折
    float pushStrength = 1.0f;        // 推箱子的力度倍率（运动学代理的"质量感"）

    // ---- 地形高度场（W4-3）----
    // 非 nullptr 时，⑥ 贴地**先问高度场**再问物理世界的射线。
    //
    // ⚠ 必须能"完全不接"：nullptr（默认）时 ⑥ 的代码路径与引入高度场之前
    //   逐位相同 —— 现有全部用例（A/B/C/D/E 五组）都只有 groundPlane，
    //   它们断言的是"贴地贴得准不准"，任何为了地形而改动的默认行为都会
    //   让那 5 组一起漂。
    // ⚠ 只用于**贴地**。水平方向撞进山坡**不做**胶囊扫掠（第一版非目标，
    //   见 docs/world_scale_roadmap.md §9 W4-3）：角色会直接走上陡坡，
    //   坡度超过 slopeLimit 时因为"贴不住 → 重力接管"而滑下来。
    const Heightfield* ground = nullptr;
};

// 角色的持久状态（跨帧）。运行态数据，**不进场景 JSON**。
struct CharacterState {
    glm::vec3 position{0.0f};          // 胶囊中心（世界空间）
    glm::vec3 velocity{0.0f};          // 水平 + 竖直（世界空间）
    bool onGround = false;
    float groundNormalZ = 1.0f;
    float timeSinceGrounded = 10.0f;   // coyote time 用
};

struct CharacterStepResult {
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    bool onGround = false;
    bool hitWall = false;
    bool steppedUp = false;
    bool jumped = false;
    float groundNormalZ = 1.0f;
    float movedDistance = 0.0f;
};

// 推进一步。`desiredVelocity` 只取水平分量（相机相对方向 × 速度）。
// `jumpRequested` 为真且贴地时给一次竖直速度（二段跳由脚本决定，不在这里）。
CharacterStepResult stepCharacter(const PhysicsWorld& world,
                                  const CharacterDesc& desc,
                                  CharacterState& state,
                                  const glm::vec3& desiredVelocity,
                                  float dt, bool jumpRequested = false,
                                  BodyId ignore = kInvalidBody);

// 单个胶囊的扫掠结果（台阶/贴地内部复用，也便于自检直接断言）
struct CapsuleSweep {
    glm::vec3 position{0.0f};
    bool hit = false;
    float minNormalZ = 1.0f;   // 接触法线里最小的 z（越小越"竖"）
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    float moved = 0.0f;
};

// 把胶囊从 from 沿 delta 推过去；遇障碍贴着表面滑（不反弹）。
CapsuleSweep sweepCapsule(const PhysicsWorld& world, float radius,
                          float halfHeight, const glm::vec3& from,
                          const glm::vec3& delta, BodyId ignore = kInvalidBody);

// ------------------------------------------------------------ 自检
// 无窗口、无 Vulkan：脚本化地造平台 / 台阶 / 斜坡 / 薄墙，逐条断言。
// 由 `Game.exe --selftest character` 调用。
int runCharacterSelfTest();

} // namespace physics
