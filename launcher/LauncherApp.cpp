#include "LauncherApp.h"

#include "LauncherDialog.h"
#include "LauncherSpawn.h"

#include "assets/AssetPath.h"
#include "assets/Texture.h"
#include "core/Logger.h"
#include "core/Version.h"
#include "core/Window.h"
#include "i18n/I18n.h"
#include "render/QualityPresets.h"
#include "ui/DebugRects.h"

#include <backends/imgui_impl_vulkan.h>
#include <imgui.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <shellapi.h>
#endif

namespace launcher {

namespace {

using project::GameType;

// ---- 尺寸（路线图 §5.1；2026-10-20 按用户要求定为 1080×720，最小 900×560）----
// 1080×720 是 16:9，与类型卡片里的 16:9 插图同比例 —— 窗口整体看起来
// 就是"一张图 + 一条边"，不会像 1280×800（16:10）那样上下留白。
// 卡片宽度是自适应的（见 createWizard 里的 cardW 计算），改这里不用动布局。
constexpr int kWindowW = 1080;
constexpr int kWindowH = 720;
constexpr float kHeaderH = 54.0f;
constexpr float kFooterH = 56.0f;
constexpr float kLeftW = 330.0f;

constexpr float kToastSeconds = 6.0f;

const char* TR(const char* en) { return i18n::tr(en); }

// 控件 label 专用：`"Browse...##code"` → 只翻 `##` 前面那半截，后缀原样拼回。
//
// ⚠ 必须走这一条而不是 `TR()`：`tr()` 是**整串精确查表**，带 `##` 的原文
//   查不到 → 静默回退英文。而"同一窗口里两个 Browse... 按钮"必须靠后缀
//   区分 ImGui ID（不加就撞 ID：点左边那个实际触发右边那个）。
//   与 `editor::trLabel` 同一套语义 —— 那个在 editor/ 里，启动器不许依赖
//   它，所以这里有一份等价实现（10 行，换依赖方向不变）。
const char* TRL(const char* label) {
    static std::string buf;
    const char* sep = std::strstr(label, "##");
    if (!sep) return i18n::tr(label);
    const std::string head(label, static_cast<size_t>(sep - label));
    buf.assign(i18n::tr(head.c_str()));
    buf += sep;
    return buf.c_str();
}

// 控件矩形锚点（`MYVK_LOG_RECTS=1` 时进日志）。给 verify 脚本点控件用。
void anchor(const char* tag) {
    ui::logRect(tag, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

void anchorf(const char* fmt, int a) {
    char tag[64];
    std::snprintf(tag, sizeof(tag), fmt, a);
    anchor(tag);
}

// 分辨率预设表。`自定义` 用两个输入框。
struct ResPreset {
    int w, h;
    const char* label;   // 英文原文（过 TR）
};
const ResPreset kResPresets[] = {
    {1280, 720, "1280x720"},
    {1920, 1080, "1920x1080"},
    {2560, 1440, "2560x1440"},
    {0, 0, "Custom"},
};
constexpr int kResCount = static_cast<int>(sizeof(kResPresets) / sizeof(kResPresets[0]));

// 相对时间（"2 min ago"）。⚠ 只做**展示**：列表排序用 ISO 串本身，
// 不解析这个。解析相对时间在跨时区/夏令时上必错，而且错了没人发现。
//
// ⚠ `timegm` 在 MSVC 上叫 `_mkgmtime64`，且返回 -1 表示失败。非 Windows
//   直接返回空串 —— 这个函数只影响"最近列表右侧那行灰字"，不该为它引入
//   平台分支的复杂度。
std::string humanAge(const std::string& iso) {
#if defined(_WIN32)
    if (iso.size() < 19) return std::string();
    std::tm t{};
    if (std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &t.tm_year, &t.tm_mon,
                    &t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec) != 6)
        return std::string();
    t.tm_year -= 1900;
    t.tm_mon -= 1;
    const __time64_t then = ::_mkgmtime64(&t);
    if (then == -1) return std::string();
    const double diff = std::difftime(std::time(nullptr), then);
    if (diff < 0) return std::string();
    char b[32];
    if (diff < 3600)
        std::snprintf(b, sizeof(b), "%d min ago", static_cast<int>(diff / 60));
    else if (diff < 86400)
        std::snprintf(b, sizeof(b), "%d h ago", static_cast<int>(diff / 3600));
    else
        std::snprintf(b, sizeof(b), "%d d ago", static_cast<int>(diff / 86400));
    return b;
#else
    (void)iso;
    return std::string();
#endif
}

bool revealInExplorer(const std::string& path) {
#if defined(_WIN32)
    // `explorer /select,` 在路径不存在时也会"成功"返回，所以先判存在。
    if (!dirExists(path)) return false;
    std::wstring w(path.begin(), path.end());
    const HINSTANCE r =
        ::ShellExecuteW(nullptr, L"open", L"explorer.exe", w.c_str(), nullptr,
                        SW_SHOWNORMAL);
    return reinterpret_cast<std::intptr_t>(r) > 32;
#else
    (void)path;
    return false;
#endif
}

std::string envStr(const char* name) {
    const char* v = std::getenv(name);
    return (v && v[0]) ? std::string(v) : std::string();
}

// ============================================================
// 新建向导的"上次目录"（2026-10-24）
// ============================================================
// 用户原话："创建新游戏会设置游戏目录时不要每次都是 C 盘的目录，如果之前
// 有创建过的游戏目录，默认会以之前的目录为准。"
//
// 上一版每次启动都把父目录重置成 `%USERPROFILE%/Documents` —— 那在装了
// 系统盘的机器上就是 `C:/Users/xxx/Documents`，于是每次新建游戏都得从 C 盘
// 一路点到 D 盘的游戏目录。
//
// 现在记在 `newproject.json`（与 recent.json **同一个目录**）里：
//   · 和 recent.json 同目录是有意的 —— 自动化用 `MYVK_LAUNCHER_RECENT`
//     重定向列表时，这个文件跟着一起被隔离，不会污染用户真实配置；
//   · 坏文件 / 目录已被删掉 → 一律当成"没记过"，回退到"文档"，
//     启动器不该因为一个手改坏的 json 打不开。
std::string wizardStatePath() {
    const std::filesystem::path rec(project::recentListPath());
    return (rec.parent_path() / "newproject.json").generic_string();
}

std::string loadLastWizardParent() {
    const std::string path = wizardStatePath();
    std::error_code ec;
    if (!std::filesystem::is_regular_file(std::filesystem::path(path), ec))
        return {};
    try {
        std::ifstream f(path);
        if (!f) return {};
        nlohmann::json j;
        f >> j;
        if (!j.is_object() || !j.contains("parent") || !j["parent"].is_string())
            return {};
        const std::string p =
            project::normalizePath(j["parent"].get<std::string>());
        // 目录已经不在了就别用（用户可能删了/换了盘符）—— 让调用方回退。
        if (p.empty() || !dirExists(p)) {
            VK_LOG_INFO("LAUNCHER-WIZARD last-parent stale '%s' (ignored)",
                        p.c_str());
            return {};
        }
        return p;
    } catch (const std::exception& e) {
        VK_LOG_WARN("LAUNCHER-WIZARD last-parent unreadable (%s)", e.what());
        return {};
    }
}

void saveLastWizardParent(const std::string& parent) {
    if (parent.empty()) return;
    const std::string path = wizardStatePath();
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path(), ec);
    if (ec) {
        VK_LOG_WARN("LAUNCHER-WIZARD cannot create state dir: %s",
                    ec.message().c_str());
        return;
    }
    nlohmann::json j;
    j["parent"] = project::normalizePath(parent);
    std::ofstream f(path);
    if (!f) {
        VK_LOG_WARN("LAUNCHER-WIZARD cannot write %s", path.c_str());
        return;
    }
    f << j.dump(2) << "\n";
    VK_LOG_INFO("LAUNCHER-WIZARD last-parent saved=%s", parent.c_str());
}

// 兜底链里"最近工程所在目录"那一档：recents 里第一条还在盘上的工程，
// 取它的父目录。`newproject.json` 不存在时（老用户第一次升级）靠它接上。
std::string lastRecentParent(const project::RecentList& rec) {
    for (const auto& it : rec.items()) {
        if (it.path.empty()) continue;
        const std::string parent =
            std::filesystem::path(it.path).parent_path().generic_string();
        if (!parent.empty() && dirExists(parent)) return parent;
    }
    return {};
}

} // namespace

// ============================================================
// 构造 / 初始化
// ============================================================

LauncherApp::LauncherApp()
    : core::Application("Venn Launcher", kWindowW, kWindowH) {}

LauncherApp::~LauncherApp() = default;

void LauncherApp::onInit() {
    // 窗口：居中 + 不可小于 900×560。
    //
    // ⚠ 自动化跑（MYVK_FRAMES / MYVK_NO_WINDOW_SAVE / MYVK_LOG_RECTS）
    //   **照样居中**：启动器的布局是"一屏两栏"，脚本要靠锚点算点击位置，
    //   窗口在任意位置都不影响（锚点是客户区坐标）。而"恢复上次几何"才是
    //   不该做的 —— 那会让脚本拿到一个 3840×2054 全屏窗口，第一帧的
    //   两栏宽度全错。所以这里只做居中，不读任何存档。
    int wx = 0, wy = 0, ww = 0, wh = 0;
    if (window().primaryMonitorWorkArea(&wx, &wy, &ww, &wh)) {
        const int cx = wx + (ww - kWindowW) / 2;
        const int cy = wy + (wh - kWindowH) / 2;
        window().setWindowPos(cx, cy);
    }

    // 最近列表。⚠ 坏文件不崩（RecentList 内部已兜住，这里只看返回值决定
    // 要不要多说一句）。
    std::string rerr;
    if (!m_recent.load(&rerr)) {
        VK_LOG_WARN("LAUNCHER-RECENT load failed (treated as empty): %s",
                    rerr.c_str());
    }
    const bool noPrune = envStr("MYVK_LAUNCHER_NO_PRUNE") == "1";
    refreshRecents(!noPrune);
    VK_LOG_INFO("LAUNCHER-RECENT path=%s count=%zu",
                project::recentListPath().c_str(), m_recent.size());

    // 向导初值。`MYVK_LAUNCHER_TEST_ROOT` 只填 Step2 的父目录 ——
    // 四步向导仍然要一步不少地走完（见头文件的说明）。
    if (const std::string r = envStr("MYVK_LAUNCHER_TEST_ROOT"); !r.empty())
        m_wizParent = r;
    if (const std::string n = envStr("MYVK_LAUNCHER_TEST_NAME"); !n.empty())
        m_wizName = n;
    if (m_wizParent.empty()) {
        // 优先"上次用过的目录"（2026-10-24）。三级兜底，全都不命中才落到
        // "文档"—— 而"文档"这一档也只在拿得到 USERPROFILE 时用，绝不写 `C:/`。
        m_wizParent = loadLastWizardParent();
        if (!m_wizParent.empty()) {
            VK_LOG_INFO("LAUNCHER-WIZARD dir-default=%s source=saved",
                        m_wizParent.c_str());
        } else {
            m_wizParent = lastRecentParent(m_recent);
            if (!m_wizParent.empty()) {
                VK_LOG_INFO("LAUNCHER-WIZARD dir-default=%s source=recent",
                            m_wizParent.c_str());
            }
        }
        if (m_wizParent.empty()) {
            const char* home = std::getenv("USERPROFILE");
            m_wizParent = home && home[0] ? std::string(home) + "/Documents"
                                          : std::string(".");
            VK_LOG_INFO("LAUNCHER-WIZARD dir-default=%s source=documents",
                        m_wizParent.c_str());
        }
    }
    syncWizardRoot();

    // 分辨率默认值：**推荐当前电脑显示设备的分辨率**（2026-10-23）。
    //   · 桌面分辨率恰好是某个预设 → 直接选中那个预设；
    //   · 不是（比如 2880×1800）→ 选中"自定义"并把两个输入框预填成桌面值，
    //     用户要么一键确认要么自己改，不用再手抄一遍自己屏幕的分辨率。
    // ⚠ 放在 MYVK_LAUNCHER_TEST_RES **之前**：环境变量是自动化入口，
    //   显式指定必须赢过"猜默认"，所以这里只填初值、后面 env 再覆盖。
    {
        int dw = 0, dh = 0;
        if (window().primaryMonitorResolution(&dw, &dh)) {
            int preset = -1;
            for (int i = 0; i < kResCount; ++i)
                if (kResPresets[i].w == dw && kResPresets[i].h == dh) {
                    preset = i;
                    break;
                }
            if (preset >= 0) {
                m_wizRes = preset;
            } else {
                m_wizRes = kResCount - 1;   // Custom
                m_wizCustomW = dw;
                m_wizCustomH = dh;
            }
            VK_LOG_INFO("LAUNCHER-WIZARD res-default=%dx%d preset=%d", dw, dh,
                        m_wizRes);
        }
    }

    if (envStr("MYVK_LAUNCHER_PAGE") == "new") beginWizard();

    // 画面初值（M4 验收用）。⚠ 同样只是**初值**：下拉框照样可以点开换。
    //   不加这两个口子的话，自动化得去点 Combo 的弹出项 —— 而弹出项没有
    //   锚点（它们在 ImGui 的 popup 窗口里，位置随内容浮动），等于没法稳定点。
    if (const std::string s = envStr("MYVK_LAUNCHER_TEST_RES"); !s.empty()) {
        const int v = std::atoi(s.c_str());
        if (v >= 0 && v < kResCount) m_wizRes = v;
        else VK_LOG_WARN("MYVK_LAUNCHER_TEST_RES=%s out of range (%d)", s.c_str(),
                         kResCount);
    }
    if (const std::string s = envStr("MYVK_LAUNCHER_TEST_QUALITY"); !s.empty()) {
        const int v = std::atoi(s.c_str());
        if (v >= 0 && v < render::qualityPresetCount()) m_wizQuality = v;
        else VK_LOG_WARN("MYVK_LAUNCHER_TEST_QUALITY=%s out of range (%d)",
                         s.c_str(), render::qualityPresetCount());
    }
    // 外置代码根（M5-b 验收用）：**只填路径，不代勾**。
    // ⚠ 复选框那一下必须由测试真的点 —— 勾选是这条特性的入口，代勾等于
    //   把被测行为（"勾上之后确实走了外置路径"）从测试里删掉了。
    //   而"路径"没法点：那是一串带盘符的临时目录，只能靠这个初值送进去。
    if (const std::string c = envStr("MYVK_LAUNCHER_TEST_CODE_EXTERNAL");
        !c.empty()) {
        m_wizCodeRoot = c;
        VK_LOG_INFO("LAUNCHER-WIZARD code-root-seeded=%s", c.c_str());
    }

    VK_LOG_INFO("LAUNCHER-READY version=%s gpu=%s", core::kVennVersion,
                renderer().device().properties().deviceName);
    m_initialized = true;
}

void LauncherApp::onShutdown() {
    // 插图句柄必须在 ImGui 后端 shutdown **之前**注销：后端没了再去
    // RemoveTexture 就是释放后使用。onShutdown 是最后一个安全点
    // （Application 在这之后才 waitIdle + 析构 Renderer）。
    for (std::uint64_t& ds : m_typeArt) {
        if (ds != 0) {
            ImGui_ImplVulkan_RemoveTexture(reinterpret_cast<VkDescriptorSet>(ds));
            ds = 0;
        }
    }

    // 列表在**每个**改动点即时保存（见 remove / touch 的调用点），
    // 这里只补一次兜底 —— 异常退出路径上不至于把整次会话丢掉。
    std::string err;
    if (!m_recent.save(&err))
        VK_LOG_WARN("LAUNCHER-RECENT save failed: %s", err.c_str());
}

void LauncherApp::refreshRecents(bool prune) {
    if (prune) {
        const int n = m_recent.pruneMissing();
        if (n > 0) VK_LOG_INFO("LAUNCHER-RECENT pruned=%d", n);
    }
    // 选中项尽量保持在同一个工程上（列表可能刚被剔除过）。
    const std::string keep =
        (m_selected >= 0 && m_selected < static_cast<int>(m_recent.items().size()))
            ? m_recent.items()[static_cast<size_t>(m_selected)].path
            : std::string();
    m_selected = m_recent.empty() ? -1 : 0;
    if (!keep.empty()) {
        for (int i = 0; i < static_cast<int>(m_recent.size()); ++i)
            if (m_recent.items()[static_cast<size_t>(i)].path == keep) {
                m_selected = i;
                break;
            }
    }
    m_detailOk = false;
    m_detail = project::ProjectFile{};
    m_detailError.clear();
    if (m_selected >= 0) {
        const std::string root =
            m_recent.items()[static_cast<size_t>(m_selected)].path;
        m_detailOk = project::loadProject(root, m_detail, &m_detailError).ok;
        if (!m_detailOk)
            VK_LOG_WARN("LAUNCHER-RECENT unreadable entry '%s': %s",
                        root.c_str(), m_detailError.c_str());
    }
}

// ============================================================
// 每帧
// ============================================================

void LauncherApp::onUpdate(float dt) {
    m_now += static_cast<double>(dt);
    // 到期的 toast 扔掉（每帧只做一次线性扫描，条数上限几条）。
    for (size_t i = 0; i < m_toasts.size();) {
        if (m_toasts[i].until <= m_now) m_toasts.erase(m_toasts.begin() + i);
        else ++i;
    }
    // 拉起子进程放在这里（onUpdate 早于 onImGui）：动作发生在**上一帧**
    // 的点击之后，本帧不再画任何东西（进程也不退出 —— 它会收进任务栏）。
    if (m_pendingSpawn != SpawnKind::None) runPendingSpawn();

    // 从任务栏回来：只打一行日志。这一行也是自动化判断"最小化之后还能
    // 被点回来"的唯一锚点（iconify 状态下主循环根本不进这里）。
    if (m_minimizedAfterSpawn && !window().iconified()) {
        m_minimizedAfterSpawn = false;
        VK_LOG_INFO("LAUNCHER-RESTORED");
    }
}

void LauncherApp::toast(const std::string& text, bool error) {
    Toast t;
    t.text = text;
    t.error = error;
    t.until = m_now + kToastSeconds;
    m_toasts.push_back(t);
    // 最多同时显示 3 条，老的先走。
    while (m_toasts.size() > 3) m_toasts.erase(m_toasts.begin());
    if (error) VK_LOG_WARN("LAUNCHER-TOAST error: %s", text.c_str());
    else VK_LOG_INFO("LAUNCHER-TOAST %s", text.c_str());
}

// ============================================================
// 向导
// ============================================================

void LauncherApp::beginWizard() {
    m_page = Page::Type;
    m_wizType = project::GameType::ThirdPerson;
    m_wizRootTouched = false;
    m_create = project::CreateResult{};
    m_createTrace.clear();
    syncWizardRoot();
    VK_LOG_INFO("LAUNCHER-WIZARD page=type");
}

void LauncherApp::cancelWizard() {
    m_page = Page::Idle;
    VK_LOG_INFO("LAUNCHER-WIZARD page=idle (cancelled)");
}

void LauncherApp::syncWizardRoot() {
    // 工程名 → 目录名。⚠ 用 `sanitizeName` 算建议值，但**用户看到的输入框**
    // 保留原始名字 —— 否则打一个空格就发现名字被悄悄改掉了。
    const std::string dir = project::sanitizeName(m_wizName);
    m_wizRoot = project::normalizePath(m_wizParent + "/" + dir);
}

void LauncherApp::commitWizard() {
    project::CreateSpec spec;
    spec.gameType = m_wizType;
    spec.templateId = project::gameTypeId(m_wizType);
    spec.name = m_wizName;
    spec.root = m_wizRoot;
    spec.codeExternal = m_wizExternal;
    spec.externalCodeRoot = m_wizCodeRoot;
    spec.display.width = (m_wizRes == kResCount - 1) ? m_wizCustomW
                                                     : kResPresets[m_wizRes].w;
    spec.display.height = (m_wizRes == kResCount - 1) ? m_wizCustomH
                                                      : kResPresets[m_wizRes].h;
    spec.display.fullscreen = m_wizFullscreen;
    spec.display.title = m_wizName;
    spec.display.qualityPreset = m_wizQuality;

    m_createTrace.clear();
    m_create = project::createFromTemplate(
        spec, [this](project::CreateStep step, const char* label) {
            char b[256];
            std::snprintf(b, sizeof(b), "%d. %s", static_cast<int>(step) + 1,
                          label ? label : "");
            m_createTrace.push_back(b);
            VK_LOG_INFO("LAUNCHER-WIZARD step=%s", b);
        });

    if (m_create.ok) {
        m_page = Page::Done;
        // 记住这次的父目录：下次"新建游戏"直接落在这里（2026-10-24）。
        // ⚠ 只记**成功**的那一次：建失败时用户选的目录往往还不存在，
        //   记下来等于下次开局就给他一个无效路径。
        saveLastWizardParent(
            std::filesystem::path(m_create.root).parent_path().generic_string());
        // ⚠⚠ 必须**重新读盘**，不能只 `refreshRecents(false)`。
        //   `createFromTemplate` 的第 5 步会自己往 recents 里追加一条
        //   （路线图 §7-M2 的明文要求），而启动器内存里那份是**启动时**读的。
        //   不重读的后果：`onShutdown` 的 `save()` 会用旧列表把刚追加的那条
        //   **覆盖掉** —— 现象是"工程建好了、列表里没有"，而且没有任何报错。
        //   （这是"两份数据漂移"的又一次变形：两个写者 + 一份内存副本。）
        m_recent.load(nullptr);
        refreshRecents(false);
        // 选中刚建的那个（列表里它已经在最前面）
        for (int i = 0; i < static_cast<int>(m_recent.size()); ++i)
            if (m_recent.items()[static_cast<size_t>(i)].path == m_create.root) {
                m_selected = i;
                break;
            }
        VK_LOG_INFO("LAUNCHER-WIZARD created root=%s files=%d", m_create.root.c_str(),
                    m_create.filesCopied);

        // ---- 建完**直接进编辑器**（2026-10-22 用户要求）----
        // 用户原话："当操作到第三步的时候点击创建的时候，就直接进入引擎编辑器
        // 即可，不需要其他的操作。"
        //
        // 以前这里停在 `Page::Done`（一张"建好了 / 写了几个文件 / 五个步骤"
        // 的收尾页），要再点 footer 的"Open editor"才拉编辑器。
        //
        // ⚠ `Page::Done` **仍然要设**：`runPendingSpawn` 是下一帧的事，
        //   而它可能失败（exe 不在、工程读不出来）。失败时停在收尾页上
        //   还能点"Open editor"重试，不会退回到一个没有任何入口的空白。
        requestSpawn(SpawnKind::Editor, m_create.root);
    } else {
        // **留在 Step3**，错误行就地显示（路线图 §5.3 Step4：不弹模态）。
        m_page = Page::Display;
        toast(m_create.error, true);
        VK_LOG_WARN("LAUNCHER-WIZARD failed at %s: %s",
                    project::createStepLabel(m_create.failedAt),
                    m_create.error.c_str());
    }
}

// ============================================================
// 拉起
// ============================================================

void LauncherApp::openProjectAt(const std::string& root) {
    project::ProjectFile pf;
    std::string err;
    if (!project::loadProject(root, pf, &err)) {
        toast(err, true);
        return;
    }
    requestSpawn(SpawnKind::Editor, pf.root);
}

void LauncherApp::openSelected() {
    if (m_selected < 0) {
        toast(TR("Select a project first."), true);
        return;
    }
    const std::string root = m_recent.items()[static_cast<size_t>(m_selected)].path;
    project::ProjectFile pf;
    std::string err;
    if (!project::loadProject(root, pf, &err)) {
        // 死路径：提示 + 移除（不弹模态），然后刷新列表。
        toast(err, true);
        m_recent.remove(root);
        m_recent.save(nullptr);
        refreshRecents(false);
        return;
    }
    requestSpawn(SpawnKind::Editor, pf.root);
}

void LauncherApp::requestSpawn(SpawnKind kind, const std::string& root) {
    m_pendingSpawn = kind;
    m_pendingRoot = root;
    m_lastSpawnError.clear();
    VK_LOG_INFO("LAUNCHER-OPEN kind=%s root=%s",
                kind == SpawnKind::Game ? "game" : "editor", root.c_str());
}

void LauncherApp::runPendingSpawn() {
    const SpawnKind kind = m_pendingSpawn;
    const std::string root = m_pendingRoot;
    m_pendingSpawn = SpawnKind::None;

    project::ProjectFile pf;
    std::string err;
    if (!project::loadProject(root, pf, &err)) {
        m_lastSpawnError = err;
        toast(err, true);
        return;
    }

    SpawnTarget t;
    t.exe = siblingExe(kind == SpawnKind::Game ? "Game.exe" : "Editor.exe");
    t.projectRoot = pf.root;
    t.assetDir = project::assetRoot(pf);
    t.scene = pf.startScene;
    t.projectFile = project::projectFilePath(pf.root);

    const SpawnResult r = spawn(t);
    if (!r.ok) {
        m_lastSpawnError = r.error;
        toast(r.error, true);
        return;
    }
    if (kind == SpawnKind::Editor) {
        // 重读一次再 touch：`createFromTemplate` 也会写这个文件，
        // 内存副本可能是旧的（同 commitWizard 里那段注释）。
        m_recent.load(nullptr);
        m_recent.touch(pf);
        m_recent.save(nullptr);
    }
    VK_LOG_INFO("LAUNCHER-SPAWN ok pid=%lu kind=%s root=%s", r.pid,
                kind == SpawnKind::Game ? "game" : "editor", pf.root.c_str());

    // 拉起成功 → **收进任务栏，不退出**。
    //
    // 以前这里是 `window().invalidate()`（"一个工程一个进程，干净"）。用户
    // 2026-10-19 明确要求反过来：编辑器关掉之后启动器还要在 —— 换个工程
    // 重开时不用再去桌面找图标。
    //
    // ⚠ 用 minimize 而不是 hide：hide 会把任务栏按钮一起拿走，用户就
    //   找不回来了。iconify 之后主循环走 `iconified()` 分支（只
    //   glfwWaitEvents，不渲染），常驻几乎不吃 CPU/GPU。
    window().minimize();
    m_minimizedAfterSpawn = true;
    VK_LOG_INFO("LAUNCHER-MINIMIZED pid=%lu kind=%s", r.pid,
                kind == SpawnKind::Game ? "game" : "editor");
}

// ============================================================
// 绘制
// ============================================================

void LauncherApp::onImGui() {
    const ImGuiIO& io = ImGui::GetIO();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("##venn-launcher", nullptr, flags);

    drawHeader();

    const float bodyH =
        io.DisplaySize.y - kHeaderH - kFooterH - ImGui::GetStyle().ItemSpacing.y * 2;
    ImGui::BeginChild("##body", ImVec2(0, bodyH > 80 ? bodyH : 80), false);
    switch (m_page) {
        case Page::Idle: drawIdle(); break;
        case Page::Done: drawDone(); break;
        default: drawWizard(); break;
    }
    ImGui::EndChild();

    drawFooter();
    ImGui::End();

    drawToasts();
}

void LauncherApp::drawHeader() {
    ImGui::BeginChild("##header", ImVec2(0, kHeaderH - 8), false);
    ImGui::SetCursorPosY(10);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 6));
    ImGui::TextUnformatted("Venn Engine");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", core::kVennVersion);

    // 语言切换靠右。⚠ 用自名（"English" / "中文"），不翻 —— 看不懂当前
    // 语言这件事必须不可能发生（i18n 的既有纪律）。
    const float comboW = 130.0f;
    ImGui::SameLine(ImGui::GetWindowWidth() - comboW - 16.0f);
    ImGui::SetNextItemWidth(comboW);
    int lang = i18n::language();
    constexpr int kMaxLang = 8;
    const char* names[kMaxLang];
    const int n = i18n::languageCount() < kMaxLang ? i18n::languageCount() : kMaxLang;
    for (int i = 0; i < n; ++i) names[i] = i18n::languageName(i);
    if (ImGui::Combo("##lang", &lang, names, n)) {
        i18n::setLanguage(lang);
        VK_LOG_INFO("LAUNCHER-LANG index=%d", lang);
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
    // 锚点：整条 header 的矩形（脚本可据此判断"窗口起来了且画到了 UI"）。
    anchor("LAUNCH-RECT-header");
    ImGui::Separator();
}

void LauncherApp::drawIdle() {
    // 左栏：最近工程 + 打开按钮
    ImGui::BeginChild("##left", ImVec2(kLeftW, 0), true);
    drawRecentColumn();
    ImGui::EndChild();

    ImGui::SameLine();

    // 右栏：详情
    ImGui::BeginChild("##right", ImVec2(0, 0), true);
    drawDetailColumn();
    ImGui::EndChild();
}

void LauncherApp::drawRecentColumn() {
    ImGui::TextDisabled("%s", TR("Recent projects"));
    ImGui::Separator();

    if (m_recent.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.62f, 0.66f, 1.0f));
        ImGui::TextWrapped("%s", TR("No recent projects yet."));
        ImGui::Spacing();
        ImGui::TextWrapped("%s", TR("Click New game at the bottom left to create one."));
        ImGui::PopStyleColor();
    } else {
        for (int i = 0; i < static_cast<int>(m_recent.size()); ++i) {
            const project::RecentItem& it = m_recent.items()[static_cast<size_t>(i)];
            ImGui::PushID(i);
            char label[512];
            const std::string age = humanAge(it.lastOpenedAt);
            std::snprintf(label, sizeof(label), "%s\n%s  %s", it.name.c_str(),
                          TR(project::gameTypeLabel(it.gameType)),
                          age.empty() ? "" : age.c_str());
            const bool sel = (i == m_selected);
            if (ImGui::Selectable(label, sel, 0, ImVec2(0, 46))) {
                m_selected = i;
                m_detailOk = false;
                std::string err;
                m_detailOk =
                    project::loadProject(it.path, m_detail, &err).ok;
                m_detailError = err;
                VK_LOG_INFO("LAUNCHER-SELECT index=%d name=%s", i,
                            it.name.c_str());
            }
            anchorf("LAUNCH-RECENT-%d", i);
            ImGui::PopID();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button(TR("Open project..."), ImVec2(-1, 32))) {
        const std::string pick = pickFolder(TR("Select project folder"), m_wizParent);
        if (!pick.empty()) openProjectAt(pick);
    }
    anchor("LAUNCH-BTN-open");
}

void LauncherApp::drawDetailColumn() {
    if (m_selected < 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.62f, 0.66f, 1.0f));
        ImGui::TextWrapped("%s", TR("Select a project on the left, or create a new one."));
        ImGui::PopStyleColor();
        anchor("LAUNCH-DETAIL");
        return;
    }
    const project::RecentItem& it = m_recent.items()[static_cast<size_t>(m_selected)];

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 8));
    ImGui::SetWindowFontScale(1.35f);
    ImGui::TextUnformatted(it.name.c_str());
    ImGui::SetWindowFontScale(1.0f);

    ImGui::Separator();
    ImGui::TextDisabled("%s", TR("Path"));
    ImGui::SameLine(110);
    ImGui::TextWrapped("%s", it.path.c_str());

    ImGui::TextDisabled("%s", TR("Type"));
    ImGui::SameLine(110);
    ImGui::TextUnformatted(TR(project::gameTypeLabel(it.gameType)));

    if (m_detailOk) {
        ImGui::TextDisabled("%s", TR("Scene"));
        ImGui::SameLine(110);
        ImGui::TextUnformatted(m_detail.startScene.c_str());

        ImGui::TextDisabled("%s", TR("Code"));
        ImGui::SameLine(110);
        if (m_detail.codeLinked) {
            ImGui::Text("%s (%s)", m_detail.codeRoot.c_str(),
                        TR("external, linked"));
        } else {
            ImGui::Text("%s (vks)", m_detail.codeRoot.c_str());
        }
    } else if (!m_detailError.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.42f, 0.40f, 1.0f));
        ImGui::TextWrapped("%s", m_detailError.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button(TR("Open editor"), ImVec2(170, 36))) openSelected();
    anchor("LAUNCH-BTN-openEditor");
    ImGui::SameLine();
    if (ImGui::Button(TR("Run game"), ImVec2(170, 36))) {
        if (m_detailOk) requestSpawn(SpawnKind::Game, m_detail.root);
        else toast(TR("This project cannot be opened."), true);
    }
    anchor("LAUNCH-BTN-runGame");

    ImGui::Spacing();
    if (ImGui::Button(TR("Show in explorer"), ImVec2(170, 28))) {
        if (!revealInExplorer(it.path))
            toast(TR("Cannot open the folder."), true);
    }
    anchor("LAUNCH-BTN-reveal");
    ImGui::SameLine();
    if (ImGui::Button(TR("Remove from list"), ImVec2(170, 28))) {
        m_recent.remove(it.path);
        m_recent.save(nullptr);
        VK_LOG_INFO("LAUNCHER-REMOVE path=%s", it.path.c_str());
        refreshRecents(false);
    }
    anchor("LAUNCH-BTN-remove");
    ImGui::PopStyleVar();

    anchor("LAUNCH-DETAIL");
}

void LauncherApp::drawFooter() {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::Separator();
    ImGui::BeginChild("##footer", ImVec2(0, kFooterH - 12), false);

    if (m_page == Page::Idle) {
        if (ImGui::Button(TR("+ New game"), ImVec2(180, 34))) beginWizard();
        anchor("LAUNCH-BTN-new");
    } else if (m_page == Page::Done) {
        if (ImGui::Button(TR("Open editor"), ImVec2(170, 34))) {
            requestSpawn(SpawnKind::Editor, m_create.root);
        }
        anchor("LAUNCH-BTN-doneOpen");
        ImGui::SameLine();
        if (ImGui::Button(TR("Run game"), ImVec2(150, 34)))
            requestSpawn(SpawnKind::Game, m_create.root);
        anchor("LAUNCH-BTN-doneRun");
        ImGui::SameLine();
        if (ImGui::Button(TR("Close"), ImVec2(120, 34))) {
            m_page = Page::Idle;
            refreshRecents(false);
        }
        anchor("LAUNCH-BTN-doneClose");
    } else {
        // 向导里 footer 只放"取消"，导航按钮在页面内部（右下方）。
        if (ImGui::Button(TR("Cancel"), ImVec2(120, 34))) cancelWizard();
        anchor("LAUNCH-BTN-cancel");
    }

    // 右端状态：GPU + 构建配置
    char status[256];
    std::snprintf(status, sizeof(status), "%s  |  %s",
                  renderer().device().properties().deviceName,
#ifdef NDEBUG
                  "Release"
#else
                  "Debug"
#endif
    );
    const float tw = ImGui::CalcTextSize(status).x;
    ImGui::SameLine(io.DisplaySize.x - tw - 24.0f);
    ImGui::TextDisabled("%s", status);

    ImGui::EndChild();
}

// ------------------------------------------------------------ 向导

void LauncherApp::drawWizard() {
    ImGui::BeginChild("##wiz", ImVec2(0, 0), true);

    // 步骤条：3 步 + 创建
    const int step = (m_page == Page::Type) ? 1
                     : (m_page == Page::Location) ? 2
                                                  : 3;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 6));
    for (int i = 1; i <= 3; ++i) {
        char tag[32];
        std::snprintf(tag, sizeof(tag), "LAUNCH-WIZ-%d", i);
        char text[64];
        std::snprintf(text, sizeof(text), "%d  %s", i,
                      i == 1 ? TR("Type") : i == 2 ? TR("Location") : TR("Display"));
        if (i == step) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.42f, 0.78f, 1.0f));
            ImGui::Button(text, ImVec2(150, 28));
            ImGui::PopStyleColor();
        } else {
            ImGui::Button(text, ImVec2(150, 28));
        }
        anchor(tag);
        if (i < 3) ImGui::SameLine();
    }
    ImGui::PopStyleVar();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    switch (m_page) {
        case Page::Type: drawWizardType(); break;
        case Page::Location: drawWizardLocation(); break;
        default: drawWizardDisplay(); break;
    }

    // 导航按钮（右下）
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + w - 260);
    if (step > 1) {
        if (ImGui::Button(TR("< Back"), ImVec2(110, 34))) {
            m_page = (step == 2) ? Page::Type : Page::Location;
        }
        anchor("LAUNCH-BTN-back");
        ImGui::SameLine();
    }
    if (step < 3) {
        if (ImGui::Button(TR("Next >"), ImVec2(140, 34))) {
            m_page = (step == 1) ? Page::Location : Page::Display;
        }
        anchor("LAUNCH-BTN-next");
    } else {
        if (ImGui::Button(TR("Create"), ImVec2(140, 34))) commitWizard();
        anchor("LAUNCH-BTN-create");
    }

    ImGui::EndChild();
}

// ------------------------------------------------------------ 类型卡片插图

void LauncherApp::ensureTypeArt() {
    if (m_typeArtTried) return;
    m_typeArtTried = true;

    const char* files[2] = {"launcher/type_first_person.png",
                            "launcher/type_third_person.png"};
    for (int i = 0; i < 2; ++i) {
        const std::string abs = assets::resolveAssetPath(files[i]);
        assets::Texture* tex = nullptr;
        try {
            // 缓存键单独给：`loadTexture(path, name)` 的 name 就是缓存键。
            // 不传的话键=路径，"以后换了插图文件名"会命中旧纹理 —— 症状是
            // "我明明换了图，界面还是老图"，而日志里一行提示都没有。
            const std::string key = std::string("launcher:type") + (i == 0 ? "fp" : "tp");
            tex = assets().loadTexture(abs, key, true);
        } catch (const std::exception& ex) {
            VK_LOG_WARN("LAUNCHER-ART load failed (%s): %s", abs.c_str(), ex.what());
        }
        if (!tex || tex->view() == VK_NULL_HANDLE) {
            VK_LOG_WARN("LAUNCHER-ART missing %s (卡片退回纯文字)", files[i]);
            continue;
        }
        m_typeArt[i] = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(
            ImGui_ImplVulkan_AddTexture(tex->view(),
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)));
        VK_LOG_INFO("LAUNCHER-ART %s %ux%u ds=%llu", files[i], tex->width(),
                    tex->height(), static_cast<unsigned long long>(m_typeArt[i]));
    }
}

void LauncherApp::drawWizardType() {
    ensureTypeArt();

    ImGui::TextUnformatted(TR("What kind of game is this?"));
    ImGui::Spacing();

    struct Card {
        GameType type;
        const char* title;
        const char* line1;
        const char* line2;
        const char* tag;
    };
    const Card cards[] = {
        {project::GameType::FirstPerson, "First person",
         "Camera sits at eye level", "The mouse turns the view directly",
         "LAUNCH-TYPE-0"},
        {project::GameType::ThirdPerson, "Third person",
         "Camera sits behind and above", "Mouse orbits, character turns",
         "LAUNCH-TYPE-1"},
    };

    // 卡片 = 16:9 插图 + 底部标题条。宽度随可用宽度收口（最宽 452），
    // 于是窗口被拉到最小尺寸时也不会挤成一条。
    const float gap = 18.0f;
    const float availW = ImGui::GetContentRegionAvail().x;
    float cardW = (availW - gap - 8.0f) * 0.5f;
    if (cardW > 452.0f) cardW = 452.0f;
    if (cardW < 240.0f) cardW = 240.0f;
    const float imgH = cardW * 9.0f / 16.0f;
    const float textH = 44.0f;
    const float cardH = imgH + textH;

    for (int i = 0; i < 2; ++i) {
        const Card& c = cards[i];
        const bool sel = (m_wizType == c.type);
        if (i > 0) ImGui::SameLine(0.0f, gap);
        ImGui::PushID(i);

        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        // 整张卡就是一个 InvisibleButton —— hover / 点击都在**父窗口**里算。
        //
        // ⚠ 别换成 `BeginChild` 画卡片：子窗口是独立窗口，会把后面画在父
        //   窗口里的 item 的 hover 全挡住（"卡片画出来了、点下去没反应"，
        //   历史上为此丢过三轮向导）。InvisibleButton 什么都不画，正好。
        const bool clicked = ImGui::InvisibleButton("##type", ImVec2(cardW, cardH));
        const bool hov = ImGui::IsItemHovered();
        anchor(c.tag);   // 必须在 item 之后立刻取矩形

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p1(p0.x + cardW, p0.y + cardH);
        const float rounding = 8.0f;
        dl->AddRectFilled(p0, p1,
                          sel ? IM_COL32(34, 56, 94, 255) : IM_COL32(30, 31, 35, 255),
                          rounding);

        // 插图区：图就是 16:9，卡片这块也是 16:9 → UV 直接整张给。
        const ImVec2 i0(p0.x, p0.y), i1(p0.x + cardW, p0.y + imgH);
        const std::uint64_t ds = m_typeArt[i];
        if (ds != 0) {
            dl->AddImageRounded(static_cast<ImTextureID>(ds), i0, i1,
                                ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                                sel ? IM_COL32(255, 255, 255, 255)
                                    : IM_COL32(255, 255, 255, hov ? 235 : 200),
                                rounding, ImDrawFlags_RoundCornersTop);
            if (!sel) {
                // 没选中的压一层暗罩：一眼能看出"哪个是当前选择"。
                // ⚠ 别压太重（试过 90）：插图里的角色视角特征会糊掉，
                //   而这张图的存在意义就是"让人一眼看出相机在哪"。
                dl->AddRectFilled(i0, i1, IM_COL32(8, 10, 16, hov ? 18 : 55),
                                  rounding, ImDrawFlags_RoundCornersTop);
            }
        } else {
            // 插图没加载出来也不崩：留一块底色 + 标题，卡片照样能点。
            dl->AddRectFilled(ImVec2(p0.x, p0.y), i1, IM_COL32(46, 48, 55, 255),
                              rounding, ImDrawFlags_RoundCornersTop);
        }

        dl->AddText(ImVec2(p0.x + 14.0f, p0.y + imgH + 13.0f),
                    sel ? IM_COL32(236, 242, 255, 255) : IM_COL32(210, 214, 222, 255),
                    TR(c.title));
        // 选中：亮蓝描边；hover：灰白描边。
        dl->AddRect(p0, p1,
                    sel ? IM_COL32(110, 165, 255, 255)
                        : (hov ? IM_COL32(126, 132, 144, 255)
                               : IM_COL32(58, 60, 66, 255)),
                    rounding, 0, sel ? 2.0f : 1.0f);

        if (clicked) {
            m_wizType = c.type;
            VK_LOG_INFO("LAUNCHER-WIZARD type=%s", project::gameTypeId(c.type));
        }
        // 注释文字**只在悬浮时**出现（用户要求：卡面保持干净）。
        if (hov) ImGui::SetTooltip("%s\n%s", TR(c.line1), TR(c.line2));

        ImGui::PopID();
    }
    ImGui::NewLine();
    ImGui::TextDisabled("%s", TR("The template decides the scene, camera and input "
                                 "defaults. You can change them later."));
}

void LauncherApp::drawWizardLocation() {
    ImGui::TextUnformatted(TR("Where should the project live?"));
    ImGui::Spacing();

    ImGui::SetNextItemWidth(520);
    char nameBuf[256];
    std::snprintf(nameBuf, sizeof(nameBuf), "%s", m_wizName.c_str());
    if (ImGui::InputText(TRL("Project name##wizname"), nameBuf, sizeof(nameBuf))) {
        m_wizName = nameBuf;
        if (!m_wizRootTouched) syncWizardRoot();
    }
    anchor("LAUNCH-INPUT-name");

    ImGui::SetNextItemWidth(520);
    char parentBuf[1024];
    std::snprintf(parentBuf, sizeof(parentBuf), "%s", m_wizParent.c_str());
    if (ImGui::InputText(TRL("Parent folder##wizparent"), parentBuf, sizeof(parentBuf))) {
        m_wizParent = parentBuf;
        if (!m_wizRootTouched) syncWizardRoot();
    }
    anchor("LAUNCH-INPUT-parent");
    ImGui::SameLine();
    if (ImGui::Button(TR("Browse..."), ImVec2(110, 0))) {
        const std::string pick = pickFolder(TR("Select parent folder"), m_wizParent);
        if (!pick.empty()) {
            m_wizParent = pick;
            if (!m_wizRootTouched) syncWizardRoot();
        }
    }
    anchor("LAUNCH-BTN-browse");

    ImGui::Spacing();
    ImGui::SetNextItemWidth(520);
    char rootBuf[1024];
    std::snprintf(rootBuf, sizeof(rootBuf), "%s", m_wizRoot.c_str());
    if (ImGui::InputText(TRL("Project folder##wizroot"), rootBuf, sizeof(rootBuf))) {
        m_wizRoot = rootBuf;
        // 手改过工程文件夹之后，"名字改 → 路径跟着变"的联动**断开**。
        // 不断开的话，用户改完名字会把刚手敲的路径冲掉（而且不提示）。
        m_wizRootTouched = true;
    }
    anchor("LAUNCH-INPUT-root");

    // 目标目录可用性：**就地红字**，不弹模态（路线图 §5.3 Step2）。
    const project::IoResult usable = project::checkTargetDirUsable(m_wizRoot);
    if (!usable.ok) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.42f, 0.40f, 1.0f));
        ImGui::TextWrapped("%s", usable.error.c_str());
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.48f, 0.90f, 0.52f, 1.0f));
        ImGui::TextWrapped("%s", TR("This folder is free."));
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("%s", TR("Code"));
    if (ImGui::Checkbox(TRL("Keep the script folder outside the project##wizext"),
                        &m_wizExternal)) {
        VK_LOG_INFO("LAUNCHER-WIZARD code-external=%d", m_wizExternal ? 1 : 0);
    }
    anchor("LAUNCH-CHK-external");
    if (m_wizExternal) {
        ImGui::SetNextItemWidth(520);
        char codeBuf[1024];
        std::snprintf(codeBuf, sizeof(codeBuf), "%s", m_wizCodeRoot.c_str());
        if (ImGui::InputText(TRL("External script folder##wizcode"), codeBuf, sizeof(codeBuf)))
            m_wizCodeRoot = codeBuf;
        anchor("LAUNCH-INPUT-coderoot");
        ImGui::SameLine();
        if (ImGui::Button(TRL("Browse...##code"), ImVec2(110, 0))) {
            const std::string pick =
                pickFolder(TR("Select script folder"), m_wizCodeRoot);
            if (!pick.empty()) m_wizCodeRoot = pick;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.62f, 0.66f, 1.0f));
        ImGui::TextWrapped("%s",
                           TR("A directory junction is created at "
                              "assets/scripts. No admin rights needed."));
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.62f, 0.66f, 1.0f));
        ImGui::TextWrapped("%s", TR("Scripts live in assets/scripts (*.vks) and are "
                                    "hot-reloaded by the editor."));
        ImGui::PopStyleColor();
    }
}

void LauncherApp::drawWizardDisplay() {
    ImGui::TextUnformatted(TR("Display and quality"));
    ImGui::Spacing();

    ImGui::SetNextItemWidth(240);
    const char* resItems[kResCount];
    for (int i = 0; i < kResCount; ++i) resItems[i] = TR(kResPresets[i].label);
    if (ImGui::Combo(TRL("Resolution##wizres"), &m_wizRes, resItems, kResCount)) {
        VK_LOG_INFO("LAUNCHER-WIZARD res=%d", m_wizRes);
    }
    anchor("LAUNCH-COMBO-res");

    if (m_wizRes == kResCount - 1) {
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt(TRL("Width##wizw"), &m_wizCustomW, 0, 0);
        anchor("LAUNCH-INPUT-width");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt(TRL("Height##wizh"), &m_wizCustomH, 0, 0);
        anchor("LAUNCH-INPUT-height");
        if (m_wizCustomW < 320) m_wizCustomW = 320;
        if (m_wizCustomH < 240) m_wizCustomH = 240;
    }

    ImGui::Checkbox(TR("Fullscreen"), &m_wizFullscreen);
    anchor("LAUNCH-CHK-fullscreen");

    ImGui::Spacing();
    // 画质档名**从 render:: 取**，不硬编码 —— 那边加一档这里自动出现
    // （这是把预设表下沉到 engine/render 的直接收益）。
    ImGui::SetNextItemWidth(240);
    const int qn = render::qualityPresetCount();
    static const char* qItems[16];
    for (int i = 0; i < qn && i < 16; ++i) qItems[i] = render::qualityPresetName(i);
    if (ImGui::Combo(TRL("Quality##wizq"), &m_wizQuality, qItems,
                     qn < 16 ? qn : 16)) {
        VK_LOG_INFO("LAUNCHER-WIZARD quality=%s",
                    render::qualityPresetName(m_wizQuality));
    }
    anchor("LAUNCH-COMBO-quality");

    // ---- "将写入"清单（路线图 §5.3 Step3 右下角）----
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("%s", TR("Will be written"));
    project::DisplaySpec d;
    d.width = (m_wizRes == kResCount - 1) ? m_wizCustomW : kResPresets[m_wizRes].w;
    d.height = (m_wizRes == kResCount - 1) ? m_wizCustomH : kResPresets[m_wizRes].h;
    d.fullscreen = m_wizFullscreen;
    d.title = m_wizName;
    d.qualityPreset = m_wizQuality;
    for (const std::string& line : project::describeWrites(d)) {
        ImGui::Bullet();
        ImGui::SameLine();
        ImGui::TextUnformatted(TR(line.c_str()));
    }
    anchor("LAUNCH-WRITES");

    // 创建失败的**可读原因**（停在 Step3，不跳页）
    if (!m_create.ok && !m_create.error.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.42f, 0.40f, 1.0f));
        ImGui::TextWrapped("%s", m_create.error.c_str());
        ImGui::PopStyleColor();
    }
}

void LauncherApp::drawDone() {
    ImGui::BeginChild("##done", ImVec2(0, 0), true);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.48f, 0.90f, 0.52f, 1.0f));
    ImGui::SetWindowFontScale(1.3f);
    ImGui::TextUnformatted(TR("Project created"));
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();
    ImGui::Spacing();

    ImGui::TextDisabled("%s", TR("Folder"));
    ImGui::SameLine(110);
    ImGui::TextWrapped("%s", m_create.root.c_str());
    ImGui::TextDisabled("%s", TR("Files"));
    ImGui::SameLine(110);
    ImGui::Text("%d", m_create.filesCopied);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("%s", TR("Steps"));
    for (const std::string& s : m_createTrace) {
        ImGui::Bullet();
        ImGui::SameLine();
        ImGui::TextUnformatted(s.c_str());
    }
    ImGui::EndChild();
    anchor("LAUNCH-DONE");
}

void LauncherApp::drawToasts() {
    const ImGuiIO& io = ImGui::GetIO();
    float y = io.DisplaySize.y - 70.0f;
    for (auto it = m_toasts.rbegin(); it != m_toasts.rend(); ++it) {
        const Toast& t = *it;
        const ImVec2 size(520, 0);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - size.x - 16.0f, y),
                                ImGuiCond_Always, ImVec2(0, 1));
        ImGui::SetNextWindowSize(size, ImGuiCond_Always);
        ImGui::PushStyleColor(ImGuiCol_WindowBg,
                              t.error ? ImVec4(0.35f, 0.12f, 0.12f, 0.96f)
                                      : ImVec4(0.12f, 0.28f, 0.16f, 0.96f));
        const ImGuiWindowFlags f =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize;
        ImGui::Begin("##toast", nullptr, f);
        ImGui::TextWrapped("%s", t.text.c_str());
        ImGui::End();
        ImGui::PopStyleColor();
        y -= 8.0f;
    }
}

} // namespace launcher
