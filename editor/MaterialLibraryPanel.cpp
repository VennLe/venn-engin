#include "MaterialLibraryPanel.h"

#include "DebugRects.h"
#include "LandscapeEditor.h"
#include "Localization.h"
#include "PanelDock.h"
#include "PanelRegistry.h"

#include "assets/AssetManager.h"
#include "assets/AssetPath.h"
#include "assets/MaterialLibrary.h"
#include "core/Logger.h"
#include "ecs/Components.h"
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace editor {

namespace {

constexpr float kCellW = 136.0f;   // 一张卡片的宽度（含间距）
constexpr float kCardLabelH = 34.0f;

// 按当前语言取条目的显示名（数据自带中英两份，见 trBilingual 的注释）
const char* labelOf(const assets::MaterialLibraryEntry& e) {
    return trBilingual(e.label, e.labelZh);
}
const char* categoryOf(const assets::MaterialLibraryEntry& e) {
    return trBilingual(e.category, e.categoryZh);
}

// 大小写不敏感的子串匹配（搜索框）
bool matchesFilter(const assets::MaterialLibraryEntry& e, const char* needle) {
    if (!needle || !needle[0]) return true;
    std::string hay = e.name + " " + e.id + " " + e.label + " " + e.labelZh +
                      " " + e.category + " " + e.categoryZh;
    std::string pat = needle;
    auto lower = [](std::string s) {
        for (char& c : s) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return s;
    };
    return lower(hay).find(lower(pat)) != std::string::npos;
}

} // namespace

// ============================================================
// 卡片
// ============================================================
void MaterialLibraryPanel::drawCard(const assets::MaterialLibraryEntry& e,
                                   float cellW) {
    ImGui::PushID(e.id.c_str());
    const float imgW = cellW - 10.0f;
    const float imgH = imgW * 0.72f;   // 略扁：地面纹理是横向铺开的
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const bool selected = (m_selected == e.id);

    // 整张卡片 = 一个不可见按钮。先占位、再自己往上画 ——
    // 这样"点哪里都算点到这张卡片"，不用为缩略图和文字各判一次命中。
    ImGui::InvisibleButton("card", ImVec2(cellW, imgH + kCardLabelH));
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked()) {
        m_selected = e.id;
        VK_LOG_INFO("MATLIB-SEL id=%s", e.id.c_str());
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        // 双击 = 按当前上下文里最可能的那个动作下手：
        // 地形模式刷地形，否则赋给选中物体。省掉"先选再点按钮"两步。
        if (m_land && m_land->hasTerrain() &&
            m_ctx.editMode() == EditMode::Landscape) {
            if (m_land->setLayerMaterial(m_land->paintLayer(), e.id)) {
                m_land->beginPaintLayer(m_land->paintLayer());
                m_ctx.notify(std::string(TR("Terrain brush: ")) + labelOf(e));
                VK_LOG_INFO("MATLIB-USE-BRUSH id=%s layer=%d via=double-click",
                            e.id.c_str(), m_land->paintLayer());
            }
        } else {
            applyToSelection(e);
        }
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 border = selected ? IM_COL32(122, 190, 255, 255)
                                  : (hovered ? IM_COL32(160, 168, 190, 220)
                                             : IM_COL32(74, 78, 94, 190));

    // 缩略图底板（贴图取不到时它就是"颜色块 + 名字"，也不算空）
    const ImVec2 q0(p0.x + 5.0f, p0.y + 5.0f);
    const ImVec2 q1(q0.x + imgW, q0.y + imgH);
    const ImU32 baseCol = ImGui::ColorConvertFloat4ToU32(
        ImVec4(e.baseColor.r, e.baseColor.g, e.baseColor.b, 1.0f));
    dl->AddRectFilled(q0, q1, baseCol, 4.0f);

    if (e.hasAlbedo()) {
        const std::string abs = assets::resolveAssetPath(e.albedoPath);
        if (const ThumbnailCache::Entry* t = m_thumbs.get(m_ctx.assets(), abs)) {
            // 居中裁剪成卡片比例：等比缩放到"盖满"，再按 imgW/imgH 裁边
            const float srcAspect = (t->h > 0.0f) ? (t->w / t->h) : 1.0f;
            const float dstAspect = imgW / std::max(imgH, 1.0f);
            ImVec2 uv0(0.0f, 0.0f), uv1(1.0f, 1.0f);
            if (srcAspect > dstAspect) {
                const float k = dstAspect / srcAspect;
                uv0.x = (1.0f - k) * 0.5f;
                uv1.x = 1.0f - uv0.x;
            } else if (srcAspect < dstAspect) {
                const float k = srcAspect / dstAspect;
                uv0.y = (1.0f - k) * 0.5f;
                uv1.y = 1.0f - uv0.y;
            }
            dl->AddImageRounded(
                static_cast<ImTextureID>(
                    reinterpret_cast<std::uintptr_t>(t->ds)),
                q0, q1, uv0, uv1, IM_COL32_WHITE, 4.0f);
        }
    }

    dl->AddRect(q0, q1, border, 4.0f, 0, selected ? 2.0f : 1.0f);

    // 名字（超出宽度就省略号）
    const ImVec2 tp(p0.x + 6.0f, q1.y + 2.0f);
    dl->PushClipRect(ImVec2(tp.x, tp.y), ImVec2(p0.x + cellW - 2.0f, tp.y + 18.0f),
                     true);
    dl->AddText(tp, selected ? IM_COL32(210, 232, 255, 255)
                             : IM_COL32(216, 220, 232, 255),
                labelOf(e));
    dl->PopClipRect();
    // 第二行：分类 · roughness
    char meta[96];
    std::snprintf(meta, sizeof(meta), "%s  r=%.2f", categoryOf(e),
                  static_cast<double>(e.roughness));
    dl->AddText(ImVec2(tp.x, tp.y + 17.0f), IM_COL32(150, 156, 172, 255), meta);

    ImGui::PopID();
}

// ============================================================
// 把材质赋给选中实体
// ============================================================
int MaterialLibraryPanel::applyToSelection(
    const assets::MaterialLibraryEntry& e) {
    if (!m_ctx.hasSelection()) {
        m_ctx.setStatus(TR("Apply material: select a mesh in the viewport or Hierarchy first"));
        VK_LOG_INFO("MATLIB-APPLY-SKIP id=%s reason=no-selection", e.id.c_str());
        return 0;
    }
    scene::Scene& sc = m_ctx.activeScene();
    ecs::World& w = sc.world();

    // 材质解析**必须**走 SceneSerializer 那条路：.vmat 的 id 去重、贴图
    // 解析、导入默认值都在里面。自己 new 一个 Material 出来会变成一个
    // 谁都认不出的孤儿对象（关掉工程再打开就丢了）。
    assets::Material* mat =
        scene::loadMaterialAsset(m_ctx.assets(), e.vmatPath);
    if (!mat) {
        VK_LOG_WARN("MATLIB-APPLY-FAIL id=%s path=%s reason=vmat-load", 
                    e.id.c_str(), e.vmatPath.c_str());
        m_ctx.setStatus(TR("Material could not be loaded (see Console)"));
        return 0;
    }

    // 收集目标：选中项自己 + 它下面所有带 MaterialComponent 的子孙。
    // 模型导入出来的结构是"根空物体 + 每个 primitive 一个子实体"，
    // 只改根是什么都不会发生的。
    std::vector<ecs::Entity> targets;
    std::vector<ecs::Entity> stack{m_ctx.selection()};
    while (!stack.empty()) {
        const ecs::Entity cur = stack.back();
        stack.pop_back();
        if (!cur.valid()) continue;
        if (w.has<ecs::MaterialComponent>(cur)) targets.push_back(cur);
        if (const auto* h = w.get<ecs::HierarchyComponent>(cur)) {
            for (const ecs::EntityId cid : h->children) {
                stack.push_back(w.handle(cid));
            }
        }
    }
    if (targets.empty()) {
        m_ctx.setStatus(TR("Selected object has no material slot (it is not a mesh)"));
        VK_LOG_INFO("MATLIB-APPLY-SKIP id=%s reason=no-material-slot",
                    e.id.c_str());
        return 0;
    }

    const std::string label = "Apply material " + e.name;
    m_ctx.structuralEdit(label, [this, &targets, mat]() {
        ecs::World& ww = m_ctx.activeScene().world();
        for (const ecs::Entity t : targets) {
            if (auto* mc = ww.get<ecs::MaterialComponent>(t)) mc->material = mat;
        }
    });
    m_ctx.setStatus(label);
    VK_LOG_INFO("MATLIB-APPLY id=%s entity=%u meshes=%d", e.id.c_str(),
                static_cast<unsigned>(m_ctx.selection().id),
                static_cast<int>(targets.size()));
    return static_cast<int>(targets.size());
}

// ============================================================
// 详情区
// ============================================================
void MaterialLibraryPanel::drawDetails(const assets::MaterialLibraryEntry& e) {
    ImGui::SeparatorText(TR("Details"));
    ImGui::TextUnformatted(labelOf(e));
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", e.category.empty() ? "?" : categoryOf(e));
    ImGui::TextDisabled("%s", e.id.c_str());
    ImGui::TextDisabled(TR("file: %s"), e.vmatPath.c_str());

    if (ImGui::BeginTable("##matlib_props", 2,
                          ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_RowBg)) {
        auto row = [](const char* k, const char* v) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", k);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(v);
        };
        char buf[512];
        std::snprintf(buf, sizeof(buf), "%.3f", static_cast<double>(e.roughness));
        row(TR("Roughness"), buf);
        std::snprintf(buf, sizeof(buf), "%.3f", static_cast<double>(e.metallic));
        row(TR("Metallic"), buf);
        std::snprintf(buf, sizeof(buf), "%.3f", static_cast<double>(e.ao));
        row(TR("AO"), buf);
        row(TR("Tier"), e.tier.empty() ? "-" : e.tier.c_str());
        row("albedo", e.hasAlbedo() ? e.albedoPath.c_str() : "-");
        row("normal", e.hasNormal() ? e.normalPath.c_str() : "-");
        row("orm", e.ormPath.empty() ? "-" : e.ormPath.c_str());
        ImGui::EndTable();
    }

    // ---------------------------------------------------------- 动作
    const bool hasTerrain = (m_land && m_land->hasTerrain());
    ImGui::BeginDisabled(!hasTerrain);
    if (ImGui::Button(TR("Use as terrain brush"), ImVec2(-1.0f, 0.0f))) {
        const int layer = std::clamp(m_land ? m_land->paintLayer() : 0, 0, 3);
        if (m_land && m_land->setLayerMaterial(layer, e.id)) {
            m_land->beginPaintLayer(layer);
            if (m_ctx.editMode() != EditMode::Landscape) {
                m_ctx.setEditMode(EditMode::Landscape);
            }
            m_ctx.notify(std::string(TR("Terrain brush: ")) + labelOf(e));
            VK_LOG_INFO("MATLIB-USE-BRUSH id=%s layer=%d", e.id.c_str(), layer);
        }
    }
    ImGui::EndDisabled();
    if (!hasTerrain && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", TR("No terrain in this scene. Create one in "
                                   "Landscape mode first."));
    }
    ImGui::TextDisabled(TR("Binds this material to terrain layer %d, then "
                           "switches to the layer-paint brush."),
                        m_land ? m_land->paintLayer() : 0);

    if (ImGui::Button(TR("Apply to selection"), ImVec2(-1.0f, 0.0f))) {
        applyToSelection(e);
    }
    ImGui::TextDisabled("%s", TR("Replaces the material of the selected mesh "
                                 "(and of every sub-mesh under it)."));
}

// ============================================================
// 面板
// ============================================================
void MaterialLibraryPanel::draw() {
    bool& open = panelOpenRef("matlib");
    if (!open) return;

    ImGuiWindowFlags wf = 0;
    const bool docked = applyDockHint(wf);
    if (!docked) {
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowSizeConstraints(ImVec2(420.0f, 420.0f),
                                            ImVec2(FLT_MAX, FLT_MAX));
        ImGui::SetNextWindowSize(ImVec2(560.0f, 700.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.30f,
                                       io.DisplaySize.y * 0.12f),
                                ImGuiCond_FirstUseEver);
    }

    if (!ImGui::Begin(trId("Material library", "panel_matlib"),
                      docked ? nullptr : &open, wf)) {
        ImGui::End();
        return;
    }
    logRect("MATLIB-WIN", ImGui::GetWindowPos(),
            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                   ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    assets::MaterialLibrary& lib = m_ctx.materialLibrary();

    // 仓库被重载过（面板自己的 Refresh、或者属性栏里"从目录导入材质"）→
    // 缩略图缓存整批失效：贴图可能换了档，条目也可能是新导入的。
    if (m_libRevision != m_ctx.materialLibraryRevision()) {
        m_libRevision = m_ctx.materialLibraryRevision();
        m_thumbs.clear();
    }

    // ---------------------------------------------------------- 头
    ImGui::TextDisabled(TR("%d material(s), tier %s"),
                        static_cast<int>(lib.size()),
                        lib.size() ? lib.entries().front().tier.c_str() : "-");
    ImGui::SameLine();
    if (ImGui::SmallButton(TR("Refresh"))) {
        m_ctx.reloadMaterialLibrary();
        // 贴图可能换了档（1k → 2k），缩略图必须跟着失效
        m_thumbs.clear();
    }
    if (lib.size() == 0) {
        ImGui::Separator();
        ImGui::TextUnformatted(TR("The material library is empty."));
        ImGui::TextDisabled("%s", TR("Run tools/gen_material_library.py to "
                                     "extract it from venn-resource."));
        ImGui::End();
        return;
    }

    // ---------------------------------------------------------- 筛选
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##matlib_filter", TR("Search name / category"),
                             m_filter, sizeof(m_filter));

    const std::vector<std::string> cats = lib.categories();
    if (ImGui::Button(TR("All"))) m_category = -1;
    for (int i = 0; i < static_cast<int>(cats.size()); ++i) {
        ImGui::SameLine();
        // 分类名从第一条同分类的条目上取中文标签
        const char* cname = cats[i].c_str();
        for (const auto& e : lib.entries()) {
            if (e.category == cats[i]) {
                cname = categoryOf(e);
                break;
            }
        }
        const bool on = (m_category == i);
        if (on) ImGui::PushStyleColor(ImGuiCol_Button,
                                      ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(cname)) m_category = i;
        if (on) ImGui::PopStyleColor();
    }

    // ---------------------------------------------------------- 货架
    ImGui::Separator();
    const float availW = ImGui::GetContentRegionAvail().x;
    const int cols = std::max(1, static_cast<int>(availW / kCellW));
    int shown = 0;
    ImGui::BeginChild("##matlib_grid", ImVec2(0.0f, 0.0f), 0, 0);
    for (const auto& e : lib.entries()) {
        if (m_category >= 0 && m_category < static_cast<int>(cats.size()) &&
            e.category != cats[static_cast<std::size_t>(m_category)]) {
            continue;
        }
        if (!matchesFilter(e, m_filter)) continue;
        if (shown % cols) ImGui::SameLine();
        drawCard(e, std::min(kCellW, availW / static_cast<float>(cols)));
        ++shown;
    }
    if (shown == 0) {
        ImGui::TextDisabled("%s", TR("Nothing matches the filter."));
    }
    // 详情区放在同一个滚动区里，跟着货架往下走
    const assets::MaterialLibraryEntry* sel = lib.find(m_selected);
    if (!sel && !lib.entries().empty()) {
        sel = lib.defaultEntry();
        m_selected = sel ? sel->id : std::string();
    }
    if (sel) {
        ImGui::Spacing();
        drawDetails(*sel);
    }
    ImGui::EndChild();

    ImGui::End();
}

} // namespace editor
