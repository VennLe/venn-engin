#pragma once
// ============================================================
// ui2/UiLayout —— 锚点布局（U4）
//
// 所有"设计尺寸"都写在**参考分辨率**（Ui2Settings::kRefWidth/Height，1920×1080）
// 里，运行时统一乘 scale = min(outW/1920, outH/1080) × ui.scale。
//
// 为什么不是"直接按屏幕像素写死"：
//   · 1280×720 与 1920×1080 上同一个 HUD 的位置会差 1.5 倍 —— 写死就是错的；
//   · "等比乘 scale + min 钳高度"保证超宽屏不被横向拉扁（竖屏同理）。
//
// ⚠ 布局**不做像素取整**：取整留给最后写顶点那一步。否则 1280/1920 之间的
//   等比断言会被 round 抹平到整数，测出来是"差 1px"，其实是取整而不是布局错。
//   （验收里那 ±1px 的余量是给"最终取整"留的，不是给布局留的。）
// ============================================================

#include <cstdint>

#include "ui2/Ui2Settings.h"

namespace ui2 {

struct UiRect {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;

    float right() const { return x + w; }
    float bottom() const { return y + h; }
    float cx() const { return x + w * 0.5f; }
    float cy() const { return y + h * 0.5f; }

    // 半开区间：右下边界**不算**命中（相邻两个按钮贴在一起时不会双命中）。
    bool contains(float px, float py) const {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
};

enum class UiAnchor : int {
    TopLeft = 0, TopCenter, TopRight,
    MiddleLeft, Center, MiddleRight,
    BottomLeft, BottomCenter, BottomRight,
};

// 设计空间（参考分辨率）里的一个元素。
// 语义：元素的**支点**贴在锚点上，x/y 是相对锚点的偏移（参考像素）。
//   左锚：盒子左边在 anchor.x + x；右锚：盒子右边在 anchor.x - x；
//   中锚：盒子中心在 anchor.x + x。纵向同理。
struct UiPlacement {
    UiAnchor anchor = UiAnchor::TopLeft;
    float x = 0.0f, y = 0.0f;
    float w = 0.0f, h = 0.0f;
};

// 参考像素 → 输出像素。scale 显式传入（自检要能钉具体倍率）。
UiRect placeRect(const UiPlacement& p, uint32_t outW, uint32_t outH,
                 float scale);

// 常用重载：scale 走 computeScale（受 ui.scale 影响）。
UiRect placeRect(const UiPlacement& p, uint32_t outW, uint32_t outH);

}  // namespace ui2
