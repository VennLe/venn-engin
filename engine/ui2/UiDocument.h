#pragma once
// ============================================================
// ui2/UiDocument —— 游戏 UI 的**资产格式**（#312）
//
// 在此之前，"界面上有什么"只存在于 C++ 里：`runtime/ui/Hud.cpp` 手写血条
// 坐标、`PauseMenu.cpp` 手写三个按钮、`Subtitle.cpp` 手写字幕框。作者
// （策划 / 界面美术）能调的只有 `config/ui.json` 的 6 个数字。
// 这个文件把"界面长什么样"变成一份 `.ui.json`：控件树 + 锚点 + 九宫格
// + 文本 + 数据绑定。编辑器（`editor/UiEditorWindow`）改它，运行时
// （`runtime`）画它，两边读的是**同一个** `parseUiDocument`。
//
// ------------------------------------------------------------
// 五条"省了就错"的设计约束
// ------------------------------------------------------------
// ⚠⚠ ① **校验只有一份**。编辑器的"检查"永远是"`toJson` → 丢回
//     `parseUiDocument`"，绝不自己再写一套。两套规则的后果不是"多写代码"，
//     而是"编辑器说没问题，进 Play 一片空白"，且两边都觉得自己有理。
//     （与 `DialogueAsset` / `SequenceAsset` 同一条纪律。）
//
// ⚠⚠ ② **子控件的锚点是相对父矩形的**，不是相对屏幕。
//     血条面板带着三个子控件，面板被作者挪到右下角时，`x=8` 的子控件
//     必须跟着走。若所有控件都按屏幕锚点算，作者每次调整面板位置都要
//     重算每个子控件 —— 那是"用编辑器反而更慢"。
//     ⚠ 代价是"同一份锚点语义要有两个入口"（`placeRect` 屏幕 /
//       `placeRectIn` 父矩形）。所以 `placeRectIn` 实现放在 `UiLayout.cpp`
//       里紧挨着 `placeRect`，两处**共用**同一套符号约定，不各写一份。
//
// ⚠⚠ ③ **九宫格是"程序化"的，不是贴图九宫格**。
//     ui2 的管线只绑**一张**纹理，而那张是字体图集（R8 距离场）。想贴
//     图标就得先有多页图集（U1 的后续）。在此之前，`nine` 控件画的是
//     "边框 + 填充 + 圆角"—— 这正是二次元 UI 里 90% 的用处（对话框、
//     按钮框、血条槽），而且零素材依赖、任意分辨率都锐利。
//     `image` 控件仍然保留 `uv`：它画的是**同一张字体图集里**的一个矩形
//     （当图标用的应急手段），文档里明说，不假装有图标图集。
//
// ⚠⚠ ④ **绘制是纯 CPU 的**（`drawDocument` 只吃 `UiCanvas`）。于是
//     "控件树画出来几个图元 / 矩形在哪 / 绑定取到几"能在**无窗口、无
//     Vulkan** 的自检里断言（`Ui2SelfTest` 的 `uidoc_*` 组）。像素留给
//     截图验收 —— 与本项目一贯的分层（数值先可测）一致。
//
// ⚠⚠ ⑤ **没有配置 = 一个像素都不画**。`runtime` 只在 `game.json`/env 指定
//     了文档时才走这条路；不指定时画面与 #311 之后**逐位相同**。这是
//     "新功能默认关"的守卫，靠调用点结构保证，不靠 if 记得写。
//
// ------------------------------------------------------------
// 坐标口径
// ------------------------------------------------------------
//   `rect: [x, y, w, h]` 全部是**参考分辨率（默认 1920×1080）下的像素**，
//   运行时统一乘 `computeScale(outW, outH)`。锚点语义与 `UiPlacement`
//   一字不差：左锚 x 是"左边到锚点的距离"，右锚 x 是"右边让出的距离"，
//   中锚 x 是"中心相对锚点的偏移"。
// ============================================================

#include "ui2/UiCanvas.h"
#include "ui2/UiLayout.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ui2 {

struct SdfFont;

// 控件种类。**新增一种 = 改 5 处**：这里、`typeName`/`typeFromName`、
// `toJson` 的写出、`placeDocument` 的几何、`drawDocument` 的画笔。
// 少改一处的症状是"存回去类型变成了 panel"（静默降级，最难受的一种）。
enum class UiWidgetType : int {
    Panel = 0,     // 圆角实心块（面板底 / 遮罩 / 分隔条）
    Nine = 1,      // 九宫格框（边框 + 填充 + 圆角）
    Image = 2,     // 图集里的一个矩形（uv）
    Text = 3,      // 文本（可带 {key} 绑定）
    Button = 4,    // 九宫格框 + 居中文字
    Progress = 5,  // 进度条（槽 + 按绑定值缩放的填充）
    Count = 6,
};

const char* typeName(UiWidgetType t);
bool typeFromName(const std::string& s, UiWidgetType& out);

const char* anchorName(UiAnchor a);
bool anchorFromName(const std::string& s, UiAnchor& out);

struct UiWidget {
    std::string id;                       // 必填；同级内唯一
    UiWidgetType type = UiWidgetType::Panel;
    UiAnchor anchor = UiAnchor::TopLeft;
    float x = 0.0f, y = 0.0f;
    float w = 100.0f, h = 40.0f;

    // 主色：面板底 / 边框 / 文字色 / 按钮底 / 进度填充
    uint32_t color = rgbA(0xFFFFFF, 255);
    // 次色：九宫格与按钮的**填充**色、进度条的槽色
    uint32_t color2 = rgbA(0x000000, 160);
    float radius = 0.0f;      // 圆角（参考像素）
    float inset = 0.0f;       // 九宫格边框宽度（参考像素）
    bool visible = true;

    std::string text;         // Text / Button 的正文，可含 {key}
    float size = 20.0f;       // 字号（参考像素）
    int align = 0;            // 0 左 / 1 中 / 2 右

    // -- Progress --
    // `bind` 为空 → 用 `value`（0..1，静值，设计期预览用）。
    // `bind` 非空 → 用绑定值；**键缺失按 0 画**（空槽），不是满值 ——
    //   "数据没来"看起来像"满血"是最容易骗过验收的一种假象。
    std::string bind;
    std::string bindMax;      // 上限键（空 = 1.0）；缺失同样按 0 算
    float value = 1.0f;

    // -- Image --
    float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;

    std::vector<UiWidget> children;
};

struct UiDocument {
    std::string name;
    int refW = 1920;
    int refH = 1080;
    std::string comment;               // JSON 的 "//"（作者的说明，原样往返）
    std::vector<UiWidget> widgets;     // 顶层（锚点是相对屏幕）

    // 解析结果。`valid == false` 时 `widgets` 里是**残片**（边解析边填），
    // 调用方不许拿它落盘（见编辑器"读盘失败 → 只读"）。
    bool valid = false;
    std::string error;
    std::vector<std::string> warnings;
};

// ---------------------------------------------------------------- 读 / 写

// `sourcePath` 只用于错误信息里的定位。
bool parseUiDocument(const std::string& json, const std::string& sourcePath,
                     UiDocument& out);
bool loadUiDocument(const std::string& path, UiDocument& out);
std::string toJson(const UiDocument& d);
// 临时文件 + rename（与 `saveDialogue` 同一条理由：写到一半崩掉不能留半份）。
bool saveUiDocument(const std::string& path, const UiDocument& d,
                    std::string& err);

// ---------------------------------------------------------------- 树工具

// 按路径找（`hud/hpbar`，顶层也接受裸 id）。找不到返回 nullptr。
UiWidget* findWidget(UiDocument& d, const std::string& path);
const UiWidget* findWidget(const UiDocument& d, const std::string& path);
// 全树控件数（不含自身不计：根层每个算 1）
int countWidgets(const std::vector<UiWidget>& ws);
// 全树 id 是否唯一（含跨层）。重复时 `dup` 拿到第一个重复的 id。
bool idsUnique(const std::vector<UiWidget>& ws, std::string* dup);
// 从树里摘掉一个节点（连同子树）。返回是否摘到。
bool detachWidget(UiDocument& d, const std::string& path, UiWidget* out);
// 按路径把节点插到父节点（或顶层）的 `index` 处（<0 = 末尾）。
bool attachWidget(UiDocument& d, const std::string& parentPath,
                  const UiWidget& w, int index);

// ---------------------------------------------------------------- 数据绑定
//
// 谁是权威：**运行时**。脚本 `setFlag("affection", 42)` → `GameFlags` →
// 这里的一张表 → 控件的 `{affection}` / `bind`。UI 不认识脚本引擎，
// 也不认识 ECS —— 它只认识"一个字符串 → 一个数"的查表（与 `HudModel`
// 的分层理由相同：UI 可以在无场景的情况下用假数据自检）。
struct UiBindings {
    std::unordered_map<std::string, float> numbers;
    std::unordered_map<std::string, std::string> strings;

    void setNumber(const std::string& key, float v) { numbers[key] = v; }
    void setText(const std::string& key, const std::string& v) { strings[key] = v; }
    float number(const std::string& key, float def) const;
    std::string text(const std::string& key, const std::string& def) const;
    void clear() { numbers.clear(); strings.clear(); }
};

// 把 `"HP {health}/{maxHealth}"` 里的 `{key}` 换成绑定值。
//   · 整数（|v - round(v)| < 1e-3）按整数写，否则保留 1 位小数
//   · **查不到的 key 原样保留 `{key}`** —— 静默变空会让作者以为"绑定
//     没生效"，而屏幕上留一个 `{maxHelth}` 一眼就能看出是拼错了
std::string formatBound(const std::string& tpl, const UiBindings& b);

// ---------------------------------------------------------------- 布局 / 绘制

// 一个控件算完的矩形（自动化与编辑器共用）。
struct UiDocPlaced {
    std::string id;
    UiWidgetType type = UiWidgetType::Panel;
    UiRect rect;        // 输出像素
    UiRect fill;        // Progress 的填充矩形（其它类型与 rect 相同）
};

// 纯几何：输出像素下每个控件的矩形，**深度优先**（父在前、子在后）。
// `outW/outH` 为 0 → 返回空表（与 `placeRect` 的零守卫同源）。
std::vector<UiDocPlaced> placeDocument(const UiDocument& d,
                                       const UiBindings& b, uint32_t outW,
                                       uint32_t outH);

// 显式给画布矩形与缩放。
//
// ⚠ 给**编辑器设计画布**用的：那里的缩放由面板自己算（`canvasW / refW`），
//   与 `ui.scale` 无关。用 `placeDocument` 的话，作者在 Inspector 里调了
//   全局 UI 缩放，设计视图里的控件会跟着动 —— 那不是"设计稿"。
//   `scale = 1` 时返回的就是**参考像素**坐标（换锚点保位置靠它算）。
std::vector<UiDocPlaced> placeDocumentAt(const UiDocument& d,
                                         const UiBindings& b,
                                         const UiRect& canvas, float scale);

struct UiDocDrawStats {
    int widgets = 0;   // 画出来的
    int skipped = 0;   // visible=false 或因尺寸为 0 跳过的
    int texts = 0;     // 画了几段文字
};

// 往画布上画整份文档。`font` 为空时**不画文字但其它照画**（与
// `UiCanvas::text` 的"字体缺失只是数据缺失"同一条：不画方块）。
UiDocDrawStats drawDocument(UiCanvas& c, const UiDocument& d,
                            const UiBindings& b, uint32_t outW, uint32_t outH,
                            const SdfFont* font);

}  // namespace ui2
