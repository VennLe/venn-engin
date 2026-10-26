#pragma once
// ============================================================
// dialogue/DialogueRunner —— 对话的"播放头"
//
// 与 C1 的时间轴播放器同一条设计纪律：**只推进状态，不画东西**。
// UI（字幕框 / 选项按钮）只读 `current()` 与 `revealedChars()`，
// 输入（空格 / 点选项）只调 `advance()` / `choose()`。
//
// 这样打字机、自动播放、快进、回看全部是**调用方的策略**，
// 而不是写死在 runner 里的分支。
//
// ------------------------------------------------------------
// 三条硬语义
// ------------------------------------------------------------
// ⚠⚠ ① **有选项时 `advance()` 不动**，返回 `NeedChoice`。把"继续"和
//     "等选择"混成一个 bool，就会出现"玩家还没看清选项，它自己跳过了"。
// ⚠⚠ ② **`next` 为空 = 结束**，`advance()` 当场把 `active()` 变 false。
//     结束之后 `current()` 返回 nullptr —— 调用方必须先查。
// ⚠⚠ ③ **不做自动前进计时**。"这句台词显示满 2 秒自动下一句"是**表现策略**：
//     放在 runner 里，测起来要等真实时间，而且和打字机速度耦合。
// ============================================================

#include "dialogue/DialogueAsset.h"

namespace dialogue {

enum class Advance {
    Moved,        // 走到下一句（或跳到了 goto 目标）
    NeedChoice,   // 当前这行有选项，等 `choose()`
    Finished,     // 说完了
    Inactive,     // 本来就没在跑（没 start 过，或者已经结束）
};

class DialogueRunner {
  public:
    // `startOverride` 非空时覆盖资产的 `start` 字段（从任意一句开始调试）。
    bool start(const DialogueAsset& asset, const std::string& startOverride = {});
    void reset();

    bool active() const { return m_active; }
    bool finished() const { return !m_active && m_steps > 0; }

    // ⚠ 可能返回 nullptr（没 start / 已结束），调用方必须先查。
    const DlgNode* current() const;
    bool atChoice() const;          // 当前这行有选项
    int choiceCount() const;

    Advance advance();
    bool choose(int index);

    // 走过的节点数（含重复进入）—— 死循环排查用。
    int steps() const { return m_steps; }
    // 最近一次错误（选择越界 / 没 start 就 choose 之类）。
    const std::string& error() const { return m_error; }

    const DialogueAsset* asset() const { return m_asset; }
    const std::string& currentId() const { return m_currentId; }

  private:
    const DialogueAsset* m_asset = nullptr;
    std::string m_currentId;
    bool m_active = false;
    int m_steps = 0;
    std::string m_error;

    // 防"数据合法但永远出不来"的死循环：`goto` 指回自己不会报错
    // （剧情里"再问一次"是合法写法），但一个纯粹的自环会让自动推进
    // 卡死。超过这个步数就报错停下 —— 上限取得足够大，正常剧情碰不到。
    static constexpr int kMaxSteps = 100000;
};

// 无窗口自检（`Game.exe --selftest dialogue`）：返回**失败条数**（0 = 全过）。
// 实现在 `DialogueSelfTest.cpp`；声明放这里是为了运行时不必再认识一个新头文件。
int runDialogueSelfTest();

} // namespace dialogue
