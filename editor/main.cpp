// ============================================================
// editor/main.cpp —— 编辑器入口
// ============================================================
// 与 game/main.cpp 完全平行：同一个 core::Application 主循环，
// 换一个子类而已。两个可执行目标共用 engine/，互相之间没有依赖。
//
// 项目的主入口就是它：`make run` 直接打开编辑器界面。游戏画面不用
// 另开窗口 —— 在编辑器里点工具条的 Play，场景会在**原视口**里跑起来
// （见 editor/EditorContext.h 的"编辑态 / 运行态分离"），按 F 还能让
// 视口占满整个窗口。
//
// ------------------------------------------------------------
// 环境变量
// ------------------------------------------------------------
//   MYVK_FRAMES=N        跑满 N 帧自动退出（无人值守冒烟测试用）
//   MYVK_EDITOR_PLAY=1   启动即进入 Play（配合 MYVK_FRAMES 验证播放链路）
//   MYVK_VALIDATION=0    关闭 Vulkan 验证层
//   MYVK_SHADER_DIR      覆盖着色器目录
// ============================================================

#include "EditorApp.h"

#include "core/Logger.h"

#include <cstdlib>
#include <exception>

namespace {

uint64_t envFrames(const char* name) {
    if (const char* env = std::getenv(name)) {
        try {
            return static_cast<uint64_t>(std::stoull(env));
        } catch (...) {
            return 0;
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    try {
        const uint64_t maxFrames = envFrames("MYVK_FRAMES");
        VK_LOG_INFO("=== Venn Engine - Editor ===");

        editor::EditorApp app;
        return app.run(maxFrames);
    } catch (const std::exception& e) {
        VK_LOG_ERROR("Fatal: %s", e.what());
        return 1;
    }
}
