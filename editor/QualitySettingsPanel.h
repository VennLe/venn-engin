#pragma once
// ============================================================
// editor/QualitySettingsPanel —— E26 · 画质档位面板
//
// 为什么需要它（用户实际会撞上的问题）：
//   E09 的 Engine 面板把 32 个特性摊成一张长表，每个特性一个开关 ——
//   想"把画面调到能跑"的人要逐条读一遍才知道该关谁。而启动器
//   （Launcher.exe）新建工程时已经能选 Low~Cinematic 了，进编辑器之后
//   **反而没有对应入口**：只能回到那张长表里手工对。
//
//   这块面板补的就是这一步：选档 → 看差异 → 应用 → 落盘。
//
// ------------------------------------------------------------
// 三个刻意的设计
// ------------------------------------------------------------
// ① **选择与应用分开**。点档位只改"预览目标"（`m_selected`），不立刻
//    改参数 —— 否则"差异预览"永远是空的（选中即应用，差异恒为 0），
//    用户也就永远看不到"切到 Low 会关掉什么"。
//
// ② **差异与一致性检查共用 `render::presetDiff`**。两处各写一套比较逻辑
//    必然漂移（"兜底条件必须与放行条件同源"），而这个项目的教训是：
//    漂移之后两边都"通过"，只有画面不对。
//
// ③ **落盘只写值，不写档位名作为权威**。`config/render.json` 里的
//    `"quality": "High"` 只是标签；标签与实际值不一致时显示 Custom，
//    **不**反向纠正参数。与启动器 M4 解掉 `game.json.quality` 空挂时
//    的判断一致（详见 `docs/launcher_roadmap.md` §11.4）。
//
// ------------------------------------------------------------
// 自动化锚点（`tools/verify_quality_presets.py` 用）
// ------------------------------------------------------------
//   QUAL-RECT              面板内容区矩形
//   QUAL-PRESET-%d         档位单选按钮
//   QUAL-PLATFORM          平台下拉
//   QUAL-APPLY / QUAL-CHECK / QUAL-SAVE
//   QUAL-DIFF-%d           差异行
//   QUAL-STATUS            状态行（"离该档差 N 项" / 落盘结果）
// 日志锚点：
//   QUALITY-UI select / apply / check / save / save-refused
//   QUALITY-SNAPSHOT ssao=.. ssr=.. taa=.. cascades=.. dist=.. lights=..
// ============================================================

namespace editor {

class QualitySettingsPanel {
public:
    // 每帧绘制（由 `EditorApp::panelDrawQuality` 派发）。
    void draw();

private:
    // 应用当前选中档（真改参数），随后立刻自检一次。
    void applySelected();
    // 自检：当前状态离 `m_selected` 还差几项。返回不匹配条数。
    int checkSelected(bool logResult);
    // 落盘到工程 `config/render.json`。
    void saveSelected();
    // 从**当前参数值**反推"现在是哪一档"：对每档跑一次 presetDiff，
    // 差 0 项的就是它；都对不上返回 -1（Custom）。
    //
    // ⚠ 不读 `config/render.json` 里的档位名做判断：那个名字只是标签，
    //   标签和实际值不一致时以**值**为准（否则用户手改一个参数，面板还
    //   理直气壮地显示"High"，而它已经不是 High 了）。
    int detectCurrentPreset();

    int m_selected = 2;    // 预览目标（默认 High —— 引擎默认值都在这一档）
    int m_platform = 0;    // 平台下拉的当前项
    int m_lastMismatch = 0;
    char m_status[192] = {0};
    bool m_savedOnce = false;
    bool m_initialised = false;   // 首帧反推一次当前档位

};

} // namespace editor
