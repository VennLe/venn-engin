#pragma once
// ============================================================
// ui2/UiFocus —— 焦点栈 + 方向键导航 + 输入消费（U6）
//
// ------------------------------------------------------------
// 为什么导航要按**几何**而不是数组顺序
// ------------------------------------------------------------
// 数组顺序是"代码里 push 的顺序"，与玩家看到的排布无关。三按钮菜单里
// 两者恰好一致，所以按数组顺序也能"看起来对"；一旦面板改成两列
// （继续/设置 在上，退出 在下且偏右），按 ↓ 就会从"继续"跳到"退出"，
// 玩家的直觉是"往下走一格"，结果是"跳到右下角"。这不是输入坏了，
// 是导航模型错了。
//
// 几何最近邻的定义（就三条，别加第四条）：
//   1) 候选必须在按键方向的**半平面**内（沿该方向位移 > 1px）；
//   2) 打分 = 沿方向的距离 + 2 × 侧向偏移 —— 侧向偏移罚 2 倍，
//      这样在垂直列表里按 ↓ / ↑ 不会"拐到旁边那一列"；
//   3) 该方向没有候选 → **回绕**到反方向最远的那个（到底再按一次回顶部）。
//
// ------------------------------------------------------------
// 为什么是**栈**
// ------------------------------------------------------------
// 单个 id 表达不了"输入框里再弹一个下拉"：关掉下拉要回到输入框，
// 而不是回到"没有焦点"。现在只有一层（暂停菜单），但接口按栈设计 ——
// 将来加层不需要改调用点。
//
// ------------------------------------------------------------
// 消费语义（`consumed`）
// ------------------------------------------------------------
// route() 返回 true = **这条输入归 UI**，调用方不该再拿它驱动玩家。
// ⚠ 顺序铁律：`route()` 必须在 `PlayerController::update()` **之前**。
//   放在后面的话，玩家的移动已经用掉了这一帧的 W/S —— 暂停菜单里按方向键
//   角色会先走一步再被拦住，"一个像素都不动"就永远不成立。
//
// ⚠ 锚点 `UI2-FOCUS id=<id>` **只在焦点变化时**打。每帧都打会让回归脚本
//   的 `grep | tail -1` 读到"最后一帧"，看起来永远正确（这也是一种假绿）。
// ⚠ id 里**不能有空格**（脚本按 `\S*` 解析）。菜单项 id 用
//   `resume` / `settings` / `quit` 这种单词。
// ============================================================

#include "ui2/UiLayout.h"

#include <string>
#include <vector>

namespace ui2 {

// 一个可聚焦的项。`rect` 是**输出像素**（布局层已经算完）。
struct UiFocusItem {
    std::string id;
    UiRect rect;
    bool enabled = true;
};

// 一帧的导航意图（由宿主从 InputMap / Input 合成，UiFocus 不认键码）。
struct UiNav {
    int moveX = 0;   // -1 左 / +1 右
    int moveY = 0;   // -1 上 / +1 下
    bool tab = false;
    bool shiftTab = false;
    bool activate = false;   // 回车 / 空格 / 鼠标左键点中
};

class UiFocusStack {
public:
    // 每帧开始：给出本帧**可见且可聚焦**的项。
    // 顺序随意（导航靠几何）；Tab 的顺序用这个数组的顺序。
    void beginFrame(std::vector<UiFocusItem> items);
    void endFrame() { m_items.clear(); }

    // ---- 查询 ----
    const std::string& focusId() const;
    int focusIndex() const { return m_index; }
    bool hasFocus() const { return m_index >= 0; }
    const std::vector<UiFocusItem>& items() const { return m_items; }

    // ---- 焦点 ----
    // 按 id 设焦点（菜单打开时设第一项、鼠标悬停时跟过去）。
    // 返回 true = 焦点**变了**（内部据此打锚点）。
    bool setFocus(const std::string& id);
    void clearFocus();
    // 把焦点放到几何上最靠左上的一项（菜单刚打开时的默认项）。
    bool focusFirst();

    // ---- 导航 ----
    bool move(int dx, int dy);
    bool cycle(bool backward);

    // 指针悬停（输出像素）。命中某个可聚焦项 → 焦点跟过去。
    bool hoverAt(float px, float py);

    // ---- 路由 ----
    // 消费本帧导航意图。`activatedId` 非空时写入被激活项的 id。
    bool route(const UiNav& nav, std::string* activatedId);

    // 单位置上的项（命中测试，输出像素）。没有返回 -1。
    int hitIndex(float px, float py) const;

private:
    int indexOf(const std::string& id) const;
    // 从 from 沿 (dx,dy) 找最近的项。dx/dy 至少一个非 0。
    int nearest(int from, int dx, int dy) const;
    // 焦点变化后打 `UI2-FOCUS`（只在真的变了时打）
    void logFocusIfChanged();

    std::vector<UiFocusItem> m_items;
    int m_index = -1;
    // 上次打出去的 id（**不是** m_index）：项的顺序每帧都可能变，
    // 比 id 才能区分"焦点没动"与"下标没动但项换了"。
    std::string m_lastLogged;
    // focusId() 返回空串时的稳定引用
    static const std::string kEmpty;
};

}  // namespace ui2
