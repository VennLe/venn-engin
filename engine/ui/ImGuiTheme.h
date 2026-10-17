#pragma once
// ============================================================
// engine/ui/ImGuiTheme —— ImGui 界面主题（多风格调色板）
//
// 为什么单独成文件：
//   · 以前整套配色写死在 ImGuiManager.cpp 的匿名命名空间里
//     （applyVennStyle），只能在**启动时**应用一次。用户要的是
//     "Preferences 里随时换风格"，那就必须是一个可以被反复调用的
//     公开接口，而且要能被编辑器落盘。
//   · 主题不只是 ImGuiCol_* 那一堆槽位。面板里还有一批"不是 ImGuiCol
//     但必须跟着主题走"的颜色（Hierarchy/Content 的容器底色、状态色、
//     gizmo 轴色……）。那些走 ThemeTokens，见下。
//
// ⚠ 三条纪律：
//   1) **默认主题（VennDark）的每一个数值必须与改动前逐字段一致** ——
//      回归里有一批像素断言（面板矩形、底色阈值），默认主题一变，全部
//      都要重算。新增主题只能"加在下标后面"，绝不能改 VennDark 的值。
//   2) **落盘存的是整数下标**（见 editor 的偏好表）。所以要往枚举里插
//      新主题，只能追加在 Count 之前 —— 往中间插会静默换掉老配置的主题。
//   3) **尺寸（Padding / Spacing / Rounding）不属于主题**：所有主题共用
//      同一套度量，见 applyBaseMetrics()。理由同上 —— 版式一变，一批
//      面板矩形的像素断言会莫名其妙地红。
// ============================================================

#include <imgui.h>

namespace ui {

// 主题下标。顺序 = 界面下拉里的顺序。
enum class ThemeId : int {
    VennDark = 0,   // 出厂主题：深灰 + 蓝强调（对齐 UE5 的观感）
    UeDark,         // Unreal 5 编辑器风：近黑、冷蓝强调
    BlenderDark,    // Blender 风：中性中灰 + 橙色强调
    UnityDark,      // Unity 风：深青灰 + 亮青强调
    Daylight,       // 浅色：浅灰面板 + 深色文字
    Count
};

// 主题里的"语义色"。面板代码写死颜色时改成从这里取 —— 浅色主题下
// 写死的深色底会变成一块黑斑。
struct ThemeTokens {
    ImVec4 panelBg;     // 面板容器底（比 WindowBg 略深一档）
    ImVec4 panelBgAlt;  // 面板里的次级底（分组头 / 交替行）
    ImVec4 accent;      // 强调色（与 ImGuiCol_CheckMark 同源）
    ImVec4 accentSoft;  // 强调色的低饱和版（选中行底 / 高亮带）
    ImVec4 textDim;     // 次级文字（说明、单位）
    ImVec4 textFaint;   // 极淡（旁注、未激活图标）
    ImVec4 ok;          // 成功 / 运行中
    ImVec4 warn;        // 警告
    ImVec4 error;       // 错误
    ImVec4 info;        // 提示
    ImVec4 axisX;       // gizmo 轴 X（红）
    ImVec4 axisY;       // gizmo 轴 Y（绿）
    ImVec4 axisZ;       // gizmo 轴 Z（蓝）
    // 顶栏 / 工具条 / 视口下方传输条的底（与 ImGuiCol_MenuBarBg 同源）。
    // 这三处原来各自硬编码 `ImVec4(0.113,0.117,0.125)`，浅色主题下会留成
    // 几条深色孤岛（"只美化了面板，顶栏还是黑的"）→ 收敛成 token。
    ImVec4 barBg;
    bool isLight;       // 浅色主题？需要反转对比度的地方查它
};

// 主题个数 / 英文名（界面显示用 —— 语言层负责把名字换成中文）
int themeCount();
const char* themeName(int index);

// 立即把第 index 套主题应用到 ImGuiStyle（越界会被夹到合法范围）。
// 尺寸度量在内部先统一重置一次，所以反复调用是幂等的。
void applyTheme(int index);

// 当前生效的主题下标。
int currentThemeIndex();

// 当前主题的语义色。
const ThemeTokens& tokens();

} // namespace ui
