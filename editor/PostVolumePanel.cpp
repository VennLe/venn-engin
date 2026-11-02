#include "PostVolumePanel.h"

#include "EditorContext.h"
#include "Localization.h"
#include "PanelDock.h"        // 视口标签页托管
#include "PanelRegistry.h"    // panelOpenRef
#include "ui/DebugRects.h"    // ui::logRect

#include "core/Logger.h"
#include "scene/Scene.h"

#include <imgui.h>

#include <cfloat>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>

namespace editor {

namespace {

void tagItem(const char* tag) {
    ui::logRect(tag, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

// 控件 label = "显示名##锚点"。`##` 之后是稳定 ID —— 显示名会随语言变，
// 而 ID 一变，ImGui 的**拖动状态、焦点、TestEngine 定位**全部换人。
// 这也是全仓 `TRL()` 那条纪律在手工拼 label 时的等价写法。
std::string idLabel(const char* shown, const char* tag) {
    return std::string(shown) + "##" + tag;
}

// 浮点覆盖行。
//   · 未覆盖（= kNoOverride）→ 不画滑条，只画一个没勾的框
//   · 勾上 → 写一个**该字段合理的初值**（不是 0），并画出滑条
//   · 取消勾选 → 写回 kNoOverride，**不是 0**（见头文件约束 ②）
bool floatRow(const char* shown, const char* tag, float* v, float lo, float hi,
              float onDefault, const char* fmt) {
    bool on = (*v != scene::kNoOverride);
    bool changed = false;
    if (ImGui::Checkbox(idLabel(shown, tag).c_str(), &on)) {
        *v = on ? onDefault : scene::kNoOverride;
        changed = true;
    }
    tagItem((std::string(tag) + "-CHECK").c_str());
    if (on) {
        ImGui::SameLine(190.0f);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::DragFloat(idLabel("", tag).c_str(), v, 0.01f, lo, hi, fmt))
            changed = true;
        tagItem((std::string(tag) + "-VAL").c_str());
    }
    return changed;
}

// int 三态：-1 不覆盖 / 0 关 / 1 开。
// 用下拉而不是勾选框：勾选框只能表达两态，"开关我不动"这一态就没有位置，
// 于是"这个体积不碰景深开关"会被迫写成"把它关掉"。
bool triRow(const char* shown, const char* tag, int* v) {
    const char* items[3] = {TR("Don't override"), TR("Off"), TR("On")};
    int idx = (*v < 0) ? 0 : (*v != 0 ? 2 : 1);
    ImGui::SetNextItemWidth(170.0f);
    const bool changed = ImGui::Combo(idLabel(shown, tag).c_str(), &idx, items, 3);
    tagItem((std::string(tag) + "-TRI").c_str());
    if (changed) *v = (idx == 0) ? -1 : (idx == 1 ? 0 : 1);
    return changed;
}

// 名字去重：`Volume` / `Volume 2` / `Volume 3`…
// 不做去重的话，两个同名体积在列表里完全无法区分，而它们可能一个开景深
// 一个关景深 —— 调错一个会得到"改了这个却变的是那个"。
std::string uniqueName(const scene::PostVolumeSet& set, const std::string& base) {
    auto taken = [&](const std::string& n) {
        for (const scene::PostVolume& v : set.volumes())
            if (v.name == n) return true;
        return false;
    };
    if (!taken(base)) return base;
    for (int i = 2; i < 1000; ++i) {
        const std::string n = base + " " + std::to_string(i);
        if (!taken(n)) return n;
    }
    return base;
}

} // namespace

// ============================================================
// 结构操作（面板与命令共用）
// ============================================================

int postVolumeAdd(scene::Scene& sc, const std::string& name,
                  const glm::vec3& center, const glm::vec3& halfSize) {
    scene::PostVolume v;
    v.name = uniqueName(sc.postVolumes(), name.empty() ? "Volume" : name);
    v.boundsMin = center - halfSize;
    v.boundsMax = center + halfSize;
    sc.postVolumes().add(v);
    return static_cast<int>(sc.postVolumes().volumes().size()) - 1;
}

bool postVolumeRemove(scene::Scene& sc, int index) {
    auto& vols = sc.postVolumes().volumes();
    if (index < 0 || index >= static_cast<int>(vols.size())) return false;
    vols.erase(vols.begin() + index);
    return true;
}

std::string postVolumeDescribe(const scene::PostVolume& v) {
    // 用 ostringstream 而不是 snprintf：这是**日志**行，不是界面文字 ——
    // 走 TR 会把日志也翻成中文，grep 一条都搜不到。代价是没有 512 字节
    // 缓冲区上限（名字很长时 snprintf 会静默截断）。
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(2);
    o << "name=" << v.name
      << " min=(" << v.boundsMin.x << "," << v.boundsMin.y << ","
      << v.boundsMin.z << ")"
      << " max=(" << v.boundsMax.x << "," << v.boundsMax.y << ","
      << v.boundsMax.z << ")"
      << " prio=" << v.priority << " blend=" << v.blendDistance
      << " dof=" << v.dof << " focus=" << v.dofFocus
      << " ap=" << v.dofAperture << " maxr=" << v.dofMaxRadius
      << " mb=" << v.motionBlur << " mbstr=" << v.motionBlurStrength
      << " chr=" << v.chromatic << " chrstr=" << v.chromaStrength
      << " vig=" << v.vignette << " vigstr=" << v.vignetteStrength
      << " lut=" << v.cgLutBlend << " exp=" << v.autoExposureBias;
    return o.str();
}

// ============================================================
// 面板
// ============================================================

void PostVolumePanel::draw(EditorContext& ctx, const glm::vec3& camPos) {
    bool& open = panelOpenRef("postvol");
    if (!open) return;

    ImGuiWindowFlags wf = 0;
    const bool docked = applyDockHint(wf);
    if (!docked) {
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowSizeConstraints(ImVec2(420.0f, 360.0f),
                                            ImVec2(FLT_MAX, FLT_MAX));
        // 默认给到 620 高：列表 + 几何 + 8 个覆盖行 + 实时读数，窄了就要
        // 一直滚，而"一次看全"正是这块面板存在的理由（同 Environment）。
        ImGui::SetNextWindowSize(ImVec2(520.0f, 620.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.10f,
                                       io.DisplaySize.y * 0.08f),
                                ImGuiCond_FirstUseEver);
    }

    if (!ImGui::Begin(trId("Post-process volumes", "panel_postvol"),
                      docked ? nullptr : &open, wf)) {
        ImGui::End();
        return;
    }
    ui::logRect("PV-RECT", ImGui::GetWindowPos(),
                ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                       ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    scene::Scene& sc = ctx.editorScene();
    scene::PostVolumeSet& set = sc.postVolumes();
    std::vector<scene::PostVolume>& vols = set.volumes();

    // 下标可能因为删除而越界 —— 每帧先钳一次，避免后面所有分支
    // 都要各写一遍"这个下标还合法吗"。
    if (m_sel >= static_cast<int>(vols.size())) m_sel = -1;
    if (m_confirmDelete >= static_cast<int>(vols.size())) m_confirmDelete = -1;
    // 没选中但有体积 → 自动选第一个。
    // 否则打开面板看到的是**右半边空着**、只有一句"选一个来编辑"，
    // 而列表里只有一个条目 —— 让人点一下才给看内容，是没必要的仪式。
    if (m_sel < 0 && !vols.empty()) m_sel = 0;

    // ---------------------------------------------------------- 动作行
    auto newVolume = [&]() {
        const glm::vec3 half(4.0f);
        ctx.structuralEdit(TR("Add Post-Process Volume"), [&] {
            m_sel = postVolumeAdd(sc, "Volume", camPos, half);
        });
        VK_LOG_INFO("PV-CREATE name=%s count=%d",
                    vols[static_cast<size_t>(m_sel)].name.c_str(),
                    static_cast<int>(vols.size()));
    };

    if (ImGui::Button(TR("New volume"))) newVolume();
    tagItem("PV-NEW");
    ImGui::SameLine();
    if (ImGui::Button(TR("Duplicate")) && m_sel >= 0) {
        const scene::PostVolume src = vols[static_cast<size_t>(m_sel)];
        const int srcIdx = m_sel;
        ctx.structuralEdit(TR("Duplicate Post-Process Volume"), [&] {
            const int at = postVolumeAdd(sc, src.name, glm::vec3(0.0f),
                                         glm::vec3(1.0f));
            vols[static_cast<size_t>(at)] = src;      // 整份复制，只改名字
            vols[static_cast<size_t>(at)].name =
                uniqueName(set, src.name + " copy");
            m_sel = at;
        });
        VK_LOG_INFO("PV-DUP from=%d index=%d count=%d", srcIdx, m_sel,
                    static_cast<int>(vols.size()));
    }
    tagItem("PV-DUP");
    ImGui::SameLine();
    // 删除要二次确认：一个体积可能配了十几个覆盖值，误删很难重建。
    if (m_confirmDelete == m_sel && m_sel >= 0) {
        if (ImGui::Button(TR("Confirm delete"))) {
            const std::string gone = vols[static_cast<size_t>(m_sel)].name;
            const int at = m_sel;
            ctx.structuralEdit(TR("Delete Post-Process Volume"), [&] {
                postVolumeRemove(sc, at);
            });
            m_sel = -1;
            m_confirmDelete = -1;
            VK_LOG_INFO("PV-DELETE name=%s count=%d", gone.c_str(),
                        static_cast<int>(vols.size()));
        }
    } else if (ImGui::Button(TR("Delete")) && m_sel >= 0) {
        m_confirmDelete = m_sel;
    }
    tagItem("PV-DEL");

    ImGui::SameLine();
    ImGui::TextDisabled(TR("%d in this scene"), static_cast<int>(vols.size()));

    // ---------------------------------------------------------- 列表
    ImGui::SeparatorText(TR("Volumes"));
    if (vols.empty()) {
        ImGui::TextDisabled("%s",
                            TR("No volume yet. 'New volume' drops one at the "
                               "camera."));
    }
    for (int i = 0; i < static_cast<int>(vols.size()); ++i) {
        const scene::PostVolume& v = vols[static_cast<size_t>(i)];
        const float w = set.weightAt(v, camPos);
        // 权重 > 0 的体积标一个点：用户一眼能看出"现在哪几个在起作用"，
        // 而不用逐个点开看坐标。
        char row[192];
        std::snprintf(row, sizeof(row), TR("%s%s   prio=%.1f  w=%.2f"),
                      (w > 0.0f ? "* " : "  "), v.name.c_str(),
                      static_cast<double>(v.priority), static_cast<double>(w));
        ImGui::PushID(i);
        if (ImGui::Selectable(row, i == m_sel)) {
            m_sel = i;
            m_confirmDelete = -1;
            VK_LOG_INFO("PV-SELECT index=%d name=%s", i, v.name.c_str());
        }
        ImGui::PopID();
        const std::string tag = "PV-ITEM-" + std::to_string(i);
        tagItem(tag.c_str());
    }

    if (m_sel < 0) {
        ImGui::Separator();
        // 走到这里只可能是"场景里一个体积都没有"（上面刚自动选过）
        ImGui::TextDisabled("%s",
                            TR("New volume drops one at the camera — then tick "
                               "an override."));
        ImGui::End();
        return;
    }

    // ---------------------------------------------------------- 几何
    scene::PostVolume& v = vols[static_cast<size_t>(m_sel)];
    ImGui::SeparatorText(TR("Bounds (world space)"));

    // 名字：输入缓冲按"当前下标"重灌（见头文件 m_nameFor 的说明）。
    if (m_nameFor != m_sel) {
        m_nameFor = m_sel;
        std::snprintf(m_nameBuf, sizeof(m_nameBuf), "%s", v.name.c_str());
    }
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText(idLabel(TR("name"), "pv-name").c_str(), m_nameBuf,
                     sizeof(m_nameBuf));
    tagItem("PV-NAME");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        if (m_nameBuf[0] && v.name != m_nameBuf) {
            v.name = m_nameBuf;
            ctx.dirty() = true;
            VK_LOG_INFO("PV-RENAME index=%d name=%s", m_sel, v.name.c_str());
        } else {
            // 空名字一律回滚：空名字在列表里是一行空白，点都点不准。
            std::snprintf(m_nameBuf, sizeof(m_nameBuf), "%s", v.name.c_str());
        }
    }

    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::DragFloat3(idLabel(TR("min"), "pv-min").c_str(), &v.boundsMin.x,
                          0.05f, -1000.0f, 1000.0f, "%.2f"))
        ctx.dirty() = true;
    tagItem("PV-MIN");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::DragFloat3(idLabel(TR("max"), "pv-max").c_str(), &v.boundsMax.x,
                          0.05f, -1000.0f, 1000.0f, "%.2f"))
        ctx.dirty() = true;
    tagItem("PV-MAX");

    // 尺寸只读显示：拖 min/max 时很容易把盒子拖成负尺寸（min > max）。
    // weightOf 内部会 min/max 归一化，所以不是错误，但作者看不出自己
    // 到底做了多大一个盒子 —— 这一行给的是"绝对值尺寸"。
    const glm::vec3 size = glm::abs(v.boundsMax - v.boundsMin);
    ImGui::SameLine();
    ImGui::TextDisabled(TR("size: %.2f x %.2f x %.2f m"),
                        static_cast<double>(size.x),
                        static_cast<double>(size.y),
                        static_cast<double>(size.z));

    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::DragFloat(idLabel(TR("priority"), "pv-prio").c_str(), &v.priority,
                         0.05f, 0.0f, 100.0f, "%.2f"))
        ctx.dirty() = true;
    tagItem("PV-PRIORITY");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("%s", TR("Higher priority wins when volumes overlap."));

    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::DragFloat(idLabel(TR("blend distance (m)"), "pv-blend").c_str(),
                         &v.blendDistance, 0.05f, 0.0f, 100.0f, "%.2f"))
        ctx.dirty() = true;
    tagItem("PV-BLEND");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("%s",
                          TR("Fade width outside the box. 0 = hard cut."));

    // ---------------------------------------------------------- 覆盖
    ImGui::SeparatorText(TR("Overrides"));
    ImGui::TextDisabled("%s", TR("Unchecked = leave the camera setting alone."));

    // 覆盖段套一个**固定高度的滚动区**。
    // 理由有两条，都是实测出来的：
    //   ① 8 个覆盖行 + 几何段一共 ~840 px，而停靠进内容区后只有 ~740 px ——
    //      最后两行（LUT / 曝光）会被面板底边裁掉，而 `logRect` **不判可见性**，
    //      于是"锚点在日志里"完全推不出"控件在屏幕上"（本仓踩过多次）。
    //   ② 底部的实时读数必须**永远可见**：它才是"为什么没生效"的答案，
    //      不能让它被挤到滚动区底下。
    ImGui::BeginChild("##pvoverrides", ImVec2(0, 214), false);
    bool touched = false;
    touched |= triRow(TR("Depth of field"), "PV-OVR-DOF", &v.dof);
    if (v.dof == 0) {
        // 关掉时下面三个浮点没有意义。**不隐藏它们**（隐藏会让"我明明
        // 填过焦距，怎么没了"），改成灰掉 + 说明。
        ImGui::BeginDisabled(true);
    }
    touched |= floatRow(TR("Focus distance"), "PV-OVR-DOF-FOCUS", &v.dofFocus,
                        0.1f, 200.0f, 5.0f, "%.2f m");
    touched |= floatRow(TR("Aperture (f-stop)"), "PV-OVR-DOF-APERTURE",
                        &v.dofAperture, 0.7f, 22.0f, 2.8f, "f/%.1f");
    touched |= floatRow(TR("Max blur radius"), "PV-OVR-DOF-MAXR",
                        &v.dofMaxRadius, 0.0f, 64.0f, 8.0f, "%.2f px");
    if (v.dof == 0) ImGui::EndDisabled();

    // 这三组把"开关 + 强度"放在**同一行**：
    //   开关与强度是同一个东西的两个维度，分成两行会让面板多出 3 行高度，
    //   而高度在这里是稀缺资源（见上面滚动区那段）。宽度不紧张（默认 520，
    //   停靠后更宽），于是用宽度换高度。
    auto pairRow = [&](const char* label, const char* tag, int* sw, float* val,
                       float lo, float hi, float onDefault, const char* fmt) {
        bool t = triRow(label, tag, sw);
        ImGui::SameLine(215.0f);
        if (*sw == 0) ImGui::BeginDisabled(true);
        t |= floatRow(TR("strength"), (std::string(tag) + "-STR").c_str(), val,
                      lo, hi, onDefault, fmt);
        if (*sw == 0) ImGui::EndDisabled();
        return t;
    };
    touched |= pairRow(TR("Motion blur"), "PV-OVR-MOTION", &v.motionBlur,
                       &v.motionBlurStrength, 0.0f, 4.0f, 0.3f, "%.2f");
    touched |= pairRow(TR("Chromatic aberration"), "PV-OVR-CHROMA",
                       &v.chromatic, &v.chromaStrength, 0.0f, 4.0f, 0.2f,
                       "%.2f");
    touched |= pairRow(TR("Vignette"), "PV-OVR-VIGNETTE", &v.vignette,
                       &v.vignetteStrength, 0.0f, 4.0f, 0.4f, "%.2f");

    // 这两个没有对应的开关（它们本身就是强度），所以只有浮点行。
    touched |= floatRow(TR("Color grading LUT blend"), "PV-OVR-CG-LUT",
                        &v.cgLutBlend, 0.0f, 1.0f, 1.0f, "%.2f");
    touched |= floatRow(TR("Auto exposure bias (EV)"), "PV-OVR-EXPOSURE",
                        &v.autoExposureBias, -6.0f, 6.0f, 0.0f, "%+.2f EV");
    ImGui::EndChild();

    if (touched) {
        ctx.dirty() = true;
        VK_LOG_INFO("PV-EDIT index=%d %s", m_sel, postVolumeDescribe(v).c_str());
    }

    // ---------------------------------------------------------- 实时
    // 这一段是"看不见就说不出"那条硬约束的落点：体积最常见的事故不是
    // 参数错，是**相机根本不里面**（或落在过渡带只生效 40%）。
    ImGui::SeparatorText(TR("Live (view camera)"));
    const scene::PostVolumeSet::Blended b = set.evaluate(camPos);
    int activeCount = 0;
    for (const scene::PostVolume& q : vols)
        if (set.weightAt(q, camPos) > 0.0f) ++activeCount;

    ImGui::Text(TR("camera (%.2f, %.2f, %.2f) — %d of %d active"),
                static_cast<double>(camPos.x), static_cast<double>(camPos.y),
                static_cast<double>(camPos.z), activeCount,
                static_cast<int>(vols.size()));
    tagItem("PV-LIVE");
    if (set.weightAt(v, camPos) <= 0.0f) {
        // 说清楚"为什么没生效"，而不是只给一个 0。
        ImGui::TextDisabled("%s",
                            TR("This volume does not affect the camera right "
                               "now (outside the box + blend distance)."));
    }

    // 只列**被覆盖**的项：全列出来会让人以为"这些都在生效"。
    auto line = [](const char* n, float val) {
        if (val == scene::kNoOverride) return;
        ImGui::BulletText("%s = %.3f", n, static_cast<double>(val));
    };
    auto lineTri = [](const char* n, int val) {
        if (val < 0) return;
        ImGui::BulletText("%s = %s", n, val ? "on" : "off");
    };
    lineTri("dof", b.dof);
    line("dofFocus", b.dofFocus);
    line("dofAperture", b.dofAperture);
    line("dofMaxRadius", b.dofMaxRadius);
    lineTri("motionBlur", b.motionBlur);
    line("motionBlurStrength", b.motionBlurStrength);
    lineTri("chromatic", b.chromatic);
    line("chromaStrength", b.chromaStrength);
    lineTri("vignette", b.vignette);
    line("vignetteStrength", b.vignetteStrength);
    line("cgLutBlend", b.cgLutBlend);
    line("autoExposureBias", b.autoExposureBias);

    ImGui::End();
}

} // namespace editor
