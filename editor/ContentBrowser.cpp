#include "ContentBrowser.h"

#include "Localization.h"
#include "DebugRects.h"
#include "EditorDragDrop.h"
#include "EditorIcons.h"
#include "EditorScene.h"

#include "assets/AssetManager.h"
#include "assets/AssetPath.h"
#include "assets/Texture.h"
#include "core/Logger.h"
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"

#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

// 导入资源要用系统的"打开文件"对话框。
// ⚠ 为什么要 `win32` 这一层而不是自己写一个 ImGui 文件浏览器：用户明确
//   要的是"本地资源管理器"—— 那意味着要能用窗口左侧的快速访问、网络位置、
//   搜索框、预览。自己做的简易浏览器一条都比不了，而且没有"最近的文件夹"。
// ⚠ NOMINMAX 已经在 CMake 里全局定义（windows.h 的 min/max 宏会和
//   std::max 打架），这里不用重复。
#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#endif

namespace fs = std::filesystem;

namespace editor {

namespace {

// ---------------------------------------------------------------- 类型判定

std::string lowerExt(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string e = name.substr(dot);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return e;
}

bool isImage(const std::string& name) {
    const std::string e = lowerExt(name);
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".bmp" ||
           e == ".tga";
}

bool isModel(const std::string& name) {
    const std::string e = lowerExt(name);
    return e == ".gltf" || e == ".glb" || e == ".obj" || e == ".fbx";
}

bool isScript(const std::string& name) { return lowerExt(name) == ".vks"; }

bool isScene(const std::string& name) { return lowerExt(name) == ".json"; }

// E13 着色器源码（类型过滤用）
bool isShader(const std::string& name) {
    const std::string e = lowerExt(name);
    return e == ".vert" || e == ".frag" || e == ".comp" || e == ".glsl" ||
           e == ".geom" || e == ".tesc" || e == ".tese" || e == ".spv";
}

std::string lowerCopy(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return r;
}

// E13 材质资产：引擎目前把材质内嵌在场景 JSON 里，独立材质文件用
// .mat.json / .vmat 约定（过滤器先认它，等独立材质落地即可直接用）。
bool isMaterial(const std::string& name) {
    if (lowerExt(name) == ".vmat") return true;
    const std::string lower = lowerCopy(name);
    const std::string suffix = ".mat.json";
    return lower.size() > suffix.size() &&
           lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool containsFold(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b));
                          });
    return it != hay.end();
}

// ------------------------------------------------- 条目右键菜单的 popup ID
//
// 必须是**常量字符串**，而且 OpenPopup / BeginPopup 两处要在**同一个 ID 栈
// 深度**上调用 —— 两边都放在 ##grid 子窗口里、drawEntry 的 PushID 之外。
// 否则 GetID() 算出来的哈希不同，BeginPopup 永远看不到刚打开的那个 popup
// （症状：菜单不出现，或者出现一帧就消失）。
constexpr const char* kEntryCtxPopup = "##cb_entry_ctx";

// 让格子矩形每帧最多记一次（见 drawGrid 里的用法）
int g_cellLogFrame = -1;

// 文件类型对应的强调色（画图标里那个小符号用的）
ImU32 accentColor(const std::string& name) {
    if (isImage(name)) return IM_COL32(96, 160, 224, 255);
    if (isModel(name)) return IM_COL32(104, 190, 132, 255);
    if (isScript(name)) return IM_COL32(198, 132, 214, 255);
    if (isScene(name)) return IM_COL32(232, 176, 96, 255);
    return IM_COL32(150, 156, 166, 255);
}

std::string humanSize(std::uintmax_t bytes) {
    char buf[32];
    if (bytes < 1024) {
        std::snprintf(buf, sizeof(buf), "%llu B",
                      static_cast<unsigned long long>(bytes));
    } else if (bytes < 1024ull * 1024ull) {
        std::snprintf(buf, sizeof(buf), "%.1f KB",
                      static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f MB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0));
    }
    return buf;
}

// UTF-8 安全的单行截断（超出宽度就加省略号）
std::string ellipsize(const std::string& text, float maxW) {
    if (ImGui::CalcTextSize(text.c_str()).x <= maxW) return text;
    std::string s = text;
    while (!s.empty()) {
        // 退到一个完整的 UTF-8 字符边界（否则 CalcTextSize 会算到半个字符）
        do {
            s.pop_back();
        } while (!s.empty() &&
                 (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80);
        if (ImGui::CalcTextSize((s + "...").c_str()).x <= maxW) break;
    }
    return s + "...";
}

// ---------------------------------------------------------------- 矢量图标
//
// 全部用 DrawList 现画（实现在 EditorIcons.h 里，Hierarchy 面板也在用）。

// glyph: 0=无 1=图片 2=模型 3=脚本 4=场景
void drawPageIcon(ImDrawList* dl, ImVec2 p, float s, ImU32 accent, int glyph) {
    const ImU32 paper = IM_COL32(228, 231, 237, 255);
    const ImU32 fold = IM_COL32(188, 194, 205, 255);
    const ImU32 edge = IM_COL32(140, 146, 158, 200);
    const float foldS = s * 0.34f;

    // 纸张（右上角切角）
    const ImVec2 pts[5] = {
        ImVec2(p.x, p.y),
        ImVec2(p.x + s - foldS, p.y),
        ImVec2(p.x + s, p.y + foldS),
        ImVec2(p.x + s, p.y + s),
        ImVec2(p.x, p.y + s),
    };
    dl->AddConvexPolyFilled(pts, 5, paper);
    // 折角
    dl->AddTriangleFilled(ImVec2(p.x + s - foldS, p.y), ImVec2(p.x + s, p.y + foldS),
                          ImVec2(p.x + s - foldS, p.y + foldS), fold);
    dl->AddPolyline(pts, 5, edge, ImDrawFlags_Closed, 1.0f);

    // 中央小符号
    const float cx = p.x + s * 0.5f;
    const float cy = p.y + s * 0.63f;
    const float u = s * 0.22f;   // 半个符号的尺寸

    if (glyph == 1) {  // 图片：小山 + 太阳
        dl->AddRect(ImVec2(cx - u, cy - u * 0.8f), ImVec2(cx + u, cy + u * 0.8f),
                    accent, 1.5f, 0, 1.6f);
        dl->AddCircleFilled(ImVec2(cx - u * 0.42f, cy - u * 0.38f), u * 0.22f,
                            accent);
        const ImVec2 tri[3] = {ImVec2(cx - u * 0.75f, cy + u * 0.6f),
                               ImVec2(cx - u * 0.10f, cy - u * 0.20f),
                               ImVec2(cx + u * 0.6f, cy + u * 0.6f)};
        dl->AddConvexPolyFilled(tri, 3, accent);
    } else if (glyph == 2) {  // 模型：等轴立方体
        const float w = u * 0.92f, h = u * 0.52f;
        const ImVec2 top(cx, cy - h * 1.7f), right(cx + w, cy - h * 0.75f);
        const ImVec2 bot(cx, cy + h * 0.2f), left(cx - w, cy - h * 0.75f);
        const ImVec2 lower(cx, cy + h * 1.15f);
        dl->AddQuad(top, right, bot, left, accent, 1.6f);
        dl->AddLine(left, bot, accent, 1.6f);
        dl->AddLine(right, bot, accent, 1.6f);
        dl->AddLine(bot, lower, accent, 1.6f);
    } else if (glyph == 3) {  // 脚本：< >
        const float k = u * 0.72f;
        dl->AddLine(ImVec2(cx - k * 0.35f, cy - k), ImVec2(cx - k * 1.05f, cy),
                    accent, 2.0f);
        dl->AddLine(ImVec2(cx - k * 1.05f, cy), ImVec2(cx - k * 0.35f, cy + k),
                    accent, 2.0f);
        dl->AddLine(ImVec2(cx + k * 0.35f, cy - k), ImVec2(cx + k * 1.05f, cy),
                    accent, 2.0f);
        dl->AddLine(ImVec2(cx + k * 1.05f, cy), ImVec2(cx + k * 0.35f, cy + k),
                    accent, 2.0f);
    } else if (glyph == 4) {  // 场景：窗口（标题条 + 内容）
        dl->AddRect(ImVec2(cx - u, cy - u * 0.8f), ImVec2(cx + u, cy + u * 0.8f),
                    accent, 1.5f, 0, 1.6f);
        dl->AddRectFilled(ImVec2(cx - u, cy - u * 0.8f),
                          ImVec2(cx + u, cy - u * 0.24f), accent, 1.5f,
                          ImDrawFlags_RoundCornersTop);
    }
}

int glyphFor(const std::string& name) {
    if (isImage(name)) return 1;
    if (isModel(name)) return 2;
    if (isScript(name)) return 3;
    if (isScene(name)) return 4;
    return 0;
}

// ---- 工具行里的小按钮：画一个自己的框 + 矢量图标 ----
using IconPainter =
    std::function<void(ImDrawList*, ImVec2, ImVec2, ImU32)>;

bool iconButton(const char* id, ImVec2 size, const IconPainter& paint,
                const char* tip, bool enabled = true) {
    ImGui::PushID(id);
    ImGui::BeginDisabled(!enabled);
    ImGui::InvisibleButton("##ib", size);
    const bool clicked = ImGui::IsItemClicked() && enabled;
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();

    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (enabled) {
        const ImU32 bg = held      ? IM_COL32(52, 96, 168, 255)
                         : hovered ? IM_COL32(46, 62, 92, 255)
                                   : IM_COL32(44, 47, 53, 255);
        dl->AddRectFilled(a, b, bg, 4.0f);
        dl->AddRect(a, b, IM_COL32(74, 79, 88, 200), 4.0f, 0, 1.0f);
    }
    paint(dl, a, b, enabled ? IM_COL32(222, 228, 236, 255)
                            : IM_COL32(120, 124, 132, 255));
    if (hovered && tip) ImGui::SetTooltip(TR("%s"), tip);
    ImGui::EndDisabled();
    ImGui::PopID();
    return clicked;
}

void paintUpArrow(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c) {
    const float cx = (a.x + b.x) * 0.5f;
    const float cy = (a.y + b.y) * 0.5f;
    const float h = 6.5f;
    dl->AddTriangleFilled(ImVec2(cx, cy - h), ImVec2(cx - h * 0.92f, cy + 0.5f),
                          ImVec2(cx + h * 0.92f, cy + 0.5f), c);
    dl->AddRectFilled(ImVec2(cx - 2.0f, cy + 0.5f), ImVec2(cx + 2.0f, cy + h),
                      c, 1.0f);
}

void paintHome(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c) {
    const float cx = (a.x + b.x) * 0.5f;
    const float cy = (a.y + b.y) * 0.5f;
    const float w = 7.0f, h = 6.5f;
    dl->AddTriangleFilled(ImVec2(cx, cy - h), ImVec2(cx - w, cy - h * 0.08f),
                          ImVec2(cx + w, cy - h * 0.08f), c);
    dl->AddRectFilled(ImVec2(cx - w * 0.72f, cy - h * 0.05f),
                      ImVec2(cx + w * 0.72f, cy + h * 0.85f), c, 1.0f);
    dl->AddRectFilled(ImVec2(cx - 2.0f, cy + 1.5f), ImVec2(cx + 2.0f, cy + h * 0.85f),
                      IM_COL32(40, 43, 49, 255), 1.0f);
}

void paintRefresh(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c) {
    // 注意：IM_PI 定义在 imgui_internal.h 里（imgui.h 不导出），这里直接用字面量
    constexpr float kPi = 3.14159265f;
    const ImVec2 ctr((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
    const float r = 6.0f;
    dl->PathArcTo(ctr, r, kPi * 0.35f, kPi * 1.85f, 24);
    dl->PathStroke(c, 0, 1.8f);
    // 箭头
    const ImVec2 tip(ctr.x + r * 0.92f, ctr.y - r * 0.42f);
    dl->AddTriangleFilled(ImVec2(tip.x + 2.6f, tip.y - 1.0f),
                          ImVec2(tip.x - 2.2f, tip.y - 3.0f),
                          ImVec2(tip.x - 0.2f, tip.y + 3.0f), c);
}

void paintPlus(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c) {
    const float cx = (a.x + b.x) * 0.5f;
    const float cy = (a.y + b.y) * 0.5f;
    const float k = 5.5f;
    dl->AddLine(ImVec2(cx - k, cy), ImVec2(cx + k, cy), c, 2.0f);
    dl->AddLine(ImVec2(cx, cy - k), ImVec2(cx, cy + k), c, 2.0f);
}

// E13 历史前进/后退箭头：**已随按钮一起删除**（2026-10-17）。
// 留个记号，免得以后有人照着旧截图又把它加回来 —— 见 drawNavRow 的注释。

// E13 网格 / 列表视图切换图标
void paintGridMode(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c) {
    const float cx = (a.x + b.x) * 0.5f;
    const float cy = (a.y + b.y) * 0.5f;
    const float s = 3.2f, gap = 1.4f;
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j)
            dl->AddRectFilled(
                ImVec2(cx - s - gap + i * (s + gap) * 1.0f,
                       cy - s - gap + j * (s + gap) * 1.0f),
                ImVec2(cx - gap + i * (s + gap) * 1.0f,
                       cy - gap + j * (s + gap) * 1.0f), c, 1.0f);
}

void paintListMode(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c) {
    const float cx = (a.x + b.x) * 0.5f;
    const float cy = (a.y + b.y) * 0.5f;
    const float w = 6.5f;
    for (int i = -1; i <= 1; ++i) {
        dl->AddRectFilled(ImVec2(cx - w, cy + i * 4.2f - 1.4f),
                          ImVec2(cx - w + 2.6f, cy + i * 4.2f + 1.4f), c, 0.5f);
        dl->AddLine(ImVec2(cx - w + 4.2f, cy + i * 4.2f),
                    ImVec2(cx + w, cy + i * 4.2f), c, 1.6f);
    }
}

// 排序图标：三根递减的横条 + 右侧一个向下箭头（"排序"的通用图形）。
// 三条横线从左到右变短 = "按长度排"，箭头 = 方向。
void paintSortIcon(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c) {
    const float cx = (a.x + b.x) * 0.5f;
    const float cy = (a.y + b.y) * 0.5f;
    const float left = cx - 7.0f;
    const float lens[3] = {9.0f, 6.5f, 4.0f};
    for (int i = 0; i < 3; ++i) {
        const float y = cy - 4.6f + i * 4.0f;
        dl->AddLine(ImVec2(left, y), ImVec2(left + lens[i], y), c, 1.6f);
    }
    const float ax = cx + 3.6f;
    dl->AddLine(ImVec2(ax, cy - 5.2f), ImVec2(ax, cy + 4.0f), c, 1.6f);
    dl->AddTriangleFilled(ImVec2(ax - 2.6f, cy + 3.2f),
                          ImVec2(ax + 2.6f, cy + 3.2f),
                          ImVec2(ax, cy + 6.6f), c);
}

// ------------------------------------------------- 动作条（Add / Save All）
//
// 2026-10-17 晚 用户需求：右侧内容区的搜索栏**上面**再加一行按钮。
// 这一行有两个"图标 + 文字"的按钮（Add / Save All）—— 而 ImGui::Button
// 画不了图标（字体里没有那些字形），所以只能自绘：InvisibleButton 拿命中，
// DrawList 画框 + 图标 + 文字。
//
// ⚠ 宽度必须**先算再画**：这一行里路径框吃剩余宽度，按钮宽度算错了
//   路径框就会压到按钮身上。所以宽度计算抽成 iconTextWidth 让两边共用。
float iconTextWidth(const char* text, float h, bool caret) {
    constexpr float kPad = 7.0f;    // 左右内边距
    constexpr float kIcon = 14.0f;  // 图标方块边长
    constexpr float kGap = 6.0f;    // 图标与文字之间
    float w = kPad + kIcon + kPad;
    if (text && text[0]) w += kGap + ImGui::CalcTextSize(text).x;
    if (caret) w += 12.0f;          // 菜单指示三角占位
    return std::max(w, h + 8.0f);
}

bool iconTextButton(const char* id, const char* text, float h,
                    const IconPainter& paint, const char* tip, bool caret) {
    ImGui::PushID(id);
    const float w = iconTextWidth(text, h, caret);
    ImGui::InvisibleButton("##itb", ImVec2(w, h));
    const bool clicked = ImGui::IsItemClicked();
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();

    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 bg = held      ? IM_COL32(52, 96, 168, 255)
                     : hovered ? IM_COL32(46, 62, 92, 255)
                               : IM_COL32(44, 47, 53, 255);
    dl->AddRectFilled(a, b, bg, 4.0f);
    dl->AddRect(a, b, IM_COL32(74, 79, 88, 200), 4.0f, 0, 1.0f);

    const ImU32 fg = IM_COL32(222, 228, 236, 255);
    const ImVec2 ia(a.x + 7.0f, a.y + (h - 14.0f) * 0.5f);
    const ImVec2 ib(ia.x + 14.0f, ia.y + 14.0f);
    paint(dl, ia, ib, fg);

    if (text && text[0]) {
        dl->AddText(ImVec2(ib.x + 6.0f, a.y + (h - ImGui::GetTextLineHeight()) * 0.5f),
                    fg, text);
    }
    if (caret) {
        const float cx = b.x - 11.0f;
        const float cy = a.y + h * 0.5f;
        dl->AddTriangleFilled(ImVec2(cx - 3.5f, cy - 2.0f),
                              ImVec2(cx + 3.5f, cy - 2.0f),
                              ImVec2(cx, cy + 2.6f), fg);
    }
    if (hovered && tip) ImGui::SetTooltip(TR("%s"), tip);
    ImGui::PopID();
    return clicked;
}

// 保存图标：软盘（外壳 + 上方金属滑片 + 下方标签块）。
// 这是"保存"最没有歧义的图形，比一个向下箭头强 —— 后者会被读成"下载"。
void paintSaveIcon(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c) {
    const float cx = (a.x + b.x) * 0.5f;
    const float cy = (a.y + b.y) * 0.5f;
    const float w = 5.8f, h = 6.2f;
    // 外壳
    dl->AddRect(ImVec2(cx - w, cy - h), ImVec2(cx + w, cy + h), c, 1.4f, 0, 1.6f);
    // 上方滑片（金属挡片）
    dl->AddRectFilled(ImVec2(cx - 2.2f, cy - h + 1.2f),
                      ImVec2(cx + 2.2f, cy - h + 3.8f), c, 0.4f);
    // 下方标签块
    dl->AddRectFilled(ImVec2(cx - w + 1.5f, cy + 0.8f),
                      ImVec2(cx + w - 1.5f, cy + h - 1.0f), c, 0.8f);
}

// 历史导航：左 / 右人字形（"上一层 / 下一层"）。
// 画成 ◀ ▶ 而不是 ↑ ↓：它们紧挨着路径框，横向的箭头天然对应
// "往路径的上一段 / 下一段走"，而且不会和左栏那个 ↑（真正的上一级）
// 撞脸 —— 两个同样的箭头做不同的事是最糟的设计。
void paintChevronLeft(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c) {
    const float cx = (a.x + b.x) * 0.5f;
    const float cy = (a.y + b.y) * 0.5f;
    dl->AddLine(ImVec2(cx + 2.4f, cy - 5.2f), ImVec2(cx - 2.6f, cy), c, 1.9f);
    dl->AddLine(ImVec2(cx - 2.6f, cy), ImVec2(cx + 2.4f, cy + 5.2f), c, 1.9f);
}

void paintChevronRight(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c) {
    const float cx = (a.x + b.x) * 0.5f;
    const float cy = (a.y + b.y) * 0.5f;
    dl->AddLine(ImVec2(cx - 2.4f, cy - 5.2f), ImVec2(cx + 2.6f, cy), c, 1.9f);
    dl->AddLine(ImVec2(cx + 2.6f, cy), ImVec2(cx - 2.4f, cy + 5.2f), c, 1.9f);
}

} // namespace

// 缩略图纹理的注销交给 ThumbnailCache 的析构（原来这里有一份手写的循环，
// 现在和 InspectorPanel 的材质槽共用同一份实现）。

// ---------------------------------------------------------------- 目录

void ContentBrowser::refresh() {
    m_entries.clear();
    m_dirty = false;
    // 目录树缓存与新的一次扫描同寿命：文件系统可能刚被新建/改名/删除过。
    m_dirCache.clear();

    if (m_root.empty()) {
        std::error_code ec;
        m_root = fs::current_path(ec).string();
        if (ec) m_root = ".";
    }

    // 目录树的根 = "当前游戏项目的内容目录"。有 assets/ 就用它，否则退化成
    // 进程工作目录（引擎/示例项目之外也能用）。
    if (m_contentRootRel.empty()) {
        std::error_code rec;
        m_contentRootRel = fs::is_directory(fs::path(m_root) / "assets", rec)
                               ? "assets"
                               : std::string();
    }

    // 第一次进来直接落在 assets/ 上 —— 引擎的"内容根"。
    // 浏览器的根是进程工作目录（双击 exe 时就是 exe 目录，那里全是 exe 和
    // 着色器，没什么可看的）；对编辑器来说有用的东西都在 assets/ 下面，
    // 所以默认往下一层：和 UE5 的 Content 面板一个意思。
    // MYVK_CONTENT_DIR 可以覆盖这个默认落点（截图 / 自动化用）。
    if (!m_initializedOnce) {
        m_initializedOnce = true;
        if (const char* env = std::getenv("MYVK_CONTENT_DIR")) {
            m_rel = env;
        } else {
            std::error_code sec;
            if (fs::is_directory(fs::path(m_root) / "assets", sec))
                m_rel = "assets";
        }
    }

    const fs::path dir = fs::path(m_root) / m_rel;
    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) {
        m_rel.clear();
        return;
    }

    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const fs::directory_entry& de = *it;
        DirEntry e;
        e.name = de.path().filename().string();
        e.isDir = de.is_directory(ec);
        if (!e.isDir) e.size = de.file_size(ec);

        if (!m_filter.empty() && !containsFold(e.name, m_filter)) continue;

        // 只关心"有意义的"文件：目录 + 引擎认识的几类资源
        if (!e.isDir && !isImage(e.name) && !isModel(e.name) &&
            !isScript(e.name) && !isScene(e.name) && !isShader(e.name) &&
            !isMaterial(e.name)) {
            continue;
        }
        // E13 类型过滤
        if (!entryMatchesType(e)) continue;
        e.absPath = de.path().string();
        m_entries.push_back(std::move(e));
    }

    // 目录在前，同类按名字排（找东西比"按修改时间"更好预测）
    // 2026-10-17：排序字段/方向改成用户可选（右栏排序图标），但
    // **"目录永远在文件前面"不受影响** —— 和资源管理器一致。
    std::sort(m_entries.begin(), m_entries.end(),
              [this](const DirEntry& a, const DirEntry& b) {
                  if (a.isDir != b.isDir) return a.isDir > b.isDir;
                  int c = 0;
                  if (m_sortBy == SortBy::Size && a.size != b.size) {
                      c = (a.size < b.size) ? -1 : 1;
                  } else {
                      const std::string la = lowerCopy(a.name);
                      const std::string lb = lowerCopy(b.name);
                      if (la != lb) c = (la < lb) ? -1 : 1;
                  }
                  return m_sortAsc ? c < 0 : c > 0;
              });

    // 自动化可断言的一行：当前目录 + 过滤条件 + 可见条目数。
    // tools/verify_content_browser.py 靠它验证"类型过滤切换后数量变化正确"。
    // ⚠ 前 5 个字段的**顺序与格式不能动**（脚本的正则按位置取），
    //   新字段一律往后加。
    VK_LOG_INFO("CB-ENTRIES dir=%s filter=%s type=%d view=%d count=%zu "
                "sort=%d asc=%d",
                m_rel.empty() ? "(root)" : m_rel.c_str(),
                m_filter.empty() ? "-" : m_filter.c_str(),
                static_cast<int>(m_typeFilter), static_cast<int>(m_view),
                m_entries.size(), static_cast<int>(m_sortBy),
                static_cast<int>(m_sortAsc));
}

// ---------------------------------------------------------------- 缩略图

const ThumbnailCache::Entry* ContentBrowser::thumbnail(
    const std::string& absPath) {
    // 实现全在 ThumbnailCache 里（InspectorPanel 的材质槽用同一份）。
    // 这里传绝对路径当缓存键，避免同名文件互相覆盖。
    return m_thumbs.get(m_ctx.assets(), absPath);
}

void ContentBrowser::invalidateThumb(const std::string& absPath) {
    m_thumbs.erase(absPath);
}

void ContentBrowser::invalidateThumbTree(const std::string& absPath) {
    // 目录被改名/删除后，缓存键还是旧路径 —— 前缀匹配一并清掉，
    // 否则下次进到那个目录会拿到一张已经被销毁的句柄。
    std::string prefix = absPath;
    if (!prefix.empty() && prefix.back() != '/' && prefix.back() != '\\') {
        prefix.push_back('/');
    }
    m_thumbs.eraseTree(prefix);
}

// ---------------------------------------------------------------- 导航

// E13 历史栈：第一次导航时把初始目录压进去；向后导航（push=true）会
// 截断"前进"分支 —— 与浏览器一致。
void ContentBrowser::pushHistory(const std::string& rel) {
    if (!m_histInit) {
        m_histInit = true;
        m_history.push_back(rel);
        m_histPos = 0;
        return;
    }
    // 与当前位置相同的重复项不入栈（反复点同一个面包屑不该堆历史）
    if (m_histPos >= 0 && static_cast<std::size_t>(m_histPos) < m_history.size() &&
        m_history[static_cast<std::size_t>(m_histPos)] == rel) {
        return;
    }
    m_history.resize(static_cast<std::size_t>(m_histPos) + 1);
    m_history.push_back(rel);
    m_histPos = static_cast<int>(m_history.size()) - 1;
}

bool ContentBrowser::canBack() const { return m_histPos > 0; }
bool ContentBrowser::canForward() const {
    return m_histPos >= 0 &&
           static_cast<std::size_t>(m_histPos) + 1 < m_history.size();
}

void ContentBrowser::back() {
    if (!canBack()) return;
    --m_histPos;
    const std::string rel = m_history[static_cast<std::size_t>(m_histPos)];
    if (rel == m_rel) return;
    m_rel = rel;
    m_selected.clear();
    m_dirty = true;
    m_ctxOpen = false;
    m_ctxRequest = false;
    VK_LOG_INFO("CB-HIST back rel=%s pos=%d", m_rel.c_str(), m_histPos);
}

void ContentBrowser::forward() {
    if (!canForward()) return;
    ++m_histPos;
    const std::string rel = m_history[static_cast<std::size_t>(m_histPos)];
    if (rel == m_rel) return;
    m_rel = rel;
    m_selected.clear();
    m_dirty = true;
    m_ctxOpen = false;
    m_ctxRequest = false;
    VK_LOG_INFO("CB-HIST fwd rel=%s pos=%d", m_rel.c_str(), m_histPos);
}

// E13 类型过滤：按扩展名/是否为目录判定
bool ContentBrowser::entryMatchesType(const DirEntry& ent) const {
    switch (m_typeFilter) {
        case TypeFilter::All:     return true;
        case TypeFilter::Folder:  return ent.isDir;
        case TypeFilter::Model:   return !ent.isDir && isModel(ent.name);
        case TypeFilter::Texture: return !ent.isDir && isImage(ent.name);
        case TypeFilter::Material:return !ent.isDir && isMaterial(ent.name);
        case TypeFilter::Script:  return !ent.isDir && isScript(ent.name);
        case TypeFilter::Shader:  return !ent.isDir && isShader(ent.name);
        case TypeFilter::Scene:
            return !ent.isDir && isScene(ent.name) && !isMaterial(ent.name);
    }
    return true;
}

void ContentBrowser::navigateTo(const std::string& rel) {
    if (rel == m_rel) return;
    // ⚠ 顺序很重要：先记"现在在哪"，再切到新目录。
    //   反过来写（先赋值 m_rel 再 pushHistory）的话，**第一次**导航时历史
    //   里只有新目录本身（pushHistory 的首次调用会用它初始化栈），于是
    //   ‹"上一层目录" 从一启动就是灰的，要切两次才亮 —— 2026-10-17 把
    //   这两个按钮摆到界面之前一直没人发现，因为入口只有 Alt+←。
    pushHistory(m_rel);
    pushHistory(rel);
    m_rel = rel;
    m_selected.clear();
    m_dirty = true;
    // 换目录了，右键菜单的目标已经不在视野里 —— 直接作废
    m_ctxOpen = false;
    m_ctxRequest = false;
}

void ContentBrowser::goUp() {
    if (m_rel.empty()) return;
    const fs::path p(m_rel);
    std::string parent = p.parent_path().generic_string();
    if (parent == ".") parent.clear();
    navigateTo(parent);
}

void ContentBrowser::drawNavRow() {
    const float h = ImGui::GetFrameHeight();
    const ImVec2 bs(h, h);

    // 左栏头部的四个图标按钮。三栏改造后这一行只剩图标 —— 宽度要留给
    // 目录树（左栏最窄时只有 132 px）。
    //
    // ⚠ 这里**故意没有**"前进 / 后退"两个箭头按钮（2026-10-17 按用户要求
    //   去掉）：那两个按钮占了最宝贵的起手位置，而它们能做的事（回到刚才
    //   浏览过的目录）在编辑器里用处很小 —— 真正常用的是"上一级"。
    //   快捷键 Alt+← / Alt+→ 仍然可用（见 drawBody 末尾的键盘处理），
    //   back()/forward() 的历史栈也没拆，所以只是**去掉了入口**。
    const bool canUp = !m_rel.empty();
    if (iconButton("up", bs, paintUpArrow,
                   canUp ? "Go up one level  (Backspace)" : "Already at the root",
                   canUp)) {
        goUp();
    }
    // 调试开关 MYVK_LOG_RECTS=1：记下 ↑ 按钮矩形（自动化脚本要按它）
    logRect("CB-RECT up", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine(0.0f, 4.0f);
    if (iconButton("home", bs, paintHome, "Jump to the project root")) {
        navigateTo(m_contentRootRel);
    }
    ImGui::SameLine(0.0f, 4.0f);
    if (iconButton("reload", bs, paintRefresh, "Re-scan this folder")) {
        m_dirty = true;
    }
    logRect("CB-RECT reload", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine(0.0f, 4.0f);

    // New ▸（原来是一个 84 px 的文字按钮，三栏下放不下 → 换成 + 图标）
    if (iconButton("new", bs, paintPlus, "Create a new asset in this folder")) {
        ImGui::OpenPopup("##cb_new");
    }
    if (ImGui::BeginPopup("##cb_new")) {
        if (ImGui::MenuItem(TR("Folder"))) m_newFolderRequest = true;
        if (ImGui::MenuItem(TR("Scene (.json)"))) createScene();
        if (ImGui::MenuItem(TR("Script (.vks)"))) createScript();
        ImGui::Separator();
        ImGui::MenuItem(TR("Material (inline in scene)"), nullptr, false, false);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                TR("Materials are stored inside the scene JSON today -\n"
                   "edit them in the Inspector's Material section."));
        }
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------- 三栏
//
// 2026-10-17（用户需求）：Content 拆成"左中右"三栏。
//   左：**项目目录树** —— 根 = 当前游戏项目的内容目录（assets/）。
//       点任意一层，右栏立刻切到那一层。树自己就把路径显示出来了，
//       所以原来的面包屑整行删掉（它在窄面板下还要三级降级裁字）。
//   中：**资源类型单选竖列** —— All / Models / Textures / ... 一列
//       RadioButton（原来是工具栏里一个 78px 宽的下拉框：要展开才知道
//       有哪些类型，也不方便一眼看到"当前在筛什么"）。
//   右：**当前目录的内容** —— 顶行 = 搜索框（带最近搜索）+ 排序图标 +
//       视图切换，下面是网格 / 列表。
void ContentBrowser::drawTreePane(float w) {
    ImGui::BeginChild("##cb_tree", ImVec2(w, 0.0f), ImGuiChildFlags_Borders);
    const ImVec2 c0 = ImGui::GetWindowPos();
    const ImVec2 c1(c0.x + ImGui::GetWindowSize().x,
                    c0.y + ImGui::GetWindowSize().y);

    drawNavRow();
    ImGui::Separator();

    // 根节点：项目内容目录。label 只放目录名（"assets"），全路径走 tooltip。
    const std::string rootLabel =
        fs::path(m_contentRootRel.empty() ? m_root : m_contentRootRel)
            .filename()
            .string();
    drawDirNode(m_contentRootRel, rootLabel.empty() ? "/" : rootLabel, 0);

    ImGui::EndChild();
    logRect("CB-TREE", c0, c1);
}

void ContentBrowser::drawTypePane(float w) {
    ImGui::BeginChild("##cb_types", ImVec2(w, 0.0f), ImGuiChildFlags_Borders);
    const ImVec2 c0 = ImGui::GetWindowPos();
    const ImVec2 c1(c0.x + ImGui::GetWindowSize().x,
                    c0.y + ImGui::GetWindowSize().y);

    ImGui::TextDisabled("%s", TR("Asset type"));
    ImGui::Separator();

    // 行距压到 2：抽屉默认高度只有 320 px，8 个单选按默认间距排下来正好
    // 把最后一项（Folders）挤到边界上（虽然能滚，但看起来像被裁了）。
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 2.0f));

    // ⚠ 顺序 = TypeFilter 的枚举值（MYVK_EDITOR_CB_TYPE 的环境钩子、
    //   CB-ENTRIES 日志里的 type=N、verify_content_browser 的下标全部
    //   依赖这个顺序，只能往后加、不能重排）。
    static const char* kTypeNames[] = {"All",     "Models",    "Textures",
                                       "Materials", "Scenes",  "Scripts",
                                       "Shaders", "Folders"};
    for (int i = 0; i < 8; ++i) {
        if (ImGui::RadioButton(TR(kTypeNames[i]), m_typeFilterIdx == i)) {
            m_typeFilterIdx = i;
            m_typeFilter = static_cast<TypeFilter>(i);
            m_dirty = true;   // 类型过滤在 refresh() 里做 → 立刻重列一次
            VK_LOG_INFO("CB-FILTER type=%s", kTypeNames[i]);
        }
        char tag[48];
        std::snprintf(tag, sizeof(tag), "CB-TYPE %s", kTypeNames[i]);
        logRect(tag, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    }
    ImGui::PopStyleVar();

    ImGui::EndChild();
    logRect("CB-TYPES", c0, c1);
}

void ContentBrowser::drawItemsPane(float w) {
    ImGui::BeginChild("##cb_items", ImVec2(w, 0.0f), ImGuiChildFlags_Borders);
    const ImVec2 c0 = ImGui::GetWindowPos();
    const ImVec2 c1(c0.x + ImGui::GetWindowSize().x,
                    c0.y + ImGui::GetWindowSize().y);

    drawActionRow();   // 最上面一行：Add / Save All / 上一层·下一层 / 路径框
    drawSearchRow();
    ImGui::Separator();
    if (m_view == ViewMode::Grid) drawGrid();
    else drawList();

    ImGui::EndChild();
    logRect("CB-ITEMS", c0, c1);
}

// 右栏顶行：搜索框 + 最近搜索 + 排序图标 + 视图切换 + 缩略图尺寸。
void ContentBrowser::drawSearchRow() {
    const float h = ImGui::GetFrameHeight();
    const ImVec2 bs(h, h);
    const ImGuiStyle& st = ImGui::GetStyle();

    // 右侧固定要占的宽度（排序 + 网格 + 列表 + 尺寸档），搜索框吃掉剩下的。
    const float sizeW = 52.0f;
    const float rightW = bs.x * 3.0f + sizeW + st.ItemSpacing.x * 5.0f;
    const float searchW =
        std::max(ImGui::GetContentRegionAvail().x - rightW, 90.0f);

    ImGui::SetNextItemWidth(searchW);
    // 平时**边打边过滤**（沿用原来的手感）；只有回车才算一次"搜索"，
    // 记进最近搜索 —— 每敲一个字存一条只会把历史刷满无用前缀。
    const bool entered = ImGui::InputTextWithHint(
        "##cb_search", TR("Search this folder..."), m_pathBuf,
        sizeof(m_pathBuf), ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemEdited()) {
        m_filter = m_pathBuf;
        m_dirty = true;
    }
    if (entered && m_pathBuf[0]) pushRecentSearch(std::string(m_pathBuf));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Search the current folder by name\n"
                             "Esc: clear   Enter: remember this search"));
    }
    // 空着点进来 = 想重新搜 → 直接把最近搜索摊开（IDE 的搜索框都这么做）
    if (ImGui::IsItemActivated() && m_filter.empty() &&
        !m_recentSearches.empty()) {
        ImGui::OpenPopup("##cb_hist");
    }
    logRect("CB-SEARCH", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    // Esc 清空（键盘在框里时消费掉，免得传给别处）
    if (ImGui::IsItemHovered() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        m_pathBuf[0] = '\0';
        m_filter.clear();
        m_dirty = true;
    }

    // ---- 最近搜索 ----
    // ⚠ popup 必须**每帧无条件提交**（不能只在"刚打开那一帧"调 BeginPopup），
    //   否则 ImGui 下一帧没看到它就把自己关掉 —— 只闪一帧。
    if (ImGui::BeginPopup("##cb_hist")) {
        if (ImGui::IsWindowAppearing()) {
            VK_LOG_INFO("CB-HISTORY n=%zu", m_recentSearches.size());
        }
        ImGui::TextDisabled("%s", TR("Recent searches"));
        ImGui::Separator();
        for (std::size_t i = 0; i < m_recentSearches.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(m_recentSearches[i].c_str())) {
                std::snprintf(m_pathBuf, sizeof(m_pathBuf), "%s",
                              m_recentSearches[i].c_str());
                m_filter = m_recentSearches[i];
                m_dirty = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndPopup();
    }

    // ---- 排序图标（紧挨在搜索框右边）----
    ImGui::SameLine(0.0f, 4.0f);
    if (iconButton("sort", bs, paintSortIcon, "Sort options")) {
        ImGui::OpenPopup("##cb_sort");
    }
    logRect("CB-SORT", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    drawSortMenu();   // 同上：每帧无条件提交

    // ---- 视图切换 + 缩略图尺寸 ----
    ImGui::SameLine(0.0f, 4.0f);
    if (iconButton("view_grid", bs, paintGridMode, "Grid view",
                   m_view == ViewMode::Grid)) {
        m_view = ViewMode::Grid;
    }
    ImGui::SameLine(0.0f, 2.0f);
    if (iconButton("view_list", bs, paintListMode, "List view",
                   m_view == ViewMode::List)) {
        m_view = ViewMode::List;
    }
    ImGui::SameLine(0.0f, 4.0f);

    static const char* kSizeNames[] = {"S", "M", "L", "XL"};
    ImGui::SetNextItemWidth(sizeW);
    ImGui::BeginDisabled(m_view == ViewMode::List);
    if (ImGui::Combo("##cb_size", &m_thumbIdx, kSizeNames,
                     IM_ARRAYSIZE(kSizeNames))) {
        m_thumb = static_cast<ThumbSize>(m_thumbIdx);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Thumbnail size (grid view)"));
    }
}

// 排序菜单（排序图标点开的那个小子菜单）。
//
// 用户要求是"排序类型两个勾选框（名称 / 大小）+ 排序方式两个勾选框
// （升序 / 降序）"。这里就是**复选框**，但语义是二选一：点已勾上的那一项
// 不会把它取消（否则会出现"既不按名称也不按大小"这种无意义状态）。
// 视觉上仍然是勾选框，符合"勾选框"的说法。
void ContentBrowser::drawSortMenu() {
    if (!ImGui::BeginPopup("##cb_sort")) return;

    ImGui::TextDisabled("%s", TR("Sort by"));
    bool byName = (m_sortBy == SortBy::Name);
    bool bySize = (m_sortBy == SortBy::Size);
    if (ImGui::Checkbox(TR("Name"), &byName) && byName) {
        m_sortBy = SortBy::Name;
        m_dirty = true;           // 重新排序发生在 refresh() 里
        VK_LOG_INFO("CB-SORT by=name");
    }
    logRect("CB-SORT-NAME", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::Checkbox(TR("Size"), &bySize) && bySize) {
        m_sortBy = SortBy::Size;
        m_dirty = true;
        VK_LOG_INFO("CB-SORT by=size");
    }
    logRect("CB-SORT-SIZE", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    ImGui::Separator();
    ImGui::TextDisabled("%s", TR("Order"));
    bool asc = m_sortAsc;
    bool desc = !m_sortAsc;
    if (ImGui::Checkbox(TR("Ascending"), &asc) && asc) {
        m_sortAsc = true;
        m_dirty = true;
        VK_LOG_INFO("CB-SORT order=asc");
    }
    logRect("CB-SORT-ASC", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::Checkbox(TR("Descending"), &desc) && desc) {
        m_sortAsc = false;
        m_dirty = true;
        VK_LOG_INFO("CB-SORT order=desc");
    }
    logRect("CB-SORT-DESC", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    ImGui::EndPopup();
}

// ================================================================ 动作条
//
// 2026-10-17 晚（用户需求 4）：右侧内容区"搜索栏的上边"再加一行。
// 布局（严格按用户说的顺序）：
//
//     [ + Add ▾ ]   [ ▣ Save All ]  [ ‹ ] [ › ]   [ /assets/models ...... ]
//
//   · Add      —— 图标 + 文字，点开一个**向上**弹的子菜单
//   · Save All —— 图标 + 文字
//   · ‹ ›      —— "上一层目录 / 下一层目录"：沿**浏览历史**往返
//                 （和左栏那个 ↑"上一级"不是一回事：↑ 走的是父子层级，
//                   ‹ › 走的是你刚才看过什么。Alt+← / Alt+→ 同款）
//   · 路径框   —— 相对项目根目录的相对路径，只读、可选中复制

// Add ▸ 的 13 种资源类型。
// ⚠ 顺序 = 菜单里的顺序 = 验证脚本按 `CB-ADDITEM <key>` 取矩形时的依据，
//   只能往后加、不能重排。
// ⚠ key 一律 ASCII 小写（进日志 / 进文件头），label 是**英文原文**（过 TR()），
//   ext 不带点。
const ContentBrowser::AddKind* ContentBrowser::addKinds(int& count) {
    static const AddKind kKinds[] = {
        {"material",  "Material",       "vmat"},
        {"model",     "Model",          "vmodel"},
        {"scene",     "Scene",          "json"},
        {"animation", "Animation",      "vanim"},
        {"cinematic", "Cinematic Sequence", "vseq"},
        {"media",     "Media",          "vmedia"},
        {"water",     "Water",          "vwater"},
        {"texture",   "Texture",        "vtex"},
        {"physics",   "Physics",        "vphys"},
        {"audio",     "Audio",          "vaudio"},
        {"foliage",   "Foliage",        "vveg"},
        {"ui",        "User Interface", "vui"},
        {"worldmap",  "World Map",      "vmap"},
    };
    count = static_cast<int>(sizeof(kKinds) / sizeof(kKinds[0]));
    return kKinds;
}

void ContentBrowser::drawActionRow() {
    const float h = ImGui::GetFrameHeight();
    const ImVec2 bs(h, h);
    const ImGuiStyle& st = ImGui::GetStyle();
    const float availW = ImGui::GetContentRegionAvail().x;

    // ---- 窄面板三级降级 ----
    // 抽屉拉到很窄时，一行塞不下"Add + Save All + 两个箭头 + 路径框"。
    // 路径框是这一行里信息量最大的东西（它是"我在哪"的唯一指示），
    // 所以先牺牲 Save All 的文字、再整个牺牲 Save All。
    bool saveText = true;
    bool showSave = true;
    const float addW = iconTextWidth(TR("Add"), h, true);
    const float navW = bs.x * 2.0f + st.ItemSpacing.x;
    const float gaps = 6.0f + 8.0f + 6.0f;
    constexpr float kMinPathW = 96.0f;
    if (addW + iconTextWidth(TR("Save All"), h, false) + navW + gaps + kMinPathW >
        availW) {
        saveText = false;
    }
    if (addW + iconTextWidth("", h, false) + navW + gaps + kMinPathW > availW) {
        showSave = false;
    }

    // ---------------- Add ----------------
    if (iconTextButton("cb_add", TR("Add"), h, paintPlus,
                       "Create a new asset  (the menu opens above)", true)) {
        ImGui::OpenPopup("##cb_add");
    }
    m_addBtnX0 = ImGui::GetItemRectMin().x;
    m_addBtnY0 = ImGui::GetItemRectMin().y;
    m_addBtnX1 = ImGui::GetItemRectMax().x;
    m_addBtnY1 = ImGui::GetItemRectMax().y;
    logRect("CB-ADD-BTN", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    // 同上：popup 必须**每帧无条件提交**，只在"刚点开那一帧"调会只闪一帧。
    drawAddMenu();

    // ---------------- Save All ----------------
    ImGui::SameLine(0.0f, 6.0f);
    if (showSave) {
        if (iconTextButton("cb_saveall", saveText ? TR("Save All") : "", h,
                           paintSaveIcon,
                           "Save every modified asset (scene + input map)",
                           false)) {
            saveAll();
        }
        logRect("CB-SAVEALL", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        ImGui::SameLine(0.0f, 8.0f);
    }

    // ---------------- 上一层 / 下一层 ----------------
    if (iconButton("cb_prev", bs, paintChevronLeft,
                   "Previous folder  (Alt+Left)", canBack())) {
        back();
    }
    logRect("CB-PREV", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine(0.0f, 2.0f);
    if (iconButton("cb_next", bs, paintChevronRight,
                   "Next folder  (Alt+Right)", canForward())) {
        forward();
    }
    logRect("CB-NEXT", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    // ---------------- 路径框 ----------------
    ImGui::SameLine(0.0f, 6.0f);
    const float pathW = std::max(ImGui::GetContentRegionAvail().x, 40.0f);
    // "相对当前游戏项目的根目录的相对路径" —— m_rel 就是（它本来就相对
    // 进程工作目录，也就是项目根）。前面补一个 '/' 让它读起来像路径。
    const std::string shown = m_rel.empty() ? std::string("/") : ("/" + m_rel);
    std::snprintf(m_pathDisplay, sizeof(m_pathDisplay), "%s", shown.c_str());
    ImGui::SetNextItemWidth(pathW);
    // 只读 InputText 而不是 Text：这样路径能**选中 / Ctrl+C 复制**，
    // 但改不动 —— 用户要的是"显示"，不是"编辑"。
    ImGui::InputText("##cb_path", m_pathDisplay, sizeof(m_pathDisplay),
                     ImGuiInputTextFlags_ReadOnly);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Path relative to the project root (read-only)"));
    }
    logRect("CB-PATH", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (m_lastPathLogged != m_rel) {
        m_lastPathLogged = m_rel;
        VK_LOG_INFO("CB-PATH rel=%s", shown.c_str());
    }
}

// Add 按钮上方弹出的子菜单。
void ContentBrowser::drawAddMenu() {
    int n = 0;
    const AddKind* kinds = addKinds(n);

    // ---- 往上弹 ----
    // ImGui 默认把 popup 放在锚点**下方**。抽屉贴着屏幕底边，往下弹直接
    // 出屏幕；用户也明确要求"在按钮的上侧打开"。所以自己算位置：底边贴
    // 按钮顶边。
    //
    // 高度只能**估**：BeginPopup 之前窗口还没排版，拿不到真实尺寸。
    // MenuItem 的高度 = 文本行高（见 imgui_widgets.cpp 的 MenuItemEx：
    // Selectable 的 size.y 传的就是 label_size.y），项之间再加 ItemSpacing。
    // 估出来的误差在几像素级，而菜单周围没有任何东西跟它对齐，看不出来。
    const ImGuiStyle& st = ImGui::GetStyle();
    const int rows = n + 1;   // 13 类型 + Import
    const float rowH = ImGui::GetTextLineHeight() + st.ItemSpacing.y;
    const float sepY = st.ItemSpacing.y + 1.0f;
    const float estH =
        rows * rowH + sepY + st.WindowPadding.y * 2.0f - st.ItemSpacing.y;

    const ImVec2 vpPos = ImGui::GetMainViewport()->Pos;
    const ImVec2 vpSize = ImGui::GetMainViewport()->Size;
    float px = m_addBtnX0;
    float py = m_addBtnY0 - estH - 4.0f;
    if (py < vpPos.y + 4.0f) {
        // 窗口特别矮（或按钮本来就在屏幕上方）→ 退回往下弹，至少不越界
        py = m_addBtnY1 + 4.0f;
    }
    px = std::min(px, vpPos.x + vpSize.x - 220.0f);
    ImGui::SetNextWindowPos(ImVec2(px, py), ImGuiCond_Always);

    if (!ImGui::BeginPopup("##cb_add")) return;

    for (int i = 0; i < n; ++i) {
        if (ImGui::MenuItem(TR(kinds[i].label))) {
            createAsset(kinds[i].key, kinds[i].label, kinds[i].ext);
        }
        char tag[64];
        std::snprintf(tag, sizeof(tag), "CB-ADDITEM %s", kinds[i].key);
        logRect(tag, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    }

    ImGui::Separator();
    if (ImGui::MenuItem(TR("Import..."))) importFiles();
    logRect("CB-ADDITEM import", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());

    ImGui::EndPopup();
}

// Add ▸ <类型>：在当前目录落一个最小资产文件，然后进重命名。
//
// 2026-10-17 的边界：除了 Scene（.json 走 SceneSerializer 那套）和
// Material（.vmat 已经是约定好的扩展名），其余类型**还没有加载器**。
// 这里仍然把文件落下去 —— "新建了但打不开"是诚实的中间态，而"菜单点了
// 什么都不发生"是坏掉的。文件体就是 {"type":...,"version":1}，
// 将来的资产工厂按 type 分派即可。
void ContentBrowser::createAsset(const char* kindKey, const char* kindLabel,
                                 const char* ext) {
    const fs::path dir = fs::path(m_root) / m_rel;
    const std::string stem = std::string("New ") + kindLabel;
    const std::string path =
        uniqueChildPath(dir.string(), stem, std::string(".") + ext);

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        m_ctx.notify(TR("Create failed: cannot open file"));
        VK_LOG_WARN("CB-ADD kind=%s failed: cannot open %s", kindKey,
                    path.c_str());
        return;
    }
    char body[192];
    const int n = std::snprintf(body, sizeof(body),
                                "{\n  \"type\": \"%s\",\n  \"version\": 1\n}\n",
                                kindKey);
    if (n > 0) std::fwrite(body, 1, static_cast<size_t>(n), f);
    std::fclose(f);

    m_dirty = true;
    m_selected = path;
    beginRename(path, fs::path(path).filename().string());
    VK_LOG_INFO("CB-ADD kind=%s ext=%s path=%s", kindKey, ext, path.c_str());
    m_ctx.notify(std::string(TR("Created ")) + TR(kindLabel));
}

// "保存所有"：面板只置位，真正落盘由 EditorApp 做 —— 它才认识场景路径、
// 也才拿得到 InputMap。和 quitRequested / windowFullscreenRequest 一个套路。
void ContentBrowser::saveAll() {
    m_ctx.saveAllRequest() = true;
    VK_LOG_INFO("CB-SAVEALL dir=%s", m_rel.c_str());
}

// "导入"：只置位。模态对话框必须等这一帧的 ImGui 数据全部提交完再开 ——
// 在遍历控件的途中卡进一个阻塞的 Win32 消息循环，本帧的顶点缓冲会对不上。
void ContentBrowser::importFiles() {
    m_importRequest = true;
    VK_LOG_INFO("CB-IMPORT request dir=%s", m_rel.c_str());
}

void ContentBrowser::openImportDialog() {
#ifdef _WIN32
    // 自动化脚本点不了系统对话框 —— 给它一个"只走流程、不开窗"的开关。
    if (std::getenv("MYVK_EDITOR_CB_NO_DIALOG")) {
        VK_LOG_INFO("CB-IMPORT dialog=suppressed");
        return;
    }

    std::vector<wchar_t> buf(64 * 1024, L'\0');
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    if (!ofn.hwndOwner) ofn.hwndOwner = GetForegroundWindow();
    // 过滤器两段之间和最后都要有 '\0'（字面量本身已经带一个收尾的）
    ofn.lpstrFilter =
        L"Game assets\0"
        L"*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.gltf;*.glb;*.obj;*.fbx;*.wav;"
        L"*.mp3;*.ogg;*.json;*.vks\0"
        L"All files\0*.*\0";
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.lpstrTitle = L"Import assets into the current folder";
    // ⚠ OFN_NOCHANGEDIR 不能省：不带它，对话框会把**进程当前目录**改成
    //   用户最后浏览的那个文件夹。而这套引擎的资源查找、m_root、m_rel
    //   全建立在"工作目录 = 项目根"上 —— 一导入就全盘错位，且下次启动
    //   还开在那个目录。
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER |
                OFN_NOCHANGEDIR | OFN_ALLOWMULTISELECT;

    if (!GetOpenFileNameW(&ofn)) {
        VK_LOG_INFO("CB-IMPORT cancelled");
        return;
    }

    // 多选的返回格式："<目录>\0<文件1>\0<文件2>\0\0"；
    // 单选时就是 "<完整路径>\0\0"（没有额外的文件段）。
    std::vector<std::wstring> picked;
    {
        const wchar_t* p = buf.data();
        const std::wstring first = p;
        p += first.size() + 1;
        if (*p == L'\0') {
            picked.push_back(first);   // 单选：first 就是完整路径
        } else {
            while (*p) {
                const std::wstring name = p;
                picked.push_back(first + L"\\" + name);
                p += name.size() + 1;
            }
        }
    }

    const fs::path dir = fs::path(m_root) / m_rel;
    int done = 0;
    std::string last;
    for (const std::wstring& wp : picked) {
        const fs::path src(wp);
        std::error_code ec;
        if (!fs::is_regular_file(src, ec)) continue;
        // 同名冲突自动加编号，绝不覆盖用户已有的资产
        const std::string dst = uniqueChildPath(
            dir.string(), src.stem().string(), src.extension().string());
        fs::copy_file(src, dst, ec);
        if (ec) {
            VK_LOG_WARN("CB-IMPORT copy failed %s: %s", dst.c_str(),
                        ec.message().c_str());
            continue;
        }
        last = dst;
        ++done;
    }

    VK_LOG_INFO("CB-IMPORT done n=%d dir=%s last=%s", done, m_rel.c_str(),
                last.c_str());
    if (done == 0) {
        m_ctx.notify(TR("Nothing imported"));
        return;
    }
    m_dirty = true;
    m_selected = last;
    m_ctx.notify(std::string(TR("Imported ")) + std::to_string(done) +
                 TR(" file(s)"));
#else
    m_ctx.notify(TR("Import is Windows-only for now"));
    VK_LOG_WARN("CB-IMPORT unsupported on this platform");
#endif
}

// 一个目录节点（递归）。用 SceneHierarchy 同一套做法：
// TreeNodeEx 的 label 传空串，图标和名字都用 DrawList 自己画 —— 字体里
// 没有文件夹字形，而 label 是纯文本，没法在名字前面插矢量图标。
void ContentBrowser::drawDirNode(const std::string& rel,
                                 const std::string& label, int depth) {
    const std::vector<std::string> kids = subDirs(rel);

    ImGui::PushID(rel.empty() ? "##root" : rel.c_str());
    ImGuiTreeNodeFlags f = ImGuiTreeNodeFlags_OpenOnArrow |
                           ImGuiTreeNodeFlags_SpanAvailWidth |
                           ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (m_rel == rel) f |= ImGuiTreeNodeFlags_Selected;
    if (kids.empty()) f |= ImGuiTreeNodeFlags_Leaf;
    // 根节点默认展开一层：一进来就能看到 assets/ 下有什么
    if (depth == 0) f |= ImGuiTreeNodeFlags_DefaultOpen;

    const float rowX = ImGui::GetCursorScreenPos().x;
    const bool open = ImGui::TreeNodeEx("##node", f, "%s", "");

    // 点击（不是点三角展开）→ 把右栏切到这个目录
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        navigateTo(rel);
    }

    const ImVec2 rmin = ImGui::GetItemRectMin();
    const ImVec2 rmax = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float fs = ImGui::GetFontSize();
    const float textY = (rmin.y + rmax.y) * 0.5f - fs * 0.5f;
    float textX = rowX + ImGui::GetTreeNodeToLabelSpacing();
    drawTreeFolderIcon(dl, ImVec2(textX, textY), fs);
    textX += fs + 5.0f;
    dl->AddText(ImVec2(textX, textY), ImGui::GetColorU32(ImGuiCol_Text),
                label.c_str());
    if (ImGui::IsItemHovered()) {
        const fs::path full =
            rel.empty() ? fs::path(m_root) : fs::path(m_root) / rel;
        ImGui::SetTooltip("%s", full.string().c_str());
    }
    if (depth == 0) {
        logRect("CB-TREE-ROOT", rmin, rmax);
    }
    // 自动化锚点：每个节点一行（脚本点它来切目录）。
    // logRect 自带去抖（矩形不变不重复打），节点数也就十几个，不刷屏。
    {
        char tag[128];
        std::snprintf(tag, sizeof(tag), "CB-TREE-NODE %s", label.c_str());
        logRect(tag, rmin, rmax);
    }

    if (open && !kids.empty()) {
        for (const std::string& k : kids) {
            const std::string childRel =
                rel.empty() ? k : (fs::path(rel) / k).generic_string();
            drawDirNode(childRel, k, depth + 1);
        }
    }
    ImGui::PopID();
}

// 子目录列表（按名字排）。缓存放在 m_dirCache，refresh() 时清空。
// 返回**拷贝**：递归时 drawDirNode 会继续往缓存里插条目，返回引用虽然
// （unordered_map 是节点式容器）不会失效，但改容器时还持着引用容易出事。
std::vector<std::string> ContentBrowser::subDirs(const std::string& rel) {
    auto it = m_dirCache.find(rel);
    if (it != m_dirCache.end()) return it->second;

    std::vector<std::string> out;
    std::error_code ec;
    const fs::path dir =
        rel.empty() ? fs::path(m_root) : (fs::path(m_root) / rel);
    for (fs::directory_iterator it2(
             dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it2 != fs::directory_iterator(); it2.increment(ec)) {
        std::error_code dec;
        if (!it2->is_directory(dec)) continue;
        const std::string n = it2->path().filename().string();
        // 跳过隐藏目录（.git / .vs / .cache …）—— 对做项目的人没有意义
        if (n.empty() || n[0] == '.') continue;
        out.push_back(n);
    }
    std::sort(out.begin(), out.end());
    m_dirCache[rel] = out;
    return out;
}

void ContentBrowser::pushRecentSearch(std::string q) {
    if (q.empty()) return;
    // 去重（同一条挪到最前），再截到上限 —— 和"最近打开的场景"一个做法
    for (std::size_t i = 0; i < m_recentSearches.size(); ++i) {
        if (m_recentSearches[i] == q) {
            m_recentSearches.erase(m_recentSearches.begin() +
                                   static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    m_recentSearches.insert(m_recentSearches.begin(), std::move(q));
    constexpr std::size_t kMaxRecentSearch = 8;
    if (m_recentSearches.size() > kMaxRecentSearch) {
        m_recentSearches.resize(kMaxRecentSearch);
    }
    VK_LOG_INFO("CB-SEARCH q=%s n=%zu", m_recentSearches.front().c_str(),
                m_recentSearches.size());
}

// ---------------------------------------------------------------- 单个条目

void ContentBrowser::drawEntry(const DirEntry& ent, float cellW, float cellH) {
    ImGui::PushID(ent.absPath.c_str());
    ImGui::BeginGroup();

    const bool selected = (m_selected == ent.absPath);
    const bool renaming = (m_renameTarget == ent.absPath);

    const ImVec2 cellPos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // 格子底色（选中 / 悬停）—— 在内容之前画，天然在底层
    const bool hoveredBefore = ImGui::IsMouseHoveringRect(
        cellPos, ImVec2(cellPos.x + cellW, cellPos.y + cellH));
    if (selected || hoveredBefore) {
        dl->AddRectFilled(cellPos, ImVec2(cellPos.x + cellW, cellPos.y + cellH),
                          selected ? IM_COL32(36, 66, 112, 200)
                                   : IM_COL32(48, 52, 60, 150),
                          5.0f);
        if (selected) {
            dl->AddRect(cellPos, ImVec2(cellPos.x + cellW, cellPos.y + cellH),
                        IM_COL32(64, 132, 226, 230), 5.0f, 0, 1.5f);
        }
    }

    // E13 图标框跟着格子尺寸缩放（缩略图四档联动）
    const float iconBox = std::max(24.0f, m_cellW - 44.0f);
    const float iconTop = std::max(4.0f, (m_cellH - 22.0f - iconBox) * 0.35f);

    // ---- 图标 ----
    ImGui::Dummy(ImVec2(cellW, iconBox + iconTop));
    const ImVec2 iconPos(cellPos.x + (cellW - iconBox) * 0.5f,
                         cellPos.y + iconTop);

    if (ent.isDir) {
        drawFolderIcon(dl, iconPos, iconBox);
    } else if (isImage(ent.name)) {
        const ThumbnailCache::Entry* t = thumbnail(ent.absPath);
        if (t && t->ds != VK_NULL_HANDLE) {
            // 等比例裁剪（aspect-fill）：按较长的边取 UV，避免缩略图被拉变形
            float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
            if (t->w > t->h) {
                const float k = t->h / t->w;
                u0 = (1.0f - k) * 0.5f;
                u1 = 1.0f - u0;
            } else if (t->h > t->w) {
                const float k = t->w / t->h;
                v0 = (1.0f - k) * 0.5f;
                v1 = 1.0f - v0;
            }
            dl->AddRectFilled(iconPos,
                              ImVec2(iconPos.x + iconBox, iconPos.y + iconBox),
                              IM_COL32(30, 32, 36, 255), 3.0f);
            dl->AddImage(static_cast<ImTextureID>(
                             reinterpret_cast<std::uintptr_t>(t->ds)),
                         iconPos, ImVec2(iconPos.x + iconBox, iconPos.y + iconBox),
                         ImVec2(u0, v0), ImVec2(u1, v1));
            dl->AddRect(iconPos,
                        ImVec2(iconPos.x + iconBox, iconPos.y + iconBox),
                        IM_COL32(90, 96, 106, 200), 3.0f, 0, 1.0f);
        } else {
            drawPageIcon(dl, iconPos, iconBox, accentColor(ent.name),
                         glyphFor(ent.name));
        }
    } else {
        drawPageIcon(dl, iconPos, iconBox, accentColor(ent.name),
                     glyphFor(ent.name));
    }

    // ---- 名字（单行，超宽截断；重命名时就地换成输入框）----
    const float nameY = cellPos.y + iconBox + iconTop + 4.0f;
    const float nameMaxW = cellW - 10.0f;

    if (renaming) {
        ImGui::SetCursorScreenPos(ImVec2(cellPos.x + 4.0f, nameY - 3.0f));
        ImGui::SetNextItemWidth(cellW - 8.0f);
        if (m_renameFocus) {
            ImGui::SetKeyboardFocusHere();
            m_renameFocus = false;
        }
        if (ImGui::InputText("##rename", m_renameBuf, sizeof(m_renameBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            m_renameCommit = true;
        }
        if (ImGui::IsItemDeactivated() && !m_renameCommit) {
            m_renameCancel = true;   // 点别处 / Esc = 放弃
        }
    } else {
        const std::string label = ellipsize(ent.name, nameMaxW);
        const float tw = ImGui::CalcTextSize(label.c_str()).x;
        const ImU32 col = selected ? IM_COL32(236, 242, 250, 255)
                                   : IM_COL32(206, 212, 220, 255);
        dl->AddText(ImVec2(cellPos.x + (cellW - tw) * 0.5f, nameY), col,
                    label.c_str());
    }

    // 把可点区域撑满整个格子（网格的空白处也要能右键）
    ImGui::SetCursorScreenPos(cellPos);
    ImGui::Dummy(ImVec2(cellW, cellH));
    ImGui::EndGroup();

    // 自动化用（MYVK_LOG_RECTS=1）：把这一个格子的屏幕矩形打出来。
    // 脚本据此把模型**从这儿**拖到视口里 —— 见 tools/verify_asset_drag.py。
    {
        const std::string tag = "CB-CELL " + ent.name;
        logRect(tag.c_str(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    }

    // ---- 交互 ----
    const bool hovered = ImGui::IsItemHovered();

    if (hovered && !renaming) {
        ImGui::BeginTooltip();
        ImGui::Text(TR("%s"), ent.name.c_str());
        if (!ent.isDir) ImGui::TextDisabled(TR("%s"), humanSize(ent.size).c_str());
        ImGui::TextDisabled(TR("%s"), ent.absPath.c_str());
        if (isModel(ent.name))
            ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1.0f),
                               TR("drag into the viewport to instantiate"));
        if (isScript(ent.name))
            ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.95f, 1.0f),
                               TR("double-click: attach to selected entity"));
        if (isScene(ent.name))
            ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.6f, 1.0f),
                               TR("double-click: open this scene"));
        if (isImage(ent.name))
            ImGui::TextDisabled(TR("texture (albedo / normal / ORM source)"));
        ImGui::Separator();
        ImGui::TextDisabled(TR("right-click: rename / duplicate / delete"));
        ImGui::EndTooltip();
    }

    // 单击选中，双击执行默认动作
    if (hovered && !renaming) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            m_selected = ent.absPath;
        }
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            openEntry(ent);
        }
    }

    // 右键：**只记下目标**，真正的 OpenPopup / 绘制都放到 drawGrid() 里。
    //
    // ⚠ 为什么不能在这里直接 BeginPopup：
    //   ImGui 只认"本帧调用过 BeginPopup/BeginPopupContextItem 的 popup"，
    //   没被提交的会在帧末直接关掉。而菜单一弹出来就正好盖在光标下面，
    //   格子当帧就不再 hovered —— 若把菜单绘制塞进 `if (hovered)` 里，
    //   下一帧这段代码不执行、popup 没提交，菜单只闪一帧就消失（真实踩过的坑）。
    //   所以这里只置一个"请求"，绘制放在格子循环之外，**每帧无条件执行**。
    if (!renaming) {
        // 用 AllowWhenBlockedByPopup：菜单已经打开时，右键切换到别的格子
        // 也要能把菜单移到新目标上。
        const bool itemHovered =
            ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup);
        if (itemHovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
            m_selected = ent.absPath;
            m_ctxEntry = ent;   // 快照一份：菜单存活期间 m_entries 可能被 refresh 重建
            m_ctxRequest = true;
        }
    }

    // ---- 拖拽：资产 → 视口 / 材质槽 ----
    // 载荷统一是"相对资产根的路径字符串"（editor/EditorDragDrop.h 的
    // drag::kAsset），接收端到现在有两个：
    //   * 视口           —— 模型文件实例化到鼠标落点（贴地）
    //   * Inspector 材质槽 —— 图片文件绑到 albedo / normal / orm
    // 所以模型和图片**都能拖**；两个接收端各自认自己认得的扩展名，
    // 拖错的组合（模型拖到材质槽上）那边会拒绝并提示。
    //
    // 缩略图条目是 Image()/Dummy() 这类"无唯一交互 ID"的 item，
    // BeginDragDropSource() 不带该标志会对 g.LastItemData.ID==0 直接
    // IM_ASSERT(0) 崩溃（imgui.cpp:15137）。必须显式允许空 ID。
    const bool draggable = !ent.isDir && (isModel(ent.name) || isImage(ent.name));
    if (draggable &&
        ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        const std::string rel = assets::makeAssetRelative(ent.absPath);
        ImGui::SetDragDropPayload(drag::kAsset, rel.c_str(), rel.size() + 1);
        ImGui::Text(TR("%s"), ent.name.c_str());
        if (isImage(ent.name)) {
            ImGui::TextDisabled(TR("drop on a Material texture slot to bind"));
        } else {
            ImGui::TextDisabled(TR("drop in the viewport -> bottom sits on the grid"));
        }
        ImGui::EndDragDropSource();
    }

    ImGui::PopID();
}

// ---------------------------------------------------------------- 打开

void ContentBrowser::openEntry(const DirEntry& ent) {
    if (ent.isDir) {
        navigateTo((fs::path(m_rel) / ent.name).generic_string());
        return;
    }
    if (isScene(ent.name)) {
        const std::string rel = assets::makeAssetRelative(ent.absPath);
        const scene::SceneIoResult r = scene::loadScene(
            m_ctx.editorScene(), m_ctx.assets(),
            assets::resolveAssetPath(rel));
        if (r.ok) {
            m_ctx.onSceneReplaced();
            m_ctx.scenePath() = rel;
            m_ctx.notify(std::string(TR("Opened ")) + ent.name + " (" +
                         std::to_string(r.entities) + TR(" entities)"));
        } else {
            m_ctx.notify(TR("Open failed: ") + r.error);
        }
    } else if (isModel(ent.name)) {
        // 双击模型 = 放到原点上并且**底部贴住栅格面**（和拖进视口同一套逻辑，
        // 只是落点固定在世界原点）
        const std::string rel = assets::makeAssetRelative(ent.absPath);
        const glm::vec3 spot(0.0f);
        ImportResult imported;
        m_ctx.structuralEdit("Import " + ent.name, [&]() {
            imported = instantiateModel(m_ctx.editorScene(), m_ctx.assets(), rel,
                                        spot);
            if (imported.ok()) {
                alignImportToGround(m_ctx.editorScene(), m_picking, imported, spot);
            }
        });
        if (imported.ok()) {
            m_ctx.select(imported.root);
            m_ctx.notify(std::string(TR("Imported ")) + ent.name + " (" +
                         std::to_string(imported.count()) + TR(" mesh)"));
        } else {
            m_ctx.notify(TR("Failed: ") + ent.name);
        }
    } else if (isScript(ent.name)) {
        if (!m_ctx.hasSelection()) {
            m_ctx.notify(TR("Select an entity first, then double-click the script"));
        } else {
            const std::string rel = assets::makeAssetRelative(ent.absPath);
            const ecs::Entity sel = m_ctx.selection();
            m_ctx.structuralEdit("Assign Script", [&]() {
                ecs::World& w = m_ctx.editorScene().world();
                if (w.has<ecs::ScriptComponent>(sel)) {
                    w.get<ecs::ScriptComponent>(sel)->path = rel;
                } else {
                    ecs::ScriptComponent sc;
                    sc.path = rel;
                    w.add<ecs::ScriptComponent>(sel, sc);
                }
            });
            m_ctx.notify(std::string(TR("Assigned ")) + ent.name +
                      TR(" to selection"));
        }
    }
}

// ---------------------------------------------------------------- 重命名

void ContentBrowser::beginRename(const std::string& absPath,
                                 const std::string& name) {
    m_renameTarget = absPath;
    m_renameFocus = true;
    m_renameCommit = false;
    m_renameCancel = false;
    std::snprintf(m_renameBuf, sizeof(m_renameBuf), "%s", name.c_str());
}

void ContentBrowser::commitRename() {
    const std::string target = m_renameTarget;
    const std::string newName = m_renameBuf;
    m_renameTarget.clear();
    m_renameCommit = false;
    m_renameCancel = false;

    if (target.empty() || newName.empty()) return;

    const fs::path src(target);
    if (src.filename().string() == newName) return;   // 没改

    const fs::path dst = src.parent_path() / newName;
    std::error_code ec;
    if (fs::exists(dst, ec)) {
        m_ctx.notify(std::string(TR("Rename failed: '")) + newName +
                      TR("' already exists"));
        return;
    }
    fs::rename(src, dst, ec);
    if (ec) {
        m_ctx.notify(TR("Rename failed: ") + ec.message());
        VK_LOG_WARN("ContentBrowser: rename %s -> %s failed: %s", target.c_str(),
                    dst.string().c_str(), ec.message().c_str());
        return;
    }
    invalidateThumbTree(target);
    m_selected = dst.string();
    m_dirty = true;
    m_ctx.notify(TR("Renamed to ") + newName);
}

// ---------------------------------------------------------------- 复制

std::string ContentBrowser::uniqueChildPath(const std::string& parent,
                                            const std::string& stem,
                                            const std::string& ext) const {
    std::error_code ec;
    const fs::path first = fs::path(parent) / (stem + ext);
    if (!fs::exists(first, ec)) return first.string();
    for (int i = 2; i < 1000; ++i) {
        const fs::path p =
            fs::path(parent) / (stem + " " + std::to_string(i) + ext);
        if (!fs::exists(p, ec)) return p.string();
    }
    return first.string();
}

void ContentBrowser::duplicateEntry(const DirEntry& ent) {
    const fs::path src(ent.absPath);
    std::error_code ec;

    std::string newName;
    if (ent.isDir) {
        newName = uniqueChildPath(src.parent_path().string(), ent.name + " copy",
                                  "");
        fs::copy(src, newName, fs::copy_options::recursive, ec);
    } else {
        const std::string stem = src.stem().string();
        const std::string ext = src.extension().string();
        newName = uniqueChildPath(src.parent_path().string(), stem + " copy", ext);
        fs::copy_file(src, newName, ec);
    }

    if (ec) {
        m_ctx.notify(TR("Duplicate failed: ") + ec.message());
        VK_LOG_WARN("ContentBrowser: duplicate %s failed: %s", ent.absPath.c_str(),
                    ec.message().c_str());
        return;
    }
    m_selected = newName;
    m_dirty = true;
    m_ctx.notify(TR("Duplicated ") + ent.name);
}

// ---------------------------------------------------------------- 删除

void ContentBrowser::requestDelete(const DirEntry& ent) {
    m_deleteTarget = ent.absPath;
    m_deleteName = ent.name;
    m_deleteIsDir = ent.isDir;
    m_openDeleteConfirm = true;
}

void ContentBrowser::doDelete() {
    if (m_deleteTarget.empty()) return;
    const std::string target = m_deleteTarget;
    const std::string name = m_deleteName;
    const bool isDir = m_deleteIsDir;
    m_deleteTarget.clear();
    m_deleteName.clear();
    m_deleteIsDir = false;

    std::error_code ec;
    // 走回收站太依赖平台 API，这里直接删 —— 但前面一定过了确认框，
    // 而且删的是用户在资源浏览器里明确点名的那个条目。
    if (isDir) {
        fs::remove_all(target, ec);
    } else {
        fs::remove(target, ec);
    }
    if (ec) {
        m_ctx.notify(TR("Delete failed: ") + ec.message());
        VK_LOG_WARN("ContentBrowser: delete %s failed: %s", target.c_str(),
                    ec.message().c_str());
        return;
    }
    invalidateThumbTree(target);
    if (m_selected == target) m_selected.clear();
    m_dirty = true;
    m_ctx.notify(TR("Deleted ") + name);
    VK_LOG_INFO("ContentBrowser: deleted %s", target.c_str());
}

// ---------------------------------------------------------------- 新建文件夹

void ContentBrowser::createFolder() {
    const fs::path dir = fs::path(m_root) / m_rel;
    const std::string path = uniqueChildPath(dir.string(), "NewFolder", "");
    std::error_code ec;
    fs::create_directories(path, ec);
    if (ec) {
        m_ctx.notify(TR("New folder failed: ") + ec.message());
        return;
    }
    m_dirty = true;
    m_selected = path;
    // 建完立刻进入重命名 —— 和资源管理器的行为一致
    beginRename(path, fs::path(path).filename().string());
    m_ctx.notify(TR("Created folder"));
}

// ---------------------------------------------------------------- 新建资产

// E13 New ▸ Scene：写一份最小可用场景（只有相机/光照字段，entities 为空），
// 落盘后立刻进入重命名 —— 与 New Folder 一致。
void ContentBrowser::createScene() {
    const fs::path dir = fs::path(m_root) / m_rel;
    const std::string path = uniqueChildPath(dir.string(), "NewScene", ".json");
    std::error_code ec;
    {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) {
            m_ctx.notify(TR("New scene failed: cannot open file"));
            return;
        }
        const char* body =
            "{\n"
            "  \"version\": 1,\n"
            "  \"generator\": \"Venn/SceneSerializer\",\n"
            "  \"entities\": []\n"
            "}\n";
        std::fwrite(body, 1, std::strlen(body), f);
        std::fclose(f);
    }
    (void)ec;
    m_dirty = true;
    m_selected = path;
    beginRename(path, fs::path(path).filename().string());
    m_ctx.notify(TR("Created scene"));
}

// E13 New ▸ Script：写一个带 update() 骨架的 .vks
void ContentBrowser::createScript() {
    const fs::path dir = fs::path(m_root) / m_rel;
    const std::string path = uniqueChildPath(dir.string(), "NewScript", ".vks");
    {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) {
            m_ctx.notify(TR("New script failed: cannot open file"));
            return;
        }
        const char* body =
            "// Venn script (.vks) - attached from the Inspector's Script\n"
            "// section, or by double-clicking this file with an entity selected.\n"
            "//\n"
            "// Scripts only run in Play mode; the editor scene is untouched.\n"
            "\n"
            "fn update(dt) {\n"
            "    // rotate 90 deg/sec around Y\n"
            "    self.rotation.y = self.rotation.y + dt * 1.5708\n"
            "}\n";
        std::fwrite(body, 1, std::strlen(body), f);
        std::fclose(f);
    }
    m_dirty = true;
    m_selected = path;
    beginRename(path, fs::path(path).filename().string());
    m_ctx.notify(TR("Created script"));
}

// ---------------------------------------------------------------- 引用查询

// E13 Show References（简版）：在资产根下扫描文本型资产（场景 JSON / 脚本）
// 里是否出现该文件名。完整版需要资产数据库，这里先用"文本搜索"顶上 ——
// 覆盖了最常见的用法（"这个贴图/模型还有谁在用"）。
void ContentBrowser::showReferences(const DirEntry& ent) {
    if (ent.isDir) return;
    const std::string needle = ent.name;
    std::error_code ec;
    const fs::path root = m_root;
    std::vector<std::string> hits;

    for (fs::recursive_directory_iterator it(
             root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string name = it->path().filename().string();
        if (name == needle) continue;   // 不算"引用自己"
        if (!(isScene(name) || isScript(name))) continue;
        std::FILE* f = std::fopen(it->path().string().c_str(), "rb");
        if (!f) continue;
        std::string text;
        char buf[4096];
        size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
        std::fclose(f);
        if (text.find(needle) != std::string::npos) {
            hits.push_back(assets::makeAssetRelative(it->path().string()));
        }
        if (hits.size() >= 12) break;
    }

    if (hits.empty()) {
        m_ctx.notify(TR("No references to ") + needle);
        return;
    }
    std::string msg = std::to_string(hits.size()) + " reference(s): ";
    for (size_t i = 0; i < hits.size(); ++i) {
        if (i) msg += ", ";
        msg += hits[i];
    }
    m_ctx.notify(msg);
}

// ---------------------------------------------------------------- 资源管理器

void ContentBrowser::revealInExplorer(const std::string& absPath) {
#ifdef _WIN32
    // explorer.exe /select,"<path>" —— 打开资源管理器并选中该项
    std::string cmd = "explorer.exe /select,\"" + absPath + "\"";
    const int rc = std::system(cmd.c_str());
    (void)rc;
#else
    (void)absPath;
    m_ctx.notify(TR("Reveal in file manager is Windows-only for now"));
#endif
}

// ---------------------------------------------------------------- 右键菜单

void ContentBrowser::drawEntryContextMenu() {
    // 没被提交 = 菜单已经被点掉 / Esc / 或者同层的空白菜单把它顶掉了
    // → 目标作废，下一帧不再重开。这样状态是自愈的，不需要额外的关闭逻辑。
    if (!ImGui::BeginPopup(kEntryCtxPopup)) {
        m_ctxOpen = false;
        return;
    }

    const DirEntry ent = m_ctxEntry;   // 拷贝：动作里可能触发 refresh()

    if (ImGui::MenuItem(ent.isDir ? "Open" : "Open / Run")) openEntry(ent);
    ImGui::Separator();
    if (ImGui::MenuItem(TR("Rename"), "F2")) beginRename(ent.absPath, ent.name);
    if (ImGui::MenuItem(TR("Duplicate"), "Ctrl+D")) duplicateEntry(ent);
    // 只有在文件夹上才有意义 —— 建到文件里一定会失败
    if (ent.isDir && ImGui::MenuItem(TR("New Folder Here"))) {
        navigateTo((fs::path(m_rel) / ent.name).generic_string());
        m_newFolderRequest = true;
    }
    ImGui::Separator();
    if (ImGui::MenuItem(TR("Copy Path"))) {
        ImGui::SetClipboardText(ent.absPath.c_str());
        m_ctx.notify(TR("Path copied to clipboard"));
    }
    if (ImGui::MenuItem(TR("Show in Explorer"))) revealInExplorer(ent.absPath);
    if (ImGui::MenuItem(TR("Show References"))) showReferences(ent);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Scan scene / script assets that mention this file"));
    }
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.55f, 0.5f, 1.0f));
    if (ImGui::MenuItem(TR("Delete"), "Del")) requestDelete(ent);
    ImGui::PopStyleColor();

    ImGui::EndPopup();
}

void ContentBrowser::drawEmptyContextMenu() {
    if (!ImGui::BeginPopupContextWindow("##empty_ctx",
                                        ImGuiPopupFlags_MouseButtonRight |
                                            ImGuiPopupFlags_NoOpenOverItems)) {
        return;
    }
    if (ImGui::MenuItem(TR("New Folder"))) m_newFolderRequest = true;
    if (ImGui::MenuItem(TR("New Scene"))) createScene();
    if (ImGui::MenuItem(TR("New Script"))) createScript();
    ImGui::Separator();
    if (ImGui::MenuItem(TR("Refresh"))) m_dirty = true;
    if (ImGui::MenuItem(TR("Show in Explorer"))) revealInExplorer(
        (fs::path(m_root) / m_rel).string());
    if (ImGui::MenuItem(TR("Copy Path"))) {
        ImGui::SetClipboardText((fs::path(m_root) / m_rel).string().c_str());
        m_ctx.notify(TR("Path copied to clipboard"));
    }
    ImGui::EndPopup();
}

void ContentBrowser::drawDeleteConfirm() {
    if (m_openDeleteConfirm) {
        ImGui::OpenPopup("Delete###cb_delete");
        m_openDeleteConfirm = false;
    }
    ImGui::SetNextWindowSize(ImVec2(470.0f, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Delete###cb_delete", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ImGui::TextColored(ImVec4(1.0f, 0.66f, 0.55f, 1.0f), TR("Delete this %s?"),
                       m_deleteIsDir ? "folder and everything inside it"
                                     : "file");
    ImGui::Spacing();
    ImGui::TextWrapped(TR("%s"), m_deleteTarget.c_str());
    ImGui::Spacing();
    ImGui::TextDisabled(TR("This cannot be undone."));
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.24f, 0.22f, 1.0f));
    if (ImGui::Button(TR("Delete"), ImVec2(120.0f, 0.0f))) {
        doDelete();
        ImGui::CloseCurrentPopup();
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(120.0f, 0.0f)) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        m_deleteTarget.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ---------------------------------------------------------------- 列表视图

// E13 列表行：小图标 + 名字 + （右对齐）类型/大小。
// 与网格共用同一套交互（选中 / 双击 / 右键 / 拖拽），只是排成一行。
void ContentBrowser::drawListRow(const DirEntry& ent, float rowH) {
    ImGui::PushID(ent.absPath.c_str());

    const bool selected = (m_selected == ent.absPath);
    const bool renaming = (m_renameTarget == ent.absPath);

    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const bool hovered = ImGui::IsMouseHoveringRect(
        p, ImVec2(p.x + w, p.y + rowH));
    if (selected || hovered) {
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rowH),
                          selected ? IM_COL32(36, 66, 112, 200)
                                   : IM_COL32(48, 52, 60, 150), 4.0f);
        if (selected) {
            dl->AddRect(p, ImVec2(p.x + w, p.y + rowH),
                        IM_COL32(64, 132, 226, 230), 4.0f, 0, 1.5f);
        }
    }

    // 图标（16px 见方，垂直居中）
    const float ic = 16.0f;
    const ImVec2 ip(p.x + 6.0f, p.y + (rowH - ic) * 0.5f);
    if (ent.isDir) {
        drawFolderIcon(dl, ip, ic);
    } else if (isImage(ent.name)) {
        const ThumbnailCache::Entry* t = thumbnail(ent.absPath);
        if (t && t->ds != VK_NULL_HANDLE) {
            dl->AddImage(static_cast<ImTextureID>(
                             reinterpret_cast<std::uintptr_t>(t->ds)),
                         ip, ImVec2(ip.x + ic, ip.y + ic));
        } else {
            drawPageIcon(dl, ip, ic, accentColor(ent.name), glyphFor(ent.name));
        }
    } else {
        drawPageIcon(dl, ip, ic, accentColor(ent.name), glyphFor(ent.name));
    }

    const float textX = ip.x + ic + 8.0f;
    const float textY = p.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f;

    if (renaming) {
        ImGui::SetCursorScreenPos(ImVec2(textX, p.y + 2.0f));
        ImGui::SetNextItemWidth(w - (textX - p.x) - 8.0f);
        if (m_renameFocus) {
            ImGui::SetKeyboardFocusHere();
            m_renameFocus = false;
        }
        if (ImGui::InputText("##rename_list", m_renameBuf, sizeof(m_renameBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            m_renameCommit = true;
        }
        if (ImGui::IsItemDeactivated() && !m_renameCommit) m_renameCancel = true;
    } else {
        dl->AddText(ImVec2(textX, textY),
                    selected ? IM_COL32(236, 242, 250, 255)
                             : IM_COL32(206, 212, 220, 255),
                    ent.name.c_str());

        // 右端：类型 + 大小
        const std::string right =
            std::string(ent.isDir ? "folder" : lowerExt(ent.name)) +
            (ent.isDir ? "" : "   " + humanSize(ent.size));
        const float rw = ImGui::CalcTextSize(right.c_str()).x;
        if (w - rw - 10.0f > textX - p.x) {
            dl->AddText(ImVec2(p.x + w - rw - 8.0f, textY),
                        IM_COL32(140, 146, 156, 255), right.c_str());
        }
    }

    // 撑满整行（交互区）
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(w, rowH));

    {
        const std::string tag = "CB-CELL " + ent.name;
        logRect(tag.c_str(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    }

    const bool hov = ImGui::IsItemHovered();
    if (hov && !renaming) {
        ImGui::BeginTooltip();
        ImGui::Text(TR("%s"), ent.name.c_str());
        if (!ent.isDir) ImGui::TextDisabled(TR("%s"), humanSize(ent.size).c_str());
        ImGui::TextDisabled(TR("%s"), ent.absPath.c_str());
        ImGui::EndTooltip();
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) m_selected = ent.absPath;
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) openEntry(ent);
    }

    if (!renaming) {
        const bool itemHovered =
            ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup);
        if (itemHovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
            m_selected = ent.absPath;
            m_ctxEntry = ent;
            m_ctxRequest = true;
        }
    }

    // 拖拽（与网格一致）
    const bool draggable = !ent.isDir && (isModel(ent.name) || isImage(ent.name));
    if (draggable &&
        ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        const std::string rel = assets::makeAssetRelative(ent.absPath);
        ImGui::SetDragDropPayload(drag::kAsset, rel.c_str(), rel.size() + 1);
        ImGui::Text(TR("%s"), ent.name.c_str());
        ImGui::EndDragDropSource();
    }

    ImGui::PopID();
}

void ContentBrowser::drawList() {
    const float rowH = ImGui::GetFrameHeight() + 6.0f;
    // 2026-10-17：原来这里给底部的 "N entries" 统计留一行（-footerH），
    // 那行文字早已挪到底栏 → 现在铺满整个可用高度。
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 2.0f));
    ImGui::BeginChild("##list", ImVec2(0, 0), ImGuiChildFlags_None);

    drawEmptyContextMenu();

    if (m_entries.empty()) {
        ImGui::TextDisabled(m_filter.empty() ? "(empty folder)" : "(no match)");
    } else {
        for (const DirEntry& e : m_entries) drawListRow(e, rowH);
    }

    if (m_ctxRequest) {
        m_ctxRequest = false;
        m_ctxOpen = true;
        ImGui::OpenPopup(kEntryCtxPopup);
    }
    if (m_ctxOpen) drawEntryContextMenu();

    ImGui::PopStyleVar();
    ImGui::EndChild();
}

// ---------------------------------------------------------------- 网格

void ContentBrowser::drawGrid() {
    // E13 缩略图四档尺寸：格子边长与图标框一起缩放
    float cellW = 96.0f;
    float cellH = 104.0f;
    switch (m_thumb) {
        case ThumbSize::Small:  cellW = 72.0f;  cellH = 74.0f;  break;
        case ThumbSize::Medium: cellW = 96.0f;  cellH = 104.0f; break;
        case ThumbSize::Large:  cellW = 132.0f; cellH = 146.0f; break;
        case ThumbSize::Huge:   cellW = 176.0f; cellH = 196.0f; break;
    }
    m_cellW = cellW;
    m_cellH = cellH;

    const float availW = ImGui::GetContentRegionAvail().x;
    int columns = static_cast<int>((availW + 8.0f) / (cellW + 8.0f));
    if (columns < 1) columns = 1;

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 6.0f));
    // 2026-10-17：这里的 -footerH 同样随"统计行搬家"一起改成铺满
    // （多出来的这一行高度正好让 assets/models 的第三行也露出来）。
    ImGui::BeginChild("##grid", ImVec2(0, 0), ImGuiChildFlags_None);

    drawEmptyContextMenu();

    if (m_entries.empty()) {
        ImGui::TextDisabled(m_filter.empty() ? "(empty folder)" : "(no match)");
    } else {
        int col = 0;
        for (const DirEntry& e : m_entries) {
            drawEntry(e, cellW, cellH);
            // 每帧只记第一个格子（每行的首格都会走到 col==0，不加限制会刷屏）
            if (col == 0 && g_cellLogFrame != ImGui::GetFrameCount()) {
                g_cellLogFrame = ImGui::GetFrameCount();
                logRect("CB-RECT cell0", ImGui::GetItemRectMin(),
                        ImGui::GetItemRectMax());
            }
            ++col;
            if (col < columns) ImGui::SameLine();
            else col = 0;
        }
    }

    // ---- 条目右键菜单：在格子循环**之外**、每帧无条件提交 ----
    // 顺序很关键：先按 drawEntry 记下的"请求"开菜单，再在同一帧里画出来。
    // 两处都在本函数（##grid 子窗口、无 PushID）作用域里 → popup ID 一致。
    if (m_ctxRequest) {
        m_ctxRequest = false;
        m_ctxOpen = true;
        ImGui::OpenPopup(kEntryCtxPopup);
    }
    if (m_ctxOpen) drawEntryContextMenu();

    ImGui::PopStyleVar(2);
    ImGui::EndChild();
}

// ---------------------------------------------------------------- 主面板

void ContentBrowser::draw() {
    EditorContext::PanelVisibility& panels = m_ctx.panels();
    if (!panels.content) return;

    const LayoutRects& L = m_ctx.layout();
    ImGui::SetNextWindowPos(ImVec2(L.content.x, L.content.y),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(L.content.w, L.content.h),
                            ImGuiCond_Always);

    const ImGuiWindowFlags wf = ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoResize |
                                ImGuiWindowFlags_NoCollapse;
    if (!ImGui::Begin(trId("Content", "panel_content"), &panels.content, wf)) {
        ImGui::End();
        return;
    }
    drawBody();
    ImGui::End();
}

// 抽屉形态下的内容区：调用方已经 Begin 了一个无标题栏窗口（并且已经画完
// 自己的头部行 —— 右侧那个 "Dock in layout" 复选框在 EditorApp 里）。
//
// 为什么不把头部也放这里：头部要改的是 **EditorApp 的**抽屉状态
// （高度 / 停靠），而 ContentBrowser 不该反过来持有编辑器的布局状态。
// 拆成"外框归 EditorApp、内容归 ContentBrowser"之后，两边各管各的。
void ContentBrowser::drawBody() {
    // 键盘快捷键只在鼠标悬停在本面板时生效 —— 否则会和 Hierarchy 的
    // 同名快捷键（F2 改名 / Del 删除）打架。
    const bool hoveredPanel =
        ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows |
                               ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

    // E13 自动化钩子（一次性）：注入类型过滤 / 视图 / 尺寸，便于脚本
    // 断言"类型过滤切换后可见项数量变化正确"。正常使用不设即无影响。
    static bool s_envViewDone = false;
    if (!s_envViewDone) {
        s_envViewDone = true;
        if (const char* t = std::getenv("MYVK_EDITOR_CB_TYPE")) {
            static const char* kNames[] = {"all",    "model", "texture",
                                           "material", "scene", "script",
                                           "shader", "folder"};
            for (int i = 0; i < 8; ++i) {
                if (lowerCopy(t) == kNames[i]) {
                    m_typeFilterIdx = i;
                    m_typeFilter = static_cast<TypeFilter>(i);
                    m_dirty = true;
                    VK_LOG_INFO("CB-FILTER type=%s", kNames[i]);
                    break;
                }
            }
        }
        if (const char* v = std::getenv("MYVK_EDITOR_CB_VIEW")) {
            m_view = (lowerCopy(v) == "list") ? ViewMode::List : ViewMode::Grid;
            m_dirty = true;
        }
        if (const char* s = std::getenv("MYVK_EDITOR_CB_SIZE")) {
            const int idx = std::atoi(s);
            if (idx >= 0 && idx <= 3) {
                m_thumbIdx = idx;
                m_thumb = static_cast<ThumbSize>(idx);
            }
        }
        // MYVK_EDITOR_CB_HIST="foo,bar"：预置"最近搜索"。
        // 之所以要这个钩子：搜索历史是**用户输入**产生的，脚本要么真的
        // 注入键盘（跨进程不稳），要么就得先有点历史才能验证"点空搜索框
        // 会摊开最近搜索"。给它一个可复现的起点。
        if (const char* s = std::getenv("MYVK_EDITOR_CB_HIST")) {
            std::string all(s);
            std::size_t pos = 0;
            while (pos <= all.size()) {
                const std::size_t comma = all.find(',', pos);
                const std::string one = all.substr(
                    pos, comma == std::string::npos ? std::string::npos
                                                    : comma - pos);
                if (!one.empty()) m_recentSearches.push_back(one);
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
            VK_LOG_INFO("CB-HISTORY seeded=%zu", m_recentSearches.size());
        }
    }

    if (m_dirty) refresh();

    // ---- 三栏：目录树 | 类型过滤 | 内容 ----
    // 宽度按比例分，但两侧都有上下限：抽屉很窄时优先保右栏（内容才是
    // 用户真正要看的东西），把左栏压到下限。
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float totalW = std::max(avail.x, 200.0f);
    constexpr float kGap = 6.0f;
    float treeW = std::clamp(totalW * 0.18f, 132.0f, 250.0f);
    float typeW = std::clamp(totalW * 0.11f, 96.0f, 140.0f);
    float itemsW = totalW - treeW - typeW - kGap * 2.0f;
    if (itemsW < 160.0f) {
        // 极窄：先收类型栏，再收树
        typeW = std::max(typeW * 0.75f, 80.0f);
        itemsW = totalW - treeW - typeW - kGap * 2.0f;
        if (itemsW < 160.0f) {
            treeW = std::max(treeW * 0.7f, 90.0f);
            itemsW = std::max(totalW - treeW - typeW - kGap * 2.0f, 80.0f);
        }
    }

    drawTreePane(treeW);
    ImGui::SameLine(0.0f, kGap);
    drawTypePane(typeW);
    ImGui::SameLine(0.0f, kGap);
    drawItemsPane(itemsW);

    // ⚠ 原来这里有一行 "N entries | assets" 统计。2026-10-17 起它挪到了
    //   编辑器底栏（EditorApp::drawBottomBar 调 entryCount()/currentRel()）——
    //   抽屉是"从底栏往上弹"的，统计留在抽屉里会跟着抽屉一起消失，
    //   而它本来就该是"常驻的全局信息"。

    // 键盘：Backspace 上一级；Alt+← / Alt+→ 历史；F2 重命名；Delete 删除
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput && m_renameTarget.empty() && hoveredPanel) {
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) goUp();
        if (io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false)) back();
        if (io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) forward();
        auto selEntry = [&]() -> const DirEntry* {
            for (const DirEntry& e : m_entries)
                if (e.absPath == m_selected) return &e;
            return nullptr;
        };
        if (const DirEntry* e = selEntry()) {
            if (ImGui::IsKeyPressed(ImGuiKey_F2, false))
                beginRename(e->absPath, e->name);
            if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) requestDelete(*e);
        }
    }

    drawDeleteConfirm();

    // ---- 帧末统一处理（不在遍历格子的过程中改文件系统）----
    if (m_renameCommit) commitRename();
    if (m_renameCancel) {
        m_renameTarget.clear();
        m_renameCancel = false;
    }
    if (m_newFolderRequest) {
        m_newFolderRequest = false;
        createFolder();
    }
    // 导入对话框放在**最后**：此时本帧所有 ImGui 控件都已提交，
    // 阻塞进 Win32 消息循环也不会破坏这一帧的数据。
    if (m_importRequest) {
        m_importRequest = false;
        openImportDialog();
    }
}

// 底栏的 "N entries | assets" 用。抽屉关着时也要能显示 —— 所以这里
// 顺手做一次 refresh（只在 dirty 时列目录，不每帧扫盘）。
std::size_t ContentBrowser::entryCount() {
    if (m_dirty) refresh();
    return m_entries.size();
}

} // namespace editor
