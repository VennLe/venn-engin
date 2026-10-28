// ============================================================
// engine/ui/ImGuiTheme.cpp —— 主题表 + 应用
// ============================================================

#include "ui/ImGuiTheme.h"

#include <algorithm>
#include <cmath>

namespace ui {
namespace {

struct Theme {
    ImVec4 accent;
    ImVec4 text, textDisabled;
    ImVec4 windowBg, childBg, popupBg, border;
    ImVec4 frameBg, frameBgHovered, frameBgActive;
    ImVec4 titleBg, titleBgActive, menuBarBg;
    ImVec4 scrollbarBg, scrollGrab, scrollGrabHovered, scrollGrabActive;
    ImVec4 sliderGrab, sliderGrabActive;
    ImVec4 button, buttonHovered, buttonActive;
    ImVec4 header, headerHovered, headerActive;
    ImVec4 separator, separatorHovered;
    ImVec4 resizeGrip, resizeGripHovered;
    ImVec4 tab, tabHovered, tabSelected;
    ImVec4 plotLines;
    ImVec4 textSelectedBg;
    ImVec4 tableHeaderBg, tableBorderStrong, tableBorderLight, tableRowBgAlt;
    ThemeTokens tok;
    ThemeMetrics met;
};

ImVec4 rgb(float r, float g, float b, float a = 1.0f) {
    return ImVec4(r, g, b, a);
}

// 整体提亮/压暗（四舍五入到 0..1 区间；alpha 不动）
ImVec4 shift(const ImVec4& c, float d) {
    return ImVec4(std::clamp(c.x + d, 0.0f, 1.0f),
                  std::clamp(c.y + d, 0.0f, 1.0f),
                  std::clamp(c.z + d, 0.0f, 1.0f), c.w);
}

// 线性插值（t=0 取 a，t=1 取 b）
ImVec4 mix(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                  a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

ImVec4 withAlpha(const ImVec4& c, float a) {
    ImVec4 o = c;
    o.w = a;
    return o;
}

// ---------------------------------------------------------------- 压暗
// ⚠ 这个函数存在的**唯一理由**是一条渲染管线事实：
//   本工程的交换链把 ImGui 的浮点色当线性值再编码一次（坑点 #34），
//   所以 `0.106` 上屏大约变成 `0.106^(1/2.2) ≈ 0.36`（RGB 92）——
//   也就是说"深色主题"在这块屏上从来没有真黑过，全是中灰。
//
//   动漫风要的是**真正深的夜底色**（屏幕上 RGB 40~60），于是这里对整套
//   颜色做一次 `c^k`（k > 1）的压暗。为什么是幂而不是减法：
//     · 幂保留 0 与 1（纯黑 / 高亮强调色不被压塌）；
//     · 色相关系保持（粉还是粉，不会发灰）；
//     · 强调色 0.98^1.45 ≈ 0.97，上屏仍是 250 —— 亮部几乎不动，
//       暗部被拉开，正好是"深底 + 霓虹"的动漫观感。
//
// ⚠ 只对**动漫档的深色主题**用。工程档（0~4）的数值一个都不许动 ——
//   `verify_theme_language.py` 按亮度区间判它们，是既有基准。
void darkenThemeColors(Theme& t, float k) {
    ImVec4* all[] = {
        &t.accent, &t.text, &t.textDisabled,
        &t.windowBg, &t.childBg, &t.popupBg, &t.border,
        &t.frameBg, &t.frameBgHovered, &t.frameBgActive,
        &t.titleBg, &t.titleBgActive, &t.menuBarBg,
        &t.scrollbarBg, &t.scrollGrab, &t.scrollGrabHovered, &t.scrollGrabActive,
        &t.sliderGrab, &t.sliderGrabActive,
        &t.button, &t.buttonHovered, &t.buttonActive,
        &t.header, &t.headerHovered, &t.headerActive,
        &t.separator, &t.separatorHovered,
        &t.resizeGrip, &t.resizeGripHovered,
        &t.tab, &t.tabHovered, &t.tabSelected,
        &t.plotLines, &t.textSelectedBg,
        &t.tableHeaderBg, &t.tableBorderStrong, &t.tableBorderLight,
        &t.tableRowBgAlt,
        &t.tok.panelBg, &t.tok.panelBgAlt, &t.tok.accent, &t.tok.accentSoft,
        &t.tok.textDim, &t.tok.textFaint,
        &t.tok.ok, &t.tok.warn, &t.tok.error, &t.tok.info,
        &t.tok.axisX, &t.tok.axisY, &t.tok.axisZ, &t.tok.barBg,
        &t.tok.cardBg, &t.tok.cardBgHover, &t.tok.borderSoft,
        &t.tok.titleBar, &t.tok.titleBarActive, &t.tok.glow, &t.tok.trackBg,
    };
    for (ImVec4* c : all) {
        c->x = std::pow(std::max(c->x, 0.0f), k);
        c->y = std::pow(std::max(c->y, 0.0f), k);
        c->z = std::pow(std::max(c->z, 0.0f), k);
    }
}

// ---------------------------------------------------------------- 收尾
// 把"派生出来的新 token + 度量"一次性补齐。
//
// ⚠ 为什么单独一个函数而不是在三套 derive 里各写一遍：
//   · 老主题（0~4）的每一个**已有**字段都必须与改动前逐字段一致 ——
//     把新字段的推导集中在这里，就不用手改那三段、也就不会手滑碰到老值；
//   · 新主题只要选一套"风格档"（anime / engineering），其余自动一致。
//
// `anime` 参数决定度量档位与是否开装饰。
Theme finalize(Theme t, bool anime) {
    const ImVec4& a = t.tok.accent;

    // ---- 层次（用户第 3 条："面板本身的颜色风格不够错落有致"）----
    // 四档明度：windowBg < panelBg < cardBg < cardBgHover。
    // 老代码只有前两档，所以面板与面板之间看起来"糊成一片"。
    t.tok.cardBg = t.tok.isLight ? shift(t.tok.panelBg, 0.045f)
                                 : shift(t.tok.panelBg, 0.042f);
    t.tok.cardBgHover = mix(t.tok.cardBg, a, t.tok.isLight ? 0.16f : 0.22f);
    t.tok.borderSoft = withAlpha(t.border, t.tok.isLight ? 0.55f : 0.42f);
    t.tok.trackBg = t.tok.isLight ? shift(t.tok.panelBg, -0.045f)
                                  : shift(t.tok.panelBg, -0.018f);
    t.tok.glow = withAlpha(a, t.tok.isLight ? 0.22f : 0.30f);

    // ---- 标题栏（用户第 2 条："窗口顶部的白框显得很呆板"）----
    // 激活态掺强调色：不再是"底色 + 一条线"，而是**有颜色倾向的一块带**。
    t.tok.titleBar = t.titleBg;
    t.tok.titleBarActive = mix(t.titleBgActive, a,
                               t.tok.isLight ? 0.14f : 0.20f);

    // ---- 圆角与边框（用户第 2 条："窗口本身应该做成圆角边框"）----
    t.tok.isAnime = anime;
    if (anime) {
        // 动漫档把"层次感"拉开一档：用户的第 3 条抱怨就是"面板本身的颜色
        // 风格不够错落有致"。派生主题的默认间距（0.035 / 0.042）是给
        // 工程软件那种"一切都差不多"的观感准备的，堆四层之后就糊成一片。
        // 这里改成 0.070 / 0.105 —— 一眼能分出"窗口 / 面板 / 卡片 / 卡片悬停"。
        t.tok.cardBg = shift(t.tok.panelBg, t.tok.isLight ? 0.055f : 0.070f);
        t.tok.panelBgAlt =
            shift(t.tok.panelBg, t.tok.isLight ? 0.090f : 0.105f);
        t.tok.cardBgHover = mix(t.tok.cardBg, a, t.tok.isLight ? 0.18f : 0.26f);

        // 卡片要真的"浮"在窗口上：`BeginChild(..., true)` 用的是
        // ImGuiCol_ChildBg，不是 cardBg。不接上的话左右两栏与窗口底同色 ——
        // 用户说的"颜色风格不够错落有致"有一半就是这个（第一版实测两栏
        // 与背景几乎同色，只有 3 个色阶的差）。
        t.childBg = t.tok.cardBg;

        // 选中行 / 悬停行：派生档是 `mix(bg, accent, 0.28)`，在深底上会变成
        // **一整块高饱和粉**（实测上屏 RGB≈140,80,110），压过正文。动漫档
        // 收到 0.14 / 0.20 / 0.28 三档，靠明度递进而非饱和递进区分状态。
        {
            const float k = t.tok.isLight ? 0.14f : 0.14f;
            t.header = mix(t.windowBg, a, k);
            t.headerHovered = mix(t.windowBg, a, k * 1.45f);
            t.headerActive = mix(t.windowBg, a, k * 2.05f);
            t.tabSelected = t.header;
            t.tabHovered = t.headerHovered;
            t.tok.accentSoft = t.header;
        }

        t.tok.radiusLg = 12.0f;
        t.tok.radiusMd = 9.0f;
        t.tok.radiusSm = 6.0f;

        t.met.windowRounding = 12.0f;
        t.met.childRounding = 12.0f;
        t.met.frameRounding = 9.0f;
        t.met.popupRounding = 14.0f;
        t.met.scrollbarRounding = 10.0f;
        t.met.grabRounding = 9.0f;
        t.met.tabRounding = 9.0f;
        // 动漫风不要硬边框：靠色差分层。留一条极细的软边给弹层，
        // 否则弹层贴在同一色面板上会"没有边界感"。
        t.met.windowBorder = 0.0f;
        t.met.childBorder = 0.0f;
        t.met.frameBorder = 0.0f;
        t.met.popupBorder = 1.0f;
        t.met.framePadY = 6.0f;   // 控件胖一点才可爱

        // 深色动漫档压暗一次（见 darkenThemeColors 的注释：这台机器的
        // 交换链会把 0.106 显示成 RGB 92，"深色主题"其实一直是中灰）。
        // 浅色档不压 —— 它要的是"奶油白"，压下去会变成脏灰。
        if (!t.tok.isLight) darkenThemeColors(t, 1.45f);
    } else {
        // 工程档：保留原来的 1px 底边与 4px 圆角体系，只是把圆角
        // 统一提到 8（原来 4 太"方"，是用户说的"硬朗"来源之一）。
        t.tok.radiusLg = 8.0f;
        t.tok.radiusMd = 6.0f;
        t.tok.radiusSm = 4.0f;

        t.met.windowRounding = 8.0f;
        t.met.childRounding = 8.0f;
        t.met.frameRounding = 6.0f;
        t.met.popupRounding = 10.0f;
        t.met.scrollbarRounding = 8.0f;
        t.met.grabRounding = 6.0f;
        t.met.tabRounding = 6.0f;
        t.met.windowBorder = 1.0f;
        t.met.childBorder = 1.0f;
        t.met.frameBorder = 0.0f;
        t.met.popupBorder = 1.0f;
        t.met.framePadY = 5.0f;
    }
    return t;
}

// ---------------------------------------------------------------- 出厂主题
// ⚠ 这一段是**逐字段照抄**改动前的 applyVennStyle()（含所有中间值）。
//   任何"顺手调到更好看"的改动都会让默认主题下的像素断言失去基准。
//   （新 token 由 finalize 补齐，不在这里写。）
Theme themeVennDark() {
    Theme t;
    const ImVec4 accent = rgb(0.16f, 0.53f, 0.94f);

    t.accent = accent;
    t.text = rgb(0.86f, 0.87f, 0.89f);
    t.textDisabled = rgb(0.48f, 0.50f, 0.53f);

    t.windowBg = rgb(0.106f, 0.110f, 0.118f);
    t.childBg = rgb(0.129f, 0.133f, 0.141f);
    t.popupBg = rgb(0.118f, 0.122f, 0.129f, 0.98f);
    t.border = rgb(0.235f, 0.245f, 0.263f, 0.85f);

    t.frameBg = rgb(0.176f, 0.184f, 0.196f);
    t.frameBgHovered = rgb(0.227f, 0.239f, 0.259f);
    t.frameBgActive = rgb(0.263f, 0.280f, 0.310f);

    t.titleBg = rgb(0.113f, 0.117f, 0.125f);
    t.titleBgActive = rgb(0.149f, 0.157f, 0.169f);
    t.menuBarBg = rgb(0.113f, 0.117f, 0.125f);

    t.scrollbarBg = rgb(0.086f, 0.090f, 0.098f);
    t.scrollGrab = rgb(0.286f, 0.298f, 0.318f);
    t.scrollGrabHovered = rgb(0.353f, 0.365f, 0.388f);
    t.scrollGrabActive = rgb(0.420f, 0.435f, 0.459f);

    t.sliderGrab = rgb(0.29f, 0.42f, 0.63f);
    t.sliderGrabActive = accent;

    t.button = rgb(0.200f, 0.211f, 0.231f);
    t.buttonHovered = rgb(0.247f, 0.310f, 0.427f);
    t.buttonActive = rgb(0.216f, 0.400f, 0.706f);

    t.header = rgb(0.188f, 0.267f, 0.400f);
    t.headerHovered = rgb(0.231f, 0.365f, 0.573f);
    t.headerActive = rgb(0.243f, 0.451f, 0.780f);

    t.separator = rgb(0.235f, 0.245f, 0.263f);
    t.separatorHovered = rgb(0.310f, 0.400f, 0.560f);
    t.resizeGrip = rgb(0.235f, 0.245f, 0.263f, 0.60f);
    t.resizeGripHovered = rgb(0.310f, 0.400f, 0.560f, 0.80f);

    t.tab = rgb(0.145f, 0.153f, 0.165f);
    t.tabHovered = rgb(0.231f, 0.365f, 0.573f);
    t.tabSelected = rgb(0.188f, 0.267f, 0.400f);

    t.plotLines = rgb(0.60f, 0.66f, 0.74f);
    t.textSelectedBg = rgb(0.16f, 0.53f, 0.94f, 0.35f);

    t.tableHeaderBg = rgb(0.145f, 0.153f, 0.165f);
    t.tableBorderStrong = rgb(0.235f, 0.245f, 0.263f);
    t.tableBorderLight = rgb(0.180f, 0.188f, 0.200f);
    t.tableRowBgAlt = rgb(1.0f, 1.0f, 1.0f, 0.02f);

    t.tok.panelBg = rgb(0.098f, 0.102f, 0.110f);
    t.tok.panelBgAlt = rgb(0.145f, 0.153f, 0.165f);
    t.tok.accent = accent;
    t.tok.accentSoft = rgb(0.188f, 0.267f, 0.400f);
    t.tok.textDim = rgb(0.48f, 0.50f, 0.53f);
    t.tok.textFaint = rgb(0.38f, 0.40f, 0.43f);
    t.tok.ok = rgb(0.35f, 0.78f, 0.42f);
    t.tok.warn = rgb(0.95f, 0.72f, 0.25f);
    t.tok.error = rgb(0.90f, 0.34f, 0.30f);
    t.tok.info = rgb(0.16f, 0.53f, 0.94f);
    t.tok.axisX = rgb(0.86f, 0.26f, 0.24f);
    t.tok.axisY = rgb(0.46f, 0.82f, 0.33f);
    t.tok.axisZ = rgb(0.35f, 0.55f, 0.95f);
    // 顶栏/工具条底 = 与 MenuBarBg 同值（改动前 Toolbar.cpp / EditorApp.cpp
    // 里那两处 `ImVec4(0.113f,0.117f,0.125f)` 就是照抄 MenuBarBg 的硬编码，
    // 在浅色主题下会留成两条深色孤岛 → 这次收敛成 token）。
    t.tok.barBg = rgb(0.113f, 0.117f, 0.125f);
    t.tok.isLight = false;
    return finalize(t, /*anime=*/false);
}

// ---------------------------------------------------------------- 派生
// 深色主题只给三个锚点（底色 / 强调色 / 文字色），其余由明度关系推出。
// 这样加一套新风格只要写一行，而且不会出现"某个槽忘了改、新主题里还是
// 上一套的颜色"这种低级事故。
Theme deriveDark(const ImVec4& bg, const ImVec4& accent, const ImVec4& text,
                 bool anime = false) {
    Theme t;
    t.accent = accent;
    t.text = text;
    t.textDisabled = mix(bg, text, 0.42f);

    t.windowBg = bg;
    t.childBg = shift(bg, 0.020f);
    t.popupBg = withAlpha(shift(bg, 0.012f), 0.98f);
    t.border = withAlpha(shift(bg, 0.120f), 0.85f);

    t.frameBg = shift(bg, 0.065f);
    t.frameBgHovered = shift(bg, 0.110f);
    t.frameBgActive = shift(bg, 0.145f);

    t.titleBg = shift(bg, 0.006f);
    t.titleBgActive = shift(bg, 0.040f);
    t.menuBarBg = t.titleBg;

    t.scrollbarBg = shift(bg, -0.020f);
    t.scrollGrab = shift(bg, 0.175f);
    t.scrollGrabHovered = shift(bg, 0.240f);
    t.scrollGrabActive = shift(bg, 0.300f);

    t.sliderGrab = mix(bg, accent, 0.55f);
    t.sliderGrabActive = accent;

    t.button = shift(bg, 0.090f);
    t.buttonHovered = mix(t.button, accent, 0.45f);
    t.buttonActive = mix(bg, accent, 0.80f);

    t.header = mix(bg, accent, 0.28f);
    t.headerHovered = mix(bg, accent, 0.44f);
    t.headerActive = mix(bg, accent, 0.62f);

    t.separator = shift(bg, 0.120f);
    t.separatorHovered = mix(t.separator, accent, 0.45f);
    t.resizeGrip = withAlpha(t.separator, 0.60f);
    t.resizeGripHovered = withAlpha(t.separatorHovered, 0.80f);

    t.tab = shift(bg, 0.035f);
    t.tabHovered = t.headerHovered;
    t.tabSelected = t.header;

    t.plotLines = mix(bg, text, 0.62f);
    t.textSelectedBg = withAlpha(accent, 0.35f);

    t.tableHeaderBg = t.tab;
    t.tableBorderStrong = t.separator;
    t.tableBorderLight = shift(bg, 0.070f);
    t.tableRowBgAlt = rgb(1.0f, 1.0f, 1.0f, 0.02f);

    t.tok.panelBg = shift(bg, -0.012f);
    t.tok.panelBgAlt = shift(bg, 0.035f);
    t.tok.accent = accent;
    t.tok.accentSoft = t.header;
    t.tok.textDim = mix(bg, text, 0.48f);
    t.tok.textFaint = mix(bg, text, 0.34f);
    t.tok.ok = rgb(0.35f, 0.78f, 0.42f);
    t.tok.warn = rgb(0.95f, 0.72f, 0.25f);
    t.tok.error = rgb(0.90f, 0.34f, 0.30f);
    t.tok.info = accent;
    t.tok.axisX = rgb(0.86f, 0.26f, 0.24f);
    t.tok.axisY = rgb(0.46f, 0.82f, 0.33f);
    t.tok.axisZ = rgb(0.35f, 0.55f, 0.95f);
    t.tok.barBg = t.menuBarBg;   // 顶栏/工具条 = 与菜单栏同色（连成一条）
    t.tok.isLight = false;
    return finalize(t, anime);
}

// 浅色主题：底色亮、边框比底色暗，"悬停/激活"是**加深**而不是提亮 ——
// 直接把深色那套规则反过来用会让浅色面板上的控件全都发白、看不见。
Theme deriveLight(const ImVec4& bg, const ImVec4& accent,
                  const ImVec4& text, bool anime = false) {
    Theme t;
    t.accent = accent;
    t.text = text;
    t.textDisabled = mix(bg, text, 0.40f);

    t.windowBg = bg;
    t.childBg = shift(bg, 0.030f);
    t.popupBg = withAlpha(shift(bg, 0.055f), 0.98f);
    t.border = withAlpha(shift(bg, -0.130f), 0.85f);

    t.frameBg = shift(bg, 0.070f);
    t.frameBgHovered = shift(bg, 0.035f);
    t.frameBgActive = shift(bg, -0.010f);

    t.titleBg = shift(bg, -0.030f);
    t.titleBgActive = shift(bg, 0.055f);
    t.menuBarBg = t.titleBg;

    t.scrollbarBg = shift(bg, -0.055f);
    t.scrollGrab = shift(bg, -0.190f);
    t.scrollGrabHovered = shift(bg, -0.260f);
    t.scrollGrabActive = shift(bg, -0.320f);

    t.sliderGrab = mix(bg, accent, 0.45f);
    t.sliderGrabActive = accent;

    t.button = shift(bg, -0.045f);
    t.buttonHovered = mix(t.button, accent, 0.30f);
    t.buttonActive = mix(t.button, accent, 0.55f);

    t.header = mix(bg, accent, 0.22f);
    t.headerHovered = mix(bg, accent, 0.34f);
    t.headerActive = mix(bg, accent, 0.46f);

    t.separator = shift(bg, -0.110f);
    t.separatorHovered = mix(t.separator, accent, 0.55f);
    t.resizeGrip = withAlpha(t.separator, 0.60f);
    t.resizeGripHovered = withAlpha(t.separatorHovered, 0.80f);

    t.tab = shift(bg, -0.030f);
    t.tabHovered = t.headerHovered;
    t.tabSelected = shift(bg, 0.055f);

    t.plotLines = mix(bg, text, 0.70f);
    t.textSelectedBg = withAlpha(accent, 0.28f);

    t.tableHeaderBg = shift(bg, -0.030f);
    t.tableBorderStrong = shift(bg, -0.130f);
    t.tableBorderLight = shift(bg, -0.070f);
    t.tableRowBgAlt = rgb(0.0f, 0.0f, 0.0f, 0.03f);

    t.tok.panelBg = shift(bg, -0.045f);
    t.tok.panelBgAlt = shift(bg, 0.030f);
    t.tok.accent = accent;
    t.tok.accentSoft = t.header;
    t.tok.textDim = mix(bg, text, 0.55f);
    t.tok.textFaint = mix(bg, text, 0.40f);
    t.tok.ok = rgb(0.13f, 0.55f, 0.24f);
    t.tok.warn = rgb(0.75f, 0.48f, 0.02f);
    t.tok.error = rgb(0.78f, 0.16f, 0.13f);
    t.tok.info = accent;
    t.tok.axisX = rgb(0.80f, 0.16f, 0.14f);
    t.tok.axisY = rgb(0.22f, 0.60f, 0.14f);
    t.tok.axisZ = rgb(0.16f, 0.36f, 0.86f);
    t.tok.barBg = t.menuBarBg;   // 顶栏/工具条 = 与菜单栏同色（连成一条）
    t.tok.isLight = true;
    return finalize(t, anime);
}

Theme themeAt(int index) {
    switch (static_cast<ThemeId>(index)) {
    case ThemeId::UeDark:
        // UE5 编辑器：接近纯黑的底 + 冷蓝强调
        return deriveDark(rgb(0.071f, 0.071f, 0.078f),
                          rgb(0.184f, 0.510f, 0.906f),
                          rgb(0.878f, 0.886f, 0.898f));
    case ThemeId::BlenderDark:
        // Blender：中性中灰 + 橙色强调（一眼就和蓝色系区分开）
        return deriveDark(rgb(0.176f, 0.176f, 0.180f),
                          rgb(0.925f, 0.478f, 0.145f),
                          rgb(0.878f, 0.878f, 0.882f));
    case ThemeId::UnityDark:
        // Unity：深青灰 + 亮青强调
        return deriveDark(rgb(0.129f, 0.137f, 0.145f),
                          rgb(0.176f, 0.737f, 0.667f),
                          rgb(0.867f, 0.882f, 0.890f));
    case ThemeId::Daylight:
        // 浅色：浅灰底 + 深色文字
        return deriveLight(rgb(0.906f, 0.910f, 0.918f),
                           rgb(0.145f, 0.451f, 0.878f),
                           rgb(0.137f, 0.149f, 0.169f));

    // ---------------- 二次元风格（2026-10-27）----------------
    // 底色都比纯灰"偏一点色相"：紫黑 / 墨绿 / 葡萄紫 / 藏青。
    // 纯灰底是"硬朗"的主要来源 —— 三次元工程软件的默认脸。
    case ThemeId::SakuraNight:
        // ★ 新出厂：深紫黑 + 樱花粉。灵感是夜樱。
        return deriveDark(rgb(0.094f, 0.082f, 0.106f),
                                   rgb(0.980f, 0.435f, 0.678f),
                                   rgb(0.945f, 0.914f, 0.945f),
                          /*anime=*/true);
    case ThemeId::MatchaMint:
        // 深墨绿 + 薄荷青绿。安静、护眼，长时间摆弄模型不刺眼。
        return deriveDark(rgb(0.078f, 0.106f, 0.098f),
                                   rgb(0.353f, 0.886f, 0.722f),
                                   rgb(0.894f, 0.949f, 0.922f),
                          /*anime=*/true);
    case ThemeId::SunsetPop:
        // 深葡萄紫 + 夕阳橘。傍晚天空那一段渐变。
        return deriveDark(rgb(0.114f, 0.078f, 0.129f),
                                   rgb(0.996f, 0.573f, 0.302f),
                                   rgb(0.961f, 0.890f, 0.914f),
                          /*anime=*/true);
    case ThemeId::CyberNeon:
        // 近黑藏青 + 霓虹青。赛博 / 电子感，对比最强。
        return deriveDark(rgb(0.051f, 0.055f, 0.086f),
                                   rgb(0.302f, 0.925f, 0.949f),
                                   rgb(0.867f, 0.937f, 0.972f),
                          /*anime=*/true);
    case ThemeId::SakuraDay:
        // 浅色奶油樱粉。白天 / 强光环境用，粉底但不刺眼。
        return deriveLight(rgb(0.976f, 0.949f, 0.961f),
                           rgb(0.898f, 0.353f, 0.549f),
                           rgb(0.235f, 0.161f, 0.204f),
                           /*anime=*/true);

    case ThemeId::VennDark:
    default:
        return themeVennDark();
    }
}

const char* kThemeNames[] = {
    "Venn Dark",      // VennDark
    "UE Dark",        // UeDark
    "Blender Dark",   // BlenderDark
    "Unity Dark",     // UnityDark
    "Daylight",       // Daylight
    "Sakura Night",   // SakuraNight  ★ 出厂
    "Matcha Mint",    // MatchaMint
    "Sunset Pop",     // SunsetPop
    "Cyber Neon",     // CyberNeon
    "Sakura Day",     // SakuraDay
};

// 尺寸度量：**所有主题共用** padding / spacing（改这里 = 改版式 = 一批像素
// 断言要重算）。圆角与边框宽度从 ThemeMetrics 拿 —— 那两个只动窗口四个角，
// 不动任何矩形，属于"风格"而不是"版式"。
void applyBaseMetrics(ImGuiStyle& s, const ThemeMetrics& m) {
    s.WindowPadding = ImVec2(10.0f, 8.0f);
    s.FramePadding = ImVec2(9.0f, m.framePadY);
    s.ItemSpacing = ImVec2(9.0f, 7.0f);    // 默认 8,4 → 行间透气
    s.ItemInnerSpacing = ImVec2(7.0f, 5.0f);
    s.CellPadding = ImVec2(7.0f, 5.0f);
    s.IndentSpacing = 20.0f;
    s.ScrollbarSize = 13.0f;
    s.GrabMinSize = 11.0f;

    s.WindowBorderSize = m.windowBorder;
    s.ChildBorderSize = m.childBorder;
    s.PopupBorderSize = m.popupBorder;
    s.FrameBorderSize = m.frameBorder;
    s.TabBorderSize = 0.0f;

    s.WindowRounding = m.windowRounding;
    s.ChildRounding = m.childRounding;
    s.FrameRounding = m.frameRounding;
    s.PopupRounding = m.popupRounding;
    s.ScrollbarRounding = m.scrollbarRounding;
    s.GrabRounding = m.grabRounding;
    s.TabRounding = m.tabRounding;

    s.WindowTitleAlign = ImVec2(0.02f, 0.5f);
    s.WindowMenuButtonPosition = ImGuiDir_None;   // 面板标题左边不留折叠三角
    s.SeparatorTextBorderSize = 1.0f;
    s.SeparatorTextPadding = ImVec2(14.0f, 4.0f);
}

int g_current = kDefaultThemeIndex;
ThemeTokens g_tokens;
ThemeMetrics g_metrics;

} // namespace

int themeCount() { return static_cast<int>(ThemeId::Count); }

const char* themeName(int index) {
    if (index < 0 || index >= themeCount()) index = 0;
    return kThemeNames[index];
}

int currentThemeIndex() { return g_current; }

const ThemeTokens& tokens() { return g_tokens; }

const ThemeMetrics& metrics() { return g_metrics; }

void themeTokensAt(int index, ThemeTokens& outTokens, ThemeMetrics& outMetrics) {
    if (index < 0 || index >= themeCount()) index = 0;
    const Theme t = themeAt(index);
    outTokens = t.tok;
    outMetrics = t.met;
}

void themeSwatch(int index, ImVec4& outBg, ImVec4& outAccent) {
    if (index < 0 || index >= themeCount()) index = 0;
    const Theme t = themeAt(index);
    // 窗口底不在 tokens 里（它是 ImGuiCol_WindowBg），所以这里仍然走
    // themeAt；两处都调一次 themeAt 的代价可以忽略（一共 10 套、只在
    // Preferences 里画色卡时调）。
    outBg = t.windowBg;
    outAccent = t.accent;
}

void applyTheme(int index) {
    if (index < 0) index = 0;
    if (index >= themeCount()) index = themeCount() - 1;
    g_current = index;

    const Theme t = themeAt(index);

    // ⚠ 没有 ImGui 上下文时（`--selftest theme` 在建窗口之前跑）只更新
    //   数据，不碰 ImGuiStyle —— 那里面每一个 StyleColors* / GetStyle()
    //   都会在 `GImGui == nullptr` 时直接断言崩掉。
    //   这条分支让"换主题"这件事在没有设备/没有窗口时也能被验证，
    //   代价是一次指针比较。
    if (ImGui::GetCurrentContext() == nullptr) {
        g_tokens = t.tok;
        g_metrics = t.met;
        return;
    }

    // 底：深色主题从 StyleColorsDark 出发、浅色从 StyleColorsLight 出发。
    // 理由：我们只显式覆盖了 ~45 个槽位，剩下那些（TabDimmed / TreeLines /
    // ModalWindowDimBg …）跟着底走才自洽。出厂主题走 Dark 分支 = 改动前
    // 的行为逐字段不变。
    if (t.tok.isLight)
        ImGui::StyleColorsLight();
    else
        ImGui::StyleColorsDark();

    ImGuiStyle& s = ImGui::GetStyle();
    applyBaseMetrics(s, t.met);

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = t.text;
    c[ImGuiCol_TextDisabled] = t.textDisabled;

    c[ImGuiCol_WindowBg] = t.windowBg;
    c[ImGuiCol_ChildBg] = t.childBg;
    c[ImGuiCol_PopupBg] = t.popupBg;
    c[ImGuiCol_Border] = t.border;
    c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

    c[ImGuiCol_FrameBg] = t.frameBg;
    c[ImGuiCol_FrameBgHovered] = t.frameBgHovered;
    c[ImGuiCol_FrameBgActive] = t.frameBgActive;

    // ---- 标题栏（用户第 2 条）----
    // 激活态掺强调色 → 不再是"灰底 + 白字"的呆板条；未激活压回 panelBg
    // 一档，让"哪个面板是活的"一眼能看出来。
    c[ImGuiCol_TitleBg] = t.tok.titleBar;
    c[ImGuiCol_TitleBgActive] = t.tok.titleBarActive;
    c[ImGuiCol_TitleBgCollapsed] = t.tok.titleBar;
    c[ImGuiCol_MenuBarBg] = t.menuBarBg;

    c[ImGuiCol_ScrollbarBg] = t.scrollbarBg;
    c[ImGuiCol_ScrollbarGrab] = t.scrollGrab;
    c[ImGuiCol_ScrollbarGrabHovered] = t.scrollGrabHovered;
    c[ImGuiCol_ScrollbarGrabActive] = t.scrollGrabActive;

    c[ImGuiCol_CheckMark] = t.accent;
    c[ImGuiCol_SliderGrab] = t.sliderGrab;
    c[ImGuiCol_SliderGrabActive] = t.sliderGrabActive;

    c[ImGuiCol_Button] = t.button;
    c[ImGuiCol_ButtonHovered] = t.buttonHovered;
    c[ImGuiCol_ButtonActive] = t.buttonActive;

    c[ImGuiCol_Header] = t.header;
    c[ImGuiCol_HeaderHovered] = t.headerHovered;
    c[ImGuiCol_HeaderActive] = t.headerActive;

    c[ImGuiCol_Separator] = t.separator;
    c[ImGuiCol_SeparatorHovered] = t.separatorHovered;
    c[ImGuiCol_SeparatorActive] = t.accent;

    c[ImGuiCol_ResizeGrip] = t.resizeGrip;
    c[ImGuiCol_ResizeGripHovered] = t.resizeGripHovered;
    c[ImGuiCol_ResizeGripActive] = t.accent;

    c[ImGuiCol_Tab] = t.tab;
    c[ImGuiCol_TabHovered] = t.tabHovered;
    c[ImGuiCol_TabSelected] = t.tabSelected;
    c[ImGuiCol_TabSelectedOverline] = t.accent;
    c[ImGuiCol_TabDimmed] = t.tab;
    c[ImGuiCol_TabDimmedSelected] = t.tabSelected;
    c[ImGuiCol_TabDimmedSelectedOverline] = t.accent;

    c[ImGuiCol_PlotLines] = t.plotLines;
    c[ImGuiCol_PlotHistogram] = t.accent;

    c[ImGuiCol_TextSelectedBg] = t.textSelectedBg;
    c[ImGuiCol_NavCursor] = t.accent;
    c[ImGuiCol_DragDropTarget] = ImVec4(1.00f, 0.78f, 0.20f, 0.90f);
    c[ImGuiCol_TableHeaderBg] = t.tableHeaderBg;
    c[ImGuiCol_TableBorderStrong] = t.tableBorderStrong;
    c[ImGuiCol_TableBorderLight] = t.tableBorderLight;
    c[ImGuiCol_TableRowBgAlt] = t.tableRowBgAlt;

    g_tokens = t.tok;
    g_metrics = t.met;
}

} // namespace ui
