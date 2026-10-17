#pragma once
// ============================================================
// runtime/GameStateStack —— 状态栈（push / pop / replace）
//
// 语义见 GameState.h 的两条规矩。这里只补三条实现层面的约定：
//
//   1. **update / render 的范围是算出来的**，不是"从顶往下遇到阻断就停"：
//         update：找**最高的** `blocksUpdate == true`，从它到栈顶逐个调
//         render：找**最高的** `blocksRender == true`，从它到栈顶逐个调
//      两者独立 —— 这就是"Paused 停更新但继续渲染"能成立的原因。
//
//   2. **迁移不在回调里直接改栈**。宿主在自己的 tick 里调 push/pop，
//      状态类只负责记"玩家点了暂停"这类意图（M4 起接真实输入）。
//      在 onUpdate 里直接 pop 自己会让正在遍历的 vector 失效。
//
//   3. **迁移序列被记录下来**（`topSequence()`），每迁移一次记一次"栈顶名"。
//      这是自动化断言的观测面：
//          Boot>Loading>MainMenu>Playing>Paused>Playing>MainMenu
//
// `MYVK_RUNTIME_TRACE_STATES=1` 时每次迁移打一行
//   `GAME-STATE <op> name=<受影响者> depth=<n> top=<新栈顶>`
// ============================================================

#include "GameState.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace runtime {

class GameStateStack {
public:
    // ---- 迁移 ----
    void push(std::unique_ptr<GameState> state, GameServices& svc);
    void pop(GameServices& svc);
    // 换掉栈顶（深度不变）
    void replace(std::unique_ptr<GameState> state, GameServices& svc);
    // 全部退出并清空
    void clear(GameServices& svc);

    // ---- 查询 ----
    GameState* top() { return m_stack.empty() ? nullptr : m_stack.back().get(); }
    const GameState* top() const {
        return m_stack.empty() ? nullptr : m_stack.back().get();
    }
    const char* topName() const {
        const GameState* t = top();
        return t ? t->name() : "(empty)";
    }
    std::size_t depth() const { return m_stack.size(); }
    bool empty() const { return m_stack.empty(); }

    // ---- 驱动 ----
    void update(GameServices& svc, float dt);
    void render(GameServices& svc);

    // ---- 观测（自动化）----
    void setTrace(bool on) { m_trace = on; }
    bool traceOn() const { return m_trace; }
    const std::vector<std::string>& topSequence() const { return m_topSequence; }
    std::string topSequenceString() const;
    void resetSequence() { m_topSequence.clear(); }

private:
    // 记录一次迁移：按需打日志 + 往 m_topSequence 里追加新栈顶名
    void record(const char* op, const char* affected);

    // 最高的 blocksUpdate == true 的下标；没有返回 0
    std::size_t updateFloor() const;
    // 最高的 blocksRender == true 的下标；没有返回 0
    std::size_t renderFloor() const;

    std::vector<std::unique_ptr<GameState>> m_stack;
    std::vector<std::string> m_topSequence;
    bool m_trace = false;
};

} // namespace runtime
