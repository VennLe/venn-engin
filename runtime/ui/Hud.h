#pragma once
// ============================================================
// runtime/ui/Hud —— 血条 / 准星 / 伤害数字（U8）
//
// 只画**数据已经算好**的东西（HudModel）。逻辑（掉血、寿命、残影）都在
// HudModel 里，能无窗口断言；这里只做几何与投影。
//
// ------------------------------------------------------------
// 世界 → 屏幕：这个项目里 Y 只翻**一次**
// ------------------------------------------------------------
// `scene::Camera::projMatrix()` 里已经有 `proj[1][1] *= -1`（Vulkan 的
// 裁剪空间 Y 向下）。所以这里换算屏幕坐标时就是
//     sy = (ndc.y * 0.5 + 0.5) * outH
// **不要**再写一次 `1 - ...` —— 那会让数字上下镜像，而且"看着还在屏幕
// 附近"，最难发现。（路线图 §3.10 只写了三个坑，这是第四个。）
//
// ------------------------------------------------------------
// 三个坑（§3.10 原文，实现处逐条对应）
// ------------------------------------------------------------
//   1. `clip.w <= 0` → **整体丢弃**（不是 clamp）。背后点的 ndc 会翻转，
//      钳到边上会得到"一个永远贴在边框上的数字"。
//   2. 屏幕外 → 不画（第一版不做"贴边 + 箭头"）。
//   3. 遮挡 → 不做（UI pass 没有深度附件）。
// ============================================================

#include "HudModel.h"
#include "UiConfig.h"

#include "ui2/UiCanvas.h"
#include "ui2/UiLayout.h"

#include <glm/glm.hpp>

#include <cstdint>

namespace ui2 {
struct SdfFont;
}

namespace runtime {

class Hud {
public:
    // 外观配置（config/ui.json）。不设就是出厂默认 —— 与"配置功能存在
    // 之前"逐像素相同。
    void setConfig(const UiConfig& c) { m_cfg = c; }
    const UiConfig& config() const { return m_cfg; }

    // 画一帧。`viewProj` = projMatrix(aspect) * viewMatrix()（Y 已经翻过）。
    void build(ui2::UiCanvas& c, const HudModel& m, const glm::mat4& viewProj,
               uint32_t outW, uint32_t outH, const ui2::SdfFont* font) const;

    // ---- 几何（输出像素）。验收**量**它，不从截图猜坐标 ----
    // 血条底槽（读 m_cfg，所以不是 static）
    ui2::UiRect healthBarRect(uint32_t outW, uint32_t outH) const;
    // 血量填充（左对齐，宽度 = ratio × 底槽宽）
    ui2::UiRect healthFillRect(const HudModel& m, uint32_t outW,
                               uint32_t outH) const;

    // 世界 → 屏幕。返回 false = 相机背后 / 屏幕外（**调用方不画**）。
    static bool worldToScreen(const glm::mat4& viewProj, const glm::vec3& world,
                              uint32_t outW, uint32_t outH, glm::vec2* out);

private:
    UiConfig m_cfg;
    // HUD-STATS 节流（每 60 帧一行）。mutable：build 是 const，
    // 但"打日志"不改变画出来的东西。
    mutable unsigned m_frames = 0;
};

} // namespace runtime
