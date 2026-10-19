#pragma once
// ============================================================
// editor/Localization —— 编辑器界面语言（中 / 英）**转发壳**
//
// ⚠ 2026-10-18（U7）：词典与查表逻辑已搬到 `engine/i18n/`。
//    为什么要搬：运行时（Game.exe）也要显示中文，而 i18n 只要还住在
//    editor/ 里，`runtime → engine ← editor` 这条单向依赖就断了。
//    搬完之后**词典只有一份**（`engine/i18n/ZhTable.cpp`）。
//
// 这个头文件现在只剩三种东西：
//   1) 转发声明（`tr` / `setLanguage` / `language` …）—— 调用点
//      `TR()` 一个都没改，所以 700 多处的 diff 是 0；
//   2) 两个**编辑器专属**包装：
//        · `trId()`    —— `"译文###稳定ID"`，服务 ImGui 的窗口 ID；
//        · `trLabel()` —— `"译文##stableId"`，服务控件 label 兼 ID。
//      它们都依赖 ImGui 的 ID 语义，天然是编辑器概念，不该进引擎；
//   3) `TrItems<N>`（Combo 选项数组的一次性翻译拷贝）。
//
// 设计前提（未变，用户明确要求）：
//   · **代码里只写英文**。`tr()` 在显示层替换；缺译文只会"某处还是英文"，
//     不会空白、不会乱码。
//   · **语言切换不碰标识符**：枚举值、命令 id、落盘 key、日志 tag、
//     着色器关键字一律英文。
//
// ⚠ 两条纪律（仍然全部有效）：
//   1) `tr()` 的入参必须是**字符串字面量**（英文原文要永远可查）。
//   2) **格式串的 % 占位符在译文里必须一模一样**（数量、顺序、类型）。
//      翻错会在运行时把格式化输出搞乱 —— `verify_i18n_coverage.py`
//      的 F 组专治这一条。
// ============================================================

#include "i18n/I18n.h"

#include <string>

namespace editor {

// 语言枚举与引擎共用一套值（落盘的 language 下标就是它）。
using Language = i18n::Language;

// 语言个数 / 自名（"English" / "中文" —— 不随界面语言变化）
int languageCount();
const char* languageName(int index);

// 设置 / 查询当前语言（越界会被夹到合法范围）
void setLanguage(int index);
int language();

// 英文原文 → 当前语言的显示文本。没有译文时原样返回英文。
const char* tr(const char* english);

// 给"兼作 ImGui 窗口 ID"的标题用：返回 "<译文>###<stableId>"。
//
// 为什么需要它：ImGui 窗口名同时是**窗口 ID**。直接把标题翻成中文会让
// ID 跟着语言变 —— 换一次语言，imgui.ini 里的位置、面板最大化状态、
// 分栏里认窗口的地方全部对不上。加上 `###` 之后显示的是译文、ID 永远是
// 那个稳定的英文串。
//
// ⚠ 返回的指针由内部缓存持有（key = 语言 + stableId），**不要**把它存下来
//   跨语言切换使用。
const char* trId(const char* english, const char* stableId);

// 控件 label 专用：`"Duration##fx"` → `"持续时间##fx"`。
//
// 场景：大量 ImGui 控件写成 `SliderFloat("Duration##fx", ...)`，`##` 之后
// 那半截是**稳定 ID**（为了同名控件不打架、为了 Tooltip 不显示后缀）。
// 直接 `TR("Duration##fx")` 是查不到译文的（原文带后缀），而只翻前半截又
// 很容易漏掉后缀 → 控件 ID 跟着语言变 → 同一窗口里两个控件撞 ID 或
// 自动化锚点失配。
//
// 这个函数负责：按第一个 `##` 切开 → 只翻前半截 → 后缀原样接回。
// 没有 `##` 时等价于 `tr()`。返回指针由内部缓存持有。
//
// ⚠ 同一个英文原文在不同控件里可能带不同后缀（"Radius" 与 "Radius##fx"），
//   这是**对的**：译文取同一个 key，后缀各自保留。
const char* trLabel(const char* englishLabel);

// 便捷：语言是中文吗（个别地方要分支，比如调字体宽度）
bool isChinese();

} // namespace editor

// 调用点的统一入口。用宏而不是函数名，是为了让调用点短到不碍眼。
#define TR(s) (::editor::tr(s))
// 控件 label（可能带 `##stableId` 后缀）。
#define TRL(s) (::editor::trLabel(s))

namespace editor {

// 把一组"英文选项名"按当前语言翻好，供 `ImGui::Combo` / `ListBox` 一类接口用：
//
//     static const char* kTypes[] = {"Static", "Kinematic", "Dynamic"};
//     ImGui::Combo(TR("Type"), &t, TrItems<3>(kTypes), 3);
//
// 为什么需要它：Combo 要的是**连续的 const char* 数组**，没法逐个 `TR()`；
// 而选项数组往往写成文件级/函数内 static 常量 —— 那是"英文原文"的存放处，
// 不该被就地改成译文（换语言要能换回来）。所以在这里做一层一次性拷贝。
//
// ⚠ 这是**临时对象**，只在同一个完整表达式里有效（ImGui 只在本调用内读
//   items，所以安全）；**不要**把它赋给变量存着用。
// ⚠ N 必须与真实元素数一致 —— 写小了会漏翻最后一项，写大了会越界读。
template <int N>
class TrItems {
public:
    explicit TrItems(const char* const* src) {
        for (int i = 0; i < N; ++i) v_[i] = tr(src[i]);
    }
    operator const char* const*() const { return v_; }

private:
    const char* v_[N];
};

} // namespace editor
