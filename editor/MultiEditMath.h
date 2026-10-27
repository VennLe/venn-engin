#pragma once
// ============================================================
// editor/MultiEditMath —— 多选批量编辑的**纯函数**部分（#316）
//
// 单独拆出来的理由只有一条：这几行是"多选到底该怎么算"的全部歧义所在，
// 而它们在 Inspector 的 DragFloat3 里、在 gizmo 的拖拽循环里各写一遍就
// 会有两套语义。抽成纯函数之后：
//   · InspectorPanel 与 GizmoController 调的是同一份；
//   · MultiSelectSelfTest 能**不开窗口**逐个数值验证（否则只能靠截图）。
//
// 两种语义（这是整块唯一需要想清楚的东西）：
//
//   位置 / 旋转 —— **增量**
//     一致时（prior = 共同值）: base + (edit - prior) = edit     ← 绝对值
//     混合时（prior = 0）      : base + edit                    ← 都加这么多
//
//   缩放 —— **比例**
//     一致时（prior = 共同值）: base * (edit / prior) = edit     ← 绝对值
//     混合时（prior = 1）      : base * edit                    ← 都放大这么多倍
//
// 同一个式子覆盖两端，所以"拖到一半值从一致变成不一致"不会突然换语义
// （那正是"混合显示"最容易掉进去的坑：单独给混合写一条分支，于是拖过
// 共同值那一刻数字会跳一下）。
// ============================================================

#include <glm/glm.hpp>

#include <cmath>

namespace editor {

// 位置 / 旋转：返回 base 在这次编辑之后应当是的新值
inline glm::vec3 batchAdd(const glm::vec3& base, const glm::vec3& edit,
                          const glm::vec3& prior) {
    return base + (edit - prior);
}

// 缩放：按比例。prior 某个分量接近 0（有人把那一轴压扁了）时那一轴不动 ——
// 除以 0 会得到 inf，写进 TransformComponent 之后整棵子树的世界矩阵
// 全是 NaN，物体直接从视口消失（而且不报错）。
inline glm::vec3 batchMul(const glm::vec3& base, const glm::vec3& edit,
                          const glm::vec3& prior, float minScale = 0.001f) {
    glm::vec3 out = base;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(prior[i]) < 1e-6f) continue;
        out[i] = glm::max(base[i] * (edit[i] / prior[i]), minScale);
    }
    return out;
}

// 多选绕支点旋转：物体在支点周围"公转"。
//
// 只转朝向不转位置的后果是"每个物体在各自原地转身"——彼此的相对布局
// 一点没变，看起来像根本没转（球体上就是完全没有变化）。
inline glm::vec3 orbitPosition(const glm::vec3& startWorld,
                              const glm::vec3& pivot, const glm::mat3& D) {
    return pivot + D * (startWorld - pivot);
}

} // namespace editor
