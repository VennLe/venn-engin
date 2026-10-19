#include "physics/Heightfield.h"

#include <algorithm>
#include <cmath>

namespace physics {

namespace {
// 双线性权重。arg 是连续采样坐标（可以越界），返回四个角的权重。
struct Bilerp {
    int c0, c1, r0, r1;
    float fc, fr;
};
Bilerp bilerpOf(const Heightfield& hf, const glm::vec2& g) {
    // 先钳到 [0, N-1]，再取整。越界时小数部分被吃成 0 → 退化成边界值，
    // 也就是"地形外面沿用边缘高度"。
    const float maxC = static_cast<float>(hf.cols - 1);
    const float maxR = static_cast<float>(hf.rows - 1);
    const float gc = std::min(std::max(g.x, 0.0f), maxC);
    const float gr = std::min(std::max(g.y, 0.0f), maxR);
    Bilerp b;
    b.c0 = static_cast<int>(std::floor(gc));
    b.r0 = static_cast<int>(std::floor(gr));
    b.fc = gc - static_cast<float>(b.c0);
    b.fr = gr - static_cast<float>(b.r0);
    // ⚠ 落在**最后一个**采样点上时，floor 给出 N-1，而格子只有 N-2..N-1。
    //   退回一格并把权重钉成 1 —— 否则 fc 会算成 2.0（外插），得到
    //   `2*h[N-1] - h[N-2]`：实测地形边缘 1.7e-4 m 的台阶状误差，
    //   而"地形最外一圈"正好是玩家最容易走到的地方。
    if (b.c0 >= hf.cols - 1) { b.c0 = hf.cols - 2; b.fc = 1.0f; }
    if (b.r0 >= hf.rows - 1) { b.r0 = hf.rows - 2; b.fr = 1.0f; }
    b.c1 = b.c0 + 1;
    b.r1 = b.r0 + 1;
    return b;
}
} // namespace

float Heightfield::raw(int col, int row) const {
    if (!heights) return 0.0f;
    col = std::min(std::max(col, 0), cols - 1);
    row = std::min(std::max(row, 0), rows - 1);
    return heights[static_cast<std::size_t>(col) * static_cast<std::size_t>(rows) +
                   static_cast<std::size_t>(row)];
}

float heightAt(const Heightfield& hf, float x, float y) {
    if (!hf.valid()) return 0.0f;
    const Bilerp b = bilerpOf(hf, hf.gridOf(glm::vec2(x, y)));
    const float h00 = hf.raw(b.c0, b.r0);
    const float h10 = hf.raw(b.c1, b.r0);
    const float h01 = hf.raw(b.c0, b.r1);
    const float h11 = hf.raw(b.c1, b.r1);
    // 先沿行（X）插值，再沿列（Y）插值。顺序不影响结果（双线性可交换），
    // 但两边都写同一个顺序，读代码时不用怀疑。
    const float a = h00 + (h10 - h00) * b.fc;
    const float c = h01 + (h11 - h01) * b.fc;
    return (a + (c - a) * b.fr) * hf.heightScale;
}

glm::vec3 normalAt(const Heightfield& hf, float x, float y) {
    if (!hf.valid()) return glm::vec3(0.0f, 0.0f, 1.0f);
    // 步长 = 半像素。中心差分：(h(x+d) - h(x-d)) / (2d)
    const float d = hf.metersPerPixel * 0.5f;
    const float hl = heightAt(hf, x - d, y);
    const float hr = heightAt(hf, x + d, y);
    const float hd = heightAt(hf, x, y - d);
    const float hu = heightAt(hf, x, y + d);
    // z = f(x,y) 的显式曲面法线：(-∂f/∂x, -∂f/∂y, 1)，再归一化
    glm::vec3 n(-(hr - hl) / (2.0f * d), -(hu - hd) / (2.0f * d), 1.0f);
    const float len = glm::length(n);
    return (len > 1.0e-8f) ? (n / len) : glm::vec3(0.0f, 0.0f, 1.0f);
}

bool raycast(const Heightfield& hf, const glm::vec3& origin,
             const glm::vec3& dir, float maxT, HeightHit& out) {
    out = HeightHit{};
    if (!hf.valid()) return false;
    const float dlen = glm::length(dir);
    if (dlen < 1.0e-8f) return false;
    const glm::vec3 d = dir / dlen;
    const float tMax = (maxT > 0.0f) ? maxT : 1.0e4f;

    auto f = [&](float t) {
        const glm::vec3 p = origin + d * t;
        return p.z - heightAt(hf, p.x, p.y);
    };

    const float f0 = f(0.0f);
    // 起点已在地表以下：判 t=0 命中。正常向下探测不会走到这里
    // （起点在地面之上），但相机/角色被摆到地形里时会。
    if (f0 <= 0.0f) {
        out.hit = true;
        out.t = 0.0f;
        out.point = origin;
        out.normal = normalAt(hf, origin.x, origin.y);
        return true;
    }

    // 步长：半像素。比像素小会把成本翻倍，而"洞"的最小尺度就是一个像素。
    const float step = std::max(hf.metersPerPixel * 0.5f, 0.05f);
    float tPrev = 0.0f;
    float fPrev = f0;
    for (float t = step; t <= tMax + step; t += step) {
        const float tc = std::min(t, tMax);
        const float fc = f(tc);
        if (fc <= 0.0f) {
            // 括号区间 [tPrev, tc] 内二分。高度场是分片双线性的，
            // 区间内可能有多个零点，但取最靠前的那个 —— 这正是"第一个交点"。
            float lo = tPrev;
            float hi = tc;
            for (int i = 0; i < 24; ++i) {
                const float mid = 0.5f * (lo + hi);
                if (f(mid) > 0.0f) lo = mid; else hi = mid;
            }
            const float tHit = 0.5f * (lo + hi);
            out.hit = true;
            out.t = tHit;
            out.point = origin + d * tHit;
            out.normal = normalAt(hf, out.point.x, out.point.y);
            return true;
        }
        tPrev = tc;
        fPrev = fc;
        if (tc >= tMax) break;
    }
    (void)fPrev;
    return false;
}

} // namespace physics
