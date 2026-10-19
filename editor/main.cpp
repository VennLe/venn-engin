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
//   MYVK_PHYS_TEST=1     **只跑物理自检然后退出**（不初始化 Vulkan、不开窗）
//                        —— 见 physics/PhysicsSelfTest.h 与 tools/verify_physics.py
//   MYVK_RENDER_SETTINGS_TEST=1
//                        **只跑渲染设置层级自检然后退出**（不初始化 Vulkan、不开窗）
//                        —— 见 render/RenderSettingsIO.h 与
//                        tools/verify_settings_layers.py
// ============================================================

#include "EditorApp.h"

#include "core/Logger.h"
#include "animation/AnimationSelfTest.h"
#include "physics/PhysicsSelfTest.h"
#include "project/ProjectFile.h"
#include "render/RenderSettingsIO.h"
#include "ui2/Ui2SelfTest.h"

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

        // 物理自检：**在 Vulkan / 窗口之前**返回。物理模块不依赖渲染，
        // 所以这条路径能在没有 GPU、没有显示器、跑得飞快的情况下验证
        // 数值正确性（见 physics/PhysicsSelfTest.h 的说明）。
        if (std::getenv("MYVK_PHYS_TEST") != nullptr) {
            const int failures = physics::runPhysicsSelfTest();
            return failures == 0 ? 0 : 1;
        }

        // E03 渲染设置层级自检：同一条"在 Vulkan / 窗口之前返回"的路径。
        // 覆盖链 / 解析 / 落盘都是纯数据逻辑，不需要设备 —— 也正因为如此
        // 它能在 CI 里秒级跑完（见 render/RenderSettingsIO.h）。
        if (std::getenv("MYVK_RENDER_SETTINGS_TEST") != nullptr) {
            const int failures = render::runSettingsSelfTest();
            return failures == 0 ? 0 : 1;
        }

        // 骨骼动画自检（角色链 A0-3/A0-5）：同样在 Vulkan 之前返回。
        // 覆盖"时间推进 / 循环回绕 / 逐位重构不变性 / 混合端点 / 层次顺序"，
        // 全部是纯数值 —— 这条链过去一个用例都没有，正是因为采样埋在
        // Renderer 里、必须开 GUI 才能验证。见 animation/AnimationSelfTest.h。
        if (std::getenv("MYVK_ANIM_TEST") != nullptr) {
            const int failures = animation::runAnimationSelfTest();
            return failures == 0 ? 0 : 1;
        }

        // 游戏 UI 自检（U0~U6）：同样在 Vulkan 之前返回。覆盖缩放公式 /
        // 默认值 / 字体度量 / 文本换行 / 布局比例 / 焦点导航 —— 全是纯 CPU
        // 数值，不需要设备。见 ui2/Ui2SelfTest.h。
        if (std::getenv("MYVK_UI2_TEST") != nullptr) {
            const int failures = ui2::runUi2SelfTest();
            return failures == 0 ? 0 : 1;
        }

        // 工程数据模型自检（启动器 M0/M2）：与上面四条同一条"Vulkan 之前
        // 返回"的路。⚠ 走 **env** 而不是 argv —— `Editor.exe` 丢弃命令行
        // （见文件头 P1：`Editor.exe --frames` 会照常渲染到超时且零提示）。
        if (std::getenv("MYVK_PROJECT_TEST") != nullptr) {
            const int failures = project::runProjectSelfTest();
            return failures == 0 ? 0 : 1;
        }

        editor::EditorApp app;
        return app.run(maxFrames);    } catch (const std::exception& e) {
        VK_LOG_ERROR("Fatal: %s", e.what());
        return 1;
    }
}
