// ============================================================
// engine/ui/UiChrome.cpp —— 装饰件实现（见头文件里的纪律）
// ============================================================

#include "ui/UiChrome.h"

#include "ui/ImGuiTheme.h"

#include <algorithm>
#include <cmath>

namespace ui {
namespace {

ImVec4 shiftC(const ImVec4& c, float d) {
    return ImVec4(std::clamp(c.x + d, 0.0f, 1.0f),
                  std::clamp(c.y + d, 0.0f, 1.0f),
                  std::clamp(c.z + d, 0.0f, 1.0f), c.w);
}

ImVec4 mixC(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                  a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

ImU32 u32(const ImVec4& c) { return ImGui::GetColorU32(c); }

} // namespace

void gradientRectV(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 top, ImU32 bot,
                   float rounding) {
    if (!dl || b.x <= a.x || b.y <= a.y) return;

    if (rounding <= 0.5f) {
        dl->AddRectFilledMultiColor(a, b, top, top, bot, bot);
        return;
    }

    // 圆角处理：先铺一整块 bottom 色的圆角矩形（这就是"底部纯色"），
    // 再把渐变画在上半部分。渐变区只到 y = b.y - 1，避免盖住底部的圆角。
    dl->AddRectFilled(a, b, bot, rounding);
    const float cut = std::min(rounding, (b.y - a.y) * 0.5f);
    if (b.y - cut > a.y + 1.0f) {
        dl->PushClipRect(a, ImVec2(b.x, b.y - cut), true);
        dl->AddRectFilledMultiColor(a, b, top, top, bot, bot);
        dl->PopClipRect();
        // 渐变到纯色的接缝用一条同色细线糊一下，否则会出现一条 1px 暗线
        dl->AddLine(ImVec2(a.x, b.y - cut), ImVec2(b.x, b.y - cut), bot, 1.0f);
    }
}

void cardBackground(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding,
                    bool hot, float glow) {
    if (!dl) return;
    const ThemeTokens& t = tokens();

    if (glow > 0.0f) {
        // 外发光：往外扩 2px 画一圈半透明强调色。不做模糊（draw list 没有
        // 模糊），两层递减 alpha 就够看出"发亮"了。
        const ImVec4 g1 = t.glow;
        ImVec4 g2 = g1;
        g2.w *= 0.45f;
        const float r = rounding + 2.0f;
        dl->AddRectFilled(ImVec2(a.x - 2.0f, a.y - 2.0f),
                          ImVec2(b.x + 2.0f, b.y + 2.0f), u32(g2), r * 1.1f);
        dl->AddRect(ImVec2(a.x - 1.0f, a.y - 1.0f),
                    ImVec2(b.x + 1.0f, b.y + 1.0f), u32(g1), r,
                    ImDrawFlags_RoundCornersAll,
                    1.0f + 1.2f * std::min(glow, 1.0f));
    }

    const ImVec4 base = hot ? t.cardBgHover : t.cardBg;
    // 上亮下暗一点点：一块纯色卡片看起来是"贴纸"，有一点渐变才像"卡"。
    const ImVec4 top = t.isLight ? shiftC(base, 0.012f) : shiftC(base, 0.026f);
    const ImVec4 bot = t.isLight ? shiftC(base, -0.020f) : base;

    gradientRectV(dl, a, b, u32(top), u32(bot), rounding);
    dl->AddRect(a, b, u32(hot ? mixC(t.borderSoft, t.accent, 0.55f)
                              : t.borderSoft),
                rounding, ImDrawFlags_RoundCornersAll, 1.0f);
}

void headerBand(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding) {
    if (!dl) return;
    const ThemeTokens& t = tokens();

    // 水平渐变：左 = 掺强调色的 barBg，右 = barBg 本身。
    // 这样"标题从左边进来、右边留白"，比通栏纯色有方向感。
    // ⚠ 动漫深色档的 barBg 被压暗到 0.04 左右，`accent` 却仍然是 0.97 ——
    //   两边量级差 20 倍，混 0.24 会直接把顶栏变成"粉条"（第一版实测
    //   RGB≈138,85,113，比正文还亮）。收到 0.10 才是"微微透着强调色"。
    const ImVec4 left = mixC(t.barBg, t.accent, t.isLight ? 0.10f : 0.05f);
    const ImVec4 right = t.barBg;
    dl->AddRectFilledMultiColor(a, b, u32(left), u32(right), u32(right),
                                u32(left));
    if (rounding > 0.5f) {
        // 圆角的两端用底色补一下（AddRectFilledMultiColor 不支持圆角）
        dl->AddRectFilled(ImVec2(a.x, a.y), ImVec2(b.x, b.y), u32(t.barBg),
                          0.0f);
        dl->AddRectFilledMultiColor(ImVec2(a.x + rounding, a.y),
                                    ImVec2(b.x - rounding, b.y), u32(left),
                                    u32(right), u32(right), u32(left));
    }
    // 底部一条更亮的线 = 高光边
    dl->AddLine(ImVec2(a.x, b.y - 0.5f), ImVec2(b.x, b.y - 0.5f),
                u32(mixC(t.barBg, t.accent, 0.40f)), 1.0f);
}

bool pillButton(const char* id, const char* label, ImVec2 size, int kind) {
    const ThemeTokens& t = tokens();
    ImGuiStyle& st = ImGui::GetStyle();

    if (size.x <= 0.0f) {
        const ImVec2 ts = ImGui::CalcTextSize(label);
        size.x = ts.x + st.FramePadding.x * 2.0f + 12.0f;
    }
    if (size.y <= 0.0f) size.y = ImGui::GetFrameHeight() + 6.0f;

    ImGui::PushID(id);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##pill", size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();

    ImVec4 base, hover, text;
    switch (kind) {
    case 1:   // 主按钮
        base = t.accent;
        hover = shiftC(t.accent, 0.10f);
        text = t.isLight ? ImVec4(1, 1, 1, 1) : ImVec4(0.10f, 0.06f, 0.10f, 1);
        break;
    case 2:   // 危险
        base = t.error;
        hover = shiftC(t.error, 0.10f);
        text = ImVec4(1, 1, 1, 1);
        break;
    default:  // 普通
        base = t.panelBgAlt;
        hover = t.cardBgHover;
        text = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        break;
    }
    if (held) base = mixC(base, hover, 0.6f);

    const float r = size.y * 0.5f;   // 药丸：圆角 = 半高
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a = p;
    const ImVec2 b = ImVec2(p.x + size.x, p.y + size.y);

    // 底：上亮下暗。用 tokens 里的明暗关系，浅色主题下不会变成黑块。
    const ImVec4 topC = hovered ? shiftC(hover, 0.03f) : shiftC(base, 0.03f);
    const ImVec4 botC = hovered ? hover : shiftC(base, -0.03f);
    gradientRectV(dl, a, b, u32(topC), u32(botC), r);

    // 边框：主/危险按钮用自身亮一档，普通按钮用软边框
    const ImVec4 edge = (kind == 0) ? t.borderSoft : shiftC(base, 0.18f);
    dl->AddRect(a, b, u32(edge), r, ImDrawFlags_RoundCornersAll, 1.0f);

    // 悬停高光：顶部一条半透明白线（玻璃质感）
    if (hovered) {
        dl->AddLine(ImVec2(a.x + r, a.y + 1.0f),
                    ImVec2(b.x - r, a.y + 1.0f),
                    ImGui::GetColorU32(ImVec4(1, 1, 1, t.isLight ? 0.35f : 0.14f)),
                    1.0f);
    }

    // 文字居中
    const ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(std::floor(p.x + (size.x - ts.x) * 0.5f),
                       std::floor(p.y + (size.y - ts.y) * 0.5f)),
                u32(text), label);

    ImGui::PopID();
    return clicked;
}

void statusPill(const char* text, const ImVec4& color) {
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const float h = ts.y + 4.0f;
    const float w = ts.x + 14.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImVec4 bg = color;
    bg.w *= 0.22f;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), u32(bg), h * 0.5f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), u32(color), h * 0.5f,
                ImDrawFlags_RoundCornersAll, 1.0f);
    dl->AddText(ImVec2(p.x + 7.0f, p.y + 2.0f), u32(color), text);

    ImGui::Dummy(ImVec2(w, h));
}

} // namespace ui
