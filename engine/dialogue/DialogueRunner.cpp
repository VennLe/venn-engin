// ============================================================
// dialogue/DialogueRunner.cpp
// ============================================================

#include "dialogue/DialogueRunner.h"

#include "core/Logger.h"

namespace dialogue {

bool DialogueRunner::start(const DialogueAsset& asset,
                           const std::string& startOverride) {
    reset();
    if (!asset.valid) {
        m_error = "asset is not valid";
        return false;
    }
    const std::string& id = startOverride.empty() ? asset.startId : startOverride;
    if (findNode(asset, id) < 0) {
        m_error = "start line \"" + id + "\" not found";
        return false;
    }
    m_asset = &asset;
    m_currentId = id;
    m_active = true;
    ++m_steps;
    return true;
}

void DialogueRunner::reset() {
    m_asset = nullptr;
    m_currentId.clear();
    m_active = false;
    m_steps = 0;
    m_error.clear();
}

const DlgNode* DialogueRunner::current() const {
    if (!m_asset || !m_active) return nullptr;
    const int i = findNode(*m_asset, m_currentId);
    if (i < 0) return nullptr;
    return &m_asset->nodes[static_cast<size_t>(i)];
}

bool DialogueRunner::atChoice() const {
    const DlgNode* n = current();
    return n && !n->choices.empty();
}

int DialogueRunner::choiceCount() const {
    const DlgNode* n = current();
    return n ? static_cast<int>(n->choices.size()) : 0;
}

Advance DialogueRunner::advance() {
    if (!m_active || !m_asset) return Advance::Inactive;
    const DlgNode* n = current();
    if (!n) {
        m_active = false;
        return Advance::Finished;
    }

    // ⚠ ①：有选项就**一步都不走**。这里若顺手跳到 choices[0]，
    //   玩家看到的是"选项闪现一下然后自动选了第一个"。
    if (!n->choices.empty()) return Advance::NeedChoice;

    // ⚠ ②：`next` 为空 = 说完了。`active` 当场变 false，`current()` 之后
    //   返回 nullptr —— 调用方（字幕框）必须按这个顺序收尾。
    if (n->nextId.empty()) {
        m_active = false;
        VK_LOG_INFO("DLG-END steps=%d last=%s", m_steps, m_currentId.c_str());
        return Advance::Finished;
    }

    if (m_steps >= kMaxSteps) {
        m_error = "too many steps (runaway goto loop?)";
        m_active = false;
        return Advance::Finished;
    }
    m_currentId = n->nextId;
    ++m_steps;
    return Advance::Moved;
}

bool DialogueRunner::choose(int index) {
    if (!m_active || !m_asset) {
        m_error = "choose() with no active dialogue";
        return false;
    }
    const DlgNode* n = current();
    if (!n || n->choices.empty()) {
        m_error = "choose() on a line without choices";
        return false;
    }
    if (index < 0 || index >= static_cast<int>(n->choices.size())) {
        // ⚠ 越界**不改状态**：返回 false 让调用方决定（点空白 / 键盘越界
        //   都不该把对话推进到乱七八糟的地方）。
        m_error = "choice index out of range";
        return false;
    }
    if (m_steps >= kMaxSteps) {
        m_error = "too many steps (runaway goto loop?)";
        m_active = false;
        return false;
    }
    const std::string to = n->choices[static_cast<size_t>(index)].gotoId;
    if (findNode(*m_asset, to) < 0) {
        // 加载时已经查过，走到这里说明资产被改过而索引没重建。
        m_error = "choice goto \"" + to + "\" vanished";
        m_active = false;
        return false;
    }
    m_currentId = to;
    ++m_steps;
    return true;
}

} // namespace dialogue
