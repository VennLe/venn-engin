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

} // namespace

// 缩略图纹理的注销交给 ThumbnailCache 的析构（原来这里有一份手写的循环，
// 现在和 InspectorPanel 的材质槽共用同一份实现）。

// ---------------------------------------------------------------- 目录

void ContentBrowser::refresh() {
    m_entries.clear();
    m_dirty = false;

    if (m_root.empty()) {
        std::error_code ec;
        m_root = fs::current_path(ec).string();
        if (ec) m_root = ".";
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
    std::sort(m_entries.begin(), m_entries.end(),
              [](const DirEntry& a, const DirEntry& b) {
                  if (a.isDir != b.isDir) return a.isDir > b.isDir;
                  return a.name < b.name;
              });

    // 自动化可断言的一行：当前目录 + 过滤条件 + 可见条目数。
    // tools/verify_content_browser.py 靠它验证"类型过滤切换后数量变化正确"。
    VK_LOG_INFO("CB-ENTRIES dir=%s filter=%s type=%d view=%d count=%zu",
                m_rel.empty() ? "(root)" : m_rel.c_str(),
                m_filter.empty() ? "-" : m_filter.c_str(),
                static_cast<int>(m_typeFilter), static_cast<int>(m_view),
                m_entries.size());
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
    m_rel = rel;
    pushHistory(rel);
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

    // ---- 返回上一级（有正常图标，而不是一个 ".." 文字按钮）----
    //
    // ⚠ 这里**故意没有**"前进 / 后退"两个箭头按钮（2026-10-17 按用户要求
    //   去掉）：Content 面板默认只有约 290 px 宽，那两个按钮占了最宝贵的
    //   起手位置，而它们能做的事（回到刚才浏览过的目录）在编辑器里用处很小
    //   —— 真正常用的是"上一级"。快捷键 Alt+← / Alt+→ 仍然可用（见
    //   draw() 末尾的键盘处理），back()/forward() 的历史栈也没拆，
    //   所以只是**去掉了入口**，没有删能力。
    const bool canUp = !m_rel.empty();
    if (iconButton("up", bs, paintUpArrow,
                   canUp ? "Go up one level  (Backspace)" : "Already at the root",
                   canUp)) {
        goUp();
    }
    // 调试开关 MYVK_LOG_RECTS=1：记下 ↑ 按钮矩形（自动化脚本要按它）
    logRect("CB-RECT up", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine(0.0f, 4.0f);
    if (iconButton("home", bs, paintHome, "Jump to the browsing root")) {
        navigateTo(std::string());
    }
    ImGui::SameLine(0.0f, 4.0f);
    if (iconButton("reload", bs, paintRefresh, "Re-scan this folder")) {
        m_dirty = true;
    }
    logRect("CB-RECT reload", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    // ---- 右端：New ▸ 菜单 ----
    // 与 4 个导航按钮同行（Content 面板约 290px 宽：导航 168 + New 84 = 252，
    // 放得下），面包屑另起一整行，见 drawCrumbRow()。
    const float newBtnW = ImGui::CalcTextSize("New").x +
                          ImGui::GetStyle().FramePadding.x * 2.0f + 34.0f;
    const float nx = ImGui::GetWindowContentRegionMax().x - newBtnW;
    if (nx > ImGui::GetCursorPosX()) {
        ImGui::SameLine();
        ImGui::SetCursorPosX(nx);
        if (ImGui::Button(TR("New"), ImVec2(newBtnW, 0.0f))) {
            ImGui::OpenPopup("##cb_new");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("Create a new asset in this folder"));
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
}

// 面包屑独占一整行：Content 面板默认只有约 290px 宽，和导航按钮挤一行时
// 连当前目录名都显示不出来。三级降级：全部显示 → "…" + 末尾若干段 →
// 只显示当前目录名。（完整路径在底部状态行里始终可见，这里可以放心裁。）
void ContentBrowser::drawCrumbRow() {
    const std::string rootName =
        fs::path(m_root.empty() ? "." : m_root).filename().string();

    std::vector<std::pair<std::string, std::string>> crumbs;   // label, rel
    crumbs.emplace_back(rootName.empty() ? "/" : rootName, std::string());
    if (!m_rel.empty()) {
        fs::path acc;
        for (const auto& part : fs::path(m_rel)) {
            acc /= part;
            crumbs.emplace_back(part.string(), acc.generic_string());
        }
    }

    // 面包屑按钮用 FramePadding(6)，宽度估算按 6 算，否则末尾会被裁半个字。
    const float pad = 6.0f * 2.0f + 2.0f;
    const float sepW = ImGui::CalcTextSize("/").x + 4.0f;
    const float dotsW = ImGui::CalcTextSize("...").x + sepW + 2.0f;
    auto crumbW = [&](std::size_t i) {
        return ImGui::CalcTextSize(crumbs[i].first.c_str()).x + pad;
    };

    const float availW =
        std::max(ImGui::GetContentRegionAvail().x - 2.0f, 40.0f);

    float total = 0.0f;
    for (std::size_t i = 0; i < crumbs.size(); ++i)
        total += crumbW(i) + (i ? sepW : 0.0f);

    bool clippedLeft = false;
    bool onlyLast = false;
    std::size_t firstShown = 0;
    if (total > availW) {
        clippedLeft = true;
        const float a = availW - dotsW;
        float used = 0.0f;
        firstShown = crumbs.size();
        for (std::size_t i = crumbs.size(); i-- > 0;) {
            const float w = crumbW(i) + sepW;
            if (used + w > a && firstShown != crumbs.size()) break;
            used += w;
            firstShown = i;
        }
        if (firstShown == crumbs.size()) {
            // 连"… + 末段"都放不下：只画当前目录名
            clippedLeft = false;
            onlyLast = true;
        }
    }

    const ImVec2 clipA = ImGui::GetCursorScreenPos();
    const float rowH = ImGui::GetTextLineHeightWithSpacing() + 2.0f;
    ImGui::PushClipRect(clipA, ImVec2(clipA.x + availW, clipA.y + rowH), true);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 2.0f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImVec4(0.23f, 0.36f, 0.57f, 1.0f));

    auto drawCrumb = [&](std::size_t i) {
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::SmallButton(crumbs[i].first.c_str()))
            navigateTo(crumbs[i].second);
        ImGui::PopID();
        if (i == 0 && ImGui::IsItemHovered())
            ImGui::SetTooltip(TR("%s"), m_root.c_str());
    };

    if (onlyLast) {
        drawCrumb(crumbs.size() - 1);
    } else {
        bool firstItem = true;
        if (clippedLeft) {
            ImGui::TextDisabled(TR("..."));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(TR("%s"), m_root.c_str());
            firstItem = false;
        }
        for (std::size_t i = firstShown; i < crumbs.size(); ++i) {
            if (!firstItem) {
                ImGui::SameLine(0.0f, 2.0f);
                ImGui::TextDisabled(TR("/"));
                ImGui::SameLine(0.0f, 2.0f);
            }
            firstItem = false;
            drawCrumb(i);
        }
    }
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    ImGui::PopClipRect();

    // 面包屑的宽度决策只在矩形调试开关下打印，且**只在决策变化时打**
    // （否则每帧一行会刷爆日志，自动化脚本读不到真正有用的行）。
    if (std::getenv("MYVK_LOG_RECTS")) {
        static int lastKey = -1;
        const int key = int(firstShown) * 4 + (clippedLeft ? 2 : 0) +
                        (onlyLast ? 1 : 0);
        if (key != lastKey) {
            lastKey = key;
            VK_LOG_INFO("CB-CRUMB avail=%.1f total=%.1f first=%zu dots=%d only=%d",
                        availW, total, firstShown, clippedLeft ? 1 : 0,
                        onlyLast ? 1 : 0);
        }
    }
}

void ContentBrowser::drawToolbarRow() {
    // 单行工具栏：类型过滤 + 视图切换 + 缩略图尺寸 + 名字过滤。
    // Content 面板默认只有约 290px 宽。原先"工具栏 + 过滤"拆两行，会把网格挤
    // 到只剩一行可见（拖放用例的目标资产会掉进裁剪区外）；全部并成一行后网格
    // 能稳定显示近两行。既然横向只有 278px，就放弃独立的 Clear 按钮（改按
    // Esc 清除，输入框也有右键菜单），把宽度让给名字过滤（约 100px）。
    static const char* kTypeNames[] = {"All",     "Models",  "Textures",
                                      "Materials", "Scenes", "Scripts",
                                      "Shaders", "Folders"};
    ImGui::SetNextItemWidth(78.0f);
    if (ImGui::Combo("##cb_type", &m_typeFilterIdx, kTypeNames,
                     IM_ARRAYSIZE(kTypeNames))) {
        m_typeFilter = static_cast<TypeFilter>(m_typeFilterIdx);
        m_dirty = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Filter by asset type (extension-based)"));
    }
    ImGui::SameLine(0.0f, 4.0f);

    const float h = ImGui::GetFrameHeight();
    const ImVec2 bs(h, h);
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
    ImGui::SetNextItemWidth(46.0f);
    ImGui::BeginDisabled(m_view == ViewMode::List);
    if (ImGui::Combo("##cb_size", &m_thumbIdx, kSizeNames,
                     IM_ARRAYSIZE(kSizeNames))) {
        m_thumb = static_cast<ThumbSize>(m_thumbIdx);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Thumbnail size (grid view)"));
    }
    ImGui::SameLine(0.0f, 4.0f);

    // ---- 名字过滤（吃掉本行剩余宽度）----
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##filter", "filter...", m_pathBuf,
                                 sizeof(m_pathBuf))) {
        m_filter = m_pathBuf;
        m_dirty = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Filter by name (case-insensitive)\nEsc to clear"));
    }
    // Esc 清除过滤（键盘在框里时消费掉，免得传给别处）
    if (ImGui::IsItemHovered() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        m_pathBuf[0] = '\0';
        m_filter.clear();
        m_dirty = true;
    }
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
    const float footerH = ImGui::GetFrameHeightWithSpacing();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 2.0f));
    ImGui::BeginChild("##list", ImVec2(0, -footerH), ImGuiChildFlags_None);

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
    // 高度留出底部那一行统计文字 —— 否则网格会把剩余空间全吃掉，
    // 统计行被挤到窗口外面（原来的写法就是这样，一直看不到）。
    const float footerH = ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("##grid", ImVec2(0, -footerH), ImGuiChildFlags_None);

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
    }

    if (m_dirty) refresh();

    drawNavRow();
    drawCrumbRow();
    drawToolbarRow();
    ImGui::Separator();
    if (m_view == ViewMode::Grid) drawGrid();
    else drawList();
    ImGui::Separator();

    // 统计文字（原来的底行：播放条已移到视口正下方，这里保持原状）
    char stat[512];
    std::snprintf(stat, sizeof(stat), "%zu entries  |  %s", m_entries.size(),
                  m_rel.empty() ? "(root)" : m_rel.c_str());
    ImGui::TextDisabled(TR("%s"), stat);

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
    ImGui::End();

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
}

} // namespace editor
