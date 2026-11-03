#include "ProjectSettingsPanel.h"

#include "Localization.h"
#include "PanelDock.h"         // applyDockHint
#include "PanelRegistry.h"     // panelOpenRef（"品质"那张面板的直达按钮）
#include "ui/DebugRects.h"     // ui::logRect

#include "assets/AssetPath.h"
#include "render/QualityPresets.h"

#include "core/Logger.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace editor {

namespace {

void tagItem(const char* tag) {
    ui::logRect(tag, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

// 控件 label = "显示名##锚点"（同 PostVolumePanel / AudioMixerPanel 的纪律：
// 显示名随语言变，`##` 后的锚点不能变，否则自动化锚点跟着语言漂）。
// ⚠ `tag` 必须**自带 "##"**（`idLabel(TR("Save"), "##PROJ-SAVE")`）。
//   两个理由：① 这是全仓其它面板的写法（`"##AM-FADER-MASTER"`）；
//   ② `verify_i18n_coverage.py` 把 ImGui 实参里所有字符串字面量都当界面
//   文字，"PROJ-SAVE"（大写开头）会被判成"没包 TR"，而 "##..." 命中它的
//   SKIP_PREFIX 白名单。写成 `"##" + tag` 会让上面那条校验红 16 项。
std::string idLabel(const char* shown, const char* tag) {
    return std::string(shown) + tag;
}

bool iequals(const std::string& a, const char* b) {
    std::size_t i = 0;
    for (; i < a.size() && b[i]; ++i) {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) return false;
    }
    return i == a.size() && b[i] == '\0';
}

// 全部字段的浅比较（用来算"未保存"）。
//
// ⚠ 用"快照 vs 当前"比较，而不是每个控件改动时置脏位：控件有 20 多个，
//   漏一个的后果是"改了但状态显示已保存" → 用户直接关窗口，改动丢了。
//   比较法漏不了 —— 它对"哪个控件改的"一无所知，只看值变了没有。
bool sameSettings(const project::GameSettings& a,
                  const project::GameSettings& b) {
    // ⚠ 浮点用**容差**比较，不用 `==`：滑块走一位（0.7000001 vs 0.7）
    //   就判成"改了"的话，面板会永远显示"未保存"，而用户一个键都没碰。
    //   1e-4 比滑块的显示精度（%.2f / %.3f）还细一档，误判方向是安全的。
    auto fe = [](float p, float q) {
        const float d = p - q;
        return (d < 0.0f ? -d : d) <= 1e-4f;
    };
    const bool base = a.scene == b.scene && a.title == b.title &&
                      a.width == b.width && a.height == b.height &&
                      a.fullscreen == b.fullscreen &&
                      a.startState == b.startState && a.uiDoc == b.uiDoc &&
                      a.maxFps == b.maxFps && a.unfocusedFps == b.unfocusedFps &&
                      fe(a.timeScale, b.timeScale) && a.language == b.language &&
                      a.quality == b.quality;
    if (!base) return false;
    const project::GameAudio& x = a.audio;
    const project::GameAudio& y = b.audio;
    if (!(x.enabled == y.enabled && x.device == y.device &&
          fe(x.master, y.master) && fe(x.music, y.music) && fe(x.sfx, y.sfx) &&
          fe(x.voice, y.voice) && x.mute == y.mute &&
          x.muteOnFocusLost == y.muteOnFocusLost &&
          fe(x.duckDepth, y.duckDepth) && x.maxVoices == y.maxVoices))
        return false;
    const project::GamePlayer& p = a.player;
    const project::GamePlayer& q = b.player;
    return p.present == q.present && p.model == q.model && p.clip == q.clip &&
           p.fitBody == q.fitBody && p.controller == q.controller &&
           p.idleClip == q.idleClip && p.walkClip == q.walkClip &&
           p.runClip == q.runClip && p.jumpClip == q.jumpClip;
}

// 扫 `assets/<sub>/` 下指定后缀的资产，返回**相对 assets 根**的路径。
//
// 为什么扫盘而不是写死清单：工程里的角色/动画器是用户自己导进来的，
// 写死的清单会在第二个工程里就过期。为什么限制深度：`assets/models`
// 下面有 `.fbm` 贴图目录，递归进去会扫出几千个 png。
std::vector<std::string> scanAssets(const std::string& assetsRoot,
                                    const char* sub, const char* ext,
                                    int maxDepth = 3) {
    namespace fs = std::filesystem;
    std::vector<std::string> out;
    const fs::path root = fs::path(assetsRoot) / sub;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return out;
    const std::string want = ext;
    auto walk = [&](const fs::path& dir, int depth) {
        auto rec = [&](auto&& self, const fs::path& d, int dep) -> void {
            std::error_code e2;
            fs::directory_iterator it(d, fs::directory_options::skip_permission_denied,
                                      e2);
            if (e2) return;
            for (const auto& ent : it) {
                std::error_code e3;
                if (ent.is_directory(e3)) {
                    if (dep < maxDepth) self(self, ent.path(), dep + 1);
                    continue;
                }
                const std::string name = ent.path().filename().string();
                if (name.size() <= want.size()) continue;
                if (name.compare(name.size() - want.size(), want.size(), want) != 0)
                    continue;
                // 相对 assets 根（`models/characters/x.vmesh`）—— 这正是
                // `game.json` 里 `player.model` 要写的口径。
                out.push_back((fs::path(sub) / fs::relative(ent.path(), root, e3))
                                  .generic_string());
            }
        };
        rec(rec, dir, depth);
    };
    walk(root, 0);
    std::sort(out.begin(), out.end());
    return out;
}

// 数值行：Input + 两个微调按钮。
//
// 为什么要有 `-` / `+`：分辨率这种值是"在这个数附近挪一点"，手打 1920
// 要选中三次；更要紧的是**自动化**需要一个稳定的点击目标（`PROJ-DEC-WIDTH`
// / `PROJ-INC-WIDTH`）—— 键盘输入在合成鼠标那一套里是最脆的一环。
bool intRow(const char* shown, const char* tag, const char* decTag,
            const char* incTag, int& v, int step, int lo, int hi) {
    bool changed = false;
    ImGui::TextUnformatted(shown);
    ImGui::SameLine(190.0f);
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::InputInt(idLabel("##v", tag).c_str(), &v)) changed = true;
    tagItem(tag);
    ImGui::SameLine();
    if (ImGui::SmallButton(idLabel("-", decTag).c_str())) {
        v -= step;
        changed = true;
    }
    tagItem(decTag);
    ImGui::SameLine();
    if (ImGui::SmallButton(idLabel("+", incTag).c_str())) {
        v += step;
        changed = true;
    }
    tagItem(incTag);
    if (v < lo) {
        v = lo;
        changed = true;
    }
    if (v > hi) {
        v = hi;
        changed = true;
    }
    return changed;
}

bool floatRow(const char* shown, const char* tag, float& v, float lo, float hi) {
    bool changed = false;
    ImGui::TextUnformatted(shown);
    ImGui::SameLine(190.0f);
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderFloat(idLabel("##v", tag).c_str(), &v, lo, hi, "%.3f"))
        changed = true;
    tagItem(tag);
    return changed;
}

bool volumeRow(const char* shown, const char* tag, float& v) {
    bool changed = false;
    ImGui::TextUnformatted(shown);
    ImGui::SameLine(190.0f);
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderFloat(idLabel("##v", tag).c_str(), &v, 0.0f, 1.0f, "%.2f"))
        changed = true;
    tagItem(tag);
    return changed;
}

bool boolRow(const char* shown, const char* tag, bool& v) {
    bool changed = false;
    ImGui::TextUnformatted(shown);
    ImGui::SameLine(190.0f);
    if (ImGui::Checkbox(idLabel("##v", tag).c_str(), &v)) changed = true;
    tagItem(tag);
    return changed;
}

} // namespace

// ---------------------------------------------------------------- 键表
//
// 键名 = `game.json` 的**真实键名**（小驼峰，`audio.` 前缀表示子对象）。
// 命令 `project.set <key> <value>` 按这张表匹配（大小写不敏感）。
// ⚠ 面板的每一行、命令的每一次 set、判据里的每一条断言都从这里取键 ——
//   三处各写一个字符串，改一个名字就静默断两处。
const char* const kFieldNames[] = {
    "title", "scene", "uiDoc", "startState", "width", "height", "fullscreen",
    "maxFps", "unfocusedFps", "timeScale", "language", "quality",
    "audio.enabled", "audio.device", "audio.master", "audio.music",
    "audio.sfx", "audio.voice", "audio.mute", "audio.muteOnFocusLost",
    "audio.duckDepth", "audio.maxVoices",
};

const char* const* ProjectSettingsPanel::fieldNames(int* count) {
    if (count)
        *count = static_cast<int>(sizeof(kFieldNames) / sizeof(kFieldNames[0]));
    return kFieldNames;
}

// ---------------------------------------------------------------- 读写

void ProjectSettingsPanel::loadIfNeeded() {
    if (m_loaded) return;
    m_path = assets::resolveAssetPath("game.json");
    m_loaded = true;
    reload(nullptr);
}

bool ProjectSettingsPanel::reload(std::string* msg) {
    if (m_path.empty()) m_path = assets::resolveAssetPath("game.json");
    project::GameSettings s;
    const project::GameFileIo io = project::loadGameFile(m_path, s);
    if (!io.ok) {
        m_err = io.error;
        // ⚠ 读失败**不动** m_st：让界面继续显示上一次读到的值（并挂错误行），
        //   而不是把一堆出厂默认值摆出来 —— 后者看起来像"配置文件被清空了"，
        //   然后一次误保存就把它们真的写进盘。
        VK_LOG_WARN("PROJ-UI reload-failed path=%s err=%s", m_path.c_str(),
                    m_err.c_str());
        if (msg)
            *msg = TR("Cannot read the file (see the message below).");
        m_result.reloaded = false;
        m_result.message = m_err;
        return false;
    }
    m_st = s;
    m_saved = s;
    m_dirty = false;
    m_err.clear();
    m_note.clear();
    syncBuffers();
    VK_LOG_INFO("PROJ-UI reload path=%s created=%d title=%s", m_path.c_str(),
                io.created ? 1 : 0, m_st.title.c_str());
    m_result.reloaded = true;
    if (msg) *msg = TR("Reloaded from disk.");
    return true;
}

bool ProjectSettingsPanel::save(std::string* msg) {
    loadIfNeeded();
    // 先夹取：越界值在写盘前夹回合法区间，并把"夹了哪些"留在界面与日志里。
    std::string notes;
    const int clamped = project::clampGameSettings(m_st, &notes);
    if (clamped > 0) {
        m_note = notes;
        VK_LOG_INFO("PROJ-UI clamp n=%d %s", clamped, notes.c_str());
    }
    const project::GameFileIo io = project::saveGameFile(m_path, m_st);
    if (!io.ok) {
        m_err = io.error;
        m_result.saved = false;
        m_result.message = m_err;
        VK_LOG_WARN("PROJ-UI save-failed path=%s err=%s", m_path.c_str(),
                    m_err.c_str());
        if (msg) *msg = m_err;
        return false;
    }
    m_err.clear();
    m_saved = m_st;
    m_dirty = false;
    syncBuffers();
    char buf[256];
    std::snprintf(buf, sizeof(buf), TR("Saved %d bytes to %s"), io.bytes,
                  m_path.c_str());
    // ⚠ i18n-keep
    int fieldCount = 0;
    ProjectSettingsPanel::fieldNames(&fieldCount);
    VK_LOG_INFO("PROJ-UI save path=%s bytes=%d fields=%d", m_path.c_str(),
                io.bytes, fieldCount);
    m_result.saved = true;
    m_result.message = buf;
    if (msg) *msg = buf;
    return true;
}

void ProjectSettingsPanel::syncBuffers() {
    std::snprintf(m_titleBuf, sizeof(m_titleBuf), "%s", m_st.title.c_str());
    std::snprintf(m_sceneBuf, sizeof(m_sceneBuf), "%s", m_st.scene.c_str());
    std::snprintf(m_uiDocBuf, sizeof(m_uiDocBuf), "%s", m_st.uiDoc.c_str());
    std::snprintf(m_deviceBuf, sizeof(m_deviceBuf), "%s", m_st.audio.device.c_str());
    std::snprintf(m_modelBuf, sizeof(m_modelBuf), "%s", m_st.player.model.c_str());
    std::snprintf(m_ctrlBuf, sizeof(m_ctrlBuf), "%s", m_st.player.controller.c_str());
    std::snprintf(m_idleBuf, sizeof(m_idleBuf), "%s", m_st.player.idleClip.c_str());
    std::snprintf(m_walkBuf, sizeof(m_walkBuf), "%s", m_st.player.walkClip.c_str());
    std::snprintf(m_runBuf, sizeof(m_runBuf), "%s", m_st.player.runClip.c_str());
    std::snprintf(m_jumpBuf, sizeof(m_jumpBuf), "%s", m_st.player.jumpClip.c_str());
    // 换工程 / 重读之后**重新扫盘**：新导进来的角色必须立刻出现在下拉里，
    // 否则"我刚导入的模型挑不到"会变成一个必须重启编辑器的诡异 bug。
    m_choicesScanned = false;
}

bool ProjectSettingsPanel::setField(const std::string& key,
                                   const std::string& value,
                                   std::string* msg) {
    loadIfNeeded();

    auto bad = [&](const char* why) {
        VK_LOG_WARN("PROJ-UI set-bad key=%s value=%s why=%s", key.c_str(),
                    value.c_str(), why);
        if (msg) *msg = std::string(why);
        return false;
    };
    auto asInt = [&](int& dst, int lo, int hi) {
        char* end = nullptr;
        const long v = std::strtol(value.c_str(), &end, 10);
        // 整串必须吃掉：`12abc` 用 atoi 会静默得到 12（同 `ui2::parseUiAction`）
        if (!end || *end != '\0' || end == value.c_str()) return false;
        dst = static_cast<int>(v < lo ? lo : (v > hi ? hi : v));
        return true;
    };
    auto asFloat = [&](float& dst, float lo, float hi) {
        char* end = nullptr;
        const float v = std::strtof(value.c_str(), &end);
        if (!end || *end != '\0' || end == value.c_str()) return false;
        dst = v < lo ? lo : (v > hi ? hi : v);
        return true;
    };
    auto asBool = [&](bool& dst) {
        if (value == "1" || iequals(value, "true") || iequals(value, "on")) {
            dst = true;
            return true;
        }
        if (value.empty() || value == "0" || iequals(value, "false") ||
            iequals(value, "off")) {
            dst = false;
            return true;
        }
        return false;
    };

    if (iequals(key, "title")) m_st.title = value;
    else if (iequals(key, "scene")) m_st.scene = value;
    else if (iequals(key, "uiDoc") || iequals(key, "uidoc")) m_st.uiDoc = value;
    else if (iequals(key, "startState")) m_st.startState = value;
    else if (iequals(key, "width")) {
        if (!asInt(m_st.width, 320, 7680)) return bad("width needs an integer");
    } else if (iequals(key, "height")) {
        if (!asInt(m_st.height, 320, 7680)) return bad("height needs an integer");
    } else if (iequals(key, "fullscreen")) {
        if (!asBool(m_st.fullscreen)) return bad("fullscreen needs 0/1");
    } else if (iequals(key, "maxFps")) {
        if (!asInt(m_st.maxFps, 0, 1000)) return bad("maxFps needs an integer");
    } else if (iequals(key, "unfocusedFps")) {
        if (!asInt(m_st.unfocusedFps, 0, 1000))
            return bad("unfocusedFps needs an integer");
    } else if (iequals(key, "timeScale")) {
        if (!asFloat(m_st.timeScale, 0.01f, 10.0f))
            return bad("timeScale needs a number");
    } else if (iequals(key, "language")) {
        m_st.language = project::normalizeLanguage(value);
    } else if (iequals(key, "quality")) {
        m_st.quality = value;
    } else if (iequals(key, "audio.enabled")) {
        if (!asBool(m_st.audio.enabled)) return bad("audio.enabled needs 0/1");
    } else if (iequals(key, "audio.device")) {
        m_st.audio.device = value;
    } else if (iequals(key, "audio.master")) {
        if (!asFloat(m_st.audio.master, 0.0f, 1.0f))
            return bad("audio.master needs 0..1");
    } else if (iequals(key, "audio.music")) {
        if (!asFloat(m_st.audio.music, 0.0f, 1.0f))
            return bad("audio.music needs 0..1");
    } else if (iequals(key, "audio.sfx")) {
        if (!asFloat(m_st.audio.sfx, 0.0f, 1.0f))
            return bad("audio.sfx needs 0..1");
    } else if (iequals(key, "audio.voice")) {
        if (!asFloat(m_st.audio.voice, 0.0f, 1.0f))
            return bad("audio.voice needs 0..1");
    } else if (iequals(key, "audio.mute")) {
        if (!asBool(m_st.audio.mute)) return bad("audio.mute needs 0/1");
    } else if (iequals(key, "audio.muteOnFocusLost")) {
        if (!asBool(m_st.audio.muteOnFocusLost))
            return bad("audio.muteOnFocusLost needs 0/1");
    } else if (iequals(key, "audio.duckDepth")) {
        if (!asFloat(m_st.audio.duckDepth, 0.0f, 1.0f))
            return bad("audio.duckDepth needs 0..1");
    } else if (iequals(key, "audio.maxVoices")) {
        if (!asInt(m_st.audio.maxVoices, 4, 256))
            return bad("audio.maxVoices needs an integer");
    } else {
        return bad("unknown key");
    }

    syncBuffers();
    m_dirty = !sameSettings(m_st, m_saved);
    VK_LOG_INFO("PROJ-UI set key=%s value=%s dirty=%d", key.c_str(),
                value.c_str(), m_dirty ? 1 : 0);
    if (msg) *msg = TR("Changed (not saved yet).");
    return true;
}

std::string ProjectSettingsPanel::fieldValue(const std::string& key) {
    loadIfNeeded();
    char buf[64];
    auto i = [&](int v) {
        std::snprintf(buf, sizeof(buf), "%d", v);
        return std::string(buf);
    };
    auto f = [&](float v) {
        std::snprintf(buf, sizeof(buf), "%.4g", static_cast<double>(v));
        return std::string(buf);
    };
    if (iequals(key, "title")) return m_st.title;
    if (iequals(key, "scene")) return m_st.scene;
    if (iequals(key, "uiDoc")) return m_st.uiDoc;
    if (iequals(key, "startState")) return m_st.startState;
    if (iequals(key, "width")) return i(m_st.width);
    if (iequals(key, "height")) return i(m_st.height);
    if (iequals(key, "fullscreen")) return i(m_st.fullscreen ? 1 : 0);
    if (iequals(key, "maxFps")) return i(m_st.maxFps);
    if (iequals(key, "unfocusedFps")) return i(m_st.unfocusedFps);
    if (iequals(key, "timeScale")) return f(m_st.timeScale);
    if (iequals(key, "language")) return m_st.language.empty() ? "auto" : m_st.language;
    if (iequals(key, "quality")) return m_st.quality;
    if (iequals(key, "audio.enabled")) return i(m_st.audio.enabled ? 1 : 0);
    if (iequals(key, "audio.device")) return m_st.audio.device;
    if (iequals(key, "audio.master")) return f(m_st.audio.master);
    if (iequals(key, "audio.music")) return f(m_st.audio.music);
    if (iequals(key, "audio.sfx")) return f(m_st.audio.sfx);
    if (iequals(key, "audio.voice")) return f(m_st.audio.voice);
    if (iequals(key, "audio.mute")) return i(m_st.audio.mute ? 1 : 0);
    if (iequals(key, "audio.muteOnFocusLost"))
        return i(m_st.audio.muteOnFocusLost ? 1 : 0);
    if (iequals(key, "audio.duckDepth")) return f(m_st.audio.duckDepth);
    if (iequals(key, "audio.maxVoices")) return i(m_st.audio.maxVoices);
    return std::string("?");
}

// ---------------------------------------------------------------- 绘制

void ProjectSettingsPanel::draw() {
    bool& open = panelOpenRef("projectsettings");
    if (!open) return;

    loadIfNeeded();

    ImGuiWindowFlags wf = 0;
    const bool docked = applyDockHint(wf);
    if (!docked) {
        const ImGuiIO& gio = ImGui::GetIO();
        ImGui::SetNextWindowSizeConstraints(ImVec2(520.0f, 380.0f),
                                            ImVec2(FLT_MAX, FLT_MAX));
        // 默认 720 高：通用 7 行 + 循环 3 行 + 本地化 1 行 + 画质 1 行 +
        // 音频 10 行 + 发布 3 行，全部展开刚好一屏（这一屏的意义就是
        // "不用滚就能看全一个工程的出厂设置"）。
        ImGui::SetNextWindowSize(ImVec2(588.0f, 720.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(gio.DisplaySize.x * 0.30f,
                                       gio.DisplaySize.y * 0.06f),
                                ImGuiCond_FirstUseEver);
    }

    // 标题带 `*` = 有未保存改动（`###` 之后是稳定 ID，显示名可以随便变）
    std::string title = TR("Project settings");
    if (m_dirty) title += " *";
    title += "###panel_projectsettings";

    if (!ImGui::Begin(title.c_str(), docked ? nullptr : &open, wf)) {
        ImGui::End();
        return;
    }
    ui::logRect("PROJ-RECT", ImGui::GetWindowPos(),
                ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                       ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    bool changed = false;

    // ---------------------------------------------------------- 文件 + 按钮
    ImGui::TextWrapped("%s", m_path.c_str());
    tagItem("PROJ-PATH");
    if (!m_err.empty()) {
        ImGui::TextWrapped("%s", m_err.c_str());
    } else if (m_dirty) {
        ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.28f, 1.0f), "%s",
                           TR("Unsaved changes"));
    } else {
        ImGui::TextDisabled("%s", TR("Saved"));
    }
    tagItem("PROJ-DIRTY");

    if (ImGui::Button(idLabel(TR("Save"), "##PROJ-SAVE").c_str()))
        save(nullptr);
    tagItem("PROJ-SAVE");
    ImGui::SameLine();
    if (ImGui::Button(idLabel(TR("Reload"), "##PROJ-RELOAD").c_str()))
        reload(nullptr);
    tagItem("PROJ-RELOAD");
    ImGui::SameLine();
    if (ImGui::Button(idLabel(TR("Quality settings..."),
                              "##PROJ-OPEN-QUALITY").c_str())) {
        // 真正的"渲染默认"在那张面板上（本面板的 quality 只是标签）。
        // 这个按钮就是为"用户改完标签想问'具体开了什么'"准备的。
        panelOpenRef("quality") = true;
    }
    tagItem("PROJ-OPEN-QUALITY");

    if (!m_note.empty()) ImGui::TextWrapped("%s", m_note.c_str());
    tagItem("PROJ-NOTE");

    // ---------------------------------------------------------- 通用
    if (ImGui::CollapsingHeader(idLabel(TR("General"), "##PROJ-SEC-GENERAL")
                                    .c_str(),
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextUnformatted(TR("Title"));
        ImGui::SameLine(190.0f);
        ImGui::SetNextItemWidth(340.0f);
        if (ImGui::InputText(idLabel("##v", "##PROJ-TITLE").c_str(), m_titleBuf,
                             sizeof(m_titleBuf)))
            changed = true;
        tagItem("PROJ-TITLE");

        ImGui::TextUnformatted(TR("Start scene"));
        ImGui::SameLine(190.0f);
        ImGui::SetNextItemWidth(340.0f);
        if (ImGui::InputText(idLabel("##v", "##PROJ-SCENE").c_str(), m_sceneBuf,
                             sizeof(m_sceneBuf)))
            changed = true;
        tagItem("PROJ-SCENE");

        ImGui::TextUnformatted(TR("UI document"));
        ImGui::SameLine(190.0f);
        ImGui::SetNextItemWidth(340.0f);
        if (ImGui::InputText(idLabel("##v", "##PROJ-UIDOC").c_str(), m_uiDocBuf,
                             sizeof(m_uiDocBuf)))
            changed = true;
        tagItem("PROJ-UIDOC");
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", TR("Empty = no UI layer. "
                                       "e.g. ui/docs/hud_demo.ui.json"));

        // 起始状态：两档就够（`mainmenu` 是用来验证"菜单是不透明状态"的）
        static const char* kStates[] = {"playing", "mainmenu"};
        int si = (m_st.startState == "mainmenu") ? 1 : 0;
        ImGui::TextUnformatted(TR("Start state"));
        ImGui::SameLine(190.0f);
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::Combo(idLabel("##v", "##PROJ-STARTSTATE").c_str(), &si,
                         kStates, 2)) {
            m_st.startState = kStates[si];
            changed = true;
        }
        tagItem("PROJ-STARTSTATE");

        changed |= intRow(TR("Width"), "PROJ-WIDTH", "PROJ-DEC-WIDTH",
                          "PROJ-INC-WIDTH", m_st.width, 16, 320, 7680);
        changed |= intRow(TR("Height"), "PROJ-HEIGHT", "PROJ-DEC-HEIGHT",
                          "PROJ-INC-HEIGHT", m_st.height, 16, 320, 7680);
        changed |= boolRow(TR("Fullscreen"), "PROJ-FULLSCREEN",
                           m_st.fullscreen);
    }

    // ---------------------------------------------------------- 循环
    if (ImGui::CollapsingHeader(idLabel(TR("Loop"), "##PROJ-SEC-LOOP").c_str(),
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= intRow(TR("Max FPS"), "PROJ-MAXFPS", "PROJ-DEC-MAXFPS",
                          "PROJ-INC-MAXFPS", m_st.maxFps, 10, 0, 1000);
        ImGui::TextDisabled("%s", TR("0 = uncapped"));
        changed |= intRow(TR("Unfocused FPS"), "PROJ-UNFOCUSFPS",
                          "PROJ-DEC-UNFOCUSFPS", "PROJ-INC-UNFOCUSFPS",
                          m_st.unfocusedFps, 5, 0, 1000);
        changed |= floatRow(TR("Time scale"), "PROJ-TIMESCALE", m_st.timeScale,
                            0.01f, 10.0f);
    }

    // ---------------------------------------------------------- 本地化
    if (ImGui::CollapsingHeader(idLabel(TR("Localization"),
                                        "##PROJ-SEC-LOCALE").c_str(),
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        int n = 0;
        const char* const* langs = project::languageChoices(&n);
        bool isAuto = false;
        const std::string cur = project::normalizeLanguage(m_st.language, &isAuto);
        int li = 0;
        for (int i = 0; i < n; ++i) {
            if ((isAuto && std::strcmp(langs[i], "auto") == 0) ||
                (!isAuto && cur == langs[i]))
                li = i;
        }
        ImGui::TextUnformatted(TR("Language"));
        ImGui::SameLine(190.0f);
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::Combo(idLabel("##v", "##PROJ-LANG").c_str(), &li, langs, n)) {
            m_st.language = project::normalizeLanguage(langs[li]);
            changed = true;
        }
        tagItem("PROJ-LANG");
        ImGui::TextDisabled("%s", TR("auto = follow the system language"));
    }

    // ---------------------------------------------------------- 玩家外观
    //
    // 2026-11-03：这一段补的是"让一个角色走起来"路径上**唯一必须手改 JSON**
    // 的地方。在此之前 `player` 块有 25 个字段、面板一个都不编辑，于是
    // "新工程能看到自己的角色"这件事要求作者知道 `assets/game.json` 在哪、
    // 键叫什么、路径相对谁。
    //
    // ⚠ 只暴露六项（见 GamePlayer 的注释），其余靠合并写回原样保留。
    // ⚠ 首次打开（没配过角色）时这一段**默认折叠**：不配角色的工程
    //   （卡牌 / 纯 UI 演示）不该被一段用不上的控件占掉半屏。
    if (ImGui::CollapsingHeader(
            idLabel(TR("Player"), "##PROJ-SEC-PLAYER").c_str(),
            m_st.player.present ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
        if (!m_choicesScanned) {
            namespace fs = std::filesystem;
            // `m_path` = `<project>/assets/game.json` → 资产根是它的父目录。
            const std::string assetsRoot =
                fs::path(m_path).parent_path().generic_string();
            m_modelChoices = scanAssets(assetsRoot, "models", ".vmesh");
            m_animChoices = scanAssets(assetsRoot, "animators", ".anim.json");
            m_choicesScanned = true;
        }

        // ---- 模型 ----
        ImGui::TextUnformatted(TR("Character model"));
        ImGui::SameLine(190.0f);
        ImGui::SetNextItemWidth(280.0f);
        if (ImGui::InputText(idLabel("##v", "##PROJ-PLAYER-MODEL").c_str(),
                             m_modelBuf, sizeof(m_modelBuf)))
            changed = true;
        tagItem("PROJ-PLAYER-MODEL");
        ImGui::SameLine();
        if (ImGui::Button(idLabel(TR("Pick..."), "##PROJ-PLAYER-PICK").c_str()))
            ImGui::OpenPopup("##PROJ-PLAYER-POPUP");
        tagItem("PROJ-PLAYER-PICK");
        if (ImGui::BeginPopup("##PROJ-PLAYER-POPUP")) {
            if (m_modelChoices.empty()) {
                ImGui::TextDisabled("%s", TR("No .vmesh under assets/models"));
            }
            // 第 0 项 = 清空（回到胶囊占位体）。**必须**有这一项，否则
            // "把角色撤掉"只能靠手改 JSON —— 又回到这条路要消灭的问题。
            if (ImGui::Selectable(TR("(placeholder capsule)"))) {
                std::snprintf(m_modelBuf, sizeof(m_modelBuf), "%s", "");
                changed = true;
            }
            for (const std::string& m : m_modelChoices) {
                if (ImGui::Selectable(m.c_str())) {
                    std::snprintf(m_modelBuf, sizeof(m_modelBuf), "%s",
                                  m.c_str());
                    changed = true;
                }
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", TR("(empty = capsule placeholder)"));

        // ---- 动画器 ----
        ImGui::TextUnformatted(TR("Animator"));
        ImGui::SameLine(190.0f);
        ImGui::SetNextItemWidth(280.0f);
        if (ImGui::InputText(idLabel("##v", "##PROJ-PLAYER-CTRL").c_str(),
                             m_ctrlBuf, sizeof(m_ctrlBuf)))
            changed = true;
        tagItem("PROJ-PLAYER-CTRL");
        ImGui::SameLine();
        if (ImGui::Button(idLabel(TR("Pick..."), "##PROJ-PLAYER-CTRLPICK")
                              .c_str()))
            ImGui::OpenPopup("##PROJ-PLAYER-CTRLPOP");
        tagItem("PROJ-PLAYER-CTRLPICK");
        if (ImGui::BeginPopup("##PROJ-PLAYER-CTRLPOP")) {
            if (ImGui::Selectable(TR("(four-state hardcoded)"))) {
                std::snprintf(m_ctrlBuf, sizeof(m_ctrlBuf), "%s", "");
                changed = true;
            }
            if (m_animChoices.empty()) {
                ImGui::TextDisabled("%s", TR("No .anim.json under assets/animators"));
            }
            for (const std::string& a : m_animChoices) {
                if (ImGui::Selectable(a.c_str())) {
                    std::snprintf(m_ctrlBuf, sizeof(m_ctrlBuf), "%s", a.c_str());
                    changed = true;
                }
            }
            ImGui::EndPopup();
        }

        changed |= boolRow(TR("Fit body"), "PROJ-PLAYER-FITBODY",
                           m_st.player.fitBody);

        // ---- 四段 clip 名 ----
        // 只在**没有**动画器时生效（控制器接管后"哪段动画归哪个状态"
        // 写在控制器里）。灰掉比藏掉好：作者能看到"这里本来有四个名字"。
        ImGui::BeginDisabled(!m_st.player.controller.empty());
        ImGui::TextUnformatted(TR("Idle / Walk / Run / Jump clips"));
        ImGui::SameLine(190.0f);
        ImGui::SetNextItemWidth(84.0f);
        if (ImGui::InputText(idLabel("##v", "##PROJ-PLAYER-IDLE").c_str(),
                             m_idleBuf, sizeof(m_idleBuf)))
            changed = true;
        tagItem("PROJ-PLAYER-IDLE");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(84.0f);
        if (ImGui::InputText(idLabel("##v", "##PROJ-PLAYER-WALK").c_str(),
                             m_walkBuf, sizeof(m_walkBuf)))
            changed = true;
        tagItem("PROJ-PLAYER-WALK");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(84.0f);
        if (ImGui::InputText(idLabel("##v", "##PROJ-PLAYER-RUN").c_str(),
                             m_runBuf, sizeof(m_runBuf)))
            changed = true;
        tagItem("PROJ-PLAYER-RUN");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(84.0f);
        if (ImGui::InputText(idLabel("##v", "##PROJ-PLAYER-JUMP").c_str(),
                             m_jumpBuf, sizeof(m_jumpBuf)))
            changed = true;
        tagItem("PROJ-PLAYER-JUMP");
        ImGui::EndDisabled();
        if (!m_st.player.controller.empty())
            ImGui::TextDisabled("%s",
                                TR("Ignored: the animator decides which clip "
                                   "belongs to which state."));

        ImGui::TextWrapped("%s",
                           TR("The player is spawned from the scene's GameStart "
                              "point at play time. Paths are relative to the "
                              "project's assets folder."));
        tagItem("PROJ-PLAYER-NOTE");
    }

    // ---------------------------------------------------------- 画质标签
    if (ImGui::CollapsingHeader(idLabel(TR("Render defaults"),
                                        "##PROJ-SEC-RENDER").c_str(),
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        const int pc = render::qualityPresetCount();
        // 选项：第 0 项是"保持原值"，后面是五个档名。用"保持"而不是把
        // 未知值硬塞成 Low —— 与 QualitySettingsPanel 的 Custom 同一条纪律：
        // 标签与实际值不一致时**不反向纠正**。
        const char* items[render::kQualityPresetMax + 1];
        items[0] = TR("(unchanged)");
        for (int i = 0; i < pc; ++i) items[i + 1] = render::qualityPresetName(i);
        int qi = 0;
        for (int i = 0; i < pc; ++i) {
            if (iequals(m_st.quality, render::qualityPresetName(i))) qi = i + 1;
        }
        ImGui::TextUnformatted(TR("Quality tier"));
        ImGui::SameLine(190.0f);
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::Combo(idLabel("##v", "##PROJ-QUALITY").c_str(), &qi, items,
                         pc + 1)) {
            if (qi > 0) {
                m_st.quality = render::qualityPresetName(qi - 1);
                changed = true;
            }
        }
        tagItem("PROJ-QUALITY");
        ImGui::TextDisabled("%s", m_st.quality.c_str());
        ImGui::TextWrapped("%s",
                           TR("This is only a label (it is logged, not applied). "
                              "The real render values live in config/render.json — "
                              "edit them in Quality settings."));
        tagItem("PROJ-RENDER-NOTE");
    }

    // ---------------------------------------------------------- 音频出厂值
    if (ImGui::CollapsingHeader(idLabel(TR("Audio (factory defaults)"),
                                        "##PROJ-SEC-AUDIO").c_str(),
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        // ⚠ 这一段与混音台**不是**同一件事：这里写的是"下次启动时初始成
        //   多少"（落盘），混音台拖的是**现在**的增益（不落盘）。两条都动
        //   "master"，不说清楚的话用户会以为混音台在改工程配置。
        ImGui::TextWrapped("%s",
                           TR("These values are written to game.json and applied "
                              "at the next launch. The Audio mixer panel changes "
                              "the live mix instead (and never saves)."));
        changed |= boolRow(TR("Enabled"), "PROJ-AUDIO-ENABLED",
                           m_st.audio.enabled);
        ImGui::TextUnformatted(TR("Device"));
        ImGui::SameLine(190.0f);
        ImGui::SetNextItemWidth(340.0f);
        if (ImGui::InputText(idLabel("##v", "##PROJ-AUDIO-DEVICE").c_str(),
                             m_deviceBuf, sizeof(m_deviceBuf)))
            changed = true;
        tagItem("PROJ-AUDIO-DEVICE");
        changed |= volumeRow(TR("Master"), "PROJ-AUDIO-MASTER",
                             m_st.audio.master);
        changed |= volumeRow(TR("Music"), "PROJ-AUDIO-MUSIC", m_st.audio.music);
        changed |= volumeRow(TR("SFX"), "PROJ-AUDIO-SFX", m_st.audio.sfx);
        changed |= volumeRow(TR("Voice"), "PROJ-AUDIO-VOICE", m_st.audio.voice);
        changed |= boolRow(TR("Mute"), "PROJ-AUDIO-MUTE", m_st.audio.mute);
        changed |= boolRow(TR("Mute when unfocused"), "PROJ-AUDIO-MUTEFOCUS",
                           m_st.audio.muteOnFocusLost);
        changed |= volumeRow(TR("Duck depth"), "PROJ-AUDIO-DUCK",
                             m_st.audio.duckDepth);
        changed |= intRow(TR("Max voices"), "PROJ-AUDIO-MAXVOICES",
                          "PROJ-DEC-VOICES", "PROJ-INC-VOICES",
                          m_st.audio.maxVoices, 8, 4, 256);
    }

    // ---------------------------------------------------------- 发布
    if (ImGui::CollapsingHeader(idLabel(TR("Publish"), "##PROJ-SEC-PUBLISH")
                                    .c_str())) {
        // 只读摘要：把"发布时会被用到的东西"摆出来，而不是再给一堆输入框。
        // 真正的出包是 File ▸ Package（E28/G29），这里不重复实现。
        ImGui::TextWrapped("%s", TR("Dist folder (File > Package writes here):"));
        ImGui::SameLine();
        const std::string dist =
            std::string("dist/") + (m_st.title.empty() ? "(title)" : m_st.title);
        ImGui::TextDisabled("%s", dist.c_str());
        ImGui::TextWrapped("%s", TR("Engine assets (shaders / fonts / ui / LUT) are "
                                    "packed automatically — they live next to the "
                                    "exe, not in the project."));
        tagItem("PROJ-PUBLISH");
    }

    // ---------------------------------------------------------- 收尾
    if (changed) {
        // 把输入缓冲写回模型（改了值的行才需要，但写全是幂等的）
        m_st.title = m_titleBuf;
        m_st.scene = m_sceneBuf;
        m_st.uiDoc = m_uiDocBuf;
        m_st.audio.device = m_deviceBuf;
        // 玩家外观：**任何一项被改就置 present**。这不是"顺手"，它表达的
        // 是"这个工程现在有角色配置了"—— 不置的话面板改了、存盘却没写出去，
        // 症状是"我明明选了模型，重新打开面板又变回空的"。
        if (m_st.player.model != m_modelBuf ||
            m_st.player.controller != m_ctrlBuf ||
            m_st.player.idleClip != m_idleBuf ||
            m_st.player.walkClip != m_walkBuf ||
            m_st.player.runClip != m_runBuf ||
            m_st.player.jumpClip != m_jumpBuf) {
            m_st.player.present = true;
        }
        m_st.player.model = m_modelBuf;
        m_st.player.controller = m_ctrlBuf;
        m_st.player.idleClip = m_idleBuf;
        m_st.player.walkClip = m_walkBuf;
        m_st.player.runClip = m_runBuf;
        m_st.player.jumpClip = m_jumpBuf;
        m_dirty = !sameSettings(m_st, m_saved);
    }

    ImGui::End();
}

} // namespace editor
