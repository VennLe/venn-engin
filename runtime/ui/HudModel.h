#pragma once
// ============================================================
// runtime/ui/HudModel —— HUD 的**纯数据**（U8）
//
// 为什么数据与绘制分开：
//   · 数据可以在**无窗口**的自检里推进（伤害数字的寿命、血条的残影回落
//     全是纯数值，能毫秒级断言）；
//   · 绘制要 UiCanvas，而 UiCanvas 又只在有帧的时候存在。
// 这条分层与 `engine/ui2` 的"UiCanvas 不认识 Vulkan"是同一条思路。
//
// ------------------------------------------------------------
// 血量为什么有三个数
// ------------------------------------------------------------
//   `health`       权威值（玩法写它）
//   `healthShown`  显示值（平滑追 health，掉血时看得见"血在流走"）
//   `healthTrail`  白色残影（**延迟**回落，一眼看出"刚掉了多少"）
// 少任何一个都会让血条变成"跳一下就没了"——玩家来不及读到"我掉了 30"。
// ============================================================

#include <glm/glm.hpp>

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace runtime {

// 一条伤害数字。`t` 是**归一化寿命**：0 = 刚出现，1 = 该消失。
struct DamagePopup {
    glm::vec3 worldPos{0.0f};
    float amount = 0.0f;
    float t = 0.0f;
};

struct HudModel {
    // 对象池容量。固定 32 而不是 vector 无限增长：一片 AoE 打中二十个敌人
    // 时，屏幕上同时出现上百个数字既看不清也没有信息量。
    static constexpr std::size_t kMaxPopups = 32;

    float maxHealth = 100.0f;
    float health = 100.0f;
    float healthShown = 100.0f;
    float healthTrail = 100.0f;

    bool showCrosshair = true;
    int ammo = -1;            // < 0 = 不显示
    std::string hint;         // 交互提示（"[E] 开门"），空 = 不显示

    std::vector<DamagePopup> popups;

    // 血量百分比（夹到 0..1）。maxHealth <= 0 时返回 0 —— 不做除零。
    float healthRatio() const {
        if (!(maxHealth > 0.0f)) return 0.0f;
        const float r = health / maxHealth;
        return r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r);
    }

    // 设置血量（玩法/脚本调用）。同时**夹到 [0, maxHealth]** ——
    // 负血在 UI 上会画成"反向血条"，那不是"玩家很惨"，是 bug。
    void setHealth(float v) {
        if (!(maxHealth > 0.0f)) maxHealth = 100.0f;
        health = v < 0.0f ? 0.0f : (v > maxHealth ? maxHealth : v);
    }

    // 加一条伤害数字。池满时**替换最老的那条**（t 最大的）——
    // 挤掉最新的会让"刚打中的那一击"看不见，正好反了。
    void addPopup(const glm::vec3& worldPos, float amount) {
        if (popups.size() < kMaxPopups) {
            popups.push_back({worldPos, amount, 0.0f});
            return;
        }
        std::size_t oldest = 0;
        for (std::size_t i = 1; i < popups.size(); ++i)
            if (popups[i].t > popups[oldest].t) oldest = i;
        popups[oldest] = {worldPos, amount, 0.0f};
    }

    // 推进一帧。`kPopupLife` 秒的寿命；掉血的残影比显示值慢半拍。
    static constexpr float kPopupLife = 1.0f;
    static constexpr float kShownRate = 8.0f;    // 显示值追赶速度（1/s）
    static constexpr float kTrailDelay = 0.35f;  // 残影开始回落前的停顿
    static constexpr float kTrailRate = 1.6f;

    void tick(float dt) {
        if (dt < 0.0f) dt = 0.0f;

        // 伤害数字
        for (std::size_t i = 0; i < popups.size();) {
            popups[i].t += dt / kPopupLife;
            if (popups[i].t >= 1.0f) {
                popups[i] = popups.back();
                popups.pop_back();
            } else {
                ++i;
            }
        }

        // 显示值：指数追随（掉血与回血都平滑，回血也不该瞬间跳满）
        const float k = 1.0f - std::exp(-kShownRate * dt);
        healthShown += (health - healthShown) * k;
        if (std::fabs(healthShown - health) < 0.05f) healthShown = health;

        // 血量**下降** → 重置残影的停顿计时。判据用"比上一帧低"，
        // 不用"显示值落后于残影"—— 后者在连续掉血时会一直重置，
        // 残影就永远停在满血位置（看起来像血条坏了）。
        if (health < m_lastHealth - 0.01f) m_trailHold = kTrailDelay;
        m_lastHealth = health;

        // 残影：先等一拍再回落，否则它和显示值同步下降 = 白多一个数
        if (m_trailHold > 0.0f) {
            m_trailHold -= dt;
            // 停顿期间回血了：残影跟上（不能让白色条比红条还短）
            if (healthTrail < healthShown) healthTrail = healthShown;
        } else if (healthTrail > healthShown) {
            healthTrail -= (healthTrail - healthShown) *
                           (1.0f - std::exp(-kTrailRate * dt));
            if (healthTrail - healthShown < 0.05f) healthTrail = healthShown;
        } else {
            healthTrail = healthShown;
        }
    }

    void reset() {
        health = healthShown = healthTrail = maxHealth;
        m_trailHold = 0.0f;
        m_lastHealth = maxHealth;
        popups.clear();
    }

private:
    float m_trailHold = 0.0f;
    float m_lastHealth = 100.0f;
};

} // namespace runtime
