// ============================================================
// dialogue/DialogueRunner.cpp
// ============================================================

#include "dialogue/DialogueRunner.h"

#include "core/Logger.h"

namespace dialogue {

bool DialogueRunner::start(const DialogueAsset& asset,
                           const std::string& startOverride) {
    // ⚠ `reset()` 会清 `m_store`？**不会** —— store 是宿主注入的能力，
    //   不是这一段的运行态（清掉的话"每段对话开始"都要重新注入一次，
    //   而漏注入一处就静默走 else）。
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
    m_active = true;
    if (!enterNode(id)) {
        m_active = false;
        return false;
    }
    return true;
}

// C7：条件跳转的唯一实现点。
//
// 语义（三条都会有一条用例钉住）：
//   ① 条件成立 → 停在这句；
//   ② 不成立 → 跳到 `elseId`；`elseId` 空 = **结束**（`m_active = false`）；
//   ③ 链式跳转（A 的 else 指向 B，B 也有条件）继续解析，`depth` 只是
//      防御（理论上有 kMaxSteps 兜底，这里再挡一层递归深度）。
bool DialogueRunner::enterNode(const std::string& id) {
    if (!m_asset) {
        m_error = "enterNode() with no asset";
        return false;
    }
    std::string cur = id;
    // 链式跳转的**跳数**上限（与 `kMaxSteps` 不是一回事：那个是整段对话的
    // 步数，这个是"一次进入里连续跳了几次"）。两个节点互相 else 是**合法
    // 数据**（`parseDialogue` 查不出环），但运行期会永远跳。
    int hops = 0;
    for (;;) {
        if (++hops > 64) {
            m_error = "condition chain too deep (else loops back?)";
            m_active = false;
            return false;
        }
        const int i = findNode(*m_asset, cur);
        if (i < 0) {
            m_error = "line \"" + cur + "\" does not exist";
            return false;
        }
        const DlgNode& n = m_asset->nodes[static_cast<size_t>(i)];
        ++m_steps;
        if (m_steps >= kMaxSteps) {
            m_error = "too many steps (runaway goto loop?)";
            m_active = false;
            return false;
        }
        if (n.conds.empty()) {
            m_currentId = cur;
            // ⚠ 落在有条件但**全部成立**的那句时也要打一行：验收要靠它
            //   区分"本来没条件"和"条件成立"（两者走线一样，日志不一样）。
            return true;
        }
        if (!m_store) {
            // 见 .h：不装 store 而剧本用了条件 = 报错并停住。
            m_error =
                "conditions need a flag store (host did not install one)";
            m_currentId = cur;
            return false;
        }
        const bool ok = evalConds(n.conds, m_store);
        VK_LOG_INFO("DLG-COND id=%s ok=%d flags=%d else=%s", cur.c_str(),
                    ok ? 1 : 0, static_cast<int>(n.conds.size()),
                    n.elseId.c_str());
        if (ok) {
            m_currentId = cur;
            return true;
        }
        if (n.elseId.empty()) {
            // ② 没写 else = 这段对话到此结束。
            m_currentId = cur;
            m_active = false;
            return true;
        }
        cur = n.elseId;
    }
}

void DialogueRunner::reset() {
    m_asset = nullptr;
    m_currentId.clear();
    m_active = false;
    m_steps = 0;
    m_error.clear();
    // `m_store` 不清（见 `start()` 的注释）。
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
    // C7：走 `enterNode` 而不是直接赋值 —— 下一句可能带条件。
    // ⚠ 它失败（没 store / else 目标坏了）时**保持当前这句**，返回
    //   `Moved` 也不对（玩家会看到"字幕没变"两次）。所以这里按
    //   `Finished` 收尾：条件链断了就是这段演不下去了，错误留在 `error()`。
    if (!enterNode(n->nextId)) {
        m_active = false;
        return Advance::Finished;
    }
    // 走到这里 `m_active` 可能已被 else-空 置 false（"条件不满足就此结束"）。
    return m_active ? Advance::Moved : Advance::Finished;
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
    const DlgChoice& ch = n->choices[static_cast<size_t>(index)];
    const std::string to = ch.gotoId;
    if (findNode(*m_asset, to) < 0) {
        // 加载时已经查过，走到这里说明资产被改过而索引没重建。
        m_error = "choice goto \"" + to + "\" vanished";
        m_active = false;
        return false;
    }
    // ---- C7：先写旗标，再跳 ----
    //
    // ⚠ 顺序不能反：`goto` 的目标可能**立刻**要读这些旗标（"选了'我救过你'
    //   之后那句带条件的话"就是同一个选项的后果）。先跳再写的话，那一句
    //   永远走 else，而作者只会觉得"条件写错了"。
    //
    // ⚠ 没装 store 时**不写、但也不拦**：选项照样生效（这是老资产的
    //   兼容路径），只在用了 `set` 的情况下留一条 WARN。
    //   拦下来会让"一份带 set 的剧本在没有 store 的宿主里连选项都点不动"，
    //   比"没记住"更难排查。
    if (!ch.writes.empty()) {
        if (!m_store) {
            VK_LOG_WARN("DLG-WRITE-SKIP id=%s choice=%d writes=%d reason=no-store",
                        n->id.c_str(), index,
                        static_cast<int>(ch.writes.size()));
        } else {
            for (const auto& w : ch.writes) {
                m_store->set(w.flag, w.value);
                VK_LOG_INFO("DLG-WRITE id=%s choice=%d flag=%s value=%g",
                            n->id.c_str(), index, w.flag.c_str(), w.value);
            }
        }
    }
    if (!enterNode(to)) {
        m_active = false;
        return false;
    }
    return true;
}

} // namespace dialogue
