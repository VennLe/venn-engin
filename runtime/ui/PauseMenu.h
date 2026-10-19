#pragma once
// ============================================================
// runtime/ui/PauseMenu —— 暂停菜单（U6 交互骨架 + U8 视觉）
//
// 三个按钮：继续 / 设置 / 退出。
//
// ------------------------------------------------------------
// 它**不碰状态栈**
// ------------------------------------------------------------
// 开关由宿主决定：Playing 里按 ESC → push Paused；Paused 里按 ESC → pop。
// 菜单自己只管"长什么样、怎么被点中"。
// 为什么这样分：GameStateStack.h 的约定是"迁移不在回调里直接改栈" ——
// 在 onUpdate 里 pop 自己会让正在遍历的 vector 失效。
//
// ------------------------------------------------------------
// 文案走 `i18n::tr()`，不是 `TR()`
// ------------------------------------------------------------
// `TR` 是编辑器宏（`::editor::tr`），而 runtime **不许**依赖 editor
// （分层：`runtime → engine ← editor`）。两边查的是同一张表
// （`engine/i18n/ZhTable.cpp`），只是入口不同。
//
// ------------------------------------------------------------
// 绘制与命中用**同一个**矩形
// ------------------------------------------------------------
// `panelRect()` / `focusItems()` / `build()` 三处都从 `UiPlacement` 走
// `placeRect()` 算，不许各自写一套像素公式 —— 否则会出现"看得见的地方
// 点不中"（这类 bug 在 1280×720 上尤其容易漏，因为坐标偏得不多）。
// ============================================================

#include "ui2/UiCanvas.h"
#include "ui2/UiFocus.h"
#include "ui2/UiLayout.h"

#include <string>
#include <vector>

namespace ui2 {
struct SdfFont;
}

namespace runtime {

class PauseMenu {
public:
    struct Item {
        // 稳定 id：**不许有空格**（`UI2-FOCUS id=` 按 `\S*` 解析）。
        std::string id;
        // 英文原文（显示时 i18n::tr → 中文）。
        std::string label;
        bool enabled = true;
    };

    PauseMenu();

    const std::vector<Item>& items() const { return m_items; }

    // 本帧的可聚焦项（输出像素）。焦点导航**只认**这个返回值。
    std::vector<ui2::UiFocusItem> focusItems(uint32_t outW, uint32_t outH) const;

    // 画：全屏半透明黑 + 标题 + 三个按钮。
    // `focusedId` 用来画焦点高亮（键盘导航时玩家得看得见选中的是哪一项）。
    // `font` 为空时只画色块（无字体不算错误，只是少字形）。
    void build(ui2::UiCanvas& c, uint32_t outW, uint32_t outH,
               const ui2::SdfFont* font, const std::string& focusedId) const;

    // 菜单面板矩形（输出像素）。点击在面板外 = 不算 UI 命中。
    ui2::UiRect panelRect(uint32_t outW, uint32_t outH) const;

    // 标题（i18n 之后）。单独暴露：HUD 的调试锚点/日志也要用。
    static const char* title();

private:
    std::vector<Item> m_items;
};

} // namespace runtime
