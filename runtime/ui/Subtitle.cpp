// ============================================================
// runtime/ui/Subtitle.cpp
// ============================================================

#include "Subtitle.h"

#include "core/Logger.h"
#include "ui2/SdfFont.h"
#include "ui2/Ui2Settings.h"

#include <algorithm>
#include <cstdio>

namespace runtime {

namespace {

// ---- 设计尺寸（参考分辨率 1920×1080，统一乘 scale）----
constexpr float kBoxW = 1240.0f;
constexpr float kBoxBottom = 96.0f;   // 底边距屏幕底（见文件头 ①）
constexpr float kPadX = 30.0f;
constexpr float kPadY = 20.0f;
constexpr float kSpeakerH = 32.0f;
constexpr float kTextLineH = 36.0f;
constexpr float kHintH = 30.0f;
constexpr float kChoiceH = 36.0f;
constexpr float kChoiceGap = 6.0f;
constexpr float kSpeakerSize = 22.0f;
constexpr float kTextSize = 26.0f;
constexpr float kChoiceSize = 22.0f;

// 配色（sRGB，与 Hud 同一口径：交换链是 sRGB，这里写设计稿上的值）
uint32_t colBack() { return ui2::rgba(10, 12, 16, 214); }
uint32_t colHairline() { return ui2::rgba(255, 255, 255, 34); }
uint32_t colSpeaker() { return ui2::rgba(255, 214, 120, 255); }
uint32_t colText() { return ui2::rgba(238, 242, 248, 255); }
uint32_t colHint() { return ui2::rgba(200, 208, 220, 210); }
uint32_t colChoiceIdle() { return ui2::rgba(206, 214, 226, 255); }
uint32_t colChoiceFocus() { return ui2::rgba(255, 232, 160, 255); }
uint32_t colChoiceBg() { return ui2::rgba(255, 255, 255, 30); }

int lineCount(const std::string& s) {
    int n = 1;
    for (const char c : s)
        if (c == '\n') ++n;
    return n;
}

ui2::UiPlacement boxPlacement(float hRef) {
    ui2::UiPlacement p;
    p.anchor = ui2::UiAnchor::BottomCenter;
    p.x = 0.0f;
    p.y = kBoxBottom;
    p.w = kBoxW;
    p.h = hRef;
    return p;
}

// 框高（参考像素）。**所有高度都从这里算**，draw 不再自己推第二遍 ——
// 两处各算一次，迟早会出现"文字溢出框外 3 像素"这种没人发现的分歧。
float boxHeightRef(const SubtitleModel& m) {
    const int lines = lineCount(m.text);
    const float choices = m.atChoice
                              ? kChoiceH * static_cast<float>(m.choices.size()) +
                                    kChoiceGap
                              : kHintH;
    return kPadY * 2.0f + kSpeakerH + kTextLineH * static_cast<float>(lines) +
           choices;
}

} // namespace

ui2::UiRect SubtitleBox::boxRect(const SubtitleModel& m, uint32_t outW,
                                 uint32_t outH) const {
    return ui2::placeRect(boxPlacement(boxHeightRef(m)), outW, outH);
}

ui2::UiRect SubtitleBox::choiceRect(const SubtitleModel& m, int i, uint32_t outW,
                                    uint32_t outH) const {
    const ui2::UiRect b = boxRect(m, outW, outH);
    const float s = ui2::computeScale(outW, outH);
    const int lines = lineCount(m.text);
    // 选项接在正文下面（框内）。y 从框顶往下：padY + speakerH + 行高×行数。
    const float top = b.y + (kPadY + kSpeakerH +
                             kTextLineH * static_cast<float>(lines) +
                             kChoiceGap) *
                                s;
    return ui2::UiRect{b.x + kPadX * s, top + kChoiceH * s * static_cast<float>(i),
                       b.w - kPadX * 2.0f * s, kChoiceH * s};
}

void SubtitleBox::build(ui2::UiCanvas& c, const SubtitleModel& m, uint32_t outW,
                        uint32_t outH, const ui2::SdfFont* font) const {
    if (!m.visible || outW == 0 || outH == 0) return;
    const float s = ui2::computeScale(outW, outH);
    if (s <= 0.0f) return;

    const ui2::UiRect box = boxRect(m, outW, outH);
    if (box.w <= 0.0f || box.h <= 0.0f) return;

    // ---- 背板 + 顶部一条细线 ----
    // 圆角 6：字幕框用**小圆角**而不是血条那种胶囊，胶囊在长条上会显得
    // 两端"被吃掉"一块文字。
    c.roundRect(box.x, box.y, box.w, box.h, 6.0f * s, colBack());
    c.rect(box.x + kPadX * s, box.y + 1.5f * s, box.w - kPadX * 2.0f * s,
           1.5f * s, colHairline());

    const bool hasFont = font && font->valid();

    // ---- 说话人（名字在上，正文在下）----
    if (hasFont && !m.speaker.empty()) {
        const float size = kSpeakerSize * s;
        c.text(*font, box.x + kPadX * s, box.y + kPadY * s + size * 0.85f,
               m.speaker.c_str(), colSpeaker(), size);
    }

    // ---- 正文 ----
    if (hasFont && !m.text.empty()) {
        const float size = kTextSize * s;
        const float baseline = box.y + (kPadY + kSpeakerH + kTextSize * 0.85f) * s;
        c.text(*font, box.x + kPadX * s, baseline, m.text.c_str(), colText(),
               size);
    }

    // ---- 选项 / 提示 ----
    if (m.atChoice) {
        const float size = kChoiceSize * s;
        for (int i = 0; i < static_cast<int>(m.choices.size()); ++i) {
            const ui2::UiRect r = choiceRect(m, i, outW, outH);
            const bool focus = (i == m.focus);
            if (focus)
                c.roundRect(r.x, r.y, r.w, r.h, 4.0f * s, colChoiceBg());
            if (hasFont) {
                // 左侧留一个箭头位：焦点在哪一项要**一眼看出来**，
                // 只靠颜色在色弱/低对比显示器上是不够的。
                const char* label = m.choices[static_cast<size_t>(i)].c_str();
                if (focus)
                    c.text(*font, r.x + 10.0f * s,
                           r.y + r.h * 0.5f + size * 0.36f, ">", colChoiceFocus(),
                           size);
                c.text(*font, r.x + 30.0f * s,
                       r.y + r.h * 0.5f + size * 0.36f, label,
                       focus ? colChoiceFocus() : colChoiceIdle(), size);
            }
        }
    } else if (!m.typing && hasFont) {
        // 出完了才提示"按空格继续"。打字中提示会让人以为"没出完也能跳"。
        const float size = kHintH * 0.6f * s;
        const char* hint = u8"\u25bd";   // ▽
        const float tw = c.measure(*font, hint, size);
        c.text(*font, box.right() - kPadX * s - tw, box.bottom() - 8.0f * s,
               hint, colHint(), size);
    }

    // ---- 日志锚点 ----
    ++m_frames;

    // 布局类：只在变化时打（每帧打会在 400 fps 下把日志淹掉，而"框在哪"
    // 这件事一帧只可能变一次）。
    char key[128];
    std::snprintf(key, sizeof(key), "%d|%zu|%d|%d", m.visible ? 1 : 0,
                  m.choices.size(), m.focus, lineCount(m.text));
    if (m_lastLayoutKey != key) {
        m_lastLayoutKey = key;
        VK_LOG_INFO("SUB-BOX x=%.1f y=%.1f w=%.1f h=%.1f lines=%d choices=%d "
                    "speaker=%s",
                    static_cast<double>(box.x), static_cast<double>(box.y),
                    static_cast<double>(box.w), static_cast<double>(box.h),
                    lineCount(m.text), static_cast<int>(m.choices.size()),
                    m.speaker.empty() ? "-" : m.speaker.c_str());
        for (int i = 0; i < static_cast<int>(m.choices.size()); ++i) {
            const ui2::UiRect r = choiceRect(m, i, outW, outH);
            VK_LOG_INFO("SUB-CHOICE i=%d focus=%d rect=%.1f,%.1f,%.1f,%.1f "
                        "text=[%s]",
                        i, i == m.focus ? 1 : 0, static_cast<double>(r.x),
                        static_cast<double>(r.y), static_cast<double>(r.w),
                        static_cast<double>(r.h),
                        m.choices[static_cast<size_t>(i)].c_str());
        }
    }

    // 打字机：每 30 帧一行，证明 `shown` 在涨（"字幕一动不动"和"字幕出完了"
    // 在没有这行日志时长得完全一样）。
    if (m.typing && (m_frames % 30u) == 0u) {
        // ⚠ 两个数都是**码点**，不是 `m.text.size()`（字节）——
        //   按字节报会让"中文一行 8 个字"显示成 24/24，看起来永远出完了。
        VK_LOG_INFO("SUB-TYPE id=%s shown=%d total=%d", m.id.c_str(),
                    m.shownChars, m.totalChars);
    }
}

} // namespace runtime
