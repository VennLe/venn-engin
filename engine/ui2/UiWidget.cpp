#include "ui2/UiWidget.h"

#include "core/Logger.h"
#include "ui2/SdfFont.h"
#include "ui2/Ui2Settings.h"

#include <algorithm>

namespace ui2 {

bool hitTest(const UiButton& b, float px, float py, uint32_t outW,
             uint32_t outH) {
    if (!b.enabled) return false;
    const UiRect r = placeRect(b.frame, outW, outH);
    return r.contains(px, py);
}

void drawButton(UiCanvas& c, UiButton& b, uint32_t outW, uint32_t outH,
                const SdfFont* font) {
    const float s = computeScale(outW, outH);
    if (s <= 0.0f) return;
    const UiRect r = placeRect(b.frame, outW, outH, s);
    if (r.w <= 0.0f || r.h <= 0.0f) return;

    // 配色优先级：禁用 > 按住 > 悬停 > 常态。
    // （按住的视觉必须压过悬停，否则"按下"看起来和"悬停"一模一样。）
    uint32_t col = b.style.idle;
    if (!b.enabled)      col = b.style.idle;
    else if (b.held)     col = b.style.press;
    else if (b.hovered)  col = b.style.hover;

    c.roundRect(r.x, r.y, r.w, r.h, b.style.radius * s, col);

    if (!font || !font->valid() || b.label.empty()) return;

    // 字号：跟随 ui.fontSize，但被按钮高度约束住（矮按钮上的大字会溢出，
    // 那看起来像"文字位置错了"，其实是尺寸没约束）。
    const float base = settings().fontSize * 2.0f * s;
    const float size = std::max(8.0f, std::min(r.h * 0.45f, base));
    const float tw = c.measure(*font, b.label.c_str(), size);
    const float baseline =
        r.cy() + (font->ascent - font->descent) * 0.5f * (size / font->size);
    c.text(*font, r.cx() - tw * 0.5f, baseline, b.label.c_str(),
           b.style.label, size);
}

DebugButton& debugButton() {
    static DebugButton d;
    return d;
}

namespace {
bool g_pending = false;
bool g_useRef = false;
float g_px = 0.0f;
float g_py = 0.0f;
}

void requestClick(float px, float py) {
    g_pending = true;
    g_useRef = false;
    g_px = px;
    g_py = py;
}

void requestClickRef(float rx, float ry) {
    g_pending = true;
    g_useRef = true;
    g_px = rx;
    g_py = ry;
}

void processPendingClick(uint32_t outW, uint32_t outH) {
    if (!g_pending) return;
    g_pending = false;

    // 参考像素 → 输出像素（与 placeRect 同一条换算，**不另写一份**）
    float px = g_px;
    float py = g_py;
    const float s = computeScale(outW, outH);
    if (g_useRef && s > 0.0f) {
        px *= s;
        py *= s;
    }

    DebugButton& d = debugButton();
    const bool hit = d.enabled && hitTest(d.button, px, py, outW, outH);
    if (hit) {
        d.pressFrames = 2;   // 本帧 + 下一帧，保证截图能撞上
        d.button.held = true;
    }
    VK_LOG_INFO("UI2-CLICK hit=%d x=%.1f y=%.1f ref=%d out=%ux%u",
                hit ? 1 : 0, static_cast<double>(px), static_cast<double>(py),
                g_useRef ? 1 : 0, static_cast<unsigned>(outW),
                static_cast<unsigned>(outH));
}

}  // namespace ui2
