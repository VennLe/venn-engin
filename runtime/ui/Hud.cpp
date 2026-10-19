#include "Hud.h"

#include "core/Logger.h"
#include "i18n/I18n.h"
#include "ui2/SdfFont.h"
#include "ui2/Ui2Settings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace runtime {

namespace {

// ---- 参考分辨率下的尺寸由 UiConfig 给（placeRect 统一乘 scale）----

// 配色。**交换链是 sRGB**，这些值就是设计稿上的值（shader 里反编码一次）。
uint32_t colBarBack() { return ui2::rgba(0, 0, 0, 150); }
uint32_t colBarTrail() { return ui2::rgba(255, 255, 255, 205); }
uint32_t colBarHealth() { return ui2::rgba(217, 58, 58, 255); }
uint32_t colCrosshair() { return ui2::rgba(255, 255, 255, 190); }
uint32_t colDamage() { return ui2::rgba(255, 92, 72, 255); }

ui2::UiPlacement barPlacement(const UiConfig& cfg) {
    ui2::UiPlacement p;
    p.anchor = ui2::UiAnchor::BottomLeft;
    p.x = cfg.barMargin;
    p.y = cfg.barMargin;
    p.w = cfg.barWidth;
    p.h = cfg.barHeight;
    return p;
}

}  // namespace

ui2::UiRect Hud::healthBarRect(uint32_t outW, uint32_t outH) const {
    return ui2::placeRect(barPlacement(m_cfg), outW, outH);
}

ui2::UiRect Hud::healthFillRect(const HudModel& m, uint32_t outW,
                                uint32_t outH) const {
    ui2::UiRect r = healthBarRect(outW, outH);
    r.w *= m.healthRatio();
    return r;
}

bool Hud::worldToScreen(const glm::mat4& viewProj, const glm::vec3& world,
                        uint32_t outW, uint32_t outH, glm::vec2* out) {
    if (!out || outW == 0 || outH == 0) return false;
    const glm::vec4 clip = viewProj * glm::vec4(world, 1.0f);
    // ① 相机背后：**整体丢弃**。w 接近 0 时 ndc 会炸到极大值，
    //    钳到边框上会得到一个"永远贴在屏幕边上的数字"。
    if (clip.w <= 1e-5f) return false;
    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    // ② Y 只翻**一次**：投影矩阵里已经翻过（见头文件）。
    const float sx = (ndc.x * 0.5f + 0.5f) * static_cast<float>(outW);
    const float sy = (ndc.y * 0.5f + 0.5f) * static_cast<float>(outH);
    // ③ 屏幕外 → 不画
    if (sx < 0.0f || sx > static_cast<float>(outW)) return false;
    if (sy < 0.0f || sy > static_cast<float>(outH)) return false;
    out->x = sx;
    out->y = sy;
    return true;
}

void Hud::build(ui2::UiCanvas& c, const HudModel& m,
                const glm::mat4& viewProj, uint32_t outW, uint32_t outH,
                const ui2::SdfFont* font) const {
    const float s = ui2::computeScale(outW, outH);
    if (s <= 0.0f || outW == 0 || outH == 0) return;

    // ---- 准星（中心，4 条线 + 中心点）----
    // 画在**血条之前**：血条在角落，两者不重叠，但顺序固定下来，
    // 以后加"准星命中变色"时不用再考虑层级。
    if (m.showCrosshair && m_cfg.crosshair) {
        const float cx = static_cast<float>(outW) * 0.5f;
        const float cy = static_cast<float>(outH) * 0.5f;
        const float gap = m_cfg.crosshairGap * s;
        const float len = m_cfg.crosshairLen * s;
        const float thick = std::max(1.0f, 2.0f * s);
        const uint32_t col = colCrosshair();
        c.rect(cx - gap - len, cy - thick * 0.5f, len, thick, col);      // 左
        c.rect(cx + gap, cy - thick * 0.5f, len, thick, col);            // 右
        c.rect(cx - thick * 0.5f, cy - gap - len, thick, len, col);      // 上
        c.rect(cx - thick * 0.5f, cy + gap, thick, len, col);            // 下
        const float dot = std::max(1.0f, 2.0f * s);
        c.rect(cx - dot * 0.5f, cy - dot * 0.5f, dot, dot, col);          // 中心
    }

    // ---- 血条 ----
    const ui2::UiRect back = healthBarRect(outW, outH);
    // 填充矩形提到 if 外面：HUD-STATS 要量它（"扣血 30 → 宽度缩到 70%"
    // 是 U8 的验收条款，量几何比数像素可靠）。注意 fill 是按 ratio 缩过的，
    // **不会**因为 outW 变了就跟 back 同比例。outW 为 0 时 placeRect 返回
    // 空矩形，fill 也就自然是 0 —— 不需要额外守卫。
    const ui2::UiRect fill = healthFillRect(m, outW, outH);
    if (back.w > 0.0f && back.h > 0.0f) {
        const float radius = back.h * 0.5f;
        c.roundRect(back.x, back.y, back.w, back.h, radius, colBarBack());

        // 白色残影（先画，血条盖在上面）—— 宽度按 trail，比血条宽出来的
        // 那一截就是"刚掉了多少"。
        const ui2::UiRect trail = [&] {
            ui2::UiRect r = back;
            const float ratio =
                (m.maxHealth > 0.0f) ? (m.healthTrail / m.maxHealth) : 0.0f;
            r.w *= std::max(0.0f, std::min(1.0f, ratio));
            return r;
        }();
        if (trail.w > 0.5f)
            c.roundRect(trail.x, trail.y, trail.w, trail.h, radius,
                        colBarTrail());

        if (fill.w > 0.5f)
            c.roundRect(fill.x, fill.y, fill.w, fill.h, radius,
                        colBarHealth());

        // 数值（有字体才画）：右下角对齐在血条右侧上方
        if (font && font->valid()) {
            char buf[48];
            std::snprintf(buf, sizeof(buf), "%d / %d",
                          static_cast<int>(std::lround(m.healthShown)),
                          static_cast<int>(std::lround(m.maxHealth)));
            const float size = 15.0f * s;
            const float tw = c.measure(*font, buf, size);
            c.text(*font, back.right() - tw, back.y - 6.0f * s, buf,
                   ui2::rgba(230, 237, 243, 235), size);
        }
    }

    // ---- 交互提示（血条右上方）----
    if (!m.hint.empty() && font && font->valid()) {
        const char* text = i18n::tr(m.hint.c_str());
        const float size = 18.0f * s;
        const float tw = c.measure(*font, text, size);
        const float x = back.right() + 24.0f * s;
        const float baseline = back.y + back.h * 0.5f + size * 0.35f;
        c.text(*font, x, baseline, text, ui2::rgba(255, 255, 255, 220), size);
        (void)tw;
    }

    // ---- 伤害数字（世界 → 屏幕，上浮 + 淡出）----
    for (const DamagePopup& p : m.popups) {
        glm::vec2 sp;
        if (!worldToScreen(viewProj, p.worldPos, outW, outH, &sp)) continue;
        const float t = std::max(0.0f, std::min(1.0f, p.t));
        const float alpha = 1.0f - t;
        if (alpha <= 0.02f) continue;

        // 上浮 60 参考像素 + 字号从大变小（打击感：先"炸"出来再稳住）
        const float rise = 60.0f * s * t;
        const float size = (28.0f - 6.0f * t) * s;

        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d",
                      static_cast<int>(std::lround(p.amount)));

        const int a = static_cast<int>(alpha * 255.0f);
        const uint32_t col = ui2::rgba(255, 92, 72, a);

        if (font && font->valid()) {
            const float tw = c.measure(*font, buf, size);
            c.text(*font, sp.x - tw * 0.5f, sp.y - rise, buf, col, size);
        } else {
            // 没有字体时画一个小方块（至少能证明"位置对了"）——
            // 这不是兜底美化，是让无字体环境下的验收也能量到东西。
            const float q = 4.0f * s;
            c.rect(sp.x - q, sp.y - rise - q, q * 2.0f, q * 2.0f, col);
        }
    }

    // ---- 锚点：HUD-STATS（每 60 帧一行）----
    if (++m_frames % 60 == 0) {
        VK_LOG_INFO("HUD-STATS popups=%u health=%.1f shown=%.1f trail=%.1f "
                    "bar=%.0f,%.0f,%.0fx%.0f fill=%.0f,%.0f,%.0fx%.0f "
                    "out=%ux%u scale=%.4f",
                    static_cast<unsigned>(m.popups.size()),
                    static_cast<double>(m.health),
                    static_cast<double>(m.healthShown),
                    static_cast<double>(m.healthTrail),
                    static_cast<double>(back.x), static_cast<double>(back.y),
                    static_cast<double>(back.w), static_cast<double>(back.h),
                    static_cast<double>(fill.x), static_cast<double>(fill.y),
                    static_cast<double>(fill.w), static_cast<double>(fill.h),
                    static_cast<unsigned>(outW), static_cast<unsigned>(outH),
                    static_cast<double>(s));
    }
}

}  // namespace runtime
