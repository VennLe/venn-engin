#include "ui2/Ui2SelfTest.h"

#include "core/Logger.h"
#include "ui2/SdfFont.h"
#include "ui2/Ui2Settings.h"
#include "ui2/UiCanvas.h"
#include "ui2/UiFocus.h"
#include "ui2/UiLayout.h"
#include "ui2/UiWidget.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace ui2 {

namespace {

int g_pass = 0;
int g_fail = 0;

// 一行断言。tol 只用于人读；判定在外面算好（避免"浮点相等"这类假红）。
void report(const char* name, const std::string& actual,
            const std::string& expected, double tol, bool ok) {
    if (ok) ++g_pass; else ++g_fail;
    VK_LOG_INFO("UI2-TEST %s actual=%s expected=%s tol=%g result=%s",
                name, actual.c_str(), expected.c_str(), tol,
                ok ? "PASS" : "FAIL");
}

std::string f2s(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4f", v);
    return buf;
}

// ---- 缩放（反映"UI 布局除的是输出分辨率"这条契约）----
void testScale() {
    const double kTol = 1e-4;

    // 1920×1080（参考分辨率本身）→ 恰好 1.0
    // ⚠ 这条看着平凡，但它守住的是"参考分辨率改了没同步缩放公式"。
    const double s1 = computeScale(1920, 1080);
    report("scale_1080p", f2s(s1), "1.0000", kTol,
           std::fabs(s1 - 1.0) < kTol);

    // 1280×720 → 2/3（min(0.6667, 0.6667)）
    const double s2 = computeScale(1280, 720);
    report("scale_720p", f2s(s2), "0.6667", kTol,
           std::fabs(s2 - 2.0 / 3.0) < kTol);

    // 3840×2160 → 2.0
    const double s3 = computeScale(3840, 2160);
    report("scale_4k", f2s(s3), "2.0000", kTol,
           std::fabs(s3 - 2.0) < kTol);

    // 超宽屏 2560×1080 → 按**高度**钳（min 生效）：1.0 而不是 1.333
    // ⚠ 这是"等比不变形"的关键：用宽度算会让 UI 被横向拉扁。
    const double s4 = computeScale(2560, 1080);
    report("scale_ultrawide", f2s(s4), "1.0000", kTol,
           std::fabs(s4 - 1.0) < kTol);

    // 窗口最小化：outW/outH = 0 → 不能除零，返回 1.0（不是 NaN/inf）
    const double s5 = computeScale(0, 0);
    report("scale_zero_guard", f2s(s5), "1.0000", kTol,
           std::isfinite(s5) && std::fabs(s5 - 1.0) < kTol);

    // ui.scale 参与相乘：2.0 倍率 → 参考分辨率下就是 2.0
    Ui2Settings& s = settings();
    const float saved = s.scale;
    s.scale = 2.0f;
    const double s6 = computeScale(1920, 1080);
    s.scale = saved;
    report("scale_user_multiplier", f2s(s6), "2.0000", kTol,
           std::fabs(s6 - 2.0) < kTol);
}

// ---- 默认值（"默认关 = 逐像素不变"的守卫生效的前提）----
void testDefaults() {
    const Ui2Settings& s = settings();
    // ⚠ ui.enabled 默认 **true** 是对的：true 但没有 UI 数据 = 零绘制。
    //   真正要守住的是 hudInEditor=false —— 它一变，编辑器截图全漂。
    report("default_enabled", s.enabled ? "1" : "0", "1", 0, s.enabled);
    report("default_hud_in_editor", s.hudInEditor ? "1" : "0", "0", 0,
           !s.hudInEditor);
    report("default_debug_bounds", s.debugBounds ? "1" : "0", "0", 0,
           !s.debugBounds);
    report("default_font_size", f2s(s.fontSize), "16.0000", 1e-4,
           std::fabs(s.fontSize - 16.0f) < 1e-4f);
    report("default_scale", f2s(s.scale), "1.0000", 1e-4,
           std::fabs(s.scale - 1.0f) < 1e-4f);
}

// ---- 字体（U1）：图集数据自洽 ----
// ⚠ 这些断言读的是**真产物**（assets/ui/fonts/*.json），不是常量。
//   所以只钉"结构性质"（有汉字、adv>0、uv 在图集内），不钉具体字形数 ——
//   钉死数字就等于每次重生成图集都要改测试，最后一定被改成永远通过。
void testFont(SdfFont& f) {
    std::string err;
    const std::string path = resolveUiAsset("fonts/noto_sc_sdf.json");
    const bool ok = loadSdfFontJson(path, f, err);
    report("font_json_load", ok ? "1" : "0", "1", 0, ok);
    if (!ok) {
        VK_LOG_WARN("UI2-TEST font path=%s err=%s", path.c_str(), err.c_str());
        return;
    }
    report("font_glyph_count", std::to_string(f.glyphs.size()), ">=700", 0,
           f.glyphs.size() >= 700);
    // 汉字必须真的有 —— 否则"图集加载成功"只是个空壳。
    report("font_has_cjk", f.find(0x4E00) ? "1" : "0", "1", 0,
           f.find(0x4E00) != nullptr);
    report("font_has_ascii", f.find('A') ? "1" : "0", "1", 0,
           f.find('A') != nullptr);
    report("font_metrics", f2s(f.ascent), ">0", 0,
           f.ascent > 0.0f && f.descent > 0.0f && f.spread > 0.0f &&
               f.size > 0.0f);
    // uv 必须落在图集内（改了 packing 却没重写 json → 全部错位，而且画面
    // 看起来"有字"，只是字和内容对不上 —— 最难查的一类）。
    int bad = 0;
    for (const auto& it : f.glyphs) {
        const SdfGlyph& g = it.second;
        if (g.u0 < 0.0f || g.v0 < 0.0f || g.u1 > 1.0001f || g.v1 > 1.0001f) ++bad;
        if (g.adv <= 0.0f) ++bad;
    }
    report("font_uv_in_range", std::to_string(bad), "0", 0, bad == 0);
    // 缺字回退：任意未知码点都应能退到 '?'
    report("font_fallback", f.findOrFallback(0x10FFFF) ? "1" : "0", "1", 0,
           f.findOrFallback(0x10FFFF) != nullptr);
}

// 第一个顶点的 SDF 平滑宽度（负值 = 没有顶点）。
double firstHx(const UiCanvas& c) {
    return c.vertices().empty() ? -1.0 : c.vertices()[0].hx;
}

// ---- 文本排版（U3 的地基，但现在就能测：全是 CPU）----
void testTextLayout(const SdfFont& f) {
    if (!f.valid()) {
        report("text_skip", "0", "1", 0, false);
        return;
    }
    UiCanvas c;

    // 逐字 quad：2 字符 = 12 顶点（每 quad 2 个三角形 × 3）
    c.begin();
    c.text(f, 100.0f, 200.0f, "AB", 0xFFFFFFFFu, f.size);
    report("text_verts_2chars", std::to_string(c.vertices().size()), "12", 0,
           c.vertices().size() == 12);

    // 宽度 = Σadv（scale=1 时就是 adv 之和）
    const SdfGlyph* a = f.find('A');
    const SdfGlyph* b = f.find('B');
    const double expect = static_cast<double>(a ? a->adv : 0.0f) +
                          static_cast<double>(b ? b->adv : 0.0f);
    const double got = c.measure(f, "AB", f.size);
    report("text_measure", f2s(got), f2s(expect), 0.01,
           std::fabs(got - expect) < 0.01);

    // 换行：顶点翻倍，第二行基线正好下移 lineHeight
    c.begin();
    c.text(f, 0.0f, 0.0f, "A\nB", 0xFFFFFFFFu, f.size);
    const bool two = c.vertices().size() == 12;
    const double dy = two ? c.vertices()[6].y - c.vertices()[0].y : -1.0;
    const double lineH = static_cast<double>(f.lineHeight());
    report("text_newline_verts", std::to_string(c.vertices().size()), "12", 0,
           two);
    report("text_newline_dy", f2s(dy), f2s(lineH), 0.01,
           two && std::fabs(dy - lineH) < 0.01);

    // 笔位推进必须按 **adv**，不能按格子宽（格子含 pad）：否则 48 号字
    // 每字多 16px，整句字距散开。判据：两字左边缘之差 == adv。
    c.begin();
    c.text(f, 0.0f, 0.0f, "AB", 0xFFFFFFFFu, f.size);
    const double step =
        c.vertices().size() == 12 ? c.vertices()[6].x - c.vertices()[0].x : -1.0;
    report("text_advance_uses_adv", f2s(step), f2s(a ? a->adv : 0.0), 0.01,
           a && std::fabs(step - a->adv) < 0.01);

    // SDF 的平滑宽度与**屏幕字号**成反比：字越小过渡带越宽。
    // 这是 SDF 的核心性质（放大不糊），不是"随便调个常数"。
    c.begin();
    c.text(f, 0.0f, 0.0f, "A", 0xFFFFFFFFu, f.size * 0.5f);
    const double wSmall = firstHx(c);
    c.begin();
    c.text(f, 0.0f, 0.0f, "A", 0xFFFFFFFFu, f.size * 2.0f);
    const double wBig = firstHx(c);
    report("sdf_smooth_shrinks", f2s(wBig), "<" + f2s(wSmall), 0,
           wSmall > wBig && wBig > 0.0);
}

std::string hex32(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08X", v);
    return buf;
}

// ---- 锚点布局（U4）----
// 判据的核心是"**比例**"，不是"某个具体像素值"：同一份设计尺寸在两种
// 分辨率下，边距之比必须等于缩放之比。这才是"分辨率无关"的定义。
void testLayout() {
    const double kTol = 1e-3;
    UiPlacement p;
    p.anchor = UiAnchor::TopLeft;
    p.x = 100.0f;
    p.y = 40.0f;
    p.w = 200.0f;
    p.h = 50.0f;

    const UiRect a = placeRect(p, 1920, 1080);
    report("layout_ref_exact", f2s(a.x), "100.0000", kTol,
           std::fabs(a.x - 100.0) < kTol && std::fabs(a.y - 40.0) < kTol);

    // 1280×720 → scale 2/3 → 100 变 66.667
    const UiRect b = placeRect(p, 1280, 720);
    const double want = 100.0 * 2.0 / 3.0;
    report("layout_720_margin", f2s(b.x), f2s(want), 0.01,
           std::fabs(b.x - want) < 0.01);

    // 边距之比 == 缩放之比（换分辨率的判据）
    report("layout_margin_ratio", f2s(b.x / a.x), "0.6667", 1e-3,
           std::fabs(b.x / a.x - 2.0 / 3.0) < 1e-3);

    // 右锚：贴右边缘（1920 与 2560 各一次 —— 超宽屏下要真的贴到 2560）
    UiPlacement r;
    r.anchor = UiAnchor::TopRight;
    r.w = 200.0f;
    r.h = 50.0f;
    const UiRect rr = placeRect(r, 1920, 1080);
    report("layout_right_edge", f2s(rr.right()), "1920.0000", kTol,
           std::fabs(rr.right() - 1920.0) < kTol);
    const UiRect rr2 = placeRect(r, 2560, 1080);
    report("layout_ultrawide_right", f2s(rr2.right()), "2560.0000", kTol,
           std::fabs(rr2.right() - 2560.0) < kTol);

    // 中心锚：中心恰好等于画布中心
    UiPlacement c;
    c.anchor = UiAnchor::Center;
    c.w = 300.0f;
    c.h = 100.0f;
    const UiRect cr = placeRect(c, 1920, 1080);
    report("layout_center", f2s(cr.cx()), "960.0000", kTol,
           std::fabs(cr.cx() - 960.0) < kTol && std::fabs(cr.cy() - 540.0) < kTol);

    // 窗口最小化：空矩形，且 contains 永远 false
    const UiRect z = placeRect(p, 0, 0);
    report("layout_zero_guard", f2s(z.w), "0", 0,
           z.w == 0.0f && !z.contains(0.0f, 0.0f));
}

// ---- 控件（U5）：命中测试 + 三态配色 ----
void testWidget(const SdfFont& f) {
    UiButton btn;
    btn.label = "OK";
    btn.frame.anchor = UiAnchor::TopLeft;
    btn.frame.x = 100.0f;
    btn.frame.y = 100.0f;
    btn.frame.w = 200.0f;
    btn.frame.h = 60.0f;

    report("hit_inside", hitTest(btn, 150, 130, 1920, 1080) ? "1" : "0", "1",
           0, hitTest(btn, 150, 130, 1920, 1080));
    report("hit_outside", hitTest(btn, 99, 130, 1920, 1080) ? "0" : "1", "1",
           0, !hitTest(btn, 99, 130, 1920, 1080));
    // 右下边界是**半开**的：贴边不算命中（两个按钮贴在一起时不会双命中）
    report("hit_right_exclusive",
           hitTest(btn, 300, 130, 1920, 1080) ? "0" : "1", "1", 0,
           !hitTest(btn, 300, 130, 1920, 1080));

    // 禁用必须不命中（否则"点了没反应"会伪装成"点了但看不出来"）
    btn.enabled = false;
    report("hit_disabled", hitTest(btn, 150, 130, 1920, 1080) ? "0" : "1",
           "1", 0, !hitTest(btn, 150, 130, 1920, 1080));
    btn.enabled = true;

    // 三态配色：直接看**顶点里的颜色**（不需要 GPU 就能断言"按下会变暗"）
    auto firstColor = [&](bool held, bool hovered) {
        btn.held = held;
        btn.hovered = hovered;
        UiCanvas c;
        c.begin();
        drawButton(c, btn, 1920, 1080, nullptr);
        return c.vertices().empty() ? 0u : c.vertices()[0].color;
    };
    const uint32_t cIdle = firstColor(false, false);
    const uint32_t cHover = firstColor(false, true);
    const uint32_t cPress = firstColor(true, true);
    report("button_idle_color", hex32(cIdle), hex32(btn.style.idle), 0,
           cIdle == btn.style.idle);
    report("button_hover_color", hex32(cHover), hex32(btn.style.hover), 0,
           cHover == btn.style.hover);
    // 按住必须压过悬停（否则"按下"和"悬停"看起来一样）
    report("button_press_beats_hover", hex32(cPress), hex32(btn.style.press),
           0, cPress == btn.style.press);

    // 有字体时：背景 6 顶点 + 每个字符 6 顶点
    if (f.valid()) {
        btn.held = btn.hovered = false;
        UiCanvas c;
        c.begin();
        drawButton(c, btn, 1920, 1080, &f);
        const size_t want = 6 + 6 * 2;   // "OK" = 2 个字符
        report("button_with_label_verts", std::to_string(c.vertices().size()),
               std::to_string(want), 0, c.vertices().size() == want);
    }
}

// ---- 焦点栈（U6）----
//
// 全部是**纯数值**断言：给定的矩形 → 按方向键 → 焦点 id 是什么。
// 之所以值得单独测：导航"看起来对"太容易了（三按钮垂直列里按数组顺序
// 也能对），必须用**非数组顺序的布局**才测得出几何导航真的在起作用。
void testFocus() {
    auto mk = [](const char* id, float x, float y, float w, float h,
                 bool en = true) {
        UiFocusItem it;
        it.id = id;
        it.rect.x = x; it.rect.y = y; it.rect.w = w; it.rect.h = h;
        it.enabled = en;
        return it;
    };

    // ---- A：垂直三按钮，数组顺序**故意**与视觉顺序不同 ----
    // 视觉（上→下）：resume / settings / quit
    // 数组      ：[quit, resume, settings]
    // 若实现按数组顺序导航，↓ 会 resume→settings→quit 之外的某个顺序出错。
    {
        UiFocusStack fs;
        fs.beginFrame({mk("quit", 700, 500, 300, 60),
                       mk("resume", 700, 300, 300, 60),
                       mk("settings", 700, 400, 300, 60)});

        const bool okFirst = fs.focusFirst();
        report("focus_first_resume", fs.focusId(), "resume", 0,
               okFirst && fs.focusId() == "resume");

        fs.move(0, 1);
        report("focus_down1_settings", fs.focusId(), "settings", 0,
               fs.focusId() == "settings");

        fs.move(0, 1);
        report("focus_down2_quit", fs.focusId(), "quit", 0,
               fs.focusId() == "quit");

        // 到底再按 ↓ → 回到最上面（循环，路线图 U6 的验收条款）
        fs.move(0, 1);
        report("focus_down_wraps_resume", fs.focusId(), "resume", 0,
               fs.focusId() == "resume");

        // 最上面再按 ↑ → 回到底部
        fs.move(0, -1);
        report("focus_up_wraps_quit", fs.focusId(), "quit", 0,
               fs.focusId() == "quit");

        // 左右键在这个布局里**不应该**跳到别的按钮（侧向惩罚生效：
        // 三个按钮中心 x 相同，右方向没有候选 → 回绕到最左……
        // 这里只有一列，回绕会转到另一项，所以只断言"没有越界/没有崩"）
        report("focus_index_in_range",
               std::to_string(fs.focusIndex()),
               "0..2", 0,
               (fs.focusIndex() == 0 || fs.focusIndex() == 1 ||
                fs.focusIndex() == 2));
    }

    // ---- B：左右导航（两列同排）----
    {
        UiFocusStack fs;
        fs.beginFrame({mk("left", 100, 300, 200, 60),
                       mk("right", 400, 300, 200, 60)});
        fs.focusFirst();
        fs.move(1, 0);
        report("focus_right", fs.focusId(), "right", 0,
               fs.focusId() == "right");
        fs.move(-1, 0);
        report("focus_left", fs.focusId(), "left", 0,
               fs.focusId() == "left");
    }

    // ---- C：Tab / Shift+Tab 走**数组顺序**（移动端/键盘可达性的通行做法）----
    {
        UiFocusStack fs;
        fs.beginFrame({mk("a", 700, 300, 300, 60),
                       mk("b", 700, 400, 300, 60),
                       mk("c", 700, 500, 300, 60)});
        fs.setFocus("b");
        fs.cycle(false);
        report("focus_tab_next", fs.focusId(), "c", 0, fs.focusId() == "c");
        fs.cycle(false);
        report("focus_tab_wraps", fs.focusId(), "a", 0, fs.focusId() == "a");
        fs.cycle(true);
        report("focus_shift_tab_prev", fs.focusId(), "c", 0,
               fs.focusId() == "c");
    }

    // ---- D：禁用项被跳过 ----
    {
        UiFocusStack fs;
        fs.beginFrame({mk("a", 700, 300, 300, 60),
                       mk("disabled", 700, 400, 300, 60, false),
                       mk("c", 700, 500, 300, 60)});
        fs.setFocus("a");
        fs.move(0, 1);
        report("focus_skips_disabled", fs.focusId(), "c", 0,
               fs.focusId() == "c");
        report("focus_disabled_not_focusable", fs.setFocus("disabled") ? "1" : "0",
               "0", 0, !fs.setFocus("disabled"));
    }

    // ---- E：消费语义（consumed）----
    {
        UiFocusStack empty;
        empty.beginFrame({});
        UiNav nav;
        nav.moveY = 1;
        // 没有可聚焦项 = 没有菜单 → **一点输入都不吃**（否则角色在
        // 正常游戏里按方向键会没反应）
        report("consume_empty_false", empty.route(nav, nullptr) ? "1" : "0",
               "0", 0, !empty.route(nav, nullptr));

        UiFocusStack fs;
        fs.beginFrame({mk("resume", 700, 300, 300, 60)});
        report("consume_nav_true", fs.route(nav, nullptr) ? "1" : "0", "1", 0,
               fs.route(nav, nullptr));

        std::string act;
        UiNav enter;
        enter.activate = true;
        const bool c = fs.route(enter, &act);
        report("consume_activate_id", act, "resume", 0,
               c && act == "resume");
    }

    // ---- F：锚点口径（id 里不能有空格）----
    {
        UiFocusStack fs;
        fs.beginFrame({mk("resume", 700, 300, 300, 60)});
        fs.focusFirst();
        const std::string& id = fs.focusId();
        report("focus_id_no_space",
               id.find(' ') == std::string::npos ? "1" : "0", "1", 0,
               id.find(' ') == std::string::npos);
    }
}

}  // namespace

int runUi2SelfTest() {
    VK_LOG_INFO("=== UI2 self-test (no window / no Vulkan) ===");
    g_pass = 0;
    g_fail = 0;

    testScale();
    testDefaults();

    // 字体与排版要拿真图集数据测 —— 但**不建 Vulkan**（SdfFont 是纯 CPU）。
    SdfFont font;
    testFont(font);
    testTextLayout(font);
    testLayout();
    testWidget(font);
    testFocus();

    VK_LOG_INFO("UI2-TOTAL pass=%d fail=%d", g_pass, g_fail);
    return g_fail;
}

}  // namespace ui2
