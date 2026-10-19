#include "HudSelfTest.h"

#include "Hud.h"
#include "HudModel.h"
#include "UiConfig.h"

#include <glm/glm.hpp>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace runtime {

namespace {

namespace fs = std::filesystem;

int g_pass = 0;
int g_fail = 0;

void check(const char* name, bool ok, const std::string& detail = {}) {
    std::printf("  [%s] %-32s %s\n", ok ? "PASS" : "FAIL", name,
                detail.c_str());
    if (ok) ++g_pass;
    else ++g_fail;
}

// 只按 double 格式化 —— 混着 %d 传 double 是 UB，所以统一用 %.Nf。
// ⚠ 参数个数必须**覆盖格式串**：漏传的那个是未初始化值，会打出
//   `h=-9.25e61` 这种 detail 垃圾（断言本身还可能是对的，最容易被忽略）。
std::string fmt(const char* f, double a = 0.0, double b = 0.0,
                double c = 0.0, double d = 0.0) {
    char buf[192];
    std::snprintf(buf, sizeof(buf), f, a, b, c, d);
    return buf;
}

// 以固定步长推进若干秒。用固定 dt 而不是真实帧时长：自检必须可复现。
void advance(HudModel* m, float seconds, float dt = 1.0f / 60.0f) {
    const int n = static_cast<int>(seconds / dt + 0.5f);
    for (int i = 0; i < n; ++i) m->tick(dt);
}

// ---------------------------------------------------------------- 血量数值

void testHealthNumeric() {
    std::printf("-- 血量：显示值追随 / 残影延迟 / 夹取\n");

    {
        HudModel m;
        check("hud_initial_full",
              m.healthShown == 100.0f && m.healthTrail == 100.0f &&
                  m.healthRatio() == 1.0f,
              fmt("shown=%.3f trail=%.3f", m.healthShown, m.healthTrail));
    }

    {
        // 掉血 30：显示值必须**平滑**下降（不能一帧跳到 40，否则玩家读不到
        // "我掉了多少"）；但也不能不动。
        HudModel m;
        m.setHealth(40.0f);
        float prev = m.healthShown;
        bool monotone = true;
        bool moved = false;
        for (int i = 0; i < 30; ++i) {   // 0.5s
            m.tick(1.0f / 60.0f);
            if (m.healthShown > prev + 1e-4f) monotone = false;
            if (m.healthShown < 99.0f) moved = true;
            prev = m.healthShown;
        }
        check("hud_shown_monotone_down", monotone && moved,
              fmt("shown=%.3f", m.healthShown));
        check("hud_shown_between",
              m.healthShown > 40.0f && m.healthShown < 100.0f,
              fmt("shown=%.3f", m.healthShown));
    }

    {
        // 收敛：指数追随 + 死区吸附 → 2s 后必须**精确**等于权威值。
        HudModel m;
        m.setHealth(40.0f);
        advance(&m, 2.0f);
        check("hud_shown_converge", m.healthShown == 40.0f,
              fmt("shown=%.4f", m.healthShown));
    }

    {
        // 残影：停顿期内必须**不动**（这就是"刚掉了 30"那截白色）。
        HudModel m;
        m.setHealth(40.0f);
        advance(&m, 0.2f);   // < kTrailDelay(0.35)
        check("hud_trail_holds", m.healthTrail >= 99.0f,
              fmt("trail=%.3f", m.healthTrail));
        // 4.2s 总计 → 停顿之后还有 ~3.8s 回落：60 × e^(-1.6×3.8) ≈ 0.13
        advance(&m, 4.0f);
        check("hud_trail_falls_later", std::fabs(m.healthTrail - 40.0f) < 0.5f,
              fmt("trail=%.3f", m.healthTrail));
        // 残影永远不该比红条**短**（白条比红条窄 = 画反了）
        check("hud_trail_not_below_shown",
              m.healthTrail + 1e-4f >= m.healthShown,
              fmt("trail=%.3f shown=%.3f", m.healthTrail, m.healthShown));
    }

    {
        HudModel m;
        m.setHealth(-50.0f);
        const bool lo = m.health == 0.0f && m.healthRatio() == 0.0f;
        m.setHealth(1000.0f);
        const bool hi = m.health == 100.0f && m.healthRatio() == 1.0f;
        check("hud_health_clamped", lo && hi,
              fmt("lo=%.0f hi=%.0f", lo ? 1.0 : 0.0, hi ? 1.0 : 0.0));
    }
}

// ---------------------------------------------------------------- 伤害数字

void testPopups() {
    std::printf("-- 伤害数字：寿命 / 对象池\n");

    {
        HudModel m;
        m.addPopup(glm::vec3(0.0f), 12.0f);
        const bool born = m.popups.size() == 1;
        advance(&m, 0.5f);
        const bool aliveMid = m.popups.size() == 1;
        advance(&m, 0.6f);   // 合计 1.1s > kPopupLife(1.0)
        check("hud_popup_life", born && aliveMid && m.popups.empty(),
              fmt("mid=%.0f end=%.0f", aliveMid ? 1.0 : 0.0,
                  static_cast<double>(m.popups.size())));
    }

    {
        // 池满：替换而不是无限增长，且**最新的那条一定在**。
        HudModel m;
        for (int i = 1; i <= 40; ++i)
            m.addPopup(glm::vec3(0.0f), static_cast<float>(i));
        bool newest = false;
        bool oldest = false;
        for (const auto& p : m.popups) {
            if (std::fabs(p.amount - 40.0f) < 1e-3f) newest = true;
            if (std::fabs(p.amount - 1.0f) < 1e-3f) oldest = true;
        }
        check("hud_popup_pool_capped",
              m.popups.size() == HudModel::kMaxPopups,
              fmt("size=%.0f", static_cast<double>(m.popups.size())));
        check("hud_popup_keeps_newest", newest,
              fmt("newest=%.0f", newest ? 1.0 : 0.0));
        check("hud_popup_drops_oldest", !oldest,
              fmt("oldest=%.0f", oldest ? 1.0 : 0.0));
    }
}

// ---------------------------------------------------------------- 几何

void testBarGeometry() {
    std::printf("-- 血条几何（输出像素）\n");

    Hud hud;                     // 出厂 config
    const UiConfig& cfg = hud.config();

    {
        const ui2::UiRect r = hud.healthBarRect(1920, 1080);
        const bool pos = std::fabs(r.x - cfg.barMargin) < 0.01f &&
                         std::fabs((static_cast<float>(1080) - (r.y + r.h)) -
                                   cfg.barMargin) < 0.01f;
        const bool size = std::fabs(r.w - cfg.barWidth) < 0.01f &&
                          std::fabs(r.h - cfg.barHeight) < 0.01f;
        check("hud_bar_rect_1920", pos && size,
              fmt("x=%.1f y=%.1f w=%.1f h=%.1f", r.x, r.y, r.w, r.h));
    }

    {
        // 换一个分辨率：**等比缩放**（1920×1080 是参考画布）。
        const ui2::UiRect r = hud.healthBarRect(1280, 720);
        const float s = 1280.0f / 1920.0f;
        const bool ok = std::fabs(r.w - cfg.barWidth * s) < 0.6f &&
                        std::fabs(r.h - cfg.barHeight * s) < 0.6f;
        check("hud_bar_scales", ok,
              fmt("w=%.2f expect=%.2f", r.w, cfg.barWidth * s));
    }

    {
        HudModel m;
        m.setHealth(50.0f);
        const ui2::UiRect back = hud.healthBarRect(1920, 1080);
        const ui2::UiRect fill = hud.healthFillRect(m, 1920, 1080);
        const bool ok = std::fabs(fill.w - back.w * 0.5f) < 0.01f &&
                        std::fabs(fill.x - back.x) < 0.01f;
        check("hud_bar_fill_half", ok, fmt("fill=%.2f", fill.w));
    }
}

// ---------------------------------------------------------------- 投影

void testWorldToScreen() {
    std::printf("-- 世界 → 屏幕（Y 只翻一次）\n");

    const glm::mat4 id(1.0f);

    {
        glm::vec2 s(0.0f);
        const bool ok = Hud::worldToScreen(id, glm::vec3(0.0f), 800, 600, &s);
        check("hud_w2s_center",
              ok && std::fabs(s.x - 400.0f) < 0.01f &&
                  std::fabs(s.y - 300.0f) < 0.01f,
              fmt("sx=%.1f sy=%.1f", s.x, s.y));
    }

    {
        // ⚠ 这一条守的是本轮最贵的那个 bug：**Vulkan 的 NDC y 向下**。
        //   world y=+1 在单位矩阵下 ndc.y=+1，必须落到屏幕**底部**。
        //   写成 OpenGL 约定的 `1-2y` 会让整层 UI 上下镜像。
        glm::vec2 s(0.0f);
        const bool ok = Hud::worldToScreen(id, glm::vec3(0.0f, 1.0f, 0.0f),
                                          800, 600, &s);
        check("hud_w2s_y_grows_down", ok && std::fabs(s.y - 600.0f) < 0.01f,
              fmt("sy=%.1f expect=600", s.y));
    }

    {
        // 相机背后：w<=0 必须**整体丢弃**，不能钳到边上。
        glm::vec2 s(-1.0f, -1.0f);
        const bool ok = Hud::worldToScreen(glm::mat4(0.0f), glm::vec3(0.0f),
                                          800, 600, &s);
        check("hud_w2s_behind_dropped", !ok, fmt("ok=%.0f", ok ? 1.0 : 0.0));
    }

    {
        // 屏幕外（x 方向越界）→ 不画
        glm::vec2 s(0.0f);
        const bool ok = Hud::worldToScreen(id, glm::vec3(2.0f, 0.0f, 0.0f),
                                          800, 600, &s);
        check("hud_w2s_offscreen_dropped", !ok,
              fmt("ok=%.0f sx=%.0f", ok ? 1.0 : 0.0, s.x));
    }
}

// ---------------------------------------------------------------- 配置

void testUiConfig() {
    std::printf("-- config/ui.json 读写\n");

    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "venn_hud_selftest";
    fs::remove_all(dir, ec);
    const fs::path file = dir / "ui.json";

    {
        // ① 不存在 = 静默：返回 false，调用方拿到的仍是默认值
        UiConfig c;
        const bool loaded = loadUiConfig(file.string(), &c);
        check("ui_cfg_missing_silent",
              !loaded && std::fabs(c.barWidth - 360.0f) < 0.01f,
              fmt("loaded=%.0f w=%.0f", loaded ? 1.0 : 0.0, c.barWidth));
    }

    {
        UiConfig a;
        a.barWidth = 288.0f;
        a.barHeight = 22.0f;
        a.barMargin = 40.0f;
        a.crosshair = false;
        a.crosshairLen = 14.0f;
        a.crosshairGap = 6.0f;
        const bool saved = saveUiConfig(file.string(), a);
        UiConfig b;
        const bool loaded = loadUiConfig(file.string(), &b);
        const bool same = std::fabs(b.barWidth - a.barWidth) < 0.01f &&
                          std::fabs(b.barHeight - a.barHeight) < 0.01f &&
                          std::fabs(b.barMargin - a.barMargin) < 0.01f &&
                          b.crosshair == a.crosshair &&
                          std::fabs(b.crosshairLen - a.crosshairLen) < 0.01f &&
                          std::fabs(b.crosshairGap - a.crosshairGap) < 0.01f;
        check("ui_cfg_roundtrip", saved && loaded && same,
              fmt("saved=%.0f loaded=%.0f same=%.0f", saved ? 1.0 : 0.0,
                  loaded ? 1.0 : 0.0, same ? 1.0 : 0.0));
    }

    {
        // ② 值夹到合理范围：负宽度不能变成"反向血条"；
        //    类型不对的键**只拒那一个**，其余照读。
        const fs::path bad = dir / "bad.json";
        fs::create_directories(dir, ec);
        {
            std::ofstream o(bad);
            o << "{\"hud\":{\"barWidth\":-5,\"barHeight\":99999,"
                 "\"barMargin\":\"oops\"}}\n";
        }
        UiConfig c;
        const bool loaded = loadUiConfig(bad.string(), &c);
        const bool clamped = c.barWidth >= 120.0f && c.barHeight <= 72.0f &&
                             c.barMargin == 24.0f;   // 类型错 → 保留默认
        check("ui_cfg_clamped", loaded && clamped,
              fmt("w=%.0f h=%.0f m=%.0f", c.barWidth, c.barHeight, c.barMargin));
    }

    fs::remove_all(dir, ec);
}

}  // namespace

int runHudSelfTest() {
    std::printf("== selftest: hud ==\n");
    g_pass = 0;
    g_fail = 0;

    testHealthNumeric();
    testPopups();
    testBarGeometry();
    testWorldToScreen();
    testUiConfig();

    std::printf("HUD-TOTAL pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

}  // namespace runtime
