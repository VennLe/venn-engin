#pragma once
// ============================================================
// dialogue/DialoguePlayer —— "一份正在播的对话"的全部状态
//
// 它是 `DialogueRunner`（纯状态机）外面的一层**表现状态**：
//   · 打字机进度（elapsed / 已出字数）
//   · 两段式推进（第一次按键补完打字机，第二次才真的走下一句）
//   · 资产所有权
//
// 拆成两层而不是塞进 runner，是因为 runner 的 49 条自检已经把语义钉死了
// （有选项时 `advance()` 一步都不走、`next` 空 = 结束……）。打字机速度与
// "按一下先补完"都是**表现策略**，换个游戏可能就不要了 —— 混进 runner 会让
// 那些断言变成"必须等真实时间"的测试。
//
// ------------------------------------------------------------
// ⚠⚠ 资产为什么用 `shared_ptr` 持有
// ------------------------------------------------------------
// `DialogueRunner` 里存的是 `const DialogueAsset*`。如果本类按值持有资产，
// 而本类又被放进 ECS 组件（`DialogueComponent`）—— 组件池扩容时会**搬移**
// 元素，`m_asset` 的地址跟着变，runner 里那个裸指针立刻悬垂。
// 症状极具欺骗性：`current()` 读到的是一段已释放的内存，绝大多数时候
// "看起来还能用"（vector 容量没被复用），偶尔在第 N 句崩掉。
//
// 所以资产放堆上，由 `shared_ptr` 保证地址稳定；本类被搬移/拷贝多少次都
// 没关系。**这不是过度设计，是 ECS + 裸指针的必然结果。**
//
// ------------------------------------------------------------
// 两段式推进（`Press::Revealed` → `Press::Moved`）
// ------------------------------------------------------------
// 第一次按键把正文**全部显示出来**，第二次才走下一句。这是文字冒险的通行
// 手感（玩家点快一点就变成"整段阅读"），也让"按键"与"翻页"不再是一件事。
// ⚠ 它必须发生在**玩家按键**上，不是定时器上：定时器会让"点得快的玩家
//   错过整句"。
// ============================================================

#include "dialogue/DialogueAsset.h"
#include "dialogue/DialogueRunner.h"

#include <memory>
#include <string>

namespace dialogue {

// 一次"推进"请求的结果。⚠ `Revealed` 不是"什么都没发生"——它改了显示状态。
enum class Press {
    Inactive,    // 没有在播的对话
    Revealed,    // 打字机没出完 → 这一下把正文全部显示出来（**不翻页**）
    Moved,       // 真的走到了下一句
    NeedChoice,  // 当前这行有选项，等 `choose()`
    Finished,    // 说完最后一句
};

class DialoguePlayer {
  public:
    // ---- 资产 ----
    // 读盘 + 解析（走 `loadDialogue`）。同一路径重复调用不重读。
    // 失败时 `ready()==false`、`error()` 给原因；**不抛**、不清空已有状态。
    bool load(const std::string& path);
    bool ready() const { return m_asset && m_asset->valid; }
    const DialogueAsset* asset() const { return m_asset.get(); }
    const std::string& path() const { return m_path; }
    const std::string& error() const { return m_error; }
    // 资产名（日志用）。没加载时是空串。
    const std::string& assetName() const;

    // ---- 播放 ----
    // `startNode` 非空时覆盖资产的 `start`（从任意一句开始调试）。
    bool start(const std::string& startNode = {});
    // 收起当前播放（资产缓存保留）。
    void stop();

    bool running() const { return m_runner.active(); }
    // 说完最后一句之后为 true（`start()` 会清掉）。
    // ⚠ 与 `running()` 是两个问题："正在播" vs "播完了"。
    bool finished() const { return m_finished; }

    void tick(float dt, float cps);

    // 玩家按了一下推进键。`cps` 用来判断"打字机出完了没有"。
    Press pressAdvance(float cps);
    bool choose(int index);

    // ---- 读（UI 只读这些）----
    const DlgNode* current() const { return m_runner.current(); }
    const std::string& currentId() const { return m_runner.currentId(); }
    int lineChars() const;      // 当前这行的**字符数**（UTF-8 码点，不是字节）
    int revealed() const { return m_revealed; }
    bool fullyRevealed() const { return m_revealed >= lineChars(); }
    // 已经显示出来的那一段（按码点切，不会劈开汉字）。
    std::string shownText() const;
    bool atChoice() const { return m_runner.atChoice(); }
    int choiceCount() const { return m_runner.choiceCount(); }
    int steps() const { return m_runner.steps(); }

  private:
    void resetTypewriter();

    std::string m_path;
    std::shared_ptr<const DialogueAsset> m_asset;
    DialogueRunner m_runner;
    float m_elapsed = 0.0f;
    // 最近一次喂进来的打字速度。`resetTypewriter()` 要用它决定"复位到 0 个字
    // 还是全出" —— `cps <= 0` 的约定是**立刻全出**（见 DialogueAsset.h ④），
    // 那么换行之后的第一帧就应该是满的，不能先空一帧。
    float m_cps = 30.0f;
    int m_revealed = 0;
    bool m_finished = false;
    std::string m_error;
};

} // namespace dialogue
