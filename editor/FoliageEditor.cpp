#include "FoliageEditor.h"

#include "DebugRects.h"
#include "EditorDragDrop.h"
#include "Localization.h"
#include "ModeRail.h"
#include "PanelLayout.h"

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

// ---- 类型列表里的"眼睛"开关 ----
// 为什么自绘而不是用文字：ImGui 自带字体里没有眼睛/U+F06E 这类图标，
// 而 `[x]` 这种字符画在 26px 行里既不齐也不好点。三个圆/两段线就够了。
bool eyeButton(const char* id, bool on, float size) {
    ImGui::PushID(id);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool hit = ImGui::InvisibleButton("eye", ImVec2(size, size));
    const bool hov = ImGui::IsItemHovered();
    ImGui::PopID();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p.x + size * 0.5f, p.y + size * 0.5f);
    const ImU32 col = ImGui::GetColorU32(
        hov ? ImGuiCol_Text
            : (on ? ImGuiCol_Text : ImGuiCol_TextDisabled));
    const float rx = size * 0.42f;
    const float ry = size * 0.28f;
    // 眼形：两段对称的圆弧（用 polyline 画一个杏仁形）
    ImVec2 pts[13];
    for (int i = 0; i <= 6; ++i) {
        const float t = static_cast<float>(i) / 6.0f;
        const float a = 3.14159265f * t;
        pts[i] = ImVec2(c.x + std::cos(a) * rx, c.y - std::sin(a) * ry);
    }
    // ⚠ 下半弧只补 6 个点（pts[7..12]）：上面的循环已经把 pts[6] 写成
    //   左端点了，再补 7 个就会写到 pts[13] —— 数组只有 13 个元素，
    //   而 MSVC 的 /RTC1（Debug）会当场弹 "Stack around the variable 'pts'
    //   was corrupted" 断言框，**整个编辑器冻在那里**，日志停在上一行。
    //   表现是"切到 Foliage 之后面板一片空白、连 ERROR 都没有"。
    for (int i = 0; i < 6; ++i) {
        const float t = static_cast<float>(i) / 6.0f;
        const float a = 3.14159265f * t;
        pts[7 + i] = ImVec2(c.x - std::cos(a) * rx, c.y + std::sin(a) * ry);
    }
    dl->AddPolyline(pts, 13, col, 0, 1.3f);
    if (on) {
        dl->AddCircleFilled(c, size * 0.16f, col, 12);
    } else {
        // 关掉时画一道斜杠：比"瞳孔没了"明确得多（空眼眶像是画坏了）
        dl->AddLine(ImVec2(p.x + 4.0f, p.y + size - 4.0f),
                    ImVec2(p.x + size - 4.0f, p.y + 4.0f), col, 1.6f);
    }
    return hit;
}

} // namespace

const char* FoliageEditor::toolId(Tool t) {
    switch (t) {
        case Tool::Paint:  return "paint";
        case Tool::Erase:  return "erase";
        case Tool::Single: return "single";
        case Tool::Select: return "select";
    }
    return "paint";
}

const FoliageEditor::Kind& FoliageEditor::kind(int i) {
    static const Kind kKinds[kKindCount] = {
        // 草：一株 0.55 m 高，成片撒。密度高、缩放小。能长在较陡的坡上。
        {"Grass", "grass", "grass", 0.55f, glm::vec4(0.34f, 0.52f, 0.22f, 1.0f),
         0.85f, 26, 0.7f, 1.5f, 1.00f, 38.0f, 0.00f, 0.92f, 5},
        // 灌木：4 团位移叶簇 + 8 张叶卡（2026-10-28 起不再借压扁球）。
        // ⚠ shape 从 "sphere" 改成了 "bush"：旧场景里的灌木还是 "sphere"，
        //   统计/擦除/Reapply 都走 matchesKind()，两条都认。
        {"Bush", "bush", "bush", 1.10f, glm::vec4(0.20f, 0.36f, 0.16f, 1.0f),
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

void FoliageEditor::resetTypes() {
    for (int i = 0; i < kKindCount; ++i) {
        const Kind& K = kind(i);
        TypeSettings& t = m_types[i];
        t = TypeSettings{};
        t.visible = true;
        t.density = K.density;
        t.minScale = K.minScale;
        t.maxScale = K.maxScale;
        t.tilt = 0.35f;
        t.edge = 0.15f;
        t.align = true;
        t.slopeDeg = K.maxSlopeDeg;
        t.minH = K.minH;
        t.maxH = K.maxH;
        t.variants = K.variants;
        t.sizeScale = 1.0f;
        m_visApplied[i] = true;
        // 网格缓存置成"还没解析过"的哨兵值：空串是**合法**路径（= 内置），
        // 用它当哨兵会让首次解析被判成"没变化"→ `FOLIAGE-TYPE-MESH` 那行
        // 永远打不出来（而它正是"内置网格现在多少三角"的唯一判据）。
        m_typeMesh[i] = nullptr;
        m_typeMeshPath[i] = "\x01";
    }
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
    // 灌木：2026-10-28 起是专门的网格（4 团位移叶簇 + 8 张叶卡），
    // 不再是"借一颗 8×5 压扁球"—— 那样远看就是一个绿球。
    return am.bush(K.size, key);
}

// ============================================================
// 自定义网格（用户从内容浏览器拖进来的模型）
// ============================================================
assets::Mesh* FoliageEditor::loadCustomMesh(const std::string& relPath) {
    if (relPath.empty()) return nullptr;
    assets::AssetManager& am = m_ctx.assets();
    try {
        const std::string resolved = assets::resolveAssetPath(relPath);
        // .obj：单网格，直接拿 Mesh
        if (assets::isObjPath(relPath)) {
            assets::Mesh* m = am.loadModel(resolved, relPath);
            if (!m || !m->valid()) {
                VK_LOG_WARN("FOLIAGE-MESH-FAIL path=%s (obj 加载失败)",
                            relPath.c_str());
                return nullptr;
            }
            VK_LOG_INFO("FOLIAGE-MESH kind=obj path=%s tris=%u", relPath.c_str(),
                        static_cast<unsigned>(m->triangleCount()));
            return m;
        }
        // .fbx / .gltf / .glb：一个文件可能含多个子网格，只取**第一个**。
        // 一个 Foliage Type 在 UE 里也只对应一个 Static Mesh —— 多材质的
        // 植被要多开几个类型，而不是把一堆子网格塞进同一个实例。
        assets::Model* model = assets::isFbxPath(relPath)
                                   ? am.loadFBX(resolved, relPath)
                                   : am.loadGLTF(resolved, relPath);
        if (!model || !model->valid() || model->subMeshes.empty()) {
            VK_LOG_WARN("FOLIAGE-MESH-FAIL path=%s (模型为空)", relPath.c_str());
            return nullptr;
        }
        assets::Mesh* m = model->subMeshes[0].mesh;
        VK_LOG_INFO("FOLIAGE-MESH kind=model path=%s subs=%llu used=0 tris=%u%s",
                    relPath.c_str(),
                    static_cast<unsigned long long>(model->subMeshes.size()),
                    static_cast<unsigned>(m ? m->triangleCount() : 0u),
                    model->subMeshes.size() > 1 ? " (只取第一个子网格)" : "");
        return m;
    } catch (const std::exception& ex) {
        // ⚠ 加载失败**不能**悄悄回退成内置网格：用户明明拖了一个模型进来，
        //   出来却是程序化草，会以为"拖拽没生效"。这里报错并保持空网格，
        //   面板上那行显示 (load failed)。
        VK_LOG_WARN("FOLIAGE-MESH-FAIL path=%s err=%s", relPath.c_str(),
                    ex.what());
        return nullptr;
    }
}

assets::Mesh* FoliageEditor::typeMesh(int k) {
    const int i = std::clamp(k, 0, kKindCount - 1);
    // 缓存：AssetManager 本身按 key 缓存，但每帧 5 次查表不值得；更要紧的是
    // 加载失败时返回 nullptr —— 不缓存的话每帧都会重试并刷一条 WARN。
    if (m_typeMesh[i] && m_typeMeshPath[i] == m_types[i].meshPath) {
        return m_typeMesh[i];
    }
    const bool changed = (m_typeMeshPath[i] != m_types[i].meshPath);
    m_typeMeshPath[i] = m_types[i].meshPath;
    m_typeMesh[i] = m_types[i].meshPath.empty()
                        ? meshFor(i)
                        : loadCustomMesh(m_types[i].meshPath);
    if (changed) {
        // 只在"来源变了"时打：这行是"拖了新模型/换回内置"的唯一可见证据，
        // 也是"精细化之后内置网格到底有几个三角"的判据（测试读它）。
        VK_LOG_INFO("FOLIAGE-TYPE-MESH kind=%s src=%s tris=%u",
                    kind(i).id,
                    m_types[i].meshPath.empty() ? "builtin"
                                                : m_types[i].meshPath.c_str(),
                    static_cast<unsigned>(m_typeMesh[i]
                                              ? m_typeMesh[i]->triangleCount()
                                              : 0u));
    }
    return m_typeMesh[i];
}

bool FoliageEditor::matchesKind(int k, const assets::Mesh* mesh) {
    if (!mesh) return false;
    const int i = std::clamp(k, 0, kKindCount - 1);
    // 1) 指针精确匹配：内置网格按 key 缓存、自定义按路径缓存，同一份资产
    //    永远是同一个 Mesh*。
    if (mesh == typeMesh(i)) return true;
    // 2) 自定义类型只认那一份资产：不许退回按 shape 认，否则模型里那个
    //    "grass" 形状的名字会把内置草也当成自己人。
    if (!m_types[i].meshPath.empty()) return false;
    // 3) shape 兜底：场景**读回来**之后内置网格会重新生成（key 可能不同），
    //    而且老场景里的灌木还是旧形状 "sphere"。判据是"形状名对得上"。
    const std::string& s = meshShapeOf(mesh);
    if (s.empty()) return false;
    const char* want = kind(i).shape;
    if (std::strcmp(want, "bush") == 0) return s == "bush" || s == "sphere";
    return s == want;
}

void FoliageEditor::meshExtent(int k, float& height, float& bottom) {
    height = 1.0f;
    bottom = 0.0f;
    const assets::Mesh* m = typeMesh(k);
    if (!m) return;
    const glm::vec3 lo = m->boundsMin();
    const glm::vec3 hi = m->boundsMax();
    height = std::max(hi.z - lo.z, 1.0e-3f);
    // bottom = 原点到底边的距离。内置网格的原点就在脚底（≈0），而外部模型
    // 的原点常常在**几何中心**（Blender 导出默认）—— 不补这一下，模型会有
    // 一半埋进地里。
    bottom = lo.z;
}

assets::Material* FoliageEditor::materialFor(int k, int variant) {
    const Kind& K = kind(k);
    assets::AssetManager& am = m_ctx.assets();
    char key[64];
    const int v = std::clamp(variant, 0, std::max(m_types[std::clamp(k, 0,
                                                     kKindCount - 1)].variants,
                                                 1) - 1);
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
    int hidden[kKindCount] = {0, 0, 0, 0, 0};

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
        // 归属判定统一走 matchesKind（指针精确 + shape 兜底）：
        //   · 内置网格按形状名认（读档后网格会重新生成，指针会变）；
        //   · 自定义网格按资产指针认（形状名是资产自己的，不可比）；
        //   · 老场景里的灌木形状名还是 "sphere"，一并认。
        int k = -1;
        for (int c = 0; c < kKindCount; ++c) {
            if (matchesKind(c, mc ? mc->mesh : nullptr)) { k = c; break; }
        }
        if (k < 0) continue;
        ++m_counts[k];
        const auto* vis = w.get<ecs::VisibilityComponent>(child);
        if (vis && !vis->visible) ++hidden[k];
    }

    // 眼睛状态**从场景反推**，而不是另存一份：
    // 读回来一个"全是隐藏的树"的工程时，面板上的眼睛必须显示"关" ——
    // 否则用户看见眼睛是睁的、画面里一棵树都没有。
    for (int i = 0; i < kKindCount; ++i) {
        if (m_counts[i] > 0) m_types[i].visible = (hidden[i] < m_counts[i]);
    }

    if (logCounts && m_total != m_lastLoggedTotal) {
        m_lastLoggedTotal = m_total;
        VK_LOG_INFO("FOLIAGE-COUNT total=%d grass=%d bush=%d tree=%d "
                    "flower=%d rock=%d",
                    m_total, m_counts[0], m_counts[1], m_counts[2],
                    m_counts[3], m_counts[4]);
    }
}

void FoliageEditor::applyEnvOverrides() {
    const char* kindEnv = std::getenv("MYVK_FOLIAGE_KIND");
    if (kindEnv) {
        const char* id = kindEnv;
        int k = -1;
        for (int c = 0; c < kKindCount; ++c) {
            if (std::strcmp(kind(c).id, id) == 0) { k = c; break; }
        }
        if (k < 0) k = std::atoi(kindEnv);
        if (k >= 0 && k < kKindCount) m_kind = k;
    }
    const int k = std::clamp(m_kind, 0, kKindCount - 1);
    TypeSettings& T = m_types[k];
    float a = 0.0f, b = 0.0f, c = 0.0f;
    if (const char* e = std::getenv("MYVK_FOLIAGE_SCALE")) {
        if (std::sscanf(e, "%f,%f", &a, &b) == 2) {
            T.minScale = a;
            T.maxScale = b;
        }
    }
    if (const char* e = std::getenv("MYVK_FOLIAGE_OFFSET")) {
        if (std::sscanf(e, "%f,%f,%f", &a, &b, &c) == 3) {
            T.offsetMin = glm::vec3(-a, -b, -c);
            T.offsetMax = glm::vec3(a, b, c);
        }
    }
    if (const char* e = std::getenv("MYVK_FOLIAGE_ROT")) {
        if (std::sscanf(e, "%f,%f,%f", &a, &b, &c) == 3) {
            T.rotMinDeg = glm::vec3(-a, -b, -c);
            T.rotMaxDeg = glm::vec3(a, b, c);
        }
    }
    if (const char* e = std::getenv("MYVK_FOLIAGE_MESH")) T.meshPath = e;

    VK_LOG_INFO("FOLIAGE-ENV kind=%s scale=%.2f..%.2f offset=%.2f,%.2f,%.2f "
                "rot=%.0f,%.0f,%.0f mesh=%s",
                kind(k).id, static_cast<double>(T.minScale),
                static_cast<double>(T.maxScale),
                static_cast<double>(T.offsetMax.x),
                static_cast<double>(T.offsetMax.y),
                static_cast<double>(T.offsetMax.z),
                static_cast<double>(T.rotMaxDeg.x),
                static_cast<double>(T.rotMaxDeg.y),
                static_cast<double>(T.rotMaxDeg.z),
                T.meshPath.empty() ? "builtin" : T.meshPath.c_str());
}

void FoliageEditor::sync() {
    if (!m_envApplied) {
        m_envApplied = true;
        applyEnvOverrides();
    }
    if (!m_root.valid() || !m_ctx.editorScene().world().alive(m_root)) {
        ensureRoot();
    }
    recount(true);
}

// ============================================================
// 单株的随机化（撒点与 Reapply 共用同一份）
// ============================================================
// ============================================================
// 落点采样：**三条路共用这一份**（撒点 / 单株 / Reapply）
// ============================================================
bool FoliageEditor::samplePlacement(int k, float wx, float wy, Placement& out,
                                   const char** reject) {
    const int i = std::clamp(k, 0, kKindCount - 1);
    const Kind& K = kind(i);
    const TypeSettings& T = m_types[i];
    if (reject) *reject = "";

    // ---- 1) 先叠随机位置偏移，再在**偏移后的落点**上取地形 ----
    // 顺序很重要：偏移后的落点可能落在陡坡上，坡度过滤必须按"植株真正站的
    // 那一格"判断，否则偏移量大时会有一批树挂到悬崖上。
    wx += T.offsetMin.x + (T.offsetMax.x - T.offsetMin.x) * rnd();
    wy += T.offsetMin.y + (T.offsetMax.y - T.offsetMin.y) * rnd();
    const float oz = T.offsetMin.z + (T.offsetMax.z - T.offsetMin.z) * rnd();

    const float wz = m_land.heightAtWorld(wx, wy);
    const glm::vec3 nrm = m_land.normalAtWorld(wx, wy);

    // ---- 2) 坡度过滤 ----
    if (T.slopeDeg > 0.0f) {
        const float cosT = std::clamp(nrm.z, -1.0f, 1.0f);
        const float slopeDeg = std::acos(cosT) * 57.2957795f;
        if (slopeDeg > T.slopeDeg) {
            if (reject) *reject = "slope";
            return false;
        }
    }
    // ---- 3) 高度带过滤 ----
    if (T.minH > 0.0f || T.maxH < 1.0f) {
        float hLo = 0.0f, hHi = 1.0f;
        m_land.heightRangeWorld(hLo, hHi);
        const float t = (wz - hLo) / std::max(hHi - hLo, 1.0e-4f);
        if (t < T.minH || t > T.maxH) {
            if (reject) *reject = "height";
            return false;
        }
    }

    // ---- 4) 缩放 ----
    // ⚠ 2026-10-28：这里原来还有一层 `lateral`（横向 ±12% 抖动），用来避免
    //   "同网格 + 同缩放 = 一眼看出复制粘贴"。加了**用户可设区间**之后它是错的：
    //   用户把区间设成 2.0..2.0，实测却出来 1.76..2.24 —— 区间承诺被内部抖动
    //   打破了（用例 C2 当场抓到这个）。变化量本来就该由区间提供，所以删掉。
    const float lo = std::min(T.minScale, T.maxScale) * T.sizeScale;
    const float hi = std::max(T.minScale, T.maxScale) * T.sizeScale;
    const float scale = lo + (hi - lo) * rnd();
    out.scaleXZ = scale;
    out.scaleZ = scale * K.squashY;

    // ---- 5) 贴地：网格原点不一定在脚底 ----
    // `-bottom * scaleZ` 把"原点在几何中心"的外部模型抬到地面上；
    // 再压一点（sink）让根/石底的圆面埋进土里，避免"悬空的球"。
    float melH = 1.0f, melBottom = 0.0f;
    meshExtent(i, melH, melBottom);
    const bool roundBase = std::strcmp(K.shape, "bush") == 0 ||
                           std::strcmp(K.shape, "sphere") == 0 ||
                           std::strcmp(K.shape, "rock") == 0;
    // 自定义网格不做"圆底埋深"（不知道模型的底长什么样），默认只埋 2%
    const float sinkFrac =
        T.meshPath.empty() ? (roundBase ? 0.30f : 0.02f) : 0.02f;
    const float sink = sinkFrac * melH * out.scaleZ;
    out.pos = glm::vec3(wx, wy, wz - melBottom * out.scaleZ - sink + oz);

    // ---- 6) 旋转：贴坡 + 倾斜抖动 + 用户区间 ----
    // 贴合法线的近似：绕 X 转 -asin(ny)、绕 Y 转 asin(nx)。地形坡度在
    // ±40° 以内时与"从 up 转到 n 的四元数"肉眼无差。
    float rx = 0.0f, ry = 0.0f;
    if (T.align) {
        rx = -std::asin(std::clamp(nrm.y, -1.0f, 1.0f));
        ry = std::asin(std::clamp(nrm.x, -1.0f, 1.0f));
    }
    const float jitter = T.tilt * 0.24f;   // 1.0 → ±14°
    if (jitter > 0.0f) {
        rx += (rnd() * 2.0f - 1.0f) * jitter;
        ry += (rnd() * 2.0f - 1.0f) * jitter;
    }
    // 用户设的区间（度 → 弧度）叠在贴坡结果之上
    constexpr float kDeg = 3.14159265f / 180.0f;
    rx += (T.rotMinDeg.x + (T.rotMaxDeg.x - T.rotMinDeg.x) * rnd()) * kDeg;
    ry += (T.rotMinDeg.y + (T.rotMaxDeg.y - T.rotMinDeg.y) * rnd()) * kDeg;
    // Yaw：**基础随机 0..360°** 始终生效（撒点本来就该朝向各异），
    // 用户区间是在它之上再加的抖动（与 UE 的 Random Yaw Angle 同义）。
    float rz = 6.2831853072f * rnd();
    rz += (T.rotMinDeg.z + (T.rotMaxDeg.z - T.rotMinDeg.z) * rnd()) * kDeg;
    // 石头随便躺：只压扁不旋转的话，一堆石头是同一个朝向的"饼"。
    if (std::strcmp(K.shape, "rock") == 0) {
        rx += (rnd() * 2.0f - 1.0f) * 0.6f;
        rz += (rnd() * 2.0f - 1.0f) * 3.14159265f;
    }
    out.rot = glm::vec3(rx, ry, rz);

    // ---- 7) 颜色变体 ----
    out.variant =
        static_cast<int>(rnd() * static_cast<float>(std::max(T.variants, 1)));
    return true;
}

void FoliageEditor::spawn(int k, const Placement& pl) {
    scene::Scene& sc = m_ctx.editorScene();
    ecs::World& w = sc.world();

    const int i = std::clamp(k, 0, kKindCount - 1);
    const Kind& K = kind(i);
    char name[48];
    std::snprintf(name, sizeof(name), "%s %03d", K.name, ++m_spawnSeq);

    const ecs::Entity e = sc.createObject(name);
    if (auto* t = w.get<ecs::TransformComponent>(e)) {
        t->position = pl.pos;
        t->rotation = pl.rot;
        t->scale = glm::vec3(pl.scaleXZ, pl.scaleZ, pl.scaleXZ);
    }

    ecs::MeshComponent mc;
    mc.mesh = typeMesh(i);
    w.add<ecs::MeshComponent>(e, mc);

    ecs::MaterialComponent matc;
    matc.material = materialFor(i, pl.variant);
    w.add<ecs::MaterialComponent>(e, matc);

    if (auto* vis = w.get<ecs::VisibilityComponent>(e)) {
        vis->visible = m_types[i].visible;
    }

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
    int removed = 0;

    for (ecs::EntityId id : kids) {
        const ecs::Entity child = w.handle(id);
        if (!child.valid() || !w.alive(child)) continue;
        if (!anyKind) {
            const auto* mc = w.get<ecs::MeshComponent>(child);
            // 只删**当前选中的种类**：撒了一片草地之后想把树擦掉，结果
            // 连带把草也铲了，是最容易让人骂人的交互。
            // ⚠ 判据是 matchesKind（指针精确 + shape 兜底），不是"形状名相等"
            //   —— 自定义网格类型的形状名是资产自己的，字符串比不了。
            if (!matchesKind(m_kind, mc ? mc->mesh : nullptr)) continue;
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
    int removed = 0;
    for (ecs::EntityId id : kids) {
        const ecs::Entity child = w.handle(id);
        if (!child.valid() || !w.alive(child)) continue;
        if (kindIndex >= 0) {
            const auto* mc = w.get<ecs::MeshComponent>(child);
            if (!matchesKind(kindIndex, mc ? mc->mesh : nullptr)) continue;
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

// ============================================================
// 撒点
// ============================================================
void FoliageEditor::stamp(const glm::vec3& worldHit, bool erase) {
    if (!m_land.hasTerrain()) return;
    if (erase) {
        eraseAt(worldHit, !m_onlyCurrent);
        return;
    }

    if (m_total >= kMaxInstances) {
        VK_LOG_WARN("FOLIAGE-CAP total=%d limit=%d (stop scattering)", m_total,
                    kMaxInstances);
        return;
    }

    const Kind& K = kind(m_kind);
    const TypeSettings& T = m_types[std::clamp(m_kind, 0, kKindCount - 1)];

    // 一次落笔撒 N 株。落点用**极坐标均匀采样**（r = R·sqrt(u)）而不是
    // 在方框里拒绝采样 —— 后者在圆边缘会稀疏，视觉上像一圈空边。
    //
    // edge > 0 时再压一次半径分布：把点往圆心拉，笔刷边缘自然变稀。
    // 硬边的圆盘在草地上是一个**能看出来的圆**，那是最出戏的细节。
    const int n = std::max(1, T.density);
    const float minDist =
        0.45f * m_radius / std::sqrt(static_cast<float>(n) + 1.0f);
    std::vector<glm::vec2> placed;

    int placedCount = 0, skippedSlope = 0, skippedHeight = 0;
    // 本笔的缩放实测范围：写进日志，让"随机区间到底生效没有"有据可查
    // （测试靠这个断言，人肉也一眼看得出是"真的随机"还是"全都一样大"）。
    float sMin = 1.0e9f, sMax = -1.0e9f;
    // 本笔实例相对**落点**的最大水平距离：它是"随机偏移区间有没有生效"的
    // 直接证据（偏移为 0 时它不可能超过笔刷半径，设了 5 m 偏移就会明显超出）。
    float maxOff = 0.0f;

    for (int i = 0; i < n * 3 && placedCount < n; ++i) {
        // sqrt 采样保证面积均匀；edge 把 u 往上抬 → 半径分布偏向圆心
        const float u = rnd();
        const float rNorm = std::sqrt(u) * (1.0f - T.edge * 0.65f);
        const float r = m_radius * rNorm;
        const float a = 6.2831853072f * rnd();
        const glm::vec2 off(std::cos(a) * r, std::sin(a) * r);
        bool tooClose = false;
        for (const glm::vec2& p : placed) {
            if (glm::length(p - off) < minDist) { tooClose = true; break; }
        }
        if (tooClose) continue;
        placed.push_back(off);

        // 落点规格（含随机偏移 / 缩放 / 旋转 / 坡度高度过滤）统一由
        // samplePlacement 算 —— 单株放置与 Reapply 调的是同一个函数。
        Placement pl;
        const char* reject = "";
        if (!samplePlacement(m_kind, worldHit.x + off.x, worldHit.y + off.y, pl,
                             &reject)) {
            if (std::strcmp(reject, "slope") == 0) ++skippedSlope;
            else if (std::strcmp(reject, "height") == 0) ++skippedHeight;
            continue;
        }
        spawn(m_kind, pl);
        sMin = std::min(sMin, pl.scaleXZ);
        sMax = std::max(sMax, pl.scaleXZ);
        maxOff = std::max(maxOff,
                          glm::length(glm::vec2(pl.pos.x - worldHit.x,
                                                pl.pos.y - worldHit.y)));
        ++placedCount;
    }

    recount(false);
    m_ctx.dirty() = true;
    VK_LOG_INFO("FOLIAGE-SPAWN kind=%s placed=%d skippedSlope=%d "
                "skippedHeight=%d total=%d scale=%.2f..%.2f maxOff=%.2f "
                "mesh=%s",
                K.id, placedCount, skippedSlope, skippedHeight, m_total,
                placedCount ? static_cast<double>(sMin) : 0.0,
                placedCount ? static_cast<double>(sMax) : 0.0,
                static_cast<double>(maxOff),
                T.meshPath.empty() ? "builtin" : T.meshPath.c_str());
}

// Single 工具：光标处**一株**。
// 与 stamp 的区别只有"落几株"和"要不要做半径采样"——过滤规则完全一样，
// 所以石头上撒不出草这件事在这里也成立（用户期望是一致的）。
void FoliageEditor::placeOne(const glm::vec3& worldHit) {
    if (!m_land.hasTerrain()) return;
    if (m_total >= kMaxInstances) {
        VK_LOG_WARN("FOLIAGE-CAP total=%d limit=%d (stop scattering)", m_total,
                    kMaxInstances);
        return;
    }
    const Kind& K = kind(m_kind);

    // 与撒点**同一套规则**（含过滤/偏移/缩放/旋转）：换工具不该换规则。
    Placement pl;
    const char* reject = "";
    if (!samplePlacement(m_kind, worldHit.x, worldHit.y, pl, &reject)) {
        VK_LOG_INFO("FOLIAGE-SINGLE kind=%s placed=0 reason=%s", K.id, reject);
        return;
    }
    spawn(m_kind, pl);
    recount(false);
    m_ctx.dirty() = true;
    VK_LOG_INFO("FOLIAGE-SINGLE kind=%s placed=1 total=%d mesh=%s", K.id,
                m_total,
                m_types[std::clamp(m_kind, 0, kKindCount - 1)].meshPath.empty()
                    ? "builtin"
                    : m_types[std::clamp(m_kind, 0, kKindCount - 1)]
                          .meshPath.c_str());
}

// ============================================================
// 类型可见性 / Reapply（UE5 的两个"没有就只能清空重撒"的功能）
// ============================================================
void FoliageEditor::setTypeVisible(int k, bool on) {
    scene::Scene& sc = m_ctx.editorScene();
    ecs::World& w = sc.world();
    if (!m_root.valid() || !w.alive(m_root)) return;
    const auto* h = w.get<ecs::HierarchyComponent>(m_root);
    if (!h) return;
    int n = 0;
    for (ecs::EntityId id : h->children) {
        const ecs::Entity child = w.handle(id);
        if (!child.valid() || !w.alive(child)) continue;
        const auto* mc = w.get<ecs::MeshComponent>(child);
        if (!matchesKind(k, mc ? mc->mesh : nullptr)) continue;
        if (auto* vis = w.get<ecs::VisibilityComponent>(child)) {
            vis->visible = on;
            ++n;
        }
    }
    m_visApplied[k] = on;
    VK_LOG_INFO("FOLIAGE-VIS %s=%d n=%d", kind(k).id, on ? 1 : 0, n);
}

int FoliageEditor::reapplyCurrentType() {
    scene::Scene& sc = m_ctx.editorScene();
    ecs::World& w = sc.world();
    if (!m_root.valid() || !w.alive(m_root)) return 0;
    const auto* h = w.get<ecs::HierarchyComponent>(m_root);
    if (!h) return 0;

    const int k = std::clamp(m_kind, 0, kKindCount - 1);
    const Kind& K = kind(k);
    const TypeSettings& T = m_types[k];
    // 拷贝：循环里会改 Transform / Material / 甚至销毁，但不动 children
    const std::vector<ecs::EntityId> kids = h->children;
    int n = 0, dropped = 0;
    for (ecs::EntityId id : kids) {
        const ecs::Entity child = w.handle(id);
        if (!child.valid() || !w.alive(child)) continue;
        const auto* mc = w.get<ecs::MeshComponent>(child);
        if (!matchesKind(k, mc ? mc->mesh : nullptr)) continue;

        // ⚠ 与 samplePlacement **同一份规则**：位置 / 缩放 / 旋转 / 过滤全在那儿。
        //   被新规则排除的实例**删掉** —— 这就是 UE "Reapply 按新规则重算"的
        //   语义，否则把坡度上限调低之后，陡坡上的老树永远清不掉。
        //
        //   为什么用 world 坐标的 XY 作为采样点：Reapply 的语义是"原地重随机"，
        //   不是"挪到别处"。偏移量由 samplePlacement 内部再加（会叠加，这是
        //   可以接受的：连续点几次 Reapply 会慢慢漂移，但只要用户不改参数
        //   就不会动 —— 因为偏移区间默认为 0）。
        const glm::vec3 p(sc.worldMatrix(child)[3]);
        Placement pl;
        const char* reject = "";
        if (!samplePlacement(k, p.x, p.y, pl, &reject)) {
            if (auto* ch = w.get<ecs::HierarchyComponent>(child)) {
                ch->parent = ecs::kInvalidEntity;
            }
            w.destroy(child);
            ++dropped;
            continue;
        }
        if (auto* t = w.get<ecs::TransformComponent>(child)) {
            t->position = pl.pos;
            t->rotation = pl.rot;
            t->scale = glm::vec3(pl.scaleXZ, pl.scaleZ, pl.scaleXZ);
        }
        if (auto* matc = w.get<ecs::MaterialComponent>(child)) {
            matc->material = materialFor(k, pl.variant);
        }
        if (auto* vis = w.get<ecs::VisibilityComponent>(child)) {
            vis->visible = T.visible;
        }
        // 网格也可能换过（用户拖了新模型）→ 一起刷新
        if (auto* mcomp = w.get<ecs::MeshComponent>(child)) {
            if (assets::Mesh* m = typeMesh(k)) mcomp->mesh = m;
        }
        ++n;
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
    VK_LOG_INFO("FOLIAGE-REAPPLY kind=%s n=%d dropped=%d total=%d mesh=%s",
                K.id, n, dropped, m_total,
                T.meshPath.empty() ? "builtin" : T.meshPath.c_str());
    return n;
}

// ============================================================
// 视口输入
// ============================================================
bool FoliageEditor::viewportInput(const glm::vec2& vpPos,
                                  const glm::vec2& vpSize, bool hovered,
                                  ImDrawList* dl) {
    sync();
    if (!m_land.hasTerrain() || !m_ctx.isEditing()) return false;

    // Select 工具：**什么都不吃**，直接让视口的正常点选去干活。
    // 这正是 UE 的语义（Select 模式下点一株就是选中它，然后在 Details
    // 面板里改/删），而"选中"这件事视口已经做得很好了 —— 在这里再写一遍
    // 拾取只会得到两套略微不同的语义（比如隐藏的实例能不能点到）。
    if (m_tool == Tool::Select) return false;

    const ImGuiIO& io = ImGui::GetIO();
    const glm::vec2 mouse(io.MousePos.x, io.MousePos.y);
    const Ray ray =
        PickingSystem::rayFromViewport(m_ctx.editorScene().camera(), vpPos,
                                       vpSize, mouse);
    glm::vec3 hit(0.0f), nrm(0.0f);
    const bool onTerrain =
        (hovered || m_stroking) && m_land.raycast(ray.origin, ray.dir, hit, nrm);

    const bool erasing = (m_tool == Tool::Erase);
    const bool single = (m_tool == Tool::Single);

    // 光标：地上的圆环（与地形笔刷同一个观感，颜色区分开）。
    // 内圈 = 密度衰减后的"实心区"，一眼能看出笔刷边缘会不会稀疏。
    // Single 不画内圈（它不撒一片，画衰减圈是误导）。
    if (onTerrain && dl) {
        const glm::mat4 vp = PickingSystem::viewProj(
            m_ctx.editorScene().camera(), vpSize);
        const float ringR = single ? m_types[std::clamp(m_kind, 0,
                                                        kKindCount - 1)]
                                         .sizeScale * kind(m_kind).size * 0.5f
                                   : m_radius;
        auto projectRing = [&](float radiusScale, ImVec2* pts) -> bool {
            for (int i = 0; i <= 32; ++i) {
                const float a = 6.2831853072f * static_cast<float>(i) / 32.0f;
                const float rr = ringR * radiusScale;
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
            if (!single) dl->AddConcavePolyFilled(outer, 32, fill);
            dl->AddPolyline(outer, 33, col, 0, 1.8f);
            const float edge = m_types[std::clamp(m_kind, 0, kKindCount - 1)].edge;
            if (!single && edge > 0.02f && !erasing) {
                ImVec2 inner[33];
                const float innerScale = std::max(1.0f - edge * 0.8f, 0.2f);
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
            TypeSettings& T = m_types[std::clamp(m_kind, 0, kKindCount - 1)];
            T.density = std::clamp(T.density + static_cast<int>(n * 2.0f), 1, 200);
            VK_LOG_INFO("FO-BRUSH density=%d", T.density);
        } else {
            m_radius = std::clamp(m_radius * std::pow(1.12f, n), 1.0f, 120.0f);
            VK_LOG_INFO("FO-BRUSH radius=%.2f", static_cast<double>(m_radius));
        }
        wheelConsumed = true;
    }

    if (clicked && hovered && onTerrain && !navGesture) {
        if (single) {
            placeOne(hit);
        } else {
            m_stroking = true;
            m_lastStamp = hit;
            stamp(hit, erasing);
        }
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
                stamp(hit, erasing);
            }
        }
    }
    return consumed || wheelConsumed || (onTerrain && hovered);
}

// ============================================================
// 面板（对齐 UE5 的四块：工具 / 类型树 / 笔刷 / 类型细节）
// ============================================================
void FoliageEditor::drawPanel() {
    bool& open = panelOpenRef("foliage");
    if (!open) return;

    sync();

    // 2026-10-21：与地形同一个接法 —— 左栏下半是"当前模式的参数区"。
    ImGuiWindowFlags wf = 0;
    const bool railed = beginModeRailPanel(m_ctx, wf);
    if (!railed) {
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowSizeConstraints(ImVec2(320.0f, 360.0f),
                                            ImVec2(FLT_MAX, FLT_MAX));
        ImGui::SetNextWindowSize(ImVec2(420.0f, 680.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.32f,
                                       io.DisplaySize.y * 0.10f),
                                ImGuiCond_FirstUseEver);
    }
    // ID 必须是 `###panel_<面板表 id>`，理由见 LandscapeEditor::drawPanel
    if (!ImGui::Begin(TR("Foliage###panel_foliage"),
                      railed ? nullptr : &open, wf)) {
        ImGui::End();
        return;
    }
    logRect("FO-PANEL", ImGui::GetWindowPos(),
            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                   ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    ImGui::TextDisabled("%s", TR(editModeHint(EditMode::Foliage)));

    if (!m_land.hasTerrain()) {
        drawNoTerrainCard();
        ImGui::End();
        return;
    }

    drawToolRow();
    drawTypeList();
    drawBrushSection();
    drawTypeSettings();
    drawStats();

    ImGui::End();
}

void FoliageEditor::drawNoTerrainCard() {
    ImGui::Separator();
    ImGui::TextWrapped("%s",
                       TR("Foliage is scattered on a terrain. Create one "
                          "in Landscape mode first."));
    if (ImGui::Button(TR("Go to Landscape mode"), ImVec2(-1.0f, 30.0f))) {
        m_ctx.setEditMode(EditMode::Landscape);
    }
    logRect("FO-GO-LANDSCAPE", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
}

void FoliageEditor::drawToolRow() {
    ImGui::SeparatorText(TR("Tool"));
    // UE 是 5 个图标一排；这里没有图标字体，用文字按钮 + 响应式网格
    // （宽栏 4 列一排，窄栏退成 2×2）。锚点 FO-TOOL-<i> 按视觉顺序打。
    struct Item { Tool t; const char* label; };
    const Item items[kToolCount] = {
        {Tool::Paint, "Paint"},
        {Tool::Erase, "Erase"},
        {Tool::Single, "Single"},
        {Tool::Select, "Select"},
    };
    const float avail = ImGui::GetContentRegionAvail().x;
    const int cols = panelGridColumns(avail, 74.0f, kBtnGap);
    const float bw = panelGridWidth(avail, cols, kBtnGap);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(kBtnGap, kBtnGap));
    for (int i = 0; i < kToolCount; ++i) {
        const bool on = (m_tool == items[i].t);
        if (on) {
            ImGui::PushStyleColor(
                ImGuiCol_Button,
                ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        if (panelGridButton(TR(items[i].label), i, cols, bw, kBtnRowH)) {
            m_tool = items[i].t;
            // 换工具时收尾：正在拖的笔画要结束，否则切到 Erase 之后
            // 鼠标还按着 → 下一帧 m_stroking 仍是 true，会"接着擦"。
            m_stroking = false;
            VK_LOG_INFO("FOLIAGE-TOOL %s", toolId(m_tool));
        }
        if (on) ImGui::PopStyleColor();
        char tag[32];
        std::snprintf(tag, sizeof(tag), "FO-TOOL-%d", i);
        logRect(tag, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    }
    ImGui::PopStyleVar();

    // 一行提示：当前工具的鼠标语义（UE 里要去 hover tooltip 才知道，
    // 这里直接写出来 —— 左栏不缺点地方）。
    const char* hint = "";
    switch (m_tool) {
        case Tool::Paint:
            hint = "Left-drag on the terrain to scatter. Shift = erase.";
            break;
        case Tool::Erase:
            hint = "Left-drag to remove instances of the selected type.";
            break;
        case Tool::Single:
            hint = "Left-click places exactly one instance.";
            break;
        case Tool::Select:
            hint = "Left-click an instance to select it in the Inspector.";
            break;
    }
    ImGui::TextDisabled("%s", TR(hint));
}

void FoliageEditor::drawTypeList() {
    ImGui::SeparatorText(TR("Foliage types"));
    // 每行：[眼睛] [名字 …………… 株数]
    //   · 眼睛 = 可见性（UE 同款）。关掉不等于删除，存盘也还在。
    //   · 点名字那一块 = 选中类型（Inspector 语义：选中谁，下面就配谁）。
    //
    // ⚠ 眼睛和名字是**两个控件**，不是"行内塞按钮"：ImGui 的
    //   `Selectable` + 行内小按钮要靠 AllowOverlap + 手工 SetCursorScreenPos，
    //   在 26px 行里很容易点串（点眼睛变成选中行）。分成两个并排控件之后
    //   谁都不抢谁的命中区，代价只是行首那 26px。
    const float avail = ImGui::GetContentRegionAvail().x;
    const float eyeW = 24.0f;
    const float rowH = 26.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 4.0f));
    for (int i = 0; i < kKindCount; ++i) {
        const Kind& K = kind(i);
        TypeSettings& T = m_types[i];
        ImGui::PushID(i);

        if (eyeButton("vis", T.visible, eyeW)) {
            T.visible = !T.visible;
            setTypeVisible(i, T.visible);
        }
        char visTag[24];
        std::snprintf(visTag, sizeof(visTag), "FO-VIS-%d", i);
        logRect(visTag, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", T.visible
                                        ? TR("Hide these instances (keeps them "
                                             "in the scene)")
                                        : TR("Show these instances again"));
        }

        ImGui::SameLine();
        const bool sel = (m_kind == i);
        // 标签直接用名字：ID 唯一性由外面的 `PushID(i)` 保证，不需要
        // `"%s##row"` 这种拼接（拼了反而让中文化扫描把它当成"未包的格式串"）。
        if (ImGui::Selectable(TR(K.name), sel, ImGuiSelectableFlags_None,
                              ImVec2(std::max(avail - eyeW - 4.0f, 40.0f),
                                     rowH))) {
            m_kind = i;
            VK_LOG_INFO("FOLIAGE-KIND %s", K.id);
        }
        // 株数右对齐画在行尾（Selectable 的 label 只有一个，没法两端对齐）
        {
            char cnt[32];
            std::snprintf(cnt, sizeof(cnt), "%d", m_counts[i]);
            const ImVec2 rmin = ImGui::GetItemRectMin();
            const ImVec2 rmax = ImGui::GetItemRectMax();
            const ImVec2 tw = ImGui::CalcTextSize(cnt);
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(rmax.x - tw.x - 6.0f,
                       (rmin.y + rmax.y - tw.y) * 0.5f),
                ImGui::GetColorU32(T.visible ? ImGuiCol_Text
                                             : ImGuiCol_TextDisabled),
                cnt);
        }
        char kindTag[24];
        std::snprintf(kindTag, sizeof(kindTag), "FO-KIND-%d", i);
        logRect(kindTag, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", TR(K.name));
        }
        ImGui::PopID();
    }
    ImGui::PopStyleVar();

    // 只有当前类型参与擦除 / 过滤（UE 的 "Filter by type"）
    ImGui::Checkbox(TR("Only the selected type"), &m_onlyCurrent);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s",
                          TR("Erasing removes only the selected type. "
                             "Untick to erase everything under the brush."));
    }
    ImGui::TextDisabled(TR("Instances: %d"), m_total);
}

void FoliageEditor::drawBrushSection() {
    ImGui::SeparatorText(TR("Brush"));
    TypeSettings& T = m_types[std::clamp(m_kind, 0, kKindCount - 1)];

    labeledSlider(TR("Size"), "##fo_size", &m_radius, 1.0f, 80.0f, "%.1f m");
    labeledSlider(TR("Falloff"), "##fo_falloff", &T.edge, 0.0f, 1.0f, "%.2f");
    ImGui::TextUnformatted(TR("Per stamp"));
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderInt("##fo_density", &T.density, 1, 200);

    ImGui::TextDisabled("%s",
                        TR("Scroll = brush size, Shift+scroll = per stamp. "
                           "Right-drag moves the camera."));
}

// ============================================================
// 网格来源槽：把内容浏览器里的模型拖到这一行 = 绑定到当前类型
// ============================================================
void FoliageEditor::drawMeshSlot() {
    const int k = std::clamp(m_kind, 0, kKindCount - 1);
    TypeSettings& T = m_types[k];
    const Kind& K = kind(k);

    ImGui::TextUnformatted(TR("Mesh"));
    const bool custom = !T.meshPath.empty();
    assets::Mesh* m = typeMesh(k);

    // 槽里显示的是**当前真的在用的东西**：内置名 / 资产相对路径 /
    // 加载失败（后者必须显式写出来 —— 悄悄退回内置网格会让人以为拖拽没生效）。
    std::string label;
    if (!custom) label = std::string(TR("Built-in: ")) + TR(K.name);
    else if (m) label = T.meshPath;
    else label = T.meshPath + TR("  (load failed)");

    const float avail = ImGui::GetContentRegionAvail().x;
    ImGui::Button(label.c_str(), ImVec2(custom ? avail - 32.0f : -1.0f, 30.0f));
    logRect("FO-MESH", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s",
                          TR("Drag a model (.obj / .fbx / .gltf / .glb) from "
                             "the Content browser onto this slot to use it "
                             "for this type."));
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(drag::kAsset)) {
            const char* path = static_cast<const char*>(p->Data);
            T.meshPath = path ? path : "";
            // typeMesh() 下一帧发现路径变了会重新解析（缓存键就是路径）
            VK_LOG_INFO("FOLIAGE-MESH kind=%s path=%s", K.id,
                        T.meshPath.c_str());
            m_ctx.notify(std::string(TR("Foliage mesh: ")) + T.meshPath);
        }
        ImGui::EndDragDropTarget();
    }
    if (custom) {
        ImGui::SameLine();
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 0.0f));
        if (ImGui::Button("x##fo_mesh_clear", ImVec2(28.0f, 30.0f))) {
            T.meshPath.clear();
            VK_LOG_INFO("FOLIAGE-MESH kind=%s path=builtin", K.id);
        }
        ImGui::PopStyleVar();
        logRect("FO-MESH-CLEAR", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", TR("Back to the built-in mesh"));
        }
    }
}

void FoliageEditor::drawTypeSettings() {
    const int k = std::clamp(m_kind, 0, kKindCount - 1);
    const Kind& K = kind(k);
    TypeSettings& T = m_types[k];

    ImGui::SeparatorText(TR("Foliage type"));
    drawMeshSlot();

    // ---- 随机区间（用户要求"偏移/旋转/缩放区间可以自己设"）----
    // 用 DragFloat**Range2 会让标签占掉半行，所以这里用"min 一行 / max 一行"
    // 的 DragFloat3 —— 六个数字两行放完，面板宽度 300 时也点得准。
    if (ImGui::CollapsingHeader(TR("Random ranges"),
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextUnformatted(TR("Scale range"));
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::DragFloatRange2("##fo_scale", &T.minScale, &T.maxScale, 0.02f,
                               0.1f, 3.0f, "%.2f", "%.2f");

        ImGui::TextUnformatted(TR("Offset min (m)"));
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::DragFloat3("##fo_offmin", &T.offsetMin.x, 0.01f, -20.0f, 20.0f,
                          "%.2f");

        ImGui::TextUnformatted(TR("Offset max (m)"));
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::DragFloat3("##fo_offmax", &T.offsetMax.x, 0.01f, -20.0f, 20.0f,
                          "%.2f");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s",
                              TR("Random offset added to the hit point, in "
                                 "metres (X / Y / Z). Z is handy to lift "
                                 "plants slightly out of the ground."));
        }

        ImGui::TextUnformatted(TR("Rotation min (deg)"));
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::DragFloat3("##fo_rotmin", &T.rotMinDeg.x, 1.0f, -180.0f, 180.0f,
                          "%.0f");

        ImGui::TextUnformatted(TR("Rotation max (deg)"));
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::DragFloat3("##fo_rotmax", &T.rotMaxDeg.x, 1.0f, -180.0f, 180.0f,
                          "%.0f");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s",
                              TR("Extra random rotation in degrees: X = pitch, "
                                 "Y = roll, Z = extra yaw. A random 0-360 "
                                 "yaw is always applied on top of this."));
        }
    }

    // ---- 放置规则（坡度 / 高度 / 倾斜）：默认收起，面板窄且高有限 ----
    if (ImGui::CollapsingHeader(TR("Placement rules"),
                                ImGuiTreeNodeFlags_None)) {
        labeledSlider(TR("Random tilt"), "##fo_tilt", &T.tilt, 0.0f, 1.0f,
                      "%.2f");
        labeledSlider(TR("Slope limit"), "##fo_slope", &T.slopeDeg, 0.0f, 90.0f,
                      "%.0f deg");
        if (T.slopeDeg < 0.5f) ImGui::TextDisabled("%s", TR("0 = any slope"));

        ImGui::TextUnformatted(TR("Height band"));
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::DragFloatRange2("##fo_band", &T.minH, &T.maxH, 0.005f, 0.0f, 1.0f,
                               "%.2f", "%.2f");
        ImGui::TextDisabled("%s", TR("0..1 of the terrain height range"));

        ImGui::Checkbox(TR("Align to slope"), &T.align);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderInt("##fo_variants", &T.variants, 1, 5, TR("colors %d"));
    }

    // UE 的 Reapply：把上面的设置重新套到**已经撒下去**的那些株上。
    // 没有它，改完参数只能清空重撒（几千株的话就是几分钟的手工活）。
    if (ImGui::Button(TR("Reapply to existing"), ImVec2(-1.0f, kBtnRowH))) {
        const int n = reapplyCurrentType();
        m_ctx.notify(std::string(TR("Reapplied ")) + K.name + ": " +
                     std::to_string(n) + TR(" instances"));
    }
    logRect("FO-REAPPLY", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s",
                          TR("Re-randomise every existing instance of this "
                             "type with the settings above. Instances that "
                             "break the slope rule are removed."));
    }

    const float avail = ImGui::GetContentRegionAvail().x;
    const float halfW = panelGridWidth(avail, 2, kBtnGap);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(kBtnGap, kBtnGap));
    if (ImGui::Button(TR("Clear this type"), ImVec2(halfW, kBtnRowH))) {
        clearAll(k);
    }
    logRect("FO-CLEAR-TYPE", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::Button(TR("Clear all"), ImVec2(halfW, kBtnRowH))) {
        clearAll(-1);
    }
    logRect("FO-CLEAR-ALL", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::PopStyleVar();
}

void FoliageEditor::drawStats() {
    ImGui::Separator();
    ImGui::Text(TR("Instances: %d  (grass %d · bush %d · tree %d · flower %d · rock %d)"),
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
