#pragma once
// ============================================================
// editor/SaveSlotsPanel —— 存档槽位面板（G17-3 的尾巴，2026-10-31）
//
// 为什么要有这一块：`listSaveSlots()` 早就写好了，但**没有任何人消费它** ——
// 于是"存了哪些档、每个档是什么时候的、属于哪个场景"只有翻文件系统才知道。
// 运行时的 F5/F9 只认 `--slot` 指定的那一个槽位，玩家没法选。
//
// 定位：**调试 + 验证用**，不是最终的游戏内存档界面（那个应该是 `.ui.json`
// 摆出来的，见 `docs/ui2`）。所以这里：
//   · 五个白名单槽位全部列出来（空槽也列 —— "哪些还能用"本身是信息）；
//   · 每个槽位显示 时间 / 场景 / 实体数 / 时长，以及**存在但读不动**的标记；
//   · 三个动作：存到该槽 / 从该槽读 / 删除该槽。
//
// ------------------------------------------------------------
// 三条纪律（都是踩过的）
// ------------------------------------------------------------
// ① **"有这个文件但读不动"必须显示成"坏档"，不能显示成"空槽"**。
//    显示成空槽的后果是：玩家以为档没了，然后存一次把好档覆盖掉。
//    `SaveSlotInfo::exists` 与 `timestamp.empty()` 两件事要分开看。
// ② **删除要二次确认**。这是本面板唯一不可撤销的动作（存/读都能重来）。
// ③ 面板与运行时看的是**同一批文件**：两边 CWD 都是 `build/bin`，
//    `root` 都是相对路径 `saves/`。这条不是巧合，是刻意的 —— 在编辑器里
//    存的档，Game.exe 按 F9 就该读得到（判据 `verify_save_panel.py` 的 C 组）。
// ============================================================

#include "EditorContext.h"

#include "scene/SaveGameState.h"

#include <string>

namespace editor {

class SaveSlotsPanel {
public:
    explicit SaveSlotsPanel(EditorContext& ctx) : m_ctx(ctx) {}

    void draw();

private:
    EditorContext& m_ctx;

    // 存档根目录。空 = `scene::kSaveDirDefault`（相对 CWD 的 `saves/`）。
    // ⚠ 不做成可编辑输入框：改了它之后"编辑器存的档运行时读不到"这件事
    //   没有任何提示，而症状是"我明明存了"。要改路径请用运行时的
    //   `--save-dir` / `MYVK_RUNTIME_SAVE_DIR`。
    std::string m_root;

    // 本面板自己的基线。编辑器没有"关卡加载"这个时刻，所以第一次存档前
    // 现抓一次（`captureBaseline`）—— 结果等于**全量存档**，合法且正确，
    // 只是比增量大。"编辑器里存的档永远是全量"是可以接受的取舍。
    scene::SaveGameState m_state;
    // ⚠ 没有 `m_baselineReady` 了：编辑器里的"存"是**全量快照**，基线
    //   永远为空（见 .cpp 里 doSave 的注释）。留着一个 `false` 的布尔量
    //   只会让人以为"抓过一次基线"，于是又写回那个空档 bug。

    int m_selected = 4;        // 默认选 `quick`（白名单最后一个）
    int m_confirmDelete = -1;  // >=0 = 正在确认删除的那个下标
    std::string m_status;      // 底部最后一条结果（成功 / 失败都写）
    bool m_statusOk = false;

    void doSave(const std::string& slot);
    void doLoad(const std::string& slot);
    void doDelete(const std::string& slot);
};

} // namespace editor
