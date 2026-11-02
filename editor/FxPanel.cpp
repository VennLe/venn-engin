#include "FxPanel.h"

#include "DebugRects.h"
#include "Localization.h"

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

} // namespace

const std::vector<std::string>& FxPanel::presets() {
    // ⚠ 路径相对 `assets/`。列的是随仓库带的三个示例，作者可以照它们的
    //   结构做自己的；面板不扫目录（打包后是 `pak:`，扫不了）。
    static const std::vector<std::string> k = {
        "fx/hit_spark.veffect",
        "fx/slash_a.veffect",
        "fx/heal_glow.veffect",
    };
    return k;
}

std::string FxPanel::stateLine() const {
    if (!m_fx) return "fx budget=0 active=0 played=0 dropped=0 cached=0 ready=0";
    return m_fx->stateLine();
}

uint32_t FxPanel::playPath(const std::string& path, const glm::vec3& at,
                           std::string* err) {
    if (!m_fx) {
        if (err) *err = "no effect service";
        return 0;
    }
    fx::EffectRequest r;
    r.asset = path;
    r.position = at;
    const uint32_t id = m_fx->play(r);
    if (id == 0 && err) {
        *err = "asset missing or all slots busy";
    }
    return id;
}

void FxPanel::stopAll() {
    if (m_fx) m_fx->stopAll();
    m_status = TR("All effects stopped.");
}

bool FxPanel::setBudget(int n, std::string* err) {
    if (!m_fx) {
        if (err) *err = "no effect service";
        return false;
    }
    // ⚠ 上限 64 与 `MYVK_FX_EMITTERS` 的上限一致 —— 超过 64 个发射器
    //   粒子池的显存会翻好几倍，而收益只有"同屏更多特效"。
    if (n < 1 || n > 64) {
        if (err) *err = "budget must be 1..64";
        return false;
    }
    m_fx->setBudget(n);
    return true;
}

void FxPanel::draw() {
    if (!ImGui::Begin(TR("Effects"), nullptr)) {
        ImGui::End();
        return;
    }
    tagItem("FX-RECT");

    if (!m_fx) {
        ImGui::TextDisabled("%s", TR("No effect service."));
        tagItem("FX-NOSERVICE");
        ImGui::End();
        return;
    }

    // ---- 池状态 ----
    {
        char buf[256];
        // i18n-keep：与 `fx.state` 命令同一份文本（面板显示 + 脚本解析）。
        std::snprintf(buf, sizeof(buf),
                      "budget=%d active=%zu played=%llu dropped=%llu",
                      m_fx->budget(), m_fx->activeCount(),
                      static_cast<unsigned long long>(m_fx->played()),
                      static_cast<unsigned long long>(m_fx->dropped()));
        ImGui::TextUnformatted(buf);
        tagItem("FX-STATE");
    }
    // ⚠ `dropped > 0` 是唯一能看出"有些命中没有火花"的地方 ——
    //   画面上永远不会出现"这里本该有一个特效"的证据。
    if (m_fx->dropped() > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", TR("(dropped -- pool full)"));
        tagItem("FX-DROPPED");
    }

    ImGui::Separator();

    // ---- 一键盘播放 ----
    ImGui::TextDisabled("%s", TR("Play a preset at the origin:"));
    for (const std::string& p : presets()) {
        // 按钮名取文件名主干（`fx/hit_spark.veffect` → `hit_spark`），
        // 锚点带上全名以免两个目录下的同名文件撞车。
        std::string stem = p;
        const std::size_t slash = stem.find_last_of('/');
        if (slash != std::string::npos) stem = stem.substr(slash + 1);
        const std::size_t dot = stem.find_last_of('.');
        if (dot != std::string::npos) stem = stem.substr(0, dot);

        const std::string label = idLabel(stem.c_str(), ("##FXPLAY-" + p).c_str());
        if (ImGui::Button(label.c_str())) {
            std::string err;
            const uint32_t id = playPath(p, glm::vec3(0.0f, 0.0f, 1.0f), &err);
            m_status = (id != 0) ? (TR("Playing: ") + stem)
                                 : (TR("Failed: ") + err);
        }
        tagItem(("FX-PLAY-" + stem).c_str());
        ImGui::SameLine();
    }
    ImGui::NewLine();

    if (ImGui::Button(idLabel(TR("Stop all"), "##FX-STOPALL").c_str())) {
        stopAll();
    }
    tagItem("FX-STOPALL");

    if (!m_status.empty()) {
        ImGui::Separator();
        ImGui::TextUnformatted(m_status.c_str());
        tagItem("FX-STATUS");
    }

    ImGui::Separator();
    ImGui::TextDisabled("%s", TR("Particle host slots:"));
    int b = m_fx->budget();
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::SliderInt(idLabel(TR("budget"), "##FX-BUDGET").c_str(), &b, 1, 32)) {
        std::string err;
        setBudget(b, &err);
    }
    tagItem("FX-BUDGET");

    ImGui::End();
}

} // namespace editor
