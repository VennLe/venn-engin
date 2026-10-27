// ============================================================
// dialogue/DialogueSystem.cpp
// ============================================================

#include "dialogue/DialogueSystem.h"

#include "core/Logger.h"
#include "ecs/Components.h"
#include "scene/Scene.h"

#include <limits>
#include <string>

namespace dialogue {

namespace {

// 组件的出厂语速。30 字/秒 ≈ 中文正常阅读的 1.5 倍（够快但不糊），
// 也是文字冒险里最常见的一档。
constexpr float kDefaultCps = 30.0f;

const char* kindName(bool isCue) { return isCue ? "cue" : "component"; }

} // namespace

std::string resolveDialoguePath(const std::string& id) {
    if (id.empty()) return {};
    // 带目录分隔符或显式扩展名 → 当成路径原样用。这样脚本可以直接写
    // `dialogues/sub/debug.dlg.json`，不必被迫遵守"文件名即 id"。
    if (id.find('/') != std::string::npos || id.find('\\') != std::string::npos ||
        id.rfind(".json") != std::string::npos)
        return id;
    return "dialogues/" + id + ".dlg.json";
}

DialogueSystem& activeDialogueSystem() {
    // 函数内静态：避开静态初始化顺序问题（与 `activeCameraSystem` /
    // `activeSequencerSystem` 同一写法）。
    static DialogueSystem s;
    return s;
}

// ---------------------------------------------------------------- 换行日志
//
// 只在**换行**时打一条。逐帧打会让日志在 400 fps 下每秒 400 行，
// 而"哪一句、多少字、几个选项"恰恰是要从日志里读的东西。
void DialogueSystem::noteLine(const DialoguePlayer* p, const char* src) {
    if (!p) return;
    const DlgNode* n = p->current();
    if (!n) {
        // 没有当前行 = 说完了（或者压根没 start）。只在**第一次**打。
        if (p->finished() && !m_endLogged) {
            m_endLogged = true;
            VK_LOG_INFO("DLG-END asset=%s steps=%d last=%s src=%s",
                        p->assetName().c_str(), p->steps(),
                        p->currentId().c_str(), src);
        }
        return;
    }
    if (p->currentId() == m_loggedLineId) return;
    m_loggedLineId = p->currentId();
    m_endLogged = false;
    VK_LOG_INFO("DLG-LINE id=%s speaker=%s chars=%d shown=%d choices=%d "
                "asset=%s src=%s",
                p->currentId().c_str(), n->speaker.c_str(), p->lineChars(),
                p->revealed(), static_cast<int>(n->choices.size()),
                p->assetName().c_str(), src);
}

// ---------------------------------------------------------------- cue

bool DialogueSystem::cue(scene::Scene& scene, ecs::Entity host,
                         const std::string& id) {
    const std::string path = resolveDialoguePath(id);
    if (path.empty()) {
        m_error = "empty dialogue id";
        VK_LOG_WARN("DLG-CUE-FAIL id='' (empty payload)");
        return false;
    }

    // 语速从**宿主组件**借：过场通常把 DialogueComponent 和时间轴挂在同一个
    // 实体上（导演轨），这样"这段台词快一点"只需改一个数字。
    // ⚠ 只借 `cps` 这一个字段。起点永远用资产的 `start` —— 见头文件。
    m_cueCps = kDefaultCps;
    if (host.valid()) {
        if (const auto* dc = scene.world().get<ecs::DialogueComponent>(host))
            m_cueCps = dc->cps;
    }
    m_cueHost = host;

    if (!m_cue.load(path)) {
        m_error = m_cue.error();
        VK_LOG_WARN("DLG-CUE-FAIL id=%s path=%s err=%s", id.c_str(),
                    path.c_str(), m_error.c_str());
        return false;
    }
    if (!m_cue.start()) {
        m_error = m_cue.error();
        VK_LOG_WARN("DLG-CUE-START-FAIL id=%s path=%s err=%s", id.c_str(),
                    path.c_str(), m_error.c_str());
        return false;
    }

    m_error.clear();
    m_loggedLineId.clear();   // 强制新的一段重新打首行
    m_endLogged = false;
    VK_LOG_INFO("DLG-START asset=%s path=%s node=%s cps=%.1f host=%u src=cue",
                m_cue.assetName().c_str(), path.c_str(),
                m_cue.currentId().c_str(), static_cast<double>(m_cueCps),
                static_cast<unsigned>(host.id));
    noteLine(&m_cue, kindName(true));
    return true;
}

// ---------------------------------------------------------------- 每帧

void DialogueSystem::tick(scene::Scene& scene, float dt) {
    m_lastRunning = 0;
    ecs::Entity best{};
    int bestPri = std::numeric_limits<int>::min();

    scene.world().each<ecs::DialogueComponent>(
        [&](ecs::Entity e, ecs::DialogueComponent& dc) {
            if (!dc.enabled || dc.dialoguePath.empty()) return;

            // ---- 路径被改过 → 换一份台词（并让 autoStart 重新起播）----
            // 不这么做的话，Inspector 里改完路径画面毫无变化，而日志里
            // DLG-LOAD-FAIL 都没有 —— 最难查的一类"什么都没发生"。
            if (dc.player.path() != dc.dialoguePath) {
                if (dc.player.running()) dc.player.stop();
                dc.started = false;
                dc.loadFailedLogged = false;
            }

            // 每次都调 `load`：同路径内部直接返回，路径变了才读盘。
            if (!dc.player.ready() && !dc.player.load(dc.dialoguePath)) {
                if (!dc.loadFailedLogged) {
                    dc.loadFailedLogged = true;
                    VK_LOG_WARN("DLG-LOAD-FAIL entity=%u path=%s err=%s",
                                static_cast<unsigned>(e.id),
                                dc.dialoguePath.c_str(),
                                dc.player.error().c_str());
                }
                return;
            }

            // ---- autoStart ----
            if (dc.autoStart && !dc.started && !dc.player.running() &&
                !dc.player.finished()) {
                dc.started = true;
                if (dc.player.start(dc.startNode)) {
                    VK_LOG_INFO("DLG-START asset=%s path=%s node=%s cps=%.1f "
                                "entity=%u src=component auto=1",
                                dc.player.assetName().c_str(),
                                dc.dialoguePath.c_str(),
                                dc.player.currentId().c_str(),
                                static_cast<double>(dc.cps),
                                static_cast<unsigned>(e.id));
                } else {
                    VK_LOG_WARN("DLG-START-FAIL entity=%u path=%s err=%s",
                                static_cast<unsigned>(e.id),
                                dc.dialoguePath.c_str(),
                                dc.player.error().c_str());
                }
            }

            if (!dc.player.running()) return;
            ++m_lastRunning;
            if (dc.priority > bestPri) {
                bestPri = dc.priority;
                best = e;
            }
        });

    // ---- 选谁在播：cue > 组件的最高优先级 ----
    // 过场里"该说话了"是导演命令，普通 NPC 的自动台词必须让位。
    if (m_cue.running()) {
        m_active = m_cueHost;
        m_cue.tick(dt, m_cueCps);
        noteLine(&m_cue, kindName(true));
        return;
    }

    if (!best.valid()) {
        m_active = ecs::Entity{};
        return;
    }
    m_active = best;
    if (auto* dc = scene.world().get<ecs::DialogueComponent>(best)) {
        dc->player.tick(dt, dc->cps);
        noteLine(&dc->player, kindName(false));
    }
}

// ---------------------------------------------------------------- 读写选中

const DialoguePlayer* DialogueSystem::activePlayer(scene::Scene& scene) const {
    // ⚠ 先从**事实**判断（cue 槽位在跑就是它），不看"上一帧 tick 选了谁"。
    //   依赖 tick 留下的标志位时，`cue()` 之后、下一次 tick 之前调
    //   `active()` 会得到 false —— 自检就是这么抓到的（`sys_active`）。
    if (m_cue.running()) return &m_cue;
    if (!m_active.valid()) return nullptr;
    const auto* dc = scene.world().get<ecs::DialogueComponent>(m_active);
    if (!dc || !dc->enabled || !dc->player.running()) return nullptr;
    return &dc->player;
}

DialoguePlayer* DialogueSystem::activeMut(scene::Scene& scene) {
    if (m_cue.running()) return &m_cue;
    if (!m_active.valid()) return nullptr;
    auto* dc = scene.world().get<ecs::DialogueComponent>(m_active);
    if (!dc || !dc->enabled || !dc->player.running()) return nullptr;
    return &dc->player;
}

bool DialogueSystem::active(scene::Scene& scene) const {
    return activePlayer(scene) != nullptr;
}

float DialogueSystem::activeCps(scene::Scene& scene) const {
    if (m_cue.running()) return m_cueCps;
    if (!m_active.valid()) return kDefaultCps;
    const auto* dc = scene.world().get<ecs::DialogueComponent>(m_active);
    return dc ? dc->cps : kDefaultCps;
}

// ---------------------------------------------------------------- 输入

Press DialogueSystem::pressAdvance(scene::Scene& scene) {
    DialoguePlayer* p = activeMut(scene);
    if (!p) return Press::Inactive;
    const float cps = activeCps(scene);
    // ⚠ 先记下"这一下是 cue 还是组件"，再调用：`pressAdvance` 可能当场
    //   把这段对话说完，之后 `m_cue.running()` 就变 false 了。
    const char* src = kindName(m_cue.running());
    const Press r = p->pressAdvance(cps);

    switch (r) {
        case Press::Revealed:
            // 不是"什么都没发生"：这一下把正文补完了。日志里要能看出来，
            // 否则"按了没反应"和"按了补完了"在日志上长得一样。
            VK_LOG_INFO("DLG-PRESS reveal id=%s chars=%d/%d src=%s",
                        p->currentId().c_str(), p->revealed(), p->lineChars(),
                        src);
            break;
        case Press::Moved:
        case Press::Finished:
            noteLine(p, src);
            break;
        case Press::NeedChoice:
            VK_LOG_INFO("DLG-PRESS need-choice id=%s n=%d src=%s",
                        p->currentId().c_str(), p->choiceCount(), src);
            break;
        case Press::Inactive:
            break;
    }
    return r;
}

bool DialogueSystem::choose(scene::Scene& scene, int index) {
    DialoguePlayer* p = activeMut(scene);
    if (!p) {
        m_error = "choose() with no active dialogue";
        return false;
    }
    if (!p->choose(index)) {
        m_error = p->error();
        VK_LOG_WARN("DLG-CHOOSE-FAIL i=%d id=%s err=%s", index,
                    p->currentId().c_str(), m_error.c_str());
        return false;
    }
    VK_LOG_INFO("DLG-CHOICE i=%d id=%s asset=%s", index,
                p->currentId().c_str(), p->assetName().c_str());
    noteLine(p, kindName(m_cue.running()));
    return true;
}

// ---------------------------------------------------------------- 重置

void DialogueSystem::resetRuntime(scene::Scene& scene) {
    m_cue.stop();
    m_cueHost = ecs::Entity{};
    scene.world().each<ecs::DialogueComponent>(
        [](ecs::Entity, ecs::DialogueComponent& dc) {
            dc.player.stop();
            dc.started = false;
            dc.loadFailedLogged = false;
            // 资产缓存保留：路径没变就没必要重读盘。
        });
    m_active = ecs::Entity{};
    m_lastRunning = 0;
    m_loggedLineId.clear();
    m_endLogged = false;
    m_error.clear();
}

} // namespace dialogue
