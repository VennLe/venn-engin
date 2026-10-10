#include "SceneHierarchy.h"

#include "DebugRects.h"
#include "EditorDragDrop.h"
#include "EditorIcons.h"
#include "ecs/Components.h"
#include "scene/Camera.h"
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

// ---------------------------------------------------------------- 过滤

bool SceneHierarchy::passesFilter(const std::string& name) const {
    if (m_filter[0] == '\0') return true;
    return name.find(m_filter) != std::string::npos;
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

    if (!passesFilter(name)) return;

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                               ImGuiTreeNodeFlags_SpanAvailWidth |
                               ImGuiTreeNodeFlags_DefaultOpen;
    if (!hasChildren)
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (m_ctx.selection() == e) flags |= ImGuiTreeNodeFlags_Selected;

    // 用后缀标出组件构成，一眼看出这节点是干什么的
    char label[256];
    const auto* mc = w.get<ecs::MeshComponent>(e);
    const auto* pl = w.get<ecs::PointLightComponent>(e);
    const auto* sl = w.get<ecs::SpotLightComponent>(e);
    const auto* scp = w.get<ecs::ScriptComponent>(e);
    const auto* vis = w.get<ecs::VisibilityComponent>(e);
    const auto* dl = w.get<ecs::DirectionalLightComponent>(e);
    const bool locked = w.has<ecs::LockedComponent>(e);

    std::snprintf(label, sizeof(label), "%s%s%s%s%s%s%s%s", name.c_str(),
                  mc ? "  [mesh]" : "", dl ? "  [sun]" : "",
                  (pl || sl) ? "  [light]" : "", scp ? "  [script]" : "",
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

    // ---- 行内容：目录图标（仅文件夹）+ 名字 ----
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
        }

        // 文件夹的名字用暖色（跟目录图标一套），一眼能从一列物体里挑出来
        const ImU32 col = isGroup ? IM_COL32(238, 202, 132, 255)
                                  : ImGui::GetColorU32(ImGuiCol_Text);
        draw->AddText(ImVec2(textX, textY), col, label);
    }

    // 调试开关 MYVK_LOG_RECTS=1：记录第一行节点的矩形（给自动化脚本点右键用）。
    // ⚠ 必须限制成"每帧只记一次"：drawNode 是递归的，同一帧里每个节点都会
    //   走到这里，如果都记同一个 tag，tag 的值每帧都在跳 → 日志被刷爆
    //   （真踩过：两分钟写了 30MB，而且取"最后一行"拿到的是随机某一行）。
    if (g_logRectFrame != ImGui::GetFrameCount()) {
        g_logRectFrame = ImGui::GetFrameCount();
        logRect("HIER-RECT row0", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());
    }

    const bool clicked = ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen();
    if (clicked) m_ctx.select(e);

    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        m_renameTarget = e;
        m_renameFocus = true;
        std::snprintf(m_renameBuf, sizeof(m_renameBuf), "%s", name.c_str());
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
    if (ImGui::BeginMenu("Create Child")) {
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
        if (ImGui::MenuItem("Folder (group)")) {
            m_pendingCreate = true;
            m_pendingCreateKind = PrimitiveKind::Empty;
            m_createParent = e;
            m_createAsFolder = true;
        }
        ImGui::EndMenu();
    }
    ImGui::Separator();

    if (ImGui::MenuItem("Rename", "F2")) {
        m_renameTarget = e;
        m_renameFocus = true;
        const std::string* n = m_ctx.editorScene().world().name(e);
        std::snprintf(m_renameBuf, sizeof(m_renameBuf), "%s",
                      n ? n->c_str() : "");
    }
    if (ImGui::MenuItem("Duplicate", "Ctrl+D")) m_pendingDuplicate = e;

    // ---- 文件夹标记 ----
    // "文件夹"= 纯分组节点（没有网格/灯光/脚本），树里显示成目录图标。
    // 空节点不一定就是文件夹（可能只是还没来得及放东西的容器），
    // 所以标记由用户显式决定，而不是靠"没有组件"去猜。
    const auto* nodeH = m_ctx.editorScene().world().get<ecs::HierarchyComponent>(e);
    const bool isGroup = nodeH && nodeH->group;
    if (ImGui::MenuItem(isGroup ? "Unmark as Folder" : "Mark as Folder")) {
        m_groupTarget = e;
        m_groupValue = !isGroup;
        m_hasGroupToggle = true;
    }

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.55f, 0.5f, 1.0f));
    if (ImGui::MenuItem("Delete", "Del")) m_pendingDelete = e;
    ImGui::PopStyleColor();

    ImGui::Separator();
    if (ImGui::MenuItem("Focus", "F")) m_pendingFocus = e;
    if (ImGui::MenuItem("Unparent")) {
        m_unparentTarget = e;
        m_hasUnparent = true;
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
    if (!ImGui::Begin("Hierarchy", &panels.hierarchy, wf)) {
        ImGui::End();
        return;
    }

    scene::Scene& sc = m_ctx.editorScene();

    // ---- 顶部：新建 + 过滤 ----
    if (ImGui::Button("+ Create")) ImGui::OpenPopup("create_menu");
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
        if (ImGui::MenuItem("Folder (group)")) {
            m_pendingCreate = true;
            m_pendingCreateKind = PrimitiveKind::Empty;
            m_createParent = ecs::Entity{};
            m_createAsFolder = true;
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("New Folder")) {
        m_pendingCreate = true;
        m_pendingCreateKind = PrimitiveKind::Empty;
        m_createParent = ecs::Entity{};
        m_createAsFolder = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Create an empty grouping node named 'Folder'.\n"
                          "Drag other entities onto it to nest them.");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##filter", "search...", m_filter,
                             sizeof(m_filter));

    ImGui::Separator();

    // ---- 树 ----
    ImGui::BeginChild("##tree", ImVec2(0, -28.0f), ImGuiChildFlags_None);

    // 右键空白处也能新建
    if (ImGui::BeginPopupContextWindow("##empty_ctx",
                                       ImGuiPopupFlags_MouseButtonRight |
                                           ImGuiPopupFlags_NoOpenOverItems)) {
        if (ImGui::MenuItem("New Folder (group)")) {
            m_pendingCreate = true;
            m_pendingCreateKind = PrimitiveKind::Empty;
            m_createParent = ecs::Entity{};
            m_createAsFolder = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("New Cube")) {
            m_pendingCreate = true;
            m_pendingCreateKind = PrimitiveKind::Cube;
            m_createParent = ecs::Entity{};
            m_createAsFolder = false;
        }
        if (ImGui::MenuItem("New Sphere")) {
            m_pendingCreate = true;
            m_pendingCreateKind = PrimitiveKind::Sphere;
            m_createParent = ecs::Entity{};
            m_createAsFolder = false;
        }
        if (ImGui::MenuItem("New Point Light")) {
            m_pendingCreate = true;
            m_pendingCreateKind = PrimitiveKind::PointLight;
            m_createParent = ecs::Entity{};
            m_createAsFolder = false;
        }
        if (ImGui::MenuItem("New Empty")) {
            m_pendingCreate = true;
            m_pendingCreateKind = PrimitiveKind::Empty;
            m_createParent = ecs::Entity{};
            m_createAsFolder = false;
        }
        ImGui::Separator();
        ImGui::TextDisabled("Tip: right-click a node for rename /");
        ImGui::TextDisabled("duplicate / delete / create child");
        ImGui::EndPopup();
    }

    // 根节点 = 没有父级的实体
    std::vector<ecs::Entity> roots;
    ecs::World& w = sc.world();
    w.each<ecs::NameComponent>([&](ecs::Entity e, ecs::NameComponent&) {
        const auto* h = w.get<ecs::HierarchyComponent>(e);
        if (!h || h->parent == ecs::kInvalidEntity) roots.push_back(e);
    });
    for (ecs::Entity e : roots) drawNode(e, 0);

    ImGui::EndChild();

    ImGui::Separator();
    ImGui::TextDisabled("%zu entities   |   %zu roots", sc.objectCount(),
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
        return;
    }

    scene::Scene& sc = m_ctx.editorScene();
    ecs::World& w = sc.world();

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
        const std::string* np = w.name(victim);
        const std::string label =
            "Delete " + (np ? *np : std::string("Entity"));
        m_ctx.structuralEdit(label, [&]() {
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
        });
        m_ctx.validateSelection();
        m_ctx.dirty() = true;
        m_ctx.setStatus(label);
    }

    if (m_pendingFocus.valid()) {
        const ecs::Entity target = m_pendingFocus;
        m_pendingFocus = ecs::Entity{};
        sc.camera().setTarget(
            glm::vec3(sc.worldMatrix(target)[3]));
        m_ctx.notify("Focused selection");
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
    const ecs::Entity src = m_ctx.selection();
    if (!src.valid()) return;

    scene::Scene& sc = m_ctx.editorScene();
    ecs::World& w = sc.world();
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
        m_ctx.notify("Duplicated " + newName);
    }
}

} // namespace editor
