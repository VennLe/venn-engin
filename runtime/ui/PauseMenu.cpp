#include "PauseMenu.h"

#include "i18n/I18n.h"
#include "ui2/SdfFont.h"
#include "ui2/Ui2Settings.h"
#include "ui2/UiWidget.h"

namespace runtime {

namespace {

// 三个按钮的位置 —— **唯一来源**。
// focusItems() / build() / 命中测试全部调它，不许各自再写一套像素公式：
// 两套公式之间的偏差就是"看得见的地方点不中"。
ui2::UiPlacement buttonPlacement(int i) {
    ui2::UiPlacement p;
    p.anchor = ui2::UiAnchor::Center;
    p.w = 320.0f;
    p.h = 64.0f;
    // 第一项在中心上方，往下每项 72（64 高 + 8 间距）
    p.y = -60.0f + 72.0f * static_cast<float>(i);
    return p;
}

ui2::UiPlacement panelPlacement() {
    ui2::UiPlacement p;
    p.anchor = ui2::UiAnchor::Center;
    p.w = 480.0f;
    p.h = 400.0f;
    p.x = 0.0f;
    p.y = 0.0f;
    return p;
}

}  // namespace

PauseMenu::PauseMenu() {
    // 顺序 = Tab 的顺序（几何上也正好是上→下）。
    m_items = {
        {"resume", "Resume", true},
        {"settings", "Settings", true},
        {"quit", "Quit", true},
    };
}

const char* PauseMenu::title() { return i18n::tr("PAUSED"); }

ui2::UiRect PauseMenu::panelRect(uint32_t outW, uint32_t outH) const {
    return ui2::placeRect(panelPlacement(), outW, outH);
}

std::vector<ui2::UiFocusItem> PauseMenu::focusItems(uint32_t outW,
                                                    uint32_t outH) const {
    std::vector<ui2::UiFocusItem> out;
    out.reserve(m_items.size());
    for (std::size_t i = 0; i < m_items.size(); ++i) {
        ui2::UiFocusItem it;
        it.id = m_items[i].id;
        it.enabled = m_items[i].enabled;
        it.rect = ui2::placeRect(buttonPlacement(static_cast<int>(i)), outW,
                                 outH);
        out.push_back(std::move(it));
    }
    return out;
}

void PauseMenu::build(ui2::UiCanvas& c, uint32_t outW, uint32_t outH,
                      const ui2::SdfFont* font,
                      const std::string& focusedId) const {
    const float s = ui2::computeScale(outW, outH);
    if (s <= 0.0f || outW == 0 || outH == 0) return;

    // ---- 半透明黑底（alpha 0.6 ≈ 153）----
    // 刻意**不模糊**：模糊需要把已渲染的场景抓成一张 RT 再采样，
    // 那是另一个 pass + 一份额外显存，先不做（路线图 U8 写明了）。
    c.rect(0.0f, 0.0f, static_cast<float>(outW), static_cast<float>(outH),
           ui2::rgba(0, 0, 0, 153));

    // ---- 面板 ----
    const ui2::UiRect pr = panelRect(outW, outH);
    if (pr.w <= 0.0f || pr.h <= 0.0f) return;
    c.roundRect(pr.x, pr.y, pr.w, pr.h, 14.0f * s, ui2::rgbA(0x14171B, 235));

    // ---- 标题 ----
    if (font && font->valid()) {
        const float size = 34.0f * s;
        const char* t = title();
        const float tw = c.measure(*font, t, size);
        const float baseline = pr.y + 74.0f * s;
        c.text(*font, pr.cx() - tw * 0.5f, baseline, t,
               ui2::rgbA(0xE6EDF3, 255), size);
    }

    // ---- 按钮 ----
    for (std::size_t i = 0; i < m_items.size(); ++i) {
        const Item& item = m_items[i];
        ui2::UiButton b;
        b.id = item.id;
        b.label = i18n::tr(item.label.c_str());
        b.frame = buttonPlacement(static_cast<int>(i));
        b.enabled = item.enabled;
        // 焦点态画成"悬停"配色：键盘导航时玩家必须看得见选中的是哪一项。
        b.hovered = (item.id == focusedId);
        b.held = false;
        ui2::drawButton(c, b, outW, outH, font);
    }
}

}  // namespace runtime
