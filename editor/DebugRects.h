#pragma once
// ============================================================
// editor/DebugRects —— 一个小小的调试工具：把 ImGui 控件的真实
// 屏幕矩形打一行日志。
//
// 为什么需要它：
//   ImGui 的坐标就是**客户区物理像素**，和 PrintWindow 截出来的图 1:1。
//   自动化截图脚本（tools/_verify_*.py）因此可以直接用这些坐标去点控件，
//   不用在截图里"猜"按钮在哪 —— 猜坐标是踩过坑的（DPI 缩放 + 窗口移动
//   会让写死的坐标整体偏掉，而且偏了不报错，只是点不中，很难查）。
//
// 用法：设 MYVK_LOG_RECTS=1 启动，日志里就会出现形如
//   CB-RECT up=(8,485)-(41,518) center=(24,501)
//   的行。**只在矩形明显变化时**才打 —— 窗口刚起来时 ImGui 还没拿到
//   最终尺寸（GLFW 恢复上一次的窗口大小、脚本又会 SetWindowPos），
//   首帧那一次的坐标是错的；只有重打才能拿到 resize 之后的真值。
//   脚本侧按 tag 取**最后一条**即可。
// ============================================================

#include "core/Logger.h"

#include <cmath>
#include <cstdlib>
#include <string>
#include <unordered_map>

#include <imgui.h>

namespace editor {

inline void logRect(const char* tag, const ImVec2& a, const ImVec2& b) {
    if (!std::getenv("MYVK_LOG_RECTS")) return;

    struct Rect {
        float x0, y0, x1, y1;
    };
    static std::unordered_map<std::string, Rect> last;

    const auto it = last.find(tag);
    if (it != last.end()) {
        const Rect& r = it->second;
        constexpr float kEps = 4.0f;
        if (std::fabs(r.x0 - a.x) < kEps && std::fabs(r.y0 - a.y) < kEps &&
            std::fabs(r.x1 - b.x) < kEps && std::fabs(r.y1 - b.y) < kEps) {
            return;   // 没动，不刷屏
        }
    }
    last[tag] = Rect{a.x, a.y, b.x, b.y};
    VK_LOG_INFO("%s=(%.0f,%.0f)-(%.0f,%.0f) center=(%.0f,%.0f)", tag, a.x, a.y,
                b.x, b.y, (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
}

} // namespace editor
