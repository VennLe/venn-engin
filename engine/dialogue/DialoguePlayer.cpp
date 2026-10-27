// ============================================================
// dialogue/DialoguePlayer.cpp
//
// 状态只有三样：`m_elapsed`（打字机计时）、`m_revealed`（已出字数）、
// `m_finished`（说完最后一句）。其余全在 `DialogueRunner` / 资产里。
//
// ⚠ `m_revealed` 是**缓存**而不是每帧算：`revealedChars(elapsed, cps, total)`
//   在 tick 与 pressAdvance 两处都要用，而两处的 cps 可能不同（组件字段被
//   Inspector 改了）。缓存下来，"这一帧显示到第几个字"就只有一个答案。
// ============================================================

#include "dialogue/DialoguePlayer.h"

#include "core/Logger.h"

#include <algorithm>

namespace dialogue {

namespace {

// 打字机计时上限。`elapsed` 只要够长到"全部出完"就够了，继续累加没有意义，
// 而且 float 累加到很大之后 `floor(elapsed*cps)` 会开始掉精度（每帧步进
// 小于 1 个 ULP，字数忽然不涨了）。夹到一个"肯定出得完"的值。
constexpr float kMaxElapsed = 600.0f;

} // namespace

const std::string& DialoguePlayer::assetName() const {
    static const std::string kEmpty;
    return m_asset ? m_asset->name : kEmpty;
}

bool DialoguePlayer::load(const std::string& path) {
    if (path.empty()) {
        m_error = "empty dialogue path";
        return false;
    }
    // 同路径不重读：Inspector 每帧都会把路径喂进来，每次 fopen 受不了。
    if (m_asset && m_path == path) return m_asset->valid;

    m_path = path;
    auto a = std::make_shared<DialogueAsset>();
    // ⚠ 失败也**保留**这个资产对象：`error()` / `name()` 要能报出原因，
    //   否则 Inspector 上只会看到一个"路径填了但什么都没发生"。
    if (!loadDialogue(path, *a) || !a->valid) {
        m_asset = std::move(a);
        m_error = m_asset->error.empty() ? "invalid dialogue asset"
                                         : m_asset->error;
        return false;
    }
    m_asset = std::move(a);
    m_error.clear();
    return true;
}

bool DialoguePlayer::start(const std::string& startNode) {
    if (!m_asset) {
        m_error = "no asset";
        return false;
    }
    // ⚠ `DialogueRunner::start` 内部先 `reset()`：它会清掉上一次的 `m_steps`。
    //   "重播"因此是干净的 —— 但这也意味着 **`start()` 之后 runner 的指针是
    //   新的**，所以资产必须在调用它之前就已经定好（上面 load 的职责）。
    if (!m_runner.start(*m_asset, startNode)) {
        m_error = m_runner.error();
        m_finished = false;
        return false;
    }
    m_error.clear();
    m_finished = false;
    resetTypewriter();
    return true;
}

void DialoguePlayer::stop() {
    m_runner.reset();
    m_finished = false;
    resetTypewriter();
}

void DialoguePlayer::resetTypewriter() {
    m_elapsed = 0.0f;
    // ⚠ 不能直接写 `m_revealed = 0`：`cps <= 0` 的约定是**立刻全出**，
    //   复位到 0 会让"关掉打字机"的那一档先空一帧。400 fps 下看不出来，
    //   但截图与"首帧字数"类断言会抓个正着。
    m_revealed = revealedChars(0.0f, m_cps, lineChars());
}

void DialoguePlayer::tick(float dt, float cps) {
    if (!m_runner.active()) return;
    const DlgNode* n = m_runner.current();
    if (!n) return;

    m_cps = cps;
    if (dt > 0.0f) m_elapsed = std::min(m_elapsed + dt, kMaxElapsed);
    const int total = utf8Count(n->text);
    m_revealed = revealedChars(m_elapsed, cps, total);
}

Press DialoguePlayer::pressAdvance(float cps) {
    if (!m_runner.active()) return Press::Inactive;
    const DlgNode* n = m_runner.current();
    if (!n) return Press::Inactive;

    m_cps = cps;

    // ---- ① 打字机没出完 → 只补完显示，**不翻页** ----
    const int total = utf8Count(n->text);
    if (m_revealed < total) {
        // 直接把 elapsed 推到"刚好全部出完"的时刻；比逐个字符加更稳，
        // 而且 cps<=0 时 `revealedChars` 本来就返回 total。
        m_revealed = total;
        if (cps > 0.0f) m_elapsed = std::max(m_elapsed, total / cps + 0.001f);
        return Press::Revealed;
    }

    // ---- ② 翻页 ----
    const Advance a = m_runner.advance();
    switch (a) {
        case Advance::Moved:
            resetTypewriter();
            return Press::Moved;
        case Advance::NeedChoice:
            return Press::NeedChoice;
        case Advance::Finished:
            m_finished = true;
            return Press::Finished;
        case Advance::Inactive:
            break;
    }
    return Press::Inactive;
}

bool DialoguePlayer::choose(int index) {
    if (!m_runner.choose(index)) {
        // ⚠ 越界**不改状态**（runner 那边已经保证），这里只记错误。
        m_error = m_runner.error();
        return false;
    }
    resetTypewriter();
    return true;
}

int DialoguePlayer::lineChars() const {
    const DlgNode* n = m_runner.current();
    return n ? utf8Count(n->text) : 0;
}

std::string DialoguePlayer::shownText() const {
    const DlgNode* n = m_runner.current();
    if (!n) return {};
    if (m_revealed <= 0) return {};
    return utf8Prefix(n->text, m_revealed);
}

} // namespace dialogue
