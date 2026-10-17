#include "ParticlesPanel.h"

#include "DebugRects.h"
#include "Localization.h"
#include "PanelRegistry.h"

#include "core/Logger.h"

#include "ecs/Components.h"
#include "render/FeatureRegistry.h"
#include "render/Renderer.h"
#include "scene/Scene.h"

#include <imgui.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace editor {

namespace {

constexpr float kLabelW = 140.0f;

// 预设：六个"开箱即用"的粒子。
//
// 数值不是随手填的 —— 每一个都能在 Unity / Blender 的对应模板里找到出处：
//   火焰 = 向上浮力 + 短寿命 + 加色；烟 = 长寿命 + 慢速 + alpha；
//   火花 = 高速 + 短寿命 + 沿速度拉伸。
//
// 2026-10-17 第四轮新增雨 / 雪 / 闪电（用户原话："粒子特效的类型，可以再多
// 加一些嘛，比如从上往下落的，类似，下雪，下雨，闪电之类的，并一一实现其
// 效果"）。这三种都靠**同一个机制**做到"从上往下落"：
//
//   ⚠ 发射方向永远是**局部 +Z 锥**（见 particle_sim.comp 的 fxSampleDir）。
//     想让它朝下，就把发射器整体绕 X 转 180°（`flipDown`）—— 世界空间下
//     `em.world` 会把这个轴翻过来。不要去改 startSpeed 的符号：那是标量
//     速度，负值只会让 fxMix 里的方向计算变成"反向但仍是锥轴"，且与
//     speedVariance 相乘时符号会抖。
struct FxPreset {
    const char* label;
    float rate;
    float lifetime;
    float lifetimeVar;
    float startSpeed;
    float spread;
    float drag;
    glm::vec3 gravity;
    float startSize;
    float emissive;
    ecs::ParticleBlend blend;
    ecs::ParticleRenderMode render;
    float stretchScale;
    // ---- 雨 / 雪 / 闪电这一批要用的 ----
    bool flipDown;                       // 发射器绕 X 转 180°（往下落）
    ecs::ParticleShape shape;
    glm::vec3 shapeExtents;              // Box 的半轴
    float shapeRadius;
    bool noise;                          // 湍流（雪的飘）
    float noiseStrength;
    float noiseFreq;
    glm::vec3 colorA;                    // 渐变起点
    glm::vec3 colorB;                    // 渐变终点
    glm::vec4 alphaOverLife;
    glm::vec4 sizeOverLife;
    int burstCount;                      // >0 → 不用 rate，用循环 burst（闪电）
    float duration;                      // >0 且 looping → 每 duration 秒重来一轮
    // 出生高度（米）。火 / 烟 / 火花要贴着地面，取 0；雨 / 雪 / 闪电是
    // "从上往下"的，必须**抬起来**再落 —— 放 z=0 的话第一帧就在地面以下，
    // 直接被地面挡住，看起来就是"预设点了没效果"（实测踩过）。
    float originZ;
};

const FxPreset kPresets[] = {
    // ---------------- 火 ----------------
    // 热空气往上走 → 重力取正值（Z-up，+Z 是上）
    {"Fire", 90.0f, 1.4f, 0.25f, 1.6f, 18.0f, 0.55f, glm::vec3(0.0f, 0.0f, 1.8f),
     0.34f, 3.6f, ecs::ParticleBlend::Additive,
     ecs::ParticleRenderMode::Billboard, 0.0f,
     false, ecs::ParticleShape::Cone, glm::vec3(0.3f), 0.18f,
     false, 1.0f, 0.5f,
     glm::vec3(1.00f, 0.75f, 0.25f), glm::vec3(0.25f, 0.04f, 0.02f),
     glm::vec4(0.25f, 1.0f, 0.7f, 0.0f), glm::vec4(0.35f, 1.0f, 0.75f, 0.0f),
     0, 0.0f, 0.0f},

    // ---------------- 烟 ----------------
    // 慢、久、飘，普通 alpha 混合（加色会把烟变成一团发光的雾）
    {"Smoke", 40.0f, 3.6f, 0.3f, 0.7f, 30.0f, 0.85f, glm::vec3(0.0f, 0.0f, 0.45f),
     0.95f, 0.6f, ecs::ParticleBlend::Alpha,
     ecs::ParticleRenderMode::Billboard, 0.0f,
     false, ecs::ParticleShape::Cone, glm::vec3(0.4f), 0.25f,
     false, 1.0f, 0.5f,
     glm::vec3(0.55f, 0.55f, 0.58f), glm::vec3(0.22f, 0.22f, 0.24f),
     glm::vec4(0.0f, 0.45f, 0.30f, 0.0f), glm::vec4(0.5f, 1.0f, 1.6f, 1.0f),
     0, 0.0f, 0.0f},

    // ---------------- 火花 ----------------
    // 真重力 + 高速 + 沿速度拉丝（不拉伸的话每一颗只是个圆点）
    {"Sparks", 220.0f, 0.55f, 0.4f, 6.5f, 42.0f, 0.1f,
     glm::vec3(0.0f, 0.0f, -9.8f), 0.055f, 4.5f, ecs::ParticleBlend::Additive,
     ecs::ParticleRenderMode::StretchedBillboard, 0.12f,
     false, ecs::ParticleShape::Point, glm::vec3(0.1f), 0.05f,
     false, 1.0f, 0.5f,
     glm::vec3(1.0f, 0.85f, 0.45f), glm::vec3(1.0f, 0.35f, 0.05f),
     glm::vec4(1.0f, 1.0f, 0.6f, 0.0f), glm::vec4(1.0f, 0.8f, 0.4f, 0.0f),
     0, 0.0f, 0.0f},

    // ---------------- 雨 ----------------
    // 一片 12×12m 的"雨云"（Box 半轴 6/6/0.2），朝下的窄锥 + 强初速 + 真重力。
    // 拉伸系数 0.05 让它变成"雨丝"而不是"圆点"；寿命 1.1s × 9.8m/s ≈ 落下
    // 十来米，正好穿过整个视口。
    {"Rain", 620.0f, 1.1f, 0.15f, 9.0f, 2.5f, 0.02f,
     glm::vec3(0.0f, 0.0f, -9.8f), 0.025f, 1.2f, ecs::ParticleBlend::Alpha,
     ecs::ParticleRenderMode::StretchedBillboard, 0.05f,
     true, ecs::ParticleShape::Box, glm::vec3(6.0f, 6.0f, 0.2f), 0.5f,
     false, 1.0f, 0.5f,
     glm::vec3(0.62f, 0.72f, 0.86f), glm::vec3(0.78f, 0.86f, 0.98f),
     glm::vec4(0.0f, 0.5f, 0.45f, 0.0f), glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
     0, 0.0f, 8.0f},

    // ---------------- 雪 ----------------
    // ⚠ shapeExtents 是**半轴**：雨用 (6,6,0.2) 是一片薄板（雨落得快，一片
    //   板一秒就填满视野）；雪落得慢，必须给**一整块空气** (7,7,4.0)，否则
    //   前几秒整片雪还在镜头上面没落下来，看着像"点了没效果"。
    // 极慢的下落（0.35 m/s² 的"轻重力" + 大阻力），长寿命 + 湍流 → 打着旋飘。
    // 尺寸比雨大一个量级、不拉伸（雪片是点状的，不是丝状的）。
    {"Snow", 320.0f, 9.0f, 0.3f, 0.9f, 32.0f, 0.55f,
     glm::vec3(0.0f, 0.0f, -0.5f), 0.09f, 0.9f, ecs::ParticleBlend::Alpha,
     ecs::ParticleRenderMode::Billboard, 0.0f,
     true, ecs::ParticleShape::Box, glm::vec3(7.0f, 7.0f, 4.0f), 0.5f,
     true, 0.55f, 0.65f,
     glm::vec3(0.95f, 0.97f, 1.00f), glm::vec3(0.88f, 0.93f, 1.00f),
     glm::vec4(0.0f, 0.9f, 0.9f, 0.0f), glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
     0, 0.0f, 4.0f},

    // ---------------- 闪电 ----------------
    // 一次性爆发 + 极短寿命 + 加色 = "闪一下"。用 duration + looping 让整轮
    // 每 2.4 秒重来（⚠ 不要指望 bursts[].cycles 做循环：shader 里 `c < 4`
    // 把它硬夹到 4 次，见 particle_sim.comp 的 Bursts 段）。
    //
    // ⚠ 但**光靠 burst 不行**：0.22s 的闪光 + 2.4s 的周期 = 90% 的时间屏幕上
    //   什么都没有，用户点完预设大概率只看到空场景，以为坏了（实测就是这
    //   样）。所以再叠一路 rate=26 的常驻电晕：平时是噼啪的电弧，每 2.4 秒
    //   来一发 90 颗的大闪。两路共用同一个发射器，GPU 侧只是多算几十颗。
    {"Lightning", 200.0f, 0.40f, 0.35f, 3.2f, 80.0f, 2.2f,
     glm::vec3(0.0f), 0.16f, 8.0f, ecs::ParticleBlend::Additive,
     ecs::ParticleRenderMode::StretchedBillboard, 0.03f,
     false, ecs::ParticleShape::Point, glm::vec3(0.1f), 0.05f,
     false, 1.0f, 0.5f,
     glm::vec3(0.86f, 0.93f, 1.00f), glm::vec3(0.55f, 0.70f, 1.00f),
     glm::vec4(1.0f, 1.0f, 0.8f, 0.0f), glm::vec4(1.0f, 1.0f, 0.7f, 0.0f),
     160, 2.0f, 3.0f},
};

constexpr int kPresetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));

// `base 1`、`base 2`……里第一个没被占用的名字。
//
// ⚠ 从 1 开始（不是 2）：用户明确要"编号数字后缀，比如 1，2，3"。
//   和 SceneHierarchy 的 uniqueName（从 2 起，因为那是"同名再加一个"的
//   语义）刻意不同 —— 这里生成的是"第一种雨 / 第二种雨"。
std::string uniqueEmitterName(scene::Scene& sc, const std::string& base) {
    for (int i = 1; i < 10000; ++i) {
        const std::string n = base + " " + std::to_string(i);
        if (!sc.find(n).valid()) return n;
    }
    return base;
}

void tagItem(const char* tag) {
    logRect(tag, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

} // namespace

// ---------------------------------------------------------------- 增删改

void ParticlesPanel::addPreset(int preset) {
    if (preset < 0 || preset >= kPresetCount) return;
    const FxPreset& p = kPresets[preset];

    scene::Scene& sc = m_ctx.editorScene();
    ecs::Entity created{};
    std::string createdName;
    m_ctx.structuralEdit(std::string("Add ") + p.label + " Emitter", [&]() {
        // 名字按类型 + 编号（Rain 1 / Rain 2 ……），不再全员叫 Particle Effect
        createdName = uniqueEmitterName(sc, p.label);
        created = sc.createObject(createdName);
        if (auto* tc = sc.world().get<ecs::TransformComponent>(created)) {
            tc->position = glm::vec3(0.0f, 0.0f, p.originZ);
            // 雨 / 雪 / 闪电要"从上往下落"：发射轴是局部 +Z，绕 X 翻 180°
            // 180°（别用 glm::pi<float>()：这文件没 include gtc/constants）
            tc->rotation = p.flipDown ? glm::vec3(3.14159265f, 0.0f, 0.0f)
                                      : glm::vec3(0.0f);
            tc->scale = glm::vec3(1.0f);
        }
        ecs::ParticleEmitterComponent fx;
        fx.rate = p.rate;
        fx.lifetime = p.lifetime;
        fx.lifetimeVariance = p.lifetimeVar;
        fx.startSpeed = p.startSpeed;
        fx.spread = p.spread;
        fx.drag = p.drag;
        fx.gravity = p.gravity;
        fx.startSize = p.startSize;
        fx.emissive = p.emissive;
        fx.blend = p.blend;
        fx.renderMode = p.render;
        fx.stretchScale = p.stretchScale;
        fx.shape = p.shape;
        fx.shapeExtents = p.shapeExtents;
        fx.shapeRadius = p.shapeRadius;
        fx.noiseEnabled = p.noise;
        fx.noiseStrength = p.noiseStrength;
        fx.noiseFrequency = p.noiseFreq;
        fx.alphaOverLife = p.alphaOverLife;
        fx.sizeOverLife = p.sizeOverLife;
        // 渐变：三段（起点 → 中点 → 终点）。只给两个颜色时，中点取两者均值，
        // 这样"雨丝在半空中变亮"这种过渡是连续的，不会在 t=0.5 突变。
        fx.colorRamp.count = 3;
        fx.colorRamp.stops[0] = glm::vec4(p.colorA, 0.0f);
        fx.colorRamp.stops[1] = glm::vec4((p.colorA + p.colorB) * 0.5f, 0.5f);
        fx.colorRamp.stops[2] = glm::vec4(p.colorB, 1.0f);
        if (p.burstCount > 0) {
            // ⚠ 不要把 rate 清零：闪电是"常驻电晕 + 周期性大闪"两路叠加，
            //   rate 由表里给（上一版在这里写 fx.rate = 0，结果两发 burst
            //   之间屏幕上什么都没有，用户以为预设没生效）。
            fx.bursts[0].time = 0.0f;
            fx.bursts[0].count = p.burstCount;
            fx.bursts[0].cycles = 1;
            fx.bursts[0].interval = 0.0f;
            fx.duration = p.duration;
            fx.looping = true;
        }
        sc.world().add<ecs::ParticleEmitterComponent>(created, fx);
    });
    if (!created.valid()) return;

    m_ctx.select(created);
    m_ctx.dirty() = true;
    // 总开关不开的话新特效是"隐形"的 —— 用户会以为按钮坏了。
    if (void* pp = render::paramPtr("fx.enabled")) {
        *static_cast<bool*>(pp) = true;
    }
    m_ctx.notify(std::string(TR("Added ")) + createdName);
    VK_LOG_INFO("FX-PANEL preset=%s name=\"%s\" entity=%u", p.label,
                createdName.c_str(), created.id);
}

void ParticlesPanel::beginRename(ecs::Entity e) {
    const std::string* nm = m_ctx.editorScene().world().name(e);
    std::snprintf(m_renameBuf, sizeof(m_renameBuf), "%s",
                  nm ? nm->c_str() : "Emitter");
    m_renameId = e.id;
    m_renameFocus = true;
    VK_LOG_INFO("FX-PANEL rename begin entity=%u", e.id);
}

void ParticlesPanel::commitRename(ecs::Entity e) {
    if (m_renameId != e.id) return;
    std::string want = m_renameBuf;
    // 去首尾空白；空名字不接受（空名字在层级树里就是一条看不见的行）
    const size_t b = want.find_first_not_of(" \t");
    const size_t en = want.find_last_not_of(" \t");
    want = (b == std::string::npos) ? std::string() : want.substr(b, en - b + 1);

    m_renameId = 0;
    if (want.empty()) {
        VK_LOG_INFO("FX-PANEL rename cancel entity=%u (empty)", e.id);
        return;
    }
    if (!m_ctx.editorScene().find(want).valid()) {
        m_ctx.structuralEdit("Rename Emitter", [&]() {
            m_ctx.editorScene().world().setName(e, want);
        });
        m_ctx.dirty() = true;
        VK_LOG_INFO("FX-PANEL rename entity=%u name=\"%s\"", e.id, want.c_str());
    } else {
        // 重名会把层级树弄成一堆同名的行，直接拒绝（比静默加后缀更可预期）
        m_ctx.notify(std::string(TR("Name already used: ")) + want);
        VK_LOG_INFO("FX-PANEL rename reject entity=%u name=\"%s\"", e.id,
                    want.c_str());
    }
}

void ParticlesPanel::duplicateEmitter(ecs::Entity e) {
    scene::Scene& sc = m_ctx.editorScene();
    const ecs::World& w = sc.world();
    const auto* src = w.get<ecs::ParticleEmitterComponent>(e);
    if (!src) return;
    const ecs::ParticleEmitterComponent copy = *src;
    const std::string* nm = w.name(e);
    const std::string base = nm ? *nm : "Emitter";
    // "Rain 1" → "Rain"（去尾部的 " 数字"），副本继续按类型编号，
    // 否则会造出 "Rain 1 1" 这种越堆越长的名字。
    std::string stem = base;
    const size_t sp = base.find_last_of(' ');
    if (sp != std::string::npos && sp + 1 < base.size() &&
        base.find_first_not_of("0123456789", sp + 1) == std::string::npos) {
        stem = base.substr(0, sp);
    }

    ecs::Entity dup{};
    std::string dupName;
    m_ctx.structuralEdit("Duplicate Emitter", [&]() {
        dupName = uniqueEmitterName(sc, stem);
        dup = sc.createObject(dupName);
        if (const auto* t = w.get<ecs::TransformComponent>(e)) {
            if (auto* td = sc.world().get<ecs::TransformComponent>(dup)) {
                *td = *t;
                td->position.x += 1.0f;   // 挪开 1m，否则和原来的完全重合看不出
            }
        }
        sc.world().add<ecs::ParticleEmitterComponent>(dup, copy);
    });
    if (!dup.valid()) return;
    m_ctx.select(dup);
    m_ctx.dirty() = true;
    VK_LOG_INFO("FX-PANEL duplicate from=%u to=%u name=\"%s\"", e.id, dup.id,
                dupName.c_str());
}

void ParticlesPanel::deleteEmitter(ecs::Entity e) {
    scene::Scene& sc = m_ctx.editorScene();
    const std::string* nm = sc.world().name(e);
    const std::string label = nm ? *nm : "Emitter";
    if (m_ctx.selection() == e) m_ctx.select(ecs::Entity{});
    m_ctx.structuralEdit("Delete Emitter", [&]() {
        sc.world().destroy(e);
    });
    m_ctx.dirty() = true;
    m_ctx.notify(std::string(TR("Deleted ")) + label);
    VK_LOG_INFO("FX-PANEL delete entity=%u name=\"%s\"", e.id, label.c_str());
}

// ---------------------------------------------------------------- 清单一行

bool ParticlesPanel::emitterRow(ecs::Entity e, const char* name, int index) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h = 24.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;

    const bool selected = (m_ctx.selection() == e);
    ImGui::PushID(index);
    const bool clicked = ImGui::Selectable("##fxrow", selected,
                                           ImGuiSelectableFlags_AllowOverlap,
                                           ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();

    // ---- 右键菜单：重命名 / 复制 / 删除 ----
    // 挂着 Selectable 上（BeginPopupContextItem），所以只有这一行会响，
    // 点在空白处不会弹出"不知道操作的是谁"的菜单。
    if (ImGui::BeginPopupContextItem("##fxctx")) {
        // 自动化锚点：菜单窗口矩形（脚本据此断言"右键真的弹出了菜单"）
        logRect("FX-CTX", ImGui::GetWindowPos(),
                ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                       ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));
        // 每个菜单项自己的矩形（脚本按它点，不按"第几行"估位 —— 行高随
        // 主题/翻译变，估位会在换语言之后点空）
        if (ImGui::MenuItem(TR("Rename"))) {
            beginRename(e);
            ImGui::CloseCurrentPopup();
        }
        tagItem("FX-CTX-Rename");
        if (ImGui::MenuItem(TR("Duplicate"))) duplicateEmitter(e);
        tagItem("FX-CTX-Duplicate");
        ImGui::Separator();
        if (ImGui::MenuItem(TR("Delete"))) deleteEmitter(e);
        tagItem("FX-CTX-Delete");
        ImGui::EndPopup();
    }

    ImVec4 tc = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    if (!selected && !hovered) tc.w *= 0.88f;
    const ImU32 col = ImGui::GetColorU32(tc);
    const float cy = p.y + h * 0.5f;

    // 一颗小火花（和底栏那个 Particles 图标同形）
    const float cx = p.x + 12.0f;
    dl->AddLine(ImVec2(cx - 3.0f, cy), ImVec2(cx + 3.0f, cy), col, 1.4f);
    dl->AddLine(ImVec2(cx, cy - 3.0f), ImVec2(cx, cy + 3.0f), col, 1.4f);
    dl->AddLine(ImVec2(cx - 2.2f, cy - 2.2f), ImVec2(cx + 2.2f, cy + 2.2f), col,
                1.1f);
    dl->AddLine(ImVec2(cx - 2.2f, cy + 2.2f), ImVec2(cx + 2.2f, cy - 2.2f), col,
                1.1f);

    // ---- 名字（重命名中就换成输入框）----
    if (m_renameId == e.id) {
        ImGui::SetCursorScreenPos(ImVec2(p.x + 24.0f, p.y + 1.0f));
        ImGui::SetNextItemWidth(w - 30.0f);
        if (m_renameFocus) {
            ImGui::SetKeyboardFocusHere();
            m_renameFocus = false;
        }
        const bool entered = ImGui::InputText(
            "##fxrename", m_renameBuf, sizeof(m_renameBuf),
            ImGuiInputTextFlags_EnterReturnsTrue |
                ImGuiInputTextFlags_AutoSelectAll);
        // Esc / 点到别处 = 取消（结束编辑、不改名）；回车 = 确认
        const bool cancelled = ImGui::IsItemDeactivated() && !entered;
        if (entered) {
            commitRename(e);
        } else if (cancelled && m_renameId == e.id) {
            m_renameId = 0;
            VK_LOG_INFO("FX-PANEL rename cancel entity=%u", e.id);
        }
        // 行内控件会把光标推下去：画完摆回"这一行之后"，下一行才不会错位
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
    } else {
        dl->AddText(ImVec2(p.x + 24.0f, cy - ImGui::GetTextLineHeight() * 0.5f),
                    col, name);
    }

    char tag[48];
    std::snprintf(tag, sizeof(tag), "FX-ROW %d", index);
    logRect(tag, p, ImVec2(p.x + w, p.y + h));
    ImGui::PopID();
    return clicked;
}

// ---------------------------------------------------------------- 面板

void ParticlesPanel::draw() {
    bool& open = panelOpenRef("particles");
    if (!open) return;

    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(ImVec2(380.0f, 520.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.34f,
                                   io.DisplaySize.y * 0.10f),
                            ImGuiCond_FirstUseEver);

    if (!ImGui::Begin(trId("Particles", "panel_particles"), &open)) {
        ImGui::End();
        return;
    }
    logRect("FX-WIN", ImGui::GetWindowPos(),
            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                   ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    render::Renderer& r = m_renderer;
    fx::ParticleSettings& ps = r.particleSettings();
    scene::Scene& sc = m_ctx.editorScene();

    // ---------------------------------------------------------- 全局开关
    if (ImGui::Checkbox(TR("Particle system enabled"), &ps.enabled)) {
        if (void* pp = render::paramPtr("fx.enabled")) {
            *static_cast<bool*>(pp) = ps.enabled;
        }
        VK_LOG_INFO("FX-PANEL enabled=%d", ps.enabled ? 1 : 0);
    }
    tagItem("FX-SET enabled");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Master switch: off = the GPU simulation is skipped\n"
                          "entirely (no particles are drawn at all)."));
    }
    ImGui::SameLine();
    ImGui::TextDisabled(TR("%u emitter(s) live, %u slots each"),
                        r.particleEmitterCount(), ps.perEmitterCapacity);

    // ---------------------------------------------------------- 场景清单
    ImGui::SeparatorText(TR("Emitters in this scene"));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Right-click a row to rename, duplicate or delete it."));
    }

    int count = 0;
    unsigned sig = 0;
    ImGui::BeginChild("##fx_list", ImVec2(-1.0f, 156.0f), ImGuiChildFlags_Borders);
    sc.world().each<ecs::ParticleEmitterComponent>(
        [&](ecs::Entity e, ecs::ParticleEmitterComponent& c) {
            const int idx = count++;
            const std::string* nm = sc.world().name(e);
            char label[128];
            std::snprintf(label, sizeof(label), "%s   %.0f/s  %.2fs",
                          nm ? nm->c_str() : "Emitter",
                          static_cast<double>(c.rate),
                          static_cast<double>(c.lifetime));
            sig = sig * 131u + static_cast<unsigned>(e.id) + 1u;
            if (emitterRow(e, label, idx)) {
                m_ctx.select(e);
                VK_LOG_INFO("FX-PANEL select entity=%u", e.id);
            }
        });
    if (count == 0) {
        ImGui::TextDisabled(TR("No particle emitter in this scene yet.\n"
                               "Use a preset below to add one."));
    }
    ImGui::EndChild();
    if (sig != m_lastListSig) {
        m_lastListSig = sig;
        VK_LOG_INFO("FX-PANEL list=%d live=%u", count, r.particleEmitterCount());
    }

    // ---------------------------------------------------------- 预设
    ImGui::SeparatorText(TR("Add a preset"));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Fire / Smoke / Sparks appear at the world origin.\n"
                          "Rain / Snow / Lightning are raised 6-8 m so they\n"
                          "have room to fall."));
    }
    for (int i = 0; i < kPresetCount; ++i) {
        if (i % 3) ImGui::SameLine();
        ImGui::PushID(900 + i);
        if (ImGui::Button(TR(kPresets[i].label), ImVec2(104.0f, 0.0f))) {
            addPreset(i);
        }
        ImGui::PopID();
        char tag[40];
        std::snprintf(tag, sizeof(tag), "FX-PRESET %d", i);
        tagItem(tag);
    }
    ImGui::TextDisabled(TR("Fire / Smoke / Sparks / Rain / Snow / Lightning -\n"
                           "tuned emitters you can then edit parameter by\n"
                           "parameter in the Inspector."));

    // ---------------------------------------------------------- 全局调参
    ImGui::SeparatorText(TR("Global"));
    const auto row = [](const char* label, const char* tip) {
        ImGui::TextUnformatted(TR(label));
        if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", TR(tip));
        ImGui::SameLine(kLabelW);
        ImGui::SetNextItemWidth(-1.0f);
    };

    row("Intensity", "Global brightness multiplier on particle colour");
    if (ImGui::DragFloat("##fx_int", &ps.intensity, 0.02f, 0.0f, 10.0f, "%.2f")) {
        VK_LOG_INFO("FX-PANEL intensity=%.2f", static_cast<double>(ps.intensity));
    }
    row("Size scale", "Global size multiplier (fades the whole effect up/down)");
    if (ImGui::DragFloat("##fx_size", &ps.sizeScale, 0.01f, 0.05f, 10.0f, "%.2f")) {
        VK_LOG_INFO("FX-PANEL sizeScale=%.2f", static_cast<double>(ps.sizeScale));
    }
    row("Softness", "Soft-particle fade distance multiplier");
    if (ImGui::DragFloat("##fx_soft", &ps.softness, 0.01f, 0.0f, 5.0f, "%.2f")) {
        VK_LOG_INFO("FX-PANEL softness=%.2f", static_cast<double>(ps.softness));
    }
    row("Simulation", "Fixed simulation step in Hz - 60 is Unity/Blender's default");
    int hz = static_cast<int>(ps.simHz);
    if (ImGui::SliderInt("##fx_hz", &hz, 20, 120, "%d Hz")) {
        ps.simHz = static_cast<uint32_t>(hz);
        VK_LOG_INFO("FX-PANEL simHz=%d", hz);
    }
    row("Slots / emitter",
        "Per-emitter particle capacity. Changing it reallocates the GPU\n"
        "buffers on the next frame.");
    int cap = static_cast<int>(ps.perEmitterCapacity);
    if (ImGui::SliderInt("##fx_cap", &cap, 64, 4096)) {
        ps.perEmitterCapacity = static_cast<uint32_t>(cap);
        VK_LOG_INFO("FX-PANEL capacity=%d", cap);
    }

    ImGui::End();
}

} // namespace editor
