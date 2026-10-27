#include "SceneHierarchy.h"

#include "Localization.h"
#include "DebugRects.h"
#include "EditorFocus.h"   // 双击 → 相机聚焦（与 F 键共用一份）
#include "PanelRegistry.h"

#include "core/Logger.h"
#include "EditorDragDrop.h"
#include "EditorIcons.h"
#include "ecs/Components.h"
#include "scene/Camera.h"
#include "scene/Prefab.h"
#include "scene/Scene.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace editor {

namespace {

// 让 logRect 每帧最多记一次（drawNode 递归调用，见下面用法）
int g_logRectFrame = -1;
// 上一句"第一个被记的行"是哪个实体 —— 行尾状态列（👁/🔒）的锚点只给它记。
// 自动化脚本（tools/verify_hierarchy_status.py）靠 HIER-LOCK/HIER-EYE 的
// 矩形去点图标，所以锚点必须真的落在"画出来的那个按钮"上，不能猜坐标。
ecs::Entity g_logRectRow{};

// base、base 2、base 3 …… 里第一个没被占用的名字。
// "文件夹"这种节点天生会重名（就叫 Folder），直接建一堆同名节点会让人
// 在树里分不清哪个是哪个。
std::string uniqueName(scene::Scene& sc, const std::string& base) {
    if (!sc.find(base).valid()) return base;
    for (int i = 2; i < 10000; ++i) {
        const std::string n = base + " " + std::to_string(i);
        if (!sc.find(n).valid()) return n;
    }
    return base;
}

// e 是否位于 root 的子树里（防止把父节点拖进自己孩子里，形成环）
bool isInSubtree(scene::Scene& scene, ecs::Entity e, ecs::EntityId root) {
    const ecs::World& w = scene.world();
    ecs::Entity cur = e;
    for (int depth = 0; depth < 64 && cur.valid(); ++depth) {
        if (cur.id == root) return true;
        const auto* h = w.get<ecs::HierarchyComponent>(cur);
        if (!h || h->parent == ecs::kInvalidEntity) break;
        cur = w.handle(h->parent);
    }
    return false;
}

// 复制一个实体（只带"内容"组件，不带层级）
ecs::Entity duplicateEntity(scene::Scene& scene, ecs::Entity src,
                            const std::string& newName) {
    ecs::World& w = scene.world();
    ecs::Entity dst = scene.createObject(newName);

    if (const auto* st = w.get<ecs::TransformComponent>(src)) {
        if (auto* dt = w.get<ecs::TransformComponent>(dst)) *dt = *st;
    }
    if (const auto* sm = w.get<ecs::MeshComponent>(src))
        w.add<ecs::MeshComponent>(dst, *sm);
    if (const auto* sm = w.get<ecs::MaterialComponent>(src))
        w.add<ecs::MaterialComponent>(dst, *sm);
    if (const auto* sv = w.get<ecs::VisibilityComponent>(src)) {
        if (auto* dv = w.get<ecs::VisibilityComponent>(dst)) *dv = *sv;
    }
    if (const auto* sl = w.get<ecs::PointLightComponent>(src))
        w.add<ecs::PointLightComponent>(dst, *sl);
    if (const auto* sl = w.get<ecs::SpotLightComponent>(src))
        w.add<ecs::SpotLightComponent>(dst, *sl);
    if (const auto* ss = w.get<ecs::ScriptComponent>(src))
        w.add<ecs::ScriptComponent>(dst, *ss);
    // "文件夹"标记也要跟着复制，否则复制出来的分组节点会丢掉目录图标
    if (const auto* sh = w.get<ecs::HierarchyComponent>(src)) {
        if (auto* dh = w.get<ecs::HierarchyComponent>(dst))
            dh->group = sh->group;
    }

    return dst;
}

} // namespace

// ---------------------------------------------------------------- E12 图标
//
// 类型小图标与状态列（👁/🔒）都用 DrawList 手绘 —— 字体里没有这些
// 字形（矢量图标库只有文件夹那一套，见 drawTreeFolderIcon）。

// 类型枚举（type: 过滤与行内图标共用同一份判定）
enum class NodeType { None, Group, Mesh, Light, Script, Collision, Decal, Player };

static NodeType nodeTypeOf(const ecs::World& w, ecs::Entity e) {
    if (const auto* h = w.get<ecs::HierarchyComponent>(e))
        if (h->group) return NodeType::Group;
    if (w.has<ecs::MeshComponent>(e)) return NodeType::Mesh;
    if (w.has<ecs::PointLightComponent>(e) ||
        w.has<ecs::SpotLightComponent>(e) ||
        w.has<ecs::DirectionalLightComponent>(e))
        return NodeType::Light;
    if (w.has<ecs::ScriptComponent>(e)) return NodeType::Script;
    if (w.has<ecs::CollisionComponent>(e)) return NodeType::Collision;
    if (w.has<ecs::DecalComponent>(e)) return NodeType::Decal;
    if (w.has<ecs::PlayerComponent>(e)) return NodeType::Player;
    return NodeType::None;
}

static const char* nodeTypeName(NodeType t) {
    switch (t) {
        case NodeType::Group:     return "folder";
        case NodeType::Mesh:      return "mesh";
        case NodeType::Light:     return "light";
        case NodeType::Script:    return "script";
        case NodeType::Collision: return "collider";
        case NodeType::Decal:     return "decal";
        case NodeType::Player:    return "player";
        default:                  return "empty";
    }
}

// 行首类型图标。p = 图标区左上角，fs = 字号（图标与文字等高对齐）。
static void drawTypeIcon(ImDrawList* dl, ImVec2 p, float fs, NodeType t) {
    if (t == NodeType::None || t == NodeType::Group) return;   // 文件夹另有图标
    const float r = fs * 0.34f;
    const ImVec2 c(p.x + r + 1.0f, p.y + fs * 0.5f);
    switch (t) {
    case NodeType::Mesh: {
        // 立方体线框：两个错位方块
        const ImU32 col = IM_COL32(110, 170, 255, 230);
        dl->AddRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r * 0.35f, c.y + r * 0.35f), col);
        dl->AddRect(ImVec2(c.x - r * 0.35f, c.y - r * 0.35f), ImVec2(c.x + r, c.y + r), col);
        break;
    }
    case NodeType::Light: {
        // 实心圆 + 四条光线
        const ImU32 col = IM_COL32(250, 214, 110, 240);
        dl->AddCircleFilled(c, r * 0.55f, col);
        const float r0 = r * 0.8f, r1 = r * 1.3f;
        const float dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (const auto& d : dirs) {
            dl->AddLine(ImVec2(c.x + d[0] * r0, c.y + d[1] * r0),
                        ImVec2(c.x + d[0] * r1, c.y + d[1] * r1), col, 1.2f);
        }
        break;
    }
    case NodeType::Script: {
        // "{ }" 字形直接用文本（字体里有花括号）
        dl->AddText(ImVec2(p.x, p.y + fs * 0.06f), IM_COL32(130, 220, 140, 240), "{}");
        break;
    }
    case NodeType::Collision: {
        // 绿色虚线感方框（碰撞体）
        const ImU32 col = IM_COL32(120, 230, 120, 210);
        dl->AddRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), col, 0.0f, 0, 1.2f);
        dl->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), col, 1.0f);
        break;
    }
    case NodeType::Decal: {
        // 贴花：叠着的两层小方块
        const ImU32 col = IM_COL32(230, 140, 240, 230);
        dl->AddRect(ImVec2(c.x - r, c.y - r * 0.4f), ImVec2(c.x + r * 0.3f, c.y + r), col);
        dl->AddRect(ImVec2(c.x - r * 0.3f, c.y - r), ImVec2(c.x + r, c.y + r * 0.4f), col);
        break;
    }
    case NodeType::Player: {
        // 玩家：小人（圆头 + 身子线）
        const ImU32 col = IM_COL32(255, 150, 120, 240);
        dl->AddCircleFilled(ImVec2(c.x, c.y - r * 0.5f), r * 0.4f, col);
        dl->AddLine(ImVec2(c.x, c.y - r * 0.1f), ImVec2(c.x, c.y + r), col, 1.4f);
        dl->AddLine(ImVec2(c.x - r * 0.6f, c.y + r * 0.2f), ImVec2(c.x + r * 0.6f, c.y + r * 0.2f), col, 1.2f);
        break;
    }
    default: break;
    }
}

// 搜索框左侧的小放大镜：一个圆圈 + 一根斜柄。ImGui 自带字体里没有这个字形，
// 和文件夹 / 类型图标一样用 DrawList 现画（清晰、不依赖资源文件）。
static void drawMagnifierIcon(ImDrawList* dl, ImVec2 p, float s, ImU32 col) {
    const float r = s * 0.32f;
    const ImVec2 c(p.x + s * 0.42f, p.y + s * 0.42f);
    dl->AddCircle(c, r, col, 0, 1.4f);
    dl->AddLine(ImVec2(c.x + r * 0.74f, c.y + r * 0.74f),
                ImVec2(p.x + s * 0.96f, p.y + s * 0.96f), col, 1.6f);
}

// 把一行文字塞进 [p.x, p.x + width)：放不下就在**字符边界**上截断并补 ".."。
//
// 为什么不直接让子窗口裁：名字画到类型列上会跟类型文字叠在一起（右侧那一列
// 是独立绘制的，不是 ImGui 的列系统，没有自动裁切）。为什么不用 ImGui 内部的
// RenderTextEllipsis：那是 imgui_internal.h 的东西，本项目没有暴露。
// UTF-8 边界：只要 (byte & 0xC0) != 0x80 就是一个字符的起点 —— 中文名字被从
// 中间截断会出现半个字形的乱码。
static void drawClippedText(ImDrawList* dl, ImVec2 p, float width, ImU32 col,
                            const char* text) {
    if (!text || !text[0] || width <= 4.0f) return;
    if (ImGui::CalcTextSize(text).x <= width) {
        dl->AddText(p, col, text);
        return;
    }
    static const char* kDots = "..";
    const float dotsW = ImGui::CalcTextSize(kDots).x;
    std::vector<int> cut;    // 合法的截断位置（字节下标）
    const int n = static_cast<int>(std::strlen(text));
    for (int i = 1; i <= n; ++i) {
        if (i == n || (static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
            cut.push_back(i);
        }
    }
    if (cut.empty()) return;
    int lo = 0;
    int hi = static_cast<int>(cut.size()) - 1;
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        const std::string part(text, static_cast<size_t>(cut[mid]));
        if (ImGui::CalcTextSize(part.c_str()).x + dotsW <= width) lo = mid;
        else hi = mid - 1;
    }
    std::string out(text, static_cast<size_t>(cut[lo]));
    out += kDots;
    dl->AddText(p, col, out.c_str());
}

// 类型列的显示名（UE Outliner 的 "Type" 列那一套：文件夹显示成"文件夹"，
// 其余按组件给一个类名）。
//
// 2026-10-18 改：**全部走 TR**。原先这里刻意留英文（理由：是类名，和日志对得上），
// 但 UE / Unity / Blender 三家的 Outliner 在中文界面下都会本地化显示名
// （UE 中文的 Type 列就是"静态网格体"），所以"对齐行业基准"这条要求把原决定推翻了。
// ⚠ 日志与 `type:` 过滤 token 用的是**另一个函数** nodeTypeName()（小写
//   "mesh"/"light"），那个必须保持英文 —— 翻了这个函数不影响日志 grep。
//
// `brief` = 短名。Hierarchy 默认宽 285px，其中类型列只分到 ~77px，
// "DirectionalLight" / "StaticMeshActor" 放不下，截断成 "Directio.." /
// "StaticM.." 反而认不出是什么。所以**先试全名、放不下退短名、再放不下才截断**
// （见 drawNode 里那段）。短名只做"去掉后缀/取首词"，不改叫法体系。
// ⚠ 中文之后全名多数只有 3~5 字（比英文更窄），退短名的分支会少走很多 ——
//   但**不能删**：用户可能把列拖宽/缩窄，宽度判定是按实际字宽算的。
static const char* hierTypeLabel(const ecs::World& w, ecs::Entity e,
                                bool brief = false) {
    if (const auto* h = w.get<ecs::HierarchyComponent>(e)) {
        if (h->group) return TR("Folder");
    }
    if (w.has<ecs::DirectionalLightComponent>(e))
        return brief ? TR("DirLight") : TR("DirectionalLight");
    if (w.has<ecs::PointLightComponent>(e))
        return brief ? TR("PointLight") : TR("PointLight");   // 本来就够短
    if (w.has<ecs::SpotLightComponent>(e))
        return brief ? TR("SpotLight") : TR("SpotLight");
    if (w.has<ecs::ParticleEmitterComponent>(e))
        return brief ? TR("Particles") : TR("ParticleEmitter");
    if (w.has<ecs::MeshComponent>(e))
        return brief ? TR("Mesh") : TR("StaticMeshActor");
    if (w.has<ecs::ScriptComponent>(e))
        return brief ? TR("Script") : TR("ScriptActor");
    if (w.has<ecs::CollisionComponent>(e))
        return brief ? TR("Collider") : TR("ColliderActor");
    if (w.has<ecs::DecalComponent>(e))
        return brief ? TR("Decal") : TR("DecalActor");
    if (w.has<ecs::PlayerComponent>(e))
        return brief ? TR("Player") : TR("PlayerStart");
    return TR("Empty");
}

// 状态列：眼睛（可见性）。visible=false 时画成"闭眼"（轮廓 + 斜杠）。
static void drawEyeIcon(ImDrawList* dl, ImVec2 mn, ImVec2 mx, bool visible,
                        ImU32 col) {
    const ImVec2 c((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f);
    const float w = (mx.x - mn.x) * 0.5f;
    dl->AddCircle(c, w * 0.75f, col, 0, 1.2f);
    if (visible) {
        dl->AddCircleFilled(c, w * 0.3f, col);
    } else {
        dl->AddLine(ImVec2(c.x - w * 0.8f, c.y + w * 0.8f),
                    ImVec2(c.x + w * 0.8f, c.y - w * 0.8f), col, 1.3f);
    }
}

// 状态列：锁。locked=false 时梁画在锁体旁边（开锁态）+ 灰色。
static void drawLockIcon(ImDrawList* dl, ImVec2 mn, ImVec2 mx, bool locked,
                         ImU32 colHot, ImU32 colIdle) {
    const float w = mx.x - mn.x;
    const float h = mx.y - mn.y;
    const ImU32 col = locked ? colHot : colIdle;
    const float bw = w * 0.62f;
    const float bx = mn.x + (w - bw) * 0.5f;
    const float by = mn.y + h * 0.42f;
    dl->AddRect(ImVec2(bx, by), ImVec2(bx + bw, mn.y + h * 0.92f), col, 1.0f, 0, 1.2f);
    if (locked) {
        dl->AddCircle(ImVec2(bx + bw * 0.5f, by), bw * 0.34f, col, 0, 1.2f);
    } else {
        // 开锁：梁偏向一边
        dl->AddCircle(ImVec2(bx + bw * 0.82f, by - h * 0.1f), bw * 0.34f, col, 0, 1.2f);
    }
}

// ---------------------------------------------------------------- 过滤

bool SceneHierarchy::nodeMatches(ecs::Entity e, const std::string& name) const {
    if (m_filter[0] == '\0') return true;
    // "type:xxx" 前缀语法：按组件构成过滤（type:mesh / light / script /
    // collider / decal / player / folder / empty）。其余文本按子串匹配。
    if (std::strncmp(m_filter, "type:", 5) == 0) {
        const char* want = m_filter + 5;
        const NodeType t = nodeTypeOf(m_ctx.editorScene().world(), e);
        // 前缀匹配：type:li 命中 light
        const std::string tn = nodeTypeName(t);
        return tn.rfind(want, 0) == 0;   // want 是 tn 的前缀？
    }
    return name.find(m_filter) != std::string::npos;
}

bool SceneHierarchy::subtreeMatches(ecs::Entity e) const {
    const ecs::World& w = m_ctx.editorScene().world();
    const std::string* np = w.name(e);
    if (nodeMatches(e, np ? *np : std::string("Entity"))) return true;
    if (const auto* h = w.get<ecs::HierarchyComponent>(e)) {
        for (ecs::EntityId cid : h->children) {
            const ecs::Entity ce = w.handle(cid);
            if (ce.valid() && subtreeMatches(ce)) return true;
        }
    }
    return false;
}

// 收集 e 及其全部后代（含顺序 = 树里的顺序，父在前）。批量删除要它：
// destroy() **不递归**，只 destroy 父节点会留下一堆"parent 指向死 id"的
// 孤儿实体 —— 它们既不在树里（不是根节点），又还在渲染，等于凭空多出一堆
// 点不中、删不掉的幽灵。
void collectSubtreeIds(ecs::World& w, ecs::Entity e,
                       std::vector<ecs::Entity>& out, int depth = 0) {
    if (!e.valid() || !w.alive(e) || depth > 128) return;
    for (ecs::Entity k : out)
        if (k.id == e.id) return;
    out.push_back(e);
    if (const auto* h = w.get<ecs::HierarchyComponent>(e)) {
        for (ecs::EntityId cid : h->children) {
            const ecs::Entity ce = w.handle(cid);
            if (ce.valid()) collectSubtreeIds(w, ce, out, depth + 1);
        }
    }
}

// 在**画行之前**把整棵树按可见顺序摊平（见 m_flatOrder 的注释）。
void SceneHierarchy::collectFlatOrder(ecs::Entity e) {
    if (!e.valid()) return;
    const bool filterActive = m_filter[0] != '\0';
    if (filterActive && !subtreeMatches(e)) return;
    m_flatOrder.push_back(e);
    const ecs::World& w = m_ctx.editorScene().world();
    if (const auto* h = w.get<ecs::HierarchyComponent>(e)) {
        for (ecs::EntityId cid : h->children) {
            const ecs::Entity ce = w.handle(cid);
            if (ce.valid()) collectFlatOrder(ce);
        }
    }
}

// ---------------------------------------------------------------- 单节点

void SceneHierarchy::drawNode(ecs::Entity e, int depth) {
    scene::Scene& sc = m_ctx.editorScene();
    ecs::World& w = sc.world();

    const std::string* np = w.name(e);
    const std::string name = np ? *np : std::string("Entity");

    const auto* h = w.get<ecs::HierarchyComponent>(e);
    const bool hasChildren = h && !h->children.empty();

    // 重命名中 → 就地替换成输入框
    if (m_renameTarget == e) {
        ImGui::SetNextItemWidth(-1.0f);
        if (m_renameFocus) {
            ImGui::SetKeyboardFocusHere();
            m_renameFocus = false;
        }
        ImGui::PushID(static_cast<int>(e.id));
        if (ImGui::InputText("##rename", m_renameBuf, sizeof(m_renameBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            m_renameCommit = e;
            m_renameValue = m_renameBuf;
            m_hasRename = true;
            m_renameTarget = ecs::Entity{};
        }
        ImGui::PopID();
        if (ImGui::IsItemDeactivated() && m_renameTarget == e) {
            // 点了别处 / 按 Esc → 取消重命名（不写入）
            m_renameTarget = ecs::Entity{};
        }
        return;
    }

    // E12：过滤保留祖先链 —— 自己没命中但后代命中时，祖先照样画出来
    //（名字灰显），树形上下文不丢。
    const bool filterActive = m_filter[0] != '\0';
    if (filterActive && !subtreeMatches(e)) return;
    const bool dimmed = filterActive && !nodeMatches(e, name);

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                               ImGuiTreeNodeFlags_SpanAvailWidth |
                               ImGuiTreeNodeFlags_DefaultOpen |
                               // ⚠ 必须开 AllowOverlap：行尾那一列 👁/🔒 图标是
                               //    **叠在节点矩形上**的 InvisibleButton，而
                               //    ImGui 的命中测试是"提交顺序靠前的先占"——
                               //    节点先提交就把 g.HoveredId 占了，后面的图标
                               //    在 ItemHoverable() 里直接被 `g.HoveredId != 0
                               //    && !g.HoveredIdAllowOverlap` 挡掉 → **永远收不到
                               //    hover / click**（图标画得出来、点了没反应）。
                               //    开这个标志 = 节点主动让位，命中测试变成
                               //    "后提交的（图标）优先"。见 imgui.cpp::ItemHoverable。
                               ImGuiTreeNodeFlags_AllowOverlap;
    if (!hasChildren)
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (m_ctx.isSelected(e)) flags |= ImGuiTreeNodeFlags_Selected;

    // 名字后缀只留**状态**（隐藏 / 不投影 / 锁定）。
    // "这个节点是什么"已经由右侧的类型列回答（mesh / DirectionalLight / ...），
    // 原来那几个 "  [mesh]" "  [sun]" 和类型列完全重复，占的还正是名字最缺的
    // 那点宽度（Hierarchy 默认只有 280px 宽）。
    char label[256];
    const auto* vis = w.get<ecs::VisibilityComponent>(e);
    const bool locked = w.has<ecs::LockedComponent>(e);

    std::snprintf(label, sizeof(label), "%s%s%s%s", name.c_str(),
                  (vis && !vis->visible) ? "  [hidden]" : "",
                  (vis && !vis->castShadow) ? "  [noshadow]" : "",
                  locked ? "  [locked]" : "");

    // 是不是"文件夹"（纯分组节点）：Hierarchy 里给它画目录图标 + 暖色名字
    const bool isGroup = h && h->group;

    // 行的左边缘（= ImGui 内部 text_pos.x - text_offset_x）。
    // TreeNodeEx 的 label 是**纯文本**，没法在名字前面插一个矢量图标
    // （字体里没有文件夹字形），所以 label 传空串，图标 + 名字都在下面自己
    // 用 DrawList 画。文本起点 = 行左边缘 + GetTreeNodeToLabelSpacing()，
    // 这个值恰好等于 imgui_widgets.cpp 里的 text_offset_x
    // （FontSize + FramePadding.x * 2，未加 Framed 标志时）。
    const float rowX = ImGui::GetCursorScreenPos().x;

    ImGui::PushID(static_cast<int>(e.id));
    const bool open = ImGui::TreeNodeEx("##node", flags, "%s", "");

    // ---- 行内容：类型图标 / 目录图标 + 名字（状态列在后面画）----
    ImVec2 rowRectMin = ImGui::GetItemRectMin();
    ImVec2 rowRectMax = ImGui::GetItemRectMax();
    {
        const ImVec2 rmin = ImGui::GetItemRectMin();
        const ImVec2 rmax = ImGui::GetItemRectMax();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float fs = ImGui::GetFontSize();
        const float textY = (rmin.y + rmax.y) * 0.5f - fs * 0.5f;

        float textX = rowX + ImGui::GetTreeNodeToLabelSpacing();
        if (isGroup) {
            drawTreeFolderIcon(draw, ImVec2(textX, textY), fs);
            textX += fs + 5.0f;
        } else {
            // E12 类型图标（mesh/light/script/collider/decal/player）
            const NodeType t = nodeTypeOf(w, e);
            if (t != NodeType::None) {
                drawTypeIcon(draw, ImVec2(textX, textY), fs, t);
                textX += fs + 4.0f;
            }
        }

        // 文件夹暖色（跟目录图标一套）；过滤命中的祖先灰显 ——
        // "它没中，但树里留着它是为了给命中后代当路径"
        ImU32 col;
        if (dimmed)
            col = IM_COL32(150, 152, 158, 160);
        else if (isGroup)
            col = IM_COL32(238, 202, 132, 255);
        else
            col = ImGui::GetColorU32(ImGuiCol_Text);
        // 名字画到类型列左边界为止（超长就 ".." 截断，否则会压到类型文字上）
        const float nameW = m_typeColX - textX - 6.0f;
        // 名字后面还跟着状态后缀（[hidden] / [noshadow] / [locked]）。宽度不够
        // 时**先丢后缀、保名字** —— 后缀是补充信息，把名字挤成 "Grou.." 属于
        // 本末倒置（Hierarchy 默认宽度下每条长名字都会踩到）。
        const char* shown = label;
        if (ImGui::CalcTextSize(label).x > nameW &&
            ImGui::CalcTextSize(name.c_str()).x <= nameW) {
            shown = name.c_str();
        }
        drawClippedText(draw, ImVec2(textX, textY), nameW, col, shown);

        // ---- 类型列：每一行都画（列头固定在上面） ----
        // 右边界用**本行**自己的右沿而不是列头算出来的那个值：出现竖向滚动条
        // 时行会窄 14px，用列头的值会让类型文字压进行尾的 👁/🔒。
        const float typeLimit =
            std::min(m_typeColRight, rmax.x - (ImGui::GetFontSize() + 6.0f) * 2.0f - 6.0f);
        if (typeLimit > m_typeColX + 8.0f) {
            const float typeW = typeLimit - m_typeColX;
            // 全名放不下 → 退短名（"DirectionalLight" 77px 的列里只能留成
            // "DirLight"；硬截成 "Directio.." 等于没说）。短名也放不下才截断。
            const char* tl = hierTypeLabel(w, e);
            if (ImGui::CalcTextSize(tl).x > typeW) {
                const char* tb = hierTypeLabel(w, e, /*brief=*/true);
                if (ImGui::CalcTextSize(tb).x <= typeW) tl = tb;
            }
            drawClippedText(draw, ImVec2(m_typeColX, textY), typeW,
                            IM_COL32(150, 156, 166, dimmed ? 110 : 200), tl);
        }

        // 记下行矩形给状态列用（见下面"状态列"——必须在右键菜单
        // 之后画，否则 InvisibleButton 会抢走 BeginPopupContextItem
        // 依赖的"最后项"绑定）。
        rowRectMin = rmin;
        rowRectMax = rmax;
    }

    // 调试开关 MYVK_LOG_RECTS=1：记录第一行节点的矩形（给自动化脚本点右键用）。
    // ⚠ 必须限制成"每帧只记一次"：drawNode 是递归的，同一帧里每个节点都会
    //   走到这里，如果都记同一个 tag，tag 的值每帧都在跳 → 日志被刷爆
    //   （真踩过：两分钟写了 30MB，而且取"最后一行"拿到的是随机某一行）。
    if (g_logRectFrame != ImGui::GetFrameCount()) {
        g_logRectFrame = ImGui::GetFrameCount();
        g_logRectRow = e;   // 行尾状态列（HIER-LOCK/HIER-EYE）只记这一行
        logRect("HIER-RECT row0", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());
    }

    // ---- 每一行的矩形（2026-10-27 #316）----
    //
    // 多选的 Ctrl / Shift 点击必须用**真鼠标**点第 N 行（这是修饰键分派唯一
    // 可信的验证方式），而自动化拿不到行坐标就只能瞎猜 —— 行高随 DPI /
    // 主题变，猜一次错一次。
    //
    // 两条节流，缺一条都会出事：
    //   · 最多 16 行（超出的不记，验证脚本用不到那么深）；
    //   · **名字变了才打一次**。drawNode 是递归的、每帧都走一遍，无条件
    //     打就是 60fps × 行数 的日志洪水（这个项目真踩过，两分钟 30MB，
    //     而且取"最后一行"拿到的是随机某一行）。
    if (m_rowCursor < kMaxLoggedRows) {
        const int idx = m_rowCursor++;
        if (m_rowLogNames.size() <= static_cast<std::size_t>(idx))
            m_rowLogNames.resize(static_cast<std::size_t>(idx) + 1);
        if (m_rowLogNames[static_cast<std::size_t>(idx)] != name) {
            m_rowLogNames[static_cast<std::size_t>(idx)] = name;
            const std::string tag = "HIER-ROW " + std::to_string(idx);
            logRect(tag.c_str(), ImGui::GetItemRectMin(),
                    ImGui::GetItemRectMax());
        }
    }

    const bool clicked = ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen();
    if (clicked) {
        // ---- 三种点法（2026-10-27 #316）----
        //   普通点   → 单选（集合被替换成一项）
        //   Ctrl+点  → 加/减一项
        //   Shift+点 → 从"上次点的那项"到这一项的一整段
        // ImGui 的 KeyCtrl 在 macOS 上也会认 Cmd（io.ConfigMacOSXBehaviors），
        // 所以这里不用再区分平台。
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl) {
            m_ctx.selectToggle(e);
            VK_LOG_INFO("HIER-SELECT mode=ctrl id=%u name=%s n=%zu",
                        static_cast<unsigned>(e.id), name.c_str(),
                        m_ctx.selectionCount());
        } else if (io.KeyShift) {
            m_ctx.selectRange(e, m_flatOrder);
            VK_LOG_INFO("HIER-SELECT mode=range id=%u name=%s n=%zu",
                        static_cast<unsigned>(e.id), name.c_str(),
                        m_ctx.selectionCount());
        } else {
            m_ctx.select(e);
            VK_LOG_INFO("HIER-SELECT mode=single id=%u name=%s n=%zu",
                        static_cast<unsigned>(e.id), name.c_str(),
                        m_ctx.selectionCount());
        }
    }

    // 双击 = **在视口里聚焦到它**（2026-10-21 第九轮，用户要求）。
    //   之前双击是"就地重命名"，但用户的第一反应是"双击跳过去看看"——
    //   尤其是视口里点不到的实体（GameStart 只有叠加层标记、水体没有网格），
    //   双击层级是唯一能"跳过去"的手段。
    //   重命名没丢：F2（beginRenameSelection）与右键菜单都还在。
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        m_ctx.select(e);
        // 直接把相机对过去（与 F 键 / 右键菜单共用 EditorFocus.h 那一份）
        editor::focusCameraOnEntity(m_ctx.editorScene(), e);
        VK_LOG_INFO("HIER-FOCUS entity=%u name=%s", static_cast<unsigned>(e.id),
                    name.c_str());
    }

    drawNodeContextMenu(e);

    // ---- 拖拽重挂父级 ----
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        ImGui::SetDragDropPayload(drag::kEntity, &e.id, sizeof(ecs::EntityId));
        ImGui::TextUnformatted(name.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p =
                ImGui::AcceptDragDropPayload(drag::kEntity)) {
            ecs::EntityId childId = ecs::kInvalidEntity;
            if (p->DataSize == sizeof(ecs::EntityId)) {
                std::memcpy(&childId, p->Data, sizeof(ecs::EntityId));
            }
            const ecs::Entity child = w.handle(childId);
            // 不能挂到自己身上，也不能挂到自己的后代（会成环）
            if (child.valid() && child.id != e.id && !isInSubtree(sc, e, child.id)) {
                m_reparentChild = child;
                m_reparentParent = e;
                m_hasReparent = true;
            }
        }
        ImGui::EndDragDropTarget();
    }

    // ---- E12 状态列（右对齐）：👁 可见 / 🔒 锁定 ----
    // 放在右键菜单/dragdrop 之后：BeginPopupContextItem 绑定"最后
    // 一个 item"，这里若先画 InvisibleButton 会把右键菜单吃掉。
    // 锁定 = 视口点选直接跳过（PickingSystem 一直在查
    // LockedComponent，这里是它的开关）。
    {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float fs = ImGui::GetFontSize();
        const float rowH = rowRectMax.y - rowRectMin.y;
        const float colW = fs + 6.0f;
        float ix = rowRectMax.x - colW - 4.0f;
        // 行悬停：用**矩形**判，而不是 IsItemHovered()。
        // 节点开了 AllowOverlap 之后，指针落在行尾图标上时节点会主动让出
        // hover（后提交者优先）→ IsItemHovered() 变 false → 图标会"一亮一暗"
        // 地闪。矩形判只问"指针在不在这一行里"，与命中测试无关，稳定。
        const bool hoveredRow =
            ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
            ImGui::IsMouseHoveringRect(rowRectMin, rowRectMax);
        // 常态半透明，悬停该行才点亮 —— 不与名字抢注意力
        const ImU32 hot = ImGui::GetColorU32(ImGuiCol_Text);
        const ImU32 idle = IM_COL32(150, 152, 158, 90);

        // 🔒 锁定（所有实体都显示；无组件实体加/删 LockedComponent）
        {
            const ImVec2 mn(ix, rowRectMin.y + (rowH - fs) * 0.5f);
            const ImVec2 mx(ix + fs, mn.y + fs);
            const ImU32 col = locked ? IM_COL32(250, 200, 90, 255)
                                     : (hoveredRow ? hot : idle);
            drawLockIcon(draw, mn, mx, locked, col, col);
            ImGui::SetCursorScreenPos(mn);
            ImGui::PushID(static_cast<int>(e.id));
            if (ImGui::InvisibleButton("##lock", ImVec2(colW, rowH))) {
                m_pendingLockToggles.push_back({e, !locked});
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(TR(locked ? TRL("Unlock (viewport can pick)")
                                           : TRL("Lock (viewport cannot pick)")));
            ImGui::PopID();
            if (g_logRectRow.id == e.id)
                logRect("HIER-LOCK row0", mn, ImVec2(mn.x + colW, mn.y + rowH));
            ix -= colW;
        }
        // 👁 可见（**每一行都有**这一格）。
        // 组件缺失 = 可见（`forEachRenderable` 的闸门也是这个语义：没组件
        // 就不过滤）。缺失时点它 → 由 flushPending 补一个组件再写值。
        // ⚠ 别退回 `if (vis)`：太阳（m_lightEntity）曾是**唯一**缺这个组件的
        //   实体（它不走 Scene::createObject），于是它那一行少一个按钮 ——
        //   用户看到的就是"灯的可见性开关不见了"。
        {
            const ImVec2 mn(ix, rowRectMin.y + (rowH - fs) * 0.5f);
            const ImVec2 mx(ix + fs, mn.y + fs);
            const bool shown = vis ? vis->visible : true;
            const ImU32 col = shown ? (hoveredRow ? hot : idle)
                                    : IM_COL32(200, 90, 80, 230);
            drawEyeIcon(draw, mn, mx, shown, col);
            ImGui::SetCursorScreenPos(mn);
            ImGui::PushID(static_cast<int>(e.id));
            if (ImGui::InvisibleButton("##eye", ImVec2(colW, rowH))) {
                m_pendingVisToggles.push_back({e, !shown});
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(TR(shown ? TRL("Hide (visible = false)")
                                           : TRL("Show (visible = true)")));
            ImGui::PopID();
            if (g_logRectRow.id == e.id)
                logRect("HIER-EYE row0", mn, ImVec2(mn.x + colW, mn.y + rowH));
        }
        ImGui::SetCursorScreenPos(ImVec2(rowRectMin.x, rowRectMax.y));
    }

    if (open && hasChildren) {
        // children 里存的是 EntityId，这里换成当前代的完整句柄
        const std::vector<ecs::EntityId> kids = h->children;
        for (ecs::EntityId cid : kids) {
            const ecs::Entity ce = w.handle(cid);
            if (ce.valid()) drawNode(ce, depth + 1);
        }
        ImGui::TreePop();
    }

    ImGui::PopID();
}

void SceneHierarchy::drawNodeContextMenu(ecs::Entity e) {
    if (!ImGui::BeginPopupContextItem("##ctx")) return;

    // ---- 建子物体（UE5 的 Outliner 也是这么放的）----
    if (ImGui::BeginMenu(TR("Create Child"))) {
        static const PrimitiveKind kinds[] = {
            PrimitiveKind::Empty,     PrimitiveKind::Cube,
            PrimitiveKind::Sphere,    PrimitiveKind::Plane,
            PrimitiveKind::PointLight, PrimitiveKind::SpotLight};
        for (PrimitiveKind k : kinds) {
            if (ImGui::MenuItem(primitiveKindName(k))) {
                m_pendingCreate = true;
                m_pendingCreateKind = k;
                m_createParent = e;
                m_createAsFolder = false;
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem(TR("Folder (group)"))) {
            m_pendingCreate = true;
            m_pendingCreateKind = PrimitiveKind::Empty;
            m_createParent = e;
            m_createAsFolder = true;
        }
        ImGui::EndMenu();
    }
    ImGui::Separator();

    if (ImGui::MenuItem(TR("Rename"), "F2")) {
        m_renameTarget = e;
        m_renameFocus = true;
        const std::string* n = m_ctx.editorScene().world().name(e);
        std::snprintf(m_renameBuf, sizeof(m_renameBuf), "%s",
                      n ? n->c_str() : "");
    }
    if (ImGui::MenuItem(TR("Duplicate"), "Ctrl+D")) m_pendingDuplicate = e;

    // ---- 文件夹标记 ----
    // "文件夹"= 纯分组节点（没有网格/灯光/脚本），树里显示成目录图标。
    // 空节点不一定就是文件夹（可能只是还没来得及放东西的容器），
    // 所以标记由用户显式决定，而不是靠"没有组件"去猜。
    const auto* nodeH = m_ctx.editorScene().world().get<ecs::HierarchyComponent>(e);
    const bool isGroup = nodeH && nodeH->group;
    if (ImGui::MenuItem(isGroup ? TRL("Unmark as Folder") : TRL("Mark as Folder"))) {
        m_groupTarget = e;
        m_groupValue = !isGroup;
        m_hasGroupToggle = true;
    }

    // ---- E12：状态快捷开关（与行内 👁/🔒 图标同一 pending 通道）----
    if (const auto* vc = m_ctx.editorScene().world().get<ecs::VisibilityComponent>(e)) {
        if (ImGui::MenuItem(TR(vc->visible ? TRL("Hide") : TRL("Show")))) {
            m_pendingVisToggles.push_back({e, !vc->visible});
        }
    }
    const bool nowLocked =
        m_ctx.editorScene().world().has<ecs::LockedComponent>(e);
    if (ImGui::MenuItem(
            TR(nowLocked ? TRL("Unlock (pickable)") : TRL("Lock (not pickable)")))) {
        m_pendingLockToggles.push_back({e, !nowLocked});
    }

    // ---- G18 预制体 ----
    //
    // 三项放在一起，因为它们是一条链：一份子树 → 一个资产 → 之后每次改
    // 都在"改母版"或"改这一份"之间二选一。分开摆的话用户找不到对应关系。
    //
    // 后两项对"不是 prefab 成员"的对象没有意义：用 BeginDisabled 而不是
    // 直接不画 —— 菜单项忽有忽无会让整张菜单的高度跳变，用户下一次会去
    // 错误的位置点上一秒还在这里的 Delete。
    if (ImGui::MenuItem(TR("Create Prefab from Selection"))) {
        m_prefabCreateTarget = e;
        m_hasPrefabCreate = true;
    }
    const bool isPrefabMember = scene::isPrefabMember(m_ctx.editorScene(), e);
    ImGui::BeginDisabled(!isPrefabMember);
    if (ImGui::MenuItem(TR("Apply to Prefab"))) {
        m_prefabApplyTarget = e;
        m_hasPrefabApply = true;
    }
    if (ImGui::MenuItem(TR("Revert from Prefab"))) {
        m_prefabRevertTarget = e;
        m_hasPrefabRevert = true;
    }
    ImGui::EndDisabled();
    ImGui::Separator();

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.55f, 0.5f, 1.0f));
    // 多选时写明"删几个"：菜单上的 Delete 与 Del 键走的是**同一条**路径
    // （都进 deleteSelection），文案不一致会让人以为右键只删这一个。
    const bool multiHere = m_ctx.multiSelected() && m_ctx.isSelected(e);
    char delLabel[64];
    if (multiHere) {
        std::snprintf(delLabel, sizeof(delLabel), TR("Delete %zu Objects"),
                      m_ctx.selectionCount());
    } else {
        std::snprintf(delLabel, sizeof(delLabel), "%s", TR("Delete"));
    }
    if (ImGui::MenuItem(delLabel, "Del")) m_pendingDelete = e;
    ImGui::PopStyleColor();

    ImGui::Separator();
    if (ImGui::MenuItem(TR("Focus"), "F")) m_pendingFocus = e;
    if (ImGui::MenuItem(TR("Unparent"))) {
        m_unparentTarget = e;
        m_hasUnparent = true;
    }

    // ---- 打开这个对象对应的功能面板 ----
    //
    // 2026-10-18（用户原话："左侧的层级面板中对应的粒子那一项在鼠标右键时
    // 其对应的右键菜单中应该要有打开粒子面板这一选项"）。
    //
    // 为什么值得单开一项：粒子面板是个**浮动面板**，只能从底栏的 + 托盘里
    // 勾出来；而"我在树里看见一个粒子发射器、想调它的参数"是最自然的一次
    // 操作 —— 中间隔着一层"去找底栏的 +"就断了。这里顺手把选中也带过去：
    // 命令的语义是"打开并**编辑这一个**"，不是"打开一个空面板"。
    if (m_ctx.editorScene().world().has<ecs::ParticleEmitterComponent>(e)) {
        if (ImGui::MenuItem(TR("Open Particles Panel"))) {
            m_ctx.select(e);
            const int idx = panelIndexOf("particles");
            if (idx >= 0) setPanelOpen(idx, true);
            VK_LOG_INFO("HIER-OPEN-PANEL particles entity=%u", e.id);
        }
        logRect("HIER-CTX-Particles", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());
    }
    if (m_ctx.editorScene().world().has<ecs::RigidBodyComponent>(e)) {
        if (ImGui::MenuItem(TR("Open Physics Panel"))) {
            m_ctx.select(e);
            const int idx = panelIndexOf("physics");
            if (idx >= 0) setPanelOpen(idx, true);
            VK_LOG_INFO("HIER-OPEN-PANEL physics entity=%u", e.id);
        }
        logRect("HIER-CTX-Physics", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());
    }

    ImGui::EndPopup();
}

// ---------------------------------------------------------------- 主面板

void SceneHierarchy::draw() {
    EditorContext::PanelVisibility& panels = m_ctx.panels();
    if (!panels.hierarchy) return;

    const LayoutRects& L = m_ctx.layout();
    // 分栏布局：面板位置/尺寸每帧由分栏树决定，用 Always 跟随；
    // 不再允许手动拖动面板窗口本身（改由分隔条控制）。
    ImGui::SetNextWindowPos(ImVec2(L.hierarchy.x, L.hierarchy.y),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(L.hierarchy.w, L.hierarchy.h),
                             ImGuiCond_Always);

    const ImGuiWindowFlags wf = ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoResize |
                                ImGuiWindowFlags_NoCollapse;
    if (!ImGui::Begin(trId("Hierarchy", "panel_hierarchy"), &panels.hierarchy, wf)) {
        ImGui::End();
        return;
    }

    scene::Scene& sc = m_ctx.editorScene();

    // ---- 顶部：新建 + 过滤 ----
    if (ImGui::Button(TR("+ Create"))) ImGui::OpenPopup("create_menu");
    if (ImGui::BeginPopup("create_menu")) {
        static const PrimitiveKind kinds[] = {
            PrimitiveKind::Cube,     PrimitiveKind::Sphere,
            PrimitiveKind::Plane,    PrimitiveKind::PointLight,
            PrimitiveKind::SpotLight, PrimitiveKind::Empty};
        for (PrimitiveKind k : kinds) {
            if (ImGui::MenuItem(primitiveKindName(k))) {
                m_pendingCreate = true;
                m_pendingCreateKind = k;
                m_createParent = ecs::Entity{};   // 默认建在根下
                m_createAsFolder = false;
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem(TR("Folder (group)"))) {
            m_pendingCreate = true;
            m_pendingCreateKind = PrimitiveKind::Empty;
            m_createParent = ecs::Entity{};
            m_createAsFolder = true;
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("New Folder"))) {
        m_pendingCreate = true;
        m_pendingCreateKind = PrimitiveKind::Empty;
        m_createParent = ecs::Entity{};
        m_createAsFolder = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Create an empty grouping node named 'Folder'.\n"
                          "Drag other entities onto it to nest them."));
    }
    ImGui::SameLine();
    {
        // 搜索框左侧塞一个小放大镜。做法：把 FramePadding.x 临时撑大 →
        // 输入框的**文字起点右移**（提示语也跟着右移，不会压在图标上），
        // 图标本身画在框内左侧、垂直居中。
        const ImGuiStyle& st = ImGui::GetStyle();
        const float fs = ImGui::GetFontSize();
        const float iconS = fs * 0.82f;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(st.FramePadding.x + iconS + 6.0f,
                                   st.FramePadding.y));
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint(
            "##filter", TRL("search... (type:mesh / type:light)"),
            m_filter, sizeof(m_filter));
        ImGui::PopStyleVar();
        const ImVec2 mn = ImGui::GetItemRectMin();
        const ImVec2 mx = ImGui::GetItemRectMax();
        drawMagnifierIcon(ImGui::GetWindowDrawList(),
                          ImVec2(mn.x + st.FramePadding.x + 3.0f,
                                 (mn.y + mx.y) * 0.5f - iconS * 0.5f),
                          iconS, ImGui::GetColorU32(ImGuiCol_TextDisabled));
        logRect("HIER-SEARCH", mn, mx);
    }

    // 自动化钩子（截图 / 回归测试）：启动时注入一次过滤词，
    // 验证 type: 语法与"过滤保留祖先链"行为。正常使用不设即无影响。
    static bool s_envFilterDone = false;
    if (!s_envFilterDone) {
        s_envFilterDone = true;
        if (const char* envF = std::getenv("MYVK_EDITOR_HIER_FILTER")) {
            if (envF[0] != '\0') {
                std::snprintf(m_filter, sizeof(m_filter), "%s", envF);
                core::logMessage(core::LogLevel::Info,
                                 "HIER-FILTER injected=\"%s\"", envF);
            }
        }
    }

    // ---- E12 面包屑整行已删（2026-10-17 用户要求）----
    //
    // 原来这里是"Path: (no selection)"或可点击的祖先链。用户要求去掉那行字；
    // 顺着这条线整行删掉更符合参照的 UE Outliner（它没有路径行），也解决了
    // 一个真实的坑：这一行**有/没有**会让下面整棵树上下跳一行 —— 点行尾
    // 👁/🔒 时点击落在旧坐标上、松手时那一行已经挪走，表现为"图标点了没反应"
    // （verify_hierarchy_status 的 A2/A3 抓到过）。整行不存在，就不存在
    // "要不要给它留高度"的两难。
    //
    // 顺带一提：ContentBrowser 的目录面包屑也早就因为同样的理由整行删了。

    // ---- 列头：项目标签 | 类型 ----
    //
    // 类型列的 x 用**绝对屏幕坐标**在这里算一次存进成员，树里的每一行都用
    // 同一个值。不能两边各按"自己的左边缘 + 比例"算：列头在树 child 之外、
    // 行在 child 之内，两者的内容起点差了一个 WindowPadding（8px），而且
    // 出现滚动条时行的右边界还会再缩 14px —— 各算各的必然对不齐。
    {
        const float fs = ImGui::GetFontSize();
        const ImVec2 mn = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const float hdrH = fs + 8.0f;
        const ImVec2 mx(mn.x + w, mn.y + hdrH);
        // 行尾状态列占的宽度（👁 + 🔒 两格 + 右边距），类型文字不许压上去
        const float iconsW = (fs + 6.0f) * 2.0f + 10.0f;
        // 宽度分配：**名字列优先**。Hierarchy 默认只有 ~270px 内容宽，三列
        // （名字 / 类型 / 两个状态图标）本来就挤，所以先按"至少 148px 给名字"
        // 划一刀，剩下的才归类型列；面板再窄也要给类型列留 46px，不能被挤没
        // （挤没的后果是两列文字直接叠在一起）。面板拉宽时类型列自动长大。
        // 146 这个数是量出来的：默认宽度下 "DirectionalLight" 需要约 100px，
        // 而名字文字起点在行左缘 +46（缩进 + 折行箭头 + 类型图标）→ 146 刚好
        // 放得下；再小就会截成 "Direction.."。
        float typeX = std::max(mn.x + 146.0f, mx.x - iconsW - 92.0f);
        typeX = std::min(typeX, mx.x - iconsW - 46.0f);
        m_typeColX = typeX;
        m_typeColRight = mx.x - iconsW - 4.0f;

        ImGui::Dummy(ImVec2(w, hdrH));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(mn, mx, ImGui::GetColorU32(ImGuiCol_TableHeaderBg));
        dl->AddLine(ImVec2(typeX, mn.y + 2.0f), ImVec2(typeX, mx.y - 2.0f),
                    ImGui::GetColorU32(ImGuiCol_Separator));
        dl->AddLine(ImVec2(mn.x, mx.y - 0.5f), ImVec2(mx.x, mx.y - 0.5f),
                    ImGui::GetColorU32(ImGuiCol_Separator));

        const ImU32 txt = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        const float eyeS = fs * 0.8f;
        const float eyeY = mn.y + (hdrH - eyeS) * 0.5f;
        // 左边那颗 👁 只是"这一列是可见性"的表意（真正的开关在每一行行尾），
        // 所以画成禁用色、不做命中。
        drawEyeIcon(dl, ImVec2(mn.x + 3.0f, eyeY),
                    ImVec2(mn.x + 3.0f + eyeS, eyeY + eyeS), true, txt);
        const float ty = mn.y + (hdrH - fs) * 0.5f;
        dl->AddText(ImVec2(mn.x + 3.0f + eyeS + 6.0f, ty), txt,
                    TR("Item Label"));
        dl->AddText(ImVec2(typeX + 6.0f, ty), txt, TR("Type"));
        // 自动化锚点：列头矩形 + 类型列左边界（脚本据此断言"两列都在、
        // 类型列没压到行尾图标上"，也顺带证明它画在列表上方）。
        if (std::getenv("MYVK_LOG_RECTS")) {
            VK_LOG_INFO("HIER-COLHDR=(%.0f,%.0f)-(%.0f,%.0f) typeX=%.0f",
                        mn.x, mn.y, mx.x, mx.y, typeX);
        }
    }

    // ---- 树（底部预留 footer 一行）----
    ImGui::BeginChild("##tree", ImVec2(0, -46.0f), ImGuiChildFlags_None);

    // 右键空白处也能新建
    if (ImGui::BeginPopupContextWindow("##empty_ctx",
                                       ImGuiPopupFlags_MouseButtonRight |
                                           ImGuiPopupFlags_NoOpenOverItems)) {
        if (ImGui::MenuItem(TR("New Folder (group)"))) {
            m_pendingCreate = true;
            m_pendingCreateKind = PrimitiveKind::Empty;
            m_createParent = ecs::Entity{};
            m_createAsFolder = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem(TR("New Cube"))) {
            m_pendingCreate = true;
            m_pendingCreateKind = PrimitiveKind::Cube;
            m_createParent = ecs::Entity{};
            m_createAsFolder = false;
        }
        if (ImGui::MenuItem(TR("New Sphere"))) {
            m_pendingCreate = true;
            m_pendingCreateKind = PrimitiveKind::Sphere;
            m_createParent = ecs::Entity{};
            m_createAsFolder = false;
        }
        if (ImGui::MenuItem(TR("New Point Light"))) {
            m_pendingCreate = true;
            m_pendingCreateKind = PrimitiveKind::PointLight;
            m_createParent = ecs::Entity{};
            m_createAsFolder = false;
        }
        if (ImGui::MenuItem(TR("New Empty"))) {
            m_pendingCreate = true;
            m_pendingCreateKind = PrimitiveKind::Empty;
            m_createParent = ecs::Entity{};
            m_createAsFolder = false;
        }
        ImGui::Separator();
        ImGui::TextDisabled(TR("Tip: right-click a node for rename /"));
        ImGui::TextDisabled(TR("duplicate / delete / create child"));
        ImGui::EndPopup();
    }

    // 根节点 = 没有父级的实体
    std::vector<ecs::Entity> roots;
    ecs::World& w = sc.world();
    w.each<ecs::NameComponent>([&](ecs::Entity e, ecs::NameComponent&) {
        const auto* h = w.get<ecs::HierarchyComponent>(e);
        if (!h || h->parent == ecs::kInvalidEntity) roots.push_back(e);
    });
    // 可见行顺序（Shift 范围选择要用）：**先**整棵走一遍再画行，
    // 理由见 m_flatOrder 的注释。
    m_flatOrder.clear();
    for (ecs::Entity e : roots) collectFlatOrder(e);

    m_rowCursor = 0;   // HIER-ROW 的"第几行"从 0 重新数
    for (ecs::Entity e : roots) drawNode(e, 0);

    // ---- E13 拖放落点：树下方的空白区接住从 Content 拖来的模型 ----
    // 放在节点之后：节点自身的 drop target（改父子）优先命中，这里的
    // Dummy 只吃"没落在任何节点上"的释放。
    {
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        // 高度铺满剩余空间、**宽度也要铺满** —— 宽度为 0 的 Dummy 没有可命中的
        // 矩形，真鼠标拖不到（自动化用例也会落在 x 边界外）。
        ImGui::Dummy(ImVec2(avail.x > 1.0f ? avail.x : 1.0f,
                            avail.y > 6.0f ? avail.y : 6.0f));
        const ImVec2 a = ImGui::GetItemRectMin();
        const ImVec2 b = ImGui::GetItemRectMax();
        // 这里原来还画了一行灰色提示 "drop a model here to create an entity"。
        // 2026-10-17 用户要求去掉（树下方那块空白就是落点，文字纯属噪音）。
        // 落点矩形照旧上报 —— verify_asset_drag 靠它算拖放坐标。
        logRect("HIER-RECT drop", a, b);
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(drag::kAsset)) {
                const char* path = static_cast<const char*>(p->Data);
                if (path && path[0]) m_pendingImportRel = path;
            }
            ImGui::EndDragDropTarget();
        }
    }

    ImGui::EndChild();

    ImGui::Separator();
    ImGui::TextDisabled(TR("%zu entities   |   %zu roots"), sc.objectCount(),
                        roots.size());

    ImGui::End();

    flushPending();
}

// ---------------------------------------------------------------- pending

void SceneHierarchy::flushPending() {
    if (!m_ctx.isEditing()) {
        // 播放期间不允许结构性改动：此刻活动场景是 Play 那一刻复制出来的
        // 运行态副本，在这儿增删既不会进历史也不会写回编辑态 ——
        // 与其让用户以为改了没用，不如先挡住，Stop 之后再编辑。
        m_pendingDelete = ecs::Entity{};
        m_pendingDuplicate = ecs::Entity{};
        m_pendingCreate = false;
        m_createParent = ecs::Entity{};
        m_createAsFolder = false;
        m_hasReparent = false;
        m_hasUnparent = false;
        m_hasGroupToggle = false;
        m_groupTarget = ecs::Entity{};
        m_hasRename = false;
        m_pendingFocus = ecs::Entity{};
        m_pendingVisToggles.clear();
        m_pendingLockToggles.clear();
        m_pendingImportRel.clear();
        return;
    }

    scene::Scene& sc = m_ctx.editorScene();
    ecs::World& w = sc.world();

    // E13 拖放导入：模型落到树上 → 在原点生成实体（底部贴栅格）并选中
    if (!m_pendingImportRel.empty()) {
        const std::string rel = m_pendingImportRel;
        m_pendingImportRel.clear();
        ImportResult imported;
        m_ctx.structuralEdit("Import " + rel, [&]() {
            const glm::vec3 spot(0.0f);
            imported = instantiateModel(sc, m_ctx.assets(), rel, spot);
            if (imported.ok()) {
                alignImportToGround(sc, m_picking, imported, spot);
            }
        });
        if (imported.ok()) {
            m_pendingFocus = imported.root;
            m_ctx.notify(std::string(TR("Imported ")) + rel + " (" +
                         std::to_string(imported.count()) + TR(" mesh)"));
            VK_LOG_INFO("HIER-IMPORT rel=%s count=%zu", rel.c_str(),
                        imported.count());
        } else {
            m_ctx.notify(TR("Import failed: ") + rel);
            VK_LOG_INFO("HIER-IMPORT rel=%s FAILED", rel.c_str());
        }
    }

    // 按"安全顺序"执行：新建 → 改名 → 复制 → 改父 → 删除
    if (m_pendingCreate) {
        m_pendingCreate = false;
        const PrimitiveKind kind = m_pendingCreateKind;
        const ecs::Entity parent = m_createParent;
        const bool asFolder = m_createAsFolder;
        m_createParent = ecs::Entity{};
        m_createAsFolder = false;

        ecs::Entity created{};
        m_ctx.structuralEdit(
            std::string("Create ") +
                (asFolder ? "Folder" : primitiveKindName(kind)),
            [&]() {
                created = createPrimitive(sc, m_ctx.assets(), kind,
                                          glm::vec3(0.0f));
                if (created.valid() && asFolder) {
                    // "文件夹"就是**一个空节点当分组用** + 一个显式标记 ——
                    // 引擎没有非实体的目录概念（层级在序列化里是数组下标），
                    // 所以这里老实做成一个名叫 Folder 的空物体，并给
                    // HierarchyComponent::group 打上标记（树里据此画目录图标）。
                    // 名字做去重，否则建第二个文件夹时树里会出现两个 "Folder"。
                    const std::string fname = uniqueName(sc, "Folder");
                    w.setName(created, fname);
                    if (auto* nc = w.get<ecs::NameComponent>(created))
                        nc->name = fname;
                    if (auto* ch = w.get<ecs::HierarchyComponent>(created))
                        ch->group = true;
                }
                if (created.valid() && parent.valid()) {
                    // 建在右键的那一级下面；父级有变换时会自动补偿偏移
                    sc.setParent(created, parent);
                }
            });
        if (created.valid()) {
            m_ctx.select(created);
            m_ctx.dirty() = true;
        }
    }

    if (m_hasRename) {
        m_hasRename = false;
        const ecs::Entity target = m_renameCommit;
        const std::string value = m_renameValue;
        m_renameCommit = ecs::Entity{};
        if (target.valid() && !value.empty()) {
            m_ctx.structuralEdit("Rename " + value, [&]() {
                w.setName(target, value);
                if (auto* nc = w.get<ecs::NameComponent>(target)) nc->name = value;
            });
            m_ctx.select(target);
            m_ctx.dirty() = true;
        }
    }

    if (m_pendingDuplicate.valid()) {
        const ecs::Entity src = m_pendingDuplicate;
        m_pendingDuplicate = ecs::Entity{};
        const std::string* np = w.name(src);
        const std::string newName =
            (np ? *np : std::string("Entity")) + " Copy";
        ecs::Entity dup{};
        m_ctx.structuralEdit("Duplicate " + newName, [&]() {
            dup = duplicateEntity(sc, src, newName);
        });
        if (dup.valid()) {
            m_ctx.select(dup);
            m_ctx.dirty() = true;
        }
    }

    if (m_hasGroupToggle) {
        m_hasGroupToggle = false;
        const ecs::Entity target = m_groupTarget;
        const bool value = m_groupValue;
        m_groupTarget = ecs::Entity{};
        if (target.valid()) {
            m_ctx.structuralEdit(value ? "Mark as Folder" : "Unmark as Folder",
                                 [&]() {
                                     if (auto* h = w.get<ecs::HierarchyComponent>(
                                             target)) {
                                         h->group = value;
                                     }
                                 });
            m_ctx.dirty() = true;
        }
    }

    // ---- E12：状态列切换（👁 可见 / 🔒 锁定）----
    for (const VisToggle& vt : m_pendingVisToggles) {
        if (!vt.e.valid()) continue;
        m_ctx.structuralEdit(vt.value ? "Show entity" : "Hide entity", [&]() {
            if (auto* v = w.get<ecs::VisibilityComponent>(vt.e)) {
                v->visible = vt.value;
            } else {
                // 行内 👁 对"没有这个组件"的行也画得出来（缺失 = 可见），
                // 所以这里要把组件补上再写值，否则点了等于没点。
                ecs::VisibilityComponent nv{};
                nv.visible = vt.value;
                w.add<ecs::VisibilityComponent>(vt.e, nv);
            }
            // G18：可见性也是"字段"，实例上改过就记一条覆盖。
            // 不记的话，下次母版同步会把 👁 悄悄改回去 —— 而用户以为自己
            // 只改了缩放（"我的隐藏又自己弹出来了"是最难联想的症状）。
            scene::recordOverride(sc, vt.e, "visible");
        });
        m_ctx.dirty() = true;
    }
    m_pendingVisToggles.clear();

    for (const auto& [lt, wantLock] : m_pendingLockToggles) {
        if (!lt.valid()) continue;
        m_ctx.structuralEdit(wantLock ? "Lock entity" : "Unlock entity",
                             [&]() {
                                 if (wantLock)
                                     w.add<ecs::LockedComponent>(lt);
                                 else
                                     w.remove<ecs::LockedComponent>(lt);
                                 // 同上：锁定状态也要记账（"我锁住的实例
                                 // 又变回可拖了"是同一类 bug）。
                                 scene::recordOverride(sc, lt,
                                                       "locked");
                             });
        m_ctx.dirty() = true;
    }
    m_pendingLockToggles.clear();

    if (m_hasUnparent) {
        m_hasUnparent = false;
        const ecs::Entity target = m_unparentTarget;
        m_unparentTarget = ecs::Entity{};
        if (target.valid()) {
            m_ctx.structuralEdit("Unparent", [&]() {
                if (auto* h = w.get<ecs::HierarchyComponent>(target)) {
                    if (h->parent != ecs::kInvalidEntity) {
                        const ecs::Entity old = w.handle(h->parent);
                        if (auto* oh = w.get<ecs::HierarchyComponent>(old)) {
                            auto& kids = oh->children;
                            kids.erase(std::remove(kids.begin(), kids.end(),
                                                   target.id),
                                       kids.end());
                        }
                    }
                    h->parent = ecs::kInvalidEntity;
                }
            });
            m_ctx.dirty() = true;
        }
    }

    if (m_hasPrefabCreate) {
        m_hasPrefabCreate = false;
        const ecs::Entity target = m_prefabCreateTarget;
        m_prefabCreateTarget = ecs::Entity{};
        if (target.valid()) {
            const std::string* np = w.name(target);
            // 路径由引擎侧统一取名（撞名自动 _2、_3），编辑器不做第二套口径
            const std::string rel =
                scene::prefabDefaultPath(np ? *np : std::string("prefab"));
            m_ctx.prefabCreateFromSelection(rel);
        }
    }

    if (m_hasPrefabApply) {
        m_hasPrefabApply = false;
        const ecs::Entity target = m_prefabApplyTarget;
        m_prefabApplyTarget = ecs::Entity{};
        if (target.valid()) m_ctx.prefabApply(target);
    }

    if (m_hasPrefabRevert) {
        m_hasPrefabRevert = false;
        const ecs::Entity target = m_prefabRevertTarget;
        m_prefabRevertTarget = ecs::Entity{};
        if (target.valid()) m_ctx.prefabRevert(target);
    }

    if (m_hasReparent) {
        m_hasReparent = false;
        const ecs::Entity child = m_reparentChild;
        const ecs::Entity parent = m_reparentParent;
        m_reparentChild = ecs::Entity{};
        m_reparentParent = ecs::Entity{};
        if (child.valid() && parent.valid()) {
            m_ctx.structuralEdit("Reparent", [&]() {
                sc.setParent(child, parent);
            });
            m_ctx.select(child);
            m_ctx.dirty() = true;
        }
    }

    if (m_pendingDelete.valid()) {
        const ecs::Entity victim = m_pendingDelete;
        m_pendingDelete = ecs::Entity{};
        // 右键点在"已经选中的一员"身上（且多选）→ 删**整个集合**，与 Del
        // 键同一个语义；右键点了个没选中的节点 → 只删它一个（用户指的
        // 就是这一行）。两条路都收敛到 deleteSelection()，别在这儿再写一遍
        // 删除逻辑（树里的孤儿就是这么来的）。
        if (!(m_ctx.multiSelected() && m_ctx.isSelected(victim)))
            m_ctx.select(victim);
        deleteSelection();
    }

    if (m_pendingFocus.valid()) {
        const ecs::Entity target = m_pendingFocus;
        m_pendingFocus = ecs::Entity{};
        sc.camera().setTarget(
            glm::vec3(sc.worldMatrix(target)[3]));
        m_ctx.notify(TR("Focused selection"));
    }
}

// ---------------------------------------------------------------- 快捷键入口
//
// F2 / Ctrl+D 由 EditorApp 统一转发过来（那边才知道当前有没有在输入文本、
// 是不是在 Play 态）。放成公开方法而不是让 App 直接改内部状态，是为了让
// "结构性编辑走 structuralEdit" 这条规则只在这一个类里出现。

void SceneHierarchy::beginRenameSelection() {
    if (!m_ctx.isEditing()) return;
    const ecs::Entity e = m_ctx.selection();
    if (!e.valid()) return;
    const std::string* n = m_ctx.editorScene().world().name(e);
    m_renameTarget = e;
    m_renameFocus = true;
    std::snprintf(m_renameBuf, sizeof(m_renameBuf), "%s", n ? n->c_str() : "");
}

void SceneHierarchy::duplicateSelection() {
    if (!m_ctx.isEditing()) return;
    scene::Scene& sc = m_ctx.editorScene();
    ecs::World& w = sc.world();

    // 一批复制：集合里每一项都复制一份（**一次** structuralEdit —— 撤销
    // 一次全部回滚）。父子关系不复制（duplicateEntity 只带 8 类内容组件），
    // 复制出来的都是根节点下的散件，与单选时代一致。
    std::vector<ecs::Entity> sources;
    for (ecs::Entity e : m_ctx.selectionSet())
        if (e.valid() && w.alive(e)) sources.push_back(e);
    if (sources.empty()) return;

    std::vector<ecs::Entity> dups;
    const std::string onlyName = [&] {
        const std::string* np = w.name(sources.front());
        return (np ? *np : std::string("Entity")) + " Copy";
    }();
    const std::string label = sources.size() > 1
                                  ? "Duplicate " + std::to_string(sources.size()) +
                                        " objects"
                                  : "Duplicate " + onlyName;

    m_ctx.structuralEdit(label, [&] {
        for (ecs::Entity src : sources) {
            const std::string* np = w.name(src);
            // uniqueName 是**边建边查**的：同一批里第二个 "Crate Copy"
            // 会自然变成 "Crate Copy 2"，不用自己维护计数。
            const std::string base = (np ? *np : std::string("Entity")) + " Copy";
            dups.push_back(duplicateEntity(sc, src, uniqueName(sc, base)));
        }
    });

    if (!dups.empty()) {
        m_ctx.selectMany(dups);
        m_ctx.dirty() = true;
        m_ctx.notify(sources.size() > 1
                         ? TR("Duplicated ") + std::to_string(dups.size()) +
                               TR(" objects")
                         : TR("Duplicated ") + onlyName);
        VK_LOG_INFO("HIER-DUP n=%zu", dups.size());
    }
}

void SceneHierarchy::deleteSelection() {
    if (!m_ctx.isEditing()) return;
    scene::Scene& sc = m_ctx.editorScene();
    ecs::World& w = sc.world();

    // ① 先摊平：每个待删实体连**整棵子树**一起删。
    //    destroy() 不递归，只删父节点会留下"parent 指向死 id"的孤儿 ——
    //    它们不在树里（不是根），却还在渲染，等于凭空多出一堆点不中的幽灵。
    std::vector<ecs::Entity> victims;
    for (ecs::Entity e : m_ctx.selectionSet()) {
        if (!e.valid() || !w.alive(e)) continue;
        collectSubtreeIds(w, e, victims);
    }
    if (victims.empty()) return;

    const std::string* np = w.name(victims.front());
    const std::string label =
        victims.size() > 1
            ? "Delete " + std::to_string(victims.size()) + " objects"
            : "Delete " + (np ? *np : std::string("Entity"));

    m_ctx.structuralEdit(label, [&] {
        for (ecs::Entity victim : victims) {
            // 先把自己从父节点的 children 里摘掉，避免留下悬空 id
            if (auto* h = w.get<ecs::HierarchyComponent>(victim)) {
                if (h->parent != ecs::kInvalidEntity) {
                    const ecs::Entity parent = w.handle(h->parent);
                    if (auto* ph = w.get<ecs::HierarchyComponent>(parent)) {
                        auto& kids = ph->children;
                        kids.erase(std::remove(kids.begin(), kids.end(),
                                               victim.id),
                                   kids.end());
                    }
                }
            }
            w.destroy(victim);
        }
    });

    // 集合里的成员全没了 → validateSelection 会按名字逐个找回 / 丢掉，
    // 最后主选中项也一起失效。这里不用手动清。
    m_ctx.validateSelection();
    m_ctx.dirty() = true;
    m_ctx.setStatus(label);
    VK_LOG_INFO("HIER-DEL n=%zu", victims.size());
}

} // namespace editor
