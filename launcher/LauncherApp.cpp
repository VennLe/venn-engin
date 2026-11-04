#include "LauncherApp.h"

#include "LauncherDialog.h"
#include "LauncherSpawn.h"

#include "assets/AssetPath.h"
#include "assets/Texture.h"
#include "core/Logger.h"
#include "core/Version.h"
#include "core/Window.h"
#include "i18n/I18n.h"
#include "project/GameCodeGen.h"
#include "render/QualityPresets.h"
#include "ui/DebugRects.h"
#include "ui/ImGuiTheme.h"
#include "ui/UiChrome.h"

#include <backends/imgui_impl_vulkan.h>
#include <imgui.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <algorithm>
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

// ---- 尺寸 ----
// 2026-10-27 用户第 1 条：「启动器部分窗口大小可以大一些，宽高调大一些」。
// 1080×720 → 1400×880。
//
// 2026-11-04 用户第 1 条：「引擎启动器部分的这个结构性问题就比较大，显得
// 特别的局促，文字也很小，显得很不美观。这些方面你可以多参考下 UE5 的。」
// → 1400×880 → **1560×960**，并且版式整个重排（见 onImGui 的注释）：
//   顶栏/底栏通栏、内容两栏填满高度、右栏用工程预览大图占位。原来右栏只有
//   200px 内容、底下 600px 空白，是"局促"感最主要的来源。
//
// ⚠ 不能无脑写死：这块屏如果是 1366×768 的笔记本，1560×960 会有一半在
//   屏幕外。所以 onInit 里按**工作区**夹一次（见 kWinWPct / kWinHPct），
//   夹完仍然居中。用例靠锚点算点击位置，窗口多大都不影响
//   —— 但 `verify_launcher_open` 的 O0 断言要跟着这两个数一起改。
constexpr int kWindowW = 1560;
constexpr int kWindowH = 960;
// 工作区占比上限（夹窗口用）。0.94 / 0.90 留出任务栏与标题栏的空间。
constexpr float kWinWPct = 0.94f;
constexpr float kWinHPct = 0.90f;

// 启动器的**全局字体比例**。
//
// "面板文字 / 面板尺度"的比例在 1.0 时明显偏小：18px 的字落在 1560 宽的
// 窗口里像蚂蚁，而且 ImGui 的行高是从字号推出来的（GetFrameHeight），
// 所以这一个数同时决定"字多大"和"每行多高"。
//
// ⚠ 只作用于**本进程**：编辑器走自己的 `EditorContext::fontScale()`
//   （1.2，见那里），Game.exe 的 HUD 保持 1.0 —— 三者共用 ImGuiManager，
//   谁都不设就都用 1.0，所以必须各设各的。
constexpr float kFontScale = 1.2f;

constexpr float kHeaderH = 92.0f;
constexpr float kFooterH = 72.0f;
constexpr float kLeftW = 430.0f;
constexpr float kPad = 26.0f;      // 内容区左右留白（通栏元素不吃这个）
constexpr float kCardGap = 16.0f;

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

// ---- 版式小件（本文件专用）----
//
// 2026-11-04 版式重排时抽出来的三件：分区标题 / 字段标签 / 类型插图下标。
// 抽出来是为了让"1 类型""2 位置""3 画面"三段长得**一模一样** —— 三段各画
// 一套的话，段距、强调条位置迟早互相漂。

// 带强调色竖条的分区标题。`step` 可空（"最近工程"这种没有序号）。
void sectionHeader(const char* step, const char* title) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float th = ImGui::GetTextLineHeight();
    const ImVec4 acc = ui::tokens().accent;
    dl->AddRectFilled(ImVec2(p.x, p.y + 1.0f),
                      ImVec2(p.x + 3.0f, p.y + th - 1.0f),
                      ImGui::GetColorU32(acc), 1.5f);

    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 13.0f);
    if (step && step[0]) {
        ImGui::PushStyleColor(ImGuiCol_Text, acc);
        ImGui::TextUnformatted(step);
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, 8.0f);
    }
    ImGui::TextUnformatted(title);
}

// 字段标签画在**输入框上方**（UE5 的设置页就是这种排布）。
//
// 为什么不用 ImGui 的"标签在右边"：输入框一旦拉满列宽（SetNextItemWidth(-1)），
// 右标签就被推到列外看不见了；而固定写 520 宽又会在窄窗口里溢出。
void fieldLabel(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ui::tokens().textDim);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

// `project::GameType` → 下标（= `kGameTypes[]` 的顺序 = 插图数组下标）。
// engine/project 里没有反查接口，这里自己走一遍：越界退化成 1（第三人称，
// 与 `gameTypeAt()` 的越界行为一致）。
int typeIndex(project::GameType t) {
    for (int i = 0; i < project::kGameTypeCount; ++i)
        if (project::gameTypeAt(i) == t) return i;
    return 1;
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
    // 字体比例：**本进程**唯一的设置点。
    //
    // 编辑器每帧从 EditorContext 推一次（见 EditorApp::drawUI 开头），启动器
    // 没有设置界面，所以在这里定死。必须在第一次 beginFrame 之前 —— 在这里
    // 设，首帧就是最终字号，不会先小一号再跳大。
    renderer().ui().setFontScale(kFontScale);

    // 窗口：居中 + 不可小于 900×560。
    //
    // ⚠ 自动化跑（MYVK_FRAMES / MYVK_NO_WINDOW_SAVE / MYVK_LOG_RECTS）
    //   **照样居中**：启动器的布局是"一屏两栏"，脚本要靠锚点算点击位置，
    //   窗口在任意位置都不影响（锚点是客户区坐标）。而"恢复上次几何"才是
    //   不该做的 —— 那会让脚本拿到一个 3840×2054 全屏窗口，第一帧的
    //   两栏宽度全错。所以这里只做居中，不读任何存档。
    int wx = 0, wy = 0, ww = 0, wh = 0;
    if (window().primaryMonitorWorkArea(&wx, &wy, &ww, &wh)) {
        // 大窗口优先，但**永远不超过工作区**（笔记本 1366×768 上 1400×880
        // 会有一半跑到屏幕外）。夹完再居中 —— 顺序不能反：先居中再缩，
        // 窗口会从中心往右下缩，看起来像"跑到右边去了"。
        const int winW = std::min(kWindowW, static_cast<int>(ww * kWinWPct));
        const int winH = std::min(kWindowH, static_cast<int>(wh * kWinHPct));
        if (winW > 640 && winH > 480 &&
            (winW != kWindowW || winH != kWindowH)) {
            window().setWindowSize(winW, winH);
            VK_LOG_INFO("LAUNCHER-WINSIZE clamp=%dx%d (want %dx%d work=%dx%d)",
                        winW, winH, kWindowW, kWindowH, ww, wh);
        }
        const int cx = wx + (ww - winW) / 2;
        const int cy = wy + (wh - winH) / 2;
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
    // 自动化口子：**只关"拉起 VS"这一步**，不关"生成代码工程"。
    //
    // ⚠ 自从 `m_wizCpp` 默认开（2026-11-04），`verify_launcher_create.py`
    //   一轮要建 4~5 个工程 —— 不关这个，每建一个就弹一个 Visual Studio，
    //   桌面上会堆一排 devenv，而且每个都要加载几秒，把用例拖到超时。
    //   生成物（code/*.sln）照旧写，所以"建出来的工程带代码工程"这条
    //   判据仍然是被真实验证过的，只是不点那一下"打开"。
    if (envStr("MYVK_LAUNCHER_TEST_NO_IDE") == "1") {
        m_wizOpenIde = false;
        VK_LOG_INFO("LAUNCHER-WIZARD open-ide=0 (MYVK_LAUNCHER_TEST_NO_IDE)");
    }

    // 界面语言：启动器**没有**自己的设置存储，所以只有两档 ——
    // 上面的下拉框选的（本次进程有效）、以及出厂默认。
    // 出厂默认 = 当前电脑的系统语言（2026-10-29，见 i18n/I18n.h 文件头），
    // 以前写死英文，中文机器上第一次打开是英文界面。
    VK_LOG_INFO("LAUNCHER-LANG lang=%s src=system",
                i18n::languageCode(i18n::language()));

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
    m_page = Page::New;
    m_wizType = project::GameType::ThirdPerson;
    m_wizRootTouched = false;
    m_create = project::CreateResult{};
    m_createTrace.clear();
    syncWizardRoot();
    VK_LOG_INFO("LAUNCHER-WIZARD page=new");
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
    spec.generateCode = m_wizCpp;
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

        // ---- C++ 代码工程：按用户选择拉起 Visual Studio ----
        //
        // ⚠ 放在 `requestSpawn` **之前**：spawn 是下一帧才发生的事，而
        //   打开 IDE 是同步的 CreateProcess（几十毫秒）。反过来也能跑，但
        //   "刚建好的东西先给你看"更符合直觉。
        // ⚠ `slnPath` 为空**不是**创建失败：源码与 CMakeLists 都在，
        //   用户仍能在 VS 里打开文件夹。
        if (m_wizCpp) {
            if (m_create.slnPath.empty()) {
                toast(m_create.codeNote.empty()
                          ? TR("Code project created, but no solution was generated.")
                          : m_create.codeNote,
                      true);
            } else if (m_wizOpenIde) {
                std::string ideErr;
                if (!project::openInVisualStudio(m_create.slnPath, &ideErr))
                    toast(ideErr, true);
            }
        }

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
        // **留在向导页**，错误行就地显示（不弹模态）。
        m_page = Page::New;
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
//
// 版式（2026-11-04 重排）：
//
//   ┌──────────────────────────────────────────────┐ ← 通栏渐变顶栏 92px
//   │  ▣ Venn Engine                        [中文▾] │
//   ├──────────────────────────────────────────────┤
//   │  ┌ 最近工程 ────┐  ┌ 工程详情 / 空白页 ─────┐ │ ← 内容区（左右留白 26）
//   │  │ 行…          │  │  预览大图 + 信息 + 按钮 │ │
//   │  └──────────────┘  └───────────────────────┘ │
//   ├──────────────────────────────────────────────┤ ← 通栏底栏 72px
//   │ [+ 新建游戏]              显卡 | Debug        │
//   └──────────────────────────────────────────────┘
//
// 两个"通栏"是这次重排的核心：原来顶栏/底栏是**圆角矩形贴在窗口里**，
// 四周一圈 10px 的窗口内边距把它们切成了两块色块，整个窗口像一张没铺满的
// 卡片。现在根窗口 WindowPadding = 0，渐变带直接吃到窗口边缘，内容区的
// 留白由 body 子窗口自己给。

void LauncherApp::onImGui() {
    const ImGuiIO& io = ImGui::GetIO();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##venn-launcher", nullptr, flags);

    drawHeader();

    // ⚠ 高度必须减掉**两条 ItemSpacing**：根窗口每个 item 之间有 spacing，
    //   顶栏→body、body→底栏各一条。只减 kHeaderH+kFooterH 的话底栏会被顶
    //   出窗口下沿（实测差 14px，"新建游戏"按钮最后 10px 看不见）。
    const float bodyH = io.DisplaySize.y - kHeaderH - kFooterH -
                        ImGui::GetStyle().ItemSpacing.y * 2.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kPad, 16.0f));
    // ⚠ 必须显式 `AlwaysUseWindowPadding`：ImGui 对**无边框**子窗口会把
    //   WindowPadding 强制清零（imgui.cpp 的 "LOCK BORDER SIZE AND PADDING"
    //   那一段）。而主题给的 ChildBorderSize 可能就是 0 —— 于是"我明明设了
    //   26px 留白"在实际渲染里是 0，内容贴着窗口边缘。这个坑只在拿锚点量
    //   坐标时才看得出来（肉眼看像"布局没生效"）。
    ImGui::BeginChild("##body", ImVec2(0, bodyH > 120 ? bodyH : 120),
                      ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar);
    switch (m_page) {
        case Page::Idle: drawIdle(); break;
        case Page::Done: drawDone(); break;
        default: drawWizard(); break;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    drawFooter();
    ImGui::End();
    ImGui::PopStyleVar();

    drawToasts();
}

void LauncherApp::drawHeader() {
    // 顶栏做成一条"高光带"（左深右亮的水平渐变 + 底部一条亮线），圆角 0：
    // 要的就是它顶到窗口左/右/上三边。
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("##header", ImVec2(0, kHeaderH), false,
                      ImGuiWindowFlags_NoScrollbar);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetWindowPos();
        const ImVec2 sz = ImGui::GetWindowSize();
        ui::headerBand(dl, p0, ImVec2(p0.x + sz.x, p0.y + sz.y), 0.0f);

        // ---- 左侧品牌块：42×42 的强调色方块 + 手画的 "V" ----
        // "V" 用两条线画而不是排一行字：字号由 FontGlobalScale 决定，而这里
        // 要的是一个**固定** 42px 的标记，排字会随缩放变形。
        const float markS = 42.0f;
        const ImVec2 m0(p0.x + kPad, p0.y + (kHeaderH - markS) * 0.5f - 4.0f);
        const ImVec2 m1(m0.x + markS, m0.y + markS);
        ui::cardBackground(dl, m0, m1, 11.0f, true, 0.40f);
        const ImU32 ac = ImGui::GetColorU32(ui::tokens().accent);
        const ImVec2 mc((m0.x + m1.x) * 0.5f, (m0.y + m1.y) * 0.5f);
        dl->AddLine(ImVec2(mc.x - 9.0f, mc.y - 7.0f),
                    ImVec2(mc.x, mc.y + 8.0f), ac, 4.0f);
        dl->AddLine(ImVec2(mc.x, mc.y + 8.0f),
                    ImVec2(mc.x + 9.0f, mc.y - 7.0f), ac, 4.0f);

        // ---- 标题 + 版本（两行）----
        // ⚠ 用 SetCursorPos 而不是 SameLine：第一行要换行成两行，SameLine
        //   之后的光标位置由上一行的行高决定，字号一变就错位。
        const float titleScale = 1.45f;
        ImGui::SetCursorPos(
            ImVec2(kPad + markS + 16.0f,
                   (kHeaderH - 44.0f) * 0.5f - 6.0f));
        ImGui::SetWindowFontScale(titleScale);
        ImGui::TextUnformatted("Venn Engine");
        ImGui::SetWindowFontScale(1.0f);

        ImGui::SetCursorPos(
            ImVec2(kPad + markS + 18.0f,
                   (kHeaderH - 44.0f) * 0.5f - 6.0f +
                       ImGui::GetTextLineHeight() * titleScale));
        ImGui::PushStyleColor(ImGuiCol_Text, ui::tokens().textDim);
        ImGui::Text("%s  ·  %s", core::kVennVersion,
                    TR("Anime-style 3D game engine"));
        ImGui::PopStyleColor();

        // ---- 右侧语言切换 ----
        // ⚠ 用自名（"English" / "中文"），不翻 —— 看不懂当前语言这件事必须
        //   不可能发生（i18n 的既有纪律）。
        const float comboW = 150.0f;
        ImGui::SetCursorPos(
            ImVec2(sz.x - comboW - kPad, (kHeaderH - 34.0f) * 0.5f - 4.0f));
        ImGui::SetNextItemWidth(comboW);
        int lang = i18n::language();
        constexpr int kMaxLang = 8;
        const char* names[kMaxLang];
        const int n =
            i18n::languageCount() < kMaxLang ? i18n::languageCount() : kMaxLang;
        for (int i = 0; i < n; ++i) names[i] = i18n::languageName(i);
        if (ImGui::Combo("##lang", &lang, names, n)) {
            i18n::setLanguage(lang);
            VK_LOG_INFO("LAUNCHER-LANG index=%d", lang);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    // 锚点：整条 header 的矩形（脚本可据此判断"窗口起来了且画到了 UI"）。
    anchor("LAUNCH-RECT-header");
}

void LauncherApp::drawIdle() {
    // 左栏：最近工程 + 打开按钮；右栏：详情 / 空白引导。
    // ⚠ 两栏都**填满高度**（size.y = 0），否则内容一少就露出一片"没画完"的
    //   空底 —— 用户说的"局促"有一半来自这个比例失衡。
    ImGui::BeginChild("##left", ImVec2(kLeftW, 0),
                      ImGuiChildFlags_Borders |
                          ImGuiChildFlags_AlwaysUseWindowPadding);
    drawRecentColumn();
    ImGui::EndChild();

    ImGui::SameLine(0.0f, 20.0f);

    ImGui::BeginChild("##right", ImVec2(0, 0),
                      ImGuiChildFlags_Borders |
                          ImGuiChildFlags_AlwaysUseWindowPadding);
    drawDetailColumn();
    ImGui::EndChild();
}

void LauncherApp::drawRecentColumn() {
    sectionHeader(nullptr, TR("Recent projects"));
    ImGui::Spacing();

    // 列表占满剩余高度（留出底部那颗"打开已有工程"的位置）。
    constexpr float kRowH = 64.0f;
    constexpr float kOpenBtnH = 40.0f;
    const float listH = ImGui::GetContentRegionAvail().y - kOpenBtnH - 16.0f;
    ImGui::BeginChild("##recentList", ImVec2(0, listH > 100 ? listH : 100),
                      false);

    if (m_recent.empty()) {
        // 空态：顶部留一点，一段说明就够 —— 不做成"整屏空白"。
        ImGui::Dummy(ImVec2(0, 18));
        ImGui::PushStyleColor(ImGuiCol_Text, ui::tokens().textDim);
        ImGui::TextWrapped("%s", TR("No recent projects yet."));
        ImGui::Spacing();
        ImGui::TextWrapped(
            "%s", TR("Click New game at the bottom left to create one."));
        ImGui::PopStyleColor();
    } else {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (int i = 0; i < static_cast<int>(m_recent.size()); ++i) {
            const project::RecentItem& it =
                m_recent.items()[static_cast<size_t>(i)];
            ImGui::PushID(i);
            // 整行是一个 InvisibleButton（与类型卡片同一套：命中交给 ImGui，
            // 画由我们负责）。不用 Selectable 是因为它只能一种字色 ——
            // "名字亮 + 副标题灰"这种两行结构画不出来。
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            const float rowW = ImGui::GetContentRegionAvail().x;
            const bool clicked =
                ImGui::InvisibleButton("##row", ImVec2(rowW, kRowH));
            const bool hov = ImGui::IsItemHovered();
            const bool sel = (i == m_selected);
            anchorf("LAUNCH-RECENT-%d", i);

            const ImVec2 p1(p0.x + rowW, p0.y + kRowH);
            const ImVec4 acc = ui::tokens().accent;
            const ImU32 bg = ImGui::GetColorU32(
                sel ? ImVec4(acc.x, acc.y, acc.z, 0.22f)
                    : (hov ? ui::tokens().cardBgHover : ui::tokens().cardBg));
            dl->AddRectFilled(ImVec2(p0.x, p0.y + 1.0f),
                              ImVec2(p1.x, p1.y - 1.0f), bg, 9.0f);
            if (sel)
                dl->AddRect(ImVec2(p0.x, p0.y + 1.0f),
                            ImVec2(p1.x, p1.y - 1.0f),
                            ImGui::GetColorU32(acc), 9.0f, 0, 1.5f);

            const std::string age = humanAge(it.lastOpenedAt);
            std::string meta = TR(project::gameTypeLabel(it.gameType));
            if (!age.empty()) {
                meta += "   ·   ";
                meta += age;
            }
            dl->AddText(ImVec2(p0.x + 15.0f, p0.y + 11.0f),
                        ImGui::GetColorU32(ImGuiCol_Text), it.name.c_str());
            dl->AddText(ImVec2(p0.x + 15.0f, p0.y + 36.0f),
                        ImGui::GetColorU32(ui::tokens().textDim), meta.c_str());

            if (clicked) {
                m_selected = i;
                m_detailOk = false;
                std::string err;
                m_detailOk = project::loadProject(it.path, m_detail, &err).ok;
                m_detailError = err;
                VK_LOG_INFO("LAUNCHER-SELECT index=%d name=%s", i,
                            it.name.c_str());
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();
    if (ImGui::Button(TR("Open project..."), ImVec2(-1, kOpenBtnH))) {
        const std::string pick =
            pickFolder(TR("Select project folder"), m_wizParent);
        if (!pick.empty()) openProjectAt(pick);
    }
    anchor("LAUNCH-BTN-open");
}

void LauncherApp::drawDetailColumn() {
    // 右栏整块的矩形就是 U4 那条"左右两栏没有塌成一栏"的判据 —— 用它而不是
    // 用里面某个文字控件的矩形：文字控件宽度随内容变，两栏比例一改就误判。
    {
        const ImVec2 wp = ImGui::GetWindowPos();
        const ImVec2 ws = ImGui::GetWindowSize();
        ui::logRect("LAUNCH-DETAIL", wp, ImVec2(wp.x + ws.x, wp.y + ws.y));
    }

    if (m_selected < 0) {
        // ---- 空白态：垂直居中一段引导 ----
        // 以前这里只有一句 "Select a project on the left"，孤零零贴在左上角，
        // 下面 600px 全空 —— 这是"没画完"观感最直接的来源。
        const float availY = ImGui::GetContentRegionAvail().y;
        const float availX = ImGui::GetContentRegionAvail().x;
        constexpr float kBlockH = 250.0f;
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
                             ((availY - kBlockH) > 0 ? (availY - kBlockH) * 0.42f
                                                     : 0.0f));

        const char* title = TR("Start here");
        ImGui::SetWindowFontScale(1.7f);
        const float tw = ImGui::CalcTextSize(title).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availX - tw) * 0.5f);
        ImGui::TextUnformatted(title);
        ImGui::SetWindowFontScale(1.0f);

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ui::tokens().textDim);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + availX - 90.0f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 45.0f);
        ImGui::TextWrapped("%s",
                           TR("Create a new project from a template, or open "
                              "one you already have."));
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();

        ImGui::Spacing();
        ImGui::Spacing();
        constexpr float kBtnW = 200.0f;
        constexpr float kBtnH = 42.0f;
        const float rowW = kBtnW * 2.0f + 16.0f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                             ((availX - rowW) > 0 ? (availX - rowW) * 0.5f
                                                  : 0.0f));
        if (ui::pillButton("heroNew", TR("+ New game"), ImVec2(kBtnW, kBtnH), 1))
            beginWizard();
        anchor("LAUNCH-BTN-heroNew");
        ImGui::SameLine(0.0f, 16.0f);
        if (ui::pillButton("heroOpen", TR("Open project..."),
                           ImVec2(kBtnW, kBtnH), 0)) {
            const std::string pick =
                pickFolder(TR("Select project folder"), m_wizParent);
            if (!pick.empty()) openProjectAt(pick);
        }
        anchor("LAUNCH-BTN-heroOpen");
        return;
    }

    const project::RecentItem& it =
        m_recent.items()[static_cast<size_t>(m_selected)];

    // ---- 预览大图：按工程类型取那张 16:9 插图 ----
    //
    // 存在的意义是"把右栏上半部分撑起来"。原来这里从标题直接跳到四行等宽
    // 文本，然后一大片空白；UE5 的项目页在这个位置放的是工程缩略图，
    // 观感差别主要就在这一块。
    ensureTypeArt();
    const float availW = ImGui::GetContentRegionAvail().x;
    const float availH = ImGui::GetContentRegionAvail().y;
    // 左图右信息：图占 58% 宽、信息占剩下那 42%。
    // 为什么不是"通栏大图 + 底部信息"：通栏时 16:9 会把高度吃光（1058 宽
    // → 595 高），必须按高度反算宽度，结果图片只占左边 2/3，右边空一块；
    // 并排之后两个方向都填满，也不再需要"压暗罩 + 图上写字"的补丁。
    const float infoGap = 24.0f;
    float imgW = (availW - infoGap) * 0.58f;
    float imgH = imgW * 9.0f / 16.0f;
    const float maxH = availH * 0.58f;   // 不许把操作区挤出去
    if (imgH > maxH) {
        imgH = maxH;
        imgW = imgH * 16.0f / 9.0f;
    }
    // 整块（图 + 信息 + 按钮）在面板里**稍微下移**一点再看：
    // 原来内容全贴着顶边，下面 300px 空着，像"没画完"。按剩余高度给一个
    // 0.35 的偏移（不是 0.5 —— 完全居中会显得上方空得刻意）。
    {
        const float blockH = imgH + 24.0f + 96.0f;
        const float off = (availH - blockH) * 0.35f;
        if (off > 0.0f) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + off);
    }

    const ImVec2 b0 = ImGui::GetCursorScreenPos();
    const ImVec2 b1(b0.x + imgW, b0.y + imgH);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const std::uint64_t ds = m_typeArt[typeIndex(it.gameType)];
        const float r = 10.0f;
        if (ds != 0) {
            dl->AddImageRounded(static_cast<ImTextureID>(ds), b0, b1,
                                ImVec2(0, 0), ImVec2(1, 1),
                                IM_COL32(255, 255, 255, 255), r);
        } else {
            ui::gradientRectV(dl, b0, b1, ImGui::GetColorU32(ui::tokens().cardBg),
                              ImGui::GetColorU32(ui::tokens().panelBg), r);
        }
        // 底部压一层暗罩，让工程名压在图上也读得清（只压图片下缘，
        // 不涉及面板底色，所以浅色主题下同样成立）。
        dl->AddRectFilledMultiColor(
            ImVec2(b0.x, b1.y - imgH * 0.40f), b1, IM_COL32(0, 0, 0, 0),
            IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 175),
            IM_COL32(0, 0, 0, 175));
        dl->AddRect(b0, b1, ImGui::GetColorU32(ui::tokens().borderSoft), r);

        // 工程名压在大图左下角。
        // ⚠ 用带 font_size 的 AddText 重载：`SetWindowFontScale` 对 AddText
        //   无效（它只改下一个 item 的字体），要在一张图里排大字号只能显式
        //   给尺寸。
        const float nameSize = ImGui::GetFontSize() * 1.45f;
        dl->AddText(ImGui::GetFont(), nameSize,
                    ImVec2(b0.x + 20.0f, b1.y - nameSize - 12.0f),
                    IM_COL32(245, 246, 250, 255), it.name.c_str());
    }
    ImGui::Dummy(ImVec2(imgW, imgH));
    ImGui::SameLine(0.0f, infoGap);

    // 信息列：单独一个子窗口，`SameLine(labelX)` 才是相对**本列**的偏移
    // （SameLine 的 x 是相对当前窗口的，直接并排画会跑到图片下面去）。
    ImGui::BeginChild("##detailInfo", ImVec2(0, imgH));

    const float labelX = 92.0f;
    auto infoRow = [&](const char* label, const std::string& value) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::tokens().textDim);
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
        ImGui::SameLine(labelX);
        ImGui::TextWrapped("%s", value.c_str());
    };

    infoRow(TR("Type"), TR(project::gameTypeLabel(it.gameType)));
    infoRow(TR("Path"), it.path);
    if (m_detailOk) {
        infoRow(TR("Scene"), m_detail.startScene);
        if (m_detail.codeLinked)
            infoRow(TR("Code"),
                    m_detail.codeRoot + "  (" + TR("external, linked") + ")");
        else
            infoRow(TR("Code"), m_detail.codeRoot + "  (vks)");
    } else if (!m_detailError.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::tokens().error);
        ImGui::TextWrapped("%s", m_detailError.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();

    // ---- 操作区：紧跟上面的信息块（不再贴底）----
    // 贴底时"图+信息"和按钮之间会拉出一条 200px 的空隙，视线要跨过它才
    // 找得到"打开编辑器"。放一起之后整个右栏是**一块内容**，空的部分
    // 落在面板下半部分，读起来像留白而不是断裂。
    ImGui::Spacing();
    ImGui::Spacing();

    if (ui::pillButton("openEditor", TR("Open editor"), ImVec2(180, 40), 1))
        openSelected();
    anchor("LAUNCH-BTN-openEditor");
    ImGui::SameLine(0.0f, 14.0f);
    if (ui::pillButton("runGame", TR("Run game"), ImVec2(180, 40), 0)) {
        if (m_detailOk) requestSpawn(SpawnKind::Game, m_detail.root);
        else toast(TR("This project cannot be opened."), true);
    }
    anchor("LAUNCH-BTN-runGame");

    ImGui::Spacing();
    if (ImGui::Button(TR("Show in explorer"), ImVec2(180, 32))) {
        if (!revealInExplorer(it.path))
            toast(TR("Cannot open the folder."), true);
    }
    anchor("LAUNCH-BTN-reveal");
    ImGui::SameLine(0.0f, 14.0f);
    if (ImGui::Button(TR("Remove from list"), ImVec2(180, 32))) {
        m_recent.remove(it.path);
        m_recent.save(nullptr);
        VK_LOG_INFO("LAUNCHER-REMOVE path=%s", it.path.c_str());
        refreshRecents(false);
    }
    anchor("LAUNCH-BTN-remove");
}

void LauncherApp::drawFooter() {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("##footer", ImVec2(0, kFooterH), false,
                      ImGuiWindowFlags_NoScrollbar);

    // 通栏分隔线（顶栏用 headerBand 的亮线，这里用一条软边框就够）。
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetWindowPos();
        const ImVec2 sz = ImGui::GetWindowSize();
        dl->AddLine(ImVec2(p0.x, p0.y + 0.5f),
                    ImVec2(p0.x + sz.x, p0.y + 0.5f),
                    ImGui::GetColorU32(ui::tokens().borderSoft), 1.0f);
    }
    const float rowY = (kFooterH - 40.0f) * 0.5f;
    ImGui::SetCursorPos(ImVec2(kPad, rowY));

    if (m_page == Page::Idle) {
        if (ui::pillButton("newGame", TR("+ New game"), ImVec2(210, 40), 1))
            beginWizard();
        anchor("LAUNCH-BTN-new");
    } else if (m_page == Page::Done) {
        if (ImGui::Button(TR("Open editor"), ImVec2(170, 40)))
            requestSpawn(SpawnKind::Editor, m_create.root);
        anchor("LAUNCH-BTN-doneOpen");
        ImGui::SameLine(0.0f, 12.0f);
        if (ImGui::Button(TR("Run game"), ImVec2(150, 40)))
            requestSpawn(SpawnKind::Game, m_create.root);
        anchor("LAUNCH-BTN-doneRun");
        ImGui::SameLine(0.0f, 12.0f);
        if (ImGui::Button(TR("Close"), ImVec2(120, 40))) {
            m_page = Page::Idle;
            refreshRecents(false);
        }
        anchor("LAUNCH-BTN-doneClose");
    } else {
        // 向导：左"取消"，右"创建"。单页向导没有"上一步/下一步"了 ——
        // 三段内容同屏（见 drawWizard），导航这个概念在这里没有意义。
        if (ImGui::Button(TR("Cancel"), ImVec2(120, 40))) cancelWizard();
        anchor("LAUNCH-BTN-cancel");

        // ⚠ Y 必须用 rowY，不能取"画完取消之后的光标"：那颗按钮已经让光标
        //   下移了 40 + ItemSpacing，照抄就会把"创建"顶到底栏外面去
        //   （实测刚好被窗口下沿裁掉，只剩一个粉色边）。
        ImGui::SetCursorPos(ImVec2(io.DisplaySize.x - kPad - 240.0f, rowY));
        if (ui::pillButton("create", TR("Create and open editor"),
                           ImVec2(240, 40), 1))
            commitWizard();
        anchor("LAUNCH-BTN-create");
        ImGui::EndChild();
        ImGui::PopStyleColor();
        return;
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
    ImGui::SameLine(io.DisplaySize.x - tw - kPad);
    ImGui::TextDisabled("%s", status);

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ------------------------------------------------------------ 向导

void LauncherApp::drawWizard() {
    // ============================================================
    // 单页向导（2026-11-04 用户第 2 条）
    // ============================================================
    // 用户原话：「在点击新建游戏按钮时，1.类型，2.位置，3.画面，这三步完全
    // 可以放在一个画面里啊，不需要三个子画面去分开逐步点击。」
    //
    // 于是三段同屏，布局是"上面一排模板卡 + 下面左右两栏设置"：
    //
    //   ┌ 新建游戏 ────────────────────────────────────────┐
    //   │ 1 类型   [卡1][卡2][卡3][卡4]                     │
    //   │ ┌ 2 位置 ──────────┐ ┌ 3 画面 ──────────────────┐ │
    //   │ │ 名称/父目录/文件夹 │ │ 分辨率/全屏/画质/将写入  │ │
    //   │ └──────────────────┘ └─────────────────────────┘ │
    //   └──────────────[取消]                    [创建]────┘
    //
    // 为什么不留"上一步/下一步"：三段都看得见，没有"下一步"要表达的东西。
    // 序号 1/2/3 保留在分区标题里 —— 心智模型不变，只是不再需要翻页。
    ImGui::BeginChild("##wiz", ImVec2(0, 0), false,
                      ImGuiWindowFlags_NoScrollbar);

    ImGui::SetWindowFontScale(1.35f);
    ImGui::TextUnformatted(TR("New game"));
    ImGui::SetWindowFontScale(1.0f);
    ImGui::SameLine(0.0f, 16.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, ui::tokens().textDim);
    ImGui::TextUnformatted(
        TR("Pick a template, then set where it lives and how it looks."));
    ImGui::PopStyleColor();
    ImGui::Spacing();

    // ---- 1 类型：一排 4 张 ----
    drawWizardType();

    ImGui::Spacing();
    ImGui::Spacing();

    // ---- 2 位置 / 3 画面：左右两栏，各自填满剩余高度 ----
    const float availW = ImGui::GetContentRegionAvail().x;
    const float colH = ImGui::GetContentRegionAvail().y;
    const float colW = (availW - 20.0f) * 0.5f;
    // 两栏内部行距收紧一档：设置项是"标签 + 控件"两行结构，用全局的
    // 9/7 会让三段内容超出可用高度（超出就出现列内滚动条，而"创建"在
    // 底栏 —— 用户会以为下面还有东西没填）。
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));
    ImGui::BeginChild("##wizLoc", ImVec2(colW, colH),
                      ImGuiChildFlags_Borders |
                          ImGuiChildFlags_AlwaysUseWindowPadding);
    drawWizardLocation();
    ImGui::EndChild();
    ImGui::SameLine(0.0f, 20.0f);
    ImGui::BeginChild("##wizDisp", ImVec2(0, colH),
                      ImGuiChildFlags_Borders |
                          ImGuiChildFlags_AlwaysUseWindowPadding);
    drawWizardDisplay();
    ImGui::EndChild();
    ImGui::PopStyleVar();   // 两栏收紧的行距

    ImGui::EndChild();
}

// ------------------------------------------------------------ 类型卡片插图

void LauncherApp::ensureTypeArt() {
    if (m_typeArtTried) return;
    m_typeArtTried = true;

    // 顺序 = `project::gameTypeAt()` 的顺序（第一人称 / 第三人称 /
    // 等距俯视 / 卡牌对战）。文件名与枚举 id 对应，别按"第几张"命名 ——
    // 那样在中间插一种类型时，图会整体错位一格（且不报错）。
    const char* files[project::kGameTypeCount] = {
        "launcher/type_first_person.png",
        "launcher/type_third_person.png",
        "launcher/type_isometric.png",
        "launcher/type_card_battler.png",
    };
    const char* keys[project::kGameTypeCount] = {
        "launcher:type:first_person", "launcher:type:third_person",
        "launcher:type:isometric", "launcher:type:card_battler",
    };
    for (int i = 0; i < project::kGameTypeCount; ++i) {
        const std::string abs = assets::resolveAssetPath(files[i]);
        assets::Texture* tex = nullptr;
        try {
            // 缓存键单独给：`loadTexture(path, name)` 的 name 就是缓存键。
            // 不传的话键=路径，"以后换了插图文件名"会命中旧纹理 —— 症状是
            // "我明明换了图，界面还是老图"，而日志里一行提示都没有。
            const std::string key = keys[i];
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

    sectionHeader("1", TR("Type"));
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ui::tokens().textDim);
    // 模板说明并进标题行（原来是卡片下面独立一行）：三段同屏要省高度，
    // 而这句话属于"看一眼就够"的信息，不占独立一行。
    ImGui::TextUnformatted(TR("What kind of game is this?"));
    ImGui::PopStyleColor();
    ImGui::Spacing();

    struct Card {
        const char* title;
        const char* line1;
        const char* line2;
    };
    // ⚠ **表顺序就是 `project::gameTypeAt()` 的顺序**，`GameType` 由下标反查。
    //   这里再写一遍 `GameType::XXX` 就是第二份真值：加一种类型时只改一处、
    //   漏改另一处不报错 —— 卡片画出来了、点下去选的是**另一个**类型。
    const Card cards[project::kGameTypeCount] = {
        {"First person", "Camera sits at eye level",
         "The mouse turns the view directly"},
        {"Third person", "Camera sits behind and above",
         "Mouse orbits, character turns"},
        // 2026-11-04：玩法层（属性/状态/伤害/回合/牌堆/配表）做完之后，
        // "游戏类型"就不该只问相机在哪 —— 这两张才是回合制/卡牌的入口。
        {"Isometric", "Camera looks down at an angle",
         "Turn-based tactics: pick a tile, then act"},
        {"Card battler", "Camera is a card table",
         "Hand, deck and discard drive the fight"},
    };

    // 卡片 = 16:9 插图 + 底部标题条。
    //
    // 2026-11-04 版式：4 张卡**横排一行**（原来是 2×2 两行）。横排把高度
    // 从 ~500px 压到 ~250px，省下来的高度正好给下面的"位置/画面"两栏 ——
    // 三段同屏就靠这个。
    const float gap = kCardGap;
    const float availW = ImGui::GetContentRegionAvail().x;
    float cardW = (availW - gap * 3.0f - 6.0f) / 4.0f;
    // 上限 320：4 张横排时 340 会把两栏区挤到只剩 ~430px，而"位置"栏需要
    // 460 左右（三个"标签+输入框"+ 三个勾选框）。差这 20px 就是"最后一个
    // 勾选框被底边裁掉"和"完整可见"的区别。
    if (cardW > 320.0f) cardW = 320.0f;   // 窗口再宽也不许拉成一条横幅
    if (cardW < 200.0f) cardW = 200.0f;
    const float imgH = cardW * 9.0f / 16.0f;
    const float textH = 42.0f;
    const float cardH = imgH + textH;

    const ImVec4 acc = ui::tokens().accent;
    for (int i = 0; i < project::kGameTypeCount; ++i) {
        const Card& c = cards[i];
        const project::GameType type = project::gameTypeAt(i);
        const bool sel = (m_wizType == type);
        if (i > 0) ImGui::SameLine(0.0f, gap);
        ImGui::PushID(i);

        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        // 整张卡就是一个 InvisibleButton —— hover / 点击都在**父窗口**里算。
        //
        // ⚠ 别换成 `BeginChild` 画卡片：子窗口是独立窗口，会把后面画在父
        //   窗口里的 item 的 hover 全挡住（"卡片画出来了、点下去没反应"，
        //   历史上为此丢过三轮向导）。InvisibleButton 什么都不画，正好。
        const bool clicked =
            ImGui::InvisibleButton("##type", ImVec2(cardW, cardH));
        const bool hov = ImGui::IsItemHovered();
        // 锚点编号 = 卡片下标 = `gameTypeAt()` 的顺序（判据按它点卡片）。
        char tag[32];
        std::snprintf(tag, sizeof(tag), "LAUNCH-TYPE-%d", i);
        anchor(tag);   // 必须在 item 之后立刻取矩形

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p1(p0.x + cardW, p0.y + cardH);
        const float rounding = 10.0f;
        // 底色/描边一律从主题取：写死深色在浅色主题下会变成一块黑斑。
        dl->AddRectFilled(
            p0, p1,
            ImGui::GetColorU32(sel ? ImVec4(acc.x, acc.y, acc.z, 0.28f)
                                   : (hov ? ui::tokens().cardBgHover
                                          : ui::tokens().cardBg)),
            rounding);

        // 插图区：图就是 16:9，卡片这块也是 16:9 → UV 直接整张给。
        const ImVec2 i0(p0.x, p0.y), i1(p0.x + cardW, p0.y + imgH);
        const std::uint64_t ds = m_typeArt[i];
        if (ds != 0) {
            dl->AddImageRounded(static_cast<ImTextureID>(ds), i0, i1,
                                ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                                sel ? IM_COL32(255, 255, 255, 255)
                                    : IM_COL32(255, 255, 255, hov ? 235 : 205),
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
            dl->AddRectFilled(i0, i1, ImGui::GetColorU32(ui::tokens().panelBg),
                              rounding, ImDrawFlags_RoundCornersTop);
        }

        // ⚠ 这里不要用 SetWindowFontScale 调标题字号：它只影响**下一个
        //   item** 的字体，而 AddText 用的是 draw list 当前的字体尺寸 ——
        //   调了等于没调，还容易让人以为"标题小了一号"。字号统一由
        //   kFontScale 决定。
        dl->AddText(ImVec2(p0.x + 14.0f, p0.y + imgH + 12.0f),
                    ImGui::GetColorU32(ImGuiCol_Text), TR(c.title));
        // 选中：强调色描边；hover：灰白描边。
        dl->AddRect(p0, p1,
                    sel ? ImGui::GetColorU32(acc)
                        : ImGui::GetColorU32(hov ? ui::tokens().textDim
                                                 : ui::tokens().borderSoft),
                    rounding, 0, sel ? 2.0f : 1.0f);

        if (clicked) {
            m_wizType = type;
            VK_LOG_INFO("LAUNCHER-WIZARD type=%s index=%d",
                        project::gameTypeId(type), i);
        }
        // 注释文字**只在悬浮时**出现（用户要求：卡面保持干净）。
        if (hov) ImGui::SetTooltip("%s\n%s", TR(c.line1), TR(c.line2));

        ImGui::PopID();
    }
}

void LauncherApp::drawWizardLocation() {
    sectionHeader("2", TR("Location"));
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ui::tokens().textDim);
    ImGui::TextUnformatted(TR("Where should the project live?"));
    ImGui::PopStyleColor();
    ImGui::Spacing();

    // 字段一律"标签在上、输入框拉满列宽"（UE5 的设置页排布）。
    // 竖排而不是横排，是因为这一栏只有半屏宽 —— 横排时标签会挤在输入框
    // 右边溢出列外。
    constexpr float kBrowseW = 104.0f;

    // "工程名"和"上级文件夹"并排一行。
    // ⚠ 竖着排（各占"标签+输入框"两行 = 68px × 2）时这一栏会超出一屏，
    //   最后那颗"创建后打开 VS"被底边裁掉 17px —— 而它恰是用户上次抱怨
    //   "没自动开 VS"的那个开关，绝不能再藏到看不见的地方。
    const float nameW = 210.0f;
    constexpr float kColGap = 14.0f;
    const float colAvail = ImGui::GetContentRegionAvail().x;

    ImGui::BeginGroup();
    fieldLabel(TR("Project name"));
    ImGui::SetNextItemWidth(nameW);
    char nameBuf[256];
    std::snprintf(nameBuf, sizeof(nameBuf), "%s", m_wizName.c_str());
    if (ImGui::InputText("##wizname", nameBuf, sizeof(nameBuf))) {
        m_wizName = nameBuf;
        if (!m_wizRootTouched) syncWizardRoot();
    }
    anchor("LAUNCH-INPUT-name");
    ImGui::EndGroup();

    ImGui::SameLine(0.0f, kColGap);

    ImGui::BeginGroup();
    fieldLabel(TR("Parent folder"));
    ImGui::SetNextItemWidth(colAvail - nameW - kColGap - kBrowseW - 10.0f);
    char parentBuf[1024];
    std::snprintf(parentBuf, sizeof(parentBuf), "%s", m_wizParent.c_str());
    if (ImGui::InputText("##wizparent", parentBuf, sizeof(parentBuf))) {
        m_wizParent = parentBuf;
        if (!m_wizRootTouched) syncWizardRoot();
    }
    anchor("LAUNCH-INPUT-parent");
    ImGui::SameLine(0.0f, 10.0f);
    if (ImGui::Button(TR("Browse..."), ImVec2(kBrowseW, 0))) {
        const std::string pick =
            pickFolder(TR("Select parent folder"), m_wizParent);
        if (!pick.empty()) {
            m_wizParent = pick;
            if (!m_wizRootTouched) syncWizardRoot();
        }
    }
    anchor("LAUNCH-BTN-browse");
    ImGui::EndGroup();

    ImGui::Spacing();
    fieldLabel(TR("Project folder"));
    ImGui::SetNextItemWidth(-1.0f);
    char rootBuf[1024];
    std::snprintf(rootBuf, sizeof(rootBuf), "%s", m_wizRoot.c_str());
    if (ImGui::InputText("##wizroot", rootBuf, sizeof(rootBuf))) {
        m_wizRoot = rootBuf;
        // 手改过工程文件夹之后，"名字改 → 路径跟着变"的联动**断开**。
        // 不断开的话，用户改完名字会把刚手敲的路径冲掉（而且不提示）。
        m_wizRootTouched = true;
    }
    anchor("LAUNCH-INPUT-root");

    // 目标目录可用性：**就地红字**，不弹模态（路线图 §5.3 Step2）。
    const project::IoResult usable = project::checkTargetDirUsable(m_wizRoot);
    if (!usable.ok) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::tokens().error);
        ImGui::TextWrapped("%s", usable.error.c_str());
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::tokens().ok);
        ImGui::TextWrapped("%s", TR("This folder is free."));
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ---- 脚本 / 代码（M5-c）----
    //
    // 这一节原来在"第 2 步"、创建按钮在"第 3 步"，用户从没看见过这两个
    // 勾选框 —— 于是"建完没有 code/、也没开 VS"看起来像功能缺失。
    // 现在三段同屏，勾选框和创建按钮之间只隔一眼。
    if (ImGui::Checkbox(TRL("Keep the script folder outside the project##wizext"),
                        &m_wizExternal)) {
        VK_LOG_INFO("LAUNCHER-WIZARD code-external=%d", m_wizExternal ? 1 : 0);
    }
    anchor("LAUNCH-CHK-external");
    if (m_wizExternal) {
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - kBrowseW -
                                 10.0f);
        char codeBuf[1024];
        std::snprintf(codeBuf, sizeof(codeBuf), "%s", m_wizCodeRoot.c_str());
        if (ImGui::InputText("##wizcode", codeBuf, sizeof(codeBuf)))
            m_wizCodeRoot = codeBuf;
        anchor("LAUNCH-INPUT-coderoot");
        ImGui::SameLine(0.0f, 10.0f);
        if (ImGui::Button(TRL("Browse...##code"), ImVec2(kBrowseW, 0))) {
            const std::string pick =
                pickFolder(TR("Select script folder"), m_wizCodeRoot);
            if (!pick.empty()) m_wizCodeRoot = pick;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ui::tokens().textDim);
        ImGui::TextWrapped("%s",
                           TR("A directory junction is created at "
                              "assets/scripts. No admin rights needed."));
        ImGui::PopStyleColor();
    }
    // ⚠ 不勾"放到工程外"时**不再**补一句"脚本在 assets/scripts（*.vks）会被
    //   热重载"：勾选框的标签已经说了这件事，而这句话要占两行 —— 两行正好
    //   把这一栏最后那颗"创建后打开 VS"顶到可视区外面（实测 y=959 > 客户区
    //   960 的底边，最后 4px 被裁）。


    ImGui::Spacing();
    if (ImGui::Checkbox(
            TRL("Also generate a C++ game module (Visual Studio project)##wizcpp"),
            &m_wizCpp)) {
        VK_LOG_INFO("LAUNCHER-WIZARD code-cpp=%d", m_wizCpp ? 1 : 0);
    }
    anchor("LAUNCH-CHK-cpp");
    if (m_wizCpp) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::tokens().textDim);
        ImGui::TextWrapped(
            "%s", TR("A ready-to-build VS project is created in code/. "
                     "Changing it needs a rebuild - keep gameplay in Lua."));
        ImGui::PopStyleColor();
        if (ImGui::Checkbox(TRL("Open Visual Studio after creating##wizide"),
                            &m_wizOpenIde)) {
            VK_LOG_INFO("LAUNCHER-WIZARD open-ide=%d", m_wizOpenIde ? 1 : 0);
        }
        anchor("LAUNCH-CHK-openide");
    }
}

void LauncherApp::drawWizardDisplay() {
    sectionHeader("3", TR("Display"));
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ui::tokens().textDim);
    ImGui::TextUnformatted(TR("Display and quality"));
    ImGui::PopStyleColor();
    ImGui::Spacing();

    fieldLabel(TR("Resolution"));
    ImGui::SetNextItemWidth(-1.0f);
    const char* resItems[kResCount];
    for (int i = 0; i < kResCount; ++i) resItems[i] = TR(kResPresets[i].label);
    if (ImGui::Combo("##wizres", &m_wizRes, resItems, kResCount)) {
        VK_LOG_INFO("LAUNCHER-WIZARD res=%d", m_wizRes);
    }
    anchor("LAUNCH-COMBO-res");

    if (m_wizRes == kResCount - 1) {
        const float halfW =
            (ImGui::GetContentRegionAvail().x - 10.0f) * 0.5f;
        ImGui::SetNextItemWidth(halfW);
        ImGui::InputInt("##wizw", &m_wizCustomW, 0, 0);
        anchor("LAUNCH-INPUT-width");
        ImGui::SameLine(0.0f, 10.0f);
        ImGui::SetNextItemWidth(halfW);
        ImGui::InputInt("##wizh", &m_wizCustomH, 0, 0);
        anchor("LAUNCH-INPUT-height");
        if (m_wizCustomW < 320) m_wizCustomW = 320;
        if (m_wizCustomH < 240) m_wizCustomH = 240;
    }

    ImGui::Spacing();
    fieldLabel(TR("Quality"));
    ImGui::SetNextItemWidth(-1.0f);
    // 画质档名**从 render:: 取**，不硬编码 —— 那边加一档这里自动出现
    // （这是把预设表下沉到 engine/render 的直接收益）。
    const int qn = render::qualityPresetCount();
    static const char* qItems[16];
    for (int i = 0; i < qn && i < 16; ++i) qItems[i] = render::qualityPresetName(i);
    if (ImGui::Combo("##wizq", &m_wizQuality, qItems, qn < 16 ? qn : 16)) {
        VK_LOG_INFO("LAUNCHER-WIZARD quality=%s",
                    render::qualityPresetName(m_wizQuality));
    }
    anchor("LAUNCH-COMBO-quality");

    ImGui::Checkbox(TR("Fullscreen"), &m_wizFullscreen);
    anchor("LAUNCH-CHK-fullscreen");

    // ---- "将写入"清单 ----
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
    // 代码工程**不在** game.json / render.json 里，所以 describeWrites 列不到它
    // —— 但它确实是"点创建会被写出来的东西"，必须在同一份清单里出现。
    // 这一行的存在本身就是修一个可见性 bug：勾选框和创建按钮之间没有任何
    // 反馈时，"建完没有 code/、也没开 VS"看起来像功能缺失，其实只是没勾。
    if (m_wizCpp) {
        ImGui::Bullet();
        ImGui::SameLine();
        // 路径 + 产品名，不需要翻译；后面那句提示才进词典。
        ImGui::Text("code/%s.sln", project::moduleIdFromName(m_wizName).c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", m_wizOpenIde
                                      ? TR("(opens in Visual Studio after creating)")
                                      : TR("(Visual Studio not opened automatically)"));
    } else {
        ImGui::Bullet();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", TR("code/ skipped - scripts only"));
    }
    anchor("LAUNCH-WRITES");

    // 创建失败的**可读原因**（留在本页，不跳页）
    if (!m_create.ok && !m_create.error.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ui::tokens().error);
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
