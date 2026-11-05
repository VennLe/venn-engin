#include "animation/LeaderPose.h"

#include <algorithm>

namespace animation {

void buildLeaderBoneMap(const assets::Skeleton& follower,
                        const assets::Skeleton& leader,
                        std::vector<int>& out, int* unmatched) {
    const int n = follower.jointCount();
    out.assign(static_cast<size_t>(n), -1);
    int miss = 0;

    // 同一份资产（body 的 follower 就是 body 自己，或两个组件共用同一个
    // `Model*`）→ 下标必然一一对应，不必查名字。
    // 这不是"优化"，是**正确性保险**：重名骨在合并过的骨架里真实存在，
    // 查表只能返回第一个，而下标恒等对"同一个骨架"永远是对的。
    const bool sameSkeleton = (&follower == &leader);
    for (int j = 0; j < n; ++j) {
        if (sameSkeleton) {
            out[static_cast<size_t>(j)] = j;
            continue;
        }
        const std::string& nm = follower.joints[static_cast<size_t>(j)].name;
        // 只开大小写不敏感：**不含** kJointMatchSubstring / SkipEndTips。
        const int k = leader.findJoint(nm, assets::kJointMatchCaseInsensitive);
        out[static_cast<size_t>(j)] = k;
        if (k < 0) ++miss;
    }
    if (unmatched) *unmatched = miss;
}

void remapPose(const assets::Skeleton& follower, const Pose& leaderPose,
               const std::vector<int>& map, Pose& out) {
    out.setRest(follower);
    const int n = std::min(follower.jointCount(), static_cast<int>(map.size()));
    const int avail = leaderPose.count();
    for (int j = 0; j < n; ++j) {
        const int s = map[static_cast<size_t>(j)];
        if (s < 0 || s >= avail) continue;
        out.translation[static_cast<size_t>(j)] =
            leaderPose.translation[static_cast<size_t>(s)];
        out.rotation[static_cast<size_t>(j)] =
            leaderPose.rotation[static_cast<size_t>(s)];
        out.scale[static_cast<size_t>(j)] =
            leaderPose.scale[static_cast<size_t>(s)];
    }
}

} // namespace animation
