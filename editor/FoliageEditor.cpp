#include "FoliageEditor.h"

#include "DebugRects.h"
#include "Localization.h"
#include "PickingSystem.h"

#include "assets/AssetManager.h"
#include "core/Logger.h"
#include "scene/Scene.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace editor {

namespace {

// 一个场景里最多几株。为什么要有上限：每株是一个实体，渲染层每帧要给它
// 建 DrawItem、算世界矩阵、做视锥剔除。几千株还行，几万株就该上"真正的
// 实例化植被系统"了（W5）。到上限后打一行 WARN 并停止撒点 —— 静默停止
// 会让人以为"笔刷坏了"。
constexpr int kMaxInstances = 6000;

// 见 LandscapeEditor.cpp 的同名函数：ImGui 的标签画在控件右边，满宽控件
// 会把标签裁掉，所以标题单独一行。
void labeledSlider(const char* label, const char* id, float* v, float lo,
                   float hi, const char* fmt) {
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderFloat(id, v, lo, hi, fmt);
}

// 网格来源的 shape 名（"grass" / "tree" / "rock" …）。空 = 没有网格。
const std::string& meshShapeOf(const assets::Mesh* m) {
    static const std::string kEmpty;
    return m ? m->source().shape : kEmpty;
}

// 颜色变体：同一种植物的 5 档亮度/冷暖。
//
// 为什么不是随机 RGB：随机 RGB 会跑出紫的草和粉的树。这里只动**亮度**和
// 一点点冷暖偏移，得到的永远是"同一物种的不同株"。
glm::vec4 variantColor(const glm::vec4& base, int v) {
    static const float kBright[5] = {1.00f, 1.22f, 0.80f, 1.10f, 0.90f};
    static const float kWarm[5] = {0.00f, 0.06f, -0.05f, 0.03f, -0.03f};
    const int i = std::clamp(v, 0, 4);
    glm::vec4 c = base;
    c.x = std::clamp(base.x * kBright[i] + kWarm[i], 0.0f, 1.0f);
    c.y = std::clamp(base.y * kBright[i], 0.0f, 1.0f);
    c.z = std::clamp(base.z * kBright[i] - kWarm[i], 0.0f, 1.0f);
    return c;
}

} // namespace

const FoliageEditor::Kind& FoliageEditor::kind(int i) {
    static const Kind kKinds[kKindCount] = {
        // 草：一株 0.55 m 高，成片撒。密度高、缩放小。能长在较陡的坡上。
        {"Grass", "grass", "grass", 0.55f, glm::vec4(0.34f, 0.52f, 0.22f, 1.0f),
         0.85f, 26, 0.7f, 1.5f, 1.00f, 38.0f, 0.00f, 0.92f, 5},
        // 灌木：球压扁，当"树丛"。密度中等，怕陡坡。
        {"Bush", "bush", "sphere", 1.10f, glm::vec4(0.20f, 0.36f, 0.16f, 1.0f),
         0.90f, 5, 0.6f, 1.3f, 0.72f, 30.0f, 0.02f, 0.85f, 4},
        // 树：干 + 两层冠，5 m 基准高。密度最低，最怕陡坡。
        {"Tree", "tree", "tree", 5.00f, glm::vec4(0.22f, 0.34f, 0.19f, 1.0f),
         0.85f, 2, 0.7f, 1.5f, 1.00f, 26.0f, 0.00f, 0.78f, 4},
        // 花：细杆 + 花冠。成片点缀在草地里，颜色偏暖。
        {"Flower", "flower", "flower", 0.35f,
         glm::vec4(0.86f, 0.72f, 0.26f, 1.0f), 0.70f, 14, 0.7f, 1.3f, 1.00f,
         30.0f, 0.03f, 0.75f, 4},
        // 石头：哪都能待（坡度上限很高），灰褐色，海拔不限。
        {"Rock", "rock", "rock", 1.40f, glm::vec4(0.44f, 0.42f, 0.39f, 1.0f),
         0.75f, 4, 0.5f, 1.6f, 1.00f, 70.0f, 0.00f, 1.00f, 3},
    };
    return kKinds[std::clamp(i, 0, kKindCount - 1)];
}

float FoliageEditor::rnd() {
    // xorshift32：确定性。测试脚本连点两下拿到的分布是可复现的 ——
    // 用 std::rand 的话"这次撒出来好看、下次一团糟"就无从复现。
    uint32_t x = m_rng ? m_rng : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    m_rng = x;
    return static_cast<float>(x & 0x00FFFFFFu) / 16777216.0f;
}

assets::Mesh* FoliageEditor::meshFor(int k) {
    const Kind& K = kind(k);
    assets::AssetManager& am = m_ctx.assets();
    char key[64];
    std::snprintf(key, sizeof(key), "foliage/mesh/%s", K.id);
    if (std::strcmp(K.shape, "grass") == 0) return am.grass(K.size, key);
    if (std::strcmp(K.shape, "tree") == 0) return am.tree(K.size, key);
    if (std::strcmp(K.shape, "flower") == 0) return am.flower(K.size, key);
    if (std::strcmp(K.shape, "rock") == 0) return am.rock(K.size, key);
    // 灌木：低分段球 —— 8×5 已经能看出圆，且只有 ~80 个三角形
    return am.sphere(K.size * 0.5f, key, 8, 5);
}

assets::Material* FoliageEditor::materialFor(int k, int variant) {
    const Kind& K = kind(k);
    assets::AssetManager& am = m_ctx.assets();
    char key[64];
    const int v = std::clamp(variant, 0, std::max(K.variants - 1, 0));
    std::snprintf(key, sizeof(key), "foliage/mat/%s/%d", K.id, v);
    // doubleSided = true 是**必须**的：草是交叉薄片、树冠是开口锥体、
    // 花冠是单三角形，单面渲染时从背后看会整片消失（而阴影还在，
    // 看起来像"影子有草"）。
    return am.makeMaterialPBR(key, nullptr, nullptr, nullptr,
                              variantColor(K.color, v), K.roughness, 0.0f,
                              glm::vec3(0.0f), true);
}

// ============================================================
// 同步 / 统计
// ============================================================
ecs::Entity FoliageEditor::ensureRoot() {
    scene::Scene& sc = m_ctx.editorScene();
    ecs::World& w = sc.world();

    if (m_root.valid() && w.alive(m_root)) return m_root;

    ecs::Entity found{};
    w.each<ecs::NameComponent>([&](ecs::Entity e, ecs::NameComponent& nc) {
        if (found.valid()) return;
        if (nc.name != m_rootName) return;
        if (!w.has<ecs::HierarchyComponent>(e)) return;
        found = e;
    });
    if (found.valid()) {
        m_root = found;
        return m_root;
    }

    m_root = sc.createObject(m_rootName);
    if (auto* h = w.get<ecs::HierarchyComponent>(m_root)) h->group = true;
    VK_LOG_INFO("FOLIAGE-ROOT created entity=%u", m_root.id);
    return m_root;
}

void FoliageEditor::recount(bool logCounts) {
    m_total = 0;
    for (int i = 0; i < kKindCount; ++i) m_counts[i] = 0;

    scene::Scene& sc = m_ctx.editorScene();
    ecs::World& w = sc.world();
    if (!m_root.valid() || !w.alive(m_root)) {
        m_root = ecs::Entity{};
        if (logCounts && m_lastLoggedTotal != 0) {
            m_lastLoggedTotal = 0;
            VK_LOG_INFO("FOLIAGE-COUNT total=0");
        }
        return;
    }
    const auto* h = w.get<ecs::HierarchyComponent>(m_root);
    if (!h) return;

    for (ecs::EntityId id : h->children) {
        const ecs::Entity child = w.handle(id);
        if (!child.valid() || !w.alive(child)) continue;
        ++m_total;
        const auto* mc = w.get<ecs::MeshComponent>(child);
        const std::string& shape = meshShapeOf(mc ? mc->mesh : nullptr);
        // 顺序要对上 Kind 表：grass / bush(sphere) / tree / flower / rock
        if (shape == "grass") ++m_counts[0];
        else if (shape == "sphere") ++m_counts[1];
        else if (shape == "tree") ++m_counts[2];
        else if (shape == "flower") ++m_counts[3];
        else if (shape == "rock") ++m_counts[4];
    }

    if (logCounts && m_total != m_lastLoggedTotal) {
        m_lastLoggedTotal = m_total;
        VK_LOG_INFO("FOLIAGE-COUNT total=%d grass=%d bush=%d tree=%d "
                    "flower=%d rock=%d",
                    m_total, m_counts[0], m_counts[1], m_counts[2],
                    m_counts[3], m_counts[4]);
    }
}

void FoliageEditor::sync() {
    if (!m_root.valid() || !m_ctx.editorScene().world().alive(m_root)) {
        ensureRoot();
    }
    recount(true);
}

// ============================================================
// 撒点 / 擦除
// ============================================================
void FoliageEditor::spawn(int k, const glm::vec3& pos,
                          const glm::vec3& normal, float scale, float yaw,
                          int variant, float lateral) {
    scene::Scene& sc = m_ctx.editorScene();
    ecs::World& w = sc.world();

    const Kind& K = kind(k);
    char name[48];
    std::snprintf(name, sizeof(name), "%s %03d", K.name, ++m_spawnSeq);

    const ecs::Entity e = sc.createObject(name);
    if (auto* t = w.get<ecs::TransformComponent>(e)) {
        t->position = pos;
        // 横向再抖一点：同一种网格 + 同一种缩放 = 一眼看出复制粘贴
        t->scale = glm::vec3(scale * lateral, scale * K.squashY, scale * lateral);

        // 旋转（欧拉顺序 X→Y→Z，见 TransformComponent::matrix）。
        // 贴合法线的近似：绕 X 转 -asin(ny)、绕 Y 转 asin(nx)。地形坡度在
        // ±40° 以内时与"从 up 转到 n 的四元数"肉眼无差。
        float rx = 0.0f, ry = 0.0f, rz = yaw;
        if (m_alignNormal) {
            rx = -std::asin(std::clamp(normal.y, -1.0f, 1.0f));
            ry = std::asin(std::clamp(normal.x, -1.0f, 1.0f));
        }
        // 随机倾斜：完全对齐法线的草看起来像列队站好。
        const float jitter = m_tiltJitter * 0.24f;   // 1.0 → ±14°
        if (jitter > 0.0f) {
            rx += (rnd() * 2.0f - 1.0f) * jitter;
            ry += (rnd() * 2.0f - 1.0f) * jitter;
        }
        // 石头随便躺：只压扁不旋转的话，一堆石头是同一个朝向的"饼"。
        if (std::strcmp(K.shape, "rock") == 0) {
            rx += (rnd() * 2.0f - 1.0f) * 0.6f;
            rz += (rnd() * 2.0f - 1.0f) * 3.14159265f;
        }
        t->rotation = glm::vec3(rx, ry, rz);
    }

    ecs::MeshComponent mc;
    mc.mesh = meshFor(k);
    w.add<ecs::MeshComponent>(e, mc);

    ecs::MaterialComponent matc;
    matc.material = materialFor(k, variant);
    w.add<ecs::MaterialComponent>(e, matc);

    sc.setParent(e, m_root);
}

void FoliageEditor::eraseAt(const glm::vec3& worldHit, bool anyKind) {
    scene::Scene& sc = m_ctx.editorScene();
    ecs::World& w = sc.world();
    if (!m_root.valid() || !w.alive(m_root)) return;

    auto* h = w.get<ecs::HierarchyComponent>(m_root);
    if (!h) return;
    // 拷贝一份 id 再删：destroy 会改 children 列表
    const std::vector<ecs::EntityId> kids = h->children;
    const float r2 = m_radius * m_radius;
    const char* onlyShape = kind(m_kind).shape;
    int removed = 0;

    for (ecs::EntityId id : kids) {
        const ecs::Entity child = w.handle(id);
        if (!child.valid() || !w.alive(child)) continue;
        if (!anyKind) {
            const auto* mc = w.get<ecs::MeshComponent>(child);
            // 只删**当前选中的种类**：撒了一片草地之后想把树擦掉，结果
            // 连带把草也铲了，是最容易让人骂人的交互。
            if (meshShapeOf(mc ? mc->mesh : nullptr) != onlyShape) continue;
        }
        const glm::vec3 p(sc.worldMatrix(child)[3]);
        const float dx = p.x - worldHit.x;
        const float dy = p.y - worldHit.y;
        if (dx * dx + dy * dy > r2) continue;

        // 先从父节点的 children 里摘掉，避免留下悬空 id
        if (auto* ch = w.get<ecs::HierarchyComponent>(child)) {
            ch->parent = ecs::kInvalidEntity;
        }
        w.destroy(child);
        ++removed;
    }

    if (removed > 0) {
        // children 里可能还有已销毁的 id（上面的 destroy 不会回改父列表），
        // 这里重建一遍干净的列表
        if (auto* ph = w.get<ecs::HierarchyComponent>(m_root)) {
            auto& kids2 = ph->children;
            kids2.erase(std::remove_if(kids2.begin(), kids2.end(),
                                       [&](ecs::EntityId cid) {
                                           return !w.alive(w.handle(cid));
                                       }),
                        kids2.end());
        }
        m_ctx.dirty() = true;
        recount(false);
        VK_LOG_INFO("FOLIAGE-ERASE removed=%d anyKind=%d total=%d", removed,
                    anyKind ? 1 : 0, m_total);
    }
}

void FoliageEditor::clearAll(int kindIndex) {
    scene::Scene& sc = m_ctx.editorScene();
    ecs::World& w = sc.world();
    if (!m_root.valid() || !w.alive(m_root)) return;
    auto* h = w.get<ecs::HierarchyComponent>(m_root);
    if (!h) return;

    const std::vector<ecs::EntityId> kids = h->children;
    const char* only = (kindIndex >= 0) ? kind(kindIndex).shape : nullptr;
    int removed = 0;
    for (ecs::EntityId id : kids) {
        const ecs::Entity child = w.handle(id);
        if (!child.valid() || !w.alive(child)) continue;
        if (only) {
            const auto* mc = w.get<ecs::MeshComponent>(child);
            if (meshShapeOf(mc ? mc->mesh : nullptr) != only) continue;
        }
        if (auto* ch = w.get<ecs::HierarchyComponent>(child)) {
            ch->parent = ecs::kInvalidEntity;
        }
        w.destroy(child);
        ++removed;
    }
    if (auto* ph = w.get<ecs::HierarchyComponent>(m_root)) {
        auto& kids2 = ph->children;
        kids2.erase(std::remove_if(kids2.begin(), kids2.end(),
                                   [&](ecs::EntityId cid) {
                                       return !w.alive(w.handle(cid));
                                   }),
                    kids2.end());
    }
    recount(false);
    m_ctx.dirty() = true;
    VK_LOG_INFO("FOLIAGE-CLEAR kind=%d removed=%d total=%d", kindIndex,
                removed, m_total);
    m_ctx.notify(std::string(TR("Removed ")) + std::to_string(removed) +
                 TR(" foliage instances"));
}

void FoliageEditor::stamp(const glm::vec3& worldHit, bool erase) {
    if (!m_land.hasTerrain()) return;
    if (erase) {
        eraseAt(worldHit, m_eraseAllKinds);
        return;
    }

    if (m_total >= kMaxInstances) {
        VK_LOG_WARN("FOLIAGE-CAP total=%d limit=%d (stop scattering)", m_total,
                    kMaxInstances);
        return;
    }

    const Kind& K = kind(m_kind);
    // 地形高度带：用于"雪线以上不长树"这类过滤。每次落笔算一次。
    float hLo = 0.0f, hHi = 1.0f;
    if (m_filterHeight) m_land.heightRangeWorld(hLo, hHi);
    const float hSpan = std::max(hHi - hLo, 1.0e-4f);

    // 一次落笔撒 N 株。落点用**极坐标均匀采样**（r = R·sqrt(u)）而不是
    // 在方框里拒绝采样 —— 后者在圆边缘会稀疏，视觉上像一圈空边。
    //
    // edgeFalloff > 0 时再压一次半径分布：把点往圆心拉，笔刷边缘自然变稀。
    // 硬边的圆盘在草地上是一个**能看出来的圆**，那是最出戏的细节。
    const int n = std::max(1, m_density);
    const float minDist =
        0.45f * m_radius / std::sqrt(static_cast<float>(n) + 1.0f);
    std::vector<glm::vec2> placed;
    placed.reserve(static_cast<std::size_t>(n));

    int skippedSlope = 0;
    int skippedHeight = 0;
    int placedCount = 0;

    for (int i = 0; i < n; ++i) {
        if (m_total + placedCount >= kMaxInstances) break;

        const float u = rnd();
        const float rNorm = std::pow(u, 0.5f + m_edgeFalloff * 1.1f);
        const float r = m_radius * rNorm;
        const float a = 6.2831853072f * rnd();
        const glm::vec2 off(std::cos(a) * r, std::sin(a) * r);
        bool tooClose = false;
        for (const glm::vec2& p : placed) {
            if (glm::length(p - off) < minDist) { tooClose = true; break; }
        }
        if (tooClose) continue;
        placed.push_back(off);

        const float wx = worldHit.x + off.x;
        const float wy = worldHit.y + off.y;
        const float wz = m_land.heightAtWorld(wx, wy);
        const glm::vec3 n = m_land.normalAtWorld(wx, wy);

        // ---- 坡度过滤 ----
        if (m_filterSlope) {
            const float cosT = std::clamp(n.z, -1.0f, 1.0f);
            const float slopeDeg = std::acos(cosT) * 57.2957795f;
            if (slopeDeg > K.maxSlopeDeg) {
                ++skippedSlope;
                continue;
            }
        }
        // ---- 高度带过滤 ----
        if (m_filterHeight) {
            const float t = (wz - hLo) / hSpan;
            if (t < K.minH || t > K.maxH) {
                ++skippedHeight;
                continue;
            }
        }

        const float scale = m_minScale + (m_maxScale - m_minScale) * rnd();
        const float yaw = 6.2831853072f * rnd();
        const float lateral = 0.88f + 0.24f * rnd();
        const int variant = static_cast<int>(rnd() * static_cast<float>(K.variants));
        // 埋进地面一点：球的底是圆的，不留余量会看到"悬空的球"。
        // 圆球类（灌木/石头）埋得深，竖直生长的（草/花/树）几乎不埋。
        const float sink = (std::strcmp(K.shape, "sphere") == 0 ||
                            std::strcmp(K.shape, "rock") == 0)
                               ? scale * K.size * 0.30f
                               : scale * K.size * 0.02f;
        spawn(m_kind, glm::vec3(wx, wy, wz - sink), n, scale, yaw, variant,
              lateral);
        ++placedCount;
    }

    recount(false);
    m_ctx.dirty() = true;
    VK_LOG_INFO("FOLIAGE-SPAWN kind=%s placed=%d skippedSlope=%d "
                "skippedHeight=%d total=%d",
                K.id, placedCount, skippedSlope, skippedHeight, m_total);
}

// ============================================================
// 视口输入
// ============================================================
bool FoliageEditor::viewportInput(const glm::vec2& vpPos,
                                  const glm::vec2& vpSize, bool hovered,
                                  ImDrawList* dl) {
    sync();
    if (!m_land.hasTerrain() || !m_ctx.isEditing()) return false;

    const ImGuiIO& io = ImGui::GetIO();
    const glm::vec2 mouse(io.MousePos.x, io.MousePos.y);
    const Ray ray =
        PickingSystem::rayFromViewport(m_ctx.editorScene().camera(), vpPos,
                                       vpSize, mouse);
    glm::vec3 hit(0.0f), nrm(0.0f);
    const bool onTerrain =
        (hovered || m_stroking) && m_land.raycast(ray.origin, ray.dir, hit, nrm);

    // 光标：地上的圆环（与地形笔刷同一个观感，颜色区分开）。
    // 内圈 = 密度衰减后的"实心区"，一眼能看出笔刷边缘会不会稀疏。
    if (onTerrain && dl) {
        const glm::mat4 vp = PickingSystem::viewProj(
            m_ctx.editorScene().camera(), vpSize);
        auto projectRing = [&](float radiusScale, ImVec2* pts) -> bool {
            for (int i = 0; i <= 32; ++i) {
                const float a = 6.2831853072f * static_cast<float>(i) / 32.0f;
                const float rr = m_radius * radiusScale;
                const float wx = hit.x + std::cos(a) * rr;
                const float wy = hit.y + std::sin(a) * rr;
                const glm::vec3 wp(wx, wy, m_land.heightAtWorld(wx, wy) + 0.05f);
                const glm::vec4 clip = vp * glm::vec4(wp, 1.0f);
                if (clip.w <= 1.0e-5f) return false;
                const glm::vec3 ndc = glm::vec3(clip) / clip.w;
                pts[i] = ImVec2(vpPos.x + (ndc.x * 0.5f + 0.5f) * vpSize.x,
                                vpPos.y + (ndc.y * 0.5f + 0.5f) * vpSize.y);
            }
            return true;
        };

        const bool erasing = m_eraseMode || io.KeyShift;
        const glm::vec4 kc = kind(m_kind).color;
        ImU32 col = erasing
                        ? IM_COL32(255, 105, 100, 230)
                        : IM_COL32(static_cast<int>(std::min(kc.x * 255.0f + 80.0f, 255.0f)),
                                   static_cast<int>(std::min(kc.y * 255.0f + 80.0f, 255.0f)),
                                   static_cast<int>(std::min(kc.z * 255.0f + 60.0f, 255.0f)),
                                   230);
        ImU32 fill = (col & 0x00FFFFFFu) | 0x1E000000u;

        ImVec2 outer[33];
        if (projectRing(1.0f, outer)) {
            dl->AddConcavePolyFilled(outer, 32, fill);
            dl->AddPolyline(outer, 33, col, 0, 1.8f);
            if (m_edgeFalloff > 0.02f && !erasing) {
                ImVec2 inner[33];
                const float innerScale = std::max(1.0f - m_edgeFalloff * 0.8f, 0.2f);
                if (projectRing(innerScale, inner)) {
                    dl->AddPolyline(inner, 33, (col & 0x00FFFFFFu) | 0x80000000u,
                                    0, 1.0f);
                }
            }
            dl->AddCircleFilled(outer[0], 2.5f, col);
        }
    }

    const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    const bool navGesture = ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
                            ImGui::IsMouseDown(ImGuiMouseButton_Middle);

    // 滚轮调半径（与地形笔刷同一套操作；Shift = 每笔株数）。
    // 为什么不用 `[` / `]`：OEM 键在某些键盘布局下 ImGui 收不到，
    // 滚轮在任何布局下都到得了。
    bool wheelConsumed = false;
    if (onTerrain && hovered && !m_stroking && !navGesture &&
        !ImGui::GetIO().KeyCtrl && io.MouseWheel != 0.0f) {
        const float n = io.MouseWheel;
        if (io.KeyShift) {
            m_density = std::clamp(m_density + static_cast<int>(n * 2.0f), 1, 200);
            VK_LOG_INFO("FO-BRUSH density=%d", m_density);
        } else {
            m_radius = std::clamp(m_radius * std::pow(1.12f, n), 1.0f, 120.0f);
            VK_LOG_INFO("FO-BRUSH radius=%.2f", static_cast<double>(m_radius));
        }
        wheelConsumed = true;
    }

    if (clicked && hovered && onTerrain && !navGesture) {
        m_stroking = true;
        m_lastStamp = hit;
        stamp(hit, m_eraseMode || io.KeyShift);
    }

    bool consumed = false;
    if (m_stroking) {
        consumed = true;
        if (released) {
            m_stroking = false;
            VK_LOG_INFO("FOLIAGE-STROKE-END total=%d", m_total);
        } else if (down && onTerrain && !navGesture) {
            // 拖着撒：走出 1/4 个笔刷直径才落一次，否则一次拖动就是几千株
            const float step = std::max(m_radius * 0.25f, 0.5f);
            if (glm::length(glm::vec2(hit.x - m_lastStamp.x,
                                      hit.y - m_lastStamp.y)) >= step) {
                m_lastStamp = hit;
                stamp(hit, m_eraseMode || io.KeyShift);
            }
        }
    }
    return consumed || wheelConsumed || (onTerrain && hovered);
}

// ============================================================
// 面板
// ============================================================
void FoliageEditor::drawPanel() {
    bool& open = panelOpenRef("foliage");
    if (!open) return;

    sync();

    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSizeConstraints(ImVec2(380.0f, 360.0f),
                                        ImVec2(FLT_MAX, FLT_MAX));
    ImGui::SetNextWindowSize(ImVec2(420.0f, 680.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.32f,
                                   io.DisplaySize.y * 0.10f),
                            ImGuiCond_FirstUseEver);
    // ID 必须是 `###panel_<面板表 id>`，理由见 LandscapeEditor::drawPanel
    if (!ImGui::Begin(TR("Foliage###panel_foliage"), &open)) {
        ImGui::End();
        return;
    }
    logRect("FO-PANEL", ImGui::GetWindowPos(),
            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                   ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    ImGui::TextDisabled("%s", TR(editModeHint(EditMode::Foliage)));
    ImGui::Separator();

    if (!m_land.hasTerrain()) {
        ImGui::TextWrapped("%s",
                           TR("Foliage is scattered on a terrain. Create one "
                              "in Landscape mode first."));
        if (ImGui::Button(TR("Go to Landscape mode"), ImVec2(-1.0f, 30.0f))) {
            m_ctx.setEditMode(EditMode::Landscape);
        }
        logRect("FO-GO-LANDSCAPE", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());
        ImGui::End();
        return;
    }

    drawPalette();
    ImGui::Separator();
    drawBrush();
    ImGui::Separator();
    drawFilters();
    ImGui::Separator();
    drawStats();

    ImGui::End();
}

void FoliageEditor::drawPalette() {
    ImGui::SeparatorText(TR("Plants"));
    // 5 种按 2 列排（最后一行只放一个），比竖排 5 行省一半高度。
    for (int i = 0; i < kKindCount; ++i) {
        const Kind& K = kind(i);
        if (i % 2 == 1) ImGui::SameLine();
        const bool on = (m_kind == i);
        if (on) {
            ImGui::PushStyleColor(
                ImGuiCol_Button,
                ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        char label[96];
        std::snprintf(label, sizeof(label), "%s  (%d)", TR(K.name), m_counts[i]);
        if (ImGui::Button(label, ImVec2(194.0f, 28.0f))) {
            m_kind = i;
            m_density = K.density;
            m_minScale = K.minScale;
            m_maxScale = K.maxScale;
            VK_LOG_INFO("FOLIAGE-KIND %s", K.id);
        }
        if (on) ImGui::PopStyleColor();
        char tag[48];
        std::snprintf(tag, sizeof(tag), "FO-KIND-%d", i);
        logRect(tag, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    }
}

void FoliageEditor::drawBrush() {
    ImGui::SeparatorText(TR("Brush"));

    labeledSlider(TR("Radius"), "##fo_radius", &m_radius, 1.0f, 80.0f, "%.1f m");
    ImGui::TextUnformatted(TR("Per stamp"));
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderInt("##fo_density", &m_density, 1, 200);
    ImGui::TextUnformatted(TR("Scale range"));
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::DragFloatRange2("##fo_scale", &m_minScale, &m_maxScale, 0.02f, 0.1f,
                           3.0f, "%.2f", "%.2f");
    labeledSlider(TR("Random tilt"), "##fo_tilt", &m_tiltJitter, 0.0f, 1.0f,
                  "%.2f");
    labeledSlider(TR("Edge density falloff"), "##fo_edge", &m_edgeFalloff, 0.0f,
                  1.0f, "%.2f");

    ImGui::Checkbox(TR("Align to slope"), &m_alignNormal);
    ImGui::SameLine();
    ImGui::Checkbox(TR("Erase"), &m_eraseMode);
    if (m_eraseMode) {
        ImGui::Checkbox(TR("Erase all kinds"), &m_eraseAllKinds);
    }

    if (ImGui::Button(TR("Clear this type"), ImVec2(194.0f, 26.0f))) {
        clearAll(m_kind);
    }
    logRect("FO-CLEAR-TYPE", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::Button(TR("Clear all"), ImVec2(194.0f, 26.0f))) {
        clearAll(-1);
    }
    logRect("FO-CLEAR-ALL", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    ImGui::Spacing();
    ImGui::TextDisabled("%s",
                        TR("Left-click / drag on the terrain to scatter. "
                           "Hold Shift (or tick Erase) to remove. Right-drag "
                           "moves the camera. Scroll = radius."));
}

void FoliageEditor::drawFilters() {
    ImGui::SeparatorText(TR("Placement rules"));
    const Kind& K = kind(m_kind);

    ImGui::Checkbox(TR("Slope limit"), &m_filterSlope);
    ImGui::SameLine();
    ImGui::TextDisabled(TR("%.0f deg max for %s"), static_cast<double>(K.maxSlopeDeg),
                        TR(K.name));

    ImGui::Checkbox(TR("Height band"), &m_filterHeight);
    ImGui::SameLine();
    ImGui::TextDisabled(TR("%.0f%% .. %.0f%% of the terrain height"),
                        static_cast<double>(K.minH * 100.0f),
                        static_cast<double>(K.maxH * 100.0f));

    if (m_filterSlope || m_filterHeight) {
        ImGui::TextWrapped("%s",
                           TR("Stamped plants that fall outside the rules are "
                              "skipped, so steep cliffs and snowy peaks stay "
                              "bare - that is what makes the distribution "
                              "read as natural."));
    }
}

void FoliageEditor::drawStats() {
    ImGui::Text(TR("Instances: %d  (grass %d · bush %d · tree %d · flower %d "
                   "· rock %d)"),
                m_total, m_counts[0], m_counts[1], m_counts[2], m_counts[3],
                m_counts[4]);
    if (m_total >= kMaxInstances) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s",
                           TR("Instance cap reached - clear some first."));
    }
    ImGui::TextDisabled("%s",
                        TR("Instances live under the 'Foliage' group in the "
                           "Hierarchy, so they save with the scene."));
}

} // namespace editor
