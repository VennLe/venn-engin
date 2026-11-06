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

#include "LayoutSelfTest.h"
#include "MultiSelectSelfTest.h"

#include "core/Logger.h"
#include "animation/AnimationSelfTest.h"
#include "character/MannequinSkeleton.h"
#include "character/MannequinSelfTest.h"
#include "character/ViewRigSelfTest.h"
#include "assets/AssetSelfTest.h"
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

        // 第一 / 第三人称装配自检（2026-11-06）：同一条"Vulkan 之前返回"
        // 的路。盖眼位（骨架头骨）/ 帧率无关的阻尼 / 换视角过渡 / 可见性。
        // 这三类问题在截图里只表现成"看着有点怪"，必须用数值钉住。
        if (std::getenv("MYVK_VIEWRIG_TEST") != nullptr) {
            const int failures = character::runViewRigSelfTest();
            return failures == 0 ? 0 : 1;
        }

        // UE5 Mannequin 骨架自检（2026-11-06）：骨表（35 根）/ 重定向后
        // **世界姿势逐位不变** / ik_* 与 weapon_* 是非形变骨 / 动画轨道
        // 下标重映射 / 对 UE 骨架再跑一遍的幂等性。
        // 这个用例存在的理由：重定向错了不会报错，只会"人歪了一点、动画扭
        // 错一根骨"，截图里全是"看着还行"。
        if (std::getenv("MYVK_MANNEQUIN_TEST") != nullptr) {
            const int failures = character::runMannequinSelfTest();
            return failures == 0 ? 0 : 1;
        }

        // `MYVK_MANNEQUIN_EMIT=<path>`：把 UE 标准骨表导出成独立骨架资产
        // `.vskel`（`assets/characters/mannequin/Rigs/SK_Mannequin.vskel`）。
        //
        // 为什么是 env 而不是 AssetC 的子命令：骨表在 **engine/character**
        // （`MannequinBoneDef`），而 `assetio` 按设计不认识 engine/ ——
        // 把表抄一份到 assetc 就是"两份骨表"，改了漏一处那种。所以让唯一
        // 能看到两边的那一层（Editor 的启动钩子）做这一次性导出，
        // 产物落在 assets 里之后，AssetC 只负责读它。
        if (const char* emitPath = std::getenv("MYVK_MANNEQUIN_EMIT")) {
            if (emitPath[0] == '\0') {
                std::fprintf(stderr, "MYVK_MANNEQUIN_EMIT needs a path\n");
                return 2;
            }
            std::string err;
            if (!character::writeMannequinSkeletonAsset(emitPath, &err)) {
                std::fprintf(stderr, "MANNEQUIN-SKEL FAIL %s (%s)\n", emitPath,
                             err.c_str());
                return 1;
            }
            return 0;
        }

        // 离线资产链自检（.vmesh）：FBX 导入 → 几何修正 → 写盘 → 读盘
        // → 语义等价，外加"坏文件必须被拒"。同样是纯 CPU、不建设备。
        // 为什么挂在 Editor 上而不是 AssetC 上：AssetC 是**工具**，
        // 工具自己的自检不该成为回归入口（它可能根本没被构建）；
        // 而这条链的正确性直接影响渲染，应该跟着应用一起被验证。
        if (std::getenv("MYVK_ASSET_TEST") != nullptr) {
            const int failures = assets::runAssetSelfTest();
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

        // 多选自检（#316）：选择集语义 / 支点 / 多目标撤销。纯 CPU，
        // 同样在 Vulkan 与窗口之前返回。见 editor/MultiSelectSelfTest.h。
        if (std::getenv("MYVK_MULTISELECT_TEST") != nullptr) {
            const int failures = editor::runMultiSelectSelfTest();
            return failures == 0 ? 0 : 1;
        }

        // E29-1 分栏槽位映射（`--selftest` 的等价物，走 env）：老位置 / 搬家 /
        // 交换 / 矩形跟随。⚠ 走 env 而不是 argv —— 本文件头 P1：`Editor.exe`
        // 丢弃命令行，`--frames` 会照常渲染到超时且零提示。
        if (std::getenv("MYVK_LAYOUT_TEST") != nullptr) {
            const int failures = editor::runLayoutSelfTest();
            return failures == 0 ? 0 : 1;
        }
        editor::EditorApp app;
        return app.run(maxFrames);    } catch (const std::exception& e) {
        VK_LOG_ERROR("Fatal: %s", e.what());
        return 1;
    }
}
