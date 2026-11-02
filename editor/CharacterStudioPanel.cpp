// ============================================================
// editor/CharacterStudioPanel.cpp
// ============================================================

#include "CharacterStudioPanel.h"

#include "AnimatorGraphPanel.h"
#include "EditorContext.h"
#include "Localization.h"
#include "PanelDock.h"

#include "animation/AnimController.h"
#include "assets/Model.h"
#include "assets/Skeleton.h"
#include "core/Logger.h"
#include "ecs/Components.h"
#include "scene/Scene.h"

#include <algorithm>
#include <cstdio>

namespace editor {

namespace {

// 步骤表。**顺序就是真实的工作顺序** —— 这一条比任何单步的实现都重要：
// 步骤条存在的意义是回答"下一步该干什么"，顺序错了就是在教人走弯路。
//
// `panelId` 非空 = 这一步有独立的编辑器窗口，右侧给一个"打开"按钮；
// 空 = 本壳自己画（目前只有状态机，因为它是唯一没有编辑器的那一步）。
struct Step {
    const char* id;
    const char* title;
    const char* panelId;
    const char* hint;
};

const Step kSteps[] = {
    {"import", "1. Import mesh", "content",
     "Bring a rigged FBX/GLB in from the Content browser."},
    {"skeleton", "2. Skeleton", "character",
     "Check joint count and bone names. A wrong rig is not fixable later."},
    {"skinning", "3. Skinning", "character",
     "Bind the mesh to the skeleton and verify the joint matrices."},
    {"pose", "4. Pose", "character",
     "Test poses, save the good ones to the pose library."},
    {"keys", "5. Keyframes", "dope",
     "Author timing: which bone moves when."},
    {"curves", "6. Curves", "bonecurv",
     "Shape the interpolation between keys."},
    {"nla", "7. NLA", "nla",
     "Lay clips on a timeline; blend and offset them."},
    {"statemachine", "8. State machine", "",
     "Wire clips into Idle/Walk/Run with transition conditions."},
    {"layers", "9. Layers & events", "character",
     "Overlay masks for upper/lower body; fire events at clip times."},
    {"preview", "10. Preview", "character",
     "Play it in the viewport with the real controller attached."},
};

constexpr int kStepCount = static_cast<int>(sizeof(kSteps) / sizeof(kSteps[0]));

constexpr float kSideW = 210.0f;

ImVec4 okColor() { return ImVec4(0.55f, 0.85f, 0.55f, 1.0f); }
ImVec4 todoColor() { return ImVec4(0.62f, 0.65f, 0.72f, 1.0f); }
ImVec4 warnColor() { return ImVec4(0.95f, 0.62f, 0.42f, 1.0f); }

// 当前 clip 的关键帧总数（所有轨道）。
int totalKeys(const assets::AnimationClip* c) {
    if (!c) return 0;
    int n = 0;
    for (const auto& tr : c->tracks) n += static_cast<int>(tr.times.size());
    return n;
}

}  // namespace

CharacterStudioPanel::CharacterStudioPanel(EditorContext& ctx,
                                           AnimatorGraphPanel& graph)
    : m_ctx(ctx), m_graph(graph) {}

void CharacterStudioPanel::goTo(const std::string& stepId) {
    for (int i = 0; i < kStepCount; ++i) {
        if (stepId == kSteps[i].id) {
            m_step = i;
            m_open = true;
            return;
        }
    }
    VK_LOG_WARN("CHARSTUDIO-GOTO unknown step '%s'", stepId.c_str());
}

CharacterStudioPanel::Target CharacterStudioPanel::resolveTarget() const {
    Target t;
    t.entity = m_ctx.selection();
    if (!t.entity.valid()) return t;

    ecs::World& w = m_ctx.editorScene().world();
    if (!w.alive(t.entity)) return Target{};
    if (const auto* nm = w.get<ecs::NameComponent>(t.entity)) t.name = nm->name;
    if (t.name.empty()) t.name = "Entity " + std::to_string(t.entity.id);

    t.sk = w.get<ecs::SkinnedMeshComponent>(t.entity);
    t.an = w.get<ecs::AnimatorComponent>(t.entity);

    // 骨架来源有两条（`AnimatorComponent::model` 优先，其次蒙皮组件）——
    // 与 `AnimatorSystem` 的判定保持一致，否则面板显示"有骨架"而
    // 实际跑的时候找不到骨架。
    assets::Model* m = nullptr;
    if (t.an && t.an->model) m = t.an->model;
    else if (t.sk) m = t.sk->model;
    if (m && m->skeleton) t.skel = m->skeleton.get();
    else if (t.sk && t.sk->model && t.sk->model->skeleton)
        t.skel = t.sk->model->skeleton.get();
    return t;
}

bool CharacterStudioPanel::stepHasData(const char* id) const {
    const Target t = resolveTarget();
    const std::string s(id);

    if (s == "import") return t.sk != nullptr && t.sk->model != nullptr;
    if (s == "skeleton") return t.skel != nullptr && !t.skel->joints.empty();
    if (s == "skinning")
        return t.sk != nullptr && !t.sk->jointMatrices.empty();
    if (s == "pose") return t.an != nullptr || t.sk != nullptr;
    if (s == "keys" || s == "curves" || s == "preview") {
        if (!t.skel) return false;
        const assets::AnimationClip* c = t.skel->findClip(
            t.sk ? t.sk->clipName : std::string());
        return totalKeys(c) > 0;
    }
    if (s == "nla") return t.an != nullptr && !t.an->nlaTracks.empty();
    if (s == "statemachine") return t.an != nullptr && !t.an->controllerPath.empty();
    if (s == "layers") return t.an != nullptr && !t.an->layers.empty();
    return false;
}

const char* CharacterStudioPanel::stepPanelId(const char* id) const {
    for (int i = 0; i < kStepCount; ++i)
        if (std::string(id) == kSteps[i].id) return kSteps[i].panelId;
    return "";
}

// ---------------------------------------------------------------- 主绘制

void CharacterStudioPanel::draw() {
    if (!m_open) return;

    const ImGuiIO& io = ImGui::GetIO();
    // 同 AnimatorGraphPanel：本面板 layoutSlot = kSlotNone（视口标签页宿主），
    // 必须接 dock 提示，否则被设为当前页时浮在左上角盖住整个界面。
    ImGuiWindowFlags wf = 0;
    const bool docked = applyDockHint(wf);
    if (!docked) {
        ImGui::SetNextWindowSize(
            ImVec2(glm::clamp(io.DisplaySize.x * 0.62f, 820.0f, 1180.0f),
                   glm::clamp(io.DisplaySize.y * 0.68f, 480.0f, 800.0f)),
            ImGuiCond_FirstUseEver);
    }

    // 稳定 ID 必须 = trayWindowName("charstudio")
    if (!ImGui::Begin(trId("Character Studio", "panel_charstudio"),
                      docked ? nullptr : &m_open, wf)) {
        ImGui::End();
        return;
    }

    const Target t = resolveTarget();

    // 顶部：目标是谁。没目标时这里就是全部信息 —— 不往下画一个
    // 全是"未知"的表，那只会让人以为面板坏了。
    if (!t.entity.valid()) {
        ImGui::TextColored(warnColor(), "%s", TR("No character selected."));
        ImGui::Spacing();
        ImGui::TextWrapped(
            "%s", TR("Select an entity with a SkinnedMeshComponent in the "
                     "Hierarchy, then come back. The steps below show what "
                     "each one needs."));
        ImGui::Spacing();
        ImGui::Separator();
        for (int i = 0; i < kStepCount; ++i) {
            ImGui::TextDisabled("%s", TR(kSteps[i].title));
            ImGui::SameLine(kSideW);
            ImGui::TextDisabled("%s", TR(kSteps[i].hint));
        }
        ImGui::End();
        return;
    }

    // 目标行
    ImGui::Text("%s: %s", TR("Character"), t.name.c_str());
    ImGui::SameLine();
    if (t.skel) {
        ImGui::TextDisabled("| %s %d  %s %d", TR("joints"),
                            static_cast<int>(t.skel->joints.size()),
                            TR("clips"),
                            static_cast<int>(t.skel->clips.size()));
    } else {
        ImGui::TextDisabled("| %s", TR("no skeleton"));
    }
    ImGui::SameLine();
    // 这里**不能**写 `TextDisabled("| %s", TR("entity %u"), t.entity.id)`：
    // 格式串是 "%s"，`%u` 会被原样印出来（面板上就是 "entity %u"）。
    // TR 返回的是**已经翻译过、可能带占位符**的模板串，要由它当格式串用。
    {
        char ent[64];
        std::snprintf(ent, sizeof(ent), TR("| entity %u"), t.entity.id);
        ImGui::TextDisabled("%s", ent);
    }

    ImGui::Separator();

    const float bodyH = ImGui::GetContentRegionAvail().y - 4.0f;
    ImGui::BeginChild("##steps", ImVec2(kSideW, bodyH), true);
    drawStepList(kSideW, bodyH);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##content", ImVec2(0.0f, bodyH), false);
    drawStepBody();
    ImGui::EndChild();

    ImGui::End();
}

void CharacterStudioPanel::drawStepList(float width, float height) {
    (void)width;
    (void)height;
    ImGui::TextUnformatted(TR("Workflow"));
    ImGui::Separator();

    for (int i = 0; i < kStepCount; ++i) {
        const bool has = stepHasData(kSteps[i].id);

        // 状态点：● 有数据 / ○ 还没做。用文字而不是小方块 ——
        // 方块要自己算位置，而 ImGui 的 Selectable 会把行占满。
        ImGui::TextColored(has ? okColor() : todoColor(), has ? "\xe2\x97\x8f"
                                                             : "\xe2\x97\x8b");
        ImGui::SameLine();

        ImGui::PushID(i);
        if (ImGui::Selectable(kSteps[i].title, m_step == i)) m_step = i;
        ImGui::PopID();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextWrapped("%s", TR("Dots show whether that step has data on the "
                                "selected character. None of them block you."));
}

void CharacterStudioPanel::drawStepBody() {
    if (m_step < 0 || m_step >= kStepCount) m_step = 0;
    const Step& s = kSteps[m_step];

    ImGui::PushStyleColor(ImGuiCol_Text, todoColor());
    ImGui::TextWrapped("%s", TR(s.hint));
    ImGui::PopStyleColor();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const std::string id(s.id);
    if (id == "import") drawImportStep();
    else if (id == "skeleton") drawSkeletonStep();
    else if (id == "skinning") drawSkinningStep();
    else if (id == "pose") drawPoseStep();
    else if (id == "keys") drawKeysStep();
    else if (id == "curves") drawCurvesStep();
    else if (id == "nla") drawNlaStep();
    else if (id == "statemachine") drawStateMachineStep();
    else if (id == "layers") drawLayersStep();
    else drawPreviewStep();

    // 底部：跳到独立编辑器。状态机那一步没有按钮（它就在上面画着）。
    const char* panel = stepPanelId(s.id);
    if (panel && panel[0]) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        char label[128];
        std::snprintf(label, sizeof(label), "%s: %s", TR("Open"),
                      TR(s.title));
        if (ImGui::Button(label)) {
            m_ctx.requestPanelOpen(panel);
            VK_LOG_INFO("CHARSTUDIO-OPEN panel=%s step=%s", panel, s.id);
        }
    }
}

// ---------------------------------------------------------------- 各步

void CharacterStudioPanel::drawImportStep() {
    const Target t = resolveTarget();
    if (!t.sk || !t.sk->model) {
        ImGui::TextColored(warnColor(), "%s", TR("This entity has no mesh."));
        ImGui::Spacing();
        ImGui::TextWrapped(
            "%s", TR("Drag a rigged FBX/GLB from the Content browser into the "
                     "viewport, or use GameObject > Create character. The "
                     "asset must contain a skeleton — a static mesh can not "
                     "be animated."));
        return;
    }
    const assets::Model& m = *t.sk->model;
    ImGui::Text("%s: %d", TR("Submeshes"),
                static_cast<int>(m.subMeshes.size()));
    // 材质挂在子网格上（`SubMesh::material`），没有 model 级的材质表 ——
    // 这里数"有材质的子网格"，比数材质指针更贴近用户看到的"几片带贴图"。
    int withMat = 0;
    for (const auto& sm : m.subMeshes)
        if (sm.material) ++withMat;
    ImGui::Text("%s: %d/%d", TR("Submeshes with material"), withMat,
                static_cast<int>(m.subMeshes.size()));
    ImGui::Text("%s: %s", TR("Skeleton"),
                m.skeleton ? TR("yes") : TR("no"));
    if (!m.skeleton) {
        ImGui::Spacing();
        ImGui::TextColored(warnColor(), "%s",
                           TR("No skeleton in this asset. Nothing after this "
                              "step can work."));
    }
}

void CharacterStudioPanel::drawSkeletonStep() {
    const Target t = resolveTarget();
    if (!t.skel || t.skel->joints.empty()) {
        ImGui::TextColored(warnColor(), "%s", TR("No skeleton."));
        return;
    }
    ImGui::Text("%s: %d", TR("Joints"),
                static_cast<int>(t.skel->joints.size()));
    ImGui::Text("%s: %d", TR("Clips"),
                static_cast<int>(t.skel->clips.size()));

    ImGui::Spacing();
    ImGui::TextDisabled("%s", TR("Bone names (first 200)"));
    ImGui::BeginChild("##bones", ImVec2(0.0f, 220.0f), true);
    const int n = std::min(200, static_cast<int>(t.skel->joints.size()));
    for (int i = 0; i < n; ++i)
        ImGui::Text("%3d  %s", i, t.skel->joints[static_cast<size_t>(i)].name.c_str());
    ImGui::EndChild();
    if (static_cast<int>(t.skel->joints.size()) > 200)
        ImGui::TextDisabled("%s", TR("... truncated"));
}

void CharacterStudioPanel::drawSkinningStep() {
    const Target t = resolveTarget();
    if (!t.sk) {
        ImGui::TextColored(warnColor(), "%s",
                           TR("No SkinnedMeshComponent on this entity."));
        return;
    }
    const int joints = static_cast<int>(t.sk->jointMatrices.size());
    const int skelJoints =
        t.skel ? static_cast<int>(t.skel->joints.size()) : 0;
    ImGui::Text("%s: %d", TR("Joint matrices"), joints);
    ImGui::Text("%s: %d", TR("Skeleton joints"), skelJoints);

    // 两者不等是真实的坏状态：矩阵数由 AnimatorSystem 每帧重算，
    // 数量对不上意味着"这一帧还没跑过"或者"绑定用的是另一具骨架"。
    if (joints > 0 && skelJoints > 0 && joints != skelJoints) {
        ImGui::Spacing();
        ImGui::TextColored(warnColor(), "%s",
                           TR("Joint matrix count does not match the "
                              "skeleton — skinning will look wrong."));
    } else if (joints == 0) {
        ImGui::Spacing();
        ImGui::TextDisabled(
            "%s", TR("Matrices appear once the entity has ticked at least "
                     "one frame (i.e. after Play or a viewport frame)."));
    }

    if (t.skel && !t.skel->clips.empty()) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("%s", TR("Clips in this asset"));
        for (const auto& c : t.skel->clips) {
            ImGui::BulletText("%s  (%.2f s, %d %s)", c.name.c_str(),
                              static_cast<double>(c.duration),
                              totalKeys(&c), TR("keys"));
        }
    }
}

void CharacterStudioPanel::drawPoseStep() {
    const Target t = resolveTarget();
    if (!t.sk || !t.sk->model) {
        ImGui::TextColored(warnColor(), "%s", TR("No mesh to pose."));
        return;
    }
    ImGui::TextDisabled("%s", TR("Pose editing happens in the Character "
                                 "window: pick a bone, rotate it, then "
                                 "\"Save to library\"."));
    ImGui::Spacing();
    ImGui::Text("%s: %s", TR("Current clip"),
                t.sk->clipName.empty() ? "-" : t.sk->clipName.c_str());
    ImGui::Text("%s: %.3f s", TR("Time"), static_cast<double>(t.sk->time));
}

void CharacterStudioPanel::drawKeysStep() {
    const Target t = resolveTarget();
    if (!t.skel) {
        ImGui::TextColored(warnColor(), "%s", TR("No skeleton."));
        return;
    }
    const assets::AnimationClip* c =
        t.skel->findClip(t.sk ? t.sk->clipName : std::string());
    if (!c) {
        ImGui::TextColored(warnColor(), "%s",
                           TR("No clip selected. Set one in the Character "
                              "window's Animation tab."));
        return;
    }
    ImGui::Text("%s: %s", TR("Clip"), c->name.c_str());
    ImGui::Text("%s: %.3f s", TR("Duration"), static_cast<double>(c->duration));
    ImGui::Text("%s: %d", TR("Tracks"),
                static_cast<int>(c->tracks.size()));
    ImGui::Text("%s: %d", TR("Keys"), totalKeys(c));

    ImGui::Spacing();
    ImGui::TextDisabled("%s", TR("Edit timing in the Dope sheet."));
}

void CharacterStudioPanel::drawCurvesStep() {
    const Target t = resolveTarget();
    if (!t.skel) {
        ImGui::TextColored(warnColor(), "%s", TR("No skeleton."));
        return;
    }
    const assets::AnimationClip* c =
        t.skel->findClip(t.sk ? t.sk->clipName : std::string());
    if (!c) {
        ImGui::TextColored(warnColor(), "%s", TR("No clip selected."));
        return;
    }
    // 只有"有轨道"才值得去曲线编辑器 —— 空片段打开它是一片空白，
    // 用户会以为工具坏了。
    ImGui::Text("%s: %s", TR("Clip"), c->name.c_str());
    ImGui::Text("%s: %d", TR("Tracks"),
                static_cast<int>(c->tracks.size()));
    if (c->tracks.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", TR("This clip has no tracks yet. Key some "
                                     "bones first (step 5)."));
    }
}

void CharacterStudioPanel::drawNlaStep() {
    const Target t = resolveTarget();
    if (!t.an) {
        ImGui::TextColored(warnColor(), "%s",
                           TR("No AnimatorComponent on this entity."));
        return;
    }
    if (t.an->nlaTracks.empty()) {
        ImGui::TextDisabled("%s", TR("No NLA tracks. Open the NLA panel to "
                                     "lay clips on a timeline."));
        return;
    }
    for (const auto& tr : t.an->nlaTracks) {
        ImGui::BulletText("%s  %d %s", tr.name.c_str(),
                          static_cast<int>(tr.strips.size()), TR("strips"));
    }
}

void CharacterStudioPanel::drawStateMachineStep() {
    const Target t = resolveTarget();

    // 没有路径时先给"从哪儿来"——直接把人丢进一个空图编辑器，
    // 他并不知道自己编辑的是哪个文件。
    std::string path = t.an ? t.an->controllerPath : std::string();
    if (path.empty()) {
        ImGui::TextColored(warnColor(), "%s",
                           TR("No controller assigned to this character."));
        ImGui::Spacing();
        ImGui::TextWrapped(
            "%s", TR("Pick one below to open it in the editor, then assign "
                     "its path in the Inspector's Animator section."));
        ImGui::Spacing();
        if (ImGui::Button(TR("Edit assets/animators/humanoid.anim.json"))) {
            path = "animators/humanoid.anim.json";
            m_graph.open(path);
            VK_LOG_INFO("CHARSTUDIO-SM-OPEN path=%s", path.c_str());
        }
    } else {
        if (!m_graph.isOpen() || m_graph.path() != path) {
            // 首次进入：自动打开这个角色挂的那一份。
            m_graph.open(path);
            VK_LOG_INFO("CHARSTUDIO-SM-OPEN path=%s", path.c_str());
        }
        ImGui::Text("%s: %s", TR("Controller"), path.c_str());
        ImGui::TextDisabled("%s",
                            TR("Assigned to this character; edits apply on "
                               "the next reload."));
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // 内嵌：状态机是唯一"没有独立窗口"的一步，所以它必须画在这里。
    // 其余九步都是把已有编辑器打开 —— 见头文件第 ① 条。
    ImGui::BeginChild("##sm", ImVec2(0.0f, 0.0f), true);
    m_graph.drawBody(m_ctx, true);
    ImGui::EndChild();
}

void CharacterStudioPanel::drawLayersStep() {
    const Target t = resolveTarget();
    if (!t.an) {
        ImGui::TextColored(warnColor(), "%s",
                           TR("No AnimatorComponent on this entity."));
        return;
    }
    ImGui::Text("%s: %d", TR("Layers"),
                static_cast<int>(t.an->layers.size()));
    for (const auto& l : t.an->layers) {
        ImGui::BulletText(TR("%s  (clip: %s, %d %s)"), l.name.c_str(),
                          l.clip.empty() ? "-" : l.clip.c_str(),
                          static_cast<int>(l.mask.roots.size()), TR("mask"));
    }
    ImGui::Spacing();
    ImGui::Text("%s: %d", TR("Event tracks"),
                static_cast<int>(t.an->eventTracks.size()));
    for (const auto& e : t.an->eventTracks) {
        ImGui::BulletText("%s  %d %s", e.clip.c_str(),
                          static_cast<int>(e.events.size()), TR("events"));
    }
    if (t.an->layers.empty() && t.an->eventTracks.empty()) {
        ImGui::TextDisabled("%s", TR("Nothing here yet. Add layers in the "
                                     "Character window's Layers section."));
    }
}

void CharacterStudioPanel::drawPreviewStep() {
    const Target t = resolveTarget();
    if (!t.sk) {
        ImGui::TextColored(warnColor(), "%s", TR("No mesh to preview."));
        return;
    }
    ImGui::Text("%s: %s", TR("Clip"),
                t.sk->clipName.empty() ? "-" : t.sk->clipName.c_str());
    ImGui::Text("%s: %.3f s   %s: %.2f", TR("Time"),
                static_cast<double>(t.sk->time), TR("Speed"),
                static_cast<double>(t.sk->speed));
    ImGui::Text("%s: %s", TR("Playing"),
                t.sk->playing ? TR("yes") : TR("no"));
    if (t.an && !t.an->controllerPath.empty())
        ImGui::Text("%s: %s", TR("Controller"), t.an->controllerPath.c_str());
    else
        ImGui::TextDisabled("%s", TR("No controller: only the single clip "
                                     "plays. Finish step 8 to see the real "
                                     "behaviour."));

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("%s", TR("Press Play in the toolbar to run the "
                                 "controller against the physics inputs."));
}

}  // namespace editor
