#include "gameplay/DamageNumbers.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace gameplay {

namespace {

// ⚠ 这里**不 include `ui2/UiCanvas.h`**：`gameplay/` 那一层的约定是"纯数据 +
//   纯函数，连 rhi 都不认识"（见 GameplaySelfTest.h）。为了一个颜色打包函数
//   把整条 UI 通道拖进来，`--selftest gameplay` 就得跟着编 ui2。
//   字节序与 `ui2::rgba` **必须一致**（R 在最低地址），否则红蓝互换。
constexpr uint32_t packRgba(int r, int g, int b, int a = 255) {
    return (static_cast<uint32_t>(r & 0xFF)) |
           (static_cast<uint32_t>(g & 0xFF) << 8) |
           (static_cast<uint32_t>(b & 0xFF) << 16) |
           (static_cast<uint32_t>(a & 0xFF) << 24);
}
constexpr uint32_t packRgbA(uint32_t rrggbb, int a = 255) {
    return packRgba(static_cast<int>((rrggbb >> 16) & 0xFF),
                    static_cast<int>((rrggbb >> 8) & 0xFF),
                    static_cast<int>(rrggbb & 0xFF), a);
}

// 整数散列（PCG 家族的 xor-shift 混合）。用它而不是 `std::rand`：
// 同一个 `serial` 必须永远得到同一个偏移（见头注释第 2 条）。
uint32_t hash32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

// -1..1 的确定性"随机"数。`axis` 只是换个盐，让 x/y 不相关。
float driftUnit(uint32_t serial, uint32_t axis) {
    const uint32_t h = hash32(serial * 0x9E3779B9u + axis * 0x85EBCA6Bu);
    return (static_cast<float>(h & 0xFFFFu) / 65535.0f) * 2.0f - 1.0f;
}

int roundToInt(float v) {
    // `lround(0.4)` = 0 → 屏幕上出现一个 "0"，看起来像"伤害被吞了"。
    // 非零的小数一律显示成 1。0 本身保持 0（"确实没造成伤害"要能表达）。
    const int n = static_cast<int>(std::lround(v));
    if (n == 0 && v > 0.0f) return 1;
    if (n == 0 && v < 0.0f) return -1;
    return n;
}

// 描边/颜色的 alpha 只乘一次（`color` 是 sRGB 字节序 RGBA）
uint32_t withAlpha(uint32_t c, float a) {
    if (a < 0.0f) a = 0.0f;
    if (a > 1.0f) a = 1.0f;
    const uint32_t keep = c & 0x00FFFFFFu;
    const uint32_t al = static_cast<uint32_t>(a * 255.0f + 0.5f) & 0xFFu;
    return keep | (al << 24);
}

} // namespace

const PopupStyle& popupStyle(PopupKind k) {
    // ⚠ 索引就是枚举值，**只能追加**。中间插一条会让"暴击"显示成"治疗"，
    //   而这种错在画面上完全合理（都是数字），自查不出来。
    static const PopupStyle table[static_cast<int>(PopupKind::Count)] = {
        // color 是 RGBA 字节序（见上面的 packRgba）；表里用 0xRRGGBB 读起来
        // 才像设计稿上的值，所以一律过 `packRgbA(0xRRGGBB, 255)`。
        /* Damage */ {packRgbA(0xFFD9A0, 255), 26.0f, 0.85f, 52.0f, 1.28f,
                      0.18f, 0.60f, 14.0f, true, "", "", nullptr},
        /* Crit   */ {packRgbA(0xFF7A3C, 255), 36.0f, 1.05f, 74.0f, 1.55f,
                      0.16f, 0.60f, 22.0f, true, "", "!", nullptr},
        /* Heal   */ {packRgbA(0x6EE7A0, 255), 26.0f, 0.95f, 56.0f, 1.22f,
                      0.18f, 0.60f, 12.0f, true, "+", "", nullptr},
        /* Shield */ {packRgbA(0x7CC6FF, 255), 24.0f, 0.95f, 50.0f, 1.20f,
                      0.18f, 0.60f, 12.0f, true, "[", "]", nullptr},
        /* Buff   */ {packRgbA(0xC9A6FF, 255), 22.0f, 0.85f, 44.0f, 1.15f,
                      0.18f, 0.55f, 10.0f, true, "", "", nullptr},
        /* Miss   */ {packRgbA(0xB8C2CE, 255), 22.0f, 0.80f, 40.0f, 1.10f,
                      0.20f, 0.55f, 8.0f, false, "", "", "MISS"},
    };
    const int i = static_cast<int>(k);
    if (i < 0 || i >= static_cast<int>(PopupKind::Count)) return table[0];
    return table[i];
}

bool projectToScreen(const glm::mat4& viewProj, const glm::vec3& world,
                     uint32_t outW, uint32_t outH, glm::vec2* out) {
    if (!out || outW == 0 || outH == 0) return false;
    const glm::vec4 clip = viewProj * glm::vec4(world, 1.0f);
    // ① 相机背后：**整体丢弃**。`w` 接近 0 时 ndc 会炸到极大值，
    //    钳到边框上会得到一个"永远贴在屏幕边上的数字"。
    if (clip.w <= 1e-5f) return false;
    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    // ② Y 只翻**一次**：投影矩阵里已经翻过（见 Hud.h 头注释）。
    const float sx = (ndc.x * 0.5f + 0.5f) * static_cast<float>(outW);
    const float sy = (ndc.y * 0.5f + 0.5f) * static_cast<float>(outH);
    // ③ 屏幕外 → 不画
    if (sx < 0.0f || sx > static_cast<float>(outW)) return false;
    if (sy < 0.0f || sy > static_cast<float>(outH)) return false;
    out->x = sx;
    out->y = sy;
    return true;
}

void DamageNumberLayer::clear() {
    m_items.clear();
    m_serial = 0;
    m_merged = 0;
    m_dropped = 0;
}

bool DamageNumberLayer::spawn(const PopupSpawn& s) {
    // ---- 合并：同点 + 同类 + 年龄接近 ----
    if (s.merge) {
        for (DamageNumber& n : m_items) {
            if (n.kind != s.kind) continue;
            if (n.t * n.life() > kMergeWindow) continue;
            const glm::vec3 d = n.worldPos - s.worldPos;
            if (glm::dot(d, d) > kMergeRadius * kMergeRadius) continue;
            n.amount += s.amount;
            ++n.combo;
            // ⚠ **不重置 `t`**：重置会让"每帧都在挨打的目标"永远弹不掉，
            //   屏幕上留一条永生的数字。合并只累加数值，不放慢它变老。
            ++m_merged;
            return false;
        }
    }

    DamageNumber n;
    n.worldPos = s.worldPos;
    n.amount = s.amount;
    n.kind = s.kind;
    n.t = 0.0f;
    n.combo = 1;
    n.serial = ++m_serial;
    // 漂移只在这里算一次（之后上浮/淡出都不再引入随机）
    n.driftX = driftUnit(n.serial, 1u) * popupStyle(s.kind).driftRef;
    n.driftY = driftUnit(n.serial, 2u) * popupStyle(s.kind).driftRef * 0.4f;

    if (m_items.size() < kMax) {
        m_items.push_back(n);
        return true;
    }
    // 池满：替换**最老**的那条（t 最大的）。挤掉最新的会让"刚打中的
    // 那一击"看不见，正好反了。
    std::size_t oldest = 0;
    for (std::size_t i = 1; i < m_items.size(); ++i)
        if (m_items[i].t > m_items[oldest].t) oldest = i;
    m_items[oldest] = n;
    ++m_dropped;
    return false;
}

void DamageNumberLayer::tick(float dt) {
    if (dt <= 0.0f) return;   // 暂停时数字不该偷偷变老
    for (std::size_t i = 0; i < m_items.size();) {
        const float life = m_items[i].life();
        m_items[i].t += (life > 0.0f) ? (dt / life) : 1.0f;
        if (m_items[i].t >= 1.0f) {
            m_items[i] = m_items.back();
            m_items.pop_back();
        } else {
            ++i;
        }
    }
}

std::string DamageNumberLayer::formatText(const DamageNumber& n) {
    const PopupStyle& st = n.style();
    std::string text;
    if (st.fixed && st.fixed[0]) {
        text = st.fixed;
    } else {
        char buf[48];
        const int v = roundToInt(n.amount);
        if (st.suffix && st.suffix[0])
            std::snprintf(buf, sizeof(buf), "%s%d%s", st.prefix ? st.prefix : "",
                          v, st.suffix);
        else
            std::snprintf(buf, sizeof(buf), "%s%d", st.prefix ? st.prefix : "", v);
        text = buf;
    }
    if (n.combo > 1) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), " x%d", n.combo);
        text += buf;
    }
    return text;
}

void DamageNumberLayer::layout(const glm::mat4& viewProj, uint32_t outW,
                               uint32_t outH, float uiScale,
                               std::vector<PopupLayout>& out) const {
    out.clear();
    if (uiScale <= 0.0f) return;
    for (const DamageNumber& n : m_items) {
        glm::vec2 sp;
        if (!projectToScreen(viewProj, n.worldPos, outW, outH, &sp)) continue;

        const PopupStyle& st = n.style();
        float t = n.t;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;

        // ---- 三条曲线 ----
        // 上浮：先快后慢（ease-out）。线性上浮看着像"匀速电梯"。
        const float ease = 1.0f - (1.0f - t) * (1.0f - t);
        // 缩放：出生时弹大一点，`popTime` 之内收回 1
        float sizeMul = 1.0f;
        if (st.popTime > 0.0f && t < st.popTime) {
            const float k = 1.0f - t / st.popTime;   // 1 → 0
            sizeMul = 1.0f + (st.popScale - 1.0f) * k;
        }
        // 淡出：`hold` 之前不透明，之后线性收到 0
        float alpha = 1.0f;
        if (t > st.hold) {
            const float span = std::max(0.001f, 1.0f - st.hold);
            alpha = 1.0f - (t - st.hold) / span;
        }
        if (alpha <= 0.02f) continue;

        PopupLayout L;
        L.centerX = sp.x + n.driftX * uiScale;
        L.baselineY = sp.y - st.rise * ease * uiScale + n.driftY * uiScale;
        L.sizePx = st.size * sizeMul * uiScale;
        L.alpha = alpha;
        L.color = withAlpha(st.color, alpha);
        L.outline = st.outline;
        L.text = formatText(n);
        out.push_back(std::move(L));
    }
}

} // namespace gameplay
