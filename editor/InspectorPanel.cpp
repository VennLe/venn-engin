#include "InspectorPanel.h"

#include "Localization.h"
#include "DebugRects.h"
#include "EditorDragDrop.h"
#include "EditorLayoutIO.h"
#include "InspectorIcons.h"
#include "PrimitiveBuilder.h"

#include "ui/ImGuiTheme.h"

#include "assets/AssetManager.h"
#include "assets/AssetPath.h"
#include "assets/Material.h"
#include "assets/MaterialImport.h"
#include "assets/MaterialLibrary.h"
#include "assets/Mesh.h"
#include "assets/Model.h"
#include "FeatureUIGen.h"

#include "assets/Texture.h"
#include "core/Logger.h"
#include "ecs/Components.h"
#include "physics/CollisionWorld.h"
#include "render/FeatureRegistry.h"
#include "render/ShaderKeywords.h"
#include "scene/Camera.h"
#include "scene/Light.h"
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"
#include "terrain/Terrain.h"

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

// 属性行里"数值控件 + 右侧标签"的槽位宽度。
//
// 面板最小宽 330px（SplitLayout::kRightMin），扣掉图标栏 ~48、内边距和竖向
// 滚动条之后，一行只剩 ~250px 可用。槽位给 140 时，`Dynamic Friction`
// `Angular Thresh` `Freeze frame` 这类标签会被**面板右缘和滚动条切掉**
// （2026-10-16 截图像素级放大肉眼确认：`Max Substep` / `Freeze fra` /
// `Angular Thres`）。数值控件并不需要那么宽 —— 把 30px 让给标签。
constexpr float kPhysParamW = 110.0f;  // 物理页（世界 + 刚体）
constexpr float kFxParamW = 140.0f;    // 粒子 FX 全局参数（标签更短）

// 说明文字的统一出口 —— **不上屏，只在悬浮时出现**。
//
// 2026-10-17 用户要求："面板上不要有任何注释说明，只有鼠标悬浮在相应区域
// 才显示。" 于是这个函数从"画一行灰字"变成了"给上一个控件挂 tooltip"。
//
// 为什么调用点一行都不用改：所有 hint 都紧跟在**它要解释的那个控件 / 分区
// 标题**之后（这是 E11 以来一直的写法），而 ImGui 的 IsItemHovered() 在
// 同一帧里对"刚刚提交的那个 item"永远有效 —— 所以把文字挂到 last item 上
// 正好就是"悬浮在相应区域"。
//
// ⚠ 副作用（有意为之）：一行 hint 不再占垂直空间。窄侧栏里这才是想要的，
//   以前那些换行说明会吃掉半屏。
void hint(const char* text) {
    if (!text || !text[0]) return;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled |
                             ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("%s", text);
    }
}

// 组件的"移除"按钮（跟在同行控件后面）
bool removeButton() {
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.16f, 0.16f, 1.0f));
    const bool clicked = ImGui::SmallButton(TR("Remove"));
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
        // G14-7 音频源。与粒子那条同一条纪律：**每一个字段**都要列
        //（少一个词，用户搜 "rolloff" 就会看到 "no component matches"）。
        {"Audio Source",
         "audio source sound sfx music voice clip volume pitch loop spatial "
         "3d distance min max rolloff attenuation falloff bus streaming "
         "play on awake listener"},
        {"Camera", "camera fov target distance yaw pitch"},
        // M2（G07~G09）：组件级关键词必须把**每一个字段**都列进来，
        // 否则用户搜 "step" / "trigger" 会看到"no component matches"
        //（与下面粒子发射器那条注释同一条教训）。
        {"Camera Component",
         "camera component fov priority enabled follow target offset arm length "
         "min distance collide world shake amplitude frequency trauma"},
        {"Character Controller",
         "character controller capsule radius half height step height slope "
         "limit ground snap distance gravity scale max speed sprint "
         "acceleration air control turn speed push strength walk run jump "
         "player pawn kinematic"},
        {"Trigger",
         "trigger volume enter stay exit overlap layer mask report events "
         "sensor"},
        {"Physics Material",
         "physics material override friction static dynamic restitution "
         "bounce density surface"},
        {"Directional Light", "sun direction ambient sky"},
        // 出生点（2026-10-24）：面板上所有字段都要能搜到。
        {"Player Start",
         "player start gamestart spawn spawnpoint respawn view mode first "
         "third person camera spawn on run enabled yaw heading orientation "
         "birth initial"},
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

// ---------------------------------------------------------------- 页签表
//
// 图标栏里的一项。**顺序必须与 InspectorPanel::InspTab 的整数值一致** ——
// 下面那条 static_assert 只能保证"个数对得上"，对不上顺序它看不出来，
// 所以加/删/挪页签时表与枚紧要一起改。
struct TabDesc {
    const char* id;      // 稳定名：落盘（LayoutState::inspTab）与
                         // MYVK_EDITOR_INSP_TAB 都用它，**改了就丢配置**
    const char* label;   // tooltip 标题
    const char* tip;     // tooltip 正文（一句话讲这页管什么）
    void (*icon)(ImDrawList*, ImVec2, float, ImU32);
};

const TabDesc kTabs[] = {
    {"object", "Object", "Transform / visibility / mesh geometry", iconObject},
    {"tool", "Tool", "Script behavior / add & remove components", iconTool},
    {"render", "Render", "Lights / decals / reflection probes", iconRender},
    {"output", "Output", "Viewport and render-output info", iconOutput},
    {"scene", "Scene", "Scene statistics", iconScene},
    {"world", "World", "Camera / main light", iconWorld},
    {"particles", "Particles", "GPU particle emitters", iconParticles},
    {"physics", "Physics", "Colliders", iconPhysics},
    {"material", "Material", "Material factors and textures", iconMaterial},
    {"animation", "Animation", "Skinning and animation clips", iconAnimation},
    {"all", "All", "Everything (the old stacked view)", iconAll},
};
constexpr int kTabCount = static_cast<int>(sizeof(kTabs) / sizeof(kTabs[0]));

static_assert(kTabCount == static_cast<int>(InspectorPanel::InspTab::Count),
              "kTabs[] must have exactly as many entries as InspectorPanel::InspTab");
// 最后一项必须是 All —— 自动化默认值与"搜索跨页签"都依赖它。
static_assert(static_cast<int>(InspectorPanel::InspTab::All) == kTabCount - 1,
              "All must be the last tab");

// 页签名（给日志用）；越界返回 "?"。
const char* tabNameAt(int i) {
    return (i >= 0 && i < kTabCount) ? kTabs[i].id : "?";
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

        // 页签注入：MYVK_EDITOR_INSP_TAB=<名字|下标>。
        //
        // ⚠ **自动化运行下默认选 All**。项目里已有 9 个验证脚本靠
        //   INS-MESH-HEADER / INS-COLLISION-HEADER / INS-PROBE-HEADER /
        //   INS-FX-EXPAND-ALL 这类矩形锚点断言"那个分区被画出来了"。
        //   分区现在按页签分散到不同页面，脚本不指定页签就只看得到 Object
        //   页的内容 → 会整批误报。默认值取"全部"一次性保住了它们，
        //   脚本想单独测某一页再显式设这个变量。
        //   "自动化"的判定与 EditorLayoutIO 用同一条（MYVK_FRAMES /
        //   MYVK_NO_WINDOW_SAVE），不另造一个口径。
        if (const char* envT = std::getenv("MYVK_EDITOR_INSP_TAB")) {
            int want = tabIndexById(envT);
            if (want < 0) {
                // 也接受下标（0..kTabCount-1）：脚本循环遍历页签时更省事。
                char* end = nullptr;
                const long v = std::strtol(envT, &end, 10);
                if (end && *end == '\0' && v >= 0 && v < kTabCount) {
                    want = static_cast<int>(v);
                }
            }
            if (want >= 0) {
                setActiveTab(want);
                core::logMessage(core::LogLevel::Info,
                                 "INSP-TAB injected=\"%s\" tab=%s", envT,
                                 tabNameAt(m_tab));
            } else {
                core::logMessage(core::LogLevel::Warn,
                                 "INSP-TAB unrecognized \"%s\" (ignored)", envT);
            }
        } else if (editorLayoutAutomationRun()) {
            setActiveTab(static_cast<int>(InspTab::All));
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
    if (!ImGui::Begin(trId("Inspector", "panel_inspector"), &panels.inspector, wf)) {
        ImGui::End();
        return;
    }

    // 面板自身的屏幕矩形。验收脚本靠它判断"某个锚点是不是被滚出可视区了"——
    // `logRect` 报的是 `GetItemRectMin/Max`，那是**未裁剪**的坐标：控件滚出
    // 滚区之后照样有坐标，照它的中心点下去会落到面板外面（表现为"点了没反应"
    // 且截图前后像素级一致，很像功能坏了，其实是点空了）。
    // 和 `CONSOLE-PANEL` / `FO-PANEL` 同一个写法。
    logRect("INS-PANEL", ImGui::GetWindowPos(),
            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x,
                   ImGui::GetWindowPos().y + ImGui::GetWindowSize().y));

    scene::Scene& sc = m_ctx.activeScene();
    const bool hasSel = m_ctx.hasSelection();
    const ecs::Entity e = hasSel ? m_ctx.selection() : ecs::Entity{};

    // ---- 头部：实体名（跨页签常驻，翻页签时不会跟着滚走）----
    //
    // 刻意放在**导航栏上方**、占满整宽：它就是"我现在在编辑谁"，
    // 是整页的上下文，不该只在 Object 页里出现。
    if (hasSel) {
        const std::string* np = sc.world().name(e);
        ImGui::Text(TR("Entity: %s"), np ? np->c_str() : "(unnamed)");
        ImGui::SameLine();
        ImGui::TextDisabled(TR("#%u.%u"), e.id, e.generation);
        if (!m_ctx.isEditing()) {
            ImGui::SameLine();
            ImGui::TextColored(
                ImVec4(1.0f, 0.72f, 0.3f, 1.0f), TR("  (playing)"));
        }
    } else {
        // 没选中物体时**不再整页退出**：World / Scene / Output 三页与
        // "选中谁"无关，仍然该能看。其余页签各自会画一行"先选个物体"。
        ImGui::TextDisabled(TR("Nothing selected"));
        ImGui::SameLine();
        ImGui::TextDisabled(TR("(World / Scene / Output still work)"));
    }
    ImGui::Separator();

    // ---- E11 组件搜索：过滤分区与其字段关键词 ----
    // 搜索词非空时**跨所有页签**摊平命中项（见 drawTabContent），
    // 所以这行提示在头部，而不是塞进某一页里。
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##insp_search", TRL("search components..."),
                             m_search, sizeof(m_search));
    ImGui::Separator();

    // ---- 一级导航（窄图标列）+ 内容区 ----
    drawNavBar();
    ImGui::SameLine();
    ImGui::BeginChild("##insp_content", ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_None);
    drawTabContent(e);
    drawNoMatchHint();
    ImGui::EndChild();

    // INSP-TAB：只在页签真的变了时打一行（去抖），供
    // tools/verify_inspector_tabs.py 断言"点哪个图标生效了"。
    {
        const char* cur = tabNameAt(m_tab);
        if (m_lastTabSig != cur) {
            m_lastTabSig = cur;
            core::logMessage(core::LogLevel::Info, "INSP-TAB selected=%s", cur);
        }
    }

    logSectionState();
    ImGui::End();
}

// ---------------------------------------------------------------- 一级导航

// 左侧那一条窄窄的图标列。
//
// 几个刻意的地方：
//   · 用 InvisibleButton + 自绘，而不是 Button/Selectable —— 要的是
//     "方形的图标块"，不是一行文字；而且选中态想画成圆角填充块，
//     用标准控件拼不出来（拼出来也要跟主题色打架）。
//   · 每个按钮都 logRect("INS-NAV <id>")：验证脚本靠它拿到点击坐标，
//     也顺便断言"这一项确实存在"。没有这个锚点就只能去截图里认图标。
//   · 图标颜色：常态用 TextDisabled、悬停/选中用 Text —— 主题自带的一对，
//     亮/暗两套配色都好看，不用自己调 RGB。
void InspectorPanel::drawNavBar() {
    ImGuiStyle& st = ImGui::GetStyle();
    const float btn = ImGui::GetFrameHeight();   // 方形按钮边长（跟字号/DPI 走）
    const float colW = btn + st.WindowPadding.x * 2.0f;

    // 图标导航条的底：比面板再深一档（凹槽感）。浅色主题下换成浅一档，
    // 否则会留一条深色竖条。VennDark 走 else = 逐字段等于改动前的字面量。
    ImGui::PushStyleColor(ImGuiCol_ChildBg,
                          ui::tokens().isLight
                              ? ImVec4(0.90f, 0.91f, 0.925f, 1.0f)
                              : ImVec4(0.09f, 0.095f, 0.105f, 1.0f));
    ImGui::BeginChild("##insp_nav", ImVec2(colW, 0.0f), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 colIdle = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImU32 colHot = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 bgOn = ImGui::GetColorU32(ImGuiCol_Header);
    const ImU32 bgHover = ImGui::GetColorU32(ImGuiCol_HeaderHovered);
    const float round = st.FrameRounding;

    for (int i = 0; i < kTabCount; ++i) {
        // All 与上面十个是两回事（一个是"页"、一个是"全部摊平"），
        // 用一道空隙把它分出去，免得看起来像第 11 个平级页签。
        if (i == static_cast<int>(InspTab::All)) {
            ImGui::Dummy(ImVec2(0.0f, 4.0f));
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0.0f, 2.0f));
        }

        ImGui::PushID(i);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##tab", ImVec2(btn, btn));
        const bool hovered = ImGui::IsItemHovered();
        const bool active = (m_tab == i);
        if (ImGui::IsItemClicked()) setActiveTab(i);

        if (active) {
            dl->AddRectFilled(p0, ImVec2(p0.x + btn, p0.y + btn), bgOn, round);
            // 左侧一道短竖条 = 当前页的指示器（Blender 同款语义）
            const float h = btn * 0.55f;
            dl->AddRectFilled(
                ImVec2(p0.x + 1.0f, p0.y + (btn - h) * 0.5f),
                ImVec2(p0.x + 1.0f + 2.0f, p0.y + (btn + h) * 0.5f),
                colHot, 1.0f);
        } else if (hovered) {
            dl->AddRectFilled(p0, ImVec2(p0.x + btn, p0.y + btn), bgHover,
                              round);
        }

        // 0.165 是"看过的"值：0.20 时图标明显偏小、和 28px 的按钮不匹配；
        // 再小就没有留白了（线稿会顶到高亮圆角块的边上）。
        const float inset = btn * 0.165f;
        kTabs[i].icon(dl, ImVec2(p0.x + inset, p0.y + inset),
                      btn - inset * 2.0f,
                      (active || hovered) ? colHot : colIdle);

        logRect((std::string("INS-NAV ") + kTabs[i].id).c_str(), p0,
                ImVec2(p0.x + btn, p0.y + btn));

        if (hovered) {
            ImGui::SetTooltip(TR("%s  -  %s"), TR(kTabs[i].label),
                              TR(kTabs[i].tip));
        }
        ImGui::PopID();
    }

    ImGui::EndChild();
}

// 内容派发。搜索词非空时**忽略页签选择**、跨所有页签摊平 ——
// 和 Preferences 窗口的搜索行为保持一致：搜东西的时候不该还要先猜它在哪页。
void InspectorPanel::drawTabContent(ecs::Entity e) {
    if (m_search[0] != '\0') {
        ImGui::TextDisabled(TR("(searching across all tabs)"));
        drawTabAll(e);
        return;
    }
    switch (static_cast<InspTab>(m_tab)) {
        case InspTab::Object:    drawTabObject(e); break;
        case InspTab::Tool:      drawTabTool(e); break;
        case InspTab::Render:    drawTabRender(e); break;
        case InspTab::Output:    drawTabOutput(); break;
        case InspTab::Scene:     drawTabScene(); break;
        case InspTab::World:     drawTabWorld(e); break;
        case InspTab::Particles: drawTabParticles(e); break;
        case InspTab::Physics:   drawTabPhysics(e); break;
        case InspTab::Material:  drawTabMaterial(e); break;
        case InspTab::Animation: drawTabAnimation(e); break;
        case InspTab::All:       drawTabAll(e); break;
        case InspTab::Count:     break;
    }
}

// 页签需要组件、而这台实体上没有 → 说清楚为什么是空的。
void InspectorPanel::tabEmptyHint(const char* what) {
    // 日志锚点：**空页签本身也要可断言**。
    // 像 Animation 页，只有"没有 Skinned Mesh 组件"时才知道它被画到了 ——
    // 没有这一行，脚本只能靠"其它锚点都不在"来反推，那是空断言。
    // 去抖：只在(页签, 原因)变化时打一行。
    {
        const std::string sig = std::string(tabNameAt(m_tab)) + "/" + what;
        if (m_lastEmptySig != sig) {
            m_lastEmptySig = sig;
            core::logMessage(core::LogLevel::Info,
                             "INS-TAB-EMPTY tab=%s need=%s",
                             tabNameAt(m_tab), what);
        }
    }
    if (!m_ctx.hasSelection()) {
        ImGui::TextDisabled(TR("Nothing selected."));
        hint("Click an object in the viewport (or in the Hierarchy) to edit it.");
    } else {
        ImGui::TextDisabled(TR("This object has no %s."), what);
    }
}

// 页签状态访问（落盘 / env / 验证脚本都用 id 而不是下标：
// 下标会随着"往中间插一个页签"而整体漂移，id 不会）。
int InspectorPanel::tabCount() { return kTabCount; }

const char* InspectorPanel::tabId(int t) { return tabNameAt(t); }

int InspectorPanel::tabIndexById(const char* id) {
    if (!id) return -1;
    for (int i = 0; i < kTabCount; ++i) {
        if (std::strcmp(kTabs[i].id, id) == 0) return i;
    }
    return -1;
}

void InspectorPanel::setActiveTab(int t) {
    if (t < 0 || t >= kTabCount) return;
    m_tab = t;
}

// ---------------------------------------------------------------- 各页签内容

void InspectorPanel::drawTabObject(ecs::Entity e) {
    if (!e.valid()) { tabEmptyHint("an object"); return; }
    drawTransform(e);
    drawVisibility(e);
    drawMeshSection(e);      // 网格几何：只读资产信息 + 内置图元的生成参数
}

void InspectorPanel::drawTabTool(ecs::Entity e) {
    if (!e.valid()) { tabEmptyHint("an object"); return; }
    drawScript(e);
    drawAudioSource(e);
    drawAddRemove(e);
}

// 渲染页签 = "渲染细节属性设置"。
//
// 分两层，顺序固定：
//   ① **全局**（Rendering settings）—— 引擎级的渲染细节，从 FeatureRegistry
//      生成。**不需要选中物体**，所以不能像其它页签那样一进来就早退。
//   ② **物体级**（Object / Light / Decal / Reflection Probe）—— 只有选中
//      了带对应组件的实体才出现。
//
// ⚠ 这里刻意**不重复** Engine 面板的"全部特性 + 档位 + 搜索 + 预设"顶栏：
//   那是"全量旋钮"的入口，本页签只挑渲染相关的分组，并且共用同一个
//   tier 选择，避免两处出现不同的可见集合。
void InspectorPanel::drawTabRender(ecs::Entity e) {
    drawRenderSettingsSection();

    if (!e.valid()) {
        hint("Select an object to edit its per-object render properties "
             "(visibility, shadow casting, lights, decals, probes).");
        tabEmptyHint("an object");
        return;
    }
    drawObjectRenderSection(e);
    drawLights(e);
    drawDecalSection(e);
    drawReflectionProbeSection(e);
    drawTerrainSection(e);
}

// ---- 全局渲染细节（FeatureRegistry 驱动）----
void InspectorPanel::drawRenderSettingsSection() {
    const bool open = sectionHeader("Rendering", true);
    logRect("INS-RENDER-HEADER", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    if (!open) return;

    // 档位：与 Engine 面板同一套语义（0=Basic 1=Advanced 2=Debug）。
    // 静态 —— 它是"这次会话想看到多深"，不是场景数据，不该进撤销栈。
    static int tier = 1;   // 渲染页默认 Advanced：Basic 太少，Debug 太吵
    ImGui::TextUnformatted(TR("Detail"));
    ImGui::SameLine();
    ImGui::RadioButton(TRL("Basic##render"), &tier, 0);
    ImGui::SameLine();
    ImGui::RadioButton(TRL("Adv##render"), &tier, 1);
    ImGui::SameLine();
    ImGui::RadioButton(TRL("Debug##render"), &tier, 2);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            TR("How deep the parameter list goes.\n"
            "Same tiers as the Engine panel; values are the same storage."));
    }
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##renderfilter", TRL("Filter render params..."),
                             m_renderFilter, sizeof(m_renderFilter));

    // 只挑与"渲染"直接相关的分组。Lights / Scene 里的雾、Post 的后处理都
    // 属于"画面长什么样"，所以放进本页；Camera / Viewport 归视口与相机，
    // 不在这里重复。
    static const char* kGroups[] = {"Rendering Path", "Rendering", "Culling",
                                    "Surface", "Shadows", "Lights", "Post",
                                    "Scene", "Sky", "UI"};
    int shown = 0;
    for (const char* grp : kGroups) {
        bool first = true;
        for (int i = 0; i < render::kFeatureCount; ++i) {
            const render::FeatureDesc& f = render::kFeatures[i];
            if (!f.group || std::strcmp(f.group, grp) != 0) continue;
            if (!editor::drawFeatureBlock(f, tier, m_renderFilter, nullptr)) {
                continue;
            }
            if (first) {
                ImGui::SeparatorText(TR(grp));
                first = false;
            }
            ++shown;
        }
    }
    if (shown == 0) {
        ImGui::TextDisabled(TR("(no render settings at this detail level)"));
    }
    hint("Shared with the Engine panel - both write the same registry, so the "
         "values here and MYVK_* env vars are one source of truth.");
}

// ---- 物体级渲染属性 ----
void InspectorPanel::drawObjectRenderSection(ecs::Entity e) {
    ecs::World& w = m_ctx.editorScene().world();
    auto* vis = w.get<ecs::VisibilityComponent>(e);
    if (!vis) return;

    const bool open = sectionHeader("Object Rendering", true, e);
    logRect("INS-OBJ-RENDER-HEADER", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    if (!open) return;
    if (!m_ctx.isEditing()) {
        ImGui::TextDisabled(TR("run mode - read-only here"));
        return;
    }

    bool visible = vis->visible;
    if (ImGui::Checkbox(TR("Visible"), &visible)) {
        vis->visible = visible;
        discreteEdit(m_ctx, "Visible", &vis->visible, !visible);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Participates in the main forward pass."));
    }

    bool cast = vis->castShadow;
    if (ImGui::Checkbox(TR("Cast Shadow"), &cast)) {
        vis->castShadow = cast;
        discreteEdit(m_ctx, "Cast Shadow", &vis->castShadow, !cast);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            TR("Participates in the shadow map passes (CSM + local shadows).\n"
            "Turn off for ground planes and things that should only receive."));
    }
}

// 粒子页签。
//
// 改造前它只有"编辑选中物体上已有的发射器"这一条路 —— 想新建必须去 Tool 页
// 的 Add Component 里翻，而且新建出来的发射器**什么都看不见**（全局总开关
// 默认关）。现在把这条链路补成完整的一圈：
//
//   ① Preview —— 全局总开关 + 预览倍率（不需要选中任何物体）
//   ② Add     —— 一键在**世界原点**生成粒子特效，自动选中 + 自动打开总开关
//   ③ Emitter —— 选中物体的发射器参数（原有内容，一字未改）
//
// "实时预览"不需要额外机制：GPU 粒子每帧都在跑（见 Renderer 的发射器收集），
// 编辑态与 Play 态用的是**同一条模拟链**，差别只是渲染哪个场景。所以只要
// 总开关是开的、编辑场景里有一个发射器，视口里看到的就是游戏里的效果。
//
// 移动 / 旋转 / 缩放走**视口常规手柄**（W/E/R）—— 发射器实体是普通的
// TransformComponent 实体，和搬箱子没有任何区别，不必为它单做一套。
void InspectorPanel::drawTabParticles(ecs::Entity e) {
    drawParticlePreviewSection();
    drawParticleAddSection(e);

    if (e.valid() &&
        m_ctx.activeScene().world().has<ecs::ParticleEmitterComponent>(e)) {
        drawParticleEmitterSection(e);
    } else {
        tabEmptyHint("particle emitter");
        hint("Use \"Add Particle Effect at origin\" above, or attach one to the "
             "selected object.");
    }
}

// ---- ① 全局：总开关 + 预览倍率 ----
void InspectorPanel::drawParticlePreviewSection() {
    const bool open = sectionHeader("Particles", true);
    logRect("INS-FX-PREVIEW-HEADER", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    if (!open) return;

    // 这几个参数就是 FeatureRegistry 里 fx.* 那一组 —— 写的是**同一份存储**，
    // 所以这里拨一下，Engine 面板和 MYVK_FX_* 环境变量看到的是同一个值。
    auto boolParam = [this](const char* id, const char* label,
                            const char* tip) {
        void* p = render::paramPtr(id);
        if (!p) { ImGui::TextDisabled(TR("%s (unbound)"), label); return; }
        bool v = *static_cast<bool*>(p);
        if (ImGui::Checkbox(label, &v)) {
            *static_cast<bool*>(p) = v;
            editor::emitParamChange(id);
        }
        if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip(TR("%s"), tip);
    };
    auto floatParam = [this](const char* id, const char* label, float lo,
                             float hi, const char* fmt) {
        void* p = render::paramPtr(id);
        if (!p) { ImGui::TextDisabled(TR("%s (unbound)"), label); return; }
        float v = *static_cast<float*>(p);
        ImGui::SetNextItemWidth(kFxParamW);
        if (ImGui::SliderFloat(label, &v, lo, hi, fmt)) {
            *static_cast<float*>(p) = v;
            editor::emitParamChange(id);
        }
    };

    boolParam("fx.enabled", "Enable (live preview)",
              "The GPU particle system. Off = emitters draw nothing.\n"
              "Editing uses the same simulation as Play, so the viewport "
              "shows exactly what the game will show.");
    logRect("INS-FX-PREVIEW-ENABLE", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    ImGui::BeginDisabled(!render::paramPtr("fx.enabled") ||
                         !*static_cast<bool*>(render::paramPtr("fx.enabled")));
    floatParam("fx.sizeScale", "Size scale", 0.0f, 4.0f, "%.2f");
    floatParam("fx.intensity", "Intensity", 0.0f, 8.0f, "%.2f");
    floatParam("fx.softness", "Softness", 0.0f, 4.0f, "%.2f");
    floatParam("fx.freezeFrame", "Freeze frame", 0.0f, 600.0f, "%.0f");
    ImGui::EndDisabled();
    hint("Freeze frame pins the simulation to a fixed time - useful for "
         "reading a burst frame by frame.");
}

// ---- ② Add：在世界原点生成 ----
void InspectorPanel::drawParticleAddSection(ecs::Entity e) {
    const bool open = sectionHeader("Add Effect", true, e);
    logRect("INS-FX-ADD-HEADER", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    if (!open) return;
    if (!m_ctx.isEditing()) {
        ImGui::TextDisabled(TR("run mode - effects are read-only here"));
        return;
    }

    if (ImGui::Button(TR("Add Particle Effect at origin"), ImVec2(-1.0f, 0.0f))) {
        addParticleEffectAtOrigin();
    }
    logRect("INS-FX-ADD-ORIGIN", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            TR("Creates a new object at the world origin (0,0,0) carrying a\n"
            "particle emitter, selects it, and turns the particle system on."));
    }

    // 选中的物体还没有发射器时，给一条"挂上去"的近路（等价于 Tool 页的
    // Add Component，只是不用切页签）。
    ecs::World& w = m_ctx.editorScene().world();
    if (e.valid() && !w.has<ecs::ParticleEmitterComponent>(e)) {
        if (ImGui::Button(TR("Attach emitter to selected object"),
                          ImVec2(-1.0f, 0.0f))) {
            m_ctx.structuralEdit("Add Particle Emitter", [&]() {
                w.add<ecs::ParticleEmitterComponent>(
                    e, ecs::ParticleEmitterComponent{});
            });
            if (void* p = render::paramPtr("fx.enabled")) {
                *static_cast<bool*>(p) = true;
                editor::emitParamChange("fx.enabled");
            }
            m_lastFxParamSig.clear();
            m_ctx.setStatus(TR("Attached particle emitter"));
            VK_LOG_INFO("INS-FX-ATTACH entity=%u", e.id);
        }
        logRect("INS-FX-ATTACH", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());
    }
}

// ---- 在世界原点建一个粒子特效实体 ----
ecs::Entity InspectorPanel::addParticleEffectAtOrigin() {
    scene::Scene& sc = m_ctx.editorScene();
    ecs::Entity created{};
    m_ctx.structuralEdit("Add Particle Effect", [&]() {
        created = sc.createObject("Particle Effect");
        // createObject 给的 Transform 已经是单位变换（位置 = 世界原点），
        // 这里显式再写一次是为了**把意图钉在代码里** —— 用户要的就是
        // "默认坐标原点上生成"，以后有人改了 createObject 的默认值也不会
        // 悄悄把粒子挪走。
        if (auto* tc = sc.world().get<ecs::TransformComponent>(created)) {
            tc->position = glm::vec3(0.0f);
            tc->rotation = glm::vec3(0.0f);
            tc->scale = glm::vec3(1.0f);
        }
        sc.world().add<ecs::ParticleEmitterComponent>(
            created, ecs::ParticleEmitterComponent{});
    });
    if (!created.valid()) return created;

    m_ctx.select(created);
    m_ctx.setStatus(TR("Added particle effect at origin"));
    // 实时预览：总开关不开的话新特效是"隐形"的，用户会以为按钮坏了。
    if (void* p = render::paramPtr("fx.enabled")) {
        *static_cast<bool*>(p) = true;
        editor::emitParamChange("fx.enabled");
    }
    m_lastFxParamSig.clear();
    VK_LOG_INFO("INS-FX-ADD at=origin entity=%u", created.id);
    return created;
}

// ---------------------------------------------------------------- R7-1 物理
//
// Physics 页签 = "整个引擎的物理世界"的控制台。分三段：
//
//   ① Physics World —— 场景级参数（重力 / 步长 / 求解器 / 休眠）。写的是
//                      `Scene::physicsSettings()`，**随场景存盘** —— 一份
//                      场景一套手感，换场景就换物理。
//   ② Runtime       —— **只读**运行态统计（步数 / 体数 / 接触数 / 最大穿透）。
//                      数据由 EditorApp 每帧灌进 EditorContext（见
//                      PhysicsRuntimeInfo）。这一块是"物理到底在不在跑"的
//                      唯一可见证据。
//   ③ Rigid Body    —— 选中实体的刚体参数。没有就一键添加。
//
// 为什么世界参数和物体参数挤在同一页：调重力的时候十有八九正盯着某个物体
// 掉得对不对 —— 来回切页签会把注意力打散。
//
// ⚠ 仿真**只发生在 Play 态**（用户确认的选择）。所以本页所有旋钮改的都是
//   "初始条件"：改完要重新 Play 才生效。运行态下控件一律禁用并写明原因 ——
//   "能改但没反应"是最糟的交互。
void InspectorPanel::drawTabPhysics(ecs::Entity e) {
    drawPhysicsWorldSection();
    drawRigidBodySection(e);
    // 碰撞体也放这儿：它和刚体是**两件事**（前者只有位置分离、没有动力学，
    // 见 Component 里的说明），但刚体的 Auto 形状就是从它推断出来的 ——
    // "我在调的这两个东西互相有关"这件事只有摆在同一页才看得见。
    // 没有 CollisionComponent 时它自己提前 return，所以不会画出空段。
    if (e.valid()) drawCollisionSection(e);
    // M2（G07/G08）：角色控制器 / 触发体 / 物理材质。三段同样"有才画"。
    // 顺序是**故意**的：先角色（最常用），再触发体，最后材质覆盖 ——
    // 越靠下越是"只在特定场合才加"的东西。
    drawCharacterSection(e);
    drawTriggerSection(e);
    drawPhysicsMaterialSection(e);
}

void InspectorPanel::drawPhysicsWorldSection() {
    const bool open = sectionHeader("Physics World", true);
    logRect("INS-PHYS-WORLD-HEADER", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    if (!open) return;

    // 永远编辑**编辑场景**的那一份参数（即使此刻正在 Play）。理由：运行态
    // 是内存副本，Stop 就没了；而这两个面板共用一个指针的话，撤销命令里
    // 存的目标地址会在 Stop 之后悬垂（见 discreteEdit 的说明）。
    scene::PhysicsSettings& ps = m_ctx.editorScene().physicsSettings();
    const bool editable = m_ctx.isEditing();

    if (!editable) {
        ImGui::TextDisabled(TR("run mode - world settings are read-only here"));
        hint("Physics runs in the Play copy of the scene. Stop to edit these, "
             "then Play again - the values below are the initial conditions.");
    }
    ImGui::BeginDisabled(!editable);

    bool enabled = ps.enabled;
    if (ImGui::Checkbox(TR("Simulate Physics"), &enabled)) {
        ps.enabled = enabled;
        discreteEdit(m_ctx, "Physics Enabled", &ps.enabled, !enabled);
    }
    logRect("INS-PHYS-ENABLE", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            TR("Master switch. Off = Play does not build a physics world at all;\n"
            "rigid bodies stay exactly where you left them."));
    }

    bool ground = ps.groundPlane;
    if (ImGui::Checkbox(TR("Infinite Ground (z = 0)"), &ground)) {
        ps.groundPlane = ground;
        discreteEdit(m_ctx, "Physics Ground", &ps.groundPlane, !ground);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            TR("Adds an infinite static plane at z = 0 so things land instead of\n"
            "falling forever. Turn it off to get true free-fall, or place your\n"
            "own floor and give it a Static rigid body."));
    }

    ImGui::SeparatorText(TR("World"));
    const glm::vec3 g0 = ps.gravity;
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::DragFloat3("##phys-gravity", glm::value_ptr(ps.gravity), sp(0.05f));
    fieldEdit(m_ctx, "Gravity", &ps.gravity, g0);
    ImGui::SetItemTooltip(TRL("Gravity (m/s^2). Z-up world, so the default -9.81 "
                          "points down."));

    ImGui::SeparatorText(TR("Stepping"));
    // 固定步长：显示成 Hz 更直观（1/60 ↔ 60 Hz）
    const float step0 = ps.fixedTimeStep;
    ImGui::SetNextItemWidth(kPhysParamW);
    ImGui::SliderFloat(TRL("Step"), &ps.fixedTimeStep, 1.0f / 240.0f, 1.0f / 30.0f,
                       TR("%.5f s"));
    fieldEdit(m_ctx, "Physics Step", &ps.fixedTimeStep, step0);
    ImGui::SameLine();
    ImGui::TextDisabled(TR("= %.0f Hz"), 1.0f / std::max(ps.fixedTimeStep, 1e-6f));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            TR("Fixed simulation step. Smaller = more accurate + more expensive.\n"
            "The real frame time is an accumulator: several steps per frame\n"
            "when the frame rate drops, capped by Max Substeps (anti death "
            "spiral)."));
    }

    const int sub0 = ps.maxSubSteps;
    ImGui::SetNextItemWidth(kPhysParamW);
    ImGui::SliderInt(TRL("Max Substeps"), &ps.maxSubSteps, 1, 10);
    fieldEdit(m_ctx, "Physics Substeps", &ps.maxSubSteps, sub0);

    ImGui::SeparatorText(TR("Solver"));
    const int vi0 = ps.velocityIterations;
    ImGui::SetNextItemWidth(kPhysParamW);
    ImGui::SliderInt(TRL("Velocity Iters"), &ps.velocityIterations, 1, 30);
    fieldEdit(m_ctx, "Velocity Iterations", &ps.velocityIterations, vi0);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            TR("Sequential-impulse iterations per step. More = stiffer stacks\n"
            "(and a bit more CPU). 8-12 is the usual sweet spot."));
    }

    const int pi0 = ps.positionIterations;
    ImGui::SetNextItemWidth(kPhysParamW);
    ImGui::SliderInt(TRL("Position Iters"), &ps.positionIterations, 0, 12);
    fieldEdit(m_ctx, "Position Iterations", &ps.positionIterations, pi0);

    const float bau0 = ps.baumgarte;
    ImGui::SetNextItemWidth(kPhysParamW);
    ImGui::SliderFloat(TRL("Baumgarte"), &ps.baumgarte, 0.0f, 0.9f, "%.3f");
    fieldEdit(m_ctx, "Baumgarte", &ps.baumgarte, bau0);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            TR("How aggressively penetration is pushed out per step.\n"
            "Too high = jitter (energy injected), too low = visible overlap."));
    }

    const float slop0 = ps.slop;
    ImGui::SetNextItemWidth(kPhysParamW);
    ImGui::SliderFloat(TRL("Slop"), &ps.slop, 0.0f, 0.02f, TR("%.4f m"));
    fieldEdit(m_ctx, "Slop", &ps.slop, slop0);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            TR("Allowed penetration. A tiny amount is deliberate: without it the\n"
            "solver fights floating-point noise forever and the stack buzzes."));
    }

    ImGui::SeparatorText(TR("Sleeping"));
    bool sleep = ps.allowSleep;
    if (ImGui::Checkbox(TR("Allow Sleeping"), &sleep)) {
        ps.allowSleep = sleep;
        discreteEdit(m_ctx, "Physics Sleep", &ps.allowSleep, !sleep);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            TR("Freezes bodies that have been still for Sleep Time.\n"
            "Islands sleep together: a whole stack stops as one, otherwise\n"
            "the bottom box keeps waking the top one and the pile buzzes."));
    }

    ImGui::BeginDisabled(!ps.allowSleep);
    const float slt0 = ps.sleepLinearThreshold;
    ImGui::SetNextItemWidth(kPhysParamW);
    ImGui::SliderFloat(TRL("Linear Thresh"), &ps.sleepLinearThreshold, 0.0f, 0.5f,
                       TR("%.3f m/s"));
    fieldEdit(m_ctx, "Sleep Linear Threshold", &ps.sleepLinearThreshold, slt0);

    const float sat0 = ps.sleepAngularThreshold;
    ImGui::SetNextItemWidth(kPhysParamW);
    ImGui::SliderFloat(TRL("Angular Thresh"), &ps.sleepAngularThreshold, 0.0f, 1.0f,
                       TR("%.3f rad/s"));
    fieldEdit(m_ctx, "Sleep Angular Threshold", &ps.sleepAngularThreshold, sat0);

    const float st0 = ps.sleepTime;
    ImGui::SetNextItemWidth(kPhysParamW);
    ImGui::SliderFloat(TRL("Sleep Time"), &ps.sleepTime, 0.0f, 3.0f, TR("%.2f s"));
    fieldEdit(m_ctx, "Sleep Time", &ps.sleepTime, st0);
    ImGui::EndDisabled();

    if (ImGui::Button(TR("Reset World Settings"))) {
        m_ctx.structuralEdit("Reset Physics Settings", [&]() {
            m_ctx.editorScene().physicsSettings() = scene::PhysicsSettings{};
        });
    }

    ImGui::EndDisabled();

    // ---- 运行态只读统计 ----
    const PhysicsRuntimeInfo& info = m_ctx.physicsRuntime();
    ImGui::SeparatorText(TR("Runtime"));
    logRect("INS-PHYS-RUNTIME", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    if (!info.active) {
        ImGui::TextDisabled(m_ctx.isEditing()
                                ? TRL("not simulating (edit mode - press Play)")
                                : TRL("not simulating (physics disabled)"));
    } else {
        ImGui::Text(TR("step %llu   t %.2f s"), info.steps,
                    static_cast<double>(info.simTime));
        ImGui::Text(TR("bodies %d  (dynamic %d, asleep %d)"), info.bodies,
                    info.dynamic, info.sleeping);
        ImGui::Text(TR("contacts %d in %d manifolds"), info.contactPoints,
                    info.manifolds);
        ImGui::Text(TR("broadphase pairs %d"), info.broadphasePairs);
        ImGui::Text(TR("max penetration %.4f m   max speed %.2f m/s"),
                    static_cast<double>(info.maxPenetration),
                    static_cast<double>(info.maxSpeed));
    }
}

void InspectorPanel::addRigidBodyTo(ecs::Entity e) {
    m_ctx.structuralEdit("Add Rigid Body", [&]() {
        m_ctx.editorScene().world().add<ecs::RigidBodyComponent>(
            e, ecs::RigidBodyComponent{});
    });
    m_ctx.notify(TR("Rigid body added — press Play to simulate"));
    // 自动化锚点：加完之后的回执（点击是否真的落上了）
    VK_LOG_INFO("INS-PHYS-ADD entity=%u", e.id);
}

void InspectorPanel::removeRigidBodyFrom(ecs::Entity e) {
    m_ctx.structuralEdit("Remove Rigid Body", [&]() {
        m_ctx.editorScene().world().remove<ecs::RigidBodyComponent>(e);
    });
}

void InspectorPanel::drawRigidBodySection(ecs::Entity e) {
    if (!e.valid()) {
        // 没选中物体不算"空页签" —— 上面那段世界参数本来就跟选择无关。
        hint("Select an object to give it a rigid body. Physics runs in Play "
             "mode only, so a body added here will not move until you press "
             "Play.");
        return;
    }

    scene::Scene& sc = m_ctx.editorScene();
    ecs::World& w = sc.world();
    auto* rb = w.get<ecs::RigidBodyComponent>(e);

    const bool open = sectionHeader("Rigid Body", true, e);
    logRect("INS-PHYS-BODY-HEADER", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    if (!open) return;

    // ---- 没有刚体：给出添加入口 ----
    if (!rb) {
        ImGui::TextDisabled(TR("This object is not simulated."));
        if (ImGui::Button(TR("Add Rigid Body"))) addRigidBodyTo(e);
        logRect("INS-PHYS-ADD", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());
        ImGui::SameLine();
        ImGui::TextDisabled(TR("(?)"));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                TR("Adds a Dynamic body. Its shape defaults to Auto: the\n"
                "collision component if there is one, otherwise the mesh\n"
                "bounds. Press Play to see it fall."));
        }
        hint("Rigid bodies only simulate in Play mode. Stop restores the scene "
             "exactly as you left it (the Play copy is thrown away).");
        return;
    }

    if (!m_ctx.isEditing()) {
        ImGui::TextDisabled(TR("run mode - body parameters are read-only here"));
    }
    ImGui::BeginDisabled(!m_ctx.isEditing());

    // ---- 运动类型 ----
    static const char* kTypes[] = {"Static", "Kinematic", "Dynamic"};
    int type = static_cast<int>(rb->bodyType);
    ImGui::SetNextItemWidth(kPhysParamW);
    if (ImGui::Combo(TR("Type"), &type, TrItems<3>(kTypes), 3)) {
        const ecs::PhysicsBodyType prev = rb->bodyType;
        rb->bodyType = static_cast<ecs::PhysicsBodyType>(type);
        discreteEdit(m_ctx, "Body Type", &rb->bodyType, prev);
    }
    logRect("INS-PHYS-BODY-TYPE", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            TR("Static    - never moves (walls, floors). Infinite mass.\n"
            "Kinematic - driven by scene logic: whatever moves the object\n"
            "            (script / animation) becomes its velocity, so it\n"
            "            pushes dynamic bodies out of the way.\n"
            "Dynamic   - full rigid-body dynamics."));
    }

    // ---- 形状 ----
    static const char* kShapes[] = {"Auto", "Sphere", "Box", "Capsule",
                                    "Convex Hull"};
    int shape = static_cast<int>(rb->shape);
    ImGui::SetNextItemWidth(kPhysParamW);
    if (ImGui::Combo(TR("Shape"), &shape, TrItems<5>(kShapes), 5)) {
        const ecs::PhysicsShapeType prev = rb->shape;
        rb->shape = static_cast<ecs::PhysicsShapeType>(shape);
        discreteEdit(m_ctx, "Body Shape", &rb->shape, prev);
    }
    logRect("INS-PHYS-BODY-SHAPE", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            TR("Auto        - collision component, else mesh bounds, else a 0.5 m "
            "sphere.\n"
            "Sphere/Box/Capsule - explicit sizes in **world metres** (not "
            "scaled by\n"
            "the object's own scale, so the numbers mean what they say).\n"
            "Convex Hull - reuses the collider's hull points."));
    }

    // ---- 形状尺寸（世界空间的米）----
    if (rb->shape == ecs::PhysicsShapeType::Sphere) {
        const float r0 = rb->radius;
        ImGui::SetNextItemWidth(kPhysParamW);
        ImGui::SliderFloat(TRL("Radius"), &rb->radius, 0.01f, 10.0f, TR("%.3f m"));
        fieldEdit(m_ctx, "Body Radius", &rb->radius, r0);
    } else if (rb->shape == ecs::PhysicsShapeType::Capsule) {
        const float r0 = rb->radius;
        ImGui::SetNextItemWidth(kPhysParamW);
        ImGui::SliderFloat(TRL("Radius"), &rb->radius, 0.01f, 10.0f, TR("%.3f m"));
        fieldEdit(m_ctx, "Body Radius", &rb->radius, r0);
        const float h0 = rb->halfHeight;
        ImGui::SetNextItemWidth(kPhysParamW);
        ImGui::SliderFloat(TRL("Half Height"), &rb->halfHeight, 0.0f, 10.0f,
                           TR("%.3f m"));
        fieldEdit(m_ctx, "Body Half Height", &rb->halfHeight, h0);
        ImGui::SetItemTooltip(TRL("Half height of the cylindrical section (the two "
                              "hemispheres add Radius on top)."));
    } else if (rb->shape == ecs::PhysicsShapeType::Box) {
        const glm::vec3 h0 = rb->halfExtents;
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::DragFloat3("##phys-half", glm::value_ptr(rb->halfExtents),
                          sp(0.01f), 0.001f, 100.0f);
        fieldEdit(m_ctx, "Body Half Extents", &rb->halfExtents, h0);
        ImGui::SetItemTooltip(TRL("Half extents (m)"));
    } else {
        ImGui::TextDisabled(TR("shape is derived at Play time"));
    }

    ImGui::SeparatorText(TR("Mass"));
    const float m0 = rb->mass;
    ImGui::SetNextItemWidth(kPhysParamW);
    ImGui::DragFloat(TRL("Mass"), &rb->mass, sp(1.0f), 0.0f, 100000.0f, TR("%.1f kg"));
    fieldEdit(m_ctx, "Body Mass", &rb->mass, m0);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            TR("0 = derive from Density x volume (recommended: resizing the body\n"
            "then updates the mass automatically). > 0 = this exact mass."));
    }
    const float d0 = rb->density;
    ImGui::SetNextItemWidth(kPhysParamW);
    ImGui::DragFloat(TRL("Density"), &rb->density, sp(10.0f), 0.0f, 100000.0f,
                     TR("%.0f kg/m3"));
    fieldEdit(m_ctx, "Body Density", &rb->density, d0);
    ImGui::SetItemTooltip(TRL("Used only when Mass = 0. 500 kg/m3 = a 1 m cube "
                          "weighs 250 kg."));

    ImGui::SeparatorText(TR("Material"));
    const float sf0 = rb->staticFriction;
    ImGui::SetNextItemWidth(kPhysParamW);
    ImGui::SliderFloat(TRL("Static Friction"), &rb->staticFriction, 0.0f, 1.5f,
                       "%.2f");
    fieldEdit(m_ctx, "Body Static Friction", &rb->staticFriction, sf0);
    const float df0 = rb->dynamicFriction;
    ImGui::SetNextItemWidth(kPhysParamW);
    ImGui::SliderFloat(TRL("Dynamic Friction"), &rb->dynamicFriction, 0.0f, 1.5f,
                       "%.2f");
    fieldEdit(m_ctx, "Body Dynamic Friction", &rb->dynamicFriction, df0);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            TR("Two objects: friction combines as sqrt(a*b), restitution takes\n"
            "the larger one. Keep static >= dynamic - the other way round\n"
            "makes things creep downhill."));
    }
    const float re0 = rb->restitution;
    ImGui::SetNextItemWidth(kPhysParamW);
    ImGui::SliderFloat(TRL("Restitution"), &rb->restitution, 0.0f, 1.0f, "%.2f");
    fieldEdit(m_ctx, "Body Restitution", &rb->restitution, re0);
    ImGui::SetItemTooltip(TRL("0 = dead stop on landing, 1 = perfectly bouncy."));

    ImGui::SeparatorText(TR("Behaviour"));
    const float ld0 = rb->linearDamping;
    ImGui::SetNextItemWidth(kPhysParamW);
    ImGui::SliderFloat(TRL("Linear Damping"), &rb->linearDamping, 0.0f, 2.0f,
                       "%.3f");
    fieldEdit(m_ctx, "Body Linear Damping", &rb->linearDamping, ld0);
    const float ad0 = rb->angularDamping;
    ImGui::SetNextItemWidth(kPhysParamW);
    ImGui::SliderFloat(TRL("Angular Damping"), &rb->angularDamping, 0.0f, 2.0f,
                       "%.3f");
    fieldEdit(m_ctx, "Body Angular Damping", &rb->angularDamping, ad0);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Keep this small but non-zero: with zero damping "
                          "solver error slowly accumulates into drift."));
    }

    bool grav = rb->useGravity;
    if (ImGui::Checkbox(TR("Use Gravity"), &grav)) {
        rb->useGravity = grav;
        discreteEdit(m_ctx, "Body Gravity", &rb->useGravity, !grav);
    }
    bool bsleep = rb->allowSleep;
    if (ImGui::Checkbox(TR("Allow Sleep"), &bsleep)) {
        rb->allowSleep = bsleep;
        discreteEdit(m_ctx, "Body Sleep", &rb->allowSleep, !bsleep);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Effective value is this AND the world switch above."));
    }

    ImGui::SeparatorText(TR("Initial Velocity"));
    const glm::vec3 lv0 = rb->initialVelocity;
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::DragFloat3("##phys-linvel", glm::value_ptr(rb->initialVelocity),
                      sp(0.1f));
    fieldEdit(m_ctx, "Body Initial Velocity", &rb->initialVelocity, lv0);
    ImGui::SetItemTooltip(TRL("Applied once, the moment Play starts."));

    const glm::vec3 av0 = rb->initialAngularVelocity;
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::DragFloat3("##phys-angvel",
                      glm::value_ptr(rb->initialAngularVelocity), sp(0.05f));
    fieldEdit(m_ctx, "Body Initial Spin", &rb->initialAngularVelocity, av0);

    if (ImGui::Button(TR("Zero Velocities"))) {
        m_ctx.structuralEdit("Zero Body Velocities", [&]() {
            auto* r2 = m_ctx.editorScene()
                           .world()
                           .get<ecs::RigidBodyComponent>(e);
            if (!r2) return;
            r2->initialVelocity = glm::vec3(0.0f);
            r2->initialAngularVelocity = glm::vec3(0.0f);
        });
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Remove Rigid Body"))) removeRigidBodyFrom(e);

    ImGui::EndDisabled();

    hint("Shape and mass are read once, when Play starts. Changing them while "
         "running has no effect until you stop and Play again - that is what "
         "makes Stop a perfect undo.");
}

// ---------------------------------------------------------------- M2 玩法组件
//
// 四段共同的形状：**只在组件存在时出现**（加组件统一走 Tool 页签的
// "Add Component"，与贴花 / 探针 / 粒子发射器同一条链路），没有"Add"按钮。
// 理由只有一个：添加入口分散在每段里，迟早会出现两个入口写两套默认值。
//
// ⚠ 这四个组件的参数都是"**Play 开始时读一次**"的初始条件（和刚体一样）。
//   所以运行态一律 BeginDisabled + 写明原因 —— "能改但没反应"是最糟的交互。
void InspectorPanel::drawCharacterSection(ecs::Entity e) {
    if (!e.valid()) return;
    ecs::World& w = m_ctx.editorScene().world();
    auto* cc = w.get<ecs::CharacterControllerComponent>(e);
    if (!cc) return;   // 没有就整段不出现（不是空段）

    const bool open = sectionHeader("Character Controller", true, e);
    logRect("INS-PHYS-CHAR-HEADER", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    if (!open) return;

    // 互斥提示：两个组件同时存在时刚体不参与仿真（见 ScenePhysics 的说明）
    if (w.has<ecs::RigidBodyComponent>(e)) {
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.3f, 1.0f),
                           TR("Also has a Rigid Body - the character wins."));
        hint("A character is kinematic: it is moved by stepCharacter(), not by "
             "the solver. With both components present the body is ignored "
             "while the character is active. Remove one of them.");
    }

    ImGui::BeginDisabled(!m_ctx.isEditing());

    ImGui::SeparatorText(TR("Capsule"));
    const float r0 = cc->radius;
    ImGui::SetNextItemWidth(kPhysParamW);
    ImGui::SliderFloat(TRL("Radius"), &cc->radius, 0.05f, 1.0f, TR("%.3f m"));
    fieldEdit(m_ctx, "Char Radius", &cc->radius, r0);
    const float hh0 = cc->halfHeight;
    ImGui::SetNextItemWidth(kPhysParamW);
    ImGui::SliderFloat(TRL("Half Height"), &cc->halfHeight, 0.0f, 2.0f, TR("%.3f m"));
    fieldEdit(m_ctx, "Char Half Height", &cc->halfHeight, hh0);
    ImGui::SetItemTooltip(TRL("Total height = 2 x (Half Height + Radius). The object "
                          "origin is at the FEET, not the centre."));
    // 锚点记在**提前返回之后**（见项目纪律）：header 只要函数被调用就会
    // 打，只有 body 才能证明这一段真的展开着、控件真的建出来了。
    logRect("INS-PHYS-CHAR-BODY", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());

    ImGui::SeparatorText(TR("Walking"));
    const float st0 = cc->stepHeight;
    ImGui::SetNextItemWidth(kPhysParamW);
    ImGui::SliderFloat(TRL("Step Height"), &cc->stepHeight, 0.0f, 1.0f, TR("%.3f m"));
    fieldEdit(m_ctx, "Char Step Height", &cc->stepHeight, st0);
    ImGui::SetItemTooltip(TRL("Ledges up to this height are walked over instead of "
                          "blocking. Roughly a third of the capsule radius is a "
                          "good default; more looks like teleporting."));
    const float sl0 = cc->slopeLimitDeg;
    ImGui::SetNextItemWidth(kPhysParamW);
    ImGui::SliderFloat(TRL("Slope Limit"), &cc->slopeLimitDeg, 10.0f, 89.0f,
                       TR("%.0f deg"));
    fieldEdit(m_ctx, "Char Slope Limit", &cc->slopeLimitDeg, sl0);
    ImGui::SetItemTooltip(TRL("Steeper than this is not floor: gravity keeps "
                          "pulling and the character slides down."));

    bool snap = cc->groundSnap;
    if (ImGui::Checkbox(TR("Snap To Ground"), &snap)) {
        cc->groundSnap = snap;
        discreteEdit(m_ctx, "Char Ground Snap", &cc->groundSnap, !snap);
    }
    ImGui::SetItemTooltip(TRL("Sticks to the floor when walking down slopes. Off = "
                          "you bounce down every step of a staircase."));
    if (cc->groundSnap) {
        const float gs0 = cc->groundSnapDistance;
        ImGui::SetNextItemWidth(kPhysParamW);
        ImGui::SliderFloat(TRL("Snap Distance"), &cc->groundSnapDistance, 0.02f,
                           1.0f, TR("%.2f m"));
        fieldEdit(m_ctx, "Char Snap Distance", &cc->groundSnapDistance, gs0);
    }

    ImGui::SeparatorText(TR("Motion"));
    const float ms0 = cc->maxSpeed;
    ImGui::SetNextItemWidth(kPhysParamW);
    ImGui::SliderFloat(TRL("Max Speed"), &cc->maxSpeed, 0.5f, 20.0f, TR("%.2f m/s"));
    fieldEdit(m_ctx, "Char Max Speed", &cc->maxSpeed, ms0);
    const float sp0 = cc->sprintMultiplier;
    ImGui::SetNextItemWidth(kPhysParamW);
    ImGui::SliderFloat(TRL("Sprint x"), &cc->sprintMultiplier, 1.0f, 4.0f, "%.2f");
    fieldEdit(m_ctx, "Char Sprint", &cc->sprintMultiplier, sp0);
    const float ac0 = cc->acceleration;
    ImGui::SetNextItemWidth(kPhysParamW);
    ImGui::SliderFloat(TRL("Acceleration"), &cc->acceleration, 1.0f, 200.0f,
                       TR("%.0f m/s2"));
    fieldEdit(m_ctx, "Char Acceleration", &cc->acceleration, ac0);
    ImGui::SetItemTooltip(TRL("How fast the character reaches Max Speed. Very high "
                          "values feel like ice-free tank controls."));
    const float js0 = cc->jumpSpeed;
    ImGui::SetNextItemWidth(kPhysParamW);
    ImGui::SliderFloat(TRL("Jump Speed"), &cc->jumpSpeed, 0.0f, 15.0f, TR("%.2f m/s"));
    fieldEdit(m_ctx, "Char Jump Speed", &cc->jumpSpeed, js0);
    ImGui::SetItemTooltip(TRL("Initial upward speed. Height ~ v^2 / (2 * 9.81)."));
    const float gsc0 = cc->gravityScale;
    ImGui::SetNextItemWidth(kPhysParamW);
    ImGui::SliderFloat(TRL("Gravity Scale"), &cc->gravityScale, 0.0f, 3.0f, "%.2f");
    fieldEdit(m_ctx, "Char Gravity Scale", &cc->gravityScale, gsc0);
    const float air0 = cc->airControl;
    ImGui::SetNextItemWidth(kPhysParamW);
    ImGui::SliderFloat(TRL("Air Control"), &cc->airControl, 0.0f, 1.0f, "%.2f");
    fieldEdit(m_ctx, "Char Air Control", &cc->airControl, air0);
    ImGui::SetItemTooltip(TRL("Fraction of the ground acceleration that still "
                          "applies while airborne."));
    const float ts0 = cc->turnSpeed;
    ImGui::SetNextItemWidth(kPhysParamW);
    ImGui::SliderFloat(TRL("Turn Speed"), &cc->turnSpeed, 1.0f, 40.0f, "%.1f /s");
    fieldEdit(m_ctx, "Char Turn Speed", &cc->turnSpeed, ts0);
    const float ps0 = cc->pushStrength;
    ImGui::SetNextItemWidth(kPhysParamW);
    ImGui::SliderFloat(TRL("Push Strength"), &cc->pushStrength, 0.0f, 5.0f, "%.2f");
    fieldEdit(m_ctx, "Char Push", &cc->pushStrength, ps0);
    ImGui::SetItemTooltip(TRL("How hard the kinematic proxy shoves dynamic bodies "
                          "it walks into."));

    ImGui::Separator();
    if (ImGui::Button(TR("Remove Character Controller"))) {
        m_ctx.structuralEdit("Remove Character Controller", [&]() {
            m_ctx.editorScene()
                .world()
                .remove<ecs::CharacterControllerComponent>(e);
        });
        m_ctx.notify(TR("Character controller removed"));
        VK_LOG_INFO("INS-PHYS-CHAR-REMOVE entity=%u", e.id);
    }
    ImGui::EndDisabled();

    hint("Read once when Play starts - Stop is a perfect undo for all of it.");
}

void InspectorPanel::drawTriggerSection(ecs::Entity e) {
    if (!e.valid()) return;
    ecs::World& w = m_ctx.editorScene().world();
    auto* tr = w.get<ecs::TriggerComponent>(e);
    if (!tr) return;

    const bool open = sectionHeader("Trigger", true, e);
    logRect("INS-PHYS-TRIGGER-HEADER", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    if (!open) return;

    if (!w.has<ecs::CollisionComponent>(e)) {
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.3f, 1.0f),
                           TR("No collider - this trigger has no volume."));
        hint("A trigger has no geometry of its own: the Collision component is "
             "the volume. Close this section, add a collision component in the "
             "viewport (right-click > Add Collision), then come back.");
    } else {
        hint("The Collision component above is the volume. Things pass through "
             "it; every Enter / Stay / Exit is reported to scripts.");
    }

    ImGui::BeginDisabled(!m_ctx.isEditing());
    bool en = tr->enabled;
    if (ImGui::Checkbox(TR("Enabled"), &en)) {
        tr->enabled = en;
        discreteEdit(m_ctx, "Trigger Enabled", &tr->enabled, !en);
    }
    ImGui::SetItemTooltip(TRL("Off = the volume stays but reports nothing."));
    bool stay = tr->reportStay;
    if (ImGui::Checkbox(TR("Report Stay"), &stay)) {
        tr->reportStay = stay;
        discreteEdit(m_ctx, "Trigger Stay", &tr->reportStay, !stay);
    }
    ImGui::SetItemTooltip(TRL("Stay fires every frame for every overlapping body - "
                          "that is the main cost of a trigger. Turn it off when "
                          "you only care about entering."));
    int mask = static_cast<int>(tr->mask);
    ImGui::SetNextItemWidth(kPhysParamW);
    if (ImGui::InputInt(TR("Layer Mask"), &mask, 0, 0,
                        ImGuiInputTextFlags_CharsHexadecimal)) {
        const unsigned prev = tr->mask;
        tr->mask = static_cast<unsigned>(mask);
        discreteEdit(m_ctx, "Trigger Mask", &tr->mask, prev);
    }
    ImGui::SetItemTooltip(TRL("Bit mask of the layers this volume reports. "
                          "0xFFFFFFFF = report everything."));
    logRect("INS-PHYS-TRIGGER-BODY", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());

    ImGui::Separator();
    if (ImGui::Button(TR("Remove Trigger"))) {
        m_ctx.structuralEdit("Remove Trigger", [&]() {
            m_ctx.editorScene().world().remove<ecs::TriggerComponent>(e);
        });
        VK_LOG_INFO("INS-PHYS-TRIGGER-REMOVE entity=%u", e.id);
    }
    ImGui::EndDisabled();
}

void InspectorPanel::drawPhysicsMaterialSection(ecs::Entity e) {
    if (!e.valid()) return;
    ecs::World& w = m_ctx.editorScene().world();
    auto* pm = w.get<ecs::PhysicsMaterialComponent>(e);
    if (!pm) return;   // 没有覆盖层 = 用刚体自己的那四个字段（默认行为）

    const bool open = sectionHeader("Physics Material", true, e);
    logRect("INS-PHYS-MAT-HEADER", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    if (!open) return;

    if (!w.has<ecs::RigidBodyComponent>(e)) {
        hint("Overriding friction / restitution only matters for a body that is "
             "simulated. This object has no Rigid Body, so these values are "
             "stored but unused.");
    } else {
        hint("These four values REPLACE the matching fields on the Rigid Body. "
             "Remove this component to go back to them.");
    }

    ImGui::BeginDisabled(!m_ctx.isEditing());
    const float sf0 = pm->staticFriction;
    ImGui::SetNextItemWidth(kPhysParamW);
    ImGui::SliderFloat(TRL("Static Friction"), &pm->staticFriction, 0.0f, 1.5f,
                       "%.2f");
    fieldEdit(m_ctx, "Mat Static Friction", &pm->staticFriction, sf0);
    const float df0 = pm->dynamicFriction;
    ImGui::SetNextItemWidth(kPhysParamW);
    ImGui::SliderFloat(TRL("Dynamic Friction"), &pm->dynamicFriction, 0.0f, 1.5f,
                       "%.2f");
    fieldEdit(m_ctx, "Mat Dynamic Friction", &pm->dynamicFriction, df0);
    const float re0 = pm->restitution;
    ImGui::SetNextItemWidth(kPhysParamW);
    ImGui::SliderFloat(TRL("Restitution"), &pm->restitution, 0.0f, 1.0f, "%.2f");
    fieldEdit(m_ctx, "Mat Restitution", &pm->restitution, re0);
    ImGui::SetItemTooltip(TRL("Two bodies touching: restitution takes the LARGER "
                          "of the two, so one bouncy surface is enough."));
    const float de0 = pm->density;
    ImGui::SetNextItemWidth(kPhysParamW);
    ImGui::SliderFloat(TRL("Density"), &pm->density, 1.0f, 5000.0f, TR("%.0f kg/m3"));
    fieldEdit(m_ctx, "Mat Density", &pm->density, de0);
    logRect("INS-PHYS-MAT-BODY", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());

    ImGui::Separator();
    if (ImGui::Button(TR("Remove Override"))) {
        m_ctx.structuralEdit("Remove Physics Material", [&]() {
            m_ctx.editorScene()
                .world()
                .remove<ecs::PhysicsMaterialComponent>(e);
        });
        VK_LOG_INFO("INS-PHYS-MAT-REMOVE entity=%u", e.id);
    }
    ImGui::EndDisabled();
}

// 相机组件（G09）。与上面三段的差别：**加相机是"选镜头"，不是"改物体"**，
// 所以除了本对象的参数，这里还列出场景里所有相机与它们的优先级 ——
// "为什么画面是这个角度"必须能在同一屏里回答（输的那个为什么会输）。
void InspectorPanel::drawCameraComponentSection(ecs::Entity e) {
    if (!e.valid()) return;
    ecs::World& w = m_ctx.editorScene().world();
    auto* cam = w.get<ecs::CameraComponent>(e);
    if (!cam) return;

    const bool open = sectionHeader("Camera Component", true, e);
    logRect("INS-CAM-COMP-HEADER", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    if (!open) return;

    ImGui::BeginDisabled(!m_ctx.isEditing());
    const float f0 = cam->fov;
    ImGui::SetNextItemWidth(kPhysParamW);
    ImGui::SliderFloat(TRL("FOV"), &cam->fov, 20.0f, 120.0f, TR("%.1f deg"));
    fieldEdit(m_ctx, "Cam FOV", &cam->fov, f0);
    int prio = cam->priority;
    ImGui::SetNextItemWidth(kPhysParamW);
    if (ImGui::InputInt(TR("Priority"), &prio, 1, 10)) {
        const int prev = cam->priority;
        cam->priority = prio;
        discreteEdit(m_ctx, "Cam Priority", &cam->priority, prev);
    }
    ImGui::SetItemTooltip(TRL("Highest priority among ENABLED cameras wins. Ties go "
                          "to the lower entity id, so the winner never changes "
                          "just because the tree was walked in another order."));
    bool en = cam->enabled;
    if (ImGui::Checkbox(TR("Enabled"), &en)) {
        cam->enabled = en;
        discreteEdit(m_ctx, "Cam Enabled", &cam->enabled, !en);
    }

    ImGui::SeparatorText(TR("Follow"));
    hint("Only used when a player controller drives this camera (put it on the "
         "character). A camera on its own object uses its transform instead.");
    const glm::vec3 to0 = cam->targetOffset;
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::DragFloat3("##cam-target-offset", glm::value_ptr(cam->targetOffset),
                      sp(0.02f));
    fieldEdit(m_ctx, "Cam Target Offset", &cam->targetOffset, to0);
    ImGui::SetItemTooltip(TRL("Look-at point relative to the capsule CENTRE (the "
                          "object origin is at the feet)."));
    const float d0 = cam->distance;
    ImGui::SetNextItemWidth(kPhysParamW);
    ImGui::SliderFloat(TRL("Arm Length"), &cam->distance, 0.2f, 20.0f, TR("%.2f m"));
    fieldEdit(m_ctx, "Cam Distance", &cam->distance, d0);
    const float md0 = cam->minDistance;
    ImGui::SetNextItemWidth(kPhysParamW);
    ImGui::SliderFloat(TRL("Min Arm"), &cam->minDistance, 0.1f, 5.0f, TR("%.2f m"));
    fieldEdit(m_ctx, "Cam Min Distance", &cam->minDistance, md0);
    bool col = cam->collideWithWorld;
    if (ImGui::Checkbox(TR("Collide With World"), &col)) {
        cam->collideWithWorld = col;
        discreteEdit(m_ctx, "Cam Collide", &cam->collideWithWorld, !col);
    }
    ImGui::SetItemTooltip(TRL("Shortens the arm when a wall is in the way. Off = the "
                          "camera clips through geometry (cutscene shots)."));
    logRect("INS-CAM-COMP-BODY", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());

    ImGui::SeparatorText(TR("Shake"));
    hint("Amplitude 0 (the default) means no shake at all - with it off the "
         "camera state is bit-for-bit what it was before this feature existed.");
    const float sa0 = cam->shakeAmplitude;
    ImGui::SetNextItemWidth(kPhysParamW);
    ImGui::SliderFloat(TRL("Amplitude"), &cam->shakeAmplitude, 0.0f, 0.5f, TR("%.3f m"));
    fieldEdit(m_ctx, "Cam Shake Amp", &cam->shakeAmplitude, sa0);
    const float sf0 = cam->shakeFrequency;
    ImGui::SetNextItemWidth(kPhysParamW);
    ImGui::SliderFloat(TRL("Frequency"), &cam->shakeFrequency, 1.0f, 60.0f,
                       TR("%.1f Hz"));
    fieldEdit(m_ctx, "Cam Shake Freq", &cam->shakeFrequency, sf0);

    ImGui::Separator();
    if (ImGui::Button(TR("Remove Camera"))) {
        m_ctx.structuralEdit("Remove Camera", [&]() {
            m_ctx.editorScene().world().remove<ecs::CameraComponent>(e);
        });
        VK_LOG_INFO("INS-CAM-REMOVE entity=%u", e.id);
    }
    ImGui::EndDisabled();

    // ---- 场景里所有相机：谁在赢、谁在输 ----
    ImGui::SeparatorText(TR("Cameras In Scene"));
    std::vector<std::pair<int, ecs::Entity>> cams;
    w.each<ecs::CameraComponent>(
        [&](ecs::Entity other, const ecs::CameraComponent& oc) {
            cams.push_back({oc.priority, other});
        });
    // 排序：优先级降序、id 升序 —— 与 CameraSystem 的选法**同一套判据**
    std::sort(cams.begin(), cams.end(),
              [](const auto& a, const auto& b) {
                  if (a.first != b.first) return a.first > b.first;
                  return a.second.id < b.second.id;
              });
    int rank = 0;
    for (const auto& pr : cams) {
        const auto* oc = w.get<ecs::CameraComponent>(pr.second);
        const std::string* nm = w.name(pr.second);
        char label[128];
        std::snprintf(label, sizeof(label), TR("%s  (prio %d)%s"), 
                      nm ? nm->c_str() : "?", pr.first,
                      oc && !oc->enabled ? TR("  [off]") : "");
        if (rank == 0) {
            ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "%s",
                               label);
            ImGui::SameLine();
            ImGui::TextDisabled(TR("<- renders"));
        } else {
            ImGui::TextDisabled("%s", label);
        }
        if (pr.second != e && ImGui::IsItemClicked())
            m_ctx.select(pr.second);
        ++rank;
    }
    logRect("INS-CAM-LIST", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
}

void InspectorPanel::drawTabMaterial(ecs::Entity e) {
    if (!e.valid()) { tabEmptyHint("an object"); return; }
    drawMaterialSection(e);
}

void InspectorPanel::drawTabAnimation(ecs::Entity e) {
    if (!e.valid()) { tabEmptyHint("an object"); return; }

    scene::Scene& sc = m_ctx.activeScene();
    ecs::World& w = sc.world();
    auto* sk = w.get<ecs::SkinnedMeshComponent>(e);
    if (!sk) {
        tabEmptyHint("skinned mesh");
        hint("Only models that ship with a skeleton (glTF/FBX with skin data) "
             "get a Skinned Mesh component. Static meshes have nothing to "
             "play.");
        return;
    }

    const bool animOpen = sectionHeader("Skinned Mesh", true, e);
    logRect("INS-ANIM-HEADER", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    if (!animOpen) return;

    // 只读：这份组件挂在模型**根节点**上，网格实体是它的子级
    // （见 CharacterWindow 装配角色时的做法）。
    if (sk->model && sk->model->skeleton) {
        ImGui::TextDisabled(TR("skeleton: %s  (%d joints)"),
                            sk->model->name.c_str(),
                            sk->model->skeleton->jointCount());
    } else if (sk->model) {
        ImGui::TextDisabled(TR("skeleton: %s  (static!)"), sk->model->name.c_str());
    } else {
        ImGui::TextDisabled(TR("skeleton: (no model bound)"));
    }
    ImGui::Text(TR("joint base index: %d"), sk->jointBase);

    // ---- 权威字段（A1）----
    // 有 AnimatorComponent → 绑它的那四项（它是权威，AnimatorSystem 每帧
    // 把它们镜像回 sk）；没有 → 绑 sk 的那四项（A0 老路径，行为逐位不变）。
    //
    // ⚠ 不能"两个都显示/两个都可写"：AnimatorSystem 每帧把 anim 的值写回
    //   sk，于是写 sk 的那一半下一帧就被覆盖 —— 症状是"拖了时间轴又弹回去"，
    //   而日志里 time 确实在变（变的是 anim 那一份）。
    ecs::AnimatorComponent* an = w.get<ecs::AnimatorComponent>(e);
    float& time = an ? an->time : sk->time;
    float& speed = an ? an->speed : sk->speed;
    bool& playing = an ? an->playing : sk->playing;
    bool& loop = an ? an->loop : sk->loop;

    // 当前 clip 与时长：换 clip 时这里立刻反映，不用再去猜时间轴右端在哪。
    const assets::AnimationClip* clip = nullptr;
    if (sk->model && sk->model->skeleton && !sk->clipName.empty())
        clip = sk->model->skeleton->findClip(sk->clipName);
    const float dur = clip ? clip->duration : 0.0f;
    if (clip) {
        ImGui::Text(TR("clip: %s  (%.3f s)"), clip->name.c_str(),
                    static_cast<double>(dur));
    } else if (sk->clipName.empty()) {
        ImGui::TextDisabled(TR("clip: (rest pose - nothing playing)"));
    } else {
        ImGui::TextDisabled(TR("clip: '%s' NOT FOUND in this skeleton"),
                            sk->clipName.c_str());
    }

    // ---- 时间轴（角色链 A0-3 的可见面；A1 起绑权威字段）----
    // 拖它直接改权威 time，下一帧 AnimatorSystem 按新的 time 重算关节矩阵
    // → 视口姿势立刻跟着动。这是"动画能可视化调"的最小入口；
    // A7 的 Animator 面板会把它扩成带事件标记的完整时间轴。
    const float t0 = time;
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderFloat("##animtime", &time, 0.0f,
                       dur > 0.0f ? dur : 0.001f, TR("%.3f s"));
    logRect("INS-ANIM-TIME", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::IsItemActive()) m_ctx.dirty() = true;
    fieldEdit(m_ctx, "Set Animation Time", &time, t0);

    // time / duration 文本（A1 的验收原文要这一条）：滑条上看不出"现在
    // 是第几秒 / 一共多长"，而截图断言与人工复核都需要一个能读的数字。
    ImGui::TextDisabled(TR("time: %.3f / %.3f s"), static_cast<double>(time),
                        static_cast<double>(dur));
    logRect("INS-ANIM-TIMETEXT", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());

    ImGui::SeparatorText(TR("Playback"));

    // 片段名：和 Script 的路径一样用文本输入 —— 引擎还没有片段浏览器，
    // 做成下拉也没有数据可填。
    const bool sameEntity = (m_skinnedBufFor == e);
    if (!sameEntity) {
        std::snprintf(m_skinnedBuf, sizeof(m_skinnedBuf), "%s",
                      sk->clipName.c_str());
        m_skinnedBufFor = e;
    }
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##skinclip", TRL("clip name (empty = rest pose)"),
                             m_skinnedBuf, sizeof(m_skinnedBuf));
    if (ImGui::IsItemDeactivatedAfterEdit() && m_ctx.isEditing()) {
        const std::string newClip = m_skinnedBuf;
        if (newClip != sk->clipName) {
            // clipName 是 std::string，不能走 RawBytesEditCommand（按字节
            // memcpy 一个 std::string 是未定义行为）→ 用闭包命令。
            m_ctx.structuralEdit("Set Animation Clip",
                                 [&]() { sk->clipName = newClip; });
            // 换了 clip 就等于"用户自己指定了"，把一次性补 default 的闸门
            // 关上 —— 否则把它清空之后系统又会按 defaultClip 填回来。
            if (an) an->appliedDefaultClip = true;
        }
    }

    const float s0 = speed;
    ImGui::SliderFloat(TRL("Speed"), &speed, 0.0f, 4.0f, "%.2fx");
    fieldEdit(m_ctx, "Animation Speed", &speed, s0);

    const bool playing0 = playing;
    if (ImGui::Checkbox(TR("Playing"), &playing)) {
        discreteEdit(m_ctx, "Toggle Animation Playing", &playing, playing0);
    }

    // loop：播到末尾绕回开头（false = 停在末帧）。A0-3 新增字段，
    // 默认 true —— "一段 Walk 播完应该接着走，而不是定格"。
    const bool loop0 = loop;
    if (ImGui::Checkbox(TR("Loop"), &loop)) {
        discreteEdit(m_ctx, "Toggle Animation Loop", &loop, loop0);
    }

    // ================================================================
    // Animator 段（A1）
    // ================================================================
    ImGui::Separator();
    ImGui::TextUnformatted(TR("Animator"));
    logRect("INS-ANIM-ATORHEADER", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());

    if (!an) {
        hint("No animator: this entity plays a single clip straight from the "
             "Skinned Mesh fields above. Add an Animator to get a controller "
             "(state machine / blending lands in A2).");
        if (ImGui::Button(TR("Add Animator"))) {
            m_ctx.structuralEdit("Add Animator", [&]() {
                ecs::AnimatorComponent a;
                // defaultClip 从"当前正在播的那一段"接手：加完动画器不该
                // 让画面变样（否则用户会以为按钮把动画弄坏了）。
                a.defaultClip = sk->clipName;
                a.time = sk->time;
                a.speed = sk->speed;
                a.playing = sk->playing;
                a.loop = sk->loop;
                w.add<ecs::AnimatorComponent>(e, a);
            });
        }
        logRect("INS-ANIM-ADDANIM", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());
        return;
    }

    ImGui::TextDisabled(TR("state: %d   time in state: %.2f s"), an->stateIndex,
                        static_cast<double>(an->stateTime));

    // 控制器路径（A2 才有人读它；现在空着就是单 clip 直放模式）
    const bool sameCtrl = (m_animCtrlBufFor == e);
    if (!sameCtrl) {
        std::snprintf(m_animCtrlBuf, sizeof(m_animCtrlBuf), "%s",
                      an->controllerPath.c_str());
        m_animCtrlBufFor = e;
    }
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##animctrl", TRL("controller path (empty = single clip)"),
                             m_animCtrlBuf, sizeof(m_animCtrlBuf));
    if (ImGui::IsItemDeactivatedAfterEdit() && m_ctx.isEditing()) {
        const std::string nc = m_animCtrlBuf;
        if (nc != an->controllerPath) {
            m_ctx.structuralEdit("Set Animator Controller",
                                 [&]() { an->controllerPath = nc; });
        }
    }

    // defaultClip：只影响"clipName 还空着"的那一次（见 appliedDefaultClip）
    const bool sameDef = (m_animDefBufFor == e);
    if (!sameDef) {
        std::snprintf(m_animDefBuf, sizeof(m_animDefBuf), "%s",
                      an->defaultClip.c_str());
        m_animDefBufFor = e;
    }
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##animdef", TRL("default clip (used once)"),
                             m_animDefBuf, sizeof(m_animDefBuf));
    if (ImGui::IsItemDeactivatedAfterEdit() && m_ctx.isEditing()) {
        const std::string nd = m_animDefBuf;
        if (nd != an->defaultClip) {
            m_ctx.structuralEdit("Set Default Clip",
                                 [&]() { an->defaultClip = nd; });
        }
    }

    // 根运动（A4 才会真的驱动位移；这里先把契约摆出来）
    static const char* const kRootMotion[3] = {
        "Off", "From clip", "Delta only"};
    int rm = static_cast<int>(an->rootMotion);
    if (ImGui::Combo(TRL("Root motion"), &rm, TrItems<3>(kRootMotion), 3)) {
        const auto prev = an->rootMotion;
        an->rootMotion = static_cast<ecs::AnimatorComponent::RootMotionMode>(rm);
        discreteEdit(m_ctx, "Set Root Motion Mode", &an->rootMotion, prev);
    }
    logRect("INS-ANIM-ROOTMOTION", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());

    hint("root motion is a placeholder until A4; the controller / state "
         "machine lands in A2 (docs/character_pipeline_roadmap.md).");
}

// 输出页：引擎目前真的没有"输出设置"（分辨率 / 格式 / 保存路径都还没排进
// 路线图），所以这一页**不假装有控件**，老老实实给现状 + 指路。
// 留白比编一个假的下拉框好，编出来只会让人以为"点了没反应 = 坏了"。
void InspectorPanel::drawTabOutput() {
    const bool outOpen = sectionHeader("Output", true);
    logRect("INS-OUTPUT-HEADER", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    // 这一页没有任何控件 —— 说明只能挂在标题上（悬浮标题看）。
    hint("This build has no render-output settings yet.\n\n"
         "Where the knobs live:\n"
         "  - Engine panel: post-processing, bloom, tonemapping\n"
         "  - Engine panel: View mode (Lit / Unlit / Normal)\n"
         "  - Viewport panel: display options and overlays\n"
         "  - GPU timeline panel: per-pass timing\n\n"
         "Not implemented yet:\n"
         "  - resolution / aspect override\n"
         "  - image format and save path\n"
         "  - offline / batch frame export");
    if (!outOpen) return;

    const Rect& vp = m_ctx.layout().viewport;
    ImGui::Text(TR("viewport: %.0f x %.0f px"), vp.w, vp.h);
}

// 场景统计：与"选中谁"无关，所以没选中物体时照常可用。
void InspectorPanel::drawTabScene() { drawSceneStatsSection(); }

// 世界属性：相机 + 主光。同样与选中谁无关（它们本来就不进撤销栈）。
void InspectorPanel::drawTabWorld(ecs::Entity e) {
    drawCameraSection();
    // 相机**组件**（G09）与场景相机（上面那段 target/yaw/pitch）是两个层次：
    // 前者是"谁来当镜头、怎么摆"，后者是编辑器自己那台观察相机。
    drawCameraComponentSection(e);
    drawSunSection();
}

// 全部：旧的"一长条堆叠"视图，一字不改地沿用原来的调用顺序 ——
// 它同时是自动化运行下的默认页签，所以**改了顺序就等于改了历史基线**。
void InspectorPanel::drawTabAll(ecs::Entity e) {
    if (!e.valid()) {
        drawSceneryFallback();
        return;
    }
    drawTransform(e);
    drawVisibility(e);
    drawMeshAndMaterial(e);
    drawLights(e);
    drawScript(e);
    drawAddRemove(e);
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
    // ⚠ 标题同时是 ImGui 的窗口内 ID：直接换成译文会让折叠状态在切语言时
    //   全部失忆（m_sectionOpen 是按 k 记的，但 ImGui 自己的存储按 label 哈希）。
    //   trId 返回 "译文###稳定 key"，显示中文、ID 永远是英文。
    const bool open = ImGui::CollapsingHeader(trId(name, k));
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
    if (ImGui::MenuItem(TR("Copy Properties"), nullptr, false, k != PK_None))
        copySection(name, e);

    // Paste：剪贴板分区名一致 + 组件类型一致 + 字节数一致
    const bool canPaste = k != PK_None && m_clipSection == name &&
                          m_clipKind == static_cast<int>(k) &&
                          m_clipBytes.size() == podSize(k) &&
                          m_ctx.isEditing();
    if (ImGui::MenuItem(TR("Paste Properties"), nullptr, false, canPaste))
        pasteSection(name, e);

    ImGui::Separator();
    if (ImGui::MenuItem(TR("Reset to Default"), nullptr, false, k != PK_None &&
                                                        m_ctx.isEditing())) {
        resetSection(name, e);
    }

    if (k == PK_None) {
        ImGui::Separator();
        ImGui::TextDisabled(TR("no copy/paste for this section"));
    }
    ImGui::EndPopup();
}

// 搜索无命中时的提示
void InspectorPanel::drawNoMatchHint() {
    if (m_search[0] != '\0' && m_sectionsDrawn == 0) {
        ImGui::TextDisabled(TR("no component matches \"%s\""), m_search);
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
    ImGui::TextDisabled(TR("Nothing selected."));
    hint("Click an object in the viewport (or in the Hierarchy) to edit it.");
    ImGui::Separator();
    drawCameraSection();
    drawSunSection();
    drawSceneStatsSection();
}

// 相机与主光不进撤销栈：视口导航本来就是"看一眼就过去了"的操作，
// 把它们塞进历史只会把有用的历史挤掉。
void InspectorPanel::drawCameraSection() {
    scene::Scene& sc = m_ctx.editorScene();
    const bool camOpen = sectionHeader("Camera", true);
    logRect("INS-CAMERA-HEADER", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    if (camOpen) {
        scene::Camera& cam = sc.camera();

        // ---- 只读读数：位置 / 注视点 ----
        // 这两行原来在视口左下角的相机浮层里。浮层已并入本节（见
        // EditorContext::ViewportOverlay 的说明），所以搬到这里 ——
        // 摆机位时"现在到底在哪 / 看向哪"是最常瞄的两行。
        {
            const glm::vec3 p = cam.position();
            const glm::vec3 tg = cam.target();
            ImGui::TextDisabled(TR("pos    %.2f %.2f %.2f"),
                                static_cast<double>(p.x),
                                static_cast<double>(p.y),
                                static_cast<double>(p.z));
            ImGui::TextDisabled(TR("look   %.2f %.2f %.2f"),
                                static_cast<double>(tg.x),
                                static_cast<double>(tg.y),
                                static_cast<double>(tg.z));
        }

        glm::vec3 t = cam.target();
        if (ImGui::DragFloat3(TRL("Target"), glm::value_ptr(t), sp(0.05f))) {
            cam.setTarget(t);
        }
        float dist = cam.distance();
        if (ImGui::DragFloat(TRL("Distance"), &dist, sp(0.1f), 0.5f, 200.0f)) {
            cam.setDistance(dist);
        }
        float fov = cam.fov();
        if (ImGui::SliderFloat(TRL("FOV"), &fov, 15.0f, 110.0f)) cam.setFov(fov);

        // yaw / pitch 是角度，给个直观的滑块
        float yawDeg = glm::degrees(cam.yaw());
        if (ImGui::SliderFloat(TRL("Yaw"), &yawDeg, -180.0f, 180.0f, TR("%.1f deg"))) {
            cam.setYaw(glm::radians(yawDeg));
        }
        float pitchDeg = glm::degrees(cam.pitch());
        if (ImGui::SliderFloat(TRL("Pitch"), &pitchDeg, -85.0f, 85.0f, TR("%.1f deg"))) {
            cam.setPitch(glm::radians(pitchDeg));
        }

        // ---- 导航灵敏度（原相机浮层的内容）----
        float sens = m_ctx.navSensitivity();
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::InputFloat(TRL("Sensitivity"), &sens, 0.0f, 0.0f, "%.2f")) {
            if (sens > 0.0f) {
                m_ctx.navSensitivityRef() =
                    glm::clamp(sens, EditorContext::kMinSensitivity,
                               EditorContext::kMaxSensitivity);
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                TR("Must be > 0 (%.2f .. %.2f).\n"
                "In the viewport: hold RMB + mouse wheel to nudge it."),
                static_cast<double>(EditorContext::kMinSensitivity),
                static_cast<double>(EditorContext::kMaxSensitivity));
        }

        // ---- 相机书签（原相机浮层的内容）----
        // 左键 = 跳过去；右键 = 存当前视角。空槽暗色、已存的蓝色。
        // 键盘等价物：Ctrl+Shift+数字存 / Alt+Shift+数字跳
        // （ViewportPanel::handleOverlayShortcuts）。
        // ⚠ 日志 tag 保持 `VP-BOOK`：那是这套书签唯一的自动化锚点，改名会
        //   一次性打断 verify_viewport 的两条断言。
        ImGui::Spacing();
        ImGui::TextDisabled(TR("Bookmarks  (LMB recall / RMB save)"));
        logRect("INS-CAM-BOOKMARKS", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());
        EditorContext::CameraBookmark* bm = m_ctx.bookmarksRef();
        const ImVec4 used(0.26f, 0.52f, 0.85f, 1.0f);
        const ImVec4 empty(0.20f, 0.20f, 0.23f, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 3.0f));
        for (int i = 0; i < EditorContext::kBookmarkCount; ++i) {
            if (i) ImGui::SameLine(0.0f, 3.0f);
            ImGui::PushID(1000 + i);
            ImGui::PushStyleColor(ImGuiCol_Button, bm[i].set ? used : empty);
            char lb[8];
            std::snprintf(lb, sizeof(lb), "%d", i + 1);
            if (ImGui::SmallButton(lb)) {
                if (bm[i].set) {
                    const glm::vec3 keep = cam.target();
                    cam.setYaw(bm[i].yaw);
                    cam.setPitch(bm[i].pitch);
                    cam.setTarget(keep);
                    m_ctx.setStatus(TR("Camera bookmark ") +
                                    std::to_string(i + 1));
                    VK_LOG_INFO("VP-BOOK recall slot=%d", i + 1);
                } else {
                    VK_LOG_INFO("VP-BOOK recall slot=%d (empty)", i + 1);
                }
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                bm[i].set = true;
                bm[i].pos = cam.position();
                bm[i].yaw = cam.yaw();
                bm[i].pitch = cam.pitch();
                m_ctx.setStatus(TR("Saved camera bookmark ") +
                                std::to_string(i + 1));
                VK_LOG_INFO("VP-BOOK save slot=%d pos=%.3f,%.3f,%.3f yaw=%.4f "
                            "pitch=%.4f",
                            i + 1, static_cast<double>(bm[i].pos.x),
                            static_cast<double>(bm[i].pos.y),
                            static_cast<double>(bm[i].pos.z),
                            static_cast<double>(bm[i].yaw),
                            static_cast<double>(bm[i].pitch));
            }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    TR("%s\nLMB = recall, RMB = save\nCtrl+Shift+%d / "
                    "Alt+Shift+%d"),
                    bm[i].set ? "occupied" : "empty", i + 1, i + 1);
            }
            ImGui::PopID();
        }
        ImGui::PopStyleVar();
        hint("The viewport's old bottom-left camera panel is gone on purpose - "
             "this section is the single place to edit the camera.");
    }
}

// 主光（场景的方向光）。它也不进撤销栈 —— 它是场景自带的"环境"，不是
// 某个被编辑的对象；想给对象加灯请用 Tool 页签的 Add Component。
void InspectorPanel::drawSunSection() {
    scene::Scene& sc = m_ctx.editorScene();
    const bool sunOpen = sectionHeader("Directional Light", true);
    logRect("INS-SUN-HEADER", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    if (sunOpen) {
        scene::Light& L = sc.light();
        const glm::vec3 db = L.direction;
        ImGui::DragFloat3(TRL("Direction"), glm::value_ptr(L.direction), sp(0.01f), -1.0f,
                          1.0f);
        discreteEdit(m_ctx, "Sun Direction", &L.direction, db);

        const glm::vec3 cb = L.color;
        ImGui::ColorEdit3(TRL("Color"), glm::value_ptr(L.color));
        discreteEdit(m_ctx, "Sun Color", &L.color, cb);

        const float ib = L.intensity;
        ImGui::SliderFloat(TRL("Intensity"), &L.intensity, 0.0f, 4.0f);
        discreteEdit(m_ctx, "Sun Intensity", &L.intensity, ib);

        const float ab = L.ambientScale;
        ImGui::SliderFloat(TRL("Ambient"), &L.ambientScale, 0.0f, 1.0f, "%.2f");
        discreteEdit(m_ctx, "Ambient Scale", &L.ambientScale, ab);
        hint("Low ambient = indoor mood; 0.55 is a good outdoor default");
    }
}

// 场景统计：纯只读。放在这里而不是"没选中时才显示"——它与选中谁无关。
void InspectorPanel::drawSceneStatsSection() {
    scene::Scene& sc = m_ctx.editorScene();
    const bool statsOpen = sectionHeader("Scene");
    logRect("INS-SCENE-STATS", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    if (statsOpen) {
        ImGui::Text(TR("Entities: %zu"), sc.objectCount());
        const scene::Scene::LightStats ls = sc.lightStats();
        ImGui::Text(TR("Local lights: %zu (%zu point / %zu spot)"),
                    ls.pointLights + ls.spotLights, ls.pointLights,
                    ls.spotLights);
        ImGui::Text(TR("Editor commands: %zu (cursor %zu)"),
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
    if (ImGui::Checkbox(TR("Locked"), &lockFlag) && m_ctx.isEditing()) {
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
        ImGui::SetTooltip(TR("Locked = cannot be picked in the viewport and no transform gizmo (the built-in ground is locked by default).\n"
                          "Uncheck this to adjust the ground."));
    }
    ImGui::SameLine();
    ImGui::TextDisabled(locked ? TRL("(fixed, cannot be picked or transformed)")
                               : "(unlocked)");
    ImGui::EndDisabled();

    ImGui::BeginDisabled(locked);

    // 位置
    const glm::vec3 pb = t->position;
    ImGui::SetNextItemWidth(-90.0f);
    ImGui::DragFloat3(TRL("Position"), glm::value_ptr(t->position), sp(0.02f));
    fieldEdit(m_ctx, ("Move " + who).c_str(), &t->position, pb);
    ImGui::SameLine();
    if (ImGui::SmallButton(TRL("R##pos")) && m_ctx.isEditing()) {
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
    if (ImGui::DragFloat3(TRL("Rotation"), glm::value_ptr(rotDeg), sp(0.5f))) {
        t->rotation = glm::radians(rotDeg);
    }
    fieldEdit(m_ctx, ("Rotate " + who).c_str(), &t->rotation, rb);
    ImGui::SameLine();
    if (ImGui::SmallButton(TRL("R##rot")) && m_ctx.isEditing()) {
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
    ImGui::DragFloat3(TRL("Scale"), glm::value_ptr(t->scale), sp(0.01f), 0.001f, 100.0f);
    fieldEdit(m_ctx, ("Scale " + who).c_str(), &t->scale, sb);
    ImGui::SameLine();
    if (ImGui::SmallButton(TRL("R##scl")) && m_ctx.isEditing()) {
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
    ImGui::TextDisabled(TR("world position: (%.3f, %.3f, %.3f)"), wp.x, wp.y, wp.z);

    ImGui::EndDisabled();
    if (locked) {
        ImGui::SameLine();
        ImGui::TextDisabled(TR("(locked)"));
    }
}

void InspectorPanel::drawVisibility(ecs::Entity e) {
    scene::Scene& sc = m_ctx.activeScene();
    auto* v = sc.world().get<ecs::VisibilityComponent>(e);
    if (!v) return;

    if (!sectionHeader("Visibility", true, e))
        return;

    bool visible = v->visible;
    if (ImGui::Checkbox(TR("Visible"), &visible)) {
        v->visible = visible;
        discreteEdit(m_ctx, "Toggle Visible", &v->visible, !visible);
    }
    ImGui::SameLine();
    bool shadow = v->castShadow;
    if (ImGui::Checkbox(TR("Cast Shadow"), &shadow)) {
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
    // ⚠ 追加在**最后**：`All` 页签的顺序是"旧堆叠顺序一字不改"，
    //   9 个脚本的 INS-* 锚点都依赖它（MEMORY：别"优化"它）。
    drawTerrainSection(e);
    // 2026-10-24：同样追加在末尾 —— GameStart 的段落是新增的，排在任何
    // 老段之前都会把老锚点的纵向位置整体推移。
    drawGameStartSection(e);
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
        ImGui::TextDisabled(TR("run mode - colliders are read-only here"));
        return;
    }

    // ---- 形状切换：换完按网格重建几何（位置/旋转/缩放保留）----
    int shape = static_cast<int>(cc->shape);
    const char* kShapes[] = {"Convex Hull", "Capsule"};
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::Combo(TR("Shape"), &shape, TrItems<2>(kShapes), 2)) {
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
    if (ImGui::Checkbox(TR("Edit with W/E/R"), &editColl)) {
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
    if (ImGui::Checkbox(TR("Solid"), &solid)) {
        cc->solid = solid;   // discreteEdit 的契约：*target 已持有新值
        discreteEdit(m_ctx, "Collider Solid", &cc->solid, !solid);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(TR("Solid = blocks other colliders and the camera.\n"
                          "Off = a pure trigger (still visible, still blocks "
                          "the camera while flying through it)."));

    // ---- 线框显隐（默认显示；W/E/R 编辑中无论如何都强制显示）----
    bool showWire = cc->showWireframe;
    if (ImGui::Checkbox(TR("Show Wireframe"), &showWire)) {
        cc->showWireframe = showWire;   // discreteEdit 只压撤销栈，不赋值
        discreteEdit(m_ctx, "Collider Wireframe", &cc->showWireframe,
                     !showWire);
    }
    logRect("INS-COLLIDER-SHOW", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(TR("Show/hide the collision wireframe in the viewport.\n"
                          "Always visible while editing with W/E/R."));

    // ---- 摆放（相对实体）----
    const glm::vec3 p0 = cc->position;
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::DragFloat3("##collider-pos", glm::value_ptr(cc->position), sp(0.01f));
    fieldEdit(m_ctx, "Collider Position", &cc->position, p0);
    ImGui::SetItemTooltip(TRL("Position (relative to the object)"));

    const glm::vec3 r0 = cc->rotation;
    ImGui::SetNextItemWidth(-1.0f);
    glm::vec3 rotDeg = glm::degrees(cc->rotation);
    if (ImGui::DragFloat3("##collider-rot", glm::value_ptr(rotDeg), sp(0.5f))) {
        cc->rotation = glm::radians(rotDeg);
    }
    fieldEdit(m_ctx, "Collider Rotation", &cc->rotation, r0);
    ImGui::SetItemTooltip(TRL("Rotation (degrees)"));

    const glm::vec3 s0 = cc->scale;
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::DragFloat3("##collider-scale", glm::value_ptr(cc->scale), sp(0.01f),
                      0.01f, 100.0f);
    fieldEdit(m_ctx, "Collider Scale", &cc->scale, s0);
    ImGui::SetItemTooltip(TRL("Scale (uniform is not required)"));

    // ---- 胶囊尺寸 ----
    if (cc->shape == ecs::ColliderShape::Capsule) {
        const float rad0 = cc->capsuleRadius;
        ImGui::SetNextItemWidth(140.0f);
        ImGui::SliderFloat(TRL("Radius"), &cc->capsuleRadius, 0.01f, 10.0f, "%.3f");
        fieldEdit(m_ctx, "Collider Radius", &cc->capsuleRadius, rad0);

        const float hh0 = cc->capsuleHalfHeight;
        ImGui::SetNextItemWidth(140.0f);
        ImGui::SliderFloat(TRL("Half Height"), &cc->capsuleHalfHeight, 0.0f, 10.0f,
                           "%.3f");
        fieldEdit(m_ctx, "Collider Half Height", &cc->capsuleHalfHeight, hh0);
    } else {
        ImGui::TextDisabled(TR("convex hull: %zu points / %zu edges"),
                            cc->hullPoints.size(), cc->hullEdges.size());
    }

    if (ImGui::Button(TR("Remove Collision Body"))) {
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
        ImGui::TextDisabled(TR("run mode - decals are read-only here"));
        return;
    }

    std::string labels;   // 本帧画出的参数名（喂给 DECAL-PARAMS 签名日志）

    // ---- 开关 ----
    bool en = dc->enabled;
    if (ImGui::Checkbox(TRL("Enabled##decal"), &en)) {
        dc->enabled = en;   // discreteEdit 的契约：*target 已持有新值
        discreteEdit(m_ctx, "Toggle Decal", &dc->enabled, !en);
    }
    logRect("INS-DECAL-ENABLED", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Off = the decal is not collected at all.\n"
                          "The global switch (MYVK_DECALS) is separate - with "
                          "it off\nno decal is drawn no matter what this says."));
    }
    labels = "Enabled";

    // ---- 盒体尺寸（米，沿实体局部轴）----
    const glm::vec3 sz0 = dc->size;
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::DragFloat3(TRL("Size"), glm::value_ptr(dc->size), sp(0.01f), 1e-4f, 500.0f,
                      "%.3f");
    fieldEdit(m_ctx, "Decal Size", &dc->size, sz0);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Box volume in metres, along the object's own axes.\n"
                          "The decal only shows where the scene surface is "
                          "inside it."));
    }
    labels += ",Size";

    // ---- 覆盖度 ----
    const float op0 = dc->opacity;
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::SliderFloat(TRL("Opacity"), &dc->opacity, 0.0f, 1.0f, "%.2f");
    fieldEdit(m_ctx, "Decal Opacity", &dc->opacity, op0);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Upper bound on the coverage written to the "
                          "DBuffer.\n0 = invisible (the pass still runs - it "
                          "is the baseline knob)."));
    }
    labels += ",Opacity";

    const glm::vec3 t0 = dc->tint;
    ImGui::ColorEdit3(TRL("Tint"), glm::value_ptr(dc->tint),
                      ImGuiColorEditFlags_NoInputs);
    fieldEdit(m_ctx, "Decal Tint", &dc->tint, t0);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Multiplied with the albedo map - lets one texture "
                          "serve as several markers."));
    }
    labels += ",Tint";

    // ---- 贴花自己的 PBR 参数（按 coverage 覆盖被贴表面）----
    const float rg0 = dc->roughness;
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::SliderFloat(TRL("Roughness"), &dc->roughness, 0.02f, 1.0f, "%.2f");
    fieldEdit(m_ctx, "Decal Roughness", &dc->roughness, rg0);
    labels += ",Roughness";

    const float mt0 = dc->metallic;
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::SliderFloat(TRL("Metallic"), &dc->metallic, 0.0f, 1.0f, "%.2f");
    fieldEdit(m_ctx, "Decal Metallic", &dc->metallic, mt0);
    labels += ",Metallic";

    const float ns0 = dc->normalScale;
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::SliderFloat(TRL("Normal Scale"), &dc->normalScale, 0.0f, 2.0f, "%.2f");
    fieldEdit(m_ctx, "Decal Normal Scale", &dc->normalScale, ns0);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("0 = the decal leaves the surface normal alone,\n"
                          "1 = the normal map is used as authored."));
    }
    labels += ",NormalScale";

    // ---- 淡出 ----
    const float ef0 = dc->edgeFade;
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::SliderFloat(TRL("Edge Fade"), &dc->edgeFade, 0.0f, 0.5f, "%.2f");
    fieldEdit(m_ctx, "Decal Edge Fade", &dc->edgeFade, ef0);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Soft border, as a fraction of the half extents.\n"
                          "0 gives a hard edge -> a visible seam where the box "
                          "cuts the surface."));
    }
    labels += ",EdgeFade";

    const float fs0 = dc->fadeStart;
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::DragFloat(TRL("Fade Start"), &dc->fadeStart, sp(0.05f), 0.0f, 1000.0f,
                     "%.2f");
    fieldEdit(m_ctx, "Decal Fade Start", &dc->fadeStart, fs0);

    const float fe0 = dc->fadeEnd;
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::DragFloat(TRL("Fade End"), &dc->fadeEnd, sp(0.05f), 0.0f, 1000.0f, "%.2f");
    fieldEdit(m_ctx, "Decal Fade End", &dc->fadeEnd, fe0);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Distance fade, in metres from the camera to the "
                          "decal centre.\nEnd <= Start disables it (the "
                          "default) - that is the pixel-identical path."));
    }
    labels += ",FadeStart,FadeEnd";

    ImGui::TextDisabled(TR("fade: camera %.1f m .. %.1f m %s"),
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
        ImGui::TextUnformatted(TR(label));
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

    if (ImGui::Button(TR("Remove Decal"))) {
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
        ImGui::TextDisabled(TR("run mode - probes are read-only here"));
        return;
    }

    std::string labels;   // 本帧画出的参数名（喂给 PROBE-PARAMS 签名日志）

    bool en = pc->enabled;
    if (ImGui::Checkbox(TRL("Enabled##probe"), &en)) {
        pc->enabled = en;   // discreteEdit 的契约：*target 已持有新值
        discreteEdit(m_ctx, "Toggle Reflection Probe", &pc->enabled, !en);
    }
    logRect("INS-PROBE-ENABLED", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Off = the probe is not collected at all (and its\n"
                          "box no longer captures anything). The global switch\n"
                          "(MYVK_PROBES) is separate and off by default."));
    }
    labels = "Enabled";

    // ---- 盒体半轴（米，沿实体局部轴，再乘实体 scale）----
    const glm::vec3 ex0 = pc->extents;
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::DragFloat3(TRL("Extents"), glm::value_ptr(pc->extents), sp(0.05f), 1e-3f,
                      1000.0f, "%.2f");
    fieldEdit(m_ctx, "Probe Extents", &pc->extents, ex0);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Half extents of the box in metres, along the "
                          "object's own axes.\nOnly surfaces INSIDE this box "
                          "sample the probe.\nScaled by the object's scale, so "
                          "scaling the object scales its reach."));
    }
    labels += ",Extents";

    const float in0 = pc->intensity;
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::SliderFloat(TRL("Intensity"), &pc->intensity, 0.0f, 4.0f, "%.2f");
    fieldEdit(m_ctx, "Probe Intensity", &pc->intensity, in0);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Multiplier on this probe's specular contribution.\n"
                          "0 = as if the probe were not there (the capture "
                          "still runs)."));
    }
    labels += ",Intensity";

    bool bp = pc->boxProjection;
    if (ImGui::Checkbox(TRL("Box Projection##probe"), &bp)) {
        pc->boxProjection = bp;
        discreteEdit(m_ctx, "Toggle Box Projection", &pc->boxProjection, !bp);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Project the reflection onto the box walls before "
                          "sampling.\nOn (default) a probe beside a flat wall "
                          "gives correct parallax;\noff squeezes the whole "
                          "room into one direction - fine for a ball,\nwrong "
                          "for a floor."));
    }
    labels += ",BoxProjection";

    const float bd0 = pc->blendDistance;
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::SliderFloat(TRL("Blend Distance"), &pc->blendDistance, 0.01f, 20.0f,
                       "%.2f");
    fieldEdit(m_ctx, "Probe Blend Distance", &pc->blendDistance, bd0);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Width of the fade band just inside the box border,\n"
                          "in metres. Without it two neighbouring probes meet "
                          "at a hard seam."));
    }
    labels += ",BlendDistance";

    // ---- Capture ----
    // 只置一个请求标志：本面板拿不到 Renderer，由 EditorApp 每帧取走
    // 转交（与 requestCharacterWindow 同一个套路）。
    ImGui::Separator();
    if (ImGui::Button(TR("Capture Now"))) {
        m_ctx.requestProbeCapture();
        m_ctx.notify(TR("Reflection probe: recapture requested"));
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Re-render all six cube faces from this probe's "
                          "position and\nre-bake the GGX prefilter chain.\n"
                          "Normally you do not need this: the capture reruns\n"
                          "automatically whenever the probe data changes."));
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

    if (ImGui::Button(TR("Remove Reflection Probe"))) {
        m_ctx.structuralEdit("Remove Reflection Probe", [&]() {
            w.remove<ecs::ReflectionProbeComponent>(e);
        });
        m_lastProbeParamSig.clear();
    }
}

// ---------------------------------------------------------------- 地形（W4-4）
//
// 与贴花 / 探针同一形态：位置与朝向复用实体的 Transform，这一段只管
// 地形**自己的**那几个参数。
//
// 划分原则（路线图 §9 W4-4 原话「只做能看见，不做雕刻」）：
//   · **只读**：来自 `.vterrain` 的物理事实 —— 采样点数、米每像素、
//     世界尺寸、maxHeight、分块数。这些由文件决定，做成灰掉的输入框只会
//     让人以为"是不是我没权限"。要改就去改 `.vterrain`（或生成器）。
//   · **可改**：`visible`、`heightScale`、4 层材质的覆盖。
//   · **不做**：高度笔刷（那是 E24 的活）。
//
// ⚠ `.vterrain` 的 JSON 只读一次并按路径缓存：面板每帧都画，而这份文件在
//   交互期间不会变（换资产 = 换 source = 换 cache key）。
//   缓存里只有 TerrainDesc，**不读** 264 KB 的高度图 —— 那是渲染层的活，
//   `loadTerrainDesc` 存在的理由就是这个。
// ---------------------------------------------------------------- 出生点
//
// 2026-10-24：GameStart 从"一个标记"升级成"玩家出生参数"——位置 + 朝向 +
// 视角模式 + 要不要生成玩家。以前这两项（yaw / enabled）在面板上**根本改
// 不了**，因为 GameStart 没有网格、也不属于任何已有的组件段；现在加进来，
// 顺手把新增的两个字段也放这儿。
//
// 这一段是"添加人称角色"唯一的可编辑落点：视口菜单只负责建/选中，
// 具体是第一人称还是第三人称在这里改。
void InspectorPanel::drawGameStartSection(ecs::Entity e) {
    ecs::World& w = m_ctx.editorScene().world();

    auto* g = w.get<ecs::GameStartComponent>(e);
    if (!g) return;

    const bool open = sectionHeader("Player Start", true, e);
    logRect("INS-GAMESTART-HEADER", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    if (!open) return;

    if (!m_ctx.isEditing()) {
        ImGui::TextDisabled(TR("run mode - player start is read-only here"));
        return;
    }

    ImGui::TextDisabled(TR("The player is created here when the game runs"));
    hint(TR("adding a person-view player only configures this marker - no body "
            "is placed in the viewport"));

    // ---- 视角模式 ----
    // ⚠ Combo 的条目**不走** ImGui 的 label 路径，必须自己 TR 一遍，
    //   否则中文界面里这一列永远是英文（与"下拉框里混着两种语言"同一个坑）。
    const char* kModes[] = {TR("First person"), TR("Third person")};
    int mode = g->mode == ecs::PlayerMode::FirstPerson ? 0 : 1;
    ImGui::SetNextItemWidth(-110.0f);
    if (ImGui::Combo(TRL("Player view##gamestart"), &mode, kModes, 2)) {
        const ecs::PlayerMode before = g->mode;
        g->mode = mode == 0 ? ecs::PlayerMode::FirstPerson
                            : ecs::PlayerMode::ThirdPerson;
        discreteEdit(m_ctx, "Player View Mode", &g->mode, before);
        VK_LOG_INFO("INS-GAMESTART mode=%s",
                    g->mode == ecs::PlayerMode::FirstPerson ? "first" : "third");
    }
    logRect("INS-GAMESTART-MODE", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("First person: the camera IS the player.\n"
                          "Third person: a character with an orbit camera.\n"
                          "Takes effect the next time you Run."));
    }

    // ---- 要不要生成玩家 ----
    bool on = g->spawnOnRun;
    if (ImGui::Checkbox(TRL("Spawn player on run##gamestart"), &on)) {
        g->spawnOnRun = on;
        discreteEdit(m_ctx, "Toggle Spawn On Run", &g->spawnOnRun, !on);
    }
    logRect("INS-GAMESTART-SPAWN", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Off = this scene has no player (fly-through /\n"
                          "cut-scene levels). Nothing is spawned at all."));
    }

    // ---- 出生朝向（度；数据是弧度）----
    float yawDeg = glm::degrees(g->yaw);
    ImGui::SetNextItemWidth(-110.0f);
    if (ImGui::DragFloat(TRL("Spawn yaw##gamestart"), &yawDeg, 0.5f, -360.0f,
                         360.0f, "%.1f")) {
        const float before = g->yaw;
        g->yaw = glm::radians(yawDeg);
        fieldEdit(m_ctx, "Spawn Yaw", &g->yaw, before);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Heading the character faces on spawn, added on "
                          "top of the object's own rotation."));
    }

    bool en = g->enabled;
    if (ImGui::Checkbox(TRL("Enabled##gamestart"), &en)) {
        g->enabled = en;
        discreteEdit(m_ctx, "Toggle Player Start", &g->enabled, !en);
    }
    logRect("INS-GAMESTART-ENABLED", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
}

// ============================================================
// 地形层材质：材质包目录 + 刷新（2026-10-24）
//
// 用户原话："在 Terrain 的属性面板中的 Layer materials 那里，那里的地形材质
// 一般是指定一个目录里面，将对应的材质信息按照之前 E:\code\cpp\venn-resource
// 中的材质信息的形式去自动添加，这个通过一个刷新按钮去找到最新的材质信息"
//
// 所以在 Layer materials 上方给两行：
//   ① 目录输入框（回车 = 刷新；首次自动填"上次用过 / 猜到的"目录）
//   ② 档位（1k / 2k / 4k）+ Refresh 按钮 + 当前库里的条数
//
// 刷新一次做完四件事：扫目录 → 复制贴图 → 生成 .vmat → 合并清单 + 重载仓库。
// 以前这四件事只有 `tools/gen_material_library.py` 会做，用户加一个材质包
// 就得退出编辑器跑 Python 再回来 —— 这正是用户要求干掉的那一步。
// ============================================================
bool InspectorPanel::drawMaterialSourceRow() {
    static const char* kTiers[] = {"1k", "2k", "4k"};

    // 首次画到这里才读设置文件：编辑器启动时不碰它（与材质仓库的惰性
    // 加载同一个理由，见 EditorContext::materialLibrary）。
    if (!m_matSrcLoaded) {
        m_matSrcLoaded = true;
        assets::MaterialSourceSettings s = assets::loadMaterialSourceSettings();
        if (s.dir.empty()) s.dir = assets::guessMaterialSourceDir();
        std::snprintf(m_matSrcDir, sizeof(m_matSrcDir), "%s", s.dir.c_str());
        for (int i = 0; i < 3; ++i) {
            if (s.tier == kTiers[i]) m_matSrcTier = i;
        }
        VK_LOG_INFO("INS-MATSRC-INIT dir=%s tier=%s", m_matSrcDir,
                    kTiers[m_matSrcTier]);
    }

    bool imported = false;

    // 唯一的导入入口。回车、Refresh 按钮、以及空的目录自动猜测都走它 ——
    // 三条路各写一遍迟早漂移（和 createWithCurrentSettings 同一条理由）。
    const auto doImport = [&]() {
        assets::MaterialImportOptions o;
        o.sourceDir = m_matSrcDir;
        o.tier = kTiers[m_matSrcTier];
        if (o.sourceDir.empty()) {
            const std::string g = assets::guessMaterialSourceDir();
            if (!g.empty()) {
                std::snprintf(m_matSrcDir, sizeof(m_matSrcDir), "%s", g.c_str());
                o.sourceDir = g;
            }
        }
        assets::saveMaterialSourceSettings({o.sourceDir, o.tier});

        const assets::MaterialImportResult r = assets::importMaterialPackages(o);
        // 自动化锚点（verify_material_import 靠这一行判"刷新真的跑了"）
        VK_LOG_INFO("INS-MATSRC-REFRESH dir=%s tier=%s ok=%d imported=%d "
                    "added=%d",
                    o.sourceDir.c_str(), o.tier.c_str(), r.ok ? 1 : 0,
                    r.imported, r.added);
        if (r.ok) {
            // 结果文案自己拼，不用 `r.summary()` —— 那是给日志用的英文，
            // 界面这条要走译文（i18n 的 F 组会校验占位符一致）。
            char buf[192];
            std::snprintf(buf, sizeof(buf), TR("%d imported, %d new"),
                          r.imported, r.added);
            m_matSrcStatus = buf;
            if (!r.warnings.empty()) {
                m_matSrcStatus += " (" + std::to_string(r.warnings.size()) +
                                  " warning(s), see Console)";
            }
            m_ctx.reloadMaterialLibrary();   // 下拉 / 材质面板立刻能看到新条目
            imported = true;
            m_ctx.setStatus(std::string(TR("Materials imported: ")) + buf);
        } else {
            m_matSrcStatus = std::string(TR("Import failed: ")) + r.error;
            m_ctx.setStatus(m_matSrcStatus);
        }
    };

    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint(
            "##matsrc_dir",
            TR("folder of <name>/<name>_1k.gltf material packages"),
            m_matSrcDir, sizeof(m_matSrcDir),
            ImGuiInputTextFlags_EnterReturnsTrue)) {
        doImport();
    }
    logRect("INS-MATSRC-DIR", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            TR("A folder laid out like venn-resource:\n"
               "<name>/<name>_1k.gltf/  (glTF + .bin + textures/)\n"
               "Press Enter or hit Refresh to import every package in it."));
    }

    ImGui::TextDisabled(TR("Tier"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(72.0f);
    if (ImGui::Combo("##matsrc_tier", &m_matSrcTier, kTiers, 3)) {
        // 档位也是"下次还想用"的设置，改完就落盘
        assets::MaterialSourceSettings s;
        s.dir = m_matSrcDir;
        s.tier = kTiers[m_matSrcTier];
        assets::saveMaterialSourceSettings(s);
    }
    logRect("INS-MATSRC-TIER", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::Button(TRL("Refresh##matsrc"))) doImport();
    logRect("INS-MATSRC-REFRESH", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Scan the folder, copy its textures in, write the "
                             ".vmat files, then reload the library."));
    }

    // 两行信息放按钮**下面**：Inspector 只有 300 来像素宽，跟在按钮后面的
    // `SameLine` 文本会被截成 "3 mat..."（截图确认过）。
    ImGui::TextDisabled(TR("%d material(s)"),
                        static_cast<int>(m_ctx.materialLibrary().size()));
    if (!m_matSrcStatus.empty()) {
        ImGui::TextDisabled("%s", m_matSrcStatus.c_str());
    }
    logRect("INS-MATSRC-STATUS", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    return imported;
}

void InspectorPanel::drawTerrainSection(ecs::Entity e) {
    scene::Scene& sc = m_ctx.editorScene();
    ecs::World& w = sc.world();

    auto* tc = w.get<ecs::TerrainComponent>(e);
    if (!tc) return;

    const bool open = sectionHeader("Terrain", true, e);
    logRect("INS-TERRAIN-HEADER", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    if (!open) return;

    // ---- 资产信息（只读）----
    static std::string cachedSource;
    static terrain::TerrainDesc cachedDesc;
    static std::string cachedErr;
    if (cachedSource != tc->source) {
        cachedSource = tc->source;
        cachedErr.clear();
        cachedDesc = terrain::TerrainDesc{};
        if (!tc->source.empty() &&
            !terrain::loadTerrainDesc(tc->source, cachedDesc, &cachedErr)) {
            cachedDesc = terrain::TerrainDesc{};
        }
    }

    const float kLabelX = 100.0f;
    if (tc->source.empty()) {
        ImGui::TextDisabled(TR("no terrain asset assigned"));
    } else if (!cachedErr.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.40f, 1.0f), "%s",
                           cachedErr.c_str());
        hint("this .vterrain could not be parsed - the renderer skips it");
    } else {
        const float wm = static_cast<float>(cachedDesc.cols - 1) *
                         cachedDesc.metersPerPixel;
        const float hm = static_cast<float>(cachedDesc.rows - 1) *
                         cachedDesc.metersPerPixel;
        const int cxn = (cachedDesc.cols - 1) / terrain::kChunkCells;
        const int cyn = (cachedDesc.rows - 1) / terrain::kChunkCells;

        ImGui::TextDisabled(TR("samples"));
        ImGui::SameLine(kLabelX);
        ImGui::Text("%d x %d", cachedDesc.cols, cachedDesc.rows);
        ImGui::SameLine();
        ImGui::TextDisabled(TR("(%.2f m per sample)"),
                            static_cast<double>(cachedDesc.metersPerPixel));

        ImGui::TextDisabled(TR("world size"));
        ImGui::SameLine(kLabelX);
        ImGui::Text(TR("%.0f x %.0f m"), static_cast<double>(wm),
                    static_cast<double>(hm));

        ImGui::TextDisabled(TR("max height"));
        ImGui::SameLine(kLabelX);
        ImGui::Text("%.1f m", static_cast<double>(cachedDesc.maxHeight));
        ImGui::SameLine();
        ImGui::TextDisabled(TR("(edit the file to change)"));

        ImGui::TextDisabled(TR("chunks"));
        ImGui::SameLine(kLabelX);
        ImGui::Text("%d x %d = %d", cxn, cyn, cxn * cyn);
        ImGui::SameLine();
        ImGui::TextDisabled(TR("(%.0f m each)"),
                            static_cast<double>(terrain::kChunkCells) *
                                static_cast<double>(cachedDesc.metersPerPixel));

        ImGui::TextDisabled(TR("asset"));
        ImGui::SameLine(kLabelX);
        ImGui::TextUnformatted(tc->source.c_str());
    }
    logRect("INS-TERRAIN-ASSET", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());

    if (!m_ctx.isEditing()) {
        ImGui::TextDisabled(TR("run mode - terrain is read-only here"));
        return;
    }

    ImGui::Separator();

    bool vis = tc->visible;
    if (ImGui::Checkbox(TRL("Visible##terrain"), &vis)) {
        tc->visible = vis;
        discreteEdit(m_ctx, "Toggle Terrain", &tc->visible, !vis);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Off = the terrain is not drawn at all (and its\n"
                          "physics height field stops answering).\n"
                          "The object's own eye icon does the same thing; this "
                          "one is\nthe terrain-specific switch and it is "
                          "stored in the scene."));
    }
    logRect("INS-TERRAIN-VISIBLE", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());

    // ---- Height scale ----
    // ⚠ 渲染侧读 `maxHeight * heightScale`，物理侧（Heightfield.heightScale）
    //   必须读**同一个乘积**。所以拖这个滑条时"地形长高了、箱子陷进去了"
    //   是**预期**的：箱子是按旧的 heightAt 摆的。要摆回去用 Drop to Floor。
    const float hs0 = tc->heightScale;
    ImGui::SetNextItemWidth(-110.0f);
    // ⚠ 不用裸 `Height Scale`：那个键在词典里已经是**材质的**视差高度强度
    //   （`{"Height Scale", "高度强度"}`，kZh 是 emplace = 先到先得）。
    //   一词两义只能靠"让 UI 原文更具体"，不能靠加第二条译文。
    ImGui::SliderFloat(TRL("Height Scale##terrain"), &tc->heightScale, 0.05f,
                       4.0f, "%.2f");
    fieldEdit(m_ctx, "Terrain Height Scale", &tc->heightScale, hs0);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Multiplier on the asset's max height, in metres.\n"
                          "Rendering AND physics read this same value, so a "
                          "character\nstanding on the terrain follows it. "
                          "Props placed earlier do not -\nre-drop them with "
                          "Drop to Floor."));
    }
    logRect("INS-TERRAIN-HEIGHTSCALE", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());

    ImGui::Spacing();
    ImGui::TextDisabled(TR("Layer materials"));

    // 材质包目录 + 刷新（2026-10-24）。见 drawMaterialSourceRow 的说明。
    drawMaterialSourceRow();

    // ---- 4 层材质槽 ----
    // 空串 = 用 `.vterrain` 里那一个。做成"覆盖表"而不是"拷贝 4 个路径"：
    // 改了 `.vterrain` 的第 3 层，没被覆盖的槽自动跟着变（与 D2 同一条理由）。
    //
    // ⚠ 改完必须置 `dirty`：4 层贴图绑在实例的描述符集里，只有重建实例
    //   才换得掉（`Renderer::syncTerrains` 认这个位）。
    for (int i = 0; i < 4; ++i) {
        ImGui::PushID(i);
        const std::string inher = cachedErr.empty() ? cachedDesc.layerIds[i]
                                                    : std::string();
        const bool overridden = !tc->layerOverrides[i].empty();
        ImGui::Text("%d", i);
        ImGui::SameLine(24.0f);

        char buf[192];
        std::snprintf(buf, sizeof(buf), "%s", tc->layerOverrides[i].c_str());
        ImGui::SetNextItemWidth(-158.0f);
        if (ImGui::InputTextWithHint(
                "##terlayer", inher.empty() ? "(unused)" : inher.c_str(), buf,
                sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
            const std::string after = buf;
            m_ctx.structuralEdit("Set Terrain Layer", [&] {
                tc->layerOverrides[i] = after;
                tc->dirty = true;
            });
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("Material id for this layer, e.g. "
                              "materials/terrain_rock.\nLeave empty to inherit "
                              "the one in the .vterrain file.\n"
                              "Press Enter to apply."));
        }
        ImGui::SameLine();
        // 材质仓库下拉（2026-10-24）：从 `assets/materials/library.json`
        // 里挑一个直接填进这个槽。InputText 保留给"手写一个不在仓库里的
        // id" —— 两条路并存，谁都不挡谁。
        if (ImGui::SmallButton(TRL("Pick##terlayer"))) {
            ImGui::OpenPopup("##terlayer_pick");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", TR("Pick a material from the library"));
        }
        if (ImGui::BeginPopup("##terlayer_pick")) {
            const assets::MaterialLibrary& lib = m_ctx.materialLibrary();
            if (lib.size() == 0) {
                ImGui::TextDisabled("%s", TR("Material library is empty"));
            }
            for (const auto& le : lib.entries()) {
                const bool cur = (tc->layerOverrides[i] == le.id);
                std::string text = trBilingual(le.label, le.labelZh);
                if (!le.hasAlbedo()) text += TR("  (flat)");
                if (ImGui::Selectable(text.c_str(), cur)) {
                    const std::string id = le.id;
                    m_ctx.structuralEdit("Set Terrain Layer", [&] {
                        tc->layerOverrides[i] = id;
                        tc->dirty = true;
                    });
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (overridden) {
            if (ImGui::SmallButton(TRL("Reset##terlayer"))) {
                m_ctx.structuralEdit("Reset Terrain Layer", [&] {
                    tc->layerOverrides[i].clear();
                    tc->dirty = true;
                });
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(TR("Drop the override and inherit the "
                                  "material from the .vterrain file."));
            }
        } else {
            ImGui::TextDisabled(TR("inherit"));
        }
        // 每个槽一个独立锚点（`INS-TERRAIN-LAYER0..3`）：4 行共用一个 tag
        // 的话 `last_rect` 只能证明"有至少一行"，数不出"是不是 4 行"。
        char tag[32];
        std::snprintf(tag, sizeof(tag), "INS-TERRAIN-LAYER%d", i);
        logRect(tag, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        ImGui::PopID();
    }
    hint("weights (which layer wins where) live in the asset's splat map, not "
         "here - the editor has no height/sculpt brush yet");

    if (ImGui::Button(TR("Remove Terrain"))) {
        m_ctx.structuralEdit("Remove Terrain", [&] {
            w.remove<ecs::TerrainComponent>(e);
        });
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Removes the terrain component. The asset file "
                          "stays on disk."));
    }
    logRect("INS-TERRAIN-REMOVE", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
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
                ImGui::SetTooltip(TR("Linear HDR colour of this stop.\n"
                                  "Values above 1 are allowed and are what\n"
                                  "makes a spark bloom."));
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
                    TR("Where this stop sits along the particle's life\n"
                    "(0 = born, 1 = about to die).\n"
                    "Stops are re-sorted when you let go, so you can drag\n"
                    "one past its neighbours."));
            }
        }

        ImGui::SameLine();
        ImGui::BeginDisabled(ramp.count <= 1);
        if (ImGui::SmallButton(TR("x"))) {
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
            ImGui::SetTooltip(TR("Delete this stop (at least one must remain)."));
        }

        ImGui::PopID();
    }

    // ---------------------------------------------------------- Add stop
    ImGui::BeginDisabled(ramp.count >= ecs::kMaxColorStops);
    if (ImGui::SmallButton(TR("Add stop"))) {
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
    ImGui::TextDisabled(TR("%d / %d stops"), ramp.count, ecs::kMaxColorStops);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(TR("A new stop is inserted in the widest gap and takes\n"
                          "the colour already there, so adding one never\n"
                          "changes how the ramp looks by itself."));
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
        ImGui::TextDisabled(TR("run mode - particle emitters are read-only here"));
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
        if (ImGui::SmallButton(TR("Reset to default"))) {
            const ecs::ParticleEmitterComponent before = *fx;
            apply(*fx, defaults);
            if (std::memcmp(&before, fx, sizeof(before)) == 0) {
                m_ctx.setStatus(std::string(title) + " is already at default");
            } else {
                pushWhole((std::string("Reset ") + title).c_str(), before);
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("Restore every field of this module to its\n"
                              "built-in default. One undo step."));
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
    ImGui::TextDisabled(TR("%d modules"), static_cast<int>(sizeof(kModules) /
                                                       sizeof(kModules[0])));
    if (ImGui::SmallButton(TR("Expand all"))) m_fxForceModules = 1;
    // 矩形日志：E11 的既有约定 —— 每个能被自动化的控件都打一行
    // "在哪"，测试脚本据此注入点击，不用去截图里猜坐标。
    logRect("INS-FX-EXPAND-ALL", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::SmallButton(TR("Collapse all"))) m_fxForceModules = 0;
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
                ImGui::SetTooltip(TR("%s"), tip ? tip : "Enable this module.");
            }
        } else {
            ImGui::BeginDisabled();
            ImGui::Checkbox("##on", &on);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip(TR("%s"), kIntrinsicTip);
            }
        }
        ImGui::SameLine();

        // ② 折叠标题。
        // ⚠ 标签里 "##" 的后半段才是 ImGui 的 ID。所以绝不能让所有模块都
        //   叫 "XXX##fx" —— 那样 12 个折叠会共用一个 ID、共用一个展开状态。
        //   唯一的 key 必须放进 "##" 后面。
        char label[96];
        // 模块标题翻成中文，`##key` 原样保留（那是 ID，不是给人看的）。
        std::snprintf(label, sizeof(label), "%s##%s", TR(title), key);
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
        ImGui::SliderFloat(TRL("Duration##fx"), &fx->duration, 0.0f, 60.0f, TR("%.2f s"));
        fieldEdit(m_ctx, "Emitter Duration", &fx->duration, dur0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("Length of one emission cycle, in seconds.\n"
                              "0 = infinite (the default): the cycle never\n"
                              "restarts, so Looping / Prewarm / Bursts have\n"
                              "nothing to act on."));
        }
        markField("Duration");

        {
            const bool b0 = fx->looping;
            ImGui::Checkbox(TRL("Looping##fx"), &fx->looping);
            discreteEdit(m_ctx, "Emitter Looping", &fx->looping, b0);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(TR("Repeat the cycle forever.\n"
                                  "Only meaningful when Duration > 0."));
            }
            markField("Looping");
        }
        {
            const bool b0 = fx->prewarm;
            ImGui::Checkbox(TRL("Prewarm##fx"), &fx->prewarm);
            discreteEdit(m_ctx, "Emitter Prewarm", &fx->prewarm, b0);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    TR("Run one full cycle before the first rendered frame, so\n"
                    "the emitter opens at steady state instead of visibly\n"
                    "filling up.\n"
                    "• needs Duration > 0 and Looping on;\n"
                    "• keep it OFF for frozen-frame verification - it would\n"
                    "  make \"the first simulated step\" not be step 1."));
            }
            markField("Prewarm");
        }

        ImGui::SetNextItemWidth(-110.0f);
        const float sd0 = fx->startDelay;
        ImGui::SliderFloat(TRL("Start Delay##fx"), &fx->startDelay, 0.0f, 30.0f,
                           TR("%.2f s"));
        fieldEdit(m_ctx, "Emitter Start Delay", &fx->startDelay, sd0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("Nothing is emitted before this moment.\n"
                              "Rate over Distance is frozen for just as long,\n"
                              "so a delayed emitter does not \"catch up\"."));
        }
        markField("StartDelay");

        ImGui::SetNextItemWidth(-110.0f);
        const float life0 = fx->lifetime;
        ImGui::SliderFloat(TRL("Start Lifetime##fx"), &fx->lifetime, 0.05f, 30.0f,
                           TR("%.2fs"), ImGuiSliderFlags_Logarithmic);
        fieldEdit(m_ctx, "Emitter Lifetime", &fx->lifetime, life0);
        markField("Lifetime");

        ImGui::SetNextItemWidth(-110.0f);
        const float lifeV0 = fx->lifetimeVariance;
        ImGui::SliderFloat(TRL("Lifetime +/-##fx"), &fx->lifetimeVariance, 0.0f, 1.0f,
                           "%.2f");
        fieldEdit(m_ctx, "Emitter Lifetime Variance", &fx->lifetimeVariance,
                  lifeV0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("Relative spread of the lifetime\n"
                              "(0 = every particle dies at the same age, which\n"
                              "makes the pool empty in visible waves)."));
        }
        markField("LifetimeVariance");

        ImGui::SetNextItemWidth(-110.0f);
        const float spd0 = fx->startSpeed;
        ImGui::SliderFloat(TRL("Start Speed##fx"), &fx->startSpeed, 0.0f, 30.0f,
                           TR("%.2f m/s"));
        fieldEdit(m_ctx, "Emitter Speed", &fx->startSpeed, spd0);
        markField("Speed");

        ImGui::SetNextItemWidth(-110.0f);
        const float spdV0 = fx->speedVariance;
        ImGui::SliderFloat(TRL("Speed +/-##fx"), &fx->speedVariance, 0.0f, 1.0f,
                           "%.2f");
        fieldEdit(m_ctx, "Emitter Speed Variance", &fx->speedVariance, spdV0);
        markField("SpeedVariance");

        ImGui::SetNextItemWidth(-110.0f);
        const glm::vec3 g0 = fx->gravity;
        ImGui::DragFloat3(TRL("Gravity##fx"), glm::value_ptr(fx->gravity), sp(0.1f),
                          -50.0f, 50.0f, "%.2f");
        fieldEdit(m_ctx, "Emitter Gravity", &fx->gravity, g0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("Constant acceleration in m/s^2 (Unity: Gravity\n"
                              "Modifier). The engine is Z-up, so -9.8 on Z is\n"
                              "real gravity and a positive Z makes smoke rise.\n"
                              "With Simulation Space = Local the solver\n"
                              "subtracts the emitter's own motion, so a moving\n"
                              "torch still drips downwards."));
        }
        markField("Gravity");

        static const char* kSpaceNames[] = {"Local", "World"};
        int space = static_cast<int>(fx->simulationSpace);
        ImGui::SetNextItemWidth(-110.0f);
        if (ImGui::Combo(TRL("Simulation Space##fx"), &space, TrItems<2>(kSpaceNames), 2)) {
            const ecs::ParticleSimulationSpace before = fx->simulationSpace;
            fx->simulationSpace = static_cast<ecs::ParticleSimulationSpace>(space);
            discreteEdit(m_ctx, "Emitter Simulation Space", &fx->simulationSpace,
                         before);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("Local: particles are stored relative to the\n"
                              "emitter, so they follow it (torch, exhaust).\n"
                              "World: they are left behind in the scene\n"
                              "(campfire smoke).\n"
                              "This is a per-particle flag baked in at spawn -\n"
                              "changing it does not teleport live particles."));
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
        ImGui::SliderFloat(TRL("Rate over Time##fx"), &fx->rate, 0.0f, 1000.0f,
                           "%.0f/s");
        fieldEdit(m_ctx, "Emitter Rate", &fx->rate, rate0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                TR("Particles spawned per second. Uniform in time: every pool\n"
                "slot is an independent periodic emitter, so raising the rate\n"
                "never produces a burst - it just fills the pool faster.\n"
                "0 = only the bursts below emit."));
        }
        markField("Rate");

        ImGui::SetNextItemWidth(-110.0f);
        const float rod0 = fx->rateOverDistance;
        ImGui::SliderFloat(TRL("Rate over Distance##fx"), &fx->rateOverDistance,
                           0.0f, 300.0f, "%.0f/m");
        fieldEdit(m_ctx, "Emitter Rate Over Distance", &fx->rateOverDistance,
                  rod0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                TR("Extra particles per metre travelled, added on top of the\n"
                "rate above. Stationary = no effect; a fast parent draws a\n"
                "streak. Great for muzzle flashes and slashing trails.\n"
                "Distance only accumulates while a simulation step actually\n"
                "runs, so a frozen frame adds nothing."));
        }
        markField("RateOverDistance");

        // ---- Bursts ----
        // Unity 的 Bursts 是可增删的数组；这里固定 4 条（kMaxBursts）。
        // count = 0 的行就等于"没用"（GPU 侧按 count > 0 跳过），
        // 所以不需要额外的启用开关。
        ImGui::SeparatorText(TR("Bursts"));
        if (ImGui::BeginTable("##fxBursts", 5,
                              ImGuiTableFlags_SizingStretchProp |
                                  ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed,
                                    20.0f);
            ImGui::TableSetupColumn(TRL("Time"));
            ImGui::TableSetupColumn(TRL("Count"));
            ImGui::TableSetupColumn(TRL("Cycles"));
            ImGui::TableSetupColumn(TRL("Interval"));
            ImGui::TableHeadersRow();
            for (int i = 0; i < ecs::kMaxBursts; ++i) {
                ecs::ParticleBurst& b = fx->bursts[i];
                ImGui::PushID(i);
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled(TR("%d"), i);

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
                TR("A burst is a one-shot payload at a fixed moment in the\n"
                "cycle: Count particles at Time seconds after the cycle\n"
                "begins, repeated Cycles times every Interval seconds.\n"
                "Count = 0 disables the row. Bursts need Duration > 0 -\n"
                "with an infinite cycle there is no moment to fire at."));
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
        static const char* kShapeNames[] = {"Point (origin)", "Box", "Sphere",
                                            "Hemisphere", "Cone", "Circle",
                                            "Edge"};
        int shape = static_cast<int>(fx->shape);
        ImGui::SetNextItemWidth(-110.0f);
        if (ImGui::Combo(TRL("Shape##fx"), &shape, TrItems<7>(kShapeNames),
                         7)) {
            const ecs::ParticleShape before = fx->shape;
            fx->shape = static_cast<ecs::ParticleShape>(shape);
            discreteEdit(m_ctx, "Emitter Shape", &fx->shape, before);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("Where inside the emitter volume particles are\n"
                              "born.\n"
                              "Point: all from the origin.\n"
                              "Box: uniform inside Extents.\n"
                              "Sphere / Hemisphere: uniform inside Radius\n"
                              "  (not clustered at the centre).\n"
                              "Cone: along local +Z, circle: its base disc.\n"
                              "Edge: a line segment across local X."));
        }
        markField("Shape");

        static const char* kEmitFromNames[] = {"Volume", "Edge", "Shell"};
        int from = static_cast<int>(fx->emitFrom);
        ImGui::SetNextItemWidth(-110.0f);
        if (ImGui::Combo(TRL("Emit From##fx"), &from, TrItems<3>(kEmitFromNames),
                         3)) {
            const ecs::ParticleEmitFrom before = fx->emitFrom;
            fx->emitFrom = static_cast<ecs::ParticleEmitFrom>(from);
            discreteEdit(m_ctx, "Emitter Emit From", &fx->emitFrom, before);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("Volume: filled volume (weighted by Radius\n"
                              "  Thickness).\n"
                              "Shell: only the surface (sphere shell / box\n"
                              "  faces).\n"
                              "Edge: only the rim (box edges / cone base rim /\n"
                              "  the two ends of a line)."));
        }
        markField("EmitFrom");

        const ecs::ParticleShape sk = fx->shape;
        if (sk == ecs::ParticleShape::Box) {
            ImGui::SetNextItemWidth(-110.0f);
            const glm::vec3 ex0 = fx->shapeExtents;
            ImGui::DragFloat3(TRL("Extents##fx"), glm::value_ptr(fx->shapeExtents),
                              sp(0.05f), 0.0f, 100.0f, "%.2f");
            fieldEdit(m_ctx, "Emitter Extents", &fx->shapeExtents, ex0);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(TR("Half-extents of the box, in metres\n"
                                  "(the box is 2x this on each axis)."));
            }
            markField("Extents");
        } else if (sk != ecs::ParticleShape::Point) {
            ImGui::SetNextItemWidth(-110.0f);
            const float r0 = fx->shapeRadius;
            ImGui::SliderFloat(TRL("Radius##fx"), &fx->shapeRadius, 0.0f, 50.0f,
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
            ImGui::SliderFloat(TRL("Radius Thickness##fx"), &fx->radiusThickness,
                               0.0f, 1.0f, "%.2f");
            fieldEdit(m_ctx, "Emitter Radius Thickness", &fx->radiusThickness,
                      th0);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip(
                    TR("How much of the volume is used: 1 = solid, 0 = a hollow\n"
                    "shell hugging the surface. Ignored when Emit From is\n"
                    "Shell or Edge (those already sit on the surface)."));
            }
            markField("RadiusThickness");
        }

        if (sk != ecs::ParticleShape::Point) {
            const bool arcRelevant = (sk != ecs::ParticleShape::Edge);
            ImGui::BeginDisabled(!arcRelevant);
            ImGui::SetNextItemWidth(-110.0f);
            const float arc0 = fx->arc;
            ImGui::SliderFloat(TRL("Arc##fx"), &fx->arc, 1.0f, 360.0f, TR("%.0f deg"));
            fieldEdit(m_ctx, "Emitter Arc", &fx->arc, arc0);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip(
                    TR("Angular span around local +Z. 360 = the whole turn;\n"
                    "180 = a half disc (the classic \"fan\" emitter).\n"
                    "Uses the same deterministic angle as the shape sample,\n"
                    "so 360 stays bit-identical to the old behaviour."));
            }
            markField("Arc");

            ImGui::SetNextItemWidth(-110.0f);
            const float sr0 = fx->shapeRotation;
            ImGui::SliderFloat(TRL("Shape Rotation##fx"), &fx->shapeRotation, -180.0f,
                               180.0f, TR("%.0f deg"));
            fieldEdit(m_ctx, "Emitter Shape Rotation", &fx->shapeRotation, sr0);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(TR("Rotates the whole shape about local +Z -\n"
                                  "how you aim that half disc."));
            }
            markField("ShapeRotation");
        }

        ImGui::SetNextItemWidth(-110.0f);
        const float sp0 = fx->spread;
        ImGui::SliderFloat(TRL("Angle##fx"), &fx->spread, 0.0f, 90.0f, TR("%.0f deg"));
        fieldEdit(m_ctx, "Emitter Spread", &fx->spread, sp0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                TR("Half-angle of the emission cone around local +Z\n"
                "(0 = a straight jet, 90 = a full hemisphere).\n"
                "Sampled uniformly in cos(theta), so the cone mouth is evenly\n"
                "covered instead of clumping along the axis."));
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
        ImGui::TextUnformatted(TR("Linear X / Y / Z"));
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
                ImGui::SetTooltip(TR("A curve of 0,0,0,0 does nothing, which is\n"
                                  "why the module defaults to off."));
            }
            markField("VelZ");
        }

        {
            const bool b0 = fx->velOverLifeWorldSpace;
            ImGui::Checkbox(TRL("World Space##fx"), &fx->velOverLifeWorldSpace);
            discreteEdit(m_ctx, "Emitter Velocity World Space",
                         &fx->velOverLifeWorldSpace, b0);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(TR("Off (default): the curves are read in the\n"
                                  "emitter's own frame, so they turn with it.\n"
                                  "On: they are world axes."));
            }
            markField("VelWorldSpace");
        }

        ImGui::SeparatorText(TR("Orbital / radial"));
        ImGui::SetNextItemWidth(-110.0f);
        const glm::vec3 o0 = fx->orbital;
        ImGui::DragFloat3(TRL("Orbital##fx"), glm::value_ptr(fx->orbital), sp(1.0f),
                          -720.0f, 720.0f, TR("%.0f deg/s"));
        fieldEdit(m_ctx, "Emitter Orbital", &fx->orbital, o0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("Angular velocity about the emitter's local axes\n"
                              "(degrees/second). Even a single non-zero axis\n"
                              "turns a jet into a swirl; all three give the\n"
                              "fire-twister look."));
        }
        markField("Orbital");

        ImGui::SetNextItemWidth(-110.0f);
        const float oo0 = fx->orbitalOffset;
        ImGui::SliderFloat(TRL("Orbital Offset##fx"), &fx->orbitalOffset, 0.0f, 10.0f,
                           TR("%.2f m"));
        fieldEdit(m_ctx, "Emitter Orbital Offset", &fx->orbitalOffset, oo0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("Radius offset for the orbital motion - pushes\n"
                              "the rotation axis away from the particle, so it\n"
                              "orbits a circle instead of spinning in place."));
        }
        markField("OrbitalOffset");

        ImGui::SetNextItemWidth(-110.0f);
        const float rd0 = fx->radial;
        ImGui::SliderFloat(TRL("Radial##fx"), &fx->radial, -20.0f, 20.0f, TR("%.2f m/s"));
        fieldEdit(m_ctx, "Emitter Radial", &fx->radial, rd0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("Speed along the direction away from the emitter\n"
                              "origin. Positive = outward, negative = the\n"
                              "particles gather back in (implosions)."));
        }
        markField("Radial");

        {
            const glm::vec4 c0 = fx->speedModifier;
            ImGui::TextUnformatted(TR("Speed Modifier (multiplier, 0 0 0 0 = off)"));
            ImGui::SetNextItemWidth(-110.0f);
            ImGui::SliderFloat4("##fxSpeedMod",
                                glm::value_ptr(fx->speedModifier), 0.0f, 4.0f,
                                "%.2f");
            fieldEdit(m_ctx, "Emitter Speed Modifier", &fx->speedModifier, c0);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(TR("Multiplies |velocity| over the lifetime -\n"
                                  "the cheapest way to make a burst decelerate\n"
                                  "(1, 0.5, 0.2, 0) or a plume accelerate."));
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
        ImGui::SliderFloat(TRL("Speed Limit##fx"), &fx->limitSpeed, 0.0f, 50.0f,
                           TR("%.2f m/s"));
        fieldEdit(m_ctx, "Emitter Limit Speed", &fx->limitSpeed, ls0);
        markField("LimitSpeed");

        ImGui::SetNextItemWidth(-110.0f);
        const float ld0 = fx->limitDampen;
        ImGui::SliderFloat(TRL("Dampen##fx"), &fx->limitDampen, 0.0f, 1.0f, "%.2f /s");
        fieldEdit(m_ctx, "Emitter Limit Dampen", &fx->limitDampen, ld0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("Fraction of the excess speed removed per second\n"
                              "once the limit is exceeded. 0 = a hard clamp,\n"
                              "1 = snap straight down to the limit."));
        }
        markField("LimitDampen");

        ImGui::SeparatorText(TR("Air drag (engine extra)"));
        ImGui::SetNextItemWidth(-110.0f);
        const float drag0 = fx->drag;
        ImGui::SliderFloat(TRL("Drag##fx"), &fx->drag, 0.0f, 5.0f, "%.2f /s");
        fieldEdit(m_ctx, "Emitter Drag", &fx->drag, drag0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("Exponential velocity damping (1/s), 0 = none.\n"
                              "Integrated analytically as exp(-drag*dt), so it\n"
                              "is stable at any frame rate.\n"
                              "Not a Unity module field - the damping it has is\n"
                              "the per-particle Dampen above; a global air drag\n"
                              "is what an engine is expected to provide."));
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
        ImGui::DragFloat3(TRL("Force##fx"), glm::value_ptr(fx->force), sp(0.1f),
                          -100.0f, 100.0f, "%.2f");
        fieldEdit(m_ctx, "Emitter Force", &fx->force, f0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("Added on top of Gravity, so (0,0,9.8) with the\n"
                              "default -9.8 gravity gives weightless particles."));
        }
        markField("Force");

        ImGui::SetNextItemWidth(-110.0f);
        const float fr0 = fx->forceRandomize;
        ImGui::SliderFloat(TRL("Randomize##fx"), &fx->forceRandomize, 0.0f, 1.0f,
                           "%.2f");
        fieldEdit(m_ctx, "Emitter Force Randomize", &fx->forceRandomize, fr0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("Per-particle jitter of the force vector, as a\n"
                              "fraction of its length. Breaks up the \"every\n"
                              "particle takes the identical path\" look."));
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

        ImGui::TextUnformatted(TR("Alpha over life"));
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
                ImGui::SetTooltip(TR("Opacity at t = 0, 1/3, 2/3, 1 of the life.\n"
                                  "Fire fades in fast and out slowly; smoke\n"
                                  "does the opposite."));
            }
            logRect("INS-FX-ALPHA-CURVE", ImGui::GetItemRectMin(),
                    ImGui::GetItemRectMax());
            markField("AlphaOverLife");
        }

        ImGui::SetNextItemWidth(-110.0f);
        const float em0 = fx->emissive;
        // 线性：0 是合法值（"不发光，只当普通贴片"），对数滑条不允许 min = 0。
        ImGui::SliderFloat(TRL("Emissive##fx"), &fx->emissive, 0.0f, 20.0f, "%.2f");
        fieldEdit(m_ctx, "Emitter Emissive", &fx->emissive, em0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("Multiplier on the colour. Above ~1 the particles\n"
                              "cross the bloom threshold, which is what turns a\n"
                              "few bright quads into glowing fire.\n"
                              "Kept here rather than in Renderer because it is\n"
                              "an appearance property of the colour itself."));
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
            ImGui::SetTooltip(TR("Size multiplier at t = 0, 1/3, 2/3, 1 of the\n"
                              "particle's life, linearly interpolated in\n"
                              "between. Multiplied by the Renderer's Start Size\n"
                              "and by the global fx.sizeScale."));
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
        ImGui::SliderFloat(TRL("Start Rotation##fx"), &fx->startRotation, -180.0f,
                           180.0f, TR("%.0f deg"));
        fieldEdit(m_ctx, "Emitter Start Rotation", &fx->startRotation, sr0);
        markField("StartRotation");

        ImGui::SetNextItemWidth(-110.0f);
        const float srv0 = fx->startRotationVariance;
        ImGui::SliderFloat(TRL("Rotation +/-##fx"), &fx->startRotationVariance, 0.0f,
                           180.0f, TR("%.0f deg"));
        fieldEdit(m_ctx, "Emitter Start Rotation Variance",
                  &fx->startRotationVariance, srv0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("Random half-range of the initial angle (uniform).\n"
                              "180 is the default: every particle starts at a\n"
                              "random angle, which is what a fire needs -\n"
                              "identical angles make the sprites march in step."));
        }
        markField("StartRotationVariance");

        ImGui::SetNextItemWidth(-110.0f);
        const float av0 = fx->angularVelocity;
        ImGui::SliderFloat(TRL("Angular Velocity##fx"), &fx->angularVelocity,
                           -720.0f, 720.0f, TR("%.0f deg/s"));
        fieldEdit(m_ctx, "Emitter Angular Velocity", &fx->angularVelocity, av0);
        markField("AngularVelocity");

        ImGui::SetNextItemWidth(-110.0f);
        const float avv0 = fx->angularVelocityVariance;
        ImGui::SliderFloat(TRL("Spin +/-##fx"), &fx->angularVelocityVariance, 0.0f,
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
        ImGui::SliderFloat(TRL("Strength##fx"), &fx->noiseStrength, 0.0f, 20.0f,
                           "%.2f");
        fieldEdit(m_ctx, "Emitter Noise Strength", &fx->noiseStrength, ns0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("Magnitude of the acceleration the field applies\n"
                              "(m/s^2). Small values (0.5-2) are already a lot:\n"
                              "this is an acceleration, not a velocity."));
        }
        markField("NoiseStrength");

        ImGui::SetNextItemWidth(-110.0f);
        const float nf0 = fx->noiseFrequency;
        ImGui::SliderFloat(TRL("Frequency##fx"), &fx->noiseFrequency, 0.01f, 5.0f,
                           "%.2f");
        fieldEdit(m_ctx, "Emitter Noise Frequency", &fx->noiseFrequency, nf0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("Spatial frequency of the field: low = big slow\n"
                              "swirls, high = fine chatter."));
        }
        markField("NoiseFrequency");

        ImGui::SetNextItemWidth(-110.0f);
        const float nss0 = fx->noiseScrollSpeed;
        ImGui::SliderFloat(TRL("Scroll Speed##fx"), &fx->noiseScrollSpeed, 0.0f, 5.0f,
                           "%.2f");
        fieldEdit(m_ctx, "Emitter Noise Scroll Speed", &fx->noiseScrollSpeed,
                  nss0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("How fast the noise field itself drifts (0 = the\n"
                              "field is frozen in space, so particles merely\n"
                              "fall through a static pattern)."));
        }
        markField("NoiseScrollSpeed");

        ImGui::SetNextItemWidth(-110.0f);
        const float nd0 = fx->noiseDamping;
        ImGui::SliderFloat(TRL("Damping##fx"), &fx->noiseDamping, 0.0f, 1.0f, "%.2f");
        fieldEdit(m_ctx, "Emitter Noise Damping", &fx->noiseDamping, nd0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("Damps the noise contribution per second.\n"
                              "Higher = the particles fly straighter and the\n"
                              "noise only nuzzles them."));
        }
        markField("NoiseDamping");

        ImGui::SetNextItemWidth(-110.0f);
        const int no0 = fx->noiseOctaves;
        ImGui::SliderInt(TRL("Octaves##fx"), &fx->noiseOctaves, 1, 4, "%d");
        fieldEdit(m_ctx, "Emitter Noise Octaves", &fx->noiseOctaves, no0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("fBm layers: 1 = one smooth field, 4 = the same\n"
                              "field plus three finer copies. Each layer costs\n"
                              "three more noise samples per particle."));
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
        ImGui::SliderFloat(TRL("Start Size##fx"), &fx->startSize, 0.005f, 2.0f,
                           TR("%.3f m"), ImGuiSliderFlags_Logarithmic);
        fieldEdit(m_ctx, "Emitter Start Size", &fx->startSize, size0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("Edge length of the quad in metres (this engine\n"
                              "uses a round sprite, so it is a diameter).\n"
                              "Zero particles are the same size - Size over\n"
                              "Lifetime is what varies them."));
        }
        markField("StartSize");

        static const char* kRenderNames[] = {"Billboard", "Stretched Billboard",
                                             "Horizontal Billboard",
                                             "Vertical Billboard"};
        int rmode = static_cast<int>(fx->renderMode);
        ImGui::SetNextItemWidth(-110.0f);
        if (ImGui::Combo(TRL("Render Mode##fx"), &rmode, TrItems<4>(kRenderNames),
                         4)) {
            const ecs::ParticleRenderMode before = fx->renderMode;
            fx->renderMode = static_cast<ecs::ParticleRenderMode>(rmode);
            discreteEdit(m_ctx, "Emitter Render Mode", &fx->renderMode, before);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("Billboard: always faces the camera.\n"
                              "Stretched: the long axis follows the velocity\n"
                              "(rain streaks, sparks).\n"
                              "Horizontal: lies flat, facing world +Z (ground\n"
                              "shockwaves).\n"
                              "Vertical: stands up but is locked about world Z\n"
                              "(fire columns, waterfalls)."));
        }
        markField("RenderMode");

        ImGui::SetNextItemWidth(-110.0f);
        const float st0 = fx->stretchScale;
        ImGui::SliderFloat(TRL("Stretch##fx"), &fx->stretchScale, 0.0f, 5.0f, "%.2f");
        fieldEdit(m_ctx, "Emitter Stretch", &fx->stretchScale, st0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("Stretched billboard only: the quad's length grows\n"
                              "by |v| * this. 0 = a plain camera-facing quad.\n"
                              "Rain and sparks want ~0.1."));
        }
        markField("Stretch");

        static const char* kSortNames[] = {"None", "By Distance", "Oldest First",
                                           "Youngest First"};
        int smode = static_cast<int>(fx->sortMode);
        ImGui::SetNextItemWidth(-110.0f);
        if (ImGui::Combo(TRL("Sort Mode##fx"), &smode, TrItems<4>(kSortNames),
                         4)) {
            const ecs::ParticleSortMode before = fx->sortMode;
            fx->sortMode = static_cast<ecs::ParticleSortMode>(smode);
            discreteEdit(m_ctx, "Emitter Sort Mode", &fx->sortMode, before);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                TR("None (default): draw in pool order - free, and correct for\n"
                "additive blending (order does not matter for light).\n"
                "The other three run a bucket sort on the GPU each frame and\n"
                "cost a dispatch (see FX-DUMP's sort= field)."));
        }
        markField("SortMode");

        static const char* kBlendNames[] = {"Additive", "Alpha Blend"};
        int blend = static_cast<int>(fx->blend);
        ImGui::SetNextItemWidth(-110.0f);
        if (ImGui::Combo(TRL("Blend##fx"), &blend, TrItems<2>(kBlendNames), 2)) {
            const ecs::ParticleBlend before = fx->blend;
            fx->blend = static_cast<ecs::ParticleBlend>(blend);
            discreteEdit(m_ctx, "Emitter Blend", &fx->blend, before);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("Additive: light adds up - fire, sparks, magic.\n"
                              "Alpha: normal over-blending - smoke, dust, water.\n"
                              "Blend mode is fixed pipeline state, so particles\n"
                              "are compacted into two lists and drawn with two\n"
                              "indirect draws (the empty one costs nothing)."));
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
        ImGui::SliderFloat(TRL("Soft Distance##fx"), &fx->softDistance, 0.0f, 5.0f,
                           TR("%.2f m"));
        fieldEdit(m_ctx, "Emitter Soft Distance", &fx->softDistance, sd0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("Distance over which the fade happens (metres).\n"
                              "Too large and particles floating well in front of\n"
                              "a wall go transparent; 0.2-0.5 is the useful\n"
                              "range. The renderer takes the MAX over all\n"
                              "emitters."));
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

    if (ImGui::Button(TR("Remove Particle Emitter"))) {
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
        ImGui::TextDisabled(TR("(null mesh)"));
        return;
    }

    const assets::Mesh& m = *mc->mesh;
    const assets::MeshSource& s = m.source();

    // ---- 身份（只读）----
    static const char* kKinds[] = {"Unknown", "Builtin", "OBJ", "glTF"};
    const int ki = static_cast<int>(s.kind);
    ImGui::Text(TR("asset: %s"), kKinds[(ki >= 0 && ki < 4) ? ki : 0]);
    if (!s.shape.empty()) ImGui::Text(TR("shape: %s"), s.shape.c_str());
    if (!s.path.empty()) ImGui::Text(TR("file : %s"), s.path.c_str());
    if (s.subMeshIndex >= 0)
        ImGui::Text(TR("sub  : primitive #%d"), s.subMeshIndex);
    if (!s.name.empty()) {
        ImGui::TextDisabled(TR("key  : %s"), s.name.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                TR("AssetManager cache key.\n"
                "Two meshes share GPU buffers exactly when their keys match."));
        }
    }

    // ---- 几何统计（只读）----
    ImGui::SeparatorText(TR("Geometry"));
    ImGui::Text(TR("vertices : %zu"), m.vertices().size());
    ImGui::Text(TR("triangles: %zu"), m.indices().size() / 3);
    if (m.hasBounds()) {
        const glm::vec3 mn = m.boundsMin();
        const glm::vec3 mx = m.boundsMax();
        const glm::vec3 ext = mx - mn;
        ImGui::Text(TR("size     : %.3f x %.3f x %.3f"),
                    static_cast<double>(ext.x), static_cast<double>(ext.y),
                    static_cast<double>(ext.z));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("local AABB\nmin (%.3f, %.3f, %.3f)\nmax (%.3f, %.3f, %.3f)"),
                              static_cast<double>(mn.x), static_cast<double>(mn.y),
                              static_cast<double>(mn.z), static_cast<double>(mx.x),
                              static_cast<double>(mx.y), static_cast<double>(mx.z));
        }
    }
    hint("read-only - these come from the file or from the built-in generator");

    // ---- 生成参数（可拖拽；只有内置图元才有）----
    ImGui::SeparatorText(TR("Parameters"));

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
            ImGui::DragFloat(TRL("Size"), &p.size, sp(0.01f), 0.01f, 500.0f, "%.3f");
            probe("Size");
            edited |= ImGui::IsItemDeactivatedAfterEdit();
            labels = "Size";
            break;
        }
        case ecs::MeshPrimitive::Sphere: {
            ImGui::SetNextItemWidth(-110.0f);
            ImGui::DragFloat(TRL("Radius"), &p.radius, sp(0.005f), 0.001f, 200.0f, "%.3f");
            probe("Radius");
            edited |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::SetNextItemWidth(-110.0f);
            ImGui::DragInt(TRL("Segments"), &p.segments, 0.5f, 3, 256);
            probe("Segments");
            edited |= ImGui::IsItemDeactivatedAfterEdit();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(TR("longitude divisions - drives the vertex count"));
            ImGui::SetNextItemWidth(-110.0f);
            ImGui::DragInt(TRL("Rings"), &p.rings, 0.5f, 2, 256);
            probe("Rings");
            edited |= ImGui::IsItemDeactivatedAfterEdit();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(TR("latitude divisions - drives the vertex count"));
            labels = "Radius,Segments,Rings";
            break;
        }
        case ecs::MeshPrimitive::Cylinder: {
            ImGui::SetNextItemWidth(-110.0f);
            ImGui::DragFloat(TRL("Radius"), &p.radius, sp(0.005f), 0.001f, 200.0f, "%.3f");
            probe("Radius");
            edited |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::SetNextItemWidth(-110.0f);
            ImGui::DragInt(TRL("Segments"), &p.segments, 0.5f, 3, 256);
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
    m_ctx.setStatus(TR("Rebuild ") + who);
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
// 88 是量出来的：最长的属性名 "Normal Scale" 在默认字体下约 84px，留 4px 余量。
// 这两个数是**宽面板下的目标值**，不是硬约束 —— 窄面板时由材质段自己按可用宽
// 把值列压小、优先保住槽位（见 `drawMaterialSection` 里的 `valueW`）。
// ⚠ 旧注释写"面板默认 330px → 槽位还剩 ~116px"是 **E11.1 之前**的口径（那时行首
//   只有 ~8px、还没有左侧那条 48px 图标导航条），**别照抄**。
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
        // ⚠ 窄面板下 `fitText` 只能保住"...r.png"这一点点尾巴（330px 面板里
        //   槽位最多拿到 ~99px，装不下 18 个字符的文件名）—— 三个同后缀的槽
        //   在画面上长得一样。**唯一能报出完整名字的地方就是 tooltip。**
        if (tex) {
            ImGui::SetTooltip(TR("%s\n\n%s\n\ndrag another image here to replace\n"
                              "right-click to clear"),
                              displayNameOf(tex).c_str(),
                              TR(hintText ? hintText : slotId));
        } else {
            ImGui::SetTooltip(TR("%s\n\n%s"), TR(hintText ? hintText : slotId),
                              TR("drag an image from the Content Browser here"));
        }
    }

    // ---- 右键清空（方块太小，放不下 Clear 按钮）----
    if (tex && ImGui::BeginPopupContextItem("##clear")) {
        if (ImGui::MenuItem(TR("Clear texture"))) {
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

// Inspector 的材质行标签 → 覆盖位（W1-3）。标签是**英文原文**（TR 之前），
// 所以这张表与当前语言无关。
static std::uint32_t overriddenBitOf(const char* label) {
    using M = assets::Material;
    struct Entry {
        const char* name;
        std::uint32_t bit;
    };
    static const Entry kTable[] = {
        {"Base Color", M::kOvBaseColor},
        {"Metallic", M::kOvMetallic},
        {"Roughness", M::kOvRoughness},
        {"Emissive", M::kOvEmissive},
        {"AO", M::kOvAo},
        {"Normal Scale", M::kOvNormalScale},
        {"Height Scale", M::kOvHeightScale},
        {"Alpha Mode", M::kOvAlphaMode},
        {"Alpha Cutoff", M::kOvAlphaCutoff},
        {"Dither Alpha", M::kOvDitherAlpha},
        {"Shading Model", M::kOvShadingModel},
        {"Double Sided", M::kOvDoubleSided},
    };
    for (const auto& t : kTable) {
        if (std::strcmp(t.name, label) == 0) return t.bit;
    }
    return 0;
}

void InspectorPanel::drawMaterialSection(ecs::Entity e) {
    scene::Scene& sc = m_ctx.activeScene();
    ecs::World& w = sc.world();

    auto* matc = w.get<ecs::MaterialComponent>(e);
    if (!matc || !matc->material) return;
    assets::Material* mat = matc->material;
    // 本帧开始时的参数快照：函数结束时 diff 一次就知道用户动了哪几个字段。
    // 这样不用在 12 个 fieldEdit 调用点各插一行（漏一个就静默不记覆盖）。
    assets::Material::Params paramsAtStart = mat->captureParams();

    if (!sectionHeader("Material", true, e))
        return;
    // 锚点：证明"材质段这一帧真的画了"（页签重构后，它只在 Material 页出现，
    // tools/verify_inspector_tabs.py 靠这个正/负对照判断页签隔离生效）。
    logRect("INS-MATERIAL-HEADER", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());

    ImGui::Text(TR("name: %s"), mat->name.c_str());

    // ============================================================
    // 从材质仓库直接换（2026-10-24）
    // ============================================================
    // 用户的要求："将来可以……给各种其他的静态网格体在属性栏中绑定对应的
    // 材质信息"。所以这一段的入口长在**属性栏**里，而不是只在材质面板里
    // —— 选中一个网格体、展开 Material 段、点一下换成草地材质，这才是
    // 最顺的一条路。
    //
    // 换的是 `MaterialComponent::material` 这**一个指针**（走 structuralEdit
    // 以便撤销），不走"复制参数"：材质资产是共享的，改一处两处都变。
    {
        ImGui::SameLine();
        if (ImGui::SmallButton(TRL("Pick##ins_mat_lib"))) {
            ImGui::OpenPopup("##ins_mat_lib_pick");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", TR("Replace this material with one from the library"));
        }
        if (ImGui::BeginPopup("##ins_mat_lib_pick")) {
            const assets::MaterialLibrary& lib = m_ctx.materialLibrary();
            if (lib.size() == 0) {
                ImGui::TextDisabled("%s", TR("Material library is empty"));
            }
            for (const auto& le : lib.entries()) {
                std::string text = trBilingual(le.label, le.labelZh);
                if (!le.hasAlbedo()) text += TR("  (flat)");
                if (ImGui::Selectable(text.c_str(), mat->id == le.id)) {
                    // 解析走 SceneSerializer 那条路（id 去重 / 贴图解析 /
                    // 导入默认值都在里面），与"场景里引用同一个 .vmat"
                    // 拿到的是同一个对象。
                    assets::Material* next =
                        scene::loadMaterialAsset(m_ctx.assets(), le.vmatPath);
                    if (next) {
                        ecs::Entity ent = e;
                        m_ctx.structuralEdit("Set Material From Library",
                                             [this, ent, next]() {
                            if (auto* mc = m_ctx.activeScene().world().get<
                                    ecs::MaterialComponent>(ent)) {
                                mc->material = next;
                            }
                        });
                        VK_LOG_INFO("INS-MAT-LIB entity=%u id=%s",
                                    static_cast<unsigned>(e.id),
                                    le.id.c_str());
                    } else {
                        VK_LOG_WARN("INS-MAT-LIB-FAIL id=%s path=%s",
                                    le.id.c_str(), le.vmatPath.c_str());
                    }
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
    }

    // ============================================================
    // 材质实例（W1-3）
    //
    // 关卡里 300 件东西引用同一个母材质时，改一次 roughness 要生效 300 次 ——
    // 这就是"实例"这层的全部目的。反过来，某一件要不一样时，`Make Unique`
    // 给它派生一个只属于自己的副本，之后改的只是它。
    // ============================================================
    {
        const bool wasInstance = mat->isInstance();

        if (wasInstance) {
            const assets::Material* par = mat->parent;
            ImGui::TextColored(ImVec4(0.62f, 0.82f, 1.0f, 1.0f),
                               TR("Instance of %s"),
                               par ? (par->id.empty() ? par->name.c_str()
                                                      : par->id.c_str())
                                   : "?");
        } else {
            ImGui::TextUnformatted(TR("Material (asset)"));
        }
        logRect("INS-MAT-INSTANCE", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());

        ImGui::SameLine();
        ImGui::BeginDisabled(wasInstance || mat->source.kind ==
                                              assets::MaterialSource::Kind::Model);
        if (ImGui::SmallButton(TR("Make Unique##mat"))) {
            // 结构变化（换了一个材质对象）→ 走整场景快照的撤销
            assets::Material* before = mat;
            ecs::Entity ent = e;
            const std::string instName =
                mat->name + "@e" + std::to_string(static_cast<unsigned>(e.id));
            m_ctx.structuralEdit("Material Make Unique", [this, ent, before,
                                                          instName]() {
                assets::Material* inst = m_ctx.assets().makeInstanceOf(
                    before, instName);
                if (!inst) return;
                if (auto* mc = m_ctx.activeScene().world().get<
                        ecs::MaterialComponent>(ent))
                    mc->material = inst;
            });
            mat = m_ctx.activeScene().world().get<ecs::MaterialComponent>(e)
                      ? m_ctx.activeScene().world()
                            .get<ecs::MaterialComponent>(e)
                            ->material
                      : mat;
            if (mat && mat != before) {
                // 换了一个材质对象 → 参数基准也要跟着换，否则本帧末尾的
                // diff 会拿"上一个材质的参数"当基准，把所有字段都算成覆盖。
                paramsAtStart = mat->captureParams();
                VK_LOG_INFO("MATERIAL-INSTANCE action=make-unique entity=%u "
                            "parent=%s name=%s",
                            static_cast<unsigned>(e.id),
                            assets::assetLogSafe(before->id.empty()
                                                     ? before->name
                                                     : before->id)
                                .c_str(),
                            assets::assetLogSafe(mat->name).c_str());
            }
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(
                TR("Give this entity its own copy of the material.\n"
                   "Edits then affect only this object; the rest keep following\n"
                   "the shared material."));
        logRect("INS-MAT-MAKEUNIQUE", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());

        if (!mat) return;

        // 覆盖项一览 + 逐字段 Revert（清除该位 → 重新跟随母材质）
        if (mat->isInstance() && mat->overridden) {
            static const struct {
                assets::Material::OverrideBits bit;
                const char* name;
                const char* label;
            } kOv[] = {
                {assets::Material::kOvBaseColor, "base-color", "Base Color"},
                {assets::Material::kOvMetallic, "metallic", "Metallic"},
                {assets::Material::kOvRoughness, "roughness", "Roughness"},
                {assets::Material::kOvNormalScale, "normal-scale", "Normal Scale"},
                {assets::Material::kOvAo, "ao", "AO"},
                {assets::Material::kOvEmissive, "emissive", "Emissive"},
                {assets::Material::kOvAlphaMode, "alpha-mode", "Alpha Mode"},
                {assets::Material::kOvAlphaCutoff, "alpha-cutoff", "Alpha Cutoff"},
                {assets::Material::kOvDitherAlpha, "dither-alpha", "Dither Alpha"},
                {assets::Material::kOvShadingModel, "shading-model", "Shading Model"},
                {assets::Material::kOvDoubleSided, "double-sided", "Double Sided"},
                {assets::Material::kOvHeightScale, "height-scale", "Height Scale"},
            };
            ImGui::TextUnformatted(TR("Overrides:"));
            for (const auto& o : kOv) {
                if ((mat->overridden & o.bit) == 0) continue;
                ImGui::SameLine();
                const std::string id =
                    std::string("Revert##ov-") + o.name;
                if (ImGui::SmallButton(id.c_str())) {
                    const assets::Material::Params before = mat->captureParams();
                    assets::Material::Params after = before;
                    assets::Material::mergeParams(
                        after, mat->parent->captureParams(), o.bit);
                    mat->applyParams(after);
                    mat->overridden &= ~static_cast<std::uint32_t>(o.bit);
                    m_ctx.commands().pushAlreadyApplied(
                        std::make_unique<LambdaCommand>(
                            std::string("Material Revert ") + o.label,
                            [mat, before]() { mat->applyParams(before); },
                            [mat, after]() { mat->applyParams(after); }));
                    m_ctx.dirty() = true;
                }
                const std::string anchor =
                    std::string("INS-MAT-REVERT-") + o.name;
                logRect(anchor.c_str(), ImGui::GetItemRectMin(),
                        ImGui::GetItemRectMax());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        TR("Stop overriding %s and follow the parent again."),
                        TR(o.label));
            }
        }
    }

    // ---- 三列布局：属性名 | 值 | 纹理槽方块 ----
    //
    // 每帧在**第一行的行首**取一次 rowX0，它就是"第 1 列的左缘"。
    // 之后所有列都用绝对偏移：SameLine(offset) 的 offset 是**窗口坐标系**
    // 的横坐标（imgui.cpp:11708），所以必须基于 rowX0 加，直接写 88 会被
    // 窗口 padding 顶掉一截，而且行数一多还会各偏一点。
    //
    // 两列各自对齐 = 各行的"值"和"方块"落在同一条竖直线上。之前每行的方块
    // 都跟在标签屁股后面（标签长短不一），看下来参差不齐 —— 那正是"拥挤"的来源。
    //
    // ⚠⚠ 但**值列的宽度不能拍死**。88 是 E11.1 之前的口径（那时行首 ≈ 8px、
    //   Inspector 还没有左侧图标导航条）。图标条一加，面板内容列从 ~324 掉到
    //   ~251px：88(标签) + 10 + 88(值) + 10 + 槽位 → 槽位只剩 **48px**，
    //   `drawTextureChip` 的 `fitText` 于是把文件名裁成 ".png" —— 三个已绑定槽
    //   看起来一模一样（2026-10-16 全量回归的 `verify_material_roundtrip` 抓到的
    //   就是这个）。
    //
    //   分配原则：**标签列固定 88（标签必须读得全）**，让**值列**去吸收挤压
    //   （它信息密度最低，44px 仍放得下一个 float 拖拽框），槽位至少拿到 96px。
    //   面板够宽时（inner ≥ 292）值列照样顶到 88 → **与改造前逐像素一致**。
    const float rowX0 = ImGui::GetCursorPosX();
    const float rowRight = ImGui::GetWindowContentRegionMax().x;
    const float inner = std::max(rowRight - rowX0, 0.0f);
    const float kChipWant = 96.0f;    // 槽位下限：方框 28 + 两侧边距 + ~7 个字符
    const float kValueMinW = 44.0f;   // 一个 float 拖拽框的下限
    const float valueW = std::clamp(
        inner - kLabelWidth - kChipWant - kColGap * 2.0f,
        kValueMinW, kValueWidth);
    const float colValue = rowX0 + kLabelWidth + kColGap;
    const float colChip = colValue + valueW + kColGap;

    auto rowBegin = [&](const char* label) {
        // AlignTextToFramePadding：文字要跟右边的输入框在**同一条基线上**，
        // 否则标签会贴着行顶，看起来像没对齐。
        // ⚠ TR 放在这里而不是调用点：材质 / 光照页有几十个 `rowBegin("xxx")`，
        //   逐处包一遍既啰嗦又必然漏（这几个是**自定义辅助函数**，不在
        //   ImGui 调用里，扫描器也抓不到）。一处翻，全部受益。
        ImGui::AlignTextToFramePadding();
        // 被覆盖的字段用高亮标出 —— 这是"我改的是这一件还是全部"的
        // **唯一视觉线索**（W1-3）。用 TextColored 而不是 Push/PopStyleColor：
        // 后者要在每个控件之后再弹一次，几十处必然漏。
        const std::uint32_t bit = overriddenBitOf(label);
        const char* lbl = TR(label);
        if (mat->isInstance() && bit && (mat->overridden & bit))
            ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.35f, 1.0f), "%s", lbl);
        else
            ImGui::TextUnformatted(lbl);
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
    ImGui::SetNextItemWidth(valueW);
    ImGui::ColorEdit4("##base-color", glm::value_ptr(mat->baseColorFactor),
                      ImGuiColorEditFlags_NoInputs);
    fieldEdit(m_ctx, "Material Base Color", &mat->baseColorFactor, c0);
    chipAt("albedo", "Material Albedo", "Base colour map (sRGB)",
           &mat->albedoMap, a1, /*srgb=*/true);

    // ---- Metallic ----
    const float m0 = mat->metallic;
    rowBegin("Metallic");
    ImGui::SetNextItemWidth(valueW);
    ImGui::SliderFloat("##metallic", &mat->metallic, 0.0f, 1.0f, "%.2f");
    fieldEdit(m_ctx, "Material Metallic", &mat->metallic, m0);
    chipAt("metallic", "Material Metallic Map",
           "Metallic map (grayscale, linear). Multiplied by the slider on the left.",
           &mat->metallicMap, mt1, /*srgb=*/false);

    // ---- Roughness ----
    const float r0 = mat->roughness;
    rowBegin("Roughness");
    ImGui::SetNextItemWidth(valueW);
    ImGui::SliderFloat("##roughness", &mat->roughness, 0.02f, 1.0f, "%.2f");
    fieldEdit(m_ctx, "Material Roughness", &mat->roughness, r0);
    chipAt("roughness", "Material Roughness Map",
           "Roughness map (grayscale, linear). Multiplied by the slider on the left.",
           &mat->roughnessMap, rg1, /*srgb=*/false);

    // ---- Emissive ----
    const glm::vec3 e0 = mat->emissive;
    rowBegin("Emissive");
    ImGui::SetNextItemWidth(valueW);
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
    ImGui::SetNextItemWidth(valueW);
    ImGui::SliderFloat("##ao", &mat->ao, 0.0f, 1.0f, "%.2f");
    fieldEdit(m_ctx, "Material AO", &mat->ao, a0);
    chipAt("orm", "Material ORM",
           "ORM packed map: Occlusion(R) / Roughness(G) / Metallic(B), linear",
           &mat->ormMap, or1, /*srgb=*/false);

    const float n0 = mat->normalScale;
    rowBegin("Normal Scale");
    ImGui::SetNextItemWidth(valueW);
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
    ImGui::SetNextItemWidth(valueW);
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
    ImGui::SetNextItemWidth(valueW);
    if (ImGui::Combo("##alpha-mode", &mode, TrItems<3>(kAlpha), 3)) {
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
            TR("Opaque - alpha ignored, always depth-writes.\n"
            "Blend  - alpha-weighted, drawn back-to-front, no depth write.\n"
            "Mask   - binary cutout at the alpha cutoff (foliage / fences)."));
    }
    // ---- Mask（alpha 裁切）专属：阈值 + 抖动模式（R4-1）----
    // 只在 Mask 模式下出现 —— 其它模式这两个字段不参与着色器编译，显示
    // 出来只会让人以为"调了没反应"。
    if (mat->alphaMode == assets::Material::AlphaMode::Mask) {
        const float c0 = mat->alphaCutoff;
        rowBegin("Alpha Cutoff");
        ImGui::SetNextItemWidth(valueW);
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
                TR("Stochastic alpha test: keep the fragment with a Bayer-dither\n"
                "threshold instead of a fixed cutoff. Still writes depth (unlike\n"
                "Blend), and TAA turns the dithered edge into a soft one."));
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
        ImGui::SetNextItemWidth(valueW);
        if (ImGui::Combo("##shading-model", &sm, TrItems<2>(kShading), 2)) {
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
                TR("Default = metallic-roughness BRDF.\n\n"
                "Skin = pre-integrated subsurface scattering: the diffuse term\n"
                "looks up a baked (N.L, curvature) LUT instead of max(N.L,0),\n"
                "so light wraps around the terminator and the red channel bleeds\n"
                "furthest -> nose bridge / ear rims glow red. Default materials\n"
                "are compiled without this code path entirely (pixel-identical)."));
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
        ImGui::TextDisabled(TR("k%u"), kw);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                TR("%s\n\n"
                "Keyword bits (one-to-one with SHADER_KEYWORDS in CMakeLists.txt):\n"
                "  bit0  double-sided  VENN_DOUBLE_SIDED  disable back-face culling\n"
                "  bit1  alpha-blend   VENN_ALPHA_BLEND   blended, no depth write\n"
                "  bit2  normal-map    VENN_NORMAL_MAP    sample the normal map + TBN\n"
                "  bit3  alpha-mask    VENN_ALPHA_MASK    alpha cutout (discard)\n"
                "  bit4  shading-skin  VENN_SHADING_SKIN pre-integrated skin scattering LUT\n"
                "\nWith no normal map bound bit2 is 0: that sampling and TBN block is compiled out entirely, "
                "replaced by a built-in flat normal - pixel-identical.\n"
                "\nIf MYVK_SHADER_FORCE_MASK is set, the actual variant is force-overridden, "
                "and the Stats panel marks it FORCED."),
                render::variantShaderFile("pbr.frag", kw).c_str());
        }
        ImGui::TextDisabled(TR("  %s"), render::keywordNames(kw).c_str());
    }

    // ---- 本帧的编辑 → 覆盖位 / 母材质传播（W1-2 / W1-3）----
    // ⚠ 必须在**所有控件之后**：ImGui 在被调用的那一刻就把新值写进 `mat` 了，
    //   所以一次 diff 能抓全 12 个字段，不用在每处 fieldEdit 后面加一行。
    const std::uint32_t touched =
        assets::Material::diffParams(paramsAtStart, mat->captureParams());
    if (touched) {
        if (mat->isInstance()) {
            // 实例：本地改过 = 覆盖，之后母材质再变也不跟这一位
            mat->overridden |= touched;
        } else {
            // 母材质：把未覆盖该字段的实例全刷一遍
            m_ctx.assets().materialChanged(mat);
        }
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
    const bool typeChanged = ImGui::Combo(TR("Type"), &type, TrItems<3>(kTypes), 3);
    probe("Type");
    ImGui::EndDisabled();
    if (isKeyLight) {
        hint("scene key light - its type is fixed (see scene::Scene::light)");
    } else if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("switching the type keeps colour / intensity / direction"));
    }

    if (typeChanged && type != typeNow && m_ctx.isEditing()) {
        m_ctx.structuralEdit("Change Light Type", [&] { convertLightType(w, e, type); });
    }

    // ---- Directional ----
    if (dl) {
        ImGui::SeparatorText(TR("Directional"));

        const glm::vec3 d0 = dl->direction;
        ImGui::DragFloat3(TRL("Direction"), glm::value_ptr(dl->direction), sp(0.01f), -1.0f, 1.0f);
        probe("Direction");
        fieldEdit(m_ctx, "Sun Direction", &dl->direction, d0);

        const glm::vec3 c0 = dl->color;
        ImGui::ColorEdit3(TRL("Color"), glm::value_ptr(dl->color));
        probe("Color");
        fieldEdit(m_ctx, "Sun Color", &dl->color, c0);

        const float i0 = dl->intensity;
        ImGui::SliderFloat(TRL("Intensity"), &dl->intensity, 0.0f, 4.0f);
        probe("Intensity");
        fieldEdit(m_ctx, "Sun Intensity", &dl->intensity, i0);

        const float a0 = dl->ambientScale;
        ImGui::SliderFloat(TRL("Ambient"), &dl->ambientScale, 0.0f, 1.0f, "%.2f");
        probe("Ambient");
        fieldEdit(m_ctx, "Sun Ambient Scale", &dl->ambientScale, a0);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(TR("procedural IBL strength\nindoor scenes: 0.1 - 0.3"));

        bool cs = dl->castsShadow;
        if (ImGui::Checkbox(TR("Casts Shadow"), &cs)) {
            dl->castsShadow = cs;
            discreteEdit(m_ctx, "Sun Casts Shadow", &dl->castsShadow, !cs);
        }
        labels = "Direction,Color,Intensity,Ambient,CastsShadow";
        hint("no range / cone: a sun is parallel light from infinitely far away");
    }

    // ---- Point ----
    if (pl) {
        ImGui::SeparatorText(TR("Point"));

        const glm::vec3 c0 = pl->color;
        ImGui::ColorEdit3(TRL("Color"), glm::value_ptr(pl->color));
        probe("Color");
        fieldEdit(m_ctx, "Point Light Color", &pl->color, c0);

        const float i0 = pl->intensity;
        ImGui::DragFloat(TRL("Intensity"), &pl->intensity, sp(0.2f), 0.0f, 500.0f, "%.1f");
        probe("Intensity");
        fieldEdit(m_ctx, "Point Light Intensity", &pl->intensity, i0);

        const float r0 = pl->range;
        ImGui::DragFloat(TRL("Range"), &pl->range, sp(0.05f), 0.1f, 60.0f, "%.2f");
        probe("Range");
        fieldEdit(m_ctx, "Point Light Range", &pl->range, r0);
        labels = "Color,Intensity,Range,Enabled";
        hint("range drives cluster culling - keep it tight");

        bool en = pl->enabled;
        if (ImGui::Checkbox(TR("Enabled"), &en)) {
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
        ImGui::SeparatorText(TR("Spot"));

        const glm::vec3 c0 = sl->color;
        ImGui::ColorEdit3(TRL("Color"), glm::value_ptr(sl->color));
        probe("Color");
        fieldEdit(m_ctx, "Spot Light Color", &sl->color, c0);

        const float i0 = sl->intensity;
        ImGui::DragFloat(TRL("Intensity"), &sl->intensity, sp(0.4f), 0.0f, 800.0f, "%.1f");
        probe("Intensity");
        fieldEdit(m_ctx, "Spot Light Intensity", &sl->intensity, i0);

        const float r0 = sl->range;
        ImGui::DragFloat(TRL("Range"), &sl->range, sp(0.05f), 0.1f, 80.0f, "%.2f");
        probe("Range");
        fieldEdit(m_ctx, "Spot Light Range", &sl->range, r0);

        const glm::vec3 d0 = sl->direction;
        ImGui::DragFloat3(TRL("Direction"), glm::value_ptr(sl->direction), sp(0.01f), -1.0f, 1.0f);
        probe("Direction");
        fieldEdit(m_ctx, "Spot Light Direction", &sl->direction, d0);

        float innerDeg = glm::degrees(sl->innerAngle);
        float outerDeg = glm::degrees(sl->outerAngle);
        const float in0 = sl->innerAngle;
        const float out0 = sl->outerAngle;
        if (ImGui::SliderFloat(TRL("Inner Cone"), &innerDeg, 1.0f, 80.0f, TR("%.1f deg"))) {
            sl->innerAngle = glm::radians(innerDeg);
        }
        fieldEdit(m_ctx, "Spot Inner Cone", &sl->innerAngle, in0);
        if (ImGui::SliderFloat(TRL("Outer Cone"), &outerDeg, 1.0f, 89.0f, TR("%.1f deg"))) {
            sl->outerAngle = glm::radians(outerDeg);
        }
        fieldEdit(m_ctx, "Spot Outer Cone", &sl->outerAngle, out0);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(TR("penumbra = outer - inner\nnarrow the gap for a harder edge"));

        // ---- 光源修饰（R3-3）：区域光尺寸 / IES / light function ----
        // 三者都共享灯的局部正交基，所以只有射灯有这几项。真正的总开关
        // 在渲染器（MYVK_AREA_LIGHT / MYVK_IES / MYVK_COOKIE）—— 这里只
        // 写"这盏灯想用什么"。
        ImGui::SeparatorText(TR("Modifiers"));
        {
            const float aw0 = sl->areaWidth;
            ImGui::DragFloat(TRL("Area Width"), &sl->areaWidth, sp(0.05f), 0.0f, 40.0f,
                             "%.2f");
            probe("AreaWidth");
            fieldEdit(m_ctx, "Spot Area Width", &sl->areaWidth, aw0);
            const float ah0 = sl->areaHeight;
            ImGui::DragFloat(TRL("Area Height"), &sl->areaHeight, sp(0.05f), 0.0f, 40.0f,
                             "%.2f");
            probe("AreaHeight");
            fieldEdit(m_ctx, "Spot Area Height", &sl->areaHeight, ah0);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    TR("rect area light size in metres (0 = point-like)\n"
                    "softens the highlight; needs MYVK_AREA_LIGHT=1"));

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
                ImGui::SetTooltip(TR("IES photometric web\nneeds MYVK_IES=1"));
            nameCombo("Light Function", sl->lightFunction, kCookieItems, 4);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(TR("projected cookie texture\nneeds MYVK_COOKIE=1"));
        }

        labels = "Color,Intensity,Range,Direction,InnerCone,OuterCone,"
                 "Area,IES,Cookie,Enabled";

        bool en = sl->enabled;
        if (ImGui::Checkbox(TR("Enabled"), &en)) {
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
    if (ImGui::Checkbox(TRL("Enabled##script"), &en)) {
        scomp->enabled = en;
        discreteEdit(m_ctx, "Toggle Script", &scomp->enabled, !en);
    }
    ImGui::SameLine();
    const float ts0 = scomp->timeScale;
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderFloat(TRL("Time Scale"), &scomp->timeScale, 0.0f, 4.0f, "%.2f");
    fieldEdit(m_ctx, "Script Time Scale", &scomp->timeScale, ts0);

    // ★ S5：这个实体的脚本实例状态（Idle / Running / Waiting / Failed）。
    //   面板上是"一列"，这里是"选中这一个" —— 排查"这个怪怎么不动"
    //   时不用去右边那张表里找。
    if (vks::ScriptRuntime* rt = m_ctx.scriptRuntime()) {
        const vks::ScriptInstance* inst = rt->find(e);
        if (inst) {
            const char* st = rt->stateOf(*inst);
            const ImVec4 col =
                inst->failed
                    ? ImVec4(1.0f, 0.45f, 0.42f, 1.0f)
                    : inst->waitingNow ? ImVec4(1.0f, 0.85f, 0.45f, 1.0f)
                                       : ImVec4(0.62f, 0.88f, 0.62f, 1.0f);
            ImGui::TextColored(col, "%s", st);
            logRect("INS-SCRIPT-STATE", ImGui::GetItemRectMin(),
                    ImGui::GetItemRectMax());
            ImGui::SameLine();
            ImGui::TextDisabled(TR("t=%.2fs  vars=%zu  runs=%u"),
                                static_cast<double>(inst->localTime),
                                inst->vars.size(), inst->runCount);
            if (inst->failed && !inst->lastError.empty()) {
                ImGui::TextWrapped("%s", inst->lastError.c_str());
            }
            if (ImGui::SmallButton(TR("Reset Script State##insp"))) {
                rt->resetInstance(e);
                m_ctx.notify(std::string(TR("Reset script on ")) +
                             (w.name(e) ? *w.name(e) : std::string("?")));
            }
            logRect("INS-SCRIPT-RESET", ImGui::GetItemRectMin(),
                    ImGui::GetItemRectMax());
        } else {
            ImGui::TextDisabled(TR("no instance (not synced yet)"));
        }
    }

    // 脚本只在运行态求值 —— 这条提示很重要，否则用户会以为坏了
    if (m_ctx.isEditing()) {
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.3f, 1.0f),
                           TR("Scripts only run in Play mode."));
    }

    if (removeButton() && m_ctx.isEditing()) {
        m_ctx.structuralEdit("Remove Script",
                             [&]() { w.remove<ecs::ScriptComponent>(e); });
        m_scriptBufFor = ecs::Entity{};
    }
}

// ---------------------------------------------------------------- 音频源（G14-7）

// 放在 **Tool** 页签而不是新开一个页签：
// 加页签要同时改 `kTabs[]` 的顺序（枚举值 == 图标栏顺序）、图标、
// 落盘 id 和两处 `static_assert` —— 而音频的属性只有 9 个字段，
// 语义上本来就属于"这个物体挂了什么行为"（与 Script 同一格）。
void InspectorPanel::drawAudioSource(ecs::Entity e) {
    scene::Scene& sc = m_ctx.activeScene();
    ecs::World& w = sc.world();
    auto* a = w.get<ecs::AudioSourceComponent>(e);
    if (!a) return;

    if (!sectionHeader("Audio Source", true, e, "Audio Source")) return;
    // 锚点：编辑器里"音频段真的画出来了"的正面证据（verify_audio.py 用）
    logRect("INS-AUDIO-HEADER", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());

    if (m_audioBufFor != e) {
        std::snprintf(m_audioBuf, sizeof(m_audioBuf), "%s", a->clip.c_str());
        m_audioBufFor = e;
    }

    ImGui::SetNextItemWidth(-96.0f);
    ImGui::InputTextWithHint("##audioclip", "audio/click.wav", m_audioBuf,
                             sizeof(m_audioBuf));
    if (ImGui::IsItemDeactivatedAfterEdit() && m_ctx.isEditing()) {
        const std::string np = m_audioBuf;
        if (np != a->clip) {
            m_ctx.structuralEdit("Edit Audio Clip",
                                 [&]() { a->clip = np; });
        }
    }
    ImGui::SameLine();
    // ▶ 试听：**不进撤销栈**（它不改变任何状态，只是"放一声"）。
    // ⚠ 走 `requestAudioPreview` 而不是自己拿 AudioSystem：面板不该持有
    //   引擎对象（否则 Inspector 就成了第二个 Assets 面板）。
    const bool canPreview = !a->clip.empty();
    ImGui::BeginDisabled(!canPreview);
    if (ImGui::Button(TRL("Preview"))) {
        m_ctx.requestAudioPreview(a->clip, a->volume);
    }
    ImGui::EndDisabled();
    logRect("INS-AUDIO-PREVIEW", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered() && canPreview) {
        ImGui::SetTooltip("%s", a->clip.c_str());
    }

    ImGui::BeginDisabled(!m_ctx.isEditing());

    bool awake = a->playOnAwake;
    if (ImGui::Checkbox(TRL("Play On Awake"), &awake))
        discreteEdit(m_ctx, "Audio Play On Awake", &a->playOnAwake, !awake);
    ImGui::SameLine();
    bool loop = a->loop;
    if (ImGui::Checkbox(TRL("Loop"), &loop))
        discreteEdit(m_ctx, "Audio Loop", &a->loop, !loop);
    ImGui::SameLine();
    bool spatial = a->spatial;
    if (ImGui::Checkbox(TRL("3D"), &spatial))
        discreteEdit(m_ctx, "Audio Spatial", &a->spatial, !spatial);
    ImGui::SetItemTooltip(
        TRL("3D: gain falls off with distance and the sound is panned by "
            "direction.\nOff: constant volume, centred (UI sounds)."));

    const float v0 = a->volume;
    ImGui::SetNextItemWidth(-1.0f);
    // ⚠ 标签刻意写成 "Clip Volume" 而不是 "Volume"：词典里 "Volume" 已经
    //   是粒子模块的**体积**。一词两义只能靠"让 UI 原文更具体"来区分 ——
    //   共用一个键会让其中一个永远翻错（i18n 纪律）。
    ImGui::SliderFloat(TRL("Clip Volume"), &a->volume, 0.0f, 1.0f, "%.2f");
    fieldEdit(m_ctx, "Audio Volume", &a->volume, v0);

    const float p0 = a->pitch;
    ImGui::SetNextItemWidth(-1.0f);
    // 同上：词典里的 "Pitch" 是相机的**俯仰**。
    ImGui::SliderFloat(TRL("Clip Pitch"), &a->pitch, 0.25f, 4.0f, "%.2f");
    fieldEdit(m_ctx, "Audio Pitch", &a->pitch, p0);

    // ---- 3D 距离参数（只有 spatial 时才画：关着的时候它们完全没用）----
    if (a->spatial) {
        const float mn0 = a->minDist;
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat(TRL("Min Distance"), &a->minDist, 0.1f, 50.0f,
                           TR("%.2f m"));
        fieldEdit(m_ctx, "Audio Min Distance", &a->minDist, mn0);

        const float mx0 = a->maxDist;
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat(TRL("Max Distance"), &a->maxDist, 1.0f, 500.0f,
                           TR("%.1f m"));
        fieldEdit(m_ctx, "Audio Max Distance", &a->maxDist, mx0);

        const float ro0 = a->rolloff;
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat(TRL("Rolloff"), &a->rolloff, 0.0f, 4.0f, "%.2f");
        fieldEdit(m_ctx, "Audio Rolloff", &a->rolloff, ro0);

        // ⚠ 一条**真的会无声**的配置必须当场说出来，否则用户只会觉得"坏了"
        if (a->maxDist <= a->minDist) {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
                               TR("Max Distance must be greater than Min "
                                  "Distance - this sound is silent."));
        }
    }

    // ---- 总线 ----
    static const char* kBusLabels[] = {"Music", "SFX", "Voice"};
    int busIdx = a->bus - 1;
    if (busIdx < 0 || busIdx > 2) busIdx = 1;
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo(TRL("Bus"), &busIdx, kBusLabels, 3)) {
        const int newBus = busIdx + 1;
        const int oldBus = a->bus;
        a->bus = newBus;
        // ⚠ 撤销栈里存的是**枚举值**（1/2/3），不是下拉框下标 ——
        //   下标会随标签顺序变，恢复出来的就是另一条总线了。
        discreteEdit(m_ctx, "Audio Bus", &a->bus, oldBus);
    }
    ImGui::SetItemTooltip(
        TRL("Voice = dialogue. While any Voice clip plays, the Music bus is "
            "ducked automatically."));

    bool streaming = a->streaming;
    if (ImGui::Checkbox(TRL("Streaming"), &streaming))
        discreteEdit(m_ctx, "Audio Streaming", &a->streaming, !streaming);
    ImGui::SetItemTooltip(
        TRL("Stream from disk with a 3 s ring buffer. Use it for long music\n"
            "tracks; short effects should stay in memory."));

    ImGui::EndDisabled();

    if (removeButton() && m_ctx.isEditing()) {
        m_ctx.structuralEdit("Remove Audio Source", [&]() {
            m_ctx.editorScene().world().remove<ecs::AudioSourceComponent>(e);
        });
        m_audioBufFor = ecs::Entity{};
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
    if (ImGui::Button(TR("Add Component"))) ImGui::OpenPopup("add_component");
    // 锚点：Tool 页签的正面证据（它只有这一个"显式控件"是新写的）。
    logRect("INS-ADD-COMPONENT", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    if (ImGui::BeginPopup("add_component")) {
        const bool hasPoint = w.has<ecs::PointLightComponent>(e);
        const bool hasSpot = w.has<ecs::SpotLightComponent>(e);
        const bool hasScript = w.has<ecs::ScriptComponent>(e);
        const bool hasDecal = w.has<ecs::DecalComponent>(e);

        if (ImGui::MenuItem(TR("Point Light"), nullptr, false, !hasPoint)) {
            m_ctx.structuralEdit("Add Point Light", [&]() {
                ecs::PointLightComponent pl;
                w.add<ecs::PointLightComponent>(e, pl);
            });
        }
        if (ImGui::MenuItem(TR("Spot Light"), nullptr, false, !hasSpot)) {
            m_ctx.structuralEdit("Add Spot Light", [&]() {
                ecs::SpotLightComponent sl;
                w.add<ecs::SpotLightComponent>(e, sl);
            });
        }
        if (ImGui::MenuItem(TR("Script"), nullptr, false, !hasScript)) {
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
        // 动画器（A1）。只有**已经挂了蒙皮组件**的实体才配得上它 ——
        // 没有骨架的实体挂一个动画器，界面上会全是"没东西可播"的空控件。
        // 所以菜单项对普通物体直接灰掉（ImGui 的 enable 参数），而不是
        // 加完再报错。
        const bool hasAnimator = w.has<ecs::AnimatorComponent>(e);
        const bool hasSkinned = w.has<ecs::SkinnedMeshComponent>(e);
        if (ImGui::MenuItem(TR("Animator"), nullptr, false,
                            hasSkinned && !hasAnimator)) {
            m_ctx.structuralEdit("Add Animator", [&]() {
                ecs::AnimatorComponent a;
                if (const auto* sk0 = w.get<ecs::SkinnedMeshComponent>(e)) {
                    // 从"当前正在播的那一段"接手，加完画面不变样
                    a.defaultClip = sk0->clipName;
                    a.time = sk0->time;
                    a.speed = sk0->speed;
                    a.playing = sk0->playing;
                    a.loop = sk0->loop;
                }
                w.add<ecs::AnimatorComponent>(e, a);
            });
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("Only for entities that already have a "
                                 "Skinned Mesh (the model root)."));
        }

        // 延迟贴花（R4-4）：默认 1 m 立方体、不透明度 1、白色 tint，
        // 两张贴图都留空 —— 于是它一加出来就是个"能看见的纯色方块"，
        // 把贴图拖进 Inspector 的 Decal 段才算真正有内容。
        // 位置/朝向直接吃实体的 Transform，不需要额外初始化。
        if (ImGui::MenuItem(TR("Decal"), nullptr, false, !hasDecal)) {
            m_ctx.structuralEdit("Add Decal", [&]() {
                ecs::DecalComponent d;
                w.add<ecs::DecalComponent>(e, d);
            });
            m_lastDecalParamSig.clear();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                TR("A box-volume decal projected onto whatever surface is inside "
                "it.\nPlace it with the same W/E/R gizmo as the object itself."));
        }

        // 反射探针（R5-1）：默认 10×10×5 的盒子、强度 1、开盒投影。
        // 一加出来就"能烘"—— 但全局开关（MYVK_PROBES）默认关，所以
        // 真正看到反射还要在 Engine 面板里打开 probe.enabled。
        const bool hasProbe = w.has<ecs::ReflectionProbeComponent>(e);
        if (ImGui::MenuItem(TR("Reflection Probe"), nullptr, false, !hasProbe)) {
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
                TR("Captures the surroundings into a cubemap and reflects it on\n"
                "everything inside the box - fills in what SSR cannot see\n"
                "(off-screen geometry). Needs MYVK_PROBES=1."));
        }

        // GPU 粒子发射器（R6-1）：默认是"一加出来就能看见的火焰"——
        // 加色混合 + 暖色三段渐变 + 向上初速。没有网格（几何是顶点着
        // 色器现拼的 billboard），位置 / 朝向吃实体的 Transform：
        // **局部 +Z 就是喷射方向**。
        const bool hasFx = w.has<ecs::ParticleEmitterComponent>(e);
        if (ImGui::MenuItem(TR("Particle Emitter"), nullptr, false, !hasFx)) {
            m_ctx.structuralEdit("Add Particle Emitter", [&]() {
                ecs::ParticleEmitterComponent fx;
                w.add<ecs::ParticleEmitterComponent>(e, fx);
            });
            m_lastFxParamSig.clear();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                TR("Spawns particles from this object's origin along its local\n"
                "+Z. Simulated entirely on the GPU (compute) and drawn with\n"
                "one indirect draw - CPU never counts them.\n"
                "Needs the global switch MYVK_FX=1."));
        }

        // 音频源（G14-7）：默认 2D + 循环关 + 音量 1 —— 一加出来是"点开
        // 自己填个 clip 就能响"的状态，不预设任何素材路径。
        const bool hasAudio = w.has<ecs::AudioSourceComponent>(e);
        if (ImGui::MenuItem(TR("Audio Source"), nullptr, false, !hasAudio)) {
            m_ctx.structuralEdit("Add Audio Source", [&]() {
                ecs::AudioSourceComponent a;
                w.add<ecs::AudioSourceComponent>(e, a);
            });
            m_audioBufFor = ecs::Entity{};
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                TR("Plays a wav/ogg clip from this object's position.\n"
                "Turn 3D on to make it fall off with distance and pan\n"
                "by direction."));
        }

        // 听者（G14-7）：挂在相机实体上可以覆盖"听者位置"（耳朵在角色
        // 头上、相机在身后 3 m 的第三人称混音）。
        const bool hasListener = w.has<ecs::AudioListenerComponent>(e);
        if (ImGui::MenuItem(TR("Audio Listener"), nullptr, false, !hasListener)) {
            m_ctx.structuralEdit("Add Audio Listener", [&]() {
                ecs::AudioListenerComponent al;
                w.add<ecs::AudioListenerComponent>(e, al);
            });
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                TR("Overrides where the listener (ears) sits. Only the\n"
                "position is taken from here - orientation always comes\n"
                "from the active camera."));
        }

        // 刚体（R7-1）：默认 Dynamic + Auto 形状 —— 一加出来"按下 Play 就会
        // 掉下去"，这就是能马上看见正确性的默认值。
        // ⚠ 它和 CollisionComponent 是**两件事**：后者只是"有个碰撞体"
        //   （编辑器预览 + 无动力学的位置分离），前者才受物理支配。
        const bool hasRigid = w.has<ecs::RigidBodyComponent>(e);
        if (ImGui::MenuItem(TR("Rigid Body"), nullptr, false, !hasRigid))
            addRigidBodyTo(e);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                TR("Makes this object part of the physics simulation (Play mode\n"
                "only). Shape defaults to Auto: the collision component if\n"
                "present, otherwise the mesh bounds. See the Physics tab."));
        }

        // ---- M2（G07~G09）：玩法组件 ----
        //
        // 这四个都是"Play 态才动"的组件，所以添加之后**当场看不出变化**
        // （角色要按 WASD、触发体要有人撞进去、相机要有人接管它）。
        // tooltip 里一律写清"证据在哪"，否则用户会以为按钮坏了 —— 这与
        // 粒子发射器当年那条教训是同一个。
        ImGui::Separator();

        const bool hasChar = w.has<ecs::CharacterControllerComponent>(e);
        if (ImGui::MenuItem(TR("Character Controller"), nullptr, false,
                            !hasChar)) {
            m_ctx.structuralEdit("Add Character Controller", [&]() {
                w.add<ecs::CharacterControllerComponent>(
                    e, ecs::CharacterControllerComponent{});
            });
            m_ctx.notify(TR("Character controller added — press Play to walk"));
            VK_LOG_INFO("INS-PHYS-CHAR-ADD entity=%u", e.id);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                TR("Turns this object into a walkable character (capsule,\n"
                "step-up, slopes, jump). Steps are kinematic: the physics\n"
                "solver never moves it, so it cannot be pushed around.\n"
                "Mutually exclusive with Rigid Body. Press Play, then WASD."));
        }

        const bool hasTrg = w.has<ecs::TriggerComponent>(e);
        if (ImGui::MenuItem(TR("Trigger"), nullptr, false, !hasTrg)) {
            m_ctx.structuralEdit("Add Trigger", [&]() {
                w.add<ecs::TriggerComponent>(e, ecs::TriggerComponent{});
            });
            m_ctx.notify(TR("Trigger added — it reports Enter / Stay / Exit"));
            VK_LOG_INFO("INS-PHYS-TRIGGER-ADD entity=%u", e.id);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                TR("Makes this object's collider a trigger volume: things pass\n"
                "through it, but you get Enter / Stay / Exit events.\n"
                "Needs a Collision component (its shape is the volume)."));
        }

        const bool hasMat = w.has<ecs::PhysicsMaterialComponent>(e);
        if (ImGui::MenuItem(TR("Physics Material"), nullptr, false, !hasMat)) {
            m_ctx.structuralEdit("Add Physics Material", [&]() {
                w.add<ecs::PhysicsMaterialComponent>(
                    e, ecs::PhysicsMaterialComponent{});
            });
            m_ctx.notify(TR("Physics material override added"));
            VK_LOG_INFO("INS-PHYS-MAT-ADD entity=%u", e.id);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                TR("Overrides the friction / restitution / density that\n"
                "otherwise come from the Rigid Body. Without this component\n"
                "the body behaves exactly as before."));
        }

        const bool hasCam = w.has<ecs::CameraComponent>(e);
        if (ImGui::MenuItem(TR("Camera"), nullptr, false, !hasCam)) {
            m_ctx.structuralEdit("Add Camera", [&]() {
                w.add<ecs::CameraComponent>(e, ecs::CameraComponent{});
            });
            m_ctx.notify(TR("Camera added — highest priority wins"));
            VK_LOG_INFO("INS-CAM-ADD entity=%u", e.id);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                TR("Registers this object as a camera. The enabled camera with\n"
                "the highest priority is the one rendered.\n"
                "Put it on the character for a third-person follow camera;\n"
                "put it on its own object for a fixed shot."));
        }

        ImGui::EndPopup();
    }
}

} // namespace editor
