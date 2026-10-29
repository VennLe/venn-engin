#pragma once
// ============================================================
// animation/Constraint —— **约束栈**（S-C，2026-10-29）
//
// 为什么要有这一层：
//   在这之前"表演层后处理"是**三段写死的代码**（`AnimatorSystem::
//   applyPerformance`）：LookAt → TwoBoneIK → SpringBone。这个顺序是常量，
//   不能改、不能跳过、不能叠第二份，也不存在"这根骨上先限位再复制旋转"。
//   而作者真正要的是 Blender 那种**骨上一条有序的表**：
//
//       weapon.R:  ChildOf(hand.R)  →  LimitRotation(X: -90..90)
//       arm.L:     CopyRotation(arm.R) → TwoBoneIK(hand.L)
//
//   顺序就是语义（第二条读到的是第一条改完的结果），所以才叫"栈"而不是
//   "三个特性"。本模块把这件事做成数据：`ConstraintStack = vector<Constraint>`，
//   按 `items` 的顺序**逐条求值**，每条只改自己那根骨的局部 TRS。
//
// ------------------------------------------------------------
// 三条设计约定（和 `RigEdit` / `WeightSet` 是同一个套路）
// ------------------------------------------------------------
//   ① **`influence <= 0` 或 `!enabled` → 一个字节都不写。**
//      不是"乘 0 再写回去"：`slerp(q, q, 0)` 在浮点下**不是恒等**，会引入
//      1e-7 的漂移，长跑之后角色会慢慢转歪。所以每条约束的第一行都是短路
//      return，"关掉约束 ≡ 没有这个约束"这条必须能逐位断言。
//      同理 `LimitRotation` 在**限幅以内**时也必须原样返回（不 decompose →
//      recompose）：否则"给一根没超限的骨加个限位约束"会让整条轨道漂。
//
//   ② **骨一律用下标（`int`），不用名字。**
//      约束栈跟的是**一具具体的骨架**，改名/换骨架时就该失效，而不是悄悄
//      绑到另一根同名骨上。落盘时写成名字（人看得懂、重导入后还能对上），
//      读盘时用 `Skeleton::findJoint` 解析回来 —— 解析不到就留 -1（该条
//      失效）并计数，绝不猜。
//
//   ③ **只读/写 `Pose`，不碰骨架、不碰 GPU。**
//      和 `TwoBoneIK` / `LookAt` / `SpringBone` 一样：输入一份局部姿态，
//      输出改过的局部姿态。于是可以在不自检窗口、不建渲染器的情况下断言，
//      也保证了求值顺序里"上一条的改动"能被下一条看见（世界矩阵按需重算）。
//
// ------------------------------------------------------------
// 世界矩阵是**按需重算**的
// ------------------------------------------------------------
//   `CopyRotation` / `CopyLocation` / `ChildOf` / IK 都要看世界矩阵，而
//   `LimitRotation` 只看自己那根骨的局部旋转。所以求值器拿一个 `dirty`
//   标志：任何一条约束写完就置脏，下一条真要世界矩阵时重算一次
//   （`poseToWorld` 是 63 次 mat4 乘，几条约束下可以忽略）。
//   不这样做的话，"先转上臂、再复制给前臂"会读到**转之前**的世界矩阵 ——
//   症状是前臂差一个上臂的旋转量，而且只在有两条以上约束时出现。
//
// ------------------------------------------------------------
// 与老路径的关系
// ------------------------------------------------------------
//   `TwoBoneIK` 仍然保留独立入口（`solveTwoBoneIK`），约束栈里的 IK 是**薄
//   包装**：构造 `TwoBoneIKSettings` 后调它。不重写解算 —— 那份余弦定理里
//   的 `maxReachScale = 0.999`（正好伸直时 `acos(1.0000001) → NaN → 整条链
//   消失）和"极向量从当帧动画反推"两条，只该有一个实现。
// ============================================================

#include "animation/Pose.h"
#include "assets/Skeleton.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace animation {

// 约束类型。数字值**进文件**（`venn.constraints`），所以只许追加、不许改。
enum ConstraintType : int {
    kConLimitRotation = 0,   // 局部欧拉每轴限幅（肘不能反折）
    kConCopyRotation = 1,    // 世界旋转抄给目标骨
    kConCopyLocation = 2,    // 世界位置抄给目标骨（+偏移）
    kConChildOf = 3,         // 像挂在目标骨下面一样跟着走（rest 时定 inverse）
    kConTwoBoneIK = 4,       // 两骨解析 IK（薄包装 solveTwoBoneIK）
    kConTypeCount = 5,
};

const char* constraintTypeName(int t);
// 从名字解析类型（区分大小写不敏感）。失败返回 false，`out` 不动。
bool constraintTypeFromName(const std::string& name, int& out);

struct Constraint {
    int type = kConLimitRotation;
    bool enabled = true;

    // 融合权重 0..1。0 = 完全没这条约束（逐位）。
    // ⚠ 这个值必须**可打帧**（S-C 的验收之一）：FK 切 IK 就是给它打 0→1
    //   两帧，不需要在采样链里再加一个混合分支。
    float influence = 1.0f;

    // 被约束的骨（下标，-1 = 没绑上/没解析到）。
    int bone = -1;

    // ---- Copy* / ChildOf 的源骨 ----
    int target = -1;
    // CopyLocation：世界轴掩码（bit0=x bit1=y bit2=z）+ 偏移。
    int axisMask = 7;
    glm::vec3 offset{0.0f};

    // ---- LimitRotation：每轴限幅（度）+ 掩码 ----
    // ⚠ 存成"每轴 min/max"而不是一个 cone：肘/膝的可用区间不是各向同性的，
    //   cone 只能表达"偏离静止 N 度以内"，表达不了"只能往一个方向弯"。
    float minDeg[3] = {-180.0f, -180.0f, -180.0f};
    float maxDeg[3] = {180.0f, 180.0f, 180.0f};
    int limitMask = 7;

    // ---- TwoBoneIK ----
    // 三根骨用下标（`resolveTwoBoneChain` 里"留空自动挑最长子骨"那套启发式
    // 仍然生效：任一项 < 0 就当没指定）。
    int ikRoot = -1;
    int ikMid = -1;
    int ikEnd = -1;
    bool ikHasTarget = false;
    glm::vec3 ikTarget{0.0f};          // 世界坐标（glTF 场景空间，见 TwoBoneIK.h）
    int ikPoleMode = 0;                // 0 = 从当帧动画反推，1 = 用 ikPole
    glm::vec3 ikPole{0.0f, 0.0f, 1.0f};
    float ikMaxReach = 0.999f;
    // 拉伸：目标够不到时允许骨链变长多少倍（0 = 不拉伸，0.1 = 最长 1.1 倍）。
    // 只改 mid/end 的局部平移长度，不改旋转 —— 于是"拉伸"和"朝向"互不干扰。
    float stretch = 0.0f;

    // 需要世界矩阵吗（限幅不需要）。求值器靠它决定要不要重算 FK。
    bool needsWorld() const;
};

// 一次求值的账。`CONSTRAINT-EVAL` 日志与自检断言都吃它。
struct ConstraintEvalResult {
    int total = 0;        // 栈里几条
    int applied = 0;      // 真的写了 pose 的条数
    int skipped = 0;      // 关掉 / influence 0 / 限幅内（= 没写）
    int failed = 0;       // 绑不上骨 / 目标无效 / 成不了链
    int clamped = 0;      // 只对 LimitRotation：真的夹到了的条数
    int ikSolved = 0;
};

// 约束栈本体。`items` 的顺序就是求值顺序。
struct ConstraintStack {
    std::vector<Constraint> items;

    int size() const { return static_cast<int>(items.size()); }
    bool empty() const { return items.empty(); }

    // 增删移。返回新下标（add）/ 受影响条数（其余）。
    int add(const Constraint& c);
    int removeAt(int i);
    int moveUp(int i);      // 与上一条交换（i <= 0 → 不动，返回 0）
    int moveDown(int i);
    int clearBone(int bone);
    int clear();

    // 找一个"已经挂在 bone 上的同类约束"（UI 加条目时避免重复到分不清）。
    int indexOf(int bone, int type) const;

    Constraint* at(int i);
    const Constraint* at(int i) const;

    // 按顺序求值，就地改 `pose`。**不改** skel / 不分配全局状态。
    ConstraintEvalResult evaluate(const assets::Skeleton& skel, Pose& pose) const;

    // 内容摘要（FNV-1a 64）。跨进程比对"同一份栈"用它 —— 比逐个字段比
    // 更省事，也顺手抓"换了个类型名字但数值没变"这种半吊子改动。
    unsigned long long digest() const;

    void logSummary(const char* tag) const;
};

// 落盘：`venn.constraints` / version 1。骨**同时写名字和下标**：
//   名字给人看、也让"重导入之后下标全变了"这件事在 diff 里看得见；
//   读盘优先用下标（校验越界），下标无效时**不猜**（留 -1，计入 `unresolved`）。
// 不拿骨架骨数当硬校验：重导入之后骨数本来就可能变（导入器去重/补骨），
// 那样子等于"资产一更新就读不进来"。
bool saveConstraints(const std::string& path, const ConstraintStack& stack,
                     const assets::Skeleton& skel, std::string& err);
bool loadConstraints(const std::string& path, const assets::Skeleton& skel,
                     ConstraintStack& out, std::string& err,
                     int* unresolved = nullptr);

} // namespace animation
