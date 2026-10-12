#pragma once
// ============================================================
// render/ShadowCull.h —— 阴影 Pass 共用的"局部 AABB vs 光源视锥"剔除
//
// 为什么按 Pass 单独抽一个头：级联方向光（正交）与局部光源（透视）
// 用的是**同一套裁剪空间判据**，只是 w 分量不同。写在两处必然漂移，
// 而剔除写错的症状是"影子凭空缺一块"——比多画几个物体难查得多。
//
// 为什么在裁剪空间做（而不是在世界空间重建斜视锥逐个测平面）：
// 六个面退化成 x = ±w、y = ±w、z ∈ [0, w] 的区间比较，既简单又没有
// 平面方程的数值噪声。Vulkan 的 clip z 是 **Zero-to-One**，所以下界
// 是 0（不是 GL 的 -1）——这一点与投影矩阵必须用 *_ZO 系列是同一条约定。
// ============================================================

#include "rhi/VulkanCommon.h"

#include <glm/glm.hpp>

namespace render {

// 判断局部 AABB（lo..hi，世界空间）经 `clip`（世界 → 光源裁剪空间）
// 变换后，是否**可能与**光源视锥相交。
//
// 返回 false = 确定完全在外，可以安全跳过；返回 true = 保守保留。
//
// 两种失效模式都要防：
//   · **假阴性**（该画却剔掉）→ 影子缺一块，肉眼可见，绝不可接受；
//   · 假阳性（不该画却留着）→ 只损失一点提交量。
// 因此这里只做"整盒在同一侧之外才剔除"的判定，且对 w ≤ 0 的顶点
// （在光源背后、透视投影会翻号）直接放弃剔除。
inline bool visibleInLightClip(const glm::mat4& clip, const glm::vec3& lo,
                               const glm::vec3& hi) {
    glm::vec3 nlo(1e30f), nhi(-1e30f);
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 c((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y,
                          (i & 4) ? hi.z : lo.z);
        const glm::vec4 p = clip * glm::vec4(c, 1.0f);
        // w ≤ 0：顶点在光源背后。正交投影下 w 恒为 1（永不触发）；
        // 透视投影下这会把坐标翻到镜像象限，AABB 判据随即失效 → 保守保留。
        if (p.w <= 1e-6f) return true;
        const glm::vec3 n = glm::vec3(p) / p.w;
        nlo = glm::min(nlo, n);
        nhi = glm::max(nhi, n);
    }
    return nhi.x >= -1.0f && nlo.x <= 1.0f && nhi.y >= -1.0f &&
           nlo.y <= 1.0f && nhi.z >= 0.0f && nlo.z <= 1.0f;
}

} // namespace render
