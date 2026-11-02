#include "GameplayPanel.h"

#include "DebugRects.h"
#include "Localization.h"

#include "ecs/Components.h"
#include "scene/GameplayRuntime.h"
#include "scene/Scene.h"

#include <imgui.h>

#include <cstdio>

namespace editor {

namespace {

void tagItem(const char* tag) {
    ui::logRect(tag, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

std::string idLabel(const char* shown, const char* tag) {
    return std::string(shown) + tag;
}

const char* kHitTypes[] = {"physical", "magic", "fire", "ice", "poison", "true"};

} // namespace

// ---------------------------------------------------------------- 命令入口

std::string GameplayPanel::stateLine() const {
    if (!m_rt) return std::string("gameplay units=0 queued=0 seed=0 turn=0 ready=0");
    return std::string("gameplay ") + m_rt->stateLine() + " ready=1";
}

std::string GameplayPanel::orderLine(int n) const {
    if (!m_rt) return std::string("order=-");
    const std::string s = m_rt->orderLine(n);
    return s.empty() ? std::string("order=-") : ("order=" + s);
}

std::string GameplayPanel::advance(float dt) {
    if (!m_rt) return std::string("FAIL: no gameplay runtime");
    return m_rt->advance(dt);
}

std::string GameplayPanel::endTurn() {
    if (!m_rt) return std::string("FAIL: no gameplay runtime");
    return m_rt->endTurn();
}

std::string GameplayPanel::tickRound() {
    if (!m_rt) return std::string("FAIL: no gameplay runtime");
    return m_rt->tickTurn();
}

std::string GameplayPanel::rebuild() {
    if (!m_rt) return std::string("FAIL: no gameplay runtime");
    m_rt->rebuildQueue();
    return m_rt->orderLine(8);
}

bool GameplayPanel::setSeed(unsigned long long seed, std::string* err) {
    if (!m_rt) {
        if (err) *err = "no gameplay runtime";
        return false;
    }
    // 0 是 PCG 的退化状态，被 `Random::setState` 拒掉 —— 这里提前拦下来，
    // 否则命令会"成功"但种子没变（症状是"设了种子抽牌还是老样子"）。
    if (seed == 0ull) {
        if (err) *err = "seed must be non-zero";
        return false;
    }
    m_rt->setSeed(seed);
    return true;
}

std::string GameplayPanel::roundTrip() {
    if (!m_rt) return std::string("FAIL: no gameplay runtime");
    scene::GameplayRuntime& rt = *m_rt;
    const std::string text = rt.queue().serialize();
    const std::string before = rt.orderLine(6);

    scene::GameplayRuntime probe;
    // ⚠ 用一个**临时**队列做往返，不动真的那个：往返失败时把真队列搞坏
    //   会把"存档坏了"变成"编辑器不能用了"。
    gameplay::TurnQueue copy;
    std::string err;
    const bool ok = copy.deserialize(text, &err);
    const std::string after = copy.orderPreview(6);
    const bool same = (before == after);
    char buf[128];
    // i18n-keep：`key=value` 连写的诊断行（脚本按字段名解析，翻了就失联）
    std::snprintf(buf, sizeof(buf), "roundtrip ok=%d same=%d bytes=%d",
                  ok ? 1 : 0, same ? 1 : 0, static_cast<int>(text.size()));
    return std::string(buf);
}

std::string GameplayPanel::addPreset(ecs::Entity e, const std::string& which) {
    if (!m_rt) return std::string("FAIL: no gameplay runtime");
    gameplay::StatusDef def;
    if (which == "poison") {
        def = gameplay::presetPoison();
    } else if (which == "shield") {
        def = gameplay::presetShield();
    } else if (which == "vuln" || which == "vulnerable") {
        def = gameplay::presetVulnerable();
    } else {
        return "FAIL: unknown preset '" + which + "'";
    }
    return m_rt->addStatus(e, def);
}

std::string GameplayPanel::hit(ecs::Entity from, ecs::Entity to, float amount,
                               const std::string& type) {
    if (!m_rt) return std::string("FAIL: no gameplay runtime");
    gameplay::DamageSpec spec;
    spec.amount = amount;
    spec.type = gameplay::DamageType::Physical;
    if (!type.empty() && !gameplay::parseDamageType(type, spec.type)) {
        return "FAIL: unknown damage type '" + type + "'";
    }
    spec.source = "panel:test";
    return m_rt->damage(from, to, spec);
}

// ---------------------------------------------------------------- 绘制

void GameplayPanel::draw(ecs::Entity selected) {
    if (!ImGui::Begin(TR("Gameplay"), nullptr)) {
        ImGui::End();
        return;
    }
    tagItem("GP-RECT");

    if (!m_rt) {
        ImGui::TextDisabled("%s", TR("No gameplay runtime."));
        tagItem("GP-NORT");
        ImGui::End();
        return;
    }
    scene::GameplayRuntime& rt = *m_rt;

    // ---- 总状态 ----
    {
        const std::string s = rt.stateLine();
        ImGui::TextUnformatted(s.c_str());
        tagItem("GP-STATE");
    }

    ImGui::Separator();

    // ---- 行动顺序条 ----
    //
    // ⚠ 这是整页最重要的一行：回合制里所有的策略都建立在"接下来谁先动"上。
    //   顺序条**必须**和 `advance()` 用同一套 tie-break —— 两处不一致时
    //   玩家看到的就是顺序条在骗人（`gameplay::TurnQueue` 的 K7 钉这件事）。
    ImGui::TextDisabled("%s", TR("Turn order:"));
    {
        const std::string s = rt.orderLine(6);
        ImGui::TextWrapped("%s", s.empty() ? "-" : s.c_str());
        tagItem("GP-ORDER");
    }

    ImGui::SetNextItemWidth(90.0f);
    ImGui::DragFloat(idLabel(TR("dt"), "##GP-DT").c_str(), &m_advanceDt, 0.05f, 0.0f, 100.0f,
                     "%.2f");
    tagItem("GP-DT");
    ImGui::SameLine();
    if (ImGui::Button(idLabel(TR("Advance"), "##GP-ADVANCE").c_str())) {
        m_status = advance(m_advanceDt);
    }
    tagItem("GP-ADVANCE");
    ImGui::SameLine();
    if (ImGui::Button(idLabel(TR("End turn"), "##GP-ENDTURN").c_str())) {
        m_status = endTurn();
    }
    tagItem("GP-ENDTURN");

    if (ImGui::Button(idLabel(TR("Rebuild queue"), "##GP-REBUILD").c_str())) {
        m_status = rebuild();
    }
    tagItem("GP-REBUILD");
    ImGui::SameLine();
    if (ImGui::Button(idLabel(TR("Tick round"), "##GP-TICK").c_str())) {
        m_status = tickRound();
    }
    tagItem("GP-TICK");
    ImGui::SameLine();
    if (ImGui::Button(idLabel(TR("Round-trip"), "##GP-ROUNDTRIP").c_str())) {
        m_status = roundTrip();
    }
    tagItem("GP-ROUNDTRIP");

    ImGui::Separator();

    // ---- 随机种子 ----
    //
    // 为什么把它做成 UI：`setSeed` 是**唯一**能让"同一份操作两次跑出同样
    // 结果"的入口。没有它，排查"抽牌顺序不对"只能改代码重编。
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::InputInt(idLabel(TR("seed"), "##GP-SEEDIN").c_str(), &m_seedBuf)) {
        // 只改缓冲，不生效 —— 生效要点右边的按辵。半途输入的 "1" 不该
        // 立刻把整局的随机流重置掉。
    }
    tagItem("GP-SEEDIN");
    ImGui::SameLine();
    if (ImGui::Button(idLabel(TR("Apply seed"), "##GP-SEEDAPPLY").c_str())) {
        std::string err;
        if (!setSeed(static_cast<unsigned long long>(m_seedBuf > 0 ? m_seedBuf : 1), &err)) {
            m_status = "FAIL: " + err;
        } else {
            char buf[96];
            // i18n-keep：诊断读数（`gameplay.seed` 命令回显同一个形状）
            std::snprintf(buf, sizeof(buf), "seed=%d", m_seedBuf);
            m_status = buf;
        }
    }
    tagItem("GP-SEEDAPPLY");

    ImGui::Separator();

    // ---- 选中单位 ----
    if (!selected.valid()) {
        ImGui::TextDisabled("%s", TR("Select a unit to see its numbers."));
    } else if (!rt.scene() || !rt.scene()->world().get<ecs::GameplayComponent>(selected)) {
        ImGui::TextDisabled("%s", TR("Selected object has no Gameplay component."));
    } else {
        ImGui::TextUnformatted(rt.attrLine(selected).c_str());
        tagItem("GP-SEL-ATTRS");
        ImGui::TextWrapped("%s", rt.statusLine(selected).c_str());
        tagItem("GP-SEL-STATUS");

        if (ImGui::Button(idLabel(TR("+ poison"), "##GP-ADD-POISON").c_str())) {
            m_status = addPreset(selected, "poison");
        }
        tagItem("GP-ADD-POISON");
        ImGui::SameLine();
        if (ImGui::Button(idLabel(TR("+ shield"), "##GP-ADD-SHIELD").c_str())) {
            m_status = addPreset(selected, "shield");
        }
        tagItem("GP-ADD-SHIELD");
        ImGui::SameLine();
        if (ImGui::Button(idLabel(TR("+ vuln"), "##GP-ADD-VULN").c_str())) {
            m_status = addPreset(selected, "vuln");
        }
        tagItem("GP-ADD-VULN");

        // 自己打自己：链路最短的"伤害明细"验证（不用摆两个实体）。
        ImGui::SetNextItemWidth(80.0f);
        ImGui::DragFloat(idLabel(TR("hit"), "##GP-HITAMT").c_str(), &m_hitAmount, 1.0f, 0.0f,
                         1.0e6f, "%.0f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(96.0f);
        ImGui::Combo(idLabel(TR("type"), "##GP-HITTYPE").c_str(), &m_hitType,
                     "physical\0magic\0fire\0ice\0poison\0true\0");
        ImGui::SameLine();
        if (ImGui::Button(idLabel(TR("Hit"), "##GP-HIT").c_str())) {
            m_status = hit(selected, selected, m_hitAmount,
                           kHitTypes[m_hitType % 6]);
        }
        tagItem("GP-HIT");
    }

    if (!m_status.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", m_status.c_str());
        tagItem("GP-STATUS");
    }

    ImGui::End();
}

} // namespace editor
