#pragma once
// ============================================================
// ui2/UiWidget —— 最小控件集（U5）
//
// 只做一件事件里必须有的东西：**一个能被点中的按钮**。不是"通用控件库"：
// 血条 / 准星 / 伤害数字都是"画什么"（U8），不是"怎么交互"。
//
// 为什么先做按钮而不是先做 HUD：
//   按钮是唯一同时需要 **布局 + 文本 + 命中测试 + 视觉状态** 的控件 ——
//   它一通过，四件设施就都验证过了；而血条只有布局 + 几何，做十个也证明不了
//   点击链路是通的。
//
// ⚠ 状态语义（三态 + 禁用，别混）：
//     hovered = 指针悬停；held = 按住；enabled = false 时**永远不命中**
//     （禁用还能命中 → "点了没反应"会伪装成"点了有反应但看不出来"）。
// ============================================================

#include "ui2/UiCanvas.h"
#include "ui2/UiLayout.h"

#include <string>

namespace ui2 {

struct UiButtonStyle {
    uint32_t idle = rgbA(0x2A2E36, 230);
    uint32_t hover = rgbA(0x3A404B, 240);
    uint32_t press = rgbA(0x14171B, 250);
    uint32_t label = rgbA(0xFFFFFF, 255);
    float radius = 6.0f;   // 参考像素
};

struct UiButton {
    std::string id;
    std::string label;
    UiPlacement frame;
    UiButtonStyle style;
    bool enabled = true;
    bool hovered = false;
    bool held = false;
};

// 命中测试（输入是**输出像素**）。禁用按钮不命中。
bool hitTest(const UiButton& b, float px, float py, uint32_t outW,
             uint32_t outH);

// 画一个按钮：背景圆角矩形 + 居中文字。font 为空时只画背景。
void drawButton(UiCanvas& c, UiButton& b, uint32_t outW, uint32_t outH,
                const SdfFont* font);

// ---- 调试按钮（控制台 `ui.button` / `ui.click` 的操作对象）----
struct DebugButton {
    bool enabled = false;
    UiButton button;
    int pressFrames = 0;   // >0 时按"按下"配色画 —— 让自动化能观察到一帧按下
};
DebugButton& debugButton();

// 模拟一次点击（输出像素）。**排队**，由 buildOverlay 在下一帧消费。
//
// ⚠ 为什么必须排队：控制台命令在**首帧之前**执行，那时还没有输出尺寸、
//   也没算过布局 —— 当场做命中测试必然 miss，而且"miss"会被误读成
//   "按钮没画上"。排队之后，判定发生在"布局刚算完"的那一刻，测的才是
//   真实链路。
void requestClick(float px, float py);

// 参考像素口径的点击（与 `ui.button`、布局同一坐标系）。
// 自动化用这个：脚本不必先知道视口多大就能点中"设计上那个位置"。
void requestClickRef(float rx, float ry);

// 消费待处理点击（buildOverlay 在画完按钮之后调用）。命中则 pressFrames=2，
// 并打 `UI2-CLICK hit=0/1` —— 命中与未命中**都要有日志**，否则"点歪了"
// 和"按钮没画"分不清。
void processPendingClick(uint32_t outW, uint32_t outH);

}  // namespace ui2
