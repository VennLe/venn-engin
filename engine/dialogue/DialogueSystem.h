#pragma once
// ============================================================
// dialogue/DialogueSystem —— 对话的"调度层"
//
// 三个职责：**懒加载资产**、**选谁在播**、**把每帧的 dt 喂给播放器**。
// 与 `SequencerSystem` 同一条纪律：自己不画东西、不读输入，输入只通过
// `pressAdvance()` / `choose()` 进来。
//
// ------------------------------------------------------------
// 两个来源（优先级：cue > 组件）
// ------------------------------------------------------------
// ① **组件驱动**（`ecs::DialogueComponent`）：作者在 Inspector 里挂一份
//    `.dlg.json`，`autoStart` 一开就进 Play 自动播。多个组件按 `priority`
//    选一个在播 —— 与时间轴同一条规则，改一个概念不用学两套。
//
// ② **cue 驱动**：时间轴的 `kind=Line` 事件，payload 就是台词 id。
//    走 `cue()`，它**优先于任何组件**：过场里"该说话了"是导演命令，
//    普通 NPC 的自动台词必须让位。
//
// ⚠⚠ `cue()` **绝不给场景加组件**。它的状态存在系统自己的槽位里。
//   给场景动态加组件看着很方便，但那份组件会被 `scene.save` 写进磁盘 ——
//   于是"播过一次过场"就永久改变了场景文件，而作者从没配过那一段。
//
// ------------------------------------------------------------
// 为什么 UI 不能直接持有播放器
// ------------------------------------------------------------
// 组件在 ECS 池里会**搬移**（扩容、排序、场景拷贝），任何跨帧存下来的
// `DialoguePlayer*` 都可能悬垂。所以 UI 每帧用 `activePlayer(scene)` 现取。
// ============================================================

#include "dialogue/DialoguePlayer.h"

#include "ecs/Entity.h"

#include <string>

namespace scene {
class Scene;
}

namespace dialogue {

// 对话 id → 资产路径。`id` 带 `/` 或 `.json` 时当路径原样用，否则解析成
// `dialogues/<id>.dlg.json`（与示例资产的文件名规则一致：文件名就是 id）。
std::string resolveDialoguePath(const std::string& id);

class DialogueSystem {
  public:
    // 每帧调用。`dt` 用**游戏时间**（含 timeScale）：对话框在暂停时该停。
    void tick(scene::Scene& scene, float dt);

    // 时间轴 / 脚本的点名起播。`host` 上若挂了 `DialogueComponent`，就用它的
    // `cps`（以及 `startNode` 之外的配置）；没挂也没关系 —— 用系统槽位播，
    // 只是 cps 取默认值。
    // ⚠ 起点**永远**用资产的 `start`，不用组件的 `startNode`：过场点名的是
    //   "这段台词"，从哪一句开始是资产自己的事。
    bool cue(scene::Scene& scene, ecs::Entity host, const std::string& id);

    // ---- 输入 ----
    Press pressAdvance(scene::Scene& scene);
    bool choose(scene::Scene& scene, int index);

    // ---- 读（UI 每帧现取，不缓存指针）----
    bool active(scene::Scene& scene) const;
    const DialoguePlayer* activePlayer(scene::Scene& scene) const;
    float activeCps(scene::Scene& scene) const;
    int lastRunning() const { return m_lastRunning; }
    const std::string& error() const { return m_error; }

    // 切场景 / 退出 Play：把播放头收掉、`started` 清零。
    // **不动组件的纯数据**（路径 / cps 是作者写的）。
    void resetRuntime(scene::Scene& scene);

    void setTrace(bool on) { m_trace = on; }

  private:
    DialoguePlayer* activeMut(scene::Scene& scene);
    // 换行 / 结束的日志去重（只在变化时打）。
    void noteLine(const DialoguePlayer* p, const char* src);

    // cue 槽位：只在这条路径上存在，不落盘。
    DialoguePlayer m_cue;
    float m_cueCps = 30.0f;
    ecs::Entity m_cueHost{};

    ecs::Entity m_active{};
    int m_lastRunning = 0;

    // 日志去重：换行才打一条 DLG-LINE，说完才打一条 DLG-END。
    std::string m_loggedLineId;
    bool m_endLogged = false;

    std::string m_error;
    bool m_trace = false;
};

// 引擎层唯一一份（与 `activeSequencerSystem()` 同一理由：一个场景只该有一个
// 播放头，否则编辑器与运行时会各推一份、互相打架）。
DialogueSystem& activeDialogueSystem();

} // namespace dialogue
