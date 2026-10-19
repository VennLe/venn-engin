// ============================================================
// launcher/main.cpp —— 引擎启动器入口
// ============================================================
// 第三个可执行目标（`VennLauncher` → `Launcher.exe`），与 `Editor.exe` /
// `Game.exe` 平行：同一个 `core::Application` 主循环，换一个子类。
//
// 它自己的职责只有三件：
//   ① 列出「最近工程」并让用户选一个 → 拉起 `Editor.exe` / `Game.exe`
//   ② 用四步向导从模板建一个新工程
//   ③ 把「画面设置」写进工程的既有配置文件（`assets/game.json` /
//      `config/render.json`）—— **不引入第二份配置**（路线图 D3）
//
// ------------------------------------------------------------
// 为什么这个 exe **同时**认 argv 和 env（与 `Editor.exe` 不同）
// ------------------------------------------------------------
// `Editor.exe` 的 `main` 里是 `(void)argc; (void)argv;` —— 传 `--frames`
// 不会有任何提示地无效（记忆里踩过：它会照常渲染到超时，外部零日志）。
// 那是历史包袱，路线图 D7 明确"先不动它"。
//
// 启动器是新文件，没有这个包袱，所以两条都收：
//   `Launcher.exe --selftest project`   等价于 `MYVK_PROJECT_TEST=1`
// 并且**在日志里说清楚走的是哪一条** —— 免得将来某次"命令行没生效"又要
// 靠猜。
// ============================================================

#include "LauncherApp.h"

#include "core/Logger.h"
#include "project/ProjectFile.h"

#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

namespace {

std::string envStr(const char* name) {
    const char* v = std::getenv(name);
    return (v && v[0]) ? std::string(v) : std::string();
}

} // namespace

int main(int argc, char** argv) {
    try {
        VK_LOG_INFO("=== Venn Engine - Launcher ===");

        // ---- 自检入口（在 Vulkan / 窗口之前返回）----
        // 纯数值 + 临时目录 IO：路径规则 / 逃逸拒绝 / 序列化往返 / 版本拒绝 /
        // 目标目录冲突 / 模板查找 / 创建与回滚 / junction / 画面设置 / 最近列表。
        // 不开窗口也不要 GPU，与 `Game.exe --selftest project` 同一套用例。
        std::string selftest = envStr("MYVK_PROJECT_TEST") == "1" ? "project"
                                                                 : std::string();
        if (selftest.empty() && argc >= 3 && std::strcmp(argv[1], "--selftest") == 0)
            selftest = argv[2];
        if (!selftest.empty()) {
            if (selftest != "project") {
                VK_LOG_WARN("unknown selftest '%s' (available: project)",
                            selftest.c_str());
                return 1;
            }
            VK_LOG_INFO("LAUNCHER-SELFTEST name=project");
            return project::runAllProjectSelfTests() == 0 ? 0 : 1;
        }

        launcher::LauncherApp app;
        return app.run();
    } catch (const std::exception& e) {
        VK_LOG_ERROR("Fatal: %s", e.what());
        return 1;
    }
}
