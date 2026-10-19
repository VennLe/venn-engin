#include "ui2/UiLayout.h"

namespace ui2 {

UiRect placeRect(const UiPlacement& p, uint32_t outW, uint32_t outH,
                 float scale) {
    UiRect r;
    // 窗口最小化 / 异常尺寸：返回空矩形（contains 永远 false），
    // 与 computeScale 的零守卫同源 —— 这里绝不能除零算出 NaN。
    if (outW == 0 || outH == 0 || scale <= 0.0f) return r;

    // 逻辑画布 = 输出尺寸 / scale。等比缩放时它就是"参考画布被拉伸后的等效
    // 尺寸"：1920×1080 下 = 1920×1080；超宽 2560×1080 下 = 2560×1080
    // （高度不变、宽度变宽 —— 所以右锚的 UI 真的贴到右边缘）。
    const float lw = static_cast<float>(outW) / scale;
    const float lh = static_cast<float>(outH) / scale;

    const bool left = p.anchor == UiAnchor::TopLeft ||
                      p.anchor == UiAnchor::MiddleLeft ||
                      p.anchor == UiAnchor::BottomLeft;
    const bool midX = p.anchor == UiAnchor::TopCenter ||
                      p.anchor == UiAnchor::Center ||
                      p.anchor == UiAnchor::BottomCenter;
    const bool top = p.anchor == UiAnchor::TopLeft ||
                     p.anchor == UiAnchor::TopCenter ||
                     p.anchor == UiAnchor::TopRight;
    const bool midY = p.anchor == UiAnchor::MiddleLeft ||
                      p.anchor == UiAnchor::Center ||
                      p.anchor == UiAnchor::MiddleRight;

    // ---- 横向：支点贴锚点 ----
    float x = 0.0f;
    if (left)      x = p.x;                        // 左边缘
    else if (midX) x = lw * 0.5f + p.x - p.w * 0.5f;  // 中心
    else           x = lw - p.x - p.w;             // 右边缘（往左让出宽度）

    // ---- 纵向：y 向下，与画布/顶点一致 ----
    float y = 0.0f;
    if (top)       y = p.y;
    else if (midY) y = lh * 0.5f + p.y - p.h * 0.5f;
    else           y = lh - p.y - p.h;

    // 回到输出像素
    r.x = x * scale;
    r.y = y * scale;
    r.w = p.w * scale;
    r.h = p.h * scale;
    return r;
}

UiRect placeRect(const UiPlacement& p, uint32_t outW, uint32_t outH) {
    return placeRect(p, outW, outH, computeScale(outW, outH));
}

}  // namespace ui2
