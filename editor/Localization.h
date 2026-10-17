#pragma once
// ============================================================
// editor/Localization —— 编辑器界面语言（中 / 英）
//
// 设计前提（用户明确要求）：
//   · **代码里只写英文**。所有界面文字的原文本就是英文，`tr()` 负责在
//     运行时把它替换成当前语言的显示文本。加一条译文 = 在映射表里加一行，
//     不需要动调用点，更不会让"中文"渗进逻辑分支。
//   · **映射替换，不是双份文案**。没有译文的字符串原样返回英文 —— 所以
//     缺译文只会"某处还是英文"，绝不会变成空白或乱码。
//   · **语言切换不碰标识符**。枚举值、命令 id、落盘的 key、着色器关键字、
//     日志 tag 一律保持英文；翻译只发生在"要画给人看的那一层"。
//
// 用法：
//     #include "Localization.h"
//     ImGui::Button(TR("Save"))                     // 按钮
//     ImGui::Text(TR("Entities: %zu"), n);          // 带格式参数
//     ImGui::MenuItem(TR("View"), nullptr, false);  // 菜单
//
// ⚠ 两条纪律：
//   1) `tr()` 的入参必须是**字符串字面量**（映射表按指针/内容查表都行，
//      但字面量才能保证"英文原文"永远可查）。别拼接字符串再传进来。
//   2) **格式串的 % 占位符在译文里必须一模一样**（数量、顺序、类型）。
//      翻错了会在运行时把格式化输出搞乱 —— 这是这类映射表最容易踩的坑。
// ============================================================

#include <string>

namespace editor {

enum class Language : int {
    English = 0,
    Chinese = 1,
    Count
};

// 语言个数 / 自名（"English" / "中文" —— 这两种写法不随界面语言变化，
// 语言选择器里就该用它自己的文字，这是通行做法）
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
// ⚠ 返回的指针由内部缓存持有（key = stableId），**不要**把它存下来
//   跨语言切换使用。
const char* trId(const char* english, const char* stableId);

// 便捷：语言是中文吗（个别地方要分支，比如调字体宽度）
bool isChinese();

} // namespace editor

// 调用点的统一入口。用宏而不是函数名，是为了让调用点短到不碍眼。
#define TR(s) (::editor::tr(s))
