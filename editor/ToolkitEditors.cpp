#include "ToolkitEditors.h"

#include "DebugRects.h"
#include "EditModes.h"
#include "Localization.h"
#include "ModeRail.h"
#include "PanelRegistry.h"
#include "PickingSystem.h"

#include "assets/AssetManager.h"
#include "i18n/I18n.h"
#include "scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace editor {

namespace {

// 与 LandscapeEditor 里那份一致：标题单独一行 + 满宽控件。
// 复制一份而不是抽公共头：这里只有三处用到，而抽出去要改 LandscapeEditor
// 那边的调用点（那个文件正在被改动，不想同时开两个战场）。
void rowSlider(const char* label, const char* id, float* v, float lo, float hi,
               const char* fmt, bool logScale = false) {
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderFloat(id, v, lo, hi, fmt,
                       logScale ? ImGuiSliderFlags_Logarithmic : 0);
}

void rowSliderInt(const char* label, const char* id, int* v, int lo, int hi) {
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderInt(id, v, lo, hi);
}

// 12 位掩码的确定性随机（同一个种子永远给出同一串）。
// 不用 std::rand：编辑器里"重新生成一次"必须能复现，否则"上次那批石头
// 摆得挺好"就再也回不来了。
struct Rng {
    std::uint32_t s;
    explicit Rng(std::uint32_t seed) : s(seed ? seed : 1u) {}
    std::uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    float unit() { return static_cast<float>(next() & 0x00FFFFFFu) / 16777216.0f; }
    float range(float a, float b) { return a + (b - a) * unit(); }
};

// 顶点法线重算（按面积加权）。镜像之后法线是反的、细分之后法线是旧的 ——
// 这两件事都只能重算，没有"只修一半"的说法。
void recomputeNormals(std::vector<assets::Vertex>& verts,
                      const std::vector<std::uint32_t>& idx) {
    for (auto& v : verts) v.normal = glm::vec3(0.0f);
    for (std::size_t i = 0; i + 2 < idx.size(); i += 3) {
        const std::uint32_t a = idx[i], b = idx[i + 1], c = idx[i + 2];
        if (a >= verts.size() || b >= verts.size() || c >= verts.size()) continue;
        const glm::vec3 n = glm::cross(verts[b].pos - verts[a].pos,
                                       verts[c].pos - verts[a].pos);
        // 叉积长度 = 2×面积 → 直接累加就自带面积加权
        verts[a].normal += n;
        verts[b].normal += n;
        verts[c].normal += n;
    }
    for (auto& v : verts) {
        const float len = glm::length(v.normal);
        v.normal = (len > 1e-8f) ? (v.normal / len) : glm::vec3(0.0f, 0.0f, 1.0f);
    }
}

} // namespace

// ============================================================
// 公共：面板窗口 + 选中网格
// ============================================================
// 2026-10-21：八个模式面板不再是浮动窗口，而是**左栏下半那一块**
// （用户要求把"模式类型选择列表 + 各模式的配置参数"统一放左侧）。
//
// 具体做法：窗口矩形每帧钉到 `modeRailBodyRect(ctx.layout())`，并且
// NoTitleBar | NoMove | NoResize | NoCollapse —— 标题由上面那 9 行单选
// 表达（当前模式高亮），再挂一条标题栏只是白占 30px 高度。
//
// 为什么仍然用**独立顶层窗口**而不是主栏里的 BeginChild：
//   面板代码几十处 `ImGui::End()`，改成 EndChild 得逐处改，漏一个就是
//   Assert。顶层窗口只要换矩形，8 个模式的 drawPanel 一行都不用动。
bool beginToolkitWindow(EditorContext& ctx, const char* id, const char* title,
                        float w, float h) {
    bool& open = panelOpenRef(id);
    if (!open) return false;

    char win[128];
    std::snprintf(win, sizeof(win), "%s###panel_%s", TR(title), id);

    ImGuiWindowFlags wf = 0;
    if (beginModeRailPanel(ctx, wf)) {
        if (!ImGui::Begin(win, nullptr, wf)) {
            ImGui::End();
            return false;
        }
        char tag[64];
        std::snprintf(tag, sizeof(tag), "TK-%s", id);
        logRect(tag, ImGui::GetWindowPos(),
                ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                       ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));
        return true;
    }

    // 兜底：左栏被拖到折叠（宽度归零）时退回浮动窗口 —— 否则模式面板会
    // 彻底失联（矩形 0 宽，点也点不到）。
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSizeConstraints(ImVec2(380.0f, 300.0f),
                                        ImVec2(FLT_MAX, FLT_MAX));
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_FirstUseEver);
    const float px = io.DisplaySize.x * 0.60f;
    ImGui::SetNextWindowPos(ImVec2(px, io.DisplaySize.y * 0.10f),
                            ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(win, &open)) {
        ImGui::End();
        return false;
    }
    if (!open) {
        ImGui::End();
        return false;
    }
    char tag[64];
    std::snprintf(tag, sizeof(tag), "TK-%s", id);
    logRect(tag, ImGui::GetWindowPos(),
            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                   ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));
    return true;
}

bool selectedMesh(EditorContext& ctx, ecs::Entity& outEntity,
                  assets::Mesh*& outMesh, glm::mat4& outWorld) {
    ecs::Entity e = ctx.selection();
    if (!e.valid()) return false;
    ecs::World& w = ctx.editorScene().world();
    if (!w.alive(e)) return false;
    auto* mc = w.get<ecs::MeshComponent>(e);
    if (!mc || !mc->mesh || !mc->mesh->valid()) return false;
    outEntity = e;
    outMesh = mc->mesh;
    outWorld = ctx.editorScene().worldMatrix(e);
    return true;
}

// ------------------------------------------------------------
// Brush / PCG 生成实体时用的材质
// ------------------------------------------------------------
// ⚠ **材质指针不能为空**。渲染层对空材质的处理是
//     `if (!it.material) continue;  // 空材质只有深度预通道会画`
// 也就是说：实体存在、层级里看得见、深度里挡光，但**颜色通道直接跳过** ——
// 用户看到的就是"撒了 120 个实例，屏幕上什么都没有"，而且日志里一条错误
// 都没有（这正是第一版的样子：`findMaterial("materials/default")` 而这个
// 资产根本不存在）。
//
// 所以这里按"优先用像样的材质，退而求其次程序化造一份"的顺序解析：
//   1. 先试 `preferred`（调用方按种类给的，比如岩石用 terrain_rock）；
//   2. 再试几份常见材质 —— 它们是否已加载取决于当前场景加载过什么；
//   3. 都没有就用 `makeMaterialPBR` 造一份无贴图的纯色 PBR 材质。
//      `makeMaterialPBR` 对同名同参数是幂等的，所以反复调用不会越造越多。
assets::Material* resolveToolkitMaterial(EditorContext& ctx,
                                         const char* preferred,
                                         const glm::vec4& tint) {
    assets::AssetManager& am = ctx.assets();
    if (preferred) {
        if (assets::Material* m = am.findMaterial(preferred)) return m;
    }
    static const char* kFallbacks[] = {"materials/default", "materials/ground",
                                       "materials/crate", "materials/wall"};
    for (const char* f : kFallbacks) {
        if (assets::Material* m = am.findMaterial(f)) return m;
    }
    char key[64];
    std::snprintf(key, sizeof(key), "toolkit/mat/%s",
                  preferred ? preferred : "plain");
    assets::Material* made =
        am.makeMaterialPBR(key, nullptr, nullptr, nullptr, tint,
                           /*roughness=*/0.65f, /*metallic=*/0.0f,
                           glm::vec3(0.0f), /*doubleSided=*/false);
    VK_LOG_INFO("TK-MAT-FALLBACK key=%s（场景里没有可复用材质，已程序化生成）",
                key);
    return made;
}

// ============================================================
// 1. 网格体绘制
// ============================================================
void MeshPaintEditor::sync() {
    ecs::Entity e{};
    assets::Mesh* mesh = nullptr;
    glm::mat4 world(1.0f);
    if (!selectedMesh(m_ctx, e, mesh, world)) {
        if (m_bound) VK_LOG_INFO("MP-UNBIND");
        m_bound = false;
        m_entity = ecs::Entity{};
        m_src = nullptr;
        m_verts.clear();
        m_idx.clear();
        return;
    }
    if (m_bound && m_src == mesh && m_entity.id == e.id) return;

    // 换绑：把源网格的顶点拷成 CPU 副本（之后所有染色都在副本上做）
    m_entity = e;
    m_src = mesh;
    m_verts = mesh->vertices();
    m_idx = mesh->indices();
    m_bound = true;
    m_rev = 0;
    m_paintedVertices = 0;
    for (const auto& v : m_verts) {
        if (v.color.r < 0.999f || v.color.g < 0.999f || v.color.b < 0.999f) {
            ++m_paintedVertices;
        }
    }
    VK_LOG_INFO("MP-BIND entity=%u verts=%d painted=%d", e.id,
                static_cast<int>(m_verts.size()), m_paintedVertices);
}

void MeshPaintEditor::applyToMesh() {
    if (!m_bound || m_entity.id == 0) return;
    ecs::World& w = m_ctx.editorScene().world();
    if (!w.alive(m_entity)) return;

    // ⚠ 每次上传都要一个新键：registerMesh 对同名键是"直接复用"，
    //   复用同一个键 = 第二次染色什么都不会发生（而且完全没有报错）。
    char key[96];
    std::snprintf(key, sizeof(key), "toolkit/paint/%u/%d", m_entity.id, ++m_rev);
    assets::Mesh* mesh = m_ctx.assets().registerMesh(key, m_verts, m_idx);
    if (!mesh) return;
    if (auto* mc = w.get<ecs::MeshComponent>(m_entity)) {
        mc->mesh = mesh;
        mc->params = ecs::MeshPrimitiveParams{};   // 不再是内置图元
    }
    m_ctx.dirty() = true;
    ++m_applyTimer;
    if (m_applyTimer > 0.5f) {
        m_applyTimer = 0.0f;
        VK_LOG_INFO("MP-APPLY rev=%d painted=%d", m_rev, m_paintedVertices);
    }
}

bool MeshPaintEditor::raycastMesh(const glm::vec3& origin, const glm::vec3& dir,
                                  glm::vec3& hit) {
    if (!m_bound || m_idx.size() < 3) return false;
    // 世界 → 网格局部（只做刚体/均匀缩放变换时，用逆矩阵把射线搬进去最省）
    const glm::mat4 world = m_ctx.editorScene().worldMatrix(m_entity);
    const glm::mat4 inv = glm::inverse(world);
    const glm::vec3 o = glm::vec3(inv * glm::vec4(origin, 1.0f));
    const glm::vec3 d = glm::vec3(inv * glm::vec4(dir, 0.0f));

    float best = 1.0e30f;
    for (std::size_t i = 0; i + 2 < m_idx.size(); i += 3) {
        const glm::vec3& a = m_verts[m_idx[i]].pos;
        const glm::vec3& b = m_verts[m_idx[i + 1]].pos;
        const glm::vec3& c = m_verts[m_idx[i + 2]].pos;
        // Möller–Trumbore
        const glm::vec3 e1 = b - a, e2 = c - a;
        const glm::vec3 p = glm::cross(d, e2);
        const float det = glm::dot(e1, p);
        if (std::fabs(det) < 1e-8f) continue;
        const float invDet = 1.0f / det;
        const glm::vec3 t = o - a;
        const float u = glm::dot(t, p) * invDet;
        if (u < 0.0f || u > 1.0f) continue;
        const glm::vec3 q = glm::cross(t, e1);
        const float v = glm::dot(d, q) * invDet;
        if (v < 0.0f || u + v > 1.0f) continue;
        const float dist = glm::dot(e2, q) * invDet;
        if (dist > 1e-5f && dist < best) best = dist;
    }
    if (best > 1.0e29f) return false;
    hit = glm::vec3(world * glm::vec4(o + d * best, 1.0f));
    return true;
}

bool MeshPaintEditor::viewportInput(const glm::vec2& vpPos,
                                    const glm::vec2& vpSize, bool hovered,
                                    ImDrawList* dl) {
    sync();
    if (!m_bound) return false;

    const ImGuiIO& io = ImGui::GetIO();
    const glm::vec2 mouse = io.MousePos.x != 0.0f || io.MousePos.y != 0.0f
                               ? glm::vec2(io.MousePos.x, io.MousePos.y)
                               : glm::vec2(-1.0f);
    const Ray ray = PickingSystem::rayFromViewport(
        m_ctx.editorScene().camera(), vpPos, vpSize, mouse);
    glm::vec3 hit(0.0f);
    const bool onMesh = (hovered || m_painting) &&
                        raycastMesh(ray.origin, ray.dir, hit);

    // ---- 笔刷光标：贴在命中点上的三段圆环（屏幕空间）+ 世界半径提示 ----
    if (onMesh && dl) {
        glm::vec2 s;
        const glm::mat4 vp =
            PickingSystem::viewProj(m_ctx.editorScene().camera(), vpSize);
        if (PickingSystem::worldToScreen(vp, vpPos, vpSize, hit, s)) {
            const ImU32 col = m_erase ? IM_COL32(150, 150, 150, 220)
                                      : ImGui::ColorConvertFloat4ToU32(
                                            ImVec4(m_color.r, m_color.g,
                                                   m_color.b, 0.9f));
            dl->AddCircleFilled(ImVec2(s.x, s.y), 4.0f, col);
            dl->AddCircle(ImVec2(s.x, s.y), 18.0f, col, 40, 1.6f);
            // 中心十字：小半径时纯圆环会盖住目标点
            dl->AddLine(ImVec2(s.x - 28.0f, s.y), ImVec2(s.x - 8.0f, s.y), col, 1.0f);
            dl->AddLine(ImVec2(s.x + 8.0f, s.y), ImVec2(s.x + 28.0f, s.y), col, 1.0f);
        }
    }

    const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    const bool navGesture = ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
                            ImGui::IsMouseDown(ImGuiMouseButton_Middle);

    // 滚轮 = 半径（与地形笔刷同一套手感；不做成快捷键，理由见 LandscapeEditor）
    bool wheelConsumed = false;
    if (onMesh && hovered && !m_painting && !navGesture && !io.KeyCtrl &&
        io.MouseWheel != 0.0f) {
        m_radius = std::clamp(m_radius * std::pow(1.15f, io.MouseWheel), 0.02f,
                              50.0f);
        VK_LOG_INFO("MP-BRUSH radius=%.3f", static_cast<double>(m_radius));
        wheelConsumed = true;
    }

    if (clicked && hovered && onMesh && !navGesture) {
        m_painting = true;
        m_strokeTouched = 0;
        VK_LOG_INFO("MP-STROKE-BEGIN radius=%.3f at=(%.1f,%.1f,%.1f)",
                    static_cast<double>(m_radius), static_cast<double>(hit.x),
                    static_cast<double>(hit.y), static_cast<double>(hit.z));
    }

    bool consumed = false;
    if (m_painting) {
        consumed = true;
        if (down && onMesh && !navGesture) {
            // 逐顶点：世界位置到命中点的距离 < radius 的按权重染色。
            // 权重用 smoothstep 而不是线性 —— 线性在边界上是一圈硬边，
            // 画出来是"一个个圆盘"，而不是一片渐变。
            const glm::mat4 world = m_ctx.editorScene().worldMatrix(m_entity);
            const glm::vec3 target = m_erase ? glm::vec3(1.0f) : m_color;
            const float r2 = m_radius * m_radius;
            int touched = 0;
            for (auto& v : m_verts) {
                const glm::vec3 wp = glm::vec3(world * glm::vec4(v.pos, 1.0f));
                const float d2 = glm::dot(wp - hit, wp - hit);
                if (d2 > r2) continue;
                float t = 1.0f - std::sqrt(d2) / m_radius;
                t = t * t * (3.0f - 2.0f * t);
                const float k = std::clamp(m_strength * t * 0.6f, 0.0f, 1.0f);
                v.color = glm::vec4(glm::mix(glm::vec3(v.color), target, k),
                                    v.color.a);
                ++touched;
            }
            if (touched > 0) {
                m_strokeTouched += touched;
                applyToMesh();
            }
        }
        if (released) {
            m_painting = false;
            m_paintedVertices = 0;
            for (const auto& v : m_verts) {
                if (v.color.r < 0.999f || v.color.g < 0.999f ||
                    v.color.b < 0.999f) {
                    ++m_paintedVertices;
                }
            }
            VK_LOG_INFO("MP-STROKE-END touched=%d painted=%d rev=%d",
                        m_strokeTouched, m_paintedVertices, m_rev);
        }
    }
    return consumed || wheelConsumed || (onMesh && hovered);
}

void MeshPaintEditor::drawPanel() {
    if (!beginToolkitWindow(m_ctx, "meshpaint", "Mesh Paint", 420.0f, 560.0f)) return;
    ImGui::TextDisabled("%s", TR(editModeHint(EditMode::MeshPaint)));
    ImGui::Separator();

    sync();
    if (!m_bound) {
        ImGui::TextWrapped("%s",
                           TR("Select an entity that has a mesh. Painting "
                              "writes per-vertex colors, so the change travels "
                              "with the mesh (and survives LOD reduction)."));
        ImGui::End();
        return;
    }

    ImGui::Text(TR("Vertices: %d  ·  colored: %d"),
                static_cast<int>(m_verts.size()), m_paintedVertices);
    ImGui::Text(TR("Uploads: %d  (one per stroke step)"), m_rev);
    ImGui::Separator();

    ImGui::SeparatorText(TR("Brush"));
    ImGui::ColorEdit3(TR("Color"), &m_color.x, ImGuiColorEditFlags_NoInputs);
    rowSlider(TR("Radius"), "##mp_radius", &m_radius, 0.02f, 50.0f, "%.2f m",
              true);
    rowSlider(TR("Strength"), "##mp_strength", &m_strength, 0.02f, 1.0f, "%.2f");
    ImGui::Checkbox(TR("Erase (back to white)"), &m_erase);

    ImGui::Spacing();
    if (ImGui::Button(TR("Reset all colors"), ImVec2(-1.0f, 26.0f))) {
        for (auto& v : m_verts) v.color = glm::vec4(1.0f);
        m_paintedVertices = 0;
        applyToMesh();
        VK_LOG_INFO("MP-CLEAR verts=%d", static_cast<int>(m_verts.size()));
    }
    // 自动化锚点：这个面板里唯一一个"动数据"的按钮，验收脚本要靠它点。
    logRect("MP-RESET", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::Spacing();
    ImGui::TextDisabled("%s",
                        TR("Left-drag on the mesh to paint. Scroll = radius. "
                           "Colors multiply the material albedo, so a fully "
                           "white vertex is untouched."));
    ImGui::End();
}

// ============================================================
// 2. 建模（Blender Edit Mode 那一套）
// ============================================================
//
// 与第一版的差别：第一版是四个**全局**按钮（整个网格细分 / 整个网格平滑），
// 那不是建模 —— 那是"给网格换个形状"，连"只想倒一个角"都做不到。
//
// 现在有真正的编辑模式：
//   · 三层选择（1 顶点 / 2 边 / 3 面），视口里点选、Shift 加选、Ctrl 减选；
//   · A 全选 / Alt+A 清空；
//   · G 拖动 · E 挤出 · S 缩放 · R 旋转 · X 删除面 · M 合并（Blender 键位）；
//   · 只对选区生效的细分 / 翻转；整网格的平滑 / 镜像留在 Shape 段里。
//
// ------------------------------------------------------------
// 为什么必须先按位置 weld
// ------------------------------------------------------------
// GPU 缓冲里的"顶点"是**渲染顶点**：立方体的一个角有 3 份（三个面的法线
// 不同），球体的极点有 N 份（UV 接缝）。直接按渲染顶点做选择的话：
//   · "选一个角"只能选到其中一份 → 拖动它，那个面被拽出来，模型当场裂开；
//   · "选一条边"在同一条几何边上会数出两条；
//   · "选一个面"按三角形处理没问题，但拖动它的顶点仍然裂。
// 所以先按**位置**（量化到 0.1 mm）焊接成 weld 组，所有选择与变换都在组
// 上做，写回时把组位置广播给该组的所有渲染顶点。
//
// ------------------------------------------------------------
// 选区为什么只有"一份顶点集合"
// ------------------------------------------------------------
// 三份独立选区（点 / 边 / 面）在每次操作后都要同步，是一定会写漏的地方。
// 这里：权威 = `m_sel`（weld 组 id），边和面都由它推出来 ——
//   选中的面 = 三个顶点**全**在 `m_sel` 里的三角形
//   选中的边 = 两端点**都**在 `m_sel` 里的边
// 于是切到顶点模式看到的点，与切回来时看到的面天然一致，不需要同步代码。
namespace {

// 位置量化键：0.1 mm 的分辨率。太细（1e-6）会让本该共享的顶点被法线误差
// 分开，太粗（1 mm）会把薄壁的两面焊成一个。
struct PosKey {
    int x, y, z;
};
struct PosKeyHash {
    std::size_t operator()(const PosKey& k) const noexcept {
        // 三个 32 位整数各自乘一个质数再异或（经典的空间哈希组合）
        std::uint64_t h = static_cast<std::uint32_t>(k.x) * 73856093ull;
        h ^= static_cast<std::uint32_t>(k.y) * 19349663ull;
        h ^= static_cast<std::uint32_t>(k.z) * 83492791ull;
        return static_cast<std::size_t>(h);
    }
};
struct PosKeyEq {
    bool operator()(const PosKey& a, const PosKey& b) const noexcept {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }
};

PosKey quantizePos(const glm::vec3& p) {
    constexpr float kQ = 10000.0f;  // 1/10000 米 = 0.1 mm
    return PosKey{static_cast<int>(std::lround(p.x * kQ)),
                  static_cast<int>(std::lround(p.y * kQ)),
                  static_cast<int>(std::lround(p.z * kQ))};
}

// 三角形与射线的 Möller–Trumbore。**双面**求交：建模时经常要选背面的面
// （从外面看一个盒子的内壁），背面剔除会让"点了没反应"变成玄学。
bool rayTriangle(const glm::vec3& ro, const glm::vec3& rd, const glm::vec3& a,
                 const glm::vec3& b, const glm::vec3& c, float& outT) {
    const glm::vec3 e1 = b - a;
    const glm::vec3 e2 = c - a;
    const glm::vec3 p = glm::cross(rd, e2);
    const float det = glm::dot(e1, p);
    if (std::fabs(det) < 1e-9f) return false;
    const float inv = 1.0f / det;
    const glm::vec3 tv = ro - a;
    const float u = glm::dot(tv, p) * inv;
    if (u < -1e-5f || u > 1.0f + 1e-5f) return false;
    const glm::vec3 q = glm::cross(tv, e1);
    const float v = glm::dot(rd, q) * inv;
    if (v < -1e-5f || u + v > 1.0f + 1e-5f) return false;
    const float t = glm::dot(e2, q) * inv;
    if (t <= 1e-5f) return false;
    outT = t;
    return true;
}

// 屏幕上的点线距离（像素）—— 顶点/边模式下用它做命中判定，比 3D 距离更符合
// "我看到哪个就点哪个"的直觉。
float screenSegDist(const glm::vec2& p, const glm::vec2& a, const glm::vec2& b) {
    const glm::vec2 ab = b - a;
    const float len2 = glm::dot(ab, ab);
    float t = 0.0f;
    if (len2 > 1e-9f) t = std::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f);
    return glm::length(p - (a + ab * t));
}

} // namespace

void ModelingEditor::rebuildTopo() {
    Topo& t = m_topo;
    t.weldOf.assign(m_verts.size(), 0);
    t.weldVerts.clear();
    t.weldPos.clear();
    t.tri.clear();
    t.triVert.clear();
    t.triNormal.clear();

    std::unordered_map<PosKey, std::uint32_t, PosKeyHash, PosKeyEq> bucket;
    bucket.reserve(m_verts.size());
    for (std::size_t i = 0; i < m_verts.size(); ++i) {
        const PosKey k = quantizePos(m_verts[i].pos);
        auto it = bucket.find(k);
        std::uint32_t id;
        if (it == bucket.end()) {
            id = static_cast<std::uint32_t>(t.weldPos.size());
            bucket.emplace(k, id);
            t.weldPos.push_back(m_verts[i].pos);
            t.weldVerts.emplace_back();
        } else {
            id = it->second;
        }
        t.weldOf[i] = id;
        t.weldVerts[id].push_back(static_cast<std::uint32_t>(i));
    }

    for (std::size_t i = 0; i + 2 < m_idx.size(); i += 3) {
        const std::uint32_t a = m_idx[i], b = m_idx[i + 1], c = m_idx[i + 2];
        if (a >= m_verts.size() || b >= m_verts.size() || c >= m_verts.size())
            continue;
        t.triVert.push_back({a, b, c});
        t.tri.push_back({t.weldOf[a], t.weldOf[b], t.weldOf[c]});
        const glm::vec3 n = glm::cross(m_verts[b].pos - m_verts[a].pos,
                                       m_verts[c].pos - m_verts[a].pos);
        const float len = glm::length(n);
        t.triNormal.push_back(len > 1e-12f ? n / len
                                           : glm::vec3(0.0f, 0.0f, 1.0f));
    }

    // ------------------------------------------------------------
    // 多边形层：把共面且共享一条边的两个三角形并成一个面
    // ------------------------------------------------------------
    // 渲染网格只有三角形，但**建模看的是面**。立方体一个正方形面在缓冲里
    // 是两个三角形，如果照着三角形画选区、做操作，用户看到的就是"每个
    // 正方形都被一条对角线劈成两半"——这就是用户报的那个显示问题。
    //
    // 只做**成对**合并（一个三角形最多和邻居合并一次），不做传递合并。
    // 传递合并会把一块细分过的平面并成一整张 n 边形：选一个格子等于选中
    // 整个地面，环切和内插也无从下手。成对合并给出的正好是"网格单元"
    // ——立方体 6 个四边形、平面每格一个四边形，和 Blender 的观感一致。
    t.poly.clear();
    t.innerEdge.clear();
    t.triPoly.assign(t.tri.size(), 0);
    {
        auto ekey = [](std::uint32_t a, std::uint32_t b) {
            if (a > b) std::swap(a, b);
            return (static_cast<std::uint64_t>(a) << 32) | b;
        };
        auto newell = [](const std::vector<std::uint32_t>& v,
                         const std::vector<glm::vec3>& pos) {
            glm::vec3 n(0.0f);
            for (std::size_t i = 0; i < v.size(); ++i) {
                const glm::vec3& a = pos[v[i]];
                const glm::vec3& b = pos[v[(i + 1) % v.size()]];
                n.x += (a.y - b.y) * (a.z + b.z);
                n.y += (a.z - b.z) * (a.x + b.x);
                n.z += (a.x - b.x) * (a.y + b.y);
            }
            return n;
        };

        // 面表有两种来源：
        //   ① 拓扑操作刚做完 → `m_polyHint` 里就是真面表（能还原 n 边形）；
        //   ② 刚加载/刚绑定 → 没有提示，只能靠"共面两两合并"从三角形反推。
        // ② 的极限就是四边形（倒角后的八边形会被切成好几块），所以①优先。
        const bool useHint = [&] {
            if (m_polyHint.empty()) return false;
            std::size_t sum = 0;
            for (std::uint32_t c : m_polyHint) sum += c;
            return sum == t.tri.size();
        }();

        if (useHint) {
            std::size_t ti = 0;
            for (std::uint32_t count : m_polyHint) {
                if (count < 1 || ti + count > t.tri.size()) break;
                Topo::Poly p;
                p.tris.reserve(count);
                for (std::uint32_t k = 0; k < count; ++k)
                    p.tris.push_back(static_cast<std::uint32_t>(ti + k));
                // 扇形三角化 (v0,v1,v2), (v0,v2,v3)… 的逆运算
                p.v.push_back(t.tri[ti][0]);
                p.v.push_back(t.tri[ti][1]);
                for (std::uint32_t k = 0; k < count; ++k)
                    p.v.push_back(t.tri[ti + k][2]);
                const glm::vec3 n = newell(p.v, t.weldPos);
                const float len = glm::length(n);
                p.area = len * 0.5f;
                p.normal = len > 1e-12f ? n / len : t.triNormal[p.tris[0]];
                const std::uint32_t pi = static_cast<std::uint32_t>(t.poly.size());
                for (std::uint32_t tri : p.tris) t.triPoly[tri] = pi;
                t.poly.push_back(std::move(p));
                ti += count;
            }
            // 提示与实际三角形对不上（被别的操作改过）→ 剩下的自己成面，
            // 至少不会把三角形归到错误的面里。
            for (; ti < t.tri.size(); ++ti) {
                Topo::Poly p;
                p.tris.push_back(static_cast<std::uint32_t>(ti));
                p.v = {t.tri[ti][0], t.tri[ti][1], t.tri[ti][2]};
                p.normal = t.triNormal[ti];
                p.area = 0.0f;
                t.triPoly[ti] = static_cast<std::uint32_t>(t.poly.size());
                t.poly.push_back(std::move(p));
            }
        } else {
            std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> edgeTris;
            edgeTris.reserve(t.tri.size() * 3);
            for (std::uint32_t f = 0; f < t.tri.size(); ++f) {
                for (int k = 0; k < 3; ++k)
                    edgeTris[ekey(t.tri[f][k], t.tri[f][(k + 1) % 3])].push_back(f);
            }
            std::vector<int> partner(t.tri.size(), -1);
            for (const auto& kv : edgeTris) {
                if (kv.second.size() != 2) continue;   // 非流形边不合并
                const std::uint32_t a = kv.second[0], b = kv.second[1];
                if (partner[a] >= 0 || partner[b] >= 0) continue;
                // 0.99995 ≈ 0.57°：法线差一点点就不合并，避免把球面磨成多边形
                if (glm::dot(t.triNormal[a], t.triNormal[b]) < 0.99995f) continue;
                partner[a] = static_cast<int>(b);
                partner[b] = static_cast<int>(a);
            }

            // 两个三角形的有向边互相抵消，剩下的就是合并后的外轮廓。
            auto boundaryLoop = [&](std::uint32_t triA, std::uint32_t triB) {
                std::unordered_map<std::uint64_t,
                                   std::pair<std::uint32_t, std::uint32_t>> keep;
                auto addDir = [&](std::uint32_t a, std::uint32_t b) {
                    const std::uint64_t k = ekey(a, b);
                    auto it = keep.find(k);
                    if (it == keep.end()) keep.emplace(k, std::make_pair(a, b));
                    else keep.erase(it);              // 反向边 → 内部边，抵消
                };
                const std::uint32_t pair[2] = {triA, triB};
                for (std::uint32_t f : pair)
                    for (int k = 0; k < 3; ++k)
                        addDir(t.tri[f][k], t.tri[f][(k + 1) % 3]);
                std::unordered_map<std::uint32_t,
                                   std::pair<std::uint32_t, std::uint32_t>> out;
                for (const auto& kv : keep) out[kv.second.first] = kv.second;
                std::vector<std::uint32_t> loop;
                if (out.empty()) return loop;
                const std::uint32_t start = out.begin()->second.first;
                std::uint32_t cur = start;
                for (std::size_t guard = 0; guard <= keep.size(); ++guard) {
                    loop.push_back(cur);
                    auto it = out.find(cur);
                    if (it == out.end()) break;
                    cur = it->second.second;
                    if (cur == start) break;
                }
                return loop;
            };

            for (std::uint32_t f = 0; f < t.tri.size(); ++f) {
                const int mate = partner[f];
                // 伙伴编号小于自己的，说明这一对已经发过了
                if (mate >= 0 && static_cast<std::uint32_t>(mate) < f) continue;
                Topo::Poly p;
                p.tris.push_back(f);
                if (mate >= 0) {
                    p.v = boundaryLoop(f, static_cast<std::uint32_t>(mate));
                    p.tris.push_back(static_cast<std::uint32_t>(mate));
                }
                if (p.v.size() < 3)
                    p.v = {t.tri[f][0], t.tri[f][1], t.tri[f][2]};
                const glm::vec3 n = newell(p.v, t.weldPos);
                const float len = glm::length(n);
                p.area = len * 0.5f;
                p.normal = len > 1e-12f ? n / len : t.triNormal[f];
                const std::uint32_t pi = static_cast<std::uint32_t>(t.poly.size());
                for (std::uint32_t tri : p.tris) t.triPoly[tri] = pi;
                t.poly.push_back(std::move(p));
            }


        }
        m_polyHint.clear();

        // 多边形内部的边（三角形的对角线）：边模式下不画、不可选。
        // 不排掉的话，用户能在立方体一个正方形面上选中一条"本来不存在"的
        // 对角线 —— 那正是这次要修的东西。
        std::unordered_set<std::uint64_t> boundary;
        boundary.reserve(t.poly.size() * 4);
        for (const auto& p : t.poly)
            for (std::size_t i = 0; i < p.v.size(); ++i)
                boundary.insert(ekey(p.v[i], p.v[(i + 1) % p.v.size()]));
        for (const auto& tr : t.tri)
            for (int k = 0; k < 3; ++k) {
                const std::uint64_t key = ekey(tr[k], tr[(k + 1) % 3]);
                if (!boundary.count(key)) t.innerEdge.insert(key);
            }
    }

    t.valid = true;
    m_vertices = static_cast<int>(m_verts.size());
    m_triangles = static_cast<int>(t.tri.size());
}

void ModelingEditor::bind() {
    ecs::Entity e{};
    assets::Mesh* mesh = nullptr;
    glm::mat4 world(1.0f);
    if (!selectedMesh(m_ctx, e, mesh, world)) {
        m_entity = ecs::Entity{};
        m_src = nullptr;
        m_verts.clear();
        m_idx.clear();
        m_topo = Topo{};
        m_sel.clear();
        m_vertices = 0;
        m_triangles = 0;
        m_selectedVerts = m_selectedFaces = m_selectedEdges = 0;
        return;
    }
    m_entity = e;
    m_src = mesh;
    m_verts = mesh->vertices();
    m_idx = mesh->indices();
    m_sel.clear();
    rebuildTopo();
    // `polys=` 是 2026-10-21 加的：立方体必须报 6（六个四边形），
    // 报 12 就说明共面合并没生效 —— 那正是"每个正方形被对角线劈开"的根。
    VK_LOG_INFO("MODEL-BIND entity=%u verts=%d welded=%d tris=%d polys=%d",
                static_cast<unsigned>(e.id), m_vertices,
                static_cast<int>(m_topo.weldPos.size()), m_triangles,
                static_cast<int>(m_topo.poly.size()));
}

void ModelingEditor::sync() {
    ecs::Entity e = m_ctx.selection();
    assets::Mesh* mesh = nullptr;
    if (e.valid()) {
        ecs::World& w = m_ctx.editorScene().world();
        if (w.alive(e)) {
            if (auto* mc = w.get<ecs::MeshComponent>(e)) mesh = mc->mesh;
        }
    }
    // 只有"选中的实体或它挂的网格换了"才重拉 —— 每帧重拉会把本地还没上传
    // 的编辑丢掉（拖动过程中每帧都会被清回原样，看起来像"拖不动"）。
    if (e.id == m_entity.id && mesh == m_src && m_topo.valid) {
        m_selectedVerts = static_cast<int>(m_sel.size());
        // 面数按**多边形**计：面板上写 "3 faces" 时，用户心里数的是立方体
        // 上的三个正方形，不是六个三角形。
        m_selectedFaces = static_cast<int>(selPolys().size());
        m_selectedEdges = 0;
        for (std::size_t f = 0; f < m_topo.tri.size(); ++f) {
            const auto& tr = m_topo.tri[f];
            for (int k = 0; k < 3; ++k) {
                const std::uint32_t a = tr[k], b = tr[(k + 1) % 3];
                if (a >= b || !m_sel.count(a) || !m_sel.count(b)) continue;
                // 与覆盖层同一套口径：三角形对角线不算边
                const std::uint64_t key =
                    (static_cast<std::uint64_t>(a) << 32) | b;
                if (m_topo.innerEdge.count(key)) continue;
                ++m_selectedEdges;
            }
        }
        return;
    }
    bind();
}

std::vector<std::uint32_t> ModelingEditor::selFaces() const {
    std::vector<std::uint32_t> out;
    if (!m_topo.valid) return out;
    out.reserve(16);
    for (std::size_t f = 0; f < m_topo.tri.size(); ++f) {
        const auto& tr = m_topo.tri[f];
        if (m_sel.count(tr[0]) && m_sel.count(tr[1]) && m_sel.count(tr[2]))
            out.push_back(static_cast<std::uint32_t>(f));
    }
    return out;
}

// 选中多边形：所有顶点都在 m_sel 里的面。
//
// 这是面模式下**真正的选区单位**。`selFaces()`（三角形）仍然保留 ——
// 挤出 / 细分 / 删除这些老操作按三角形走完全等价（一个四边形的两个三角形
// 会一起被选中），而且不用改它们的实现。
std::vector<std::uint32_t> ModelingEditor::selPolys() const {
    std::vector<std::uint32_t> out;
    if (!m_topo.valid) return out;
    for (std::size_t i = 0; i < m_topo.poly.size(); ++i) {
        const auto& p = m_topo.poly[i];
        bool all = !p.v.empty();
        for (std::uint32_t w : p.v) {
            if (!m_sel.count(w)) {
                all = false;
                break;
            }
        }
        if (all) out.push_back(static_cast<std::uint32_t>(i));
    }
    return out;
}

glm::vec3 ModelingEditor::selectionCenter() const {
    if (m_sel.empty()) return glm::vec3(0.0f);
    glm::vec3 sum(0.0f);
    int n = 0;
    for (std::uint32_t id : m_sel) {
        if (id < m_topo.weldPos.size()) {
            sum += m_topo.weldPos[id];
            ++n;
        }
    }
    return (n > 0) ? (sum / static_cast<float>(n)) : glm::vec3(0.0f);
}

glm::vec3 ModelingEditor::selectionNormal() const {
    // 用**多边形**法线而不是三角形法线：挤出一个正方形面时，两条三角形
    // 法线本来就一样，结果相同；但选中多个面时按面积加权会更稳（不会被
    // 一堆细碎三角形把方向拽偏）。
    const std::vector<std::uint32_t> polys = selPolys();
    glm::vec3 n(0.0f);
    for (std::uint32_t p : polys) {
        if (p < m_topo.poly.size())
            n += m_topo.poly[p].normal * std::max(m_topo.poly[p].area, 1e-6f);
    }
    if (glm::length(n) < 1e-6f) {
        for (std::uint32_t f : selFaces())
            if (f < m_topo.triNormal.size()) n += m_topo.triNormal[f];
    }
    const float len = glm::length(n);
    return (len > 1e-6f) ? (n / len) : glm::vec3(0.0f, 0.0f, 1.0f);
}

void ModelingEditor::selectAll(bool on) {
    m_sel.clear();
    if (on) {
        for (std::uint32_t i = 0; i < m_topo.weldPos.size(); ++i) m_sel.insert(i);
    }
    VK_LOG_INFO("MODEL-SEL mode=%s verts=%d faces=%d", on ? "all" : "none",
                static_cast<int>(m_sel.size()),
                static_cast<int>(selPolys().size()));
}

// 把 weld 位置广播回所有渲染顶点 → 重算法线 → 注册新网格 → 换指针。
//
// 法线必须在**渲染顶点**上重算（不是 weld 组上）：立方体的一个角有三份
// 顶点分属三个面，按组平均会把硬边磨圆 —— 那个立方体就变成了"圆角盒子"。
// recomputeNormals 本来就是按原始顶点下标累加的，正好。
void ModelingEditor::upload(const char* op) {
    if (!m_topo.valid || m_entity.id == 0) return;

    for (std::size_t w = 0; w < m_topo.weldVerts.size(); ++w) {
        for (std::uint32_t vi : m_topo.weldVerts[w]) {
            if (vi < m_verts.size()) m_verts[vi].pos = m_topo.weldPos[w];
        }
    }
    recomputeNormals(m_verts, m_idx);

    char key[96];
    std::snprintf(key, sizeof(key), "toolkit/model/%u/%d",
                  static_cast<unsigned>(m_entity.id), ++m_rev);
    assets::Mesh* created = m_ctx.assets().registerMesh(key, m_verts, m_idx);
    if (!created) {
        VK_LOG_ERROR("MODEL-OP-FAIL op=%s entity=%u", op,
                     static_cast<unsigned>(m_entity.id));
        return;
    }
    ecs::World& w = m_ctx.editorScene().world();
    if (auto* mc = w.get<ecs::MeshComponent>(m_entity)) {
        mc->mesh = created;
        mc->params = ecs::MeshPrimitiveParams{};
    }
    m_src = created;
    rebuildTopo();
    m_ctx.dirty() = true;
    VK_LOG_INFO("MODEL-OP op=%s verts=%d->%d tris=%d polys=%d rev=%d selV=%d "
                "selF=%d",
                op, m_beforeVerts, m_vertices, m_triangles,
                static_cast<int>(m_topo.poly.size()), m_rev,
                static_cast<int>(m_sel.size()),
                static_cast<int>(selPolys().size()));
}

// ------------------------------------------------------------ 拓扑操作

void ModelingEditor::extrude() {
    m_beforeVerts = static_cast<int>(m_verts.size());
    const std::vector<std::uint32_t> faces = selFaces();
    if (faces.empty()) {
        VK_LOG_INFO("MODEL-EXTRUDE skip=no-face-selected");
        m_ctx.notify(TR("Extrude needs at least one selected face."));
        return;
    }
    const glm::vec3 n = selectionNormal();
    const float d = m_extrude;

    // 1) 选中面用到的**原始顶点**各复制一份，沿法线推出去。
    //    只复制选中面的顶点（不是整个 weld 组）：相邻面要留在原地，缝隙
    //    由侧面补上 —— 这正是 Blender 的 extrude region 行为。
    std::unordered_map<std::uint32_t, std::uint32_t> pushOut;
    pushOut.reserve(faces.size() * 3);
    for (std::uint32_t f : faces) {
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t vi = m_topo.triVert[f][k];
            if (pushOut.count(vi)) continue;
            assets::Vertex nv = m_verts[vi];
            nv.pos += n * d;
            pushOut.emplace(vi, static_cast<std::uint32_t>(m_verts.size()));
            m_verts.push_back(nv);
        }
    }

    // 2) 找边界边：只被**一个**选中面用到的 weld 边就是边界。
    //    内部的边（两个选中面共享）不需要补侧面 —— 那里是连续的顶面。
    //    值里同时留下"该边在某次使用时的原始顶点顺序"，侧面要靠它定绕序。
    std::unordered_map<std::uint64_t, int> edgeCount;
    std::unordered_map<std::uint64_t, std::pair<std::uint32_t, std::uint32_t>>
        edgeUse;
    auto edgeKey = [](std::uint32_t a, std::uint32_t b) {
        if (a > b) std::swap(a, b);
        return (static_cast<std::uint64_t>(a) << 32) | b;
    };
    for (std::uint32_t f : faces) {
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t wa = m_topo.tri[f][k];
            const std::uint32_t wb = m_topo.tri[f][(k + 1) % 3];
            const std::uint64_t key = edgeKey(wa, wb);
            ++edgeCount[key];
            edgeUse[key] = {m_topo.triVert[f][k], m_topo.triVert[f][(k + 1) % 3]};
        }
    }

    // 3) 重建索引：选中面的三个顶点指向新的，其余原样；再补侧面。
    std::unordered_set<std::uint32_t> selSet(faces.begin(), faces.end());
    std::vector<std::uint32_t> idx;
    idx.reserve(m_idx.size() + edgeCount.size() * 6);
    for (std::size_t f = 0; f < m_topo.triVert.size(); ++f) {
        const auto& tv = m_topo.triVert[f];
        if (selSet.count(static_cast<std::uint32_t>(f))) {
            idx.push_back(pushOut[tv[0]]);
            idx.push_back(pushOut[tv[1]]);
            idx.push_back(pushOut[tv[2]]);
        } else {
            idx.push_back(tv[0]);
            idx.push_back(tv[1]);
            idx.push_back(tv[2]);
        }
    }
    int sides = 0;
    for (const auto& kv : edgeCount) {
        if (kv.second != 1) continue;
        const auto use = edgeUse[kv.first];
        const std::uint32_t a = use.first, b = use.second;
        auto ia = pushOut.find(a);
        auto ib = pushOut.find(b);
        if (ia == pushOut.end() || ib == pushOut.end()) continue;
        // 绕序与原来的面一致 → 侧面朝外（否则整圈侧面都是背面，看起来
        // 像"挤出之后中间是空的"）
        idx.push_back(a);
        idx.push_back(b);
        idx.push_back(ib->second);
        idx.push_back(a);
        idx.push_back(ib->second);
        idx.push_back(ia->second);
        ++sides;
    }

    m_idx.swap(idx);

    // 挤出之后选区跟着顶面走（Blender：挤出完新顶面是**选中**的，所以
    // 紧接着 G 拖 / Delete / 再挤一次都作用在刚推出去的那层上）。
    //
    // ⚠ 这里原来只写了 `m_sel.clear()` —— 注释说要"跟着走"、代码却只清空。
    //   后果：挤出一次之后 Delete 立刻变成 `skip=no-face-selected`，用户看
    //   起来就是"挤出之后什么都不能再做了"。
    //
    // 旧的 weld id 也不能直接留：新顶面用的是 pushOut 里那份**副本**顶点，
    // 它们要等 upload() 重建拓扑才会被 weld 成新的组。所以先把副本的渲染
    // 顶点下标记下来，upload 之后按 weldOf 反查组号。
    std::vector<std::uint32_t> pushed;
    pushed.reserve(pushOut.size());
    for (const auto& kv : pushOut) pushed.push_back(kv.second);

    m_sel.clear();
    upload("extrude");

    if (m_topo.valid) {
        for (std::uint32_t vi : pushed) {
            if (vi < m_topo.weldOf.size()) m_sel.insert(m_topo.weldOf[vi]);
        }
    }
    VK_LOG_INFO("MODEL-EXTRUDE faces=%d sides=%d dist=%.3f verts=%d tris=%d "
                "selV=%d selF=%d",
                static_cast<int>(faces.size()), sides,
                static_cast<double>(d), m_vertices, m_triangles,
                static_cast<int>(m_sel.size()),
                static_cast<int>(selPolys().size()));
}

void ModelingEditor::deleteFaces() {
    m_beforeVerts = static_cast<int>(m_verts.size());
    const std::vector<std::uint32_t> faces = selFaces();
    if (faces.empty()) {
        VK_LOG_INFO("MODEL-DELETE skip=no-face-selected");
        return;
    }
    std::unordered_set<std::uint32_t> del(faces.begin(), faces.end());
    std::vector<std::uint32_t> idx;
    idx.reserve(m_idx.size());
    for (std::size_t f = 0; f < m_topo.triVert.size(); ++f) {
        if (del.count(static_cast<std::uint32_t>(f))) continue;
        idx.push_back(m_topo.triVert[f][0]);
        idx.push_back(m_topo.triVert[f][1]);
        idx.push_back(m_topo.triVert[f][2]);
    }
    const int removed = static_cast<int>(faces.size());
    m_idx.swap(idx);
    m_sel.clear();
    upload("delete");
    VK_LOG_INFO("MODEL-DELETE faces=%d tris=%d", removed, m_triangles);
}

void ModelingEditor::mergeSelection() {
    m_beforeVerts = static_cast<int>(m_verts.size());
    if (m_sel.size() < 2) {
        VK_LOG_INFO("MODEL-MERGE skip=need-2-verts selV=%d",
                    static_cast<int>(m_sel.size()));
        return;
    }
    const glm::vec3 c = selectionCenter();
    for (std::uint32_t id : m_sel) {
        if (id < m_topo.weldPos.size()) m_topo.weldPos[id] = c;
    }
    // 合并之后三角形会退化（三个角里有两个在同一个位置）—— 直接丢掉，
    // 否则网格里会留一堆零面积面，后面求法线、算包围盒全是垃圾。
    std::vector<std::uint32_t> idx;
    idx.reserve(m_idx.size());
    int dropped = 0;
    for (const auto& tv : m_topo.triVert) {
        const PosKey a = quantizePos(m_topo.weldPos[m_topo.weldOf[tv[0]]]);
        const PosKey b = quantizePos(m_topo.weldPos[m_topo.weldOf[tv[1]]]);
        const PosKey c2 = quantizePos(m_topo.weldPos[m_topo.weldOf[tv[2]]]);
        const bool degen = (a.x == b.x && a.y == b.y && a.z == b.z) ||
                           (b.x == c2.x && b.y == c2.y && b.z == c2.z) ||
                           (a.x == c2.x && a.y == c2.y && a.z == c2.z);
        if (degen) {
            ++dropped;
            continue;
        }
        idx.push_back(tv[0]);
        idx.push_back(tv[1]);
        idx.push_back(tv[2]);
    }
    m_idx.swap(idx);
    const std::size_t merged = m_sel.size();
    // 合并后的那一个点保持选中（否则"合并"完选区没了，想接着移动它还得
    // 重新点一次）。位置取自 selectionCenter，与写进 weldPos 的一致。
    const glm::vec3 center = c;
    m_sel.clear();
    upload("merge");
    if (m_topo.valid) {
        int best = -1;
        float bd = 1e30f;
        for (std::size_t i = 0; i < m_topo.weldPos.size(); ++i) {
            const glm::vec3 d2v = m_topo.weldPos[i] - center;
            const float d2 = glm::dot(d2v, d2v);
            if (d2 < bd) {
                bd = d2;
                best = static_cast<int>(i);
            }
        }
        if (best >= 0 && bd < 1e-4f) m_sel.insert(static_cast<std::uint32_t>(best));
    }
    VK_LOG_INFO("MODEL-MERGE verts=%d droppedTris=%d tris=%d",
                static_cast<int>(merged), dropped, m_triangles);
}

void ModelingEditor::subdivideSelected(const char* opName) {
    m_beforeVerts = static_cast<int>(m_verts.size());
    const std::vector<std::uint32_t> faces = selFaces();
    if (faces.empty()) {
        VK_LOG_INFO("MODEL-SUBDIV skip=no-face-selected（全网格细分见 Shape 段）");
        m_ctx.notify(TR("Select faces to subdivide, or use the whole-mesh "
                        "button below."));
        return;
    }
    std::unordered_set<std::uint32_t> selSet(faces.begin(), faces.end());
    std::vector<std::uint32_t> idx;
    idx.reserve(m_idx.size() * 2);
    // 新建的中点（渲染顶点下标）：细分完要连同原来的三个角一起保持选中，
    // 否则 1→4 之后四个子面各有一个"没选中的中点"，selFaces() 立刻变 0 ——
    // 表现就是"细分一次之后选区没了，什么都不能接着做"。
    std::vector<std::uint32_t> midpoints;

    auto midpoint = [](const assets::Vertex& a, const assets::Vertex& b) {
        assets::Vertex m = a;
        m.pos = (a.pos + b.pos) * 0.5f;
        m.normal = glm::normalize(a.normal + b.normal + glm::vec3(1e-6f));
        m.uv = (a.uv + b.uv) * 0.5f;
        m.color = (a.color + b.color) * 0.5f;
        return m;
    };
    for (std::size_t f = 0; f < m_topo.triVert.size(); ++f) {
        const auto tv = m_topo.triVert[f];
        if (!selSet.count(static_cast<std::uint32_t>(f))) {
            idx.push_back(tv[0]);
            idx.push_back(tv[1]);
            idx.push_back(tv[2]);
            continue;
        }
        // 1 → 4：三个中点 + 四个子三角。中点直接新建顶点（不查邻面共享），
        // 代价是相邻面各自建一份 —— 但那些面本来就不在选区里，硬边正是
        // 期望的结果。
        const assets::Vertex& va = m_verts[tv[0]];
        const assets::Vertex& vb = m_verts[tv[1]];
        const assets::Vertex& vc = m_verts[tv[2]];
        const std::uint32_t iab = static_cast<std::uint32_t>(m_verts.size());
        m_verts.push_back(midpoint(va, vb));
        const std::uint32_t ibc = static_cast<std::uint32_t>(m_verts.size());
        m_verts.push_back(midpoint(vb, vc));
        const std::uint32_t ica = static_cast<std::uint32_t>(m_verts.size());
        m_verts.push_back(midpoint(vc, va));
        midpoints.push_back(iab);
        midpoints.push_back(ibc);
        midpoints.push_back(ica);
        const std::uint32_t quad[4][3] = {{tv[0], iab, ica},
                                          {iab, tv[1], ibc},
                                          {ica, ibc, tv[2]},
                                          {iab, ibc, ica}};
        for (const auto& t : quad) {
            idx.push_back(t[0]);
            idx.push_back(t[1]);
            idx.push_back(t[2]);
        }
    }
    m_idx.swap(idx);
    upload(opName);
    // 细分之后 4 个子面全部保持选中（Blender 也是这样）：角点本来就还在
    // m_sel 里，补上新中点的 weld 组即可。
    if (m_topo.valid) {
        for (std::uint32_t vi : midpoints) {
            if (vi < m_topo.weldOf.size()) m_sel.insert(m_topo.weldOf[vi]);
        }
    }
    VK_LOG_INFO("MODEL-SUBDIV op=%s faces=%d tris=%d verts=%d selF=%d", opName,
                static_cast<int>(faces.size()), m_triangles, m_vertices,
                static_cast<int>(selPolys().size()));
}

void ModelingEditor::flipSelected() {
    m_beforeVerts = static_cast<int>(m_verts.size());
    const std::vector<std::uint32_t> faces = selFaces();
    if (faces.empty()) {
        VK_LOG_INFO("MODEL-FLIP skip=no-face-selected");
        return;
    }
    for (std::uint32_t f : faces) {
        std::swap(m_idx[f * 3 + 1], m_idx[f * 3 + 2]);
        // 就地翻转法线，省一次全网格重算（其余面的法线是对的）
        for (std::uint32_t vi : {m_topo.triVert[f][0], m_topo.triVert[f][1],
                                 m_topo.triVert[f][2]})
            m_verts[vi].normal = -m_verts[vi].normal;
    }
    upload("flip-selected");
    VK_LOG_INFO("MODEL-FLIP faces=%d", static_cast<int>(faces.size()));
}

void ModelingEditor::smoothAll(float amount) {
    m_beforeVerts = static_cast<int>(m_verts.size());
    // 拉普拉斯平滑，按 weld 邻接做（不 weld 的话立方体的角会被三份顶点
    // 各自拉向不同方向，越平滑越碎）
    if (!m_topo.valid || m_topo.weldPos.empty()) return;
    std::vector<glm::vec3> sum(m_topo.weldPos.size(), glm::vec3(0.0f));
    std::vector<int> cnt(m_topo.weldPos.size(), 0);
    for (const auto& tr : m_topo.tri) {
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t a = tr[k], b = tr[(k + 1) % 3];
            sum[a] += m_topo.weldPos[b];
            sum[b] += m_topo.weldPos[a];
            cnt[a] += 1;
            cnt[b] += 1;
        }
    }
    const float k = std::clamp(amount, 0.0f, 1.0f);
    std::vector<glm::vec3> next = m_topo.weldPos;
    for (std::size_t i = 0; i < next.size(); ++i) {
        if (cnt[i] == 0) continue;
        next[i] = glm::mix(m_topo.weldPos[i], sum[i] / static_cast<float>(cnt[i]), k);
    }
    m_topo.weldPos.swap(next);
    upload("smooth");
    VK_LOG_INFO("MODEL-SMOOTH amount=%.2f verts=%d", static_cast<double>(k),
                m_vertices);
}

void ModelingEditor::mirrorX() {
    m_beforeVerts = static_cast<int>(m_verts.size());
    for (auto& p : m_topo.weldPos) p.x = -p.x;
    // 镜像会翻转绕序 —— 不翻的话所有面都变成背面，模型看起来"里面朝外"。
    for (std::size_t f = 0; f + 2 < m_idx.size(); f += 3)
        std::swap(m_idx[f + 1], m_idx[f + 2]);
    m_sel.clear();
    upload("mirror");
    VK_LOG_INFO("MODEL-MIRROR axis=x verts=%d", m_vertices);
}

// ============================================================
// 编辑网格（EditMesh）：三个新拓扑操作的公共地基
// ============================================================
//
// 内插面 / 倒角 / 环切都要**增删面**，而且会动到没被选中的邻居面（倒角要
// 切掉所有相邻面的角）。直接在三张索引表上做增量拼接的话，每个操作都得
// 写一套"旧三角形怎么删、新三角形怎么接"的胶水代码，漏掉一个角就是一地
// 碎面 —— 而且三种操作互相之间还会打架。
//
// 所以统一摊平成 EditMesh（位置数组 + 面表 + 每角对应的原渲染顶点），
// 在面表上做手术，最后一次性写回。
//
// 为什么"每角保留原渲染顶点"这一条不能省：
//   立方体一个角有 3 份渲染顶点（三个面法线不同）。如果写回时让同一位置的
//   角共用一个渲染顶点，`recomputeNormals` 会把三条法线平均掉 ——
//   一次倒角就能把整个立方体变成"圆角盒子"。

void ModelingEditor::buildEditMesh(EditMesh& em) const {
    em.pos = m_topo.weldPos;
    em.faces.clear();
    em.cornerVert.clear();
    em.faces.reserve(m_topo.poly.size());
    em.cornerVert.reserve(m_topo.poly.size());

    for (const auto& p : m_topo.poly) {
        em.faces.push_back(p.v);
        std::vector<std::uint32_t> cv(p.v.size(), EditMesh::kNewVert);
        // 优先用**本面自己**的三角形去找渲染顶点（不能随手拿 weldVerts[0]：
        // 那是别的面的顶点，法线/UV 都是别的面的）。
        for (std::uint32_t tri : p.tris) {
            if (tri >= m_topo.tri.size()) continue;
            const auto& tw = m_topo.tri[tri];
            const auto& tv = m_topo.triVert[tri];
            for (int k = 0; k < 3; ++k) {
                for (std::size_t c = 0; c < p.v.size(); ++c)
                    if (p.v[c] == tw[k] && cv[c] == EditMesh::kNewVert)
                        cv[c] = tv[k];
            }
        }
        // 兜底：万一某个角在三角形里找不到（理论上不会），退回 weld 组首项
        for (std::size_t c = 0; c < cv.size(); ++c) {
            if (cv[c] != EditMesh::kNewVert) continue;
            const std::uint32_t w = p.v[c];
            if (w < m_topo.weldVerts.size() && !m_topo.weldVerts[w].empty())
                cv[c] = m_topo.weldVerts[w][0];
        }
        em.cornerVert.push_back(std::move(cv));
    }
}

void ModelingEditor::commitEditMesh(EditMesh& em, const char* op) {
    if (em.faces.empty()) return;

    // 1) 新角 → 每个角建**一份独立的**渲染顶点（属性抄本面第一个老角）。
    //    不能按 weld 去重：倒角的切点被两个相邻面共用，去重就意味着两个面
    //    共用一个渲染顶点，法线被平均 → 硬边没了。
    for (std::size_t f = 0; f < em.faces.size(); ++f) {
        const auto& face = em.faces[f];
        auto& cv = em.cornerVert[f];
        if (cv.size() != face.size()) cv.assign(face.size(), EditMesh::kNewVert);
        std::uint32_t tmpl = EditMesh::kNewVert;
        for (std::uint32_t v : cv)
            if (v != EditMesh::kNewVert && v < m_verts.size() && tmpl == EditMesh::kNewVert)
                tmpl = v;
        if (tmpl == EditMesh::kNewVert) tmpl = 0;
        for (std::size_t c = 0; c < face.size(); ++c) {
            if (cv[c] != EditMesh::kNewVert) continue;
            assets::Vertex nv = m_verts[tmpl];
            nv.pos = em.pos[face[c]];
            cv[c] = static_cast<std::uint32_t>(m_verts.size());
            m_verts.push_back(nv);
        }
    }

    // 2) 老顶点：位置写回 weld 层（upload 会把组位置广播给所有渲染顶点）
    const std::size_t nOld = std::min(m_topo.weldPos.size(), em.pos.size());
    for (std::size_t w = 0; w < nOld; ++w) m_topo.weldPos[w] = em.pos[w];

    // 3) 重建索引：每个面扇形三角化（面都是凸/近凸的，扇形足够）
    //
    //    同时把"每个面几个三角形"记进 m_polyHint：rebuildTopo 靠它把 n 边形
    //    原样还原。没有这一条，倒角出来的八边形会被重新猜成 3~4 个四边形。
    m_idx.clear();
    m_idx.reserve(em.faces.size() * 6);
    m_polyHint.clear();
    m_polyHint.reserve(em.faces.size());
    for (std::size_t f = 0; f < em.faces.size(); ++f) {
        const auto& face = em.faces[f];
        const auto& cv = em.cornerVert[f];
        if (face.size() < 3) continue;
        for (std::size_t k = 1; k + 1 < face.size(); ++k) {
            m_idx.push_back(cv[0]);
            m_idx.push_back(cv[k]);
            m_idx.push_back(cv[k + 1]);
        }
        m_polyHint.push_back(static_cast<std::uint32_t>(face.size() - 2));
    }
    upload(op);
}

void ModelingEditor::selectByPosition(const std::vector<glm::vec3>& pts) {
    m_sel.clear();
    if (!m_topo.valid || pts.empty()) return;
    std::unordered_map<PosKey, std::uint32_t, PosKeyHash, PosKeyEq> map;
    map.reserve(m_topo.weldPos.size());
    for (std::uint32_t i = 0; i < m_topo.weldPos.size(); ++i)
        map.emplace(quantizePos(m_topo.weldPos[i]), i);
    for (const auto& p : pts) {
        auto it = map.find(quantizePos(p));
        if (it != map.end()) m_sel.insert(it->second);
    }
}

// ------------------------------------------------------------ 内插面（Inset）
//
// 每个选中的面往里收一圈，中间那块面保留、外圈补一圈四边形。
// 与"把面放大缩小"的区别：这里是**每条边向内平移固定距离**（Blender 的
// Offset），所以长条面收出来的内面还是长条，而不是等比缩小的形状。
void ModelingEditor::insetFaces() {
    m_beforeVerts = static_cast<int>(m_verts.size());
    const std::vector<std::uint32_t> polys = selPolys();
    if (polys.empty()) {
        VK_LOG_INFO("MODEL-INSET skip=no-face-selected");
        m_ctx.notify(TR("Inset needs at least one selected face."));
        return;
    }
    EditMesh em;
    buildEditMesh(em);
    const float dist = std::max(m_inset, 1.0e-4f);

    std::vector<glm::vec3> innerPts;   // 新内角的位置（写回后用来恢复选区）
    int made = 0;

    for (std::uint32_t pi : polys) {
        if (pi >= em.faces.size()) continue;
        std::vector<std::uint32_t> face = em.faces[pi];
        const std::size_t n = face.size();
        if (n < 3 || n > 256) continue;
        const glm::vec3 nrm = m_topo.poly[pi].normal;

        // 面平面内的正交基
        glm::vec3 u(1.0f, 0.0f, 0.0f);
        if (std::fabs(glm::dot(u, nrm)) > 0.9f) u = glm::vec3(0.0f, 1.0f, 0.0f);
        u = glm::normalize(u - nrm * glm::dot(u, nrm));
        const glm::vec3 v = glm::cross(nrm, u);
        const float h = glm::dot(em.pos[face[0]], nrm);

        std::vector<glm::vec2> p2(n);
        glm::vec2 c2(0.0f);
        for (std::size_t i = 0; i < n; ++i) {
            const glm::vec3& q = em.pos[face[i]];
            p2[i] = glm::vec2(glm::dot(q, u), glm::dot(q, v));
            c2 += p2[i];
        }
        c2 /= static_cast<float>(n);

        // 每条边的内法线（面内）。逆时针绕序时把边方向转 +90° 指向内侧。
        std::vector<glm::vec2> nrm2(n);
        for (std::size_t i = 0; i < n; ++i) {
            glm::vec2 e = p2[(i + 1) % n] - p2[i];
            const float len = glm::length(e);
            if (len < 1.0e-7f) {
                nrm2[i] = glm::vec2(0.0f);
                continue;
            }
            e /= len;
            nrm2[i] = glm::vec2(-e.y, e.x);
        }
        // 绕序可能反（导入的网格不保证 CCW）：用"内法线是否指向重心"校验
        float inward = 0.0f;
        for (std::size_t i = 0; i < n; ++i)
            inward += glm::dot(nrm2[i], c2 - p2[i]);
        if (inward < 0.0f)
            for (auto& d : nrm2) d = -d;

        // 顶点 i = 边(i-1) 与 边(i) 各自向内平移 dist 后的交点
        std::vector<glm::vec2> inner2(n);
        bool bad = false;
        for (std::size_t i = 0; i < n; ++i) {
            const glm::vec2 n0 = nrm2[(i + n - 1) % n];
            const glm::vec2 n1 = nrm2[i];
            const float det = n0.x * n1.y - n0.y * n1.x;
            if (std::fabs(det) < 1.0e-6f) {
                // 两条边几乎共线（180° 的角）→ 直接沿法线平移
                inner2[i] = p2[i] + n1 * dist;
                continue;
            }
            const float b0 = dist + glm::dot(p2[(i + n - 1) % n], n0);
            const float b1 = dist + glm::dot(p2[i], n1);
            inner2[i] = glm::vec2((b0 * n1.y - b1 * n0.y) / det,
                                  (b1 * n0.x - b0 * n1.x) / det);
            // 收过头 → 内面会翻出去。这种情况退回"朝重心插值"。
            if (glm::dot(inner2[i] - p2[i], n1) < -1.0e-4f) bad = true;
        }
        if (bad) {
            float apothem = 1.0e30f;
            for (std::size_t i = 0; i < n; ++i) {
                const float d = glm::dot(c2 - p2[i], nrm2[i]);
                if (d > 1.0e-5f) apothem = std::min(apothem, d);
            }
            if (apothem > 1.0e29f) apothem = 1.0f;
            const float t = std::clamp(dist / apothem, 0.0f, 0.45f);
            for (std::size_t i = 0; i < n; ++i)
                inner2[i] = glm::mix(p2[i], c2, t);
        }

        // 建新顶点 + 换掉原面 + 补一圈四边形
        std::vector<std::uint32_t> innerW(n);
        for (std::size_t i = 0; i < n; ++i) {
            const glm::vec3 p3 =
                u * inner2[i].x + v * inner2[i].y + nrm * h;
            innerW[i] = static_cast<std::uint32_t>(em.pos.size());
            em.pos.push_back(p3);
            innerPts.push_back(p3);
        }
        std::vector<std::vector<std::uint32_t>> ring(n);
        std::vector<std::vector<std::uint32_t>> ringCv(n);
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t j = (i + 1) % n;
            ring[i] = {face[i], face[j], innerW[j], innerW[i]};
            ringCv[i] = {em.cornerVert[pi][i], em.cornerVert[pi][j],
                         EditMesh::kNewVert, EditMesh::kNewVert};
        }
        em.faces[pi] = innerW;
        em.cornerVert[pi].assign(n, EditMesh::kNewVert);
        for (std::size_t i = 0; i < n; ++i) {
            em.faces.push_back(std::move(ring[i]));
            em.cornerVert.push_back(std::move(ringCv[i]));
        }
        made += static_cast<int>(n);
    }

    if (innerPts.empty()) return;
    commitEditMesh(em, "inset");
    selectByPosition(innerPts);
    VK_LOG_INFO("MODEL-INSET faces=%d newVerts=%d dist=%.3f verts=%d tris=%d "
                "polys=%d selF=%d",
                static_cast<int>(polys.size()), made,
                static_cast<double>(dist), m_vertices, m_triangles,
                static_cast<int>(m_topo.poly.size()),
                static_cast<int>(selPolys().size()));
}

// ------------------------------------------------------------ 倒角（Bevel）
//
// 顶点倒角：每个选中的角，沿它的每条相邻边量出 bevel 距离切一刀，然后
// 在角上补一个"切面"。全选一个立方体的 8 个角 → 倒角立方体（Blender
// Ctrl+B 选全部顶点时的效果）。
//
// 为什么做顶点倒角而不是边倒角：边倒角要沿一条边把两侧的顶点各自劈成
// 两份，还会在顶点处产生三岔的补角面 —— 那是半个 half-edge 内核的工作量。
// 顶点倒角在**每个面上**都只是一次"corner truncation"，天然不会漏面。
void ModelingEditor::bevelVertices() {
    m_beforeVerts = static_cast<int>(m_verts.size());
    if (m_sel.empty()) {
        VK_LOG_INFO("MODEL-BEVEL skip=no-vertex-selected");
        m_ctx.notify(TR("Bevel works on selected vertices: switch to 1 "
                        "and select the corners you want to cut."));
        return;
    }
    EditMesh em;
    buildEditMesh(em);
    const float dist = std::max(m_bevel, 1.0e-4f);
    const float eps = 1.0e-4f;

    // 建邻接（从面表推，不用三角形 —— 面表才是拓扑真相）
    std::vector<std::unordered_set<std::uint32_t>> adj(em.pos.size());
    for (const auto& face : em.faces)
        for (std::size_t i = 0; i < face.size(); ++i) {
            const std::uint32_t a = face[i];
            const std::uint32_t b = face[(i + 1) % face.size()];
            if (a >= adj.size() || b >= adj.size()) continue;
            adj[a].insert(b);
            adj[b].insert(a);
        }

    auto ekey = [](std::uint32_t a, std::uint32_t b) {
        if (a > b) std::swap(a, b);
        return (static_cast<std::uint64_t>(a) << 32) | b;
    };

    // 每条被切的边一个切点（两个相邻面共用同一个 weld 位置）
    std::unordered_map<std::uint64_t, std::uint32_t> cut;
    std::vector<glm::vec3> cutPts;
    std::vector<std::uint32_t> beveled;
    for (std::uint32_t v : m_sel) {
        if (v >= em.pos.size()) continue;
        beveled.push_back(v);
        for (std::uint32_t u : adj[v]) {
            const std::uint64_t k = ekey(v, u);
            if (cut.count(k)) continue;
            const glm::vec3 d3 = em.pos[u] - em.pos[v];
            const float len = glm::length(d3);
            if (len < eps) continue;
            // 0.45 而不是 0.5：两端都倒角时两个切点才不会撞在一起
            const float dd = std::min(dist, 0.45f * len);
            const glm::vec3 p = em.pos[v] + d3 * (dd / len);
            cut[k] = static_cast<std::uint32_t>(em.pos.size());
            em.pos.push_back(p);
            cutPts.push_back(p);
        }
    }
    if (cut.empty()) {
        VK_LOG_INFO("MODEL-BEVEL skip=degenerate");
        return;
    }

    // 2) 每个面：把被倒角的角换成"两个切点"
    for (std::size_t f = 0; f < em.faces.size(); ++f) {
        const std::vector<std::uint32_t> face = em.faces[f];
        const std::vector<std::uint32_t> cv = em.cornerVert[f];
        const std::size_t n = face.size();
        std::vector<std::uint32_t> nf, ncv;
        nf.reserve(n + 2);
        ncv.reserve(n + 2);
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t w = face[i];
            if (!m_sel.count(w)) {
                nf.push_back(w);
                ncv.push_back(cv.size() == n ? cv[i] : EditMesh::kNewVert);
                continue;
            }
            const std::uint32_t prev = face[(i + n - 1) % n];
            const std::uint32_t next = face[(i + 1) % n];
            auto ip = cut.find(ekey(w, prev));
            auto in = cut.find(ekey(w, next));
            if (ip == cut.end() || in == cut.end()) {   // 退化边，保持原样
                nf.push_back(w);
                ncv.push_back(cv.size() == n ? cv[i] : EditMesh::kNewVert);
                continue;
            }
            nf.push_back(ip->second);
            ncv.push_back(EditMesh::kNewVert);
            nf.push_back(in->second);
            ncv.push_back(EditMesh::kNewVert);
        }
        em.faces[f] = std::move(nf);
        em.cornerVert[f] = std::move(ncv);
    }

    // 3) 每个被倒角的角补一个"切面"
    auto faceNormal = [&](const std::vector<std::uint32_t>& v) {
        glm::vec3 n(0.0f);
        for (std::size_t i = 0; i < v.size(); ++i) {
            const glm::vec3& a = em.pos[v[i]];
            const glm::vec3& b = em.pos[v[(i + 1) % v.size()]];
            n.x += (a.y - b.y) * (a.z + b.z);
            n.y += (a.z - b.z) * (a.x + b.x);
            n.z += (a.x - b.x) * (a.y + b.y);
        }
        return n;
    };
    int caps = 0;
    for (std::uint32_t v : beveled) {
        std::vector<std::uint32_t> ring;
        for (std::uint32_t u : adj[v]) {
            auto it = cut.find(ekey(v, u));
            if (it != cut.end()) ring.push_back(it->second);
        }
        if (ring.size() < 3) continue;
        // 顶点法线 = 相邻面法线之和（近似）
        glm::vec3 vn(0.0f);
        for (const auto& face : em.faces)
            for (std::uint32_t w : face)
                if (w == v) {
                    vn += glm::normalize(faceNormal(face) + glm::vec3(1e-9f));
                    break;
                }
        if (glm::length(vn) < 1e-6f) vn = glm::vec3(0.0f, 0.0f, 1.0f);
        vn = glm::normalize(vn);
        // 切面绕顶点排序（按绕 vn 的角度）→ 凸多边形，法线朝外
        glm::vec3 t1(1.0f, 0.0f, 0.0f);
        if (std::fabs(glm::dot(t1, vn)) > 0.9f) t1 = glm::vec3(0.0f, 1.0f, 0.0f);
        t1 = glm::normalize(t1 - vn * glm::dot(t1, vn));
        const glm::vec3 t2 = glm::cross(vn, t1);
        std::sort(ring.begin(), ring.end(),
                  [&](std::uint32_t a, std::uint32_t b) {
                      const glm::vec3 da = em.pos[a] - em.pos[v];
                      const glm::vec3 db = em.pos[b] - em.pos[v];
                      return std::atan2(glm::dot(da, t2), glm::dot(da, t1)) <
                             std::atan2(glm::dot(db, t2), glm::dot(db, t1));
                  });
        em.faces.push_back(ring);
        em.cornerVert.push_back(std::vector<std::uint32_t>(
            ring.size(), EditMesh::kNewVert));
        ++caps;
    }

    commitEditMesh(em, "bevel");
    selectByPosition(cutPts);
    VK_LOG_INFO("MODEL-BEVEL corners=%d cuts=%d caps=%d dist=%.3f verts=%d "
                "tris=%d polys=%d selV=%d",
                static_cast<int>(beveled.size()), static_cast<int>(cut.size()),
                caps, static_cast<double>(dist), m_vertices, m_triangles,
                static_cast<int>(m_topo.poly.size()),
                static_cast<int>(m_sel.size()));
}

// ------------------------------------------------------------ 环切（Loop cut）
//
// 沿一条四边形带插入一圈边。做法：
//   1. 定 seed 边：边模式下选了正好两个点就用它；否则取选中面的最短边
//      （最短边一定是"横向"的那条 —— 长条面环切不该顺着长边走）；
//   2. 在四边形带里一路穿过去（进入边 → 对面边 → 邻面），直到回到起点
//      （闭合环）或走出边界（开带）；
//   3. 每条被穿过的边插入 m_loopCount 个点，每个四边形拆成一排四边形。
void ModelingEditor::loopCut() {
    m_beforeVerts = static_cast<int>(m_verts.size());
    EditMesh em;
    buildEditMesh(em);
    if (em.faces.empty()) return;

    auto ekey = [](std::uint32_t a, std::uint32_t b) {
        if (a > b) std::swap(a, b);
        return (static_cast<std::uint64_t>(a) << 32) | b;
    };

    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> edgeFaces;
    for (std::uint32_t f = 0; f < em.faces.size(); ++f) {
        const auto& face = em.faces[f];
        for (std::size_t i = 0; i < face.size(); ++i)
            edgeFaces[ekey(face[i], face[(i + 1) % face.size()])].push_back(f);
    }

    // ---- seed 边 ----
    std::uint64_t seed = 0;
    bool haveSeed = false;
    if (m_selMode == SelectMode::Edge && m_sel.size() == 2) {
        std::uint32_t a = 0, b = 0;
        int n = 0;
        for (std::uint32_t v : m_sel) (n++ == 0 ? a : b) = v;
        if (a < em.pos.size() && b < em.pos.size()) {
            seed = ekey(a, b);
            haveSeed = edgeFaces.count(seed) > 0;
        }
    }
    if (!haveSeed) {
        const auto polys = selPolys();
        if (!polys.empty()) {
            const auto& face = em.faces[polys[0]];
            float best = 1.0e30f;
            for (std::size_t i = 0; i < face.size(); ++i) {
                const float L = glm::length(em.pos[face[(i + 1) % face.size()]] -
                                            em.pos[face[i]]);
                if (L < best && L > 1.0e-6f) {
                    best = L;
                    seed = ekey(face[i], face[(i + 1) % face.size()]);
                    haveSeed = true;
                }
            }
        }
    }
    if (!haveSeed) {
        VK_LOG_INFO("MODEL-LOOPCUT skip=no-seed-edge");
        m_ctx.notify(TR("Loop cut needs a selected face (or two vertices in "
                        "edge mode) to know which way the loop runs."));
        return;
    }

    // ---- 沿四边形带游走 ----
    std::vector<std::uint32_t> ring;
    std::vector<std::uint64_t> splitEdges;
    std::unordered_set<std::uint64_t> seenEdge;
    const std::uint32_t startFace = edgeFaces[seed].front();
    std::uint32_t f = startFace;
    std::uint64_t cur = seed;
    for (std::size_t guard = 0; guard <= em.faces.size(); ++guard) {
        const auto& face = em.faces[f];
        if (face.size() != 4) break;                  // 非四边形，带到此为止
        if (seenEdge.count(cur)) break;
        seenEdge.insert(cur);
        splitEdges.push_back(cur);

        std::uint32_t i0 = 4;
        for (std::uint32_t i = 0; i < 4; ++i)
            if (ekey(face[i], face[(i + 1) % 4]) == cur) {
                i0 = i;
                break;
            }
        if (i0 == 4) break;
        ring.push_back(f);
        const std::uint64_t opp =
            ekey(face[(i0 + 2) % 4], face[(i0 + 3) % 4]);
        auto it = edgeFaces.find(opp);
        if (it == edgeFaces.end()) break;
        std::uint32_t nf = 0xFFFFFFFFu;
        for (std::uint32_t cand : it->second)
            if (cand != f) {
                nf = cand;
                break;
            }
        if (nf == 0xFFFFFFFFu) break;                 // 边界
        if (nf == startFace) {                        // 闭环：最后一刀落在 seed 上
            if (!seenEdge.count(opp)) {
                seenEdge.insert(opp);
                splitEdges.push_back(opp);
            }
            break;
        }
        f = nf;
        cur = opp;
    }
    if (ring.empty()) {
        VK_LOG_INFO("MODEL-LOOPCUT skip=not-quad");
        m_ctx.notify(TR("Loop cut only walks quads - select a quad first."));
        return;
    }

    // ---- 每条边插入 m_loopCount 个点 ----
    const int cuts = std::clamp(m_loopCount, 1, 10);
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> onEdge;
    std::vector<glm::vec3> newPts;
    for (std::uint64_t k : splitEdges) {
        const std::uint32_t a = static_cast<std::uint32_t>(k >> 32);
        const std::uint32_t b = static_cast<std::uint32_t>(k & 0xFFFFFFFFu);
        std::vector<std::uint32_t> ids;
        ids.reserve(cuts);
        for (int c = 1; c <= cuts; ++c) {
            const float t = static_cast<float>(c) / static_cast<float>(cuts + 1);
            const glm::vec3 p = glm::mix(em.pos[a], em.pos[b], t);
            ids.push_back(static_cast<std::uint32_t>(em.pos.size()));
            em.pos.push_back(p);
            newPts.push_back(p);
        }
        onEdge[k] = std::move(ids);
    }

    // ---- 拆分每个环内的四边形 ----
    int madeQuads = 0;
    for (std::uint32_t fi : ring) {
        const std::vector<std::uint32_t> face = em.faces[fi];
        const std::vector<std::uint32_t> cv = em.cornerVert[fi];
        int ei = -1, eo = -1;
        for (int i = 0; i < 4; ++i) {
            const std::uint64_t k = ekey(face[i], face[(i + 1) % 4]);
            if (!onEdge.count(k)) continue;
            if (ei < 0) ei = i;
            else eo = i;
        }
        if (ei < 0 || eo < 0) continue;
        if ((eo + 4 - ei) % 4 != 2) continue;   // 两条边必须是对边

        const std::uint32_t c0 = face[ei], c1 = face[(ei + 1) % 4];
        const std::uint32_t c2 = face[(ei + 2) % 4], c3 = face[(ei + 3) % 4];
        const auto& A = onEdge[ekey(c0, c1)];
        const auto& B = onEdge[ekey(c2, c3)];
        // 左轨：c0 → c1；右轨反着走：c3 → c2，这样四边形才不扭
        std::vector<std::uint32_t> L, R, Lcv, Rcv;
        L.push_back(c0);          Lcv.push_back(cv[ei]);
        R.push_back(c3);          Rcv.push_back(cv[(ei + 3) % 4]);
        for (int c = 0; c < cuts; ++c) {
            L.push_back(A[c]);    Lcv.push_back(EditMesh::kNewVert);
            R.push_back(B[cuts - 1 - c]); Rcv.push_back(EditMesh::kNewVert);
        }
        L.push_back(c1);          Lcv.push_back(cv[(ei + 1) % 4]);
        R.push_back(c2);          Rcv.push_back(cv[(ei + 2) % 4]);

        em.faces[fi] = {};        // 原面被拆掉，用下面这一排替代
        em.cornerVert[fi] = {};
        for (int c = 0; c <= cuts; ++c) {
            em.faces.push_back({L[c], L[c + 1], R[c + 1], R[c]});
            em.cornerVert.push_back({Lcv[c], Lcv[c + 1], Rcv[c + 1], Rcv[c]});
            ++madeQuads;
        }
    }

    if (newPts.empty()) return;
    commitEditMesh(em, "loopcut");
    selectByPosition(newPts);
    VK_LOG_INFO("MODEL-LOOPCUT faces=%d cuts=%d newQuads=%d verts=%d tris=%d "
                "polys=%d selV=%d",
                static_cast<int>(ring.size()), cuts, madeQuads, m_vertices,
                m_triangles, static_cast<int>(m_topo.poly.size()),
                static_cast<int>(m_sel.size()));
}

// ------------------------------------------------------------ 模态变换

int ModelingEditor::pickTriangle(const glm::vec3& ro, const glm::vec3& rd,
                                 float& outT) const {
    int best = -1;
    float bestT = 1e30f;
    for (std::size_t f = 0; f < m_topo.triVert.size(); ++f) {
        const auto& tv = m_topo.triVert[f];
        float t = 0.0f;
        if (rayTriangle(ro, rd, m_verts[tv[0]].pos, m_verts[tv[1]].pos,
                        m_verts[tv[2]].pos, t)) {
            if (t < bestT) {
                bestT = t;
                best = static_cast<int>(f);
            }
        }
    }
    outT = bestT;
    return best;
}

void ModelingEditor::beginModal(Modal m) {
    if (m_sel.empty()) {
        VK_LOG_INFO("MODEL-MODAL skip=empty-selection mode=%d",
                    static_cast<int>(m));
        return;
    }
    m_modal = m;
    m_modalBase = m_topo.weldPos;
    m_modalPivot = selectionCenter();
    const ImVec2 mp = ImGui::GetIO().MousePos;
    m_modalStart = glm::vec2(mp.x, mp.y);
    m_modalMoved = false;
    m_beforeVerts = static_cast<int>(m_verts.size());
    VK_LOG_INFO("MODEL-MODAL begin mode=%s verts=%d pivot=(%.2f,%.2f,%.2f)",
                m == Modal::Grab ? "grab" : (m == Modal::Scale ? "scale" : "rotate"),
                static_cast<int>(m_sel.size()),
                static_cast<double>(m_modalPivot.x),
                static_cast<double>(m_modalPivot.y),
                static_cast<double>(m_modalPivot.z));
}

void ModelingEditor::updateModal(const glm::vec2& mouse) {
    if (m_modal == Modal::None) return;
    const glm::vec2 d = mouse - m_modalStart;
    if (glm::length(d) > 1.0f) m_modalMoved = true;

    // **每次从备份重算**，不在上一帧结果上累加：累加会随鼠标抖动漂移，而且
    // ESC 之后回不到原样（中间态已经吃进顶点里了）。
    m_topo.weldPos = m_modalBase;

    // 屏幕像素 → 世界单位的换算：把枢轴投到屏幕上，再按"枢轴到相机的距离"
    // 和 FOV 算 1 像素对应多少米。这样拖同样的像素数，在近处和远处走的世界
    // 距离都"看起来一样"（Blender 的 free move 就是这个行为）。
    const scene::Camera& cam = m_ctx.editorScene().camera();
    const glm::vec3 camPos = cam.position();
    const float dist = std::max(glm::length(m_modalPivot - camPos), 0.5f);
    const float fovRad = glm::radians(cam.fov());
    const float viewH = std::max(ImGui::GetIO().DisplaySize.y, 1.0f);
    // 世界/像素 ≈ 2·tan(fov/2)·dist / 视口高度
    const float wpp = (2.0f * std::tan(fovRad * 0.5f) * dist) / viewH;

    if (m_modal == Modal::Grab) {
        const glm::vec3 right = cam.rightAxis();
        const glm::vec3 fwd = cam.forwardAxis();
        const glm::vec3 up = glm::normalize(glm::cross(right, fwd));
        const glm::vec3 delta = right * (d.x * wpp) + up * (-d.y * wpp);
        for (std::uint32_t id : m_sel) {
            if (id < m_topo.weldPos.size())
                m_topo.weldPos[id] = m_modalBase[id] + delta;
        }
    } else if (m_modal == Modal::Scale) {
        // 水平拖动 200 像素 = ×2 / ×0.5（对数手感）
        const float k = std::max(0.01f, 1.0f + d.x / 200.0f);
        for (std::uint32_t id : m_sel) {
            if (id < m_topo.weldPos.size())
                m_topo.weldPos[id] = m_modalPivot + (m_modalBase[id] - m_modalPivot) * k;
        }
    } else {
        // 绕枢轴的 Z 轴（俯视旋转，建模时最常用的一根）。180 像素 = 90°。
        const float ang = (d.x / 180.0f) * (3.14159265f * 0.5f);
        const float c = std::cos(ang), s = std::sin(ang);
        for (std::uint32_t id : m_sel) {
            if (id >= m_topo.weldPos.size()) continue;
            const glm::vec3 rel = m_modalBase[id] - m_modalPivot;
            m_topo.weldPos[id] =
                m_modalPivot + glm::vec3(rel.x * c - rel.y * s,
                                         rel.x * s + rel.y * c, rel.z);
        }
    }
}

void ModelingEditor::commitModal() {
    if (m_modal == Modal::None) return;
    const char* name = m_modal == Modal::Grab
                           ? "grab"
                           : (m_modal == Modal::Scale ? "scale" : "rotate");
    m_modal = Modal::None;
    if (!m_modalMoved) {
        // 只是点了一下就松开：不改几何（否则会白白多出一颗网格资产）
        m_topo.weldPos = m_modalBase;
        VK_LOG_INFO("MODEL-MODAL cancel=%s reason=no-drag", name);
        return;
    }
    upload(name);
    VK_LOG_INFO("MODEL-MODAL commit=%s verts=%d", name,
                static_cast<int>(m_sel.size()));
}

void ModelingEditor::cancelModal() {
    if (m_modal == Modal::None) return;
    m_topo.weldPos = m_modalBase;
    m_modal = Modal::None;
    VK_LOG_INFO("MODEL-MODAL cancel=escape");
}

// ------------------------------------------------------------ 键盘

bool ModelingEditor::handleKeys() {
    if (m_ctx.editMode() != EditMode::Modeling) return false;
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return false;

    // 模态期间收下一切按键：此时 W/E/R 不该顺手把手柄模式也切一遍。
    if (m_modal != Modal::None) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) cancelModal();
        return true;
    }
    if (!m_topo.valid) return false;

    // 三层选择（Blender 的 1/2/3）。
    // ⚠ 这三个键原本归"视图方向"（ViewportPanel::handleViewShortcuts 认主键盘
    //   1/3/7）。建模模式下必须让路 —— 但**只在这一支里让**：返回 true 之后
    //   调用方直接 return，视图快捷键不会再跑。小键盘 1/3/7 仍然可用。
    if (!io.KeyCtrl && !io.KeyAlt && !io.KeyShift) {
        if (ImGui::IsKeyPressed(ImGuiKey_1, false)) {
            m_selMode = SelectMode::Vertex;
            VK_LOG_INFO("MODEL-SELMODE mode=vertex");
            return true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_2, false)) {
            m_selMode = SelectMode::Edge;
            VK_LOG_INFO("MODEL-SELMODE mode=edge");
            return true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_3, false)) {
            m_selMode = SelectMode::Face;
            VK_LOG_INFO("MODEL-SELMODE mode=face");
            return true;
        }
    }

    if (ImGui::IsKeyPressed(ImGuiKey_A, false)) {
        if (io.KeyAlt) {
            selectAll(false);
            return true;
        }
        if (!io.KeyCtrl) {
            selectAll(true);
            return true;
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_E, false)) {
        extrude();
        return true;
    }
    // ---- 拓扑刷子（Blender 键位）----
    // I 内插面 / B 倒角 / Ctrl+R 环切。
    // ⚠ Ctrl+R 必须排在裸 R **前面**：否则按住 Ctrl 也会先命中下面的
    //   `R → 旋转模态`，环切永远按不出来。
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_R, false)) {
        loopCut();
        return true;
    }
    if (!io.KeyCtrl && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_I, false)) {
        insetFaces();
        return true;
    }
    if (!io.KeyCtrl && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_B, false)) {
        bevelVertices();
        return true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_G, false)) {
        beginModal(Modal::Grab);
        return true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        beginModal(Modal::Scale);
        return true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) {
        beginModal(Modal::Rotate);
        return true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_X, false)) {
        deleteFaces();
        return true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_M, false)) {
        mergeSelection();
        return true;
    }
    return false;
}

// ------------------------------------------------------------ 视口

void ModelingEditor::drawOverlay(const glm::vec2& vpPos, const glm::vec2& vpSize,
                                 ImDrawList* dl) {
    if (!dl || !m_topo.valid || m_sel.empty()) return;
    ecs::World& w = m_ctx.editorScene().world();
    if (!w.alive(m_entity)) return;
    const glm::mat4 world = m_ctx.editorScene().worldMatrix(m_entity);
    const glm::mat4 vp = PickingSystem::viewProj(m_ctx.editorScene().camera(), vpSize);
    auto toScreen = [&](const glm::vec3& local, glm::vec2& out) {
        const glm::vec3 wp = glm::vec3(world * glm::vec4(local, 1.0f));
        return PickingSystem::worldToScreen(vp, vpPos, vpSize, wp, out);
    };

    const ImU32 kSelVert = IM_COL32(255, 150, 40, 255);
    const ImU32 kSelEdge = IM_COL32(255, 190, 60, 255);
    const ImU32 kSelFace = IM_COL32(255, 140, 30, 64);

    if (m_selMode == SelectMode::Face) {
        // 按**多边形**画：立方体一个正方形面就是一整块，没有对角线。
        //
        // 这里原来按三角形画（`selFaces()` + `AddTriangle`），于是每个平面
        // 都被一条对角线劈开 —— 用户报的正是这个。
        // 用 AddConcavePolyFilled 而不是 Convex 版：倒角/内插之后会出现
        // 凹多边形（比如 L 形的面），凸版本会画出错误的填充。
        std::vector<ImVec2> pts;
        for (std::uint32_t pi : selPolys()) {
            const auto& p = m_topo.poly[pi];
            if (p.v.size() < 3) continue;
            pts.clear();
            pts.reserve(p.v.size());
            bool ok = true;
            for (std::uint32_t w : p.v) {
                if (w >= m_topo.weldPos.size()) {
                    ok = false;
                    break;
                }
                glm::vec2 s;
                if (!toScreen(m_topo.weldPos[w], s)) {
                    ok = false;
                    break;
                }
                pts.push_back(ImVec2(s.x, s.y));
            }
            if (!ok || pts.size() < 3) continue;
            dl->AddConcavePolyFilled(pts.data(), static_cast<int>(pts.size()),
                                     kSelFace);
            dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), kSelEdge,
                            1.5f, ImDrawFlags_Closed);
        }
        return;
    }

    // 顶点 / 边模式：画点和线。顶点数太多时跳过点（几万个点画上去是一片
    // 橙色，既慢又看不出选区）。
    const bool drawVerts = m_selMode == SelectMode::Vertex &&
                           m_topo.weldPos.size() <= 20000;
    std::unordered_set<std::uint64_t> drawnEdges;
    for (std::size_t f = 0; f < m_topo.tri.size(); ++f) {
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t a = m_topo.tri[f][k];
            const std::uint32_t b = m_topo.tri[f][(k + 1) % 3];
            if (!m_sel.count(a) || !m_sel.count(b)) continue;
            const std::uint32_t lo = std::min(a, b), hi = std::max(a, b);
            const std::uint64_t key = (static_cast<std::uint64_t>(lo) << 32) | hi;
            // 三角形对角线不是"真的边"：立方体一个正方形面上有两条三角形
            // 拼出来的内部边，它在 Blender 里根本不存在，画出来/允许选中只会
            // 让用户以为模型坏了。
            if (m_topo.innerEdge.count(key)) continue;
            if (!drawnEdges.insert(key).second) continue;
            glm::vec2 s0, s1;
            if (!toScreen(m_topo.weldPos[a], s0)) continue;
            if (!toScreen(m_topo.weldPos[b], s1)) continue;
            dl->AddLine(ImVec2(s0.x, s0.y), ImVec2(s1.x, s1.y), kSelEdge, 2.0f);
        }
    }
    if (drawVerts) {
        for (std::uint32_t id : m_sel) {
            glm::vec2 s;
            if (!toScreen(m_topo.weldPos[id], s)) continue;
            dl->AddCircleFilled(ImVec2(s.x, s.y), 3.5f, kSelVert);
        }
    }
}

bool ModelingEditor::viewportInput(const glm::vec2& vpPos,
                                   const glm::vec2& vpSize, bool hovered,
                                   ImDrawList* dl) {
    if (!m_topo.valid) return false;
    const ImGuiIO& io = ImGui::GetIO();
    const glm::vec2 mouse(io.MousePos.x, io.MousePos.y);

    // ---- 模态优先：鼠标移出视口也要能继续拖（Blender 就是这样）----
    if (m_modal != Modal::None) {
        updateModal(mouse);
        drawOverlay(vpPos, vpSize, dl);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            commitModal();
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
                   ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            cancelModal();
        }
        return true;
    }

    if (!hovered) return false;
    drawOverlay(vpPos, vpSize, dl);

    if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
        ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
        return false;
    }
    // 点下来了：先说清楚"函数进来了、数据是什么"。不点就不打（不刷屏），
    // 但一旦点了就必有一条 —— 这样"没反应"能立刻分成三类：
    // 没有这行 = 事件没到 Modelling；没有 PICK-MISS = 命中了；
    // 有 MISS 而 pick 不到 = 射线/几何对不上。
    VK_LOG_INFO("MODEL-VP-CLICK entity=%u verts=%d tris=%d selV=%d",
                static_cast<unsigned>(m_entity.id), m_vertices, m_triangles,
                static_cast<int>(m_sel.size()));

    // ---- 射线 → 物体空间 ----
    ecs::World& w = m_ctx.editorScene().world();
    if (!w.alive(m_entity)) return false;
    const glm::mat4 world = m_ctx.editorScene().worldMatrix(m_entity);
    const Ray ray = PickingSystem::rayFromViewport(
        m_ctx.editorScene().camera(), vpPos, vpSize, mouse);
    const glm::mat4 inv = glm::inverse(world);
    const glm::vec3 ro = glm::vec3(inv * glm::vec4(ray.origin, 1.0f));
    const glm::vec3 rd = glm::vec3(inv * glm::vec4(ray.dir, 0.0f));

    float t = 0.0f;
    const int tri = pickTriangle(ro, rd, t);
    if (tri < 0) {
        VK_LOG_INFO("MODEL-PICK-MISS ro=(%.3f,%.3f,%.3f) rd=(%.3f,%.3f,%.3f)",
                    static_cast<double>(ro.x), static_cast<double>(ro.y),
                    static_cast<double>(ro.z), static_cast<double>(rd.x),
                    static_cast<double>(rd.y), static_cast<double>(rd.z));
        return false;
    }

    const bool add = io.KeyShift;
    const bool sub = io.KeyCtrl;
    std::unordered_set<std::uint32_t> touched;

    if (m_selMode == SelectMode::Face) {
        for (int k = 0; k < 3; ++k)
            touched.insert(m_topo.tri[tri][k]);
    } else {
        // 顶点 / 边：在命中三角形的三个顶点里按**屏幕距离**挑最近的
        const glm::mat4 vp =
            PickingSystem::viewProj(m_ctx.editorScene().camera(), vpSize);
        glm::vec2 sp[3];
        bool ok[3];
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t wid = m_topo.tri[tri][k];
            const glm::vec3 wp =
                glm::vec3(world * glm::vec4(m_topo.weldPos[wid], 1.0f));
            ok[k] = PickingSystem::worldToScreen(vp, vpPos, vpSize, wp, sp[k]);
        }
        if (m_selMode == SelectMode::Vertex) {
            int best = -1;
            float bd = 1e30f;
            for (int k = 0; k < 3; ++k) {
                if (!ok[k]) continue;
                const float d = glm::length(sp[k] - mouse);
                if (d < bd) {
                    bd = d;
                    best = k;
                }
            }
            if (best < 0 || bd > 24.0f) return false;
            touched.insert(m_topo.tri[tri][best]);
        } else {
            int best = -1;
            float bd = 1e30f;
            for (int k = 0; k < 3; ++k) {
                const int k2 = (k + 1) % 3;
                if (!ok[k] || !ok[k2]) continue;
                const float d = screenSegDist(mouse, sp[k], sp[k2]);
                if (d < bd) {
                    bd = d;
                    best = k;
                }
            }
            if (best < 0 || bd > 18.0f) return false;
            touched.insert(m_topo.tri[tri][best]);
            touched.insert(m_topo.tri[tri][(best + 1) % 3]);
        }
    }

    if (!add && !sub) m_sel.clear();
    for (std::uint32_t id : touched) {
        if (sub)
            m_sel.erase(id);
        else
            m_sel.insert(id);
    }
    VK_LOG_INFO("MODEL-PICK mode=%s tri=%d selV=%d selF=%d add=%d sub=%d",
                m_selMode == SelectMode::Vertex
                    ? "vertex"
                    : (m_selMode == SelectMode::Edge ? "edge" : "face"),
                tri, static_cast<int>(m_sel.size()),
                static_cast<int>(selFaces().size()), add ? 1 : 0, sub ? 1 : 0);
    return true;
}

// ------------------------------------------------------------ 面板

void ModelingEditor::drawPanel() {
    if (!beginToolkitWindow(m_ctx, "modeling", "Modeling", 460.0f, 660.0f)) return;
    ImGui::TextDisabled("%s", TR(editModeHint(EditMode::Modeling)));
    ImGui::Separator();

    sync();
    if (m_vertices == 0) {
        ImGui::TextWrapped("%s",
                           TR("Select an entity with a mesh. Every operation "
                              "builds a new mesh from a CPU copy and swaps the "
                              "component over - the original stays intact, so "
                              "undo puts it back."));
        ImGui::End();
        return;
    }

    ImGui::Text(TR("Vertices: %d (%d welded)  ·  triangles: %d"), m_vertices,
                static_cast<int>(m_topo.weldPos.size()), m_triangles);

    // ---- 选择层级（Blender 的 1 / 2 / 3）----
    // 标题特意用 "Selection mode" 而不是 "Selection"：词典里 "Selection"
    // 已经被属性面板的"选中项"占着（一词两义只能改调用点的英文原文）。
    ImGui::SeparatorText(TR("Selection mode"));
    const char* kModes[3] = {"Vertex", "Edge", "Face"};
    for (int i = 0; i < 3; ++i) {
        if (i) ImGui::SameLine();
        const bool on = (static_cast<int>(m_selMode) == i);
        if (on)
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(TR(kModes[i]), ImVec2(120.0f, 26.0f))) {
            m_selMode = static_cast<SelectMode>(i);
            VK_LOG_INFO("MODEL-SELMODE mode=%s", kModes[i]);
        }
        if (on) ImGui::PopStyleColor();
        char tag[32];
        std::snprintf(tag, sizeof(tag), "MD-SELMODE-%d", i);
        logRect(tag, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    }
    ImGui::Text(TR("Selected: %d verts  ·  %d edges  ·  %d faces"),
                m_selectedVerts, m_selectedEdges, m_selectedFaces);
    if (ImGui::Button(TR("Select all"), ImVec2(120.0f, 24.0f))) selectAll(true);
    logRect("MD-SELALL", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::Button(TR("Clear"), ImVec2(120.0f, 24.0f))) selectAll(false);
    logRect("MD-SELCLEAR", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::TextDisabled("%s",
                        TR("Click in the viewport to select: Shift adds, Ctrl "
                           "removes. Keys 1 / 2 / 3 switch the level, A "
                           "selects all, Alt+A clears."));

    // ---- 选区操作 ----
    ImGui::SeparatorText(TR("Edit selection"));
    rowSlider(TR("Extrude distance"), "##md_extrude", &m_extrude, 0.01f, 4.0f,
              "%.2f m", true);
    if (ImGui::Button(TR("Extrude"), ImVec2(-1.0f, 26.0f))) extrude();
    logRect("MD-EXTRUDE", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::TextDisabled("%s",
                        TR("Pushes the selected faces along their normal and "
                           "bridges the border with side walls (E). Only the "
                           "selected faces move - the rest stays put."));

    if (ImGui::Button(TR("Subdivide selected"), ImVec2(150.0f, 26.0f)))
        subdivideSelected();
    logRect("MD-SUBDIVSEL", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::Button(TR("Flip selected"), ImVec2(140.0f, 26.0f)))
        flipSelected();
    logRect("MD-FLIP", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::Button(TR("Delete"), ImVec2(110.0f, 26.0f))) deleteFaces();
    logRect("MD-DELETE", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    ImGui::TextDisabled("%s",
                        TR("Viewport keys: G drag · S scale · R rotate · "
                           "M merge to center · X delete. Drag with the left "
                           "button to confirm, right button or Esc to cancel."));

    // ---- 拓扑刷子（Blender 的 I / Ctrl+B / Ctrl+R）----
    //
    // 这三个是"改面表"的操作，与上面的挤出/细分同一个层级：都只作用在
    // 选区上，都是一次上传。
    ImGui::SeparatorText(TR("Topology brushes"));
    rowSlider(TR("Inset amount"), "##md_inset", &m_inset, 0.005f, 1.0f,
              "%.3f m");
    rowSlider(TR("Bevel width"), "##md_bevel", &m_bevel, 0.005f, 1.0f,
              "%.3f m");
    rowSliderInt(TR("Loop cuts"), "##md_loops", &m_loopCount, 1, 8);

    if (ImGui::Button(TR("Inset faces"), ImVec2(150.0f, 26.0f))) insetFaces();
    logRect("MD-INSET", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::Button(TR("Bevel"), ImVec2(120.0f, 26.0f))) bevelVertices();
    logRect("MD-BEVEL", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::Button(TR("Loop cut"), ImVec2(120.0f, 26.0f))) loopCut();
    logRect("MD-LOOPCUT", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    ImGui::TextDisabled("%s",
                        TR("Inset: shrink each selected face and bridge the rim "
                           "with a ring of quads (I).\nBevel: cut every corner "
                           "of the selected vertices (B).\nLoop cut: walk the "
                           "quad strip through the selected face and insert "
                           "an edge ring (Ctrl+R)."));

    // ---- 整网格操作（不需要选区）----
    ImGui::SeparatorText(TR("Whole mesh"));
    rowSlider(TR("Smooth amount"), "##md_smooth", &m_smoothAmount, 0.05f, 1.0f,
              "%.2f");
    if (ImGui::Button(TR("Smooth"), ImVec2(140.0f, 26.0f)))
        smoothAll(m_smoothAmount);
    logRect("MD-SMOOTH", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::Button(TR("Subdivide all"), ImVec2(140.0f, 26.0f))) {
        // 全网格细分 = 选中所有面再走同一条路径（一份实现，两种入口）。
        // 锚点仍然是 MD-SUBDIV：**老的验收脚本按它点"整网格细分"**，
        // 换成"只细分选区"会让那条断言变成"点了没反应"。
        selectAll(true);
        subdivideSelected("subdivide");
    }
    logRect("MD-SUBDIV", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::Button(TR("Mirror X"), ImVec2(120.0f, 26.0f))) mirrorX();
    logRect("MD-MIRROR", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    ImGui::Separator();
    ImGui::TextDisabled("%s", TR("Each operation is one upload; Ctrl+Z in the "
                                 "viewport undo stack restores the previous "
                                 "mesh pointer."));
    ImGui::End();
}
// ============================================================
// 3. 破裂
// ============================================================
//
// 两种切法，面板上可选：
//
// ------------------------------------------------------------
// Voronoi（默认）—— 真断裂
// ------------------------------------------------------------
// 撒 N 个种子点，碎块 = 每个种子点的 Voronoi 胞腔与源网格的交集。实现上
// **不做 CSG**（本工程没有布尔运算），而是：
//
//   1. 取源网格的三角形（物体空间）；
//   2. 对每个胞腔 i，把每个三角形依次用"种子 i 与每个邻居种子的中垂面"
//      去裁（3D Sutherland–Hodgman，保留离 i 更近的那一侧）；
//   3. 裁出来的多边形扇形三角化 → 这就是"沿真实表面崩开"的那部分；
//   4. 封口：裁切会在网格上开一个洞，洞里那块正好是"胞腔与某个中垂面
//      的交"。因为 Voronoi 胞腔是**凸**的，它与平面的交也是凸多边形，所以
//      把落在该平面上的点做 2D 凸包就是精确的断面；
//   5. 每个胞腔一个独立网格 + 实体，位置 = 胞腔中心（顶点已居中）。
//
// 为什么这样就"真"了：切面法线是任意方向（不再是坐标轴），碎块形状是
// 不规则凸体，碎块之间能拼回原物（`gap = 0` 时）。
//
// 代价：N 个胞腔 × M 个三角形 × (N-1) 个平面。10k 三角形 × 20 胞腔 就是
// 380 万次多边形裁剪 —— 面板上有一道上限保护，超了就提示而不是把编辑器
// 卡死。
//
// ------------------------------------------------------------
// Grid —— 旧版
// ------------------------------------------------------------
// 轴对齐格子 + 抖动。留着是因为它便宜、形状规整，"砖墙被打碎"这类其实
// 比 Voronoi 更合适，而且能当 Voronoi 的低配对照。
namespace {

struct Box3 {
    glm::vec3 lo{0.0f};
    glm::vec3 hi{0.0f};
};

// 选中网格在**世界空间**的包围盒（把 8 个角都变换一遍 —— 只变换 min/max
// 在物体旋转时会给错框）
Box3 worldBounds(const assets::Mesh& mesh, const glm::mat4& world) {
    const glm::vec3 a = mesh.boundsMin();
    const glm::vec3 b = mesh.boundsMax();
    Box3 out;
    bool first = true;
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 corner((i & 1) ? b.x : a.x, (i & 2) ? b.y : a.y,
                               (i & 4) ? b.z : a.z);
        const glm::vec3 wp = glm::vec3(world * glm::vec4(corner, 1.0f));
        if (first) {
            out.lo = out.hi = wp;
            first = false;
        } else {
            out.lo = glm::min(out.lo, wp);
            out.hi = glm::max(out.hi, wp);
        }
    }
    return out;
}

// 带法线的多边形顶点（裁剪时插值法线，这样"沿原表面"的部分保留原着色，
// 新断面才是硬边）
struct PolyVert {
    glm::vec3 pos{0.0f};
    glm::vec3 nrm{0.0f, 0.0f, 1.0f};
};

// 用平面 `dot(n, p) <= d` 裁多边形（保留满足条件的一侧 = 离自己的种子更近）。
// Sutherland–Hodgman：逐边判断，跨越时插入交点。
void clipPoly(const std::vector<PolyVert>& in, const glm::vec3& n, float d,
              std::vector<PolyVert>& out) {
    out.clear();
    const std::size_t cnt = in.size();
    if (cnt == 0) return;
    out.reserve(cnt + 4);
    for (std::size_t i = 0; i < cnt; ++i) {
        const PolyVert& a = in[i];
        const PolyVert& b = in[(i + 1) % cnt];
        const float da = glm::dot(n, a.pos) - d;
        const float db = glm::dot(n, b.pos) - d;
        const bool ina = da <= 1e-6f;
        const bool inb = db <= 1e-6f;
        if (ina) out.push_back(a);
        if (ina != inb) {
            // 交点参数：da → db 之间过零
            const float t = da / (da - db);
            PolyVert m;
            m.pos = a.pos + (b.pos - a.pos) * t;
            m.nrm = glm::normalize(a.nrm + (b.nrm - a.nrm) * t +
                                   glm::vec3(1e-9f));
            out.push_back(m);
        }
    }
}

// 一个碎块（顶点最终会挪到世界空间并居中）
struct ChunkBuild {
    std::vector<glm::vec3> pos;
    std::vector<glm::vec3> nrm;
    std::vector<std::uint32_t> idx;

    std::uint32_t addVert(const glm::vec3& p, const glm::vec3& n) {
        const std::uint32_t i = static_cast<std::uint32_t>(pos.size());
        pos.push_back(p);
        nrm.push_back(n);
        return i;
    }
    void addTri(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        idx.push_back(a);
        idx.push_back(b);
        idx.push_back(c);
    }
};

// 平面内的 2D 凸包（Andrew monotone chain），返回**逆时针**的顶点下标。
// 输出顺序按 (u, v, n) 右手基的逆时针排列 —— 于是扇形三角化的法线朝 +n。
std::vector<std::uint32_t> convexHull2D(std::vector<glm::vec2>& pts) {
    const std::size_t n = pts.size();
    std::vector<std::uint32_t> order(n);
    for (std::size_t i = 0; i < n; ++i)
        order[i] = static_cast<std::uint32_t>(i);
    std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
        if (pts[a].x != pts[b].x) return pts[a].x < pts[b].x;
        return pts[a].y < pts[b].y;
    });
    auto cross2 = [&](std::uint32_t o, std::uint32_t a, std::uint32_t b) {
        return (pts[a].x - pts[o].x) * (pts[b].y - pts[o].y) -
               (pts[a].y - pts[o].y) * (pts[b].x - pts[o].x);
    };
    std::vector<std::uint32_t> hull(2 * n + 1);
    std::size_t k = 0;
    for (std::size_t i = 0; i < n; ++i) {
        while (k >= 2 && cross2(hull[k - 2], hull[k - 1], order[i]) <= 1e-12f)
            --k;
        hull[k++] = order[i];
    }
    for (std::size_t i = n - 1, t = k + 1; i-- > 0;) {
        while (k >= t && cross2(hull[k - 2], hull[k - 1], order[i]) <= 1e-12f)
            --k;
        hull[k++] = order[i];
    }
    hull.resize(k > 0 ? k - 1 : 0);  // 最后一个点与第一个重复
    return hull;
}

} // namespace

// ------------------------------------------------------------ Voronoi

void FractureEditor::breakVoronoi(ecs::Entity e, assets::Mesh* mesh,
                                  const glm::mat4& world) {
    // ---- 源三角形（物体空间 + 局部包围盒里的种子）----
    const std::vector<assets::Vertex> sv = mesh->vertices();
    const std::vector<std::uint32_t> si = mesh->indices();
    const int srcTris = static_cast<int>(si.size() / 3);
    if (srcTris <= 0) {
        VK_LOG_ERROR("FRACTURE-FAIL 源网格没有三角形");
        return;
    }

    const int cells = std::max(2, m_chunks);
    // 上限保护：一次裁剪的开销是 cells × tris × planes。超过 300 万次就
    // 不是"慢"而是"编辑器假死"了 —— 宁可拒绝并说清楚怎么降。
    const long long work =
        static_cast<long long>(cells) * srcTris * (cells + 1);
    if (work > 6000000LL) {
        VK_LOG_ERROR("FRACTURE-FAIL cells=%d tris=%d work=%lld 超上限"
                     "（6000000）—— 降低碎块数或先给网格减面",
                     cells, srcTris, work);
        m_ctx.notify(TR("Too many cells for this mesh: lower the cell count "
                        "or decimate the mesh first."));
        return;
    }

    // 局部包围盒 → 种子点撒在**局部**空间（裁剪也在局部空间做，最后整块
    // 碎块再变换到世界，这样源实体有旋转/缩放也不需要把几何 bake 到世界）。
    const glm::vec3 lo = mesh->boundsMin();
    const glm::vec3 hi = mesh->boundsMax();
    const glm::vec3 size = glm::max(hi - lo, glm::vec3(1e-3f));

    Rng rng(static_cast<std::uint32_t>(m_seed));
    // 规则格心 + 抖动。规则网格保证胞腔体积相近（不会出现"一大块 + 一堆
    // 灰尘"），抖动负责把切面从轴对齐上打散 —— **不抖**的话种子排成方格、
    // 中垂面就是三个方向的对称平面，等于又回到了 Grid。
    int nx = std::max(1, static_cast<int>(std::floor(
                             std::cbrt(static_cast<double>(cells)))));
    while (nx * nx * nx < cells) ++nx;
    const int ny = nx, nz = nx;

    std::vector<glm::vec3> seeds;
    seeds.reserve(static_cast<std::size_t>(nx) * ny * nz);
    const float jit = std::clamp(m_irregularity, 0.0f, 1.0f) * 0.48f;
    for (int i = 0; i < nx; ++i) {
        for (int j = 0; j < ny; ++j) {
            for (int k = 0; k < nz; ++k) {
                const glm::vec3 cell(size.x / nx, size.y / ny, size.z / nz);
                glm::vec3 p(lo.x + (i + 0.5f) * cell.x,
                            lo.y + (j + 0.5f) * cell.y,
                            lo.z + (k + 0.5f) * cell.z);
                p.x += rng.range(-1.0f, 1.0f) * cell.x * jit;
                p.y += rng.range(-1.0f, 1.0f) * cell.y * jit;
                p.z += rng.range(-1.0f, 1.0f) * cell.z * jit;
                seeds.push_back(p);
            }
        }
    }
    const int cellCount = static_cast<int>(seeds.size());

    // ---- 预取三角形的顶点与法线（裁剪要插值法线）----
    struct SrcTri {
        PolyVert v[3];
    };
    std::vector<SrcTri> tris;
    tris.reserve(srcTris);
    for (std::size_t t = 0; t + 2 < si.size(); t += 3) {
        const std::uint32_t a = si[t], b = si[t + 1], c = si[t + 2];
        if (a >= sv.size() || b >= sv.size() || c >= sv.size()) continue;
        SrcTri st;
        st.v[0].pos = sv[a].pos;
        st.v[1].pos = sv[b].pos;
        st.v[2].pos = sv[c].pos;
        st.v[0].nrm = sv[a].normal;
        st.v[1].nrm = sv[b].normal;
        st.v[2].nrm = sv[c].normal;
        tris.push_back(st);
    }
    if (tris.empty()) {
        VK_LOG_ERROR("FRACTURE-FAIL 源三角形全部非法（索引越界）");
        return;
    }

    std::vector<glm::vec3> planeN(static_cast<std::size_t>(cellCount));
    std::vector<float> planeD(static_cast<std::size_t>(cellCount));

    assets::Material* mat = nullptr;
    ecs::World& w = m_ctx.editorScene().world();
    if (auto* mc = w.get<ecs::MaterialComponent>(e)) mat = mc->material;

    int totalTris = 0;
    int madeChunks = 0;
    const glm::mat3 nrmMat = glm::transpose(glm::inverse(glm::mat3(world)));
    // 碎块实体（只用来计数与日志；真正的句柄已经在 World 里）
    std::vector<ecs::Entity> pieces;

    m_ctx.structuralEdit("Fracture (Voronoi)", [&] {
        for (int ci = 0; ci < cellCount; ++ci) {
            const glm::vec3 self = seeds[ci];
            // 中垂面 j：法线 (seeds[j]-self) 归一化，平面过中点。
            // d = +INF 表示"这个平面不裁"（自己是自己 / 种子重合）。
            for (int cj = 0; cj < cellCount; ++cj) {
                glm::vec3 dir = seeds[cj] - self;
                const float len = glm::length(dir);
                if (cj == ci || len < 1e-6f) {
                    planeN[cj] = glm::vec3(0.0f);
                    planeD[cj] = 1e30f;
                    continue;
                }
                dir /= len;
                planeN[cj] = dir;
                planeD[cj] = glm::dot(dir, (self + seeds[cj]) * 0.5f);
            }

            ChunkBuild cb;
            // 封口点：每个平面收集"落在该平面上的顶点"，最后做 2D 凸包
            std::vector<std::vector<glm::vec3>> onPlane(
                static_cast<std::size_t>(cellCount));

            std::vector<PolyVert> poly, tmp;
            for (const SrcTri& st : tris) {
                poly.assign({st.v[0], st.v[1], st.v[2]});
                bool alive = true;
                for (int cj = 0; cj < cellCount; ++cj) {
                    if (planeN[cj] == glm::vec3(0.0f)) continue;
                    clipPoly(poly, planeN[cj], planeD[cj], tmp);
                    poly.swap(tmp);
                    if (poly.size() < 3) {
                        alive = false;
                        break;
                    }
                }
                if (!alive) continue;
                // 扇形三角化 + 收集封口点
                const std::uint32_t base =
                    static_cast<std::uint32_t>(cb.pos.size());
                for (const PolyVert& p : poly) cb.addVert(p.pos, p.nrm);
                for (std::size_t k = 1; k + 1 < poly.size(); ++k)
                    cb.addTri(base, base + static_cast<std::uint32_t>(k),
                              base + static_cast<std::uint32_t>(k + 1));
                for (const PolyVert& p : poly) {
                    for (int cj = 0; cj < cellCount; ++cj) {
                        if (planeN[cj] == glm::vec3(0.0f)) continue;
                        if (std::fabs(glm::dot(planeN[cj], p.pos) - planeD[cj]) <
                            1e-4f)
                            onPlane[cj].push_back(p.pos);
                    }
                }
            }

            if (cb.idx.empty()) continue;

            // ---- 封口 ----
            // 把落在每个中垂面上的点做 2D 凸包 → 扇形三角化。Voronoi 胞腔是
            // 凸的，所以凸包就是精确断面（不会多盖也不会漏）。
            int caps = 0;
            for (int cj = 0; cj < cellCount; ++cj) {
                if (planeN[cj] == glm::vec3(0.0f)) continue;
                if (onPlane[cj].size() < 3) continue;
                const glm::vec3 n = planeN[cj];
                // 平面基：(u, v, n) 右手系
                const glm::vec3 helper = (std::fabs(n.z) < 0.9f)
                                             ? glm::vec3(0.0f, 0.0f, 1.0f)
                                             : glm::vec3(1.0f, 0.0f, 0.0f);
                const glm::vec3 u = glm::normalize(glm::cross(helper, n));
                const glm::vec3 v = glm::cross(n, u);

                // 去重（同一点被多个三角形共享时会出现很多份）
                std::vector<glm::vec2> pts2;
                std::vector<glm::vec3> pts3;
                pts2.reserve(onPlane[cj].size());
                for (const glm::vec3& p : onPlane[cj]) {
                    const glm::vec2 q(glm::dot(p, u), glm::dot(p, v));
                    bool dup = false;
                    for (const glm::vec2& e2 : pts2) {
                        if (glm::length(e2 - q) < 1e-5f) {
                            dup = true;
                            break;
                        }
                    }
                    if (dup) continue;
                    pts2.push_back(q);
                    pts3.push_back(p);
                }
                if (pts2.size() < 3) continue;

                const std::vector<std::uint32_t> hull = convexHull2D(pts2);
                if (hull.size() < 3) continue;

                // 断面朝内缩一点造缝（gap）。不缩的话碎块拼回去是严丝合缝的
                // 一个整体 —— 看不出断过。
                glm::vec3 center(0.0f);
                for (std::uint32_t hi : hull) center += pts3[hi];
                center /= static_cast<float>(hull.size());
                const float shrink = std::clamp(m_gap, 0.0f, 0.6f);

                const std::uint32_t base =
                    static_cast<std::uint32_t>(cb.pos.size());
                for (std::uint32_t hi : hull)
                    cb.addVert(glm::mix(pts3[hi], center, shrink), n);
                for (std::size_t k = 1; k + 1 < hull.size(); ++k)
                    cb.addTri(base, base + static_cast<std::uint32_t>(k),
                              base + static_cast<std::uint32_t>(k + 1));
                ++caps;
            }

            // ---- 居中 → 变换到世界 ----
            Box3 b;
            b.lo = b.hi = cb.pos[0];
            for (const glm::vec3& p : cb.pos) {
                b.lo = glm::min(b.lo, p);
                b.hi = glm::max(b.hi, p);
            }
            const glm::vec3 center = (b.lo + b.hi) * 0.5f;
            const glm::vec3 worldCenter =
                glm::vec3(world * glm::vec4(center, 1.0f));

            // 顶点直接烘到"世界空间、以世界中心为原点"。这样碎块实体的
            // rotation/scale 都是 identity —— 源实体有旋转或非均匀缩放时
            // 也不会让碎块错位（那是"把几何烘焙掉"，Blender 的 apply
            // transform 同款做法）。
            std::vector<assets::Vertex> outVerts(cb.pos.size());
            for (std::size_t i = 0; i < cb.pos.size(); ++i) {
                outVerts[i].pos =
                    glm::vec3(world * glm::vec4(cb.pos[i], 1.0f)) - worldCenter;
                // 法线要用法线矩阵：非均匀缩放时直接用 world 会把法线拉歪
                const glm::vec3 nn = nrmMat * cb.nrm[i];
                outVerts[i].normal = (glm::length(nn) > 1e-8f)
                                         ? glm::normalize(nn)
                                         : glm::vec3(0.0f, 0.0f, 1.0f);
                outVerts[i].uv = glm::vec2(0.0f);
                outVerts[i].color = glm::vec4(1.0f);
            }

            char key[96];
            std::snprintf(key, sizeof(key), "toolkit/fracture/%u/%d/%d",
                          static_cast<unsigned>(e.id), m_seed, ci);
            assets::Mesh* piece =
                m_ctx.assets().registerMesh(key, outVerts, cb.idx);
            if (!piece) {
                VK_LOG_ERROR("FRACTURE-FAIL 注册碎块网格失败 ci=%d", ci);
                continue;
            }

            char name[48];
            std::snprintf(name, sizeof(name), "Chunk %02d", ci);
            const ecs::Entity pe = m_ctx.editorScene().createObject(name);
            if (auto* tc = w.get<ecs::TransformComponent>(pe)) {
                tc->position = worldCenter;
                tc->rotation = glm::vec3(0.0f);
                tc->scale = glm::vec3(1.0f);
            }
            if (auto* mc = w.get<ecs::MeshComponent>(pe)) {
                mc->mesh = piece;
            } else {
                ecs::MeshComponent m;
                m.mesh = piece;
                w.add<ecs::MeshComponent>(pe, m);
            }
            ecs::MaterialComponent matc;
            matc.material = mat;
            w.add<ecs::MaterialComponent>(pe, matc);

            if (m_addPhysics) {
                const glm::vec3 half = glm::max((b.hi - b.lo) * 0.5f,
                                                glm::vec3(0.02f));
                ecs::RigidBodyComponent rb;
                rb.bodyType = ecs::PhysicsBodyType::Dynamic;
                // 碎块是不规则凸体 → 走凸包。Box 只对轴对齐的方块成立，
                // 用在这里会让物理形状和画面差很远（尤其是很薄的碎块）。
                rb.shape = ecs::PhysicsShapeType::ConvexHull;
                rb.halfExtents = half;
                w.add<ecs::RigidBodyComponent>(pe, rb);
                ecs::CollisionComponent cc;
                cc.shape = ecs::ColliderShape::ConvexHull;
                cc.scale = glm::vec3(1.0f);
                w.add<ecs::CollisionComponent>(pe, cc);
            }
            pieces.push_back(pe);
            totalTris += static_cast<int>(cb.idx.size() / 3);
            ++madeChunks;
            VK_LOG_INFO("FRACTURE-CELL index=%d verts=%d tris=%d caps=%d "
                        "center=(%.2f,%.2f,%.2f)",
                        ci, static_cast<int>(outVerts.size()),
                        static_cast<int>(cb.idx.size() / 3), caps,
                        static_cast<double>(worldCenter.x),
                        static_cast<double>(worldCenter.y),
                        static_cast<double>(worldCenter.z));
        }

        // 源物体隐藏而不是删除：删了就是"原地消失"，而隐藏之后 undo
        // （整场景快照）能把它原样恢复 —— 也不破坏它身上其它组件。
        if (m_hideSource) {
            if (auto* vis = w.get<ecs::VisibilityComponent>(e)) {
                vis->visible = false;
            } else {
                // ⚠ 变量不能叫 `vis` —— if-init 声明的 `auto* vis` 在 else
                //   分支里仍然在作用域内，同名就成了一次重声明。
                ecs::VisibilityComponent hide;
                hide.visible = false;
                w.add<ecs::VisibilityComponent>(e, hide);
            }
        }
    });

    m_lastChunks = madeChunks;
    m_lastTriangles = totalTris;
    m_ctx.dirty() = true;
    VK_LOG_INFO("FRACTURE chunks=%d method=voronoi cells=%d srcTris=%d "
                "outTris=%d gap=%.2f seed=%d physics=%d",
                m_lastChunks, cellCount, srcTris, totalTris,
                static_cast<double>(m_gap), m_seed, m_addPhysics ? 1 : 0);
    m_ctx.notify(std::string(TR("Fractured into ")) +
                 std::to_string(m_lastChunks) + TR(" chunks"));
}

// ------------------------------------------------------------ Grid（旧版）

void FractureEditor::breakGrid(ecs::Entity e, assets::Mesh* mesh,
                               const glm::mat4& world) {
    const Box3 box = worldBounds(*mesh, world);
    const glm::vec3 size = box.hi - box.lo;
    if (size.x <= 1e-4f || size.y <= 1e-4f || size.z <= 1e-4f) return;

    // 块数按立方根分到三轴：8 块 → 2×2×2，27 → 3×3×3。
    const int n = std::max(m_chunks, 1);
    int nx = std::max(1, static_cast<int>(std::floor(std::cbrt(
                             static_cast<double>(n)))));
    int ny = nx, nz = nx;
    if (nx * ny * nz < n) ++nx;
    if (nx * ny * nz < n) ++ny;
    if (nx * ny * nz < n) ++nz;

    Rng rng(static_cast<std::uint32_t>(m_seed));

    assets::Material* mat = nullptr;
    ecs::World& w = m_ctx.editorScene().world();
    if (auto* mc = w.get<ecs::MaterialComponent>(e)) mat = mc->material;

    std::vector<ecs::Entity> pieces;
    m_ctx.structuralEdit("Fracture (Grid)", [&] {
        for (int i = 0; i < nx; ++i) {
            for (int j = 0; j < ny; ++j) {
                for (int k = 0; k < nz; ++k) {
                    const glm::vec3 t0(static_cast<float>(i) / nx,
                                       static_cast<float>(j) / ny,
                                       static_cast<float>(k) / nz);
                    const glm::vec3 t1(static_cast<float>(i + 1) / nx,
                                       static_cast<float>(j + 1) / ny,
                                       static_cast<float>(k + 1) / nz);
                    const glm::vec3 c = box.lo + (t0 + t1) * 0.5f * size;
                    const glm::vec3 cell = (t1 - t0) * size;
                    const float jitterAmt = 0.25f;
                    const glm::vec3 jitter(
                        rng.range(-1.0f, 1.0f) * cell.x * jitterAmt,
                        rng.range(-1.0f, 1.0f) * cell.y * jitterAmt,
                        rng.range(-1.0f, 1.0f) * cell.z * jitterAmt);
                    const glm::vec3 shrink = glm::max(
                        cell * (1.0f - std::clamp(m_gap, 0.0f, 0.8f)),
                        glm::vec3(0.02f));

                    char name[48];
                    std::snprintf(name, sizeof(name), "Chunk %02d%02d%02d", i, j,
                                  k);
                    const ecs::Entity piece =
                        m_ctx.editorScene().createObject(name);
                    if (auto* tc = w.get<ecs::TransformComponent>(piece)) {
                        tc->position = c + jitter;
                        tc->rotation = glm::vec3(rng.range(-0.35f, 0.35f),
                                                 rng.range(-0.35f, 0.35f),
                                                 rng.range(-3.14f, 3.14f));
                        tc->scale = shrink * 0.5f;  // cube 的边长是 2×size
                    }
                    assets::Mesh* cube =
                        m_ctx.assets().cube(1.0f, "toolkit/fracture/cube");
                    if (auto* mc = w.get<ecs::MeshComponent>(piece)) {
                        mc->mesh = cube;
                    } else {
                        ecs::MeshComponent m;
                        m.mesh = cube;
                        w.add<ecs::MeshComponent>(piece, m);
                    }
                    ecs::MaterialComponent matc;
                    matc.material = mat;
                    w.add<ecs::MaterialComponent>(piece, matc);

                    if (m_addPhysics) {
                        ecs::RigidBodyComponent rb;
                        rb.bodyType = ecs::PhysicsBodyType::Dynamic;
                        rb.shape = ecs::PhysicsShapeType::Box;
                        rb.halfExtents = shrink * 0.5f;
                        w.add<ecs::RigidBodyComponent>(piece, rb);
                        ecs::CollisionComponent cc;
                        cc.shape = ecs::ColliderShape::ConvexHull;
                        cc.scale = shrink;
                        w.add<ecs::CollisionComponent>(piece, cc);
                    }
                    pieces.push_back(piece);
                }
            }
        }
        if (m_hideSource) {
            if (auto* vis = w.get<ecs::VisibilityComponent>(e)) {
                vis->visible = false;
            } else {
                ecs::VisibilityComponent hide;
                hide.visible = false;
                w.add<ecs::VisibilityComponent>(e, hide);
            }
        }
    });

    m_lastChunks = static_cast<int>(pieces.size());
    m_lastTriangles = m_lastChunks * 12;
    m_ctx.dirty() = true;
    VK_LOG_INFO("FRACTURE chunks=%d method=grid grid=%dx%dx%d seed=%d "
                "physics=%d bounds=(%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f)",
                m_lastChunks, nx, ny, nz, m_seed, m_addPhysics ? 1 : 0,
                static_cast<double>(box.lo.x), static_cast<double>(box.lo.y),
                static_cast<double>(box.lo.z), static_cast<double>(box.hi.x),
                static_cast<double>(box.hi.y), static_cast<double>(box.hi.z));
    m_ctx.notify(std::string(TR("Fractured into ")) +
                 std::to_string(m_lastChunks) + TR(" chunks"));
}

void FractureEditor::breakSelected() {
    ecs::Entity e{};
    assets::Mesh* mesh = nullptr;
    glm::mat4 world(1.0f);
    if (!selectedMesh(m_ctx, e, mesh, world)) {
        VK_LOG_INFO("FRACTURE-FAIL 没有选中带网格的实体");
        return;
    }
    if (m_method == 1)
        breakGrid(e, mesh, world);
    else
        breakVoronoi(e, mesh, world);
}

void FractureEditor::drawPanel() {
    if (!beginToolkitWindow(m_ctx, "fracture", "Fracture", 440.0f, 620.0f)) return;
    ImGui::TextDisabled("%s", TR(editModeHint(EditMode::Fracture)));
    ImGui::Separator();

    ecs::Entity e{};
    assets::Mesh* mesh = nullptr;
    glm::mat4 world(1.0f);
    if (!selectedMesh(m_ctx, e, mesh, world)) {
        ImGui::TextWrapped("%s",
                           TR("Select an entity with a mesh. Voronoi cuts the "
                              "mesh with mid-planes between seed points, so "
                              "the chunks follow the real surface."));
        ImGui::End();
        return;
    }

    // ---- 切法 ----
    ImGui::SeparatorText(TR("Method"));
    const bool voronoi = (m_method == 0);
    if (voronoi)
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    // 切换时打一条状态日志。验收脚本原来靠"FR-BREAK 的屏幕 y 变了"来判
    // "Irregularity 那行消失了"，但左栏参数区只有 ~400px 高、面板滚到底时
    // 删掉一行会被滚动条 clamp 回去 —— FR-BREAK 的 y 几乎不动，判据就假失败。
    // 有这条日志就能直接断言"方法真的切了"，跟滚动位置无关。
    if (ImGui::Button(TR("Voronoi"), ImVec2(150.0f, 26.0f)) && m_method != 0) {
        m_method = 0;
        VK_LOG_INFO("FR-METHOD set=voronoi");
    }
    if (voronoi) ImGui::PopStyleColor();
    logRect("FR-METHOD-VORONOI", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (!voronoi)
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button(TR("Grid"), ImVec2(120.0f, 26.0f)) && m_method != 1) {
        m_method = 1;
        VK_LOG_INFO("FR-METHOD set=grid");
    }
    if (!voronoi) ImGui::PopStyleColor();
    logRect("FR-METHOD-GRID", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::TextDisabled("%s",
                        TR("Voronoi: cut planes point anywhere, chunks follow "
                           "the original surface. Grid: axis-aligned boxes."));

    rowSliderInt(TR("Cells"), "##fr_chunks", &m_chunks, 2, 200);
    if (m_method == 0) {
        rowSlider(TR("Irregularity"), "##fr_irr", &m_irregularity, 0.0f, 1.0f,
                  "%.2f");
        ImGui::TextDisabled("%s",
                            TR("0 keeps the seed grid regular (equal-size "
                               "cells), 1 scatters them fully - small shards "
                               "next to big ones."));
    }
    rowSlider(TR("Gap"), "##fr_gap", &m_gap, 0.0f, 0.6f, "%.2f");
    ImGui::TextUnformatted(TR("Seed"));
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::DragInt("##fr_seed", &m_seed, 1.0f, 0, 999999);
    ImGui::Checkbox(TR("Add rigid bodies"), &m_addPhysics);
    ImGui::Checkbox(TR("Hide the source mesh"), &m_hideSource);

    ImGui::Spacing();
    if (ImGui::Button(TR("Break"), ImVec2(-1.0f, 30.0f))) breakSelected();
    logRect("FR-BREAK", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::Text(TR("Last break: %d chunks, %d triangles"), m_lastChunks,
                m_lastTriangles);
    ImGui::Spacing();
    ImGui::TextDisabled("%s",
                        TR("Each chunk is its own mesh entity with a convex "
                           "collider. Press Play to watch rigid bodies fall."));
    ImGui::End();
}

// ============================================================
// 4. 笔刷编辑（用实体当"刷子体"）
// ============================================================
void BrushEditor::placeAt(const glm::vec3& world) {
    glm::vec3 p = world;
    if (m_snap > 1e-4f) {
        p.x = std::round(p.x / m_snap) * m_snap;
        p.y = std::round(p.y / m_snap) * m_snap;
        p.z = std::round(p.z / m_snap) * m_snap;
    }
    // 贴地：让刷子体的**底面**落在命中点上（而不是中心埋进地里）
    if (m_dropToGround) p.z += m_size.z * 0.5f;

    assets::Mesh* mesh = nullptr;
    const char* shapeName = "cube";
    switch (m_shape) {
        case 0:
            mesh = m_ctx.assets().cube(1.0f, "toolkit/brush/box");
            shapeName = "cube";
            break;
        case 1:
            mesh = m_ctx.assets().cylinder(0.5f, "toolkit/brush/cyl");
            shapeName = "cylinder";
            break;
        default:
            mesh = m_ctx.assets().sphere(0.5f, "toolkit/brush/sphere");
            shapeName = "sphere";
            break;
    }
    if (!mesh) return;

    // sphere/cylinder 的生成器尺寸是"半径 0.5 / 高 1"，所以 scale 直接用
    // 目标尺寸（不用乘 2）—— 与 cube（边长 1）同一口径。
    ecs::Entity e{};
    m_ctx.structuralEdit("Place brush", [&] {
        char name[48];
        std::snprintf(name, sizeof(name), "Brush %s %02d", shapeName, ++m_placed);
        e = m_ctx.editorScene().createObject(name);
        ecs::World& w = m_ctx.editorScene().world();
        if (auto* t = w.get<ecs::TransformComponent>(e)) {
            t->position = p;
            t->scale = m_size;
        }
        ecs::MeshComponent mc;
        mc.mesh = mesh;
        w.add<ecs::MeshComponent>(e, mc);
        ecs::MaterialComponent matc;
        // 不能为空：空材质在颜色通道会被 `continue` 掉（只有深度预通道画），
        // 实体就在层级里、却怎么也看不见。理由详见 resolveToolkitMaterial。
        matc.material = resolveToolkitMaterial(
            m_ctx, "materials/wall", glm::vec4(0.72f, 0.72f, 0.74f, 1.0f));
        w.add<ecs::MaterialComponent>(e, matc);
        if (m_addCollision) {
            // ⚠ ColliderShape 只有 ConvexHull / Capsule（没有 Box / Cylinder /
            //   Sphere）—— 拼关卡用的刷子体统一走凸包：圆柱的凸包就是圆柱，
            //   球的凸包就是球，语义正好对。
            ecs::CollisionComponent cc;
            cc.shape = ecs::ColliderShape::ConvexHull;
            cc.scale = m_size;
            w.add<ecs::CollisionComponent>(e, cc);
            // 刚体这一层有 Box / Sphere / Capsule 可选，按形状给更省的
            // 解析形状（凸包每帧要做 GJK，方盒只是 3 个轴的区间测试）。
            ecs::RigidBodyComponent rb;
            rb.bodyType = ecs::PhysicsBodyType::Static;
            rb.shape = m_shape == 0   ? ecs::PhysicsShapeType::Box
                       : m_shape == 1 ? ecs::PhysicsShapeType::Capsule
                                      : ecs::PhysicsShapeType::Sphere;
            rb.halfExtents = m_size * 0.5f;
            rb.radius = std::max(m_size.x, m_size.z) * 0.5f;
            rb.halfHeight = m_size.z * 0.5f;
            w.add<ecs::RigidBodyComponent>(e, rb);
        }
    });
    if (e.valid()) m_ctx.select(e);
    m_ctx.dirty() = true;
    VK_LOG_INFO("BRUSH-PLACE shape=%s pos=(%.2f,%.2f,%.2f) size=(%.2f,%.2f,"
                "%.2f) collision=%d",
                shapeName, static_cast<double>(p.x), static_cast<double>(p.y),
                static_cast<double>(p.z), static_cast<double>(m_size.x),
                static_cast<double>(m_size.y), static_cast<double>(m_size.z),
                m_addCollision ? 1 : 0);
}

bool BrushEditor::viewportInput(const glm::vec2& vpPos,
                                const glm::vec2& vpSize, bool hovered,
                                ImDrawList* dl) {
    const ImGuiIO& io = ImGui::GetIO();
    if (!hovered) return false;
    const glm::vec2 mouse(io.MousePos.x, io.MousePos.y);
    const Ray ray = PickingSystem::rayFromViewport(
        m_ctx.editorScene().camera(), vpPos, vpSize, mouse);

    // 落点 = 与 z=0 平面的交点（有地形时由视口先处理地形射线；这里只
    // 负责"空地上放刷子"）。用平面而不是拾取实体：拼关卡时你要的是
    // "在地面上放一个"，不是"贴在某个物体表面"。
    float t = 0.0f;
    if (!PickingSystem::intersectPlane(ray, glm::vec3(0.0f), glm::vec3(0, 0, 1),
                                       t) ||
        t <= 0.0f) {
        return false;
    }
    const glm::vec3 hit = ray.origin + ray.dir * t;

    if (dl) {
        glm::vec2 s;
        const glm::mat4 vp =
            PickingSystem::viewProj(m_ctx.editorScene().camera(), vpSize);
        if (PickingSystem::worldToScreen(vp, vpPos, vpSize, hit, s)) {
            const ImU32 col = IM_COL32(120, 210, 255, 230);
            dl->AddCircleFilled(ImVec2(s.x, s.y), 3.5f, col);
            dl->AddCircle(ImVec2(s.x, s.y), 14.0f, col, 32, 1.4f);
        }
    }

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
        placeAt(hit);
        return true;
    }
    return false;
}

void BrushEditor::drawPanel() {
    if (!beginToolkitWindow(m_ctx, "brushedit", "Brush Editing", 420.0f, 560.0f))
        return;
    ImGui::TextDisabled("%s", TR(editModeHint(EditMode::BrushEdit)));
    ImGui::Separator();

    ImGui::SeparatorText(TR("Shape"));
    const char* labels[3] = {"Box", "Cylinder", "Sphere"};
    for (int i = 0; i < 3; ++i) {
        if (i) ImGui::SameLine();
        const bool on = (m_shape == i);
        if (on) {
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        if (ImGui::Button(TR(labels[i]), ImVec2(126.0f, 26.0f))) m_shape = i;
        if (on) ImGui::PopStyleColor();
        char tag[32];
        std::snprintf(tag, sizeof(tag), "BR-SHAPE-%d", i);
        logRect(tag, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    }

    ImGui::SeparatorText(TR("Size (m)"));
    rowSlider(TR("Width (X)"), "##br_sx", &m_size.x, 0.1f, 200.0f, "%.2f m",
              true);
    rowSlider(TR("Depth (Y)"), "##br_sy", &m_size.y, 0.1f, 200.0f, "%.2f m",
              true);
    rowSlider(TR("Height (Z)"), "##br_sz", &m_size.z, 0.1f, 200.0f, "%.2f m",
              true);
    rowSlider(TR("Grid snap"), "##br_snap", &m_snap, 0.0f, 32.0f, "%.1f m");
    ImGui::TextDisabled("%s", TR("0 = no snapping"));
    ImGui::Checkbox(TR("Drop to the ground plane"), &m_dropToGround);
    ImGui::Checkbox(TR("Add a collider"), &m_addCollision);

    ImGui::Spacing();
    ImGui::Text(TR("Placed: %d"), m_placed);
    ImGui::Spacing();
    ImGui::TextDisabled("%s",
                        TR("Click in the viewport to stamp a brush on the "
                           "ground plane. Brushes are ordinary entities, so "
                           "W/E/R and the inspector work on them."));
    ImGui::End();
}

// ============================================================
// 5. 动画（实体 transform 关键帧）
// ============================================================
//
// 数据在 `ecs::AnimationClipComponent` 上，**不是本类的成员**。第一版把
// keys 放在 `m_keys` 里，于是切选中丢、关面板丢、存盘丢 —— 最后一条是
// 用户点出来的。挂到组件上之后：
//   · 序列化顺手带上（`je["animClip"]`，见 SceneSerializer）；
//   · undo 自动成立（`structuralEdit` 的整场景快照本来就包含它）；
//   · 切选中再切回来，轨道还在。
//
// 本类只做三件事：面板 UI、播放头推进、把当前时刻的插值写回 transform。
//
// ⚠ 与 `AnimatorComponent` 的分工见 Components.h：那个播**骨骼片段**
//   （导入的 clip / 状态机），这个动**实体自身的 TRS**。
namespace {

// 关键帧之间用 smoothstep 缓入缓出。线性插值在每个关键帧处速度突变，
// 看上去"顿一下"；Blender 的 Bezier 默认也是这个意图（这里不做切线，
// 只做时间重映射 —— 效果足够，而且不会过冲）。
float easeKey(float u) {
    u = std::clamp(u, 0.0f, 1.0f);
    return u * u * (3.0f - 2.0f * u);
}

void sampleInto(const ecs::TransformKey& a, const ecs::TransformKey& b, float u,
                ecs::TransformComponent& out) {
    const float k = easeKey(u);
    out.position = glm::mix(a.position, b.position, k);
    out.rotation = glm::mix(a.rotation, b.rotation, k);
    out.scale = glm::mix(a.scale, b.scale, k);
}

void keyRowLabel(char* buf, std::size_t n, int i,
                 const ecs::TransformKey& k) {
    // 格式串要过 TR()：这一行是**上屏的**（关键帧列表每一项），不是日志。
    // 只翻这里，`#%02d` / `%.2fs` / `pos(...)` 的排布原样保留。
    std::snprintf(buf, n, TR("#%02d   %.2fs   pos(%.2f, %.2f, %.2f)"), i,
                  static_cast<double>(k.t), static_cast<double>(k.position.x),
                  static_cast<double>(k.position.y),
                  static_cast<double>(k.position.z));
}

} // namespace

ecs::AnimationClipComponent* AnimationEditor::clip() {
    ecs::Entity e = m_ctx.selection();
    if (!e.valid()) return nullptr;
    ecs::World& w = m_ctx.editorScene().world();
    if (!w.alive(e)) return nullptr;
    return w.get<ecs::AnimationClipComponent>(e);
}

// 按需创建：**不要**在选中实体时就自动加组件 —— 光点一下物体就让它多一个
// 组件是很脏的行为（Inspector 里会突然冒出一段空轨道）。
ecs::AnimationClipComponent& AnimationEditor::ensureClip() {
    ecs::Entity e = m_ctx.selection();
    ecs::World& w = m_ctx.editorScene().world();
    if (auto* c = w.get<ecs::AnimationClipComponent>(e)) return *c;
    return w.add<ecs::AnimationClipComponent>(e);
}

void AnimationEditor::addKey() {
    ecs::Entity e = m_ctx.selection();
    if (!e.valid()) return;
    ecs::World& w = m_ctx.editorScene().world();
    auto* t = w.get<ecs::TransformComponent>(e);
    if (!t) return;
    m_entity = e;

    // 走 structuralEdit 而不是直接改组件：加一帧要能撤销，而撤销正是靠
    // "操作前后的整场景快照"实现的（animClip 现在在快照里，所以成立）。
    int total = 0;
    bool replaced = false;
    float keyTime = 0.0f;
    m_ctx.structuralEdit("Add keyframe", [&] {
        ecs::AnimationClipComponent& c = ensureClip();
        ecs::TransformKey k;
        k.t = c.time;  // 打在播放头位置，而不是"最后一帧之后"
        k.position = t->position;
        k.rotation = t->rotation;
        k.scale = t->scale;
        keyTime = k.t;
        // 同一时刻重复加点 = 覆盖（否则时间轴上会叠出一堆看不见的重影）
        for (auto& old : c.keys) {
            if (std::fabs(old.t - k.t) < 1e-3f) {
                old = k;
                replaced = true;
                break;
            }
        }
        if (!replaced) c.keys.push_back(k);
        std::sort(c.keys.begin(), c.keys.end(),
                  [](const ecs::TransformKey& a, const ecs::TransformKey& b) {
                      return a.t < b.t;
                  });
        // 时长要罩得住最后一帧，否则刚加的这帧永远播不到
        if (!c.keys.empty() && c.duration < c.keys.back().t)
            c.duration = c.keys.back().t;
        total = static_cast<int>(c.keys.size());
    });
    m_ctx.dirty() = true;
    VK_LOG_INFO("ANIM-KEY t=%.2f total=%d replaced=%d", static_cast<double>(keyTime),
                total, replaced ? 1 : 0);
}

void AnimationEditor::deleteNearestKey() {
    ecs::AnimationClipComponent* c = clip();
    if (!c || c->keys.empty()) return;

    int best = m_selectedKey;
    if (best < 0 || best >= static_cast<int>(c->keys.size())) {
        best = 0;
        float bd = 1e30f;
        for (int i = 0; i < static_cast<int>(c->keys.size()); ++i) {
            const float d = std::fabs(c->keys[i].t - c->time);
            if (d < bd) {
                bd = d;
                best = i;
            }
        }
    }
    const float removedT = c->keys[best].t;
    m_ctx.structuralEdit("Delete keyframe", [&] {
        ecs::AnimationClipComponent& cc = ensureClip();
        if (best >= 0 && best < static_cast<int>(cc.keys.size()))
            cc.keys.erase(cc.keys.begin() + best);
    });
    m_selectedKey = -1;
    m_ctx.dirty() = true;
    VK_LOG_INFO("ANIM-DELKEY t=%.2f total=%d", static_cast<double>(removedT),
                static_cast<int>(c->keys.size()));
}

void AnimationEditor::clearKeys() {
    ecs::AnimationClipComponent* c = clip();
    if (!c) return;
    m_ctx.structuralEdit("Clear keyframes", [&] {
        ecs::AnimationClipComponent& cc = ensureClip();
        cc.keys.clear();
        cc.time = 0.0f;
        cc.playing = false;
    });
    m_selectedKey = -1;
    m_ctx.dirty() = true;
    VK_LOG_INFO("ANIM-CLEAR");
}

void AnimationEditor::jumpToKey(int index) {
    ecs::AnimationClipComponent* c = clip();
    if (!c || index < 0 || index >= static_cast<int>(c->keys.size())) return;
    c->playing = false;
    c->time = c->keys[index].t;
    applyAt(c->time);
    VK_LOG_INFO("ANIM-GOTO t=%.2f", static_cast<double>(c->time));
}

void AnimationEditor::applyAt(float t) {
    ecs::AnimationClipComponent* c = clip();
    if (!c || c->keys.empty() || !m_entity.valid()) return;
    ecs::World& w = m_ctx.editorScene().world();
    if (!w.alive(m_entity)) return;
    auto* tr = w.get<ecs::TransformComponent>(m_entity);
    if (!tr) return;

    const ecs::TransformKey& first = c->keys.front();
    const ecs::TransformKey& last = c->keys.back();
    if (c->keys.size() == 1 || t <= first.t) {
        sampleInto(first, first, 0.0f, *tr);
    } else if (t >= last.t) {
        sampleInto(last, last, 0.0f, *tr);
    } else {
        for (std::size_t i = 0; i + 1 < c->keys.size(); ++i) {
            const ecs::TransformKey& a = c->keys[i];
            const ecs::TransformKey& b = c->keys[i + 1];
            if (t >= a.t && t <= b.t) {
                const float u = (b.t > a.t) ? ((t - a.t) / (b.t - a.t)) : 0.0f;
                sampleInto(a, b, u, *tr);
                break;
            }
        }
    }
    m_ctx.dirty() = true;
}

// 每帧（只在 Animation 模式下由 EditorApp 调用）
void AnimationEditor::sync() {
    ecs::Entity e = m_ctx.selection();
    if (e.id != m_entity.id) {
        // 换了选中项：播放头跟着新实体的轨道走，不要保留上一个物体的时间
        m_entity = e;
        m_selectedKey = -1;
    }
    if (!e.valid()) return;

    ecs::AnimationClipComponent* c = clip();
    if (!c || !c->playing) return;

    const float dt = std::min(ImGui::GetIO().DeltaTime, 0.1f);
    c->time += dt;
    if (c->time >= c->duration) {
        if (c->loop && c->duration > 1e-4f) {
            c->time = std::fmod(c->time, c->duration);
        } else {
            c->time = c->duration;
            c->playing = false;
            VK_LOG_INFO("ANIM-STOP t=%.2f", static_cast<double>(c->time));
        }
    }
    applyAt(c->time);
}

void AnimationEditor::drawPanel() {
    if (!beginToolkitWindow(m_ctx, "animation", "Animation", 480.0f, 620.0f)) return;
    ImGui::TextDisabled("%s", TR(editModeHint(EditMode::Animation)));
    ImGui::Separator();

    ecs::Entity e = m_ctx.selection();
    ecs::World& w = m_ctx.editorScene().world();
    if (!e.valid() || !w.alive(e) || !w.get<ecs::TransformComponent>(e)) {
        ImGui::TextWrapped("%s",
                           TR("Select an entity first. Keys record its "
                              "position / rotation / scale at the current "
                              "time and are saved with the scene."));
        ImGui::End();
        return;
    }
    if (m_entity.id != e.id) {
        m_entity = e;
        m_selectedKey = -1;
    }

    ecs::AnimationClipComponent* c = clip();
    if (!c) {
        ImGui::TextWrapped("%s",
                           TR("This entity has no transform track yet. Press "
                              "Add key to create one."));
        if (ImGui::Button(TR("Add key"), ImVec2(-1.0f, 30.0f))) addKey();
        logRect("AN-ADDKEY", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        ImGui::End();
        return;
    }

    ImGui::Text(TR("Keys: %d  ·  duration: %.1f s"),
                static_cast<int>(c->keys.size()),
                static_cast<double>(c->duration));

    // 时长滑条：不能短于最后一帧（短了那段就播不到，用户会以为丢帧了）
    float minDur = 0.5f;
    if (!c->keys.empty()) minDur = std::max(0.5f, c->keys.back().t);
    ImGui::TextUnformatted(TR("Duration"));
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderFloat("##an_dur", &c->duration, minDur, 60.0f,
                           TR("%.1f s"), ImGuiSliderFlags_Logarithmic))
        m_ctx.dirty() = true;

    // 时间轴：滑块 + 一行自绘刻度。ImGui 没有现成的"轨道"控件，滑条 +
    // 刻度带就够 —— 关键帧位置必须**看得出来**，否则用户不知道拖到哪儿
    // 才有内容。
    ImGui::TextUnformatted(TR("Time"));
    ImGui::SetNextItemWidth(-1.0f);
    // 显示格式串要过 TR：这是直接喂给 ImGui 的（不走 rowSlider），
    // `%.2f s` 会以英文单位上屏。
    if (ImGui::SliderFloat("##an_time", &c->time, 0.0f, c->duration,
                           TR("%.2f s"))) {
        // 手动拖时间 = 预览，不自动播放
        c->playing = false;
        applyAt(c->time);
    }
    logRect("AN-TIME", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    {
        const ImVec2 a = ImGui::GetItemRectMin();
        const ImVec2 b = ImGui::GetItemRectMax();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float y = b.y + 4.0f;
        dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), IM_COL32(90, 95, 105, 200),
                    1.0f);
        for (const auto& k : c->keys) {
            const float f = (c->duration > 1e-4f) ? (k.t / c->duration) : 0.0f;
            const float x = a.x + (b.x - a.x) * std::clamp(f, 0.0f, 1.0f);
            dl->AddLine(ImVec2(x, y - 5.0f), ImVec2(x, y + 5.0f),
                        IM_COL32(255, 190, 90, 240), 2.0f);
        }
        // 播放头
        const float pf =
            (c->duration > 1e-4f) ? (c->time / c->duration) : 0.0f;
        const float px = a.x + (b.x - a.x) * std::clamp(pf, 0.0f, 1.0f);
        dl->AddTriangleFilled(ImVec2(px, y - 9.0f), ImVec2(px - 4.0f, y - 15.0f),
                              ImVec2(px + 4.0f, y - 15.0f),
                              IM_COL32(120, 210, 255, 255));
        ImGui::Dummy(ImVec2(1.0f, 16.0f));
    }

    if (ImGui::Button(TR("Add key"), ImVec2(120.0f, 28.0f))) addKey();
    logRect("AN-ADDKEY", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::Button(c->playing ? TR("Pause") : TR("Play"),
                      ImVec2(120.0f, 28.0f))) {
        c->playing = !c->playing;
        if (c->playing && c->time >= c->duration - 1e-3f) c->time = 0.0f;
        VK_LOG_INFO("ANIM-%s t=%.2f", c->playing ? "PLAY" : "PAUSE",
                    static_cast<double>(c->time));
    }
    logRect("AN-PLAY", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::Button(TR("Clear"), ImVec2(110.0f, 28.0f))) clearKeys();
    logRect("AN-CLEAR", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    ImGui::Checkbox(TR("Loop"), &c->loop);
    ImGui::SameLine();
    ImGui::Checkbox(TR("Autoplay on scene load"), &c->autoplay);
    ImGui::SameLine();
    if (ImGui::Button(TR("Delete key"), ImVec2(120.0f, 24.0f)))
        deleteNearestKey();
    logRect("AN-DELKEY", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    // ---- 关键帧列表 ----
    // 时间轴上的刻度只能告诉"有几帧"，改不了具体某一帧。列表可以直接点
    // （选中）/ 双击（跳到那一刻）—— 这是序列编辑器最基本的一件事。
    ImGui::SeparatorText(TR("Keys"));
    if (ImGui::BeginChild("##an_keys", ImVec2(-1.0f, 150.0f), true)) {
        for (int i = 0; i < static_cast<int>(c->keys.size()); ++i) {
            char lbl[144];
            keyRowLabel(lbl, sizeof(lbl), i, c->keys[i]);
            const bool on = (m_selectedKey == i);
            if (ImGui::Selectable(lbl, on, ImGuiSelectableFlags_AllowDoubleClick)) {
                m_selectedKey = i;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    jumpToKey(i);
            }
        }
        if (c->keys.empty()) {
            ImGui::TextDisabled("%s", TR("No keys yet."));
        }
    }
    ImGui::EndChild();
    logRect("AN-LIST", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    ImGui::Separator();
    ImGui::TextDisabled("%s",
                        TR("Keys are stored on the entity and saved with the "
                           "scene. Move the entity with the viewport gizmo, "
                           "set the time, then press Add key."));
    ImGui::End();
}

// ============================================================
// 6. 程序化生成
// ============================================================
void PCGEditor::clearGenerated() {
    ecs::World& w = m_ctx.editorScene().world();
    // 只有本编辑器自己撒出来的东西才会挂在 m_root 底下，所以按"根的子节点"
    // 清是安全的 —— 不要去猜"哪些实体看起来像是我生成的"。
    //
    // ⚠ 用 `World::destroy` 而不是 `Scene::destroyObject`：Scene **没有**
    //   删除接口（它的职责是"世界矩阵/遍历/父子关系"，生命周期归 World）。
    //   销毁会把 generation 自增，旧句柄立刻失效，所以先取一份 id 列表再删
    //   是安全的 —— 循环途中不会踩到被换掉的句柄。
    if (m_root.valid() && w.alive(m_root)) {
        if (auto* h = w.get<ecs::HierarchyComponent>(m_root)) {
            const std::vector<ecs::EntityId> kids = h->children;
            for (ecs::EntityId id : kids) {
                const ecs::Entity child = w.handle(id);
                if (w.alive(child)) w.destroy(child);
            }
            // 子节点已销毁，父的白名单必须同步清空：`World::destroy` 不会
            // 回头去改**别人**的 HierarchyComponent，留着就是一堆失效 id
            // （渲染前遍历若直接解引用就会撞上已回收的槽位）。
            h->children.clear();
        }
    }
    m_generated = 0;
    VK_LOG_INFO("PCG-CLEAR");
}

void PCGEditor::generate() {
    clearGenerated();
    ecs::World& w = m_ctx.editorScene().world();
    // 开头也打一行：`PCG-GEN` 在**末尾**，一旦中途提前返回，日志里就只剩
    // `PCG-CLEAR` 而没有任何结论 —— 用户看到的是"点了生成，什么都没发生，
    // 也没报错"。有了这行，"清理了但没生成"和"根本没点"能分清。
    VK_LOG_INFO("PCG-BEGIN kind=%d count=%d area=%.0f", m_kind, m_count,
                static_cast<double>(m_area));
    // 输出网格 / 材质
    assets::Mesh* mesh = nullptr;
    assets::Material* mat = nullptr;
    const char* kindName = "cube";
    // 每种输出给一份"像样的"材质 + 一个兜底色。石头配岩石材质、草配草材质，
    // 找不到就走 resolveToolkitMaterial 的兜底链（理由见那里）。
    const char* matName = "materials/crate";
    glm::vec4 matTint(0.78f, 0.60f, 0.42f, 1.0f);
    switch (m_kind) {
        case 0:
            mesh = m_ctx.assets().cube(1.0f, "pcg/cube");
            kindName = "cube";
            matName = "materials/crate";
            matTint = glm::vec4(0.78f, 0.60f, 0.42f, 1.0f);
            break;
        case 1:
            mesh = m_ctx.assets().sphere(0.5f, "pcg/sphere");
            kindName = "sphere";
            matName = "materials/metal";
            matTint = glm::vec4(0.70f, 0.72f, 0.76f, 1.0f);
            break;
        case 2:
            mesh = m_ctx.assets().rock(1.4f, "pcg/rock", m_seed);
            kindName = "rock";
            matName = "materials/terrain_rock";
            matTint = glm::vec4(0.55f, 0.53f, 0.50f, 1.0f);
            break;
        default:
            mesh = m_ctx.assets().grass(0.6f, "pcg/grass");
            kindName = "grass";
            matName = "materials/terrain_grass";
            matTint = glm::vec4(0.42f, 0.62f, 0.30f, 1.0f);
            break;
    }
    mat = resolveToolkitMaterial(m_ctx, matName, matTint);
    if (!mesh) {
        // ⚠ 以前这里是 `if (!mesh) return;` —— 静默退出。PCG 已经先打了
        //   `PCG-CLEAR`（清掉上一次的输出），于是现场变成"旧实例没了、新的
        //   一个没撒、日志里也没有任何错误"，最难查的一类。
        VK_LOG_ERROR("PCG-GEN-FAIL 造不出网格 kind=%s", kindName);
        return;
    }
    if (!mat) {
        // resolveToolkitMaterial 连程序化兜底都造不出来的话才会走到这里。
        // 实体照样建得出来，只是颜色通道会被跳过（只有深度预通道画）——
        // 这是"看不见"的那一类，必须报 ERROR 而不是 WARN。
        VK_LOG_ERROR("PCG-GEN-FAIL 没有可用材质，实例将不可见");
        return;
    }

    if (!m_root.valid() || !w.alive(m_root)) {
        m_root = m_ctx.editorScene().createObject("PCG Output");
        if (auto* h = w.get<ecs::HierarchyComponent>(m_root)) h->group = true;
    }

    Rng rng(static_cast<std::uint32_t>(m_seed));
    int placed = 0, skipped = 0;
    m_ctx.structuralEdit("PCG generate", [&] {
        for (int i = 0; i < m_count; ++i) {
            // 抖动网格而不是纯随机：纯随机会出现"两株重叠 + 一大片空地"，
            // 而抖动网格在保持均匀的同时没有可见的格线。
            const int side = std::max(1, static_cast<int>(
                                            std::sqrt(static_cast<double>(m_count))));
            const float cell = m_area / static_cast<float>(side);
            const int gx = i % side;
            const int gy = i / side;
            const float wx = m_centerX + (static_cast<float>(gx) + rng.unit()) *
                                             cell - m_area * 0.5f;
            const float wy = m_centerY + (static_cast<float>(gy) + rng.unit()) *
                                             cell - m_area * 0.5f;

            // 坡度 / 高度过滤需要地形；没有地形就退化成"平地上撒"
            float wz = 0.0f;
            float slopeDeg = 0.0f;
            if (m_land.hasTerrain()) {
                wz = m_land.heightAtWorld(wx, wy);
                const glm::vec3 n = m_land.normalAtWorld(wx, wy);
                slopeDeg = glm::degrees(std::acos(
                    std::clamp(n.z, -1.0f, 1.0f)));
                float lo = 0.0f, hi = 1.0f;
                m_land.heightRangeWorld(lo, hi);
                const float t = (hi - lo > 1e-4f) ? (wz - lo) / (hi - lo) : 0.5f;
                if (t < m_minHeight || t > m_maxHeight) {
                    ++skipped;
                    continue;
                }
            }
            if (slopeDeg > m_maxSlopeDeg) {
                ++skipped;
                continue;
            }

            const float s = rng.range(m_minScale, m_maxScale);
            char name[48];
            std::snprintf(name, sizeof(name), "PCG %s %03d", kindName, ++m_generated);
            const ecs::Entity e = m_ctx.editorScene().createObject(name);
            if (auto* t = w.get<ecs::TransformComponent>(e)) {
                t->position = glm::vec3(wx, wy, wz);
                t->scale = glm::vec3(s);
                if (m_alignNormal && m_land.hasTerrain()) {
                    const glm::vec3 n = m_land.normalAtWorld(wx, wy);
                    t->rotation = glm::vec3(-std::asin(std::clamp(n.y, -1.0f, 1.0f)),
                                            std::asin(std::clamp(n.x, -1.0f, 1.0f)),
                                            rng.range(0.0f, 6.283f));
                } else {
                    t->rotation = glm::vec3(0.0f, 0.0f, rng.range(0.0f, 6.283f));
                }
            }
            ecs::MeshComponent mc;
            mc.mesh = mesh;
            w.add<ecs::MeshComponent>(e, mc);
            ecs::MaterialComponent matc;
            matc.material = mat;
            w.add<ecs::MaterialComponent>(e, matc);
            m_ctx.editorScene().setParent(e, m_root);
            ++placed;
        }
    });
    m_ctx.dirty() = true;
    VK_LOG_INFO("PCG-GEN kind=%s placed=%d skipped=%d area=%.0f seed=%d mat=%s",
                kindName, placed, skipped, static_cast<double>(m_area), m_seed,
                mat && mat->name.size() ? mat->name.c_str() : "-");
    // ⚠ 别把 `%d` 塞进 TR() 的原文 —— `TR()` 只是**查表替换**，不跑 printf，
    //   屏幕上会原样显示 "%d instances: 120" 这种字面占位符。
    //   要拼数字就把数字留在 TR 外面（译文拆成两截）。
    m_ctx.notify(std::string(TR("PCG placed ")) + std::to_string(placed) +
                 TR(" instances"));
}

void PCGEditor::drawPanel() {
    if (!beginToolkitWindow(m_ctx, "pcg", "PCG", 440.0f, 660.0f)) return;
    ImGui::TextDisabled("%s", TR(editModeHint(EditMode::PCG)));
    ImGui::Separator();

    // ---- 主动作放在**最上面**，不是最后 ----
    //
    // 参数有十来行，加起来比 660 px 的窗口高。原来 Generate 排在末尾，于是
    // 它被挤到可视区之外 —— 必须先在面板里滚下去才点得到。用户的原话是
    // "点了生成什么都没发生"，实际是"那个按钮根本没在屏幕上"（自动化点击
    // 也点不到：`logRect` 报的是 ImGui 算出的 item 矩形，被裁掉的行照样
    // 会有一份窗口外的坐标）。
    //
    // 放在最上面是 UE5 的习惯（面板第一行就是那个动作），也保证任何窗口
    // 高度下它都在第一屏。
    if (ImGui::Button(TR("Generate"), ImVec2(200.0f, 30.0f))) generate();
    logRect("PCG-GEN", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::Button(TR("Clear"), ImVec2(160.0f, 30.0f))) {
        clearGenerated();
        m_ctx.dirty() = true;
    }
    logRect("PCG-CLEAR", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::Text(TR("Instances in the scene: %d"), m_generated);
    ImGui::Separator();

    ImGui::SeparatorText(TR("Output"));
    const char* labels[4] = {"Cube", "Sphere", "Rock", "Grass"};
    for (int i = 0; i < 4; ++i) {
        if (i) ImGui::SameLine();
        const bool on = (m_kind == i);
        if (on) {
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        if (ImGui::Button(TR(labels[i]), ImVec2(96.0f, 26.0f))) m_kind = i;
        if (on) ImGui::PopStyleColor();
        char tag[32];
        std::snprintf(tag, sizeof(tag), "PCG-KIND-%d", i);
        logRect(tag, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    }

    ImGui::SeparatorText(TR("Sampling"));
    rowSliderInt(TR("Instances"), "##pcg_count", &m_count, 1, 2000);
    rowSlider(TR("Area (m)"), "##pcg_area", &m_area, 10.0f, 512.0f, "%.0f m");
    rowSlider(TR("Center X"), "##pcg_cx", &m_centerX, -256.0f, 256.0f, "%.0f m");
    rowSlider(TR("Center Y"), "##pcg_cy", &m_centerY, -256.0f, 256.0f, "%.0f m");
    ImGui::TextUnformatted(TR("Seed"));
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::DragInt("##pcg_seed", &m_seed, 1.0f, 0, 999999);

    ImGui::SeparatorText(TR("Rules"));
    rowSlider(TR("Min scale"), "##pcg_smin", &m_minScale, 0.1f, 4.0f, "%.2f");
    rowSlider(TR("Max scale"), "##pcg_smax", &m_maxScale, 0.1f, 8.0f, "%.2f");
    rowSlider(TR("Slope limit"), "##pcg_slope", &m_maxSlopeDeg, 0.0f, 89.0f,
              "%.0f deg");
    rowSlider(TR("Height band low"), "##pcg_hlo", &m_minHeight, 0.0f, 1.0f,
              "%.2f");
    rowSlider(TR("Height band high"), "##pcg_hhi", &m_maxHeight, 0.0f, 1.0f,
              "%.2f");
    ImGui::Checkbox(TR("Align to surface"), &m_alignNormal);
    if (!m_land.hasTerrain()) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "%s",
                           TR("No terrain in this scene: sampling falls back "
                              "to the flat z=0 plane."));
    }

    ImGui::Spacing();
    ImGui::TextDisabled("%s",
                        TR("Sampling is a jittered grid inside the area, then "
                           "filtered by slope and height band. Output is "
                           "grouped under a \"PCG Output\" folder entity."));
    ImGui::End();
}

} // namespace editor
