// ============================================================
// animation/BoneMask.cpp
// ============================================================

#include "animation/BoneMask.h"

#include <algorithm>
#include <cctype>

namespace animation {

namespace {

std::string toLower(const std::string& s) {
    std::string r = s;
    for (char& c : r)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

} // namespace

const char* boneMaskModeName(BoneMaskMode m) {
    switch (m) {
        case BoneMaskMode::All: return "All";
        case BoneMaskMode::Allow: return "Allow";
        case BoneMaskMode::Deny: return "Deny";
    }
    return "?";
}

bool boneMaskModeFromInt(int v, BoneMaskMode* out) {
    if (v < 0 || v > 2) return false;
    if (out) *out = static_cast<BoneMaskMode>(v);
    return true;
}

BoneMask buildBoneMask(const assets::Skeleton& skel, const BoneMaskSpec& spec) {
    BoneMask m;
    const int n = skel.jointCount();
    if (n <= 0) return m;
    m.weights.assign(static_cast<size_t>(n), 0.0f);

    // All（或 roots 空）= 全骨。**roots 空但 mode 是 Allow 也算全骨**：
    // UI 上"选成 Allow 还没来得及填根"不该让角色突然变成 T-pose。
    if (spec.isAll()) {
        std::fill(m.weights.begin(), m.weights.end(), 1.0f);
        m.included = n;
        return m;
    }

    // ---- ① 种子：每个 root 匹配**全部**命中骨 ----
    std::vector<char> seed(static_cast<size_t>(n), 0);
    for (const std::string& raw : spec.roots) {
        if (raw.empty()) continue;
        const std::string want = toLower(raw);
        int hits = 0;
        for (int i = 0; i < n; ++i) {
            if (toLower(skel.joints[static_cast<size_t>(i)].name)
                    .find(want) != std::string::npos) {
                seed[static_cast<size_t>(i)] = 1;
                ++hits;
            }
        }
        if (hits == 0) {
            ++m.unresolved;
            if (m.firstProblem.empty()) m.firstProblem = raw;
        }
    }

    // ---- ② 子树：沿父链往上找种子 ----
    // ⚠ 不假设 `joints` 按拓扑序排（`topoOrder` 才是权威顺序，而遮罩要
    //   在装配期就算出来）。父链走法 O(n·depth)，63 根骨的深度不到 10，
    //   而且在缓存里只算一次。
    // ⚠ 带 guard：骨架数据损坏（父链成环）时不能死循环 —— 那种情况下
    //   编辑器会直接卡死，比"遮罩算错"严重得多。
    std::vector<char> inSub(static_cast<size_t>(n), 0);
    for (int i = 0; i < n; ++i) {
        int cur = i;
        int guard = 0;
        while (cur >= 0 && cur < n && guard++ <= n) {
            if (seed[static_cast<size_t>(cur)]) {
                inSub[static_cast<size_t>(i)] = 1;
                break;
            }
            cur = skel.joints[static_cast<size_t>(cur)].parent;
        }
    }

    // ---- ③ 落成权重 ----
    const bool allow = (spec.mode == BoneMaskMode::Allow);
    for (int i = 0; i < n; ++i) {
        const bool in = inSub[static_cast<size_t>(i)] != 0;
        const bool take = allow ? in : !in;
        m.weights[static_cast<size_t>(i)] = take ? 1.0f : 0.0f;
        if (take) ++m.included;
    }
    return m;
}

std::string boneMaskKey(const BoneMaskSpec& spec) {
    std::string k = boneMaskModeName(spec.mode);
    k += '\x1f';
    for (const std::string& r : spec.roots) {
        k += r;
        k += '\x1f';
    }
    return k;
}

void blendPoseMasked(const Pose& base, const Pose& layer, const BoneMask& mask,
                     float weight, Pose& out) {
    // 先整体拷一份：这样 `out` 与 `base` 同址也安全（调用方常常直接传
    // `m_pose`，多一次拷贝换掉一整类"别名改坏输入"的 bug）。
    out = base;

    const int n = base.count();
    if (n <= 0 || weight <= 0.0f || mask.empty()) return;

    // 三边取最小：mask 是按**同一具骨架**解出来的，但 layer 姿势可能来自
    // 另一具骨架（用户把层挂到了关节数更少的模型上）。越界读比"少混几根
    // 骨"严重得多 —— 后者只是姿势不对，前者是崩溃。
    const int m = std::min(n, std::min(layer.count(), mask.count()));
    for (int j = 0; j < m; ++j) {
        const size_t jj = static_cast<size_t>(j);
        const float w = mask.weights[jj] * weight;
        if (w <= 0.0f) continue;
        if (w >= 1.0f) {
            // 逐位短路（见头文件约束 ②）
            out.rotation[jj] = layer.rotation[jj];
            out.translation[jj] = layer.translation[jj];
            out.scale[jj] = layer.scale[jj];
            continue;
        }
        out.rotation[jj] = glm::slerp(base.rotation[jj], layer.rotation[jj], w);
        out.translation[jj] =
            glm::mix(base.translation[jj], layer.translation[jj], w);
        out.scale[jj] = glm::mix(base.scale[jj], layer.scale[jj], w);
    }
}

} // namespace animation
