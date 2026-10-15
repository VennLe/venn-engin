#include "InspectorPanel.h"

#include "DebugRects.h"
#include "EditorDragDrop.h"
#include "PrimitiveBuilder.h"

#include "assets/AssetManager.h"
#include "assets/AssetPath.h"
#include "assets/Material.h"
#include "assets/Mesh.h"
#include "assets/Texture.h"
#include "core/Logger.h"
#include "ecs/Components.h"
#include "physics/CollisionWorld.h"
#include "render/ShaderKeywords.h"
#include "scene/Camera.h"
#include "scene/Light.h"
#include "scene/Scene.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <unordered_map>

namespace editor {

namespace {

// 小工具：一个带 tooltip 的灰色说明行
void hint(const char* text) {
    // 换行显示：侧栏不宽，长提示不换行就会被右边缘裁掉（原来是 TextDisabled
    // 单行，实测 "Click an object in the viewport (or in the Hierarchy)…"
    // 正好被切掉半句）。
    const ImVec4 col = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

// 组件的"移除"按钮（跟在同行控件后面）
bool removeButton() {
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.16f, 0.16f, 1.0f));
    const bool clicked = ImGui::SmallButton("Remove");
    ImGui::PopStyleColor();
    return clicked;
}

// E11 字段拖拽微调：按住 Shift 时拖拽速度降到 1/10，方便精修小数值
// （ImGui 原生 DragFloat 没有"慢速修饰键"，所以在调用点把 speed 包一层）。
float sp(float base) { return ImGui::GetIO().KeyShift ? base * 0.1f : base; }

// 大小写不敏感子串（搜索结果对大小写不敏感）
bool containsCI(const char* hay, const char* needle) {
    if (!needle || !needle[0]) return true;
    if (!hay) return false;
    std::string h(hay), n(needle);
    std::transform(h.begin(), h.end(), h.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(n.begin(), n.end(), n.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return h.find(n) != std::string::npos;
}

// E11 组件搜索的**字段关键词表**：分区名之外，命中这些词也保留该分区
// （否则"搜 rough 想找粗糙度"这种最常见的用法会因为分区叫 Material 而落空）。
const char* sectionKeywords(const char* name) {
    struct KV { const char* k; const char* v; };
    static const KV kTable[] = {
        {"Transform", "position rotation scale translate move lock"},
        {"Visibility", "visible shadow cast hide show"},
        {"Locked", "lock locked"},
        {"Mesh", "mesh geometry primitive vertex size radius segments rings cube sphere"},
        {"Material", "material base color albedo metallic roughness emissive normal ao "
                     "alpha shading model texture opacity tint"},
        {"Collision", "collision collider convex hull capsule solid physics"},
        {"Decal", "decal fade opacity tint project"},
        {"Reflection Probe", "probe reflection capture cube mirror boxproject "
                             "blend environment"},
        {"Light", "light intensity range color spot point area ies shadow"},
        {"Script", "script vks enabled timescale path"},
        {"Camera", "camera fov target distance yaw pitch"},
        {"Directional Light", "sun direction ambient sky"},
        {"Scene", "scene entities stats statistics count"},
        // 粒子发射器（R6-1）。这里必须把**每一个模块名和每一个字段**都
        // 列进来：面板给搜索做了一次"两级过滤"——组件级关键词决定整段
        // 要不要出现，模块级关键词决定段里哪些模块要出现。组件级漏一个
        // 词，用户搜 "turbulence" 就会看到"no component matches"。
        // 关键词取自 Unity Shuriken 的面板用词 + 本引擎自己的叫法。
        {"Particle Emitter",
         "particle emitter fx rate burst bursts emission playback duration "
         "looping prewarm delay lifetime speed gravity simulation space local "
         "world shape box sphere hemisphere cone circle edge emit from volume "
         "shell thickness radius extents arc rotation spread angle "
         "velocity orbital radial speed modifier limit dampen force noise "
         "turbulence octaves frequency scroll damping colour color gradient "
         "ramp alpha opacity size render billboard stretched horizontal "
         "vertical sort blend additive emissive material soft particles "
         "distance drag start enabled capacity pool"},
    };
    for (const KV& kv : kTable) {
        if (std::strcmp(kv.k, name) == 0) return kv.v;
    }
    return "";
}

// 粒子面板里**单个模块**的搜索关键词。key 用 fxModuleKey() 的短名。
const char* fxModuleKeywords(const char* key) {
    struct KV { const char* k; const char* v; };
    static const KV kTable[] = {
        {"playback", "main playback duration looping prewarm delay start "
                     "lifetime speed gravity simulation space local world "
                     "enabled"},
        {"emission", "emission rate distance burst bursts interval cycles "
                     "periodic"},
        {"shape", "shape box sphere hemisphere cone circle edge extents radius "
                  "thickness emit from volume shell arc angle rotation spread "
                  "direction"},
        {"velocity", "velocity orbital radial speed modifier world space curve"},
        {"limit", "limit velocity speed dampen damping drag clamp"},
        {"force", "force acceleration randomize wind"},
        {"colour", "colour color gradient ramp alpha opacity over lifetime fade"},
        {"size", "size scale curve grow shrink over lifetime"},
        {"rot", "rotation rotate spin angular start variance degree tilt"},
        {"noise", "noise turbulence fbm octaves frequency scroll damping "
                  "strength"},
        {"renderer", "renderer render mode billboard stretched horizontal "
                     "vertical sort blend additive alpha emissive start size "
                     "material"},
        {"soft", "soft particles fade depth distance intersection camera"},
    };
    for (const KV& kv : kTable) {
        if (std::strcmp(kv.k, key) == 0) return kv.v;
    }
    return "";
}

// E11 可"按字节快照"的 POD 组件类别。带 std::string / std::vector 的组件
// （SpotLight / Collision / Decal / Script）不在其列 —— 对它们做 memcpy 是
// 未定义行为，所以 Copy/Paste 只覆盖纯数值组件。
enum PodKind {
    PK_None = 0,
    PK_Transform,
    PK_Visibility,
    PK_DirectionalLight,
    PK_PointLight,
    // 粒子发射器**全是 POD**（含 3 个内建数组：colorRamp.stops[8] /
    // bursts[4] / 各自一堆 vec4），所以可以整体按字节快照。
    // 判据是"没有任何 std::string / std::vector / 指针"——不是"没有数组"。
    PK_ParticleEmitter,
};

PodKind podKindOf(const char* name, ecs::World& w, ecs::Entity e) {
    if (std::strcmp(name, "Transform") == 0)
        return w.has<ecs::TransformComponent>(e) ? PK_Transform : PK_None;
    if (std::strcmp(name, "Visibility") == 0)
        return w.has<ecs::VisibilityComponent>(e) ? PK_Visibility : PK_None;
    if (std::strcmp(name, "Particle Emitter") == 0)
        return w.has<ecs::ParticleEmitterComponent>(e) ? PK_ParticleEmitter
                                                       : PK_None;
    if (std::strcmp(name, "Light") == 0) {
        if (w.has<ecs::DirectionalLightComponent>(e)) return PK_DirectionalLight;
        if (w.has<ecs::PointLightComponent>(e)) return PK_PointLight;
        return PK_None;   // 聚光灯含 std::string，不能按字节复制
    }
    return PK_None;
}

void* podPtr(PodKind k, ecs::World& w, ecs::Entity e) {
    switch (k) {
        case PK_Transform:
            return w.get<ecs::TransformComponent>(e);
        case PK_Visibility:
            return w.get<ecs::VisibilityComponent>(e);
        case PK_DirectionalLight:
            return w.get<ecs::DirectionalLightComponent>(e);
        case PK_PointLight:
            return w.get<ecs::PointLightComponent>(e);
        case PK_ParticleEmitter:
            return w.get<ecs::ParticleEmitterComponent>(e);
        default:
            return nullptr;
    }
}

std::size_t podSize(PodKind k) {
    switch (k) {
        case PK_Transform: return sizeof(ecs::TransformComponent);
        case PK_Visibility: return sizeof(ecs::VisibilityComponent);
        case PK_DirectionalLight: return sizeof(ecs::DirectionalLightComponent);
        case PK_PointLight: return sizeof(ecs::PointLightComponent);
        case PK_ParticleEmitter: return sizeof(ecs::ParticleEmitterComponent);
        default: return 0;
    }
}

} // namespace

// ---------------------------------------------------------------- 主流程

void InspectorPanel::draw() {
    EditorContext::PanelVisibility& panels = m_ctx.panels();
    if (!panels.inspector) return;

    m_sectionsDrawn = 0;

    // E11 自动化钩子（一次性）：启动时按 env 折叠指定分区，用于验证
    // "折叠状态按名持久化"。正常使用不设即无影响。
    static bool s_envCollapseDone = false;
    if (!s_envCollapseDone) {
        s_envCollapseDone = true;
        if (const char* env = std::getenv("MYVK_EDITOR_INSP_COLLAPSE")) {
            std::string s(env), one;
            for (std::size_t i = 0; i <= s.size(); ++i) {
                if (i == s.size() || s[i] == ',') {
                    if (!one.empty()) m_sectionOpen[one] = false;
                    one.clear();
                } else if (s[i] != ' ') {
                    one.push_back(s[i]);
                }
            }
            core::logMessage(core::LogLevel::Info, "INSP-COLLAPSE injected=\"%s\"",
                             env);
        }
        if (const char* envS = std::getenv("MYVK_EDITOR_INSP_SEARCH")) {
            std::snprintf(m_search, sizeof(m_search), "%s", envS);
            core::logMessage(core::LogLevel::Info, "INSP-SEARCH injected=\"%s\"",
                             envS);
        }
        // 展开注入（与上面的折叠注入配对）。
        //
        // 为什么单独一个变量而不是给折叠注入加 "xxx" 之类的语法：折叠注入
        // 的解析器**会把空格吃掉**（分区名里本来没有空格），而这里必须能
        // 传 "Particle Emitter" 这种带空格的键。两个变量、两种解析，比一个
        // 变量加转义规则清楚。
        // 键就是 m_sectionOpen 的键 —— 组件级分区名，或粒子面板的
        // "fx/<模块>" 子键。
        if (const char* envE = std::getenv("MYVK_EDITOR_INSP_EXPAND")) {
            std::string s(envE), one;
            for (std::size_t i = 0; i <= s.size(); ++i) {
                if (i == s.size() || s[i] == ',') {
                    if (!one.empty()) m_sectionOpen[one] = true;
                    one.clear();
                } else {
                    one.push_back(s[i]);
                }
            }
            core::logMessage(core::LogLevel::Info, "INSP-EXPAND injected=\"%s\"",
                             envE);
        }
    }

    const LayoutRects& L = m_ctx.layout();
    // 分栏布局：每帧跟随分栏树，不允许手动拖动窗口
    ImGui::SetNextWindowPos(ImVec2(L.inspector.x, L.inspector.y),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(L.inspector.w, L.inspector.h),
                             ImGuiCond_Always);

    const ImGuiWindowFlags wf = ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoResize |
                                ImGuiWindowFlags_NoCollapse;
    if (!ImGui::Begin("Inspector", &panels.inspector, wf)) {
        ImGui::End();
        return;
    }

    scene::Scene& sc = m_ctx.activeScene();
    const ecs::Entity e = m_ctx.selection();

    if (!m_ctx.hasSelection()) {
        drawSceneryFallback();
        drawNoMatchHint();
        ImGui::End();
        return;
    }

    const std::string* np = sc.world().name(e);
    ImGui::Text("Entity: %s", np ? np->c_str() : "(unnamed)");
    ImGui::SameLine();
    ImGui::TextDisabled("#%u.%u", e.id, e.generation);

    if (!m_ctx.isEditing()) {
        ImGui::SameLine();
        ImGui::TextColored(
            ImVec4(1.0f, 0.72f, 0.3f, 1.0f),
            "  (playing - showing the runtime copy, not the editable scene)");
    }

    ImGui::Separator();

    // ---- E11 组件搜索：过滤分区与其字段关键词 ----
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##insp_search", "search components...",
                             m_search, sizeof(m_search));

    drawTransform(e);
    drawVisibility(e);
    drawMeshAndMaterial(e);
    drawLights(e);
    drawScript(e);
    drawAddRemove(e);

    drawNoMatchHint();
    logSectionState();
    ImGui::End();
}

// ------------------------------------------------------------ E11 分区辅助

// 分区标题：折叠按名记忆 + 搜索过滤 + 右键菜单。
bool InspectorPanel::sectionHeader(const char* name, bool defaultOpen,
                                   ecs::Entity e, const char* key) {
    const char* k = key ? key : name;

    // 搜索：命中分区名或字段关键词才显示（否则整段跳过）
    if (m_search[0] != '\0' && !containsCI(name, m_search) &&
        !containsCI(sectionKeywords(k), m_search)) {
        return false;
    }

    const auto it = m_sectionOpen.find(k);
    const bool wantOpen = (it != m_sectionOpen.end()) ? it->second : defaultOpen;
    // 只在 ImGui 尚未建立该分区状态时施加初始值（首帧 / 注入折叠）；
    // 之后由 ImGui 自身存储接用户点击，这里只把当代状态同步回来。
    ImGui::SetNextItemOpen(wantOpen, ImGuiCond_Appearing);
    const bool open = ImGui::CollapsingHeader(name);
    m_sectionOpen[k] = open;
    ++m_sectionsDrawn;

    if (e.valid()) drawSectionContextMenu(k, e);
    return open;
}

// 分区标题上的右键菜单：Copy / Paste / Reset to Default
void InspectorPanel::drawSectionContextMenu(const char* name, ecs::Entity e) {
    if (!ImGui::BeginPopupContextItem()) return;

    ecs::World& w = m_ctx.activeScene().world();
    const PodKind k = podKindOf(name, w, e);

    // Copy：仅 POD 分区可复制（含容器的组件不能按字节快照）
    if (ImGui::MenuItem("Copy Properties", nullptr, false, k != PK_None))
        copySection(name, e);

    // Paste：剪贴板分区名一致 + 组件类型一致 + 字节数一致
    const bool canPaste = k != PK_None && m_clipSection == name &&
                          m_clipKind == static_cast<int>(k) &&
                          m_clipBytes.size() == podSize(k) &&
                          m_ctx.isEditing();
    if (ImGui::MenuItem("Paste Properties", nullptr, false, canPaste))
        pasteSection(name, e);

    ImGui::Separator();
    if (ImGui::MenuItem("Reset to Default", nullptr, false, k != PK_None &&
                                                        m_ctx.isEditing())) {
        resetSection(name, e);
    }

    if (k == PK_None) {
        ImGui::Separator();
        ImGui::TextDisabled("no copy/paste for this section");
    }
    ImGui::EndPopup();
}

// 搜索无命中时的提示
void InspectorPanel::drawNoMatchHint() {
    if (m_search[0] != '\0' && m_sectionsDrawn == 0) {
        ImGui::TextDisabled("no component matches \"%s\"", m_search);
    }
}

// INSP-SECTIONS 日志（去抖）：当前处于展开状态的分区清单。
// 供 tools/verify_inspector.py 断言"折叠状态跨实体保持"。
void InspectorPanel::logSectionState() {
    std::string sig;
    for (const auto& kv : m_sectionOpen) {
        if (kv.second) {
            sig += kv.first;
            sig += '|';
        }
    }
    if (sig == m_lastSectionSig) return;
    m_lastSectionSig = sig;
    core::logMessage(core::LogLevel::Info, "INSP-SECTIONS open=%s",
                     sig.empty() ? "(none)" : sig.c_str());
}

// ------------------------------------------------------------ E11 剪贴板

bool InspectorPanel::copySection(const char* name, ecs::Entity e) {
    ecs::World& w = m_ctx.activeScene().world();
    const PodKind k = podKindOf(name, w, e);
    if (k == PK_None) return false;
    const void* src = podPtr(k, w, e);
    const std::size_t n = podSize(k);
    if (!src || n == 0) return false;

    m_clipKind = static_cast<int>(k);
    m_clipSection = name;
    m_clipBytes.assign(static_cast<const std::uint8_t*>(src),
                       static_cast<const std::uint8_t*>(src) + n);
    m_ctx.setStatus(std::string("Copy ") + name);
    return true;
}

bool InspectorPanel::pasteSection(const char* name, ecs::Entity e) {
    if (!m_ctx.isEditing()) return false;
    if (m_clipSection != name || m_clipBytes.empty()) return false;

    ecs::World& w = m_ctx.activeScene().world();
    const PodKind k = podKindOf(name, w, e);
    if (k == PK_None || static_cast<int>(k) != m_clipKind) return false;
    if (m_clipBytes.size() != podSize(k)) return false;

    void* dst = podPtr(k, w, e);
    if (!dst) return false;

    const std::size_t n = podSize(k);
    std::vector<std::uint8_t> before(n);
    std::memcpy(before.data(), dst, n);
    std::memcpy(dst, m_clipBytes.data(), n);

    m_ctx.commands().pushAlreadyApplied(std::make_unique<RawBytesEditCommand>(
        std::string("Paste ") + name, dst, before.data(), m_clipBytes.data(), n));
    m_ctx.setStatus(std::string("Paste ") + name);
    m_ctx.dirty() = true;
    return true;
}

// Reset to Default：用**类型化赋值**回到构造默认（聚光灯含 std::string，
// 不能走 memcpy，所以这里按具体类型写，而不是复用 podPtr 的字节路径）。
bool InspectorPanel::resetSection(const char* name, ecs::Entity e) {
    if (!m_ctx.isEditing()) return false;
    ecs::World& w = m_ctx.activeScene().world();

    // 值为默认 → 直接跳过（不污染撤销栈）
    auto apply = [&](auto* comp, const auto& def) -> bool {
        if (!comp) return false;
        if (std::memcmp(comp, &def, sizeof(def)) == 0) {
            m_ctx.setStatus(std::string("Reset ") + name + " (already default)");
            return true;
        }
        auto before = std::make_unique<std::remove_pointer_t<decltype(comp)>>(*comp);
        *comp = def;
        m_ctx.commands().pushAlreadyApplied(std::make_unique<RawBytesEditCommand>(
            std::string("Reset ") + name, comp, before.get(), comp,
            sizeof(std::remove_pointer_t<decltype(comp)>)));
        m_ctx.setStatus(std::string("Reset ") + name);
        m_ctx.dirty() = true;
        return true;
    };

    if (std::strcmp(name, "Transform") == 0) {
        return apply(w.get<ecs::TransformComponent>(e), ecs::TransformComponent{});
    }
    if (std::strcmp(name, "Visibility") == 0) {
        return apply(w.get<ecs::VisibilityComponent>(e), ecs::VisibilityComponent{});
    }
    if (std::strcmp(name, "Particle Emitter") == 0) {
        return apply(w.get<ecs::ParticleEmitterComponent>(e),
                     ecs::ParticleEmitterComponent{});
    }
    if (std::strcmp(name, "Light") == 0) {
        if (auto* dl = w.get<ecs::DirectionalLightComponent>(e))
            return apply(dl, ecs::DirectionalLightComponent{});
        if (auto* pl = w.get<ecs::PointLightComponent>(e))
            return apply(pl, ecs::PointLightComponent{});
        if (auto* sl = w.get<ecs::SpotLightComponent>(e))
            return apply(sl, ecs::SpotLightComponent{});
    }
    return false;
}

// ---- E11 自检（Console `insp.selftest`）----
bool InspectorPanel::selfTest(std::string& out) {
    scene::Scene& sc = m_ctx.activeScene();
    ecs::Entity e = m_ctx.selection();
    if (!e.valid()) {
        out = "FAIL no-selection";
        return false;
    }

    // ① Copy → 改值 → Paste 必须回到原值（往返一致）
    auto* t = sc.world().get<ecs::TransformComponent>(e);
    if (!t) {
        out = "FAIL no-transform";
        return false;
    }
    const ecs::TransformComponent orig = *t;
    if (!copySection("Transform", e)) {
        out = "FAIL copy";
        return false;
    }
    t->position += glm::vec3(7.0f, 7.0f, 7.0f);
    if (!pasteSection("Transform", e)) {
        out = "FAIL paste";
        return false;
    }
    const bool roundtrip = std::memcmp(&orig, t, sizeof(orig)) == 0;
    const char* roundtripS = roundtrip ? "OK" : "MISMATCH";

    // ② Reset to Default 必须回到构造默认
    t->position = glm::vec3(9.0f, 9.0f, 9.0f);
    t->scale = glm::vec3(3.0f);
    if (!resetSection("Transform", e)) {
        out = "FAIL reset";
        return false;
    }
    const ecs::TransformComponent def;
    const bool resetOk = std::memcmp(&def, t, sizeof(def)) == 0;
    const char* resetS = resetOk ? "OK" : "MISMATCH";

    // ③ 折叠状态按名持久化（面板级 map：与当前实体无关）
    m_sectionOpen["__selftest__"] = false;
    const bool persist = (m_sectionOpen.count("__selftest__") == 1) &&
                         (m_sectionOpen["__selftest__"] == false);
    m_sectionOpen.erase("__selftest__");

    out = std::string("copy-paste=") + roundtripS + " reset=" + resetS +
          " collapse-persist=" + (persist ? "OK" : "FAIL");
    return roundtrip && resetOk && persist;
}

// ---------------------------------------------------------------- 兜底视图

void InspectorPanel::drawSceneryFallback() {
    ImGui::TextDisabled("Nothing selected.");
    hint("Click an object in the viewport (or in the Hierarchy) to edit it.");
    ImGui::Separator();

    scene::Scene& sc = m_ctx.editorScene();

    // 相机与主光不进撤销栈：视口导航本来就是"看一眼就过去了"的操作，
    // 把它们塞进历史只会把有用的历史挤掉。
    if (sectionHeader("Camera", true)) {
        scene::Camera& cam = sc.camera();
        glm::vec3 t = cam.target();
        if (ImGui::DragFloat3("Target", glm::value_ptr(t), sp(0.05f))) {
            cam.setTarget(t);
        }
        float dist = cam.distance();
        if (ImGui::DragFloat("Distance", &dist, sp(0.1f), 0.5f, 200.0f)) {
            cam.setDistance(dist);
        }
        float fov = cam.fov();
        if (ImGui::SliderFloat("FOV", &fov, 15.0f, 110.0f)) cam.setFov(fov);

        // yaw / pitch 是角度，给个直观的滑块
        float yawDeg = glm::degrees(cam.yaw());
        if (ImGui::SliderFloat("Yaw", &yawDeg, -180.0f, 180.0f, "%.1f deg")) {
            cam.setYaw(glm::radians(yawDeg));
        }
        float pitchDeg = glm::degrees(cam.pitch());
        if (ImGui::SliderFloat("Pitch", &pitchDeg, -85.0f, 85.0f, "%.1f deg")) {
            cam.setPitch(glm::radians(pitchDeg));
        }
    }

    if (sectionHeader("Directional Light", true)) {
        scene::Light& L = sc.light();
        const glm::vec3 db = L.direction;
        ImGui::DragFloat3("Direction", glm::value_ptr(L.direction), sp(0.01f), -1.0f,
                          1.0f);
        discreteEdit(m_ctx, "Sun Direction", &L.direction, db);

        const glm::vec3 cb = L.color;
        ImGui::ColorEdit3("Color", glm::value_ptr(L.color));
        discreteEdit(m_ctx, "Sun Color", &L.color, cb);

        const float ib = L.intensity;
        ImGui::SliderFloat("Intensity", &L.intensity, 0.0f, 4.0f);
        discreteEdit(m_ctx, "Sun Intensity", &L.intensity, ib);

        const float ab = L.ambientScale;
        ImGui::SliderFloat("Ambient", &L.ambientScale, 0.0f, 1.0f, "%.2f");
        discreteEdit(m_ctx, "Ambient Scale", &L.ambientScale, ab);
        hint("Low ambient = indoor mood; 0.55 is a good outdoor default");
    }

    if (sectionHeader("Scene")) {
        ImGui::Text("Entities: %zu", sc.objectCount());
        const scene::Scene::LightStats ls = sc.lightStats();
        ImGui::Text("Local lights: %zu (%zu point / %zu spot)",
                    ls.pointLights + ls.spotLights, ls.pointLights,
                    ls.spotLights);
        ImGui::Text("Editor commands: %zu (cursor %zu)",
                    m_ctx.commands().depth(), m_ctx.commands().cursor());
    }
}

// ---------------------------------------------------------------- 变换

void InspectorPanel::drawTransform(ecs::Entity e) {
    scene::Scene& sc = m_ctx.activeScene();
    auto* t = sc.world().get<ecs::TransformComponent>(e);
    if (!t) return;

    if (!sectionHeader("Transform", true, e))
        return;

    const std::string* np = sc.world().name(e);
    const std::string who = np ? *np : std::string("Object");

    // ---- 锁定开关：上锁 = 视口点不到 + 不给变换手柄 ----
    // 引擎自带的地面默认就是锁着的（它是参照系，不该被平移/缩放/旋转）。
    // 想调整地面，先在这里把勾去掉 —— 所以这个开关是"出口"，不能省。
    const bool locked = sc.world().has<ecs::LockedComponent>(e);
    ImGui::BeginDisabled(!m_ctx.isEditing());
    bool lockFlag = locked;
    if (ImGui::Checkbox("Locked", &lockFlag) && m_ctx.isEditing()) {
        const char* label = lockFlag ? "Lock " : "Unlock ";
        m_ctx.structuralEdit(std::string(label) + who, [&] {
            if (lockFlag) {
                sc.world().add<ecs::LockedComponent>(e);
            } else {
                sc.world().remove<ecs::LockedComponent>(e);
            }
        });
        m_ctx.setStatus(std::string(lockFlag ? "Locked " : "Unlocked ") + who);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("上锁 = 视口点不到 + 不给变换手柄（自带地面默认锁着）。\n"
                          "要调整地面就先取消这个勾。");
    }
    ImGui::SameLine();
    ImGui::TextDisabled(locked ? "(fixed, cannot be picked or transformed)"
                               : "(unlocked)");
    ImGui::EndDisabled();

    ImGui::BeginDisabled(locked);

    // 位置
    const glm::vec3 pb = t->position;
    ImGui::SetNextItemWidth(-90.0f);
    ImGui::DragFloat3("Position", glm::value_ptr(t->position), sp(0.02f));
    fieldEdit(m_ctx, ("Move " + who).c_str(), &t->position, pb);
    ImGui::SameLine();
    if (ImGui::SmallButton("R##pos") && m_ctx.isEditing()) {
        const glm::vec3 zero(0.0f);
        m_ctx.commands().pushAlreadyApplied(
            std::make_unique<RawBytesEditCommand>("Reset Position",
                                                  &t->position, &t->position,
                                                  &zero, sizeof(glm::vec3)));
        m_ctx.dirty() = true;
    }

    // 旋转（内部是弧度，界面上用角度）
    const glm::vec3 rb = t->rotation;
    glm::vec3 rotDeg = glm::degrees(rb);
    ImGui::SetNextItemWidth(-90.0f);
    if (ImGui::DragFloat3("Rotation", glm::value_ptr(rotDeg), sp(0.5f))) {
        t->rotation = glm::radians(rotDeg);
    }
    fieldEdit(m_ctx, ("Rotate " + who).c_str(), &t->rotation, rb);
    ImGui::SameLine();
    if (ImGui::SmallButton("R##rot") && m_ctx.isEditing()) {
        const glm::vec3 zero(0.0f);
        m_ctx.commands().pushAlreadyApplied(
            std::make_unique<RawBytesEditCommand>("Reset Rotation", &t->rotation,
                                                  &t->rotation, &zero,
                                                  sizeof(glm::vec3)));
        m_ctx.dirty() = true;
    }

    // 缩放
    const glm::vec3 sb = t->scale;
    ImGui::SetNextItemWidth(-90.0f);
    ImGui::DragFloat3("Scale", glm::value_ptr(t->scale), sp(0.01f), 0.001f, 100.0f);
    fieldEdit(m_ctx, ("Scale " + who).c_str(), &t->scale, sb);
    ImGui::SameLine();
    if (ImGui::SmallButton("R##scl") && m_ctx.isEditing()) {
        const glm::vec3 one(1.0f);
        m_ctx.commands().pushAlreadyApplied(
            std::make_unique<RawBytesEditCommand>("Reset Scale", &t->scale,
                                                  &t->scale, &one,
                                                  sizeof(glm::vec3)));
        m_ctx.dirty() = true;
    }

    // 世界坐标（只读）：有父级时最容易看清"局部 vs 世界"
    const glm::mat4 wm = sc.worldMatrix(e);
    const glm::vec3 wp = glm::vec3(wm[3]);
    ImGui::TextDisabled("world position: (%.3f, %.3f, %.3f)", wp.x, wp.y, wp.z);

    ImGui::EndDisabled();
    if (locked) {
        ImGui::SameLine();
        ImGui::TextDisabled("(locked)");
    }
}

void InspectorPanel::drawVisibility(ecs::Entity e) {
    scene::Scene& sc = m_ctx.activeScene();
    auto* v = sc.world().get<ecs::VisibilityComponent>(e);
    if (!v) return;

    if (!sectionHeader("Visibility", true, e))
        return;

    bool visible = v->visible;
    if (ImGui::Checkbox("Visible", &visible)) {
        v->visible = visible;
        discreteEdit(m_ctx, "Toggle Visible", &v->visible, !visible);
    }
    ImGui::SameLine();
    bool shadow = v->castShadow;
    if (ImGui::Checkbox("Cast Shadow", &shadow)) {
        v->castShadow = shadow;
        discreteEdit(m_ctx, "Toggle Cast Shadow", &v->castShadow, !shadow);
    }
}

// ---------------------------------------------------------------- 网格
//
// 这一段抄的是 Blender 的 Object Data Properties 的组织方式：
//
//   · 上半 = **只读的资产信息**（它是什么、从哪来、多少几何）。
//     这些值由文件或工厂决定，改不了，所以画成普通文本而不是控件 ——
//     做成灰掉的输入框只会让人以为"是不是我没权限"。
//   · 下半 = **能改的参数**，而且按资产类型分化：
//       内置图元 → 生成参数（边长 / 半径 / 分段数），松开鼠标就重建几何
//       OBJ/glTF → 一个可改项都没有，明确说一句"几何由文件决定"
//     这就是需求里"不同的 mesh 其属性参数不同"的落点。

void InspectorPanel::drawMeshAndMaterial(ecs::Entity e) {
    drawMeshSection(e);
    drawMaterialSection(e);
    drawCollisionSection(e);
    drawDecalSection(e);
    drawReflectionProbeSection(e);
    drawParticleEmitterSection(e);
}

// ---------------------------------------------------------------- 碰撞体
//
// 从 2026-10-10 起碰撞体是可以加到任意物体上的（视口右键菜单：
// Add Collision Body → Convex Hull / Capsule）。这一段负责"加完之后还能改"：
// 换形状、单独摆位、调胶囊尺寸、关掉 solid、移除。
//
// 为什么把"手柄编辑碰撞体"的开关也放这儿：视口里点碰撞框也能切过去，但
// 那要求碰撞框在屏幕上看得见、点得准。面板上的勾选框是那条路走不通时的
// 兜底（也是"怎么看都回不到编辑物体"的唯一出口）。
void InspectorPanel::drawCollisionSection(ecs::Entity e) {
    scene::Scene& sc = m_ctx.editorScene();
    ecs::World& w = sc.world();

    auto* cc = w.get<ecs::CollisionComponent>(e);
    if (!cc) return;

    const bool open = sectionHeader("Collision", false, e);
    logRect("INS-COLLISION-HEADER", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    if (!open) return;

    if (!m_ctx.isEditing()) {
        ImGui::TextDisabled("run mode - colliders are read-only here");
        return;
    }

    // ---- 形状切换：换完按网格重建几何（位置/旋转/缩放保留）----
    int shape = static_cast<int>(cc->shape);
    const char* kShapes[] = {"Convex Hull", "Capsule"};
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::Combo("Shape", &shape, kShapes, 2)) {
        const auto next = static_cast<ecs::ColliderShape>(shape);
        const auto prev = cc->shape;
        if (next != prev) {
            m_ctx.structuralEdit(
                next == ecs::ColliderShape::ConvexHull
                    ? "Collider -> Convex Hull"
                    : "Collider -> Capsule",
                [&]() {
                    auto* c2 = w.get<ecs::CollisionComponent>(e);
                    const auto* mc = w.get<ecs::MeshComponent>(e);
                    if (!c2 || !mc || !mc->mesh) return;
                    if (next == ecs::ColliderShape::ConvexHull) {
                        physics::setupHullCollider(*mc->mesh, *c2);
                    } else {
                        physics::setupCapsuleCollider(*mc->mesh, *c2);
                    }
                });
        }
    }

    // ---- 手柄目标开关 ----
    bool editColl = m_ctx.colliderEdit();
    if (ImGui::Checkbox("Edit with W/E/R", &editColl)) {
        m_ctx.setColliderEdit(editColl);
        // 自动化需要"编辑态到底切没切上"的回执 —— 勾选框点击在这台机器
        // 上偶发落空（坑点 #32），盲目重试 toggle 会把状态勾回去。
        if (std::getenv("MYVK_LOG_RECTS"))
            VK_LOG_INFO("INS-COLLIDER-EDIT-SET %d", editColl ? 1 : 0);
    }
    logRect("INS-COLLIDER-EDIT", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    ImGui::SameLine();
    bool solid = cc->solid;
    if (ImGui::Checkbox("Solid", &solid)) {
        cc->solid = solid;   // discreteEdit 的契约：*target 已持有新值
        discreteEdit(m_ctx, "Collider Solid", &cc->solid, !solid);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Solid = blocks other colliders and the camera.\n"
                          "Off = a pure trigger (still visible, still blocks "
                          "the camera while flying through it).");

    // ---- 线框显隐（默认显示；W/E/R 编辑中无论如何都强制显示）----
    bool showWire = cc->showWireframe;
    if (ImGui::Checkbox("Show Wireframe", &showWire)) {
        cc->showWireframe = showWire;   // discreteEdit 只压撤销栈，不赋值
        discreteEdit(m_ctx, "Collider Wireframe", &cc->showWireframe,
                     !showWire);
    }
    logRect("INS-COLLIDER-SHOW", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Show/hide the collision wireframe in the viewport.\n"
                          "Always visible while editing with W/E/R.");

    // ---- 摆放（相对实体）----
    const glm::vec3 p0 = cc->position;
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::DragFloat3("##collider-pos", glm::value_ptr(cc->position), sp(0.01f));
    fieldEdit(m_ctx, "Collider Position", &cc->position, p0);
    ImGui::SetItemTooltip("Position (relative to the object)");

    const glm::vec3 r0 = cc->rotation;
    ImGui::SetNextItemWidth(-1.0f);
    glm::vec3 rotDeg = glm::degrees(cc->rotation);
    if (ImGui::DragFloat3("##collider-rot", glm::value_ptr(rotDeg), sp(0.5f))) {
        cc->rotation = glm::radians(rotDeg);
    }
    fieldEdit(m_ctx, "Collider Rotation", &cc->rotation, r0);
    ImGui::SetItemTooltip("Rotation (degrees)");

    const glm::vec3 s0 = cc->scale;
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::DragFloat3("##collider-scale", glm::value_ptr(cc->scale), sp(0.01f),
                      0.01f, 100.0f);
    fieldEdit(m_ctx, "Collider Scale", &cc->scale, s0);
    ImGui::SetItemTooltip("Scale (uniform is not required)");

    // ---- 胶囊尺寸 ----
    if (cc->shape == ecs::ColliderShape::Capsule) {
        const float rad0 = cc->capsuleRadius;
        ImGui::SetNextItemWidth(140.0f);
        ImGui::SliderFloat("Radius", &cc->capsuleRadius, 0.01f, 10.0f, "%.3f");
        fieldEdit(m_ctx, "Collider Radius", &cc->capsuleRadius, rad0);

        const float hh0 = cc->capsuleHalfHeight;
        ImGui::SetNextItemWidth(140.0f);
        ImGui::SliderFloat("Half Height", &cc->capsuleHalfHeight, 0.0f, 10.0f,
                           "%.3f");
        fieldEdit(m_ctx, "Collider Half Height", &cc->capsuleHalfHeight, hh0);
    } else {
        ImGui::TextDisabled("convex hull: %zu points / %zu edges",
                            cc->hullPoints.size(), cc->hullEdges.size());
    }

    if (ImGui::Button("Remove Collision Body")) {
        m_ctx.structuralEdit("Remove Collision Body", [&]() {
            w.remove<ecs::CollisionComponent>(e);
        });
        m_ctx.setColliderEdit(false);
    }
}

// ---------------------------------------------------------------- 延迟贴花
//
// 贴花（R4-4）和网格 / 材质是三种不同的东西，所以它单独立一段而不是塞进
// 材质里：
//
//   · 它**没有网格**。渲染时画的是一个盒体体积（单位立方体 × 世界矩阵），
//     片元再按深度重建出的场景表面坐标决定"这个像素要不要贴"。所以
//     Mesh 段对贴花是空的，而这里必须自己给出盒体尺寸。
//   · 位置 / 朝向**复用实体的 Transform** —— 拖贴花与拖物体共用同一套
//     手柄、同一套撤销、同一套父链。这一段刻意不再提供 position/rotation
//     控件，否则"两个地方都能改位置"迟早会打架。
//   · 两张贴图复用材质区那个纹理槽控件：同一条拖放路径、同一个缩略图
//     缓存、同样的 sRGB 规则（反照率要 sRGB 解码，法线必须保持线性）。
//     槽位名前缀是 decal-，日志 tag 与矩形 tag 都跟着变（见 drawTextureChip）。
//
// 全局开关（MYVK_DECALS / ScreenSpaceSettings::decals）在渲染器里 ——
// 关着时下面这些参数照存不误，但画面上不会出现任何贴花，所以顶部给一句
// 提示，免得"我调了参数怎么没反应"。
void InspectorPanel::drawDecalSection(ecs::Entity e) {
    scene::Scene& sc = m_ctx.editorScene();
    ecs::World& w = sc.world();

    auto* dc = w.get<ecs::DecalComponent>(e);
    if (!dc) return;

    const bool open = sectionHeader("Decal", false, e);
    logRect("INS-DECAL-HEADER", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    if (!open) return;

    if (!m_ctx.isEditing()) {
        ImGui::TextDisabled("run mode - decals are read-only here");
        return;
    }

    std::string labels;   // 本帧画出的参数名（喂给 DECAL-PARAMS 签名日志）

    // ---- 开关 ----
    bool en = dc->enabled;
    if (ImGui::Checkbox("Enabled##decal", &en)) {
        dc->enabled = en;   // discreteEdit 的契约：*target 已持有新值
        discreteEdit(m_ctx, "Toggle Decal", &dc->enabled, !en);
    }
    logRect("INS-DECAL-ENABLED", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Off = the decal is not collected at all.\n"
                          "The global switch (MYVK_DECALS) is separate - with "
                          "it off\nno decal is drawn no matter what this says.");
    }
    labels = "Enabled";

    // ---- 盒体尺寸（米，沿实体局部轴）----
    const glm::vec3 sz0 = dc->size;
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::DragFloat3("Size", glm::value_ptr(dc->size), sp(0.01f), 1e-4f, 500.0f,
                      "%.3f");
    fieldEdit(m_ctx, "Decal Size", &dc->size, sz0);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Box volume in metres, along the object's own axes.\n"
                          "The decal only shows where the scene surface is "
                          "inside it.");
    }
    labels += ",Size";

    // ---- 覆盖度 ----
    const float op0 = dc->opacity;
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::SliderFloat("Opacity", &dc->opacity, 0.0f, 1.0f, "%.2f");
    fieldEdit(m_ctx, "Decal Opacity", &dc->opacity, op0);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Upper bound on the coverage written to the "
                          "DBuffer.\n0 = invisible (the pass still runs - it "
                          "is the baseline knob).");
    }
    labels += ",Opacity";

    const glm::vec3 t0 = dc->tint;
    ImGui::ColorEdit3("Tint", glm::value_ptr(dc->tint),
                      ImGuiColorEditFlags_NoInputs);
    fieldEdit(m_ctx, "Decal Tint", &dc->tint, t0);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Multiplied with the albedo map - lets one texture "
                          "serve as several markers.");
    }
    labels += ",Tint";

    // ---- 贴花自己的 PBR 参数（按 coverage 覆盖被贴表面）----
    const float rg0 = dc->roughness;
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::SliderFloat("Roughness", &dc->roughness, 0.02f, 1.0f, "%.2f");
    fieldEdit(m_ctx, "Decal Roughness", &dc->roughness, rg0);
    labels += ",Roughness";

    const float mt0 = dc->metallic;
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::SliderFloat("Metallic", &dc->metallic, 0.0f, 1.0f, "%.2f");
    fieldEdit(m_ctx, "Decal Metallic", &dc->metallic, mt0);
    labels += ",Metallic";

    const float ns0 = dc->normalScale;
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::SliderFloat("Normal Scale", &dc->normalScale, 0.0f, 2.0f, "%.2f");
    fieldEdit(m_ctx, "Decal Normal Scale", &dc->normalScale, ns0);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("0 = the decal leaves the surface normal alone,\n"
                          "1 = the normal map is used as authored.");
    }
    labels += ",NormalScale";

    // ---- 淡出 ----
    const float ef0 = dc->edgeFade;
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::SliderFloat("Edge Fade", &dc->edgeFade, 0.0f, 0.5f, "%.2f");
    fieldEdit(m_ctx, "Decal Edge Fade", &dc->edgeFade, ef0);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Soft border, as a fraction of the half extents.\n"
                          "0 gives a hard edge -> a visible seam where the box "
                          "cuts the surface.");
    }
    labels += ",EdgeFade";

    const float fs0 = dc->fadeStart;
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::DragFloat("Fade Start", &dc->fadeStart, sp(0.05f), 0.0f, 1000.0f,
                     "%.2f");
    fieldEdit(m_ctx, "Decal Fade Start", &dc->fadeStart, fs0);

    const float fe0 = dc->fadeEnd;
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::DragFloat("Fade End", &dc->fadeEnd, sp(0.05f), 0.0f, 1000.0f, "%.2f");
    fieldEdit(m_ctx, "Decal Fade End", &dc->fadeEnd, fe0);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Distance fade, in metres from the camera to the "
                          "decal centre.\nEnd <= Start disables it (the "
                          "default) - that is the pixel-identical path.");
    }
    labels += ",FadeStart,FadeEnd";

    ImGui::TextDisabled("fade: camera %.1f m .. %.1f m %s",
                        static_cast<double>(dc->fadeStart),
                        static_cast<double>(dc->fadeEnd),
                        dc->fadeEnd <= dc->fadeStart ? "(off)" : "");

    // ---- 两张贴图（复用材质区的纹理槽控件）----
    // 空槽 = 纯色方块：albedo 走缺省白图（alpha=255，于是 coverage 只由
    // opacity 决定）、normal 走缺省平坦法线（不扰动场景法线）。
    assets::Texture* const ab0 = dc->albedo;
    assets::Texture* const nm0 = dc->normal;

    auto texRow = [&](const char* label, const char* slotId, const char* undo,
                      const char* tip, assets::Texture** slot,
                      assets::Texture* before, bool srgb) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(0.0f, 8.0f);
        const float avail =
            ImGui::GetWindowContentRegionMax().x - ImGui::GetCursorPosX() - 4.0f;
        if (drawTextureChip(slotId, tip, slot, srgb, avail, "DECAL-SLOT"))
            discreteEdit(m_ctx, undo, slot, before);
    };

    texRow("Albedo", "decal-albedo", "Decal Albedo",
           "Decal colour map (sRGB). Empty = a plain square of Tint.",
           &dc->albedo, ab0, /*srgb=*/true);
    labels += ",Albedo";

    texRow("Normal", "decal-normal", "Decal Normal",
           "Tangent-space normal map, kept linear. Empty = flat normal\n"
           "(the decal only changes colour, not shading).",
           &dc->normal, nm0, /*srgb=*/false);
    labels += ",Normal";

    hint("needs the global switch: MYVK_DECALS=1 (off by default - then the "
         "pass is not even recorded)");

    // ---- 这一帧画了哪些参数 ----
    // 与 MESH-PARAMS / LIGHT-PARAMS 同一套路：让"贴花段真的有这些控件"
    // 这件事可被自动化断言，不用去截图里认控件。只在变化时打一行。
    if (labels != m_lastDecalParamSig) {
        m_lastDecalParamSig = labels;
        const std::string* nm = w.name(e);
        VK_LOG_INFO("DECAL-PARAMS entity=\"%s\" labels=%s",
                    nm ? nm->c_str() : "?", labels.c_str());
    }

    if (ImGui::Button("Remove Decal")) {
        m_ctx.structuralEdit("Remove Decal", [&]() {
            w.remove<ecs::DecalComponent>(e);
        });
        m_lastDecalParamSig.clear();
    }
}

// ---------------------------------------------------------------- 反射探针（R5-1）
//
// 与贴花段同一形态：
//   · 位置 / 朝向**复用实体的 Transform** —— 拖探针与拖物体共用同一套
//     手柄、同一套撤销、同一套父链。所以这里不提供 position/rotation。
//   · 盒体半轴 extents 会再乘上实体的 scale（放大实体 = 放大作用范围），
//     与"拖大一个盒子"的直觉一致。
//
// 与贴花最大的不同：**探针有一个烘焙产物**（立方体贴图）。它不是每帧
// 重算的，而是"数据变了 / 按了 Capture"才重烘一次，所以这里有一个
// Capture 按钮（走 EditorContext → EditorApp → Renderer 的请求链，与
// 角色详情窗口同一个套路 —— 本面板拿不到 Renderer）。
//
// 全局开关（MYVK_PROBES / probe.enabled，默认关）在渲染器 / Engine 面板里。
// 关着时下面这些参数照存不误，但一次捕获都不会发生，所以给一句提示。
void InspectorPanel::drawReflectionProbeSection(ecs::Entity e) {
    scene::Scene& sc = m_ctx.editorScene();
    ecs::World& w = sc.world();

    auto* pc = w.get<ecs::ReflectionProbeComponent>(e);
    if (!pc) return;

    const bool open = sectionHeader("Reflection Probe", false, e);
    logRect("INS-PROBE-HEADER", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    if (!open) return;

    if (!m_ctx.isEditing()) {
        ImGui::TextDisabled("run mode - probes are read-only here");
        return;
    }

    std::string labels;   // 本帧画出的参数名（喂给 PROBE-PARAMS 签名日志）

    bool en = pc->enabled;
    if (ImGui::Checkbox("Enabled##probe", &en)) {
        pc->enabled = en;   // discreteEdit 的契约：*target 已持有新值
        discreteEdit(m_ctx, "Toggle Reflection Probe", &pc->enabled, !en);
    }
    logRect("INS-PROBE-ENABLED", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Off = the probe is not collected at all (and its\n"
                          "box no longer captures anything). The global switch\n"
                          "(MYVK_PROBES) is separate and off by default.");
    }
    labels = "Enabled";

    // ---- 盒体半轴（米，沿实体局部轴，再乘实体 scale）----
    const glm::vec3 ex0 = pc->extents;
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::DragFloat3("Extents", glm::value_ptr(pc->extents), sp(0.05f), 1e-3f,
                      1000.0f, "%.2f");
    fieldEdit(m_ctx, "Probe Extents", &pc->extents, ex0);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Half extents of the box in metres, along the "
                          "object's own axes.\nOnly surfaces INSIDE this box "
                          "sample the probe.\nScaled by the object's scale, so "
                          "scaling the object scales its reach.");
    }
    labels += ",Extents";

    const float in0 = pc->intensity;
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::SliderFloat("Intensity", &pc->intensity, 0.0f, 4.0f, "%.2f");
    fieldEdit(m_ctx, "Probe Intensity", &pc->intensity, in0);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Multiplier on this probe's specular contribution.\n"
                          "0 = as if the probe were not there (the capture "
                          "still runs).");
    }
    labels += ",Intensity";

    bool bp = pc->boxProjection;
    if (ImGui::Checkbox("Box Projection##probe", &bp)) {
        pc->boxProjection = bp;
        discreteEdit(m_ctx, "Toggle Box Projection", &pc->boxProjection, !bp);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Project the reflection onto the box walls before "
                          "sampling.\nOn (default) a probe beside a flat wall "
                          "gives correct parallax;\noff squeezes the whole "
                          "room into one direction - fine for a ball,\nwrong "
                          "for a floor.");
    }
    labels += ",BoxProjection";

    const float bd0 = pc->blendDistance;
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::SliderFloat("Blend Distance", &pc->blendDistance, 0.01f, 20.0f,
                       "%.2f");
    fieldEdit(m_ctx, "Probe Blend Distance", &pc->blendDistance, bd0);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Width of the fade band just inside the box border,\n"
                          "in metres. Without it two neighbouring probes meet "
                          "at a hard seam.");
    }
    labels += ",BlendDistance";

    // ---- Capture ----
    // 只置一个请求标志：本面板拿不到 Renderer，由 EditorApp 每帧取走
    // 转交（与 requestCharacterWindow 同一个套路）。
    ImGui::Separator();
    if (ImGui::Button("Capture Now")) {
        m_ctx.requestProbeCapture();
        m_ctx.notify("Reflection probe: recapture requested");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Re-render all six cube faces from this probe's "
                          "position and\nre-bake the GGX prefilter chain.\n"
                          "Normally you do not need this: the capture reruns\n"
                          "automatically whenever the probe data changes.");
    }
    logRect("INS-PROBE-CAPTURE", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());

    hint("needs the global switch: MYVK_PROBES=1 (off by default - then no "
         "capture is recorded at all)");
    // 第二行是"为什么我开了探针却什么都没看到"的头号答案：pbr.frag 里探针
    // 那段在 `ubo.envParams.x > 0.5`（= IBL 总开关）**内部**，因为探针替换
    // 的就是 IBL 的镜面预过滤项、菲涅尔/BRDF 表也来自那一路。语义上说得通
    // （"关掉环境光"就包含探针），但对使用者是个坑 —— 必须写出来。
    hint("also needs IBL: MYVK_IBL=1 (on by default - the probe replaces the "
         "IBL specular term, so with IBL off it has nothing to replace)");

    if (labels != m_lastProbeParamSig) {
        m_lastProbeParamSig = labels;
        const std::string* nm = w.name(e);
        VK_LOG_INFO("PROBE-PARAMS entity=\"%s\" labels=%s",
                    nm ? nm->c_str() : "?", labels.c_str());
    }

    if (ImGui::Button("Remove Reflection Probe")) {
        m_ctx.structuralEdit("Remove Reflection Probe", [&]() {
            w.remove<ecs::ReflectionProbeComponent>(e);
        });
        m_lastProbeParamSig.clear();
    }
}

// 颜色渐变带编辑器（Colour over Lifetime 的核心控件）。
//
// Unity 是一条能加停靠点的色带，Blender 是 Color Ramp。这里做两者的共同
// 子集：**顶部一条预览带 + 每行一个停靠点（颜色方块 / 位置滑条 / 删除）+
// 底部 Add stop**。预设的三段"橙→红→暗红"就是加的 3 个停靠点。
//
// 三个必须点名的实现细节：
//
// ① **拖中不排序**。ramp.sort() 只在松手时（commit）和增删时调用。
//    每帧排序会让"第 i 行"在拖动过程中换成另一个停靠点，鼠标拖着的值
//    会突然跳到别的颜色上 —— 这是色带控件最经典的坑。
//
// ② 求值/预览用**已排序的局部队本**（view），行控件则用**原数组**（ramp）。
//    view 保证色带在拖动中的颜色是连续的；ramp 保证行号↔停靠点的对应在
//    拖动中是稳定的。
//
// ③ 撤销记整条带子而不是单个停靠点 —— 理由见下面的注释。
// 颜色渐变带编辑器（Colour over Lifetime 的核心控件）。
//
// Unity 是一条能加停靠点的色带，Blender 是 Color Ramp。这里做两者的共同
// 子集：**顶部一条预览带 + 每行一个停靠点（颜色方块 / 位置滑条 / 删除）+
// 底部 Add stop**。预设的三段"橙→红→暗红"就是加的 3 个停靠点。
//
// 三个必须点名的实现细节：
//
// ① **拖中不排序**。ramp.sort() 只在松手时（commit）和增删时调用。
//    每帧排序会让"第 i 行"在拖动过程中换成另一个停靠点，鼠标拖着的值
//    会突然跳到别的颜色上 —— 这是色带控件最经典的坑。
//
// ② 求值/预览用**已排序的局部队本**（view），行控件则用**原数组**（ramp）。
//    view 保证色带在拖动中的颜色是连续的；ramp 保证行号↔停靠点的对应在
//    拖动中是稳定的。
//
// ③ 撤销记整条带子而不是单个停靠点 —— 理由见下面的注释。
bool InspectorPanel::drawColorRampEditor(const char* id,
                                         ecs::ParticleColorRamp& ramp) {
    bool changed = false;

    // 序列化层可能读进越界或乱序的 count，先夹住再干活。
    if (ramp.count < 1) ramp.count = 1;
    if (ramp.count > ecs::kMaxColorStops) ramp.count = ecs::kMaxColorStops;

    ImGui::PushID(id);

    // ------------------------------------------------------------ 撤销记账
    //
    // ⚠ 记的是**整条带子**（4 + 8x16 = 132 字节的 POD），不是"第 i 个
    //   停靠点"。原因：拖动停靠点之后会重排序，目标指针 &stops[i] 在重排
    //   之后就指向另一个停靠点了，撤销会把值写回错误的位置。整条快照既
    //   没有这个坑，语义也更自然 —— "拖了一下色带"就是一条历史。
    //   增删停靠点同样走整条快照（structuralEdit），这样"删掉第 5 个点"
    //   一次撤销就能回来。
    auto arm = [&](const ecs::ParticleColorRamp& before) {
        if (!m_ctx.isEditing()) return;
        if (!ImGui::IsItemActivated()) return;
        m_armTarget = &ramp;
        m_armSize = sizeof(ecs::ParticleColorRamp);
        m_armName = std::string("Colour Ramp (") + id + ")";
        m_armBefore.assign(
            reinterpret_cast<const std::uint8_t*>(&before),
            reinterpret_cast<const std::uint8_t*>(&before) + sizeof(before));
    };
    auto commit = [&]() {
        if (!m_ctx.isEditing()) return;
        if (m_armTarget != &ramp) return;
        if (!ImGui::IsItemDeactivatedAfterEdit()) return;
        ramp.sort();   // 见文件头 ①：只在这里排序
        if (m_armBefore.size() == sizeof(ecs::ParticleColorRamp) &&
            std::memcmp(m_armBefore.data(), &ramp, sizeof(ramp)) != 0) {
            m_ctx.commands().pushAlreadyApplied(
                std::make_unique<RawBytesEditCommand>(
                    m_armName, &ramp, m_armBefore.data(), &ramp, sizeof(ramp)));
            m_ctx.setStatus(m_armName);
            m_ctx.dirty() = true;
        }
        m_armTarget = nullptr;
    };

    // -------------------------------------------------------------- 预览带
    //
    // 一排竖条铺满控件宽度：每列取该位置的渐变颜色。不做离屏纹理 ——
    // 一条色带最多几百个矩形，和画一根进度条是同一量级的开销。
    {
        float bandW = ImGui::GetContentRegionAvail().x;
        const float bandH = 18.0f;
        if (bandW < 24.0f) bandW = 24.0f;
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##band", ImVec2(bandW, bandH));
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // 见文件头 ②：求值用已排序的局部队本，拖动中也不会闪。
        ecs::ParticleColorRamp view = ramp;
        view.sort();

        const int cols = static_cast<int>(bandW) < 2
                             ? 2
                             : static_cast<int>(bandW);
        for (int i = 0; i < cols; ++i) {
            const float t =
                static_cast<float>(i) / static_cast<float>(cols - 1);
            const glm::vec3 c = view.eval(t);
            // 色带里的颜色是**线性 HDR**（>1 才是火焰发光的原因），直接当
            // 8 位色画会在 1.0 处整片截成白色。这里做一次 Reinhard + sRGB
            // 只是为了"看得见走向"，不代表画面最终亮度。
            const glm::vec3 m = c / (c + glm::vec3(1.0f));
            auto to8 = [](float v) {
                const float s =
                    std::pow(glm::clamp(v, 0.0f, 1.0f), 1.0f / 2.2f);
                return static_cast<int>(glm::clamp(s, 0.0f, 1.0f) * 255.0f +
                                        0.5f);
            };
            dl->AddRectFilled(ImVec2(p0.x + static_cast<float>(i), p0.y),
                              ImVec2(p0.x + static_cast<float>(i + 1),
                                     p0.y + bandH),
                              IM_COL32(to8(m.x), to8(m.y), to8(m.z), 255));
        }
        dl->AddRect(p0, ImVec2(p0.x + bandW, p0.y + bandH),
                    IM_COL32(255, 255, 255, 70));

        // 停靠点标记用**未排序**的原数组：拖动时标记要跟着鼠标走，
        // 用排序后的位置会让标记在跨过邻居的一瞬间跳到别处。
        for (int i = 0; i < ramp.count; ++i) {
            const float x =
                p0.x + glm::clamp(ramp.stops[i].w, 0.0f, 1.0f) * bandW;
            dl->AddLine(ImVec2(x, p0.y - 3.0f), ImVec2(x, p0.y + bandH + 3.0f),
                        IM_COL32(255, 255, 255, 190), 1.5f);
        }
        ImGui::Spacing();
    }

    // ------------------------------------------------------------ 停靠点行
    for (int i = 0; i < ramp.count; ++i) {
        glm::vec4& stop = ramp.stops[i];
        ImGui::PushID(i);

        // 颜色（HDR：>1 是合法的，emissive 靠它把粒子推过 bloom 阈值）
        // arm() 的快照必须是**这条控件改动之前**的整条带子 —— 所以每个
        // 控件各抄一份，而不是一行抄一次。
        {
            const ecs::ParticleColorRamp c0 = ramp;
            ImGui::ColorEdit3("##c", glm::value_ptr(stop),
                              ImGuiColorEditFlags_NoInputs |
                                  ImGuiColorEditFlags_NoLabel |
                                  ImGuiColorEditFlags_HDR);
            arm(c0);
            commit();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Linear HDR colour of this stop.\n"
                                  "Values above 1 are allowed and are what\n"
                                  "makes a spark bloom.");
            }
        }

        ImGui::SameLine();
        // 位置滑条：占掉这一行剩下的宽度（右边留给删除按钮）
        {
            const float avail = ImGui::GetContentRegionAvail().x;
            ImGui::SetNextItemWidth(avail - 30.0f);
            const ecs::ParticleColorRamp t0 = ramp;
            float t = stop.w;
            if (ImGui::SliderFloat("##t", &t, 0.0f, 1.0f, "t=%.2f")) {
                stop.w = t;
                changed = true;
            }
            arm(t0);
            commit();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Where this stop sits along the particle's life\n"
                    "(0 = born, 1 = about to die).\n"
                    "Stops are re-sorted when you let go, so you can drag\n"
                    "one past its neighbours.");
            }
        }

        ImGui::SameLine();
        ImGui::BeginDisabled(ramp.count <= 1);
        if (ImGui::SmallButton("x")) {
            // 见文件头 ③：增删走 structuralEdit（整场景快照）。
            const int k = i;
            m_ctx.structuralEdit("Colour Ramp Remove Stop", [&]() {
                for (int j = k; j < ramp.count - 1; ++j) {
                    ramp.stops[j] = ramp.stops[j + 1];
                }
                if (ramp.count > 1) --ramp.count;
            });
            changed = true;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Delete this stop (at least one must remain).");
        }

        ImGui::PopID();
    }

    // ---------------------------------------------------------- Add stop
    ImGui::BeginDisabled(ramp.count >= ecs::kMaxColorStops);
    if (ImGui::SmallButton("Add stop")) {
        // 插在**最大的那个空隙**的中间，颜色取该处的渐变值 —— 这样"加点"
        // 从来不改变色带的外观，只是多了个能拖的控制点。（插在末尾的话新点
        // 会立刻改变渐变形状，用户会以为点错了。）
        // 插点位置保证 t 仍单调递增，所以不需要再排序。
        int at = ramp.count;
        float bestGap = -1.0f;
        float bestT = 1.0f;
        float prev = 0.0f;
        for (int i = 0; i < ramp.count; ++i) {
            const float gap = ramp.stops[i].w - prev;
            if (gap > bestGap) {
                bestGap = gap;
                bestT = (prev + ramp.stops[i].w) * 0.5f;
                at = i;
            }
            prev = ramp.stops[i].w;
        }
        if (1.0f - prev > bestGap) {
            bestT = (prev + 1.0f) * 0.5f;
            at = ramp.count;
        }
        const glm::vec3 c = ramp.eval(bestT);
        m_ctx.structuralEdit("Colour Ramp Add Stop", [&]() {
            for (int i = ramp.count; i > at; --i) {
                ramp.stops[i] = ramp.stops[i - 1];
            }
            ramp.stops[at] = glm::vec4(c, bestT);
            if (ramp.count < ecs::kMaxColorStops) ++ramp.count;
        });
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%d / %d stops", ramp.count, ecs::kMaxColorStops);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("A new stop is inserted in the widest gap and takes\n"
                          "the colour already there, so adding one never\n"
                          "changes how the ramp looks by itself.");
    }

    ImGui::PopID();
    return changed;
}

// ---------------------------------------------------------------- 粒子面板
//
// 结构照 Unity 的 Shuriken（粒子系统检查器）：一个组件 = 一串**模块**，
// 每个模块是"勾选框 + 可折叠标题 + 一组字段 + 一个模块级 Reset"。
// 模块的顺序、命名、字段归位都照 Unity 的默认面板排（Playback → Emission
// → Shape → Velocity → Limit Velocity → Force → Colour → Size → Rotation
// → Noise → Renderer → Soft Particles），从 Unity / Blender 过来的用户
// 能在同一个位置找到同一个参数。
//
// 四处刻意的工程决定：
//
// ① 模块折叠状态复用 m_sectionOpen，但键加 "fx/" 前缀。
//    好处是免费获得"折叠按名跨实体保持"。前缀不能省：INSP-SECTIONS
//    状态日志会把 m_sectionOpen 里所有 true 的键打出来，而
//    tools/verify_inspector.py 断言"Crate / Ball 的展开集合必须完全相等"。
//    子模块混进那个清单会让这类断言的语义变模糊。
//
// ② 固有模块（Shape / Colour / Size / Renderer）没有"关"的语义，勾选框
//    画成"勾住但不可点"，tooltip 说明原因。比塞一个不生效的假开关诚实，
//    Unity 自己的 Renderer 模块同样没有勾选框。
//
// ③ 每个模块带一个 "Reset to default"：回滚粒度是**模块**。
//    用户点的是"把 Noise 恢复默认"，不是"把整个发射器清空"。
//
// ④ 撤销记的是**整个组件**的一条字节快照，不是逐字段。
//    逐字段记会让"恢复默认 Shape"压出 7 条历史，要按 7 次才退回去。
//    ParticleEmitterComponent 是纯 POD（内建的 colorRamp / bursts 数组
//    也全是数值），整块 memcpy 进 RawBytesEditCommand 是安全的 —— 这也
//    是它支持 Copy/Paste Properties（podKindOf → PK_ParticleEmitter）的
//    前提。连续拖拽仍然走 fieldEdit，"一次拖拽 = 一条历史"不变。
void InspectorPanel::drawParticleEmitterSection(ecs::Entity e) {
    scene::Scene& sc = m_ctx.editorScene();
    ecs::World& w = sc.world();

    auto* fx = w.get<ecs::ParticleEmitterComponent>(e);
    if (!fx) return;

    const bool sectionOpen = sectionHeader("Particle Emitter", false, e);
    logRect("INS-FX-HEADER", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (!sectionOpen) return;

    if (!m_ctx.isEditing()) {
        ImGui::TextDisabled("run mode - particle emitters are read-only here");
        return;
    }

    const ecs::ParticleEmitterComponent defaults{};

    // 本帧画出的模块键（"playback|emission|..."）与字段标签，拼成签名喂给
    // 去抖的 FX-PARAMS 日志。验证脚本靠它断言"这一段真的有这些控件"，
    // 比去截图里认控件靠谱得多（也一样不会刷屏）。
    std::string modKeys, fieldNames;
    auto markField = [&](const char* n) {
        fieldNames += n;
        fieldNames += ',';
    };

    // 一次拖拽 / 一次点击的整组件快照进撤销栈（见文件头 ④）。
    auto pushWhole = [&](const char* label,
                         const ecs::ParticleEmitterComponent& before) {
        if (std::memcmp(&before, fx, sizeof(before)) == 0) return;
        m_ctx.commands().pushAlreadyApplied(std::make_unique<RawBytesEditCommand>(
            label, fx, &before, fx, sizeof(before)));
        m_ctx.setStatus(label);
        m_ctx.dirty() = true;
    };

    // 模块末尾的 "Reset to default"（见文件头 ③）。
    // apply 是"把 defaults 里的这几个字段抄回组件"的逐模块函数。
    auto modReset = [&](const char* title, auto&& apply) {
        ImGui::PushID(title);
        if (ImGui::SmallButton("Reset to default")) {
            const ecs::ParticleEmitterComponent before = *fx;
            apply(*fx, defaults);
            if (std::memcmp(&before, fx, sizeof(before)) == 0) {
                m_ctx.setStatus(std::string(title) + " is already at default");
            } else {
                pushWhole((std::string("Reset ") + title).c_str(), before);
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Restore every field of this module to its\n"
                              "built-in default. One undo step.");
        }
        ImGui::PopID();
    };

    // ---- 模块表（顺序 = 面板顺序 = Unity 的默认顺序）----
    struct FxModuleRow { const char* title; const char* key; bool open; };
    static const FxModuleRow kModules[] = {
        {"Playback", "playback", true},
        {"Emission", "emission", true},
        {"Shape", "shape", true},
        {"Velocity over Lifetime", "velocity", false},
        {"Limit Velocity", "limit", false},
        {"Force over Lifetime", "force", false},
        {"Colour over Lifetime", "colour", false},
        {"Size over Lifetime", "size", false},
        {"Rotation over Lifetime", "rot", false},
        {"Noise", "noise", false},
        {"Renderer", "renderer", true},
        {"Soft Particles", "soft", false},
    };

    static const char* kIntrinsicTip =
        "This module has no on/off state: its fields are always applied.\n"
        "The box is drawn ticked and greyed so that every module looks\n"
        "the same, instead of faking a switch that would do nothing.";

    // 搜索的**第二级过滤**。组件级关键词（sectionKeywords）决定整段要不要
    // 出现，这里决定段里哪些模块要出现。若搜索词只命中了组件级关键词
    // （比如 "particle"）而没命中**任何模块**，就不过滤模块 —— 否则会得到
    // 一个"标题在、里面全空"的界面，比不过滤更让人困惑。
    //
    // ⚠ "命中模块"必须**同时**看模块名与模块关键词。
    //   第一版只看模块名（m.title），于是搜 "billboard"（它只在 renderer 的
    //   关键词表里）得到 anyTitleHit=false → 兜底分支 `!anyTitleHit` 为真
    //   → **12 个模块全部显示**，过滤等于没生效。译成一句话：
    //   "兜底"的判据必须与"逐模块放行"的判据**同源**，否则两者会互相打架。
    //   （verify_particles.py 的 D5c 就是钉这一条的。）
    bool anyModuleHit = (m_search[0] == '\0');
    if (!anyModuleHit) {
        for (const FxModuleRow& m : kModules) {
            if (containsCI(m.title, m_search) ||
                containsCI(fxModuleKeywords(m.key), m_search)) {
                anyModuleHit = true;
                break;
            }
        }
    }
    auto moduleVisible = [&](const char* title, const char* key) {
        if (m_search[0] == '\0') return true;
        if (containsCI(title, m_search)) return true;
        if (containsCI(fxModuleKeywords(key), m_search)) return true;
        return !anyModuleHit;
    };

    // 12 个模块逐个点开太累，两个按钮一键到位。
    //
    // ⚠ 计数和按钮**分两行**：侧栏只有 ~200 px 宽，"12 modules" 加两个
    //   按钮挤一行会被右边缘裁掉（实测 "Collapse all" 只剩 "Collapse a"）。
    //
    // ⚠ 这必须走 m_fxForceModules 这一帧的强制（而不是只改 m_sectionOpen）：
    //   ImGui 的 SetNextItemOpen(cond = Appearing) 只在节点**第一次出现**
    //   时生效，之后状态归 ImGui 自己的 storage 管 —— 光改 m_sectionOpen
    //   会"点了没反应"。
    ImGui::TextDisabled("%d modules", static_cast<int>(sizeof(kModules) /
                                                       sizeof(kModules[0])));
    if (ImGui::SmallButton("Expand all")) m_fxForceModules = 1;
    // 矩形日志：E11 的既有约定 —— 每个能被自动化的控件都打一行
    // "在哪"，测试脚本据此注入点击，不用去截图里猜坐标。
    logRect("INS-FX-EXPAND-ALL", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::SmallButton("Collapse all")) m_fxForceModules = 0;
    logRect("INS-FX-COLLAPSE-ALL", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());

    // 一个模块行。返回 true = 展开着（调用方接着画 body）。
    auto module = [&](const char* title, const char* key, bool defaultOpen,
                      bool* flag, const char* tip, auto&& body) {
        // 被搜索过滤掉：整行连同 body 都不画。
        //
        // ⚠ 这里必须是裸 `return;`，不能写 `return false;` —— 这个 lambda 的
        //   返回类型是**推导**出来的，混进一个 `return false;` 会把返回类型
        //   推成 bool，而函数末尾没有 return 就是 UB。MSVC 在 Debug 下会
        //   在末尾塞一条 trap 指令，于是"打开粒子面板"变成整进程
        //   0xC000001D（Illegal instruction），而且一行日志都不给。
        if (!moduleVisible(title, key)) return;

        ImGui::PushID(key);

        // ① 勾选框：有真实开关的模块可点，固有模块勾住但不可点。
        bool on = flag ? *flag : true;
        if (flag) {
            if (ImGui::Checkbox("##on", &on)) {
                const ecs::ParticleEmitterComponent before = *fx;
                *flag = on;
                pushWhole((std::string("Toggle ") + title + " module").c_str(),
                          before);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", tip ? tip : "Enable this module.");
            }
        } else {
            ImGui::BeginDisabled();
            ImGui::Checkbox("##on", &on);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("%s", kIntrinsicTip);
            }
        }
        ImGui::SameLine();

        // ② 折叠标题。
        // ⚠ 标签里 "##" 的后半段才是 ImGui 的 ID。所以绝不能让所有模块都
        //   叫 "XXX##fx" —— 那样 12 个折叠会共用一个 ID、共用一个展开状态。
        //   唯一的 key 必须放进 "##" 后面。
        char label[96];
        std::snprintf(label, sizeof(label), "%s##%s", title, key);
        const std::string k = std::string("fx/") + key;
        const auto it = m_sectionOpen.find(k);
        bool want = (it != m_sectionOpen.end()) ? it->second : defaultOpen;
        ImGuiCond cond = ImGuiCond_Appearing;
        if (m_fxForceModules >= 0) {
            want = (m_fxForceModules != 0);
            cond = ImGuiCond_Always;
        }
        ImGui::SetNextItemOpen(want, cond);
        const bool opened = ImGui::CollapsingHeader(label);
        m_sectionOpen[k] = opened;

        if (opened) {
            modKeys += key;
            modKeys += '|';
            // Unity 的行为：模块关掉的时候，里面的字段是灰的、点不动的。
            // 这条很值得抄 —— 它直接回答了"我改了参数怎么没反应"。
            // 把 body 包在这里（而不是让 12 个调用点各写一遍
            // BeginDisabled/EndDisabled）是为了让"模块关掉 = 内容置灰"
            // 这件事只在一个地方表达。
            const bool off = (flag != nullptr) && !*flag;
            ImGui::BeginDisabled(off);
            body();
            ImGui::EndDisabled();
        }
        ImGui::PopID();
    };

    // ============================================================== Playback
    // Unity 的 Main 模块：勾选框就是组件的总开关。
    // 字段归位说明：Unity 把 Start Lifetime / Start Speed / Gravity Modifier
    // 放在 Main 里，所以这里也放这儿，而不是塞进 Emission。
    module("Playback", "playback", true, &fx->enabled,
               "Master switch for this emitter.\n"
               "Off = it contributes no particles at all.\n"
               "(This is separate from the global MYVK_FX switch.)", [&] {
        ImGui::SetNextItemWidth(-110.0f);
        const float dur0 = fx->duration;
        ImGui::SliderFloat("Duration##fx", &fx->duration, 0.0f, 60.0f, "%.2f s");
        fieldEdit(m_ctx, "Emitter Duration", &fx->duration, dur0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Length of one emission cycle, in seconds.\n"
                              "0 = infinite (the default): the cycle never\n"
                              "restarts, so Looping / Prewarm / Bursts have\n"
                              "nothing to act on.");
        }
        markField("Duration");

        {
            const bool b0 = fx->looping;
            ImGui::Checkbox("Looping##fx", &fx->looping);
            discreteEdit(m_ctx, "Emitter Looping", &fx->looping, b0);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Repeat the cycle forever.\n"
                                  "Only meaningful when Duration > 0.");
            }
            markField("Looping");
        }
        {
            const bool b0 = fx->prewarm;
            ImGui::Checkbox("Prewarm##fx", &fx->prewarm);
            discreteEdit(m_ctx, "Emitter Prewarm", &fx->prewarm, b0);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Run one full cycle before the first rendered frame, so\n"
                    "the emitter opens at steady state instead of visibly\n"
                    "filling up.\n"
                    "• needs Duration > 0 and Looping on;\n"
                    "• keep it OFF for frozen-frame verification - it would\n"
                    "  make \"the first simulated step\" not be step 1.");
            }
            markField("Prewarm");
        }

        ImGui::SetNextItemWidth(-110.0f);
        const float sd0 = fx->startDelay;
        ImGui::SliderFloat("Start Delay##fx", &fx->startDelay, 0.0f, 30.0f,
                           "%.2f s");
        fieldEdit(m_ctx, "Emitter Start Delay", &fx->startDelay, sd0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Nothing is emitted before this moment.\n"
                              "Rate over Distance is frozen for just as long,\n"
                              "so a delayed emitter does not \"catch up\".");
        }
        markField("StartDelay");

        ImGui::SetNextItemWidth(-110.0f);
        const float life0 = fx->lifetime;
        ImGui::SliderFloat("Start Lifetime##fx", &fx->lifetime, 0.05f, 30.0f,
                           "%.2fs", ImGuiSliderFlags_Logarithmic);
        fieldEdit(m_ctx, "Emitter Lifetime", &fx->lifetime, life0);
        markField("Lifetime");

        ImGui::SetNextItemWidth(-110.0f);
        const float lifeV0 = fx->lifetimeVariance;
        ImGui::SliderFloat("Lifetime +/-##fx", &fx->lifetimeVariance, 0.0f, 1.0f,
                           "%.2f");
        fieldEdit(m_ctx, "Emitter Lifetime Variance", &fx->lifetimeVariance,
                  lifeV0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Relative spread of the lifetime\n"
                              "(0 = every particle dies at the same age, which\n"
                              "makes the pool empty in visible waves).");
        }
        markField("LifetimeVariance");

        ImGui::SetNextItemWidth(-110.0f);
        const float spd0 = fx->startSpeed;
        ImGui::SliderFloat("Start Speed##fx", &fx->startSpeed, 0.0f, 30.0f,
                           "%.2f m/s");
        fieldEdit(m_ctx, "Emitter Speed", &fx->startSpeed, spd0);
        markField("Speed");

        ImGui::SetNextItemWidth(-110.0f);
        const float spdV0 = fx->speedVariance;
        ImGui::SliderFloat("Speed +/-##fx", &fx->speedVariance, 0.0f, 1.0f,
                           "%.2f");
        fieldEdit(m_ctx, "Emitter Speed Variance", &fx->speedVariance, spdV0);
        markField("SpeedVariance");

        ImGui::SetNextItemWidth(-110.0f);
        const glm::vec3 g0 = fx->gravity;
        ImGui::DragFloat3("Gravity##fx", glm::value_ptr(fx->gravity), sp(0.1f),
                          -50.0f, 50.0f, "%.2f");
        fieldEdit(m_ctx, "Emitter Gravity", &fx->gravity, g0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Constant acceleration in m/s^2 (Unity: Gravity\n"
                              "Modifier). The engine is Z-up, so -9.8 on Z is\n"
                              "real gravity and a positive Z makes smoke rise.\n"
                              "With Simulation Space = Local the solver\n"
                              "subtracts the emitter's own motion, so a moving\n"
                              "torch still drips downwards.");
        }
        markField("Gravity");

        static const char* kSpaceNames[] = {"Local", "World"};
        int space = static_cast<int>(fx->simulationSpace);
        ImGui::SetNextItemWidth(-110.0f);
        if (ImGui::Combo("Simulation Space##fx", &space, kSpaceNames, 2)) {
            const ecs::ParticleSimulationSpace before = fx->simulationSpace;
            fx->simulationSpace = static_cast<ecs::ParticleSimulationSpace>(space);
            discreteEdit(m_ctx, "Emitter Simulation Space", &fx->simulationSpace,
                         before);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Local: particles are stored relative to the\n"
                              "emitter, so they follow it (torch, exhaust).\n"
                              "World: they are left behind in the scene\n"
                              "(campfire smoke).\n"
                              "This is a per-particle flag baked in at spawn -\n"
                              "changing it does not teleport live particles.");
        }
        markField("SimulationSpace");

        modReset("Playback", [](auto& c, const auto& d) {
            c.enabled = d.enabled;
            c.duration = d.duration;
            c.looping = d.looping;
            c.prewarm = d.prewarm;
            c.startDelay = d.startDelay;
            c.simulationSpace = d.simulationSpace;
            c.rate = d.rate;
            c.rateOverDistance = d.rateOverDistance;
            c.lifetime = d.lifetime;
            c.lifetimeVariance = d.lifetimeVariance;
            c.startSpeed = d.startSpeed;
            c.speedVariance = d.speedVariance;
            c.gravity = d.gravity;
        });
    });

    // ============================================================== Emission
    module("Emission", "emission", true, nullptr, nullptr, [&] {
        ImGui::SetNextItemWidth(-110.0f);
        const float rate0 = fx->rate;
        // ⚠ 线性而不是对数：ImGui 的对数滑条**要求两端都不为 0**
        //   （IM_ASSERT(v_min != 0.0f)），而 0 在这里是合法且有意义的取值
        //   （"只发一次性的 burst"）。
        ImGui::SliderFloat("Rate over Time##fx", &fx->rate, 0.0f, 1000.0f,
                           "%.0f/s");
        fieldEdit(m_ctx, "Emitter Rate", &fx->rate, rate0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Particles spawned per second. Uniform in time: every pool\n"
                "slot is an independent periodic emitter, so raising the rate\n"
                "never produces a burst - it just fills the pool faster.\n"
                "0 = only the bursts below emit.");
        }
        markField("Rate");

        ImGui::SetNextItemWidth(-110.0f);
        const float rod0 = fx->rateOverDistance;
        ImGui::SliderFloat("Rate over Distance##fx", &fx->rateOverDistance,
                           0.0f, 300.0f, "%.0f/m");
        fieldEdit(m_ctx, "Emitter Rate Over Distance", &fx->rateOverDistance,
                  rod0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Extra particles per metre travelled, added on top of the\n"
                "rate above. Stationary = no effect; a fast parent draws a\n"
                "streak. Great for muzzle flashes and slashing trails.\n"
                "Distance only accumulates while a simulation step actually\n"
                "runs, so a frozen frame adds nothing.");
        }
        markField("RateOverDistance");

        // ---- Bursts ----
        // Unity 的 Bursts 是可增删的数组；这里固定 4 条（kMaxBursts）。
        // count = 0 的行就等于"没用"（GPU 侧按 count > 0 跳过），
        // 所以不需要额外的启用开关。
        ImGui::SeparatorText("Bursts");
        if (ImGui::BeginTable("##fxBursts", 5,
                              ImGuiTableFlags_SizingStretchProp |
                                  ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed,
                                    20.0f);
            ImGui::TableSetupColumn("Time");
            ImGui::TableSetupColumn("Count");
            ImGui::TableSetupColumn("Cycles");
            ImGui::TableSetupColumn("Interval");
            ImGui::TableHeadersRow();
            for (int i = 0; i < ecs::kMaxBursts; ++i) {
                ecs::ParticleBurst& b = fx->bursts[i];
                ImGui::PushID(i);
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("%d", i);

                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(-1.0f);
                const float bt0 = b.time;
                ImGui::DragFloat("##bt", &b.time, sp(0.01f), 0.0f, 600.0f,
                                 "%.2f");
                fieldEdit(m_ctx,
                          ("Burst " + std::to_string(i) + " Time").c_str(),
                          &b.time, bt0);

                ImGui::TableSetColumnIndex(2);
                ImGui::SetNextItemWidth(-1.0f);
                const int bc0 = b.count;
                ImGui::DragInt("##bc", &b.count, 1.0f, 0, 65536, "%d");
                fieldEdit(m_ctx,
                          ("Burst " + std::to_string(i) + " Count").c_str(),
                          &b.count, bc0);

                ImGui::TableSetColumnIndex(3);
                ImGui::SetNextItemWidth(-1.0f);
                const int bc2 = b.cycles;
                ImGui::DragInt("##bcy", &b.cycles, 1.0f, 0, 1024, "%d");
                fieldEdit(m_ctx,
                          ("Burst " + std::to_string(i) + " Cycles").c_str(),
                          &b.cycles, bc2);

                ImGui::TableSetColumnIndex(4);
                ImGui::SetNextItemWidth(-1.0f);
                const float bi0 = b.interval;
                ImGui::DragFloat("##bi", &b.interval, sp(0.01f), 0.0f, 60.0f,
                                 "%.2f");
                fieldEdit(m_ctx,
                          ("Burst " + std::to_string(i) + " Interval").c_str(),
                          &b.interval, bi0);

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "A burst is a one-shot payload at a fixed moment in the\n"
                "cycle: Count particles at Time seconds after the cycle\n"
                "begins, repeated Cycles times every Interval seconds.\n"
                "Count = 0 disables the row. Bursts need Duration > 0 -\n"
                "with an infinite cycle there is no moment to fire at.");
        }
        logRect("INS-FX-BURSTS", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        markField("Bursts");

        modReset("Emission", [](auto& c, const auto& d) {
            c.rate = d.rate;
            c.rateOverDistance = d.rateOverDistance;
            for (int i = 0; i < ecs::kMaxBursts; ++i) c.bursts[i] = d.bursts[i];
        });
    });

    // ================================================================= Shape
    module("Shape", "shape", true, nullptr, nullptr, [&] {
        static const char* kShapeNames[] = {"Point",    "Box",    "Sphere",
                                            "Hemisphere", "Cone", "Circle",
                                            "Edge"};
        int shape = static_cast<int>(fx->shape);
        ImGui::SetNextItemWidth(-110.0f);
        if (ImGui::Combo("Shape##fx", &shape, kShapeNames,
                         IM_ARRAYSIZE(kShapeNames))) {
            const ecs::ParticleShape before = fx->shape;
            fx->shape = static_cast<ecs::ParticleShape>(shape);
            discreteEdit(m_ctx, "Emitter Shape", &fx->shape, before);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Where inside the emitter volume particles are\n"
                              "born.\n"
                              "Point: all from the origin.\n"
                              "Box: uniform inside Extents.\n"
                              "Sphere / Hemisphere: uniform inside Radius\n"
                              "  (not clustered at the centre).\n"
                              "Cone: along local +Z, circle: its base disc.\n"
                              "Edge: a line segment across local X.");
        }
        markField("Shape");

        static const char* kEmitFromNames[] = {"Volume", "Edge", "Shell"};
        int from = static_cast<int>(fx->emitFrom);
        ImGui::SetNextItemWidth(-110.0f);
        if (ImGui::Combo("Emit From##fx", &from, kEmitFromNames,
                         IM_ARRAYSIZE(kEmitFromNames))) {
            const ecs::ParticleEmitFrom before = fx->emitFrom;
            fx->emitFrom = static_cast<ecs::ParticleEmitFrom>(from);
            discreteEdit(m_ctx, "Emitter Emit From", &fx->emitFrom, before);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Volume: filled volume (weighted by Radius\n"
                              "  Thickness).\n"
                              "Shell: only the surface (sphere shell / box\n"
                              "  faces).\n"
                              "Edge: only the rim (box edges / cone base rim /\n"
                              "  the two ends of a line).");
        }
        markField("EmitFrom");

        const ecs::ParticleShape sk = fx->shape;
        if (sk == ecs::ParticleShape::Box) {
            ImGui::SetNextItemWidth(-110.0f);
            const glm::vec3 ex0 = fx->shapeExtents;
            ImGui::DragFloat3("Extents##fx", glm::value_ptr(fx->shapeExtents),
                              sp(0.05f), 0.0f, 100.0f, "%.2f");
            fieldEdit(m_ctx, "Emitter Extents", &fx->shapeExtents, ex0);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Half-extents of the box, in metres\n"
                                  "(the box is 2x this on each axis).");
            }
            markField("Extents");
        } else if (sk != ecs::ParticleShape::Point) {
            ImGui::SetNextItemWidth(-110.0f);
            const float r0 = fx->shapeRadius;
            ImGui::SliderFloat("Radius##fx", &fx->shapeRadius, 0.0f, 50.0f,
                               "%.2f");
            fieldEdit(m_ctx, "Emitter Radius", &fx->shapeRadius, r0);
            markField("Radius");

            // Radius Thickness 只对 Volume 有意义（Shell / Edge 本身就是
            // 贴着表面/边缘的），所以按情况置灰而不是一律画出来。
            const bool thicknessRelevant =
                (fx->emitFrom == ecs::ParticleEmitFrom::Volume);
            ImGui::BeginDisabled(!thicknessRelevant);
            ImGui::SetNextItemWidth(-110.0f);
            const float th0 = fx->radiusThickness;
            ImGui::SliderFloat("Radius Thickness##fx", &fx->radiusThickness,
                               0.0f, 1.0f, "%.2f");
            fieldEdit(m_ctx, "Emitter Radius Thickness", &fx->radiusThickness,
                      th0);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip(
                    "How much of the volume is used: 1 = solid, 0 = a hollow\n"
                    "shell hugging the surface. Ignored when Emit From is\n"
                    "Shell or Edge (those already sit on the surface).");
            }
            markField("RadiusThickness");
        }

        if (sk != ecs::ParticleShape::Point) {
            const bool arcRelevant = (sk != ecs::ParticleShape::Edge);
            ImGui::BeginDisabled(!arcRelevant);
            ImGui::SetNextItemWidth(-110.0f);
            const float arc0 = fx->arc;
            ImGui::SliderFloat("Arc##fx", &fx->arc, 1.0f, 360.0f, "%.0f deg");
            fieldEdit(m_ctx, "Emitter Arc", &fx->arc, arc0);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip(
                    "Angular span around local +Z. 360 = the whole turn;\n"
                    "180 = a half disc (the classic \"fan\" emitter).\n"
                    "Uses the same deterministic angle as the shape sample,\n"
                    "so 360 stays bit-identical to the old behaviour.");
            }
            markField("Arc");

            ImGui::SetNextItemWidth(-110.0f);
            const float sr0 = fx->shapeRotation;
            ImGui::SliderFloat("Shape Rotation##fx", &fx->shapeRotation, -180.0f,
                               180.0f, "%.0f deg");
            fieldEdit(m_ctx, "Emitter Shape Rotation", &fx->shapeRotation, sr0);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Rotates the whole shape about local +Z -\n"
                                  "how you aim that half disc.");
            }
            markField("ShapeRotation");
        }

        ImGui::SetNextItemWidth(-110.0f);
        const float sp0 = fx->spread;
        ImGui::SliderFloat("Angle##fx", &fx->spread, 0.0f, 90.0f, "%.0f deg");
        fieldEdit(m_ctx, "Emitter Spread", &fx->spread, sp0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Half-angle of the emission cone around local +Z\n"
                "(0 = a straight jet, 90 = a full hemisphere).\n"
                "Sampled uniformly in cos(theta), so the cone mouth is evenly\n"
                "covered instead of clumping along the axis.");
        }
        markField("Spread");

        modReset("Shape", [](auto& c, const auto& d) {
            c.shape = d.shape;
            c.emitFrom = d.emitFrom;
            c.shapeExtents = d.shapeExtents;
            c.shapeRadius = d.shapeRadius;
            c.radiusThickness = d.radiusThickness;
            c.arc = d.arc;
            c.shapeRotation = d.shapeRotation;
            c.spread = d.spread;
        });
    });

    // =================================================== Velocity over Lifetime
    module("Velocity over Lifetime", "velocity", false,
               &fx->velOverLifeEnabled,
               "Add a velocity curve on top of Start Speed.\n"
               "Curves are metres/second sampled at t = 0, 1/3, 2/3, 1.", [&] {
        ImGui::TextUnformatted("Linear X / Y / Z");
        {
            const glm::vec4 c0 = fx->velOverLifeX;
            ImGui::SetNextItemWidth(-110.0f);
            ImGui::SliderFloat4("##fxVelX", glm::value_ptr(fx->velOverLifeX),
                                -10.0f, 10.0f, "%.2f");
            fieldEdit(m_ctx, "Emitter Velocity X", &fx->velOverLifeX, c0);
            logRect("INS-FX-VEL-X", ImGui::GetItemRectMin(),
                    ImGui::GetItemRectMax());
            markField("VelX");
        }
        {
            const glm::vec4 c0 = fx->velOverLifeY;
            ImGui::SetNextItemWidth(-110.0f);
            ImGui::SliderFloat4("##fxVelY", glm::value_ptr(fx->velOverLifeY),
                                -10.0f, 10.0f, "%.2f");
            fieldEdit(m_ctx, "Emitter Velocity Y", &fx->velOverLifeY, c0);
            markField("VelY");
        }
        {
            const glm::vec4 c0 = fx->velOverLifeZ;
            ImGui::SetNextItemWidth(-110.0f);
            ImGui::SliderFloat4("##fxVelZ", glm::value_ptr(fx->velOverLifeZ),
                                -10.0f, 10.0f, "%.2f");
            fieldEdit(m_ctx, "Emitter Velocity Z", &fx->velOverLifeZ, c0);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("A curve of 0,0,0,0 does nothing, which is\n"
                                  "why the module defaults to off.");
            }
            markField("VelZ");
        }

        {
            const bool b0 = fx->velOverLifeWorldSpace;
            ImGui::Checkbox("World Space##fx", &fx->velOverLifeWorldSpace);
            discreteEdit(m_ctx, "Emitter Velocity World Space",
                         &fx->velOverLifeWorldSpace, b0);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Off (default): the curves are read in the\n"
                                  "emitter's own frame, so they turn with it.\n"
                                  "On: they are world axes.");
            }
            markField("VelWorldSpace");
        }

        ImGui::SeparatorText("Orbital / radial");
        ImGui::SetNextItemWidth(-110.0f);
        const glm::vec3 o0 = fx->orbital;
        ImGui::DragFloat3("Orbital##fx", glm::value_ptr(fx->orbital), sp(1.0f),
                          -720.0f, 720.0f, "%.0f deg/s");
        fieldEdit(m_ctx, "Emitter Orbital", &fx->orbital, o0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Angular velocity about the emitter's local axes\n"
                              "(degrees/second). Even a single non-zero axis\n"
                              "turns a jet into a swirl; all three give the\n"
                              "fire-twister look.");
        }
        markField("Orbital");

        ImGui::SetNextItemWidth(-110.0f);
        const float oo0 = fx->orbitalOffset;
        ImGui::SliderFloat("Orbital Offset##fx", &fx->orbitalOffset, 0.0f, 10.0f,
                           "%.2f m");
        fieldEdit(m_ctx, "Emitter Orbital Offset", &fx->orbitalOffset, oo0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Radius offset for the orbital motion - pushes\n"
                              "the rotation axis away from the particle, so it\n"
                              "orbits a circle instead of spinning in place.");
        }
        markField("OrbitalOffset");

        ImGui::SetNextItemWidth(-110.0f);
        const float rd0 = fx->radial;
        ImGui::SliderFloat("Radial##fx", &fx->radial, -20.0f, 20.0f, "%.2f m/s");
        fieldEdit(m_ctx, "Emitter Radial", &fx->radial, rd0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Speed along the direction away from the emitter\n"
                              "origin. Positive = outward, negative = the\n"
                              "particles gather back in (implosions).");
        }
        markField("Radial");

        {
            const glm::vec4 c0 = fx->speedModifier;
            ImGui::TextUnformatted("Speed Modifier (multiplier, 0 0 0 0 = off)");
            ImGui::SetNextItemWidth(-110.0f);
            ImGui::SliderFloat4("##fxSpeedMod",
                                glm::value_ptr(fx->speedModifier), 0.0f, 4.0f,
                                "%.2f");
            fieldEdit(m_ctx, "Emitter Speed Modifier", &fx->speedModifier, c0);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Multiplies |velocity| over the lifetime -\n"
                                  "the cheapest way to make a burst decelerate\n"
                                  "(1, 0.5, 0.2, 0) or a plume accelerate.");
            }
            logRect("INS-FX-SPEED-MOD", ImGui::GetItemRectMin(),
                    ImGui::GetItemRectMax());
            markField("SpeedModifier");
        }

        modReset("Velocity over Lifetime", [](auto& c, const auto& d) {
            c.velOverLifeEnabled = d.velOverLifeEnabled;
            c.velOverLifeX = d.velOverLifeX;
            c.velOverLifeY = d.velOverLifeY;
            c.velOverLifeZ = d.velOverLifeZ;
            c.velOverLifeWorldSpace = d.velOverLifeWorldSpace;
            c.orbital = d.orbital;
            c.orbitalOffset = d.orbitalOffset;
            c.radial = d.radial;
            c.speedModifier = d.speedModifier;
        });
    });

    // ======================================================= Limit Velocity
    module("Limit Velocity", "limit", false, &fx->limitVelEnabled,
               "Clamp how fast a particle may travel.\n"
               "The classic use is stopping a launched spark from becoming a\n"
               "laser beam once gravity is small.", [&] {
        ImGui::SetNextItemWidth(-110.0f);
        const float ls0 = fx->limitSpeed;
        ImGui::SliderFloat("Speed Limit##fx", &fx->limitSpeed, 0.0f, 50.0f,
                           "%.2f m/s");
        fieldEdit(m_ctx, "Emitter Limit Speed", &fx->limitSpeed, ls0);
        markField("LimitSpeed");

        ImGui::SetNextItemWidth(-110.0f);
        const float ld0 = fx->limitDampen;
        ImGui::SliderFloat("Dampen##fx", &fx->limitDampen, 0.0f, 1.0f, "%.2f /s");
        fieldEdit(m_ctx, "Emitter Limit Dampen", &fx->limitDampen, ld0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Fraction of the excess speed removed per second\n"
                              "once the limit is exceeded. 0 = a hard clamp,\n"
                              "1 = snap straight down to the limit.");
        }
        markField("LimitDampen");

        ImGui::SeparatorText("Air drag (engine extra)");
        ImGui::SetNextItemWidth(-110.0f);
        const float drag0 = fx->drag;
        ImGui::SliderFloat("Drag##fx", &fx->drag, 0.0f, 5.0f, "%.2f /s");
        fieldEdit(m_ctx, "Emitter Drag", &fx->drag, drag0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Exponential velocity damping (1/s), 0 = none.\n"
                              "Integrated analytically as exp(-drag*dt), so it\n"
                              "is stable at any frame rate.\n"
                              "Not a Unity module field - the damping it has is\n"
                              "the per-particle Dampen above; a global air drag\n"
                              "is what an engine is expected to provide.");
        }
        markField("Drag");

        modReset("Limit Velocity", [](auto& c, const auto& d) {
            c.limitVelEnabled = d.limitVelEnabled;
            c.limitSpeed = d.limitSpeed;
            c.limitDampen = d.limitDampen;
            c.drag = d.drag;
        });
    });

    // ==================================================== Force over Lifetime
    module("Force over Lifetime", "force", false, &fx->forceEnabled,
               "A constant acceleration added to gravity, in m/s^2.\n"
               "Use it for wind and for rising heat.", [&] {
        ImGui::SetNextItemWidth(-110.0f);
        const glm::vec3 f0 = fx->force;
        ImGui::DragFloat3("Force##fx", glm::value_ptr(fx->force), sp(0.1f),
                          -100.0f, 100.0f, "%.2f");
        fieldEdit(m_ctx, "Emitter Force", &fx->force, f0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Added on top of Gravity, so (0,0,9.8) with the\n"
                              "default -9.8 gravity gives weightless particles.");
        }
        markField("Force");

        ImGui::SetNextItemWidth(-110.0f);
        const float fr0 = fx->forceRandomize;
        ImGui::SliderFloat("Randomize##fx", &fx->forceRandomize, 0.0f, 1.0f,
                           "%.2f");
        fieldEdit(m_ctx, "Emitter Force Randomize", &fx->forceRandomize, fr0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Per-particle jitter of the force vector, as a\n"
                              "fraction of its length. Breaks up the \"every\n"
                              "particle takes the identical path\" look.");
        }
        markField("ForceRandomize");

        modReset("Force over Lifetime", [](auto& c, const auto& d) {
            c.forceEnabled = d.forceEnabled;
            c.force = d.force;
            c.forceRandomize = d.forceRandomize;
        });
    });

    // ==================================================== Colour over Lifetime
    module("Colour over Lifetime", "colour", false, nullptr, nullptr, [&] {
        const bool rampChanged = drawColorRampEditor("fxRamp", fx->colorRamp);
        logRect("INS-FX-COLOR", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        // ⚠ 无条件登记字段名。以前写成 `if (rampChanged) markField(...)`，
        //   结果 FX-PARAMS 的 fields= 里只在"恰好改了一下色带"的那一帧才有
        //   ColourRamp —— 验证脚本据此断言"面板里有这个控件"就会随机失败。
        //   签名日志描述的是**面板的结构**，不是"这一帧有没有被改"。
        markField("ColourRamp");
        // "这一帧改过色带"是另一回事，给自动化留一条矩形日志（和面板里
        // 其它可交互控件同一个约定）。
        if (rampChanged) {
            logRect("INS-FX-RAMP-EDIT", ImGui::GetItemRectMin(),
                    ImGui::GetItemRectMax());
        }

        ImGui::TextUnformatted("Alpha over life");
        {
            // ⚠ 渐变的 vec4 里 w 是**位置**而不是不透明度，所以透明度
            //   只能单独给一条曲线 —— 这也是本引擎的色带和 Unity 的那条
            //   色带唯一的接口差异（Unity 把 alpha 塞进色带的 alpha 通道）。
            const glm::vec4 a0 = fx->alphaOverLife;
            ImGui::SetNextItemWidth(-110.0f);
            ImGui::SliderFloat4("##fxAlphaCurve",
                                glm::value_ptr(fx->alphaOverLife), 0.0f, 1.0f,
                                "%.2f");
            fieldEdit(m_ctx, "Emitter Alpha Curve", &fx->alphaOverLife, a0);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Opacity at t = 0, 1/3, 2/3, 1 of the life.\n"
                                  "Fire fades in fast and out slowly; smoke\n"
                                  "does the opposite.");
            }
            logRect("INS-FX-ALPHA-CURVE", ImGui::GetItemRectMin(),
                    ImGui::GetItemRectMax());
            markField("AlphaOverLife");
        }

        ImGui::SetNextItemWidth(-110.0f);
        const float em0 = fx->emissive;
        // 线性：0 是合法值（"不发光，只当普通贴片"），对数滑条不允许 min = 0。
        ImGui::SliderFloat("Emissive##fx", &fx->emissive, 0.0f, 20.0f, "%.2f");
        fieldEdit(m_ctx, "Emitter Emissive", &fx->emissive, em0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Multiplier on the colour. Above ~1 the particles\n"
                              "cross the bloom threshold, which is what turns a\n"
                              "few bright quads into glowing fire.\n"
                              "Kept here rather than in Renderer because it is\n"
                              "an appearance property of the colour itself.");
        }
        markField("Emissive");

        modReset("Colour over Lifetime", [](auto& c, const auto& d) {
            c.colorRamp = d.colorRamp;
            c.alphaOverLife = d.alphaOverLife;
            c.emissive = d.emissive;
        });
    });

    // ====================================================== Size over Lifetime
    module("Size over Lifetime", "size", false, nullptr, nullptr, [&] {
        // 四个控制点 = t 在 0 / 1/3 / 2/3 / 1 处的取值，段内线性插值。
        // 线性段的"折角"恰好让每个滑条的视觉含义是确定的：拖第 2 个只改变
        // [1/3, 2/3] 那一段的斜率。所以四根并排的滑条就是一条能编的曲线，
        // 不需要真的曲线编辑器（路线图："曲线编辑先用滑条组"）。
        const glm::vec4 s0 = fx->sizeOverLife;
        ImGui::SetNextItemWidth(-110.0f);
        ImGui::SliderFloat4("##fxSizeCurve", glm::value_ptr(fx->sizeOverLife),
                            0.0f, 3.0f, "%.2f");
        fieldEdit(m_ctx, "Emitter Size Curve", &fx->sizeOverLife, s0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Size multiplier at t = 0, 1/3, 2/3, 1 of the\n"
                              "particle's life, linearly interpolated in\n"
                              "between. Multiplied by the Renderer's Start Size\n"
                              "and by the global fx.sizeScale.");
        }
        logRect("INS-FX-SIZE-CURVE", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());
        markField("SizeCurve");

        modReset("Size over Lifetime", [](auto& c, const auto& d) {
            c.sizeOverLife = d.sizeOverLife;
        });
    });

    // ================================================== Rotation over Lifetime
    module("Rotation over Lifetime", "rot", false, nullptr, nullptr, [&] {
        ImGui::SetNextItemWidth(-110.0f);
        const float sr0 = fx->startRotation;
        ImGui::SliderFloat("Start Rotation##fx", &fx->startRotation, -180.0f,
                           180.0f, "%.0f deg");
        fieldEdit(m_ctx, "Emitter Start Rotation", &fx->startRotation, sr0);
        markField("StartRotation");

        ImGui::SetNextItemWidth(-110.0f);
        const float srv0 = fx->startRotationVariance;
        ImGui::SliderFloat("Rotation +/-##fx", &fx->startRotationVariance, 0.0f,
                           180.0f, "%.0f deg");
        fieldEdit(m_ctx, "Emitter Start Rotation Variance",
                  &fx->startRotationVariance, srv0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Random half-range of the initial angle (uniform).\n"
                              "180 is the default: every particle starts at a\n"
                              "random angle, which is what a fire needs -\n"
                              "identical angles make the sprites march in step.");
        }
        markField("StartRotationVariance");

        ImGui::SetNextItemWidth(-110.0f);
        const float av0 = fx->angularVelocity;
        ImGui::SliderFloat("Angular Velocity##fx", &fx->angularVelocity,
                           -720.0f, 720.0f, "%.0f deg/s");
        fieldEdit(m_ctx, "Emitter Angular Velocity", &fx->angularVelocity, av0);
        markField("AngularVelocity");

        ImGui::SetNextItemWidth(-110.0f);
        const float avv0 = fx->angularVelocityVariance;
        ImGui::SliderFloat("Spin +/-##fx", &fx->angularVelocityVariance, 0.0f,
                           1.0f, "%.2f");
        fieldEdit(m_ctx, "Emitter Angular Velocity Variance",
                  &fx->angularVelocityVariance, avv0);
        markField("AngularVelocityVariance");

        modReset("Rotation over Lifetime", [](auto& c, const auto& d) {
            c.startRotation = d.startRotation;
            c.startRotationVariance = d.startRotationVariance;
            c.angularVelocity = d.angularVelocity;
            c.angularVelocityVariance = d.angularVelocityVariance;
        });
    });

    // ================================================================= Noise
    module("Noise", "noise", false, &fx->noiseEnabled,
               "Push particles around with a smooth, deterministic turbulence\n"
               "field (value-noise fBm, not curl noise - see\n"
               "particle_common.glsl for why).", [&] {
        ImGui::SetNextItemWidth(-110.0f);
        const float ns0 = fx->noiseStrength;
        ImGui::SliderFloat("Strength##fx", &fx->noiseStrength, 0.0f, 20.0f,
                           "%.2f");
        fieldEdit(m_ctx, "Emitter Noise Strength", &fx->noiseStrength, ns0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Magnitude of the acceleration the field applies\n"
                              "(m/s^2). Small values (0.5-2) are already a lot:\n"
                              "this is an acceleration, not a velocity.");
        }
        markField("NoiseStrength");

        ImGui::SetNextItemWidth(-110.0f);
        const float nf0 = fx->noiseFrequency;
        ImGui::SliderFloat("Frequency##fx", &fx->noiseFrequency, 0.01f, 5.0f,
                           "%.2f");
        fieldEdit(m_ctx, "Emitter Noise Frequency", &fx->noiseFrequency, nf0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Spatial frequency of the field: low = big slow\n"
                              "swirls, high = fine chatter.");
        }
        markField("NoiseFrequency");

        ImGui::SetNextItemWidth(-110.0f);
        const float nss0 = fx->noiseScrollSpeed;
        ImGui::SliderFloat("Scroll Speed##fx", &fx->noiseScrollSpeed, 0.0f, 5.0f,
                           "%.2f");
        fieldEdit(m_ctx, "Emitter Noise Scroll Speed", &fx->noiseScrollSpeed,
                  nss0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("How fast the noise field itself drifts (0 = the\n"
                              "field is frozen in space, so particles merely\n"
                              "fall through a static pattern).");
        }
        markField("NoiseScrollSpeed");

        ImGui::SetNextItemWidth(-110.0f);
        const float nd0 = fx->noiseDamping;
        ImGui::SliderFloat("Damping##fx", &fx->noiseDamping, 0.0f, 1.0f, "%.2f");
        fieldEdit(m_ctx, "Emitter Noise Damping", &fx->noiseDamping, nd0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Damps the noise contribution per second.\n"
                              "Higher = the particles fly straighter and the\n"
                              "noise only nuzzles them.");
        }
        markField("NoiseDamping");

        ImGui::SetNextItemWidth(-110.0f);
        const int no0 = fx->noiseOctaves;
        ImGui::SliderInt("Octaves##fx", &fx->noiseOctaves, 1, 4, "%d");
        fieldEdit(m_ctx, "Emitter Noise Octaves", &fx->noiseOctaves, no0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("fBm layers: 1 = one smooth field, 4 = the same\n"
                              "field plus three finer copies. Each layer costs\n"
                              "three more noise samples per particle.");
        }
        markField("NoiseOctaves");

        modReset("Noise", [](auto& c, const auto& d) {
            c.noiseEnabled = d.noiseEnabled;
            c.noiseStrength = d.noiseStrength;
            c.noiseFrequency = d.noiseFrequency;
            c.noiseScrollSpeed = d.noiseScrollSpeed;
            c.noiseDamping = d.noiseDamping;
            c.noiseOctaves = d.noiseOctaves;
        });
    });

    // ============================================================== Renderer
    module("Renderer", "renderer", true, nullptr, nullptr, [&] {
        ImGui::SetNextItemWidth(-110.0f);
        const float size0 = fx->startSize;
        ImGui::SliderFloat("Start Size##fx", &fx->startSize, 0.005f, 2.0f,
                           "%.3f m", ImGuiSliderFlags_Logarithmic);
        fieldEdit(m_ctx, "Emitter Start Size", &fx->startSize, size0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Edge length of the quad in metres (this engine\n"
                              "uses a round sprite, so it is a diameter).\n"
                              "Zero particles are the same size - Size over\n"
                              "Lifetime is what varies them.");
        }
        markField("StartSize");

        static const char* kRenderNames[] = {"Billboard", "Stretched Billboard",
                                             "Horizontal Billboard",
                                             "Vertical Billboard"};
        int rmode = static_cast<int>(fx->renderMode);
        ImGui::SetNextItemWidth(-110.0f);
        if (ImGui::Combo("Render Mode##fx", &rmode, kRenderNames,
                         IM_ARRAYSIZE(kRenderNames))) {
            const ecs::ParticleRenderMode before = fx->renderMode;
            fx->renderMode = static_cast<ecs::ParticleRenderMode>(rmode);
            discreteEdit(m_ctx, "Emitter Render Mode", &fx->renderMode, before);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Billboard: always faces the camera.\n"
                              "Stretched: the long axis follows the velocity\n"
                              "(rain streaks, sparks).\n"
                              "Horizontal: lies flat, facing world +Z (ground\n"
                              "shockwaves).\n"
                              "Vertical: stands up but is locked about world Z\n"
                              "(fire columns, waterfalls).");
        }
        markField("RenderMode");

        ImGui::SetNextItemWidth(-110.0f);
        const float st0 = fx->stretchScale;
        ImGui::SliderFloat("Stretch##fx", &fx->stretchScale, 0.0f, 5.0f, "%.2f");
        fieldEdit(m_ctx, "Emitter Stretch", &fx->stretchScale, st0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Stretched billboard only: the quad's length grows\n"
                              "by |v| * this. 0 = a plain camera-facing quad.\n"
                              "Rain and sparks want ~0.1.");
        }
        markField("Stretch");

        static const char* kSortNames[] = {"None", "By Distance", "Oldest First",
                                           "Youngest First"};
        int smode = static_cast<int>(fx->sortMode);
        ImGui::SetNextItemWidth(-110.0f);
        if (ImGui::Combo("Sort Mode##fx", &smode, kSortNames,
                         IM_ARRAYSIZE(kSortNames))) {
            const ecs::ParticleSortMode before = fx->sortMode;
            fx->sortMode = static_cast<ecs::ParticleSortMode>(smode);
            discreteEdit(m_ctx, "Emitter Sort Mode", &fx->sortMode, before);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "None (default): draw in pool order - free, and correct for\n"
                "additive blending (order does not matter for light).\n"
                "The other three run a bucket sort on the GPU each frame and\n"
                "cost a dispatch (see FX-DUMP's sort= field).");
        }
        markField("SortMode");

        static const char* kBlendNames[] = {"Additive", "Alpha"};
        int blend = static_cast<int>(fx->blend);
        ImGui::SetNextItemWidth(-110.0f);
        if (ImGui::Combo("Blend##fx", &blend, kBlendNames, 2)) {
            const ecs::ParticleBlend before = fx->blend;
            fx->blend = static_cast<ecs::ParticleBlend>(blend);
            discreteEdit(m_ctx, "Emitter Blend", &fx->blend, before);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Additive: light adds up - fire, sparks, magic.\n"
                              "Alpha: normal over-blending - smoke, dust, water.\n"
                              "Blend mode is fixed pipeline state, so particles\n"
                              "are compacted into two lists and drawn with two\n"
                              "indirect draws (the empty one costs nothing).");
        }
        logRect("INS-FX-BLEND", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        markField("Blend");

        modReset("Renderer", [](auto& c, const auto& d) {
            c.startSize = d.startSize;
            c.renderMode = d.renderMode;
            c.stretchScale = d.stretchScale;
            c.sortMode = d.sortMode;
            c.blend = d.blend;
        });
    });

    // ========================================================= Soft Particles
    module("Soft Particles", "soft", false, &fx->softParticles,
               "Fade a particle out as it approaches scene geometry, so it\n"
               "never cuts a hard square edge where it meets a wall.", [&] {
        ImGui::SetNextItemWidth(-110.0f);
        const float sd0 = fx->softDistance;
        ImGui::SliderFloat("Soft Distance##fx", &fx->softDistance, 0.0f, 5.0f,
                           "%.2f m");
        fieldEdit(m_ctx, "Emitter Soft Distance", &fx->softDistance, sd0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Distance over which the fade happens (metres).\n"
                              "Too large and particles floating well in front of\n"
                              "a wall go transparent; 0.2-0.5 is the useful\n"
                              "range. The renderer takes the MAX over all\n"
                              "emitters.");
        }
        logRect("INS-FX-SOFT", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        markField("SoftDistance");

        modReset("Soft Particles", [](auto& c, const auto& d) {
            c.softParticles = d.softParticles;
            c.softDistance = d.softDistance;
        });
    });

    // 一次性强制全展开/全折叠只在**本帧**有效 —— 上面那些模块已经吃到了。
    m_fxForceModules = -1;

    hint("needs the global switch: MYVK_FX=1 (off by default - then no "
         "particle pass is recorded and no pool is allocated at all)");
    hint("pool sizing is a build-time knob: MYVK_FX_CAPACITY (per emitter) x "
         "MYVK_FX_EMITTERS; watch for the FX-POOL warning");
    hint("right-click the section header above for Copy / Paste / Reset on the "
         "whole emitter");

    const std::string sig = modKeys + "\n" + fieldNames;
    if (sig != m_lastFxParamSig) {
        m_lastFxParamSig = sig;
        const std::string* nm = w.name(e);
        VK_LOG_INFO("FX-PARAMS entity=\"%s\" modules=%s fields=%s",
                    nm ? nm->c_str() : "?",
                    modKeys.empty() ? "(none)" : modKeys.c_str(),
                    fieldNames.empty() ? "(none)" : fieldNames.c_str());
    }

    if (ImGui::Button("Remove Particle Emitter")) {
        m_ctx.structuralEdit("Remove Particle Emitter", [&]() {
            w.remove<ecs::ParticleEmitterComponent>(e);
        });
        m_lastFxParamSig.clear();
    }
}


void InspectorPanel::drawMeshSection(ecs::Entity e) {
    scene::Scene& sc = m_ctx.activeScene();
    ecs::World& w = sc.world();

    auto* mc = w.get<ecs::MeshComponent>(e);
    if (!mc) return;

    // 标题栏的矩形也要报：Mesh 段默认是折叠的（自动化运行时 ini 不落盘，
    // 且 E11 之前折叠状态不持久，所以恒定折叠），验收脚本得先点开它才能
    // 读到参数。E11 之后折叠按名持久化，但自动化每次都是全新会话，
    // 首帧仍然是折叠 —— 脚本行为不变。
    const bool meshOpen = sectionHeader("Mesh", false, e);
    logRect("INS-MESH-HEADER", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (!meshOpen) return;

    if (!mc->mesh) {
        ImGui::TextDisabled("(null mesh)");
        return;
    }

    const assets::Mesh& m = *mc->mesh;
    const assets::MeshSource& s = m.source();

    // ---- 身份（只读）----
    static const char* kKinds[] = {"Unknown", "Builtin", "OBJ", "glTF"};
    const int ki = static_cast<int>(s.kind);
    ImGui::Text("asset: %s", kKinds[(ki >= 0 && ki < 4) ? ki : 0]);
    if (!s.shape.empty()) ImGui::Text("shape: %s", s.shape.c_str());
    if (!s.path.empty()) ImGui::Text("file : %s", s.path.c_str());
    if (s.subMeshIndex >= 0)
        ImGui::Text("sub  : primitive #%d", s.subMeshIndex);
    if (!s.name.empty()) {
        ImGui::TextDisabled("key  : %s", s.name.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "AssetManager cache key.\n"
                "Two meshes share GPU buffers exactly when their keys match.");
        }
    }

    // ---- 几何统计（只读）----
    ImGui::SeparatorText("Geometry");
    ImGui::Text("vertices : %zu", m.vertices().size());
    ImGui::Text("triangles: %zu", m.indices().size() / 3);
    if (m.hasBounds()) {
        const glm::vec3 mn = m.boundsMin();
        const glm::vec3 mx = m.boundsMax();
        const glm::vec3 ext = mx - mn;
        ImGui::Text("size     : %.3f x %.3f x %.3f",
                    static_cast<double>(ext.x), static_cast<double>(ext.y),
                    static_cast<double>(ext.z));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("local AABB\nmin (%.3f, %.3f, %.3f)\nmax (%.3f, %.3f, %.3f)",
                              static_cast<double>(mn.x), static_cast<double>(mn.y),
                              static_cast<double>(mn.z), static_cast<double>(mx.x),
                              static_cast<double>(mx.y), static_cast<double>(mx.z));
        }
    }
    hint("read-only - these come from the file or from the built-in generator");

    // ---- 生成参数（可拖拽；只有内置图元才有）----
    ImGui::SeparatorText("Parameters");

    if (!mc->params.isBuiltin()) {
        hint("OBJ / glTF geometry is defined by the file - nothing to tune here");
        return;
    }
    if (w.has<ecs::LockedComponent>(e)) {
        hint("locked - primitive parameters are disabled for this entity");
        return;
    }

    // 撤销要用"改之前"的整份组件（参数 + 当时那颗 mesh）
    const ecs::MeshComponent before = *mc;
    ecs::MeshPrimitiveParams& p = mc->params;
    bool edited = false;

    // 控件矩形探针：tools/verify_mesh_params.py 靠这些日志定位滑块，然后用
    // 真实鼠标去拖 —— 和验证材质拖拽是同一套路子。
    const auto probe = [](const char* label) {
        const std::string tag = std::string("MESH-PARAM ") + label;
        logRect(tag.c_str(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    };

    std::string labels;   // 本帧画出的参数名，喂给下面的签名日志

    switch (p.primitive) {
        case ecs::MeshPrimitive::Cube:
        case ecs::MeshPrimitive::Plane: {
            ImGui::SetNextItemWidth(-110.0f);
            ImGui::DragFloat("Size", &p.size, sp(0.01f), 0.01f, 500.0f, "%.3f");
            probe("Size");
            edited |= ImGui::IsItemDeactivatedAfterEdit();
            labels = "Size";
            break;
        }
        case ecs::MeshPrimitive::Sphere: {
            ImGui::SetNextItemWidth(-110.0f);
            ImGui::DragFloat("Radius", &p.radius, sp(0.005f), 0.001f, 200.0f, "%.3f");
            probe("Radius");
            edited |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::SetNextItemWidth(-110.0f);
            ImGui::DragInt("Segments", &p.segments, 0.5f, 3, 256);
            probe("Segments");
            edited |= ImGui::IsItemDeactivatedAfterEdit();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("longitude divisions - drives the vertex count");
            ImGui::SetNextItemWidth(-110.0f);
            ImGui::DragInt("Rings", &p.rings, 0.5f, 2, 256);
            probe("Rings");
            edited |= ImGui::IsItemDeactivatedAfterEdit();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("latitude divisions - drives the vertex count");
            labels = "Radius,Segments,Rings";
            break;
        }
        case ecs::MeshPrimitive::Cylinder: {
            ImGui::SetNextItemWidth(-110.0f);
            ImGui::DragFloat("Radius", &p.radius, sp(0.005f), 0.001f, 200.0f, "%.3f");
            probe("Radius");
            edited |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::SetNextItemWidth(-110.0f);
            ImGui::DragInt("Segments", &p.segments, 0.5f, 3, 256);
            probe("Segments");
            edited |= ImGui::IsItemDeactivatedAfterEdit();
            labels = "Radius,Segments";
            hint("height is fixed at 2 x radius (Blender's default cylinder)");
            break;
        }
        case ecs::MeshPrimitive::None:
            break;
    }

    // "这一帧到底画了哪些参数" —— 只在签名变化时打一行，验收脚本据此断言
    // 不同图元的**结构性差异**（Cube 只有 Size；Sphere 多出 Segments/Rings）。
    {
        const char* kindName = "?";
        switch (p.primitive) {
            case ecs::MeshPrimitive::Cube: kindName = "Cube"; break;
            case ecs::MeshPrimitive::Plane: kindName = "Plane"; break;
            case ecs::MeshPrimitive::Sphere: kindName = "Sphere"; break;
            case ecs::MeshPrimitive::Cylinder: kindName = "Cylinder"; break;
            case ecs::MeshPrimitive::None: kindName = "None"; break;
        }
        // 顺带把资产缓存键也带上：键里编着生成参数（builtin/cube/s1.9000），
        // 所以这一行同时能证明"参数有没有活着走过存盘/读盘"。
        const std::string key = mc->mesh ? mc->mesh->source().name : "";
        const std::string sig =
            std::string(kindName) + "|" + labels + "|" + key;
        if (sig != m_lastMeshParamSig) {
            m_lastMeshParamSig = sig;
            VK_LOG_INFO("MESH-PARAMS kind=%s labels=%s key=%s", kindName,
                        labels.empty() ? "(none)" : labels.c_str(),
                        key.empty() ? "(none)" : key.c_str());
        }
    }

    hint("the geometry is regenerated when you release the mouse - undoable");

    if (edited) commitPrimitiveEdit(e, before);
}

void InspectorPanel::commitPrimitiveEdit(ecs::Entity e,
                                         const ecs::MeshComponent& before) {
    scene::Scene& sc = m_ctx.activeScene();
    ecs::World& w = sc.world();
    auto* mc = w.get<ecs::MeshComponent>(e);
    if (!mc) return;

    // 参数值已经由 DragFloat 写进组件了，这里负责把它变成几何。
    // 内部只走 AssetManager 的缓存键查表 —— 参数没变就是纯查表，零开销。
    rebuildPrimitive(m_ctx.assets(), *mc);

    const std::string who = [&] {
        const auto* nm = w.get<ecs::NameComponent>(e);
        return nm && !nm->name.empty() ? nm->name : std::string("mesh");
    }();
    const assets::MeshSource& s =
        mc->mesh ? mc->mesh->source() : assets::MeshSource{};
    VK_LOG_INFO("primitive rebuild: '%s' %s size=%.4f radius=%.4f seg=%d rings=%d "
                "-> %zu verts (key %s)",
                who.c_str(), s.shape.c_str(), static_cast<double>(mc->params.size),
                static_cast<double>(mc->params.radius), mc->params.segments,
                mc->params.rings, mc->mesh ? mc->mesh->vertices().size() : 0,
                s.name.c_str());

    if (!m_ctx.isEditing()) return;

    m_ctx.commands().pushAlreadyApplied(std::make_unique<PrimitiveEditCommand>(
        "Rebuild " + who, mc, &m_ctx.assets(), before, *mc));
    m_ctx.setStatus("Rebuild " + who);
    m_ctx.dirty() = true;
}

// ---------------------------------------------------------------- 材质

namespace {

// 材质纹理的缓存键。
//
// ⚠ 必须把 sRGB 编进键里。AssetManager 只按名字缓存，而同一张图片当
// albedo（要 sRGB 解码）和当 normal（必须保持线性）需要的是**两种不同的
// GPU 纹理**（VK_FORMAT_R8G8B8A8_SRGB vs _UNORM，见 Texture.h）。
// 共用一个键的话第二次会拿回第一次那张 —— 法线贴图被 gamma 过一次，
// 光照方向就全歪了，而且这种错很难查。
std::string slotCacheKey(const std::string& rel, bool srgb) {
    return srgb ? rel : rel + "#linear";
}

std::string lowerExtOf(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string e = path.substr(dot);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return e;
}

bool isImagePath(const std::string& path) {
    const std::string e = lowerExtOf(path);
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".bmp" ||
           e == ".tga";
}

// 按名字取缩略图缓存键。纹理是共享资产、指针稳定，所以"有名字用名字、
// 没名字（程序化 solid/checker）退回指针"就够了。
std::string thumbKeyFor(const assets::Texture* t) {
    if (!t) return {};
    const assets::TextureSource& s = t->source();
    if (!s.name.empty()) return "slot:" + s.name;
    char buf[40];
    std::snprintf(buf, sizeof(buf), "slot:@%p", static_cast<const void*>(t));
    return buf;
}

// "#linear" 只是 slotCacheKey 用来区分色彩空间分支的内部标记，纯粹给缓存
// 看的。它必须留在键里，但绝不能显示到界面上（"xxx.png#linear" 看着像文
// 件名的一部分，很容易让人以为磁盘上真有这么个文件）。
std::string stripVariantSuffix(std::string s) {
    static const std::string kMarker = "#linear";
    if (s.size() > kMarker.size() &&
        s.compare(s.size() - kMarker.size(), kMarker.size(), kMarker) == 0) {
        s.erase(s.size() - kMarker.size());
    }
    return s;
}

std::string displayNameOf(const assets::Texture* t) {
    if (!t) return "(default)";
    const assets::TextureSource& s = t->source();
    if (!s.name.empty()) return stripVariantSuffix(s.name);
    if (!s.path.empty()) return stripVariantSuffix(s.path);
    switch (s.kind) {
        case assets::TextureSource::Kind::Solid: return "(solid colour)";
        case assets::TextureSource::Kind::Checker: return "(checkerboard)";
        default: break;
    }
    return "(procedural)";
}

void truncateLeft(std::string& s, size_t maxChars) {
    if (s.size() <= maxChars) return;
    s = "..." + s.substr(s.size() - (maxChars - 3));
}

// 槽位方块的尺寸。空着的时候就是一个"带加号的小方块"，绑定之后向右
// 长成一条胶囊（缩略图 + 文件名）。
//
// 28 而不是更小：它是**唯一**的拖放落点，大小直接决定好不好点中；行高
// 也随之变成 28，六行排下来属性之间自然拉开，不再挤成一片。
constexpr float kChipH = 28.0f;
constexpr float kChipBox = 28.0f;

// ---- 三列布局：属性名 | 值 | 槽位方块 ----
//
// 两列都定宽，于是"标签起点"和"方块左边缘"在六行之间天然对齐 —— 这是
// "不拥挤"的关键。原来靠 SetNextItemWidth 把标签甩在控件后面，每行的
// 起点取决于标签宽度，六行看下来参差不齐。
//
// 88 是量出来的：最长的属性名 "Normal Scale" 在默认字体下约 84px，留
// 4px 余量。面板默认只有 330px 宽（SplitLayout::kRightMin），两列
// 88+88 加上两次间距之后就只剩 ~116px 给槽位 —— kChipMaxW 也不需要
// 原来那个 190 了，反正吃不满。
constexpr float kLabelWidth = 88.0f;
constexpr float kValueWidth = 88.0f;
// 数值控件与标签之间、方块与值之间的呼吸空间（比默认 ItemSpacing 略大）
constexpr float kColGap = 10.0f;

constexpr float kChipMinW = kChipBox;  // 空槽就是那个小方块
constexpr float kChipMaxW = 160.0f;    // 上限（面板更宽时不至于拉成一长条）

// 绑定后显示的是**文件名**，不是整条资产路径：这么小的方块放不下
// "models/Cube/Cube_BaseColor.png"，而路径的前缀在同一个材质里几乎总是
// 重复的，真正有信息量的是最后那一段。
std::string fileNameOf(const std::string& pathOrName) {
    const size_t slash = pathOrName.find_last_of("/\\");
    return slash == std::string::npos ? pathOrName
                                      : pathOrName.substr(slash + 1);
}

// 把名字裁到放得下为止：**从左边截**，保住扩展名 —— 一眼就能看出它是
// 颜色图还是数据图，这比保住文件名的前缀有用得多（同目录下前缀往往一样）。
std::string fitText(const std::string& full, float availPx) {
    if (full.empty()) return full;
    if (ImGui::CalcTextSize(full.c_str()).x <= availPx) return full;
    for (size_t keep = full.size(); keep > 3; --keep) {
        const std::string cand = "..." + full.substr(full.size() - keep);
        if (ImGui::CalcTextSize(cand.c_str()).x <= availPx) return cand;
    }
    return full.substr(full.size() - std::min<size_t>(full.size(), 4));
}

// 槽位当前"显示的是什么"的签名日志（只在变化时打一行）。
// 用途和 Mesh 的 MESH-PARAMS 一样：**让"方块里显示的是文件名"这件事
// 可被自动化断言**。靠截图认字既脆又慢，而这是界面文案的唯一来源 ——
// tools/verify_material_chips.py 断言的就是它。
//
// prefix：整行前缀。默认 MAT-CHIP（材质区）；贴花区的槽位同样叫
// albedo / normal，日志里必须能区分，所以它打 DECAL-CHIP。
void logChipContent(const char* slotId, const std::string& shown,
                    const char* prefix = "MAT-CHIP") {
    if (!std::getenv("MYVK_LOG_RECTS")) return;
    // key 带上 prefix：两个区的同名槽位各自独立去抖
    const std::string key = std::string(prefix) + "|" + slotId;
    static std::unordered_map<std::string, std::string> last;
    auto it = last.find(key);
    if (it != last.end() && it->second == shown) return;
    last[key] = shown;
    VK_LOG_INFO("%s %s shows=\"%s\"", prefix, slotId,
                shown.empty() ? "(empty)" : shown.c_str());
}

} // namespace

bool InspectorPanel::drawTextureChip(const char* slotId, const char* hintText,
                                     assets::Texture** slot, bool srgb,
                                     float maxWidth, const char* logTag) {
    bool changed = false;

    ImGui::PushID(slotId);

    assets::Texture* tex = *slot;
    const ThumbnailCache::Entry* th =
        tex ? m_thumbs.getBound(thumbKeyFor(tex), tex) : nullptr;
    const bool hasPixels = th && th->ds != VK_NULL_HANDLE;

    std::string name = tex ? fileNameOf(displayNameOf(tex)) : std::string();

    // 空槽恰好是那个小方块；有内容才向右长（宽度受面板剩余空间限制）
    const float boxW = std::max(std::min(maxWidth, kChipMaxW), kChipMinW);
    if (!name.empty()) name = fitText(name, boxW - kChipBox - 12.0f);
    const ImVec2 textSz =
        name.empty() ? ImVec2(0.0f, 0.0f) : ImGui::CalcTextSize(name.c_str());
    const float w = name.empty()
                        ? kChipBox
                        : std::min(kChipBox + 6.0f + textSz.x + 6.0f, boxW);

    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + w, p0.y + kChipH);
    // Dummy：占位 + 成为一个合法的拖放落点。
    // ⚠ 它给 ItemAdd 传的 ID 是 0，而 BeginDragDropTarget 在 ID 为 0 时会
    //   退回 GetIDFromRectangle()，所以"整个方块"天然就是热区，不需要
    //   额外的 InvisibleButton（tools/verify_material_drag.py 依赖这个行为）。
    ImGui::Dummy(ImVec2(w, kChipH));

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool hovered = ImGui::IsItemHovered();
    const ImGuiPayload* payload = ImGui::GetDragDropPayload();
    const bool dropReady =
        payload != nullptr && payload->IsDataType(drag::kAsset);

    // ---- 底与边 ----
    const ImU32 bg = hasPixels ? IM_COL32(18, 19, 23, 255)
                               : (hovered ? IM_COL32(62, 65, 73, 255)
                                          : IM_COL32(46, 48, 54, 255));
    dl->AddRectFilled(p0, p1, bg, 3.0f);
    dl->AddRect(p0, p1,
                dropReady ? IM_COL32(252, 218, 62, 230)
                          : (hovered ? IM_COL32(150, 156, 166, 230)
                                     : IM_COL32(96, 100, 110, 210)),
                3.0f, 0, dropReady ? 2.0f : 1.0f);

    const ImVec2 box0 = p0;
    const ImVec2 box1(p0.x + kChipBox, p0.y + kChipH);

    if (hasPixels) {
        // 缩略图铺满方框，再补一圈描边把它和胶囊的底色分开
        dl->AddImage(static_cast<ImTextureID>(
                         reinterpret_cast<std::uintptr_t>(th->ds)),
                     box0, box1, ImVec2(0, 0), ImVec2(1, 1));
        dl->AddRect(box0, box1, IM_COL32(96, 100, 110, 200), 3.0f, 0, 1.0f);
        if (!name.empty()) {
            dl->AddText(ImVec2(box1.x + 6.0f, p0.y + (kChipH - textSz.y) * 0.5f),
                        IM_COL32(206, 210, 218, 255), name.c_str());
        }
    } else if (tex) {
        // 有纹理对象但缩略图还没就绪（或加载失败）：画个问号
        const ImVec2 ts = ImGui::CalcTextSize("?");
        dl->AddText(ImVec2(box0.x + (kChipBox - ts.x) * 0.5f,
                           box0.y + (kChipH - ts.y) * 0.5f),
                    IM_COL32(146, 150, 158, 255), "?");
    } else {
        // 空槽：正中一个加号。两笔就够，不必引图标字体。
        const ImVec2 c((box0.x + box1.x) * 0.5f, (box0.y + box1.y) * 0.5f);
        const ImU32 pc = dropReady ? IM_COL32(252, 218, 62, 255)
                                   : IM_COL32(150, 156, 166, 255);
        const float a = 5.0f;
        dl->AddLine(ImVec2(c.x - a, c.y), ImVec2(c.x + a, c.y), pc, 1.6f);
        dl->AddLine(ImVec2(c.x, c.y - a), ImVec2(c.x, c.y + a), pc, 1.6f);
    }

    if (hovered) {
        ImGui::SetTooltip("%s\n\n%s", hintText ? hintText : slotId,
                          tex ? "drag another image here to replace\n"
                                "right-click to clear"
                              : "drag an image from the Content Browser here");
    }

    // ---- 右键清空（方块太小，放不下 Clear 按钮）----
    if (tex && ImGui::BeginPopupContextItem("##clear")) {
        if (ImGui::MenuItem("Clear texture")) {
            *slot = nullptr;
            changed = true;
            VK_LOG_INFO("material slot '%s' <- (cleared)", slotId);
        }
        ImGui::EndPopup();
    }

    // 自动化用（MYVK_LOG_RECTS=1）：槽位的屏幕矩形。脚本据此把 Content
    // 面板里的图片拖过来 —— 见 tools/verify_material_drag.py。
    // 贴花区复用同一个控件，但 tag 前缀换成 DECAL-SLOT：两个区都有
    // albedo / normal 两个槽，日志里必须分得开。
    {
        const std::string tag = std::string(logTag) + " " + slotId;
        logRect(tag.c_str(), p0, p1);
        logChipContent(slotId, name, std::strcmp(logTag, "MAT-SLOT") == 0
                                         ? "MAT-CHIP"
                                         : "DECAL-CHIP");
    }

    // ---- 整个方块都是落点 ----
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(drag::kAsset)) {
            const char* rel = static_cast<const char*>(pl->Data);
            if (rel && *rel) {
                if (!isImagePath(rel)) {
                    m_ctx.setStatus(std::string("not an image: ") + rel);
                    VK_LOG_WARN("material slot '%s': '%s' is not an image",
                                slotId, rel);
                } else {
                    try {
                        // slotCacheKey：同样的图当颜色贴图和数据贴图用时是
                        // 两份不同的 GPU 纹理（sRGB 解码与否），必须分开缓存
                        *slot = m_ctx.assets().loadTexture(
                            assets::resolveAssetPath(rel),
                            slotCacheKey(rel, srgb), srgb);
                        changed = true;
                        VK_LOG_INFO("material slot '%s' <- %s (srgb=%d)",
                                    slotId, rel, srgb ? 1 : 0);
                    } catch (const std::exception& ex) {
                        VK_LOG_ERROR("material slot '%s': load '%s' failed: %s",
                                     slotId, rel, ex.what());
                    }
                }
            }
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::PopID();
    return changed;
}

void InspectorPanel::drawMaterialSection(ecs::Entity e) {
    scene::Scene& sc = m_ctx.activeScene();
    ecs::World& w = sc.world();

    auto* matc = w.get<ecs::MaterialComponent>(e);
    if (!matc || !matc->material) return;
    assets::Material* mat = matc->material;

    if (!sectionHeader("Material", true, e))
        return;

    ImGui::Text("name: %s", mat->name.c_str());

    // ---- 三列布局：属性名 | 值 | 纹理槽方块 ----
    //
    // 每帧在**第一行的行首**取一次 rowX0，它就是"第 1 列的左缘"。
    // 之后所有列都用绝对偏移：SameLine(offset) 的 offset 是**窗口坐标系**
    // 的横坐标（imgui.cpp:11708），所以必须基于 rowX0 加，直接写 88 会被
    // 窗口 padding 顶掉一截，而且行数一多还会各偏一点。
    //
    // 两列都定宽 = 各行的"值"和"方块"各自落在同一条竖直线上。之前每行
    // 的方块都跟在标签屁股后面（标签长短不一），看下来参差不齐 ——
    // 那正是"拥挤"的来源。
    const float rowX0 = ImGui::GetCursorPosX();
    const float rowRight = ImGui::GetWindowContentRegionMax().x;
    const float colValue = rowX0 + kLabelWidth + kColGap;
    const float colChip = colValue + kValueWidth + kColGap;

    auto rowBegin = [&](const char* label) {
        // AlignTextToFramePadding：文字要跟右边的输入框在**同一条基线上**，
        // 否则标签会贴着行顶，看起来像没对齐。
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(colValue);
    };

    // 七个槽各自的旧值 —— 一次拖放 = 一条撤销记录。
    // 材质是**共享资产**（AssetManager 按名字缓存，可能被多个实体引用），
    // 所以只能用按字节回滚的 RawBytesEditCommand，不能用整场景快照。
    assets::Texture* const a1 = mat->albedoMap;
    assets::Texture* const mt1 = mat->metallicMap;
    assets::Texture* const rg1 = mat->roughnessMap;
    assets::Texture* const em1 = mat->emissiveMap;
    assets::Texture* const nm1 = mat->normalMap;
    assets::Texture* const or1 = mat->ormMap;
    assets::Texture* const hd1 = mat->heightMap;   // R4-3 POM 高度图

    // 跳到第 3 列画槽位方块。avail 用"面板右缘 - 方块左缘"算 —— 这样
    // 面板被拖窄时方块不会越界（tools/verify_material_chips.py 断言这条）。
    auto chipAt = [&](const char* slotId, const char* undoName, const char* tip,
                      assets::Texture** slot, assets::Texture* before,
                      bool srgb) {
        ImGui::SameLine(colChip);
        const float avail = rowRight - ImGui::GetCursorPosX() - 4.0f;
        if (drawTextureChip(slotId, tip, slot, srgb, avail))
            discreteEdit(m_ctx, undoName, slot, before);
    };

    // ---- Base Color ----
    // 颜色用 NoInputs：第 2 列只有 88px，塞 4 个数值框进去必然挤成一团。
    // 换成一个色块按钮，点开才是完整取色器 —— 既省地方又看得清当前颜色。
    const glm::vec4 c0 = mat->baseColorFactor;
    rowBegin("Base Color");
    ImGui::SetNextItemWidth(kValueWidth);
    ImGui::ColorEdit4("##base-color", glm::value_ptr(mat->baseColorFactor),
                      ImGuiColorEditFlags_NoInputs);
    fieldEdit(m_ctx, "Material Base Color", &mat->baseColorFactor, c0);
    chipAt("albedo", "Material Albedo", "Base colour map (sRGB)",
           &mat->albedoMap, a1, /*srgb=*/true);

    // ---- Metallic ----
    const float m0 = mat->metallic;
    rowBegin("Metallic");
    ImGui::SetNextItemWidth(kValueWidth);
    ImGui::SliderFloat("##metallic", &mat->metallic, 0.0f, 1.0f, "%.2f");
    fieldEdit(m_ctx, "Material Metallic", &mat->metallic, m0);
    chipAt("metallic", "Material Metallic Map",
           "Metallic map (grayscale, linear). Multiplied by the slider on the left.",
           &mat->metallicMap, mt1, /*srgb=*/false);

    // ---- Roughness ----
    const float r0 = mat->roughness;
    rowBegin("Roughness");
    ImGui::SetNextItemWidth(kValueWidth);
    ImGui::SliderFloat("##roughness", &mat->roughness, 0.02f, 1.0f, "%.2f");
    fieldEdit(m_ctx, "Material Roughness", &mat->roughness, r0);
    chipAt("roughness", "Material Roughness Map",
           "Roughness map (grayscale, linear). Multiplied by the slider on the left.",
           &mat->roughnessMap, rg1, /*srgb=*/false);

    // ---- Emissive ----
    const glm::vec3 e0 = mat->emissive;
    rowBegin("Emissive");
    ImGui::SetNextItemWidth(kValueWidth);
    ImGui::ColorEdit3("##emissive", glm::value_ptr(mat->emissive),
                      ImGuiColorEditFlags_NoInputs);
    fieldEdit(m_ctx, "Material Emissive", &mat->emissive, e0);
    chipAt("emissive", "Material Emissive Map",
           "Emissive map (sRGB). ADDED to the colour on the left - dropping "
           "one lights the surface up even with the colour left at black.",
           &mat->emissiveMap, em1, /*srgb=*/true);

    // ---- AO / Normal Scale ----
    // 这两项没有"自己的"贴图（AO 来自 ORM 的 R，法线是整张图），槽位挂的
    // 是它们真正消费的那张纹理。
    const float a0 = mat->ao;
    rowBegin("AO");
    ImGui::SetNextItemWidth(kValueWidth);
    ImGui::SliderFloat("##ao", &mat->ao, 0.0f, 1.0f, "%.2f");
    fieldEdit(m_ctx, "Material AO", &mat->ao, a0);
    chipAt("orm", "Material ORM",
           "ORM packed map: Occlusion(R) / Roughness(G) / Metallic(B), linear",
           &mat->ormMap, or1, /*srgb=*/false);

    const float n0 = mat->normalScale;
    rowBegin("Normal Scale");
    ImGui::SetNextItemWidth(kValueWidth);
    ImGui::SliderFloat("##normal-scale", &mat->normalScale, 0.0f, 2.0f, "%.2f");
    fieldEdit(m_ctx, "Material Normal Scale", &mat->normalScale, n0);
    chipAt("normal", "Material Normal",
           "Tangent-space normal map, kept linear",
           &mat->normalMap, nm1, /*srgb=*/false);

    // ---- Height Scale + 高度图（R4-3 POM）----
    // 挂了高度图 → 关键字 bit5 VENN_PARALLAX 置位，片元先沿切线空间视向把
    // UV 推开再采样其余贴图（平面上的砖缝/凹槽会随视角"错位"）。尺度 0
    // 时 offset 恒 0，画面与没挂高度图逐像素相同。
    // 滑条上限 0.2：UV 位移超过两成就会出现明显的"纹理被拉长"穿帮感。
    const float hs0 = mat->heightScale;
    rowBegin("Height Scale");
    ImGui::SetNextItemWidth(kValueWidth);
    ImGui::SliderFloat("##height-scale", &mat->heightScale, 0.0f, 0.2f, "%.3f");
    fieldEdit(m_ctx, "Material Height Scale", &mat->heightScale, hs0);
    chipAt("height", "Material Height Map",
           "Parallax height map (grayscale, linear, REPEAT). Drop one in to\n"
           "enable parallax occlusion mapping; the slider on the left is the\n"
           "depth in UV units (0 = off, pixel-identical to no height map).",
           &mat->heightMap, hd1, /*srgb=*/false);

    // ---- 杂项（同样两列：名字 + 值，没有槽位）----
    const int modeBefore = static_cast<int>(mat->alphaMode);
    int mode = modeBefore;
    const char* kAlpha[] = {"Opaque", "Blend", "Mask"};
    rowBegin("Alpha Mode");
    ImGui::SetNextItemWidth(kValueWidth);
    if (ImGui::Combo("##alpha-mode", &mode, kAlpha, 3)) {
        const auto newMode = static_cast<assets::Material::AlphaMode>(mode);
        const auto oldMode = mat->alphaMode;
        if (newMode != oldMode) {
            mat->alphaMode = newMode;
            m_ctx.commands().pushAlreadyApplied(
                std::make_unique<LambdaCommand>(
                    "Material Alpha Mode",
                    [mat, oldMode]() { mat->alphaMode = oldMode; },
                    [mat, newMode]() { mat->alphaMode = newMode; }));
            m_ctx.dirty() = true;
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Opaque - alpha ignored, always depth-writes.\n"
            "Blend  - alpha-weighted, drawn back-to-front, no depth write.\n"
            "Mask   - binary cutout at the alpha cutoff (foliage / fences).");
    }
    // ---- Mask（alpha 裁切）专属：阈值 + 抖动模式（R4-1）----
    // 只在 Mask 模式下出现 —— 其它模式这两个字段不参与着色器编译，显示
    // 出来只会让人以为"调了没反应"。
    if (mat->alphaMode == assets::Material::AlphaMode::Mask) {
        const float c0 = mat->alphaCutoff;
        rowBegin("Alpha Cutoff");
        ImGui::SetNextItemWidth(kValueWidth);
        ImGui::SliderFloat("##alpha-cutoff", &mat->alphaCutoff, 0.0f, 1.0f,
                           "%.2f");
        fieldEdit(m_ctx, "Material Alpha Cutoff", &mat->alphaCutoff, c0);

        bool dither = mat->ditherAlpha;
        rowBegin("Dither Alpha");
        if (ImGui::Checkbox("##dither-alpha", &dither)) {
            const bool ob = mat->ditherAlpha;
            mat->ditherAlpha = dither;
            discreteEdit(m_ctx, "Material Dither Alpha", &mat->ditherAlpha, ob);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Stochastic alpha test: keep the fragment with a Bayer-dither\n"
                "threshold instead of a fixed cutoff. Still writes depth (unlike\n"
                "Blend), and TAA turns the dithered edge into a soft one.");
        }
    }

    // ---- 着色模型（R4-2）----
    // Default = 标准金属-粗糙度 BRDF；Skin = 预积分次表面散射（漫反射改查
    // (N·L, 曲率) LUT，鼻梁/耳缘泛红透光）。它是**关键字位**（bit4），所以
    // 切一下就能从下面 "Shader Var" 那行看到 k 值跟着变 —— 生效与否可自证。
    {
        int sm = static_cast<int>(mat->shadingModel);
        const char* kShading[] = {"Default", "Skin"};
        rowBegin("Shading Model");
        ImGui::SetNextItemWidth(kValueWidth);
        if (ImGui::Combo("##shading-model", &sm, kShading, 2)) {
            const auto newSm = static_cast<assets::Material::ShadingModel>(sm);
            const auto oldSm = mat->shadingModel;
            if (newSm != oldSm) {
                mat->shadingModel = newSm;
                m_ctx.commands().pushAlreadyApplied(
                    std::make_unique<LambdaCommand>(
                        "Material Shading Model",
                        [mat, oldSm]() { mat->shadingModel = oldSm; },
                        [mat, newSm]() { mat->shadingModel = newSm; }));
                m_ctx.dirty() = true;
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Default = metallic-roughness BRDF.\n\n"
                "Skin = pre-integrated subsurface scattering: the diffuse term\n"
                "looks up a baked (N.L, curvature) LUT instead of max(N.L,0),\n"
                "so light wraps around the terminator and the red channel bleeds\n"
                "furthest -> nose bridge / ear rims glow red. Default materials\n"
                "are compiled without this code path entirely (pixel-identical).");
        }
    }

    bool ds = mat->doubleSided;
    rowBegin("Double Sided");
    if (ImGui::Checkbox("##double-sided", &ds)) {
        const bool ob = mat->doubleSided;
        mat->doubleSided = ds;
        discreteEdit(m_ctx, "Material Double Sided", &mat->doubleSided, ob);
    }

    // ---- 这个材质会被编成哪个着色器变体（见 render/ShaderKeywords.h）----
    // 只读。上面那些控件（Double Sided / Alpha Mode / Shading Model /
    // Normal 槽位）的组合
    // 结果直接写出来，省得去猜"双面 + 挂了法线图"到底是哪一份 .spv ——
    // 也省得在改了开关后看不出"到底有没有生效"。
    //
    // 这一行就是关键字系统的用户可见面：材质属性 → 掩码 → 变体文件，
    // 一条规则，三处（CMake 的 -D / pbr.frag 的 #if / 管线固定状态）共用。
    //
    // 排版：值列只放 `k5` —— 值列到面板右边只剩 ~216px，把名字也塞进去时
    // 全开三个关键字（`k7 (double-sided|alpha-blend|normal-map)` 约 294px）
    // 会被 ImGui 直接裁掉、还不报错。名字另起一行、跟着值列对齐写，
    // 宽了就自己换行。变体文件名放 tooltip。
    {
        const render::KeywordMask kw = render::keywordsOf(*mat);
        // 标签必须 <= 12 字符：标签列只有 kLabelWidth(88) px，写
        // "Shader Variant"(14) 会和值列挤在一起（实测粘成 "Shader Variantk5"）。
        rowBegin("Shader Var");
        ImGui::TextDisabled("k%u", kw);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "%s\n\n"
                "关键字位（与 CMakeLists.txt 的 SHADER_KEYWORDS 一一对应）：\n"
                "  bit0  double-sided  VENN_DOUBLE_SIDED  关背面剔除\n"
                "  bit1  alpha-blend   VENN_ALPHA_BLEND   走混合、不写深度\n"
                "  bit2  normal-map    VENN_NORMAL_MAP    采样法线贴图 + TBN\n"
                "  bit3  alpha-mask    VENN_ALPHA_MASK    alpha 裁切（discard）\n"
                "  bit4  shading-skin  VENN_SHADING_SKIN 预积分皮肤散射 LUT\n"
                "\n没挂法线图时 bit2 为 0：那段采样与 TBN 会被整段编掉，"
                "改为绑一张内置平坦法线，两者像素等价。\n"
                "\n若设了 MYVK_SHADER_FORCE_MASK，实际用的变体会被强制覆盖，"
                "Stats 面板会标出 FORCED。",
                render::variantShaderFile("pbr.frag", kw).c_str());
        }
        ImGui::TextDisabled("  %s", render::keywordNames(kw).c_str());
    }
}

// ---------------------------------------------------------------- 光照
//
// 三种灯在引擎里是**三个独立的组件类型**（ecs/Components.h 刻意没有搞一个
// LightType 枚举 —— "挂了哪个组件"就是类型本身），所以"不同灯显示不同参数"
// 是天然成立的：下面每一支只画自己组件里真实存在的字段。
//
// 顶部那个 Type 下拉是额外的便利：切换 = 换组件。能带过去的参数（颜色 /
// 强度 / 方向）会带过去，其余用该类型的默认值。
//
// 唯一的例外是**场景主光**（scene::Scene::light() 返回的那盏方向光）：
// 它的类型不允许改，否则 Scene::light() 会当场给你再补一盏方向光出来，
// 实体上就同时挂着两盏灯了。

namespace {

void convertLightType(ecs::World& w, ecs::Entity e, int to) {
    // 先取值再删组件 —— 删完指针就悬垂了
    glm::vec3 color(1.0f, 0.98f, 0.95f);
    glm::vec3 dir(0.0f, 0.0f, -1.0f);
    float intensity = 1.0f;

    if (const auto* d = w.get<ecs::DirectionalLightComponent>(e)) {
        color = d->color;
        dir = d->direction;
        intensity = d->intensity;
    } else if (const auto* p = w.get<ecs::PointLightComponent>(e)) {
        color = p->color;
        intensity = p->intensity;
    } else if (const auto* s = w.get<ecs::SpotLightComponent>(e)) {
        color = s->color;
        dir = s->direction;
        intensity = s->intensity;
    }

    if (w.has<ecs::DirectionalLightComponent>(e))
        w.remove<ecs::DirectionalLightComponent>(e);
    if (w.has<ecs::PointLightComponent>(e)) w.remove<ecs::PointLightComponent>(e);
    if (w.has<ecs::SpotLightComponent>(e)) w.remove<ecs::SpotLightComponent>(e);

    switch (to) {
        case 0: {
            ecs::DirectionalLightComponent c;
            c.color = color;
            c.direction = dir;
            c.intensity = intensity;
            w.add<ecs::DirectionalLightComponent>(e, c);
            break;
        }
        case 1: {
            ecs::PointLightComponent c;
            c.color = color;
            c.intensity = intensity;
            w.add<ecs::PointLightComponent>(e, c);
            break;
        }
        case 2: {
            ecs::SpotLightComponent c;
            c.color = color;
            c.direction = dir;
            c.intensity = intensity;
            w.add<ecs::SpotLightComponent>(e, c);
            break;
        }
        default:
            break;
    }
}

} // namespace

void InspectorPanel::drawLights(ecs::Entity e) {
    scene::Scene& sc = m_ctx.activeScene();
    ecs::World& w = sc.world();

    auto* pl = w.get<ecs::PointLightComponent>(e);
    auto* sl = w.get<ecs::SpotLightComponent>(e);
    auto* dl = w.get<ecs::DirectionalLightComponent>(e);
    if (!pl && !sl && !dl) return;

    if (!sectionHeader("Light", true, e))
        return;

    // 控件矩形探针 + "这一帧画了哪些字段"的签名（同 Mesh 段，供
    // tools/verify_light_params.py 断言三种灯的参数集合各不相同）
    const auto probe = [](const char* label) {
        const std::string tag = std::string("LIGHT-PARAM ") + label;
        logRect(tag.c_str(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    };
    std::string labels;

    // ---- 类型 ----
    const int typeNow = dl ? 0 : (pl ? 1 : 2);
    const char* kTypes[] = {"Directional (sun)", "Point", "Spot"};

    // 场景主光的类型锁死：改掉它会让 Scene::light() 自动补一盏新的方向光
    const bool isKeyLight = (sc.lightEntity() == e);

    int type = typeNow;
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::BeginDisabled(isKeyLight);
    const bool typeChanged = ImGui::Combo("Type", &type, kTypes, 3);
    probe("Type");
    ImGui::EndDisabled();
    if (isKeyLight) {
        hint("scene key light - its type is fixed (see scene::Scene::light)");
    } else if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("switching the type keeps colour / intensity / direction");
    }

    if (typeChanged && type != typeNow && m_ctx.isEditing()) {
        m_ctx.structuralEdit("Change Light Type", [&] { convertLightType(w, e, type); });
    }

    // ---- Directional ----
    if (dl) {
        ImGui::SeparatorText("Directional");

        const glm::vec3 d0 = dl->direction;
        ImGui::DragFloat3("Direction", glm::value_ptr(dl->direction), sp(0.01f), -1.0f, 1.0f);
        probe("Direction");
        fieldEdit(m_ctx, "Sun Direction", &dl->direction, d0);

        const glm::vec3 c0 = dl->color;
        ImGui::ColorEdit3("Color", glm::value_ptr(dl->color));
        probe("Color");
        fieldEdit(m_ctx, "Sun Color", &dl->color, c0);

        const float i0 = dl->intensity;
        ImGui::SliderFloat("Intensity", &dl->intensity, 0.0f, 4.0f);
        probe("Intensity");
        fieldEdit(m_ctx, "Sun Intensity", &dl->intensity, i0);

        const float a0 = dl->ambientScale;
        ImGui::SliderFloat("Ambient", &dl->ambientScale, 0.0f, 1.0f, "%.2f");
        probe("Ambient");
        fieldEdit(m_ctx, "Sun Ambient Scale", &dl->ambientScale, a0);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("procedural IBL strength\nindoor scenes: 0.1 - 0.3");

        bool cs = dl->castsShadow;
        if (ImGui::Checkbox("Casts Shadow", &cs)) {
            dl->castsShadow = cs;
            discreteEdit(m_ctx, "Sun Casts Shadow", &dl->castsShadow, !cs);
        }
        labels = "Direction,Color,Intensity,Ambient,CastsShadow";
        hint("no range / cone: a sun is parallel light from infinitely far away");
    }

    // ---- Point ----
    if (pl) {
        ImGui::SeparatorText("Point");

        const glm::vec3 c0 = pl->color;
        ImGui::ColorEdit3("Color", glm::value_ptr(pl->color));
        probe("Color");
        fieldEdit(m_ctx, "Point Light Color", &pl->color, c0);

        const float i0 = pl->intensity;
        ImGui::DragFloat("Intensity", &pl->intensity, sp(0.2f), 0.0f, 500.0f, "%.1f");
        probe("Intensity");
        fieldEdit(m_ctx, "Point Light Intensity", &pl->intensity, i0);

        const float r0 = pl->range;
        ImGui::DragFloat("Range", &pl->range, sp(0.05f), 0.1f, 60.0f, "%.2f");
        probe("Range");
        fieldEdit(m_ctx, "Point Light Range", &pl->range, r0);
        labels = "Color,Intensity,Range,Enabled";
        hint("range drives cluster culling - keep it tight");

        bool en = pl->enabled;
        if (ImGui::Checkbox("Enabled", &en)) {
            pl->enabled = en;
            discreteEdit(m_ctx, "Toggle Point Light", &pl->enabled, !en);
        }
        if (removeButton() && m_ctx.isEditing()) {
            m_ctx.structuralEdit("Remove Point Light",
                                 [&]() { w.remove<ecs::PointLightComponent>(e); });
        }
    }

    // ---- Spot ----
    if (sl) {
        ImGui::SeparatorText("Spot");

        const glm::vec3 c0 = sl->color;
        ImGui::ColorEdit3("Color", glm::value_ptr(sl->color));
        probe("Color");
        fieldEdit(m_ctx, "Spot Light Color", &sl->color, c0);

        const float i0 = sl->intensity;
        ImGui::DragFloat("Intensity", &sl->intensity, sp(0.4f), 0.0f, 800.0f, "%.1f");
        probe("Intensity");
        fieldEdit(m_ctx, "Spot Light Intensity", &sl->intensity, i0);

        const float r0 = sl->range;
        ImGui::DragFloat("Range", &sl->range, sp(0.05f), 0.1f, 80.0f, "%.2f");
        probe("Range");
        fieldEdit(m_ctx, "Spot Light Range", &sl->range, r0);

        const glm::vec3 d0 = sl->direction;
        ImGui::DragFloat3("Direction", glm::value_ptr(sl->direction), sp(0.01f), -1.0f, 1.0f);
        probe("Direction");
        fieldEdit(m_ctx, "Spot Light Direction", &sl->direction, d0);

        float innerDeg = glm::degrees(sl->innerAngle);
        float outerDeg = glm::degrees(sl->outerAngle);
        const float in0 = sl->innerAngle;
        const float out0 = sl->outerAngle;
        if (ImGui::SliderFloat("Inner Cone", &innerDeg, 1.0f, 80.0f, "%.1f deg")) {
            sl->innerAngle = glm::radians(innerDeg);
        }
        fieldEdit(m_ctx, "Spot Inner Cone", &sl->innerAngle, in0);
        if (ImGui::SliderFloat("Outer Cone", &outerDeg, 1.0f, 89.0f, "%.1f deg")) {
            sl->outerAngle = glm::radians(outerDeg);
        }
        fieldEdit(m_ctx, "Spot Outer Cone", &sl->outerAngle, out0);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("penumbra = outer - inner\nnarrow the gap for a harder edge");

        // ---- 光源修饰（R3-3）：区域光尺寸 / IES / light function ----
        // 三者都共享灯的局部正交基，所以只有射灯有这几项。真正的总开关
        // 在渲染器（MYVK_AREA_LIGHT / MYVK_IES / MYVK_COOKIE）—— 这里只
        // 写"这盏灯想用什么"。
        ImGui::SeparatorText("Modifiers");
        {
            const float aw0 = sl->areaWidth;
            ImGui::DragFloat("Area Width", &sl->areaWidth, sp(0.05f), 0.0f, 40.0f,
                             "%.2f");
            probe("AreaWidth");
            fieldEdit(m_ctx, "Spot Area Width", &sl->areaWidth, aw0);
            const float ah0 = sl->areaHeight;
            ImGui::DragFloat("Area Height", &sl->areaHeight, sp(0.05f), 0.0f, 40.0f,
                             "%.2f");
            probe("AreaHeight");
            fieldEdit(m_ctx, "Spot Area Height", &sl->areaHeight, ah0);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "rect area light size in metres (0 = point-like)\n"
                    "softens the highlight; needs MYVK_AREA_LIGHT=1");

            static const char* kIesItems[] = {"(none)", "ies:narrow",
                                              "ies:wide", "ies:batwing"};
            static const char* kCookieItems[] = {"(none)", "cookie:grid",
                                                 "cookie:rings",
                                                 "cookie:bars"};
            auto nameCombo = [](const char* label, std::string& value,
                                const char* const* items, int n) {
                int cur = 0;
                for (int i = 1; i < n; ++i)
                    if (value == items[i]) cur = i;
                if (ImGui::Combo(label, &cur, items, n))
                    value = (cur == 0) ? std::string() : std::string(items[cur]);
            };
            nameCombo("IES", sl->iesProfile, kIesItems, 4);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("IES photometric web\nneeds MYVK_IES=1");
            nameCombo("Light Function", sl->lightFunction, kCookieItems, 4);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("projected cookie texture\nneeds MYVK_COOKIE=1");
        }

        labels = "Color,Intensity,Range,Direction,InnerCone,OuterCone,"
                 "Area,IES,Cookie,Enabled";

        bool en = sl->enabled;
        if (ImGui::Checkbox("Enabled", &en)) {
            sl->enabled = en;
            discreteEdit(m_ctx, "Toggle Spot Light", &sl->enabled, !en);
        }
        if (removeButton() && m_ctx.isEditing()) {
            m_ctx.structuralEdit("Remove Spot Light",
                                 [&]() { w.remove<ecs::SpotLightComponent>(e); });
        }
    }

    // "这一帧画了哪些字段" —— 只在签名变化时打一行。三种灯的集合本来就该
    // 互不相同，验收脚本直接断言这个差异，不用去截图里认控件。
    {
        const char* typeName = dl ? "Directional" : (pl ? "Point" : "Spot");
        const std::string sig = std::string(typeName) + "|" + labels;
        if (sig != m_lastLightParamSig) {
            m_lastLightParamSig = sig;
            VK_LOG_INFO("LIGHT-PARAMS type=%s labels=%s", typeName,
                        labels.empty() ? "(none)" : labels.c_str());
        }
    }
}

// ---------------------------------------------------------------- 脚本

void InspectorPanel::drawScript(ecs::Entity e) {
    scene::Scene& sc = m_ctx.activeScene();
    ecs::World& w = sc.world();
    auto* scomp = w.get<ecs::ScriptComponent>(e);
    if (!scomp) return;

    if (!sectionHeader("Script (.vks)", true, e, "Script"))
        return;

    if (m_scriptBufFor != e) {
        std::snprintf(m_scriptBuf, sizeof(m_scriptBuf), "%s",
                      scomp->path.c_str());
        m_scriptBufFor = e;
    }

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##scriptpath", "scripts/foo.vks", m_scriptBuf,
                             sizeof(m_scriptBuf));
    if (ImGui::IsItemDeactivatedAfterEdit() && m_ctx.isEditing()) {
        const std::string newPath = m_scriptBuf;
        if (newPath != scomp->path) {
            m_ctx.structuralEdit("Edit Script Path",
                                 [&]() { scomp->path = newPath; });
        }
    }
    hint("relative to the asset root, or an absolute path");

    bool en = scomp->enabled;
    if (ImGui::Checkbox("Enabled##script", &en)) {
        scomp->enabled = en;
        discreteEdit(m_ctx, "Toggle Script", &scomp->enabled, !en);
    }
    ImGui::SameLine();
    const float ts0 = scomp->timeScale;
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderFloat("Time Scale", &scomp->timeScale, 0.0f, 4.0f, "%.2f");
    fieldEdit(m_ctx, "Script Time Scale", &scomp->timeScale, ts0);

    // 脚本只在运行态求值 —— 这条提示很重要，否则用户会以为坏了
    if (m_ctx.isEditing()) {
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.3f, 1.0f),
                           "Scripts only run in Play mode.");
    }

    if (removeButton() && m_ctx.isEditing()) {
        m_ctx.structuralEdit("Remove Script",
                             [&]() { w.remove<ecs::ScriptComponent>(e); });
        m_scriptBufFor = ecs::Entity{};
    }
}

// ---------------------------------------------------------------- 增删组件

void InspectorPanel::drawAddRemove(ecs::Entity e) {
    // 结构性改动（加/删组件）只在编辑态可用：运行态改的是副本，
    // 而且撤销栈里的整场景快照只认编辑态那份场景。
    if (!m_ctx.isEditing()) return;

    scene::Scene& sc = m_ctx.editorScene();
    ecs::World& w = sc.world();

    ImGui::Separator();
    if (ImGui::Button("Add Component")) ImGui::OpenPopup("add_component");
    if (ImGui::BeginPopup("add_component")) {
        const bool hasPoint = w.has<ecs::PointLightComponent>(e);
        const bool hasSpot = w.has<ecs::SpotLightComponent>(e);
        const bool hasScript = w.has<ecs::ScriptComponent>(e);
        const bool hasDecal = w.has<ecs::DecalComponent>(e);

        if (ImGui::MenuItem("Point Light", nullptr, false, !hasPoint)) {
            m_ctx.structuralEdit("Add Point Light", [&]() {
                ecs::PointLightComponent pl;
                w.add<ecs::PointLightComponent>(e, pl);
            });
        }
        if (ImGui::MenuItem("Spot Light", nullptr, false, !hasSpot)) {
            m_ctx.structuralEdit("Add Spot Light", [&]() {
                ecs::SpotLightComponent sl;
                w.add<ecs::SpotLightComponent>(e, sl);
            });
        }
        if (ImGui::MenuItem("Script", nullptr, false, !hasScript)) {
            m_ctx.structuralEdit("Add Script", [&]() {
                ecs::ScriptComponent scp;
                // 路径留空，让用户自己填（不再指向任何示例脚本 —— 示例内容
                // 已经按用户要求从项目里删掉了，硬写一个不存在的路径只会
                // 让 Scripts 面板一上来就报编译错误）
                scp.path.clear();
                w.add<ecs::ScriptComponent>(e, scp);
            });
            m_scriptBufFor = ecs::Entity{};
        }
        // 延迟贴花（R4-4）：默认 1 m 立方体、不透明度 1、白色 tint，
        // 两张贴图都留空 —— 于是它一加出来就是个"能看见的纯色方块"，
        // 把贴图拖进 Inspector 的 Decal 段才算真正有内容。
        // 位置/朝向直接吃实体的 Transform，不需要额外初始化。
        if (ImGui::MenuItem("Decal", nullptr, false, !hasDecal)) {
            m_ctx.structuralEdit("Add Decal", [&]() {
                ecs::DecalComponent d;
                w.add<ecs::DecalComponent>(e, d);
            });
            m_lastDecalParamSig.clear();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "A box-volume decal projected onto whatever surface is inside "
                "it.\nPlace it with the same W/E/R gizmo as the object itself.");
        }

        // 反射探针（R5-1）：默认 10×10×5 的盒子、强度 1、开盒投影。
        // 一加出来就"能烘"—— 但全局开关（MYVK_PROBES）默认关，所以
        // 真正看到反射还要在 Engine 面板里打开 probe.enabled。
        const bool hasProbe = w.has<ecs::ReflectionProbeComponent>(e);
        if (ImGui::MenuItem("Reflection Probe", nullptr, false, !hasProbe)) {
            m_ctx.structuralEdit("Add Reflection Probe", [&]() {
                ecs::ReflectionProbeComponent p;
                w.add<ecs::ReflectionProbeComponent>(e, p);
            });
            m_lastProbeParamSig.clear();
            // 顺手请求一次捕获：不然新加的探针要等下一帧自动判定才烘。
            m_ctx.requestProbeCapture();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Captures the surroundings into a cubemap and reflects it on\n"
                "everything inside the box - fills in what SSR cannot see\n"
                "(off-screen geometry). Needs MYVK_PROBES=1.");
        }

        // GPU 粒子发射器（R6-1）：默认是"一加出来就能看见的火焰"——
        // 加色混合 + 暖色三段渐变 + 向上初速。没有网格（几何是顶点着
        // 色器现拼的 billboard），位置 / 朝向吃实体的 Transform：
        // **局部 +Z 就是喷射方向**。
        const bool hasFx = w.has<ecs::ParticleEmitterComponent>(e);
        if (ImGui::MenuItem("Particle Emitter", nullptr, false, !hasFx)) {
            m_ctx.structuralEdit("Add Particle Emitter", [&]() {
                ecs::ParticleEmitterComponent fx;
                w.add<ecs::ParticleEmitterComponent>(e, fx);
            });
            m_lastFxParamSig.clear();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Spawns particles from this object's origin along its local\n"
                "+Z. Simulated entirely on the GPU (compute) and drawn with\n"
                "one indirect draw - CPU never counts them.\n"
                "Needs the global switch MYVK_FX=1.");
        }
        ImGui::EndPopup();
    }
}

} // namespace editor
