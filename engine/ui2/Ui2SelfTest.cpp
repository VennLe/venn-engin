#include "ui2/Ui2SelfTest.h"

#include "core/Logger.h"
#include "ui2/SdfFont.h"
#include "ui2/Ui2Settings.h"
#include "ui2/UiAction.h"
#include "ui2/UiCanvas.h"
#include "ui2/UiDocInput.h"
#include "ui2/UiDocument.h"
#include "ui2/UiFocus.h"
#include "ui2/UiLayout.h"
#include "ui2/UiWidget.h"

#include <algorithm>
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

// ---- UI 文档（#312）：解析 / 往返 / 锚点 / 绑定 / 绘制 ----
//
// 这一组的价值在于"界面长什么样"第一次变成**可断言的数据**：矩形从哪来、
// 绑定取到几、存回去有没有变样，全都不用开窗口。刻意**不读磁盘**（JSON
// 用字面量）—— 资产改了不该让自检红；资产本身由 `verify_ui_editor.py` 管。
void testDocument(const SdfFont& f) {
    // 一份"最小但每种控件都出现一次"的文档：面板 → 两个子控件，
    // 外加一个顶层文本。层级是刻意做的 —— 子控件锚点是相对**父矩形**的，
    // 那正是最容易写成"相对屏幕"的一段。
    static const char* kDoc = R"JSON({
  "version": 1,
  "name": "selftest",
  "canvas": [1920, 1080],
  "//": "self-test fixture",
  "widgets": [
    {
      "id": "hud",
      "type": "nine",
      "anchor": "bottom-left",
      "rect": [24, 24, 320, 64],
      "color": "#101319FF",
      "color2": "#1B2028FF",
      "radius": 6,
      "inset": 2,
      "children": [
        {
          "id": "hp",
          "type": "progress",
          "rect": [8, 8, 260, 16],
          "color": "#D93A3AFF",
          "color2": "#00000096",
          "radius": 8,
          "bind": "health",
          "bindMax": "maxHealth"
        },
        {
          "id": "hpnum",
          "type": "text",
          "rect": [8, 30, 260, 24],
          "text": "HP {health}/{maxHealth}",
          "size": 16,
          "align": "right"
        }
      ]
    },
    { "id": "title", "type": "text", "anchor": "top-center", "rect": [0, 40, 600, 40],
      "text": "Chapter 1", "size": 28, "align": "center" }
  ]
})JSON";

    UiDocument d;
    const bool ok = parseUiDocument(kDoc, "selftest.ui.json", d);
    report("uidoc_parse_ok", ok ? "1" : "0", "1", 0, ok);
    if (!ok) {
        VK_LOG_WARN("UI2-TEST uidoc error=%s", d.error.c_str());
        return;
    }
    report("uidoc_parse_widgets", std::to_string(countWidgets(d.widgets)), "4",
           0, countWidgets(d.widgets) == 4);
    report("uidoc_parse_warn_free", std::to_string(d.warnings.size()), "0", 0,
           d.warnings.empty());
    report("uidoc_parse_comment", d.comment, "self-test fixture", 0,
           d.comment == "self-test fixture");

    // ---- 往返：`toJson → parse → toJson` 必须**逐字节**相同 ----
    // 这一条是"编辑器按 Save 不丢东西"的最硬判据。只比"字段还在"是不够的
    // ——漏写一个字段、把 `2.0f` 写成 `2`、把颜色大小写改了，都要红。
    const std::string j1 = toJson(d);
    UiDocument d2;
    const bool ok2 = parseUiDocument(j1, "roundtrip", d2);
    const std::string j2 = ok2 ? toJson(d2) : std::string("<parse failed>");
    report("uidoc_roundtrip_bytes", std::to_string(j2.size()) + "B",
           std::to_string(j1.size()) + "B", 0, ok2 && j1 == j2);

    // ---- 锚点：右下角 320×64，距边 24 ----
    {
        const UiDocument& doc = d;
        UiBindings b;
        const auto placed = placeDocument(doc, b, 1920, 1080);
        report("uidoc_place_count", std::to_string(placed.size()), "4", 0,
               placed.size() == 4);
        if (placed.size() == 4) {
            // 深度优先：hud → hp → hpnum → title
            report("uidoc_place_order", placed[0].id + ">" + placed[1].id + ">" +
                                            placed[2].id + ">" + placed[3].id,
                   "hud>hp>hpnum>title", 0,
                   placed[0].id == "hud" && placed[1].id == "hp" &&
                       placed[2].id == "hpnum" && placed[3].id == "title");
            const UiRect& hud = placed[0].rect;
            report("uidoc_anchor_bottom_left_x", f2s(hud.x), "24.0000", 1e-3,
                   std::fabs(hud.x - 24.0f) < 1e-3f);
            report("uidoc_anchor_bottom_left_y", f2s(hud.y), "992.0000", 1e-3,
                   std::fabs(hud.y - (1080.0f - 24.0f - 64.0f)) < 1e-3f);

            // 子控件在父矩形里：x=8 → hud.x + 8
            const UiRect& hp = placed[1].rect;
            report("uidoc_child_in_parent_x", f2s(hp.x), f2s(hud.x + 8.0f),
                   1e-3, std::fabs(hp.x - (hud.x + 8.0f)) < 1e-3f);
            report("uidoc_child_in_parent_w", f2s(hp.w), "260.0000", 1e-3,
                   std::fabs(hp.w - 260.0f) < 1e-3f);

            // 顶层居中：600 宽在 1920 里 → x = (1920-600)/2 = 660
            const UiRect& title = placed[3].rect;
            report("uidoc_anchor_top_center_x", f2s(title.x), "660.0000", 1e-3,
                   std::fabs(title.x - 660.0f) < 1e-3f);
        }
    }

    // ---- 分辨率无关：1280×720 下"占屏比例"必须与 1920×1080 一致 ----
    {
        UiBindings b;
        const auto a = placeDocument(d, b, 1920, 1080);
        const auto c = placeDocument(d, b, 1280, 720);
        bool same = (a.size() == c.size()) && !a.empty();
        double worst = 0.0;
        for (std::size_t i = 0; same && i < a.size(); ++i) {
            const double rx = a[i].rect.x / 1920.0;
            const double cx = c[i].rect.x / 1280.0;
            const double rw = a[i].rect.w / 1920.0;
            const double cw = c[i].rect.w / 1280.0;
            worst = std::max(worst, std::max(std::fabs(rx - cx),
                                             std::fabs(rw - cw)));
        }
        report("uidoc_scale_independent", f2s(worst), "<0.002", 2e-3,
               same && worst < 2e-3);
    }

    // ---- 绑定：文本替换 + 进度条几何 ----
    {
        UiBindings b;
        b.setNumber("health", 70.0f);
        b.setNumber("maxHealth", 100.0f);
        report("uidoc_bind_text",
               formatBound("HP {health}/{maxHealth}", b), "HP 70/100", 0,
               formatBound("HP {health}/{maxHealth}", b) == "HP 70/100");
        {
            // 整数写整数、非整数留一位小数 —— 血条旁边出现 "70.0 / 100" 是
            // 二次元 UI 里最碍眼的一种瑕疵。
            UiBindings t;
            t.setNumber("a", 70.0f);
            t.setNumber("c", 70.5f);
            const std::string got = formatBound("{a} {c}", t);
            report("uidoc_bind_int_vs_float", got, "70 70.5", 0,
                   got == "70 70.5");
        }
        // 拼错的 key **原样保留** —— 静默变空会让作者以为绑定没生效
        report("uidoc_bind_unknown_kept", formatBound("[{maxHelth}]", b),
               "[{maxHelth}]", 0,
               formatBound("[{maxHelth}]", b) == "[{maxHelth}]");

        const auto placed = placeDocument(d, b, 1920, 1080);
        const float wantW = (260.0f - 0.0f) * 0.7f;  // hp: inset 未设
        report("uidoc_bind_progress_fill",
               placed.size() > 1 ? f2s(placed[1].fill.w) : "n/a",
               f2s(wantW), 1e-2,
               placed.size() > 1 && std::fabs(placed[1].fill.w - wantW) < 1e-2f);

        // 没绑定时退回静值（value），且 maxHealth=0 不能除零
        UiBindings none;
        const auto p2 = placeDocument(d, none, 1920, 1080);
        report("uidoc_bind_missing_zero",
               p2.size() > 1 ? f2s(p2[1].fill.w) : "n/a", "0.0000", 1e-3,
               p2.size() > 1 && std::fabs(p2[1].fill.w) < 1e-3f);
    }

    // ---- 绘制：图元数与"不可见整棵子树跳过" ----
    {
        UiBindings b;
        b.setNumber("health", 70.0f);
        b.setNumber("maxHealth", 100.0f);
        UiCanvas c;
        c.begin();
        const UiDocDrawStats st = drawDocument(c, d, b, 1920, 1080, &f);
        report("uidoc_draw_widgets", std::to_string(st.widgets), "4", 0,
               st.widgets == 4);
        // 4 个控件 = 4 个矩形图元（nine 外框+内芯、progress 槽+填充）+
        // 两段文字。⚠ 字形是**逐字** 6 顶点的，所以只断言"矩形图元那 24
        // 个顶点一定在"，再单独断言文字段数 —— 把字数写死等于字体图集
        // 一改就红，而那条断言什么也没证明。
        const std::size_t base = 24;   // 6 顶点 × 4 个矩形
        report("uidoc_draw_rect_verts",
               std::to_string(c.vertices().size()), ">=24", 0,
               c.vertices().size() >= base &&
                   (f.valid() ? c.vertices().size() > base
                              : c.vertices().size() == base));
        report("uidoc_draw_texts", std::to_string(st.texts),
               f.valid() ? "2" : "0", 0, st.texts == (f.valid() ? 2 : 0));

        // 隐藏父 → 父与两个子一起消失（3 个控件），只剩顶层文本
        UiDocument h = d;
        h.widgets[0].visible = false;
        UiCanvas c2;
        c2.begin();
        const UiDocDrawStats st2 = drawDocument(c2, h, b, 1920, 1080, &f);
        report("uidoc_hidden_parent_skips_children",
               std::to_string(st2.widgets) + "/" + std::to_string(st2.skipped),
               "1/1", 0, st2.widgets == 1 && st2.skipped == 1);

        // 输出尺寸为 0（窗口最小化）→ 一个顶点都不许吐
        UiCanvas c3;
        c3.begin();
        drawDocument(c3, d, b, 0, 0, &f);
        report("uidoc_zero_extent", std::to_string(c3.vertices().size()), "0", 0,
               c3.vertices().empty());
    }

    // ---- 坏文档必须被拒（每一条都对应一种"静默画歪"）----
    {
        UiDocument bad;
        const bool r1 = parseUiDocument(
            R"({"widgets":[{"id":"a","type":"text","rect":[0,0,10,10]},
                          {"id":"a","type":"text","rect":[0,0,10,10]}]})",
            "dup", bad);
        report("uidoc_reject_dup_id", r1 ? "accepted" : "rejected", "rejected",
               0, !r1);
        report("uidoc_reject_dup_id_msg",
               bad.error.find("duplicate") != std::string::npos ? "1" : "0",
               "1", 0, bad.error.find("duplicate") != std::string::npos);

        UiDocument bad2;
        const bool r2 = parseUiDocument(
            R"({"widgets":[{"id":"a","type":"text","anchor":"middle-upp",
                            "rect":[0,0,10,10]}]})",
            "anchor", bad2);
        report("uidoc_reject_bad_anchor", r2 ? "accepted" : "rejected",
               "rejected", 0, !r2);

        UiDocument bad3;
        const bool r3 = parseUiDocument(
            R"({"widgets":[{"id":"a","type":"text"}]})", "rect", bad3);
        report("uidoc_reject_missing_rect", r3 ? "accepted" : "rejected",
               "rejected", 0, !r3);

        UiDocument bad4;
        const bool r4 = parseUiDocument(
            R"({"widgets":[{"id":"a","type":"slider","rect":[0,0,10,10]}]})",
            "type", bad4);
        report("uidoc_reject_unknown_type", r4 ? "accepted" : "rejected",
               "rejected", 0, !r4);

        UiDocument bad5;
        const bool r5 = parseUiDocument(
            R"({"widgets":[{"id":"a b","type":"text","rect":[0,0,10,10]}]})",
            "id", bad5);
        report("uidoc_reject_id_with_space", r5 ? "accepted" : "rejected",
               "rejected", 0, !r5);

        // 颜色写错只是**警告**（控件照样画，用默认色）—— 与上面五条不同：
        // 它不会让画面结构错位，只是颜色不对。
        UiDocument warn;
        const bool r6 = parseUiDocument(
            R"({"widgets":[{"id":"a","type":"panel","rect":[0,0,10,10],
                            "color":"0x123456"}]})",
            "color", warn);
        report("uidoc_bad_color_warns", r6 ? "1" : "0", "1", 0,
               r6 && warn.warnings.size() == 1);
    }
}


// ---- 文档层交互（2026-11-02）----
//
// 这一组替代不了运行期的端到端（那是 `tools/verify_ui_actions.py` 的事），
// 但它能在**无窗口、无 Vulkan** 的机器上把三条最容易写错的语义钉住：
// ① 最上面的赢（DFS 倒序）② 按下与松开必须同一个控件 ③ 禁用/不可见不命中。
void testInteraction() {
    // ---- 动作解析 ----
    UiAction a;
    std::string err;
    bool ok = parseUiAction("", a, &err);
    report("uiint_parse_empty", ok ? "ok" : "bad", "ok", 0,
           ok && a.kind == UiActionKind::None);

    ok = parseUiAction("flag:hp=10", a, &err);
    report("uiint_parse_flag", ok ? "ok" : "bad", "ok", 0,
           ok && a.kind == UiActionKind::Flag && a.key == "hp" &&
               std::fabs(a.value - 10.0) < 1e-9);

    // 省略 `=n` 等价于 1（"记住点了"最常用的写法）
    ok = parseUiAction("flag:seen", a, &err);
    report("uiint_parse_flag_bare_value", f2s(a.value), "1.0000", 0,
           ok && std::fabs(a.value - 1.0) < 1e-9);

    ok = parseUiAction("toggle:menu", a, &err);
    report("uiint_parse_toggle", ok ? "ok" : "bad", "ok", 0,
           ok && a.kind == UiActionKind::Toggle && a.key == "menu");

    ok = parseUiAction("sound:ui/click.wav", a, &err);
    report("uiint_parse_sound", ok ? "ok" : "bad", "ok", 0,
           ok && a.kind == UiActionKind::Sound &&
               a.key == "ui/click.wav");

    ok = parseUiAction("quit", a, &err);
    report("uiint_parse_quit", ok ? "ok" : "bad", "ok", 0,
           ok && a.kind == UiActionKind::Quit);

    // 认不出的动词 / 数字尾巴有垃圾：必须**报错**而不是静默取前几位
    ok = parseUiAction("jump:high", a, &err);
    report("uiint_parse_bad_verb", ok ? "accepted" : "rejected", "rejected", 0,
           !ok);
    ok = parseUiAction("flag:hp=10abc", a, &err);
    report("uiint_parse_bad_number", ok ? "accepted" : "rejected", "rejected", 0,
           !ok);
    ok = parseUiAction("flag:a b=3", a, &err);
    report("uiint_parse_space_in_key", ok ? "accepted" : "rejected", "rejected",
           0, !ok);

    // ---- 命中测试 ----
    // 一份 4 个控件的文档：底面板（大）+ 可点按钮 + 禁用按钮 + 无动作按钮。
    //
    // ⚠ 输出尺寸必须写成 **1920x1080**：`computeScale` 除的是**全局参考
    //   分辨率**（`kRefWidth/kRefHeight`），不是文档自己的 `canvas`。
    //   这一点与直觉不同 —— 第一版这里写 1000x1000，算出来 scale≈0.52，
    //   于是所有命中点都落到了别的控件上（三条断言红，而产品是对的）。
    const char* json =
        R"({"name":"t","canvas":[1920,1080],"widgets":[
             {"id":"base","type":"panel","rect":[0,0,600,600]},
             {"id":"ok","type":"button","rect":[10,10,100,40],
              "action":"flag:a=1"},
             {"id":"off","type":"button","rect":[10,100,100,40],
              "action":"flag:b=1","enabled":false},
             {"id":"deco","type":"button","rect":[10,200,100,40]}]})";
    UiDocument d;
    if (!parseUiDocument(json, "uiint", d)) {
        report("uiint_doc_load", "failed", "ok", 0, false);
        return;
    }
    report("uiint_doc_load", "ok", "ok", 0, true);

    const UiBindings b;
    const auto placed = placeDocument(d, b, 1920, 1080);

    // 点在按钮上 → 命中 ok
    const std::string h1 = hitTestDocument(placed, 50.0f, 25.0f);
    report("uiint_hit_button", h1, "ok", 0, h1 == "ok");

    // 点在面板空白处 → 面板**不可交互**（没 action、不是 button）→ 不命中。
    // ⚠ 这条是"面板底下的按钮抢点击"那个 bug 的反面判据。
    const std::string h2 = hitTestDocument(placed, 300.0f, 300.0f);
    report("uiint_hit_panel_not_interactive", h2.empty() ? "(none)" : h2,
           "(none)", 0, h2.empty());

    // 禁用按钮不命中
    const std::string h3 = hitTestDocument(placed, 50.0f, 115.0f);
    report("uiint_hit_disabled", h3.empty() ? "(none)" : h3, "(none)", 0,
           h3.empty());

    // 没配 action 的 button **仍然可交互**（这样点上去能打一条
    // UIDOC-NOACTION，而不是让一个看起来能按的按钮静默吃掉点击）
    const std::string h4 = hitTestDocument(placed, 50.0f, 215.0f);
    report("uiint_hit_button_without_action", h4, "deco", 0, h4 == "deco");

    // ---- 按下 / 松开语义 ----
    {
        UiDocInput in;
        // 按下在 ok 上、松开也在 ok 上 → 点击成立
        in.update(d, placed, 50.0f, 25.0f, true, true, false);
        const auto r = in.update(d, placed, 50.0f, 25.0f, false, false, true);
        report("uiint_click_same_widget", r.id.empty() ? "(none)" : r.id, "ok",
               0, r.id == "ok" && r.action == "flag:a=1");
    }
    {
        UiDocInput in;
        // 按下在 ok、松开时已经拖到面板空白处 → **不算点击**
        in.update(d, placed, 50.0f, 25.0f, true, true, false);
        const auto r = in.update(d, placed, 300.0f, 300.0f, false, false, true);
        report("uiint_click_drag_out", r.id.empty() ? "(none)" : r.id, "(none)",
               0, r.id.empty());
    }
    {
        UiDocInput in;
        // 注入的点击：压入参考像素坐标，下一次 update 消费。
        // ⚠ 注入走的是与真鼠标**完全相同**的下游 —— 否则会出现
        //   "自动化能点、人手点不动"这种只在验收里绿的假绿。
        in.requestClickRef(50.0f, 25.0f, 1920, 1080);
        const auto r = in.update(d, placed, 0.0f, 0.0f, false, false, false);
        report("uiint_click_injected", r.id.empty() ? "(none)" : r.id, "ok", 0,
               r.id == "ok" && r.action == "flag:a=1");
    }

    // ---- 条件可见 ----
    // `visibleBind` 生效时布局与绘制必须同时跳过（共用 widgetVisible）——
    // 只跳一边会让后面所有控件错位一格。
    {
        const char* vjson =
            R"({"name":"v","canvas":[1920,1080],"widgets":[
                 {"id":"hidden","type":"panel","rect":[0,0,100,100],
                  "visibleBind":"menuOpen"},
                 {"id":"after","type":"panel","rect":[0,200,100,100]}]})";
        UiDocument vd;
        if (!parseUiDocument(vjson, "uiint-v", vd)) {
            report("uiint_visbind_load", "failed", "ok", 0, false);
        } else {
            UiBindings vb;                     // menuOpen 未设 → 0 → 隐藏
            const auto p0 = placeDocument(vd, vb, 1920, 1080);
            report("uiint_visbind_hidden", std::to_string(p0.size()), "1", 0,
                   p0.size() == 1 && p0[0].id == "after");

            vb.setNumber("menuOpen", 1.0f);
            const auto p1 = placeDocument(vd, vb, 1920, 1080);
            report("uiint_visbind_shown", std::to_string(p1.size()), "2", 0,
                   p1.size() == 2 && p1[0].id == "hidden");

            // 设计画布口径：忽略 visibleBind，两个都在（否则一配就消失）
            UiRect canvas;
            canvas.w = 1920.0f;
            canvas.h = 1080.0f;
            const auto pd = placeDocumentAt(vd, UiBindings{}, canvas, 1.0f,
                                            false);
            report("uiint_visbind_design_ignores", std::to_string(pd.size()),
                   "2", 0, pd.size() == 2);
        }
    }

    // ---- 序列化往返 ----
    {
        const char* rjson =
            R"({"name":"r","canvas":[1000,1000],"widgets":[
                 {"id":"btn","type":"button","rect":[0,0,100,40],
                  "action":"toggle:menu","visibleBind":"menuOpen",
                  "enabled":false}]})";
        UiDocument rd;
        if (!parseUiDocument(rjson, "uiint-r", rd)) {
            report("uiint_roundtrip_load", "failed", "ok", 0, false);
        } else {
            const UiWidget& w = rd.widgets[0];
            const bool parsedOk = w.action == "toggle:menu" &&
                                  w.visibleBind == "menuOpen" && !w.enabled;
            report("uiint_roundtrip_parse", parsedOk ? "ok" : "bad", "ok", 0,
                   parsedOk);

            UiDocument back;
            const bool reloaded = parseUiDocument(toJson(rd), "uiint-r2", back);
            const UiWidget& w2 = back.widgets.empty() ? UiWidget{}
                                                      : back.widgets[0];
            const bool same = reloaded && w2.action == "toggle:menu" &&
                              w2.visibleBind == "menuOpen" && !w2.enabled;
            report("uiint_roundtrip_write", same ? "ok" : "lost", "ok", 0, same);
        }
    }

    // ---- 写错的动作不让整份文档加载失败 ----
    {
        const char* bjson =
            R"({"name":"b","canvas":[1000,1000],"widgets":[
                 {"id":"x","type":"button","rect":[0,0,100,40],
                  "action":"nonsense"}]})";
        UiDocument bd;
        const bool loaded = parseUiDocument(bjson, "uiint-b", bd);
        const bool warned = loaded && !bd.warnings.empty();
        report("uiint_bad_action_warns_not_fails",
               loaded ? (warned ? "warned" : "silent") : "failed", "warned", 0,
               loaded && warned);
    }
}

}  // namespace

// P3-2 的三组用例定义在文件尾部（它们要用到一组局部辅助类型）。
// ⚠ 这里前置声明，而不是把入口也挪到最后：入口留在中间，
//   "跑了哪几组"一眼能看全；用例实现集中在尾部。
namespace {
void testTransform(const SdfFont& font);
void testScrollGrid();
void testDrag();
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
    testDocument(font);
    testInteraction();

    // P3-2：变换 / 滚动 / 网格 / 拖放
    testTransform(font);
    testScrollGrid();
    testDrag();

    VK_LOG_INFO("UI2-TOTAL pass=%d fail=%d", g_pass, g_fail);
    return g_fail;
}

// ============================================================
// P3-2：变换 / 滚动 / 网格 / 拖放（2026-11-04）
//
// 这一组的判据全部落在"**两种实现会不一样**"的量上：
//   · 转了 90° 的 100×50 矩形，包围盒必须是 50×100（不是 100×50）；
//   · 转了角度的框必须走 mode 4（不切换就是把框按"中心±半宽"再放大一遍）；
//   · 滚动出可视区的子控件必须**不产出顶点**（不裁就会画在容器外面）；
//   · 拖拽松手时**不许**同时产出点击（同时产出 = 一次拖拽结算两次）。
// 只断言"没崩 / 顶点数 > 0"的用例，在这些点上一个都拦不住。
// ============================================================

namespace {

struct Aabb {
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    float w() const { return x1 - x0; }
    float h() const { return y1 - y0; }
    bool empty() const { return x1 < x0; }
};

// 顶点包围盒。⚠ 只对 **pos = 顶点** 的图元有意义（mode 1/2/4）；
//   mode 0/3 的 pos 是"中心"，四个顶点重合 —— 那正是
//   `xform_rot_uses_mode4` 要抓的东西。
Aabb vertsAabb(const UiCanvas& c) {
    Aabb b;
    for (const auto& v : c.vertices()) {
        if (v.x < b.x0) b.x0 = v.x;
        if (v.y < b.y0) b.y0 = v.y;
        if (v.x > b.x1) b.x1 = v.x;
        if (v.y > b.y1) b.y1 = v.y;
    }
    return b;
}

UiWidget mkPanel(const std::string& id, float x, float y, float w, float h) {
    UiWidget p;
    p.id = id;
    p.type = UiWidgetType::Panel;
    p.anchor = UiAnchor::TopLeft;
    p.x = x; p.y = y; p.w = w; p.h = h;
    return p;
}

UiDocument mkDoc(std::vector<UiWidget> ws) {
    UiDocument d;
    d.valid = true;
    d.widgets = std::move(ws);
    return d;
}

// ---- 变换 ----
void testTransform(const SdfFont& font) {
    const double kTol = 1e-3;

    // ① 绕中心转 90°：100×50 的框包围盒变成 50×100。
    //    ⚠ 忽略 rot 的实现会得到 100×50 —— 一眼可判。
    {
        UiCanvas c;
        c.begin();
        const float cx = 150.0f, cy = 125.0f;
        c.pushTransform(Ui2Xform::rotateScaleAbout(1.57079632679f, 1.0f, cx, cy));
        c.roundRect(100.0f, 100.0f, 100.0f, 50.0f, 8.0f, rgba(255, 255, 255));
        c.popTransform();
        const Aabb b = vertsAabb(c);
        const bool ok = std::fabs(b.w() - 50.0) < 0.01 &&
                        std::fabs(b.h() - 100.0) < 0.01;
        report("xform_rot90_aabb", f2s(b.w()) + "x" + f2s(b.h()), "50x100", kTol,
               ok);
        // ② 旋转后的框必须走 mode 4。不切换的话着色器会按"中心 ± 半宽"
        //    把四个顶点再展开一次 → 画出一个 200×100 的轴对齐大框。
        bool allMode4 = !c.vertices().empty();
        for (const auto& v : c.vertices())
            if (v.mode != static_cast<float>(Ui2Prim::RoundBoxRotated))
                allMode4 = false;
        report("xform_rot_uses_mode4",
               allMode4 ? "RoundBoxRotated" : "other", "RoundBoxRotated", 0,
               allMode4);
        // ③ 但 vLocal 的输入（uv / half）必须**保持未旋转**，否则 frag 里
        //    的 sdRoundBox 会在一个错位的局部系里求距离（圆角变歪）。
        bool localOk = true;
        for (const auto& v : c.vertices()) {
            if (std::fabs(v.hx - 50.0f) > 1e-3f) localOk = false;
            if (std::fabs(v.hy - 25.0f) > 1e-3f) localOk = false;
            if (std::fabs(std::fabs(v.u) - 1.0f) > 1e-3f) localOk = false;
        }
        report("xform_rot_keeps_local", localOk ? "half=50x25" : "broken",
               "half=50x25", 0, localOk);
    }

    // ④ 缩放（无旋转）走 mode 0/3 快路径，半宽翻倍。
    {
        UiCanvas c;
        c.begin();
        c.pushTransform(Ui2Xform::rotateScaleAbout(0.0f, 2.0f, 150.0f, 125.0f));
        c.roundRect(100.0f, 100.0f, 100.0f, 50.0f, 8.0f, rgba(255, 255, 255));
        c.popTransform();
        bool mode0or3 = !c.vertices().empty();
        bool halfOk = true;
        for (const auto& v : c.vertices()) {
            if (v.mode == static_cast<float>(Ui2Prim::RoundBoxRotated))
                mode0or3 = false;
            if (std::fabs(v.hx - 100.0f) > 1e-3f) halfOk = false;
            if (std::fabs(v.hy - 50.0f) > 1e-3f) halfOk = false;
        }
        report("xform_scale2_fastpath",
               (mode0or3 && halfOk) ? "mode3+half=100x50" : "broken",
               "mode3+half=100x50", kTol, mode0or3 && halfOk);
    }

    // ⑤ 文字跟着父控件转（**最容易漏的一条**：只转面板不转文字，
    //    截图里一眼能看出，但"父控件旋转"的断言照样绿）。
    {
        if (!font.valid()) {
            report("xform_text_follows", "no-font", "rotated", 0, false);
        } else {
            UiCanvas a;
            a.begin();
            a.text(font, 100.0f, 200.0f, "Rotation", rgba(255, 255, 255), 32.0f);
            const Aabb flat = vertsAabb(a);

            UiCanvas b;
            b.begin();
            b.pushTransform(
                Ui2Xform::rotateScaleAbout(1.57079632679f, 1.0f, 150.0f, 190.0f));
            b.text(font, 100.0f, 200.0f, "Rotation", rgba(255, 255, 255), 32.0f);
            b.popTransform();
            const Aabb rot = vertsAabb(b);
            // 横排文字转 90° 之后：宽度方向 → 高度方向
            const bool ok = std::fabs(rot.w() - flat.h()) < 2.0f &&
                            std::fabs(rot.h() - flat.w()) < 2.0f &&
                            flat.w() > flat.h();   // 前提：横排确实比高宽
            report("xform_text_follows",
                   f2s(rot.w()) + "x" + f2s(rot.h()),
                   f2s(flat.h()) + "x" + f2s(flat.w()), 2.0, ok);
        }
    }

    // ⑥ 透明度：乘进子树、可嵌套、弹出后复原。
    {
        UiCanvas c;
        c.begin();
        c.pushOpacity(0.5f);
        c.rect(0.0f, 0.0f, 10.0f, 10.0f, rgba(255, 255, 255, 255));
        c.pushOpacity(0.5f);
        c.rect(0.0f, 0.0f, 10.0f, 10.0f, rgba(255, 255, 255, 255));
        c.popOpacity();
        c.popOpacity();
        c.rect(0.0f, 0.0f, 10.0f, 10.0f, rgba(255, 255, 255, 255));
        // 三块各 6 顶点
        const auto& v = c.vertices();
        const int a1 = (v.size() >= 18) ? static_cast<int>((v[0].color >> 24) & 0xFF)
                                        : -1;
        const int a2 = (v.size() >= 18) ? static_cast<int>((v[6].color >> 24) & 0xFF)
                                        : -2;
        const int a3 = (v.size() >= 18) ? static_cast<int>((v[12].color >> 24) & 0xFF)
                                        : -3;
        const bool ok = (a1 == 128 || a1 == 127) &&
                        (a2 == 64 || a2 == 63) && a3 == 255;
        report("xform_opacity_nested",
               std::to_string(a1) + "/" + std::to_string(a2) + "/" +
                   std::to_string(a3),
               "127/63/255", 1, ok);

        // ⑦ 全透明 → **一个顶点都不产生**（不是"画成全透明"：
        //    后者仍然占顶点、仍然进 draw，而且 alpha=0 的混合在某些
        //    驱动上会与"没画"表现不同）。
        UiCanvas z;
        z.begin();
        z.pushOpacity(0.0f);
        z.roundRect(0.0f, 0.0f, 100.0f, 100.0f, 8.0f, rgba(255, 255, 255));
        z.popOpacity();
        report("xform_opacity_zero_no_verts", std::to_string(z.vertices().size()),
               "0", 0, z.vertices().empty());
    }
}

// ---- Scroll / Grid ----
void testScrollGrid() {
    // ① Grid：3 列等分，子控件自己的 rect **被忽略**。
    {
        UiWidget g = mkPanel("grid", 0.0f, 0.0f, 300.0f, 100.0f);
        g.type = UiWidgetType::Grid;
        g.cols = 3;
        g.gapX = 0.0f;
        g.gapY = 0.0f;
        for (int i = 0; i < 3; ++i) {
            UiWidget k = mkPanel("cell" + std::to_string(i), 999.0f, 999.0f,
                                 7.0f, 7.0f);   // 故意填垃圾值
            g.children.push_back(k);
        }
        const UiDocument d = mkDoc({g});
        UiBindings b;
        const auto placed = placeDocumentAt(d, b, UiRect{0, 0, 300, 100}, 1.0f);
        // placed[0] = grid，后面 3 个 cell
        bool ok = placed.size() == 4;
        if (ok) {
            for (int i = 0; i < 3; ++i) {
                const UiRect& r = placed[static_cast<std::size_t>(i) + 1].rect;
                if (std::fabs(r.x - static_cast<float>(i) * 100.0f) > 0.01f)
                    ok = false;
                if (std::fabs(r.w - 100.0f) > 0.01f) ok = false;
                if (std::fabs(r.h - 100.0f) > 0.01f) ok = false;
            }
        }
        report("grid_cells_3col",
               ok ? "x=0,100,200 w=100" : "wrong", "x=0,100,200 w=100", 0, ok);
    }

    // ② Grid 两行：4 个子控件 3 列 → 第二行 1 个，行高 = (100-gap)/2。
    {
        UiWidget g = mkPanel("grid", 0.0f, 0.0f, 300.0f, 100.0f);
        g.type = UiWidgetType::Grid;
        g.cols = 3;
        g.gapX = 0.0f;
        g.gapY = 0.0f;
        for (int i = 0; i < 4; ++i) g.children.push_back(mkPanel("c" + std::to_string(i), 0, 0, 1, 1));
        const UiDocument d = mkDoc({g});
        UiBindings b;
        const auto placed = placeDocumentAt(d, b, UiRect{0, 0, 300, 100}, 1.0f);
        bool ok = placed.size() == 5;
        if (ok) {
            const UiRect& r3 = placed[4].rect;   // 第 4 个 = 第二行第一个
            ok = std::fabs(r3.x) < 0.01f && std::fabs(r3.y - 50.0f) < 0.01f &&
                 std::fabs(r3.h - 50.0f) < 0.01f;
        }
        report("grid_rows_2", ok ? "c3=(0,50) h=50" : "wrong", "c3=(0,50) h=50",
               0, ok);
    }

    // ③ Scroll：scrollY=50 把子控件上移 50。
    {
        UiWidget s = mkPanel("scroll", 0.0f, 0.0f, 200.0f, 100.0f);
        s.type = UiWidgetType::Scroll;
        s.scrollY = 50.0f;
        s.children.push_back(mkPanel("row", 0.0f, 0.0f, 200.0f, 40.0f));
        const UiDocument d = mkDoc({s});
        UiBindings b;
        const auto placed = placeDocumentAt(d, b, UiRect{0, 0, 200, 100}, 1.0f);
        const bool ok = placed.size() == 2 &&
                        std::fabs(placed[1].rect.y + 50.0f) < 0.01f;
        report("scroll_offset", ok ? f2s(placed.back().rect.y) : "wrong", "-50",
               0.01, ok);
        // ④ 子控件被标记"有裁剪"，且裁剪矩形 = 容器矩形
        const bool clipOk = placed.size() == 2 && placed[1].hasClip &&
                            std::fabs(placed[1].clip.h - 100.0f) < 0.01f;
        report("scroll_child_clipped", clipOk ? "clip=container" : "no-clip",
               "clip=container", 0, clipOk);
    }

    // ⑤ 滚出可视区的子控件：**画不出顶点**，也**点不中**。
    //    ⚠ 不裁的话它会被画在容器外面（叠在牌桌上），而"点不中"这条
    //      是最容易被忽略的 —— 滚出列表的行矩形还在，命中测试照样会中。
    //
    //    ⚠⚠ 判据**不能**写成"顶点数 == 0"：Scroll 容器自己就是个面板，
    //      它自己要画 6 个顶点。写成 0 会被这个 6 绊住（而且看起来像
    //      "裁剪没生效"，实际是容器底）。正确的对照量是
    //      "把唯一的孩子滚出视野之后，顶点数与**没有孩子时**一样"。
    {
        auto scrollDoc = [](bool withChild, float scrollY) {
            UiWidget s = mkPanel("scroll", 0.0f, 0.0f, 200.0f, 100.0f);
            s.type = UiWidgetType::Scroll;
            s.scrollY = scrollY;
            if (withChild) {
                UiWidget row = mkPanel("row", 0.0f, 0.0f, 200.0f, 40.0f);
                row.action = "quit";   // 让它"可交互"，才测得到命中
                s.children.push_back(row);
            }
            return mkDoc({s});
        };
        UiBindings b;
        UiCanvas empty;
        empty.begin();
        drawDocument(empty, scrollDoc(false, 500.0f), b, 200, 100, nullptr,
                     nullptr);

        UiCanvas out;
        out.begin();
        const auto st = drawDocument(out, scrollDoc(true, 500.0f), b, 200, 100,
                                     nullptr, nullptr);
        report("scroll_out_of_view_no_verts",
               std::to_string(out.vertices().size()) + " vs bare " +
                   std::to_string(empty.vertices().size()),
               "same", 0,
               out.vertices().size() == empty.vertices().size() &&
                   empty.vertices().size() == 6);

        const auto placed =
            placeDocumentAt(scrollDoc(true, 500.0f), b, UiRect{0, 0, 200, 100},
                            1.0f);
        // row 的真实矩形是 y = -500..-460，容器外 —— 直接点在容器里不该命中
        const std::string hit = hitTestDocument(placed, 100.0f, 20.0f);
        report("scroll_out_of_view_no_hit", hit.empty() ? "(none)" : hit,
               "(none)", 0, hit.empty());
        (void)st;
    }

    // ⑥ 部分可见 → 有顶点 + **只有被裁的那一个**记 scissor，
    //    且 drawCalls 按"裁剪相同"切成 3 段（容器 / 子 / 容器外）。
    //
    //    ⚠ 3 段而不是 2 段是对的：两个不裁的控件被中间那个带裁的隔开，
    //      scissor 变了两次。想压成 2 段就得重排绘制顺序 —— 那是另一件事
    //      （而且会改变叠放次序）。
    {
        UiWidget s = mkPanel("scroll", 0.0f, 0.0f, 200.0f, 100.0f);
        s.type = UiWidgetType::Scroll;
        s.scrollY = 20.0f;
        s.children.push_back(mkPanel("row", 0.0f, 0.0f, 200.0f, 40.0f));
        UiWidget out = mkPanel("outside", 0.0f, 150.0f, 50.0f, 20.0f);
        const UiDocument d = mkDoc({s, out});
        UiBindings b;
        UiCanvas c;
        c.begin();
        drawDocument(c, d, b, 200, 100, nullptr, nullptr);
        int clipped = 0;
        for (const auto& sc : c.scissors())
            if (sc.any) ++clipped;
        const uint32_t calls = c.drawCalls();
        const bool ok = clipped == 1 && calls == 3;
        report("scroll_partial_scissor",
               std::to_string(clipped) + " clipped / draws=" +
                   std::to_string(calls),
               "1 clipped / draws=3", 0, ok);
    }
}

// ---- 拖放 ----
void testDrag() {
    // ① 判据本身：整词匹配 + 通配 + 空格容错。
    {
        const bool a = dropAccepts("card", "card");
        const bool b = dropAccepts("card, item", "item");
        const bool c = dropAccepts("card", "cardboard");   // 不许命中
        const bool d = dropAccepts("*", "anything");
        const bool e = !dropAccepts("", "card");
        const std::string got = std::string(a ? "1" : "0") + (b ? "1" : "0") +
                                (c ? "1" : "0") + (d ? "1" : "0") +
                                (e ? "1" : "0");
        report("drag_accepts_word", got, "11011", 0,
               a && b && !c && d && e);
    }

    // 造一份"手上两张牌 + 一个接卡牌的槽 + 一个只接道具的槽"的文档。
    auto build = [](UiDocument& d) {
        UiWidget card = mkPanel("card0", 0.0f, 0.0f, 100.0f, 140.0f);
        card.dragTag = "card";
        UiWidget slotA = mkPanel("slotA", 300.0f, 0.0f, 120.0f, 140.0f);
        slotA.dropAccept = "card";
        UiWidget slotB = mkPanel("slotB", 500.0f, 0.0f, 120.0f, 140.0f);
        slotB.dropAccept = "item";
        d = mkDoc({card, slotA, slotB});
    };

    // ② 按下就进拖拽，并记下 tag。
    {
        UiDocument d;
        build(d);
        UiBindings b;
        const auto placed = placeDocumentAt(d, b, UiRect{0, 0, 800, 200}, 1.0f);
        UiDocInput in;
        const auto r = in.update(d, placed, 50.0f, 70.0f, true, true, false);
        const bool ok = r.dragBegan && r.dragId == "card0" && r.dragTag == "card";
        report("drag_begin", ok ? r.dragId + "/" + r.dragTag : "no", "card0/card",
               0, ok);
    }

    // ③ 拖动时**只有合法目标**被高亮。
    {
        UiDocument d;
        build(d);
        UiBindings b;
        const auto placed = placeDocumentAt(d, b, UiRect{0, 0, 800, 200}, 1.0f);
        UiDocInput in;
        in.update(d, placed, 50.0f, 70.0f, true, true, false);
        // 指针移到 slotA 上方（合法）
        in.update(d, placed, 360.0f, 70.0f, true, false, false);
        const auto& st = in.state();
        UiCanvas c;
        c.begin();
        const auto ds = drawDocument(c, d, b, 800, 200, nullptr, &st);
        const bool ok = ds.highlights == 1 && st.dropTarget == "slotA";
        report("drag_highlight_legal_only",
               std::to_string(ds.highlights) + "/" +
                   (st.dropTarget.empty() ? "(none)" : st.dropTarget),
               "1/slotA", 0, ok);
    }

    // ④ 指针在**不合法**目标上 → 高亮 0 个（但合法目标仍应高亮？不 ——
    //    P3-2 的约定是"拖动期间所有合法目标都淡高亮，指针底下的那个亮"）。
    //    所以这里验的是"只接 item 的槽不算合法目标"：高亮总数仍是 1。
    {
        UiDocument d;
        build(d);
        UiBindings b;
        const auto placed = placeDocumentAt(d, b, UiRect{0, 0, 800, 200}, 1.0f);
        UiDocInput in;
        in.update(d, placed, 50.0f, 70.0f, true, true, false);
        in.update(d, placed, 560.0f, 70.0f, true, false, false);   // slotB
        const auto& st = in.state();
        UiCanvas c;
        c.begin();
        const auto ds = drawDocument(c, d, b, 800, 200, nullptr, &st);
        const bool ok = ds.highlights == 1 && st.dropTarget.empty();
        report("drag_highlight_skips_illegal",
               std::to_string(ds.highlights) + "/" +
                   (st.dropTarget.empty() ? "(none)" : st.dropTarget),
               "1/(none)", 0, ok);
    }

    // ⑤ 松手落在合法目标上 → 有 dropId、**且没有点击**。
    {
        UiDocument d;
        build(d);
        UiBindings b;
        const auto placed = placeDocumentAt(d, b, UiRect{0, 0, 800, 200}, 1.0f);
        UiDocInput in;
        in.update(d, placed, 50.0f, 70.0f, true, true, false);
        in.update(d, placed, 360.0f, 70.0f, true, false, false);
        const auto r = in.update(d, placed, 360.0f, 70.0f, false, false, true);
        const bool ok = r.dragEnded && r.dropId == "slotA" && r.id.empty();
        report("drag_drop_no_click",
               (r.dropId.empty() ? "no-drop" : r.dropId) +
                   (r.id.empty() ? "/no-click" : "/CLICKED"),
               "slotA/no-click", 0, ok);
    }

    // ⑥ 松手落在空白 / 非法目标上 → 取消，且没有点击。
    {
        UiDocument d;
        build(d);
        UiBindings b;
        const auto placed = placeDocumentAt(d, b, UiRect{0, 0, 800, 200}, 1.0f);
        UiDocInput in;
        in.update(d, placed, 50.0f, 70.0f, true, true, false);
        const auto r = in.update(d, placed, 700.0f, 190.0f, false, false, true);
        const bool ok = r.dragEnded && r.dropId.empty() && r.id.empty();
        report("drag_cancel_no_target",
               (r.dropId.empty() ? "(none)" : r.dropId) +
                   (r.id.empty() ? "/no-click" : "/CLICKED"),
               "(none)/no-click", 0, ok);
    }

    // ⑦ 落在自己身上 → 不算放下（否则拖起来又松手 = 一次自我转移）。
    {
        UiDocument d = mkDoc({[] {
            UiWidget c = mkPanel("self", 0.0f, 0.0f, 100.0f, 100.0f);
            c.dragTag = "card";
            c.dropAccept = "card";     // 自己也能接
            return c;
        }()});
        UiBindings b;
        const auto placed = placeDocumentAt(d, b, UiRect{0, 0, 400, 200}, 1.0f);
        UiDocInput in;
        in.update(d, placed, 50.0f, 50.0f, true, true, false);
        const auto r = in.update(d, placed, 50.0f, 50.0f, false, false, true);
        const bool ok = r.dragEnded && r.dropId.empty() && r.id.empty();
        report("drag_self_no_drop", r.dropId.empty() ? "(none)" : r.dropId,
               "(none)", 0, ok);
    }

    // ⑧ 旋转后的命中：100×20 的牌子转 90° 变成竖条，原来的 AABB 两侧
    //    **不该**命中，竖条两端**该**命中。
    {
        UiDocument d;
        UiWidget w0 = mkPanel("bar", 100.0f, 100.0f, 100.0f, 20.0f);
        w0.action = "quit";
        w0.rot = 90.0f;            // 绕中心 (150,110)
        d = mkDoc({w0});
        UiBindings b;
        const auto placed = placeDocumentAt(d, b, UiRect{0, 0, 400, 300}, 1.0f);
        // 竖条范围约 x∈[140,160], y∈[60,160]
        const std::string inBody = hitTestDocument(placed, 150.0f, 150.0f);
        const std::string inAabbOnly = hitTestDocument(placed, 200.0f, 110.0f);
        const bool ok = inBody == "bar" && inAabbOnly.empty();
        report("drag_hit_rotated",
               (inBody.empty() ? "(none)" : inBody) + "/" +
                   (inAabbOnly.empty() ? "(none)" : inAabbOnly),
               "bar/(none)", 0, ok);
    }

    // ⑨ 拖拽幽灵：拖动期间顶点数明显多于静态绘制。
    {
        UiDocument d;
        build(d);
        UiBindings b;
        const auto placed = placeDocumentAt(d, b, UiRect{0, 0, 800, 200}, 1.0f);
        UiDocInput in;
        in.update(d, placed, 50.0f, 70.0f, true, true, false);
        const auto& st = in.state();
        UiCanvas still;
        still.begin();
        drawDocument(still, d, b, 800, 200, nullptr, nullptr);
        UiCanvas drag;
        drag.begin();
        drawDocument(drag, d, b, 800, 200, nullptr, &st);
        const bool ok = drag.vertices().size() > still.vertices().size();
        report("drag_ghost_draws",
               std::to_string(drag.vertices().size()) + ">" +
                   std::to_string(still.vertices().size()),
               "more", 0, ok);
    }
}

}  // namespace

}  // namespace ui2
