#pragma once
// ============================================================
// animation/LookAt —— 头部看向（L2 表演层）
//
// 为什么单骨就够：
//   "看着你说话"在二次元里几乎全靠**头**。脖子/脊柱的跟随在特写镜头下
//   是加分项，但在全身镜头里几乎看不出来，而它会把"转头时肩膀跟着扭"这种
//   错误引进来。所以这里只做一根骨，明确地只做一根。
//
// ------------------------------------------------------------
// 三个容易做错的点
// ------------------------------------------------------------
//
// ① **必须在父骨空间里算，不在世界空间算。**
//    在世界空间算的话，角色本体一转（转身 / 被相机跟随改变朝向），头会
//    "拧"在原地不肯跟着身体 —— 因为目标点在世界里没动。
//    在父骨空间算：父骨（通常是 neck）转了，头自动跟着转，额外只补"差多少"。
//
// ② **forwardLocal 是"骨骼空间里的眼睛朝向"，不是模型前方。**
//    glTF 模型前方通常是 -Z，但那是 **mesh 空间**。骨骼的局部轴由 rest
//    姿势决定，两者不一定一致。所以它是可配的，默认 -Z（大多数 DCC 导出的
//    人形骨架碰巧也是这样）。
//    配错的失败模式非常明确：头会**朝后**看。改一下符号即可。
//
// ③ **必须限幅。**
//    无限转的头比不转更怪 —— 目标跑到背后时，头会扭 180°。二次元的做法是
//    "转到极限就停住"，剩下的交给身体朝向。
//
// ------------------------------------------------------------
// 为什么用最短弧 + 限幅，而不是分解成 yaw/pitch
// ------------------------------------------------------------
// yaw/pitch 分解要选两根参考轴（up / right），而骨骼空间的 up 不是恒定的
// （不同 DCC 的骨骼 roll 不同）。选错轴的失败模式是"头会歪着转"，很难调。
// 最短弧只需要一个方向，剩下两个自由度原样保留 —— 对头来说这恰好是想要的。
// ============================================================

#include "animation/Pose.h"
#include "assets/Skeleton.h"

#include <glm/glm.hpp>
#include <string>

namespace animation {

struct LookAtSettings {
    bool enabled = true;
    // target 有效才生效。分开一个 bool 是因为 (0,0,0) 是一个合法的目标点，
    // 不能拿"target 是不是零向量"当开关。
    bool hasTarget = false;

    // 看向哪根骨。空 → 在骨架里找第一个名字含 "head" 的（不区分大小写）。
    std::string boneName;
    // 目标点（**世界坐标**，glTF 场景空间 —— 与 Pose 同一套）。
    glm::vec3 target{0.0f, 0.0f, 0.0f};

    // 融合权重：0 → 逐位等于没有 LookAt（这条要能断言）。
    float weight = 1.0f;

    // 单次最多偏转多少度。超过就停住（见 ③）。
    float maxAngleDeg = 55.0f;

    // 平滑速率（1/s）。0 = 瞬间到位（会出现"眼球瞬移"的观感）。
    // 8~15 是"自然但不迟钝"的区间。
    float smoothRate = 12.0f;

    // 骨骼空间里的"眼睛朝向"（见 ②）。
    glm::vec3 forwardLocal{0.0f, 0.0f, -1.0f};
};

// 解析要驱动哪根骨。name 为空 → 自动找含 "head" 的（跳过名字里同时含
// "end"/"tip" 的末端辅助骨）。找不到返回 -1。
int resolveLookAtBone(const assets::Skeleton& skel, const std::string& name);

// 每个角色一份的运行时状态（不进存档）。
class LookAtState {
public:
    void reset() {
        m_delta = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        m_init = false;
    }
    bool initialized() const { return m_init; }

    // 就地修改 pose（只写一根骨的 rotation）。返回 1 = 本帧施加了旋转。
    int apply(const assets::Skeleton& skel, int bone, float dt,
              const LookAtSettings& s, Pose& pose);

    // 上一帧实际施加的偏转角（度）。断言与调试用 ——
    // "限幅真的生效了吗"只能靠这个数断，截图上分不出 55° 和 70°。
    float appliedAngleDeg() const;

private:
    glm::quat m_delta{1.0f, 0.0f, 0.0f, 0.0f};
    bool m_init = false;
};

} // namespace animation
