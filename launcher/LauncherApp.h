#pragma once
// ============================================================
// launcher/LauncherApp —— 引擎启动器（路线图 D1/D2）
//
// 它是**第三个应用层**，与 editor/、runtime/ 平行：
//
//     launcher/ ─┐
//     editor/   ─┼─►  engine/
//     runtime/  ─┘
//
// 三者互不依赖。启动器**不** include 任何 `editor/` 头（连 `editor::tr`
// 都不用，直接走 `i18n::tr` —— 那才是唯一权威，编辑器那边只是转发壳）。
//
// ------------------------------------------------------------
// 页面状态机（路线图 §5.4）
// ------------------------------------------------------------
//   Idle ──新建──► Type ─► Location ─► Display ──创建──► Done
//     ▲                └──取消─────────────────────┘
//   Idle ──选中+打开──► (拉起编辑器) → 退出
//
// ⚠ 创建**不是**异步的：四个阶段（校验 / 复制 / 替换 / 提交）都在同一个
//   帧里跑完，一秒钟都不需要。进度条画的是"这四步各走过了"，不是
//   "还差多久"——所以它只会一帧闪过。做成线程池只会引入"UI 读半成品
//   状态"这一类新 bug，而收益是零。
//
// ------------------------------------------------------------
// 自动化钩子（全部 `MYVK_LAUNCHER_*`）
// ------------------------------------------------------------
//   MYVK_LAUNCHER_RECENT=<path>   最近列表落点（默认 %LOCALAPPDATA%/Venn/launcher/recent.json）
//   MYVK_LAUNCHER_TEST_ROOT=<dir> 新建向导 Step2 的父目录初值（省掉"点浏览…"）
//   MYVK_LAUNCHER_TEST_NAME=<name> 新建向导 Step2 的工程名初值
//   MYVK_LAUNCHER_PAGE=new        启动直接进新建向导 Step1
//   MYVK_LAUNCHER_TEST_RES=<i>    新建向导 Step3 的分辨率档初值（0/1/2/3）
//   MYVK_LAUNCHER_TEST_QUALITY=<i> 新建向导 Step3 的画质档初值（0..4）
//   MYVK_LAUNCHER_TEST_CODE_EXTERNAL=<dir> 外置代码根的**路径初值**
//                                 （⚠ 复选框仍需真点，不代勾）
//   MYVK_LAUNCHER_NO_PRUNE=1      启动时**不**剔除失效条目（"空列表"用例要用）
//
// ⚠ 这几条**不是**"绕过 UI 的后门**：它们只填初值，四步向导仍然一步不少
//   地走完、按钮仍然要真点。区别只是不用在测试里模拟"往系统文件对话框
//   里敲一串带中文的临时路径"或"点开 Combo 的弹出项再选第 N 行"——
//   那两件事在 CI 上根本没法可靠地做（弹出项没有锚点，位置随内容浮动）。
// ============================================================

#include "core/Application.h"
#include "project/ProjectCreate.h"
#include "project/ProjectDisplay.h"
#include "project/ProjectFile.h"
#include "project/RecentList.h"

#include <string>
#include <vector>

namespace launcher {

class LauncherApp : public core::Application {
public:
    LauncherApp();
    ~LauncherApp() override;

    // D2 的补丁 1：启动器没有着色器/场景/纹理要预编译，那 2 秒缓冲
    // 只会让用户以为"双击没反应"。见 core/Application.h 的说明。
    double startupShowDelay() const override { return 0.0; }

protected:
    void onInit() override;
    void onUpdate(float dt) override;
    void onImGui() override;
    void onShutdown() override;

private:
    // ---- 页面 ----
    enum class Page { Idle, Type, Location, Display, Done };
    enum class SpawnKind { None, Editor, Game };

    // ---- 绘制 ----
    void drawHeader();
    void drawIdle();
    void drawRecentColumn();
    void drawDetailColumn();
    void drawFooter();
    void drawWizard();
    void drawWizardType();
    void drawWizardLocation();
    void drawWizardDisplay();
    void drawDone();
    void drawToasts();

    // ---- 行为 ----
    void refreshRecents(bool prune);
    void beginWizard();
    void cancelWizard();
    void syncWizardRoot();
    void commitWizard();
    void openSelected();
    void openProjectAt(const std::string& root);
    void requestSpawn(SpawnKind kind, const std::string& root);
    void runPendingSpawn();
    void toast(const std::string& text, bool error);

    // ---- 最近的打开项（右栏详情）----
    project::ProjectFile m_detail;
    bool m_detailOk = false;
    std::string m_detailError;

    // ---- 状态 ----
    project::RecentList m_recent;
    int m_selected = -1;
    Page m_page = Page::Idle;

    // ---- 新建向导 ----
    project::GameType m_wizType = project::GameType::ThirdPerson;
    std::string m_wizName = "MyGame";
    std::string m_wizParent;
    std::string m_wizRoot;
    bool m_wizRootTouched = false;
    bool m_wizExternal = false;
    std::string m_wizCodeRoot;
    int m_wizRes = 1;            // 0=1280x720 1=1920x1080 2=2560x1440 3=自定义
    int m_wizCustomW = 1280;
    int m_wizCustomH = 720;
    bool m_wizFullscreen = false;
    int m_wizQuality = 2;        // High

    // ---- 创建结果 ----
    project::CreateResult m_create;
    std::vector<std::string> m_createTrace;   // 四步各一条，Done 页列出来

    // ---- 拉起 ----
    SpawnKind m_pendingSpawn = SpawnKind::None;
    std::string m_pendingRoot;
    std::string m_lastSpawnError;

    // ---- 提示 ----
    struct Toast {
        std::string text;
        bool error = false;
        double until = 0.0;
    };
    std::vector<Toast> m_toasts;
    double m_now = 0.0;

    bool m_initialized = false;
};

} // namespace launcher
