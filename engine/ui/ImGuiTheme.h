#pragma once
// ============================================================
// engine/ui/ImGuiTheme —— ImGui 界面主题（多风格调色板 + 每主题度量）
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
// ------------------------------------------------------------
// 2026-10-27：二次元风格改造（用户六条需求里的 2/3/5/6）
// ------------------------------------------------------------
// 用户原话：「定位是一款二次元风格的 3d 游戏引擎，那么就要有浓厚的二次元
// 可爱的风格」+「所有的窗口本身风格应该做成圆角边框，窗口顶部的白框显得
// 很呆板」+「窗体的主题颜色风格不要太单一硬朗了，可以多样化供用户选择」。
//
// 于是这一版做了三件事：
//   ① **圆角从"全局唯一"变成"每主题一份"**（ThemeMetrics）。原来注释里
//      写着"尺寸不属于主题"，那是为了保住像素断言；但圆角是**风格**，
//      而且改圆角只会动到窗口四个角的几十个像素，不动任何矩形 —— 所以
//      把它拆出来是安全的。padding / spacing 仍然全局共用（那个一动，
//      一批面板矩形的断言全都要重算）。
//   ② **新增 5 套动漫风格主题**（下标 5..9，追加在原有 5 套之后 ——
//      落盘存的是下标，往中间插会静默换掉老配置的主题）。
//   ③ **出厂默认换成 SakuraNight**（`kDefaultThemeIndex`）。老配置里存着
//      `theme=0` 的用户仍然得到 VennDark，新装用户直接看到樱花粉。
//
// ⚠ 三条纪律：
//   1) **下标 0~4 的每一套配色必须与改动前逐字段一致** ——
//      `tools/verify_theme_language.py` 按下标扫 5 套主题并判深/浅亮度，
//      还有一批像素断言。新增主题只能**追加**（见 ② ）。
//   2) **落盘存的是整数下标**（见 editor 的偏好表）。往枚举中间插新主题
//      会静默换掉老配置的主题 —— 只能追加在 Count 之前。
//   3) **padding / spacing 不属于主题**：所有主题共用同一套度量值，
//      见 applyBaseMetrics()。理由同上 —— 版式一变，一批面板矩形的像素
//      断言会莫名其妙地红。圆角与边框宽度属于 ThemeMetrics（见 ①）。
// ============================================================

#include <imgui.h>

namespace ui {

// 主题下标。顺序 = 界面下拉里的顺序。
// ⚠ 只能在 **Count 之前追加**，不许往中间插（落盘存下标）。
enum class ThemeId : int {
    VennDark = 0,      // 出厂主题（老）：深灰 + 蓝强调
    UeDark,            // Unreal 5 编辑器风：近黑、冷蓝强调
    BlenderDark,       // Blender 风：中性中灰 + 橙色强调
    UnityDark,         // Unity 风：深青灰 + 亮青强调
    Daylight,          // 浅色：浅灰面板 + 深色文字
    SakuraNight,       // ★ 新出厂：深紫黑 + 樱花粉（二次元主推）
    MatchaMint,        // 深墨绿 + 薄荷绿
    SunsetPop,         // 深葡萄紫 + 夕阳橘
    CyberNeon,         // 近黑藏青 + 霓虹青紫
    SakuraDay,         // 浅色奶油樱粉（白天用）
    Count
};

// 出厂默认主题。老配置里存着 theme=0 的用户不受影响（读盘会覆盖它）。
constexpr int kDefaultThemeIndex = static_cast<int>(ThemeId::SakuraNight);

// ---------------------------------------------------------------- 度量
// 圆角与边框宽度。这几个值**跟着主题走**：动漫风要圆，工程风要直。
// 与"版式"无关 —— padding / spacing / 行高全在 applyBaseMetrics 里写死一份，
// 所有主题共用，免得动一下就让面板矩形断言全红。
struct ThemeMetrics {
    float windowRounding = 4.0f;
    float childRounding = 4.0f;
    float frameRounding = 4.0f;
    float popupRounding = 4.0f;
    float scrollbarRounding = 6.0f;
    float grabRounding = 4.0f;
    float tabRounding = 4.0f;

    float windowBorder = 1.0f;
    float childBorder = 1.0f;
    float frameBorder = 0.0f;
    float popupBorder = 1.0f;

    // 控件的"厚度"（按钮 / 复选框高度增量）。动漫风要胖一点才可爱。
    // ⚠ 只用在小控件上，**不动 WindowPadding / ItemSpacing**。
    float framePadY = 5.0f;
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
    ImVec4 barBg;
    bool isLight;       // 浅色主题？需要反转对比度的地方查它

    // ---- 2026-10-27 新增（二次元风格改造）----
    // 全部由 finalize() 从上面那些派生，所以老主题一行都不用改就能拿到
    // 一套自洽的值。面板代码可以放心用：它们在每套主题下都有意义。
    ImVec4 cardBg;        // 卡片 / 分组容器底（比 panelBg 亮一档）
    ImVec4 cardBgHover;   // 卡片悬停（往强调色偏一点）
    ImVec4 borderSoft;    // 软边框（细、低对比。用来替代硬 1px 线）
    ImVec4 titleBar;      // 标题栏底（未激活）
    ImVec4 titleBarActive;// 标题栏底（激活）—— 掺了强调色，不再是"白框"
    ImVec4 glow;          // 强调光晕（已带 alpha，直接当填充色用）
    ImVec4 trackBg;       // 滑轨 / 进度槽底
    float radiusLg = 4.0f; // 大圆角（窗口）
    float radiusMd = 4.0f; // 中圆角（卡片 / 控件）
    float radiusSm = 4.0f; // 小圆角（标签 / 小徽章）
    bool isAnime = false;  // 动漫风：额外装饰（渐变条 / 光晕）才开
};

// 主题个数 / 英文名（界面显示用 —— 语言层负责把名字换成中文）
int themeCount();
const char* themeName(int index);

// 立即把第 index 套主题应用到 ImGuiStyle（越界会被夹到合法范围）。
// 尺寸度量在内部先统一重置一次，所以反复调用是幂等的。
void applyTheme(int index);

// 当前生效的主题下标。
int currentThemeIndex();

// 当前主题的语义色 / 度量。
const ThemeTokens& tokens();
const ThemeMetrics& metrics();

// 纯查询：取第 index 套主题的完整 tokens + 度量。
// **不改变当前主题、不碰 ImGuiStyle** —— 所以可以在没有 ImGui 上下文的
// 场合调用（`--selftest theme` 就是这么用的，它在建窗口之前跑）。
// 越界会夹到合法范围。
void themeTokensAt(int index, ThemeTokens& outTokens, ThemeMetrics& outMetrics);

// 取第 index 套主题的**两个代表色**（窗口底 / 强调色），供 Preferences
// 里的"主题色卡"用 —— 那一排小色块是整个设置页里唯一能让人一眼看懂
// "还有多少种风格"的东西，只给一个下拉名（"Sunset Pop"）是看不出来的。
// 越界会夹到合法范围。不改变当前主题。
void themeSwatch(int index, ImVec4& outBg, ImVec4& outAccent);

} // namespace ui
