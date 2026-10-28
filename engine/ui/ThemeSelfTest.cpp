// ============================================================
// engine/ui/ThemeSelfTest.cpp —— 主题系统验收（`Game.exe --selftest theme`）
//
// 全部是**纯数值**断言：不建窗口、不要 GPU、不碰 ImGui 上下文。
// （`applyTheme` 在 `GImGui == nullptr` 时只更新数据，见它的注释。）
// ============================================================

#include "ui/ThemeSelfTest.h"

#include "core/Logger.h"
#include "ui/ImGuiTheme.h"

#include <cmath>
#include <string>
#include <vector>

namespace ui {

namespace {

int g_pass = 0;
int g_fail = 0;

// ⚠ detail 里不能有空格（全仓自检的硬约定：脚本用 `detail=(\S*)` 解析）。
void report(const char* name, double actual, double expected, double tol) {
    const bool ok = std::fabs(actual - expected) <= tol;
    VK_LOG_INFO("THEME-TEST %s actual=%.6f expected=%.6f tol=%.6f result=%s",
                name, actual, expected, tol, ok ? "PASS" : "FAIL");
    if (ok) ++g_pass;
    else ++g_fail;
}

void reportFlag(const char* name, bool ok, const std::string& detail = {}) {
    VK_LOG_INFO("THEME-TEST %s actual=%d expected=1 tol=0 detail=%s result=%s",
                name, ok ? 1 : 0, detail.c_str(), ok ? "PASS" : "FAIL");
    if (ok) ++g_pass;
    else ++g_fail;
}

// 亮度（Rec.601，够用）。用来判"深色主题是不是真深"。
double luma(const ImVec4& c) {
    return 0.299 * c.x + 0.587 * c.y + 0.114 * c.z;
}

// ============================================================
// A 下标稳定：0~4 的关键色逐字段等于改造前的值
// ============================================================
// ⚠ 这是**最重要的一组**。落盘存的是下标，往 ThemeId 中间插一套主题会
//   静默换掉所有老配置的主题；而"数值漂了"更隐蔽 —— 老用户不会去比色，
//   只会觉得"界面好像不太一样了"。
//
// 取的锚点：VennDark 的 windowBg / accent / button + Daylight 的 windowBg。
// 四个数就够锁住"0~4 这一段没被动过"，再多也只是重复。
void groupStableIndices() {
    ThemeTokens tk;
    ThemeMetrics mt;

    themeTokensAt(0, tk, mt);
    report("venn_window_bg", tk.panelBg.x, 0.098f, 1e-6);
    report("venn_accent_r", tk.accent.x, 0.16f, 1e-6);
    report("venn_accent_g", tk.accent.y, 0.53f, 1e-6);
    report("venn_accent_b", tk.accent.z, 0.94f, 1e-6);

    themeTokensAt(4, tk, mt);   // Daylight
    report("daylight_is_light", tk.isLight ? 1.0 : 0.0, 1.0, 0.0);
    report("daylight_text_dim", tk.textDim.w, 1.0, 1e-6);
    // 浅色主题的 panelBg 必须比深色主题的亮（方向别写反）
    ThemeTokens dark;
    ThemeMetrics dm;
    themeTokensAt(0, dark, dm);
    reportFlag("light_brighter_than_dark",
               luma(tk.panelBg) > luma(dark.panelBg) + 0.3f);
}

// ============================================================
// B 每套都不同：两两 (windowBg, accent) 不重复
// ============================================================
// 抓的是"新加一套主题忘了给 accent / 抄了上一套" —— 那种情况下下拉里有
// 10 个名字、实际只有 7 种观感，用户会以为"换主题没生效"。
void groupDistinct() {
    const int n = themeCount();
    report("theme_count", static_cast<double>(n), 10.0, 0.0);

    int dup = 0;
    for (int i = 0; i < n; ++i) {
        ImVec4 bgI, acI;
        themeSwatch(i, bgI, acI);
        for (int j = i + 1; j < n; ++j) {
            ImVec4 bgJ, acJ;
            themeSwatch(j, bgJ, acJ);
            const bool sameBg = std::fabs(bgI.x - bgJ.x) < 1e-4f &&
                                std::fabs(bgI.y - bgJ.y) < 1e-4f &&
                                std::fabs(bgI.z - bgJ.z) < 1e-4f;
            const bool sameAc = std::fabs(acI.x - acJ.x) < 1e-4f &&
                                std::fabs(acI.y - acJ.y) < 1e-4f &&
                                std::fabs(acI.z - acJ.z) < 1e-4f;
            if (sameBg && sameAc) ++dup;
        }
    }
    report("themes_all_distinct", static_cast<double>(dup), 0.0, 0.0);

    // 名字不许重复（下拉里两个 "Sakura" 谁也分不清）
    int nameDup = 0;
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            if (std::string(themeName(i)) == std::string(themeName(j))) ++nameDup;
    report("theme_names_distinct", static_cast<double>(nameDup), 0.0, 0.0);
}

// ============================================================
// C 明暗自洽：isLight 与亮度方向一致；动漫深色档确实更暗
// ============================================================
void groupBrightness() {
    const int n = themeCount();
    int mismatched = 0;
    int animeDark = 0;
    int tooBrightAnime = 0;
    for (int i = 0; i < n; ++i) {
        ThemeTokens tk;
        ThemeMetrics mt;
        themeTokensAt(i, tk, mt);
        const double l = luma(tk.panelBg);
        // 浅色主题：底必须**比文字亮**；深色主题：底必须**比文字暗**。
        // 判反了就是"浅色主题下白字压白底"，而截图里一眼看不出来
        // （设置页那一片正好是空的）。
        //
        // ⚠ 这条断言第一版把方向写反了（深色判成 `l <= textL` 算错），
        //   于是 10 套**全部**报 FAIL —— 而实现是对的。教训还是那条：
        //   FAIL 先怀疑「期望」，别去改实现。
        const double textL = luma(tk.textDim);
        if (tk.isLight && l < textL) ++mismatched;
        if (!tk.isLight && l > textL) ++mismatched;

        if (tk.isAnime && !tk.isLight) {
            ++animeDark;
            // 动漫深色档压暗过（darkenThemeColors），panelBg 应该明显低于
            // 工程档那批（0.098 ~ 0.176）。0.06 是"确实压过"的分界线。
            if (l > 0.06) ++tooBrightAnime;
        }
    }
    report("lightflag_matches_luma", static_cast<double>(mismatched), 0.0, 0.0);
    report("anime_dark_count", static_cast<double>(animeDark), 4.0, 0.0);
    report("anime_dark_is_dark", static_cast<double>(tooBrightAnime), 0.0, 0.0);
}

// ============================================================
// D 度量：动漫档圆角 >= 10 且无硬边框；工程档圆角 >= 8
// ============================================================
void groupMetrics() {
    const int n = themeCount();
    int badAnime = 0;
    int badEng = 0;
    int animeCount = 0;
    for (int i = 0; i < n; ++i) {
        ThemeTokens tk;
        ThemeMetrics mt;
        themeTokensAt(i, tk, mt);
        if (tk.isAnime) {
            ++animeCount;
            if (mt.windowRounding < 10.0f || mt.childRounding < 10.0f) ++badAnime;
            if (mt.windowBorder > 0.5f) ++badAnime;   // 有硬边就不软
            if (tk.radiusLg < 10.0f) ++badAnime;
        } else {
            if (mt.windowRounding < 8.0f) ++badEng;
        }
    }
    report("anime_metrics_ok", static_cast<double>(badAnime), 0.0, 0.0);
    report("eng_metrics_ok", static_cast<double>(badEng), 0.0, 0.0);
    report("anime_theme_count", static_cast<double>(animeCount), 5.0, 0.0);

    // 窗口圆角与 tokens.radiusLg 必须是同一个数（两处漂了就会出现
    // "token 说 12、实际画 8"，而只有对比两种取值时才发现）
    ThemeTokens tk;
    ThemeMetrics mt;
    themeTokensAt(static_cast<int>(ThemeId::SakuraNight), tk, mt);
    report("radius_token_matches_style", mt.windowRounding, tk.radiusLg, 1e-4);
}

// ============================================================
// E 应用：越界被夹、幂等、tokens() 跟着变
// ============================================================
void groupApply() {
    const int saved = currentThemeIndex();

    applyTheme(-5);
    report("clamp_low", static_cast<double>(currentThemeIndex()), 0.0, 0.0);
    applyTheme(999);
    report("clamp_high", static_cast<double>(currentThemeIndex()),
           static_cast<double>(themeCount() - 1), 0.0);

    // 出厂默认必须是动漫档（用户要的是"打开就是二次元"）。
    // ⚠ 必须先 applyTheme(kDefaultThemeIndex) 再读 tokens()：
    //   上面两条 clamp 已经把当前主题改成 0 / 9 了，直接读 tokens() 验的是
    //   "最后夹到的那个下标"而不是"默认值"——这种断言永远是绿的，等于没有。
    applyTheme(kDefaultThemeIndex);
    reportFlag("default_is_anime", tokens().isAnime, themeName(kDefaultThemeIndex));

    // 幂等：连按两次同一个下标，tokens 逐字段相同
    applyTheme(static_cast<int>(ThemeId::SakuraNight));
    const ThemeTokens a = tokens();
    applyTheme(static_cast<int>(ThemeId::SakuraNight));
    const ThemeTokens b = tokens();
    reportFlag("apply_idempotent",
               a.accent.x == b.accent.x && a.panelBg.z == b.panelBg.z &&
                   a.cardBg.y == b.cardBg.y);

    // tokens() 必须**真的跟着下标变**（不是永远返回同一份）
    applyTheme(static_cast<int>(ThemeId::VennDark));
    const ThemeTokens v = tokens();
    reportFlag("tokens_follow_index", v.accent.x != b.accent.x);

    // 恢复原状，别把后面跑的用例带偏
    applyTheme(saved);
}

} // namespace

int runThemeSelfTest() {
    g_pass = 0;
    g_fail = 0;

    VK_LOG_INFO("== selftest: theme (2026-10-27 anime restyle) ==");
    groupStableIndices();
    groupDistinct();
    groupBrightness();
    groupMetrics();
    groupApply();

    VK_LOG_INFO("THEME-TOTAL pass=%d fail=%d", g_pass, g_fail);
    return g_fail;
}

} // namespace ui
