#pragma once
// ============================================================
// core/RunMode.h —— "这一次是自动化在跑，还是人在用"
//
// 为什么要一个共用判据：
//   有几处默认值是**为了手感**定的，而手感和自动化的诉求正好相反 ——
//     · 窗口出现前缓冲两秒（Application 的 startupShowDelay）
//     · 呈现锁垂直同步（RHI/Swapchain 的呈现模式）
//   两边各写一份环境变量列表的话，新加一个脚本变量就要记得改两处，
//   漏掉的那一处会表现为"这个脚本跑得特别慢 / 特别快"，极难定位。
//
// 判据：**只要设了其中任何一个 MYVK_ 变量就算自动化**。理由是所有
//   verify / 截图脚本至少会设一个（`MYVK_LOG_RECTS` 82 个、
//   `MYVK_NO_WINDOW_SAVE` 100 个……），而普通用户不会去设这些东西。
//   `MYVK_VSYNC` 自己除外 —— 它是**显式指定**，优先级最高。
//
// ⚠ 只认"开着"的变量：`MYVK_X=0` 也算设了（脚本里 `=0` 是常见写法，
//   语义上仍然是"这是自动化"）。
// ============================================================

#include <cstdlib>

namespace core {

// 自动化运行？（见文件头）
inline bool automationRun() {
    // 列一份"已知会被脚本设置"的变量：既快又不会误判（普通用户不会设）。
    // 用显式列表而不是扫描整个环境，是为了让"什么算自动化"这件事可读、
    // 可审计 —— 判据散落成隐式规则之后，没人敢改。
    static const char* const kKeys[] = {
        "MYVK_FRAMES",          // 跑满 N 帧就退出
        "MYVK_NO_WINDOW_SAVE",  // 不写窗口几何 / imgui.ini
        "MYVK_LOG_RECTS",       // 打印 UI 锚点
        "MYVK_HIDE_UI",         // 截图对比时藏 UI
        "MYVK_EDITOR_NO_HUD",
        "MYVK_EDITOR_SCENE",    // 指定启动场景
        "MYVK_RUNTIME_SCENE",
        "MYVK_ASSET_DIR",
        "MYVK_ANIM_TEST",
        "MYVK_VIEW_MODE",
        "MYVK_ENGINE_PANEL",
        "MYVK_EDITMODE",
        "MYVK_EDITOR_CAM",
    };
    for (const char* k : kKeys) {
        if (const char* v = std::getenv(k)) {
            (void)v;
            return true;
        }
    }
    return false;
}

// 呈现模式：true = FIFO（锁垂直同步），false = MAILBOX（跑满不锁）。
//
// 为什么默认**锁**：不锁的时候主循环能跑到 400~500 fps，而显示器只有
// 60/144 Hz。渲染帧与显示刷新不是整数倍关系 → 合成器每次取的"最新一帧"
// 距上一次刷新的时间不等，画面里的匀速运动（尤其是转视角时的整个视野）
// 会呈现不规则推进 = 肉眼看到的"一顿一顿"。锁上之后每帧严格一次刷新，
// 步长恒定。
//
// 显式开关：MYVK_VSYNC=0 强制不锁（量 GPU 峰值 / 延迟用），=1 强制锁。
inline bool vsyncEnabled() {
    if (const char* s = std::getenv("MYVK_VSYNC")) return s[0] != '0';
    return !automationRun();
}

} // namespace core
