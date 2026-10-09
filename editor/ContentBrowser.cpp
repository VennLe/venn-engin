#include "ContentBrowser.h"

#include "DebugRects.h"
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
    return e == ".gltf" || e == ".glb" || e == ".obj";
}

bool isScript(const std::string& name) { return lowerExt(name) == ".vks"; }

bool isScene(const std::string& name) { return lowerExt(name) == ".json"; }

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
    if (hovered && tip) ImGui::SetTooltip("%s", tip);
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

} // namespace

ContentBrowser::~ContentBrowser() {
    // 注销所有缩略图纹理。这里必须还在 ImGui 后端存活期内
    // （EditorApp 的成员先于 Renderer 释放，满足这个前提）。
    for (auto& kv : m_thumbs) {
        if (kv.second.ds != VK_NULL_HANDLE) {
            ImGui_ImplVulkan_RemoveTexture(kv.second.ds);
        }
    }
    m_thumbs.clear();
}

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
            !isScript(e.name) && !isScene(e.name)) {
            continue;
        }
        e.absPath = de.path().string();
        m_entries.push_back(std::move(e));
    }

    // 目录在前，同类按名字排（找东西比"按修改时间"更好预测）
    std::sort(m_entries.begin(), m_entries.end(),
              [](const DirEntry& a, const DirEntry& b) {
                  if (a.isDir != b.isDir) return a.isDir > b.isDir;
                  return a.name < b.name;
              });
}

// ---------------------------------------------------------------- 缩略图

VkDescriptorSet ContentBrowser::thumbnail(const std::string& absPath) {
    auto it = m_thumbs.find(absPath);
    if (it != m_thumbs.end()) return it->second.ds;
    if (m_thumbFailed.count(absPath)) return VK_NULL_HANDLE;

    assets::Texture* tex = nullptr;
    try {
        // 用绝对路径当缓存键，避免同名文件互相覆盖
        tex = m_ctx.assets().loadTexture(absPath, absPath, true);
    } catch (const std::exception& ex) {
        VK_LOG_WARN("ContentBrowser: thumbnail failed (%s): %s",
                    absPath.c_str(), ex.what());
    }

    if (!tex || tex->view() == VK_NULL_HANDLE) {
        m_thumbFailed[absPath] = true;
        return VK_NULL_HANDLE;
    }

    Thumb t;
    t.ds = ImGui_ImplVulkan_AddTexture(tex->view(),
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    t.w = static_cast<float>(tex->width() ? tex->width() : 1u);
    t.h = static_cast<float>(tex->height() ? tex->height() : 1u);
    m_thumbs.emplace(absPath, t);
    return t.ds;
}

void ContentBrowser::invalidateThumb(const std::string& absPath) {
    auto it = m_thumbs.find(absPath);
    if (it != m_thumbs.end()) {
        ImGui_ImplVulkan_RemoveTexture(it->second.ds);
        m_thumbs.erase(it);
    }
    m_thumbFailed.erase(absPath);
}

void ContentBrowser::invalidateThumbTree(const std::string& absPath) {
    // 目录被改名/删除后，缓存键还是旧路径 —— 前缀匹配一并清掉，
    // 否则下次进到那个目录会拿到一张已经被销毁的句柄。
    std::string prefix = absPath;
    if (!prefix.empty() && prefix.back() != '/' && prefix.back() != '\\') {
        prefix.push_back('/');
    }
    for (auto it = m_thumbs.begin(); it != m_thumbs.end();) {
        if (it->first.compare(0, prefix.size(), prefix) == 0) {
            ImGui_ImplVulkan_RemoveTexture(it->second.ds);
            it = m_thumbs.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = m_thumbFailed.begin(); it != m_thumbFailed.end();) {
        if (it->first.compare(0, prefix.size(), prefix) == 0)
            it = m_thumbFailed.erase(it);
        else
            ++it;
    }
}

// ---------------------------------------------------------------- 导航

void ContentBrowser::navigateTo(const std::string& rel) {
    if (rel == m_rel) return;
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

    // ---- 返回上一级（有正常图标，而不是一个 ".." 文字按钮）----
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

    ImGui::SameLine(0.0f, 8.0f);

    // ---- 面包屑：每一段都能点 ----
    const std::string rootName =
        fs::path(m_root.empty() ? "." : m_root).filename().string();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 2.0f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImVec4(0.23f, 0.36f, 0.57f, 1.0f));

    if (ImGui::SmallButton(rootName.empty() ? "/" : rootName.c_str())) {
        navigateTo(std::string());
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m_root.c_str());

    if (!m_rel.empty()) {
        const fs::path relPath(m_rel);
        fs::path acc;
        for (const auto& part : relPath) {
            acc /= part;
            ImGui::SameLine(0.0f, 2.0f);
            ImGui::TextDisabled("/");
            ImGui::SameLine(0.0f, 2.0f);
            const std::string s = part.string();
            if (ImGui::SmallButton(s.c_str()))
                navigateTo(acc.generic_string());
        }
    }
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();

    // ---- 右端：新建文件夹 ----
    const float btnW = ImGui::CalcTextSize("New Folder").x +
                       ImGui::GetStyle().FramePadding.x * 2.0f + 26.0f;
    const float x = ImGui::GetWindowContentRegionMax().x - btnW;
    if (x > ImGui::GetCursorPosX()) {
        ImGui::SameLine();
        ImGui::SetCursorPosX(x);
        if (ImGui::Button("New Folder", ImVec2(btnW, 0.0f))) {
            m_newFolderRequest = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Create a folder here, then type its name");
        }
    }
}

void ContentBrowser::drawSearchRow() {
    const float clearW = ImGui::CalcTextSize("Clear").x +
                         ImGui::GetStyle().FramePadding.x * 2.0f + 12.0f;
    ImGui::SetNextItemWidth(-(clearW + 6.0f));
    if (ImGui::InputTextWithHint("##filter", "filter by name...", m_pathBuf,
                                 sizeof(m_pathBuf))) {
        m_filter = m_pathBuf;
        m_dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear", ImVec2(clearW, 0.0f))) {
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

    const float iconBox = 52.0f;
    const float iconTop = 10.0f;

    // ---- 图标 ----
    ImGui::Dummy(ImVec2(cellW, iconBox + iconTop));
    const ImVec2 iconPos(cellPos.x + (cellW - iconBox) * 0.5f,
                         cellPos.y + iconTop);

    if (ent.isDir) {
        drawFolderIcon(dl, iconPos, iconBox);
    } else if (isImage(ent.name)) {
        const VkDescriptorSet ds = thumbnail(ent.absPath);
        if (ds != VK_NULL_HANDLE) {
            // 等比例裁剪（aspect-fill）：按较长的边取 UV，避免缩略图被拉变形
            const Thumb& t = m_thumbs[ent.absPath];
            float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
            if (t.w > t.h) {
                const float k = t.h / t.w;
                u0 = (1.0f - k) * 0.5f;
                u1 = 1.0f - u0;
            } else if (t.h > t.w) {
                const float k = t.w / t.h;
                v0 = (1.0f - k) * 0.5f;
                v1 = 1.0f - v0;
            }
            dl->AddRectFilled(iconPos,
                              ImVec2(iconPos.x + iconBox, iconPos.y + iconBox),
                              IM_COL32(30, 32, 36, 255), 3.0f);
            dl->AddImage(static_cast<ImTextureID>(
                             reinterpret_cast<std::uintptr_t>(ds)),
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
        ImGui::Text("%s", ent.name.c_str());
        if (!ent.isDir) ImGui::TextDisabled("%s", humanSize(ent.size).c_str());
        ImGui::TextDisabled("%s", ent.absPath.c_str());
        if (isModel(ent.name))
            ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1.0f),
                               "drag into the viewport to instantiate");
        if (isScript(ent.name))
            ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.95f, 1.0f),
                               "double-click: attach to selected entity");
        if (isScene(ent.name))
            ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.6f, 1.0f),
                               "double-click: open this scene");
        if (isImage(ent.name))
            ImGui::TextDisabled("texture (albedo / normal / ORM source)");
        ImGui::Separator();
        ImGui::TextDisabled("right-click: rename / duplicate / delete");
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

    // ---- 拖拽：网格文件 → 视口 ----
    // **只有模型**（.gltf / .glb / .obj）能拖：视口那边只认"网格资产"这一种
    // 载荷，拖图片 / 脚本过去也只会得到一句 "Failed to import"，不如干脆
    // 不给拖（想挂脚本就双击）。
    //
    // 缩略图条目是 Image()/Dummy() 这类"无唯一交互 ID"的 item，
    // BeginDragDropSource() 不带该标志会对 g.LastItemData.ID==0 直接
    // IM_ASSERT(0) 崩溃（imgui.cpp:15137）。必须显式允许空 ID。
    if (!ent.isDir && isModel(ent.name) &&
        ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        const std::string rel = assets::makeAssetRelative(ent.absPath);
        ImGui::SetDragDropPayload("EDITOR_ASSET", rel.c_str(), rel.size() + 1);
        ImGui::Text("%s", ent.name.c_str());
        ImGui::TextDisabled("drop in the viewport -> bottom sits on the grid");
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
            m_ctx.notify("Opened " + ent.name + " (" +
                         std::to_string(r.entities) + " entities)");
        } else {
            m_ctx.notify("Open failed: " + r.error);
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
            m_ctx.notify("Imported " + ent.name + " (" +
                         std::to_string(imported.count()) + " mesh)");
        } else {
            m_ctx.notify("Failed: " + ent.name);
        }
    } else if (isScript(ent.name)) {
        if (!m_ctx.hasSelection()) {
            m_ctx.notify("Select an entity first, then double-click the script");
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
            m_ctx.notify("Assigned " + ent.name + " to selection");
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
        m_ctx.notify("Rename failed: '" + newName + "' already exists");
        return;
    }
    fs::rename(src, dst, ec);
    if (ec) {
        m_ctx.notify("Rename failed: " + ec.message());
        VK_LOG_WARN("ContentBrowser: rename %s -> %s failed: %s", target.c_str(),
                    dst.string().c_str(), ec.message().c_str());
        return;
    }
    invalidateThumbTree(target);
    m_selected = dst.string();
    m_dirty = true;
    m_ctx.notify("Renamed to " + newName);
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
        m_ctx.notify("Duplicate failed: " + ec.message());
        VK_LOG_WARN("ContentBrowser: duplicate %s failed: %s", ent.absPath.c_str(),
                    ec.message().c_str());
        return;
    }
    m_selected = newName;
    m_dirty = true;
    m_ctx.notify("Duplicated " + ent.name);
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
        m_ctx.notify("Delete failed: " + ec.message());
        VK_LOG_WARN("ContentBrowser: delete %s failed: %s", target.c_str(),
                    ec.message().c_str());
        return;
    }
    invalidateThumbTree(target);
    if (m_selected == target) m_selected.clear();
    m_dirty = true;
    m_ctx.notify("Deleted " + name);
    VK_LOG_INFO("ContentBrowser: deleted %s", target.c_str());
}

// ---------------------------------------------------------------- 新建文件夹

void ContentBrowser::createFolder() {
    const fs::path dir = fs::path(m_root) / m_rel;
    const std::string path = uniqueChildPath(dir.string(), "NewFolder", "");
    std::error_code ec;
    fs::create_directories(path, ec);
    if (ec) {
        m_ctx.notify("New folder failed: " + ec.message());
        return;
    }
    m_dirty = true;
    m_selected = path;
    // 建完立刻进入重命名 —— 和资源管理器的行为一致
    beginRename(path, fs::path(path).filename().string());
    m_ctx.notify("Created folder");
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
    m_ctx.notify("Reveal in file manager is Windows-only for now");
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
    if (ImGui::MenuItem("Rename", "F2")) beginRename(ent.absPath, ent.name);
    if (ImGui::MenuItem("Duplicate", "Ctrl+D")) duplicateEntry(ent);
    // 只有在文件夹上才有意义 —— 建到文件里一定会失败
    if (ent.isDir && ImGui::MenuItem("New Folder Here")) {
        navigateTo((fs::path(m_rel) / ent.name).generic_string());
        m_newFolderRequest = true;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Copy Path")) {
        ImGui::SetClipboardText(ent.absPath.c_str());
        m_ctx.notify("Path copied to clipboard");
    }
    if (ImGui::MenuItem("Show in Explorer")) revealInExplorer(ent.absPath);
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.55f, 0.5f, 1.0f));
    if (ImGui::MenuItem("Delete", "Del")) requestDelete(ent);
    ImGui::PopStyleColor();

    ImGui::EndPopup();
}

void ContentBrowser::drawEmptyContextMenu() {
    if (!ImGui::BeginPopupContextWindow("##empty_ctx",
                                        ImGuiPopupFlags_MouseButtonRight |
                                            ImGuiPopupFlags_NoOpenOverItems)) {
        return;
    }
    if (ImGui::MenuItem("New Folder")) m_newFolderRequest = true;
    ImGui::Separator();
    if (ImGui::MenuItem("Refresh")) m_dirty = true;
    if (ImGui::MenuItem("Show in Explorer")) revealInExplorer(
        (fs::path(m_root) / m_rel).string());
    if (ImGui::MenuItem("Copy Path")) {
        ImGui::SetClipboardText((fs::path(m_root) / m_rel).string().c_str());
        m_ctx.notify("Path copied to clipboard");
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

    ImGui::TextColored(ImVec4(1.0f, 0.66f, 0.55f, 1.0f), "Delete this %s?",
                       m_deleteIsDir ? "folder and everything inside it"
                                     : "file");
    ImGui::Spacing();
    ImGui::TextWrapped("%s", m_deleteTarget.c_str());
    ImGui::Spacing();
    ImGui::TextDisabled("This cannot be undone.");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.24f, 0.22f, 1.0f));
    if (ImGui::Button("Delete", ImVec2(120.0f, 0.0f))) {
        doDelete();
        ImGui::CloseCurrentPopup();
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        m_deleteTarget.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ---------------------------------------------------------------- 网格

void ContentBrowser::drawGrid() {
    // 格子稍微放大一点：图标 52px + 名字，88×118 会挤
    const float cellW = 96.0f;
    const float cellH = 104.0f;

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
    if (!ImGui::Begin("Content", &panels.content, wf)) {
        ImGui::End();
        return;
    }

    // 键盘快捷键只在鼠标悬停在本面板时生效 —— 否则会和 Hierarchy 的
    // 同名快捷键（F2 改名 / Del 删除）打架。
    const bool hoveredPanel =
        ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows |
                               ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

    if (m_dirty) refresh();

    drawNavRow();
    drawSearchRow();
    ImGui::Separator();
    drawGrid();
    ImGui::Separator();
    ImGui::TextDisabled("%zu entries  |  %s", m_entries.size(),
                        m_rel.empty() ? "(root)" : m_rel.c_str());

    // 键盘：Backspace 上一级；F2 重命名；Delete 删除（都对选中项）
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput && m_renameTarget.empty() && hoveredPanel) {
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) goUp();
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
