// ============================================================
// engine/ui/ImGuiTheme.cpp —— 主题表 + 应用
// ============================================================

#include "ui/ImGuiTheme.h"

#include <algorithm>

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

// ---------------------------------------------------------------- 出厂主题
// ⚠ 这一段是**逐字段照抄**改动前的 applyVennStyle()（含所有中间值）。
//   任何"顺手调到更好看"的改动都会让默认主题下的像素断言失去基准。
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
    return t;
}

// ---------------------------------------------------------------- 派生
// 深色主题只给三个锚点（底色 / 强调色 / 文字色），其余由明度关系推出。
// 这样加一套新风格只要写一行，而且不会出现"某个槽忘了改、新主题里还是
// 上一套的颜色"这种低级事故。
Theme deriveDark(const ImVec4& bg, const ImVec4& accent, const ImVec4& text) {
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
    return t;
}

// 浅色主题：底色亮、边框比底色暗，"悬停/激活"是**加深**而不是提亮 ——
// 直接把深色那套规则反过来用会让浅色面板上的控件全都发白、看不见。
Theme deriveLight(const ImVec4& bg, const ImVec4& accent,
                  const ImVec4& text) {
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
    return t;
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
    case ThemeId::VennDark:
    default:
        return themeVennDark();
    }
}

const char* kThemeNames[] = {
    "Venn Dark",     // VennDark
    "UE Dark",       // UeDark
    "Blender Dark",  // BlenderDark
    "Unity Dark",    // UnityDark
    "Daylight",      // Daylight
};

// 尺寸度量：**所有主题共用**。改动这里 = 改版式 = 一批像素断言要重算。
void applyBaseMetrics(ImGuiStyle& s) {
    s.WindowPadding = ImVec2(10.0f, 8.0f);
    s.FramePadding = ImVec2(9.0f, 5.0f);   // 默认 3 → 控件更"厚"，不挤
    s.ItemSpacing = ImVec2(9.0f, 7.0f);    // 默认 8,4 → 行间透气
    s.ItemInnerSpacing = ImVec2(7.0f, 5.0f);
    s.CellPadding = ImVec2(7.0f, 5.0f);
    s.IndentSpacing = 20.0f;
    s.ScrollbarSize = 13.0f;
    s.GrabMinSize = 11.0f;

    s.WindowBorderSize = 1.0f;
    s.ChildBorderSize = 1.0f;
    s.PopupBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f;
    s.TabBorderSize = 0.0f;

    s.WindowRounding = 4.0f;
    s.ChildRounding = 4.0f;
    s.FrameRounding = 4.0f;
    s.PopupRounding = 4.0f;
    s.ScrollbarRounding = 6.0f;
    s.GrabRounding = 4.0f;
    s.TabRounding = 4.0f;

    s.WindowTitleAlign = ImVec2(0.02f, 0.5f);
    s.WindowMenuButtonPosition = ImGuiDir_None;   // 面板标题左边不留折叠三角
    s.SeparatorTextBorderSize = 1.0f;
    s.SeparatorTextPadding = ImVec2(14.0f, 4.0f);
}

int g_current = 0;
ThemeTokens g_tokens;

} // namespace

int themeCount() { return static_cast<int>(ThemeId::Count); }

const char* themeName(int index) {
    if (index < 0 || index >= themeCount()) index = 0;
    return kThemeNames[index];
}

int currentThemeIndex() { return g_current; }

const ThemeTokens& tokens() { return g_tokens; }

void applyTheme(int index) {
    if (index < 0) index = 0;
    if (index >= themeCount()) index = themeCount() - 1;
    g_current = index;

    const Theme t = themeAt(index);
    // 底：深色主题从 StyleColorsDark 出发、浅色从 StyleColorsLight 出发。
    // 理由：我们只显式覆盖了 ~45 个槽位，剩下那些（TabDimmed / TreeLines /
    // ModalWindowDimBg …）跟着底走才自洽。出厂主题走 Dark 分支 = 改动前
    // 的行为逐字段不变。
    if (t.tok.isLight)
        ImGui::StyleColorsLight();
    else
        ImGui::StyleColorsDark();

    ImGuiStyle& s = ImGui::GetStyle();
    applyBaseMetrics(s);

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

    c[ImGuiCol_TitleBg] = t.titleBg;
    c[ImGuiCol_TitleBgActive] = t.titleBgActive;
    c[ImGuiCol_TitleBgCollapsed] = t.titleBg;
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
}

} // namespace ui
